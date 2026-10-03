/* css-counter-styles-3 §5 "Extending list-style-type, counter(), and counters()"' `<counter-style>` — see
   css_counter_style.h for why the production is a component, for why the answer is a boolean, and for why the
   canonical spelling is a second entry rather than a case-folding comparison. */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "check.h"
#include "core/css/css_code_point.h"
#include "core/css/css_counter_style.h"
#include "core/css/css_defaulting.h"
#include "core/css/css_image.h"
#include "core/css/css_shorthand.h"

/* CSS Syntax §4.3.8 "Check if two code points are a valid escape", at `p`: "If the first code point is not
   U+005C REVERSE SOLIDUS (\), return false. Otherwise, if the second code point is a newline, return false.
   Otherwise, return true." EOF IS NOT A NEWLINE, so a span ending in `\` ends in a valid escape — §4.3.7
   "Consume an escaped code point" answers U+FFFD for it rather than refusing. `css_cp_at` has already folded
   CR and CRLF to one U+000A, which is why one comparison is the whole newline test. */
static bool cs_valid_escape(const char *p, const char *end)
{
    size_t n;

    if (p >= end || *p != '\\') return false;
    return css_cp_at(p + 1, end, &n) != (uint32_t)'\n';
}

/* IS THE WHOLE SPAN ONE IDENT SEQUENCE — CSS Syntax §4.3.9 "Check if three code points would start an ident
   sequence" at its start, and §4.3.12 "Consume an ident sequence" over the rest, with the requirement that the
   consumption reach the END. §4.3.12's last arm is "anything else: Reconsume the current input code point.
   Return result", so a span that STOPS being an ident sequence before its end is an ident FOLLOWED by
   something, which is two component values and not a `<custom-ident>`.
   AN ESCAPE CONSUMES ONE CODE POINT HERE AND §4.3.7 CONSUMES UP TO SIX HEX DIGITS, and the two agree about
   every whole-span answer: each hex digit and each letter §4.3.7 would swallow is itself an ident code point,
   so the walk below continues over exactly the same bytes and stops in exactly the same places. What it does
   NOT produce is §4.3.7's unescaped VALUE, which this component deliberately has no caller for — §3
   "Defining Custom Counter Styles: the @counter-style rule" makes a name case-sensitive and css-lists-3 §3.4
   "Text-based Markers: the list-style-type property" computes to the specified value, so the bytes the author
   wrote are the value. */
static bool cs_is_ident_sequence(const char *text, size_t len)
{
    const char *p = text, *end = text + len;
    size_t n;
    uint32_t c;

    if (len == 0) return false;
    c = css_cp_at(p, end, &n);
    if (c == (uint32_t)'-') {
        size_t n2;
        uint32_t c2 = css_cp_at(p + n, end, &n2);

        /* §4.3.9's hyphen arm: "If the second code point is an ident-start code point or a U+002D HYPHEN-MINUS,
           or the second and third code points are a valid escape, return true. Otherwise, return false." */
        if (!(css_cp_is_ident_start(c2) || c2 == (uint32_t)'-' || cs_valid_escape(p + n, end))) return false;
    } else if (c == (uint32_t)'\\') {
        if (!cs_valid_escape(p, end)) return false;
    } else if (!css_cp_is_ident_start(c)) {
        return false;
    }
    while (p < end) {
        c = css_cp_at(p, end, &n);
        if (css_cp_is_ident(c)) { p += n; continue; }
        if (!cs_valid_escape(p, end)) return false;
        p += n;                               /* the U+005C itself */
        if (p >= end) return true;            /* §4.3.7's EOF arm, which ends the sequence at the end of it */
        css_cp_at(p, end, &n);
        p += n;
    }
    return true;
}

/* THE NAMES css-counter-styles-3 DEFINES AS COUNTER STYLES, which is the set §3's case rule is stated over:
   "the names defined in this specification are ASCII lowercased on parse wherever they are used as counter
   styles". They are the spec's own index entries for §6 "Simple Predefined Counter Styles" and §7 "Complex
   Predefined Counter Styles" and nothing else — §3.1's `cyclic`/`numeric`/`alphabetic`/`symbolic`/`additive`/
   `fixed`/`extends` are SYSTEM values, §3.9 "Speech Synthesis: the speak-as descriptor"' `bullets`, `numbers`,
   `words` and `spell-out` are that descriptor's, and the `footnote`, `dice`, `go`, `triangle`, `trinary`,
   `box-corner`, `upper-alpha-legal` and `circled-lower-latin` that appear beside them are the sections' own
   EXAMPLE `@counter-style` rules rather than styles this specification defines.
   §8 "Additional “Ready-made” Counter Styles" IS DELIBERATELY ABSENT: it supplies `@counter-style` rules for
   authors to copy rather than defining names, so a name from it is an author's own and is case-sensitive like
   any other — which is the answer NULL gives.
   IN ASCENDING ORDER, asserted at the lookup, so the scan below is one comparison per row and a row inserted
   out of place crashes rather than silently shadowing nothing. */
