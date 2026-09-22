/* Web Cryptography API §31 HMAC — "The HMAC algorithm calculates and verifies hash-based message
 * authentication codes according to [FIPS-198-1] using the SHA hash functions defined in this specification."
 * (§31.1 Description, verbatim.)
 *
 * WHY HMAC IS THE ALGORITHM AFTER §32.3.1's DIGEST, AND WHY IT IS THE ONLY ONE THAT COULD BE. §14's twelve
 * operations divide by what primitive they stand on. HMAC stands on a MESSAGE DIGEST and on nothing else:
 * FIPS 198-1 §4 is a CONSTRUCTION over an approved hash function H, so with core/crypto/secure_hash.c already
 * computing SHA-1/256/384/512 there is no new number theory, no ASN.1, no JWK and no field arithmetic between
 * this file and a working `sign`. THIS SENTENCE USED TO END "Every other algorithm of §20-§34 needs a bignum
 * or a curve this engine does not have and cannot bind to, so it is a different and larger piece of work", and
 * it is rewritten rather than deleted because a reader who re-derives it from THIS ladder will re-add it: AES
 * needs neither, and core/crypto/aes.c and core/crypto/aes_gcm.c arrived to prove it, so AES-GCM is the second
 * algorithm and the claim now holds only of RSA, ECDSA/ECDH and X25519/Ed25519. subtle_crypto.h carries the
 * same retirement and the landing order it implies. CLAUDE.md §Do-subproblems-IN-ORDER is what put HMAC first
 * rather than a preference, and it is not an argument that nothing could follow it.
 *
 * THE BIND-BEFORE-BUILD LADDER FOR **THIS** PRIMITIVE, WALKED RATHER THAN INHERITED. secure_hash.h reports the
 * ladder for the DIGEST and lands on rung four, the faithful spec port. HMAC's ladder is shorter and ends one
 * rung EARLIER, which is the whole reason it is affordable: rung one, the host runtime, offers nothing (neither
 * emscripten's ports nor WASI expose a MAC, and JavaScript's own `crypto.subtle.sign` is asynchronous, lives
 * outside the COW delta and would put a browser feature in the bridge); rung two, an engine intrinsic, offers
 * nothing (quickjs-ng has no keyed hash and no hash at all); rung three is where it STOPS — an EXISTING MODULE
 * of this engine, `core/crypto/secure_hash.c`, IS the H that FIPS 198-1 §4 is written over. What this file adds
 * is the nine steps of that section's Table 1 and no cryptographic primitive whatever. "Never crypto from
 * scratch" is satisfied by construction: there is nothing here a reader cannot check against nine lines of a
 * published table.
 *
 * WHAT IT IS PORTED FROM, EXACTLY. FIPS PUB 198-1 (July 2008), "The Keyed-Hash Message Authentication Code
 * (HMAC)", which §31.1 names normatively and §31.6.1 Sign and §31.6.2 Verify each reach by name ("the MAC
 * Generation operation described in Section 4 of [FIPS-198-1]"):
 *   §2.3 HMAC Parameters and Symbols — B, H, ipad, K, K0, L, opad, text
 *   §3   Cryptographic Keys         — the over-long key is hashed first
 *   §4   HMAC Specification         — the equation, and Table 1's nine steps
 * The quotations below are pasted from the fetched document. NOTE FOR ANY READER CHECKING THEM: FIPS 198-1 is
 * a NIST publication and `engine/specindex` holds no index for it, so `engine/citegen.mjs` COUNTS THIS FILE'S
 * FIPS CITATIONS and CHECKS none of them — not the number, not the title and not the quotation. That is
 * a COUNTED zero rather than a clean bill: the standard is NAMED, with its tally, on the auditor's
 * "standards seen but not indexed" census line, so this is a blind spot a reader can see the size of
 * rather than a silence. It is still why each FIPS citation here names the section's own title and quotes
 * its own words — a reader with the PDF open is the only instrument those have.
 *   THAT SCOPE READ `every citation in this file`, AND THE FILE IS THE ONE THING A COVERAGE NOTE MAY NOT NAME.
 *   It is REWRITTEN rather than deleted because the FIPS half is true and a reader who re-derives it from the
 *   census line will re-add it at the same scope. The defect is not a wording slip: an absence claim's only
 *   reader is somebody deciding whether to BUILD an instrument for the axis, so a note saying the auditor
 *   checks NOTHING here reads as an instruction to discount every finding in the file — including the ones a
 *   live channel is already making, which nothing anywhere reports. The Web Cryptography half of this file is
 *   INDEXED and JUDGED, and the split is a command rather than a number a reader has to trust:
 *   `node engine/citegen.mjs engine/host/browser/core/crypto/hmac.h` prints how many citations resolved on
 *   their own evidence, how many step references were compared, and — on its `standards seen but not indexed`
 *   line — how many were FIPS 198-1's. MEASURED at 6cc8439a it was 10 resolved of 62 read, 8 steps compared,
 *   1 quotation compared, and 4 FIPS; the quotation compared is this file's own opening one, and corrupting
 *   ONE word of it in a copy outside the tree took that run from 0 findings to 1 and named the word, so the
 *   channel is ARMED rather than merely quiet.
 *   RETIREMENT: this record goes when no coverage note in this tree names a FILE as the thing an unindexed
 *   standard leaves unchecked. `git grep -ci` over the phrase `citation in this file` is the sweep, and it is
 *   run case-insensitively because this tree writes its load-bearing statements in capitals.
 *
 * §4's ONE EQUATION, VERBATIM:  MAC(text) = HMAC(K, text) = H((K0 ^ opad )|| H((K0 ^ ipad) || text))
 *
 * IT IS TWO WALKS OF THE PAGE'S DATA AND BOTH OF THEM PARK. This is not a detail of the API below — it is the
 * reason the API is shaped the way it is, and it is CLAUDE.md §The-scheduler's requirement rather than a
 * preference. TWO of the nine steps are unbounded in the page's own input, not one: Table 1 step 2 hashes the
 * KEY when the key is longer than a block, and a key is a BufferSource of whatever size the page passes; step 5
 * appends `text`, which is the message. A one-shot `hmac(key, msg, out)` would drive both to completion inside
 * one C activation, so a `sign()` over a large buffer could not be preempted, no sibling flow would run and the
 * cooperative quantum would raise a bit nothing would poll. So the key walk and the text walk are each
 * advanced ONE FIPS §2.3 `B`-byte block per call, and the caller returns JS_STEP_YIELD between them, exactly as
 * core/crypto/subtle_crypto.c's §14.3.5 digest already walks its message. There is deliberately no one-shot
 * convenience wrapper, for secure_hash.h's reason: it would be the un-parkable spelling sitting next to the
 * parkable one, and the first caller in a hurry would take it.
 *
 * THE STATE IS PLAIN OLD DATA AND HOLDS NO POINTER AND NO JSValue. It rides a step state across suspends,
 * forks and cross-session resumes, all of which copy the state's BYTES — so a pointer here would be one
 * allocation two arms both free and a JSValue here would be a reference the copy does not count. The KEY
 * MATERIAL it reads is not in here either: it lives as §13.3's [[handle]] on the CryptoKey, which is a JS value
 * for exactly the reason CLAUDE.md §PLATFORM-DATA states — see crypto_key.h.
 *
 * ONE PROBLEM PER FILE: this file is §31's ALGORITHM. Which pages may call it, how a promise settles and what
 * §14.3's methods do around it belong to core/crypto/subtle_crypto.c, and what a CryptoKey IS belongs to
 * core/crypto/crypto_key.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_CRYPTO_HMAC_H
#define ENGINE_HOST_BROWSER_CORE_CRYPTO_HMAC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "quickjs.h"
#include "core/crypto/secure_hash.h"

/* WHERE THE NINE STEPS OF FIPS 198-1 §4 Table 1 HAVE GOT TO. A phase and not a stage: the CALLER owns the
   stages its own algorithm is written in, and this is the sub-walk's cursor inside them, in the same spirit as
   the request cursors quickjs-step.h's STEP_GOTO asserts over. */
