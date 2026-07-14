/* BOOT-AS-FLOW + CANDIDATE-REPLAY + ATTACKER SESSION — the flow TYPES beyond a plain orphan, each a way of
   re-running the page to reach cross-flow / cross-handler state. boot_replay re-runs the page's inline scripts;
   reg_add_boot/reg_add_session enqueue a forking boot / an attacker session; boot_replay_candidate re-runs boot
   under a candidate; resolve_replayed_handler re-resolves a candidate-closure handler by source identity;
   js_session_fns/js_session_drain build + drive the attacker-session fire list. Reaches the scheduler only
   through scheduler.h (reg_add + the decision context + the orphan collection + the {pm} event) and the handler
   registry; never indexes g_reg. See scheduler.h / handler_registry.h / boot_scripts.h. */
#include <stdlib.h>
#include <string.h>
#include "solver/scheduler.h"       /* reg_add + Flow + g_dec/g_c/g_candidate/g_in_session/g_boot_replay/g_in_boot_flow + g_orphan_buf/g_msg_event */
#include "solver/heap_cow.h"        /* per-flow HEAP COW verb-API: heap_cow_seed_boot_inverse / heap_cow_revert etc. */
#include "solver/boot_scripts.h"    /* boot_scripts_run — re-run the page's inline scripts */
#include "core/dom/handler_registry.h"   /* g_replay_handlers/replay_handlers_clear — candidate-closure re-resolution */

void boot_replay(JSContext *ctx) { g_boot_replay = 1; boot_scripts_run(ctx); g_boot_replay = 0; }
/* Enqueue a BOOT FLOW: re-run boot as a FORKING starter (decision vector), so cached async replies resolve
   synchronously and their continuations' gated branches fork with the concolic example. */
int reg_add_boot(JSContext *ctx, signed char *dec, int dec_n) {
    reg_add(ctx, JS_UNDEFINED, 1.3, dec, dec_n)->is_boot = 1;   /* reg_add never fails (OOM aborts) */
    return 1;
}
/* BOOT AS THE FIRST FLOW: the page's boot (its inline scripts) is captured as a COW DELTA — g_boot_delta —
   exactly like any flow (mutations + global CREATIONS). It stays APPLIED between opaque flows (its effects
   are the post-boot baseline). A candidate flow UNAPPLIES it to reach a TRUE pre-boot heap (boot's globals
   deleted, so a guarded init `if(!window.d){window.d=src}` re-fires), re-runs boot under the concrete
   candidate as its OWN delta, drives, then REAPPLIES g_boot_delta so the next opaque flow sees post-boot.
   No host-side property save/delete/restore — the delta IS the mechanism (heap; DOM boot stays baseline). */
void *g_boot_delta = NULL; int g_boot_delta_n = 0, g_boot_delta_cap = 0;
/* Merge the RUNNING active delta (a post-boot lazy chunk's captured baseline globals) INTO g_boot_delta, so the
   chunk's globals extend the ONE canonical baseline exactly like boot's own — the chunk loader calls this after
   evaluating a post-boot chunk COW-active at mark 0 (see chunk_loader.c). Owned here because g_boot_delta is. */
void boot_delta_merge_active(JSContext *ctx) {
    g_boot_delta = heap_cow_boot_delta_merge(ctx, g_boot_delta, &g_boot_delta_n, &g_boot_delta_cap);
}
void boot_replay_candidate(JSContext *ctx) {
    /* Seed the RUNNING candidate flow's OWN delta with the boot INVERSE (heap -> pre-boot, RECORDED so a
       suspend/revert restores the post-boot baseline), then re-run boot under the concrete candidate as more
       of the SAME delta. The whole boot-undo + candidate-replay is ONE preemptible flow delta — no host-side
       bracket, so a candidate flow yields per-opcode like any other. g_boot_delta is only READ (canonical
       post-boot baseline for non-candidate flows), never unapplied on the shared heap. */
    if (g_boot_delta) heap_cow_seed_boot_inverse(ctx, g_boot_delta, g_boot_delta_n);
    boot_replay(ctx);   /* re-run boot under the concrete candidate (guards re-fire), captured in the candidate's own delta */
}
/* CLOSURE cross-flow: an orphan handler captured at seed time (f.handle) closes over the BASELINE source;
   boot_replay under the candidate re-created that handler with the CANDIDATE closure. Re-resolve to the
   fresh one by SOURCE IDENTITY (same JS_OrphanHash) among the current global functions, so the candidate
   actually flows through the closure the handler reads. Returns a NEW ref (caller frees), or JS_UNDEFINED
   if none differs (then the original handle is correct — e.g. it reads a shared global boot_replay updated). */
JSValue resolve_replayed_handler(JSContext *ctx, JSValueConst orig) {
    if (JS_IsUndefined(orig)) return JS_UNDEFINED;
    uint32_t want = JS_OrphanHash(ctx, orig);
    JSValue found = JS_UNDEFINED;
    /* FIRST the handlers boot_replay re-registered via addEventListener (a closure handler isn't on any
       global) — then the global functions (window.h = closure, module pattern). */
    for (int i = 0; i < g_replay_handler_n && JS_IsUndefined(found); i++) {
        JSValueConst v = g_replay_handlers[i];
        if (JS_VALUE_GET_PTR(v) != JS_VALUE_GET_PTR(orig) && JS_OrphanHash(ctx, v) == want) found = JS_DupValue(ctx, v);
    }
    if (JS_IsUndefined(found)) {
        JSValue g = JS_GetGlobalObject(ctx);
        JSPropertyEnum *tab = NULL; uint32_t n = 0;
        if (JS_GetOwnPropertyNames(ctx, &tab, &n, g, JS_GPN_STRING_MASK) == 0) {
            for (uint32_t i = 0; i < n && JS_IsUndefined(found); i++) {
                JSValue v = JS_GetProperty(ctx, g, tab[i].atom);
                if (JS_IsFunction(ctx, v) && JS_VALUE_GET_PTR(v) != JS_VALUE_GET_PTR(orig) && JS_OrphanHash(ctx, v) == want)
                    found = JS_DupValue(ctx, v);
                JS_FreeValue(ctx, v);
            }
            JS_FreePropertyEnum(ctx, tab, n);
        }
        JS_FreeValue(ctx, g);
    }
    return found;   /* g_replay_handlers/msg stay valid until is_msg_handler(drive) has run; cleared after the drive */
}
