/* HTML §4.12.5.1.7 "Path2D objects", and §4.12.5.1.6 "Building paths"'s mixin members placed on its
   prototype. See path_2d.h for why this is buildable with no rendering context. */
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/canvas/canvas_path.h"
#include "core/canvas/path_2d.h"
#include "core/canvas/svg_path_data.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/concolic.h"
#include "solver/cow.h"

/* §4.12.5.1.6's "Objects that implement the CanvasPath interface have a path". One owned reference, behind the
   class opaque: the path is not exposed in any way, because the interface declares nothing that would. */
typedef struct {
    JSValue path;
} Path2DBox;

/* The one statement of what the record owns — read by the COW capture, by the finalizer and by the gc_mark. */
static const uint16_t PATH_2D_OFF[] = { (uint16_t)offsetof(Path2DBox, path) };
static const CowRecord PATH_2D_REC = { sizeof(Path2DBox), PATH_2D_OFF, 1 };

static JSClassID g_class;
static int g_id_ctor = -1;
static int g_id_m[10];

/* §4.12.5.1.6's ten operations, in the order the IDL declares them. The magic IS the member, and the order is
   the one place it is written down. `roundRect` is absent from this list — see the residual at the install. */
typedef enum {
    P2D_CLOSE_PATH = 0, P2D_MOVE_TO, P2D_LINE_TO, P2D_QUADRATIC_CURVE_TO, P2D_BEZIER_CURVE_TO,
    P2D_ARC_TO, P2D_RECT, P2D_ARC, P2D_ELLIPSE, P2D_MEMBER_COUNT
} Path2DMember;

static const char *const P2D_NAMES[P2D_MEMBER_COUNT] = {
    "closePath", "moveTo", "lineTo", "quadraticCurveTo", "bezierCurveTo", "arcTo", "rect", "arc", "ellipse"
};

/* The declared argument count of each member, from the IDL. Read by the declaration below so the arity a
   member is declared with and the arity its body reads cannot be two facts. */
static const int P2D_ARGC[P2D_MEMBER_COUNT] = { 0, 2, 2, 4, 6, 5, 4, 6, 8 };

static Path2DBox *p2d_box(JSValueConst v)
{
    Path2DBox *b = JS_GetOpaque(v, g_class);

    /* CLAUDE.md §A-COMPONENT'S-OWN-C-RECORD-TIME-TRAVELS: the capture belongs in the ACCESSOR, so a record a
       flow has reached is one it may write and there is no write site left to miss. The path ARRAY's own
       element writes are captured separately and without this component's help — cow_capture runs at
       JS_SetPropertyInternal2's head, which is where canvas_path.c's writes arrive. */
    if (b) cow_capture_host_record(v, b, &PATH_2D_REC);
    return b;
}

/* THE BRAND CHECK, AND IT IS A `TypeError` RATHER THAN AN ASSERT. `this` is whatever the page wrote —
   `Path2D.prototype.rect.call(null, 0, 0, 1, 1)` is one line — so CLAUDE.md §WHOSE-BYTES-STATE-THE-VALUE puts
   it outside what a DCHECK may stand on: asserting here would hand any page an abort switch for the engine,
   which this tree has already been caught on once. Web IDL §3.7.7 Operations' own answer is the TypeError. */
static JSValue p2d_path_of(JSContext *ctx, JSValueConst this_val)
{
    Path2DBox *b = p2d_box(this_val);

    if (!b) return JS_ThrowTypeError(ctx, "a Path2D member was reached on something that is not a Path2D");
    DCHECK(JS_IsArray(b->path), "a Path2D's record held something that is not its path array");
    return b->path;
}

static void p2d_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    Path2DBox *b = JS_GetAnyOpaque(val, &id);

    (void)id;
    if (!b) return;
    JS_FreeValueRT(rt, b->path);
    free(b);
}

static void p2d_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID id = 0;
    Path2DBox *b = JS_GetAnyOpaque(val, &id);

    (void)id;
    DCHECK(b != NULL, "a Path2D was marked with no path — p2d_alloc attaches the record before the object can "
                      "reach a collection");
    JS_MarkValue(rt, b->path, mark_func);
}

