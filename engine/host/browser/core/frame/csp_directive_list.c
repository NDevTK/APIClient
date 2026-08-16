/* CSP §2.2.1's parse and §6.8's directive algorithms — see csp_directive_list.h for why this is one component
 * and why the source list is not part of it.
 *
 * THE PARSE IS THE STANDARD'S STEPS IN THE STANDARD'S ORDER, and three of them are the ones an ad-hoc scanner
 * always drops: a token is STRIPPED of leading and trailing ASCII whitespace before anything is read from it,
 * a token that is not an ASCII string is DISCARDED WHOLE rather than parsed for the part that is ASCII, and a
 * directive whose name is already in this policy is IGNORED rather than merged or replaced. The duplicate rule
 * is the one with teeth: `script-src 'unsafe-inline'; script-src 'self'` permits inline script, because the
 * SECOND directive does not exist — while the very same two directives written as two comma-delimited POLICIES
 * intersect and forbid it. A scanner that takes the last-seen directive gets both of those backwards.
 *
 * ONE DEVIATION FROM THE LETTER, and it is editorial rather than substantive: §2.2.1 step 3.5 says "let
 * directive value be the result of splitting TOKEN on ASCII whitespace", which read literally would put the
 * directive's own NAME into its value. It cannot mean that — §6.7.2.7 decides 'none' by testing that the
 * source list's size is 1, so `script-src 'none'` would have a list of size 2 and 'none' would never take
 * effect anywhere. The name is collected from the token by "collecting a sequence of code points", which in
 * INFRA advances a position, and the value is the REMAINDER. That is what is implemented. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/frame/csp_directive_list.h"

/* INFRA's ASCII WHITESPACE: TAB, LF, FF, CR, SPACE. Spelled out rather than reached for through <ctype.h>,
   whose isspace() is locale-dependent and includes VT. */
static bool csp_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

static bool csp_is_ascii_lowercase(const char *s)
{
    size_t i;

    for (i = 0; s[i]; i++)
        if (s[i] >= 'A' && s[i] <= 'Z') return false;
    return true;
}

bool csp_token_is(CspToken token, const char *ascii_lowercase)
{
    size_t k, i;

    DCHECK(ascii_lowercase != NULL && csp_is_ascii_lowercase(ascii_lowercase),
           "a CSP token was compared against a literal that is not ASCII lowercase — the comparison folds the "
           "TOKEN's case and not the literal's, so a capital here matches nothing while reading as if it must");
    k = strlen(ascii_lowercase);
    if (token.n != k) return false;
    for (i = 0; i < k; i++) {
        char a = token.p[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != ascii_lowercase[i]) return false;
    }
    return true;
}

/* Two token slices, compared the way §2.2.1 compares directive names: the standard lowercases the name before
   the containment test, so the test is ASCII case-insensitive. */
