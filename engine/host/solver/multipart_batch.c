/* The sub-requests a request body names — see multipart_batch.h. Stateless: every endpoint it reads goes
   straight to solver/endpoint.c, so there is no table to initialise and none to free. */
#include "solver/multipart_batch.h"
#include "solver/endpoint.h"
#include "core/mime/mime_type.h"     /* Fetch §2.2.2 "Headers"'s extract a MIME type — the request's own STATEMENT */
#include "core/fetch/headers.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>

/* ── the two spans this file walks ───────────────────────────────────────────────────────────────────────
 *
 * OVER LENGTHS AND NEVER A NUL, for the reason solver/reply_decode.c states about a decoded stream and which
 * is sharper here: this is a REQUEST body, so a `Uint8Array` a page assembled by hand may hold a 0x00 in the
 * middle of a perfectly well-formed multipart envelope, and a `strchr` walk would have read every part after
 * it as absent. */

static const char *mem_find(const char *h, size_t hn, const char *n, size_t nn)
{
    size_t i;
    if (nn == 0 || hn < nn) return NULL;
    for (i = 0; i + nn <= hn; i++)
        if (h[i] == n[0] && !memcmp(h + i, n, nn)) return h + i;
    return NULL;
}

/* RFC 2046 §5.1.1: the delimiter is CRLF followed by the dash-boundary, so a dash-boundary that is not at the
   start of a line is ORDINARY CONTENT — a part may quote its own boundary string mid-line and the framing is
   unaffected. `from` must itself be a line start, which is the invariant the caller keeps and asserts. */
static const char *line_start_find(const char *from, const char *end, const char *needle, size_t nn)
{
    const char *q = from;

    DCHECK(nn > 0, "the multipart walk searched for an empty delimiter — the dash-boundary is built from a "
                   "boundary this file already refused to accept as empty, so a zero length here means the "
                   "build and the search disagree about what was constructed");
    while ((size_t)(end - q) >= nn) {
        const char *hit = mem_find(q, (size_t)(end - q), needle, nn);
        if (!hit) return NULL;
        if (hit == from || hit[-1] == '\n') return hit;
        q = hit + 1;
    }
    return NULL;
}

/* PAST THE REST OF THE DELIMITER LINE. RFC 2046 allows transport-padding (LWSP) between the dash-boundary and
   the CRLF that ends the line, so the next part begins after that CRLF and not after the boundary itself.
   NULL is a delimiter line the body never terminated — the last thing in the buffer, with no part after it. */
static const char *after_line(const char *q, const char *end)
{
    const char *nl = memchr(q, '\n', (size_t)(end - q));
    return nl ? nl + 1 : NULL;
}

/* ── one body part ───────────────────────────────────────────────────────────────────────────────────────── */

/* THE HEADERS THE SUB-REQUEST REQUIRES, collected before anything is recorded. `EndpointHeader` borrows its
   two strings for the length of the `endpoint_record` call (endpoint.h), so this owns them until then. */
typedef struct { EndpointHeader *e; int n, cap; } EhBuf;

static void eh_add(EhBuf *b, const char *name, size_t nn, const char *val, size_t vn)
{
    char *n2, *v2;

    if (b->n >= b->cap) {
        b->cap = b->cap ? b->cap * 2 : 4;
        b->e = realloc(b->e, (size_t)b->cap * sizeof(*b->e));
        CHECK(b->e, "multipart_batch: OOM collecting a sub-request's headers");
    }
    n2 = malloc(nn + 1); CHECK(n2, "multipart_batch: OOM copying a sub-request header name");
    memcpy(n2, name, nn); n2[nn] = 0;
    v2 = malloc(vn + 1); CHECK(v2, "multipart_batch: OOM copying a sub-request header value");
    memcpy(v2, val, vn); v2[vn] = 0;
    b->e[b->n].name = n2;
    b->e[b->n].value = v2;
    b->n++;
}

