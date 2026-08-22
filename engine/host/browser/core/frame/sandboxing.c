/* HTML §7.1.5's sandboxing flag set — see sandboxing.h for why it is not a field of the policy container.
 *
 * THE PARSE IS THE WHOLE OF THE COMPONENT, and it is stated as the spec states it: a list of flags that are
 * ADDED, each with its own "unless tokens contains ..." clause, two of them naming the SAME keyword and two of
 * them naming SEVERAL. Written the other way round — a switch over keywords that clears bits — the two
 * many-to-one cases (`allow-scripts` relaxes scripts AND automatic features; `allow-popups` and
 * `allow-top-navigation` each also relax custom protocols) become special cases someone has to remember, and
 * `allow-top-navigation` silently overriding `allow-top-navigation-by-user-activation` stops being visible at
 * all. Here every clause is one line of the standard. */
#include <string.h>

#include "check.h"
#include "core/frame/sandboxing.h"

/* An ASCII case-insensitive match between a token and a sandbox keyword. §7.1.5's keywords are matched that
   way because the `sandbox` content attribute's value is an unordered set of unique space-separated tokens,
   and §2.4.7 says those are compared ASCII case-insensitively. */
static bool tok_is(const char *tok, size_t n, const char *keyword)
{
    size_t k = strlen(keyword), i;

    if (n != k) return false;
    for (i = 0; i < n; i++) {
        char a = tok[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != keyword[i]) return false;
    }
    return true;
}

/* "Split input on ASCII whitespace, to obtain tokens", then "tokens contains `keyword`". Asked once per
   clause rather than tokenised into an allocated list: the clauses are a fixed sixteen and the input is one
   attribute value, so a pass per clause is a walk over a short string and needs no allocation to fail at. */
static bool tokens_contain(const char *input, size_t len, const char *keyword)
{
    size_t i = 0;

    while (i < len) {
        size_t s;
        /* INFRA's ASCII whitespace: TAB, LF, FF, CR, SPACE. */
        while (i < len && (input[i] == ' ' || input[i] == '\t' || input[i] == '\n' || input[i] == '\f' ||
                           input[i] == '\r'))
            i++;
        s = i;
        while (i < len && !(input[i] == ' ' || input[i] == '\t' || input[i] == '\n' || input[i] == '\f' ||
                            input[i] == '\r'))
            i++;
        if (i > s && tok_is(input + s, i - s, keyword)) return true;
    }
    return false;
}

