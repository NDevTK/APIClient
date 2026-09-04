/* CSP §2.3.1's source-expression grammar and §6.7.3.2 — see csp_source_list.h for why the two live together
 * and why the inline answer is total.
 *
 * THE GRAMMAR IS TRANSCRIBED, NOT RECONSTRUCTED. §2.3.1:
 *
 *   nonce-source  = "'nonce-" base64-value "'"
 *   hash-source   = "'" hash-algorithm "-" base64-value "'"
 *   hash-algorithm= "sha256" / "sha384" / "sha512"
 *   base64-value  = 1*( ALPHA / DIGIT / "+" / "/" / "-" / "_" ) *2( "=" )
 *
 * Three details of that last line are load-bearing and each of them is a real policy on the web. The value
 * needs AT LEAST ONE character, so `'nonce-'` is not a nonce-source — it is an unrecognised expression, which
 * §6.7.3.2 ignores, so it does NOT override 'unsafe-inline'. The alphabet includes `-` and `_`, because the
 * grammar admits base64url as well as base64 (§2.3.1's own note says so, and §6.7.3.3 normalises the two when
 * it compares a hash). And the padding is at MOST two `=`, at the end and nowhere else.
 *
 * AND THE OTHER THREE PRODUCTIONS, which arrived with the caller that has a URL to match:
 *
 *   scheme-source = scheme-part ":"
 *   host-source   = [ scheme-part "://" ] host-part [ ":" port-part ] [ path-part ]
 *   scheme-part   = scheme                       ; RFC 3986 §3.1
 *   host-part     = "*" / [ "*." ] 1*host-char *( "." 1*host-char ) [ "." ]
 *   host-char     = ALPHA / DIGIT / "-"
 *   port-part     = 1*DIGIT / "*"
 *   path-part     = path-absolute                ; but not ";" or ","
 *
 * THE DECOMPOSITION IS THE MATCHER'S HALF THAT CANNOT BE GUESSED AT. §6.7.2.8 reads an expression as four
 * OPTIONAL parts and asks a different relation of each, so "does this token match the host-source grammar" and
 * "which bytes are its host-part" are one answer, produced once (csp_host_source_parse) and read by every arm.
 * Two details of that parse decide real policies. A `scheme-part` is recognised only when a run of RFC 3986
 * scheme characters is followed by `://` — which is what keeps `example.com:8080/a` a host-source with a PORT
 * rather than a scheme named `example.com:8080/a` — and `host-char` admits no `_` and no non-ASCII, because
 * §2.3.1 says an internationalized domain "MUST be Punycode-encoded" before it is written in a policy.
 *
 * WHY AN IP ADDRESS NEVER MATCHES A host-part, which §2.3.1's own note asserts and this tree makes structural:
 * §6.7.2.10 step 1 is "if host is not a domain, return Does Not Match", and a host that is not a domain is
 * exactly `url->host.kind != URL_HOST_DOMAIN` — the URL parser resolved `127.0.0.1` and `0x7f.1` alike to an
 * IPv4 NUMBER, so neither is a domain and neither can be spelled as a host-part. The note's "only 127.0.0.1
 * will actually match" is true through `'self'` (§7.1.1's host equality is over parsed hosts, and two IPv4
 * numbers compare equal) and through nothing else. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/crypto/secure_hash.h"
#include "core/fetch/subresource_integrity.h"
#include "core/frame/csp_source_list.h"

static bool csp_base64_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '+' || c == '/' || c == '-' || c == '_';
}

/* base64-value = 1*( ALPHA / DIGIT / "+" / "/" / "-" / "_" ) *2( "=" ) */
static bool csp_base64_value(const char *p, size_t n)
{
    size_t i = 0, pad = 0;

    while (i < n && csp_base64_char(p[i])) i++;
    if (i == 0) return false;                 /* 1*, not *: the value cannot be empty */
    while (i < n && p[i] == '=') { i++; pad++; }
    return i == n && pad <= 2;
}

/* An ASCII case-insensitive prefix test, for the quoted ABNF literals that open a nonce- or hash-source. */
static bool csp_prefix_ci(CspToken t, const char *ascii_lowercase_prefix)
{
    size_t k = strlen(ascii_lowercase_prefix), i;

    if (t.n < k) return false;
    for (i = 0; i < k; i++) {
        char a = t.p[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != ascii_lowercase_prefix[i]) return false;
    }
    return true;
}

/* The body between an opening literal of `k` bytes and the closing U+0027, checked as a base64-value. Shared
   by the two productions because they differ only in that literal. */
static bool csp_quoted_base64_tail(CspToken t, size_t k)
{
    /* The opening literal, at least one base64 character, and the closing quote. */
    if (t.n < k + 2) return false;
    if (t.p[t.n - 1] != '\'') return false;
    return csp_base64_value(t.p + k, t.n - k - 1);
}

/* THE WHOLE DECOMPOSITION OF A nonce-source, produced once — does the token match the grammar, and where its
   base64-value part is. Two algorithms need that span and each used to compute it from the literal 7 and 8 at
   its own site: §6.7.3.3 step 2's arm, which compares it against an ELEMENT's `nonce` attribute, and §6.7.2.3,
   which compares it against a REQUEST's cryptographic nonce metadata. Two readings of one offset is the
   second-reading defect in miniature, and the day `'nonce-` changed length only one of them would have moved. */
static bool csp_nonce_source_parse(CspToken expression, const char **pvalue, size_t *pvalue_len)
{
    static const char PREFIX[] = "'nonce-";
    const size_t k = sizeof PREFIX - 1;

    if (!csp_prefix_ci(expression, PREFIX)) return false;
    if (!csp_quoted_base64_tail(expression, k)) return false;
    /* The base64-value part: everything between the opening literal and the closing U+0027. */
    if (pvalue) *pvalue = expression.p + k;
    if (pvalue_len) *pvalue_len = expression.n - k - 1;
    return true;
}

bool csp_source_is_nonce(CspToken expression)
{
    return csp_nonce_source_parse(expression, NULL, NULL);
}

/* §2.3.1's hash-algorithm, and §6.7.3.3 step 5.2.2's mapping from it to a digest, in ONE table. The production
   admits exactly these three and nothing else — an expression naming `'sha1-…'` is not a hash-source at all
   and must not override 'unsafe-inline' — and step 5.2.2's three "is an ASCII case-insensitive match for
   'sha256' / 'sha384' / 'sha512'" clauses are the same three read from the other end. Two tables would be two
   statements of one grammar, and the day they disagreed the recogniser would accept an expression the matcher
   could not evaluate, which is the state this file was in when it had only the first half. */
