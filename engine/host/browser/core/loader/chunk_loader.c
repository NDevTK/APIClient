/* CHUNK LOADER — see chunk_loader.h. The pending/done registries + the fetch-and-evaluate of a lazy chunk,
 * extracted from main.c's qjs_provide so the scheduler entry holds no loader control flow. */
#include "core/loader/chunk_loader.h"
#include "core/loader/module_loader.h"      /* modsrc_put / dynimport_link / pendimport_resolve / is_moddep */
#include "core/html/html_script_runner.h"   /* eval_page_script / boot_exec_one / dom_script_kind (SK_*) / dom_boot_parked_is / dom_boot_resume */
#include "core/html/html_script_element.h"  /* script_load_release — a loaded chunk makes its <script> load handlers eligible */
#include "solver/boot_scripts.h"            /* boot_script_cache — compile-once bytecode for the shared classic executor */
#include "solver/boot_flow.h"               /* boot_delta_merge_active — a chunk's captured baseline globals extend the ONE g_boot_delta */
#include "solver/dom_cow.h"                 /* g_dom_capture — suspended while a chunk's baseline DOM writes land */
#include "solver/why.h"                     /* why_add — a chunk's runtime throw surfaces as @WHY */
#include "check.h"                          /* DCHECK — the loader's own invariants crash LOUD at their origin */
#include <string.h>
#include <stdlib.h>

/* Boot-orchestration state the loader reads/drives (owned by the scheduler entry, main.c): whether boot's script
   phase is still running/parked (a chunk arriving mid-boot joins the boot delta, not the post-boot baseline) and
   boot completion (a parked sync external resuming to the cursor end). A loader legitimately depends on boot
   state; this is that narrow edge. (g_dom_capture is the solver's DOM-COW flag -> solver/dom_cow.h.) */
extern int g_boot_active;
void boot_complete(JSContext *ctx);

/* ── pending: a lazy chunk to fetch + eval IN PLACE (no promise awaits it — like a browser async script load). */
static char **g_pending = NULL; static int g_pending_n = 0, g_pending_cap = 0;
/* ── done: a lazy chunk is a STATIC resource (bytes never change) -> fetched ONCE. Its body is cached / module-
   linked and re-run on every boot re-run, never re-fetched. Without this the next boot re-run's re-injection
   re-adds it -> the bridge re-fetches forever (a provide LIVELOCK). This is the one-per-URL resource cache
   CLAUDE.md prescribes, NOT a flow seen-set (it dedups an identical STATIC GET, never distinct exploration). */
static char **g_done = NULL; static int g_done_n = 0, g_done_cap = 0;

static int chunk_is_done(const char *url) {
    for (int i = 0; i < g_done_n; i++) if (strcmp(g_done[i], url) == 0) return 1;
    return 0;
}
static void chunk_mark_done(const char *url) {
    if (!url || chunk_is_done(url)) return;
    if (g_done_n >= g_done_cap) { int nc = g_done_cap ? g_done_cap * 2 : 16; char **n = realloc(g_done, (size_t)nc * sizeof(char *)); if (!n) return; g_done = n; g_done_cap = nc; }
    g_done[g_done_n++] = strdup(url);
}
void chunk_pending_add(const char *url) {
    if (!url) return;
    if (chunk_is_done(url)) return;   /* already fetched this session -> re-run from cache, never re-fetch (kills the provide livelock) */
    for (int i = 0; i < g_pending_n; i++) if (strcmp(g_pending[i], url) == 0) return;   /* dedup pending */
    if (g_pending_n >= g_pending_cap) { int nc = g_pending_cap ? g_pending_cap * 2 : 16; char **n = realloc(g_pending, (size_t)nc * sizeof(char *)); if (!n) return; g_pending = n; g_pending_cap = nc; }
    g_pending[g_pending_n++] = strdup(url);
}
int chunk_pending_count(void) { return g_pending_n; }
const char *chunk_list(void) {
    static char *buf = NULL; static size_t cap = 0;
    size_t need = 1;
    for (int i = 0; i < g_pending_n; i++) need += strlen(g_pending[i]) + 1;
    if (need > cap) { char *n = realloc(buf, need); if (!n) return ""; buf = n; cap = need; }
    size_t off = 0;
    for (int i = 0; i < g_pending_n; i++) { size_t l = strlen(g_pending[i]); memcpy(buf + off, g_pending[i], l); off += l; buf[off++] = '\n'; }
    buf[off] = 0;
    return buf;
}

/* Run an external CLASSIC chunk (async <script src>, or a runtime-inserted classic) through the SAME executor
   as inline classics + every boot replay: cache its bytecode (so boot-replay re-runs it) and exec. A first-fetch
   page throw surfaces as @WHY, non-fatal. The chunk runs COW-ACTIVE (the caller sets the delta placement: mid-boot
   the open boot delta, post-boot a captured delta merged into g_boot_delta) — its globals are captured either way,
   never a raw baseline write. Shared by both provide paths. */
static void chunk_run_classic(JSContext *ctx, const char *url, const char *body) {
    JSValueConst bc = boot_script_cache(ctx, JS_NULL, body, strlen(body));
    if (boot_exec_one(ctx, JS_NULL, bc)) {
        JSValue e = JS_GetException(ctx); const char *m = JS_ToCString(ctx, e);
        char rz[300]; snprintf(rz, sizeof rz, "%s: %s", url, m ? m : "throw");
        if (m) JS_FreeCString(ctx, m); JS_FreeValue(ctx, e);
        why_add(ctx, "script-eval", rz);
    }
}
/* Link a MODULE chunk's singleton (evaluated once, exports cached — dynimport_link is idempotent) and PARK-RESUME
   deliver its namespace to every parked import() of this URL. Runs COW-ACTIVE like any chunk: the module's global
   side-effects are captured into the baseline delta (open mid-boot / merged post-boot), never a COW-off raw write
   (the wrong pattern deleted from provide_baseline in the shape-desync fix). Shared by both provide paths. */
