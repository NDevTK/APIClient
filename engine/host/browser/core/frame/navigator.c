/* THE NAVIGATOR INTERFACE — HTML §8.10.1, Blink core/frame, the client-identity half of the browsing context.
 *
 * WHY IT MATTERS MORE THAN ITS SIZE SUGGESTS. `navigator` was absent, and a missing global is a THROW: a bundle
 * doing `navigator.userAgent.indexOf("Chrome")` aborted boot before reaching a single endpoint.
 *
 * IT IS AN INTERFACE, AND IT WAS NOT ONE. Every member below is `readonly attribute` on a PROTOTYPE, and they
 * were writable data properties on the instance — which is not a placement detail, it is four things a page
 * observes and real bundles do all four: `navigator.userAgent = "x"` stuck (a browser ignores it in sloppy mode
 * and throws in strict), `Object.getOwnPropertyNames(navigator)` listed every member (a browser lists none),
 * `delete navigator.userAgent` succeeded, and `Navigator.prototype` and `window.Navigator` did not exist at all
 * — so `navigator instanceof Navigator` threw and every UA sniffer that patches the prototype patched nothing.
 *
 * THE NAVIGATOR COMPATIBILITY MODE IS THE FACT THAT DECIDES HALF THIS FILE, and §8.10.1.1 says so: "The user
 * agent has a navigator compatibility mode, which is either Chrome, Gecko, or WebKit", and it "constrains the
 * NavigatorID mixin to the combinations of attribute values and presence of taintEnabled() and oscpu that are
 * known to be compatible with existing web content". THIS USER AGENT'S MODE IS CHROME. Stated once, it decides
 * four things that were otherwise four independent choices to get inconsistent: productSub is "20030107" (Gecko
 * would be "20100101"), vendor is "Google Inc." (Gecko the empty string, WebKit "Apple Computer, Inc."),
 * appVersion is the WHOLE trail after "Mozilla/" (Gecko truncates it at the first ";"), and `taintEnabled()`
 * and `oscpu` DO NOT EXIST — the spec puts them in a partial interface a user agent supports only "If the
 * navigator compatibility mode is Gecko". `taintEnabled` was installed here and is deleted with this line: a
 * Chrome-mode Navigator that answers it is a combination the sentence above exists to forbid, and Chrome itself
 * has not had it for years.
 *
 * THE OTHER DESIGN DECISION IS PER MEMBER, AND IT IS THE ONE CLAUDE.md STATES FOR matchMedia. Navigator's
 * members split cleanly in two, and getting the split wrong loses code either way:
 *
 *   SPEC-FIXED members are CONCRETE. HTML says `appCodeName` MUST return "Mozilla", `appName` MUST return
 *   "Netscape", `product` MUST return "Gecko", `vendorSub` MUST return the empty string, and javaEnabled()
 *   MUST return false. There is no other world for a branch to fork into, so a concolic here would model an
 *   ignorance the engine does not have and fork a branch whose sibling cannot exist. `productSub` and `vendor`
 *   are concrete for the same reason once the compatibility mode is stated: their alternatives are other
 *   BROWSERS, not other worlds this document could be in.
 *
 *   ENVIRONMENT members are CONCOLIC WITH A CONCRETE EXAMPLE. `userAgent`, `platform`, `language`, `onLine`,
 *   `hardwareConcurrency`, `maxTouchPoints` are exactly the values a bundle GATES ITS CODE ON — the mobile
 *   path, the Safari workaround, the locale-specific host, the offline queue — and every one of those gates
 *   hides endpoints this tool exists to find. Concretising them picks one arm and DELETES the others; that is
 *   the "a modelable value collapsed to bare-concrete deletes the fork and its coverage" failure. So each
 *   carries the value a real desktop Chrome would report as its EXAMPLE (so `ua.slice(0,4)` computes "Mozi"
 *   and a pinned comparison yields a real @H value) while staying opaque for control flow.
 *
 * WHAT IS HONESTLY ABSENT, and named by the IDL audit rather than by a comment that can go stale:
 * NavigatorPlugins' `plugins` and `mimeTypes` (§8.10.1.6's PluginArray/MimeTypeArray/Plugin/MimeType, four
 * interfaces this engine has not built) and NavigatorContentUtils' registerProtocolHandler /
 * unregisterProtocolHandler. A page reading one gets `undefined` from a member that does not exist, which is
 * the forcing function; a shape-only object with the right member names would be the stub the audit exists to
 * expose. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "solver/concolic.h"
#include "core/frame/navigator.h"
#include "core/html/user_activation.h"
#include "core/idl_args.h"
#include "core/permissions/permissions.h"
#include "core/realm.h"

/* A real desktop Chrome's identity, used as the EXAMPLE for the environment members. §8.10.1.1's appVersion
   getter steps are a SUBSTRING of this rather than a second string: "Let trail be the substring of userAgent
   that follows the `Mozilla/` prefix", and in Chrome mode the answer is that trail whole. Two constants that
   could disagree would be one fact answered from two places; the split is where the spec puts it. */
