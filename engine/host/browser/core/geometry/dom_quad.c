/* Geometry Interfaces §5 "The DOMQuad interface". See dom_quad.h. */
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/geometry/dom_point.h"
#include "core/geometry/dom_quad.h"
#include "core/geometry/dom_rect.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/concolic.h"
#include "solver/cow.h"
#include <lexbor/core/base.h>   /* lexbor_calloc: the allocator the engine gives its refusal edge */

/* §5's internal member variables point 1 to point 4, each a DOMPoint. */
typedef struct {
    JSValue p[4];
} DomQuadBox;

static const uint16_t DOM_QUAD_OFF[] = {
    (uint16_t)offsetof(DomQuadBox, p[0]), (uint16_t)offsetof(DomQuadBox, p[1]),
    (uint16_t)offsetof(DomQuadBox, p[2]), (uint16_t)offsetof(DomQuadBox, p[3]),
};
static const CowRecord DOM_QUAD_REC = { sizeof(DomQuadBox), DOM_QUAD_OFF, 4 };

static const char *const DQ_NAMES[4] = { "p1", "p2", "p3", "p4" };

static JSClassID g_quad_class;
static int g_id_ctor = -1, g_id_from_rect = -1, g_id_from_quad = -1, g_id_bounds = -1, g_id_tojson = -1;

static DomQuadBox *dq_this(JSContext *ctx, JSValueConst this_val)
{
    DomQuadBox *b = JS_GetOpaque(this_val, g_quad_class);

    if (!b) {
        JS_ThrowTypeError(ctx, "a DOMQuad member was reached on something that is not a DOMQuad");
        return NULL;
    }
    cow_capture_host_record(this_val, b, &DOM_QUAD_REC);
    return b;
}

static void dq_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    DomQuadBox *b = JS_GetAnyOpaque(val, &id);
    int i;

    DCHECK(b != NULL, "a DOMQuad was finalized with no internal member variables — dq_alloc attaches the record "
                      "before the object can reach anything that would free it");
    for (i = 0; i < 4; i++) JS_FreeValueRT(rt, b->p[i]);
    free(b);
}

static void dq_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID id = 0;
    DomQuadBox *b = JS_GetAnyOpaque(val, &id);
    int i;

    DCHECK(b != NULL, "a DOMQuad was marked with no internal member variables");
    for (i = 0; i < 4; i++) JS_MarkValue(rt, b->p[i], mark_func);
}

/* "Return a new DOMQuad with point 1 set to point1, ...". CONSUMES the four points. */
static JSValue dq_alloc(JSContext *ctx, JSValue p[4])
{
    JSValue proto, obj;
    DomQuadBox *b;
    int i;

    DCHECK(g_quad_class != 0, "a quad was built before dom_quad_init declared its class");
    proto = JS_GetClassProto(ctx, g_quad_class);
    DCHECK(!JS_IsNull(proto), "a quad was built in a realm that never ran its prototype install");
    obj = JS_NewObjectProtoClass(ctx, proto, g_quad_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "a DOMQuad could not be allocated");
    b = lexbor_calloc(1, sizeof(*b));
    CHECK(b != NULL, "the DOMQuad record allocation failed");
    for (i = 0; i < 4; i++) {
        DCHECK(dom_point_is(p[i]), "a DOMQuad point is not a DOMPoint");
        b->p[i] = p[i];
    }
    JS_SetOpaque(obj, b);
    return obj;
}

/* The constructor: "Let point1 be a new DOMPoint object with its attributes set to the values of the namesake
   dictionary members in p1", and the same for the other three. */
static JSValue js_dq_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue p[4];
    int i;

    (void)this_val; (void)magic;
    for (i = 0; i < 4; i++) p[i] = dom_point_from_init(ctx, i < argc ? argv[i] : JS_UNDEFINED);
    return dq_alloc(ctx, p);
}

/* `a + b` over two Numbers or unknown inputs, derived where either is unknown. */
static JSValue dq_add(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    JSValueConst ops[2] = { a, b };
    JSValue ea = concolic_is(a) ? concolic_example(ctx, a) : JS_DupValue(ctx, a);
    JSValue eb = concolic_is(b) ? concolic_example(ctx, b) : JS_DupValue(ctx, b);
    JSValue example = JS_UNDEFINED, v;
    double x, y;

    if (JS_IsNumber(ea) && JS_IsNumber(eb)) {
        JS_ToFloat64(ctx, &x, ea);
        JS_ToFloat64(ctx, &y, eb);
        example = JS_NewFloat64(ctx, x + y);
    }
    JS_FreeValue(ctx, ea);
    JS_FreeValue(ctx, eb);
    if (!concolic_is(a) && !concolic_is(b)) return example;
    v = concolic_new_derived(ctx, "DOMQuad.fromRect", ops, 2, example);
    DCHECK(!JS_IsUninitialized(v), "concolic_new_derived found no unknown between two operands one of which is");
    return v;
}

