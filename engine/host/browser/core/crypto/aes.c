/* FIPS 197's CIPHER() and KEYEXPANSION(), transcribed from Algorithm 1 and Algorithm 2. See aes.h for the
 * bind-before-build ladder this lands on, for why there is no inverse cipher, and for why the S-box below is
 * derived rather than copied.
 *
 * IT IS BYTE-ORIENTED AND STATE-ARRAY SHAPED RATHER THAN TABLE-DRIVEN, DELIBERATELY. The fast way to write AES
 * is four 1 KiB T-tables that fold SUBBYTES(), SHIFTROWS() and MIXCOLUMNS() into one lookup per column; it is
 * also a TRANSFORMATION of the specification that a reader cannot check against Algorithm 1 by eye, and
 * core/crypto/secure_hash.c states the same preference for the same reason — "every rotation would then be an
 * expression instead of a number that can be checked against equation (4.4)-(4.13) by eye". What this engine
 * needs from a cipher is that it be RIGHT and that it rest between blocks, not that it be fast: the scheduler
 * hands it one §5.1 block per turn and yields, so a table-driven inner loop would buy throughput the design
 * spends anyway. A second, faster spelling beside this one is the dual system CLAUDE.md forbids.
 *
 * THE KNOWN-ANSWER TESTS ARE NOT HERE. A self-consistent round trip proves nothing about a cryptographic
 * primitive, so what this file is checked by is the published vectors of NIST's AES Algorithm Validation Suite
 * — the GFSbox, KeySbox, VarKey and VarTxt known-answer sets for all three key lengths — driven from
 * engine/host/test_forced.c. What IS here is the invariants a CALLER can violate. */
#include <string.h>

#include "check.h"
#include "core/crypto/aes.h"

/* THE RECORD IS POD, AND ITS SIZE IS WHAT PROVES IT — core/crypto/secure_hash.c's argument, owed again here
   because this record is the one that would be replaced by a vendored context. Every member is fixed-width,
   and a POINTER is eight bytes natively and FOUR under emcc's wasm32, so once one is added no single constant
   can satisfy both of the targets engine/build.mjs compiles. A deliberate layout change updates this number; a
   pointer cannot, which is the case worth refusing — mbedTLS's own GCM context takes a `mbedtls_cipher_context_t`
   holding two pointers whenever MBEDTLS_BLOCK_CIPHER_C is undefined, and would otherwise be dropped in here and
   read correctly for a whole session, failing only on a cold-tier resume that restores the bytes into a
   different address space. */
_Static_assert(sizeof(Aes) == 241,
               "FIPS 197's key schedule record is no longer 241 bytes. If a POINTER was added it can no longer "
               "ride a COW snapshot or a cold-tier resume — see aes.h's own paragraph on why this record holds "
               "none. If the layout changed deliberately, update this number.");
_Static_assert(AES_SCHEDULE_MAX == 240, "§5.2 generates 4*(Nr+1) words and AES-256's Nr is 14, so the largest "
                                        "schedule is 60 words of four bytes");

/* ---- FIPS 197 §5.1.1 SUBBYTES(): Table 4's S-box ------------------------------------------------------- */

/* "SUBBYTES() is an invertible, non-linear transformation of the state in which a substitution table, called
   an S-box, is applied independently to each byte in the state." (§5.1.1, verbatim.)
   THESE BYTES WERE DERIVED FROM THE SECTION'S OWN EQUATIONS AND NOT PROOF-READ OFF TABLE 4, which is the one
   thing that makes a 256-entry constant checkable at all. §5.1.1 defines SBOX(b) as two composed steps: an
   intermediate b~ that is the multiplicative inverse of b in GF(2^8) per §4.4 (and {00} for b = {00}), then the
   affine transformation of equation (5.3), b'_i = b~_i (+) b~_(i+4 mod 8) (+) b~_(i+5 mod 8) (+) b~_(i+6 mod 8)
   (+) b~_(i+7 mod 8) (+) c_i, with c the constant byte {01100011} = {63}. Computing that for all 256 inputs is
   what produced this table. A reader checking it need not compare hex against a PDF: re-run the definition.
   Two facts that fall out of it and are worth stating because each catches a whole class of typo — the table is
   a BIJECTION (all 256 values occur exactly once), and SBOX({53}) is {ed}, which is §5.1.1's own worked example
   sentence. Both hold of these bytes. */
