/* MessagePort AND MessageChannel — HTML §9.4.2 and §9.4.3.
 *
 * WHAT A PORT IS. Two ports are ENTANGLED: what you post into one is delivered at the other. That is the whole
 * of the interface, and it is the local half of every cross-document channel — an iframe or a popup is a second
 * document and therefore, under this engine's one-WASM-instance-per-document rule, a second agent; a port pair
 * is what a message crosses. Building the same-agent pair first is not a step toward the cross-agent one, it IS
 * the cross-agent one minus the transport: the queue, the enabling, the serialization and the delivery event
 * are identical, and what changes is where the second port lives.
 *
 * A MESSAGE IS SERIALIZED AT THE POST AND DESERIALIZED AT THE DELIVERY, which is why structured_clone.h has two
 * operations rather than one. The DataCloneError for an unclonable message is thrown at `postMessage` — the
 * caller sees it where it wrote its try/catch — while the copy the receiver gets is built in the delivery task.
 * A port that handed the same object reference across would not be a port; it would be a shared variable, and
 * the standard's own example of why is a page mutating the object after posting it.
 *
 * THE PORT MESSAGE QUEUE IS A TASK SOURCE, and it starts DISABLED. Nothing is delivered until `start()` runs or
 * an `onmessage` handler is set — that setter enabling the queue is a real HTML rule, not a convenience, and a
 * page that only calls addEventListener('message') and never start() receives nothing. Everything already
 * queued is delivered when the queue is enabled, in order.
 *
 * WHAT IS NOT HERE. TRANSFERRING a port through another port (`postMessage(m, [port])`) needs
 * StructuredSerializeWithTransfer, which structured_clone.c does not have; the transfer list is refused with
 * the DataCloneError a non-transferable value would get. That is the honest answer while transfer does not
 * exist, and it is what makes MessageEvent's `ports` still always empty. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_args.h"
#include "core/structured_clone.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/events/message_event.h"
#include "core/events/message_port.h"
#include "solver/cow.h"

typedef struct {
    JSValue entangled;   /* §9.4.2's entangled port, or JS_UNDEFINED once closed (owned) */
    /* §9.4.2's PORT MESSAGE QUEUE: an Array of ArrayBuffers, each one message as StructuredSerialize left it,
       and `head` how many have been taken. §4.2's stream queue is the same shape for the same reasons, and the
       reasons are why this is NOT the malloc'd linked list it started as:
         - A QUEUED MESSAGE IS FRONTIER DATA. It is immutable, it belongs to the flow that will read it, and it
           has to survive being parked to disk and resumed in another session. A malloc'd node can do none of
           that; a JS value is exactly what the snapshot machinery already carries.
         - IT HAS TO TIME-TRAVEL, and an Array does that for free — its mutations are property writes the COW
           delta already captures, so one forked arm's post is invisible to its sibling. The record's own head
           index rides the record capture. A raw `head`/`tail` pointer pair captured as bytes reverted the
           POINTERS on a context switch and left the NODES reachable from nothing: every message a flow queued
           was leaked when its delta was freed, and being C memory rather than GC objects, the runtime's own
           leak walk could not see it.
         - The collector owns the bytes, so there is no second ownership contract to get wrong. */
    JSValue queue;
    uint32_t head;
    uint8_t enabled;     /* §9.4.2: the queue starts DISABLED */
    uint8_t detached;    /* §9.4.2's [[Detached]], set by close() */
} PortData;

static JSClassID g_port_class;
static JSValue   g_port_proto = JS_UNDEFINED;
static JSValue   g_chan_proto = JS_UNDEFINED;
static JSRuntime *g_mp_rt;
static int       g_chan_ctor_stepid = -1;
static JSValue   g_deliver_fn = JS_UNDEFINED;   /* the queued delivery task's callee */

/* THE RECORD TIME-TRAVELS. `enabled` and the queue head are state a flow writes where no property hook can see
   — a forked arm that called start() would have enabled the queue for its sibling, and an arm that received a
   message would have consumed it for every other. The capture is in the ACCESSOR for the reason the streams
   components give: a record a flow has reached is one it may write, and there is then no write site to miss.
   The offset list is the same list the finalizer frees. */
