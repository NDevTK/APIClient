/* window.postMessage — HTML §9.3.3 "Posting messages", the WINDOW POST MESSAGE STEPS.
 *
 * WHAT IT IS FOR HERE. Two things, and the second is the one that matters to this project.
 *
 * The first is that it is ordinary: a great deal of real code posts to its OWN window to defer work past the
 * current task, and a bundle that does it and never receives its message has stopped running. That case is
 * complete below.
 *
 * The second is that `message` is one of the platform's richest ATTACKER SOURCES, and this file declares both
 * halves of it: `message.data` and `message.origin`, with the delivery mechanism a reproduction is built from
 * (SRC_DELIVER_CROSS_DOCUMENT_MESSAGE — the attacker holds the victim open in a document of their own and
 * posts to it, which is neither a navigation nor a §S(b) plant).
 *
 * THE TWO ARE DIFFERENT KINDS OF ATTACKER INPUT AND THE WHOLE VERDICT TURNS ON THAT. The attacker WRITES
 * `data`; the attacker only OWNS `origin`, because §9.3.2.2 "User agents" makes the browser stamp it — "the
 * integrity of this API is based on the inability for scripts of one origin to post arbitrary events … to
 * objects in other origins" — and §9.3.2.1 "Authors" is why every real bundle checks it. So `origin` is
 * declared a PRINCIPAL (solver/concolic.h): a forgeable check (endsWith, includes, startsWith) pins nothing
 * and is SOLVED, while an exact `===` PINS the principal and is a demand no cross-document attacker can meet,
 * which suppresses the finding rather than emitting a PoC that cannot be delivered.
 *
 * WHICH MESSAGES ARE ATTACKER INPUT IS DECIDED AT ONE PLACE — window_message_deliver_remote, against this
 * agent's own origin. An agent is origin-keyed, so a post from inside it is the app posting to itself and its
 * data is the app's own value; a routed post from a CROSS-ORIGIN document is, from the receiver's side, exactly
 * what an attacker's post would be, so its `origin` and its `data` are both unknown external input carrying
 * what arrived as their example.
 *
 * WHAT IS STILL MISSING IS THE ATTACKER'S OWN POST, AND IT IS THE TRUSTED ZONE'S TO MAKE. This file's inbound
 * half is complete — window_message_route takes a routed record and window_message_deliver_remote turns it
 * into §9.3.3's step 8 task — but nothing ever ROUTES one from a document the attacker controls, so on a real
 * page the two sources above are minted only when a cross-origin peer instance posts. The engine may not
 * manufacture one: SECURITY.md keys the sending origin on the trusted zone precisely because an origin the
 * untrusted engine chose would defeat every `event.origin` check in every bundle. What is owed is a routed
 * delivery whose sender is an attacker document at an origin the trusted zone stamps; nothing in this file
 * changes when it arrives.
 *
 * THE TARGET IS A NAVIGABLE'S ACTIVE WINDOW, WHICH IS WHY IT TAKES A WindowProxy. §9.3.3's steps are given a
 * targetWindow and the member runs them on `this`; with a cross-document target that Window is in
 * another agent — under this engine's one-WASM-instance-per-document rule, another WASM instance. The proxy is
 * where that indirection lives (see window_proxy.c) and it is why `source` on the event is a proxy rather than
 * a Window: the receiver holds a handle to the SENDER'S NAVIGABLE, not to a Window object it could never have.
 *
 * WHAT IS NOT BUILT, AND IS A CRASH RATHER THAN A WRONG ANSWER. A post whose target proxy names another
 * instance travels — the message, its origin, and the SENDING FLOW'S WORLD, because two arms of a fork post two
 * different messages and the receiver must not merge them into one timeline — but its TRANSFER LIST does not.
 * §2.7.7 has detached the transferables by the time the notice is built, and a holder is a live JS value of
 * this instance, so sending the bytes alone would destroy them silently. window_message_send_remote aborts
 * there instead, naming the holder each kind needs. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/structured_clone.h"
#include "core/events/event_target.h"
#include "core/events/message_event.h"
#include "core/frame/window_message.h"
#include "core/frame/window_proxy.h"
#include "core/dom/document.h"
#include "core/url/url.h"
#include "solver/concolic.h"
#include "solver/engine.h"
#include "solver/flow.h"
#include "solver/world.h"

/* THIS DOCUMENT'S WindowProxy is §7.2.3's ONE-per-navigable, and it is READ from where that rule lives
   rather than cached here: a same-origin child is a second realm in this agent, so a file-scope copy would be
   whichever document installed last and `e.source` from one document would have compared equal to the other's.
   The document handle is the identity, and window_proxy.c owns the mapping. */
static JSValueConst win_proxy(JSContext *ctx) { return document_window_proxy(ctx); }
static JSValue g_deliver_fn = JS_UNDEFINED;
static int     g_deliver_stepid = -1;   /* §9.3.3's queued delivery task, as a machine */

/* One queued post: the message, the origin check it still owes, and the target. Held as a JS Array for the
   reason a port's queue is one — it is frontier data, it has to park, and the collector should own it. */
/* PQ_SOURCE IS CAPTURED AT THE POST, not read at the delivery. §9.3.3 step 8.3's `source` is the INCUMBENT
   settings object's global — the window that CALLED postMessage — and the delivery task has no way to know
   that later: its callee is one function object for the agent, so the ctx it runs under is whichever realm
   minted it, and reading the sender from there named the first document for every post in the instance. This
   is §scheduler's rule that an operation which becomes a work item takes its inputs with it. */
/* PQ_SENDER_ORIGIN IS THE EVENT'S `origin`, AND IT TRAVELS RATHER THAN BEING READ AT DELIVERY. Within one
   instance it is always this agent's own — an agent is origin-keyed — which is why it could be a module static
   for as long as every post was local. A post from ANOTHER instance is cross-origin by construction, its
   origin is stamped by the trusted zone, and `event.origin` is the check every bundle in this corpus writes:
   answering it from the receiver's own origin would tell a page that a foreign message came from itself. */
enum { PQ_DATA = 0, PQ_HOLDERS, PQ_TARGET_ORIGIN, PQ_SOURCE, PQ_SENDER_ORIGIN, PQ_N };

