/* THE HEADERS INTERFACE — WHATWG Fetch §5, and the header list behind it.
 *
 * WHY IT EXISTS HERE. The tool's headline output is what a request NEEDS, and a header is half of that: an
 * endpoint reached only with `Authorization` and `X-Api-Version` is not usable without them, and the popup has
 * had a "Required Headers" section reading a `requiredHeaders` record for as long as it has existed — which the
 * engine never emitted, because `fetch` read `init.method` and `init.url` and nothing else. This is the first of
 * the three things that closes: the LIST, and the interface a page builds one with.
 *
 * THE LIST IS NOT A MAP. §5.1 keeps (name, value) PAIRS and appends rather than replacing, because `Set-Cookie`
 * is genuinely repeated and `getSetCookie` reads those repeats back; `get` is what combines, joining with ", "
 * per §2.2.4. A map keyed by name would answer `get` correctly and lose every repeat, which is exactly the
 * header the difference exists for.
 *
 * THE FILL IS A REQUEST SEQUENCE, not a C walk. `new Headers({'X-Api-Key': k})` converts a Web IDL
 * `record<ByteString, ByteString>`, which is [[OwnPropertyKeys]] followed by a [[Get]] per key — on a Proxy the
 * page's `ownKeys` and `get` traps, and from C that is the drive-to-completion this engine aborts on. It is
 * written as a sub-sequence rather than inside the constructor because `fetch(u, {headers: ...})` performs the
 * SAME conversion, and the spec states it once. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/fetch/headers.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/idl_iter.h"
#include "solver/concolic.h"

static JSClassID g_headers_class;
static int       g_ctor_stepid = -1;
static JSRuntime *g_headers_rt;

/* ---- the header list ---------------------------------------------------------------------------------- */

/* §5.1 normalizes a header NAME to lowercase; a value keeps its case. Done on the way IN, so every comparison
   below is a plain strcmp and no consumer has to remember. */
static char *header_lower(const char *s)
{
    size_t i, n = strlen(s);
    char *r = malloc(n + 1);
    CHECK(r, "headers: OOM copying a header name — a dropped header loses what the endpoint requires");
    for (i = 0; i < n; i++)
        r[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] - 'A' + 'a') : s[i];
    r[n] = 0;
    return r;
}

static char *header_dup(const char *s)
{
    char *r = strdup(s ? s : "");
    CHECK(r, "headers: OOM copying a header value");
    return r;
}

void header_list_free(HeaderList *l)
{
    int i;
    if (!l) return;
    for (i = 0; i < l->n; i++) { free(l->e[i].name); free(l->e[i].value); }
    free(l->e);
    l->e = NULL; l->n = l->cap = 0;
}

void header_list_append(HeaderList *l, const char *name, const char *value)
{
    DCHECK(l != NULL && name != NULL, "a header was appended to no list");
    if (l->n >= l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->e = realloc(l->e, (size_t)l->cap * sizeof(HeaderEntry));
        CHECK(l->e, "headers: OOM growing a header list");
    }
    l->e[l->n].name = header_lower(name);
    l->e[l->n].value = header_dup(value);
    l->n++;
}

void header_list_delete(HeaderList *l, const char *name)
{
    char *lo = header_lower(name);
    int i, w = 0;
    for (i = 0; i < l->n; i++) {
        if (!strcmp(l->e[i].name, lo)) { free(l->e[i].name); free(l->e[i].value); continue; }
        l->e[w++] = l->e[i];
    }
    l->n = w;
    free(lo);
}

void header_list_set(HeaderList *l, const char *name, const char *value)
{
    header_list_delete(l, name);
    header_list_append(l, name, value);
}

void header_list_parse_field_lines(HeaderList *l, const char *block)
{
    const char *p = block;

    DCHECK(l != NULL, "a field block was parsed into no header list");
    DCHECK(l->n == 0, "a field block was parsed into a header list that already holds headers — a response has "
                      "ONE header list and it is built once, so a second parse into the same list would make "
                      "one response look like two and Fetch's `get` would join the pair");
    if (!block) return;   /* a response with no headers at all — a real answer, and an empty list is it */
    while (*p) {
        const char *eol = strchr(p, '\n');
        const char *end = eol ? eol : p + strlen(p);
        const char *colon;
        const char *vs;
        char *name;

        /* RFC 9112's field lines end CRLF, so a CR before the LF is the TERMINATOR and not part of the value.
           Taking it as one is how a `Content-Security-Policy` acquires a trailing carriage return and stops
           matching anything the parser below compares it against. */
        if (end > p && end[-1] == '\r') end--;
        colon = memchr(p, ':', (size_t)(end - p));

        if (end == p) { p = eol ? eol + 1 : end; continue; }   /* a blank line separates nothing here */
        /* THE ZONE THAT WROTE THIS BLOCK HAD A `Headers` OBJECT, so every line it wrote came from a real
           response and has a name and a value. A line without a colon is that zone's bug, and it is one this
           engine must not paper over: dropping it silently would make a `Content-Security-Policy` that the
           server DID send disappear, and the sink it kills would be reported as a working exploit. */
        CHECK(colon != NULL, "a response field block carries a line with no colon — the trusted zone builds "
                             "this from a `Headers` object, where every entry is a (name, value) pair, so a "
                             "line that is not one means the serialization and this parse disagree. ALWAYS "
                             "fatal rather than dev-only: what follows this test is a read through the colon, "
                             "and a release build that skipped the line would drop a policy the server sent");
        vs = colon + 1;
        while (vs < end && (*vs == ' ' || *vs == '\t')) vs++;   /* RFC 9112's OWS after the colon */
        name = malloc((size_t)(colon - p) + 1);
        CHECK(name != NULL, "headers: OOM reading a response field line");
        memcpy(name, p, (size_t)(colon - p));
        name[colon - p] = 0;
        /* §5.1's grammar, asked of input that came from outside this engine. A name that is not a TOKEN could
           not have come off a `Headers` object, so it is the same disagreement the colon check names. */
        DCHECK(header_name_valid(name, strlen(name)),
               "a response field block carries a line whose name is not an HTTP token — a header list stores "
               "what a response delivered, and a name Fetch would have rejected never was one");
        {
            char *value = malloc((size_t)(end - vs) + 1);
            CHECK(value != NULL, "headers: OOM reading a response field value");
            memcpy(value, vs, (size_t)(end - vs));
            value[end - vs] = 0;
            /* APPEND, NEVER SET: §5.1 keeps repeats, and §7.1.4.1's own table turns on them — two
               `Cross-Origin-Embedder-Policy: require-corp` headers must combine into a value that FAILS to
               parse as an item, which a list that replaced would have quietly turned into one that succeeds. */
            header_list_append(l, name, value);
            free(value);
        }
        free(name);
        p = eol ? eol + 1 : end;
    }
}

char *header_list_get(const HeaderList *l, const char *name)
{
    char *lo = header_lower(name), *out = NULL;
    size_t total = 0;
    int i, first = 1;

    for (i = 0; i < l->n; i++)
        if (!strcmp(l->e[i].name, lo))
            total += strlen(l->e[i].value) + 2;   /* ", " between, never after the last */
    if (!total) { free(lo); return NULL; }
    out = malloc(total + 1);
    CHECK(out, "headers: OOM joining a header's values");
    out[0] = 0;
    for (i = 0; i < l->n; i++) {
        if (strcmp(l->e[i].name, lo)) continue;
        if (!first) strcat(out, ", ");
        strcat(out, l->e[i].value);
        first = 0;
    }
    free(lo);
    return out;
}

