/* Web Cryptography API §31 HMAC — FIPS PUB 198-1 §4's nine steps, §31.6.4 Import Key's "raw" arm, and the one
 * sentence §31.6.1 Sign and §31.6.2 Verify share. See hmac.h for the bind-before-build ladder (this lands on
 * rung THREE — core/crypto/secure_hash.c IS the H the construction is written over), for why both of its walks
 * park, and for the warning that no instrument in this tree checks a FIPS citation.
 *
 * FIPS 198-1 §4 HMAC Specification, Table 1: The HMAC Algorithm — quoted from the fetched document, because
 * every line of hmac_begin/_key_update/_key_end/_text_update/_finish below is one of these steps and a reader
 * must be able to check the pair without opening a PDF:
 *
 *   Step 1  If the length of K = B: set K0 = K. Go to step 4.
 *   Step 2  If the length of K > B: hash K to obtain an L byte string, then append (B-L) zeros to create a
 *           B-byte string K0 (i.e., K0 = H(K) || 00...00). Go to step 4.
 *   Step 3  If the length of K < B: append zeros to the end of K to create a B-byte string K0 (e.g., if K is
 *           20 bytes in length and B = 64, then K will be appended with 44 zero bytes x'00').
 *   Step 4  Exclusive-Or K0 with ipad to produce a B-byte string: K0 ^ ipad.
 *   Step 5  Append the stream of data 'text' to the string resulting from step 4: (K0 ^ ipad) || text.
 *   Step 6  Apply H to the stream generated in step 5: H((K0 ^ ipad) || text).
 *   Step 7  Exclusive-Or K0 with opad: K0 ^ opad.
 *   Step 8  Append the result from step 6 to step 7: (K0 ^ opad) || H((K0 ^ ipad) || text).
 *   Step 9  Apply H to the result from step 8: H((K0 ^ opad )|| H((K0 ^ ipad) || text)).
 *
 * FIPS 198-1 §2.3 HMAC Parameters and Symbols supplies the four constants those steps are written in, also
 * verbatim — the standard is repeated here rather than inherited from the banner above, because nothing
 * carries a document across a paragraph and a bare number placed nothing at all:
 *   "B  Block size (in bytes) of the input to the Approved hash function."
 *   "ipad  Inner pad; the byte x'36' repeated B times."
 *   "L  Block size (in bytes) of the output of the Approved hash function."
 *   "opad  Outer pad; the byte x'5c' repeated B times."
 * — so B is secure_hash_block_size and L is secure_hash_digest_size, and neither is a number written here.
 *
 * STEPS 1-3 ARE DECIDED BEFORE A BYTE IS READ, WHICH IS WHY THE BRANCH IS NOT AT THE END OF THE WALK. The three
 * cases turn on len(K) against B, and len(K) is the byte length of the [[handle]] the caller already holds — so
 * hmac_begin asks it once and the walk that follows has one shape. The alternative, hashing K unconditionally
 * and choosing afterwards, would compress a block the standard never asks for and would put the decision at the
 * end of a span that suspends, where a resume would have to re-derive it. §3 Cryptographic Keys is the sentence
 * that makes step 2 the odd one out: "When an application uses a K longer than B-bytes, then it shall first
 * hash the K using H and then use the resultant L-byte string as the key K0".
 *
 * AND STEP 2 IS THE HALF OF THIS ALGORITHM NOBODY EXPECTS TO BE UNBOUNDED. The message is obviously of the
 * page's size; the KEY looks like a small constant and is not — `importKey("raw", new Uint8Array(1<<24), …)` is
 * a legal call, and a browser hashes all of it. So the key walk is preemptible on exactly the same terms as the
 * text walk, and the DCHECK in hmac_key_update is what keeps a caller from feeding it in one go. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/crypto/crypto_key.h"
#include "core/crypto/hmac.h"
#include "core/crypto/secure_hash.h"
#include "core/idl_slots.h"

/* §2.3: "ipad  Inner pad; the byte x'36' repeated B times." / "opad  Outer pad; the byte x'5c' repeated B
   times." The REPETITION is B and lives in the loops below; these are the two bytes. */
#define HMAC_IPAD 0x36u
#define HMAC_OPAD 0x5cu

size_t hmac_mac_size(SecureHashAlgorithm alg)
{
    /* §2.3's L. §31.6.1 returns the MAC in full, so the MAC's length IS L. */
    return secure_hash_digest_size(alg);
}

size_t hmac_block(const Hmac *m)
{
    DCHECK(m != NULL, "hmac_block was given no state");
    return secure_hash_block_size((SecureHashAlgorithm)m->alg);
}