#define NAV_UA_PREFIX "Mozilla/"
#define NAV_UA_REST   "5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) " \
                      "Chrome/131.0.0.0 Safari/537.36"
#define NAV_UA        NAV_UA_PREFIX NAV_UA_REST

/* THE MEMBER LIST, IN ONE PLACE, because it is read THREE times — the magic a getter carries is an index into
   it, the per-realm record is filled at those indices, and the install walks it to define the accessors. Three
   hand-kept lists is a member that exists in two of them and not the third, which is a getter answering
   undefined with nothing to say so; one X-list makes that unspellable, and the record's completeness check
   below is the other half of the assertion.
   `userActivation` is NOT here: it is the one member whose value is not a fact stored for the realm but §6.4.1
   state read at the moment of the call, so it has its own getter. */
/* THE THIRD COLUMN IS WEB IDL §3.9's EXPOSURE, and it is a column rather than a branch for the same reason the
   other two are: a member's IDL states it, so it is DATA about the member and the install reads it off the one
   list. `NavigatorDeviceMemory` is the mixin that carries [SecureContext] — the attribute is on the MIXIN, so
   every member of it inherits the condition — and Chrome accordingly has no `deviceMemory` property on
   Navigator.prototype over `http`. That is not a value difference a page shrugs at: `if (navigator.deviceMemory
   > 4)` and `'deviceMemory' in navigator` both take the other road, and whatever is down that road is code
   this engine would otherwise never run. */
#define NAV_MEMBERS(X)                                                             \
    /* NavigatorID — §8.10.1.1 */                                                  \
    X(APP_CODE_NAME,         "appCodeName",          IDL_EXPOSED)                  \
    X(APP_NAME,              "appName",              IDL_EXPOSED)                  \
    X(APP_VERSION,           "appVersion",           IDL_EXPOSED)                  \
    X(PLATFORM,              "platform",             IDL_EXPOSED)                  \
    X(PRODUCT,               "product",              IDL_EXPOSED)                  \
    X(PRODUCT_SUB,           "productSub",           IDL_EXPOSED)                  \
    X(USER_AGENT,            "userAgent",            IDL_EXPOSED)                  \
    X(VENDOR,                "vendor",               IDL_EXPOSED)                  \
    X(VENDOR_SUB,            "vendorSub",            IDL_EXPOSED)                  \
    /* NavigatorLanguage — §8.10.1.2 */                                            \
    X(LANGUAGE,              "language",             IDL_EXPOSED)                  \
    X(LANGUAGES,             "languages",            IDL_EXPOSED)                  \
    /* NavigatorOnLine — §8.10.1.3 */                                              \
    X(ON_LINE,               "onLine",               IDL_EXPOSED)                  \
    /* NavigatorCookies — §8.10.1.5 */                                             \
    X(COOKIE_ENABLED,        "cookieEnabled",        IDL_EXPOSED)                  \
    /* NavigatorPlugins — §8.10.1.6 */                                             \
    X(PDF_VIEWER_ENABLED,    "pdfViewerEnabled",     IDL_EXPOSED)                  \
    /* NavigatorConcurrentHardware — HTML §10 */                                   \
    X(HARDWARE_CONCURRENCY,  "hardwareConcurrency",  IDL_EXPOSED)                  \
    /* NavigatorAutomationInformation — WebDriver §12 */                           \
    X(WEBDRIVER,             "webdriver",            IDL_EXPOSED)                  \
    /* NavigatorDeviceMemory — Device Memory §3, and the mixin is `[SecureContext]` */ \
    X(DEVICE_MEMORY,         "deviceMemory",         IDL_SECURE_CONTEXT)           \
    /* partial interface Navigator — Pointer Events §12 */                         \
    X(MAX_TOUCH_POINTS,      "maxTouchPoints",       IDL_EXPOSED)

