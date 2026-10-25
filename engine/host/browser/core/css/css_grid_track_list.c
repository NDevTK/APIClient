/* css-grid-1 §7.2's `<track-list>`. See css_grid_track_list.h. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "check.h"
#include "core/css/css_grid_track_list.h"
#include <lexbor/core/base.h>   /* lexbor_realloc: the allocator the engine gives its refusal edge */

/* The end of the component value starting at `s` and ending by `end`: a run outside parentheses and brackets,
   ended by white space at depth zero. An unbalanced closer ends the value with an error, reported as NULL. */
static const char *gtl_component_end(const char *s, const char *end)
{
    int depth = 0;

    for (; s < end && *s != '\0'; s++) {
        if (*s == '(' || *s == '[') depth++;
        else if (*s == ')' || *s == ']') { if (--depth < 0) return NULL; }
        else if (depth == 0 && isspace((unsigned char)*s)) break;
    }
    return depth == 0 ? s : NULL;
}

/* A `<flex>`: a CSS number, then the unit `fr` (ASCII case-insensitive). False when `t` is not one. The
   number's grammar is css-syntax-3 §4.3.12 "Consume a number": digits, an optional fraction and an optional
   exponent, with an optional sign; `strtod`'s other spellings (hexadecimal, `inf`, `nan`) are refused. */
static bool gtl_flex(const char *t, size_t n, double *out)
{
    char buf[64], *end;
    size_t i = 0;

    if (n < 3 || n >= sizeof buf || strncasecmp(t + n - 2, "fr", 2) != 0) return false;
    memcpy(buf, t, n - 2);
    buf[n - 2] = '\0';
    if (buf[i] == '+' || buf[i] == '-') i++;
    if (!(isdigit((unsigned char)buf[i]) || (buf[i] == '.' && isdigit((unsigned char)buf[i + 1])))) return false;
    for (; buf[i] != '\0'; i++)
        if (!isdigit((unsigned char)buf[i]) && buf[i] != '.' && buf[i] != 'e' && buf[i] != 'E' &&
            buf[i] != '+' && buf[i] != '-')
            return false;
    *out = strtod(buf, &end);
    return *end == '\0';
}

static bool gtl_push(CssGridTrackList *l, size_t *cap, CssGridTrackSize t)
{
    if (l->n == *cap) {
        size_t c = *cap ? *cap * 2 : 4;
        CssGridTrackSize *grown = lexbor_realloc(l->tracks, c * sizeof *grown);

        CHECK(grown != NULL, "css-grid-1 §7.2: out of memory holding a track list");
        l->tracks = grown;
        *cap = c;
    }
    l->tracks[l->n++] = t;
    return true;
}

void css_grid_track_list_free(CssGridTrackList *list)
{
    DCHECK(list != NULL, "a track list was released with no list");
    free(list->tracks);
    list->tracks = NULL;
    list->n = list->auto_at = list->auto_n = 0;
    list->auto_fit = false;
}

/* A `<track-breadth>` (`inflexible` false) or an `<inflexible-breadth>` over the `n` bytes at `t`. False for
   text outside it. */
static bool gtl_breadth(JSContext *realm, const CssFontMetrics *font, const char *t, size_t n, bool inflexible,
                        CssGridBreadth *out)
{
    char *copy;
    bool ok;

    out->length.kind = CSS_LENGTH_KEYWORD;
    out->length.px = css_px(0.0);
    out->length.pct = 0.0;
    out->length.keyword = CSS_KEYWORD_AUTO;
    out->flex = 0.0;
    if (n == 4 && strncasecmp(t, "auto", 4) == 0) { out->kind = CSS_GRID_BREADTH_AUTO; return true; }
    if (n == 11 && strncasecmp(t, "min-content", 11) == 0) { out->kind = CSS_GRID_BREADTH_MIN_CONTENT; return true; }
    if (n == 11 && strncasecmp(t, "max-content", 11) == 0) { out->kind = CSS_GRID_BREADTH_MAX_CONTENT; return true; }
    if (gtl_flex(t, n, &out->flex)) {
        /* §7.2.1: "<flex [0,∞]> A non-negative dimension with the unit fr", and "As a maximum, a <flex> value
           sets the track's flex factor; it is invalid as a minimum." */
        out->kind = CSS_GRID_BREADTH_FLEX;
        return !inflexible && out->flex >= 0.0;
    }
    copy = malloc(n + 1);
    CHECK(copy != NULL, "css-grid-1 §7.2: out of memory copying one track breadth");
    memcpy(copy, t, n);
    copy[n] = '\0';
    out->kind = CSS_GRID_BREADTH_LENGTH;
    /* `<length-percentage [0,∞]>`: no keyword, and a negative literal is outside the range. */
    ok = css_length_parse(realm, font, copy, CSS_KEYWORDS_NONE, &out->length) &&
         !(out->length.kind == CSS_LENGTH_ABSOLUTE && out->length.px.px < 0.0) &&
         !(out->length.kind == CSS_LENGTH_PERCENTAGE && out->length.pct < 0.0);
    free(copy);
    return ok;
}

