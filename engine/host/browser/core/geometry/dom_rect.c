/* GEOMETRY INTERFACES §3 — the DOMRect interfaces. See dom_rect.h for why this is its own component.
 *
 * THE FOUR INTERNAL MEMBER VARIABLES ARE JSValues, NOT doubles, AND THAT IS THE ONE DESIGN DECISION IN THIS
 * FILE. §3 declares them `unrestricted double`, so four `double`s look like the record's obvious shape — and
 * they silently DE-TAINT. `r.x = location.hash.length` stores unknown external input; a C `double` can hold
 * only the example, so the very next `r.x` reads back a bare number and the value has stopped forking control
 * flow and stopped being solvable at a sink. CLAUDE.md states the rule for exactly this shape (a builtin's
 * opaque operand is "the OPAQUE itself, taint preserved, never a de-tainting placeholder"), and a JSValue is
 * also what §PLATFORM-DATA already requires of anything a flow may park: the four are ordinary own values that
 * the COW delta captures, the snapshot machinery carries and the cold tier writes out, with nothing written
 * here to make any of that happen.
 *
 * SO THE FOUR ATTRIBUTES §3 STORES ARE ANSWERED BY HANDING BACK WHAT WAS STORED, and the four it DERIVES
 * (`top`, `right`, `bottom`, `left`) are the only place this file computes anything. Their derivation is the
 * NaN-safe minimum/maximum of an origin and an origin-plus-extent, and it is NOT C's `fmin`/`fmax`: those
 * return the non-NaN operand, and §3 says the answer is NaN if any member of the list is NaN. `new
 * DOMRect(0, 0, NaN, 0).right` is NaN, and Geometry's own DOMRect-nan test is what that is for.
 *
 * A DERIVED EDGE OVER AN UNKNOWN OPERAND DOES NOT FORK. The minimum's comparison is a branch INSIDE a builtin,
 * over a value whose two arms are both derived from the same tainted source, so forking it explores nothing and
 * doubles the frontier — CLAUDE.md's short-circuit rule for exactly that case. The edge yields the OPAQUE, with
 * the real derivation run on the operands' examples as the example, so `rect.top` stays as solvable as `rect.y`
 * was and carries a concrete number wherever the source has one.
 *
 * WHAT IS NOT HERE, DELIBERATELY: any statement about where a box is. §3 is four numbers and four derivations
 * over them and is complete without a layout engine — see core/dom/element_view.h for the component that has to
 * decide WHICH four numbers an element's box has, and for the crash that stands where that decision needs a
 * layout this engine does not have. */
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/geometry/dom_rect.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/concolic.h"
#include "solver/cow.h"

/* §3's four internal member variables. "Internal member variables must not be exposed in any way", which is
   what keeps them behind the class opaque rather than on an own slot the page could reach. */
typedef struct {
    JSValue x, y, width, height;
} DomRectBox;

/* THE ONE STATEMENT OF WHAT THE RECORD OWNS — read by the COW capture, by the finalizer and by the gc_mark, so
   a field added to the struct and not to this list is caught by reading the three together. */
static const uint16_t DOM_RECT_OFF[] = {
    (uint16_t)offsetof(DomRectBox, x),     (uint16_t)offsetof(DomRectBox, y),
    (uint16_t)offsetof(DomRectBox, width), (uint16_t)offsetof(DomRectBox, height),
};
static const CowRecord DOM_RECT_REC = { sizeof(DomRectBox), DOM_RECT_OFF, 4 };

/* THE MEMBERS. The first four are §3's stored variables and are the four `DOMRect` redeclares with `inherit
   attribute`; the last four are its derivations. The getter's magic IS the member. */
typedef enum {
    DR_X = 0, DR_Y, DR_WIDTH, DR_HEIGHT,
    DR_TOP, DR_RIGHT, DR_BOTTOM, DR_LEFT,
    DR_MEMBER_COUNT
} DomRectMember;

/* Which of the two interfaces a constructor, a static or a prototype install is acting for. `DR_MUTABLE` is the
   whole difference between them: §3 gives DOMRect the same four variables and adds the four setters. */
enum { DR_READONLY = 0, DR_MUTABLE = 1 };

