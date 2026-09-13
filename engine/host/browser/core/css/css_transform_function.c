/* CSS Transforms 1 §7 "The Transform Functions". See css_transform_function.h for why an authoring mistake is
 * a dropped declaration here and a missing level is a crash, and for which edition's numbers these are. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_dimension.h"
#include "core/css/css_math.h"
#include "core/css/css_transform_function.h"
#include "core/json_buf.h"

/* WHICH PRODUCTION A FUNCTION'S ARGUMENTS ARE, which §7.1 "2D Transform Functions" fixes per function: every
   argument of one function is the same production, and no function mixes two. That is what lets the three
   arrays on `CssTransformFunction` be alternatives rather than a union a reader has to decode. */
typedef enum {
    CSS_TF_ARG_NUMBER = 0,              /* `<number>` */
    CSS_TF_ARG_LENGTH_PERCENTAGE,       /* `<length-percentage>` */
    CSS_TF_ARG_ANGLE                    /* `[ <angle> | <zero> ]` */
} CssTfArgType;

/* §7.1's ELEVEN GRAMMARS, TRANSCRIBED. `min`/`max` are the argument counts the production admits — one of them
   for a `#{6}` or a single required argument, two where §7.1 writes an optional second. */
static const struct {
    const char        *name;
    CssTransformFnKind kind;
    CssTfArgType       arg;
    unsigned           min, max;
} CSS_TF_GRAMMAR[] = {
    /* matrix() = matrix( <number>#{6} ) */
    { "matrix",     CSS_TF_MATRIX,      CSS_TF_ARG_NUMBER,             6, 6 },
    /* translate() = translate( <length-percentage> , <length-percentage>? ) */
    { "translate",  CSS_TF_TRANSLATE,   CSS_TF_ARG_LENGTH_PERCENTAGE,  1, 2 },
    /* translateX() = translateX( <length-percentage> ) */
    { "translateX", CSS_TF_TRANSLATE_X, CSS_TF_ARG_LENGTH_PERCENTAGE,  1, 1 },
    /* translateY() = translateY( <length-percentage> ) */
    { "translateY", CSS_TF_TRANSLATE_Y, CSS_TF_ARG_LENGTH_PERCENTAGE,  1, 1 },
    /* scale() = scale( <number> , <number>? ) */
    { "scale",      CSS_TF_SCALE,       CSS_TF_ARG_NUMBER,             1, 2 },
    /* scaleX() = scaleX( <number> ) */
    { "scaleX",     CSS_TF_SCALE_X,     CSS_TF_ARG_NUMBER,             1, 1 },
    /* scaleY() = scaleY( <number> ) */
    { "scaleY",     CSS_TF_SCALE_Y,     CSS_TF_ARG_NUMBER,             1, 1 },
    /* rotate() = rotate( [ <angle> | <zero> ] ) */
    { "rotate",     CSS_TF_ROTATE,      CSS_TF_ARG_ANGLE,              1, 1 },
    /* skew() = skew( [ <angle> | <zero> ] , [ <angle> | <zero> ]? ) */
    { "skew",       CSS_TF_SKEW,        CSS_TF_ARG_ANGLE,              1, 2 },
    /* skewX() = skewX( [ <angle> | <zero> ] ) */
    { "skewX",      CSS_TF_SKEW_X,      CSS_TF_ARG_ANGLE,              1, 1 },
    /* skewY() = skewY( [ <angle> | <zero> ] ) */
    { "skewY",      CSS_TF_SKEW_Y,      CSS_TF_ARG_ANGLE,              1, 1 }
};

/* css-transforms-2 §12.2 "3D Transform Functions", BY NAME, and the list is what separates a gap from an
   authoring mistake rather than decoration on the crash. A function nothing enumerates is invalid input and is
   dropped; one a STANDARD enumerates is a capability this engine has not built, and answering `none` for it
   would report a WRONG transform in place of an absent one. Transcribed from that section's own grammars:
   matrix3d(), translate3d(), translateZ(), scale3d(), scaleZ(), rotate3d(), rotateX(), rotateY(), rotateZ()
   and perspective(). */
