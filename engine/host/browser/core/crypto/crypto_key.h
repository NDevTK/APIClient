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
 * `extractable` "Reflect" their internal slots — the value IS the slot. `algorithm` and `usages` "Return the
 * cached ECMAScript object associated with" theirs, which is §9 Terminology's operation over a SECOND slot
 * ([[algorithm_cached]], [[usages_cached]]), and the difference is reachable from three lines of script: the
 * object a page gets back is the SAME object every read, and writing to it cannot reach the slot the
 * algorithms of §20-§34 consult. Modelling the pair as one value answers `key.algorithm === key.algorithm`
 * correctly and then lets `key.algorithm.name = 'AES-CBC'` change which cipher the key IS.
 *
 * WHAT [[handle]] IS AND WHY IT IS NOT HERE YET — a NAMED RESIDUAL, because this is narrower than §13.3 rather
 * than wrong. §13.3's seventh slot holds "whatever data the underlying cryptographic implementation uses to
 * represent a logical key", and nothing in this build has keying material to put in one: no §14 method mints a
 * key, so the set of CryptoKeys a page can obtain is EMPTY and a slot for their bytes would be a field with no
 * producer. THE NEXT DIFF is §14.3.9's `importKey` restricted to the "raw" arm of §31.6.4 HMAC Import Key —
 * the one import arm that needs neither §9's "parse an ASN.1 structure" (SPKI and PKCS#8) nor its "parse a
 * JWK" — and it adds [[handle]] as the byte sequence it stores together with §31.4's HmacKeyAlgorithm as
 * [[algorithm]]. ITS ABSENCE SHOWS as `crypto.subtle.importKey` being a TypeError on a missing member, which
 * is the honest ABSENT §NO STUBS asks for, and as this header's `crypto_key_new` having no caller.
 *
 * AND §13.5's SERIALIZATION IS THE SECOND RESIDUAL. The interface is `[Serializable]` and §13.5 gives the five
 * serialization and five deserialization steps, which structured_clone.c would have to route — its writer
 * currently answers a platform object it does not know with HTML §2.7's "DataCloneError" DOMException, so a
 * `structuredClone(key)` or a `postMessage(key)` throws where a browser clones. That is correct-and-narrower
 * only while no key exists; the diff that adds [[handle]] is the diff that must add §13.5 beside it, because a
 * clone that drops the handle would produce a key that is not the key. ITS ABSENCE SHOWS as that
 * DataCloneError, from a page that round-trips a key through an IndexedDB store — which is §5.2 Key Storage's
 * own stated use of this interface.
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

/* Declared ONCE PER AGENT; the per-realm install registers itself through core/realm.h. */
void crypto_key_init(JSContext *ctx);
void crypto_key_free(void);

/* MINT A CryptoKey WITH ITS SLOTS SET — what §14.3.6's generateKey, §14.3.7's deriveKey, §14.3.9's importKey
   and §14.3.12's unwrapKey each end in. There is no constructor: §13's IDL declares none, so the interface
   object's [[Construct]] throws and this is the only way one comes into existence.
   `algorithm` is the [[algorithm]] slot — the KeyAlgorithm (§12) or the derivation of one that the minting
   algorithm built, e.g. §31.4's HmacKeyAlgorithm — and is CONSUMED. It is never handed to the page: §13.4's
   `algorithm` answers with §9's cached ECMAScript object, which this builds beside it.
   `usages` is §9's normalized value as a mask of CryptoKeyUsage bits. */
JSValue crypto_key_new(JSContext *ctx, CryptoKeyType type, bool extractable, JSValue algorithm,
                       uint32_t usages);

#endif