static const uint8_t SBOX[256] = {
    /* 0 */ 0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    /* 1 */ 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    /* 2 */ 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    /* 3 */ 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    /* 4 */ 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    /* 5 */ 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    /* 6 */ 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    /* 7 */ 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    /* 8 */ 0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    /* 9 */ 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    /* a */ 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    /* b */ 0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    /* c */ 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    /* d */ 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    /* e */ 0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    /* f */ 0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,};

/* ---- FIPS 197 §5.2 KEYEXPANSION(): Table 5's round constants ------------------------------------------- */

/* "KEYEXPANSION() invokes 10 fixed words denoted by Rcon[j] for 1 <= j <= 10. These 10 words are called the
   round constants." (§5.2.) Table 5 gives each as [xx,00,00,00], so only the leftmost byte is stored — the
   other three are zero for every j and XORing them is the identity. §5.2's own note says why the sequence is
   what it is: "The value of the left-most byte of Rcon[j] in polynomial form is x^(j-1)", so each entry after
   the first is XTIMES() of the one before it, which is where {80} is followed by {1b} rather than {100}. */
static const uint8_t RCON[10] = {
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
};

/* ---- FIPS 197 §4.2 Multiplication in GF(2^8) ----------------------------------------------------------- */

/* §4.2's XTIMES(): multiplication by x, i.e. a left shift, "followed by a conditional bitwise XOR with {1b}"
   when the byte that shifted out was set — the reduction modulo the standard's own m(x) = x^8 + x^4 + x^3 + x
   + 1, whose low eight coefficients are {1b}.
   IT IS BRANCHLESS BECAUSE THE OPERAND IS SECRET. This runs on round-key and state bytes derived from the
   PAGE'S key, and a conditional on a key bit is a timing oracle for that bit; the mask below is the same
   arithmetic with the branch removed. CLAUDE.md's constant-time rule is stated for §31.6.2's MAC comparison,
   and the reason — an attacker learning where two secret-dependent values differ — is the same reason here. */
static uint8_t xtimes(uint8_t b)
{
    return (uint8_t)((b << 1) ^ (uint8_t)(((b >> 7) & 1u) * 0x1bu));
}

/* §4.2's product of two field elements, by the "shift-and-add" the section describes: accumulate a * x^i for
   each set bit i of b. Only the four constants §5.1.3 MIXCOLUMNS() names ({01}, {02}, {03}) ever reach it, so
   `b` is never secret and its loop is not a leak; `a` is, which is why the accumulation is masked rather than
   branched. */
static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t p = 0;
    int     i;

    for (i = 0; i < 8; i++) {
        p ^= (uint8_t)((uint8_t)(-(int)(b & 1u)) & a);
        a  = xtimes(a);
        b  = (uint8_t)(b >> 1);
    }
    return p;
}

/* ---- FIPS 197 §5.2 KEYEXPANSION(), Algorithm 2 --------------------------------------------------------- */

