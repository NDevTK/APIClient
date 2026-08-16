/* PARSING AND SERIALIZING A MIME TYPE — WHATWG MIME Sniffing §4.4 and §4.5.
 *
 * WHY IT EXISTS AS A COMPONENT. There was half of one, private to xml_http_request.c: a `mime_essence` that
 * cut the string at the first ';' and a `mime_charset` that walked for a `charset=` — with a comment naming
 * its own limit ("a component that needs the whole record must not reach for these"). A comment is not a
 * mechanism: the limit it named was reached by the very next algorithm that needed a MIME type, and a limit
 * that is not enforced is a bug waiting for its second caller. What the two of them approximated is one
 * specified algorithm producing one RECORD, and the record is what both of this file's callers need — Fetch
 * §6's data: URL processor returns one, and XHR §3.6.6 reads its essence AND its charset off one.
 *
 * THE APPROXIMATION WAS ALSO WRONG, in the ways a splitter always is. `text/plain;charset=gbk, text/html` is
 * two header values that "get a header" joined, and Fetch §2.2.3 says the answer is text/html; the splitter
 * answered type "text", subtype "plain" and charset "gbk, text/html". `text/html;charset="shift_jis"` is a
 * quoted-string whose closing quote the splitter kept as part of the label. `text/html;charset=` is a
 * parameter §4.4 drops and the splitter reported as the empty encoding label. None of those are edge cases
 * dressed up: they are what a real Content-Type header looks like when more than one hop wrote it.
 *
 * THE GRAMMAR IS THE OTHER STANDARD'S. §4.4 is written on Fetch §2.2's HTTP whitespace and its "collect an
 * HTTP quoted string", and on §3's token / quoted-string token code points, so those are here as named
 * static helpers rather than inlined as character tests — the difference between "HTTP whitespace" (which
 * includes LF and CR, and does NOT include FF) and "ASCII whitespace" is exactly one of the things a
 * hand-rolled version gets wrong. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/encoding/encoding.h"   /* §4.2's get an encoding, which §3.5 step 3 runs on the charset */
#include "core/mime/mime_type.h"

/* ---- Fetch §2.2 and MIME Sniffing §3: the code point classes the algorithm is written on ---------------- */

/* Fetch §2.2: "HTTP whitespace is U+000A LF, U+000D CR, or an HTTP tab or space" — and the standard's own
   note says this deliberately EXCLUDES U+000C FF, which ASCII whitespace includes. */
static bool mime_http_ws(unsigned char c)
{
    return c == 0x09 || c == 0x0A || c == 0x0D || c == 0x20;
}

/* §3: "An HTTP token code point is U+0021 (!), U+0023 (#), U+0024 ($), U+0025 (%), U+0026 (&), U+0027 ('),
   U+002A (*), U+002B (+), U+002D (-), U+002E (.), U+005E (^), U+005F (_), U+0060 (`), U+007C (|),
   U+007E (~), or an ASCII alphanumeric." */
static bool mime_token_cp(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '!' || c == '#' || c == '$' || c == '%' || c == '&' || c == '\'' || c == '*' ||
           c == '+' || c == '-' || c == '.' || c == '^' || c == '_' || c == '`' || c == '|' || c == '~';
}

static bool mime_solely_token(const char *s, size_t n)
{
    size_t i;
    if (!n) return false;
    for (i = 0; i < n; i++)
        if (!mime_token_cp((unsigned char)s[i])) return false;
    return true;
}

/* §3: "An HTTP quoted-string token code point is U+0009 TAB, a code point in the range U+0020 SPACE to
   U+007E (~), inclusive, or a code point in the range U+0080 through U+00FF, inclusive."
   IT IS A CODE POINT TEST, NOT A BYTE TEST, and this engine's strings are UTF-8 — so U+0100 arrives as two
   bytes that are both individually in 0x80..0xFF and is NOT a quoted-string token code point. The sequence is
   decoded rather than scanned, which is the whole difference between `charset=ÿ` (allowed) and
   `charset=Ā` (a parameter §4.4 drops). A malformed sequence is no code point at all and fails. */
static bool mime_solely_qstring(const char *s, size_t n)
{
    size_t i = 0;

    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        size_t need;

        if (c < 0x80) {
            if (c != 0x09 && (c < 0x20 || c > 0x7E)) return false;
            i++;
            continue;
        }
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; need = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; need = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; need = 3; }
        else return false;
        if (i + need >= n) return false;   /* a truncated sequence is no code point at all */
        for (size_t k = 1; k <= need; k++) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (cp < 0x80 || cp > 0xFF) return false;
        i += need + 1;
    }
    return true;
}

/* ---- a growable byte string, for the two collectors that build one ------------------------------------- */