/* ---- the interface ------------------------------------------------------------------------------------ */

/* §5.1: "A Headers object has an associated GUARD." It is the object's, not the list's — the same header list
   is reachable through a Response's immutable Headers and through the fetch machinery that built it, and only
   the first refuses writes. So the class opaque is the pair. */
typedef struct { HeaderList list; uint8_t guard; } HeadersObj;

static void headers_finalizer(JSRuntime *rt, JSValue val)
{
    HeadersObj *h = JS_GetOpaque(val, g_headers_class);
    (void)rt;
    if (h) { header_list_free(&h->list); free(h); }
}

const HeaderList *headers_list_of(JSValueConst v)
{
    HeadersObj *h = JS_GetOpaque(v, g_headers_class);
    return h ? &h->list : NULL;
}

HeadersGuard headers_guard_of(JSValueConst v)
{
    HeadersObj *h = JS_GetOpaque(v, g_headers_class);
    DCHECK(h != NULL, "the guard of something that is not a Headers was asked for");
    return (HeadersGuard)h->guard;
}

static HeadersObj *headers_of(JSContext *ctx, JSValueConst v)
{
    HeadersObj *h = JS_GetOpaque(v, g_headers_class);
    if (!h) JS_ThrowTypeError(ctx, "not a Headers");
    return h;
}

JSValue headers_new(JSContext *ctx, const HeaderList *src, HeadersGuard guard)
{
    HeadersObj *h;
    JSValue obj;
    int i;

    DCHECK(g_headers_class != 0, "a Headers was built before the class existed — headers_init runs at install");
    {
        JSValue proto = JS_GetClassProto(ctx, g_headers_class);
        DCHECK(!JS_IsNull(proto), "a Headers was minted in a realm that never ran its install");
        obj = JS_NewObjectProtoClass(ctx, proto, g_headers_class);
        JS_FreeValue(ctx, proto);
    }
    if (JS_IsException(obj))
        return obj;
    h = calloc(1, sizeof *h);
    CHECK(h, "headers: OOM building a Headers");
    h->guard = (uint8_t)guard;
    /* The list is copied ALREADY VALIDATED — it is a header list this engine built, never a page's. The guard
       governs what the PAGE may then do to it, which is why "immutable" does not block this loop. */
    for (i = 0; src && i < src->n; i++)
        header_list_append(&h->list, src->e[i].name, src->e[i].value);
    JS_SetOpaque(obj, h);
    return obj;
}

/* §5.2's members. Every argument is a ByteString, which the IDL machine has already made a real string by the
   time a body runs — so what is left here is the BYTE range, which ToString does not enforce and which is the
   whole of what makes a ByteString different from a DOMString. */
/* EVERY CHECK HERE IS LENGTH-DELIMITED, because U+0000 IS A ByteString CHARACTER. These read a C string and
   stopped at the first NUL, so `new Headers({"set-cookie": "\0"})` presented an EMPTY value, normalized to
   empty, validated clean, and was stored — where the spec forbids 0x00 in a header value and wpt asserts the
   TypeError. A NUL cannot be excluded by the shape of the buffer; it has to be looked for.
   What survives validation is provably NUL-free (a name is a token, a value forbids 0x00), which is why the
   LIST may still hold plain C strings — and header_check DCHECKs exactly that before handing one over. */


/* §5.1 "HTTP whitespace" — these FOUR and not isspace()'s set. \f is not one of them, which is what makes
   wpt's "\t\f\tnewLine\n" normalize to "\f\tnewLine" rather than to "newLine". */
static int header_is_ws(unsigned char c) { return c == 0x09 || c == 0x0a || c == 0x0d || c == 0x20; }

/* §5.1 "normalize a header value": strip LEADING and TRAILING HTTP whitespace, never inner. Caller frees.
   `*pn` is the normalized LENGTH, which is not strlen(out) when the value carries an embedded NUL — the case
   the validation below exists to reject. */
static char *header_normalize_value(const char *v, size_t len, size_t *pn)
{
    const char *b = v, *e = v + len;
    char *out;
    size_t n;
    while (b < e && header_is_ws((unsigned char)*b)) b++;
    while (e > b && header_is_ws((unsigned char)e[-1])) e--;
    n = (size_t)(e - b);
    out = malloc(n + 1);
    CHECK(out, "headers: OOM normalizing a header value");
    memcpy(out, b, n);
    out[n] = 0;
    *pn = n;
    return out;
}

/* §5.1 "a header name is a NAME": an RFC 7230 token — one or more tchar and nothing else. `{}` reaches this as
   "[object Object]", and the space and brackets are what make it a TypeError rather than a header. */
static int header_name_is_valid(const char *s, size_t len)
{
    const unsigned char *p = (const unsigned char *)s, *end = p + len;
    if (len == 0) return 0;
    for (; p < end; p++) {
        unsigned char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) continue;
        if (c && strchr("!#$%&'*+-.^_`|~", (char)c)) continue;   /* c == 0 is not a tchar */
        return 0;
    }
    return 1;
}

/* §5.1 "a header value is a VALUE": no NUL, CR or LF anywhere. The leading/trailing whitespace the definition
   also forbids is what normalization has already removed. */
static int header_value_is_valid(const char *s, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++)
        if (s[i] == 0x00 || s[i] == 0x0a || s[i] == 0x0d) return 0;
    return 1;
}

bool header_name_valid(const char *name, size_t len) { return header_name_is_valid(name, len) != 0; }

char *header_value_normalize_valid(const char *value, size_t len, size_t *pn)
{
    size_t n = 0;
    char *norm = header_normalize_value(value, len, &n);

    if (!header_value_is_valid(norm, n)) { free(norm); return NULL; }
    if (pn) *pn = n;
    return norm;
}

/* §5.1's guard as ONE operation, because every entry point performs the same one: normalize the value, then
   reject a bad name or a bad value with a TypeError. `*pnorm` is the normalized value (caller frees), left NULL
   for the name-only members. 0 on success, -1 with a TypeError live. */
static int header_check(JSContext *ctx, const char *name, size_t name_len,
                       const char *value, size_t value_len, char **pnorm)
{
    char *norm;
    size_t norm_len;
    if (pnorm) *pnorm = NULL;
    if (!idl_is_bytestring(name, name_len) || (value && !idl_is_bytestring(value, value_len))) {
        JS_ThrowTypeError(ctx, "a header name or value is not a ByteString");
        return -1;
    }
    if (!header_name_is_valid(name, name_len)) {
        JS_ThrowTypeError(ctx, "invalid header name");
        return -1;
    }
    DCHECK(strlen(name) == name_len, "a header name passed validation while carrying an embedded NUL");
    if (!value) return 0;
    norm = header_normalize_value(value, value_len, &norm_len);
    if (!header_value_is_valid(norm, norm_len)) {
        free(norm);
        JS_ThrowTypeError(ctx, "invalid header value");
        return -1;
    }
    /* THE LIST MAY HOLD A C STRING because of this: a value that got here has no 0x00 in it, so nothing is
       lost by dropping the length. The assert is what keeps that true if the rule above ever changes. */
    DCHECK(strlen(norm) == norm_len, "a header value passed validation while carrying an embedded NUL");
    *pnorm = norm;
    return 0;
}