static JSClassID g_ro_class, g_rect_class;
static int g_id_ctor_ro = -1, g_id_ctor_rect = -1;
static int g_id_from_ro = -1, g_id_from_rect = -1;
static int g_id_tojson = -1;
static int g_id_set[4] = { -1, -1, -1, -1 };

/* ---- the record ------------------------------------------------------------------------------------------ */

/* §3.7.6 Attributes' and §3.7.7 Operations' BRAND — the accessors and `toJSON` share this one entry — and the
   point at which the flow REACHES the record. A record a flow has reached is one it may
   write, and capturing here is what makes it impossible to have a `r.x = …` this delta did not see — CLAUDE.md's
   rule for a component's own C record. The delta dedups to one entry per flow. */
static DomRectBox *dr_box(JSValueConst v)
{
    DomRectBox *b = JS_GetOpaque(v, g_ro_class);

    if (!b) b = JS_GetOpaque(v, g_rect_class);
    if (b) cow_capture_host_record(v, b, &DOM_RECT_REC);
    return b;
}

/* WRITE ONE OF §3'S FOUR STORED VARIABLES, and never `JS_FreeValue(ctx, *slot); *slot = <a new value>;` — see
   cow.h for the order and the defect. The RELEASE is the side that bites here: a variable holding unknown
   external input is an object, so giving it back can drop the last reference to one whose finalizer may
   allocate, and an allocation IS a collection (js_trigger_gc has exactly one caller, JS_NewObjectFromShape)
   that reaches this record through dr_gc_mark and decrefs a JSObject already back on the allocator's free
   list. The setter writes THROUGH a pointer it picked from a switch, so the layout assert this operation
   carries is the only thing that can say the pointer named one of the four the list knows.
   dr_alloc's mint does not come here: it fills the box before JS_SetOpaque, where the collector cannot reach
   it and its slots hold no value to release. */
/* THE ADDRESS PASSES THROUGH: the asserts inside are about the SLOT, so they must name the WRITE and not this
   line — see cow.h's THE SITE TRAVELS WITH THE OPERATION. */
static void dr_set_at(JSContext *ctx, DomRectBox *b, JSValue *slot, JSValue v,
                      const char *file, int line)
{
    cow_record_set_at(ctx, b, &DOM_RECT_REC, slot, v, file, line);
}
#define dr_set(ctx_, b_, slot_, v_) dr_set_at((ctx_), (b_), (slot_), (v_), __FILE__, __LINE__)

bool dom_rect_is(JSValueConst v)
{
    return JS_GetOpaque(v, g_ro_class) != NULL || JS_GetOpaque(v, g_rect_class) != NULL;
}

/* The brand, with §3.7.6 Attributes' TypeError pending when it fails: `DOMRectReadOnly.prototype.x` read off
   a plain
   object throws, and a page's feature detector reads that throw as "this is a real interface". */
static DomRectBox *dr_this(JSContext *ctx, JSValueConst this_val)
{
    DomRectBox *b = dr_box(this_val);

    if (!b) JS_ThrowTypeError(ctx, "a DOMRect member was reached on something that is not a DOMRect");
    return b;
}

/* THE COLLECTOR'S TWO ENTRIES REACH THE RECORD FROM THE OBJECT AND READ NO STATIC OF THIS FILE — see
   core/agent_state.h's note on what a release owes a finalizer. dom_rect_free is a row on core/platform.h's
   release column and gives BOTH class ids back to 0, and platform_agent_free runs BEFORE the collection that
   finalizes the page's object graph, so these two run with g_ro_class and g_rect_class already zero. Asking
   JS_GetOpaque for class 0 answers NULL for every rectangle a page still held, and the two halves fail
   differently: the finalizer would leak the box and never subtract its four references, while the MARK is
   worse — an unmarked child keeps the internal reference gc_decref exists to subtract, so gc_scan reads it as
   rooted from outside the heap and the object is never collected at all.
   The id is not read because it is not needed: the collector dispatched here THROUGH the class, and §3 and §4
   keep the same record, which is why one pair of entries serves both. JS_GetAnyOpaque and never dr_box, for
   the reason every other component's mark reaches past its accessor: a capture during collection would dup
   values on an object being torn down. */
