/* THE WINDOW INTERFACE — Blink core/frame, the browsing-context half of the global object.
 *
 * WHY THIS IS A COMPONENT AND NOT TWO LINES IN main.c. `window` and `self` were assigned there directly, which
 * covered the two names a bundle spells most often and nothing else. The rest of the browsing-context surface
 * — `parent`, `top`, `frames`, `opener`, `closed` — was simply missing, and a missing property on the global
 * is `undefined`, not a throw: 139 of 175 WPT dom/nodes documents died inside testharness.js's
 *
 *     var w = self; while (w != w.parent) { w = w.parent; ... }
 *
 * with "cannot read property 'parent' of undefined" on the SECOND iteration. A real browser ends that loop
 * immediately because a top-level window's `parent` IS the window.
 *
 * WHICH BROWSING CONTEXT IS THIS. HTML 7.2.2 answers window/self/frames with this Window's own WindowProxy,
 * and answers parent/top with the parent (or top) navigable's proxy, falling back to this one when there is
 * none. This engine runs ONE document per instance — SECURITY.md's one-WASM-instance-per-DOCUMENT — and holds
 * no channel to an embedder, so within the world this instance models the document IS its own top: parent and
 * top are the window. That is the concrete truth of the modelled context, not a shrug: making it concolic
 * would model an ignorance the engine does not have, and would fork the loop above without end. When the
 * cross-WASM chain that lets an embedded document reach its embedder exists, parent/top resolve through it and
 * this is where that lands.
 *

 * WHAT IT DOES NOT DO. This installed the PLATFORM GLOBALS as well — URL, URLSearchParams, FormData, Blob, the
 * four stream interfaces, TextEncoder/Decoder and their stream forms. None of them is a browsing-context
 * member and none belongs to this component; they were here only because main.c happened to be the one caller.
 * That cost a real gate: the WPT runner installs every one of those itself, so it could not call this without
 * double-installing, and it therefore had NO browsing-context members at all — `window` was not defined, and
 * every test in html/browsers/the-window-object failed on its first line. Two jobs in one function is what
 * made the second caller impossible; each install now sits with its own caller.
 *
 * `name` IS ATTACKER INPUT and is the one member here that is concolic. It survives navigation, so an attacker
 * who can open the document sets it — CLAUDE.md lists it beside cookies and the referrer. Example-free and
 * read through a GETTER, for the same reason location.hash is: a candidate run substitutes a source at MINT
 * time, and a source minted once at install could never receive a breakout. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "solver/concolic.h"
#include "core/frame/window.h"
#include "core/url/url.h"
#include "core/frame/bar_prop.h"
#include "core/html/html_iframe.h"
#include "core/idl_args.h"
#include "solver/cow.h"

/* §7.2.5's `closed`, and the reason it is not the plain `false` that stood here. `close()` CHANGES it, and a
   page reads it to decide whether the window it is holding is still usable. It is therefore per-flow state:
   the flow that closed the window is the only one whose timeline contains that, and a sibling arm that did not
   must still see it open. A single byte with no JSValues in it, so the raw POD capture is the right one — the
   record capture exists for state that OWNS values, and a byte does not. */
static uint8_t g_closed;
static JSValue g_closed_owner = JS_UNDEFINED;   /* what the delta keys the byte by (owned) */

static uint8_t *closed_state(JSContext *ctx)
{
    DCHECK(!JS_IsUndefined(g_closed_owner), "the Window's closed state was reached before window_install ran");
    cow_capture_host_state(ctx, g_closed_owner, &g_closed, sizeof g_closed);
    return &g_closed;
}

static JSValue js_win_closed(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)this_val; (void)magic;
    return JS_NewBool(ctx, *closed_state(ctx));
}

/* §7.2.5.2 `close()`. A REAL STATE CHANGE, never a no-effect: the navigable is closed and `closed` reports it
   from that point on, which is exactly what a page tests before touching a popup again. */
static JSValue js_win_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)this_val; (void)argc; (void)argv; (void)magic;
    *closed_state(ctx) = 1;
    return JS_UNDEFINED;
}

/* §7.2.5's `focus()` and `blur()` move SYSTEM focus between windows, and §7.2.5 defines no scriptable result
   for either — nothing observable to a script changes. This is the documented no-effect the IDL audit permits,
   not a stub standing in for a value: there is no value to compute. `stop()` is the same shape here, because
   this engine has no in-flight navigation to abort. */
static JSValue js_win_noeffect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv; (void)magic;
    return JS_UNDEFINED;
}

/* §7.2.5's `length`: the number of DOCUMENT-TREE CHILD NAVIGABLES. A GETTER over a real walk, never a stored
   number — a page appends a frame, removes it and reads this, and a count something forgot to adjust is wrong
   in exactly that case. It was absent entirely, which is `undefined` rather than 0 and is not the same answer
   for any page that tests it. */