/* ---- §5.1's guard: which writes a PAGE is allowed to make -------------------------------------------------
 *
 * A header list this engine builds is trusted; what the guard governs is what the page may do to it afterwards.
 * The three answers are distinct and the spec is explicit about which is which: "immutable" THROWS a TypeError,
 * a forbidden name under "request"/"response" is a SILENT no-op (validating returns false and the member simply
 * returns), and everything else writes. Collapsing the silent case into a throw would break every page that
 * sets `Host` defensively; collapsing it into a write would let a page forge headers the browser owns. */

static int header_ci_eq(const char *lower_name, const char *lit)
{
    /* names arrive lowercased or are lowercased by the caller; the literal is written lowercase */
    return !strcmp(lower_name, lit);
}

/* §5.1 "forbidden method": the three a page may never send, however it spells them. */
static int header_is_forbidden_method(const char *m, size_t len)
{
    static const char *const METHODS[] = { "connect", "trace", "track" };
    size_t i, k;
    char buf[8];
    if (len >= sizeof buf) return 0;
    for (i = 0; i < len; i++)
        buf[i] = (m[i] >= 'A' && m[i] <= 'Z') ? (char)(m[i] - 'A' + 'a') : m[i];
    buf[len] = 0;
    for (k = 0; k < sizeof(METHODS) / sizeof(METHODS[0]); k++)
        if (!strcmp(buf, METHODS[k])) return 1;
    return 0;
}

/* §5.1 "getting, decoding, and splitting" a value, for the method-override headers: split on ",", strip HTTP
   whitespace around each token, and ask whether ANY of them is a forbidden method. `X-HTTP-Method: ",TRACE,"`
   is forbidden for the same reason `X-HTTP-Method: TRACE` is — the server would see both. */
static int header_value_has_forbidden_method(const char *v)
{
    const char *p = v;
    for (;;) {
        const char *comma = strchr(p, ',');
        const char *b = p, *e = comma ? comma : p + strlen(p);
        while (b < e && header_is_ws((unsigned char)*b)) b++;
        while (e > b && header_is_ws((unsigned char)e[-1])) e--;
        if (header_is_forbidden_method(b, (size_t)(e - b))) return 1;
        if (!comma) return 0;
        p = comma + 1;
    }
}

/* §5.1's OWN list, in the standard's own order and containing nothing else — so a name added to or removed
   from this array is a diff against the spec text and can be read as one. Fetch's list is not the whole of the
   platform's, and the way a reader tells the difference is that the other standards' entries are BELOW, each
   under the sentence that adds it. */
static const char *const FORBIDDEN_REQUEST_FETCH[] = {
    "accept-charset", "accept-encoding", "access-control-request-headers",
    "access-control-request-method", "connection", "content-length", "cookie", "cookie2", "date", "dnt",
    "expect", "host", "keep-alive", "origin", "referer", "set-cookie", "te", "trailer",
    "transfer-encoding", "upgrade", "via",
};

/* PRIVATE NETWORK ACCESS §3.4.4 "Forbidden header names", in full: "A new entry is added to the list of
   forbidden request-header names: `Access-Control-Request-Private-Network`." (Draft Community Group Report,
   26 September 2024, https://wicg.github.io/private-network-access/#forbidden-header-names.) The note under it
   gives the reason this component cares: "The user agent should have full control over this header, just as it
   does over other CORS headers."
 *
 * IT IS KEPT, AND THIS IS THE DECISION RATHER THAN AN ACCIDENT. `private-network` does not occur anywhere in
 * fetch.spec.whatwg.org, and an entry that matched only Chrome's behaviour would be exactly the kind of
 * hardcoded string this codebase deletes. It is not one: the platform's list is the union of Fetch's plus every
 * standard that adds to it, and this is a live specification adding to it in those words — so keeping it IS
 * "implement the spec at the root", and Chrome shipping it is the CONFIRMATION, in that order and not the
 * other. The array it lives in is separate so that stays legible without a reader having to trust a comment:
 * two lists, two standards, and the Fetch one still diffs clean against §5.1.
 *
 * The consequence this engine is built for follows the same way. What a request CARRIES is the report's claim,
 * and a header the user agent strips is a header the report must not carry — so a bundle that sets this name
 * gets the browser's answer (dropped, `get` answers null) rather than a fabricated one. The rest of Private
 * Network Access is a network-layer feature (target IP address space, the preflight that would send this
 * header); none of it is scriptable and none of it changes what this list answers. */
static const char *const FORBIDDEN_REQUEST_PNA[] = {
    "access-control-request-private-network",
};

/* §5.1 "forbidden request-header". The name arrives LOWERCASED (header_lower is what every entry point runs
   first), so these comparisons are the spec's byte-case-insensitive match. */
static int header_is_forbidden_request(const char *lower_name, const char *value)
{
    size_t i;
    for (i = 0; i < sizeof(FORBIDDEN_REQUEST_FETCH) / sizeof(FORBIDDEN_REQUEST_FETCH[0]); i++)
        if (header_ci_eq(lower_name, FORBIDDEN_REQUEST_FETCH[i])) return 1;
    for (i = 0; i < sizeof(FORBIDDEN_REQUEST_PNA) / sizeof(FORBIDDEN_REQUEST_PNA[0]); i++)
        if (header_ci_eq(lower_name, FORBIDDEN_REQUEST_PNA[i])) return 1;
    /* the two PREFIXES the spec reserves for the browser and for the platform */
    if (!strncmp(lower_name, "proxy-", 6) || !strncmp(lower_name, "sec-", 4)) return 1;
    /* the method-override family is forbidden only for the VALUES that would smuggle a forbidden method */
    if (header_ci_eq(lower_name, "x-http-method") || header_ci_eq(lower_name, "x-http-method-override") ||
        header_ci_eq(lower_name, "x-method-override"))
        return value && header_value_has_forbidden_method(value);
    return 0;
}

bool header_forbidden_request(const char *lower_name, const char *value)
{
    return header_is_forbidden_request(lower_name, value) != 0;
}

/* §5.1 "forbidden response-header name": the two a page may not put on a response it did not receive. */
static int header_is_forbidden_response(const char *lower_name)
{
    return header_ci_eq(lower_name, "set-cookie") || header_ci_eq(lower_name, "set-cookie2");
}

/* §5.1's "CORS-unsafe request-header byte": what a safelisted value may not contain. */
static int header_is_cors_unsafe_byte(unsigned char c)
{
    return (c < 0x20 && c != 0x09) || c == 0x7f || !!strchr("\"():<>?@[\\]{}", (char)c);
}

/* §5.1's "CORS-safelisted request-header". The names each have their OWN value rule — this is not a name
   list with a length cap bolted on, and treating it as one would let `Content-Type: application/json` through
   as safelisted, which is the whole difference between a preflighted request and one that is not. */