static void eh_free(EhBuf *b)
{
    int i;
    for (i = 0; i < b->n; i++) {
        /* Cast away the borrow: `EndpointHeader` names what the surface READS, and these two are this file's
           own allocations for the length of the call. */
        free((char *)b->e[i].name);
        free((char *)b->e[i].value);
    }
    free(b->e);
    b->e = NULL; b->n = 0; b->cap = 0;
}

/* RFC 9110 §9's METHOD is a token, and RFC 9110 §5.1's field-name is the SAME token production, so the engine's
   one implementation of it answers both. A second tchar table beside `header_name_valid` would be the
   redundant duplicate CLAUDE.md forbids in the codec case for the same reason. */
static bool is_token(const char *s, size_t n) { return header_name_valid(s, n); }

/* RFC 9112 §2.3's HTTP-version, exactly: "HTTP/" DIGIT "." DIGIT and nothing else. Requiring it in full is what
   makes a request-line a request-line rather than any line that happens to hold two spaces — a
   `Content-Disposition: form-data; name="a b"` part header would otherwise read as one. */
static bool is_http_version(const char *s, size_t n)
{
    return n == 8 && !memcmp(s, "HTTP/", 5) &&
           s[5] >= '0' && s[5] <= '9' && s[6] == '.' && s[7] >= '0' && s[7] <= '9';
}

/* ONE PART BODY, READ AS AN RFC 9112 REQUEST MESSAGE. `p`..`end` is everything after the part's own blank line.
   A part that is not a request message records nothing: the discrimination is the GRAMMAR and never the part's
   declared `Content-Type`, because the batch conventions spell that header four ways (`application/http`,
   `application/http; msgtype=request`, `message/http`, and absent) and an allowlist of spellings is the
   recognizer shape §RUN, DON'T MATCH names — one format answering differently depending on how it was
   written. */
