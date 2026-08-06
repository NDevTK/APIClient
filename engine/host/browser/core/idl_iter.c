/* WEB IDL'S `sequence<T>` CONVERSION, as a cursor — see idl_iter.h.
 *
 * It was inside core/fetch/headers.c, which is where it was first needed and not where it belongs: nothing in
 * it is about a header, and the second consumer (URLSearchParams' `sequence<sequence<USVString>>` arm) would
 * otherwise have copied it. Web IDL states this protocol once; so does the engine. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_iter.h"

enum { IT_GET_ITERFN = 0, IT_CALL_ITERFN, IT_GET_NEXT, IT_CALL_NEXT, IT_GET_DONE, IT_GET_VALUE };

void iter_cursor_init(IterCursor *c)
{
    memset(c, 0, sizeof *c);
    c->iterfn = c->iter = c->next_fn = c->res = c->value = JS_UNDEFINED;
    c->cb[0] = c->cb[1] = JS_UNDEFINED;
}

void iter_cursor_visit(JSContext *ctx, IterCursor *c, JSStepVisit *v)
{
    int k;
    v->val(ctx, &c->iterfn); v->val(ctx, &c->iter); v->val(ctx, &c->next_fn);
    v->val(ctx, &c->res);    v->val(ctx, &c->value);
    for (k = 0; k < 2; k++) v->val(ctx, &c->cb[k]);
}

void iter_cursor_release(JSContext *ctx, IterCursor *c)
{
    int k;
    JS_FreeValue(ctx, c->iterfn); JS_FreeValue(ctx, c->iter); JS_FreeValue(ctx, c->next_fn);
    JS_FreeValue(ctx, c->res);    JS_FreeValue(ctx, c->value);
    c->iterfn = c->iter = c->next_fn = c->res = c->value = JS_UNDEFINED;
    for (k = 0; k < 2; k++) { JS_FreeValue(ctx, c->cb[k]); c->cb[k] = JS_UNDEFINED; }
}

int iter_cursor_run(JSContext *ctx, JSStepHdr *h, IterCursor *c, JSValueConst src,
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



/* ---- §3.2.21's record<K, V> ------------------------------------------------------------------------------ */

enum { RC_KEYS_ASKED = 0, RC_DESC_ASKED, RC_VALUE_ASKED, RC_NEXT_KEY };

void record_cursor_init(RecordCursor *c)
{
    memset(c, 0, sizeof *c);
    c->keys = c->name = c->value = JS_UNDEFINED;
}

void record_cursor_visit(JSContext *ctx, RecordCursor *c, JSStepVisit *v)
{
    v->val(ctx, &c->keys); v->val(ctx, &c->name); v->val(ctx, &c->value);
}

void record_cursor_release(JSContext *ctx, RecordCursor *c)
{
    JS_FreeValue(ctx, c->keys); JS_FreeValue(ctx, c->name); JS_FreeValue(ctx, c->value);
    c->keys = c->name = c->value = JS_UNDEFINED;
}

