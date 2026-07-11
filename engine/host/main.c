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
#include "solver/async_flow.h"   /* ASYNC-AS-FLOW: the async-call/reaction/await hooks + the cross-session async-recipe map */
#include "solver/boot_flow.h"    /* BOOT-AS-FLOW + candidate-replay + attacker session (the flow types beyond a plain orphan) */
#include "solver/solve_html.h"   /* @S HTML breakout analysis (context-detect + firing-verify), split into its own TU */
#include "solver/envelope.h"     /* @S structured-source delivery envelope (JSON/query/delim addressing), its own TU */
#include "core/frame/csp.h"          /* Content-Security-Policy: effective policy + per-sink-class relevance, its own TU */
#include "core/dom/dom_select.h"   /* CSS selector engine (querySelector/All, matches) over the Lexbor DOM, its own TU */
#include "core/dom/handler_registry.h"   /* registered event listeners (addEventListener) the orphan driver force-fires, its own TU */
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
#include "core/loader/chunk_loader.h"       /* chunk_pending_add / chunk_provide / chunk_list — the lazy-chunk resource loader */
#include "core/loader/reply_registry.h"     /* reply_fetch_register / reply_pending_list / drop — the reply fetch registry */
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
JSContext *g_ctx;       /* fwd: the MAIN analysis context (defined near the persistent-instance protocol) — the
                                  async/reaction hooks claim a call as a flow ONLY for this ctx, never the @S solve realm */
int g_in_boot_flow = 0; /* a BOOT flow is re-running boot: fork boot siblings; suppress handler re-registration */
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
JSValue g_orphan_buf[4096];
int     g_orphan_n = 0;
/* The WFQ dispatch loop + its state (cooperative quantum, parked-weight cache, decision vector) -> scheduler.c. */
static uint32_t g_bundle_id = 0;        /* stable id of THIS document's own scripts (Lexbor DOM scan, not regex) — the frontier key */
static char *g_boot_preamble = NULL;    /* the offscreen combined-script preamble, stashed so run_initial_boot can run it when boot is dispatched as Flow #0 (de-entangled from qjs_init's synchronous return) */
int g_boot_active = 0;                   /* boot's script phase is running or PARKED on a sync external (not complete) — the scheduler must not dispatch flows until boot completes; READ by the chunk loader (a chunk mid-boot joins the boot delta) */
static char *g_pending_recipes = NULL;  /* resume recipes stashed by qjs_begin, consumed by seed_frontier at boot completion */
static int g_need_initial_boot = 0;     /* the first qjs_step runs run_initial_boot */
/* CSP (g_csp/g_header_csp + csp_lacks/csp_derive/csp_set_header/csp_free) lives in csp.c — included below. */
extern lxb_html_document_t *g_dom;   /* the live parsed document (defined below); the @S emitter needs it for the CSP nonce scan */
/* g_concolic + js_noop/js_concolic_read/js_concolic_stub + concolic_init/free live in opaque.c (included via opaque.h). */
char *g_candidate = NULL;          /* @S: the running REPLAY flow's concrete candidate (source getters return it); NULL in normal flows */

/* has_hole (opaque-hole URL test) is in url.c (included via url.h). */

/* Per-flow value-domain constraint tracker (Cons, g_cons, cons_reset-set-feasible-fixed_value, pair_contradicts; plus @S-delivery state g_origin_req, g_sink_jkey, g_sink_root) lives in solver-constraints.c. */
/* url_solve_holes ({src}-hole value-solve) -> browser/url.c (URL construction module). */

/* Per-flow DOM COW delta (dom_attr_capture/insert_capture + dom_apply/unapply/revert + dom_buf_*) is in
   dom_cow.c — included via dom_cow.h above; the buffer is an opaque void* on the Flow. */
/* Append a fresh flow to the ONE registry and RETURN it (never NULL — OOM is a physical floor that CHECK-aborts,
   never a dropped flow). Callers configure the returned Flow directly (f->candidate/session/orphan_idx/...),
   so no caller reaches into g_reg[g_reg_n-1] — the registry array stays encapsulated behind this one entry. */

/* Re-add a SUSPENDED flow (a full copy, fs retained) so it interleaves back into the ONE registry. */


/* fromReply: a bridge-provided map { url -> concrete reply body text }. A real GET is fired by the
   TRUSTED offscreen (safeFetch, one-per-endpoint) and its body injected here so r.json()/r.text()
   return the CONCRETE server reply -> a reply field flowing into a downstream request param becomes a
   REAL example value instead of {}. Absent url -> opaque (the honest shape). */
