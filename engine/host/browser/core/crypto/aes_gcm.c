/* NIST SP 800-38D's GHASH, GCTR and the two GCM algorithms, transcribed from §6.3, §6.4, §6.5, §7.1 and §7.2.
 * See aes_gcm.h for the bind-before-build ladder, for why one walk rather than two, and for the contract a
 * decrypting caller owes its own output buffer.
 *
 * THE KNOWN-ANSWER TESTS ARE NOT HERE. A self-consistent encrypt/decrypt round trip proves nothing about an
 * AEAD — it passes for any keystream at all, including a constant one — so what this file is checked by is the
 * published vectors of NIST's Cryptographic Algorithm Validation Program, the GCM Validation System's
 * gcmEncryptExtIV response files for all three key lengths, driven from engine/host/test_forced.c. What IS
 * here is the invariants a CALLER can violate: a phase out of order, a length that was declared and not fed, a
 * tag written into a buffer sized for a different tagLength. */
#include <string.h>

#include "check.h"
#include "core/crypto/aes_gcm.h"

/* THE RECORD IS POD, AND ITS SIZE IS WHAT PROVES IT — core/crypto/secure_hash.c's argument, owed again here
   because this is the record a vendored GCM context would have replaced. Every member is fixed-width, and a
   POINTER is eight bytes natively and FOUR under emcc's wasm32, so once one is added no single constant can
   satisfy both of the targets engine/build.mjs compiles. This is not hypothetical for THIS struct in
   particular: mbedTLS's `mbedtls_gcm_context` opens with a `mbedtls_cipher_context_t` — which holds
   `const mbedtls_cipher_info_t *cipher_info` and `void *cipher_ctx` — on every build where
   MBEDTLS_BLOCK_CIPHER_C is undefined, so its POD-ness is a property of somebody else's configuration rather
   than of the type. A deliberate layout change updates this number; a pointer cannot. */
_Static_assert(sizeof(AesGcm) == 376,
               "SP 800-38D's streaming GCM state is no longer 376 bytes. If a POINTER was added it can no "
               "longer ride a COW snapshot or a cold-tier resume — see aes_gcm.h's own paragraph on why this "
               "record holds none. If the layout changed deliberately, update this number.");
_Static_assert(AES_GCM_MAX_TAG == AES_BLOCK,
               "§6.5's MSB_t is taken from ONE GCTR output block, so t can never exceed the block size");

/* ---- SP 800-38D §6.2 Incrementing Function ------------------------------------------------------------- */

/* inc_32: "the function increments the right-most s bits of the string, regarded as the binary representation
   of an integer, modulo 2^s; the remaining, left-most len(X) - s bits remain unchanged." GCM's s is 32, so the
   left-most 96 bits of the counter block are fixed for the whole invocation and only the trailing four bytes
   move — which is why J_0's first twelve bytes are the IV in the len(IV) = 96 arm. Big-endian, because §4.1's
   convention reads a block's left-most bit as most significant. */
static void inc32(uint8_t cb[AES_BLOCK])
{
    unsigned i;
    for (i = AES_BLOCK; i-- > AES_BLOCK - 4u; )
        if (++cb[i] != 0) break;   /* modulo 2^32: the carry stops inside the four bytes, never past them */
}

/* ---- SP 800-38D §6.3 Multiplication Operation on Blocks ------------------------------------------------ */

/* Algorithm 1's bullet-dot product, Z = X (*) Y, in GF(2^128). §6.3 states it as a 128-step loop over the bits
   of X, with V starting at Y and being halved each step, exclusive-ored with R = 11100001 || 0^120 whenever
   the bit shifted out was set:
     Z_0 = 0, V_0 = Y; and for i = 0 to 127,
       Z_{i+1} = Z_i            if x_i = 0;   Z_i (+) V_i   if x_i = 1.
       V_{i+1} = V_i >> 1       if LSB_1(V_i) = 0;   (V_i >> 1) (+) R   if LSB_1(V_i) = 1.
   §6.3's own paragraph says why the shift is RIGHTWARD where a polynomial multiplication would shift left:
   "The convention for interpreting strings as polynomials is little endian", so the block x_0 x_1 ... x_127
   is the polynomial x_0 + x_1 u + ... + x_127 u^127 and the left-most BIT is the LOWEST-order coefficient.
   IT IS BRANCHLESS ON BOTH SECRET BITS. X is the running GHASH value exclusive-ored with the data, and Y is
   the hash subkey H = CIPH_K(0^128) — both are secret-derived, so a conditional on either is a timing oracle
   for the key. A table-driven GHASH (mbedTLS precomputes a 256-byte HTable, and the fastest spellings use a
   4 KiB one) would be faster and would introduce a key-dependent MEMORY ACCESS PATTERN, which is the same leak
   through a cache instead of through a branch; the bit-serial form below has neither. */