/* §9.3.3 step 8.1: the origin check happens IN THE TASK, not at the call — the target may have navigated
   between the post and the delivery, and the standard checks the origin it has THEN. `want` is the serialized
   target origin, or NULL for "*"; `have` is the target document's ORIGIN RECORD.
   THE TWO SIDES ARE NOT THE SAME KIND OF THING, and that asymmetry is the standard's rather than a shortcut:
   `targetOrigin` reaches this engine as a string — the page wrote one, or the trusted zone stamped one on a
   routed message — and a string can only ever name a TUPLE origin, because §7.1.1's opaque origin has "no
   serialization it can be recreated from". So the comparison is §7.1.1 step 2 with one side still in bytes,
   which is what origin_is_serialized_tuple is; an opaque target refuses every `want`, "null" included. */
static bool origin_matches(const char *want, const Origin *have)
{
    if (!want) return true;                       /* "*" — any origin */
    return origin_is_serialized_tuple(have, want);
}

/* §9.3.3's QUEUED TASK, AS A MACHINE. The window post message steps queue ONE global task on the posted
 * message task source and its LAST step is "fire an event named message at targetWindow" — synchronous, in the
 * task the origin check and the deserialize happened in. So the dispatch is event_target_fire_run, the request
 * reach into §2.9, and not a second enqueue: enqueued again, a `message` listener ran behind every task already
 * standing, so `frame.postMessage(x, "*"); setTimeout(f, 0)` ran `f` first. A plain C callee had no other
 * option, because a fire it cannot park on is a fire it must queue. */
#define WM_DELIVER_STAGES(X) \
    X(WD_DESERIALIZE, "HTML §9.3.3 the window post message steps step 8.1-8.6 (the targetOrigin check, and " \
                      "StructuredDeserializeWithTransfer into the target window's realm)") \
    X(WD_FIRE,        "HTML §9.3.3 the window post message steps step 8.7 (fire an event named message at " \
                      "targetWindow, using MessageEvent)")
enum { WM_DELIVER_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const WM_DELIVER_STEPS[] = { WM_DELIVER_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr   hdr;
    uint8_t     fphase;   /* the fire request's own phase */
    JSValue     ev;       /* the MessageEvent, minted at WD_DESERIALIZE and held across the suspension (owned) */
    EventFireCb cb;       /* the fire request's buffer — the type carries §2.9's argument count */
} WmDeliverTask;

static int js_window_deliver_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    WmDeliverTask *s = st;
    JSValueConst target = step_arg(&s->hdr, 0);   /* the target WindowProxy */
    JSContext *tctx;
    int r;

    /* §7.5.10 STEP 7 FROM THE OTHER SIDE — "remove any tasks whose document is document from any task queue
       (without running those tasks)". This task's document is the TARGET's and the task is standing on the
       SENDER's flow queue, which is not a queue that walk can reach: it is keyed on the realm that ENQUEUED the
       job (JS_DropJobsForContext / engine.c's engine_drop_jobs), and this one was enqueued by the poster. So
       the same fact is stated here, where the task runs, and it is a POSITIVE statement rather than a guard: a
       navigable whose active document was destroyed has no Window to fire at and no listeners left to fire.
       ASKED AT EVERY STAGE, not only at the first: the destruction may be performed by one of this very
       document's own `message` listeners (`frameElement.remove()` in a handler), and the listeners after it
       belong to a document that no longer exists. Without this the fire's window_proxy_realm asks a destroyed
       navigable for an active document and crashes naming a capability that is not the missing one. */
    if (window_proxy_destroyed(target)) {
        JS_FreeValue(ctx, cb_result);
        JS_FreeValue(ctx, s->ev);
        s->ev = JS_UNDEFINED;
        return JS_STEP_DONE;
    }
    /* AND IT IS ASKED BEFORE THE DISPATCH, not after: STEP_DISPATCH jumps straight to the armed stage, so
       anything written between it and the first STEP_ARM is code no entry ever executes. */
    STEP_DISPATCH(WM_DELIVER_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(WD_DESERIALIZE);
    {
        JSValueConst entry = step_arg(&s->hdr, 1);
        StructuredWithTransfer swt;
        JSValue buf, want, data, source, sender_origin, ports = JS_UNDEFINED;
        const char *want_s = NULL;
        size_t blen = 0;
        int k;

        JS_FreeValue(ctx, cb_result);
        /* EVERY OWNED FIELD BEFORE THE FIRST THING THAT CAN FAIL — the failure path tears this machine down
           through js_window_deliver_visit, which frees exactly what the state holds. */
        s->ev = JS_UNDEFINED;
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
        s->fphase = 0;
        want = JS_GetPropertyUint32(ctx, entry, PQ_TARGET_ORIGIN);
        if (!JS_IsNull(want)) {
            want_s = JS_ToCString(ctx, want);
            if (!want_s) { JS_FreeValue(ctx, want); return JS_STEP_ABRUPT; }
        }
        /* The origin the target has NOW — read through the proxy, so a flow that navigated it sees its own. */
        if (!origin_matches(want_s, window_proxy_origin(target))) {
            if (want_s) JS_FreeCString(ctx, want_s);
            JS_FreeValue(ctx, want);
            return JS_STEP_DONE;   /* §9.3.3 step 8.1: not same origin, so nothing is delivered */
        }
        if (want_s) JS_FreeCString(ctx, want_s);
        JS_FreeValue(ctx, want);

        /* §9.3.3 step 8: THE DESERIALIZE AND THE EVENT BELONG TO THE TARGET'S REALM. The standard says "in
           targetWindow's relevant realm" for both, and it is not pedantry here: the deserialized objects and
           the MessageEvent are handed to the RECEIVING document's code, so building them under the delivery
           callee's own realm gave a child document a message whose every object belonged to the parent. */
        tctx = window_proxy_realm(ctx, target);
        DCHECK(tctx != NULL, "a window message was delivered to a navigable this agent holds no realm for");
        source = JS_GetPropertyUint32(ctx, entry, PQ_SOURCE);   /* the sender's proxy, taken at the post */
        /* §9.3.3 STEP 8.2's `origin`, HANDED ON AS THE VALUE IT IS. It used to be taken through JS_ToCString
           and re-minted inside the event, which was a round trip with nothing to gain from it — and which
           stopped being merely wasteful the moment the queue could hold an UNKNOWN one: a message from a
           CROSS-ORIGIN document carries an origin this engine may not decide (§9.3.2.2 "User agents"), so the
           slot holds a concolic, and coercing one is exactly what the unknown boundary refuses. */
        sender_origin = JS_GetPropertyUint32(ctx, entry, PQ_SENDER_ORIGIN);
        DCHECK(JS_IsString(sender_origin) || concolic_is(sender_origin),
               "a queued window message carried no sender origin — §9.3.3 step 8.2's `origin` is the one field "
               "a page's check is written against, and it is either this agent's serialization or the unknown "
               "principal of a document outside it");
        buf = JS_GetPropertyUint32(ctx, entry, PQ_DATA);
        swt.holders = JS_GetPropertyUint32(ctx, entry, PQ_HOLDERS);
        swt.data.buf = JS_GetArrayBuffer(ctx, &blen, buf);
        swt.data.len = blen;
        DCHECK(swt.data.buf != NULL, "a queued window message held something that is not the bytes it stored");
        data = structured_deserialize_transfer(tctx, &swt, &ports);
        JS_FreeValue(ctx, buf);
        JS_FreeValue(ctx, swt.holders);

        /* §9.3.3 STEP 8.5's `messageClone`, AS AN ATTACKER SOURCE — the reason this file's header calls
           `message` one of the platform's richest.
           WHICH MESSAGES: exactly those whose ORIGIN IS UNKNOWN, which is the same fact read once rather than
           a second flag beside it. window_message_deliver_remote decides it, where the trusted zone's stamp is
           in hand and this agent's own origin can be compared against it; a post from THIS agent (an
           origin-keyed cluster — every document in it is this principal's) carries a serialization and its
           data is the app's own value, which must stay concrete or every same-origin frame's protocol would
           lose its example values for nothing.
           THE ARRIVED VALUE IS THE EXAMPLE, so §Attacker-sources' one-value rule holds here as everywhere: the
           handler's `if (e.data.type === "resize")` forks BOTH arms — which is what reaches the gated code —
           while the arm the real message took keeps the bytes that peer actually sent. */
        if (concolic_is(sender_origin) && !JS_IsException(data))
            data = concolic_source_wrap(tctx, MESSAGE_DATA_SHAPE, MESSAGE_DATA_SRC, data);
        if (JS_IsException(data)) {
            JS_FreeValue(tctx, JS_GetException(tctx));
            JS_FreeValue(tctx, ports);
            s->ev = message_event_new(tctx, "messageerror", JS_UNDEFINED, sender_origin, source, JS_UNDEFINED);
        } else {
            /* §9.3.3 steps 8.2 and 8.3: the origin is the SENDER'S and the source is the sender's WindowProxy
               — which is what a handler's `event.origin` check is about, and the whole reason this event is
               worth anything to the solver. `ports` is `newPorts`: the MessagePorts among the
               [[TransferredValues]], so a transferred ArrayBuffer arrives inside `data` and is not one. */
            JSValue new_ports = message_event_ports_of(tctx, ports);
            JS_FreeValue(tctx, ports);
            if (JS_IsException(new_ports)) {
                JS_FreeValue(tctx, data);
                JS_FreeValue(ctx, sender_origin);
                JS_FreeValue(ctx, source);
                return JS_STEP_ABRUPT;
            }
            s->ev = message_event_new(tctx, "message", data, sender_origin, source, new_ports);
            JS_FreeValue(tctx, data);
            JS_FreeValue(tctx, new_ports);
        }
        JS_FreeValue(ctx, sender_origin);
        JS_FreeValue(ctx, source);
        if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; return JS_STEP_ABRUPT; }
        STEP_GOTO(s->hdr.stage, WD_FIRE, &s->fphase, NULL);
        return JS_STEP_YIELD;
    }

