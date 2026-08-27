/* CSS Fonts 4 §2.7's `font` shorthand — see css_font_shorthand.h for why it is a component of its own. */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_defaulting.h"
#include "core/css/css_font_shorthand.h"
#include "core/css/css_length.h"
#include "core/css/css_style_declaration.h"
#include "core/css/font_size_functions.h"

/* §2.7's Set Explicitly group in the canonical order of its grammar, then its Reset Implicitly group. The two
   halves are ONE array because css_shorthand.c's table row is one list and the split is an index into it. */
const char *const CSS_FONT_SHORTHAND_LONGHANDS[CSS_FONT_SHORTHAND_N] = {
    /* `[ <'font-style'> || <font-variant-css2> || <'font-weight'> || <font-width-css3> ]?` */
    "font-style", "font-variant-caps", "font-weight", "font-stretch",
    /* `<'font-size'> [ / <'line-height'> ]? <'font-family'>#` */
    "font-size", "line-height", "font-family",
    /* "Reset Implicitly ... may not be set, but are reset to their initial values" */
    "font-feature-settings", "font-kerning", "font-language-override", "font-optical-sizing",
    "font-size-adjust", "font-variant-alternates", "font-variant-east-asian", "font-variant-emoji",
    "font-variant-ligatures", "font-variant-numeric", "font-variant-position", "font-variation-settings",
};

/* The four slots of the optional PREFIX, which are the first four longhands above — so a slot index IS an
   index into that array and the two cannot come apart. */
#define FONT_SLOT_STYLE   0
#define FONT_SLOT_VARIANT 1
#define FONT_SLOT_WEIGHT  2
#define FONT_SLOT_STRETCH 3
#define FONT_SLOT_SIZE    4
#define FONT_SLOT_LH      5
#define FONT_SLOT_FAMILY  6

#define FONT_N(a) (sizeof(a) / sizeof((a)[0]))

static char *font_strdup(const char *s)
{
    char *out = strdup(s);

    CHECK(out != NULL, "cssom: OOM expanding a `font` shorthand — a dropped component would read as undeclared, "
                       "which is the property's INITIAL value and a different declaration");
    return out;
}

static char *font_dupn(const char *s, size_t n)
{
    char *out = malloc(n + 1);

    CHECK(out != NULL, "cssom: OOM copying a `font` component — a dropped one would read as undeclared");
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

/* A CSS keyword comparison over a span. CSS Syntax §4 makes an ident ASCII case-insensitive and lexbor hands a
   `__CUSTOM` declaration's value on as the author wrote it, so `SMALL-CAPS` and `small-caps` arrive here as
   different bytes and are one value. `kw` is the canonical (lower-case) spelling, which is what CSSOM
   serializes back out. */
static bool font_word_is(const char *w, size_t n, const char *kw)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (kw[i] == '\0' || (char)tolower((unsigned char)w[i]) != kw[i]) return false;
    return kw[n] == '\0';
}

static const char *font_keyword(const char *const *set, unsigned n, const char *w, size_t len)
{
    unsigned i;

    for (i = 0; i < n; i++)
        if (font_word_is(w, len, set[i])) return set[i];
    return NULL;
}

/* ---- §2.7's COMPONENT GRAMMARS ---------------------------------------------------------------------------
 *
 * Each set below is the property definition's own `Value:` line, transcribed and cited. They are here rather
 * than asked of lexbor for the reason css_font_shorthand.h gives: the `font` declaration is a `__CUSTOM` whose
 * components nothing has parsed, so there is no typed value to read an answer out of. */

/* css-fonts-4 §2.4 "Font style: the font-style property": `normal | italic | left | right |
   oblique <angle [-90deg,90deg]>?`. The `oblique` arm's optional angle is a SECOND component value and is
   consumed by the caller, which is why it is not in this set. */
static const char *const FONT_STYLE_KEYWORDS[] = { "normal", "italic", "left", "right", "oblique" };

/* css-fonts-4 §2.7's `<font-variant-css2> = normal | small-caps` — "Values for the font-variant property can
   also be included but only those supported in CSS 2.1; none of the font-variant values added in CSS Fonts
   Levels 3 or 4 can be used in the font shorthand". */
static const char *const FONT_VARIANT_CSS2[] = { "normal", "small-caps" };

