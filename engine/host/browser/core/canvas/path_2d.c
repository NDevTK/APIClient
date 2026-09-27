/* HTML §4.12.5.1.7 "Path2D objects", and §4.12.5.1.6 "Building paths"'s mixin members placed on its
   prototype. See path_2d.h for why this is buildable with no rendering context. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/canvas/canvas_path.h"
#include "core/canvas/canvas_path_members.h"
#include "core/canvas/path_2d.h"
#include "core/canvas/svg_path_data.h"
#include "core/idl_args.h"
#include "core/realm.h"
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

/* §4.12.5.1.6's members are the MIXIN's — its list, its names, its arities, its declared types and its
   dispatch all live in core/canvas/canvas_path.h, which says why a second includer is what moved them there.
   THE THREE TABLES THAT STOOD HERE ARE DELETED RATHER THAN KEPT BESIDE IT: an enum, a name table and an argc
   table copied per includer are three facts free to disagree about an arity, and CLAUDE.md
   §A-superseded-system-is-DELETED-in-the-same-diff forbids the copy surviving the thing that replaced it.
   `roundRect` is absent from the mixin's list too — see the residual at the install below. */
static int g_id_m[CANVAS_PATH_M_COUNT];

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

/* §4.12.5.1.6's members. THE BODY, THE COORDINATE READ AND THE NINE-ARM SWITCH THAT STOOD HERE ARE DELETED
   AND ROUTED to core/canvas/canvas_path.h's `canvas_path_member`, because a second interface now includes the
   same mixin and a copy of one dispatch per includer is the two-right-answers shape. What is left here is the
   only part that is genuinely this interface's: WHICH PATH the members build, which `p2d_path_of` answers
   through the class opaque and with the Web IDL §3.7.7 TypeError a page-supplied receiver is owed.
   THE COORDINATE READ WENT FURTHER THAN THE MIXIN: it was a local spelling of `core/idl_args.h`'s
   `idl_number_of`, which is this engine's canonical answer for what a converted numeric argument denotes and
   has several other callers, so the mixin routes to THAT rather than carrying the copy forward. */
