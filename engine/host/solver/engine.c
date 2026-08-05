/* The dispatch loop — see engine.h. */
#include "solver/engine.h"
#include "core/html/unhandled_rejection.h"
#include "core/idl_args.h"   /* the one point every Web API member passes through — see idl_slowest_step */   /* HTML §8.1.7.5: what the browser half owes this checkpoint */
#include "solver/result.h"
#include "solver/solve.h"
#include "solver/flow.h"
#include "solver/decide.h"
#include "solver/concolic.h"
#include "solver/cow.h"
#include "solver/dom_cow.h"   /* the DOM half of time-travel — swapped per-flow alongside the heap COW delta */
#include "check.h"
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* FETCH-AWAIT parking: a host fetch that is not synchronously available (a live network GET) returns a PENDING
   promise and registers its resolve capability + the value it will deliver on THE RUNNING FLOW. A flow that awaits
   it suspends its async body; when the flow's scripts + microtasks are drained but a live fetch is still pending,
   flow_step resolves the flow's OWN pending fetches (the network completing) — each awaiting async body's reaction
   enqueues as a job in that flow's queue — and resumes. Per-flow (not global) so one flow's drain never resolves
   another flow's fetch (which would route the reaction to the wrong flow's COW — a leak + contamination). */
void engine_pending_fetch(JSContext *ctx, JSValueConst resolve, JSValueConst value) {
    engine_pending_fetch_url(ctx, resolve, value, NULL);
}

/* The same park, with the URL the HOST must fetch. The value arrives later through engine_provide; until it
   does the flow cannot finish, which is what keeps the reply-gated code reachable. */
void engine_pending_fetch_url(JSContext *ctx, JSValueConst resolve, JSValueConst value, const char *url) {
    Flow *f = flow_running();
    /* A live fetch is ALWAYS issued from a running flow — both explore and @S verify are the ONE scheduler now
       (run_scheduler), so flow_running() is set; the flow's stall drains it (flow_step). */
    DCHECK(f != NULL, "engine_pending_fetch: a live fetch issued outside a running flow");
    if (f->npend >= f->pendcap) {
        f->pendcap = f->pendcap ? f->pendcap * 2 : 8;
        f->pending = realloc(f->pending, (size_t)f->pendcap * sizeof(FlowPending));
        CHECK(f->pending, "engine: OOM growing the flow's pending-fetch list");
    }
    f->pending[f->npend].resolve = JS_DupValue(ctx, resolve);
    f->pending[f->npend].value = JS_DupValue(ctx, value);
    f->pending[f->npend].url = url ? strdup(url) : NULL;
    CHECK(!url || f->pending[f->npend].url, "engine: OOM recording a pending fetch's URL");
    f->pending[f->npend].have_value = (url == NULL);
    f->pending[f->npend].kind = FLOW_PENDING_RESOLVE;
    f->pending[f->npend].script_i = -1;
    f->npend++;
}

/* PARK ON AN EXTERNAL DOCUMENT SCRIPT. Registered at most once per flow per slot — the flow is asked again on
   every scheduler pass while it waits, and a second registration would make the host owe the same URL twice. */
void engine_pending_docscript(JSContext *ctx, const char *url, int script_i) {
    Flow *f = flow_running();
    DCHECK(f != NULL, "an external document script was awaited outside a running flow");
    DCHECK(url != NULL && *url, "an external document script entry carries no URL");
    for (int i = 0; i < f->npend; i++)
        if (f->pending[i].kind == FLOW_PENDING_DOCSCRIPT && f->pending[i].script_i == script_i)
            return;
    if (f->npend >= f->pendcap) {
        f->pendcap = f->pendcap ? f->pendcap * 2 : 8;
        f->pending = realloc(f->pending, (size_t)f->pendcap * sizeof(FlowPending));
        CHECK(f->pending, "engine: OOM growing the flow's pending list");
    }
    f->pending[f->npend].resolve = JS_UNDEFINED;
    f->pending[f->npend].value = JS_UNDEFINED;
    f->pending[f->npend].url = strdup(url);
    CHECK(f->pending[f->npend].url, "engine: OOM recording an external script's URL");
    f->pending[f->npend].have_value = 0;
    f->pending[f->npend].kind = FLOW_PENDING_DOCSCRIPT;
    f->pending[f->npend].script_i = script_i;
    f->npend++;
    (void)ctx;
}

/* PARK ON AN INJECTED SCRIPT. `document.body.appendChild(s)` with `s.src` set is the other way a page loads code
   conditionally, and it has no promise for the reply to settle — the reply IS more program. The flow parks on
   the URL exactly as a fetch does (same register, same dedup, same stall accounting) and the drain queues the
   body as this flow's next script, so the loaded code runs in the world that injected it: its COW delta, its
   pins, its position in the BFS. A sibling that never took that branch never sees the script. */
void engine_pending_script_url(JSContext *ctx, const char *url) {
    Flow *f = flow_running();
    DCHECK(f != NULL, "a <script src> was injected outside a running flow");
    DCHECK(url != NULL && *url, "a <script src> was injected with no URL");
    if (f->npend >= f->pendcap) {
        f->pendcap = f->pendcap ? f->pendcap * 2 : 8;
        f->pending = realloc(f->pending, (size_t)f->pendcap * sizeof(FlowPending));
        CHECK(f->pending, "engine: OOM growing the flow's pending list");
    }
    f->pending[f->npend].resolve = JS_UNDEFINED;
    f->pending[f->npend].value = JS_UNDEFINED;
    f->pending[f->npend].url = strdup(url);
    CHECK(f->pending[f->npend].url, "engine: OOM recording an injected script's URL");
    f->pending[f->npend].have_value = 0;
    f->pending[f->npend].kind = FLOW_PENDING_SCRIPT;
    f->pending[f->npend].script_i = -1;
    f->npend++;
    (void)ctx;
}

/* THE SESSION'S SCRIPT SEQUENCE — the document's own scripts in order. Entry i is inline (bodies[i] is its text)
   or external (srcs[i] is its URL and bodies[i] is filled when the host replies). Declared here because the
   pending DRAIN writes into it: an external script's text is the DOCUMENT's, shared by every flow. */
/* The browser layer's document-load lifecycle, asked when a flow has run everything the document gave it. */
static int (*g_docdone_hook)(JSContext *ctx, int stage);
void engine_set_document_done_hook(int (*fn)(JSContext *ctx, int stage)) { g_docdone_hook = fn; }

static char **g_sess_bodies;
static char **g_sess_srcs;

/* THE URLS THE HOST OWES, newline-joined across every live flow, or "" — one register, the flows' own. The
   buffer is this function's and is valid until the next call. */
/* THE URLS THE HOST OWES, newline-joined across every live flow, DEDUPED. Several flows park on the same URL —
   a candidate re-fire re-runs the same fetches the exploring flow made — and engine_provide fills every entry
   that names it, so listing it twice makes the host provide twice and the second call finds nothing left. The
   list is a set of requests, not a list of waiters. */
