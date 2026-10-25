/* css-grid-1 §7.4's `grid-template`, §7.8's `grid` and their longhands' grammars. See css_grid_template.h. */
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "check.h"
#include "core/css/css_defaulting.h"
#include "core/css/css_grid_template.h"
#include "core/css/css_grid_template_areas.h"
#include "core/css/css_length.h"
#include "core/css/css_shorthand.h"
#include <lexbor/core/base.h>   /* lexbor_realloc: the allocator the engine gives its refusal edge */

const char *const CSS_GRID_LONGHANDS[CSS_GRID_N] = {
    "grid-template-rows", "grid-template-columns", "grid-template-areas",
    "grid-auto-rows", "grid-auto-columns", "grid-auto-flow",
};

/* ---- text ------------------------------------------------------------------------------------------------- */

typedef struct {
    char  *p;
    size_t n, cap;
} GtBuf;

static void gt_put(GtBuf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        char *grown;

        while (b->n + n + 1 > cap) cap *= 2;
        grown = lexbor_realloc(b->p, cap);
        CHECK(grown != NULL, "css-grid-1 §7: out of memory composing a specified value");
        b->p = grown;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}

static void gt_puts(GtBuf *b, const char *s) { gt_put(b, s, strlen(s)); }

/* A separating space before every piece but the first. */
static void gt_sep(GtBuf *b)
{
    if (b->n > 0) gt_put(b, " ", 1);
}

static char *gt_dup(const char *s)
{
    GtBuf b = { NULL, 0, 0 };

    gt_puts(&b, s);
    return b.p;
}

static bool gt_word_is(const char *s, size_t n, const char *word)
{
    return strlen(word) == n && strncasecmp(s, word, n) == 0;
}

/* ---- CSS Syntax §4's component values, as these grammars group them ------------------------------------------
   A `<string>` is one token whatever it holds, a `[ … ]` group is one `<line-names>`, a function is one token
   with its arguments, and `/` is a delimiter of its own whether or not white space surrounds it. */

typedef enum { GT_TOK_WORD = 0, GT_TOK_STRING, GT_TOK_NAMES, GT_TOK_SLASH } GtTokKind;

typedef struct {
    const char *s;
    size_t      n;
    GtTokKind   kind;
} GtTok;

#define GT_TOK_MAX 256

/* The tokens of the `n` bytes at `v`, or -1 for text no grammar here admits (an unbalanced group, an
   unterminated string, more tokens than GT_TOK_MAX). */
static int gt_tokens(const char *v, size_t n, GtTok *t)
{
    const char *end = v + n;
    int k = 0;

    while (v < end) {
        const char *s;

        if (isspace((unsigned char)*v)) { v++; continue; }
        if (k == GT_TOK_MAX) return -1;
        s = v;
        if (*v == '/') {
            t[k].kind = GT_TOK_SLASH;
            v++;
        } else if (*v == '"' || *v == '\'') {
            char q = *v++;

            while (v < end && *v != q && *v != '\n') v += (*v == '\\' && v + 1 < end) ? 2 : 1;
            if (v >= end || *v != q) return -1;
            v++;
            t[k].kind = GT_TOK_STRING;
        } else if (*v == '[') {
            while (v < end && *v != ']') {
                if (*v == '[' && v != s) return -1;
                v++;
            }
            if (v >= end) return -1;
            v++;
            t[k].kind = GT_TOK_NAMES;
        } else {
            int depth = 0;

            while (v < end && (depth > 0 || (!isspace((unsigned char)*v) && *v != '/' && *v != '[' &&
                                             *v != '"' && *v != '\''))) {
                if (*v == '(') depth++;
                else if (*v == ')' && --depth < 0) return -1;
                v++;
            }
            if (depth != 0) return -1;
            t[k].kind = GT_TOK_WORD;
        }
        t[k].s = s;
        t[k].n = (size_t)(v - s);
        k++;
    }
    return k;
}

