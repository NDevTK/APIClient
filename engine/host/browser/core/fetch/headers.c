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

static JSClassID g_headers_class;
static JSValue   g_proto = JS_UNDEFINED;
static int       g_ctor_stepid = -1;
static JSRuntime *g_headers_rt;
static JSAtom    g_atom_length;

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

static void headers_finalizer(JSRuntime *rt, JSValue val)
{
    HeaderList *l = JS_GetOpaque(val, g_headers_class);
    (void)rt;
    if (l) { header_list_free(l); free(l); }
}

const HeaderList *headers_list_of(JSValueConst v) { return JS_GetOpaque(v, g_headers_class); }

static HeaderList *headers_of(JSContext *ctx, JSValueConst v)
{
    HeaderList *l = JS_GetOpaque(v, g_headers_class);
    if (!l) JS_ThrowTypeError(ctx, "not a Headers");
    return l;
}

JSValue headers_new(JSContext *ctx, const HeaderList *src)
{
    HeaderList *l;
    JSValue obj;
    int i;

    DCHECK(g_headers_class != 0, "a Headers was built before the class existed — headers_init runs at install");
    obj = JS_NewObjectProtoClass(ctx, g_proto, g_headers_class);
    if (JS_IsException(obj))
        return obj;
    l = calloc(1, sizeof *l);
    CHECK(l, "headers: OOM building a Headers");
    for (i = 0; src && i < src->n; i++)
        header_list_append(l, src->e[i].name, src->e[i].value);
    JS_SetOpaque(obj, l);
    return obj;
}

/* §5.2's members. Every argument is a ByteString, which the IDL machine has already made a real string by the
   time a body runs — so what is left here is the BYTE range, which ToString does not enforce and which is the
   whole of what makes a ByteString different from a DOMString. */
static int header_is_bytestring(const char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s > 0x7f)   /* a UTF-8 lead byte: the code unit was > 255, or is not one byte */
            return 0;
    return 1;
}

enum { HDR_APPEND = 0, HDR_SET, HDR_DELETE, HDR_GET, HDR_HAS, HDR_GETSETCOOKIE };

