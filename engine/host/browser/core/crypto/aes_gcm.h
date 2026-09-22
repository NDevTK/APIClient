/* GALOIS/COUNTER MODE — NIST SP 800-38D, and the ONE authenticated cipher this engine has.
 *
 * WHY GCM IS THE MODE AFTER §32.3.1's DIGEST AND §31's HMAC, AND WHY IT IS AFFORDABLE NOW. Web Cryptography
 * §14's eleven absent operations divide by what primitive they stand on, and GCM stands on a BLOCK CIPHER and
 * on field arithmetic over GF(2^128) — no bignum, no elliptic curve, no ASN.1. With core/crypto/aes.c
 * computing FIPS 197's CIPHER() there is nothing between this file and a working §29.4.1 Encrypt but the five
 * steps of §7.1 and the two functions of §6.4 and §6.5. Every other unbuilt algorithm of §20-§34 needs a
 * bignum or a curve this engine does not have and cannot bind to, so each is a larger piece of work;
 * CLAUDE.md §Do-subproblems-IN-ORDER is what puts this one first rather than a preference.
 *
 * THE BIND-BEFORE-BUILD LADDER FOR **THIS** PRIMITIVE. Rung one, the host runtime, offers nothing (neither
 * emscripten's ports nor WASI expose an AEAD, and JavaScript's own `crypto.subtle.encrypt` is asynchronous,
 * lives outside the COW delta and would put a browser feature in the bridge); rung two, an engine intrinsic,
 * offers nothing (quickjs-ng has no cipher and no field arithmetic); rung three is where it STOPS — an
 * EXISTING MODULE of this engine, `core/crypto/aes.c`, IS the "approved block cipher CIPH with a 128-bit block
 * size" that §7.1's prerequisites are written over. What this file adds is two functions of §6 and the seven
 * steps of §7.1/§7.2, and no block cipher whatever. aes.h reports the ladder for the cipher itself, and
 * records why a vendored mbedTLS was priced and refused.
 *
 * WHAT IT IS PORTED FROM, EXACTLY. NIST Special Publication 800-38D (November 2007), "Recommendation for Block
 * Cipher Modes of Operation: Galois/Counter Mode (GCM) and GMAC", which §29.1 Description names by reference
 * ("as described in [NIST-SP800-38D]") and which §29.4.1 and §29.4.2 each reach by section number:
 *   §5.2.1.1 Input Data              — the supported lengths of IV, P and A
 *   §6.2   Incrementing Function     — inc_32
 *   §6.3   Multiplication Operation on Blocks — the bullet-dot product in GF(2^128), Algorithm 1
 *   §6.4   GHASH Function            — Algorithm 2
 *   §6.5   GCTR Function             — Algorithm 3
 *   §7.1   Algorithm for the Authenticated Encryption Function — Algorithm 4, GCM-AE
 *   §7.2   Algorithm for the Authenticated Decryption Function — Algorithm 5, GCM-AD
 * NOTE FOR ANY READER CHECKING THE CITATIONS: SP 800-38D is a NIST publication and `engine/specindex` holds no
 * index for it, so `engine/citegen.mjs` COUNTS THIS FILE'S NIST CITATIONS and CHECKS none of them — not the
 * number, not the title and not the quotation. That is a silent zero rather than a clean bill, and it is why
 * each citation here names its section's own title and quotes its own words.
 *   THAT SCOPE READ `every citation in this file`, AND THE FILE IS THE ONE THING A COVERAGE NOTE MAY NOT NAME
 *   — the same defect core/crypto/hmac.h records at length, kept here in short form because a reader
 *   re-deriving it from the NIST half will re-add it at the same scope. The only reader of a coverage claim is
 *   somebody deciding whether to BUILD an instrument for that axis, so `CHECKS none of them` said of a FILE
 *   argues for a second auditor beside a working one and tells everyone else to discount a live channel.
 *   THE SPLIT IS A COMMAND AND NOT A NUMBER TO TRUST: `node engine/citegen.mjs
 *   engine/host/browser/core/crypto/aes_gcm.h` prints how many citations resolved on their own evidence and
 *   names, on its `standards seen but not indexed` line, the standards it counted and did not check. MEASURED
 *   at 6cc8439a it was 2 resolved of 80 read, `fips 197=1` on that line, and 0 findings — so the NIST numbers
 *   here are mostly BARE, which is a different silence from a named-and-counted one and the one a file vote
 *   can still reach.
 *   RETIREMENT: this record goes when every NIST number in this file carries its standard's name in front of
 *   it, so the census line states the count and the vote can no longer reach them.
 *
 * IT IS ONE WALK AND NOT TWO, AND THE STANDARD SAYS THAT IS ALLOWED IN ITS OWN WORDS. Read literally,
 * Algorithm 4 encrypts the WHOLE plaintext at step 3 and only then hashes the whole ciphertext at step 5 — two
 * passes over data of the page's size. §7 permits the restructuring that makes it one: "For both algorithms,
 * equivalent sets of steps that produce the correct output are permitted." So each block is enciphered and
 * immediately absorbed into the running GHASH, which is the same output by the same equations and is what lets
 * the state below be O(1).
 *
 * IT IS BLOCK-AT-A-TIME BECAUSE ITS CALLER MUST BE PREEMPTIBLE, AND GCM IS SHAPED FOR THAT. Three of this
 * algorithm's inputs are of the PAGE'S size — the IV (§29.3: "May be up to 2^64-1 bytes long"), the additional
 * data, and the plaintext — so none of them is an O(1) engine action and quickjs-step.h's rule is explicit
 * about what that means. GCM costs nothing to suspend: §6.4's Algorithm 2 step 3 is "For i = 1, ..., m, let
 * Y_i = (Y_{i-1} (+) X_i) (*) H", a recurrence whose ENTIRE state between blocks is the one 128-bit block Y,
 * and §6.5's Algorithm 3 is the same shape over the one counter block CB. So one turn is one 128-bit block —
 * the standard's own unit of work, and the same unit core/crypto/secure_hash.c already rests between.
 * There is deliberately no one-shot convenience entry, for secure_hash.h's reason: it would be the un-parkable
 * spelling sitting next to the parkable one, and the first caller in a hurry would take it.
 *
 * THE STATE IS PLAIN OLD DATA AND HOLDS NO POINTER AND NO JSValue. It rides a step state across suspends,
 * forks and cross-session resumes, all of which copy the state's BYTES. The KEY MATERIAL it reads is not in
 * here either: it lives as §13.3's [[handle]] on the CryptoKey, which is a JS value for exactly the reason
 * CLAUDE.md §PLATFORM-DATA states — see crypto_key.h. aes_gcm.c's _Static_assert is what holds the POD
 * invariant rather than this paragraph.
 *
 * WHOSE BYTES THESE ARE. The key, the IV, the additional data, the plaintext and the candidate tag are all the
 * PAGE'S, so nothing here asserts on their CONTENT — CLAUDE.md §WHOSE-BYTES-STATE-THE-VALUE, and a forcing
 * solver meets unusual arguments constantly. What this file does assert is its own LENGTHS and its own PHASE
 * ORDER, which are facts §14.3.1's caller has already established: by the time a call reaches here, §29.4.1
 * steps 1-5 have refused every length this engine does not serve, so a bad one is a defect in THIS tree.
 *
 * ONE PROBLEM PER FILE: this file is SP 800-38D's MODE. FIPS 197's cipher is core/crypto/aes.c, and which
 * pages may ask for one — §14.3.1's promise, §18.4.4's normalization and §29.3's AesGcmParams — belongs to
 * core/crypto/subtle_crypto.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_CRYPTO_AES_GCM_H
#define ENGINE_HOST_BROWSER_CORE_CRYPTO_AES_GCM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/crypto/aes.h"

/* §29.4.1 Encrypt steps 3-4's SEVEN tag lengths, in BYTES: "If the tagLength member of normalizedAlgorithm is
   one of 32, 64, 96, 104, 112, 120 or 128" bits. §7.1's `t` is the largest of them and is also the block
   size — §6.5's MSB_t cannot take more bits than the one block GCTR produces. */
