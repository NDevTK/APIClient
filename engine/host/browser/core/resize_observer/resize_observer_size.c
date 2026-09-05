/* RESIZE OBSERVER §2.3's `ResizeObserverSize`. See resize_observer_size.h for why this is its own component
 * and why its state is a JS Array rather than a C record. */
#include <stdbool.h>
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/resize_observer/resize_observer_size.h"
#include "solver/concolic.h"

/* §2.3's TWO MEMBERS, IN THE ORDER THE IDL DECLARES THEM — the enum IS the slot index and IS the accessor's
   magic, so a name installed with no case to answer it is the one way this can go wrong and the getter's own
   assert is exactly that. */
typedef enum {
    ROS_INLINE_SIZE = 0,
    ROS_BLOCK_SIZE,
    ROS_COUNT
} RoSizeMember;

static const char *const ROS_MEMBER_NAMES[ROS_COUNT] = { "inlineSize", "blockSize" };

static JSClassID g_class;
static JSValue   g_key = JS_UNDEFINED;      /* the size record's own state slot */
static JSAtom    g_atom = JS_ATOM_NULL;
static int       g_ready;

/* THE RECORD'S OWN STATE — one Array whose indices are RoSizeMember. OWNED. */
static JSValue ros_state(JSContext *ctx, JSValueConst obj)
{
    JSValue s;

    if (JS_GetClassID(obj) != g_class)
        return JS_ThrowTypeError(ctx, "not a ResizeObserverSize");
    if (JS_GetOwnSlot(ctx, &s, obj, g_atom) <= 0)
        s = JS_UNDEFINED;
    DCHECK(JS_IsObject(s),
           "a ResizeObserverSize carries no state — every one this engine mints has both members written "
           "before the object exists, so one without them was made somewhere else");
    return s;
}

static JSValue js_ros_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue state = ros_state(ctx, this_val), v;

    if (JS_IsException(state)) return JS_EXCEPTION;
    DCHECK(magic >= 0 && magic < ROS_COUNT,
           "a ResizeObserverSize member was read with a magic no member of §2.3 declares — the magic IS the "
           "member, so an unknown one means a name was installed without a slot to answer it");
    v = JS_GetPropertyUint32(ctx, state, (uint32_t)magic);
    JS_FreeValue(ctx, state);
    return v;
}

bool resize_observer_size_is(JSValueConst v)
{
    return g_class != 0 && JS_GetClassID(v) == g_class;
}

JSValue resize_observer_size_new(JSContext *ctx, JSValue inline_size, JSValue block_size)
{
    JSValue obj, state, proto;

    DCHECK(g_ready, "a ResizeObserverSize was constructed before its interface was declared");
    /* WHAT EACH OF §2.3'S TWO MEMBERS MAY BE, ASSERTED WHERE THE RECORD IS BUILT. There is exactly ONE
       producer — §3.4.8, which reads a used value out of core/layout — and the two shapes it can hand over are
       a Number and unknown external input crossed whole (a viewport-derived length carries a domain that must
       keep forking control flow at a page's `if (size.inlineSize > 600)`; a de-tainting coercion here is the
       placeholder core/geometry/dom_rect.c refused at the same kind of record). Anything else is a producer
       this component does not have. */
    DCHECK(JS_IsNumber(inline_size) || concolic_is(inline_size),
           "§2.3's `readonly attribute unrestricted double inlineSize` was given something that is neither a "
           "Number nor unknown external input — §3.4.8 sets it from a used length, and unknown input is the "
           "one thing this boundary passes through as itself");
    DCHECK(JS_IsNumber(block_size) || concolic_is(block_size),
           "§2.3's `readonly attribute unrestricted double blockSize` was given something that is neither a "
           "Number nor unknown external input");
    proto = JS_GetClassProto(ctx, g_class);
    DCHECK(!JS_IsNull(proto),
           "a ResizeObserverSize was constructed in a realm that never ran its prototype install");
    obj = JS_NewObjectProtoClass(ctx, proto, g_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "a ResizeObserverSize could not be allocated");
    state = JS_NewArray(ctx);
    CHECK(!JS_IsException(state), "a ResizeObserverSize's state could not be allocated");
    /* BOTH MEMBERS ARE PLACED BEFORE THE OBJECT IS HANDED BACK — the two are the record, and one missing is
       what `ros_state`'s reader would then have to default past. */
    JS_SetPropertyUint32(ctx, state, ROS_INLINE_SIZE, inline_size);
    JS_SetPropertyUint32(ctx, state, ROS_BLOCK_SIZE, block_size);
    JS_DefinePropertyValue(ctx, obj, g_atom, state, 0);
    return obj;
}

