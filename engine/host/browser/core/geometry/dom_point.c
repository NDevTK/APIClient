/* Geometry Interfaces §2 "The DOMPoint interfaces". See dom_point.h. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/geometry/dom_matrix.h"
#include "core/geometry/dom_point.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/concolic.h"
#include "solver/cow.h"
#include <lexbor/core/base.h>   /* lexbor_calloc: the allocator the engine gives its refusal edge */

/* §2's four internal member variables, which "must not be exposed in any way". */
typedef struct {
    JSValue v[4];   /* x coordinate, y coordinate, z coordinate, w perspective */
} DomPointBox;

/* What the record owns — read by the COW capture, the finalizer and the gc_mark. */
static const uint16_t DOM_POINT_OFF[] = {
    (uint16_t)offsetof(DomPointBox, v[0]), (uint16_t)offsetof(DomPointBox, v[1]),
    (uint16_t)offsetof(DomPointBox, v[2]), (uint16_t)offsetof(DomPointBox, v[3]),
};
static const CowRecord DOM_POINT_REC = { sizeof(DomPointBox), DOM_POINT_OFF, 4 };

enum { DP_READONLY = 0, DP_MUTABLE = 1 };

static const char *const DP_NAMES[4] = { "x", "y", "z", "w" };

static JSClassID g_ro_class, g_pt_class;
static int g_id_ctor_ro = -1, g_id_ctor_pt = -1, g_id_from_ro = -1, g_id_from_pt = -1;
static int g_id_matrix_transform = -1, g_id_tojson = -1;
static int g_id_set[4] = { -1, -1, -1, -1 };

/* The brand. A read captures nothing; the setter takes the record through dp_box_for_write. */
static const DomPointBox *dp_box(JSValueConst v)
{
    const DomPointBox *b = JS_GetOpaque(v, g_ro_class);

    if (!b) b = JS_GetOpaque(v, g_pt_class);
    return b;
}

/* The write accessor: captures the record into the running flow's head before answering it mutable. */
static DomPointBox *dp_box_for_write(JSValueConst v)
{
    DomPointBox *b = JS_GetOpaque(v, g_ro_class);

    if (!b) b = JS_GetOpaque(v, g_pt_class);
    if (b) cow_capture_host_record(v, b, &DOM_POINT_REC);
    return b;
}

static const DomPointBox *dp_this(JSContext *ctx, JSValueConst this_val)
{
    const DomPointBox *b = dp_box(this_val);

    if (!b) JS_ThrowTypeError(ctx, "a DOMPoint member was reached on something that is not a DOMPoint");
    return b;
}

bool dom_point_is(JSValueConst v)
{
    return JS_GetOpaque(v, g_ro_class) != NULL || JS_GetOpaque(v, g_pt_class) != NULL;
}

void dom_point_values(JSValueConst v, JSValueConst out[4])
{
    const DomPointBox *b = dp_box(v);
    int i;

    DCHECK(b != NULL, "dom_point_values was handed something that is not a DOMPoint");
    for (i = 0; i < 4; i++) out[i] = b->v[i];
}

/* The collector reaches the record from the object and reads no static of this file (core/agent_state.h). */
static void dp_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    DomPointBox *b = JS_GetAnyOpaque(val, &id);
    int i;

    DCHECK(b != NULL, "a DOMPoint was finalized with no internal member variables — dp_alloc attaches the record "
                      "before the object can reach anything that would free it");
    for (i = 0; i < 4; i++) JS_FreeValueRT(rt, b->v[i]);
    free(b);
}

static void dp_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID id = 0;
    const DomPointBox *b = JS_GetAnyOpaque(val, &id);
    int i;

    DCHECK(b != NULL, "a DOMPoint was marked with no internal member variables");
    for (i = 0; i < 4; i++) JS_MarkValue(rt, b->v[i], mark_func);
}