/* css-fonts-4 §2.2 "Font weight: the font-weight property": `<font-weight-absolute> | bolder | lighter`, with
   `<font-weight-absolute> = [ normal | bold | <number [1,1000]> ]`. The number arm is not a keyword and is
   tested below. */
static const char *const FONT_WEIGHT_KEYWORDS[] = { "normal", "bold", "bolder", "lighter" };

/* css-fonts-4 §2.7's `<font-width-css3>`, entire and in the spec's own order — "Values for the font-width
   property can also be included but only those supported in CSS Fonts level 3, none of the font-width values
   added in this specification can be used in the font shorthand". §2.3's own `Value:` line additionally admits
   a `<percentage [0,∞]>`, and that is exactly the level-4 addition this production excludes. */
static const char *const FONT_WIDTH_CSS3[] = {
    "normal", "ultra-condensed", "extra-condensed", "condensed", "semi-condensed",
    "semi-expanded", "expanded", "extra-expanded", "ultra-expanded",
};

/* css-fonts-4 §2.5 "Font size: the font-size property": `<absolute-size> | <relative-size> |
   <length-percentage [0,∞]> | math`. §2.5's own listing gives `<relative-size>` as `[ larger | smaller ]`.
   `<absolute-size>`'s EIGHT KEYWORDS ARE NOT LISTED HERE, deliberately: §2.5 defines each of them as "an entry
   in a table of font sizes computed and kept by the user agent" and §2.5.1 is that table, so the names and the
   scaling factors are ONE list — core/css/font_size_functions.h's — and the copy this file used to hold was a
   second answer to the question "is `x-large` one of them" sitting beside the only one that also knows what it
   computes to. */
static const char *const FONT_RELATIVE_SIZE[] = { "larger", "smaller" };

/* css-fonts-4 §2.1.3 "Syntax of <system-font-family-name>": `caption | icon | menu | message-box |
   small-caption | status-bar`. */
static const char *const FONT_SYSTEM_FAMILY[] = {
    "caption", "icon", "menu", "message-box", "small-caption", "status-bar",
};

/* css-values-4 §7.1 "Angle Units: the `<angle>` type and deg, grad, rad, turn units" states the four
   `<angle>` units. `oblique`'s argument is the only place §2.7's grammar
   admits one, and telling an angle from a length is what stops `font: oblique 12px serif` reading the size as
   the slant. */
static const char *const FONT_ANGLE_UNITS[] = { "deg", "grad", "rad", "turn" };

/* A `<number>` — css-values-4 §5.3 "Real Numbers: the `<number>` type"'s production, answered by whether the whole span is one. `*out` receives the
   value so the range checks the two numeric grammars carry (`<number [1,1000]>` for a weight,
   `<number [0,∞]>` for a line-height) are made by their own callers rather than here. */
static bool font_number(const char *w, size_t n, double *out)
{
    char buf[64];
    char *end = NULL;

    if (n == 0 || n >= sizeof buf) return false;
    memcpy(buf, w, n);
    buf[n] = '\0';
    *out = strtod(buf, &end);
    return end != NULL && *end == '\0';
}

/* A `<dimension>` whose unit is one of `set` — the shape both the angle test and the percentage-free half of
   the `<length-percentage>` test need. */
static bool font_dimension_in(const char *const *set, unsigned n, const char *w, size_t len, double *out)
{
    char buf[64];
    char *end = NULL;
    unsigned i;

    if (len == 0 || len >= sizeof buf) return false;
    memcpy(buf, w, len);
    buf[len] = '\0';
    *out = strtod(buf, &end);
    if (end == NULL || end == buf) return false;
    for (i = 0; i < n; i++) {
        size_t k = strlen(set[i]), j;

        if (strlen(end) != k) continue;
        for (j = 0; j < k; j++)
            if (tolower((unsigned char)end[j]) != set[i][j]) break;
        if (j == k) return true;
    }
    return false;
}

/* css-values-4 §6's `<length>` and §5.5's `<percentage>`, as the one production §2.5 and css-inline-3 §5.1
   both name — `<length-percentage [0,∞]>`. The range is part of the production and is CHECKED rather than
   clamped: §2.5 says "Negative lengths are invalid" and "Negative percentages are invalid" in its own words,
   so a negative one is a DROPPED declaration.
   THE `<length>` HALF IS ASKED OF core/css/css_length.h, which owns css-values §6's unit table entire —
   including the font-relative units it cannot yet absolutize, which are a MISSING COMPONENT and not a syntax
   error, so `font: 1.2em serif` is a valid declaration whose computed value crashes by name rather than an
   invalid one this grammar drops. */
