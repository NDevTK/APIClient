/* PERMISSIONS §3's MODEL AND §5.1's READ. See permission_store.h for why step 8 is a source and steps 2, 4
   and 7 are not. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/url/origin.h"
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
 *     type is the default `PermissionDescriptor`. Most are a `boolean X = false`; File System Access's `mode`
 *     is a two-valued ENUMERATION, which is the same one bit reached through Web IDL §3.2.18's conversion rather than
 *     through ToBoolean, and ASPECT_VALUES is what says which of the two a row is.
 *   ASPECT_VALUES — that enumeration's values, first the IDL's default and second the one that sets the bit;
 *     NULL for a boolean member. Under ToBoolean `{mode:"read"}` and `{mode:"readwrite"}` are one descriptor,
 *     so this column is what keeps the read and the readwrite worlds apart.
 *   SUBJECT — the `required` member identifying WHICH INSTANCE of the feature a descriptor is about, or NULL
 *     where a descriptor is a fact about the origin alone (which every row but one is). Its value's TYPE is
 *     the feature's own, so the brand test is registered by the feature's component rather than named here.
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
/* THE TWO COLUMNS THE FIRST NON-BOOLEAN DESCRIPTOR TYPE ADDED. ASPECT_VALUES is the enumeration a feature's
   aspect member is, NULL where it is a `boolean X = false`; SUBJECT is the `required` member naming WHICH
   instance of the feature the descriptor is about, NULL where the descriptor is a fact about the origin alone.
   Both are the same kind of statement as ASPECT itself — data read off §4's registry entry — which is why they
   are columns and not a branch at the conversion. */
static const char *const PF_MODE_VALUES[] = { "read", "readwrite", NULL };

