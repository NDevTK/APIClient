/* css-transforms-1 §2 "The Transform Rendering Model" and css-transforms-2 §6 "Current Transformation Matrix".
   See css_transform_matrix.h for which edition's section numbers these are, for why §3.2's two-step product is
   NOT this algorithm, and for why every entry is a `CssPx`. */
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>

#include <lexbor/core/base.h>
#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_dimension.h"
#include "core/css/css_style_declaration.h"
#include "core/css/css_length.h"
#include "core/css/css_transform.h"
#include "core/css/css_transform_function.h"
#include "core/css/css_transform_matrix.h"
#include "core/css/css_transform_origin.h"
#include "core/layout/flow_position.h"
#include "core/layout/used_value.h"

/* A product and a sum over core/css/css_length.h's arithmetic, exact where an operand is a concrete 0 or 1, so
   an affine matrix's zero projective row cancels exactly and carries no fact. */
static CssPx css_tm_mul(CssPx a, CssPx b)
{
    if (a.env == CSS_ENV_NONE && b.env == CSS_ENV_NONE) return css_px(a.px * b.px);
    if (a.env == CSS_ENV_NONE) return a.px == 0.0 ? css_px(0.0) : a.px == 1.0 ? b : css_px_scale(b, a.px);
    if (b.env == CSS_ENV_NONE) return b.px == 0.0 ? css_px(0.0) : b.px == 1.0 ? a : css_px_scale(a, b.px);
    return css_px_mul(a, b);
}

static CssPx css_tm_add(CssPx a, CssPx b)
{
    if (a.env == CSS_ENV_NONE && b.env == CSS_ENV_NONE) return css_px(a.px + b.px);
    if (a.env == CSS_ENV_NONE && a.px == 0.0) return b;
    if (b.env == CSS_ENV_NONE && b.px == 0.0) return a;
    return css_px_add(a, b);
}

static bool css_tm_is(CssPx v, double k)
{
    return v.env == CSS_ENV_NONE && v.px == k;
}

CssTransformMatrix css_transform_matrix_identity(void)
{
    CssTransformMatrix out;

    out.a = css_px(1.0); out.b = css_px(0.0); out.c = css_px(0.0); out.d = css_px(1.0);
    out.e = css_px(0.0); out.f = css_px(0.0);
    out.p = css_px(0.0); out.q = css_px(0.0); out.s = css_px(1.0);
    return out;
}

/* The 3x3 as rows x, y, w over columns x, y, 1. */
static void css_tm_rows(const CssTransformMatrix *m, CssPx r[3][3])
{
    r[0][0] = m->a; r[0][1] = m->c; r[0][2] = m->e;
    r[1][0] = m->b; r[1][1] = m->d; r[1][2] = m->f;
    r[2][0] = m->p; r[2][1] = m->q; r[2][2] = m->s;
}

CssTransformMatrix css_transform_matrix_multiply(CssTransformMatrix outer, CssTransformMatrix inner)
{
    CssPx o[3][3], n[3][3], r[3][3];
    CssTransformMatrix out;
    int i, j, k;

    css_tm_rows(&outer, o);
    css_tm_rows(&inner, n);
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) {
            r[i][j] = css_px(0.0);
            for (k = 0; k < 3; k++) r[i][j] = css_tm_add(r[i][j], css_tm_mul(o[i][k], n[k][j]));
        }
    out.a = r[0][0]; out.c = r[0][1]; out.e = r[0][2];
    out.b = r[1][0]; out.d = r[1][1]; out.f = r[1][2];
    out.p = r[2][0]; out.q = r[2][1]; out.s = r[2][2];
    return out;
}

void css_transform_matrix_map(CssTransformMatrix m, CssPx x, CssPx y, CssPx *out_x, CssPx *out_y)
{
    CssPx w;

    DCHECK(out_x != NULL && out_y != NULL,
           "css-transforms-1 §2 \"The Transform Rendering Model\"'s \"post-multiply the transformation matrix "
           "TM of the element by p local\" was asked with nowhere to put the mapped point");
    *out_x = css_tm_add(css_tm_add(css_tm_mul(m.a, x), css_tm_mul(m.c, y)), m.e);
    *out_y = css_tm_add(css_tm_add(css_tm_mul(m.b, x), css_tm_mul(m.d, y)), m.f);
    w = css_tm_add(css_tm_add(css_tm_mul(m.p, x), css_tm_mul(m.q, y)), m.s);
    if (css_tm_is(w, 1.0)) return;
    if (!(w.px > 0.0)) {
        DFAIL("css-transforms-2 §4.2 \"Processing of Perspective-Transformed Boxes\": a point mapped to w <= 0, "
              "behind the viewer. BUILD that section's clipping of the box to the w > 0 half-space before its "
              "corners are divided");
        return;
    }
    *out_x = css_px_div(*out_x, w);
    *out_y = css_px_div(*out_y, w);
}

/* css-transforms-1 §5 "Transform reference box: the transform-box property"'s reference box of `el` on one axis,
   its width or (`vertical`) its height: "For elements with associated CSS layout box, the used value for
   fill-box is content-box and for stroke-box and view-box is border-box". */