/* ---- §7.2's track sizes -----------------------------------------------------------------------------------
   Each production is checked and written in its canonical specified form: keywords lower-cased, a flex or a
   length number in css_length.h's shortest round-trip form with its unit lower-cased (a unitless zero is
   `0px`), a math function as written, and `, ` between a function's arguments. `*fixed` reports a
   `<fixed-breadth>` or a `<fixed-size>`, which §7.2's `<auto-track-list>` requires beside an `<auto-repeat>`. */

/* A literal number followed by `unit`-letters (or `%`) over the whole span: the number's value and the unit. */
static bool gt_dimension(const char *s, size_t n, double *v, const char **unit, size_t *un)
{
    size_t i = n;

    while (i > 0 && (isalpha((unsigned char)s[i - 1]) || s[i - 1] == '%')) i--;
    if (i == 0 || i == n) return false;
    if (!css_shorthand_number(s, i, v)) return false;
    *unit = s + i;
    *un = n - i;
    return true;
}

static bool gt_breadth(const char *s, size_t n, bool inflexible, GtBuf *out, bool *fixed)
{
    static const char *const KEYWORDS[] = { "auto", "min-content", "max-content" };
    char copy[128], unit[16];
    const char *u;
    size_t un, i;
    double v;

    *fixed = false;
    for (i = 0; i < sizeof KEYWORDS / sizeof KEYWORDS[0]; i++)
        if (gt_word_is(s, n, KEYWORDS[i])) { gt_puts(out, KEYWORDS[i]); return true; }
    if (n == 0 || n >= sizeof copy) return false;
    memcpy(copy, s, n);
    copy[n] = '\0';
    if (gt_dimension(s, n, &v, &u, &un) && un == 2 && strncasecmp(u, "fr", 2) == 0) {
        char *num;

        /* §7.2.1: "<flex [0,∞]>", and "As a maximum, a <flex> value sets the track's flex factor; it is invalid
           as a minimum." */
        if (inflexible || v < 0.0) return false;
        num = css_length_serialize_number(v, "fr");
        gt_puts(out, num);
        free(num);
        return true;
    }
    /* `<length-percentage [0,∞]>`: a negative literal is outside the range; a unitless number is a length only
       where it is zero (css-values-4 §6: "For zero lengths the unit identifier is optional"). */
    if (!css_length_is_length_percentage(copy)) return false;
    *fixed = true;
    if (css_shorthand_number(s, n, &v)) {
        if (v != 0.0) return false;
        gt_puts(out, "0px");
        return true;
    }
    if (gt_dimension(s, n, &v, &u, &un)) {
        char *num;

        if (v < 0.0 || un >= sizeof unit) return false;
        for (i = 0; i < un; i++) unit[i] = (char)tolower((unsigned char)u[i]);
        unit[un] = '\0';
        num = css_length_serialize_number(v, unit);
        gt_puts(out, num);
        free(num);
        return true;
    }
    gt_put(out, s, n);
    return true;
}

/* The `n` bytes at `s` with ASCII white space trimmed from both ends. */
static const char *gt_trim(const char *s, size_t *n)
{
    while (*n > 0 && isspace((unsigned char)s[0])) { s++; (*n)--; }
    while (*n > 0 && isspace((unsigned char)s[*n - 1])) (*n)--;
    return s;
}

/* The top-level comma of a function's argument span, or NULL. */
static const char *gt_comma(const char *s, size_t n)
{
    int depth = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        if (s[i] == '(') depth++;
        else if (s[i] == ')') depth--;
        else if (s[i] == ',' && depth == 0) return s + i;
    }
    return NULL;
}

/* `<track-size> = <track-breadth> | minmax( <inflexible-breadth> , <track-breadth> ) | fit-content(
   <length-percentage [0,∞]> )`. A `<fixed-size>` is a minmax() with a `<fixed-breadth>` on either side, or a
   `<fixed-breadth>`. */