const char *engine_pending_urls(void) {
    static char *join;
    size_t need = 1;
    Flow *f;

    free(join); join = NULL;
    for (int k = 0; (f = flow_at(k)) != NULL; k++)
        for (int i = 0; i < f->npend; i++)
            if (f->pending[i].url && !f->pending[i].have_value) need += strlen(f->pending[i].url) + 1;
    join = malloc(need);
    CHECK(join != NULL, "engine: OOM joining the pending URLs");
    join[0] = 0;
    for (int k = 0; (f = flow_at(k)) != NULL; k++)
        for (int i = 0; i < f->npend; i++) {
            const char *u = f->pending[i].url;
            if (!u || f->pending[i].have_value) continue;
            /* already listed? a linear scan over the answer being built, which is the set itself */
            {
                const char *p = join; int dup = 0; size_t ul = strlen(u);
                while (*p) {
                    const char *e = strchr(p, '\n');
                    size_t l = e ? (size_t)(e - p) : strlen(p);
                    if (l == ul && !memcmp(p, u, ul)) { dup = 1; break; }
                    if (!e) break;
                    p = e + 1;
                }
                if (dup) continue;
            }
            strcat(join, u); strcat(join, "\n");
        }
    return join;
}

/* Deliver a body for `url` into every flow parked on it. The value lands on the flow's OWN pending entry, so the
   reaction the resolve enqueues belongs to that flow and to its COW delta — which is why this is here and not in
   a register beside it. Returns how many entries it filled. */
int engine_provide(JSContext *ctx, const char *url, JSValueConst value) {
    int n = 0;
    DCHECK(url != NULL, "a body was provided for no URL");
    for (int k = 0; ; k++) { Flow *f = flow_at(k); if (!f) break;
        for (int i = 0; i < f->npend; i++) {
            FlowPending *p = &f->pending[i];
            if (!p->url || p->have_value || strcmp(p->url, url) != 0) continue;
            JS_FreeValue(ctx, p->value);
            p->value = JS_DupValue(ctx, value);
            p->have_value = 1;
            n++;
        }
    }
    return n;
}
/* Resolve every pending fetch this flow issued (the network completed). Returns how many were drained. */
/* Is any of this flow's pending fetches deliverable? A flow with only host-owed entries has no work — it stalls
   rather than spinning on a drain that would resolve nothing. */
static int flow_pending_ready(const Flow *f) {
    for (int i = 0; i < f->npend; i++) if (f->pending[i].have_value) return 1;
    return 0;
}

static int flow_drain_pending(JSContext *ctx, Flow *f) {
    int n = 0, keep = 0;
    for (int i = 0; i < f->npend; i++) {
        FlowPending *p = &f->pending[i];
        if (!p->have_value) { f->pending[keep++] = *p; continue; }   /* the host still owes this one */
        if (p->kind == FLOW_PENDING_DOCSCRIPT) {
            /* the DOCUMENT's text, shared by every flow: fill the slot once and all waiters proceed in order */
            if (!g_sess_bodies[p->script_i]) {
                const char *body = JS_ToCString(ctx, p->value);
                DCHECK(body != NULL, "an external document script's body did not arrive as text");
                g_sess_bodies[p->script_i] = body ? strdup(body) : strdup("");
                CHECK(g_sess_bodies[p->script_i], "engine: OOM storing an external document script");
                if (body) JS_FreeCString(ctx, body);
            }
        } else if (p->kind == FLOW_PENDING_SCRIPT) {
            /* the reply is PROGRAM: it joins this flow's script sequence, and the one BFS runs it */
            const char *body = JS_ToCString(ctx, p->value);
            DCHECK(body != NULL, "an injected script's body did not arrive as text");
            if (body) { engine_queue_script(body); JS_FreeCString(ctx, body); }
        } else {
            JSValue r = JS_Call(ctx, p->resolve, JS_UNDEFINED, 1, (JSValueConst *)&p->value);
            JS_FreeValue(ctx, r);
        }
        JS_FreeValue(ctx, p->resolve);
        JS_FreeValue(ctx, p->value);
        free(p->url);
        n++;
    }
    f->npend = keep;
    return n;
}

/* Snapshot-fork handoff: solver_decide stashes the sibling's hot decision + pins here at a forking branch;
   the interpreter then clones the frame and calls engine_fork_finalize, which assembles the sibling flow. */
static void *g_fork_dec = NULL, *g_fork_pins = NULL;
void engine_prepare_fork(void *dec_blob, void *pin_blob) { g_fork_dec = dec_blob; g_fork_pins = pin_blob; }

/* GENERATOR-STATE fork stash: clone_deep_flow fires gen_fork for each generator body frame it clones (during
   JS_FlowClone), BEFORE engine_fork_finalize exists to hold the sibling's delta. Append here; the finalize
   drains all onto the just-created sibling delta and resets. Filled-then-fully-drained within one fork. */
typedef struct { JSValueConst genobj; void *g0, *g1; } GenForkRec;
static GenForkRec *g_genforks = NULL; static int g_genfork_n = 0, g_genfork_cap = 0;
void engine_gen_fork(JSContext *ctx, JSValueConst genobj, void *base_gd, void *cur_gd) {
    (void)ctx;
    if (g_genfork_n >= g_genfork_cap) {
        g_genfork_cap = g_genfork_cap ? g_genfork_cap * 2 : 8;
        g_genforks = realloc(g_genforks, (size_t)g_genfork_cap * sizeof(GenForkRec));
        CHECK(g_genforks, "engine: OOM generator-fork stash");
    }
    g_genforks[g_genfork_n].genobj = genobj; g_genforks[g_genfork_n].g0 = base_gd; g_genforks[g_genfork_n].g1 = cur_gd;
    g_genfork_n++;
}

