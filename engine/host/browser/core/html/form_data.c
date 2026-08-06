/* THE FormData INTERFACE — XMLHttpRequest §5, over an ENTRY LIST.
 *
 * WHY IT EXISTS HERE. `.formData()` on a Request or a Response returns one, and that is how a page reads a
 * form submission back — 70 of wpt's urlencoded-parser cases are exactly that call. It is also one of the
 * three arms of Fetch's `BodyInit` union, so a body built from a form has somewhere to come from.
 *
 * THE ENTRY LIST IS §5.1's, WHICH IS THE URL STANDARD'S urlencoded list. FormData's entries are (name, value)
 * pairs in insertion order with repeats kept — `getAll` reads the repeats back and `get` answers with the
 * first — which is the same list Headers and URLSearchParams are built on, so it is the same one and not a
 * third copy of it.
 *
 * A VALUE IS A STRING HERE, and that is the union's own answer rather than a simplification: §5's `append` is
 * overloaded on `(USVString value)` and `(Blob blobValue, optional USVString filename)`, and Web IDL resolves
 * the overload by asking whether the argument is a platform object of the Blob interface. This engine has no
 * Blob, so nothing can be one, and every value takes the USVString arm — the same reasoning BodyInit's union
 * follows. When Blob lands it is one brand test in this file and a File in the entry, not a redesign. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/file/blob.h"
#include "core/html/form_data.h"
#include "core/idl_args.h"
#include "core/idl_iter.h"

/* §5's ENTRY: a name, and a value that is EITHER a USVString OR a File. It was the URL Standard's urlencoded
   list, whose value is bytes, and that was right for exactly as long as nothing could be a File — the file
   comment said so, and `append`'s overload reached for the string arm on a Blob the moment Blob landed and
   stringified it out of C, which is the ToPrimitive the engine aborts on.
   THE VALUE IS A JSValue, so a File entry holds the File ITSELF: §5's `get` must hand back the same object, and
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
    *pcstr = JS_ToCStringLen(ctx, plen, e->value);
    return *pcstr;
}

static JSClassID g_fd_class;
static JSValue   g_fd_proto = JS_UNDEFINED;
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
    obj = JS_NewObjectProtoClass(ctx, g_fd_proto, g_fd_class);
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

/* ---- Fetch §5.2's multipart/form-data parser --------------------------------------------------------------
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

/* Fetch §5.1's `multipart/form-data` SERIALIZER — the parser above, run backwards, and what a
 * `new Response(formData)` carries. RFC 7578's shape: `--boundary CRLF` then each part's Content-Disposition,
 * a blank line, its bytes, and a closing `--boundary--`. A FILE entry additionally carries `filename=` and its
 * own Content-Type, which is the whole reason a form's file control arrives as a file rather than as text.
 *
 * THE BOUNDARY MUST NOT OCCUR IN ANY PART, or the receiver splits the body in the wrong place. It is chosen by
 * SCANNING the parts rather than by drawing a random number: this engine is deterministic on purpose — a
 * time-travel resume must produce the byte-identical body — and a random boundary would make the same flow
 * serialise differently on every run. A counter that stops at the first candidate no part contains is both
 * deterministic AND correct, which a random string only ever is with high probability. */