    STEP_ARM(WD_FIRE);
    /* THE REALM AND THE WINDOW ARE RESOLVED TOGETHER, at the moment the dispatch is made — a pair that
       disagreed would fire this document's event under another document's ctx. Re-reading them across the
       yield above is sound where holding one of the two would not be: the proxy's active document is per-flow
       state under the COW delta, and this flow has not navigated it between the two stages, so what another
       flow's navigation did is invisible here by construction. */
    tctx = window_proxy_realm(ctx, target);
    DCHECK(tctx != NULL, "§9.3.3's delivery resumed at its fire with no realm for the target navigable");
    DCHECK(JS_IsObject(s->ev), "§9.3.3's delivery resumed at its fire with no MessageEvent to dispatch");
    {
        JSValue w = window_proxy_window(tctx, target);

        r = event_target_fire_run(tctx, &s->fphase, STEP_CB(s->cb), w, s->ev, JS_UNDEFINED, cb_result,
                                  NULL, out_cb, out_argc);
        JS_FreeValue(tctx, w);
    }
    if (r > 0) return r;
    if (r < 0) return JS_STEP_ABRUPT;
    JS_FreeValue(tctx, s->ev);
    s->ev = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static void js_window_deliver_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    WmDeliverTask *s = st;
    int k;

    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, k) v->val(ctx, &s->cb[k]);
}

static const JSTrampStepDef js_window_deliver_def = {
    sizeof(WmDeliverTask), js_window_deliver_step, NULL, 0,
    .visit = js_window_deliver_visit,
    .algorithm = "HTML §9.3.3 the posted message task source delivery task",
    .steps = WM_DELIVER_STEPS
};

/* §9.3.3 STEP 8 ACROSS INSTANCES — an EMISSION, and every part of that word is load-bearing.
 *
 * The bytes are already serialized and immutable, so nothing has to un-send this when the posting flow parks or
 * is outranked, and it needs no COW capture: that is why a message crosses as an emission rather than as shared
 * mutation. What the engine cannot do is ROUTE it — only the trusted zone knows which instance holds the target
 * document — and what it MUST NOT do is stamp the sender's origin. `event.origin` is the check every bundle in
 * this corpus writes; an origin the untrusted engine chose would defeat all of them, and would make the solver
 * report cross-origin XSS that is not real and miss the ones that are. So the notice carries no origin at all
 * and the offscreen adds the one it knows, exactly as SECURITY.md keys authorization on `sender.tab.url`.
 *
 * THE SENDING FLOW'S WORLD TRAVELS WITH IT, with its ancestry, because two arms of a fork post two messages
 * belonging to two contradictory worlds — and a peer that received only the bytes would merge them into a
 * timeline neither sender was in. What the receiver does with the vector is engine_route's: the delivery is
 * made by each live flow of the receiving document, in that flow's own timeline (the page's listener was
 * registered by a script and lives in the delta of the flow that ran it), CONJOINED with this instance's
 * segment for the world named here — which the ancestry is what materializes, by forking the nearest ancestor
 * the receiver already holds.
 *
 * THE BYTES RIDE AS BASE64 because the notice channel is text. The codec is the ENGINE'S (JS_Base64Encode) —
 * the one the spec already made it implement for `btoa` — rather than a second one grown here. */
