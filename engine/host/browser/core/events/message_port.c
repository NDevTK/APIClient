/* MessagePort AND MessageChannel — HTML §9.4.4 "Message ports" and §9.4.2 "Message channels".
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
 * A PORT CROSSES A PORT. §9.4.4's transfer steps and transfer-receiving steps are below: `postMessage(m,
 * [port])` MOVES that port's queue and its entanglement onto a new object at the other end and detaches the
 * one that was sent, which is what fills the delivered event's `ports`. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/structured_clone.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/events/message_event.h"
#include "core/events/message_port.h"
#include "solver/concolic.h"
#include "solver/cow.h"

typedef struct {
    JSValue entangled;   /* §9.4.4's entangled port, or JS_UNDEFINED once closed (owned) */
    /* §9.4.4's PORT MESSAGE QUEUE: an Array of ArrayBuffers, each one message as StructuredSerialize left it,
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
    /* §9.4.4's RELEVANT REALM — the realm this port was created in. §9.4.4's delivery creates its
       MessageEvent "in the relevant realm of the port", and the delivery task's callee is ONE function object
       for the agent, so the ctx it runs under is whichever realm minted that callee and never the port's. A
       port handed to a child document delivered every message as an event belonging to the parent.
       BORROWED: the agent owns its realms and releases them with itself, exactly as a WindowProxy's is. */
    JSContext *realm;
    uint8_t enabled;     /* §9.4.4: the queue starts DISABLED */
    uint8_t detached;    /* §9.4.4's [[Detached]], set by close() */
} PortData;

static JSClassID g_port_class;
/* §9.4.2's MessageChannel HOLDS ITS TWO PORTS, which is the whole of its state: the constructor's steps are
   "Set this's port 1 to a new MessagePort", the same for port 2, and entangle them, and the `port1` getter
   steps are "return this's port 1". They are INTERNAL SLOTS, so they live in the class's record.
   THE COMMENT HERE SAID THE CLASS "holds nothing in it" AND THAT A MessageChannel "has no state beyond the
   pair" — the pair IS the state, and with nowhere to keep it the ports could only be own DATA properties of
   the channel, which is Web IDL §3.7.6 wrong twice over: an attribute is an ACCESSOR, and a regular attribute
   of an interface that is not [Global] lives on the INTERFACE PROTOTYPE OBJECT rather than on the instance.
   Both are page-observable, and the enumerability is the one to state carefully: at flags 0 the own property
   was NOT enumerable, and `Object.keys(new MessageChannel())` is `[]` in a browser too — because the members
   are on the PROTOTYPE, not because they are hidden. What differs is `for (const k in chan)`, which walks the
   chain and yields `port1` and `port2` in a browser and nothing here, and
   `Object.getOwnPropertyDescriptor(MessageChannel.prototype, "port1")`, which is a getter in a browser and
   was `undefined` here.
   THE RECORD IS WRITTEN ONCE, BY THE CONSTRUCTING FLOW, AND NEVER AGAIN — both members are readonly and there
   is no setter — so it needs no COW capture: the delta captures state a flow WRITES, and the object is
   flow-local at the one moment its ports are stored. */
typedef struct { JSValue port1, port2; } ChanData;

static JSClassID g_chan_class;
static JSRuntime *g_mp_rt;
static int       g_chan_ctor_stepid = -1;
static int       g_id_post = -1, g_id_start = -1, g_id_close = -1;   /* the AGENT's pool entries */
static int       g_deliver_stepid = -1;         /* §9.4.4's delivery task, as a machine */
static JSValue   g_deliver_fn = JS_UNDEFINED;   /* the queued delivery task's callee */

/* THE RECORD TIME-TRAVELS. `enabled` and the queue head are state a flow writes where no property hook can see
   — a forked arm that called start() would have enabled the queue for its sibling, and an arm that received a
   message would have consumed it for every other. The capture is in the ACCESSOR for the reason the streams
   components give: a record a flow has reached is one it may write, and there is then no write site to miss.
   The offset list is the same list the finalizer frees. */
#define MP_OFF(f) (uint16_t)offsetof(PortData, f)
static const uint16_t PORT_VALS[] = { MP_OFF(entangled), MP_OFF(queue) };
static const CowRecord PORT_REC = { sizeof(PortData), PORT_VALS, 2 };

/* WRITE ONE OF THE TWO, and never `JS_FreeValue(ctx, d->f); d->f = <build one>;` — see cow.h for the order and
   the defect. Both slots make the release side of it real: giving back `queue` releases every message still on
   it and giving back `entangled` may drop the last reference to the peer port, and a host finalizer reached
   either way is the page's platform code, which may allocate — and an allocation IS a collection (js_trigger_gc
   has exactly one caller, JS_NewObjectFromShape), which re-enters this record through port_gc_mark.
   The record and its layout are bound HERE rather than at each call, so no site can pass a slot from one record
   with the layout of another. port_new's mint does not come here, and that is the one honest exception: before
   JS_SetOpaque the record is unreachable by the collector and its calloc'd slots hold no value to release.
   A MOVE-OUT does not come here either, and it is not the same operation: §9.4.4's disentangle hands the slot's
   reference to the step state and clears the slot, so there is no release at all and no instant at which the
   slot names storage that has been given back. */
/* THE ADDRESS PASSES THROUGH: the asserts inside are about the SLOT, so they must name the WRITE and not this
   line — see cow.h's THE SITE TRAVELS WITH THE OPERATION. */
static void mp_set_at(JSContext *ctx, PortData *d, JSValue *slot, JSValue v,
                      const char *file, int line)
{
    cow_record_set_at(ctx, d, &PORT_REC, slot, v, file, line);
}
#define mp_set(ctx_, d_, slot_, v_) mp_set_at((ctx_), (d_), (slot_), (v_), __FILE__, __LINE__)

/* HOW MANY MESSAGES THE QUEUE HOLDS, building the Array on first use. Lazy because most ports never receive
   one and an Array per port is not free; the record's slot is JS_UNDEFINED until then. Returns 0 with an
   exception live. */