static void dr_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    DomRectBox *b = JS_GetAnyOpaque(val, &id);

    (void)id;
    /* NOT `if (!b) return;`. dr_alloc is the one mint, and it places the record on the object with nothing
       between JS_NewObjectProtoClass and JS_SetOpaque that can fail or allocate — so there is no half-built
       rectangle for either of these entries to meet, and a NULL here means an object wearing one of §3's
       classes was built somewhere that is not this file. */
    DCHECK(b != NULL, "a DOMRect was finalized with no internal member variables — dr_alloc attaches the "
                      "record before the object can reach anything that would free it");
    JS_FreeValueRT(rt, b->x);
    JS_FreeValueRT(rt, b->y);
    JS_FreeValueRT(rt, b->width);
    JS_FreeValueRT(rt, b->height);
    free(b);
}

static void dr_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID id = 0;
    DomRectBox *b = JS_GetAnyOpaque(val, &id);

    (void)id;
    DCHECK(b != NULL, "a DOMRect was marked with no internal member variables — dr_alloc attaches the record "
                      "before the object can reach a collection");
    JS_MarkValue(rt, b->x, mark_func);
    JS_MarkValue(rt, b->y, mark_func);
    JS_MarkValue(rt, b->width, mark_func);
    JS_MarkValue(rt, b->height, mark_func);
}

/* §3's `= 0`, WHICH THE BODY APPLIES AND WHICH IS STATED HERE ONCE. The constructor's four arguments and
   DOMRectInit's four members carry the same `= 0`, and the two arrive by different routes: an optional argument
   the page omitted or passed `undefined` for is ABSENT (Web IDL §3.6 "Overload resolution algorithm", and
   idl_args.c hands the body `undefined` to say so), and an absent dictionary member simply is not on the
   converted dictionary. Both are a PRODUCER'S POSITIVE STATEMENT that the page wrote nothing there, which is
   exactly what the IDL's default is about — not a hole a `||` fills, since IDL_UNRESTRICTED_DOUBLE never
   produces `undefined` for a value the page did write. One rule, one place; a numeric default expressible
   only on dictionary members would leave the positional half restating it. CONSUMES `v`. */
static JSValue dr_value(JSContext *ctx, JSValue v)
{
    if (JS_IsUndefined(v)) return JS_NewFloat64(ctx, 0.0);
    DCHECK(JS_IsNumber(v) || concolic_is(v),
           "a DOMRect internal member variable was given something that is neither a Number nor unknown "
           "external input — its IDL type is `unrestricted double`, whose conversion produces a Number, and "
           "unknown input is the one thing the IDL boundary passes through as itself");
    return v;
}

/* ---- §3's construction ------------------------------------------------------------------------------------ */

static JSValue dr_alloc(JSContext *ctx, int which, JSValue x, JSValue y, JSValue w, JSValue h)
{
    JSClassID cls = which == DR_MUTABLE ? g_rect_class : g_ro_class;
    JSValue proto, obj;
    DomRectBox *b;

    DCHECK(which == DR_READONLY || which == DR_MUTABLE,
           "a rectangle was allocated for neither of the two interfaces §3 declares");
    DCHECK(cls != 0, "a rectangle was built before dom_rect_init declared its classes");
    proto = JS_GetClassProto(ctx, cls);
    DCHECK(!JS_IsNull(proto), "a rectangle was built in a realm that never ran its prototype install");
    obj = JS_NewObjectProtoClass(ctx, proto, cls);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "a DOMRect could not be allocated");
    b = calloc(1, sizeof(*b));
    CHECK(b != NULL, "the DOMRect record allocation failed");
    /* Step 2 — "set rect's variables x coordinate to x, y coordinate to y, width dimension to width and height
       dimension to height". Every owned field is placed before anything that can fail. */
    b->x = x;
    b->y = y;
    b->width = w;
    b->height = h;
    JS_SetOpaque(obj, b);
    return obj;
}

JSValue dom_rect_new(JSContext *ctx, double x, double y, double width, double height)
{
    return dr_alloc(ctx, DR_MUTABLE, JS_NewFloat64(ctx, x), JS_NewFloat64(ctx, y),
                    JS_NewFloat64(ctx, width), JS_NewFloat64(ctx, height));
}

