/* THE FormData INTERFACE — XMLHttpRequest §4 Interface FormData, over an ENTRY LIST.
 *
 * THE NUMBER IS 4 AND EVERY CITATION IN THIS FILE USED TO SAY 5. XHR §5 is `Interface ProgressEvent` — which
 * core/platform.c cites correctly, one file over, for exactly that interface — so the old number resolved,
 * existed, and named the wrong interface at every site including the two step-machine STAGE LABELS a parked
 * flow prints across a resume. Nothing could have caught it: `engine/citegen.mjs` finds a number a standard
 * does not have and a TITLE that disagrees with one, and a bare `§5` supplies neither. Which is the whole of
 * why the TITLE is written beside the number here and at every anchoring site below — the number is the half
 * an instrument can check for you only once the title makes the claim falsifiable.
 *
 * WHY IT EXISTS HERE. `.formData()` on a Request or a Response returns one, and that is how a page reads a
 * form submission back — 70 of wpt's urlencoded-parser cases are exactly that call. It is also one of the
 * three arms of Fetch's `BodyInit` union, so a body built from a form has somewhere to come from.
 *
 * THE ENTRY LIST IS HTML §4.10.22.4 Constructing the entry list's, AND IT IS SHAPED LIKE THE URL STANDARD'S
 * urlencoded list. It is HTML's and not XHR's by the XHR standard's own words — XHR §4 says "This section used to
 * define entry, an entry's name and value, and the create an entry algorithm." and "These definitions have
 * been moved to the HTML Standard." — which is why the citation here was `§5.1`, a number XHR gives to
 * `Firing events using the ProgressEvent interface` and never gave to this. FormData's entries are
 * (name, value) pairs in insertion order with repeats kept — `getAll` reads the repeats back and `get`
 * answers with the first — which is the same list Headers and URLSearchParams are built on, so it is the
 * same one and not a third copy of it.
 *
 * A VALUE IS A STRING OR A FILE: XHR §4's `append` is overloaded on `(USVString value)` and
 * `(Blob blobValue, optional USVString filename)`, and Web IDL resolves the overload by asking whether the
 * argument is a platform object of the Blob interface — so the arm is chosen by a BRAND TEST on the argument
 * (`fd_entry_value` below), and HTML §4.10.22.4's create an entry then turns a Blob into a File. The
 * paragraph that stood here said this engine had no Blob and that every value therefore took the USVString
 * arm; core/file/blob.c has existed since, and this file has included it and read `blob_bytes_of` off
 * entries for as long. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/file/blob.h"
#include "core/html/form_data.h"
#include "core/html/form_entry_list.h"
#include "core/html/html_element.h"
#include "core/html/html_form.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/idl_iter.h"
#include "solver/concolic.h"

/* HTML §4.10.22.4 Constructing the entry list's ENTRY: a name, and a value that is EITHER a USVString OR a File. It was the URL Standard's urlencoded
   list, whose value is bytes, and that was right for exactly as long as nothing could be a File — the file
   comment said so, and `append`'s overload reached for the string arm on a Blob the moment Blob landed and
   stringified it out of C, which is the ToPrimitive the engine aborts on.
   THE VALUE IS A JSValue, so a File entry holds the File ITSELF: XHR §4's `get` must hand back the same object, and
   a copy of its bytes would answer a different one. That makes a FormData a GC ROOT for its entries, which is
   what `gc_mark` below is for — the entry can hold a File whose own properties reach back. */
typedef struct { char *name; size_t nlen; JSValue value; } FdEntry;
typedef struct { FdEntry *e; int n, cap; } FdList;
typedef struct { FdList list; } FormDataObj;

static void fd_list_free(JSRuntime *rt, FdList *l)
{
    int i;
    for (i = 0; i < l->n; i++) {
        free(l->e[i].name);
        JS_FreeValueRT(rt, l->e[i].value);
    }
    free(l->e);
    l->e = NULL;
    l->n = l->cap = 0;
}

/* Takes ownership of `value`. */
static void fd_list_append(FdList *l, const char *name, size_t nlen, JSValue value)
{
    if (l->n == l->cap) {
        FdEntry *g = realloc(l->e, sizeof *g * (size_t)(l->cap = l->cap ? l->cap * 2 : 8));
        CHECK(g, "formdata: OOM growing an entry list");
        l->e = g;
    }
    l->e[l->n].name = url_encoded_strdup(name, nlen);
    l->e[l->n].nlen = nlen;
    l->e[l->n].value = value;
    l->n++;
}

/* THE ENTRY'S VALUE AS BYTES, for the two serializers. A string entry is its UTF-8; a File entry is its
   CONTENTS, and its file NAME goes in the part's Content-Disposition rather than in its body. `*pfile` is the
   file's name when the entry is one, NULL otherwise. The returned pointer is borrowed from `ctx`'s string pool
   when `*pcstr` is set, which the caller releases. */
static const char *fd_entry_bytes(JSContext *ctx, const FdEntry *e, size_t *plen,
                                  const char **pfile, const char **pcstr)
{
    const char *bytes;
    *pcstr = NULL;
    *pfile = NULL;
    bytes = blob_bytes_of(e->value, plen, NULL);
    if (bytes) {
        *pfile = blob_file_name_of(e->value);
        return bytes;
    }
    /* AN UNKNOWN ENTRY CONTRIBUTES ITS DISPLAY SHAPE, which is the same answer endpoint_record gives a concolic
       URL and fb_value gives a concolic form entry: the request body then reads `q={location.hash}` and names
       where the value came from, rather than inventing one or converting at a boundary that cannot. Borrowed
       from the value like the Blob arm above, so `*pcstr` stays NULL and the caller releases nothing. */
    if (concolic_is(e->value)) {
        const char *shape = concolic_shape_c(e->value);
        DCHECK(shape != NULL, "a FormData entry's unknown value reached the serializer with no display shape — "
                              "every concolic is minted with one, so this part would carry no bytes at all and "
                              "the request body would lose the field");
        *plen = shape ? strlen(shape) : 0;
        return shape;
    }
    *pcstr = JS_ToCStringLen(ctx, plen, e->value);
    return *pcstr;
}

static JSClassID g_fd_class;
static JSRuntime *g_fd_rt;
static int       g_fd_ctor_stepid = -1;
static int       g_fd_pair_handle = -1;

static void form_data_finalizer(JSRuntime *rt, JSValue val)
{
    FormDataObj *d = JS_GetOpaque(val, g_fd_class);
    if (d) { fd_list_free(rt, &d->list); free(d); }
}

/* An entry's value may be a File, whose own properties can reach back to this FormData — a real cycle, which
   is what gc_mark is for. */
