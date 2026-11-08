/* Geometry Interfaces §6 "The DOMMatrix interfaces". See dom_matrix.h.
 *
 * The sixteen elements are stored in §6.3's column-major order: element m{i}{j} is `m[(i-1)*4 + (j-1)]`, which is
 * the entry in row j and column i of the matrix as a column vector is pre-multiplied by it (§2.1), so m41 and m42
 * are the translation. Every algorithm computes on the elements' examples in doubles; where any operand is
 * unknown external input each result element is concolic_new_derived over the operands and the element's index,
 * and a decision inside an algorithm (is 2D, a non-finite element) is taken on the examples. */
#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/css/css_length.h"
#include "core/css/css_transform_function.h"
#include "core/css/css_transform_matrix.h"
#include "core/geometry/dom_matrix.h"
#include "core/geometry/dom_point.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/concolic.h"
#include "solver/cow.h"
#include <lexbor/core/base.h>   /* lexbor_calloc: the allocator the engine gives its refusal edge */

/* §6's internal member variables, which "must not be exposed in any way". */
typedef struct {
    JSValue m[16];
    bool is2d;
} DomMatrixBox;

#define DM_OFF(k) (uint16_t)offsetof(DomMatrixBox, m[k])
static const uint16_t DOM_MATRIX_OFF[] = {
    DM_OFF(0),  DM_OFF(1),  DM_OFF(2),  DM_OFF(3),  DM_OFF(4),  DM_OFF(5),  DM_OFF(6),  DM_OFF(7),
    DM_OFF(8),  DM_OFF(9),  DM_OFF(10), DM_OFF(11), DM_OFF(12), DM_OFF(13), DM_OFF(14), DM_OFF(15),
};
#undef DM_OFF
static const CowRecord DOM_MATRIX_REC = { sizeof(DomMatrixBox), DOM_MATRIX_OFF, 16 };

enum { DM_READONLY = 0, DM_MUTABLE = 1 };

/* §6.4's attributes in declaration order: the six 2D aliases, then m11 to m44. The getter's magic is the element
   index each names. */
static const char *const DM_ATTR[22] = {
    "a", "b", "c", "d", "e", "f",
    "m11", "m12", "m13", "m14", "m21", "m22", "m23", "m24",
    "m31", "m32", "m33", "m34", "m41", "m42", "m43", "m44",
};
static const int DM_ATTR_ELEMENT[22] = { 0, 1, 4, 5, 12, 13, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

static JSClassID g_ro_class, g_mx_class;
static int g_id_ctor[2] = { -1, -1 }, g_id_from_matrix[2] = { -1, -1 };
static int g_id_from_f32[2] = { -1, -1 }, g_id_from_f64[2] = { -1, -1 };
static int g_id_set[22];
static int g_id_method[64];

/* ---- the record ------------------------------------------------------------------------------------------ */

static DomMatrixBox *dm_box(JSValueConst v)
{
    DomMatrixBox *b = JS_GetOpaque(v, g_ro_class);

    if (!b) b = JS_GetOpaque(v, g_mx_class);
    if (b) cow_capture_host_record(v, b, &DOM_MATRIX_REC);
    return b;
}

static DomMatrixBox *dm_this(JSContext *ctx, JSValueConst this_val)
{
    DomMatrixBox *b = dm_box(this_val);

    if (!b) JS_ThrowTypeError(ctx, "a DOMMatrix member was reached on something that is not a DOMMatrix");
    return b;
}

static void dm_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    DomMatrixBox *b = JS_GetAnyOpaque(val, &id);
    int k;

    DCHECK(b != NULL, "a DOMMatrix was finalized with no internal member variables — dm_alloc attaches the "
                      "record before the object can reach anything that would free it");
    for (k = 0; k < 16; k++) JS_FreeValueRT(rt, b->m[k]);
    free(b);
}

static void dm_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID id = 0;
    DomMatrixBox *b = JS_GetAnyOpaque(val, &id);
    int k;

    DCHECK(b != NULL, "a DOMMatrix was marked with no internal member variables");
    for (k = 0; k < 16; k++) JS_MarkValue(rt, b->m[k], mark_func);
}

static void dm_set_element(JSContext *ctx, DomMatrixBox *b, int k, JSValue v)
{
    DCHECK(JS_IsNumber(v) || concolic_is(v),
           "a DOMMatrix element was given something that is neither a Number nor unknown external input");
    cow_record_set_at(ctx, b, &DOM_MATRIX_REC, &b->m[k], v, __FILE__, __LINE__);
}

/* A new matrix of `which` with the sixteen elements and is 2D. CONSUMES the elements. */
static JSValue dm_alloc(JSContext *ctx, int which, JSValue m[16], bool is2d)
{
    JSClassID cls = which == DM_MUTABLE ? g_mx_class : g_ro_class;
    JSValue proto, obj;
    DomMatrixBox *b;
    int k;

    DCHECK(cls != 0, "a matrix was built before dom_matrix_init declared its classes");
    proto = JS_GetClassProto(ctx, cls);
    DCHECK(!JS_IsNull(proto), "a matrix was built in a realm that never ran its prototype install");
    obj = JS_NewObjectProtoClass(ctx, proto, cls);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "a DOMMatrix could not be allocated");
    b = lexbor_calloc(1, sizeof(*b));
    CHECK(b != NULL, "the DOMMatrix record allocation failed");
    for (k = 0; k < 16; k++) {
        DCHECK(JS_IsNumber(m[k]) || concolic_is(m[k]),
               "a DOMMatrix element was minted from something that is neither a Number nor unknown input");
        b->m[k] = m[k];
    }
    b->is2d = is2d;
    JS_SetOpaque(obj, b);
    return obj;
}

static void dm_identity_values(JSContext *ctx, JSValue m[16])
{
    int k;

    for (k = 0; k < 16; k++) m[k] = JS_NewFloat64(ctx, k % 5 == 0 ? 1.0 : 0.0);
}

/* §6.3's "create a 2d matrix" with `six` (m11, m12, m21, m22, m41, m42) into `m`. CONSUMES the six. */
static void dm_2d_values(JSContext *ctx, JSValue six[6], JSValue m[16])
{
    static const int AT[6] = { 0, 1, 4, 5, 12, 13 };
    int i;

    dm_identity_values(ctx, m);
    for (i = 0; i < 6; i++) m[AT[i]] = six[i];
}

/* ---- arithmetic over examples ---------------------------------------------------------------------------- */

#define DM_OPS_MAX 40

/* The operands one algorithm reads, and whether any is unknown and every unknown has a numeric example. */
typedef struct {
    JSValueConst ops[DM_OPS_MAX + 1];
    int n;
    bool unknown, exact;
} DmOps;

static void dm_ops_init(DmOps *o)
{
    o->n = 0;
    o->unknown = false;
    o->exact = true;
}

/* `v`'s number as the algorithm reads it, recording it as an operand. */
static double dm_read(JSContext *ctx, DmOps *o, JSValueConst v)
{
    double d = NAN;

    DCHECK(o->n < DM_OPS_MAX, "a DOMMatrix algorithm read more operands than DM_OPS_MAX");
    o->ops[o->n++] = v;
    if (concolic_is(v)) {
        JSValue e = concolic_example(ctx, v);

        o->unknown = true;
        if (JS_IsNumber(e)) JS_ToFloat64(ctx, &d, e);
        else o->exact = false;
        JS_FreeValue(ctx, e);
        return d;
    }
    DCHECK(JS_IsNumber(v), "a DOMMatrix operand is neither a Number nor unknown external input");
    JS_ToFloat64(ctx, &d, v);
    return d;
}

