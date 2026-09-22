/* THE ADVANCED ENCRYPTION STANDARD — FIPS PUB 197, and the ONE block cipher this engine has.
 *
 * WHY THIS EXISTS AT ALL, AND WHY IT IS C RATHER THAN A HOST EDGE. Web Cryptography §29.1 Description says the
 * "AES-GCM" identifier performs "authenticated encryption and decryption using AES in Galois/Counter Mode
 * mode, as described in [NIST-SP800-38D]", and §29.4.1 Encrypt is awaited by the page mid-flow — so the answer
 * must differ per forked arm and must park and resume with the flow, which is CLAUDE.md §Architecture's own
 * test for what belongs in the engine. A cipher run in the trusted zone would be one answer for every arm,
 * reached across a boundary a suspended flow cannot cross.
 *
 * THE BIND-BEFORE-BUILD LADDER, WALKED FOR **THIS** PRIMITIVE RATHER THAN INHERITED FROM secure_hash.h.
 * Rung one, the HOST RUNTIME: neither host offers a block cipher — emscripten ships no crypto port (its ports
 * directory has zlib, libpng, SDL and friends and no mbedtls/openssl), and WASI exposes randomness but no
 * cipher; the only host-side AES is JavaScript's own `crypto.subtle`, which is asynchronous, lives outside the
 * COW delta, and would put a browser feature in the bridge. Rung two, an ENGINE INTRINSIC: quickjs-ng has no
 * cipher of any kind — `git grep -iE '\baes\b|rijndael|ghash|galois' -- engine/qjs/` answers empty. Rung
 * three, an EXISTING MODULE of this engine: lexbor's modules are core, css, dom, encoding, engine, html, ns,
 * ports, punycode, selectors, style, tag, unicode, url and utils, and there is no crypto among them; this
 * engine's own `core/crypto/secure_hash.c` is a DIGEST and no construction over a digest is a block cipher. So
 * the rung this lands on is the FAITHFUL SPEC PORT, which is the last rung before hand-rolling and is exactly
 * where secure_hash.h landed FIPS 180-4 for the same reasons.
 *
 * A VENDORED LIBRARY IS NOT A RUNG OF THAT LADDER, AND THE ONE THAT WAS PRICED FAILS THIS FILE'S OWN TEST.
 * mbedTLS was considered and refused, and the refusal is recorded because the next reader will otherwise price
 * it again. Its `mbedtls_aes_context` IS plain old data — it stores `size_t rk_offset` deliberately instead of
 * a pointer into its own buffer, which is the relocatable shape this file needs. Its `mbedtls_gcm_context` is
 * NOT: that struct opens with `mbedtls_block_cipher_context_t` only when `MBEDTLS_BLOCK_CIPHER_C` is defined,
 * and takes `mbedtls_cipher_context_t` otherwise — which holds `const mbedtls_cipher_info_t *cipher_info` and
 * `void *cipher_ctx`, two pointers, and is the BearSSL failure `secure_hash.c`'s own _Static_assert exists to
 * refuse. A record whose POD-ness is a property of somebody else's build configuration is an invariant held by
 * a macro rather than by construction, which is CLAUDE.md §Fix-the-ROOT's own complaint. The engine also
 * vendors no third-party C under `engine/host/` at all today: lexbor and quickjs ARE the engine, and neither
 * is a library this tree reached for.
 *
 * WHAT IT IS PORTED FROM, EXACTLY. FIPS PUB 197 (upd1, 9 May 2023), "Advanced Encryption Standard (AES)",
 * which NIST SP 800-38D names as the approved block cipher and which §29.1 reaches through it:
 *   §4.2   Multiplication in GF(2^8)  — XTIMES() and the reduction by m(x) = x^8 + x^4 + x^3 + x + 1
 *   §4.4   Multiplicative Inverses in GF(2^8)   — the b^-1 of equation (5.2)
 *   §5.1   CIPHER()                   — Algorithm 1's pseudocode, transcribed line for line
 *   §5.1.1 SUBBYTES()                 — equations (5.2) and (5.3), and Table 4's S-box
 *   §5.1.2 SHIFTROWS()   §5.1.3 MIXCOLUMNS()   §5.1.4 ADDROUNDKEY()
 *   §5.2   KEYEXPANSION()             — Algorithm 2, Table 5's round constants, and equations (5.10)/(5.11)
 *   §3.4   The State                  — the column-major convention the input is read under
 * The quotations below are transcribed from the fetched PDF, with the standard's SMALL-CAPS rendering of a
 * function name normalized (the document sets CIPHER() in small capitals, which extracts as spaced letters).
 * NOTE FOR ANY READER CHECKING THEM: FIPS 197 is a NIST publication and `engine/specindex` holds no index for
 * it, so `engine/citegen.mjs` COUNTS THIS FILE'S FIPS CITATIONS and CHECKS none of them — not the number, not
 * the title and not the quotation. That is a silent zero rather than a clean bill, which is why each citation
 * here names its section's own title, and it is why the S-BOX BELOW IS NOT TRANSCRIBED FROM TABLE 4 AT ALL: it
 * was DERIVED from §5.1.1's equations (5.2) and (5.3) and then checked, so a reader who distrusts the table can
 * re-derive it rather than proof-read 256 hexadecimal bytes against a PDF.
 *   THAT SCOPE READ `every citation in this file`, AND THE FILE IS THE ONE THING A COVERAGE NOTE MAY NOT NAME
 *   — the same defect core/crypto/hmac.h records at length, kept here in short form because a reader
 *   re-deriving it from the FIPS half will re-add it at the same scope. A coverage claim is read by somebody
 *   deciding whether to BUILD an instrument, so `CHECKS none of them` said of a FILE tells them to discount a
 *   channel that is running. The Web Cryptography citations here are INDEXED and JUDGED; the split is a
 *   command rather than a number to trust: `node engine/citegen.mjs engine/host/browser/core/crypto/aes.h`,
 *   MEASURED at 6cc8439a as 3 resolved of 37 read with 2 quotations compared and 0 findings.
 *   AND THE SILENCE OVER THE FIPS NUMBERS IS NOT THE SAME SILENCE hmac.h HAS, which is worth one line because
 *   the two look identical from the sentence above. Every FIPS 197 number in this file is written BARE, so it
 *   names no standard, resolves to nothing, and lands in the auditor's unanchored band — it is NOT on the
 *   `standards seen but not indexed` census line, which for this file names only `sp 800-38d`. So a reader
 *   cannot see the size of this blind spot the way hmac.h's reader can, and a bare number in a file that also
 *   carries indexed anchors is the shape a file vote places on whichever indexed standard owns that number.
 *   RETIREMENT: this record goes when every FIPS number in this file carries `FIPS 197` in front of it, so the
 *   census line states the count and the vote can no longer reach them.
 *
 * THERE IS NO INVERSE CIPHER, AND THAT IS A DECISION RATHER THAN AN OMISSION. GCM never runs AES backwards.
 * SP 800-38D §6.5's GCTR builds a KEYSTREAM with CIPH_K and exclusive-ors it into the data, so decryption uses
 * the FORWARD cipher exactly as encryption does; §7.1 step 1's hash subkey is CIPH_K(0^128); and §7.1 step 6's
 * tag is another GCTR. So §5.3 INVCIPHER() would have no caller in this engine, and CLAUDE.md's §NO STUBS is
 * about a shape with no working algorithm behind it — an unreferenced 200-line inverse is the same defect with
 * the shape on the inside.
 *   RESIDUAL — WHAT IS NOT COVERED: §5.3 INVCIPHER() and §5.3.5 EQINVCIPHER(), and with them the inverse
 *   transformations §5.3.1 INVSHIFTROWS(), §5.3.2 INVSUBBYTES() and §5.3.3 INVMIXCOLUMNS(). WHAT THE NEXT DIFF
 *   BUILDS: those five, when a mode that runs the cipher backwards arrives — Web Cryptography §28 AES-CBC and
 *   §33 AES-KW are the two the registry declares that would need them, and AES-CTR like GCM would not.
 *   HOW ITS ABSENCE WOULD SHOW: a caller wanting to decrypt one block finds NO ENTRY POINT in this header at
 *   all, so it is a missing declaration at compile time and never a wrong answer at run time.
 *
 * THE CONTEXT IS PLAIN OLD DATA AND HOLDS NO POINTER. It rides a step state across suspends, forks and
 * cross-session resumes, all of which copy the state's BYTES — so a pointer here would be one allocation two
 * arms both free, and a JSValue here would be a reference the copy does not count. Everything it holds is
 * value bytes, and aes.c's _Static_assert is what holds that rather than this paragraph.
 *
 * ONE PROBLEM PER FILE: this file is FIPS 197's BLOCK CIPHER and nothing else. Galois/Counter Mode is
 * core/crypto/aes_gcm.c, and which pages may ask for one is core/crypto/subtle_crypto.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_CRYPTO_AES_H
#define ENGINE_HOST_BROWSER_CORE_CRYPTO_AES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* §3.1 Inputs and Outputs: "the input and output blocks are sequences of 128 bits". One block, in BYTES — and
   SP 800-38D §5.1's prerequisite for GCM is an "approved block cipher CIPH with a 128-bit block size", so this
   is the unit BOTH standards are written over and the unit a preemptible caller advances by. */