static const char *const CS_PREDEFINED[] = {
    "arabic-indic", "armenian", "bengali", "cambodian", "circle", "cjk-decimal", "cjk-earthly-branch",
    "cjk-heavenly-stem", "cjk-ideographic", "decimal", "decimal-leading-zero", "devanagari", "disc",
    "disclosure-closed", "disclosure-open", "ethiopic-numeric", "georgian", "gujarati", "gurmukhi", "hebrew",
    "hiragana", "hiragana-iroha", "japanese-formal", "japanese-informal", "kannada", "katakana",
    "katakana-iroha", "khmer", "korean-hangul-formal", "korean-hanja-formal", "korean-hanja-informal", "lao",
    "lower-alpha", "lower-armenian", "lower-greek", "lower-latin", "lower-roman", "malayalam", "mongolian",
    "myanmar", "oriya", "persian", "simp-chinese-formal", "simp-chinese-informal", "square", "tamil", "telugu",
    "thai", "tibetan", "trad-chinese-formal", "trad-chinese-informal", "upper-alpha", "upper-armenian",
    "upper-latin", "upper-roman",
};

/* §4's `<symbols-type> = cyclic | numeric | alphabetic | symbolic | fixed`, in that line's own order. */
static const char *const CS_SYMBOLS_TYPE[] = { "cyclic", "numeric", "alphabetic", "symbolic", "fixed" };

/* An ASCII case-insensitive comparison of the span against a lower-case keyword — CSS Syntax §4 "Tokenization"
   makes an ident case-insensitive, and §3's own rule for the names above is stated in those terms ("not an
   ASCII case-insensitive match for none"). It is ASCII and never `tolower`, whose answer is the C locale's. */
static bool cs_word_is(const char *w, size_t n, const char *kw)
{
    size_t i;

    for (i = 0; i < n; i++) {
        char c = w[i];

        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (kw[i] == '\0' || c != kw[i]) return false;
    }
    return kw[n] == '\0';
}

const char *css_counter_style_canonical_name(const char *text, size_t len)
{
    unsigned i;

    DCHECK(text != NULL,
           "css-counter-styles-3 §3's counter-style-name spelling was asked about a NULL span — a component "
           "value is a span inside the declaration it was split out of, and an absent pointer is a caller that "
           "lost it");
#if APICLIENT_DEV
    /* THE ORDER IS ASSERTED IN ITS OWN PASS AND NOT INSIDE THE LOOKUP, because the lookup RETURNS at its match
       — so a check written there would cover the rows BEFORE the answer and nothing after it, which is a test
       whose coverage depends on its own subject. The ordering buys no lookup speed (the scan is linear either
       way); what it buys is that a DUPLICATE row is a crash rather than a row nothing can reach. */
    for (i = 1; i < sizeof(CS_PREDEFINED) / sizeof(CS_PREDEFINED[0]); i++)
        DCHECK(strcmp(CS_PREDEFINED[i - 1], CS_PREDEFINED[i]) < 0,
               "css-counter-styles-3's predefined-name table is not in ascending order, so a reader cannot tell "
               "a missing name from a misplaced one and a duplicate would never be reached");
#endif
    for (i = 0; i < sizeof(CS_PREDEFINED) / sizeof(CS_PREDEFINED[0]); i++)
        if (cs_word_is(text, len, CS_PREDEFINED[i])) return CS_PREDEFINED[i];
    return NULL;
}

/* §3's `<counter-style-name>`: "a <custom-ident> that is not an ASCII case-insensitive match for none", whose
   `<custom-ident>` exclusions are css-values-4 §4.2 "Unprefixed Author-defined Identifiers: the <custom-ident>
   type"' and are asked of core/css/css_defaulting.h rather than restated — two copies of that set have already
   disagreed about `revert-rule` once. */