static const struct { const char *opener; SecureHashAlgorithm alg; } CSP_HASH_ALGORITHMS[] = {
    { "'sha256-", SECURE_HASH_SHA256 },
    { "'sha384-", SECURE_HASH_SHA384 },
    { "'sha512-", SECURE_HASH_SHA512 },
};
#define CSP_HASH_ALGORITHMS_N ((int)(sizeof CSP_HASH_ALGORITHMS / sizeof CSP_HASH_ALGORITHMS[0]))

/* THE WHOLE DECOMPOSITION OF A hash-source, produced once: does the token match the grammar, which digest its
   hash-algorithm part names, and where its base64-value part is. §6.7.3.3 step 5.2.2 needs all three and
   §6.7.3.2 needs only the first, which is what csp_source_is_hash asks for. */
/* THE hash-algorithm PART IS A SPAN AND NOT ONLY AN ENUM, and the reason is a comparison across two standards.
   §6.7.3.3 step 5.2.2 asks which DIGEST an expression names, which is what SecureHashAlgorithm answers.
   §6.7.2.4 step 6.1 asks something else — whether the expression's hash-algorithm "is an ASCII
   case-insensitive match for source's hash-algorithm", where `source` came out of SRI §3.3.2 "Parse metadata"
   and is a token of ANOTHER document's set. Answering that by mapping SRI's token to this enum and comparing
   enums would make each standard's answer depend on the other's edition; comparing the two STRINGS is what the
   step says and is what stays right the day one document names an algorithm the other does not. */
static bool csp_hash_source_parse(CspToken expression, SecureHashAlgorithm *palg,
                                  const char **pname, size_t *pname_len,
                                  const char **pvalue, size_t *pvalue_len)
{
    int i;

    for (i = 0; i < CSP_HASH_ALGORITHMS_N; i++) {
        size_t k = strlen(CSP_HASH_ALGORITHMS[i].opener);

        if (!csp_prefix_ci(expression, CSP_HASH_ALGORITHMS[i].opener)) continue;
        if (!csp_quoted_base64_tail(expression, k)) return false;
        if (palg) *palg = CSP_HASH_ALGORITHMS[i].alg;
        /* The hash-algorithm part: the opener without its leading U+0027 and its trailing U+002D. It is taken
           from the TOKEN and not from the table, so it carries the author's own case for step 6.1 to fold. */
        if (pname) *pname = expression.p + 1;
        if (pname_len) *pname_len = k - 2;
        /* The base64-value part: everything between the opening literal and the closing U+0027. */
        if (pvalue) *pvalue = expression.p + k;
        if (pvalue_len) *pvalue_len = expression.n - k - 1;
        return true;
    }
    return false;
}

bool csp_source_is_hash(CspToken expression)
{
    return csp_hash_source_parse(expression, NULL, NULL, NULL, NULL, NULL);
}

/* §6.7.3.3 step 5.2.2's `expected`: "expression's base64-value part, with all '-' characters replaced with
   '+', and all '_' characters replaced with '/'", compared against `actual` for identity. The replacement is
   applied AS THE COMPARISON rather than into a buffer, because that is all it is for — the standard's own note
   says it "normalizes hashes expressed in base64url encoding into base64 encoding for matching".
   THE PADDING IS NOT NORMALISED, and that is the standard and not an omission: `actual` is RFC 4648 §4's
   encoding, which always pads, and step 5.2.2 asks whether the two are IDENTICAL. A policy that wrote its
   hash without the trailing '=' does not match, in this engine and in the standard. */
static bool csp_hash_value_equal(const char *actual, size_t actual_len, const char *expected, size_t expected_len)
{
    size_t i;

    if (actual_len != expected_len) return false;
    for (i = 0; i < actual_len; i++) {
        char e = expected[i];

        if (e == '-') e = '+';
        else if (e == '_') e = '/';
        if (actual[i] != e) return false;
    }
    return true;
}

bool csp_source_list_contains(const CspDirective *directive, const char *keyword)
{
    size_t i;

    DCHECK(directive != NULL, "a source list was searched for a keyword through a directive that does not "
                              "exist — a policy carrying no governing directive says NOTHING about the check, "
                              "which the caller decides before it gets here");
    for (i = 0; i < directive->n_value; i++)
        if (csp_token_is(directive->value[i], keyword)) return true;
    return false;
}

/* §6.7.2.3 "Does nonce match source list?", WHOLE.
 *
 *   1. Assert: source list is not null.
 *   2. If nonce is the empty string, return "Does Not Match".
 *   3. For each expression of source list:
 *      3.1. If expression matches the nonce-source grammar, and nonce is identical to expression's
 *           base64-value part, return "Matches".
 *   4. Return "Does Not Match".
 *
 * STEP 2 IS WHAT MAKES AN ABSENT NONCE AN ANSWER RATHER THAN A HOLE. Fetch §2.2.5 makes the empty string the
 * initial value of every request's cryptographic nonce metadata, so the overwhelming majority of requests
 * arrive here with one, and the standard's own first act is to refuse them. A caller therefore never has to
 * decide whether its request "has" a nonce — it states the field, and the field's emptiness is the decision.
 *
 * STEP 3.1's COMPARISON IS `identical`, WHICH IS BYTES AND NOT A FOLD. §2.3.1's own note says why: "Nonces,
 * however, are strict string matches: we use the base64-value grammar to limit the characters available, and
 * reduce the complexity for the server-side operator (encodings, etc), but the user agent doesn't actually
 * care about any underlying value, nor does it do any decoding of the nonce-source value." The GRAMMAR's
 * literal is matched case-insensitively (RFC 5234 §2.3 makes ABNF string literals so, which is what admits
 * `'NONCE-abc'`) and the VALUE is not — the two are different questions about one token, and
 * csp_nonce_source_parse answers both in one place. */