static const char *const CSS_TF_LEVEL2[] = {
    "matrix3d", "translate3d", "translateZ", "scale3d", "scaleZ",
    "rotate3d", "rotateX", "rotateY", "rotateZ", "perspective"
};

/* CSS Syntax §4.2 "Definitions"' WHITESPACE — "A newline, U+0009 CHARACTER TABULATION, or U+0020 SPACE" —
   with §3.3 "Preprocessing the input stream"'s two newline spellings folded in, because the value reaching
   this component is a declaration's raw text rather than a filtered stream. `isspace` is NOT this set: it also
   answers true for U+000B LINE TABULATION, which CSS tokenizes as an ident-adjacent code point and not as a
   separator, so a value split on it would parse text no browser separates. */
static bool css_tf_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static const char *css_tf_skip_ws(const char *p, const char *end)
{
    while (p < end && css_tf_ws(*p)) p++;
    return p;
}

/* The span with CSS Syntax §4.2 whitespace removed from both ends. Every argument reaches its own reader this
   way, which is what makes `translate( 1px , 2px )` and `translate(1px,2px)` one value. */
static void css_tf_trim(const char **s, const char **e)
{
    while (*s < *e && css_tf_ws(**s)) (*s)++;
    while (*e > *s && css_tf_ws((*e)[-1])) (*e)--;
}

/* ASCII case-insensitive equality against a table entry — CSS Syntax makes a function name an ident sequence
   and CSS compares those ASCII case-insensitively, so `translateX(`, `TRANSLATEX(` and `translatex(` are one
   function.
   BOTH SIDES ARE FOLDED, which is what lets the tables above hold §7.1's OWN spelling. The alternative was a
   lower-case matching column beside a canonical printing one, and a name written twice is the copy that
   drifts: one table that both the matcher and the serializer read cannot disagree with itself about which
   function this is. */
static bool css_tf_name_is(const char *s, size_t len, const char *name)
{
    size_t i;

    if (strlen(name) != len) return false;
    for (i = 0; i < len; i++)
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)name[i])) return false;
    return true;
}

/* CSS Syntax §4.3.13 "Consume a number" over a WHOLE span: sign, integer digits, optional fraction, optional
   exponent, and nothing left over. §4.3.10 "Check if three code points would start a number" is folded in as
   the requirement that at least one digit appear in the integer or the fraction, which is what keeps `.` and
   `+` and `e5` from parsing.
   THE SHAPE IS DECIDED HERE AND THE VALUE IS CONVERTED BY THE LIBRARY, deliberately: `strtod` accepts `0x10`,
   `inf` and `nan`, none of which CSS has, so validating with CSS's own grammar first is what stops a
   hexadecimal scale factor from becoming a number this engine believes and a browser drops. */
static bool css_tf_number(const char *s, const char *e, double *out)
{
    const char *p = s;
    bool digits = false;
    char *stop = NULL;
    char *copy;
    double v;

    if (p < e && (*p == '+' || *p == '-')) p++;
    while (p < e && isdigit((unsigned char)*p)) { p++; digits = true; }
    if (p < e && *p == '.') {
        p++;
        while (p < e && isdigit((unsigned char)*p)) { p++; digits = true; }
    }
    if (!digits) return false;
    if (p < e && (*p == 'e' || *p == 'E')) {
        const char *q = p + 1;
        bool exp_digits = false;

        if (q < e && (*q == '+' || *q == '-')) q++;
        while (q < e && isdigit((unsigned char)*q)) { q++; exp_digits = true; }
        /* An `e` with no digits after it is not part of the number token at all, so the span has a tail and
           this is not a `<number>`. Leaving `p` where it was makes that the leftover test below. */
        if (exp_digits) p = q;
    }
    if (p != e) return false;
    copy = malloc((size_t)(e - s) + 1);
    CHECK(copy != NULL, "css-transforms-1 §7: OOM copying a <number> out of a transform function");
    memcpy(copy, s, (size_t)(e - s));
    copy[e - s] = '\0';
    v = strtod(copy, &stop);
    DCHECK(stop != NULL && *stop == '\0',
           "a span this file validated against CSS Syntax §4.3.13 \"Consume a number\" was not wholly consumed "
           "by strtod — the two grammars are CSS's and C's and the validation above exists precisely so the "
           "library never decides, so a disagreement is that validation and this conversion having come apart");
    free(copy);
    *out = v;
    return true;
}

