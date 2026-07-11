/* APIClient v2 host entry — the ONE scheduler.
 *
 * DESIGN (the ONE invariant): ONE persistent runtime, ONE top-level scheduler loop, EVERYTHING is a
 * flow the loop schedules. No phases, no separate grind, no second loop.
 *
 * Capabilities so far, each verified on the proven loop:
 *  - value-ordered (NON-FIFO) flow registry in scheduler-owned C memory; everything-a-flow (reg_add).
 *  - PREEMPTION: a flow suspends per-opcode (wfq_yield) + resumes WITH STATE via quickjs-ng async-frame suspend.
 *  - fetch(url) host edge -> @H (the COMPUTED endpoint), all flow code runs in the ONE loop.
 *  - ORPHAN-INVOKE: force-invoke never-executed functions (JS_CollectOrphans) -> the UNUSED endpoints.
 *  - FORCED BRANCH-ARMS: auto-forking at OP_if on OPAQUE external input (branch_decide via the engine's
 *    JS_SetBranchHook) explores BOTH arms of a gated branch by decision-vector BFS — a new decision returns
 *    true for this flow and FORKS a sibling that replays the prefix then takes false, surfacing the
 *    branch-gated (login/flag-gated) endpoints. Value-ordered, so productive paths first.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "quickjs.h"
#include "quickjs-libc.h"
#include <lexbor/html/html.h>
#include <lexbor/css/css.h>
#include <lexbor/selectors/selectors.h>
#include <lexbor/dom/dom.h>
#include "check.h"        /* CHECK (always fatal: OOM/security) / DCHECK (dev-only fatal: should-never-happen), its own TU */
#include "prelude.h"     /* self-hosted JS prelude strings (ARRAY_PRELUDE_JS, DEDUP_JS) */
#include "solver/constraints.h"  /* per-flow value-domain constraint tracker (concolic path constraint), its own TU */
#include "solver/wfq.h"          /* the ONE WFQ priority policy (order key), its own TU */
#include "solver/concolic.h"       /* the OPAQUE sentinel g_concolic + js_noop/js_concolic_read/js_concolic_stub, its own TU */
#include "solver/scheduler.h"    /* Flow (per-flow scheduler state) + AsyncRecipe (replay recipe) — the registry's record types */
#include "solver/solve.h"        /* the @S SOLVER component: solve_add sink entry + gate_collect/solve_all/solve_init/solve_free */
#include "solver/solve_html.h"   /* @S HTML breakout analysis (context-detect + firing-verify), split into its own TU */
#include "solver/envelope.h"     /* @S structured-source delivery envelope (JSON/query/delim addressing), its own TU */
#include "core/frame/csp.h"          /* Content-Security-Policy: effective policy + per-sink-class relevance, its own TU */
#include "core/dom/dom_select.h"   /* CSS selector engine (querySelector/All, matches) over the Lexbor DOM, its own TU */
#include "solver/dom_cow.h"      /* per-flow DOM COW delta (record + apply/unapply/revert + park buffer), its own TU */
#include "solver/attr_shadow.h"  /* DOM attribute taint side-map ((el,name)->opaque), its own TU */
#include "core/html/forms/forms.h"        /* HTML form submission -> @H endpoint, its own TU */
#include "core/dom/classlist.h"    /* element.classList, its own TU */
#include "core/html/docwrite.h"     /* document.write -> @S sink + script loader, its own TU */
#include "platform/urlobj.h"       /* URL + URLSearchParams objects (endpoint URL construction), its own TU */
#include "core/loader/module_loader.h" /* ES-module loader: static+dynamic import graph (modsrc/moddep/pendmod + hooks), its own TU */
#include "core/dom/domparser.h"    /* DOMParser + Range HTML parsing -> {parsedhtml} taint, its own TU */
#include "core/frame/location.h"     /* the browser location object + external-input source getters + principal split, its own TU */
#include "core/dom/dom_element.h"  /* the DOM Element JSClass + el_wrap (methods migrate here incrementally), its own TU */
#include "core/loader/document_scripts.h"  /* scr_ctx + dom_collect_scripts + script_is_exec + document_bundle_id (identity component) */
#include "solver/boot_scripts.h"  /* boot_script_cache/boot_scripts_run/boot_script_count/boot_scripts_free (boot-replay substrate) */
#include "solver/why.h"  /* why_add — runtime-reasoned @WHY */
#include "core/html/html_script_runner.h"  /* eval_page_script + dom_run_scripts (HTMLScriptRunner) */
#include "modules/storage.h"      /* localStorage/sessionStorage concolic round-trip, its own TU */
#include "modules/indexeddb.h"    /* IndexedDB shape stub (Blink modules/indexeddb/), its own TU */
#include "core/frame/messaging.h"    /* MessageChannel/BroadcastChannel (Blink core/messaging), its own TU */
#include "core/dom/document.h"     /* document.querySelector/getElementById/... (Blink core/dom/Document), its own TU */
#include "core/dom/event_target.h" /* EventTarget.prototype — DOM inheritance spine root */
#include "core/dom/node.h"          /* Node.prototype — DOM inheritance spine middle */
#include "core/frame/local_dom_window.h" /* install_window_apis — window global constructors */
#include "bindings/global_functions.h"   /* eval / new Function (@S code sinks) + structuredClone (Blink bindings/core/v8) */
#include "core/html/forms/formdata.h"      /* FormData -> POST body params (Blink core/html/forms), its own TU */
#include "core/dom/custom_elements.h" /* customElements registry + createElement upgrade (Blink core/html/custom), its own TU */
#include "platform/url.h"          /* URL query-parameter extraction (pure string + JS API), its own TU */
#include "solver/reply.h"        /* fetch Response + reply-body learning (make_response), its own TU */
#include "core/loader/xhr.h"          /* XMLHttpRequest emulation -> the @H recorder, its own TU */
#include "core/loader/fetch.h"        /* the fetch() host edge -> the @H recorder, its own TU */
#include "modules/websocket.h"    /* WebSocket + EventSource ctor -> WS/SSE handshake endpoint, its own TU */
#include "modules/worker.h"       /* Worker + SharedWorker ctor -> worker-script chunk, its own TU */
#include "core/frame/navigator.h"    /* navigator.sendBeacon + serviceWorker.register -> @H, its own TU */
#include "core/css/cssom.h"        /* getComputedStyle + matchMedia -> opaque CSSOM environment reads, its own TU */
#include "core/intersection_observer/intersection_observer.h"   /* IntersectionObserver (Blink core/intersection_observer) — real IDL shape */
#include "core/dom/mutation_observer.h"            /* MutationObserver (Blink core/dom) */
#include "core/resize_observer/resize_observer.h"  /* ResizeObserver (Blink core/resize_observer) */
#include "core/timing/performance_observer.h"      /* PerformanceObserver (Blink core/timing) */
#include "bindings/idl.h"          /* Web IDL binding driver — declarative interface tables -> native objects */
#include "core/dom/abort.h"        /* AbortSignal (IDL-defined), its own TU */
#include "core/frame/intl.h"         /* Intl formatters (IDL-defined, opaque results), its own TU */
#include "modules/notification.h" /* Notification (IDL-defined) + requestPermission, its own TU */
#include "core/html/media_element.h" /* Image/Audio/Option ctors + Audio media state machine, its own TU */
#include "core/frame/history.h"      /* window.history real state machine (pushState sets state), its own TU */
#include "core/frame/cookie.h"       /* document.cookie per-flow cookie jar (round-trips writes), its own TU */
#include "core/frame/winname.h"      /* window.name — raw attacker-controlled source (opener-set, not URL-encoded) */
#include "core/frame/screen.h"       /* window.screen + innerWidth/... concolic viewport, its own TU */
#include "core/dom/events/event.h"       /* Event/CustomEvent ctor, its own TU */
#include "modules/crypto.h"      /* Web Crypto (window.crypto), its own TU */
#include "core/timing/performance.h" /* Performance API, its own TU */
#include "core/timing/timers.h"      /* setTimeout/rAF/queueMicrotask -> BFS flows, its own TU */
#include "modules/encoding.h"    /* TextEncoder/TextDecoder (real UTF-8), its own TU */
#include "modules/fsa.h"         /* File System Access (mock FS, attacker file content), its own TU */
#include "core/fileapi/filereader.h"  /* FileReader (real readAsText -> onload), its own TU */
#include "core/fileapi/blob.h"        /* Blob/File (real content taint) */
#include "core/trustedtypes/trusted_types.h"  /* Trusted Types (runs createPolicy) */
#include "core/loader/response.h"    /* Response (real body taint) */
#include "solver/endpoint.h"     /* the shared @H endpoint sink (record_endpoint + g_endpoints), its own TU */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define KEEP EMSCRIPTEN_KEEPALIVE   /* export qjs_init/step/teardown for the persistent-instance protocol */
#else
#define KEEP
#endif

/* ---- the ONE flow registry (scheduler-owned memory) --------------------------------
   A flow is a STARTER (a function to force-invoke, with a decision vector for its branch choices); it may be
   SUSPENDED mid-run (fs = live heap frame, JS_FlowResume) and re-queued. All carry value-of-information. The
   per-flow record `Flow` + async replay recipe `AsyncRecipe` are defined in solver/scheduler.h (so a
   scheduler-coupled solver TU can name them); the registry array + WFQ + dispatch stay here. NO BOUNDS: the
   registry + decision vector grow until the RAM/disk floor; no FLOW_MAX/DEC_MAX cap that truncates work. */
static JSContext *g_ctx;       /* fwd: the MAIN analysis context (defined near the persistent-instance protocol) — the
                                  async/reaction hooks claim a call as a flow ONLY for this ctx, never the @S solve realm */
int g_in_session = 0;   /* a session flow is running -> solve_add enqueues candidate SESSION flows */
static int g_in_boot_flow = 0; /* a BOOT flow is re-running boot: fork boot siblings; suppress handler re-registration */
static Flow   *g_reg = NULL;
static int     g_reg_n = 0, g_reg_cap = 0;
int     g_running = 0;
double  g_cur_val = 0;
Flow   *g_cur_flow = NULL;   /* running flow (a stable local copy; its weight is read by the yield hook) */
int            g_emit_total = 0;    /* non-static: the emit counter (endpoint.c bumps it on a recorded @H) */
JSRuntime *g_rt = NULL;   /* non-static: module_loader.c drives JS_ExecutePendingJob on it */
/* Raise the running flow's emitted VALUE (the WFQ progress signal) by one emit. Encapsulates the g_cur_flow
   write so host-edge components (endpoint.c) can signal an emit WITHOUT the Flow struct — the scheduler owns
   the flow record; they just say "I emitted". */
void flow_emit_value(void) {
    if (g_running) { g_cur_val += 1.0; if (g_cur_flow) { g_cur_flow->val = g_cur_val; g_cur_flow->cpu = 0; } }
}
/* Cross-session frontier (park/resume by REPLAY): deterministic orphan collection gives each function a
   stable index; a parked flow's recipe = (orphan_idx, decision-vector). Parking is RAM-PRESSURE-driven
   (g_park_requested, host-set), never a dispatch/step count — with headroom the page runs to completion. */
static JSValue g_orphan_buf[4096];
static int     g_orphan_n = 0;
int     g_cur_orphan_idx = -1;   /* running flow's orphan index (inherited by its branch siblings) */
static int     g_park_requested = 0;   /* host sets this under RAM pressure -> park the cold tail to IDB (NOT a dispatch count) */
static long    g_work = 0;              /* flow DISPATCHES this run (starter OR resume) — diagnostic only, never a park trigger */
static double  g_yield_floor = -1e300;  /* host cross-document WFQ: yield HOT to the host the moment this engine's best
                                           flow no longer outranks the RUNNER-UP engine (whose weight the host sets here).
                                           VALUE-driven, never a dispatch count — the WFQ, not a clock, decides the switch.
                                           -1e300 = no runner-up (single engine): run to completion/park. */
static int     g_made_progress = 0;     /* dispatched >=1 flow this qjs_step? (guards zero-work ping-pong; NOT a cap) */
/* COOPERATIVE QUANTUM (§NO BOUNDS: a thread-YIELD, never a cap). The ONE worker thread is shared across all
   engines + the message pump + incremental merge; so the running flow hands the thread back to the host loop
   after a bounded wall-clock slice and RESUMES the byte-identical frontier from g_reg. Wall-clock (the honest
   measure of thread-hogging), not an opcode count. g_quantum_start is stamped at each scheduler_run entry; both
   wfq_yield (per-opcode, so a long single flow yields mid-run) and scheduler_run's loop head (so it RETURNS to
   qjs_step rather than re-dispatching the just-expired flow) consult it. Truncates no work, drops no flow. */
#define QUANTUM_MS 12.0
#define QUANTUM_SAMPLE 512u   /* read the wall clock once per this many back-edges (perf: emscripten_get_now
                                 crosses WASM->JS; sampling throttles that). NOT a work bound — it drops/skips
                                 NO flow (the §NO BOUNDS razor); it only throttles WHEN the 12ms wall-clock
                                 quantum is sampled, and the frontier resumes byte-identical regardless. */
static double  g_quantum_start = 0.0;
static unsigned g_quantum_sample = 0;
static int quantum_expired(void) { return g_made_progress && (emscripten_get_now() - g_quantum_start) > QUANTUM_MS; }
static long    g_switches = 0;          /* flow SUSPEND/re-queue events (interleave) -> @RESULT._switches */
/* Highest weight among the PARKED flows (g_reg, excluding the running one). A parked flow's weight is CONSTANT
   while parked (val/visits/cpu don't change), so it changes only when a flow is ADDED (reg_add). Cache it:
   recompute once per dispatch, update O(1) on reg_add. wfq_yield then decides preemption in O(1) instead of
   scanning the whole registry EVERY opcode — same decision, no per-opcode O(N) on the productive flow. */
static double  g_max_parked = -1e300;
static int     g_resume_mode = 0;       /* resuming a parked frontier: seed ONLY the recipes, not fresh orphans */
static uint32_t g_bundle_id = 0;        /* stable id of THIS document's own scripts (Lexbor DOM scan, not regex) — the frontier key */
/* CSP (g_csp/g_header_csp + csp_lacks/csp_derive/csp_set_header/csp_free) lives in csp.c — included below. */
extern lxb_html_document_t *g_dom;   /* the live parsed document (defined below); the @S emitter needs it for the CSP nonce scan */
/* g_concolic + js_noop/js_concolic_read/js_concolic_stub + concolic_init/free live in opaque.c (included via opaque.h). */
char *g_candidate = NULL;          /* @S: the running REPLAY flow's concrete candidate (source getters return it); NULL in normal flows */

/* has_hole (opaque-hole URL test) is in url.c (included via url.h). */

/* decision-vector state for the RUNNING starter flow (branch-arm BFS) — grows unbounded */
static JSValue      g_cur_fn = JS_UNDEFINED;   /* the running starter's function (borrowed) so branch_decide can fork a sibling that re-runs it */
signed char *g_dec = NULL;              /* working decision vector: forced prefix + this flow's chosen-true suffix */
static int          g_dec_cap = 0;
int          g_dec_n = 0;               /* length of decisions made/forced so far */
int          g_c = 0;                   /* cursor: next decision index branch_decide will consume */

static int g_dec_ensure(int n) {              /* grow g_dec to hold >= n decisions */
    if (n <= g_dec_cap) return 1;
    int nc = g_dec_cap ? g_dec_cap * 2 : 64; while (nc < n) nc *= 2;
    signed char *nd = (signed char *)realloc(g_dec, (size_t)nc);
    if (!nd) return 0;
    g_dec = nd; g_dec_cap = nc; return 1;
}
/* Per-flow value-domain constraint tracker (Cons, g_cons, cons_reset-set-feasible-fixed_value, pair_contradicts; plus @S-delivery state g_origin_req, g_sink_jkey, g_sink_root) lives in solver-constraints.c. */
/* url_solve_holes ({src}-hole value-solve) -> browser/url.c (URL construction module). */

/* Per-flow DOM COW delta (dom_attr_capture/insert_capture + dom_apply/unapply/revert + dom_buf_*) is in
   dom_cow.c — included via dom_cow.h above; the buffer is an opaque void* on the Flow. */
/* Append a fresh flow to the ONE registry and RETURN it (never NULL — OOM is a physical floor that CHECK-aborts,
   never a dropped flow). Callers configure the returned Flow directly (f->candidate/session/orphan_idx/...),
   so no caller reaches into g_reg[g_reg_n-1] — the registry array stays encapsulated behind this one entry. */
Flow *reg_add(JSContext *ctx, JSValue handle, double val, signed char *dec, int dec_n)
{
    if (g_reg_n >= g_reg_cap) {
        int nc = g_reg_cap ? g_reg_cap * 2 : 256;
        Flow *nr = (Flow *)realloc(g_reg, (size_t)nc * sizeof(Flow));
        CHECK(nr, "reg_oom: flow-registry realloc failed — OOM is a physical floor, never continue with a dropped flow");
        g_reg = nr; g_reg_cap = nc;
    }
    Flow *f = &g_reg[g_reg_n];
    f->handle = handle; f->val = val;
    f->dec = dec; f->dec_n = dec_n;
    f->fs = NULL; f->saved_c = 0; f->cpu = 0; f->visits = 0;
    f->cow = NULL; f->cow_n = 0; f->cow_cap = 0;
    f->dom = NULL; f->dom_n = 0; f->dom_cap = 0;
    f->orphan_idx = -1; f->candidate = NULL; f->session = 0; f->vtarget = NULL;
    f->drive_src = NULL;
    f->is_boot = 0;
    f->aresolve = JS_UNDEFINED; f->areject = JS_UNDEFINED; f->await_promise = JS_UNDEFINED;
    f->rthis = JS_UNDEFINED; f->rargs = NULL; f->rargc = 0; f->is_async = 0;
    f->sess_ctx = 0;
    g_reg_n++;
    { double w = wfq_weight(val, 0, 0); if (w > g_max_parked) g_max_parked = w; }   /* fresh flow (visits=cpu=0): same ONE policy, no duplicated formula — keeps g_max_parked current for wfq_yield */
    return f;
}
/* Release an async-call flow's owned refs (result-promise resolve fn + a parked await promise). No-op for a
   non-async-call flow (both UNDEFINED), so safe at every Flow free site. */
