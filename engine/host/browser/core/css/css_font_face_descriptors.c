/* css-fonts-4 §4.4 "Font property descriptors: the font-style, font-weight, and font-width descriptors". See
   css_font_face_descriptors.h. */
#include <stdlib.h>
#include <string.h>

#include <lexbor/core/base.h>   /* lexbor_malloc: the allocator the engine gives its refusal edge */

#include "check.h"
#include "core/css/css_font_face_descriptors.h"

enum { FD_MAX_COMPONENTS = 3, FD_COMPONENT_BYTES = 40 };

/* `value` split at white space into at most three lower-cased components. False for more, for a component
   too long to be a keyword or a number, or for a function, which no grammar here admits unevaluated. */
static bool fd_split(const char *value, char out[FD_MAX_COMPONENTS][FD_COMPONENT_BYTES], int *n)
{
    const char *p = value;

    *n = 0;
    for (;;) {
        size_t len = 0;

        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f') p++;
        if (*p == '\0') return *n > 0;
        if (*n == FD_MAX_COMPONENTS) return false;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '\f') {
            char c = *p++;

            if (c == '(' || len + 1 == FD_COMPONENT_BYTES) return false;
            out[*n][len++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        out[*n][len] = '\0';
        (*n)++;
    }
}

/* A CSS number with the unit `unit` (the empty string for none) wholly spelling `s`. */
static bool fd_number(const char *s, const char *unit, double *out)
{
    char *end = NULL;
    double v;

    if (!((*s >= '0' && *s <= '9') || *s == '.' || *s == '-' || *s == '+')) return false;
    v = strtod(s, &end);
    if (end == s || strcmp(end, unit) != 0 || v != v) return false;
    *out = v;
    return true;
}

/* §4.4's `<font-weight-absolute>`: `normal | bold | <number [1,1000]>` (css-fonts-4 §2.2). */
static bool fd_weight_one(const char *s, double *out)
{
    if (strcmp(s, "normal") == 0) { *out = 400.0; return true; }
    if (strcmp(s, "bold") == 0) { *out = 700.0; return true; }
    return fd_number(s, "", out) && *out >= 1.0 && *out <= 1000.0;
}

/* css-fonts-4 §2.3 "Font width: the font-width property"'s keywords and their percentages. */
static const struct { const char *kw; double pct; } FD_WIDTH[] = {
    { "ultra-condensed", 50.0 }, { "extra-condensed", 62.5 }, { "condensed", 75.0 },
    { "semi-condensed", 87.5 }, { "normal", 100.0 }, { "semi-expanded", 112.5 }, { "expanded", 125.0 },
    { "extra-expanded", 150.0 }, { "ultra-expanded", 200.0 },
};

static bool fd_width_one(const char *s, double *out)
{
    size_t i;

    for (i = 0; i < sizeof FD_WIDTH / sizeof FD_WIDTH[0]; i++)
        if (strcmp(s, FD_WIDTH[i].kw) == 0) { *out = FD_WIDTH[i].pct; return true; }
    return fd_number(s, "%", out) && *out >= 0.0;
}

static void fd_order(CssFontRange *r)
{
    if (r->lo > r->hi) { double t = r->lo; r->lo = r->hi; r->hi = t; }
}

bool css_font_face_weight(const char *value, CssFontRange *out)
{
    char c[FD_MAX_COMPONENTS][FD_COMPONENT_BYTES];
    int n;

    if (!fd_split(value, c, &n) || n > 2) return false;
    if (n == 1 && strcmp(c[0], "auto") == 0) { out->lo = out->hi = 400.0; return true; }
    if (!fd_weight_one(c[0], &out->lo)) return false;
    out->hi = out->lo;
    if (n == 2 && !fd_weight_one(c[1], &out->hi)) return false;
    fd_order(out);
    return true;
}

bool css_font_face_width(const char *value, CssFontRange *out)
{
    char c[FD_MAX_COMPONENTS][FD_COMPONENT_BYTES];
    int n;

    if (!fd_split(value, c, &n) || n > 2) return false;
    if (n == 1 && strcmp(c[0], "auto") == 0) { out->lo = out->hi = 100.0; return true; }
    if (!fd_width_one(c[0], &out->lo)) return false;
    out->hi = out->lo;
    if (n == 2 && !fd_width_one(c[1], &out->hi)) return false;
    fd_order(out);
    return true;
}

/* §4.4's `auto | normal | italic | oblique [ <angle [-90deg,90deg]>{1,2} ]?`. A bare `oblique` is css-fonts-4
   §2.4 "Font style: the font-style property"'s 14deg. */
bool css_font_face_style(const char *value, CssFontFaceStyle *out)
{
    char c[FD_MAX_COMPONENTS][FD_COMPONENT_BYTES];
    int n, i;

    if (!fd_split(value, c, &n)) return false;
    out->angle.lo = out->angle.hi = 0.0;
    if (n == 1 && (strcmp(c[0], "auto") == 0 || strcmp(c[0], "normal") == 0)) {
        out->slant = CSS_FONT_FACE_NORMAL;
        return true;
    }
    if (n == 1 && strcmp(c[0], "italic") == 0) { out->slant = CSS_FONT_FACE_ITALIC; return true; }
    if (strcmp(c[0], "oblique") != 0) return false;
    out->slant = CSS_FONT_FACE_OBLIQUE;
    out->angle.lo = out->angle.hi = 14.0;
    for (i = 1; i < n; i++) {
        double a;

        if (!fd_number(c[i], "deg", &a) || a < -90.0 || a > 90.0) return false;
        if (i == 1) out->angle.lo = out->angle.hi = a;
        else out->angle.hi = a;
    }
    fd_order(&out->angle);
    return true;
}

bool css_font_face_property_descriptor(const char *name)
{
    return strcmp(name, "font-weight") == 0 || strcmp(name, "font-style") == 0 ||
           strcmp(name, "font-width") == 0 || strcmp(name, "font-stretch") == 0;
}

char *css_font_face_property_descriptor_value(const char *name, const char *value)
{
    char c[FD_MAX_COMPONENTS][FD_COMPONENT_BYTES];
    CssFontFaceStyle style;
    CssFontRange range;
    size_t at = 0;
    bool valid;
    char *out;
    int n, i;

    DCHECK(name != NULL && css_font_face_property_descriptor(name),
           "css-fonts-4 §4.4's grammar was asked of a name that is none of its three descriptors");
    if (value == NULL) return NULL;
    valid = strcmp(name, "font-weight") == 0 ? css_font_face_weight(value, &range)
          : strcmp(name, "font-style") == 0 ? css_font_face_style(value, &style)
          : css_font_face_width(value, &range);
    if (!valid || !fd_split(value, c, &n)) return NULL;
    out = lexbor_malloc((size_t)FD_MAX_COMPONENTS * FD_COMPONENT_BYTES);
    CHECK(out != NULL, "cssom: OOM serializing a css-fonts-4 §4.4 descriptor");
    for (i = 0; i < n; i++) {
        size_t len = strlen(c[i]);

        if (i) out[at++] = ' ';
        memcpy(out + at, c[i], len);
        at += len;
    }
    out[at] = '\0';
    return out;
}
