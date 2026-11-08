/* CSS Animations Level 1 §4 "Declaring Animations". See css_animation_shorthand.h. */
#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "check.h"
#include "core/css/css_animation_shorthand.h"
#include "core/css/css_defaulting.h"
#include "core/css/css_shorthand.h"

enum { AN_DURATION, AN_TIMING, AN_DELAY, AN_COUNT, AN_DIRECTION, AN_FILL, AN_PLAY, AN_NAME };

const char *const CSS_ANIMATION_SHORTHAND_LONGHANDS[CSS_ANIMATION_SHORTHAND_N] = {
    "animation-duration", "animation-timing-function", "animation-delay", "animation-iteration-count",
    "animation-direction", "animation-fill-mode", "animation-play-state", "animation-name",
};

/* Each longhand's `Initial:` line (§4.1-§4.8). */
static const char *const AN_INITIAL[CSS_ANIMATION_SHORTHAND_N] = {
    "0s", "ease", "0s", "1", "normal", "none", "running", "none",
};

static const char *const KW_DIRECTION[] = { "normal", "reverse", "alternate", "alternate-reverse", NULL };
static const char *const KW_FILL[] = { "none", "forwards", "backwards", "both", NULL };
static const char *const KW_PLAY[] = { "running", "paused", NULL };
/* CSS Easing Level 2 §2 "Easing functions"' keyword forms: `linear`, §2.2's four cubic Bézier keywords and
   §2.3's two step keywords. */
static const char *const KW_EASING[] = { "linear", "ease", "ease-in", "ease-out", "ease-in-out", "step-start",
                                         "step-end", NULL };
static const char *const KW_STEP_POSITION[] = { "jump-start", "jump-end", "jump-none", "jump-both", "start", "end",
                                                NULL };

static int an_index(const char *longhand)
{
    int i;

    for (i = 0; i < CSS_ANIMATION_SHORTHAND_N; i++)
        if (strcmp(CSS_ANIMATION_SHORTHAND_LONGHANDS[i], longhand) == 0) return i;
    return -1;
}

