/* INTERSECTION OBSERVER §2.3 "The IntersectionObserverEntry interface". See intersection_observer_entry.h for
 * why this is its own component and why its state is a JS Array rather than a C record. */
#include <stdbool.h>
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/geometry/dom_rect.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/intersection_observer/intersection_observer_entry.h"
#include "solver/concolic.h"

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

JSValue intersection_observer_entry_new(JSContext *ctx, JSValue time, JSValue root_bounds, JSValue bounding,
                                        JSValue intersection, JSValue is_intersecting, JSValue is_visible,
                                        JSValue ratio, JSValueConst target)
{
    JSValue obj, state, proto;

    DCHECK(g_ready, "an IntersectionObserverEntry was constructed before its interface was declared");
    /* WHAT EACH OF §2.3'S EIGHT MEMBERS MAY BE, ASSERTED WHERE THE ENTRY IS BUILT. There are exactly two
       producers — §3.2.10's walk, which computes every value, and §2.3's constructor, which takes them through
       Web IDL — and they agree on these shapes. Unknown external input is admitted at the four positions a
       page's initialiser can put one at (a member of a crossing type reaches a body as ITSELF) and at NO
       other: the three rectangles are minted by this component's own constructor body, so an unknown never
       stands where one belongs, and admitting it here would be a shape no producer produces. */
    DCHECK(JS_IsNumber(time) || concolic_is(time),
           "§2.3's `readonly attribute DOMHighResTimeStamp time` was given something that is neither a Number "
           "nor unknown external input — HR-TIME §5 The DOMHighResTimeStamp typedef writes `typedef double "
           "DOMHighResTimeStamp`, whose conversion produces a Number, and unknown input is the one thing the "
           "IDL boundary passes through as itself");
    DCHECK(JS_IsNumber(ratio) || concolic_is(ratio),
           "§2.3's `readonly attribute double intersectionRatio` was given something that is neither a Number "
           "nor unknown external input");
    DCHECK((JS_IsBool(is_intersecting) || concolic_is(is_intersecting)) &&
           (JS_IsBool(is_visible) || concolic_is(is_visible)),
           "§2.3's `isIntersecting` / `isVisible` were given something that is neither a Boolean nor unknown "
           "external input — both are `readonly attribute boolean`, and a page reads the value back");
    DCHECK(JS_IsNull(root_bounds) || dom_rect_is(root_bounds),
           "§2.3's `readonly attribute DOMRectReadOnly? rootBounds` was given something that is neither a "
           "rectangle nor the IDL null — the `?` is what admits null and it admits nothing else");
    DCHECK(dom_rect_is(bounding) && dom_rect_is(intersection),
           "§2.3's `boundingClientRect` / `intersectionRect` were given something that is not a rectangle — "
           "both are `readonly attribute DOMRectReadOnly`, with no `?` to admit a null");
    DCHECK(element_is(target) || concolic_is(target),
           "§2.3's `readonly attribute Element target` was given something that is not an Element — §3.2.15's "
           "brand and the declaration's narrowing are what refuse a Text node at the constructor, and "
           "§3.2.10's walk hands this component the element it observed");
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
    JS_SetPropertyUint32(ctx, state, IOE_TIME, time);
    JS_SetPropertyUint32(ctx, state, IOE_ROOT_BOUNDS, root_bounds);
    JS_SetPropertyUint32(ctx, state, IOE_BOUNDING_CLIENT_RECT, bounding);
    JS_SetPropertyUint32(ctx, state, IOE_INTERSECTION_RECT, intersection);
    JS_SetPropertyUint32(ctx, state, IOE_IS_INTERSECTING, is_intersecting);
    JS_SetPropertyUint32(ctx, state, IOE_IS_VISIBLE, is_visible);
    JS_SetPropertyUint32(ctx, state, IOE_INTERSECTION_RATIO, ratio);
    JS_SetPropertyUint32(ctx, state, IOE_TARGET, JS_DupValue(ctx, target));
    JS_DefinePropertyValue(ctx, obj, g_atom, state, 0);
    return obj;
}

/* ---- §2.3's CONSTRUCTOR ----------------------------------------------------------------------------------- */