typedef enum {
    HMAC_PH_KEY = 0,   /* Table 1 steps 1-3: K is being read, to be copied or hashed into K0 */
    HMAC_PH_TEXT,      /* Table 1 steps 4-5: K0 ^ ipad is hashed and `text` is being appended, block by block */
    HMAC_PH_DONE       /* Table 1 steps 6-9 have run; the MAC has been written out */
} HmacPhase;

typedef struct {
    /* THE ONE HASH CONTEXT, REUSED THREE TIMES — over K for step 2, then over (K0 ^ ipad) || text for step 6,
       then over (K0 ^ opad) || H(...) for step 9. The three are strictly sequential and each is finished
       before the next is initialised, so a second context would be a second thing to keep in step. */
    SecureHash h;
    uint8_t    k0[SECURE_HASH_MAX_BLOCK];      /* §2.3's K0, "the key K after any necessary pre-processing" */
    uint8_t    inner[SECURE_HASH_MAX_DIGEST];  /* Table 1 step 6's H((K0 ^ ipad) || text) */
    uint64_t   key_len;                        /* len(K) in bytes, as the caller stated it */
    uint64_t   key_taken;                      /* how much of K has been fed through hmac_key_update */
    uint8_t    alg;                            /* §2.3's H, as a SecureHashAlgorithm */
    uint8_t    key_over;                       /* §3's "a K longer than B-bytes": step 2 rather than 1 or 3 */
    uint8_t    phase;                          /* an HmacPhase */
} Hmac;