static bool gt_track_size(const char *s, size_t n, GtBuf *out, bool *fixed)
{
    bool fa, fb;

    if (n > 8 && strncasecmp(s, "minmax(", 7) == 0 && s[n - 1] == ')') {
        const char *a = s + 7, *comma = gt_comma(s + 7, n - 8), *b;
        size_t an, bn;

        if (comma == NULL) return false;
        an = (size_t)(comma - a);
        a = gt_trim(a, &an);
        b = comma + 1;
        bn = (size_t)(s + n - 1 - b);
        b = gt_trim(b, &bn);
        gt_puts(out, "minmax(");
        if (an == 0 || !gt_breadth(a, an, true, out, &fa)) return false;
        gt_puts(out, ", ");
        if (bn == 0 || !gt_breadth(b, bn, false, out, &fb)) return false;
        gt_puts(out, ")");
        *fixed = fa || fb;
        return true;
    }
    if (n > 13 && strncasecmp(s, "fit-content(", 12) == 0 && s[n - 1] == ')') {
        const char *a = s + 12;
        size_t an = n - 13;
        GtBuf arg = { NULL, 0, 0 };
        bool ok;

        a = gt_trim(a, &an);
        ok = an > 0 && gt_breadth(a, an, true, &arg, &fa) && fa;
        if (ok) {
            gt_puts(out, "fit-content(");
            gt_puts(out, arg.p);
            gt_puts(out, ")");
        }
        free(arg.p);
        *fixed = false;
        return ok;
    }
    return gt_breadth(s, n, false, out, fixed);
}

/* §7.2.2's `<line-names> = '[' <custom-ident>* ']'`, written with one space between the names. A
   `<custom-ident>` here excludes `span` and `auto` (§7.2.2: "Excluded words: span, auto") and css-values-4
   §4.2's CSS-wide keywords and `default`. Appended to `out` without its brackets when `bare`, so two adjacent
   lists can be spliced. */
static bool gt_line_names(const char *s, size_t n, GtBuf *out, bool bare)
{
    const char *end = s + n - 1, *p = s + 1;
    bool first = true;

    DCHECK(n >= 2 && s[0] == '[' && s[n - 1] == ']', "a line-names token was split without its brackets");
    if (!bare) gt_puts(out, "[");
    while (p < end) {
        const char *w;
        size_t wn;
        char word[64];

        if (isspace((unsigned char)*p)) { p++; continue; }
        w = p;
        while (p < end && !isspace((unsigned char)*p)) p++;
        wn = (size_t)(p - w);
        if (!(isalpha((unsigned char)w[0]) || w[0] == '_' || w[0] == '-' || (unsigned char)w[0] >= 0x80))
            return false;
        if (wn >= sizeof word) return false;
        memcpy(word, w, wn);
        word[wn] = '\0';
        if (css_wide_keyword(word) || gt_word_is(w, wn, "default") || gt_word_is(w, wn, "span") ||
            gt_word_is(w, wn, "auto"))
            return false;
        if (!first || (bare && out->n > 0 && out->p[out->n - 1] != '[')) gt_puts(out, " ");
        gt_put(out, w, wn);
        first = false;
    }
    if (!bare) gt_puts(out, "]");
    return true;
}

typedef enum {
    GT_LIST_TRACK = 0,   /* §7.2's `<track-list> | <auto-track-list>`, a template longhand's value */
    GT_LIST_EXPLICIT,    /* §7.4's `<explicit-track-list>`: names and sizes, no repeat() */
    GT_LIST_AUTO         /* §7.6's `<track-size>+`: sizes only */
} GtListMode;

static bool gt_track_list(const GtTok *t, int from, int to, GtListMode mode, GtBuf *out);

/* §7.2.3's `repeat()`: `<track-repeat> = repeat( [ <integer [1,∞]> ] , [ <line-names>? <track-size> ]+
   <line-names>? )`, `<auto-repeat> = repeat( [ auto-fill | auto-fit ] , [ <line-names>? <fixed-size> ]+
   <line-names>? )` and `<fixed-repeat>`, whose sizes are fixed. `*autorep` reports the automatic form and
   `*fixed` whether every size inside was fixed. */