JSValue g_reply_table = JS_UNDEFINED;
/* g_storage + js_storage_get/set + storage_free live in storage.c (localStorage/sessionStorage concolic). */
JSContext *g_ctx;   /* the run's context (defined = NULL below); fwd-declared for Lexbor callbacks */
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
/* Reply FETCH registry (reply_fetch_register / reply_pending_list / drop) -> browser/core/loader/reply_registry.
   A reply-consume (r.json()/r.text()) with no cached body registers its url so qjs_step returns NEED_FETCH; the
   offscreen safe-fetches + qjs_provide()s the body, which CACHES it and delivers the concolic reply to the parked
   consumer (solver/reply.c). NO promise resolve fn is held across the fetch (persistent async state outside the
   per-flow COW delta): r.json() resolves OPAQUE in place; concrete comes from the delivery. */
/* Chunk-load pending/done registry + the fetch-and-evaluate of a lazy <script src>/dyn-import chunk ->
   browser/core/loader/chunk_loader.{c,h} (Blink core/loader). qjs_provide asks chunk_provide first. */

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
JSValue g_park = JS_UNDEFINED;         /* JS array of "hash,decbits" frontier replay recipes */
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
int g_boot_replay = 0;   /* re-running boot to re-establish shared state for a cross-flow @S candidate */
/* reg_add_boot / reg_add_session (+ boot_replay / boot_replay_candidate / resolve_replayed_handler / the
   attacker session) -> solver/boot_flow.c, declared in boot_flow.h. */
/* Does the CURRENT context fork/decide opaque branches (so an opaque-collection iterator can yield an opaque
   `done` and let the loop-back branch fork/terminate), or is it the MONOLITHIC initial boot where branch_decide
   returns 0 unconditionally (an opaque loop-back cond would resolve 'continue' forever)? Mirrors branch_decide's
   non-zero condition EXACTLY — the single source of truth for "will the branch do something other than fall
   through". */
/* crypto (getRandomValues/randomUUID/subtle) -> browser/crypto.c. */
/* setTimeout/setInterval/requestAnimationFrame(cb, ...): a deferred callback is NOT a wait on real time —
   it is just another BFS FLOW. Register cb in the ONE scheduler (reg_add) so it is driven, ordered, and
   starved by the same WFQ as every other flow (the whole point: bundles that defer init in a timer still
   get explored). */
/* THE scheduler edge every deferred callback uses: register cb as a first-class BFS FLOW (setTimeout/rAF/
   queueMicrotask/observer/opaque-method-cb) at the running flow's value, ordered + starved by the one WFQ,
   never a real wait. A host-edge only FEEDS this; the scheduler DECIDES. */
/* setTimeout/setInterval/requestAnimationFrame -> browser/timers.c (defers the callback as a flow). */
/* JS_SetCbHook target: a callback passed to a method CALLED ON opaque input (items.forEach(cb) where items
   is a reply/injected-state array) — the method-on-opaque returns opaque without invoking cb, so register cb
   as a starter FLOW (exactly like a deferred timer). The scheduler force-invokes it with opaque args so its
   per-element endpoints/sinks are reached; transient (no orphan_idx), WFQ-ordered/starved like any flow. */
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
/* The event-handler registry (g_handlers, is_handler, js_add_listener, ...) -> core/dom/handler_registry.c.
   The synthetic {pm} attacker MessageEvent a 'message' listener is driven with stays here (the scheduler owns
   the attacker source): its .data is the source-tagged concolic {pm}, so a postMessage-XSS sink reports {pm}
   and the PoC assembler builds a postMessage-delivered PoC. Borrowed into the driven flow, not owned by it. */
JSValue g_msg_event = JS_UNDEFINED;
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
/* eval_page_script + dom_run_scripts -> browser/core/html/html_script_runner.{c,h} (Blink HTMLScriptRunner):
   running the document's <script> elements is a BROWSER component, not scheduler state. */

/* js_is_concolic/js_concolic_example (__isOpaque/__opaqueExample leaf intrinsics) -> solver/opaque.{c,h}. */

/* orphan flow source — CONTINUOUS discovery, NOT a one-shot phase. Called every scheduler iteration so
   functions defined DYNAMICALLY during a forced flow (a login-gated lazy CHUNK: eval/import of fetched
   JS reached only by forcing the auth branch) become orphan flows and get driven -> we learn the
   logged-in surface while logged out. Already-executed fns are not returned by JS_CollectOrphans;
   already-queued fns are dup-skipped. Idempotent: re-running adds only NEW never-executed functions. */

