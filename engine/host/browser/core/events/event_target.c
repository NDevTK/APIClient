/* EVENTTARGET — DOM §2.7, over the objects this engine already hands the page.
 *
 * WPT NAMED THIS ONE. Every DOM test loads testharness.js, which declares its tests and then waits for the LOAD
 * EVENT before reporting — so with no event loop, 175 of 175 dom/nodes files ran the harness perfectly and
 * produced nothing at all. It is also how ordinary pages are written: the half of a bundle that runs on
 * DOMContentLoaded is the half that touches the DOM and calls the API.
 *
 * WHERE THE LISTENERS LIVE. On the TARGET, as an ordinary own property under a private key the page cannot
 * reach. That is not a shortcut — it is what makes registration per-flow for free: a listener added in one arm
 * of a fork is a property write like any other, so the COW delta captures it and the sibling never sees it.
 * A side table keyed by object pointer would have needed its own delta kind and its own swap, for the same
 * result.
 *
 * HOW A LISTENER RUNS. Not by JS_Call from C — a listener body is the page's code and holds loops, awaits and
 * concolic branches, so calling it from a C activation is the drive-to-completion the engine aborts on. Each
 * listener is dispatched through the promise machinery, which is how every other page callback in this engine
 * reaches a flow: the reaction runs as a call-root flow, preemptible and forkable like anything else. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/events/event.h"
#include "core/dom/node.h"
#include <lexbor/dom/dom.h>

/* The two shapes every DOM member in this file has. Spelled once so a member declares its IDL, not a bitmask. */
static const IdlArgType IDL_1STR[1] = { IDL_DOMSTRING };
static const IdlArgType IDL_2STR[2] = { IDL_DOMSTRING, IDL_DOMSTRING };
#include "core/events/event_target.h"

/* The private key the listener map hangs off. A SYMBOL, so a page enumerating its own objects cannot see it and
   cannot collide with it — the same reason the platform uses internal slots. */
/* `g_ready` rather than testing g_key: a static JSValue is ZERO-initialised, and zero is not JS_UNDEFINED — the
   tag is part of the value. Asking JS_IsUndefined of it answers "no" before anything has run, which fired the
   ran-twice assert on the FIRST call. A JSValue's emptiness is not the allocator's default. */
static JSValue g_key;
static int g_ready;
/* The WINDOW — §7.6's document parent on a propagation path, and Web IDL's relevant global for a [Global]
   operation called with no receiver. Declared here because both of those are read before the machine below. */
static JSValue g_window;
/* HTML §8.1.7's handler map key, and the MARKER that holds the handler's place in a listener list — see the
   event-handler section below. Declared here because event_target_init mints them. */
static JSValue g_handler_key;
static JSValue g_handler_marker;
/* The internal DISPATCH_PAIR function object — the one door a C caller has into the §2.9 machine, and the
   window the propagation path ends at. Neither is installed anywhere the page can reach. */
static JSValue g_dispatch_fn;
/* The ids JS_RegisterStepDef handed this runtime for add/removeEventListener. `type` is a Web IDL DOMString,
   so it is ToString on whatever the page passed and cannot be a JS_ToCString from C. */
static int g_add_stepid = -1, g_remove_stepid = -1, g_dispatch_stepid = -1;
static JSValue idl_add_or_remove(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic);

void event_target_init(JSContext *ctx)
{
    DCHECK(!g_ready, "event_target_init ran twice — one instance is one document");
    g_key = JS_NewSymbol(ctx, "eventListeners", false);
    CHECK(!JS_IsException(g_key), "the event-listener key allocation failed");
    g_handler_key = JS_NewSymbol(ctx, "eventHandlers", false);
    g_handler_marker = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_handler_key) && !JS_IsException(g_handler_marker),
          "the event-handler key or marker allocation failed");
    g_ready = 1;
    if (g_add_stepid < 0) {
        g_add_stepid    = idl_method_id(ctx, IDL_1STR, 1, idl_add_or_remove, 0);   /* (DOMString type, EventListener?) */
        g_remove_stepid = idl_method_id(ctx, IDL_1STR, 1, idl_add_or_remove, 1);
    }

}

void event_target_free(JSContext *ctx)
{
    if (!g_ready)
        return;
    JS_FreeValue(ctx, g_key);
    JS_FreeValue(ctx, g_handler_key);
    JS_FreeValue(ctx, g_handler_marker);
    JS_FreeValue(ctx, g_dispatch_fn);
    JS_FreeValue(ctx, g_window);
    g_key = g_handler_key = g_handler_marker = g_dispatch_fn = g_window = JS_UNDEFINED;
    g_ready = 0;
}

/* WEB IDL §3.6's [Global] RULE: an operation on the Window interface called with an undefined `this` uses the
   RELEVANT GLOBAL OBJECT. That is not a nicety — `addEventListener('load', init)` written unqualified is how a
   great deal of real code registers, and a bare call has an undefined this-binding, so without this rule every
   one of those listeners was registered on nothing at all and silently never fired. It is applied at the shared
   entry because that is where the receiver arrives; a non-global interface reached with undefined would
   otherwise be an immediate TypeError, so there is nothing here for this to take away. */