static void engine_fork_finalize(JSContext *ctx, JSValue *clone) {
    Flow *parent = flow_running();
    DCHECK(parent != NULL && g_fork_dec != NULL, "engine_fork_finalize: fork without a running flow / prepared state");
    Flow *sib = flow_add(ctx, parent->fn, NULL, 0);
    sib->started = 1;                 /* HOT: resume from the cloned frame + blobs, never a fresh re-run */
    sib->frame = clone;               /* the frame snapshot taken AT the branch */
    sib->script_i = parent->script_i; /* same position in the script sequence */
    sib->delta = cow_delta_fork(ctx, (CowDelta *)parent->delta);   /* O(1) shared base segment, then diverges */
    /* GENERATOR-STATE swaps built by clone_deep_flow for this fork: record each on the sibling's delta so the
       shared generator object resolves to the sibling's own cloned execution state while it runs. */
    for (int i = 0; i < g_genfork_n; i++)
        cow_delta_add_gendata(ctx, (CowDelta *)sib->delta, g_genforks[i].genobj, g_genforks[i].g0, g_genforks[i].g1);
    g_genfork_n = 0;
    /* DOM analog of the heap clone: freeze the parent's live DOM head into a SHARED refcounted base segment
       (refcount 2 — parent keeps a fresh empty head over it, sibling references it too), so the sibling INHERITS
       the parent's PRE-FORK document writes in O(1) instead of a copy, then each diverges on its own head. */
    sib->dom_base = dom_cow_fork();
    sib->dec_blob = g_fork_dec; g_fork_dec = NULL;
    sib->pin_blob = g_fork_pins; g_fork_pins = NULL;
    if (parent->dyn_n) {              /* inherit the lazy chunks loaded up to the branch */
        sib->dyn = malloc((size_t)parent->dyn_n * sizeof(char *)); CHECK(sib->dyn, "engine: OOM fork dyn");
        /* THE FLAGS COME WITH THE BODIES. A field added to the queue is an obligation at every clone, free and
           finish site; the sibling inheriting bodies without knowing which are candidates would re-arm the
           page-script assert on a dead breakout it inherited. */
        sib->dyn_cand = malloc((size_t)parent->dyn_n); CHECK(sib->dyn_cand, "engine: OOM fork dyn flags");
        for (int i = 0; i < parent->dyn_n; i++) {
            sib->dyn[i] = strdup(parent->dyn[i]); CHECK(sib->dyn[i], "engine: OOM fork dyn body");
            sib->dyn_cand[i] = parent->dyn_cand ? parent->dyn_cand[i] : 0;
        }
        sib->dyn_n = sib->dyn_cap = parent->dyn_n;
    }
    sib->dom_stage = parent->dom_stage;   /* the sibling is at the same point in the document's lifecycle */
    /* THE REPLIES STILL IN FLIGHT ARE INHERITED TOO. A flow that forks while a request is outstanding — a
       fetch whose `.then` has not run, an injected <script src> whose body has not arrived — was leaving the
       sibling with an empty register, so the reply reached exactly one world and everything behind it was
       silently missing from the other. Both arms wait on the same URL (engine_pending_urls dedups it, and
       engine_provide fills every entry that names it), and each then delivers on its OWN timeline: the resolve
       function is shared, but its already_resolved latch and the promise's settlement are per-flow state the
       COW delta captures, which is precisely what lets both arms settle one capability. */
    /* THE QUEUED JOBS ARE INHERITED FOR THE SAME REASON THE REPLIES ARE, and their absence was the same bug
       one layer down: a flow that forks with reactions still queued — a `.then` attached before the branch, a
       custom-element reaction, a listener task — left the sibling with an EMPTY queue, so that arm silently
       never ran them. Every one of those is a first-class flow in the one BFS, and dropping a work item is the
       thing the WFQ is forbidden to do; a fork that drops them is the same violation with a different spelling.
       It surfaced as a rejection reported unhandled in the arm whose `.catch` job never arrived — the report
       was right about its own world, and its world was missing a job. */
    if (parent->njob) {
        sib->jobs = malloc((size_t)parent->njob * sizeof(FlowJob));
        CHECK(sib->jobs, "engine: OOM inheriting the queued jobs at a fork");
        for (int i = 0; i < parent->njob; i++) {
            FlowJob *sj = &parent->jobs[i], *dj = &sib->jobs[i];
            dj->fn = sj->fn;
            dj->argc = sj->argc;
            dj->argv = sj->argc ? malloc((size_t)sj->argc * sizeof(JSValue)) : NULL;
            CHECK(!sj->argc || dj->argv, "engine: OOM inheriting a queued job's arguments at a fork");
            for (int a = 0; a < sj->argc; a++) dj->argv[a] = JS_DupValue(ctx, sj->argv[a]);
        }
        sib->njob = sib->jobcap = parent->njob;
    }
    if (parent->npend) {
        sib->pending = malloc((size_t)parent->npend * sizeof(FlowPending));
        CHECK(sib->pending, "engine: OOM inheriting the pending replies at a fork");
        for (int i = 0; i < parent->npend; i++) {
            FlowPending *sp = &parent->pending[i], *dp = &sib->pending[i];
            dp->resolve = JS_DupValue(ctx, sp->resolve);
            dp->value = JS_DupValue(ctx, sp->value);
            dp->url = sp->url ? strdup(sp->url) : NULL;
            CHECK(!sp->url || dp->url, "engine: OOM inheriting a pending URL at a fork");
            dp->have_value = sp->have_value;
            dp->kind = sp->kind;
        }
        sib->npend = sib->pendcap = parent->npend;
    }
}

/* The frame-agnostic REPLAY fork is DELETED: re-running a nested/deep flow from its start is BANNED (not
   byte-identical — shared state can differ between the run and the re-run). A concolic branch inside an async
   body on the tramp chain now DFAILs in the engine (see branch_arm_fork) until the sound async-frame snapshot
   is built; there is no re-run fallback to hide that gap. */

static void engine_queue(const char *body, int is_candidate) {
    Flow *f = flow_running();   /* the running flow owns the lazy chunk it loads */
    if (!body || !f) return;
    if (f->dyn_n >= f->dyn_cap) {
        f->dyn_cap = f->dyn_cap ? f->dyn_cap * 2 : 8;
        f->dyn = realloc(f->dyn, (size_t)f->dyn_cap * sizeof(char *));
        f->dyn_cand = realloc(f->dyn_cand, (size_t)f->dyn_cap);
        CHECK(f->dyn && f->dyn_cand, "engine: OOM dynamic-script queue");
    }
    f->dyn[f->dyn_n] = strdup(body); CHECK(f->dyn[f->dyn_n], "engine: OOM dynamic-script body");
    f->dyn_cand[f->dyn_n] = (unsigned char)(is_candidate != 0);
    f->dyn_n++;
}

void engine_queue_script(const char *body) { engine_queue(body, 0); }

/* AN @S CANDIDATE, queued as the program it would be if it fired. It is the same queue because it IS the same
   thing — code the page caused to run — but it carries the one difference that matters: it is allowed not to
   compile. Most breakouts do not fit most sink contexts, which is exactly why the solver tries several and
   keeps whichever FIRES; a candidate that does not parse simply never fires. */
void engine_queue_candidate(const char *body) { engine_queue(body, 1); }

/* Preempt hook, two orthogonal yield decisions at the one per-back-edge check:
   (1) VALUE yield — suspend the running flow the MOMENT a parked flow outranks it (the WFQ, not a clock,
       decides which flow runs). The rival is recomputed only when the frontier membership changes (a fork
       adds a flow) or the running flow switches — cached by (gen, cur) so this is O(1) per back-edge, never
       an O(flows) scan per opcode.
   (2) COOPERATIVE-QUANTUM yield — a thread-sharing floor: even a top-ranked flow breathes every Q back-edges
       so the host loop can interleave / pump / snapshot. NOT a step cap: it drops/reorders no flow and the
       flow resumes byte-identically. */
/* THE COOPERATIVE QUANTUM IS WALL-CLOCK, and it was a COUNT — which §scheduler bans by name: the yield comes
   "after a bounded wall-clock slice", "never after N opcodes (an opcode-budget counter is a step cap, banned)".
   The difference is not academic. Measured on the fixture's main program: 64 suspend points were offered over
   FIVE SECONDS, the WFQ declined 63 of them because no sibling outranked the runner, and the 64th tick was the
   only thing that ever parked it. A count cannot bound a slice when the work between two suspend points is
   ~78ms; only the clock can. Reading it per consultation is the point — a stride would reintroduce exactly the
   count this replaces, and at any stride worth having the same five seconds fit inside it. */