CspMatch csp_nonce_match_source_list(const CspDirective *directive, const char *nonce, size_t nonce_len)
{
    size_t i;

    /* STEP 1. */
    DCHECK(directive != NULL, "§6.7.2.3 step 1 asserts its source list is not null, and it was asked about a "
                              "directive that does not exist — a policy carrying no governing directive says "
                              "NOTHING about the check, which §6.8.4 decides at the CALLER and which is not "
                              "the same answer as a list that matches nothing");
    DCHECK(nonce != NULL || nonce_len == 0,
           "§6.7.2.3 was given a length with no nonce bytes — the field it reads is Fetch §2.2.5's "
           "cryptographic nonce metadata of some request, and a caller that has none states a zero length");
    /* STEP 2 — and this is where the overwhelming majority of requests leave, because §2.2.5 makes the empty
       string the field's initial value and most requests never have one set. */
    if (nonce_len == 0)
        return CSP_DOES_NOT_MATCH;
    /* STEP 3. */
    for (i = 0; i < directive->n_value; i++) {
        const char *b64 = NULL;
        size_t b64_len = 0;

        if (!csp_nonce_source_parse(directive->value[i], &b64, &b64_len)) continue;
        if (b64_len == nonce_len && memcmp(b64, nonce, nonce_len) == 0)
            return CSP_MATCHES;
    }
    /* STEP 4. */
    return CSP_DOES_NOT_MATCH;
}

/* §6.7.2.4 steps 2-3's `integrity expressions`, reduced to the question step 3 asks of it: "If integrity
   expressions is empty, return Does Not Match". It is a fact about the LIST alone, which is why it can be
   answered before the request's field is parsed at all. */
static bool csp_list_has_hash_source(const CspDirective *directive)
{
    size_t i;

    for (i = 0; i < directive->n_value; i++)
        if (csp_source_is_hash(directive->value[i])) return true;
    return false;
}

/* §6.7.2.4 step 6.1, over ONE parsed integrity source: does this list hold a hash-source "whose hash-algorithm
   is an ASCII case-insensitive match for source's hash-algorithm, and whose base64-value is identical to
   source's base64-value"?
   THE VALUE COMPARISON IS `identical` AND IS NOT THE ONE §6.7.3.3 MAKES. That step normalises base64url into
   base64 before comparing, in its own words, and this one does not — the difference is the standard's, and
   csp_hash_value_equal above is deliberately not reached from here. Reusing it would make an `integrity` value
   written in base64url match a policy written in base64, which §6.7.2.4 does not say. */
static bool csp_list_holds_integrity_source(const CspDirective *directive, const SriHashExpression *src)
{
    size_t i, k;

    for (i = 0; i < directive->n_value; i++) {
        const char *name = NULL, *b64 = NULL;
        size_t name_len = 0, b64_len = 0;

        if (!csp_hash_source_parse(directive->value[i], NULL, &name, &name_len, &b64, &b64_len)) continue;
        if (name_len != src->alg_len || b64_len != src->val_len) continue;
        for (k = 0; k < name_len; k++) {
            char a = name[k], b = src->alg[k];

            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (k != name_len) continue;
        /* The lengths are equal by the guard above, so a zero-length compare is the two empty values — which
           a hash-source cannot have, since §2.3.1's base64-value needs at least one character. A bare
           `sha256` in an `integrity` attribute therefore matches nothing, which is §6.7.2.4 step 6.1 refusing
           it. */
        if (memcmp(b64, src->val, b64_len) == 0) return true;
    }
    return false;
}

/* §6.7.2.4 "Does integrity metadata match source list?", WHOLE.
 *
 *   1. Assert: source list is not null.
 *   2. Let integrity expressions be the set of source expressions in source list that match the hash-source
 *      grammar.
 *   3. If integrity expressions is empty, return "Does Not Match".
 *   4. Let integrity sources be the result of parsing metadata given integrity metadata. [SRI]
 *   5. If integrity sources is "no metadata" or an empty set, return "Does Not Match".
 *   6. For each source of integrity sources:
 *      6.1. If integrity expressions does not contain a source expression whose hash-algorithm is an ASCII
 *           case-insensitive match for source's hash-algorithm, and whose base64-value is identical to
 *           source's base64-value, return "Does Not Match".
 *   7. Return "Matches".
 *
 * THE QUANTIFIER IS UNIVERSAL AND IT IS THE ONE THING HERE THAT IS EASY TO READ BACKWARDS. Step 6.1 refuses
 * the moment ONE of the request's integrity sources is unlisted, so `integrity="sha256-A sha384-B"` under a
 * policy naming only `'sha256-A'` is Does Not Match. The standard's own note says what that buys: "Here, we
 * verify only whether the integrity metadata is a non-empty subset of the hash-source sources in source list."
 *
 * STEP 5's `no metadata` ARM CANNOT BE REACHED AGAINST THE EDITION OF SRI THIS ENGINE READS, and that is a
 * fact about the two documents rather than a narrowing here: SRI §3.3.2 "Parse metadata" returns a SET on
 * every input — its step 1 is "Let result be the empty set" and its step 3 returns it — so the only half of
 * step 5 that can fire is the empty one, which is also what an empty integrity metadata yields. Both halves
 * are the same answer, so nothing is lost by the arm being unreachable; it is written down because a reader
 * who greps for it would otherwise find one condition where the standard states two. */
CspMatch csp_integrity_match_source_list(const CspDirective *directive, const char *integrity,
                                         size_t integrity_len)
{
    SriMetadataParse parse;
    SriHashExpression src;

    /* STEP 1. */
    DCHECK(directive != NULL, "§6.7.2.4 step 1 asserts its source list is not null, and it was asked about a "
                              "directive that does not exist — a policy carrying no governing directive says "
                              "NOTHING about the check, which §6.8.4 decides at the CALLER");
    DCHECK(integrity != NULL || integrity_len == 0,
           "§6.7.2.4 was given a length with no integrity bytes — the field it reads is Fetch §2.2.5's "
           "integrity metadata of some request, and a caller that has none states a zero length");
    /* STEPS 2-3. */
    if (!csp_list_has_hash_source(directive))
        return CSP_DOES_NOT_MATCH;
    /* STEP 4. */
    sri_parse_metadata(&parse, integrity, integrity_len);
    /* STEP 5 — the empty set, asked by advancing the parse once. */
    if (!sri_parse_metadata_next(&parse, &src))
        return CSP_DOES_NOT_MATCH;
    /* STEP 6. */
    do {
        if (!csp_list_holds_integrity_source(directive, &src))
            return CSP_DOES_NOT_MATCH;
    } while (sri_parse_metadata_next(&parse, &src));
    /* STEP 7. */
    return CSP_MATCHES;
}

bool csp_source_list_allows_all_inline(const CspDirective *directive, CspInlineType type)
{
    bool allow_all_inline = false;
    size_t i;

    DCHECK(directive != NULL, "§6.7.3.2 was asked about a source list that does not exist — a policy with no "
                              "governing directive for this check permits it, and that is the CALLER's answer "
                              "to give, not a list-allows-nothing answer for this one");
    /* §6.7.3.2, in its order. The two early returns are returns and not flags: a nonce or hash ANYWHERE in the
       list overrides an 'unsafe-inline' that appeared before it, which is the rule that makes adding a nonce
       to a legacy policy tighten it rather than widen it. */
    for (i = 0; i < directive->n_value; i++) {
        CspToken e = directive->value[i];

        if (csp_source_is_nonce(e) || csp_source_is_hash(e))
            return false;
        /* 'strict-dynamic' overrides 'unsafe-inline' for the SCRIPT types only — the standard's own note says
           it does not apply to other resource types. "navigation" is one of the three, so a javascript: URL is
           killed by it exactly as an inline handler is. */
        if ((type == CSP_INLINE_SCRIPT || type == CSP_INLINE_SCRIPT_ATTRIBUTE ||
             type == CSP_INLINE_NAVIGATION) &&
            csp_token_is(e, "'strict-dynamic'"))
            return false;
        if (csp_token_is(e, "'unsafe-inline'"))
            allow_all_inline = true;
    }
    return allow_all_inline;
}

/* ---- §6.7.3's ELEMENT MATCHING -------------------------------------------------------------------------- */

/* "contains an ASCII case-insensitive match for" over raw bytes — §6.7.3.1's own phrasing, and the only string
   relation it needs. `needle` must be ASCII lowercase, asserted for the reason csp_token_is asserts it. */
static bool csp_bytes_contain_ci(const lxb_char_t *hay, size_t n, const char *needle)
{
    size_t k = strlen(needle), i, j;

    DCHECK(csp_is_ascii_lowercase(needle),
           "§6.7.3.1's substring test was given a literal that is not ASCII lowercase — the fold is applied to "
           "the HAYSTACK only, so a capital here matches nothing while reading as if it must");
    if (n < k) return false;
    for (i = 0; i + k <= n; i++) {
        for (j = 0; j < k; j++) {
            char c = (char)hay[i + j];

            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != needle[j]) break;
        }
        if (j == k) return true;
    }
    return false;
}

