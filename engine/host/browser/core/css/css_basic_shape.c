/* css-shapes-1 §3's `<basic-shape>` and css-masking-1 §5.1's clip-path grammar. See css_basic_shape.h. */
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <lexbor/core/base.h>

#include "check.h"
#include "core/css/css_basic_shape.h"
#include "core/css/css_dimension.h"
#include "core/css/css_length.h"
#include "core/css/css_math.h"
#include "core/css/css_position_value.h"
#include "core/css/css_shorthand.h"

/* One component value of a function's arguments, with `,` and `/` each a token of their own. */
typedef struct { const char *s; size_t n; } BsTok;

/* CSS Syntax §4's component values of `text`, split on whitespace outside any function or string, with a
   comma or a solidus outside one standing alone. Answers the count; `max` bounds it and a longer list is -1. */
static int bs_tokens(const char *text, size_t n, BsTok *out, int max)
{
    const char *p = text, *end = text + n;
    int k = 0;

    while (p < end) {
        const char *s;
        int depth = 0;

        while (p < end && isspace((unsigned char)*p)) p++;
        if (p == end) break;
        s = p;
        if (*p == ',' || *p == '/') p++;
        else
            while (p < end && (depth > 0 || (!isspace((unsigned char)*p) && *p != ',' && *p != '/'))) {
                if (*p == '"' || *p == '\'') {
                    char q = *p++;

                    while (p < end && *p != q) p += (*p == '\\' && p + 1 < end) ? 2 : 1;
                    if (p < end) p++;
                    continue;
                }
                if (*p == '(') depth++;
                else if (*p == ')' && depth > 0) depth--;
                p++;
            }
        if (k == max) return -1;
        out[k].s = s;
        out[k].n = (size_t)(p - s);
        k++;
    }
    return k;
}

static bool bs_is(BsTok t, const char *kw)
{
    size_t n = strlen(kw);

    return t.n == n && strncasecmp(t.s, kw, n) == 0;
}

static bool bs_is_any(BsTok t, const char *const *kw, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (bs_is(t, kw[i])) return true;
    return false;
}

static char *bs_dup(BsTok t)
{
    char *p = lexbor_malloc(t.n + 1);

    CHECK(p != NULL, "css-shapes-1 §3: OOM copying a basic-shape component");
    memcpy(p, t.s, t.n);
    p[t.n] = '\0';
    return p;
}

/* A `<length-percentage>`, or with `percent` false a `<length>`; `non_negative` refuses a literal below zero
   (a math function is clamped where the value is used). */
static bool bs_length(BsTok t, bool percent, bool non_negative)
{
    char *c = bs_dup(t);
    bool ok = percent ? css_length_is_length_percentage(c) : css_length_is_length(c);

    free(c);
    return ok && !(non_negative && t.s[0] == '-' && memchr(t.s, '(', t.n) == NULL);
}

static bool bs_angle(BsTok t)
{
    size_t u = t.n;
    double d;
    char *c;
    bool ok;

    while (u > 0 && isalpha((unsigned char)t.s[u - 1])) u--;
    if (u > 0 && u < t.n && css_shorthand_number(t.s, u, &d) && css_angle_unit(t.s + u, t.n - u)) return true;
    if (memchr(t.s, '(', t.n) == NULL) return false;
    c = bs_dup(t);
    ok = css_math_matches(c, t.n, CSS_MATH_PROD_ANGLE);
    free(c);
    return ok;
}

/* css-values-4 §8.3 "2D Positioning: the <position> type", whole over the `k` tokens. */
static bool bs_position(const BsTok *t, int k)
{
    const char *w[4];
    size_t wl[4];
    CssPositionValue p;
    int i;

    if (k < 1 || k > 4) return false;
    for (i = 0; i < k; i++) {
        if (bs_is(t[i], ",") || bs_is(t[i], "/")) return false;
        w[i] = t[i].s;
        wl[i] = t[i].n;
    }
    return css_position_match(w, wl, (unsigned)k, false, &p) == (unsigned)k;
}

/* css-backgrounds-3's `<'border-radius'>`: `<length-percentage [0,∞]>{1,4} [ / <length-percentage [0,∞]>{1,4}
   ]?`. */
