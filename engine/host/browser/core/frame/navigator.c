/* Network-initiating navigator methods — see navigator.h. Extracted from main.c. sendBeacon(url,data) is a
 * real POST (analytics/telemetry endpoint) emitted like fetch; serviceWorker.register(url) fetches + analyzes
 * the SW script as a chunk. A state-mutating POST is never fired to learn — the beacon body's example comes
 * ONLY from this forced-exec serialize (url_solve_holes fills == gate-pinned holes). */
#include <stdlib.h>
#include <stdio.h>
#include "core/frame/navigator.h"
#include "modules/permissions/permissions.h"   /* navigator.permissions — a modeled virtual permission system */
#include "modules/clipboard/clipboard.h"        /* navigator.clipboard — the Async Clipboard module (attacker source) */
#include "modules/credentialmanagement/credentials.h"   /* navigator.credentials — the auth-moat module */
#include "modules/quota/storage_manager.h"       /* navigator.storage — the Storage API module */
#include "bindings/idl.h"        /* idl_dfail_wrap — the shared unbuilt-member DFAIL audit trap */
#include "platform/url.h"        /* url_from_arg, url_solve_holes, has_hole, build_query_params */
#include "solver/endpoint.h"   /* record_endpoint — the shared @H sink */
#include "solver/concolic.h"     /* g_concolic, js_concolic */
#include "solver/source.h"       /* source_candidate — clipboard is an attacker source, delivered raw */
#include "check.h"      /* DFAIL — an unbuilt navigator feature crashes LOUD, never an opaque shrug */

/* Proxy get-trap for the still-unbuilt navigator surface: a read of a member NOT yet implemented DFAILs in dev,
   naming the exact feature to BUILD — the forcing function that replaces the banned g_concolic shrug (which
   silently served every unbuilt member as opaque, hiding a missing browser feature; omitting features is not our
   choice — a browser has them all). Implemented members (own or inherited) delegate to the target; a symbol/
   internal probe returns undefined (not a named feature). In release DFAIL is compiled out and it degrades to
   the concolic opaque (the exemption: no feature is buildable outside dev). */
/* Wrap an object so its unbuilt members DFAIL instead of opaque-shrugging — the shared idl_dfail_wrap audit trap
   (bindings/idl.c). navigator's own sub-objects still carry the "navigator.*" naming; split sub-interfaces name
   themselves (Clipboard, CredentialsContainer, …) at their own make(). */
static JSValue wrap_unbuilt(JSContext *ctx, JSValue obj) { return idl_dfail_wrap(ctx, obj, "navigator"); }

/* taintEnabled()/javaEnabled(): legacy methods Chrome still exposes, both return false headless (real values). */
static JSValue nav_false(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)t; (void)c; (void)v; return JS_FALSE; }
/* vibrate(pattern): no vibration device, but the spec returns a bool success — true (a real value, not a noop). */
static JSValue nav_vibrate(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)t; (void)c; (void)v; return JS_TRUE; }
/* canShare(data): whether the data is shareable — genuinely unknown headless, concolic bool (forks the feature gate). */
static JSValue nav_canshare(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)t; (void)c; (void)v; return js_concolic(ctx, "{canShare}", JS_TRUE); }
/* getAutoplayPolicy(): the autoplay policy for the given type — "allowed" is the desktop default (concolic, forks). */
static JSValue nav_autoplay(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)t; (void)c; (void)v; return js_concolic(ctx, "{autoplayPolicy}", JS_NewString(ctx, "allowed")); }
/* registerProtocolHandler/unregisterProtocolHandler: no real handler registry headless — dedicated documented no-effect. */
static JSValue nav_reg_proto(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)ctx; (void)t; (void)c; (void)v; return JS_UNDEFINED; }

extern void chunk_pending_add(const char *url);              /* scheduler-side (main.c): queue a script chunk for host fetch + analyze */
extern JSValue js_resolved(JSContext *ctx, JSValue val);     /* scheduler-side: wrap a value in a resolved promise */
extern JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);   /* serviceWorker.onmessage -> driven flow */

JSValue js_sw_register(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc >= 1) {
        char *url = url_from_arg(ctx, argv[0]);
        if (url) { if (!has_hole(url)) chunk_pending_add(url); free(url); }   /* -> host fetch + engine analyze */
    }
    return js_resolved(ctx, js_concolic(ctx, "{swRegistration}", JS_UNDEFINED));   /* Promise<ServiceWorkerRegistration> */
}

/* share()/setAppBadge()/clearAppBadge(): resolve to undefined (Promise<undefined>) — no share sheet / app badge
   headless, a dedicated documented no-effect that keeps the await chain flowing. */
