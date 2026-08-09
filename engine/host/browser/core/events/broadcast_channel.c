/* BroadcastChannel — HTML §9.5.
 *
 * WHAT IT IS. A named bus: every open channel with the same name, in the same origin, receives what any other
 * one posts — except the sender. It is the third thing in HTML that delivers a MessageEvent, after a port and
 * a window post, and it reuses all of the same machinery: StructuredSerialize at the post, a TASK per
 * destination, and the event fired at each.
 *
 * THE REGISTRY IS A JS ARRAY, and that is not incidental. A channel is shared state a flow creates, and the
 * membership of the bus is exactly the kind of thing one forked arm must not decide for its sibling: an arm
 * that opens a channel and posts must not deliver into an arm that never opened one. An Array's mutations are
 * property writes the COW delta already captures, so the per-flow behaviour is the engine's rather than this
 * file's — the same reason a port's message queue is one.
 *
 * DELIVERY IS ORDERED BY CREATION, which is the standard's own rule and is why the registry is a list rather
 * than a set. A page that opens three channels and posts once observes them in the order it opened them.
 *
 * CROSS-DOCUMENT IS WHERE THIS BUS ACTUALLY LIVES, and half of it is built. An INSTANCE is an origin-keyed
 * agent cluster, and §9.5's destination set is "same origin, same agent cluster" — which is exactly one
 * instance — so every document of this origin in this browsing-context group is a realm HERE and the registry
 * below already spans them: an opener and its same-origin popup share this bus with no transport at all. What
 * remains is the SECOND browsing-context group (a `noopener` popup is its own), where the destination list
 * spans instances and the post has to travel with the sender's origin stamped by the trusted zone and the
 * sending flow's world carried alongside, for the reason window_message.c gives. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/structured_clone.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/events/message_event.h"
#include "core/events/broadcast_channel.h"
#include "solver/cow.h"

/* THE NAME IS AN ATOM, WHICH IS THE IDENTITY §9.5 MATCHES ON. It was a JSValue string compared by POINTER,
   with a comment claiming the interning made that immune to a page handing back an equal-but-distinct string.
   That claim is FALSE for a whole class of names: an atom whose characters are a decimal integer below 2^32 is
   stored TAGGED, and JS_AtomToString ALLOCATES A FRESH STRING for a tagged-int atom every time it is called —
   so two channels both named "42" held two different pointers and neither ever heard the other. A page that
   names its bus after a counter or a random integer, which is what the corpus does, silently had no bus at
   all. The atom IS the interning; comparing it is the comparison the comment described. */
typedef struct {
    JSAtom  name;      /* §9.5's channel name (owned — released in the finalizer) */
    uint8_t closed;    /* §9.5's closed flag */
} ChanData;

static JSClassID g_chan_class;
static JSValue   g_registry = JS_UNDEFINED;   /* the open channels, in creation order — see the file comment */
static JSValue   g_deliver_fn = JS_UNDEFINED;
static JSRuntime *g_bc_rt;
static int       g_ctor_stepid = -1;
static int       g_id_post = -1, g_id_close = -1;   /* the agent's pool entries; the objects are each realm's */
static char     *g_origin;

/* NO OWNED JSValues: the name is an atom, which is not a GC object, and it is written once at construction and
   never again — so the byte capture is the whole of what a flow can change here, which is `closed`. */
static const CowRecord CHAN_REC = { sizeof(ChanData), NULL, 0 };

/* The capture is in the accessor, as it is for every other component record here: `closed` is state a flow
   writes, and an arm that closes a channel must not close it for its sibling. */
static ChanData *chan_of(JSValueConst v)
{
    ChanData *c = JS_GetOpaque(v, g_chan_class);
    if (c) cow_capture_host_record(v, c, &CHAN_REC);
    return c;
}

static void chan_finalizer(JSRuntime *rt, JSValue val)
{
    ChanData *c = JS_GetOpaque(val, g_chan_class);
    if (!c) return;
    JS_FreeAtomRT(rt, c->name);
    free(c);
}