static int port_queue_len(JSContext *ctx, PortData *d, uint32_t *pn)
{
    JSValue len;
    if (JS_IsUndefined(d->queue)) {
        JSValue q = JS_NewArray(ctx);
        /* BUILT INTO A LOCAL AND THEN PUBLISHED, so the exception marker never reaches the record's slot at all
           — port_gc_mark walks that slot, and a marker there is a value the walk has no answer for. */
        if (JS_IsException(q)) return 0;
        mp_set(ctx, d, &d->queue, q);
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

/* THE LIVE PORTS OF THIS AGENT, BY BORROWED POINTER — attr_shadow's pattern, for attr_shadow's reason: the key
 * is a record the collector owns, so the entry is removed where the record is destroyed and the list is
 * therefore bounded by LIVE ports rather than by every port the run ever had.
 *
 * IT EXISTS BECAUSE §7.5.10 ASKS A QUESTION NOTHING COULD ANSWER. Destroying a Document must disentangle "the
 * MessagePorts whose relevant global object's associated Document is document", and a port's realm is on the
 * port — but there was no way to reach the ports at all, so the step could not even be attempted. This is the
 * enumeration half of it; the disentangle half is not built here, because a disentangle is a WRITE and a write
 * belongs to the flow that made it. A list of borrowed pointers is agent-global and cannot tell whose port it
 * is looking at, so destroying a document while any port of its realm is live STOPS at the DCHECK in
 * document_lifecycle.c rather than reaching into a sibling flow's timeline.
 *
 * ITS LIFETIME IS THE PORTS', NOT THE AGENT'S, AND THAT IS WHY IT IS NOT ON core/platform.h's RELEASE COLUMN.
 * message_port_free used to free this table and assert it was empty, and both were wrong at that instant for
 * the same reason: the release column runs BEFORE the collection that finalizes the page's object graph, so a
 * page holding a live port — React's scheduler holds one on every page it runs on — reached the assert with a
 * table that is legitimately full, and then port_untrack reached its DFAIL with the storage already freed.
 * Each of the three faults masked the next. So the table is allocated by the first port and released by the
 * LAST one, which is the only statement of its lifetime that is true at every instant; the emptiness the
 * release used to claim is asserted in message_port_init instead — see there for why that is the moment it
 * holds, and for what asserts it in a host that only ever declares one agent.
 * IT IS DELIBERATELY NOT A LINK INSIDE PortData. That would make untrack O(1) and read no static at all, and
 * it would put agent-global list pointers inside a record the COW delta captures BY BYTES (cow.c's
 * COW_STATE_HOST_REC memcpys the whole struct): a context switch would restore this port's prev/next to what
 * they were when the flow first reached it, splicing freed ports back into the live list. A row OUTSIDE the
 * record is what keeps the two lifetimes apart. */
static PortData **g_ports;
static int        g_ports_n, g_ports_cap;

static void port_track(PortData *d)
{
    if (g_ports_n == g_ports_cap) {
        int cap = g_ports_cap ? g_ports_cap * 2 : 8;
        PortData **g = realloc(g_ports, (size_t)cap * sizeof *g);

        CHECK(g != NULL, "message port: OOM recording a live MessagePort — an unrecorded port is one §7.5.10 "
                         "cannot see, and a destroyed document would keep an entangled channel with nothing "
                         "to say so");
        g_ports = g;
        g_ports_cap = cap;
    }
    g_ports[g_ports_n++] = d;
}

/* Called from port_finalizer, which runs in a collection that may be LATER THAN THE AGENT'S RELEASE — so this
   reads no slot that release resets, and the table it walks is one nothing else frees. */
static void port_untrack(const PortData *d)
{
    int i;

    for (i = 0; i < g_ports_n; i++)
        if (g_ports[i] == d) {
            g_ports[i] = g_ports[--g_ports_n];
            /* THE LAST PORT TAKES THE TABLE WITH IT. Not a cache eviction and not a shrink policy: the table's
               whole content is the live ports, so an empty one is a table with no reason to exist, and freeing
               it here is what leaves the next agent's message_port_init the pre-init state it asserts. */
            if (g_ports_n == 0) { free(g_ports); g_ports = NULL; g_ports_cap = 0; }
            return;
        }
    DFAIL("a MessagePort was destroyed that was never recorded as live — the two sites are port_new and this "
          "finalizer, so a port that reaches here unrecorded was built somewhere else");
}

int message_port_count_in_realm(JSContext *realm)
{
    int i, n = 0;

    for (i = 0; i < g_ports_n; i++)
        if (g_ports[i]->realm == realm) n++;
    return n;
}

/* THE FOUR COLLECTOR ENTRIES BELOW REACH THEIR RECORD FROM THE OBJECT AND READ NO STATIC OF THIS FILE — see
   core/agent_state.h on what a release owes a finalizer. message_port_free is a row on core/platform.h's
   release column and gives g_port_class and g_chan_class back to 0, and platform_agent_free runs BEFORE the
   collection that finalizes the page's object graph, so every one of these four ran with both ids already
   zero for a page that still held a port. JS_GetOpaque(val, 0) answers NULL for such an object, and the two
   halves fail differently: the finalizers would leak the record and never subtract its references, while the
   MARKS are worse — an unmarked child keeps the internal reference gc_decref exists to subtract, so gc_scan
   reads it as rooted from outside the heap and it is never collected at all. JS_GetAnyOpaque asks the OBJECT,
   which is the question the collector already answered by dispatching here: each of these two classes names
   its own pair of entries, so the id is a fact none of them has to look up and none of them may. */
static void port_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    PortData *d = JS_GetAnyOpaque(val, &id);

    (void)id;
    /* NOT `if (!d) return;`. port_new attaches the record with nothing between JS_NewObjectProtoClass and
       JS_SetOpaque that allocates on the JS heap or returns, so a MessagePort with no record has never
       existed and a NULL here is an object of this class built somewhere that is not this file. */
    DCHECK(d != NULL, "a MessagePort was finalized with no record — port_new attaches one before the object "
                      "can reach a collection or an entanglement");
    port_untrack(d);
    JS_FreeValueRT(rt, d->entangled);
    JS_FreeValueRT(rt, d->queue);
    free(d);
}

/* §9.4.2's two slots are OWNED by the channel, so they are released with it and marked while it lives — the
   ports outlive the channel in every page that keeps one, and a mark that missed them would collect a port a
   page still holds through `chan.port1`. */
static void chan_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    ChanData *d = JS_GetAnyOpaque(val, &id);

    (void)id;
    DCHECK(d != NULL, "a MessageChannel was finalized with no record — §9.4.2's constructor attaches one, with "
                      "both slots undefined, BEFORE it mints the pair that fills them");
    JS_FreeValueRT(rt, d->port1);
    JS_FreeValueRT(rt, d->port2);
    free(d);
}

static void chan_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID id = 0;
    ChanData *d = JS_GetAnyOpaque(val, &id);

    (void)id;
    /* Reachable only if the record could be attached late — §9.4.2's constructor mints two MessagePorts, and
       minting a JS object is where a collection can begin. It attaches the record first for exactly that
       reason, so this holds and the mark below always has two slots to read. */
    DCHECK(d != NULL, "a MessageChannel was marked with no record — §9.4.2's constructor attaches one before "
                      "it mints the ports, which is the step that can start a collection");
    JS_MarkValue(rt, d->port1, mark_func);
    JS_MarkValue(rt, d->port2, mark_func);
}

static void port_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID id = 0;
    PortData *d = JS_GetAnyOpaque(val, &id);

    (void)id;
    DCHECK(d != NULL, "a MessagePort was marked with no record — port_new attaches one before the object can "
                      "reach a collection");
    /* THE PAIR IS A CYCLE: each port holds the other. Without this, an entangled pair is unreachable to the
       collector and every MessageChannel a page makes is a leak the runtime's own walk reports. */
    JS_MarkValue(rt, d->entangled, mark_func);
    JS_MarkValue(rt, d->queue, mark_func);
}

