/* window.name attacker source — see winname.h. Mirrors document.cookie's per-flow override model: a page
 * write makes the value page-owned concrete; otherwise reads are the attacker opaque {name} (or, on an @S
 * replay flow, the concrete candidate delivered RAW — window.name is not URL-encoded, so the payload survives
 * verbatim to the sink). */
#include "core/frame/winname.h"
#include "opaque.h"   /* JS_NewOpaqueSourced */
#include <stddef.h>

extern char *g_candidate;   /* @S replay: the concrete candidate (raw for window.name — no URL encode set) */

static JSValue g_winname = JS_UNDEFINED;   /* page-set override; JS_UNDEFINED = the page has not written name */

JSValue js_winname_get(JSContext *ctx, JSValueConst this_val) {
    (void)this_val;
    if (!JS_IsUndefined(g_winname)) return JS_DupValue(ctx, g_winname);   /* the page overwrote name -> its own value (blocks the attacker path) */
    if (g_candidate) return JS_NewString(ctx, g_candidate);              /* @S replay: raw attacker candidate (no percent-encoding) */
    return JS_NewOpaqueSourced(ctx, "{name}", "{name}");                 /* attacker external input: control-flow forks, @S taint holds */
}

JSValue js_winname_set(JSContext *ctx, JSValueConst this_val, JSValueConst val) {
    (void)this_val;
    JS_FreeValue(ctx, g_winname);
    g_winname = JS_DupValue(ctx, val);   /* concrete OR concolic (a value the page derived from other input stays tainted) */
    return JS_UNDEFINED;
}

void winname_free(JSContext *ctx) { JS_FreeValue(ctx, g_winname); g_winname = JS_UNDEFINED; }