/* A DOMRectInit member with its `= 0`, which core/geometry/dom_rect.c's declaration leaves to the reader. */
static JSValue dq_member(JSContext *ctx, JSValueConst init, const char *name)
{
    JSValue v = idl_dict_get(ctx, init, name);

    return JS_IsUndefined(v) ? JS_NewFloat64(ctx, 0.0) : v;
}

/* fromRect(other): the four corners x, y; x + width, y; x + width, y + height; x, y + height, each with z 0 and
   w 1. */
static JSValue js_dq_from_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue x = dq_member(ctx, init, "x"), y = dq_member(ctx, init, "y");
    JSValue w = dq_member(ctx, init, "width"), h = dq_member(ctx, init, "height");
    JSValue right, bottom, p[4];

    (void)this_val; (void)magic;
    right = dq_add(ctx, x, w);
    bottom = dq_add(ctx, y, h);
    p[0] = dom_point_new(ctx, true, JS_DupValue(ctx, x), JS_DupValue(ctx, y), JS_NewFloat64(ctx, 0.0),
                         JS_NewFloat64(ctx, 1.0));
    p[1] = dom_point_new(ctx, true, JS_DupValue(ctx, right), JS_DupValue(ctx, y), JS_NewFloat64(ctx, 0.0),
                         JS_NewFloat64(ctx, 1.0));
    p[2] = dom_point_new(ctx, true, JS_DupValue(ctx, right), JS_DupValue(ctx, bottom), JS_NewFloat64(ctx, 0.0),
                         JS_NewFloat64(ctx, 1.0));
    p[3] = dom_point_new(ctx, true, JS_DupValue(ctx, x), JS_DupValue(ctx, bottom), JS_NewFloat64(ctx, 0.0),
                         JS_NewFloat64(ctx, 1.0));
    JS_FreeValue(ctx, x);
    JS_FreeValue(ctx, y);
    JS_FreeValue(ctx, w);
    JS_FreeValue(ctx, h);
    JS_FreeValue(ctx, right);
    JS_FreeValue(ctx, bottom);
    return dq_alloc(ctx, p);
}

/* fromQuad(other): "create a DOMPoint from the dictionary p1 dictionary member of other, if it exists", and an
   absent member is a DOMPointInit whose members take their defaults. */
static JSValue js_dq_from_quad(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue p[4];
    int i;

    (void)this_val; (void)magic;
    for (i = 0; i < 4; i++) {
        JSValue member = idl_dict_get(ctx, init, DQ_NAMES[i]);

        p[i] = dom_point_from_init(ctx, member);
        JS_FreeValue(ctx, member);
    }
    return dq_alloc(ctx, p);
}

/* "The p1 attribute must return point 1", the same object on every read ([SameObject]). */
static JSValue js_dq_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    DomQuadBox *b = dq_this(ctx, this_val);

    if (!b) return JS_EXCEPTION;
    DCHECK(magic >= 0 && magic < 4, "a DOMQuad attribute ran with a magic §5 does not declare");
    return JS_DupValue(ctx, b->p[magic]);
}

/* The NaN-safe minimum or maximum of four examples: NaN if any is NaN. */
static double dq_nan_extreme(const double v[4], bool maximum)
{
    double r = v[0];
    int i;

    for (i = 0; i < 4; i++) {
        if (isnan(v[i])) return NAN;
        if (maximum ? v[i] > r : v[i] < r) r = v[i];
    }
    return r;
}

/* getBounds(): left and top the NaN-safe minima of the four x and y coordinates, right and bottom the maxima,
   and the rectangle (left, top, right - left, bottom - top). */
static JSValue js_dq_get_bounds(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    static const char *const OP[4] = { "DOMQuad.getBounds.x", "DOMQuad.getBounds.y", "DOMQuad.getBounds.width",
                                       "DOMQuad.getBounds.height" };
    DomQuadBox *b = dq_this(ctx, this_val);
    JSValueConst ops[8];
    double xs[4], ys[4], r[4];
    bool unknown = false, exact = true;
    JSValue out[4];
    int i;

    (void)argc; (void)argv; (void)magic;
    if (!b) return JS_EXCEPTION;
    for (i = 0; i < 4; i++) {
        JSValueConst v[4];
        int c;

        dom_point_values(b->p[i], v);
        for (c = 0; c < 2; c++) {
            double *d = c == 0 ? &xs[i] : &ys[i];

            ops[i * 2 + c] = v[c];
            if (concolic_is(v[c])) {
                JSValue e = concolic_example(ctx, v[c]);

                unknown = true;
                *d = NAN;
                if (JS_IsNumber(e)) JS_ToFloat64(ctx, d, e);
                else exact = false;
                JS_FreeValue(ctx, e);
            }
            else JS_ToFloat64(ctx, d, v[c]);
        }
    }
    r[0] = dq_nan_extreme(xs, false);
    r[1] = dq_nan_extreme(ys, false);
    r[2] = dq_nan_extreme(xs, true) - r[0];
    r[3] = dq_nan_extreme(ys, true) - r[1];
    for (i = 0; i < 4; i++) {
        if (!unknown) {
            out[i] = JS_NewFloat64(ctx, r[i]);
            continue;
        }
        out[i] = concolic_new_derived(ctx, OP[i], ops, 8, exact ? JS_NewFloat64(ctx, r[i]) : JS_UNDEFINED);
    }
    return dom_rect_new_values(ctx, out[0], out[1], out[2], out[3]);
}