static bool bs_border_radius(const BsTok *t, int k)
{
    int i, side = 0, run = 0;

    for (i = 0; i < k; i++) {
        if (bs_is(t[i], "/")) {
            if (side == 1 || run == 0) return false;
            side = 1;
            run = 0;
            continue;
        }
        if (!bs_length(t[i], true, true) || ++run > 4) return false;
    }
    return run > 0;
}

static int bs_find(const BsTok *t, int k, const char *kw)
{
    int i;

    for (i = 0; i < k; i++)
        if (bs_is(t[i], kw)) return i;
    return k;
}

/* inset(), xywh() and rect(): four or fewer edges, then `[ round <'border-radius'> ]?`. */
static bool bs_rect_family(const char *fn, const BsTok *t, int k)
{
    int r = bs_find(t, k, "round"), i;

    if (r < k && !bs_border_radius(t + r + 1, k - r - 1)) return false;
    if (strcmp(fn, "inset") == 0) {
        if (r < 1 || r > 4) return false;
        for (i = 0; i < r; i++)
            if (!bs_length(t[i], true, false)) return false;
        return true;
    }
    if (r != 4) return false;
    if (strcmp(fn, "xywh") == 0)
        return bs_length(t[0], true, false) && bs_length(t[1], true, false) && bs_length(t[2], true, true) &&
               bs_length(t[3], true, true);
    for (i = 0; i < 4; i++)
        if (!bs_is(t[i], "auto") && !bs_length(t[i], true, false)) return false;
    return true;
}

/* circle() and ellipse(): css-images-4's `<radial-size> = <radial-extent>{1,2} | <length-percentage
   [0,∞]>{1,2}`, of which circle() admits one component, then `[ at <position> ]?`. */
static bool bs_radial(bool circle, const BsTok *t, int k)
{
    static const char *const EXTENT[] = { "closest-corner", "closest-side", "farthest-corner", "farthest-side" };
    int a = bs_find(t, k, "at"), i, extents = 0;

    if (a > (circle ? 1 : 2)) return false;
    for (i = 0; i < a; i++) {
        if (bs_is_any(t[i], EXTENT, COUNTOF(EXTENT))) extents++;
        else if (!bs_length(t[i], true, true)) return false;
    }
    if (extents != 0 && extents != a) return false;
    return a == k || bs_position(t + a + 1, k - a - 1);
}

static bool bs_fill_rule(BsTok t)
{
    return bs_is(t, "nonzero") || bs_is(t, "evenodd");
}

/* The comma-separated groups of `t`, each as a start and a count into it. */
static int bs_groups(const BsTok *t, int k, int *start, int *count, int max)
{
    int g = 0, i, s = 0;

    for (i = 0; i <= k; i++)
        if (i == k || bs_is(t[i], ",")) {
            if (g == max) return -1;
            start[g] = s;
            count[g] = i - s;
            g++;
            s = i + 1;
        }
    return g;
}

static bool bs_pair(const BsTok *t, int k)
{
    return k == 2 && bs_length(t[0], true, false) && bs_length(t[1], true, false);
}

/* `<command-end-point> = [ to <position> | by <coordinate-pair> ]` over the whole of `t`. */
static bool bs_end_point(const BsTok *t, int k)
{
    if (k < 2) return false;
    if (bs_is(t[0], "to")) return bs_position(t + 1, k - 1);
    return bs_is(t[0], "by") && bs_pair(t + 1, k - 1);
}

/* `<relative-control-point> = <coordinate-pair> [ from [ start | end | origin ] ]?`, and with `absolute` the
   `<control-point>` that also admits a `<position>`. */
static bool bs_control_point(const BsTok *t, int k, bool absolute)
{
    static const char *const ANCHOR[] = { "start", "end", "origin" };

    if (k == 4 && bs_is(t[2], "from") && bs_is_any(t[3], ANCHOR, COUNTOF(ANCHOR)) && bs_pair(t, 2)) return true;
    if (bs_pair(t, k)) return true;
    return absolute && bs_position(t, k);
}

/* curve's and smooth's `[ to <position> | by <coordinate-pair> ] with ...`; smooth admits no `with` and one
   control point, curve needs one and admits two. */
