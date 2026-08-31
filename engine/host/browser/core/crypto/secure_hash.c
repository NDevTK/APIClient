/* FIPS PUB 180-4's Secure Hash Standard — see secure_hash.h for the ladder this landed on and the exact
 * sections each table and each loop is transcribed from.
 *
 * THE CONSTANTS ARE THE STANDARD'S OWN BYTES AND NOTHING IS DERIVED HERE. FIPS 180-4 §4.2.2 and §4.2.3 give K
 * as hex words, §5.3.1 / §5.3.3 / §5.3.4 / §5.3.5 give each H(0) the same way, and the reason those sections
 * state them rather than the recurrence that produces them ("the first thirty-two bits of the fractional parts
 * of the cube roots of the first sixty-four prime numbers") is precisely that an implementation must not
 * recompute them. So they are copied, in the standard's own order, and the count of each table is asserted
 * against the number of rounds that reads it — a table one word short is a silently wrong digest and the last
 * place it would surface is at the value.
 *
 * WHY TWO COMPRESSION FUNCTIONS AND NOT ONE PARAMETERISED BY WORD SIZE. §4.1.2 and §4.1.3 are different
 * functions over different word widths with different rotation amounts and different round counts; folding
 * them into one body parameterised by `w` would be this file's own reading of a similarity the standard does
 * not state, and every rotation would then be an expression instead of a number that can be checked against
 * equation (4.4)-(4.13) by eye. SHA-1 is a third, and shares only its padding.
 *
 * THE KNOWN-ANSWER TESTS ARE NOT HERE. A self-consistent round trip proves nothing about a cryptographic
 * primitive, so what this file is checked by is the published vectors from RFC 6234 §8.5's test driver, driven
 * from engine/host/test_forced.c. What IS here is the invariants a caller can violate — a block that is not a
 * block, a length that cannot be padded, a digest written into a buffer sized for a different algorithm. */
#include <string.h>

#include "check.h"
#include "core/crypto/secure_hash.h"

/* ---- FIPS 180-4 §4.2.1: SHA-1's four constants, one per twenty-round window --------------------------- */
static const uint32_t SHA1_K[4] = { 0x5a827999u, 0x6ed9eba1u, 0x8f1bbcdcu, 0xca62c1d6u };

/* ---- FIPS 180-4 §4.2.2: SHA-224 and SHA-256's sixty-four 32-bit constants ------------------------------ */
static const uint32_t SHA256_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

/* ---- FIPS 180-4 §4.2.3: SHA-384, SHA-512, SHA-512/224 and SHA-512/256's eighty 64-bit constants -------- */
static const uint64_t SHA512_K[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
};

/* ---- FIPS 180-4 §5.3: the initial hash values ---------------------------------------------------------- */
/* §5.3.1 SHA-1 — five words; the three this record does not use are never read (SHA1_H is indexed 0..4). */
static const uint32_t SHA1_H[5]   = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u };
/* §5.3.3 SHA-256 */
static const uint32_t SHA256_H[8] = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
};
/* §5.3.4 SHA-384 */
static const uint64_t SHA384_H[8] = {
    0xcbbb9d5dc1059ed8ull, 0x629a292a367cd507ull, 0x9159015a3070dd17ull, 0x152fecd8f70e5939ull,
    0x67332667ffc00b31ull, 0x8eb44a8768581511ull, 0xdb0c2e0d64f98fa7ull, 0x47b5481dbefa4fa4ull,
};
/* §5.3.5 SHA-512 */
static const uint64_t SHA512_H[8] = {
    0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
    0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull,
};

/* ---- FIPS 180-4 §3.2's word operations, and §4.1's six logical functions ------------------------------- */
/* §3.2 note 6: ROTR^n(x) = (x >> n) OR (x << (w-n)). Written for each width separately because `w` is fixed
   per algorithm and a shared macro would need it as a parameter that no equation in §4.1 has. */
static uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