static void window_message_send_remote(JSContext *ctx, JSValueConst target, JSValueConst entry)
{
    Flow *f = flow_running();
    JSValue buf = JS_GetPropertyUint32(ctx, entry, PQ_DATA);
    JSValue want = JS_GetPropertyUint32(ctx, entry, PQ_TARGET_ORIGIN);
    const char *want_s = NULL;
    uint8_t *bytes;
    size_t blen = 0, b64_cap, n;
    char world[512];
    char *op, *b64;
    JSValue holders = JS_GetPropertyUint32(ctx, entry, PQ_HOLDERS);
    uint32_t nheld = structured_transfer_len(ctx, holders);

    JS_FreeValue(ctx, holders);
    /* A TRANSFER DOES NOT CROSS AN INSTANCE YET, AND A DROPPED ONE IS WORSE THAN A THROW. §2.7.7 has already
       DETACHED every object in the transfer list by the time this runs — the page no longer has its port or its
       buffer — so sending the bytes without the holders delivers a message whose `ports` is empty and destroys
       the transferables with nothing to say so. What is missing is named rather than approximated: a holder is
       a live JS value belonging to this instance (a port's queue, its head and its entangled REMOTE PORT), and
       none of those crosses as text. A MessagePort holder needs the remote-port handle window_proxy.c's
       remote_object.c already builds for a WindowProxy, so the arriving port entangles back across the
       boundary; an ArrayBuffer holder is bytes and needs only a second base64 field on this record. */
    DCHECK(nheld == 0,
           "postMessage transferred an object to ANOTHER INSTANCE — the transfer steps have already detached it "
           "here, and the routed record has no field to carry a data holder, so the transferable would be "
           "destroyed and the delivered event's `ports` would be empty. Build the cross-instance holder: a "
           "remote-port handle for a MessagePort, a base64 field for an ArrayBuffer");
    (void)nheld;   /* the count is the assert's, and the assert is compiled out of a release build */
    DCHECK(f != NULL, "a cross-instance post was made outside a flow — there would be no world to carry");
    bytes = JS_GetArrayBuffer(ctx, &blen, buf);
    DCHECK(bytes != NULL, "a queued window message held something that is not the bytes it stored");
    world_serialize(f->world, world, sizeof world);

    b64_cap = JS_Base64EncodedSize(blen) + 1;
    b64 = malloc(b64_cap);
    CHECK(b64 != NULL, "window message: OOM encoding a cross-instance post — a dropped message is a delivery "
                       "the peer never makes and a handler this solver never reaches");
    n = JS_Base64Encode(b64, b64_cap, bytes, blen);
    CHECK(n > 0 || blen == 0, "the base64 buffer was sized wrong for this message");
    b64[n] = 0;

    /* §9.3.3 step 8.1's origin check is the RECEIVER'S to make — it tests the target's origin at DELIVERY, and
       the target may navigate between this call and then — so the requested target origin travels rather than
       being resolved here. "*" is the spec's any-origin, and an absent one means the same. */
    if (!JS_IsNull(want) && !JS_IsUndefined(want))
        want_s = JS_ToCString(ctx, want);
    b64_cap = strlen(world) + strlen(world_doc_name(window_proxy_doc(target))) + strlen(want_s ? want_s : "*") + n + 64;
    op = malloc(b64_cap);
    CHECK(op != NULL, "window message: OOM building the cross-instance post notice");
    snprintf(op, b64_cap, "windowproxy.post\t%s\t%s\t%s\t%s",
             world_doc_name(window_proxy_doc(target)), world, want_s ? want_s : "*", b64);
    engine_host_notify(ctx, op);
    free(op);
    free(b64);
    if (want_s) JS_FreeCString(ctx, want_s);
    JS_FreeValue(ctx, want);
    JS_FreeValue(ctx, buf);
}

/* THE INBOUND HALF: a message the trusted zone ROUTED here from another instance. It is the same §9.3.3 step 8
   task the local path enqueues, built from the wire fields instead of from a call — which is the point, because
   a delivery that took a different path would be a second implementation of the one thing this file does.
 *
 * THE ORIGIN IS THE HOST'S TO SUPPLY and this function takes it as an argument rather than deriving it: the
 * engine is untrusted (SECURITY.md), so an origin it computed for a foreign message would be a forgery every
 * `event.origin` check in every bundle would then trust.
 *
 * THE SOURCE IS A REMOTE PROXY for the sending document, which is what makes `event.source.postMessage(...)` —
 * the reply half of every real messaging protocol — resolve to a route back rather than to null.
 *
 * THE WORLD IS ALREADY INSTALLED WHEN THIS RUNS, and WHICH world is the caller's statement rather than this
 * file's — the two callers answer it differently because their documents hold state differently. The solver
 * engine delivers from inside a RECEIVING FLOW (engine.c's flow_deliver, with that flow's delta applied and the
 * sender's segment asserted to add nothing to it), because a document's state there IS its flows. The WPT host
 * has no per-flow timelines — its documents' scripts run with captures dropped, so their listeners are baseline
 * — and installs the SENDING world's segment, which is the same answer for a document whose state is baseline.
 * What both owe is the same one thing: the world installed must be one in which the receiving document's
 * listeners exist, and neither this function nor any local check can decide that. */
