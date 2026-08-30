/* Web Cryptography API §14 SubtleCrypto interface — and, of its twelve methods, the ONE this engine performs.
 *
 *   [SecureContext,Exposed=(Window,Worker)]
 *   interface SubtleCrypto {
 *     …
 *     Promise<ArrayBuffer> digest(AlgorithmIdentifier algorithm, BufferSource data);
 *     …
 *   };
 *
 * WHY DIGEST WAS FIRST, stated so the audit's list reads as a plan rather than as neglect. §14.3.5 is the one
 * method whose whole answer is a computation over bytes: it takes no CryptoKey, mints none, and reaches
 * neither §18.4's registry beyond the four SHA rows nor the ASN.1/JWK import and export formats §20-§34
 * define. Every other method stands on §13's CryptoKey, whose interface and internal slots are
 * core/crypto/crypto_key.c — and what each still needs beyond it is an ALGORITHM: a key to mint means
 * §20.9.3/§27.7.3/§31.6.3's Generate Key or §31.6.4's Import Key, and a key to use means the operation itself.
 * So `encrypt`, `sign`, `importKey` and the rest are ABSENT — the page's own TypeError names each, and
 * engine/idlgen.mjs's audit prints the list — and the component they belong to is this one, which is why they
 * are named here rather than left to be rediscovered. The first of them to build is §14.3.9's `importKey`
 * restricted to the "raw" arm of §31.6.4 HMAC Import Key, because that is the one arm needing neither §9
 * Terminology's "parse an ASN.1 structure" nor its "parse a JWK".
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
 * §14.4's EXCEPTIONS are the two this method can raise: a "NotSupportedError" DOMException from §18.4.4 for an
 * algorithm name no row registers, and an "OperationError" for a digest operation that fails — which, for a
 * pure function over bytes with no device behind it, cannot happen, and is asserted rather than written. */
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