#define MP_OFF(f) (uint16_t)offsetof(PortData, f)
static const uint16_t PORT_VALS[] = { MP_OFF(entangled), MP_OFF(queue) };
static const CowRecord PORT_REC = { sizeof(PortData), PORT_VALS, 2 };

/* HOW MANY MESSAGES THE QUEUE HOLDS, building the Array on first use. Lazy because most ports never receive
   one and an Array per port is not free; the record's slot is JS_UNDEFINED until then. Returns 0 with an
   exception live. */
static int port_queue_len(JSContext *ctx, PortData *d, uint32_t *pn)
{
    JSValue len;
    if (JS_IsUndefined(d->queue)) {
        d->queue = JS_NewArray(ctx);
        if (JS_IsException(d->queue)) { d->queue = JS_UNDEFINED; return 0; }
    }
    len = JS_GetPropertyStr(ctx, d->queue, "length");
    if (JS_IsException(len)) return 0;
    *pn = 0;
    if (JS_ToUint32(ctx, pn, len) < 0) { JS_FreeValue(ctx, len); return 0; }
    JS_FreeValue(ctx, len);
    return 1;
}

static PortData *port_of(JSValueConst v)
{
    PortData *p = JS_GetOpaque(v, g_port_class);
    if (p) cow_capture_host_record(v, p, &PORT_REC);
    return p;
}

bool message_port_is(JSValueConst v)
{
    return g_port_class != 0 && JS_GetOpaque(v, g_port_class) != NULL;
}

static void port_finalizer(JSRuntime *rt, JSValue val)
{
    PortData *d = JS_GetOpaque(val, g_port_class);
    if (!d) return;
    JS_FreeValueRT(rt, d->entangled);
    JS_FreeValueRT(rt, d->queue);
    free(d);
}

static void port_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    PortData *d = JS_GetOpaque(val, g_port_class);
    if (!d) return;
    /* THE PAIR IS A CYCLE: each port holds the other. Without this, an entangled pair is unreachable to the
       collector and every MessageChannel a page makes is a leak the runtime's own walk reports. */
    JS_MarkValue(rt, d->entangled, mark_func);
    JS_MarkValue(rt, d->queue, mark_func);
}

/* ---- delivery ----------------------------------------------------------------------------------------------
 *
 * §9.4.2's delivery task: deserialize, then fire `message` at the port. It is a TASK on the port message queue
 * — HTML's own words — so it is enqueued through the task half of the event loop and every microtask
 * outstanding runs before it, which is what a page observes when it posts and then awaits. */

static JSValue js_port_deliver(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValueConst port = argc > 0 ? argv[0] : JS_UNDEFINED;
    PortData *d = port_of(port);
    JSValue data, ev, entry, buf, ports = JS_UNDEFINED;
    StructuredWithTransfer swt;
    uint32_t n = 0;
    size_t blen = 0;

    (void)this_val;
    /* The port may have been closed, or its queue disabled again, between the enqueue and now — the task is in
       the queue and the state is the port's, so the state is what decides. */
    if (!d || !d->enabled || !port_queue_len(ctx, d, &n) || d->head >= n)
        return JS_UNDEFINED;
    entry = JS_GetPropertyUint32(ctx, d->queue, d->head);
    d->head++;
    /* The Array's own elements, read with no page code in the way: this component built them and nothing else
       can reach them. The bytes are BORROWED for the deserialize — the buffer owns them. */
    buf = JS_GetPropertyUint32(ctx, entry, 0);
    swt.holders = JS_GetPropertyUint32(ctx, entry, 1);
    JS_FreeValue(ctx, entry);
    swt.data.buf = JS_GetArrayBuffer(ctx, &blen, buf);
    swt.data.len = blen;
    DCHECK(swt.data.buf != NULL, "a port's queue held something that is not the serialized message it stored");
    data = structured_deserialize_transfer(ctx, &swt, &ports);
    JS_FreeValue(ctx, buf);
    JS_FreeValue(ctx, swt.holders);
    if (JS_IsException(data)) {
        /* §9.4.2: a message that cannot be deserialized fires `messageerror` rather than `message`. This
           engine's deserializer only fails where its own writer and reader disagree, which crashes instead —
           so reaching here means the exception came from somewhere else and is the page's to see. */
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, ports);
        ev = message_event_new(ctx, "messageerror", JS_UNDEFINED, "", JS_UNDEFINED, JS_UNDEFINED);
    } else {
        /* §9.4.2: `ports` is the [[TransferredValues]] — the ports that ARRIVED with this message. */
        ev = message_event_new(ctx, "message", data, "", JS_UNDEFINED, ports);
        JS_FreeValue(ctx, data);
        JS_FreeValue(ctx, ports);
    }
    if (JS_IsException(ev))
        return JS_EXCEPTION;
    event_target_fire(ctx, port, ev);
    return JS_UNDEFINED;
}