static char *an_strndup(const char *s, size_t n)
{
    char *d = malloc(n + 1);

    CHECK(d != NULL, "CSS Animations: OOM copying a value");
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static bool span_is(const char *w, size_t n, const char *kw)
{
    return strlen(kw) == n && strncasecmp(w, kw, n) == 0;
}

static int span_keyword(const char *w, size_t n, const char *const *set)
{
    int i;

    for (i = 0; set[i]; i++)
        if (span_is(w, n, set[i])) return i;
    return -1;
}

/* css-values-4 §7.2 "Duration Units: the <time> type and s, ms units": a number followed by `s` or `ms`. */
static bool span_time(const char *w, size_t n, double *ms)
{
    double v = 0;
    size_t u;

    if (n > 2 && strncasecmp(w + n - 2, "ms", 2) == 0) u = 2;
    else if (n > 1 && (w[n - 1] == 's' || w[n - 1] == 'S')) u = 1;
    else return false;
    if (!css_shorthand_number(w, n - u, &v)) return false;
    *ms = u == 2 ? v : v * 1000.0;
    return true;
}

/* Split `inside` (the text between a function's parentheses) on top-level commas into at most `max` spans,
   trimmed. Answers the count, or -1 for more than `max` or an empty argument. */
static int fn_args(const char *inside, size_t n, const char **a, size_t *al, int max)
{
    size_t i = 0, start = 0;
    int k = 0, depth = 0;

    for (;; i++) {
        if (i == n || (inside[i] == ',' && depth == 0)) {
            size_t s = start, e = i;

            while (s < e && isspace((unsigned char)inside[s])) s++;
            while (e > s && isspace((unsigned char)inside[e - 1])) e--;
            if (e == s || k == max) return -1;
            a[k] = inside + s;
            al[k++] = e - s;
            if (i == n) return k;
            start = i + 1;
        } else if (inside[i] == '(') {
            depth++;
        } else if (inside[i] == ')') {
            depth--;
        }
    }
}

static bool span_number(const char *w, size_t n, double *v) { return css_shorthand_number(w, n, v); }

static bool span_percentage(const char *w, size_t n)
{
    double v;

    return n > 1 && w[n - 1] == '%' && css_shorthand_number(w, n - 1, &v);
}

/* CSS Easing Level 2 §2's `<easing-function>`. */
static bool span_easing(const char *w, size_t n)
{
    const char *a[64], *in;
    size_t al[64], fl, inl;
    int k, i;
    double x;

    if (span_keyword(w, n, KW_EASING) >= 0) return true;
    in = memchr(w, '(', n);
    if (!in || w[n - 1] != ')') return false;
    fl = (size_t)(in - w);
    in++;
    inl = n - fl - 2;
    if (span_is(w, fl, "cubic-bezier")) {
        /* §2.2: `cubic-bezier( [ <number [0,1]>, <number> ]#{2} )`. */
        if (fn_args(in, inl, a, al, 4) != 4) return false;
        for (i = 0; i < 4; i++) {
            if (!span_number(a[i], al[i], &x)) return false;
            if ((i == 0 || i == 2) && (x < 0 || x > 1)) return false;
        }
        return true;
    }
    if (span_is(w, fl, "steps")) {
        /* §2.3: `steps( <integer>, <step-position>?)`, at least 2 for jump-none and at least 1 otherwise. */
        int pos = -1;

        k = fn_args(in, inl, a, al, 2);
        if (k < 1 || !span_number(a[0], al[0], &x) || x != floor(x)) return false;
        if (k == 2 && (pos = span_keyword(a[1], al[1], KW_STEP_POSITION)) < 0) return false;
        return x >= (pos == 2 ? 2 : 1);
    }
    if (span_is(w, fl, "linear")) {
        /* §2.1: `linear( [ <number> && <percentage>{0,2} ]# )`. */
        k = fn_args(in, inl, a, al, 64);
        if (k < 1) return false;
        for (i = 0; i < k; i++) {
            const char *pw[4];
            size_t pl[4];
            char *item = an_strndup(a[i], al[i]);
            int m = css_shorthand_components(item, pw, pl, 3), j, nums = 0, pcts = 0;

            for (j = 0; j < m; j++) {
                if (span_percentage(pw[j], pl[j])) pcts++;
                else if (span_number(pw[j], pl[j], &x)) nums++;
                else nums = 2;
            }
            free(item);
            if (m < 1 || nums != 1 || pcts > 2) return false;
        }
        return true;
    }
    return false;
}

/* css-values-4 §4.2 "Author-defined Identifiers: the <custom-ident> type", which CSS Animations §4.1 narrows
   further by excluding `none`: an identifier that is not a CSS-wide keyword and not `default`. */
static bool span_custom_ident(const char *w, size_t n)
{
    size_t i;
    unsigned char c0 = (unsigned char)w[0];

    if (n == 0 || isdigit(c0) || (c0 == '-' && n > 1 && isdigit((unsigned char)w[1])) || (c0 == '-' && n == 1))
        return false;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)w[i];

        if (!(isalnum(c) || c == '-' || c == '_' || c >= 0x80)) return false;
    }
    if (span_is(w, n, "none") || span_is(w, n, "default")) return false;
    {
        char *s = an_strndup(w, n);
        bool wide = css_wide_keyword(s);

        free(s);
        return !wide;
    }
}

/* §4.1's `[ none | <keyframes-name> ]`, `<keyframes-name>` being `<custom-ident> | <string>`. */
static bool span_name(const char *w, size_t n)
{
    return span_is(w, n, "none") || span_custom_ident(w, n) || css_shorthand_string(w, n);
}

/* Does one component match the longhand's single-item grammar? */
static bool span_item(int idx, const char *w, size_t n)
{
    double v;

    switch (idx) {
    case AN_DURATION: return span_time(w, n, &v) && v >= 0;   /* `<time [0s,∞]>` */
    case AN_TIMING:   return span_easing(w, n);
    case AN_DELAY:    return span_time(w, n, &v);
    case AN_COUNT:    return span_is(w, n, "infinite") || (span_number(w, n, &v) && v >= 0);
    case AN_DIRECTION: return span_keyword(w, n, KW_DIRECTION) >= 0;
    case AN_FILL:     return span_keyword(w, n, KW_FILL) >= 0;
    case AN_PLAY:     return span_keyword(w, n, KW_PLAY) >= 0;
    case AN_NAME:     return span_name(w, n);
    default:          DFAIL("a CSS Animations item was asked of a longhand outside the eight");
                      return false;
    }
}

/* An item as this component serializes it: a keyword and a time's unit in lowercase, a `<keyframes-name>`,
   a number and an easing function's arguments as written. OWNED. */
static char *item_text(int idx, const char *w, size_t n)
{
    char *s = an_strndup(w, n);
    size_t i, lower = 0, from = 0;

    if (idx == AN_DURATION || idx == AN_DELAY) {
        for (from = n; from > 0 && isalpha((unsigned char)s[from - 1]); from--) {}
        lower = n;
    } else if (idx == AN_NAME || idx == AN_COUNT) {
        if (span_is(w, n, "none") || span_is(w, n, "infinite")) lower = n;
    } else if (idx == AN_TIMING) {
        const char *paren = memchr(w, '(', n);

        lower = paren ? (size_t)(paren - w) : n;
    } else {
        lower = n;
    }
    for (i = from; i < lower; i++) s[i] = (char)tolower((unsigned char)s[i]);
    return s;
}

