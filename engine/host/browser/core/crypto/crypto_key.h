/* Web Cryptography API §13 CryptoKey interface — "an opaque reference to keying material that is managed by
 * the user agent".
 *
 *   enum KeyType { "public", "private", "secret" };
 *   enum KeyUsage { "encrypt", "decrypt", "sign", "verify", "deriveKey", "deriveBits", "wrapKey",
 *                   "unwrapKey" };
 *
 *   [SecureContext,Exposed=(Window,Worker),Serializable]
 *   interface CryptoKey {
 *     readonly attribute KeyType type;
 *     readonly attribute boolean extractable;
 *     readonly attribute object  algorithm;
 *     readonly attribute object  usages;
 *   };
 *
 * WHY THIS INTERFACE COMES BEFORE ANY OF §14's ELEVEN ABSENT METHODS. Every one of them takes a CryptoKey, or
 * returns one, or both: §14.3.1's `encrypt` and §14.3.3's `sign` take one, §14.3.6's `generateKey` and
 * §14.3.9's `importKey` mint one, §14.3.11's `wrapKey` takes two. So the key model is not one of twelve pieces
 * of work — it is the piece the other eleven each stand on, and a shape of one would be the stub CLAUDE.md's
 * §NO STUBS forbids in the place it would do the most damage, since a wrong `type` or `usages` is what decides
 * whether an algorithm's step 2 throws an InvalidAccessError.
 *
 * THE FOUR MEMBERS ARE TWO DIFFERENT KINDS OF ANSWER, AND §13.4 SAYS SO IN ITS OWN WORDS. `type` and
 * `extractable` "Reflect" their internal slots — the value IS the slot. `algorithm` and `usages` each
 * "returns the cached ECMAScript object associated with" its own internal slot, which is §9 Terminology's
 * operation over a SECOND slot
 * ([[algorithm_cached]], [[usages_cached]]), and the difference is reachable from three lines of script: the
 * object a page gets back is the SAME object every read, and writing to it cannot reach the slot the
 * algorithms of §20-§34 consult. Modelling the pair as one value answers `key.algorithm === key.algorithm`
 * correctly and then lets `key.algorithm.name = 'AES-CBC'` change which cipher the key IS.
 *
 * WHAT [[handle]] IS AND WHERE THE BYTES LIVE. §13.3's seventh slot holds "whatever data the underlying
 * cryptographic implementation uses to represent a logical key", and for a §31 HMAC key that is the byte
 * sequence §31.6.4 step 9 names. IT IS A JS ArrayBuffer IN THE SLOT RECORD, never a malloc'd C buffer behind
 * JS_SetOpaque, and CLAUDE.md §PLATFORM-DATA states the rule while §5.2 Key Storage states the use that forces
 * it: a key is handed to IndexedDB, held in a page's closure and read back in another turn, so its bytes must
 * FORK per flow and PARK to the cold tier with the flow that holds them. A property write is already captured
 * by the per-flow COW delta and the snapshot machinery already carries a JS value; a C pointer captured as a
 * pointer reverts on a context switch and leaves the bytes reachable from nothing — a leak the runtime's own
 * GC walk cannot see, so no gate reports it. Being a JS value costs the key nothing in opacity: the record it
 * hangs off is keyed by a private Symbol the page never receives, so §13.1's "opaque reference to keying
 * material" holds exactly as it does for the other six slots.
 *
 * §13.5's SERIALIZATION AND DESERIALIZATION STEPS ARE BUILT, AS A ROW OF HTML §2.7.1's REGISTRY — the ten
 * steps are in crypto_key.c beside the mint whose slots they read, and the seam they are a row of is in
 * core/structured_clone.h. The NAMED RESIDUAL that stood here is retired by that landing, which is the
 * condition it stated; TWO OF ITS FINDINGS SURVIVE IT and are kept, because each is one a reader re-derives
 * from the nearest thing that looks like it:
 *   - A CLAUSE WRITTEN IN ONE FILE REASONS FROM WHERE ITS AUTHOR IS STANDING. This residual twice named the
 *     wrong file as the one that had to change — first claiming a clone could silently lose the handle (a
 *     platform object with no encoding is REFUSED, not dropped), then that the ten steps were ones
 *     core/structured_clone.c would `route`, when that file hands the whole value to the engine's own writer
 *     and owns only which values are refused. The file that decided was engine/qjs/quickjs.c's per-class
 *     dispatch, which core/indexeddb/idb_key_path.c had already said in as many words, about Blob, in a
 *     component the clause's author never opened.
 *   - THE SEAM AND THE ROW LANDED AS ONE COMMIT, and that is §NO STUBS rather than convenience: a registry
 *     with no row is a write with no reader, and a row whose steps are a shape makes structuredClone appear
 *     to succeed and hand back something that is not the key.
 *
 * §13.5 CARRIES FIVE OF §13.3's SEVEN SLOTS, NOT SEVEN, and neither of its lists names a CACHED slot — so §9
 * Terminology's two cached objects are re-minted in the target realm out of the deserialized ones, which is
 * what keeps one key's `algorithm` from being another key's. crypto_key.c states that at the steps.
 *
 * WHAT CAN SCORE IT is WebCryptoAPI/serialization/aes-gcm.https.any.js and, today, nothing else in that
 * directory: its nineteen documents share one META script that calls crypto.subtle.generateKey and then
 * crypto.subtle.exportKey, and AES-GCM is the one algorithm with a row in BOTH of those operations'
 * registries — `hmac.https.any.js` still takes §18.4.4's refusal at generateKey, and every other vector names
 * an algorithm neither registry has. ONE document and three subtests (the META script asks for AES-GCM at 128,
 * 192 and 256 bits), which is enough to score a seam and not enough to read as coverage. The cheaper witness
 * this file used to offer instead — a raw HMAC key imported, cloned, signed with both and compared, as a
 * fixture statement, because no collected document does it — is still not written, and is still the fallback
 * for the day the collected document cannot be run.
 *
 * §13.2's TWO ENUMS ARE C ENUMS AND THE USAGES ONE IS A BITMASK, which is not a compression of §13.3's
 * "Sequence<KeyUsage>" but a faithful model of it: §9 Terminology defines the "usage intersection" of two
 * sequences as "a sequence containing each recognized key usage value that appears in both a and b, IN THE
 * ORDER LISTED IN the list of recognized key usage values", and [[usages]] is always "the normalized value of
 * a usages list", i.e. that intersection against every recognized value. A normalized sequence is therefore
 * exactly a SET drawn from a fixed ordered list, with duplicates removed and the order fixed — which is a
 * bitmask, and the ECMAScript Array §3.2.21 Sequences — sequence< T > builds from it is the mask expanded in
 * that same order. Modelling it as an array of strings would let a value be stored that normalization cannot
 * produce, and every §14 method's "if usages contains an entry which is not …" test would then be a string
 * walk over data with two spellings of the same fact. */
