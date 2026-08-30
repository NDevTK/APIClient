/* Web Cryptography API §13's CryptoKey — the interface, its four members and §13.3's internal slots. See
 * crypto_key.h for the IDL, for why this comes before every absent method of §14, and for the two NAMED
 * RESIDUALS ([[handle]] and §13.5's serialization steps) this file is narrower than the standard by.
 *
 * ONE PROBLEM: a key is a VALUE. §13 declares four readonly attributes and no operations, so this file holds a
 * mint, four getters and nothing else. WHICH keys exist, what their bytes are and what an algorithm may do
 * with them belong to §14.3 and §20-§34, and keeping them apart is what makes this exercisable with one
 * fixture.
 *
 * THE SLOTS ARE A NULL-PROTOTYPE RECORD IN AN OWN SLOT, not a malloc'd C struct behind JS_SetOpaque. CLAUDE.md
 * states the rule and §5.2 Key Storage states the use that forces it: a key is handed to IndexedDB, held in a
 * page's closure and read back in another turn, so it must PARK to the cold tier with the flow that holds it
 * and FORK per flow. A property write is already captured by the per-flow COW delta; a C pointer captured as a
 * pointer reverts on a context switch and leaves the record reachable from nothing.
 *
 * §9 Terminology's CACHED OBJECTS ARE BUILT AT THE MINT, NOT AT THE FIRST READ, and that is a decision this
 * engine has to make differently from a single-timeline browser. §9 writes the operation lazily — "If the
 * [[slot_cached]] internal slot of object is undefined: Set the [[slot_cached]] internal slot … Return the
 * contents" — and lazily is precisely wrong here: the first FLOW to read `key.algorithm` would create the
 * object inside its own delta, so it would be that flow's private object and every sibling would either see a
 * different one or observe one that vanishes when the flow unapplies. core/crypto/crypto.c makes the same
 * argument for §10.1's draw position and core/realm.h for a realm's intrinsics. Nothing can tell the two
 * apart: [[slot_cached]] is not exposed, the conversion runs none of the page's code, and both orders answer
 * `key.algorithm === key.algorithm` with true and hand back the same object for the life of the key. So §9's
 * "is undefined" arm is UNREACHABLE BY CONSTRUCTION here, and that is asserted at the read rather than left as
 * a claim in this comment. */
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/crypto/crypto_key.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"

/* §13.2: "The recognized key type values are "public", "private", and "secret"", indexed by CryptoKeyType. */
static const char *const CK_TYPE_NAMES[CRYPTO_KEY_TYPE_N] = { "public", "private", "secret" };

/* §13.2's "list of recognized key usage values", in the order §9 Terminology's usage intersection produces —
   bit i of a normalized mask is entry i of this list, which is what makes the mask and the sequence one fact
   rather than two. */
static const char *const CK_USAGE_NAMES[] = {
    "encrypt", "decrypt", "sign", "verify", "deriveKey", "deriveBits", "wrapKey", "unwrapKey"
};
#define CK_USAGE_N ((int)(sizeof CK_USAGE_NAMES / sizeof CK_USAGE_NAMES[0]))
/* THE MASK AND THE LIST ARE ONE DECLARATION, asserted by the compiler rather than by a getter: a ninth
   recognized usage added to the header without a name here would report the eight it knows and silently drop
   the new one from every `key.usages`. */
_Static_assert(CRYPTO_KEY_USAGES_ALL == (1u << 8) - 1u,
               "Web Cryptography §13.2's list of recognized key usage values and CRYPTO_KEY_USAGES_ALL have "
               "come apart — the mask is the set of entries of that list and there is no third statement of it");

/* THE FIELDS ARE §13.3's INTERNAL SLOTS, NAMED AS §13.3 NAMES THEM. Six of the seven: [[handle]] is the
   residual crypto_key.h states. */
#define CK_SLOT_TYPE       "type"
#define CK_SLOT_EXTRACT    "extractable"
#define CK_SLOT_ALGORITHM  "algorithm"
#define CK_SLOT_ALG_CACHED "algorithm_cached"
#define CK_SLOT_USAGES     "usages"
#define CK_SLOT_USE_CACHED "usages_cached"

/* The getter's magic — which member of §13.4 is being read. */
enum { CK_M_TYPE = 0, CK_M_EXTRACTABLE, CK_M_ALGORITHM, CK_M_USAGES };

/* §9's "If the [[slot_cached]] internal slot of object is undefined" ARM IS UNREACHABLE HERE, and this is the
   assert that says so rather than the file comment: both cached objects are built by the mint, so a getter
   that found one absent would be reading a key built somewhere other than crypto_key_new. */
