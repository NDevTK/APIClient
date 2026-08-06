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
#include "solver/concolic.h"

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

enum { ITER_KEYS = 0, ITER_VALUES, ITER_ENTRIES };

/* The default iterator object Web IDL §3.7.10 defines: it holds the TARGET and an INDEX, not a snapshot, so a
   list mutated between steps is seen. */
typedef struct { JSValue target; int index; int kind; } HeadersIter;
static JSClassID g_iter_class;
static JSValue   g_iter_proto = JS_UNDEFINED;

static void headers_iter_finalizer(JSRuntime *rt, JSValue val)
{
    HeadersIter *it = JS_GetOpaque(val, g_iter_class);
    if (it) { JS_FreeValueRT(rt, it->target); js_free_rt(rt, it); }
}

static JSValue headers_iter_new(JSContext *ctx, JSValueConst target, int kind)
{
    HeadersIter *it;
    JSValue obj = JS_NewObjectProtoClass(ctx, g_iter_proto, g_iter_class);
    if (JS_IsException(obj))
        return obj;
    it = js_mallocz(ctx, sizeof(*it));
    if (!it) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    it->target = JS_DupValue(ctx, target);
    it->kind = kind;
    JS_SetOpaque(obj, it);
    return obj;
}

static JSValue js_headers_iter_next(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    HeadersIter *it = JS_GetOpaque(this_val, g_iter_class);
    HeaderList combined = { 0 };
    const HeaderList *src;
    JSValue res, val;

    (void)argc; (void)argv;
    if (!it)
        return JS_ThrowTypeError(ctx, "not a Headers iterator");
    src = headers_list_of(it->target);
    DCHECK(src != NULL, "a Headers iterator outlived the Headers it holds a reference to");
    header_sort_and_combine(src, &combined);
    res = JS_NewObject(ctx);
    if (JS_IsException(res)) { header_list_free(&combined); return res; }
    if (it->index >= combined.n) {
        header_list_free(&combined);
        JS_SetPropertyStr(ctx, res, "value", JS_UNDEFINED);
        JS_SetPropertyStr(ctx, res, "done", JS_NewBool(ctx, true));
        return res;
    }
    if (it->kind == ITER_KEYS) {
        val = JS_NewString(ctx, combined.e[it->index].name);
    } else if (it->kind == ITER_VALUES) {
        val = JS_NewString(ctx, combined.e[it->index].value);
    } else {
        val = JS_NewArray(ctx);
        if (!JS_IsException(val)) {
            JS_SetPropertyUint32(ctx, val, 0, JS_NewString(ctx, combined.e[it->index].name));
            JS_SetPropertyUint32(ctx, val, 1, JS_NewString(ctx, combined.e[it->index].value));
        }
    }
    it->index++;
    header_list_free(&combined);
    JS_SetPropertyStr(ctx, res, "value", val);
    JS_SetPropertyStr(ctx, res, "done", JS_NewBool(ctx, false));
    return res;
}

static JSValue js_headers_iter_self(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    return JS_DupValue(ctx, this_val);
}

static JSValue js_headers_iter_make(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                    int magic)
{
    (void)argc; (void)argv;
    if (!headers_list_of(this_val))
        return JS_ThrowTypeError(ctx, "not a Headers");
    return headers_iter_new(ctx, this_val, magic);
}

/* §3.7.10 forEach(callback, thisArg): the callback is the PAGE'S CODE, called once per pair with
   (value, key, this) — so this is a step machine with a call request, exactly like an event dispatch. Running
   it from a C loop is the drive-to-completion the engine aborts on. The list is re-combined at each step, so a
   callback that appends is seen by the steps after it. */
typedef struct {
    JSStepHdr hdr;
    uint8_t   cphase;   /* the call request's own phase */
    int       i;        /* THE RESUME POINT: the pair being handed to the callback */
    /* 2 + argc, which the call request states and which is NOT the argument count: [this, callback] and then
       the three the spec passes (value, key, headers). Sized at 4 it wrote one slot past the end of the state
       on every entry — a buffer overflow whose visible symptom was the machine reading its own callback
       argument back as undefined. */
    JSValue   cb[5];
} JSHeadersForEachState;

#define FOREACH_CB_SLOTS 5

static void js_headers_foreach_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSHeadersForEachState *s = st;
    int k;
    for (k = 0; k < FOREACH_CB_SLOTS; k++)
        v->val(ctx, &s->cb[k]);
}

