/* THE SECURE HASH STANDARD — FIPS PUB 180-4, and the ONE message digest this engine has.
 *
 * WHY THIS EXISTS AT ALL, AND WHY IT IS C RATHER THAN A HOST EDGE. Two callers need a digest and neither can
 * wait for a round trip to JavaScript. CSP §6.7.3.3 step 5.2.2 hashes an inline block's source WHILE the
 * browser decides whether that block runs, and Web Cryptography §14.3.5's digest() is awaited by the page
 * mid-flow — so the answer must differ per forked arm and must park and resume with the flow, which is
 * CLAUDE.md §Architecture's own test for what belongs in the engine. A digest computed in the trusted zone
 * would be one answer for every arm, reached across a boundary a suspended flow cannot cross.
 *
 * THE BIND-BEFORE-BUILD LADDER, WALKED AND REPORTED. Host runtime: neither host offers a digest — emscripten
 * ships no crypto port (its ports directory has zlib, libpng, SDL and friends and no mbedtls/openssl), and
 * WASI exposes randomness but no hash; the only host-side digest is JavaScript's own `crypto.subtle`, which is
 * asynchronous, lives outside the COW delta, and would put a browser feature in the bridge. Engine intrinsic:
 * quickjs-ng has no digest of any kind (it has base64 — see the encoder this file's CSP caller uses — and
 * nothing else). Lexbor: its modules are core, css, dom, encoding, engine, html, ns, ports, punycode,
 * selectors, style, tag, unicode, url and utils; there is no crypto among them. So the rung this lands on is
 * the FAITHFUL SPEC PORT, which is the last rung before hand-rolling and is where a published standard with a
 * published reference implementation belongs.
 *
 * WHAT IT IS PORTED FROM, EXACTLY. FIPS PUB 180-4 (August 2015), the standard Web Cryptography §32.1 names
 * normatively and the one CSP's hash-source ultimately rests on:
 *   §4.1.1 SHA-1 Functions          — f_t is Ch / Parity / Maj / Parity over the four twenty-round windows
 *   §4.1.2 SHA-224 and SHA-256 Functions   §4.1.3 SHA-384, SHA-512, … Functions
 *   §4.2.1 SHA-1 Constants          — 5a827999 6ed9eba1 8f1bbcdc ca62c1d6
 *   §4.2.2 SHA-224 and SHA-256 Constants   §4.2.3 SHA-384, SHA-512, … Constants
 *   §5.1.1 SHA-1, SHA-224 and SHA-256 (padding)   §5.1.2 SHA-384, SHA-512, … (padding)
 *   §5.3.1 SHA-1   §5.3.3 SHA-256   §5.3.4 SHA-384   §5.3.5 SHA-512  (initial hash values)
 *   §6.1.2 SHA-1 Hash Computation   §6.2.2 SHA-256 Hash Computation   §6.4.2 SHA-512 Hash Computation
 *   §6.5   SHA-384 (SHA-512's computation from §5.3.4's H(0), truncated to the leftmost six words)
 * The six logical functions are transcribed from RFC 6234 §5.1 and §5.2, which state in ASCII text the
 * formulas FIPS 180-4 sets as equations (4.2)-(4.13) — the same functions, and the reason the RFC exists.
 *
 * IT IS STREAMING BECAUSE ITS CALLER MUST BE PREEMPTIBLE. A message is of the PAGE'S size, so hashing one is
 * not an O(1) engine action and a stage may not name it as a range (quickjs-step.h states the rule and
 * JS_STEP_YIELD is its answer). The caller therefore drives ONE FIPS §6.x block per turn and yields between
 * them, which it can only do if the compression state is separable from the walk — hence init/update/finish
 * rather than a one-shot. There is no one-shot convenience wrapper: it would be the un-parkable spelling
 * sitting next to the parkable one, and the first caller in a hurry would take it.
 *
 * THE CONTEXT IS PLAIN OLD DATA AND HOLDS NO POINTER. It rides a step state across suspends, forks and
 * cross-session resumes, all of which copy the state's BYTES — so a pointer here would be one allocation two
 * arms both free, and a JSValue here would be a reference the copy does not count. Everything it holds is
 * value bytes. */