static bool font_length_percentage_nonneg(const char *w, size_t len)
{
    char *probe;
    bool ok;

    if (len > 0 && w[0] == '-') return false;
    if (len > 1 && w[len - 1] == '%') {
        double n;

        return font_number(w, len - 1, &n);
    }
    probe = font_dupn(w, len);
    /* §2.5's own production is `<length-percentage [0,∞]>`, so the entry asked is the one that answers BOTH
       arms — which matters for a math function and for nothing else, because a literal `50%` was answered by
       the branch above. `font-size: calc(50% + 2px)` is a valid declaration (css-values-4 §10.9.1 makes the
       percentage resolve against the parent's computed size, exactly as a bare `50%` does), and asking the
       `<length>`-only entry would drop it. */
    ok = css_length_is_length_percentage(probe);
    /* §6's unitless zero is a `<length>` and a bare NON-zero is not, and `css_length_is_length` answers TRUE
       for both because its own caller asserts the difference. A `<'font-size'>` grammar has no `<number>` arm
       at all, so the bare number is refused here. */
    if (ok) {
        double n;

        if (font_number(w, len, &n)) ok = (n == 0.0);
    }
    free(probe);
    return ok;
}

/* §2.5's `<'font-size'>`. */
static bool font_size_valid(const char *w, size_t len)
{
    if (css_absolute_size_keyword(w, len)) return true;
    if (font_keyword(FONT_RELATIVE_SIZE, FONT_N(FONT_RELATIVE_SIZE), w, len)) return true;
    if (font_word_is(w, len, "math")) return true;
    return font_length_percentage_nonneg(w, len);
}

/* css-inline-3 §5.1 "Line Spacing: the line-height property": `normal | <number [0,∞]> |
   <length-percentage [0,∞]>`. */
static bool font_line_height_valid(const char *w, size_t len)
{
    double n;

    if (font_word_is(w, len, "normal")) return true;
    if (font_number(w, len, &n)) return n >= 0.0;
    return font_length_percentage_nonneg(w, len);
}

/* css-fonts-4 §2.1.1 "Syntax of <font-family-name>": `<font-family-name> = <string> | <custom-ident>+`, over
 * §2.1's `<'font-family'> = [ <family-name> | <generic-family> ]#`. A `<generic-family>` is an ident, so the
 * `<custom-ident>+` arm covers it and the two need no separate test.
 *
 * §2.1.1's OWN THREE INVALID EXAMPLES ARE THE TEST: `Red/Black`, `"Lucida" Grande` and `Ahem!` are each named
 * there as an invalid declaration. So a comma-separated part is either ONE `<string>` and nothing else, or a
 * sequence of idents each of which starts with an ident-start code point — CSS Syntax §4.2's "a letter, a
 * non-ASCII code point, or U+005F LOW LINE", plus a leading hyphen or escape, which §4.3.9 admits — and
 * carries no delimiter. That refuses all three, and admits `Palatino`, `new century schoolbook` unquoted and
 * `"new century schoolbook"` quoted.
 *
 * IT IS NOT A CHARACTER-EXACT `<custom-ident>` PARSE, and the gap is named rather than papered: an ESCAPE
 * (`\31 0th`) is admitted by its backslash without its hex digits being read. That is the direction §2.1.1's
 * own note points ("most punctuation characters and digits at the start of each token must be escaped"), so
 * the looseness admits a valid spelling rather than a value the grammar rejects. */
static bool font_family_ident_start(unsigned char c)
{
    return isalpha(c) || c == '_' || c == '-' || c == '\\' || c >= 0x80;
}

static bool font_family_part_valid(const char *s, size_t n)
{
    size_t i = 0;

    while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
    while (i < n && isspace((unsigned char)s[i])) i++;
    if (i >= n) return false;   /* an empty list item: `font-family: a,,b` is not `#` */
    if (s[i] == '"' || s[i] == '\'') {
        char q = s[i];

        /* ONE `<string>` AND NOTHING ELSE — §2.1.1's `"Lucida" Grande` is its own invalid example. */
        for (i++; i < n; i++)
            if (s[i] == q) break;
        return i == n - 1;
    }
    while (i < n) {
        size_t start = i;

        if (!font_family_ident_start((unsigned char)s[i])) return false;
        while (i < n && !isspace((unsigned char)s[i])) {
            /* §2.1.1's `Red/Black` and `Ahem!`: a delimiter inside an unquoted family name is invalid. */
            if (strchr("/()[]{}!\"'`;:@#$%^&*=+<>?|~,\\", s[i]) != NULL && s[i] != '\\') return false;
            i++;
        }
        if (i == start) return false;
        while (i < n && isspace((unsigned char)s[i])) i++;
    }
    return true;
}