static JSValueConst event_target_receiver(JSValueConst this_val)
{
    if (JS_IsObject(this_val)) return this_val;
    return g_window;
}

/* The map of type -> listener array on `target`, created on first use. NULL only on allocation failure. */
static JSValue listener_map(JSContext *ctx, JSValueConst target, int create)
{
    JSAtom k;
    JSValue map;

    DCHECK(g_ready, "an event-listener map was asked for before the key existed");
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL)
        return JS_UNDEFINED;
    /* AN OWN SLOT, never a property LOOKUP. `window` is the global object, and a miss on the global is the
       solver's absent-state seam: it mints a concolic for the name a page read and did not define. That is
       right for the page's own reads and wrong for an internal slot — the map came back as a concolic, every
       listener was stored on it, and window.addEventListener silently registered nothing. An internal slot is
       by definition an own slot, so it is read as one. */
    if (JS_GetOwnSlot(ctx, &map, target, k) <= 0)
        map = JS_UNDEFINED;
    if (!JS_IsObject(map) && create) {
        JS_FreeValue(ctx, map);
        map = JS_NewObject(ctx);
        if (!JS_IsException(map))
            JS_SetProperty(ctx, (JSValue)target, k, JS_DupValue(ctx, map));
    }
    JS_FreeAtom(ctx, k);
    return map;
}

/* The listener-list work, once `type` is a real string. Split from the coercion so the part that CAN reach the
   page's code is a request and the part that cannot is ordinary C. */
static JSValue add_listener_with_type(JSContext *ctx, JSValueConst this_val, JSValueConst cb, const char *type)
{
    JSValue map, arr;
    uint32_t len = 0;
    JSValueConst argv[2];

    argv[0] = JS_UNDEFINED; argv[1] = cb;
    map = listener_map(ctx, this_val, 1);
    if (!JS_IsObject(map)) { JS_FreeValue(ctx, map); return JS_UNDEFINED; }
    arr = JS_GetPropertyStr(ctx, map, type);
    if (!JS_IsArray(arr)) {
        JS_FreeValue(ctx, arr);
        arr = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, map, type, JS_DupValue(ctx, arr));
    }
    /* "If the event listener list already contains a listener with the same callback, do nothing." */
    JS_ToUint32(ctx, &len, JS_GetPropertyStr(ctx, arr, "length"));
    for (uint32_t i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, i);
        int same = JS_VALUE_GET_TAG(e) == JS_VALUE_GET_TAG(argv[1]) &&
                   JS_VALUE_GET_PTR(e) == JS_VALUE_GET_PTR(argv[1]);
        JS_FreeValue(ctx, e);
        if (same) { JS_FreeValue(ctx, arr); JS_FreeValue(ctx, map); return JS_UNDEFINED; }
    }
    JS_SetPropertyUint32(ctx, arr, len, JS_DupValue(ctx, argv[1]));
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, map);
   
    return JS_UNDEFINED;
}

static JSValue remove_listener_with_type(JSContext *ctx, JSValueConst this_val, JSValueConst cb, const char *type)
{
    JSValue map, arr, kept;
    uint32_t len = 0, k = 0;
    JSValueConst argv[2];

    argv[0] = JS_UNDEFINED; argv[1] = cb;
    map = listener_map(ctx, this_val, 0);
    if (!JS_IsObject(map)) { JS_FreeValue(ctx, map); return JS_UNDEFINED; }
    arr = JS_GetPropertyStr(ctx, map, type);
    if (!JS_IsArray(arr)) { JS_FreeValue(ctx, arr); JS_FreeValue(ctx, map); return JS_UNDEFINED; }
    JS_ToUint32(ctx, &len, JS_GetPropertyStr(ctx, arr, "length"));
    kept = JS_NewArray(ctx);
    for (uint32_t i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, i);
        int same = JS_VALUE_GET_TAG(e) == JS_VALUE_GET_TAG(argv[1]) &&
                   JS_VALUE_GET_PTR(e) == JS_VALUE_GET_PTR(argv[1]);
        if (same) JS_FreeValue(ctx, e);
        else JS_SetPropertyUint32(ctx, kept, k++, e);
    }
    JS_SetPropertyStr(ctx, map, type, kept);
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, map);
   
    return JS_UNDEFINED;
}

/* add/removeEventListener's `type` is a Web IDL DOMString, so it is ToString on whatever the page passed and
   cannot be a JS_ToCString from C. They use the SHARED coerce-then-call machine rather than one of their own:
   what they have in common with getAttribute and createElement is exactly the thing that needs a machine, and a
   second copy is a second chance to get the resumption wrong.
   IT ALSO FIXES AN ORDERING MISTAKE I MADE. A bespoke version checked 2.7's "if callback is null, return"
   BEFORE the coercion, to avoid running a toString for a call that does nothing. That is backwards: Web IDL
   converts arguments in ORDER at call time, so `type` is converted first and the null-callback step is part of
   the algorithm that runs after. `addEventListener({toString(){ … }}, null)` DOES run that toString in a real
   browser, and now here. */
