/* filter-effects-1 §5 and §6's grammar and computed value. See css_filter.h. */
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <lexbor/core/base.h>

#include "check.h"
#include "core/css/css_color.h"
#include "core/css/css_dimension.h"
#include "core/css/css_filter.h"
#include "core/css/css_length.h"
#include "core/css/css_math.h"
#include "core/css/css_shorthand.h"

/* §6.1's functions and each one's argument grammar. */
typedef enum {
    FL_URL,          /* <url>, kept as specified */
    FL_LENGTH,       /* blur( <length>? ), non-negative */
    FL_AMOUNT,       /* brightness() … sepia(): ( [ <number> | <percentage> ]? ), non-negative */
    FL_ANGLE,        /* hue-rotate( [ <angle> | <zero> ]? ) */
    FL_SHADOW        /* drop-shadow( [ <color>? && <length>{2,3} ] ) */
} FlArgs;

static const struct { const char *name; FlArgs args; const char *omitted; } FL_FUNCTIONS[] = {
    { "url", FL_URL, NULL }, { "src", FL_URL, NULL },
    { "blur", FL_LENGTH, "0px" }, { "brightness", FL_AMOUNT, "1" }, { "contrast", FL_AMOUNT, "1" },
    { "drop-shadow", FL_SHADOW, NULL }, { "grayscale", FL_AMOUNT, "1" }, { "hue-rotate", FL_ANGLE, "0deg" },
    { "invert", FL_AMOUNT, "1" }, { "opacity", FL_AMOUNT, "1" }, { "saturate", FL_AMOUNT, "1" },
    { "sepia", FL_AMOUNT, "1" },
};

/* drop-shadow()'s most arguments: a colour and three lengths. */
#define FL_SHADOW_PARTS 4

typedef struct { char *s; size_t n, cap; } FlBuf;

static void fl_put(FlBuf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t cap = (b->n + n + 1) * 2;
        char *p = lexbor_realloc(b->s, cap);

        CHECK(p != NULL, "filter-effects-1 §5: OOM building a filter value");
        b->s = p;
        b->cap = cap;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = '\0';
}

static void fl_puts(FlBuf *b, const char *s)
{
    fl_put(b, s, strlen(s));
}