static void chunk_link_module(JSContext *ctx, const char *url) {
    JSValue mns;
    if (dynimport_link(ctx, url, &mns)) { JS_FreeValue(ctx, mns); pendimport_resolve(ctx, url); }
    else defermod_add(url);   /* dep not fetched: defer by URL, re-linked against the module map when the dep arrives (Blink ModuleTreeLinker; no source re-eval) */
}

/* Evaluate a chunk that arrives WHILE BOOT IS PARKED on a synchronous external: it joins the OPEN boot delta (COW
   stays active), routed by the request-time kind — the parked-on external resumes the cursor, an async external
   runs now (non-blocking), a module a boot script imported links + delivers to its parked import(). Same eval
   helpers as post-boot provide_baseline; only the delta is already-open here vs merged there. */
static void provide_boot_active(JSContext *ctx, const char *url, const char *body) {
    modsrc_put(url, body, strlen(body));
    if (dom_boot_parked_is(url)) {
        if (!dom_boot_resume(ctx)) boot_complete(ctx);   /* the sync CLASSIC external boot waited on ran in position; cursor reached the end -> g_boot_delta + seed */
    } else if (dom_script_kind(url) == SK_ASYNC) {
        chunk_run_classic(ctx, url, body);               /* async external: run now, globals join the open boot delta */
    } else {
        chunk_link_module(ctx, url);               /* a module a boot script imported: evaluate once + deliver to its parked import() (COW-on, globals join the boot delta) */
    }
}

/* Evaluate a chunk POST-BOOT: extend the page BASELINE. The chunk runs COW-ACTIVE at baseline mark (mark 0),
   EXACTLY like boot's own scripts — so its global CREATIONS/mutations are CAPTURED by (obj,atom) identity — then
   its captured delta is MERGED into g_boot_delta, the ONE canonical baseline every flow reconstructs from. Every
   boot-inverse / candidate / cross-session replay flow then handles a chunk's globals IDENTICALLY to boot's own
   (pointer-safe via find_own_property). This is the SAME treatment as a chunk arriving mid-boot (provide_boot_
   active); the only difference is the baseline is already TAKEN, so we merge instead of appending to the open
   delta. (Deleted: the old COW-OFF raw write, whose uncaptured add_property grew global_obj's shape with no delta
   entry, desyncing the shape pointer per-flow — the concrete-onload bug.) DOM writes stay baseline (g_dom_capture
   0), matching how boot treats the DOM (heap in the delta; DOM boot stays baseline). Route by the RECORDED kind,
   never by JS_DetectModule (which mis-classifies a plain classic script as a module). */
static void provide_baseline(JSContext *ctx, const char *url, const char *body) {
    JS_CowRevert(ctx);                                   /* to baseline (parked flows have empty delta); g_cow_undo empty for the chunk's fresh captures */
    int dsv = g_dom_capture; g_dom_capture = 0; JS_SetFlowLocalMark(0);   /* chunk heap objects are BASELINE (shared); DOM writes baseline like boot */
    modsrc_put(url, body, strlen(body));                 /* available to the module loader by URL */
    if (is_moddep(url)) {
        defermod_retry(ctx);                             /* a dep arrived: re-link every deferred URL'd module against the map (Blink ModuleTreeLinker) */
        pendmod_retry(ctx);                              /* + inline modules deferred by source (no URL to key the map) */
    } else if (dom_script_kind(url) == SK_MODULE) {
        chunk_link_module(ctx, url);               /* external MODULE: evaluate the singleton ONCE (dynimport_link) + deliver to parked import()s + defer on a missing dep */
    } else {
        chunk_run_classic(ctx, url, body);               /* external CLASSIC: same executor as inline classics + every replay */
    }
    boot_delta_merge_active(ctx);                         /* MERGE the chunk's captured baseline delta into g_boot_delta (the ONE baseline) */
    JS_SetFlowLocalMark(1); g_dom_capture = dsv;
}

int chunk_provide(JSContext *ctx, const char *url, const char *body) {
    DCHECK(ctx, "chunk_provide: NULL context — the engine's ctx is created at qjs_init, present for every provide");
    for (int i = 0; i < g_pending_n; i++) {
        if (strcmp(g_pending[i], url) != 0) continue;
        if (body && body[0]) {
            if (g_boot_active) provide_boot_active(ctx, url, body);
            else               provide_baseline(ctx, url, body);
            script_load_release(url);   /* the chunk LOADED: its 'load' handlers are now eligible (drive on the post-provide baseline, not seed) */
        }
        chunk_mark_done(url);   /* fetched once: body cached/linked + re-run on boot re-runs, never re-fetched */
        free(g_pending[i]);
        for (int j = i; j < g_pending_n - 1; j++) g_pending[j] = g_pending[j + 1];
        g_pending_n--;
        return 1;
    }
    return 0;
}

void chunk_loader_free(void) {
    for (int i = 0; i < g_pending_n; i++) free(g_pending[i]);
    free(g_pending); g_pending = NULL; g_pending_n = g_pending_cap = 0;
    for (int i = 0; i < g_done_n; i++) free(g_done[i]);
    free(g_done); g_done = NULL; g_done_n = g_done_cap = 0;
    script_load_free();   /* the <script> load-eligibility registry (html_script_element) — same loader lifecycle */
}