#ifndef ENGINE_HOST_BROWSER_CORE_CRYPTO_CRYPTO_KEY_H
#define ENGINE_HOST_BROWSER_CORE_CRYPTO_CRYPTO_KEY_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"

/* §13.2 Key interface data types: "The recognized key type values are "public", "private", and "secret"." The
   order is the `enum KeyType` declaration's, which is what `type` answers with. */
typedef enum {
    CRYPTO_KEY_TYPE_PUBLIC = 0,
    CRYPTO_KEY_TYPE_PRIVATE,
    CRYPTO_KEY_TYPE_SECRET,
    CRYPTO_KEY_TYPE_N
} CryptoKeyType;

/* §13.2's "list of recognized key usage values", in the order §9 Terminology's usage intersection lists them —
   which is the `enum KeyUsage` declaration order and therefore the order `usages` reports. One bit each. */
typedef enum {
    CRYPTO_KEY_USAGE_ENCRYPT     = 1u << 0,
    CRYPTO_KEY_USAGE_DECRYPT     = 1u << 1,
    CRYPTO_KEY_USAGE_SIGN        = 1u << 2,
    CRYPTO_KEY_USAGE_VERIFY      = 1u << 3,
    CRYPTO_KEY_USAGE_DERIVE_KEY  = 1u << 4,
    CRYPTO_KEY_USAGE_DERIVE_BITS = 1u << 5,
    CRYPTO_KEY_USAGE_WRAP_KEY    = 1u << 6,
    CRYPTO_KEY_USAGE_UNWRAP_KEY  = 1u << 7
} CryptoKeyUsage;
/* Every recognized value, which is the second operand of §9's "normalized value of a usages list". */
#define CRYPTO_KEY_USAGES_ALL 0xFFu

/* §14.1 Data Types' `enum KeyUsage`, AS ITS EIGHT STRINGS, NULL-terminated — the SAME list in the SAME order as
 * the bit enum directly above, so `CRYPTO_KEY_USAGE_NAMES[k]` is the name of bit `1u << k` and nothing anywhere
 * has to restate that pairing.
 * IT LIVES BESIDE THE BITS BECAUSE IT IS THE SAME FACT. It was a file static in core/crypto/subtle_crypto.c,
 * where §14.3.9's `sequence<KeyUsage>` position declares it to Web IDL §3.2.18 — and the day a SECOND reader
 * needed it (§31.6.4 Import Key the jwk arm's step 8 compares the jwk `key_ops` field against the requested usages by
 * NAME) the choice was between exporting the one list and writing a second copy of eight strings whose ORDER
 * is load-bearing in release. RFC 7517 §4.3 "key_ops" (Key Operations) Parameter says the two vocabularies are
 * deliberately one — "Note that the "key_ops" values intentionally match the "KeyUsage" values defined in the
 * Web Cryptography API specification" — so the jwk reader and the IDL conversion are reading one list. */