/* ── Persistent-instance protocol ─────────────────────────────────────────────────
   ONE wasm instance per page, driven in steps by the offscreen: qjs_init (build runtime + env + boot +
   seed the frontier), qjs_step (advance the ONE scheduler), qjs_teardown. This replaces the old
   re-instantiate-and-re-run-per-pass model so chunks/fromReply/frontier become ONE continuous run
   (in-place suspend/resume) instead of re-running the whole page. */
JSContext *g_ctx = NULL;
static int g_rc = 0;

/* Run the page's initial boot: the preamble + inline <script>s as the FIRST forking boot flow (all-false primary
   -> the canonical logged-out g_boot_delta the whole frontier layers over; each opaque boot gate forks a TRUE
   sibling boot flow, dispatched later). Extracted from qjs_init so boot EXECUTION can be de-entangled from the
   host's synchronous return — the host only needs g_bundle_id (a pure DOM scan), never g_boot_delta, at return. */
/* Finish boot's script phase (cursor reached the last script — after every sync external ran in position): capture
   g_boot_delta, fix the COW baseline, then seed the frontier (its functions now exist). */
void boot_complete(JSContext *ctx) {   /* non-static: the chunk loader completes a boot parked on a sync external */
    g_initial_boot = 0; g_in_boot_flow = 0; g_running = 0; g_cur_flow = NULL;
    JS_CowSetActive(0);
    g_boot_delta = JS_CowBufTake(&g_boot_delta_n, &g_boot_delta_cap);
    JS_SetFlowLocalMark(1);
    g_boot_active = 0;
    seed_frontier(ctx, g_pending_recipes);
}
static void run_initial_boot(JSContext *ctx) {
    JS_CowSetActive(1);
    g_running = 1; g_in_boot_flow = 1; g_initial_boot = 1; g_c = 0; g_dec_n = 0; cons_reset();
    if (g_boot_preamble && g_boot_preamble[0]) {
        JSValueConst bc = boot_script_cache(ctx, JS_NULL, g_boot_preamble, strlen(g_boot_preamble));
        if (boot_exec_one(ctx, JS_NULL, bc)) { js_std_dump_error(ctx); g_rc = 1; }   /* preamble runs through the SAME executor as replays */
    }
    g_boot_active = 1;
    if (dom_run_scripts(ctx)) return;   /* PARKED on a synchronous external <script src>: wait for qjs_provide -> dom_boot_resume */
    boot_complete(ctx);
}

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
    g_orphan_n = 0; g_dom_capture = 0;
    reply_registry_free();   /* fresh session: drop pending reply fetches */
    chunk_loader_free();     /* + pending chunks and the per-session fetched-chunk cache (a new visit re-fetches the current bytes) */
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

    handlers_init(ctx);   /* create the event-handler registry (g_handlers) -> handler_registry.c */
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
        /* `boot` (arg0) is the offscreen combined-script preamble; stash it so run_initial_boot can run it (with
           the inline + fetched-external scripts) as the first boot flow, unapply-able to pre-boot for candidates. */
        g_boot_preamble = boot[0] ? strdup(boot) : NULL;
        g_bundle_id = document_bundle_id(g_dom);   /* IDENTITY: a PURE DOM <script> scan (no execution) — the ONLY thing the host needs at return, INDEPENDENT of boot execution. Boot EXECUTION is deferred to the first qjs_step (run_initial_boot), so it is not welded into this synchronous return. */
    }
    return 0;   /* g_bundle_id is now readable via qjs_bundle_id(). Host reads it, looks up the parked frontier by
                   ORIGIN|bundle-id in IDB (its only job), then calls qjs_begin(recipes) to seed. */
}

/* The document's stable bundle IDENTITY = FNV-1a over its OWN scripts, computed by the REAL Lexbor <script>
   scan in dom_run_scripts (never a host-side regex). The host uses origin|this as the frontier key. */
KEEP unsigned qjs_bundle_id(void) { return (unsigned)g_bundle_id; }


/* Phase 2: the host has read g_bundle_id (identity) + looked up the parked frontier; it now hands over the resume
   recipes. Boot EXECUTION and frontier seeding are DEFERRED to the first qjs_step (run_initial_boot then
   seed_frontier) — so boot runs as a dispatched flow, NOT welded into qjs_init's synchronous return. */
KEEP void qjs_begin(const char *recipes)
{
    if (!g_ctx) return;
    free(g_pending_recipes); g_pending_recipes = (recipes && recipes[0]) ? strdup(recipes) : NULL;
    g_need_initial_boot = 1;
}