/* The `n` bytes at `t` with ASCII white space trimmed from both ends, as a start and a length. */
static const char *gtl_trim(const char *t, size_t *n)
{
    while (*n > 0 && isspace((unsigned char)t[0])) { t++; (*n)--; }
    while (*n > 0 && isspace((unsigned char)t[*n - 1])) (*n)--;
    return t;
}

/* A `<track-size>` over the `n` bytes at `s`: `<track-breadth> | minmax( <inflexible-breadth> , <track-breadth> )
   | fit-content( <length-percentage [0,∞]> )`. False for text outside it. */
static bool gtl_track_size(JSContext *realm, const CssFontMetrics *font, const char *s, size_t n,
                           CssGridTrackSize *out)
{
    if (n > 8 && strncasecmp(s, "minmax(", 7) == 0 && s[n - 1] == ')') {
        const char *a = s + 7, *comma = NULL, *b;
        size_t an, bn, i;
        int depth = 0;

        for (i = 7; i < n - 1; i++) {
            if (s[i] == '(') depth++;
            else if (s[i] == ')') depth--;
            else if (s[i] == ',' && depth == 0) { comma = s + i; break; }
        }
        if (comma == NULL) return false;
        an = (size_t)(comma - a);
        a = gtl_trim(a, &an);
        b = comma + 1;
        bn = (size_t)(s + n - 1 - b);
        b = gtl_trim(b, &bn);
        return an > 0 && bn > 0 && gtl_breadth(realm, font, a, an, true, &out->min) &&
               gtl_breadth(realm, font, b, bn, false, &out->max);
    }
    if (n > 13 && strncasecmp(s, "fit-content(", 12) == 0 && s[n - 1] == ')') {
        const char *a = s + 12;
        size_t an = n - 13;

        a = gtl_trim(a, &an);
        if (an == 0 || !gtl_breadth(realm, font, a, an, true, &out->max) || out->max.kind != CSS_GRID_BREADTH_LENGTH)
            return false;
        out->max.kind = CSS_GRID_BREADTH_FIT_CONTENT;
        (void)gtl_breadth(realm, font, "auto", 4, true, &out->min);
        return true;
    }
    if (!gtl_breadth(realm, font, s, n, false, &out->max)) return false;
    if (out->max.kind == CSS_GRID_BREADTH_FLEX) return gtl_breadth(realm, font, "auto", 4, true, &out->min);
    out->min = out->max;
    return true;
}

/* §7.2's `<fixed-size>`: "<fixed-breadth> | minmax( <fixed-breadth> , <track-breadth> ) | minmax(
   <inflexible-breadth> , <fixed-breadth> )", so a track with a `<length-percentage>` on either side. */
static bool gtl_fixed_size(const CssGridTrackSize *t)
{
    return t->min.kind == CSS_GRID_BREADTH_LENGTH || t->max.kind == CSS_GRID_BREADTH_LENGTH;
}

/* The `<track-size>`s of the space-separated `n` bytes at `s`, appended to `out`. False for text outside the
   grammar; a line name is refused by name. */
static bool gtl_track_sizes(JSContext *realm, const CssFontMetrics *font, const char *text, const char *s, size_t n,
                            CssGridTrackList *out, size_t *cap)
{
    const char *end = s + n;

    while (s < end && isspace((unsigned char)*s)) s++;
    while (s < end) {
        const char *e = gtl_component_end(s, end);
        CssGridTrackSize t;

        if (e == NULL) return false;
        if (s[0] == '[')
            DFAILF("css-grid-1 §7.2.2 \"Named Grid Lines: the [<custom-ident>*] syntax\" in the track list "
                   "`%s`. BUILD line names", text);
        if (!gtl_track_size(realm, font, s, (size_t)(e - s), &t)) return false;
        gtl_push(out, cap, t);
        s = e;
        while (s < end && isspace((unsigned char)*s)) s++;
    }
    return true;
}