/* A new Path2D over a freshly created path, minted in `ctx` — which Web IDL makes the relevant realm of
   whatever built it, so a child navigable's `new Path2D()` is an instance of the CHILD's Path2D. */
static JSValue p2d_alloc(JSContext *ctx, JSValueConst new_target)
{
    JSValue proto, obj, path;
    Path2DBox *b;

    path = canvas_path_new(ctx);
    if (JS_IsException(path)) return path;

    /* Web IDL §3.8 "Platform objects implementing interfaces"'s *internally create a new object implementing
       the interface*: "If newTarget is undefined, then let prototype be the interface prototype object for
       interface in realm", otherwise "Let prototype be ? Get(newTarget, `prototype`)" and, where that is not
       an Object, "Let targetRealm be ? GetFunctionRealm(newTarget)" and the prototype "for interface in
       targetRealm". The first arm is what makes `class P extends Path2D {}` give its instances their own
       prototype; the LAST is why this reads the function's realm rather than `ctx` — `Reflect.construct(
       Path2D, [], otherRealmFunction)` must take the OTHER realm's Path2D.prototype, and a component whose
       prototypes are per-realm is exactly the one that can tell those apart.
       THE RECEIVER SLOT OF A CONSTRUCTOR CARRIES NEW.TARGET and not a `this` value — core/idl_args.c says so
       at the mint — so there is no brand to check here and nothing of §3.7's to ask. */
    if (JS_IsUndefined(new_target)) {
        proto = JS_GetClassProto(ctx, g_class);
    } else {
        proto = JS_GetPropertyStr(ctx, new_target, "prototype");
        if (JS_IsException(proto)) { JS_FreeValue(ctx, path); return proto; }
        if (!JS_IsObject(proto)) {
            JSContext *target_realm = JS_GetFunctionRealm(ctx, new_target);
            JS_FreeValue(ctx, proto);
            if (!target_realm) { JS_FreeValue(ctx, path); return JS_EXCEPTION; }
            proto = JS_GetClassProto(target_realm, g_class);
        }
    }
    obj = JS_NewObjectProtoClass(ctx, proto, g_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) { JS_FreeValue(ctx, path); return obj; }

    b = calloc(1, sizeof(*b));
    CHECK(b != NULL, "a Path2D's path record could not be allocated");
    b->path = path;
    JS_SetOpaque(obj, b);
    return obj;
}

/* §4.12.5.1.7's `Path2D(path)` CONSTRUCTOR STEPS, in the standard's own order. */
static JSValue js_p2d_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv, int magic)
{
    JSValue out, dst, src;
    Path2DBox *from;

    (void)magic;
    out = p2d_alloc(ctx, new_target);                                   /* step 1 */
    if (JS_IsException(out)) return out;
    if (argc < 1 || JS_IsUndefined(argv[0])) return out;                /* step 2 */

    dst = p2d_path_of(ctx, out);
    if (JS_IsException(dst)) { JS_FreeValue(ctx, out); return dst; }

    /* Step 3 — "If path is a Path2D object, then add all subpaths of path to output and return output. (In
       other words, it returns a copy of the argument.)" The argument's `(Path2D or DOMString)` union is
       DECLARED (IDL_STRING_UNLESS_IFACE with this class branded), so by here the value is either a real
       Path2D or a real string and this component runs no §3.2.25 clause of its own.
       NOTE WHICH ARM DOES NOT TAKE STEP 7: the copy arm returns after adding, while the string arm below goes
       on to open a trailing one-point subpath. The asymmetry is the standard's and is kept. */
    from = JS_GetOpaque(argv[0], g_class);
    if (from) {
        src = p2d_path_of(ctx, argv[0]);
        if (JS_IsException(src)) { JS_FreeValue(ctx, out); return src; }
        canvas_path_add_all(ctx, dst, src);
        return out;
    }

    /* Steps 4 to 7 — parse the SVG path data, then add its subpaths and open a subpath at its last point. */
    {
        const char *d = JS_ToCString(ctx, argv[0]);
        JSValue svg;
        double lx, ly;

        if (!d) { JS_FreeValue(ctx, out); return JS_EXCEPTION; }
        svg = canvas_path_new(ctx);
        if (JS_IsException(svg)) { JS_FreeCString(ctx, d); JS_FreeValue(ctx, out); return svg; }
        svg_path_data_parse(ctx, svg, d);                               /* step 4 */
        JS_FreeCString(ctx, d);

        /* Steps 5 to 7 run only where there IS a last point. SVG 2 §9.3.9 allows an empty path data string and its
           own error handling can leave a parse with no segment at all, and "Let (x, y) be the last point in
           svgPath" names nothing in that case — so an empty parse adds nothing and opens nothing, which is
           also the only reading under which step 7 does not invent a point. */
        if (!canvas_path_is_empty(ctx, svg)) {
            lx = canvas_path_header(ctx, svg, CANVAS_PATH_CUR_X);       /* step 5 */
            ly = canvas_path_header(ctx, svg, CANVAS_PATH_CUR_Y);
            canvas_path_add_all(ctx, dst, svg);                         /* step 6 */
            canvas_path_move_to(ctx, dst, lx, ly);                      /* step 7 */
        }
        JS_FreeValue(ctx, svg);
    }
    return out;                                                         /* step 8 */
}