#define AES_GCM_MAX_TAG AES_BLOCK

/* WHERE THE STEPS OF §7.1 / §7.2 HAVE GOT TO. A phase and not a stage: the CALLER owns the stages its own
   algorithm is written in, and this is the sub-walk's cursor inside them, in the same spirit as hmac.h's
   HmacPhase and the request cursors quickjs-step.h's STEP_GOTO asserts over. */
typedef enum {
    AES_GCM_PH_IV = 0,   /* §7.1 step 2 / §7.2 step 3: J_0 is being formed and the IV read block by block */
    AES_GCM_PH_AAD,      /* §7.1 step 5's A region, absorbed into GHASH block by block */
    AES_GCM_PH_TEXT,     /* §7.1 step 3's GCTR and step 5's C region, interleaved per §7's own licence */
    AES_GCM_PH_DONE      /* §7.1 step 6 produced T, or §7.2 step 8 answered */
} AesGcmPhase;

typedef struct {
    /* §7.1's len(A) and len(C), in BYTES — step 5 writes them as [len(A)]_64 and [len(C)]_64, in BITS, and
       the multiplication by eight happens there rather than here so that a byte count cannot overflow before
       the standard's own 2^64-1 bound is reached. */
    uint64_t aad_len;
    uint64_t txt_len;
    uint64_t iv_len;     /* len(IV) in bytes, as the caller stated it before a byte was read */
    uint64_t iv_taken;   /* how much of the IV has been fed through aes_gcm_iv_update */
    uint32_t buf_len;    /* bytes pending in `buf` for the region currently being absorbed */
    Aes      aes;        /* §7.1's prerequisite: "approved block cipher CIPH with a 128-bit block size" */
    uint8_t  h[AES_BLOCK];    /* §7.1 step 1: "Let H = CIPH_K(0^128)" — the hash subkey */
    uint8_t  j0[AES_BLOCK];   /* §7.1 step 2's pre-counter block, kept because step 6's tag GCTR uses it */
    uint8_t  cb[AES_BLOCK];   /* §6.5's CB_i, the running counter block */
    uint8_t  y[AES_BLOCK];    /* §6.4's Y_i, the running GHASH accumulator */
    uint8_t  buf[AES_BLOCK];  /* the partial block §7.1 step 4 will pad with 0^v or 0^u */
    uint8_t  ks[AES_BLOCK];   /* §6.5 step 6's CIPH_K(CB_i), consumed byte by byte so the caller may feed any
                                 length; `buf_len` is the cursor into it as well, because in the TEXT phase the
                                 byte being enciphered and the byte being absorbed are the same byte's two
                                 halves and a second cursor could disagree with this one */
    uint8_t  tag_len;    /* §7.1's `t`, in BYTES — one of 4, 8, 12, 13, 14, 15, 16 */
    uint8_t  phase;      /* an AesGcmPhase */
    uint8_t  decrypting; /* §7.2's Algorithm 5 rather than §7.1's Algorithm 4 */
} AesGcm;