/* "element is a script element" — HTML's `script`, which SVG's same-named element is not (that is
   SVGScriptElement, a different interface with its own standard), so the namespace is part of the question. */
static bool csp_is_script_element(const lxb_dom_element_t *element)
{
    const lxb_dom_node_t *n = lxb_dom_interface_node((lxb_dom_element_t *)element);
    size_t len = 0;
    const lxb_char_t *name;

    if (n->type != LXB_DOM_NODE_TYPE_ELEMENT || n->ns != LXB_NS_HTML) return false;
    name = lxb_dom_element_local_name((lxb_dom_element_t *)element, &len);
    return name && len == 6 && memcmp(name, "script", 6) == 0;
}

bool csp_element_is_nonceable(const lxb_dom_element_t *element)
{
    lxb_dom_element_t *el = (lxb_dom_element_t *)element;
    lxb_dom_attr_t *a;

    /* STEP 1 — "if element does not have an attribute named nonce, return Not Nonceable", which is also the
       whole of the answer for the two callers that have no element at all (§4.2.4's javascript: navigation
       runs the inline check "upon null", and an injected breakout has not been inserted anywhere). PRESENCE,
       not a non-empty value: `<style nonce="">` has the attribute and is Nonceable, and it then matches no
       nonce-source because §2.3.1's base64-value needs at least one character. */
    if (!el) return false;
    if (!lxb_dom_element_has_attribute(el, (const lxb_char_t *)"nonce", 5)) return false;

    /* STEP 2 — the mitigation §7.2.1 dangling markup attacks names, and it is script-only in the standard: an injected
       `<img src=x onerror=…  <script` swallows the following markup into an ATTRIBUTE, which is how an
       attacker steals a legitimate element's nonce without ever reading it. Both the attribute's NAME and its
       VALUE are searched, because the swallowed bytes can land in either. */
    if (csp_is_script_element(el)) {
        for (a = lxb_dom_element_first_attribute(el); a; a = lxb_dom_element_next_attribute(a)) {
            size_t len = 0;
            const lxb_char_t *s = lxb_dom_attr_qualified_name(a, &len);

            if (s && (csp_bytes_contain_ci(s, len, "<script") || csp_bytes_contain_ci(s, len, "<style")))
                return false;
            len = 0;
            s = lxb_dom_attr_value(a, &len);
            if (s && (csp_bytes_contain_ci(s, len, "<script") || csp_bytes_contain_ci(s, len, "<style")))
                return false;
        }
    }
    /* STEP 3 is the duplicate-attribute parse error, which is not implemented and is not implementable from a
       parsed tree — see csp_source_list.h. STEP 4: */
    return true;
}