/* The distinct pending fetch urls (newline-joined) the offscreen must safe-fetch. Static buffer. */
KEEP const char *qjs_pending(void) { return reply_pending_list(); }
/* Chunk urls to fetch (as JS) + eval in place (newline-joined) -> the chunk loader owns the list. */
KEEP const char *qjs_chunks(void) { return chunk_list(); }
/* Resolve every pending consume of `url` with the concrete body (empty -> opaque); cache in the reply
   table so a later response of the same url is concrete too. If `url` is a pending CHUNK, eval its body
   in place — extending the page BASELINE (cow off during eval), so its defs are permanent (not a flow
   write to be reverted) and its orphans get driven on the next step. Continuations run on the next step. */
KEEP void qjs_provide(const char *url, const char *body)
{
    JSContext *ctx = g_ctx; if (!ctx || !url) return;
    if (chunk_provide(ctx, url, body)) return;   /* a pending lazy chunk: fetched + evaluated by the loader component */
    int has = body && body[0];
    if (has) {
        /* CACHE the reply so a re-run's make_response injects __body (r.json()/r.text() -> CONCOLIC synchronously),
           creating g_reply_table if the host seeded none. On a NEW url, enqueue a FORKING BOOT FLOW: it re-runs
           boot with this reply now synchronous, so a reply-GATED continuation forks WITH the concolic example
           (the value reaches gated fetches, not just ungated). */
        if (!JS_IsObject(g_reply_table)) { JS_FreeValue(ctx, g_reply_table); g_reply_table = JS_NewObject(ctx); }
        JS_SetPropertyStr(ctx, g_reply_table, url, JS_NewString(ctx, body));   /* cache for a re-entrant/cached read (make_response __body) */
        pendreply_resolve(ctx, url, body);   /* PARK-RESUME: deliver the concrete concolic reply to every parked r.json()/r.text() of this url — the continuation resumes in place, no boot re-run */
    }
    reply_pending_drop(url);   /* the reply is CACHED + its parked consumers resolved above; drop the fetch registration */
}
/* Drop all remaining fetch registrations (a reply/chunk never fetched). A parked r.json()/r.text() whose body
   never arrived is resolved OPAQUE {reply} here so its continuation still runs (shape coverage), never left
   hanging — the park-and-resume model's honest fallback. */
KEEP void qjs_finalize(void)
{
    JSContext *ctx = g_ctx; if (!ctx) return;
    pendreply_drain_opaque(ctx);   /* resolve any never-delivered parked reply with {reply} (shape) */
    reply_registry_free();         /* drop remaining reply fetch registrations */
    chunk_loader_free();   /* node CLI can't fetch chunks -> drop pending (+ the per-session done cache) */
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
KEEP double qjs_top_weight(void) { return scheduler_top_weight(); }   /* this engine's value-of-information = its best flow's weight -> scheduler.c */

KEEP int qjs_step(void)
{
    if (!g_ctx) return 0;
    if (g_need_initial_boot) { run_initial_boot(g_ctx); g_need_initial_boot = 0; }   /* FIRST step: run boot (document-order; may PARK on a sync external) */
    if (g_boot_active) return 1;   /* boot PARKED on a sync external: NEED_FETCH; do NOT dispatch flows (seed runs in boot_complete) */
    g_made_progress = 0;                                 /* fresh progress guard each host visit */
    scheduler_run(g_ctx);
    js_std_loop(g_ctx);
    if (reply_pending_count() > 0 || chunk_pending_count() > 0) return 1;      /* NEED_FETCH: replies and/or chunks */
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
    free(g_boot_preamble); g_boot_preamble = NULL;   /* free the stashed preamble */
    free(g_pending_recipes); g_pending_recipes = NULL;   /* free the stashed resume recipes */
    csp_free();
    cons_free();   /* free the per-flow value-domain constraint set (solver/constraints.c) */
    module_loader_free(ctx);   /* free the modsrc/moddep/pendmod tables + import delivery-park (module_loader.c) */
    pendreply_free(ctx);       /* free the reply delivery-park table (reply.c) */
    reply_registry_free();     /* free the reply fetch registry (reply_registry.c) */
    chunk_loader_free();       /* free the chunk pending/done registries (chunk_loader.c) */
    if (g_boot_delta) JS_CowBufFree(ctx, g_boot_delta, g_boot_delta_n);   /* free the stashed boot delta */
    g_boot_delta = NULL; g_boot_delta_n = g_boot_delta_cap = 0;
    attr_shadow_free(ctx);
    handlers_free(ctx);   /* g_handlers + candidate-closure replay handlers + counts -> handler_registry.c */
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
    JS_FreeValue(ctx, g_msg_event); g_msg_event = JS_UNDEFINED;   /* g_handlers/g_msg_handler_n freed by handlers_free above */
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
