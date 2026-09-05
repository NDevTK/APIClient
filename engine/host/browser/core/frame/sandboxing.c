/* HTML §7.1.5's sandboxing flag set — see sandboxing.h for why it is not a field of the policy container.
 *
 * THE PARSE IS THE WHOLE OF THE COMPONENT, and it is stated as the spec states it: a list of flags that are
 * ADDED, each with its own "unless tokens contains ..." clause, two of them naming the SAME keyword and two of
 * them naming SEVERAL. Written the other way round — a switch over keywords that clears bits — the two
 * many-to-one cases (`allow-scripts` relaxes scripts AND automatic features; `allow-popups` and
 * `allow-top-navigation` each also relax custom protocols) become special cases someone has to remember, and
 * `allow-top-navigation` silently overriding `allow-top-navigation-by-user-activation` stops being visible at
 * all. Here every clause is one line of the standard. */
#include <stdlib.h>
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

bool sandbox_keyword_supported(const char *token, size_t len)
{
    DCHECK(token != NULL,
           "HTML §4.8.5 \"The iframe element\"'s supported-token question was asked with no token — the "
           "caller resolves the candidate string before asking, so a null here is a caller that asked "
           "without one");
    /* The empty directive is the most restrictive set there is, so ANY relaxation is a keyword the clauses
       above name. Derived rather than listed — see the declaration for why that is the whole point. */
    return sandbox_parse_directive(token, len) != sandbox_parse_directive(NULL, 0);
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
    /* §7.3.1.7 "Navigable target names"' rules for choosing a navigable: "If sandboxingFlagSet's sandbox
       propagates to auxiliary browsing contexts flag is set, then all the flags that are set in
       sandboxingFlagSet must be set in chosen's active browsing context's popup sandboxing flag set." The
       flag's own definition is one section away, in §7.1.5 Sandboxing, and says what it is for: "This flag
       prevents content from escaping the sandbox by ensuring that any auxiliary browsing context it creates
       inherits the content's active sandboxing flag set". Without it the popup starts clean, which is
       exactly what `allow-popups-to-escape-sandbox` asks for. */
    return (source_flags & SANDBOX_PROPAGATES_TO_AUXILIARY) ? source_flags : 0;
}

/* §7.1.5's SET AS TEXT — see sandboxing.h for why the vocabulary is the standard's own flag names and why the
   separator is a comma. Both directions are here, next to the list they are over, so a flag added to
   SANDBOX_FLAG_LIST gains its wire spelling in the same edit that gives it a bit. */
/* THE SET IS WALKED THROUGH SANDBOX_FLAG_LIST AND NEVER BY SHIFTING A BIT ALONG. Both spellings read the
   same at sixteen flags, and only one of them still reads at thirty-two: `1u << SANDBOX_FLAG_COUNT_` is
   undefined the day the list reaches the width of the word it is a set of, which is a bug that arrives with an
   edit to a DIFFERENT file and shows as whatever the optimiser felt like. The X macro is also what makes the
   pairing structural — a flag added to the list gains its bit, its name and its wire spelling in one edit,
   with nothing here to forget. */