CspMatch csp_element_match_source_list(const CspDirective *directive, const lxb_dom_element_t *element,
                                       CspInlineType type, const char *source, size_t source_len)
{
    bool unsafe_hashes = false;
    size_t i;

    DCHECK(directive != NULL, "§6.7.3.3 was asked about a source list that does not exist — a policy with no "
                              "governing directive says NOTHING about this check, which is the CALLER's answer "
                              "to give (§6.8.4 decides it) and is not a list that matches nothing");
    DCHECK(source != NULL || source_len == 0,
           "§6.7.3.3 was given a length with no source bytes — its step 5 hashes exactly these bytes, so a "
           "caller that has no source has an empty one and states that with a zero length");

    /* STEP 1 — §6.7.3.2. */
    if (csp_source_list_allows_all_inline(directive, type))
        return CSP_MATCHES;

    /* STEP 2 — THE NONCE ARM, and the standard's own note says exactly how far it reaches: "Nonces only apply
       to inline script and inline style, not to attributes of either element or to javascript: navigations."
       That is why the two attribute types and "navigation" are excluded here and not by the caller. */
    if ((type == CSP_INLINE_SCRIPT || type == CSP_INLINE_STYLE) && csp_element_is_nonceable(element)) {
        size_t nlen = 0;
        const lxb_char_t *nonce = lxb_dom_element_get_attribute((lxb_dom_element_t *)element,
                                                                (const lxb_char_t *)"nonce", 5, &nlen);

        for (i = 0; i < directive->n_value; i++) {
            const char *b64 = NULL;
            size_t b64_len = 0;

            if (!csp_nonce_source_parse(directive->value[i], &b64, &b64_len)) continue;
            /* "element has a nonce attribute whose value is expression's base64-value part". The comparison is
               CASE-SENSITIVE and the value is never decoded — a base64 value is data, not a keyword, and
               folding it would let `'nonce-ABC'` admit an element carrying `abc`. */
            if (nonce && b64_len == nlen && memcmp(b64, nonce, nlen) == 0)
                return CSP_MATCHES;
        }
    }

    /* STEPS 3-4 — the 'unsafe-hashes' flag, which is what extends the hash arm below to event handlers, style
       attributes and javascript: URLs. */
    for (i = 0; i < directive->n_value; i++) {
        if (csp_token_is(directive->value[i], "'unsafe-hashes'")) {
            unsafe_hashes = true;
            break;
        }
    }

    /* STEP 5 — the hash arm and 'strict-dynamic'. Step 5.1's "UTF-8 encode the result of JavaScript string
       converting on source" is a no-op on the bytes this engine holds: Lexbor's text content, its attribute
       values and a serialized URL are all already UTF-8. */
    if (type == CSP_INLINE_SCRIPT || type == CSP_INLINE_STYLE || unsafe_hashes) {
        for (i = 0; i < directive->n_value; i++) {
            CspToken e = directive->value[i];

            /* STEP 5.2.1 — 'strict-dynamic' matches for "script" alone, and only for an element that is NOT
               parser-inserted: "If type is \"script\", and element is not parser-inserted, return
               \"Matches\"". Every other type falls through, which is the standard's answer and not a gap —
               'strict-dynamic' does not apply to style.
               THE ELEMENT'S `parser document` EXISTS NOW AND THIS ALGORITHM STILL CANNOT ASK FOR IT, which is
               a different absence from the one that used to stand here and is why the crash is kept rather
               than deleted. core/html/html_script.h records the flag for every element §4.12.1.1 prepared and
               core/frame/policy_container.h reads it into a REQUEST's Fetch §2.2.5 parser metadata for
               §6.7.1.1 step 1.3 — but §6.7.3.3 is asked about an ELEMENT through policy_allows_inline, whose
               three callers are a `<style>`, an event-handler attribute and an @S breakout with no element,
               so none of them is this arm's subject and none of them can state its answer. Answering "Does
               Not Match" instead would report a script real Chrome runs as blocked, in the one direction
               'strict-dynamic' exists to invert. */
            if (csp_token_is(e, "'strict-dynamic'")) {
                DCHECK(!(type == CSP_INLINE_SCRIPT && element != NULL),
                       "§6.7.3.3 step 5.2.1 reached a real script element under 'strict-dynamic' and this "
                       "algorithm was given no way to say whether that element is PARSER-INSERTED. The flag "
                       "itself is recorded (core/html/html_script.h's html_script_parser_metadata); what is "
                       "missing is the caller — this engine runs CSP §4.2.3 over a `<style>` element and over "
                       "an event-handler attribute and over NO inline `<script>` element at all, so the "
                       "§4.12.1.1 caller that would reach this arm does not exist yet. Build that caller and "
                       "let it carry the element's answer down, then return Matches for a null parser "
                       "document");
                continue;
            }
            /* STEP 5.2.2 — the DIGEST:
                 "If expression matches the hash-source grammar:
                    Let algorithm be null.
                    If expression's hash-algorithm part is an ASCII case-insensitive match for "sha256", set
                    algorithm to SHA-256.  [and likewise sha384 / sha512]
                    If algorithm is not null:
                      Let actual be the result of base64 encoding the result of applying algorithm to source.
                      Let expected be expression's base64-value part, with all '-' characters replaced with
                      '+', and all '_' characters replaced with '/'.
                      If actual is identical to expected, return "Matches"."
               `algorithm is not null` is not a branch here: §2.3.1's hash-algorithm production admits exactly
               the three spellings the table above lists, so a token that matched the grammar named one of
               them — which is what makes the parse return the digest rather than a bool the caller re-derives.
               THE BASE64 IS THE ENGINE'S OWN, not a fourth one written here. "base64 encoding" links to
               RFC 4648 §4, which is what JS_Base64Encode implements for `btoa`; a codec re-implemented beside
               the one that already runs is the second reading this file exists to avoid. */
            {
                SecureHashAlgorithm alg;
                const char *b64 = NULL;
                size_t b64_len = 0;

                if (csp_hash_source_parse(e, &alg, NULL, NULL, &b64, &b64_len)) {
                    uint8_t digest[SECURE_HASH_MAX_DIGEST];
                    /* RFC 4648 §4 turns the largest digest FIPS 180-4 Figure 1 lists (64 bytes) into 88
                       characters; the assert below is what keeps that arithmetic and this array one fact. */
                    char actual[96];
                    SecureHash h;
                    size_t n;

                    secure_hash_init(&h, alg);
                    secure_hash_update(&h, (const uint8_t *)source, source_len);
                    secure_hash_finish(&h, digest, sizeof digest);
                    n = JS_Base64Encode(actual, sizeof actual, digest, secure_hash_digest_size(alg));
                    CHECK(n > 0, "§6.7.3.3 step 5.2.2's base64 encoding did not fit the buffer this file sizes "
                                 "for it — the size is RFC 4648 §4's over FIPS 180-4 Figure 1's largest digest, "
                                 "so a refusal here means one of those two changed and the other did not");
                    if (csp_hash_value_equal(actual, n, b64, b64_len))
                        return CSP_MATCHES;
                }
            }
        }
    }
    return CSP_DOES_NOT_MATCH;
}

/* ---- §2.3.1's scheme-source / host-source, DECOMPOSED --------------------------------------------------- */

/* RFC 3986 §3.1: scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ). */
static bool csp_scheme_first(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static bool csp_scheme_char(char c)
{
    return csp_scheme_first(c) || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
}

/* §2.3.1: host-char = ALPHA / DIGIT / "-". Deliberately no "_" and nothing above ASCII. */
static bool csp_host_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}

/* §2.3.1's host-part, whole: "*" / [ "*." ] 1*host-char *( "." 1*host-char ) [ "." ]. The `1*` is what makes
   `*.` and a doubled dot invalid, and the trailing `[ "." ]` is what makes the fully-qualified `example.com.`
   valid — both are real spellings and neither is a special case here. */
