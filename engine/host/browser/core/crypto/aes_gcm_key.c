/* Web Cryptography API §29.4.4 Import Key — see core/crypto/aes_gcm_key.h for why §29's key operations are a
 * component of their own and what §29.2 Registration's `None` decides for the caller.
 *
 * §29.4.4's NINE STEPS, verbatim, because every line below is one of them:
 *   1. If usages contains an entry which is not one of "encrypt", "decrypt", "wrapKey" or "unwrapKey", then
 *      throw a SyntaxError.
 *   2. [the format dispatch — three sibling lists, see below]
 *   3. Let key be a new CryptoKey object representing an AES key with value data.
 *   4. Set the [[type]] internal slot of key to "secret".
 *   5. Let algorithm be a new AesKeyAlgorithm.
 *   6. Set the name attribute of algorithm to "AES-GCM".
 *   7. Set the length attribute of algorithm to the length, in bits, of data.
 *   8. Set the [[algorithm]] internal slot of key to algorithm.
 *   9. Return key.
 *
 * A SUB-NUMBER IS NEVER WRITTEN BARE FOR STEP 2, AND THAT IS THE STANDARD'S SHAPE RATHER THAN A HOUSE STYLE.
 * Step 2 is a DISPATCH holding THREE sibling lists — the `raw` arm's two items, the `jwk` arm's eight, and the
 * `Otherwise` — each restarting at 1, so a bare "step 2.2" names three different steps and a reader cannot tell
 * which. §31.6.4's step 5 has the identical shape and core/crypto/hmac.c states the same rule over it. Every
 * citation here therefore names the arm in the spec's own words.
 *
 * THE `jwk` ARM HAS NO LENGTH SUB-STEP AND THAT IS THE ONE THING A READER OF THIS ALGORITHM MOST EASILY GETS
 * WRONG. The `raw` arm's SECOND ITEM is a sub-step whose whole content is the length test — "If the length in
 * bits of data is not 128, 192 or 256 then throw a DataError" — and the `jwk` arm has nothing of the kind. Its
 * step 5 is a four-clause dispatch ON the length in bits of data, three of whose clauses check `alg` against
 * "A128GCM", "A192GCM" and "A256GCM" and whose fourth reads "Otherwise: throw a DataError". So that Otherwise
 * IS the arm's length validation, and a reading of step 5 as "a three-way `alg` match against the length"
 * — which is how it was described in this file's own retired next-diff clause — names three of its four
 * clauses and drops the only one that refuses anything a page can write. The clause was also wrong about the
 * `use` sub-step, which it called word for word §31.6.4's: this chapter's value is "enc" and §31.6.4's is
 * "sig", so building to the clause would have refused exactly the JSON Web Key a real AES-GCM import carries.
 * Both halves were checkable against the fetched document before anything was built, and neither was checked.
 *
 * STEP 1 RUNS BEFORE STEP 2 AND THE ORDER IS OBSERVABLE. `importKey("raw", <a 17-byte view>, {name:"AES-GCM"},
 * true, ["sign"])` is step 1's SyntaxError and not step 2's `raw` arm's DataError, because "sign" is outside
 * the four usages this algorithm permits and the standard asks that first. Two different exceptions for one
 * call, and a page tells them apart with a `.catch`.
 *
 * AND §14.3.9's OWN STEPS ARE NOT HERE. Step 4's JsonWebKey/BufferSource converse test, step 10's empty-usages
 * SyntaxError and steps 11-12's [[extractable]] and [[usages]] belong to the METHOD and run in
 * core/crypto/subtle_crypto.c, which is what makes them one implementation shared with §31.6.4 rather than a
 * copy per algorithm. The mint takes `extractable` and `usages` as arguments for steps 11 and 12 rather than
 * writing them afterwards, because a CryptoKey whose slots are filled in later is a CryptoKey that briefly
 * exists with the wrong ones.
 *
 * NOTHING HERE TOUCHES THE CIPHER. §29.4.4 step 3's "a new CryptoKey object representing an AES key with value
 * data" is the BYTES and not a key schedule: §13.3's [[handle]] holds "whatever data the underlying
 * cryptographic implementation uses to represent a logical key", and core/crypto/aes.c's `aes_init` expands a
 * schedule from those bytes at the point §29.4.1 Encrypt needs one. Expanding it here would put a POD C
 * structure in a slot that must fork per flow and park to the cold tier, which is the defect
 * core/crypto/crypto_key.h's [[handle]] paragraph is entirely about. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/crypto/aes_gcm_key.h"
#include "core/crypto/crypto.h"
#include "core/crypto/crypto_key.h"
#include "core/crypto/jwk.h"
#include "core/idl_slots.h"

/* §29.4.4 step 1's four usages, as the mask §9 Terminology's normalized value already is. The standard's
   contains-an-entry-which-is-not test is then a bit OUTSIDE this set — that gloss is this file's own and is
   not in quotation marks, because a fragment of it would be attributed to whichever citation stands nearest.
   IT IS §29.4.4's OWN LIST AND NOT AES's IN GENERAL: §30.3.4 AES-KW permits only the two wrapping usages, and
   §27's and §28's Import Key permit only encrypt and decrypt, so a shared constant here would be four
   algorithms' different sentences under one name. */