#define CK_CACHE_WHY \
    "a CryptoKey's cached ECMAScript object is absent — §9 Terminology's lazy arm does not exist in this " \
    "engine, because a cache built inside whichever flow read first would be that flow's private object"

static JSClassID g_key_class;
/* The private key the slot record hangs off — a Symbol, so a page enumerating a key cannot see it and cannot
   collide with it. `g_ready` rather than testing g_slot_key, because a static JSValue is zero-initialised and
   zero is not JS_UNDEFINED. */
static JSValue    g_slot_key = JS_UNDEFINED;
static JSAtom     g_slot_atom = JS_ATOM_NULL;
static int        g_ready;
/* THE RUNTIME THE SYMBOL AND ITS ATOM BELONG TO. Interned names are agent state freed against the runtime they
   were interned in; a release that cannot name one leaks a JSAtomStruct, which JS_FreeRuntime's atom walk
   reports by description and nothing else would have shown. crypto/subtle_crypto.c carries the same field for
   the same reason. */
static JSRuntime *g_rt;

/* ---- §13.3's internal slots ------------------------------------------------------------------------------ */

/* THE BRAND AND THE RECORD ARE ONE LOOKUP. Web IDL §3.7.6 Attributes' attribute getter throws a TypeError when
   the `this` value "does not implement" the interface — `Object.getOwnPropertyDescriptor(CryptoKey.prototype,
   'type').get.call({})` is that throw, and a page tells it apart from `undefined`. The class id is the brand
   because the state is an own slot, which anything could be given; the class is what cannot be forged. */
static JSValue ck_slots(JSContext *ctx, JSValueConst this_val)
{
    JSValue st;

    DCHECK(g_ready, "a CryptoKey slot record was asked for before crypto_key_init declared its key");
    if (JS_GetClassID(this_val) != g_key_class)
        return JS_ThrowTypeError(ctx, "a CryptoKey attribute getter was reached on something that is not a "
                                      "CryptoKey");
    /* AN OWN SLOT, never a lookup: a miss on a lookup is the solver's absent-state seam and would mint a
       concolic for an internal slot — right for the page's own reads, wrong here. */
    if (JS_GetOwnSlot(ctx, &st, this_val, g_slot_atom) <= 0)
        st = JS_UNDEFINED;
    DCHECK(JS_IsObject(st), "a CryptoKey carries no slot record — crypto_key_new sets all six of §13.3's slots "
                            "before the key exists, so an object branded CryptoKey without one was built "
                            "somewhere other than that mint");
    return st;
}

/* ---- §9 Terminology's "cached ECMAScript object" ---------------------------------------------------------- */

/* §9: "Set the [[slot_cached]] internal slot of object to the result of performing type conversion to an
 * ECMAScript object as defined in [WebIDL] to the contents of the [[slot]] internal slot of object."
 *
 * FOR [[algorithm]] THAT IS A DICTIONARY CONVERSION, AND THE RESULT MUST BE A DIFFERENT OBJECT FROM THE SLOT.
 * That is the whole reason §13.3 declares [[algorithm]] and [[algorithm_cached]] separately rather than one
 * slot: the page owns what comes back — `key.algorithm.name = 'AES-CBC'` is an ordinary write to an ordinary
 * object — while §20.9.1's "Let algorithm be the [[algorithm]] internal slot of key" reads the slot. Handing
 * out the slot itself answers `key.algorithm === key.algorithm` correctly and then lets three characters of
 * script change which cipher a key IS.
 *
 * THE MEMBER KINDS ARE ASKED POSITIVELY AND ANYTHING ELSE CRASHES, because a member this does not convert is a
 * capability to build and not a value to guess at. §12's KeyAlgorithm is `{name}`; the derivations add
 * DOMStrings (§23.5's `namedCurve`), integers (§27.4's `length`, §20.6's `modulusLength`) and NESTED
 * KeyAlgorithms (§20.7's and §31.4's `hash`), all of which this converts. The one shape it does not is §16's
 * `BigInteger` — §20.6's `publicExponent`, a Uint8Array — whose conversion is a copy of the BYTES rather than
 * of the members, and which no minting algorithm in this build reaches. */