static CssPx css_tm_reference_size(lxb_dom_element_t *el, bool vertical)
{
    return css_computed_transform_box_is_content(el) ? used_value_content_px(el, vertical)
                                                     : used_value_border_edge_px(el, vertical);
}

static bool css_tm_reference_size_ask(lxb_dom_element_t *el, bool vertical)
{
    CssPx unused;

    return css_computed_transform_box_is_content(el) ? used_value_content_ask(el, vertical, &unused)
                                                     : used_value_border_edge_ask(el, vertical, &unused);
}

/* One `<length-percentage>` of `el`'s transform or transform-origin against the reference box size `ref` on
   its axis: css-transforms-1 §7 "The Transform Functions" ("A percentage for horizontal translations is
   relative to the width of the reference box") and css-transforms-1 §4 ("A percentage for the horizontal
   offset is relative to the width of the reference box"). The percentage is a used value, since
   css-values-4 §10.11 "Computed Value" leaves it unresolved at computed-value time. `ref` is NULL on the
   pass that reads only the linear part. */
static CssPx css_tm_length_px(const CssLength *len, const CssPx *ref)
{
    DCHECK(len->kind != CSS_LENGTH_KEYWORD,
           "a transform or transform-origin length is a keyword; core/css/css_transform_function.c and "
           "core/css/css_transform_origin.c parse with no keyword set and compute keywords to percentages");
    if (len->kind == CSS_LENGTH_ABSOLUTE) return len->px;
    if (ref == NULL) return css_px(0.0);
    return css_length_resolve_pct(*len, *ref);
}

/* An angle argument in degrees: css-values-4 §7.1's canonical unit, or 0 for §7's `<zero>`, which the parse
   stores with an empty unit. */
static double css_tm_angle_deg(const CssTransformFunction *fn, unsigned i)
{
    double deg;
    bool ok;

    if (fn->angle_unit[i][0] == '\0') return 0.0;
    ok = css_angle_deg(fn->angle_unit[i], strlen(fn->angle_unit[i]), fn->angle[i], &deg);
    DCHECK(ok, "a transform angle carries a unit core/css/css_transform_function.c admitted and "
               "css_angle_deg does not convert; the two read one unit table");
    return deg;
}

/* sin and cos of `deg` degrees, exact where they are 0 or ±1: π/180 is not a binary fraction, so
   sin(90 · π/180) is 1 but cos(90 · π/180) is 6.1e-17, and a quarter turn would shear by that much. */
static void css_tm_sin_cos(double deg, double *s, double *c)
{
    double r = fmod(deg, 360.0);

    if (r < 0.0) r += 360.0;
    if (r == 0.0)        { *s = 0.0;  *c = 1.0; }
    else if (r == 90.0)  { *s = 1.0;  *c = 0.0; }
    else if (r == 180.0) { *s = 0.0;  *c = -1.0; }
    else if (r == 270.0) { *s = -1.0; *c = 0.0; }
    else                 { *s = sin(deg * M_PI / 180.0); *c = cos(deg * M_PI / 180.0); }
}

/* tan of `deg` degrees, exactly 0 at the multiples of 180 for css_tm_sin_cos's reason. */
static double css_tm_tan(double deg)
{
    return fmod(deg, 180.0) == 0.0 ? 0.0 : tan(deg * M_PI / 180.0);
}

static CssTransformMatrix3d css_tm4_identity(void)
{
    CssTransformMatrix3d out;
    int r, c;

    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++) out.m[r][c] = css_px(r == c ? 1.0 : 0.0);
    return out;
}

static CssTransformMatrix3d css_tm4_multiply(const CssTransformMatrix3d *a, const CssTransformMatrix3d *b)
{
    CssTransformMatrix3d out;
    int r, c, k;

    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++) {
            out.m[r][c] = css_px(0.0);
            for (k = 0; k < 4; k++) out.m[r][c] = css_tm_add(out.m[r][c], css_tm_mul(a->m[r][k], b->m[k][c]));
        }
    return out;
}

static CssTransformMatrix3d css_tm4_translation(CssPx x, CssPx y, CssPx z)
{
    CssTransformMatrix3d out = css_tm4_identity();

    out.m[0][3] = x; out.m[1][3] = y; out.m[2][3] = z;
    return out;
}

/* css-transforms-2 §16's rotation about the normalized [x, y, z] by `deg`, with "sc = sin(alpha/2) ·
   cos(alpha/2)" and "sq = sin²(alpha/2)" taken as sin(alpha)/2 and (1 − cos(alpha))/2, the same numbers, so
   css_tm_sin_cos's exact quarter turns carry through. */