static int64_t engine_now_ms(void);   /* the slice's clock, defined with the session below */
static int64_t g_slice_start = 0;
static void engine_slice_begin(void) { g_slice_start = engine_now_ms(); }
static unsigned g_seen_gen = 0; static Flow *g_seen_cur = NULL; static int g_outranked = 0;
/* SUSPEND POINTS REACHED — every call to this hook IS one, which is the number the seam assertion needs and
   the one quickjs's counters do not give. g_flow_preempt_requested is incremented only where the hook returns
   TRUE, so it counts preempts WANTED, not points offered: a step showing requested=1 may have reached one
   suspend point or a million with the WFQ declining every one of them. Reading it as the latter is a mistake I
   made and wrote into a commit message; this counter is what tells the two apart. */
static uint64_t g_preempt_asked = 0;
/* THE GAP BETWEEN SUSPEND POINTS is the quantity the contract is about, and it is not the same as how long a
   step ran. A step that offers the scheduler a point every few milliseconds and still runs for ten seconds is
   BEHAVING — the scheduler was asked and declined, which is a ranking decision and lossless. A step that runs
   five seconds between two consecutive offers is the violation, whatever its total. Asserting on the total
   conflated the two and cost several rounds of chasing a "missing seam" in a step that turned out to offer 22
   points quickly and then one long gap. Measured per step: reset when the step starts, updated at each
   consultation, and closed off with the tail after the last one. */
static int64_t g_last_ask = 0, g_max_gap = 0;

/* The solver's policy does not care WHICH kind of point it was offered — its two decisions are the WFQ ranking
   and the wall-clock slice, and both ask whether this flow should still hold the thread. */
static int preempt_hook(int kind) {
    (void)kind;
    Flow *cur = flow_running();
    int64_t now = engine_now_ms();
    g_preempt_asked++;
    if (now - g_last_ask > g_max_gap) g_max_gap = now - g_last_ask;
    g_last_ask = now;
    if (flow_frontier_gen() != g_seen_gen || cur != g_seen_cur) {   /* (1) recompute rival only on change */
        g_seen_gen = flow_frontier_gen(); g_seen_cur = cur;
        Flow *rival = cur ? flow_best_other(cur) : NULL;
        g_outranked = (rival && cur && flow_weight(rival) > flow_weight(cur));
    }
    if (g_outranked) return 1;                        /* value yield */
    /* (2) COOPERATIVE-QUANTUM floor — thread-sharing, not value. Nothing is dropped, starved or reordered
       across it: the flow parks and the SAME flow resumes byte-identically unless the WFQ says otherwise. */
    return now - g_slice_start >= ENGINE_QUANTUM_MS;
}

/* Advance flow `f` by up to one quantum. Returns 1 when the flow has FINISHED all its scripts + lazy chunks,
   0 when it yielded mid-execution (resume it later). Each <script>/chunk is its OWN program (JS_FlowNew) run
   in document order in the shared context, under f's COW delta (set by the caller). */
/* ASYNC-AS-FLOW job-enqueue hook (installed as JS_SetJobEnqueueHook): route a promise reaction / microtask to
   the ENQUEUING flow's own queue instead of the global list, so it runs later under that flow's live COW. */
static int engine_enqueue_job(JSContext *ctx, JSJobFunc *fn, int argc, JSValueConst *argv) {
    Flow *f = flow_running();
    if (!f) return 0;   /* enqueued outside a flow (baseline setup) -> let the fork use its default global list */
    if (f->njob >= f->jobcap) {
        f->jobcap = f->jobcap ? f->jobcap * 2 : 4;
        f->jobs = realloc(f->jobs, (size_t)f->jobcap * sizeof(FlowJob));
        CHECK(f->jobs, "engine: OOM flow job queue — a dropped reaction corrupts async exploration");
    }
    FlowJob *j = &f->jobs[f->njob++];
    j->fn = fn; j->argc = argc;
    j->argv = argc ? malloc((size_t)argc * sizeof(JSValue)) : NULL;
    if (argc) CHECK(j->argv, "engine: OOM job argv");
    for (int i = 0; i < argc; i++) j->argv[i] = JS_DupValue(ctx, argv[i]);
    return 1;   /* host owns it */
}

/* Run ONE of the flow's queued jobs (FIFO) under its currently-applied COW; free its args + result. */
static void flow_run_one_job(JSContext *ctx, Flow *f) {
    FlowJob j = f->jobs[0];
    memmove(f->jobs, f->jobs + 1, (size_t)(--f->njob) * sizeof(FlowJob));   /* FIFO pop */
    JSValue r = j.fn(ctx, j.argc, (JSValueConst *)j.argv);   /* the reaction runs in this flow's timeline */
    JS_FreeValue(ctx, r);
    for (int i = 0; i < j.argc; i++) JS_FreeValue(ctx, j.argv[i]);
    free(j.argv);
}

/* ONE UNIT OF WORK, THEN RETURN — flow_step is a step, and it used to be a drain.
   Every branch below that finished something looped back inside this call instead of returning: a completed
   script advanced to the next one and ran it, a drained fetch ran the continuation, a fired load stage ran its
   listeners, a candidate that failed to compile went straight to the next candidate. So a flow holding many
   short programs — none of them long enough to hit a back-edge preempt — ran ALL of them back-to-back with no
   return to the scheduler, which is a drive-to-completion at the C level even though every individual program
   was perfectly preemptible. The scheduler could not interleave, could not re-rank, and could not honour its
   own wall-clock quantum, because none of them are consulted until this returns.
   Making each unit a return puts the scheduler back in charge of the pump, which is what §scheduler requires of
   it, and costs only loop iterations: the switch counter moves when a DIFFERENT flow is picked, so re-picking
   the same flow is the same execution with the scheduler given the chance to choose otherwise. Nothing is
   dropped, skipped or reordered by it — every branch that returns here made progress first. */
/* WHICH UNIT, when one of them turns out to have no suspend point. The scheduler's assertion can say that a
   flow ran too long but not what it was doing, and "one of seven branches" is not a localisation — the label is
   set by the branch that is about to run, so a step with no seam names itself. */
static const char *g_step_unit = "(none)";