#define AES_GCM_IMPORT_USAGES                                                                                 \
    ((uint32_t)(CRYPTO_KEY_USAGE_ENCRYPT | CRYPTO_KEY_USAGE_DECRYPT | CRYPTO_KEY_USAGE_WRAP_KEY |             \
                CRYPTO_KEY_USAGE_UNWRAP_KEY))

/* §29.4.3 step 1's four usages. THE SAME FOUR WORDS AS THE CONSTANT ABOVE AND A SECOND CONSTANT ANYWAY, on
   that one's own argument read one chapter further: these are two sentences of the standard that happen to
   agree, and a single name would make a later edition's disagreement invisible — the §31 chapter's import and
   generate rows already differ from each other in exactly this way. The cost of the duplicate is one line; the
   cost of the share is that nothing would report the day they part. */
#define AES_GCM_GENERATE_USAGES                                                                               \
    ((uint32_t)(CRYPTO_KEY_USAGE_ENCRYPT | CRYPTO_KEY_USAGE_DECRYPT | CRYPTO_KEY_USAGE_WRAP_KEY |             \
                CRYPTO_KEY_USAGE_UNWRAP_KEY))

/* §29.4.4 STEPS 5-7: "Let algorithm be a new AesKeyAlgorithm", then its `name` and its `length`.
   §27.4 declares the dictionary in the AES-CTR chapter and every AES algorithm mints one:
   `dictionary AesKeyAlgorithm : KeyAlgorithm { required [EnforceRange] unsigned short length; };`, of whose
   member it says "The length member represents the length, in bits, of the key." The inherited `name` is
   KeyAlgorithm's. A null-prototype record, for core/idl_slots.h's reason: §13.4's `algorithm` hands the page a
   CACHED object built beside this one, and this one is the SLOT — nothing the page writes can reach it, and
   nothing it holds can reach the page's prototype chain. */
