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

/* See idl_iter.h. GetMethod's steps 2 to 4, over the answer its step 1 already obtained. */
int idl_get_method(JSContext *ctx, JSValueConst method, const char *what)
{
    if (JS_IsUndefined(method) || JS_IsNull(method)) return 0;   /* step 2 */
    if (!JS_IsFunction(ctx, method)) {                           /* step 3 */
        JS_ThrowTypeError(ctx, "%s is not callable", what);
        return -1;
    }
    return 1;                                                    /* step 4 */
}

void iter_cursor_init(IterCursor *c)
{
    memset(c, 0, sizeof *c);
    c->iterfn = c->iter = c->next_fn = c->res = c->value = JS_UNDEFINED;
    c->cb[0] = c->cb[1] = JS_UNDEFINED;
}

/* See idl_iter.h. The one difference from the init above is WHERE the cursor starts: the @@iterator read has
   already happened, in the union algorithm that chose this arm, so re-running it here would be the page's
   getter called twice for one conversion. */
void iter_cursor_init_from_method(JSContext *ctx, IterCursor *c, JSValue method)
{
    DCHECK(JS_IsFunction(ctx, method),
           "an iterable cursor was planted with an @@iterator that is not callable — ECMAScript's GetMethod "
           "answers undefined for null and undefined and throws a TypeError for anything else, so the caller "
           "has already decided this and there is nothing here to fall back to");
    /* AND IT HOLDS NOTHING YET. The init below MEMSETS, so a cursor planted mid-walk drops its iterator, its
       `next` and its last result rather than releasing them — a leak the runtime's GC walk sees and no call
       site names. IT_GET_ITERFN is the one phase at which nothing has been written, including the park inside
       the @@iterator read, so the phase alone states the invariant for a zeroed state and an init'd one alike. */
    DCHECK(c->phase == IT_GET_ITERFN,
           "an iterable cursor was planted with a method while it was already walking a source of its own");
    iter_cursor_init(c);
    c->iterfn = method;
    c->phase = IT_CALL_ITERFN;
}

void iter_cursor_visit(JSContext *ctx, IterCursor *c, JSStepVisit *v)
{
    int k;
    v->val(ctx, &c->iterfn); v->val(ctx, &c->iter); v->val(ctx, &c->next_fn);
    v->val(ctx, &c->res);    v->val(ctx, &c->value);
    for (k = 0; k < 2; k++) v->val(ctx, &c->cb[k]);
}

/* The declaration above discharged, and nothing restated. §6.2's own conversion releases a cursor MID-walk (the
   previous pair's iterator, before the next pair's), which is why this stays a function; a machine whose `visit`
   names a cursor must not call it at a teardown, because the teardown discharges the same declaration. */
