/* Web Cryptography API §13's CryptoKey — the interface, its four members, §13.3's internal slots and §13.5's
 * serialization and deserialization steps. See crypto_key.h for the IDL and for why this comes before every
 * absent method of §14.
 * THIS LINE HAS NAMED A COUNT OF NAMED RESIDUALS TWICE AND BEEN WRONG BOTH TIMES, and the shape is the same
 * each time rather than two accidents: it said TWO and named [[handle]] as one when the mint already took it
 * as a REQUIRED parameter, then ONE and named §13.5's steps when the landing that built them is the commit
 * this sentence is in. A count of what is MISSING is the artifact CLAUDE.md's opening rates worst — it is read
 * by exactly the person about to invalidate it — so there is no count here now: what this file is narrower
 * than the standard by is stated at the sites, by named residuals, and `git grep -n RESIDUAL` over this
 * directory is the derivation that cannot go stale.
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
#include "core/structured_clone.h"

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

/* THE FIELDS ARE §13.3's INTERNAL SLOTS, NAMED AS §13.3 NAMES THEM — all seven. [[handle]] holds the keying
   material as an ArrayBuffer, which is a JS value for the reason crypto_key.h states at length. */
#define CK_SLOT_TYPE       "type"
#define CK_SLOT_EXTRACT    "extractable"
#define CK_SLOT_ALGORITHM  "algorithm"
#define CK_SLOT_ALG_CACHED "algorithm_cached"
#define CK_SLOT_USAGES     "usages"
#define CK_SLOT_USE_CACHED "usages_cached"
#define CK_SLOT_HANDLE     "handle"

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
    DCHECK(JS_IsObject(st), "a CryptoKey carries no slot record — crypto_key_new sets all SEVEN of §13.3's "
                            "slots before the key exists, so an object branded CryptoKey without one was built "
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
   "in the order listed in the list of recognized key usage values" — which is this walk.
   IT IS THIS COMPONENT'S ENTRY RATHER THAN A FILE STATIC because §29.4.5 and §31.6.5 each end their jwk arm
   with "Set the key_ops attribute of jwk to equal the usages attribute of key", and the `usages` attribute is
   the object this builds. An export that walked the mask itself would be a SECOND statement of the order, and
   the order is exactly what a jwk round trip compares — `key_ops` is checked as a stringified array, so two
   walks could agree on which usages a key has and still disagree about the value, which is the shape no
   membership test anywhere would report. */
JSValue crypto_key_usages_sequence(JSContext *ctx, uint32_t usages)
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

/* ---- §13.3's slots, read by the algorithms of §20-§34 ------------------------------------------------------ */

/* THE READERS GO THROUGH ck_slots, WHICH IS THE BRAND — so an algorithm handed something that is not a
   CryptoKey aborts at the read rather than answering out of an own property anything could carry. They are
   separate from js_ck_get because they answer with the SLOT and js_ck_get answers with §13.4's member, and for
   [[algorithm]] those are deliberately two different objects. */
JSValue crypto_key_handle(JSContext *ctx, JSValueConst key)
{
    JSValue st = ck_slots(ctx, key), v;

    DCHECK(!JS_IsException(st), "an algorithm read the [[handle]] of something that is not a CryptoKey — the "
                                "§3.2.15 brand at the argument position is what keeps one out");
    v = JS_GetPropertyStr(ctx, st, CK_SLOT_HANDLE);
    JS_FreeValue(ctx, st);
    DCHECK(JS_IsArrayBuffer(v),
           "a CryptoKey's [[handle]] is not the ArrayBuffer its mint was given — §13.3 declares the slot on "
           "every key and crypto_key_new is the only writer, so a key without one was built somewhere else");
    return v;
}

JSValue crypto_key_algorithm(JSContext *ctx, JSValueConst key)
{
    JSValue st = ck_slots(ctx, key), v;

    DCHECK(!JS_IsException(st), "an algorithm read the [[algorithm]] of something that is not a CryptoKey");
    v = JS_GetPropertyStr(ctx, st, CK_SLOT_ALGORITHM);
    JS_FreeValue(ctx, st);
    DCHECK(JS_IsObject(v), "a CryptoKey's [[algorithm]] slot is not a dictionary");
    return v;
}

bool crypto_key_extractable(JSContext *ctx, JSValueConst key)
{
    JSValue st = ck_slots(ctx, key), v;
    bool ext;

    DCHECK(!JS_IsException(st), "an algorithm read the [[extractable]] of something that is not a CryptoKey");
    v = JS_GetPropertyStr(ctx, st, CK_SLOT_EXTRACT);
    JS_FreeValue(ctx, st);
    /* THE ASSERT IS THE TYPE AND NOT THE VALUE, which is the whole of what may be asserted here: the slot is
       written by crypto_key_new from §14.3.9's own `extractable` argument, so BOTH booleans are states this
       engine legitimately puts in it and neither is an invariant. A non-boolean is this codebase's own logic
       being wrong, which is what a DCHECK is for. */
    DCHECK(JS_IsBool(v), "a CryptoKey's [[extractable]] slot does not hold a boolean — §13.3 declares the slot "
                         "on every key and crypto_key_new is the only writer of it");
    ext = JS_ToBool(ctx, v) != 0;
    JS_FreeValue(ctx, v);
    return ext;
}

uint32_t crypto_key_usages(JSContext *ctx, JSValueConst key)
{
    JSValue st = ck_slots(ctx, key), v;
    int64_t u = 0;

    DCHECK(!JS_IsException(st), "an algorithm read the [[usages]] of something that is not a CryptoKey");
    v = JS_GetPropertyStr(ctx, st, CK_SLOT_USAGES);
    JS_ToInt64(ctx, &u, v);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, st);
    DCHECK(u >= 0 && (uint64_t)u <= (uint64_t)CRYPTO_KEY_USAGES_ALL,
           "a CryptoKey's [[usages]] slot holds a bit §13.2 does not recognize — the slot is §9 Terminology's "
           "normalized value of a usages list, which cannot contain one");
    return (uint32_t)u;
}