static int registry_len(JSContext *ctx, uint32_t *pn)
{
    JSValue len = JS_GetPropertyStr(ctx, g_registry, "length");
    *pn = 0;
    if (JS_IsException(len)) return 0;
    if (JS_ToUint32(ctx, pn, len) < 0) { JS_FreeValue(ctx, len); return 0; }
    JS_FreeValue(ctx, len);
    return 1;
}

/* ---- delivery ----------------------------------------------------------------------------------------------
 *
 * §9.5 step 6: one TASK per destination, on the DOM manipulation task source. The closed check happens IN the
 * task, not when the list is built — a channel closed between the post and its delivery receives nothing, and
 * the corpus asserts exactly that. */
static JSValue js_chan_deliver(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValueConst dest = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst buf  = argc > 1 ? argv[1] : JS_UNDEFINED;
    ChanData *c = chan_of(dest);
    StructuredData sd;
    JSValue data, ev;
    size_t blen = 0;

    (void)this_val;
    if (!c || c->closed)
        return JS_UNDEFINED;
    sd.buf = JS_GetArrayBuffer(ctx, &blen, buf);
    sd.len = blen;
    DCHECK(sd.buf != NULL, "a broadcast task held something that is not the bytes it stored");
    data = structured_deserialize(ctx, &sd);
    if (JS_IsException(data)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        ev = message_event_new(ctx, "messageerror", JS_UNDEFINED, g_origin, JS_UNDEFINED, JS_UNDEFINED);
    } else {
        /* §9.5: the origin is the SENDER'S, and a broadcast has no source and no ports — the bus is named, not
           addressed, so there is nothing to reply to. */
        ev = message_event_new(ctx, "message", data, g_origin, JS_UNDEFINED, JS_UNDEFINED);
        JS_FreeValue(ctx, data);
    }
    if (JS_IsException(ev)) return JS_EXCEPTION;
    event_target_fire(ctx, dest, ev);
    return JS_UNDEFINED;
}

/* ---- the members -------------------------------------------------------------------------------------------- */

static JSValue js_chan_name(JSContext *ctx, JSValueConst this_val, int magic)
{
    ChanData *c = chan_of(this_val);
    (void)magic;
    if (!c) return JS_ThrowTypeError(ctx, "not a BroadcastChannel");
    return JS_AtomToString(ctx, c->name);
}

static JSValue js_chan_post(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    ChanData *c = chan_of(this_val);
    StructuredData sd;
    JSValue buf;
    uint32_t n = 0, i;

    (void)magic;
    if (!c) return JS_ThrowTypeError(ctx, "not a BroadcastChannel");
    /* §9.5 step 1: posting on a closed channel is an InvalidStateError — not a silent no-op, because a page
       that closed and kept posting has a bug it should be told about. */
    if (c->closed)
        return JS_ThrowDOMException(ctx, "InvalidStateError", "the channel is closed");
    if (structured_serialize(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &sd) < 0)
        return JS_EXCEPTION;
    buf = JS_NewArrayBufferCopy(ctx, sd.buf, sd.len);
    structured_data_free(ctx, &sd);
    if (JS_IsException(buf)) return JS_EXCEPTION;

    if (!registry_len(ctx, &n)) { JS_FreeValue(ctx, buf); return JS_EXCEPTION; }
    for (i = 0; i < n; i++) {
        JSValue d = JS_GetPropertyUint32(ctx, g_registry, i);
        ChanData *dc;
        if (JS_IsException(d)) { JS_FreeValue(ctx, buf); return JS_EXCEPTION; }
        dc = JS_GetOpaque(d, g_chan_class);
        /* §9.5 step 4: same name, still open, and NOT the sender. The identity test is what makes a page that
           posts on its own channel not hear itself, which is the first thing every user of this relies on. */
        if (dc && !dc->closed && JS_VALUE_GET_PTR(d) != JS_VALUE_GET_PTR(this_val) &&
            dc->name == c->name) {
            JSValueConst args[2];
            args[0] = d;
            args[1] = buf;
            JS_EnqueueCallTask(ctx, g_deliver_fn, 2, args);
        }
        JS_FreeValue(ctx, d);
    }
    JS_FreeValue(ctx, buf);
    return JS_UNDEFINED;
}