/* A MATH FUNCTION IN A POSITION THIS FILE CANNOT EVALUATE, TOLD APART FROM AN AUTHORING MISTAKE BEFORE EITHER
   IS ANSWERED. css-values-4 §10 "Mathematical Expressions" admits a math function wherever a `<number>` or an
   `<angle>` is accepted, so `scale(calc(1 + 1))` is a valid declaration this engine does not yet compute — a
   capability and not a mistake — while `scale(rgb(1,2,3))` is a mistake. core/css/css_math.h answers which,
   and `css_math_eval` would answer the VALUE; what is missing is only its `CssMathResolver`, whose `length_px`
   arm already exists as a static in core/css/css_length.c for exactly these two productions.
   THE QUESTION IS §10.9'S TYPE AND NOT §10.8'S SYNTAX, and the two are different populations: `scale(calc(1px
   + 1s))` IS one math function by §10.8's grammar and its type is §4.3.2's FAILURE, so it is an authoring
   mistake and must take the dropped declaration rather than this crash — which is why the productions this
   file would evaluate are named here rather than asking whether the text is a math function at all. The two
   named are the two the DFAIL's own remedy names, and they are the two css-transforms-1 §7.1 writes. */
static void css_tf_reject_math(const char *s, const char *e)
{
    size_t n;

    if (memchr(s, '(', (size_t)(e - s)) == NULL) return;
    n = (size_t)(e - s);
    if (!css_math_matches(s, n, CSS_MATH_PROD_NUMBER) && !css_math_matches(s, n, CSS_MATH_PROD_ANGLE)) return;
    DFAIL("css-values-4 §10 \"Mathematical Expressions\" admits a math function wherever a <number> or an "
          "<angle> is accepted, and css-transforms-1 §7.1 \"2D Transform Functions\" writes both productions — "
          "so this is a VALID declaration this engine cannot yet compute rather than an authoring mistake, and "
          "answering CSS Syntax §2.2 \"Error Handling\"'s dropped declaration for it would report a transform "
          "the author did not write. WHAT TO BUILD: core/css/css_math.h's `css_math_eval` already answers this "
          "for CSS_MATH_PROD_NUMBER and CSS_MATH_PROD_ANGLE; what it needs is a `CssMathResolver`, and the "
          "`length_px` arm of one already exists as `css_len_math_length` in core/css/css_length.c, reached "
          "there through `css_length_parse`'s own function branch. EXPOSE that resolver from "
          "core/css/css_length.h rather than writing a second one here — two resolvers for one question is the "
          "copy that drifts — and this file then evaluates both productions through it. The <length-percentage> "
          "arguments need none of this: `css_length_parse` already routes a math function for them");
}

/* `[ <angle> | <zero> ]` over a whole span. §7 states the second arm in its own words — "a <zero> behaves the
   same as 0deg (\"unitless 0\" angles are preserved for legacy compat)" — so a bare `0` is admitted HERE and
   is not admitted by css-values-4 §7.1 "Angle Units: the <angle> type and deg, grad, rad, turn units" in
   general, which is why this reader states the legacy arm for itself instead of asking css_angle_unit about an
   absent unit.
   THE UNIT IS STORED LOWERCASED. "As specified" is a statement about the VALUE and not about the author's
   bytes — §3's line moves lengths and leaves everything else alone — and CSSOM §6.7.2's serialization of a
   dimension writes the unit in lower case, so `1TURN` and `1turn` are one computed value and one string. The
   empty unit is `<zero>`, which serializes as the bare number §7 says is preserved. */