void aes_init(Aes *a, const uint8_t *key, size_t key_len)
{
    unsigned nk, nw, i, j;
    uint8_t  t[4];

    DCHECK(a != NULL, "aes_init was given no context");
    DCHECK(key != NULL, "aes_init was given no key");
    /* §6.1 Key Length Requirements' three lengths, which are also Web Cryptography §29.4.4 Import Key's three.
       A length outside them is a defect in THIS tree and not a page's doing: §29.4.4 has already answered a
       DataError for one by the time a key reaches here, so this asserts the ENGINE's own logic, never the
       page's bytes — aes.h's paragraph on whose bytes these are. */
    /* A CHECK AND NOT A DCHECK, BECAUSE THE CONSEQUENCE IS AN OVERFLOW AND NOT A WRONG ANSWER. The memcpy
       below writes `key_len` bytes into a fixed 240-byte schedule, and `key_len` is derived from a page's own
       BufferSource — so a caller that let an unvalidated length through would, in a build where DCHECK is
       compiled out, write past the record with no diagnostic at all. secure_hash_finish states the identical
       argument for its own out_size. The three values themselves are still this ENGINE'S fact rather than the
       page's: §29.4.4 Import Key answers a DataError for every other length long before a key reaches here. */
    CHECK(key_len == 16 || key_len == 24 || key_len == 32,
          "a key was expanded at a length FIPS 197 §6.1 does not define — the three are 128, 192 and 256 bits, "
          "and Web Cryptography §29.4.4 Import Key refuses every other length with a DataError before a key "
          "reaches this engine's cipher, so this length was composed here");

    nk    = (unsigned)(key_len / 4u);      /* §2.3's Nk: "the number of 32-bit words comprising the key" */
    a->nr = (uint8_t)(nk + 6u);            /* §5 Table 3: Nr is 10, 12, 14 for Nk of 4, 6, 8 */
    nw    = 4u * ((unsigned)a->nr + 1u);   /* §5.2: "4 * (Nr + 1) words" */
    DCHECK(nw * 4u <= AES_SCHEDULE_MAX, "a key schedule longer than AES-256's would overrun the record");

    /* Algorithm 2 lines 2-4: the first Nk words are the key itself. */
    memcpy(a->rk, key, key_len);

    /* Algorithm 2 lines 5-15: each later word is the one Nk back, exclusive-ored with a temp derived from its
       immediate predecessor. */
    for (i = nk; i < nw; i++) {
        memcpy(t, &a->rk[4u * (i - 1u)], 4);
        if (i % nk == 0) {
            /* Line 8: temp <- SUBWORD(ROTWORD(temp)) XOR Rcon[i/Nk].
               §5.2 equation (5.10): ROTWORD([a0,a1,a2,a3]) = [a1,a2,a3,a0]. */
            uint8_t a0 = t[0];
            t[0] = t[1]; t[1] = t[2]; t[2] = t[3]; t[3] = a0;
            /* equation (5.11): SUBWORD([a0,...,a3]) = [SBOX(a0), ..., SBOX(a3)]. */
            for (j = 0; j < 4u; j++) t[j] = SBOX[t[j]];
            /* Table 5 is indexed from j = 1, so Rcon[i/Nk] is this array's element i/Nk - 1. Only the leftmost
               byte is nonzero, which is why only t[0] is touched. */
            DCHECK(i / nk >= 1u && i / nk <= 10u,
                   "§5.2 Table 5 declares Rcon[j] for 1 <= j <= 10 only, and the walk asked for one outside it");
            t[0] ^= RCON[i / nk - 1u];
        } else if (nk > 6u && i % nk == 4u) {
            /* Lines 10-11: AES-256 alone applies SUBWORD() a second time, four words into each group. §5.2
               states the condition as Nk > 6, which is true of Nk = 8 and of nothing else this file admits. */
            for (j = 0; j < 4u; j++) t[j] = SBOX[t[j]];
        }
        /* Line 13: w[i] <- w[i-Nk] XOR temp. */
        for (j = 0; j < 4u; j++)
            a->rk[4u * i + j] = (uint8_t)(a->rk[4u * (i - nk) + j] ^ t[j]);
    }
}

/* ---- FIPS 197 §5.1 CIPHER(), Algorithm 1 --------------------------------------------------------------- */