/* ---- delivery ----------------------------------------------------------------------------------------------
 *
 * §9.4.4's delivery task: deserialize, then fire `message` at the port. It is a TASK on the port message queue
 * — HTML's own words — so it is enqueued through the task half of the event loop and every microtask
 * outstanding runs before it, which is what a page observes when it posts and then awaits.
 *
 * THE FIRE IS INSIDE THE TASK, WHICH IS WHY THIS IS A STEP MACHINE. §9.4.4's message port post message steps
 * step 7 queues ONE task and its last step is "fire an event named message at messageEventTarget" — the same
 * task the deserialize happened in — so the dispatch is event_target_fire_run, the request reach into §2.9,
 * and not a second enqueue. Enqueued a second time, the listener ran behind every task already standing:
 * `port.postMessage(x); setTimeout(f, 0)` ran `f` FIRST, which is the reverse of the standard and of every
 * browser. A plain C callee had no other option, because a fire it cannot park on is a fire it must queue —
 * so the task's callee is a machine, exactly as §4.11.4's dialog toggle task is. */

#define PORT_DELIVER_STAGES(X) \
    X(PD_TAKE, "HTML §9.4.4 the message port post message steps step 7.1-7.6 (take the message off the port " \
               "message queue and StructuredDeserializeWithTransfer it into the port's relevant realm)") \
    X(PD_FIRE, "HTML §9.4.4 the message port post message steps step 7.7 (fire an event named message at the " \
               "message event target, using MessageEvent)")
enum { PORT_DELIVER_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const PORT_DELIVER_STEPS[] = { PORT_DELIVER_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr   hdr;
    uint8_t     fphase;   /* the fire request's own phase */
    JSValue     ev;       /* the MessageEvent, minted at PD_TAKE and held across the suspension (owned) */
    EventFireCb cb;       /* the fire request's buffer — the type carries §2.9's argument count */
} PortDeliverTask;

static int js_port_deliver_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    PortDeliverTask *s = st;
    JSValueConst port = step_arg(&s->hdr, 0);
    PortData *d = port_of(port);
    JSContext *rctx;
    int r;

    STEP_DISPATCH(PORT_DELIVER_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(PD_TAKE);
    {
        JSValue data, entry, buf, org, ports = JS_UNDEFINED;
        StructuredWithTransfer swt;
        uint32_t n = 0;
        size_t blen = 0;
        int k;

        JS_FreeValue(ctx, cb_result);
        /* EVERY OWNED FIELD IS ON THE STATE BEFORE ANYTHING THAT CAN FAIL — the failure path tears this
           machine down through js_port_deliver_visit, which frees exactly what the state holds, and a zeroed
           block is not a block of JS_UNDEFINEDs. */
        s->ev = JS_UNDEFINED;
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
        s->fphase = 0;
        /* The port may have been closed, or its queue disabled again, between the enqueue and now — the task
           is in the queue and the state is the port's, so the state is what decides. */
        if (!d || !d->enabled || !port_queue_len(ctx, d, &n) || d->head >= n)
            return JS_STEP_DONE;
        entry = JS_GetPropertyUint32(ctx, d->queue, d->head);
        d->head++;
        /* The Array's own elements, read with no page code in the way: this component built them and nothing
           else can reach them. The bytes are BORROWED for the deserialize — the buffer owns them. */
        buf = JS_GetPropertyUint32(ctx, entry, 0);
        swt.holders = JS_GetPropertyUint32(ctx, entry, 1);
        JS_FreeValue(ctx, entry);
        swt.data.buf = JS_GetArrayBuffer(ctx, &blen, buf);
        swt.data.len = blen;
        DCHECK(swt.data.buf != NULL, "a port's queue held something that is not the serialized message it stored");
        /* §9.4.4: THE DESERIALIZE AND THE EVENT BELONG TO THE PORT'S REALM, not to whichever realm minted the
           one delivery callee this agent has. */
        rctx = d->realm;
        DCHECK(rctx != NULL, "a port was delivered to without the realm it was created in");
        data = structured_deserialize_transfer(rctx, &swt, &ports);
        JS_FreeValue(ctx, buf);
        JS_FreeValue(ctx, swt.holders);
        /* §9.4.4 fires the event with NO `origin` — the port message queue's steps name `data` and `ports` and
           nothing else, so §9.1's member keeps the empty string its IDL gives it. It is spelled as a VALUE
           because that is what the mint takes, and it is minted ONCE for both arms below: the empty origin is
           the same fact whether the deserialize succeeded or not, and two mints would be two places to get it
           wrong. Not a default filling a hole — a positive statement that a port has no sending origin, which
           is the whole difference between §9.4.4's delivery and §9.3.3's. */
        org = JS_NewString(rctx, "");
        CHECK(!JS_IsException(org), "§9.4.4: a port delivery's empty origin could not be allocated");
        if (JS_IsException(data)) {
            /* §9.4.4: a message that cannot be deserialized fires `messageerror` rather than `message`. This
               engine's deserializer only fails where its own writer and reader disagree, which crashes instead
               — so reaching here means the exception came from somewhere else and is the page's to see. */
            JS_FreeValue(rctx, JS_GetException(rctx));
            JS_FreeValue(rctx, ports);
            s->ev = message_event_new(rctx, "messageerror", JS_UNDEFINED, org, JS_UNDEFINED, JS_UNDEFINED);
        } else {
            /* §9.4.4: `ports` is `newPorts` — the MessagePorts among the [[TransferredValues]] that ARRIVED
               with this message, in order. A transferred ArrayBuffer is in the same list and is not one. */
            JSValue new_ports = message_event_ports_of(rctx, ports);
            JS_FreeValue(rctx, ports);
            if (JS_IsException(new_ports)) { JS_FreeValue(rctx, org); JS_FreeValue(rctx, data); return JS_STEP_ABRUPT; }
            s->ev = message_event_new(rctx, "message", data, org, JS_UNDEFINED, new_ports);
            JS_FreeValue(rctx, data);
            JS_FreeValue(rctx, new_ports);
        }
        JS_FreeValue(rctx, org);
        if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; return JS_STEP_ABRUPT; }
        STEP_GOTO(s->hdr.stage, PD_FIRE, &s->fphase, NULL);
        return JS_STEP_YIELD;
    }

    STEP_ARM(PD_FIRE);
    DCHECK(d != NULL, "§9.4.4's delivery resumed at its fire with no port record — the take stage is the only "
                      "way to this stage and it answers DONE for a port that has none");
    rctx = d->realm;
    DCHECK(JS_IsObject(s->ev), "§9.4.4's delivery resumed at its fire with no MessageEvent to dispatch");
    r = event_target_fire_run(rctx, &s->fphase, STEP_CB(s->cb), port, s->ev, JS_UNDEFINED, cb_result,
                              NULL, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    JS_FreeValue(rctx, s->ev);
    s->ev = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static void js_port_deliver_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    PortDeliverTask *s = st;
    int k;

    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, k) v->val(ctx, &s->cb[k]);
}

static const JSTrampStepDef js_port_deliver_def = {
    sizeof(PortDeliverTask), js_port_deliver_step, NULL, 0,
    .visit = js_port_deliver_visit,
    .algorithm = "HTML §9.4.4 the port message queue delivery task",
    .steps = PORT_DELIVER_STEPS
};

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

/* §9.4.4's "enable this's port message queue": everything already in it becomes a task, in order. */
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

