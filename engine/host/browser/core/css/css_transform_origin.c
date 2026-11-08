/* css-transforms-1 §4 "The transform-origin Property". See css_transform_origin.h. */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_length.h"
#include "core/css/css_math.h"
#include "core/css/css_shorthand.h"
#include "core/css/css_transform_function.h"
#include "core/css/css_transform_origin.h"
#include "solver/reclaim.h"

/* What one component value of §4's grammar is. `left`/`right` name only the horizontal axis and `top`/`bottom`
   only the vertical one; `center` and a `<length-percentage>` fit either. */
typedef enum { TO_INVALID, TO_HORIZONTAL_KW, TO_VERTICAL_KW, TO_CENTER, TO_LENGTH_PERCENTAGE } ToClass;

typedef struct {
    ToClass   cls;
    CssLength len;   /* every class but TO_INVALID; a keyword holds the percentage it computes to */
} ToComponent;

static CssLength to_pct(double pct)
{
    CssLength l;

    memset(&l, 0, sizeof l);
    l.kind = CSS_LENGTH_PERCENTAGE;
    l.pct = pct;
    l.keyword = CSS_KEYWORD_INVALID;
    return l;
}

/* An ASCII case-insensitive comparison of a component against a canonical lower-case keyword (CSS Syntax §4
   makes an ident case-insensitive, and this property arrives as the author's bytes). */
static bool to_word_is(const char *w, size_t n, const char *kw)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (kw[i] == '\0' || (char)tolower((unsigned char)w[i]) != kw[i]) return false;
    return kw[n] == '\0';
}

/* One component, classified and computed: css-transforms-1 §4's keyword table ("left Computes to 0% for the horizontal
   position", "right … 100%", "top … 0% for the vertical position", "bottom … 100%", "center Computes to 50%"),
   or a `<length-percentage>` absolutized by core/css/css_length.h. */
static ToComponent to_component(JSContext *realm, const CssFontMetrics *font, const char *w, size_t n)
{
    ToComponent c;
    char *copy;
    bool ok;

    c.cls = TO_INVALID;
    c.len = to_pct(0.0);
    if (to_word_is(w, n, "left"))   { c.cls = TO_HORIZONTAL_KW; return c; }
    if (to_word_is(w, n, "right"))  { c.cls = TO_HORIZONTAL_KW; c.len = to_pct(100.0); return c; }
    if (to_word_is(w, n, "top"))    { c.cls = TO_VERTICAL_KW; return c; }
    if (to_word_is(w, n, "bottom")) { c.cls = TO_VERTICAL_KW; c.len = to_pct(100.0); return c; }
    if (to_word_is(w, n, "center")) { c.cls = TO_CENTER; c.len = to_pct(50.0); return c; }
    copy = reclaim_strndup(w, n);
    CHECK(copy != NULL, "css-transforms-1 §4: OOM copying a transform-origin component");
    ok = css_length_parse(realm, font, copy, CSS_KEYWORDS_NONE, &c.len);
    free(copy);
    if (ok) c.cls = TO_LENGTH_PERCENTAGE;
    return c;
}

static bool to_fits_horizontal(ToClass c)
{
    return c == TO_HORIZONTAL_KW || c == TO_CENTER || c == TO_LENGTH_PERCENTAGE;
}

static bool to_fits_vertical(ToClass c)
{
    return c == TO_VERTICAL_KW || c == TO_CENTER || c == TO_LENGTH_PERCENTAGE;
}

CssTransformOrigin css_transform_origin_initial(void)
{
    CssTransformOrigin o;

    o.x = to_pct(50.0);
    o.y = to_pct(50.0);
    o.z = css_px(0.0);
    return o;
}

bool css_transform_origin_parse(JSContext *realm, const CssFontMetrics *font, const char *value,
                                CssTransformOrigin *out)
{
    const char *w[3];
    size_t wl[3];
    ToComponent c[3];
    CssTransformOrigin o = css_transform_origin_initial();
    int n, i;

    DCHECK(value != NULL && out != NULL, "css-transforms-1 §4's grammar was asked with no value or no output");
    n = css_shorthand_components(value, w, wl, 3);
    if (n < 1) return false;
    for (i = 0; i < n; i++) {
        c[i] = to_component(realm, font, w[i], wl[i]);
        if (c[i].cls == TO_INVALID) return false;
    }
    if (n == 1) {
        /* "If only one value is specified, the second value is assumed to be center." A vertical keyword is
           the vertical position, and the assumed center is then the horizontal one. */
        if (c[0].cls == TO_VERTICAL_KW) o.y = c[0].len;
        else o.x = c[0].len;
        *out = o;
        return true;
    }
    /* The second arm, `[ left | center | right | <length-percentage> ] [ top | center | bottom |
       <length-percentage> ]`, and then the third, `[ center | left | right ] && [ center | top | bottom ]`,
       whose `&&` admits the vertical keyword first. */
    if (to_fits_horizontal(c[0].cls) && to_fits_vertical(c[1].cls)) {
        o.x = c[0].len;
        o.y = c[1].len;
    } else if ((c[0].cls == TO_VERTICAL_KW || c[0].cls == TO_CENTER) &&
               (c[1].cls == TO_HORIZONTAL_KW || c[1].cls == TO_CENTER)) {
        o.x = c[1].len;
        o.y = c[0].len;
    } else {
        return false;
    }
    /* "A third value always represents the Z position (or offset) and must be of type <length>." */
    if (n == 3) {
        if (c[2].cls != TO_LENGTH_PERCENTAGE || c[2].len.kind != CSS_LENGTH_ABSOLUTE) return false;
        o.z = c[2].len.px;
    }
    *out = o;
    return true;
}

char *css_transform_origin_serialize(const CssTransformOrigin *o)
{
    char *x, *y, *z = NULL, *out;
    size_t n;

    DCHECK(o != NULL, "a transform-origin was serialized through a NULL handle");
    x = css_transform_length_serialize(&o->x);
    y = css_transform_length_serialize(&o->y);
    if (o->z.env != CSS_ENV_NONE || o->z.px != 0.0) z = css_length_serialize_px(o->z.px);
    n = strlen(x) + strlen(y) + (z ? strlen(z) + 1 : 0) + 2;
    out = reclaim_malloc(n);
    CHECK(out != NULL, "css-transforms-1 §4: OOM serializing a transform-origin");
    snprintf(out, n, "%s %s%s%s", x, y, z ? " " : "", z ? z : "");
    free(x);
    free(y);
    free(z);
    return out;
}