JSValue dom_rect_new_values(JSContext *ctx, JSValue x, JSValue y, JSValue width, JSValue height)
{
    return dr_alloc(ctx, DR_MUTABLE, dr_value(ctx, x), dr_value(ctx, y),
                    dr_value(ctx, width), dr_value(ctx, height));
}

JSValue dom_rect_readonly_new_values(JSContext *ctx, JSValue x, JSValue y, JSValue width, JSValue height)
{
    return dr_alloc(ctx, DR_READONLY, dr_value(ctx, x), dr_value(ctx, y),
                    dr_value(ctx, width), dr_value(ctx, height));
}

/* `DOMRectReadOnly(x, y, width, height)` and `DOMRect(x, y, width, height)` — one body, because §3 gives them
   the same three steps and the interface is the magic. */
static JSValue js_dr_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue a[4];
    int i;

    (void)this_val;
    for (i = 0; i < 4; i++)
        a[i] = dr_value(ctx, i < argc ? JS_DupValue(ctx, argv[i]) : JS_UNDEFINED);
    return dr_alloc(ctx, magic, a[0], a[1], a[2], a[3]);
}

/* §3's `fromRect(other)` — "create a DOMRectReadOnly / DOMRect from the dictionary other", which is the same
   three steps reading four dictionary members instead of four arguments. */
static JSValue js_dr_from_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 0 ? argv[0] : JS_UNDEFINED;

    (void)this_val;
    return dr_alloc(ctx, magic,
                    dr_value(ctx, idl_dict_get(ctx, init, "x")),
                    dr_value(ctx, idl_dict_get(ctx, init, "y")),
                    dr_value(ctx, idl_dict_get(ctx, init, "width")),
                    dr_value(ctx, idl_dict_get(ctx, init, "height")));
}

/* §3's `DOMRectInit`, in LEXICOGRAPHIC order — Web IDL §3.2.17 reads a dictionary's members that way and a page
   with getters on the initialiser observes which one runs first. None is `required`; each carries `= 0`, which
   dr_value applies for the reason stated there. */
static const IdlDictMember DOM_RECT_INIT[] = {
    { "height", IDL_UNRESTRICTED_DOUBLE, false },
    { "width",  IDL_UNRESTRICTED_DOUBLE, false },
    { "x",      IDL_UNRESTRICTED_DOUBLE, false },
    { "y",      IDL_UNRESTRICTED_DOUBLE, false },
};

/* THE SAME LIST, NAMED — see dom_rect.h for why a NESTED member needs the identifier and the count that an
   argument position does not. The extent is written HERE and nowhere else: the two `fromRect` declarations
   below read `DOM_RECT_INIT_DECL.n` rather than restating a `sizeof`, so there is one statement of how many
   members §3 declares and nothing for a fifth to be added past. */
const IdlDictDecl DOM_RECT_INIT_DECL = {
    "DOMRectInit", DOM_RECT_INIT, (int)(sizeof(DOM_RECT_INIT) / sizeof(DOM_RECT_INIT[0]))
};

/* ---- §3's eight attributes -------------------------------------------------------------------------------- */

/* THE NaN-SAFE MINIMUM AND MAXIMUM of a two-value list — Geometry's own definition: "NaN if any member of the
   list is NaN, or the minimum of the list otherwise". C's `fmin`/`fmax` answer the OTHER operand for a NaN,
   which is the opposite rule, so neither may be used here.
   THE ZEROES ARE DISTINGUISHED, because the two of them are the one pair that compares equal and is still
   observable: the minimum of -0 and +0 is -0 and the maximum is +0, which is ECMAScript's own reading of
   "minimum" in `Math.min` and is what `Object.is` reports back to a page. */
static double dr_nan_min(double a, double b)
{
    if (isnan(a) || isnan(b)) return NAN;
    if (a < b) return a;
    if (b < a) return b;
    return signbit(a) ? a : b;
}

static double dr_nan_max(double a, double b)
{
    if (isnan(a) || isnan(b)) return NAN;
    if (a > b) return a;
    if (b > a) return b;
    return signbit(a) ? b : a;
}