/* §9.4.4's MESSAGE PORT POST MESSAGE STEPS, in the standard's order — and the order is the algorithm.
 *
 * targetPort IS READ AT STEP 1, before anything is transferred, because the transfer list may CONTAIN it: the
 * standard's step 4 calls that case `doomed`, transfers the port anyway and then delivers nothing, which is a
 * different thing from "there was no target". Reading `entangled` back after the transfer steps had already
 * disentangled it happened to answer the same way for the shapes tried so far, and would have stopped the
 * moment anything else moved that field — §scheduler's rule that an operation takes its inputs with it.
 *
 * ONE BODY FOR BOTH OVERLOADS, WHICH IS WHAT §9.4.4 ITSELF WRITES. The `(message, options)` entry's method
 * steps run the message port post message steps with `options`; the `(message, transfer)` entry's steps say
 * "Let options be «[ "transfer" → transfer ]»" and then run the SAME steps — so there is one algorithm and its
 * step 1 is `options["transfer"]`. The wrapping is all that differs, and it is three lines below. */
static JSValue js_port_post(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    PortData *d = port_of(this_val), *t;
    JSValueConst second = argc > 1 ? argv[1] : JS_UNDEFINED;
    StructuredWithTransfer swt;
    JSValue transfer, target;
    uint32_t n, i;
    bool doomed = false;

    (void)magic;
    if (!d)
        return JS_ThrowTypeError(ctx, "not a MessagePort");
    /* UNKNOWN EXTERNAL INPUT CROSSES A DECLARED POSITION AS ITSELF, which means §3.6 chose NO overload for it
       and there is no arm to read back. Both arms stay FEASIBLE: an unknown that is an iterable is a transfer
       list, and an unknown that is not is an options bag — and the two differ in what gets DETACHED, so
       deciding it here would either detach objects the page still holds or silently transfer nothing. What
       every earlier version of this body did instead was fall to the dictionary arm and read `transfer` off
       the attacker's own value, which manufactures a plausible datum out of a measurement nobody made.
       js_window_post names the same missing mechanism at its own §3.6 split for the same reason. */
    if (concolic_is(second))
        DFAIL("MessagePort.postMessage was given a CONCOLIC second argument — §3.6 step 12 decides its two "
              "overloads by `? GetMethod(V, %Symbol.iterator%)`, so an unpinned one leaves the transfer-list "
              "arm and the options arm both feasible. Fork the post: convert and run the member under each "
              "arm, the way a branch on unknown external input forks anywhere else, rather than choosing one "
              "here");
    /* §9.4.4 STEP 1 of `postMessage`: targetPort is the port this one is entangled with, TAKEN NOW. */
    target = JS_DupValue(ctx, d->entangled);

    /* THE OVERLOAD, READ BACK OFF THE CONVERTED VALUE — never duck-typed off the page's. This body used to
       perform §3.6's whole resolution itself: `JS_IsArray(opts)` on the page's own object (which is not the
       algorithm's test — §3.6 step 12 asks `? GetMethod(V, %Symbol.iterator%)`, so every non-Array iterable
       took the dictionary arm and had `transfer` read off it), then a `length`-and-indices walk that is the
       array-like algorithm rather than §3.2.21's iterator protocol, run from an activation with no flow base
       under the page's getters. IDL_SEQUENCE_OBJECT_OR_DICT performs it on the tramp now, so what arrives here
       is one of exactly two ENGINE-BUILT values: §3.2.21's materialized Array, or the converted
       StructuredSerializeOptions. Asking which is reading an arm off a value this engine made.
       IT IS MATERIALIZED ONCE for the reason it always was: the three questions below and §2.7.7's two loops
       are five walks of one list, and a page-supplied one can answer each of them differently. */
    DCHECK(JS_IsObject(second),
           "postMessage's second argument reached the body as neither a materialized sequence nor a "
           "dictionary — IDL_SEQUENCE_OBJECT_OR_DICT resolves to exactly those two, and an OMITTED one is the "
           "`optional StructuredSerializeOptions options = {}` dictionary rather than an absent argument");
    transfer = JS_IsArray(second) ? JS_DupValue(ctx, second)      /* «[ "transfer" → transfer ]» */
                                  : idl_dict_get(ctx, second, "transfer");
    if (JS_IsException(transfer)) { JS_FreeValue(ctx, target); return JS_EXCEPTION; }
    /* `sequence<object> transfer = []`: an options dictionary carrying no `transfer` member IS a transfer list
       of nothing. That is the IDL's own default and not a hole a reader fills — the absence is the positive
       statement — and js_structured_clone and js_window_post make the same sentence about the same member. */
    if (JS_IsUndefined(transfer)) {
        transfer = JS_NewArray(ctx);
        if (JS_IsException(transfer)) { JS_FreeValue(ctx, target); return JS_EXCEPTION; }
    }
    /* THE MEMBER ROAD, which the DFAIL at the top of this body does not cover. That one fires when the SECOND
       ARGUMENT is unknown — §3.6 with no overload to choose. `port.postMessage(m, {transfer: location.hash})`
       is a real Object, so the dictionary arm is correctly chosen and the unknown is one member inside it;
       Web IDL §3.2.17 Dictionary types' member loop crosses it as itself, and the assert below then reported
       that as the declaration having failed to materialize a sequence.
       IT IS NOT THE ENUMERATION CASE AND MUST NOT BE FORKED LIKE ONE. Web IDL §3.2.21 Sequences —
       sequence<T> step 2 is "Let method be ? GetMethod(V, %Symbol.iterator%)", which over an unknown is
       another unknown, and §3.2.21.1 Creating a sequence from an iterable repeats to an unknown length — so
       unlike an enumeration there is no arm set the spec writes down, and the honest statement is that the
       conversion has no answer over unknown input yet. js_window_post names the same gap at its own copy of
       this member. */
    if (concolic_is(transfer))
        DFAIL("MessagePort.postMessage's `transfer` MEMBER is UNKNOWN EXTERNAL INPUT, so §3.2.21's "
              "iterator-protocol conversion never ran and what follows would read a `length` and indices off "
              "the page's own unknown — the array-like walk this body was converted away from, over a value "
              "that is not even an array. There is no given arm set to ask step_fork_run over: §3.2.21 step "
              "2's GetMethod over an unknown is another unknown and §3.2.21.1 repeats to an unknown length. "
              "Build §3.2.21 over unknown input AT THE CONVERSION — the element cursor already parks, so what "
              "is missing is what the protocol answers when the ITERABLE is unknown");
    DCHECK(JS_IsArray(transfer), "postMessage's transfer list is not the materialized sequence — "
                                 "IDL_SEQUENCE_OBJECT is what §3.2.21 builds, and reading the page's object "
                                 "again here would run its iterator a second time");

    /* §9.4.4 STEPS 2-4. A transfer list naming THIS port is a DataCloneError; one naming the TARGET dooms the
       message. Both are here rather than in the transfer steps because only the post knows which ports it is
       between, and both precede any detaching — a page that catches the error still has a working port. */
    n = structured_transfer_len(ctx, transfer);
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, transfer, i);
        bool self = JS_VALUE_GET_PTR(e) == JS_VALUE_GET_PTR(this_val);

        if (!JS_IsUndefined(target) && JS_VALUE_GET_PTR(e) == JS_VALUE_GET_PTR(target))
            doomed = true;
        JS_FreeValue(ctx, e);
        if (self) {
            JS_FreeValue(ctx, transfer);
            JS_FreeValue(ctx, target);
            return JS_ThrowDOMException(ctx, "DataCloneError",
                                        "a port cannot be transferred through itself");
        }
    }

    /* §9.4.4 STEP 5: SERIALIZE NOW, and run the transfer steps. The throw belongs to this call, not to the
       delivery task — a page's try/catch around postMessage is where the standard puts the DataCloneError. It
       happens even when there is no target, which is why it precedes the null check.
       `message` IS PRESENT, and this asserts it rather than defaulting it: `any message` is not optional, so
       §3.6 step 4 removes both entries at argcount 0 and step 5 throws before this body runs. The
       `argc > 0 ? … : JS_UNDEFINED` that stood here read as prudence and was a consumer-side default over a
       producer that cannot omit — the shape §Offensive-programming names, one call site at a time. */
    DCHECK(argc >= 1, "MessagePort.postMessage ran with no message — `any message` is a required argument, so "
                      "§3.6 step 5 is the declaration's and idl_optional_from names position 1 as the first "
                      "optional one");
    if (structured_serialize_transfer(ctx, argv[0], transfer, &swt) < 0) {
        JS_FreeValue(ctx, transfer);
        JS_FreeValue(ctx, target);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, transfer);

    /* §9.4.4 STEP 6: a port with nothing on the other end posts into nowhere, and neither does a doomed one —
       and neither is an error. The transfer has still HAPPENED in both cases: the standard detaches before it
       looks at the target, so a page that transfers into a closed port does not get its port back. */
    t = JS_IsUndefined(target) ? NULL : port_of(target);
    if (!t || doomed) {
        structured_with_transfer_free(ctx, &swt);
        JS_FreeValue(ctx, target);
        return JS_UNDEFINED;
    }

    {
        /* ONE QUEUE ENTRY IS THE MESSAGE AND ITS TRANSFER HOLDERS. The bytes become an ArrayBuffer the collector
           owns — see the queue's comment; NewArrayBufferCopy rather than adopting the block, because the block
           is js_malloc'd and an adopting ArrayBuffer frees with its own allocator. */
        JSValue buf = JS_NewArrayBufferCopy(ctx, swt.data.buf, swt.data.len);
        JSValue entry;
        uint32_t qn = 0;
        if (JS_IsException(buf)) { structured_with_transfer_free(ctx, &swt); JS_FreeValue(ctx, target); return JS_EXCEPTION; }
        entry = JS_NewArray(ctx);
        if (JS_IsException(entry)) {
            JS_FreeValue(ctx, buf);
            structured_with_transfer_free(ctx, &swt);
            JS_FreeValue(ctx, target);
            return JS_EXCEPTION;
        }
        JS_SetPropertyUint32(ctx, entry, 0, buf);
        JS_SetPropertyUint32(ctx, entry, 1, JS_DupValue(ctx, swt.holders));
        structured_with_transfer_free(ctx, &swt);
        if (!port_queue_len(ctx, t, &qn)) { JS_FreeValue(ctx, entry); JS_FreeValue(ctx, target); return JS_EXCEPTION; }
        JS_SetPropertyUint32(ctx, t->queue, qn, entry);
    }
    if (t->enabled)
        port_enqueue_delivery(ctx, target);
    JS_FreeValue(ctx, target);
    return JS_UNDEFINED;
}