char *form_data_serialize_multipart(JSContext *ctx, JSValueConst fd, char *boundary, size_t *out_n)
{
    FormDataObj *d = JS_GetOpaque(fd, g_fd_class);
    const FdList *l;
    size_t cap = 256, n = 0;
    char *out;
    int i;
    unsigned attempt;

    DCHECK(d != NULL, "the multipart serializer was handed something that is not a FormData");
    l = &d->list;

    for (attempt = 0; ; attempt++) {
        int clash = 0;
        snprintf(boundary, FORM_DATA_BOUNDARY_MAX, "----APIClientFormBoundary%u", attempt);
        for (i = 0; i < l->n; i++) {
            const char *cstr = NULL, *fname = NULL, *bytes;
            size_t vlen = 0;
            if (memmem(l->e[i].name, l->e[i].nlen, boundary, strlen(boundary))) { clash = 1; break; }
            bytes = fd_entry_bytes(ctx, &l->e[i], &vlen, &fname, &cstr);
            if (!bytes) return NULL;
            if (memmem(bytes, vlen, boundary, strlen(boundary)) ||
                (fname && strstr(fname, boundary)))
                clash = 1;
            if (cstr) JS_FreeCString(ctx, cstr);
            if (clash) break;
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
        const char *cstr = NULL, *fname = NULL, *btype = NULL, *bytes;
        size_t vlen = 0;

        bytes = fd_entry_bytes(ctx, &l->e[i], &vlen, &fname, &cstr);
        if (!bytes) { free(out); return NULL; }
        FD_PUTS("--"); FD_PUTS(boundary); FD_PUTS("\r\n");
        FD_PUTS("Content-Disposition: form-data; name=\"");
        FD_PUT(l->e[i].name, l->e[i].nlen);
        FD_PUTS("\"");
        if (fname) {
            FD_PUTS("; filename=\""); FD_PUTS(fname); FD_PUTS("\"");
            blob_bytes_of(l->e[i].value, NULL, &btype);
            /* §5.1: a file part carries a Content-Type, and a File with no type of its own is
               application/octet-stream — the type a receiver must assume for arbitrary bytes. */
            FD_PUTS("\r\nContent-Type: ");
            FD_PUTS(btype && *btype ? btype : "application/octet-stream");
        }
        FD_PUTS("\r\n\r\n");
        FD_PUT(bytes, vlen);
        FD_PUTS("\r\n");
        if (cstr) JS_FreeCString(ctx, cstr);
    }
    FD_PUTS("--"); FD_PUTS(boundary); FD_PUTS("--\r\n");
#undef FD_PUTS
#undef FD_PUT
    out[n] = 0;
    *out_n = n;
    return out;
}

/* ---- §5's members ---------------------------------------------------------------------------------------- */

enum { FD_APPEND = 0, FD_DELETE, FD_GET, FD_GETALL, FD_HAS, FD_SET };

/* §5's `append` and `set` are OVERLOADED on their second argument: `(USVString value)` and
 * `(Blob blobValue, optional USVString filename)`. Web IDL resolves the overload by asking whether the
 * argument is a platform object of the Blob interface — so this is a brand test, and the third argument
 * belongs to one arm only.
 *
 * THE BLOB ARM STORES A FILE, not the Blob. §5 says to "create a new File object" from a Blob value: its name
 * is the `filename` argument, or "blob" when none was given, and its type is the Blob's. A page that appends a
 * Blob and reads the entry back gets a File, which is what every form submission carries. */
static JSValue fd_entry_value(JSContext *ctx, JSValueConst v, JSValueConst filename, bool have_filename)
{
    size_t blen = 0, nlen = 0;
    const char *btype = NULL;
    const char *bytes = blob_bytes_of(v, &blen, &btype);
    const char *name;
    JSValue r;

    if (!bytes)
        return JS_ToString(ctx, v);   /* the USVString arm — already a string, the declaration converted it */
    if (have_filename) {
        name = JS_ToCStringLen(ctx, &nlen, filename);
        if (!name) return JS_EXCEPTION;
    } else {
        /* §5: a Blob with no filename argument becomes a File named "blob" — and a value that is ALREADY a
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
        r = JS_NULL;   /* §5: absent is null, not "" */
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
        /* §5 set(): the FIRST entry with this name keeps its POSITION and takes the new value, and every other
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

/* §3.7.10's two operations, over the entry list as it stands right now. */
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

/* ---- §5's constructor -------------------------------------------------------------------------------------
 *
 * `constructor(optional HTMLFormElement form, optional HTMLElement? submitter = null)`. Constructing FROM a
 * form is the form's own "constructing the entry list" algorithm, which needs a form element to walk; this
 * engine's DOM has one, but the entry list it produces is the SUBMISSION algorithm rather than anything this
 * file owns, so a `form` argument reaches a DFAIL naming it rather than an empty FormData that would look like
 * a form with no controls. `new FormData()` with no argument is an empty entry list and is the whole of what
 * `.formData()` and the wpt corpus construct. */
typedef struct { uint8_t stage; } JSFormDataCtorState;

static void js_fd_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }
static void js_fd_ctor_release(JSContext *ctx, void *st) { (void)ctx; (void)st; }

static int js_fd_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                           JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    (void)st; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    if (JS_IsUndefined(hdr->this_val)) {
        JS_ThrowTypeError(ctx, "constructor FormData requires 'new'");
        return -1;
    }
    if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0]))
        DFAIL("new FormData(form) reached its constructor — build the form-submission entry list algorithm, "
              "which is what turns a form element's controls into entries");
    *presult = form_data_new(ctx, NULL);
    return JS_IsException(*presult) ? -1 : 0;
}