static int flow_step(JSContext *ctx, Flow *f, char **bodies, int n) {
    for (;;) {
        /* THE PARKED CONTINUATION OUTRANKS EVERYTHING ELSE THIS FLOW COULD DO — that is the park's whole
           contract: a forced preempt must be transparent to observable ordering, so the flow resumes BEFORE any
           job it has queued. Yielding after one resume keeps the scheduler in charge of fairness; the park (if
           it parks again immediately) rides the switch-out with the flow. Without this the solver host never
           pumped the slot at all: the continuation sat there until a second flow parked and asserted. */
        g_step_unit = "resume-parked-continuation";
        if (JS_ResumeParkedFlow(JS_GetRuntime(ctx))) return 0;
        if (!f->frame) {
            const char *body;
            int is_cand = 0;   /* an @S candidate is allowed not to parse; a page script is not */
            if (f->script_i < n) {
                body = bodies[f->script_i];
                if (!body) {
                    /* AN EXTERNAL DOCUMENT SCRIPT whose text has not arrived. Classic scripts run in document
                       order, so the flow WAITS here rather than skipping ahead — running what comes after a
                       bundle before the bundle is a different program. The text is the DOCUMENT's, not the
                       flow's: every flow runs the same bytes, so the reply fills the shared slot and every
                       waiting flow proceeds. */
                    /* A reply that has already arrived is delivered FIRST — this branch returns before the
                       drain below, so parking without checking would leave the flow owed forever on a URL the
                       host had already answered. */
                    if (flow_pending_ready(f)) { flow_drain_pending(ctx, f); return 0; }
                    engine_pending_docscript(ctx, g_sess_srcs[f->script_i], f->script_i);
                    return FLOW_STEP_OWED;
                }
            }
            else if (f->script_i - n < f->dyn_n) { body = f->dyn[f->script_i - n];
                                                   is_cand = f->dyn_cand[f->script_i - n]; }
            else if (f->njob > 0) { g_step_unit = "run-one-job"; flow_run_one_job(ctx, f); return 0; }   /* scripts done -> drain a microtask, yield */
            else if (f->npend > 0 && !flow_pending_ready(f))
                return FLOW_STEP_OWED;   /* only host-owed replies remain: no progress, and NOT finished */
            else if (flow_pending_ready(f)) {
                /* FETCH-AWAIT: scripts + microtasks are drained, but a suspended async body is awaiting a LIVE
                   fetch (a pending promise). The network completes now: resolve THIS flow's pending fetches — each
                   awaiting async body's reaction is enqueued as a job in this flow's queue (we are switched in,
                   flow_running == f) — then loop to run those jobs and resume the continuations. */
                g_step_unit = "drain-pending-fetch";
                flow_drain_pending(ctx, f);
                return 0;
            }
            else if (f->dom_stage < 2 && g_docdone_hook) {
                /* THE DOCUMENT FINISHED LOADING, in this flow's world. DOMContentLoaded then load, in that
                   order, each once per flow — the order IS the spec, and a page's real work is behind them:
                   the half of a bundle that touches the DOM and calls the API runs here. Every listener is
                   queued as a task, so the loop above picks them up like any other job. */
                g_step_unit = "document-done-stage";
                g_docdone_hook(ctx, f->dom_stage);
                f->dom_stage++;
                return 0;
            }
            /* HTML §8.1.7.5 "notify about rejected promises". The flow has nothing left to run, so every
               rejection still on its list is one no handler will ever be attached to. The browser half keeps
               the lists and fires `unhandledrejection`; those fires are JOBS, so the flow has work again and
               the loop picks them up like any other. Only what the page did not cancel comes back through the
               report hook — and what it means then is this half's answer, the same thing a script that threw
               means: a capability the page needed. Notifying clears the list, so the next pass finds none. */
            else if ((g_step_unit = "unhandled-rejection-notify", unhandled_rejection_notify(ctx))) return 0;
            else return 1;   /* all scripts + chunks + microtask jobs + live fetches + load listeners done */
            /* NULL ScriptOrModule name: an inline page script's name is the DOCUMENT's URL, which this host does
               not model yet — nothing here has one to give. It is what a relative `import('./chunk.js')` resolves
               against, so the moat's lazy-chunk surface needs the document URL plumbed to this call. */
            g_step_unit = "compile-program";
            f->frame = JS_FlowNew(ctx, body, strlen(body), NULL, 0);   /* page <script>/chunk: classic non-strict global */
            if (f->frame == NULL) {
                /* AN @S CANDIDATE THAT DOES NOT PARSE is a dead candidate and nothing more — the search tries
                   several breakouts per sink precisely because most do not fit most contexts. A PAGE script that
                   does not compile is a different thing entirely and still asserts. */
                DCHECK(is_cand, "flow_step: a page <script>/chunk did not compile");
                JS_FreeValue(ctx, JS_GetException(ctx));
                return 0;
            }
        }
        {
            /* A <script>'s completion value is not observable to the page (only an eval API surfaces one), so it is
               taken and released here — never DISCARDED by the engine, which would hide a live value from the host. */
            JSValue cv = JS_UNDEFINED;
            g_step_unit = "resume-program";
            int r = JS_FlowResume(ctx, (JSValue *)f->frame, &cv);
            /* A SCRIPT THAT THREW names a capability the page needed and this engine does not have. Ending the
               flow there is intentional; losing WHICH capability was not. */
            if (JS_IsException(cv)) {
                JSValue e = JS_GetException(ctx);
                result_page_error_value(ctx, e);
                JS_FreeValue(ctx, e);
            }
            JS_FreeValue(ctx, cv);
            if (r == 1) return 0;   /* quantum yield — more work, resume later */
            if (r == JS_FLOW_DETACHED) {
                /* the base registered itself as a continuation elsewhere (a module body's top-level await): it
                   is no longer this flow's to free, and the awaited promise will drive it from here. */
                f->frame = NULL; f->script_i++;
                return 0;
            }
        }
        JS_FlowFree(ctx, (JSValue *)f->frame); f->frame = NULL; f->script_i++;   /* this script done -> next */
        return 0;
    }
}

/* Context switches performed by the dispatch loop, for the result document (result.h). Cumulative for the
   life of this engine — one wasm instance is one document, so that is the document's count. */
static int g_switches = 0;
int engine_switch_count(void) { return g_switches; }

static void flow_switch_out(JSContext *ctx, Flow *f) {   /* pause f: snapshot its solver state, restore baseline */
    /* the PARKED CONTINUATION travels with the flow, for the reason the delta does: it resumes a suspended
       async activation of THIS flow, under THIS flow's heap. Left in the runtime it would be resumed by
       whichever flow the scheduler picked next — against the wrong delta — or, if that flow parked too, hit
       JS_ParkFlow's one-slot assertion, which is exactly what the smoke test was aborting on. */
    { JSContext *pc; JSFlowParkFn *pf; void *po;
      if (JS_TakeParkedFlow(JS_GetRuntime(ctx), &pc, &pf, &po)) {
          f->park_ctx = pc; f->park_fn = (void *)pf; f->park_opaque = po;
      } }
    f->dec_blob = decide_suspend();
    f->pin_blob = concolic_pins_suspend();
    cow_unapply(ctx, (CowDelta *)f->delta);
    cow_set_current(NULL);
    dom_unapply();                                  /* DOM twin of cow_unapply: restore the baseline document */
    f->dom = dom_buf_take(&f->dom_n, &f->dom_cap);  /* detach this flow's DOM head so the global is empty for the next flow */
    f->dom_base = dom_base_take();                  /* ...and its shared base chain (NULL until a DOM fork) */
    flow_set_running(NULL);
}