static bool cs_is_name(const char *text, size_t len)
{
    char buf[128];

    if (!cs_is_ident_sequence(text, len)) return false;
    if (cs_word_is(text, len, "none")) return false;
    /* `css_custom_ident_excluded` takes a NAME and every keyword it refuses is ASCII and short, so a span too
       long to be one of them cannot be one — which is why this is a copy and not a refusal. */
    if (len >= sizeof buf) return true;
    memcpy(buf, text, len);
    buf[len] = '\0';
    return !css_custom_ident_excluded(buf);
}

/* §4 "Defining Anonymous Counter Styles: the symbols() function"'s
   `symbols() = symbols( <symbols-type>? [ <string> | <image> ]+ )`, as a validity test over the one component
   value `symbols(...)`.
   §4's ONE CONDITIONAL ARM IS ASKED: "If the system is alphabetic or numeric, there must be at least two
   <string>s or <image>s, or else the function is invalid." The other two sentences of §4 are about what the
   anonymous style MEANS — its implied prefix, suffix, range, fallback, negative, pad and speak-as, and that
   "if the system is fixed, the first symbol value is 1" — which is the marker-generating algorithm and not
   this production. */
/* NAMED RESIDUAL — THE ARGUMENT LIST IS COPIED INTO A FIXED BUFFER AND SPLIT BY A NUL-TERMINATED ENTRY, so a
   `symbols()` longer than that buffer, or carrying more arguments than the array holds, is answered FALSE. The
   code is CORRECT for what it does and NARROWER than §4: a `<counter-style>` this entry refuses is a
   declaration the cascade DROPS, which is CSS Syntax §2.2 "Error Handling"'s answer for a value outside a
   grammar and is the wrong answer for a value inside one.
     WHAT IS NOT COVERED: a `symbols()` whose argument list does not fit, which §4's `+` places no bound on —
     its own examples are four symbols and a page may write a longer cycle.
     WHAT THE NEXT DIFF BUILDS: a LENGTH-TAKING form of `css_shorthand_components`, so the arguments are split
     over the span in place and neither the copy nor the fixed array exists. It is not a wider buffer: §4 states
     no bound, so every bound here is one this engine chose.
     HOW ITS ABSENCE WOULD SHOW: a page whose marker cycles through a long list of symbols renders with
     css-lists-3 §3.4's `disc` instead, because the whole declaration was dropped — observed at
     `getComputedStyle().listStyleType`, which answers `disc` where the author wrote the function. */
static bool cs_is_symbols(const char *text, size_t len)
{
    static const char FN[] = "symbols(";
    const char *args[16];
    size_t arglen[16];
    char inner[512];
    int n, i, first = 0;
    bool two_needed = false;

    if (len <= sizeof FN || text[len - 1] != ')') return false;
    if (!cs_word_is(text, sizeof FN - 1, FN)) return false;
    /* The argument list, as its own component values. A `<string>` carries spaces and an `<image>` is a
       function of its own, so the split is CSS Syntax §4's and is asked of the one entry that owns it. */
    len -= sizeof FN;                       /* the name, its `(` and the trailing `)` */
    if (len >= sizeof inner) return false;
    memcpy(inner, text + sizeof FN - 1, len);
    inner[len] = '\0';
    n = css_shorthand_components(inner, args, arglen, (int)(sizeof args / sizeof args[0]));
    if (n < 1) return false;
    for (i = 0; i < (int)(sizeof(CS_SYMBOLS_TYPE) / sizeof(CS_SYMBOLS_TYPE[0])); i++)
        if (cs_word_is(args[0], arglen[0], CS_SYMBOLS_TYPE[i])) {
            first = 1;
            two_needed = i == 1 || i == 2;  /* §4's `numeric` and `alphabetic`, in that line's own order */
            break;
        }
    if (n - first < 1) return false;        /* the `+` */
    if (two_needed && n - first < 2) return false;
    for (i = first; i < n; i++)
        if (!css_shorthand_string(args[i], arglen[i]) && !css_image_is_image(args[i], arglen[i])) return false;
    return true;
}

bool css_counter_style_is_counter_style(const char *text, size_t len)
{
    DCHECK(text != NULL,
           "css-counter-styles-3 §5's `<counter-style>` production was asked about a NULL span — a component "
           "value is a span inside the declaration it was split out of, and an absent pointer is a caller that "
           "lost it");
    /* §5: `<counter-style> = <counter-style-name> | <symbols()>`. The FUNCTION arm is tried on the one
       character CSS Syntax makes the difference between a function and an ident, which is the same fork
       core/css/css_shorthand.c's length production makes and for the same reason. */
    if (len > 0 && memchr(text, '(', len) != NULL) return cs_is_symbols(text, len);
    return cs_is_name(text, len);
}
