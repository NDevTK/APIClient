/* The JSON Web Key sub-steps §29.4.4 Import Key and §31.6.4 Import Key share — see core/crypto/jwk.h for the
 * measured diff of the two arms, for why the run STOPS at `alg`, and for why none of these reads can suspend.
 *
 * ONE PROBLEM PER FILE: what a JWK's `kty`, `k`, `alg`, `use`, `key_ops` and `ext` MEAN to an import is here;
 * which algorithm is importing, what its key length may be and what a CryptoKey of it IS belong to the
 * chapter's own component. That is why nothing in this file names an algorithm and why no message here does
 * either — a shared sub-step does not know which chapter called it, and threading a name through so that it
 * could pretend to would be the first step back toward two copies. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/crypto/crypto_key.h"
#include "core/crypto/jwk.h"

/* A DICTIONARY MEMBER OFF THE CONVERTED VALUE. Web IDL §3.2.17 built a plain engine object carrying exactly
   the declared members, so this is an ordinary property get with nothing of the page's on it — no accessor, no
   trap, nothing that could suspend. An ABSENT member is `undefined`, which is what §3.2.17 leaves for a member
   with no declared default, and every "is present" test below is that. */
static JSValue jwk_get(JSContext *ctx, JSValueConst jwk, const char *name)
{
    return JS_GetPropertyStr(ctx, jwk, name);
}

/* "PRESENT AND IS NOT <want>", THE TEST FOUR OF THESE SUB-STEPS ARE BUILT OUT OF, as one reading. Answers 1
   when the member is present and is not the string `want` — which is the condition each of those sub-steps
   throws on — and 0 when it is absent or matches. A member that is present and is not a string cannot survive
   an IDL_DOMSTRING conversion, so that case is folded into "is not `want`" rather than given an arm of its
   own: it is the same DataError either way. */
static int jwk_str_present_and_not(JSContext *ctx, JSValueConst jwk, const char *name, const char *want)
{
    JSValue v = jwk_get(ctx, jwk, name);
    const char *str;
    int bad;

    if (JS_IsUndefined(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    str = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;
    bad = (str == NULL || strcmp(str, want) != 0);
    if (str) JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, v);
    return bad;
}

/* BASE64URL IS DELEGATED TO THE ENGINE'S OWN DECODER AND IS NOT A SECOND CODEC, and the reason is the order the
 * algorithm already puts things in rather than a convenience. The arm's step 3 is "If jwk does not meet the
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
static int jwk_k_bytes(const char *k, size_t n, uint8_t **out, uint32_t *out_len)
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
    CHECK(padded != NULL, "the jwk arm's step 4: OOM translating a jwk `k` into the engine's base64 alphabet");
    for (i = 0; i < n; i++) padded[i] = k[i] == '-' ? '+' : (k[i] == '_' ? '/' : k[i]);
    for (i = 0; i < pad; i++) padded[n + i] = '=';
    padded[n + pad] = '\0';

    cap = JS_Base64DecodedMax(n + pad);
    dst = malloc(cap ? cap : 1);
    CHECK(dst != NULL, "the jwk arm's step 4: OOM decoding a jwk `k`");
    got = JS_Base64Decode(dst, cap, padded, n + pad, &err);
    free(padded);
    if (err) { free(dst); return -1; }
    *out = dst;
    *out_len = (uint32_t)got;
    return 0;
}

int jwk_oct_key_bytes(JSContext *ctx, JSValueConst jwk, uint8_t **out, uint32_t *out_len)
{
    JSValue v;
    const char *str;
    int bad;

    /* THE ARM'S STEP 1: "If keyData is a JsonWebKey dictionary: Let jwk equal keyData. Otherwise: Throw a DataError."
       §14.3.9 step 4's jwk arm has already refused everything that is not one — with a TypeError, which is
       that step's own error and not this one's — so what reaches here IS the dictionary and the DataError arm
       is unreachable. It is a DCHECK rather than a throw for exactly that reason: a flow standing here holding
       something else is this engine's own logic being wrong, never a page's input being unusual. Web IDL
       §3.2.17 admits `undefined` and `null` at its step 4 and defaults every member, so even those arrive as
       the engine-built object this asserts and not as themselves. */
    DCHECK(JS_IsObject(jwk),
           "the jwk arm's step 1 was handed something that is not the JsonWebKey dictionary Web IDL §3.2.17 "
           "built — §14.3.9 step 4's jwk arm throws a TypeError for every other value before an import "
           "algorithm runs");

    /* THE ARM'S STEP 2: "If the kty field of jwk is not \"oct\", then throw a DataError." An ABSENT `kty` is not "oct"
       and takes the same arm, which is why the test is on the STRING and not on presence — and why this one
       sub-step cannot use the present-and-not reading the three below share. */
    v = jwk_get(ctx, jwk, "kty");
    str = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;
    bad = (str == NULL || strcmp(str, "oct") != 0);
    if (str) JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, v);
    if (bad)
        return JS_ThrowDOMException(ctx, "DataError", "%s",
                                    "a JSON Web Key must have a `kty` of \"oct\" to be imported as a "
                                    "symmetric key"),
               -1;

    /* THE ARM'S STEPS 3 AND 4, WHICH ARE ONE READ. "If jwk does not meet the requirements of Section 6.4 of JSON Web
       Algorithms [JWA], then throw a DataError" and "Let data be the byte sequence obtained by decoding the
       k field of jwk". JWA §6.4 is the `kty`-is-"oct" section and §6.4.1 makes `k` that section's one member, so
       an absent `k` fails step 3 and a `k` that is not base64url fails it too — the decode is what establishes
       both, which is why one helper answers them. */
    v = jwk_get(ctx, jwk, "k");
    str = JS_IsString(v) ? JS_ToCString(ctx, v) : NULL;
    bad = (str == NULL || jwk_k_bytes(str, strlen(str), out, out_len) < 0);
    if (str) JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, v);
    if (bad)
        return JS_ThrowDOMException(ctx, "DataError", "%s",
                                    "a JSON Web Key whose `kty` is \"oct\" must carry a `k` field holding the "
                                    "base64url encoding of the key value, which JSON Web Algorithms §6.4 "
                                    "requires of every such key"),
               -1;
    return 0;
}

