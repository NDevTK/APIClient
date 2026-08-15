/* PERMISSIONS §3's MODEL AND §5.1's READ. See permission_store.h for why step 8 is a source and steps 2, 4
   and 7 are not. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/realm.h"
#include "core/url/url.h"
#include "core/dom/document.h"
#include "core/frame/secure_context.h"
#include "core/frame/window_proxy.h"
#include "core/permissions/permission_store.h"
#include "solver/concolic.h"

/* §3.1's THREE STATES. The strings are the IDL's, and they are the values a page compares against — a bundle
   writes `state === "granted"` and nothing else, so the string IS the value and the enum is only how this
   engine indexes it. */
static const char *const PERMISSION_STATE_STR[PERMISSION_STATE_N] = { "granted", "denied", "prompt" };

const char *permission_state_str(int state)
{
    DCHECK(state >= 0 && state < PERMISSION_STATE_N,
           "a permission state outside §3.1's three was asked for its string — the enum is the whole of what a "
           "PermissionState can be, and a fourth value means a caller invented one");
    return PERMISSION_STATE_STR[state];
}

int permission_state_of(const char *s)
{
    int i;

    if (!s) return -1;
    for (i = 0; i < PERMISSION_STATE_N; i++)
        if (strcmp(s, PERMISSION_STATE_STR[i]) == 0)
            return i;
    return -1;
}

/* ---- §4's REGISTRY OF POWERFUL FEATURES -------------------------------------------------------------------
 *
 * §4: a conforming specification that specifies a powerful feature "MUST give the powerful feature a name in
 * the form of an ascii lowercase string", "MAY define a permission descriptor type that inherits from
 * PermissionDescriptor", "MAY define zero or more aspects", and "MUST register the powerful feature in the
 * Permissions Registry". So a feature is DATA — a name, a descriptor type, a default state, and whether it is
 * also a policy-controlled feature — and this list is that data, one row per feature, read from the feature's
 * OWN specification rather than from the registry table (which lags: it lists four standardized permissions
 * and three provisional ones, and does not carry `camera`, `microphone`, `midi`, `persistent-storage`,
 * `background-sync`, `screen-wake-lock` or the sensors, every one of which its own spec registers).
 *
 * THE COLUMNS, AND WHY EACH IS A COLUMN RATHER THAN A BRANCH:
 *   POLICY — is this ALSO a policy-controlled feature? §5.1 step 4 asks it, and a branch per feature at that
 *     step is the hand-kept list this X-list exists to abolish. `notifications`, `push`, `persistent-storage`,
 *     `background-sync` and `clipboard-write` are powerful features that are NOT policy-controlled, which the
 *     registry states in its own column and their specs confirm by defining no policy-controlled feature.
 *   ASPECT — the ONE member the feature's permission descriptor type declares beyond `name`, or NULL where the
 *     type is the default `PermissionDescriptor`. Every one of them is a `boolean X = false`.
 *   STRONG — §4's PARTIAL ORDER, as the aspect value that is the STRONGER descriptor. It is not always `true`:
 *     Push states `{name:"push", userVisibleOnly:false}` is stronger than `{name:"push", userVisibleOnly:
 *     true}`, the opposite way round from Web MIDI's `sysex` and Media Capture's `panTiltZoom`.
 *   DEFAULT — §4's "default permission state ... If not specified, the permission's default state is
 *     `prompt`". Not one of the specifications read for this list specifies another, so every row is PROMPT
 *     and the column exists so that the first one that does is a row edit rather than a new mechanism.
 *
 * WHAT IS DELIBERATELY ABSENT, each for a reason that is a spec sentence and not a shrug:
 *   "storage-access" — the Storage Access API defines a CUSTOM permission key type ("a tuple consisting of a
 *     site top-level and a site requester") and a custom permission query algorithm (its "denied" is reported
 *     as "prompt", so the user's decision is not revealed). Registered with the default origin key it would
 *     answer for the wrong SCOPE, which is worse than answering not-supported; it arrives with a key type that
 *     is a pair of sites.
 *   "clipboard-read" — the Clipboard API registers exactly ONE powerful feature, "clipboard-write". Chrome
 *     answers a `clipboard-read` query, and Chrome is confirmation rather than the source.
 *   "web-share" — the registry lists it as a policy-controlled feature that is NOT a powerful feature
 *     ("it doesn't require express permission to be used"), so it has no permission state to query. */