/* A `repeat()` over the `n` bytes at `s`, appended to `out`: §7.2.3.1's `repeat( [ <integer [1,∞]> | auto-fill |
   auto-fit ] , [ <line-names>? <track-size> ]+ <line-names>? )`. An integer count is expanded; an automatic one is
   recorded once, and "can only appear once in the track list". False for text outside the grammar. */
static bool gtl_repeat(JSContext *realm, const CssFontMetrics *font, const char *text, const char *s, size_t n,
                       CssGridTrackList *out, size_t *cap)
{
    const char *a = s + 7, *comma = memchr(s + 7, ',', n - 8), *body;
    size_t an, bn, first, m, k, count = 0, i;
    char *endp, num[32];
    bool autorep = false, fit = false;

    if (comma == NULL) return false;
    an = (size_t)(comma - a);
    a = gtl_trim(a, &an);
    body = comma + 1;
    bn = (size_t)(s + n - 1 - body);
    if (an == 9 && strncasecmp(a, "auto-fill", 9) == 0) {
        autorep = true;
    } else if (an == 8 && strncasecmp(a, "auto-fit", 8) == 0) {
        autorep = fit = true;
    } else {
        if (an == 0 || an >= sizeof num) return false;
        for (i = 0; i < an; i++)
            if (!isdigit((unsigned char)a[i]) && !(i == 0 && a[i] == '+')) return false;
        memcpy(num, a, an);
        num[an] = '\0';
        count = (size_t)strtoull(num, &endp, 10);
        if (*endp != '\0' || count < 1) return false;
    }
    first = out->n;
    if (!gtl_track_sizes(realm, font, text, body, bn, out, cap) || out->n == first) return false;
    if (autorep) {
        if (out->auto_n != 0) return false;
        out->auto_at = first;
        out->auto_n = out->n - first;
        out->auto_fit = fit;
        return true;
    }
    m = out->n - first;
    for (k = 1; k < count; k++)
        for (i = 0; i < m; i++) gtl_push(out, cap, out->tracks[first + i]);
    return true;
}

bool css_grid_track_list_parse(JSContext *realm, const CssFontMetrics *font, const char *text,
                               CssGridTrackList *out)
{
    size_t cap = 0;
    const char *s = text;

    DCHECK(text != NULL && out != NULL, "a track list was parsed from nothing or into nothing");
    out->tracks = NULL;
    out->n = out->auto_at = out->auto_n = 0;
    out->auto_fit = false;
    while (isspace((unsigned char)*s)) s++;
    if (strcasecmp(s, "none") == 0) return false;
    while (*s != '\0') {
        const char *e = gtl_component_end(s, s + strlen(s));
        size_t n;
        CssGridTrackSize t;
        bool ok;

        if (e == NULL) { css_grid_track_list_free(out); return false; }
        n = (size_t)(e - s);
        if (s[0] == '[')
            DFAILF("css-grid-1 §7.2.2 \"Named Grid Lines: the [<custom-ident>*] syntax\" in the track list "
                   "`%s`. BUILD line names", text);
        if (n > 8 && strncasecmp(s, "repeat(", 7) == 0 && s[n - 1] == ')') {
            ok = gtl_repeat(realm, font, text, s, n, out, &cap);
        } else {
            ok = gtl_track_size(realm, font, s, n, &t);
            if (ok) gtl_push(out, &cap, t);
        }
        if (!ok) { css_grid_track_list_free(out); return false; }
        s = e;
        while (isspace((unsigned char)*s)) s++;
    }
    if (out->n == 0) return false;
    /* §7.2's `<auto-track-list>`: beside an `<auto-repeat>` every track is a `<fixed-size>`, the repeated ones
       included, since "requires definite track sizes so that the number of repetitions can be calculated". */
    if (out->auto_n != 0) {
        size_t i;

        for (i = 0; i < out->n; i++)
            if (!gtl_fixed_size(&out->tracks[i])) { css_grid_track_list_free(out); return false; }
    }
    return true;
}