/* ONE DECLARED `unrestricted double` POSITION, READ. Web IDL §3.2.8's conversion produces a Number, and the
   ONE other thing that can stand here is unknown external input, which the IDL boundary passes through AS
   ITSELF — CLAUDE.md §Every-value-is-CONCOLIC, and idl_concolic_rule leaves a numeric position at CROSSES. So
   exactly two values can arrive and the DCHECK over that pair is a statement about this codebase's own logic
   rather than about the page's value, which is what lets it be an assert at all.
   `JS_ToFloat64` IS NEVER CALLED ON THE UNKNOWN ARM, and that is the whole reason this is a helper. Opacity
   SURVIVES §7.1.4 ToNumber in this engine — that is what keeps control flow forking rather than collapsing to
   NaN — so a concolic handed to JS_ToFloat64 reaches the ToNumber boundary's own assert rather than yielding a
   double. The example is taken through concolic_example, which is the run-COMPUTED value and not an invented
   one.
 *
 * NAMED RESIDUAL. WHAT IS NOT COVERED: a coordinate that is unknown external input is recorded as its
 * concrete EXAMPLE rather than as itself, and one carrying no example reaches HTML §4.12.5.1.6's own step
 * "If any of the arguments are infinite or NaN, then return" instead of being placed — so the path's bytes stop
 * carrying the source identity that reached them, even though no fork is lost here, since a stored coordinate
 * is branched on by nothing in this component. WHAT THE NEXT DIFF BUILDS: the point-recording ops — moveTo,
 * lineTo, quadraticCurveTo and bezierCurveTo, which perform NO arithmetic at all and merely append what they
 * were given — take the JSValue and store it in the op stream verbatim, the array already being a JSValue
 * store; the four that COMPUTE (arcTo's tangent circle, ellipse's parametric points, rect's corners) keep a
 * double contract, because an affine of an unknown is a concolic and not a number and that is a
 * concolic-arithmetic question rather than a path one. HOW ITS ABSENCE WOULD SHOW: the day anything READS a
 * path back — §4.12.5.1.13's `isPointInPath`, which returns a boolean a page branches on — that branch would
 * be DECIDED by an example where the coordinate it rests on was never known. */