static JSValue idl_add_or_remove(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    const char *type;
    JSValue r;

    if (argc < 2 || (magic == 0 && !JS_IsFunction(ctx, argv[1])))
        return JS_UNDEFINED;   /* 2.7: a non-callable listener is ignored, not an error */
    type = JS_ToCString(ctx, argv[0]);   /* a real string by now: this cannot reach the page */
    if (!type)
        return JS_EXCEPTION;
    r = (magic == 0) ? add_listener_with_type(ctx, event_target_receiver(this_val), argv[1], type)
                     : remove_listener_with_type(ctx, event_target_receiver(this_val), argv[1], type);
    JS_FreeCString(ctx, type);
    return r;
}

/* ---- EVENT HANDLER IDL ATTRIBUTES — HTML §8.1.7.2 --------------------------------------------------------
 *
 * `el.onclick = f` is not a listener registration a page could have written itself with addEventListener; it is
 * its own mechanism, and it was absent entirely. That absence is the single largest entry in this engine's IDL
 * gap report — about 150 of Window's 227 missing members are `on*` attributes, and the same list repeats on
 * Document and Element — and it is absent in the way that hurts most: `window.onload = init` is how a great
 * deal of real code starts, and it silently became an ordinary JS property that nothing ever read.
 *
 * ONE MECHANISM, NOT ONE PER NAME. An event handler attribute is entirely determined by its NAME: `onfoo` is
 * the handler for the event type `foo`. So the names are DATA — one X-macro list, from which the enum, the type
 * strings and the accessor table are all generated — and the behaviour is written once. Spelling the names out
 * rather than generating them from the IDL is the gap engine/idlgen.mjs exists to report; what must not happen
 * is an attribute that answers something its spec does not say.
 *
 * THE HANDLER IS NOT THE LISTENER, and that distinction is the whole design. §8.1.7 registers ONE listener the
 * first time a handler is set for a type, and later assignments change the HANDLER the listener reads — so the
 * listener keeps its position in the list. `el.onclick = a; el.addEventListener('click', b); el.onclick = c`
 * runs c then b, not b then c. Registering the handler function itself would append it and get that backwards.
 * So the listener list holds a MARKER for the handler slot, and the list snapshot resolves the marker to
 * whatever the handler is at dispatch time — a read of an engine-built map under a private Symbol, so it runs
 * none of the page's code and needs no request. Setting null removes the marker, which is §8.1.7's "deactivate".
 *
 * The handler map is an own property under a private Symbol, for the reason the listener map is: it makes the
 * handler per-flow for free, so `onclick` assigned in one arm of a fork is invisible to its sibling. */
#define EVENT_HANDLERS(X)                                                                                     \
    /* GlobalEventHandlers — HTML §8.1.7.2.1, on Window, Document and Element alike. */                       \
    X("onabort", EH_GLOBAL | EH_SIGNAL) X("onauxclick", EH_GLOBAL) X("onbeforeinput", EH_GLOBAL)                    \
    X("onbeforematch", EH_GLOBAL) X("onbeforetoggle", EH_GLOBAL) X("onblur", EH_GLOBAL) X("oncancel", EH_GLOBAL)      \
    X("oncanplay", EH_GLOBAL) X("oncanplaythrough", EH_GLOBAL) X("onchange", EH_GLOBAL) X("onclick", EH_GLOBAL)       \
    X("onclose", EH_GLOBAL) X("oncontextlost", EH_GLOBAL) X("oncontextmenu", EH_GLOBAL)                             \
    X("oncontextrestored", EH_GLOBAL) X("oncuechange", EH_GLOBAL) X("ondblclick", EH_GLOBAL) X("ondrag", EH_GLOBAL)   \
    X("ondragend", EH_GLOBAL) X("ondragenter", EH_GLOBAL) X("ondragleave", EH_GLOBAL) X("ondragover", EH_GLOBAL)      \
    X("ondragstart", EH_GLOBAL) X("ondrop", EH_GLOBAL) X("ondurationchange", EH_GLOBAL) X("onemptied", EH_GLOBAL)     \
    X("onended", EH_GLOBAL) X("onerror", EH_GLOBAL) X("onfocus", EH_GLOBAL) X("onformdata", EH_GLOBAL)                \
    X("oninput", EH_GLOBAL) X("oninvalid", EH_GLOBAL) X("onkeydown", EH_GLOBAL) X("onkeypress", EH_GLOBAL)            \
    X("onkeyup", EH_GLOBAL) X("onload", EH_GLOBAL) X("onloadeddata", EH_GLOBAL) X("onloadedmetadata", EH_GLOBAL)      \
    X("onloadstart", EH_GLOBAL) X("onmousedown", EH_GLOBAL) X("onmouseenter", EH_GLOBAL) X("onmouseleave", EH_GLOBAL) \
    X("onmousemove", EH_GLOBAL) X("onmouseout", EH_GLOBAL) X("onmouseover", EH_GLOBAL) X("onmouseup", EH_GLOBAL)      \
    X("onpause", EH_GLOBAL) X("onplay", EH_GLOBAL) X("onplaying", EH_GLOBAL) X("onprogress", EH_GLOBAL)               \
    X("onratechange", EH_GLOBAL) X("onreset", EH_GLOBAL) X("onresize", EH_GLOBAL) X("onscroll", EH_GLOBAL)            \
    X("onscrollend", EH_GLOBAL) X("onsecuritypolicyviolation", EH_GLOBAL) X("onseeked", EH_GLOBAL)                  \
    X("onseeking", EH_GLOBAL) X("onselect", EH_GLOBAL) X("onslotchange", EH_GLOBAL) X("onstalled", EH_GLOBAL)         \
    X("onsubmit", EH_GLOBAL) X("onsuspend", EH_GLOBAL) X("ontimeupdate", EH_GLOBAL) X("ontoggle", EH_GLOBAL)          \
    X("onvolumechange", EH_GLOBAL) X("onwaiting", EH_GLOBAL) X("onwheel", EH_GLOBAL)                                \
    /* DocumentAndElementEventHandlers — §8.1.7.2.3, on Document and Element (and Window, which mixes it in). */\
    X("oncopy", EH_GLOBAL) X("oncut", EH_GLOBAL) X("onpaste", EH_GLOBAL)                                            \
    /* WindowEventHandlers — §8.1.7.2.2, on Window (and, per the mixin, Document's body-delegated set). */     \
    X("onafterprint", EH_WINDOW) X("onbeforeprint", EH_WINDOW) X("onbeforeunload", EH_WINDOW)                       \
    X("onhashchange", EH_WINDOW) X("onlanguagechange", EH_WINDOW) X("onmessage", EH_WINDOW)                          \
    X("onmessageerror", EH_WINDOW) X("onoffline", EH_WINDOW) X("ononline", EH_WINDOW) X("onpagehide", EH_WINDOW)      \
    X("onpagereveal", EH_WINDOW) X("onpageshow", EH_WINDOW) X("onpageswap", EH_WINDOW) X("onpopstate", EH_WINDOW)     \
    X("onrejectionhandled", EH_WINDOW) X("onstorage", EH_WINDOW) X("onunhandledrejection", EH_WINDOW)               \
    X("onunload", EH_WINDOW)                                                                                    \
    /* Document's own — §3.1.1 and the Page Visibility API. */                                                 \
    X("onreadystatechange", EH_DOCUMENT) X("onvisibilitychange", EH_DOCUMENT)