static bool gt_repeat(const char *s, size_t n, GtBuf *out, bool *autorep, bool *fixed)
{
    const char *a = s + 7, *comma = gt_comma(s + 7, n - 8), *body;
    size_t an, bn, i;
    GtTok t[GT_TOK_MAX];
    GtBuf inner = { NULL, 0, 0 };
    int k;
    bool ok;

    if (comma == NULL) return false;
    an = (size_t)(comma - a);
    a = gt_trim(a, &an);
    body = comma + 1;
    bn = (size_t)(s + n - 1 - body);
    *autorep = false;
    gt_puts(out, "repeat(");
    if (gt_word_is(a, an, "auto-fill") || gt_word_is(a, an, "auto-fit")) {
        *autorep = true;
        gt_puts(out, an == 9 ? "auto-fill" : "auto-fit");
    } else {
        if (an == 0) return false;
        for (i = 0; i < an; i++)
            if (!isdigit((unsigned char)a[i]) && !(i == 0 && a[i] == '+')) return false;
        if (strtol(a, NULL, 10) < 1) return false;
        gt_put(out, a[0] == '+' ? a + 1 : a, a[0] == '+' ? an - 1 : an);
    }
    gt_puts(out, ", ");
    k = gt_tokens(body, bn, t);
    if (k <= 0) return false;
    /* The body is an `<explicit-track-list>`'s shape; whether its sizes are fixed is asked per size. */
    *fixed = true;
    for (i = 0; i < (size_t)k; i++) {
        GtBuf probe = { NULL, 0, 0 };
        bool f = false;

        if (t[i].kind == GT_TOK_WORD && gt_track_size(t[i].s, t[i].n, &probe, &f) && !f) *fixed = false;
        free(probe.p);
    }
    ok = gt_track_list(t, 0, k, GT_LIST_EXPLICIT, &inner);
    if (ok) gt_puts(out, inner.p);
    free(inner.p);
    if (!ok) return false;
    gt_puts(out, ")");
    return true;
}

/* A track list over tokens `[from, to)`: line names never adjacent, at least one track, and in
   GT_LIST_TRACK at most one `<auto-repeat>`, beside which every other track and repetition is fixed. */
static bool gt_track_list(const GtTok *t, int from, int to, GtListMode mode, GtBuf *out)
{
    int i, tracks = 0, autoreps = 0;
    bool all_fixed = true, prev_names = false;

    for (i = from; i < to; i++) {
        bool fixed = false;

        if (t[i].kind == GT_TOK_NAMES) {
            if (mode == GT_LIST_AUTO || prev_names) return false;
            gt_sep(out);
            if (!gt_line_names(t[i].s, t[i].n, out, false)) return false;
            prev_names = true;
            continue;
        }
        if (t[i].kind != GT_TOK_WORD) return false;
        prev_names = false;
        gt_sep(out);
        if (t[i].n > 8 && strncasecmp(t[i].s, "repeat(", 7) == 0 && t[i].s[t[i].n - 1] == ')') {
            bool autorep;

            if (mode != GT_LIST_TRACK) return false;
            if (!gt_repeat(t[i].s, t[i].n, out, &autorep, &fixed)) return false;
            if (autorep) {
                if (!fixed) return false;
                autoreps++;
            }
        } else if (!gt_track_size(t[i].s, t[i].n, out, &fixed)) {
            return false;
        }
        all_fixed &= fixed;
        tracks++;
    }
    if (tracks == 0) return false;
    /* §7.2's `<auto-track-list>`: "requires definite track sizes so that the number of repetitions can be
       calculated", and an `<auto-repeat>` "can only appear once in the track list". */
    return autoreps == 0 || (autoreps == 1 && all_fixed);
}

/* A longhand's track list, or `none` where `none_ok`, from tokens `[from, to)`; OWNED, NULL when invalid. */
static char *gt_track_list_value(const GtTok *t, int from, int to, GtListMode mode, bool none_ok)
{
    GtBuf b = { NULL, 0, 0 };

    if (none_ok && to - from == 1 && t[from].kind == GT_TOK_WORD && gt_word_is(t[from].s, t[from].n, "none"))
        return gt_dup("none");
    if (!gt_track_list(t, from, to, mode, &b)) {
        free(b.p);
        return NULL;
    }
    return b.p;
}

/* ---- §7.3's strings --------------------------------------------------------------------------------------- */

/* One `<string>` of a template in §7.3's serialized form: "serialize each null cell token as a single "."
   (U+002E FULL STOP), and consecutive cell tokens as separated by a single space (U+0020 SPACE), with all
   other white space elided". The template's validity (trash tokens, rectangles, equal rows) is §7.3's parse,
   asked over the whole value afterwards. */