/* §2.1's `#` multiplier over the whole trailing text. A comma inside a `<string>` is not a list separator. */
static bool font_family_valid(const char *s, size_t n)
{
    size_t i = 0, start = 0;
    char q = '\0';

    if (n == 0) return false;
    for (i = 0; i < n; i++) {
        if (q != '\0') { if (s[i] == q) q = '\0'; continue; }
        if (s[i] == '"' || s[i] == '\'') { q = s[i]; continue; }
        if (s[i] != ',') continue;
        if (!font_family_part_valid(s + start, i - start)) return false;
        start = i + 1;
    }
    if (q != '\0') return false;   /* an unterminated string */
    return font_family_part_valid(s + start, n - start);
}

/* ---- §2.7's SEQUENCE ------------------------------------------------------------------------------------- */

/* THE COMPONENT VALUES of the declaration, as spans into it. `/` is NOT split here — it binds the size to the
 * line-height and is separated below — but a component is otherwise whitespace-delimited, with parentheses
 * and strings counted so a `<family-name>` or a function carrying spaces stays one component.
 *
 * IT STOPS AT `max` RATHER THAN FAILING, AND `max` IS DERIVED FROM §2.7's GRAMMAR RATHER THAN PICKED. That
 * distinction is the whole reason this differs from css_shorthand.c's splitter, which refuses a value carrying
 * more components than its grammar admits: `margin`'s `{1,4}` really is a maximum, and `<'font-family'>#` is
 * not — its `#` has no upper bound, so a fixed buffer that DROPPED a long family list would be a CAP wearing a
 * parse error, and a bundle's own `font: 14px A, B, C, …` stack is exactly the value that hits it.
 * SO ONLY THE BOUNDED PREFIX IS TOKENIZED. Everything §2.7 puts before `<'font-family'>#` is at most eight
 * components — four `||` terms, one of which (§2.4's `oblique <angle>`) may be two; then the size term, which
 * is at most three when the `/` is written detached on both sides (`12pt / 14pt`) — and the family is then the
 * REST OF THE DECLARATION, taken verbatim because a family list's own commas and spaces must not be split. The
 * ninth slot is the one the family starts in, and is what lets "no words left" mean "no family" rather than
 * "the buffer ran out"; the tenth is asserted-unreachable slack so that reading is provable rather than tight. */
#define FONT_MAX_WORDS 10

typedef struct { const char *s; size_t n; } FontWord;

static int font_words(const char *v, FontWord *w, int max)
{
    int n = 0;

    while (*v && n < max) {
        const char *s;
        int depth = 0;
        char q = '\0';

        while (*v && isspace((unsigned char)*v)) v++;
        if (!*v) break;
        s = v;
        while (*v && (q != '\0' || depth > 0 || !isspace((unsigned char)*v))) {
            if (q != '\0') { if (*v == q) q = '\0'; }
            else if (*v == '"' || *v == '\'') q = *v;
            else if (*v == '(') depth++;
            else if (*v == ')' && depth > 0) depth--;
            v++;
        }
        w[n].s = s;
        w[n].n = (size_t)(v - s);
        n++;
    }
    return n;
}

/* WHICH SLOT OF §2.7's OPTIONAL PREFIX this component fills, or -1 when it fills none — which is what ends the
 * prefix and begins `<'font-size'>`.
 *
 * `normal` IS AMBIGUOUS BY CONSTRUCTION AND THAT IS WHY IT NEEDS NO DECISION. It is a value of all four
 * prefix terms, and all four state `Initial: normal` (§2.4, §2.7's `<font-variant-css2>` over §6.5's
 * `font-variant-caps`, §2.2, §2.3) — and §2.7 resets every settable sub-property to its initial value before
 * setting any. So whichever slot it is read into holds the value that slot would hold anyway, and the only
 * thing the choice can change is how many MORE `normal`s the `||` still has room for. It fills the first
 * unfilled slot for exactly that reason.
 *
 * A BARE `<number>` IS A WEIGHT AND NEVER A SIZE, which is not a heuristic: §2.5's `<'font-size'>` has no
 * `<number>` arm at all (its numeric arm is `<length-percentage>`, and css-values §6 permits the unit to be
 * omitted only for zero), while §2.2's `<font-weight-absolute>` is `normal | bold | <number [1,1000]>`. So the
 * two productions are disjoint over an unsuffixed number and the split is the grammar's, not a guess. */
