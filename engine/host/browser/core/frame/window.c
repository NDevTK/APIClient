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
#include "core/url/url_search_params.h"

static JSValue js_win_get_name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return concolic_new(ctx, "{window.name}", "window.name", JS_UNDEFINED);
}

void window_install(JSContext *ctx, JSValueConst global, const char *url)
{
    JSValue g = (JSValue)global;

    DCHECK(JS_IsObject(global), "window_install was given something that is not the global object");

    /* §5's URL interface is a plain platform global, not part of any one component's surface — a bundle builds
       request URLs with it long before it fetches one. */
    url_init(ctx);
    url_install(ctx, global);
    usp_init(ctx);
    usp_install(ctx, global);

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
    /* A window is closed only after close() ran; this one is running its own script. */
    JS_SetPropertyStr(ctx, g, "closed", JS_FALSE);

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