/* An absent optional argument or dictionary member takes the IDL's default: 0, or 1 for `w`. CONSUMES `v`. */
static JSValue dp_value(JSContext *ctx, JSValue v, int i)
{
    if (JS_IsUndefined(v)) return JS_NewFloat64(ctx, i == 3 ? 1.0 : 0.0);
    DCHECK(JS_IsNumber(v) || concolic_is(v),
           "a DOMPoint variable was given something that is neither a Number nor unknown external input — its "
           "IDL type is `unrestricted double`");
    return v;
}

static JSValue dp_alloc(JSContext *ctx, int which, JSValue v[4])
{
    JSClassID cls = which == DP_MUTABLE ? g_pt_class : g_ro_class;
    JSValue proto, obj;
    DomPointBox *b;
    int i;

    DCHECK(cls != 0, "a point was built before dom_point_init declared its classes");
    proto = JS_GetClassProto(ctx, cls);
    DCHECK(!JS_IsNull(proto), "a point was built in a realm that never ran its prototype install");
    obj = JS_NewObjectProtoClass(ctx, proto, cls);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "a DOMPoint could not be allocated");
    b = lexbor_calloc(1, sizeof(*b));
    CHECK(b != NULL, "the DOMPoint record allocation failed");
    for (i = 0; i < 4; i++) b->v[i] = dp_value(ctx, v[i], i);
    JS_SetOpaque(obj, b);
    return obj;
}

JSValue dom_point_new(JSContext *ctx, bool mutable_point, JSValue x, JSValue y, JSValue z, JSValue w)
{
    JSValue v[4] = { x, y, z, w };

    return dp_alloc(ctx, mutable_point ? DP_MUTABLE : DP_READONLY, v);
}

/* §2's constructor steps: "Set point's variables x coordinate to x, y coordinate to y, z coordinate to z and w
   perspective to w". */
static JSValue js_dp_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue v[4];
    int i;

    (void)this_val;
    for (i = 0; i < 4; i++) v[i] = i < argc ? JS_DupValue(ctx, argv[i]) : JS_UNDEFINED;
    return dp_alloc(ctx, magic, v);
}

/* "Create a DOMPointReadOnly / DOMPoint from the dictionary other". */
static JSValue dp_from_dict(JSContext *ctx, int which, JSValueConst init)
{
    JSValue v[4];
    int i;

    for (i = 0; i < 4; i++) v[i] = idl_dict_get(ctx, init, DP_NAMES[i]);
    return dp_alloc(ctx, which, v);
}

JSValue dom_point_from_init(JSContext *ctx, JSValueConst init)
{
    return dp_from_dict(ctx, DP_MUTABLE, init);
}

static JSValue js_dp_from_point(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)this_val;
    return dp_from_dict(ctx, magic, argc > 0 ? argv[0] : JS_UNDEFINED);
}

/* §2's `DOMPointInit`, in Web IDL §3.2.17's lexicographic read order, with its `= 0` and `= 1` defaults. */
static const IdlDictMember DOM_POINT_INIT[] = {
    { "w", IDL_UNRESTRICTED_DOUBLE, false, NULL, 0, NULL, IDL_DEFAULT_ONE },
    { "x", IDL_UNRESTRICTED_DOUBLE, false, NULL, 0, NULL, IDL_DEFAULT_ZERO },
    { "y", IDL_UNRESTRICTED_DOUBLE, false, NULL, 0, NULL, IDL_DEFAULT_ZERO },
    { "z", IDL_UNRESTRICTED_DOUBLE, false, NULL, 0, NULL, IDL_DEFAULT_ZERO },
};

const IdlDictDecl DOM_POINT_INIT_DECL = {
    "DOMPointInit", DOM_POINT_INIT, (int)(sizeof(DOM_POINT_INIT) / sizeof(DOM_POINT_INIT[0]))
};

static JSValue js_dp_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    const DomPointBox *b = dp_this(ctx, this_val);

    if (!b) return JS_EXCEPTION;
    DCHECK(magic >= 0 && magic < 4, "a DOMPoint attribute ran with a magic §2 does not declare");
    return JS_DupValue(ctx, b->v[magic]);
}

