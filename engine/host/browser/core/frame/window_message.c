/* window.postMessage — HTML §9.4.4's WINDOW POST MESSAGE STEPS.
 *
 * WHAT IT IS FOR HERE. Two things, and the second is the one that matters to this project.
 *
 * The first is that it is ordinary: a great deal of real code posts to its OWN window to defer work past the
 * current task, and a bundle that does it and never receives its message has stopped running. That case is
 * complete below.
 *
 * The second is that `message` is one of the platform's richest ATTACKER SOURCES. A handler reads
 * `event.data` and reaches a sink with it, and whether that is exploitable turns on `event.origin` — which
 * §attacker-sources describes exactly: an origin the attacker controls by registering a domain, so a forgeable
 * check (endsWith, includes, startsWith) is SOLVED while an unforgeable one (=== against a fixed origin) is
 * unsatisfiable cross-origin and suppresses the finding. None of that can be reached until the event exists
 * with a real origin on it, which is what this file puts there.
 *
 * THE TARGET IS A NAVIGABLE'S ACTIVE WINDOW, WHICH IS WHY IT TAKES A WindowProxy. §9.4.4's first step is "let
 * targetWindow be this's browsing context's active Window", and with a cross-document target that Window is in
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
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/structured_clone.h"
#include "core/events/event_target.h"
#include "core/events/message_event.h"
#include "core/frame/window_message.h"
#include "core/frame/window_proxy.h"
#include "core/dom/document.h"
#include "core/url/url.h"
#include "solver/engine.h"
#include "solver/flow.h"
#include "solver/world.h"

/* THIS DOCUMENT'S WindowProxy is §7.2.5.1's ONE-per-navigable, and it is READ from where that rule lives
   rather than cached here: a same-origin child is a second realm in this agent, so a file-scope copy would be
   whichever document installed last and `e.source` from one document would have compared equal to the other's.
   The document handle is the identity, and window_proxy.c owns the mapping. */
static JSValueConst win_proxy(JSContext *ctx) { return document_window_proxy(ctx); }
static char   *g_origin;          /* this document's origin, serialized (owned) */
static JSValue g_deliver_fn = JS_UNDEFINED;

/* One queued post: the message, the origin check it still owes, and the target. Held as a JS Array for the
   reason a port's queue is one — it is frontier data, it has to park, and the collector should own it. */
/* PQ_SOURCE IS CAPTURED AT THE POST, not read at the delivery. §9.4.4 step 7.3's `source` is the INCUMBENT
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

/* §9.4.4 step 7.1: the origin check happens IN THE TASK, not at the call — the target may have navigated
   between the post and the delivery, and the standard checks the origin it has THEN. `want` is the serialized
   target origin, or NULL for "*". */
static bool origin_matches(const char *want, const char *have)
{
    if (!want) return true;                       /* "*" — any origin */
    if (!have) return false;
    /* §7.5's "same origin": for a tuple origin the serializations are equal, and an OPAQUE origin ("null") is
       same-origin with nothing, not even itself — which is why this is not a plain string compare. */
    if (!strcmp(want, "null") || !strcmp(have, "null")) return false;
    return strcmp(want, have) == 0;
}