static bool csp_host_part_valid(const char *p, size_t n)
{
    size_t i = 0;

    if (!n) return false;
    if (n == 1 && p[0] == '*') return true;
    if (n >= 2 && p[0] == '*' && p[1] == '.') i = 2;
    for (;;) {
        size_t s = i;

        while (i < n && csp_host_char(p[i])) i++;
        if (i == s) return false;      /* 1*host-char: a label may not be empty */
        if (i == n) return true;
        if (p[i] != '.') return false;
        i++;
        if (i == n) return true;       /* the optional trailing "." */
    }
}

/* §2.3.1: port-part = 1*DIGIT / "*". */
static bool csp_port_part_valid(const char *p, size_t n)
{
    size_t i;

    if (!n) return false;
    if (n == 1 && p[0] == '*') return true;
    for (i = 0; i < n; i++)
        if (p[i] < '0' || p[i] > '9') return false;
    return true;
}

/* ONE EXPRESSION, READ AS §2.3.1 WRITES IT. An absent part is `n == 0`, which is §6.7.2.8's "does not have a
   scheme-part" and its "port-part be … null" spelled the one way. */
typedef struct {
    CspToken scheme;      /* the scheme-part WITHOUT its ":" or "://" */
    CspToken host;        /* the host-part; empty for a scheme-source */
    CspToken port;        /* the port-part WITHOUT its ":" */
    CspToken path;        /* the path-part, WITH its leading "/" */
    bool     scheme_source;   /* the expression is `scheme-part ":"` and nothing else */
} CspHostSource;

/* Recognise scheme-source or host-source and split it. False for every other token — a keyword, a nonce, a
   hash, and anything the grammar does not admit — which §6.7.2.8 then falls past to its `'self'` arm. */
static bool csp_host_source_parse(CspToken e, CspHostSource *out)
{
    const char *p = e.p;
    size_t n = e.n, i = 0, rest;

    memset(out, 0, sizeof *out);
    if (!n) return false;
    /* THE SCHEME-PART IS ONLY A SCHEME-PART BEFORE "://" (or as the whole of a scheme-source). Searching for
       the first "://" anywhere would read `x.com:80/a://b` as a scheme named `x.com:80/a`, and reading a bare
       leading "…:" as a scheme would read `example.com:8080` as the scheme `example.com` — the port form is
       overwhelmingly the common one and both misreadings silently widen a policy. */
    if (csp_scheme_first(p[0])) {
        while (i < n && csp_scheme_char(p[i])) i++;
        if (i < n && p[i] == ':') {
            if (i + 1 == n) {                       /* scheme-source: `https:` */
                out->scheme.p = p;
                out->scheme.n = i;
                out->scheme_source = true;
                return true;
            }
            if (i + 2 < n && p[i + 1] == '/' && p[i + 2] == '/') {
                out->scheme.p = p;
                out->scheme.n = i;
                i += 3;
            } else {
                i = 0;                              /* the ":" belongs to a port-part */
            }
        } else {
            i = 0;
        }
    }
    rest = i;
    /* host-part: everything up to a ":" (port) or a "/" (path). */
    while (i < n && p[i] != ':' && p[i] != '/') i++;
    if (!csp_host_part_valid(p + rest, i - rest)) return false;
    out->host.p = p + rest;
    out->host.n = i - rest;
    if (i < n && p[i] == ':') {
        i++;
        rest = i;
        while (i < n && p[i] != '/') i++;
        if (!csp_port_part_valid(p + rest, i - rest)) return false;
        out->port.p = p + rest;
        out->port.n = i - rest;
    }
    if (i < n) {
        /* path-part = path-absolute, which begins with "/" — the only byte that can be here, since the host
           and port scans above stop at exactly ":" and "/" and the ":" was consumed. */
        DCHECK(p[i] == '/', "§2.3.1's host-source scan stopped at a byte that is neither a port nor a path — "
                            "the two scans above end only at ':' or '/', so this is a third exit nobody wrote");
        out->path.p = p + i;
        out->path.n = n - i;
    }
    return true;
}

/* §2.3.1's `scheme-source / host-source` as a PREDICATE — see csp_source_list.h for who asks and why it is
   exported rather than written again next door. The split is discarded: the caller that asks this is STORING
   the expression as text (Permissions Policy §4.7's allowlist holds "an ordered set of
   permissions-source-expression") and re-parses at the match, exactly as csp_source_match_url does. */
bool csp_source_is_scheme_or_host_source(CspToken expression)
{
    CspHostSource hs;

    return csp_host_source_parse(expression, &hs);
}

/* ---- §6.7.2.9 - §6.7.2.12, THE FOUR RELATIONS ------------------------------------------------------------ */

/* §6.7.2.9 scheme-part matching. ASYMMETRIC BY DESIGN: `http:` matches an https URL and not the other way
   round, which is the standard's "we always allow a secure upgrade from an explicitly insecure expression".
   `a` is the expression's (or the self-origin's) scheme; `b` is the URL's, which the URL parser has already
   lowercased. */
static CspMatch csp_scheme_part_match(CspToken a, const char *b)
{
    if (csp_token_is(a, b))                                                    return CSP_MATCHES;
    if (csp_token_is(a, "http") && !strcmp(b, "https"))                        return CSP_MATCHES;
    if (csp_token_is(a, "ws") &&
        (!strcmp(b, "wss") || !strcmp(b, "http") || !strcmp(b, "https")))      return CSP_MATCHES;
    if (csp_token_is(a, "wss") && !strcmp(b, "https"))                         return CSP_MATCHES;
    return CSP_DOES_NOT_MATCH;
}

/* §6.7.2.10 host-part matching. */
static CspMatch csp_host_part_match(CspToken pattern, const UrlHost *host)
{
    size_t i;

    /* STEP 1, and it is the whole reason an IP address is unspellable as a host-part: "if host is not a
       domain, return Does Not Match". §4.2's five kinds and only one of them is a domain. */
    if (host->kind != URL_HOST_DOMAIN)
        return CSP_DOES_NOT_MATCH;
    DCHECK(host->domain != NULL, "§4.2's DOMAIN host carries no bytes — a record the host parser produced "
                                 "always does, so this one was written by something else");
    if (pattern.n == 1 && pattern.p[0] == '*')                       /* step 2 */
        return CSP_MATCHES;
    if (pattern.n >= 2 && pattern.p[0] == '*' && pattern.p[1] == '.') {   /* step 3 */
        /* "remaining is pattern with the leading U+002A removed and ASCII lowercased" — so the DOT stays, and
           it is what stops `*.example.com` matching `notexample.com`. */
        size_t rn = pattern.n - 1, hn = strlen(host->domain);

        if (hn < rn) return CSP_DOES_NOT_MATCH;
        for (i = 0; i < rn; i++) {
            char a = pattern.p[1 + i], b = host->domain[hn - rn + i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) return CSP_DOES_NOT_MATCH;
        }
        return CSP_MATCHES;
    }
    /* Steps 4-5: an ASCII case-insensitive equality against the whole host. */
    {
        size_t hn = strlen(host->domain);

        if (hn != pattern.n) return CSP_DOES_NOT_MATCH;
        for (i = 0; i < hn; i++) {
            char a = pattern.p[i], b = host->domain[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) return CSP_DOES_NOT_MATCH;
        }
    }
    return CSP_MATCHES;
}

