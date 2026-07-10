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
#include "solver/opaque.h"              /* js_noop / js_opaque_stub */

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
        JS_SetPropertyStr(ctx, idb, "databases", JS_NewCFunction(ctx, js_opaque_stub, "databases", 0));
        JS_SetPropertyStr(ctx, idb, "cmp", JS_NewCFunction(ctx, js_opaque_stub, "cmp", 2));
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
