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
#include "core/frame/window_proxy.h"
#include "core/dom/document.h"

static JSClassID g_bar_class;
static JSRuntime *g_bar_rt;

/* §7.2.5.3: `visible` is true for a navigable whose chrome was not suppressed at creation, and that is now
   read from the NAVIGABLE — §7.4's features argument decides it, navigable.c records the decision, and this
   answers the negation. It was the constant `true`, which made every popup indistinguishable from a tab: the
   corpus tells the two apart by reading exactly these six, and `allBarProps.every(x => !x)` is how it does it.
   THE RECORD IS THE BRAND, not the answer. Six independent objects are what §7.2.5.3 declares and what
   `window.locationbar !== window.menubar` rests on, but what they answer is one fact about one navigable — so
   the state lives where the fact does and the instance exists to be a distinct object with a class to check. */
typedef struct { uint8_t unused; } BarProp;

static void bar_finalizer(JSRuntime *rt, JSValue val)
{
    BarProp *b = JS_GetOpaque(val, g_bar_class);
    (void)rt;
    free(b);
}

/* §7.2.5.3's `visible`, answered for THE REALM THE GETTER BELONGS TO.
 *
 * `ctx` HERE IS THE FUNCTION'S OWN REALM, NOT THE CALLER'S — js_call_c_function does `ctx = p->u.cfunc.realm`
 * before invoking a C function, which is §3.7's rule that every realm gets its own intrinsics doing its job.
 * So a getter installed ONCE, on a prototype built once at agent init, answers every realm's question with the
 * ROOT realm's `ctx` forever. That is not a subtlety to remember at this call site: it is what makes a shared
 * prototype WRONG rather than merely unfaithful, and it cost a whole feature — every popup read all six of
 * these as `true` and reported itself a tab, because the root navigable is not a popup and the root is whose
 * ctx arrived. The 51 subtests of window-open-popup-behavior.html split exactly along that line: every
 * "expect tab" passed and every "expect popup" failed.
 *
 * SO THE PROTOTYPE IS PER REALM, in quickjs's own per-context class-proto slot — the same place window.c keeps
 * Window.prototype and for the same reason. `frames[0].BarProp === BarProp` is false in a browser, and now
 * here; the six instances a realm installs chain to ITS prototype, so ITS getter carries ITS ctx. */
static JSValue js_bar_visible(JSContext *ctx, JSValueConst this_val, int magic)
{
    BarProp *b = JS_GetOpaque(this_val, g_bar_class);
    (void)magic;
    DCHECK(b != NULL, "BarProp.visible was read off something that is not a BarProp");
    /* IT MUST BE THIS REALM'S NAVIGABLE. §7.2.5.1 gives a navigable ONE WindowProxy and a realm IS one
       document, so a realm holding a proxy over another document would answer all six for the wrong window —
       indistinguishable from the right answer whenever the two happen to agree, which is most of the time. */
    DCHECK(window_proxy_doc(document_window_proxy(ctx)) == document_doc(ctx),
           "a realm's WindowProxy names a different document than the realm — §7.2.5.1 gives a navigable one "
           "proxy and document_install is handed the navigable's, so these two cannot disagree");
    return JS_NewBool(ctx, !window_proxy_is_popup(document_window_proxy(ctx)));
}

/* THE CLASS IS THE AGENT'S — a class id is a runtime-wide registration and there is one BarProp interface. */
void bar_prop_init(JSContext *ctx)
{
    JSClassDef d = { "BarProp", .finalizer = bar_finalizer };
    JSRuntime *rt = JS_GetRuntime(ctx);

    DCHECK(g_bar_rt == NULL, "BarProp was installed into a second runtime");
    g_bar_rt = rt;
    JS_NewClassID(rt, &g_bar_class);
    JS_NewClass(rt, g_bar_class, &d);
}

/* THE PROTOTYPE IS THE REALM'S — §3.7 gives every realm its own, and here that is load-bearing rather than
   pedantic: the getter it carries runs in the realm that BUILT it (see js_bar_visible). Kept in quickjs's
   per-context class-proto slot, which is where a per-realm prototype belongs and what makes the instances a
   realm mints chain to the right one with no table here to keep. */
static JSValue bar_prop_build_proto(JSContext *ctx)
{
    JSValue proto = JS_NewObject(ctx);

    CHECK(!JS_IsException(proto), "the BarProp prototype could not be allocated");
    idl_interface_tag(ctx, proto, "BarProp");
    idl_install_accessor(ctx, proto, "visible", js_bar_visible, 0, -1);
    JS_SetClassProto(ctx, g_bar_class, JS_DupValue(ctx, proto));   /* the realm owns it from here */
    return proto;
}

/* One bar. Six of these hang off the Window, each its own object. */
static JSValue bar_prop_new(JSContext *ctx)
{
    JSValue obj;
    BarProp *b;

    DCHECK(g_bar_rt != NULL, "a BarProp was minted before bar_prop_init ran");
    obj = JS_NewObjectClass(ctx, g_bar_class);
    if (JS_IsException(obj)) return obj;
    /* JS_NewObjectClass already gives it this REALM's class prototype, which bar_prop_install built. */
    b = calloc(1, sizeof *b);
    CHECK(b != NULL, "BarProp: OOM");
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
    JSValue proto = bar_prop_build_proto(ctx);

    JS_FreeValue(ctx, proto);   /* the realm's class-proto slot holds it */
    for (i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
        JSValue bar = bar_prop_new(ctx);
        CHECK(!JS_IsException(bar), "a BarProp could not be allocated");
        JS_SetPropertyStr(ctx, (JSValue)global, NAMES[i], bar);
    }
}

void bar_prop_free(JSContext *ctx)
{
    if (!g_bar_rt) return;
    /* NOTHING TO RELEASE HERE ANY MORE: each realm's prototype is held by that realm's class-proto slot and
       goes with the realm. */
    g_bar_rt = NULL;
}
