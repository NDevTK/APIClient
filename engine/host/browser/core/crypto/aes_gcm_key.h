/* Web Cryptography API §29 AES-GCM's KEY OPERATIONS — what an AES-GCM CryptoKey IS, as opposed to what the
 * GCM mode DOES with one. The split is the same one core/crypto/hmac.c stands on the other side of, and it is
 * the split core/crypto/subtle_crypto.c's own banner states: everything about how a promise is settled and
 * which slots §14.3.9 itself writes lives in that file, and everything about what a key of THIS algorithm is
 * lives here.
 *
 * WHY THIS IS NOT core/crypto/aes_gcm.c. That file is NIST SP 800-38D's mode as a pure C primitive — it takes
 * `const uint8_t *` and knows nothing of a JSContext, which is what lets it carry a `_Static_assert` on its own
 * context size and be walked one block per turn. §29.4's operations are Web IDL algorithms that mint and read
 * platform objects. Putting them in the primitive would give the cipher a reason to include quickjs.h, and the
 * layering here is the same one secure_hash.c and hmac.c already have.
 *
 * WHAT §29.2 Registration DECLARES, AND WHY IT IS THE LOAD-BEARING FACT FOR THE CALLER. Its table gives
 * `importKey` the Parameters `None` and the Result `CryptoKey`. §18.3 Specification Conventions says what that
 * column is: "The contents of the Parameters column for a given row will contain the IDL type to use for
 * algorithm normalization for that operation". So the desiredType §18.4.4 normalizes an AES-GCM import against
 * is the base Algorithm dictionary, whose one member is `name` — and §18.4.4's "Let dictionaries be a list
 * consisting of the IDL dictionary type desiredType and all of desiredType's inherited dictionaries, in order
 * from least to most derived" therefore has NOTHING to walk beyond the name its step 2 already read.
 *
 * THAT IS OBSERVABLE FROM THREE LINES OF SCRIPT AND IS NOT A SIMPLIFICATION. §31.2 gives HMAC's importKey row
 * the type HmacImportParams, so an HMAC import READS `hash` and `length` off the page's algorithm object — one
 * accessor or Proxy trap away from the page's own code. An AES-GCM import must read neither, so
 * `importKey("raw", buf, {name:"AES-GCM", get hash(){ ran = true; return "SHA-256"; }}, true, ["encrypt"])`
 * leaves `ran` undefined, and `importKey("raw", buf, "AES-GCM", true, ["encrypt"])` — §18.4.4's DOMString arm,
 * which builds an Algorithm with a name and nothing else — resolves where the same call naming HMAC is a
 * TypeError for the required member it has no object to find. The caller's member walk is what has to skip,
 * which is why this header states the registration rather than leaving it to be re-derived at the call.
 *
 * §27.4 AesKeyAlgorithm IS SHARED AND ITS BUILDER IS NOT EXPORTED YET. The dictionary is declared once, in the
 * AES-CTR chapter, and §28, §29 and §30 each mint one; only §29's operation exists here, so the builder is a
 * file static. WHAT IS NOT COVERED is the other three chapters' Import Key operations. WHAT THE NEXT DIFF THAT
 * needs one BUILDS is that builder as a shared export, taking the recognized algorithm name as its argument
 * exactly as the static below does. HOW ITS ABSENCE WOULD SHOW is a second `*_key_algorithm_new` appearing in
 * this directory with the same two attributes under a different chapter's name. */
#ifndef ENGINE_HOST_BROWSER_CORE_CRYPTO_AES_GCM_KEY_H
#define ENGINE_HOST_BROWSER_CORE_CRYPTO_AES_GCM_KEY_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"