/* §2: "For the DOMPoint interface, setting the x attribute must set the x coordinate to the new value." */
static JSValue js_dp_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    DomPointBox *b;

    if (!dp_this(ctx, this_val)) return JS_EXCEPTION;
    DCHECK(magic >= 0 && magic < 4, "a DOMPoint setter ran with a magic §2 does not declare");
    b = dp_box_for_write(this_val);
    cow_record_set(ctx, this_val, b, &DOM_POINT_REC, &b->v[magic], dp_value(ctx, JS_DupValue(ctx, val), magic));
    return JS_UNDEFINED;
}

/* §2's matrixTransform(matrix): "Let matrixObject be the result of invoking create a DOMMatrix from the
   dictionary matrix", then §2.1 with the current point, which is not modified. */
static JSValue js_dp_matrix_transform(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                      int magic)
{
    (void)magic;
    if (!dp_this(ctx, this_val)) return JS_EXCEPTION;
    return dom_matrix_transform_point_init(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, this_val);
}

/* Web IDL §3.7.7.1.1's default toJSON steps over §2's four attributes, all `unrestricted double`. */
static JSValue js_dp_tojson(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    const DomPointBox *b = dp_this(ctx, this_val);
    JSValue out;
    int i;

    (void)argc; (void)argv; (void)magic;
    if (!b) return JS_EXCEPTION;
    out = JS_NewObject(ctx);
    CHECK(!JS_IsException(out), "a DOMPoint's toJSON result could not be allocated");
    for (i = 0; i < 4; i++) JS_SetPropertyStr(ctx, out, DP_NAMES[i], JS_DupValue(ctx, b->v[i]));
    return out;
}

void dom_point_init(JSContext *ctx)
{
    JSClassDef ro = { "DOMPointReadOnly", dp_finalizer, dp_gc_mark };
    JSClassDef pt = { "DOMPoint", dp_finalizer, dp_gc_mark };
    static const IdlArgType FOUR[4] = { IDL_UNRESTRICTED_DOUBLE, IDL_UNRESTRICTED_DOUBLE,
                                        IDL_UNRESTRICTED_DOUBLE, IDL_UNRESTRICTED_DOUBLE };
    static const IdlArgType ONE_DICT[1] = { IDL_DICT };
    int i;

    DCHECK(g_ro_class == 0, "dom_point_init ran twice — §2's classes are declared once per agent");
    JS_NewClassID(JS_GetRuntime(ctx), &g_ro_class);
    JS_NewClass(JS_GetRuntime(ctx), g_ro_class, &ro);
    JS_NewClassID(JS_GetRuntime(ctx), &g_pt_class);
    JS_NewClass(JS_GetRuntime(ctx), g_pt_class, &pt);

    g_id_ctor_ro = idl_method_id(ctx, FOUR, 4, js_dp_ctor, DP_READONLY);
    idl_optional_from(0);
    g_id_ctor_pt = idl_method_id(ctx, FOUR, 4, js_dp_ctor, DP_MUTABLE);
    idl_optional_from(0);
    g_id_from_ro = idl_method_id_dict(ctx, ONE_DICT, 1, DOM_POINT_INIT_DECL.members, DOM_POINT_INIT_DECL.n,
                                      js_dp_from_point, DP_READONLY);
    idl_optional_from(0);
    g_id_from_pt = idl_method_id_dict(ctx, ONE_DICT, 1, DOM_POINT_INIT_DECL.members, DOM_POINT_INIT_DECL.n,
                                      js_dp_from_point, DP_MUTABLE);
    idl_optional_from(0);
    g_id_matrix_transform = idl_method_id_dict(ctx, ONE_DICT, 1, DOM_MATRIX_INIT_DECL.members,
                                               DOM_MATRIX_INIT_DECL.n, js_dp_matrix_transform, 0);
    idl_optional_from(0);
    g_id_tojson = idl_method_id(ctx, NULL, 0, js_dp_tojson, 0);
    for (i = 0; i < 4; i++) g_id_set[i] = idl_setter_id(ctx, IDL_UNRESTRICTED_DOUBLE, false, js_dp_set, i);

    agent_state_class("dom_point", &g_ro_class, "§2's DOMPointReadOnly class, and the declaration latch");
    agent_state_class("dom_point", &g_pt_class, "§2's DOMPoint class");
    agent_state_id("dom_point", &g_id_ctor_ro, "§2's DOMPointReadOnly constructor declaration");
    agent_state_id("dom_point", &g_id_ctor_pt, "§2's DOMPoint constructor declaration");
    realm_declare_intrinsic(dom_point_install_realm);
}