void hmac_begin(Hmac *m, SecureHashAlgorithm alg, uint64_t key_len)
{
    size_t b;

    DCHECK(m != NULL, "hmac_begin was given no state");
    memset(m, 0, sizeof *m);
    m->alg = (uint8_t)alg;
    m->key_len = key_len;
    m->key_taken = 0;
    m->phase = (uint8_t)HMAC_PH_KEY;
    b = secure_hash_block_size(alg);
    DCHECK(b <= SECURE_HASH_MAX_BLOCK,
           "FIPS 198-1 §2.3's B for this hash is larger than the K0 buffer — SECURE_HASH_MAX_BLOCK is FIPS "
           "180-4 Figure 1's largest Block Size and K0 is exactly B bytes, so the two are one fact");
    DCHECK(secure_hash_digest_size(alg) <= b,
           "FIPS 198-1 Table 1 step 2 appends (B-L) zeros, so L must not exceed B — an H whose output is wider "
           "than its input block has no K0 this construction can build");
    /* §3's "a K longer than B-bytes", which is Table 1 step 2 and the ONLY case that hashes the key. Steps 1
       and 3 both copy K into K0 and differ only in whether any zero padding follows, so they are one arm: the
       memset above has already written step 3's "append zeros to the end of K". */
    m->key_over = key_len > (uint64_t)b;
    if (m->key_over)
        secure_hash_init(&m->h, alg);
}

uint64_t hmac_key_left(const Hmac *m)
{
    DCHECK(m != NULL, "hmac_key_left was given no state");
    DCHECK(m->phase == (uint8_t)HMAC_PH_KEY,
           "FIPS 198-1 Table 1 steps 1-3 were asked how much of K is left after the key walk had already been "
           "closed — hmac_key_end is what ends it, and after it K0 is complete and K is not read again");
    DCHECK(m->key_taken <= m->key_len, "more of K has been fed than the key is long");
    return m->key_len - m->key_taken;
}

void hmac_key_update(Hmac *m, const uint8_t *p, size_t n)
{
    size_t b;

    DCHECK(m != NULL, "hmac_key_update was given no state");
    DCHECK(m->phase == (uint8_t)HMAC_PH_KEY,
           "K was extended after FIPS 198-1 Table 1 steps 1-3 had already produced K0 — the key is read once, "
           "before step 4 exclusive-ors K0 with ipad");
    DCHECK(p != NULL || n == 0, "hmac_key_update was given a null key fragment with a non-zero length");
    DCHECK((uint64_t)n <= m->key_len - m->key_taken,
           "the key walk was fed past the end of K — the caller states len(K) at hmac_begin and advances by at "
           "most that many bytes");
    b = secure_hash_block_size((SecureHashAlgorithm)m->alg);
    /* THE UNIT IS B, AND THE ASSERT IS WHAT KEEPS THE WALK PREEMPTIBLE. A caller handing the whole key in one
       call would run FIPS §4 step 2 to completion inside one C activation, which is the drive-to-completion
       CLAUDE.md §The-scheduler forbids at any depth; one B-byte block is the standard's own unit of work and
       the smallest thing this can rest between. The final turn is short, which is why the test is `<=`. */
    DCHECK(n <= b,
           "the key walk was handed more than one FIPS 198-1 §2.3 B-byte block in one turn — this span is of "
           "the PAGE'S size (an importKey over a 16MB Uint8Array is a legal call), so it advances one block "
           "per turn and its caller yields between turns");
    if (m->key_over) {
        /* STEP 2: "hash K to obtain an L byte string". */
        secure_hash_update(&m->h, p, n);
    } else {
        /* STEPS 1 AND 3: K is copied into K0, whose tail is already the zeros step 3 appends. The bound is
           the DCHECK above plus key_len <= B, which is what m->key_over being false means. */
        DCHECK(m->key_taken + (uint64_t)n <= (uint64_t)b,
               "steps 1 and 3 copied past the end of K0 — this arm is reached only for a K no longer than B");
        memcpy(m->k0 + m->key_taken, p, n);
    }
    m->key_taken += (uint64_t)n;
}

void hmac_key_end(Hmac *m)
{
    size_t b, i;
    uint8_t pad[SECURE_HASH_MAX_BLOCK];

    DCHECK(m != NULL, "hmac_key_end was given no state");
    DCHECK(m->phase == (uint8_t)HMAC_PH_KEY, "the key walk was closed twice");
    DCHECK(m->key_taken == m->key_len,
           "FIPS 198-1 Table 1 steps 1-3 were closed with part of K unread — K0 is a function of the WHOLE key, "
           "so a short walk produces a MAC of a key nobody has");
    b = secure_hash_block_size((SecureHashAlgorithm)m->alg);
    if (m->key_over) {
        /* STEP 2, finished: "K0 = H(K) || 00...00". The (B-L) zeros are already there — hmac_begin zeroed the
           whole buffer — so this writes only the L bytes H produced. */
        secure_hash_finish(&m->h, m->k0, sizeof m->k0);
        DCHECK(secure_hash_digest_size((SecureHashAlgorithm)m->alg) <= b,
               "step 2's H(K) is wider than B and would leave no room for its (B-L) zeros");
    }
    /* STEP 4: "Exclusive-Or K0 with ipad to produce a B-byte string: K0 ^ ipad." STEP 5's append is what
       hmac_text_update does, one block at a time, into the hash this opens. */
    secure_hash_init(&m->h, (SecureHashAlgorithm)m->alg);
    for (i = 0; i < b; i++)
        pad[i] = (uint8_t)(m->k0[i] ^ HMAC_IPAD);
    secure_hash_update(&m->h, pad, b);
    m->phase = (uint8_t)HMAC_PH_TEXT;
}