#define NAV_ENUM_ONE(id, str, exposure) NAV_##id,
#define NAV_NAME_ONE(id, str, exposure) str,
#define NAV_EXPOSURE_ONE(id, str, exposure) exposure,

enum { NAV_MEMBERS(NAV_ENUM_ONE) NAV_N };
static const char *const NAV_NAME[] = { NAV_MEMBERS(NAV_NAME_ONE) };
static const IdlExposure NAV_EXPOSURE[] = { NAV_MEMBERS(NAV_EXPOSURE_ONE) };

/* THE TWO MEMBERS THIS MODE FORBIDS. §8.10.1.1 puts them in a partial interface the user agent supports only
   "if the navigator compatibility mode is Gecko", and the published html.idl carries that partial FLAT — the
   condition is a sentence, not an extended attribute. So an audit of this file against the corpus sees two
   members nobody installed and cannot tell "this UA must not have them" from "nobody has written them yet",
   which is how a member the spec FORBIDS here gets added by someone working an ABSENT list. Stated here, beside
   the mode the four lines above commit to, and asserted per realm by idl_members_excluded. */
static const char *const NAV_MODE_EXCLUDED[] = { "taintEnabled", "oscpu" };

/* THE CLASS IS THE BRAND. Web IDL §3.7.5's check on every getter is "does esValue have the interface's internal
   slot", and the one object per realm WEARS the class, so the check is a class-id comparison a page cannot
   forge. It carries no per-object data — the values are the realm's — so it needs no finalizer and no gc_mark. */
static JSClassID g_nav_class;
static int g_vals_slot = -1;   /* this realm's member VALUES, indexed by the enum above */
static int g_obj_slot  = -1;   /* this realm's one Navigator */
static int g_id_java_enabled = -1;

/* WEB IDL §3.7.5's BRAND CHECK. `Navigator.prototype.userAgent` read off a plain object is a TypeError, and a
   page tells that apart from `undefined` — a feature detector that probes the descriptor and applies the getter
   reads the throw as "this is a real interface". It is a real throw and not an assert for exactly that reason. */
static bool nav_brand(JSContext *ctx, JSValueConst this_val)
{
    DCHECK(g_nav_class != 0, "a Navigator member ran before navigator_init declared the class — the member is "
                             "only reachable through a prototype the per-realm install builds, so there is no "
                             "route here that has not run the declaration first");
    if (JS_GetClassID(this_val) == g_nav_class) return true;
    JS_ThrowTypeError(ctx, "a Navigator member was reached on something that is not a Navigator");
    return false;
}

/* THE HALF OF "THIS's ..." THIS ENGINE CAN ANSWER, asserted rather than assumed — the same shape, and the same
   reason, as user_activation.c's. A C member runs in the realm that DEFINED it (js_call_c_function takes `ctx`
   from the function object), so an ordinary `navigator.userAgent` arrives with the ctx of the document whose
   prototype it went through, which is the right Navigator. What does NOT arrive right is one realm's getter
   applied to ANOTHER realm's Navigator: the values would come out of the getter's realm, so `languages` would
   answer a frozen array belonging to a different document (breaking the SameObject the spec states for it) and
   `userActivation` would report a different Window's interaction. */
static void nav_assert_this_realm(JSContext *ctx, JSValueConst this_val)
{
    JSValue own = realm_value_get(ctx, g_obj_slot);
    bool same = JS_VALUE_GET_PTR(own) == JS_VALUE_GET_PTR(this_val);

    JS_FreeValue(ctx, own);
    DCHECK(same, "a Navigator member was reached through ONE realm's Navigator.prototype on ANOTHER realm's "
                 "Navigator — answering out of the member's own realm reports the wrong document's values. "
                 "BUILD the Navigator that carries its own realm's record: give the instance the record as its "
                 "class opaque (with the finalizer, gc_mark and cow_capture_host_record contract that entails) "
                 "so the member reads it off THIS, and delete the two realm slots below");
}