static CssTransformMatrix3d css_tm4_rotation(double x, double y, double z, double deg)
{
    CssTransformMatrix3d out = css_tm4_identity();
    double sn, cs, sc, sq;

    css_tm_sin_cos(deg, &sn, &cs);
    sc = sn / 2.0;
    sq = (1.0 - cs) / 2.0;
    out.m[0][0] = css_px(1.0 - 2.0 * (y * y + z * z) * sq);
    out.m[0][1] = css_px(2.0 * (x * y * sq - z * sc));
    out.m[0][2] = css_px(2.0 * (x * z * sq + y * sc));
    out.m[1][0] = css_px(2.0 * (x * y * sq + z * sc));
    out.m[1][1] = css_px(1.0 - 2.0 * (x * x + z * z) * sq);
    out.m[1][2] = css_px(2.0 * (y * z * sq - x * sc));
    out.m[2][0] = css_px(2.0 * (x * z * sq - y * sc));
    out.m[2][1] = css_px(2.0 * (y * z * sq + x * sc));
    out.m[2][2] = css_px(1.0 - 2.0 * (x * x + y * y) * sq);
    /* At a whole axis rotation the closed form is exact; this keeps rotateZ(90deg)'s zeros zero. */
    if (x == 0.0 && y == 0.0 && z == 1.0) {
        out.m[0][0] = css_px(cs); out.m[0][1] = css_px(-sn); out.m[1][0] = css_px(sn); out.m[1][1] = css_px(cs);
    }
    return out;
}

/* css-transforms-2 §16's perspective(d), the identity with m34 = −1/d. §8 "The perspective Property": "values
   less than 1px must be treated as 1px for rendering purposes", and §12.2 says the same of the function. */
static CssTransformMatrix3d css_tm4_perspective(CssPx d)
{
    CssTransformMatrix3d out = css_tm4_identity();

    out.m[3][2] = css_px_div(css_px(-1.0), css_px_max(d, css_px(1.0)));
    return out;
}

/* css-transforms-1 §12 "Mathematical Description of Transform Functions"' matrix for one 2D
   `<transform-function>`, and css-transforms-2 §16 "Mathematical Description of Transform Functions"' for one
   3D one. `ref` is the reference box (width, height), or NULL on the pass that reads only the linear part. */
static CssTransformMatrix3d css_tm_of_function(const CssTransformFunction *fn, const CssPx *ref)
{
    CssTransformMatrix3d out = css_tm4_identity();
    const CssPx *w = ref, *h = ref ? &ref[1] : NULL;
    const double *n = fn->number;
    double len;
    int r, c;

    DCHECK(fn->argc > 0, "css-transforms-1 §3's `<transform-list>` is `<transform-function>+` and every §7.1 "
                         "and css-transforms-2 §12.2 function has a required argument");
    switch (fn->kind) {
    case CSS_TF_MATRIX:
        out.m[0][0] = css_px(n[0]); out.m[1][0] = css_px(n[1]); out.m[0][1] = css_px(n[2]);
        out.m[1][1] = css_px(n[3]); out.m[0][3] = css_px(n[4]); out.m[1][3] = css_px(n[5]);
        break;
    /* css-transforms-1 §7.1: "If <ty> is not provided, ty has zero as a value". */
    case CSS_TF_TRANSLATE:
        out.m[0][3] = css_tm_length_px(&fn->length[0], w);
        if (fn->argc > 1) out.m[1][3] = css_tm_length_px(&fn->length[1], h);
        break;
    case CSS_TF_TRANSLATE_X:
        out.m[0][3] = css_tm_length_px(&fn->length[0], w);
        break;
    case CSS_TF_TRANSLATE_Y:
        out.m[1][3] = css_tm_length_px(&fn->length[0], h);
        break;
    case CSS_TF_TRANSLATE3D:
        out.m[0][3] = css_tm_length_px(&fn->length[0], w);
        out.m[1][3] = css_tm_length_px(&fn->length[1], h);
        out.m[2][3] = css_tm_length_px(&fn->length[2], NULL);
        break;
    case CSS_TF_TRANSLATE_Z:
        out.m[2][3] = css_tm_length_px(&fn->length[0], NULL);
        break;
    /* css-transforms-1 §7.1: "If the second parameter is not provided, it takes a value equal to the first." */
    case CSS_TF_SCALE:
        out.m[0][0] = css_px(n[0]);
        out.m[1][1] = css_px(fn->argc > 1 ? n[1] : n[0]);
        break;
    case CSS_TF_SCALE_X:
        out.m[0][0] = css_px(n[0]);
        break;
    case CSS_TF_SCALE_Y:
        out.m[1][1] = css_px(n[0]);
        break;
    case CSS_TF_SCALE3D:
        out.m[0][0] = css_px(n[0]); out.m[1][1] = css_px(n[1]); out.m[2][2] = css_px(n[2]);
        break;
    case CSS_TF_SCALE_Z:
        out.m[2][2] = css_px(n[0]);
        break;
    /* css-transforms-2 §12.2: rotateZ() is "same as rotate3d(0, 0, 1, <angle>), which is a 3d transform
       equivalent to the 2d transform rotate(<angle>)", and rotateX() and rotateY() are rotate3d(1, 0, 0, a) and
       rotate3d(0, 1, 0, a). */
    case CSS_TF_ROTATE:
    case CSS_TF_ROTATE_Z:
        out = css_tm4_rotation(0.0, 0.0, 1.0, css_tm_angle_deg(fn, 0));
        break;
    case CSS_TF_ROTATE_X:
        out = css_tm4_rotation(1.0, 0.0, 0.0, css_tm_angle_deg(fn, 0));
        break;
    case CSS_TF_ROTATE_Y:
        out = css_tm4_rotation(0.0, 1.0, 0.0, css_tm_angle_deg(fn, 0));
        break;
    /* §16: x, y and z "have been normalized"; css-transforms-2 §12.2 "3D Transform Functions": "A direction
       vector that cannot be normalized, such as [0,0,0], will cause the rotation to not be applied." */
    case CSS_TF_ROTATE3D:
        len = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 0.0 && isfinite(len))
            out = css_tm4_rotation(n[0] / len, n[1] / len, n[2] / len, css_tm_angle_deg(fn, 0));
        break;
    /* css-transforms-1 §12's skew: m21 = tan(alpha) and m12 = tan(beta); css-transforms-1 §7.1: "If the
       second parameter is not provided, it has a zero value." */
    case CSS_TF_SKEW:
        out.m[0][1] = css_px(css_tm_tan(css_tm_angle_deg(fn, 0)));
        if (fn->argc > 1) out.m[1][0] = css_px(css_tm_tan(css_tm_angle_deg(fn, 1)));
        break;
    case CSS_TF_SKEW_X:
        out.m[0][1] = css_px(css_tm_tan(css_tm_angle_deg(fn, 0)));
        break;
    case CSS_TF_SKEW_Y:
        out.m[1][0] = css_px(css_tm_tan(css_tm_angle_deg(fn, 0)));
        break;
    /* css-transforms-2 §12.2: "16 values in column-major order": m11, m12, m13, m14, m21, …, m44. */
    case CSS_TF_MATRIX3D:
        for (c = 0; c < 4; c++)
            for (r = 0; r < 4; r++) out.m[r][c] = css_px(n[c * 4 + r]);
        break;
    /* css-transforms-2 §16: m34 = −1/d; "If the parameter d is none it is treated as infinity (and the
       resulting matrix is the identity matrix)", and css-transforms-2 §12.2: "If the depth value is less than
       1px, it must be treated as 1px". */
    case CSS_TF_PERSPECTIVE:
        if (!fn->perspective_none) out = css_tm4_perspective(css_tm_length_px(&fn->length[0], NULL));
        break;
    }
    return out;
}