static JSValue aes_key_algorithm_new(JSContext *ctx, const char *name, uint32_t length_bits)
{
    JSValue alg = idl_slots_new(ctx);

    CHECK(!JS_IsException(alg), "§27.4's AesKeyAlgorithm could not be allocated");
    /* §27.4's `length` is an `unsigned short`, and step 2 admitted exactly three values before this ran — so a
       length outside them is this engine's own arithmetic having gone wrong rather than anything a page said.
       THIS USED TO READ "step 2's `raw` arm admitted … its second item is the only writer of `data`", AND THE
       `jwk` ARM LANDING IS WHAT MADE THAT FALSE rather than anybody disagreeing with it. It is rewritten and
       not deleted because a reader who re-derives the assertion from the `raw` arm alone will re-add the
       narrower reason and then be surprised by a jwk import. There are now TWO writers of `data` and the three
       admitted lengths are a fact about BOTH: the `raw` arm's second item states them as a sub-step of its own,
       and the `jwk` arm's step 5 states them as the four clauses it dispatches on, whose Otherwise throws. */
    DCHECK(length_bits == 128 || length_bits == 192 || length_bits == 256,
           "§29.4.4 step 7's length in bits is not one of the three step 2 admits — the `raw` arm's second item "
           "and the `jwk` arm's step 5's Otherwise are the two writers of `data`, and both admit exactly those "
           "three, so one of them has come apart from this");
    JS_SetPropertyStr(ctx, alg, "name", JS_NewString(ctx, name));
    JS_SetPropertyStr(ctx, alg, "length", JS_NewUint32(ctx, length_bits));
    return alg;
}

/* §29.4.4 the jwk arm's step 5's per-length `alg` value — 128, 192 and 256 bits against "A128GCM", "A192GCM"
   and "A256GCM", which is the whole of that sub-step's first three clauses.
   ITS FOURTH CLAUSE IS "Otherwise: throw a DataError", AND THAT CLAUSE IS THE ONLY LENGTH TEST THE `jwk` ARM
   HAS. The `raw` arm states its own as a sub-step of its own — "If the length in bits of data is not 128, 192
   or 256 then throw a DataError" — and the `jwk` arm states none, so every byte of length validation a JSON
   Web Key import performs is carried by this dispatch's Otherwise. A reader who takes this sub-step for a
   three-way `alg` match and writes three clauses without a fourth admits a key of ANY length, which reaches
   step 7 and fires aes_key_algorithm_new's assertion on a value a page supplied. NULL IS THAT FOURTH CLAUSE,
   answered by the caller because it is a throw rather than a value.
   THE SELECTOR IS THE BYTE COUNT for the reason the `raw` arm's own comment gives: an ArrayBuffer's length is a
   `uint32_t`, so eight times one overflows above 512MB, and a 536870912-byte `k` would compute a bit length of
   0 and be admitted as 128. Sixteen, twenty-four and thirty-two bytes ARE 128, 192 and 256 bits. */
static const char *aes_gcm_jwk_alg_for(uint32_t byte_len)
{
    switch (byte_len) {
    case 16: return "A128GCM";
    case 24: return "A192GCM";
    case 32: return "A256GCM";
    default: return NULL;
    }
}

/* §29.4.4 STEPS 3-9, OVER THE `data` STEP 2 PRODUCED — ONE TAIL FOR BOTH ARMS, which is what step 2 being a
   dispatch means: its `raw` and `jwk` arms differ in how they obtain `data` and in nothing after that. It is a
   function of its own so that the jwk arm's decoded key material has exactly ONE owner and exactly one free,
   rather than a `free` before each of that arm's throws.
   AND IT IS §29.4.3 STEPS 5-13 AS WELL, WHICH IS WHY IT NO LONGER CARRIES `import` IN ITS NAME. The two
   chapters write the same eight writes in a different order and differ in exactly ONE sentence: §29.4.4 step 7
   is "Set the length attribute of algorithm to the length, in bits, of data" and §29.4.3 step 8 is "Set the
   length attribute of algorithm to equal the length member of normalizedAlgorithm". Those are the same
   number for a generated key and they are the same number BY CONSTRUCTION rather than by resemblance —
   §29.4.3 step 3 generates a key OF that length — so the generate path asserts the equality at its own call
   rather than relying on a reader noticing it. A name saying `import` over a body two chapters reach would be
   this file's AES_GCM_IMPORT_USAGES hazard one level up: one name standing for two sentences. */
static JSValue aes_gcm_key_from_bytes(JSContext *ctx, const uint8_t *bytes, uint32_t byte_len,
                                      bool extractable, uint32_t usages);