static void dm_read_box(JSContext *ctx, DmOps *o, const DomMatrixBox *b, double a[16])
{
    int k;

    for (k = 0; k < 16; k++) a[k] = dm_read(ctx, o, b->m[k]);
}

/* An optional `unrestricted double` argument with its IDL default. */
static double dm_arg(JSContext *ctx, DmOps *o, int argc, JSValueConst *argv, int i, double dflt)
{
    if (i >= argc || JS_IsUndefined(argv[i])) return dflt;
    return dm_read(ctx, o, argv[i]);
}

/* Result element `k` of the algorithm `op`: a Number where every operand is one, else derived over them. */
static JSValue dm_result(JSContext *ctx, DmOps *o, const char *op, int k, double d)
{
    JSValue v;

    if (!o->unknown) return JS_NewFloat64(ctx, d);
    o->ops[o->n] = JS_NewInt32(ctx, k);
    v = concolic_new_derived(ctx, op, o->ops, o->n + 1, o->exact ? JS_NewFloat64(ctx, d) : JS_UNDEFINED);
    DCHECK(!JS_IsUninitialized(v), "concolic_new_derived found no unknown among operands dm_read recorded one in");
    return v;
}

static void dm_mul(const double a[16], const double b[16], double out[16])
{
    int r, c, k;

    for (c = 0; c < 4; c++)
        for (r = 0; r < 4; r++) {
            double s = 0.0;

            for (k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = s;
        }
}

static void dm_set_identity(double a[16])
{
    int k;

    for (k = 0; k < 16; k++) a[k] = k % 5 == 0 ? 1.0 : 0.0;
}

/* `b`'s elements become `c`, each derived over `o` where an operand is unknown. */
static void dm_store(JSContext *ctx, DomMatrixBox *b, DmOps *o, const char *op, const double c[16])
{
    int k;

    for (k = 0; k < 16; k++) dm_set_element(ctx, b, k, dm_result(ctx, o, op, k, c[k]));
}

/* `b` post-multiplied (or with `pre`, pre-multiplied) by `t`, whose operands `o` already holds with `a`, `b`'s. */
static void dm_store_product(JSContext *ctx, DomMatrixBox *b, DmOps *o, const char *op, const double a[16],
                             const double t[16], bool pre)
{
    double c[16];

    if (pre) dm_mul(t, a, c);
    else dm_mul(a, t, c);
    dm_store(ctx, b, o, op, c);
}

/* css-transforms-2 §16's rotate3d() matrix about (x, y, z) by `deg` degrees, the axis normalized; identity for a
   zero axis. */
static void dm_rotation(double x, double y, double z, double deg, double out[16])
{
    double len = sqrt(x * x + y * y + z * z), half = deg * M_PI / 360.0, sc, sq;

    dm_set_identity(out);
    if (len == 0.0) return;
    x /= len;
    y /= len;
    z /= len;
    sc = sin(half) * cos(half);
    sq = sin(half) * sin(half);
    out[0] = 1.0 - 2.0 * (y * y + z * z) * sq;
    out[1] = 2.0 * (x * y * sq + z * sc);
    out[2] = 2.0 * (x * z * sq - y * sc);
    out[4] = 2.0 * (x * y * sq - z * sc);
    out[5] = 1.0 - 2.0 * (x * x + z * z) * sq;
    out[6] = 2.0 * (y * z * sq + x * sc);
    out[8] = 2.0 * (x * z * sq + y * sc);
    out[9] = 2.0 * (y * z * sq - x * sc);
    out[10] = 1.0 - 2.0 * (x * x + y * y) * sq;
}

static void dm_translation(double tx, double ty, double tz, double out[16])
{
    dm_set_identity(out);
    out[12] = tx;
    out[13] = ty;
    out[14] = tz;
}

static void dm_scaling(double sx, double sy, double sz, double out[16])
{
    dm_set_identity(out);
    out[0] = sx;
    out[5] = sy;
    out[10] = sz;
}

/* The inverse of `a`, false where it is singular (a zero or non-finite determinant). */
static bool dm_invert(const double a[16], double inv[16])
{
    double d;
    int k;

    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] +
             a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] -
             a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] +
             a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] -
              a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] -
             a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] +
             a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] -
             a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] +
              a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] +
             a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] -
             a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] +
              a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] -
              a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] -
             a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] +
             a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] -
              a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] +
              a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    d = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (d == 0.0 || !isfinite(d)) return false;
    for (k = 0; k < 16; k++) inv[k] /= d;
    return true;
}

/* ---- §6.2 parsing a string into an abstract matrix ------------------------------------------------------- */

/* The CSS Syntax comments of `s` removed and the whitespace trimmed, OWNED. */
static char *dm_strip(const char *s)
{
    size_t n = strlen(s), o = 0, i;
    char *out = lexbor_malloc(n + 1);

    CHECK(out != NULL, "Geometry Interfaces §6.2: OOM copying a transform list");
    for (i = 0; i < n;) {
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
            const char *e = strstr(s + i + 2, "*/");

            i = e ? (size_t)(e - s) + 2 : n;
            out[o++] = ' ';
            continue;
        }
        out[o++] = s[i++];
    }
    while (o > 0 && isspace((unsigned char)out[o - 1])) o--;
    out[o] = '\0';
    for (i = 0; out[i] != '\0' && isspace((unsigned char)out[i]); i++)
        ;
    memmove(out, out + i, o - i + 1);
    return out;
}

/* Whether `s` writes a `<length>` without an absolute unit, or a percentage: §6.2's "any <transform-function> has
   <length> values without absolute length units". Every numeric token is classified by its unit. */
static bool dm_has_relative(const char *s)
{
    const char *p = s;

    while (*p != '\0') {
        const char *u;
        double px;

        if (!(isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))) ||
            (p > s && (isalnum((unsigned char)p[-1]) || p[-1] == '-' || p[-1] == '_'))) {
            p++;
            continue;
        }
        while (isdigit((unsigned char)*p) || *p == '.') p++;
        if ((*p == 'e' || *p == 'E') && (isdigit((unsigned char)p[1]) ||
                                         ((p[1] == '+' || p[1] == '-') && isdigit((unsigned char)p[2])))) {
            p += 2;
            while (isdigit((unsigned char)*p)) p++;
        }
        if (*p == '%') return true;
        u = p;
        while (isalpha((unsigned char)*p)) p++;
        if (p > u && css_length_is_length_unit(u, (size_t)(p - u)) &&
            !css_length_absolute_px(u, (size_t)(p - u), 1.0, &px))
            return true;
    }
    return false;
}

static CssPx dm_no_font(void *ctx, CssFontMetric which)
{
    (void)ctx; (void)which;
    DFAIL("Geometry Interfaces §6.2 reached a font-relative unit, which dm_has_relative refuses before the parse");
    return css_px(0.0);
}