/* §2.3: "L Block size (in bytes) of the output of the Approved hash function" — the MAC's length, which
   §31.6.1 Sign returns in full. FIPS 198-1 §5 admits a TRUNCATED MAC; §31.6.1 does not ask for one and states
   no truncation at all, so this engine has no `t` and the operation has no parameter for it. */
size_t hmac_mac_size(SecureHashAlgorithm alg);

/* Table 1 steps 1-3's BRANCH, decided from len(K) before a byte is read — `key_len` is the byte length of the
   key represented by the [[handle]] internal slot, which is a fact the caller already has. The state is left in
   HMAC_PH_KEY wanting `key_len` bytes. */
void hmac_begin(Hmac *m, SecureHashAlgorithm alg, uint64_t key_len);

/* HOW MANY BYTES OF K STILL HAVE TO BE FED. Zero means the walk may end; the caller advances by at most
   hmac_block(m) bytes per turn and yields between turns, which is what makes step 2 preemptible. */
uint64_t hmac_key_left(const Hmac *m);

/* §2.3's B for this state's H — the unit both walks advance by, and FIPS §4's own unit of work. */
size_t hmac_block(const Hmac *m);

/* ONE TURN OF THE KEY WALK. `n` must be at most hmac_key_left(m); a caller passing a whole block per turn is
   what a preemptible caller does. */
void hmac_key_update(Hmac *m, const uint8_t *p, size_t n);

/* CLOSE Table 1 steps 1-3 (K0 is now complete) AND PERFORM steps 4-5's opening: K0 ^ ipad is hashed, and the
   state moves to HMAC_PH_TEXT so `text` can be appended to it. Every byte of K must have been fed, which is
   asserted rather than assumed. */
void hmac_key_end(Hmac *m);

/* ONE TURN OF THE TEXT WALK — Table 1 step 5's "Append the stream of data 'text'". Any length; a caller that
   wants a rest point between blocks passes hmac_block(m) bytes and yields, which is the whole reason this is
   not one function. */
void hmac_text_update(Hmac *m, const uint8_t *p, size_t n);

/* Table 1 steps 6-9: finish the inner hash, exclusive-or K0 with opad, prepend it to the inner digest and hash
   the result. `mac` must hold hmac_mac_size(alg) bytes; the size is passed and CHECKED rather than trusted, for
   secure_hash_finish's reason. The state is left HMAC_PH_DONE and must not be reused, which is asserted. */
void hmac_finish(Hmac *m, uint8_t *mac, size_t mac_size);