/* THE VALUE A MEMBER ANSWERS WITH, out of this realm's record. Owned — the caller returns it. */
static JSValue nav_value(JSContext *ctx, int idx)
{
    JSValue rec = realm_value_get(ctx, g_vals_slot);
    JSValue v;

    DCHECK(idx >= 0 && idx < NAV_N, "a Navigator getter was installed with a magic that is not a member index "
                                    "— the magic IS the index into the one member X-list");
    v = JS_GetPropertyUint32(ctx, rec, (uint32_t)idx);
    JS_FreeValue(ctx, rec);
    DCHECK(!JS_IsUndefined(v), "a Navigator member's realm record holds nothing at its index — the member list "
                               "and the record builder are one X-list, so an empty index means a member was "
                               "declared and never given the value its IDL says it answers with");
    return v;
}

/* EVERY DECLARED MEMBER'S GETTER, once. Its magic is its index; there is nothing per member to write, which is
   what stops a member from arriving with a hand-written getter that forgets the brand check. */
static JSValue js_nav_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    if (!nav_brand(ctx, this_val)) return JS_EXCEPTION;
    nav_assert_this_realm(ctx, this_val);
    return nav_value(ctx, magic);
}

/* HTML §8.10.1.6: "The NavigatorPlugins mixin's javaEnabled() method steps are to return false." A no-effect
   would be a stub; returning the value the spec states is the implementation. */
static JSValue js_nav_java_enabled(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                   int magic)
{
    (void)argc; (void)argv; (void)magic;
    if (!nav_brand(ctx, this_val)) return JS_EXCEPTION;
    return JS_FALSE;
}

/* HTML §6.4.4: `partial interface Navigator { [SameObject] readonly attribute UserActivation userActivation; }`.
   "The userActivation getter steps are to return this's relevant global object's associated UserActivation" —
   which user_activation.c minted with this realm, so the SAME object comes back on every read without this
   getter caching anything, and the two booleans behind it are §6.4.1's real state rather than a constant. It is
   not in the record because it is not a value stored for the realm: it is state read at the call. */
static JSValue js_nav_user_activation(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!nav_brand(ctx, this_val)) return JS_EXCEPTION;
    nav_assert_this_realm(ctx, this_val);
    return user_activation_object(ctx);
}

/* PERMISSIONS §6.1: `partial interface Navigator { [SameObject] readonly attribute Permissions permissions; }`,
   installed here for the reason `userActivation` is — the ATTRIBUTE belongs on §3.7's interface prototype
   object, which is this component's, while the VALUE is the permissions component's one object for the realm.
   [SameObject] therefore comes from where that object is KEPT and not from a cache in this getter. */
static JSValue js_nav_permissions(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!nav_brand(ctx, this_val)) return JS_EXCEPTION;
    nav_assert_this_realm(ctx, this_val);
    return permissions_object(ctx);
}

/* HTML §7.2.5's two Window members that name this object. `navigator` is a plain `readonly attribute Navigator`
   and `clientInformation` is `[Replaceable] readonly attribute Navigator` — the legacy alias, which the IDL
   marks replaceable and the real one does not, so they install through different helpers. BOTH read the ONE
   realm slot rather than one of them holding a second reference: `navigator === clientInformation` is what HTML
   means by "legacy alias of .navigator", and two stored references are two things that can come apart. */
static JSValue js_win_navigator(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)this_val; (void)magic;
    return realm_value_get(ctx, g_obj_slot);   /* OWNED — realm_value_get asserts the realm ran its install */
}

/* THIS REALM'S Navigator, for a PARTIAL INTERFACE another component owns. Storage §2's `navigator.storage` is
   declared on `partial interface Navigator` by a different standard, so the member belongs to that component
   and the OBJECT belongs to this one — which is exactly the shape a partial interface has. It reads the one
   realm slot, so a partial member is installed on the same object §7.2.5 hands the page. OWNED. */