static int header_is_cors_safelisted(const char *lower_name, const char *value)
{
    size_t i, n = strlen(value);

    /* §5.1 HAS A FIFTH ARM THIS COMPONENT HAS NOT BUILT: "`range`: Let rangeValue be the result of parsing a
       single range header value given value and false. If rangeValue is failure, then return false. If
       rangeValue[0] is null, then return false." — a suffix range like `bytes=-500` is deliberately NOT
       safelisted. Every caller today reaches this through the no-CORS safelist, whose four names do not
       include `range`, so the arm is unreachable; this is what makes it CRASH the day the CORS-unsafe
       request-header names (the preflight computation) asks the question for real, rather than quietly
       answering false and preflighting a request Chrome sends directly. */
    DCHECK(strcmp(lower_name, "range") != 0,
           "§5.1's CORS-safelisted request-header was asked about `range`, whose arm parses a single range "
           "header value and is not built — build it here rather than letting this answer false");
    if (n > 128) return 0;
    if (!strcmp(lower_name, "accept")) {
        for (i = 0; i < n; i++) if (header_is_cors_unsafe_byte((unsigned char)value[i])) return 0;
        return 1;
    }
    if (!strcmp(lower_name, "accept-language") || !strcmp(lower_name, "content-language")) {
        for (i = 0; i < n; i++) {
            unsigned char c = (unsigned char)value[i];
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) continue;
            if (c == 0x20 || c == '*' || c == ',' || c == '-' || c == '.' || c == ';' || c == '=') continue;
            return 0;
        }
        return 1;
    }
    if (!strcmp(lower_name, "content-type")) {
        const char *semi;
        size_t essence;
        for (i = 0; i < n; i++) if (header_is_cors_unsafe_byte((unsigned char)value[i])) return 0;
        /* the MIME type's ESSENCE — everything before the first `;`, trimmed — must be one of three */
        semi = strchr(value, ';');
        essence = semi ? (size_t)(semi - value) : n;
        while (essence > 0 && header_is_ws((unsigned char)value[essence - 1])) essence--;
        {
            static const char *const OK[] = { "application/x-www-form-urlencoded", "multipart/form-data",
                                              "text/plain" };
            size_t k;
            for (k = 0; k < sizeof(OK) / sizeof(OK[0]); k++)
                if (essence == strlen(OK[k]) && !strncasecmp(value, OK[k], essence)) return 1;
        }
        return 0;
    }
    return 0;
}

/* §5.1's "no-CORS-safelisted request-header NAME": one of four. It is its own predicate because `delete` asks
   the NAME question on its own — with no value to ask the value question about. */
static int header_is_no_cors_safelisted_name(const char *lower_name)
{
    return !strcmp(lower_name, "accept") || !strcmp(lower_name, "accept-language") ||
           !strcmp(lower_name, "content-language") || !strcmp(lower_name, "content-type");
}

/* §5.1's "no-CORS-safelisted request-header": one of those four names, and then the CORS rule for its value. */
static int header_is_no_cors_safelisted(const char *lower_name, const char *value)
{
    return header_is_no_cors_safelisted_name(lower_name) && header_is_cors_safelisted(lower_name, value);
}

/* §5.1's "privileged no-CORS request-header names" — stated ONCE, because two things read it: `delete`, which
   lets one THROUGH (the browser's header is exactly what unprivileged code is allowed to drop), and "remove
   privileged no-CORS request-headers", which strips every one of them after any unprivileged write. */
static const char *const HEADER_PRIVILEGED_NO_CORS[] = { "range" };

static int header_is_privileged_no_cors(const char *lower_name)
{
    size_t i;
    for (i = 0; i < sizeof(HEADER_PRIVILEGED_NO_CORS) / sizeof(HEADER_PRIVILEGED_NO_CORS[0]); i++)
        if (!strcmp(lower_name, HEADER_PRIVILEGED_NO_CORS[i])) return 1;
    return 0;
}

/* §5.1 "remove privileged no-CORS request-headers": every append, set and delete under the "request-no-cors"
   guard ends with it — "This is called when headers are modified by unprivileged code", and the spec's own
   note beside the privileged list says such a header is "preserved if their associated request object is
   copied, but will be removed if the request is modified by unprivileged APIs". */
static void header_remove_privileged_no_cors(HeaderList *l)
{
    size_t i;
    for (i = 0; i < sizeof(HEADER_PRIVILEGED_NO_CORS) / sizeof(HEADER_PRIVILEGED_NO_CORS[0]); i++)
        header_list_delete(l, HEADER_PRIVILEGED_NO_CORS[i]);
}

/* §5.1 "validating (name, value) for headers", AFTER header_check has done step 1's name/value syntax half.
   Returns 1 to write, 0 to silently do nothing, -1 with a TypeError live.
   STEP 3 IS "request" AND NOT "request-no-cors". The spec says so twice — the step names one guard, and the
   note under it says "Steps for 'request-no-cors' are not shared as you cannot have a fake value (for
   delete()) that always succeeds in CORS-safelisted request-header". This asked the forbidden question for
   both, which made `delete` under "request-no-cors" answer out of the forbidden list instead of out of that
   guard's own step. */
static int headers_guard_allows(JSContext *ctx, uint8_t guard, const char *name, const char *value)
{
    char *lower = header_lower(name);
    int r = 1;
    if (guard == HEADERS_GUARD_IMMUTABLE) {
        JS_ThrowTypeError(ctx, "the headers are immutable");
        r = -1;
    } else if (guard == HEADERS_GUARD_REQUEST && header_is_forbidden_request(lower, value)) {
        r = 0;
    } else if (guard == HEADERS_GUARD_RESPONSE && header_is_forbidden_response(lower)) {
        r = 0;
    }
    free(lower);
    return r;
}

/* §5.1 "APPEND A HEADER (name, value) TO A HEADERS OBJECT", steps 2-5, over the LIST and the GUARD — which is
   all the algorithm reads. ONE function because the standard states it once and TWO callers run it: the
   `append` member, and the FILL, which the standard defines as "append (key, value) to headers" for every
   entry of the init and NOT as a list write of its own. The fill ran step 2 and then wrote the list itself,
   so it skipped step 3 — `new Request(u, {mode:"no-cors", headers:{"X-Custom":"1"}})` kept a header every
   browser drops — and its record arm REPLACED a same-named entry, so `new Headers({a:"1", A:"2"}).get("a")`
   answered "2" where appending twice and combining gives "1, 2".
   `norm` is the already-normalized value (step 1). Returns 0 for written OR silently refused — two outcomes
   the spec does not let a caller distinguish — and -1 with a TypeError live. */
static int headers_append_one(JSContext *ctx, HeaderList *l, uint8_t guard, const char *name, const char *norm)
{
    char *lower = header_lower(name);
    int allow = headers_guard_allows(ctx, guard, name, norm);   /* step 2 */

    /* step 3: under "request-no-cors" the header must be no-CORS-safelisted with the value it would END UP
       with — appending a second 127-byte `accept` makes 255 bytes, which is not safelisted, so the append
       does nothing. That is why this joins and `set` below does not. */
    if (allow > 0 && guard == HEADERS_GUARD_REQUEST_NO_CORS) {
        char *existing = header_list_get(l, lower), *combined = NULL;
        const char *test = norm;
        if (existing) {
            size_t a = strlen(existing), b = strlen(norm);
            combined = malloc(a + b + 3);
            CHECK(combined, "headers: OOM joining a no-cors value");
            memcpy(combined, existing, a);
            combined[a] = ','; combined[a + 1] = ' ';
            memcpy(combined + a + 2, norm, b);
            combined[a + b + 2] = 0;
            test = combined;
        }
        free(existing);
        if (!header_is_no_cors_safelisted(lower, test)) allow = 0;
        free(combined);
    }
    if (allow > 0) {
        header_list_append(l, name, norm);                                          /* step 4 */
        if (guard == HEADERS_GUARD_REQUEST_NO_CORS) header_remove_privileged_no_cors(l);   /* step 5 */
    }
    free(lower);
    return allow < 0 ? -1 : 0;
}

