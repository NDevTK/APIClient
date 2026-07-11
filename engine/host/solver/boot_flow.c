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
#include "solver/boot_scripts.h"    /* boot_scripts_run — re-run the page's inline scripts */
#include "core/dom/handler_registry.h"   /* g_handlers/is_msg_handler/g_replay_handlers/replay_handlers_clear */
#include "core/dom/events/event.h"  /* js_event_ctor — a driven non-message handler gets a real DOM Event */
#include "core/html/html_script_element.h"   /* script_load_gated — a <script> load handler isn't session-eligible until its chunk provides */

extern JSRuntime *g_rt;   /* the JS runtime (main.c) — js_session_drain drains microtasks per fired handler */

void boot_replay(JSContext *ctx) { g_boot_replay = 1; boot_scripts_run(ctx); g_boot_replay = 0; }
/* Enqueue a BOOT FLOW: re-run boot as a FORKING starter (decision vector), so cached async replies resolve
   synchronously and their continuations' gated branches fork with the concolic example. */
int reg_add_boot(JSContext *ctx, signed char *dec, int dec_n) {
    reg_add(ctx, JS_UNDEFINED, 1.3, dec, dec_n)->is_boot = 1;   /* reg_add never fails (OOM aborts) */
    return 1;
}
/* An EXPLORATORY attacker-session that FORKS: re-fire ALL handlers over one accumulating delta, replaying
   `dec` then forking new opaque branches. This is how a gate in handler B on state that handler A wrote from
   an OPAQUE source (the canonical "login handler sets auth flag; gated code reads it" SPA pattern) gets its
   admin arm explored — a per-handler orphan flow can't (it's COW-isolated from A's write), and a fixed-arm
   session can't (it never forks). A candidate session (verifying a breakout) stays fixed-arm; only this
   exploratory kind forks. */