static const IdlStepDecl js_fd_ctor_decl = {
    js_fd_ctor_step, sizeof(JSFormDataCtorState), js_fd_ctor_visit, js_fd_ctor_release
};

/* ---- install --------------------------------------------------------------------------------------------- */

void form_data_init(JSContext *ctx)
{
    JSClassDef def = { "FormData", .finalizer = form_data_finalizer, .gc_mark = form_data_gc_mark };
    JSRuntime *rt = JS_GetRuntime(ctx);
    static const IdlArgType TWO_STR[3] = { IDL_USVSTRING, IDL_ANY, IDL_USVSTRING };
    static const IdlArgType CTOR_ARGS[2] = { IDL_ANY, IDL_ANY };

    DCHECK(g_fd_rt == NULL || g_fd_rt == rt,
           "FormData was installed into a second runtime — its class id and step ids belong to the first, and "
           "one WASM instance is one document");
    if (g_fd_rt == rt)
        return;
    g_fd_rt = rt;
    JS_NewClassID(rt, &g_fd_class);
    JS_NewClass(rt, g_fd_class, &def);
    g_fd_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_fd_proto), "FormData.prototype could not be allocated");
    idl_interface_tag(ctx, g_fd_proto, "FormData");

    /* The value argument is IDL_ANY because §5's overload picks its type: a Blob crosses as itself and
       everything else is a USVString. With no Blob interface nothing is one, and the member's own
       JS_ToCStringLen is that arm — declaring it USVString here would convert a Blob too, once there is one. */
    idl_install_method(ctx, g_fd_proto, "append", 2,
                       idl_method_id(ctx, TWO_STR, 3, js_form_data_member, FD_APPEND));
    idl_optional_from(2);   /* §5: `append(name, blobValue, optional filename)` — two are required */
    idl_install_method(ctx, g_fd_proto, "delete", 1,
                       idl_method_id(ctx, TWO_STR, 1, js_form_data_member, FD_DELETE));
    idl_install_method(ctx, g_fd_proto, "get", 1,
                       idl_method_id(ctx, TWO_STR, 1, js_form_data_member, FD_GET));
    idl_install_method(ctx, g_fd_proto, "getAll", 1,
                       idl_method_id(ctx, TWO_STR, 1, js_form_data_member, FD_GETALL));
    idl_install_method(ctx, g_fd_proto, "has", 1,
                       idl_method_id(ctx, TWO_STR, 1, js_form_data_member, FD_HAS));
    idl_install_method(ctx, g_fd_proto, "set", 2,
                       idl_method_id(ctx, TWO_STR, 3, js_form_data_member, FD_SET));
    idl_optional_from(2);   /* §5: `set(name, blobValue, optional filename)` — two are required */

    g_fd_pair_handle = idl_pair_iter_declare(ctx, &FD_PAIR_OPS);
    idl_pair_iter_install(ctx, g_fd_proto, g_fd_pair_handle);

    g_fd_ctor_stepid = idl_method_id_step(ctx, CTOR_ARGS, 2, NULL, 0, &js_fd_ctor_decl, 0);
    idl_optional_from(0);   /* §5: both constructor arguments are optional */
}

void form_data_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;
    DCHECK(g_fd_ctor_stepid >= 0, "FormData was installed before form_data_init declared its constructor");
    ctor = idl_step_constructor(ctx, "FormData", 0, g_fd_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the FormData interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, g_fd_proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "FormData", ctor);
}

void form_data_free(JSContext *ctx)
{
    if (!g_fd_rt)
        return;
    JS_FreeValue(ctx, g_fd_proto);
    g_fd_proto = JS_UNDEFINED;
    idl_pair_iter_free(ctx, g_fd_pair_handle);
    g_fd_rt = NULL;
    g_fd_ctor_stepid = -1;
}