static bool css_tf_angle(const char *s, const char *e, double *num, char unit[CSS_TF_ANGLE_UNIT_MAX])
{
    const char *p = s;
    size_t i, ulen;

    css_tf_reject_math(s, e);
    /* The unit is an ident sequence and therefore starts at the first code point that cannot continue a
       number; every §7.1 unit is ASCII alphabetic, so the split is where the digits, sign, dot and exponent
       stop. The exponent's own `e` is why this scans for the LAST such boundary rather than the first letter:
       `1e2deg` is a dimension whose number is `1e2`. */
    while (p < e && (isdigit((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.')) p++;
    if (p < e && (*p == 'e' || *p == 'E')) {
        const char *q = p + 1;

        if (q < e && (*q == '+' || *q == '-')) q++;
        if (q < e && isdigit((unsigned char)*q)) {
            while (q < e && isdigit((unsigned char)*q)) q++;
            p = q;
        }
    }
    if (!css_tf_number(s, p, num)) return false;
    ulen = (size_t)(e - p);
    if (ulen == 0) {
        /* §7's `<zero>`: the legacy arm, and it is the NUMBER that qualifies a value for it rather than the
           absent unit — `rotate(2)` is not an angle in any browser. */
        if (*num != 0.0) return false;
        unit[0] = '\0';
        return true;
    }
    if (ulen >= CSS_TF_ANGLE_UNIT_MAX) return false;
    if (!css_angle_unit(p, ulen)) return false;
    for (i = 0; i < ulen; i++) unit[i] = (char)tolower((unsigned char)p[i]);
    unit[ulen] = '\0';
    return true;
}

/* §3's "with lengths made absolute" for one `<length-percentage>` argument. The membership test comes FIRST
   and is a separate entry from the parse for the reason core/css/css_length.h states: `css_length_parse` is
   for a value already known to match the grammar and CRASHES on one that does not, and nothing validated this
   declaration before this component — lexbor's registry carries no `transform` entry at all — so asking
   `css_length_is_length_percentage` is what keeps `translate(2)` an authoring mistake rather than an abort. */
static bool css_tf_length(JSContext *realm, const CssFontMetrics *font, const char *s, const char *e,
                          CssLength *out)
{
    char *copy = malloc((size_t)(e - s) + 1);
    double bare;
    bool ok;

    CHECK(copy != NULL, "css-transforms-1 §7: OOM copying a <length-percentage> out of a transform function");
    memcpy(copy, s, (size_t)(e - s));
    copy[e - s] = '\0';
    /* A BARE NON-ZERO NUMBER IS REFUSED HERE, AND IT IS THE ONE INPUT THAT WOULD OTHERWISE ABORT THIS ENGINE
       FROM A STYLESHEET. `css_length_is_length_percentage` answers TRUE for a unitless number — CSS Values §6
       "Distance Units: the <length> type" permits the unit to be omitted for ZERO, and that predicate cannot
       see the value — while `css_length_parse` DCHECKs that the number is zero, its own comment saying a
       unitless non-zero belongs to a property whose grammar admits a bare `<number>` (`line-height`). Neither
       is wrong: a `transform` is simply the first caller whose value is a STRANGER'S text that nothing
       validated first, because lexbor's registry carries no `transform` entry. So `translate(2)` — an ordinary
       authoring mistake — would reach that assert and hand any page an abort switch for this engine, which
       CLAUDE.md's rule about foreign bytes forbids outright. §7.1 "2D Transform Functions" settles it without
       widening anything: every translation's argument is `<length-percentage>`, which admits no bare non-zero
       number, so this is CSS Syntax §2.2 "Error Handling"'s invalid declaration and the check belongs to the
       grammar rather than to the length parser. THE ZERO STAYS ADMITTED, which is why this tests the VALUE and
       not the shape: `translate(0)` is a length in every browser. */
    if (css_tf_number(s, e, &bare) && bare != 0.0) { free(copy); return false; }
    ok = css_length_is_length_percentage(copy);
    if (ok) {
        *out = css_length_parse(realm, font, copy);
        DCHECK(out->kind != CSS_LENGTH_KEYWORD,
               "a value core/css/css_length.h's `css_length_is_length_percentage` admitted parsed to a KEYWORD, "
               "and css-transforms-1 §7.1 \"2D Transform Functions\" writes no keyword in any translation's "
               "grammar — so this is those two entries disagreeing about one production rather than a "
               "declaration this file should have refused");
        /* AND THE STATE IS REFUSED RATHER THAN ONLY ASSERTED, so it is impossible in the build where the
           assert is compiled out too. A keyword that reached the serializer would be printed where a length
           belongs, and the consumer that reduces this list to a matrix would read a translation out of a word;
           refusing makes it CSS Syntax §2.2 "Error Handling"'s dropped declaration in both builds, which is an
           answer every consumer already handles. */
        if (out->kind == CSS_LENGTH_KEYWORD) ok = false;
    }
    free(copy);
    return ok;
}

/* ONE `<transform-function>`'s arguments, split on the commas its own grammar writes. `s`..`e` is the text
   BETWEEN the function's parentheses. A comma nested inside a further function — `min()`, `calc()` — belongs
   to that function and not to this list, which is why the split tracks depth rather than scanning for commas. */
static bool css_tf_args(JSContext *realm, const CssFontMetrics *font, const char *s, const char *e,
                        CssTfArgType type, unsigned min, unsigned max, CssTransformFunction *fn)
{
    const char *arg = s;
    unsigned depth = 0;

    fn->argc = 0;
    for (;;) {
        const char *p = arg;
        const char *stop;
        const char *as, *ae;

        /* The body this walks was delimited by the caller's own paren match, so it is BALANCED and `depth`
           can never be decremented below zero — which is what makes an unsigned counter the right shape here
           rather than one that has to represent a state its producer cannot create. */
        while (p < e && (depth > 0 || *p != ',')) {
            if (*p == '(') depth++;
            else if (*p == ')') {
                DCHECK(depth > 0,
                       "a transform function's argument list closed a parenthesis it never opened, and the "
                       "span this walks is the text BETWEEN a matched pair core/css/css_transform_function.c "
                       "found by depth — so the body is balanced and this is that match and this walk "
                       "disagreeing about where the function ends");
                depth--;
            }
            p++;
        }
        stop = p;
        as = arg;
        ae = stop;
        css_tf_trim(&as, &ae);
        /* An EMPTY argument is not a production §7.1 writes, and it is how a trailing or doubled comma
           arrives. It is refused here rather than reaching a reader that would see an empty span. */
        if (as == ae) return false;
        if (fn->argc >= max) return false;
        switch (type) {
        case CSS_TF_ARG_NUMBER:
            css_tf_reject_math(as, ae);
            if (!css_tf_number(as, ae, &fn->number[fn->argc])) return false;
            break;
        case CSS_TF_ARG_LENGTH_PERCENTAGE:
            /* NOT A REFUSAL, BECAUSE A TABLE TYPO IS NOT AN AUTHORING MISTAKE. `max` is already 2 for every
               row with this production, so an argument past the second means the transcription of §7.1 above
               disagrees with the array it writes into — this codebase's own data, which is what a DCHECK may
               stand on and what a `return false` would have hidden as a dropped declaration. */
            DCHECK(fn->argc < 2,
                   "a `<length-percentage>` transform function accepted more than two arguments, and "
                   "css-transforms-1 §7.1 \"2D Transform Functions\" writes at most two in any translation's "
                   "grammar — so CSS_TF_GRAMMAR's `max` column and CssTransformFunction's `length` array "
                   "disagree about the same section");
            if (!css_tf_length(realm, font, as, ae, &fn->length[fn->argc])) return false;
            break;
        case CSS_TF_ARG_ANGLE:
            DCHECK(fn->argc < 2,
                   "an `[ <angle> | <zero> ]` transform function accepted more than two arguments, and "
                   "css-transforms-1 §7.1 \"2D Transform Functions\" writes at most two in any rotation's or "
                   "skew's grammar — so CSS_TF_GRAMMAR's `max` column and CssTransformFunction's `angle` array "
                   "disagree about the same section");
            if (!css_tf_angle(as, ae, &fn->angle[fn->argc], fn->angle_unit[fn->argc])) return false;
            break;
        }
        fn->argc++;
        if (stop == e) break;
        arg = stop + 1;                                      /* past the comma */
    }
    return fn->argc >= min;
}

bool css_transform_list_parse(JSContext *realm, const CssFontMetrics *font, const char *value,
                              CssTransformList *out)
{
    const char *p, *end;
    size_t cap = 0;

    DCHECK(value != NULL,
           "css-transforms-1 §3 \"The transform Property\"'s specified value was asked for as NULL — the "
           "cascade answers `transform` for every element (core/css/css_style_declaration.c carries the row "
           "lexbor's registry has no entry for), so an absent value is a cascade that stopped early");
    DCHECK(out != NULL, "a <transform-list> was parsed with nowhere to write it");
    out->fn = NULL;
    out->n = 0;
    end = value + strlen(value);
    /* CSS Syntax §4.3.7 "Consume an escaped code point" is a capability this reader does not have, and it is a
       CRASH rather than a refusal because an escape is always VALID syntax: `\74 ranslate(1px)` is a
       declaration every browser honours, so answering CSS Syntax §2.2 "Error Handling"'s dropped declaration
       for it would drop what an author wrote. It is asked of the whole value up front because an escape may
       appear in a function name OR in a unit identifier, and those are two readers below. */
    if (memchr(value, '\\', (size_t)(end - value)) != NULL)
        DFAIL("a `transform` declaration carries a CSS Syntax §4.3.7 \"Consume an escaped code point\" escape, "
              "and this component's ident and unit readers walk bytes rather than consuming ident sequences. "
              "An escape is VALID CSS — `\\74 ranslate(1px)` names translate() in every browser — so refusing "
              "it as CSS Syntax §2.2 \"Error Handling\"'s invalid declaration would drop an author's own "
              "transform and report `none` where a browser reports a translation. WHAT TO BUILD: CSS Syntax "
              "§4.3.12 \"Consume an ident sequence\" over the name scan and the unit scan in this file, which "
              "is where both would decode. HOW ITS ABSENCE SHOWS: a stylesheet whose transform function name "
              "or angle unit is written with an escape aborts here instead of computing");
    p = css_tf_skip_ws(value, end);
    /* `<transform-list> = <transform-function>+` admits no empty match, so an all-whitespace value is not a
       list. The `none` arm of §3's `Value:` line is the caller's and never reaches here. */
    if (p == end) return false;
    while (p < end) {
        const char *name = p;
        size_t nlen, i;
        const char *open, *body;
        unsigned depth;
        bool known = false;

        /* CSS Syntax §4.3.4 "Consume an ident-like token" makes a FUNCTION TOKEN an ident sequence immediately
           followed by U+0028, so no whitespace may stand between the name and its parenthesis. Every §7.1
           function name is ASCII alphanumeric, and the escape arm above has already left. */
        while (p < end && (isalnum((unsigned char)*p) || *p == '-' || *p == '_')) p++;
        nlen = (size_t)(p - name);
        if (nlen == 0 || p == end || *p != '(') { css_transform_list_free(out); return false; }
        open = p;
        /* The matching close, by depth, so a nested function's own parentheses do not end this one. */
        depth = 0;
        do {
            if (*p == '(') depth++;
            else if (*p == ')') depth--;
            p++;
        } while (p < end && depth > 0);
        if (depth != 0) { css_transform_list_free(out); return false; }
        body = open + 1;                                     /* p is one past the matching ')' */
        for (i = 0; i < sizeof CSS_TF_LEVEL2 / sizeof CSS_TF_LEVEL2[0]; i++)
            if (css_tf_name_is(name, nlen, CSS_TF_LEVEL2[i]))
                DFAILF("css-transforms-2 §12.2 \"3D Transform Functions\" defines `%s()` and this engine "
                       "implements no level 2, so this is a capability that is MISSING rather than an "
                       "authoring mistake — CSS Syntax §2.2 \"Error Handling\"'s dropped declaration is the "
                       "answer for a value no standard names, and answering it here would report the author's "
                       "transform as `none` and hand every consumer a WRONG matrix rather than an absent one. "
                       "WHAT TO BUILD: §12.2's ten grammars beside §7.1's table in this file, and with them "
                       "the third row and column css-transforms-2 §11 \"Current Transformation Matrix\" gives "
                       "every function — the 2D matrix this file's consumers reduce to is that 4x4 with the "
                       "Z axis left as the identity, so the two land together or a translate3d() computes as "
                       "a translate(). HOW ITS ABSENCE SHOWS: a page whose transform names any 3D function "
                       "aborts here rather than computing. THE RELEASE ARM IS THE LINE BELOW AND IT IS A "
                       "STATE RATHER THAN A SILENCE: with the abort compiled out the name matches no row of "
                       "CSS_TF_GRAMMAR either, so the value is refused, the declaration is dropped and the "
                       "element computes §3's `Initial:` value of `none` — which every consumer of this "
                       "component already handles as the identity, so the pair of arms leaves no state a "
                       "sibling has to test for",
                       CSS_TF_LEVEL2[i]);
        for (i = 0; i < sizeof CSS_TF_GRAMMAR / sizeof CSS_TF_GRAMMAR[0]; i++) {
            CssTransformFunction fn;

            if (!css_tf_name_is(name, nlen, CSS_TF_GRAMMAR[i].name)) continue;
            memset(&fn, 0, sizeof fn);
            fn.kind = CSS_TF_GRAMMAR[i].kind;
            if (!css_tf_args(realm, font, body, p - 1, CSS_TF_GRAMMAR[i].arg,
                             CSS_TF_GRAMMAR[i].min, CSS_TF_GRAMMAR[i].max, &fn)) {
                css_transform_list_free(out);
                return false;
            }
            if (out->n == cap) {
                CssTransformFunction *g;

                cap = cap ? cap * 2 : 4;
                g = realloc(out->fn, cap * sizeof *g);
                CHECK(g != NULL, "css-transforms-1 §3: OOM growing a <transform-list>");
                out->fn = g;
            }
            out->fn[out->n++] = fn;
            known = true;
            break;
        }
        /* A name §7.1 does not write and css-transforms-2 §12.2 does not write either is a function no
           standard this engine knows enumerates, which is CSS Syntax §2.2's invalid declaration. */
        if (!known) { css_transform_list_free(out); return false; }
        p = css_tf_skip_ws(p, end);
    }
    DCHECK(out->n > 0,
           "a <transform-list> parsed to ZERO functions, and css-transforms-1 §3 \"The transform Property\" "
           "writes the production as `<transform-list> = <transform-function>+` — a `+` admits no empty match, "
           "and the empty-value arm above already left. An empty list would also be indistinguishable from "
           "`none` at every consumer, which is the other arm of §3's `Value:` line and a different fact");
    return true;
}

void css_transform_list_free(CssTransformList *list)
{
    DCHECK(list != NULL, "a <transform-list> was released through a NULL handle");
    free(list->fn);
    list->fn = NULL;
    list->n = 0;
}

/* One `<length-percentage>` argument as CSSOM §6.7.2 "Serializing CSS Values" writes it, through the three
   entries core/css/css_length.h already states over the same sentence. The CALCULATED arm is §3.1's "avoiding
   <calc()> expressions where possible" read against css-values-4 §10.13 "Serialization", which keeps BOTH
   terms of the residue — "A value like calc(20px + 0%) would serialize as calc(0% + 20px), maintaining both
   terms in the serialized value" — so a calc() that survives here is one no serialization may avoid. */
static char *css_tf_length_text(const CssLength *len)
{
    switch (len->kind) {
    case CSS_LENGTH_ABSOLUTE:   return css_length_serialize_px(len->px.px);
    case CSS_LENGTH_PERCENTAGE: return css_length_serialize_pct(len->pct);
    case CSS_LENGTH_CALCULATED: return css_length_serialize_calc(len->pct, len->px.px);
    case CSS_LENGTH_KEYWORD:    break;
    }
    /* UNREACHABLE BY CONSTRUCTION, and this arm is a GUARD rather than a capability this file owes. The only
       producer of these values REFUSES a keyword outright (see `css_tf_length`), so no `CssTransformList` can
       hold one — which is why the message states its own unreachability proof instead of naming something to
       build. The release arm returns the keyword's own text: the state cannot arise, and a defined string is
       what keeps a build with the assert compiled out from dereferencing a NULL here. */
    DFAIL("a `<length-percentage>` argument of a transform function is a KEYWORD, and css-transforms-1 §7.1 "
          "\"2D Transform Functions\" writes no keyword in any translation's grammar. `css_tf_length` refuses "
          "one at the parse rather than only asserting against it, so no list this component builds can carry "
          "one — reaching this arm is that refusal and this switch having come apart");
    return strdup(len->keyword);
}

char *css_transform_list_serialize(const CssTransformList *list)
{
    JsonBuf b;
    size_t i;

    DCHECK(list != NULL, "a <transform-list> was serialized through a NULL handle");
    DCHECK(list->n > 0 && list->fn != NULL,
           "an EMPTY <transform-list> was serialized. §3's production is `<transform-function>+`, so there is "
           "no such list, and `none` is the other arm of that property's `Value:` line rather than a list of "
           "length zero — a caller holding `none` never reaches this component");
    memset(&b, 0, sizeof b);
    for (i = 0; i < list->n; i++) {
        const CssTransformFunction *fn = &list->fn[i];
        unsigned a;

        /* §8 "The Transform Function Lists": "If a list of <transform-function>s is provided, then the net
           effect is as if each transform function had been specified separately in the order provided." The
           order is therefore load-bearing and not a presentation choice, and §3.1's "joining space-separated
           tokens with a single space" is what separates them. */
        if (i > 0) json_buf_raw(&b, " ");
        json_buf_raw(&b, CSS_TF_GRAMMAR[fn->kind].name);
        json_buf_raw(&b, "(");
        for (a = 0; a < fn->argc; a++) {
            /* §3.1: "following each serialized comma with a single space". */
            if (a > 0) json_buf_raw(&b, ", ");
            switch (CSS_TF_GRAMMAR[fn->kind].arg) {
            case CSS_TF_ARG_NUMBER: {
                char *t = css_length_serialize_number(fn->number[a], "");

                json_buf_raw(&b, t);
                free(t);
                break;
            }
            case CSS_TF_ARG_LENGTH_PERCENTAGE: {
                char *t = css_tf_length_text(&fn->length[a]);

                json_buf_raw(&b, t);
                free(t);
                break;
            }
            case CSS_TF_ARG_ANGLE: {
                /* The unit is appended verbatim, and the empty one is §7's `<zero>` — "a <zero> behaves the
                   same as 0deg (\"unitless 0\" angles are preserved for legacy compat)" — so the bare number
                   §7 says is PRESERVED is what comes back out. */
                char *t = css_length_serialize_number(fn->angle[a], fn->angle_unit[a]);

                json_buf_raw(&b, t);
                free(t);
                break;
            }
            }
        }
        json_buf_raw(&b, ")");
    }
    /* THE NAME IS THE GRAMMAR TABLE'S, which holds §7.1's own spelling, so what comes back out is what that
       section writes — `translateX(` and not the folded form the matcher compares. That is the whole reason
       `css_tf_name_is` folds both sides rather than the table being spelled for the matcher's convenience. */
    return json_buf_take(&b);
}