static double p2d_coord(JSContext *ctx, JSValueConst v)
{
    JSValue ex;
    double d = 0;

    if (JS_IsNumber(v)) {
        /* THE CONVERSION IS NOT INSIDE THE DCHECK. A DCHECK's condition is compiled out in release, so a
           condition that WRITES is a value the release build never sets — CLAUDE.md §DCHECK's "condition MUST
           be side-effect-free". The call runs, and the assert reads its result. */
        int ok = JS_ToFloat64(ctx, &d, v);
        DCHECK(ok == 0, "a Number at a declared `unrestricted double` position did not convert to a double");
        return d;
    }
    DCHECK(concolic_is(v),
           "a Path2D member's declared `unrestricted double` argument was neither a Number nor unknown "
           "external input — Web IDL §3.2.8's conversion produces the first and the IDL boundary passes the "
           "second through as itself, and there is no third thing that reaches a converted position");
    ex = concolic_example(ctx, v);
    if (JS_IsNumber(ex)) {
        int ok = JS_ToFloat64(ctx, &d, ex);
        DCHECK(ok == 0, "a concolic's Number example did not convert to a double");
        JS_FreeValue(ctx, ex);
        return d;
    }
    JS_FreeValue(ctx, ex);
    /* NOT ZERO. An unknown with no example has no coordinate, and zero would be a plausible datum — the origin
       is a point a page really draws at. NaN is the algorithm's OWN answer for a position it has no number
       for. HTML §4.12.5.1.6 opens every one of these members with "If any of the arguments are infinite or
       NaN, then return". */
    return NAN;
}

/* §4.12.5.1.6's members. Every argument is `unrestricted double` or `boolean` and is DECLARED, so each has
   already been converted by the time this runs and nothing here converts anything. */
static JSValue js_p2d_member(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue path = p2d_path_of(ctx, this_val);
    double a[8];
    int i, want;

    if (JS_IsException(path)) return path;
    DCHECK(magic >= 0 && magic < P2D_MEMBER_COUNT, "a Path2D member ran with a magic outside its own list");
    want = P2D_ARGC[magic];
    /* `arc` and `ellipse` end in `optional boolean counterclockwise = false`; every other position of every
       member is required, so Web IDL §3.6 has already thrown for a short call and each of these is present. */
    if (magic == P2D_ARC || magic == P2D_ELLIPSE) want--;
    DCHECK(argc >= want, "a Path2D member ran with fewer arguments than its required positions — Web IDL "
                         "§3.6's overload resolution throws before a body is reached");
    for (i = 0; i < want; i++) a[i] = p2d_coord(ctx, argv[i]);

    switch ((Path2DMember)magic) {
    case P2D_CLOSE_PATH:
        return canvas_path_close_path(ctx, path) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    case P2D_MOVE_TO:
        return canvas_path_move_to(ctx, path, a[0], a[1]) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    case P2D_LINE_TO:
        return canvas_path_line_to(ctx, path, a[0], a[1]) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    case P2D_QUADRATIC_CURVE_TO:
        return canvas_path_quadratic_curve_to(ctx, path, a[0], a[1], a[2], a[3]) < 0
               ? JS_EXCEPTION : JS_UNDEFINED;
    case P2D_BEZIER_CURVE_TO:
        return canvas_path_bezier_curve_to(ctx, path, a[0], a[1], a[2], a[3], a[4], a[5]) < 0
               ? JS_EXCEPTION : JS_UNDEFINED;
    case P2D_ARC_TO:
        return canvas_path_arc_to(ctx, path, a[0], a[1], a[2], a[3], a[4]) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    case P2D_RECT:
        return canvas_path_rect(ctx, path, a[0], a[1], a[2], a[3]) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    case P2D_ARC:
        return canvas_path_arc(ctx, path, a[0], a[1], a[2], a[3], a[4],
                               argc > 5 && JS_ToBool(ctx, argv[5])) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    case P2D_ELLIPSE:
        return canvas_path_ellipse(ctx, path, a[0], a[1], a[2], a[3], a[4], a[5], a[6],
                                   argc > 7 && JS_ToBool(ctx, argv[7])) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    case P2D_MEMBER_COUNT:
        break;
    }
    /* The operand is this component's own enum over a closed set the DCHECK above has already narrowed, so
       the arm is unreachable by construction and is a guard rather than an unbuilt capability. */
    DFAIL("a Path2D member dispatched on a magic its own member list does not contain");
    return JS_UNDEFINED;
}