static JSValue ck_dictionary_to_es_object(JSContext *ctx, JSValueConst dict)
{
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0, i;
    JSValue out;

    DCHECK(JS_IsObject(dict), "§9's cached-object conversion was handed a [[algorithm]] slot that is not a "
                              "dictionary — §13.3 requires its contents to be, or be derived from, a §12 "
                              "KeyAlgorithm, and every minting algorithm builds one");
    out = JS_NewObject(ctx);
    CHECK(!JS_IsException(out), "§9's cached ECMAScript object for a CryptoKey's [[algorithm]] could not be "
                                "allocated");
    CHECK(JS_GetOwnPropertyNames(ctx, &tab, &len, dict, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0,
          "the members of a CryptoKey's [[algorithm]] could not be enumerated — the slot is an object this "
          "engine built out of a §12 KeyAlgorithm, so there is no page code and no exotic behaviour in it");
    for (i = 0; i < len; i++) {
        JSValue v = JS_GetProperty(ctx, dict, tab[i].atom), copy;

        CHECK(!JS_IsException(v), "a member of a CryptoKey's [[algorithm]] could not be read back — the slot "
                                  "is an engine-built dictionary of data properties");
        if (JS_IsString(v) || JS_IsNumber(v) || JS_IsBool(v) || JS_IsNull(v) || JS_IsUndefined(v)) {
            copy = v;                                  /* immutable: the value IS the conversion */
        } else if (JS_IsObject(v) && JS_GetTypedArrayType(v) < 0 && !JS_IsArrayBuffer(v) &&
                   !JS_IsDataView(v) && !JS_IsArray(v) && !JS_IsFunction(ctx, v)) {
            copy = ck_dictionary_to_es_object(ctx, v); /* §20.7's and §31.4's `hash`, a nested KeyAlgorithm */
            JS_FreeValue(ctx, v);
        } else {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, out);
            JS_FreePropertyEnum(ctx, tab, len);
            DFAIL("a CryptoKey's [[algorithm]] carries a member that is neither a primitive nor a nested "
                  "KeyAlgorithm — §16 BigInteger (§20.6 RsaKeyAlgorithm dictionary's `publicExponent`, a "
                  "Uint8Array) is the shape this reaches next, and its conversion copies the BYTES rather "
                  "than the members. Build that arm here; sharing the view would hand the page the slot's "
                  "own buffer, which is the aliasing this whole conversion exists to prevent");
            return JS_UNDEFINED;
        }
        /* CreateDataPropertyOrThrow, never a Set — the object is being BUILT, so its members are own data
           properties and no accessor of any prototype may intercept them. idl_slots.h states the pair rule. */
        CHECK(JS_DefinePropertyValue(ctx, out, tab[i].atom, copy,
                                     JS_PROP_C_W_E) >= 0,
              "a member of §9's cached [[algorithm]] object could not be defined on it");
    }
    JS_FreePropertyEnum(ctx, tab, len);
    return out;
}

/* §9's other cached object: [[usages]] is a Sequence<KeyUsage>, and Web IDL §3.2.21 Sequences — sequence< T >
   converts one to "a new Array object created as if by the expression []" filled by CreateDataPropertyOrThrow
   at 0..n-1. The sequence is §9's normalized value, so its entries are the recognized usages the mask names,
   "in the order listed in the list of recognized key usage values" — which is this walk. */
static JSValue ck_usages_to_es_object(JSContext *ctx, uint32_t usages)
{
    JSValue arr = JS_NewArray(ctx);
    uint32_t n = 0;
    int i;

    CHECK(!JS_IsException(arr), "§9's cached ECMAScript object for a CryptoKey's [[usages]] could not be "
                                "allocated");
    for (i = 0; i < CK_USAGE_N; i++)
        if (usages & (1u << i))
            CHECK(JS_DefinePropertyValueUint32(ctx, arr, n++, JS_NewString(ctx, CK_USAGE_NAMES[i]),
                                               JS_PROP_C_W_E) >= 0,
                  "an entry of §9's cached [[usages]] Array could not be defined on it");
    return arr;
}

/* ---- §13.4's four members --------------------------------------------------------------------------------- */