/* Web IDL §3.7.3's prototypes, §3.7.1's interface objects and §3.8's global property references for one realm,
   as core/geometry/dom_rect.c installs §3's; `LegacyWindowAlias=SVGPoint` is DOMPoint's. */
void dom_point_install_realm(JSContext *ctx)
{
    JSValue rop, pp, prev, ro_ctor, ctor, global;
    int i;

    DCHECK(g_ro_class != 0, "a realm asked for DOMPointReadOnly.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_ro_class);
    DCHECK(JS_IsNull(prev), "dom_point_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);

    rop = JS_NewObject(ctx);
    CHECK(!JS_IsException(rop), "DOMPointReadOnly.prototype could not be allocated");
    idl_interface_tag(ctx, rop, "DOMPointReadOnly");
    for (i = 0; i < 4; i++) idl_install_accessor(ctx, rop, DP_NAMES[i], js_dp_get, i, -1);
    idl_install_method(ctx, rop, "matrixTransform", g_id_matrix_transform);
    idl_install_method(ctx, rop, "toJSON", g_id_tojson);

    pp = JS_NewObjectProto(ctx, rop);
    CHECK(!JS_IsException(pp), "DOMPoint.prototype could not be allocated");
    idl_interface_tag(ctx, pp, "DOMPoint");
    for (i = 0; i < 4; i++) idl_install_accessor(ctx, pp, DP_NAMES[i], js_dp_get, i, g_id_set[i]);

    global = JS_GetGlobalObject(ctx);
    ro_ctor = idl_step_constructor(ctx, "DOMPointReadOnly", g_id_ctor_ro);
    CHECK(!JS_IsException(ro_ctor), "the DOMPointReadOnly interface object could not be allocated");
    JS_SetConstructor(ctx, ro_ctor, rop);
    idl_install_method(ctx, ro_ctor, "fromPoint", g_id_from_ro);
    ctor = idl_step_constructor(ctx, "DOMPoint", g_id_ctor_pt);
    CHECK(!JS_IsException(ctor), "the DOMPoint interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, pp);
    idl_install_method(ctx, ctor, "fromPoint", g_id_from_pt);
    JS_SetPrototype(ctx, ctor, ro_ctor);
    {
        JSValue alias = JS_DupValue(ctx, ctor);

        idl_define_global_property_reference(ctx, global, "DOMPointReadOnly", ro_ctor);
        idl_define_global_property_reference(ctx, global, "DOMPoint", ctor);
        /* §2's IDL declares `LegacyWindowAlias=SVGPoint`, and §10.2 SVG defines SVGPoint "as aliases to
           DOMPoint". Chrome 155.0.8059.84 keeps SVGPoint a separate interface (`SVGPoint !== DOMPoint`); this
           follows the spec. */
        idl_define_legacy_window_alias(ctx, global, "SVGPoint", alias);
    }
    JS_FreeValue(ctx, global);
    JS_SetClassProto(ctx, g_ro_class, rop);
    JS_SetClassProto(ctx, g_pt_class, pp);
}

void dom_point_free(void)
{
    g_ro_class = g_pt_class = 0;
    g_id_ctor_ro = g_id_ctor_pt = g_id_from_ro = g_id_from_pt = g_id_matrix_transform = g_id_tojson = -1;
    g_id_set[0] = g_id_set[1] = g_id_set[2] = g_id_set[3] = -1;
}