/* ---- the declaration and the per-realm install ------------------------------------------------------------ */

void path_2d_init(JSContext *ctx)
{
    JSClassDef def = { "Path2D", p2d_finalizer, p2d_gc_mark };
    static const IdlArgType CTOR[1] = { IDL_STRING_UNLESS_IFACE };
    static const IdlArgType DOUBLES[8] = {
        IDL_UNRESTRICTED_DOUBLE, IDL_UNRESTRICTED_DOUBLE, IDL_UNRESTRICTED_DOUBLE, IDL_UNRESTRICTED_DOUBLE,
        IDL_UNRESTRICTED_DOUBLE, IDL_UNRESTRICTED_DOUBLE, IDL_UNRESTRICTED_DOUBLE, IDL_UNRESTRICTED_DOUBLE
    };
    /* `arc` and `ellipse` end in `optional boolean counterclockwise = false`, so their last position is a
       boolean and every earlier one an unrestricted double. Two tables rather than a per-member one, because
       every other member's positions are doubles all the way down. */
    static const IdlArgType ARC_ARGS[6] = {
        IDL_UNRESTRICTED_DOUBLE, IDL_UNRESTRICTED_DOUBLE, IDL_UNRESTRICTED_DOUBLE, IDL_UNRESTRICTED_DOUBLE,
        IDL_UNRESTRICTED_DOUBLE, IDL_BOOLEAN
    };
    static const IdlArgType ELLIPSE_ARGS[8] = {
        IDL_UNRESTRICTED_DOUBLE, IDL_UNRESTRICTED_DOUBLE, IDL_UNRESTRICTED_DOUBLE, IDL_UNRESTRICTED_DOUBLE,
        IDL_UNRESTRICTED_DOUBLE, IDL_UNRESTRICTED_DOUBLE, IDL_UNRESTRICTED_DOUBLE, IDL_BOOLEAN
    };
    int i;

    DCHECK(g_class == 0, "path_2d_init ran twice — §4.12.5.1.7's class is declared once per AGENT");
    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    JS_NewClass(JS_GetRuntime(ctx), g_class, &def);

    /* `constructor(optional (Path2D or DOMString) path)`. The union is DECLARED: an object of this class
       crosses as itself and everything else is converted to a DOMString, which is exactly §3.2.25's brand
       clause followed by its string one — so `new Path2D(123)` parses the path data "123" (and finds no
       leading moveto) rather than throwing, and the page's own `toString` runs AT the conversion boundary
       rather than from inside a C activation of this file. */
    g_id_ctor = idl_method_id(ctx, CTOR, 1, js_p2d_ctor, 0);
    idl_iface_brand(g_class);
    idl_optional_from(0);

    for (i = 0; i < P2D_MEMBER_COUNT; i++) {
        const IdlArgType *types = DOUBLES;
        if (i == P2D_ARC) types = ARC_ARGS;
        else if (i == P2D_ELLIPSE) types = ELLIPSE_ARGS;
        g_id_m[i] = idl_method_id(ctx, P2D_ARGC[i] ? types : NULL, P2D_ARGC[i], js_p2d_member, i);
        /* `arc`'s and `ellipse`'s trailing `counterclockwise` is the one optional position in the mixin. */
        if (i == P2D_ARC || i == P2D_ELLIPSE) idl_optional_from(P2D_ARGC[i] - 1);
    }

    agent_state_class("path_2d", &g_class, "§4.12.5.1.7's Path2D class, and the declaration latch");
    agent_state_id("path_2d", &g_id_ctor, "§4.12.5.1.7's constructor declaration");
    realm_declare_intrinsic(path_2d_install_realm);
}