int record_cursor_run(JSContext *ctx, JSStepHdr *h, RecordCursor *c, JSValueConst src, JSValue in,
                      int (*key_ok)(JSContext *ctx, JSValueConst key, void *user), void *user,
                      JSValue **out_cb, int *out_argc)
{
    int r;

    if (c->phase == RC_KEYS_ASKED) {
        JSValue lv;
        uint32_t n = 0;
        r = step_ownkeys_run(ctx, h, src, in, &c->keys, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return -1;
        in = JS_UNDEFINED;
        lv = JS_GetPropertyStr(ctx, c->keys, "length");   /* the engine's own array; nothing of the page's */
        JS_ToUint32(ctx, &n, lv);
        JS_FreeValue(ctx, lv);
        c->n = (int)n;
        c->i = 0;
        c->phase = RC_NEXT_KEY;
    }

    for (;;) {
        if (c->phase == RC_NEXT_KEY) {
            if (c->i >= c->n) { JS_FreeValue(ctx, in); c->done = 1; return 0; }
            JS_FreeValue(ctx, c->name);
            c->name = JS_GetPropertyUint32(ctx, c->keys, (uint32_t)c->i);
            c->phase = RC_DESC_ASKED;
        }
        if (c->phase == RC_DESC_ASKED) {
            JSValue desc;
            JSAtom a = JS_ValueToAtom(ctx, c->name);
            int keep;
            if (a == JS_ATOM_NULL) { JS_FreeValue(ctx, in); return -1; }
            r = step_getownprop_run(ctx, h, src, a, in, &desc, out_cb, out_argc);
            JS_FreeAtom(ctx, a);
            if (r > 0) return r;
            if (r < 0) return -1;
            in = JS_UNDEFINED;
            /* step 5.2: present AND enumerable, or the key is not part of the record. The descriptor is the
               engine's own object, so reading it reaches nothing of the page's. */
            keep = JS_IsObject(desc);
            if (keep) {
                JSValue en = JS_GetPropertyStr(ctx, desc, "enumerable");
                keep = JS_ToBool(ctx, en);
                JS_FreeValue(ctx, en);
            }
            JS_FreeValue(ctx, desc);
            if (!keep) { c->i++; c->phase = RC_NEXT_KEY; continue; }
            if (key_ok && key_ok(ctx, c->name, user) < 0) return -1;
            c->phase = RC_VALUE_ASKED;
        }
        DCHECK(c->phase == RC_VALUE_ASKED, "the record cursor was re-entered at a phase it never parks in");
        {
            JSAtom a = JS_ValueToAtom(ctx, c->name);
            if (a == JS_ATOM_NULL) { JS_FreeValue(ctx, in); return -1; }
            JS_FreeValue(ctx, c->value);
            c->value = JS_UNDEFINED;
            r = step_getprop_run(ctx, h, src, a, in, &c->value, out_cb, out_argc);
            JS_FreeAtom(ctx, a);
            if (r > 0) return r;            /* parked ON THIS KEY; the resume comes back to it */
            if (r < 0) return -1;
        }
        c->i++;
        c->phase = RC_NEXT_KEY;
        return 0;
    }
}

/* ---- §3.7.10's default iterator object ------------------------------------------------------------------- */

#include "core/idl_args.h"

enum { PAIR_KEYS = 0, PAIR_VALUES, PAIR_ENTRIES };

/* One declared interface. There are as many as there are `iterable<>` interfaces, which is a handful, so the
   table is fixed and full is a DCHECK rather than a growth path nobody exercises. */
#define IDL_PAIR_ITER_MAX 8
typedef struct {
    const IdlPairIterOps *ops;
    JSClassID class_id;
    JSValue   proto;      /* the ITERATOR prototype object (owned) */
    int       foreach_stepid;
} IdlPairIface;

static IdlPairIface g_pair[IDL_PAIR_ITER_MAX];
static int g_pair_n;

/* §3.7.10's iterator object: the TARGET and an INDEX, not a snapshot, so a list mutated between steps is seen. */
typedef struct { JSValue target; int index; int kind; int iface; } IdlPairIter;

static void idl_pair_iter_finalizer(JSRuntime *rt, JSValue val)
{
    int i;
    for (i = 0; i < g_pair_n; i++) {
        IdlPairIter *it = JS_GetOpaque(val, g_pair[i].class_id);
        if (it) { JS_FreeValueRT(rt, it->target); js_free_rt(rt, it); return; }
    }
}

static IdlPairIter *idl_pair_iter_of(JSValueConst v)
{
    int i;
    for (i = 0; i < g_pair_n; i++) {
        IdlPairIter *it = JS_GetOpaque(v, g_pair[i].class_id);
        if (it) return it;
    }
    return NULL;
}

static JSValue js_idl_pair_next(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    IdlPairIter *it = idl_pair_iter_of(this_val);
    const IdlPairIface *f;
    JSValue res, val, key, value;
    int n;

    (void)argc; (void)argv;
    if (!it)
        return JS_ThrowTypeError(ctx, "not an iterator of this interface");
    f = &g_pair[it->iface];
    n = f->ops->count(ctx, it->target);
    DCHECK(n >= 0, "an iterator outlived the object it holds a reference to");
    res = JS_NewObject(ctx);
    if (JS_IsException(res)) return res;
    if (it->index >= n) {
        JS_SetPropertyStr(ctx, res, "value", JS_UNDEFINED);
        JS_SetPropertyStr(ctx, res, "done", JS_NewBool(ctx, true));
        return res;
    }
    f->ops->pair(ctx, it->target, it->index, &key, &value);
    if (it->kind == PAIR_KEYS)        { val = key;   JS_FreeValue(ctx, value); }
    else if (it->kind == PAIR_VALUES) { val = value; JS_FreeValue(ctx, key); }
    else {
        val = JS_NewArray(ctx);
        if (JS_IsException(val)) { JS_FreeValue(ctx, key); JS_FreeValue(ctx, value); JS_FreeValue(ctx, res); return val; }
        JS_SetPropertyUint32(ctx, val, 0, key);
        JS_SetPropertyUint32(ctx, val, 1, value);
    }
    it->index++;
    JS_SetPropertyStr(ctx, res, "value", val);
    JS_SetPropertyStr(ctx, res, "done", JS_NewBool(ctx, false));
    return res;
}

/* magic packs the interface handle and the kind, because one C function serves every declared interface. */
static JSValue js_idl_pair_make(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    int iface = magic >> 2, kind = magic & 3;
    const IdlPairIface *f = &g_pair[iface];
    IdlPairIter *it;
    JSValue obj;

    (void)argc; (void)argv;
    if (f->ops->count(ctx, this_val) < 0)
        return JS_ThrowTypeError(ctx, "not a %s", f->ops->iface);
    obj = JS_NewObjectProtoClass(ctx, f->proto, f->class_id);
    if (JS_IsException(obj))
        return obj;
    it = js_mallocz(ctx, sizeof(*it));
    if (!it) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    it->target = JS_DupValue(ctx, this_val);
    it->kind = kind;
    it->iface = iface;
    JS_SetOpaque(obj, it);
    return obj;
}

/* §3.7.10 forEach(callback, thisArg): the callback is the PAGE'S CODE, called once per pair with
   (value, key, this) — so this is a step machine with a call request, exactly like an event dispatch. Running
   it from a C loop is the drive-to-completion the engine aborts on. The pairs are recomputed at each step, so
   a callback that appends is seen by the steps after it. */
typedef struct {
    JSStepHdr hdr;
    uint8_t   cphase;   /* the call request's own phase */
    int       i;        /* THE RESUME POINT: the pair being handed to the callback */
    /* 2 + argc, which the call request states and which is NOT the argument count: [this, callback] and then
       the three the spec passes (value, key, target). */
    JSValue   cb[5];
} IdlPairForEachState;

#define PAIR_FOREACH_CB_SLOTS 5

static void js_idl_pair_foreach_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    IdlPairForEachState *s = st;
    int k;
    for (k = 0; k < PAIR_FOREACH_CB_SLOTS; k++)
        v->val(ctx, &s->cb[k]);
}