/* ---- declaration and installation -------------------------------------------------------------------------- */

void resize_observer_size_init(JSContext *ctx)
{
    JSClassDef d = { "ResizeObserverSize" };

    if (g_ready) return;   /* one AGENT, one class */
    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    JS_NewClass(JS_GetRuntime(ctx), g_class, &d);

    g_key = JS_NewSymbol(ctx, "resizeObserverSizeState", false);
    CHECK(!JS_IsException(g_key), "the ResizeObserverSize state slot key allocation failed");
    g_atom = JS_ValueToAtom(ctx, g_key);
    CHECK(g_atom != JS_ATOM_NULL, "the ResizeObserverSize state slot key could not be interned");

    realm_declare_intrinsic(resize_observer_size_install_proto);
    g_ready = 1;

    /* DECLARED UNDER THE COMPONENT THAT OWNS THE PLATFORM ROW — core/agent_state.h. §2.3's two interfaces are
       not two platform rows: resize_observer.c's declare calls this init, its install calls this install, and
       its release calls this release, so these slots are undone on that row's column and are asserted against
       it. */
    agent_state_flag("resize_observer", &g_ready, "§2.3's ResizeObserverSize declaration latch");
    agent_state_class("resize_observer", &g_class, "§2.3's ResizeObserverSize class");
    agent_state_value("resize_observer", &g_key, "the size record's state-slot key");
    agent_state_atom("resize_observer", &g_atom, "the size record's state-slot key, interned");
}

void resize_observer_size_install_proto(JSContext *ctx)
{
    JSValue proto, prev;
    int i;

    DCHECK(g_class != 0, "a realm asked for ResizeObserverSize.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_class);
    DCHECK(JS_IsNull(prev), "resize_observer_size_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "ResizeObserverSize.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "ResizeObserverSize");
    for (i = 0; i < ROS_COUNT; i++)
        idl_install_accessor(ctx, proto, ROS_MEMBER_NAMES[i], js_ros_get, i, -1);
    JS_SetClassProto(ctx, g_class, proto);
}

/* §2.3 DECLARES NO CONSTRUCTOR FOR THIS INTERFACE, so the interface object is Web IDL §3.7.1 Interface
   object's non-constructible form — a page that writes `new ResizeObserverSize()` gets the TypeError every
   browser throws, and `ResizeObserverSize` is still a name the page can find and compare a prototype against.
   `idl_interface_object` is that form; a constructor here would be a member the IDL does not declare. */
void resize_observer_size_install(JSContext *ctx, JSValueConst global)
{
    JSValue proto = JS_GetClassProto(ctx, g_class);

    DCHECK(!JS_IsNull(proto),
           "ResizeObserverSize was installed in a realm that never ran its prototype install");
    idl_define_global_property_reference(ctx, global, "ResizeObserverSize",
                                        idl_interface_object(ctx, "ResizeObserverSize", proto));
    JS_FreeValue(ctx, proto);
}

/* THIS CLASS HAS NO FINALIZER AND NO gc_mark, which is why zeroing its id below reaches nothing: the
 * `JSClassDef` above is `{ "ResizeObserverSize" }` and every other field is a zero, so
 * `rt->class_array[id].finalizer` and `.gc_mark` are NULL and the collector has nothing of this component's to
 * dispatch to. Nothing is held where the collector cannot see it either — the two members are one Array in an
 * OWN PROPERTY, and the property walk in mark_children and free_object is unconditional. */
void resize_observer_size_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;` — this is reached from a release column whose declare pass is
       unconditional, so a release in an agent that never declared is the thing to CRASH on. */
    DCHECK(g_ready, "§2.3's ResizeObserverSize was released in an agent that never declared it");
    JS_FreeValueRT(rt, g_key);
    g_key = JS_UNDEFINED;
    JS_FreeAtomRT(rt, g_atom);
    g_atom = JS_ATOM_NULL;
    g_class = 0;
    g_ready = 0;
}