/* §4.1.2, equations (4.2)-(4.7); RFC 6234 §5.1 states the same six in text. */
static uint32_t ch32(uint32_t x, uint32_t y, uint32_t z)  { return (x & y) ^ (~x & z); }
static uint32_t maj32(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
static uint32_t bsig0_32(uint32_t x) { return rotr32(x, 2) ^ rotr32(x, 13) ^ rotr32(x, 22); }
static uint32_t bsig1_32(uint32_t x) { return rotr32(x, 6) ^ rotr32(x, 11) ^ rotr32(x, 25); }
static uint32_t ssig0_32(uint32_t x) { return rotr32(x, 7) ^ rotr32(x, 18) ^ (x >> 3); }
static uint32_t ssig1_32(uint32_t x) { return rotr32(x, 17) ^ rotr32(x, 19) ^ (x >> 10); }

/* §4.1.3, equations (4.8)-(4.13); RFC 6234 §5.2 states the same six in text. */
static uint64_t ch64(uint64_t x, uint64_t y, uint64_t z)  { return (x & y) ^ (~x & z); }
static uint64_t maj64(uint64_t x, uint64_t y, uint64_t z) { return (x & y) ^ (x & z) ^ (y & z); }
static uint64_t bsig0_64(uint64_t x) { return rotr64(x, 28) ^ rotr64(x, 34) ^ rotr64(x, 39); }
static uint64_t bsig1_64(uint64_t x) { return rotr64(x, 14) ^ rotr64(x, 18) ^ rotr64(x, 41); }
static uint64_t ssig0_64(uint64_t x) { return rotr64(x, 1) ^ rotr64(x, 8) ^ (x >> 7); }
static uint64_t ssig1_64(uint64_t x) { return rotr64(x, 19) ^ rotr64(x, 61) ^ (x >> 6); }

/* §4.1.1's Parity, the one function SHA-1 uses that the SHA-2 families do not. */
static uint32_t parity32(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }

/* §5.2's PARSING: "the first 32 bits of message block i are denoted M(i)0" — big-endian, which is what makes
   the message a sequence of words at all. Read rather than cast, because a block arrives at whatever alignment
   the page's ArrayBuffer has. */
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint64_t be64(const uint8_t *p)
{
    return ((uint64_t)be32(p) << 32) | (uint64_t)be32(p + 4);
}
static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void put_be64(uint8_t *p, uint64_t v)
{
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)v);
}

size_t secure_hash_digest_size(SecureHashAlgorithm alg)
{
    switch (alg) {
    case SECURE_HASH_SHA1:   return 20;   /* Figure 1: 160 bits */
    case SECURE_HASH_SHA256: return 32;   /* Figure 1: 256 bits */
    case SECURE_HASH_SHA384: return 48;   /* Figure 1: 384 bits */
    case SECURE_HASH_SHA512: return 64;   /* Figure 1: 512 bits */
    }
    DFAIL("a digest size was asked for an algorithm FIPS 180-4 Figure 1 does not list among the four this "
          "engine computes — the enum is closed and a value outside it can only come from a byte that was "
          "copied over, never from a caller naming an algorithm");
    return 0;
}

size_t secure_hash_block_size(SecureHashAlgorithm alg)
{
    switch (alg) {
    case SECURE_HASH_SHA1:
    case SECURE_HASH_SHA256: return 64;    /* Figure 1: 512 bits */
    case SECURE_HASH_SHA384:
    case SECURE_HASH_SHA512: return 128;   /* Figure 1: 1024 bits */
    }
    DFAIL("a block size was asked for an algorithm FIPS 180-4 Figure 1 does not list");
    return 0;
}

/* ---- Web Cryptography §32.2 Registration's four names, which this enum already IS ----------------------- */

/* THE NAMES LIVE BESIDE THE ENUM BECAUSE THE ENUM'S MEMBERSHIP IS THEIR SENTENCE. secure_hash.h quotes §32.2
   Registration to say why there are exactly four rows and not FIPS 180-4's seven — "The recognized algorithm
   names are "SHA-1", "SHA-256", "SHA-384", and "SHA-512" for the respective SHA algorithms" — so which four and
   what each is called are one fact, and a second table of these strings in whichever component needs one is the
   copy that drifts. §31.6.4 step 14 writes one of these into a key's HmacKeyAlgorithm and §18.4.4 step 5 looks
   one up; both read this. */
const char *secure_hash_name(SecureHashAlgorithm alg)
{
    switch (alg) {
    case SECURE_HASH_SHA1:   return "SHA-1";
    case SECURE_HASH_SHA256: return "SHA-256";
    case SECURE_HASH_SHA384: return "SHA-384";
    case SECURE_HASH_SHA512: return "SHA-512";
    }
    DFAIL("a §32.2 algorithm name was asked for a value this enum does not declare");
    return "";
}

/* THE INVERSE, AND IT IS AN EXACT MATCH RATHER THAN §18.4.4 step 5's CASE-INSENSITIVE ONE. That step's input is
   the PAGE'S string and so its comparison is "a case-insensitive string match for algName"; this one's input is
   a name THIS ENGINE wrote — out of secure_hash_name, into a key's [[algorithm]] — so a mismatch of case would
   be a defect in this file rather than a spelling a page is entitled to. The two questions are not the same
   question and sharing one comparison would answer the page's leniently or this engine's strictly. */