/* Enqueue one delivery task for `port`. One task per message, so a queue enabled with three messages in it
   delivers three times and each is its own turn of the event loop, which is what the standard's "add a task"
   per message means. */
static void port_enqueue_delivery(JSContext *ctx, JSValueConst port)
{
    JSValueConst args[1];
    args[0] = port;
    DCHECK(JS_IsFunction(ctx, g_deliver_fn), "a port delivery was queued before message_port_init built it");
    JS_EnqueueCallTask(ctx, g_deliver_fn, 1, args);
}

/* §9.4.2's "enable this's port message queue": everything already in it becomes a task, in order. */
static void port_enable(JSContext *ctx, JSValueConst port, PortData *d)
{
    uint32_t n = 0, i;
    if (d->enabled) return;
    d->enabled = 1;
    if (!port_queue_len(ctx, d, &n)) return;
    for (i = d->head; i < n; i++)
        port_enqueue_delivery(ctx, port);
}

/* ---- the members -------------------------------------------------------------------------------------------- */

/* §9.4.2's message port post message steps. `transfer` is refused for the reason the file comment gives. */
static JSValue js_port_post(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    PortData *d = port_of(this_val), *t;
    JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValueConst target;
    StructuredWithTransfer swt;
    JSValue list;

    (void)magic;
    if (!d)
        return JS_ThrowTypeError(ctx, "not a MessagePort");
    /* §9.4.2's second argument is either the transfer SEQUENCE or a StructuredSerializeOptions whose
       `transfer` member is one — the two overloads the standard still carries. */
    list = JS_IsArray(opts) ? JS_DupValue(ctx, opts)
         : JS_IsObject(opts) ? JS_GetPropertyStr(ctx, opts, "transfer") : JS_UNDEFINED;
    if (JS_IsException(list)) return JS_EXCEPTION;

    /* §9.4.2 STEP 2: a transfer list naming THIS port is a DataCloneError. The check is here rather than in the
       transfer steps because only the post knows which port it is going through, and it must happen before
       anything is detached — a page that catches this error still has a working port. */
    if (JS_IsObject(list)) {
        uint32_t n = 0, i;
        JSValue len = JS_GetPropertyStr(ctx, list, "length");
        if (JS_IsException(len)) { JS_FreeValue(ctx, list); return JS_EXCEPTION; }
        if (JS_ToUint32(ctx, &n, len) < 0) { JS_FreeValue(ctx, len); JS_FreeValue(ctx, list); return JS_EXCEPTION; }
        JS_FreeValue(ctx, len);
        for (i = 0; i < n; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, list, i);
            bool self = JS_VALUE_GET_PTR(e) == JS_VALUE_GET_PTR(this_val);
            JS_FreeValue(ctx, e);
            if (self) {
                JS_FreeValue(ctx, list);
                return JS_ThrowDOMException(ctx, "DataCloneError",
                                            "a port cannot be transferred through itself");
            }
        }
    }

    /* §9.4.2 step 5: SERIALIZE NOW, and run the transfer steps. The throw belongs to this call, not to the
       delivery task — a page's try/catch around postMessage is where the standard puts the DataCloneError. It
       happens even when there is no target, which is why it precedes the null check. */
    if (structured_serialize_transfer(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, list, &swt) < 0) {
        JS_FreeValue(ctx, list);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, list);

    /* §9.4.2 step 6: a port with nothing on the other end posts into nowhere, and that is not an error. The
       transfer has still HAPPENED — the standard detaches before it looks at the target, and a page that
       transfers into a closed port does not get its port back. */
    target = d->entangled;
    t = JS_IsUndefined(target) ? NULL : port_of(target);
    if (!t) { structured_with_transfer_free(ctx, &swt); return JS_UNDEFINED; }

    {
        /* ONE QUEUE ENTRY IS THE MESSAGE AND ITS TRANSFER HOLDERS. The bytes become an ArrayBuffer the collector
           owns — see the queue's comment; NewArrayBufferCopy rather than adopting the block, because the block
           is js_malloc'd and an adopting ArrayBuffer frees with its own allocator. */
        JSValue buf = JS_NewArrayBufferCopy(ctx, swt.data.buf, swt.data.len);
        JSValue entry;
        uint32_t n = 0;
        if (JS_IsException(buf)) { structured_with_transfer_free(ctx, &swt); return JS_EXCEPTION; }
        entry = JS_NewArray(ctx);
        if (JS_IsException(entry)) { JS_FreeValue(ctx, buf); structured_with_transfer_free(ctx, &swt); return JS_EXCEPTION; }
        JS_SetPropertyUint32(ctx, entry, 0, buf);
        JS_SetPropertyUint32(ctx, entry, 1, JS_DupValue(ctx, swt.holders));
        structured_with_transfer_free(ctx, &swt);
        if (!port_queue_len(ctx, t, &n)) { JS_FreeValue(ctx, entry); return JS_EXCEPTION; }
        JS_SetPropertyUint32(ctx, t->queue, n, entry);
    }
    if (t->enabled)
        port_enqueue_delivery(ctx, target);
    return JS_UNDEFINED;
}