/* §31.6.2 Verify step 2: "Return true if mac is equal to signature and false otherwise. This comparison must be
 * performed in constant-time."
 *
 * CONSTANT IN THE BYTES AND NOT IN THE LENGTH, WHICH IS THE ONLY READING THAT IS BOTH ACHIEVABLE AND MEANT. The
 * LENGTH of the candidate signature is the page's own argument and is not a secret — it is `signature.byteLength`,
 * which the caller could read back itself — so a length mismatch answers immediately. What must not leak is
 * WHERE two equal-length byte sequences first differ, because that is an oracle for forging a MAC one byte at a
 * time, so equal-length inputs are compared by accumulating the difference of EVERY byte and testing once. */
bool hmac_mac_equal(const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len);

/* §31.3's HmacImportParams AFTER §18.4.4 Normalizing an algorithm has run over it, which is the only shape
 * §31.6.4 Import Key ever sees one in:
 *
 *   dictionary HmacImportParams : Algorithm {
 *     required HashAlgorithmIdentifier hash;
 *     [EnforceRange] unsigned long length;
 *   };
 *
 * `hash` IS ALREADY A NAME AND NOT AN IDENTIFIER, because §18.4.4 step 10's per-member walk says so for this
 * member's type by name: "If member is of the type HashAlgorithmIdentifier: Set the dictionary member on
 * normalizedAlgorithm with key name key to the result of normalizing an algorithm, with the alg set to idlValue
 * and the op set to \"digest\"". So the caller has already resolved a string or an `{name}` object down to one
 * of §32.2's four registered SHA rows, and this struct carries the row.
 *
 * `length` IS OPTIONAL AND ITS ABSENCE IS A POSITIVE STATEMENT, never a zero: §31.6.4 step 1 throws a DataError
 * for a `length` that is PRESENT and zero, and step 8 does nothing at all when it is absent. A single
 * `uint32_t length` with 0 meaning absent would make those two steps one, and they are two. */
typedef struct {
    SecureHashAlgorithm hash;
    bool                has_length;
    uint32_t            length;   /* in BITS, as §31.3 states ("the length (in bits) of the key") */
} HmacImportParams;

/* §31.6.4 Import Key, whose step 5 is a three-way dispatch on `format` — the "raw" arm, the "jwk" arm, and
 * "Otherwise: throw a NotSupportedError" for the two DER formats. THE DISPATCH IS HERE AND NOT AT THE CALLER
 * because it is a step of THIS algorithm: §14.3.9 hands the operation a format and does not decide what an
 * algorithm does with one, which is why "spki" is a NotSupportedError for HMAC and a key for §20.9.4.
 *
 * `format` is §14.1's KeyFormat as the string the argument conversion produced. `key_data` is the algorithm's
 * step 2 `keyData`, as the ArrayBuffer §14.3.9 step 4's "getting a copy of the bytes" produced; BORROWED.
 * `usages` is §9's normalized value of the usages list as a CryptoKeyUsage mask, and `extractable` is §14.3.9's
 * own parameter — this operation sets neither slot (§14.3.9 steps 11 and 12 do), but the mint takes them, so
 * they are passed through rather than written twice.
 *
 * Returns the §13 CryptoKey, or JS_EXCEPTION with the DOMException the algorithm names live in the context:
 * a "DataError" for steps 1, 7 and 8, a "SyntaxError" for step 3, a "NotSupportedError" for step 5's Otherwise.
 *
 * STEP 5's `format is "jwk"` ARM IS BUILT, AND ITS RESIDUAL IS GONE. All nine sub-steps are performed — six of
 * them in core/crypto/jwk.c, which is where they moved when §29.4.4 Import Key's own `jwk` arm was read against
 * this one and six of its eight sub-steps turned out to be these sentences word for word. THIS PARAGRAPH SAID
 * "in hmac.c" AND IS REWRITTEN RATHER THAN DELETED: a reader who re-derives the claim from the arm's numbering
 * will look for the sub-steps in this file and conclude, from a correct grep answering nothing, that they are
 * absent. What stays here is step 5, which §29.4.4 has no member for, and step 6's per-hash selection. And the
 * three mechanisms the retired clause named landed with them and in its order: core/idl_args.h's
 * IDL_BUFFERSOURCE_OR_DICT for §14.3.9's `(BufferSource or JsonWebKey) keyData` position, the two
 * IdlDictMember tables in core/crypto/subtle_crypto.c, and these steps. The clause was right about all three
 * and right that they are ONE landing: the table alone turns a gap audit's row green while
 * `importKey("jwk", …)` fails exactly as before, and the first two without the third move the failure from a
 * TypeError at the conversion to step 5's NotSupportedError, which is a different wrong answer.
 * WHAT IT DID NOT NAME WAS TWO MORE MECHANISMS, and that is the part worth keeping rather than the part it got
 * right. `sequence<RsaOtherPrimesInfo> oth` needed a row of its own — there was no sequence-of-bare-dictionary
 * type at all, and the corpus declares a hundred dictionary members and five argument positions of that shape,
 * so IDL_SEQUENCE_DICT is a platform mechanism the crypto surface merely happened to need first — see that
 * row for the figure this file first carried as eighty-five, for why it was wrong, and for the far smaller
 * number that is actually work. And
 * that arm's step 4 decode needed base64url, which the engine's exported JS_Base64Decode is not; what closed it was
 * reading the jwk arm's step 3's own requirement — JSON Web Algorithms §6.4.1 through RFC 7515 §2's definition — as the
 * validation the ALGORITHM already owes, after which the decode is an alphabet substitution over an
 * already-validated string handed to the engine's own codec. A next-diff clause is a claim about this tree
 * written by someone who knew what was missing and was guessing at what fills it; this one guessed three of
 * five, and the two it missed were each in a different file from the ones it named.
 * THE SURFACE IT STATED WAS EXACT and is kept because it is the half that is easy to get wrong: 23 members
 * across TWO dictionaries, not the 20 across one that a gap-audit row named — §15 "JsonWebKey dictionary"
 * declares eighteen, a `partial dictionary JsonWebKey` in a WICG draft adds `pub` and `priv`, and the
 * remaining three are RsaOtherPrimesInfo's `r`, `d` and `t`. */
