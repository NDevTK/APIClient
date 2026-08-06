/* THE BODY MIXIN — WHATWG Fetch §5.2, which BOTH Request and Response include.
 *
 * It lived inside response.c, which is where it was first needed and not where it belongs: `Request` includes
 * the same mixin, so the alternative was a second copy of the single-use latch, the four readers and the
 * promise-settling machine — and the latch is COW-captured state, so two copies would be two places for a
 * time-travel bug to live.
 *
 * ONE SET OF READER MACHINES for the whole platform. A step def is registered per (interface, reader) pair
 * only because the def's `arg` is what the step reads to know both which reader it is and whose receiver it
 * has; the CODE is one function. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "solver/cow.h"
#include "core/fetch/body.h"

/* One interface that includes Body. There are two in the platform (Request and Response), so the table is
   fixed and full is a DCHECK rather than a growth path nobody exercises. */
#define BODY_IFACE_MAX 4
typedef struct {
    JSClassID    class_id;
    BodyState *(*of)(JSValueConst v);
    const char  *iface;
    int          stepid[4];   /* one per reader kind */
} BodyIface;

static BodyIface g_body_iface[BODY_IFACE_MAX];
static int g_body_iface_n;

void body_state_free(JSContext *ctx, BodyState *b)
{
    js_free(ctx, b->bytes);
    b->bytes = NULL;
    b->len = 0;
    b->has = 0;
}

int body_state_set(JSContext *ctx, BodyState *b, const char *bytes, size_t len)
{
    js_free(ctx, b->bytes);
    b->bytes = NULL;
    b->len = 0;
    b->has = bytes != NULL;
    if (!bytes) return 0;
    /* +1 and a NUL past the end, so the bytes can still be handed to a C string consumer; `len` is what every
       read here uses, and it is what an interior NUL no longer truncates. */
    b->bytes = js_mallocz(ctx, len + 1);
    if (!b->bytes) return -1;
    if (len) memcpy(b->bytes, bytes, len);
    b->len = len;
    return 0;
}

/* §5.2 "consume body": the latch is per object and the second read is a TypeError — a page's retry path tests
   exactly that, so answering the body twice would hide the branch it takes.
   THE LATCH IS PER FLOW, TOO. It lives in the including component's class opaque, which no property hook and no
   engine hook can see, so setting it was a write that did not time-travel: a Response created before a fork and
   read in one arm came back CONSUMED in the sibling, whose own first read then threw `body stream already
   read`. Every other kind of shared state a flow writes rides its COW delta, and so does this one — the capture
   goes immediately before the write, so the bytes the delta records are the ones this flow found. */
static JSValue body_take(JSContext *ctx, const BodyIface *f, JSValueConst this_val,
                         const char **pbody, size_t *plen)
{
    BodyState *b = f->of(this_val);
    if (!b)
        return JS_ThrowTypeError(ctx, "not a %s", f->iface);
    if (b->used)
        return JS_ThrowTypeError(ctx, "body stream already read");
    cow_capture_host_state(ctx, this_val, &b->used, sizeof b->used);
    b->used = 1;
    *pbody = b->bytes ? b->bytes : "";
    *plen  = b->bytes ? b->len : 0;
    return JS_UNDEFINED;
}

/* §5.2's READERS AS ONE STEP MACHINE.
 *
 * The bytes are already here, so the promise this returns is settled before the page ever sees it — but SETTLING
 * it is not a C-private act. 27.2.1.3.2 step 8 reads `Get(resolution, "then")` off the value, which for
 * `json()`'s result is an ordinary object whose prototype the page owns: `Object.prototype.then = { get(){…} }`
 * makes that read the page's code, and prototype pollution is a gadget class this engine exists to run rather
 * than assume away. Performed with a JS_Call from C it would run in an activation with no flow base — a loop in
 * that getter would drive to completion — which is what the assert here used to say instead of fixing, and
 * `json()` therefore aborted on EVERY object body: `fetch(u).then(r => r.json())`, the shape every bundle is
 * written in. The resolving function is a CALL REQUEST now, so the read happens on the tramp where it can
 * suspend and fork, and both readers share the one machine because they differ only in how the value is
 * computed. */
enum { BODY_TEXT = 0, BODY_JSON = 1, BODY_ARRAYBUFFER = 2, BODY_BYTES = 3, BODY_KINDS };