JSClassID crypto_key_class(void)
{
    DCHECK(g_ready, "a CryptoKey argument position was declared before crypto_key_init declared the class");
    return g_key_class;
}

/* ---- the mint --------------------------------------------------------------------------------------------- */

/* Web Cryptography §13.3's SEVEN SLOTS, WRITTEN ONTO A KEY THAT ALREADY EXISTS — the mint's second half, and
   the whole body of Web Cryptography §13.5's deserialization steps. It is ONE function rather than two because
   a key has one set of slots: the pair would be two statements of which slots §13.3 declares, and the one that
   drifts is whichever of them a new slot is not added to. `algorithm` and `handle` are CONSUMED by both
   callers. */
static void ck_set_slots(JSContext *ctx, JSValueConst key, CryptoKeyType type, bool extractable,
                         JSValue algorithm, uint32_t usages, JSValue handle)
{
    JSValue st = idl_slots_new(ctx);

    CHECK(!JS_IsException(st), "a CryptoKey's slot record could not be allocated");
    JS_SetPropertyStr(ctx, st, CK_SLOT_TYPE, JS_NewInt32(ctx, (int32_t)type));
    JS_SetPropertyStr(ctx, st, CK_SLOT_EXTRACT, JS_NewBool(ctx, extractable));
    /* THE CACHED OBJECTS ARE BUILT BEFORE THE SLOTS THEY CACHE ARE HANDED OVER, so the conversion reads the
       [[algorithm]] this mint was given and not a value some later step could have replaced. */
    JS_SetPropertyStr(ctx, st, CK_SLOT_ALG_CACHED, ck_dictionary_to_es_object(ctx, algorithm));
    JS_SetPropertyStr(ctx, st, CK_SLOT_ALGORITHM, algorithm);   /* CONSUMED */
    JS_SetPropertyStr(ctx, st, CK_SLOT_USAGES, JS_NewInt32(ctx, (int32_t)usages));
    JS_SetPropertyStr(ctx, st, CK_SLOT_USE_CACHED, crypto_key_usages_sequence(ctx, usages));
    /* §13.3's [[handle]]. It has NO CACHED OBJECT beside it and no member of §13.4 answers with it: §13.1 calls
       a CryptoKey "an opaque reference to keying material", and the whole of that opacity is that the bytes are
       reachable only from this record, which hangs off a private Symbol. §31.6.5 Export Key is the one
       operation that ever reads them back out to the page, and it is gated on [[extractable]]. */
    JS_SetPropertyStr(ctx, st, CK_SLOT_HANDLE, handle);         /* CONSUMED */
    JS_SetProperty(ctx, key, g_slot_atom, st);
}