/* The matrix of every function in `list`, post-multiplied left to right: §2's step 3, "Multiply by each of
   the transform functions in transform property from left to right". */
static CssTransformMatrix3d css_tm_of_list(const CssTransformList *list, const CssPx *ref)
{
    CssTransformMatrix3d out = css_tm4_identity(), fm;
    size_t i;

    for (i = 0; i < list->n; i++) {
        fm = css_tm_of_function(&list->fn[i], ref);
        out = css_tm4_multiply(&out, &fm);
    }
    return out;
}

/* Whether a 4x4 maps every point by its translation alone: its upper-left 3x3 the identity and its projective
   row 0 0 0 1. Then T(A)·M·T(−A) is M for every A, so the origin and the position are not read. */
static bool css_tm4_is_translation(const CssTransformMatrix3d *m)
{
    int r, c;

    for (r = 0; r < 4; r++)
        for (c = 0; c < 3; c++)
            if (!css_tm_is(m->m[r][c], r == c ? 1.0 : 0.0)) return false;
    return css_tm_is(m->m[3][3], 1.0);
}

/* §7's flattening of an element's 4x4 into its parent's plane: rows x, y, w over columns x, y, 1. */
static CssTransformMatrix css_tm4_flatten(const CssTransformMatrix3d *m)
{
    CssTransformMatrix out;

    out.a = m->m[0][0]; out.c = m->m[0][1]; out.e = m->m[0][3];
    out.b = m->m[1][0]; out.d = m->m[1][1]; out.f = m->m[1][3];
    out.p = m->m[3][0]; out.q = m->m[3][1]; out.s = m->m[3][3];
    return out;
}

/* Whether `list` reads the reference box on each axis through a percentage translation. */
static void css_tm_list_reads(const CssTransformList *list, bool *x, bool *y)
{
    size_t i;

    *x = *y = false;
    for (i = 0; i < list->n; i++) {
        const CssTransformFunction *fn = &list->fn[i];

        switch (fn->kind) {
        case CSS_TF_TRANSLATE:
        case CSS_TF_TRANSLATE3D:
            *x |= fn->length[0].kind != CSS_LENGTH_ABSOLUTE;
            *y |= fn->argc > 1 && fn->length[1].kind != CSS_LENGTH_ABSOLUTE;
            break;
        case CSS_TF_TRANSLATE_X:
            *x |= fn->length[0].kind != CSS_LENGTH_ABSOLUTE;
            break;
        case CSS_TF_TRANSLATE_Y:
            *y |= fn->length[0].kind != CSS_LENGTH_ABSOLUTE;
            break;
        default:
            break;
        }
    }
}