void window_message_deliver_remote(JSContext *ctx, const char *sender_doc, const char *sender_origin,
                                   const char *target_origin, const uint8_t *bytes, size_t len)
{
    JSValue entry, buf, target, source;

    DCHECK(sender_doc && *sender_doc, "a routed message named no sending document — `event.source` would have "
                                      "nothing to name and a reply could not be routed back");
    DCHECK(sender_origin && *sender_origin,
           "a routed message carried no origin — only the trusted zone may stamp one, and a delivery without it "
           "would have to invent the one field a page's check is written against");
    target = win_proxy(ctx);
    DCHECK(!JS_IsUndefined(target), "a message was routed to an instance whose own WindowProxy does not exist");

    buf = JS_NewArrayBufferCopy(ctx, bytes, len);
    entry = JS_IsException(buf) ? buf : JS_NewArray(ctx);
    CHECK(!JS_IsException(entry), "window message: OOM receiving a routed message");
    /* §7.2.3's proxy for the SENDER, minted remote because that document lives in the instance that sent
       this. Its origin is the stamped one — the same value `event.origin` reports, and the value a
       same-origin check inside the peer would be made against. */
    /* THE STAMPED SERIALIZATION BECOMES A RECORD HERE, and it is the peer's own: origin_parse mints a fresh
       opaque origin for "null", so a sandboxed peer is same origin with nothing on this side — which is right,
       since two Documents sharing ONE opaque origin share an agent cluster and therefore an instance. */
    /* THROUGH THE ONE DOOR, which is also the fix for an identity this file was quietly breaking: a fresh
       proxy per delivery made `a.source === b.source` false for two messages from ONE document, and a reply
       posted back through the second reached the same navigable by a second name. The FIRST delivery from a
       document mints; every one after it answers with that object. */
    source = window_proxy_for_document(ctx, world_doc_intern(sender_doc), origin_parse(sender_origin), NULL,
                                       JS_UNDEFINED, JS_NULL);
    CHECK(!JS_IsException(source), "the sending document's WindowProxy could not be allocated");
    JS_SetPropertyUint32(ctx, entry, PQ_DATA, buf);
    /* NO HOLDERS ARRIVE, because none can be SENT: window_message_send_remote aborts on a transfer rather than
       dropping one, so a routed record carrying holders cannot exist. The empty list is that fact, not a
       default — when the cross-instance holder is built, it is read here. */
    JS_SetPropertyUint32(ctx, entry, PQ_HOLDERS, JS_UNDEFINED);
    JS_SetPropertyUint32(ctx, entry, PQ_TARGET_ORIGIN,
                         (target_origin && strcmp(target_origin, "*")) ? JS_NewString(ctx, target_origin)
                                                                       : JS_NULL);
    JS_SetPropertyUint32(ctx, entry, PQ_SOURCE, source);
    /* §9.3.3 STEP 8.2's `origin` — AND THE ONE PLACE THIS ENGINE DECIDES WHETHER A MESSAGE IS ATTACKER INPUT.
     *
     * THE TEST IS THE PRINCIPAL, AND IT IS THE SPEC'S OWN. §9.3.2.2 "User agents" states the basis of the
     * whole API: "the integrity of this API is based on the inability for scripts of one origin to post
     * arbitrary events … to objects in other origins". So a message from a document that is NOT same origin
     * with this agent is, from the receiver's side, indistinguishable from one an attacker sent — the attacker
     * registers a domain and posts from it, which is a capability every cross-origin sender already has — and
     * its `data` and its `origin` are both unknown external input. A SAME-ORIGIN sender is this app: an agent
     * is origin-keyed, so its bytes are the app's own values and concretising them is right.
     *
     * THE STAMPED ORIGIN IS THE EXAMPLE AND NOT THE ANSWER. It is a real measurement — the trusted zone made
     * it, and SECURITY.md is why this engine may not — so the concolic carries it and a branch marks the arm
     * that origin really takes as the primary one; what it must not do is DECIDE the branch, because the
     * attacker chooses which origin posts. Handing the serialization over concrete would let
     * `if (e.origin !== TRUSTED) return;` prune the arm every real postMessage XSS lives on.
     *
     * `origin_is_serialized_tuple` IS THE COMPARISON AND NOT A strcmp, for the reason origin_matches states one
     * screen up: §7.1.1's opaque origin "has no serialization it can be recreated from", so an opaque agent is
     * same origin with nothing — including another "null" — and the tuple test is the only side of this that
     * can be asked with one operand still in bytes. */
    if (origin_is_serialized_tuple(origin_agent(), sender_origin)) {
        JS_SetPropertyUint32(ctx, entry, PQ_SENDER_ORIGIN, JS_NewString(ctx, sender_origin));
    } else {
        JSValue org = concolic_source_wrap(ctx, MESSAGE_ORIGIN_SHAPE, MESSAGE_ORIGIN_SRC,
                                           JS_NewString(ctx, sender_origin));
        /* A CONFORMANCE HOST HAS NO SOURCE OVERLAY and gets the string back unchanged (solver/concolic.h), so
           this line is not a fork in behaviour — it is the same delivery with the solver's half added where
           the solver is installed. What it must never be is UNDEFINED: that would be the one field every
           bundle's security check reads, arriving absent. */
        CHECK(!JS_IsException(org),
              "window message: the sending origin of a routed cross-origin message could not be allocated");
        JS_SetPropertyUint32(ctx, entry, PQ_SENDER_ORIGIN, org);
    }
    {
        JSValueConst args[2];
        args[0] = target;
        args[1] = entry;
        JS_EnqueueCallTask(ctx, g_deliver_fn, 2, args);
    }
    JS_FreeValue(ctx, entry);
}

/* THE RECORD'S OWN TAIL, READ WHERE IT WAS WRITTEN. window_message_send_remote builds
   `windowproxy.post\t<target>\t<world>\t<targetOrigin>\t<base64>`; the first two fields are the TRANSPORT's
   (which instance, whose timeline) and are read by the router, and everything after them is this file's — so
   this is where they are taken apart. A host that split them itself was the second half of the format, written
   where nothing can check it against the writer, and both hosts that had one had written a different one.
   `tail` is `<targetOrigin>\t<base64>`. */
void window_message_route(JSContext *ctx, const char *tail, const char *sender_doc, const char *sender_origin)
{
    const char *b64;
    char *want;
    uint8_t *bytes;
    size_t b64n, cap, blen;
    int err = 0;

    DCHECK(tail != NULL, "a routed window message carried no payload fields");
    b64 = strchr(tail, '\t');
    DCHECK(b64 != NULL, "a routed window message had no base64 field — the record was built by something that "
                        "is not window_message_send_remote, so the two halves of this format disagree");
    want = strndup(tail, (size_t)(b64 - tail));
    CHECK(want != NULL, "window message: OOM reading a routed message's target origin");
    b64++;
    b64n = strlen(b64);
    cap = JS_Base64DecodedMax(b64n) + 1;
    bytes = malloc(cap);
    CHECK(bytes != NULL, "window message: OOM receiving a routed message");
    blen = JS_Base64Decode(bytes, cap, b64, b64n, &err);
    CHECK(err == 0, "a routed message did not survive the text channel it crossed — the bytes the peer sent and "
                    "the bytes this instance decoded are not the same message");
    window_message_deliver_remote(ctx, sender_doc, sender_origin, want, bytes, blen);
    free(bytes);
    free(want);
}

/* §9.3.3's `WindowPostMessageOptions.targetOrigin` DEFAULT, STATED ONCE and read from two places that must not
   drift. The declaration places it on the converted dictionary (§3.2.17 step 4.1.5), so `postMessage(m, {})`
   arrives here carrying it; a call passing NO second argument never reaches the conversion at all, and §3.6
   step 15 answers that with `optional WindowPostMessageOptions options = {}`'s own default — a dictionary
   whose every member takes ITS default, which is this same value. `postMessage(m)` therefore posts to the
   sender's own origin, not to "*", which is what a second literal here would quietly have made it. */