#define PERMISSION_FEATURES(X)                                                                                 \
    /* Geolocation API §3.4: "Geolocation is a DEFAULT powerful feature identified by the name geolocation". */\
    X(GEOLOCATION,        "geolocation",          1, NULL,                 0, PERMISSION_PROMPT)               \
    /* Notifications §2.2: "a powerful feature which is identified by the name notifications". */              \
    X(NOTIFICATIONS,      "notifications",        0, NULL,                 0, PERMISSION_PROMPT)               \
    /* Push API: `dictionary PushPermissionDescriptor : PermissionDescriptor { boolean userVisibleOnly =       \
       false; }`, and `{userVisibleOnly:false}` is stronger than `{userVisibleOnly:true}`. */                   \
    X(PUSH,               "push",                 0, "userVisibleOnly",    0, PERMISSION_PROMPT)               \
    /* Web MIDI: `MidiPermissionDescriptor { boolean sysex = false; }`; sysex:true is the stronger. */         \
    X(MIDI,               "midi",                 1, "sysex",              1, PERMISSION_PROMPT)               \
    /* Media Capture §: two powerful features. Only `camera` carries an aspect —                               \
       `CameraDevicePermissionDescriptor { boolean panTiltZoom = false; }`, panTiltZoom:true the stronger. */   \
    X(CAMERA,             "camera",               1, "panTiltZoom",        1, PERMISSION_PROMPT)               \
    X(MICROPHONE,         "microphone",           1, NULL,                 0, PERMISSION_PROMPT)               \
    /* Storage §5: the "persistent-storage" powerful feature, algorithms defaulted except for a permission     \
       state that must agree across every environment of an origin (which the one agent-wide store gives) and  \
       a revocation algorithm that demotes the default bucket. */                                              \
    X(PERSISTENT_STORAGE, "persistent-storage",   0, NULL,                 0, PERMISSION_PROMPT)               \
    /* Screen Wake Lock: the "screen-wake-lock" powerful feature, policy-controlled with 'self'. */            \
    X(SCREEN_WAKE_LOCK,   "screen-wake-lock",     1, NULL,                 0, PERMISSION_PROMPT)               \
    /* Window Management §3.7: "a default powerful feature identified by the name window-management". */       \
    X(WINDOW_MANAGEMENT,  "window-management",    1, NULL,                 0, PERMISSION_PROMPT)               \
    /* Local Font Access: "a default powerful feature that is identified by the name local-fonts". */          \
    X(LOCAL_FONTS,        "local-fonts",          1, NULL,                 0, PERMISSION_PROMPT)               \
    /* Idle Detection: "the idle-detection permission is a default powerful feature". */                       \
    X(IDLE_DETECTION,     "idle-detection",       1, NULL,                 0, PERMISSION_PROMPT)               \
    /* Background Sync: "a default powerful feature that is identified by the name background-sync". */        \
    X(BACKGROUND_SYNC,    "background-sync",      0, NULL,                 0, PERMISSION_PROMPT)               \
    /* Generic Sensor: a sensor type's SENSOR PERMISSION NAMES are powerful feature names, and each concrete   \
       sensor spec names its own. */                                                                           \
    X(ACCELEROMETER,      "accelerometer",        1, NULL,                 0, PERMISSION_PROMPT)               \
    X(GYROSCOPE,          "gyroscope",            1, NULL,                 0, PERMISSION_PROMPT)               \
    X(MAGNETOMETER,       "magnetometer",         1, NULL,                 0, PERMISSION_PROMPT)               \
    X(AMBIENT_LIGHT,      "ambient-light-sensor", 1, NULL,                 0, PERMISSION_PROMPT)               \
    /* Clipboard API §9: `ClipboardPermissionDescriptor { boolean allowWithoutGesture = false; }`, and         \
       `{allowWithoutGesture:true}` is stronger than `{allowWithoutGesture:false}`. */                          \
    X(CLIPBOARD_WRITE,    "clipboard-write",      0, "allowWithoutGesture", 1, PERMISSION_PROMPT)

#define PF_ENUM_ONE(id, name, policy, aspect, strong, def) PF_##id,
#define PF_NAME_ONE(id, name, policy, aspect, strong, def) name,
#define PF_POLICY_ONE(id, name, policy, aspect, strong, def) policy,
#define PF_ASPECT_ONE(id, name, policy, aspect, strong, def) aspect,
#define PF_STRONG_ONE(id, name, policy, aspect, strong, def) strong,
#define PF_DEFAULT_ONE(id, name, policy, aspect, strong, def) def,