/* The NAMES are string literals, not stringified identifiers, so the IDL gap auditor — which scans a component
   for the property names it installs — can SEE them. Behind a `#n` it saw none of these and reported all ninety
   as absent, which is the audit lying by omission: the same failure as leaving an interface out of its map. */
static const char *const EH_NAME[] = {
#define X(n, m) n,
    EVENT_HANDLERS(X)
#undef X
};
#define EH_COUNT ((int)(sizeof(EH_NAME) / sizeof(EH_NAME[0])))

/* The EVENT TYPE each attribute handles: its own name past the `on`, which is what §8.1.7 says and why one
   list produces both. */
static const char *const EH_TYPE[] = {
#define X(n, m) n + 2,
    EVENT_HANDLERS(X)
#undef X
};

static const int EH_MASK[] = {
#define X(n, m) (m),
    EVENT_HANDLERS(X)
#undef X
};


/* The handler map (type -> handler) and the marker that stands for it in a listener list. The map is per
   TARGET; the marker is ONE object for the whole runtime, because it carries no information — its identity is
   the whole of what it means. */
static JSValue handler_map(JSContext *ctx, JSValueConst target, int create)
{
    JSAtom k;
    JSValue map;

    DCHECK(g_ready, "an event-handler map was asked for before the key existed");
    k = JS_ValueToAtom(ctx, g_handler_key);
    if (k == JS_ATOM_NULL)
        return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &map, target, k) <= 0)   /* an own SLOT, never a lookup — see listener_map */
        map = JS_UNDEFINED;
    if (!JS_IsObject(map) && create) {
        JS_FreeValue(ctx, map);
        map = JS_NewObject(ctx);
        if (!JS_IsException(map))
            JS_SetProperty(ctx, (JSValue)target, k, JS_DupValue(ctx, map));
    }
    JS_FreeAtom(ctx, k);
    return map;
}

/* The handler currently set for `type` on `target`, or JS_NULL. A map read, so no page code and no request —
   which is what lets the dispatch walk resolve the marker in place. */
static JSValue handler_current(JSContext *ctx, JSValueConst target, const char *type)
{
    JSValue map = handler_map(ctx, target, 0), h;

    if (!JS_IsObject(map)) { JS_FreeValue(ctx, map); return JS_NULL; }
    h = JS_GetPropertyStr(ctx, map, type);
    JS_FreeValue(ctx, map);
    return JS_IsFunction(ctx, h) ? h : (JS_FreeValue(ctx, h), JS_NULL);
}

static JSValue js_handler_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    DCHECK(magic >= 0 && magic < EH_COUNT, "an event handler was declared with a magic the list does not name");
    return handler_current(ctx, this_val, EH_TYPE[magic]);
}