static void gt_template_string(const char *s, size_t n, GtBuf *out)
{
    const char *p = s + 1, *end = s + n - 1;
    bool first = true;

    gt_puts(out, "\"");
    while (p < end) {
        const char *w = p;

        if (isspace((unsigned char)*p)) { p++; continue; }
        if (!first) gt_puts(out, " ");
        first = false;
        if (*p == '.') {
            while (p < end && *p == '.') p++;
            gt_puts(out, ".");
            continue;
        }
        while (p < end && !isspace((unsigned char)*p) && *p != '.') p++;
        if (p == w) p++;   /* one code point no cell token takes, kept for §7.3's trash test */
        gt_put(out, w, (size_t)(p - w));
    }
    gt_puts(out, "\"");
}

/* The template value over the strings at `t[idx[0..k)]`, checked by §7.3's own parse; OWNED, NULL when invalid. */
static char *gt_areas_value(const GtTok *t, const int *idx, int k)
{
    GtBuf b = { NULL, 0, 0 };
    CssGridTemplateAreas a;
    int i;

    for (i = 0; i < k; i++) {
        gt_sep(&b);
        gt_template_string(t[idx[i]].s, t[idx[i]].n, &b);
    }
    if (!css_grid_template_areas_parse(b.p, &a)) {
        free(b.p);
        return NULL;
    }
    css_grid_template_areas_free(&a);
    return b.p;
}

/* ---- §7.7's `grid-auto-flow` ------------------------------------------------------------------------------ */

/* `[ row | column ] || dense` from tokens `[from, to)`, as `*column` and `*dense`. */
static bool gt_flow(const GtTok *t, int from, int to, bool *column, bool *dense)
{
    bool axis = false;
    int i;

    *column = *dense = false;
    if (to <= from) return false;
    for (i = from; i < to; i++) {
        if (t[i].kind != GT_TOK_WORD) return false;
        if (!*dense && gt_word_is(t[i].s, t[i].n, "dense")) { *dense = true; continue; }
        if (axis) return false;
        if (gt_word_is(t[i].s, t[i].n, "row")) axis = true;
        else if (gt_word_is(t[i].s, t[i].n, "column")) axis = *column = true;
        else return false;
    }
    return true;
}

/* The canonical `grid-auto-flow` value: CSSOM §6.7.2's shortest form, so `row` is omitted beside `dense`. */
static const char *gt_flow_name(bool column, bool dense)
{
    if (column) return dense ? "column dense" : "column";
    return dense ? "dense" : "row";
}

/* ---- the longhands ---------------------------------------------------------------------------------------- */

static int gt_longhand_index(const char *longhand)
{
    int i;

    for (i = 0; i < CSS_GRID_N; i++)
        if (strcmp(CSS_GRID_LONGHANDS[i], longhand) == 0) return i;
    return -1;
}

bool css_grid_template_validates_longhand(const char *longhand)
{
    DCHECK(longhand != NULL, "the grid longhand-grammar question was asked about no property");
    return gt_longhand_index(longhand) >= 0;
}

char *css_grid_template_longhand_value(const char *longhand, const char *value)
{
    GtTok t[GT_TOK_MAX];
    int which = gt_longhand_index(longhand), k, i, idx[GT_TOK_MAX];
    bool column, dense;

    DCHECK(which >= 0 && value != NULL, "a longhand this component does not own was routed to its grammar");
    k = gt_tokens(value, strlen(value), t);
    if (k <= 0) return NULL;
    switch (which) {
    case 0:
    case 1:
        return gt_track_list_value(t, 0, k, GT_LIST_TRACK, true);
    case 2:
        if (k == 1 && t[0].kind == GT_TOK_WORD && gt_word_is(t[0].s, t[0].n, "none")) return gt_dup("none");
        for (i = 0; i < k; i++) {
            if (t[i].kind != GT_TOK_STRING) return NULL;
            idx[i] = i;
        }
        return gt_areas_value(t, idx, k);
    case 3:
    case 4:
        return gt_track_list_value(t, 0, k, GT_LIST_AUTO, false);
    default:
        return gt_flow(t, 0, k, &column, &dense) ? gt_dup(gt_flow_name(column, dense)) : NULL;
    }
}