static void flow_free_async_refs(JSContext *ctx, Flow *f) {
    JS_FreeValue(ctx, f->aresolve); f->aresolve = JS_UNDEFINED;
    JS_FreeValue(ctx, f->areject); f->areject = JS_UNDEFINED;
    JS_FreeValue(ctx, f->await_promise); f->await_promise = JS_UNDEFINED;
    JS_FreeValue(ctx, f->rthis); f->rthis = JS_UNDEFINED;
    if (f->rargs) { for (int i = 0; i < f->rargc; i++) JS_FreeValue(ctx, f->rargs[i]); free(f->rargs); f->rargs = NULL; f->rargc = 0; }
}
/* PENDING ASYNC-RECIPE MAP — cross-session resume for async-call flows. A parked async flow persists as
   ("a" + source-hash + decvec); on resume qjs_begin loads each into this map, and it is attached to the
   re-fired async call by SOURCE HASH at TWO consult sites of ONE map: (1) a post-load SWEEP over flows the
   phase-1 boot already re-created (those calls fired before this map existed, so they cannot consult it at
   call time), and (2) reg_add_async_call for a HANDLER-triggered call that fires in phase 3, after the map
   is live. Both replay the flow's parked await/branch path instead of re-forking from scratch. Unconsumed
   entries (a handler that never re-fires this session) are freed at teardown — never a leak, never lost
   (a never-fired handler's recipe simply waits for the session where it does fire). */
static AsyncRecipe *g_arec = NULL; static int g_arec_n = 0, g_arec_cap = 0;
/* Attach the first UNUSED recipe whose source hash matches this fresh async flow's function. Ownership of
   `dec` transfers to the flow (freed at flow teardown); the map slot is marked used. Returns 1 if attached. */
static int arec_attach(JSContext *ctx, Flow *f) {
    if (!f->is_async || f->dec || !JS_IsFunction(ctx, f->handle)) return 0;
    uint32_t h = JS_OrphanHash(ctx, f->handle);
    for (int i = 0; i < g_arec_n; i++) {
        if (!g_arec[i].used && g_arec[i].hash == h) {   /* an EMPTY decvec (dec=NULL) still attaches: it restores prior val/visits (UCB), exactly as an orphan recipe does */
            f->dec = g_arec[i].dec; f->dec_n = g_arec[i].dec_n; f->val = g_arec[i].val; f->visits = g_arec[i].visits;
            g_arec[i].dec = NULL; g_arec[i].used = 1;
            return 1;
        }
    }
    return 0;
}
static void arec_free(void) {
    for (int i = 0; i < g_arec_n; i++) free(g_arec[i].dec);
    free(g_arec); g_arec = NULL; g_arec_n = g_arec_cap = 0;
}

/* JSAsyncCallHook target: the bundle CALLED a native async function. `fs` is its pre-created live
   JSAsyncFunctionState (real args captured); `resolve` is the result-promise's resolve fn (settled on
   COMPLETION so an `await asyncFn()` caller-flow resumes). Register it as a flow driven from its START via
   JS_FlowResume (f.fs pre-set => dispatched as a resume with an empty delta). So a fire-and-forget async
   recursion (loadPage(d.next)) is a TREE of preemptible/parkable flows, each its own bounded COW delta —
   NOT a non-preemptible promise-reaction drain whose single delta cow-oom-aborts. */
static int reg_add_async_call(JSContext *ctx, void *fs, JSValueConst func_obj, JSValueConst resolve, JSValueConst reject) {
    if (ctx != g_ctx) return 0;                 /* CLAIM only for the MAIN analysis ctx; the @S solve realm (g_solve_ctx)
                                                   runs async native — routing its call into the main g_reg is cross-ctx corruption */
    Flow *f = reg_add(ctx, JS_DupValue(ctx, func_obj), g_running ? g_cur_val : 1.0, NULL, 0);
    f->fs = fs;                                 /* the live async state — the flow owns + frees it (JS_FlowResume) */
    f->is_async = 1;
    f->aresolve = JS_DupValue(ctx, resolve);
    f->areject = JS_DupValue(ctx, reject);
    /* Capture the PRISTINE re-run recipe (this + args, before the call runs) so an await FORK can spawn a
       reject-replay sibling (re-run the same call, forcing one await to reject -> the inline try/catch arm). */
    JSValueConst rthis = JS_UNDEFINED, *rargs = NULL; int rargc = 0;
    JS_FlowRecipe(fs, NULL, &rthis, &rargc, &rargs);
    f->rthis = JS_DupValue(ctx, rthis);
    if (rargc > 0 && rargs) {
        f->rargs = (JSValue *)malloc((size_t)rargc * sizeof(JSValue));
        if (f->rargs) { f->rargc = rargc; for (int i = 0; i < rargc; i++) f->rargs[i] = JS_DupValue(ctx, rargs[i]); }
    }
    arec_attach(ctx, f);   /* resume: a handler-triggered async call (fired after qjs_begin) replays its parked path */
    return 1;
}
/* Spawn a re-run sibling of an async flow: a fresh call state from the RECIPE (func+args), the decision vector
   `dec` (branch + await decisions, ownership transferred to reg_add), is_async set, recipe carried so it can
   fork further. Used by BOTH await_decide (a new await -> reject sibling) and branch_decide (an async flow's
   branch fork) — one recipe re-run path, so branch and await decisions replay together over ONE vector. */
Flow *spawn_async_sibling(JSContext *ctx, Flow *pf, signed char *dec, int dec_n) {
    void *sfs = JS_FlowNew(ctx, pf->handle, pf->rthis, pf->rargc, (JSValueConst *)pf->rargs);
    if (!sfs) { free(dec); return NULL; }
    Flow *sib = reg_add(ctx, JS_DupValue(ctx, pf->handle), g_cur_val, dec, dec_n);
    sib->fs = sfs; sib->is_async = 1;
    sib->rthis = JS_DupValue(ctx, pf->rthis);
    if (pf->rargc > 0 && pf->rargs) {
        sib->rargs = (JSValue *)malloc((size_t)pf->rargc * sizeof(JSValue));
        if (sib->rargs) { sib->rargc = pf->rargc; for (int i = 0; i < pf->rargc; i++) sib->rargs[i] = JS_DupValue(ctx, pf->rargs[i]); }
    }
    return sib;
}
/* JSFlowAwaitHook: a fulfilled await is a GATE on the ONE decision vector (g_dec/g_c), exactly like branch_decide
   at OP_if. Replay the recorded arm, or fork a REJECT sibling (re-run via recipe, this await -> reject = the inline
   try/catch catch arm) and take FULFIL. Returns 1=reject, 0=fulfil. Non-async / non-forking context -> fulfil. */
static int await_decide(JSContext *ctx) {
    if (ctx != g_ctx || !g_running || !g_cur_flow || !g_cur_flow->is_async) return 0;
    if (JS_IsUndefined(g_cur_flow->handle)) return 0;
    if (g_c < g_dec_n) { int arm = g_dec[g_c] ? 1 : 0; g_c++; return arm; }   /* replay this flow's recorded await/branch decisions */
    if (!g_dec_ensure(g_c + 1)) return 0;
    signed char *sib = (signed char *)malloc((size_t)(g_c + 1));   /* fork the REJECT sibling; this flow takes FULFIL */
    if (sib) { for (int i = 0; i < g_c; i++) sib[i] = g_dec[i]; sib[g_c] = 1; spawn_async_sibling(ctx, g_cur_flow, sib, g_c + 1); }
    g_dec[g_c] = 0; g_dec_n = g_c + 1; g_c++;
    return 0;
}
/* JSReactionHook target: a settled promise's REACTION (.then/.catch/.finally handler). Run handler(value) as a
   preemptible/parkable flow and SETTLE the chained promise on completion (resolve with the return value / reject
   on throw) — so a .then-based recursion is a TREE of flows, not a non-preemptible drain. A pass-through reaction
   (no handler) settles the chained promise directly. Non-bytecode handler / non-main-ctx -> native job (return 0). */
static int reaction_flow(JSContext *ctx, JSValueConst handler, JSValueConst value, int is_reject, JSValueConst resolve, JSValueConst reject) {
    if (ctx != g_ctx) return 0;                 /* main analysis ctx only, never the @S solve realm */
    if (JS_IsUndefined(handler)) {              /* pass-through: fulfill->resolve, reject->reject the chained promise with value */
        JSValueConst fn = is_reject ? reject : resolve;
        if (!JS_IsUndefined(fn)) { JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 1, (JSValueConst *)&value); if (JS_IsException(r)) { JSValue e = JS_GetException(ctx); JS_FreeValue(ctx, e); } JS_FreeValue(ctx, r); }
        return 1;
    }
    void *fs = JS_FlowNew(ctx, handler, JS_UNDEFINED, 1, (JSValueConst *)&value);
    if (!fs) return 0;                          /* non-bytecode handler (C fn / bound) -> native job */
    Flow *f = reg_add(ctx, JS_DupValue(ctx, handler), g_running ? g_cur_val : 1.0, NULL, 0);
    f->fs = fs;
    /* HANDLER-TIME ASYNC @S: a reaction spawned while a session fired the handler inherits the session sink-CONTEXT
       (so a sink it reaches enqueues a candidate SESSION) and, if the parent is a candidate replay, the candidate
       (so when it resumes g_candidate is pinned and the sink takes solve_add's VERIFY branch — the awaited value
       IS the candidate). This is what makes addEventListener('message', e=>P.resolve(e.data).then(t=>sink)) solve. */
    f->sess_ctx = g_in_session || (g_cur_flow && g_cur_flow->sess_ctx);
    if (g_cur_flow && g_cur_flow->candidate) f->candidate = strdup(g_cur_flow->candidate);
    f->aresolve = JS_DupValue(ctx, resolve);
    f->areject = JS_DupValue(ctx, reject);
    return 1;
}

/* Re-add a SUSPENDED flow (a full copy, fs retained) so it interleaves back into the ONE registry. */
static int reg_readd(JSContext *ctx, Flow f)
{
    if (g_reg_n >= g_reg_cap) {
        int nc = g_reg_cap ? g_reg_cap * 2 : 256;
        Flow *nr = (Flow *)realloc(g_reg, (size_t)nc * sizeof(Flow));
        CHECK(nr, "reg_oom: flow-registry realloc failed — OOM is a physical floor, never continue with a dropped flow");
        g_reg = nr; g_reg_cap = nc;
    }
    g_reg[g_reg_n++] = f; return 1;
}


/* fromReply: a bridge-provided map { url -> concrete reply body text }. A real GET is fired by the
   TRUSTED offscreen (safeFetch, one-per-endpoint) and its body injected here so r.json()/r.text()
   return the CONCRETE server reply -> a reply field flowing into a downstream request param becomes a
   REAL example value instead of {}. Absent url -> opaque (the honest shape). */
JSValue g_reply_table = JS_UNDEFINED;
/* g_storage + js_storage_get/set + storage_free live in storage.c (localStorage/sessionStorage concolic). */
static JSContext *g_ctx;   /* the run's context (defined = NULL below); fwd-declared for Lexbor callbacks */
/* ENDPOINT REGISTRY + IDENTITY: the engine ACCUMULATES every learned endpoint (method/url/params/
   headers/body) as a JS object here, and at finalize DEDUPS them (exact by method+hole-normalized url,
   then collapses a concrete instance into its shape with a path-param example) and emits the deduped
   set. Identity is the ENGINE's, not the host's (the JS mergeCallsites/dedupShapeConcrete were DELETED).
   The dedup runs on the engine's OWN quickjs (g_dedup_fn) — proven logic, executed in-engine, never a
   host context-switch. */
/* g_endpoints (the @H array) is in endpoint.c — extern via endpoint.h; main.c's finalize reads it. */
static JSValue g_dedup_fn = JS_UNDEFINED;    /* (eps) => deduped array, evaluated once at init */
/* DEDUP_JS (in-engine endpoint dedup at finalize) -> prelude.c */
/* SELF-HOSTED Array.prototype iterators (forEach/map/filter/some/every): the C js_array_every holds
   its loop counter/accumulator on the C STACK across each callback JS_Call, so a fn recursing THROUGH
   the callback (node.children.forEach(walk)) C-recurses and overflows -- the continuation-holding
   re-entry the tail-forward trampoline cannot reach. As bytecode, the loop is a HEAP frame and the
   callback dispatches via the (trampolined) call path, so recursion through any of them is unbounded
   and snapshot/preempt/replay work at any iteration depth (verified to depth 3000). Each is a
   NON-CLOSURE (no IIFE, no captured var_refs, no shared helper -> no global pollution): an init-defined
   CLOSURE does NOT trampoline its deep recursion -- a real engine defect at the pre-boot-baseline/
   var_ref boundary; a non-closure avoids it (init non-closures trampoline, like the pages' boot fns).
   `>>>0` is internal ToUint32 (exact for real arrays; length>2^32 array-likes don't exist in practice).
   Spec-faithful: ToObject(this), HasProperty (`__ain(O,k)`) skips holes, (value,index,array) args, thisArg,
   early-exit for some/every, hole-preserving map. Deviation: map/filter build a plain Array (Symbol.
   species not honored) -- effectively never overridden in real bundles. */
/* ARRAY_PRELUDE_JS (self-hosted Array/String iterators) -> prelude.c */
/* Reply FETCH registration: a reply-consume (r.json()/r.text()) with no cached body registers its (concrete)
   url so qjs_step returns NEED_FETCH; the offscreen safe-fetches + qjs_provide()s the body, which CACHES it and
   enqueues a forking BOOT RE-RUN that re-runs boot with the reply now synchronously concolic (make_response
   injects __body) — the reply is delivered CONCOLIC in a LIVE flow. NO promise is parked: a parked resolve fn
   held across the fetch is persistent async state OUTSIDE the per-flow COW delta (it outlives the flow's revert
   and resolves against torn-down heap). r.json() resolves OPAQUE in place; concrete comes from the re-run. */
typedef struct { char *url; int is_json; } Pending;
static Pending *g_pending = NULL;
static int g_pending_n = 0, g_pending_cap = 0;
void reply_fetch_register(const char *url, int is_json) {
    if (!url) return;
    for (int i = 0; i < g_pending_n; i++) if (g_pending[i].url && strcmp(g_pending[i].url, url) == 0) return;   /* dedup: one fetch per url */
    if (g_pending_n >= g_pending_cap) {
        int nc = g_pending_cap ? g_pending_cap * 2 : 32;
        Pending *n = realloc(g_pending, (size_t)nc * sizeof(Pending));
        if (!n) return;
        g_pending = n; g_pending_cap = nc;
    }
    g_pending[g_pending_n].url = strdup(url); g_pending[g_pending_n].is_json = is_json;
    g_pending_n++;
}
/* Chunk-load pending (M3): an injected <script src> to fetch + eval IN PLACE (no promise; nothing awaits
   it — like the browser's async script load). The offscreen provides the body; qjs_provide evals it. */
static char **g_chunk_pending = NULL;
static int g_chunk_n = 0, g_chunk_cap = 0;
/* Chunks already FETCHED + provided this session: a lazy chunk is a STATIC resource (its bytes never change),
   so it is fetched ONCE — its content is boot_script_cache'd / module-linked and re-run on every boot re-run,
   never re-fetched. Without this, a chunk removed from g_chunk_pending on provision is re-added by the next
   boot re-run's re-injection -> the bridge re-fetches forever (a fetch/provide LIVELOCK). This is the
   one-per-endpoint resource cache CLAUDE.md prescribes, NOT a flow seen-set (it dedups an identical STATIC
   GET, never distinct exploration work). */
static char **g_chunk_done = NULL;
static int g_chunk_done_n = 0, g_chunk_done_cap = 0;
static int chunk_is_done(const char *url) {
    for (int i = 0; i < g_chunk_done_n; i++) if (strcmp(g_chunk_done[i], url) == 0) return 1;
    return 0;
}
static void chunk_mark_done(const char *url) {
    if (!url || chunk_is_done(url)) return;
    if (g_chunk_done_n >= g_chunk_done_cap) { int nc = g_chunk_done_cap ? g_chunk_done_cap * 2 : 16; char **n = realloc(g_chunk_done, (size_t)nc * sizeof(char *)); if (!n) return; g_chunk_done = n; g_chunk_done_cap = nc; }
    g_chunk_done[g_chunk_done_n++] = strdup(url);
}
void chunk_pending_add(const char *url) {
    if (!url) return;
    if (chunk_is_done(url)) return;   /* already fetched this session -> re-run from cache, never re-fetch (kills the provide livelock) */
    for (int i = 0; i < g_chunk_n; i++) if (strcmp(g_chunk_pending[i], url) == 0) return;   /* dedup pending */
    if (g_chunk_n >= g_chunk_cap) { int nc = g_chunk_cap ? g_chunk_cap * 2 : 16; char **n = realloc(g_chunk_pending, (size_t)nc * sizeof(char *)); if (!n) return; g_chunk_pending = n; g_chunk_cap = nc; }
    g_chunk_pending[g_chunk_n++] = strdup(url);
}

/* ---- ESM static+dynamic import graph -> module_loader.{c,h} (modsrc/moddep/pendmod + the quickjs hooks). */

/* wrap val in an already-RESOLVED promise (consumes val) so `await`/`.then` chains continue synchronously. */
JSValue js_resolved(JSContext *ctx, JSValue val)
{
    JSValue rf[2]; JSValue promise = JS_NewPromiseCapability(ctx, rf);
    if (!JS_IsException(promise)) {
        JSValue rr = JS_Call(ctx, rf[0], JS_UNDEFINED, 1, &val); JS_FreeValue(ctx, rr);
        JS_FreeValue(ctx, rf[0]); JS_FreeValue(ctx, rf[1]);
    }
    JS_FreeValue(ctx, val);
    return promise;
}
/* A REJECTED promise (CONSUMES err): the await re-throws err into the continuation so try/catch/.catch runs —
   e.g. Response.json() on a malformed body rejects, never resolves to a fake concolic that hides the throw path. */
