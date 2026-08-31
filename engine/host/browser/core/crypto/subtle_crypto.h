/* Web Cryptography API §14 SubtleCrypto interface — and, of its twelve methods, the FOUR this engine performs.
 *
 *   [SecureContext,Exposed=(Window,Worker)]
 *   interface SubtleCrypto {
 *     …
 *     Promise<ArrayBuffer> sign(AlgorithmIdentifier algorithm, CryptoKey key, BufferSource data);
 *     Promise<boolean> verify(AlgorithmIdentifier algorithm, CryptoKey key, BufferSource signature,
 *                             BufferSource data);
 *     Promise<ArrayBuffer> digest(AlgorithmIdentifier algorithm, BufferSource data);
 *     …
 *     Promise<CryptoKey> importKey(KeyFormat format, (BufferSource or JsonWebKey) keyData,
 *                                  AlgorithmIdentifier algorithm, boolean extractable,
 *                                  sequence<KeyUsage> keyUsages);
 *     …
 *   };
 *
 * THE ORDER THESE WERE BUILT IN IS THE ORDER THEIR DEPENDENCIES ALLOW, stated so the audit's remaining list
 * reads as a plan rather than as neglect. §14.3.5's digest was first because it is the one method whose whole
 * answer is a computation over bytes: it takes no CryptoKey, mints none, and reaches neither §18.4's registry
 * beyond the four SHA rows nor the ASN.1/JWK import and export formats §20-§34 define. Every other method
 * stands on §13's CryptoKey, whose interface and internal slots are core/crypto/crypto_key.c — and what each
 * still needs beyond it is an ALGORITHM. §31 HMAC is the only algorithm of §20-§34 that needs no primitive this
 * engine lacks, because FIPS 198-1 §4 is a CONSTRUCTION over the message digest already here (core/crypto/
 * hmac.h walks the ladder), so `importKey`, `sign` and `verify` follow it and nothing else can.
 *
 * WHAT REMAINS ABSENT, AND WHY IT IS NOT AN ORDERING ANYONE CHOSE. `encrypt`, `decrypt`, `generateKey`,
 * `deriveKey`, `deriveBits`, `exportKey`, `wrapKey` and `unwrapKey` are absent — the page's own TypeError names
 * each, and engine/idlgen.mjs's audit prints the list. Every remaining algorithm behind them (AES, RSA,
 * ECDSA/ECDH, X25519/Ed25519) needs a field or bignum layer this engine does not have and cannot bind to, so
 * they are a different and larger piece of work rather than the next one. The two of them that HMAC alone could
 * reach are §31.6.3's Generate Key, which needs §10.1.1's random source spent on key material, and §31.6.5's
 * Export Key, whose "raw" arm is small and whose "jwk" arm needs the same JSON Web Key layer §31.6.4's jwk arm
 * does (named as a residual at hmac.h's `hmac_import_key`).
 *
 * WHAT DIGEST IS FOR IN THIS ENGINE, WHICH IS TWO THINGS AND NEITHER IS OPTIONAL. Real bundles call it: an
 * IDL triage over this corpus found `crypto.subtle.digest` referenced by two bundles across twenty call sites,
 * every one of them reaching a `crypto` that resolved to nothing. And CSP §6.7.3.3 step 5.2.2 needs the same
 * primitive to decide whether a page's own inline `<script>`/`<style>` runs — see core/frame/csp_source_list.c,
 * whose hash arm used to crash naming exactly this.
 *
 * THE AUDIT'S ABSENT LIST IS LONGER THAN §14 IS, AND THAT IS THE SNAPSHOT AND NOT THE ENGINE. engine/idlgen.mjs
 * reads @webref/idl, which tracks editor's drafts, and §14's IDL there carries six members the published
 * standard does not contain at all: `encapsulateKey`, `encapsulateBits`, `decapsulateKey`, `decapsulateBits`
 * and `getPublicKey` appear ZERO times in the specification's text, and `supports` appears only in prose (a
 * registered algorithm "supports the export key operation") and never as an interface member. §14's own IDL
 * block declares TWELVE operations, §14.3.1 through §14.3.12, and that is the inventory this component owes.
 * The number is written here rather than in the auditor's map for the reason that map states: a count in prose
 * is what a row makes redundant, and what is durable is WHICH members are ahead of the standard and how a
 * reader confirms it — open the standard at §14 SubtleCrypto interface and count the operations in its IDL.
 *
 * §14.2's CRYPTO TASK SOURCE IS WHY THE RESOLUTION IS A JOB. "This task source is used to queue tasks to
 * resolve or reject promises created in response to calls to methods of SubtleCrypto"; §14.3.5's steps 7-11
 * return the promise, compute in parallel, and then "queue a global task … to perform the remaining steps".
 * So the settle is enqueued rather than performed, and a `Promise.resolve().then(…)` written after the digest
 * call runs FIRST — which is what real Chrome does and is observable in three lines.
 *
 * §14.4's EXCEPTIONS, over the four methods that exist. A "NotSupportedError" DOMException from §18.4.4 for an
 * algorithm name no row registers, at all four. An "InvalidAccessError" from §14.3.3 step 9 / step 10 and
 * §14.3.4 step 10 / step 11, for a key minted for another algorithm or without the usage the call needs. A
 * "DataError" and a "SyntaxError" from §31.6.4's steps 1, 3, 7 and 8, plus §14.3.9 step 10's SyntaxError for a
 * secret key imported with no usages. And an "OperationError" for an operation that FAILS, which none of these
 * four can: they are pure functions over bytes with no device behind them, so that exception is asserted
 * against rather than written. */
#ifndef ENGINE_HOST_BROWSER_CORE_CRYPTO_SUBTLE_CRYPTO_H
#define ENGINE_HOST_BROWSER_CORE_CRYPTO_SUBTLE_CRYPTO_H

#include "quickjs.h"

/* Declared ONCE PER AGENT; the per-realm install registers itself through core/realm.h. */
void subtle_crypto_init(JSContext *ctx);
void subtle_crypto_free(void);

/* THIS REALM'S `[SameObject]` SubtleCrypto — what §10.2.1's `subtle` answers with. OWNED: the caller frees.
   It is reached through this function rather than through a slot core/crypto/crypto.c also knows about,
   because which object a realm's SubtleCrypto IS is this component's fact and not that one's. */
JSValue subtle_crypto_object(JSContext *ctx);

#endif
