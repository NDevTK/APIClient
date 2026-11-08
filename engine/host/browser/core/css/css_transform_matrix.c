/* css-transforms-1 §2 "The Transform Rendering Model". See css_transform_matrix.h for which edition's section
   numbers these are, for why §3.2's two-step product is NOT this algorithm, and for why the linear part of the
   matrix is a `double` while its translation is a `CssPx`.
   A list holding a 3D function whose matrix is not a 2D matrix crashes in dev; its release arm makes that
   element's matrix the identity, so a release build reports the element's own box untransformed rather than
   a box mapped through part of its list. */
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_dimension.h"
#include "core/css/css_length.h"
#include "core/css/css_transform.h"
#include "core/css/css_transform_function.h"
#include "core/css/css_transform_matrix.h"
#include "core/css/css_transform_origin.h"
#include "core/layout/flow_position.h"
#include "core/layout/used_value.h"

CssTransformMatrix css_transform_matrix_identity(void)
{
    CssTransformMatrix out;

    out.a = 1.0; out.b = 0.0; out.c = 0.0; out.d = 1.0;
    out.e = css_px(0.0); out.f = css_px(0.0);
    return out;
}

CssTransformMatrix css_transform_matrix_multiply(CssTransformMatrix outer, CssTransformMatrix inner)
{
    CssTransformMatrix out;

    /* THE LINEAR PART IS A PRODUCT OF PURE RATIOS and carries no environment fact, for the reason the header
       states: §7.1 "2D Transform Functions" gives every argument that reaches it a `<number>` or an
       `[ <angle> | <zero> ]`, both of which are literals an author wrote. */
    out.a = outer.a * inner.a + outer.c * inner.b;
    out.b = outer.b * inner.a + outer.d * inner.b;
    out.c = outer.a * inner.c + outer.c * inner.d;
    out.d = outer.b * inner.c + outer.d * inner.d;
    /* THE TRANSLATION IS `inner`'s MAPPED THROUGH `outer`'s LINEAR PART AND THEN OFFSET BY `outer`'s, which is
       what the third column of the product is. `css_px_scale` is the multiplication because the scalar is the
       ratio and the length is the operand — the other spelling would ask core/css/css_length.h to multiply two
       lengths, which is Intersection Observer's AREA and carries a different dimension. */
    out.e = css_px_add(css_px_add(css_px_scale(inner.e, outer.a), css_px_scale(inner.f, outer.c)), outer.e);
    out.f = css_px_add(css_px_add(css_px_scale(inner.e, outer.b), css_px_scale(inner.f, outer.d)), outer.f);
    return out;
}