void hmac_text_update(Hmac *m, const uint8_t *p, size_t n)
{
    DCHECK(m != NULL, "hmac_text_update was given no state");
    DCHECK(m->phase == (uint8_t)HMAC_PH_TEXT,
           "FIPS 198-1 Table 1 step 5 appended `text` before step 4 had produced (K0 ^ ipad), or after step 6 "
           "had already hashed the stream it appends to");
    DCHECK(p != NULL || n == 0, "hmac_text_update was given a null message fragment with a non-zero length");
    DCHECK(n <= secure_hash_block_size((SecureHashAlgorithm)m->alg),
           "the message walk was handed more than one FIPS 198-1 §2.3 B-byte block in one turn — `text` is of "
           "the PAGE'S size, so it advances one block per turn and its caller yields between turns");
    secure_hash_update(&m->h, p, n);
}

void hmac_finish(Hmac *m, uint8_t *mac, size_t mac_size)
{
    size_t b, l, i;
    uint8_t pad[SECURE_HASH_MAX_BLOCK];

    DCHECK(m != NULL, "hmac_finish was given no state");
    DCHECK(m->phase == (uint8_t)HMAC_PH_TEXT,
           "a MAC was finished twice, or finished before step 4 opened the inner hash — steps 6-9 run once, and "
           "a second run would hash the padding of the first and answer a MAC of something nobody signed");
    b = secure_hash_block_size((SecureHashAlgorithm)m->alg);
    l = secure_hash_digest_size((SecureHashAlgorithm)m->alg);
    CHECK(mac != NULL && mac_size >= l,
          "FIPS 198-1 §2.3's L bytes of MAC do not fit the buffer they were asked to be written into — the "
          "caller sized for one hash function and named another");
    /* STEP 6: "Apply H to the stream generated in step 5: H((K0 ^ ipad) || text)." */
    secure_hash_finish(&m->h, m->inner, sizeof m->inner);
    /* STEP 7: "Exclusive-Or K0 with opad: K0 ^ opad." */
    secure_hash_init(&m->h, (SecureHashAlgorithm)m->alg);
    for (i = 0; i < b; i++)
        pad[i] = (uint8_t)(m->k0[i] ^ HMAC_OPAD);
    /* STEP 8: "Append the result from step 6 to step 7." STEP 9: "Apply H to the result from step 8." */
    secure_hash_update(&m->h, pad, b);
    secure_hash_update(&m->h, m->inner, l);
    secure_hash_finish(&m->h, mac, mac_size);
    m->phase = (uint8_t)HMAC_PH_DONE;
}

bool hmac_mac_equal(const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len)
{
    size_t i;
    uint8_t diff = 0;

    DCHECK(a != NULL || a_len == 0, "the MAC comparison was given a null operand with a non-zero length");
    DCHECK(b != NULL || b_len == 0, "the MAC comparison was given a null operand with a non-zero length");
    /* THE LENGTH IS NOT A SECRET — see hmac.h. `signature.byteLength` is the page's own argument and the MAC's
       length is L, a constant of the named hash — so a mismatch answers here rather than being folded into a
       comparison that would then have to invent bytes for the shorter operand. */
    if (a_len != b_len)
        return false;
    /* EVERY BYTE IS READ AND THE TEST IS ONCE. §31.6.2 step 2: "This comparison must be performed in
       constant-time." An early `return false` at the first difference is a timing oracle that recovers a
       forged MAC one byte at a time, which is the whole reason the standard writes that sentence. */
    for (i = 0; i < a_len; i++)
        diff = (uint8_t)(diff | (uint8_t)(a[i] ^ b[i]));
    return diff == 0;
}

/* ---- §31.6.4 Import Key ------------------------------------------------------------------------------------ */

/* §31.4's HmacKeyAlgorithm, built as the [[algorithm]] internal slot's contents:
 *
 *   dictionary HmacKeyAlgorithm : KeyAlgorithm {
 *     required KeyAlgorithm hash;
 *     required [EnforceRange] unsigned long length;
 *   };
 *
 * A NULL-PROTOTYPE RECORD, for crypto_key.c's reason: this is the SLOT and never what the page receives.
 * §13.4's `algorithm` answers with §9 Terminology's cached ECMAScript object, which crypto_key_new builds from
 * this and which is a different object precisely so `key.algorithm.name = 'AES-CBC'` cannot change which
 * algorithm the key IS. Handing the page a record with Object.prototype behind it would also let a page's
 * `Object.prototype.hash` answer a read this file makes of its own data. */
static JSValue hmac_key_algorithm_new(JSContext *ctx, SecureHashAlgorithm hash, uint32_t length_bits)
{
    JSValue alg = idl_slots_new(ctx), h;

    CHECK(!JS_IsException(alg), "§31.4's HmacKeyAlgorithm could not be allocated");
    h = idl_slots_new(ctx);
    CHECK(!JS_IsException(h), "§31.6.4 step 4's KeyAlgorithm for the inner hash could not be allocated");
    /* §31.6.4 steps 12-14, in the standard's own order: name, then length, then hash. */
    JS_SetPropertyStr(ctx, alg, "name", JS_NewString(ctx, "HMAC"));
    JS_SetPropertyStr(ctx, alg, "length", JS_NewUint32(ctx, length_bits));
    JS_SetPropertyStr(ctx, h, "name", JS_NewString(ctx, secure_hash_name(hash)));
    JS_SetPropertyStr(ctx, alg, "hash", h);
    return alg;
}