#define AES_BLOCK 16

/* §5.2 KEYEXPANSION() generates "4 * (Nr + 1) words", and §6.1 Key Length Requirements admits key lengths of
   128, 192 and 256 bits — which is also exactly the set Web Cryptography §29.4.4 Import Key admits ("If the
   length in bits of data is not 128, 192 or 256 then throw a DataError"). Nr is 10, 12 and 14 respectively, so
   the largest schedule is 4 * 15 = 60 words = 240 bytes and every key length shares this one array. */
#define AES_MAX_ROUNDS   14
#define AES_SCHEDULE_MAX (4u * 4u * (AES_MAX_ROUNDS + 1u))   /* 240 bytes: w[0] .. w[59] */

typedef struct {
    /* §5.2's "linear array of words, denoted by w[i]", as BYTES: w[i] is the four bytes at rk[4*i]. Words
       rather than a 2-D array because the standard's own index is linear, and bytes rather than uint32_t
       because §5.1.4's ADDROUNDKEY() adds w[j] to a state COLUMN byte by byte and §5.2's ROTWORD() is a
       rotation of four adjacent bytes — both are the identity on this layout and neither needs an endian
       convention, which a uint32_t would silently introduce. */
    uint8_t rk[AES_SCHEDULE_MAX];
    /* §2.3 Algorithm Parameters and Symbols' Nr, "the number of rounds": 10, 12 or 14. A byte so the record
       stays copyable POD. */
    uint8_t nr;
} Aes;

