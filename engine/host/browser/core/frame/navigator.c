/* THE NAVIGATOR INTERFACE — Blink core/frame, the client-identity half of the browsing context.
 *
 * WHY IT MATTERS MORE THAN ITS SIZE SUGGESTS. `navigator` was absent, and a missing global is a THROW: a bundle
 * doing `navigator.userAgent.indexOf("Chrome")` aborted boot before reaching a single endpoint. That is the
 * forcing function working — the capability was not built — and this is the root fix.
 *
 * THE DESIGN DECISION IS PER MEMBER, AND IT IS THE ONE CLAUDE.md STATES FOR matchMedia. Navigator's members
 * split cleanly in two, and getting the split wrong loses code either way:
 *
 *   SPEC-FIXED members are CONCRETE. HTML says `appCodeName` MUST return "Mozilla", `appName` MUST return
 *   "Netscape", `product` MUST return "Gecko", `vendorSub` MUST return the empty string, and javaEnabled() and
 *   taintEnabled() MUST return false. There is no other world for a branch to fork into, so a concolic here
 *   would model an ignorance the engine does not have and fork a branch whose sibling cannot exist.
 *
 *   ENVIRONMENT members are CONCOLIC WITH A CONCRETE EXAMPLE. `userAgent`, `platform`, `language`, `onLine`,
 *   `hardwareConcurrency`, `maxTouchPoints` are exactly the values a bundle GATES ITS CODE ON — the mobile
 *   path, the Safari workaround, the locale-specific host, the offline queue — and every one of those gates
 *   hides endpoints this tool exists to find. Concretising them picks one arm and DELETES the others; that is
 *   the "a modelable value collapsed to bare-concrete deletes the fork and its coverage" failure. So each
 *   carries the value a real Chrome would report as its EXAMPLE (so `ua.slice(0,4)` computes "Mozi" and a
 *   pinned comparison yields a real @H value) while staying opaque for control flow (so both arms run).
 *
 * The examples are a real desktop Chrome's. They are examples, not claims: the engine reports what the code
 * COMPUTED from them, and the fork is what stops the choice of example from deciding which code is reached. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "solver/concolic.h"
#include "core/frame/navigator.h"
#include "core/html/user_activation.h"
#include "core/idl_args.h"

/* A real desktop Chrome's identity, used as the EXAMPLE for the environment members. `appVersion` is the user
   agent minus the leading "Mozilla/", which is what HTML says it is. */
#define NAV_UA_PREFIX "Mozilla/"
#define NAV_UA_REST   "5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) " \
                      "Chrome/131.0.0.0 Safari/537.36"

/* An ENVIRONMENT member: opaque for control flow, carrying what a real browser would answer. One helper so a
   later member cannot accidentally be added as bare-concrete — the shape and the source identity are the same
   string here because a Navigator member IS its own source. */
static void nav_env(JSContext *ctx, JSValueConst nav, const char *name, JSValue example)
{
    char path[64];
    JSValue v;

    DCHECK(strlen(name) + 11 < sizeof(path), "a Navigator member name longer than any in the IDL");
    snprintf(path, sizeof(path), "navigator.%s", name);
    v = concolic_new(ctx, path, path, example);
    CHECK(!JS_IsException(v), "minting a Navigator environment value failed");
    JS_SetPropertyStr(ctx, (JSValue)nav, name, v);
}

/* HTML: "must return false". Both of them, and for the same reason — they are compatibility relics the spec
   pins so that feature detection cannot be fooled. A no-effect would be a stub; returning the value the spec
   states is the implementation. */
static JSValue js_nav_false(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_FALSE;
}

/* HTML §6.4.4: `partial interface Navigator { [SameObject] readonly attribute UserActivation userActivation; }`.
   "The userActivation getter steps are to return this's relevant global object's associated UserActivation" —
   which user_activation.c minted with this realm, so the SAME object comes back on every read without this
   getter caching anything, and the two booleans behind it are §6.4.1's real state rather than a constant.
   AN ACCESSOR, NOT A DATA PROPERTY, because the IDL says `readonly attribute` and a page assigning to it must
   not replace the object. That every other member above is a data property a page can overwrite is Navigator's
   own unbuilt half — this interface has no prototype here and its members are on the instance — and the way to
   fix it is to give Navigator its interface, not to add one more member with the same defect. */
static JSValue js_nav_user_activation(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)this_val; (void)magic;
    return user_activation_object(ctx);
}