static JSValue nav_promise_undef(JSContext *ctx, JSValueConst t, int c, JSValueConst *v) { (void)t; (void)c; (void)v; return js_resolved(ctx, JS_UNDEFINED); }

/* window.navigator — the standard properties as CONCOLIC values (a real desktop Chrome as the example) plus the
   built methods/sub-interfaces. UNBUILT members DFAIL via the wrap_unbuilt trap (never an opaque shrug). */
/* NetworkInformation (navigator.connection): the connection's properties are genuinely unknown headless, so each
   is a CONCOLIC value carrying a realistic desktop EXAMPLE and FORKING at a feature-detection branch — a page
   that adapts on `if (navigator.connection.saveData)` or `connection.effectiveType==='slow-2g'` explores BOTH
   the data-saver and full arms (each may ship a different resource/endpoint), never pinned to one world. */
static JSValue make_connection(JSContext *ctx) {
    JSValue c = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, c, "effectiveType", js_concolic(ctx, "{effectiveType}", JS_NewString(ctx, "4g")));   /* forks 4g/3g/2g/slow-2g adaptive arms */
    JS_SetPropertyStr(ctx, c, "type", js_concolic(ctx, "{connectionType}", JS_NewString(ctx, "wifi")));
    JS_SetPropertyStr(ctx, c, "downlink", js_concolic(ctx, "{downlink}", JS_NewFloat64(ctx, 10.0)));            /* Mbps */
    JS_SetPropertyStr(ctx, c, "rtt", js_concolic(ctx, "{rtt}", JS_NewInt32(ctx, 50)));                          /* ms */
    JS_SetPropertyStr(ctx, c, "saveData", js_concolic(ctx, "{saveData}", JS_FALSE));                            /* forks the data-saver code path */
    JS_SetPropertyStr(ctx, c, "onchange", JS_NULL);
    JS_SetPropertyStr(ctx, c, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));  /* the 'change' listener becomes an orphan flow */
    JS_SetPropertyStr(ctx, c, "removeEventListener", JS_NewCFunction(ctx, js_noop, "removeEventListener", 2));
    return wrap_unbuilt(ctx, c);   /* unbuilt NetworkInformation members DFAIL, never an opaque shrug */
}
/* NavigatorUAData (navigator.userAgentData): UA Client Hints. `mobile`/`platform` and the high-entropy fields
   are CONCOLIC (desktop example, fork the branch) so `if (navigator.userAgentData.mobile)` explores BOTH the
   mobile and desktop code paths (each ships its own bundle/endpoints — the classic responsive moat split).
   getHighEntropyValues(hints) resolves to a concolic detail object; unbuilt members (toJSON, ...) DFAIL. */