#define PERMISSION_FEATURES(X)                                                                                 \
    /* Geolocation API §3.4: "Geolocation is a DEFAULT powerful feature identified by the name geolocation". */\
    X(GEOLOCATION,        "geolocation",          1, NULL,                 0, PERMISSION_PROMPT, NULL, NULL)               \
    /* Notifications §2.2: "a powerful feature which is identified by the name notifications". */              \
    X(NOTIFICATIONS,      "notifications",        0, NULL,                 0, PERMISSION_PROMPT, NULL, NULL)               \
    /* Push API: `dictionary PushPermissionDescriptor : PermissionDescriptor { boolean userVisibleOnly =       \
       false; }`, and `{userVisibleOnly:false}` is stronger than `{userVisibleOnly:true}`. */                   \
    X(PUSH,               "push",                 0, "userVisibleOnly",    0, PERMISSION_PROMPT, NULL, NULL)               \
    /* Web MIDI: `MidiPermissionDescriptor { boolean sysex = false; }`; sysex:true is the stronger. */         \
    X(MIDI,               "midi",                 1, "sysex",              1, PERMISSION_PROMPT, NULL, NULL)               \
    /* Media Capture §: two powerful features. Only `camera` carries an aspect —                               \
       `CameraDevicePermissionDescriptor { boolean panTiltZoom = false; }`, panTiltZoom:true the stronger. */   \
    X(CAMERA,             "camera",               1, "panTiltZoom",        1, PERMISSION_PROMPT, NULL, NULL)               \
    X(MICROPHONE,         "microphone",           1, NULL,                 0, PERMISSION_PROMPT, NULL, NULL)               \
    /* Storage §5: the "persistent-storage" powerful feature, algorithms defaulted except for a permission     \
       state that must agree across every environment of an origin (which the one agent-wide store gives) and  \
       a revocation algorithm that demotes the default bucket. */                                              \
    X(PERSISTENT_STORAGE, "persistent-storage",   0, NULL,                 0, PERMISSION_PROMPT, NULL, NULL)               \
    /* Screen Wake Lock: the "screen-wake-lock" powerful feature, policy-controlled with 'self'. */            \
    X(SCREEN_WAKE_LOCK,   "screen-wake-lock",     1, NULL,                 0, PERMISSION_PROMPT, NULL, NULL)               \
    /* Window Management §3.7: "a default powerful feature identified by the name window-management". */       \
    X(WINDOW_MANAGEMENT,  "window-management",    1, NULL,                 0, PERMISSION_PROMPT, NULL, NULL)               \
    /* Local Font Access: "a default powerful feature that is identified by the name local-fonts". */          \
    X(LOCAL_FONTS,        "local-fonts",          1, NULL,                 0, PERMISSION_PROMPT, NULL, NULL)               \
    /* Idle Detection: "the idle-detection permission is a default powerful feature". */                       \
    X(IDLE_DETECTION,     "idle-detection",       1, NULL,                 0, PERMISSION_PROMPT, NULL, NULL)               \
    /* Background Sync: "a default powerful feature that is identified by the name background-sync". */        \
    X(BACKGROUND_SYNC,    "background-sync",      0, NULL,                 0, PERMISSION_PROMPT, NULL, NULL)               \
    /* Generic Sensor: a sensor type's SENSOR PERMISSION NAMES are powerful feature names, and each concrete   \
       sensor spec names its own. */                                                                           \
    X(ACCELEROMETER,      "accelerometer",        1, NULL,                 0, PERMISSION_PROMPT, NULL, NULL)               \
    X(GYROSCOPE,          "gyroscope",            1, NULL,                 0, PERMISSION_PROMPT, NULL, NULL)               \
    X(MAGNETOMETER,       "magnetometer",         1, NULL,                 0, PERMISSION_PROMPT, NULL, NULL)               \
    X(AMBIENT_LIGHT,      "ambient-light-sensor", 1, NULL,                 0, PERMISSION_PROMPT, NULL, NULL)               \
    /* Clipboard API §9: `ClipboardPermissionDescriptor { boolean allowWithoutGesture = false; }`, and         \
       `{allowWithoutGesture:true}` is stronger than `{allowWithoutGesture:false}`. */                          \
    X(CLIPBOARD_WRITE,    "clipboard-write",      0, "allowWithoutGesture", 1, PERMISSION_PROMPT, NULL, NULL)                                                                                               \
    /* File System Access §2.2: the "file-system" powerful feature, and the first registered one whose        \
       permission descriptor type declares a member that is neither a name nor a boolean —                     \
       `FileSystemPermissionDescriptor : PermissionDescriptor { required FileSystemHandle handle;              \
       FileSystemPermissionMode mode = "read"; }`. `mode` is the ASPECT (a two-valued enumeration rather than a \
       boolean, which is the same one bit through §3.2.18's conversion instead of ToBoolean) and `handle` is    \
       the SUBJECT. §4's PARTIAL ORDER is the feature's own fourth permission state constraint read as an       \
       order: "if desc['mode'] is readwrite ... if read state is not granted, this descriptor's permission      \
       state must be equal to read state" says a granted readwrite forces read granted and a denied read        \
       forces readwrite denied, which is exactly what STRONG=1 (the readwrite bit) states here. It is NOT a     \
       policy-controlled feature: File System Access defines no Permissions-Policy feature and the Permissions  \
       Policy registry lists none, so §5.1 step 4's outer condition is false for it. */                         \
    X(FILE_SYSTEM,        "file-system",          0, "mode",              1, PERMISSION_PROMPT,                \
      PF_MODE_VALUES, "handle")

#define PF_ENUM_ONE(id, name, policy, aspect, strong, def, values, subject) PF_##id,
#define PF_NAME_ONE(id, name, policy, aspect, strong, def, values, subject) name,
#define PF_POLICY_ONE(id, name, policy, aspect, strong, def, values, subject) policy,
#define PF_ASPECT_ONE(id, name, policy, aspect, strong, def, values, subject) aspect,
#define PF_STRONG_ONE(id, name, policy, aspect, strong, def, values, subject) strong,
#define PF_DEFAULT_ONE(id, name, policy, aspect, strong, def, values, subject) def,
#define PF_VALUES_ONE(id, name, policy, aspect, strong, def, values, subject) values,
#define PF_SUBJECT_ONE(id, name, policy, aspect, strong, def, values, subject) subject,