/* ---- §7.4's expansion ------------------------------------------------------------------------------------- */

/* The three template values of a `grid-template` over tokens `[from, to)`, into `out` (OWNED strings), or false
   for a value outside §7.4's grammar. */
static bool gt_template_expand(const GtTok *t, int from, int to, char *out[CSS_GRID_TEMPLATE_N])
{
    int i, slash = -1, strings = 0, idx[GT_TOK_MAX];
    GtBuf rows = { NULL, 0, 0 };
    bool pending_names = false;

    out[0] = out[1] = out[2] = NULL;
    if (to - from == 1 && t[from].kind == GT_TOK_WORD && gt_word_is(t[from].s, t[from].n, "none")) {
        out[0] = gt_dup("none");
        out[1] = gt_dup("none");
        out[2] = gt_dup("none");
        return true;
    }
    for (i = from; i < to; i++) {
        if (t[i].kind == GT_TOK_STRING) strings++;
        if (t[i].kind == GT_TOK_SLASH) {
            if (slash >= 0) return false;
            slash = i;
        }
    }
    if (strings == 0) {
        /* "<'grid-template-rows'> / <'grid-template-columns'>: Sets grid-template-rows and
           grid-template-columns to the specified values, respectively, and sets grid-template-areas to none." */
        if (slash < 0) return false;
        out[0] = gt_track_list_value(t, from, slash, GT_LIST_TRACK, true);
        out[1] = gt_track_list_value(t, slash + 1, to, GT_LIST_TRACK, true);
        out[2] = gt_dup("none");
        return out[0] != NULL && out[1] != NULL;
    }
    /* "[ <line-names>? <string> <track-size>? <line-names>? ]+ [ / <explicit-track-list> ]?": "Sets
       grid-template-areas to the strings listed. Sets grid-template-rows to the <track-size>s following each
       string (filling in auto for any missing sizes), and splicing in the named lines defined before/after
       each size. Sets grid-template-columns to the track listing specified after the slash (or none, if not
       specified)." A row's trailing names and the next row's leading ones are one line, so they are spliced
       into one list. */
    strings = 0;
    i = from;
    while (i < (slash >= 0 ? slash : to)) {
        int lim = slash >= 0 ? slash : to;
        bool fixed;

        if (t[i].kind == GT_TOK_NAMES) {
            if (!pending_names) { gt_sep(&rows); gt_puts(&rows, "["); }
            if (!gt_line_names(t[i].s, t[i].n, &rows, true)) goto bad;
            pending_names = true;
            i++;
        }
        if (i >= lim || t[i].kind != GT_TOK_STRING) goto bad;
        if (pending_names) { gt_puts(&rows, "]"); pending_names = false; }
        idx[strings++] = i++;
        gt_sep(&rows);
        if (i < lim && t[i].kind == GT_TOK_WORD) {
            if (t[i].n > 7 && strncasecmp(t[i].s, "repeat(", 7) == 0) goto bad;
            if (!gt_track_size(t[i].s, t[i].n, &rows, &fixed)) goto bad;
            i++;
        } else {
            gt_puts(&rows, "auto");
        }
        if (i < lim && t[i].kind == GT_TOK_NAMES) {
            gt_sep(&rows);
            gt_puts(&rows, "[");
            if (!gt_line_names(t[i].s, t[i].n, &rows, true)) goto bad;
            pending_names = true;
            i++;
        }
    }
    if (pending_names) gt_puts(&rows, "]");
    if (slash >= 0) {
        out[1] = gt_track_list_value(t, slash + 1, to, GT_LIST_EXPLICIT, false);
        if (out[1] == NULL) goto bad;
    } else {
        out[1] = gt_dup("none");
    }
    out[2] = gt_areas_value(t, idx, strings);
    if (out[2] == NULL) goto bad;
    out[0] = rows.p;
    return true;
bad:
    free(rows.p);
    free(out[1]);
    free(out[2]);
    out[1] = out[2] = NULL;
    return false;
}