JSValue aes_gcm_import_key(JSContext *ctx, const char *format, JSValueConst key_data, bool extractable,
                           uint32_t usages)
{
    uint32_t byte_len = 0;
    const uint8_t *bytes;

    DCHECK(format != NULL, "§29.4.4 step 2 was given no format — §14.1's KeyFormat is a required argument of "
                           "§14.3.9 and its conversion admits exactly four strings");
    DCHECK((usages & ~(uint32_t)CRYPTO_KEY_USAGES_ALL) == 0,
           "§29.4.4 was given a usages mask with a bit outside §13.2's list of recognized key usage values — "
           "§9's normalized value is what this argument is, and this engine is the only thing that builds one");
    /* STEP 1. */
    if ((usages & ~AES_GCM_IMPORT_USAGES) != 0)
        return JS_ThrowDOMException(ctx, "SyntaxError", "%s",
                                    "an AES-GCM key may only be used to encrypt, to decrypt, to wrap a key or "
                                    "to unwrap one");
    /* STEP 2's `jwk` ARM, ALL EIGHT SUB-STEPS, IN THE STANDARD'S OWN ORDER — AN ORDER THE CALL SEQUENCE
       CARRIES RATHER THAN A COMMENT ASKING FOR IT. core/crypto/jwk.c holds the six of them §31.6.4's own `jwk`
       arm writes word for word, measured against the fetched document sentence by sentence rather than assumed
       from the two arms looking alike; that header states the diff, including which sub-steps are NOT shared.
       The decoded key material is this function's for exactly as long as the tail needs it, which is the whole
       reason aes_gcm_key_from_bytes is a separate function: one owner, one free. */
    if (strcmp(format, "jwk") == 0) {
        uint8_t *data = NULL;
        const char *want;
        JSValue r;

        /* THE ARM'S STEPS 1-4 — keyData is the dictionary, `kty` is "oct", JSON Web Algorithms §6.4's
           requirements, and the decode of `k`. */
        if (jwk_oct_key_bytes(ctx, key_data, &data, &byte_len) < 0)
            return JS_EXCEPTION;
        /* THE ARM'S STEP 5. Its Otherwise clause first, because that is the clause that decides whether any of
           the other three can apply — and it is this arm's whole length check, so it is performed here and not
           left to step 7's assertion. */
        want = aes_gcm_jwk_alg_for(byte_len);
        if (want == NULL) {
            free(data);
            return JS_ThrowDOMException(ctx, "DataError", "%s",
                                        "an AES-GCM key must be 128, 192 or 256 bits long");
        }
        /* THE REST OF STEP 5, then THE ARM'S STEPS 6, 7 AND 8. The `use` value "enc" is the one word §31.6.4's
           copy of that sentence writes differently — it is "sig" there, because an HMAC key signs and an
           AES-GCM key encrypts, and a shared constant would be two algorithms' different sentences under one
           name. */
        if (jwk_alg_is(ctx, key_data, want) < 0 ||
            jwk_oct_tail(ctx, key_data, "enc", usages, extractable) < 0) {
            free(data);
            return JS_EXCEPTION;
        }
        r = aes_gcm_key_from_bytes(ctx, data, byte_len, extractable, usages);
        free(data);
        return r;
    }
    /* STEP 2's `Otherwise`: "throw a NotSupportedError". §14.1's KeyFormat has four values and this is the
       standard's own answer for the two DER ones, so it is a refusal rather than a gap — an AES key is never
       in a SubjectPublicKeyInfo or a PrivateKeyInfo. */
    if (strcmp(format, "raw") != 0)
        return JS_ThrowDOMException(ctx, "NotSupportedError",
                                    "an AES-GCM key cannot be imported from the '%s' format", format);
    /* STEP 2's `raw` ARM, both items: "Let data be keyData", then "If the length in bits of data is not 128,
       192 or 256 then throw a DataError." THE SECOND ITEM IS THIS ARM'S OWN, which is why it is stated here
       and not in the tail: the `jwk` arm has no such sub-step and carries the identical three lengths inside
       its step 5's dispatch instead, so a shared test would be one sentence standing for two the standard
       writes separately — and the two are NOT interchangeable, since this one throws on a length the other
       reaches only after `k` has decoded. */
    bytes = JS_GetBufferBytes(key_data, &byte_len);
    DCHECK(bytes != NULL || byte_len == 0,
           "§14.3.9 step 4's own copy of the key data is detached — nothing but this algorithm holds it, and "
           "the whole reason that step copies is that the page cannot reach these bytes");
    /* THE MULTIPLICATION CANNOT WRAP AND THE TEST DOES NOT RELY ON THAT. An ArrayBuffer's length is a
       `uint32_t`, so eight times one overflows 32 bits above 512MB — and the three admitted lengths are 16, 24
       and 32 bytes, so the byte count is compared and the bit count is derived from it rather than the other
       way round. A 536870912-byte key would otherwise compute a bit length of 0 and be admitted as 128. */
    if (byte_len != 16 && byte_len != 24 && byte_len != 32)
        return JS_ThrowDOMException(ctx, "DataError", "%s",
                                    "an AES-GCM key must be 128, 192 or 256 bits long");
    return aes_gcm_key_from_bytes(ctx, bytes, byte_len, extractable, usages);
}