enum { PERMISSION_FEATURES(PF_ENUM_ONE) PF_N };
static const char *const PF_NAME[]   = { PERMISSION_FEATURES(PF_NAME_ONE) };
static const int         PF_POLICY[] = { PERMISSION_FEATURES(PF_POLICY_ONE) };
static const char *const PF_ASPECT[] = { PERMISSION_FEATURES(PF_ASPECT_ONE) };
static const int         PF_STRONG[] = { PERMISSION_FEATURES(PF_STRONG_ONE) };
static const int         PF_DEFAULT[] = { PERMISSION_FEATURES(PF_DEFAULT_ONE) };

/* A DESCRIPTOR'S INDEX in the store and in the source record: the feature and its aspect bit. §3.2 says an
   entry is denoted by its descriptor AND its key, and the key is asserted to be one value below — so within
   this agent a descriptor names an entry on its own. */
#define PF_SLOT(d) ((d)->feature * 2 + ((d)->aspect ? 1 : 0))

/* ---- the agent's state ------------------------------------------------------------------------------------
 *
 * THREE THINGS, ALL OF THEM THE AGENT'S. §3.2's store is the user agent's by the standard's own words; the
 * per-feature sources are the user's decisions about this ORIGIN, which is what an instance is (SECURITY.md);
 * and the key is the one permission key every environment of this agent generates.
 *
 * THE STORE AND THE SOURCES ARE JS OBJECTS, so the per-flow COW delta captures a write to either with no new
 * delta kind — a flow that learns the user granted `camera` has learnt it in its own timeline, and its sibling
 * still holds the unknown. A malloc'd table would be one timeline for every flow at once. */
static JSValue g_store;     /* §3.2: index PF_SLOT -> a PermissionState string, or undefined for no entry */
static JSValue g_sources;   /* index by FEATURE -> §5.1 step 8's value for it */
static JSValue g_key;       /* the one permission key: the top-level origin, serialized */
static int     g_ready;
static JSRuntime *g_rt;

/* THE PERMISSION KEY THIS AGENT GENERATES — §5.1 step 5 with the default permission key generation algorithm,
 * which is "return origin", where `origin` is the settings object's TOP-LEVEL ORIGIN. That is permission
 * delegation and the standard says so: "Most powerful features grant permission to the top-level origin and
 * delegate access to the requesting document via Permissions Policy."
 *   IT IS COMPUTED ONCE, because within one agent it cannot differ. An instance is a (browsing-context group,
 * origin) pair, and core/realm.h's top-level creation URL is INHERITED by every realm a navigable creates — so
 * every environment this agent holds generates the identical key, and the store therefore needs no per-entry
 * key field. That is asserted rather than assumed at every read, and the assert names what a second key means:
 * a peer instance, whose store is its own. */
static JSValue permission_key(JSContext *ctx)
{
    JSValue url = realm_top_level_creation_url(ctx), key;
    const char *u = JS_ToCString(ctx, url);
    UrlRecord rec;
    char *origin;

    JS_FreeValue(ctx, url);
    CHECK(u != NULL, "the top-level creation URL could not be read as a string — §5.1 step 5 has no origin to "
                     "generate a permission key from");
    url_record_init(&rec);
    if (!url_parse(&rec, u, strlen(u), NULL)) {
        /* §4.7's answer for input that is not a URL is the opaque origin, which is same origin with nothing —
           so it is its own key and every entry under it belongs to this environment alone. */
        url_record_free(&rec);
        JS_FreeCString(ctx, u);
        return JS_NewString(ctx, "null");
    }
    JS_FreeCString(ctx, u);
    origin = url_serialize_origin(&rec);
    url_record_free(&rec);
    CHECK(origin != NULL, "the top-level origin could not be serialized for §5.1 step 5's permission key");
    key = JS_NewString(ctx, origin);
    free(origin);
    return key;
}

/* THE ASSERTION IS THE WHOLE REASON THE KEY IS A FIELD AND NOT A PER-ENTRY ONE, so it re-generates the key at
   every store touch rather than trusting the sentence above. It is DEV-ONLY in its body as well as in its
   verdict: generating a key parses a URL, and a release build must not pay for a check it cannot act on. */