enum { PERMISSION_FEATURES(PF_ENUM_ONE) PF_N };
static const char *const PF_NAME[]   = { PERMISSION_FEATURES(PF_NAME_ONE) };
static const int         PF_POLICY[] = { PERMISSION_FEATURES(PF_POLICY_ONE) };
static const char *const PF_ASPECT[] = { PERMISSION_FEATURES(PF_ASPECT_ONE) };
static const int         PF_STRONG[] = { PERMISSION_FEATURES(PF_STRONG_ONE) };
static const int         PF_DEFAULT[] = { PERMISSION_FEATURES(PF_DEFAULT_ONE) };
static const char *const *const PF_VALUES[] = { PERMISSION_FEATURES(PF_VALUES_ONE) };
static const char *const PF_SUBJECT[] = { PERMISSION_FEATURES(PF_SUBJECT_ONE) };
/* THE FEATURE'S OWN ALGORITHMS, registered by the component that owns it — §4's registry is a table of them
   and these two are the entries a feature whose descriptor names OBJECTS has to fill in. NULL is the default
   §4 states for each: no subject member to brand, and "no constraints beyond the user's intent". */
static PermissionSubjectFn    PF_SUBJECT_FN[PF_N];
static PermissionConstraintFn PF_CONSTRAINT_FN[PF_N];

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
/* §5.1 STEP 5's PERMISSION KEY, WHICH IS AN ORIGIN AND IS HELD AS ONE. The default permission key generation
   algorithm "takes an origin origin and an origin embedded origin" and its whole body is "Return origin",
   where step 5 passes "settings's TOP-LEVEL ORIGIN and settings's origin" — so the key IS §7.1.1's record
   (core/url/origin.h) and it is the top-level one, which is permission delegation and the standard says so:
   "Most powerful features grant permission to the top-level origin and delegate access to the requesting
   document via Permissions Policy."
   THE KEY THIS STORE IS KEYED BY is the one the first read generated; every read after it generates its own
   and compares, which is §3.2's own shape ("an entry whose descriptor is descriptor, and whose key is EQUAL
   TO key given descriptor") with one entry per descriptor because this agent has one key. A record here is
   agent-lifetime and immutable, so which environment happened to read first cannot change what is held. */
static const Origin *g_key;
static int     g_ready;
static JSRuntime *g_rt;

/* §5.1 STEP 5, GENERATED WHERE THE STANDARD GENERATES IT — at the read, from the environment doing the
 * reading. It reads §8.1.3.1's TOP-LEVEL ORIGIN, which is a FIELD OF THE ENVIRONMENT and not a derivation of
 * its top-level creation URL; HTML says exactly that at the field ("This is distinct from the top-level
 * creation URL's origin when sandboxing, workers, and worklets are involved"), and this component used to run
 * §4.7 over the URL instead. That is wrong twice over and both showed: §4.7 MINTS a new opaque origin for an
 * address that has no tuple, and the URL is not the environment's origin in the first place — §7.3.2.1 gives
 * every top-level browsing context the URL `about:blank` while giving it the initial Document's ORIGIN, so a
 * popup and its opener are one origin with two URLs.
 *   The environment's copy of the field lives on its NAVIGABLE (core/frame/window_proxy.h), because a
 * navigable outlives the documents in it and is created before their realms are. */
static const Origin *permission_key(JSContext *ctx)
{
    JSValueConst nav = document_window_proxy(ctx);

    DCHECK(window_proxy_is(nav),
           "§5.1 step 5 was asked of a realm with no navigable — every Window this agent builds has one, and "
           "§8.1.3.1's top-level origin is a field of the environment that navigable's documents are created "
           "with");
    return window_proxy_top_level_origin(nav);
}