#ifndef ENGINE_HOST_BROWSER_CORE_CRYPTO_SECURE_HASH_H
#define ENGINE_HOST_BROWSER_CORE_CRYPTO_SECURE_HASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* THE ALGORITHMS THIS ENGINE COMPUTES, which is exactly the set Web Cryptography §32.2 registers: "The
   recognized algorithm names are "SHA-1", "SHA-256", "SHA-384", and "SHA-512" for the respective SHA
   algorithms." CSP §2.3.1's hash-algorithm names three of them and no more. SHA-224 and the two SHA-512/t
   variants FIPS 180-4 also defines are absent because no standard in this engine can ask for one — a name
   nothing spells is a name nothing can be wrong about. */
typedef enum {
    SECURE_HASH_SHA1 = 0,
    SECURE_HASH_SHA256,
    SECURE_HASH_SHA384,
    SECURE_HASH_SHA512,
} SecureHashAlgorithm;

/* FIPS 180-4 Figure 1's Message Digest Size, in BYTES: SHA-1 160 bits, SHA-256 256, SHA-384 384, SHA-512 512. */
#define SECURE_HASH_MAX_DIGEST 64

/* FIPS 180-4 Figure 1's Block Size, in BYTES: 512 bits for SHA-1/SHA-256, 1024 bits for SHA-384/SHA-512. This
   is the unit the caller advances by, because §6.1.2 / §6.2.2 / §6.4.2 each perform their computation "for
   each of the N message blocks" and ONE of those is the O(1) engine action a stage may name. */
#define SECURE_HASH_MAX_BLOCK  128

typedef struct {
    /* The hash value H(i). Eight words, 32-bit for SHA-1 (of which five are used) and SHA-256, 64-bit for
       SHA-384 and SHA-512 — held as 64-bit so one record serves both word sizes without a union whose active
       arm nothing records. */
    uint64_t h[8];
    /* §5.1's L, in BYTES rather than bits: the padding writes 8*len and this engine cannot be handed a message
       whose bit length overflows 64 bits (an ArrayBuffer is bounded by the heap), which secure_hash_update
       asserts rather than assumes. */
    uint64_t len;
    uint8_t  buf[SECURE_HASH_MAX_BLOCK];   /* the partial block §5.1 will pad */
    uint32_t buf_len;
    uint8_t  alg;                          /* a SecureHashAlgorithm; a byte so the record stays copyable POD */
    /* §5.1's padding is written INTO `buf`, so a context that has been finished is one whose h and len no
       longer describe the message. A second finish would pad the padding and answer a digest of something
       nobody hashed, and an update after one would silently continue from it — both are asserted rather than
       tolerated, which needs one bit of state to be asserted ABOUT. */
    uint8_t  done;
} SecureHash;

/* FIPS 180-4 Figure 1's Message Digest Size for `alg`, in bytes. */
size_t secure_hash_digest_size(SecureHashAlgorithm alg);
/* FIPS 180-4 Figure 1's Block Size for `alg`, in bytes — what a preemptible caller advances by. */
size_t secure_hash_block_size(SecureHashAlgorithm alg);

/* WEB CRYPTOGRAPHY §32.2 Registration's NAME for `alg` — one of the four the paragraph above quotes. It is
   declared here rather than in whichever component needs one because the sentence that names them is the same
   sentence that says why this enum has four members: which four, and what each is called, are one fact. */
const char *secure_hash_name(SecureHashAlgorithm alg);
/* Its inverse over the same four rows, by EXACT match — the input is a name this engine wrote, not the page's,
   so §18.4.4 step 5's case-insensitive comparison is a different question and stays where the page's string
   arrives. False for anything else, which is a name §32.2 does not register. */
bool secure_hash_by_name(const char *name, SecureHashAlgorithm *out);

/* §5.3's SETTING THE INITIAL HASH VALUE. */
void secure_hash_init(SecureHash *h, SecureHashAlgorithm alg);

/* §5.2's PARSING plus §6.x's per-block computation over whatever whole blocks `data` completes. Any number of
   bytes; the remainder is held for the next call. A caller that wants a rest point between blocks passes ONE
   block at a time and yields between calls — that is the whole reason this is not one function. */
void secure_hash_update(SecureHash *h, const uint8_t *data, size_t len);

/* §5.1's PADDING and the final concatenation of §6.x. `out` must hold secure_hash_digest_size(alg) bytes; the
   size is passed and CHECKED rather than trusted, because a caller that sized for SHA-256 and asked for
   SHA-512 would otherwise write 32 bytes past its buffer with the right answer in it. The context is left
   FINISHED and must not be updated again, which is asserted. */
void secure_hash_finish(SecureHash *h, uint8_t *out, size_t out_size);

#endif