bool secure_hash_by_name(const char *name, SecureHashAlgorithm *out)
{
    static const SecureHashAlgorithm ALL[] = {
        SECURE_HASH_SHA1, SECURE_HASH_SHA256, SECURE_HASH_SHA384, SECURE_HASH_SHA512
    };
    size_t i;

    DCHECK(name != NULL && out != NULL, "secure_hash_by_name was given no name or nowhere to put the answer");
    for (i = 0; i < sizeof ALL / sizeof ALL[0]; i++)
        if (strcmp(name, secure_hash_name(ALL[i])) == 0) { *out = ALL[i]; return true; }
    return false;
}

/* ---- FIPS 180-4 §6.1.2: ONE SHA-1 message block -------------------------------------------------------- */
static void sha1_block(SecureHash *h, const uint8_t *m)
{
    uint32_t w[80], a, b, c, d, e, t;
    int i;

    /* Step 1, the message schedule: W_t = M(i)_t for 0 <= t <= 15, and
       W_t = ROTL^1(W_{t-3} XOR W_{t-8} XOR W_{t-14} XOR W_{t-16}) for 16 <= t <= 79. */
    for (i = 0; i < 16; i++) w[i] = be32(m + 4 * i);
    for (i = 16; i < 80; i++) w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    /* Step 2: initialize the five working variables with the (i-1)st hash value. */
    a = (uint32_t)h->h[0]; b = (uint32_t)h->h[1]; c = (uint32_t)h->h[2];
    d = (uint32_t)h->h[3]; e = (uint32_t)h->h[4];

    /* Step 3: T = ROTL^5(a) + f_t(b,c,d) + e + K_t + W_t; e = d; d = c; c = ROTL^30(b); b = a; a = T.
       §4.1.1's f_t and §4.2.1's K_t both switch on the same twenty-round window, which is why one `if` chain
       selects both rather than two tables indexed by t/20. */
    for (i = 0; i < 80; i++) {
        uint32_t f, k;

        if (i < 20)      { f = ch32(b, c, d);      k = SHA1_K[0]; }
        else if (i < 40) { f = parity32(b, c, d);  k = SHA1_K[1]; }
        else if (i < 60) { f = maj32(b, c, d);     k = SHA1_K[2]; }
        else             { f = parity32(b, c, d);  k = SHA1_K[3]; }
        t = rotl32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rotl32(b, 30); b = a; a = t;
    }

    /* Step 4: the ith intermediate hash value. */
    h->h[0] = (uint32_t)(h->h[0] + a); h->h[1] = (uint32_t)(h->h[1] + b); h->h[2] = (uint32_t)(h->h[2] + c);
    h->h[3] = (uint32_t)(h->h[3] + d); h->h[4] = (uint32_t)(h->h[4] + e);
}

/* ---- FIPS 180-4 §6.2.2: ONE SHA-256 message block ------------------------------------------------------ */
static void sha256_block(SecureHash *h, const uint8_t *m)
{
    uint32_t w[64], a, b, c, d, e, f, g, hh, t1, t2;
    int i;

    /* Step 1: W_t = M(i)_t for 0 <= t <= 15, and
       W_t = SSIG1(W_{t-2}) + W_{t-7} + SSIG0(W_{t-15}) + W_{t-16} for 16 <= t <= 63. */
    for (i = 0; i < 16; i++) w[i] = be32(m + 4 * i);
    for (i = 16; i < 64; i++)
        w[i] = ssig1_32(w[i - 2]) + w[i - 7] + ssig0_32(w[i - 15]) + w[i - 16];

    /* Step 2 */
    a = (uint32_t)h->h[0]; b = (uint32_t)h->h[1]; c  = (uint32_t)h->h[2]; d  = (uint32_t)h->h[3];
    e = (uint32_t)h->h[4]; f = (uint32_t)h->h[5]; g  = (uint32_t)h->h[6]; hh = (uint32_t)h->h[7];

    /* Step 3: T1 = h + BSIG1(e) + CH(e,f,g) + K_t + W_t; T2 = BSIG0(a) + MAJ(a,b,c); then the shift. */
    for (i = 0; i < 64; i++) {
        t1 = hh + bsig1_32(e) + ch32(e, f, g) + SHA256_K[i] + w[i];
        t2 = bsig0_32(a) + maj32(a, b, c);
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }

    /* Step 4 */
    h->h[0] = (uint32_t)(h->h[0] + a); h->h[1] = (uint32_t)(h->h[1] + b);
    h->h[2] = (uint32_t)(h->h[2] + c); h->h[3] = (uint32_t)(h->h[3] + d);
    h->h[4] = (uint32_t)(h->h[4] + e); h->h[5] = (uint32_t)(h->h[5] + f);
    h->h[6] = (uint32_t)(h->h[6] + g); h->h[7] = (uint32_t)(h->h[7] + hh);
}