/* ONE DERIVED EDGE. `top` is the NaN-safe minimum of the y coordinate and y + height; `bottom` is their maximum;
   `left` and `right` are the same two over x and x + width. `member` names the attribute, and is read only by
   the unknown-operand arm below. */
static JSValue dr_edge(JSContext *ctx, JSValueConst origin, JSValueConst extent, bool maximum,
                       const char *member)
{
    double o, e;

    if (concolic_is(origin) || concolic_is(extent)) {
        /* THE SHORT-CIRCUIT, not a fork: see the file header. The example is the REAL derivation run on the
           operands' own examples — never predicted, and absent when either operand has none to run it on. */
        JSValueConst tainted = concolic_is(origin) ? origin : extent;
        JSValue oe = concolic_is(origin) ? concolic_example(ctx, origin) : JS_DupValue(ctx, origin);
        JSValue ee = concolic_is(extent) ? concolic_example(ctx, extent) : JS_DupValue(ctx, extent);
        JSValue example = JS_UNDEFINED;

        if (JS_IsNumber(oe) && JS_IsNumber(ee)) {
            JS_ToFloat64(ctx, &o, oe);
            JS_ToFloat64(ctx, &e, ee);
            example = JS_NewFloat64(ctx, maximum ? dr_nan_max(o, o + e) : dr_nan_min(o, o + e));
        }
        JS_FreeValue(ctx, oe);
        JS_FreeValue(ctx, ee);
        return concolic_builtin_hook(ctx, tainted, member, example);
    }
    DCHECK(JS_IsNumber(origin) && JS_IsNumber(extent),
           "a DOMRect derived edge read an internal member variable that is neither a Number nor unknown input "
           "— every write to one goes through dr_value, which admits exactly those two");
    JS_ToFloat64(ctx, &o, origin);
    JS_ToFloat64(ctx, &e, extent);
    return JS_NewFloat64(ctx, maximum ? dr_nan_max(o, o + e) : dr_nan_min(o, o + e));
}

static JSValue dr_member(JSContext *ctx, const DomRectBox *b, DomRectMember m)
{
    switch (m) {
    case DR_X:      return JS_DupValue(ctx, b->x);
    case DR_Y:      return JS_DupValue(ctx, b->y);
    case DR_WIDTH:  return JS_DupValue(ctx, b->width);
    case DR_HEIGHT: return JS_DupValue(ctx, b->height);
    case DR_TOP:    return dr_edge(ctx, b->y, b->height, false, "top");
    case DR_RIGHT:  return dr_edge(ctx, b->x, b->width,  true,  "right");
    case DR_BOTTOM: return dr_edge(ctx, b->y, b->height, true,  "bottom");
    case DR_LEFT:   return dr_edge(ctx, b->x, b->width,  false, "left");
    case DR_MEMBER_COUNT: break;
    }
    DFAIL("a DOMRect attribute ran with a magic §3 does not declare — the magic IS the member, so an unknown "
          "one means a name was installed without a case to answer it");
    return JS_UNDEFINED;
}

static JSValue js_dr_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    DomRectBox *b = dr_this(ctx, this_val);

    if (!b) return JS_EXCEPTION;
    return dr_member(ctx, b, (DomRectMember)magic);
}

/* §3: "For the DOMRect interface, setting the x attribute must set the x coordinate to the new value." Only the
   four stored variables have a setter, which is what `inherit attribute` on exactly those four says. */
static JSValue js_dr_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    DomRectBox *b = dr_this(ctx, this_val);
    JSValue *slot;

    if (!b) return JS_EXCEPTION;
    switch ((DomRectMember)magic) {
    case DR_X:      slot = &b->x; break;
    case DR_Y:      slot = &b->y; break;
    case DR_WIDTH:  slot = &b->width; break;
    case DR_HEIGHT: slot = &b->height; break;
    default:
        DFAIL("a DOMRect setter ran with a magic that is not one of the four variables §3 declares writable — "
              "the derived edges have no setter, and `inherit attribute` names exactly the four that do");
        return JS_UNDEFINED;
    }
    dr_set(ctx, b, slot, dr_value(ctx, JS_DupValue(ctx, val)));
    return JS_UNDEFINED;
}