/* §7.8's expansion over the whole value: `<'grid-template'> | <'grid-template-rows'> / [ auto-flow && dense? ]
   <'grid-auto-columns'>? | [ auto-flow && dense? ] <'grid-auto-rows'>? / <'grid-template-columns'>`. "Also
   sets all the implicit grid properties to their initial values" in the first form; in the other two, "the
   other sub-properties of grid are reset to their initial values". */
static bool gt_grid_expand(const GtTok *t, int k, char *out[CSS_GRID_N])
{
    int i, slash = -1, flow_at = -1;
    bool dense = false;

    for (i = 0; i < CSS_GRID_N; i++) out[i] = NULL;
    for (i = 0; i < k; i++) {
        if (t[i].kind == GT_TOK_SLASH && slash < 0) slash = i;
        if (t[i].kind == GT_TOK_WORD && gt_word_is(t[i].s, t[i].n, "auto-flow")) {
            if (flow_at >= 0) return false;
            flow_at = i;
        }
    }
    if (flow_at < 0) {
        if (!gt_template_expand(t, 0, k, out)) return false;
        out[3] = gt_dup("auto");
        out[4] = gt_dup("auto");
        out[5] = gt_dup("row");
        return true;
    }
    if (slash < 0) return false;
    if (flow_at < slash) {
        /* `[ auto-flow && dense? ] <'grid-auto-rows'>? / <'grid-template-columns'>`: row flow. */
        int from = 0;

        while (from < slash && t[from].kind == GT_TOK_WORD &&
               (from == flow_at || (!dense && gt_word_is(t[from].s, t[from].n, "dense")))) {
            if (from != flow_at) dense = true;
            from++;
        }
        if (from <= flow_at) return false;
        out[0] = gt_dup("none");
        out[1] = gt_track_list_value(t, slash + 1, k, GT_LIST_TRACK, true);
        out[3] = from < slash ? gt_track_list_value(t, from, slash, GT_LIST_AUTO, false) : gt_dup("auto");
        out[4] = gt_dup("auto");
        out[5] = gt_dup(gt_flow_name(false, dense));
    } else {
        /* `<'grid-template-rows'> / [ auto-flow && dense? ] <'grid-auto-columns'>?`: column flow. */
        int from = slash + 1;

        while (from < k && t[from].kind == GT_TOK_WORD &&
               (from == flow_at || (!dense && gt_word_is(t[from].s, t[from].n, "dense")))) {
            if (from != flow_at) dense = true;
            from++;
        }
        if (from <= flow_at) return false;
        out[0] = gt_track_list_value(t, 0, slash, GT_LIST_TRACK, true);
        out[1] = gt_dup("none");
        out[3] = gt_dup("auto");
        out[4] = from < k ? gt_track_list_value(t, from, k, GT_LIST_AUTO, false) : gt_dup("auto");
        out[5] = gt_dup(gt_flow_name(true, dense));
    }
    out[2] = gt_dup("none");
    for (i = 0; i < CSS_GRID_N; i++)
        if (out[i] == NULL) return false;
    return true;
}

char *css_grid_template_component(bool grid, const char *value, const char *longhand)
{
    GtTok t[GT_TOK_MAX];
    char *out[CSS_GRID_N] = { NULL, NULL, NULL, NULL, NULL, NULL }, *mine;
    int which = gt_longhand_index(longhand), k, i;
    bool ok;

    DCHECK(value != NULL && longhand != NULL, "a grid shorthand was expanded with no value or no longhand");
    if (which < 0 || (!grid && which >= CSS_GRID_TEMPLATE_N)) return NULL;
    /* css-cascade-5 §3 "Shorthand Properties": a CSS-wide keyword "sets all of its sub-properties to that
       keyword". */
    if (css_wide_keyword(value)) return gt_dup(value);
    k = gt_tokens(value, strlen(value), t);
    if (k <= 0) return NULL;
    ok = grid ? gt_grid_expand(t, k, out) : gt_template_expand(t, 0, k, out);
    mine = ok ? out[which] : NULL;
    for (i = 0; i < CSS_GRID_N; i++)
        if (i != which || !ok) free(out[i]);
    return mine;
}

