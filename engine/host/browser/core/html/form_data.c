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
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/html/form_data.h"
#include "core/idl_args.h"
#include "core/idl_iter.h"

typedef struct { UrlEncodedList list; } FormDataObj;

static JSClassID g_fd_class;
static JSValue   g_fd_proto = JS_UNDEFINED;
static JSRuntime *g_fd_rt;
static int       g_fd_ctor_stepid = -1;
static int       g_fd_pair_handle = -1;

static void form_data_finalizer(JSRuntime *rt, JSValue val)
{
    FormDataObj *d = JS_GetOpaque(val, g_fd_class);
    (void)rt;
    if (d) { url_encoded_list_free(&d->list); free(d); }
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
        url_encoded_list_append(&d->list, entries->e[i].name, entries->e[i].nlen,
                                entries->e[i].value, entries->e[i].vlen);
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

int form_data_parse_multipart(const char *body, size_t len, const char *boundary, size_t blen,
                              UrlEncodedList *entries)
{
    /* `--` + boundary, which every delimiter starts with */
    char *delim;
    size_t dlen = blen + 2, pos;
    int ok = -1;

    if (!blen) return -1;
    delim = malloc(dlen + 1);
    CHECK(delim, "formdata: OOM parsing a multipart body");
    delim[0] = delim[1] = '-';
    memcpy(delim + 2, boundary, blen);
    delim[dlen] = 0;

    /* the body must OPEN with a delimiter */
    if (len < dlen || memcmp(body, delim, dlen)) goto done;
    pos = dlen;
    for (;;) {
        const char *hdr_end, *part, *next;
        const char *name = NULL, *filename = NULL;
        size_t name_len = 0, filename_len = 0, part_len;

        /* `--` here means the epilogue: this was the closing delimiter */
        if (pos + 2 <= len && body[pos] == '-' && body[pos + 1] == '-') { ok = 0; goto done; }
        if (pos + 2 > len || body[pos] != '\r' || body[pos + 1] != '\n') goto done;
        pos += 2;

        /* the part's headers end at the blank line */
        hdr_end = memmem(body + pos, len - pos, "\r\n\r\n", 4);
        if (!hdr_end) goto done;
        {
            /* §5.2 reads ONE of them: Content-Disposition, whose `name` is required. A part without it is a
               failure, not a nameless entry. */
            const char *h = body + pos;
            size_t hn = (size_t)(hdr_end - h), i;
            for (i = 0; i < hn; i++) {
                size_t line = i, end;
                const char *nl = memmem(h + i, hn - i, "\r\n", 2);
                end = nl ? (size_t)(nl - h) : hn;
                if (end - line >= 19 && !strncasecmp(h + line, "content-disposition", 19)) {
                    name = cd_param(h + line, end - line, "name", &name_len);
                    filename = cd_param(h + line, end - line, "filename", &filename_len);
                }
                if (!nl) break;
                i = end + 1;
            }
        }
        if (!name) goto done;
        if (filename)
            DFAIL("a multipart/form-data part carried a filename — build Blob and File, which is what a "
                  "filename part's entry value must be; a string there would be the wrong TYPE, not a "
                  "shorter one");
        part = hdr_end + 4;
        next = memmem(part, len - (size_t)(part - body), delim, dlen);
        /* the part's body ends at the CRLF BEFORE the delimiter, which belongs to the delimiter and not to it */
        if (!next || next < part + 2 || next[-1] != '\n' || next[-2] != '\r') goto done;
        part_len = (size_t)(next - part) - 2;
        url_encoded_list_append(entries, name, name_len, part, part_len);
        pos = (size_t)(next - body) + dlen;
    }
done:
    free(delim);
    if (ok < 0) url_encoded_list_free(entries);
    return ok;
}

/* ---- §5's members ---------------------------------------------------------------------------------------- */

enum { FD_APPEND = 0, FD_DELETE, FD_GET, FD_GETALL, FD_HAS, FD_SET };

static JSValue js_form_data_member(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                   int magic)
{
    FormDataObj *d = form_data_of(ctx, this_val);
    const char *name = NULL, *value = NULL;
    size_t nn = 0, vn = 0;
    JSValue r = JS_UNDEFINED;
    int i, w;

    if (!d) return JS_EXCEPTION;
    name = JS_ToCStringLen(ctx, &nn, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (!name) return JS_EXCEPTION;
    if (magic == FD_APPEND || magic == FD_SET) {
        value = JS_ToCStringLen(ctx, &vn, argc > 1 ? argv[1] : JS_UNDEFINED);
        if (!value) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
    }
#define FD_NAME_IS(i) (d->list.e[i].nlen == nn && !memcmp(d->list.e[i].name, name, nn))

    switch (magic) {
    case FD_APPEND:
        url_encoded_list_append(&d->list, name, nn, value, vn);
        break;
    case FD_DELETE:
        for (i = 0, w = 0; i < d->list.n; i++) {
            if (FD_NAME_IS(i)) { free(d->list.e[i].name); free(d->list.e[i].value); continue; }
            d->list.e[w++] = d->list.e[i];
        }
        d->list.n = w;
        break;
    case FD_GET:
        r = JS_NULL;   /* §5: absent is null, not "" */
        for (i = 0; i < d->list.n; i++)
            if (FD_NAME_IS(i)) { r = JS_NewStringLen(ctx, d->list.e[i].value, d->list.e[i].vlen); break; }
        break;
    case FD_GETALL: {
        uint32_t k = 0;
        r = JS_NewArray(ctx);
        if (JS_IsException(r)) break;
        for (i = 0; i < d->list.n; i++)
            if (FD_NAME_IS(i))
                JS_SetPropertyUint32(ctx, r, k++,
                                     JS_NewStringLen(ctx, d->list.e[i].value, d->list.e[i].vlen));
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
                    free(d->list.e[i].value);
                    d->list.e[i].value = url_encoded_strdup(value, vn);
                    d->list.e[i].vlen = vn;
                    d->list.e[w++] = d->list.e[i];
                } else {
                    free(d->list.e[i].name);
                    free(d->list.e[i].value);
                }
                continue;
            }
            d->list.e[w++] = d->list.e[i];
        }
        d->list.n = w;
        if (found < 0) url_encoded_list_append(&d->list, name, nn, value, vn);
        break;
    }
    }
#undef FD_NAME_IS
    JS_FreeCString(ctx, name);
    if (value) JS_FreeCString(ctx, value);
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
    *value = JS_NewStringLen(ctx, d->list.e[i].value, d->list.e[i].vlen);
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
    JSClassDef def = { "FormData", .finalizer = form_data_finalizer };
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

    /* The value argument is IDL_ANY because §5's overload picks its type: a Blob crosses as itself and
       everything else is a USVString. With no Blob interface nothing is one, and the member's own
       JS_ToCStringLen is that arm — declaring it USVString here would convert a Blob too, once there is one. */
    idl_install_method(ctx, g_fd_proto, "append", 2,
                       idl_method_id(ctx, TWO_STR, 3, js_form_data_member, FD_APPEND));
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