/* THE EIGHT ATTRIBUTE IDENTIFIERS, IN §3'S DECLARATION ORDER, and the order is load-bearing twice over: it is
   the order the members are installed in, and it is the order Web IDL §3.7.7.1.1's default toJSON steps collect
   them in. One list, because they are one list — the getter's magic indexes it. */
static const char *const DR_MEMBER_NAMES[DR_MEMBER_COUNT] = {
    "x", "y", "width", "height", "top", "right", "bottom", "left"
};

/* Web IDL §3.7.7.1.1's DEFAULT toJSON STEPS, which `[Default] object toJSON()` selects: collect this
   interface's regular attributes whose type is a JSON type, in declaration order, base-first, and put each on a
   fresh ordinary object. All eight of DOMRectReadOnly's are `unrestricted double`, which is a JSON type, and
   the operation is declared on DOMRectReadOnly alone — so the inheritance stack is DOMRectReadOnly's and a
   DOMRect answers with those same eight rather than with the four it redeclares. */
static JSValue js_dr_tojson(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    DomRectBox *b = dr_this(ctx, this_val);
    JSValue out;
    int i;

    (void)argc; (void)argv; (void)magic;
    if (!b) return JS_EXCEPTION;
    out = JS_NewObject(ctx);
    CHECK(!JS_IsException(out), "a DOMRect's toJSON result could not be allocated");
    for (i = 0; i < DR_MEMBER_COUNT; i++)
        JS_SetPropertyStr(ctx, out, DR_MEMBER_NAMES[i], dr_member(ctx, b, (DomRectMember)i));
    return out;
}

/* ---- the declaration and the per-realm install ------------------------------------------------------------ */

void dom_rect_init(JSContext *ctx)
{
    JSClassDef ro = { "DOMRectReadOnly", dr_finalizer, dr_gc_mark };
    JSClassDef rc = { "DOMRect", dr_finalizer, dr_gc_mark };
    static const IdlArgType FOUR[4] = { IDL_UNRESTRICTED_DOUBLE, IDL_UNRESTRICTED_DOUBLE,
                                        IDL_UNRESTRICTED_DOUBLE, IDL_UNRESTRICTED_DOUBLE };
    static const IdlArgType ONE_DICT[1] = { IDL_DICT };
    int i;

    /* NOT `if (g_ro_class) return;`. This component has exactly ONE declaration site — core/platform.c's row —
       so the test could never be true, and it hid dom_rect_free leaving both class ids set. See
       core/agent_state.h. */
    DCHECK(g_ro_class == 0, "dom_rect_init ran twice — §3 and §4's classes are declared once per AGENT");
    JS_NewClassID(JS_GetRuntime(ctx), &g_ro_class);
    JS_NewClass(JS_GetRuntime(ctx), g_ro_class, &ro);
    JS_NewClassID(JS_GetRuntime(ctx), &g_rect_class);
    JS_NewClass(JS_GetRuntime(ctx), g_rect_class, &rc);

    /* All four constructor arguments are optional, which is what makes `new DOMRect()` a rectangle at the
       origin rather than Web IDL §3.6 step 5's TypeError. */
    g_id_ctor_ro = idl_method_id(ctx, FOUR, 4, js_dr_ctor, DR_READONLY);
    idl_optional_from(0);
    g_id_ctor_rect = idl_method_id(ctx, FOUR, 4, js_dr_ctor, DR_MUTABLE);
    idl_optional_from(0);
    g_id_from_ro = idl_method_id_dict(ctx, ONE_DICT, 1, DOM_RECT_INIT_DECL.members, DOM_RECT_INIT_DECL.n,
                                      js_dr_from_rect, DR_READONLY);
    idl_optional_from(0);
    g_id_from_rect = idl_method_id_dict(ctx, ONE_DICT, 1, DOM_RECT_INIT_DECL.members, DOM_RECT_INIT_DECL.n,
                                        js_dr_from_rect, DR_MUTABLE);
    idl_optional_from(0);
    g_id_tojson = idl_method_id(ctx, NULL, 0, js_dr_tojson, 0);
    for (i = DR_X; i <= DR_HEIGHT; i++)
        g_id_set[i] = idl_setter_id(ctx, IDL_UNRESTRICTED_DOUBLE, false, js_dr_set, i);

    agent_state_class("dom_rect", &g_ro_class, "§3's DOMRectReadOnly class, and the declaration latch");
    agent_state_class("dom_rect", &g_rect_class, "§4's DOMRect class");
    agent_state_id("dom_rect", &g_id_ctor_ro, "§3's constructor declaration");
    agent_state_id("dom_rect", &g_id_ctor_rect, "§4's constructor declaration");
    realm_declare_intrinsic(dom_rect_install_realm);
}