static int font_prefix_slot(const FontWord *w, int n, int i, const bool *filled, int *consumed)
{
    const char *word = w[i].s;
    size_t len = w[i].n;
    double num;
    int slot;

    *consumed = 1;
    if (font_word_is(word, len, "normal")) {
        for (slot = FONT_SLOT_STYLE; slot <= FONT_SLOT_STRETCH; slot++)
            if (!filled[slot]) return slot;
        return -1;   /* a fifth `normal`: the `||` admits each term at most once, so the value is invalid */
    }
    /* §2.7's `<font-width-css3>` is asked first because its nine keywords are disjoint from every other term's
       and from `<'font-size'>`, so it can never be the one that has to be disambiguated. */
    if (!filled[FONT_SLOT_STRETCH] &&
        font_keyword(FONT_WIDTH_CSS3, FONT_N(FONT_WIDTH_CSS3), word, len))
        return FONT_SLOT_STRETCH;
    if (!filled[FONT_SLOT_VARIANT] &&
        font_keyword(FONT_VARIANT_CSS2, FONT_N(FONT_VARIANT_CSS2), word, len))
        return FONT_SLOT_VARIANT;
    if (!filled[FONT_SLOT_STYLE] &&
        font_keyword(FONT_STYLE_KEYWORDS, FONT_N(FONT_STYLE_KEYWORDS), word, len)) {
        /* §2.4's `oblique <angle [-90deg,90deg]>?` — the angle is a second component value and belongs to this
           one. §2.7's own example pins that it must follow immediately: "Note that the 25deg in this rule must
           be immediately following the `oblique` keyword". */
        if (font_word_is(word, len, "oblique") && i + 1 < n &&
            font_dimension_in(FONT_ANGLE_UNITS, FONT_N(FONT_ANGLE_UNITS), w[i + 1].s, w[i + 1].n, &num))
            *consumed = 2;
        return FONT_SLOT_STYLE;
    }
    if (!filled[FONT_SLOT_WEIGHT]) {
        if (font_keyword(FONT_WEIGHT_KEYWORDS, FONT_N(FONT_WEIGHT_KEYWORDS), word, len))
            return FONT_SLOT_WEIGHT;
        if (font_number(word, len, &num) && num >= 1.0 && num <= 1000.0)
            return FONT_SLOT_WEIGHT;
    }
    return -1;
}

/* §2.7's `[ / <'line-height'> ]?`, whose slash may be written attached to either side or to neither —
   `12pt/14pt`, `12pt /14pt`, `12pt/ 14pt` and `12pt / 14pt` are one declaration. So the size component is the
   text up to the first `/` outside a string, and the line-height is what follows it, wherever the whitespace
   fell. Returns the number of WORDS the size-and-line-height term consumed, or -1 for a value outside the
   grammar; `*plh` is left NULL when the optional term is absent. */
static int font_size_term(const FontWord *w, int n, int i, FontWord *psize, FontWord *plh, bool *has_lh)
{
    const char *slash = NULL;
    size_t k;
    char q = '\0';

    for (k = 0; k < w[i].n; k++) {
        if (q != '\0') { if (w[i].s[k] == q) q = '\0'; continue; }
        if (w[i].s[k] == '"' || w[i].s[k] == '\'') { q = w[i].s[k]; continue; }
        if (w[i].s[k] == '/') { slash = w[i].s + k; break; }
    }
    *has_lh = false;
    psize->s = w[i].s;
    psize->n = slash ? (size_t)(slash - w[i].s) : w[i].n;
    if (psize->n == 0) return -1;   /* a `/` with no size before it */
    if (slash != NULL) {
        size_t rest = w[i].n - psize->n - 1;

        *has_lh = true;
        if (rest > 0) { plh->s = slash + 1; plh->n = rest; return 1; }
        if (i + 1 >= n) return -1;   /* `12px/` with nothing after it */
        plh->s = w[i + 1].s;
        plh->n = w[i + 1].n;
        return 2;
    }
    /* The slash opens the NEXT component instead. */
    if (i + 1 < n && w[i + 1].n > 0 && w[i + 1].s[0] == '/') {
        *has_lh = true;
        if (w[i + 1].n > 1) { plh->s = w[i + 1].s + 1; plh->n = w[i + 1].n - 1; return 2; }
        if (i + 2 >= n) return -1;
        plh->s = w[i + 2].s;
        plh->n = w[i + 2].n;
        return 3;
    }
    return 1;
}