/* §6.2's steps over `text`: false for failure, which the caller throws as a SyntaxError. */
static bool dm_parse(JSContext *ctx, const char *text, double out[16], bool *two_d)
{
    CssFontMetrics font = { dm_no_font, NULL };
    CssTransformList list = { NULL, 0 };
    CssTransformMatrix3d m;
    char *s;
    size_t i;
    int r, c;
    bool ok;

    /* Step 1: "If transformList is the empty string, set it to the string "matrix(1, 0, 0, 1, 0, 0)"." */
    if (text[0] == '\0') {
        dm_set_identity(out);
        *two_d = true;
        return true;
    }
    s = dm_strip(text);
    /* Step 3: "If parsedValue is none, set parsedValue to a <transform-list> containing a single identity
       matrix." */
    if (strcasecmp(s, "none") == 0) {
        free(s);
        dm_set_identity(out);
        *two_d = true;
        return true;
    }
    /* Step 2's failures: a value outside the grammar, a relative or percentage length, any other keyword. */
    ok = s[0] != '\0' && !dm_has_relative(s) && css_transform_list_parse(ctx, &font, s, &list);
    free(s);
    if (!ok) return false;
    /* Step 4: "If parsedValue consists of any three-dimensional transform functions, set 2dTransform to
       false". css_transform_function.h lists the 3D functions from CSS_TF_MATRIX3D on. */
    *two_d = true;
    for (i = 0; i < list.n; i++)
        if (list.fn[i].kind >= CSS_TF_MATRIX3D) *two_d = false;
    /* Steps 5 and 6: each function's matrix, post-multiplied left to right. */
    m = css_transform_list_matrix3d(&list);
    css_transform_list_free(&list);
    for (c = 0; c < 4; c++)
        for (r = 0; r < 4; r++) out[c * 4 + r] = m.m[r][c].px;
    return true;
}

/* The parsed matrix as sixteen elements: Numbers, or derived over `src` where the string was unknown input. */
static void dm_parsed_values(JSContext *ctx, JSValueConst src, const double a[16], bool two_d, JSValue m[16])
{
    DmOps o;
    int k;

    dm_ops_init(&o);
    if (concolic_is(src)) {
        o.ops[o.n++] = src;
        o.unknown = true;
    }
    for (k = 0; k < 16; k++) m[k] = dm_result(ctx, &o, "DOMMatrix.parse", k, a[k]);
    if (two_d) {
        /* §6.3's "create a 2d matrix" sets the 3D elements to their identity values. */
        static const int THREE_D[10] = { 2, 3, 6, 7, 8, 9, 10, 11, 14, 15 };

        for (k = 0; k < 10; k++) {
            JS_FreeValue(ctx, m[THREE_D[k]]);
            m[THREE_D[k]] = JS_NewFloat64(ctx, THREE_D[k] % 5 == 0 ? 1.0 : 0.0);
        }
    }
}

/* The string `v` (a String, or unknown input whose example is one) parsed into `m`, or a SyntaxError. */
static int dm_parse_value(JSContext *ctx, JSValueConst v, JSValue m[16], bool *two_d)
{
    double a[16];
    const char *s;
    JSValue ex = concolic_is(v) ? concolic_example(ctx, v) : JS_DupValue(ctx, v);
    bool ok;

    if (!JS_IsString(ex)) {
        JS_FreeValue(ctx, ex);
        JS_ThrowTypeError(ctx, "Geometry Interfaces §6.3: the matrix initializer's example is not a string");
        return -1;
    }
    s = JS_ToCString(ctx, ex);
    JS_FreeValue(ctx, ex);
    if (s == NULL) return -1;
    ok = dm_parse(ctx, s, a, two_d);
    JS_FreeCString(ctx, s);
    if (!ok) {
        JS_ThrowDOMException(ctx, "SyntaxError", "the string is not a transform list with absolute lengths");
        return -1;
    }
    dm_parsed_values(ctx, v, a, *two_d, m);
    return 0;
}

/* ---- §6.1 validate and fixup ----------------------------------------------------------------------------- */

static const IdlDictMember DOM_MATRIX_INIT[] = {
    { "a",    IDL_UNRESTRICTED_DOUBLE, false, NULL, 0, NULL, IDL_DEFAULT_NONE },
    { "b",    IDL_UNRESTRICTED_DOUBLE, false, NULL, 0, NULL, IDL_DEFAULT_NONE },
    { "c",    IDL_UNRESTRICTED_DOUBLE, false, NULL, 0, NULL, IDL_DEFAULT_NONE },
    { "d",    IDL_UNRESTRICTED_DOUBLE, false, NULL, 0, NULL, IDL_DEFAULT_NONE },
    { "e",    IDL_UNRESTRICTED_DOUBLE, false, NULL, 0, NULL, IDL_DEFAULT_NONE },
    { "f",    IDL_UNRESTRICTED_DOUBLE, false, NULL, 0, NULL, IDL_DEFAULT_NONE },
    { "m11",  IDL_UNRESTRICTED_DOUBLE, false, NULL, 0, NULL, IDL_DEFAULT_NONE },
    { "m12",  IDL_UNRESTRICTED_DOUBLE, false, NULL, 0, NULL, IDL_DEFAULT_NONE },
    { "m21",  IDL_UNRESTRICTED_DOUBLE, false, NULL, 0, NULL, IDL_DEFAULT_NONE },
    { "m22",  IDL_UNRESTRICTED_DOUBLE, false, NULL, 0, NULL, IDL_DEFAULT_NONE },
    { "m41",  IDL_UNRESTRICTED_DOUBLE, false, NULL, 0, NULL, IDL_DEFAULT_NONE },
    { "m42",  IDL_UNRESTRICTED_DOUBLE, false, NULL, 0, NULL, IDL_DEFAULT_NONE },
    { "is2D", IDL_BOOLEAN_NO_DEFAULT,  false, NULL, 1, NULL, IDL_DEFAULT_NONE },
    { "m13",  IDL_UNRESTRICTED_DOUBLE, false, NULL, 1, NULL, IDL_DEFAULT_ZERO },
    { "m14",  IDL_UNRESTRICTED_DOUBLE, false, NULL, 1, NULL, IDL_DEFAULT_ZERO },
    { "m23",  IDL_UNRESTRICTED_DOUBLE, false, NULL, 1, NULL, IDL_DEFAULT_ZERO },
    { "m24",  IDL_UNRESTRICTED_DOUBLE, false, NULL, 1, NULL, IDL_DEFAULT_ZERO },
    { "m31",  IDL_UNRESTRICTED_DOUBLE, false, NULL, 1, NULL, IDL_DEFAULT_ZERO },
    { "m32",  IDL_UNRESTRICTED_DOUBLE, false, NULL, 1, NULL, IDL_DEFAULT_ZERO },
    { "m33",  IDL_UNRESTRICTED_DOUBLE, false, NULL, 1, NULL, IDL_DEFAULT_ONE },
    { "m34",  IDL_UNRESTRICTED_DOUBLE, false, NULL, 1, NULL, IDL_DEFAULT_ZERO },
    { "m43",  IDL_UNRESTRICTED_DOUBLE, false, NULL, 1, NULL, IDL_DEFAULT_ZERO },
    { "m44",  IDL_UNRESTRICTED_DOUBLE, false, NULL, 1, NULL, IDL_DEFAULT_ONE },
};

const IdlDictDecl DOM_MATRIX_INIT_DECL = {
    "DOMMatrixInit", DOM_MATRIX_INIT, (int)(sizeof(DOM_MATRIX_INIT) / sizeof(DOM_MATRIX_INIT[0]))
};

static double dm_example(JSContext *ctx, JSValueConst v)
{
    DmOps o;

    dm_ops_init(&o);
    return dm_read(ctx, &o, v);
}

/* ECMAScript's SameValueZero over two Numbers' examples. */
static bool dm_same_value_zero(double x, double y)
{
    return (isnan(x) && isnan(y)) || x == y;
}

/* §6.1's "validate and fixup" over a converted DOMMatrixInit `init`, then "create a DOMMatrix from the
   dictionary": the sixteen elements into `m` and is 2D, or -1 with the TypeError live. */