JSValue crypto_key_new(JSContext *ctx, CryptoKeyType type, bool extractable, JSValue algorithm,
                       uint32_t usages, JSValue handle)
{
    JSValue key, st, proto;

    DCHECK(g_ready, "a CryptoKey was minted before crypto_key_init declared the interface");
    DCHECK(JS_IsArrayBuffer(handle),
           "a CryptoKey was minted with a [[handle]] that is not an ArrayBuffer — §13.3's seventh slot holds "
           "the keying material, and it is a JS value so that it forks per flow and parks with the flow that "
           "holds it (crypto_key.h states why at length)");
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

    ck_set_slots(ctx, key, type, extractable, algorithm, usages, handle);
    return key;
}

/* ---- §13.5's serialization and deserialization steps ------------------------------------------------------ */

/* Web Cryptography §13.5 "Serialization and deserialization steps": "CryptoKey objects are serializable
 * objects." FIVE steps each way, counted as the top-level items of two flat lists — the IDL's
 * `[Serializable]` on Web Cryptography §13's interface is what HTML §2.7.1 reads, and
 * core/structured_clone.h holds the registry this is a row of and the whole derivation of its shape. THE
 * DERIVATION IS THERE AND NOT HERE ON PURPOSE: a citation lives in the file whose algorithm it describes, and
 * a run of foreign section numbers dropped into a file whose own standard is Web Cryptography is what moves
 * that file's own citations out from under it.
 *
 * THE HOLDER'S FIELDS ARE Web Cryptography §13.5's OWN Record FIELDS, SPELLED AS IT SPELLS THEM, and it is a
 * null-prototype record because it is `serialized` and never a value a page receives. The registry hands the
 * writer ONE value, so all five ride one record that the SAME walk reaches under the SAME `memory` — which is
 * what makes the two genuinely nested ones ([[algorithm]] and [[handle]]) sub-serializations rather than a
 * second encoding. core/structured_clone.h states why it is one value rather than a Record of fields.
 *
 * [[Handle]] IS A SUB-SERIALIZATION HERE AND A PLAIN COPY IN Web Cryptography §13.5, AND THAT IS THIS
 * ENGINE'S SLOT AND NOT A DIVERGENCE. Its step 5 is "Set serialized.[[Handle]] to the [[handle]] internal slot
 * of value", and Web Cryptography §13.3 makes that slot "whatever data the underlying cryptographic
 * implementation uses to represent a logical key" — a spec-level opaque with no realm in it. In this engine
 * the representation IS a JS ArrayBuffer, for the reason crypto_key.h states at length, so putting it in the
 * holder is what carries its bytes; the reader builds a NEW buffer, which is the realm-independent form the
 * seam requires and not an extra copy this row chose to make.
 *
 * NEITHER LIST NAMES A CACHED SLOT, which is why the deserialization side re-mints both. The seam hands these
 * steps a value with none of its internal data set up, so Web Cryptography §9 Terminology's two cached objects
 * are built in the TARGET realm out of the deserialized slots — which is what keeps one key's `algorithm` from
 * being another key's. */
#define CK_SER_TYPE    "Type"
#define CK_SER_EXTRACT "Extractable"
#define CK_SER_ALG     "Algorithm"
#define CK_SER_USAGES  "Usages"
#define CK_SER_HANDLE  "Handle"

/* THE ROW'S OWN BRAND TEST — which of the registry's interfaces this value is, if any. The CLASS is the
   question and not the slot record, for ck_slots' reason: a slot record is an own property anything could be
   given, and the class is what cannot be forged — and the reader hands this predicate an instance whose record
   is deliberately not set up yet. */
static bool ck_is(JSContext *ctx, JSValueConst v)
{
    (void)ctx;
    return g_ready && JS_GetClassID(v) == g_key_class;
}