/* §2.7's grammar, run over `value`. `out` receives the seven Set Explicitly values, each OWNED, or the whole
   parse fails and `out` is untouched — CSS Syntax drops an INVALID declaration whole, so a `font` that fails
   one component sets none of the nineteen. */
static bool font_parse(const char *value, char **out)
{
    FontWord w[FONT_MAX_WORDS], size, lh;
    bool filled[FONT_SLOT_STRETCH + 1] = { false, false, false, false };
    const char *canon[FONT_SLOT_STRETCH + 1] = { NULL, NULL, NULL, NULL };
    FontWord raw[FONT_SLOT_STRETCH + 1];
    bool has_lh = false;
    int n, i = 0, slot, consumed, taken, k;
    const char *family;
    size_t family_n;

    n = font_words(value, w, FONT_MAX_WORDS);
    if (n < 1) return false;
    /* §2.1.3's `<system-font-family-name>` arm. §2.7: "System fonts can only be set as a whole; that is, the
       font family, size, weight, style, etc. are all set at the same time", out of "the operating system's
       available user preferences". */
    if (n == 1 && font_keyword(FONT_SYSTEM_FAMILY, FONT_N(FONT_SYSTEM_FAMILY), w[0].s, w[0].n))
        DFAIL("a `font` declaration named one of css-fonts-4 §2.1.3's SIX SYSTEM FONTS (`caption`, `icon`, "
              "`menu`, `message-box`, `small-caption`, `status-bar`). §2.7 makes that the shorthand's second "
              "whole arm and defines every sub-property's value by the OPERATING SYSTEM's user preferences — "
              "a family, a size, a weight and a style this engine has no record of, which is why the answer "
              "cannot be the initial values (§2.7 says only that properties the OS does not expose take those, "
              "and the ones it always exposes are exactly the ones a page reads). This is NOT css-values "
              "§Headless's missing device: a system font record is a set of UA-MODELLED values in the same "
              "sense core/frame/viewport.h models the viewport, so BUILD one — six rows of "
              "{family, size, weight, style} picked by the same test viewport.h applies, each row a PICKED "
              "environment fact because `getComputedStyle(el).fontSize` after `font: menu` is a question with "
              "more than one answer and a bundle sizing a control against the platform reads it");
    for (k = 0; k <= FONT_SLOT_STRETCH; k++) { raw[k].s = NULL; raw[k].n = 0; }
    while (i < n && (slot = font_prefix_slot(w, n, i, filled, &consumed)) >= 0) {
        DCHECK(!filled[slot], "§2.7's `||` filled one prefix term twice — each term is admitted at most once "
                              "and font_prefix_slot answers only for an UNFILLED slot, so the two have come "
                              "apart");
        filled[slot] = true;
        /* A KEYWORD is canonicalized to its lower-case spelling, which is what CSSOM §6.7.1 serializes back;
           a `<number>` weight and an `oblique <angle>` are copied verbatim, the way every other numeric
           component value in this engine's shorthands is. */
        canon[slot] = (slot == FONT_SLOT_STRETCH)
                          ? font_keyword(FONT_WIDTH_CSS3, FONT_N(FONT_WIDTH_CSS3), w[i].s, w[i].n)
                          : (slot == FONT_SLOT_VARIANT)
                                ? font_keyword(FONT_VARIANT_CSS2, FONT_N(FONT_VARIANT_CSS2), w[i].s, w[i].n)
                                : (slot == FONT_SLOT_STYLE && consumed == 1)
                                      ? font_keyword(FONT_STYLE_KEYWORDS, FONT_N(FONT_STYLE_KEYWORDS),
                                                     w[i].s, w[i].n)
                                      : font_keyword(FONT_WEIGHT_KEYWORDS, FONT_N(FONT_WEIGHT_KEYWORDS),
                                                     w[i].s, w[i].n);
        raw[slot].s = w[i].s;
        raw[slot].n = (consumed == 2) ? (size_t)(w[i + 1].s + w[i + 1].n - w[i].s) : w[i].n;
        i += consumed;
    }
    /* `<'font-size'>` is REQUIRED by the grammar, and so is the family after it. */
    if (i >= n) return false;
    taken = font_size_term(w, n, i, &size, &lh, &has_lh);
    if (taken < 0) return false;
    if (!font_size_valid(size.s, size.n)) return false;
    if (has_lh && !font_line_height_valid(lh.s, lh.n)) return false;
    i += taken;
    DCHECK(i <= FONT_MAX_WORDS - 2,
           "§2.7's bounded prefix consumed more component values than its grammar has — four `||` terms with "
           "at most one two-word `oblique <angle>`, then a size term of at most three when the `/` is detached "
           "on both sides, is eight, and FONT_MAX_WORDS is sized from exactly that. A ninth would mean the "
           "family's first word was never tokenized, and `i >= n` below would then read `the buffer ran out` "
           "as `the declaration has no family`");
    if (i >= n) return false;
    /* `<'font-family'>#` is the LAST term and consumes the rest of the declaration verbatim — a family list
       carries commas and spaces that no component split may take apart. */
    family = w[i].s;
    family_n = strlen(family);
    while (family_n > 0 && isspace((unsigned char)family[family_n - 1])) family_n--;
    if (!font_family_valid(family, family_n)) return false;

    for (k = 0; k <= FONT_SLOT_STRETCH; k++) {
        if (!filled[k]) { out[k] = font_strdup("normal"); continue; }
        out[k] = canon[k] ? font_strdup(canon[k]) : font_dupn(raw[k].s, raw[k].n);
    }
    out[FONT_SLOT_SIZE] = font_dupn(size.s, size.n);
    out[FONT_SLOT_LH] = has_lh ? font_dupn(lh.s, lh.n) : font_strdup("normal");
    out[FONT_SLOT_FAMILY] = font_dupn(family, family_n);
    return true;
}