JSValue js_rejected(JSContext *ctx, JSValue err)
{
    JSValue rf[2]; JSValue promise = JS_NewPromiseCapability(ctx, rf);
    if (!JS_IsException(promise)) {
        JSValue rr = JS_Call(ctx, rf[1], JS_UNDEFINED, 1, &err); JS_FreeValue(ctx, rr);
        JS_FreeValue(ctx, rf[0]); JS_FreeValue(ctx, rf[1]);
    }
    JS_FreeValue(ctx, err);
    return promise;
}
/* make_response + the Response json()/text()/body accessors + reply-body concolic-wrap are in reply.c. */
/* URL query-parameter extraction (hexval + url_pct_decode + build_query_params) is in url.c (pure). */
JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv);   /* fwd (non-static: xhr.c borrows it) */
/* record_endpoint + g_endpoints (the shared @H sink) are in endpoint.c (included via endpoint.h). */
/* XMLHttpRequest — a PRIMARY request mechanism (many apps use it over fetch). Missing, `new XMLHttpRequest()`
   threw and lost every XHR endpoint. open() stashes method+url; send() emits the endpoint through the shared
   sink (concolic-example URL + query params + body), like fetch. Response fields are opaque (external input). */
/* Capture request header name:value pairs into ep.headers (required-headers replay spec). Reads a plain
   object OR a `new Headers()`'s __fields, and a concolic value's EXAMPLE (`'Bearer '+token` -> the real token).
   ONE home for fetch + XHR (no duplication). */
void capture_headers(JSContext *ctx, JSValueConst ep, JSValueConst hdrs) {
    if (!JS_IsObject(hdrs) || JS_IsConcolic(hdrs)) return;
    JSValue hf = JS_GetPropertyStr(ctx, hdrs, "__fields");
    JSValueConst hsrc = JS_IsObject(hf) ? (JSValueConst)hf : hdrs;
    JSValue hobj = JS_NewObject(ctx); int any = 0;
    JSPropertyEnum *tab = NULL; uint32_t hn = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &hn, hsrc, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
        for (uint32_t hi = 0; hi < hn; hi++) {
            const char *hk = JS_AtomToCString(ctx, tab[hi].atom);
            JSValue hv = JS_GetProperty(ctx, hsrc, tab[hi].atom);
            JSValue hex = JS_IsConcolic(hv) ? JS_ConcolicExample(ctx, hv) : JS_UNDEFINED;
            const char *hvs = !JS_IsUndefined(hex) ? JS_ToCString(ctx, hex) : JS_ToCString(ctx, hv);
            JS_FreeValue(ctx, hex);
            if (hk && hvs) { JS_SetPropertyStr(ctx, hobj, hk, JS_NewString(ctx, hvs)); any = 1; }
            if (hk) JS_FreeCString(ctx, hk);
            if (hvs) JS_FreeCString(ctx, hvs);
            JS_FreeValue(ctx, hv);
        }
        JS_FreePropertyEnum(ctx, tab, hn);
    }
    JS_FreeValue(ctx, hf);
    if (any) JS_SetPropertyStr(ctx, ep, "headers", hobj); else JS_FreeValue(ctx, hobj);
}
/* Observers (Intersection/Mutation/Resize/Performance): missing constructors threw. A no-op object (observe/
   disconnect/etc.) prevents the throw; the callback passed to the constructor is an uncalled function the
   orphan driver reaches, so its endpoints/sinks are still learned. */
/* Observers (Intersection/Mutation/Resize/Performance) -> browser/observer.c (callback = scheduler flow, object opaque). */
/* getComputedStyle + matchMedia (CSSOM environment reads, opaque -> browser/cssom.c). */
/* Intl.NumberFormat/DateTimeFormat/etc.: `new Intl.X().format(v)` threw (not built into this quickjs). A
   constructor returning {format/…} whose results are opaque (locale-formatted external input). */
/* Network-initiating web APIs extracted to Blink-named components (each FEEDS record_endpoint/chunk_pending_add,
   holds no scheduler logic, uses the shared url_from_arg idiom):
     WebSocket + EventSource -> browser/websocket.c   (modules/websockets, modules/eventsource)
     Worker + SharedWorker   -> browser/worker.c      (core/workers)
     navigator.sendBeacon + serviceWorker.register -> browser/navigator.c (core/frame/navigator, modules/service_worker)
     FormData                -> browser/formdata.c    (core/html/forms/FormData) */
/* Intl.* / Notification / Notification.requestPermission / AbortSignal.timeout|any|abort: results genuinely
   unknown headless (locale / OS permission / timer-signal), so they install the shared OPAQUE concolic value
   (opaque.h's js_concolic_stub) directly — any read/method is the honest unknown, .aborted/permission FORK, a
   handler arg is driven. No fixed-shape stub object, no per-one-liner component. */
/* fetch(url): the moat's host edge. URL = whatever the bundle COMPUTED. ACCUMULATE the endpoint record
   (method/url/params/headers/body) into g_endpoints — identity/dedup runs in-engine at finalize — raise
   the flow's value (the WFQ progress signal), and return a resolved promise wrapping the Response. */
/* js_fetch (the fetch() host edge) is in fetch.c (included via fetch.h). */
/* At finalize: DEDUP the accumulated endpoints in-engine (g_dedup_fn) and emit the whole structured
   result as ONE `@RESULT <json>` line (JSON.stringify — correct escaping, single line). The host does
   ONE JSON.parse and relays it: no @H/@P/@HDR/@BODY text protocol, no host-side re-parse, no host
   identity. Sinks/chunks/errors/park are added to the same object by their accumulate sites. */
JSValue g_chunkurls = JS_UNDEFINED;    /* JS array of discovered external <script src> */
static JSValue g_park = JS_UNDEFINED;         /* JS array of "hash,decbits" frontier replay recipes */
void arr_push_str(JSContext *ctx, JSValueConst arr, const char *s) {
    if (!JS_IsArray(arr) || !s) return;
    uint32_t n = 0; JSValue lv = JS_GetPropertyStr(ctx, arr, "length"); JS_ToUint32(ctx, &n, lv); JS_FreeValue(ctx, lv);
    JS_SetPropertyUint32(ctx, arr, n, JS_NewString(ctx, s));
}
/* @WHY IS A FATAL ERROR, NOT A DIAGNOSTIC. A zero-result / gap / unresolved path means the design failed to
   handle something the moat requires. Emitting a @RESULT and CONTINUING would ship DEGRADED output that hides
   the gap behind a logged reason the reviewer ignores — the exact "0 endpoints + 0 errors is the worst output"
   trap, one level up. So every @WHY CRASHES the review WITHOUT continuing: the fix is to eliminate the ROOT so
   it never fires — never log-and-continue, never skip, never delete the check. The design goal is ZERO @WHY. */
/* why_add -> solver/why.{c,h} (the runtime-reasoned @WHY emitter — a tiny component so any subsystem raises it). */
/* with_sinks is a NO-OP retained only for the call-site signature: solve_all is a PURE COLLECTOR of already-
   verified PoCs (g_verified, populated by candidate REPLAY flows in the scheduler — the solving is NOT here),
   so it is side-effect-free and safe to run on EVERY snapshot, incremental or final. @S PoCs are part of the
   ONE continuous frontier and MUST surface incrementally like @H endpoints — never gated behind a teardown a
   live/unbounded engine may never reach (the exact reason a looping XSS page reported zero sinks). */
static void emit_result_ex(JSContext *ctx, int with_sinks) {
    (void)with_sinks;
    if (JS_IsUndefined(g_dedup_fn) || !JS_IsArray(g_endpoints)) return;
    JSValueConst args[1] = { g_endpoints };
    JSValue deduped = JS_Call(ctx, g_dedup_fn, JS_UNDEFINED, 1, args);
    if (JS_IsException(deduped)) { js_std_dump_error(ctx); deduped = JS_NewArray(ctx); }
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "fetchCallSites", deduped);                 /* consumes deduped */
    JS_SetPropertyStr(ctx, result, "securitySinks", solve_all(ctx));   /* @S: COLLECT verified PoCs (pure read of g_verified) — surfaced incrementally, exactly like @H endpoints */
    JS_SetPropertyStr(ctx, result, "chunkUrls", JS_DupValue(ctx, g_chunkurls));
    JS_SetPropertyStr(ctx, result, "_park", JS_DupValue(ctx, g_park));
    JS_SetPropertyStr(ctx, result, "_orphans", JS_NewInt32(ctx, g_orphan_n));
    JS_SetPropertyStr(ctx, result, "_emit", JS_NewInt32(ctx, g_emit_total));
    JS_SetPropertyStr(ctx, result, "_switches", JS_NewInt64(ctx, g_switches));   /* flow interleave events (fairness, incl. at depth) */
    JS_SetPropertyStr(ctx, result, "_work", JS_NewInt64(ctx, g_work));           /* flow dispatches this run (diagnostic) */
    JS_SetPropertyStr(ctx, result, "_parked", JS_NewInt32(ctx, g_park_requested)); /* 1 = host RAM pressure forced a cold-tier park this run */
    JSValue json = JS_JSONStringify(ctx, result, JS_UNDEFINED, JS_UNDEFINED);
    JS_FreeValue(ctx, result);
    if (JS_IsString(json)) { const char *js = JS_ToCString(ctx, json); if (js) { printf("@RESULT %s\n", js); JS_FreeCString(ctx, js); } }
    JS_FreeValue(ctx, json);
    fflush(stdout);
}
static void emit_result(JSContext *ctx) { emit_result_ex(ctx, 1); }   /* teardown: full result incl. @S solve */

/* branch_decide: the decision-vector fork logic (0/1) at a gate on opaque external input. Called by the
   engine's OP_if hook (JS_SetBranchHook) when a branch condition is OPAQUE (real bundles). If the running
   flow's decision vector already fixes this point, replay it. Otherwise it's a NEW branch: FORK a sibling
   flow that re-runs the SAME function, replaying this flow's decisions so far then taking FALSE here; this
   flow takes TRUE (recorded so deeper new branches fork correctly). BFS over the decision tree -> both arms
   of every gate are explored, surfacing the branch-gated (login/flag-gated) endpoints. */
static int g_boot_replay = 0;   /* re-running boot to re-establish shared state for a cross-flow @S candidate */
static int reg_add_boot(JSContext *ctx, signed char *dec, int dec_n);   /* fwd: defined near boot_replay */
static int reg_add_session(JSContext *ctx, signed char *dec, int dec_n); /* fwd: an exploratory session that FORKS */
static int branch_decide(JSContext *ctx, JSValueConst cond)
{
    if (g_boot_replay) return 1;                            /* boot-replay: fixed arm (re-establishing shared state for an @S candidate, no vector to replay) */
    if (!g_running || (JS_IsUndefined(g_cur_fn) && !g_in_boot_flow && !g_in_session)) return 0;   /* meaningful inside a starter flow, a boot flow, OR a session flow (all re-run without a single fn handle) */
    /* value-domain provenance of the condition: cond TRUE means `src <op> tok`; false arm holds the negation. */
    const char *src = NULL, *tok = NULL; int op = JS_ConcolicCmp(cond, &src, &tok);
    const char *jk = JS_ConcolicJKey(cond);   /* method-clean JSON field path of the compared value (for the @S envelope gate-merge) */
    int has = (op != OPCMP_NONE) && src;
    int true_op = op, false_op = opcmp_neg(op);

    if (g_c < g_dec_n) {                                    /* forced replay: take the recorded arm; RE-RECORD its constraint */
        int arm = g_dec[g_c] ? 1 : 0;
        cons_set(g_c, has ? src : NULL, has ? tok : NULL, has ? (arm ? true_op : false_op) : OPCMP_NONE, has ? jk : NULL);
        g_c++; return arm;
    }
    if (!g_dec_ensure(g_c + 1)) return 1;                    /* only RAM/disk (the platform floor) bounds depth — not a cap */

    /* A CANDIDATE session (verifying an @S breakout) takes a fixed arm for a NEW branch — it replays the
       detecting session's vector (above) then follows through with the candidate, it does not re-explore. */
    if (g_in_session && g_candidate) { cons_set(g_c, has ? src : NULL, has ? tok : NULL, has ? true_op : OPCMP_NONE, has ? jk : NULL); g_dec[g_c] = 1; g_dec_n = g_c + 1; g_c++; return 1; }

    /* NEW decision: PRUNE a provably-infeasible arm given the accumulated domain (no phantom @H); else fork. */
    int tf = !has || cons_feasible(src, tok, true_op, g_c);
    int ff = !has || cons_feasible(src, tok, false_op, g_c);
    if (has && !tf && ff) { cons_set(g_c, src, tok, false_op, jk); g_dec[g_c] = 0; g_dec_n = g_c + 1; g_c++; return 0; }   /* TRUE arm impossible */
    if (has && !ff && tf) { cons_set(g_c, src, tok, true_op, jk);  g_dec[g_c] = 1; g_dec_n = g_c + 1; g_c++; return 1; }   /* FALSE arm impossible */

    signed char *sib = (signed char *)malloc((size_t)(g_c + 1));   /* both arms feasible: fork FALSE sibling, take TRUE */
    if (sib) {
        for (int i = 0; i < g_c; i++) sib[i] = g_dec[i];
        sib[g_c] = 0;
        if (g_in_session) reg_add_session(ctx, sib, g_c + 1);      /* an exploratory session forks ANOTHER session (re-fire handlers with the sibling vector) */
        else if (g_in_boot_flow) reg_add_boot(ctx, sib, g_c + 1);  /* a boot flow forks ANOTHER boot flow (re-run boot with the sibling vector) */
        else if (g_cur_flow && g_cur_flow->is_async)               /* an ASYNC flow: re-run via the recipe (func+args) so the awaits — recorded in this SAME g_dec vector — replay too (re-enters catches) */
            spawn_async_sibling(ctx, g_cur_flow, sib, g_c + 1);
        else reg_add(ctx, JS_DupValue(ctx, g_cur_fn), g_cur_val, sib, g_c + 1)->orphan_idx = g_cur_orphan_idx;   /* sibling = same function (same locator), different decisions */
    }
    cons_set(g_c, has ? src : NULL, has ? tok : NULL, has ? true_op : OPCMP_NONE, has ? jk : NULL);
    g_dec[g_c] = 1; g_dec_n = g_c + 1; g_c++;
    return 1;
}
/* Does the CURRENT context fork/decide opaque branches (so an opaque-collection iterator can yield an opaque
   `done` and let the loop-back branch fork/terminate), or is it the MONOLITHIC initial boot where branch_decide
   returns 0 unconditionally (an opaque loop-back cond would resolve 'continue' forever)? Mirrors branch_decide's
   non-zero condition EXACTLY — the single source of truth for "will the branch do something other than fall
   through". */
static int ctx_forks(void) {
    if (g_boot_replay) return 1;                                                   /* fixed-arm replay (exit after 1) */
    if (!g_running) return 0;
    if (JS_IsUndefined(g_cur_fn) && !g_in_boot_flow && !g_in_session) return 0;    /* monolithic boot -> drive-once */
    return 1;
}
/* crypto (getRandomValues/randomUUID/subtle) -> browser/crypto.c. */
/* setTimeout/setInterval/requestAnimationFrame(cb, ...): a deferred callback is NOT a wait on real time —
   it is just another BFS FLOW. Register cb in the ONE scheduler (reg_add) so it is driven, ordered, and
   starved by the same WFQ as every other flow (the whole point: bundles that defer init in a timer still
   get explored). */
/* THE scheduler edge every deferred callback uses: register cb as a first-class BFS FLOW (setTimeout/rAF/
   queueMicrotask/observer/opaque-method-cb) at the running flow's value, ordered + starved by the one WFQ,
   never a real wait. A host-edge only FEEDS this; the scheduler DECIDES. */
void flow_defer_callback(JSContext *ctx, JSValueConst cb) {
    Flow *f = reg_add(ctx, JS_DupValue(ctx, cb), g_running ? g_cur_val : 1.0, NULL, 0);
    /* CONTINUATION: a callback deferred from a running (revert-)flow inherits that flow's live PROPERTY delta,
       so it sees state the flow wrote before deferring (obj.x = tainted; setTimeout(()=>use(obj.x))) — a
       fresh-baseline flow reads it undefined. Closure-var state already rides the callback's own closure, so
       only property writes need this snapshot. At the monolithic boot (g_running=0) nothing is snapshotted:
       boot's writes commit to the baseline, which a boot-deferred flow already sees. */
    if (g_running && g_cur_flow) {
        int n = 0, cap = 0; void *snap = JS_CowBufSnapshot(ctx, &n, &cap);
        if (snap) { f->cow = snap; f->cow_n = n; f->cow_cap = cap; }
        int dn = 0, dcap = 0; void *dsnap = dom_buf_snapshot(&dn, &dcap);   /* + attribute writes (el.dataset.x = tainted) */
        if (dsnap) { f->dom = dsnap; f->dom_n = dn; f->dom_cap = dcap; }
    }
}
/* setTimeout/setInterval/requestAnimationFrame -> browser/timers.c (defers the callback as a flow). */
/* JS_SetCbHook target: a callback passed to a method CALLED ON opaque input (items.forEach(cb) where items
   is a reply/injected-state array) — the method-on-opaque returns opaque without invoking cb, so register cb
   as a starter FLOW (exactly like a deferred timer). The scheduler force-invokes it with opaque args so its
   per-element endpoints/sinks are reached; transient (no orphan_idx), WFQ-ordered/starved like any flow. */
static void drive_opaque_cb(JSContext *ctx, JSValueConst cb, JSValueConst coll) {
    /* Register cb as a starter FLOW. NO seen-set: an unbounded recursion that calls `x.forEach(cb)` per level
       registers cb per level, but every level past the first drives cb with the SAME opaque args -> emits
       nothing new -> the WFQ STARVES those flows to ~0 CPU and RAM-pressure PARKS the tail to the IDB cold
       tier (unbounded-until-disk, the intended design). A dedup keyed by function identity was a BANNED
       seen-set (§NO BOUNDS: "only emitted output — never identity — proves a flow is done") masking the
       recursion as a hang; the cooperative quantum keeps the worker responsive while it starves + parks. */
    flow_defer_callback(ctx, cb);
    /* PROVENANCE: the element `cb` receives is an element OF `coll` (the reply/injected opaque the method was
       called on), so tag it with coll's shape — the starter drives arg0 as {reply} (not a bare {}), keeping
       the collection's taint through f.key etc. A non-opaque/untagged receiver leaves drive_src NULL (default
       g_concolic args). Registered flow is the last one appended by flow_defer_callback. */
    const char *cs = JS_IsConcolic(coll) ? JS_ConcolicShapeC(coll) : NULL;
    if (cs && cs[0] && strcmp(cs, "{}") != 0 && g_reg_n > 0) g_reg[g_reg_n - 1].drive_src = strdup(cs);
}
/* eval / new Function (code-execution @S sinks) + structuredClone -> browser/bindings/global_functions.c. */