char *sandbox_flags_serialize(SandboxFlags flags)
{
    /* THE DECLARED SET, AS ONE VALUE, so the assert below is over §7.1.5's list rather than over a count. */
#define SANDBOX_FLAG_OR_(name, text) | (SandboxFlags)(name)
    const SandboxFlags declared = 0 SANDBOX_FLAG_LIST(SANDBOX_FLAG_OR_);
#undef SANDBOX_FLAG_OR_
    size_t n = 1;   /* the NUL */
    char *out, *q;

    /* THE WHOLE WORD IS ACCOUNTED FOR, NOT ONLY THE BITS THIS BUILD DECLARES. A set carrying a bit outside
       §7.1.5's list would serialize to a string that says LESS than the value does, and the peer would then be
       created with only the flags this side could spell — a sandbox silently weaker than the markup asks for,
       which is the direction that matters and the one nothing downstream could see. It cannot arise (every
       producer is one of the three algorithms above, each an OR of declared bits), so it is an invariant, and
       it is asserted at the one place where the value is turned into something lossy. */
    DCHECKF((flags & ~declared) == 0,
            "a §7.1.5 sandboxing flag set carrying bits this build does not declare (0x%x outside 0x%x) was "
            "about to cross an instance boundary — that section's flags are the whole of the set, so a bit "
            "outside them is a producer that ORed in something which is not one, and the peer would be created "
            "with a WEAKER sandbox than the markup states with no field anywhere to say so",
            (unsigned)(flags & ~declared), (unsigned)declared);
    if (flags == 0) {
        out = malloc(sizeof SANDBOX_FLAGS_SERIALIZED_NONE);
        if (out) memcpy(out, SANDBOX_FLAGS_SERIALIZED_NONE, sizeof SANDBOX_FLAGS_SERIALIZED_NONE);
        return out;
    }
    /* `sizeof text` IS THE NAME AND ITS SEPARATOR — the literal's length plus one — so this over-counts by
       exactly one byte (the last member needs no separator, and `n` already carries the NUL). Over by one is
       the safe direction and is cheaper than a second walk to get it exact. */
#define SANDBOX_FLAG_LEN_(name, text) if (flags & (SandboxFlags)(name)) n += sizeof text;
    SANDBOX_FLAG_LIST(SANDBOX_FLAG_LEN_)
#undef SANDBOX_FLAG_LEN_
    out = malloc(n);
    if (!out) return NULL;
    q = out;
    /* THE SEPARATOR'S OWN INVARIANT, ASSERTED OVER THE VOCABULARY RATHER THAN ASSUMED OF IT. A name that
       gained a comma — or a TAB, which is what the record carrying this field splits on — would make the field
       mean something else at the far end, and this is the only place that can see it happen. */
#define SANDBOX_FLAG_EMIT_(name, text)                                                                       \
    if (flags & (SandboxFlags)(name)) {                                                                      \
        DCHECKF(!strchr(text, ',') && !strchr(text, '\t'),                                                   \
                "the §7.1.5 flag name `%s` contains the byte this set is delimited by, or the TAB the record " \
                "that carries it is delimited by — the section's names are lowercase ASCII letters, SPACE and " \
                "one '.', so this is that vocabulary having changed under a wire format that splits on those", \
                text);                                                                                       \
        if (q != out) *q++ = ',';                                                                            \
        memcpy(q, text, sizeof text - 1);                                                                     \
        q += sizeof text - 1;                                                                                 \
    }
    SANDBOX_FLAG_LIST(SANDBOX_FLAG_EMIT_)
#undef SANDBOX_FLAG_EMIT_
    *q = 0;
    return out;
}

bool sandbox_flags_of_serialized(const char *text, SandboxFlags *out)
{
    SandboxFlags acc = 0;
    const char *p = text;

    if (!text || !*text) return false;
    if (!strcmp(text, SANDBOX_FLAGS_SERIALIZED_NONE)) { *out = 0; return true; }
    for (;;) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        SandboxFlags found = 0;

        /* AN EMPTY MEMBER IS REFUSED RATHER THAN SKIPPED — `a,,b`, a leading separator, a trailing one. Each
           is a writer that composed the field wrongly, and skipping it would accept a set with a flag missing
           from it, which is the failure this whole field exists to prevent. */
        if (len == 0) return false;
#define SANDBOX_FLAG_MATCH_(name, text)                                                                      \
        if (!found && len == sizeof text - 1 && !memcmp(p, text, len)) found = (SandboxFlags)(name);
        SANDBOX_FLAG_LIST(SANDBOX_FLAG_MATCH_)
#undef SANDBOX_FLAG_MATCH_
        if (!found) return false;
        acc |= found;
        if (!comma) break;
        p = comma + 1;
    }
    *out = acc;
    return true;
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