/* §2.3's `dictionary IntersectionObserverEntryInit`, in Web IDL §3.2.17 Dictionary types' LEXICOGRAPHIC read
   order — the order a page with getters on the initialiser observes, and the order a throw from one of them
   pins. ALL EIGHT ARE `required`, which is part of the type and not a note: §3.2.17 (ES-to-IDL list) step
   4.1.6 is "Otherwise, if jsMemberValue is undefined and member is required, then throw a TypeError", so `{}`
   and `{boundingClientRect: undefined}` are refused at the member the loop is standing on, and nothing in this
   file defaults anything.

   THE THREE DICTIONARY-TYPED MEMBERS NAME GEOMETRY INTERFACES §3 The DOMRect interfaces' DECLARATION and are
   the reason it has one — see core/geometry/dom_rect.h.
   `rootBounds` is IDL_DICT_NULLABLE where the other two are IDL_DICT, and that one
   `?` is the whole of a difference a page can see. §3.2.20 Nullable types — T? step 3 is "Otherwise, if V is
   null or undefined, then return the IDL nullable type T? value null", so `{rootBounds: null}` is the IDL
   null and `entry.rootBounds` answers null — which is what §2.3's `readonly attribute DOMRectReadOnly?
   rootBounds` declares. §3.2.17's own step 1 is "If jsDict is not an Object and jsDict is neither undefined
   nor null, then throw a TypeError", so the UN-nullable type ADMITS a null and step 4.1.2 then makes every
   DOMRectInit member undefined: `{boundingClientRect: null}` is a rectangle carrying Geometry §3's four
   `= 0`. One keyword, a null and a rectangle at the origin.

   `time` and `intersectionRatio` are IDL_DOUBLE, the RESTRICTED type — HR-TIME §5 The DOMHighResTimeStamp
   typedef writes "typedef double DOMHighResTimeStamp;", so both are §3.2.7 double and `{time: NaN}` and
   `{time: Infinity}` are that type's TypeError rather than moments an entry can carry.

   `isIntersecting` and `isVisible` are IDL_BOOLEAN_NO_DEFAULT because §2.3's IDL writes no `= false`. The
   absence that type exists to keep visible is unreachable on a `required` member — step 4.1.6 answers an
   absent one first — and the type is still the one the IDL states, which is what stops a later edition adding
   a default from silently keeping this engine's old answer.

   `target` is IDL_INTERFACE against Element. This engine's class system reaches every node through ONE class,
   so `idl_iface_brand(node_class_id())` says "a Node" and `idl_iface_narrow(element_is)` is what says which
   kind — both stated at the declaration below, and both needed: `{target: document.createTextNode('x')}` is a
   TypeError. */
static const IdlDictMember IOE_INIT[] = {
    { "boundingClientRect", IDL_DICT,               true, NULL, 0, &DOM_RECT_INIT_DECL },
    { "intersectionRatio",  IDL_DOUBLE,             true },
    { "intersectionRect",   IDL_DICT,               true, NULL, 0, &DOM_RECT_INIT_DECL },
    { "isIntersecting",     IDL_BOOLEAN_NO_DEFAULT, true },
    { "isVisible",          IDL_BOOLEAN_NO_DEFAULT, true },
    { "rootBounds",         IDL_DICT_NULLABLE,      true, NULL, 0, &DOM_RECT_INIT_DECL },
    { "target",             IDL_INTERFACE,          true },
    { "time",               IDL_DOUBLE,             true },
};

/* ONE OF §2.3'S THREE `DOMRectInit` MEMBERS, AS THE `DOMRectReadOnly` THE INTERFACE ANSWERS.
 *
 * §3.2.17 HAS ALREADY RUN OVER IT: what arrives is the engine-built dictionary the nested level placed, so
 * these four reads reach nothing of the page's and no getter of the page's can run from here.
 *
 * GEOMETRY §3's `= 0` IS NOT APPLIED HERE AND MUST NOT BE. `DOMRectInit` declares each member
 * `unrestricted double = 0`, and core/geometry/dom_rect.c applies that default in exactly ONE place —
 * `dr_value`, which the mint below goes through — because §3's positional constructor carries the same `= 0`
 * and an omitted argument reaches it by the other road. So an absent member is `undefined` here, and that is
 * the PRODUCER'S POSITIVE STATEMENT that the page wrote nothing rather than a hole to fill; a `? 0` written at
 * this site would be the second answer to §3 that dom_rect.c's own note refuses.
 *
 * THE MEMBER MAY ALSO BE UNKNOWN EXTERNAL INPUT CROSSED WHOLE — core/idl_args.c's member loop crosses an
 * unknown as itself whatever the declared type says — and the same four reads are the right answer for that
 * too: reading `x` off an unknown yields another unknown (solver/concolic.c's exotic get), which is what
 * §3.2.17 would have produced one level down, so the rectangle carries four values that still fork control
 * flow and are still solvable at a sink instead of four zeroes that de-taint it.
 *
 * `DOMRectReadOnly` AND NOT `DOMRect`, because §2.3 declares all three rectangles `readonly attribute
 * DOMRectReadOnly`: a page holding an entry must not be able to write the geometry back, and
 * `entry.boundingClientRect instanceof DOMRect` answers false in every browser.
 * CONSUMES `init_rect`. */
static JSValue ioe_rect(JSContext *ctx, JSValue init_rect)
{
    JSValue rect;

    DCHECK(JS_IsObject(init_rect),
           "a §2.3 DOMRectInit member reached the constructor's body as something that is not an object — "
           "§3.2.17 step 1 admits an Object or a null and refuses everything else, `required` refuses the "
           "absent one, and this file's caller answers the nullable member's null before calling here");
    rect = dom_rect_readonly_new_values(ctx,
                                        idl_dict_get(ctx, init_rect, "x"),
                                        idl_dict_get(ctx, init_rect, "y"),
                                        idl_dict_get(ctx, init_rect, "width"),
                                        idl_dict_get(ctx, init_rect, "height"));
    JS_FreeValue(ctx, init_rect);
    return rect;
}