static void flow_switch_in(JSContext *ctx, Flow *f) {   /* resume/start f: apply its delta + solver state */
    JS_PutParkedFlow(JS_GetRuntime(ctx), (JSContext *)f->park_ctx, (JSFlowParkFn *)f->park_fn, f->park_opaque);
    f->park_ctx = NULL; f->park_fn = NULL; f->park_opaque = NULL;
    if (!f->delta) f->delta = cow_delta_new();
    cow_set_current((CowDelta *)f->delta);
    cow_apply(ctx, (CowDelta *)f->delta);
    dom_buf_load(f->dom, f->dom_n, f->dom_cap);   /* attach this flow's DOM head (NULL/0 for a fresh flow = empty) */
    dom_base_load(f->dom_base);                   /* ...and its base chain, BEFORE dom_apply walks it */
    dom_apply();                                  /* DOM twin of cow_apply: replay this flow's document writes */
    if (!f->started) { f->started = 1; decide_enter(ctx, f); }   /* fresh flow: replay from cursor 0 */
    else {                                                        /* paused flow: restore where it left off */
        decide_resume(f->dec_blob, f->fn);   decide_blob_free(f->dec_blob); f->dec_blob = NULL;
        concolic_pins_resume(f->pin_blob);   concolic_pins_blob_free(f->pin_blob); f->pin_blob = NULL;
    }
    flow_set_running(f);
}

static void flow_finish(JSContext *ctx, Flow *f) {   /* f completed: tear down its interleaving state + remove */
    /* "all scripts, chunks, jobs and fetches are done" cannot be true with a continuation still parked — the
       loop above resumes one before it can answer that. Asserting it here is what keeps the park inside the
       no-work-item-is-ever-dropped rule rather than merely intending to. */
    DCHECK(!JS_HasParkedFlow(JS_GetRuntime(ctx)) && f->park_fn == NULL,
           "a flow finished with a continuation still parked — that flow's async activation is dropped");
    decide_leave(ctx);
    cow_unapply(ctx, (CowDelta *)f->delta); cow_set_current(NULL);
    cow_delta_free(ctx, (CowDelta *)f->delta); f->delta = NULL;
    /* f is CURRENT here (its head+base are loaded as the globals, and the head may have realloc'd during the run
       so f->dom is stale). dom_revert restores baseline + frees the head entries + unrefs the base chain; then
       free the now-empty global head ARRAY. Never touch the stale f->dom/f->dom_base — the live buffers are the
       globals. */
    dom_revert();
    { int dn, dc; free(dom_buf_take(&dn, &dc)); }
    f->dom = NULL; f->dom_n = f->dom_cap = 0; f->dom_base = NULL;
    for (int i = 0; i < f->dyn_n; i++) free(f->dyn[i]);
    free(f->dyn); f->dyn = NULL;
    free(f->dyn_cand); f->dyn_cand = NULL;
    f->dyn_n = f->dyn_cap = 0;
    /* flow_step returns 1 (finished) only with an empty job queue, but free any residual defensively. */
    for (int i = 0; i < f->njob; i++) { for (int k = 0; k < f->jobs[i].argc; k++) JS_FreeValue(ctx, f->jobs[i].argv[k]); free(f->jobs[i].argv); }
    free(f->jobs); f->jobs = NULL; f->njob = f->jobcap = 0;
    /* FETCH-AWAIT: flow_step drains pending before finishing, but free any residual (resolve capabilities + values). */
    for (int i = 0; i < f->npend; i++) { JS_FreeValue(ctx, f->pending[i].resolve); JS_FreeValue(ctx, f->pending[i].value); free(f->pending[i].url); }
    free(f->pending); f->pending = NULL; f->npend = f->pendcap = 0;
    flow_set_running(NULL);
    flow_remove(ctx, f);
}

/* THE ONE BFS SCHEDULER — explore and @S candidate-verify are the SAME loop, differing ONLY in whether a concolic
   branch FORKS. A separate verify executor (a `while(JS_FlowResume){}` driving one candidate to completion with
   preemption off) is the cardinal violation twice over — a second scheduler beside the BFS AND a drive-to-
   completion (an unbounded candidate loop would hang, non-parkable). So verify is this same loop with forking off:
   ONE concrete path (no branch/fork hook), yet every candidate flow is preemptible + parkable like any other. */
static const JSFlowControlHooks FC_EXPLORE = { .branch = solver_decide, .fork = engine_fork_finalize, .preempt = preempt_hook };
static const JSFlowControlHooks FC_VERIFY  = { .preempt = preempt_hook };   /* candidate re-fire: no fork, still preemptible */
static const JSFlowControlHooks FC_OFF     = { 0 };

/* The quantum's wall clock. CLOCK_MONOTONIC, because the slice is about elapsed thread time and a wall-clock
   adjustment must not shorten or extend it. */
static int64_t engine_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* THE SESSION. The dispatch loop is not a function that drains — it is a state machine its HOST steps, because
   the cooperative-quantum yield in CLAUDE.md's §scheduler is exactly that: after a bounded wall-clock slice the
   scheduler RETURNS so the one thread pumps its message port, streams findings, interleaves other documents,
   and then resumes the byte-identical frontier. A `for(;;)` that runs to exhaustion cannot do any of it, and in
   the extension it freezes the worker outright. The state that used to be the loop's C locals lives here so a
   return between two iterations costs nothing to resume from. */
static JSContext *g_sess_ctx;
static int g_sess_n;
static Flow *g_sess_cur;
static int g_sess_live;

void engine_sched_begin(JSContext *ctx, char **bodies, char **srcs, int n, int forking) {
    DCHECK(!g_sess_live, "engine_sched_begin while a session is already running — one scheduler, one session");
    g_sess_ctx = ctx; g_sess_bodies = bodies; g_sess_srcs = srcs; g_sess_n = n; g_sess_cur = NULL; g_sess_live = 1;
    /* WHAT AN UNCANCELLED REJECTION MEANS is this half's answer: the browser half fires the event and honours
       preventDefault, and a reason that survives that is a page error exactly like a script that threw. */
    unhandled_rejection_set_report_hook(result_page_error_value);
    flow_add(ctx, JS_UNDEFINED, NULL, 0);   /* the first flow: the page's scripts, empty decision vector */
    JS_SetFlowLocalMark(1);                 /* objects created while a flow runs are flow-local (discarded) */
    dom_cow_set_ctx(ctx);                   /* the DOM delta needs ctx for the attribute taint-shadow dup/free */
    g_dom_capture = 1;                       /* record DOM writes into the running flow's delta (twin of FlowLocalMark) */
    JS_SetFlowControlHooks(forking ? &FC_EXPLORE : &FC_VERIFY);   /* preempt ALWAYS on; fork only when exploring */
    JS_SetJobEnqueueHook(engine_enqueue_job);   /* ASYNC-AS-FLOW: reactions route to the enqueuing flow's queue */
}