/* Web Cryptography §13.5's FIVE SERIALIZATION STEPS. */
static JSValue ck_serialize(JSContext *ctx, JSValueConst v)
{
    JSValue st = ck_slots(ctx, v), out, f;

    DCHECK(!JS_IsException(st), "Web Cryptography §13.5's serialization steps were performed on something "
                                "that is not a CryptoKey — the serializer reaches them only through this "
                                "row's own brand test");
    out = idl_slots_new(ctx);
    CHECK(!JS_IsException(out), "Web Cryptography §13.5's `serialized` record could not be allocated");

    /* STEP 1: "Set serialized.[[Type]] to the [[type]] internal slot of value." The slot holds the KeyType,
       which is this engine's spelling of §13.2's three recognized values and travels as itself. */
    f = JS_GetPropertyStr(ctx, st, CK_SLOT_TYPE);
    DCHECK(JS_IsNumber(f), "a CryptoKey's [[type]] slot does not hold a KeyType");
    JS_SetPropertyStr(ctx, out, CK_SER_TYPE, f);
    /* STEP 2: "Set serialized.[[Extractable]] to the [[extractable]] internal slot of value." */
    f = JS_GetPropertyStr(ctx, st, CK_SLOT_EXTRACT);
    DCHECK(JS_IsBool(f), "a CryptoKey's [[extractable]] slot does not hold a boolean");
    JS_SetPropertyStr(ctx, out, CK_SER_EXTRACT, f);
    /* STEP 3: "Set serialized.[[Algorithm]] to the sub-serialization of the [[algorithm]] internal slot of
       value." The SLOT and never §13.4's cached object — handing the cached one across would make the clone's
       slot the page-owned object, which is the aliasing crypto_key.h's whole cached-object argument prevents. */
    f = JS_GetPropertyStr(ctx, st, CK_SLOT_ALGORITHM);
    DCHECK(JS_IsObject(f), "a CryptoKey's [[algorithm]] slot is not a dictionary");
    JS_SetPropertyStr(ctx, out, CK_SER_ALG, f);
    /* STEP 4: "Set serialized.[[Usages]] to the sub-serialization of the [[usages]] internal slot of value."
       The slot is §9 Terminology's normalized value as a mask, which crypto_key.h argues IS that sequence. */
    f = JS_GetPropertyStr(ctx, st, CK_SLOT_USAGES);
    DCHECK(JS_IsNumber(f), "a CryptoKey's [[usages]] slot does not hold a normalized usage mask");
    JS_SetPropertyStr(ctx, out, CK_SER_USAGES, f);
    /* STEP 5: "Set serialized.[[Handle]] to the [[handle]] internal slot of value." */
    f = JS_GetPropertyStr(ctx, st, CK_SLOT_HANDLE);
    DCHECK(JS_IsArrayBuffer(f), "a CryptoKey's [[handle]] is not the ArrayBuffer its mint was given");
    JS_SetPropertyStr(ctx, out, CK_SER_HANDLE, f);

    JS_FreeValue(ctx, st);
    return out;
}

/* THE REGISTRY'S `create` — a new CryptoKey in this realm, with none of its internal data set up. IT SETS NO
   SLOTS, and that is HTML §2.7.1's own requirement rather than an economy: the value handed to the
   deserialization steps has "none of its internal data set up; setting that up is the job of these steps".
   Between this and ck_deserialize the key is branded and recordless, which no page code can observe — the
   reader puts it in its reference map and then fills it, with nothing of the page's running in between. */
static JSValue ck_create(JSContext *ctx)
{
    JSValue proto, key;

    DCHECK(g_ready, "Web Cryptography §13.5's deserialization was asked for a CryptoKey before "
                    "crypto_key_init declared the interface");
    proto = JS_GetClassProto(ctx, g_key_class);
    DCHECK(!JS_IsNull(proto), "a CryptoKey was deserialized into a realm that never ran its prototype install");
    key = JS_NewObjectProtoClass(ctx, proto, g_key_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(key), "a deserialized CryptoKey could not be allocated");
    return key;
}

/* Web Cryptography §13.5's FIVE DESERIALIZATION STEPS.
   THE HOLDER IS ASSERTED AND NOT REFUSED, which is the same answer core/structured_clone.c's ArrayBuffer
   transfer holder already gives: it is a record THIS engine's own serialization steps wrote, so a field of the
   wrong shape is this codebase disagreeing with itself. The value a page hands `structuredClone` never reaches
   here — a shape §2.7 refuses was refused by the writer, with the "DataCloneError" the standard names. The one
   path on which these bytes are not this agent's own is the routed message core/structured_clone.h's residual
   is about, and it is the whole deserializer's question rather than this row's. */