/* Web IDL §3.7.3 Interface prototype object's OBJECTS FOR GEOMETRY INTERFACES §3 The DOMRect interfaces, THEIR
   §3.7.1 INTERFACE OBJECTS, AND WEB IDL §3.8's PROPERTY REFERENCES FOR ALL THREE NAMES — FOR ONE REALM.

   THE INTERFACE OBJECTS ARE HERE BECAUSE WEB IDL §3.8 IS GIVEN A REALM. Web IDL §3.8 Platform objects
   implementing interfaces' `define the global property references` is "To define the global property
   references on target, given realm realm", and its step 1 is "Let interfaces be a list that contains every
   interface that is exposed in realm" — the population is a REALM's and the algorithm names no Document.
   Geometry Interfaces §3 declares both interfaces `[Exposed=(Window,Worker)]`, so a realm whose global object
   implements a worker scope owes both names; while they were placed from core/platform.c's per-DOCUMENT
   column, which such a realm never reaches, it got neither, and nor did a Window realm until a Document was
   installed over it. Both prototypes are in hand here, so the separate per-document entry's two
   JS_GetClassProto re-reads are gone: re-reading either would be a second answer to a question this function
   has just settled.

   THE TWO JS_SetClassProto HANDOVERS MOVED TO THE END. They were early, and `rp` was then built over an `rop`
   whose only owner was the class slot it had already been given to — correct, and a borrow-after-transfer that
   the two interface-object mints would have extended across twice as many lines. The locals own their objects
   until the realm does; this is the same repair core/xhr/xml_http_request.c made for §3's three prototypes. */