/* §9.4.2's `start()`. */
static JSValue js_port_start(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    PortData *d = port_of(this_val);
    (void)argc; (void)argv; (void)magic;
    if (!d) return JS_ThrowTypeError(ctx, "not a MessagePort");
    port_enable(ctx, this_val, d);
    return JS_UNDEFINED;
}

/* §9.4.2's `close()`: detach, and disentangle the pair — BOTH sides, because entanglement is symmetric and a
   port left pointing at a closed one would keep queueing messages nobody will ever read. */
static JSValue js_port_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    PortData *d = port_of(this_val);
    (void)argc; (void)argv; (void)magic;
    if (!d) return JS_ThrowTypeError(ctx, "not a MessagePort");
    d->detached = 1;
    if (!JS_IsUndefined(d->entangled)) {
        PortData *o = port_of(d->entangled);
        JSValue other = d->entangled;
        d->entangled = JS_UNDEFINED;
        if (o) { JS_FreeValue(ctx, o->entangled); o->entangled = JS_UNDEFINED; }
        JS_FreeValue(ctx, other);
    }
    return JS_UNDEFINED;
}

/* §9.4.2: setting `onmessage` ENABLES the port message queue. It is the platform's one event handler attribute
   whose setter has a side effect, and it is why a page that assigns onmessage never has to call start() while
   a page that only uses addEventListener('message') must. The BRAND TEST is here rather than in event_target.c
   so that file does not have to know what a MessagePort is — every other target's handlers reach this and are
   left alone. */
static void port_handler_set(JSContext *ctx, JSValueConst target, const char *name)
{
    PortData *d;
    if (strcmp(name, "onmessage") != 0)
        return;
    d = port_of(target);   /* NULL for every target that is not a port, which is most of them */
    if (d)
        port_enable(ctx, target, d);
}

/* ---- §9.4.2's TRANSFER STEPS ---------------------------------------------------------------------------------
 *
 * A transferred port is MOVED, not copied: the queue and the entanglement leave the old object and arrive at a
 * new one, and the old one is detached. The holder is a three-element Array — the queue, how much of it has
 * been taken, and the port on the other end — because a holder is a value that has to ride the frontier like
 * everything else, and a JS Array is what the snapshot machinery already carries.
 *
 * THE REMOTE IS DISENTANGLED HERE AND RE-ENTANGLED AT THE RECEIVE, which is the standard's own order and the
 * reason it works: between the two steps the remote is entangled with nothing, so a message posted into it in
 * that window is queued on it and delivered when the new port arrives — which is exactly what the corpus's
 * "outgoing messages sent at each transfer step are received in order" asserts. */

enum { TH_QUEUE = 0, TH_HEAD, TH_REMOTE, TH_N };