/* §5.2 KEYEXPANSION(), Algorithm 2: "KEYEXPANSION() is a routine that is applied to the key to generate
   4 * (Nr + 1) words." `key_len` is in BYTES and must be 16, 24 or 32 — §6.1's three key lengths. It is
   CHECKED rather than trusted, because a caller that passed a length the standard does not define would
   otherwise expand a schedule from memory it does not own.
   THE KEY'S BYTES ARE THE PAGE'S and their CONTENT is never asserted on: CLAUDE.md §WHOSE-BYTES-STATE-THE-VALUE
   forbids a DCHECK standing on a value a page supplied, because that hands the page an abort switch. The LENGTH
   is this engine's own fact — §29.4.4 has already refused a key that is not 128, 192 or 256 bits with a
   DataError by the time a key reaches here — so a length outside the three is a defect in THIS tree. */
void aes_init(Aes *a, const uint8_t *key, size_t key_len);

/* §5.1 CIPHER(), Algorithm 1 — ONE block, which is the O(1) engine action a stage may name. `in` and `out` are
   AES_BLOCK bytes each and MAY ALIAS, which GCTR relies on. There is deliberately no multi-block entry: it
   would be the un-parkable spelling sitting next to the parkable one, and the first caller in a hurry would
   take it — secure_hash.h's argument, and the reason that file has no one-shot either. */
void aes_encrypt_block(const Aes *a, const uint8_t in[AES_BLOCK], uint8_t out[AES_BLOCK]);

#endif