static void form_data_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    FormDataObj *d = JS_GetOpaque(val, g_fd_class);
    int i;
    if (!d) return;
    for (i = 0; i < d->list.n; i++)
        JS_MarkValue(rt, d->list.e[i].value, mark_func);
}

static FormDataObj *form_data_of(JSContext *ctx, JSValueConst v)
{
    FormDataObj *d = JS_GetOpaque(v, g_fd_class);
    if (!d) JS_ThrowTypeError(ctx, "not a FormData");
    return d;
}

JSValue form_data_new(JSContext *ctx, const UrlEncodedList *entries)
{
    FormDataObj *d;
    JSValue obj;
    int i;

    DCHECK(g_fd_class != 0, "a FormData was built before the class existed — form_data_init runs at install");
    {
        JSValue proto = JS_GetClassProto(ctx, g_fd_class);
        DCHECK(!JS_IsNull(proto), "a FormData was minted in a realm that never ran its install");
        obj = JS_NewObjectProtoClass(ctx, proto, g_fd_class);
        JS_FreeValue(ctx, proto);
    }
    if (JS_IsException(obj))
        return obj;
    d = calloc(1, sizeof *d);
    CHECK(d, "formdata: OOM building a FormData");
    for (i = 0; entries && i < entries->n; i++)
        fd_list_append(&d->list, entries->e[i].name, entries->e[i].nlen,
                       JS_NewStringLen(ctx, entries->e[i].value, entries->e[i].vlen));
    JS_SetOpaque(obj, d);
    return obj;
}

/* INFRA'S "CLONE" OF AN ENTRY LIST, as one more FormData. HTML §4.13.7.3's `setFormValue` stores "a clone of
   value's entry list" and not the FormData it was handed, and the difference is the whole point: a page that
   calls `setFormValue(fd)` and then appends to `fd` has not changed what its element submits. A SHALLOW clone
   of the list — the entries themselves are immutable (a name and a USVString-or-File), and a File entry must
   stay the SAME object because XHR §4's `get` answers with identity. */
JSValue form_data_clone(JSContext *ctx, JSValueConst src)
{
    FormDataObj *from = JS_GetOpaque(src, g_fd_class), *to;
    JSValue obj = form_data_new(ctx, NULL);
    int i;

    DCHECK(from != NULL, "an entry list was cloned from something that is not a FormData — the brand test is "
                         "the union's, and it runs before this");
    if (JS_IsException(obj)) return obj;
    to = JS_GetOpaque(obj, g_fd_class);
    for (i = 0; from && i < from->list.n; i++)
        fd_list_append(&to->list, from->list.e[i].name, from->list.e[i].nlen,
                       JS_DupValue(ctx, from->list.e[i].value));
    return obj;
}

/* ---- Fetch §5.3 "Body mixin"'s multipart/form-data parser -------------------------------------------------
 *
 * The parse is §5.3's `formData()` method steps, whose "multipart/form-data" arm says to "parse bytes, using
 * the value of the `boundary` parameter from mimeType, per the rules set forth in Returning Values from Forms:
 * multipart/form-data" — so the SHAPE is RFC 7578's and the OBLIGATION to run it is §5.3's. (§5.2 "BodyInit
 * unions" stood on this heading; the only multipart it names is the ENCODING algorithm the FormData arm of an
 * extract runs, which is this parser's inverse and lives in HTML, so the number named the opposite direction.)
 *
 * The shape is fixed by the spec and by RFC 7578: `--boundary CRLF` then the part's headers, then `CRLF CRLF`,
 * then its body, then `CRLF --boundary`, and the last part's boundary carries a trailing `--`. Every
 * departure from that is a FAILURE and not something to recover from — an empty body with a multipart type is
 * the case wpt checks, and it must reject rather than produce an empty FormData that would read as a form the
 * user submitted blank. */

/* One header line's value for `name`, case-insensitively, out of a Content-Disposition line. The value is
 * either a quoted string or a token; `*out_len` is its length. NULL when the parameter is absent. */
static const char *cd_param(const char *s, size_t n, const char *key, size_t *out_len)
{
    size_t klen = strlen(key), i;
    for (i = 0; i + klen + 1 <= n; i++) {
        if (strncasecmp(s + i, key, klen) || s[i + klen] != '=') continue;
        /* it must be preceded by a `;` or by the start, so `filename` does not match inside `xfilename` */
        if (i > 0) {
            size_t j = i;
            while (j > 0 && (s[j - 1] == ' ' || s[j - 1] == '\t')) j--;
            if (j == 0 || s[j - 1] != ';') continue;
        }
        {
            const char *v = s + i + klen + 1;
            size_t rest = n - (size_t)(v - s);
            if (rest && *v == '"') {
                const char *q = memchr(v + 1, '"', rest - 1);
                if (!q) return NULL;
                *out_len = (size_t)(q - (v + 1));
                return v + 1;
            }
            *out_len = strcspn(v, ";");
            if (*out_len > rest) *out_len = rest;
            return v;
        }
    }
    return NULL;
}