static bool bs_curve(bool smooth, const BsTok *t, int k)
{
    int w = bs_find(t, k, "with"), s;
    bool to;

    if (!bs_end_point(t, w)) return false;
    to = bs_is(t[0], "to");
    if (w == k) return smooth;
    s = bs_find(t + w + 1, k - w - 1, "/") + w + 1;
    if (!bs_control_point(t + w + 1, s - w - 1, to)) return false;
    if (s == k) return true;
    return !smooth && bs_control_point(t + s + 1, k - s - 1, to);
}

/* `arc <command-end-point> [ [ of <length-percentage>{1,2} ] && <arc-sweep>? && <arc-size>? && [rotate
   <angle>]? ]`. The end point runs to the first of the bracket's keywords. */
static bool bs_arc(const BsTok *t, int k)
{
    static const char *const KW[] = { "of", "cw", "ccw", "large", "small", "rotate" };
    int e = 1, i;
    bool of = false, sweep = false, size = false, rotate = false;

    while (e < k && !bs_is_any(t[e], KW, COUNTOF(KW))) e++;
    if (!bs_end_point(t, e)) return false;
    for (i = e; i < k; i++) {
        if (bs_is(t[i], "of") && !of) {
            int r = 0;

            of = true;
            while (r < 2 && i + 1 < k && bs_length(t[i + 1], true, false)) { i++; r++; }
            if (r == 0) return false;
        }
        else if ((bs_is(t[i], "cw") || bs_is(t[i], "ccw")) && !sweep) sweep = true;
        else if ((bs_is(t[i], "large") || bs_is(t[i], "small")) && !size) size = true;
        else if (bs_is(t[i], "rotate") && !rotate && i + 1 < k && bs_angle(t[i + 1])) { rotate = true; i++; }
        else return false;
    }
    return of;
}

/* §3.1.1's `<shape-command>`. */
static bool bs_command(const BsTok *t, int k)
{
    static const char *const HTO[] = { "left", "center", "right", "x-start", "x-end" };
    static const char *const VTO[] = { "top", "center", "bottom", "y-start", "y-end" };
    bool h;

    if (k < 1) return false;
    if (bs_is(t[0], "close")) return k == 1;
    if (bs_is(t[0], "move") || bs_is(t[0], "line")) return bs_end_point(t + 1, k - 1);
    if ((h = bs_is(t[0], "hline")) || bs_is(t[0], "vline")) {
        if (k != 3) return false;
        if (bs_is(t[1], "by")) return bs_length(t[2], true, false);
        return bs_is(t[1], "to") && (bs_length(t[2], true, false) ||
                                     (h ? bs_is_any(t[2], HTO, COUNTOF(HTO)) : bs_is_any(t[2], VTO, COUNTOF(VTO))));
    }
    if (bs_is(t[0], "curve")) return bs_curve(false, t + 1, k - 1);
    if (bs_is(t[0], "smooth")) return bs_curve(true, t + 1, k - 1);
    if (bs_is(t[0], "arc")) return bs_arc(t + 1, k - 1);
    return false;
}

/* polygon(), path() and shape(), each over its comma groups. */
static bool bs_listed(const char *fn, const BsTok *t, int k)
{
    int *start, *count, g, i, first = 0;
    bool ok = true;

    start = lexbor_malloc(sizeof *start * (size_t)(k + 1));
    count = lexbor_malloc(sizeof *count * (size_t)(k + 1));
    CHECK(start != NULL && count != NULL, "css-shapes-1 §3.1: OOM splitting a basic shape's arguments");
    g = bs_groups(t, k, start, count, k + 1);
    if (strcmp(fn, "polygon") == 0) {
        /* `<'fill-rule'>? [ round <length> ]? , [<length-percentage> <length-percentage>]#` */
        const BsTok *p = t + start[0];
        int n = count[0], j = 0;

        if (n > 0 && bs_fill_rule(p[0])) j++;
        if (j + 1 < n && bs_is(p[j], "round") && bs_length(p[j + 1], false, true)) j += 2;
        if (j > 0) {
            if (j != n) ok = false;
            first = 1;
        }
        if (g - first < 1) ok = false;
        for (i = first; ok && i < g; i++) ok = bs_pair(t + start[i], count[i]);
    }
    else if (strcmp(fn, "path") == 0) {
        /* `<'fill-rule'>? , <string>` */
        if (g == 2) ok = count[0] == 1 && bs_fill_rule(t[start[0]]);
        else ok = g == 1;
        ok = ok && count[g - 1] == 1 && css_shorthand_string(t[start[g - 1]].s, t[start[g - 1]].n);
    }
    else {
        /* `<'fill-rule'>? from <position> , <shape-command>#` */
        const BsTok *p = t + start[0];
        int n = count[0], j = n > 0 && bs_fill_rule(p[0]) ? 1 : 0;

        ok = g >= 2 && j < n && bs_is(p[j], "from") && bs_position(p + j + 1, n - j - 1);
        for (i = 1; ok && i < g; i++) ok = bs_command(t + start[i], count[i]);
    }
    free(start);
    free(count);
    return ok;
}