void navigator_install(JSContext *ctx, JSValueConst global)
{
    JSValue nav, langs;

    DCHECK(JS_IsObject(global), "navigator_install was given something that is not the global object");

    nav = JS_NewObject(ctx);
    CHECK(!JS_IsException(nav), "the Navigator allocation failed");

    /* ---- SPEC-FIXED: concrete, because the spec admits no other answer ---- */
    JS_SetPropertyStr(ctx, nav, "appCodeName", JS_NewString(ctx, "Mozilla"));
    JS_SetPropertyStr(ctx, nav, "appName",     JS_NewString(ctx, "Netscape"));
    JS_SetPropertyStr(ctx, nav, "product",     JS_NewString(ctx, "Gecko"));
    JS_SetPropertyStr(ctx, nav, "vendorSub",   JS_NewString(ctx, ""));
    /* HTML pins productSub to one of two strings and vendor to one of three; these are the pair a Chrome
       reports, and they are concrete because the spec's alternatives are other BROWSERS, not other worlds this
       document could be in. A bundle branching on them is branching on which browser it is running in, and
       this engine knows: it is modelling one. */
    JS_SetPropertyStr(ctx, nav, "productSub",  JS_NewString(ctx, "20030107"));
    JS_SetPropertyStr(ctx, nav, "vendor",      JS_NewString(ctx, "Google Inc."));
    JS_SetPropertyStr(ctx, nav, "javaEnabled",
                      JS_NewCFunction(ctx, js_nav_false, "javaEnabled", 0));
    JS_SetPropertyStr(ctx, nav, "taintEnabled",
                      JS_NewCFunction(ctx, js_nav_false, "taintEnabled", 0));

    /* ---- ENVIRONMENT: concolic, example = what a real Chrome answers ---- */
    nav_env(ctx, nav, "userAgent",  JS_NewString(ctx, NAV_UA_PREFIX NAV_UA_REST));
    nav_env(ctx, nav, "appVersion", JS_NewString(ctx, NAV_UA_REST));
    nav_env(ctx, nav, "platform",   JS_NewString(ctx, "Win32"));
    nav_env(ctx, nav, "language",   JS_NewString(ctx, "en-US"));
    nav_env(ctx, nav, "onLine",     JS_TRUE);
    nav_env(ctx, nav, "cookieEnabled", JS_TRUE);
    nav_env(ctx, nav, "pdfViewerEnabled", JS_TRUE);
    /* WebDriver: a page's anti-automation gate. The engine IS automation, but what a page can observe is the
       flag a browser sets, and BOTH answers lead to code worth reaching — which is exactly why it forks. */
    nav_env(ctx, nav, "webdriver",  JS_FALSE);
    nav_env(ctx, nav, "hardwareConcurrency", JS_NewInt32(ctx, 8));
    nav_env(ctx, nav, "deviceMemory",        JS_NewInt32(ctx, 8));
    /* Pointer Events. `maxTouchPoints === 0` is the desktop-vs-touch gate a responsive bundle routes on, and
       the two arms ship different code. */
    nav_env(ctx, nav, "maxTouchPoints",      JS_NewInt32(ctx, 0));

    /* NavigatorLanguage's `languages` is a FrozenArray<DOMString>, so it is a real frozen array rather than a
       scalar — a bundle writes `navigator.languages.includes("de")` and `languages[0]`, and both must work.
       Its ELEMENT is the same concolic source `language` is, so a branch on either forks the same way.
       Frozen means: elements non-writable and non-configurable (which is what DefinePropertyValue gives with
       only ENUMERABLE set) and the array not extensible. */
    langs = JS_NewArray(ctx);
    CHECK(!JS_IsException(langs), "the navigator.languages allocation failed");
    {
        JSValue el = concolic_new(ctx, "navigator.language", "navigator.language",
                                  JS_NewString(ctx, "en-US"));
        CHECK(!JS_IsException(el), "minting navigator.languages[0] failed");
        JS_DefinePropertyValueUint32(ctx, langs, 0, el, JS_PROP_ENUMERABLE);
    }
    JS_PreventExtensions(ctx, langs);
    JS_SetPropertyStr(ctx, nav, "languages", langs);

    /* HTML §6.4.4's member on Navigator. It is installed HERE and not from user_activation.c because this is
       the only thing that reaches the Navigator object, and the object it answers with was built with the
       realm — so a `navigator.userActivation.isActive` gate reads the Window's real activation state. */
    idl_install_accessor(ctx, nav, "userActivation", js_nav_user_activation, 0, -1);

    /* HTML: `clientInformation` is the SAME Navigator object, not a copy. */
    JS_SetPropertyStr(ctx, (JSValue)global, "clientInformation", JS_DupValue(ctx, nav));
    JS_SetPropertyStr(ctx, (JSValue)global, "navigator", nav);
}