enum { HDR_APPEND = 0, HDR_SET, HDR_DELETE, HDR_GET, HDR_HAS, HDR_GETSETCOOKIE, HDR_MEMBER_N };
/* THE AGENT'S POOL ENTRIES, one per §5.2 operation — the OBJECTS they are installed as are each realm's. */
static int g_id[HDR_MEMBER_N];

static JSValue js_headers_member(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    HeadersObj *h = headers_of(ctx, this_val);
    HeaderList *l = h ? &h->list : NULL;
    const char *name = NULL, *value = NULL;
    size_t name_len = 0, value_len = 0;
    char *norm = NULL;
    JSValue r = JS_UNDEFINED;

    if (!h)
        return JS_EXCEPTION;
    if (magic == HDR_GETSETCOOKIE) {
        /* §5.2 getSetCookie(): every `set-cookie` value, each on its own — the one member for which the list's
           repeats are the answer rather than something `get` folds away. */
        JSValue arr = JS_NewArray(ctx);
        uint32_t k = 0;
        int i;
        if (JS_IsException(arr)) return arr;
        for (i = 0; i < l->n; i++)
            if (!strcmp(l->e[i].name, "set-cookie"))
                JS_SetPropertyUint32(ctx, arr, k++, JS_NewString(ctx, l->e[i].value));
        return arr;
    }
    DCHECK(argc >= 1, "a Headers member was declared with fewer arguments than its IDL lists");
    name = JS_ToCStringLen(ctx, &name_len, argv[0]);
    if (!name) return JS_EXCEPTION;
    if (magic == HDR_APPEND || magic == HDR_SET) {
        value = JS_ToCStringLen(ctx, &value_len, argv[1]);
        if (!value) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
    }
    /* §5.1's guard, on EVERY member and not just the two that write. `headers.get({})` reads a name of
       "[object Object]", which is not a token, and the spec makes that a TypeError rather than a miss — wpt
       asserts it by name for get, has, delete and set alike. */
    if (header_check(ctx, name, name_len, value, value_len, value ? &norm : NULL) < 0) {
        JS_FreeCString(ctx, name);
        if (value) JS_FreeCString(ctx, value);
        return JS_EXCEPTION;
    }
    /* §5.2's three WRITES each run their own steps; a read is not guarded at all, because the page can
       already see every one of these headers and refusing to answer would hide state rather than protect it. */
    switch (magic) {
    case HDR_APPEND:
        if (headers_append_one(ctx, l, h->guard, name, norm) < 0) {
            JS_FreeCString(ctx, name);
            JS_FreeCString(ctx, value);
            free(norm);
            return JS_EXCEPTION;
        }
        break;
    case HDR_SET: {
        /* §5.2 set steps 2-5. Step 3 tests the value ON ITS OWN, because set REPLACES rather than joins. */
        char *lower = header_lower(name);
        int allow = headers_guard_allows(ctx, h->guard, name, norm);
        if (allow > 0 && h->guard == HEADERS_GUARD_REQUEST_NO_CORS &&
            !header_is_no_cors_safelisted(lower, norm))
            allow = 0;
        if (allow > 0) {
            header_list_set(l, name, norm);
            if (h->guard == HEADERS_GUARD_REQUEST_NO_CORS) header_remove_privileged_no_cors(l);
        }
        free(lower);
        if (allow < 0) {
            JS_FreeCString(ctx, name);
            JS_FreeCString(ctx, value);
            free(norm);
            return JS_EXCEPTION;
        }
        break;
    }
    case HDR_DELETE: {
        /* §5.2 delete steps 1-5, and step 2 IS NOT WHAT STOOD HERE. It reads: "If this's guard is
           'request-no-cors', name is not a no-CORS-safelisted request-header name, AND name is not a
           privileged no-CORS request-header name, then return." A privileged name is one of the two that get
           THROUGH — the note beside the privileged list says such a header "will be removed if the request is
           modified by unprivileged APIs", so `delete("Range")` is exactly the call that must succeed. This
           refused that one call and let every OTHER unsafelisted name through, which is both halves inverted.
           Step 1 validates with the DUMMY value ``, which is why the method-override names are deletable. */
        char *lower = header_lower(name);
        int allow = headers_guard_allows(ctx, h->guard, name, "");
        if (allow > 0 && h->guard == HEADERS_GUARD_REQUEST_NO_CORS &&
            !header_is_no_cors_safelisted_name(lower) && !header_is_privileged_no_cors(lower))
            allow = 0;
        if (allow > 0) {
            header_list_delete(l, name);
            if (h->guard == HEADERS_GUARD_REQUEST_NO_CORS) header_remove_privileged_no_cors(l);
        }
        free(lower);
        if (allow < 0) {
            JS_FreeCString(ctx, name);
            JS_FreeCString(ctx, value);
            free(norm);
            return JS_EXCEPTION;
        }
        break;
    }
    case HDR_HAS: {
        char *v = header_list_get(l, name);
        r = JS_NewBool(ctx, v != NULL);
        free(v);
        break;
    }
    default: {
        char *v;
        DCHECK(magic == HDR_GET, "a Headers member was declared with a magic this component does not answer");
        v = header_list_get(l, name);
        r = v ? JS_NewString(ctx, v) : JS_NULL;   /* §5.2: absent is null, not "" */
        free(v);
    }
    }
    JS_FreeCString(ctx, name);
    if (value) JS_FreeCString(ctx, value);
    free(norm);
    return r;
}

/* ---- iteration: §5.2's `iterable<ByteString, ByteString>` ------------------------------------------------ */

/* §5.2 "sort and combine": iteration does NOT walk the raw list. It yields each name ONCE, lowercased and in
   byte order, with that name's values joined by ", " — so `for (const [k, v] of h)` over an append-append pair
   sees one entry, not two. `Set-Cookie` is the exception the spec spells out: each of its values is yielded on
   its own, which is the same reason the list keeps pairs at all.
   Computed per call rather than cached, because the list is live: a callback that appends during forEach must
   be seen by the steps after it, which is what the spec's "value pairs to iterate over" means. */
static void header_sort_and_combine(const HeaderList *l, HeaderList *out)
{
    int i, j;
    for (i = 0; i < l->n; i++) {
        const char *name = l->e[i].name;
        int seen = 0, first = 1;
        char *joined = NULL;
        size_t total = 0;
        for (j = 0; j < i; j++) if (!strcmp(l->e[j].name, name)) { seen = 1; break; }
        if (seen) continue;
        if (!strcmp(name, "set-cookie")) {            /* each value on its own, per §5.2 */
            for (j = 0; j < l->n; j++)
                if (!strcmp(l->e[j].name, name)) header_list_append(out, name, l->e[j].value);
            continue;
        }
        for (j = 0; j < l->n; j++)
            if (!strcmp(l->e[j].name, name)) total += strlen(l->e[j].value) + 2;
        joined = malloc(total + 1);
        CHECK(joined, "headers: OOM combining a header's values for iteration");
        joined[0] = 0;
        for (j = 0; j < l->n; j++) {
            if (strcmp(l->e[j].name, name)) continue;
            if (!first) strcat(joined, ", ");
            strcat(joined, l->e[j].value);
            first = 0;
        }
        header_list_append(out, name, joined);
        free(joined);
    }
    /* byte order over the names, which is what "sort" means for a header list */
    for (i = 1; i < out->n; i++) {
        HeaderEntry tmp = out->e[i];
        for (j = i - 1; j >= 0 && strcmp(out->e[j].name, tmp.name) > 0; j--)
            out->e[j + 1] = out->e[j];
        out->e[j + 1] = tmp;
    }
}