static JSValue js_headers_foreach_fini(JSContext *ctx, void *st, bool take_result)
{
    JSHeadersForEachState *s = st;
    int k;
    (void)take_result;
    for (k = 0; k < FOREACH_CB_SLOTS; k++) { JS_FreeValue(ctx, s->cb[k]); s->cb[k] = JS_UNDEFINED; }
    return JS_UNDEFINED;   /* forEach returns undefined */
}

static int js_headers_foreach_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSHeadersForEachState *s = st;
    const HeaderList *src = headers_list_of(s->hdr.this_val);
    JSValueConst fn = step_arg(&s->hdr, 0), this_arg = step_arg(&s->hdr, 1);
    HeaderList combined = { 0 };
    JSValue ignored;
    int r, k;

    if (s->cphase == 0 && s->i == 0)
        for (k = 0; k < FOREACH_CB_SLOTS; k++) s->cb[k] = JS_UNDEFINED;   /* a zeroed state reads as INTEGER 0 */
    if (!src) {
        JS_FreeValue(ctx, cb_result);
        JS_ThrowTypeError(ctx, "not a Headers");
        return JS_STEP_ABRUPT;
    }
    if (!JS_IsFunction(ctx, fn)) {
        JS_FreeValue(ctx, cb_result);
        JS_ThrowTypeError(ctx, "Headers.forEach requires a callback");
        return JS_STEP_ABRUPT;
    }
    for (;;) {
        JSValueConst args[3];
        header_sort_and_combine(src, &combined);
        if (s->i >= combined.n) { header_list_free(&combined); JS_FreeValue(ctx, cb_result); return JS_STEP_DONE; }
        if (s->cphase == 0) {
            /* the operands are built fresh each entry; step_call_run dups them into its own buffer */
            JSValue v = JS_NewString(ctx, combined.e[s->i].value);
            JSValue k = JS_NewString(ctx, combined.e[s->i].name);
            args[0] = v; args[1] = k; args[2] = s->hdr.this_val;
            r = step_call_run(ctx, &s->cphase, s->cb, fn, this_arg, 3, args, cb_result, &ignored,
                              out_cb, out_argc);
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, k);
        } else {
            r = step_call_run(ctx, &s->cphase, s->cb, fn, this_arg, 3, NULL, cb_result, &ignored,
                              out_cb, out_argc);
        }
        header_list_free(&combined);
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;          /* parked ON THIS ENTRY; the resume comes back to it */
        JS_FreeValue(ctx, ignored);   /* the callback's return value is discarded */
        s->i++;
    }
}

static const JSTrampStepDef js_headers_foreach_def = {
    sizeof(JSHeadersForEachState), js_headers_foreach_step, js_headers_foreach_fini, 0,
    .visit = js_headers_foreach_visit
};
static int g_foreach_stepid = -1;

/* ---- the fill (HeadersInit -> a header list) ------------------------------------------------------------ */

enum { FILL_START = 0, FILL_ITER_ASKED, FILL_SEQ_PAIR, FILL_SEQ_ITEM,
       FILL_KEYS_ASKED, FILL_VALUE_ASKED, FILL_NAME_STR, FILL_VALUE_STR };

/* ---- the iterable cursor ---------------------------------------------------------------------------------- */

enum { IT_GET_ITERFN = 0, IT_CALL_ITERFN, IT_GET_NEXT, IT_CALL_NEXT, IT_GET_DONE, IT_GET_VALUE };

static void iter_cursor_init(IterCursor *c)
{
    memset(c, 0, sizeof *c);
    c->iterfn = c->iter = c->next_fn = c->res = c->value = JS_UNDEFINED;
    c->cb[0] = c->cb[1] = JS_UNDEFINED;
}

static void iter_cursor_visit(JSContext *ctx, IterCursor *c, JSStepVisit *v)
{
    int k;
    v->val(ctx, &c->iterfn); v->val(ctx, &c->iter); v->val(ctx, &c->next_fn);
    v->val(ctx, &c->res);    v->val(ctx, &c->value);
    for (k = 0; k < 2; k++) v->val(ctx, &c->cb[k]);
}

static void iter_cursor_release(JSContext *ctx, IterCursor *c)
{
    int k;
    JS_FreeValue(ctx, c->iterfn); JS_FreeValue(ctx, c->iter); JS_FreeValue(ctx, c->next_fn);
    JS_FreeValue(ctx, c->res);    JS_FreeValue(ctx, c->value);
    c->iterfn = c->iter = c->next_fn = c->res = c->value = JS_UNDEFINED;
    for (k = 0; k < 2; k++) { JS_FreeValue(ctx, c->cb[k]); c->cb[k] = JS_UNDEFINED; }
}