#define POST_TARGET_ORIGIN_DEFAULT "/"

/* §9.3.3's `postMessage(message, options)` and the legacy `postMessage(message, targetOrigin, transfer)`.
 *
 * THE ARGUMENTS ARRIVE CONVERTED, AND THE OVERLOAD IS ALREADY RESOLVED. Both of this member's second-argument
 * shapes were read from C: `targetOrigin` with a JS_GetPropertyStr and a ToString on whatever came back, and
 * the transfer list with a `length`-and-indices walk — a getter, a Proxy trap and a `toString` of the page's,
 * in an activation with no flow base under any of them. The declaration performs all three now
 * (IDL_USVSTRING_OR_DICT for §3.6's split, IDL_SEQUENCE_OBJECT for §3.2.21's iterator protocol), so what this
 * body sees at position 1 is either a real STRING or an engine-built OBJECT holding converted members, and
 * asking which is reading the arm back off a value the engine made rather than duck-typing the page's. */
static JSValue js_window_post(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst second = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue transfer = JS_UNDEFINED, tov, want = JS_NULL, entry, buf;
    StructuredWithTransfer swt;
    const char *to = NULL;
    /* §9.3.3 POSTS TO THE WINDOW IT WAS CALLED ON, and the receiver is how that is known. This discarded
       `this_val` and always delivered to THIS window, so `iframe.contentWindow.postMessage(m, "*")` — which is
       most of the real uses of this method — fired the message at the sender. Nothing could be routed by a
       target that was never read. `window.postMessage(...)` calls it on the global, which is this navigable's
       own proxy; a call on a proxy targets that proxy. */
    JSValueConst target = window_proxy_is(this_val) ? this_val : win_proxy(ctx);

    (void)magic;
    DCHECK(!JS_IsUndefined(target), "postMessage ran before this window's WindowProxy existed");
    /* UNKNOWN EXTERNAL INPUT CROSSES A DECLARED POSITION AS ITSELF, which means §3.6 chose NO overload for it
       and there is no arm to read back. It is not a wrong value to route past — the target origin decides
       §9.3.3 step 8.1's same-origin check, so a concolic one makes BOTH deliveries feasible and the post is a
       FORK rather than one queue entry. That mechanism does not exist, and the alternative every earlier
       version of this line took was to fall into the dictionary arm and read `targetOrigin` off the attacker's
       own value, which manufactures a plausible datum out of a measurement nobody made. */
    if (concolic_is(second))
        DFAIL("postMessage was given a CONCOLIC target origin — §9.3.3 step 5 parses it and step 8.1 compares "
              "it against the target document's origin, so an unpinned one leaves both the delivered and the "
              "dropped arm feasible. Fork the post: queue the entry under each arm of the origin comparison, "
              "the way a branch on unknown external input forks anywhere else, rather than deciding it here");
    /* THE OVERLOAD, READ BACK OFF THE CONVERTED VALUE. A STRING is §3.6's longer entry — the legacy
       `(message, targetOrigin, transfer)` — and an OBJECT is the options dictionary the declaration built.
       THERE IS NO THIRD STATE, and that is the declaration's doing rather than this body's: `optional
       WindowPostMessageOptions options = {}` means an omitted second argument IS a dictionary carrying every
       member's default, which the argument machine now materializes (core/idl_args.c,
       idl_type_is_dictionary). The sentence that used to stand here said `undefined` reached this body and
       that idl_dict_get answered the defaults for it — it does not: it answers UNDEFINED, so
       `window.postMessage(msg)` read an undefined target origin where §9.3.3 reads the IDL's "/". The assert
       is what keeps that from coming back silently. */
    DCHECK(JS_IsString(second) || JS_IsObject(second),
           "postMessage's second argument reached the body as neither a string nor a dictionary — "
           "IDL_USVSTRING_OR_DICT resolves to one of those two, and an OMITTED one is the `= {}` dictionary "
           "rather than an absent argument");
    DCHECK(JS_IsString(second) || argc <= 2,
           "postMessage was called with three arguments and a second that is not a string — §3.6 step 4 "
           "removes the options entry at that arity, so the conversion owed a USVString here");
    if (JS_IsString(second)) {
        tov = JS_DupValue(ctx, second);
        transfer = argc > 2 ? JS_DupValue(ctx, argv[2]) : JS_UNDEFINED;
    } else {
        tov = idl_dict_get(ctx, second, "targetOrigin");
        transfer = idl_dict_get(ctx, second, "transfer");
    }
    if (JS_IsUndefined(tov)) tov = JS_NewString(ctx, POST_TARGET_ORIGIN_DEFAULT);
    if (JS_IsException(tov)) { JS_FreeValue(ctx, transfer); return JS_EXCEPTION; }
    DCHECK(JS_IsString(tov), "postMessage's target origin is not a string — the declaration converts it as a "
                             "USVString and places the IDL's own default when the page wrote none");
    /* §9.4.4's `= []`: no `transfer` member is a transfer list of nothing. That is the IDL's own default and
       not a hole filled at the reader — see js_structured_clone, which states the same thing about the same
       dictionary member. Everything downstream walks THIS array, which the engine built. */
    if (JS_IsUndefined(transfer)) {
        transfer = JS_NewArray(ctx);
        if (JS_IsException(transfer)) { JS_FreeValue(ctx, tov); return JS_EXCEPTION; }
    }
    DCHECK(JS_IsArray(transfer), "postMessage's transfer list is not the materialized sequence — "
                                 "IDL_SEQUENCE_OBJECT is what §3.2.21 builds, and reading the page's object "
                                 "again here would run its iterator a second time");
    /* A REAL STRING, so this runs none of the page's code — the ToString §3.2.12 owed happened at the
       conversion, on the tramp, where a `toString` could park. */
    to = JS_ToCString(ctx, tov);
    JS_FreeValue(ctx, tov);
    if (!to) { JS_FreeValue(ctx, transfer); return JS_EXCEPTION; }

    /* §9.3.3 steps 4 and 5. "*" is any origin; "/" is the SENDER's own (step 4); anything else is a URL
       whose origin is taken (step 5), and a URL that does not parse is a SyntaxError — which is thrown HERE,
       at the call, because it is the page's mistake and not the delivery's.
       THERE IS NO "THE PAGE NAMED NO ORIGIN" CASE. It used to read `to && …`, which made an absent
       `targetOrigin` mean "*" — the widest possible delivery, arrived at by a C null rather than by anything
       the IDL says. §9.3.3 has no such state: the member's default IS "/", so a page that writes neither
       argument posts to its own origin. */
    if (strcmp(to, "*") != 0) {
        if (!strcmp(to, "/")) {
            /* "/" IS THE SENDER'S OWN ORIGIN, and the queue carries a SERIALIZATION because this entry may
               cross an instance (window_message_send_remote reads this very slot). A serialization can name a
               tuple origin and nothing else, so an OPAQUE sender writing "/" is a request this queue cannot
               express — and it is a real page doing a legal thing (a `data:` document posting to itself), not
               an invariant violation, so what is missing is a mechanism and not a check. */
            if (origin_is_opaque(origin_agent()))
                DFAIL("§9.3.3's `/` target origin was used by a document whose origin is OPAQUE. `/` means THE "
                      "SENDER'S OWN ORIGIN, and §7.1.1 step 1 makes that origin same origin with itself, so the "
                      "delivery must succeed — but the post queue carries the requested origin as a "
                      "SERIALIZATION (it may cross an instance, where a record cannot go) and every opaque "
                      "origin serializes to \"null\", which is same origin with nothing. Build the queue slot "
                      "as a HANDLE for a local delivery and a serialization for a routed one, so §7.1.1 step "
                      "1 decides the local case by identity — see core/url/origin.h");
            want = JS_NewString(ctx, origin_serialized(origin_agent()));
        } else {
            UrlRecord rec;
            char *o;
            url_record_init(&rec);
            if (!url_parse(&rec, to, strlen(to), NULL)) {
                url_record_free(&rec);
                JS_FreeCString(ctx, to);
                JS_FreeValue(ctx, transfer);
                return JS_ThrowDOMException(ctx, "SyntaxError", "the target origin is not a URL");
            }
            o = url_serialize_origin(&rec);
            url_record_free(&rec);
            want = JS_NewString(ctx, o ? o : "null");
            free(o);
        }
    }
    JS_FreeCString(ctx, to);

    /* §9.3.3 step 7: serialize and transfer NOW — the DataCloneError belongs to this call. */
    if (structured_serialize_transfer(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, transfer, &swt) < 0) {
        JS_FreeValue(ctx, transfer);
        JS_FreeValue(ctx, want);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, transfer);

    buf = JS_NewArrayBufferCopy(ctx, swt.data.buf, swt.data.len);
    entry = JS_IsException(buf) ? buf : JS_NewArray(ctx);
    if (JS_IsException(entry)) {
        structured_with_transfer_free(ctx, &swt);
        JS_FreeValue(ctx, want);
        return JS_EXCEPTION;
    }
    JS_SetPropertyUint32(ctx, entry, PQ_DATA, buf);
    JS_SetPropertyUint32(ctx, entry, PQ_HOLDERS, JS_DupValue(ctx, swt.holders));
    JS_SetPropertyUint32(ctx, entry, PQ_TARGET_ORIGIN, want);
    /* §9.3.3 step 8.3's source, read HERE where the caller's realm is the one running. */
    JS_SetPropertyUint32(ctx, entry, PQ_SOURCE, JS_DupValue(ctx, win_proxy(ctx)));
    /* §9.1 "The MessageEvent interface" declares `origin` a USVString — "the serialization of the origin of
       the incumbent settings object" — so this is one of the places a serialization is the ANSWER rather than
       a lost identity. */
    JS_SetPropertyUint32(ctx, entry, PQ_SENDER_ORIGIN, JS_NewString(ctx, origin_serialized(origin_agent())));
    structured_with_transfer_free(ctx, &swt);

    /* THE RESOLVED TARGET, not this window. A task in THIS instance can only deliver to a navigable this
       instance holds; a proxy naming another document is ROUTED. */
    if (window_proxy_is_remote(target)) {
        /* A POST TO THIS INSTANCE'S OWN DOCUMENT MUST NEVER TAKE THE ROUTE. Routing one means emitting a notice
           and returning — so if the target is in fact local, the message is handed to a host that has nothing
           to deliver it to and the handler never runs, which is silent. The two facts are named together so a
           disagreement says WHICH document each side thinks this is. */
        DCHECK(window_proxy_doc(target) != world_local_doc(),
               "postMessage routed a message OUT of the instance that holds the target document — the proxy "
               "reports remote while naming this instance's own document, so the world registry and the proxy "
               "disagree about who hosts it and the delivery would be lost on the way out");
        window_message_send_remote(ctx, target, entry);
        JS_FreeValue(ctx, entry);
        return JS_UNDEFINED;
    }
    /* §9.3.3 step 8: a TASK on the posted message task source. Not a microtask — a page that posts and then
       awaits observes its own continuation first, which is the ordering the event loop's two halves exist for. */
    {
        JSValueConst args[2];

        args[0] = target;
        args[1] = entry;
        JS_EnqueueCallTask(ctx, g_deliver_fn, 2, args);
        JS_FreeValue(ctx, entry);
    }
    return JS_UNDEFINED;
}