/* ---- §31.6.4 Import Key STEP 5's `jwk` ARM ----------------------------------------------------------------
 *
 * A SUB-NUMBER IS NEVER WRITTEN BARE FOR THIS STEP, AND THAT IS THE STANDARD'S SHAPE RATHER THAN A HOUSE
 * STYLE. §31.6.4's step 5 is a DISPATCH: it holds THREE sibling lists — the `raw` arm's, the `jwk` arm's and
 * the `Otherwise` — each restarting at 1, so a bare "step 5.4" names three different steps and a reader cannot
 * tell which. Every citation below therefore names the arm in the spec's own words ("the jwk arm's step 4"),
 * which is the one form that resolves. engine/citegen.mjs is what caught the bare numbering: it reads step 5's
 * FIRST list, which is the two-item `raw` arm, and reported that there is no step 5.4 — a correct finding
 * about an ambiguous citation rather than a miscount.
 *
 * BASE64URL IS DELEGATED TO THE ENGINE'S OWN DECODER AND IS NOT A SECOND CODEC, and the reason is the order the
 * algorithm already puts things in rather than a convenience. The jwk arm's step 3 is "If jwk does not meet the
 * requirements of Section 6.4 of JSON Web Algorithms [JWA], then throw a DataError", and JWA §6.4.1 "k" (Key
 * Value) Parameter says the value "is represented as the base64url encoding of the octet sequence containing
 * the key value" — where RFC 7515 §2 Terminology defines base64url as "Base64 encoding using the URL- and
 * filename-safe character set defined in Section 5 of RFC 4648 [RFC4648], with all trailing '=' characters
 * omitted (as permitted by Section 3.2) and without the inclusion of any line breaks, whitespace, or other
 * additional characters". So the host OWES that validation as a step of the algorithm: `-`/`_` and no padding,
 * no whitespace, no `+`, no `/`, no `=`. Once it has been performed, what is left is an alphabet substitution
 * over an already-validated string, and the DECODE ITSELF is JS_Base64Decode — the same forgiving-base64 the
 * engine runs for `atob` and for a `data:` URL body. Nothing here re-implements base64: this is
 * §Bind-before-build's "source it from an existing engine intrinsic", and the validation is a spec step rather
 * than a guard invented to make the delegation safe.
 * (The engine's `b64_flags_url` table is not reachable from a host — it feeds a static `from_base64` — so the
 * alternative was a new export from the submodule for a decode this already performs.) */