static JSValue js_idl_pair_foreach_fini(JSContext *ctx, void *st, bool take_result)
{
    IdlPairForEachState *s = st;
    int k;
    (void)take_result;
    for (k = 0; k < PAIR_FOREACH_CB_SLOTS; k++) { JS_FreeValue(ctx, s->cb[k]); s->cb[k] = JS_UNDEFINED; }
    return JS_UNDEFINED;   /* forEach returns undefined */
}

static int js_idl_pair_foreach_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    IdlPairForEachState *s = st;
    const IdlPairIface *f = &g_pair[s->hdr.arg];
    JSValueConst fn = step_arg(&s->hdr, 0), this_arg = step_arg(&s->hdr, 1);
    JSValue ignored;
    int r, k, n;

    if (s->cphase == 0 && s->i == 0)
        for (k = 0; k < PAIR_FOREACH_CB_SLOTS; k++) s->cb[k] = JS_UNDEFINED;   /* a zeroed state reads as INTEGER 0 */
    if (f->ops->count(ctx, s->hdr.this_val) < 0) {
        JS_FreeValue(ctx, cb_result);
        JS_ThrowTypeError(ctx, "not a %s", f->ops->iface);
        return JS_STEP_ABRUPT;
    }
    if (!JS_IsFunction(ctx, fn)) {
        JS_FreeValue(ctx, cb_result);
        JS_ThrowTypeError(ctx, "%s.forEach requires a callback", f->ops->iface);
        return JS_STEP_ABRUPT;
    }
    for (;;) {
        JSValueConst args[3];
        JSValue key, value;
        n = f->ops->count(ctx, s->hdr.this_val);
        if (s->i >= n) { JS_FreeValue(ctx, cb_result); return JS_STEP_DONE; }
        if (s->cphase == 0) {
            /* the operands are built fresh each entry; step_call_run dups them into its own buffer */
            f->ops->pair(ctx, s->hdr.this_val, s->i, &key, &value);
            args[0] = value; args[1] = key; args[2] = s->hdr.this_val;
            r = step_call_run(ctx, &s->cphase, s->cb, fn, this_arg, 3, args, cb_result, &ignored,
                              out_cb, out_argc);
            JS_FreeValue(ctx, key);
            JS_FreeValue(ctx, value);
        } else {
            /* THE ARGC MUST MATCH ON RESUME. step_call_run's buffer is 2 + argc slots and the resume half reads
               it back at that width, so handing it 0 here left the call half-built and the machine parked for
               ever — visible only as a runtime that would not tear down, because a parked flow holds its
               receiver. The operands are already in the buffer; NULL says "do not rebuild them". */
            r = step_call_run(ctx, &s->cphase, s->cb, fn, this_arg, 3, NULL, cb_result, &ignored,
                              out_cb, out_argc);
        }
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;          /* parked ON THIS PAIR; the callback's own body suspends and resumes */
        if (r < 0) return JS_STEP_ABRUPT;
        JS_FreeValue(ctx, ignored);   /* §3.7.10 discards the callback's return value */
        s->i++;
    }
}