/* The next top-level comma-separated group of `text`. */
static bool next_group(const char *text, size_t len, size_t *pos, const char **out, size_t *out_len)
{
    size_t i;
    int depth = 0;
    char quote = 0;

    if (*pos > len) return false;
    for (i = *pos; i < len; i++) {
        char c = text[i];

        if (quote) {
            if (c == '\\' && i + 1 < len) i++;
            else if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '(') {
            depth++;
        } else if (c == ')') {
            if (depth > 0) depth--;
        } else if (c == ',' && depth == 0) {
            break;
        }
    }
    *out = text + *pos;
    *out_len = i - *pos;
    *pos = i + 1;
    return true;
}

/* §4.9's `<single-animation>`, one group: each component goes to the first term it can be that is not yet
   set, in the grammar's order — so the first `<time>` is the duration and the second the delay, and a keyword
   another property admits goes to that property while it is unset, before `animation-name` (§4.9's two
   "Order is important" paragraphs). False for a group outside the grammar. */
static bool single_animation(const char *g, size_t gl, char **slot)
{
    const char *w[CSS_ANIMATION_SHORTHAND_N + 1];
    size_t wl[CSS_ANIMATION_SHORTHAND_N + 1];
    char *copy = an_strndup(g, gl);
    int n = css_shorthand_components(copy, w, wl, CSS_ANIMATION_SHORTHAND_N), i, t;
    bool ok = n >= 1;

    for (i = 0; ok && i < n; i++) {
        static const int ORDER[] = { AN_DURATION, AN_DELAY, AN_TIMING, AN_COUNT, AN_DIRECTION, AN_FILL, AN_PLAY,
                                     AN_NAME };
        bool placed = false;

        for (t = 0; t < CSS_ANIMATION_SHORTHAND_N && !placed; t++) {
            int idx = ORDER[t];

            if (slot[idx] || !span_item(idx, w[i], wl[i])) continue;
            slot[idx] = item_text(idx, w[i], wl[i]);
            placed = true;
        }
        ok = placed;
    }
    free(copy);
    return ok;
}

static void slots_free(char **slot)
{
    int i;

    for (i = 0; i < CSS_ANIMATION_SHORTHAND_N; i++) { free(slot[i]); slot[i] = NULL; }
}

/* A growable string. */
typedef struct { char *s; size_t n, cap; } AnBuf;

static void buf_add(AnBuf *b, const char *t)
{
    size_t k = strlen(t);

    if (b->n + k + 1 > b->cap) {
        b->cap = (b->n + k + 1) * 2;
        b->s = realloc(b->s, b->cap);
        CHECK(b->s != NULL, "CSS Animations: OOM serializing a value");
    }
    memcpy(b->s + b->n, t, k + 1);
    b->n += k;
}

char *css_animation_shorthand_component(const char *value, const char *longhand)
{
    int idx = an_index(longhand);
    size_t len, pos = 0;
    const char *g;
    size_t gl;
    AnBuf b = { NULL, 0, 0 };
    char *slot[CSS_ANIMATION_SHORTHAND_N] = { NULL };

    DCHECK(value != NULL, "an `animation` declaration was expanded with no value");
    if (idx < 0) return NULL;
    /* CSS Cascade's CSS-wide keyword sets every longhand to itself, ahead of §4.9's own grammar. */
    if (css_wide_keyword(value)) return an_strndup(value, strlen(value));
    len = strlen(value);
    buf_add(&b, "");
    while (next_group(value, len, &pos, &g, &gl)) {
        if (!single_animation(g, gl, slot)) {
            slots_free(slot);
            free(b.s);
            return NULL;
        }
        if (b.n) buf_add(&b, ", ");
        buf_add(&b, slot[idx] ? slot[idx] : AN_INITIAL[idx]);
        slots_free(slot);
    }
    return b.s;
}

/* Split a longhand's list into its items, into `items` (each OWNED). Answers the count, -1 when too many. */
static int list_items(const char *v, char **items, int max)
{
    size_t len = strlen(v), pos = 0, gl;
    const char *g;
    int k = 0;

    while (next_group(v, len, &pos, &g, &gl)) {
        while (gl && isspace((unsigned char)*g)) { g++; gl--; }
        while (gl && isspace((unsigned char)g[gl - 1])) gl--;
        if (k == max) { while (k) free(items[--k]); return -1; }
        items[k++] = an_strndup(g, gl);
    }
    return k;
}

