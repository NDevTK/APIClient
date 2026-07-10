/* HTMLScriptElement / ScriptLoader — see html_script_element.h. Moved out of the scheduler (main.c): an
 * inserted <script src> is discovered as a lazy chunk and its URL queued for fetch. It FEEDS the ONE scheduler
 * via extern edges (chunk_pending_add / g_chunkurls); the scheduler decides when to fetch + execute. */
#include "core/html/html_script_element.h"
#include "attr_shadow.h"   /* attr_shadow_find/opaque — a computed src leaves the real URL in the taint shadow */
#include "platform/url.h"           /* has_hole — a still-holey (computed) src is not concretely fetchable */
#include "check.h"                  /* DCHECK — the node is a real inserted element (appendChild guarantees non-NULL) */
#include <string.h>
#include <stdlib.h>

extern char *g_candidate;                            /* @S replay: a candidate flow VERIFIES a sink, it must not discover chunks */
extern void chunk_pending_add(const char *url);      /* queue a discovered chunk for fetch (scheduler edge, main.c) */
extern void arr_push_str(JSContext *ctx, JSValueConst arr, const char *s);   /* push a string onto a JS array (main.c) */
extern JSValue g_chunkurls;                          /* JS array of discovered external <script src> (main.c) */

static int el_is_script(lxb_dom_element_t *el) {
    size_t nl = 0; const lxb_char_t *nm = lxb_dom_element_qualified_name(el, &nl);
    return nm && nl == 6 && memcmp(nm, "script", 6) == 0;
}

void script_maybe_load(JSContext *ctx, lxb_dom_element_t *el) {
    DCHECK(el, "script_maybe_load: NULL node — only ever called on a real appended child element, impossible");
    if (!el_is_script(el)) return;
    /* A candidate-REPLAY flow's <script src> is derived from the injected candidate PAYLOAD, not a real chunk
       URL — discovering it drives a nonsensical fetch that livelocks a multi-sink handler. Skip under a candidate. */
    if (g_candidate) return;
    char *u = NULL;
    /* Prefer the CONCOLIC EXAMPLE from the attribute shadow: a reply-field / computed src (`s.src = m.chunk`)
       leaves only the holey SHAPE in the Lexbor attribute, so the real chunk URL lives in the shadow. */
    int si = attr_shadow_find(el, "src");
    if (si >= 0) {
        JSValue ex = JS_OpaqueExample(ctx, attr_shadow_opaque(si));
        if (!JS_IsUndefined(ex)) { const char *e = JS_ToCString(ctx, ex); if (e) { u = strdup(e); JS_FreeCString(ctx, e); } }
        JS_FreeValue(ctx, ex);
    }
    if (!u) {
        size_t sl = 0; const lxb_char_t *src = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"src", 3, &sl);
        if (src && sl) u = strndup((const char *)src, sl);
    }
    if (u) { arr_push_str(ctx, g_chunkurls, u); if (!has_hole(u)) chunk_pending_add(u); free(u); }   /* -> chunkUrls + fetch in place */
}
