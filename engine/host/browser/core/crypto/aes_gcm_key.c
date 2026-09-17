/* Web Cryptography API §29.4.4 Import Key — see core/crypto/aes_gcm_key.h for why §29's key operations are a
 * component of their own and what §29.2 Registration's `None` decides for the caller.
 *
 * §29.4.4's NINE STEPS, verbatim, because every line below is one of them:
 *   1. If usages contains an entry which is not one of "encrypt", "decrypt", "wrapKey" or "unwrapKey", then
 *      throw a SyntaxError.
 *   2. [the format dispatch — three sibling lists, see below]
 *   3. Let key be a new CryptoKey object representing an AES key with value data.
 *   4. Set the [[type]] internal slot of key to "secret".
 *   5. Let algorithm be a new AesKeyAlgorithm.
 *   6. Set the name attribute of algorithm to "AES-GCM".
 *   7. Set the length attribute of algorithm to the length, in bits, of data.
 *   8. Set the [[algorithm]] internal slot of key to algorithm.
 *   9. Return key.
 *
 * A SUB-NUMBER IS NEVER WRITTEN BARE FOR STEP 2, AND THAT IS THE STANDARD'S SHAPE RATHER THAN A HOUSE STYLE.
 * Step 2 is a DISPATCH holding THREE sibling lists — the `raw` arm's two items, the `jwk` arm's eight, and the
 * `Otherwise` — each restarting at 1, so a bare "step 2.2" names three different steps and a reader cannot tell
 * which. §31.6.4's step 5 has the identical shape and core/crypto/hmac.c states the same rule over it. Every
 * citation here therefore names the arm in the spec's own words.
 *
 * STEP 1 RUNS BEFORE STEP 2 AND THE ORDER IS OBSERVABLE. `importKey("raw", <a 17-byte view>, {name:"AES-GCM"},
 * true, ["sign"])` is step 1's SyntaxError and not step 2's `raw` arm's DataError, because "sign" is outside
 * the four usages this algorithm permits and the standard asks that first. Two different exceptions for one
 * call, and a page tells them apart with a `.catch`.
 *
 * AND §14.3.9's OWN STEPS ARE NOT HERE. Step 4's JsonWebKey/BufferSource converse test, step 10's empty-usages
 * SyntaxError and steps 11-12's [[extractable]] and [[usages]] belong to the METHOD and run in
 * core/crypto/subtle_crypto.c, which is what makes them one implementation shared with §31.6.4 rather than a
 * copy per algorithm. The mint takes `extractable` and `usages` as arguments for steps 11 and 12 rather than
 * writing them afterwards, because a CryptoKey whose slots are filled in later is a CryptoKey that briefly
 * exists with the wrong ones.
 *
 * NOTHING HERE TOUCHES THE CIPHER. §29.4.4 step 3's "a new CryptoKey object representing an AES key with value
 * data" is the BYTES and not a key schedule: §13.3's [[handle]] holds "whatever data the underlying
 * cryptographic implementation uses to represent a logical key", and core/crypto/aes.c's `aes_init` expands a
 * schedule from those bytes at the point §29.4.1 Encrypt needs one. Expanding it here would put a POD C
 * structure in a slot that must fork per flow and park to the cold tier, which is the defect
 * core/crypto/crypto_key.h's [[handle]] paragraph is entirely about. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/crypto/aes_gcm_key.h"
#include "core/crypto/crypto_key.h"
#include "core/idl_slots.h"

/* §29.4.4 step 1's four usages, as the mask §9 Terminology's normalized value already is. The standard's
   contains-an-entry-which-is-not test is then a bit OUTSIDE this set — that gloss is this file's own and is
   not in quotation marks, because a fragment of it would be attributed to whichever citation stands nearest.
   IT IS §29.4.4's OWN LIST AND NOT AES's IN GENERAL: §30.3.4 AES-KW permits only the two wrapping usages, and
   §27's and §28's Import Key permit only encrypt and decrypt, so a shared constant here would be four
   algorithms' different sentences under one name. */