/* ---- serialization ---------------------------------------------------------------------------------------- */

static bool gt_is(const char *v, const char *word) { return strcasecmp(v, word) == 0; }

/* §7.4's ASCII-art form from canonical rows, columns and areas, or NULL where the rows are not one size per
   string with names only between them, or the columns hold a repeat(). Line names between two sizes are written
   after the earlier row's size, the position the expansion splices them from. */
static char *gt_template_art(const char *rows, const char *cols, const char *areas)
{
    GtTok rt[GT_TOK_MAX], at[GT_TOK_MAX], ct[GT_TOK_MAX];
    int rk = gt_tokens(rows, strlen(rows), rt), ak = gt_tokens(areas, strlen(areas), at), i, s = 0, r = 0;
    GtBuf b = { NULL, 0, 0 };

    if (rk <= 0 || ak <= 0 || gt_is(rows, "none")) return NULL;
    for (i = 0; i < ak; i++)
        if (at[i].kind != GT_TOK_STRING) return NULL;
    while (r < rk) {
        if (rt[r].kind == GT_TOK_NAMES) {
            gt_sep(&b);
            gt_put(&b, rt[r].s, rt[r].n);
            r++;
            continue;
        }
        if (rt[r].kind != GT_TOK_WORD || s >= ak || strncasecmp(rt[r].s, "repeat(", 7) == 0) {
            free(b.p);
            return NULL;
        }
        gt_sep(&b);
        gt_put(&b, at[s].s, at[s].n);
        if (!gt_word_is(rt[r].s, rt[r].n, "auto")) {
            gt_sep(&b);
            gt_put(&b, rt[r].s, rt[r].n);
        }
        s++;
        r++;
    }
    if (s != ak) { free(b.p); return NULL; }
    if (!gt_is(cols, "none")) {
        int ck = gt_tokens(cols, strlen(cols), ct);

        for (i = 0; i < ck; i++)
            if (ct[i].kind == GT_TOK_WORD && strncasecmp(ct[i].s, "repeat(", 7) == 0) { free(b.p); return NULL; }
        gt_puts(&b, " / ");
        gt_puts(&b, cols);
    }
    return b.p;
}

static char *gt_template_serialize(const char *rows, const char *cols, const char *areas)
{
    GtBuf b = { NULL, 0, 0 };

    if (!gt_is(areas, "none")) return gt_template_art(rows, cols, areas);
    if (gt_is(rows, "none") && gt_is(cols, "none")) return gt_dup("none");
    gt_puts(&b, rows);
    gt_puts(&b, " / ");
    gt_puts(&b, cols);
    return b.p;
}

char *css_grid_template_value(bool grid, const char *const *v)
{
    GtBuf b = { NULL, 0, 0 };
    bool dense;

    DCHECK(v != NULL && v[0] != NULL && v[1] != NULL && v[2] != NULL &&
               (!grid || (v[3] != NULL && v[4] != NULL && v[5] != NULL)),
           "a grid shorthand was serialized with a longhand value missing");
    if (!grid || (gt_is(v[3], "auto") && gt_is(v[4], "auto") && gt_is(v[5], "row")))
        return gt_template_serialize(v[0], v[1], v[2]);
    if (!gt_is(v[2], "none")) return NULL;
    dense = strstr(v[5], "dense") != NULL;
    if (gt_is(v[0], "none") && gt_is(v[4], "auto") && (gt_is(v[5], "row") || gt_is(v[5], "dense"))) {
        gt_puts(&b, dense ? "auto-flow dense" : "auto-flow");
        if (!gt_is(v[3], "auto")) { gt_puts(&b, " "); gt_puts(&b, v[3]); }
        gt_puts(&b, " / ");
        gt_puts(&b, v[1]);
        return b.p;
    }
    if (gt_is(v[1], "none") && gt_is(v[3], "auto") && strncasecmp(v[5], "column", 6) == 0) {
        gt_puts(&b, v[0]);
        gt_puts(&b, dense ? " / auto-flow dense" : " / auto-flow");
        if (!gt_is(v[4], "auto")) { gt_puts(&b, " "); gt_puts(&b, v[4]); }
        return b.p;
    }
    return NULL;
}