static JSValue js_window_deliver(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValueConst target = argc > 0 ? argv[0] : JS_UNDEFINED;   /* the target WindowProxy */
    JSValueConst entry  = argc > 1 ? argv[1] : JS_UNDEFINED;
    StructuredWithTransfer swt;
    JSValue buf, want, data, ev, source, sender_origin, ports = JS_UNDEFINED;
    JSContext *tctx;
    const char *want_s = NULL, *from = NULL;
    size_t blen = 0;

    (void)this_val;
    want = JS_GetPropertyUint32(ctx, entry, PQ_TARGET_ORIGIN);
    if (!JS_IsNull(want)) {
        want_s = JS_ToCString(ctx, want);
        if (!want_s) { JS_FreeValue(ctx, want); return JS_EXCEPTION; }
    }
    /* The origin the target has NOW — read through the proxy, so a flow that navigated it sees its own. */
    if (!origin_matches(want_s, window_proxy_origin(target))) {
        if (want_s) JS_FreeCString(ctx, want_s);
        JS_FreeValue(ctx, want);
        return JS_UNDEFINED;   /* §9.4.4 step 7.1.3: not same origin, so nothing is delivered */
    }
    if (want_s) JS_FreeCString(ctx, want_s);
    JS_FreeValue(ctx, want);

    /* §9.4.4 step 7: THE DESERIALIZE AND THE EVENT BELONG TO THE TARGET'S REALM. The standard says "in
       targetWindow's relevant realm" for both, and it is not pedantry here: the deserialized objects and the
       MessageEvent are handed to the RECEIVING document's code, so building them under the delivery callee's
       own realm gave a child document a message whose every object belonged to the parent. */
    tctx = window_proxy_realm(ctx, target);
    DCHECK(tctx != NULL, "a window message was delivered to a navigable this agent holds no realm for");
    source = JS_GetPropertyUint32(ctx, entry, PQ_SOURCE);   /* the sender's proxy, taken at the post */
    sender_origin = JS_GetPropertyUint32(ctx, entry, PQ_SENDER_ORIGIN);
    from = JS_ToCString(ctx, sender_origin);
    DCHECK(from != NULL, "a queued window message carried no sender origin — §9.4.4's `origin` is the one field "
                         "a page's check is written against");
    buf = JS_GetPropertyUint32(ctx, entry, PQ_DATA);
    swt.holders = JS_GetPropertyUint32(ctx, entry, PQ_HOLDERS);
    swt.data.buf = JS_GetArrayBuffer(ctx, &blen, buf);
    swt.data.len = blen;
    DCHECK(swt.data.buf != NULL, "a queued window message held something that is not the bytes it stored");
    data = structured_deserialize_transfer(tctx, &swt, &ports);
    JS_FreeValue(ctx, buf);
    JS_FreeValue(ctx, swt.holders);

    if (JS_IsException(data)) {
        JS_FreeValue(tctx, JS_GetException(tctx));
        JS_FreeValue(tctx, ports);
        ev = message_event_new(tctx, "messageerror", JS_UNDEFINED, from, source, JS_UNDEFINED);
    } else {
        /* §9.4.4 steps 7.2 and 7.3: the origin is the SENDER'S and the source is the sender's WindowProxy —
           which is what a handler's `event.origin` check is about, and the whole reason this event is worth
           anything to the solver. `ports` is `newPorts`: the MessagePorts among the [[TransferredValues]], so a
           transferred ArrayBuffer arrives inside `data` and is not one of them. */
        JSValue new_ports = message_event_ports_of(tctx, ports);
        JS_FreeValue(tctx, ports);
        if (JS_IsException(new_ports)) {
            JS_FreeValue(tctx, data);
            JS_FreeCString(ctx, from);
            JS_FreeValue(ctx, sender_origin);
            JS_FreeValue(ctx, source);
            return JS_EXCEPTION;
        }
        ev = message_event_new(tctx, "message", data, from, source, new_ports);
        JS_FreeValue(tctx, data);
        JS_FreeValue(tctx, new_ports);
    }
    JS_FreeCString(ctx, from);
    JS_FreeValue(ctx, sender_origin);
    JS_FreeValue(ctx, source);
    if (JS_IsException(ev)) return JS_EXCEPTION;
    /* Fired at the WINDOW, which is the target proxy's active Window — the same read a flow that navigated the
       navigable would answer differently, and the reason the proxy is the thing held rather than the Window. */
    {
        JSValue w = window_proxy_window(tctx, target);
        event_target_fire(tctx, w, ev, JS_UNDEFINED);
        JS_FreeValue(tctx, w);
    }
    return JS_UNDEFINED;
}