/* §3.7.10's DEFAULT ITERATOR OBJECT is the shared one. Headers' own copy of it — the iterator class, the
   prototype, `next`, `keys`/`values`/`entries`, `@@iterator` and the forEach machine — is deleted rather than
   kept beside it: the six things are identical for every `iterable<K, V>` interface, and what is actually
   Headers' is the two operations below. */
static int headers_pair_count(JSContext *ctx, JSValueConst target)
{
    const HeaderList *src = headers_list_of(target);
    HeaderList combined = { 0 };
    int n;
    (void)ctx;
    if (!src) return -1;
    header_sort_and_combine(src, &combined);
    n = combined.n;
    header_list_free(&combined);
    return n;
}

static void headers_pair_at(JSContext *ctx, JSValueConst target, int i, JSValue *key, JSValue *value)
{
    const HeaderList *src = headers_list_of(target);
    HeaderList combined = { 0 };
    DCHECK(src != NULL, "a Headers iterator outlived the Headers it holds a reference to");
    header_sort_and_combine(src, &combined);
    DCHECK(i < combined.n, "a Headers pair was asked for past the end of the combined list");
    *key = JS_NewString(ctx, combined.e[i].name);
    *value = JS_NewString(ctx, combined.e[i].value);
    header_list_free(&combined);
}

static const IdlPairIterOps HEADERS_PAIR_OPS = { headers_pair_count, headers_pair_at, "Headers" };
static int g_pair_handle = -1;

/* ---- the fill (HeadersInit -> a header list) ------------------------------------------------------------ */

enum { FILL_START = 0, FILL_ITER_ASKED, FILL_SEQ_PAIR, FILL_SEQ_ITEM, FILL_KEY_PAIR, FILL_VALUE_STR };

/* ---- the fill's own state ---------------------------------------------------------------------------------- */

void headers_fill_init(HeadersFill *f)
{
    memset(f, 0, sizeof *f);
    f->name = f->value = JS_UNDEFINED;
    record_cursor_init(&f->rec);
    f->item[0] = f->item[1] = JS_UNDEFINED;
    iter_cursor_init(&f->outer);
    iter_cursor_init(&f->inner);
}

void headers_fill_visit(JSContext *ctx, HeadersFill *f, JSStepVisit *v)
{
    record_cursor_visit(ctx, &f->rec, v);
    v->val(ctx, &f->name);
    v->val(ctx, &f->value);
    v->val(ctx, &f->item[0]);
    v->val(ctx, &f->item[1]);
    iter_cursor_visit(ctx, &f->outer, v);
    iter_cursor_visit(ctx, &f->inner, v);
}

/* Web IDL §3.2.23 step 4.2.1's `typedKey = key converted to K`, and K is ByteString — run BEFORE the value's [[Get]] is
   issued, which is what makes a record of {a:"b", "\uFFFF":"d"} five operations and not six. A SYMBOL cannot
   be a ByteString, so an ENUMERABLE symbol key is a TypeError; skipping it was the older, wrong answer. */
static int headers_record_key_ok(JSContext *ctx, JSValueConst key, void *user)
{
    size_t kn_len = 0;
    const char *kn;
    int ok;

    (void)user;
    if (JS_IsSymbol(key)) {
        JS_ThrowTypeError(ctx, "a Symbol is not a valid header name");
        return -1;
    }
    kn = JS_ToCStringLen(ctx, &kn_len, key);
    if (!kn) return -1;
    ok = idl_is_bytestring(kn, kn_len);
    JS_FreeCString(ctx, kn);
    if (!ok) {
        JS_ThrowTypeError(ctx, "a header name is not a ByteString");
        return -1;
    }
    return 0;
}