/* ---- FIPS 180-4 §6.4.2: ONE SHA-512 message block, which §6.5 makes SHA-384's too -------------------- */
static void sha512_block(SecureHash *h, const uint8_t *m)
{
    uint64_t w[80], a, b, c, d, e, f, g, hh, t1, t2;
    int i;

    /* Step 1: sixteen words parsed, sixty-four derived. */
    for (i = 0; i < 16; i++) w[i] = be64(m + 8 * i);
    for (i = 16; i < 80; i++)
        w[i] = ssig1_64(w[i - 2]) + w[i - 7] + ssig0_64(w[i - 15]) + w[i - 16];

    /* Step 2 */
    a = h->h[0]; b = h->h[1]; c  = h->h[2]; d  = h->h[3];
    e = h->h[4]; f = h->h[5]; g  = h->h[6]; hh = h->h[7];

    /* Step 3 */
    for (i = 0; i < 80; i++) {
        t1 = hh + bsig1_64(e) + ch64(e, f, g) + SHA512_K[i] + w[i];
        t2 = bsig0_64(a) + maj64(a, b, c);
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }

    /* Step 4 */
    h->h[0] += a; h->h[1] += b; h->h[2] += c; h->h[3] += d;
    h->h[4] += e; h->h[5] += f; h->h[6] += g; h->h[7] += hh;
}

static void secure_hash_compress(SecureHash *h, const uint8_t *block)
{
    switch ((SecureHashAlgorithm)h->alg) {
    case SECURE_HASH_SHA1:   sha1_block(h, block);   return;
    case SECURE_HASH_SHA256: sha256_block(h, block); return;
    case SECURE_HASH_SHA384:
    case SECURE_HASH_SHA512: sha512_block(h, block); return;
    }
    DFAIL("a message block was compressed under an algorithm outside SecureHashAlgorithm — the context's `alg` "
          "byte is written once by secure_hash_init and read by everything else, so a value here means the "
          "record was copied from something that is not one");
}

void secure_hash_init(SecureHash *h, SecureHashAlgorithm alg)
{
    int i;

    DCHECK(h != NULL, "secure_hash_init was given no context");
    memset(h, 0, sizeof *h);
    h->alg = (uint8_t)alg;
    switch (alg) {
    case SECURE_HASH_SHA1:
        for (i = 0; i < 5; i++) h->h[i] = SHA1_H[i];
        break;
    case SECURE_HASH_SHA256:
        for (i = 0; i < 8; i++) h->h[i] = SHA256_H[i];
        break;
    case SECURE_HASH_SHA384:
        for (i = 0; i < 8; i++) h->h[i] = SHA384_H[i];
        break;
    case SECURE_HASH_SHA512:
        for (i = 0; i < 8; i++) h->h[i] = SHA512_H[i];
        break;
    default:
        DFAIL("secure_hash_init was asked for an algorithm outside the four FIPS 180-4 sections this engine "
              "ports — §5.3 has no initial hash value to set for it");
    }
    /* THE TABLES AND THE ROUND COUNTS ARE ONE FACT, asserted where both are in scope. A K table one word short
       of its loop is a digest that is wrong in the last rounds and right everywhere a short message looks. */
    DCHECK(sizeof SHA256_K / sizeof SHA256_K[0] == 64,
           "FIPS 180-4 §4.2.2 gives SHA-256 sixty-four constants and §6.2.2 step 3 runs t = 0 to 63");
    DCHECK(sizeof SHA512_K / sizeof SHA512_K[0] == 80,
           "FIPS 180-4 §4.2.3 gives SHA-512 eighty constants and §6.4.2 step 3 runs t = 0 to 79");
    DCHECK(secure_hash_block_size(alg) <= SECURE_HASH_MAX_BLOCK &&
           secure_hash_digest_size(alg) <= SECURE_HASH_MAX_DIGEST,
           "an algorithm's Figure 1 block or digest size does not fit the context this file declares for it");
}