static void permission_assert_key(JSContext *ctx)
{
#if APICLIENT_DEV
    JSValue here, hold;
    const char *a, *b;
    bool same;

    DCHECK(!JS_IsUndefined(g_key), "§3.2's store was touched before any realm generated its permission key — "
                                   "the key comes from the ENVIRONMENT's top-level creation URL, which "
                                   "core/realm.h creates with the realm, so the first realm's install is the "
                                   "earliest moment it exists");
    here = permission_key(ctx);
    hold = JS_DupValue(ctx, g_key);
    a = JS_ToCString(ctx, here);
    b = JS_ToCString(ctx, hold);
    same = a && b && strcmp(a, b) == 0;
    if (a) JS_FreeCString(ctx, a);
    if (b) JS_FreeCString(ctx, b);
    JS_FreeValue(ctx, here);
    JS_FreeValue(ctx, hold);
    DCHECK(same, "§5.1 step 5 generated a permission key this agent's store is not keyed by — an instance is "
                 "one (browsing-context group, origin) and every realm in it inherits ONE top-level creation "
                 "URL, so two keys mean this read belongs to a PEER instance. BUILD the cross-instance store: "
                 "§3.2's entry carries its key, the read is posted to the instance holding that document "
                 "exactly as a cross-instance property read is, and this assert becomes the routing question");
#else
    (void)ctx;
#endif
}

/* THE KEY IS GENERATED PER REALM AND KEPT ONCE — which is where the ordering forces it and where the invariant
   is worth stating. §5.1 step 5 reads the ENVIRONMENT's top-level origin, and core/realm.h creates the
   environment WITH the realm, so no key exists at agent-declaration time at all; the first realm generates it
   and every later realm asserts it generated the same one, which is the assertion that says an instance is one
   origin. */
static void permission_store_install_realm(JSContext *ctx)
{
    if (JS_IsUndefined(g_key)) {
        g_key = permission_key(ctx);
        CHECK(!JS_IsException(g_key), "§5.1 step 5's permission key could not be allocated");
        return;
    }
    permission_assert_key(ctx);
}

/* ---- §3.2's store ---------------------------------------------------------------------------------------- */

int permission_store_get(JSContext *ctx, const PermissionDescriptor *d)
{
    JSValue v;
    const char *s;
    int state;

    DCHECK(g_ready, "§3.2's store was read before permission_store_init built it");
    DCHECK(d->feature >= 0 && d->feature < PF_N, "§3.2's store was asked about a feature §4's registry has no "
                                                 "row for — a descriptor is built from that registry and "
                                                 "nowhere else");
    permission_assert_key(ctx);
    v = JS_GetPropertyUint32(ctx, g_store, (uint32_t)PF_SLOT(d));
    if (JS_IsUndefined(v)) {
        JS_FreeValue(ctx, v);
        return -1;                         /* §3.2's "Return null" — no entry, which is not the same as prompt */
    }
    s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    state = permission_state_of(s);
    if (s) JS_FreeCString(ctx, s);
    DCHECK(state >= 0, "§3.2's store holds an entry whose state is not one of §3.1's three — the store's only "
                       "writer takes a PermissionState enum value, so a fourth value means something else "
                       "wrote it");
    return state;
}