static JSValue js_win_length(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)this_val; (void)magic;
    return JS_NewInt32(ctx, iframe_child_navigable_count(ctx));
}

static JSValue js_win_get_name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return concolic_new(ctx, "{window.name}", "window.name", JS_UNDEFINED);
}

/* §7.2.5.1's EXOTIC OWN-PROPERTY BEHAVIOUR — `window[0]`, and why it is not a property anyone sets.
 *
 * The global is a LEGACY PLATFORM OBJECT. Its SUPPORTED PROPERTY INDICES are its document-tree child
 * navigables, and `window[i]` is the i-th one's WindowProxy. Materialising those as own data properties would
 * be right until the first frame was appended, removed or moved — the set changes on every tree mutation, and
 * a count or a slot that a mutation forgot to adjust is wrong in exactly the case the spec files test. Exotic
 * behaviour is what §7.2.5.1 describes and what this is: every answer is computed from the tree at the moment
 * it is asked.
 *
 * THE THREE OPERATIONS ARE NOT SYMMETRIC, and each asymmetry is a spec sentence:
 *   [[GetOwnProperty]] answers only for a SUPPORTED index — an unsupported one is an ordinary miss, so
 *     `window[7] = "x"` on a frameless page really does create an ordinary property.
 *   [[DefineOwnProperty]] returns FALSE for EVERY array index, supported or not. `Object.defineProperty(window,
 *     0, …)` fails on a page with no frames at all, which is what distinguishes it from the above.
 *   [[Delete]] returns false only for a SUPPORTED index; anything else is the ordinary "nothing to delete".
 *
 * IT IS REACHED ONLY AFTER THE ORDINARY LOOKUP MISSES (quickjs consults a class's exotic get_own_property when
 * find_own_property found nothing), so a global variable read pays for this only when it was going to fail
 * anyway — and the index test is the engine's own, not a re-parse of the atom's text. */
static JSClassID g_window_class;

/* The child navigable this index names, or false. Owned on true. */
static bool win_supported_index(JSContext *ctx, JSAtom prop, JSValue *out)
{
    uint32_t idx;
    JSValue nav;

    if (!JS_AtomIsIndex(ctx, &idx, prop) || idx > (uint32_t)INT32_MAX) return false;
    nav = iframe_child_navigable(ctx, (int)idx);
    if (JS_IsUndefined(nav)) return false;
    *out = nav;
    return true;
}

static int win_get_own(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom prop)
{
    JSValue v;
    (void)obj;
    if (!win_supported_index(ctx, prop, &v)) return 0;
    if (!desc) { JS_FreeValue(ctx, v); return 1; }
    /* §7.2.5.1: { [[Value]]: the child's WindowProxy, [[Writable]]: false, [[Enumerable]]: true,
       [[Configurable]]: true }. */
    desc->flags = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE;
    desc->getter = JS_UNDEFINED;
    desc->setter = JS_UNDEFINED;
    desc->value = v;
    return 1;
}

static int win_define_own(JSContext *ctx, JSValueConst obj, JSAtom prop, JSValueConst val,
                          JSValueConst getter, JSValueConst setter, int flags)
{
    uint32_t idx;

    if (JS_AtomIsIndex(ctx, &idx, prop)) {
        if (flags & JS_PROP_THROW) {
            JS_ThrowTypeError(ctx, "cannot define an indexed property on the window object");
            return -1;
        }
        return 0;
    }
    /* EVERYTHING ELSE IS ORDINARY, and the exotic hook REPLACES the ordinary path rather than preceding it —
       so the ordinary path is re-entered here explicitly, with the exotic step suppressed. Forgetting this
       would not break `window[0]`; it would break every `var` and every property a page defines on the
       global. */
    return JS_DefineProperty(ctx, obj, prop, val, getter, setter, flags | JS_PROP_NO_EXOTIC);
}

static int win_delete(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    JSValue v;
    (void)obj;
    /* Reached only when the ordinary own-property scan found nothing, so "not an index" is "nothing to
       delete", which is true. */
    if (!win_supported_index(ctx, prop, &v)) return 1;
    JS_FreeValue(ctx, v);
    return 0;
}

static int win_own_names(JSContext *ctx, JSPropertyEnum **ptab, uint32_t *plen, JSValueConst obj)
{
    int n = iframe_child_navigable_count(ctx), i;
    JSPropertyEnum *tab;

    (void)obj;
    /* §7.2.5.1 [[OwnPropertyKeys]] lists the supported indices FIRST, in order. quickjs merges what this
       returns with the object's ordinary keys, so only the indices belong here. */
    tab = n ? js_malloc(ctx, sizeof(*tab) * (size_t)n) : NULL;
    if (n && !tab) return -1;
    for (i = 0; i < n; i++) {
        tab[i].is_enumerable = true;
        tab[i].atom = JS_NewAtomUInt32(ctx, (uint32_t)i);
    }
    *ptab = tab;
    *plen = (uint32_t)n;
    return 0;
}