static JSValue js_handler_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    const char *type;
    JSValue map;

    DCHECK(magic >= 0 && magic < EH_COUNT, "an event handler was declared with a magic the list does not name");
    type = EH_TYPE[magic];
    map = handler_map(ctx, this_val, 1);
    if (!JS_IsObject(map)) { JS_FreeValue(ctx, map); return JS_UNDEFINED; }
    /* §8.1.7.1: anything that is not callable sets the handler to null. A page assigning a string here is
       writing legacy markup-style code, which HTML compiles — this engine does not, and an uncompiled string
       is honestly not a handler rather than one that silently never fires. */
    if (JS_IsFunction(ctx, val)) {
        JS_SetPropertyStr(ctx, map, type, JS_DupValue(ctx, val));
        /* §8.1.7: the listener is registered ONCE, the first time a handler is set for this type. */
        add_listener_with_type(ctx, this_val, g_handler_marker, type);
    } else {
        JS_SetPropertyStr(ctx, map, type, JS_NULL);
        remove_listener_with_type(ctx, this_val, g_handler_marker, type);   /* §8.1.7 "deactivate" */
    }
    JS_FreeValue(ctx, map);
    return JS_UNDEFINED;
}

void event_target_install_handlers(JSContext *ctx, JSValueConst target, int mask)
{
    int i;

    DCHECK(JS_IsObject(target), "event handlers were installed on something that is not an object");
    for (i = 0; i < EH_COUNT; i++) {
        JSAtom a;
        if (!(EH_MASK[i] & mask))
            continue;
        a = JS_NewAtom(ctx, EH_NAME[i]);
        CHECK(a != JS_ATOM_NULL, "an event handler name could not be interned");
        /* The getter/setter cprotos take their own signatures, which the magic-function constructor reaches
           through one pointer type — the same cast every JS_CGETSET_MAGIC_DEF performs at compile time. */
        JS_DefinePropertyGetSet(ctx, (JSValue)target, a,
                                JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_handler_get, EH_NAME[i], 0,
                                                     JS_CFUNC_getter_magic, i),
                                JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_handler_set, EH_NAME[i], 1,
                                                     JS_CFUNC_setter_magic, i),
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, a);
    }
}

/* §2.9 step 5: dispatch runs over a COPY of the listener list. That matters more here than in a browser — the
   walk suspends across every listener, so one that adds or removes a listener has arbitrarily long to do it.
   ONE list walk, so the engine's own firing and the page's dispatchEvent can never disagree about which
   listeners a target has; what differs between them is only how each listener is DELIVERED. */
static JSValue listener_snapshot(JSContext *ctx, JSValueConst target, const char *type)
{
    JSValue map, arr, copy = JS_NewArray(ctx);
    uint32_t len = 0, k = 0;

    map = listener_map(ctx, target, 0);
    if (!JS_IsObject(map)) { JS_FreeValue(ctx, map); return copy; }
    arr = JS_GetPropertyStr(ctx, map, type);
    if (JS_IsArray(arr)) {
        JS_ToUint32(ctx, &len, JS_GetPropertyStr(ctx, arr, "length"));
        for (uint32_t i = 0; i < len; i++) {
            JSValue fn = JS_GetPropertyUint32(ctx, arr, i);
            /* §8.1.7: the HANDLER SLOT resolves HERE, at dispatch time, to whatever `ontype` currently is —
               that is what keeps the slot's POSITION in the list while its handler changes underneath it. */
            if (JS_VALUE_GET_PTR(fn) == JS_VALUE_GET_PTR(g_handler_marker)) {
                JS_FreeValue(ctx, fn);
                fn = handler_current(ctx, target, type);
            }
            if (JS_IsFunction(ctx, fn)) JS_SetPropertyUint32(ctx, copy, k++, fn);
            else                        JS_FreeValue(ctx, fn);
        }
    }
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, map);
    return copy;
}


/* §2.9 DISPATCH, as a machine — and the reason dispatchEvent could not exist before.
 *
 * The spec makes dispatch SYNCHRONOUS and makes its return value depend on what the listeners did: it answers
 * `!canceled`, so `if (!el.dispatchEvent(ev)) { … }` is how a page asks whether anything called preventDefault.
 * Neither of the two obvious implementations can answer that. Calling the listeners from C is the
 * drive-to-completion this engine aborts on — a listener body holds loops, awaits and concolic branches.
 * Enqueueing them as jobs answers before any of them has run, so the answer would always be "not cancelled".
 *
 * So each listener is a CALL REQUEST: the machine parks on it, the listener runs as ordinary preemptible page
 * code at whatever depth it likes, and the machine resumes at the listener it was on with `i` as its cursor.
 * That is the same shape every continuation-holding builtin in the engine has, which is why this needs no
 * machinery of its own beyond the state.
 *
 * THE LIST IS SNAPSHOT FIRST, which the spec requires and which matters here more than in a browser: a listener
 * that runs mid-walk can add or remove listeners, and this walk is suspended across every one of them. */
enum { DISPATCH_ARG = 0, CLICK_SYNTH = 1, DISPATCH_PAIR = 2 };