static char *fl_dupn(const char *s, size_t n)
{
    char *p = lexbor_malloc(n + 1);

    CHECK(p != NULL, "filter-effects-1 §5: OOM copying a filter component");
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* `w` as a function, its lowercase name matched against §6.1's table: the row, with the arguments' span. */
static int fl_function(const char *w, size_t n, const char **args, size_t *args_n)
{
    const char *open = memchr(w, '(', n);
    size_t name_n, i;

    if (open == NULL || n < 2 || w[n - 1] != ')') return -1;
    name_n = (size_t)(open - w);
    for (i = 0; i < COUNTOF(FL_FUNCTIONS); i++)
        if (strlen(FL_FUNCTIONS[i].name) == name_n && strncasecmp(w, FL_FUNCTIONS[i].name, name_n) == 0) {
            *args = open + 1;
            *args_n = n - name_n - 2;
            return (int)i;
        }
    return -1;
}

static bool fl_math(const char *w, size_t n, CssMathProduction want)
{
    char *copy;
    bool ok;

    if (memchr(w, '(', n) == NULL) return false;
    copy = fl_dupn(w, n);
    ok = css_math_matches(copy, n, want);
    free(copy);
    return ok;
}

/* A `<length>`; `non_negative` refuses a literal below zero ("Negative values are not allowed"), and a math
   function's result is clamped at computed-value time (css-values-4 §10.12 "Range Checking"). */
static bool fl_length(const char *w, size_t n, bool non_negative)
{
    char *copy = fl_dupn(w, n);
    bool ok = css_length_is_length(copy);

    free(copy);
    return ok && !(non_negative && w[0] == '-' && memchr(w, '(', n) == NULL);
}

static bool fl_amount(const char *w, size_t n)
{
    double d;

    if (css_shorthand_number(w, n, &d)) return d >= 0.0;
    if (n > 1 && w[n - 1] == '%' && css_shorthand_number(w, n - 1, &d)) return d >= 0.0;
    return fl_math(w, n, CSS_MATH_PROD_NUMBER) || fl_math(w, n, CSS_MATH_PROD_PERCENTAGE);
}

static bool fl_angle(const char *w, size_t n)
{
    size_t u = n;
    double d;

    if (css_shorthand_number(w, n, &d)) return d == 0.0;
    while (u > 0 && isalpha((unsigned char)w[u - 1])) u--;
    if (u > 0 && u < n && css_shorthand_number(w, u, &d) && css_angle_unit(w + u, n - u)) return true;
    return fl_math(w, n, CSS_MATH_PROD_ANGLE);
}

static bool fl_color(const char *w, size_t n)
{
    CssColor c;

    return css_color_parse(w, n, &c);
}

/* drop-shadow()'s `<color>? && <length>{2,3}`: the colour, if any, is the first or the last part, and the
   lengths are the rest, of which the third is a standard deviation and so non-negative. `*color` is the
   colour's index or -1. */
static bool fl_shadow(const char *const *a, const size_t *al, int n, int *color)
{
    int first, i;

    if (n < 2 || n > FL_SHADOW_PARTS) return false;
    *color = -1;
    if (fl_color(a[0], al[0])) *color = 0;
    else if (fl_color(a[n - 1], al[n - 1])) *color = n - 1;
    first = *color == 0 ? 1 : 0;
    if (n - (*color >= 0) < 2 || n - (*color >= 0) > 3) return false;
    for (i = 0; i < n - (*color >= 0); i++)
        if (!fl_length(a[first + i], al[first + i], i == 2)) return false;
    return true;
}

/* One list item's arguments, split; false outside the row's grammar. */
static bool fl_args_valid(FlArgs kind, const char *const *a, const size_t *al, int n)
{
    int color;

    switch (kind) {
    case FL_URL:
        return true;
    case FL_LENGTH:
        return n == 0 || (n == 1 && fl_length(a[0], al[0], true));
    case FL_AMOUNT:
        return n == 0 || (n == 1 && fl_amount(a[0], al[0]));
    case FL_ANGLE:
        return n == 0 || (n == 1 && fl_angle(a[0], al[0]));
    case FL_SHADOW:
        return fl_shadow(a, al, n, &color);
    }
    DFAIL("filter-effects-1 §6.1: a filter function row names an argument grammar this file does not enumerate");
    return false;
}

/* The item's arguments split into component values; -1 for a list the split refuses. `inner` is owned by the
   caller and outlives `a`. */
static int fl_split(const char *args, size_t args_n, char **inner, const char **a, size_t *al, int max)
{
    *inner = fl_dupn(args, args_n);
    return css_shorthand_components(*inner, a, al, max);
}

char *css_filter_specified(const char *value)
{
    size_t len, i;
    const char **w;
    size_t *wl;
    int n, k;
    FlBuf out = { NULL, 0, 0 };

    DCHECK(value != NULL, "filter-effects-1 §5's grammar was asked about no value");
    len = strlen(value);
    if (len == 0) return NULL;
    w = lexbor_malloc(sizeof *w * len);
    wl = lexbor_malloc(sizeof *wl * len);
    CHECK(w != NULL && wl != NULL, "filter-effects-1 §5: OOM splitting a filter value");
    n = css_shorthand_components(value, w, wl, (int)len);
    if (n == 1 && wl[0] == 4 && strncasecmp(w[0], "none", 4) == 0) {
        fl_puts(&out, "none");
        n = 0;
    }
    else if (n < 1) {
        free(w);
        free(wl);
        return NULL;
    }
    for (k = 0; k < n; k++) {
        const char *args, *a[FL_SHADOW_PARTS + 1];
        size_t args_n, al[FL_SHADOW_PARTS + 1];
        char *inner;
        int row = fl_function(w[k], wl[k], &args, &args_n), m;

        if (row < 0) break;
        if (FL_FUNCTIONS[row].args == FL_URL) {
            if (k > 0) fl_puts(&out, " ");
            fl_puts(&out, FL_FUNCTIONS[row].name);
            fl_put(&out, w[k] + strlen(FL_FUNCTIONS[row].name), wl[k] - strlen(FL_FUNCTIONS[row].name));
            continue;
        }
        m = fl_split(args, args_n, &inner, a, al, FL_SHADOW_PARTS + 1);
        if (m < 0 || !fl_args_valid(FL_FUNCTIONS[row].args, a, al, m)) {
            free(inner);
            break;
        }
        if (k > 0) fl_puts(&out, " ");
        fl_puts(&out, FL_FUNCTIONS[row].name);
        fl_puts(&out, "(");
        for (i = 0; i < (size_t)m; i++) {
            if (i > 0) fl_puts(&out, " ");
            fl_put(&out, a[i], al[i]);
        }
        fl_puts(&out, ")");
        free(inner);
    }
    free(w);
    free(wl);
    if (k < n) {
        free(out.s);
        return NULL;
    }
    return out.s;
}

/* A length argument's computed value: absolute, with a math function clamped where the row is non-negative. */
static void fl_put_length(FlBuf *b, JSContext *realm, const CssFontMetrics *font, const char *w, size_t n,
                          bool non_negative)
{
    char *copy = fl_dupn(w, n), *s;
    CssLength len;
    bool ok = css_length_parse(realm, font, copy, CSS_KEYWORDS_NONE, &len);

    free(copy);
    DCHECK(ok && len.kind == CSS_LENGTH_ABSOLUTE,
           "filter-effects-1 §6.2: a filter length the specified-value grammar admitted is not an absolute "
           "length once computed, so the two parses disagree");
    s = css_length_serialize_px(non_negative && len.px.px < 0.0 ? 0.0 : len.px.px);
    fl_puts(b, s);
    free(s);
}

/* CSS Color 4's computed `<color>`: `currentcolor` stays the keyword, any other colour is its value. */
static void fl_put_color(FlBuf *b, const char *w, size_t n)
{
    char text[CSS_COLOR_FUNCTION_MAX];
    CssColor c;
    bool ok;

    if (n == 12 && strncasecmp(w, "currentcolor", 12) == 0) {
        fl_puts(b, "currentcolor");
        return;
    }
    ok = css_color_parse(w, n, &c);
    DCHECK(ok, "filter-effects-1 §6.2: a drop-shadow colour the specified-value grammar admitted does not parse");
    if (c.space == CSS_COLOR_SPACE_SRGB) css_color_serialize_srgb(&c, text);
    else css_color_serialize_function(&c, text);
    fl_puts(b, text);
}

char *css_filter_computed(JSContext *realm, const CssFontMetrics *font, const char *specified)
{
    size_t len;
    const char **w;
    size_t *wl;
    int n, k;
    FlBuf out = { NULL, 0, 0 };

    DCHECK(specified != NULL, "filter-effects-1 §6.2's computed value was asked of no specified value");
    if (css_filter_is_none(specified)) {
        fl_puts(&out, "none");
        return out.s;
    }
    len = strlen(specified);
    w = lexbor_malloc(sizeof *w * len);
    wl = lexbor_malloc(sizeof *wl * len);
    CHECK(w != NULL && wl != NULL, "filter-effects-1 §6.2: OOM splitting a filter value");
    n = css_shorthand_components(specified, w, wl, (int)len);
    DCHECK(n >= 1, "filter-effects-1 §6.2: a specified filter list the grammar admitted splits into nothing");
    for (k = 0; k < n; k++) {
        const char *args, *a[FL_SHADOW_PARTS + 1];
        size_t args_n, al[FL_SHADOW_PARTS + 1];
        char *inner;
        int row = fl_function(w[k], wl[k], &args, &args_n), m, color, i;
        FlArgs kind;

        DCHECK(row >= 0, "filter-effects-1 §6.2: a specified filter item the grammar admitted names no function");
        if (k > 0) fl_puts(&out, " ");
        kind = FL_FUNCTIONS[row].args;
        if (kind == FL_URL) {
            fl_put(&out, w[k], wl[k]);
            continue;
        }
        m = fl_split(args, args_n, &inner, a, al, FL_SHADOW_PARTS + 1);
        fl_puts(&out, FL_FUNCTIONS[row].name);
        fl_puts(&out, "(");
        /* "Omitted values are included and compute to their defaults." */
        if (kind == FL_SHADOW) {
            bool ok = fl_shadow(a, al, m, &color);
            int first = color == 0 ? 1 : 0, lengths = m - (color >= 0);

            DCHECK(ok, "filter-effects-1 §6.2: a specified drop-shadow() the grammar admitted fails it");
            /* "drop-shadow() starts with the computed value of <color> followed by the computed value of the
               <length> values"; an omitted colour is the `color` property's, which is `currentcolor`. */
            if (color >= 0) fl_put_color(&out, a[color], al[color]);
            else fl_puts(&out, "currentcolor");
            for (i = 0; i < lengths; i++) {
                fl_puts(&out, " ");
                fl_put_length(&out, realm, font, a[first + i], al[first + i], i == 2);
            }
            if (lengths == 2) fl_puts(&out, " 0px");
        }
        else if (m == 0) fl_puts(&out, FL_FUNCTIONS[row].omitted);
        else if (kind == FL_LENGTH) fl_put_length(&out, realm, font, a[0], al[0], true);
        else fl_put(&out, a[0], al[0]);
        fl_puts(&out, ")");
        free(inner);
    }
    free(w);
    free(wl);
    return out.s;
}

bool css_filter_is_none(const char *computed)
{
    DCHECK(computed != NULL, "filter-effects-1 §5's `none` test was asked of no value");
    return strcasecmp(computed, "none") == 0;
}