static int hmac_jwk_k_bytes(JSContext *ctx, const char *k, size_t n, uint8_t **out, uint32_t *out_len)
{
    char *padded;
    size_t i, pad;
    uint8_t *dst;
    size_t cap, got;
    int err = 0;

    /* JWA §6.4.1's ALPHABET, refused character by character. `=` is refused with everything else: RFC 7515 §2
       omits all trailing padding, so a `k` that carries any is not base64url. */
    for (i = 0; i < n; i++) {
        char c = k[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_'))
            return -1;
    }
    /* A LENGTH OF 4m+1 ENCODES NOTHING. RFC 4648 §4's groups are 2, 3 or 4 characters once the padding is
       gone; one leftover character is 6 bits, which is fewer than the 8 a byte needs. RFC 7515 Appendix C is
       where the same arithmetic is written for an implementer. */
    if (n % 4 == 1) return -1;
    /* RFC 7515 §2's "with all trailing '=' characters omitted (as permitted by Section 3.2)" READ BACKWARDS —
       the engine's decoder is Infra's forgiving-base64, which requires the padding §3.2 permits, so it is put
       back. The empty string is the empty octet sequence, which that section says in its own words: "Note that
       the base64url encoding of the empty octet sequence is the empty string." */
    pad = (n % 4) ? 4 - (n % 4) : 0;
    padded = malloc(n + pad + 1);
    CHECK(padded != NULL, "§31.6.4 the jwk arm's step 4: OOM translating a jwk `k` into the engine's base64 alphabet");
    for (i = 0; i < n; i++) padded[i] = k[i] == '-' ? '+' : (k[i] == '_' ? '/' : k[i]);
    for (i = 0; i < pad; i++) padded[n + i] = '=';
    padded[n + pad] = '\0';

    cap = JS_Base64DecodedMax(n + pad);
    dst = malloc(cap ? cap : 1);
    CHECK(dst != NULL, "§31.6.4 the jwk arm's step 4: OOM decoding a jwk `k`");
    got = JS_Base64Decode(dst, cap, padded, n + pad, &err);
    free(padded);
    if (err) { free(dst); return -1; }
    *out = dst;
    *out_len = (uint32_t)got;
    return 0;
}

/* §31.6.4 the jwk arm's step 6's per-hash `alg` value — SHA-1/256/384/512 against HS1/HS256/HS384/HS512, which is the
   whole of that sub-step's four clauses. Its fifth clause ("Otherwise, if the name attribute of hash is
   defined in another applicable specification") names no algorithm this engine registers, so a hash that
   reached it would be one §18.4.4 never normalized. */
static const char *hmac_jwk_alg_for(SecureHashAlgorithm hash)
{
    switch (hash) {
    case SECURE_HASH_SHA1:   return "HS1";
    case SECURE_HASH_SHA256: return "HS256";
    case SECURE_HASH_SHA384: return "HS384";
    default:
        DCHECK(hash == SECURE_HASH_SHA512,
               "§31.6.4 the jwk arm's step 6 was asked for the jwk `alg` of a hash §18.4.4 does not normalize — the four "
               "SHA variants are the whole of what a registered HMAC hash can be");
        return "HS512";
    }
}

/* A DICTIONARY MEMBER OFF THE CONVERTED VALUE. §3.2.17 built a plain engine object carrying exactly the
   declared members, so this is an ordinary property get with nothing of the page's on it — no accessor, no
   trap, nothing that could suspend. An ABSENT member is `undefined`, which is what §3.2.17 leaves for a member
   with no declared default, and every "is present" test below is that. */
static JSValue hmac_jwk_get(JSContext *ctx, JSValueConst jwk, const char *name)
{
    return JS_GetPropertyStr(ctx, jwk, name);
}

/* The jwk arm's steps 1-3 and 6-9, plus its step 4's decode — every sub-step of that arm except its step 5,
   which is the same
   "Set hash to equal the hash member of normalizedAlgorithm" the raw arm performs and which this engine has
   already done: `p->hash` IS that member, normalized by §18.4.4 step 10 before this function was called.
   Returns 0 with `*out`/`*out_len` owning the decoded key material, or -1 with an exception pending. */
static int hmac_jwk_import(JSContext *ctx, JSValueConst jwk, const HmacImportParams *p, bool extractable,
                           uint32_t usages, uint8_t **out, uint32_t *out_len)
{
    JSValue v;
    const char *str;
    int bad;

    /* THE JWK ARM'S STEP 1: "If keyData is a JsonWebKey dictionary: Let jwk equal keyData. Otherwise: Throw a DataError."
       §14.3.9 step 4's jwk arm has already refused everything that is not one — with a TypeError, which is
       that step's own error and not this one's — so what reaches here IS the dictionary and the DataError arm
       is unreachable. It is a DCHECK rather than a throw for exactly that reason: a flow standing here holding
       something else is this engine's own logic being wrong, never a page's input being unusual. */
    DCHECK(JS_IsObject(jwk),
           "§31.6.4 the jwk arm's step 1 was handed something that is not the JsonWebKey dictionary §3.2.17 built — "
           "§14.3.9 step 4's jwk arm throws a TypeError for every other value before this algorithm runs");

    /* THE JWK ARM'S STEP 2: "If the kty field of jwk is not \"oct\", then throw a DataError." An ABSENT `kty` is not "oct"
       and takes the same arm, which is why the test is on the STRING and not on presence. */
    v = hmac_jwk_get(ctx, jwk, "kty");
    str = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;
    bad = (str == NULL || strcmp(str, "oct") != 0);
    if (str) JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, v);
    if (bad)
        return JS_ThrowDOMException(ctx, "DataError", "%s",
                                    "an HMAC key can only be imported from a JSON Web Key whose `kty` is "
                                    "\"oct\""),
               -1;

    /* THE JWK ARM'S STEPS 3 AND 4, WHICH ARE ONE READ. "If jwk does not meet the requirements of Section 6.4 of JSON Web
       Algorithms [JWA], then throw a DataError" and "Let data be the byte sequence obtained by decoding the k
       field of jwk". JWA §6.4 is the `kty`-is-"oct" section and §6.4.1 makes `k` that section's one member, so
       an absent `k` fails step 3 and a `k` that is not base64url fails it too — the decode is what establishes
       both, which is why one helper answers them. */
    v = hmac_jwk_get(ctx, jwk, "k");
    str = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;
    bad = (str == NULL || hmac_jwk_k_bytes(ctx, str, strlen(str), out, out_len) < 0);
    if (str) JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, v);
    if (bad)
        return JS_ThrowDOMException(ctx, "DataError", "%s",
                                    "a JSON Web Key for an HMAC import must carry a `k` field holding the "
                                    "base64url encoding of the key value, which JSON Web Algorithms §6.4 "
                                    "requires of every \"oct\" key"),
               -1;

    /* THE JWK ARM'S STEP 6: the four hash clauses, each "If the alg field of jwk is present and is not \"HSn\", then throw
       a DataError". PRESENT is the whole condition — an absent `alg` is permitted, which is JWA §6.4's own
       "An "alg" member SHOULD also be present" read as the SHOULD it is. */
    v = hmac_jwk_get(ctx, jwk, "alg");
    if (!JS_IsUndefined(v)) {
        const char *want = hmac_jwk_alg_for(p->hash);

        str = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;
        bad = (str == NULL || strcmp(str, want) != 0);
        if (str) JS_FreeCString(ctx, str);
        JS_FreeValue(ctx, v);
        if (bad) {
            free(*out); *out = NULL;
            return JS_ThrowDOMException(ctx, "DataError",
                                        "this JSON Web Key names an algorithm other than `%s`, which is the "
                                        "one that matches the requested hash", want),
                   -1;
        }
    } else {
        JS_FreeValue(ctx, v);
    }

    /* THE JWK ARM'S STEP 7: "If usages is non-empty and the use field of jwk is present and is not \"sig\", then throw a
       DataError." Three conjuncts, and the first is about the CALL rather than the key. */
    v = hmac_jwk_get(ctx, jwk, "use");
    if (usages != 0 && !JS_IsUndefined(v)) {
        str = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;
        bad = (str == NULL || strcmp(str, "sig") != 0);
        if (str) JS_FreeCString(ctx, str);
        JS_FreeValue(ctx, v);
        if (bad) {
            free(*out); *out = NULL;
            return JS_ThrowDOMException(ctx, "DataError", "%s",
                                        "this JSON Web Key is not marked for signature use, so it cannot be "
                                        "imported with any key usage"),
                   -1;
        }
    } else {
        JS_FreeValue(ctx, v);
    }

    /* THE JWK ARM'S STEP 8: "If the key_ops field of jwk is present, and is invalid according to the requirements of JSON
       Web Key [JWK] or does not contain all of the specified usages values, then throw a DataError."
       WHAT "INVALID ACCORDING TO JWK" IS, read rather than guessed: RFC 7517 §4.3 "key_ops" (Key Operations)
       Parameter states exactly one MUST — "Duplicate key operation values MUST NOT be present in the array" —
       and explicitly permits unrecognized entries ("Other values MAY be used"), so a name outside §14.1's
       eight is NOT invalid and must not be refused here. The two conjuncts are therefore duplicates, and
       coverage of every requested usage.
       THE NAMES COME FROM crypto_key.h's ONE LIST, which is the same list §14.3.9's `sequence<KeyUsage>`
       position declares to §3.2.18 — RFC 7517 §4.3 says the two vocabularies are deliberately one. */
    v = hmac_jwk_get(ctx, jwk, "key_ops");
    if (!JS_IsUndefined(v)) {
        uint32_t seen = 0, named = 0, extra = 0, i, n = 0;
        JSValue len_v = JS_GetPropertyStr(ctx, v, "length");

        JS_ToUint32(ctx, &n, len_v);
        JS_FreeValue(ctx, len_v);
        bad = 0;
        for (i = 0; i < n && !bad; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, v, i);
            const char *nm = JS_IsString(e) ? JS_ToCString(ctx, e) : NULL;
            int k = -1, j;

            if (nm) {
                for (j = 0; CRYPTO_KEY_USAGE_NAMES[j]; j++)
                    if (strcmp(nm, CRYPTO_KEY_USAGE_NAMES[j]) == 0) { k = j; break; }
            }
            /* RFC 7517 §4.3's duplicate MUST, over BOTH populations: a recognized name is a bit in `seen` and
               an unrecognized one is counted in `extra`, because "Other values MAY be used" makes those legal
               entries whose duplicates are just as forbidden. Two identical unrecognized strings are the one
               case a bitmask alone cannot see, and this is where the count answers it. */
            if (k >= 0) {
                if (seen & (1u << k)) bad = 1;
                seen |= 1u << k;
                named++;
            } else {
                extra++;
            }
            if (nm) JS_FreeCString(ctx, nm);
            JS_FreeValue(ctx, e);
        }
        /* THE UNRECOGNIZED HALF OF THE DUPLICATE TEST, stated as arithmetic rather than as a second walk: the
           array's own length must be the recognized names it holds plus the unrecognized ones, and any
           repetition of either makes the sum disagree. */
        if (!bad && named + extra != n) bad = 1;
        /* "does not contain all of the specified usages values" — every bit the CALL asked for must be one
           this array named. */
        if (!bad && (usages & ~seen) != 0) bad = 1;
        JS_FreeValue(ctx, v);
        if (bad) {
            free(*out); *out = NULL;
            return JS_ThrowDOMException(ctx, "DataError", "%s",
                                        "this JSON Web Key's `key_ops` repeats an operation or does not "
                                        "permit every usage the import asked for"),
                   -1;
        }
    } else {
        JS_FreeValue(ctx, v);
    }

    /* THE JWK ARM'S STEP 9: "If the ext field of jwk is present and has the value false and extractable is true, then
       throw a DataError." PRESENCE is one of the three conjuncts, which is why `ext` is declared
       IDL_BOOLEAN_NO_DEFAULT — with a default this test could not tell an absent `ext` from `ext: false`. */
    v = hmac_jwk_get(ctx, jwk, "ext");
    bad = (JS_IsBool(v) && !JS_ToBool(ctx, v) && extractable);
    JS_FreeValue(ctx, v);
    if (bad) {
        free(*out); *out = NULL;
        return JS_ThrowDOMException(ctx, "DataError", "%s",
                                    "this JSON Web Key is marked non-extractable and the import asked for an "
                                    "extractable key"),
               -1;
    }
    return 0;
}

