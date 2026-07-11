/* LocalDOMWindow global-scope API install — see local_dom_window.h. */
#include "core/frame/local_dom_window.h"
#include "core/dom/custom_elements.h"   /* install_dom_interface_ctors */
#include "core/loader/xhr.h"            /* js_xhr_ctor */
#include "core/dom/domparser.h"         /* js_domparser_ctor */
#include "core/intersection_observer/intersection_observer.h"
#include "core/dom/mutation_observer.h"
#include "core/resize_observer/resize_observer.h"
#include "core/timing/performance_observer.h"
#include "modules/websocket.h"          /* js_ws_ctor (WebSocket/EventSource) */
#include "modules/worker.h"             /* js_worker_ctor (Worker/SharedWorker) */
#include "core/frame/messaging.h"       /* js_msg_channel_ctor / js_broadcast_ctor */
#include "modules/notification.h"       /* js_notification_ctor / js_notif_request_perm */
#include "core/dom/abort.h"             /* js_abortsignal_make */
#include "modules/indexeddb.h"          /* js_idb_open */
#include "core/css/cssom.h"             /* js_get_computed_style / js_match_media */
#include "core/frame/screen.h"          /* screen_install_viewport */
#include "modules/fsa.h"                /* js_fsa_open_picker / save / dir (File System Access) */
#include "core/html/media_element.h"    /* js_image_ctor / js_audio_ctor / js_option_ctor */
#include "core/dom/events/event.h"      /* js_event_ctor (Event / CustomEvent) */
#include "core/frame/intl.h"            /* js_intl_ctor */
#include "core/frame/history.h"         /* js_history_make */
#include "solver/concolic.h"              /* js_noop / js_concolic_stub */