SandboxFlags sandbox_parse_directive(const char *input, size_t len)
{
    SandboxFlags out = 0;
    bool allow_popups, allow_top, allow_top_by_ua, allow_scripts;

    /* An ABSENT value is not a distinguishable case here: `sandbox` with no value and `sandbox=""` are the
       same directive with an empty token list, and both are the most restrictive form of the attribute. The
       caller decides whether the attribute is present at all — an ABSENT attribute is an EMPTY flag set, and
       that decision cannot be made from the value. */
    if (!input) len = 0;
    allow_popups    = tokens_contain(input, len, "allow-popups");
    allow_top       = tokens_contain(input, len, "allow-top-navigation");
    allow_top_by_ua = tokens_contain(input, len, "allow-top-navigation-by-user-activation");
    allow_scripts   = tokens_contain(input, len, "allow-scripts");

    out |= SANDBOX_NAVIGATION;                                              /* unconditional */
    if (!allow_popups)                    out |= SANDBOX_AUXILIARY_NAVIGATION;
    if (!allow_top)                       out |= SANDBOX_TOP_LEVEL_NAVIGATION_WITHOUT_USER_ACTIVATION;
    /* "unless tokens contains either the allow-top-navigation-by-user-activation keyword or the
       allow-top-navigation keyword" — which is why specifying both is a conformance error: the broader one
       already covers this clause, so the narrower one has no effect. */
    if (!allow_top_by_ua && !allow_top)   out |= SANDBOX_TOP_LEVEL_NAVIGATION_WITH_USER_ACTIVATION;
    if (!tokens_contain(input, len, "allow-same-origin"))  out |= SANDBOX_ORIGIN;
    if (!tokens_contain(input, len, "allow-forms"))        out |= SANDBOX_FORMS;
    if (!tokens_contain(input, len, "allow-pointer-lock")) out |= SANDBOX_POINTER_LOCK;
    if (!allow_scripts)                   out |= SANDBOX_SCRIPTS;
    /* "This flag is relaxed by the same keyword as scripts, because when scripts are enabled these features
       are trivially possible anyway" — one keyword, two flags, which is why the parse is written as a list of
       additions rather than as a switch that clears a bit per keyword. */
    if (!allow_scripts)                   out |= SANDBOX_AUTOMATIC_FEATURES;
    out |= SANDBOX_DOCUMENT_DOMAIN;                                         /* unconditional */
    if (!tokens_contain(input, len, "allow-popups-to-escape-sandbox")) out |= SANDBOX_PROPAGATES_TO_AUXILIARY;
    if (!tokens_contain(input, len, "allow-modals"))            out |= SANDBOX_MODALS;
    if (!tokens_contain(input, len, "allow-orientation-lock"))  out |= SANDBOX_ORIENTATION_LOCK;
    if (!tokens_contain(input, len, "allow-presentation"))      out |= SANDBOX_PRESENTATION;
    if (!tokens_contain(input, len, "allow-downloads"))         out |= SANDBOX_DOWNLOADS;
    /* "unless tokens contains either the allow-top-navigation-to-custom-protocols keyword, the allow-popups
       keyword, or the allow-top-navigation keyword" — three keywords, one flag. */
    if (!tokens_contain(input, len, "allow-top-navigation-to-custom-protocols") && !allow_popups && !allow_top)
        out |= SANDBOX_CUSTOM_PROTOCOLS_NAVIGATION;
    return out;
}

SandboxFlags sandbox_creation_flags(const SandboxEmbedder *embedder, SandboxFlags popup_flags)
{
    /* "Return the UNION of the flags that are present in the following sandboxing flag sets: if embedder is
       null, then the flags set on browsing context's popup sandboxing flag set; if embedder is an element,
       then the flags set on embedder's iframe sandboxing flag set; if embedder is an element, then the flags
       set on embedder's node document's active sandboxing flag set."
       THE TWO ARMS ARE EXCLUSIVE IN THE STANDARD'S OWN TEXT — a navigable is nested through an element or it
       is a top-level traversable — so a caller that supplies both has not decided which it is building, and
       the popup set of a nested navigable is a set that does not exist. */
    if (embedder) {
        DCHECK(popup_flags == 0,
               "§7.1.5's determine-the-creation-sandboxing-flags was given BOTH an embedder element and a "
               "popup sandboxing flag set — a POPUP flag set belongs to a TOP-LEVEL browsing context and an "
               "embedder makes this navigable a nested one, so one of the two callers is describing the wrong "
               "kind of navigable");
        return embedder->iframe_flags | embedder->document_flags;
    }
    return popup_flags;
}

SandboxFlags sandbox_popup_flags(SandboxFlags source_flags)
{
    /* §7.1: "If sandboxingFlagSet's sandbox propagates to auxiliary browsing contexts flag is set, then all
       the flags that are set in sandboxingFlagSet must be set in chosen's active browsing context's popup
       sandboxing flag set." The flag's own definition says what it is for: "prevents content from escaping
       the sandbox by ensuring that any auxiliary browsing context it creates INHERITS the content's active
       sandboxing flag set". Without it the popup starts clean, which is exactly what
       `allow-popups-to-escape-sandbox` asks for. */
    return (source_flags & SANDBOX_PROPAGATES_TO_AUXILIARY) ? source_flags : 0;
}

const char *sandbox_flag_name(SandboxFlags one)
{
#define SANDBOX_FLAG_NAME_(name, text) if (one == (SandboxFlags)name) return text;
    SANDBOX_FLAG_LIST(SANDBOX_FLAG_NAME_)
#undef SANDBOX_FLAG_NAME_
    DFAIL("a sandboxing flag was asked for its §7.1.5 name and it is not ONE flag of that set — a message "
          "naming a union of flags names none of them, so the caller has a set where it wanted a member");
    return "";
}