/* ONE VALUE per successful return: `c->done` says the iteration ended, otherwise `c->value` holds it (owned by
   the cursor). Call it again for the next. Returns >0 (the caller returns it), 0, or -1 with a throw live. */
static int iter_cursor_run(JSContext *ctx, JSStepHdr *h, IterCursor *c, JSValueConst src,
                           JSValue in, JSValue **out_cb, int *out_argc)
{
    int r;

    if (c->phase == IT_GET_ITERFN) {
        r = step_getprop_run(ctx, h, src, JS_WellKnownSymbolAtom(JS_WKS_ITERATOR), in, &c->iterfn,
                             out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return -1;
        in = JS_UNDEFINED;
        if (!JS_IsFunction(ctx, c->iterfn)) {
            JS_ThrowTypeError(ctx, "the value is not iterable");
            return -1;
        }
        c->phase = IT_CALL_ITERFN;
    }
    if (c->phase == IT_CALL_ITERFN) {
        r = step_call_run(ctx, &c->cphase, c->cb, c->iterfn, src, 0, NULL, in, &c->iter, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return -1;
        in = JS_UNDEFINED;
        if (!JS_IsObject(c->iter)) {
            JS_ThrowTypeError(ctx, "the iterator is not an object");
            return -1;
        }
        c->phase = IT_GET_NEXT;
    }
    if (c->phase == IT_GET_NEXT) {
        JSAtom a = JS_NewAtom(ctx, "next");
        r = step_getprop_run(ctx, h, c->iter, a, in, &c->next_fn, out_cb, out_argc);
        JS_FreeAtom(ctx, a);
        if (r > 0) return r;
        if (r < 0) return -1;
        in = JS_UNDEFINED;
        c->phase = IT_CALL_NEXT;
    }
    if (c->phase == IT_CALL_NEXT) {
        JS_FreeValue(ctx, c->res);
        c->res = JS_UNDEFINED;
        r = step_call_run(ctx, &c->cphase, c->cb, c->next_fn, c->iter, 0, NULL, in, &c->res, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return -1;
        in = JS_UNDEFINED;
        if (!JS_IsObject(c->res)) {
            JS_ThrowTypeError(ctx, "an iterator result is not an object");
            return -1;
        }
        c->phase = IT_GET_DONE;
    }
    if (c->phase == IT_GET_DONE) {
        JSValue d;
        JSAtom a = JS_NewAtom(ctx, "done");
        r = step_getprop_run(ctx, h, c->res, a, in, &d, out_cb, out_argc);
        JS_FreeAtom(ctx, a);
        if (r > 0) return r;
        if (r < 0) return -1;
        in = JS_UNDEFINED;
        c->done = JS_ToBool(ctx, d);
        JS_FreeValue(ctx, d);
        if (c->done) { c->phase = IT_CALL_NEXT; return 0; }
        c->phase = IT_GET_VALUE;
    }
    DCHECK(c->phase == IT_GET_VALUE, "the iterable cursor was re-entered at a phase it never parks in");
    {
        JSAtom a = JS_NewAtom(ctx, "value");
        JS_FreeValue(ctx, c->value);
        c->value = JS_UNDEFINED;
        r = step_getprop_run(ctx, h, c->res, a, in, &c->value, out_cb, out_argc);
        JS_FreeAtom(ctx, a);
        if (r > 0) return r;
        if (r < 0) return -1;
    }
    c->phase = IT_CALL_NEXT;   /* the next call resumes the loop, not the setup */
    return 0;
}

/* ---- the fill's own state ---------------------------------------------------------------------------------- */

void headers_fill_init(HeadersFill *f)
{
    memset(f, 0, sizeof *f);
    f->keys = f->name = f->value = JS_UNDEFINED;
    f->item[0] = f->item[1] = JS_UNDEFINED;
    iter_cursor_init(&f->outer);
    iter_cursor_init(&f->inner);
}

void headers_fill_visit(JSContext *ctx, HeadersFill *f, JSStepVisit *v)
{
    v->val(ctx, &f->keys);
    v->val(ctx, &f->name);
    v->val(ctx, &f->value);
    v->val(ctx, &f->item[0]);
    v->val(ctx, &f->item[1]);
    iter_cursor_visit(ctx, &f->outer, v);
    iter_cursor_visit(ctx, &f->inner, v);
}

void headers_fill_release(JSContext *ctx, HeadersFill *f)
{
    JS_FreeValue(ctx, f->keys);
    JS_FreeValue(ctx, f->name);
    JS_FreeValue(ctx, f->value);
    JS_FreeValue(ctx, f->item[0]);
    JS_FreeValue(ctx, f->item[1]);
    f->keys = f->name = f->value = f->item[0] = f->item[1] = JS_UNDEFINED;
    iter_cursor_release(ctx, &f->outer);
    iter_cursor_release(ctx, &f->inner);
}

int headers_fill_run(JSContext *ctx, JSStepHdr *h, HeadersFill *f, JSValueConst init, HeaderList *out,
                     JSValue in, JSValue **out_cb, int *out_argc)
{
    const HeaderList *src;
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
        JS_FreeValue(ctx, in);   /* nothing here asked for it; the arm below starts its own request */
        in = JS_UNDEFINED;
        f->phase = FILL_ITER_ASKED;
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
        if (JS_IsFunction(ctx, itf)) {
            JS_FreeValue(ctx, itf);
            f->phase = FILL_SEQ_PAIR;
        } else {
            JS_FreeValue(ctx, itf);
            f->phase = FILL_KEYS_ASKED;
        }
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
            const char *kn = JS_ToCString(ctx, f->item[0]);
            const char *kv = JS_ToCString(ctx, f->item[1]);
            int bad;
            if (!kn || !kv) {
                if (kn) JS_FreeCString(ctx, kn);
                if (kv) JS_FreeCString(ctx, kv);
                return -1;
            }
            bad = !header_is_bytestring(kn) || !header_is_bytestring(kv);
            if (!bad) header_list_append(out, kn, kv);
            JS_FreeCString(ctx, kn);
            JS_FreeCString(ctx, kv);
            if (bad) {
                JS_ThrowTypeError(ctx, "a header name or value is not a ByteString");
                return -1;
            }
        }
        f->phase = FILL_SEQ_PAIR;
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

    /* §5.2's `iterable<ByteString, ByteString>`. The three getters take no arguments and run none of the page's
       code, so they are ordinary members; forEach CALLS the page and is a machine. */
    {
        JSClassDef idef = { "Headers Iterator", .finalizer = headers_iter_finalizer };
        static const JSCFunctionListEntry iter_proto[] = {
            JS_CFUNC_DEF("next", 0, js_headers_iter_next),
        };
        JS_NewClassID(rt, &g_iter_class);
        JS_NewClass(rt, g_iter_class, &idef);
        g_iter_proto = JS_NewObject(ctx);
        CHECK(!JS_IsException(g_iter_proto), "the Headers iterator prototype could not be allocated");
        JS_SetPropertyFunctionList(ctx, g_iter_proto, iter_proto,
                                   (int)(sizeof(iter_proto) / sizeof(iter_proto[0])));
        /* §3.7.10: an iterator object IS iterable, returning itself — which is what lets `for (const e of
           h.entries())` work as well as `for (const e of h)`. */
        JS_SetProperty(ctx, g_iter_proto, JS_DupAtom(ctx, JS_WellKnownSymbolAtom(JS_WKS_ITERATOR)),
                       JS_NewCFunction(ctx, js_headers_iter_self, "[Symbol.iterator]", 0));
    }
    {
        static const IdlArgType NONE[1] = { IDL_ANY };
        idl_install_method(ctx, g_proto, "keys", 0,
                           idl_method_id(ctx, NONE, 0, js_headers_iter_make, ITER_KEYS));
        idl_install_method(ctx, g_proto, "values", 0,
                           idl_method_id(ctx, NONE, 0, js_headers_iter_make, ITER_VALUES));
        idl_install_method(ctx, g_proto, "entries", 0,
                           idl_method_id(ctx, NONE, 0, js_headers_iter_make, ITER_ENTRIES));
    }
    g_foreach_stepid = JS_RegisterStepDef(rt, &js_headers_foreach_def);
    idl_install_step_method(ctx, g_proto, "forEach", 1, g_foreach_stepid);
    /* §3.7.10: @@iterator on the interface itself IS `entries` — the same function object, so
       `h[Symbol.iterator] === h.entries` the way the spec states it. */
    {
        JSValue entries = JS_GetPropertyStr(ctx, g_proto, "entries");
        JS_SetProperty(ctx, g_proto, JS_DupAtom(ctx, JS_WellKnownSymbolAtom(JS_WKS_ITERATOR)), entries);
    }
}

/* The prototype and the interned name are this component's for the runtime's life, so they are released WITH
   it. Without this the prototype is a GC object nobody drops and JS_FreeRuntime's gc_obj_list walk reports it —
   which is exactly how it was found, on the first run of this file. */
void headers_free(JSContext *ctx)
{
    if (!g_headers_rt)
        return;
    JS_FreeValue(ctx, g_proto);
    JS_FreeValue(ctx, g_iter_proto);
    g_proto = g_iter_proto = JS_UNDEFINED;
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
