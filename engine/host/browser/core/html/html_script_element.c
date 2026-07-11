/* HTMLScriptElement / ScriptLoader — see html_script_element.h. Moved out of the scheduler (main.c): an
 * inserted <script src> is discovered as a lazy chunk and its URL queued for fetch. It FEEDS the ONE scheduler
 * via extern edges (chunk_pending_add / g_chunkurls); the scheduler decides when to fetch + execute. */
#include "core/html/html_script_element.h"
#include "core/html/html_script_runner.h"  /* dom_script_kind_set / SK_MODULE — record the kind so provide routes it (no re-parse) */
#include "solver/attr_shadow.h"   /* attr_shadow_find/opaque — a computed src leaves the real URL in the taint shadow */
#include "platform/url.h"           /* has_hole — a still-holey (computed) src is not concretely fetchable */
#include "check.h"                  /* DCHECK — the node is a real inserted element (appendChild guarantees non-NULL) */
#include <string.h>
#include <stdlib.h>

extern char *g_candidate;                            /* @S replay: a candidate flow VERIFIES a sink, it must not discover chunks */
extern void chunk_pending_add(const char *url);      /* queue a discovered chunk for fetch (scheduler edge, main.c) */
extern void arr_push_str(JSContext *ctx, JSValueConst arr, const char *s);   /* push a string onto a JS array (main.c) */
extern JSValue g_chunkurls;                          /* JS array of discovered external <script src> (main.c) */
extern JSClassID g_el_class_id;                      /* Element JSClass — unwrap a listener target to its Lexbor element */

static int el_is_script(lxb_dom_element_t *el) {
    size_t nl = 0; const lxb_char_t *nm = lxb_dom_element_qualified_name(el, &nl);
    return nm && nl == 6 && memcmp(nm, "script", 6) == 0;
}

/* SCRIPT-ELEMENT LOAD-EVENT ELIGIBILITY — a <script>'s 'load' handler is a LOAD-GATED CONTINUATION: it must fire
   when the chunk LOADS (chunk-provide), never at boot seed (window.CHUNKGLOBAL doesn't exist yet -> a phantom
   endpoint). We keep it in g_handlers (so the orphan drive passes a real Event) but GATE its eligibility: while
   its chunk is unprovided the scheduler SKIPS it (seed_orphans + the attacker session), and chunk-provide RELEASES
   it so the SAME continuous orphan-drive picks it up on the post-provide baseline. Keyed by the STABLE Lexbor
   element pointer (el_wrap makes a fresh JS wrapper per access, so a wrapper property would not persist). One fact
   per (element,handler); the URL is bound when script_maybe_load resolves it, `ready` flipped on provide. */
typedef struct { void *el; void *fn; char *url; int ready; } ScriptLoad;
static ScriptLoad *g_script_load = NULL; static int g_sl_n = 0, g_sl_cap = 0;

/* Register a script element's 'load' handler as load-gated. Returns 1 iff it was a <script> 'load' listener (the
   caller still adds it to g_handlers for the Event-arg drive; this only records the eligibility gate). */
int script_load_bind_if(JSContext *ctx, JSValueConst target, const char *type, JSValueConst fn) {
    if (!type || strcmp(type, "load") != 0 || !JS_IsFunction(ctx, fn)) return 0;
    lxb_dom_element_t *el = JS_GetOpaque(target, g_el_class_id);
    if (!el || !el_is_script(el)) return 0;
    if (g_sl_n >= g_sl_cap) { int nc = g_sl_cap ? g_sl_cap * 2 : 8; ScriptLoad *n = realloc(g_script_load, (size_t)nc * sizeof(ScriptLoad)); if (!n) return 1; g_script_load = n; g_sl_cap = nc; }
    g_script_load[g_sl_n].el = el; g_script_load[g_sl_n].fn = JS_VALUE_GET_PTR(fn); g_script_load[g_sl_n].url = NULL; g_script_load[g_sl_n].ready = 0; g_sl_n++;
    return 1;
}
/* Bind the resolved chunk URL to this element's pending load handler(s) (script_maybe_load, onload-before-append). */
static void script_load_seturl(void *el, const char *url) {
    for (int i = 0; i < g_sl_n; i++) if (g_script_load[i].el == el && !g_script_load[i].url) g_script_load[i].url = strdup(url);
}
/* Chunk provided: RELEASE every load handler waiting on this URL (now eligible; the next seed_orphans drives it). */
void script_load_release(const char *url) {
    if (!url) return;
    for (int i = 0; i < g_sl_n; i++) if (g_script_load[i].url && strcmp(g_script_load[i].url, url) == 0) g_script_load[i].ready = 1;
}
/* Is `fnptr` a load handler whose chunk has NOT yet provided? Then the scheduler must not drive it (onload before
   load = a phantom). Once released (ready) it is no longer gated -> the normal orphan drive runs it. */
int script_load_gated(void *fnptr) {
    for (int i = 0; i < g_sl_n; i++) if (g_script_load[i].fn == fnptr && !g_script_load[i].ready) return 1;
    return 0;
}
void script_load_free(void) {
    for (int i = 0; i < g_sl_n; i++) free(g_script_load[i].url);
    free(g_script_load); g_script_load = NULL; g_sl_n = g_sl_cap = 0;
}
static int el_is_module(lxb_dom_element_t *el) {
    size_t tl = 0; const lxb_char_t *t = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"type", 4, &tl);
    return t && tl == 6 && memcmp(t, "module", 6) == 0;
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
        JSValue ex = JS_ConcolicExample(ctx, attr_shadow_opaque(si));
        if (!JS_IsUndefined(ex)) { const char *e = JS_ToCString(ctx, ex); if (e) { u = strdup(e); JS_FreeCString(ctx, e); } }
        JS_FreeValue(ctx, ex);
    }
    if (!u) {
        size_t sl = 0; const lxb_char_t *src = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"src", 3, &sl);
        if (src && sl) u = strndup((const char *)src, sl);
    }
    if (u) {
        arr_push_str(ctx, g_chunkurls, u);
        if (!has_hole(u)) {
            dom_script_kind_set(u, el_is_module(el) ? SK_MODULE : SK_SYNC);   /* route module-vs-classic by the ELEMENT type, not by re-parsing (JS_DetectModule mis-detects classic as module) */
            script_load_seturl(el, u);   /* bind the resolved URL to this element's load handler(s) so provide releases them */
            chunk_pending_add(u);
        }
        free(u);
    }
}