static JSValue port_transfer_out(JSContext *ctx, JSValueConst v)
{
    PortData *d = port_of(v);
    JSValue h;

    DCHECK(d != NULL, "the port transfer steps ran on something that is not a MessagePort");
    /* §9.4.2: transferring a detached port is a DataCloneError. So is transferring the port a message is being
       posted THROUGH, which the caller checks — it has the source in hand and this does not. */
    if (d->detached)
        return JS_ThrowDOMException(ctx, "DataCloneError", "the port has been closed");
    h = JS_NewArray(ctx);
    if (JS_IsException(h)) return h;
    JS_SetPropertyUint32(ctx, h, TH_QUEUE, JS_DupValue(ctx, d->queue));
    JS_SetPropertyUint32(ctx, h, TH_HEAD, JS_NewUint32(ctx, d->head));
    JS_SetPropertyUint32(ctx, h, TH_REMOTE, JS_DupValue(ctx, d->entangled));
    /* The old object keeps nothing: its queue has moved and it is detached. */
    JS_FreeValue(ctx, d->queue);
    d->queue = JS_UNDEFINED;
    d->head = 0;
    d->enabled = 0;
    d->detached = 1;
    if (!JS_IsUndefined(d->entangled)) {
        PortData *o = port_of(d->entangled);
        if (o) { JS_FreeValue(ctx, o->entangled); o->entangled = JS_UNDEFINED; }
        JS_FreeValue(ctx, d->entangled);
        d->entangled = JS_UNDEFINED;
    }
    return h;
}

static JSValue port_new(JSContext *ctx);

static JSValue port_transfer_in(JSContext *ctx, JSValueConst holder)
{
    JSValue obj = port_new(ctx), remote;
    PortData *d;
    uint32_t head = 0;

    if (JS_IsException(obj)) return obj;
    d = JS_GetOpaque(obj, g_port_class);
    d->queue = JS_GetPropertyUint32(ctx, holder, TH_QUEUE);
    { JSValue hv = JS_GetPropertyUint32(ctx, holder, TH_HEAD);
      JS_ToUint32(ctx, &head, hv);
      JS_FreeValue(ctx, hv); }
    d->head = head;
    /* §9.4.2: the arriving port's queue starts DISABLED however full it is — the receiver must start() it or
       set onmessage, exactly as for a port it made itself. */
    d->enabled = 0;
    remote = JS_GetPropertyUint32(ctx, holder, TH_REMOTE);
    if (!JS_IsUndefined(remote)) {
        PortData *o = port_of(remote);
        d->entangled = JS_DupValue(ctx, remote);
        if (o) {
            JS_FreeValue(ctx, o->entangled);
            o->entangled = JS_DupValue(ctx, obj);
        }
    }
    JS_FreeValue(ctx, remote);
    return obj;
}

static const StructuredTransferable PORT_TRANSFERABLE = {
    message_port_is, port_transfer_out, port_transfer_in
};

/* ---- MessageChannel ------------------------------------------------------------------------------------------ */

static JSValue port_new(JSContext *ctx)
{

    JSValue obj = JS_NewObjectProtoClass(ctx, g_port_proto, g_port_class);
    PortData *d;

    if (JS_IsException(obj)) return obj;
    d = calloc(1, sizeof *d);
    CHECK(d != NULL, "message port: OOM building a MessagePort");
    d->entangled = d->queue = JS_UNDEFINED;
    JS_SetOpaque(obj, d);
    return obj;
}

JSValue message_port_pair(JSContext *ctx, JSValue *port2)
{
    JSValue p1 = port_new(ctx), p2;
    PortData *d1, *d2;

    if (JS_IsException(p1)) return p1;
    p2 = port_new(ctx);
    if (JS_IsException(p2)) { JS_FreeValue(ctx, p1); return p2; }
    /* ENTANGLE. Each holds the other, which is the cycle port_gc_mark exists for. */
    d1 = JS_GetOpaque(p1, g_port_class);
    d2 = JS_GetOpaque(p2, g_port_class);
    d1->entangled = JS_DupValue(ctx, p2);
    d2->entangled = JS_DupValue(ctx, p1);
    *port2 = p2;
    return p1;
}

typedef struct { uint8_t unused; } JSChanCtorState;
static void js_chan_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }
static void js_chan_release(JSContext *ctx, void *st) { (void)ctx; (void)st; }

/* §9.4.3's `new MessageChannel()`. It takes no arguments and reads nothing of the page's, so its body runs to
   completion; it is declared through the IDL machine because that is how a constructor is declared here. */