/* §7.1's INITIAL VALUE of a reset-only sub-property, out of the ONE place this engine states one. It is asked
   rather than tabulated here, because a second table of thirteen initial values is the second answer that is
   always subtly wrong — css_style_declaration.c's own comment on CSSD_INITIAL_UNREGISTERED says why the fact
   belongs there whether or not lexbor's registry carries the property. */
static char *font_reset_initial(const char *longhand)
{
    char *v = cssom_initial_value(longhand);

    DCHECK(v != NULL,
           "a RESET-ONLY sub-property of css-fonts-4 §2.7's `font` has no INITIAL value in this engine. §2.7 "
           "resets every one of them before setting anything, so answering NULL here makes the whole `font` "
           "declaration invalid and lets the declaration it overrode survive. Every one of them states an "
           "`Initial:` line of its own — add the row to CSSD_INITIAL_UNREGISTERED in "
           "core/css/css_style_declaration.c, which is where a property lexbor's registry does not carry "
           "states one");
    return v;
}

char *css_font_shorthand_component(const char *value, const char *longhand)
{
    char *set[CSS_FONT_SHORTHAND_SET_N];
    unsigned at;
    int k;

    DCHECK(value != NULL && longhand != NULL,
           "§2.7's expansion was asked about a NULL value or property name — a declaration carries both by the "
           "time lexbor has serialized it");
    for (at = 0; at < CSS_FONT_SHORTHAND_N; at++)
        if (strcmp(CSS_FONT_SHORTHAND_LONGHANDS[at], longhand) == 0) break;
    if (at == CSS_FONT_SHORTHAND_N) return NULL;   /* a property `font` does not set or reset */
    /* CSS Cascade §7.3: "if a shorthand is specified as one of the CSS-wide keywords, it sets all of its
       sub-properties to that keyword" — the ENTIRE value, so it precedes §2.7's own grammar and is handed on
       for §7's defaulting step to resolve. It reaches the RESET-ONLY group too: `font: inherit` is not a
       declaration that resets them, it is one that sets all nineteen to `inherit`. */
    if (css_wide_keyword(value)) return font_strdup(value);
    if (at >= CSS_FONT_SHORTHAND_SET_N) {
        /* A reset-only sub-property still needs the declaration to be VALID before it is reset — an invalid
           `font` sets nothing at all. */
        if (!font_parse(value, set)) return NULL;
        for (k = 0; k < CSS_FONT_SHORTHAND_SET_N; k++) free(set[k]);
        return font_reset_initial(longhand);
    }
    if (!font_parse(value, set)) return NULL;
    for (k = 0; k < CSS_FONT_SHORTHAND_SET_N; k++)
        if ((unsigned)k != at) free(set[k]);
    return set[at];
}