int reg_add_session(JSContext *ctx, signed char *dec, int dec_n) {
    reg_add(ctx, JS_UNDEFINED, 1.2, dec, dec_n)->session = 1;   /* reg_add never fails (OOM aborts) */
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
    g_boot_delta = JS_CowBootDeltaMerge(ctx, g_boot_delta, &g_boot_delta_n, &g_boot_delta_cap);
}
void boot_replay_candidate(JSContext *ctx) {
    /* Seed the RUNNING candidate flow's OWN delta with the boot INVERSE (heap -> pre-boot, RECORDED so a
       suspend/revert restores the post-boot baseline), then re-run boot under the concrete candidate as more
       of the SAME delta. The whole boot-undo + candidate-replay is ONE preemptible flow delta — no host-side
       bracket, so a candidate flow yields per-opcode like any other. g_boot_delta is only READ (canonical
       post-boot baseline for non-candidate flows), never unapplied on the shared heap. */
    if (g_boot_delta) JS_CowSeedBootInverse(ctx, g_boot_delta, g_boot_delta_n);
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
/* ATTACKER SESSION: fire ALL registered handlers in seed order over the CURRENT (accumulating) COW delta,
   modeling an attacker firing a sequence of events. Handler A's tainted write to shared state persists to
   handler B (no revert between them), so a cross-handler sink — source stored by A, sunk by B — is reached:
   opaque -> the sink is DETECTED (task recorded), candidate -> breakout is VERIFIED. Branches take a fixed
   arm here (per-handler branch exploration is the individual orphan flows' job); the session adds only the
   cross-handler STATE dimension. */
/* __sessionFns(): the session fire list as [fn, event] pairs — event handlers (msg handlers get the synthetic
   {pm} MessageEvent) then the collected ORPHANS (deduped vs handlers, opaque arg). Exposed so the session LOOP
   is SELF-HOSTED bytecode (like the array methods) rather than a C loop that can't yield mid-iteration and so
   runs to completion — a violation of the per-opcode-yield core. */
JSValue js_session_fns(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue arr = JS_NewArray(ctx); uint32_t n = 0, hn = 0;
    if (!JS_IsUndefined(g_handlers)) { JSValue lv = JS_GetPropertyStr(ctx, g_handlers, "length"); JS_ToUint32(ctx, &hn, lv); JS_FreeValue(ctx, lv); }
    for (uint32_t i = 0; i < hn; i++) {
        JSValue h = JS_GetPropertyUint32(ctx, g_handlers, i);
        if (JS_IsFunction(ctx, h) && script_load_gated(JS_VALUE_GET_PTR(h))) { JS_FreeValue(ctx, h); continue; }   /* a <script> load handler whose chunk hasn't provided: not eligible in the session either */
        if (JS_IsFunction(ctx, h)) {
            /* a 'message' handler gets the {pm} MessageEvent; every other handler gets a real DOM Event whose
               target is the PER-CODE-FLOW document (js_event_ctor), so e.target.querySelector/closest work. */
            JSValue ev = (is_msg_handler(h) && !JS_IsUndefined(g_msg_event)) ? JS_DupValue(ctx, g_msg_event) : js_event_ctor(ctx, JS_UNDEFINED, 0, NULL);
            JSValue pair = JS_NewArray(ctx);
            JS_SetPropertyUint32(ctx, pair, 0, JS_DupValue(ctx, h));
            JS_SetPropertyUint32(ctx, pair, 1, ev);   /* consumes ev */
            JS_SetPropertyUint32(ctx, pair, 2, JS_UNDEFINED);   /* handlers fire with this=undefined */
            JS_SetPropertyUint32(ctx, arr, n++, pair);
        }
        JS_FreeValue(ctx, h);
    }
    for (int oi = 0; oi < g_orphan_n; oi++) {
        JSValueConst fn = g_orphan_buf[oi];
        if (!JS_IsFunction(ctx, fn)) continue;
        int is_h = 0;
        for (uint32_t i = 0; i < hn && !is_h; i++) { JSValue h = JS_GetPropertyUint32(ctx, g_handlers, i); if (JS_VALUE_GET_PTR(h) == JS_VALUE_GET_PTR(fn)) is_h = 1; JS_FreeValue(ctx, h); }
        if (is_h) continue;
        JSValue pair = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, pair, 0, JS_DupValue(ctx, fn));
        /* a NON-handler orphan (is_h excluded above) is NOT an event listener: driving it with an Event as arg0
           makes `fetch('/api/org/'+id)` learn a GARBAGE /api/org/[object Object] endpoint. Give it distinct
           external-input source identity ({arg0}), like the main orphan drive — its arg is attacker input, not
           an Event; the session only needs it to run over the accumulated handler state. */
        JS_SetPropertyUint32(ctx, pair, 1, JS_NewConcolicSourced(ctx, "{arg0}", "{arg0}"));
        JS_SetPropertyUint32(ctx, pair, 2, JS_FindReceiver(ctx, fn));   /* real receiver (upgraded custom-element instance) so this.attachShadow/getAttribute work when the session fires connectedCallback */
        JS_SetPropertyUint32(ctx, arr, n++, pair);
    }
    /* THEN re-fire boot-EXECUTED page functions (globalThis own bytecode fns) over the SAME accumulating delta:
       a boot-time reader like `loadDashboard()` ran ONCE at boot with logged-out state, but firing it here —
       AFTER the login handler wrote `state.user=admin` — reaches its gated arm (the admin endpoint) with the
       handler's own concrete values. Orphan-collection excludes executed fns (they ran at boot); the SESSION
       wants them precisely because the accumulated handler state is NEW. C host-edges (fetch/WebSocket) are
       non-bytecode (OrphanHash 0) -> skipped; handlers/orphans already in the list -> deduped. The WFQ starves
       any that emit nothing new — this only ADDS the boot-reader dimension the handler→handler session misses. */
    JSValue g = JS_GetGlobalObject(ctx);
    JSPropertyEnum *gt = NULL; uint32_t gn = 0;
    if (JS_GetOwnPropertyNames(ctx, &gt, &gn, g, JS_GPN_STRING_MASK) == 0) {
        for (uint32_t i = 0; i < gn; i++) {
            const char *nm = JS_AtomToCString(ctx, gt[i].atom);
            int internal = (nm && nm[0] == '_' && nm[1] == '_');   /* OUR injected machinery (__driveSession/__sessionFns/...) — firing it re-enters the session; skip by our OWN naming, not a page heuristic */
            if (nm) JS_FreeCString(ctx, nm);
            JSValue fn = internal ? JS_UNDEFINED : JS_GetProperty(ctx, g, gt[i].atom);
            if (!internal && JS_IsFunction(ctx, fn) && JS_OrphanHash(ctx, fn) != 0) {   /* page-defined bytecode fn, not a C host-edge or our machinery */
                int dup = 0;
                for (int oi = 0; oi < g_orphan_n && !dup; oi++) if (JS_VALUE_GET_PTR(g_orphan_buf[oi]) == JS_VALUE_GET_PTR(fn)) dup = 1;
                for (uint32_t hi = 0; hi < hn && !dup; hi++) { JSValue h = JS_GetPropertyUint32(ctx, g_handlers, hi); if (JS_VALUE_GET_PTR(h) == JS_VALUE_GET_PTR(fn)) dup = 1; JS_FreeValue(ctx, h); }
                if (!dup) {
                    JSValue pair = JS_NewArray(ctx);
                    JS_SetPropertyUint32(ctx, pair, 0, JS_DupValue(ctx, fn));
                    /* a boot-EXECUTED reader (loadDashboard()) is a DATA function, not an event listener — an
                       Event arg makes its `fetch('/api/'+arg)` learn a garbage [object Object] endpoint. Give it
                       {arg0} external-input identity; the session re-fires it for the accumulated state, not an event. */
                    JS_SetPropertyUint32(ctx, pair, 1, JS_NewConcolicSourced(ctx, "{arg0}", "{arg0}"));
                    JS_SetPropertyUint32(ctx, pair, 2, JS_UNDEFINED);
                    JS_SetPropertyUint32(ctx, arr, n++, pair);
                }
            }
            JS_FreeValue(ctx, fn);
        }
        JS_FreePropertyEnum(ctx, gt, gn);
    }
    JS_FreeValue(ctx, g);
    return arr;
}
JSValue js_session_drain(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSContext *c; while (JS_ExecutePendingJob(g_rt, &c) > 0) {}   /* per-fn microtask drain, parity with the C loop */
    return JS_UNDEFINED;
}