/* §31.6.4 Import Key — STEPS 1 AND 3, THEN STEP 5's FORMAT DISPATCH, whose two arms produce the `data` the
   tail below consumes. The "raw" arm is two sub-steps — "Let data be keyData" and "Set hash to equal the hash
   member of normalizedAlgorithm" — and both are in hand: `key_data` is step 2's keyData, which §14.3.9 step
   4's Otherwise arm got a copy of the bytes of, and `p->hash` is what §18.4.4 step 10's
   HashAlgorithmIdentifier arm normalized. The "jwk" arm is the nine sub-steps above. Everything else is the
   standard's own "Otherwise: throw a NotSupportedError", which is the honest answer for a DER format an HMAC
   key is never in. */
/* Forward-declared rather than moved, so that §31.6.4's steps stand in the file in the standard's own order:
   the entry below is steps 1, 3 and 5 and the tail beneath it is steps 6-15, which is how a reader checking
   the numbering reads them. */
static JSValue hmac_import_key_data(JSContext *ctx, const uint8_t *bytes, uint32_t byte_len,
                                    const HmacImportParams *p, bool extractable, uint32_t usages);

JSValue hmac_import_key(JSContext *ctx, const char *format, JSValueConst key_data, const HmacImportParams *p,
                        bool extractable, uint32_t usages)
{
    uint32_t byte_len = 0;
    const uint8_t *bytes;

    DCHECK(p != NULL, "§31.6.4 was given no normalizedAlgorithm");
    DCHECK(format != NULL, "§31.6.4 step 5 was given no format — §14.1's KeyFormat is a required argument of "
                           "§14.3.9 and its conversion admits exactly four strings");
    /* STEP 1: "If the length member of normalizedAlgorithm is present and is zero, then throw a DataError." */
    if (p->has_length && p->length == 0)
        return JS_ThrowDOMException(ctx, "DataError", "%s",
                                    "the HMAC import parameters name a key length of zero");
    /* §31.6.4 Import Key STEP 3: "If usages contains an entry which is not \"sign\" or \"verify\", then throw
       a SyntaxError." §9's normalized value is a mask, so the standard's contains-an-entry-which-is-not test
       is a bit outside the pair — that gloss is this file's own and is not in quotation marks, because a
       five-word fragment of the sentence above it would be attributed to whichever citation stands nearest
       and the nearest one here is §9. THE SECTION IS NAMED BECAUSE THIS SENTENCE IS NOT UNIQUE: §31.6.3 Generate Key writes it word
       for word, and so do the Import Key steps of algorithms that are not HMAC at all — so a bare citation
       here resolves against whichever section an auditor reaches first, which is a correct quotation of a
       section that does not govern this function. This one is `hmac_import_key`, and §31.6.4's own step order
       is what makes the numbering in this file check out: step 1 is the zero-length member, step 2 takes the
       key data, step 3 is this — AND IT RUNS BEFORE STEP 5, WHICH IT DID NOT. It stood after the format
       dispatch, so `importKey("pkcs8", buf, HMAC, false, ["encrypt"])` answered §31.6.4's step-5
       NotSupportedError where the standard's own order reaches this SyntaxError first. Two different
       exceptions for one call, and a page tells them apart. */
    if (usages & ~(uint32_t)(CRYPTO_KEY_USAGE_SIGN | CRYPTO_KEY_USAGE_VERIFY))
        return JS_ThrowDOMException(ctx, "SyntaxError", "%s",
                                    "an HMAC key may only be used to sign or to verify");
    /* STEP 5's `jwk` ARM. The decoded key material is this function's for exactly as long as the tail needs
       it, which is the whole reason the tail is a separate function: one owner, one free, and no throw
       between them that could skip it. */
    if (strcmp(format, "jwk") == 0) {
        uint8_t *data = NULL;
        JSValue r;

        if (hmac_jwk_import(ctx, key_data, p, extractable, usages, &data, &byte_len) < 0)
            return JS_EXCEPTION;
        r = hmac_import_key_data(ctx, data, byte_len, p, extractable, usages);
        free(data);
        return r;
    }
    /* STEP 5's `raw` ARM, and then its Otherwise. */
    if (strcmp(format, "raw") != 0)
        return JS_ThrowDOMException(ctx, "NotSupportedError",
                                    "an HMAC key cannot be imported from the '%s' format", format);
    bytes = JS_GetBufferBytes(key_data, &byte_len);
    DCHECK(bytes != NULL || byte_len == 0,
           "§14.3.9 step 4's own copy of the key data is detached — nothing but this algorithm holds it, and "
           "the whole reason that step copies is that the page cannot reach these bytes");
    return hmac_import_key_data(ctx, bytes, byte_len, p, extractable, usages);
}

