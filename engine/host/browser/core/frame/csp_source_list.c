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
 * WHAT IS DELIBERATELY NOT HERE. host-source, scheme-source and path-part are §2.3.1 productions this file
 * does not recognise, and that is not a gap: §6.7.3.2 does not look at them, so a recogniser for them would
 * have no caller and no test, which is the shape of a stub. They arrive with §6.7.2's URL matching, whose
 * caller is Fetch. */
#include <string.h>

#include "check.h"
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

bool csp_source_is_nonce(CspToken expression)
{
    static const char PREFIX[] = "'nonce-";

    if (!csp_prefix_ci(expression, PREFIX)) return false;
    return csp_quoted_base64_tail(expression, sizeof PREFIX - 1);
}

bool csp_source_is_hash(CspToken expression)
{
    /* hash-algorithm is one of three, and each spelling is its own opening literal. A table rather than a
       parse of the algorithm name, because the production admits exactly these three and nothing else — an
       expression naming `'sha1-…'` is not a hash-source at all and must not override 'unsafe-inline'. */
    static const char *const OPENERS[] = { "'sha256-", "'sha384-", "'sha512-", NULL };
    size_t i;

    for (i = 0; OPENERS[i]; i++) {
        if (!csp_prefix_ci(expression, OPENERS[i])) continue;
        return csp_quoted_base64_tail(expression, strlen(OPENERS[i]));
    }
    return false;
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