static JSValue js_ck_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue st = ck_slots(ctx, this_val), v;

    if (JS_IsException(st))
        return st;
    switch (magic) {
    case CK_M_TYPE: {
        /* §13.4: "Reflects the [[type]] internal slot, which contains the type of the underlying key." The
           slot holds the KeyType, and the DOMString is §13.2's name for it — derived here rather than stored,
           so the enum stays the one statement of which types exist. */
        int32_t t = -1;

        v = JS_GetPropertyStr(ctx, st, CK_SLOT_TYPE);
        JS_ToInt32(ctx, &t, v);
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, st);
        DCHECK(t >= 0 && t < CRYPTO_KEY_TYPE_N,
               "a CryptoKey's [[type]] slot holds a value §13.2's KeyType does not declare");
        return JS_NewString(ctx, CK_TYPE_NAMES[t]);
    }
    case CK_M_EXTRACTABLE:
        /* §13.4: "Reflects the [[extractable]] internal slot", a boolean. */
        v = JS_GetPropertyStr(ctx, st, CK_SLOT_EXTRACT);
        DCHECK(JS_IsBool(v), "a CryptoKey's [[extractable]] slot does not hold a boolean");
        break;
    case CK_M_ALGORITHM:
        /* §13.4: "Returns the cached ECMAScript object associated with the [[algorithm]] internal slot." */
        v = JS_GetPropertyStr(ctx, st, CK_SLOT_ALG_CACHED);
        DCHECK(JS_IsObject(v), CK_CACHE_WHY);
        break;
    case CK_M_USAGES:
        /* §13.4: "Returns the cached ECMAScript object associated with the [[usages]] internal slot." */
        v = JS_GetPropertyStr(ctx, st, CK_SLOT_USE_CACHED);
        DCHECK(JS_IsObject(v), CK_CACHE_WHY);
        break;
    default:
        JS_FreeValue(ctx, st);
        DFAIL("a CryptoKey attribute getter ran with a magic §13.4 does not declare");
        return JS_UNDEFINED;
    }
    JS_FreeValue(ctx, st);
    return v;
}

/* ---- the mint --------------------------------------------------------------------------------------------- */

JSValue crypto_key_new(JSContext *ctx, CryptoKeyType type, bool extractable, JSValue algorithm,
                       uint32_t usages)
{
    JSValue key, st, proto;

    DCHECK(g_ready, "a CryptoKey was minted before crypto_key_init declared the interface");
    DCHECK((int)type >= 0 && type < CRYPTO_KEY_TYPE_N,
           "a CryptoKey was minted with a [[type]] §13.2's KeyType does not declare");
    /* §9's "normalized value of a usages list" IS what [[usages]] holds — the usage intersection against every
       recognized value — so a bit outside the recognized set is a caller that did not normalize. */
    DCHECK((usages & ~(uint32_t)CRYPTO_KEY_USAGES_ALL) == 0,
           "a CryptoKey was minted with a usage bit §13.2 does not recognize — [[usages]] is §9 Terminology's "
           "normalized value of a usages list, which cannot contain one");

    proto = JS_GetClassProto(ctx, g_key_class);
    DCHECK(!JS_IsNull(proto), "a CryptoKey was minted in a realm that never ran its prototype install");
    key = JS_NewObjectProtoClass(ctx, proto, g_key_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(key), "a CryptoKey could not be allocated");

    st = idl_slots_new(ctx);
    CHECK(!JS_IsException(st), "a CryptoKey's slot record could not be allocated");
    JS_SetPropertyStr(ctx, st, CK_SLOT_TYPE, JS_NewInt32(ctx, (int32_t)type));
    JS_SetPropertyStr(ctx, st, CK_SLOT_EXTRACT, JS_NewBool(ctx, extractable));
    /* THE CACHED OBJECTS ARE BUILT BEFORE THE SLOTS THEY CACHE ARE HANDED OVER, so the conversion reads the
       [[algorithm]] this mint was given and not a value some later step could have replaced. */
    JS_SetPropertyStr(ctx, st, CK_SLOT_ALG_CACHED, ck_dictionary_to_es_object(ctx, algorithm));
    JS_SetPropertyStr(ctx, st, CK_SLOT_ALGORITHM, algorithm);   /* CONSUMED */
    JS_SetPropertyStr(ctx, st, CK_SLOT_USAGES, JS_NewInt32(ctx, (int32_t)usages));
    JS_SetPropertyStr(ctx, st, CK_SLOT_USE_CACHED, ck_usages_to_es_object(ctx, usages));
    JS_SetProperty(ctx, key, g_slot_atom, st);
    return key;
}

/* ---- the per-realm install --------------------------------------------------------------------------------- */