/* §9.4.4's `start()`. */
static JSValue js_port_start(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    PortData *d = port_of(this_val);
    (void)argc; (void)argv; (void)magic;
    if (!d) return JS_ThrowTypeError(ctx, "not a MessagePort");
    port_enable(ctx, this_val, d);
    return JS_UNDEFINED;
}

/* §9.4.4's `close()` AND THE DISENTANGLE STEPS IT RUNS — and it is a STEP MACHINE because its last step fires
 * an event. "Set this's [[Detached]] internal slot value to true. If this is entangled, disentangle it", and
 * the disentangle steps end "Fire an event named close at otherPort" — a §2.9 dispatch, which runs the other
 * end's `onclose` and its `close` listeners. §2.9 is SYNCHRONOUS, so the fire is the REQUEST reach
 * (event_target_fire_run) from a machine that can park, and never a C call into the page's code.
 *
 * ONLY THE OTHER PORT IS TOLD, WHICH IS THE SPEC'S OWN NOTE AND NOT AN OPTIMISATION: "we only dispatch the
 * event on otherPort because initiatorPort explicitly triggered the close, its Document no longer exists, or
 * it was already garbage collected". The three cases §9.4.4 lists for the event — close() was called, the
 * Document was destroyed, the port was garbage collected — are three CALLERS of these steps; this is the
 * first, and the other two reach the same disentangle rather than a second copy of it.
 *
 * THE PAIR IS DISENTANGLED BEFORE THE FIRE, in the order the steps are written: a listener that reads
 * `otherPort` during its own `close` event must already see a port entangled with nothing, and a listener that
 * posts through it must find no target rather than a queue nobody will read. */
#define PORT_CLOSE_STAGES(X) \
    X(PC_BEGIN, "HTML §9.4.4 Message ports, the close() method steps (set [[Detached]]; if entangled, run the " \
                "disentangle steps: take otherPort, then disentangle both sides of the pair)") \
    X(PC_FIRE, "HTML §9.4.4 Message ports, the disentangle steps' last step (fire an event named close at " \
               "otherPort)")
enum { IDL_STEP_STAGE_BASE(PORT_CLOSE_STAGES) PORT_CLOSE_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const PORT_CLOSE_STEPS[] = { PORT_CLOSE_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    uint8_t     phase;
    JSValue     other;   /* the disentangled otherPort, held across the suspension — the fire's TARGET */
    JSValue     ev;
    EventFireCb cb;
} PortCloseState;

static void js_port_close_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    PortCloseState *s = st;
    int i;

    v->val(ctx, &s->other);
    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, i) v->val(ctx, &s->cb[i]);
}

