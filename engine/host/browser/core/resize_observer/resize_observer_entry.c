/* RESIZE OBSERVER §2.3's `ResizeObserverEntry`. See resize_observer_entry.h for why this is its own component
 * and why its state is a JS Array rather than a C record. */
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
#include "core/resize_observer/resize_observer_entry.h"
#include "core/resize_observer/resize_observer_size.h"
#include "solver/concolic.h"

/* §2.3's FIVE MEMBERS, IN THE ORDER THE IDL DECLARES THEM — the enum IS the slot index and IS the accessor's
   magic, so a name installed with no case to answer it is the one way this can go wrong and the getter's own
   assert is exactly that. */
typedef enum {
    ROE_TARGET = 0,
    ROE_CONTENT_RECT,
    ROE_BORDER_BOX_SIZE,
    ROE_CONTENT_BOX_SIZE,
    ROE_DEVICE_PIXEL_CONTENT_BOX_SIZE,
    ROE_COUNT
} RoEntryMember;

static const char *const ROE_MEMBER_NAMES[ROE_COUNT] = {
    "target", "contentRect", "borderBoxSize", "contentBoxSize", "devicePixelContentBoxSize"
};

static JSClassID g_class;
static JSValue   g_key = JS_UNDEFINED;      /* the entry's own state slot */
static JSAtom    g_atom = JS_ATOM_NULL;
static int       g_ready;

/* THE ENTRY'S OWN STATE — one Array whose indices are RoEntryMember. OWNED. */
static JSValue roe_state(JSContext *ctx, JSValueConst obj)
{
    JSValue s;

    if (JS_GetClassID(obj) != g_class)
        return JS_ThrowTypeError(ctx, "not a ResizeObserverEntry");
    if (JS_GetOwnSlot(ctx, &s, obj, g_atom) <= 0)
        s = JS_UNDEFINED;
    DCHECK(JS_IsObject(s),
           "a ResizeObserverEntry carries no state — every one this engine mints has all five members written "
           "before the object exists, so one without them was made somewhere else");
    return s;
}

static JSValue js_roe_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue state = roe_state(ctx, this_val), v;

    if (JS_IsException(state)) return JS_EXCEPTION;
    DCHECK(magic >= 0 && magic < ROE_COUNT,
           "a ResizeObserverEntry member was read with a magic no member of §2.3 declares — the magic IS the "
           "member, so an unknown one means a name was installed without a slot to answer it");
    v = JS_GetPropertyUint32(ctx, state, (uint32_t)magic);
    JS_FreeValue(ctx, state);
    return v;
}

/* ONE `FrozenArray<ResizeObserverSize>` — Web IDL §3.2.27 Frozen arrays over the single size this level of the
   standard produces. CONSUMES `size`; the array comes back frozen, which is the interface's own shape and not
   the computation's (see the header). */
static JSValue roe_frozen_size(JSContext *ctx, JSValue size)
{
    JSValue arr = JS_NewArray(ctx);

    CHECK(!JS_IsException(arr), "a ResizeObserverEntry's FrozenArray<ResizeObserverSize> could not be "
                                "allocated");
    JS_SetPropertyUint32(ctx, arr, 0, size);
    CHECK(idl_freeze_array(ctx, arr) == 0,
          "a ResizeObserverEntry's FrozenArray<ResizeObserverSize> could not be frozen");
    return arr;
}