/* WHICH INTERFACE THE RECEIVER BELONGS TO is found from the receiver, not carried in the def's `arg`. Packing
   the pair into `arg` needed a def per (interface, kind) built at RUNTIME, and a runtime-assembled def is
   invisible to engine/check_step_visits.mjs — the gate that pairs every step declaration with the visit for
   its state struct silently stopped covering these four. `arg` stays the KIND, the defs stay static
   initialisers the gate can read, and the interface is a class-id lookup. */
static const BodyIface *body_iface_of(JSValueConst v)
{
    int i;
    for (i = 0; i < g_body_iface_n; i++)
        if (g_body_iface[i].of(v)) return &g_body_iface[i];
    return NULL;
}

typedef struct JSBodyState {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t   stage;
    uint8_t   cphase;   /* the settle call's own phase, so the stage can hold it across a suspension */
    JSValue   promise;  /* the capability's promise — this machine's result (owned) */
    JSValue   func;     /* its resolve or its reject, whichever this body read settles with (owned) */
    JSValue   value;    /* what it settles WITH: the text, the parsed body, or the error (owned) */
    JSValue   cb[3];    /* the call request buffer: [this, resolving function, value] */
} JSBodyState;

/* WHAT THIS MACHINE OWNS. The call buffer is in here for the reason dispatch's is: a `then` getter that forks
   the flow must not leave two arms sharing one invocation. */
static void js_body_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSBodyState *s = st;
    int k;
    v->val(ctx, &s->promise);
    v->val(ctx, &s->func);
    v->val(ctx, &s->value);
    for (k = 0; k < 3; k++)
        v->val(ctx, &s->cb[k]);
}

static JSValue js_body_fini(JSContext *ctx, void *st, bool take_result)
{
    JSBodyState *s = st;
    JSValue r = take_result ? s->promise : JS_UNDEFINED;
    int k;

    if (take_result) s->promise = JS_UNDEFINED;
    JS_FreeValue(ctx, s->promise);
    JS_FreeValue(ctx, s->func);
    JS_FreeValue(ctx, s->value);
    s->promise = s->func = s->value = JS_UNDEFINED;
    for (k = 0; k < 3; k++) {
        JS_FreeValue(ctx, s->cb[k]);
        s->cb[k] = JS_UNDEFINED;
    }
    return r;
}

/* Stage 0 turns the bytes into the value this reader answers with; stage 1 settles the promise with it, which is
   the request. Nothing in stage 0 runs the page's code — the body is the host's bytes and the latch is this
   component's — which is why the four kinds are one stage and not four machines. json() parses with the REAL
   parser, so a malformed body rejects with the SyntaxError the page would actually catch rather than a
   placeholder this engine invented. */
static int js_body_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSBodyState *s = st;
    JSValue settled, funcs[2];
    int reject = 0, r;

    if (s->stage == 0) {
        const char *body = NULL;
        size_t len = 0;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        {
            const BodyIface *f = body_iface_of(s->hdr.this_val);
            if (!f) {
                JS_ThrowTypeError(ctx, "a body reader was called on something that does not include Body");
                return JS_STEP_ABRUPT;
            }
            settled = body_take(ctx, f, s->hdr.this_val, &body, &len);
        }
        if (JS_IsException(settled)) {
            s->value = JS_GetException(ctx);
            reject = 1;
        } else if (s->hdr.arg == BODY_JSON) {
            s->value = JS_ParseJSON(ctx, body, len, "<response>");
            if (JS_IsException(s->value)) { s->value = JS_GetException(ctx); reject = 1; }
        } else if (s->hdr.arg == BODY_ARRAYBUFFER || s->hdr.arg == BODY_BYTES) {
            /* §5.2's arrayBuffer() / bytes(): the byte sequence itself, the two readers the string body
               could not have answered honestly. A COPY, because what the page gets is ITS OWN to detach,
               transfer or write through — handing out this Response's storage would let one flow mutate a
               reply another is still reading, and a detach would leave that flow reading freed memory. */
            s->value = s->hdr.arg == BODY_BYTES
                     ? JS_NewUint8ArrayCopy(ctx, (const uint8_t *)body, len)
                     : JS_NewArrayBufferCopy(ctx, (const uint8_t *)body, len);
            if (JS_IsException(s->value)) return JS_STEP_ABRUPT;
        } else {
            /* §5.2's text(): UTF-8 decode, which is what JS_NewStringLen performs over the host's bytes. */
            DCHECK(s->hdr.arg == BODY_TEXT, "the body-read machine was declared with a kind it does not read");
            s->value = JS_NewStringLen(ctx, body, len);
            if (JS_IsException(s->value)) return JS_STEP_ABRUPT;
        }
        /* The NATIVE capability: %Promise% with no subclass in sight, so building it constructs nothing of the
           page's. Only the settle below is the page's, and that is the request. */
        s->promise = JS_NewPromiseCapability(ctx, funcs);
        if (JS_IsException(s->promise)) return JS_STEP_ABRUPT;
        s->func = funcs[reject];
        JS_FreeValue(ctx, funcs[reject ^ 1]);
        s->stage = 1;
    }

    DCHECK(s->stage == 1, "the body-read machine was re-entered at a stage it never parks in");
    r = step_call_run(ctx, &s->cphase, s->cb, s->func, JS_UNDEFINED, 1, (JSValueConst *)&s->value,
                      cb_result, &settled, out_cb, out_argc);
    if (r > 0) return r;          /* parked ON THE SETTLE; the `then` read runs with a flow base under it */
    JS_FreeValue(ctx, settled);   /* a resolving function's return value is undefined and unobservable */
    return JS_STEP_DONE;
}

