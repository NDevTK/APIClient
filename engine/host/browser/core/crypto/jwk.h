/* Web Cryptography API — the JSON Web Key sub-steps that §29.4.4 Import Key and §31.6.4 Import Key WRITE WORD
 * FOR WORD, as one component rather than as two copies.
 *
 * THIS IS NOT A UTILITY FILE AND THE DISTINCTION IS THE WHOLE OF WHY IT EXISTS. What is here is exactly the
 * run of sub-steps whose SENTENCES ARE IDENTICAL in the two chapters, established by reading both from the
 * fetched document rather than by noticing that they look alike — and the run STOPS at the first sentence that
 * is not. Each chapter keeps everything else, including the two sub-steps that merely RESEMBLE each other.
 *
 * THE DIFF, MEASURED, because a claim that two algorithms agree is the one claim a reader cannot check without
 * fetching both. §29.4.4's `jwk` arm has EIGHT sub-steps and §31.6.4's has NINE; the extra one is §31.6.4's
 * step 5, "Set the hash to equal the hash member of normalizedAlgorithm", which AES-GCM has no member for.
 *   IDENTICAL, and therefore here:
 *     the arm's step 1  — "If keyData is a JsonWebKey dictionary: Let jwk equal keyData. Otherwise: Throw a DataError."
 *     the arm's step 2  — "If the kty field of jwk is not \"oct\", then throw a DataError."
 *     the arm's step 3  — "If jwk does not meet the requirements of Section 6.4 of JSON Web Algorithms [JWA], then throw a DataError."
 *     the arm's step 4  — "Let data be the byte sequence obtained by decoding the k field of jwk."
 *     the `key_ops` sub-step (§29.4.4's step 7, §31.6.4's step 8)
 *     the `ext` sub-step     (§29.4.4's step 8, §31.6.4's step 9)
 *   ONE WORD APART, and therefore here as a PARAMETER rather than as two functions:
 *     the `use` sub-step (§29.4.4's step 6, §31.6.4's step 7) — "If usages is non-empty and the use field of
 *     jwk is present and is not \"enc\", then throw a DataError" for AES-GCM and "\"sig\"" for HMAC. ONE VALUE
 *     DIFFERS AND NOTHING ELSE DOES, so it is the argument.
 *   DIFFERENT IN STRUCTURE, and therefore NOT here at all:
 *     the `alg` sub-step (§29.4.4's step 5, §31.6.4's step 6). Both chapters END in the same leaf — "If the alg
 *     field of jwk is present, and is not \"X\", then throw a DataError" — and they SELECT X differently and
 *     have different Otherwise arms. §29.4.4 selects on the length in bits of data, with three clauses and a
 *     fourth reading "Otherwise: throw a DataError"; §31.6.4 selects on the name attribute of hash, with four
 *     clauses and a fifth that defers to another applicable specification. Only the LEAF is shared, and that is
 *     what `jwk_alg_is` is. A helper parameterised over the selection would be one predicate answering two
 *     questions, and the arm §29.4.4 would lose is the one that carries ALL of its length validation.
 *
 * WHY THE SPLIT IS AT `alg` AND WHY THAT MAKES THE ORDER SAFE. In BOTH chapters the `alg` sub-step sits between
 * the decode and the `use` sub-step, so a caller that calls `jwk_oct_key_bytes`, then performs its own `alg`
 * sub-step, then calls `jwk_oct_tail` has performed the arm in the standard's own order and could not have
 * performed it in any other — the order is carried by the call sequence rather than by a comment asking for it.
 *
 * A SUB-NUMBER IS NEVER WRITTEN BARE FOR EITHER ARM. Both chapters' format dispatch is a step holding THREE
 * sibling lists, each restarting at 1, so a bare "step 2.2" or "step 5.4" names three different steps and a
 * reader cannot tell which. Every citation here names the arm in the spec's own words, and where a sub-step's
 * NUMBER differs between the two chapters this file states BOTH — which is what a shared implementation owes
 * and is the reason the numbers appear at all.
 *
 * EVERY MEMBER READ HERE IS AN ORDINARY PROPERTY GET AND NONE OF THEM CAN SUSPEND, which is a fact about the
 * CONVERSION and not an assumption about the page. §14.3.9 declares `keyData` as `(BufferSource or JsonWebKey)`
 * and core/idl_args.h's IDL_BUFFERSOURCE_OR_DICT runs Web IDL §3.2.17's member walk AT THE CONVERSION — that
 * walk is where a page's accessor or Proxy trap runs, and where it PARKS — so what reaches an import algorithm
 * is an engine-built plain object carrying exactly the twenty declared members and nothing of the page's. A
 * step machine over these reads would be a SECOND copy of §3.2.17 performed after the conversion boundary,
 * which core/idl_args.h's own row for that type forbids by name.
 *
 * NOTHING HERE IS A `DCHECK` ON WHAT THE PAGE WROTE. A JWK's fields are page input, so every malformed one
 * takes the error the standard names — a "DataError" at each of these sub-steps — and the one assertion in the
 * file is about which ARM §14.3.9 step 4 already took, which is this engine's own logic. */
#ifndef ENGINE_HOST_BROWSER_CORE_CRYPTO_JWK_H
#define ENGINE_HOST_BROWSER_CORE_CRYPTO_JWK_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"

/* THE ARM'S STEPS 1-4, which carry the same numbers in both chapters. On success returns 0 with `*out` owning
   a malloc'd copy of the decoded key material and `*out_len` its byte count; the CALLER frees it, and frees it
   on the failure of every later sub-step too, which is why this does not also perform them.
   On failure returns -1 with the "DataError" the sub-step that refused names live in the context. */
int jwk_oct_key_bytes(JSContext *ctx, JSValueConst jwk, uint8_t **out, uint32_t *out_len);

/* THE LEAF BOTH CHAPTERS' `alg` SUB-STEP IS MADE OF: "If the alg field of jwk is present, and is not \"<want>\",
   then throw a DataError." PRESENCE is the whole of the first conjunct — an absent `alg` is permitted, which is
   JSON Web Algorithms §6.4's own "An \"alg\" member SHOULD also be present" read as the SHOULD it is.
   The CALLER decides `want`, because that is the half of the sub-step the two chapters do not share, and the
   caller also owns its own chapter's Otherwise arm. Returns 0, or -1 with a "DataError" pending. */
int jwk_alg_is(JSContext *ctx, JSValueConst jwk, const char *want);

/* THE ARM'S LAST THREE SUB-STEPS — `use`, `key_ops` and `ext` — in the standard's own order, which is
   §29.4.4's steps 6, 7 and 8 and §31.6.4's steps 7, 8 and 9. `use_value` is the one word that differs: "enc"
   for AES-GCM, "sig" for HMAC. `usages` is §9 Terminology's normalized value as a CryptoKeyUsage mask and
   `extractable` is §14.3.9's own argument; both are read by sub-steps of this run and neither is written here.
   Returns 0, or -1 with the "DataError" of whichever sub-step refused pending. */
int jwk_oct_tail(JSContext *ctx, JSValueConst jwk, const char *use_value, uint32_t usages, bool extractable);

#endif