/* §7.1 step 1 and the PREREQUISITES of Algorithm 4 / Algorithm 5: expand the key, derive H, and declare the
 * lengths this invocation will be given. The state is left in AES_GCM_PH_IV wanting `iv_len` bytes.
 *
 * `key_len` is 16, 24 or 32 — FIPS 197 §6.1's three, which are also §29.4.4 Import Key's three.
 * `iv_len` is len(IV) in BYTES and must be at least 1: §5.2.1.1 Input Data requires 1 <= len(IV) <= 2^64-1,
 *   and §29.4.1 does not state that lower bound itself, so §14.3.1's caller owes an OperationError for an
 *   empty IV BEFORE it reaches here — which is why a zero here is this engine's defect and is asserted.
 * `tag_len` is §7.1's `t` in BYTES, one of the seven §29.4.1 step 4 admits.
 * `decrypting` selects §7.2's Algorithm 5; the two differ only in the order of the two halves of a text block
 *   and in how the walk ends, which is why one state serves both. */
void aes_gcm_begin(AesGcm *g, const uint8_t *key, size_t key_len,
                   uint64_t iv_len, size_t tag_len, bool decrypting);

/* HOW MANY BYTES OF THE IV STILL HAVE TO BE FED. Zero means the walk may end; the caller advances by at most
   AES_BLOCK bytes per turn and yields between turns, which is what makes §7.1 step 2's GHASH arm preemptible
   for an IV of the page's size. */