#define AES_GCM_IMPORT_USAGES                                                                                 \
    ((uint32_t)(CRYPTO_KEY_USAGE_ENCRYPT | CRYPTO_KEY_USAGE_DECRYPT | CRYPTO_KEY_USAGE_WRAP_KEY |             \
                CRYPTO_KEY_USAGE_UNWRAP_KEY))

/* §29.4.4 STEPS 5-7: "Let algorithm be a new AesKeyAlgorithm", then its `name` and its `length`.
   §27.4 declares the dictionary in the AES-CTR chapter and every AES algorithm mints one:
   `dictionary AesKeyAlgorithm : KeyAlgorithm { required [EnforceRange] unsigned short length; };`, of whose
   member it says "The length member represents the length, in bits, of the key." The inherited `name` is
   KeyAlgorithm's. A null-prototype record, for core/idl_slots.h's reason: §13.4's `algorithm` hands the page a
   CACHED object built beside this one, and this one is the SLOT — nothing the page writes can reach it, and
   nothing it holds can reach the page's prototype chain. */
static JSValue aes_key_algorithm_new(JSContext *ctx, const char *name, uint32_t length_bits)
{
    JSValue alg = idl_slots_new(ctx);

    CHECK(!JS_IsException(alg), "§27.4's AesKeyAlgorithm could not be allocated");
    /* §27.4's `length` is an `unsigned short`, and step 2's `raw` arm admitted exactly three values before this
       ran — so a length outside them is this engine's own arithmetic having gone wrong rather than anything a
       page said. */
    DCHECK(length_bits == 128 || length_bits == 192 || length_bits == 256,
           "§29.4.4 step 7's length in bits is not one of the three §29.4.4 step 2's `raw` arm admits — its "
           "second item is the only writer of `data`, so the two have come apart");
    JS_SetPropertyStr(ctx, alg, "name", JS_NewString(ctx, name));
    JS_SetPropertyStr(ctx, alg, "length", JS_NewUint32(ctx, length_bits));
    return alg;
}