typedef struct JSDispatchState {
    JSStepHdr hdr;       /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t   stage;
    uint8_t   cphase;    /* the call request's own phase, so a stage can hold a call across a suspension */
    uint32_t  i, n;      /* THE RESUME POINT: the listener being called, and how many there are */
    uint32_t  ti, tn;    /* and which TARGET on the propagation path its list belongs to */
    JSValue   path;      /* §2.9's propagation path — the target and its ancestors (owned) */
    JSValue   cur;       /* the target whose listeners are running (owned) */
    JSValue   arr;       /* that target's listener list SNAPSHOT (owned) */
    JSValue   ev;        /* the event (owned) */
    JSValue   result;    /* !canceled (owned) */
    JSValue   cb[3];     /* the call request buffer: [this, listener, event] */
} JSDispatchState;

/* WHAT THIS MACHINE OWNS. The call buffer is in here because a fork mid-listener must not hand two arms one
   invocation — that is exactly what the visit contract is for. */
static void js_dispatch_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSDispatchState *s = st;
    int k;
    v->val(ctx, &s->path);
    v->val(ctx, &s->cur);
    v->val(ctx, &s->arr);
    v->val(ctx, &s->ev);
    v->val(ctx, &s->result);
    for (k = 0; k < 3; k++)
        v->val(ctx, &s->cb[k]);
}

static JSValue js_dispatch_fini(JSContext *ctx, void *st, bool take_result)
{
    JSDispatchState *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    int k;

    if (take_result) s->result = JS_UNDEFINED;
    JS_FreeValue(ctx, s->result);
    JS_FreeValue(ctx, s->path);
    JS_FreeValue(ctx, s->cur);
    JS_FreeValue(ctx, s->arr);
    JS_FreeValue(ctx, s->ev);
    s->result = s->path = s->cur = s->arr = s->ev = JS_UNDEFINED;
    for (k = 0; k < 3; k++) {
        JS_FreeValue(ctx, s->cb[k]);
        s->cb[k] = JS_UNDEFINED;
    }
    return r;
}

/* §2.9 THE PROPAGATION PATH: the target, then its ancestors, then the window for a document. This engine has
   no capture phase and no shadow trees, so the path is exactly the bubble chain — and computing it is what let
   `bubble_to` go: event_target_fire used to pass window in by hand as a special case, which is the document's
   ancestor stated twice, once in the spec and once in an argument. */
static JSValue dispatch_path(JSContext *ctx, JSValueConst target)
{
    JSValue path = JS_NewArray(ctx);
    lxb_dom_node_t *n = node_of(target);
    uint32_t k = 0;

    JS_SetPropertyUint32(ctx, path, k++, JS_DupValue(ctx, target));
    if (n) {
        for (n = n->parent; n; n = n->parent)
            JS_SetPropertyUint32(ctx, path, k++, node_wrap(ctx, n));
        /* §7.6: the window is the document's parent for event purposes, which is how a `load` listener on
           window hears an event fired at the document. */
        if (JS_IsObject(g_window))
            JS_SetPropertyUint32(ctx, path, k++, JS_DupValue(ctx, g_window));
    }
    return path;
}