/* URL / URLSearchParams: endpoint construction. `new URL(path, base).href|pathname` is how a huge share of
   bundles build request URLs — undefined `URL` = ReferenceError = the endpoint is lost. A CONCRETE input is
   resolved by the REAL vendored LEXBOR URL parser (bind-before-build: existing Lexbor module, never a
   hand-rolled string resolver). An OPAQUE input (external-input-tainted, or a shape with {} holes Lexbor
   can't parse) -> OPAQUE, so its shape flows through untouched (the endpoint is learned as its shape) and
   the tool never concretely decides external input. searchParams.get is OPAQUE (query values = external). */
const char *g_origin;   /* defined below; forward for the URL helpers (non-static: forms.c borrows it) */
/* url_resolve (WHATWG Lexbor URL canonicalization) now lives in browser/url.c — it owns no scheduler state. */
/* Return the OPAQUE sentinel — external input the tool must not concretely decide. The shared handler for
   every "unknown/non-deterministic value" host edge (Math.random, Date.now, crypto.randomUUID, performance.now,
   …); a branch on its result auto-forks BOTH arms via the engine OP_if hook (branch_decide). */
/* js_concolic_read/js_noop/js_concolic_stub are in opaque.c (included via opaque.h). */
/* A CONSTRUCTABLE base class so `class X extends HTMLElement {…}` DEFINES (else it throws and the whole Web
   Component — with its connectedCallback endpoints/sinks — is LOST). super() returns the default derived
   `this`; connectedCallback then becomes an uncalled method the orphan driver reaches like any other. */
static JSValue g_el_proto = JS_UNDEFINED;   /* the element-method proto; custom-element bases chain to it (install_dom_interface_ctors), freed in qjs_teardown */
/* DOM interface base constructors (def_ctor, EventTarget..SVGElement) + the customElements registry ->
   browser/core/dom/custom_elements.c (install_dom_interface_ctors). */
/* addEventListener(type, handler): a registered handler that NEVER FIRES is exactly the unused surface —
   keep it reachable (in g_handlers) so orphan-invoke drives it and surfaces its gated endpoints. */
static JSValue g_handlers = JS_UNDEFINED;
static int g_handler_n = 0;
/* Handlers registered for 'message' (postMessage). Driven with a synthetic MessageEvent whose .data is
   the source-tagged opaque {pm}, so a postMessage-XSS sink reports {pm} and the PoC assembler builds a
   postMessage-delivered PoC. Borrowed refs (also held live in g_handlers). */
static void *g_msg_handlers[128];
static int g_msg_handler_n = 0;
static JSValue g_msg_event = JS_UNDEFINED;   /* synthetic MessageEvent: { data: opaque("{pm}"), origin, source } */
/* Handlers RE-REGISTERED during a candidate boot_replay (addEventListener) — captured so a closure handler
   (which isn't on any global) can be re-resolved to its candidate-closure version. Transient per candidate
   flow; cleared after the drive. */
static JSValue *g_replay_handlers = NULL; static int g_replay_handler_n = 0, g_replay_handler_cap = 0;
static void *g_replay_msg[128]; static int g_replay_msg_n = 0;   /* re-registered 'message' handler ptrs (candidate closure) */
static void replay_handlers_clear(JSContext *ctx) {
    for (int i = 0; i < g_replay_handler_n; i++) JS_FreeValue(ctx, g_replay_handlers[i]);
    g_replay_handler_n = 0; g_replay_msg_n = 0;
}
/* Is `fn` a registered event handler (addEventListener)? Then orphan-driving it must pass a real Event (not a
   bare opaque), else `if(e.preventDefault)`-style shape checks FORK an impossible arm -> a phantom endpoint. */
static int is_handler(JSContext *ctx, JSValueConst fn) {
    if (JS_IsUndefined(g_handlers) || !JS_IsFunction(ctx, fn)) return 0;
    void *p = JS_VALUE_GET_PTR(fn);
    for (int i = 0; i < g_handler_n; i++) {
        JSValue h = JS_GetPropertyUint32(ctx, g_handlers, (uint32_t)i);
        int m = (JS_VALUE_GET_PTR(h) == p); JS_FreeValue(ctx, h);
        if (m) return 1;
    }
    return 0;
}
JSValue js_add_listener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    JSValueConst h0 = (argc >= 2) ? argv[1] : (argc >= 1 ? argv[0] : JS_UNDEFINED);
    if (g_in_boot_flow) return JS_UNDEFINED;   /* boot flow re-run: handlers already registered by the initial boot — don't duplicate g_handlers */
    if (g_boot_replay) {   /* capture the re-registered handler (candidate closure) for re-resolution; don't grow g_handlers */
        if (JS_IsFunction(ctx, h0)) {
            if (g_replay_handler_n >= g_replay_handler_cap) { int nc = g_replay_handler_cap ? g_replay_handler_cap * 2 : 16;
                JSValue *n = realloc(g_replay_handlers, (size_t)nc * sizeof(JSValue)); if (n) { g_replay_handlers = n; g_replay_handler_cap = nc; } }
            if (g_replay_handler_n < g_replay_handler_cap) g_replay_handlers[g_replay_handler_n++] = JS_DupValue(ctx, h0);
            const char *type = argc >= 2 ? JS_ToCString(ctx, argv[0]) : NULL;   /* a re-registered 'message' handler must still be driven with the {pm} event */
            if (type && strcmp(type, "message") == 0 && g_replay_msg_n < 128) g_replay_msg[g_replay_msg_n++] = JS_VALUE_GET_PTR(h0);
            if (type) JS_FreeCString(ctx, type);
        }
        return JS_UNDEFINED;
    }
    JSValueConst h = (argc >= 2) ? argv[1] : (argc >= 1 ? argv[0] : JS_UNDEFINED);
    if (JS_IsFunction(ctx, h) && !JS_IsUndefined(g_handlers)) {
        JS_SetPropertyUint32(ctx, g_handlers, (uint32_t)g_handler_n++, JS_DupValue(ctx, h));
        const char *type = argc >= 2 ? JS_ToCString(ctx, argv[0]) : NULL;   /* addEventListener(type, handler) */
        if (type && strcmp(type, "message") == 0 && g_msg_handler_n < 128)
            g_msg_handlers[g_msg_handler_n++] = JS_VALUE_GET_PTR(h);
        if (type) JS_FreeCString(ctx, type);
    }
    return JS_UNDEFINED;
}
static int is_msg_handler(JSValueConst h) {
    void *p = JS_VALUE_GET_PTR(h);
    for (int i = 0; i < g_msg_handler_n; i++) if (g_msg_handlers[i] == p) return 1;
    for (int i = 0; i < g_replay_msg_n; i++) if (g_replay_msg[i] == p) return 1;   /* re-resolved candidate 'message' closure */
    return 0;
}
/* on<event> content-attribute handlers (js_el_on_set + ON_EVENTS) -> dom_element.c (Element event handlers). */

/* The page's OWN identity (principal) is CONCRETE for URL building — location.origin/protocol/host/
   hostname/port/pathname/href are REAL (a bundle does `location.origin + '/api/...'`; opaque here would
   yield a "{}"-shaped garbage URL and lose the endpoint). Only the EXTERNAL-INPUT parts — search/hash —
   stay OPAQUE (must never force a branch), yet carry a concrete example when page state already has one.
   The host injects the real principal at wire time; g_origin is the node-harness placeholder. */
const char *g_origin   = "https://app.example.com";   /* host-injected real page principal (argv[3]); placeholder for node tests */
/* location.* + external-input source getters + set_origin/make_location -> location.{c,h}. */
/* document.currentScript: the executing <script> element during boot — the CONFIG-INJECTION source (an embed
   reads `document.currentScript.dataset.apiKey` / .getAttribute('data-endpoint')). Set per-script in
   dom_run_scripts, NULL otherwise. */
/* document.currentScript state + getter -> browser/document.c (Blink core/dom/Document); the scheduler boot
   loop feeds it via doc_set_current_script. */

/* ── Real DOM (Lexbor) ───────────────────────────────────────────────────────────
   The page's own structure/config is CONCRETE (a bundle reads `#cfg[data-api]` and builds a real
   URL); only external-input values stay opaque. document.* is backed by a live Lexbor DOM parsed from
   the page HTML, NOT a bridge-side scrape — so it can carry real attribute values now, and become
   per-flow COW state + intercept dynamic script injection (steps C/D). */
lxb_html_document_t *g_dom = NULL;   /* the live parsed document (non-static: dom_select.c reads it) */
/* g_el_class_id + el_wrap -> dom_element.{c,h} (the element JSClass foundation; methods migrate there). */
/* new Image()/Audio()/Option(): missing constructors threw (killing the script). Return a REAL wrapped
   element (its methods/attrs work); .src is stored via the element's src setter — NOT emitted as @H, since
   an image/media load is a STATIC ASSET (magic-byte, not URL), so emitting would pollute with asset noise. */
/* Image/Audio/Option constructors -> browser/media_element.c (correct element per ctor + a real HTMLMediaElement
   state machine for Audio, replacing a shared ctor that wrongly made an <img> for all three). */
/* js_el_querySelectorAll -> dom_element.c. */
/* js_el_rect (getBoundingClientRect) -> dom_element.c. */
/* Event / CustomEvent constructors: `new CustomEvent('x',{detail})` threw when missing. An object carrying
   type + detail (attacker-influenceable in a dispatched event -> detail opaque unless the init provided it). */
/* Event / CustomEvent ctor -> browser/event.c. */
/* DOMParser.parseFromString / Range.createContextualFragment: parse attacker HTML into a subtree that carries
   {parsedhtml} TAINT, so a later appendChild of it into the LIVE DOM is the @S sink (js_el_appendChild). The
   example carries the input (the concrete candidate on a replay flow) so solve_broke_html verifies breakout.
   Non-throwing constructors (these were undefined -> `new DOMParser()` threw, losing all coverage after). */
/* DOMParser.parseFromString + Range.createContextualFragment -> {parsedhtml} taint -> domparser.{c,h}. */
/* DOM ATTRIBUTE SHADOW TAINT: Lexbor stores attribute values as bytes, so an OPAQUE external-input value set
   via setAttribute would be ToString'd -> taint LOST -> a source stashed in a data-attribute and read back
   (getAttribute) in a separate flow goes undetected. Keep a shadow map (element,name)->opaque so an
   OPAQUE/exploration flow reading the attr gets the opaque back (taint preserved, the sink is detected),
   while a CANDIDATE flow reads the REAL concrete attr so the payload flows through the real DOM. Populated at
   boot (baseline); candidate flows never write it (they set the real attr, isolated by the DOM COW delta). */
/* AttrShadow (the DOM attribute taint side-map) is in attr_shadow.c — accessed via attr_shadow_find/set/opaque. */
/* getAttribute (attr-shadow taint round-trip) + querySelector -> dom_element.c. */
/* js_el_textContent (+ SSR-json concolic) -> dom_element.c. */
/* ── Per-flow DOM COW isolation ──────────────────────────────────────────────────
   DOM mutations (attribute set, node insert) by a FORCED flow must not leak to sibling flows — the
   same invariant as the JS-heap COW. A host-side undo log records the inverse of each mutation while
   capture is active (during flow exploration, NOT boot which builds the baseline); dom_revert replays
   it to restore the post-boot DOM baseline, called alongside JS_CowRevert before each starter. */
/* PER-FLOW DOM COW DELTA (mirrors the JS heap delta): `old` = baseline value/absence (kind 0 attr) or the
   inserted node's detach position (kind 1); `cur` = the flow's value, held while parked. Swapped on
   context-switch so a DOM writer interleaves like a heap writer. */

/* el_is_script + script_maybe_load (<script src> lazy-chunk discovery) -> browser/html_script_element.c
   (HTMLScriptElement / ScriptLoader). It feeds the scheduler's chunk queue via extern edges. */
/* resolve_with + dynimport_link + host_dyn_import/host_module_normalize/host_module_loader + pendmod_retry
   -> module_loader.c (the whole ESM subsystem). */
/* appendChild/insertBefore (ContainerNode tree mutation + @S parsedhtml sink + <script src> discovery),
   cloneNode (js_el_self), attachShadow, and el_install_methods (the Element binding install) -> dom_element.c.
   They feed the scheduler via extern edges (solve_add / js_add_listener / script_maybe_load). */
/* js_el_dataset_get (real data-* attrs camelCased) -> dom_element.c. */
/* classList: real bundles branch on `el.classList.contains('active')` constantly; undefined THREW. contains
   checks the REAL class attribute (whitespace-separated tokens) — concrete page structure, like className.
   add/remove/toggle are UI no-ops (not endpoint-relevant). */
/* element.classList (contains/add/remove/toggle) is in classlist.c (included via classlist.h). */
/* DOM TRAVERSAL: parentNode/children/firstElementChild/nextElementSibling were undefined -> `el.children
   .length` / `el.parentNode.x` THREW, killing DOM-walking bundles. Return REAL el_wrap'd element nodes from
   Lexbor so a tree walk that reaches a fetch/sink is explored. children is a REAL array (.length/.forEach/[i]
   all work). Only ELEMENT nodes (text nodes aren't wrapped — a walker keying on .children matches the browser). */
/* DOM traversal getters (parentNode/children/firstElementChild/nextElementSibling) -> dom_element.c. */
/* el_install_methods (the Element interface binding install, Blink-generated from Element.idl) -> dom_element.c. */
/* document.createElement -> browser/document.c (Blink core/dom/Document). */
/* document.write (js_doc_write + script running) is in docwrite.c (included via docwrite.h). */
/* eval(concrete) -> forced-execute (dynamic code path, orphans); eval(external input) stays opaque. */
/* eval + new Function (code-execution @S sink bindings) -> browser/bindings/global_functions.c. */
/* Parse page HTML into the live DOM + init the CSS-selector engine. Returns 0 on success. */
static int dom_init(const char *html, size_t len) {
    g_dom = lxb_html_document_create();
    if (!g_dom) return -1;
    if (html && len && lxb_html_document_parse(g_dom, (const lxb_char_t *)html, len) != LXB_STATUS_OK) return -1;
    return 0;
}

/* Run the document's own scripts (in document order) against the real DOM — the moat runs the page's
   UNMODIFIED bundle, so the ENGINE extracts + executes them, not a bridge-side scrape. Inline <script>
   is eval'd in the global scope; external <script src> is surfaced as @CHUNK for safe-fetch + forced-
   execute (step C — computed/injected src is the same edge). */
/* struct scr_ctx + dom_collect_scripts + script_is_exec + document_bundle_id -> browser/core/loader/
   document_scripts.{c,h} (the document's script inventory + bundle IDENTITY, a pure DOM scan — a COMPONENT, not
   scheduler-monolith state). */
/* boot_script_cache/boot_scripts_run/boot_script_count/boot_scripts_free -> solver/boot_scripts.{c,h} (the
   boot-replay substrate: cached inline <script> texts re-run so a cross-flow @S candidate or a forking boot flow
   re-establishes shared state under a concrete input — a COMPONENT that owns its state, no scheduler globals).
   boot_replay stays here: it only wraps boot_scripts_run with the g_boot_replay branch-mode flag (a scheduler
   concern). Remaining limit (full boot-as-flow): external <script src> chunks aren't re-run. */
static void boot_replay(JSContext *ctx) { g_boot_replay = 1; boot_scripts_run(ctx); g_boot_replay = 0; }
/* Enqueue a BOOT FLOW: re-run boot as a FORKING starter (decision vector), so cached async replies resolve
   synchronously and their continuations' gated branches fork with the concolic example. */
static int reg_add_boot(JSContext *ctx, signed char *dec, int dec_n) {
    reg_add(ctx, JS_UNDEFINED, 1.3, dec, dec_n)->is_boot = 1;   /* reg_add never fails (OOM aborts) */
    return 1;
}
/* An EXPLORATORY attacker-session that FORKS: re-fire ALL handlers over one accumulating delta, replaying
   `dec` then forking new opaque branches. This is how a gate in handler B on state that handler A wrote from
   an OPAQUE source (the canonical "login handler sets auth flag; gated code reads it" SPA pattern) gets its
   admin arm explored — a per-handler orphan flow can't (it's COW-isolated from A's write), and a fixed-arm
   session can't (it never forks). A candidate session (verifying a breakout) stays fixed-arm; only this
   exploratory kind forks. */
static int reg_add_session(JSContext *ctx, signed char *dec, int dec_n) {
    reg_add(ctx, JS_UNDEFINED, 1.2, dec, dec_n)->session = 1;   /* reg_add never fails (OOM aborts) */
    return 1;
}
/* BOOT AS THE FIRST FLOW: the page's boot (its inline scripts) is captured as a COW DELTA — g_boot_delta —
   exactly like any flow (mutations + global CREATIONS). It stays APPLIED between opaque flows (its effects
   are the post-boot baseline). A candidate flow UNAPPLIES it to reach a TRUE pre-boot heap (boot's globals
   deleted, so a guarded init `if(!window.d){window.d=src}` re-fires), re-runs boot under the concrete
   candidate as its OWN delta, drives, then REAPPLIES g_boot_delta so the next opaque flow sees post-boot.
   No host-side property save/delete/restore — the delta IS the mechanism (heap; DOM boot stays baseline). */