static JSValue js_headers_member(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    HeaderList *l = headers_of(ctx, this_val);
    const char *name = NULL, *value = NULL;
    JSValue r = JS_UNDEFINED;

    if (!l)
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
    name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    if (magic == HDR_APPEND || magic == HDR_SET) {
        value = JS_ToCString(ctx, argv[1]);
        if (!value) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
    }
    if (!header_is_bytestring(name) || (value && !header_is_bytestring(value))) {
        JS_FreeCString(ctx, name);
        if (value) JS_FreeCString(ctx, value);
        return JS_ThrowTypeError(ctx, "a header name or value is not a ByteString");
    }
    switch (magic) {
    case HDR_APPEND: header_list_append(l, name, value); break;
    case HDR_SET:    header_list_set(l, name, value); break;
    case HDR_DELETE: header_list_delete(l, name); break;
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
    return r;
}

/* ---- the fill (HeadersInit -> a header list) ------------------------------------------------------------ */

enum { FILL_START = 0, FILL_ITER_ASKED, FILL_KEYS_ASKED, FILL_VALUE_ASKED, FILL_NAME_STR, FILL_VALUE_STR };

void headers_fill_init(HeadersFill *f)
{
    memset(f, 0, sizeof *f);
    f->keys = f->name = f->value = JS_UNDEFINED;
}

void headers_fill_visit(JSContext *ctx, HeadersFill *f, JSStepVisit *v)
{
    v->val(ctx, &f->keys);
    v->val(ctx, &f->name);
    v->val(ctx, &f->value);
}

void headers_fill_release(JSContext *ctx, HeadersFill *f)
{
    JS_FreeValue(ctx, f->keys);
    JS_FreeValue(ctx, f->name);
    JS_FreeValue(ctx, f->value);
    f->keys = f->name = f->value = JS_UNDEFINED;
}

int headers_fill_run(JSContext *ctx, JSStepHdr *h, HeadersFill *f, JSValueConst init, HeaderList *out,
                     JSValue in, JSValue **out_cb, int *out_argc)
{
    const HeaderList *src;
    int r;

    if (f->phase == FILL_START) {
        if (JS_IsUndefined(init) || JS_IsNull(init)) { JS_FreeValue(ctx, in); return 0; }
        if (!JS_IsObject(init)) {
            JS_FreeValue(ctx, in);
            JS_ThrowTypeError(ctx, "a Headers init is not an object");
            return -1;
        }
        /* A Headers ALREADY: §5.1 fill copies its list. Web IDL would reach the same pairs by ITERATING it —
           Headers declares `iterable<ByteString, ByteString>` — so this is the same answer by a shorter route,
           and it is the route that works today because that iterator is one of the members still to build. */
        src = headers_list_of(init);
        if (src) {
            int i;
            JS_FreeValue(ctx, in);
            for (i = 0; i < src->n; i++)
                header_list_append(out, src->e[i].name, src->e[i].value);
            return 0;
        }
        f->phase = FILL_ITER_ASKED;
        in = JS_UNDEFINED;
    }
    /* WHICH ARM: Web IDL picks `sequence<sequence<ByteString>>` over `record<ByteString, ByteString>` by whether
       the init is ITERABLE, and that is a [[Get]] of @@iterator — an accessor or a Proxy trap away from being
       the page's code, so it is a request like every other read here. It was JS_IsArray, which is a DIFFERENT
       question: `new Headers(new Map(...))` is iterable and is not an array, so it took the record arm, found no
       own string keys, and produced an EMPTY header list — the request would have gone out missing exactly the
       headers the page set. */
    if (f->phase == FILL_ITER_ASKED) {
        JSValue itf;
        r = step_getprop_run(ctx, h, init, JS_WellKnownSymbolAtom(JS_WKS_ITERATOR), in, &itf, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return -1;
        in = JS_UNDEFINED;
        /* THE SEQUENCE ARM IS NOT BUILT YET. Its steps are the iterator protocol — call @@iterator, then `next`
           until `done`, then convert each pair — and every one of those is a request that now exists, so this
           is a capability to write and not a hole in the surface. It aborts naming itself rather than answering
           with the record arm's empty list, which is the stub this engine does not write. */
        DCHECK(!JS_IsFunction(ctx, itf),
               "a Headers init is ITERABLE, so Web IDL converts it as sequence<sequence<ByteString>> — build "
               "that arm of the fill (@@iterator is read, its result is called, and `next` is driven to `done`, "
               "all of them requests the host step surface exports)");
        JS_FreeValue(ctx, itf);
        f->phase = FILL_KEYS_ASKED;
    }
    /* The RECORD arm: [[OwnPropertyKeys]], then a [[Get]] per key, both requests. The phase is the same on the
       way in and while parked, which is what the request's own cursor is for — it asks on the first call and
       answers on the second. */
    if (f->phase == FILL_KEYS_ASKED) {
        r = step_ownkeys_run(ctx, h, init, in, &f->keys, out_cb, out_argc);
        if (r > 0) { f->phase = FILL_KEYS_ASKED; return r; }
        if (r < 0) return -1;
        in = JS_UNDEFINED;
        /* The key list is the ARRAY the engine built from the validated keys, so reading its length reaches
           nothing of the page's — the one read in this algorithm that is not a request. */
        {
            JSValue len = JS_GetProperty(ctx, f->keys, g_atom_length);
            int32_t n = 0;
            if (JS_IsException(len) || JS_ToInt32(ctx, &n, len) < 0) { JS_FreeValue(ctx, len); return -1; }
            JS_FreeValue(ctx, len);
            f->n = n;
        }
        f->phase = FILL_NAME_STR;
    }

    for (;;) {
        JSValue key;
        const char *kname, *kval;

        if (f->phase == FILL_NAME_STR) {
            if (f->i >= f->n) { JS_FreeValue(ctx, in); return 0; }
            key = JS_GetPropertyUint32(ctx, f->keys, (uint32_t)f->i);
            if (JS_IsException(key)) { JS_FreeValue(ctx, in); return -1; }
            /* A SYMBOL key is not a record key: Web IDL's record<> conversion takes the string ones. */
            if (!JS_IsString(key)) { JS_FreeValue(ctx, key); f->i++; continue; }
            JS_FreeValue(ctx, f->name);
            f->name = key;
            f->phase = FILL_VALUE_ASKED;
            in = JS_UNDEFINED;
        }
        if (f->phase == FILL_VALUE_ASKED) {
            JSAtom a = JS_ValueToAtom(ctx, f->name);
            if (a == JS_ATOM_NULL) { JS_FreeValue(ctx, in); return -1; }
            r = step_getprop_run(ctx, h, init, a, in, &f->value, out_cb, out_argc);
            JS_FreeAtom(ctx, a);
            if (r > 0) return r;            /* parked ON THIS KEY; the resume comes back to it */
            if (r < 0) return -1;
            in = JS_UNDEFINED;
            f->phase = FILL_VALUE_STR;
        }
        DCHECK(f->phase == FILL_VALUE_STR, "the headers fill was re-entered at a phase it never parks in");
        /* The value is ByteString, so ToString on it is the page's code AGAIN — a third request per key. */
        {
            JSValue s;
            r = step_tostring_run(ctx, h, f->value, in, &s, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return -1;
            in = JS_UNDEFINED;
            JS_FreeValue(ctx, f->value);
            f->value = s;
        }
        kname = JS_ToCString(ctx, f->name);
        kval = JS_ToCString(ctx, f->value);
        if (!kname || !kval) {
            if (kname) JS_FreeCString(ctx, kname);
            if (kval) JS_FreeCString(ctx, kval);
            return -1;
        }
        if (!header_is_bytestring(kname) || !header_is_bytestring(kval)) {
            JS_FreeCString(ctx, kname); JS_FreeCString(ctx, kval);
            JS_ThrowTypeError(ctx, "a header name or value is not a ByteString");
            return -1;
        }
        header_list_append(out, kname, kval);
        JS_FreeCString(ctx, kname);
        JS_FreeCString(ctx, kval);
        f->i++;
        f->phase = FILL_NAME_STR;
    }
}

/* ---- the constructor ------------------------------------------------------------------------------------ */

/* `stage` is the BODY's own, not the header's: hdr->stage is the args machine's argument cursor, and a body
   that wrote to it would move the conversion loop it is running inside. */
typedef struct { uint8_t stage; HeadersFill fill; HeaderList list; JSValue result; } JSHeadersCtorState;

static void js_headers_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSHeadersCtorState *s = st;
    headers_fill_visit(ctx, &s->fill, v);
    v->val(ctx, &s->result);
}

static void js_headers_ctor_release(JSContext *ctx, void *st)
{
    JSHeadersCtorState *s = st;
    headers_fill_release(ctx, &s->fill);
    header_list_free(&s->list);
    JS_FreeValue(ctx, s->result);
    s->result = JS_UNDEFINED;
}

static int js_headers_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSHeadersCtorState *s = st;
    int r;

    if (s->stage == 0) {
        /* JS_CFUNC_step_ctor delivers NEW_TARGET in the receiver slot and undefined for a plain call, which is
           how `Headers()` is told apart from `new Headers()` — the IDL declares a constructor, so it throws. */
        if (JS_IsUndefined(hdr->this_val)) {
            JS_FreeValue(ctx, cb_result);
            JS_ThrowTypeError(ctx, "constructor Headers requires 'new'");
            return -1;
        }
        headers_fill_init(&s->fill);
        s->result = JS_UNDEFINED;
        s->stage = 1;
    }
    r = headers_fill_run(ctx, hdr, &s->fill, argc > 0 ? argv[0] : JS_UNDEFINED, &s->list,
                         cb_result, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return -1;
    *presult = headers_new(ctx, &s->list);
    return JS_IsException(*presult) ? -1 : 0;
}

static const IdlStepDecl js_headers_ctor_decl = {
    js_headers_ctor_step, sizeof(JSHeadersCtorState), js_headers_ctor_visit, js_headers_ctor_release
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
    g_atom_length = JS_NewAtom(ctx, "length");
    g_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_proto), "Headers.prototype could not be allocated");
    idl_install_method(ctx, g_proto, "append", 2, idl_method_id(ctx, TWO_STR, 2, js_headers_member, HDR_APPEND));
    idl_install_method(ctx, g_proto, "set", 2, idl_method_id(ctx, TWO_STR, 2, js_headers_member, HDR_SET));
    idl_install_method(ctx, g_proto, "delete", 1, idl_method_id(ctx, TWO_STR, 1, js_headers_member, HDR_DELETE));
    idl_install_method(ctx, g_proto, "get", 1, idl_method_id(ctx, TWO_STR, 1, js_headers_member, HDR_GET));
    idl_install_method(ctx, g_proto, "has", 1, idl_method_id(ctx, TWO_STR, 1, js_headers_member, HDR_HAS));
    idl_install_method(ctx, g_proto, "getSetCookie", 0,
                       idl_method_id(ctx, TWO_STR, 0, js_headers_member, HDR_GETSETCOOKIE));
    g_ctor_stepid = idl_method_id_step(ctx, ONE_ANY, 1, NULL, 0, &js_headers_ctor_decl, 0);
}

/* The prototype and the interned name are this component's for the runtime's life, so they are released WITH
   it. Without this the prototype is a GC object nobody drops and JS_FreeRuntime's gc_obj_list walk reports it —
   which is exactly how it was found, on the first run of this file. */
void headers_free(JSContext *ctx)
{
    if (!g_headers_rt)
        return;
    JS_FreeValue(ctx, g_proto);
    g_proto = JS_UNDEFINED;
    JS_FreeAtom(ctx, g_atom_length);
    g_atom_length = JS_ATOM_NULL;
    g_headers_rt = NULL;
    g_ctor_stepid = -1;
}

void headers_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;

    DCHECK(g_ctor_stepid >= 0, "Headers was installed before headers_init declared its constructor");
    ctor = idl_step_constructor(ctx, "Headers", 1, g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the Headers interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, g_proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "Headers", ctor);
}