static bool csp_tokens_equal_ci(CspToken a, CspToken b)
{
    size_t i;

    if (a.n != b.n) return false;
    for (i = 0; i < a.n; i++) {
        char x = a.p[i], y = b.p[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

/* §2.3's value: "the result of splitting the remainder on ASCII whitespace" — INFRA's split, which strips and
   yields no empty strings. */
static void csp_directive_split_value(CspDirective *d)
{
    size_t i = 0;

    while (i < d->value_text.n) {
        size_t s;
        CspToken *grown;

        while (i < d->value_text.n && csp_ws(d->value_text.p[i])) i++;
        s = i;
        while (i < d->value_text.n && !csp_ws(d->value_text.p[i])) i++;
        if (i == s) break;
        grown = realloc(d->value, (d->n_value + 1) * sizeof *grown);
        CHECK(grown != NULL, "CSP: OOM splitting a directive value");
        d->value = grown;
        d->value[d->n_value].p = d->value_text.p + s;
        d->value[d->n_value].n = i - s;
        d->n_value++;
    }
}

/* §2.2.1 steps 3.6-3.8: append a directive to the policy's directive set. */
static void csp_policy_append(CspPolicy *policy, CspToken name, CspToken value_text)
{
    CspDirective *grown = realloc(policy->directives, (policy->n_directives + 1) * sizeof *grown);
    CspDirective *d;

    CHECK(grown != NULL, "CSP: OOM appending a directive to a policy's directive set");
    policy->directives = grown;
    d = &policy->directives[policy->n_directives++];
    d->name = name;
    d->value_text = value_text;
    d->value = NULL;
    d->n_value = 0;
    csp_directive_split_value(d);
}

/* §2.2.1 over ONE serialized policy. */
static void csp_policy_parse(CspPolicy *out, const char *text, size_t len)
{
    size_t pos = 0;

    /* "For each token returned by STRICTLY SPLITTING serialized on U+003B SEMICOLON" — strictly, so an empty
       token between two semicolons is produced and then discarded by step 3.2 rather than skipped silently. */
    for (;;) {
        size_t s = pos, e;
        size_t k, name_end;
        CspToken name, value_text;
        bool ascii = true, duplicate = false;

        while (pos < len && text[pos] != ';') pos++;
        e = pos;

        /* 3.1: strip leading and trailing ASCII whitespace from token. */
        while (s < e && csp_ws(text[s])) s++;
        while (e > s && csp_ws(text[e - 1])) e--;
        /* 3.2: an empty token, or one that is not an ASCII string, is discarded WHOLE. */
        for (k = s; k < e; k++)
            if ((unsigned char)text[k] > 0x7F) { ascii = false; break; }
        if (e > s && ascii) {
            /* 3.3: the directive name is the leading run of non-whitespace code points. */
            name_end = s;
            while (name_end < e && !csp_ws(text[name_end])) name_end++;
            name.p = text + s;
            name.n = name_end - s;
            /* 3.5: the value is the remainder, split on ASCII whitespace. Its leading whitespace is dropped
               here so that value_text is exactly the bytes another standard's parse would be handed. */
            while (name_end < e && csp_ws(text[name_end])) name_end++;
            value_text.p = text + name_end;
            value_text.n = e - name_end;
            /* 3.4: a directive whose name is ALREADY in this policy is IGNORED — the first one wins. */
            for (k = 0; k < out->n_directives; k++)
                if (csp_tokens_equal_ci(out->directives[k].name, name)) { duplicate = true; break; }
            if (!duplicate)
                csp_policy_append(out, name, value_text);
        }

        if (pos >= len) break;
        pos++;   /* past the U+003B that ended this token */
    }
}

void csp_list_parse(CspList *out, const char *serialized, size_t len)
{
    size_t pos = 0;

    DCHECK(out != NULL, "a CSP list was parsed into nothing");
    DCHECK(out->policies == NULL && out->n_policies == 0,
           "a CSP list was parsed over one that already holds policies — csp_list_free frees exactly what ONE "
           "parse allocated, so the earlier parse's directive arrays would be unreachable and unfreed");
    if (!serialized || !len) {
        DCHECK(len == 0 || serialized != NULL, "a CSP list parse was given a length with no bytes");
        return;
    }
    /* A serialized CSP LIST is comma-delimited (§2.2). */
    for (;;) {
        size_t start = pos;
        CspPolicy policy;

        while (pos < len && serialized[pos] != ',') pos++;
        memset(&policy, 0, sizeof policy);
        csp_policy_parse(&policy, serialized + start, pos - start);
        /* §2.2.2: "if policy's directive set is NOT EMPTY, append policy to policies." An empty one is not a
           policy that permits everything, it is not a policy at all — which matters because every caller's
           quantifier runs over this list, and an "any policy requires X" question would otherwise count it. */
        if (policy.n_directives) {
            CspPolicy *grown = realloc(out->policies, (out->n_policies + 1) * sizeof *grown);
            CHECK(grown != NULL, "CSP: OOM appending a policy to a CSP list");
            out->policies = grown;
            out->policies[out->n_policies++] = policy;
        } else {
            DCHECK(policy.directives == NULL,
                   "a policy with an empty directive set still allocated its directive array, so dropping it "
                   "here leaks — the append and the count have disagreed");
        }
        if (pos >= len) break;
        pos++;   /* past the U+002C that ended this policy */
    }
}

void csp_list_free(CspList *list)
{
    size_t i, j;

    if (!list) return;
    for (i = 0; i < list->n_policies; i++) {
        for (j = 0; j < list->policies[i].n_directives; j++)
            free(list->policies[i].directives[j].value);
        free(list->policies[i].directives);
    }
    free(list->policies);
    list->policies = NULL;
    list->n_policies = 0;
}

const CspDirective *csp_policy_directive(const CspPolicy *policy, const char *name)
{
    size_t i;

    DCHECK(policy != NULL, "a directive was looked up in a policy that does not exist");
    DCHECK(name != NULL && csp_is_ascii_lowercase(name),
           "a directive was looked up by a name that is not ASCII lowercase — §2.2.1 stores names lowercased "
           "and this model compares them case-insensitively against the KEY, so a capital never matches");
    for (i = 0; i < policy->n_directives; i++)
        if (csp_token_is(policy->directives[i].name, name))
            return &policy->directives[i];
    return NULL;
}

const char *csp_effective_directive_for_inline_checks(CspInlineType type)
{
    switch (type) {
    /* §6.8.2: "script" AND "navigation" both map to script-src-elem. The navigation row is the one that is
       easy to get wrong and expensive to get wrong: a javascript: URL is governed with the script ELEMENT, so
       `script-src-attr 'none'` does not touch it, and §6.1.11 states the same thing from the other side
       ("script-src-elem applies to inline checks whose type is 'script' and 'navigation'"). */
    case CSP_INLINE_SCRIPT:
    case CSP_INLINE_NAVIGATION:       return "script-src-elem";
    case CSP_INLINE_SCRIPT_ATTRIBUTE: return "script-src-attr";
    case CSP_INLINE_STYLE:            return "style-src-elem";
    case CSP_INLINE_STYLE_ATTRIBUTE:  return "style-src-attr";
    }
    DFAIL("§6.8.2 was asked for the effective directive of an inline-check type the standard does not define — "
          "its trailing 'return null' covers types other specifications add, and a null here would silently "
          "govern nothing at all");
    return NULL;
}

const char *const *csp_fetch_directive_fallback_list(const char *directive_name)
{
    /* §6.8.3, one row per switch arm, in the standard's own order. The rows are static because they are the
       standard's text and not state; the arrays end in NULL so a caller walks them without a second count. */
    static const char *const SCRIPT_ELEM[] = { "script-src-elem", "script-src", "default-src", NULL };
    static const char *const SCRIPT_ATTR[] = { "script-src-attr", "script-src", "default-src", NULL };
    static const char *const STYLE_ELEM[]  = { "style-src-elem", "style-src", "default-src", NULL };
    static const char *const STYLE_ATTR[]  = { "style-src-attr", "style-src", "default-src", NULL };
    static const char *const WORKER[]      = { "worker-src", "child-src", "script-src", "default-src", NULL };
    static const char *const CONNECT[]     = { "connect-src", "default-src", NULL };
    static const char *const MANIFEST[]    = { "manifest-src", "default-src", NULL };
    static const char *const OBJECT[]      = { "object-src", "default-src", NULL };
    static const char *const FRAME[]       = { "frame-src", "child-src", "default-src", NULL };
    static const char *const MEDIA[]       = { "media-src", "default-src", NULL };
    static const char *const FONT[]        = { "font-src", "default-src", NULL };
    static const char *const IMG[]         = { "img-src", "default-src", NULL };
    /* §6.8.3's trailing "return «»". `script-src`, `style-src`, `child-src` and `default-src` land here, and
       that is correct rather than a gap: they are only ever ENTRIES in another directive's list, never what
       §6.8.1 or §6.8.2 returns, so nothing ever asks for their own. */
    static const char *const NONE[]        = { NULL };

    DCHECK(directive_name != NULL && csp_is_ascii_lowercase(directive_name),
           "§6.8.3 was asked for the fallback list of a name that is not ASCII lowercase");
    if (!strcmp(directive_name, "script-src-elem")) return SCRIPT_ELEM;
    if (!strcmp(directive_name, "script-src-attr")) return SCRIPT_ATTR;
    if (!strcmp(directive_name, "style-src-elem"))  return STYLE_ELEM;
    if (!strcmp(directive_name, "style-src-attr"))  return STYLE_ATTR;
    if (!strcmp(directive_name, "worker-src"))      return WORKER;
    if (!strcmp(directive_name, "connect-src"))     return CONNECT;
    if (!strcmp(directive_name, "manifest-src"))    return MANIFEST;
    if (!strcmp(directive_name, "object-src"))      return OBJECT;
    if (!strcmp(directive_name, "frame-src"))       return FRAME;
    if (!strcmp(directive_name, "media-src"))       return MEDIA;
    if (!strcmp(directive_name, "font-src"))        return FONT;
    if (!strcmp(directive_name, "img-src"))         return IMG;
    return NONE;
}

bool csp_should_fetch_directive_execute(const char *effective_name, const char *directive_name,
                                        const CspPolicy *policy)
{
    const char *const *fallback = csp_fetch_directive_fallback_list(effective_name);
    size_t i;

    DCHECK(policy != NULL, "§6.8.4 was asked about a policy that does not exist");
    DCHECK(directive_name != NULL, "§6.8.4 was asked about a directive with no name");
    DCHECK(fallback[0] != NULL,
           "§6.8.4 was asked about an EFFECTIVE directive that §6.8.3 gives no fallback list — every answer is "
           "then No, so no directive governs the check and the policy reads as if it said nothing. §6.8.3's "
           "lists exist for exactly the names §6.8.1 and §6.8.2 can RETURN; a name outside that set means the "
           "caller asked with a fallback ENTRY (`script-src`, `default-src`) in place of an effective one");
    for (i = 0; fallback[i]; i++) {
        if (!strcmp(directive_name, fallback[i])) return true;
        if (csp_policy_directive(policy, fallback[i])) return false;
    }
    return false;
}

const CspDirective *csp_policy_governing_directive(const CspPolicy *policy, const char *effective_name)
{
    const char *const *fallback = csp_fetch_directive_fallback_list(effective_name);
    size_t i;

    DCHECK(policy != NULL, "the governing directive was asked of a policy that does not exist");
    for (i = 0; fallback[i]; i++) {
        const CspDirective *d = csp_policy_directive(policy, fallback[i]);
        if (d) {
            DCHECK(csp_should_fetch_directive_execute(effective_name, fallback[i], policy),
                   "the FIRST directive of §6.8.3's fallback list that this policy carries is not the one "
                   "§6.8.4 says executes — the two readings of one list have diverged, and the check would "
                   "then be decided by a directive the standard says defers");
            return d;
        }
    }
    /* No directive of this policy governs the check. §6.8.4 says No for every one of them, and §4.2.3's loop
       therefore leaves `result` at "Allowed" — a policy that says NOTHING about this behavior. */
    return NULL;
}