/* §6.7.2.11 port-part matching. `has_input` is the algorithm's "null input", which §6.7.2.8 produces for an
   expression with no port-part — and null is a REAL value here rather than an absence to skip: step 4 compares
   it against url's port, and a null on both sides MATCHES. That is what makes `script-src example.com` allow
   `http://example.com/x.js` and refuse `http://example.com:8080/x.js`. */
static CspMatch csp_port_part_match(CspToken input, bool has_input, const UrlRecord *url)
{
    long normalized = -1;   /* §4.1's null port, spelled as url.h spells it */
    size_t i;

    DCHECK(!has_input || csp_port_part_valid(input.p, input.n),
           "§6.7.2.11's assert: its input is null, \"*\", or a sequence of ASCII digits — anything else is an "
           "expression csp_host_source_parse should have refused to call a host-source");
    if (has_input && input.n == 1 && input.p[0] == '*')                        /* step 2 */
        return CSP_MATCHES;
    if (has_input) {                                                          /* step 3 */
        normalized = 0;
        for (i = 0; i < input.n; i++) {
            normalized = normalized * 10 + (input.p[i] - '0');
            /* §4.1 caps a URL's port at 65535, so any longer number is one no url->port can equal. Stopping
               here rather than wrapping is what keeps that true of an expression like `:999999999999`. */
            if (normalized > 65535) return CSP_DOES_NOT_MATCH;
        }
    }
    if (normalized == url->port)                                              /* step 4 */
        return CSP_MATCHES;
    if (url->port == -1) {                                                    /* step 5 */
        int def = url_default_port(url->scheme);
        if (def >= 0 && normalized == def) return CSP_MATCHES;
    }
    return CSP_DOES_NOT_MATCH;
}

/* INFRA's STRICT SPLIT on U+002F, counted rather than materialised: strictly splitting never drops an empty
   token, so the count is one more than the number of solidi and `/a/b` is « "", "a", "b" ». */
static size_t csp_path_seg_count(const char *s, size_t n)
{
    size_t i, c = 1;

    for (i = 0; i < n; i++)
        if (s[i] == '/') c++;
    return c;
}

/* The k-th segment of a strict split, by walking. */
static void csp_path_seg(const char *s, size_t n, size_t k, const char **out, size_t *out_n)
{
    size_t i = 0, start = 0, seen = 0;

    for (i = 0; i <= n; i++) {
        if (i == n || s[i] == '/') {
            if (seen == k) { *out = s + start; *out_n = i - start; return; }
            seen++;
            start = i + 1;
        }
    }
    DFAIL("a path segment past the end of a strict split was asked for — the caller counted the segments with "
          "csp_path_seg_count and then indexed past that count");
    *out = s;
    *out_n = 0;
}

/* §6.7.2.12 path-part matching. A is the expression's path-part, B the URL's serialized path. ASYMMETRIC:
   `/a/` matches `/a/b` and `/a/b` does not match `/a/`. The percent-DECODING in step 8 is why this cannot be a
   memcmp of the two strings: `/a b` and `/a%20b` are one path. */
static CspMatch csp_path_part_match(CspToken a, const char *b, size_t bn)
{
    size_t na, nb, i;
    bool exact;

    if (!a.n)                                                     /* step 1 */
        return CSP_MATCHES;
    if (a.n == 1 && a.p[0] == '/' && !bn)                         /* step 2 */
        return CSP_MATCHES;
    exact = a.p[a.n - 1] != '/';                                  /* step 3 */
    na = csp_path_seg_count(a.p, a.n);                            /* step 4 */
    nb = csp_path_seg_count(b, bn);
    if (na > nb)                                                  /* step 5 */
        return CSP_DOES_NOT_MATCH;
    if (exact && na != nb)                                        /* step 6 */
        return CSP_DOES_NOT_MATCH;
    if (!exact) {                                                 /* step 7 */
        const char *last;
        size_t last_n;

        csp_path_seg(a.p, a.n, na - 1, &last, &last_n);
        DCHECK(last_n == 0, "§6.7.2.12 step 7's assert: a path-part that does not end in a solidus was about "
                            "to have a NON-empty final segment removed — the split and the exact-match test "
                            "have disagreed about which character the path ends with");
        na--;
    }
    for (i = 0; i < na; i++) {                                    /* step 8 */
        const char *pa, *pb;
        size_t pan, pbn, da_n = 0, db_n = 0;
        char *da, *db;
        bool same;

        csp_path_seg(a.p, a.n, i, &pa, &pan);
        csp_path_seg(b, bn, i, &pb, &pbn);
        da = url_percent_decode(pa, pan, &da_n);
        db = url_percent_decode(pb, pbn, &db_n);
        CHECK(da != NULL && db != NULL, "CSP: OOM percent-decoding a path segment for §6.7.2.12");
        same = da_n == db_n && !memcmp(da, db, da_n);
        free(da);
        free(db);
        if (!same) return CSP_DOES_NOT_MATCH;
    }
    return CSP_MATCHES;                                           /* step 9 */
}

/* ---- §6.7.2.8 and §6.7.2.7 ------------------------------------------------------------------------------ */

/* Fetch §2.1 URL: "An HTTP(S) scheme is `http` or `https`" — §6.7.2.8 step 1's first condition, and the same
   sentence core/fetch/port_blocking.c reads for §2.9. */
static bool csp_http_scheme(const char *scheme)
{
    return !strcmp(scheme, "http") || !strcmp(scheme, "https");
}

/* "origin's port and url's port are either the same or the DEFAULT PORTS for their respective schemes", with
   §4.1's normalisation folded in where it belongs: the URL parser DROPS a scheme's default port, so a null
   port IS that scheme's default port — `https://x:443/` and `https://x/` are one record and must answer the
   same here. */
static bool csp_port_is_default_for(int port, const char *scheme)
{
    return port == -1 || port == url_default_port(scheme);
}