typedef struct { char *p; size_t n, cap; } MStr;

static void mstr_add(MStr *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 32;
        char *g;
        while (cap < b->n + n + 1) cap *= 2;
        g = realloc(b->p, cap);
        CHECK(g != NULL, "MIME: OOM building a parameter value");
        b->p = g;
        b->cap = cap;
    }
    if (n) memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static char *mime_strdup_lower(const char *s, size_t n, bool lower)
{
    char *out = malloc(n + 1);
    size_t i;

    CHECK(out != NULL, "MIME: OOM taking a MIME type's component");
    for (i = 0; i < n; i++)
        out[i] = (lower && s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] - 'A' + 'a') : s[i];
    out[n] = 0;
    return out;
}

/* Fetch §2.2 "collect an HTTP quoted string", with extract-value TRUE (§4.4 step 11.8.1's caller) or FALSE
   (§2.2's "get, decode, and split", which keeps the quotes so the value it splits still parses). `*pp` is the
   position variable and is left one past the closing quote — or at the end for an unterminated string, which
   is NOT a failure: §2.2 breaks out of its loop and returns what it collected. */
static char *mime_collect_quoted(const char *s, size_t end, size_t *pp, bool extract_value, size_t *out_n)
{
    MStr b = { 0 };
    size_t p = *pp, start = p;

    DCHECK(p < end && s[p] == '"',
           "collect an HTTP quoted string was entered at a position that is not a U+0022 — its first step is "
           "an assert, and a caller that reaches it otherwise has mis-tracked its position variable");
    p++;
    for (;;) {
        size_t run = p;
        while (p < end && s[p] != '"' && s[p] != '\\') p++;
        mstr_add(&b, s + run, p - run);
        if (p >= end) break;
        if (s[p] == '\\') {
            p++;
            if (p >= end) { mstr_add(&b, "\\", 1); break; }
            mstr_add(&b, s + p, 1);
            p++;
        } else {
            p++;                     /* the closing U+0022 */
            break;
        }
    }
    *pp = p;
    if (!extract_value) {
        free(b.p);
        b.p = mime_strdup_lower(s + start, p - start, false);
        b.n = p - start;
    } else if (!b.p) {
        b.p = mime_strdup_lower("", 0, false);
        b.n = 0;
    }
    if (out_n) *out_n = b.n;
    return b.p;
}

/* ---- §4.1's record ------------------------------------------------------------------------------------- */

void mime_type_init(MimeType *m)
{
    m->type = m->subtype = NULL;
    m->params = NULL;
    m->nparams = m->cparams = 0;
}

void mime_type_free(MimeType *m)
{
    int i;

    free(m->type);
    free(m->subtype);
    for (i = 0; i < m->nparams; i++) {
        free(m->params[i].name);
        free(m->params[i].value);
    }
    free(m->params);
    mime_type_init(m);
}

const char *mime_type_parameter(const MimeType *m, const char *name)
{
    int i;

    for (i = 0; i < m->nparams; i++)
        if (!strcmp(m->params[i].name, name)) return m->params[i].value;
    return NULL;
}

/* §4.1's map is ORDERED and its keys are unique, so a set is an append and never a re-order: §4.4 step 11.10
   only sets a key that "does not exist", and §2.2.3's charset carry-over only sets one that does not either. */
static void mime_param_append(MimeType *m, char *name, char *value)
{
    DCHECK(mime_type_parameter(m, name) == NULL,
           "a MIME type parameter was appended over one the record already has — §4.1's parameters is a map, "
           "and both of its writers test for existence first");
    if (m->nparams == m->cparams) {
        int cap = m->cparams ? m->cparams * 2 : 4;
        MimeParam *g = realloc(m->params, sizeof(*g) * (size_t)cap);
        CHECK(g != NULL, "MIME: OOM appending a MIME type parameter");
        m->params = g;
        m->cparams = cap;
    }
    m->params[m->nparams].name = name;
    m->params[m->nparams].value = value;
    m->nparams++;
}

/* ---- §4.4 "parse a MIME type" -------------------------------------------------------------------------- */