static void gf_mul(uint8_t z[AES_BLOCK], const uint8_t x[AES_BLOCK], const uint8_t y[AES_BLOCK])
{
    uint8_t  v[AES_BLOCK];
    uint8_t  acc[AES_BLOCK];
    unsigned i, j;

    memset(acc, 0, AES_BLOCK);
    memcpy(v, y, AES_BLOCK);

    for (i = 0; i < 128u; i++) {
        /* x_i, the i-th bit counting from the left-most, as an all-ones or all-zeros mask. */
        uint8_t xi   = (uint8_t)(-(int)((x[i >> 3] >> (7u - (i & 7u))) & 1u));
        uint8_t lsb  = (uint8_t)(-(int)(v[AES_BLOCK - 1u] & 1u));
        uint8_t carry;

        for (j = 0; j < AES_BLOCK; j++) acc[j] ^= (uint8_t)(xi & v[j]);

        /* V >> 1 across the whole 128-bit block, most-significant byte first. */
        carry = 0;
        for (j = 0; j < AES_BLOCK; j++) {
            uint8_t next = (uint8_t)(v[j] << 7);
            v[j]  = (uint8_t)((v[j] >> 1) | carry);
            carry = next;
        }
        /* R is 11100001 || 0^120 — one byte, 0xe1, at the left-most position. */
        v[0] ^= (uint8_t)(lsb & 0xe1u);
    }
    memcpy(z, acc, AES_BLOCK);
}

/* ---- SP 800-38D §6.4 GHASH Function -------------------------------------------------------------------- */

/* Algorithm 2 step 3, for ONE block: "For i = 1, ..., m, let Y_i = (Y_{i-1} (+) X_i) (*) H." Step 2 sets Y_0
   to "the zero block, 0^128", which is what aes_gcm_begin and the end of the IV phase each leave behind. This
   one call IS the unit of work the scheduler rests between. */
static void ghash_block(AesGcm *g, const uint8_t x[AES_BLOCK])
{
    uint8_t t[AES_BLOCK];
    unsigned i;

    for (i = 0; i < AES_BLOCK; i++) t[i] = (uint8_t)(g->y[i] ^ x[i]);
    gf_mul(g->y, t, g->h);
}

/* Absorb the pending partial block after zero-padding it — §7.1 step 4's `0^v` for the A region and `0^u` for
   the C region, which that step describes as appending "the minimum number of 0 bits, possibly none, so that
   the bit lengths of the resulting strings are multiples of the block size". "Possibly none" is the buf_len
   == 0 case, and absorbing a whole zero block there would change the answer, so it returns instead. */
static void ghash_pad(AesGcm *g)
{
    if (g->buf_len == 0) return;
    DCHECK(g->buf_len < AES_BLOCK, "a whole block was left pending in the partial-block buffer — the feeding "
                                   "loop absorbs at AES_BLOCK and must never leave sixteen bytes behind");
    memset(&g->buf[g->buf_len], 0, AES_BLOCK - g->buf_len);
    ghash_block(g, g->buf);
    g->buf_len = 0;
}

/* Write a 64-bit big-endian length, which is §7.1 step 5's [len(A)]_64 and [len(C)]_64 and §7.1 step 2's
   [len(IV)]_64 — all three in BITS, which is where the byte counts this state keeps are multiplied by eight. */
static void put_len_bits(uint8_t out[8], uint64_t bytes)
{
    uint64_t bits = bytes << 3;
    unsigned i;
    for (i = 0; i < 8u; i++) out[i] = (uint8_t)(bits >> (56u - 8u * i));
}

/* ---- SP 800-38D §7.1 step 1 and the Algorithm 4 / Algorithm 5 prerequisites ---------------------------- */