/* §29.4.4 Import Key, WHOLE — the operation §14.3.9 step 9 performs when normalizedAlgorithm names AES-GCM:
   "Let result be the CryptoKey object that results from performing the import key operation specified by
   normalizedAlgorithm using keyData, algorithm, format, extractable and usages."
   `format` is §14.1's KeyFormat, already checked against the enumeration's four values by the argument
   conversion. `key_data` is what §14.3.9 step 4's Otherwise arm got a copy of the bytes of. `usages` is §9
   Terminology's normalized value of a usages list, as a CryptoKeyUsage mask.
   It takes NO normalized-parameters argument, and that absence is §29.2's `None` rather than an omission —
   see the file comment. Returns JS_EXCEPTION with the exception pending, like every algorithm here: a
   "SyntaxError" for step 1, and for step 2 a "DataError" from the `raw` arm's second item or from any of the
   `jwk` arm's eight sub-steps, or the `Otherwise`'s "NotSupportedError" for the two DER formats.
   STEP 2's THREE ARMS ARE ALL PERFORMED. The `jwk` arm's six sub-steps that §31.6.4's own `jwk` arm writes word
   for word are core/crypto/jwk.c, which states the measured diff of the two arms; the two that are this
   chapter's alone — its step 5's length-to-`alg` dispatch, whose Otherwise is this arm's entire length
   validation, and its step 6's "enc" — are in aes_gcm_key.c beside the rest of the algorithm. */
JSValue aes_gcm_import_key(JSContext *ctx, const char *format, JSValueConst key_data, bool extractable,
                           uint32_t usages);

/* §29.4.3 Generate Key, WHOLE — the operation §14.3.6 step 8 performs when normalizedAlgorithm names AES-GCM.
   `length_bits` is §27.5 AesKeyGenParams' required `length` member, which §29.2's Registration gives as this
   row's Parameters type, already converted under Web IDL §3.3.6 [EnforceRange] by the normalization; step 2's
   three-value test is the STANDARD's and is performed here, on the page's number, as a rejection.
   IT TAKES NO `format` AND RETURNS A CryptoKey AND NOT A PAIR, which is §29.2's Result column for this row
   ("generateKey … CryptoKey") rather than a narrowing: an AES key is symmetric, so §14.3.6 step 9's
   CryptoKeyPair arm is a world no registered row of this engine reaches.
   Returns JS_EXCEPTION with the exception pending: a "SyntaxError" for step 1 and an "OperationError" for
   step 2. Note that step 2's exception is NOT the `raw` import arm's "DataError" for the same three lengths —
   the two chapters name different exceptions and a page reads which one it got. */
JSValue aes_gcm_generate_key(JSContext *ctx, uint32_t length_bits, bool extractable, uint32_t usages);

/* §29.4.5 Export Key, WHOLE — the operation §14.3.10 step 8 performs when the `name` member of the key's
   [[algorithm]] slot is "AES-GCM": "Let result be the result of performing the export key operation specified
   by the [[algorithm]] internal slot of key using key and format."
   `format` is §14.1's KeyFormat, already checked against the enumeration's four values by the argument
   conversion, and `key` is a CryptoKey §14.3.10 step 7 has already found extractable.
   IT TAKES NO usages AND NO extractable ARGUMENT, and that absence is the operation's shape rather than an
   omission: both are read off the KEY — step 2's jwk arm sets `key_ops` from "the usages attribute of key" and
   `ext` from "the [[extractable]] internal slot of key" — so passing them would be a second spelling of state
   the key already carries, which is the copy that drifts.
   THE SECTION HAS THREE TOP-LEVEL STEPS, counted with list depth tracked: step 2 is ONE `<li>` holding the
   whole format switch, and a flat item count promotes that switch's arms and the jwk arm's seven sub-steps to
   peers. A citation of a "step 5" here names a step §29.4.5 does not have.
   Returns JS_EXCEPTION with the exception pending: step 2's "Otherwise: throw a NotSupportedError" for the two
   DER formats, which is the only throw this operation can reach for a key this engine minted. Step 1's
   OperationError is a DCHECK here for the reason §31.6.5's is — see aes_gcm_key.c. */
JSValue aes_gcm_export_key(JSContext *ctx, const char *format, JSValueConst key);

#endif