static void crypto_key_install_realm(JSContext *ctx)
{
    JSValue proto, prev, global;

    prev = JS_GetClassProto(ctx, g_key_class);
    DCHECK(JS_IsNull(prev), "crypto_key_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "CryptoKey.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "CryptoKey");
    /* §13's INTERFACE IS `[SecureContext]` AS A WHOLE, so Web IDL §3.3.13 [SecureContext] removes each member
       in a non-secure realm rather than making it throw — its own example says so of an interface-level
       annotation: "HeartbeatSensor will not be exposed in a non-secure context, nor will its members".
       A NAMED RESIDUAL RIDES WITH THAT, and it is the OTHER half of the same sentence: §3.3.13 also removes
       the interface OBJECT ("there will be no 'HeartbeatSensor' property on Window"), and core/idl_args.h has
       no exposed form of idl_interface_object to state that with — the only installers carrying an
       IdlExposure are the accessor's and the method's. THE NEXT DIFF adds `idl_install_interface_object_exposed`
       there, taking the same IdlExposure and asking the same one `idl_exposed`, and both §13 and §14 pass
       IDL_SECURE_CONTEXT to it. ITS ABSENCE SHOWS as `'CryptoKey' in window` and `'SubtleCrypto' in window`
       answering true over plain http, where real Chrome answers false — a feature-detect branch a bundle
       really writes, taken the wrong way. */
    idl_install_accessor_exposed(ctx, proto, "type", js_ck_get, CK_M_TYPE, -1, IDL_SECURE_CONTEXT);
    idl_install_accessor_exposed(ctx, proto, "extractable", js_ck_get, CK_M_EXTRACTABLE, -1,
                                 IDL_SECURE_CONTEXT);
    idl_install_accessor_exposed(ctx, proto, "algorithm", js_ck_get, CK_M_ALGORITHM, -1, IDL_SECURE_CONTEXT);
    idl_install_accessor_exposed(ctx, proto, "usages", js_ck_get, CK_M_USAGES, -1, IDL_SECURE_CONTEXT);
    JS_SetClassProto(ctx, g_key_class, JS_DupValue(ctx, proto));

    global = JS_GetGlobalObject(ctx);
    /* §13 DECLARES NO CONSTRUCTOR, so the interface object's [[Call]] and [[Construct]] both throw — a key
       comes into existence only through §14.3's minting methods, which is what "an opaque reference to keying
       material that is managed by the user agent" means. */
    JS_SetPropertyStr(ctx, global, "CryptoKey", idl_interface_object(ctx, "CryptoKey", proto));
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, proto);
}

void crypto_key_init(JSContext *ctx)
{
    JSClassDef d = { "CryptoKey" };

    DCHECK(!g_ready, "crypto_key_init ran twice — the class and the slot key are the AGENT's");
    g_rt = JS_GetRuntime(ctx);
    JS_NewClassID(g_rt, &g_key_class);
    CHECK(JS_NewClass(g_rt, g_key_class, &d) == 0,
          "CryptoKey: the per-realm prototype slot could not be declared");
    g_slot_key = JS_NewSymbol(ctx, "cryptoKeySlots", false);
    CHECK(!JS_IsException(g_slot_key), "the CryptoKey slot key allocation failed");
    g_slot_atom = JS_ValueToAtom(ctx, g_slot_key);
    CHECK(g_slot_atom != JS_ATOM_NULL, "the CryptoKey slot key could not be interned");
    g_ready = 1;
    /* DECLARED UNDER THE ROW THAT RELEASES IT, which is `crypto` — §10's component declares this one through
       §14's and its release reaches this one's, so core/platform.c's two-sided check is asking about the pair.
       core/crypto/subtle_crypto.c states the same argument for its own three slots. */
    agent_state_flag("crypto", &g_ready, "§13's CryptoKey declaration latch");
    agent_state_class("crypto", &g_key_class, "Web Cryptography §13 CryptoKey's per-realm prototype slot and "
                                             "brand");
    agent_state_value("crypto", &g_slot_key, "§13.3's internal-slot record key");
    agent_state_atom("crypto", &g_slot_atom, "§13.3's internal-slot record key, interned");
    agent_state_ptr("crypto", &g_rt, "the runtime the §13.3 slot key was interned in");
    realm_declare_intrinsic(crypto_key_install_realm);
}

void crypto_key_free(void)
{
    if (!g_ready)
        return;
    DCHECK(g_rt != NULL, "CryptoKey was declared without recording the runtime its slot key belongs to");
    JS_FreeAtomRT(g_rt, g_slot_atom);
    JS_FreeValueRT(g_rt, g_slot_key);
    g_slot_atom = JS_ATOM_NULL;
    g_slot_key = JS_UNDEFINED;   /* the prototypes are the REALMS' — released with their contexts */
    g_key_class = 0;
    g_ready = 0;
    g_rt = NULL;
}