bool mime_type_parse(MimeType *out, const char *input, size_t len)
{
    size_t p, e, ts, te, ss, se;

    mime_type_init(out);
    if (!input) return false;
    /* Step 1: remove leading and trailing HTTP whitespace. */
    p = 0;
    e = len;
    while (p < e && mime_http_ws((unsigned char)input[p])) p++;
    while (e > p && mime_http_ws((unsigned char)input[e - 1])) e--;
    /* Step 3: type is everything up to the first U+002F. */
    ts = p;
    while (p < e && input[p] != '/') p++;
    te = p;
    if (!mime_solely_token(input + ts, te - ts)) return false;   /* step 4 (empty is not solely, either) */
    if (p >= e) return false;                                    /* step 5: no U+002F at all */
    p++;                                                         /* step 6 */
    /* Step 7: subtype is everything up to the first U+003B. */
    ss = p;
    while (p < e && input[p] != ';') p++;
    se = p;
    while (se > ss && mime_http_ws((unsigned char)input[se - 1])) se--;   /* step 8 */
    if (!mime_solely_token(input + ss, se - ss)) return false;            /* step 9 */
    /* Step 10: the record, both halves in ASCII lowercase. */
    out->type = mime_strdup_lower(input + ts, te - ts, true);
    out->subtype = mime_strdup_lower(input + ss, se - ss, true);
    /* Step 11: the parameters. */
    while (p < e) {
        size_t ns, ne, vn = 0;
        char *value;

        p++;                                                             /* 11.1: past the U+003B */
        while (p < e && mime_http_ws((unsigned char)input[p])) p++;       /* 11.2 */
        ns = p;                                                          /* 11.3 */
        while (p < e && input[p] != ';' && input[p] != '=') p++;
        ne = p;
        if (p < e) {                                                     /* 11.5 */
            if (input[p] == ';') continue;                               /* 11.5.1: a name with no value */
            p++;                                                         /* 11.5.2: past the U+003D */
        }
        if (p >= e) break;                                               /* 11.6 */
        if (input[p] == '"') {                                           /* 11.8 */
            value = mime_collect_quoted(input, e, &p, /*extract_value*/ true, &vn);
            /* 11.8.2: whatever follows the closing quote up to the next U+003B is DISCARDED — the standard's
               own example is that `charset="shift_jis"iso-2022-jp` yields charset=shift_jis. */
            while (p < e && input[p] != ';') p++;
        } else {                                                         /* 11.9 */
            size_t vs = p, ve;
            while (p < e && input[p] != ';') p++;
            ve = p;
            while (ve > vs && mime_http_ws((unsigned char)input[ve - 1])) ve--;
            if (ve == vs) continue;                                      /* 11.9.3: an empty value is dropped */
            value = mime_strdup_lower(input + vs, ve - vs, false);
            vn = ve - vs;
        }
        /* 11.10: four conditions, and a parameter that fails any of them is simply not recorded. */
        if (ne > ns && mime_solely_token(input + ns, ne - ns) && mime_solely_qstring(value, vn)) {
            char *name = mime_strdup_lower(input + ns, ne - ns, true);   /* 11.4 */
            if (!mime_type_parameter(out, name)) {
                mime_param_append(out, name, value);
                continue;
            }
            free(name);
        }
        free(value);
    }
    return true;
}

/* ---- §4.2's essence and §4.5's serialization ------------------------------------------------------------ */

char *mime_type_essence(const MimeType *m)
{
    size_t tn, sn;
    char *out;

    DCHECK(m->type != NULL && m->subtype != NULL,
           "the essence of a MIME type record that was never parsed was asked for — §4.1 says both halves are "
           "non-empty ASCII strings, and a failed parse leaves neither");
    tn = strlen(m->type);
    sn = strlen(m->subtype);
    out = malloc(tn + sn + 2);
    CHECK(out != NULL, "MIME: OOM serializing a MIME type's essence");
    memcpy(out, m->type, tn);
    out[tn] = '/';
    memcpy(out + tn + 1, m->subtype, sn);
    out[tn + sn + 1] = 0;
    return out;
}

char *mime_type_serialize(const MimeType *m)
{
    MStr b = { 0 };
    char *ess = mime_type_essence(m);
    int i;

    mstr_add(&b, ess, strlen(ess));
    free(ess);
    for (i = 0; i < m->nparams; i++) {
        const char *v = m->params[i].value;
        size_t vn = strlen(v), k;

        mstr_add(&b, ";", 1);
        mstr_add(&b, m->params[i].name, strlen(m->params[i].name));
        mstr_add(&b, "=", 1);
        /* §4.5: a value that is empty, or that is not solely HTTP token code points, is QUOTED — and each
           U+0022 and U+005C inside it is preceded by a U+005C, which is what makes serialize-then-parse give
           the record back unchanged. */
        if (!vn || !mime_solely_token(v, vn)) {
            mstr_add(&b, "\"", 1);
            for (k = 0; k < vn; k++) {
                if (v[k] == '"' || v[k] == '\\') mstr_add(&b, "\\", 1);
                mstr_add(&b, v + k, 1);
            }
            mstr_add(&b, "\"", 1);
        } else {
            mstr_add(&b, v, vn);
        }
    }
    return b.p;
}

/* ---- §4.6's two groups this engine branches on ---------------------------------------------------------- */