bool css_basic_shape_is(const char *text, size_t n)
{
    static const char *const FN[] = { "inset", "xywh", "rect", "circle", "ellipse", "polygon", "path", "shape" };
    const char *open = memchr(text, '(', n);
    char name[8];
    size_t name_n, i;
    BsTok *t;
    int k;
    bool ok = false;

    DCHECK(text != NULL, "css-shapes-1 §3's <basic-shape> question was asked of no text");
    if (open == NULL || text[n - 1] != ')') return false;
    name_n = (size_t)(open - text);
    if (name_n == 0 || name_n >= sizeof name) return false;
    for (i = 0; i < name_n; i++) name[i] = (char)tolower((unsigned char)text[i]);
    name[name_n] = '\0';
    for (i = 0; i < COUNTOF(FN) && strcmp(name, FN[i]) != 0; i++)
        ;
    if (i == COUNTOF(FN)) return false;
    t = lexbor_malloc(sizeof *t * (n + 1));
    CHECK(t != NULL, "css-shapes-1 §3.1: OOM splitting a basic shape's arguments");
    k = bs_tokens(open + 1, n - name_n - 2, t, (int)n + 1);
    if (k >= 0) {
        if (i < 3) ok = bs_rect_family(name, t, k);
        else if (i < 5) ok = bs_radial(i == 3, t, k);
        else ok = bs_listed(name, t, k);
    }
    free(t);
    return ok;
}

/* css-masking-1 §5.1's `<geometry-box> = <shape-box> | fill-box | stroke-box | view-box`, with css-shapes-1's
   `<shape-box> = <visual-box> | margin-box` and css-box-4's `<visual-box> = content-box | padding-box |
   border-box`. */
static bool bs_geometry_box(BsTok t)
{
    static const char *const BOX[] = { "content-box", "padding-box", "border-box", "margin-box", "fill-box",
                                       "stroke-box", "view-box" };

    return bs_is_any(t, BOX, COUNTOF(BOX));
}

static bool bs_url(BsTok t)
{
    return t.n > 5 && t.s[t.n - 1] == ')' && (strncasecmp(t.s, "url(", 4) == 0 || strncasecmp(t.s, "src(", 4) == 0);
}

char *css_clip_path_specified(const char *value)
{
    BsTok t[3];
    int k;
    char *out;
    size_t len;

    DCHECK(value != NULL, "css-masking-1 §5.1's clip-path grammar was asked about no value");
    k = bs_tokens(value, strlen(value), t, 3);
    if (k == 1 && (bs_is(t[0], "none") || bs_url(t[0]) || bs_geometry_box(t[0]) ||
                   css_basic_shape_is(t[0].s, t[0].n)))
        return bs_is(t[0], "none") ? bs_dup((BsTok){ "none", 4 }) : bs_dup(t[0]);
    if (k != 2) return NULL;
    if (!((css_basic_shape_is(t[0].s, t[0].n) && bs_geometry_box(t[1])) ||
          (bs_geometry_box(t[0]) && css_basic_shape_is(t[1].s, t[1].n))))
        return NULL;
    len = t[0].n + 1 + t[1].n;
    out = lexbor_malloc(len + 1);
    CHECK(out != NULL, "css-masking-1 §5.1: OOM copying a clip-path value");
    memcpy(out, t[0].s, t[0].n);
    out[t[0].n] = ' ';
    memcpy(out + t[0].n + 1, t[1].s, t[1].n);
    out[len] = '\0';
    return out;
}
