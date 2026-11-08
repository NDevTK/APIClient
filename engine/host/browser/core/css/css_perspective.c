/* css-transforms-2 §8 and §9's grammars. See css_perspective.h. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_length.h"
#include "core/css/css_perspective.h"
#include "core/css/css_position_value.h"
#include "core/css/css_shorthand.h"
#include "core/css/css_transform_origin.h"
#include "solver/reclaim.h"

bool css_perspective_parse(JSContext *realm, const CssFontMetrics *font, const char *value, CssPx *out)
{
    CssLength len;

    DCHECK(value != NULL && out != NULL, "css-transforms-2 §8's grammar was asked with no value or no output");
    if (!css_length_parse(realm, font, value, CSS_KEYWORDS_NONE, &len)) return false;
    if (len.kind != CSS_LENGTH_ABSOLUTE) return false;
    /* A literal below the range is outside the grammar; a math function's result is clamped. */
    if (strchr(value, '(') == NULL) {
        if (len.px.env == CSS_ENV_NONE && len.px.px < 0.0) return false;
        *out = len.px;
        return true;
    }
    *out = css_px_max(len.px, css_px(0.0));
    return true;
}

static CssLength css_po_pct(double pct)
{
    CssLength l;

    memset(&l, 0, sizeof l);
    l.kind = CSS_LENGTH_PERCENTAGE;
    l.pct = pct;
    l.keyword = CSS_KEYWORD_INVALID;
    return l;
}

/* One axis of a matched `<position>` as background-position computes it: a keyword is its percentage, an
   offset from the near edge is the offset, and one from the far edge (`right`, `bottom`) is 100% minus it. */
static bool css_po_axis(JSContext *realm, const CssFontMetrics *font, const char *kw, const char *off,
                        size_t off_len, CssLength *out)
{
    CssLength o;
    char *copy;
    bool ok, far = kw != NULL && (strcmp(kw, "right") == 0 || strcmp(kw, "bottom") == 0);

    if (off == NULL) {
        DCHECK(kw != NULL, "css-values-4 §8.3: a matched <position> axis has neither a keyword nor an offset");
        *out = css_po_pct(strcmp(kw, "center") == 0 ? 50.0 : far ? 100.0 : 0.0);
        return true;
    }
    copy = reclaim_strndup(off, off_len);
    CHECK(copy != NULL, "css-transforms-2 §9: OOM copying a perspective-origin offset");
    ok = css_length_parse(realm, font, copy, CSS_KEYWORDS_NONE, &o);
    free(copy);
    if (!ok) return false;
    if (!far) {
        *out = o;
        return true;
    }
    if (o.kind == CSS_LENGTH_PERCENTAGE) {
        *out = css_po_pct(100.0 - o.pct);
        return true;
    }
    if (o.kind == CSS_LENGTH_CALCULATED && o.tree != NULL)
        DFAIL("css-transforms-2 §9: a perspective-origin offset from the right or bottom edge is a calculation "
              "tree, and 100% minus it is one more operator node. BUILD the subtraction over core/css/css_math.h's "
              "tree");
    o.pct = 100.0 - (o.kind == CSS_LENGTH_CALCULATED ? o.pct : 0.0);
    o.px = css_px_scale(o.px, -1.0);
    o.kind = CSS_LENGTH_CALCULATED;
    *out = o;
    return true;
}

bool css_perspective_origin_parse(JSContext *realm, const CssFontMetrics *font, const char *value,
                                  CssTransformOrigin *out)
{
    const char *w[5];
    size_t wl[5];
    CssPositionValue p;
    CssTransformOrigin o;
    int n;

    DCHECK(value != NULL && out != NULL, "css-transforms-2 §9's grammar was asked with no value or no output");
    n = css_shorthand_components(value, w, wl, 5);
    if (n < 1 || n > 4) return false;
    if (css_position_match(w, wl, (unsigned)n, false, &p) != (unsigned)n) return false;
    if (!css_po_axis(realm, font, p.h_kw, p.h_off, p.h_off_len, &o.x) ||
        !css_po_axis(realm, font, p.v_kw, p.v_off, p.v_off_len, &o.y))
        return false;
    o.z = css_px(0.0);
    *out = o;
    return true;
}