/* §3.2's "whose key is EQUAL TO key given descriptor", which for every feature registered above is the DEFAULT
   permission key comparison algorithm — "Return key1 is same origin with key2" — and therefore §7.1.1, whose
   step 1 compares IDENTITY. It is asked at every store touch rather than once per realm, because that is when
   §5.1 generates a key at all; and the store holds ONE, so what this asserts is that this agent has exactly
   one top-level origin to be keyed by. It NAMES what a second one means, which is not a peer instance: an
   instance is (browsing-context group, origin) and a browsing-context group can hold several top-level
   traversables, so a cross-origin frame's instance that also opens a popup legitimately holds two. */
static void permission_key_assert(JSContext *ctx)
{
    const Origin *key = permission_key(ctx);

    if (!g_key)
        g_key = key;
    DCHECK(origin_same(g_key, key),
           "§5.1 step 5 generated a permission key this agent's store is not keyed by. §3.2's store is a LIST "
           "of entries each carrying its own key, and this one collapses that to a single key per descriptor "
           "because one instance has one origin — but §8.1.3.1's TOP-LEVEL origin is a different fact from "
           "this agent's own, and an instance holding both a NESTED document (keyed by its embedder's top) "
           "and a top-level traversable it opened (keyed by its own) holds two of them. BUILD the keyed "
           "store: §3.2's entry carries its key, get/set compare with the feature's permission key comparison "
           "algorithm, and this assert becomes that lookup");
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
    permission_key_assert(ctx);
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

/* §3.2's store operations both take the descriptor a caller HAS and write or read the one §4's constraints
   make it equal to — defined with §5.1's read below and used by both, because an entry written
   under a non-canonical descriptor is an entry §5.1 would never read back. */
static int permission_canonical(JSContext *ctx, PermissionDescriptor *d);

void permission_store_set(JSContext *ctx, const PermissionDescriptor *din, int state)
{
    PermissionDescriptor c = *din;
    const PermissionDescriptor *d = &c;
    int fixed;

    DCHECK(g_ready, "§3.2's store was written before permission_store_init built it");
    DCHECK(state >= 0 && state < PERMISSION_STATE_N,
           "§3.2's set-a-permission-store-entry was given something that is not a PermissionState");
    DCHECK(d->feature >= 0 && d->feature < PF_N,
           "§3.2's store was written for a feature §4's registry has no row for");
    fixed = permission_canonical(ctx, &c);
    DCHECK(fixed < 0, "§3.2's set-a-permission-store-entry was given a descriptor whose feature's permission "
                      "state CONSTRAINTS already fix its state — an entry for it could never be read back, "
                      "since §5.1 answers the constraint ahead of the store, so writing one is a decision "
                      "about a world the standard says cannot exist");
    (void)fixed;
    permission_key_assert(ctx);
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
        o.subject = d->subject;   /* the SIBLING descriptor is the same instance with the other aspect value */
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

const char *const *permission_feature_aspect_values(int feature)
{
    DCHECK(feature >= 0 && feature < PF_N, "a feature index §4's registry has no row for was asked for its "
                                           "aspect member's enumeration");
    DCHECK(!PF_VALUES[feature] || PF_ASPECT[feature],
           "§4's registry names an aspect ENUMERATION for a feature that declares no aspect member — the values "
           "are that member's TYPE, so a row carrying them and no member is half a descriptor type");
#if APICLIENT_DEV
    if (PF_VALUES[feature]) {
        int n = 0;

        while (PF_VALUES[feature][n]) n++;
        DCHECK(n == 2, "§4's registry names an aspect enumeration that is not two-valued — the aspect is ONE "
                       "BIT, so an enumeration with a third value is a descriptor type this component cannot "
                       "index and the registry has to say how it maps onto the bit");
    }
#endif
    return PF_VALUES[feature];
}

const char *permission_feature_subject(int feature)
{
    DCHECK(feature >= 0 && feature < PF_N, "a feature index §4's registry has no row for was asked for its "
                                           "subject member");
    return PF_SUBJECT[feature];
}

void permission_subject_declare(int feature, PermissionSubjectFn is)
{
    DCHECK(feature >= 0 && feature < PF_N, "a brand test was declared for a feature §4's registry has no row for");
    DCHECK(PF_SUBJECT[feature] != NULL,
           "a brand test was declared for a feature whose registry row names no SUBJECT member — the test is "
           "that member's type, so a feature with no member has nothing for it to brand");
    DCHECK(PF_SUBJECT_FN[feature] == NULL,
           "a feature's subject brand test was declared twice — §4's registry entry is the AGENT's, and a "
           "second declaration means two components both claim to own the feature");
    PF_SUBJECT_FN[feature] = is;
}

bool permission_subject_is(int feature, JSValueConst v)
{
    DCHECK(feature >= 0 && feature < PF_N, "a value was branded against a feature §4's registry has no row for");
    DCHECK(PF_SUBJECT_FN[feature] != NULL,
           "§6.2.1 step 5 reached a feature's SUBJECT member with no brand test declared for it — the test is "
           "declared once per agent by the component that owns the interface, so reaching here means that "
           "component's own `_init` did not run before the member it guards became reachable");
    return PF_SUBJECT_FN[feature](v);
}

void permission_constraints_declare(int feature, PermissionConstraintFn fn)
{
    DCHECK(feature >= 0 && feature < PF_N,
           "permission state constraints were declared for a feature §4's registry has no row for");
    DCHECK(PF_CONSTRAINT_FN[feature] == NULL,
           "a feature's permission state constraints were declared twice — §4 gives a feature ONE such column, "
           "and two would be two answers to a question the standard makes single-valued");
    PF_CONSTRAINT_FN[feature] = fn;
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
 *   THE FEATURES ABOVE ARE NOT IN THIS BUILD'S PERMISSIONS POLICY §4.1 SUPPORTED-FEATURE SET, which is why
 * this file still computes step 4 for itself rather than asking core/permissions_policy/permissions_policy.h.
 * That set is HTML §2.2 "Policy-controlled features"' three names and nothing else; `geolocation`, `camera`,
 * `midi` and the rest are policy-controlled features defined by their OWN specifications, and §4.8 makes a
 * feature's default allowlist normative — so a row cannot be added to that set until its allowlist is read
 * from the specification that defines it. The component's question is an enum, so this file cannot spell one
 * of them by accident.
 *   WHAT THIS COMPUTES IS NARROWER THAN §9.7 AND IS SAID SO PLAINLY: "same origin with the top-level
 * traversable's active document". Permissions Policy §9.7 step 7 compares against "container's node document's
 * origin" and its steps 2-3 chain that comparison through EVERY container, which is a strictly stronger
 * condition — a document nested top → cross-origin → same-origin-as-top satisfies this file's test and is
 * `Disabled` under §9.7. The two agree for every document whose whole container chain is same origin, which is
 * every frame on a single-origin page and is why an `<iframe src="https://other.example">` gets "denied" here.
 *   'self' IS §7.1.1's SAME ORIGIN AND NOTHING MORE, so a top-level document is always allowed: it is its own
 * top, and an origin is same origin with itself whether it is a tuple or opaque (step 1 compares identity).
 * That answer changed when an origin became a record — a top-level document with an opaque origin used to be
 * refused here, by a serialized comparison that could not express step 1 — and the change is the standard's:
 * the feature this file gates is not the one an opaque origin is denied, which is Storage's key.
 *   THIS IS COMPUTED, NOT UNKNOWN. The document's position in the tree and its origin are facts this engine
 * holds, so there is no ignorance here and nothing to fork over. */
static bool permission_allowed_to_use(JSContext *ctx, int feature)
{
    if (!PF_POLICY[feature])
        return true;                       /* not a policy-controlled feature: step 4's condition is false */
    DCHECK(window_proxy_is(document_window_proxy(ctx)),
           "§5.1 step 4 was asked of a realm with no navigable — every Window this agent builds has one, and "
           "step 4 reads the document's position in the tree");
    return window_proxy_same_origin_with_top(ctx);
}

/* §4's PERMISSION STATE CONSTRAINTS, APPLIED BEFORE §5.1's OWN STEPS. A constraint that FIXES the state is
 * answered here ("must ALWAYS be granted" admits no earlier step overriding it, and step 2's non-secure context
 * is unreachable for the one feature that has such a constraint — every interface File System Access declares
 * is [SecureContext], so a non-secure realm holds no handle to build the descriptor from).
 *   A constraint that says a descriptor's state "must be EQUAL to" another descriptor's REWRITES `*d` and is
 * asked again, because the constraint the rewrite lands on may itself rewrite: File System Access §2.2's third
 * constraint walks an entry to its parent, and a path is a list, so the walk is a LOOP over strictly shorter
 * paths and never C recursion. `*d` is left holding the CANONICAL descriptor — the one whose store entry is
 * the answer for every descriptor constrained to equal it, which is why §3.2's key needs no subject.
 *   Returns a PERMISSION_* value the constraint fixed, or -1 to continue with the rewritten descriptor. */
static int permission_canonical(JSContext *ctx, PermissionDescriptor *d)
{
    int fixed = -1;

    /* THE BOUNDS ARE A `CHECK` AND NOT A DCHECK, and this is the one place in this file where that is true:
       every other assert here reports a wrong ANSWER, and this one stands in front of an INDEX into the
       registry's function tables. A feature naming no row would read a function pointer past the end of the
       table and CALL it, which is a data-integrity failure that must not proceed in a release build either. */
    CHECK(d->feature >= 0 && d->feature < PF_N,
          "§4's permission state constraints were asked about a feature the registry has no row for");
    while (PF_CONSTRAINT_FN[d->feature]) {
        PermissionDescriptor out = *d;

        if (!PF_CONSTRAINT_FN[d->feature](ctx, d, &out, &fixed))
            return -1;                     /* no constraint binds this descriptor: §5.1's own steps decide */
        if (fixed >= 0) {
            DCHECK(fixed < PERMISSION_STATE_N,
                   "a feature's permission state constraints fixed a value that is not a PermissionState");
            return fixed;
        }
        DCHECK(out.feature == d->feature,
               "a feature's permission state constraints rewrote a descriptor to a DIFFERENT feature — §4's "
               "constraints are stated over descriptors of the SAME feature, and a cross-feature rewrite would "
               "read one feature's store entry as another's");
        *d = out;
    }
    return -1;
}

JSValue permission_state(JSContext *ctx, const PermissionDescriptor *din)
{
    PermissionDescriptor c = *din, o;
    const PermissionDescriptor *d = &c;
    int entry, other, fixed;

    DCHECK(g_ready, "§5.1 was asked for a permission state before permission_store_init built the model");
    DCHECK(d->feature >= 0 && d->feature < PF_N,
           "§5.1 was asked about a feature §4's registry has no row for — §6.2.1 step 4 rejects an unsupported "
           "name before a descriptor exists, so reaching here means a descriptor was built from something "
           "other than the registry");
    DCHECK(!d->aspect || PF_ASPECT[d->feature] != NULL,
           "a descriptor carries an ASPECT for a feature whose permission descriptor type is the default one — "
           "the aspect bit is the value of the member the registry names, and a feature with no member has no "
           "second descriptor");
    DCHECK(PF_SUBJECT[d->feature] == NULL || !JS_IsUndefined(d->subject),
           "a descriptor for a feature whose permission descriptor type declares a REQUIRED subject member "
           "carries none — §6.2.1 step 5's conversion makes an absent required member a TypeError before a "
           "descriptor exists, so reaching here means a descriptor was built without reading it");

    /* §4's PERMISSION STATE CONSTRAINTS, which bind before every step below — see above. */
    fixed = permission_canonical(ctx, &c);
    if (fixed >= 0)
        return JS_NewString(ctx, permission_state_str(fixed));

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
        o.subject = d->subject;
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
   zeroed byte already reads as and what every answered chain leaves behind.
     PS_ASK_N IS THE WHOLE OF THE SPACE THIS CHAIN OCCUPIES while it is outstanding, and it is exported to §5.2
   below rather than left implicit: an algorithm that DELEGATES to this one shares the caller's one byte with
   it, so its own phases have to start where these stop. */
enum { PS_ASK_DEFAULT = 0, PS_ASK_WHICH, PS_ASK_N };
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
        rc = step_fork_run(ctx, h, v, PS_OP_DEFAULT, 2, JS_OUTCOME_REAL_UNSTATED, &arm);
        if (rc) { JS_FreeValue(ctx, v); return rc; }
        if (arm == 0) {                    /* the user has not decided: the feature's default state */
            JS_FreeValue(ctx, v);
            *out = order[0];
            return 0;
        }
        *phase = PS_ASK_WHICH;
    }
    DCHECK(*phase == PS_ASK_WHICH, "§5.1's chain resumed in a phase it never parks in");
    rc = step_fork_run(ctx, h, v, PS_OP_WHICH, 2, JS_OUTCOME_REAL_UNSTATED, &arm);
    JS_FreeValue(ctx, v);
    if (rc) return rc;
    *phase = PS_ASK_DEFAULT;               /* the chain is finished; the byte is ready for the next question */
    *out = order[1 + arm];
    return 0;
}

/* ---- §5.2's REQUEST -----------------------------------------------------------------------------------------
 *
 * See permission_store.h for the algorithm and for why step 3 is a fork and step 7 is what ends it. The caller's
 * one byte carries BOTH chains — §5.1's (itself two questions) and then step 3's own — because which of them a
 * parked flow is at cannot live in a C local. They are not "never outstanding at the same moment": step 1 IS
 * §5.1's chain, so while that chain is parked this algorithm is parked inside it, which is exactly why the two
 * cannot share a value. */
/* §5.2's OWN QUESTION IS NUMBERED ABOVE §5.1's WHOLE PHASE SPACE, and that is not tidiness — a shared value
 * space here makes two different outstanding questions the same byte. §5.1's chain parks at PS_ASK_WHICH while
 * its second question is in flight; numbering step 3's express-permission question 1 as well made that value
 * mean both, and the re-entry then read "§5.1 is parked at its second question" as "§5.2 is at its own". Three
 * things follow from that one re-entry and every one of them is silent:
 *   — step 1's chain is ABANDONED mid-question, so the state §5.1 was resolving is never produced;
 *   — the answer the driver is holding for §5.1's "which of the two non-default states" is consumed by step 3's
 *     ask, which is a real arm, in range, and recorded in the flow's decision vector under the OTHER question's
 *     key — one question's world filed as another's;
 *   — step 2 never runs, so a permission this flow's world says the user already granted or denied is asked
 *     for again and OVERWRITTEN by step 7's store write.
 * The byte holds ONE chain's phase at a time, §5.1 leaves it at zero when it answers, and a delegating chain's
 * phases therefore begin at PS_ASK_N. quickjs-step.h's fork_ask_key is what now crashes on the general case. */
enum { PR_ASK_EXPRESS = PS_ASK_N };
#define PR_OP_EXPRESS "Permissions §5.2 step 3 (ask the user for express permission for the calling algorithm " \
                      "to use the powerful feature described by descriptor)"

int permission_request_run(JSContext *ctx, JSStepHdr *h, uint8_t *phase, const PermissionDescriptor *d, int *out)
{
    int rc, arm = 0, current;

    DCHECK(g_ready, "§5.2 was asked to request a permission before permission_store_init built the model");
    DCHECK(*phase < PS_ASK_N || *phase == PR_ASK_EXPRESS,
           "§5.2 was re-entered on a phase neither it nor §5.1's chain parks in — the byte is the CALLER's and "
           "a stage that holds this request and any other one would hand this algorithm the other's phase");
    /* §5.1's CHAIN STILL HOLDS THE BYTE while any of its own phases is the value in it. */
    if (*phase < PS_ASK_N) {
        /* STEP 1: "Let current state be the descriptor's permission state." The read may itself FORK — the
           user's decision is unknown until something has learned it — and §5.1's chain owns the phase byte
           while it does. */
        rc = permission_state_run(ctx, h, phase, d, &current);
        if (rc) return rc;
        DCHECK(*phase == PS_ASK_DEFAULT,
               "§5.1's chain delivered an answer without releasing the caller's byte — an answered chain leaves "
               "it at zero, so a byte still holding one of that chain's phases means a question is outstanding "
               "that this algorithm is about to step over");
        /* STEP 2: "If current state is not prompt, return current state and abort these steps." A decision
           already taken is not asked again, which is what makes a second requestPermission() in the same flow
           answer the first one's arm rather than forking a world that contradicts it. */
        if (current != PERMISSION_PROMPT) {
            *out = current;
            return 0;
        }
        *phase = PR_ASK_EXPRESS;
    }
    DCHECK(*phase == PR_ASK_EXPRESS, "§5.2's chain resumed in a phase it never parks in");
    {
        /* STEPS 3-4. The operand is the FEATURE's own source — the same value §5.1 step 8 answers with — so a
           flow that has already decided this feature's state through one door answers consistently at the
           other rather than forking a second, independent world over one user's one decision. */
        JSValue over = permission_unknown(ctx, d->feature);

        if (!concolic_is(over)) {
            /* A HOST WITH NO SOURCE OVERLAY (a conformance run) has no unknown to ask about, and the honest
               answer is the one a user agent gives a request nobody answered: §5.2 returns granted or denied,
               never prompt, and a prompt nobody responded to is not permission given. */
            JS_FreeValue(ctx, over);
            *phase = PS_ASK_DEFAULT;       /* the chain is finished; the byte is back to zero for the next one */
            *out = PERMISSION_DENIED;
            return 0;
        }
        rc = step_fork_run(ctx, h, over, PR_OP_EXPRESS, 2, JS_OUTCOME_REAL_UNSTATED, &arm);
        JS_FreeValue(ctx, over);
        if (rc) return rc;
        current = (arm == 0) ? PERMISSION_GRANTED : PERMISSION_DENIED;
    }
    /* STEPS 5-7: the key is this agent's one key (asserted at the store touch), and the entry is set — which is
       what turns the answer from a decision this flow took into a fact every later read in it answers with. */
    permission_store_set(ctx, d, current);
    *phase = PS_ASK_DEFAULT;               /* the chain is finished; the byte is ready for the next question */
    DCHECK(current == PERMISSION_GRANTED || current == PERMISSION_DENIED,
           "§5.2 answered with something other than granted or denied — the algorithm's own contract is those "
           "two, and a `prompt` here is a request that decided nothing being reported as one that did");
    *out = current;
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
    /* THE KEY CANNOT BE GENERATED HERE. §5.1 step 5 reads the ENVIRONMENT's top-level origin, and this
       declaration runs before this agent has a realm, let alone a navigable carrying the field — so it is
       generated where the standard generates it, at the read, and the first read is what this store is keyed
       by from then on. */
    g_key = NULL;
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
}

void permission_store_free(void)
{
    if (!g_ready)
        return;
    DCHECK(g_rt != NULL, "§3.2's store was built without recording the runtime that owns its values");
    JS_FreeValueRT(g_rt, g_store);
    JS_FreeValueRT(g_rt, g_sources);
    g_store = g_sources = JS_UNDEFINED;
    /* THE KEY IS AN ORIGIN AND ORIGINS GO WITH THE AGENT (origin_release), so there is nothing here to free —
       only the pointer to drop, so the next agent's first read generates its own. */
    g_key = NULL;
    g_ready = 0;
    g_rt = NULL;
    /* §4's per-feature ALGORITHMS are the agent's, declared by the components that own the features — so they
       go with the agent, and the next agent's components declare their own. Left standing they would make the
       second agent's declaration read as a duplicate. */
    memset(PF_SUBJECT_FN, 0, sizeof PF_SUBJECT_FN);
    memset(PF_CONSTRAINT_FN, 0, sizeof PF_CONSTRAINT_FN);
}