static int js_dispatch_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSDispatchState *s = st;
    JSValue fn, ignored;
    int r;

    if (s->stage == 0) {
        JSValueConst target;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->path = s->cur = s->arr = s->ev = s->result = JS_UNDEFINED;
        s->cb[0] = s->cb[1] = s->cb[2] = JS_UNDEFINED;
        /* THREE ENTRIES, ONE MACHINE, and `arg` is decided at REGISTRATION so no call site chooses:
             DISPATCH_ARG  — dispatchEvent: the receiver is the target, the page supplied the event.
             CLICK_SYNTH   — §3.2.2 click(): the receiver is the target and the event is BUILT, because click
                             is "fire a synthetic pointer event named click", which IS this dispatch.
             DISPATCH_PAIR — the ENGINE firing its own event, where there is no receiver to be the target
                             because the caller is C: both come in as arguments.
           The third is what let the second DELIVERY go. The engine used to enqueue each listener as its own
           job — a walk with no continuation, so it could not see stopImmediatePropagation, could not answer
           whether anything cancelled, and was a second implementation of §2.9 beside this one. */
        target = (s->hdr.arg == DISPATCH_PAIR) ? step_arg(&s->hdr, 0)
                                              : event_target_receiver(s->hdr.this_val);
        s->ev = (s->hdr.arg == CLICK_SYNTH)
                    ? event_new_untrusted(ctx, "click", /*bubbles*/ true, /*cancelable*/ true)
                    : JS_DupValue(ctx, step_arg(&s->hdr, s->hdr.arg == DISPATCH_PAIR ? 1 : 0));
        /* §2.9 step 1: the argument must BE an Event. The slot record is the brand — a page cannot forge the
           symbol it hangs off, so an object shaped like an event is still not one. */
        if (!event_is(ctx, s->ev)) {
            JS_ThrowTypeError(ctx, "dispatchEvent requires an Event");
            return JS_STEP_ABRUPT;
        }
        /* §2.9 step 2: an event already being dispatched cannot be dispatched again. */
        if (event_dispatch_flag(ctx, s->ev)) {
            JS_ThrowDOMException(ctx, "InvalidStateError", "the event is already being dispatched");
            return JS_STEP_ABRUPT;
        }
        event_set_dispatch_flag(ctx, s->ev, true);
        /* §2.9 step 3: an event the PAGE dispatches is untrusted, whatever it was when constructed. One the
           ENGINE fires keeps the flag it was built with, which is the whole difference between them. */
        if (s->hdr.arg != DISPATCH_PAIR)
            event_set_trusted(ctx, s->ev, false);
        s->path = dispatch_path(ctx, target);
        s->tn = 0;
        JS_ToUint32(ctx, &s->tn, JS_GetPropertyStr(ctx, s->path, "length"));
        s->ti = 0;
        s->n = s->i = 0;
        s->stage = 1;
    }

    for (;;) {
        while (s->i < s->n) {
            /* §2.9: stopImmediatePropagation ends the walk between listeners, which is the only place it can
               be observed — the flag was set by a listener that has already returned. */
            if (s->cphase == 0 && event_stop_immediate(ctx, s->ev))
                break;
            fn = JS_GetPropertyUint32(ctx, s->arr, s->i);
            if (!JS_IsFunction(ctx, fn)) {
                JS_FreeValue(ctx, fn);
                JS_FreeValue(ctx, cb_result);
                cb_result = JS_UNDEFINED;
                s->i++;
                continue;
            }
            /* §2.9 "inner invoke": the listener is called with `this` = currentTarget and the event as its one
               argument. A CALL REQUEST, so the listener is ordinary preemptible page code and this machine
               parks — which is the whole reason the engine's own firing can share this walk. */
            r = step_call_run(ctx, &s->cphase, s->cb, fn, s->cur, 1, (JSValueConst *)&s->ev,
                              cb_result, &ignored, out_cb, out_argc);
            JS_FreeValue(ctx, fn);
            cb_result = JS_UNDEFINED;
            if (r > 0) return r;         /* parked ON THIS LISTENER; the resume comes back to it */
            JS_FreeValue(ctx, ignored);  /* §2.9: a listener's return value is discarded */
            s->i++;
        }
        /* ON TO THE NEXT TARGET UP THE PATH — but only while the event bubbles and nothing stopped it. */
        if (s->ti >= s->tn) break;
        if (s->ti > 0 && (!event_bubbles(ctx, s->ev) || event_stop_propagation(ctx, s->ev))) break;
        JS_FreeValue(ctx, s->cur);
        JS_FreeValue(ctx, s->arr);
        s->cur = JS_GetPropertyUint32(ctx, s->path, s->ti);
        {
            JSValue first = JS_GetPropertyUint32(ctx, s->path, 0);
            JSValue tv = event_type(ctx, s->ev);
            const char *type = JS_IsString(tv) ? JS_ToCString(ctx, tv) : NULL;
            /* §2.9: `target` is where the event was dispatched and does not move; `currentTarget` is whose
               listeners are running now, and eventPhase says AT_TARGET only for the first. */
            event_set_targets(ctx, s->ev, first, s->cur);
            event_set_phase(ctx, s->ev, s->ti == 0 ? 2 : 3);   /* AT_TARGET, then BUBBLING_PHASE */
            s->arr = type ? listener_snapshot(ctx, s->cur, type) : JS_NewArray(ctx);
            if (type) JS_FreeCString(ctx, type);
            JS_FreeValue(ctx, tv);
            JS_FreeValue(ctx, first);
        }
        s->n = 0;
        JS_ToUint32(ctx, &s->n, JS_GetPropertyStr(ctx, s->arr, "length"));
        s->i = 0;
        s->ti++;
    }
    JS_FreeValue(ctx, cb_result);

    /* §2.9 "clean up": the dispatch flag clears, the phase goes back to NONE and currentTarget to null. */
    event_set_dispatch_flag(ctx, s->ev, false);
    event_clear_current(ctx, s->ev);
    s->result = JS_NewBool(ctx, !event_canceled(ctx, s->ev));
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_dispatch_def = {
    sizeof(JSDispatchState), js_dispatch_step, js_dispatch_fini, DISPATCH_ARG, .visit = js_dispatch_visit
};
/* §3.2.2 click(). The SAME machine — a click is a dispatch, and giving it its own would be two implementations
   of §2.9 that could disagree about listener order, the handler slot or the canceled flag. */
static const JSTrampStepDef js_click_def = {
    sizeof(JSDispatchState), js_dispatch_step, js_dispatch_fini, CLICK_SYNTH, .visit = js_dispatch_visit
};
static int g_click_stepid = -1;

void event_target_install_click(JSContext *ctx, JSValueConst target)
{
    DCHECK(JS_IsObject(target), "click was installed on something that is not an object");
    if (g_click_stepid < 0)
        g_click_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_click_def);
    idl_install_method(ctx, target, "click", 0, g_click_stepid);
}