extern const char *const CRYPTO_KEY_USAGE_NAMES[];

/* Declared ONCE PER AGENT; the per-realm install registers itself through core/realm.h. */
void crypto_key_init(JSContext *ctx);
void crypto_key_free(void);

/* MINT A CryptoKey WITH ITS SLOTS SET — what §14.3.6's generateKey, §14.3.7's deriveKey, §14.3.9's importKey
   and §14.3.12's unwrapKey each end in. There is no constructor: §13's IDL declares none, so the interface
   object's [[Construct]] throws and this is the only way one comes into existence.
   `algorithm` is the [[algorithm]] slot — the KeyAlgorithm (§12) or the derivation of one that the minting
   algorithm built, e.g. §31.4's HmacKeyAlgorithm — and is CONSUMED. It is never handed to the page: §13.4's
   `algorithm` answers with §9's cached ECMAScript object, which this builds beside it.
   `usages` is §9's normalized value as a mask of CryptoKeyUsage bits.
   `handle` is §13.3's [[handle]] — the keying material, as an ArrayBuffer — and is CONSUMED. It is a required
   parameter and not an optional one: §13.3 declares the slot on every CryptoKey, so a mint that could omit it
   is a mint that can produce a key with no key in it, and the first algorithm to read one would find a hole
   where a default would otherwise hide it. */
JSValue crypto_key_new(JSContext *ctx, CryptoKeyType type, bool extractable, JSValue algorithm,
                       uint32_t usages, JSValue handle);

/* §13.3's [[handle]], as the ArrayBuffer the mint was given. OWNED: the caller frees. Read by the §20-§34
   operation that uses the key — §31.6.1 Sign's "the key represented by the [[handle]] internal slot of key". */
JSValue crypto_key_handle(JSContext *ctx, JSValueConst key);

/* §13.3's [[algorithm]] — the SLOT and not §13.4's cached ECMAScript object, which is the distinction the file
   comment above is entirely about. OWNED: the caller frees. Read by §14.3.3 step 9 and §14.3.4 step 10 ("the
   name attribute of the [[algorithm]] internal slot of key") and by each algorithm's own operation. */
JSValue crypto_key_algorithm(JSContext *ctx, JSValueConst key);

/* §13.3's [[usages]], as §9's normalized mask — what §14.3.3 step 10's "does not contain an entry that is
   \"sign\"" asks about. */
uint32_t crypto_key_usages(JSContext *ctx, JSValueConst key);

/* §13.3's [[extractable]], as the boolean the mint was given. TWO STEPS OF ONE METHOD READ IT: §14.3.10 step 7
   ("If the [[extractable]] internal slot of key is false, then throw an InvalidAccessError") and the `ext`
   sub-step of every export operation's jwk arm, which §29.4.5 and §31.6.5 state word for word — "Set the ext
   attribute of jwk to equal the [[extractable]] internal slot of key".
   IT IS THE SLOT AND NOT §13.4's MEMBER, exactly as the three readers above are. The two agree by construction
   — §13.4's `extractable` "Reflects the [[extractable]] internal slot" and js_ck_get reads this same slot —
   and an algorithm that went through the member would be reading an accessor off the prototype rather than
   state the brand protects. */
bool crypto_key_extractable(JSContext *ctx, JSValueConst key);

/* §9's "normalized value of a usages list" AS Web IDL §3.2.21 Sequences' sequence<KeyUsage> — a new Array
   carrying the recognized usages the mask names, "in the order listed in the list of recognized key usage
   values".
   IT IS DECLARED BECAUSE §13.4's `usages` AND AN EXPORT'S `key_ops` ARE ONE WALK AND NOT TWO. §29.4.5's and
   §31.6.5's jwk arms each read "Set the key_ops attribute of jwk to equal the usages attribute of key", so
   what an export writes must be what §13.4 would answer with — and a second loop over the same mask in
   another file is the second copy of a fact, which is the copy that drifts. The ORDER is the load-bearing
   part: a jwk round trip compares `key_ops` as a stringified array, so two walks agreeing on membership and
   disagreeing on order would be a defect no membership test could see. */
JSValue crypto_key_usages_sequence(JSContext *ctx, uint32_t usages);

/* THE CLASS §3.2.15 Interface types' BRAND TESTS AGAINST for a `CryptoKey key` argument position — what
   idl_iface_brand is given by every §14.3 method that declares one. It is this component's fact: the class is
   what cannot be forged, because §13.3's slots live in an own property anything could be given. */
JSClassID crypto_key_class(void);

#endif
