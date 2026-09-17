/* Web Cryptography API §14 SubtleCrypto interface — and, of its twelve methods, the EIGHT this engine
 * performs. THAT NUMBER IS A COUNT AND IT HAS BEEN WRONG HERE, which is why it is followed by the derivation
 * rather than trusted: it read FOUR for as long as `encrypt`, `decrypt` and `exportKey` had been landed, and a
 * count in prose goes stale the turn after somebody does the work. Ask the installer instead —
 *     `grep -c idl_install_method_exposed core/crypto/subtle_crypto.c`
 * over subtle_crypto_install_realm, which is the one place a member becomes performed.
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
 * WHAT REMAINS ABSENT, AND WHY IT IS NOT AN ORDERING ANYONE CHOSE. `deriveKey`, `deriveBits`, `wrapKey` and
 * `unwrapKey` are absent — the page's own TypeError names each, and engine/idlgen.mjs's audit prints the list.
 * THIS LIST HELD `encrypt`, `decrypt`, `generateKey` AND `exportKey` AND IS REWRITTEN RATHER THAN SHORTENED,
 * because a list of ABSENCES is read by exactly one population — people who have come to build one of the
 * entries — so it is wrong the moment it is useful and wrong in the direction that makes the work look bigger.
 * What is durable is the DERIVATION and the SHAPE: run `node engine/idlgen.mjs` for today's set, and expect the
 * missing ones to be the members whose algorithms need a field or bignum layer rather than members nobody got
 * to. The one of them that HMAC alone could reach is §31.6.3's Generate Key — a second registry row on the
 * `generateKey` that now exists, needing nothing built, and NOT the next diff for the reason the landing order
 * below gives: zero sites in this corpus call it. Its sibling is §31.6.5's Export Key,
 * whose "raw" arm is small and whose "jwk" arm stands on the same JSON Web Key layer §31.6.4's jwk arm does.
 * THAT LAYER IS core/crypto/jwk.c AND THIS SENTENCE USED TO POINT AT A RESIDUAL INSTEAD — "named as a residual
 * at hmac.h's `hmac_import_key`" — which was true when written, went stale the day that arm landed, and is
 * kept in its own words because a pointer to a retired residual is the one stale claim nothing mechanical can
 * see: the retirement note is written at the site that retired it, and nothing links back to a file that cited
 * it by name. An Export Key jwk arm is a DIFFERENT direction over the same layer — it ENCODES where these
 * DECODE — so what it inherits is the vocabulary and the base64url alphabet, not these functions.
 *
 * THIS PARAGRAPH USED TO GO ON "Every remaining algorithm behind them (AES, RSA, ECDSA/ECDH, X25519/Ed25519)
 * needs a field or bignum layer this engine does not have and cannot bind to, so they are a different and
 * larger piece of work rather than the next one", AND IT IS REWRITTEN RATHER THAN DELETED BECAUSE A READER WHO
 * RE-DERIVES IT FROM HMAC'S LADDER WILL RE-ADD IT. It holds for RSA, for ECDSA/ECDH and for X25519/Ed25519. It
 * is FALSE of AES, and it was made false inside this component: core/crypto/aes.c computes FIPS 197's CIPHER()
 * and core/crypto/aes_gcm.c walks SP 800-38D §7.1 and §7.2, with no bignum, no curve and no ASN.1 between them
 * and a working §29.4.1 "Encrypt" — aes_gcm.h argues that ladder in full and this sentence went on denying it.
 * THE DIRECTION IS WHY IT IS WORTH A PARAGRAPH RATHER THAN A DELETION: a claim that something is out of reach
 * is read by one population, people deciding what to build next, and obeying it means NOT LOOKING — so nothing
 * about it is ever discovered by acting on it, which is the failure CLAUDE.md rates as the silent one. hmac.h
 * held the same sentence and is rewritten with this one. RETIREMENT: this note goes when no sentence in this
 * component says an unbuilt algorithm stands behind a primitive the component already holds.
 *
 * SO AES-GCM IS NEXT, AND ITS MEMBERS DO NOT ARRIVE IN THE ORDER §14.3 LISTS THEM. They arrive in the order
 * real pages CHAIN them, and that order is what decides what a landing is worth. An AEAD is a TRANSPORT, so a
 * page's key nearly always crosses a boundary — a worker, an IndexedDB record, a shared link — and the call
 * that mints the CryptoKey the sink actually receives is then an import or an export arm rather than §29.4.3
 * "Generate Key". That is structural rather than a census: it follows from what an authenticated cipher is
 * FOR. Derive today's set instead of trusting this sentence — run
 *     `grep -rloE 'subtle\.(encrypt|decrypt|generateKey|importKey|exportKey)' testing/corpus/mirror`
 * and read each hit's CONTINUATION, because the member a site calls first is not the member its next line
 * needs. A decomposition drawn from §14.3's list instead installs an arm no site calls while leaving every site
 * dying one call earlier, which is CLAUDE.md §AND-THE-SAME-MEASUREMENT-DECIDES-THE-UNIT.
 *
 * THE LANDING ORDER, NUMBERED BY WHAT HAS A CONSUMER AND NOT BY WHAT DEPENDS ON WHAT. (1) §29.4.4 "Import
 * Key"'s "raw" arm with its §18.4.4 row, on the `importKey` that ALREADY EXISTS — it flips no feature detect by
 * construction, since the member is installed either way, and it mints the key every other AES entry point
 * takes. THAT ONE IS BUILT WHOLE (core/crypto/aes_gcm_key.c): §29.4.4's step 2 holds three
 * sibling lists and ALL THREE are performed. THIS SENTENCE USED TO SAY THE `jwk` ARM WAS NOT, and named the
 * DFAIL it reached as the record of what to build. The crash and its next-diff clause are both gone, and this
 * is rewritten rather than deleted because the clause was WRONG in a way worth one sentence: it described
 * §29.4.4's `alg` sub-step as a three-way match of "A128GCM"/"A192GCM"/"A256GCM" against the length, where
 * the standard's FOURTH clause is an Otherwise that throws and is the arm's only length test, and it called
 * the `use` sub-step word for word §31.6.4's, where this chapter's value is "enc" and §31.6.4's is "sig" — so
 * building to it would have admitted a key of any length AND refused the one `use` a real AES-GCM JSON Web Key
 * carries. Both halves were checkable against the fetched document in one command, and neither was checked. (2) §29.4.1 "Encrypt" and §29.4.2 "Decrypt" as ONE landing: they share §29.3's AesGcmParams, the byte
 * copy and the whole §7.1/§7.2 walk, so splitting them is churn and not decomposition. (3) §29.4.3 "Generate
 * Key" with §14.3.6 "The generateKey method" and the route to §10.1.1's stream, AS ONE LANDING — BUILT, and
 * the three could not be split for §A-FIELD-A-CONSUMER-DEFAULTS' reason: the route is a write with no reader
 * until the algorithm calls it, and the algorithm has no caller until the method is installed. THE FEATURE
 * DETECT THAT LANDING FLIPS WAS PRICED FIRST and subtle_crypto.c's §14.3.6 banner states the derivation; what
 * it turned on is that the branch the guard selects calls `digest`, `importKey` and `sign` over SHA-1 and
 * SHA-256, all three of which were already registered for those algorithms. (4) §29.4.5 "Export
 * Key" on the §14.3.10 "The exportKey method" that ALREADY EXISTED — BUILT, and it was a registry ROW plus
 * this chapter's own `alg` sub-step, exactly as the exportKey machine's next-diff clause had predicted. THIS
 * LINE SAID core/crypto/jwk.c EXISTS "in the DECODING direction only", and the ENCODING direction is what that
 * landing used: `jwk_oct_export` was already there, written when §31.6.5 landed and diffed against §29.4.5
 * rather than assumed to match it, so the arm cost one call and one `alg` selection. A reader who takes the
 * decoding-only clause at face value will re-derive an encoder this directory already has. A key that crosses
 * IndexedDB additionally needs §13.5 "Serialization and deserialization steps", and THIS LINE USED TO SAY THAT
 * subproblem `belongs to core/crypto/crypto_key.c` — which was half of it, and THEN that HTML §2.7.1
 * `Serializable objects` `has NO arm in this engine`, which was true when written. Both are kept because each
 * is one a reader re-derives: the decomposition was real, the seam it named was landed WITH §13.5's row in one
 * commit for §NO STUBS' reason, and §13.5 IS now a row of core/structured_clone.h's registry, registered from
 * crypto_key_init. A lane briefed off either sentence today would go looking for work that is done.
 *
 * ADDING A ROW TO ONE OF THOSE REGISTRIES IS SAFE FOR A REASON WORTH NOT UNDOING. Each method's normalization
 * forks over its own registry and names the NOT-REGISTERED arm SYMBOLICALLY — `arm == <X>_REGISTERED_N` at all
 * four fork sites — so a new row moves that arm's ordinal and nothing at the site has to be kept in step with
 * it by hand. A literal written THERE would be an ordinal over a set this component edits —
 * CLAUDE.md §AN-INDEX-NAMES-A-THING-ONLY-WHILE-THE-SET-IS-FIXED, one revision apart, not one call apart.
 *
 * THIS PARAGRAPH SAID `by the registry's computed length rather than by a literal`, AND THAT WAS TRUE OF THE
 * SITES AND FALSE OF TWO OF THE THREE REGISTRIES — rewritten rather than deleted, because the two halves are
 * one sentence apart and a reader who checks the SITES finds the claim confirmed and stops. What the sites
 * spell symbolically is a NAME; what that name is DEFINED as is the other half, and `digest` alone defined it
 * over a table (`COUNTOF(SD_REGISTERED)`) while `sign`/`verify` and `importKey` each defined it as a bare `1`
 * with no table to count. A bare `1` is not wrong — it is the whole statement of a one-row set — but it is not
 * what this paragraph promised, and the promise is the thing a reader adding a row relies on.
 * `importKey` now has its table and its length is counted from it. `sign`/`verify`'s `SV_REGISTERED_N` is
 * still the literal, and no row is queued behind it. Whether the claim was WRONG WHEN WRITTEN or went stale is
 * NOT ESTABLISHABLE FROM THIS CHECKOUT, which is a shallow clone — `git log -S` at a graft answers with the
 * graft commit for every pre-existing line, so the honest statement is that the disagreement predates it.
 * RETIREMENT: this note goes when every `*_REGISTERED_N` in the component is counted from a table the fork
 * numbers, so the sentence above cannot be half-true again.
 *
 * THE RESIDUAL THAT STOOD HERE IS RETIRED AND ITS ARGUMENT IS KEPT, because a reader who re-derives the
 * problem will re-invent the wrong answer to it. It read: §10.1.1's stream is reached from a Crypto and §29.4.3
 * runs on a SubtleCrypto, core/crypto/crypto.c draws through a file-static helper whose first argument is the
 * Crypto OBJECT — the draw position being latched into the running flow's COW delta AGAINST that object, which
 * is what makes two forked arms mint DIFFERENT key material rather than the same bytes twice — and crypto.h
 * exported only `crypto_init` and `crypto_free`, so §29.4.3 had no source it might use. Its NEXT DIFF clause
 * said to build the route from a SubtleCrypto to its realm's Crypto and draw through the SAME object the
 * getter latches on, and that is exactly what `crypto_random_bytes` is: it reads the per-realm slot the
 * `crypto` getter reads, so one stream serves both members and neither can rewind the other.
 * WHAT THE ARGUMENT STILL FORBIDS, once the route exists: any SECOND source. A member that built its own
 * record, or latched on anything but that object, would leave two flows forked above a `generateKey` holding
 * byte-identical key material — a solver defect and not a cryptographic one, observable as two arms that
 * cannot be told apart downstream of the key rather than as any wrong ciphertext. crypto.h states that at the
 * entry, which is where a reader reaching for a shortcut will be standing.
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
 * "DataError" and a "SyntaxError" from §31.6.4's steps 1, 3, 7 and 8 and from §29.4.4's step 1 and its `raw`
 * arm's second item, plus §14.3.9 step 10's SyntaxError for a
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