/* Web IDL §3.7.3's interface prototype object for §4.12.5.1.7 Path2D objects, its §3.7.1 interface object, and
   §3.8's property reference for the one name — for ONE realm.
 *
 * NAMED RESIDUAL — `addPath`. WHAT IS NOT COVERED: §4.12.5.1.7's `undefined addPath(Path2D path, optional
 * DOMMatrix2DInit transform = {})` is not placed on this prototype, so a page that calls it gets Web IDL's
 * TypeError for an absent member. WHAT THE NEXT DIFF BUILDS: Geometry Interfaces §6.1's "validate and fixup
 * (2D)" over a DOMMatrix2DInit declaration — whose `a`/`m11` pairs throw a TypeError when both are present and
 * not SameValueZero, and default to the identity otherwise — then Geometry Interfaces §6.3's "create a DOMMatrix
 * from a 2D dictionary" yielding the six elements addPath step 3 tests for infinity and NaN, and a `const double *m`
 * parameter on canvas_path_add_all which maps a MOVE/LINE/QUAD/CUBIC's coordinates directly and decomposes an
 * ARC, an affine image of an ellipse being another ellipse whose centre, two radii, rotation, parameter offset
 * and sweep direction all move. HOW ITS ABSENCE WOULD SHOW: a document that composes one path out of several
 * — the shape every drawing surface takes when it batches — reaches the member and its flow ends there,
 * having already built every path it was going to combine.
 *
 * NAMED RESIDUAL — `roundRect`. WHAT IS NOT COVERED: §4.12.5.1.6's `roundRect` is not placed, for the same
 * TypeError. WHAT THE NEXT DIFF BUILDS: an IdlArgType for its `optional (unrestricted double or DOMPointInit
 * or sequence<(unrestricted double or DOMPointInit)>) radii = 0` — a three-armed union no position in this
 * platform declares yet, which is a row in core/idl_args.h and a conversion beside it rather than a test in
 * this body, since §3.2.25's sequence clause reads @@iterator and can therefore park — and then the section's
 * own steps: the list-size RangeError, the per-radius normalization, the four-corner assignment for a list of
 * one, two, three or four, and the scale-to-prevent-overlap that CSS 'border-radius' shares. HOW ITS ABSENCE
 * WOULD SHOW: a page that rounds a rectangle ON A PATH rather than on a context reaches the member and its
 * flow ends there. (The corpus's own roundRect calls are all on a rendering context and all feature-detected,
 * which is why this one is a residual and `addPath` is the member to build first.) */
void path_2d_install_realm(JSContext *ctx)
{
    JSValue proto, prev, ctor, global;
    int i;

    DCHECK(g_class != 0, "a realm asked for Path2D.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_class);
    DCHECK(JS_IsNull(prev), "path_2d_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "Path2D.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "Path2D");
    /* `Path2D includes CanvasPath` — Web IDL §3.7.3 gives a mixin no prototype of its own, so its members are
       placed on the INCLUDER's, which is why flattening them here is the standard's own shape and not this
       engine's shortcut. */
    for (i = 0; i < P2D_MEMBER_COUNT; i++) {
        DCHECK(g_id_m[i] >= 0, "a Path2D member's interface object was built before path_2d_init declared it");
        idl_install_method(ctx, proto, P2D_NAMES[i], g_id_m[i]);
    }

    global = JS_GetGlobalObject(ctx);
    DCHECK(g_id_ctor >= 0, "Path2D's interface object was built before path_2d_init declared its constructor");
    ctor = idl_step_constructor(ctx, "Path2D", g_id_ctor);
    CHECK(!JS_IsException(ctor), "the Path2D interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    idl_define_global_property_reference(ctx, global, "Path2D", ctor);
    JS_FreeValue(ctx, global);

    JS_SetClassProto(ctx, g_class, proto);      /* the realm owns it from here */
}

void path_2d_free(void)
{
    int i;

    /* The prototype is the REALMS' and the pool entries are the agent's. THE CLASS ID COMES BACK TOO, because
       path_2d_init consults it to decide whether it has anything to do — leaving it set would make a second
       agent's Path2D a class registered in a runtime that no longer exists. See core/agent_state.h. */
    g_class = 0;
    g_id_ctor = -1;
    for (i = 0; i < P2D_MEMBER_COUNT; i++) g_id_m[i] = -1;
}