JSValue aes_gcm_import_key(JSContext *ctx, const char *format, JSValueConst key_data, bool extractable,
                           uint32_t usages)
{
    uint32_t byte_len = 0, length;
    const uint8_t *bytes;
    JSValue algorithm, handle;

    DCHECK(format != NULL, "§29.4.4 step 2 was given no format — §14.1's KeyFormat is a required argument of "
                           "§14.3.9 and its conversion admits exactly four strings");
    DCHECK((usages & ~(uint32_t)CRYPTO_KEY_USAGES_ALL) == 0,
           "§29.4.4 was given a usages mask with a bit outside §13.2's list of recognized key usage values — "
           "§9's normalized value is what this argument is, and this engine is the only thing that builds one");
    /* STEP 1. */
    if ((usages & ~AES_GCM_IMPORT_USAGES) != 0)
        return JS_ThrowDOMException(ctx, "SyntaxError", "%s",
                                    "an AES-GCM key may only be used to encrypt, to decrypt, to wrap a key or "
                                    "to unwrap one");
    /* STEP 2's `jwk` ARM, WHICH IS NOT BUILT. It is a crash and not a refusal because the refusal would be a
       WRONG ANSWER rather than a narrower one: §29.4.4 defines the arm, so a browser RESOLVES this call and
       anything returned here is an exception a page would never see. §NO STUBS' honest-absence argument does
       not reach it either — that argument rests on a feature-detect reading false, and the format is an
       argument rather than a name a bundle can test for.
       WHAT THE NEXT DIFF BUILDS is the arm's eight sub-steps, over the two helpers §31.6.4's own jwk arm
       already has: `hmac_jwk_get` and `hmac_jwk_k_bytes` are file statics in core/crypto/hmac.c (grepped at
       the commit this landed on), and the diff that needs them here is the diff that shares them — the `kty`,
       `use`, `key_ops` and `ext` sub-steps are word for word §31.6.4's, and only the `alg` test differs, being
       a three-way match of "A128GCM"/"A192GCM"/"A256GCM" against the length rather than a hash name. */
    if (strcmp(format, "jwk") == 0) {
        DFAIL("§29.4.4 step 2's `jwk` arm is not built — a JSON Web Key naming AES-GCM reaches here and the "
              "eight sub-steps that would decode its `k` and check its `kty`, `alg`, `use`, `key_ops` and "
              "`ext` do not exist, so this engine has no `data` to reach step 3 with. Build them beside "
              "§31.6.4's, which are the same steps over the same helpers");
        /* RELEASE: a DEFINED refusal, and one the algorithm itself can end on. A quiet return would hand
           §14.3.9 step 9 a CryptoKey-shaped nothing that its step 10 would read, which is the state a release
           arm must never leave behind; this rejects the promise and the page's own catch runs. */
        return JS_ThrowDOMException(ctx, "NotSupportedError", "%s",
                                    "this engine cannot yet import an AES-GCM key from a JSON Web Key");
    }
    /* STEP 2's `Otherwise`: "throw a NotSupportedError". §14.1's KeyFormat has four values and this is the
       standard's own answer for the two DER ones, so it is a refusal rather than a gap — an AES key is never
       in a SubjectPublicKeyInfo or a PrivateKeyInfo. */
    if (strcmp(format, "raw") != 0)
        return JS_ThrowDOMException(ctx, "NotSupportedError",
                                    "an AES-GCM key cannot be imported from the '%s' format", format);
    /* STEP 2's `raw` ARM, both items: "Let data be keyData", then "If the length in bits of data is not 128,
       192 or 256 then throw a DataError." */
    bytes = JS_GetBufferBytes(key_data, &byte_len);
    DCHECK(bytes != NULL || byte_len == 0,
           "§14.3.9 step 4's own copy of the key data is detached — nothing but this algorithm holds it, and "
           "the whole reason that step copies is that the page cannot reach these bytes");
    /* THE MULTIPLICATION CANNOT WRAP AND THE TEST DOES NOT RELY ON THAT. An ArrayBuffer's length is a
       `uint32_t`, so eight times one overflows 32 bits above 512MB — and the three admitted lengths are 16, 24
       and 32 bytes, so the byte count is compared and the bit count is derived from it rather than the other
       way round. A 536870912-byte key would otherwise compute a bit length of 0 and be admitted as 128. */
    if (byte_len != 16 && byte_len != 24 && byte_len != 32)
        return JS_ThrowDOMException(ctx, "DataError", "%s",
                                    "an AES-GCM key must be 128, 192 or 256 bits long");
    length = byte_len * 8u;
    /* STEP 3: "Let key be a new CryptoKey object representing an AES key with value data." §13.3's [[handle]]
       is those bytes, as an ArrayBuffer — core/crypto/crypto_key.h states why it is a JS value and not a
       malloc'd buffer. */
    handle = JS_NewArrayBufferCopy(ctx, bytes ? bytes : (const uint8_t *)"", byte_len);
    CHECK(!JS_IsException(handle), "§13.3's [[handle]] for an imported AES-GCM key could not be allocated");
    /* STEPS 5-7. The name is §29.4.4 step 6's own literal rather than the key the registry matched: §18.4.4
       step 5 sets algName to "the value of the matching key", so a page naming "aes-gcm" normalizes to the
       registry's spelling, and step 6 then writes the one this chapter states. */
    algorithm = aes_key_algorithm_new(ctx, "AES-GCM", length);
    /* STEPS 4, 8 AND 9, plus §14.3.9's steps 11 and 12 — see the file comment for why those two are arguments
       of the mint rather than writes after it. */
    return crypto_key_new(ctx, CRYPTO_KEY_TYPE_SECRET, extractable, algorithm, usages, handle);
}
