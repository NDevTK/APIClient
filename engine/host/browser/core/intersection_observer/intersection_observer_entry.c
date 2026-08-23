/* INTERSECTION OBSERVER §2.3 "The IntersectionObserverEntry interface". See intersection_observer_entry.h for
 * why this is its own component and why its state is a JS Array rather than a C record. */
#include <stdbool.h>
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/intersection_observer/intersection_observer_entry.h"

/* §2.3's EIGHT MEMBERS, IN THE ORDER THE IDL DECLARES THEM — the enum IS the slot index and IS the accessor's
   magic, so a name installed with no case to answer it is the one way this can go wrong and the getter's
   default asserts exactly that. */
typedef enum {
    IOE_TIME = 0,
    IOE_ROOT_BOUNDS,
    IOE_BOUNDING_CLIENT_RECT,
    IOE_INTERSECTION_RECT,
    IOE_IS_INTERSECTING,
    IOE_IS_VISIBLE,
    IOE_INTERSECTION_RATIO,
    IOE_TARGET,
    IOE_COUNT
} IoEntryMember;

static const char *const IOE_MEMBER_NAMES[IOE_COUNT] = {
    "time", "rootBounds", "boundingClientRect", "intersectionRect",
    "isIntersecting", "isVisible", "intersectionRatio", "target"
};

static JSClassID g_class;
static JSValue   g_key = JS_UNDEFINED;      /* the entry's own state slot */
static JSAtom    g_atom = JS_ATOM_NULL;
static int       g_id_ctor = -1;
static int       g_ready;

/* THE ENTRY'S OWN STATE — one Array whose indices are IoEntryMember. OWNED. */
static JSValue ioe_state(JSContext *ctx, JSValueConst obj)
{
    JSValue s;

    if (JS_GetClassID(obj) != g_class)
        return JS_ThrowTypeError(ctx, "not an IntersectionObserverEntry");
    if (JS_GetOwnSlot(ctx, &s, obj, g_atom) <= 0)
        s = JS_UNDEFINED;
    DCHECK(JS_IsObject(s),
           "an IntersectionObserverEntry carries no state — every one this engine mints has all eight members "
           "written before the object exists, so one without them was made somewhere else");
    return s;
}

static JSValue js_ioe_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue state = ioe_state(ctx, this_val), v;

    if (JS_IsException(state)) return JS_EXCEPTION;
    DCHECK(magic >= 0 && magic < IOE_COUNT,
           "an IntersectionObserverEntry member was read with a magic no member of §2.3 declares — the magic IS "
           "the member, so an unknown one means a name was installed without a slot to answer it");
    v = JS_GetPropertyUint32(ctx, state, (uint32_t)magic);
    JS_FreeValue(ctx, state);
    return v;
}

JSValue intersection_observer_entry_new(JSContext *ctx, double time, JSValue root_bounds, JSValue bounding,
                                        JSValue intersection, bool is_intersecting, bool is_visible,
                                        JSValue ratio, JSValueConst target)
{
    JSValue obj, state, proto;

    DCHECK(g_ready, "an IntersectionObserverEntry was constructed before its interface was declared");
    proto = JS_GetClassProto(ctx, g_class);
    DCHECK(!JS_IsNull(proto),
           "an IntersectionObserverEntry was constructed in a realm that never ran its prototype install");
    obj = JS_NewObjectProtoClass(ctx, proto, g_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "an IntersectionObserverEntry could not be allocated");
    state = JS_NewArray(ctx);
    CHECK(!JS_IsException(state), "an IntersectionObserverEntry's state could not be allocated");
    /* EVERY MEMBER IS PLACED BEFORE THE OBJECT IS HANDED BACK — the eight are the record, and an entry missing
       one is what `ioe_state`'s reader would then have to default past. */
    JS_SetPropertyUint32(ctx, state, IOE_TIME, JS_NewFloat64(ctx, time));
    JS_SetPropertyUint32(ctx, state, IOE_ROOT_BOUNDS, root_bounds);
    JS_SetPropertyUint32(ctx, state, IOE_BOUNDING_CLIENT_RECT, bounding);
    JS_SetPropertyUint32(ctx, state, IOE_INTERSECTION_RECT, intersection);
    JS_SetPropertyUint32(ctx, state, IOE_IS_INTERSECTING, JS_NewBool(ctx, is_intersecting));
    JS_SetPropertyUint32(ctx, state, IOE_IS_VISIBLE, JS_NewBool(ctx, is_visible));
    JS_SetPropertyUint32(ctx, state, IOE_INTERSECTION_RATIO, ratio);
    JS_SetPropertyUint32(ctx, state, IOE_TARGET, JS_DupValue(ctx, target));
    JS_DefinePropertyValue(ctx, obj, g_atom, state, 0);
    return obj;
}