JSValue form_data_parse_multipart(JSContext *ctx, const char *body, size_t len,
                                  const char *boundary, size_t blen)
{
    /* `--` + boundary, which every delimiter starts with */
    char *delim;
    size_t dlen = blen + 2, pos;
    JSValue fd;
    FormDataObj *d;

    if (!blen) return JS_ThrowTypeError(ctx, "the multipart Content-Type names no boundary");
    fd = form_data_new(ctx, NULL);
    if (JS_IsException(fd)) return fd;
    d = JS_GetOpaque(fd, g_fd_class);
    delim = malloc(dlen + 1);
    CHECK(delim, "formdata: OOM parsing a multipart body");
    delim[0] = delim[1] = '-';
    memcpy(delim + 2, boundary, blen);
    delim[dlen] = 0;

    /* the body must OPEN with a delimiter */
    if (len < dlen || memcmp(body, delim, dlen)) goto fail;
    pos = dlen;
    for (;;) {
        const char *hdr_end, *part, *next;
        const char *name = NULL, *filename = NULL, *ctype = NULL;
        size_t name_len = 0, filename_len = 0, ctype_len = 0, part_len;

        /* `--` here means the epilogue: this was the closing delimiter */
        if (pos + 2 <= len && body[pos] == '-' && body[pos + 1] == '-') break;
        if (pos + 2 > len || body[pos] != '\r' || body[pos + 1] != '\n') goto fail;
        pos += 2;

        /* the part's headers end at the blank line */
        hdr_end = memmem(body + pos, len - pos, "\r\n\r\n", 4);
        if (!hdr_end) goto fail;
        {
            /* §5.2 reads TWO of them: Content-Disposition, whose `name` is required and whose `filename`
               makes the entry a File, and Content-Type, which is that File's type. A part without a
               Content-Disposition is a failure, not a nameless entry. */
            const char *h = body + pos;
            size_t hn = (size_t)(hdr_end - h), i;
            for (i = 0; i < hn; i++) {
                size_t line = i, end;
                const char *nl = memmem(h + i, hn - i, "\r\n", 2);
                end = nl ? (size_t)(nl - h) : hn;
                if (end - line >= 19 && !strncasecmp(h + line, "content-disposition", 19)) {
                    name = cd_param(h + line, end - line, "name", &name_len);
                    filename = cd_param(h + line, end - line, "filename", &filename_len);
                } else if (end - line >= 13 && !strncasecmp(h + line, "content-type:", 13)) {
                    ctype = h + line + 13;
                    while (ctype < h + end && (*ctype == ' ' || *ctype == '\t')) ctype++;
                    ctype_len = (size_t)(h + end - ctype);
                }
                if (!nl) break;
                i = end + 1;
            }
        }
        if (!name) goto fail;
        part = hdr_end + 4;
        next = memmem(part, len - (size_t)(part - body), delim, dlen);
        /* the part's body ends at the CRLF BEFORE the delimiter, which belongs to the delimiter and not to it */
        if (!next || next < part + 2 || next[-1] != '\n' || next[-2] != '\r') goto fail;
        part_len = (size_t)(next - part) - 2;
        if (filename) {
            /* §5.2: a part with a filename is a FILE entry — the value's TYPE is what distinguishes a form's
               file control from its text ones, and a string here would be the wrong type rather than a
               shorter one. Its lastModified is 0: the part carries no timestamp, and inventing `now` would
               claim a fact the wire did not. */
            JSValue f = file_new(ctx, part, part_len, ctype ? ctype : "", ctype_len,
                                 filename, filename_len, 0);
            if (JS_IsException(f)) goto fail;
            fd_list_append(&d->list, name, name_len, f);
        } else {
            fd_list_append(&d->list, name, name_len, JS_NewStringLen(ctx, part, part_len));
        }
        pos = (size_t)(next - body) + dlen;
    }
    free(delim);
    return fd;
fail:
    free(delim);
    JS_FreeValue(ctx, fd);
    return JS_ThrowTypeError(ctx, "the multipart/form-data body could not be parsed");
}

bool form_data_is(JSValueConst v)
{
    return g_fd_class != 0 && JS_GetOpaque(v, g_fd_class) != NULL;
}

JSClassID form_data_class_id(void)
{
    DCHECK(g_fd_class != 0, "a FormData-typed IDL position was declared before form_data_init made the class");
    return g_fd_class;
}

/* ---- the entry list, as HTML §4.10.22.4 builds it ---------------------------------------------------------- */

void form_data_append_entry(JSContext *ctx, JSValueConst fd, const char *name, size_t nlen, JSValue value)
{
    FormDataObj *d = JS_GetOpaque(fd, g_fd_class);

    (void)ctx;
    DCHECK(d != NULL, "an entry was appended to something that is not a FormData — §4.10.22.4 step 4 builds one");
    fd_list_append(&d->list, name, nlen, value);
}

void form_data_append_all(JSContext *ctx, JSValueConst dst, JSValueConst src)
{
    FormDataObj *to = JS_GetOpaque(dst, g_fd_class), *from = JS_GetOpaque(src, g_fd_class);
    int i;

    DCHECK(to != NULL && from != NULL,
           "HTML §4.13.7.3's entry construction was handed something that is not a FormData — the union's brand test "
           "runs before this");
    for (i = 0; i < from->list.n; i++)
        fd_list_append(&to->list, from->list.e[i].name, from->list.e[i].nlen,
                       JS_DupValue(ctx, from->list.e[i].value));
}

int form_data_entry_count(JSValueConst fd)
{
    FormDataObj *d = JS_GetOpaque(fd, g_fd_class);

    DCHECK(d != NULL, "an entry list was counted on something that is not a FormData");
    return d->list.n;
}

const char *form_data_entry_name(JSValueConst fd, int i, size_t *plen)
{
    FormDataObj *d = JS_GetOpaque(fd, g_fd_class);

    DCHECK(d != NULL && i >= 0 && i < d->list.n, "an entry was asked for past the end of the list");
    *plen = d->list.e[i].nlen;
    return d->list.e[i].name;
}

JSValueConst form_data_entry_value(JSValueConst fd, int i)
{
    FormDataObj *d = JS_GetOpaque(fd, g_fd_class);

    DCHECK(d != NULL && i >= 0 && i < d->list.n, "an entry was asked for past the end of the list");
    return d->list.e[i].value;
}

/* HTML §4.10.22.8's multipart/form-data encoding algorithm, STEP 1: "Replace every occurrence of U+000D (CR)
   not followed by U+000A (LF), and every occurrence of U+000A (LF) not preceded by U+000D (CR), in entry's
   name, by a string consisting of a U+000D (CR) and U+000A (LF). If entry's value is not a File object, then
   replace ... in entry's value" — so a lone CR and a lone LF each become a CRLF, and an existing CRLF is left
   alone. It is a step of the SERIALIZER and not of the entry list: `fd.get(name)` still answers with the
   string the page appended, which is why this builds a copy rather than rewriting the entry. Caller frees. */
static char *fd_normalize_newlines(const char *s, size_t n, size_t *out_n)
{
    char *out = malloc(n * 2 + 1);
    size_t i, w = 0;

    CHECK(out, "formdata: OOM normalizing a field's newlines");
    for (i = 0; i < n; i++) {
        if (s[i] == '\r') {
            out[w++] = '\r'; out[w++] = '\n';
            if (i + 1 < n && s[i + 1] == '\n') i++;   /* an existing CRLF is one newline, not two */
        } else if (s[i] == '\n') {
            out[w++] = '\r'; out[w++] = '\n';
        } else {
            out[w++] = s[i];
        }
    }
    out[w] = 0;
    *out_n = w;
    return out;
}

/* THE SAME ALGORITHM'S ESCAPE, which applies to FIELD NAMES and to FILENAMES FOR FILE FIELDS and to nothing
   else: "the result of the encoding in the previous bullet point must be escaped by replacing any 0x0A (LF)
   bytes with the byte sequence `%0A`, 0x0D (CR) with `%0D` and 0x22 (") with `%22`. The user agent must not
   perform any other escapes."
   IT IS NOT COSMETIC. The Content-Disposition line puts the name between DOUBLE QUOTES, so a name carrying a
   `"` closes the quoted-string early and a name carrying a CRLF ends the header line — `fd.append('a"\r\nX-
   Injected: 1', 'v')` wrote a part with a header the page never sent, and this engine's whole output is
   requests it constructs and sinks it decides are reachable. A forged header there is a finding that does not
   reproduce in any browser, because every browser performs this escape. Caller frees. */