static int dm_from_init(JSContext *ctx, JSValueConst init, JSValue m[16], bool *is2d)
{
    static const struct { const char *alias, *name; int k; double dflt; } PAIRS[6] = {
        { "a", "m11", 0, 1.0 }, { "b", "m12", 1, 0.0 }, { "c", "m21", 4, 0.0 },
        { "d", "m22", 5, 1.0 }, { "e", "m41", 12, 0.0 }, { "f", "m42", 13, 0.0 },
    };
    static const struct { const char *name; int k; double identity; } THREE_D[10] = {
        { "m13", 2, 0.0 }, { "m14", 3, 0.0 }, { "m23", 6, 0.0 }, { "m24", 7, 0.0 }, { "m31", 8, 0.0 },
        { "m32", 9, 0.0 }, { "m33", 10, 1.0 }, { "m34", 11, 0.0 }, { "m43", 14, 0.0 }, { "m44", 15, 1.0 },
    };
    JSValue is2d_v = idl_dict_get(ctx, init, "is2D");
    bool non_identity_3d = false;
    int i, k;

    for (k = 0; k < 16; k++) m[k] = JS_UNDEFINED;
    for (i = 0; i < 6; i++) {
        JSValue alias = idl_dict_get(ctx, init, PAIRS[i].alias), el = idl_dict_get(ctx, init, PAIRS[i].name);

        /* "a and m11 are both present and SameValueZero(a, m11) is false", and the same for each pair. */
        if (!JS_IsUndefined(alias) && !JS_IsUndefined(el) &&
            !dm_same_value_zero(dm_example(ctx, alias), dm_example(ctx, el))) {
            JS_FreeValue(ctx, alias);
            JS_FreeValue(ctx, el);
            for (k = 0; k < 16; k++) JS_FreeValue(ctx, m[k]);
            JS_FreeValue(ctx, is2d_v);
            JS_ThrowTypeError(ctx, "DOMMatrixInit: `%s` and `%s` are both present and differ", PAIRS[i].alias,
                              PAIRS[i].name);
            return -1;
        }
        /* "If m11 is not present then set it to the value of member a, or value 1 if a is also not present." */
        if (!JS_IsUndefined(el)) {
            m[PAIRS[i].k] = el;
            JS_FreeValue(ctx, alias);
        }
        else if (!JS_IsUndefined(alias)) m[PAIRS[i].k] = alias;
        else m[PAIRS[i].k] = JS_NewFloat64(ctx, PAIRS[i].dflt);
    }
    for (i = 0; i < 10; i++) {
        JSValue el = idl_dict_get(ctx, init, THREE_D[i].name);
        double d;

        DCHECK(!JS_IsUndefined(el), "a DOMMatrixInit 3D member carried no value although its IDL states a default");
        d = dm_example(ctx, el);
        if (!(d == THREE_D[i].identity)) non_identity_3d = true;
        m[THREE_D[i].k] = el;
    }
    /* "If is2D is true and" a 3D member is other than its identity value, "throw a TypeError"; if is2D is not
       present such a member sets it false; "If is2D is still not present, set it to true." */
    if (JS_IsBool(is2d_v) && JS_ToBool(ctx, is2d_v) && non_identity_3d) {
        for (k = 0; k < 16; k++) JS_FreeValue(ctx, m[k]);
        JS_ThrowTypeError(ctx, "DOMMatrixInit: is2D is true and a 3D member is not its identity value");
        return -1;
    }
    *is2d = JS_IsBool(is2d_v) ? (bool)JS_ToBool(ctx, is2d_v) : !non_identity_3d;
    JS_FreeValue(ctx, is2d_v);
    return 0;
}

/* ---- §6.3 creation --------------------------------------------------------------------------------------- */

/* A list of `len` elements, 6 or 16 (§6.3's "create a 2d matrix" and "create a 3d matrix"), or a TypeError. The
   list is the converted sequence or the typed array a static was given; neither element read runs page code. */
static JSValue dm_from_list(JSContext *ctx, int which, JSValueConst list, int64_t len)
{
    JSValue m[16], six[6];
    int k;

    if (len != 6 && len != 16)
        return JS_ThrowTypeError(ctx, "a matrix sequence has %lld elements; §6.3 takes 6 or 16", (long long)len);
    if (len == 6) {
        for (k = 0; k < 6; k++) six[k] = JS_GetPropertyUint32(ctx, list, (uint32_t)k);
        dm_2d_values(ctx, six, m);
        return dm_alloc(ctx, which, m, true);
    }
    for (k = 0; k < 16; k++) m[k] = JS_GetPropertyUint32(ctx, list, (uint32_t)k);
    return dm_alloc(ctx, which, m, false);
}

/* §6.3's constructor steps. */
static JSValue js_dm_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue m[16];
    bool two_d;

    (void)this_val;
    /* "If init is omitted": create a 2d matrix with [1, 0, 0, 1, 0, 0]. */
    if (JS_IsUndefined(init)) {
        dm_identity_values(ctx, m);
        return dm_alloc(ctx, magic, m, true);
    }
    if (JS_IsString(init) || concolic_is(init)) {
        /* "If current global object is not a Window object, then throw a TypeError exception." */
        if (realm_global_is_worker(ctx) || realm_global_is_worklet(ctx))
            return JS_ThrowTypeError(ctx, "a DOMMatrix is parsed from a string only in a Window");
        if (dm_parse_value(ctx, init, m, &two_d) < 0) return JS_EXCEPTION;
        return dm_alloc(ctx, magic, m, two_d);
    }
    {
        int64_t len = 0;

        /* The converted sequence is an Array this file's IDL conversion built, whose `length` is data. */
        if (JS_GetLength(ctx, init, &len) < 0) return JS_EXCEPTION;
        return dm_from_list(ctx, magic, init, len);
    }
}

static JSValue js_dm_from_matrix(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue m[16];
    bool is2d;

    (void)this_val;
    if (dm_from_init(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, m, &is2d) < 0) return JS_EXCEPTION;
    return dm_alloc(ctx, magic, m, is2d);
}

/* fromFloat32Array(array32) and fromFloat64Array(array64): "If array32 has 6 elements" a 2d matrix, 16 a 3d
   one, otherwise a TypeError. */
static JSValue js_dm_from_array(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    size_t offset = 0, bytes = 0, per = 0;
    JSValue buf;

    (void)this_val;
    DCHECK(argc > 0, "fromFloat32Array ran with no argument; its one argument is required");
    /* The element count from the view's own slots: `length` is a prototype getter a page can replace. */
    buf = JS_GetTypedArrayBuffer(ctx, argv[0], &offset, &bytes, &per);
    if (JS_IsException(buf)) return buf;
    JS_FreeValue(ctx, buf);
    DCHECK(per > 0, "a Float32Array or Float64Array reported no element size");
    return dm_from_list(ctx, magic, argv[0], (int64_t)(bytes / per));
}

/* ---- §6.4 attributes ------------------------------------------------------------------------------------- */

static JSValue js_dm_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    DomMatrixBox *b = dm_this(ctx, this_val);

    if (!b) return JS_EXCEPTION;
    DCHECK(magic >= 0 && magic < 22, "a DOMMatrix attribute ran with a magic §6.4 does not declare");
    return JS_DupValue(ctx, b->m[DM_ATTR_ELEMENT[magic]]);
}

/* §6.4: setting m13 "must set the m13 element to the new value and, if the new value is not 0 or -0, set is 2D
   to false", m33 and m44 the same against 1, and the six 2D elements and their aliases set the element alone. */
static JSValue js_dm_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    DomMatrixBox *b = dm_this(ctx, this_val);
    int k;
    double d;

    if (!b) return JS_EXCEPTION;
    DCHECK(magic >= 0 && magic < 22, "a DOMMatrix setter ran with a magic §6.4 does not declare");
    k = DM_ATTR_ELEMENT[magic];
    if (k == 2 || k == 3 || (k >= 6 && k <= 11) || k == 14 || k == 15) {
        d = dm_example(ctx, val);
        if (!(d == ((k == 10 || k == 15) ? 1.0 : 0.0))) b->is2d = false;
    }
    dm_set_element(ctx, b, k, JS_DupValue(ctx, val));
    return JS_UNDEFINED;
}