int jwk_alg_is(JSContext *ctx, JSValueConst jwk, const char *want)
{
    DCHECK(want != NULL, "the jwk arm's `alg` sub-step was asked to compare against no value — each chapter "
                         "selects the one its own clauses name and owns the Otherwise arm for the case where "
                         "no clause matches, so a caller reaching here has one");
    if (!jwk_str_present_and_not(ctx, jwk, "alg", want))
        return 0;
    return JS_ThrowDOMException(ctx, "DataError",
                                "this JSON Web Key's `alg` is not `%s`, which is the value this import "
                                "requires", want),
           -1;
}

int jwk_oct_tail(JSContext *ctx, JSValueConst jwk, const char *use_value, uint32_t usages, bool extractable)
{
    JSValue v;
    int bad;

    DCHECK(use_value != NULL, "the jwk arm's `use` sub-step was asked for no value — it is \"enc\" for "
                              "§29.4.4 and \"sig\" for §31.6.4, and that one word is the whole of what the "
                              "two chapters' copies of this sentence differ by");

    /* THE `use` SUB-STEP — §29.4.4's step 6, §31.6.4's step 7: "If usages is non-empty and the use field of jwk
       is present and is not \"enc\", then throw a DataError", with "sig" in §31.6.4's copy. Three conjuncts,
       and the first is about the CALL rather than the key. */
    if (usages != 0 && jwk_str_present_and_not(ctx, jwk, "use", use_value))
        return JS_ThrowDOMException(ctx, "DataError",
                                    "this JSON Web Key's `use` is not \"%s\", so it cannot be imported with "
                                    "any key usage", use_value),
               -1;

    /* THE `key_ops` SUB-STEP — §29.4.4's step 7, §31.6.4's step 8: "If the key_ops field of jwk is present, and is
       invalid according to the requirements of JSON Web Key [JWK] or does not contain all of the specified
       usages values, then throw a DataError."
       WHAT "INVALID ACCORDING TO JWK" IS, read rather than guessed: RFC 7517 §4.3 "key_ops" (Key Operations)
       Parameter states exactly one MUST — "Duplicate key operation values MUST NOT be present in the array" —
       and explicitly permits unrecognized entries ("Other values MAY be used"), so a name outside §14.1's
       eight is NOT invalid and must not be refused here. The two conjuncts are therefore duplicates, and
       coverage of every requested usage.
       THE NAMES COME FROM crypto_key.h's ONE LIST, which is the same list §14.3.9's `sequence<KeyUsage>`
       position declares to Web IDL §3.2.18 — RFC 7517 §4.3 says the two vocabularies are deliberately one. */
    v = jwk_get(ctx, jwk, "key_ops");
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
        if (bad)
            return JS_ThrowDOMException(ctx, "DataError", "%s",
                                        "this JSON Web Key's `key_ops` repeats an operation or does not "
                                        "permit every usage the import asked for"),
                   -1;
    } else {
        JS_FreeValue(ctx, v);
    }

    /* THE `ext` SUB-STEP — §29.4.4's step 8, §31.6.4's step 9: "If the ext field of jwk is present and has the value
       false and extractable is true, then throw a DataError." PRESENCE is one of the three conjuncts, which is
       why `ext` is declared IDL_BOOLEAN_NO_DEFAULT — with a default this test could not tell an absent `ext`
       from `ext: false`. */
    v = jwk_get(ctx, jwk, "ext");
    bad = (JS_IsBool(v) && !JS_ToBool(ctx, v) && extractable);
    JS_FreeValue(ctx, v);
    if (bad)
        return JS_ThrowDOMException(ctx, "DataError", "%s",
                                    "this JSON Web Key is marked non-extractable and the import asked for an "
                                    "extractable key"),
               -1;
    return 0;
}
