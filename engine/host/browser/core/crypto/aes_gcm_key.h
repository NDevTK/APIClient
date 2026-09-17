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
   see the file comment. Returns JS_EXCEPTION with the exception pending, like every algorithm here. */
JSValue aes_gcm_import_key(JSContext *ctx, const char *format, JSValueConst key_data, bool extractable,
                           uint32_t usages);

#endif