static JSValue js_dm_is2d(JSContext *ctx, JSValueConst this_val, int magic)
{
    DomMatrixBox *b = dm_this(ctx, this_val);

    (void)magic;
    if (!b) return JS_EXCEPTION;
    return JS_NewBool(ctx, b->is2d);
}

/* §6.4's isIdentity: m11, m22, m33 and m44 are 1 and every other element 0 or -0. */
static JSValue js_dm_is_identity(JSContext *ctx, JSValueConst this_val, int magic)
{
    DomMatrixBox *b = dm_this(ctx, this_val);
    DmOps o;
    double a[16];
    bool id = true;
    int k;

    (void)magic;
    if (!b) return JS_EXCEPTION;
    dm_ops_init(&o);
    dm_read_box(ctx, &o, b, a);
    for (k = 0; k < 16; k++)
        if (!(a[k] == (k % 5 == 0 ? 1.0 : 0.0))) id = false;
    if (!o.unknown) return JS_NewBool(ctx, id);
    return concolic_new_derived(ctx, "DOMMatrixReadOnly.isIdentity", o.ops, o.n,
                                o.exact ? JS_NewBool(ctx, id) : JS_UNDEFINED);
}

/* ---- §6.5 and §6.6 methods ------------------------------------------------------------------------------- */

typedef enum {
    DM_TRANSLATE = 0, DM_SCALE, DM_SCALE_NON_UNIFORM, DM_SCALE3D, DM_ROTATE, DM_ROTATE_FROM_VECTOR,
    DM_ROTATE_AXIS_ANGLE, DM_SKEW_X, DM_SKEW_Y, DM_MULTIPLY, DM_PRE_MULTIPLY, DM_FLIP_X, DM_FLIP_Y, DM_INVERT,
    DM_OP_COUNT
} DmOp;

static const char *const DM_OP_NAME[DM_OP_COUNT] = {
    "DOMMatrix.translateSelf", "DOMMatrix.scaleSelf", "DOMMatrix.scaleNonUniform", "DOMMatrix.scale3dSelf",
    "DOMMatrix.rotateSelf", "DOMMatrix.rotateFromVectorSelf", "DOMMatrix.rotateAxisAngleSelf",
    "DOMMatrix.skewXSelf", "DOMMatrix.skewYSelf", "DOMMatrix.multiplySelf", "DOMMatrix.preMultiplySelf",
    "DOMMatrix.flipX", "DOMMatrix.flipY", "DOMMatrix.invertSelf",
};

/* The `*Self` algorithm `op` performed on `b` with the method's arguments: -1 with a throw live (a dictionary
   validate and fixup refused). */
static int dm_perform(JSContext *ctx, DomMatrixBox *b, DmOp op, int argc, JSValueConst *argv)
{
    DmOps o;
    double a[16], t[16], u[16];
    const char *name = DM_OP_NAME[op];

    dm_ops_init(&o);
    dm_read_box(ctx, &o, b, a);
    switch (op) {
    case DM_TRANSLATE: {
        /* "Post-multiply a translation transformation"; "If tz is specified and not 0 or -0, set is 2D of the
           current matrix to false." */
        double tz = dm_arg(ctx, &o, argc, argv, 2, 0.0);

        dm_translation(dm_arg(ctx, &o, argc, argv, 0, 0.0), dm_arg(ctx, &o, argc, argv, 1, 0.0), tz, t);
        if (!(tz == 0.0)) b->is2d = false;
        dm_store_product(ctx, b, &o, name, a, t, false);
        return 0;
    }
    case DM_SCALE:
    case DM_SCALE_NON_UNIFORM: {
        /* scaleSelf: translate by the origin, "If scaleY is missing, set scaleY to the value of scaleX", the
           scale, then the negated origin; "If scaleZ is not 1, set is 2D of the current matrix to false."
           scaleNonUniform(scaleX, scaleY) is scaleSelf(scaleX, scaleY, 1, 0, 0, 0). */
        double sx = dm_arg(ctx, &o, argc, argv, 0, 1.0);
        double sy = op == DM_SCALE_NON_UNIFORM ? dm_arg(ctx, &o, argc, argv, 1, 1.0)
                                               : dm_arg(ctx, &o, argc, argv, 1, sx);
        double sz = op == DM_SCALE ? dm_arg(ctx, &o, argc, argv, 2, 1.0) : 1.0;
        double ox = op == DM_SCALE ? dm_arg(ctx, &o, argc, argv, 3, 0.0) : 0.0;
        double oy = op == DM_SCALE ? dm_arg(ctx, &o, argc, argv, 4, 0.0) : 0.0;
        double oz = op == DM_SCALE ? dm_arg(ctx, &o, argc, argv, 5, 0.0) : 0.0;
        double s[16], tr[16], back[16];

        dm_translation(ox, oy, oz, tr);
        dm_scaling(sx, sy, sz, s);
        dm_translation(-ox, -oy, -oz, back);
        dm_mul(tr, s, u);
        dm_mul(u, back, t);
        if (!(sz == 1.0)) b->is2d = false;
        dm_store_product(ctx, b, &o, name, a, t, false);
        return 0;
    }
    case DM_SCALE3D: {
        /* "If scale is not 1, set is 2D of the current matrix to false." */
        double sc = dm_arg(ctx, &o, argc, argv, 0, 1.0);
        double ox = dm_arg(ctx, &o, argc, argv, 1, 0.0), oy = dm_arg(ctx, &o, argc, argv, 2, 0.0);
        double oz = dm_arg(ctx, &o, argc, argv, 3, 0.0);
        double s[16], tr[16], back[16];

        dm_translation(ox, oy, oz, tr);
        dm_scaling(sc, sc, sc, s);
        dm_translation(-ox, -oy, -oz, back);
        dm_mul(tr, s, u);
        dm_mul(u, back, t);
        if (!(sc == 1.0)) b->is2d = false;
        dm_store_product(ctx, b, &o, name, a, t, false);
        return 0;
    }
    case DM_ROTATE: {
        /* "If rotY and rotZ are both missing, set rotZ to the value of rotX and set rotX and rotY to 0", then
           post-multiply about Z by rotZ, about Y by rotY and about X by rotX. */
        bool y_missing = argc < 2 || JS_IsUndefined(argv[1]), z_missing = argc < 3 || JS_IsUndefined(argv[2]);
        double rx = dm_arg(ctx, &o, argc, argv, 0, 0.0), ry, rz, r[16];

        if (y_missing && z_missing) {
            rz = rx;
            rx = ry = 0.0;
        }
        else {
            ry = dm_arg(ctx, &o, argc, argv, 1, 0.0);
            rz = dm_arg(ctx, &o, argc, argv, 2, 0.0);
        }
        if (!(rx == 0.0) || !(ry == 0.0)) b->is2d = false;
        dm_rotation(0.0, 0.0, 1.0, rz, r);
        dm_mul(a, r, u);
        dm_rotation(0.0, 1.0, 0.0, ry, r);
        dm_mul(u, r, t);
        dm_rotation(1.0, 0.0, 0.0, rx, r);
        dm_mul(t, r, u);
        dm_store(ctx, b, &o, name, u);
        return 0;
    }
    case DM_ROTATE_FROM_VECTOR: {
        /* "The rotation angle is determined by the angle between the vector (1,0)T and (x,y)T in the clockwise
           direction. If x and y should both be 0 or -0, the angle is specified as 0." */
        double x = dm_arg(ctx, &o, argc, argv, 0, 0.0), y = dm_arg(ctx, &o, argc, argv, 1, 0.0);

        dm_rotation(0.0, 0.0, 1.0, (x == 0.0 && y == 0.0) ? 0.0 : atan2(y, x) * 180.0 / M_PI, t);
        dm_store_product(ctx, b, &o, name, a, t, false);
        return 0;
    }
    case DM_ROTATE_AXIS_ANGLE: {
        /* "If x or y are not 0 or -0, set is 2D of the current matrix to false." */
        double x = dm_arg(ctx, &o, argc, argv, 0, 0.0), y = dm_arg(ctx, &o, argc, argv, 1, 0.0);
        double z = dm_arg(ctx, &o, argc, argv, 2, 0.0), angle = dm_arg(ctx, &o, argc, argv, 3, 0.0);

        dm_rotation(x, y, z, angle, t);
        if (!(x == 0.0) || !(y == 0.0)) b->is2d = false;
        dm_store_product(ctx, b, &o, name, a, t, false);
        return 0;
    }
    case DM_SKEW_X:
    case DM_SKEW_Y:
        /* css-transforms-1 §12's skewX(α) puts tan(α) at m21, skewY(β) at m12. */
        dm_set_identity(t);
        t[op == DM_SKEW_X ? 4 : 1] = tan(dm_arg(ctx, &o, argc, argv, 0, 0.0) * M_PI / 180.0);
        dm_store_product(ctx, b, &o, name, a, t, false);
        return 0;
    case DM_MULTIPLY:
    case DM_PRE_MULTIPLY: {
        /* "Let otherObject be the result of invoking create a DOMMatrix from the dictionary other"; "If is 2D
           of otherObject is false, set is 2D of the current matrix to false." */
        JSValue other[16];
        bool other2d;
        int k;

        if (dm_from_init(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, other, &other2d) < 0) return -1;
        for (k = 0; k < 16; k++) t[k] = dm_read(ctx, &o, other[k]);
        if (!other2d) b->is2d = false;
        dm_store_product(ctx, b, &o, name, a, t, op == DM_PRE_MULTIPLY);
        for (k = 0; k < 16; k++) JS_FreeValue(ctx, other[k]);
        return 0;
    }
    case DM_FLIP_X:
    case DM_FLIP_Y:
        /* "Post-multiply result with new DOMMatrix([-1, 0, 0, 1, 0, 0])", or [1, 0, 0, -1, 0, 0]. */
        dm_scaling(op == DM_FLIP_X ? -1.0 : 1.0, op == DM_FLIP_Y ? -1.0 : 1.0, 1.0, t);
        dm_store_product(ctx, b, &o, name, a, t, false);
        return 0;
    case DM_INVERT: {
        /* "If the current matrix is not invertible set all attributes to NaN and set is 2D to false." */
        int k;

        if (!dm_invert(a, t)) {
            for (k = 0; k < 16; k++) t[k] = NAN;
            b->is2d = false;
        }
        dm_store(ctx, b, &o, name, t);
        return 0;
    }
    case DM_OP_COUNT:
        break;
    }
    DFAIL("a DOMMatrix method ran with an operation §6.5 and §6.6 do not declare");
    return -1;
}