static JSValue js_chan_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    ChanData *c = chan_of(this_val);
    (void)argc; (void)argv; (void)magic;
    if (!c) return JS_ThrowTypeError(ctx, "not a BroadcastChannel");
    c->closed = 1;
    return JS_UNDEFINED;
}

/* ---- the constructor ---------------------------------------------------------------------------------------- */

typedef struct { uint8_t unused; } JSBcCtorState;
static void js_bc_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }
static void js_bc_release(JSContext *ctx, void *st) { (void)ctx; (void)st; }

static int js_bc_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                           JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSValue obj;
    ChanData *c;
    uint32_t n = 0;

    (void)st; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    if (JS_IsUndefined(hdr->this_val))
        return JS_ThrowTypeError(ctx, "constructor BroadcastChannel requires 'new'"), -1;
    /* §9.5: `constructor(DOMString name)` — one REQUIRED argument, so a bare `new BroadcastChannel()` is the
       TypeError Web IDL raises for a missing required argument. The declaration converts it, so what arrives
       here is already a string however the page spelled it. */
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "BroadcastChannel requires a name"), -1;
    {
        JSValue proto = JS_GetClassProto(ctx, g_chan_class);
        DCHECK(!JS_IsNull(proto), "a BroadcastChannel was constructed in a realm that never ran its install");
        obj = JS_NewObjectProtoClass(ctx, proto, g_chan_class);
        JS_FreeValue(ctx, proto);
    }
    if (JS_IsException(obj)) return -1;
    c = calloc(1, sizeof *c);
    CHECK(c != NULL, "broadcast channel: OOM building a BroadcastChannel");
    /* THE DECLARATION ALREADY CONVERTED IT, and this asserts that rather than trusting it. §9.5 declares
       `constructor(DOMString name)`, so the args machine ran §3.2's ToString — as a FLOW, which is the only
       place a page's `toString` may run — and what arrives here is a string however the page spelled it.
       Reaching for the characters from a C activation instead runs that `toString` from C, which the
       interpreter refuses: it surfaced as JS_ToPrimitiveFree aborting a popup test, three frames below this
       line, with nothing naming the contract that had been broken. A claim in a comment is not a check. */
    DCHECK(JS_IsString(argv[0]),
           "BroadcastChannel's name reached its body unconverted — §9.5 declares it DOMString and the args "
           "machine converts a declared member's arguments before the body runs, so an object here means "
           "this member's declaration is not the one that was invoked");
    /* INTERNED STRAIGHT FROM THE CONVERTED STRING — no C string in between, which is also what keeps a name
       containing a NUL or a lone surrogate the name the page gave rather than a truncation of it. */
    c->name = JS_ValueToAtom(ctx, argv[0]);
    if (c->name == JS_ATOM_NULL) { free(c); JS_FreeValue(ctx, obj); return -1; }
    JS_SetOpaque(obj, c);
    /* CREATION ORDER is the delivery order §9.5 states, so the registry is appended to and never reordered. */
    if (!registry_len(ctx, &n)) { JS_FreeValue(ctx, obj); return -1; }
    JS_SetPropertyUint32(ctx, g_registry, n, JS_DupValue(ctx, obj));
    *presult = obj;
    return 0;
}

static const IdlStepDecl js_bc_ctor_decl = {
    js_bc_ctor_step, sizeof(JSBcCtorState), js_bc_visit, js_bc_release
};

/* ---- install -------------------------------------------------------------------------------------------- */