/* §31.6.4 STEPS 6-15, over the `data` step 5 produced — ONE TAIL FOR BOTH ARMS, which is what step 5 being a
   dispatch means: its "raw" and "jwk" arms differ in how they obtain `data` and in nothing after that. It is a
   function of its own so that the jwk arm's decoded key material has exactly ONE owner and exactly one free,
   rather than a `free` before each of the five throws below. */
static JSValue hmac_import_key_data(JSContext *ctx, const uint8_t *bytes, uint32_t byte_len,
                                    const HmacImportParams *p, bool extractable, uint32_t usages)
{
    uint64_t length;
    JSValue algorithm, handle;

    /* STEP 6: "Let length be the length in bits of data." */
    length = (uint64_t)byte_len * 8u;
    /* STEP 7: "If length is zero then throw a DataError." */
    if (length == 0)
        return JS_ThrowDOMException(ctx, "DataError", "%s", "the key data to import is empty");
    /* §31.4 declares `length` as an `unsigned long`, so a key whose bit length does not fit one cannot be
       described by the KeyAlgorithm this mint has to build. That is a 512MB key, which no ArrayBuffer this
       engine can allocate reaches, so it is asserted rather than given an error the standard does not name. */
    DCHECK(length <= 0xFFFFFFFFu,
           "§31.6.4 step 6's length in bits does not fit §31.4 HmacKeyAlgorithm's `unsigned long length` — the "
           "key data is larger than 512MB, which this engine's heap cannot hold");
    /* STEP 8, whose whole body is under "If the length member of normalizedAlgorithm is present". */
    if (p->has_length) {
        if ((uint64_t)p->length > length)
            return JS_ThrowDOMException(ctx, "DataError", "%s",
                                        "the HMAC import parameters name more key bits than the key data has");
        if ((uint64_t)p->length <= length - 8u)
            return JS_ThrowDOMException(ctx, "DataError", "%s",
                                        "the HMAC import parameters discard a whole byte of the key data");
        length = p->length;
    }
    /* STEP 9: "Let key be a new CryptoKey object representing an HMAC key with the first length bits of data."
       THE FIRST `length` BITS ARE ALL THE BYTES, AND THAT IS ARITHMETIC RATHER THAN A SIMPLIFICATION. Step 6
       set length to 8n for the n bytes of data; step 8 admits only a replacement in (8n-8, 8n], i.e. 8n-7 …
       8n, whose ceiling in bytes is n for every member — so no byte is ever dropped and there is no partial
       byte to represent. What step 8 CAN change is the number §31.4's `length` reports, which is why the two
       are stored separately: [[handle]] is the bytes and [[algorithm]].length is this number. */
    handle = JS_NewArrayBufferCopy(ctx, bytes ? bytes : (const uint8_t *)"", byte_len);
    CHECK(!JS_IsException(handle), "§13.3's [[handle]] for an imported HMAC key could not be allocated");
    /* STEPS 11-14: "Let algorithm be a new HmacKeyAlgorithm" and its three attributes. */
    algorithm = hmac_key_algorithm_new(ctx, p->hash, (uint32_t)length);
    /* STEPS 10 and 15: [[type]] is "secret" and [[algorithm]] is `algorithm`. §14.3.9 steps 11 and 12 set
       [[extractable]] and [[usages]], which is why the mint takes them from there rather than from here. */
    return crypto_key_new(ctx, CRYPTO_KEY_TYPE_SECRET, extractable, algorithm, usages, handle);
}