/* A mutable method: perform it and "Return the current matrix". The magic is the DmOp. */
static JSValue js_dm_self(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    DomMatrixBox *b = dm_this(ctx, this_val);

    if (!b) return JS_EXCEPTION;
    if (!JS_GetOpaque(this_val, g_mx_class)) return JS_ThrowTypeError(ctx, "a DOMMatrixReadOnly is not mutable");
    if (dm_perform(ctx, b, (DmOp)magic, argc, argv) < 0) return JS_EXCEPTION;
    return JS_DupValue(ctx, this_val);
}

/* An immutable method: "Let result be the resulting matrix initialized to the values of the current matrix",
   perform the matching `*Self` algorithm on it, and return it. */
static JSValue js_dm_immutable(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    DomMatrixBox *b = dm_this(ctx, this_val), *rb;
    JSValue m[16], result;
    int k;

    if (!b) return JS_EXCEPTION;
    for (k = 0; k < 16; k++) m[k] = JS_DupValue(ctx, b->m[k]);
    result = dm_alloc(ctx, DM_MUTABLE, m, b->is2d);
    rb = dm_box(result);
    if (dm_perform(ctx, rb, (DmOp)magic, argc, argv) < 0) {
        JS_FreeValue(ctx, result);
        return JS_EXCEPTION;
    }
    return result;
}

/* §2.1 "Transforming a point with a matrix": the point's column vector pre-multiplied by the matrix. */
static JSValue dm_transform_point(JSContext *ctx, const DomMatrixBox *b, JSValueConst point)
{
    JSValueConst p[4];
    DmOps o;
    double a[16], v[4], r[4];
    int i, k;
    JSValue out[4];

    dom_point_values(point, p);
    dm_ops_init(&o);
    dm_read_box(ctx, &o, b, a);
    for (i = 0; i < 4; i++) v[i] = dm_read(ctx, &o, p[i]);
    for (i = 0; i < 4; i++) {
        r[i] = 0.0;
        for (k = 0; k < 4; k++) r[i] += a[k * 4 + i] * v[k];
        out[i] = dm_result(ctx, &o, "DOMMatrix.transformPoint", i, r[i]);
    }
    return dom_point_new(ctx, true, out[0], out[1], out[2], out[3]);
}

JSValue dom_matrix_transform_point_init(JSContext *ctx, JSValueConst init, JSValueConst point)
{
    JSValue m[16], mo, out;

    {
        bool is2d;

        if (dm_from_init(ctx, init, m, &is2d) < 0) return JS_EXCEPTION;
        mo = dm_alloc(ctx, DM_MUTABLE, m, is2d);
    }
    out = dm_transform_point(ctx, dm_box(mo), point);
    JS_FreeValue(ctx, mo);
    return out;
}

/* transformPoint(point): "Let pointObject be the result of invoking create a DOMPoint from the dictionary point",
   then §2.1 with the current matrix. */