static const JSClassExoticMethods WINDOW_EXOTIC = {
    .get_own_property = win_get_own,
    .get_own_property_names = win_own_names,
    .delete_property = win_delete,
    .define_own_property = win_define_own,
    /* The lookup is a walk of the document tree and a read of each iframe's navigable slot — no page code, by
       construction, which is what lets the engine's own accessor walks run it from C. */
    .get_own_property_no_user_code = true,
};

static const JSClassDef WINDOW_CLASS = { "Window", NULL, NULL, NULL, &WINDOW_EXOTIC };

void window_install(JSContext *ctx, JSValueConst global, const char *url)
{
    JSValue g = (JSValue)global;

    DCHECK(JS_IsObject(global), "window_install was given something that is not the global object");

    /* §7.2.5.1's exotic behaviour comes from the object's CLASS, and the global was created by the context
       before any host class existed — so it is given one here. It owns no per-object data (no finalizer, no
       gc_mark), which is what makes handing it to an already-built object sound. */
    DCHECK(g_window_class == 0, "window_install ran twice — one instance is one document");
    JS_NewClassID(JS_GetRuntime(ctx), &g_window_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_window_class, &WINDOW_CLASS) == 0,
          "the Window class could not be registered");
    CHECK(JS_SetGlobalClass(ctx, g_window_class) == 0,
          "the global object would not take the Window class — §7.2.5.1's indexed access has nowhere to live");

    /* 7.2.2: window, self and frames all return THIS Window's proxy, and the global object IS that proxy here —
       so `window.X`, `self.X` and a bare `X` are one read spelled three ways. */
    JS_SetPropertyStr(ctx, g, "window", JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, g, "self",   JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, g, "frames", JS_DupValue(ctx, global));
    /* No embedder is reachable from this instance, so this document's navigable is its own parent and top. */
    JS_SetPropertyStr(ctx, g, "parent", JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, g, "top",    JS_DupValue(ctx, global));

    /* The document was navigated to, not opened by a script in another navigable: 7.2.2 says null. */
    JS_SetPropertyStr(ctx, g, "opener", JS_NULL);
    /* §7.2.5.3's six user-interface bars. */
    bar_prop_init(ctx);
    bar_prop_install(ctx, global);

    /* §7.2.5 `frameElement` — the element this navigable is nested THROUGH. A top-level navigable is nested
       through nothing, so it is null: the real answer for what this is, not a placeholder for one. */
    JS_SetPropertyStr(ctx, g, "frameElement", JS_NULL);

    /* `closed` is a GETTER over per-flow state now, because close() changes it. */
    g_closed = 0;
    g_closed_owner = JS_DupValue(ctx, global);
    idl_install_accessor(ctx, g, "closed", js_win_closed, 0, -1);
    idl_install_accessor(ctx, g, "length", js_win_length, 0, -1);
    idl_install_method(ctx, g, "close", 0, idl_method_id(ctx, NULL, 0, js_win_close, 0));
    idl_install_method(ctx, g, "focus", 0, idl_method_id(ctx, NULL, 0, js_win_noeffect, 0));
    idl_install_method(ctx, g, "blur",  0, idl_method_id(ctx, NULL, 0, js_win_noeffect, 1));
    idl_install_method(ctx, g, "stop",  0, idl_method_id(ctx, NULL, 0, js_win_noeffect, 2));

    /* The Window's origin, serialized — the principal, concrete for the same reason Location's is: a bundle
       compares it and builds URLs out of it, and a shape there loses every endpoint behind the comparison.
       IT IS §4.7's SERIALIZATION, not a substring of the address. The scan that stood here took everything
       before the first `/?#` after `://`, which is not an origin: it kept a default port that §4.7 drops, kept
       userinfo that an origin never has, and had no answer at all for a scheme with an OPAQUE origin — a
       `data:` document's `origin` is the string "null", and this gave it nothing. */
    if (url && *url) {
        UrlRecord rec;
        if (url_parse(&rec, url, strlen(url), NULL)) {
            char *origin = url_serialize_origin(&rec);
            JS_SetPropertyStr(ctx, g, "origin", JS_NewString(ctx, origin));
            free(origin);
        }
        url_record_free(&rec);
    }

    JS_DefinePropertyGetSet(ctx, g, JS_NewAtom(ctx, "name"),
                            JS_NewCFunction(ctx, js_win_get_name, "get name", 0), JS_UNDEFINED,
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
}

void window_free(JSContext *ctx)
{
    g_window_class = 0;
    JS_FreeValue(ctx, g_closed_owner);
    g_closed_owner = JS_UNDEFINED;
    bar_prop_free(ctx);
}