/* §2.3's `constructor(IntersectionObserverEntryInit intersectionObserverEntryInit)`.
 *
 * THE ARGUMENT IS DECLARED AND REQUIRED, so everything that can refuse this call has already run: Web IDL
 * §3.6 Overload resolution algorithm's step 5 threw for `new IntersectionObserverEntry()`, and §3.2.17 read,
 * converted or refused all eight members — three of them by descending into Geometry §3's own dictionary.
 * What is left is this: eight values placed on an entry. Nothing here throws and nothing here reaches the
 * page's code, which is why it is a plain body and not a step machine.
 *
 * NOTHING IN THIS ENGINE REACHES IT. §3.2.6 step 1's "construct an IntersectionObserverEntry" is the internal
 * operation `intersection_observer_entry_new` above, which takes the eight values it already holds; this is
 * the page-visible constructor and only a page calls it. */
static JSValue js_ioe_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init;
    JSValue root, target, entry;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor IntersectionObserverEntry requires 'new'");
    DCHECK(argc == 1,
           "§2.3 declares ONE required argument and the member is not variadic, so the argument machine hands "
           "this body exactly that one position — fewer is Web IDL §3.6 step 5's TypeError, thrown before any "
           "conversion runs, and more is a position the declaration does not list");
    init = argv[0];
    /* §3.2.20 Nullable types — T? step 3 put the IDL null here for `{rootBounds: null}`, and §2.3's `readonly
       attribute DOMRectReadOnly? rootBounds` is what makes that the entry's own value rather than a rectangle.
       Anything else is a DOMRectInit — INCLUDING the one §3.2.17 step 1 admits a null into and step 4.1.2 then
       leaves entirely absent, which is why this member is the only one that asks. */
    root = idl_dict_get(ctx, init, "rootBounds");
    if (!JS_IsNull(root)) root = ioe_rect(ctx, root);
    /* `target` is the ONE value the mint borrows rather than consumes, so this body owns the read and gives it
       back — see intersection_observer_entry.h. */
    target = idl_dict_get(ctx, init, "target");
    entry = intersection_observer_entry_new(ctx,
                                            idl_dict_get(ctx, init, "time"),
                                            root,
                                            ioe_rect(ctx, idl_dict_get(ctx, init, "boundingClientRect")),
                                            ioe_rect(ctx, idl_dict_get(ctx, init, "intersectionRect")),
                                            idl_dict_get(ctx, init, "isIntersecting"),
                                            idl_dict_get(ctx, init, "isVisible"),
                                            idl_dict_get(ctx, init, "intersectionRatio"),
                                            target);
    JS_FreeValue(ctx, target);
    return entry;
}

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

    /* ONE DECLARED POSITION, TYPED AS THE DICTIONARY §2.3 WRITES THERE. The position is REQUIRED — no
       idl_optional_from — which is what makes `new IntersectionObserverEntry()` Web IDL §3.6 step 5's
       TypeError before any of the constructor's own work runs, and what makes Web IDL §3.7.1 Interface
       object's `length` ("the length of the shortest argument list of the entries in S", over the effective
       overload set at argument count 0) the 1 the install derives from this line rather than states by hand.
       IT WAS IDL_ANY, WHICH DECLARED THAT THE POSITION EXISTED AND NOTHING ABOUT WHAT IT ACCEPTS: the value
       crossed unconverted, the constructor could not be written, and the crash that stood here named the two
       declarations that were missing — Geometry §3's for `DOMRectInit` (now DOM_RECT_INIT_DECL) and this
       file's own eight-member list. Both are here, so the crash is gone with them.
       §3.2.15's BRAND AND ITS NARROWING ARE THE DECLARATION'S, stated once for a dictionary whose only
       interface-typed member is `required Element target`: the class reaches every node and the narrowing is
       what says Element. */
    {
        static const IdlArgType IOE_CTOR_ARGS[1] = { IDL_DICT };

        g_id_ctor = idl_method_id_dict(ctx, IOE_CTOR_ARGS, 1, IOE_INIT, (int)COUNTOF(IOE_INIT),
                                       js_ioe_ctor, 0);
        idl_iface_brand(node_class_id());
        idl_iface_narrow(element_is);
    }

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
    ctor = idl_step_constructor(ctx, "IntersectionObserverEntry", g_id_ctor);
    CHECK(!JS_IsException(ctor), "the IntersectionObserverEntry interface object could not be allocated");
    proto = JS_GetClassProto(ctx, g_class);
    DCHECK(!JS_IsNull(proto),
           "IntersectionObserverEntry was installed in a realm that never ran its prototype install");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    idl_define_global_property_reference(ctx, global, "IntersectionObserverEntry", ctor);
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