static JSValue js_dm_transform_point(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    DomMatrixBox *b = dm_this(ctx, this_val);
    JSValue p, out;

    (void)magic;
    if (!b) return JS_EXCEPTION;
    p = dom_point_from_init(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    out = dm_transform_point(ctx, b, p);
    JS_FreeValue(ctx, p);
    return out;
}

/* toFloat32Array() and toFloat64Array(): the sixteen elements in column-major order. */
static JSValue js_dm_to_array(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    DomMatrixBox *b = dm_this(ctx, this_val);
    JSValue arr, len;
    int k;

    (void)argc; (void)argv;
    if (!b) return JS_EXCEPTION;
    for (k = 0; k < 16; k++)
        if (concolic_is(b->m[k]))
            DFAILF("Geometry Interfaces §6.5's %s over an element that is unknown external input: a typed array's "
                   "bytes are concrete. BUILD the element's unknown run in the array (JS_BufferSourceUnknowns' "
                   "carrier)", magic ? "toFloat64Array" : "toFloat32Array");
    len = JS_NewInt32(ctx, 16);
    arr = JS_NewTypedArray(ctx, 1, &len, magic ? JS_TYPED_ARRAY_FLOAT64 : JS_TYPED_ARRAY_FLOAT32);
    if (JS_IsException(arr)) return arr;
    for (k = 0; k < 16; k++)
        if (JS_SetPropertyUint32(ctx, arr, (uint32_t)k, JS_DupValue(ctx, b->m[k])) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    return arr;
}

/* §6.5's stringification behavior. */
static JSValue js_dm_to_string(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    static const int TWO_D[6] = { 0, 1, 4, 5, 12, 13 };
    DomMatrixBox *b = dm_this(ctx, this_val);
    DmOps o;
    double a[16];
    char buf[16 * 32 + 16];
    size_t off;
    int n, i;

    (void)argc; (void)argv; (void)magic;
    if (!b) return JS_EXCEPTION;
    dm_ops_init(&o);
    dm_read_box(ctx, &o, b, a);
    /* "If one or more of m11 element through m44 element are a non-finite value, then throw an
       "InvalidStateError" DOMException." */
    for (i = 0; i < 16; i++)
        if (o.exact && !isfinite(a[i]))
            return JS_ThrowDOMException(ctx, "InvalidStateError", "a matrix with a non-finite element has no CSS "
                                                                  "serialization");
    off = (size_t)snprintf(buf, sizeof buf, "%s", b->is2d ? "matrix(" : "matrix3d(");
    n = b->is2d ? 6 : 16;
    for (i = 0; i < n; i++) {
        JSValue s = JS_ToString(ctx, JS_NewFloat64(ctx, a[b->is2d ? TWO_D[i] : i]));
        const char *c = JS_ToCString(ctx, s);

        JS_FreeValue(ctx, s);
        CHECK(c != NULL, "Geometry Interfaces §6.5: OOM serializing a matrix element");
        off += (size_t)snprintf(buf + off, sizeof buf - off, "%s%s", i ? ", " : "", c);
        JS_FreeCString(ctx, c);
    }
    snprintf(buf + off, sizeof buf - off, ")");
    if (!o.unknown) return JS_NewString(ctx, buf);
    return concolic_new_derived(ctx, "DOMMatrixReadOnly.stringifier", o.ops, o.n,
                                o.exact ? JS_NewString(ctx, buf) : JS_UNDEFINED);
}

/* setMatrixValue(transformList): §6.2 over the string, then "Set is 2D to the value of 2dTransform" and every
   element. */
static JSValue js_dm_set_matrix_value(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    DomMatrixBox *b = dm_this(ctx, this_val);
    JSValue m[16];
    bool two_d;
    int k;

    (void)magic;
    if (!b) return JS_EXCEPTION;
    DCHECK(argc > 0, "setMatrixValue ran with no argument; its one argument is required");
    if (dm_parse_value(ctx, argv[0], m, &two_d) < 0) return JS_EXCEPTION;
    b->is2d = two_d;
    for (k = 0; k < 16; k++) dm_set_element(ctx, b, k, m[k]);
    return JS_DupValue(ctx, this_val);
}

/* Web IDL §3.7.7.1.1's default toJSON steps: the 22 element attributes, is2D and isIdentity, in declaration
   order. */
static JSValue js_dm_tojson(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    DomMatrixBox *b = dm_this(ctx, this_val);
    JSValue out;
    int i;

    (void)argc; (void)argv; (void)magic;
    if (!b) return JS_EXCEPTION;
    out = JS_NewObject(ctx);
    CHECK(!JS_IsException(out), "a DOMMatrix's toJSON result could not be allocated");
    for (i = 0; i < 22; i++) JS_SetPropertyStr(ctx, out, DM_ATTR[i], JS_DupValue(ctx, b->m[DM_ATTR_ELEMENT[i]]));
    JS_SetPropertyStr(ctx, out, "is2D", JS_NewBool(ctx, b->is2d));
    JS_SetPropertyStr(ctx, out, "isIdentity", js_dm_is_identity(ctx, this_val, 0));
    return out;
}

/* ---- the declaration and the per-realm install ----------------------------------------------------------- */

typedef JSValue (*DmBody)(JSContext *, JSValueConst, int, JSValueConst *, int);

/* Each method's declaration: its name, how many optional `unrestricted double` arguments it declares, the body
   and its magic, whether its one argument is a DOMMatrixInit, whether it is DOMMatrix's own (`*Self`,
   setMatrixValue) and whether it is `[Exposed=Window]`. */
static const struct {
    const char *name;
    int nargs;
    DmBody body;
    int magic;
    bool dict, self, window_only;
} DM_METHODS[] = {
    { "translate",            3, js_dm_immutable, DM_TRANSLATE,          false, false, false },
    { "scale",                6, js_dm_immutable, DM_SCALE,              false, false, false },
    { "scaleNonUniform",      2, js_dm_immutable, DM_SCALE_NON_UNIFORM,  false, false, false },
    { "scale3d",              4, js_dm_immutable, DM_SCALE3D,            false, false, false },
    { "rotate",               3, js_dm_immutable, DM_ROTATE,             false, false, false },
    { "rotateFromVector",     2, js_dm_immutable, DM_ROTATE_FROM_VECTOR, false, false, false },
    { "rotateAxisAngle",      4, js_dm_immutable, DM_ROTATE_AXIS_ANGLE,  false, false, false },
    { "skewX",                1, js_dm_immutable, DM_SKEW_X,             false, false, false },
    { "skewY",                1, js_dm_immutable, DM_SKEW_Y,             false, false, false },
    { "multiply",             1, js_dm_immutable, DM_MULTIPLY,           true,  false, false },
    { "flipX",                0, js_dm_immutable, DM_FLIP_X,             false, false, false },
    { "flipY",                0, js_dm_immutable, DM_FLIP_Y,             false, false, false },
    { "inverse",              0, js_dm_immutable, DM_INVERT,             false, false, false },
    { "transformPoint",       1, js_dm_transform_point, 0,               true,  false, false },
    { "toFloat32Array",       0, js_dm_to_array,  0,                     false, false, false },
    { "toFloat64Array",       0, js_dm_to_array,  1,                     false, false, false },
    { "toString",             0, js_dm_to_string, 0,                     false, false, true },
    { "toJSON",               0, js_dm_tojson,    0,                     false, false, false },
    { "multiplySelf",         1, js_dm_self,      DM_MULTIPLY,           true,  true,  false },
    { "preMultiplySelf",      1, js_dm_self,      DM_PRE_MULTIPLY,       true,  true,  false },
    { "translateSelf",        3, js_dm_self,      DM_TRANSLATE,          false, true,  false },
    { "scaleSelf",            6, js_dm_self,      DM_SCALE,              false, true,  false },
    { "scale3dSelf",          4, js_dm_self,      DM_SCALE3D,            false, true,  false },
    { "rotateSelf",           3, js_dm_self,      DM_ROTATE,             false, true,  false },
    { "rotateFromVectorSelf", 2, js_dm_self,      DM_ROTATE_FROM_VECTOR, false, true,  false },
    { "rotateAxisAngleSelf",  4, js_dm_self,      DM_ROTATE_AXIS_ANGLE,  false, true,  false },
    { "skewXSelf",            1, js_dm_self,      DM_SKEW_X,             false, true,  false },
    { "skewYSelf",            1, js_dm_self,      DM_SKEW_Y,             false, true,  false },
    { "invertSelf",           0, js_dm_self,      DM_INVERT,             false, true,  false },
    { "setMatrixValue",       1, js_dm_set_matrix_value, 0,              false, true,  true },
};

void dom_matrix_init(JSContext *ctx)
{
    JSClassDef ro = { "DOMMatrixReadOnly", dm_finalizer, dm_gc_mark };
    JSClassDef mx = { "DOMMatrix", dm_finalizer, dm_gc_mark };
    static const IdlArgType DOUBLES[6] = { IDL_UNRESTRICTED_DOUBLE, IDL_UNRESTRICTED_DOUBLE, IDL_UNRESTRICTED_DOUBLE,
                                           IDL_UNRESTRICTED_DOUBLE, IDL_UNRESTRICTED_DOUBLE, IDL_UNRESTRICTED_DOUBLE };
    static const IdlArgType INIT[1] = { IDL_DOMSTRING_OR_SEQUENCE_UNRESTRICTED_DOUBLE };
    static const IdlArgType ONE_DICT[1] = { IDL_DICT };
    static const IdlArgType ONE_ARRAY[1] = { IDL_TYPED_ARRAY };
    static const IdlArgType ONE_STRING[1] = { IDL_DOMSTRING };
    size_t i;
    int w;

    DCHECK(g_ro_class == 0, "dom_matrix_init ran twice — §6's classes are declared once per agent");
    DCHECK(COUNTOF(DM_METHODS) <= COUNTOF(g_id_method), "§6's method table outgrew its declaration ids");
    JS_NewClassID(JS_GetRuntime(ctx), &g_ro_class);
    JS_NewClass(JS_GetRuntime(ctx), g_ro_class, &ro);
    JS_NewClassID(JS_GetRuntime(ctx), &g_mx_class);
    JS_NewClass(JS_GetRuntime(ctx), g_mx_class, &mx);

    for (w = DM_READONLY; w <= DM_MUTABLE; w++) {
        g_id_ctor[w] = idl_method_id(ctx, INIT, 1, js_dm_ctor, w);
        idl_optional_from(0);
        g_id_from_matrix[w] = idl_method_id_dict(ctx, ONE_DICT, 1, DOM_MATRIX_INIT_DECL.members,
                                                 DOM_MATRIX_INIT_DECL.n, js_dm_from_matrix, w);
        idl_optional_from(0);
        g_id_from_f32[w] = idl_method_id(ctx, ONE_ARRAY, 1, js_dm_from_array, w);
        idl_typed_array(0, JS_TYPED_ARRAY_FLOAT32, false, false);
        g_id_from_f64[w] = idl_method_id(ctx, ONE_ARRAY, 1, js_dm_from_array, w);
        idl_typed_array(0, JS_TYPED_ARRAY_FLOAT64, false, false);
    }
    for (i = 0; i < COUNTOF(DM_METHODS); i++) {
        if (DM_METHODS[i].dict)
            g_id_method[i] = idl_method_id_dict(ctx, ONE_DICT, 1, DOM_MATRIX_INIT_DECL.members,
                                                DOM_MATRIX_INIT_DECL.n, DM_METHODS[i].body, DM_METHODS[i].magic);
        else if (DM_METHODS[i].body == js_dm_set_matrix_value)
            g_id_method[i] = idl_method_id(ctx, ONE_STRING, 1, DM_METHODS[i].body, 0);
        else
            g_id_method[i] = idl_method_id(ctx, DM_METHODS[i].nargs ? DOUBLES : NULL, DM_METHODS[i].nargs,
                                           DM_METHODS[i].body, DM_METHODS[i].magic);
        if (DM_METHODS[i].dict || (DM_METHODS[i].nargs > 0 && DM_METHODS[i].body != js_dm_set_matrix_value))
            idl_optional_from(0);
    }
    for (i = 0; i < 22; i++) g_id_set[i] = idl_setter_id(ctx, IDL_UNRESTRICTED_DOUBLE, false, js_dm_set, (int)i);

    agent_state_class("dom_matrix", &g_ro_class, "§6's DOMMatrixReadOnly class, and the declaration latch");
    agent_state_class("dom_matrix", &g_mx_class, "§6's DOMMatrix class");
    agent_state_id("dom_matrix", &g_id_ctor[0], "§6's DOMMatrixReadOnly constructor declaration");
    agent_state_id("dom_matrix", &g_id_ctor[1], "§6's DOMMatrix constructor declaration");
    realm_declare_intrinsic(dom_matrix_install_realm);
}

/* One interface object with its three statics. */
static JSValue dm_interface_object(JSContext *ctx, const char *name, int w, JSValueConst proto)
{
    JSValue ctor = idl_step_constructor(ctx, name, g_id_ctor[w]);

    CHECK(!JS_IsException(ctor), "a DOMMatrix interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    idl_install_method(ctx, ctor, "fromMatrix", g_id_from_matrix[w]);
    idl_install_method(ctx, ctor, "fromFloat32Array", g_id_from_f32[w]);
    idl_install_method(ctx, ctor, "fromFloat64Array", g_id_from_f64[w]);
    return ctor;
}

/* Web IDL §3.7.3's prototypes, §3.7.1's interface objects and §3.8's global property references for one realm,
   as core/geometry/dom_rect.c installs §3's. The stringifier and setMatrixValue are `[Exposed=Window]`. */
void dom_matrix_install_realm(JSContext *ctx)
{
    JSValue rop, mp, prev, ro_ctor, ctor, global;
    bool window = !realm_global_is_worker(ctx) && !realm_global_is_worklet(ctx);
    size_t i;

    DCHECK(g_ro_class != 0, "a realm asked for DOMMatrixReadOnly.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_ro_class);
    DCHECK(JS_IsNull(prev), "dom_matrix_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);

    rop = JS_NewObject(ctx);
    CHECK(!JS_IsException(rop), "DOMMatrixReadOnly.prototype could not be allocated");
    idl_interface_tag(ctx, rop, "DOMMatrixReadOnly");
    for (i = 0; i < 22; i++) idl_install_accessor(ctx, rop, DM_ATTR[i], js_dm_get, (int)i, -1);
    idl_install_accessor(ctx, rop, "is2D", js_dm_is2d, 0, -1);
    idl_install_accessor(ctx, rop, "isIdentity", js_dm_is_identity, 0, -1);
    mp = JS_NewObjectProto(ctx, rop);
    CHECK(!JS_IsException(mp), "DOMMatrix.prototype could not be allocated");
    idl_interface_tag(ctx, mp, "DOMMatrix");
    for (i = 0; i < 22; i++) idl_install_accessor(ctx, mp, DM_ATTR[i], js_dm_get, (int)i, g_id_set[i]);
    for (i = 0; i < COUNTOF(DM_METHODS); i++) {
        if (DM_METHODS[i].window_only && !window) continue;
        idl_install_method(ctx, DM_METHODS[i].self ? mp : rop, DM_METHODS[i].name, g_id_method[i]);
    }

    global = JS_GetGlobalObject(ctx);
    ro_ctor = dm_interface_object(ctx, "DOMMatrixReadOnly", DM_READONLY, rop);
    ctor = dm_interface_object(ctx, "DOMMatrix", DM_MUTABLE, mp);
    JS_SetPrototype(ctx, ctor, ro_ctor);
    {
        JSValue svg = JS_DupValue(ctx, ctor), webkit = JS_DupValue(ctx, ctor);

        idl_define_global_property_reference(ctx, global, "DOMMatrixReadOnly", ro_ctor);
        idl_define_global_property_reference(ctx, global, "DOMMatrix", ctor);
        idl_define_legacy_window_alias(ctx, global, "SVGMatrix", svg);
        idl_define_legacy_window_alias(ctx, global, "WebKitCSSMatrix", webkit);
    }
    JS_FreeValue(ctx, global);
    JS_SetClassProto(ctx, g_ro_class, rop);
    JS_SetClassProto(ctx, g_mx_class, mp);
}

void dom_matrix_free(void)
{
    size_t i;

    g_ro_class = g_mx_class = 0;
    for (i = 0; i < 2; i++) g_id_ctor[i] = g_id_from_matrix[i] = g_id_from_f32[i] = g_id_from_f64[i] = -1;
    for (i = 0; i < 22; i++) g_id_set[i] = -1;
    for (i = 0; i < COUNTOF(g_id_method); i++) g_id_method[i] = -1;
}