void permission_store_set(JSContext *ctx, const PermissionDescriptor *d, int state)
{
    DCHECK(g_ready, "§3.2's store was written before permission_store_init built it");
    DCHECK(state >= 0 && state < PERMISSION_STATE_N,
           "§3.2's set-a-permission-store-entry was given something that is not a PermissionState");
    DCHECK(d->feature >= 0 && d->feature < PF_N,
           "§3.2's store was written for a feature §4's registry has no row for");
    permission_assert_key(ctx);
    /* §3.2: "If the user agent's permission store contains an entry whose descriptor is descriptor, and whose
       key is equal to key given descriptor, REPLACE that entry"; otherwise append. One slot per descriptor is
       both halves of that at once — the entry can appear "at most once in this list" by construction. */
    JS_SetPropertyUint32(ctx, g_store, (uint32_t)PF_SLOT(d), JS_NewString(ctx, permission_state_str(state)));

#if APICLIENT_DEV
    /* §4's PARTIAL ORDER, ASSERTED WHERE BOTH HALVES OF IT ARE KNOWN. "If descriptorA is stronger than
       descriptorB, then if descriptorA's permission state is granted, descriptorB's permission state must also
       be granted, and if descriptorB's permission state is denied, descriptorA's permission state must also be
       denied." Two store entries are two FACTS and can therefore contradict each other; §5.1's read DERIVES the
       implied one where only the sibling is present, so the one remaining way to reach a contradiction is to
       write it, and this is where that is caught. */
    if (PF_ASPECT[d->feature]) {
        PermissionDescriptor o;
        int other, strong_state, weak_state;
        bool this_is_strong = (d->aspect == (PF_STRONG[d->feature] != 0));

        o.feature = d->feature;
        o.aspect = !d->aspect;
        other = permission_store_get(ctx, &o);
        if (other >= 0) {
            strong_state = this_is_strong ? state : other;
            weak_state   = this_is_strong ? other : state;
            DCHECK(!(strong_state == PERMISSION_GRANTED && weak_state != PERMISSION_GRANTED),
                   "§4's partial order was broken by a store write: the STRONGER descriptor of a feature is "
                   "granted while the weaker one is not, and the standard makes that impossible");
            DCHECK(!(weak_state == PERMISSION_DENIED && strong_state != PERMISSION_DENIED),
                   "§4's partial order was broken by a store write: the WEAKER descriptor of a feature is "
                   "denied while the stronger one is not, and the standard makes that impossible");
        }
    }
#endif
}

/* ---- §5.1's read ------------------------------------------------------------------------------------------ */

int permission_feature_of(const char *name)
{
    int i;

    if (!name) return -1;
    for (i = 0; i < PF_N; i++)
        if (strcmp(name, PF_NAME[i]) == 0)
            return i;
    return -1;
}

const char *permission_feature_name(int feature)
{
    DCHECK(feature >= 0 && feature < PF_N, "a feature index §4's registry has no row for was asked for its name");
    return PF_NAME[feature];
}

const char *permission_feature_aspect(int feature)
{
    DCHECK(feature >= 0 && feature < PF_N, "a feature index §4's registry has no row for was asked for its "
                                           "aspect member");
    return PF_ASPECT[feature];
}

int permission_feature_default(int feature)
{
    DCHECK(feature >= 0 && feature < PF_N, "a feature index §4's registry has no row for was asked for its "
                                           "default permission state");
    return PF_DEFAULT[feature];
}

JSValue permission_unknown(JSContext *ctx, int feature)
{
    JSValue v;

    DCHECK(g_ready, "a feature's source was asked for before permission_store_init minted it");
    DCHECK(feature >= 0 && feature < PF_N, "a feature index §4's registry has no row for was asked for its source");
    v = JS_GetPropertyUint32(ctx, g_sources, (uint32_t)feature);
    DCHECK(!JS_IsUndefined(v), "§4's registry declares a feature the source record holds nothing for — the two "
                               "are built from the one X-list, so an empty slot means a row was added to the "
                               "list and not to the builder");
    return v;
}

/* §5.1 STEP 4, and the whole of what this engine can say about it. "If there exists a policy-controlled
 * feature for feature and settings' relevant global object has an associated Document ... If document is not
 * allowed to use feature, return denied."
 *   Every registered policy-controlled feature above has a DEFAULT ALLOWLIST of 'self', and this engine
 * narrows none of them: it parses no `Permissions-Policy` header and no `allow` attribute, so the default IS
 * the answer, exactly as core/html/focus.c computes the identical question for `focus-without-user-activation`.
 * A default allowlist of 'self' means a document is allowed to use the feature exactly while it is same origin
 * with the top-level traversable's active document — which is why an `<iframe src="https://other.example">`
 * without an `allow` attribute gets "denied" for geolocation and a same-origin one does not.
 *   THIS IS COMPUTED, NOT UNKNOWN. The document's position in the tree and its origin are facts this engine
 * holds, so there is no ignorance here and nothing to fork over. */
static bool permission_allowed_to_use(JSContext *ctx, int feature)
{
    JSValueConst self = document_window_proxy(ctx);
    JSValue top;
    bool same;

    if (!PF_POLICY[feature])
        return true;                       /* not a policy-controlled feature: step 4's condition is false */
    DCHECK(window_proxy_is(self), "§5.1 step 4 was asked of a realm with no navigable — every Window this agent "
                                  "builds has one, and step 4 reads the document's position in the tree");
    top = window_proxy_top_navigable(ctx, self);
    same = JS_IsObject(top) && window_proxy_same_origin_of(top);
    JS_FreeValue(ctx, top);
    return same;
}

