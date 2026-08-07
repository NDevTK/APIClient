/* BarProp — HTML §7.2.5.3, the six user-interface objects a Window exposes.
 *
 * WHAT THEY ARE. `window.locationbar`, `menubar`, `personalbar`, `scrollbars`, `statusbar` and `toolbar` each
 * answer a single boolean, `visible`. They are the last surviving trace of a time when a page could ask the
 * browser to hide its chrome, and they are still specified, still shipped, and still read — a page that probes
 * `window.toolbar.visible` to tell a popup from a tab gets a real answer in every browser.
 *
 * WHY THEY ARE NOT A SHRUG. Headless is not valueless: the spec defines what `visible` returns without any
 * screen at all. §7.2.5.3 says each returns true unless the navigable was created by `open()` with features
 * that suppressed that bar — so for the top-level navigable this engine hosts, the answer is TRUE, computed
 * from what the navigable IS rather than picked. That is the same reasoning `matchMedia` resolves a default
 * viewport by: the missing piece is a physical device, and the spec's behaviour does not depend on one.
 *
 * SIX OBJECTS, NOT ONE. `window.locationbar !== window.menubar` is asserted by the spec's own tests, and it
 * follows from what they are — six independent bars, each with its own state — so they are six instances of
 * one class rather than one object installed six times. Getting that wrong is the kind of thing nobody notices
 * until a page uses one as a map key.
 *
 * WHY IT IS ITS OWN FILE. It is its own IDL interface with its own prototype, and window.c is the Window's
 * members — the same split every other interface here gets. */
#include <stdlib.h>

#include "check.h"
#include "quickjs.h"
#include "core/frame/bar_prop.h"
#include "core/idl_args.h"

static JSClassID g_bar_class;
static JSRuntime *g_bar_rt;
static JSValue g_bar_proto = JS_UNINITIALIZED;

/* §7.2.5.3: `visible` is true for a navigable whose chrome was not suppressed at creation. This engine's
   top-level navigable is not one that `open()` created with features, so every bar is visible. It is read off
   the instance rather than returned as a constant, because a popup created with `open(url, name, "menubar=no")`
   answers differently and that is where this reads from when navigable.c carries those features. */
typedef struct { uint8_t visible; } BarProp;

static void bar_finalizer(JSRuntime *rt, JSValue val)
{
    BarProp *b = JS_GetOpaque(val, g_bar_class);
    (void)rt;
    free(b);
}

static JSValue js_bar_visible(JSContext *ctx, JSValueConst this_val, int magic)
{
    BarProp *b = JS_GetOpaque(this_val, g_bar_class);
    (void)magic;
    DCHECK(b != NULL, "BarProp.visible was read off something that is not a BarProp");
    return JS_NewBool(ctx, b->visible);
}

void bar_prop_init(JSContext *ctx)
{
    JSClassDef d = { "BarProp", .finalizer = bar_finalizer };
    JSRuntime *rt = JS_GetRuntime(ctx);

    DCHECK(g_bar_rt == NULL, "BarProp was installed into a second runtime");
    g_bar_rt = rt;
    JS_NewClassID(rt, &g_bar_class);
    JS_NewClass(rt, g_bar_class, &d);
    g_bar_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_bar_proto), "the BarProp prototype could not be allocated");
    idl_interface_tag(ctx, g_bar_proto, "BarProp");
    idl_install_accessor(ctx, g_bar_proto, "visible", js_bar_visible, 0, -1);
}

/* One bar. Six of these hang off the Window, each its own object. */
static JSValue bar_prop_new(JSContext *ctx, bool visible)
{
    JSValue obj;
    BarProp *b;

    DCHECK(g_bar_rt != NULL, "a BarProp was minted before bar_prop_init ran");
    obj = JS_NewObjectClass(ctx, g_bar_class);
    if (JS_IsException(obj)) return obj;
    JS_SetPrototype(ctx, obj, g_bar_proto);
    b = calloc(1, sizeof *b);
    CHECK(b != NULL, "BarProp: OOM");
    b->visible = visible ? 1 : 0;
    JS_SetOpaque(obj, b);
    return obj;
}

void bar_prop_install(JSContext *ctx, JSValueConst global)
{
    /* §7.2.5.3's six, in the order the IDL declares them. Each is [Replaceable], which is an ordinary writable
       property — so a page may shadow one, and the COW delta captures that like any other write. */
    static const char *const NAMES[] = {
        "locationbar", "menubar", "personalbar", "scrollbars", "statusbar", "toolbar"
    };
    size_t i;

    for (i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
        JSValue bar = bar_prop_new(ctx, true);
        CHECK(!JS_IsException(bar), "a BarProp could not be allocated");
        JS_SetPropertyStr(ctx, (JSValue)global, NAMES[i], bar);
    }
}

void bar_prop_free(JSContext *ctx)
{
    if (!g_bar_rt) return;
    JS_FreeValue(ctx, g_bar_proto);
    g_bar_proto = JS_UNINITIALIZED;
    g_bar_rt = NULL;
}