static JSValue aes_gcm_key_from_bytes(JSContext *ctx, const uint8_t *bytes, uint32_t byte_len,
                                      bool extractable, uint32_t usages)
{
    JSValue algorithm, handle;
    uint32_t length = byte_len * 8u;

    /* §29.4.4 STEP 3 / §29.4.3 STEP 5: "Let key be a new CryptoKey object representing an AES key with value
       data", and "Let key be a new CryptoKey object representing the generated AES key". §13.3's [[handle]] is
       those bytes, as an ArrayBuffer — core/crypto/crypto_key.h states why it is a JS value and not a malloc'd
       buffer. */
    handle = JS_NewArrayBufferCopy(ctx, bytes ? bytes : (const uint8_t *)"", byte_len);
    CHECK(!JS_IsException(handle), "§13.3's [[handle]] for an AES-GCM key could not be allocated");
    /* §29.4.4 STEPS 5-7 / §29.4.3 STEPS 6-8. The name is each chapter's own literal rather than the key the
       registry matched: §18.4.4 step 5 sets algName to "the value of the matching key", so a page naming
       "aes-gcm" normalizes to the registry's spelling, and the step then writes the one this chapter states.
       THE `length` IS THE ONE SENTENCE THE TWO CHAPTERS DO NOT SHARE — see the forward declaration — and it is
       derived from `byte_len` here, which is §29.4.4 step 7's own wording; the generate path's call is where
       §29.4.3 step 8's operand is asserted equal to it. */
    algorithm = aes_key_algorithm_new(ctx, "AES-GCM", length);
    /* §29.4.4 STEPS 4, 8 AND 9 / §29.4.3 STEPS 9-13, plus §14.3.9's steps 11 and 12 and §14.3.6's `extractable`
       and `usages` — see the file comment for why those are arguments of the mint rather than writes after it. */
    return crypto_key_new(ctx, CRYPTO_KEY_TYPE_SECRET, extractable, algorithm, usages, handle);
}


/* §29.4.3 "Generate Key", WHOLE — the operation §14.3.6 step 8 performs when normalizedAlgorithm names
   AES-GCM: "Let result be the result of performing the generate key operation specified by normalizedAlgorithm
   using algorithm, extractable and usages."
   `length_bits` is §27.5 AesKeyGenParams' `length` member, already through Web IDL §3.2.4.9's ConvertToInt
   under §3.3.6 [EnforceRange] at the normalization — so it is a number this engine computed and the range test
   below is the STANDARD's step 2 rather than a conversion this file repeats. */