JSValue permission_state(JSContext *ctx, const PermissionDescriptor *d)
{
    int entry, other;
    PermissionDescriptor o;

    DCHECK(g_ready, "§5.1 was asked for a permission state before permission_store_init built the model");
    DCHECK(d->feature >= 0 && d->feature < PF_N,
           "§5.1 was asked about a feature §4's registry has no row for — §6.2.1 step 4 rejects an unsupported "
           "name before a descriptor exists, so reaching here means a descriptor was built from something "
           "other than the registry");
    DCHECK(!d->aspect || PF_ASPECT[d->feature] != NULL,
           "a descriptor carries an ASPECT for a feature whose permission descriptor type is the default one — "
           "the aspect bit is the value of the member the registry names, and a feature with no member has no "
           "second descriptor");

    /* STEP 2: "If settings is a non-secure context, return denied." */
    if (!secure_context_is(ctx))
        return JS_NewString(ctx, permission_state_str(PERMISSION_DENIED));
    /* STEP 4. */
    if (!permission_allowed_to_use(ctx, d->feature))
        return JS_NewString(ctx, permission_state_str(PERMISSION_DENIED));
    /* STEPS 5-7: generate the key, get the entry, and return its state where there is one. */
    entry = permission_store_get(ctx, d);
    if (entry >= 0)
        return JS_NewString(ctx, permission_state_str(entry));
    /* §4's PARTIAL ORDER, DISCHARGED OVER WHAT IS KNOWN. This descriptor has no entry of its own, but its
       sibling may — and the order then DETERMINES this one's state rather than leaving it to the user's
       intent: a granted stronger descriptor forces the weaker to granted, and a denied weaker one forces the
       stronger to denied. Both are the standard's own implications, and both are facts once the sibling's
       entry is a fact, so neither is asked of the unknown. */
    if (PF_ASPECT[d->feature]) {
        bool this_is_strong = (d->aspect == (PF_STRONG[d->feature] != 0));

        o.feature = d->feature;
        o.aspect = !d->aspect;
        other = permission_store_get(ctx, &o);
        if (!this_is_strong && other == PERMISSION_GRANTED)
            return JS_NewString(ctx, permission_state_str(PERMISSION_GRANTED));
        if (this_is_strong && other == PERMISSION_DENIED)
            return JS_NewString(ctx, permission_state_str(PERMISSION_DENIED));
    }
    /* STEP 8: "Return the PermissionState enum value that represents the permission state of feature." This is
       the user's decision, and nothing in this engine has observed it — see permission_store.h. The value is
       the FEATURE's, not this descriptor's, which is what makes §4's order hold between the two aspects with
       nothing left to enforce. */
    return permission_unknown(ctx, d->feature);
}

/* THE CHAIN'S TWO QUESTIONS, as the caller's phase byte spells them. Zero is "ask the first", which is what a
   zeroed byte already reads as and what every answered chain leaves behind. */
enum { PS_ASK_DEFAULT = 0, PS_ASK_WHICH };
#define PS_OP_DEFAULT "Permissions §5.1 step 8 (is the user's decision this feature's default permission state)"
#define PS_OP_WHICH   "Permissions §5.1 step 8 (which of the two non-default permission states it is)"