JSValue hmac_import_key(JSContext *ctx, const char *format, JSValueConst key_data, const HmacImportParams *p,
                        bool extractable, uint32_t usages);

/* §31.6.1 Sign and §31.6.2 Verify both begin "using the key represented by the [[handle]] internal slot of key,
 * the hash function identified by the hash attribute of the [[algorithm]] internal slot of key" — one sentence,
 * word for word the same in both, and this is that sentence.
 *
 * It reads the two slots of a key this engine minted and reports the SecureHashAlgorithm the `hash` attribute
 * names. It is here rather than in crypto_key.c because WHICH member of [[algorithm]] identifies H is §31.4
 * HmacKeyAlgorithm's fact and not §13's: another algorithm's KeyAlgorithm has no `hash` at all. */
SecureHashAlgorithm hmac_key_hash(JSContext *ctx, JSValueConst key);

/* §31.6.5 Export Key, WHOLE — steps 1 to 5, including step 4's format dispatch and its "Otherwise: throw a
 * NotSupportedError". The dispatch is the ALGORITHM's step and not §14.3.10's, which is why `format` is an
 * argument here rather than a branch at the caller: §14.3.10 hands the format to the operation ("performing
 * the export key operation specified by the [[algorithm]] internal slot of key using key and format") and
 * never looks at it again except at step 10.
 *
 * IT ANSWERS WITH ONE VALUE OF TWO KINDS, which is what §14.3.10 step 10 then converts and is why this returns
 * a JSValue rather than octets: the raw arm's result is a BYTE SEQUENCE and the jwk arm's is a DICTIONARY, and
 * the engine's carriers for those are an ArrayBuffer and an object. Step 10's two arms — "creating an
 * ArrayBuffer in realm, containing result" and "converting result to an ECMAScript Object in realm" — are
 * therefore the identity on what comes back, and the realm is THIS one because a C member runs in the realm
 * that defined it, which is the same argument §14.3.3 step 13's ArrayBuffer rests on.
 *
 * Returns JS_EXCEPTION with the DOMException step 4's Otherwise arm names pending for any other format. A
 * format outside §14.1's KeyFormat cannot reach here at all: the argument position is declared IDL_ENUM, so
 * Web IDL §3.2.18 refused it during the conversion. */
JSValue hmac_export_key(JSContext *ctx, const char *format, JSValueConst key);

#endif