/* Whether `el`'s reference box is SVG 2's rather than the CSS box model's, and the crash for a percentage that
   would read it. */
static bool css_tm_svg_reference_box(lxb_dom_element_t *el, const CssTransformOrigin *o)
{
    if (css_transform_category(el) != CSS_TRANSFORMABLE_SVG) return false;
    if (o->x.kind != CSS_LENGTH_ABSOLUTE || o->y.kind != CSS_LENGTH_ABSOLUTE)
        DFAIL("css-transforms-1 §4: a percentage transform-origin or perspective-origin on an SVG element refers "
              "to §5's reference box, which for SVG elements without associated CSS layout box is SVG 2's: \"the used value for "
              "content-box is fill-box and for border-box is stroke-box\", and view-box is the nearest SVG "
              "viewport. BUILD SVG 2 §8.10 \"Bounding boxes\"' object and stroke bounding boxes and the "
              "viewport's viewBox size as that reference box");
    return true;
}

/* An origin's offsets against `el`'s reference box: css-transforms-1 §4's and css-transforms-2 §9's
   percentages each "relative to the width of the reference box", and the same for the height. */
static void css_tm_origin_used(lxb_dom_element_t *el, CssTransformOrigin o, CssPx out[3])
{
    CssPx ref[2] = { css_px(0.0), css_px(0.0) };
    bool svg;

    svg = css_tm_svg_reference_box(el, &o);
    if (!svg && o.x.kind != CSS_LENGTH_ABSOLUTE) ref[0] = css_tm_reference_size(el, false);
    if (!svg && o.y.kind != CSS_LENGTH_ABSOLUTE) ref[1] = css_tm_reference_size(el, true);
    out[0] = css_tm_length_px(&o.x, &ref[0]);
    out[1] = css_tm_length_px(&o.y, &ref[1]);
    out[2] = o.z;
}

static bool css_tm_origin_used_ask(lxb_dom_element_t *el, CssTransformOrigin o)
{
    if (css_tm_svg_reference_box(el, &o)) return true;
    return (o.x.kind == CSS_LENGTH_ABSOLUTE || css_tm_reference_size_ask(el, false)) &&
           (o.y.kind == CSS_LENGTH_ABSOLUTE || css_tm_reference_size_ask(el, true));
}

void css_transform_origin_used(lxb_dom_element_t *el, CssPx out[3])
{
    DCHECK(el != NULL && out != NULL && css_transform_is_transformable(el),
           "css-transforms-1 §4's used transform-origin was asked of no element, with nowhere to put it, or of "
           "an element its `Applies to: transformable elements` line excludes");
    css_tm_origin_used(el, css_computed_transform_origin(el), out);
}

bool css_transform_origin_used_ask(lxb_dom_element_t *el)
{
    DCHECK(el != NULL && css_transform_is_transformable(el),
           "css-transforms-1 §4's used transform-origin was asked of no element or of one it does not apply to");
    return css_tm_origin_used_ask(el, css_computed_transform_origin(el));
}

void css_perspective_origin_used(lxb_dom_element_t *el, CssPx out[3])
{
    DCHECK(el != NULL && out != NULL && css_transform_is_transformable(el),
           "css-transforms-2 §9's used perspective-origin was asked of no element, with nowhere to put it, or "
           "of an element its `Applies to: transformable elements` line excludes");
    css_tm_origin_used(el, css_computed_perspective_origin(el), out);
}

bool css_perspective_origin_used_ask(lxb_dom_element_t *el)
{
    DCHECK(el != NULL && css_transform_is_transformable(el),
           "css-transforms-2 §9's used perspective-origin was asked of no element or of one it does not apply "
           "to");
    return css_tm_origin_used_ask(el, css_computed_perspective_origin(el));
}

/* What `el`'s matrix reads of layout, decided from its computed values alone. `anchored` is false when the
   list's linear part is the identity: §2's origin bracket T(o)·M·T(−o) is then M exactly for every o, and so
   is the conjugation by the element's own position, so neither is read. */
typedef struct {
    bool               applies;      /* a transformable element with a `<transform-list>` */
    CssTransformList   list;
    CssTransformOrigin origin;
    bool               anchored;
    bool               read_x, read_y;   /* the reference box's width and height */
    bool               content_box;
} CssTmPlan;

/* `el`'s parent when §8's perspective applies to it with a value other than `none`, written to `d`; NULL
   otherwise. §8's `Applies to:` line is transformable elements. */
static lxb_dom_element_t *css_tm_perspective_parent(lxb_dom_element_t *el, CssPx *d)
{
    lxb_dom_element_t *parent = css_parent_element(el);

    if (parent == NULL || !css_transform_is_transformable(parent) || !css_computed_perspective(parent, d))
        return NULL;
    return parent;
}