#define AN_LIST_MAX 64

char *css_animation_shorthand_value(const char *const *values)
{
    char *items[CSS_ANIMATION_SHORTHAND_N][AN_LIST_MAX];
    int count[CSS_ANIMATION_SHORTHAND_N], i, j, wide = 0;
    AnBuf b = { NULL, 0, 0 };
    bool ok = true;

    for (i = 0; i < CSS_ANIMATION_SHORTHAND_N; i++) {
        DCHECK(values[i] != NULL, "the `animation` shorthand was serialized with a longhand missing");
        wide += css_wide_keyword(values[i]);
    }
    if (wide) {
        for (i = 1; i < CSS_ANIMATION_SHORTHAND_N; i++)
            if (strcasecmp(values[i], values[0]) != 0) return NULL;
        return wide == CSS_ANIMATION_SHORTHAND_N ? an_strndup(values[0], strlen(values[0])) : NULL;
    }
    for (i = 0; i < CSS_ANIMATION_SHORTHAND_N; i++) {
        count[i] = list_items(values[i], items[i], AN_LIST_MAX);
        if (count[i] < 0 || count[i] != count[0]) ok = false;
    }
    /* §4.9 serializes every term: a default may be output in any case, and writing all eight is what keeps an
       `animation-name` that spells another property's keyword distinguishable. */
    buf_add(&b, "");
    for (j = 0; ok && j < count[0]; j++) {
        if (j) buf_add(&b, ", ");
        for (i = 0; i < CSS_ANIMATION_SHORTHAND_N; i++) {
            if (i) buf_add(&b, " ");
            buf_add(&b, items[i][j]);
        }
    }
    for (i = 0; i < CSS_ANIMATION_SHORTHAND_N; i++)
        for (j = 0; j < (count[i] > 0 ? count[i] : 0); j++) free(items[i][j]);
    if (!ok) { free(b.s); return NULL; }
    return b.s;
}

bool css_animation_shorthand_validates_longhand(const char *longhand)
{
    return an_index(longhand) >= 0;
}

char *css_animation_shorthand_longhand_value(const char *longhand, const char *value)
{
    int idx = an_index(longhand);
    size_t len, pos = 0, gl;
    const char *g;
    AnBuf b = { NULL, 0, 0 };

    DCHECK(idx >= 0, "a CSS Animations longhand value was asked for a name that is not one of the eight");
    if (css_wide_keyword(value)) return an_strndup(value, strlen(value));
    len = strlen(value);
    buf_add(&b, "");
    while (next_group(value, len, &pos, &g, &gl)) {
        const char *w[2];
        size_t wl[2];
        char *copy = an_strndup(g, gl), *t;
        int n = css_shorthand_components(copy, w, wl, 1);

        if (n != 1 || !span_item(idx, w[0], wl[0])) { free(copy); free(b.s); return NULL; }
        t = item_text(idx, w[0], wl[0]);
        if (b.n) buf_add(&b, ", ");
        buf_add(&b, t);
        free(t);
        free(copy);
    }
    return b.s;
}

const char *css_animation_shorthand_initial(const char *longhand)
{
    int idx = an_index(longhand);

    return idx < 0 ? NULL : AN_INITIAL[idx];
}

bool css_animation_time_ms(const char *item, double *ms)
{
    return span_time(item, strlen(item), ms);
}

char *css_animation_shorthand_computed(const char *longhand, char *specified)
{
    int idx = an_index(longhand);
    char *items[AN_LIST_MAX];
    int n, i;
    AnBuf b = { NULL, 0, 0 };

    DCHECK(idx >= 0, "a CSS Animations computed value was asked for a name that is not one of the eight");
    DCHECK(specified != NULL, "a CSS Animations longhand reached its computed value with no specified value");
    if (idx != AN_DURATION && idx != AN_DELAY) return specified;
    /* "list, each item a duration": the time, in its canonical unit. */
    n = list_items(specified, items, AN_LIST_MAX);
    DCHECK(n > 0, "a specified `<time>` list held no item — §4's grammar admits no empty list");
    buf_add(&b, "");
    for (i = 0; i < n; i++) {
        double ms = 0;
        char num[48];

        bool parsed = css_animation_time_ms(items[i], &ms);

        DCHECK(parsed, "a specified `<time>` item did not parse as one — this component validated it when the "
                       "declaration was taken");
        snprintf(num, sizeof num, "%.15gs", ms / 1000.0);
        if (i) buf_add(&b, ", ");
        buf_add(&b, num);
        free(items[i]);
    }
    free(specified);
    return b.s;
}