static void learn_request_message(JSContext *ctx, const char *p, const char *end, int prov)
{
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    const char *le = nl ? nl : end;
    const char *sp1, *sp2, *tgt, *ver, *q;
    size_t m_n, t_n;
    char *method, *target;
    EhBuf hb = { NULL, 0, 0 };
    JSValue uv;

    if (le > p && le[-1] == '\r') le--;   /* an origin-form request line ends CRLF; the CR is the terminator */

    sp1 = memchr(p, ' ', (size_t)(le - p));
    if (!sp1 || sp1 == p) return;                       /* no method token: this part is not a request */
    m_n = (size_t)(sp1 - p);
    if (!is_token(p, m_n)) return;

    tgt = sp1 + 1;
    sp2 = memchr(tgt, ' ', (size_t)(le - tgt));
    if (!sp2 || sp2 == tgt) return;                     /* no request-target */
    t_n = (size_t)(sp2 - tgt);

    ver = sp2 + 1;
    if (!is_http_version(ver, (size_t)(le - ver))) return;

    /* RFC 9112 §3.2's FOUR TARGET FORMS, and only two of them are addresses. Asterisk-form (`*`, OPTIONS only)
       names the server rather than a resource, and authority-form (`host:port`) is CONNECT's tunnel target —
       neither is a thing this surface could report as an endpoint, and recording either would put a row in the
       @H surface that no reviewer could fetch. Origin-form and absolute-form are both recorded AS WRITTEN,
       which is what `endpoint_record` already does with the page's own `fetch()` URLs: the page's own string is
       the identity, and resolving an absolute-path target against the batch URL would invent an origin for a
       batch whose own address the code has not pinned. */
    if (t_n == 1 && tgt[0] == '*') return;
    if (m_n == 7 && !memcmp(p, "CONNECT", 7)) return;

    /* THE SUB-REQUEST'S OWN FIELD LINES. Read here rather than through `header_list_parse_field_lines`
       deliberately: that reader asserts — with a CHECK that is fatal in RELEASE — that its block came from the
       trusted zone's `Headers` object, and this block is the PAGE'S bytes. Handing page data to a reader whose
       contract is "a zone built this" is how attacker-shaped input reaches an abort. */
    q = nl ? nl + 1 : end;
    while (q < end) {
        const char *lnl = memchr(q, '\n', (size_t)(end - q));
        const char *lend = lnl ? lnl : end;
        const char *colon, *vs, *ve;

        if (lend > q && lend[-1] == '\r') lend--;
        if (lend == q) break;                           /* the empty line ends the field section */
        colon = memchr(q, ':', (size_t)(lend - q));
        /* RFC 9112 §5.1: a field line without a name and a colon makes the MESSAGE invalid, so the part is not
           a request after all and nothing from it is recorded. A tolerant skip here would file half a page's
           headers under an endpoint and report the other half as absent. */
        if (!colon || colon == q || !header_name_valid(q, (size_t)(colon - q))) { eh_free(&hb); return; }
        vs = colon + 1;
        while (vs < lend && (*vs == ' ' || *vs == '\t')) vs++;    /* RFC 9112 §5's OWS around the value */
        ve = lend;
        while (ve > vs && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        eh_add(&hb, q, (size_t)(colon - q), vs, (size_t)(ve - vs));
        if (!lnl) break;
        q = lnl + 1;
    }

    method = malloc(m_n + 1); CHECK(method, "multipart_batch: OOM copying a sub-request method");
    memcpy(method, p, m_n); method[m_n] = 0;
    target = malloc(t_n + 1); CHECK(target, "multipart_batch: OOM copying a sub-request target");
    memcpy(target, tgt, t_n); target[t_n] = 0;

    uv = JS_NewString(ctx, target);
    /* AT THE OUTER REQUEST'S GRADE, handed down from the `fetch()` that composed this body — a sub-request
       written inside a body is evidence of exactly what the request carrying it is evidence of. */
    endpoint_record(ctx, method, uv, hb.n ? hb.e : NULL, hb.n, NULL, prov);
    JS_FreeValue(ctx, uv);

    free(method);
    free(target);
    eh_free(&hb);
}

/* ONE BODY PART: RFC 2046 §5.1's part headers, an empty line, then the part's content. The part headers are
   the ENVELOPE's (`Content-ID`, the part's own `Content-Type`) and belong to no endpoint — the sub-request's
   headers are inside the message, after its request line. */
static void learn_part(JSContext *ctx, const char *p, const char *end, int prov)
{
    DCHECK(p <= end, "a multipart body part was framed with its end before its start — the two pointers come "
                     "from one delimiter walk over one buffer, so an inverted span means the CRLF trim before "
                     "this call ran off the front of the part it was trimming");
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;

        if (le > p && le[-1] == '\r') le--;
        if (le == p) { learn_request_message(ctx, nl ? nl + 1 : end, end, prov); return; }
        if (!nl) return;   /* part headers that no empty line ever closed: the part carries no content */
        p = nl + 1;
    }
}

/* ── the entry ───────────────────────────────────────────────────────────────────────────────────────────── */