void install_window_apis(JSContext *ctx, JSValue g, JSValueConst el_proto) {
    /* WEB COMPONENTS: constructable DOM bases so `class X extends HTMLElement {…}` DEFINES -> its lifecycle
       methods become uncalled methods the orphan driver reaches (learned by EXECUTION, not by reading an attr). */
    install_dom_interface_ctors(ctx, g, el_proto);   /* Element..SVGElement base ctors + customElements (custom_elements.c) */
    /* COMMON BROWSER APIs real bundles call constantly — a MISSING one threw and killed the script. */
    JS_SetPropertyStr(ctx, g, "XMLHttpRequest", JS_NewCFunction2(ctx, js_xhr_ctor, "XMLHttpRequest", 0, JS_CFUNC_constructor, 0));   /* primary request mechanism -> emits @H */
    JS_SetPropertyStr(ctx, g, "DOMParser", JS_NewCFunction2(ctx, js_domparser_ctor, "DOMParser", 0, JS_CFUNC_constructor, 0));   /* parseFromString -> {parsedhtml} taint -> appendChild @S */
    JS_SetPropertyStr(ctx, g, "IntersectionObserver", JS_NewCFunction2(ctx, js_intersection_observer_ctor, "IntersectionObserver", 1, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, g, "MutationObserver", JS_NewCFunction2(ctx, js_mutation_observer_ctor, "MutationObserver", 1, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, g, "ResizeObserver", JS_NewCFunction2(ctx, js_resize_observer_ctor, "ResizeObserver", 1, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, g, "PerformanceObserver", JS_NewCFunction2(ctx, js_performance_observer_ctor, "PerformanceObserver", 1, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, g, "WebSocket", JS_NewCFunction2(ctx, js_ws_ctor, "WebSocket", 1, JS_CFUNC_constructor, 0));         /* url endpoint emitted; send/close/addEventListener present */
    JS_SetPropertyStr(ctx, g, "EventSource", JS_NewCFunction2(ctx, js_ws_ctor, "EventSource", 1, JS_CFUNC_constructor, 0));     /* SSE: url is a GET endpoint; onmessage handler driven */
    JS_SetPropertyStr(ctx, g, "Worker", JS_NewCFunction2(ctx, js_worker_ctor, "Worker", 2, JS_CFUNC_constructor, 0));           /* worker script -> chunk (fetch+analyze); onmessage driven */
    JS_SetPropertyStr(ctx, g, "SharedWorker", JS_NewCFunction2(ctx, js_worker_ctor, "SharedWorker", 2, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, g, "MessageChannel", JS_NewCFunction2(ctx, js_msg_channel_ctor, "MessageChannel", 0, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, g, "BroadcastChannel", JS_NewCFunction2(ctx, js_broadcast_ctor, "BroadcastChannel", 1, JS_CFUNC_constructor, 0));
    {   /* Notification: constructor + static permission / requestPermission */
        JSValue nf = JS_NewCFunction2(ctx, js_notification_ctor, "Notification", 2, JS_CFUNC_constructor, 0);
        JS_SetPropertyStr(ctx, nf, "permission", JS_NewString(ctx, "default"));
        JS_SetPropertyStr(ctx, nf, "requestPermission", JS_NewCFunction(ctx, js_notif_request_perm, "requestPermission", 0));
        JS_SetPropertyStr(ctx, g, "Notification", nf);
    }
    {   /* AbortSignal: static timeout/any/abort -> a live signal (AbortController itself is a webctor) */
        JSValue as = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, as, "timeout", JS_NewCFunction(ctx, js_abortsignal_make, "timeout", 1));
        JS_SetPropertyStr(ctx, as, "any", JS_NewCFunction(ctx, js_abortsignal_make, "any", 1));
        JS_SetPropertyStr(ctx, as, "abort", JS_NewCFunction(ctx, js_abortsignal_make, "abort", 0));
        JS_SetPropertyStr(ctx, g, "AbortSignal", as);
    }
    {   /* indexedDB: non-throwing object graph; stored values opaque (state-gated code forks) */
        JSValue idb = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, idb, "open", JS_NewCFunction(ctx, js_idb_open, "open", 2));
        JS_SetPropertyStr(ctx, idb, "deleteDatabase", JS_NewCFunction(ctx, js_idb_open, "deleteDatabase", 1));
        JS_SetPropertyStr(ctx, idb, "databases", JS_NewCFunction(ctx, js_concolic_stub, "databases", 0));
        JS_SetPropertyStr(ctx, idb, "cmp", JS_NewCFunction(ctx, js_concolic_stub, "cmp", 2));
        JS_SetPropertyStr(ctx, g, "indexedDB", idb);
    }
    JS_SetPropertyStr(ctx, g, "getComputedStyle", JS_NewCFunction(ctx, js_get_computed_style, "getComputedStyle", 1));
    JS_SetPropertyStr(ctx, g, "matchMedia", JS_NewCFunction(ctx, js_match_media, "matchMedia", 1));
    screen_install_viewport(ctx, g);   /* window.screen + innerWidth/innerHeight/devicePixelRatio (concolic viewport, screen.c) */
    JS_SetPropertyStr(ctx, g, "showOpenFilePicker", JS_NewCFunction(ctx, js_fsa_open_picker, "showOpenFilePicker", 1));   /* File System Access: a picked file's content is an attacker @S source */
    JS_SetPropertyStr(ctx, g, "showSaveFilePicker", JS_NewCFunction(ctx, js_fsa_save_picker, "showSaveFilePicker", 1));
    JS_SetPropertyStr(ctx, g, "showDirectoryPicker", JS_NewCFunction(ctx, js_fsa_dir_picker, "showDirectoryPicker", 1));
    JS_SetPropertyStr(ctx, g, "Image", JS_NewCFunction2(ctx, js_image_ctor, "Image", 2, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, g, "Audio", JS_NewCFunction2(ctx, js_audio_ctor, "Audio", 1, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, g, "Option", JS_NewCFunction2(ctx, js_option_ctor, "Option", 4, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, g, "CustomEvent", JS_NewCFunction2(ctx, js_event_ctor, "CustomEvent", 2, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, g, "Event", JS_NewCFunction2(ctx, js_event_ctor, "Event", 1, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, g, "scrollTo", JS_NewCFunction(ctx, js_noop, "scrollTo", 2));
    JS_SetPropertyStr(ctx, g, "scrollBy", JS_NewCFunction(ctx, js_noop, "scrollBy", 2));
    JS_SetPropertyStr(ctx, g, "scroll", JS_NewCFunction(ctx, js_noop, "scroll", 2));
    {   /* Intl: locale formatters (constructors) — results opaque */
        JSValue intl = JS_NewObject(ctx);
        const char *cn[] = { "NumberFormat", "DateTimeFormat", "Collator", "RelativeTimeFormat", "ListFormat", "PluralRules", "Segmenter", "DisplayNames" };
        for (int i = 0; i < 8; i++) JS_SetPropertyStr(ctx, intl, cn[i], JS_NewCFunction2(ctx, js_intl_ctor, cn[i], 0, JS_CFUNC_constructor, 0));
        JS_SetPropertyStr(ctx, g, "Intl", intl);
    }
    JS_SetPropertyStr(ctx, g, "history", js_history_make(ctx));   /* real History state machine (history.c): pushState sets history.state */
}

#include "core/timing/performance.h"        /* js_performance_make */
#include "modules/crypto.h"                 /* js_crypto_make */
#include "core/trustedtypes/trusted_types.h"/* js_trusted_types_make */
#include "core/timing/timers.h"             /* js_set_timer (setTimeout/rAF/queueMicrotask -> scheduler flows) */
#include "modules/storage.h"                /* js_storage_get/set (local/sessionStorage) */
#include "platform/urlobj.h"                /* js_url_ctor/js_url_canparse/js_searchparams_ctor/js_request_ctor/js_headers_ctor */
#include "core/html/forms/formdata.h"       /* js_formdata_ctor */
#include "modules/encoding.h"               /* js_textencoder_ctor/js_textdecoder_ctor */
#include "core/fileapi/filereader.h"        /* js_filereader_ctor */
#include "core/fileapi/blob.h"              /* js_blob_ctor/js_file_ctor */
#include "core/loader/response.h"           /* js_response_ctor */

/* Time/random are EXTERNAL INPUT -> OPAQUE (a branch on Math.random()/Date.now() must FORK, values are shapes;
   a REPLAY-SOUNDNESS requirement too), plus the timer/storage/URL/fetch-object web globals. Second half of the
   window-globals install (js_concolic_read/js_noop from opaque.h, included above). */
void install_window_objects(JSContext *ctx, JSValue g) {
    JSValue mo = JS_GetPropertyStr(ctx, g, "Math");
    if (JS_IsObject(mo)) JS_SetPropertyStr(ctx, mo, "random", JS_NewCFunction(ctx, js_concolic_read, "random", 0));
    JS_FreeValue(ctx, mo);
    JSValue dt = JS_GetPropertyStr(ctx, g, "Date");
    if (JS_IsObject(dt)) {
        JS_SetPropertyStr(ctx, dt, "now", JS_NewCFunction(ctx, js_concolic_read, "now", 0));
        JSValue dp = JS_GetPropertyStr(ctx, dt, "prototype");   /* new Date().getTime()/valueOf() -> opaque too */
        if (JS_IsObject(dp)) {
            JS_SetPropertyStr(ctx, dp, "getTime", JS_NewCFunction(ctx, js_concolic_read, "getTime", 0));
            JS_SetPropertyStr(ctx, dp, "valueOf", JS_NewCFunction(ctx, js_concolic_read, "valueOf", 0));
        }
        JS_FreeValue(ctx, dp);
    }
    JS_FreeValue(ctx, dt);
    JS_SetPropertyStr(ctx, g, "performance", js_performance_make(ctx));
    JS_SetPropertyStr(ctx, g, "crypto", js_crypto_make(ctx));
    JS_SetPropertyStr(ctx, g, "trustedTypes", js_trusted_types_make(ctx));
    /* Timers: a deferred callback is a FLOW in the one scheduler (js_set_timer), not a real wait. */
    JS_SetPropertyStr(ctx, g, "setTimeout", JS_NewCFunction(ctx, js_set_timer, "setTimeout", 2));
    JS_SetPropertyStr(ctx, g, "setInterval", JS_NewCFunction(ctx, js_set_timer, "setInterval", 2));
    JS_SetPropertyStr(ctx, g, "requestAnimationFrame", JS_NewCFunction(ctx, js_set_timer, "requestAnimationFrame", 1));
    JS_SetPropertyStr(ctx, g, "requestIdleCallback", JS_NewCFunction(ctx, js_set_timer, "requestIdleCallback", 1));
    JS_SetPropertyStr(ctx, g, "queueMicrotask", JS_NewCFunction(ctx, js_set_timer, "queueMicrotask", 1));   /* a queued microtask is a FLOW (invariant), not a native-drained job */
    JS_SetPropertyStr(ctx, g, "clearTimeout", JS_NewCFunction(ctx, js_noop, "clearTimeout", 1));
    JS_SetPropertyStr(ctx, g, "clearInterval", JS_NewCFunction(ctx, js_noop, "clearInterval", 1));
    JS_SetPropertyStr(ctx, g, "cancelAnimationFrame", JS_NewCFunction(ctx, js_noop, "cancelAnimationFrame", 1));
    for (int si = 0; si < 2; si++) {   /* Web storage: values are external input -> opaque getItem; writes no-op */
        JSValue st = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, st, "getItem", JS_NewCFunction(ctx, js_storage_get, "getItem", 1));
        JS_SetPropertyStr(ctx, st, "setItem", JS_NewCFunction(ctx, js_storage_set, "setItem", 2));
        JS_SetPropertyStr(ctx, st, "removeItem", JS_NewCFunction(ctx, js_noop, "removeItem", 1));
        JS_SetPropertyStr(ctx, st, "clear", JS_NewCFunction(ctx, js_noop, "clear", 0));
        JS_SetPropertyStr(ctx, st, "key", JS_NewCFunction(ctx, js_storage_get, "key", 1));
        JS_SetPropertyStr(ctx, g, si ? "sessionStorage" : "localStorage", st);
    }
    { JSValue urlctor = JS_NewCFunction2(ctx, js_url_ctor, "URL", 2, JS_CFUNC_constructor, 0);   /* URL / URLSearchParams: endpoint construction */
      JS_SetPropertyStr(ctx, urlctor, "canParse", JS_NewCFunction(ctx, js_url_canparse, "canParse", 2));
      JS_SetPropertyStr(ctx, g, "URL", urlctor); }
    JS_SetPropertyStr(ctx, g, "URLSearchParams", JS_NewCFunction2(ctx, js_searchparams_ctor, "URLSearchParams", 1, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, g, "Request", JS_NewCFunction2(ctx, js_request_ctor, "Request", 2, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, g, "FormData", JS_NewCFunction2(ctx, js_formdata_ctor, "FormData", 0, JS_CFUNC_constructor, 0));   /* records fields -> POST body params */
    JS_SetPropertyStr(ctx, g, "Headers", JS_NewCFunction2(ctx, js_headers_ctor, "Headers", 1, JS_CFUNC_constructor, 0));      /* records header fields -> required headers */
    JS_SetPropertyStr(ctx, g, "AbortController", JS_NewCFunction2(ctx, js_abortcontroller_ctor, "AbortController", 0, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, g, "TextEncoder", JS_NewCFunction2(ctx, js_textencoder_ctor, "TextEncoder", 0, JS_CFUNC_constructor, 0));   /* real UTF-8 (encoding.c) */
    JS_SetPropertyStr(ctx, g, "TextDecoder", JS_NewCFunction2(ctx, js_textdecoder_ctor, "TextDecoder", 0, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, g, "FileReader", JS_NewCFunction2(ctx, js_filereader_ctor, "FileReader", 0, JS_CFUNC_constructor, 0));   /* readAsText -> attacker content + onload flow */
    JS_SetPropertyStr(ctx, g, "Blob", JS_NewCFunction2(ctx, js_blob_ctor, "Blob", 2, JS_CFUNC_constructor, 0));   /* content taint through .text() */
    JS_SetPropertyStr(ctx, g, "File", JS_NewCFunction2(ctx, js_file_ctor, "File", 3, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, g, "Response", JS_NewCFunction2(ctx, js_response_ctor, "Response", 2, JS_CFUNC_constructor, 0));   /* body taint through .json()/.text() */
}

#include "core/frame/winname.h"    /* js_winname_get/set — window.name attacker source */
#include "core/frame/location.h"   /* make_location + window.location getset (nav @S sink) */
#include "core/frame/navigator.h"  /* js_navigator_make */
extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);   /* scheduler edge: register a handler flow */
extern void solve_add(JSContext *ctx, const char *sink, const char *sctx, JSValueConst val);   /* @S sink recorder (solver/solve.h) */
extern JSValue g_concolic;   /* opaque sentinel (opaque.h) — the opened Window is a cross-context frame */

/* window.open(url): opens a browsing context at `url` — a NAV sink like location.assign, classified by the
   "url" sink context (solve_html.c): a finding is emitted ONLY when the constructed URL is a `javascript:`
   scheme that EXECUTES (X9 fires). A plain navigation to an attacker origin is an open-redirect, NOT an XSS —
   it is deliberately NOT a finding (the "url" breakout predicate excludes it). Returns the opened Window. */
static JSValue js_window_open(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc >= 1) solve_add(ctx, "window.open", "url", argv[0]);   /* @S: javascript:-URL XSS only */
    return JS_DupValue(ctx, g_concolic);
}

/* window.name (attacker source, page-write overrides), window.location (getset -> `location = url` is a nav @S
   sink), navigator, window.open (nav sink), and window.addEventListener. The window's own properties (Blink
   LocalDOMWindow attributes), installed before the DOM-spine init so boot sees them. */
void install_window_props(JSContext *ctx, JSValue g) {
    { JSAtom na = JS_NewAtom(ctx, "name");
      JS_DefinePropertyGetSet(ctx, g, na,
          JS_NewCFunction2(ctx, (JSCFunction *)js_winname_get, "get name", 0, JS_CFUNC_getter, 0),
          JS_NewCFunction2(ctx, (JSCFunction *)js_winname_set, "set name", 1, JS_CFUNC_setter, 0), JS_PROP_CONFIGURABLE);
      JS_FreeAtom(ctx, na); }
    { JSValue loc = make_location(ctx);   /* stores the window.location singleton internally (location.c) */
      JSAtom la = JS_NewAtom(ctx, "location");
      JS_DefinePropertyGetSet(ctx, g, la,
          JS_NewCFunction2(ctx, (JSCFunction *)js_window_location_get, "get", 0, JS_CFUNC_getter, 0),
          JS_NewCFunction2(ctx, (JSCFunction *)js_window_location_set, "set", 1, JS_CFUNC_setter, 0), JS_PROP_CONFIGURABLE);
      JS_FreeAtom(ctx, la); JS_FreeValue(ctx, loc); }
    JS_SetPropertyStr(ctx, g, "navigator", js_navigator_make(ctx));   /* concolic standard props + sendBeacon/serviceWorker + permissions */
    JS_SetPropertyStr(ctx, g, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, g, "removeEventListener", JS_NewCFunction(ctx, js_noop, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, g, "open", JS_NewCFunction(ctx, js_window_open, "open", 3));   /* window.open(url) -> nav @S sink */
}