void aes_gcm_begin(AesGcm *g, const uint8_t *key, size_t key_len,
                   uint64_t iv_len, size_t tag_len, bool decrypting)
{
    static const uint8_t ZERO[AES_BLOCK] = { 0 };

    DCHECK(g != NULL, "aes_gcm_begin was given no state");
    DCHECK(key != NULL, "aes_gcm_begin was given no key");
    /* §5.2.1.1 Input Data requires 1 <= len(IV); §29.4.1 Encrypt states only the upper bound, so §14.3.1's
       caller owes an OperationError for an empty IV and a zero here is a defect in THIS tree, never a page's
       doing — aes_gcm.h's paragraph on whose bytes these are. */
    DCHECK(iv_len >= 1u, "§5.2.1.1 Input Data requires 1 <= len(IV) <= 2^64-1 and this invocation declared an "
                         "EMPTY initialization vector — §29.4.1 does not state that lower bound, so the "
                         "SubtleCrypto method owes an OperationError for it before the mode is begun");
    /* §29.4.1 steps 3-4's seven tag lengths, in bytes. Already refused with an OperationError by the caller
       for anything else, so a value outside them was composed here. */
    DCHECK(tag_len == 4u || tag_len == 8u || tag_len == 12u || tag_len == 13u ||
           tag_len == 14u || tag_len == 15u || tag_len == 16u,
           "a tag length outside §29.4.1 step 4's seven (32, 64, 96, 104, 112, 120 or 128 bits) reached the "
           "mode — that step throws an OperationError for every other value, so this one was not the page's");

    memset(g, 0, sizeof *g);
    aes_init(&g->aes, key, key_len);

    /* §7.1 step 1 / §7.2 step 2: "Let H = CIPH_K(0^128)." */
    aes_encrypt_block(&g->aes, ZERO, g->h);

    g->iv_len     = iv_len;
    g->tag_len    = (uint8_t)tag_len;
    g->decrypting = decrypting ? 1u : 0u;
    g->phase      = (uint8_t)AES_GCM_PH_IV;
    /* §6.4 Algorithm 2 step 2: "Let Y_0 be the zero block, 0^128." The memset above is that, and it serves the
       IV's own GHASH in the len(IV) != 96 arm before it serves step 5's. */
}

uint64_t aes_gcm_iv_left(const AesGcm *g)
{
    DCHECK(g != NULL, "aes_gcm_iv_left was given no state");
    DCHECK(g->phase == (uint8_t)AES_GCM_PH_IV, "the IV walk was measured after §7.1 step 2 had already closed");
    DCHECK(g->iv_taken <= g->iv_len, "more of the IV has been fed than the caller declared");
    return g->iv_len - g->iv_taken;
}

void aes_gcm_iv_update(AesGcm *g, const uint8_t *p, size_t n)
{
    DCHECK(g != NULL, "aes_gcm_iv_update was given no state");
    DCHECK(p != NULL || n == 0, "aes_gcm_iv_update was given a null pointer with a non-zero length");
    DCHECK(g->phase == (uint8_t)AES_GCM_PH_IV, "the IV was extended after §7.1 step 2 had closed J_0");
    /* A CHECK AND NOT A DCHECK, for aes_init's reason: the len(IV) = 96 arm below copies into a FIXED sixteen-
       byte J_0, so a caller that fed more than it declared would write past the record in a build where DCHECK
       is compiled out. The bound is also what makes the GHASH arm's accounting true, which is a wrong answer
       rather than an overflow — but one assertion covers both and the stricter consequence decides the macro. */
    CHECK((uint64_t)n <= g->iv_len - g->iv_taken,
          "more IV was fed than the length aes_gcm_begin was given — the declared length is what decides "
          "which of §7.1 step 2's two arms J_0 takes, so it cannot be discovered as the bytes arrive");

    if (g->iv_len == 12u) {
        /* §7.1 step 2, first arm: "If len(IV)=96, then let J_0 = IV || 0^31 || 1." The IV is simply kept; the
           trailing word is written when the walk closes. */
        memcpy(&g->j0[g->iv_taken], p, n);
    } else {
        /* Second arm: the IV is the leading part of a GHASH input, absorbed a block at a time like any other
           region. This is the arm that makes an IV of the page's size preemptible. */
        size_t i;
        for (i = 0; i < n; i++) {
            g->buf[g->buf_len++] = p[i];
            if (g->buf_len == AES_BLOCK) { ghash_block(g, g->buf); g->buf_len = 0; }
        }
    }
    g->iv_taken += n;
}