/* §9.4.4 STEP 7 ACROSS INSTANCES — an EMISSION, and every part of that word is load-bearing.
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

    /* §9.4.4 step 7.1's origin check is the RECEIVER'S to make — it tests the target's origin at DELIVERY, and
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

/* THE INBOUND HALF: a message the trusted zone ROUTED here from another instance. It is the same §9.4.4 step 7
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
    /* §7.2.5.1's proxy for the SENDER, minted remote because that document lives in the instance that sent
       this. Its origin is the stamped one — the same value `event.origin` reports, and the value a
       same-origin check inside the peer would be made against. */
    source = window_proxy_new_remote(ctx, world_doc_intern(sender_doc), sender_origin, NULL,
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
    JS_SetPropertyUint32(ctx, entry, PQ_SENDER_ORIGIN, JS_NewString(ctx, sender_origin));
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

/* §9.4.4's `postMessage(message, options)` and the legacy `postMessage(message, targetOrigin, transfer)`. */
static JSValue js_window_post(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst second = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue raw = JS_UNDEFINED, transfer, want = JS_NULL, entry, buf;
    StructuredWithTransfer swt;
    const char *to = NULL;
    /* §9.4.4 POSTS TO THE WINDOW IT WAS CALLED ON, and the receiver is how that is known. This discarded
       `this_val` and always delivered to THIS window, so `iframe.contentWindow.postMessage(m, "*")` — which is
       most of the real uses of this method — fired the message at the sender. Nothing could be routed by a
       target that was never read. `window.postMessage(...)` calls it on the global, which is this navigable's
       own proxy; a call on a proxy targets that proxy. */
    JSValueConst target = window_proxy_is(this_val) ? this_val : win_proxy(ctx);

    (void)magic;
    DCHECK(!JS_IsUndefined(target), "postMessage ran before this window's WindowProxy existed");
    /* THE TWO OVERLOADS. A STRING second argument is the legacy targetOrigin form, with the transfer list
       third; anything else is the options dictionary. A page still writing `postMessage(m, '*')` is most of
       the code that uses this at all, so it is not a legacy path in any practical sense. */
    if (JS_IsString(second)) {
        to = JS_ToCString(ctx, second);
        if (!to) return JS_EXCEPTION;
        raw = argc > 2 ? JS_DupValue(ctx, argv[2]) : JS_UNDEFINED;
    } else if (JS_IsObject(second)) {
        JSValue t = JS_GetPropertyStr(ctx, second, "targetOrigin");
        if (JS_IsException(t)) return JS_EXCEPTION;
        if (!JS_IsUndefined(t)) {
            to = JS_ToCString(ctx, t);
            if (!to) { JS_FreeValue(ctx, t); return JS_EXCEPTION; }
        }
        JS_FreeValue(ctx, t);
        raw = JS_GetPropertyStr(ctx, second, "transfer");
        if (JS_IsException(raw)) { if (to) JS_FreeCString(ctx, to); return JS_EXCEPTION; }
    }
    /* §3.2.21's `sequence<object>`, MATERIALIZED before anything else reads it — the one place the page's code
       runs, and everything downstream walks the engine's own array. See structured_clone.h. */
    if (structured_transfer_list(ctx, raw, &transfer) < 0) {
        JS_FreeValue(ctx, raw);
        if (to) JS_FreeCString(ctx, to);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, raw);

    /* §9.4.4 step 4. "*" is any origin; "/" is the SENDER's own; anything else is a URL whose origin is taken,
       and a URL that does not parse is a SyntaxError — which is thrown HERE, at the call, because it is the
       page's mistake and not the delivery's. */
    if (to && strcmp(to, "*") != 0) {
        if (!strcmp(to, "/")) {
            want = JS_NewString(ctx, g_origin ? g_origin : "null");
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
    if (to) JS_FreeCString(ctx, to);

    /* §9.4.4 step 6: serialize and transfer NOW — the DataCloneError belongs to this call. */
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
    /* §9.4.4 step 7.3's source, read HERE where the caller's realm is the one running. */
    JS_SetPropertyUint32(ctx, entry, PQ_SOURCE, JS_DupValue(ctx, win_proxy(ctx)));
    JS_SetPropertyUint32(ctx, entry, PQ_SENDER_ORIGIN, JS_NewString(ctx, g_origin ? g_origin : "null"));
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
    /* §9.4.4 step 7: a TASK on the posted-message task source. Not a microtask — a page that posts and then
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

/* §9.4.4's `postMessage` ON THIS REALM'S WindowProxy PROTOTYPE. It is declared into core/realm.h's list AFTER
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

/* §9.4.4's `postMessage`, DECLARED ONCE PER AGENT — a member has one pool entry, and every realm's prototype
   carries that same one. */
void window_message_init(JSContext *ctx)
{
    static const IdlArgType POST_ARGS[3] = { IDL_ANY, IDL_ANY, IDL_ANY };

    /* THE DELIVERY TASK'S CALLEE IS THE AGENT'S — one function object for the whole runtime. It was minted in
       the per-DOCUMENT install, so a second same-origin realm overwrote it and the first realm's copy became
       unreachable with nothing to free it: JS_FreeRuntime's gc_obj_list walk counted it, which is the leak
       gate doing exactly its job. */
    g_deliver_fn = JS_NewCFunction(ctx, js_window_deliver, "", 2);
    CHECK(JS_IsFunction(ctx, g_deliver_fn), "the window delivery task's callee could not be allocated");
    g_id_post = idl_method_id(ctx, POST_ARGS, 3, js_window_post, 0);
    idl_optional_from(1);
    realm_declare_intrinsic(window_message_install_proto);
}

void window_message_install(JSContext *ctx, JSValueConst global, const char *origin)
{

    /* THE ORIGIN IS THE AGENT'S — an agent is origin-keyed, so a second document installing here is ordinary
       and a second ORIGIN would be two principals behind one instance, which SECURITY.md forbids. */
    DCHECK(g_origin == NULL || !strcmp(g_origin, origin ? origin : "null"),
           "a second document was installed into this agent with a DIFFERENT origin — an agent is origin-keyed, "
           "so a cross-origin document is a second INSTANCE and never a second realm in this one");
    if (!g_origin) {
        g_origin = strdup(origin ? origin : "null");
        CHECK(g_origin != NULL, "window message: OOM recording this agent's origin");
    }
    /* ONE DECLARATION, TWO PLACES IT IS REACHED — the global (`window.postMessage`) and every WindowProxy
       (`otherWindow.postMessage`). Declaring it twice would give the two spellings two members that can drift,
       and `window.postMessage === self.postMessage` would stop holding. The WindowProxy prototype is the
       AGENT's, so it takes the member once; the global is per realm. */
    idl_install_method(ctx, global, "postMessage", 1, g_id_post);
}


void window_message_free(JSContext *ctx)
{

    JS_FreeValue(ctx, g_deliver_fn);
    g_deliver_fn = JS_UNDEFINED;
    free(g_origin);
    g_origin = NULL;
}