bool mime_type_is_html(const MimeType *m)
{
    return m->type && m->subtype && !strcmp(m->type, "text") && !strcmp(m->subtype, "html");
}

bool mime_type_is_xml(const MimeType *m)
{
    size_t n;

    if (!m->type || !m->subtype) return false;
    n = strlen(m->subtype);
    if (n > 4 && !strcmp(m->subtype + n - 4, "+xml")) return true;
    return !strcmp(m->subtype, "xml") && (!strcmp(m->type, "text") || !strcmp(m->type, "application"));
}

/* ---- Fetch §2.2.3 "extract a MIME type" ----------------------------------------------------------------- */

bool mime_type_extract(MimeType *out, const char *value)
{
    size_t end, p = 0;
    char *charset = NULL, *essence = NULL;
    bool have = false;

    mime_type_init(out);
    if (!value) return false;       /* "If values is null, then return failure" */
    end = strlen(value);
    /* Fetch §2.2's "get, decode, and split a header value", performed as the loop that consumes it: each turn
       produces one value and the extraction's own step runs on it, so no list is materialised. A comma inside
       a QUOTED STRING does not split — which is why this cannot be a strtok on ','. */
    for (;;) {
        MStr tv = { 0 };
        size_t vs, ve;
        MimeType tmp;
        char *ess;

        for (;;) {
            size_t run = p;
            while (p < end && value[p] != '"' && value[p] != ',') p++;
            mstr_add(&tv, value + run, p - run);
            if (p < end && value[p] == '"') {
                size_t qn = 0;
                char *q = mime_collect_quoted(value, end, &p, /*extract_value*/ false, &qn);
                mstr_add(&tv, q, qn);
                free(q);
                if (p < end) continue;
            }
            break;
        }
        /* "Remove all HTTP tab or space from the start and end" — TAB or SPACE, not HTTP whitespace. */
        vs = 0;
        ve = tv.n;
        while (vs < ve && (tv.p[vs] == 0x09 || tv.p[vs] == 0x20)) vs++;
        while (ve > vs && (tv.p[ve - 1] == 0x09 || tv.p[ve - 1] == 0x20)) ve--;
        if (mime_type_parse(&tmp, tv.p ? tv.p + vs : "", ve - vs)) {
            ess = mime_type_essence(&tmp);
            /* "If temporaryMimeType is failure or its essence is the wildcard, then continue." */
            if (strcmp(ess, "*/*") != 0) {
                if (!essence || strcmp(ess, essence)) {
                    const char *cs = mime_type_parameter(&tmp, "charset");
                    free(charset);
                    charset = cs ? mime_strdup_lower(cs, strlen(cs), false) : NULL;
                    free(essence);
                    essence = ess;
                    ess = NULL;
                } else if (!mime_type_parameter(&tmp, "charset") && charset) {
                    mime_param_append(&tmp, mime_strdup_lower("charset", 7, false),
                                      mime_strdup_lower(charset, strlen(charset), false));
                }
                mime_type_free(out);
                *out = tmp;
                mime_type_init(&tmp);
                have = true;
            }
            free(ess);
        }
        mime_type_free(&tmp);
        free(tv.p);
        if (p >= end) break;
        DCHECK(value[p] == ',',
               "the header-value split stopped at a code point that is not a U+002C — §2.2 asserts exactly "
               "this before it advances, and reaching it otherwise means the quoted-string collector left the "
               "position somewhere the collector before it had already passed");
        p++;
    }
    free(charset);
    free(essence);
    return have;
}

/* ---- Fetch §3.5 "legacy extract an encoding" ------------------------------------------------------------ */

/* The four steps, in order, each with its own return. Every one of them answers `fallbackEncoding`, which is
   why the algorithm is "legacy": the header gets to narrow the answer and never to widen it into a failure. */
int mime_type_legacy_extract_encoding(const MimeType *m, int fallback_encoding)
{
    const char *charset;
    int tentative;

    /* "If mimeType is failure, then return fallbackEncoding." */
    if (!m) return fallback_encoding;
    /* "If mimeType["charset"] does not exist, then return fallbackEncoding." — the record's own ordered map,
       and `mime_type_parameter` answers NULL for exactly the standard's "does not exist". A charset present and
       EMPTY is not this case: §4.4 drops a parameter with no value, so it never reaches the record. */
    charset = mime_type_parameter(m, "charset");
    if (!charset) return fallback_encoding;
    /* "Let tentativeEncoding be the result of getting an encoding from mimeType["charset"]. If
       tentativeEncoding is failure, then return fallbackEncoding. Return tentativeEncoding." A label this
       registry does not know is the standard's failure, and the fallback stands. */
    tentative = encoding_lookup(charset, strlen(charset));
    return tentative < 0 ? fallback_encoding : tentative;
}