static int g_id_post = -1;

/* §9.3.3's `postMessage` ON THIS REALM'S WindowProxy PROTOTYPE. It is declared into core/realm.h's list AFTER
   window_proxy's own entry, which is what makes the object it goes onto already exist. */
void window_message_install_proto(JSContext *ctx)
{
    JSValue proto = window_proxy_proto(ctx);

    DCHECK(g_id_post >= 0, "postMessage was installed before window_message_init declared it");
    /* THE OPTIONAL INDEX IS THE DECLARATION'S, and it is stated there (window_message_init) and nowhere else.
       A second `idl_optional_from(1)` used to sit on this line, in the per-REALM install — and that setter
       names the member the LAST DECLARATION made, which by install time is whichever component happened to
       declare last. So it did not restate this member's rule twice; it wrote it onto a stranger, once per
       realm. The declaration is the only place that knows which member is being described. */
    idl_install_method(ctx, proto, "postMessage", 1, g_id_post);
    JS_FreeValue(ctx, proto);
}

/* §9.3.3's `postMessage`, DECLARED ONCE PER AGENT — a member has one pool entry, and every realm's prototype
   carries that same one. */
void window_message_init(JSContext *ctx)
{
    /* §9.3.3's TWO OVERLOADS, AS ONE DECLARATION — the longest type list the effective overload set has, with
       the position the two split at carrying that split as its type:

           undefined postMessage(any message, USVString targetOrigin, optional sequence<object> transfer = []);
           undefined postMessage(any message, optional WindowPostMessageOptions options = {});

       §3.6 steps 3-4 remove the options entry the moment a third argument is passed, which is why position 1
       can be one row (IDL_USVSTRING_OR_DICT states exactly that) rather than a shape test in the body. */
    static const IdlArgType POST_ARGS[3] = { IDL_ANY, IDL_USVSTRING_OR_DICT, IDL_SEQUENCE_OBJECT };
    /* `dictionary WindowPostMessageOptions : StructuredSerializeOptions { USVString targetOrigin = "/"; };`
       §3.2.17 reads the INHERITED members first, so `transfer` (StructuredSerializeOptions, level 0) precedes
       `targetOrigin` (level 1) — an order no single sorted list produces, which is what `level` is for. */
    static const IdlDictMember POST_OPTS[] = {
        { "transfer",     IDL_SEQUENCE_OBJECT, false, NULL, 0 },
        { "targetOrigin", IDL_USVSTRING,       false, NULL, 1, NULL,
          IDL_DEFAULT_STRING, POST_TARGET_ORIGIN_DEFAULT },
    };

    /* THE DELIVERY TASK'S CALLEE IS THE AGENT'S — one function object for the whole runtime. It was minted in
       the per-DOCUMENT install, so a second same-origin realm overwrote it and the first realm's copy became
       unreachable with nothing to free it: JS_FreeRuntime's gc_obj_list walk counted it, which is the leak
       gate doing exactly its job. */
    /* THE DECLARATION LATCH, AND THIS COMPONENT WAS THE ONE WITHOUT ONE. Every other row on core/platform.c's
       release column asserts its own `_init` did not run twice; this one asserted nothing, so a second
       declaration would have overwritten the callee below with nothing left holding the first — the exact leak
       the paragraph under this one records, arriving by the other door. */
    DCHECK(g_id_post < 0, "window_message_init ran twice — §9.3.3's declaration is made once per AGENT");
    /* THE TWO ATTACKER SOURCES THIS COMPONENT OWNS. A source's browser delivery is a fact about the COMPONENT
       and not about a document, so it is declared here with the member and not in the per-realm install — the
       same placement core/frame/location.c states for its own two, and the reason a second same-origin
       document does not re-declare.
       THE ENCODE SET IS EMPTY, AND THAT IS A MEASURED FACT RATHER THAN AN UNFILLED COLUMN. §9.3.3 step 7 hands
       the message to StructuredSerializeWithTransfer and step 8.4 takes it back through
       StructuredDeserializeWithTransfer; neither transforms a string, so the bytes the attacker writes are the
       bytes the handler reads. That is the whole reason a postMessage breakout reproduces where the same
       candidate through `location.hash` dies on the fragment set encoding `<`.
       AND THE ORIGIN IS A PRINCIPAL, WHICH THE DATA IS NOT — the difference §9.3.2.1 "Authors" tells a page to
       rely on and §9.3.2.2 "User agents" says the API's integrity rests on. The attacker WRITES `data` and
       merely OWNS `origin`, so an equality gate on the first is solved and one on the second is a demand no
       cross-document attacker can meet (solver/concolic.h, concolic_declare_source_principal). */
    concolic_declare_source(WM_COMPONENT, MESSAGE_DATA_SRC, "", 0, SRC_DELIVER_CROSS_DOCUMENT_MESSAGE);
    concolic_declare_source(WM_COMPONENT, MESSAGE_ORIGIN_SRC, "", 0, SRC_DELIVER_CROSS_DOCUMENT_MESSAGE);
    concolic_declare_source_principal(WM_COMPONENT, MESSAGE_ORIGIN_SRC);
    g_deliver_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_window_deliver_def);
    DCHECK(g_deliver_stepid >= 0, "§9.3.3's delivery machine could not be declared against this runtime");
    g_deliver_fn = JS_NewCFunction2(ctx, NULL, "", 2, JS_CFUNC_step, g_deliver_stepid);
    CHECK(JS_IsFunction(ctx, g_deliver_fn), "the window delivery task's callee could not be allocated");
    g_id_post = idl_method_id_dict(ctx, POST_ARGS, 3, POST_OPTS,
                                   (int)(sizeof POST_OPTS / sizeof POST_OPTS[0]), js_window_post, 0);
    idl_optional_from(1);
    agent_state_id(WM_COMPONENT, &g_id_post, "§9.3.3's postMessage declaration, and the declaration latch");
    agent_state_value(WM_COMPONENT, &g_deliver_fn, "§9.3.3's delivery-task callee, one per agent");
    agent_state_id(WM_COMPONENT, &g_deliver_stepid, "§9.3.3's delivery task machine");
    realm_declare_intrinsic(window_message_install_proto);
}