void aes_gcm_iv_end(AesGcm *g)
{
    DCHECK(g != NULL, "aes_gcm_iv_end was given no state");
    DCHECK(g->phase == (uint8_t)AES_GCM_PH_IV, "§7.1 step 2 was closed twice");
    DCHECK(g->iv_taken == g->iv_len,
           "§7.1 step 2 was closed with part of the IV unfed — J_0 would be formed from a prefix of the "
           "initialization vector the caller declared, which is a different J_0 and a different ciphertext");

    if (g->iv_len == 12u) {
        /* "|| 0^31 || 1": the four bytes after the twelve-byte IV are 00 00 00 01. */
        g->j0[12] = 0; g->j0[13] = 0; g->j0[14] = 0; g->j0[15] = 1;
    } else {
        /* "let s = 128 * ceil(len(IV)/128) - len(IV), and let J_0 = GHASH_H(IV || 0^(s+64) || [len(IV)]_64)".
           The 0^s brings the IV up to a block boundary, which ghash_pad does; the 0^64 and the length together
           are then one further block. */
        uint8_t last[AES_BLOCK];
        ghash_pad(g);
        memset(last, 0, 8);
        put_len_bits(&last[8], g->iv_len);
        ghash_block(g, last);
        memcpy(g->j0, g->y, AES_BLOCK);
        /* §7.1 step 5's GHASH is a SEPARATE application of Algorithm 2 and so restarts at step 2's zero block.
           Carrying this one's Y forward would hash the IV into S a second time. */
        memset(g->y, 0, AES_BLOCK);
    }

    /* §7.1 step 3: "Let C = GCTR_K(inc_32(J_0), P)" — the text walk's first counter block is one past J_0,
       and J_0 itself is held back for step 6's tag. */
    memcpy(g->cb, g->j0, AES_BLOCK);
    inc32(g->cb);
    g->phase = (uint8_t)AES_GCM_PH_AAD;
}

/* ---- SP 800-38D §7.1 step 5's A region ----------------------------------------------------------------- */

void aes_gcm_aad_update(AesGcm *g, const uint8_t *p, size_t n)
{
    size_t i;

    DCHECK(g != NULL, "aes_gcm_aad_update was given no state");
    DCHECK(p != NULL || n == 0, "aes_gcm_aad_update was given a null pointer with a non-zero length");
    DCHECK(g->phase == (uint8_t)AES_GCM_PH_AAD,
           "additional authenticated data was fed outside its own region — §7.1 step 5 hashes A || 0^v || C, "
           "in that order, so an A byte after the first C byte would land in a different place in S");
    DCHECK(g->aad_len <= ~(uint64_t)0 >> 3,
           "len(A) has passed the 2^61-byte point at which [len(A)]_64 can no longer state it in BITS");

    for (i = 0; i < n; i++) {
        g->buf[g->buf_len++] = p[i];
        if (g->buf_len == AES_BLOCK) { ghash_block(g, g->buf); g->buf_len = 0; }
    }
    g->aad_len += n;
}

void aes_gcm_aad_end(AesGcm *g)
{
    DCHECK(g != NULL, "aes_gcm_aad_end was given no state");
    DCHECK(g->phase == (uint8_t)AES_GCM_PH_AAD, "the A region was closed twice");
    ghash_pad(g);                       /* §7.1 step 4's 0^v */
    g->phase = (uint8_t)AES_GCM_PH_TEXT;
}

/* ---- SP 800-38D §6.5's GCTR and §7.1 step 5's C region, interleaved ------------------------------------ */

void aes_gcm_text_update(AesGcm *g, const uint8_t *in, uint8_t *out, size_t n)
{
    size_t i;

    DCHECK(g != NULL, "aes_gcm_text_update was given no state");
    DCHECK((in != NULL && out != NULL) || n == 0,
           "aes_gcm_text_update was given a null buffer with a non-zero length");
    DCHECK(g->phase == (uint8_t)AES_GCM_PH_TEXT,
           "text was fed outside its own region — the A region must be closed first, because §7.1 step 5 pads "
           "A to a block boundary before C begins and a late A byte would move every C byte in S");
    DCHECK(g->txt_len <= ~(uint64_t)0 >> 3,
           "len(C) has passed the 2^61-byte point at which [len(C)]_64 can no longer state it in BITS");

    for (i = 0; i < n; i++) {
        uint8_t c;

        /* §6.5 Algorithm 3 steps 5-6: a fresh keystream block per counter block, CB_{i+1} = inc_32(CB_i). The
           cursor into it is buf_len, which is also the cursor into the GHASH buffer — in this region the byte
           being enciphered and the byte being absorbed are one byte's two halves, so a second cursor would be
           a second spelling of one fact and could disagree with it. */
        if (g->buf_len == 0) {
            aes_encrypt_block(&g->aes, g->cb, g->ks);
            inc32(g->cb);
        }

        /* The CIPHERTEXT byte is what §7.1 step 5 and §7.2 step 6 absorb, in BOTH directions — which is the
           only difference between Algorithm 4 and Algorithm 5 inside this loop. Encrypting, it is the byte we
           just produced; decrypting, it is the byte we were given. */
        if (g->decrypting) {
            c      = in[i];
            out[i] = (uint8_t)(c ^ g->ks[g->buf_len]);
        } else {
            c      = (uint8_t)(in[i] ^ g->ks[g->buf_len]);
            out[i] = c;
        }

        g->buf[g->buf_len++] = c;
        if (g->buf_len == AES_BLOCK) { ghash_block(g, g->buf); g->buf_len = 0; }
    }
    g->txt_len += n;
}