static char *fd_escape_field(const char *s, size_t n, size_t *out_n)
{
    size_t nn = 0, i, w = 0;
    char *norm = fd_normalize_newlines(s, n, &nn);
    char *out = malloc(nn * 3 + 1);

    CHECK(out, "formdata: OOM escaping a multipart field name");
    for (i = 0; i < nn; i++) {
        switch (norm[i]) {
        case '\n': memcpy(out + w, "%0A", 3); w += 3; break;
        case '\r': memcpy(out + w, "%0D", 3); w += 3; break;
        case '"':  memcpy(out + w, "%22", 3); w += 3; break;
        default:   out[w++] = norm[i];
        }
    }
    out[w] = 0;
    free(norm);
    DCHECK(!memchr(out, '\r', w) && !memchr(out, '\n', w) && !memchr(out, '"', w),
           "a multipart field name or filename survived §4.10.22.8's escape still carrying a CR, an LF or a "
           "double quote — the Content-Disposition line writes it inside a quoted-string, so any of the three "
           "ends the header and turns the rest of the name into a header nobody sent");
    *out_n = w;
    return out;
}

/* HTML §4.10.22.8 "Multipart form data"'s `multipart/form-data` encoding algorithm — the SERIALIZER, the
 * parser above run backwards, and what a
 * `new Response(formData)` carries. RFC 7578's shape: `--boundary CRLF` then each part's Content-Disposition,
 * a blank line, its bytes, and a closing `--boundary--`. A FILE entry additionally carries `filename=` and its
 * own Content-Type, which is the whole reason a form's file control arrives as a file rather than as text.
 *
 * THE PARTS ARE PREPARED BEFORE THE BOUNDARY IS CHOSEN, because §4.10.22.8's escape changes the bytes the
 * boundary must not occur in: a name is scanned in the form it is WRITTEN in, never in the form it arrived in.
 *
 * THE BOUNDARY MUST NOT OCCUR IN ANY PART, or the receiver splits the body in the wrong place. It is chosen by
 * SCANNING the parts rather than by drawing a random number: this engine is deterministic on purpose — a
 * time-travel resume must produce the byte-identical body — and a random boundary would make the same flow
 * serialise differently on every run. A counter that stops at the first candidate no part contains is both
 * deterministic AND correct, which a random string only ever is with high probability. */
typedef struct {
    char       *name;     size_t nlen;    /* §4.10.22.8-escaped */
    char       *fname;    size_t fnlen;   /* §4.10.22.8-escaped, NULL when the entry is not a File */
    const char *bytes;    size_t vlen;    /* a File's contents, or `norm` for a string entry */
    char       *norm;                     /* the newline-normalized string value this part owns */
    const char *btype;                    /* a File's own MIME type */
} FdPart;

static void fd_parts_free(FdPart *p, int n)
{
    int i;
    for (i = 0; i < n; i++) { free(p[i].name); free(p[i].fname); free(p[i].norm); }
    free(p);
}

char *form_data_serialize_multipart(JSContext *ctx, JSValueConst fd, char *boundary, size_t *out_n)
{
    FormDataObj *d = JS_GetOpaque(fd, g_fd_class);
    const FdList *l;
    size_t cap = 256, n = 0;
    char *out;
    FdPart *parts;
    int i;
    unsigned attempt;

    DCHECK(d != NULL, "the multipart serializer was handed something that is not a FormData");
    l = &d->list;

    parts = calloc((size_t)(l->n > 0 ? l->n : 1), sizeof *parts);
    CHECK(parts, "formdata: OOM preparing a multipart body's parts");
    for (i = 0; i < l->n; i++) {
        const char *cstr = NULL, *fname = NULL, *bytes;
        size_t vlen = 0;

        bytes = fd_entry_bytes(ctx, &l->e[i], &vlen, &fname, &cstr);
        if (!bytes) { fd_parts_free(parts, i); return NULL; }
        parts[i].name = fd_escape_field(l->e[i].name, l->e[i].nlen, &parts[i].nlen);
        if (fname) {
            /* A FILE entry: its name is escaped, and step 1 leaves its VALUE alone — the bytes of a file are
               not a string and a CR inside them is data. */
            parts[i].fname = fd_escape_field(fname, strlen(fname), &parts[i].fnlen);
            parts[i].bytes = bytes;
            parts[i].vlen  = vlen;
            blob_bytes_of(l->e[i].value, NULL, &parts[i].btype);
            DCHECK(cstr == NULL, "a File entry's bytes came out of the string pool");
        } else {
            DCHECK(cstr != NULL || concolic_is(l->e[i].value),
                   "a part with no filename was built out of a Blob rather than out of a string — HTML "
                   "§4.10.22.4's create "
                   "an entry converts every Blob to a File, so an entry holding a nameless Blob never went "
                   "through it, and step 1's newline normalization would run over binary bytes. An UNKNOWN "
                   "entry is the third shape and is not from the pool: its bytes are its display shape, "
                   "borrowed from the value");
            parts[i].norm  = fd_normalize_newlines(bytes, vlen, &parts[i].vlen);
            parts[i].bytes = parts[i].norm;
            JS_FreeCString(ctx, cstr);
        }
    }

    for (attempt = 0; ; attempt++) {
        size_t blen;
        int clash = 0;
        snprintf(boundary, FORM_DATA_BOUNDARY_MAX, "----APIClientFormBoundary%u", attempt);
        blen = strlen(boundary);
        for (i = 0; i < l->n; i++) {
            if (memmem(parts[i].name, parts[i].nlen, boundary, blen) ||
                memmem(parts[i].bytes, parts[i].vlen, boundary, blen) ||
                (parts[i].fname && memmem(parts[i].fname, parts[i].fnlen, boundary, blen)))
                { clash = 1; break; }
        }
        if (!clash) break;
    }

    out = malloc(cap);
    CHECK(out, "formdata: OOM serialising a multipart body");
#define FD_PUT(p, len) do {                                                   \
        size_t need_ = (len);                                                 \
        while (n + need_ + 1 > cap) {                                         \
            char *g_ = realloc(out, cap *= 2);                                \
            CHECK(g_, "formdata: OOM growing a multipart body");              \
            out = g_;                                                         \
        }                                                                     \
        memcpy(out + n, (p), need_);                                          \
        n += need_;                                                           \
    } while (0)