void window_message_install(JSContext *ctx, JSValueConst global, const char *origin)
{

    /* THE ORIGIN IS THE AGENT'S — one record, in core/url/origin.c, and this file keeps no copy of it: an
       agent is origin-keyed, so a second document installing here is ordinary and a second ORIGIN would be two
       principals behind one instance, which SECURITY.md forbids. The host's per-document statement is CHECKED
       against the agent's rather than stored beside it. */
    DCHECK(origin != NULL && !strcmp(origin_serialized(origin_agent()), origin),
           "a second document was installed into this agent with a DIFFERENT origin — an agent is origin-keyed, "
           "so a cross-origin document is a second INSTANCE and never a second realm in this one");
    /* ONE DECLARATION, TWO PLACES IT IS REACHED — the global (`window.postMessage`) and every WindowProxy
       (`otherWindow.postMessage`). Declaring it twice would give the two spellings two members that can drift,
       and `window.postMessage === self.postMessage` would stop holding. The WindowProxy prototype is the
       AGENT's, so it takes the member once; the global is per realm. */
    idl_install_method(ctx, global, "postMessage", 1, g_id_post);
}


/* §9.3.3's delivery callee is ONE function object for the whole agent, so it is released against the RUNTIME —
   which is also what puts it on core/platform.h's release column instead of in each host's own teardown. */
void window_message_free(JSRuntime *rt)
{
    /* THE TWO SOURCE CLAIMS, GIVEN BACK. They are rows in solver/concolic.c's registry whose STORAGE is that
       component's array and whose CLAIM is this one's, so this release removes exactly this component's rows
       and concolic_free asserts, at the solver's own release, that nothing is left — which is what makes that
       assert a statement about the claimants rather than a wipe that covers for one. */
    concolic_undeclare_sources(WM_COMPONENT);
    JS_FreeValueRT(rt, g_deliver_fn);
    g_deliver_fn = JS_UNDEFINED;
    g_deliver_stepid = -1;
    g_id_post = -1;   /* the latch the init above consults — see core/agent_state.h */
}