uint64_t aes_gcm_iv_left(const AesGcm *g);

/* ONE TURN OF THE IV WALK. `n` must be at most aes_gcm_iv_left(g). */
void aes_gcm_iv_update(AesGcm *g, const uint8_t *p, size_t n);

/* CLOSE §7.1 step 2 — J_0 is now complete by whichever of its two arms len(IV) selected — and move to
   AES_GCM_PH_AAD. Every byte of the IV must have been fed, which is asserted rather than assumed. */
void aes_gcm_iv_end(AesGcm *g);

/* ONE TURN OF THE ADDITIONAL-DATA WALK — the `A` of §7.1 step 5. Any length; a caller that wants a rest point
   between blocks passes AES_BLOCK bytes and yields. §29.4.1 step 6 makes an absent `additionalData` "an empty
   byte sequence", which is this function called zero times and is not a special case. */
void aes_gcm_aad_update(AesGcm *g, const uint8_t *p, size_t n);

/* CLOSE the A region: §7.1 step 4's `0^v` padding is applied so that what follows starts on a block boundary,
   and the state moves to AES_GCM_PH_TEXT. */
void aes_gcm_aad_end(AesGcm *g);

/* ONE TURN OF THE TEXT WALK — §7.1 step 3's GCTR over P interleaved with step 5's absorption of C, or §7.2
 * step 4's GCTR over C interleaved with step 6's absorption of that same C. Any length; `in` and `out` are
 * each `n` bytes and MAY ALIAS.
 *
 * WHEN DECRYPTING, THE BYTES WRITTEN TO `out` ARE NOT YET PLAINTEXT THE CALLER MAY RELEASE. §7.2 step 8 is
 * "If T' = T, then return P; else return FAIL", so the plaintext is the algorithm's output only once the tag
 * has been checked — and §29.4.2 step 9 turns a FAIL into an OperationError with no plaintext at all. A
 * streaming mode cannot check the tag until the last block has been absorbed, so the caller MUST hold what it
 * writes here and hand it to the page only after aes_gcm_decrypt_verify has answered true. Releasing it early
 * is a decryption oracle: an attacker who can submit ciphertexts and read the "rejected" plaintext recovers
 * the keystream a block at a time. */
void aes_gcm_text_update(AesGcm *g, const uint8_t *in, uint8_t *out, size_t n);

/* §7.1 steps 4-6: pad the C region with `0^u`, absorb [len(A)]_64 || [len(C)]_64, and encipher the resulting
   S under J_0 to produce T. `tag` must hold `tag_size` bytes and `tag_size` must equal the tag_len this state
   was begun with; it is passed and CHECKED rather than trusted, because a caller that sized for a 32-bit tag
   and asked for a 128-bit one would otherwise write twelve bytes past its buffer with the right answer in it.
   The state is left AES_GCM_PH_DONE and must not be reused, which is asserted. Encrypting states only. */
void aes_gcm_encrypt_finish(AesGcm *g, uint8_t *tag, size_t tag_size);

/* §7.2 steps 5-8: form T' exactly as §7.1 step 6 does, then answer step 8's comparison — true for "return P",
 * false for FAIL, which §29.4.2 step 9 renders as an OperationError. Decrypting states only.
 *
 * THE COMPARISON IS CONSTANT IN THE BYTES AND NOT IN THE LENGTH, which is hmac.h's argument for §31.6.2 step 2
 * and is the same argument here. The LENGTH of the candidate tag is the page's own argument — §29.4.2 step 7
 * takes it as "the last tagLength bits of ciphertext", a number the caller chose — so a length mismatch may
 * answer immediately. What must not leak is WHERE two equal-length tags first differ, because that is an
 * oracle for forging one byte at a time, so equal-length inputs accumulate the difference of EVERY byte and
 * are tested once. */
bool aes_gcm_decrypt_verify(AesGcm *g, const uint8_t *tag, size_t tag_len);

#endif