CspMatch csp_source_match_url(CspToken expression, const UrlRecord *url, const Origin *self_origin,
                              int redirect_count)
{
    CspHostSource hs;

    DCHECK(url != NULL && url->scheme != NULL,
           "§6.7.2.8 was asked about a URL with no scheme — every arm of it reads one, and a record without "
           "one came from a parse that FAILED and whose failure the caller read as success");
    DCHECK(self_origin != NULL,
           "§6.7.2.8 was asked with NO self-origin. §2.2 puts one on every CSP list and §2.2.2 states it from "
           "the response's URL; without it the `'self'` keyword — which is in nearly every policy on the web — "
           "would match nothing, and a page's own scripts would report as CSP-blocked");
    DCHECK(redirect_count >= 0, "§6.7.2.8 was given a negative redirect count — Fetch's request holds a count "
                                "of redirects followed, which begins at zero");

    /* STEP 1: `*`. It matches any HTTP(S) URL, and any URL sharing the self-origin's scheme — which is what
       lets a `custom-scheme:` page load its own resources under `default-src *` and stops `*` from being a
       licence for `data:`. */
    if (expression.n == 1 && expression.p[0] == '*') {
        if (csp_http_scheme(url->scheme))
            return CSP_MATCHES;
        if (!origin_is_opaque(self_origin) && !strcmp(origin_scheme(self_origin), url->scheme))
            return CSP_MATCHES;
        return CSP_DOES_NOT_MATCH;
    }

    if (csp_host_source_parse(expression, &hs)) {
        /* STEP 2, which covers BOTH grammars: an expression that carries a scheme-part must scheme-part match,
           and a scheme-source is then already the whole answer. */
        if (hs.scheme.n && csp_scheme_part_match(hs.scheme, url->scheme) == CSP_DOES_NOT_MATCH)
            return CSP_DOES_NOT_MATCH;
        if (hs.scheme_source)
            return CSP_MATCHES;

        /* STEP 3, the host-source arm. */
        if (url->host.kind == URL_HOST_NULL)
            return CSP_DOES_NOT_MATCH;
        /* A SCHEMELESS host-source is measured against the SELF-ORIGIN's scheme, upgrade rule included — which
           is how `script-src example.com` on an http page also permits `https://example.com`. An OPAQUE
           self-origin has no scheme to compare, so nothing can match it: that is §7.1.1's "the only meaningful
           operation is testing for equality" and not a case to skip. */
        if (!hs.scheme.n) {
            CspToken os;

            if (origin_is_opaque(self_origin))
                return CSP_DOES_NOT_MATCH;
            os.p = origin_scheme(self_origin);
            os.n = strlen(os.p);
            if (csp_scheme_part_match(os, url->scheme) == CSP_DOES_NOT_MATCH)
                return CSP_DOES_NOT_MATCH;
        }
        if (csp_host_part_match(hs.host, &url->host) == CSP_DOES_NOT_MATCH)
            return CSP_DOES_NOT_MATCH;
        if (csp_port_part_match(hs.port, hs.port.n != 0, url) == CSP_DOES_NOT_MATCH)
            return CSP_DOES_NOT_MATCH;
        /* THE PATH IS COMPARED ONLY ON AN UNREDIRECTED REQUEST. Once a redirect has happened the path is the
           SERVER's choice rather than the policy author's, and comparing it would let an open redirector on an
           allowed host be turned into a path filter — so the standard drops the comparison instead. */
        if (hs.path.n && redirect_count == 0) {
            char *path = url_serialize_path(url);
            CspMatch m;

            CHECK(path != NULL, "CSP: OOM serializing a URL's path for §6.7.2.8");
            m = csp_path_part_match(hs.path, path, strlen(path));
            free(path);
            if (m == CSP_DOES_NOT_MATCH)
                return CSP_DOES_NOT_MATCH;
        }
        return CSP_MATCHES;
    }

    /* STEP 4: `'self'`. Its first bullet is §7.1.1's same origin outright; its second is the SECURE-UPGRADE
       case, which the standard limits to a URL on the same host at a port that is either the origin's own or
       the default for its scheme. */
    if (csp_token_is(expression, "'self'")) {
        if (origin_same_as_url(self_origin, url))
            return CSP_MATCHES;
        if (!origin_is_opaque(self_origin)) {
            const char *os = origin_scheme(self_origin);
            int op = origin_port(self_origin);

            if (url_host_equal(origin_host(self_origin), &url->host) &&
                (op == url->port || (csp_port_is_default_for(op, os) &&
                                     csp_port_is_default_for(url->port, url->scheme)))) {
                if (!strcmp(url->scheme, "https") || !strcmp(url->scheme, "wss"))
                    return CSP_MATCHES;
                if (!strcmp(os, "http") && (!strcmp(url->scheme, "http") || !strcmp(url->scheme, "ws")))
                    return CSP_MATCHES;
            }
        }
    }
    /* STEP 5 — and it is where every keyword, nonce and hash lands. None of them names a URL, so none of them
       can permit one: `script-src 'unsafe-inline'` allows no script LOAD at all, which is the answer that
       surprises policy authors and is exactly what the standard says. */
    return CSP_DOES_NOT_MATCH;
}

CspMatch csp_source_list_match_url(const CspDirective *directive, const UrlRecord *url,
                                   const Origin *self_origin, int redirect_count)
{
    size_t i;

    DCHECK(directive != NULL,
           "§6.7.2.7's assert: source list is not null. A policy that carries no governing directive says "
           "NOTHING about this request, which §6.8.4 decides and the CALLER answers — it is not a source list "
           "that matches nothing");
    /* STEP 2: an EMPTY source list matches nothing. `script-src` with no value is a real and deliberate
       policy, and the standard's own note says it is equivalent to `'none'`. */
    if (!directive->n_value)
        return CSP_DOES_NOT_MATCH;
    /* STEP 3: `'none'` ALONE. The size test is the whole of it and is not a shortcut — the standard's second
       note says `'none'` has no effect when other expressions are present, so « 'none', https://example.com »
       matches that host. */
    if (directive->n_value == 1 && csp_token_is(directive->value[0], "'none'"))
        return CSP_DOES_NOT_MATCH;
    for (i = 0; i < directive->n_value; i++)                          /* step 4 */
        if (csp_source_match_url(directive->value[i], url, self_origin, redirect_count) == CSP_MATCHES)
            return CSP_MATCHES;
    return CSP_DOES_NOT_MATCH;                                        /* step 5 */
}