#define FD_PUTS(str) FD_PUT((str), strlen(str))

    for (i = 0; i < l->n; i++) {
        FD_PUTS("--"); FD_PUTS(boundary); FD_PUTS("\r\n");
        FD_PUTS("Content-Disposition: form-data; name=\"");
        FD_PUT(parts[i].name, parts[i].nlen);
        FD_PUTS("\"");
        if (parts[i].fname) {
            FD_PUTS("; filename=\""); FD_PUT(parts[i].fname, parts[i].fnlen); FD_PUTS("\"");
            /* RFC 7578 §4.4 Content-Type Header Field for Each Part: "If the contents of a file are to be
               sent, the file data SHOULD be labeled with an appropriate media type, if known, or
               "application/octet-stream"." — which is the RFC and not XHR, because §4.10.22.8 delegates the
               whole serialization to it ("Return the byte sequence resulting from encoding the entry list
               using the rules described by RFC 7578"). It was cited as `§5.1`, a section of XHR that is
               about firing ProgressEvents. And the ARM is HTML's own: §4.10.22.8 says "The parts of the
               generated multipart/form-data resource that correspond to non-file fields must not have a
               `Content-Type` header specified", which is why this sits inside the filename arm. */
            FD_PUTS("\r\nContent-Type: ");
            FD_PUTS(parts[i].btype && *parts[i].btype ? parts[i].btype : "application/octet-stream");
        }
        FD_PUTS("\r\n\r\n");
        FD_PUT(parts[i].bytes, parts[i].vlen);
        FD_PUTS("\r\n");
    }
    FD_PUTS("--"); FD_PUTS(boundary); FD_PUTS("--\r\n");
#undef FD_PUTS
#undef FD_PUT
    fd_parts_free(parts, l->n);
    out[n] = 0;
    *out_n = n;
    return out;
}

/* ---- XHR §4's members ---------------------------------------------------------------------------------------- */

enum { FD_APPEND = 0, FD_DELETE, FD_GET, FD_GETALL, FD_HAS, FD_SET, FD_MEMBER_N };
/* THE AGENT'S POOL ENTRIES — the OBJECTS they are installed as are each realm's. */
static int g_fd_id[FD_MEMBER_N];

/* XHR §4's `append` and `set` are OVERLOADED on their second argument: `(USVString value)` and
 * `(Blob blobValue, optional USVString filename)`. Web IDL resolves the overload by asking whether the
 * argument is a platform object of the Blob interface — so this is a brand test, and the third argument
 * belongs to one arm only.
 *
 * THE BLOB ARM STORES A FILE, not the Blob. THE SECTION IS HTML’S AND NOT XHR’S, and `§4` stood here: XHR §4’s `append` steps say only
 * "Let entry be the result of creating an entry with name, value, and filename if given", and XHR §4’s own
 * prose says that algorithm has MOVED. The File rule is HTML §4.10.22.4 Constructing the entry list’s: its name
 * is the `filename` argument, or "blob" when none was given, and its type is the Blob's. A page that appends a
 * Blob and reads the entry back gets a File, which is what every form submission carries. */
static JSValue fd_entry_value(JSContext *ctx, JSValueConst v, JSValueConst filename, bool have_filename)
{
    size_t blen = 0, nlen = 0;
    const char *btype = NULL;
    const char *bytes = blob_bytes_of(v, &blen, &btype);
    const char *name;
    JSValue r;

    /* THE USVString ARM OVER UNKNOWN EXTERNAL INPUT IS THE UNKNOWN, and converting it here aborted the engine.
       §7.1.19 ToString step 10 sends an object to ToPrimitive, and a concolic carrier is one — the conversion
       boundary owes C a real string and cannot derive one, so `JS_ToString` on an unknown is the crash the
       axios caller fixture died on (`fd.append(k, cfg.value)` reached from its own toFormData). The other
       consumer of this list was already written for it: html_form.c's fb_value gives a concolic entry its
       display SHAPE at §4.10.22.4, so the producer converting it away is what disagreed. Keeping the unknown
       keeps §Solver's rule that opacity survives coercion — a later `fd.get(name)` still forks a branch, and
       the serializer below asks for the bytes at ITS own edge. */
    if (!bytes) {
        if (concolic_is(v))
            return JS_DupValue(ctx, v);
        return JS_ToString(ctx, v);   /* the USVString arm — already a string, the declaration converted it */
    }
    if (have_filename) {
        name = JS_ToCStringLen(ctx, &nlen, filename);
        if (!name) return JS_EXCEPTION;
    } else {
        /* HTML §4.10.22.4 Constructing the entry list: a Blob with no filename argument becomes a File
           named "blob" — and a value that is ALREADY a
           File keeps its own name, because the spec's create step is skipped for one. */
        const char *own = blob_file_name_of(v);
        name = own ? own : "blob";
        nlen = strlen(name);
        if (own) return JS_DupValue(ctx, v);
    }
    r = file_new(ctx, bytes, blen, btype ? btype : "", btype ? strlen(btype) : 0, name, nlen,
                 blob_last_modified_of(v));
    if (have_filename) JS_FreeCString(ctx, name);
    return r;
}