JSValue navigator_object(JSContext *ctx)
{
    return realm_value_get(ctx, g_obj_slot);
}

/* ---- the per-realm record ------------------------------------------------------------------------------- */

static void nav_put(JSContext *ctx, JSValueConst rec, int idx, JSValue v)
{
    DCHECK(idx >= 0 && idx < NAV_N, "a Navigator member value was recorded at an index that is not a member");
    CHECK(!JS_IsException(v), "a Navigator member's value could not be allocated");
    JS_SetPropertyUint32(ctx, rec, (uint32_t)idx, v);
}

/* An ENVIRONMENT member: opaque for control flow, carrying what a real browser would answer. One helper so a
   later member cannot accidentally be added as bare-concrete — the shape and the source identity are the same
   string here because a Navigator member IS its own source. */
static void nav_env(JSContext *ctx, JSValueConst rec, int idx, JSValue example)
{
    char path[64];
    JSValue v;

    DCHECK(idx >= 0 && idx < NAV_N, "a Navigator environment value was minted for a non-member index");
    DCHECK(strlen(NAV_NAME[idx]) + 11 < sizeof(path), "a Navigator member name longer than any in the IDL");
    snprintf(path, sizeof(path), "navigator.%s", NAV_NAME[idx]);
    v = concolic_new(ctx, path, path, example);
    CHECK(!JS_IsException(v), "minting a Navigator environment value failed");
    JS_SetPropertyUint32(ctx, rec, (uint32_t)idx, v);
}

/* THIS REALM'S MEMBER VALUES, built with the realm. Written here and not lazily on a first read for the reason
   §3.7 makes every prototype per realm: a value built on first touch is built inside whichever flow happened to
   ask first, and that flow's baseline becomes every other flow's. Returns an OWNED array; the caller hands it
   to the realm slot. */