void broadcast_channel_init(JSContext *ctx, const char *origin)
{
    /* NO gc_mark: the record holds no JSValue, so there is nothing for the collector to trace through it. */
    JSClassDef d = { "BroadcastChannel", .finalizer = chan_finalizer };
    JSRuntime *rt = JS_GetRuntime(ctx);
    static const IdlArgType CTOR_ARGS[1] = { IDL_DOMSTRING };

    DCHECK(g_bc_rt == NULL || g_bc_rt == rt, "BroadcastChannel was installed into a second runtime");
    if (g_bc_rt == rt) return;
    g_bc_rt = rt;
    free(g_origin);
    g_origin = strdup(origin ? origin : "null");
    CHECK(g_origin != NULL, "broadcast channel: OOM recording this document's origin");
    JS_NewClassID(rt, &g_chan_class);
    JS_NewClass(rt, g_chan_class, &d);

    g_registry = JS_NewArray(ctx);
    CHECK(!JS_IsException(g_registry), "the BroadcastChannel registry could not be allocated");
    g_deliver_fn = JS_NewCFunction(ctx, js_chan_deliver, "", 2);
    CHECK(JS_IsFunction(ctx, g_deliver_fn), "the broadcast delivery task's callee could not be allocated");

    {
        static const IdlArgType POST_ARGS[1] = { IDL_ANY };
        g_id_post  = idl_method_id(ctx, POST_ARGS, 1, js_chan_post, 0);
        g_id_close = idl_method_id(ctx, NULL, 0, js_chan_close, 0);
    }
    g_ctor_stepid = idl_method_id_step(ctx, CTOR_ARGS, 1, NULL, 0, &js_bc_ctor_decl, 0);
    realm_declare_intrinsic(broadcast_channel_install_proto);
}

/* §9.5's INTERFACE PROTOTYPE OBJECT, FOR ONE REALM — §3.7 gives every realm its own, and here it decides the
   ANSWER: a `postMessage` shared between documents would run with the defining realm's ctx, so every channel's
   broadcast would carry that document's origin rather than its own. */
void broadcast_channel_install_proto(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_chan_class != 0, "a realm asked for BroadcastChannel.prototype before the class was declared");
    prev = JS_GetClassProto(ctx, g_chan_class);
    DCHECK(JS_IsNull(prev), "broadcast_channel_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "BroadcastChannel.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "BroadcastChannel");
    /* §9.5: `interface BroadcastChannel : EventTarget`, with the same two handler attributes a port has. */
    event_target_chain(ctx, proto);   /* §9.5: `BroadcastChannel : EventTarget` */
    event_target_install_handlers(ctx, proto, EH_PORT);
    idl_install_accessor(ctx, proto, "name", js_chan_name, 0, -1);
    idl_install_method(ctx, proto, "postMessage", 1, g_id_post);
    idl_install_method(ctx, proto, "close", 0, g_id_close);
    JS_SetClassProto(ctx, g_chan_class, proto);
}

void broadcast_channel_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;

    DCHECK(g_ctor_stepid >= 0, "BroadcastChannel was installed before broadcast_channel_init declared it");
    ctor = idl_step_constructor(ctx, "BroadcastChannel", 1, g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the BroadcastChannel interface object could not be allocated");
    {
        JSValue proto = JS_GetClassProto(ctx, g_chan_class);
        DCHECK(!JS_IsNull(proto), "BroadcastChannel was installed into a realm that never ran its proto build");
        JS_SetConstructor(ctx, ctor, proto);
        JS_FreeValue(ctx, proto);
    }
    JS_SetPropertyStr(ctx, (JSValue)global, "BroadcastChannel", ctor);
}

void broadcast_channel_free(JSContext *ctx)
{
    if (!g_bc_rt) return;
    JS_FreeValue(ctx, g_registry);
    JS_FreeValue(ctx, g_deliver_fn);
    g_registry = g_deliver_fn = JS_UNDEFINED;   /* the prototypes are the REALMS' — released with their contexts */
    free(g_origin);
    g_origin = NULL;
    g_bc_rt = NULL;
    g_ctor_stepid = -1;
}