/* One QUANTUM. Returns ENGINE_STEP_DONE when the frontier is empty (the session is closed and its hooks are
   uninstalled) and ENGINE_STEP_YIELD when the slice expired with the frontier intact. The slice is wall-clock
   and it is NOT a cap: nothing is dropped, starved, reordered or forgotten across it — the next call resumes the
   same top flow on the same frontier, which is the razor §scheduler states. */
static int (*g_stall_hook)(void);
void engine_set_stall_hook(int (*owed)(void)) { g_stall_hook = owed; }

static double g_yield_floor = -1.0 / 0.0;
void engine_set_yield_floor(double w) { g_yield_floor = w; }

double engine_top_weight(void) {
    Flow *b = flow_best();
    return b ? flow_weight(b) : -1.0 / 0.0;
}

int engine_sched_step(void) {
    JSContext *ctx = g_sess_ctx;
    char **bodies = g_sess_bodies;
    int n = g_sess_n;
    Flow *cur = g_sess_cur;
    int64_t deadline = engine_now_ms() + ENGINE_QUANTUM_MS;
    engine_slice_begin();   /* the hook's floor measures THIS slice, so it starts when the slice does */
    int owed = 0;   /* consecutive picks that could not progress; == flow_count() means every member is waiting */
    DCHECK(g_sess_live, "engine_sched_step with no live session");
    for (;;) {
        Flow *best = flow_best();           /* WFQ: highest value-of-information — a fresh fork (UCB) can preempt */
        if (!best) break;
        if (best != cur) {                  /* context switch: swap COW delta + decision + pins */
            if (cur) flow_switch_out(ctx, cur);
            flow_switch_in(ctx, best);
            solve_flow_begin(best);   /* the substitution is live only while its own flow runs */
            cur = best;
            /* COUNTED, and it leaves in the result document. A scheduler that interleaves and one that runs
               its flows FIFO produce the same endpoints on an easy page and diverge on every hard one, so
               "does it actually switch" cannot be inferred from the findings — it has to be reported. The
               host's WFQ reads it for exactly that. Cumulative across steps: the host wants the document's
               total, not the last slice's. */
            g_switches++;
        }
        flow_age_running(1);   /* this step burned CPU; flow_credit_emit resets it when the flow emits value */
        {
#if APICLIENT_DEV
            int64_t t0 = engine_now_ms();
            uint64_t pq0 = 0, pf0 = 0, pa0 = g_preempt_asked;
            JS_FlowPreemptStats(&pq0, &pf0);
            g_max_gap = 0; g_last_ask = t0;   /* this step's gaps, measured from the moment it starts */
            idl_slowest_reset();              /* ...and this step's slowest single Web API member step */
#endif
            int r = flow_step(ctx, cur, bodies, n);
            /* THE COOPERATIVE-QUANTUM CONTRACT, ASSERTED AT ITS SITE. A flow_step is supposed to reach a
               suspend point — a bytecode back-edge where the preempt hook runs, a step machine's boundary —
               within the quantum, which is what makes the frontier parkable at all. A path with NO suspend
               point on it does not slow the run down, it STOPS it: the deadline below is never reached, the
               scheduler never returns, and the whole engine spins at 100% with the switch count frozen.
               Nothing else catches that. The preempt-fired-vs-requested stat cannot: a pure C loop reaches no
               back-edge, so no preempt is ever REQUESTED and the ratio stays a perfect 100% while the engine
               hangs. So the hang was silent, and localising one meant bisecting the fixture by hand.
               This is NOT a bound — it truncates nothing, drops no flow and is compiled out of release. It
               asserts that the path the flow just ran HAS the suspend/resume seam the design requires, and it
               names the flow so the missing seam identifies itself instead of having to be hunted. The margin
               is deliberately enormous (400x the 12ms quantum): anything under it is merely slow, anything over
               it has no seam at all. */
#if APICLIENT_DEV
            {
                int64_t done = engine_now_ms();
                int64_t spent = done - t0;
                /* the TAIL closes the last gap: a step that offers a point and then runs for seconds before
                   returning has that silence between its last offer and its end, and nothing else records it. */
                int64_t gap = done - g_last_ask > g_max_gap ? done - g_last_ask : g_max_gap;
                if (gap > ENGINE_QUANTUM_MS * 400) {
                    char why[448];
                    int wi = 0;
                    const char *sk = cur && cur->cand_sink ? cur->cand_sink : "(exploration flow)";
                    const char *pl = cur ? cur->cand_payload : NULL;
                    /* WHICH PROGRAM. "a resume ran 5s" is still a symptom until the JS it was running is named,
                       and the flow already knows: script_i indexes the document's scripts and then the flow's own
                       dynamic bodies (a lazy chunk, an injected <script>, a fired PoC). Without this the only way
                       left to find the code is bisecting the fixture by hand, which is the thing this assertion
                       exists to replace. */
                    const char *bodytxt = NULL;
                    int si = cur ? cur->script_i : -1;
                    if (cur) {
                        if (si < n) bodytxt = bodies[si];
                        else if (si - n < cur->dyn_n) bodytxt = cur->dyn[si - n];
                    }
                    uint64_t pq = 0, pf = 0;
                    const char *slow_name = "(none)";
                    int64_t slow_ms = idl_slowest_step(&slow_name);
                    JS_FlowPreemptStats(&pq, &pf);
                    /* THREE numbers, because two of them cannot separate the roots. `asked` is how many suspend
                       points the path OFFERED (every consultation of the hook); `requested` is how many of those
                       the WFQ wanted to take; `fired` is how many actually parked. asked==0 means the path has
                       no suspend point on it at all — the seam is missing and must be built. asked>0 with
                       requested==0 means the points were there and the scheduler declined every one, which is a
                       ranking question, not a missing seam. requested>fired means a point was reached, the
                       preempt was wanted, and it was DROPPED because no driver at that depth adopts the seam. */
                    wi += snprintf(why, sizeof why,
                                   "%d ms passed with NO suspend point offered (step ran %d ms, points "
                                   "asked=%llu, preempts wanted=%llu fired=%llu; slowest Web API member step: "
                                   "%s %dms) — this stretch has no suspend/resume seam; unit=%s script_i=%d "
                                   "flow=%s payload=",
                                   (int)gap, (int)spent, (unsigned long long)(g_preempt_asked - pa0),
                                   (unsigned long long)(pq - pq0),
                                   (unsigned long long)(pf - pf0), slow_name, (int)slow_ms,
                                   g_step_unit, si, sk);
                    /* The payload is attacker-shaped bytes and the message lands inside JSON unescaped, so
                       anything that would break the line is replaced rather than emitted. */
                    for (; pl && *pl && wi < (int)sizeof why - 2; pl++, wi++)
                        why[wi] = (*pl < 0x20 || *pl > 0x7E || *pl == '"' || *pl == '\\') ? '.' : *pl;
                    if (bodytxt) {
                        const char *b = bodytxt;
                        wi += snprintf(why + wi, sizeof why - (size_t)wi, " body=");
                        for (; *b && wi < (int)sizeof why - 2; b++, wi++)
                            why[wi] = (*b < 0x20 || *b > 0x7E || *b == '"' || *b == '\\') ? '.' : *b;
                    }
                    why[wi] = 0;
                    DFAIL(why);
                }
            }
#endif
            if (r == FLOW_STEP_DONE) { solve_flow_end(cur); flow_finish(ctx, cur); cur = NULL; owed = 0; }
            else if (r == FLOW_STEP_OWED) {
                /* This flow can make no progress until the host supplies a reply. It is NOT skipped and NOT
                   removed — it stays in the WFQ at its own weight, and the scheduler simply observes that it
                   picked it and got nowhere. Once EVERY member has answered that in a row, no member can
                   progress and the frontier is stalled. Counting the answers is what makes this lossless: a
                   flow that gains work is picked again and resets the count, and nothing was ever excluded. */
                if (++owed >= flow_count())
                    break;
            }
            else owed = 0;
        }
        if (engine_now_ms() >= deadline) {   /* THREAD-SHARING, not value: hand the thread back, keep the frontier */
            g_sess_cur = cur;
            return ENGINE_STEP_YIELD;
        }
        /* VALUE: this engine's best is now worth less than the runner-up engine's, so the thread belongs there.
           The flow keeps its snapshot and resumes where it stands — an order decision, never a drop. */
        if (cur && flow_weight(cur) < g_yield_floor) {
            g_sess_cur = cur;
            return ENGINE_STEP_YIELD;
        }
        if (engine_top_weight() < g_yield_floor) {   /* VALUE: a better DOCUMENT is waiting — same lossless yield */
            g_sess_cur = cur;
            return ENGINE_STEP_YIELD;
        }
    }
    g_sess_cur = cur;
    /* The exploration found sinks; each breakout is a FLOW on this same frontier, seeded once the exploring
       flows are done so a candidate never re-fires against a half-explored page. Seeding adds members, so the
       loop above has more to do — hence before the exhausted answer, not after it.
       ASKED EVERY TIME THE FRONTIER DRAINS, not once. A sink inside a lazily-imported chunk or an injected
       <script src> is discovered AFTER the first drain, because the code holding it had not arrived yet; a
       one-shot latch left every such sink unsearched, which bounded verification by when a sink was found. The
       seeding is per-sink and idempotent, so asking again costs a scan and adds only what is new. */
    if (solve_seed_candidates(ctx) && flow_best())
        return ENGINE_STEP_YIELD;
    /* STALLED, not exhausted: the run-queue is empty but flows are parked on something only the host can
       supply. Ask the one seam BEFORE closing — the session and every parked snapshot stay live, and the host
       steps again once it has provided. */
    if (g_stall_hook && g_stall_hook())
        return ENGINE_STEP_STALLED;
    /* ASYNC-AS-FLOW forcing function: every flow has run to completion, so NO microtask/promise reaction may
       still be queued. If one is, the scheduler DROPPED it — the not-yet-built async-as-flow capability (a
       reaction must become a first-class scheduler flow carrying the queuing flow's COW, which needs a fork
       job-enqueue hook). Crash LOUD here rather than silently drop it, so the gap cannot hide. */
    DCHECK(!JS_IsJobPending(JS_GetRuntime(ctx)),
           "async: a job reached the global list (enqueued outside a flow) but was never drained");
    JS_SetJobEnqueueHook(NULL);
    JS_SetFlowControlHooks(&FC_OFF);
    JS_SetFlowLocalMark(0);
    /* No flow is running, so no candidate substitution may be installed — the same mirror the switch-in keeps,
       completed at the one point where the answer is "none". Without it the LAST flow to run leaves its
       payload and its endpoint suppression standing over everything that reads the frontier afterwards. */
    solve_flow_begin(NULL);
    g_dom_capture = 0;
    g_sess_live = 0;
    return ENGINE_STEP_DONE;
}