void multipart_batch_learn(JSContext *ctx, const HeaderList *hdrs, JSValueConst body_mime,
                           const char *body, size_t body_n, int prov)
{
    MimeType ct_rec;
    char *ct;
    const char *bnd;
    const char *body_end;
    const char *p, *first;
    char *db;
    size_t db_n, bnd_n;

    DCHECK(ctx != NULL, "a request body was read for its sub-requests with no realm to record them in — every "
                        "endpoint this file learns is recorded through a JSValue, so a null context here is a "
                        "call site that has not yet been given the realm the request belongs to");
    if (!body || body_n == 0) return;   /* §5.1's null body, or an empty one: the page sent nothing to read */
    body_end = body + body_n;

    /* Fetch §5.3 step 37.4 states the request's Content-Type in TWO places and in this order: the header list
       when it names one, the extracted body's own arm otherwise. Asking only the list would read a batch sent
       as a typed `Blob` as having no type at all. */
    ct = hdrs ? header_list_get(hdrs, "content-type") : NULL;
    if (!ct && JS_IsString(body_mime)) {
        const char *bm = JS_ToCString(ctx, body_mime);
        if (bm) {
            ct = strdup(bm);
            CHECK(ct, "multipart_batch: OOM reading a request body's own Content-Type");
            JS_FreeCString(ctx, bm);
        }
    }
    /* A REQUEST THAT STATED NO TYPE IS A POSITIVE ANSWER, not a hole: RFC 2046's framing is a boundary the
       sender NAMES, and a body with no named boundary has no parts to find. */
    if (!ct) return;

    if (!mime_type_extract(&ct_rec, ct)) { free(ct); mime_type_free(&ct_rec); return; }
    free(ct);

    /* RFC 2046 §5.1.3: an unrecognised multipart SUBTYPE is treated as `mixed`, so the framing is a property of
       the TOP-LEVEL type and the subtype decides nothing here. That is also why `multipart/form-data` needs no
       exclusion — its parts carry no request line, so the grammar refuses them and no list of subtypes has to
       be kept correct as batch APIs invent new ones. */
    if (!ct_rec.type || strcmp(ct_rec.type, "multipart")) { mime_type_free(&ct_rec); return; }
    bnd = mime_type_parameter(&ct_rec, "boundary");
    if (!bnd) { mime_type_free(&ct_rec); return; }
    bnd_n = strlen(bnd);
    /* RFC 2046 §5.1.1's boundary is 1 to 70 characters. Outside that the sender wrote something that is not a
       boundary, which is a fact about the page's data and not about this engine. */
    if (bnd_n == 0 || bnd_n > 70) { mime_type_free(&ct_rec); return; }

    db_n = bnd_n + 2;
    db = malloc(db_n + 1);
    CHECK(db, "multipart_batch: OOM building a multipart dash-boundary");
    db[0] = '-'; db[1] = '-';
    memcpy(db + 2, bnd, bnd_n);
    db[db_n] = 0;
    mime_type_free(&ct_rec);   /* `bnd` dies with the record; `db` is this walk's own copy from here */

    /* RFC 2046 §5.1.1's multipart-body: an optional preamble, then the dash-boundary that opens the first part.
       The preamble may itself contain newlines, so the opening delimiter is found rather than assumed at 0. */
    first = line_start_find(body, body_end, db, db_n);
    if (!first) { free(db); return; }
    p = after_line(first, body_end);

    while (p) {
        const char *next, *part_end, *np;

        DCHECK(p == body || p[-1] == '\n',
               "the multipart walk resumed somewhere that is not the start of a line — every position it takes "
               "comes from `after_line`, which returns the byte after an LF, and the delimiter search below is "
               "only correct for a search that begins at a line start");
        next = line_start_find(p, body_end, db, db_n);
        if (!next) break;   /* the closing delimiter never arrived: what is left is not a complete part */

        /* The CRLF immediately before a delimiter belongs to the DELIMITER (RFC 2046's `delimiter := CRLF
           dash-boundary`), not to the part it closes — a part whose content ends without one would otherwise
           gain two bytes it never had. */
        part_end = next;
        if (part_end > p && part_end[-1] == '\n') part_end--;
        if (part_end > p && part_end[-1] == '\r') part_end--;
        learn_part(ctx, p, part_end, prov);

        /* RFC 2046's close-delimiter is the delimiter followed by "--". */
        if ((size_t)(body_end - next) >= db_n + 2 && next[db_n] == '-' && next[db_n + 1] == '-') break;

        np = after_line(next, body_end);
        /* §NO BOUNDS: this loop has no iteration cap, so its termination is STRUCTURAL and asserted here rather
           than bounded. `next` was found at or after `p` and `after_line` returns a position strictly past it,
           so a position that did not advance means the search and the skip disagree about where they are. */
        DCHECK(np == NULL || np > p, "the multipart walk did not advance past a delimiter it had just read");
        p = np;
    }
    free(db);
}