/* css-transforms-2 §4.1.1 "Perspective"'s perspective matrix of `parent`: "Translate by the computed X and Y
   values of perspective-origin", "Multiply by the matrix that would be obtained from the perspective()
   transform function, where the length is provided by the value of the perspective property", "Translate by
   the negated computed X and Y values of perspective-origin". §9 measures the origin from the parent's
   reference box corner, anchored at its layout position as the element's own origin is. */
static CssTransformMatrix3d css_tm_perspective_matrix(lxb_dom_element_t *parent, CssPx d)
{
    CssTransformMatrix3d m, t;
    CssPx o[3], ax, ay;
    FlowPoint corner = flow_border_box_origin(parent);

    css_perspective_origin_used(parent, o);
    ax = css_px_add(corner.x, o[0]);
    ay = css_px_add(corner.y, o[1]);
    if (css_computed_transform_box_is_content(parent)) {
        ax = css_px_add(ax, used_value_leading_edge_px(parent, false));
        ay = css_px_add(ay, used_value_leading_edge_px(parent, true));
    }
    t = css_tm4_translation(ax, ay, css_px(0.0));
    m = css_tm4_perspective(d);
    m = css_tm4_multiply(&t, &m);
    t = css_tm4_translation(css_px_scale(ax, -1.0), css_px_scale(ay, -1.0), css_px(0.0));
    return css_tm4_multiply(&m, &t);
}

static bool css_tm_perspective_matrix_ask(lxb_dom_element_t *parent)
{
    FlowPoint corner;
    CssPx unused;

    if (!css_perspective_origin_used_ask(parent) || !flow_border_box_origin_ask(parent, &corner)) return false;
    return !css_computed_transform_box_is_content(parent) ||
           (used_value_px_ask(parent, "padding-left", &unused) && used_value_px_ask(parent, "padding-top", &unused));
}

static void css_tm_plan(lxb_dom_element_t *el, CssTmPlan *p)
{
    CssTransformMatrix3d linear;

    memset(p, 0, sizeof *p);
    /* An SVG element under another one maps into its parent's user coordinate system, which is not the CSS
       layout space the chain is composed in: css-transforms-1 §6.4 "User coordinate space" ("the transform
       attribute and transform property define a transformation in the current user coordinate system of the
       parent"), and each viewport-establishing ancestor adds SVG 2 §8.2's equivalent transform. */
    if (lxb_dom_interface_node(el)->ns == LXB_NS_SVG && css_parent_element(el) != NULL &&
        lxb_dom_interface_node(css_parent_element(el))->ns == LXB_NS_SVG) {
        DFAIL("css-transforms-1 §2's current transformation matrix reached an SVG element under another one, "
              "whose matrix is in its parent's user coordinate system. BUILD SVG 2 §8.2 \"Computing the "
              "equivalent transform of an SVG viewport\" for each viewport-establishing ancestor and the "
              "chain from the outermost svg's content box, with SVG 2 §4.4.2 \"Interface SVGGraphicsElement\"'s "
              "getCTM over the same chain");
        return;
    }
    /* §3 "The transform Property"'s `Applies to: transformable elements` first, so a declaration on a box it
       does not apply to moves nothing, as core/css/css_transform.h states. */
    if (!css_transform_is_transformable(el) || !css_transform_composed_list(el, &p->list)) return;
    linear = css_tm_of_list(&p->list, NULL);
    p->applies = true;
    p->anchored = !css_tm4_is_translation(&linear);
    css_tm_list_reads(&p->list, &p->read_x, &p->read_y);
    p->content_box = css_computed_transform_box_is_content(el);
    if (!p->anchored) return;
    p->origin = css_computed_transform_origin(el);
    p->read_x |= p->origin.x.kind != CSS_LENGTH_ABSOLUTE;
    p->read_y |= p->origin.y.kind != CSS_LENGTH_ABSOLUTE;
}

/* The 4x4 of the plan's list within its origin bracket. Consumes the plan's list. */
static CssTransformMatrix3d css_tm_element_own4(lxb_dom_element_t *el, CssTmPlan *pp)
{
    CssTransformMatrix3d m, t;
    CssTmPlan p = *pp;
    CssPx ref[2] = { css_px(0.0), css_px(0.0) }, ax, ay;
    FlowPoint corner;

    if (p.read_x) ref[0] = css_tm_reference_size(el, false);
    if (p.read_y) ref[1] = css_tm_reference_size(el, true);
    m = css_tm_of_list(&p.list, ref);
    css_transform_list_free(&p.list);
    if (p.anchored) {
        /* css-transforms-2 §6's steps 2 and 8 bracket the product by the origin's X, Y and Z, which §4
           measures from the reference box's top-left corner and §5 places there ("A reference box adds an
           additional offset to the origin"). The chain is composed in the initial containing block's space, so
           the bracket is anchored at that corner's layout position plus the origin. */
        corner = flow_border_box_origin(el);
        ax = css_px_add(corner.x, css_tm_length_px(&p.origin.x, &ref[0]));
        ay = css_px_add(corner.y, css_tm_length_px(&p.origin.y, &ref[1]));
        if (p.content_box) {
            ax = css_px_add(ax, used_value_leading_edge_px(el, false));
            ay = css_px_add(ay, used_value_leading_edge_px(el, true));
        }
        t = css_tm4_translation(ax, ay, p.origin.z);
        m = css_tm4_multiply(&t, &m);
        t = css_tm4_translation(css_px_scale(ax, -1.0), css_px_scale(ay, -1.0), css_px_scale(p.origin.z, -1.0));
        m = css_tm4_multiply(&m, &t);
    }
    return m;
}