void css_transform_matrix_map(CssTransformMatrix m, CssPx x, CssPx y, CssPx *out_x, CssPx *out_y)
{
    DCHECK(out_x != NULL && out_y != NULL,
           "css-transforms-1 §2 \"The Transform Rendering Model\"'s \"post-multiply the transformation matrix "
           "TM of the element by p local\" was asked with nowhere to put the mapped point");
    *out_x = css_px_add(css_px_add(css_px_scale(x, m.a), css_px_scale(y, m.c)), m.e);
    *out_y = css_px_add(css_px_add(css_px_scale(x, m.b), css_px_scale(y, m.d)), m.f);
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
   relative to the width of the reference box") and §4 ("A percentage for the horizontal offset is relative to
   the width of the reference box"). The percentage is a used value, since css-values-4 §10.11 "Computed Value"
   leaves it unresolved at computed-value time. `ref` is NULL on the pass that reads only the linear part. */
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

/* §12's 2D rotation by `deg`: a = m11 = cos, b = m12 = sin, c = m21 = −sin, d = m22 = cos. */
static CssTransformMatrix css_tm_rotation(double deg)
{
    CssTransformMatrix out = css_transform_matrix_identity();
    double s, c;

    css_tm_sin_cos(deg, &s, &c);
    out.a = c; out.b = s; out.c = -s; out.d = c;
    return out;
}

/* A computed length that is exactly zero with no environment fact behind it, so a 4x4 entry it fills is the
   2D matrix's 0 on every viewport. */
static bool css_tm_length_is_zero(const CssLength *len)
{
    return len->kind == CSS_LENGTH_ABSOLUTE && len->px.env == CSS_ENV_NONE && len->px.px == 0.0;
}

/* §12 "Mathematical Description of Transform Functions"' matrix for one `<transform-function>`, as the six
   parameters a = m11, b = m12, c = m21, d = m22, e = m41, f = m42 (css-transforms-2 §3 "Two Dimensional
   Subset"'s reduction). A css-transforms-2 §12.2 "3D Transform Functions" function is answered when its
   §16 "Mathematical Description of Transform Functions" matrix is a 2D matrix in css-transforms-1
   Terminology's sense; false otherwise, after the crash. `ref` is the reference box (width, height), or NULL
   on the linear-part pass. */
static bool css_tm_of_function(const CssTransformFunction *fn, const CssPx *ref, CssTransformMatrix *out)
{
    const CssPx *w = ref, *h = ref ? &ref[1] : NULL;
    const double *n = fn->number;
    double z;

    DCHECK(fn->argc > 0, "css-transforms-1 §3's `<transform-list>` is `<transform-function>+` and every §7.1 "
                         "and css-transforms-2 §12.2 function has a required argument");
    *out = css_transform_matrix_identity();
    switch (fn->kind) {
    case CSS_TF_MATRIX:
        out->a = n[0]; out->b = n[1]; out->c = n[2]; out->d = n[3];
        out->e = css_px(n[4]); out->f = css_px(n[5]);
        return true;
    /* §7.1: "If <ty> is not provided, ty has zero as a value". */
    case CSS_TF_TRANSLATE:
        out->e = css_tm_length_px(&fn->length[0], w);
        if (fn->argc > 1) out->f = css_tm_length_px(&fn->length[1], h);
        return true;
    case CSS_TF_TRANSLATE_X:
        out->e = css_tm_length_px(&fn->length[0], w);
        return true;
    case CSS_TF_TRANSLATE_Y:
        out->f = css_tm_length_px(&fn->length[0], h);
        return true;
    /* §7.1: "If the second parameter is not provided, it takes a value equal to the first." */
    case CSS_TF_SCALE:
        out->a = n[0];
        out->d = fn->argc > 1 ? n[1] : n[0];
        return true;
    case CSS_TF_SCALE_X:
        out->a = n[0];
        return true;
    case CSS_TF_SCALE_Y:
        out->d = n[0];
        return true;
    case CSS_TF_ROTATE:
    /* css-transforms-2 §12.2: "same as rotate3d(0, 0, 1, <angle>), which is a 3d transform equivalent to the
       2d transform rotate(<angle>)". */
    case CSS_TF_ROTATE_Z:
        *out = css_tm_rotation(css_tm_angle_deg(fn, 0));
        return true;
    /* §12's skew: m21 = tan(alpha) and m12 = tan(beta); §7.1: "If the second parameter is not provided, it
       has a zero value." */
    case CSS_TF_SKEW:
        out->c = css_tm_tan(css_tm_angle_deg(fn, 0));
        if (fn->argc > 1) out->b = css_tm_tan(css_tm_angle_deg(fn, 1));
        return true;
    case CSS_TF_SKEW_X:
        out->c = css_tm_tan(css_tm_angle_deg(fn, 0));
        return true;
    case CSS_TF_SKEW_Y:
        out->b = css_tm_tan(css_tm_angle_deg(fn, 0));
        return true;
    /* "16 values in column-major order": m11, m12, m13, m14, m21, …, m44. */
    case CSS_TF_MATRIX3D:
        if (n[2] == 0.0 && n[3] == 0.0 && n[6] == 0.0 && n[7] == 0.0 && n[8] == 0.0 && n[9] == 0.0 &&
            n[10] == 1.0 && n[11] == 0.0 && n[14] == 0.0 && n[15] == 1.0) {
            out->a = n[0]; out->b = n[1]; out->c = n[4]; out->d = n[5];
            out->e = css_px(n[12]); out->f = css_px(n[13]);
            return true;
        }
        break;
    case CSS_TF_TRANSLATE3D:
        if (css_tm_length_is_zero(&fn->length[2])) {
            out->e = css_tm_length_px(&fn->length[0], w);
            out->f = css_tm_length_px(&fn->length[1], h);
            return true;
        }
        break;
    case CSS_TF_TRANSLATE_Z:
        if (css_tm_length_is_zero(&fn->length[0])) return true;
        break;
    case CSS_TF_SCALE3D:
        if (n[2] == 1.0) {
            out->a = n[0];
            out->d = n[1];
            return true;
        }
        break;
    case CSS_TF_SCALE_Z:
        if (n[0] == 1.0) return true;
        break;
    /* §16's rotation over the normalized [x, y, z]: about [0, 0, ±1] it is the Z rotation by ±alpha, and "A
       direction vector that cannot be normalized, such as [0,0,0], will cause the rotation to not be
       applied." */
    case CSS_TF_ROTATE3D:
        if (n[0] == 0.0 && n[1] == 0.0) {
            z = n[2];
            if (z != 0.0) *out = css_tm_rotation(z > 0.0 ? css_tm_angle_deg(fn, 0) : -css_tm_angle_deg(fn, 0));
            return true;
        }
        break;
    /* About the X or Y axis, a 2D matrix exactly when sc and sq are 0, a whole number of turns. */
    case CSS_TF_ROTATE_X:
    case CSS_TF_ROTATE_Y: {
        double s, c;

        css_tm_sin_cos(css_tm_angle_deg(fn, 0), &s, &c);
        if (s == 0.0 && c == 1.0) return true;
        break;
    }
    /* "If the parameter d is none it is treated as infinity (and the resulting matrix is the identity
       matrix)." */
    case CSS_TF_PERSPECTIVE:
        if (fn->perspective_none) return true;
        break;
    }
    DFAIL("css-transforms-2 §12.2 \"3D Transform Functions\": this element's list holds a 3D function whose "
          "§16 \"Mathematical Description of Transform Functions\" matrix is not a 2D matrix, and this "
          "component's matrix is the 3x2 2D one. BUILD css-transforms-2 §6 \"Current Transformation Matrix\"'s "
          "4x4 here and in css_transform_matrix.h, with §4.1 \"3D Transform Rendering\"'s flattening where a "
          "consumer maps a rectangle");
    return false;
}

/* The matrix of every function in `list`, post-multiplied left to right: §2's step 3, "Multiply by each of
   the transform functions in transform property from left to right". False when one is not a 2D matrix. */
static bool css_tm_of_list(const CssTransformList *list, const CssPx *ref, CssTransformMatrix *out)
{
    CssTransformMatrix fm;
    size_t i;

    *out = css_transform_matrix_identity();
    for (i = 0; i < list->n; i++) {
        if (!css_tm_of_function(&list->fn[i], ref, &fm)) return false;
        *out = css_transform_matrix_multiply(*out, fm);
    }
    return true;
}

static bool css_tm_linear_is_identity(CssTransformMatrix m)
{
    return m.a == 1.0 && m.b == 0.0 && m.c == 0.0 && m.d == 1.0;
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

/* What `el`'s matrix reads of layout, decided from its computed values alone. `anchored` is false when the
   list's linear part is the identity: §2's origin bracket T(o)·M·T(−o) is then M exactly for every o, and so
   is the conjugation by the element's own position, so neither is read. */
typedef struct {
    bool               applies;      /* a transformable element with a `<transform-list>` of 2D matrices */
    CssTransformList   list;
    CssTransformOrigin origin;
    bool               anchored;
    bool               read_x, read_y;   /* the reference box's width and height */
    bool               content_box;
} CssTmPlan;

static void css_tm_plan(lxb_dom_element_t *el, CssTmPlan *p)
{
    CssTransformMatrix linear;

    memset(p, 0, sizeof *p);
    /* §3 "The transform Property"'s `Applies to: transformable elements` first, so a declaration on a box it
       does not apply to moves nothing, as core/css/css_transform.h states. */
    if (!css_transform_is_transformable(el) || !css_computed_transform_list(el, &p->list)) return;
    if (!css_tm_of_list(&p->list, NULL, &linear)) {
        css_transform_list_free(&p->list);
        return;
    }
    p->applies = true;
    p->anchored = !css_tm_linear_is_identity(linear);
    css_tm_list_reads(&p->list, &p->read_x, &p->read_y);
    p->content_box = css_computed_transform_box_is_content(el);
    if (!p->anchored) return;
    p->origin = css_computed_transform_origin(el);
    p->read_x |= p->origin.x.kind != CSS_LENGTH_ABSOLUTE;
    p->read_y |= p->origin.y.kind != CSS_LENGTH_ABSOLUTE;
}

CssTransformMatrix css_transform_matrix_of_element(lxb_dom_element_t *el)
{
    CssTransformMatrix m;
    CssTmPlan p;
    CssPx ref[2] = { css_px(0.0), css_px(0.0) }, ax, ay, sx, sy;
    FlowPoint corner;
    bool ok;

    DCHECK(el != NULL, "css-transforms-1 §2's transformation matrix was asked for with no element");
    css_tm_plan(el, &p);
    if (!p.applies) return css_transform_matrix_identity();
    if (p.read_x) ref[0] = css_tm_reference_size(el, false);
    if (p.read_y) ref[1] = css_tm_reference_size(el, true);
    ok = css_tm_of_list(&p.list, ref, &m);
    css_transform_list_free(&p.list);
    DCHECK(ok, "a transform list css_tm_plan read as 2D matrices was not one on its second reading");
    if (!p.anchored) return m;
    /* §2's steps 2 and 4 bracket the product by the origin, which §4 measures from the reference box's top-left
       corner and §5 places there ("A reference box adds an additional offset to the origin"). The chain is
       composed in the initial containing block's space, so the anchor is that corner's layout position plus
       the origin: T(A)·M·T(−A) maps p to M(p − A) + A, whose translation is M's plus A − L·A. */
    corner = flow_border_box_origin(el);
    ax = css_px_add(corner.x, css_tm_length_px(&p.origin.x, &ref[0]));
    ay = css_px_add(corner.y, css_tm_length_px(&p.origin.y, &ref[1]));
    if (p.content_box) {
        ax = css_px_add(ax, used_value_leading_edge_px(el, false));
        ay = css_px_add(ay, used_value_leading_edge_px(el, true));
    }
    sx = css_px_sub(ax, css_px_add(css_px_scale(ax, m.a), css_px_scale(ay, m.c)));
    sy = css_px_sub(ay, css_px_add(css_px_scale(ax, m.b), css_px_scale(ay, m.d)));
    m.e = css_px_add(m.e, sx);
    m.f = css_px_add(m.f, sy);
    return m;
}

/* The layout `el`'s matrix reads, asked: false ("not yet") when a question was deferred. */
static bool css_tm_element_ask(lxb_dom_element_t *el)
{
    CssTmPlan p;
    FlowPoint corner;
    CssPx unused;
    bool done;

    css_tm_plan(el, &p);
    if (!p.applies) return true;
    css_transform_list_free(&p.list);
    done = (!p.read_x || css_tm_reference_size_ask(el, false)) && (!p.read_y || css_tm_reference_size_ask(el, true));
    if (done && p.anchored) {
        done = flow_border_box_origin_ask(el, &corner);
        if (done && p.content_box)
            done = used_value_px_ask(el, "padding-left", &unused) && used_value_px_ask(el, "padding-top", &unused);
    }
    return done;
}

bool css_transform_matrix_current_ask(lxb_dom_element_t *el)
{
    lxb_dom_element_t *on;

    DCHECK(el != NULL, "css-transforms-1 §2's current transformation matrix was asked of no element");
    for (on = el; on != NULL; on = css_parent_element(on))
        if (!css_tm_element_ask(on)) return false;
    return true;
}

CssTransformMatrix css_transform_matrix_current(lxb_dom_element_t *el)
{
    CssTransformMatrix out = css_transform_matrix_identity();
    lxb_dom_element_t *on;

    DCHECK(el != NULL, "css-transforms-1 §2's current transformation matrix was asked for with no element");
    /* "The current transformation matrix is computed by post-multiplying all transformation matrices starting
       from the viewport coordinate system and ending with the transformation matrix of an element": walked
       upward, each ancestor is the `outer` operand. Every element's matrix is already anchored at its own box
       in the initial containing block's space, so the product needs no further conjugation. */
    for (on = el; on != NULL; on = css_parent_element(on))
        out = css_transform_matrix_multiply(css_transform_matrix_of_element(on), out);
    return out;
}
