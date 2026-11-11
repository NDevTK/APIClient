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
#include <lexbor/core/base.h>   /* lexbor_realloc: the allocator the engine gives its refusal edge */
#include "solver/reclaim.h"

/* One argument's production. css-transforms-1 §7.1 "2D Transform Functions" gives every argument of a function
   the same one; css-transforms-2 §12.2 "3D Transform Functions" mixes them (rotate3d() is three numbers and an
   angle, translate3d() two length-percentages and a length), so a grammar row spells its productions in order.
   Each production fills its own array of CssTransformFunction in the order written. */
typedef enum {
    CSS_TF_ARG_NUMBER = 'n',               /* `<number>` */
    CSS_TF_ARG_NUMBER_PERCENTAGE = 'p',    /* `[ <number> | <percentage> ]`, css-transforms-2 §12's scales */
    CSS_TF_ARG_LENGTH_PERCENTAGE = 'L',    /* `<length-percentage>` */
    CSS_TF_ARG_LENGTH = 'l',               /* `<length>` */
    CSS_TF_ARG_ANGLE = 'a',                /* `[ <angle> | <zero> ]` */
    CSS_TF_ARG_PERSPECTIVE = 'v'           /* `[ <length [0,∞]> | none ]` */
} CssTfArgType;

/* The grammars of css-transforms-1 §7.1 and css-transforms-2 §12.1 "2D Transform Functions" and §12.2, in
   CssTransformFnKind's order. `args` is one production per position; a single letter covers every position.
   `min`/`max` are the argument counts the production admits. */
static const struct {
    const char        *name;
    CssTransformFnKind kind;
    const char        *args;
    unsigned           min, max;
} CSS_TF_GRAMMAR[] = {
    { "matrix",      CSS_TF_MATRIX,      "n",    6, 6 },    /* matrix( <number>#{6} ) */
    { "translate",   CSS_TF_TRANSLATE,   "L",    1, 2 },    /* translate( <length-percentage> , <length-percentage>? ) */
    { "translateX",  CSS_TF_TRANSLATE_X, "L",    1, 1 },
    { "translateY",  CSS_TF_TRANSLATE_Y, "L",    1, 1 },
    { "scale",       CSS_TF_SCALE,       "p",    1, 2 },    /* scale( [ <number> | <percentage> ]#{1,2} ) */
    { "scaleX",      CSS_TF_SCALE_X,     "p",    1, 1 },
    { "scaleY",      CSS_TF_SCALE_Y,     "p",    1, 1 },
    { "rotate",      CSS_TF_ROTATE,      "a",    1, 1 },    /* rotate( [ <angle> | <zero> ] ) */
    { "skew",        CSS_TF_SKEW,        "a",    1, 2 },
    { "skewX",       CSS_TF_SKEW_X,      "a",    1, 1 },
    { "skewY",       CSS_TF_SKEW_Y,      "a",    1, 1 },
    { "matrix3d",    CSS_TF_MATRIX3D,    "n",    16, 16 },  /* matrix3d( <number>#{16} ) */
    { "translate3d", CSS_TF_TRANSLATE3D, "LLl",  3, 3 },    /* ( <length-percentage> , <length-percentage> , <length> ) */
    { "translateZ",  CSS_TF_TRANSLATE_Z, "l",    1, 1 },
    { "scale3d",     CSS_TF_SCALE3D,     "p",    3, 3 },    /* scale3d( [ <number> | <percentage> ]#{3} ) */
    { "scaleZ",      CSS_TF_SCALE_Z,     "p",    1, 1 },
    { "rotate3d",    CSS_TF_ROTATE3D,    "nnna", 4, 4 },    /* ( <number> , <number> , <number> , [ <angle> | <zero> ] ) */
    { "rotateX",     CSS_TF_ROTATE_X,    "a",    1, 1 },
    { "rotateY",     CSS_TF_ROTATE_Y,    "a",    1, 1 },
    { "rotateZ",     CSS_TF_ROTATE_Z,    "a",    1, 1 },
    { "perspective", CSS_TF_PERSPECTIVE, "v",    1, 1 }     /* perspective( [ <length [0,∞]> | none ] ) */
};