JSValue aes_gcm_generate_key(JSContext *ctx, uint32_t length_bits, bool extractable, uint32_t usages)
{
    uint8_t material[32];

    DCHECK((usages & ~(uint32_t)CRYPTO_KEY_USAGES_ALL) == 0,
           "§29.4.3 was given a usages mask with a bit outside §13.2's list of recognized key usage values — "
           "§9's normalized value is what this argument is, and this engine is the only thing that builds one");
    /* STEP 1. §29.4.4's list is the same four words and IS A DIFFERENT SENTENCE, which is why this is a second
       constant and not a reuse of AES_GCM_IMPORT_USAGES — that constant's own comment states the rule, and the
       two happening to agree today is not a reason to make one name answer for both. */
    if ((usages & ~AES_GCM_GENERATE_USAGES) != 0)
        return JS_ThrowDOMException(ctx, "SyntaxError", "%s",
                                    "an AES-GCM key may only be generated to encrypt, to decrypt, to wrap a "
                                    "key or to unwrap one");
    /* STEP 2: "If the length member of normalizedAlgorithm is not equal to one of 128, 192 or 256, then throw
       an OperationError." A PAGE SUPPLIES THAT NUMBER, so it is the standard's refusal and never an assert —
       `generateKey({name:"AES-GCM",length:7}, …)` is a rejected promise in every browser and an abort here
       would hand any page a switch. It is an OperationError and not the `raw` import arm's DataError: the two
       chapters name different exceptions for the same three lengths and a page reads which it got. */
    if (length_bits != 128 && length_bits != 192 && length_bits != 256)
        return JS_ThrowDOMException(ctx, "OperationError", "%s",
                                    "an AES-GCM key must be 128, 192 or 256 bits long");
    /* STEP 3: "Generate an AES key of length equal to the length member of normalizedAlgorithm." The source is
       §10.1's stream, reached through crypto.h's entry so that BOTH members draw from the one position this
       realm's Crypto carries — that header states what a second stream would cost a forked search.
       STEP 4 — "If the key generation step fails, then throw an OperationError" — HAS NO ARM HERE AND THAT IS
       A STATEMENT RATHER THAN AN OMISSION: the draw is a counter mixer over a position that cannot fail, so
       the step's condition is false by construction. A device-backed source would put its failure here. */
    DCHECK(length_bits / 8u <= sizeof material,
           "§29.4.3 step 3 was asked for more key material than step 2 admits — the three lengths step 2 "
           "leaves standing are 128, 192 and 256 bits and this buffer holds the largest of them");
    crypto_random_bytes(ctx, material, length_bits / 8u);
    /* STEPS 5-13. `byte_len * 8` inside that tail IS §29.4.3 step 8's "the length member of
       normalizedAlgorithm", and the equality is asserted rather than left to the reader — the two chapters
       write that one sentence differently and they agree here only because step 3 drew exactly this many
       bytes. */
    DCHECK((length_bits / 8u) * 8u == length_bits,
           "§29.4.3 step 8's `length` member and step 3's generated key have come apart — the tail sets the "
           "AesKeyAlgorithm's `length` from the byte count, which is §29.4.4 step 7's wording, and the two "
           "chapters agree only while the draw is exactly length/8 bytes");
    return aes_gcm_key_from_bytes(ctx, material, length_bits / 8u, extractable, usages);
}