void iter_cursor_release(JSContext *ctx, IterCursor *c)
{
    iter_cursor_visit(ctx, c, JS_StepFreeVisitor());
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
        /* GetIterator step 1.b is a SECOND step past GetMethod, and only here: this cursor iterates
           unconditionally, so an ABSENT @@iterator is its own TypeError, while §3.2.25's union reads the same
           0 as "take the other arm". The cursor keeps `iterfn` on either throw — its `visit` names it. */
        r = idl_get_method(ctx, c->iterfn, "the value's @@iterator");
        if (r < 0) return -1;
        if (r == 0) {
            JS_ThrowTypeError(ctx, "the value is not iterable");
            return -1;
        }
        c->phase = IT_CALL_ITERFN;
    }
    if (c->phase == IT_CALL_ITERFN) {
        r = step_call_run(ctx, &c->cphase, STEP_CB(c->cb), c->iterfn, src, 0, NULL, in, &c->iter, out_cb, out_argc);
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
        r = step_call_run(ctx, &c->cphase, STEP_CB(c->cb), c->next_fn, c->iter, 0, NULL, in, &c->res, out_cb, out_argc);
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



/* ---- §3.2.23's record<K, V> ------------------------------------------------------------------------------ */

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
#include "core/realm.h"

enum { PAIR_KEYS = 0, PAIR_VALUES, PAIR_ENTRIES };

/* One declared interface. There are as many as there are `iterable<>` interfaces, which is a handful, so the
   table is fixed and full is a DCHECK rather than a growth path nobody exercises. */
#define IDL_PAIR_ITER_MAX 8
typedef struct {
    const IdlPairIterOps *ops;
    JSClassID class_id;
    int       foreach_stepid;
    /* §3.7.10's THREE OPERATIONS, declared with the interface rather than minted at each install. A pool entry
       is an AGENT registration and the pool is SEALED once the agent's realms start building: minting here per
       realm aborted the second realm with the member's name ("keys"), which is the seal doing its job. */
    int       id_keys, id_values, id_entries;
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

/* The declared interface whose instance `v` is, or NULL. */
static const IdlPairIface *idl_pair_iface_of(JSContext *ctx, JSValueConst v)
{
    int i;
    for (i = 0; i < g_pair_n; i++)
        if (g_pair[i].ops->count(ctx, v) >= 0) return &g_pair[i];
    return NULL;
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
    {
        /* §3.7.10's ITERATOR PROTOTYPE OBJECT IS THIS REALM'S — it inherits %IteratorPrototype%, which is a
           per-realm intrinsic, so one shared object would give a child document's `h.entries()` the parent's
           iterator helpers and answer `instanceof` across a boundary. */
        JSValue iproto = JS_GetClassProto(ctx, f->class_id);
        DCHECK(!JS_IsNull(iproto), "an iterable<>'s iterator was minted in a realm that never ran its install");
        obj = JS_NewObjectProtoClass(ctx, iproto, f->class_id);
        JS_FreeValue(ctx, iproto);
    }
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

/* WHERE THIS MACHINE RESTS. §3.7.10's forEach is four steps and only one of them can suspend — step 4.2's
   invocation of the callback — so the whole walk is that one stage, with `i` as its cursor. Step 4.3's
   re-read of the pairs is why `count` is asked again on every entry rather than once. */
#define PAIR_FOREACH_STAGES(X) \
    X(PAIR_FOREACH_INVOKE, \
      "Web IDL §3.7.10 forEach(callback, thisArg) steps 4.1-4.4 (invoke callback with the pair's value, its " \
      "key and the object; the pairs are re-read afterwards because the callback may have changed them)")
enum { PAIR_FOREACH_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const PAIR_FOREACH_STEPS[] = { PAIR_FOREACH_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_idl_pair_foreach_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    IdlPairForEachState *s = st;
    /* WHICH INTERFACE THE RECEIVER BELONGS TO comes from the RECEIVER. One static def serves every interface,
       so its `arg` is the same for all of them — reading the interface off it made URLSearchParams.forEach
       run as Headers.forEach and throw "not a Headers". This is the same defect the body readers had, and it
       has the same fix: ask the value, not the definition. */
    const IdlPairIface *f = idl_pair_iface_of(ctx, s->hdr.this_val);
    JSValueConst fn = step_arg(&s->hdr, 0), this_arg = step_arg(&s->hdr, 1);
    JSValue ignored;
    int r, k, n;

    DCHECK(s->hdr.stage == PAIR_FOREACH_INVOKE,
           "an iterable<>'s forEach resumed at a stage §3.7.10 does not have");
    if (s->cphase == 0 && s->i == 0)
        for (k = 0; k < PAIR_FOREACH_CB_SLOTS; k++) s->cb[k] = JS_UNDEFINED;   /* a zeroed state reads as INTEGER 0 */
    if (!f) {
        JS_FreeValue(ctx, cb_result);
        JS_ThrowTypeError(ctx, "forEach was called on something that is not an iterable interface");
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
            r = step_call_run(ctx, &s->cphase, STEP_CB(s->cb), fn, this_arg, 3, args, cb_result, &ignored,
                              out_cb, out_argc);
            JS_FreeValue(ctx, key);
            JS_FreeValue(ctx, value);
        } else {
            /* THE ARGC MUST MATCH ON RESUME. step_call_run's buffer is 2 + argc slots and the resume half reads
               it back at that width, so handing it 0 here left the call half-built and the machine parked for
               ever — visible only as a runtime that would not tear down, because a parked flow holds its
               receiver. The operands are already in the buffer; NULL says "do not rebuild them". */
            r = step_call_run(ctx, &s->cphase, STEP_CB(s->cb), fn, this_arg, 3, NULL, cb_result, &ignored,
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
        sizeof(IdlPairForEachState), js_idl_pair_foreach_step, NULL, 0,   /* forEach returns undefined */
        .visit = js_idl_pair_foreach_visit,
        .algorithm = "Web IDL §3.7.10 forEach(callback, thisArg) of an iterable<> interface",
        .steps = PAIR_FOREACH_STEPS
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

    f->foreach_stepid = JS_RegisterStepDef(rt, &foreach_def);
    CHECK(f->foreach_stepid >= 0, "no step id for an iterable<>'s forEach");
    {
        static const IdlArgType NONE[1] = { IDL_ANY };
        f->id_keys    = idl_method_id(ctx, NONE, 0, js_idl_pair_make, (handle << 2) | PAIR_KEYS);
        f->id_values  = idl_method_id(ctx, NONE, 0, js_idl_pair_make, (handle << 2) | PAIR_VALUES);
        f->id_entries = idl_method_id(ctx, NONE, 0, js_idl_pair_make, (handle << 2) | PAIR_ENTRIES);
    }
    g_pair_n++;
    /* ONE ENTRY IN core/realm.h's LIST FOR EVERY iterable<>, declared with the first of them — the list is run
       after all the declarations are in, so the one install below builds every declared interface's iterator
       prototype and a component added later needs no entry of its own. */
    if (handle == 0)
        realm_declare_intrinsic(idl_pair_iter_install_protos);
    return handle;
}

/* §3.7.10's ITERATOR PROTOTYPE OBJECTS, FOR ONE REALM — one per declared iterable<>. */
void idl_pair_iter_install_protos(JSContext *ctx)
{
    int i;

    for (i = 0; i < g_pair_n; i++) {
        IdlPairIface *f = &g_pair[i];
        /* §3.7.10: the ITERATOR PROTOTYPE OBJECT inherits from %IteratorPrototype%. That is where `@@iterator`
           returning `this` comes from (so `for (const e of h.entries())` works) and where the ES2025 iterator
           helpers come from, so none of that is re-declared here. */
        JSValue intrinsic = JS_GetIteratorPrototype(ctx);
        JSValue proto = JS_NewObjectProto(ctx, intrinsic);
        char name[64];

        JS_FreeValue(ctx, intrinsic);
        CHECK(!JS_IsException(proto), "an iterator prototype object could not be allocated");
        snprintf(name, sizeof name, "%s Iterator", f->ops->iface);
        /* §3.7.10 gives it all three attributes — ENUMERABLE included, unlike an interface prototype's. */
        JS_DefinePropertyValueStr(ctx, proto, "next",
                                  JS_NewCFunction(ctx, js_idl_pair_next, "next", 0),
                                  JS_PROP_WRITABLE | JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE);
        JS_DefinePropertyValue(ctx, proto, JS_DupAtom(ctx, JS_WellKnownSymbolAtom(JS_WKS_TO_STRING_TAG)),
                               JS_NewString(ctx, name), JS_PROP_CONFIGURABLE);
        JS_SetClassProto(ctx, f->class_id, proto);
    }
}

void idl_pair_iter_install(JSContext *ctx, JSValueConst proto, int handle)
{
    JSValue entries;

    DCHECK(handle >= 0 && handle < g_pair_n, "an iterable<> was installed with a handle nothing declared");
    idl_install_method(ctx, proto, "keys", 0, g_pair[handle].id_keys);
    idl_install_method(ctx, proto, "values", 0, g_pair[handle].id_values);
    idl_install_method(ctx, proto, "entries", 0, g_pair[handle].id_entries);
    /* ONE forEach def serves every interface; `arg` is which one, read off the header by the step. */
    idl_install_step_method(ctx, proto, "forEach", 1, g_pair[handle].foreach_stepid);
    /* §3.7.10: @@iterator on the interface IS `entries` for an `iterable<K, V>` and `values` for a
       `setlike<V>` — the SAME function object either way, so `h[Symbol.iterator] === h.entries` (or
       `=== s.values`) the way the spec states it. Which one comes off the declaration, not off the caller. */
    entries = JS_GetPropertyStr(ctx, proto, g_pair[handle].ops->setlike ? "values" : "entries");
    JS_DefinePropertyValue(ctx, (JSValue)proto, JS_DupAtom(ctx, JS_WellKnownSymbolAtom(JS_WKS_ITERATOR)),
                           entries, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}