static int js_chan_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                             JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSValue obj, p1, p2;

    (void)st; (void)argc; (void)argv; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    if (JS_IsUndefined(hdr->this_val))
        return JS_ThrowTypeError(ctx, "constructor MessageChannel requires 'new'"), -1;
    obj = JS_NewObjectProto(ctx, g_chan_proto);
    if (JS_IsException(obj)) return -1;
    p1 = message_port_pair(ctx, &p2);
    if (JS_IsException(p1)) { JS_FreeValue(ctx, obj); return -1; }
    /* [SameObject] readonly attributes: the same two ports every time, so they are OWN data properties rather
       than accessors over a record — a MessageChannel has no state beyond the pair. */
    JS_DefinePropertyValueStr(ctx, obj, "port1", p1, 0);
    JS_DefinePropertyValueStr(ctx, obj, "port2", p2, 0);
    *presult = obj;
    return 0;
}

static const IdlStepDecl js_chan_ctor_decl = {
    js_chan_ctor_step, sizeof(JSChanCtorState), js_chan_visit, js_chan_release
};

/* ---- install -------------------------------------------------------------------------------------------- */

void message_port_init(JSContext *ctx)
{
    JSClassDef pd = { "MessagePort", .finalizer = port_finalizer, .gc_mark = port_gc_mark };
    JSRuntime *rt = JS_GetRuntime(ctx);

    DCHECK(g_mp_rt == NULL || g_mp_rt == rt, "MessagePort was installed into a second runtime");
    if (g_mp_rt == rt) return;
    g_mp_rt = rt;
    JS_NewClassID(rt, &g_port_class);
    JS_NewClass(rt, g_port_class, &pd);

    g_port_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_port_proto), "MessagePort.prototype could not be allocated");
    idl_interface_tag(ctx, g_port_proto, "MessagePort");
    /* §9.4.2: `interface MessagePort : EventTarget` — the listeners and the two handler attributes. */
    event_target_install(ctx, g_port_proto);
    event_target_install_handlers(ctx, g_port_proto, EH_PORT);
    {
        static const IdlArgType POST_ARGS[2] = { IDL_ANY, IDL_ANY };
        idl_install_method(ctx, g_port_proto, "postMessage", 1,
                           idl_method_id(ctx, POST_ARGS, 2, js_port_post, 0));
        idl_optional_from(1);   /* `postMessage(any message, optional StructuredSerializeOptions options)` */
        idl_install_method(ctx, g_port_proto, "start", 0, idl_method_id(ctx, NULL, 0, js_port_start, 0));
        idl_install_method(ctx, g_port_proto, "close", 0, idl_method_id(ctx, NULL, 0, js_port_close, 0));
    }

    g_chan_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_chan_proto), "MessageChannel.prototype could not be allocated");
    idl_interface_tag(ctx, g_chan_proto, "MessageChannel");
    g_chan_ctor_stepid = idl_method_id_step(ctx, NULL, 0, NULL, 0, &js_chan_ctor_decl, 0);

    g_deliver_fn = JS_NewCFunction(ctx, js_port_deliver, "", 1);
    CHECK(JS_IsFunction(ctx, g_deliver_fn), "the port delivery task's callee could not be allocated");
    event_target_set_handler_hook(port_handler_set);
    /* §9.4.2's interface is [Transferable], and this is where it says so. */
    structured_register_transferable(&PORT_TRANSFERABLE);
}

void message_port_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;

    DCHECK(g_chan_ctor_stepid >= 0, "MessageChannel was installed before message_port_init declared it");
    ctor = JS_NewCFunction2(ctx, NULL, "MessagePort", 0, JS_CFUNC_constructor, 0);
    CHECK(!JS_IsException(ctor), "the MessagePort interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, g_port_proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "MessagePort", ctor);

    ctor = idl_step_constructor(ctx, "MessageChannel", 0, g_chan_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the MessageChannel interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, g_chan_proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "MessageChannel", ctor);
}

void message_port_free(JSContext *ctx)
{
    if (!g_mp_rt) return;
    JS_FreeValue(ctx, g_port_proto);
    JS_FreeValue(ctx, g_chan_proto);
    JS_FreeValue(ctx, g_deliver_fn);
    g_port_proto = g_chan_proto = g_deliver_fn = JS_UNDEFINED;
    g_mp_rt = NULL;
    g_chan_ctor_stepid = -1;
}