/* Web IDL §3.7.7.1.1's default toJSON steps over §5's four attributes, each a DOMPoint (a JSON type, since it
   declares toJSON). */
static JSValue js_dq_tojson(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    DomQuadBox *b = dq_this(ctx, this_val);
    JSValue out;
    int i;

    (void)argc; (void)argv; (void)magic;
    if (!b) return JS_EXCEPTION;
    out = JS_NewObject(ctx);
    CHECK(!JS_IsException(out), "a DOMQuad's toJSON result could not be allocated");
    for (i = 0; i < 4; i++) JS_SetPropertyStr(ctx, out, DQ_NAMES[i], JS_DupValue(ctx, b->p[i]));
    return out;
}

/* §5's `DOMQuadInit`, each member a DOMPointInit, in lexicographic read order. */
static const IdlDictMember DOM_QUAD_INIT[] = {
    { "p1", IDL_DICT, false, NULL, 0, &DOM_POINT_INIT_DECL, IDL_DEFAULT_NONE },
    { "p2", IDL_DICT, false, NULL, 0, &DOM_POINT_INIT_DECL, IDL_DEFAULT_NONE },
    { "p3", IDL_DICT, false, NULL, 0, &DOM_POINT_INIT_DECL, IDL_DEFAULT_NONE },
    { "p4", IDL_DICT, false, NULL, 0, &DOM_POINT_INIT_DECL, IDL_DEFAULT_NONE },
};

void dom_quad_init(JSContext *ctx)
{
    JSClassDef qd = { "DOMQuad", dq_finalizer, dq_gc_mark };
    static const IdlArgType FOUR_DICTS[4] = { IDL_DICT, IDL_DICT, IDL_DICT, IDL_DICT };
    static const IdlArgType ONE_DICT[1] = { IDL_DICT };

    DCHECK(g_quad_class == 0, "dom_quad_init ran twice — §5's class is declared once per agent");
    JS_NewClassID(JS_GetRuntime(ctx), &g_quad_class);
    JS_NewClass(JS_GetRuntime(ctx), g_quad_class, &qd);
    g_id_ctor = idl_method_id_dicts(ctx, FOUR_DICTS, 4, DOM_POINT_INIT_DECL.members, DOM_POINT_INIT_DECL.n,
                                    js_dq_ctor, 0);
    idl_optional_from(0);
    g_id_from_rect = idl_method_id_dict(ctx, ONE_DICT, 1, DOM_RECT_INIT_DECL.members, DOM_RECT_INIT_DECL.n,
                                        js_dq_from_rect, 0);
    idl_optional_from(0);
    g_id_from_quad = idl_method_id_dict(ctx, ONE_DICT, 1, DOM_QUAD_INIT, (int)COUNTOF(DOM_QUAD_INIT),
                                        js_dq_from_quad, 0);
    idl_optional_from(0);
    g_id_bounds = idl_method_id(ctx, NULL, 0, js_dq_get_bounds, 0);
    g_id_tojson = idl_method_id(ctx, NULL, 0, js_dq_tojson, 0);
    agent_state_class("dom_quad", &g_quad_class, "§5's DOMQuad class, and the declaration latch");
    agent_state_id("dom_quad", &g_id_ctor, "§5's constructor declaration");
    realm_declare_intrinsic(dom_quad_install_realm);
}

void dom_quad_install_realm(JSContext *ctx)
{
    JSValue proto, prev, ctor, global;
    int i;

    DCHECK(g_quad_class != 0, "a realm asked for DOMQuad.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_quad_class);
    DCHECK(JS_IsNull(prev), "dom_quad_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "DOMQuad.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "DOMQuad");
    for (i = 0; i < 4; i++) idl_install_accessor(ctx, proto, DQ_NAMES[i], js_dq_get, i, -1);
    idl_install_method(ctx, proto, "getBounds", g_id_bounds);
    idl_install_method(ctx, proto, "toJSON", g_id_tojson);
    ctor = idl_step_constructor(ctx, "DOMQuad", g_id_ctor);
    CHECK(!JS_IsException(ctor), "the DOMQuad interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    idl_install_method(ctx, ctor, "fromRect", g_id_from_rect);
    idl_install_method(ctx, ctor, "fromQuad", g_id_from_quad);
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "DOMQuad", ctor);
    JS_FreeValue(ctx, global);
    JS_SetClassProto(ctx, g_quad_class, proto);
}

void dom_quad_free(void)
{
    g_quad_class = 0;
    g_id_ctor = g_id_from_rect = g_id_from_quad = g_id_bounds = g_id_tojson = -1;
}