static void *g_boot_delta = NULL; static int g_boot_delta_n = 0, g_boot_delta_cap = 0;
static void boot_replay_candidate(JSContext *ctx) {
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
static JSValue resolve_replayed_handler(JSContext *ctx, JSValueConst orig) {
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
static JSValue js_session_fns(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue arr = JS_NewArray(ctx); uint32_t n = 0, hn = 0;
    if (!JS_IsUndefined(g_handlers)) { JSValue lv = JS_GetPropertyStr(ctx, g_handlers, "length"); JS_ToUint32(ctx, &hn, lv); JS_FreeValue(ctx, lv); }
    for (uint32_t i = 0; i < hn; i++) {
        JSValue h = JS_GetPropertyUint32(ctx, g_handlers, i);
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
static JSValue js_session_drain(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSContext *c; while (JS_ExecutePendingJob(g_rt, &c) > 0) {}   /* per-fn microtask drain, parity with the C loop */
    return JS_UNDEFINED;
}
/* eval_page_script + dom_run_scripts -> browser/core/html/html_script_runner.{c,h} (Blink HTMLScriptRunner):
   running the document's <script> elements is a BROWSER component, not scheduler state. */

/* js_is_concolic/js_concolic_example (__isOpaque/__opaqueExample leaf intrinsics) -> solver/opaque.{c,h}. */

/* orphan flow source — CONTINUOUS discovery, NOT a one-shot phase. Called every scheduler iteration so
   functions defined DYNAMICALLY during a forced flow (a login-gated lazy CHUNK: eval/import of fetched
   JS reached only by forcing the auth branch) become orphan flows and get driven -> we learn the
   logged-in surface while logged out. Already-executed fns are not returned by JS_CollectOrphans;
   already-queued fns are dup-skipped. Idempotent: re-running adds only NEW never-executed functions. */
static int seed_orphans(JSContext *ctx)
{
    static JSValue buf[4096];
    int n = JS_CollectOrphans(ctx, buf, 4096), seeded = 0;
    for (int i = 0; i < n; i++) {
        int dup = 0;
        for (int j = 0; j < g_reg_n; j++)
            if (JS_VALUE_GET_PTR(g_reg[j].handle) == JS_VALUE_GET_PTR(buf[i])) { dup = 1; break; }
        /* also skip one already recorded in the stable buffer (queued/parked, not yet run) */
        if (!dup) for (int j = 0; j < g_orphan_n; j++)
            if (JS_VALUE_GET_PTR(g_orphan_buf[j]) == JS_VALUE_GET_PTR(buf[i])) { dup = 1; break; }
        if (dup) { JS_FreeValue(ctx, buf[i]); continue; }
        int idx = -1;
        if (g_orphan_n < 4096) { idx = g_orphan_n; g_orphan_buf[g_orphan_n++] = JS_DupValue(ctx, buf[i]); }  /* buffer owns a ref (stable locator) */
        if (g_resume_mode) { JS_FreeValue(ctx, buf[i]); continue; }   /* resume: build locators only; recipes are seeded explicitly */
        reg_add(ctx, buf[i], 1.0, NULL, 0)->orphan_idx = idx;
        seeded++;
    }
    return seeded;   /* count surfaces in @RESULT._orphans (via g_orphan_n) — no dead @ORPHANS line */
}

/* Per-flow isolation is the engine COW (JS_CowSetActive/JS_CowRevert): shared-state writes (var-refs =
   globals/lexicals/closures; baseline-object property mutations) are captured while a flow explores, and
   reverted to the post-boot BASELINE before the next STARTER runs — so an independent flow never sees
   another's writes, yet a flow sees its OWN writes within its run. */

/* Thin Flow->scalars adapter over the ONE WFQ policy (solver/wfq.c); the order-key formula lives THERE
   (auditable + isolation-testable), this only projects a Flow's accounting into it. */
static double flow_weight(const Flow *f)
{
    return wfq_weight(f->val, f->visits, f->cpu);
}

/* The value-driven yield decision, called by the engine at each loop back-edge of the running TOP flow
   frame. Ticks CPU (accounting, NOT a cap — the flow is resumable, never truncated), then yields iff a
   PARKED flow now outranks the running one. The JS heap AND Lexbor DOM are per-flow COW deltas that swap on
   context-switch (JS_CowBufTake/Load + dom delta), so a flow is preemptible mid-write to EITHER — no writer
   runs to completion, no empty-delta guard. */
static int wfq_yield(void)
{
    if (!g_cur_flow) return 0;
    g_cur_flow->cpu += 1.0;
    /* Both the JS heap AND the Lexbor DOM are per-flow COW deltas that swap on context-switch, so a flow is
       preemptible mid-write to either — no writer runs to completion. */
    if (g_max_parked > flow_weight(g_cur_flow)) return 1;   /* VALUE yield: a parked flow outranks the running one (g_max_parked maintained at dispatch + reg_add) */
    /* COOPERATIVE-QUANTUM yield (orthogonal, thread-sharing): held the worker thread a full wall-clock slice
       -> suspend + return to host, resume identical frontier. Clock sampled every QUANTUM_SAMPLE back-edges. */
    if (++g_quantum_sample >= QUANTUM_SAMPLE) { g_quantum_sample = 0; return quantum_expired(); }
    return 0;
}

/* Emit the remaining frontier as compact REPLAY recipes (orphan_idx + decision-vector) and clear it.
   A parked flow is reconstructed next session by re-running boot + replaying its decisions — never by
   serializing a live continuation. The host persists these lines to IDB; @PARK/@PARKED are read back. */
static void park_frontier(JSContext *ctx)
{
    int parked = 0, unrecipe = 0;
    for (int i = 0; i < g_reg_n; i++) {
        Flow *f = &g_reg[i];
        /* A flow is COLD-PARKABLE iff it self-relocates by FUNCTION SOURCE on the next session. TWO kinds:
           (1) an ORPHAN-derived flow — located among the re-collected orphans by JS_OrphanHash;
           (2) an ASYNC-call flow — its OWN async function re-fires when phase-1 boot re-runs, so the live flow
               is re-created and phase-2 resume re-attaches this decvec by hash (async marker 'a'). The
               unification (await + branch decisions in ONE g_dec) is exactly what makes an async decvec a
               replayable recipe: re-driving the re-fired async call with it reconstructs its await/branch path.
           @S candidate flows are transient (never parked); a boot-triggered async flow persists, a
           handler-triggered one re-explores when its handler re-drives (still never lost, just not skipped-to). */
        uint32_t oh = 0; int async_rec = 0;
        if (!f->candidate && f->orphan_idx >= 0 && f->orphan_idx < g_orphan_n) {
            oh = JS_OrphanHash(ctx, g_orphan_buf[f->orphan_idx]);   /* stable identity, not the positional index */
        } else if (!f->candidate && f->is_async && JS_IsFunction(ctx, f->handle)) {
            oh = JS_OrphanHash(ctx, f->handle);   /* async flow: locate by its OWN function's source */
            async_rec = 1;
        } else if (!f->candidate && JS_IsFunction(ctx, f->handle)) {
            /* CONTINUATION flow (a deferred timer/promise-reaction/forEach-element callback registered with
               orphan_idx=-1): it still has a FUNCTION HANDLE, so locate it by SOURCE next session and drive it
               with its decision vector — never a silent drop. HONEST LIMIT: its inherited COW delta (the handler
               writes JS_CowBufSnapshot captured) is NOT in the recipe, so a cold resume RE-DERIVES the flow from
               scratch (re-run boot + replay decisions) rather than restoring the inherited state — sound (the
               decisions replay over a re-established baseline), only the hot inherited delta is not persisted. */
            oh = JS_OrphanHash(ctx, f->handle);
        }
        if (oh) {
            /* recipe = "[a]hash,decbits,valcenti,visits" in a DYNAMIC buffer. The decision vector is UNBOUNDED:
               a fixed buffer would TRUNCATE a deep flow's decisions -> a wrong (shorter) replay path, a
               hidden depth bound (the cardinal violation). valcenti = accumulated value*100 (the flow's
               emitted-value score) and visits = times scheduled: TOGETHER they are the cold-tier
               frontierWeight estimator (emit-per-VISIT, the guarded rate CLAUDE.md ranks rehydration by),
               and on reseed they restore the UCB explore term so a heavily-explored low-yield recipe
               correctly defers to unproven ones instead of resuming as if brand-new. A leading 'a' marks an
               async recipe (re-attach to a re-fired async call), else an orphan recipe (drive the orphan). */
            size_t cap = 17 + (size_t)(f->dec_n > 0 ? f->dec_n : 0) + 40;
            char *rec = (char *)malloc(cap);
            if (rec) {
                int o = 0;
                if (async_rec) rec[o++] = 'a';
                o += snprintf(rec + o, cap - (size_t)o, "%u,", oh);
                for (int j = 0; j < f->dec_n; j++) rec[o++] = f->dec[j] ? '1' : '0';
                snprintf(rec + o, cap - (size_t)o, ",%d,%d", (int)(f->val * 100.0), f->visits);
                arr_push_str(ctx, g_park, rec);   /* "[a]hash,decbits,valcenti,visits" replay recipe -> @RESULT._park */
                free(rec);
                parked++;
            }
        } else if (!f->candidate && !f->vtarget) {
            /* No source recipe (a session flow drives __driveSession, not a locatable page function; it re-explores
               when its handlers re-fire next session — never SKIPPED-to, but not resumed either). Count it so
               @PARKED is HONEST rather than claiming a clean park while this tail is re-derived from scratch. */
            unrecipe++;
        }
        if (f->fs) JS_FlowFree(g_rt, f->fs);
        if (f->cow) JS_CowBufFree(ctx, f->cow, f->cow_n);   /* free the parked flow's stashed heap COW delta */
        if (f->dom) dom_buf_free(f->dom, f->dom_n);   /* and its DOM delta */
        JS_FreeValue(ctx, f->handle);
        free(f->dec); free(f->candidate); free(f->vtarget); free(f->drive_src); flow_free_async_refs(ctx, f);    }
    g_reg_n = 0;
    /* HONEST observability: `parked` = flows with a source-identity replay recipe (resumed next session);
       `unrecipe` = the recipe-less tail (session flows) that re-explore from scratch when their handlers re-fire.
       Reporting both is the truth — the old comment claimed "never a silent drop" while dropping this tail. */
    printf("@PARKED %d %d\n", parked, unrecipe); fflush(stdout);
}

/* A BOOT FLOW is pending (reply-triggered forking re-run or a boot-fork sibling) — a RAM-pressure park must
   not drop it: it delivers an already-fetched reply's gated surface and has no replay recipe if dropped. */
static int reg_has_boot(void) {
    for (int i = 0; i < g_reg_n; i++) if (g_reg[i].is_boot) return 1;
    return 0;
}
/* The ONE scheduler loop: pick the highest-WEIGHT flow (NON-FIFO), run it as a preemptible heap-frame
   flow (per-opcode yield, no slice count), re-queue it if it suspended (interleave), repeat. BFS by value-of-information: shallow
   high-emit flows finish ahead of the deep residue, which is starved to ~0 CPU (resumable). */
static void scheduler_run(JSContext *ctx)
{
    g_quantum_start = emscripten_get_now();   /* fresh cooperative slice per scheduler_run (per qjs_step) */
    for (;;) {
        seed_orphans(ctx);          /* CONTINUOUS: pick up functions a prior flow defined dynamically (chunks) */
        if (g_reg_n == 0) break;
        /* COOPERATIVE-QUANTUM RETURN (§NO BOUNDS: a thread-yield, NOT a cap): held the worker thread a full
           wall-clock slice -> return to qjs_step (which reports HOT work remains) so the host loop pumps the
           worker's message queue / interleaves other engines / streams findings, then re-enters and RESUMES the
           byte-identical frontier. Distinct from the value yield-floor (which flow) and RAM park (evict tail):
           this is purely WHEN to let the one thread breathe. wfq_yield already suspended the running flow on the
           same signal, so here we just stop re-dispatching and hand control back. */
        if (quantum_expired()) break;
        /* COLD-TIER PARK (cross-page/cross-session fairness) = RESOURCE PRESSURE on ALL work, not just
           starters: on RAM PRESSURE (host-driven, RESOURCE-based — NEVER a dispatch count; a per-N slice is
           the banned step-cap the value yield-floor below already avoids), PARK the rest of the frontier as replay
           recipes (orphan_idx + decision-vector) to the IDB cold tier and stop — resumable next burst/session.
           The host sets g_park_requested when resident RAM crosses the working-set floor. With RAM headroom (a
           lone engine, a small page) it NEVER fires, so the page runs to COMPLETION in one visit — findings are
           never lost to a clock. Not a bound: parked flows re-drive by re-running boot + decisions, and BFS has
           already extracted the productive breadth, so pressure only ever touches the least-valuable starved tail.
           EXCEPTION — never park while a BOOT FLOW is pending: a reply-triggered forking boot (reg_add_boot on
           a NEW reply) is the DELIVERY of an already-fetched reply — the moat's headline gated/logged-in surface
           — and it carries NO orphan_idx, so park_frontier would DROP it with no recipe (permanently lost, not
           deferred). The WFQ, not a clock, orders the boot-fork tree; it drains (finite reply-gated branches). */
        if (g_park_requested && !reg_has_boot()) { park_frontier(ctx); break; }
        int best = -1;
        for (int i = 0; i < g_reg_n; i++) {
            /* an async-call flow still PARKED on a pending await is not runnable: skip it until its promise
               settles (a runnable flow — the callee it awaits — completes + resolves it). Its RESOLVED value
               is delivered on the next dispatch (JS_FlowResumeInject). */
            if (!JS_IsUndefined(g_reg[i].await_promise) && JS_PromiseState(ctx, g_reg[i].await_promise) == JS_PROMISE_PENDING) continue;
            if (best < 0 || flow_weight(&g_reg[i]) > flow_weight(&g_reg[best])) best = i;
        }
        if (best < 0) break;   /* every remaining flow is parked on a pending await (a genuine cycle) -> nothing runnable */
        /* HOST-LEVEL VALUE YIELD (cross-document fairness): yield HOT to the host WFQ the moment this engine's
           BEST flow no longer outranks the runner-up ENGINE (weight set by the host in g_yield_floor) — the
           frontier stays in g_reg, the host re-ranks all live engines + the top cold recipe and resumes the
           winner. VALUE-driven, NOT a dispatch count (a per-N slice is a banned step-cap). g_made_progress
           guards against a zero-work ping-pong at the boundary. */
        if (g_made_progress && flow_weight(&g_reg[best]) <= g_yield_floor) break;
        Flow f = g_reg[best];                       /* f is a STABLE COPY: reg_add during the run may realloc g_reg */
        g_reg_n--; g_reg[best] = g_reg[g_reg_n];    /* swap-remove */
        /* REDUNDANT @S candidate: another candidate already broke out this sink (in g_verified) -> skip it,
           saving a full boot-replay/bundle re-run. Not a bound: the sink IS solved; the work is duplicate. */
        if (f.vtarget && solve_is_verified(ctx, f.vtarget)) { JS_FreeValue(ctx, f.handle); free(f.dec); free(f.candidate); free(f.vtarget); free(f.drive_src); continue; }
        f.visits++;
        g_work++;                                   /* one flow got CPU (starter OR resume) — diagnostic tally */
        g_made_progress = 1;                        /* dispatched a flow this visit (progress guard, not a cap) */
        g_cur_orphan_idx = f.orphan_idx;            /* siblings forked during this flow inherit its locator */
        g_running = 1; g_cur_val = f.val; g_cur_flow = &f;
        /* recompute g_max_parked over the PARKED flows (g_reg now excludes the running flow, swap-removed above);
           reg_add keeps it current as this flow forks. O(N) once per dispatch (the pick already scanned O(N)),
           so wfq_yield stays O(1) per opcode. */
        g_max_parked = -1e300;
        for (int i = 0; i < g_reg_n; i++) { double w = flow_weight(&g_reg[i]); if (w > g_max_parked) g_max_parked = w; }
        g_candidate = f.candidate;   /* @S replay flow: source getters return this concrete candidate (else NULL=opaque) */
        g_in_session = f.session || f.sess_ctx;   /* session RUN-MODE, or an inherited session sink-CONTEXT (a reaction spawned mid-session): either way a sink here enqueues a candidate SESSION */

        if (f.session) {
            /* ATTACKER SESSION as a PREEMPTIBLE FLOW: __driveSession (bytecode) fires all handlers in seed
               order over ONE accumulating COW delta (cross-handler shared state). Run as a heap-frame flow so
               it YIELDS per-opcode like any other — run-to-completion is GONE, candidate sessions included: a
               candidate's boot-undo now lives IN the flow delta (JS_CowSeedBootInverse), so a suspend's
               JS_CowUnapply restores the post-boot baseline with no host-side bracket. On suspend the
               accumulated delta unapplies/reapplies exactly like a sync flow's, so handler A's tainted write
               still reaches B after an interleave. */
            g_cur_fn = JS_UNDEFINED;
            int sess_starter = (f.fs == NULL);
            if (sess_starter) {
                g_dec_n = f.dec_n; g_dec_ensure(g_dec_n);                 /* replay this session's decisions; new opaque branches fork siblings */
                for (int i = 0; i < g_dec_n; i++) g_dec[i] = f.dec ? f.dec[i] : 0;
                g_c = 0; cons_reset();
                if (f.candidate) boot_replay_candidate(ctx);
                JSValue gg = JS_GetGlobalObject(ctx);
                JSValue ds = JS_GetPropertyStr(ctx, gg, "__driveSession");
                f.fs = JS_FlowNew(ctx, ds, JS_UNDEFINED, 0, NULL);        /* preemptible frame for the self-hosted session loop */
                JS_FreeValue(ctx, ds); JS_FreeValue(ctx, gg);
                replay_handlers_clear(ctx);                              /* candidate boot-replay's transient listeners done (session fires g_handlers) */
                if (!f.fs) why_add(ctx, "session", "__driveSession not flow-able (alloc failure) — FATAL");   /* @WHY = crash: __driveSession is guaranteed bytecode, so NULL is a real (OOM) error to surface, never skip */
            } else {                                                     /* RESUME: reload decisions + swap in this session's OWN accumulated delta */
                g_dec_n = f.dec_n; g_dec_ensure(g_dec_n);
                for (int i = 0; i < g_dec_n; i++) g_dec[i] = f.dec ? f.dec[i] : 0;
                g_c = f.saved_c; cons_reset();
                JS_CowBufLoad(f.cow, f.cow_n, f.cow_cap); JS_CowApply(ctx); f.cow = NULL; f.cow_n = f.cow_cap = 0;
                dom_buf_load(f.dom, f.dom_n, f.dom_cap); dom_apply(); f.dom = NULL; f.dom_n = f.dom_cap = 0;
            }
            JS_SetFlowYieldHook(wfq_yield);
            JSValue out = JS_UNDEFINED;
            int st = JS_FlowResume(ctx, f.fs, &out);
            JS_SetFlowYieldHook(NULL);
            JSContext *c1; int jr; while ((jr = JS_ExecutePendingJob(g_rt, &c1)) > 0) { }
            if (jr < 0) js_std_dump_error(c1 ? c1 : ctx);
            g_running = 0; g_cur_fn = JS_UNDEFINED;
            if (st == 1) {                                               /* SUSPENDED: stash accumulated delta + re-queue (interleave with other flows) */
                g_switches++;
                JS_CowUnapply(ctx); f.cow = JS_CowBufTake(&f.cow_n, &f.cow_cap);
                dom_unapply(); f.dom = dom_buf_take(&f.dom_n, &f.dom_cap);
                f.saved_c = g_c; f.val = g_cur_val;
                if (g_dec_n > f.dec_n) {
                    signed char *nd = (signed char *)malloc((size_t)(g_dec_n > 0 ? g_dec_n : 1));
                    if (nd) { for (int i = 0; i < g_dec_n; i++) nd[i] = g_dec[i]; free(f.dec); f.dec = nd; }
                } else {
                    for (int i = 0; i < g_dec_n && f.dec; i++) f.dec[i] = g_dec[i];
                }
                f.dec_n = g_dec_n;
                g_cur_flow = NULL;
                reg_readd(ctx, f);
            } else {                                                     /* COMPLETED: revert this session's delta (boot-inverse included -> post-boot baseline) */
                JS_CowRevert(ctx); { int cn, cc; void *cb = JS_CowBufTake(&cn, &cc); JS_CowBufFree(ctx, cb, cn); }
                dom_revert(); { int dn, dc; void *db = dom_buf_take(&dn, &dc); dom_buf_free(db, dn); }
                if (JS_IsException(out)) js_std_dump_error(ctx);
                JS_FreeValue(ctx, out);
                g_cur_flow = NULL;
                JS_FreeValue(ctx, f.handle); free(f.dec); free(f.candidate); free(f.vtarget); free(f.drive_src);            }
            g_candidate = NULL;
            continue;
        }

        if (f.is_boot) {
            /* BOOT FLOW: re-run boot from the PRISTINE pre-boot baseline as a FORKING starter. Cached replies
               resolve synchronously (make_response injects __body), so a reply-consuming continuation runs
               IN-LINE and its gated branches FORK with the concolic example (unreachable via the non-forking
               promise-resume). The boot-undo lives IN the flow delta (JS_CowSeedBootInverse — the SAME primitive
               candidate flows use), so JS_CowRevert restores the post-boot baseline with NO host-side bracket:
               one uniform COW-delta mechanism, and g_boot_delta stays the canonical baseline (only READ). */
            g_cur_fn = JS_UNDEFINED;
            g_dec_n = f.dec_n; g_dec_ensure(g_dec_n);
            for (int i = 0; i < g_dec_n; i++) g_dec[i] = f.dec ? f.dec[i] : 0;
            g_c = 0; cons_reset();
            JS_SetFlowYieldHook(NULL);
            if (g_boot_delta) JS_CowSeedBootInverse(ctx, g_boot_delta, g_boot_delta_n);   /* seed flow delta with boot-inverse; heap -> pre-boot (globals incl let/const deleted), RECORDED */
            g_in_boot_flow = 1;
            boot_scripts_run(ctx);                                        /* re-run boot, FORKING; cached replies resolve sync */
            { JSContext *cb; int jr; while ((jr = JS_ExecutePendingJob(g_rt, &cb)) > 0) { } if (jr < 0) js_std_dump_error(cb ? cb : ctx); }   /* drain continuations (they fork too) */
            g_in_boot_flow = 0;
            JS_CowRevert(ctx); { int cn, cc; void *cb = JS_CowBufTake(&cn, &cc); JS_CowBufFree(ctx, cb, cn); }   /* revert boot-inverse + re-run writes -> post-boot baseline */
            dom_revert(); { int dn, dc; void *db = dom_buf_take(&dn, &dc); dom_buf_free(db, dn); }
            g_running = 0; g_cur_fn = JS_UNDEFINED; g_cur_flow = NULL;
            JS_FreeValue(ctx, f.handle); free(f.dec); free(f.candidate); free(f.vtarget); free(f.drive_src);            continue;
        }

        /* SYNC flow: load its per-flow scheduler state (decision vector + branch cursor). __branch
           consumes/extends g_dec + forks siblings; force-invoke with OPAQUE this+args (external input
           the tool must not concretely decide) so gates fork and computed URLs are shaped. */
        g_cur_fn = f.handle;
        g_dec_n = f.dec_n; g_dec_ensure(g_dec_n);
        for (int i = 0; i < g_dec_n; i++) g_dec[i] = f.dec ? f.dec[i] : 0;
        g_c = f.saved_c;
        cons_reset();   /* rebuild the value domain as this flow re-sees its branch conditions (starter = full; resume = from saved_c) */

        int is_starter = (f.fs == NULL);
        if (is_starter) {                           /* STARTER: fresh heap frame + empty heap/DOM deltas (both left NULL by the previous flow's exit) */
            /* @S CROSS-FLOW: a candidate flow re-runs boot with the concrete candidate pinned, so shared
               state a handler reads (window.x = location.hash set at boot) holds the candidate, not the
               baseline opaque. boot_replay_candidate SEEDS this flow's delta with the boot-inverse (heap ->
               pre-boot so a guarded init re-fires) then replays boot under the candidate — all in the flow's
               OWN delta, so a suspend/revert restores the post-boot baseline with no host-side bracket. */
            if (f.candidate) boot_replay_candidate(ctx);
            /* CONTINUATION starter: a callback deferred from a handler carries an INHERITED property delta
               (JS_CowBufSnapshot at defer time) — load+apply it so the callback sees the handler's writes.
               Mutually exclusive with a candidate flow (which seeds its own boot-inverse delta). */
            else if (f.cow || f.dom) {
                if (f.cow) { JS_CowBufLoad(f.cow, f.cow_n, f.cow_cap); JS_CowApply(ctx); f.cow = NULL; f.cow_n = f.cow_cap = 0; }
                if (f.dom) { dom_buf_load(f.dom, f.dom_n, f.dom_cap); dom_apply(); f.dom = NULL; f.dom_n = f.dom_cap = 0; }
            }
            /* CLOSURE cross-flow: for a candidate flow, drive the handler boot_replay RE-CREATED (candidate
               closure), located by source identity — else the ORIGINAL f.handle (baseline closure) is driven
               and the candidate never reaches a closure-captured source. Non-candidate flows drive f.handle. */
            JSValue drive = f.handle, resolved = JS_UNDEFINED;
            if (f.candidate) { resolved = resolve_replayed_handler(ctx, f.handle); if (!JS_IsUndefined(resolved)) drive = resolved; }
            /* Each orphan ARGUMENT is DISTINCT external input, so it gets its OWN source identity ({arg0}..{arg7})
               — NOT the shared source-less g_concolic. Sharing one source-less value across args aliased them in the
               per-flow constraint tracker: `function(a,b){ if(a=='x' && b=='y') sink }` recorded ==x and ==y on the
               SAME (empty) source, a false contradiction that PRUNED the sink arm. Distinct sources let each arg's
               gate tokens accumulate + reverse independently (the per-flow constraints a PoC is built from). Owned
               -> freed after JS_FlowNew dups them into the frame. */
            JSValue oargs[8]; for (int i = 0; i < 8; i++) { char s[16]; snprintf(s, sizeof s, "{arg%d}", i); oargs[i] = JS_NewConcolicSourced(ctx, s, s); }
            /* OPAQUE-COLLECTION element: an items.forEach(cb) callback's element carries the collection's
               provenance ({reply}), not a bare {arg0} — so f.key keeps the reply taint. */
            JSValue elem = JS_UNDEFINED, ev = JS_UNDEFINED;
            if (f.drive_src) { elem = JS_NewConcolicSourced(ctx, f.drive_src, f.drive_src); JS_FreeValue(ctx, oargs[0]); oargs[0] = elem; }
            /* A 'message' listener's first arg is a MessageEvent whose .data is attacker-controlled
               (postMessage): drive it with the {pm} source-tagged event so a sink reaching e.data reports
               {pm} and the PoC assembler builds a postMessage-delivered PoC. (g_msg_event is BORROWED.) */
            if (!JS_IsUndefined(g_msg_event) && is_msg_handler(drive)) { JS_FreeValue(ctx, oargs[0]); oargs[0] = g_msg_event; }
            else if (!f.drive_src && is_handler(ctx, drive)) { ev = js_event_ctor(ctx, JS_UNDEFINED, 0, NULL); JS_FreeValue(ctx, oargs[0]); oargs[0] = ev; }   /* a non-message handler gets a REAL Event (type/target/preventDefault…), not opaque -> no phantom shape-check arm */
            /* Drive an orphan METHOD with its REAL receiver instance if one exists (this.field -> concrete
               boot value, a real example) else a SOURCED {this} — external input, NOT the shared source-less
               g_concolic, so a `this=='x'` gate token is attributed + reversible (source-at-creation, like args). */
            JSValue recv = JS_FindReceiver(ctx, drive);
            JSValue this_own = JS_IsUndefined(recv) ? JS_NewConcolicSourced(ctx, "{this}", "{this}") : JS_UNDEFINED;
            JSValue this_val = JS_IsUndefined(recv) ? this_own : recv;
            f.fs = JS_FlowNew(ctx, drive, this_val, 8, oargs);   /* async funcs included now — JS_FlowNew accepts them */
            JS_FreeValue(ctx, this_own);   /* JS_FlowNew dup'd it; UNDEFINED (a real receiver) frees to noop */
            JS_FreeValue(ctx, recv);
            for (int i = 0; i < 8; i++) if (JS_VALUE_GET_PTR(oargs[i]) != JS_VALUE_GET_PTR(g_msg_event)) JS_FreeValue(ctx, oargs[i]);   /* free the owned {argN}/elem/ev; skip the borrowed shared g_msg_event */
            JS_FreeValue(ctx, resolved); replay_handlers_clear(ctx);   /* JS_FlowNew dup'd the drive handle; drop our resolved ref + transient replay handlers */
        }
        /* RESUME: swap in the parked flow's OWN heap COW delta (unapplied while it slept) so it sees its own
           shared-state writes again, not another flow's. Starters begin with an empty delta (globals NULL). */
        if (!is_starter) {
            JS_CowBufLoad(f.cow, f.cow_n, f.cow_cap); JS_CowApply(ctx); f.cow = NULL; f.cow_n = f.cow_cap = 0;
            dom_buf_load(f.dom, f.dom_n, f.dom_cap); dom_apply(); f.dom = NULL; f.dom_n = f.dom_cap = 0;
        }

        /* EVERY sync flow is preemptible mid heap-write (per-flow COW delta), CANDIDATE flows included: the
           candidate boot-undo lives in the flow delta (JS_CowSeedBootInverse), so a suspend's JS_CowUnapply
           restores the post-boot baseline — no host-side bracket to straddle. */
        JS_SetFlowYieldHook(wfq_yield);
        JSValue out = JS_UNDEFINED;
        int st;
        if (!JS_IsUndefined(f.await_promise)) {
            /* Resume an async-call flow whose awaited promise has now SETTLED (the pick loop only selects a
               runnable/settled flow): inject the settled value (or throw the rejection) and drive. */
            JSValue ap = f.await_promise; f.await_promise = JS_UNDEFINED;
            int rej = (JS_PromiseState(ctx, ap) == JS_PROMISE_REJECTED);
            JSValue av = JS_PromiseResult(ctx, ap);
            JS_FreeValue(ctx, ap);
            st = JS_FlowResumeInject(ctx, f.fs, av, rej, &out);
            JS_FreeValue(ctx, av);
        } else {
            st = JS_FlowResume(ctx, f.fs, &out);
        }
        JS_SetFlowYieldHook(NULL);
        JSContext *c1; int jr;
        while ((jr = JS_ExecutePendingJob(g_rt, &c1)) > 0) { }
        if (jr < 0) js_std_dump_error(c1 ? c1 : ctx);
        g_running = 0; g_cur_fn = JS_UNDEFINED;

        if (st == 1) {
            /* SUSPENDED: UNAPPLY this flow's heap writes (baseline restored for the next flow) and STASH its
               delta buffer; re-queue. On resume it re-applies. Interleaving of heap-writers is now sound. */
            g_switches++;   /* one flow was preempted mid-run -> a real context switch (interleave), MEASURED */
            JS_CowUnapply(ctx);
            f.cow = JS_CowBufTake(&f.cow_n, &f.cow_cap);
            dom_unapply();
            f.dom = dom_buf_take(&f.dom_n, &f.dom_cap);
            f.saved_c = g_c;
            f.val = g_cur_val;
            if (g_dec_n > f.dec_n) {                /* grew (new branch decisions taken this flow-run) */
                signed char *nd = (signed char *)malloc((size_t)(g_dec_n > 0 ? g_dec_n : 1));
                if (nd) { for (int i = 0; i < g_dec_n; i++) nd[i] = g_dec[i]; free(f.dec); f.dec = nd; }
            } else {
                for (int i = 0; i < g_dec_n && f.dec; i++) f.dec[i] = g_dec[i];
            }
            f.dec_n = g_dec_n;
            g_cur_flow = NULL;
            reg_readd(ctx, f);
        } else if (st == 2) {
            /* PENDING await (an async-call flow awaited a still-unsettled promise `out`): STASH its delta like a
               suspend + record the promise; the pick loop deprioritizes it until settled, then it resumes via
               JS_FlowResumeInject. (An unawaited or already-resolved await never parks — settled inline.) */
            g_switches++;
            JS_CowUnapply(ctx); f.cow = JS_CowBufTake(&f.cow_n, &f.cow_cap);
            dom_unapply(); f.dom = dom_buf_take(&f.dom_n, &f.dom_cap);
            f.saved_c = g_c; f.val = g_cur_val;
            if (g_dec_n > f.dec_n) { signed char *nd = (signed char *)malloc((size_t)(g_dec_n > 0 ? g_dec_n : 1)); if (nd) { for (int i = 0; i < g_dec_n; i++) nd[i] = g_dec[i]; free(f.dec); f.dec = nd; } }
            else { for (int i = 0; i < g_dec_n && f.dec; i++) f.dec[i] = g_dec[i]; }
            f.dec_n = g_dec_n;
            f.await_promise = out;   /* the pending promise (ownership transferred; out NOT freed here) */
            g_cur_flow = NULL;
            reg_readd(ctx, f);
        } else {
            /* COMPLETED/error: discard this flow's heap writes (restore baseline) + free its delta buffer. */
            JS_CowRevert(ctx);
            { int cn, cc; void *cb = JS_CowBufTake(&cn, &cc); JS_CowBufFree(ctx, cb, cn); }
            dom_revert();
            { int dn, dc; void *db = dom_buf_take(&dn, &dc); dom_buf_free(db, dn); }
            if (!JS_IsUndefined(f.aresolve)) {
                /* async-call flow: SETTLE its result promise so an awaiting caller-flow resumes — REJECT on a
                   thrown completion (st==3, out=exception value) so the awaiter re-throws (try/catch/.catch runs),
                   else RESOLVE with the return value (st==0). */
                JSValue fn = (st == 3) ? f.areject : f.aresolve;
                if (!JS_IsUndefined(fn)) { JSValue rr = JS_Call(ctx, fn, JS_UNDEFINED, 1, (JSValueConst *)&out); if (JS_IsException(rr)) { JSValue e = JS_GetException(ctx); JS_FreeValue(ctx, e); } JS_FreeValue(ctx, rr); }
            } else if (JS_IsException(out)) {
                js_std_dump_error(ctx);   /* a non-async (sync orphan/boot) flow threw on opaque input — the exploration surface */
            }
            JS_FreeValue(ctx, out);
            g_cur_flow = NULL;
            JS_FreeValue(ctx, f.handle); free(f.dec); free(f.candidate); free(f.vtarget); free(f.drive_src);   /* candidate owned by a completed replay flow */
            flow_free_async_refs(ctx, &f);
        }
        g_candidate = NULL;   /* clear the replay candidate; a suspended flow re-sets it from f.candidate on resume */
    }
    JS_SetFlowYieldHook(NULL);
}

/* ── Persistent-instance protocol ─────────────────────────────────────────────────
   ONE wasm instance per page, driven in steps by the offscreen: qjs_init (build runtime + env + boot +
   seed the frontier), qjs_step (advance the ONE scheduler), qjs_teardown. This replaces the old
   re-instantiate-and-re-run-per-pass model so chunks/fromReply/frontier become ONE continuous run
   (in-place suspend/resume) instead of re-running the whole page. */
static JSContext *g_ctx = NULL;
static int g_rc = 0;

KEEP int qjs_init(const char *boot, const char *html, const char *origin,
                  const char *replies, const char *csp)
{
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) { fprintf(stderr, "@E {\"phase\":\"newruntime\"}\n"); return 1; }
    g_rt = rt;
#if APICLIENT_DEV
    JS_SetDumpFlags(rt, JS_DUMP_LEAKS);   /* dev: a refcount/GC leak dumps the surviving objects (with concolic triples) at teardown before the abort — offensive-programming LOUD+informative, only fires on an actual leak */
#endif
    JS_SetMaxStackSize(rt, 4 * 1024 * 1024);
    JS_UpdateStackTop(rt);
    js_std_init_handlers(rt);
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { fprintf(stderr, "@E {\"phase\":\"newcontext\"}\n"); JS_FreeRuntime(rt); return 1; }
    g_ctx = ctx;
    dom_cow_set_ctx(ctx);   /* the DOM delta needs a ctx to dup/free the per-flow attr TAINT shadow it now carries */
    /* Reset all scheduler/frontier state so ONE wasm instance can serve many page analyses (init/run/
       teardown reused) with no cross-page bleed. The arrays themselves are reused (not re-malloc'd). */
    g_reg_n = 0; g_work = 0; g_switches = 0; g_yield_floor = -1e300; g_made_progress = 0; g_emit_total = 0; g_running = 0; g_cur_flow = NULL; g_msg_handler_n = 0;
    g_cur_orphan_idx = -1; g_dec_n = 0; g_c = 0; g_resume_mode = 0; g_park_requested = 0;
    arec_free();   /* fresh session: drop any async recipes a prior teardown missed (idempotent) */
    g_pending_n = 0; g_chunk_n = 0; g_orphan_n = 0; g_dom_capture = 0;
    for (int i = 0; i < g_chunk_done_n; i++) free(g_chunk_done[i]);
    g_chunk_done_n = 0;   /* fresh session: the fetched-chunk cache is per-session (a new visit re-fetches the current bytes) */
    JS_OptaintReset(ctx);   /* clear cross-flow opaque-taint set from any prior page analysis */
    tt_reset();             /* clear observed Trusted-Types policy state from the prior document */
    /* ENDPOINT/@S/etc. accumulators + the in-engine dedup fn — the engine builds the whole structured
       result and emits ONE @RESULT json at finalize (the host JSON.parses it; no host-side parse/identity). */
    endpoint_init(ctx); g_chunkurls = JS_NewArray(ctx);
    g_park = JS_NewArray(ctx);
    solve_init(ctx);   /* @S accumulators (tasks/verified/reached/enqueued + their CowExempt) -> solve.c */
    g_solve_ctx = JS_NewContext(rt);   /* fresh CLEAN realm for the @S solver's candidate eval (no forced-exec/opaque overrides) */
    /* PLATFORM BUILTINS: everything compiled here (x9, dedup, the Array/String prelude) is engine-internal, not
       page code — mark it born-executed so orphan-collection NEVER force-invokes it. Without this, an uncalled
       prelude method (e.g. Array.sort, which unlike forEach permits an undefined comparator) is orphan-driven
       with OPAQUE args and its `k<L`/`cmp?` branches fork-explode. Page bundles compile with the flag OFF. */
    JS_SetBuiltinCompile(ctx, 1);
    if (g_solve_ctx) {
        /* X9 = the fire-tracker; __u = a UNIVERSAL no-op stub (a callable/indexable Proxy that answers EVERY
           name) so a handler run under `with(__u){…}` never throws on an app function undefined in this clean
           realm (`log('')` -> no-op) — completeness — while an X9 in an EXECUTABLE position still calls the
           tracker and an X9 trapped inside a string/comment does not. `has` returns true so `with` intercepts
           all free names; get returns X9's setter for 'X9' else the stub itself (chainable, callable). */
        const char *x9 =
            "globalThis.X9=function(){globalThis.__f9=1};globalThis.__f9=0;"
            "globalThis.__u=new Proxy(function(){return globalThis.__u},"
            "{has:function(){return true},get:function(t,k){return k==='X9'?globalThis.X9:globalThis.__u}});";
        JSValue xr = JS_Eval(g_solve_ctx, x9, strlen(x9), "<x9>", JS_EVAL_TYPE_GLOBAL); JS_FreeValue(g_solve_ctx, xr); }   /* fire-tracker + universal stub for the handler-firing verify */
    g_dedup_fn = JS_Eval(ctx, DEDUP_JS, strlen(DEDUP_JS), "<dedup>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(g_dedup_fn)) { js_std_dump_error(ctx); DFAIL("dedup fn failed to compile — engine self-host bug"); g_dedup_fn = JS_UNDEFINED; }
    { JSValue pv = JS_Eval(ctx, ARRAY_PRELUDE_JS, strlen(ARRAY_PRELUDE_JS), "<array-prelude>", JS_EVAL_TYPE_GLOBAL);
      if (JS_IsException(pv)) { js_std_dump_error(ctx); DFAIL("array prelude failed to compile — engine self-host bug"); }
      JS_FreeValue(ctx, pv); }
    JS_SetBuiltinCompile(ctx, 0);   /* page bundles are NOT builtins: their unused functions stay orphans (the moat) */
    js_std_add_helpers(ctx, 0, NULL);
    js_init_module_std(ctx, "std");
    js_init_module_os(ctx, "os");

    if (origin && origin[0]) set_origin(origin);   /* real page principal (location.origin/host) */
    if (replies && replies[0]) {                   /* fromReply table: { url -> concrete reply body } */
        JSValue t = JS_ParseJSON(ctx, replies, strlen(replies), "<replies>");
        if (JS_IsException(t)) { JSValue e = JS_GetException(ctx); JS_FreeValue(ctx, e); }
        else g_reply_table = t;
    }
    csp_set_header(csp);   /* REAL HTTP CSP header (fetch(location.href)); dom_run_scripts makes it the effective g_csp, primary over meta. (recipes are seeded in phase-2 qjs_begin.) */

    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "__isOpaque", JS_NewCFunction(ctx, js_is_concolic, "__isOpaque", 1));   /* self-hosted sort: concretize a meaningless opaque order without forking */
    JS_SetPropertyStr(ctx, g, "__opaqueExample", JS_NewCFunction(ctx, js_concolic_example, "__opaqueExample", 1));   /* self-hosted stringify: a config opaque's concrete example (else undefined) */
    JS_SetPropertyStr(ctx, g, "fetch", JS_NewCFunction(ctx, js_fetch, "fetch", 2));
    install_js_global_functions(ctx, g);   /* eval / new Function (@S code sinks) + structuredClone (bindings/global_functions.c) */
    /* Register the OPAQUE sentinel + the branch hook: a branch whose condition IS this object forks both
       arms via the decision-vector logic (real bundles: external input reaches OP_if as opaque). */
    JS_InitOpaqueClass(ctx);             /* register the shape-carrying opaque class */
    concolic_init(ctx);
    JS_SetConcolicMarker(g_concolic);
    JS_SetBranchHook(branch_decide);
    JS_SetForkableHook(ctx_forks);  /* an opaque-collection iterator forks (opaque done) only where branch_decide decides */
    JS_SetGateHook(gate_collect);   /* collect strings the code tests tainted input against -> search candidates */
    JS_SetCbHook(drive_opaque_cb);  /* a callback passed to a method on OPAQUE input (forEach/map/then/…) -> drive it as a flow */
    JS_SetAsyncCallHook(reg_add_async_call);  /* a native async CALL -> a preemptible/parkable scheduler flow (async-as-flow) */
    JS_SetReactionHook(reaction_flow);        /* a settled promise's .then/.catch/.finally reaction -> a scheduler flow too */
    JS_SetFlowAwaitHook(await_decide);        /* each fulfilled await forks a reject-replay sibling -> the inline try/catch catch arm is explored */
    JS_SetDynImportHook(host_dyn_import);   /* dynamic import() -> force-fetch the ESM chunk in place */
    JS_SetModuleLoaderFunc(rt, host_module_normalize, host_module_loader, NULL);   /* static import -> fetch+link the graph like a browser */
    /* synthetic MessageEvent for driving 'message' handlers: .data is the {pm} source (magic 2) — a
       getter so a candidate-replay flow injects the concrete payload here, exactly like location.hash. */
    g_msg_event = JS_NewObject(ctx);
    def_source(ctx, g_msg_event, "data", 2);
    /* origin/source are ATTACKER-UNCONTROLLABLE (the browser stamps e.origin to the sender's REAL origin). Tag
       origin as its own source "{origin}" so an EQ gate on it (`e.origin === 'https://trusted'`) is recognized
       as a real, un-forgeable check: a sink reached ONLY by force-passing it is NOT cross-origin exploitable
       and must not emit a PoC (solve_add suppresses on cons_fixed_value("{origin}")). */
    JS_SetPropertyStr(ctx, g_msg_event, "origin", JS_NewConcolicSourced(ctx, "{origin}", "{origin}"));
    JS_SetPropertyStr(ctx, g_msg_event, "source", js_concolic(ctx, "{messageSource}", JS_UNDEFINED));
    JS_SetPropertyStr(ctx, g_msg_event, "ports", js_concolic(ctx, "{messagePorts}", JS_UNDEFINED));
    JS_SetPropertyStr(ctx, g, "__sessionFns", JS_NewCFunction(ctx, js_session_fns, "__sessionFns", 0));
    JS_SetPropertyStr(ctx, g, "__sessionDrain", JS_NewCFunction(ctx, js_session_drain, "__sessionDrain", 0));
    {   /* SELF-HOSTED attacker-session loop (bytecode) — fire each [fn,event] pair over the accumulating COW
           delta, per-fn microtask drain. CALLED ONCE HERE (handlers empty -> no-op) so its bytecode is marked
           EXECUTED and JS_CollectOrphans does NOT collect it: a driven-as-orphan __driveSession would fire all
           handlers outside a session and pollute the candidate-enqueue dedup (broke xss_const's boot-replay). */
        /* Re-poll __sessionFns after each pass: a fn fired over the accumulating delta may REGISTER new handlers
           (connectedCallback wiring a click handler), which must fire over the SAME delta so their captured
           context (a DOM attr set by the producer) is intact. Each fn fires AT MOST ONCE (tracked by identity)
           so a producer is never re-fired (which would re-register endlessly). */
        const char *sj = "var __driveSession=function(){var fired=[],fns=__sessionFns();var seen=function(f){for(var k=0;k<fired.length;k++)if(fired[k]===f)return 1;return 0;};var any=1;while(any){any=0;for(var i=0;i<fns.length;i++){if(seen(fns[i][0]))continue;fired.push(fns[i][0]);try{fns[i][0].call(fns[i][2],fns[i][1]);}catch(e){}__sessionDrain();any=1;}if(any)fns=__sessionFns();}};";
        JSValue sr = JS_Eval(ctx, sj, strlen(sj), "<session>", JS_EVAL_TYPE_GLOBAL); JS_FreeValue(ctx, sr);
        JSValue ds = JS_GetPropertyStr(ctx, g, "__driveSession");
        if (JS_IsFunction(ctx, ds)) { JSValue r = JS_Call(ctx, ds, JS_UNDEFINED, 0, NULL); JS_FreeValue(ctx, r); }
        JS_FreeValue(ctx, ds);
    }

    /* Minimal browser environment: window/self = globalThis; OPAQUE external-input sources (location,
       document.cookie, navigator, referrer) so a real bundle's gate on external input auto-forks without
       synthetic args; addEventListener = no-op (the handler is then a never-fired orphan, driven). This
       is the seam the real Lexbor DOM + real safe-fetch plug into during the host rewire. */
    /* Real DOM: parse the page HTML (argv[2], optional) into a live Lexbor document + register the
       Element JS class. The page's own structure/config is CONCRETE; document.* reads it. */
    {
        if (dom_init(html, html ? strlen(html) : 0) != 0)
            DFAIL("dom_init failed — Lexbor parses any HTML, so a failure is an engine/resource bug, not a page");
        JS_NewClassID(rt, &g_el_class_id);
        JSClassDef el_def = { "Element" };   /* lexbor owns the nodes; no JS finalizer */
        JS_NewClass(rt, g_el_class_id, &el_def);
        JSValue el_proto = JS_NewObject(ctx);
        el_install_methods(ctx, el_proto);
        g_el_proto = JS_DupValue(ctx, el_proto);   /* custom-element bases (def_ctor) inherit these methods; freed in qjs_teardown */
        JS_SetClassProto(ctx, g_el_class_id, el_proto);
        JS_SetReceiverClass(rt, g_el_class_id);   /* JS_FindReceiver drives connectedCallback with the el-backed instance as `this` */
        cssom_init(ctx);   /* native CSSStyleDeclaration class (el.style / getComputedStyle), backed by the per-flow style attribute */
        blob_init(ctx);    /* native Blob class (internal content slot) — Blob/File .text() carries the content taint */
        trusted_types_init(ctx);  /* TrustedTypePolicy class */
        response_init(ctx); /* native Response class (internal body slot) */
    }

    g_handlers = JS_NewArray(ctx);
    JS_SetPropertyStr(ctx, g, "__handlers", JS_DupValue(ctx, g_handlers));   /* reachable so handlers survive to orphan-collect */
    JS_SetPropertyStr(ctx, g, "window", JS_DupValue(ctx, g));
    JS_SetPropertyStr(ctx, g, "self",   JS_DupValue(ctx, g));
    JS_SetPropertyStr(ctx, g, "globalThis", JS_DupValue(ctx, g));
    /* window.name: a raw ATTACKER source (opener-set, survives navigation, NOT percent-encoded) — a getset so a
       page WRITE (`window.name = x`) overrides it concretely (blocking the attacker path, no false PoC) while a
       bare read is attacker input (or the @S replay candidate, raw). */
    install_window_props(ctx, g);   /* window.name/location/navigator/addEventListener (core/frame/local_dom_window.c) */
    event_target_init(ctx, g);   /* EventTarget.prototype — the DOM inheritance spine root (core/dom/event_target.c) */
    node_init(ctx, g);           /* Node.prototype (EventTarget <- Node) — the spine middle (core/dom/node.c) */
    if (!JS_IsUndefined(g_el_proto)) JS_SetPrototype(ctx, g_el_proto, node_proto(ctx));   /* an Element IS a Node IS an EventTarget: inherit through the spine */
    document_init(ctx, g);   /* register Document class + prototype (chains to Node) + window.Document */
    { JSValue doc = js_document_make(ctx);   /* the window.document instance (shares the prototype) */
      install_named_properties(ctx, g, doc);   /* window[id]/document[id] = the real element (named access, not an opaque {state} shrug) */
      JS_SetPropertyStr(ctx, g, "document", doc); }
    /* WEB COMPONENTS: constructable DOM bases so `class X extends HTMLElement {…}` DEFINES -> its lifecycle
       methods (connectedCallback etc.) become uncalled methods the orphan driver reaches -> the element's
       endpoints/sinks are learned by EXECUTION (spec), not by reading a DOM attribute. customElements.define
       is a no-op: the ctor's methods are already reachable + orphan-driven. */
    install_window_apis(ctx, g, g_el_proto);   /* web-platform interface constructors onto the window (core/frame/local_dom_window.c) */
    install_window_objects(ctx, g);   /* Math/Date opaque + timers/storage/URL/fetch-objects (local_dom_window.c) */
    JS_FreeValue(ctx, g);

    if (boot) {
        /* `boot` (arg0) is the offscreen's combined-script preamble; capture+cache it in the boot delta with
           the inline (dom_run_scripts) + fetched-external (qjs_provide) scripts so a candidate flow can
           UNAPPLY to pre-boot and re-run the whole page boot under the concrete candidate. */
        JS_CowSetActive(1);
        if (boot[0]) {
            boot_script_cache(boot, strlen(boot));
            JSValue v = JS_Eval(ctx, boot, strlen(boot), "<boot>", JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(v)) { js_std_dump_error(ctx); g_rc = 1; }
            JS_FreeValue(ctx, v);
        }
        g_bundle_id = document_bundle_id(g_dom);   /* IDENTITY first, from a PURE DOM scan (no execution): frontier key set before boot runs */
        dom_run_scripts(ctx);     /* then run inline scripts + REQUEST external <script src> loads (fetched in qjs_step) */
        JS_CowSetActive(0);
        g_boot_delta = JS_CowBufTake(&g_boot_delta_n, &g_boot_delta_cap);
        JS_SetFlowLocalMark(1);   /* baseline is now fixed: every object a FLOW creates hereafter is flow-private (COW/taint skip it) */
    }
    return 0;   /* boot done; g_bundle_id is now readable via qjs_bundle_id(). Host reads it, looks up the parked
                   frontier by ORIGIN|bundle-id in IDB (its only job), then calls qjs_begin(recipes) to seed. */
}

/* The document's stable bundle IDENTITY = FNV-1a over its OWN scripts, computed by the REAL Lexbor <script>
   scan in dom_run_scripts (never a host-side regex). The host uses origin|this as the frontier key. */
KEEP unsigned qjs_bundle_id(void) { return (unsigned)g_bundle_id; }

/* Phase 2 (after qjs_init + the host's frontierGet by bundle-id): seed the frontier and fix the COW baseline.
   recipes NULL/"" -> a fresh visit (drive all orphans); non-empty -> RESUME by re-creating each parked flow,
   located by its function SOURCE hash (JS_OrphanHash), decisions replayed by the scheduler. */
KEEP void qjs_begin(const char *recipes)
{
    JSContext *ctx = g_ctx;
    if (!ctx) return;
    g_resume_mode = (recipes && recipes[0]) ? 1 : 0;
    seed_orphans(ctx);      /* normal: seed fresh orphan flows. resume: build g_orphan_buf locators only. */
    if (g_resume_mode) {
        /* RESUME: each recipe "hash,dec" re-creates a flow by LOCATING the orphan whose stable SOURCE identity
           (JS_OrphanHash) matches — robust to collection order/context. A recipe whose function is absent in
           THIS context simply doesn't match (skipped) — never drives the wrong one. */
        const char *p = recipes; int resumed = 0;
        while (*p) {
            int async_rec = 0;
            if (*p == 'a') { async_rec = 1; p++; }   /* async recipe: re-attach decvec to the re-fired async call, not drive an orphan */
            uint32_t want = (uint32_t)strtoul(p, NULL, 10);
            const char *comma = strchr(p, ','), *semi = strchr(p, ';');
            signed char *dec = NULL; int dec_n = 0; double rval = 1.0; int rvis = 0;
            if (comma && (!semi || comma < semi)) {
                const char *d = comma + 1, *end = semi ? semi : d + strlen(d);
                /* recipe = "hash,decbits[,valcenti[,visits]]": the decision vector ends at the SECOND comma
                   (the accumulated-value field) when present, else at the recipe end; visits follows a THIRD
                   comma. A legacy "hash,decbits" recipe (no value/visits) defaults rval=1.0/rvis=0 (unproven)
                   — backward compatible. */
                const char *comma2 = (const char *)memchr(d, ',', (size_t)(end - d));
                const char *decend = comma2 ? comma2 : end;
                dec_n = (int)(decend - d);
                if (dec_n > 0) { dec = (signed char *)malloc((size_t)dec_n); for (int i = 0; i < dec_n; i++) dec[i] = (d[i] == '1') ? 1 : 0; }
                if (comma2) {
                    double v = strtol(comma2 + 1, NULL, 10) / 100.0; if (v > 0) rval = v;   /* prior accumulated value */
                    const char *comma3 = (const char *)memchr(comma2 + 1, ',', (size_t)(end - (comma2 + 1)));
                    if (comma3) { long vs = strtol(comma3 + 1, NULL, 10); if (vs > 0 && vs < (1L << 30)) rvis = (int)vs; }   /* prior visit count (UCB explore term) */
                }
            }
            if (async_rec) {
                /* ASYNC recipe: park it in the pending map (g_arec). It attaches to the re-fired async call
                   by source hash at ONE of two consult sites of this one map — the post-loop SWEEP below (for
                   a boot-triggered call already re-created by phase-1 boot) or reg_add_async_call (for a
                   handler-triggered call that fires later in phase 3). Either way it REPLAYS its parked
                   await/branch path instead of re-forking from scratch. */
                if (g_arec_n >= g_arec_cap) { int nc = g_arec_cap ? g_arec_cap * 2 : 8; AsyncRecipe *na = (AsyncRecipe *)realloc(g_arec, (size_t)nc * sizeof(AsyncRecipe)); if (na) { g_arec = na; g_arec_cap = nc; } }
                if (g_arec_n < g_arec_cap) { g_arec[g_arec_n].hash = want; g_arec[g_arec_n].dec = dec; g_arec[g_arec_n].dec_n = dec_n; g_arec[g_arec_n].val = rval; g_arec[g_arec_n].visits = rvis; g_arec[g_arec_n].used = 0; g_arec_n++; }
                else free(dec);
                if (!semi) break; p = semi + 1; continue;
            }
            int found = -1;
            for (int oi = 0; oi < g_orphan_n; oi++) { if (JS_OrphanHash(ctx, g_orphan_buf[oi]) == want) { found = oi; break; } }
            if (found >= 0) {
                { Flow *rf = reg_add(ctx, JS_DupValue(ctx, g_orphan_buf[found]), rval, dec, dec_n);   /* resume with the PRIOR accumulated value, not a flat 1.0 */
                  rf->orphan_idx = found; rf->visits = rvis; }   /* restore visits so UCB reflects prior exploration */
                resumed++;
            } else free(dec);
            if (!semi) break; p = semi + 1;
        }
        /* SWEEP: attach async recipes to the flows phase-1 boot already re-created (their calls fired before
           the map existed). Handler-triggered recipes stay pending for reg_add_async_call in phase 3. */
        for (int ri = 0; ri < g_reg_n; ri++) if (arec_attach(ctx, &g_reg[ri])) resumed++;
        printf("@RESUMED %d\n", resumed); fflush(stdout);
    }
    JS_CowSetActive(1);   /* baseline = post-boot state; capture shared-state writes during flow exploration */
    g_dom_capture = 1;    /* DOM baseline is now fixed too; capture flow DOM mutations for per-flow revert */
    /* Seed ONE attacker-SESSION flow when the page has >=2 handlers — it fires them in sequence over
       accumulating shared state, the sound way to reach cross-handler sinks (source stored by A, sunk by B). */
    /* Seed ONE exploratory session for ANY entry point (handler OR orphan): it fires them in sequence over an
       accumulating delta so a cross-handler/cross-action opaque write reaches a later gate (redux/vuex
       login-action -> thunk, or handler A -> handler B) AND a handler wired MID-drive (connectedCallback ->
       click) is fired over that delta with its producer's context. The old >=2 gate was a BOUND that truncated
       nested-handler discovery on a single-component page; the WFQ starves a session with no real cross-flow
       state, so >=1 costs ~nothing when unproductive. */
    if (!g_resume_mode && (g_handler_n + g_orphan_n) >= 1) reg_add(ctx, JS_UNDEFINED, 1.2, NULL, 0)->session = 1;
    /* BOOT AS THE FIRST FLOW: enqueue a FORKING re-run of boot so its TOP-LEVEL opaque gates are EXPLORED. The
       initial boot (dom_run_scripts) ran MONOLITHICALLY before the scheduler (g_running=0 -> branch_decide took
       the false arm on every opaque gate), so a page whose auth-gated surface sits behind a boot-level
       `if(localStorage.getItem('token'))` / cookie / `if(window.__FLAGS.admin)` gate with NO fetch would NEVER
       be surfaced. A reply also enqueues one (qjs_provide, to re-run boot with the reply synchronous); this
       covers the no-reply case. The WFQ starves it if boot has no forkable gate. */
    if (!g_resume_mode && boot_script_count() > 0) reg_add_boot(ctx, NULL, 0);
}

/* The distinct pending fetch urls (newline-joined) the offscreen must safe-fetch. Static buffer. */
KEEP const char *qjs_pending(void)
{
    static char *buf = NULL; static size_t cap = 0;
    size_t need = 1;
    for (int i = 0; i < g_pending_n; i++) if (g_pending[i].url) need += strlen(g_pending[i].url) + 1;
    if (need > cap) { char *n = realloc(buf, need); if (!n) return ""; buf = n; cap = need; }
    size_t off = 0;
    for (int i = 0; i < g_pending_n; i++) {
        if (!g_pending[i].url) continue;
        int dup = 0;
        for (int j = 0; j < i; j++) if (g_pending[j].url && strcmp(g_pending[j].url, g_pending[i].url) == 0) { dup = 1; break; }
        if (dup) continue;
        size_t l = strlen(g_pending[i].url);
        memcpy(buf + off, g_pending[i].url, l); off += l; buf[off++] = '\n';
    }
    buf[off] = 0;
    return buf;
}
/* Chunk urls to fetch (as JS) + eval in place (newline-joined). Static buffer. */
KEEP const char *qjs_chunks(void)
{
    static char *buf = NULL; static size_t cap = 0;
    size_t need = 1;
    for (int i = 0; i < g_chunk_n; i++) need += strlen(g_chunk_pending[i]) + 1;
    if (need > cap) { char *n = realloc(buf, need); if (!n) return ""; buf = n; cap = need; }
    size_t off = 0;
    for (int i = 0; i < g_chunk_n; i++) { size_t l = strlen(g_chunk_pending[i]); memcpy(buf + off, g_chunk_pending[i], l); off += l; buf[off++] = '\n'; }
    buf[off] = 0;
    return buf;
}
/* Resolve every pending consume of `url` with the concrete body (empty -> opaque); cache in the reply
   table so a later response of the same url is concrete too. If `url` is a pending CHUNK, eval its body
   in place — extending the page BASELINE (cow off during eval), so its defs are permanent (not a flow
   write to be reverted) and its orphans get driven on the next step. Continuations run on the next step. */
KEEP void qjs_provide(const char *url, const char *body)
{
    JSContext *ctx = g_ctx; if (!ctx || !url) return;
    for (int i = 0; i < g_chunk_n; i++) {
        if (strcmp(g_chunk_pending[i], url) != 0) continue;
        if (body && body[0]) {
            JS_CowRevert(ctx);                                   /* to baseline (parked flows have empty delta) */
            int dsv = g_dom_capture; g_dom_capture = 0; JS_CowSetActive(0); JS_SetFlowLocalMark(0);   /* a lazy CHUNK's globals are BASELINE (shared, re-run in boot-replay), not flow-local — mark 0 while it evals */
            modsrc_put(url, body, strlen(body));                 /* available to the module loader by URL */
            if (is_moddep(url)) pendmod_retry(ctx);              /* a static-import dep OR dyn-import chunk: link in-graph (don't eval standalone -> no double side effects) */
            else eval_page_script(ctx, body, strlen(body), url, JS_DetectModule(body, strlen(body))); /* classic external script: run standalone (module vs classic by the real detector) */
            if (JS_DetectModule(body, strlen(body))) {
                /* MODULE chunk: link the SINGLETON now, in the BASE context (COW off here), caching its
                   namespace; then enqueue a forking boot re-run so a reply/chunk-gated import() resolves to
                   the singleton SYNCHRONOUSLY in a LIVE flow (m.load runs + tears down cleanly). This is the
                   provision-driven re-run that REPLACES the deleted async-park — the same rule a new reply
                   uses. */
                JSValue mns; if (dynimport_link(ctx, url, &mns)) JS_FreeValue(ctx, mns);
                if (!g_resume_mode && boot_script_count() > 0) reg_add_boot(ctx, NULL, 0);
            } else {
                /* CLASSIC chunk: cache so boot-replay re-runs it (a source stored / handler registered in an
                   external classic script is re-established under a candidate). A module is NOT cached — it is
                   a singleton, and boot_scripts_run evals a cached script as a CLASSIC global (`export...` -> abort). */
                boot_script_cache(body, strlen(body));
            }
            JS_CowSetActive(1); JS_SetFlowLocalMark(1); g_dom_capture = dsv;
        }
        chunk_mark_done(url);   /* fetched once: its body is cached/linked + re-run on boot re-runs, never re-fetched (no provide livelock) */
        free(g_chunk_pending[i]);
        for (int j = i; j < g_chunk_n - 1; j++) g_chunk_pending[j] = g_chunk_pending[j + 1];
        g_chunk_n--;
        return;
    }
    int has = body && body[0];
    if (has) {
        /* CACHE the reply so a re-run's make_response injects __body (r.json()/r.text() -> CONCOLIC synchronously),
           creating g_reply_table if the host seeded none. On a NEW url, enqueue a FORKING BOOT FLOW: it re-runs
           boot with this reply now synchronous, so a reply-GATED continuation forks WITH the concolic example
           (the value reaches gated fetches, not just ungated). */
        if (!JS_IsObject(g_reply_table)) { JS_FreeValue(ctx, g_reply_table); g_reply_table = JS_NewObject(ctx); }
        JSValue prev = JS_GetPropertyStr(ctx, g_reply_table, url); int is_new = !JS_IsString(prev); JS_FreeValue(ctx, prev);
        JS_SetPropertyStr(ctx, g_reply_table, url, JS_NewString(ctx, body));
        if (is_new && boot_script_count() > 0) reg_add_boot(ctx, NULL, 0);
    }
    for (int i = 0; i < g_pending_n; i++) {   /* the reply is CACHED + a boot re-run enqueued above; just drop the fetch registration (no promise to resolve) */
        if (g_pending[i].url && strcmp(g_pending[i].url, url) == 0) { free(g_pending[i].url); g_pending[i].url = NULL; }
    }
    int w = 0; for (int i = 0; i < g_pending_n; i++) if (g_pending[i].url) g_pending[w++] = g_pending[i];
    g_pending_n = w;
}
/* Drop all remaining fetch registrations (a reply/chunk never fetched): r.json() already resolved OPAQUE in
   place, so there is nothing to settle — just free the url list. */
KEEP void qjs_finalize(void)
{
    JSContext *ctx = g_ctx; if (!ctx) return;
    (void)ctx;
    for (int i = 0; i < g_pending_n; i++) free(g_pending[i].url);
    g_pending_n = 0;
    for (int i = 0; i < g_chunk_n; i++) free(g_chunk_pending[i]);   /* node CLI can't fetch chunks -> drop */
    g_chunk_n = 0;
}
/* Advance the ONE scheduler; drain microtasks. Return 1 = NEED_FETCH (flows parked awaiting a real
   reply the offscreen must provide via qjs_provide), else 0 = DONE. */
/* Host cross-document WFQ enablers (Level-1): the host ranks a live engine by its best flow's weight
   (qjs_top_weight) and sets the RUNNER-UP engine's weight as this engine's yield floor; the engine runs
   until its best flow no longer outranks that floor, then yields HOT. VALUE-driven, not a slice count.
   Floor -1e300 (default / lone engine) = run to completion. */
KEEP void qjs_set_yield_floor(double w) { g_yield_floor = w; }
/* Host RAM-pressure signal (cold-tier): when resident RAM crosses the working-set floor the host raises this,
   and the scheduler parks its remaining frontier as replay recipes to IDB and yields. RESOURCE-driven, never a
   dispatch count — with headroom it is never raised and the page runs to completion in one visit. */
KEEP void qjs_request_park(void) { g_park_requested = 1; }
KEEP double qjs_top_weight(void)     /* this engine's value-of-information = its best flow's weight (0 if idle/done) */
{
    double best = 0; int seen = 0;
    for (int i = 0; i < g_reg_n; i++) { double w = flow_weight(&g_reg[i]); if (!seen || w > best) { best = w; seen = 1; } }
    return seen ? best : 0.0;
}

KEEP int qjs_step(void)
{
    if (!g_ctx) return 0;
    g_made_progress = 0;                                 /* fresh progress guard each host visit */
    scheduler_run(g_ctx);
    js_std_loop(g_ctx);
    if (g_pending_n > 0 || g_chunk_n > 0) return 1;      /* NEED_FETCH: replies and/or chunks */
    if (g_reg_n > 0) return 2;                           /* HOT work remains (value-yielded) — host re-ranks + resumes */
    return 0;                                            /* fully explored (or parked) — done */
}

/* INCREMENTAL MERGE: snapshot the CURRENT findings (endpoints/@S/chunks) as a fresh @RESULT WITHOUT teardown,
   so a lone UNBOUNDED engine (a reply-gated recursion that never drains) still surfaces its already-emitted
   breadth to the cumulative moat instead of learning nothing until a finalize that never comes. emit_result is
   teardown-free (dedup + collect verified @S + stringify, no state mutation); solve_all only COLLECTS verified
   PoCs (the solving is in replay flows), so this is repeat-safe. The host calls it on a coarse cadence for a
   hot engine and merges the snapshot (globalStore is cumulative + dedup-idempotent, so re-merge is sound). */
KEEP void qjs_emit_partial(void) { if (g_ctx) emit_result_ex(g_ctx, 0); }   /* incremental snapshot: endpoints AND verified @S PoCs (solve_all is a pure collector) */

KEEP void qjs_teardown(void)
{
    JSContext *ctx = g_ctx;
    if (!ctx) return;
    arec_free();   /* free any async recipes whose handler never re-fired this session (never lost — waits for the session that fires it) */
    qjs_finalize();   /* resolve any stragglers opaque so no promise leaks */
    /* Unresolved module graph residue -> @WHY BEFORE emit_result (so it lands in resolverErrors). */
    { int pm = module_pending_count(); if (pm) { char rz[64]; snprintf(rz, sizeof rz, "unresolved module graph x%d (dep never fetched)", pm); why_add(ctx, "module-link", rz); } }
    emit_result(ctx);   /* dedup in-engine + emit the ONE @RESULT json (endpoints/sinks/chunks/errors/park/_emit) */
    /* Clean teardown (else JS_FreeRuntime asserts gc_obj_list non-empty): stop + revert the COW log so
       its held baseline values return to their slots, and drop the opaque marker. */
    JS_CowSetActive(0);
    JS_CowRevert(ctx);
    JS_OptaintReset(ctx);   /* release the cross-flow opaque-taint set: it js_dup()s every tainted object and is
                               GLOBAL (only reset at the NEXT qjs_begin), so a run that ends in teardown without a
                               following analysis leaks every tainted object (the reply objects) and JS_FreeRuntime
                               asserts gc_obj_list non-empty. Reset here so the final run frees cleanly too. */
    g_dom_capture = 0; dom_revert();   /* drop DOM undo log (restore baseline) before teardown */
    JS_SetConcolicMarker(JS_UNDEFINED); JS_SetBranchHook(NULL); JS_SetGateHook(NULL);
    solve_free(ctx);   /* @S accumulators + gate tokens -> solve.c */
    boot_scripts_free(ctx);
    csp_free();
    cons_free();   /* free the per-flow value-domain constraint set (solver/constraints.c) */
    module_loader_free(ctx);   /* free the modsrc/moddep/pendmod tables (module_loader.c) */
    if (g_boot_delta) JS_CowBufFree(ctx, g_boot_delta, g_boot_delta_n);   /* free the stashed boot delta */
    g_boot_delta = NULL; g_boot_delta_n = g_boot_delta_cap = 0;
    attr_shadow_free(ctx);
    replay_handlers_clear(ctx); free(g_replay_handlers); g_replay_handlers = NULL; g_replay_handler_cap = 0;
    JS_FreeValue(ctx, g_el_proto); g_el_proto = JS_UNDEFINED;   /* the element-method proto ref (custom-element base chain) */
    ce_free(ctx);   /* customElements registry + instances */
    node_free(ctx);
    event_target_free(ctx);
    concolic_free(ctx);
    JS_FreeValue(ctx, g_reply_table); g_reply_table = JS_UNDEFINED;
    storage_free(ctx);
    cookie_free(ctx);
    winname_free(ctx);
    idb_free(ctx);
    endpoint_free(ctx);
    JS_FreeValue(ctx, g_chunkurls); g_chunkurls = JS_UNDEFINED;
    JS_FreeValue(ctx, g_park); g_park = JS_UNDEFINED;
    JS_FreeValue(ctx, g_dedup_fn); g_dedup_fn = JS_UNDEFINED;
    location_free(ctx);   /* free the window.location singleton (location.c) */
    for (int i = 0; i < g_orphan_n; i++) JS_FreeValue(ctx, g_orphan_buf[i]);
    g_orphan_n = 0;
    JS_FreeValue(ctx, g_handlers); g_handlers = JS_UNDEFINED;
    JS_FreeValue(ctx, g_msg_event); g_msg_event = JS_UNDEFINED; g_msg_handler_n = 0;
    js_std_free_handlers(g_rt);
    js_global_functions_free(ctx);   /* eval/Function/structuredClone bindings (bindings/global_functions.c) */
    if (g_solve_ctx) { JS_FreeContext(g_solve_ctx); g_solve_ctx = NULL; }   /* free the solver realm before its runtime */
    JS_RunGC(g_rt);   /* CYCLE-collect now the global roots are dropped: an unreachable promise<->reaction cycle (e.g. a discarded import() chain) is benign garbage refcounting can't reclaim; the gc_obj_list assert then fires ONLY for a genuinely ROOTED leak (a real bug), not for collectible cycles */
    JS_FreeContext(ctx);
    JS_FreeRuntime(g_rt);
    g_ctx = NULL; g_rt = NULL;
    fflush(stdout);
}

/* node CLI entry (design-narrowing only): drive the persistent protocol once. */
int main(int argc, char **argv)
{
    const char *boot    = (argc > 1) ? argv[1] : NULL;
    const char *html    = (argc > 2) ? argv[2] : NULL;
    const char *origin  = (argc > 3) ? argv[3] : NULL;
    const char *replies = (argc > 4) ? argv[4] : NULL;
    const char *recipes = (argc > 5) ? argv[5] : NULL;
    if (qjs_init(boot, html, origin, replies, recipes) != 0) return 1;
    qjs_begin(recipes);   /* phase 2: seed the frontier (fresh or resume) + fix the COW baseline */
    if (boot) { while (qjs_step() == 1) qjs_finalize(); }   /* node CLI has no network -> resolve pending opaque (shapes) */
    qjs_teardown();
    return g_rc;
}