/* §2's four steps for `el` — its own transformation matrix, the map "from where the element would have
   rendered into that local coordinate system" — as css-transforms-2 §6's 4x4 in the initial containing block's
   space, the origin bracket anchored at §4's origin measured from §5's reference box corner at its layout
   position; then §4.1.4's parent perspective matrix. `carries_depth` is whether points with z ≠ 0 reach it
   (descendants accumulated into it in a 3D rendering context); otherwise an untransformed element maps z = 0
   to z = 0, where the perspective matrix's w is 1, and its flattened matrix is the identity. False, writing the
   identity, when neither applies. */
static bool css_tm_element4(lxb_dom_element_t *el, bool carries_depth, CssTransformMatrix3d *out)
{
    CssTransformMatrix3d t;
    CssTmPlan p;
    CssPx d;
    lxb_dom_element_t *parent;

    DCHECK(el != NULL && out != NULL, "css-transforms-1 §2's transformation matrix was asked for with no element");
    *out = css_tm4_identity();
    css_tm_plan(el, &p);
    if (p.applies) *out = css_tm_element_own4(el, &p);
    else if (!carries_depth) return false;
    /* css-transforms-2 §4.1.4: "If parent element has a value for perspective which is not none, pre-multiply
       the parent element's perspective matrix into the transform." */
    parent = css_tm_perspective_parent(el, &d);
    if (parent != NULL) {
        t = css_tm_perspective_matrix(parent, d);
        *out = css_tm4_multiply(&t, out);
    }
    return p.applies || parent != NULL;
}

/* §3.2's percentages read the reference box of an element whose layout is the CSS box model's; SVG 2's, and
   an element with no box, have none this component can measure. */
static void css_tm_resolved_box_check(lxb_dom_element_t *el)
{
    switch (css_transform_category(el)) {
    case CSS_TRANSFORMABLE_CSS_BOX:
        return;
    case CSS_TRANSFORMABLE_SVG:
        DFAIL("css-transforms-1 §3.2: a percentage translation on an SVG element refers to §5's reference box, "
              "which for SVG elements without associated CSS layout box is SVG 2's. BUILD SVG 2 §8.10 "
              "\"Bounding boxes\"' object and stroke bounding boxes and the viewport's viewBox size as that box");
        return;
    case CSS_TRANSFORMABLE_NONE:
        DFAIL("css-transforms-1 §3.2: a percentage translation on an element that is not transformable (a "
              "non-replaced inline box, a column, or no box at all) has no §5 reference box to resolve against. "
              "BUILD the reference box user agents measure for such an element and resolve against it");
        return;
    }
}

bool css_transform_matrix_resolved(lxb_dom_element_t *el, CssTransformMatrix3d *out)
{
    CssTransformList list;
    CssPx ref[2] = { css_px(0.0), css_px(0.0) };
    bool x, y;

    DCHECK(el != NULL && out != NULL, "css-transforms-1 §3.2's matrix was asked of no element or into nowhere");
    if (!css_computed_transform_list(el, &list)) return false;
    css_tm_list_reads(&list, &x, &y);
    if (x || y) css_tm_resolved_box_check(el);
    if (x && css_transform_is_css_box_transformable(el)) ref[0] = css_tm_reference_size(el, false);
    if (y && css_transform_is_css_box_transformable(el)) ref[1] = css_tm_reference_size(el, true);
    *out = css_tm_of_list(&list, ref);
    css_transform_list_free(&list);
    return true;
}

bool css_transform_matrix3d_derived(const CssTransformMatrix3d *m)
{
    int r, c;

    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++)
            if (m->m[r][c].env != CSS_ENV_NONE) return true;
    return false;
}

