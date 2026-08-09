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
 * instance has to travel: the message, its origin, and — this is the part that is not a browser problem — the
 * SENDING FLOW'S WORLD, because two arms of a fork post two different messages and the receiver must not merge
 * them into one timeline. window_proxy_window crashes on such a proxy naming what to build. */
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

/* THIS DOCUMENT'S WindowProxy is §7.2.5.1's ONE-per-navigable, and it is READ from where that rule lives
   rather than cached here: a same-origin child is a second realm in this agent, so a file-scope copy would be
   whichever document installed last and `e.source` from one document would have compared equal to the other's.
   The document handle is the identity, and window_proxy.c owns the mapping. */
static JSValueConst win_proxy(JSContext *ctx) { return document_window_proxy(ctx); }
static char   *g_origin;          /* this document's origin, serialized (owned) */
static JSValue g_deliver_fn = JS_UNDEFINED;

/* One queued post: the message, the origin check it still owes, and the target. Held as a JS Array for the
   reason a port's queue is one — it is frontier data, it has to park, and the collector should own it. */
enum { PQ_DATA = 0, PQ_HOLDERS, PQ_TARGET_ORIGIN, PQ_N };

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
    JSValue buf, want, data, ev, ports = JS_UNDEFINED;
    const char *want_s = NULL;
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

    buf = JS_GetPropertyUint32(ctx, entry, PQ_DATA);
    swt.holders = JS_GetPropertyUint32(ctx, entry, PQ_HOLDERS);
    swt.data.buf = JS_GetArrayBuffer(ctx, &blen, buf);
    swt.data.len = blen;
    DCHECK(swt.data.buf != NULL, "a queued window message held something that is not the bytes it stored");
    data = structured_deserialize_transfer(ctx, &swt, &ports);
    JS_FreeValue(ctx, buf);
    JS_FreeValue(ctx, swt.holders);

    if (JS_IsException(data)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, ports);
        ev = message_event_new(ctx, "messageerror", JS_UNDEFINED, g_origin, win_proxy(ctx), JS_UNDEFINED);
    } else {
        /* §9.4.4 steps 7.2 and 7.3: the origin is the SENDER'S and the source is the sender's WindowProxy —
           which is what a handler's `event.origin` check is about, and the whole reason this event is worth
           anything to the solver. */
        ev = message_event_new(ctx, "message", data, g_origin, win_proxy(ctx), ports);
        JS_FreeValue(ctx, data);
        JS_FreeValue(ctx, ports);
    }
    if (JS_IsException(ev)) return JS_EXCEPTION;
    /* Fired at the WINDOW, which is the target proxy's active Window — the same read a flow that navigated the
       navigable would answer differently, and the reason the proxy is the thing held rather than the Window. */
    {
        JSValue w = window_proxy_window(ctx, target);
        event_target_fire(ctx, w, ev);
        JS_FreeValue(ctx, w);
    }
    return JS_UNDEFINED;
}

/* §9.4.4's `postMessage(message, options)` and the legacy `postMessage(message, targetOrigin, transfer)`. */
static JSValue js_window_post(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst second = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue list = JS_UNDEFINED, want = JS_NULL, entry, buf;
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
        list = argc > 2 ? JS_DupValue(ctx, argv[2]) : JS_UNDEFINED;
    } else if (JS_IsObject(second)) {
        JSValue t = JS_GetPropertyStr(ctx, second, "targetOrigin");
        if (JS_IsException(t)) return JS_EXCEPTION;
        if (!JS_IsUndefined(t)) {
            to = JS_ToCString(ctx, t);
            if (!to) { JS_FreeValue(ctx, t); return JS_EXCEPTION; }
        }
        JS_FreeValue(ctx, t);
        list = JS_GetPropertyStr(ctx, second, "transfer");
        if (JS_IsException(list)) { if (to) JS_FreeCString(ctx, to); return JS_EXCEPTION; }
    }

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
                JS_FreeValue(ctx, list);
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
    if (structured_serialize_transfer(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, list, &swt) < 0) {
        JS_FreeValue(ctx, list);
        JS_FreeValue(ctx, want);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, list);

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
    structured_with_transfer_free(ctx, &swt);

    /* §9.4.4 step 7: a TASK on the posted-message task source. Not a microtask — a page that posts and then
       awaits observes its own continuation first, which is the ordering the event loop's two halves exist for. */
    {
        JSValueConst args[2];
        /* THE RESOLVED TARGET, not this window. A task in THIS instance can only deliver to a navigable this
           instance holds; a proxy naming another document needs the message ROUTED there, which is an
           EMISSION — the bytes are already serialized and immutable, so nothing has to un-send it when this
           flow parks or is outranked, and it needs no COW capture. What it still needs is the host: only the
           trusted zone knows which instance holds that document, and only it may stamp the sender's origin
           (a forgeable event.origin would defeat every origin check in every bundle this solver reads). */
        DCHECK(!window_proxy_is_remote(target),
               "postMessage targets a navigable in ANOTHER WASM instance — build the outbound route: the "
               "serialized bytes, the sender origin stamped by the trusted zone, and the SENDING FLOW'S WORLD "
               "go to the host, which seeds a delivery flow in the peer whose world is receiver-baseline and "
               "that vector, because two arms of a fork post two messages from two contradictory worlds");
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
    idl_install_method(ctx, proto, "postMessage", 1, g_id_post);
    idl_optional_from(1);
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