static JSValue js_p2d_member(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue path = p2d_path_of(ctx, this_val);

    if (JS_IsException(path)) return path;
    return canvas_path_member(ctx, path, magic, argc, argv) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

/* ---- the declaration and the per-realm install ------------------------------------------------------------ */

void path_2d_init(JSContext *ctx)
{
    JSClassDef def = { "Path2D", p2d_finalizer, p2d_gc_mark };
    static const IdlArgType CTOR[1] = { IDL_STRING_UNLESS_IFACE };
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

    /* The mixin's own tables answer every position, every arity and every optional cursor, so this loop states
       which interface is being declared and nothing about what CanvasPath IS. */
    for (i = 0; i < CANVAS_PATH_M_COUNT; i++) {
        int opt;

        g_id_m[i] = idl_method_id(ctx, canvas_path_member_types(i), CANVAS_PATH_M_ARGC[i], js_p2d_member, i);
        opt = canvas_path_member_optional_from(i);
        if (opt >= 0) idl_optional_from(opt);
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
 * flow ends there.
 * THIS CLAUSE ENDED `all on a rendering context and all feature-detected`, AND THE SECOND HALF IS REFUTED
 * RATHER THAN CORRECTED, because the reasoning it supports still holds and a reader re-deriving it from the
 * guards would re-state the absolute. The receiver half stands: every `roundRect` call in the mirrored corpus
 * is on a rendering context and none is on a `Path2D`, which is why this member is a residual here and
 * `addPath` is the one to build first. The `all` does not: measured over that mirror, `.roundRect` carries
 * THIRTEEN guard-shaped reads against SIXTEEN calls, and several calls sit in expressions with no presence
 * test of the member anywhere in them — so an absolute over a population is a promise about every site nobody
 * opened (CLAUDE.md §AN-OVER-CLAIM-IS-REFUTABLE), and one grep ends it. THE DERIVATION, because that corpus
 * is untracked by design and its figures move: count `.roundRect` in the mirror's `.js` files and read the
 * character that FOLLOWS each hit — `(` is a call and `?`, `??` or `&&` is a guard's read — with an invented
 * member name beside it as the control. Three of the reads are neither: they are a POLYFILL, installing its
 * own implementation under `CanvasRenderingContext2D.prototype.roundRect ?? (m.roundRect = P)`, whose body
 * builds the shape out of `this.moveTo`, `this.lineTo`, `this.ellipse` and `this.closePath`. */
void path_2d_install_realm(JSContext *ctx)
{
    JSValue proto, prev, ctor, global;

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
    /* THE NAME IS WRITTEN AS A LITERAL AT EVERY ONE, AND THE LOOP THAT STOOD HERE IS DELETED FOR A MEASURED
       REASON RATHER THAN A STYLISTIC ONE. `engine/idlgen.mjs` is this project's GAP AUDITOR: it reads the real
       `.idl` and diffs it against what each component INSTALLS, by reading the install's name statically. A
       `CANVAS_PATH_M_NAMES[i]` there is a table in ANOTHER FILE, which it reports as an install construct whose
       member name could not be resolved — so it credits none of the nine and lists every one of them as a GAP.
       Measured on the diff that moved these tables: Path2D's row went from ABSENT 2 to ABSENT 11 and the 2D
       context's kept all nine, eighteen rows of a queue naming members that are installed and answering. That
       is §A-COUNT-OVER-SOURCE-TEXT-IS-A-COUNT-OF-A-SPELLING in the ACCUSING direction, in the one instrument a
       coordinator scopes a canvas lane from.
       THE COPY IS NOT FREE TO DISAGREE, WHICH IS WHAT MAKES IT A SPELLING AND NOT A SECOND FACT: the loop below
       asserts every name the MIXIN declares is on this prototype, so a member added to `CANVAS_PATH_M_NAMES`
       and left out here aborts the realm rather than going quietly missing, and the arity, the declared types
       and the body are still the mixin's alone. */
    idl_install_method(ctx, proto, "closePath",        g_id_m[CANVAS_PATH_M_CLOSE_PATH]);
    idl_install_method(ctx, proto, "moveTo",           g_id_m[CANVAS_PATH_M_MOVE_TO]);
    idl_install_method(ctx, proto, "lineTo",           g_id_m[CANVAS_PATH_M_LINE_TO]);
    idl_install_method(ctx, proto, "quadraticCurveTo", g_id_m[CANVAS_PATH_M_QUADRATIC_CURVE_TO]);
    idl_install_method(ctx, proto, "bezierCurveTo",    g_id_m[CANVAS_PATH_M_BEZIER_CURVE_TO]);
    idl_install_method(ctx, proto, "arcTo",            g_id_m[CANVAS_PATH_M_ARC_TO]);
    idl_install_method(ctx, proto, "rect",             g_id_m[CANVAS_PATH_M_RECT]);
    idl_install_method(ctx, proto, "arc",              g_id_m[CANVAS_PATH_M_ARC]);
    idl_install_method(ctx, proto, "ellipse",          g_id_m[CANVAS_PATH_M_ELLIPSE]);
#if APICLIENT_DEV
    /* Every name the mixin declares, placed. A loop and therefore a `#if` block: a DCHECK's condition must be
       side-effect-free and these reads allocate — and the cursor is declared INSIDE it, because a declaration
       outside is a variable the release build carries and uses nowhere. */
    {
        int i;

        for (i = 0; i < CANVAS_PATH_M_COUNT; i++) {
            JSValue m = JS_GetPropertyStr(ctx, proto, CANVAS_PATH_M_NAMES[i]);
            bool placed = JS_IsFunction(ctx, m);

            JS_FreeValue(ctx, m);
            DCHECKF(placed, "§4.12.5.1.6 declares `%s` and Path2D.prototype does not carry it — the mixin's "
                            "member list grew and this includer's install did not, which "
                            "core/canvas/canvas_path_members.h says is the one thing a shared list cannot "
                            "prevent by itself", CANVAS_PATH_M_NAMES[i]);
        }
    }
#endif

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
    for (i = 0; i < CANVAS_PATH_M_COUNT; i++) g_id_m[i] = -1;
}