int idl_pair_iter_declare(JSContext *ctx, const IdlPairIterOps *ops)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    int handle = g_pair_n;
    IdlPairIface *f;
    JSClassDef def;
    JSValue intrinsic;
    char name[64];
    static JSTrampStepDef foreach_def = {
        sizeof(IdlPairForEachState), js_idl_pair_foreach_step, js_idl_pair_foreach_fini, 0,
        .visit = js_idl_pair_foreach_visit
    };

    DCHECK(g_pair_n < IDL_PAIR_ITER_MAX,
           "more iterable<> interfaces were declared than this table holds — grow it, the count is fixed "
           "because the platform's is");
    f = &g_pair[handle];
    f->ops = ops;
    snprintf(name, sizeof name, "%s Iterator", ops->iface);
    memset(&def, 0, sizeof def);
    def.class_name = name;   /* JS_NewClass copies it */
    def.finalizer = idl_pair_iter_finalizer;
    JS_NewClassID(rt, &f->class_id);
    JS_NewClass(rt, f->class_id, &def);

    /* §3.7.10: the ITERATOR PROTOTYPE OBJECT inherits from %IteratorPrototype%. That is where `@@iterator`
       returning `this` comes from (so `for (const e of h.entries())` works) and where the ES2025 iterator
       helpers come from, so none of that is re-declared here. */
    intrinsic = JS_GetIteratorPrototype(ctx);
    f->proto = JS_NewObjectProto(ctx, intrinsic);
    JS_FreeValue(ctx, intrinsic);
    CHECK(!JS_IsException(f->proto), "an iterator prototype object could not be allocated");
    /* §3.7.10 gives it all three attributes — ENUMERABLE included, unlike an interface prototype's members. */
    JS_DefinePropertyValueStr(ctx, f->proto, "next",
                              JS_NewCFunction(ctx, js_idl_pair_next, "next", 0),
                              JS_PROP_WRITABLE | JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValue(ctx, f->proto, JS_DupAtom(ctx, JS_WellKnownSymbolAtom(JS_WKS_TO_STRING_TAG)),
                           JS_NewString(ctx, name), JS_PROP_CONFIGURABLE);

    f->foreach_stepid = JS_RegisterStepDef(rt, &foreach_def);
    CHECK(f->foreach_stepid >= 0, "no step id for an iterable<>'s forEach");
    g_pair_n++;
    return handle;
}

void idl_pair_iter_install(JSContext *ctx, JSValueConst proto, int handle)
{
    static const IdlArgType NONE[1] = { IDL_ANY };
    JSValue entries;

    DCHECK(handle >= 0 && handle < g_pair_n, "an iterable<> was installed with a handle nothing declared");
    idl_install_method(ctx, proto, "keys", 0,
                       idl_method_id(ctx, NONE, 0, js_idl_pair_make, (handle << 2) | PAIR_KEYS));
    idl_install_method(ctx, proto, "values", 0,
                       idl_method_id(ctx, NONE, 0, js_idl_pair_make, (handle << 2) | PAIR_VALUES));
    idl_install_method(ctx, proto, "entries", 0,
                       idl_method_id(ctx, NONE, 0, js_idl_pair_make, (handle << 2) | PAIR_ENTRIES));
    /* ONE forEach def serves every interface; `arg` is which one, read off the header by the step. */
    idl_install_step_method(ctx, proto, "forEach", 1, g_pair[handle].foreach_stepid);
    /* §3.7.10: @@iterator on the interface IS `entries` — the same function object, so
       `h[Symbol.iterator] === h.entries` the way the spec states it. */
    entries = JS_GetPropertyStr(ctx, proto, "entries");
    JS_DefinePropertyValue(ctx, (JSValue)proto, JS_DupAtom(ctx, JS_WellKnownSymbolAtom(JS_WKS_ITERATOR)),
                           entries, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}

void idl_pair_iter_free(JSContext *ctx, int handle)
{
    DCHECK(handle >= 0 && handle < g_pair_n, "an iterable<> was freed with a handle nothing declared");
    JS_FreeValue(ctx, g_pair[handle].proto);
    g_pair[handle].proto = JS_UNDEFINED;
}