/* §5.1.4 ADDROUNDKEY(): "combines a round key with the state". Round `rnd` adds w[4*rnd .. 4*rnd+3], and §3.4
   The State puts word w[j] down COLUMN j — so w[4*rnd+c] byte r lands on s[r][c], which on this layout is the
   byte at rk[4*(4*rnd+c)+r]. Because the state below is held in the same column-major order as the input
   block, that is a straight byte-for-byte exclusive-or of sixteen adjacent bytes. */
static void add_round_key(const Aes *a, uint8_t s[AES_BLOCK], unsigned rnd)
{
    unsigned i;
    for (i = 0; i < AES_BLOCK; i++)
        s[i] ^= a->rk[AES_BLOCK * rnd + i];
}

void aes_encrypt_block(const Aes *a, const uint8_t in[AES_BLOCK], uint8_t out[AES_BLOCK])
{
    /* §3.4 The State: the input is copied in column-major order, so s[r][c] is byte 4*c + r of the block. The
       state is held AS the block for that reason — `s[4*c + r]` is s_{r,c}, and the copy in and the copy out
       of Algorithm 1's lines 2 and 13 are then both memcpy. */
    uint8_t  s[AES_BLOCK];
    uint8_t  col[4];
    unsigned rnd, r, c;

    DCHECK(a != NULL && in != NULL && out != NULL, "aes_encrypt_block was given no context, input or output");
    DCHECK(a->nr == 10 || a->nr == 12 || a->nr == 14,
           "a block was enciphered with a round count FIPS 197 §5 Table 3 does not list — the record was used "
           "before aes_init wrote it, or its bytes were copied over");

    memcpy(s, in, AES_BLOCK);                       /* line 2:  state <- in */
    add_round_key(a, s, 0);                         /* line 3:  ADDROUNDKEY(state, w[0..3]) */

    for (rnd = 1; rnd <= (unsigned)a->nr; rnd++) {
        /* line 5 / line 10: SUBBYTES(state). */
        for (r = 0; r < AES_BLOCK; r++) s[r] = SBOX[s[r]];

        /* line 6 / line 11: SHIFTROWS(state). §5.1.2 shifts row r left CYCLICALLY by r, and on a column-major
           block the bytes of row r are s[r], s[r+4], s[r+8], s[r+12]. Row 0 is not shifted. */
        for (r = 1; r < 4u; r++) {
            for (c = 0; c < 4u; c++) col[c] = s[4u * ((c + r) & 3u) + r];
            for (c = 0; c < 4u; c++) s[4u * c + r] = col[c];
        }

        /* line 7: MIXCOLUMNS(state) — omitted in the final round, which is the ONLY way Algorithm 1's lines
           10-12 differ from its lines 5-8. §5.1.3 states the transformation as the matrix product whose rows
           are [02 03 01 01], [01 02 03 01], [01 01 02 03], [03 01 01 02] over GF(2^8). */
        if (rnd != (unsigned)a->nr) {
            for (c = 0; c < 4u; c++) {
                uint8_t *p = &s[4u * c];
                memcpy(col, p, 4);
                p[0] = (uint8_t)(gmul(col[0], 2) ^ gmul(col[1], 3) ^ col[2]            ^ col[3]);
                p[1] = (uint8_t)(col[0]          ^ gmul(col[1], 2) ^ gmul(col[2], 3)   ^ col[3]);
                p[2] = (uint8_t)(col[0]          ^ col[1]          ^ gmul(col[2], 2)   ^ gmul(col[3], 3));
                p[3] = (uint8_t)(gmul(col[0], 3) ^ col[1]          ^ col[2]            ^ gmul(col[3], 2));
            }
        }

        /* line 8 / line 12: ADDROUNDKEY(state, w[4*round .. 4*round+3]). */
        add_round_key(a, s, rnd);
    }

    memcpy(out, s, AES_BLOCK);                      /* line 13: return state */
}