/* ONE machine, one def per (INTERFACE, KIND) pair — the code is one function and `arg` is the pair, which is
   the whole of the difference between the eight. They are DECLARED as a list because that is what the
   registration and the install both walk: a fifth reader (§5.2's blob(), once there is a Blob) is one row here
   and nothing else, and a row that is added without a step arm reaches the step's own DCHECK. */
static const char *const BODY_READER_NAME[BODY_KINDS] = { "text", "json", "arrayBuffer", "bytes" };

static const JSTrampStepDef js_body_defs[BODY_KINDS] = {
    { sizeof(JSBodyState), js_body_step, js_body_fini, BODY_TEXT,        .visit = js_body_visit },
    { sizeof(JSBodyState), js_body_step, js_body_fini, BODY_JSON,        .visit = js_body_visit },
    { sizeof(JSBodyState), js_body_step, js_body_fini, BODY_ARRAYBUFFER, .visit = js_body_visit },
    { sizeof(JSBodyState), js_body_step, js_body_fini, BODY_BYTES,       .visit = js_body_visit },
};

int body_declare(JSContext *ctx, JSClassID class_id, BodyState *(*of)(JSValueConst v), const char *iface)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    int handle = g_body_iface_n, k;
    BodyIface *f;

    DCHECK(g_body_iface_n < BODY_IFACE_MAX,
           "more interfaces included Body than this table holds — grow it, the count is fixed because the "
           "platform's is");
    f = &g_body_iface[handle];
    f->class_id = class_id;
    f->of = of;
    f->iface = iface;
    for (k = 0; k < BODY_KINDS; k++) {
        /* ONE def per KIND, shared by every including interface — the step reads its receiver to know whose
           body it has, so the def carries nothing interface-specific. Registering the same def twice hands
           back the same behaviour under two ids, which is what lets each prototype install its own function
           object without the defs multiplying. */
        f->stepid[k] = JS_RegisterStepDef(rt, &js_body_defs[k]);
        CHECK(f->stepid[k] >= 0, "body: no step id for a reader — the body would be unreadable");
    }
    g_body_iface_n++;
    return handle;
}

/* §5.2's `bodyUsed`. One getter for every including interface, told apart by the same handle. */
static JSValue js_body_get_used(JSContext *ctx, JSValueConst this_val, int magic)
{
    const BodyIface *f = &g_body_iface[magic];
    BodyState *b = f->of(this_val);
    if (!b) return JS_ThrowTypeError(ctx, "not a %s", f->iface);
    return JS_NewBool(ctx, b->used != 0);
}

void body_install(JSContext *ctx, JSValueConst proto, int handle)
{
    int k;
    DCHECK(handle >= 0 && handle < g_body_iface_n, "Body was installed with a handle nothing declared");
    for (k = 0; k < BODY_KINDS; k++)
        JS_SetPropertyStr(ctx, (JSValue)proto, BODY_READER_NAME[k],
                          JS_NewCFunction2(ctx, NULL, BODY_READER_NAME[k], 0, JS_CFUNC_step,
                                           g_body_iface[handle].stepid[k]));
    {
        JSCFunctionListEntry e = JS_CGETSET_MAGIC_DEF("bodyUsed", js_body_get_used, NULL, 0);
        e.magic = (int16_t)handle;
        JS_SetPropertyFunctionList(ctx, (JSValue)proto, &e, 1);
    }
}