static JSValue js_form_data_member(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                   int magic)
{
    FormDataObj *d = form_data_of(ctx, this_val);
    const char *name = NULL;
    size_t nn = 0;
    JSValue value = JS_UNDEFINED, r = JS_UNDEFINED;
    int i, w;

    if (!d) return JS_EXCEPTION;
    name = JS_ToCStringLen(ctx, &nn, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (!name) return JS_EXCEPTION;
    if (magic == FD_APPEND || magic == FD_SET) {
        value = fd_entry_value(ctx, argc > 1 ? argv[1] : JS_UNDEFINED,
                               argc > 2 ? argv[2] : JS_UNDEFINED,
                               argc > 2 && !JS_IsUndefined(argv[2]));
        if (JS_IsException(value)) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
    }
#define FD_NAME_IS(i) (d->list.e[i].nlen == nn && !memcmp(d->list.e[i].name, name, nn))

    switch (magic) {
    case FD_APPEND:
        fd_list_append(&d->list, name, nn, value);
        value = JS_UNDEFINED;   /* the list owns it now */
        break;
    case FD_DELETE:
        for (i = 0, w = 0; i < d->list.n; i++) {
            if (FD_NAME_IS(i)) { free(d->list.e[i].name); JS_FreeValue(ctx, d->list.e[i].value); continue; }
            d->list.e[w++] = d->list.e[i];
        }
        d->list.n = w;
        break;
    case FD_GET:
        r = JS_NULL;   /* XHR §4: absent is null, not "" */
        for (i = 0; i < d->list.n; i++)
            if (FD_NAME_IS(i)) { r = JS_DupValue(ctx, d->list.e[i].value); break; }
        break;
    case FD_GETALL: {
        uint32_t k = 0;
        r = JS_NewArray(ctx);
        if (JS_IsException(r)) break;
        for (i = 0; i < d->list.n; i++)
            if (FD_NAME_IS(i))
                JS_SetPropertyUint32(ctx, r, k++, JS_DupValue(ctx, d->list.e[i].value));
        break;
    }
    case FD_HAS:
        r = JS_FALSE;
        for (i = 0; i < d->list.n; i++) if (FD_NAME_IS(i)) { r = JS_TRUE; break; }
        break;
    default: {
        /* XHR §4 set(): the FIRST entry with this name keeps its POSITION and takes the new value, and every other
           entry with that name is removed. Deleting then appending would move it to the end. */
        int found = -1;
        DCHECK(magic == FD_SET, "a FormData member was declared with a magic this component does not answer");
        for (i = 0, w = 0; i < d->list.n; i++) {
            if (FD_NAME_IS(i)) {
                if (found < 0) {
                    found = w;
                    JS_FreeValue(ctx, d->list.e[i].value);
                    d->list.e[i].value = value;
                    value = JS_UNDEFINED;
                    d->list.e[w++] = d->list.e[i];
                } else {
                    free(d->list.e[i].name);
                    JS_FreeValue(ctx, d->list.e[i].value);
                }
                continue;
            }
            d->list.e[w++] = d->list.e[i];
        }
        d->list.n = w;
        if (found < 0) { fd_list_append(&d->list, name, nn, value); value = JS_UNDEFINED; }
        break;
    }
    }
#undef FD_NAME_IS
    JS_FreeCString(ctx, name);
    JS_FreeValue(ctx, value);
    return r;
}

/* The two operations Web IDL §3.7.9 Iterable declarations's binding needs of an `iterable<K, V>` interface — the count and the i-th of
   §2.5.9's "value pairs to iterate over" — over the entry list as it stands right now. */
static int form_data_pair_count(JSContext *ctx, JSValueConst target)
{
    FormDataObj *d = JS_GetOpaque(target, g_fd_class);
    (void)ctx;
    return d ? d->list.n : -1;
}

static void form_data_pair_at(JSContext *ctx, JSValueConst target, int i, JSValue *key, JSValue *value)
{
    FormDataObj *d = JS_GetOpaque(target, g_fd_class);
    DCHECK(d != NULL && i < d->list.n, "a FormData entry was asked for past the end of the list");
    *key = JS_NewStringLen(ctx, d->list.e[i].name, d->list.e[i].nlen);
    *value = JS_DupValue(ctx, d->list.e[i].value);   /* a File entry yields the File, not a copy of its bytes */
}

static const IdlPairIterOps FD_PAIR_OPS = { form_data_pair_count, form_data_pair_at, "FormData" };

/* ---- XHR §4's constructor -------------------------------------------------------------------------------------
 *
 * `constructor(optional HTMLFormElement form, optional HTMLElement? submitter = null)`. Constructing FROM a
 * form runs HTML §4.10.22.4's "construct the entry list", and that algorithm FIRES A `formdata` EVENT at the
 * form — the page's own code, mid-construction, with a live handle on the list being built. So this
 * constructor is a machine that SUSPENDS, and `new FormData(form)` is one of the few constructors in the
 * platform that does. `new FormData()` with no argument is an empty entry list and never reaches step 1. */
/* WHERE THIS MACHINE RESTS. XHR §4's step 1 has two halves and the page's code sits between them, so they are
   two STAGES: the submitter's two refusals (both of which throw before anything is built), and the construction
   itself. The construction's own cursor rides the entry-list sub-sequence, which is why the second stage names
   the whole of XHR §4's steps 1.2-1.4 rather than one step of it. */
#define FD_CTOR_STAGES(X) \
    X(FD_CTOR_SUBMITTER, "XHR §4 Interface FormData, new FormData(form, submitter) step 1.1 (a submitter " \
                         "must be a submit button whose form owner is form)") \
    X(FD_CTOR_ENTRIES, "XHR §4 Interface FormData, new FormData(form, submitter) steps 1.2-1.4 (construct " \
                       "the entry list for form and submitter, refuse a null one, and adopt it)")
enum { IDL_STEP_STAGE_BASE(FD_CTOR_STAGES) FD_CTOR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const FD_CTOR_STEPS[] = { FD_CTOR_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct { FormEntryListRun entries; } JSFormDataCtorState;

static void js_fd_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSFormDataCtorState *s = st;
    form_entry_list_visit(ctx, &s->entries, v);
}

/* HTML §4.10.22.4 step 8's FLAG alone. Every value the run holds is named by js_fd_ctor_visit and released
   through that one declaration; the flag is not a reference, so nothing can name it. */
static void js_fd_ctor_release(JSContext *ctx, void *st)
{
    form_entry_list_unlock(ctx, &((JSFormDataCtorState *)st)->entries);
}

static int js_fd_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                           JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSFormDataCtorState *s = st;
    JSValueConst form = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst submitter = argc > 1 ? argv[1] : JS_UNDEFINED;
    int r;

    if (hdr->stage == FD_CTOR_SUBMITTER) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        if (JS_IsUndefined(hdr->this_val)) {
            JS_ThrowTypeError(ctx, "constructor FormData requires 'new'");
            return -1;
        }
        form_entry_list_init(&s->entries);
        /* `optional HTMLFormElement form` with no default: an omitted or explicitly-undefined argument is NOT
           GIVEN (Web IDL §3.6 Overload resolution algorithm), so this is what "if form is given" reads. Anything else that is not an
           HTMLFormElement is Web IDL's TypeError — `new FormData(null)` included, because the position is not
           nullable. */
        if (JS_IsUndefined(form)) {
            *presult = form_data_new(ctx, NULL);
            return JS_IsException(*presult) ? -1 : 0;
        }
        if (!html_form_is_form_element(form)) {
            JS_ThrowTypeError(ctx, "argument 1 of the FormData constructor is not an HTMLFormElement");
            return -1;
        }
        /* Step 1.1. `submitter` IS nullable, and null is what the IDL's own default is, so both spellings mean
           "no submitter". The two refusals are in the order XHR §4 lists them. */
        if (!JS_IsUndefined(submitter) && !JS_IsNull(submitter)) {
            if (!html_element_is(submitter)) {
                JS_ThrowTypeError(ctx, "the FormData submitter is not an HTMLElement");
                return -1;
            }
            if (!html_form_is_submit_button(ctx, submitter)) {
                JS_ThrowTypeError(ctx, "the FormData submitter is not a submit button");
                return -1;
            }
            {
                JSValue owner = html_form_owner_of(ctx, submitter);
                bool mine = JS_VALUE_GET_PTR(owner) == JS_VALUE_GET_PTR(form);
                JS_FreeValue(ctx, owner);
                if (!mine) {
                    JS_ThrowDOMException(ctx, "NotFoundError",
                                         "the FormData submitter's form owner is not the given form");
                    return -1;
                }
            }
        } else {
            /* §4.10.22.4's `submitter` defaults to null, and its step 5.1 compares fields against it — a null
               submitter therefore drops every button, which is what `new FormData(form)` must produce. */
            submitter = JS_NULL;
        }
        hdr->stage = FD_CTOR_ENTRIES;
    }
    DCHECK(hdr->stage == FD_CTOR_ENTRIES, "the FormData constructor resumed into a stage XHR §4 does not have");
    if (JS_IsUndefined(submitter)) submitter = JS_NULL;
    /* Step 1.2. The ENCODING is not given, so §4.10.22.4's own default applies — UTF-8. A form's
       `accept-charset` belongs to HTML §4.10.22.5 Selecting a form submission encoding's "pick an encoding",
       which is the SUBMISSION's step and not this constructor's; reading it here would put a `_charset_`
       entry in a list the spec builds without one. The number here was HTML §4.10.21.3, which is `The constraint
       validation API` — a real section that answers a different question, so it resolved and named nothing
       about encodings; core/html/form_entry_list.c still carries the same wrong number for a DIFFERENT
       algorithm ("submit a form", which is §4.10.22.3 Form submission algorithm). */
    r = form_entry_list_run(ctx, &s->entries, form, submitter, "UTF-8", cb_result, presult, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return -1;
    /* Step 1.3: a null list is a form already constructing one — re-entered from its own `formdata` handler. */
    if (JS_IsNull(*presult)) {
        *presult = JS_UNDEFINED;
        JS_ThrowDOMException(ctx, "InvalidStateError",
                             "the form is already constructing its entry list");
        return -1;
    }
    /* Step 1.4: "set this's entry list to list". The list IS a FormData — §4.10.22.4 step 9 returns a clone of
       the entry list and form_entry_list_run hands that clone over as the object holding it — so adopting it is
       returning it, and nothing copies a list twice. */
    return 0;
}

static const IdlStepDecl js_fd_ctor_decl = {
    js_fd_ctor_step, sizeof(JSFormDataCtorState), js_fd_ctor_visit, js_fd_ctor_release,
    "XHR §4 Interface FormData, new FormData(form, submitter)", FD_CTOR_STEPS
};

/* ---- install --------------------------------------------------------------------------------------------- */

void form_data_init(JSContext *ctx)
{
    JSClassDef def = { "FormData", .finalizer = form_data_finalizer, .gc_mark = form_data_gc_mark };
    JSRuntime *rt = JS_GetRuntime(ctx);
    static const IdlArgType TWO_STR[3] = { IDL_USVSTRING, IDL_ANY, IDL_USVSTRING };
    /* XHR §4's two arguments are `optional HTMLFormElement form` and `optional HTMLElement? submitter = null`.
       THIS COMMENT GAVE THREE REASONS THE DECLARATION SURFACE COULD EXPRESS NEITHER, AND TWO OF THEM ARE
       RETIRED — they are deleted rather than softened, because an impossibility claim is the one stale
       comment that tells the next reader not to look.
         - this file used to say "one brand and one narrowing per MEMBER, so two differently-narrowed
           positions cannot both be stated": that was true of idl_iface_brand alone. Web IDL §3.2.15 Interface types' `I` is stated PER POSITION now
           (idl_arg_iface), and core/events/mouse_event.c declares Window at position 3 and EventTarget at
           position 14 of ONE argument list — which is this member's shape exactly.
         - this file used to say "there is no nullable-interface type for the second at all":
           IDL_INTERFACE_NULLABLE is in
           IdlArgType, the positional conversion resolves Web IDL §3.2.20's null rule over it before any brand is
           read, and core/html/submit_event.c declares this very `HTMLElement? submitter` with it.
       THE THIRD REASON STILL HOLDS, AND IT RULES OUT EXACTLY ONE OF THE TWO ROUTES. idl_iface_brand takes a
       CLASS ID at declaration time and refuses a zero; core/platform.c declares `form_data` far above the
       `element` row that creates the node class (through element_init → node_init), so the brand + narrowing
       pair submit_event.c uses for the identical type is unreachable from here — and submit_event.c's own
       comment says why IT is exempt, which is that html_form_declare reaches it from under element_init.
       WHAT ACTUALLY BLOCKS THE OTHER ROUTE IS A FOURTH THING THIS COMMENT NEVER NAMED. idl_arg_iface reads
       no class id, so the ordering above says nothing about it — but its predicate is
       `bool (*)(JSContext *, JSValueConst)`, because both of its callers answer "implements" by walking THIS
       realm's prototype chain. `html_form_is_form_element` and `html_element_is` are `bool (*)(JSValueConst)`
       and correctly so: a namespace-and-tag test on a lexbor node needs no realm. The surface has a
       per-position form that demands a realm and a realm-free form (idl_iface_narrow) that is
       declaration-wide, and this member needs per-position AND realm-free at two positions naming two
       interfaces.
       NAMED RESIDUAL. WHAT IS NOT COVERED: both positions cross as IDL_ANY, so Web IDL §3.2.15's TypeError is thrown
       by the body below rather than by the type, and Web IDL §3.6 Overload resolution algorithm's `= null`
       for `submitter` is re-derived in the
       body instead of declared. WHAT THE NEXT DIFF BUILDS: core/html/html_form.h and core/html/html_element.h
       state their Web IDL §3.2.15 predicates in the shape a POSITION declares one with, this declaration becomes
       `{ IDL_INTERFACE, IDL_INTERFACE_NULLABLE }` with idl_arg_iface at 0 and 1 and
       idl_arg_default(1, IDL_DEFAULT_NULL, NULL), and the body's two brand tests and BOTH of its
       `submitter = JS_NULL` re-derivations go in that same diff — the second of those is not redundant with
       the first and deleting only one is a live bug, because the body is RE-ENTERED on every resume and
       re-derives `submitter` from argv each time, which is the whole reason a copy of it sits outside the
       stage-1 block. That diff also inherits an ORDER it does not have to think about today: Web IDL §3.7.1
       Interface object says "Calling that interface as a function will throw an exception", full stop, with
       no argument conversion between — and the `new` refusal below runs first today ONLY because nothing
       converts. A declared type converts first, so that refusal has to move ahead of the prologue in the
       same diff or `FormData({})` starts answering about argument 1. HOW ITS ABSENCE SHOWS: not in a page
       today — with both positions IDL_ANY nothing converts, so the body's refusals land exactly where a
       declared type's would. It shows in the TREE, and the way it shows is a third argument position added
       here: its brand is the one thing that does not follow from anything written down, because what these
       two accept is stated nowhere but in the `if`s below. It does NOT show as an unknown being refused: an
       argument position collapses IDL_INTERFACE_NULLABLE to IDL_INTERFACE ahead of the pass-through and
       idl_concolic_rule answers UNASKED for it, so a declared type would refuse a concolic here exactly as
       the body does — which is the opposite of what it does for submit_event.c's DICTIONARY member, and is a
       question about idl_args.c rather than about this file. */
    static const IdlArgType CTOR_ARGS[2] = { IDL_ANY, IDL_ANY };

    DCHECK(g_fd_rt == NULL || g_fd_rt == rt,
           "FormData was installed into a second runtime — its class id and step ids belong to the first, and "
           "one WASM instance is one document");
    if (g_fd_rt == rt)
        return;
    g_fd_rt = rt;
    JS_NewClassID(rt, &g_fd_class);
    JS_NewClass(rt, g_fd_class, &def);
    /* The value argument is IDL_ANY because XHR §4's overload picks its type: a Blob crosses as itself and
       everything else is a USVString. With no Blob interface nothing is one, and the member's own
       JS_ToCStringLen is that arm — declaring it USVString here would convert a Blob too, once there is one. */
    g_fd_id[FD_APPEND] = idl_method_id(ctx, TWO_STR, 3, js_form_data_member, FD_APPEND);
    idl_optional_from(2);   /* XHR §4: `append(name, blobValue, optional filename)` — two are required */
    g_fd_id[FD_DELETE] = idl_method_id(ctx, TWO_STR, 1, js_form_data_member, FD_DELETE);
    g_fd_id[FD_GET]    = idl_method_id(ctx, TWO_STR, 1, js_form_data_member, FD_GET);
    g_fd_id[FD_GETALL] = idl_method_id(ctx, TWO_STR, 1, js_form_data_member, FD_GETALL);
    g_fd_id[FD_HAS]    = idl_method_id(ctx, TWO_STR, 1, js_form_data_member, FD_HAS);
    g_fd_id[FD_SET]    = idl_method_id(ctx, TWO_STR, 3, js_form_data_member, FD_SET);
    idl_optional_from(2);   /* XHR §4: `set(name, blobValue, optional filename)` — two are required */

    g_fd_pair_handle = idl_pair_iter_declare(ctx, &FD_PAIR_OPS);

    g_fd_ctor_stepid = idl_method_id_step(ctx, CTOR_ARGS, 2, NULL, 0, &js_fd_ctor_decl, 0);
    idl_optional_from(0);   /* XHR §4: both constructor arguments are optional */
    realm_declare_intrinsic(form_data_install_realm);
}

/* XHR §4 "Interface FormData", FOR ONE REALM — its Web IDL §3.7.3 interface prototype object, its §3.7.1
   interface object, and Web IDL §3.8's property reference for its name.

   THE INTERFACE OBJECT IS HERE BECAUSE WEB IDL §3.8 IS GIVEN A REALM. `define the global property references`
   is "To define the global property references on target, given realm realm", and its step 1 is "Let
   interfaces be a list that contains every interface that is exposed in realm" — the population is a REALM's
   and the algorithm names no Document. XHR §4 declares `[Exposed=(Window,Worker)]`, so a realm whose global
   object implements a worker scope owes the name; while it was placed from core/platform.c's per-DOCUMENT
   column, which such a realm never reaches, it got nothing, and nor did a Window realm until a Document was
   installed over it. The prototype is in hand here, so the separate per-document entry's JS_GetClassProto
   re-read is gone: re-reading it would be a second answer to a question this function has just settled.

   THE COMPONENT IS IN core/html/ AND THE INTERFACE IS THE XMLHttpRequest STANDARD'S, which is why this one
   was left standing when every other XHR §3/§5 placement moved: a conversion partitioned by DIRECTORY does
   not see it from either side. The exposure set is what decides the column, and it is stated in `xhr.idl` and
   generated into browser/idl_exposure.h — never read off which directory the file happens to sit in. */
void form_data_install_realm(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_fd_class != 0, "a realm asked for FormData.prototype before the class was declared");
    prev = JS_GetClassProto(ctx, g_fd_class);
    DCHECK(JS_IsNull(prev), "form_data_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "FormData.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "FormData");
    idl_install_method(ctx, proto, "append", g_fd_id[FD_APPEND]);
    idl_install_method(ctx, proto, "delete", g_fd_id[FD_DELETE]);
    idl_install_method(ctx, proto, "get", g_fd_id[FD_GET]);
    idl_install_method(ctx, proto, "getAll", g_fd_id[FD_GETALL]);
    idl_install_method(ctx, proto, "has", g_fd_id[FD_HAS]);
    idl_install_method(ctx, proto, "set", g_fd_id[FD_SET]);
    idl_pair_iter_install(ctx, proto, g_fd_pair_handle);

    /* WEB IDL §3.7.1 Interface object's INTERFACE OBJECT AND WEB IDL §3.8's STEP 3.1.3 FOR ITS NAME. XHR §4
       declares no [LegacyWindowAlias], so Web IDL §3.8 step 3.1.4 has nothing to do for this interface, and no
       [LegacyFactoryFunction], so neither does Web IDL §3.8 step 3.2. */
    {
        JSValue ctor, global;

        DCHECK(g_fd_ctor_stepid >= 0, "FormData was installed before form_data_init declared its constructor");
        ctor = idl_step_constructor(ctx, "FormData", g_fd_ctor_stepid);
        CHECK(!JS_IsException(ctor), "the FormData interface object could not be allocated");
        JS_SetConstructor(ctx, ctor, proto);
        global = JS_GetGlobalObject(ctx);
        idl_define_global_property_reference(ctx, global, "FormData", ctor);
        JS_FreeValue(ctx, global);
    }

    JS_SetClassProto(ctx, g_fd_class, proto);   /* the realm owns it from here */
}

void form_data_free(JSContext *ctx)
{
    if (!g_fd_rt)
        return;
    /* the prototypes are the REALMS' — released with their contexts */
    g_fd_rt = NULL;
    g_fd_ctor_stepid = -1;
}