JSValue resize_observer_entry_new(JSContext *ctx, JSValueConst target, JSValue content_rect,
                                  JSValue border_box_size, JSValue content_box_size,
                                  JSValue device_pixel_content_box_size)
{
    JSValue obj, state, proto;

    DCHECK(g_ready, "a ResizeObserverEntry was constructed before its interface was declared");
    /* WHAT EACH OF §2.3'S FIVE MEMBERS MAY BE, ASSERTED WHERE THE ENTRY IS BUILT. There is exactly ONE
       producer — §3.4.4, which computes every value — and no page-visible constructor at all, so unknown
       external input is admitted at NO position here: the rectangle is minted by core/geometry/dom_rect.c, the
       three sizes by §3.4.8, and the target is the element §2.1's `observe(Element target)` was handed. A
       shape this list refuses is a shape no producer produces. */
    DCHECK(element_is(target),
           "§2.3's `readonly attribute Element target` was given something that is not an Element — §3.2.15's "
           "brand and `observe`'s narrowing are what refuse a Text node at the interface, and §3.4.4 hands "
           "this component the element the observation named");
    DCHECK(dom_rect_is(content_rect),
           "§2.3's `readonly attribute DOMRectReadOnly contentRect` was given something that is not a "
           "rectangle — there is no `?` on that member to admit a null, and §3.4.4 step 6 builds one for every "
           "entry it creates");
    DCHECK(resize_observer_size_is(border_box_size) && resize_observer_size_is(content_box_size) &&
           resize_observer_size_is(device_pixel_content_box_size),
           "one of §2.3's three FrozenArray<ResizeObserverSize> members was given something that is not a "
           "ResizeObserverSize — §3.4.8 is the only producer and it returns one for every observed box");
    proto = JS_GetClassProto(ctx, g_class);
    DCHECK(!JS_IsNull(proto),
           "a ResizeObserverEntry was constructed in a realm that never ran its prototype install");
    obj = JS_NewObjectProtoClass(ctx, proto, g_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "a ResizeObserverEntry could not be allocated");
    state = JS_NewArray(ctx);
    CHECK(!JS_IsException(state), "a ResizeObserverEntry's state could not be allocated");
    /* EVERY MEMBER IS PLACED BEFORE THE OBJECT IS HANDED BACK — the five are the record, and an entry missing
       one is what `roe_state`'s reader would then have to default past. */
    JS_SetPropertyUint32(ctx, state, ROE_TARGET, JS_DupValue(ctx, target));
    JS_SetPropertyUint32(ctx, state, ROE_CONTENT_RECT, content_rect);
    JS_SetPropertyUint32(ctx, state, ROE_BORDER_BOX_SIZE, roe_frozen_size(ctx, border_box_size));
    JS_SetPropertyUint32(ctx, state, ROE_CONTENT_BOX_SIZE, roe_frozen_size(ctx, content_box_size));
    JS_SetPropertyUint32(ctx, state, ROE_DEVICE_PIXEL_CONTENT_BOX_SIZE,
                         roe_frozen_size(ctx, device_pixel_content_box_size));
    JS_DefinePropertyValue(ctx, obj, g_atom, state, 0);
    return obj;
}

/* ---- declaration and installation -------------------------------------------------------------------------- */

void resize_observer_entry_init(JSContext *ctx)
{
    JSClassDef d = { "ResizeObserverEntry" };

    if (g_ready) return;   /* one AGENT, one class */
    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    JS_NewClass(JS_GetRuntime(ctx), g_class, &d);
    resize_observer_size_init(ctx);

    g_key = JS_NewSymbol(ctx, "resizeObserverEntryState", false);
    CHECK(!JS_IsException(g_key), "the ResizeObserverEntry state slot key allocation failed");
    g_atom = JS_ValueToAtom(ctx, g_key);
    CHECK(g_atom != JS_ATOM_NULL, "the ResizeObserverEntry state slot key could not be interned");

    realm_declare_intrinsic(resize_observer_entry_install_proto);
    g_ready = 1;

    /* DECLARED UNDER THE COMPONENT THAT OWNS THE PLATFORM ROW — core/agent_state.h. See the size record's own
       init for why §2.3's interfaces are not their own rows. */
    agent_state_flag("resize_observer", &g_ready, "§2.3's ResizeObserverEntry declaration latch");
    agent_state_class("resize_observer", &g_class, "§2.3's ResizeObserverEntry class");
    agent_state_value("resize_observer", &g_key, "the entry's state-slot key");
    agent_state_atom("resize_observer", &g_atom, "the entry's state-slot key, interned");
}

void resize_observer_entry_install_proto(JSContext *ctx)
{
    JSValue proto, prev;
    int i;

    DCHECK(g_class != 0, "a realm asked for ResizeObserverEntry.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_class);
    DCHECK(JS_IsNull(prev), "resize_observer_entry_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "ResizeObserverEntry.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "ResizeObserverEntry");
    for (i = 0; i < ROE_COUNT; i++)
        idl_install_accessor(ctx, proto, ROE_MEMBER_NAMES[i], js_roe_get, i, -1);
    JS_SetClassProto(ctx, g_class, proto);
}

/* §2.3 DECLARES NO CONSTRUCTOR HERE EITHER — see the header. */
void resize_observer_entry_install(JSContext *ctx, JSValueConst global)
{
    JSValue proto;

    resize_observer_size_install(ctx, global);
    proto = JS_GetClassProto(ctx, g_class);
    DCHECK(!JS_IsNull(proto),
           "ResizeObserverEntry was installed in a realm that never ran its prototype install");
    JS_SetPropertyStr(ctx, (JSValue)global, "ResizeObserverEntry",
                      idl_interface_object(ctx, "ResizeObserverEntry", proto));
    JS_FreeValue(ctx, proto);
}

/* THIS CLASS HAS NO FINALIZER AND NO gc_mark — see resize_observer_size.c, which states the reasoning for
 * every class in this component. An entry's five members are one Array in an OWN PROPERTY, marked and freed by
 * the unconditional property walk. */
void resize_observer_entry_free(JSRuntime *rt)
{
    DCHECK(g_ready, "§2.3's ResizeObserverEntry was released in an agent that never declared it");
    resize_observer_size_free(rt);
    JS_FreeValueRT(rt, g_key);
    g_key = JS_UNDEFINED;
    JS_FreeAtomRT(rt, g_atom);
    g_atom = JS_ATOM_NULL;
    g_class = 0;
    g_ready = 0;
}