static int ck_deserialize(JSContext *ctx, JSValueConst key, JSValueConst sub)
{
    JSValue f, algorithm, handle;
    int32_t type = -1;
    int64_t usages = 0;
    bool extractable;

    DCHECK(JS_IsObject(sub), "Web Cryptography §13.5's deserialization steps were handed a `serialized` that "
                             "is not the record its serialization steps wrote");
    /* STEP 1: "Initialize the [[type]] internal slot of value to serialized.[[Type]]." */
    f = JS_GetPropertyStr(ctx, sub, CK_SER_TYPE);
    JS_ToInt32(ctx, &type, f);
    JS_FreeValue(ctx, f);
    DCHECK(type >= 0 && type < CRYPTO_KEY_TYPE_N,
           "a serialized CryptoKey's [[Type]] is not one of §13.2's three recognized key type values");
    /* STEP 2: "Initialize the [[extractable]] internal slot of value to serialized.[[Extractable]]." */
    f = JS_GetPropertyStr(ctx, sub, CK_SER_EXTRACT);
    DCHECK(JS_IsBool(f), "a serialized CryptoKey's [[Extractable]] is not a boolean");
    extractable = JS_ToBool(ctx, f) != 0;
    JS_FreeValue(ctx, f);
    /* STEP 4, READ BEFORE STEP 3 IS SPENT: "Initialize the [[usages]] internal slot of value to the
       sub-deserialization of serialized.[[Usages]]." Both are read before anything is written, so a record
       that fails one of these assertions has had nothing built out of it. */
    f = JS_GetPropertyStr(ctx, sub, CK_SER_USAGES);
    JS_ToInt64(ctx, &usages, f);
    JS_FreeValue(ctx, f);
    DCHECK(usages >= 0 && (uint64_t)usages <= (uint64_t)CRYPTO_KEY_USAGES_ALL,
           "a serialized CryptoKey's [[Usages]] carries a bit §13.2 does not recognize — the slot is §9 "
           "Terminology's normalized value of a usages list, which cannot contain one");
    /* STEP 3: "Initialize the [[algorithm]] internal slot of value to the sub-deserialization of
       serialized.[[Algorithm]]." */
    algorithm = JS_GetPropertyStr(ctx, sub, CK_SER_ALG);
    DCHECK(JS_IsObject(algorithm), "a serialized CryptoKey's [[Algorithm]] is not a dictionary");
    /* STEP 5: "Initialize the [[handle]] internal slot of value to serialized.[[Handle]]." It is a NEW
       ArrayBuffer the reader built out of the bytes the write copied, which is why the clone's key material is
       its own and not the source key's buffer under a second name. */
    handle = JS_GetPropertyStr(ctx, sub, CK_SER_HANDLE);
    DCHECK(JS_IsArrayBuffer(handle), "a serialized CryptoKey's [[Handle]] is not an ArrayBuffer");

    ck_set_slots(ctx, key, (CryptoKeyType)type, extractable, algorithm, (uint32_t)usages, handle);
    return 0;
}

static const StructuredSerializable CK_SERIALIZABLE = {
    "CryptoKey", ck_is, ck_serialize, ck_create, ck_deserialize
};

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
       annotation: "HeartbeatSensor will not be exposed in a non-secure context, nor will its members". The
       SAME SENTENCE'S other half is the interface object ("In such a context, there will be no
       \"HeartbeatSensor\" property on Window"), which the global install below states with the same
       IdlExposure; the two are one annotation and are read from one place. */
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
    idl_install_interface_object_exposed(ctx, global, "CryptoKey", proto, IDL_SECURE_CONTEXT);
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
    /* Web Cryptography §13's `[Serializable]` EXTENDED ATTRIBUTE, AS THE ROW HTML §2.7.1 READS IT AS.
       Registered from this `_init` and not from the per-realm install, because the registry is the AGENT's —
       every realm of an agent runs the same intrinsic list, which is why core/structured_clone.c answers the
       not-exposed-in-targetRealm question out of registry membership. */
    structured_register_serializable(&CK_SERIALIZABLE);
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