static JSValue nav_build_values(JSContext *ctx)
{
    JSValue rec = JS_NewArray(ctx), langs, lang;
    int i;

    CHECK(!JS_IsException(rec), "the Navigator member record could not be allocated");

    /* ---- SPEC-FIXED: concrete, because the spec admits no other answer ---- */
    nav_put(ctx, rec, NAV_APP_CODE_NAME, JS_NewString(ctx, "Mozilla"));
    nav_put(ctx, rec, NAV_APP_NAME,      JS_NewString(ctx, "Netscape"));
    nav_put(ctx, rec, NAV_PRODUCT,       JS_NewString(ctx, "Gecko"));
    nav_put(ctx, rec, NAV_VENDOR_SUB,    JS_NewString(ctx, ""));
    /* Chrome compatibility mode, per the file comment: these two ARE the mode, read off §8.10.1.1's lists. */
    nav_put(ctx, rec, NAV_PRODUCT_SUB,   JS_NewString(ctx, "20030107"));
    nav_put(ctx, rec, NAV_VENDOR,        JS_NewString(ctx, "Google Inc."));

    /* ---- ENVIRONMENT: concolic, example = what a real Chrome answers ---- */
    /* §8.10.1.1's appVersion steps return the empty string unless the user agent starts with `Mozilla/5.0 (`,
       and the trail after `Mozilla/` otherwise. The example below IS that trail, so the modelled user agent has
       to satisfy the test the steps make of it — asserted here rather than left to whoever edits the string. */
    DCHECK(strncmp(NAV_UA, "Mozilla/5.0 (", 13) == 0,
           "§8.10.1.1's appVersion getter returns the EMPTY STRING for a user agent that does not start with "
           "`Mozilla/5.0 (`, and this file's example is the trail after `Mozilla/` — so a modelled user agent "
           "that fails the test would make appVersion answer a substring the spec says is not its value");
    nav_env(ctx, rec, NAV_USER_AGENT,  JS_NewString(ctx, NAV_UA));
    nav_env(ctx, rec, NAV_APP_VERSION, JS_NewString(ctx, NAV_UA_REST));
    nav_env(ctx, rec, NAV_PLATFORM,    JS_NewString(ctx, "Win32"));
    nav_env(ctx, rec, NAV_ON_LINE,     JS_TRUE);
    nav_env(ctx, rec, NAV_COOKIE_ENABLED,    JS_TRUE);
    nav_env(ctx, rec, NAV_PDF_VIEWER_ENABLED, JS_TRUE);
    /* WebDriver: a page's anti-automation gate. The engine IS automation, but what a page can observe is the
       flag a browser sets, and BOTH answers lead to code worth reaching — which is exactly why it forks. */
    nav_env(ctx, rec, NAV_WEBDRIVER,   JS_FALSE);
    /* `unsigned long long` and `double` respectively, which is the only thing their two IDLs differ on here. */
    nav_env(ctx, rec, NAV_HARDWARE_CONCURRENCY, JS_NewInt32(ctx, 8));
    nav_env(ctx, rec, NAV_DEVICE_MEMORY,        JS_NewFloat64(ctx, 8));
    /* Pointer Events. `maxTouchPoints === 0` is the desktop-vs-touch gate a responsive bundle routes on, and
       the two arms ship different code. */
    nav_env(ctx, rec, NAV_MAX_TOUCH_POINTS,     JS_NewInt32(ctx, 0));

    /* §8.10.1.2's two members are ONE FACT: "The most preferred language is the one returned by
       navigator.language", so `languages[0]` is `language` — the SAME concolic value object, not a second one
       minted from the same string. Two would compare unequal under `===` where a browser compares two equal
       strings, and a flow pinning one would leave the other unpinned. */
    nav_env(ctx, rec, NAV_LANGUAGE, JS_NewString(ctx, "en-US"));
    /* Read back out of the record being built, not through nav_value: the realm slot this record is going into
       is not set until this function RETURNS it, and reading a slot before its install is what realm.h's own
       assert is for. */
    lang = JS_GetPropertyUint32(ctx, rec, NAV_LANGUAGE);
    CHECK(!JS_IsException(lang), "navigator.language could not be read back out of the record it was just "
                                 "written into");
    /* `FrozenArray<DOMString>`, so a real Web IDL frozen array rather than a scalar — a bundle writes
       `navigator.languages.includes("de")` and `languages[0]`, and both must work. §8.10.1.2 also says "The
       same object must be returned until the user agent needs to return different values", which is why it
       lives in the realm's record like every other value rather than being rebuilt per read. */
    langs = JS_NewArray(ctx);
    CHECK(!JS_IsException(langs), "the navigator.languages allocation failed");
    JS_SetPropertyUint32(ctx, langs, 0, lang);
    CHECK(idl_freeze_array(ctx, langs) == 0, "navigator.languages could not be frozen");
    nav_put(ctx, rec, NAV_LANGUAGES, langs);

    /* THE OTHER HALF OF THE X-LIST'S ASSERTION: every declared member got a value. A member added to the list
       and not to the builder is a getter that answers undefined, and this is where that is caught rather than
       in whichever bundle happens to read it. */
    for (i = 0; i < NAV_N; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, rec, (uint32_t)i);
        bool got = !JS_IsUndefined(v);
        JS_FreeValue(ctx, v);
        DCHECK(got, "a Navigator member was declared in the X-list and given no value by the record builder");
    }
    return rec;
}

/* ---- the declaration and the per-realm install ------------------------------------------------------------ */

/* ONE PROTOTYPE, ONE INTERFACE OBJECT AND ONE NAVIGATOR PER REALM, built WITH the realm. HTML gives every
   Window an associated Navigator, and §3.7 gives every realm its own interface prototype object — and here
   that is not identity pedantry: js_call_c_function resolves `ctx` from the function object, so a prototype
   built once would answer every document's `navigator.userActivation` out of whichever realm built it. */