int permission_state_run(JSContext *ctx, JSStepHdr *h, uint8_t *phase, const PermissionDescriptor *d, int *out)
{
    JSValue v = permission_state(ctx, d);
    int order[PERMISSION_STATE_N], arm = 0, rc, i, n = 0, def;

    if (!concolic_is(v)) {
        const char *s = JS_ToCString(ctx, v);
        int state = permission_state_of(s);

        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, v);
        DCHECK(state >= 0, "§5.1 answered with a string that is not a PermissionState — every concrete answer "
                           "it produces comes from permission_state_str");
        DCHECK(*phase == PS_ASK_DEFAULT,
               "§5.1's chain resumed mid-question over a state that is no longer unknown — the second question "
               "is owed to a flow standing inside the first's false arm, and a store entry cannot deliver it");
        *out = state;
        return 0;
    }
    /* THE THREE STATES, ROTATED SO THE FEATURE'S DEFAULT IS FIRST: `order[0]` is what the first question's
       true arm answers, and the other two are what the second question chooses between, in the enum's order. */
    def = PF_DEFAULT[d->feature];
    order[n++] = def;
    for (i = 0; i < PERMISSION_STATE_N; i++)
        if (i != def)
            order[n++] = i;
    DCHECK(n == PERMISSION_STATE_N, "the outcome rotation lost a permission state — every one of §3.1's three "
                                    "is a feasible answer and an arm that is not offered is a world deleted");
    if (*phase == PS_ASK_DEFAULT) {
        /* THE OPERAND IS HELD ACROSS BOTH ASKS and released once, because the second question is asked over the
           same value in the same invocation. step_fork_run keeps a BORROWED pointer to it on the header, which
           the driver reads after this function has returned — that stays valid on either path, because the
           g_sources record owns this source for the life of the agent. */
        rc = step_fork_run(ctx, h, v, PS_OP_DEFAULT, 2, &arm);
        if (rc) { JS_FreeValue(ctx, v); return rc; }
        if (arm == 0) {                    /* the user has not decided: the feature's default state */
            JS_FreeValue(ctx, v);
            *out = order[0];
            return 0;
        }
        *phase = PS_ASK_WHICH;
    }
    DCHECK(*phase == PS_ASK_WHICH, "§5.1's chain resumed in a phase it never parks in");
    rc = step_fork_run(ctx, h, v, PS_OP_WHICH, 2, &arm);
    JS_FreeValue(ctx, v);
    if (rc) return rc;
    *phase = PS_ASK_DEFAULT;               /* the chain is finished; the byte is ready for the next question */
    *out = order[1 + arm];
    return 0;
}

/* ---- the agent's declaration ------------------------------------------------------------------------------ */

void permission_store_init(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    int i;

    DCHECK(!g_ready, "permission_store_init ran twice — §3.2's store is the AGENT's, and one WASM instance is "
                     "one document");
    g_rt = rt;
    g_store = JS_NewArray(ctx);
    CHECK(!JS_IsException(g_store), "§3.2's permission store could not be allocated");
    g_sources = JS_NewArray(ctx);
    CHECK(!JS_IsException(g_sources), "the permission sources record could not be allocated");
    /* THE KEY CANNOT BE GENERATED HERE. §5.1 step 5 reads the environment's TOP-LEVEL ORIGIN, and core/realm.h
       creates the environment with the REALM — this declaration runs before the agent's own first realm has
       one, so the key is generated by the realm install below and asserted equal for every realm after it. */
    g_key = JS_UNDEFINED;
    /* §5.1 STEP 8's VALUE FOR EVERY FEATURE, MINTED WITH THE AGENT. Built here and not lazily on the first
       query for the reason core/frame/navigator.c builds its record with the realm: a value minted on first
       read is minted inside whichever flow happened to ask first, and that flow's baseline becomes every other
       flow's.
       concolic_source_wrap IS THE SEAM. A host with no source overlay — a conformance run — gets back the bare
       default-state string, so §5.1 answers the standard's own "prompt", nothing forks, and the WPT oracle
       sees a user agent that has granted nothing. A host that IS exploring gets the source, whose EXAMPLE is
       that same string, so `state.length` and `state.slice(0,3)` still compute real values while every branch
       on it forks. */
    for (i = 0; i < PF_N; i++) {
        char shape[64], src[64];
        JSValue v;

        DCHECK(strlen(PF_NAME[i]) + 24 < sizeof(shape), "a powerful feature name longer than any in the registry");
        snprintf(shape, sizeof(shape), "{permission %s}", PF_NAME[i]);
        snprintf(src, sizeof(src), "navigator.permissions.%s", PF_NAME[i]);
        v = concolic_source_wrap(ctx, shape, src, JS_NewString(ctx, permission_state_str(PF_DEFAULT[i])));
        CHECK(!JS_IsException(v), "a powerful feature's permission state source could not be minted");
        JS_SetPropertyUint32(ctx, g_sources, (uint32_t)i, v);
    }
    g_ready = 1;
    realm_declare_intrinsic(permission_store_install_realm);
}

void permission_store_free(void)
{
    if (!g_ready)
        return;
    DCHECK(g_rt != NULL, "§3.2's store was built without recording the runtime that owns its values");
    JS_FreeValueRT(g_rt, g_store);
    JS_FreeValueRT(g_rt, g_sources);
    JS_FreeValueRT(g_rt, g_key);
    g_store = g_sources = g_key = JS_UNDEFINED;
    g_ready = 0;
    g_rt = NULL;
}