char *css_font_shorthand_value(const char *const *values)
{
    const char *parts[8];
    unsigned n = 0, i;
    char *size_lh = NULL, *out;
    size_t total = 1, at = 0;

    DCHECK(values != NULL, "§6.7.2's serialize-a-CSS-value for `font` was asked for with no values");
    for (i = 0; i < CSS_FONT_SHORTHAND_N; i++)
        DCHECK(values[i] != NULL,
               "§6.7.2's serialize-a-CSS-value for `font` was handed a list with a longhand missing. §6.6's "
               "loop asks whether ALL of a shorthand's longhands are present BEFORE it builds the list, so a "
               "hole here is that test having been skipped");
    /* "The shorthand cannot exactly represent the values of all the properties in list" — §2.7's three ways,
       each one of its own sentences. A reset-only longhand that does not hold its initial value first: a
       `font` declaration would RESET it, so a block in which it is something else is not one any `font`
       produced. */
    for (i = CSS_FONT_SHORTHAND_SET_N; i < CSS_FONT_SHORTHAND_N; i++) {
        char *init = font_reset_initial(CSS_FONT_SHORTHAND_LONGHANDS[i]);
        bool same = init != NULL && strcmp(init, values[i]) == 0;

        free(init);
        if (!same) return NULL;
    }
    /* "None of the font-variant values added in CSS Fonts Levels 3 or 4 can be used in the font shorthand",
       and the same sentence one property along for the widths. */
    if (!font_keyword(FONT_VARIANT_CSS2, FONT_N(FONT_VARIANT_CSS2),
                      values[FONT_SLOT_VARIANT], strlen(values[FONT_SLOT_VARIANT])))
        return NULL;
    if (!font_keyword(FONT_WIDTH_CSS3, FONT_N(FONT_WIDTH_CSS3),
                      values[FONT_SLOT_STRETCH], strlen(values[FONT_SLOT_STRETCH])))
        return NULL;
    if (values[FONT_SLOT_SIZE][0] == '\0' || values[FONT_SLOT_FAMILY][0] == '\0') return NULL;
    /* §6.7.2's "if component values can be omitted ... without changing the meaning of the value, omit them" —
       and for §2.7 that is exactly the terms holding their own initial value, because the shorthand resets
       every omitted one to it. */
    for (i = FONT_SLOT_STYLE; i <= FONT_SLOT_STRETCH; i++)
        if (strcmp(values[i], "normal") != 0) parts[n++] = values[i];
    if (strcmp(values[FONT_SLOT_LH], "normal") == 0) {
        parts[n++] = values[FONT_SLOT_SIZE];
    } else {
        size_t len = strlen(values[FONT_SLOT_SIZE]) + strlen(values[FONT_SLOT_LH]) + 2;

        size_lh = malloc(len);
        CHECK(size_lh != NULL, "cssom: OOM serializing a `font` value — a dropped one reads as the empty "
                               "string, which is how §6.6 is told the shorthand could not represent the "
                               "declarations");
        snprintf(size_lh, len, "%s/%s", values[FONT_SLOT_SIZE], values[FONT_SLOT_LH]);
        parts[n++] = size_lh;
    }
    parts[n++] = values[FONT_SLOT_FAMILY];
    DCHECK(n <= FONT_N(parts),
           "§2.7's serialization wrote more component values than its grammar has terms — four prefix terms, "
           "the size-and-line-height term and the family are six, and the array is sized for them");
    for (i = 0; i < n; i++) total += strlen(parts[i]) + 1;
    out = malloc(total);
    CHECK(out != NULL, "cssom: OOM joining a `font` value");
    for (i = 0; i < n; i++) {
        size_t l = strlen(parts[i]);

        if (i) out[at++] = ' ';
        memcpy(out + at, parts[i], l);
        at += l;
    }
    out[at] = '\0';
    free(size_lh);
    return out;
}