static void navigator_install_realm(JSContext *ctx)
{
    JSValue proto, prev, global, nav;
    int i;

    prev = JS_GetClassProto(ctx, g_nav_class);
    DCHECK(JS_IsNull(prev), "navigator_install_realm ran twice in one realm — everything already holding the "
                            "first Navigator.prototype would answer out of a discarded object");
    JS_FreeValue(ctx, prev);

    realm_value_set(ctx, g_vals_slot, nav_build_values(ctx));

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "Navigator.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "Navigator");
    for (i = 0; i < NAV_N; i++)
        idl_install_accessor_exposed(ctx, proto, NAV_NAME[i], js_nav_get, i, -1, NAV_EXPOSURE[i]);
    idl_install_accessor(ctx, proto, "userActivation", js_nav_user_activation, 0, -1);
    idl_install_accessor(ctx, proto, "permissions", js_nav_permissions, 0, -1);
    idl_install_method(ctx, proto, "javaEnabled", 0, g_id_java_enabled);
    idl_members_excluded(ctx, proto, "Navigator", NAV_MODE_EXCLUDED,
                         (int)(sizeof NAV_MODE_EXCLUDED / sizeof NAV_MODE_EXCLUDED[0]),
                         "HTML §8.10.1.1: the user agent supports this partial interface only if the "
                         "navigator compatibility mode is Gecko, and this one is Chrome — productSub is "
                         "\"20030107\" and vendor is \"Google Inc.\"");
    JS_SetClassProto(ctx, g_nav_class, JS_DupValue(ctx, proto));

    /* §3.7.1's INTERFACE OBJECT, on THIS realm's global. Navigator declares no constructor, so
       `new Navigator()` is a TypeError — and its PRESENCE is what `navigator instanceof Navigator` and every
       prototype-patching polyfill needs, which is exactly what this interface had none of. */
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "Navigator", idl_interface_object(ctx, "Navigator", proto));

    nav = JS_NewObjectProtoClass(ctx, proto, g_nav_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(nav), "the Window's associated Navigator could not be allocated");
    realm_value_set(ctx, g_obj_slot, nav);

    idl_install_accessor(ctx, global, "navigator", js_win_navigator, 0, -1);
    idl_install_replaceable(ctx, global, "clientInformation", js_win_navigator, 0);
    JS_FreeValue(ctx, global);
}

void navigator_init(JSContext *ctx)
{
    JSClassDef d = { "Navigator" };

    DCHECK(g_vals_slot < 0, "navigator_init ran twice — the class and the slots are declared once per AGENT");
    /* THE CLASS IS BOTH THE PER-REALM PROTOTYPE SLOT AND THE BRAND: the one object per realm WEARS it, so
       §3.7.5's check is a class-id comparison and a page cannot forge one. */
    JS_NewClassID(JS_GetRuntime(ctx), &g_nav_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_nav_class, &d) == 0,
          "Navigator: the per-realm prototype slot could not be declared");
    g_vals_slot = realm_value_declare(ctx, "HTML §8.10.1 the Navigator's member values");
    g_obj_slot  = realm_value_declare(ctx, "HTML §7.2.5 the Window's associated Navigator");
    /* DECLARED once per agent and INSTALLED per realm, like every other member: a declaration builds a pool
       entry and a member has ONE, so declaring inside the install would mint a second entry for the second
       realm's prototype — which is what the pool's seal asserts against. */
    g_id_java_enabled = idl_method_id(ctx, NULL, 0, js_nav_java_enabled, 0);
    realm_declare_intrinsic(navigator_install_realm);
    /* PERMISSIONS §6.1 IS A PARTIAL INTERFACE OF THIS ONE, so this is where its whole component is declared —
       §3's model, §6.3's PermissionStatus and §6.2's Permissions. Declared AFTER the line above so the
       realm-intrinsic order builds this realm's Navigator before anything that reaches for it, and declared
       HERE rather than in each host's list because a host that has a Navigator has `navigator.permissions`:
       a per-host line is the hand-copied list core/realm.h exists to abolish. */
    permissions_init(ctx);
}

void navigator_free(void)
{
    /* The prototypes, the interface objects, the Navigators and their records are the REALMS' — each is
       released with its context. What the agent holds is the two slots and the member's pool id, and a slot id
       is a class id in a runtime that is going away with it. */
    g_vals_slot = -1;
    g_obj_slot = -1;
    g_id_java_enabled = -1;
    /* PERMISSIONS §6's component is declared from navigator_init, so it is released from here — a component
       released from a list its declaration is not on is a component some host frees and another leaks. */
    permissions_free();
}
