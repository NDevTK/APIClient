/* css-grid-1 §7.2's `<track-list>`. See css_grid_track_list.h. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "check.h"
#include "core/css/css_grid_track_list.h"

/* The end of the component value starting at `s`: a run outside parentheses and brackets, ended by white space
   at depth zero. An unbalanced closer ends the value with an error, reported as NULL. */
static const char *gtl_component_end(const char *s)
{
    int depth = 0;

    for (; *s != '\0'; s++) {
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
        CssGridTrackSize *grown = realloc(l->tracks, c * sizeof *grown);

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
    list->n = 0;
}

bool css_grid_track_list_parse(JSContext *realm, const CssFontMetrics *font, const char *text,
                               CssGridTrackList *out)
{
    size_t cap = 0;
    const char *s = text;

    DCHECK(text != NULL && out != NULL, "a track list was parsed from nothing or into nothing");
    out->tracks = NULL;
    out->n = 0;
    while (isspace((unsigned char)*s)) s++;
    if (strcasecmp(s, "none") == 0) return false;
    while (*s != '\0') {
        const char *e = gtl_component_end(s);
        size_t n;
        CssGridTrackSize t;
        char *copy;
        bool ok;

        if (e == NULL) { css_grid_track_list_free(out); return false; }
        n = (size_t)(e - s);
        t.kind = CSS_GRID_TRACK_AUTO;
        t.length.kind = CSS_LENGTH_KEYWORD;
        t.length.px = css_px(0.0);
        t.length.pct = 0.0;
        t.length.keyword = CSS_KEYWORD_AUTO;
        t.flex = 0.0;
        if (s[0] == '[')
            DFAILF("css-grid-1 §7.2.2 \"Named Grid Lines: the [<custom-ident>*] syntax\" in the track list "
                   "`%s`. BUILD line names", text);
        if ((n > 7 && strncasecmp(s, "minmax(", 7) == 0) || (n > 12 && strncasecmp(s, "fit-content(", 12) == 0) ||
            (n > 7 && strncasecmp(s, "repeat(", 7) == 0) ||
            (n == 11 && (strncasecmp(s, "min-content", 11) == 0 || strncasecmp(s, "max-content", 11) == 0)))
            DFAILF("the track list `%s` uses a css-grid-1 §7.2 `<track-size>` or `<track-repeat>` that is not a "
                   "bare `<length-percentage>`, `<flex>` or `auto`. BUILD it as a track's min and max sizing "
                   "functions", text);
        if (n == 4 && strncasecmp(s, "auto", 4) == 0) {
            ok = true;
        } else if (gtl_flex(s, n, &t.flex)) {
            /* §7.2.1: "<flex [0,∞]> A non-negative dimension with the unit fr". */
            t.kind = CSS_GRID_TRACK_FLEX;
            ok = t.flex >= 0.0;
        } else {
            copy = malloc(n + 1);
            CHECK(copy != NULL, "css-grid-1 §7.2: out of memory copying one track size");
            memcpy(copy, s, n);
            copy[n] = '\0';
            t.kind = CSS_GRID_TRACK_BREADTH;
            /* `<length-percentage [0,∞]>`: no keyword, and a negative literal is outside the range. */
            ok = css_length_parse(realm, font, copy, CSS_KEYWORDS_NONE, &t.length) &&
                 !(t.length.kind == CSS_LENGTH_ABSOLUTE && t.length.px.px < 0.0) &&
                 !(t.length.kind == CSS_LENGTH_PERCENTAGE && t.length.pct < 0.0);
            free(copy);
        }
        if (!ok) { css_grid_track_list_free(out); return false; }
        gtl_push(out, &cap, t);
        s = e;
        while (isspace((unsigned char)*s)) s++;
    }
    if (out->n == 0) return false;
    return true;
}