/* ---- the sentence §31.6.1 and §31.6.2 share ---------------------------------------------------------------- */

SecureHashAlgorithm hmac_key_hash(JSContext *ctx, JSValueConst key)
{
    JSValue algorithm = crypto_key_algorithm(ctx, key), hash, name;
    SecureHashAlgorithm alg = SECURE_HASH_SHA256;
    const char *nm;
    bool known;

    /* "the hash function identified by the hash attribute of the [[algorithm]] internal slot of key" — §31.4's
       `hash` is a KeyAlgorithm, so the function is its `name`. Every read here is of a record THIS ENGINE built
       out of §31.4's three attributes: a null-prototype object carrying own data properties, so none of them
       can reach the page's code and none of them can be intercepted. */
    DCHECK(JS_IsObject(algorithm),
           "an HMAC operation read a key whose [[algorithm]] slot is not a dictionary — §31.6.4 step 15 sets it "
           "to the HmacKeyAlgorithm step 11 built, so a key without one was minted somewhere else");
    hash = JS_GetPropertyStr(ctx, algorithm, "hash");
    JS_FreeValue(ctx, algorithm);
    DCHECK(JS_IsObject(hash),
           "an HMAC key's [[algorithm]] has no `hash` — §31.4 declares the member `required`, and §31.6.4 step "
           "14 is the only writer of it");
    name = JS_GetPropertyStr(ctx, hash, "name");
    JS_FreeValue(ctx, hash);
    nm = JS_ToCString(ctx, name);
    JS_FreeValue(ctx, name);
    CHECK(nm != NULL, "an HMAC key's inner hash name could not be read back as UTF-8 — this engine wrote it "
                      "from §32.2's own list of recognized algorithm names");
    known = secure_hash_by_name(nm, &alg);
    DCHECKF(known, "an HMAC key names '%s' as its inner hash, which §32.2 Registration does not recognize — "
                   "§31.6.4 step 14 writes this name from secure_hash_name, so the two lists have come apart",
            nm);
    JS_FreeCString(ctx, nm);
    return alg;
}