/* §29.4.5 Export Key — THREE TOP-LEVEL STEPS, and the count is the first thing a reader needs because the
 * section LOOKS longer than it is. Step 2 is ONE `<li>` holding a `<dl>` switch whose jwk arm holds seven
 * sub-steps of its own, so a flat `<li>` count reads eleven or twelve and every citation drawn from it names a
 * step this section does not have. Counted with list depth tracked: 1, 2, 3.
 *
 * THE SHAPE IS §31.6.5's MIRRORED, AND THE ONE STRUCTURAL DIFFERENCE IS WHERE THE OCTETS ARE NAMED. §31.6.5
 * HOISTS them above its format dispatch ("Let bits be the raw bits…", "Let data be a byte sequence containing
 * bits") and §29.4.5 names them INSIDE each arm — "a byte sequence containing the raw octets of the key
 * represented by the [[handle]] internal slot of key" in the raw arm, and "the raw octets of the key
 * represented by the [[handle]] internal slot of key" in the jwk arm's `k` sub-step. Both arms read the same
 * [[handle]], which is why one read above the dispatch is faithful and not a hoist this chapter forbids;
 * core/crypto/jwk.h states that diff as the reason the octets are a parameter there rather than a second
 * reader.
 *
 * ITS `alg` SUB-STEP HAS THREE CLAUSES AND NO OTHERWISE, WHICH IS THE WHOLE REASON THE NULL BELOW IS A DCHECK
 * AND NOT A THROW. §29.4.4's same-named sub-step has a FOURTH clause — "Otherwise: throw a DataError" — and
 * that clause is the jwk import arm's entire length validation, so aes_gcm_jwk_alg_for's NULL is a page's
 * input being unusual THERE and is this engine's own logic being wrong HERE. One leaf, two questions, and the
 * caller answers its own chapter's: §29.4.3 step 2 ("If the length member of normalizedAlgorithm is not equal
 * to one of 128, 192 or 256, then throw an OperationError") and §29.4.4 step 2's two arms are the only writers
 * of an AES-GCM key's material, and all of them admit exactly the three lengths these clauses cover. A key
 * outside them is one this codebase minted wrong, which is what a DCHECK asserts.
 *
 * AND THE SELECTOR IS THE HANDLE'S BYTE COUNT WHERE THE STANDARD SAYS "the length attribute of key", WHICH IS
 * ASSERTED RATHER THAN LEFT TO A READER TO NOTICE. The two are the same number by construction — §29.4.4 step
 * 7 sets `length` from the bit length of `data` and §29.4.3 step 8 sets it from the length step 3 generated —
 * but "by construction" is a claim about two OTHER chapters, and this one would answer a wrong `alg` in
 * silence if either came apart. The byte count is what indexes, for aes_gcm_jwk_alg_for's own stated reason
 * (a `uint32_t` bit length overflows above 512MB), and the slot's `length` is read beside it purely so the
 * equality can fire. */