static JSValue js_ua_high_entropy(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "platform", js_concolic(ctx, "{uaPlatform}", JS_NewString(ctx, "Windows")));
    JS_SetPropertyStr(ctx, o, "platformVersion", js_concolic(ctx, "{uaPlatformVersion}", JS_NewString(ctx, "15.0.0")));
    JS_SetPropertyStr(ctx, o, "architecture", js_concolic(ctx, "{uaArch}", JS_NewString(ctx, "x86")));
    JS_SetPropertyStr(ctx, o, "bitness", js_concolic(ctx, "{uaBitness}", JS_NewString(ctx, "64")));
    JS_SetPropertyStr(ctx, o, "model", js_concolic(ctx, "{uaModel}", JS_NewString(ctx, "")));
    JS_SetPropertyStr(ctx, o, "uaFullVersion", js_concolic(ctx, "{uaFullVersion}", JS_NewString(ctx, "120.0.0.0")));
    JS_SetPropertyStr(ctx, o, "mobile", js_concolic(ctx, "{uaMobile}", JS_FALSE));
    return js_resolved(ctx, o);
}
static JSValue make_ua_data(JSContext *ctx) {
    JSValue u = JS_NewObject(ctx);
    { JSValue brands = JS_NewArray(ctx), b0 = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, b0, "brand", JS_NewString(ctx, "Chromium")); JS_SetPropertyStr(ctx, b0, "version", JS_NewString(ctx, "120"));
      JS_SetPropertyUint32(ctx, brands, 0, b0); JS_SetPropertyStr(ctx, u, "brands", brands); }
    JS_SetPropertyStr(ctx, u, "mobile", js_concolic(ctx, "{uaMobile}", JS_FALSE));            /* forks mobile/desktop code paths */
    JS_SetPropertyStr(ctx, u, "platform", js_concolic(ctx, "{uaPlatform}", JS_NewString(ctx, "Windows")));
    JS_SetPropertyStr(ctx, u, "getHighEntropyValues", JS_NewCFunction(ctx, js_ua_high_entropy, "getHighEntropyValues", 1));
    return wrap_unbuilt(ctx, u);
}
/* CredentialsContainer (navigator.credentials) — its own Blink module (modules/credentialmanagement/). */
/* StorageManager (navigator.storage) — its own Blink module (modules/quota/). */
/* Clipboard (navigator.clipboard) — its own Blink module (modules/clipboard/), an attacker source. */
JSValue js_navigator_make(JSContext *ctx) {
    JSValue nav = JS_NewObject(ctx);
    const char *UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
    JS_SetPropertyStr(ctx, nav, "userAgent", js_concolic(ctx, "{ua}", JS_NewString(ctx, UA)));
    JS_SetPropertyStr(ctx, nav, "appVersion", js_concolic(ctx, "{ua}", JS_NewString(ctx, UA + 8)));   /* appVersion = UA minus "Mozilla/" */
    JS_SetPropertyStr(ctx, nav, "appName", JS_NewString(ctx, "Netscape"));
    JS_SetPropertyStr(ctx, nav, "appCodeName", JS_NewString(ctx, "Mozilla"));
    JS_SetPropertyStr(ctx, nav, "product", JS_NewString(ctx, "Gecko"));
    JS_SetPropertyStr(ctx, nav, "productSub", JS_NewString(ctx, "20030107"));
    JS_SetPropertyStr(ctx, nav, "vendor", js_concolic(ctx, "{vendor}", JS_NewString(ctx, "Google Inc.")));
    JS_SetPropertyStr(ctx, nav, "vendorSub", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, nav, "platform", js_concolic(ctx, "{platform}", JS_NewString(ctx, "Win32")));
    JS_SetPropertyStr(ctx, nav, "language", js_concolic(ctx, "{lang}", JS_NewString(ctx, "en-US")));
    { JSValue langs = JS_NewArray(ctx); JS_SetPropertyUint32(ctx, langs, 0, JS_NewString(ctx, "en-US")); JS_SetPropertyUint32(ctx, langs, 1, JS_NewString(ctx, "en")); JS_SetPropertyStr(ctx, nav, "languages", langs); }
    JS_SetPropertyStr(ctx, nav, "onLine", js_concolic(ctx, "{online}", JS_TRUE));                     /* forks online/offline; example true */
    JS_SetPropertyStr(ctx, nav, "cookieEnabled", js_concolic(ctx, "{cookieEnabled}", JS_TRUE));
    JS_SetPropertyStr(ctx, nav, "webdriver", js_concolic(ctx, "{webdriver}", JS_FALSE));              /* a real user browser -> false; the bot-gate arm forks too */
    JS_SetPropertyStr(ctx, nav, "doNotTrack", JS_NULL);
    JS_SetPropertyStr(ctx, nav, "hardwareConcurrency", js_concolic(ctx, "{cores}", JS_NewInt32(ctx, 8)));
    JS_SetPropertyStr(ctx, nav, "deviceMemory", js_concolic(ctx, "{mem}", JS_NewInt32(ctx, 8)));
    JS_SetPropertyStr(ctx, nav, "maxTouchPoints", js_concolic(ctx, "{touch}", JS_NewInt32(ctx, 0)));
    JS_SetPropertyStr(ctx, nav, "pdfViewerEnabled", JS_TRUE);
    JS_SetPropertyStr(ctx, nav, "sendBeacon", JS_NewCFunction(ctx, js_send_beacon, "sendBeacon", 2));
    {   /* serviceWorker.register(url) -> analyze the SW script (its endpoints); wrapped so unbuilt sw members DFAIL */
        JSValue sw = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, sw, "register", JS_NewCFunction(ctx, js_sw_register, "register", 1));
        JS_SetPropertyStr(ctx, sw, "addEventListener", JS_NewCFunction(ctx, js_add_listener, "addEventListener", 2));
        JS_SetPropertyStr(ctx, sw, "ready", js_resolved(ctx, js_concolic(ctx, "{swRegistration}", JS_UNDEFINED)));
        JS_SetPropertyStr(ctx, nav, "serviceWorker", wrap_unbuilt(ctx, sw));
    }
    /* permissions: a MODELED virtual permission system (real PermissionStatus, 'prompt' default that forks). */
    JS_SetPropertyStr(ctx, nav, "permissions", js_permissions_make(ctx));
    /* connection: NetworkInformation — concolic connection properties that fork adaptive-loading branches. */
    JS_SetPropertyStr(ctx, nav, "connection", make_connection(ctx));
    /* userAgentData: UA Client Hints — concolic mobile/platform that fork the responsive mobile/desktop split. */
    JS_SetPropertyStr(ctx, nav, "userAgentData", make_ua_data(ctx));
    /* credentials: CredentialsContainer — get()/store() resolve to a concolic Credential that forks the auth gate. */
    JS_SetPropertyStr(ctx, nav, "credentials", credentials_make(ctx));
    /* storage: StorageManager — concolic quota/persisted that fork PWA offline/caching feature gates. */
    JS_SetPropertyStr(ctx, nav, "storage", storage_manager_make(ctx));
    /* clipboard: readText()/read() are an ATTACKER-CONTROLLED source ({clipboard}, paste-jacking XSS). */
    JS_SetPropertyStr(ctx, nav, "clipboard", clipboard_make(ctx));
    /* userActivation: whether the frame has ever had / currently has a user gesture — genuinely unknown headless,
       concolic bools (example true) so `if (navigator.userActivation.isActive)` gesture gates explore both arms. */
    { JSValue ua = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, ua, "hasBeenActive", js_concolic(ctx, "{hasBeenActive}", JS_TRUE));
      JS_SetPropertyStr(ctx, ua, "isActive", js_concolic(ctx, "{isActive}", JS_TRUE));
      JS_SetPropertyStr(ctx, nav, "userActivation", ua); }
    JS_SetPropertyStr(ctx, nav, "globalPrivacyControl", js_concolic(ctx, "{gpc}", JS_FALSE));   /* GPC signal: forks the honor-GPC gate */
    JS_SetPropertyStr(ctx, nav, "taintEnabled", JS_NewCFunction(ctx, nav_false, "taintEnabled", 0));
    JS_SetPropertyStr(ctx, nav, "javaEnabled", JS_NewCFunction(ctx, nav_false, "javaEnabled", 0));
    JS_SetPropertyStr(ctx, nav, "vibrate", JS_NewCFunction(ctx, nav_vibrate, "vibrate", 1));
    JS_SetPropertyStr(ctx, nav, "canShare", JS_NewCFunction(ctx, nav_canshare, "canShare", 1));
    JS_SetPropertyStr(ctx, nav, "share", JS_NewCFunction(ctx, nav_promise_undef, "share", 1));
    JS_SetPropertyStr(ctx, nav, "getAutoplayPolicy", JS_NewCFunction(ctx, nav_autoplay, "getAutoplayPolicy", 1));
    JS_SetPropertyStr(ctx, nav, "setAppBadge", JS_NewCFunction(ctx, nav_promise_undef, "setAppBadge", 1));
    JS_SetPropertyStr(ctx, nav, "clearAppBadge", JS_NewCFunction(ctx, nav_promise_undef, "clearAppBadge", 0));
    JS_SetPropertyStr(ctx, nav, "registerProtocolHandler", JS_NewCFunction(ctx, nav_reg_proto, "registerProtocolHandler", 3));
    JS_SetPropertyStr(ctx, nav, "unregisterProtocolHandler", JS_NewCFunction(ctx, nav_reg_proto, "unregisterProtocolHandler", 2));
    JS_SetPropertyStr(ctx, nav, "oscpu", JS_UNDEFINED);   /* Chrome does not expose oscpu (Firefox-only) — genuinely undefined, not unbuilt */
    /* Every remaining IDL member (geolocation/mediaDevices/bluetooth/usb/... ) is
       an UNBUILT browser feature: the trap DFAILs loud naming it, so it is BUILT at the root — never an
       opaque/undefined shrug. Each becomes a real modeled interface (permissions, connection are the first). */
    return wrap_unbuilt(ctx, nav);
}

JSValue js_send_beacon(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc >= 1) {
        char *url = url_from_arg(ctx, argv[0]);
        if (url) {
            char *usolved = url_solve_holes(ctx, url); const char *eurl = usolved ? usolved : url;
            JSValue ep = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, ep, "method", JS_NewString(ctx, "POST"));
            JS_SetPropertyStr(ctx, ep, "url", JS_NewString(ctx, eurl));
            JS_SetPropertyStr(ctx, ep, "source", JS_NewString(ctx, "ast_analysis"));
            JSValue params = JS_NewArray(ctx); build_query_params(ctx, eurl, params); JS_SetPropertyStr(ctx, ep, "params", params);
            if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
                const char *bs = JS_ToCString(ctx, argv[1]);
                if (bs && bs[0]) { char *bsolved = url_solve_holes(ctx, bs); JS_SetPropertyStr(ctx, ep, "body", JS_NewString(ctx, bsolved ? bsolved : bs)); free(bsolved); }
                if (bs) JS_FreeCString(ctx, bs);
            }
            record_endpoint(ctx, ep); free(usolved); free(url);
        }
    }
    return JS_TRUE;
}