void secure_hash_update(SecureHash *h, const uint8_t *data, size_t len)
{
    size_t block = secure_hash_block_size((SecureHashAlgorithm)h->alg);
    size_t i = 0;

    DCHECK(h != NULL, "secure_hash_update was given no context");
    DCHECK(!h->done, "a message was extended after its padding had been written — §5.1 pads ONCE, at the end "
                     "of the message, so a context that has been finished describes a padded message and not "
                     "the one the caller thinks it is still building");
    DCHECK(data != NULL || len == 0, "secure_hash_update was given a null message with a non-zero length");
    /* §5.1's L is a BIT length, so the byte counter must stay below 2^61 for `8 * len` not to wrap. A message
       that large cannot exist in this engine (it would not fit the heap), which is exactly why the impossible
       state is asserted rather than handled. */
    DCHECK(h->len <= (~(uint64_t)0 >> 3) - len,
           "a message reached 2^61 bytes, at which point FIPS 180-4 §5.1.1's 64-bit length field can no longer "
           "state its bit length — this engine cannot hold one, so the counter has been corrupted");
    h->len += len;

    if (h->buf_len > 0) {
        size_t want = block - h->buf_len;

        if (len < want) {
            memcpy(h->buf + h->buf_len, data, len);
            h->buf_len += (uint32_t)len;
            return;
        }
        memcpy(h->buf + h->buf_len, data, want);
        secure_hash_compress(h, h->buf);
        h->buf_len = 0;
        i = want;
    }
    for (; i + block <= len; i += block)
        secure_hash_compress(h, data + i);
    if (i < len) {
        memcpy(h->buf, data + i, len - i);
        h->buf_len = (uint32_t)(len - i);
    }
    DCHECK(h->buf_len < block, "a whole block was left unhashed in the partial-block buffer — the loop above "
                               "consumes every complete block, so the remainder is always short of one");
}

void secure_hash_finish(SecureHash *h, uint8_t *out, size_t out_size)
{
    size_t block = secure_hash_block_size((SecureHashAlgorithm)h->alg);
    size_t digest = secure_hash_digest_size((SecureHashAlgorithm)h->alg);
    /* §5.1.1 appends a 64-bit length, §5.1.2 a 128-bit one — so the length field is one eighth of the block in
       both families, which is why one expression serves both and neither is a magic number. */
    size_t lenfield = block / 8;
    uint64_t bits = h->len << 3;
    size_t i;

    DCHECK(h != NULL, "secure_hash_finish was given no context");
    DCHECK(!h->done, "a digest was finished twice — the second call would pad the padding and answer a hash of "
                     "a message nobody supplied");
    CHECK(out != NULL && out_size >= digest,
          "a digest was written into a buffer smaller than FIPS 180-4 Figure 1's Message Digest Size for the "
          "algorithm asked for — the size is passed rather than assumed precisely so a caller that sized for "
          "SHA-256 and asked for SHA-512 fails here instead of overrunning with a correct answer in it");

    /* §5.1: append a "1" bit (the message is a whole number of bytes here, so that is one 0x80 byte), then K
       zero bits, then the length. The one-byte 0x80 always fits, because buf_len is short of a block. */
    DCHECK(h->buf_len < block, "the padding started with a complete unhashed block still buffered");
    h->buf[h->buf_len++] = 0x80;
    if (h->buf_len > block - lenfield) {
        memset(h->buf + h->buf_len, 0, block - h->buf_len);
        secure_hash_compress(h, h->buf);
        h->buf_len = 0;
    }
    memset(h->buf + h->buf_len, 0, block - lenfield - h->buf_len);
    if (lenfield == 16)
        put_be64(h->buf + block - 16, 0);   /* §5.1.2's length is 128 bits; this engine's high half is zero */
    put_be64(h->buf + block - 8, bits);
    secure_hash_compress(h, h->buf);
    h->buf_len = 0;
    h->done = 1;

    /* §6.x's final concatenation, truncated where the algorithm truncates: SHA-1 concatenates its five 32-bit
       words, SHA-256 its eight, SHA-512 its eight 64-bit words, and §6.5 makes SHA-384 the leftmost SIX of
       SHA-512's — which is what `digest` already says, so the loop reads the size rather than restating it. */
    if (block == 64) {
        for (i = 0; i < digest; i += 4)
            put_be32(out + i, (uint32_t)h->h[i / 4]);
    } else {
        for (i = 0; i < digest; i += 8)
            put_be64(out + i, h->h[i / 8]);
    }
    DCHECK(i == digest, "the final concatenation did not write exactly Figure 1's Message Digest Size — every "
                        "digest size in that figure is a whole number of the algorithm's words, so a remainder "
                        "here means a size and a word width that do not belong to the same row");
}