JSValue aes_gcm_export_key(JSContext *ctx, const char *format, JSValueConst key)
{
    JSValue handle = crypto_key_handle(ctx, key);
    uint32_t len = 0;
    const uint8_t *bits;
    JSValue result;

    DCHECK(format != NULL, "§29.4.5 step 2's format dispatch was asked with no format");

    /* STEP 1: "If the underlying cryptographic key material represented by the [[handle]] internal slot of key
       cannot be accessed, then throw an OperationError."
       IT IS A DCHECK AND NOT THAT THROW, for the reason §31.6.5's own step 1 is one in core/crypto/hmac.c:
       §13.3's [[handle]] is an ArrayBuffer THIS ENGINE minted into an own slot that is never handed out, and
       the one operation that reads it back is this one, gated on §14.3.10 step 7. The only route to an
       unreadable handle is a key this codebase built wrong. A page that could detach the material would make
       this a real OperationError, and nothing in this engine can produce one. */
    bits = JS_GetBufferBytes(handle, &len);
    DCHECK(bits != NULL || len == 0,
           "an AES-GCM key's [[handle]] could not be read — §13.3's slot holds an ArrayBuffer this engine "
           "minted and never hands out, so a detached one is a key built somewhere other than crypto_key_new");

    /* STEP 2's DISPATCH. The three arms are the standard's own, in its own order. */
    if (strcmp(format, "raw") == 0) {
        /* THE RAW ARM, BOTH SUB-STEPS: "Let data be a byte sequence containing the raw octets of the key
           represented by the [[handle]] internal slot of key" and "Let result be data". §14.3.10 step 10's
           otherwise arm then creates the ArrayBuffer, which for this engine is the carrier the byte sequence
           already travels in — the same identity core/crypto/hmac.h argues for §31.6.5's raw arm. */
        result = JS_NewArrayBufferCopy(ctx, bits ? bits : (const uint8_t *)"", len);
        CHECK(!JS_IsException(result), "§14.3.10 step 10's ArrayBuffer for an exported AES-GCM key could not "
                                       "be allocated");
    } else if (strcmp(format, "jwk") == 0) {
        /* THE JWK ARM, ALL SEVEN SUB-STEPS. Six of them — the dictionary, `kty`, `k`, `key_ops`, `ext` and
           "Let result be jwk" — are what §31.6.5's arm states word for word, so they are core/crypto/jwk.c's
           one run; jwk.h carries the measured diff of the two chapters. The `alg` sub-step is this chapter's
           alone and is the argument. */
        const char *alg = aes_gcm_jwk_alg_for(len);

        /* THE TWO-SIDED ASSERT THE BANNER ARGUES FOR: §27.4's `length` and the [[handle]]'s octets are two
           independent statements of one fact, written by two different chapters, and this is the one place
           both are in hand. It is not the dispatch's guard — the dispatch reads the byte count either way —
           it is what stops a disagreement between them being spent as a silently wrong `alg`.
           THE READ IS INSIDE `#if APICLIENT_DEV` AND NOT MERELY THE COMPARISON, because a DCHECK is only
           "always safe to ship" while its OPERANDS cost nothing either: `((void)sizeof(cond))` drops the
           comparison in release and would leave a property read, a coercion and two frees running on every
           jwk export to feed an assert that is no longer there. */
#if APICLIENT_DEV
        {
            JSValue algorithm = crypto_key_algorithm(ctx, key);
            JSValue slot_len;
            uint32_t attr_bits = 0;

            DCHECK(JS_IsObject(algorithm),
                   "§29.4.5's jwk arm read a key whose [[algorithm]] slot is not a dictionary — §13.3 declares "
                   "the slot on every key and crypto_key_new is its only writer");
            slot_len = JS_GetPropertyStr(ctx, algorithm, "length");
            JS_FreeValue(ctx, algorithm);
            if (JS_ToUint32(ctx, &attr_bits, slot_len) < 0)
                attr_bits = 0;
            JS_FreeValue(ctx, slot_len);
            DCHECK(attr_bits == len * 8u,
                   "§27.4's `length` and §13.3's [[handle]] disagree about an AES-GCM key's size — §29.4.5's "
                   "`alg` sub-step selects on the length attribute and this engine indexes on the handle's "
                   "octets, and the two agree only while §29.4.3 step 8 and §29.4.4 step 7 write what was "
                   "drawn");
        }
#endif
        DCHECK(alg != NULL,
               "§29.4.5's `alg` sub-step has three clauses and NO Otherwise, and this key's length matched "
               "none of them — §29.4.3 step 2 and §29.4.4 step 2's two arms are the only writers of an "
               "AES-GCM key's material and every one of them admits exactly 128, 192 and 256 bits");
        result = jwk_oct_export(ctx, bits, len, alg, crypto_key_usages(ctx, key),
                                crypto_key_extractable(ctx, key));
    } else {
        /* STEP 2's "Otherwise: throw a NotSupportedError." REACHED BY "spki" AND "pkcs8" AND BY NOTHING ELSE:
           §14.1's KeyFormat has four values and the argument position is declared IDL_ENUM, so Web IDL
           §3.2.18 refused every other string before §14.3.10 step 1 ran. They are a page's own input and this
           is the refusal the standard names for them, never an assert — an AES-GCM key is a "secret" key and
           neither of those two formats carries one. */
        JS_ThrowDOMException(ctx, "NotSupportedError",
                             "an AES-GCM key cannot be exported as \"%s\" — §29.4.5 Export Key defines the "
                             "\"raw\" and \"jwk\" formats and no other", format);
        result = JS_EXCEPTION;
    }
    JS_FreeValue(ctx, handle);
    /* STEP 3: "Return result." */
    return result;
}
