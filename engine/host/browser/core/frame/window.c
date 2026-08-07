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

static JSValue js_win_get_name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return concolic_new(ctx, "{window.name}", "window.name", JS_UNDEFINED);
}

void window_install(JSContext *ctx, JSValueConst global, const char *url)
{
    JSValue g = (JSValue)global;

    DCHECK(JS_IsObject(global), "window_install was given something that is not the global object");

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
    JS_FreeValue(ctx, g_closed_owner);
    g_closed_owner = JS_UNDEFINED;
    bar_prop_free(ctx);
}