char *css_transform_matrix3d_serialize(const CssTransformMatrix3d *m)
{
    /* css-transforms-1 Terminology's 2D matrix: "a 4x4 matrix where the items m31, m32, m13, m23, m43, m14,
       m24, m34 are equal to 0 and m33, m44 are equal to 1". */
    static const int ZERO[8][2] = { { 0, 2 }, { 1, 2 }, { 2, 0 }, { 2, 1 }, { 2, 3 }, { 3, 0 }, { 3, 1 }, { 3, 2 } };
    static const int AFFINE[6][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 }, { 0, 3 }, { 1, 3 } };
    char *t[16], *out;
    size_t n = sizeof "matrix3d()" + 16 * 2;
    int i, k = 0;
    bool two_d = css_tm_is(m->m[2][2], 1.0) && css_tm_is(m->m[3][3], 1.0);

    DCHECK(m != NULL && !css_transform_matrix3d_derived(m),
           "css-transforms-2 §2.1's matrix was serialized with an entry an environment fact reaches; its caller "
           "refuses that one first");
    for (i = 0; i < 8; i++) two_d = two_d && css_tm_is(m->m[ZERO[i][0]][ZERO[i][1]], 0.0);
    /* A negative zero is written as zero. */
    if (two_d)
        for (k = 0; k < 6; k++) t[k] = css_length_serialize_number(m->m[AFFINE[k][0]][AFFINE[k][1]].px + 0.0, "");
    else
        for (k = 0; k < 16; k++) t[k] = css_length_serialize_number(m->m[k % 4][k / 4].px + 0.0, "");
    for (i = 0; i < k; i++) n += strlen(t[i]);
    out = lexbor_malloc(n);
    CHECK(out != NULL, "css-transforms-2 §2.1: OOM serializing a resolved transform");
    strcpy(out, two_d ? "matrix(" : "matrix3d(");
    for (i = 0; i < k; i++) {
        if (i > 0) strcat(out, ", ");
        strcat(out, t[i]);
        free(t[i]);
    }
    strcat(out, ")");
    return out;
}

bool css_transform_matrix_resolved_ask(lxb_dom_element_t *el)
{
    CssTransformList list;
    bool x, y;

    DCHECK(el != NULL, "css-transforms-1 §3.2's matrix was asked of no element");
    if (!css_computed_transform_list(el, &list)) return true;
    css_tm_list_reads(&list, &x, &y);
    css_transform_list_free(&list);
    if (!css_transform_is_css_box_transformable(el)) return true;
    return (!x || css_tm_reference_size_ask(el, false)) && (!y || css_tm_reference_size_ask(el, true));
}

/* The layout `el`'s matrix reads, asked: false ("not yet") when a question was deferred. */
static bool css_tm_element_ask(lxb_dom_element_t *el, bool carries_depth)
{
    CssTmPlan p;
    FlowPoint corner;
    CssPx unused, d;
    bool done = true;
    lxb_dom_element_t *parent;

    css_tm_plan(el, &p);
    if (!p.applies && !carries_depth) return true;
    if (p.applies) {
        css_transform_list_free(&p.list);
        done = (!p.read_x || css_tm_reference_size_ask(el, false)) &&
               (!p.read_y || css_tm_reference_size_ask(el, true));
        if (done && p.anchored) {
            done = flow_border_box_origin_ask(el, &corner);
            if (done && p.content_box)
                done = used_value_px_ask(el, "padding-left", &unused) &&
                       used_value_px_ask(el, "padding-top", &unused);
        }
    }
    parent = css_tm_perspective_parent(el, &d);
    return done && (parent == NULL || css_tm_perspective_matrix_ask(parent));
}

/* Whether `on`'s child on the chain participates in a 3D rendering context `on` establishes or extends
   (css-transforms-2 §4.1.2 "3D Rendering Contexts": "An element participates in a 3D rendering context if its
   parent establishes or extends a 3D rendering context"), so the child's 4x4 is not flattened into `on`'s
   plane. */
static bool css_tm_child_participates(lxb_dom_element_t *on)
{
    return on != NULL && css_transform_style_used_preserve_3d(on);
}

bool css_transform_matrix_current_ask(lxb_dom_element_t *el)
{
    lxb_dom_element_t *on;

    DCHECK(el != NULL, "css-transforms-1 §2's current transformation matrix was asked of no element");
    for (on = el; on != NULL; on = css_parent_element(on))
        if (!css_tm_element_ask(on, on != el && css_tm_child_participates(on))) return false;
    return true;
}

CssTransformMatrix css_transform_matrix_current(lxb_dom_element_t *el)
{
    CssTransformMatrix out = css_transform_matrix_identity();
    CssTransformMatrix3d acc = css_tm4_identity(), m;
    lxb_dom_element_t *on;
    bool open = false;

    DCHECK(el != NULL, "css-transforms-1 §2's current transformation matrix was asked for with no element");
    /* "The current transformation matrix is computed by post-multiplying all transformation matrices starting
       from the viewport coordinate system and ending with the transformation matrix of an element": walked
       upward, each ancestor is the `outer` operand. Every element's matrix is already anchored at its own box
       in the initial containing block's space, so the product needs no further conjugation. Within a 3D
       rendering context the 4x4s accumulate (css-transforms-2 §4.1.4's accumulated 3D transformation
       matrix); a run of them is flattened where the next ancestor's used transform-style is flat, which
       §4.1.3 "Transformed element hierarchies" states as "create a flattened representation of their content". */
    for (on = el; on != NULL; on = css_parent_element(on)) {
        if (css_tm_element4(on, on != el && css_tm_child_participates(on), &m)) {
            acc = css_tm4_multiply(&m, &acc);
            open = true;
        }
        if (open && !css_tm_child_participates(css_parent_element(on))) {
            out = css_transform_matrix_multiply(css_tm4_flatten(&acc), out);
            acc = css_tm4_identity();
            open = false;
        }
    }
    return out;
}