static int js_port_close_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                              JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    PortCloseState *s = st;
    JSValue in = cb_result;
    int r;

    (void)argc; (void)argv;

    STEP_DISPATCH(PORT_CLOSE_STAGES, hdr->stage, hdr->def->algorithm, -1);

    STEP_ARM(PC_BEGIN);
    {
        PortData *d = port_of(hdr->this_val);
        int i;

        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;   /* CONSUMED — the fire below takes ownership of whatever `in` then holds */
        /* EVERY OWNED FIELD IS PLACED BEFORE THE FIRST STEP THAT CAN FAIL, so the teardown frees exactly what
           the state holds and never reads a slot this arm had not reached yet. */
        s->other = JS_UNDEFINED;
        s->ev = JS_UNDEFINED;
        STEP_CB_FOREACH(s->cb, i) s->cb[i] = JS_UNDEFINED;
        s->phase = 0;
        if (!d)
            return JS_ThrowTypeError(ctx, "close called on something that is not a MessagePort"), -1;
        d->detached = 1;
        /* "If this is entangled, disentangle it" — and a port that is not entangled has no otherPort, so
           there is nothing to fire at and the method is done. */
        if (JS_IsUndefined(d->entangled)) {
            *presult = JS_UNDEFINED;
            return 0;
        }
        {
            /* The disentangle steps' 1-3: take otherPort, assert it exists, and make the two no longer
               entangled or associated with each other. Entanglement is symmetric, so BOTH sides are cleared
               here — a port left pointing at a closed one would keep queueing messages nobody will read. */
            PortData *o = port_of(d->entangled);

            s->other = d->entangled;
            d->entangled = JS_UNDEFINED;
            DCHECK(o != NULL, "HTML §9.4.4's disentangle steps assert otherPort exists, and this port was "
                              "entangled with something that is not a MessagePort — entanglement is written "
                              "in exactly two places and both write a port");
            if (o) mp_set(ctx, o, &o->entangled, JS_UNDEFINED);
        }
        /* The disentangle steps' step 4. A `close` event bubbles no further than its target and is not
           cancelable: §9.4.4 fires it with no initialisation, which is DOM §2.5's un-initialized event. */
        s->ev = event_new(ctx, "close", false, false);
        if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; return -1; }
        STEP_GOTO(hdr->stage, PC_FIRE, &s->phase, NULL);
        return JS_STEP_YIELD;
    }

    STEP_ARM(PC_FIRE);
    DCHECK(JS_IsObject(s->ev), "HTML §9.4.4's disentangle steps resumed with no close event to fire");
    DCHECK(message_port_is(s->other), "HTML §9.4.4's disentangle steps resumed with no otherPort to fire at");
    r = event_target_fire_run(ctx, &s->phase, STEP_CB(s->cb), s->other, s->ev, JS_UNDEFINED, in, NULL,
                              out_cb, out_argc);
    in = JS_UNDEFINED;   /* CONSUMED by the request, on both of its legs */
    if (r > 0) return r;
    if (r < 0) return -1;
    JS_FreeValue(ctx, s->ev);
    s->ev = JS_UNDEFINED;
    *presult = JS_UNDEFINED;
    return 0;
}

static const IdlStepDecl PORT_CLOSE_DECL = {
    js_port_close_step, sizeof(PortCloseState), js_port_close_visit, NULL,
    "HTML §9.4.4 Message ports, close()", PORT_CLOSE_STEPS
};

/* §9.4.4: setting `onmessage` ENABLES the port message queue. It is the platform's one event handler attribute
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

/* ---- §9.4.4's TRANSFER STEPS ---------------------------------------------------------------------------------
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
    /* §9.4.4: transferring a detached port is a DataCloneError. So is transferring the port a message is being
       posted THROUGH, which the caller checks — it has the source in hand and this does not. */
    if (d->detached)
        return JS_ThrowDOMException(ctx, "DataCloneError", "the port has been closed");
    h = JS_NewArray(ctx);
    if (JS_IsException(h)) return h;
    JS_SetPropertyUint32(ctx, h, TH_QUEUE, JS_DupValue(ctx, d->queue));
    JS_SetPropertyUint32(ctx, h, TH_HEAD, JS_NewUint32(ctx, d->head));
    JS_SetPropertyUint32(ctx, h, TH_REMOTE, JS_DupValue(ctx, d->entangled));
    /* The old object keeps nothing: its queue has moved and it is detached. */
    mp_set(ctx, d, &d->queue, JS_UNDEFINED);
    d->head = 0;
    d->enabled = 0;
    d->detached = 1;
    if (!JS_IsUndefined(d->entangled)) {
        PortData *o = port_of(d->entangled);
        if (o) mp_set(ctx, o, &o->entangled, JS_UNDEFINED);
        mp_set(ctx, d, &d->entangled, JS_UNDEFINED);
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
    mp_set(ctx, d, &d->queue, JS_GetPropertyUint32(ctx, holder, TH_QUEUE));
    { JSValue hv = JS_GetPropertyUint32(ctx, holder, TH_HEAD);
      JS_ToUint32(ctx, &head, hv);
      JS_FreeValue(ctx, hv); }
    d->head = head;
    /* §9.4.4: the arriving port's queue starts DISABLED however full it is — the receiver must start() it or
       set onmessage, exactly as for a port it made itself. */
    d->enabled = 0;
    remote = JS_GetPropertyUint32(ctx, holder, TH_REMOTE);
    if (!JS_IsUndefined(remote)) {
        PortData *o = port_of(remote);
        mp_set(ctx, d, &d->entangled, JS_DupValue(ctx, remote));
        if (o) mp_set(ctx, o, &o->entangled, JS_DupValue(ctx, obj));
    }
    JS_FreeValue(ctx, remote);
    return obj;
}

/* §2.7.7's `interfaceName` for this interface's data holders, which is the identifier of §9.4.4's primary
   interface and nothing else — §2.7.8 picks these two steps back out by it. */
static const StructuredTransferable PORT_TRANSFERABLE = {
    "MessagePort", message_port_is, port_transfer_out, port_transfer_in
};

/* ---- MessageChannel ------------------------------------------------------------------------------------------ */

static JSValue port_new(JSContext *ctx)
{

    JSValue proto = JS_GetClassProto(ctx, g_port_class);
    JSValue obj;
    PortData *d;

    DCHECK(!JS_IsNull(proto), "a MessagePort was minted in a realm that never ran its install");
    obj = JS_NewObjectProtoClass(ctx, proto, g_port_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) return obj;
    d = calloc(1, sizeof *d);
    CHECK(d != NULL, "message port: OOM building a MessagePort");
    d->entangled = d->queue = JS_UNDEFINED;
    d->realm = ctx;   /* §9.4.4's relevant realm, taken where the port is created */
    port_track(d);
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
    mp_set(ctx, d1, &d1->entangled, JS_DupValue(ctx, p2));
    mp_set(ctx, d2, &d2->entangled, JS_DupValue(ctx, p1));
    *port2 = p2;
    return p1;
}

/* §9.4.2's `port1` and `port2` getter steps — "return this's port 1" / "return this's port 2", which are the
   two slots the channel holds. ONE getter with the magic naming which, because they are one algorithm over
   two slots and a second copy is a second place to be wrong.
   THE BRAND CHECK IS THE SPEC'S: §3.7.6's getter throws a TypeError when the receiver does not implement the
   interface, and `MessageChannel.prototype.port1` read off the prototype itself is exactly that case — the
   prototype is an ordinary object and carries no channel record. */
static JSValue js_chan_port(JSContext *ctx, JSValueConst this_val, int magic)
{
    ChanData *d = JS_GetOpaque(this_val, g_chan_class);

    if (!d)
        return JS_ThrowTypeError(ctx, "Illegal invocation");
    DCHECK(magic == 0 || magic == 1, "§9.4.2 declares two ports and this getter was minted for a third");
    return JS_DupValue(ctx, magic ? d->port2 : d->port1);
}

/* WHERE THIS MACHINE RESTS. §9.4.2's constructor is three steps, none of which can reach the page's code —
   two new MessagePorts and the entanglement — so the machine has one stage and never returns to it. */
#define CHAN_CTOR_STAGES(X) \
    X(CHAN_CTOR_PAIR = IDL_STEP_FIRST, \
      "HTML §9.4.2 new MessageChannel() steps 1-3 (this's port 1 and port 2, then entangle them)")