/* THE ENGINE FIRING ITS OWN EVENT — `load`, `DOMContentLoaded`, `abort`. It builds the event and hands it to
   the SAME §2.9 machine, reached as a queued task because its callers are plain C the scheduler drives and
   cannot park. That is the whole fix: this used to walk the listener list ITSELF and enqueue each listener as
   its own job, which was a second implementation of §2.9 beside the machine — one that could not see
   stopImmediatePropagation (each listener was a separate job with no walk between them), could not bubble
   properly (the caller passed the window in by hand as `bubble_to`), and could not answer whether anything
   cancelled. There is one dispatch now; what differs between the two reach-paths is only whether the caller
   can park, which is a property of the CALLER and not of the algorithm.
   The event stays TRUSTED, which is what distinguishes one the engine fired from one the page dispatched. */
void event_target_fire(JSContext *ctx, JSValueConst target, JSValue ev)
{
    JSValueConst argv[2];

    DCHECK(JS_IsObject(g_dispatch_fn),
           "the engine fired an event before event_target_init built the dispatcher — there is one dispatch, "
           "and this is the only way a C caller reaches it");
    if (JS_IsException(ev)) { JS_FreeValue(ctx, ev); return; }
    argv[0] = target;
    argv[1] = ev;
    /* A JOB, so the dispatch runs as a call-root flow: preemptible, forkable and parkable like any other
       program, which is what every listener body needs and what a C activation cannot host. */
    JS_EnqueueCallJob(ctx, g_dispatch_fn, 2, argv);
    JS_FreeValue(ctx, ev);
}

/* THE SAME FIRE, SYNCHRONOUSLY — for a caller that CAN park. §2.9 dispatch is synchronous, and some of the
   engine's own fires are specified that way: §3.2's `abort` happens inside abort(), so a page that calls
   ac.abort() and then reads a flag its listener set must see it already set. A queued fire answers the
   question after the caller has returned.
   It is the SAME machine through the same internal door — only the reach differs, which is the whole point of
   there being one dispatch: a caller that can park calls it as a REQUEST, one that cannot enqueues it as a job.
   `phase` and `cb` are the caller machine's own; `cb` needs FOUR slots ([this, func, target, event]).
     0 = done (*pnot_canceled set when asked), 3 = the caller must return that step code. */
int event_target_fire_run(JSContext *ctx, uint8_t *phase, JSValue *cb, JSValueConst target,
                          JSValueConst ev, JSValue in,
                          bool *pnot_canceled, JSValue **out_cb, int *out_argc)
{
    JSValueConst argv[2];
    JSValue out = JS_UNDEFINED;
    int r;

    DCHECK(JS_IsObject(g_dispatch_fn),
           "an event was fired synchronously before event_target_init built the dispatcher");
    if (*phase == 0) {
        DCHECK(JS_IsObject(ev), "a synchronous fire was handed no event — §2.9 dispatches one that exists");
        argv[0] = target;
        argv[1] = ev;
        r = step_call_run(ctx, phase, cb, g_dispatch_fn, JS_UNDEFINED, 2, argv, in, &out, out_cb, out_argc);
        DCHECK(r == JS_STEP_CALL, "the dispatch request answered without parking");
        return r;
    }
    r = step_call_run(ctx, phase, cb, JS_UNDEFINED, JS_UNDEFINED, 2, NULL, in, &out, out_cb, out_argc);
    DCHECK(r == 0, "a synchronous fire resumed into something other than its answer");
    if (pnot_canceled) *pnot_canceled = JS_ToBool(ctx, out);
    JS_FreeValue(ctx, out);
    return 0;
}

/* The window, which §7.6 makes the document's parent for event purposes — how a `load` listener on window
   hears an event fired at the document. Registered rather than passed per fire, because it is a property of
   the browsing context and not of any one dispatch. */
void event_target_set_window(JSContext *ctx, JSValueConst global)
{
    JS_FreeValue(ctx, g_window);
    g_window = JS_DupValue(ctx, global);
}

static const JSTrampStepDef js_dispatch_pair_def = {
    sizeof(JSDispatchState), js_dispatch_step, js_dispatch_fini, DISPATCH_PAIR, .visit = js_dispatch_visit
};

void event_target_install(JSContext *ctx, JSValueConst target)
{
    DCHECK(JS_IsObject(target), "event_target_install was handed something that is not an object");
    /* Declared HERE rather than in event_target_init because the machine is defined below it — and one
       declaration serves every target, which is what the id being cached says. */
    if (g_dispatch_stepid < 0) {
        g_dispatch_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_dispatch_def);
        /* The C caller's door into the same machine: a step function object nobody installs, so the page can
           neither see it nor replace it — which matters, because a page that overwrote `dispatchEvent` must
           not redirect the engine's own `load`. */
        g_dispatch_fn = JS_NewCFunction2(ctx, NULL, "dispatch", 2, JS_CFUNC_step,
                                         JS_RegisterStepDef(JS_GetRuntime(ctx), &js_dispatch_pair_def));
        CHECK(!JS_IsException(g_dispatch_fn), "the internal event dispatcher could not be allocated");
    }
    idl_install_method(ctx, target, "addEventListener", 2, g_add_stepid);
    idl_install_method(ctx, target, "removeEventListener", 2, g_remove_stepid);
    idl_install_method(ctx, target, "dispatchEvent", 1, g_dispatch_stepid);
}