/* ---- §2.3's CONSTRUCTOR ----------------------------------------------------------------------------------- */

typedef struct { uint8_t unused; } JSIoeCtorState;
static void js_ioe_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

#define IOE_CTOR_STAGES(X) \
    X(IOE_CTOR_BUILD = IDL_STEP_FIRST, \
      "INTERSECTION OBSERVER §2.3 new IntersectionObserverEntry(intersectionObserverEntryInit)")
enum { IOE_CTOR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const IOE_CTOR_STEPS[] = { IOE_CTOR_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* §2.3's `constructor(IntersectionObserverEntryInit intersectionObserverEntryInit)`.
 *
 * THE ARGUMENT'S TYPE IS NOT DECLARED, AND THAT IS THE GAP THIS CRASHES FOR. `IntersectionObserverEntryInit`
 * has three members whose own type is a DICTIONARY — `required DOMRectInit? rootBounds`, `required DOMRectInit
 * boundingClientRect`, `required DOMRectInit intersectionRect` — and core/idl_args.c refuses one by name ("a
 * dictionary member was declared as a dictionary — the conversion cursor is per-argument, so a nested one would
 * read the outer's names"). Declaring the argument IDL_DICT with those members therefore aborts in the
 * conversion, one layer away from the thing that is missing, so the type is left undeclared and the crash is
 * raised HERE, where it can say what to build.
 *
 * NOTHING IN THIS ENGINE REACHES IT. §3.2.6 step 1's "construct an IntersectionObserverEntry" is the internal
 * operation `intersection_observer_entry_new` above, which takes the eight values it already holds; this is the
 * page-visible constructor and only a page calls it. */
static int js_ioe_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    (void)st; (void)argc; (void)argv; (void)presult; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(hdr->stage == IOE_CTOR_BUILD,
           "the IntersectionObserverEntry constructor resumed at a stage §2.3 does not have");
    if (JS_IsUndefined(hdr->this_val))
        return JS_ThrowTypeError(ctx, "constructor IntersectionObserverEntry requires 'new'"), -1;
    DFAIL("INTERSECTION OBSERVER §2.3's `constructor(IntersectionObserverEntryInit)` takes a dictionary THREE "
          "OF WHOSE MEMBERS ARE THEMSELVES DICTIONARIES — `required DOMRectInit? rootBounds`, `required "
          "DOMRectInit boundingClientRect` and `required DOMRectInit intersectionRect`, each of which Web IDL "
          "§3.2.17 converts by reading ITS four members with the page's own getters. core/idl_args.c states "
          "the missing capability at its own site: `DCHECK(mt != IDL_DICT, \"a dictionary member was declared "
          "as a dictionary — the conversion cursor is per-argument, so a nested one would read the outer's "
          "names\")`. BUILD the nested conversion there — the STACK OF CURSORS already exists for "
          "IDL_SEQUENCE_STRING_OR_DICT (idl_conv_push / idl_conv_run push an IdlDictDecl and run it to a "
          "value), so what is missing is a member TYPE that pushes one for a plain dictionary member plus the "
          "`IdlDictDecl` for Geometry Interfaces §3's DOMRectInit, which core/geometry/dom_rect.c already "
          "declares as a member list (DOM_RECT_INIT) and would then have to NAME. Then declare this "
          "constructor's argument IDL_DICT over IntersectionObserverEntryInit's eight members and write its "
          "one step: read them and build the entry through intersection_observer_entry_new");
    /* A RELEASE BUILD CANNOT BUILD THE CONVERSION, so the constructor refuses — which is the honest answer for
       a capability that is not supportable outside dev, and is distinguishable by a page from a missing
       interface object (the interface IS there, and every instance the engine mints answers all eight
       members). */
    return JS_ThrowTypeError(ctx, "IntersectionObserverEntry cannot be constructed by script in this engine"),
           -1;
}

static const IdlStepDecl js_ioe_ctor_decl = {
    js_ioe_ctor_step, sizeof(JSIoeCtorState), js_ioe_ctor_visit, NULL,
    "INTERSECTION OBSERVER §2.3 new IntersectionObserverEntry(intersectionObserverEntryInit)", IOE_CTOR_STEPS
};

/* ---- declaration and installation -------------------------------------------------------------------------- */

void intersection_observer_entry_init(JSContext *ctx)
{
    JSClassDef d = { "IntersectionObserverEntry" };

    if (g_ready) return;   /* one AGENT, one class */
    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    JS_NewClass(JS_GetRuntime(ctx), g_class, &d);

    g_key = JS_NewSymbol(ctx, "intersectionObserverEntryState", false);
    CHECK(!JS_IsException(g_key), "the IntersectionObserverEntry state slot key allocation failed");
    g_atom = JS_ValueToAtom(ctx, g_key);
    CHECK(g_atom != JS_ATOM_NULL, "the IntersectionObserverEntry state slot key could not be interned");

    /* No declared argument type — see js_ioe_ctor_step for the capability that decides that and for the crash
       that names it. The position crosses unconverted and the body never reads it. */
    g_id_ctor = idl_method_id_step(ctx, NULL, 0, NULL, 0, &js_ioe_ctor_decl, 0);

    realm_declare_intrinsic(intersection_observer_entry_install_proto);
    g_ready = 1;

    /* DECLARED UNDER THE COMPONENT THAT OWNS THE PLATFORM ROW — core/agent_state.h. §2.3 is a second interface
       and not a second row: intersection_observer.c's declare calls this init, its install calls this install,
       and its release calls this release, so these slots are undone on that row's column and are asserted
       against it. The name here is what core/platform.c's witness table already says by mapping
       `IntersectionObserverEntry` to `intersection_observer`. */
    agent_state_flag("intersection_observer", &g_ready, "§2.3's declaration latch");
    agent_state_class("intersection_observer", &g_class, "§2.3's IntersectionObserverEntry class");
    agent_state_value("intersection_observer", &g_key, "the entry's state-slot key");
    agent_state_atom("intersection_observer", &g_atom, "the entry's state-slot key, interned");
    agent_state_id("intersection_observer", &g_id_ctor, "§2.3's constructor declaration");
}

void intersection_observer_entry_install_proto(JSContext *ctx)
{
    JSValue proto, prev;
    int i;

    DCHECK(g_class != 0,
           "a realm asked for IntersectionObserverEntry.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_class);
    DCHECK(JS_IsNull(prev), "intersection_observer_entry_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "IntersectionObserverEntry.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "IntersectionObserverEntry");
    for (i = 0; i < IOE_COUNT; i++)
        idl_install_accessor(ctx, proto, IOE_MEMBER_NAMES[i], js_ioe_get, i, -1);
    JS_SetClassProto(ctx, g_class, proto);
}

void intersection_observer_entry_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor, proto;

    DCHECK(g_id_ctor >= 0,
           "IntersectionObserverEntry was installed before its constructor was declared");
    ctor = idl_step_constructor(ctx, "IntersectionObserverEntry", 1, g_id_ctor);
    CHECK(!JS_IsException(ctor), "the IntersectionObserverEntry interface object could not be allocated");
    proto = JS_GetClassProto(ctx, g_class);
    DCHECK(!JS_IsNull(proto),
           "IntersectionObserverEntry was installed in a realm that never ran its prototype install");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "IntersectionObserverEntry", ctor);
}

/* THIS CLASS HAS NO FINALIZER AND NO gc_mark EITHER, so zeroing its id below reaches nothing — see the note on
   intersection_observer_free, which states the reasoning for both classes. An entry's eight members are one
   Array in an OWN PROPERTY, marked and freed by the unconditional property walk. */
void intersection_observer_entry_free(JSRuntime *rt)
{
    /* Unconditional, for its caller's reason: it is reached from the release column, whose declare pass is
       unconditional. */
    DCHECK(g_ready, "§2.3's IntersectionObserverEntry was released in an agent that never declared it");
    JS_FreeValueRT(rt, g_key);
    g_key = JS_UNDEFINED;
    JS_FreeAtomRT(rt, g_atom);
    g_atom = JS_ATOM_NULL;
    g_id_ctor = -1;
    g_class = 0;
    g_ready = 0;
}