void dom_rect_install_realm(JSContext *ctx)
{
    JSValue rop, rp, prev, ro_ctor, ctor, global;
    int i;

    DCHECK(g_ro_class != 0, "a realm asked for DOMRectReadOnly.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_ro_class);
    DCHECK(JS_IsNull(prev), "dom_rect_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);

    rop = JS_NewObject(ctx);
    CHECK(!JS_IsException(rop), "DOMRectReadOnly.prototype could not be allocated");
    idl_interface_tag(ctx, rop, "DOMRectReadOnly");
    for (i = 0; i < DR_MEMBER_COUNT; i++)
        idl_install_accessor(ctx, rop, DR_MEMBER_NAMES[i], js_dr_get, i, -1);
    idl_install_method(ctx, rop, "toJSON", g_id_tojson);

    /* `interface DOMRect : DOMRectReadOnly` — the prototype chain IS the inheritance. */
    rp = JS_NewObjectProto(ctx, rop);
    CHECK(!JS_IsException(rp), "DOMRect.prototype could not be allocated");
    idl_interface_tag(ctx, rp, "DOMRect");
    /* Web IDL §3.7.6 defines EVERY regular attribute of the interface on its own prototype, `inherit attribute`
       included — so DOMRect.prototype carries its own four accessors, each with the getter its ancestor's
       attribute has and the setter this interface adds. The four derivations and toJSON are reached through the
       chain, which is what makes them one implementation rather than two. */
    for (i = DR_X; i <= DR_HEIGHT; i++)
        idl_install_accessor(ctx, rp, DR_MEMBER_NAMES[i], js_dr_get, i, g_id_set[i]);

    global = JS_GetGlobalObject(ctx);
    DCHECK(g_id_ctor_ro >= 0 && g_id_ctor_rect >= 0,
           "Geometry Interfaces §3's interface objects were built before dom_rect_init declared their "
           "constructors");
    ro_ctor = idl_step_constructor(ctx, "DOMRectReadOnly", g_id_ctor_ro);
    CHECK(!JS_IsException(ro_ctor), "the DOMRectReadOnly interface object could not be allocated");
    JS_SetConstructor(ctx, ro_ctor, rop);
    idl_install_method(ctx, ro_ctor, "fromRect", g_id_from_ro);

    ctor = idl_step_constructor(ctx, "DOMRect", g_id_ctor_rect);
    CHECK(!JS_IsException(ctor), "the DOMRect interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, rp);
    idl_install_method(ctx, ctor, "fromRect", g_id_from_rect);
    /* Web IDL §3.7.1 Interface object: the interface object of an interface that inherits has the inherited
       interface object as its [[Prototype]], which is what makes `DOMRect.fromRect` its own and
       `Object.getPrototypeOf(DOMRect)` the other constructor. */
    JS_SetPrototype(ctx, ctor, ro_ctor);

    /* WEB IDL §3.8's STEP 3.1.3 FOR EACH INTERFACE, THEN STEP 3.1.4 FOR THE ALIAS — in that order, because
       that is the order the algorithm performs them in and the alias's value is the object step 3.1.3 has
       already defined. Geometry Interfaces §3 declares `[Exposed=(Window,Worker), Serializable,
       LegacyWindowAlias=SVGRect]` on DOMRect, and Web IDL §3.8 step 3.1.4 reads "If the interface is declared
       with a [LegacyWindowAlias] extended attribute, and target implements the Window interface" — so the
       alias is a WINDOW realm's alone where the interface it aliases is owed to a worker's as well.

       `SVGRect` IS A NAME NO REALM CARRIED BEFORE THIS. Its IDL_GLOBAL_WINDOW row in
       browser/idl_exposure.h and its entry in browser/platform_names.h were both generated from the same
       declaration, and nothing in this tree ever placed it — so the alias half of §3.8 step 3.1 was unbuilt
       for this interface while its step 3.1.3 half ran on every document. It is placed here rather than left
       for a later diff because it is a step of the very algorithm this function now performs, over the very
       object it has just built, and leaving it out would be finishing one arm of one loop.

       THE DUP COMES FIRST BECAUSE THE DOOR CONSUMES. Both entries take ownership of the reference they are
       handed (core/idl_args.h says so of the door and the alias follows it), so the second reference is taken
       while `ctor` is still this function's, and the two names then share ONE object — which is what §3.8 step
       3.1.4.1.1 means by defining the alias over `interfaceObject`: `SVGRect === DOMRect` is true in a
       browser, `SVGRect.name` is "DOMRect", and `new SVGRect(...)` runs §3's constructor because it IS §3's
       constructor.

       NOTHING HERE DECIDES WHICH REALM GETS WHICH NAME. §3.8 step 3.1.4's Window condition is asked inside
       idl_define_legacy_window_alias and §3.3.7 [Exposed] step 1 inside the door, off browser/idl_exposure.h's
       generated rows — this component states three names and nothing about which realms they reach. */
    {
        JSValue alias = JS_DupValue(ctx, ctor);

        idl_define_global_property_reference(ctx, global, "DOMRectReadOnly", ro_ctor);
        idl_define_global_property_reference(ctx, global, "DOMRect", ctor);
        idl_define_legacy_window_alias(ctx, global, "SVGRect", alias);
    }
    JS_FreeValue(ctx, global);

    JS_SetClassProto(ctx, g_ro_class, rop);     /* the realm owns them from here */
    JS_SetClassProto(ctx, g_rect_class, rp);
}

void dom_rect_free(void)
{
    /* The prototypes are the REALMS' — released with their contexts — and the pool entries are the agent's.
       THE TWO CLASS IDS COME BACK TOO, and they are the reason this release is not just a tidy-up: the init
       above consulted g_ro_class to decide whether it had anything to do, so leaving it set made a second
       agent's DOMRect a pair of classes registered in a runtime that no longer exists.
       AND THE COLLECTOR RUNS AFTER THIS, which is why dr_finalizer and dr_gc_mark read neither of them: a
       rectangle a page still held is finalized in a collection this release has already happened before, so a
       class id is exactly the wrong way to reach its record. See core/agent_state.h. */
    g_ro_class = g_rect_class = 0;
    g_id_ctor_ro = g_id_ctor_rect = g_id_from_ro = g_id_from_rect = g_id_tojson = -1;
    g_id_set[0] = g_id_set[1] = g_id_set[2] = g_id_set[3] = -1;
}