/* The production of argument `i` of grammar row `g`. */
static CssTfArgType css_tf_arg_type(size_t g, unsigned i)
{
    size_t n = strlen(CSS_TF_GRAMMAR[g].args);

    DCHECK(n == 1 || n == CSS_TF_GRAMMAR[g].max,
           "a transform grammar row spells neither one production nor one per argument");
    return (CssTfArgType)CSS_TF_GRAMMAR[g].args[n == 1 ? 0 : i];
}

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
    copy = lexbor_malloc((size_t)(e - s) + 1);
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

/* A math function argument as css-values-4 §10.11 "Computed Value" computes it: its calculation tree simplified,
   which for `want` of `<number>` or `<angle>` is one number, an angle in css-values-4 §7.1's canonical deg, its
   `<length>` leaves absolutized by the cascade's one resolver (core/css/css_length.h). `absolutize` false asks
   §10.9 "Type Checking"'s type alone and writes nothing. False is a type that does not match `want`. */
static bool css_tf_math(JSContext *realm, const CssFontMetrics *font, bool absolutize, const char *s,
                        const char *e, CssMathProduction want, double *out)
{
    CssLengthMathLeaves leaves = { realm, font };
    CssMathResolver res = { css_length_math_leaf, &leaves, realm };
    CssMathValue v;

    DCHECK(want == CSS_MATH_PROD_NUMBER || want == CSS_MATH_PROD_PERCENTAGE || want == CSS_MATH_PROD_ANGLE,
           "a transform argument asked a math function for a production css-transforms-2 §12 writes for none");
    if (!absolutize) return css_math_matches(s, (size_t)(e - s), want);
    if (!css_math_eval(s, (size_t)(e - s), &res, want, &v)) return false;
    DCHECK(v.tree == NULL, "a <number>, <percentage> or <angle> math function kept a calculation tree, which only "
                           "a percentage resolved against another type needs");
    *out = want == CSS_MATH_PROD_PERCENTAGE && v.pct_term ? v.pct : v.num.px;
    if (v.num.env != CSS_ENV_NONE)
        DFAIL("a transform's <number> or <angle> math function derives from an environment fact (a viewport or "
              "font length divided out), and CssTransformFunction carries a plain double, so the fact would be "
              "dropped. WHAT TO BUILD: CssPx-typed number and angle slots, read through css_transform_matrix.c");
    /* §10.12 "Range Checking" clamps an infinity to the context's range, and §5.1 "Range Restrictions and Range
       Definition Notation" makes an unstated range [−∞,∞] within an implementation's "finite capacity". */
    if (*out * 0.0 != 0.0) {
        DFAIL("a transform argument's math function is infinite, and css-values-4 §10.12 \"Range Checking\" "
              "clamps it to the finite capacity §5.1 leaves to the implementation. WHAT TO BUILD: that clamp, "
              "measured in real Chrome, before css_transform_matrix.c multiplies by it");
        return false;
    }
    return true;
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

/* §3's "with lengths made absolute" for one `<length-percentage>` argument. Nothing validated this declaration
   before this component — lexbor's registry carries no `transform` entry — and `css_length_parse` refuses a
   value outside the grammar, so `translate(2)` and `translate(red)` are CSS Syntax §2.2 "Error Handling"'s
   invalid declaration rather than an abort. css-transforms-1 §7.1 "2D Transform Functions" writes no keyword in
   any translation's grammar, so the parse is given the empty keyword set. */
static bool css_tf_length(JSContext *realm, const CssFontMetrics *font, const char *s, const char *e,
                          CssLength *out)
{
    char *copy = lexbor_malloc((size_t)(e - s) + 1);
    bool ok;

    CHECK(copy != NULL, "css-transforms-1 §7: OOM copying a <length-percentage> out of a transform function");
    memcpy(copy, s, (size_t)(e - s));
    copy[e - s] = '\0';
    ok = css_length_parse(realm, font, copy, CSS_KEYWORDS_NONE, out);
    free(copy);
    return ok;
}

/* `<length-percentage>` over a whole span, deciding the grammar alone: a math function by css-values-4 §10.9's
   type, a percentage, a dimension in one of css-values-4 §6's length units, or the unitless zero §6 admits for a
   length. Nothing is absolutized, so no realm and no font is asked. */
static bool css_tf_length_valid(const char *s, const char *e)
{
    const char *p = s;
    double num;
    size_t ulen;

    if (memchr(s, '(', (size_t)(e - s)) != NULL)
        return css_math_matches(s, (size_t)(e - s), CSS_MATH_PROD_LENGTH_PERCENTAGE);
    /* The number ends where its digits, sign, dot and exponent do, as css_tf_angle splits a dimension. */
    while (p < e && (isdigit((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.')) p++;
    if (p < e && (*p == 'e' || *p == 'E')) {
        const char *q = p + 1;

        if (q < e && (*q == '+' || *q == '-')) q++;
        if (q < e && isdigit((unsigned char)*q)) {
            while (q < e && isdigit((unsigned char)*q)) q++;
            p = q;
        }
    }
    if (!css_tf_number(s, p, &num)) return false;
    ulen = (size_t)(e - p);
    if (ulen == 0) return num == 0.0;
    if (ulen == 1 && *p == '%') return true;
    return css_length_is_length_unit(p, ulen);
}

/* `[ <number> | <percentage> ]`: css-transforms-2 §12 "The Transform Functions" makes a percentage in a scale
   "equivalent to a number", and it "serializes as a number in specified values", so it is stored as one. */
static bool css_tf_number_percentage(JSContext *realm, const CssFontMetrics *font, bool absolutize,
                                     const char *s, const char *e, double *out)
{
    if (memchr(s, '(', (size_t)(e - s)) != NULL) {
        if (css_tf_math(realm, font, absolutize, s, e, CSS_MATH_PROD_NUMBER, out)) return true;
        if (!css_tf_math(realm, font, absolutize, s, e, CSS_MATH_PROD_PERCENTAGE, out)) return false;
        if (absolutize) *out /= 100.0;
        return true;
    }
    if (e > s && e[-1] == '%') {
        if (!css_tf_number(s, e - 1, out)) return false;
        *out /= 100.0;
        return true;
    }
    return css_tf_number(s, e, out);
}

/* `<length>` over a whole span, a `<length-percentage>` without the percentage: css-transforms-2 §12.2's
   translateZ() and translate3d()'s third argument, and perspective()'s. `nonnegative` is the `[0,∞]` range,
   which refuses a negative literal; a math function's value is clamped to it (css-values-4 §10.12 "Range
   Checking"), not refused. */
static bool css_tf_length_only(JSContext *realm, const CssFontMetrics *font, bool absolutize, const char *s,
                               const char *e, bool nonnegative, CssLength *out)
{
    bool math = memchr(s, '(', (size_t)(e - s)) != NULL;

    if (!math && e > s && e[-1] == '%') return false;
    if (!math && nonnegative && *s == '-') return false;
    if (!absolutize)
        return math ? css_math_matches(s, (size_t)(e - s), CSS_MATH_PROD_LENGTH) : css_tf_length_valid(s, e);
    if (!css_tf_length(realm, font, s, e, out)) return false;
    if (out->kind != CSS_LENGTH_ABSOLUTE) return false;
    if (nonnegative && out->px.px < 0.0) out->px = css_px(0.0);
    return true;
}

/* ONE `<transform-function>`'s arguments, split on the commas its own grammar writes. `s`..`e` is the text
   BETWEEN the function's parentheses; `g` is its grammar row. A comma nested inside a further function —
   `min()`, `calc()` — belongs to that function and not to this list, which is why the split tracks depth. */
static bool css_tf_args(JSContext *realm, const CssFontMetrics *font, bool absolutize, const char *s,
                        const char *e, size_t g, CssTransformFunction *fn)
{
    const char *arg = s;
    unsigned depth = 0, n_num = 0, n_len = 0, n_ang = 0;

    fn->argc = 0;
    for (;;) {
        const char *p = arg;
        const char *stop;
        const char *as, *ae;

        /* The body was delimited by the caller's own paren match, so it is balanced and `depth` cannot go
           below zero. */
        while (p < e && (depth > 0 || *p != ',')) {
            if (*p == '(') depth++;
            else if (*p == ')') {
                DCHECK(depth > 0, "a transform function's argument list closed a parenthesis it never opened "
                                  "inside a span its caller matched by depth");
                depth--;
            }
            p++;
        }
        stop = p;
        as = arg;
        ae = stop;
        css_tf_trim(&as, &ae);
        /* An empty argument is no production; it is how a trailing or doubled comma arrives. */
        if (as == ae) return false;
        if (fn->argc >= CSS_TF_GRAMMAR[g].max) return false;
        switch (css_tf_arg_type(g, fn->argc)) {
        case CSS_TF_ARG_NUMBER:
            DCHECK(n_num < CSS_TF_ARG_MAX, "a grammar row admits more numbers than CssTransformFunction holds");
            if (memchr(as, '(', (size_t)(ae - as)) != NULL
                    ? !css_tf_math(realm, font, absolutize, as, ae, CSS_MATH_PROD_NUMBER, &fn->number[n_num])
                    : !css_tf_number(as, ae, &fn->number[n_num]))
                return false;
            n_num++;
            break;
        case CSS_TF_ARG_NUMBER_PERCENTAGE:
            DCHECK(n_num < CSS_TF_ARG_MAX, "a grammar row admits more numbers than CssTransformFunction holds");
            if (!css_tf_number_percentage(realm, font, absolutize, as, ae, &fn->number[n_num++])) return false;
            break;
        case CSS_TF_ARG_LENGTH_PERCENTAGE:
            DCHECK(n_len < CSS_TF_LENGTH_MAX, "a grammar row admits more lengths than CssTransformFunction holds");
            if (absolutize ? !css_tf_length(realm, font, as, ae, &fn->length[n_len])
                           : !css_tf_length_valid(as, ae))
                return false;
            n_len++;
            break;
        case CSS_TF_ARG_LENGTH:
            DCHECK(n_len < CSS_TF_LENGTH_MAX, "a grammar row admits more lengths than CssTransformFunction holds");
            if (!css_tf_length_only(realm, font, absolutize, as, ae, false, &fn->length[n_len])) return false;
            n_len++;
            break;
        case CSS_TF_ARG_PERSPECTIVE:
            DCHECK(n_len < CSS_TF_LENGTH_MAX, "a grammar row admits more lengths than CssTransformFunction holds");
            if (css_tf_name_is(as, (size_t)(ae - as), "none")) {
                fn->perspective_none = true;
                break;
            }
            if (!css_tf_length_only(realm, font, absolutize, as, ae, true, &fn->length[n_len])) return false;
            n_len++;
            break;
        case CSS_TF_ARG_ANGLE:
            DCHECK(n_ang < CSS_TF_ANGLE_MAX, "a grammar row admits more angles than CssTransformFunction holds");
            /* A math function computes to its canonical unit; a literal angle keeps the unit written. */
            if (memchr(as, '(', (size_t)(ae - as)) != NULL) {
                if (!css_tf_math(realm, font, absolutize, as, ae, CSS_MATH_PROD_ANGLE, &fn->angle[n_ang]))
                    return false;
                memcpy(fn->angle_unit[n_ang], "deg", sizeof "deg");
            }
            else if (!css_tf_angle(as, ae, &fn->angle[n_ang], fn->angle_unit[n_ang]))
                return false;
            n_ang++;
            break;
        }
        fn->argc++;
        if (stop == e) break;
        arg = stop + 1;                                      /* past the comma */
    }
    return fn->argc >= CSS_TF_GRAMMAR[g].min;
}

/* CSS Syntax §4.3.7's escapes, asked of the whole value: see the crash. */
static void css_tf_refuse_escapes(const char *value, const char *end)
{
    /* CSS Syntax §4.3.7 "Consume an escaped code point" is a capability this reader does not have, and it is a
       CRASH rather than a refusal because an escape is always VALID syntax: `\74 ranslate(1px)` is a
       declaration every browser honours, so answering CSS Syntax §2.2 "Error Handling"'s dropped declaration
       for it would drop what an author wrote. It is asked of the whole value up front because an escape may
       appear in a function name OR in a unit identifier, and those are two readers below. */
    if (memchr(value, '\\', (size_t)(end - value)) != NULL)
        DFAIL("a transform value carries a CSS Syntax §4.3.7 \"Consume an escaped code point\" escape, "
              "and this component's ident and unit readers walk bytes rather than consuming ident sequences. "
              "An escape is VALID CSS — `\\74 ranslate(1px)` names translate() in every browser — so refusing "
              "it as CSS Syntax §2.2 \"Error Handling\"'s invalid declaration would drop an author's own "
              "transform and report `none` where a browser reports a translation. WHAT TO BUILD: CSS Syntax "
              "§4.3.12 \"Consume an ident sequence\" over the name scan and the unit scan in this file, which "
              "is where both would decode. HOW ITS ABSENCE SHOWS: a stylesheet whose transform function name "
              "or angle unit is written with an escape aborts here instead of computing");
}

/* ONE `<transform-function>` at `*pp`, read into `fn` with `*pp` moved past its closing parenthesis. False is CSS
   Syntax §2.2's dropped declaration. `absolutize` false decides the grammar alone and asks no realm or font. */
static bool css_tf_function(JSContext *realm, const CssFontMetrics *font, bool absolutize, const char **pp,
                            const char *end, CssTransformFunction *fn)
{
    const char *p = *pp, *name = p, *open, *body;
    size_t nlen, i;
    unsigned depth;

    /* CSS Syntax §4.3.4 "Consume an ident-like token" makes a FUNCTION TOKEN an ident sequence immediately
       followed by U+0028, so no whitespace may stand between the name and its parenthesis. Every §7.1
       function name is ASCII alphanumeric, and the escape arm above has already left. */
    while (p < end && (isalnum((unsigned char)*p) || *p == '-' || *p == '_')) p++;
    nlen = (size_t)(p - name);
    if (nlen == 0 || p == end || *p != '(') return false;
    open = p;
    /* The matching close, by depth, so a nested function's own parentheses do not end this one. */
    depth = 0;
    do {
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
        p++;
    } while (p < end && depth > 0);
    if (depth != 0) return false;
    body = open + 1;                                     /* p is one past the matching ')' */
    for (i = 0; i < sizeof CSS_TF_GRAMMAR / sizeof CSS_TF_GRAMMAR[0]; i++) {
        if (!css_tf_name_is(name, nlen, CSS_TF_GRAMMAR[i].name)) continue;
        memset(fn, 0, sizeof *fn);
        fn->kind = CSS_TF_GRAMMAR[i].kind;
        DCHECK(CSS_TF_GRAMMAR[i].kind == (CssTransformFnKind)i,
               "CSS_TF_GRAMMAR is out of CssTransformFnKind's order, and the serializer indexes it by kind");
        if (!css_tf_args(realm, font, absolutize, body, p - 1, i, fn)) return false;
        *pp = p;
        return true;
    }
    /* A name neither level writes is CSS Syntax §2.2's invalid declaration. */
    return false;
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
    css_tf_refuse_escapes(value, end);
    p = css_tf_skip_ws(value, end);
    /* `<transform-list> = <transform-function>+` admits no empty match, so an all-whitespace value is not a
       list. The `none` arm of §3's `Value:` line is the caller's and never reaches here. */
    if (p == end) return false;
    while (p < end) {
        CssTransformFunction fn;

        if (!css_tf_function(realm, font, true, &p, end, &fn)) { css_transform_list_free(out); return false; }
        if (out->n == cap) {
            CssTransformFunction *g;

            cap = cap ? cap * 2 : 4;
            g = lexbor_realloc(out->fn, cap * sizeof *g);
            CHECK(g != NULL, "css-transforms-1 §3: OOM growing a <transform-list>");
            out->fn = g;
        }
        out->fn[out->n++] = fn;
        p = css_tf_skip_ws(p, end);
    }
    DCHECK(out->n > 0,
           "a <transform-list> parsed to ZERO functions, and css-transforms-1 §3 \"The transform Property\" "
           "writes the production as `<transform-list> = <transform-function>+` — a `+` admits no empty match, "
           "and the empty-value arm above already left. An empty list would also be indistinguishable from "
           "`none` at every consumer, which is the other arm of §3's `Value:` line and a different fact");
    return true;
}

bool css_transform_function_matches(const char *value, size_t len)
{
    const char *p = value, *end = value + len;
    CssTransformFunction fn;

    DCHECK(value != NULL || len == 0, "a <transform-function> was matched over a NULL span");
    css_tf_refuse_escapes(value, end);
    p = css_tf_skip_ws(p, end);
    if (!css_tf_function(NULL, NULL, false, &p, end, &fn)) return false;
    return css_tf_skip_ws(p, end) == end;
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
char *css_transform_length_serialize(const CssLength *len)
{
    switch (len->kind) {
    case CSS_LENGTH_ABSOLUTE:   return css_length_serialize_px(len->px.px);
    case CSS_LENGTH_PERCENTAGE: return css_length_serialize_pct(len->pct);
    case CSS_LENGTH_CALCULATED:
        return len->tree != NULL ? css_math_tree_serialize(len->tree) : css_length_serialize_calc(len->pct, len->px.px);
    case CSS_LENGTH_KEYWORD:    break;
    }
    /* UNREACHABLE BY CONSTRUCTION, and this arm is a GUARD rather than a capability this file owes. The only
       producer of these values REFUSES a keyword outright (see `css_tf_length`), so no `CssTransformList` can
       hold one — which is why the message states its own unreachability proof instead of naming something to
       build. The release arm returns the empty string: the state cannot arise, and a defined string is what
       keeps a build with the assert compiled out from dereferencing a NULL here. */
    DFAIL("a `<length-percentage>` argument of a transform function is a KEYWORD, and css-transforms-1 §7.1 "
          "\"2D Transform Functions\" writes no keyword in any translation's grammar. `css_tf_length` refuses "
          "one at the parse rather than only asserting against it, so no list this component builds can carry "
          "one — reaching this arm is that refusal and this switch having come apart");
    return reclaim_strdup("");
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
        unsigned a, n_num = 0, n_len = 0, n_ang = 0;

        /* §8 "The Transform Function Lists": "If a list of <transform-function>s is provided, then the net
           effect is as if each transform function had been specified separately in the order provided." The
           order is therefore load-bearing and not a presentation choice, and §3.1's "joining space-separated
           tokens with a single space" is what separates them. */
        if (i > 0) json_buf_raw(&b, " ");
        json_buf_raw(&b, CSS_TF_GRAMMAR[fn->kind].name);
        json_buf_raw(&b, "(");
        for (a = 0; a < fn->argc; a++) {
            char *t = NULL;

            /* §3.1: "following each serialized comma with a single space". */
            if (a > 0) json_buf_raw(&b, ", ");
            switch (css_tf_arg_type((size_t)fn->kind, a)) {
            case CSS_TF_ARG_NUMBER:
            case CSS_TF_ARG_NUMBER_PERCENTAGE:
                /* css-transforms-2 §12: a scale's percentage "serializes as a number in specified values". */
                t = css_length_serialize_number(fn->number[n_num++], "");
                break;
            case CSS_TF_ARG_LENGTH_PERCENTAGE:
            case CSS_TF_ARG_LENGTH:
                t = css_transform_length_serialize(&fn->length[n_len++]);
                break;
            case CSS_TF_ARG_PERSPECTIVE:
                t = fn->perspective_none ? reclaim_strdup("none") : css_transform_length_serialize(&fn->length[n_len++]);
                CHECK(t != NULL, "css-transforms-2 §12.2: OOM serializing perspective()");
                break;
            case CSS_TF_ARG_ANGLE:
                /* The unit is appended verbatim, and the empty one is §7's `<zero>` — "a <zero> behaves the
                   same as 0deg (\"unitless 0\" angles are preserved for legacy compat)" — so the bare number
                   §7 says is PRESERVED is what comes back out. */
                t = css_length_serialize_number(fn->angle[n_ang], fn->angle_unit[n_ang]);
                n_ang++;
                break;
            }
            json_buf_raw(&b, t);
            free(t);
        }
        json_buf_raw(&b, ")");
    }
    /* THE NAME IS THE GRAMMAR TABLE'S, which holds §7.1's own spelling, so what comes back out is what that
       section writes — `translateX(` and not the folded form the matcher compares. That is the whole reason
       `css_tf_name_is` folds both sides rather than the table being spelled for the matcher's convenience. */
    return json_buf_take(&b);
}