/* ---- SP 800-38D §7.1 steps 4-6 / §7.2 steps 5-8: the tag ----------------------------------------------- */

/* The shared body of §7.1 step 6 and §7.2 step 7, which are the same sentence: "Let T = MSB_t(GCTR_K(J_0, S))".
   It closes the C region first with step 4's 0^u and step 5's two lengths. */
static void gcm_tag(AesGcm *g, uint8_t out[AES_BLOCK])
{
    uint8_t last[AES_BLOCK];
    uint8_t ks[AES_BLOCK];
    unsigned i;

    ghash_pad(g);                                       /* step 4's 0^u */
    put_len_bits(&last[0], g->aad_len);                 /* step 5's [len(A)]_64 */
    put_len_bits(&last[8], g->txt_len);                 /* step 5's [len(C)]_64 */
    ghash_block(g, last);                               /* S is now g->y */

    /* GCTR_K(J_0, S) over one block, which is §6.5 Algorithm 3 with n = 1: the initial counter block is J_0
       ITSELF and not inc_32(J_0), which is the whole reason j0 was kept. MSB_t is the caller's truncation. */
    aes_encrypt_block(&g->aes, g->j0, ks);
    for (i = 0; i < AES_BLOCK; i++) out[i] = (uint8_t)(g->y[i] ^ ks[i]);
}

void aes_gcm_encrypt_finish(AesGcm *g, uint8_t *tag, size_t tag_size)
{
    uint8_t full[AES_BLOCK];

    DCHECK(g != NULL, "aes_gcm_encrypt_finish was given no state");
    DCHECK(!g->decrypting, "§7.1's Algorithm 4 was finished on a state begun for §7.2's Algorithm 5 — a "
                           "decryption must answer step 8's comparison and never hand back a tag it computed");
    DCHECK(g->phase == (uint8_t)AES_GCM_PH_TEXT,
           "a tag was produced twice, or before the A region closed — §7.1 step 6 runs once, at the end");
    /* secure_hash_finish's argument: a caller that sized for a 32-bit tag and asked for a 128-bit one would
       write twelve bytes past its buffer with the right answer in it, so the size is CHECKED and not trusted.
       It is a CHECK and not a DCHECK because the consequence is a heap overflow in release too. */
    CHECK(tag != NULL && tag_size == g->tag_len,
          "§7.1 step 6's T was written into a buffer that is not the tagLength this invocation was begun with");

    gcm_tag(g, full);
    memcpy(tag, full, g->tag_len);      /* MSB_t: the LEFT-most t bits, which is the leading t bytes */
    g->phase = (uint8_t)AES_GCM_PH_DONE;
}

bool aes_gcm_decrypt_verify(AesGcm *g, const uint8_t *tag, size_t tag_len)
{
    uint8_t  full[AES_BLOCK];
    uint8_t  diff = 0;
    unsigned i;

    DCHECK(g != NULL, "aes_gcm_decrypt_verify was given no state");
    DCHECK(g->decrypting, "§7.2's Algorithm 5 step 8 was asked of a state begun for §7.1's Algorithm 4");
    DCHECK(g->phase == (uint8_t)AES_GCM_PH_TEXT, "§7.2 step 8 was answered twice, or before the A region closed");
    DCHECK(tag != NULL || tag_len == 0, "aes_gcm_decrypt_verify was given no tag to compare against");

    gcm_tag(g, full);
    g->phase = (uint8_t)AES_GCM_PH_DONE;

    /* §7.2 step 1 answers FAIL for "len(T) != t" before any comparison, and that length is the page's own
       argument rather than a secret, so it is decided first and separately. */
    if (tag_len != g->tag_len) return false;

    /* Step 8's "If T' = T", with no early exit: every byte contributes to `diff` and the test happens once, so
       nothing observable depends on WHERE two equal-length tags differ. */
    for (i = 0; i < g->tag_len; i++) diff = (uint8_t)(diff | (full[i] ^ tag[i]));
    return diff == 0;
}