enum { CHAN_CTOR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const CHAN_CTOR_STEPS[] = { CHAN_CTOR_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct { int unused; } JSChanCtorState;
static void js_chan_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

/* §9.4.2's `new MessageChannel()`. It takes no arguments and reads nothing of the page's, so its body runs to
   completion; it is declared through the IDL machine because that is how a constructor is declared here. */
static int js_chan_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                             JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSValue obj, p1, p2;
    ChanData *d;

    (void)st; (void)argc; (void)argv; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(hdr->stage == CHAN_CTOR_PAIR, "the MessageChannel constructor resumed at a stage §9.4.2 does not have");
    if (JS_IsUndefined(hdr->this_val))
        return JS_ThrowTypeError(ctx, "constructor MessageChannel requires 'new'"), -1;
    {
        JSValue proto = JS_GetClassProto(ctx, g_chan_class);
        DCHECK(!JS_IsNull(proto), "a MessageChannel was constructed in a realm that never ran its install");
        obj = JS_NewObjectProtoClass(ctx, proto, g_chan_class);
        JS_FreeValue(ctx, proto);
    }
    if (JS_IsException(obj)) return -1;
    /* THE RECORD IS COMPLETE BEFORE THE FIRST STEP THAT CAN ALLOCATE — the same ownership rule §C-stack states
       for a step state, and here it is the COLLECTOR that reads the half-built object rather than an unwind.
       "Set this's port 1 to a new MessagePort" mints a JS object, which is where a collection can begin, and
       chan_gc_mark would then be handed a channel whose opaque was still NULL; the same object reaches
       chan_finalizer on the failure path below. A calloc'd block is not a block of JS_UNDEFINEDs, so the two
       slots are written before the record is attached. */
    d = calloc(1, sizeof *d);
    CHECK(d != NULL, "message port: OOM building a MessageChannel");
    d->port1 = d->port2 = JS_UNDEFINED;
    JS_SetOpaque(obj, d);
    p1 = message_port_pair(ctx, &p2);
    if (JS_IsException(p1)) { JS_FreeValue(ctx, obj); return -1; }
    /* §9.4.2 steps 1-2: "Set this's port 1 to a new MessagePort in this's relevant realm", the same for port 2.
       The channel OWNS both from here; chan_finalizer releases them and §3.7.6's two accessors on the prototype
       read them. `d` survives the mint above because `obj` is held on this stack, so nothing can collect the
       object the record hangs off. */
    d->port1 = p1;
    d->port2 = p2;
    *presult = obj;
    return 0;
}

static const IdlStepDecl js_chan_ctor_decl = {
    js_chan_ctor_step, sizeof(JSChanCtorState), js_chan_visit, NULL,
    "HTML §9.4.2 new MessageChannel()", CHAN_CTOR_STEPS
};

/* ---- install -------------------------------------------------------------------------------------------- */

void message_port_init(JSContext *ctx)
{
    JSClassDef pd = { "MessagePort", .finalizer = port_finalizer, .gc_mark = port_gc_mark };
    JSClassDef cd = { "MessageChannel", .finalizer = chan_finalizer, .gc_mark = chan_gc_mark };
    JSRuntime *rt = JS_GetRuntime(ctx);

    /* NOT `if (g_mp_rt == rt) return;`. This component has exactly ONE declaration site — core/platform.c's
       row — so the test could never be true, and what it could do was hand a second agent the class ids, the
       pool entries and the delivery callee a dead runtime issued. See core/agent_state.h. */
    DCHECK(g_mp_rt == NULL, "message_port_init ran twice — §9.4.4's classes are declared once per AGENT");
    /* AND EVERY PORT OF THE PREVIOUS AGENT HAS BEEN COLLECTED, WHICH IS ASSERTED HERE BECAUSE HERE IS WHERE IT
       IS TRUE. message_port_free used to claim it, and could not: a page legitimately holds live ports when the
       release column runs, and the collection that finalizes them is the previous runtime's JS_FreeRuntime —
       which is over by the time a second agent reaches this line. So an entry surviving to here is a PortData
       whose finalizer never ran, which is the fact the old assert was reaching for.
       IN A HOST THAT DECLARES ONE AGENT PER PROCESS this line never runs, and the invariant is not therefore
       unchecked: a MessagePort that is never finalized is a GC object still live, which is exactly what
       JS_FreeRuntime's own `list_empty(&rt->gc_obj_list)` DCHECK fires on, with the [gcleak] census naming the
       class. That check is at the right instant in EVERY host, which is more than the released one managed. */
    DCHECK(g_ports == NULL && g_ports_n == 0 && g_ports_cap == 0,
           "MessagePorts of a previous agent are still recorded as live — the last port frees this table, so a "
           "surviving entry is a PortData whose finalizer never ran and whose queue, entanglement and realm "
           "leaked with it");
    g_mp_rt = rt;
    JS_NewClassID(rt, &g_port_class);
    JS_NewClass(rt, g_port_class, &pd);
    JS_NewClassID(rt, &g_chan_class);
    JS_NewClass(rt, g_chan_class, &cd);

    {
        /* §9.4.4's TWO OVERLOADS, AS ONE DECLARATION:

               undefined postMessage(any message, sequence<object> transfer);
               undefined postMessage(any message, optional StructuredSerializeOptions options = {});

           BOTH type lists are TWO long, so §3.6 step 4 removes neither at any arity and the split is decided
           entirely at position 1 — by `? GetMethod(V, %Symbol.iterator%)`, which is the page's code and
           therefore a rest point. IDL_SEQUENCE_OBJECT_OR_DICT is that split (see core/idl_args.h); position 1
           was IDL_ANY, which declared no conversion at all and left the body performing the resolution, the
           iteration and the element type check from C. */
        static const IdlArgType POST_ARGS[2] = { IDL_ANY, IDL_SEQUENCE_OBJECT_OR_DICT };
        /* §9.4.4's `dictionary StructuredSerializeOptions { sequence<object> transfer = []; };` — one member,
           declared HERE because §9.4.4 is where the standard writes it, and it is the same dictionary
           `structuredClone` takes and the one `WindowPostMessageOptions` inherits from. */
        static const IdlDictMember POST_OPTS[] = { { "transfer", IDL_SEQUENCE_OBJECT, false, NULL, 0 } };

        g_id_post = idl_method_id_dict(ctx, POST_ARGS, 2, POST_OPTS,
                                       (int)(sizeof POST_OPTS / sizeof POST_OPTS[0]), js_port_post, 0);
        idl_optional_from(1);   /* the dictionary entry's `optional … options = {}` — §3.6 step 12's first
                                   clause reads it, which is what makes `postMessage(m, undefined)` that arm */
        g_id_start = idl_method_id(ctx, NULL, 0, js_port_start, 0);
        g_id_close = idl_method_id_step(ctx, NULL, 0, NULL, 0, &PORT_CLOSE_DECL, 0);
    }
    g_chan_ctor_stepid = idl_method_id_step(ctx, NULL, 0, NULL, 0, &js_chan_ctor_decl, 0);
    realm_declare_intrinsic(message_port_install_protos);

    g_deliver_stepid = JS_RegisterStepDef(rt, &js_port_deliver_def);
    DCHECK(g_deliver_stepid >= 0, "§9.4.4's delivery machine could not be declared against this runtime");
    g_deliver_fn = JS_NewCFunction2(ctx, NULL, "", 1, JS_CFUNC_step, g_deliver_stepid);
    CHECK(JS_IsFunction(ctx, g_deliver_fn), "the port delivery task's callee could not be allocated");
    event_target_set_handler_hook(port_handler_set);
    /* §9.4.4's interface is [Transferable], and this is where it says so. The registry is
       core/structured_clone.c's own state and that component releases it whole — a row is a pointer to the
       `static const` above and this file allocated nothing. */
    structured_register_transferable(&PORT_TRANSFERABLE);
    agent_state_ptr("message_port", &g_mp_rt, "the runtime §9.4.4's classes were declared in, and the latch");
    agent_state_class("message_port", &g_port_class, "§9.4.4's MessagePort class");
    agent_state_class("message_port", &g_chan_class, "§9.4.2's MessageChannel class");
    agent_state_value("message_port", &g_deliver_fn, "the queued port delivery task's callee");
    agent_state_id("message_port", &g_chan_ctor_stepid, "§9.4.2's constructor machine");
    agent_state_id("message_port", &g_deliver_stepid, "§9.4.4's port delivery task machine");
    agent_state_id("message_port", &g_id_post, "§9.4.4's postMessage declaration");
    agent_state_id("message_port", &g_id_start, "§9.4.4's start declaration");
    agent_state_id("message_port", &g_id_close, "§9.4.4's close machine");
    /* THE LIVE-PORT TABLE IS NOT DECLARED HERE, and that is the point rather than an omission. Every slot on
       this registry is asserted back at its pre-init value at the END of the release column, and this one is
       legitimately non-empty at that instant — a page holding a MessagePort has not done anything wrong, and
       the finalizer that empties the table has not run yet. Declaring it made platform_agent_free abort on a
       correct program. Its pre-init state is asserted at the top of this function instead, which is the moment
       it is true; see there. */
}

/* §9.4.4's AND §9.4.2's INTERFACE PROTOTYPE OBJECTS, FOR ONE REALM. `postMessage` is the member that makes
   this an answer and not an identity: a shared one would serialize and deliver under the realm that built it,
   so a port handed to a child document would carry the parent's realm into every message it sent. */
void message_port_install_protos(JSContext *ctx)
{
    JSValue port_p, chan_p, prev;

    DCHECK(g_port_class != 0, "a realm asked for MessagePort.prototype before the class was declared");
    prev = JS_GetClassProto(ctx, g_port_class);
    DCHECK(JS_IsNull(prev), "message_port_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);

    /* §9.4.4: `interface MessagePort : EventTarget` — the listeners and the two handler attributes. */
    port_p = event_target_derived_proto(ctx);
    idl_interface_tag(ctx, port_p, "MessagePort");
    /* §9.4.4's own `onclose` is EH_MESSAGE_PORT and not EH_PORT: EH_PORT is the MessageEventTarget mixin's
       two names, which BroadcastChannel includes as well, and §9.5 declares no `onclose` on one. */
    event_target_install_handlers(ctx, port_p, EH_PORT | EH_MESSAGE_PORT);
    idl_install_method(ctx, port_p, "postMessage", g_id_post);
    idl_install_method(ctx, port_p, "start", g_id_start);
    idl_install_method(ctx, port_p, "close", g_id_close);
    JS_SetClassProto(ctx, g_port_class, port_p);

    chan_p = JS_NewObject(ctx);
    CHECK(!JS_IsException(chan_p), "MessageChannel.prototype could not be allocated");
    idl_interface_tag(ctx, chan_p, "MessageChannel");
    /* §9.4.2's two members, where Web IDL §3.7.6 puts a regular attribute of an interface that is not
       [Global]: on the INTERFACE PROTOTYPE OBJECT. They were own data properties of each channel. */
    idl_install_accessor(ctx, chan_p, "port1", js_chan_port, 0, -1);
    idl_install_accessor(ctx, chan_p, "port2", js_chan_port, 1, -1);
    JS_SetClassProto(ctx, g_chan_class, chan_p);
}

void message_port_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;

    JSValue port_p = JS_GetClassProto(ctx, g_port_class), chan_p = JS_GetClassProto(ctx, g_chan_class);

    DCHECK(g_chan_ctor_stepid >= 0, "MessageChannel was installed before message_port_init declared it");
    DCHECK(!JS_IsNull(port_p) && !JS_IsNull(chan_p),
           "the messaging interfaces were installed into a realm that never ran their proto build");
    ctor = idl_interface_object(ctx, "MessagePort", port_p);
    JS_SetPropertyStr(ctx, (JSValue)global, "MessagePort", ctor);

    ctor = idl_step_constructor(ctx, "MessageChannel", g_chan_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the MessageChannel interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, chan_p);
    JS_SetPropertyStr(ctx, (JSValue)global, "MessageChannel", ctor);
    JS_FreeValue(ctx, port_p);
    JS_FreeValue(ctx, chan_p);
}

void message_port_free(JSRuntime *rt)
{
    /* NOT `if (!g_mp_rt) return;`. The release is the inverse of the DECLARATION and rides the same row of
       core/platform.c's one list, whose declare pass is unconditional and whose table asserts that a release
       row has a declare. */
    DCHECK(g_mp_rt == rt, "§9.4.4's port machinery was released against a runtime that is not the one it "
                          "declared in — every value below would have its reference subtracted from a runtime "
                          "that never took it");
    /* §8.1.7.2'S HANDLER-SET HOOK IS GIVEN BACK BY THE COMPONENT THAT CLAIMED IT, which is this one. The slot
       is core/events/event_target.c's and it names `port_handler_set` in THIS file, so a release that kept it
       would leave the events layer calling into a component whose classes and delivery callee are gone — the
       defect core/agent_state.h found in idb_transaction. event_target_free asserts it, and reverse
       declaration order is what runs this row first. */
    event_target_set_handler_hook(NULL);
    JS_FreeValueRT(rt, g_deliver_fn);
    g_deliver_fn = JS_UNDEFINED;   /* the prototypes are the REALMS' — released with their contexts */
    g_mp_rt = NULL;
    /* THE TWO CLASS IDS COME BACK — core/agent_state.h requires it, so a second agent's init cannot read a
       handle this runtime issued — and the four collector entries above therefore read neither of them: the
       collection that finalizes a port runs AFTER this call, so a class id is the one thing that cannot
       identify one by then. */
    g_port_class = g_chan_class = 0;
    g_id_post = g_id_start = g_id_close = -1;
    g_chan_ctor_stepid = g_deliver_stepid = -1;
    /* THE LIVE-PORT TABLE IS NOT TOUCHED HERE. It was freed here, under an assert that it was already empty,
       and both were false at this instant: the ports of a page that still holds one — React's scheduler holds
       one on every page it runs on — are finalized by the collection this call runs BEFORE, so the assert
       fired first and, with it removed, port_untrack then reached its DFAIL with the storage freed underneath
       it. Neither is deleted: the emptiness is asserted in message_port_init, where it holds, and the storage
       is released by the last port to be finalized. See the table's own comment. */
}