int headers_fill_run(JSContext *ctx, JSStepHdr *h, HeadersFill *f, JSValueConst init, HeaderList *out,
                     HeadersGuard guard, JSValue in, JSValue **out_cb, int *out_argc)
{
    int r;

    if (f->phase == FILL_START) {
        /* UNDEFINED IS "NOT GIVEN"; NULL IS NOT. `HeadersInit` is a union of two object types and Web IDL does
           not make it nullable, so `new Headers(null)` and `{headers: null}` are TypeErrors — while an absent
           optional argument, and an init object with no `headers` member, are simply no init. Treating the two
           alike accepted null silently, which wpt's headers-basic asserts against by name. */
        if (JS_IsUndefined(init)) { JS_FreeValue(ctx, in); return 0; }
        if (!JS_IsObject(init)) {
            JS_FreeValue(ctx, in);
            JS_ThrowTypeError(ctx, "a Headers init is not an object");
            return -1;
        }
        /* THERE IS NO "IT IS ALREADY A HEADERS" CASE. A shortcut copying the list stood here while the iterator
           was unbuilt, and it was WRONG in two ways the spec is explicit about: `new Headers(h)` resolves the
           `HeadersInit` union like any other object, so it takes the SEQUENCE arm through h's own @@iterator —
           which the page may have replaced (wpt installs a generator and expects its pairs, not h's list), and
           which COMBINES duplicate names the way iteration does. The route it dodged now exists, so it is gone
           rather than kept as the fast path for the common shape. */
        JS_FreeValue(ctx, in);   /* nothing here asked for it; the arm below starts its own request */
        in = JS_UNDEFINED;
        f->phase = FILL_ITER_ASKED;
    }
    /* WHICH ARM: Web IDL §3.2.25 step 11.2 picks `sequence<sequence<ByteString>>` over
       `record<ByteString, ByteString>` by `? GetMethod(V, %Symbol.iterator%)`, whose [[Get]] is an accessor or a
       Proxy trap away from being the page's code — so it is a request like every other read here. It was
       JS_IsArray, which is a DIFFERENT question: `new Headers(new Map(...))` is iterable and is not an array, so
       it took the record arm, found no own string keys, and produced an EMPTY header list — the request would
       have gone out missing exactly the headers the page set. Then it was the [[Get]] with `JS_IsFunction` on
       the result, which is a different question AGAIN: GetMethod makes a PRESENT non-callable a TypeError, so
       `new Headers({[Symbol.iterator]: 1})` walked the record arm — quietly, and to an empty list — where the
       standard throws. The three steps that decide that are ECMAScript's and are stated once, in idl_iter.c. */
    if (f->phase == FILL_ITER_ASKED) {
        JSValue itf;
        r = step_getprop_run(ctx, h, init, JS_WellKnownSymbolAtom(JS_WKS_ITERATOR), in, &itf, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return -1;
        in = JS_UNDEFINED;
        r = idl_get_method(ctx, itf, "a Headers init's @@iterator");
        JS_FreeValue(ctx, itf);
        if (r < 0) return -1;
        f->phase = r ? FILL_SEQ_PAIR : FILL_KEY_PAIR;
    }

    /* THE SEQUENCE ARM: `sequence<sequence<ByteString>>`. The outer cursor yields one PAIR per turn and the
       inner one yields that pair's items — Web IDL nests the protocol, so this nests the cursor rather than
       assuming the pair is an array. §5.1: a pair that does not hold exactly two items is a TypeError, which is
       why the inner runs one step PAST the second item rather than stopping at it. */
    while (f->phase == FILL_SEQ_PAIR || f->phase == FILL_SEQ_ITEM) {
        if (f->phase == FILL_SEQ_PAIR) {
            r = iter_cursor_run(ctx, h, &f->outer, init, in, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return -1;
            in = JS_UNDEFINED;
            if (f->outer.done) return 0;
            if (!JS_IsObject(f->outer.value)) {
                JS_ThrowTypeError(ctx, "a Headers init pair is not a sequence");
                return -1;
            }
            iter_cursor_release(ctx, &f->inner);   /* the previous pair's cursor still held its iterator */
            iter_cursor_init(&f->inner);
            JS_FreeValue(ctx, f->item[0]); JS_FreeValue(ctx, f->item[1]);
            f->item[0] = f->item[1] = JS_UNDEFINED;
            f->nitem = 0;
            f->phase = FILL_SEQ_ITEM;
        }
        for (;;) {
            r = iter_cursor_run(ctx, h, &f->inner, f->outer.value, in, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return -1;
            in = JS_UNDEFINED;
            if (f->inner.done)
                break;
            if (f->nitem >= 2) { f->nitem = 3; break; }   /* three or more: the same TypeError as one */
            f->item[f->nitem] = f->inner.value;
            f->inner.value = JS_UNDEFINED;
            f->nitem++;
        }
        if (f->nitem != 2) {
            JS_ThrowTypeError(ctx, "a Headers init pair does not contain exactly two items");
            return -1;
        }
        {
            size_t kn_len = 0, kv_len = 0;
            const char *kn = JS_ToCStringLen(ctx, &kn_len, f->item[0]);
            const char *kv = JS_ToCStringLen(ctx, &kv_len, f->item[1]);
            char *norm = NULL;
            int bad;
            if (!kn || !kv) {
                if (kn) JS_FreeCString(ctx, kn);
                if (kv) JS_FreeCString(ctx, kv);
                return -1;
            }
            bad = header_check(ctx, kn, kn_len, kv, kv_len, &norm) < 0;
            /* §5.1 fill appends THROUGH the append algorithm — every step of it, not just the guard: a
               Request built with {headers:{Host:"x"}} silently drops it, and a no-cors one drops every
               header that is not no-CORS-safelisted. */
            if (!bad) bad = headers_append_one(ctx, out, guard, kn, norm) < 0;
            free(norm);
            JS_FreeCString(ctx, kn);
            JS_FreeCString(ctx, kv);
            if (bad) return -1;   /* §5.1's guard already threw */
        }
        f->phase = FILL_SEQ_PAIR;
    }
    /* The RECORD arm is Web IDL §3.2.23's own cursor — [[OwnPropertyKeys]], then a descriptor and a [[Get]] per key,
       every one of them a request. It is shared with URLSearchParams because the conversion is Web IDL's and
       not this component's; what stays here is the per-pair half, which is where the ByteString key check, the
       §5.1 guard and the concolic shape live. */
    for (;;) {
        const char *kname, *kval;
        size_t kname_len = 0, kval_len = 0;

        if (f->phase == FILL_KEY_PAIR) {
            r = record_cursor_run(ctx, h, &f->rec, init, in, headers_record_key_ok, NULL, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return -1;
            in = JS_UNDEFINED;
            if (f->rec.done) return 0;
            JS_FreeValue(ctx, f->name);  f->name = JS_DupValue(ctx, f->rec.name);
            JS_FreeValue(ctx, f->value); f->value = JS_DupValue(ctx, f->rec.value);
            f->phase = FILL_VALUE_STR;
        }
        /* AN UNKNOWN VALUE KEEPS ITS SHAPE. A header built out of external input — `{'Authorization': 'Bearer '
           + token}` where the token is server-injected — is a CONCOLIC, and coercing it would either abort at
           the ToString boundary or, worse, quietly de-taint it into some concrete-looking string. Its shape is
           the display form the @H surface reports, and the `{hole}` in it is exactly what marks the header as a
           runtime value the reviewer has to supply. This is the same explicit projection fetch_park asks for on
           the URL, for the same reason. */
        DCHECK(f->phase == FILL_VALUE_STR, "the headers fill was re-entered at a phase it never parks in");
        /* AN UNKNOWN VALUE KEEPS ITS SHAPE. A header built out of external input — `{'Authorization': 'Bearer '
           + token}` where the token is server-injected — is a CONCOLIC, and coercing it would either abort at
           the ToString boundary or, worse, quietly de-taint it into some concrete-looking string. Its shape is
           the display form the @H surface reports, and the `{hole}` in it is exactly what marks the header as a
           runtime value the reviewer has to supply. This is the same explicit projection fetch_park asks for on
           the URL, for the same reason. */
        if (concolic_is(f->value)) {
            const char *sh = concolic_shape_c(f->value);
            JSValue sv = JS_NewString(ctx, sh ? sh : "{}");
            if (JS_IsException(sv)) return -1;
            JS_FreeValue(ctx, f->value);
            f->value = sv;
            JS_FreeValue(ctx, in);
            in = JS_UNDEFINED;
        } else {
            /* Otherwise it is ByteString, so ToString on it is the page's code AGAIN — a third request per key. */
            JSValue s;
            r = step_tostring_run(ctx, h, f->value, in, &s, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return -1;
            in = JS_UNDEFINED;
            JS_FreeValue(ctx, f->value);
            f->value = s;
        }
        kname = JS_ToCStringLen(ctx, &kname_len, f->name);
        kval = JS_ToCStringLen(ctx, &kval_len, f->value);
        if (!kname || !kval) {
            if (kname) JS_FreeCString(ctx, kname);
            if (kval) JS_FreeCString(ctx, kval);
            return -1;
        }
        {
            char *knorm = NULL;
            if (header_check(ctx, kname, kname_len, kval, kval_len, &knorm) < 0) {
                JS_FreeCString(ctx, kname); JS_FreeCString(ctx, kval);
                return -1;   /* §5.1's guard already threw */
            }
            /* §5.1: "Otherwise, object is a record, then for each key → value of object, APPEND (key, value)
               to headers." Web IDL §3.2.23's map semantics have ALREADY deduped the record — by its ByteString KEY,
               which is case-SENSITIVE — so the two entries of `{a:"1", A:"2"}` both arrive and both append,
               and `get("a")` combines them into "1, 2". A replace loop stood here matching on the LOWERCASED
               header name, which is a different equivalence than the record's and belongs to no step: it
               answered "2", and it is deleted rather than kept for the one shape it looked right on. */
            if (headers_append_one(ctx, out, guard, kname, knorm) < 0) {
                free(knorm);
                JS_FreeCString(ctx, kname); JS_FreeCString(ctx, kval);
                return -1;
            }
            free(knorm);
        }
        JS_FreeCString(ctx, kname);
        JS_FreeCString(ctx, kval);
        f->phase = FILL_KEY_PAIR;
    }
}

/* ---- the constructor ------------------------------------------------------------------------------------ */

/* WHERE THIS MACHINE RESTS. §5.1's constructor is two steps, and they split exactly where the page's code
   starts: step 1 is an own-slot write and step 2 is the whole fill, whose every [[Get]], @@iterator read and
   ToString is the page's. The `stage` byte this state carried named neither — and the comment beside it
   explained that hdr->stage was the ARGUMENT CURSOR, which stopped being true when idl_args.c handed the stage
   over to the body at IDL_STEP_FIRST. */
#define HDR_CTOR_STAGES(X) \
    X(HDR_CTOR_GUARD = IDL_STEP_FIRST, \
      "Fetch §5.1 new Headers(init) step 1 (this's guard is \"none\"; Web IDL §3.7.1's `new` requirement " \
      "precedes it)") \
    X(HDR_CTOR_FILL, "Fetch §5.1 new Headers(init) step 2 (fill this with init)")
enum { HDR_CTOR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const HDR_CTOR_STEPS[] = { HDR_CTOR_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct { HeadersFill fill; HeaderList list; JSValue result; } JSHeadersCtorState;

static void js_headers_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSHeadersCtorState *s = st;
    headers_fill_visit(ctx, &s->fill, v);
    v->val(ctx, &s->result);
}

/* THE HEADER LIST ALONE — a malloc'd array of malloc'd name/value pairs, which is not a reference and which no
   declaration names. Everything else this state holds is named by js_headers_ctor_visit and released through
   that one list. */
static void js_headers_ctor_release(JSContext *ctx, void *st)
{
    (void)ctx;
    header_list_free(&((JSHeadersCtorState *)st)->list);
}

static int js_headers_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSHeadersCtorState *s = st;
    int r;

    if (hdr->stage == HDR_CTOR_GUARD) {
        /* JS_CFUNC_step_ctor delivers NEW_TARGET in the receiver slot and undefined for a plain call, which is
           how `Headers()` is told apart from `new Headers()` — the IDL declares a constructor, so it throws. */
        if (JS_IsUndefined(hdr->this_val)) {
            JS_FreeValue(ctx, cb_result);
            JS_ThrowTypeError(ctx, "constructor Headers requires 'new'");
            return -1;
        }
        headers_fill_init(&s->fill);
        s->result = JS_UNDEFINED;
        hdr->stage = HDR_CTOR_FILL;
    }
    DCHECK(hdr->stage == HDR_CTOR_FILL,
           "the Headers constructor was re-entered at a stage §5.1 does not have");
    /* §5.1: `new Headers(init)` sets the guard to "none" and then fills — a page's own Headers refuses
       nothing, which is why the forbidden lists are unobservable until a Request or a Response owns one. */
    r = headers_fill_run(ctx, hdr, &s->fill, argc > 0 ? argv[0] : JS_UNDEFINED, &s->list,
                         HEADERS_GUARD_NONE, cb_result, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return -1;
    *presult = headers_new(ctx, &s->list, HEADERS_GUARD_NONE);
    return JS_IsException(*presult) ? -1 : 0;
}

static const IdlStepDecl js_headers_ctor_decl = {
    js_headers_ctor_step, sizeof(JSHeadersCtorState), js_headers_ctor_visit, js_headers_ctor_release,
    "Fetch §5.1 new Headers(init)", HDR_CTOR_STEPS
};

/* ---- install --------------------------------------------------------------------------------------------- */

void headers_init(JSContext *ctx)
{
    JSClassDef def = { "Headers", .finalizer = headers_finalizer };
    JSRuntime *rt = JS_GetRuntime(ctx);
    static const IdlArgType TWO_STR[2] = { IDL_DOMSTRING, IDL_DOMSTRING };
    static const IdlArgType ONE_ANY[1] = { IDL_ANY };   /* HeadersInit: a union the fill converts, not the machine */

    DCHECK(g_headers_rt == NULL || g_headers_rt == rt,
           "Headers was installed into a second runtime — its class id and step ids belong to the first, and one "
           "WASM instance is one document");
    if (g_headers_rt == rt)
        return;
    g_headers_rt = rt;
    JS_NewClassID(rt, &g_headers_class);
    JS_NewClass(rt, g_headers_class, &def);
    g_id[HDR_APPEND]       = idl_method_id(ctx, TWO_STR, 2, js_headers_member, HDR_APPEND);
    g_id[HDR_SET]          = idl_method_id(ctx, TWO_STR, 2, js_headers_member, HDR_SET);
    g_id[HDR_DELETE]       = idl_method_id(ctx, TWO_STR, 1, js_headers_member, HDR_DELETE);
    g_id[HDR_GET]          = idl_method_id(ctx, TWO_STR, 1, js_headers_member, HDR_GET);
    g_id[HDR_HAS]          = idl_method_id(ctx, TWO_STR, 1, js_headers_member, HDR_HAS);
    g_id[HDR_GETSETCOOKIE] = idl_method_id(ctx, TWO_STR, 0, js_headers_member, HDR_GETSETCOOKIE);
    g_ctor_stepid = idl_method_id_step(ctx, ONE_ANY, 1, NULL, 0, &js_headers_ctor_decl, 0);
    idl_optional_from(0);   /* §5.1: `constructor(optional HeadersInit init)` */

    /* §5.2's `iterable<ByteString, ByteString>` — the shared default iterator object over the two operations
       above, so the six members it defines exist once for every such interface rather than once per. */
    g_pair_handle = idl_pair_iter_declare(ctx, &HEADERS_PAIR_OPS);
    realm_declare_intrinsic(headers_install_proto);
}

/* §5.1's INTERFACE PROTOTYPE OBJECT, FOR ONE REALM. */
void headers_install_proto(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_headers_class != 0, "a realm asked for Headers.prototype before the class was declared");
    prev = JS_GetClassProto(ctx, g_headers_class);
    DCHECK(JS_IsNull(prev), "headers_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "Headers.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "Headers");
    idl_install_method(ctx, proto, "append", 2, g_id[HDR_APPEND]);
    idl_install_method(ctx, proto, "set", 2, g_id[HDR_SET]);
    idl_install_method(ctx, proto, "delete", 1, g_id[HDR_DELETE]);
    idl_install_method(ctx, proto, "get", 1, g_id[HDR_GET]);
    idl_install_method(ctx, proto, "has", 1, g_id[HDR_HAS]);
    idl_install_method(ctx, proto, "getSetCookie", 0, g_id[HDR_GETSETCOOKIE]);
    idl_pair_iter_install(ctx, proto, g_pair_handle);
    JS_SetClassProto(ctx, g_headers_class, proto);
}

/* The prototype and the interned name are this component's for the runtime's life, so they are released WITH
   it. Without this the prototype is a GC object nobody drops and JS_FreeRuntime's gc_obj_list walk reports it —
   which is exactly how it was found, on the first run of this file. */
void headers_free(JSContext *ctx)
{
    if (!g_headers_rt)
        return;
    /* the prototypes are the REALMS' — released with their contexts */
    g_headers_rt = NULL;
    g_ctor_stepid = -1;
}

void headers_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;

    DCHECK(g_ctor_stepid >= 0, "Headers was installed before headers_init declared its constructor");
    ctor = idl_step_constructor(ctx, "Headers", 1, g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the Headers interface object could not be allocated");
    {
        JSValue proto = JS_GetClassProto(ctx, g_headers_class);
        DCHECK(!JS_IsNull(proto), "Headers was installed into a realm that never ran its proto build");
        JS_SetConstructor(ctx, ctor, proto);
        JS_FreeValue(ctx, proto);
    }
    JS_SetPropertyStr(ctx, (JSValue)global, "Headers", ctor);
}