/* A host that has nothing else to do between quanta — the node smoke test — drives the SAME steps in a loop.
   That is a HOST's loop over the one scheduler, not a second scheduler: the state machine is unchanged and a
   quantum boundary is invisible to it. */
/* THE HOST STREAMS WHAT THE RUN IS COSTING, because a run that does not finish reports nothing at all.
   The three cost numbers are published in the result document, which is built when the frontier drains — so a
   run that takes twenty minutes instead of three says exactly nothing about why, which is the state the last
   attempt to measure one ended in. Emitting them as the run goes is the host's own job: between returns from
   the scheduler it pumps messages, interleaves engines, streams findings and snapshots, and this is a finding.
   THE CADENCE IS A COUNT, NOT A CLOCK. A wall-clock cadence would make the output differ run to run for a
   reason that has nothing to do with the engine; a switch count is what the engine actually did, so two runs of
   the same page emit the same lines. It is a reporting interval and not a bound: nothing is dropped, skipped or
   reordered by it, and the loop it sits in is unchanged. */
/* Sized against what a run actually costs rather than guessed: this fixture's whole exploration is under six
   thousand switches, so a cadence in the hundreds of thousands emits nothing and tells nobody anything. */
#define ENGINE_PROGRESS_EVERY 1000

static void run_scheduler(JSContext *ctx, char **bodies, char **srcs, int n, int forking) {
    int next = ENGINE_PROGRESS_EVERY, last_cands = -1;
    engine_sched_begin(ctx, bodies, srcs, n, forking);
    while (engine_sched_step() != ENGINE_STEP_DONE) {
        /* Either enough work has happened to be worth a line, or the SEARCH grew — a new candidate is the event
           that changes what the rest of the run will cost, so it is worth saying when it happens. */
        if (g_switches >= next || solve_candidate_count() != last_cands) {
            while (g_switches >= next) next += ENGINE_PROGRESS_EVERY;
            last_cands = solve_candidate_count();
            /* WHAT IS RUNNING, not just how much has run. A run that stops advancing is the one thing this
               stream exists to make visible, and a line of pure counters cannot name the flow it stopped in —
               it says a stall happened and nothing about where, which leaves bisecting the fixture as the only
               way to localise it. A candidate flow is identified by the (sink, payload) it is verifying, so the
               last line before a stall names the search that entered it. */
            printf("@PROGRESS {\"switches\":%d,\"flows\":%ld,\"candidates\":%d,\"running\":\"%s\"",
                   g_switches, flow_created_count(), last_cands,
                   g_sess_cur && g_sess_cur->cand_sink ? g_sess_cur->cand_sink : "-");
            if (g_sess_cur && g_sess_cur->cand_payload) {
                printf(",\"payload\":\"");
                for (const char *p = g_sess_cur->cand_payload; *p; p++) {
                    if (*p == '"' || *p == '\\') printf("\\%c", *p);
                    else if ((unsigned char)*p < 0x20) printf("\\u%04x", (unsigned char)*p);
                    else putchar(*p);
                }
                printf("\"");
            }
            printf("}\n");
            fflush(stdout);
        }
    }
}

/* EXPLORE: seed boot + drain the frontier, forking at every concolic branch. */
void engine_run(JSContext *ctx, char **bodies, char **srcs, int n) { run_scheduler(ctx, bodies, srcs, n, 1); }

