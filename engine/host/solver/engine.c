/* The dispatch loop — see engine.h. */
#include "core/fetch/fetch.h"
#include "solver/engine.h"
#include "quickjs-step.h"   /* a host answer is TAKEN by a step machine, and a throw ends it JS_STEP_ABRUPT */
#include "core/html/unhandled_rejection.h"
#include "core/idl_args.h"   /* the one point every Web API member passes through — see idl_slowest_step */   /* HTML §8.1.7.5: what the browser half owes this checkpoint */
#include "solver/result.h"
#include "solver/solve.h"
#include "solver/flow.h"
#include "solver/decide.h"
#include "solver/concolic.h"
#include "solver/cow.h"
#include "solver/world.h"   /* the routed record's world vector: whose timeline a delivery belongs to */
#include "core/frame/window_message.h"   /* the receiving half of a routed `windowproxy.post` */
#include "core/frame/remote_op.h"        /* the receiving half of a cross-agent OPERATION: what performs it */
#include "core/frame/remote_object.h"    /* …and the grammar its completion crosses back in */
#include "core/frame/navigable.h"   /* @HEAP's realm count: the one component that holds this agent's realms */
#include "solver/dom_cow.h"   /* the DOM half of time-travel — swapped per-flow alongside the heap COW delta */
#include "solver/cold.h"      /* what the frontier's parked snapshots are made of — the cold tier's census */
#include "solver/quantum.h"   /* the cooperative quantum's asynchronous edge, and what THIS host can measure */
#include "solver/reclaim.h"   /* …and its RAM edge: which allocators can sell a flow rather than fail */
#include "solver/req2proto.h" /* an API's own rejection is a description of the request it wanted — see engine_provide */
#include "solver/discovery.h" /* …and an API's own PUBLISHED description: the flow this engine seeds to fetch one */
#include "check.h"
#include <time.h>
#include <stdlib.h>
#include <malloc.h>   /* @HEAP's allocator numbers — see engine_c_alloc_live */
#include <stdio.h>
#include <string.h>

/* THE DOM IDENTITY MAP'S SIZE, for the diagnostic line below — REGISTERED rather than reached for. Including
   the DOM's header here made the SOLVER depend on lexbor for one statistic, and through that on every host
   that links the scheduler: the streams gate could not build a real AbortSignal because the chain from it
   reached this line. A host with no document registers none and the diagnostic reports zero, which is what a
   run with no wrapped nodes should say. */
static void (*g_wrap_stats)(long *n, long *cap);

void engine_set_wrap_stats(void (*fn)(long *n, long *cap)) { g_wrap_stats = fn; }


/* FETCH-AWAIT parking: a host fetch registers its resolve capability on THE RUNNING FLOW. A flow that awaits it
   suspends its async body; the flow's own pending register is drained when the reply arrives — each awaiting
   async body's reaction enqueues as a job in that flow's queue — and it resumes. Per-flow (not global) so one
   flow's drain never resolves another flow's fetch (which would route the reaction to the wrong flow's COW — a
   leak + contamination).
   THERE IS ONE FORM, and it names a URL. A second form used to take the value UP FRONT, for an edge that
   already had the bytes; nothing in the engine ever did — the Fetch component and the module loader both owe
   theirs to the trusted host — and its only caller was a fixture stub standing in for a host that did not
   answer. With the host answering, that stub and this form both go. */
/* The same park, with the URL the HOST must fetch. The value arrives later through engine_provide; until it
   does the flow cannot finish, which is what keeps the reply-gated code reachable. */
void engine_pending_fetch_url(JSContext *ctx, JSValueConst resolve, JSValueConst value, const FetchRequest *req) {
    Flow *f = flow_running();
    /* THE URL IS WHAT THIS HOST PARKS ON TODAY. The seam now carries the whole request — method, headers and
       body — because a host that actually answers one needs them, and the trusted zone this parks for is
       exactly such a host: `safeFetch` decides SOP/CORS/method/credentials, and it cannot decide about a method
       it was never told. Recording the rest is the next step here; naming it in the seam is what makes that a
       change in one place rather than a signature every provider re-invents. */
    const char *url = req ? req->url : NULL;
    JSValue e;
    /* A live fetch is ALWAYS issued from a running flow — both explore and @S verify are the ONE scheduler now
       (run_scheduler), so flow_running() is set; the flow's stall drains it (flow_step). */
    DCHECK(f != NULL, "engine_pending_fetch_url: a live fetch issued outside a running flow");
    e = pending_push(&f->pending, FLOW_PENDING_RESOLVE);
    pending_set(e, PEND_RESOLVE, JS_DupValue(ctx, resolve));
    pending_set(e, PEND_VALUE, JS_DupValue(ctx, value));
    if (url) pending_set(e, PEND_URL, JS_NewString(ctx, url));
    /* THE REST OF THE REQUEST, recorded rather than dropped between the seam and the trusted zone. */
    if (req && req->method) pending_set(e, PEND_METHOD, JS_NewString(ctx, req->method));
    if (req && req->headers && req->headers->n > 0) {
        JSValue hl = pending_list_new();
        int hi;
        for (hi = 0; hi < req->headers->n; hi++)
            pending_list_add_pair(hl, req->headers->e[hi].name, req->headers->e[hi].value);
        pending_set(e, PEND_HEADERS, hl);
    }
    if (req && req->body) pending_set_bytes(e, PEND_BODY, req->body, req->body_len);
    if (!url) pending_set(e, PEND_HAVE_VALUE, JS_TRUE);
    JS_FreeValue(ctx, e);
}

/* PARK ON A DYNAMIC `import()`. The same register and the same URL, and a DIFFERENT delivery: a module load is
   owed SOURCE TEXT, so the drain settles `resolve` with the reply's body rather than with the reply record a
   `fetch()` becomes a Response from. */
void engine_pending_module_url(JSContext *ctx, JSValueConst resolve, const char *url) {
    Flow *f = flow_running();
    JSValue e;
    DCHECK(f != NULL, "a dynamic import was issued outside a running flow");
    DCHECK(url != NULL && *url, "a dynamic import parked with no module URL for the host to fetch");
    e = pending_push(&f->pending, FLOW_PENDING_MODULE);
    pending_set(e, PEND_RESOLVE, JS_DupValue(ctx, resolve));
    pending_set(e, PEND_URL, JS_NewString(ctx, url));
    pending_set(e, PEND_METHOD, JS_NewString(ctx, "GET"));   /* a chunk load states its own method */
    JS_FreeValue(ctx, e);
}

/* PARK ON AN EXTERNAL DOCUMENT SCRIPT. Registered at most once per flow per slot — the flow is asked again on
   every scheduler pass while it waits, and a second registration would make the host owe the same URL twice. */
void engine_pending_docscript(JSContext *ctx, const char *url, int script_i) {
    Flow *f = flow_running();
    JSValue e;
    DCHECK(f != NULL, "an external document script was awaited outside a running flow");
    DCHECK(url != NULL && *url, "an external document script entry carries no URL");
    for (int i = 0, n = pending_count(f->pending); i < n; i++) {
        JSValue p = pending_entry(f->pending, i);
        int dup = pending_get_int(p, PEND_KIND) == FLOW_PENDING_DOCSCRIPT &&
                  pending_get_int(p, PEND_SCRIPT_I) == script_i;
        JS_FreeValue(ctx, p);
        if (dup) return;
    }
    e = pending_push(&f->pending, FLOW_PENDING_DOCSCRIPT);
    pending_set(e, PEND_URL, JS_NewString(ctx, url));
    pending_set_int(e, PEND_SCRIPT_I, script_i);
    JS_FreeValue(ctx, e);
}

/* PARK ON AN INJECTED SCRIPT. `document.body.appendChild(s)` with `s.src` set is the other way a page loads code
   conditionally, and it has no promise for the reply to settle — the reply IS more program. The flow parks on
   the URL exactly as a fetch does (same register, same dedup, same stall accounting) and the drain queues the
   body as this flow's next script, so the loaded code runs in the world that injected it: its COW delta, its
   pins, its position in the BFS. A sibling that never took that branch never sees the script. */
void engine_pending_script_url(JSContext *ctx, const char *url) {
    Flow *f = flow_running();
    JSValue e;
    DCHECK(f != NULL, "a <script src> was injected outside a running flow");
    DCHECK(url != NULL && *url, "a <script src> was injected with no URL");
    e = pending_push(&f->pending, FLOW_PENDING_SCRIPT);
    pending_set(e, PEND_URL, JS_NewString(ctx, url));
    JS_FreeValue(ctx, e);
}

/* PARK ON A PUBLISHED API DESCRIPTION — the engine's own probe (solver/discovery.h), and the reason a discovery
   flow can exist at all: this engine's only network edge is this register, so a component that wants a document
   fetched needs a FLOW to park. Same register, same dedup, same stall accounting as every other kind.
   THERE IS NO METHOD PARAMETER AND THERE MUST NEVER BE ONE. The verb is stated here, once, as the constant it
   is — §Attacker sources ("a state-mutating request is NEVER fired to learn") and SECURITY.md §Network ("GET
   only") are then properties of this entry point's SHAPE, exactly as removing the method parameter from
   `pageContextGet` made them properties of the JS relay's. A caller cannot express anything else, so there is
   no check here to remember and none to get wrong. */
void engine_pending_discovery_url(JSContext *ctx, const char *url) {
    Flow *f = flow_running();
    JSValue e;
    DCHECK(f != NULL, "a discovery probe was issued outside a running flow");
    DCHECK(url != NULL && *url, "a discovery probe parked with no URL for the host to fetch");
    e = pending_push(&f->pending, FLOW_PENDING_DISCOVERY);
    pending_set(e, PEND_URL, JS_NewString(ctx, url));
    pending_set(e, PEND_METHOD, JS_NewString(ctx, "GET"));
    JS_FreeValue(ctx, e);
}

/* THE SESSION'S SCRIPT SEQUENCE — the document's own scripts in order. Entry i is inline (bodies[i] is its text)
   or external (srcs[i] is its URL and bodies[i] is filled when the host replies). Declared here because the
   pending DRAIN writes into it: an external script's text is the DOCUMENT's, shared by every flow. */
/* A PEER HOLDS A REFERENCE INTO THIS DOCUMENT — see engine.h's engine_set_referenced. It is a property of the
   INSTANCE and not of a session, so it is set once by the host that provisioned this instance and survives
   every session boundary; it is read by flow_step (a timeline that may not finish) and by the slice (a
   frontier that may not be declared exhausted), which are the two halves of one statement. */
static int g_referenced;

/* The browser layer's document-load lifecycle, asked when a flow has run everything the document gave it. */
static int (*g_docdone_hook)(JSContext *ctx);
void engine_set_document_done_hook(int (*fn)(JSContext *ctx)) { g_docdone_hook = fn; }

/* THE EVENT LOOP'S TIMER STEP, registered by the timer component for the reason the document hook is: naming
   it here would make the scheduler depend on the browser half. Asked only where this flow has nothing else to
   run, which is the one moment virtual time may move — see timer.h. */
static int (*g_timer_hook)(JSContext *ctx);
void engine_set_timer_hook(int (*fn)(JSContext *ctx)) { g_timer_hook = fn; }

/* THE EVENT LOOP'S OTHER CLOCK-DRIVEN SOURCE — §8.1.7.3's in-parallel half, which queues an update-the-
   rendering task on the rendering task source when a navigable has a rendering opportunity. Registered by the
   rendering component for the reason the timer step is: naming it here would make the scheduler depend on the
   browser half. It is asked immediately BEFORE the timer step and yields to a timer that expires first, so the
   ONE virtual clock still runs its two sources in the order their moments fall. */
static int (*g_rendering_hook)(JSContext *ctx);
void engine_set_rendering_hook(int (*fn)(JSContext *ctx)) { g_rendering_hook = fn; }

/* THE SESSION. The dispatch loop is not a function that drains — it is a state machine its HOST steps, because
   the cooperative-quantum yield in CLAUDE.md's §scheduler is exactly that: after a bounded wall-clock slice the
   scheduler RETURNS so the one thread pumps its message port, streams findings, interleaves other documents,
   and then resumes the byte-identical frontier. A `for(;;)` that runs to exhaustion cannot do any of it, and in
   the extension it freezes the worker outright. The state that used to be the loop's C locals lives here so a
   return between two iterations costs nothing to resume from.
   ALL OF IT IN ONE PLACE. Two of these sat here and four beside the loop eight hundred lines below, which is
   one piece of state with two homes — and the split is not cosmetic: everything above the second home could
   read half the session and not the other half, so a router that needs to know whether a session is live had to
   be written somewhere it could see g_sess_live rather than where it belongs. */
static JSContext *g_sess_ctx;
static char **g_sess_bodies;
static char **g_sess_srcs;
static int g_sess_n;
static Flow *g_sess_cur;
static int g_sess_live;

/* THE URLS THE HOST OWES, newline-joined across every live flow, or "" — one register, the flows' own. The
   buffer is this function's and is valid until the next call. */
/* THE URLS THE HOST OWES, newline-joined across every live flow, DEDUPED. Several flows park on the same URL —
   a candidate re-fire re-runs the same fetches the exploring flow made — and engine_provide fills every entry
   that names it, so listing it twice makes the host provide twice and the second call finds nothing left. The
   list is a set of requests, not a list of waiters. */
/* A SYNCHRONOUS REQUEST ONLY THE HOST CAN ANSWER, and the mechanism a cross-document read rides.
 *
 * WHY IT IS NOT A FETCH. A fetch hands the page a promise and the flow keeps running; this one BLOCKS, because
 * the value has to appear at the call site that asked for it. `iframe.contentWindow.document.body` must answer
 * on the line after the append, and one instance is one document, so the answer is in another instance and not
 * available in this turn. The flow therefore SUSPENDS exactly as it suspends at an await or a loop back-edge,
 * siblings run, and it resumes with the value.
 *
 * WHY THE RENDEZVOUS IS AN ID AND NOT THE REQUEST TEXT. A fetched body is shared by every flow waiting on that
 * URL, which is right for a resource. An answer here is computed under the ASKING FLOW'S WORLD — two arms of a
 * fork that navigated a frame differently must not resolve to one Window — so answers are never shared, and
 * two identical questions are two requests. */
static uint32_t g_next_req = 1;

/* HOW MANY SYNCHRONOUS REQUESTS THIS DOCUMENT HAS EVER ISSUED, AND HOW MANY THE HOST HAS EVER ANSWERED — the
 * next two facts in `finished`/`deepest`'s family, and they exist for the same reason those two do: the
 * question they answer had to be ARGUED from a census instead of read off one.
 *
 * `blocked` and `owed` are LEVELS. They say how much of the frontier is waiting right now, and no single
 * reading of a level can tell a host that is paying promptly from a host that has never paid at all: a
 * document whose flows are answered within a quantum and one whose flows are parked forever both report a
 * frontier with thousands of members waiting, because in both of them the members keep arriving. Starvation is
 * a RATE, and these two are the only numbers this engine holds that have one. `hostAnswered: 0` beside
 * `blocked: 6583` ends that diagnosis in a glance — those flows are not mis-ranked, they are unpaid — and a gap
 * between the two that only ever widens is a host that is not paying at the rate the document asks.
 *
 * They are CUMULATIVE and they are read by NOTHING but the census (§NO BOUNDS): no pick, no weight and no exit
 * consults them, so there is no spelling of them that can truncate work. */
static long g_host_asked;
static long g_host_answered;

uint32_t engine_host_request(JSContext *ctx, const char *op) {
    Flow *f = flow_running();
    JSValue e;
    uint32_t id;

    DCHECK(f != NULL, "a synchronous host request was issued outside a flow — there would be nothing to "
                      "suspend and nothing to resume with the answer");
    DCHECK(op != NULL && *op, "a synchronous host request carried no text for the host to route on");
    e = pending_push(&f->pending, FLOW_PENDING_HOSTREQ);
    pending_set(e, PEND_OP, JS_NewString(ctx, op));
    id = g_next_req++;
    g_host_asked++;   /* the ASK half of the rate above — every issue of a request passes through here */
    /* A REUSED ID ANSWERS THE WRONG QUESTION. The answer is routed by this number alone, so a wrapped counter
       delivers one flow's cross-document read into another flow's call site — in another world. */
    CHECK(g_next_req != 0, "the host-request id counter wrapped — an answer would be delivered to the wrong "
                           "call site, in a world that never asked the question");
    pending_set_int(e, PEND_REQ, id);
    JS_FreeValue(ctx, e);
    /* THE REQUEST IS WHAT MAKES THE FLOW BLOCKED, AND THE YIELD THAT FOLLOWS IS ONLY A PARK BECAUSE OF IT.
       Every caller of this returns JS_STEP_YIELD, whose contract (quickjs-step.h) is "I have more work;
       preempt me if you want" — the driver re-enters a machine that yields IMMEDIATELY unless the preempt hook
       says otherwise, so a machine using the yield to WAIT is a busy spin unless preempt_hook's clause (0)
       sees this entry on the register. Nothing else ties the two together: they are a step code in the engine
       and a pending kind in the scheduler, and a pending kind added without pending_blocked's agreement would
       turn every cross-instance read into a flow that burns the thread until the host answers — with the host
       only asked BETWEEN steps, so the answer can never arrive. Asserted here because this is where the
       blocking is claimed. */
    DCHECK(flow_blocked(f), "a synchronous host request left its flow RUNNABLE — the caller is about to return "
                            "JS_STEP_YIELD, which re-enters immediately, so the flow would spin on a question "
                            "that is only answered between steps");
    return id;
}

/* HAS THE HOST ANSWERED `req`? The asking machine's re-entry read. The value is BORROWED — it stays on the
   register until the machine takes it, so a re-entry that yields again can read it once more. */
int engine_host_answered(uint32_t req, JSValueConst *out) {
    Flow *f = flow_running();
    DCHECK(req != 0, "a host request with no id was asked about");
    DCHECK(f != NULL, "a host request was asked about outside a flow");
    for (int i = 0, n = pending_count(f->pending); i < n; i++) {
        JSValue e = pending_entry(f->pending, i);
        if ((uint32_t)pending_get_int(e, PEND_REQ) == req) {
            int have = pending_get_int(e, PEND_HAVE_VALUE) != 0;
            /* BORROWED, and the register is what holds it: the reference taken here is released before the
               return, exactly as the field read did when the record was C — the entry still names the value,
               and a machine re-entered before it consumes may read it again. */
            if (have && out) { JSValue v = pending_get(e, PEND_VALUE); *out = v; JS_FreeValue(pending_ctx(), v); }
            JS_FreeValue(pending_ctx(), e);
            return have;
        }
        JS_FreeValue(pending_ctx(), e);
    }
    /* NOT ON THIS FLOW'S REGISTER AT ALL. A machine asking about an id it never issued, or issued before a
       fork that re-issued it under the sibling's own world — either way, answering "no" would park the flow
       forever on a question nothing is going to answer. */
    DFAIL("a machine asked about a host request that is not on its flow's register — it either never issued "
          "the request or it inherited an id across a fork, which re-issues under the sibling's own world");
    return 0;
}

/* THE MACHINE TAKES ITS ANSWER and the request leaves the register. Separate from the read above because a
   machine may be re-entered several times before it is ready to consume, and taking it early would leave the
   next re-entry with nothing. */
JSValue engine_host_take(JSContext *ctx, uint32_t req, int *pcompletion) {
    Flow *f = flow_running();
    JSValue v;
    DCHECK(f != NULL, "a host answer was taken outside a flow");
    DCHECK(pcompletion != NULL,
           "a host answer was taken without reading the COMPLETION TYPE it carries — an answer is a completion "
           "record and a taker that reads only its value delivers a peer's THROW as `undefined` to the call "
           "site that parked on it");
    for (int i = 0, n = pending_count(f->pending); i < n; i++) {
        JSValue e = pending_entry(f->pending, i);
        if ((uint32_t)pending_get_int(e, PEND_REQ) == req) {
            JSValue c;
            DCHECK(pending_get_int(e, PEND_HAVE_VALUE), "a host answer was taken before it arrived");
            /* THE TYPE AND THE VALUE ARE ONE WRITE (engine_host_answer), so an answered request with no type
               is a delivery that went round this file — and reading it as a normal completion is exactly the
               silent lie this field exists to make impossible. */
            c = pending_get(e, PEND_COMPLETION);
            DCHECK(JS_IsNumber(c), "an answered host request carries no completion type");
            JS_FreeValue(ctx, c);
            *pcompletion = (int)pending_get_int(e, PEND_COMPLETION);
            v = pending_get(e, PEND_VALUE);   /* the caller's reference; the register's goes with the entry */
            JS_FreeValue(ctx, e);
            pending_remove(&f->pending, i);
            return v;
        }
        JS_FreeValue(ctx, e);
    }
    DFAIL("a host answer was taken for a request that is not on this flow's register");
    return JS_UNDEFINED;
}

int engine_host_take_completion(JSContext *ctx, uint32_t req, JSValue *presult) {
    int completion = ENGINE_COMPLETION_NORMAL;
    JSValue v = engine_host_take(ctx, req, &completion);

    DCHECK(presult != NULL, "a host completion was taken with nowhere to place its value");
    if (completion == ENGINE_COMPLETION_THROW) {
        /* RE-RAISED AT THE CALL SITE THAT PARKED ON IT. The flow suspended inside the operation, so this is
           where the peer's throw belongs — the page's own `try`/`catch` around the read, the write or the call
           is the handler that runs, exactly as it would for a local one. JS_Throw consumes the value. */
        *presult = JS_UNDEFINED;
        JS_Throw(ctx, v);
        return JS_STEP_ABRUPT;
    }
    DCHECK(completion == ENGINE_COMPLETION_NORMAL,
           "a host answer arrived under a completion type ECMA-262 6.2.4 does not have — a cross-instance "
           "operation completes normally or throws, and `return`/`break`/`continue` do not cross a call site");
    *presult = v;
    return JS_STEP_DONE;
}

/* Deliver the host's answer. Routed by id to ONE flow's ONE call site — never broadcast the way a fetched
   body is, because the answer was computed under that flow's world. */
int engine_host_answer(JSContext *ctx, uint32_t req, JSValueConst value, int completion) {
    DCHECK(req != 0, "the host answered a request with no id");
    DCHECK(completion == ENGINE_COMPLETION_NORMAL || completion == ENGINE_COMPLETION_THROW,
           "the host answered a request with a completion type that is neither normal nor a throw");
    for (int k = 0; ; k++) { Flow *f = flow_at(k); if (!f) break;
        for (int i = 0, n = pending_count(f->pending); i < n; i++) {
            JSValue p = pending_entry(f->pending, i);
            if (pending_get_int(p, PEND_KIND) != FLOW_PENDING_HOSTREQ ||
                (uint32_t)pending_get_int(p, PEND_REQ) != req) { JS_FreeValue(ctx, p); continue; }
            /* A SECOND ANSWER TO ONE QUESTION IS NOT A DUPLICATE — it is a SECOND PEER TIMELINE, and this is
               where the asking side's half of that is missing. A peer's document state IS its flows, so a
               cross-agent operation is performed by every live timeline it has and each one completes with its
               own answer; `otherW.length` has N answers for N peer timelines and they are all true. It is not
               hypothetical and it is not the peer host's oversight: engine_fork_finalize COPIES `answer_token`
               onto a sibling on purpose, because a branch inside the answering program is a real peer timeline
               in which the answer differs — so N answers under one token is what that fork was built to
               produce. Taking the first and asserting on the rest picks a timeline; overwriting picks the last,
               after the machine may already have read the first.
               WHAT BELONGS IS A FORK OF THE ASKING FLOW AT ITS PARK — a sibling per additional answer, resuming
               the same suspended frame with that answer's value — and THE ONE THING THAT MUST NOT BE DONE IS TO
               TAKE IT HERE. This entry runs BETWEEN scheduler steps: `flow_running()` still names whichever flow
               was last switched in, `cow_delta_fork` freezes the delta CURRENTLY APPLIED and `dom_cow_fork`
               forks the LIVE DOM head, so a fork on this line would clone a stranger's timeline and call it the
               asker's — the exact "two timelines wearing one name" the world registry exists to prevent, with
               nothing to say so. So the three named things to build are:
                 (1) RECORD the extra completion ON THE ENTRY, as a JS value (an Array slot beside PEND_VALUE) —
                     never a malloc'd C list, because a queued platform value must park to the cold tier and fork
                     per flow, which the COW delta gives a property write for free and gives a pointer never;
                 (2) TAKE THE FORK IN flow_step, with that flow switched in and before it resumes its frame,
                     which is the only moment the delta, the DOM head and the running flow all agree — one
                     sibling per recorded answer, each carrying that answer alone on its own forked register;
                 (3) SPLIT engine_fork_finalize: its body assembles a sibling from a BRANCH's decision and pin
                     blobs and DCHECKs that one is prepared, and a parked fork has no decision — the sibling
                     assembly is what the two share, the decision is what only the branch has.
               (A host-COMPUTED answer — a fetch this zone performed — genuinely has one answer, and answering
               one of those twice is the host bug this assert used to be about. Both readings end here.) */
            DCHECK(!pending_get_int(p, PEND_HAVE_VALUE),
                   "one request was answered twice. If the answers came from a PEER, that is two of its "
                   "timelines answering and both are true: record the extra completion on this entry as a JS "
                   "value and fork the asking flow in flow_step, with that flow switched in — never on this "
                   "line, which runs between steps where flow_running(), the applied delta and the live DOM "
                   "head all still belong to a different flow");
            pending_set(p, PEND_VALUE, JS_DupValue(ctx, value));
            pending_set(p, PEND_COMPLETION, JS_NewInt32(ctx, completion));
            pending_set(p, PEND_HAVE_VALUE, JS_TRUE);
            JS_FreeValue(ctx, p);
            /* AND THE FLOW IS ASKABLE AGAIN. This is the event a flow parked on a synchronous request is
               waiting for — the one thing that can change the answer it gave the scheduler — so the mark comes
               off HERE, on the flow the answer reached, and not on a slice boundary that means nothing to it
               (flow.h). Without this line the answer would sit on the register while the pick kept skipping the
               flow that asked for it. */
            flow_clear_host_owed(f);
            g_host_answered++;   /* the PAY half of the rate above — every delivery that reached a flow */
            return 1;
        }
    }
    return 0;   /* the asking flow is gone: an answer to a question nobody is waiting on is simply dropped */
}

/* WHAT THE HOST STILL OWES, as `id\top\n` records. Pulled each step like the pending URLs, and NOT deduped:
   two identical questions from two flows are two questions, because each is answered under its own world. */
const char *engine_host_requests(void) {
    static char *join;
    static size_t cap;
    size_t n_out = 0;
    Flow *f;

    /* ONE PASS, NOT TWO. It measured and then wrote, which meant converting every record's text TWICE — and a
       JS string's text is a conversion that allocates, where the C list's was a `strlen`. Two dom/ranges tests
       crossed the gate's CPU budget on that alone, and the measuring pass existed only so `malloc` could be
       given a size. A buffer that grows is the same answer with one conversion, and it also retires the DCHECK
       that the two loops agreed: there is one loop. */
    if (!join) { cap = 256; join = malloc(cap); CHECK(join, "engine: OOM joining the outstanding host requests"); }
    join[0] = 0;
    for (int k = 0; (f = flow_at(k)) != NULL; k++)
        for (int i = 0, n = pending_count(f->pending); i < n; i++) {
            JSValue p = pending_entry(f->pending, i);
            JSValue o;
            const char *s;
            size_t ol = 0;
            char idbuf[24];
            int idlen;
            if (pending_get_int(p, PEND_KIND) != FLOW_PENDING_HOSTREQ ||
                pending_get_int(p, PEND_HAVE_VALUE)) { JS_FreeValue(pending_ctx(), p); continue; }
            idlen = snprintf(idbuf, sizeof idbuf, "%u\t", (uint32_t)pending_get_int(p, PEND_REQ));
            o = pending_get(p, PEND_OP);
            s = JS_ToCStringLen(pending_ctx(), &ol, o);
            DCHECK(s != NULL, "an outstanding host request has no text — the host routes on it");
            while (n_out + (size_t)idlen + ol + 2 > cap) {
                cap *= 2;
                join = realloc(join, cap);
                CHECK(join, "engine: OOM growing the outstanding-request join");
            }
            memcpy(join + n_out, idbuf, (size_t)idlen); n_out += (size_t)idlen;
            memcpy(join + n_out, s, ol); n_out += ol;
            join[n_out++] = '\n';
            join[n_out] = 0;
            JS_FreeCString(pending_ctx(), s);
            JS_FreeValue(pending_ctx(), o);
            JS_FreeValue(pending_ctx(), p);
        }
    return join;
}

/* THE ONE-WAY LINE. A notice is not on a flow's pending register because nothing is waiting on it and because a
   flow that finishes must not take it with it: the document it announces exists whatever becomes of the flow
   that created it. One buffer, appended to and drained whole — a notice the host has read is gone. */
static char  *g_notices;
static size_t g_notices_n, g_notices_cap;

void engine_host_notify(JSContext *ctx, const char *op) {
    size_t len;
    (void)ctx;
    DCHECK(op != NULL && *op, "a host notice carried no text for the host to route on");
    DCHECK(strchr(op, '\n') == NULL, "a host notice contains a newline, which is the record separator — the "
                                     "host would read it as two notices and route the tail as an operation");
    len = strlen(op);
    if (g_notices_n + len + 2 > g_notices_cap) {
        size_t cap = g_notices_cap ? g_notices_cap * 2 : 256;
        char *g;
        while (cap < g_notices_n + len + 2) cap *= 2;
        g = realloc(g_notices, cap);
        CHECK(g != NULL, "engine: OOM recording a host notice — a dropped notice is a document the host never "
                         "provisions, so every read through it parks its flow forever");
        g_notices = g;
        g_notices_cap = cap;
    }
    memcpy(g_notices + g_notices_n, op, len);
    g_notices_n += len;
    g_notices[g_notices_n++] = '\n';
    g_notices[g_notices_n] = 0;
}

/* THE INBOUND HALF OF THE ONE-WAY LINE — a record the trusted zone routed to THIS instance because it holds the
   document the record names. It is the exact text the sending instance emitted as a notice, plus the SENDER's
   ORIGIN, which only that zone may stamp (SECURITY.md: an origin the untrusted engine computed for a foreign
   message is a forgery every `event.origin` check in every bundle would then trust).
 *
 * IT BECOMES A FLOW, and that is the whole design rather than a detail of it. §CLAUDE's cross-document rule says
 * a delivery SEEDS a flow in the receiver whose world is receiver-baseline ∧ the sending flow's vector, and that
 * it is a work item on the ONE frontier. RECEIVER-BASELINE IS READ AS THE RECEIVING DOCUMENT'S OWN TIMELINES,
 * not as this instance's pre-boot baseline: a flow starting from the latter arrives where the page's `message`
 * listener was never registered, because the script that registered it ran in some flow's delta. The two
 * readings differ by exactly that, and only one of them delivers a message at all — so this creates a member
 * of every live timeline and returns. It does not deliver: delivering
 * here would run page code under whatever flow the scheduler last had switched in, against that flow's delta,
 * which is the "two timelines wearing one name" the world registry exists to prevent. The frontier IS the
 * inbound queue, so there is no second queue to drain and no second pump to drain it, and the delivery is
 * parkable, rankable and cross-session resumable for free because every flow is.
 *
 * THE SEGMENT IS THE REGISTRY'S, AND ASKING FOR IT IS HALF THE DELIVERY'S WORLD. A world minted elsewhere has
 * one segment in this instance — whatever that world has written HERE — materialized on first arrival by
 * forking the nearest ancestor this instance already holds, which is the only thing the ancestry the record
 * carries is for. Asking for it here is what makes a later arrival from a world FORKED off this one inherit
 * these writes instead of starting from this instance's baseline; the registry owns it for the life of the
 * world, so no flow frees it. */
void engine_route(JSContext *ctx, const char *record, const char *sender_origin)
{
    char *dup, *doc, *worlds, *tail;
    int n;

    DCHECK(record != NULL && *record, "a routed record carried no text to route on");
    DCHECK(sender_origin != NULL && *sender_origin,
           "a routed record arrived without the sender's origin — only the trusted zone can stamp one, and a "
           "delivery without it would have to invent the one field a page's check is written against");
    DCHECK(g_sess_live, "a record was routed into an instance with no live session — the flow it would be "
                        "delivered on has no scheduler to run it, so the delivery would be dropped");

    /* EVERY ROUTED RECORD'S FIRST TWO FIELDS ARE THE TRANSPORT'S: the target DOCUMENT (which instance) and the
       sending flow's WORLD (whose timeline). Everything after them belongs to the component named by the op,
       which is the only thing that reads it. Stating that here is what lets a second op be routed without a
       second router. */
    dup = strdup(record);
    CHECK(dup != NULL, "engine: OOM routing a record");
    doc = strchr(dup, '\t');
    DCHECK(doc != NULL, "a routed record named no target document — the trusted zone routes on that field, so a "
                        "record without one could not have arrived at this instance on purpose");
    *doc++ = 0;
    worlds = strchr(doc, '\t');
    DCHECK(worlds != NULL, "a routed record carried no world vector — the delivery would belong to no timeline");
    *worlds++ = 0;
    tail = strchr(worlds, '\t');
    DCHECK(tail != NULL, "a routed record carried nothing after its transport fields — there is no delivery in it");
    *tail = 0;   /* only to bound the vector for the checks here; flow_deliver re-splits its own copy */

    /* ROUTED TO THE WRONG INSTANCE is the trusted zone's bug, not the page's, and it is silent in every other
       form: delivering anyway would fire the message at THIS document's listeners, which is a message the page
       never received arriving as if it had. */
    /* WHICH of this agent's documents it names is NOT decided here, and the router does not need to know: an
       instance is an ORIGIN-KEYED AGENT CLUSTER, so several documents are this one's, and the REALM that
       delivers is selected where the delivery RUNS (flow_deliver) for the same reason the world segment is
       asked for again there — a hosted navigable's realm is materialized by whichever flow first reads through
       it (navigable.h), so which realm answers is a property of the run and not of the record's arrival. */
    DCHECK(world_doc_hosted(world_doc_intern(doc)),
           "a record was routed to an instance that does not hold the document it names — the offscreen is the "
           "only zone that knows which instance holds which document, and it sent this one to the wrong place");
    {
        WorldId w, anc[16];
        int n_anc = world_parse(worlds, &w, anc, (int)(sizeof anc / sizeof anc[0]));
        /* AGAINST EVERY DOCUMENT THIS AGENT HOLDS, not against its root: an agent is an origin-keyed CLUSTER,
           so a message from a SAME-ORIGIN child is delivered in this heap and never reaches a transport. */
        DCHECK(!world_doc_hosted(w.doc), "a record was routed back to the instance whose flow sent it — a "
                                         "message to one's own document is delivered locally and never leaves");
        /* RECEIVER-FLOW-WORLD ∧ SENDER-VECTOR, AS TWO REAL OBJECTS. The receiving half is the delta of the flow
           that will make the delivery — the page's `message` listener was registered by a script, so it lives
           in THAT flow's delta and in no baseline. The sending half is THIS INSTANCE'S SEGMENT for the sender's
           world: what that world has already written here, materialized by forking the nearest ancestor of it
           this instance holds. Both are chains rooted at this instance's baseline, and neither is an ancestor
           of the other — so their conjunction is a JOIN, and a CowDelta is a LINEAR chain (frozen segments
           under one head) that cannot express one.
           IT IS THE IDENTITY EXACTLY WHEN THE SEGMENT IS EMPTY, which is the truth for a world that has only
           ever POSTED: a world that has written nothing here constrains nothing here, so the conjunction is the
           receiving flow's timeline unchanged and the delivery below is already it. That is asked rather than
           assumed, because the answer changes the moment this instance answers a cross-document operation that
           runs page code under a foreign world.
           MATERIALIZED ON ITS OWN LINE, because it is not part of the question. A DCHECK vanishes in release,
           so a segment created inside one would exist in dev and not in production — and the next arrival from
           a world forked off this one would find no ancestor and start from the baseline instead of inheriting
           it. That is two different timelines in two builds, which is precisely why check.h requires the
           condition to be side-effect-free. */
        CowDelta *seg = world_segment(ctx, w, anc, n_anc);
        (void)seg;
        DCHECK(cow_delta_empty(seg),
               "the sending world has WRITTEN in this instance, so the delivery's world is the receiving flow's "
               "timeline conjoined with those writes — a JOIN of two deltas both rooted at this baseline, "
               "neither an ancestor of the other, which the linear delta chain cannot express: stacking the "
               "segment over the flow would overwrite the flow's own value in any slot both touched, and the "
               "unapply restores the BASELINE rather than the flow's value, destroying what the flow recorded "
               "with nothing to say so. BUILD THE JOIN — a merge of the two entry lists where they touch "
               "disjoint slots, and a CONTRADICTION where they touch the same one (two timelines that disagree "
               "about a slot conjoin to no timeline, so there is no delivery to make in that world)");
    }
    free(dup);

    /* THE MESSAGE ARRIVES IN EVERY TIMELINE OF THE RECEIVING DOCUMENT, and that is what makes it arrive at all.
       A delivery seeded as a FRESH flow from this instance's baseline was the first shape of this, and it is
       wrong in a way that is completely silent: the receiving page registers its `message` listener from a
       script, so the registration lives in the COW delta of the flow that ran that script, and a flow starting
       from the baseline sees a document where it was never registered. The message is delivered, no handler
       exists, and nothing distinguishes that from a page that registered none. A document's state IS its flows;
       there is no other timeline to deliver into.
       Attached rather than delivered here: this runs between scheduler steps, so the flow the record belongs to
       is not the one switched in. Each flow makes its own delivery when it next steps, under its own delta,
       and the task that produces lands on its own queue — which is the whole reason the job queue is per-flow. */
    n = flow_count();
    DCHECK(n > 0, "a message was routed to a document whose every timeline had already finished — a document "
                  "that can still receive is not done, so the receiving flows must stay live while a peer holds "
                  "a WindowProxy for them: build that before this can be delivered");
    for (int i = 0; i < n; i++) {
        Flow *f = flow_at(i);
        /* ONE PENDING RECORD PER TIMELINE. Two records on one flow are delivered in sequence into a single
           timeline, and whether that is right depends ENTIRELY on the senders' worlds — which is the part the
           instruction here used to get wrong, so it is stated properly before anyone builds against it.
           TWO MESSAGES FROM ONE WORLD ARE SEQUENTIAL, not alternative: §9.4.4 queues each as a task on the
           receiving document's queue and the page observes them in order, so "fork a sibling per arrival" is
           the WRONG mechanism for them — it would turn one sender's two messages into two timelines that each
           saw one. Only senders whose worlds CONTRADICT (two arms that took opposite branches) may not be
           merged, and those must fork.
           AND THE WORLD GRAPH CANNOT YET TELL THE TWO APART, which is the real unbuilt thing and is why this
           still crashes instead of choosing. A fork mints a CHILD world for the sibling and leaves the PRIMARY
           holding the parent's, so the two arms of one branch are related as ancestor-and-descendant — exactly
           the relation that otherwise means "compatible, one is a continuation of the other". Both arms must
           get a child world (or the branch must be recorded on the edge) before "do these two senders
           contradict?" has an answer at all. Build THAT first; the queue-or-fork decision is one line once it
           can be asked. Driven by engine/route.mjs, whose three posts are exactly this: two from one world and
           one from the other arm. */
        DCHECK(f->deliver == NULL,
               "a second record was routed to a flow that has not yet made its first. It is NOT automatically a "
               "merge to prevent: two messages from ONE sending world are sequential tasks the page must see in "
               "order, and forking a sibling per arrival would be wrong for them. It is only senders whose "
               "worlds CONTRADICT that may not share a timeline — and the world graph cannot answer that yet, "
               "because a fork leaves the primary arm holding the parent world, so two contradictory arms look "
               "like ancestor and descendant. Give both arms a child world, then queue compatible arrivals and "
               "fork contradictory ones");
        f->deliver = strdup(record);
        f->deliver_origin = strdup(sender_origin);
        CHECK(f->deliver && f->deliver_origin, "engine: OOM attaching a routed record to a flow");
        /* AND IT IS ASKABLE AGAIN. A flow that reported host-owed is out of the pick until the host does
           something for it, and this IS that something: the record is work only the trusted zone could hand it
           (flow.h). A delivery attached to a flow the scheduler will not pick is a message the document never
           receives, which is indistinguishable from a page with no listener. */
        flow_clear_host_owed(f);
    }
}

/* WHICH REALM OF THIS AGENT ANSWERS FOR A DOCUMENT NAMED BY A PEER — the one lookup both halves of the
 * cross-instance seam need, and the reason neither of them is restricted to this instance's ROOT document any
 * more. An instance is an ORIGIN-KEYED AGENT CLUSTER (SECURITY.md), so several documents are this one's and a
 * peer holds references into them as a matter of course: `event.source` names the document whose script
 * posted, which is a child navigable as often as it is the root. Performing the work in the root's realm
 * instead would answer about the wrong document — `length` would be the count of the ROOT's child navigables
 * handed back as the child's, the same silent lie the routing check crashes on when the whole INSTANCE is
 * wrong. HTML §7.3 states this direction as "the navigable whose active document is node's node document": a
 * document identifies exactly one, which is what makes this a lookup and not a search.
 *
 * ASKED WHERE THE WORK RUNS, never where the record arrived — the same sentence the world segment beside it
 * carries. A hosted navigable's realm is materialized by whichever flow first reads through it (navigable.h),
 * so a realm that does not exist when the trusted zone routes the record may exist by the time the receiving
 * flow is scheduled, and the answer is a property of the run. */
static JSContext *doc_realm(uint32_t doc)
{
    JSContext *realm = world_doc_realm(doc);

    DCHECK(realm != NULL,
           "a peer reached through a navigable this agent holds whose active document has never been "
           "MATERIALIZED — §7.4 created it with the initial about:blank Document, and only a read through that "
           "navigable's own WindowProxy builds the realm, which needs the NAVIGABLE and not just the "
           "document's name. Build the (document -> navigable) direction HTML §7.3 calls the node navigable — "
           "the navigable whose active document this is — and materialize it here through window_proxy_realm, "
           "exactly as a local read does");
    return realm;
}

/* THE DELIVERY ITSELF, made by the receiving flow's own step — so it runs with that flow switched in, under its
   delta, and the task it enqueues lands on that flow's own queue like every other job. This is the dispatch on
   the op, and the ONLY place a routed op is turned into a call: an op with no component here is a transport
   carrying something nothing receives. */
static void flow_deliver(JSContext *ctx, Flow *f)
{
    char *dup = f->deliver, *doc, *worlds, *tail;
    WorldId w, anc[16];
    int n_anc;
    CowDelta *seg;
    JSContext *rctx;

    DCHECK(flow_running() == f, "a routed delivery was made while another flow was switched in — it would run "
                                "against that flow's delta and its task would land on that flow's queue");
    doc = strchr(dup, '\t');    *doc++ = 0;
    worlds = strchr(doc, '\t'); *worlds++ = 0;
    tail = strchr(worlds, '\t'); *tail++ = 0;
    /* THE RECEIVING DOCUMENT'S REALM, which is what "delivered to that document" means: the event is
       constructed in it, `event.source` is minted in it, and the task is enqueued at ITS Window. Delivering
       into this instance's root instead would fire the message at a document the sender never named — a
       message the page never received arriving as if it had, which is the same failure the routing check
       crashes on one level up. */
    rctx = doc_realm(world_doc_intern(doc));
    /* THE SENDING DOCUMENT IS THE HEAD OF THE WORLD VECTOR — a world is minted by a flow of exactly one
       document, so the vector already names the sender and a second field for it could disagree with it. */
    n_anc = world_parse(worlds, &w, anc, (int)(sizeof anc / sizeof anc[0]));
    /* THE OTHER HALF OF THIS DELIVERY'S WORLD, asked where it is CONSUMED. engine_route asked the same question
       when the record arrived; this asks it at the moment the delivery actually runs, because the scheduler has
       run other flows in between and the answer is a property of the run, not of the record. What is installed
       right now is f's timeline and nothing else, so the sender's segment being empty is what makes that the
       conjunction rather than half of it. Looked up on its own line for the reason engine_route's is: a
       DCHECK's condition is compiled out in release, and a segment materialized inside one would exist in dev
       and not in production. */
    seg = world_segment(ctx, w, anc, n_anc);
    (void)seg;
    DCHECK(cow_delta_empty(seg),
           "a delivery ran in the receiving flow's timeline alone while the sending world holds writes in this "
           "instance — the message arrives at a document missing everything its sender did here. Build the join "
           "of the two deltas that engine_route names");
    if (!strcmp(dup, "windowproxy.post"))
        window_message_route(rctx, tail, world_doc_name(w.doc), f->deliver_origin);
    else
        DFAIL("a record was routed with an op no component receives — the sending half emits it, so the "
              "receiving half is the unbuilt one; build it rather than dropping the delivery");
    free(f->deliver); f->deliver = NULL;
    free(f->deliver_origin); f->deliver_origin = NULL;
}

/* ---- ASKED TO PERFORM ONE, WHICH IS THE HALF WITH AN ANSWER ---------------------------------------------- */

/* engine_route carries a one-way delivery. THIS one is asked, and everything about its shape follows from that.
 *
 * IT IS NOT A REQUEST/RESPONSE PAIR, because a document's state IS its flows. `otherW.length` is the child-
 * navigable count of the peer's ACTIVE DOCUMENT, and the peer holds N timelines in which that document is N
 * different documents — so the honest answer is N answers, and a channel with one slot for "the" answer picks
 * one of them with nothing to say which. The record is therefore attached to EVERY live flow, exactly as a
 * delivery is and for the same reason, and each of them answers under its own delta. The asking flow's side of
 * that — forking per answer rather than taking the first — is named where the second answer arrives
 * (engine_host_answer), because that is where the mechanism is missing.
 *
 * THE ANSWER IS A PROGRAM'S COMPLETION, so it does not exist until a program has finished: the operation is
 * QUEUED as that flow's next program (flow_perform) and the completion is read where the scheduler reads one
 * (flow_step). It is a flow on the one frontier — preemptible, parkable at any depth, ranked with everything
 * else — which is the whole reason a peer answering a read that has to suspend is possible at all. A serve loop
 * that ran the program to completion would be a second scheduler and a drive-to-completion, twice over.
 *
 * `token` IS THE TRUSTED ZONE'S and is opaque here: this instance echoes it on the answer so the zone can route
 * the completion back to the instance and the request id that asked. It is not the asking flow's request id
 * because that id is unique only within the asking instance, and two peers may ask this one the same number. */
void engine_perform(JSContext *ctx, const char *token, const char *record)
{
    RemoteOp *op;
    int n;

    DCHECK(record != NULL && *record, "a cross-agent operation arrived with no text to perform");
    DCHECK(token != NULL && *token, "a cross-agent operation arrived with no rendezvous token — the completion "
                                    "would have nothing to name and the asking flow would park forever");
    DCHECK(strchr(token, '\t') == NULL && strchr(token, '\n') == NULL,
           "a rendezvous token carries a record separator — the answer notice would be read as two notices and "
           "the tail routed as an operation");
    DCHECK(g_sess_live, "a cross-agent operation was asked of an instance with no live session — there is no "
                        "scheduler to run the program that answers it");

    op = remote_op_parse(record);
    /* ROUTED TO THE WRONG INSTANCE is the trusted zone's bug, and answering anyway is worse than not answering:
       `length` would be the count of THIS document's frames returned as the other document's. */
    /* WHICH of this agent's documents it names is the OPERATION's business and not the router's, exactly as it
       is for a routed delivery: the realm that performs it is selected where the program is queued
       (flow_perform), with the answering flow switched in and after every other flow has had its turn to
       materialize a navigable this one may reach through. */
    DCHECK(world_doc_hosted(world_doc_intern(remote_op_doc(op))),
           "a cross-agent operation was routed to an instance that does not hold the document it names — the "
           "offscreen is the only zone that knows which instance holds which document, and it sent this one to "
           "the wrong place");
    {
        WorldId w, anc[16];
        int n_anc = world_parse(remote_op_worlds(op), &w, anc, (int)(sizeof anc / sizeof anc[0]));
        /* THE ASKING WORLD BELONGS TO A PEER — asserted against every document THIS AGENT HOLDS rather than
           against its root, because an agent is an origin-keyed CLUSTER: an operation on an object of a
           SAME-ORIGIN child is performed in this heap at its call site and never reaches a transport. */
        DCHECK(!world_doc_hosted(w.doc), "a cross-agent operation came back to the instance whose flow asked "
                                         "it — an operation on this agent's own object is performed locally "
                                         "and never leaves");
        /* THE ASKING WORLD'S SEGMENT IN THIS INSTANCE, materialized here for the reason engine_route does it
           here: on its own line, because a DCHECK's condition is compiled out in release and a segment created
           inside one would exist in dev and not in production. The conjunction of it with the answering flow's
           own timeline is the JOIN engine_route names, and it is the identity exactly while the segment is
           empty. */
        CowDelta *seg = world_segment(ctx, w, anc, n_anc);
        (void)seg;
        DCHECK(cow_delta_empty(seg),
               "the asking world has WRITTEN in this instance, so the operation must be performed in the "
               "conjunction of the answering flow's timeline with those writes — the same JOIN engine_route "
               "names, and the same reason: stacking one over the other overwrites whichever slot both touched "
               "and unapplies to the baseline rather than to the flow's value");
    }
    remote_op_free(op);

    n = flow_count();
    DCHECK(n > 0, "a cross-agent operation was asked of a document whose every timeline had already finished — "
                  "a document a peer still holds a reference into can still be asked, so its flows must stay "
                  "live while that reference exists: build that before this can be answered");
    for (int i = 0; i < n; i++) {
        Flow *f = flow_at(i);
        /* ONE OUTSTANDING OPERATION PER TIMELINE. A second one arriving before this flow has ANSWERED the first
           is two questions whose answers are two programs' completions, and this flow has one slot for the
           token that says which. They are sequential rather than alternative (the asking side parks on each in
           turn), so the mechanism is a QUEUE per flow — not a fork, which is what two CONTRADICTORY askers
           would need. The zone that routes them has to be able to ask twice before it can, so this crashes
           rather than answering the second question with the first one's token. */
        DCHECK(f->perform == NULL && f->answer_token == NULL,
               "a second cross-agent operation reached a flow that has not answered its first — an answer is a "
               "program's completion and this flow holds one token, so the second would be answered under the "
               "first one's name. Build the per-flow operation QUEUE (they are sequential, not alternative)");
        f->perform = strdup(record);
        f->answer_token = strdup(token);
        CHECK(f->perform && f->answer_token, "engine: OOM attaching a cross-agent operation to a flow");
        /* AND IT IS ASKABLE AGAIN — the same sentence as the delivery above, and here it is load-bearing for
           engine_set_referenced: a referenced document's last timeline reports host-owed INSTEAD of finishing,
           precisely so it is waiting when an operation arrives. This is the arrival. Without the clear, the
           flow that was kept alive to answer peers would never be picked to answer one. */
        flow_clear_host_owed(f);
    }
}


const char *engine_host_notices(void) {
    static char *drained;
    free(drained);
    drained = g_notices;
    g_notices = NULL;
    g_notices_n = g_notices_cap = 0;
    return drained ? drained : "";
}

const char *engine_pending_urls(void) {
    static char *join;
    static size_t cap;
    size_t n_out = 0;
    Flow *f;

    /* ONE PASS AND ONE CONVERSION, for the reason engine_host_requests states. The DEDUP is unchanged: a linear
       scan over the answer being built, which is the set itself — several flows park on the same URL (a
       candidate re-fire re-runs the exploring flow's fetches) and engine_provide fills every entry that names
       it, so listing it twice makes the host provide twice and the second call finds nothing left. */
    if (!join) { cap = 256; join = malloc(cap); CHECK(join, "engine: OOM joining the pending URLs"); }
    join[0] = 0;
    for (int k = 0; (f = flow_at(k)) != NULL; k++)
        for (int i = 0, n = pending_count(f->pending); i < n; i++) {
            JSValue pe = pending_entry(f->pending, i);
            JSValue uv = pending_get(pe, PEND_URL);
            size_t ul = 0;
            const char *u = JS_IsString(uv) ? JS_ToCStringLen(pending_ctx(), &ul, uv) : NULL;
            int skip = (!u || pending_get_int(pe, PEND_HAVE_VALUE));
            if (!skip) {
                const char *q = join, *stop = join + n_out;
                while (q < stop) {
                    const char *e = memchr(q, '\n', (size_t)(stop - q));
                    size_t l = e ? (size_t)(e - q) : (size_t)(stop - q);
                    if (l == ul && !memcmp(q, u, ul)) { skip = 1; break; }
                    if (!e) break;
                    q = e + 1;
                }
            }
            if (!skip) {
                while (n_out + ul + 2 > cap) {
                    cap *= 2;
                    join = realloc(join, cap);
                    CHECK(join, "engine: OOM growing the pending-URL join");
                }
                memcpy(join + n_out, u, ul); n_out += ul;
                join[n_out++] = '\n';
                join[n_out] = 0;
            }
            if (u) JS_FreeCString(pending_ctx(), u);
            JS_FreeValue(pending_ctx(), uv);
            JS_FreeValue(pending_ctx(), pe);
        }
    return join;
}

/* Deliver a body for `url` into every flow parked on it. The value lands on the flow's OWN pending entry, so the
   reaction the resolve enqueues belongs to that flow and to its COW delta — which is why this is here and not in
   a register beside it. Returns how many entries it filled. */
int engine_provide(JSContext *ctx, const char *url, JSValueConst value) {
    int n = 0;
    /* THE METHOD THE REPLY IS A REPLY TO, taken off the entry that parked on it rather than assumed. A schema
       learned from an error envelope is filed under the endpoint identity `<METHOD> <host><path>`, and this is
       the one place that holds both halves: the URL the host answered and the request record the flow kept.
       A relative `char *` would dangle the moment the entry is freed, so it is copied — one small buffer, once
       per reply, and the truncation is asserted rather than silently filing under a shortened verb. */
    char method[16];
    method[0] = 0;
    DCHECK(url != NULL, "a body was provided for no URL");
    for (int k = 0; ; k++) { Flow *f = flow_at(k); if (!f) break;
        for (int i = 0, m = pending_count(f->pending); i < m; i++) {
            JSValue p = pending_entry(f->pending, i);
            JSValue uv = pending_get(p, PEND_URL);
            const char *u = JS_IsString(uv) ? JS_ToCString(ctx, uv) : NULL;
            int hit = u && !pending_get_int(p, PEND_HAVE_VALUE) && strcmp(u, url) == 0;
            if (u) JS_FreeCString(ctx, u);
            JS_FreeValue(ctx, uv);
            if (hit) {
                if (!method[0]) {
                    JSValue mv = pending_get(p, PEND_METHOD);
                    const char *ms = JS_IsString(mv) ? JS_ToCString(ctx, mv) : NULL;
                    if (ms) {
                        size_t ml = strlen(ms);
                        DCHECK(ml < sizeof method, "a request parked with a method longer than any HTTP verb — "
                               "the learned-schema key is built from it and a truncated verb files one endpoint's "
                               "schema under another's identity");
                        if (ml < sizeof method) memcpy(method, ms, ml + 1);
                        JS_FreeCString(ctx, ms);
                    }
                }
                pending_set(p, PEND_VALUE, JS_DupValue(ctx, value));
                pending_set(p, PEND_HAVE_VALUE, JS_TRUE);
                /* AND THIS FLOW IS ASKABLE AGAIN — the reply it parked on is on its register now, which is the
                   event its host-owed mark was waiting for and the only kind of thing that can change the
                   answer it gave the scheduler (flow.h). Cleared per flow, here, because this is where the
                   host reached that flow. */
                flow_clear_host_owed(f);
                n++;
            }
            JS_FreeValue(ctx, p);
        }
    }
    /* LEARNING FROM REPLIES IS THE POINT (CLAUDE.md §Solver), and an API's own REJECTION is the richest reply
       there is: `google.rpc.Status` names the endpoint's fields, their types, its canonical service and method
       and the OAuth scopes it wants. This is the one point every fetched reply crosses exactly once — a URL two
       flows parked on is answered here once — so the learning happens here rather than in the per-flow drain,
       where it would run once per waiter. The schema is a fact about the SERVER, not about a flow's world, so
       it is not per-flow state and takes no COW capture, exactly as the endpoint surface does not.
       A method-less entry is a park with no request record at all (a docscript or an injected <script src>);
       those are code, never an error envelope, and GET is what the host performed for them. */
    if (n) req2proto_learn(ctx, method[0] ? method : "GET", url, value);
    return n;
}
/* Resolve every pending fetch this flow issued (the network completed). Returns how many were drained. */
/* Is any of this flow's pending fetches deliverable? A flow with only host-owed entries has no work — it stalls
   rather than spinning on a drain that would resolve nothing. */
static int flow_pending_ready(const Flow *f) { return pending_ready(f->pending); }

/* THE BODY OUT OF THE HOST'S REPLY RECORD. Every kind on the register is filled by ONE engine_provide call —
   a URL two flows parked on for two different reasons gets one value — so the record is the same shape for all
   three, and the two PROGRAM kinds read the field they need out of it. A network error (JS_NULL) is a script
   that did not load, which runs nothing; the fetch kind rejects on the same value, in the delivery machine. */
static JSValue reply_body_of(JSContext *ctx, JSValueConst reply) {
    DCHECK(JS_IsObject(reply) || JS_IsNull(reply),
           "a provided reply is neither the host's reply record nor a network error — qjs_provide parses the "
           "trusted zone's JSON and every host builds the same record");
    if (JS_IsNull(reply))
        return JS_NewString(ctx, "");
    return JS_GetPropertyStr(ctx, reply, "body");
}

static int flow_drain_pending(JSContext *ctx, Flow *f) {
    int n = 0, i = 0;
    while (i < pending_count(f->pending)) {
        JSValue p = pending_entry(f->pending, i);
        JSValue pv;
        int kind;
        if (!pending_get_int(p, PEND_HAVE_VALUE)) {   /* the host still owes this one */
            JS_FreeValue(ctx, p);
            i++;
            continue;
        }
        /* TAKEN OFF THE REGISTER BEFORE IT IS DELIVERED, and that ordering is the record's own lifetime. The
           delivery below runs the PAGE's code — 27.2.1.3.2 step 8 reads `Get(resolution,"then")` off an object
           whose prototype the page owns — and that code can issue another fetch, which appends to this very
           register. As a C array the walk held a `FlowPending *` into storage the append could realloc out
           from under it; as a JS record the reference here is what keeps it alive, so an append cannot move it
           and the slot it occupied cannot be walked twice. The removal is a swap-remove, so `i` is deliberately
           NOT advanced: the entry swapped into this slot has not been looked at yet. */
        pending_remove(&f->pending, i);
        kind = (int)pending_get_int(p, PEND_KIND);
        pv = pending_get(p, PEND_VALUE);
        if (kind == FLOW_PENDING_DOCSCRIPT) {
            /* the DOCUMENT's text, shared by every flow: fill the slot once and all waiters proceed in order */
            int si = (int)pending_get_int(p, PEND_SCRIPT_I);
            if (!g_sess_bodies[si]) {
                JSValue bv = reply_body_of(ctx, pv);
                const char *body = JS_ToCString(ctx, bv);
                DCHECK(body != NULL, "an external document script's body did not arrive as text");
                g_sess_bodies[si] = body ? strdup(body) : strdup("");
                CHECK(g_sess_bodies[si], "engine: OOM storing an external document script");
                if (body) JS_FreeCString(ctx, body);
                JS_FreeValue(ctx, bv);
                /* THE ONE UNBLOCKING THAT HAPPENS INSIDE A SLICE, AND THE ONE CLEAR THAT NAMES NO FLOW. This
                   text is the DOCUMENT's, not this flow's: every flow parked at the same script index was
                   waiting for exactly this slot, and their own register entries are not what changed. So every
                   member is askable again — the whole-frontier clear exists for this case and for no other
                   (flow.h). Inside the `if`, because only the flow that actually FILLS the slot has unblocked
                   anyone; a second drainer of the same URL changes nothing. */
                flow_clear_host_owed_all();
            }
        } else if (kind == FLOW_PENDING_SCRIPT) {
            /* the reply is PROGRAM: it joins this flow's script sequence, and the one BFS runs it */
            JSValue bv = reply_body_of(ctx, pv);
            const char *body = JS_ToCString(ctx, bv);
            DCHECK(body != NULL, "an injected script's body did not arrive as text");
            if (body) { engine_queue_script(body); JS_FreeCString(ctx, body); }
            JS_FreeValue(ctx, bv);
        } else if (kind == FLOW_PENDING_MODULE) {
            /* the reply is a MODULE's SOURCE: settle the load's promise with the text, and the import's own
               reaction compiles, links and continues the flow that wrote `await import(...)`. */
            JSValue bv = reply_body_of(ctx, pv);
            JSValue resolve = pending_get(p, PEND_RESOLVE);
            if (JS_CallAsFlow(ctx, resolve, bv) < 0) {
                JSValue exc = JS_GetException(ctx);
                JS_FreeValue(ctx, exc);   /* a rejected load is the page's to observe, not this drain's */
            }
            JS_FreeValue(ctx, resolve);
            JS_FreeValue(ctx, bv);
        } else if (kind == FLOW_PENDING_DISCOVERY) {
            /* the reply is a DESCRIPTION of an API: it settles no promise and runs as no program, so it goes
               straight to the component that reads it (solver/discovery.h) and what it names lands in the same
               @H surface as everything forced execution reached. The WHOLE reply record is handed over — the
               component decides what a network error and a non-JSON body mean, because those are answers about
               the address rather than states of this drain. */
            JSValue uv = pending_get(p, PEND_URL);
            const char *u = JS_IsString(uv) ? JS_ToCString(ctx, uv) : NULL;
            DCHECK(u != NULL,
                   "a discovery probe's reply arrived on an entry carrying no URL — the address is what the "
                   "document's own relative server URLs resolve against, and the park writes it at the push");
            if (u) { discovery_reply(ctx, u, pv); JS_FreeCString(ctx, u); }
            JS_FreeValue(ctx, uv);
        } else {
            /* A SYNCHRONOUS ANSWER IS TAKEN, NEVER DRAINED, and that is asserted here because this branch is
               where it would land if it were not. The machine that asked resumes through its park and consumes
               the answer with engine_host_take; the entry is gone before any drain sees it. A HOSTREQ that
               reached this line would be settled as if it were a fetch — through a `resolve` capability it does
               not have, since nothing on that path ever made a promise. */
            DCHECK(kind == FLOW_PENDING_RESOLVE,
                   "a synchronous host request's answer reached the fetch drain — its asking machine never "
                   "resumed to take it, so its parked continuation is the thing to look for");
            /* AS A FLOW, not a JS_Call. The delivery settles the page's promise, and 27.2.1.3.2 step 8 reads
               `Get(resolution, "then")` off the Response — an ordinary object whose prototype the page owns, so
               `Object.prototype.then = { get(){…} }` makes that read the page's code. Out of this drain it ran
               in a C activation with no flow base, which is the drive-to-completion this engine aborts on;
               prototype pollution is a gadget class the solver exists to RUN rather than assume away. */
            /* THE REPLY THE TRUSTED ZONE ANSWERED, DELIVERED AS IT ARRIVED. It used to be re-wrapped here —
               `fetch_reply_new(ctx, 200, "OK", NULL, body, len)` — which threw away everything safeFetch had
               seen and invented the rest: every reply this host delivered reported status 200, status message
               "OK", NO headers at all, and a URL list this zone could not have known. The record now crosses
               whole (qjs_provide parses it), so the reply the page reads is the reply the trusted zone made,
               and JS_NULL is the network error the delivery machine already knows how to reject with. */
            JSValue resolve = pending_get(p, PEND_RESOLVE);
            DCHECK(JS_IsObject(pv) || JS_IsNull(pv),
                   "a fetch reply arrived as something other than the host's reply record — qjs_provide parses "
                   "the trusted zone's JSON, and a bare string here is a host still delivering only bytes");
            if (JS_CallAsFlow(ctx, resolve, pv) < 0) {
                JSValue exc = JS_GetException(ctx);
                JS_FreeValue(ctx, exc);   /* a rejected delivery is the page's to observe, not this drain's */
            }
            JS_FreeValue(ctx, resolve);
        }
        JS_FreeValue(ctx, pv);
        JS_FreeValue(ctx, p);
        n++;
    }
    return n;
}

/* Snapshot-fork handoff: solver_decide stashes the sibling's hot decision + pins here at a forking branch;
   the interpreter then clones the frame and calls engine_fork_finalize, which assembles the sibling flow. */
static void *g_fork_dec = NULL, *g_fork_pins = NULL;
/* THE HANDOFF IS FILLED AND EMPTIED WITHIN ONE FORK, AND THAT IS ASSERTED HERE RATHER THAN HOPED FOR. Two
   pointers held between a `prepare` and a `finalize` are a slot with exactly one legal occupant: a second
   prepare arriving with the first still in it means the interpreter took the FORKED bit and never reached its
   fork hook, and the assignment then overwrites a decision blob and a pin blob that nothing else names — a leak
   per unconsumed fork, of the shared decision chain reference the sibling was going to stand on, so the whole
   frozen prefix under it stays alive too. That is the exact shape the abort seam was found in, and the only
   reason it was found is that someone went looking. The invariant belongs where it can be broken. */
void engine_prepare_fork(void *dec_blob, void *pin_blob) {
    DCHECK(g_fork_dec == NULL && g_fork_pins == NULL,
           "a sibling's snapshot state was prepared while a PREVIOUS one was still unconsumed — the branch that "
           "prepared it never reached its fork hook, and this assignment drops that flow's decision and pin "
           "blobs on the floor with nothing naming them");
    g_fork_dec = dec_blob; g_fork_pins = pin_blob;
}

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

/* THE RECLAIM SAFEPOINT'S SWITCH, declared here and defined with the partial self-park below. Two operations in
   this file hold a position in the frontier while allocating — the sibling being assembled just below, and the
   job-drop walk — and both close the safepoint around themselves with it. */
static int engine_reclaim_set(int v);

static void engine_fork_finalize(JSContext *ctx, JSValue *clone) {
    Flow *parent = flow_running();
    DCHECK(parent != NULL && g_fork_dec != NULL, "engine_fork_finalize: fork without a running flow / prepared state");
    /* A SIBLING WITHOUT A SNAPSHOT IS NOT A SIBLING. `started` is set below, so a NULL frame here produces a flow
       the scheduler believes is hot and which has nothing to resume — it compiles the program again and REPLAYS
       side effects the parent already performed. The engine's clone now CHECKs before it calls this, so this is
       the receiving end of that same contract said where the pointer arrives. */
    DCHECK(clone != NULL, "engine_fork_finalize: a fork arrived with no frame snapshot — the sibling would be "
                          "marked hot with nothing to resume and would replay the program from its start");
    /* A DELIVERY IS MADE BEFORE ANY CODE RUNS, so no flow can be at a branch while still holding its record. If
       one is, the record would be inherited by the sibling and delivered TWICE — the same message arriving in
       two timelines of one document, which no peer sent. */
    DCHECK(parent->deliver == NULL, "a flow forked while still holding a routed record — the sibling would "
                                    "inherit it and deliver the peer's one message a second time");
    /* THE SAME SENTENCE FOR AN UNSTARTED OPERATION and the OPPOSITE one for a running answer. A cross-agent
       operation is turned into a program before any code runs, so no flow can be at a branch still holding the
       RECORD — two flows would then perform one operation twice and the peer would get two answers for a
       question it asked of one timeline. But the ANSWER TOKEN is different in kind: the program that answers is
       the page's own code, so a branch inside it is a real peer timeline in which the answer differs, and the
       sibling must carry the token and answer too. That is the multiplicity §7.2.5.1 has when a document's
       state is its flows, and dropping it here would silently pick this arm's answer. */
    DCHECK(parent->perform == NULL, "a flow forked while still holding an unstarted cross-agent operation — the "
                                    "sibling would inherit the record and perform the peer's one operation a "
                                    "second time under the same token");
    /* AND A DISCOVERY PROBE CANNOT BE AT A BRANCH AT ALL. Its whole step is a park and a read (flow_step) — it
       compiles no program, so no branch hook can fire under it, and a fork arriving from one means it entered a
       program it has no business in. The sibling would not inherit the candidate either, so the probe would be
       silently duplicated as a flow with nothing to do. */
    DCHECK(parent->disc_url == NULL,
           "a DISCOVERY PROBE forked — a probe runs no program, so a branch under one means it entered the "
           "document's script sequence, and the sibling this is about to build carries no candidate and would "
           "sit in the frontier with no work at all");
    /* THE SIBLING IS A MEMBER OF THE FRONTIER BEFORE IT IS A FLOW, so nothing may page it out while this
       assembles it. flow_add publishes it into the registry on its first line and everything below is the
       CONSTRUCTION — a dozen allocations, any one of which can reach the allocator's refusal edge. A newborn
       flow carries the full optimism bonus and so is near the TOP of the ranking rather than the tail, which
       makes this a hazard that would fire rarely and read as a corrupt frontier when it did: the reclaim would
       free `sib` and every line after it would write through a dangling pointer. Same primitive, same reason
       and same shape as the job-drop walk: the engine is holding a position in its own frontier. */
    int prev_reclaim = engine_reclaim_set(0);
    /* THE SIBLING'S WORLD IS A CHILD OF THE PARENT'S, and the edge is recorded so another instance that
   already holds a segment for the parent can materialize the sibling's by forking it — the same O(1)
   shared-base-segment fork this line performs locally, performed there. */
    Flow *sib = flow_add(ctx, parent->fn, parent->world);
    /* AND ITS ACCOUNT IS A CHILD OF THE PARENT'S TOO — the WFQ's two terms, taken over at the branch like every
       other field of the parent's history copied below. It is FIRST because it is what decides where this flow
       enters the queue, and everything after this line is the construction of a flow that is already ranked. */
    flow_fork_inherit(sib, parent);
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
    /* THE ANSWER TOKEN TRAVELS, per the assertion above: this sibling resumes the same operation's program from
       the branch and completes it in its own timeline, so it owes the same peer an answer of its own. */
    if (parent->answer_token) {
        sib->answer_token = strdup(parent->answer_token);
        CHECK(sib->answer_token, "engine: OOM forking a cross-agent operation's rendezvous token");
        /* AND THE DOCUMENT IT WAS ASKED OF. The sibling resumes the SAME program, which was compiled in that
           document's realm; a sibling that inherited the token without it would answer the question and then
           compile the operation's next program somewhere else. They are one fact and travel together. */
        DCHECK(parent->perform_doc != 0,
               "a flow holding a cross-agent operation's token names no document — the two are written and "
               "cleared together, so one without the other means a program was queued outside flow_perform");
        sib->perform_doc = parent->perform_doc;
    }
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
    /* THE LIFECYCLE IS NOT COPIED HERE ANY MORE: it lives on each Document as a heap write, so the sibling
       inherits every document's readiness through the delta it forks, along with everything else the flow had
       written. A field copied here could only ever have carried ONE document's. */
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
            /* THE REALM THAT ENQUEUED IT COMES WITH IT. This was the one field of the five the copy did not
               name, and `sib->jobs` is a bare malloc, so every job a fork inherited carried a WILD realm
               pointer. Nothing reads it until §7.5.10 step 7 destroys a document — and then the comparison is
               against garbage, so the inherited reactions of the destroyed document are not removed (they run
               later against a Document whose browsing context is null) or an unrelated job matches by accident
               and a work item the WFQ may never drop is freed instead. Borrowed, exactly as it is at the
               enqueue: the agent owns its realms. */
            dj->ctx = sj->ctx;
            dj->fn = sj->fn;
            dj->argc = sj->argc;
            dj->task = sj->task;
            dj->argv = sj->argc ? malloc((size_t)sj->argc * sizeof(JSValue)) : NULL;
            CHECK(!sj->argc || dj->argv, "engine: OOM inheriting a queued job's arguments at a fork");
            for (int a = 0; a < sj->argc; a++) dj->argv[a] = JS_DupValue(ctx, sj->argv[a]);
            /* A FIELD ADDED TO FlowJob IS AN OBLIGATION HERE, and nothing but this says so — the struct copy is
               written out field by field precisely so a new one is visible, which is exactly how the omission
               above survived. The realm is the one field with an answer that can be checked. */
            DCHECK(dj->ctx != NULL, "a queued job was inherited at a fork with no realm — §7.5.10 step 7 keys on "
                                    "it, so a job without one can neither be dropped with its document nor "
                                    "safely left queued");
        }
        sib->njob = sib->jobcap = parent->njob;
    }
    /* THE ARRAY IS COPIED AND THE RECORDS ARE SHARED. The array has to be per-flow: the host walks EVERY
       flow's register from outside any flow's delta (engine_provide fills whichever flows parked on a URL,
       engine_host_requests joins what is outstanding across all of them), and each arm removes an entry when
       IT delivers. The records do not, because a record never changes after it is pushed except for the
       ANSWER, and an answer is something both arms wait on and both observe. */
    sib->pending = pending_fork(parent->pending);
    /* AN UNANSWERED SYNCHRONOUS REQUEST IS RE-ISSUED, NEVER INHERITED. Its answer is computed under the ASKING
       FLOW'S WORLD, and the sibling's world is not the parent's from this instant on — two arms of a fork that
       navigated a frame differently must not resolve to one Window. Sharing the id would deliver one answer
       into two call sites in two contradictory worlds, which is the same fabrication as merging two senders'
       messages into one timeline.
       An ALREADY-ANSWERED one keeps its id: the answer was computed before the fork existed, so both arms
       genuinely observed it. */
    for (int i = 0, n = pending_count(sib->pending); i < n; i++) {
        JSValue p = pending_entry(sib->pending, i);
        if (pending_get_int(p, PEND_KIND) == FLOW_PENDING_HOSTREQ && !pending_get_int(p, PEND_HAVE_VALUE)) {
            /* …and it is the ONE record the sibling cannot share, because this id is the one field the two
               arms must disagree about. It stops being shared first. */
            JS_FreeValue(ctx, p);
            p = pending_unshare(sib->pending, i);
            pending_set_int(p, PEND_REQ, g_next_req++);
            CHECK(g_next_req != 0, "the host-request id counter wrapped while forking a blocked flow");
        }
        JS_FreeValue(ctx, p);
    }
    engine_reclaim_set(prev_reclaim);   /* the sibling is fully assembled: it may be paged like any other member */
    /* THE HANDOFFS ARE DRAINED, SAID AT THE END OF THE ONE OPERATION THAT DRAINS THEM. Both are module statics
       filled by somebody else and emptied here, which is the shape where an arm added later takes an early
       return and leaves one behind — a leaked decision blob keeps the whole frozen prefix chain under it alive,
       and a leaked generator-fork record leaves the sibling's cloned gen_data owned by nobody. */
    DCHECK(g_fork_dec == NULL && g_fork_pins == NULL,
           "a fork finished without consuming the sibling's prepared decision and pin blobs — they are freed by "
           "nothing else, and the decision blob is a reference on the shared frozen prefix chain");
    DCHECK(g_genfork_n == 0,
           "a fork finished with generator-state swaps still in the stash — the sibling's cloned gen_data was "
           "never recorded on its delta, so it is owned by nobody and its body frame is unreachable");
}

/* The frame-agnostic REPLAY fork is DELETED: re-running a nested/deep flow from its start is BANNED (not
   byte-identical — shared state can differ between the run and the re-run). A concolic branch inside an async
   body on the tramp chain now DFAILs in the engine (see branch_arm_fork) until the sound async-frame snapshot
   is built; there is no re-run fallback to hide that gap. */

/* WHAT KIND OF PROGRAM a queued body is. It is ONE queue because they are one thing — code the page caused to
   run — and the kind decides exactly two questions, both of them at the ends of that program's life: may it
   fail to COMPILE, and does anything read its COMPLETION VALUE. */
/* A CROSS-AGENT OPERATION is a fourth kind and answers those two questions differently from all three: its
   program is the ENGINE's own text, so a compile failure is this engine's bug and not the page's; and its
   completion is not merely read, it is the ANSWER a peer's flow is parked on. */
typedef enum { DYN_PAGE_SCRIPT = 0, DYN_CANDIDATE, DYN_JAVASCRIPT_URL, DYN_CROSS_AGENT_OP } DynKind;

static void engine_queue(const char *body, DynKind kind) {
    Flow *f = flow_running();   /* the running flow owns the lazy chunk it loads */
    if (!body || !f) return;
    if (f->dyn_n >= f->dyn_cap) {
        f->dyn_cap = f->dyn_cap ? f->dyn_cap * 2 : 8;
        f->dyn = realloc(f->dyn, (size_t)f->dyn_cap * sizeof(char *));
        f->dyn_cand = realloc(f->dyn_cand, (size_t)f->dyn_cap);
        CHECK(f->dyn && f->dyn_cand, "engine: OOM dynamic-script queue");
    }
    f->dyn[f->dyn_n] = strdup(body); CHECK(f->dyn[f->dyn_n], "engine: OOM dynamic-script body");
    f->dyn_cand[f->dyn_n] = (unsigned char)kind;
    f->dyn_n++;
}

/* WHICH KIND THE PROGRAM AT `script_i` IS, asked at the two places that need it — the compile and the resume.
   It is RE-DERIVED from the cursor rather than latched in a field, because the cursor is what already says
   which program is running and a second copy of that fact is a second copy that can be behind. */
static DynKind flow_dyn_kind(const Flow *f, int n) {
    if (f->script_i < n) return DYN_PAGE_SCRIPT;                    /* one of the document's own <script>s */
    if (f->script_i - n >= f->dyn_n) return DYN_PAGE_SCRIPT;
    return (DynKind)f->dyn_cand[f->script_i - n];
}

void engine_queue_script(const char *body) { engine_queue(body, DYN_PAGE_SCRIPT); }

/* AN @S CANDIDATE, queued as the program it would be if it fired. It is the same queue because it IS the same
   thing — code the page caused to run — but it carries the one difference that matters: it is allowed not to
   compile. Most breakouts do not fit most sink contexts, which is exactly why the solver tries several and
   keeps whichever FIRES; a candidate that does not parse simply never fires. */
void engine_queue_candidate(const char *body) { engine_queue(body, DYN_CANDIDATE); }

/* HTML §7.4.2.3.2's EVALUATE A JAVASCRIPT: URL, steps 6-7 — "let script be the result of creating a classic
   script given scriptSource … let evaluationStatus be the result of running the classic script script". The
   source is the page's own code, so it is a program of the running flow like a lazy chunk: preemptible,
   forkable and parkable, which a C `JS_Eval` under the live flow could never be. */
void engine_queue_javascript_url(const char *body) { engine_queue(body, DYN_JAVASCRIPT_URL); }

/* THE OPERATION BECOMES THIS FLOW'S NEXT PROGRAM. Not a call: a peer answers by RUNNING a program, and every one
   of these is the page's own code — an IDL getter, a page's setter, a page's function — which a C activation
   has no flow base under. Queued with the flow switched in, so the operands the program reads are written into
   THIS flow's delta and no sibling sees them. */
/* AND IT RUNS IN THE REALM OF THE DOCUMENT THE PEER NAMED. Every operand is installed there and the program is
   compiled there (flow_step), because that is what the operation IS: §7.2.5.1's member is read of the OTHER
   navigable's active document, and a getter answered out of this instance's root would count the root's child
   navigables and hand them back as the child's. The document is held on the flow as the HANDLE that crossed the
   wire rather than as the JSContext, for the reason every other queued platform datum is a name and not a
   pointer: a handle survives a park and a realm does not. */
static void flow_perform(JSContext *ctx, Flow *f)
{
    RemoteOp *op = remote_op_parse(f->perform);
    WorldId w, anc[16];
    int n_anc;
    CowDelta *seg;
    JSContext *rctx;

    DCHECK(flow_running() == f, "a cross-agent operation was performed while another flow was switched in — its "
                                "operands would be written into that flow's delta and its program would run "
                                "against that flow's document");
    DCHECK(f->perform_doc == 0,
           "a flow began a cross-agent operation while still naming the document of another one — the two "
           "programs would be compiled in whichever realm was written last, and one peer's question answered "
           "out of the other's document");
    f->perform_doc = world_doc_intern(remote_op_doc(op));
    rctx = doc_realm(f->perform_doc);
    n_anc = world_parse(remote_op_worlds(op), &w, anc, (int)(sizeof anc / sizeof anc[0]));
    /* ASKED AGAIN AT THE MOMENT IT RUNS, for the reason flow_deliver asks it again: the scheduler has run other
       flows since the record arrived, and which world holds writes here is a property of the run. */
    seg = world_segment(ctx, w, anc, n_anc);
    (void)seg;
    DCHECK(cow_delta_empty(seg),
           "a cross-agent operation ran in the answering flow's timeline alone while the asking world holds "
           "writes in this instance — it answers about a document missing everything the asking flow did here. "
           "Build the join of the two deltas that engine_route names");
    engine_queue(remote_op_program(rctx, op), DYN_CROSS_AGENT_OP);
    remote_op_free(op);
    free(f->perform); f->perform = NULL;   /* the TOKEN outlives it: the answer is the program's completion */
}

/* AND THE COMPLETION, READ WHERE THE SCHEDULER READS ONE. `cv` is what the program completed with — its value,
   or JS_EXCEPTION with the throw pending. THE THROW IS THE ANSWER: the peer ran a program and a program that
   threw completed just as truly as one that returned, so it is encoded with its type and raised in the flow
   that ASKED, at the line that asked, where that page's own try/catch is. Reporting it as this document's page
   error instead would lose it and answer `undefined`.
   IT CROSSES AS AN EMISSION, one-way: nothing here waits for it, so nothing has to un-send it when this flow
   parks or is outranked — the same argument that makes a cross-document message an emission. */
static void flow_answer_perform(JSContext *ctx, Flow *f, JSValueConst cv)
{
    JSValue thrown = JS_UNDEFINED;
    int completion = ENGINE_COMPLETION_NORMAL;
    char *enc, *rec;
    size_t cap;

    DCHECK(f->answer_token != NULL,
           "a cross-agent operation's program completed with no rendezvous token — the completion names no "
           "question, so the flow that asked would park on it forever");
    if (JS_IsException(cv)) {
        thrown = JS_GetException(ctx);
        completion = ENGINE_COMPLETION_THROW;
        cv = thrown;
    }
    /* ENCODED IN THE REALM THE PROGRAM RAN IN, because the value is that realm's — §3.7 gives every realm its
       own intrinsics, and a value converted through another document's is converted by a platform that is not
       the one that produced it. It is the same realm the program was compiled in, asked the same way. */
    enc = remote_completion_encode(doc_realm(f->perform_doc), completion, cv);
    cap = strlen(f->answer_token) + strlen(enc) + 24;
    rec = malloc(cap);
    CHECK(rec != NULL, "engine: OOM writing a cross-agent operation's answer — a dropped answer parks the "
                       "asking flow on a question nothing will answer again");
    snprintf(rec, cap, "remoteop.answer\t%s\t%s", f->answer_token, enc);
    engine_host_notify(ctx, rec);
    free(rec);
    free(enc);
    JS_FreeValue(ctx, thrown);
    free(f->answer_token); f->answer_token = NULL;
    /* THE DOCUMENT GOES WITH THE TOKEN, because the two are one fact: the question and the realm it was asked
       of. Left behind, the next operation this flow performs would compile its program in the previous one's
       document — and the assert that catches that is in flow_perform, which is why this is not merely tidy. */
    f->perform_doc = 0;
}

/* Preempt hook, two orthogonal yield decisions at the one per-opcode suspend point:
   (1) VALUE yield — suspend the running flow the MOMENT a parked flow outranks it (the WFQ, not a clock,
       decides which flow runs). The rival is recomputed only when the frontier membership changes (a fork
       adds a flow) or the running flow switches — cached by (gen, cur) so this is O(1) per consultation, never
       an O(flows) scan per opcode.
   (2) COOPERATIVE-QUANTUM yield — a thread-sharing floor: even a top-ranked flow breathes once it has consumed
       a slice, so the host loop can interleave / pump / snapshot. NOT a step cap: it drops/reorders no flow and
       the flow resumes byte-identically. */
/* THE QUANTUM'S EXPIRY IS ASKED, NOT COMPUTED HERE — and the clock it used to be computed from is gone with the
   arithmetic. It was `now - g_slice_start >= ENGINE_QUANTUM_MS` on a WALL clock read at each consultation, and
   that was wrong in two ways at once.
   WRONG MEASURE: §Testing says measure the thing the invariant is about — CPU actually consumed, never elapsed
   time. The quantum is about thread-SHARING, and a descheduled thread is denying nobody anything (the host that
   would use the returned thread is the same thread, descheduled with it), so wall time it did not run for is
   not budget it spent.
   WRONG SOURCE, WHICH IS THE HALF THAT ACTUALLY BROKE IT: this hook runs only when something RAISED a yield
   request, and until now everything that raised one was a shape of the PAGE'S OWN BYTECODE — a loop back-edge,
   a call, a concolic fork. A stretch of straight-line call-free code raised nothing, so the clause was not
   merely computed from the wrong clock, it was not EVALUATED AT ALL for as long as that stretch ran. A budget
   whose expiry is only noticed when the debtor volunteers is not a budget.
   Both are now one component's problem: solver/quantum.h owns the edge that raises the request asynchronously
   (a per-thread CPU timer natively) AND the measure the expiry is decided on, per host, so the answer here is a
   question rather than an arithmetic. This hook keeps the POLICY, which is all §scheduler ever wanted here. */
static int64_t engine_now_ms(void);   /* the WALL clock, for the gap census below — reported, never a verdict */
/* THE RIVAL IS CACHED; THE COMPARISON IS NOT — and caching the comparison was a second way the monopolizer kept
   the thread. `g_outranked` was a boolean recomputed only when the frontier's membership changed or the running
   flow switched, which is sound only while nothing else can move a weight. The running flow's `cpu` moves on
   every charge and bumps no generation, so the instant aging became a real quantity the cached verdict was a
   comparison made before the flow had aged: the term would have demoted it correctly and the hook would never
   have looked again.
   Only the RUNNING flow's weight moves between generation bumps — a parked flow burns no CPU, and an emit both
   changes `val` and bumps the generation — so the RIVAL's weight is constant across the cache's key and the
   O(flows) scan for it still happens only on a change. The running flow's own weight is recomputed at every
   consultation: two divisions beside a clock read the hook already performs, and exact rather than stale. */
static unsigned g_seen_gen = 0; static Flow *g_seen_cur = NULL; static double g_rival_w = -1.0 / 0.0;
/* WHAT THE FLOW HOLDING THE THREAD WAS RANKED ON WHEN IT TOOK IT — the three quantities the value yield's
   verdict is a pure function of, recorded at the switch-in and read by the assertion in the hook's value
   clause. They are not policy and they are not a cache: nothing is decided from them, and the hook's answer is
   identical with them removed. They exist so that the sentence "a top-ranked flow runs on at ~zero switch
   cost" is a check rather than a claim, at the one point where it can be false. */
static unsigned g_ranked_gen = 0; static int64_t g_ranked_notch = 0; static double g_ranked_val = 0.0;
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
   and the consumed slice, and both ask whether this flow should still hold the thread. */
static int preempt_hook(int kind) {
    (void)kind;
    Flow *cur = flow_running();
    /* THE POLICY IS ONLY EVER ASKED BY THE SCHEDULER, and this is the ONE point it is asked from, so this is
       where that is stated. Both of this hook's decisions are about the flow the scheduler is CURRENTLY
       RUNNING — its rank against the frontier, and the slice it is holding the thread on — and between two
       engine_sched_step calls there is no such flow: the thread belongs to the host, which is precisely why
       quantum_end disarms the edge over it. Something entering the interpreter on the host's own time and
       reaching this hook is therefore asking whether to park a flow nobody is driving, and the answer it gets
       is measured against a slice that belongs to nothing.
       IT WAS ONLY EVER CAUGHT FROM ONE SIDE, which is why it is asserted here rather than left to
       quantum_expired's own precondition: clause (0) and clause (1) below can both answer 1 and RETURN before
       the budget is ever consulted, so a host-time consultation that happened to be outranked or blocked was
       answered — with a real-looking verdict — and said nothing. The measured instance was the shipped ABI's
       reply record: qjs_provide ran the JSON parser between two steps, the parser consulted this hook at every
       completed value, and every reply the extension delivered aborted here. The parser offers to its DRIVER
       now (quickjs.c's json_parse_step) and asks no policy, so a hit on this line is a NEW host-time entry
       into the interpreter — give it a driver that can act on the answer, or stop consulting the policy from
       it; never widen the slice to cover it. */
    DCHECK(quantum_slice_open(),
           "the scheduler's preempt policy was consulted with NO SLICE OPEN — whoever is running this code is "
           "not the scheduler, so there is no flow whose rank or budget this answer could be about; some entry "
           "reached the interpreter on the HOST's own time between two steps");
    /* THE GAP CENSUS IS THE SEAM MESSAGE'S, SO IT IS COMPILED OUT WITH IT. Every one of these three statics is
       read only inside this file's `#if APICLIENT_DEV` seam assertion, and the clock they are built from is the
       WALL clock — which is neither the slice's measure nor a verdict, by design. A release build was therefore
       taking a clock reading at EVERY suspend-point consultation to feed numbers nothing would ever print, and
       on the host that matters most that reading is not a vDSO call at all: emscripten answers clock_gettime by
       calling into JS. This is the mirror of the defect the aging charge had — that one was a real policy left
       inside a DEV guard, this one a DEV diagnostic left outside it — and both are the same question asked once
       per hook: is this number something the ENGINE decides on, or something a developer reads? */
#if APICLIENT_DEV
    {
        int64_t now = engine_now_ms();
        g_preempt_asked++;
        if (now - g_last_ask > g_max_gap) g_max_gap = now - g_last_ask;
        g_last_ask = now;
    }
#endif
    /* THE RIVAL IS ONE THE THREAD CAN ACTUALLY BE HANDED TO. A flow that has told the scheduler it can make no
       progress until the host answers is not a candidate: yielding to it hands the thread straight back, and
       with one outranking the running flow the two thrashed — the hook demanded a yield at every back-edge and
       the loop's pick returned the same flow, so it advanced one back-edge per scheduler iteration. It is the
       SAME question the pick asks (flow.h), which is why it is the same call.
       THE CACHE MAY LAG A MARK, AND ONLY IN THE HARMLESS DIRECTION. Marks are made during a slice and cleared
       only at its top, so a cached rival can be one that has SINCE been marked — a yield the loop then declines
       by re-picking the same flow, at the cost of one iteration. A mark can never make a flow wrongly ELIGIBLE,
       which is the direction that would cost a yield that mattered. */
    if (flow_frontier_gen() != g_seen_gen || cur != g_seen_cur) {   /* (1) rescan for the rival only on change */
        g_seen_gen = flow_frontier_gen(); g_seen_cur = cur;
        Flow *rival = cur ? flow_rival_of(cur) : NULL;
        g_rival_w = rival ? flow_weight(rival) : -1.0 / 0.0;
    }
    /* (0) BLOCKED BEATS BOTH RANKINGS. A flow holding an unanswered synchronous host request cannot make
       progress no matter how it ranks, and the answer cannot arrive while it holds the thread — the host is
       only asked between steps. Deciding this by weight would re-enter it immediately and spin. */
    if (cur && flow_blocked(cur)) return 1;
    if (cur && g_rival_w > flow_weight(cur)) {   /* value yield — the rival is cached, the verdict is not */
        /* THE VALUE YIELD MAY ONLY FIRE ON A RANK CHANGE, AND THIS IS WHERE THAT IS EITHER TRUE OR A SENTENCE
           IN CLAUDE.md. §scheduler says the yield fires "the moment a parked flow outranks (or on an emit/fork/
           suspension that changes ranks)" and that "a top-ranked flow runs on at ~zero switch cost" — so a flow
           that was picked to run, and against which NOTHING has since changed, must still be running.
           THREE THINGS CAN CHANGE THE ANSWER and they are exactly the three snapshotted at the switch-in: the
           frontier GENERATION (a fork or a finish added or removed a member, and an emission bumps it too), the
           running flow's own SERVICE NOTCH (it consumed a whole quantum — a published change, and the same
           edge the cooperative yield fires on), and its own REWARD. Nothing else moves either side of the
           comparison: a parked flow burns no CPU, and its `val` cannot change without it running, which cannot
           happen while this one holds the thread. So the comparison is a pure function of those three, and a
           yield with all three unchanged means the WFQ answered two different things about one unchanged state.
           IT FIRES ON THE TREE THIS FIXES, which is the whole reason it is worth writing. The optimism term
           quantised service with a CEILING, so the first MICROSECOND a flow was ever charged moved its notch
           from 0 to 1 and cost it half the entire bonus — the notch changed, so this assertion would have
           permitted it, and the flow was then strictly outranked by every never-run sibling at its next
           back-edge. What this catches is the version of that defect with no published change at all: a tie
           handed on by the pick's registry order, a rival recomputed against a stale cache, a weight term that
           moves with something this list does not name. Any of those is a swap of two COW deltas bought with
           nothing, and at 512 flows that was 1.28 million of them for one document. */
        DCHECK(flow_frontier_gen() != g_ranked_gen ||
               flow_service_notch(cur) != g_ranked_notch || cur->val != g_ranked_val,
               "the VALUE YIELD fired on a flow whose rank nothing changed since the scheduler switched it in — "
               "same frontier generation, same service notch, same reward on both sides of the comparison, so "
               "the pick and the hook are answering one unchanged state two different ways and every swap this "
               "buys is a COW delta swap for no ranking decision at all");
        return 1;
    }
    /* (2) COOPERATIVE-QUANTUM floor — thread-sharing, not value. The same expiry the scheduler loop returns to
       the host on, asked at both levels so a flow that parks on it and a step that returns on it are one event
       seen twice. Nothing is dropped, starved or reordered across it: the flow parks and the SAME flow resumes
       byte-identically unless the WFQ says otherwise. */
    return quantum_expired();
}

/* Advance flow `f` by up to one quantum. Returns 1 when the flow has FINISHED all its scripts + lazy chunks,
   0 when it yielded mid-execution (resume it later). Each <script>/chunk is its OWN program (JS_FlowNew) run
   in document order in the shared context, under f's COW delta (set by the caller). */
/* ASYNC-AS-FLOW job-enqueue hook (installed as JS_SetJobEnqueueHook): route a promise reaction / microtask to
   the ENQUEUING flow's own queue instead of the global list, so it runs later under that flow's live COW. */
/* HOW MANY JOBS THIS DOCUMENT QUEUED AND HOW MANY IT RAN, published beside the other cost numbers for the
   same reason they are: a run whose reactions never fire and a page that queued none are indistinguishable
   from outside, and the difference is most of a modern bundle. */
static long g_jobs_q, g_jobs_run;
long engine_jobs_queued(void) { return g_jobs_q; }
long engine_jobs_run(void) { return g_jobs_run; }

static int engine_enqueue_job(JSContext *ctx, JSJobFunc *fn, int argc, JSValueConst *argv, bool is_task) {
    Flow *f = flow_running();
    g_jobs_q++;
    /* THERE IS NO GLOBAL DRAIN. Declining here hands the job to quickjs's global list, and nothing in this
       engine ever runs that list — so the job is not "deferred to the default", it is DROPPED. Every task
       source goes through here: a window message, a port delivery, a broadcast, a timer callback, a custom
       element reaction. A dropped one is a handler the page registered and this engine never entered, which is
       invisible from the outside and looks exactly like a page that does nothing on message. */
    DCHECK(f != NULL, "a job was enqueued with no flow running — there is no global drain, so it would be "
                      "dropped: seed it as a flow on the frontier instead of declining it here");
    if (!f) return 0;
    if (f->njob >= f->jobcap) {
        f->jobcap = f->jobcap ? f->jobcap * 2 : 4;
        f->jobs = realloc(f->jobs, (size_t)f->jobcap * sizeof(FlowJob));
        CHECK(f->jobs, "engine: OOM flow job queue — a dropped reaction corrupts async exploration");
    }
    FlowJob *j = &f->jobs[f->njob++];
    DCHECK(ctx != NULL, "a job was enqueued with no realm — §7.5.10 step 7 removes a destroyed document's tasks "
                        "by comparing this, so a job without one outlives its document");
    j->ctx = ctx; j->fn = fn; j->argc = argc; j->task = is_task;
    j->argv = argc ? malloc((size_t)argc * sizeof(JSValue)) : NULL;
    if (argc) CHECK(j->argv, "engine: OOM job argv");
    for (int i = 0; i < argc; i++) j->argv[i] = JS_DupValue(ctx, argv[i]);
    return 1;   /* host owns it */
}

/* THE OTHER HALF OF engine_enqueue_job (installed as JS_SetJobDropHook): HTML §7.5.10 step 7, for the jobs this
   scheduler TOOK. Nothing else can do it — declining to register this hook would leave every destroyed
   document's reactions queued on whichever flow enqueued them, and each one would later run in a Document
   whose browsing context is null.
   IT WALKS EVERY FLOW, not the running one. A job belongs to the flow that enqueued it and a document is
   destroyed by whichever flow removed its container, which is not the same flow — a walk of the running flow's
   queue alone would drop the ones that happen to be there and silently keep the rest. */
static int engine_drop_jobs(JSContext *ctx) {
    int dropped = 0;
    /* NOT WHILE THIS WALK HOLDS AN INDEX. It runs inside a flow step, where the allocator's refusal edge is
       armed to page a member of the frontier out — and the registry removes by swapping its last member into
       the hole, so a removal here would move a flow the walk has not reached yet to a position it has already
       passed. That flow's destroyed-document jobs would stay queued and run against a Document whose browsing
       context is null: a silently skipped work item, which is what §scheduler's razor forbids. A refusal across
       this walk is simply a refusal; the step's next allocation is armed again. */
    int prev_reclaim = engine_reclaim_set(0);
    for (int k = 0; ; k++) {
        Flow *f = flow_at(k);
        if (!f) break;
        for (int i = f->njob - 1; i >= 0; i--) {
            if (f->jobs[i].ctx != ctx) continue;
            for (int a = 0; a < f->jobs[i].argc; a++) JS_FreeValue(ctx, f->jobs[i].argv[a]);
            free(f->jobs[i].argv);
            memmove(f->jobs + i, f->jobs + i + 1, (size_t)(--f->njob - i) * sizeof(FlowJob));
            dropped++;
        }
    }
    engine_reclaim_set(prev_reclaim);
    return dropped;
}

/* Run ONE of the flow's queued jobs under its currently-applied COW; free its args + result.
   THE PICK IS HTML 8.1.7's MICROTASK CHECKPOINT, not a FIFO pop. Within a queue the order is arrival order, but
   a TASK may not begin while this flow still holds a microtask — a plain FIFO ran `setTimeout(f, 0)` in the
   middle of a promise chain, which is the one ordering the event loop exists to forbid. */
static void flow_run_one_job(JSContext *ctx, Flow *f) {
    int pick = 0;
    while (pick < f->njob && f->jobs[pick].task) pick++;
    if (pick == f->njob) pick = 0;   /* nothing but tasks: the checkpoint is done, run the earliest task */
    FlowJob j = f->jobs[pick];
    memmove(f->jobs + pick, f->jobs + pick + 1, (size_t)(--f->njob - pick) * sizeof(FlowJob));
    g_jobs_run++;
    JSValue r = j.fn(ctx, j.argc, (JSValueConst *)j.argv);   /* the reaction runs in this flow's timeline */
    /* A JOB THAT THREW IS A PAGE ERROR, exactly like a script that threw, and this dropped it. A promise
       reaction, a queueMicrotask callback and a delivered message all run here — so an uncaught throw inside
       any of them vanished: no report, no result entry, nothing to say the page's own code had failed. It also
       made every observable this engine has blind to jobs, which is why "does the microtask run at all" could
       not be answered from outside. §8.1.7.5's report hook is the same one a script uses. */
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        result_page_error_value(ctx, e);
        JS_FreeValue(ctx, e);
    }
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

/* DECLARED ABOVE THE ONE FUNCTION THAT WRITES THEM, because flow_step is where a program is compiled and
   where a flow is finished — the two events these count. The commentary on what each MEANS stays with the
   accessors that publish them; this is the definition, and it has to precede its first use. */
static long g_finished;
static int  g_deepest = -1;

static int flow_step(JSContext *ctx, Flow *f, char **bodies, int n) {
    /* A FLOW IS ABOUT TO RUN, SO THE TWO MARKS THAT SAY SO MUST BE ON. This is the ONE point a flow is driven
       (every JS_FlowNew / JS_FlowResume in this engine is below this line), which is why the claim is made
       here rather than at each of them. The generation is what stamps every object the flow creates, and
       g_dom_capture is its twin over the Lexbor tree; with either off, the flow's own creations would be
       recorded as BASELINE and its DOM writes would land nowhere — the delta would be quietly incomplete and
       the run would look perfectly healthy. Half of one invariant, stated at the half that runs. */
    DCHECK(JS_FlowGen() != 0 && g_dom_capture,
           "a flow is being stepped with the flow stamp or the DOM capture off — the objects it creates would "
           "be stamped baseline and its DOM writes would be captured into no delta, so its state would leak "
           "into every sibling and its rewind would restore something that never existed");
    for (;;) {
        /* THE PARKED CONTINUATION OUTRANKS EVERYTHING ELSE THIS FLOW COULD DO — that is the park's whole
           contract: a forced preempt must be transparent to observable ordering, so the flow resumes BEFORE any
           job it has queued. Yielding after one resume keeps the scheduler in charge of fairness; the park (if
           it parks again immediately) rides the switch-out with the flow. Without this the solver host never
           pumped the slot at all: the continuation sat there until a second flow parked and asserted. */
        g_step_unit = "resume-parked-continuation";
        /* Resuming a parked continuation is progress ONLY if the flow can still get somewhere: a flow blocked
           on the host resumes into the same wait, so reporting progress spins it against the park. */
        if (JS_ResumeParkedFlow(JS_GetRuntime(ctx))) return flow_blocked(f) ? FLOW_STEP_OWED : 0;
        if (!f->frame) {
            const char *body;
            /* WHICH KIND the program about to be compiled is — only a page <script> must parse. */
            DynKind kind = DYN_PAGE_SCRIPT;
            /* THE ROUTED DELIVERY THIS FLOW EXISTS TO MAKE, and it is first because it is the flow's reason to
               exist: the task it enqueues is what every branch below then finds on the queue. Consumed once —
               the record is freed and cleared — so a resumed delivery flow falls through to its jobs. */
            if (f->deliver) { g_step_unit = "routed-delivery"; flow_deliver(ctx, f); return 0; }
            /* AND THE OPERATION A PEER IS PARKED ON, before this flow's own programs for the reason the
               delivery is: it is a work item another agent's flow is suspended at, and it becomes one of THIS
               flow's programs — after which the loop below runs it like any other. */
            if (f->perform) { g_step_unit = "cross-agent-operation"; flow_perform(ctx, f); return 0; }
            /* A DISCOVERY PROBE — the flow the ENGINE seeded to read one published API description
               (solver/discovery.h). It is before the document's script sequence because it HAS no script
               sequence: the probe is the whole flow, so it must never enter a program, and its three states are
               the ordinary ones every other flow already has. Not a driver and not a loop — one unit per step,
               like every branch here:
                 - nothing parked yet  -> park on the address and report OWED, so siblings get the thread;
                 - parked, unanswered  -> OWED again, and the flow is out of the pick until the host provides;
                 - answered            -> the ONE drain reads it (the DISCOVERY kind above) and the flow is
                                          DONE, because a probe's whole work is the document it just read. */
            if (f->disc_url) {
                g_step_unit = "discovery-probe";
                if (pending_count(f->pending) == 0) {
                    engine_pending_discovery_url(ctx, f->disc_url);
                    return FLOW_STEP_OWED;
                }
                if (!flow_pending_ready(f)) return FLOW_STEP_OWED;
                flow_drain_pending(ctx, f);
                /* AND IT FINISHES HOLDING NOTHING, asserted where "finished" is decided for it. The drain
                   removes the entry it delivered, and reading a document runs no page code — so a probe that
                   reaches here with a job queued or a reply outstanding has done something this branch does
                   not describe, and flow_release would drop it as a work item nothing replays. */
                DCHECK(pending_count(f->pending) == 0 && f->njob == 0,
                       "a discovery probe finished holding work — its one reply is drained and it queues no "
                       "job, so anything left here is a work item the ONE frontier is about to drop");
                return FLOW_STEP_DONE;
            }
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
                                                   kind = flow_dyn_kind(f, n); }
            else if (f->njob > 0) {
                /* A JOB CAN PARK, and until now that park was invisible to the scheduler. A queued step machine
                   that suspends on a synchronous host request is parked by reaction_flow_step (JS_ParkFlow) and
                   this returned PROGRESS — so the flow was resumed, parked again, resumed again, forever, and
                   never reported host-owed. The host was therefore never asked, and the answer that would have
                   let it finish could not arrive: a livelock that looks exactly like slowness, because every
                   turn is "progress".
                   It is the same rule the mid-frame yield already keeps: a blocked flow has no work, whatever
                   it just did. OWED is the register the scheduler already has for waiting-not-finished. */
                g_step_unit = "run-one-job";
                flow_run_one_job(ctx, f);
                return flow_blocked(f) ? FLOW_STEP_OWED : 0;   /* scripts done -> drain a microtask, yield */
            }
            else if (pending_count(f->pending) > 0 && !flow_pending_ready(f))
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
            /* NOTHING QUEUED, NOTHING OWED. What follows is what becomes due when the flow has nothing else,
               in the order it becomes due: first the load lifecycle, which is already due (a parser finishing
               waits on no clock), and only then the two CLOCK-DRIVEN sources — and by the time control reaches
               here everything that was already due (this flow's jobs above, a reply the host owes) has been
               offered a turn. */
            /* A DOCUMENT FINISHED LOADING, in this flow's world — DOMContentLoaded across the agent's
               documents in tree order, then `load` innermost-first, one per turn. It comes BEFORE the two
               clock-driven sources and that is the spec's order rather than a preference: the parser
               completing is not a timer and not a frame, it is already due, and everything that is due runs
               before the clock may move. It used to sit AFTER them, so a `setTimeout(f, 0)` a parse-time
               script set ran before DOMContentLoaded — which no browser does — and a rendering opportunity
               would have preceded it too. A page's real work is behind these events: the half of a bundle
               that touches the DOM and calls the API runs here. */
            else if (g_docdone_hook && g_docdone_hook(ctx)) {
                g_step_unit = "document-lifecycle-stage"; return 0; }
            /* §8.1.7.3's IN-PARALLEL HALF, asked first of the two clock-driven sources because it is the one
               that can defer: it compares the next rendering opportunity with the earliest timer expiry and
               yields when the timer is earlier. Without a rendering opportunity there is no
               requestAnimationFrame, no ResizeObserver delivery, no IntersectionObserver task, no
               scroll/resize/pagereveal and no Web Animations microtask checkpoint — a large fraction of a real
               page's code hangs off exactly those. */
            else if (g_rendering_hook && g_rendering_hook(ctx)) {
                g_step_unit = "queue-rendering-opportunity"; return 0; }
            else if (g_timer_hook && g_timer_hook(ctx)) { g_step_unit = "fire-due-timer"; return 0; }
            /* HTML §8.1.7.5 "notify about rejected promises". The flow has nothing left to run, so every
               rejection still on its list is one no handler will ever be attached to. The browser half keeps
               the lists and fires `unhandledrejection`; those fires are JOBS, so the flow has work again and
               the loop picks them up like any other. Only what the page did not cancel comes back through the
               report hook — and what it means then is this half's answer, the same thing a script that threw
               means: a capability the page needed. Notifying clears the list, so the next pass finds none. */
            else if ((g_step_unit = "unhandled-rejection-notify", unhandled_rejection_notify(ctx))) return 0;
            else {
                /* A FLOW MAY NOT FINISH HOLDING WORK. Every branch above claims to have offered its queue a
                   turn, so reaching here with a job still on it means one of them returned first and the job
                   is about to be dropped with the flow — silently, because a dropped reaction looks exactly
                   like a page that registered no handler. Asserted at the one place "finished" is decided. */
                DCHECK(f->njob == 0, "a flow finished holding queued jobs — a promise reaction, a timer "
                                     "callback or a delivered message would be dropped with it");
                /* …AND A FLOW OF A REFERENCED DOCUMENT MAY NOT FINISH AT ALL — engine.h's engine_set_referenced.
                   Having nothing left to run is not being done when a peer can still ask this timeline
                   something; it is waiting on the host for the next operation, which is exactly what OWED
                   means. The flow keeps its snapshot, its delta and its rank and is out of the pick until the
                   host has something for it. */
                if (g_referenced) return FLOW_STEP_OWED;
                return 1;   /* all scripts + chunks + microtask jobs + live fetches + load listeners done */
            }
            /* NULL ScriptOrModule name: an inline page script's name is the DOCUMENT's URL, which this host does
               not model yet — nothing here has one to give. It is what a relative `import('./chunk.js')` resolves
               against, so the moat's lazy-chunk surface needs the document URL plumbed to this call. */
            g_step_unit = "compile-program";
            /* NO REPLAY, asserted at the only place a program can start. A flow compiles each entry of its
               sequence once and thereafter RESUMES the suspended frame; reaching this line again for an index
               it already started means the resume path lost the frame and the flow is re-executing a program —
               re-running side effects it already performed, against a delta that already holds them. That is
               the one thing this scheduler must never do, and nothing was checking it. */
            DCHECK(f->script_i > f->last_compiled,
                   "a flow compiled a program it had already started — the suspended frame was lost and the "
                   "flow is REPLAYING it, re-running side effects against a delta that already holds them");
            f->last_compiled = f->script_i;
            /* THE DEEPEST PROGRAM THIS DOCUMENT HAS EVER REACHED, recorded where a program is STARTED and by
               whichever flow starts it — a coverage fact about the document, not about the flow holding the
               thread (see g_deepest). */
            if (f->script_i > g_deepest) g_deepest = f->script_i;
            /* IN THE REALM THE PROGRAM BELONGS TO, which for a CROSS-AGENT OPERATION is the realm of the
               document the PEER named and not the one this session was rooted in. A program compiled here is
               closed over the compiling realm's global (JS_FlowNew) and `globalThis[member]` is exactly the
               read the operation is, so compiling it in the root would read the ROOT Window's member and hand
               it back as the child's — the whole reason a document names its realm at all. */
            f->frame = JS_FlowNew(kind == DYN_CROSS_AGENT_OP ? doc_realm(f->perform_doc) : ctx,
                                  body, strlen(body), NULL, 0);   /* page <script>/chunk: classic non-strict global */
            if (f->frame == NULL) {
                /* WHAT ACTUALLY FAILED, read before anything is decided from it. A compile can fail two ways
                   and they are not the same event: a SyntaxError is the program's, and OUT OF MEMORY is the
                   physical floor — the frontier could not hold another flow. Reporting the second as the first
                   sends every reader looking for a parse bug in code that parses; it cost most of a session. */
                JSValue exc = JS_GetException(ctx);
                bool oom = JS_IsOutOfMemoryError(ctx, exc);

                /* OOM IS A `CHECK`, in dev and in release alike: a dropped flow corrupts the frontier, and
                   there is no version of this the engine may proceed past.
                   AND REACHING IT NOW MEANS SOMETHING NARROWER THAN IT USED TO, because the partial self-park
                   is built and this allocation is INSIDE its safepoint. The refusal that produced this NULL
                   already went through the runtime's reclaim edge (quickjs.h's JSMemoryReclaimFunc, answered by
                   engine_reclaim_tail below), which paged the lowest-weight member out to the cold tier and
                   retried — and it did that for as long as it had a member to give. So a NULL here is not "the
                   frontier could not hold another flow"; it is "the frontier is down to the flow that is
                   RUNNING and one more allocation would not fit". That is the physical floor with nothing left
                   to sell, and the only thing carrying past it is the host: a whole-engine park (Level-1
                   eviction) frees the RAM of a document this instance should not be holding at all.
                   WHAT IS STILL NOT COVERED, so that a reader standing here knows where to look: the reclaim is
                   armed only inside a flow step (engine_reclaim_allow), because that is the one region in which
                   the engine holds no position in its own frontier. An allocation refused at HOST time — in a
                   walk over the flows, in the result document, in a reply being routed — is refused with the
                   tail still unsold, and its failure is reported by whoever asked for it rather than here. */
                CHECK(!oom, "the frontier could not hold another flow after the cold tier had already paged out "
                            "every member it could — this is the physical RAM floor with the tail already sold. "
                            "The engine is down to the flow that is running; what carries past this is the "
                            "HOST's Level-1 eviction (engine_request_park), which gives up this whole document's "
                            "residue for a document worth more");
                /* AN @S CANDIDATE THAT DOES NOT PARSE is a dead candidate and nothing more — the search tries
                   several breakouts per sink precisely because most do not fit most contexts. A `javascript:`
                   URL that does not parse is HTML §7.4.2.3.2's abrupt evaluation, which produces no Document and
                   no navigation — `<a href="javascript:{{{">` is a link that does nothing, not an engine bug. A
                   PAGE script that does not compile is a different thing entirely and still asserts. */
                DCHECK(kind != DYN_PAGE_SCRIPT, "flow_step: a page <script>/chunk did not compile");
                /* A CROSS-AGENT OPERATION'S PROGRAM IS THE ENGINE'S OWN TEXT (core/frame/remote_op.c), so it
                   parses or this engine wrote it wrong — and skipping it would leave the peer's flow parked on
                   an answer that is now never coming. */
                DCHECK(kind != DYN_CROSS_AGENT_OP,
                       "the program that performs a cross-agent operation did not compile — it is this engine's "
                       "own text, and skipping it parks the asking flow on an answer nothing will send");
                JS_FreeValue(ctx, exc);
                /* STEP OVER IT. Not advancing left the flow pointing at the same unparseable body, so the next
                   scheduler step compiled it again, and again — the flow could never finish and never made
                   progress. It was invisible because the search seeds several breakouts per sink and most do
                   not fit most contexts, so a dead candidate looks exactly like a busy one from the outside;
                   the no-replay DCHECK at the compile site is what named it. Dead means SKIPPED, which is what
                   "a dead candidate and nothing more" was always supposed to mean. */
                f->script_i++;
                return 0;
            }
        }
        {
            /* A <script>'s completion value is not observable to the page (only an eval API surfaces one), so it is
               taken and released here — never DISCARDED by the engine, which would hide a live value from the host. */
            JSValue cv = JS_UNDEFINED;
            g_step_unit = "resume-program";
            int r = JS_FlowResume(ctx, (JSValue *)f->frame, &cv);
            /* A CROSS-AGENT OPERATION'S COMPLETION IS AN ANSWER, AND IT IS ASKED FIRST because the two readings
               of a throw are mutually exclusive: this program is another agent's operation, so its throw
               belongs to the flow that ASKED — reported here as this document's page error it would be lost and
               the peer would resume with `undefined` where the spec propagates a throw. */
            if (r == 0 && flow_dyn_kind(f, n) == DYN_CROSS_AGENT_OP)
                flow_answer_perform(ctx, f, cv);
            /* A SCRIPT THAT THREW names a capability the page needed and this engine does not have. Ending the
               flow there is intentional; losing WHICH capability was not. */
            else if (JS_IsException(cv)) {
                JSValue e = JS_GetException(ctx);
                result_page_error_value(ctx, e);
                JS_FreeValue(ctx, e);
            }
            /* ONE PROGRAM'S COMPLETION VALUE IS READ, AND IT DECIDES A NAVIGATION. HTML §7.4.2.3.2's
               evaluate-a-javascript:-URL step 9: "if evaluationStatus is a normal completion, and
               evaluationStatus.[[Value]] is a String, then set result to it" — and a non-null result becomes a
               new Document, built from a synthesized `text/html` response whose body is that string, which
               REPLACES the target navigable's active document. Every other completion is step 10's null, which
               is the ordinary `javascript:doThing()` and is finished here.
               This is the only place in the engine where that value exists, which is why the condition is
               asked here rather than at the row that queued the program. */
            if (r == 0 && JS_IsString(cv) && flow_dyn_kind(f, n) == DYN_JAVASCRIPT_URL)
                DFAIL("a `javascript:` URL evaluated to a STRING — HTML §7.4.2.3.2 step 9 turns that into a new "
                      "Document that REPLACES the target navigable's active document, built from a synthesized "
                      "`text/html;charset=utf-8` response whose body is the string. §7.4's navigate can only "
                      "load an address the host FETCHES (navigable.c's js_nav_load_step asks "
                      "`document.fetch\\t<url>`), so build the navigate that takes a RESPONSE THE ENGINE ALREADY "
                      "HAS and route this through it");
            JS_FreeValue(ctx, cv);
            if (r == 1) {
                /* A MID-FRAME YIELD, and the one case where it is not "more work". A flow that suspended
                   inside a machine holding an unanswered synchronous host request has no work at all until the
                   host answers, and reporting it runnable would have the scheduler hand it the thread again
                   immediately — the spin the blocked yield above exists to prevent. OWED is the register the
                   scheduler already has for exactly this: waiting, not finished. */
                return flow_blocked(f) ? FLOW_STEP_OWED : 0;
            }
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

/* TWO FACTS THE SCHEDULER HAS AND HAS NEVER SAID, and both of them are questions that were being ANSWERED BY
 * INFERENCE from numbers that do not mean what they were read as.
 *
 * `g_finished` — how many flows have ever reached flow_step's "all scripts, chunks, jobs, replies and load
 * listeners done" and been finished. It was being read off `live == flows`, which is a comparison of the
 * frontier's CURRENT size against the number ever CREATED: those are equal whenever creations and finishes
 * happen to balance, and they are also equal when nothing has ever finished. One number says which, and "not
 * one flow has ever completed" is a strong enough claim about a scheduler that it should not be a subtraction
 * anyone has to trust.
 *
 * `g_deepest` — the highest program index this DOCUMENT has ever compiled, across every flow. The progress
 * line's `script` is the CURRENT flow's cursor, which says what the flow holding the thread is doing and
 * nothing about the document's coverage: a run reporting `script: 1` forever may be one where no flow has ever
 * reached the second <script>, or one where thousands have and the flow that happens to hold the thread is at
 * its first. Those need opposite fixes, and until now they read identically. */

/* THE WORK THIS ENGINE HAS PERFORMED — forks taken, flows created, jobs run, context switches. ONE definition,
   because two consumers ask the same question about it and they were asking two different ones.
 *
 * The seam verdict below measures a single step's SHARE of this, on the argument that being descheduled cannot
 * inflate any of the four (a thread that is not running performs none of them). The PROGRESS STREAM needs
 * exactly the same quantity and was keyed on `g_switches` alone — which is a subset, and in the one run that
 * most needs reporting it is a subset that never moves. Measured on the minimal fixture: the unknown-length
 * walk holds the thread because nothing outranks it, so `switches` stays at 1 for the whole run and the stream
 * emitted ONE line while RSS climbed past two gigabytes. A cadence keyed on a counter that stops is a report
 * that goes silent exactly when there is something to report, which is the same defect as a corpus file the
 * collector does not collect: the output LOOKS complete. Keying both on the same "work done" fixes it and also
 * makes the two agree by construction rather than by two people remembering to. */
static long engine_work_done(void) {
    return decide_fork_total() + flow_created_count() + g_jobs_run + g_switches;
}

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
    /* WHAT THIS FLOW WAS RANKED ON WHEN IT TOOK THE THREAD. Recorded HERE because this is the moment the WFQ's
       answer was "this one" — the pick that led here compared it against every runnable member and found none
       strictly better, so from this instant the value yield may only fire if one of these three moves. The
       hook's assertion reads them; nothing decides from them. */
    g_ranked_gen = flow_frontier_gen(); g_ranked_notch = flow_service_notch(f); g_ranked_val = f->val;
}

static void flow_finish(JSContext *ctx, Flow *f) {   /* f completed: tear down its interleaving state + remove */
    g_finished++;   /* the one place a flow ever COMPLETES — see g_finished */
    /* "all scripts, chunks, jobs and fetches are done" cannot be true with a continuation still parked — the
       loop above resumes one before it can answer that. Asserting it here is what keeps the park inside the
       no-work-item-is-ever-dropped rule rather than merely intending to. */
    DCHECK(!JS_HasParkedFlow(JS_GetRuntime(ctx)) && f->park_fn == NULL,
           "a flow finished with a continuation still parked — that flow's async activation is dropped");
    /* A FINISHED FLOW HAS NO LIVE FRAME. `frame` is the JS_FlowNew handle holding this flow's heap frame chain
       — every activation, closure and local it is suspended across — so one left behind at finish retains the
       whole execution graph, and the runtime's leak walk reports it as thousands of anonymous Functions with no
       hint of the owner. Asserted rather than freed defensively: if a flow can reach here with one, the finish
       path ran while it was still suspended and freeing it silently would hide that. */
    DCHECK(f->frame == NULL, "a flow finished with a live preemptible frame — its whole activation chain, and "
                             "everything those frames close over, is retained by a handle nothing will free");
    DCHECK(f->deliver == NULL, "a delivery flow finished without making its delivery — the record it was seeded "
                               "with is a message the peer sent and this document never received");
    /* AND THE SAME FOR AN OPERATION, at both ends of it: a record never turned into a program is a question
       nobody performed, and a token never spent is a peer's flow parked at the line that asked, forever. */
    DCHECK(f->perform == NULL && f->answer_token == NULL,
           "a flow finished holding a cross-agent operation — either the record was never performed, or its "
           "program's completion was never sent, and either way the flow that ASKED is suspended at the line "
           "that asked it with nothing coming");
    /* THE QUEUE AND THE PENDING LIST ARE EMPTY HERE, AND THAT IS ASSERTED RATHER THAN CLEANED UP AFTER. Both
       used to be walked and freed "defensively" right here, which is the fallback shape: the walk can only ever
       run when a work item is being DROPPED, and freeing it quietly is precisely how that drop stays invisible.
       Neither is reachable — flow_step decides "finished" only after offering the job queue a turn (njob > 0
       runs one), and only when the register is empty, since a pending entry with a value drains and one without
       reports host-owed. The release below DOES free both, because an EVICTED flow legitimately holds them (the
       cold tier's recipe re-enqueues the reactions and re-issues the requests as it replays). So the assertion
       has to stand HERE, where "finished" is the claim being made, and not there. */
    DCHECK(f->njob == 0, "a finishing flow still held queued jobs — its promise reactions and timer callbacks "
                         "are being dropped, and the release below is what would hide that");
    DCHECK(pending_count(f->pending) == 0,
           "a finishing flow still held pending host replies — a flow awaiting one is parked, not finished, "
           "and the release below is what would hide that");
    /* A FINISH IS A SWITCH-OUT FOLLOWED BY A RELEASE, and it is spelled that way rather than as its own
       teardown. It used to be one: `dom_revert` restored the document and discarded the head, `cow_delta_free`
       reverted the whole installed chain to the baseline, and nine fields were freed inline — a second copy of
       what the frontier's teardown does, which had already drifted from it. The release (solver/flow.h) is the
       one that also serves an EVICTED flow, so routing the finish through it is what keeps the eviction path
       exercised by every flow that ever ends instead of only by an OOM no test reaches.
       decide_leave rather than decide_suspend: a finishing flow's chain reference ends here, and there is no
       blob for the release to free. Everything else is the switch-out, in its order — the heap delta unapplied
       and un-currented, then the document, whose head and base chain come back ONTO the flow so the release
       frees the live buffers rather than the stale pointers the run may have realloc'd away from. */
    decide_leave(ctx);
    cow_unapply(ctx, (CowDelta *)f->delta); cow_set_current(NULL);
    dom_unapply();
    f->dom = dom_buf_take(&f->dom_n, &f->dom_cap);
    f->dom_base = dom_base_take();
    flow_set_running(NULL);
    flow_release(ctx, f);
}

/* THE ONE BFS SCHEDULER — explore and @S candidate-verify are the SAME loop, differing ONLY in whether a concolic
   branch FORKS. A separate verify executor (a `while(JS_FlowResume){}` driving one candidate to completion with
   preemption off) is the cardinal violation twice over — a second scheduler beside the BFS AND a drive-to-
   completion (an unbounded candidate loop would hang, non-parkable). So verify is this same loop with forking off:
   ONE concrete path (no branch/fork hook), yet every candidate flow is preemptible + parkable like any other. */
static const JSFlowControlHooks FC_EXPLORE = { .branch = solver_decide, .outcome = solver_outcome,
                                               .fork = engine_fork_finalize, .preempt = preempt_hook };
static const JSFlowControlHooks FC_VERIFY  = { .preempt = preempt_hook };   /* candidate re-fire: no fork, still preemptible */
static const JSFlowControlHooks FC_OFF     = { 0 };

/* THE WALL CLOCK, AND IT NOW TIMES NOTHING THE SCHEDULER DECIDES ON. It used to be "ONE clock for both things
   this file times: the cooperative slice and the WFQ's aging charge", and both of those moved to the quantum's
   own measure (solver/quantum.h) — the slice because §Testing says a budget is CPU actually consumed, the aging
   charge because §WFQ calls its term CPU-aging and the two must be in one currency. What is left for elapsed
   time is the seam message's REPORTED numbers: the gap between two suspend points, and how long a step took on
   the wall. Both are printed and neither is a verdict, which is exactly the distinction §Testing insists on.
   CLOCK_MONOTONIC, so a wall-clock adjustment cannot make a reported gap negative and read as an impossibility.
   MICROSECONDS IS THE PRIMITIVE and the resolution is still load-bearing: milliseconds would quantise a
   sub-millisecond step to zero, and the gap census is built out of exactly those. */
static int64_t engine_now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static int64_t engine_now_ms(void) { return engine_now_us() / 1000; }

/* THE SESSION'S HOOKS COME BACK OFF, in ONE place, because there are now two ways for a session to end and
   they must leave the runtime in the same state. A frontier that DRAINED and a frontier that was PAGED OUT
   differ in what remains on the queue, not in what this engine has installed in the runtime — and a second
   copy of this list is exactly the hand-copied list build.mjs warns about, where the park path would quietly
   leave a fork hook installed over a runtime whose flows are gone. */
static void engine_session_close(void) {
    /* THE FRONTIER A CLOSED SESSION LEAVES BEHIND IS A SET OF SNAPSHOTS, and that is a property of ENDING rather
       than of the park path that happened to need it first. Every flow's decision state has to be in its own
       blob, its COW delta unapplied and its DOM head detached — which is true of every member except the one
       the scheduler is HOLDING, whose state is live in decide.c's globals and applied to the heap and the
       document. So the last act of a session is the ordinary suspend, exactly as the park takes it (and the
       park path, which must take it before it WRITES, has already left g_sess_cur NULL by the time it gets
       here). Without this a session that ended any other way handed flow_registry_free a flow whose delta was
       still applied, and the teardown freed the head as if it were parked — leaving that flow's writes standing
       in the shared baseline with nothing left that could ever unapply them. flow_release asserts it now. */
    if (g_sess_cur) { flow_switch_out(g_sess_ctx, g_sess_cur); g_sess_cur = NULL; }
    /* AND THE FRONTIER IS NO LONGER FOR SALE. The reclaim hook pages a member out at the allocator's refusal;
       past this line there is no scheduler to page for, and the teardown that follows allocates (the result
       document, the leak walk) — an allocation there must fail as an allocation, not reach into a frontier that
       is being torn down. */
    reclaim_uninstall(JS_GetRuntime(g_sess_ctx));
    JS_SetJobEnqueueHook(NULL);
    JS_SetJobDropHook(NULL);
    JS_SetFlowControlHooks(&FC_OFF);
    /* THE SESSION'S GENERATION ENDS HERE, and it is the LIVE stamp that is cleared rather than the scheduler's
       saved copy, because this runs INSIDE the slice: the bracket below saves whatever the slice leaves, so
       clearing it here is what makes the saved copy zero too. A session that ended and a host that is merely
       between two slices are then the same statement — no flow is running — said with the same number. */
    JS_SetFlowGen(0);
    /* No flow is running, so no candidate substitution may be installed — the same mirror the switch-in keeps,
       completed at the one point where the answer is "none". Without it the LAST flow to run leaves its
       payload and its endpoint suppression standing over everything that reads the frontier afterwards. */
    solve_flow_begin(NULL);
    /* g_dom_capture is NOT cleared here any more: it is the flow stamp's twin and the SLICE owns both, so a
       second copy of the clear in this function would be a second place that has to agree with the bracket. It
       is off the instant this slice returns, which is also the instant this session's last flow stopped. */
    g_sess_live = 0;
}

/* THE RAM→DISK FLOOR, and the two flags that carry it. The host decides there is pressure — it is the only
   zone that can see the OTHER documents' engines and the summed working set — and asks THIS engine to give up
   its residue; the engine decides WHEN, which is at a scheduler step boundary with no flow switched in,
   because that is the only moment every flow's decision state is in its own blob rather than in decide.c's
   globals. `g_parked` then says, for the rest of the instance's life, that the frontier was WRITTEN OUT
   rather than dropped — which is what the teardown asserts read: a paged flow's continuation, its owed
   replies and its queued jobs are all regenerated by the replay its recipe drives, so none of them is the
   dropped work item those asserts exist to catch. */
static int g_park_req, g_parked;

/* THE GENERATION THE SCHEDULER HOLDS ACROSS THE HOST'S OWN TIME. Not a second source of truth: the live stamp
   is quickjs's g_flow_gen, this is where engine_sched_step puts it down while the host has the thread, and
   JS_FlowGen() is read back at the end of every slice so the forks taken inside it are carried forward.
   Monotonic across slices, which is exactly what the boolean it replaced could not be. */
static uint32_t g_slice_flow_gen;

void engine_request_park(void) {
    DCHECK(g_sess_live, "a park was requested of an engine with no live session — there is no frontier to "
                        "write out, and the host would then store an empty residue over a real one");
    g_park_req = 1;
}

int engine_frontier_paged(void) { return g_parked; }

/* ─────────────────────────────────────────────────────────────────────────────────────────────────────────
   THE PARTIAL SELF-PARK — the engine selling its own tail at the RAM floor, which is the OTHER half of the
 * paragraph above. Level-1 eviction is the host giving up a whole DOCUMENT for a document worth more; this is
 * a LONE engine, which no host will ever ask to park (there is no other document to free the RAM for), meeting
 * the floor on its own and staying alive by paging its least valuable flows.
 *
 * IT IS DRIVEN BY THE ALLOCATOR'S REFUSAL AND BY NOTHING ELSE. There is no watermark, no budget and no
 * high-water mark to compare against: any of those is a number someone picked, and a number that fires early
 * truncates work while memory remains, while a number that fires late is the OOM it was supposed to prevent.
 * The runtime asks (quickjs.h's JSMemoryReclaimFunc) at the moment an allocation cannot be satisfied, and it
 * asks again after each flow this gives up, so the STOPPING CONDITION is the allocation succeeding and the
 * frontier's floor is "there is no member left that is not the running flow". Both ends are physical.
 *
 * THE SAFEPOINT. This runs INSIDE a failing allocation, and what it does — removing a member from the frontier
 * — is unsafe wherever the engine is holding a position in that frontier: a walk over flow_at that allocates
 * would have a member swapped into the hole behind it and would silently skip one, which is a dropped work
 * item and the exact thing §scheduler's razor forbids. So the reclaim is ARMED only across the flow step,
 * which is the region in which the engine holds no such position and in which essentially every allocation of
 * a run happens, and the one frontier walk that runs inside a step disarms it around itself. This is quickjs's
 * version of what V8 spells `DisallowGarbageCollection`: not a fallback (there is no second reclaimer to fall
 * back to) but a declaration of where the operation is legal, and outside it an allocation fails exactly as it
 * did before this existed. */
static int g_reclaim_allowed;

static int engine_reclaim_set(int v) { int prev = g_reclaim_allowed; g_reclaim_allowed = v; return prev; }

/* THE REPLIES THAT DIED WITH A PAGED FLOW. A flow BLOCKED on the host is the cheapest member to page — its
   recipe re-issues the request in the session that resumes it and gets today's answer, which is §Time-travel's
   whole point — so the tail is very often a flow the host is still fetching for. The reply then arrives for a
   URL nobody is parked on any more, and that is not the host mispairing its list: it is a park that happened
   between the ask and the answer. Counted rather than assumed, so the pairing assert stays armed for the case
   it was written for (a host that names a URL no flow ever parked on) and is answered exactly once per reply
   the sale made unnecessary. It is the same event engine_host_answer already drops for a vanished asker; this
   is the fetch side of it, with the difference that the fetch side had an assert to keep honest. */
static long g_paged_owed;

/* HOW MANY FLOWS THIS SESSION ACTUALLY SOLD. The pager's whole claim is that `live` stops tracking `flows`
   under pressure, and until this counter existed that claim had no WRITER: `live < flows` is equally the
   signature of flows FINISHING, and `_park`'s record count is two record kinds (segments and flows) added
   together, so neither of the two numbers a run already printed can answer "did it page". CLAUDE.md's rule is
   that a consumer never defaults a producer's field; the same rule read forwards is that a measurement gets a
   producer before it gets quoted. */
static long g_flows_sold;

int engine_take_paged_owed(void) {
    if (g_paged_owed <= 0)
        return 0;
    g_paged_owed--;
    return 1;
}

static int engine_reclaim_tail(JSRuntime *rt, void *opaque, size_t wanted) {
    Flow *tail;

    (void)opaque; (void)wanted;
    if (!g_reclaim_allowed)
        return 0;
    /* THE HOOK IS INSTALLED WITH THE SESSION AND REMOVED WITH IT, so reaching here without one would mean the
       runtime kept a pointer into a scheduler that no longer has a frontier to sell. */
    DCHECK(g_sess_live, "the runtime asked this engine to page out a flow with no live session — the hook is "
                        "installed by engine_sched_begin and removed by engine_session_close, so this is a "
                        "reclaim against a frontier that is gone");
    DCHECK(rt == JS_GetRuntime(g_sess_ctx),
           "a reclaim was asked of this engine by a runtime that is not its session's — one instance is one "
           "runtime, so paging this frontier would not free the memory the other one is short of");
    /* THE RUNNING FLOW IS THE ONE MEMBER THAT CANNOT GO: its decision state is live in decide.c rather than in
       its blob and its delta is applied to the heap, which is exactly what cold_park_flow and flow_release both
       refuse. Everything else in the frontier is a snapshot and can be written down. */
    tail = flow_worst(flow_running());
    if (!tail)
        return 0;   /* the floor: nothing here but the flow that is running */
    /* AND THE SHARPER STATEMENT, WHICH THE SOLVER'S OWN ALLOCATIONS MADE NECESSARY. "Not the flow the
       scheduler is switched into" is how flow_worst is CONSTRUCTED; what has to be true at this line is that
       the flow being sold is not the flow this allocation is FOR. Since the engine's own allocations now ask
       here (solver/reclaim.h), the caller is very often GROWING the running flow's own state — its delta's
       entry array, its DOM undo log — and holding the old buffer in a local across this call. Selling that
       flow would free the buffer the caller is about to write through, and the corruption would look like a
       COW bug rather than a paging one. The exclusion is the construction; this is the assertion. */
    DCHECK(tail != flow_running(),
           "the pager chose the flow the scheduler is switched into — the allocation that asked for this "
           "memory is usually growing that flow's own delta or undo log, so selling it frees the buffer the "
           "caller is holding and is about to write through");
    /* AND IT IS STILL IN THE REGISTRY — a separate fact with a separate failure. flow_worst walks the members,
       so a non-member here is a stale Flow* the frontier already removed, and cold_park_flow would then write a
       recipe for a flow that does not exist and flow_release would free it a second time. Two facts, two
       asserts: a conjunction reports whichever failed under whichever message was written for the other. */
    DCHECK(flow_is_member(tail),
           "the pager chose a flow that is not in the frontier — flow_worst walks the registry, so this is a "
           "stale pointer, and both the park below it and the release after it would act on freed memory");
    /* WRITE IT DOWN, THEN GIVE ITS RAM BACK — in that order, because the release is what makes the recipe the
       only remaining copy of this flow. cold_park_flow appends one record to the session's park document (the
       host stores it under this bundle's key and the next session MERGES it back in); flow_release unapplies
       nothing that is live, frees the flow's suspended frame chain, its heap delta, its DOM head and the nodes
       it created, and walks the shared heap and document chains down only as far as the segments this flow held
       the last reference on. A tail flow's dying prefix cannot be a segment the RUNNING flow stands on — a
       shared segment has a second reference by definition — so at this size the release moves nothing in the
       live heap or document and is pure free. */
    g_paged_owed += pending_count(tail->pending);
    cold_park_flow(tail);
    flow_release(g_sess_ctx, tail);
    g_flows_sold++;
    return 1;
}

void engine_sched_begin(JSContext *ctx, char **bodies, char **srcs, int n, int forking, const char *recipes) {
    DCHECK(!g_sess_live, "engine_sched_begin while a session is already running — one scheduler, one session");
    g_sess_ctx = ctx; g_sess_bodies = bodies; g_sess_srcs = srcs; g_sess_n = n; g_sess_cur = NULL; g_sess_live = 1;
    g_parked = 0;
    g_paged_owed = 0;   /* replies owed to flows this session paged out — see engine_take_paged_owed */
    g_flows_sold = 0;   /* …and how many flows that was, which is what @PROGRESS's `sold` reports */
    /* WHAT AN UNCANCELLED REJECTION MEANS is this half's answer: the browser half fires the event and honours
       preventDefault, and a reason that survives that is a page error exactly like a script that threw. */
    unhandled_rejection_set_report_hook(result_page_error_value);
    /* THE FRONTIER IS SEEDED ONCE, FROM ONE OF TWO PLACES, and they are alternatives rather than layers. A
       document nobody has explored starts with ONE flow: the page's scripts over an empty decision vector. A
       document with a PARKED RESIDUE starts with that residue — every flow the last session could not finish,
       each standing on the path it had taken — and adding a fresh boot flow beside them would explore the
       un-forked path a second time and re-fork every branch the residue already stands on, growing a duplicate
       of the frontier on every visit. If the boot flow itself was still live at the park, it IS one of the
       records (an empty chain), so nothing is lost by not adding one here.
       CLAUDE.md §scheduler: a new page APPENDS to the ONE continuous cross-session frontier; it does not start
       a new scheduler, and it does not start the same exploration twice.
       THE CLAIM IS MADE HERE, WHERE THE CHOICE IS. It used to be asserted inside cold_resume — "the frontier is
       empty" — which made the rebuild refuse to run over a live frontier and so made a PARTIAL park impossible
       to read back: its tail returns to the flows that were not paged out. Emptiness is a property of STARTING
       a session, not of rebuilding a residue, so it is asserted at the one call that starts one and the rebuild
       is free to be the merge it has to be. */
    DCHECK(flow_count() == 0,
           "a session began over a frontier that already has members — the boot flow and a parked residue are "
           "ALTERNATIVES, so whichever is chosen below would be a second seeding of the same document: the "
           "un-forked path explored twice, and every branch the existing members already stand on re-forked");
    if (recipes && *recipes) cold_resume(ctx, recipes);
    else flow_add(ctx, JS_UNDEFINED, WORLD_NONE);   /* the first flow: the page's scripts, empty decision vector */
    /* THE GENERATION THE FIRST SLICE WILL OPEN AT — written to the scheduler's saved copy and NOT to the live
       stamp, which is the whole point of the move. Setting the live one here would stamp everything created
       between this call and the first step — the host reads qjs_bundle_id, pulls the pending list, provides
       the replies it already had — as belonging to a flow that has not run yet. Both this call and cold_resume
       above it create at baseline, exactly as they did before. */
    g_slice_flow_gen = 1;
    dom_cow_set_ctx(ctx);                   /* the DOM delta needs ctx for the attribute taint-shadow dup/free */
    cow_set_ctx(ctx);                       /* …and the heap delta needs one for the component records it captures */
    JS_SetFlowControlHooks(forking ? &FC_EXPLORE : &FC_VERIFY);   /* preempt ALWAYS on; fork only when exploring */
    JS_SetJobEnqueueHook(engine_enqueue_job);   /* ASYNC-AS-FLOW: reactions route to the enqueuing flow's queue */
    JS_SetJobDropHook(engine_drop_jobs);        /* …and §7.5.10 step 7 takes them back off it */
    /* THE FRONTIER IS THE ENGINE'S RESERVE, and this is what lets an allocator spend it. Installed with the
       session because that is exactly when there is a frontier to page: a refusal asks engine_reclaim_tail,
       which sells the lowest-weight member to the cold tier and answers "retry".
       EVERY ALLOCATOR, NOT THE RUNTIME'S ALONE — which is what solver/reclaim.h is, and it is there because
       measurement said so: with the edge in the runtime's allocator only, the fixture under a 2 GB wall paged
       nothing and aborted inside LEXBOR's, holding 315 MB of frozen document segments against 23 MB of heap
       ones. Most of this engine's memory is the HTML parser's, so most refusals are its. */
    reclaim_install(JS_GetRuntime(ctx), engine_reclaim_tail, NULL);
}

/* One QUANTUM. Returns ENGINE_STEP_DONE when the frontier is empty (the session is closed and its hooks are
   uninstalled) and ENGINE_STEP_YIELD when the slice expired with the frontier intact. The slice is a budget of
   CPU actually consumed (solver/quantum.h says what each host can measure) and it is NOT a cap: nothing is
   dropped, starved, reordered or forgotten across it — the next call resumes the same top flow on the same
   frontier, which is the razor §scheduler states. */
static int (*g_stall_hook)(void);
void engine_set_stall_hook(int (*owed)(void)) { g_stall_hook = owed; }

void engine_set_referenced(int referenced) {
    g_referenced = referenced;
    /* AND EVERY MEMBER IS ASKABLE AGAIN, because this flag is one of the answers. A flow with nothing left to
       run reports host-owed INSTEAD of finishing while this is set (flow_step's last branch), so clearing it
       means those flows can now FINISH — a change to what they would answer, made by the host, naming no flow.
       That is exactly the whole-frontier clear (flow.h). Unconditional rather than on the 1→0 edge: a mark
       cleared a step early costs one step, and a mark kept after the fact it rested on has gone costs the flow
       its ending. */
    flow_clear_host_owed_all();
}

static double g_yield_floor = -1.0 / 0.0;
void engine_set_yield_floor(double w) { g_yield_floor = w; }

double engine_top_weight(void) {
    Flow *b = flow_best();
    return b ? flow_weight(b) : -1.0 / 0.0;
}

static int engine_sched_slice(void) {
    JSContext *ctx = g_sess_ctx;
    char **bodies = g_sess_bodies;
    int n = g_sess_n;
    Flow *cur = g_sess_cur;
    /* ONE READING OF THE SLICE'S OWN MEASURE PER ITERATION, carried. It closes the step just run (the WFQ's
       aging charge) and opens the next one, so the charges TELESCOPE across the whole quantum and no
       quantisation is lost between steps.
       IT IS THE QUANTUM'S MEASURE AND NOT A SECOND CLOCK, which is the correction this line carries. It used to
       be `engine_now_us()` — CLOCK_MONOTONIC — with a paragraph explaining that "this host has no CPU clock" and
       that an ORDER decision can afford a wrong reading. Half of that was true of emscripten and none of it was
       true of the native host, which has had a per-thread CPU clock all along; and §WFQ names the term CPU-AGING
       — "a monopolizer that burns CPU without emitting" — so the currency the aging charge is denominated in has
       to be the one the slice is. quantum_thread_us() is that one quantity, answered per host, and where the
       host genuinely has no CPU clock quantum_measure() says so instead of this comment claiming it.
       WHAT TELESCOPING PUTS ON THE WRONG BILL, said plainly rather than left to be discovered: the context
       switch and the finish that happen BETWEEN two readings are charged to the flow stepped next, not to the
       one they were performed for. That is one COW delta swap of misattribution, it is bounded by the swap this
       scheduler already counts, and buying it back would cost a second reading per iteration for an ORDER
       decision that a notch of quantisation already absorbs. Charging nothing at all — which is what a
       release build did — is the error that matters here, not which of two flows pays for a swap. */
    int64_t now = quantum_thread_us();
    /* THE SESSION THE HOST STEPPED IS STILL OPEN — and the way this fires is a CALLER THAT TRANSFORMED THE
       PREVIOUS ANSWER. Two exits close the session and both answer ENGINE_STEP_DONE: the frontier draining, and
       the PARK that writes the residue to the cold tier. A wrapper that folded DONE into YIELD — the way the ABI
       entry legitimately folds STALLED, because the bridge speaks two values — would send the host straight back
       in here with nothing live, and the abort would read as a bug in whatever the park had just done rather
       than in the fold. So DONE is propagated unchanged by every layer above this (engine_sched_step brackets
       the slice and returns it verbatim; qjs_step latches it in g_done and answers DONE to every later step). */
    DCHECK(g_sess_live,
           "a slice was opened on a session that is no longer live — the previous step answered "
           "ENGINE_STEP_DONE and the host stepped again anyway");
    /* THE FLOW CARRIED ACROSS A QUANTUM BOUNDARY IS STILL A MEMBER OF THE FRONTIER. §scheduler's razor says the
       cooperative yield resumes "the SAME top flow on the byte-identical frontier", and the whole of what makes
       that true here is a raw `Flow *` held in a static across a RETURN TO THE HOST — during which the host
       routes records, provides replies and answers requests. If any of those ever removed a flow, this pointer
       would be read (`best != cur`, then flow_switch_out) after its free, and the corruption would look like a
       scheduling bug rather than a lifetime one. Asserted where the pointer is picked back up, which is the one
       place the claim is made. */
    DCHECK(cur == NULL || flow_is_member(cur),
           "the flow this quantum resumes is no longer in the frontier — it was removed while the scheduler was "
           "returned to the host, so the resume is neither the same flow nor a byte-identical frontier");
    /* THE PARK IS TAKEN HERE — BEFORE the first pick, and that position is the mechanism rather than a
       convenience. Every flow's decision state has to be in its OWN blob for the walk to read it, and the one
       flow whose state is NOT is whichever the scheduler is holding: decide.c keeps the running flow's
       evolving vector in its globals. So the running flow is switched out first — the ordinary suspend, the
       same one a context switch performs — and from that instant the frontier is a set of snapshots, which is
       exactly what a park needs and exactly what §Time-travel-resume says a parked flow is.
       IT IS NOT A DROP AND NOT A BOUND. Nothing is truncated, skipped or forgotten: every member is written,
       in full, and the host stores the document under this bundle's key. The flows themselves are released by
       the instance teardown that follows, which is what frees the RAM the host asked for — Level-1 eviction is
       a whole document's engine leaving memory, and its residue coming back is the SAME admission step, ranked
       by the same one order. */
    if (g_park_req) {
        if (cur) { flow_switch_out(ctx, cur); cur = NULL; }
        cold_park();
        g_park_req = 0;
        g_parked = 1;
        g_sess_cur = NULL;
        engine_session_close();
        return ENGINE_STEP_DONE;
    }
    /* NOTHING IS RE-ADMITTED HERE, AND THE LINE THAT DID IT IS DELETED. A `flow_clear_host_owed()` stood at the
       top of every slice, reasoned as "between two slices the HOST ran". That is true of a slice that ended
       because the engine had nothing left to do — and FALSE of one that ended on its CPU QUANTUM, which is
       thread-sharing and hands the thread to a host with nothing to answer. Since the quantum is the exit a
       busy slice always takes, the mark was being laundered by the one exit that means nothing.
       WHAT IT COST, MEASURED: a document whose entire frontier was blocked on the host (512 of 512) marked the
       ~59 flows one 12 ms slice had time for, was cut short, and re-admitted all of them at the next slice. The
       sweep could never reach the end of the frontier, so "every member is waiting" — the STALL — was
       unreachable BY CONSTRUCTION, and the engine swapped COW deltas 1.76 MILLION times with not one flow
       finishing, not one candidate found and the heap unchanged. The marks now live until the HOST does
       something that could have answered them, and each of those events clears the flow it reached
       (engine_provide, engine_host_answer, engine_deliver, engine_perform, and the shared document-script slot
       inside flow_drain_pending). */
    for (;;) {
        /* WFQ: highest value-of-information among the members that can still make progress — a fresh fork
           (UCB) can preempt. The filter is the ONE order with the flows that have told the scheduler they are
           waiting on the host taken out of the PICK only; they keep their weight, their place and every work
           item they hold (flow.h).
           THE FLOW THAT HOLDS THE THREAD IS PASSED IN, and that is what makes this a SCHEDULE rather than a
           ranking read out loud. Asked without it, the pick returned the first maximum in registry order, so a
           field of equal-weight members — which is most frontiers, since most flows have emitted the same
           amount and usually none — moved the thread to g_flows[0] on every iteration and paid a COW delta swap
           for a ranking that had not changed. The incumbent is displaced only by a STRICTLY better flow, which
           is the same comparison the preempt hook makes, so the two ends of the decision cannot disagree. */
        Flow *best = flow_next_to_run(cur);
        /* NOTHING LEFT TO RUN: either the frontier is empty, or every member is waiting on the host — which is
           the STALL, decided by asking each member rather than by counting a run of unproductive picks. */
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
        {
            /* AGING IS CHARGED IN THREAD TIME, not in steps, and the difference is what a step MEANS.
               `flow_age_running(1)` was written when a step was a whole drain — a long, roughly comparable
               chunk of work — so one unit per step approximated CPU. A step is now ONE unit of work, so the
               same charge bills a flow the same amount for twelve milliseconds of execution as for advancing a
               single script index. §scheduler says the term demotes "a monopolizer that burns CPU without
               emitting", so it is time that must be measured, and in the SAME currency as the reward it is
               subtracted from (flow.c's FLOW_AGE_RATE: one emitted finding per second held without emitting).
               THE READING IS UNCONDITIONAL, and it was the one thing wrong with the paragraph that used to
               stand here. It said "the step is already timed for the seam assertion; this is that number" while
               the only clock read on this path sat inside `#if APICLIENT_DEV` — so a release build charged the
               constant 1 per step and had no aging policy at all. CLAUDE.md's release exemption covers DCHECKs,
               which assert; it never covers the engine's BEHAVIOUR, and a WFQ that is fair only in development
               is not the one policy §scheduler requires. It costs nothing: the reading is carried in `now`, so
               this loop performs exactly the readings its slice check always did.
               IT IS THE QUANTUM'S OWN MEASURE (solver/quantum.h), which is CPU actually consumed wherever the
               host has a CPU clock — natively CLOCK_THREAD_CPUTIME_ID, the same clock the slice's timer is armed
               on. The paragraph that stood here said flatly that "this host has no CPU clock", which was a claim
               about emscripten written into a file that also compiles for Linux, and it was the licence for
               charging aging in wall time. Where it IS true — the extension's wasm instance, whose every WASI
               clock is emscripten_get_now() — the fallback is the same wall clock as before and quantum_measure()
               names it, so nothing here has to claim anything. On that host a descheduled step is charged for
               time it did not burn, so a flow is demoted one notch early and a sibling runs sooner; nothing is
               dropped, truncated, skipped or reordered out of the frontier, which is the only thing the WFQ is
               allowed to be and the razor §scheduler states. The seam assertion below may not take that trade,
               because it reaches a VERDICT and aborts — which is why it decides on WORK and not on any clock.
               WORK-DONE WOULD BE THE WRONG QUANTITY HERE even though it is the right one below: a flow that
               forks nothing, queues nothing and emits nothing — a tight compute loop over opaque input — burns
               the thread while its share of engine_work_done() stays at zero, so aging by work would never
               demote the very shape this term exists for. */
            int64_t t0 = now;   /* this step's start: the previous iteration's reading, carried */
#if APICLIENT_DEV
            uint64_t pq0 = 0, pf0 = 0, pa0 = g_preempt_asked;
            /* THE WORK THIS STEP PERFORMS, sampled beside the clocks and for a reason no clock can serve —
               see the verdict below. Forks, flows and jobs are things the engine DID; no amount of being
               descheduled can inflate them. */
            long w0 = engine_work_done();
            /* AND THE WALL CLOCK, READ SEPARATELY BECAUSE IT MEASURES A DIFFERENT THING. `now` is the slice's
               measure (CPU, where the host has one); the gap census and the step duration below are WALL
               quantities the seam message REPORTS and never decides on, so mixing the two units into one
               subtraction would produce a "wall gap" that is neither. Two numbers, two clocks, one verdict —
               and the verdict is neither of them. */
            int64_t wall0_ms = engine_now_ms();
            JS_FlowPreemptStats(&pq0, &pf0);
            g_max_gap = 0; g_last_ask = wall0_ms;   /* this step's gaps, measured from the moment it starts */
            idl_slowest_reset();              /* ...and this step's slowest single Web API member step */
#endif
            /* THE RECLAIM SAFEPOINT, and it is this ONE region for a structural reason rather than a
               performance one: across the step the engine holds no position in its own frontier, so a member
               removed by the allocator's refusal edge cannot be removed out from under a walk. It is also where
               essentially every allocation of a run happens — the page's own objects, its frames, its deltas —
               so arming it here is what makes "the engine pages its tail at the RAM floor" true in general
               rather than at one hand-picked call site. Restored rather than cleared, because the step can
               reach this loop again through a nested driver. */
            int prev_reclaim = engine_reclaim_set(1);
            int r = flow_step(ctx, cur, bodies, n);
            engine_reclaim_set(prev_reclaim);
            /* THE CHARGE, AND IT IS CHARGED TO THE FLOW THAT RAN. `flow_age_running` bills whoever the registry
               says is running, and that is only the flow this step advanced while nothing between the switch-in
               above and here has changed it — a step that returned with a different flow running would bill the
               wrong one, and the symptom would be a monopolizer that never ages and a sibling demoted for time
               it never spent. The finish path clears it, which is why this stands BEFORE the r == DONE branch. */
            DCHECK(flow_running() == cur,
                   "the flow the scheduler stepped is not the one it is about to charge for that step's thread "
                   "time — the aging term would demote a flow that did not burn it and leave the one that did "
                   "holding its rank");
            now = quantum_thread_us();
            flow_age_running(now - t0);
            /* THE COOPERATIVE-QUANTUM CONTRACT, ASSERTED AT ITS SITE. A flow_step is supposed to reach a
               suspend point — a bytecode back-edge where the preempt hook runs, a step machine's boundary —
               within the quantum, which is what makes the frontier parkable at all. A path with NO suspend
               point on it does not slow the run down, it STOPS it: the slice's expiry below is never observed,
               the scheduler never returns, and the whole engine spins at 100% with the switch count frozen.
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
                /* THE WALL CLOCK'S OTHER END, taken once, for the two REPORTED numbers below. It is deliberately
                   a different reading from the aging charge above: that one is the slice's measure and this one
                   is elapsed time, and a message that printed one of them under the other's name is exactly the
                   confusion §Testing's four false reds came from. */
                int64_t done = engine_now_ms();
                int64_t spent = done - wall0_ms;
                (void)spent;
                /* the TAIL closes the last gap: a step that offers a point and then runs for seconds before
                   returning has that silence between its last offer and its end, and nothing else records it. */
                int64_t gap = done - g_last_ask > g_max_gap ? done - g_last_ask : g_max_gap;
                /* THE VERDICT IS WORK, NOT TIME — and the clock is now only something the message PRINTS.
                   `asked == 0` says the path offered the scheduler no suspend point, which is the missing seam
                   this exists to catch. It was ANDed with a wall-clock gap, and a wall clock cannot tell the two
                   states apart that matter here:
                     - a stretch of C that genuinely has no seam, which spins forever with the switch count
                       frozen and is the real defect, and
                     - a stretch that was simply DESCHEDULED because something else on the box had the CPU.
                   A descheduled thread executes no instructions, so it reaches no preempt check either: it shows
                   `asked=0` for exactly the same reason, and the wall gap it accumulates is not its own. That is
                   not hypothetical — three concurrent builds made this fire at 14486 ms with `points asked=0` on
                   a tree whose seam was intact, and the same commit run alone passed; the resulting diagnosis
                   was published and had to be retracted. A false failure is worse than a missing one, because it
                   sends the next reader hunting a phantom in whichever component happened to be under test.
                   CPU TIME IS THE RIGHT MEASURE AND ONE OF THE TWO HOSTS HAS IT — see the second verdict below.
                   This paragraph used to say flatly that "this host does not have one", which was true of the
                   extension's wasm instance (emscripten's WASI clock_time_get answers CLOCK_MONOTONIC and both
                   CPUTIME clocks from emscripten_get_now(), i.e. wall time) and false of the native build, which
                   this same file compiles for. solver/quantum.h answers it per host instead of a comment
                   claiming it for both, and quantum_measure_is_cpu() is what the CPU verdict is gated on so
                   there is never a wall clock inside something calling itself CPU.
                   WHAT IS MEASURED HERE IS WHAT THE STEP DID. Forks, flows created and jobs run are work the
                   engine PERFORMED; being descheduled cannot inflate any of them, because a thread that is not
                   running performs none. A step that mints thousands of siblings without once consulting the
                   preempt hook has no seam on that path whatever the machine's load — that is the invariant,
                   stated in the quantity it is actually about. It truncates nothing and drops no flow: the
                   engine that reaches this margin is already the one that never returns.
                   THE SHAPE THAT REACHES IT is real and is in the tree today: an unknown-length walk over
                   opaque input asks one outcome-fork question per position and the driver re-enters the same
                   step immediately after each fork, so one scheduler step mints an unbounded chain of siblings
                   and never comes back to be ranked.
                   THE ONE CASE THIS CANNOT SEE is a seamless stretch that also does no observable work — a bare
                   `for(;;);` inside C, which forks nothing, queues nothing and emits nothing. That is what the
                   SECOND verdict below is for, and it exists only because the native host now has a real thread
                   CPU clock: consumed CPU cannot be inflated by load, so it separates a seamless stretch from a
                   descheduled one where a wall clock never could. It is a SEPARATE verdict with its own message
                   deliberately — §Testing: two measures must never collapse into one answer, or a reader cannot
                   tell which one decided. */
                long work = engine_work_done() - w0;
                /* THE CPU VERDICT — the seamless stretch that does no observable work, which the work verdict
                   below is blind to by construction. Gated on the host actually having a CPU clock rather than
                   assumed: on emscripten this quantity is wall time, a loaded machine would falsify it, and a
                   false red is worse than a missing one (§Testing). Its own message, its own margin, so the two
                   answers are never confused for each other. */
                if (quantum_measure_is_cpu() && (now - t0) > ENGINE_SEAMLESS_CPU_US && g_preempt_asked == pa0) {
                    char why[256];
                    snprintf(why, sizeof why,
                             "%d ms of THREAD CPU consumed in one step with NO suspend point offered (measure: "
                             "%s; %ld units of work, which is why the work verdict cannot see this one) — this "
                             "stretch has no suspend/resume seam; unit=%s script_i=%d",
                             (int)((now - t0) / 1000), quantum_measure(), work, g_step_unit,
                             cur ? cur->script_i : -1);
                    DFAIL(why);
                }
                if (work > ENGINE_SEAMLESS_WORK && g_preempt_asked == pa0) {
                    /* Sized for the whole line INCLUDING quantum_measure(), which on the host that has no CPU
                       clock is a sentence rather than a word — it names the transport requirement, and a buffer
                       that truncated it would drop the payload and the body, which are the two fields that
                       localise the stretch. */
                    char why[640];
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
                    long wrap_n = 0, wrap_cap = 0;
                    long steps_n = 0;
                    int64_t slow_ms = idl_slowest_step(&slow_name);
                    JSMemoryUsage mem;
                    int64_t steps_ms = idl_step_total(&steps_n);
                    JS_FlowPreemptStats(&pq, &pf);
                    if (g_wrap_stats) g_wrap_stats(&wrap_n, &wrap_cap);
                    /* THE LIVE HEAP, because a garbage collection is the one thing reachable from an ordinary
                       Web API call whose cost is the size of everything ELSE. createElement allocates — a
                       wrapper, a shape — so it can trigger a collection, and a collection marks the whole live
                       set. That makes ONE call arbitrarily slow while every other call of the same member is
                       fast. The wrapper map's size sits beside it deliberately: the two together say whether
                       the live set is the DOM the flows built or something else entirely, which is the
                       difference between a leak this fixes and a leak still to find. */
                    JS_ComputeMemoryUsage(JS_GetRuntime(ctx), &mem);
                    /* THREE numbers, because two of them cannot separate the roots. `asked` is how many suspend
                       points the path OFFERED (every consultation of the hook); `requested` is how many of those
                       the WFQ wanted to take; `fired` is how many actually parked. asked==0 means the path has
                       no suspend point on it at all — the seam is missing and must be built. asked>0 with
                       requested==0 means the points were there and the scheduler declined every one, which is a
                       ranking question, not a missing seam. requested>fired means a point was reached, the
                       preempt was wanted, and it was DROPPED because no driver at that depth adopts the seam. */
                    /* BOTH MEASURES ARE PRINTED, the work one that decided it and the wall clock that no
                       longer does, so the next reader can see at a glance which state they are in: a big work
                       count with a small gap is a real seamless stretch on an idle box, and a big gap with the
                       same work count says the box was also loaded — neither changes the verdict, and that is
                       the point. */
                    wi += snprintf(why, sizeof why,
                                   "%ld units of work (forks+flows+jobs) with NO suspend point offered "
                                   "(wall gap %d ms, step ran %d ms — reported, NOT the verdict; the slice's own "
                                   "measure here is %s; points asked=%llu, preempts wanted=%llu fired=%llu; slowest "
                                   "Web API member step: %s %dms of %dms over %ld member steps; wrapper map "
                                   "%ld/%ld; live objects %lld, heap %lld KiB) — this stretch has no "
                                   "suspend/resume seam; unit=%s script_i=%d "
                                   "flow=%s payload=",
                                   work, (int)gap, (int)spent, quantum_measure(),
                                   (unsigned long long)(g_preempt_asked - pa0),
                                   (unsigned long long)(pq - pq0),
                                   (unsigned long long)(pf - pf0), slow_name, (int)slow_ms,
                                   (int)steps_ms, steps_n, wrap_n, wrap_cap,
                                   (long long)mem.obj_count, (long long)(mem.malloc_size / 1024),
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
            if (r == FLOW_STEP_DONE) { solve_flow_end(cur); flow_finish(ctx, cur); cur = NULL; }
            else if (r == FLOW_STEP_OWED) {
                /* THIS FLOW CAN MAKE NO PROGRESS UNTIL THE HOST ANSWERS, and the scheduler records that ON THE
                   FLOW. It is NOT skipped and NOT removed — it stays in the WFQ at its own weight with every
                   work item it holds, and it is picked again at the top of the next slice — which is the first
                   moment anything CAN have answered it, since only the host can.
                   IT USED TO BE A COUNT of consecutive owed answers, broken at `flow_count()` on the claim that
                   "every member has answered that in a row". The claim was false: this loop re-picks the SAME
                   top-ranked flow, because an owed step burns microseconds and so moves neither its service
                   notch nor its weight. N owed answers were therefore N answers from ONE flow, and the break
                   declared the whole frontier stalled while runnable siblings had never been asked — which in
                   the smoke host ends the run over them (the provider answers nothing and run_scheduler
                   breaks). A no-progress count is in §NO-BOUNDS' own list, and this is why. */
                flow_set_host_owed(cur);
            }
            /* AND NOTHING IS CLEARED BY PROGRESS, which is a statement about what a slice can do rather than an
               omission. No step of any flow can answer another flow's wait: every kind on the register — a
               fetch reply, an external script's text, a synchronous request's answer, a routed record, a
               cross-agent operation — is filled by the HOST, and the host does not run until this slice
               returns. Clearing here instead would re-admit every marked flow after every step, and with a
               marked flow outranking a working one that is a wasted step and two COW swaps per unit of real
               progress. */
        }
        /* THREAD-SHARING, not value: the slice's budget is gone, so hand the thread back and keep the frontier.
           It is the SAME question preempt_hook asks — one budget, one edge, asked at the two levels it acts on:
           there the running flow parks, here the step returns. Asking it twice from one source is what makes a
           flow that parked on the quantum and a step that ends on it the same event rather than two policies
           that have to be kept in step. */
        if (quantum_expired()) {
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
    /* THE STALL CLAIM, CHECKED OF EVERY MEMBER — the other half of the host-owed mark, and the half that would
       catch the OPPOSITE failure from the one just fixed. flow_set_host_owed asserts that a mark is never
       cleared by something that cannot have answered it; nothing asserted that a mark is never LAID DOWN on a
       flow that could still have progressed. That failure is silent in the other direction: the engine stops
       exploring early and reports a frontier "waiting on the host" that the host owes nothing, which from
       outside is indistinguishable from a document that really is waiting.
       Reaching this line means the pick found no runnable member, so every member is marked; each of them must
       therefore hold something ONLY THE HOST can supply. That is an entry on its register — a fetch reply, a
       document script's text, an answer to a synchronous request — or the one case with no register entry at
       all: a document a peer still holds a reference into, whose last timeline reports host-owed INSTEAD of
       finishing (engine_set_referenced), and which is waiting for an operation rather than for a reply.
       IT IS ALSO WHERE "how many requests may one stall answer" IS DECIDED, and the answer is ALL of them:
       engine_host_requests joins every outstanding record into a buffer that grows (no truncation, no cap) and
       every host walks the whole list. So a frontier that stays blocked across a stall is not a seam that
       answers one at a time — it is this set being REGENERATED by forking, since a fork re-issues its parent's
       unanswered synchronous request under the sibling's own world (engine_fork_finalize). */
    for (int i = 0; i < flow_count(); i++) {
        DCHECK(pending_count(flow_at(i)->pending) > 0 || g_referenced,
               "the frontier reported a STALL while one of its members owed the host NOTHING — its mark says it "
               "cannot progress and its register is empty, so it was marked while it still had work to do and "
               "the exploration of that timeline stops here for no reason at all");
    }
    /* STALLED, not exhausted: the run-queue is empty but flows are parked on something only the host can
       supply. Ask the one seam BEFORE closing — the session and every parked snapshot stay live, and the host
       steps again once it has provided. */
    if (g_stall_hook && g_stall_hook())
        return ENGINE_STEP_STALLED;
    /* The exploration found sinks; each breakout is a FLOW on this same frontier, seeded once the exploring
       flows are done so a candidate never re-fires against a half-explored page. Seeding adds members, so the
       loop above has more to do — hence before the exhausted answer, not after it.
       ASKED EVERY TIME THE FRONTIER DRAINS, not once. A sink inside a lazily-imported chunk or an injected
       <script src> is discovered AFTER the first drain, because the code holding it had not arrived yet; a
       one-shot latch left every such sink unsearched, which bounded verification by when a sink was found. The
       seeding is per-sink and idempotent, so asking again costs a scan and adds only what is new.
       AND IT IS ASKED AFTER THE STALL IS REPORTED, WHICH IS THE CORRECTION. The sentence above says "seeded
       once the exploring flows are done", and it stood ABOVE the stall report, where that sentence is FALSE:
       reaching this line means the pick found no runnable member, so the exploring flows are not DONE, they are
       PARKED ON THE HOST — every one of them mid-script with a suspended frame and an unanswered entry on its
       register. Seeding there turned the one moment the whole frontier was waiting for a reply into a YIELD
       carrying fresh from-baseline work, so the host was never told what it was holding up, and the flows it
       was holding up went to the back of a queue that had just grown by a full re-exploration of the document
       PER SINK.
       MEASURED, and it is the entire shape of the smoke fixture's frontier: that run reports `candidates 27`,
       and 27 is exactly the number of flows that took this seam. Its fork histogram's FIRST decision site is
       entered by 28 flows — one boot flow and those 27 candidate sessions — and every later site doubles it
       (28, 56, 112, 224, 414, 634, 1169, 2281, 4514). Nine decisions, 9432 flows, and 9404 of them exist
       because a candidate was seeded instead of a reply being asked for. Not one flow ever finished, and the
       flows that were parked at the instant of the seeding were still parked when the run was stopped.
       A candidate re-firing against a page whose every timeline is suspended mid-script IS the half-explored
       page the paragraph above forbids, arrived at from the other side: the page is not half-explored because
       a chunk has not loaded, it is half-explored because the host has not been asked. */
    /* AND THAT ORDER IS ASSERTED — asked of the FLOWS, which is what makes it a check rather than the `if`
       above read out loud a second time. The stall report is the HOST's view (its two registers are empty);
       this is the FRONTIER's (no member is parked on a synchronous request), and the two are only ever the same
       answer if every blocked flow's entry really is on the register the host was shown. It fires on the tree
       this replaces — where 500-odd members were suspended mid-script at exactly this line — and it fires again
       on any future edit that puts engine-side work between a fully host-owed frontier and the one report that
       tells the host so, which is a change nothing else can see: the run looks livelier afterwards, because a
       YIELD always has something to run, and the parked flows simply never come back.
       IT IS THE SYNCHRONOUS HALF SPECIFICALLY, because that is the half that suspends a flow MID-FRAME: a flow
       waiting on a fetch reply has finished its script and is holding a promise, while a flow that is `blocked`
       is stopped inside the call that asked, and re-firing a candidate against a document whose timelines are
       stopped there is re-firing it against a page that never finished loading. */
    for (int i = 0; i < flow_count(); i++)
        DCHECK(!flow_blocked(flow_at(i)),
               "a candidate is about to be seeded against a page whose timelines are PARKED ON THE HOST — this "
               "member is suspended inside the read that asked, its answer is a work item nobody has been told "
               "about, and the flow this seeds re-runs the document from the baseline in front of it");
    if (solve_seed_candidates(ctx) && flow_best())
        return ENGINE_STEP_YIELD;
    /* AND THE OTHER REASON A FRONTIER IS NOT EXHAUSTED, which is not a reply the host owes but a QUESTION it
       has not yet been asked: a document another instance holds a reference into can be asked something at any
       moment, so its timelines are parked (flow_step returns OWED for them) rather than finished, and the
       session stays live with every snapshot intact. Closing here instead would leave the next operation with
       no timeline to answer in — which engine_perform's own assert names, from the other end. */
    if (g_referenced)
        return ENGINE_STEP_STALLED;
    /* ASYNC-AS-FLOW forcing function: every flow has run to completion, so NO microtask/promise reaction may
       still be queued. If one is, the scheduler DROPPED it — the not-yet-built async-as-flow capability (a
       reaction must become a first-class scheduler flow carrying the queuing flow's COW, which needs a fork
       job-enqueue hook). Crash LOUD here rather than silently drop it, so the gap cannot hide. */
    DCHECK(!JS_IsJobPending(JS_GetRuntime(ctx)),
           "async: a job reached the global list (enqueued outside a flow) but was never drained");
    /* THE SAME RULE ONE LEVEL UP, over the FLOWS' OWN queues. flow_step asserts that a flow may not FINISH
       holding work, and that covers the flow that runs out of work — but the loop above can also LEAVE with a
       flow still alive: every member has reported itself host-owed, so no member can be picked, and this line
       closes the session over the survivors. A reaction still on one of their queues is dropped exactly as it
       would be at finish, and only the finish case was being checked, so the wider one was invisible. */
    for (int i = 0; i < flow_count(); i++) {
        DCHECK(flow_at(i)->njob == 0,
               "the frontier was declared exhausted while a live flow still held queued jobs — its promise "
               "reactions, timer callbacks and delivered messages die with the session");
        /* AND THE SAME RULE FOR A ROUTED RECORD, which is a work item exactly as a job is. flow_finish asserts
           it for the flow that RUNS OUT of work; a flow that leaves this loop alive (every member host-owed, so
           none is pickable) had nothing checking it, and the record then dies in flow_registry_free — the
           peer's message, dropped, indistinguishable from a page that registered no handler. A flow suspended
           inside a live frame is the shape that reaches here holding one: the delivery is made only where
           flow_step has no frame, so if this fires, the enqueue belongs earlier than that branch. */
        DCHECK(flow_at(i)->perform == NULL && flow_at(i)->answer_token == NULL,
               "the frontier was declared exhausted while a live flow still owed a peer the answer to a "
               "cross-agent operation — the asking flow, in another instance, is suspended at the line that "
               "asked and this session is about to end without ever telling it anything");
        DCHECK(flow_at(i)->deliver == NULL,
               "the frontier was declared exhausted while a live flow still held a routed record — a peer's "
               "message this document never received, dropped with the session");
    }
    engine_session_close();
    return ENGINE_STEP_DONE;
}

/* THE SLICE'S BRACKET, and it is a WRAPPER because the body has seven exits. Opening the budget is arming an
   asynchronous edge (solver/quantum.h) and closing it is disarming that edge, so a return that forgot one would
   leave a CPU timer running over the host's own time between two steps — the signal would land while the host
   pumps its port, raise a request nothing is there to answer, and expire the NEXT slice at its first opcode.
   Seven `quantum_end()` calls before seven returns is the shape where one of them is eventually missing; one
   bracket around one call is the shape where it cannot be. */
int engine_sched_step(void) {
    int r;
    quantum_begin();
    /* THE FLOW STAMP AND THE DOM CAPTURE ARE THE SLICE'S, NOT THE SESSION'S — and that is one correction, not
       two, because they are twins over the two halves of the same state (the JS heap and the Lexbor tree).
       They were set in engine_sched_begin and cleared in engine_session_close, which reads like a session-
       scoped flag and IS one — but engine_sched_slice returns YIELD without switching the running flow out, so
       "the session is live" and "a flow is running" are not the same statement and the gap between them is the
       host's own time. Everything the host created there (the ABI's parse of a fetch reply, and then a dup of
       it onto EVERY parked flow's pending register) was stamped with the live generation, so
       `JS_ObjFlowGen(obj) > d->fork_gen` skipped it in every delta forked earlier: a write to that shared
       object by any of those flows was recorded nowhere and survived that flow's unapply. Silent, and
       refcounting kept it from ever crashing.
       IT IS THE WRAPPER THAT MAKES THIS SYMMETRIC, which is the same reason the wrapper exists for the
       quantum: the body has seven exits (three value yields, the quantum yield, STALLED, the seeded-candidate
       yield, and the exhausted path that closes the session), and a mark cleared at six of them would look
       correct forever. One bracket around one call cannot be got wrong.
       AND IT IS SUSPEND/RESTORE, NEVER CLEAR/RE-ENTER. The generation is monotonic — every delta's fork_gen is
       a point on that one line — so the next slice resumes at the number this one left, and a restart at 1
       would make a later object compare as older than an earlier fork. */
    JS_SetFlowGen(g_slice_flow_gen);
    g_dom_capture = 1;
    r = engine_sched_slice();
    g_slice_flow_gen = JS_FlowGen();   /* the forks this slice took moved it; carry them, never restart */
    JS_SetFlowGen(0);                  /* the host's own time is baseline: what it creates is shared by construction */
    g_dom_capture = 0;
    quantum_end();
    return r;
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

/* THE C ALLOCATOR'S OWN TWO NUMBERS — see the @HEAP line for why they are here. `live` is what it has handed
   out and not been given back (quickjs's bytes included, since js_malloc routes to malloc); `arena` is the
   total space it has taken from the system, which in wasm is LINEAR MEMORY AND ONLY EVER GROWS. It is one
   allocator interface with two spellings: emscripten's dlmalloc exports the classic `mallinfo` with size_t
   fields, glibc's `mallinfo` is int-wide and deprecated in favour of the size_t `mallinfo2`. Picking the
   spelling is an ABI question about the platform's malloc, not a choice about what to measure — both answer
   the same two fields, and the int-wide one would silently wrap on a run this size. */
#if defined(__EMSCRIPTEN__)
#define ENGINE_MALLINFO_T struct mallinfo
#define ENGINE_MALLINFO() mallinfo()
#else
#define ENGINE_MALLINFO_T struct mallinfo2
#define ENGINE_MALLINFO() mallinfo2()
#endif

static size_t engine_c_alloc_live(void)
{
    ENGINE_MALLINFO_T m = ENGINE_MALLINFO();
    return (size_t)m.uordblks;
}

static size_t engine_c_alloc_arena(void)
{
    ENGINE_MALLINFO_T m = ENGINE_MALLINFO();
    return (size_t)m.arena + (size_t)m.hblkhd;   /* brk'd space plus whatever came from mmap */
}

static int (*g_provider)(JSContext *ctx);
void engine_set_provider(int (*provide)(JSContext *ctx)) { g_provider = provide; }

static void run_scheduler(JSContext *ctx, char **bodies, char **srcs, int n, int forking) {
    int next = ENGINE_PROGRESS_EVERY, last_cands = -1, r;
    /* NO PARKED RESIDUE: this driver is a host with nothing else to do and no store to have kept one in — it
       runs a document to exhaustion in one call. The cold tier's resume belongs to the host that has an
       IndexedDB to have written the residue to, which is the extension's. */
    engine_sched_begin(ctx, bodies, srcs, n, forking, NULL);
    for (;;) {
        int filled;
        r = engine_sched_step();
        if (r == ENGINE_STEP_DONE)
            break;
        /* THE HOST OWES A REPLY, so this is where it pays. Without this seam the loop had nowhere to answer a
           stall from and a request the trusted host must make simply ended the run: every flow that fetched
           stopped at its fetch, and its continuation — the part that reads the reply — never ran at all.
           IT PAYS AT EVERY SLICE BOUNDARY, WHICH IS WHAT A SLICE BOUNDARY IS FOR, and paying only on STALLED —
           which is what stood here — was a SECOND host policy beside the one this project ships. main.c folds
           STALLED into YIELD deliberately ("the bridge speaks two values") and the extension's bridge pulls
           qjs_pending and qjs_host_requests after EVERY return, so in the product a blocked flow is answered at
           the next quantum. Here it was answered only once every OTHER flow in the document had also stopped:
           the run-queue had to empty first, so flow A's reply was a function of flows B..Z's states, which is a
           cross-flow coupling the scheduler forbids everywhere else. On a document that forks faster than its
           flows block, that gate is not merely late — it is never reached. And "a host with nothing else to do
           between quanta" is the whole reason this one may not defer: there is no other work for it to be
           doing instead. */
        filled = g_provider ? g_provider(ctx) : 0;
        /* NOBODY CAN SUPPLY IT, so this driver stops driving — and STOPPING IS NOT THE SAME AS THE SESSION
           ENDING, which is why the close is below rather than here. The frontier that remains is real (every
           flow parked on a reply that never came), and it is handed to the teardown as SNAPSHOTS. The question
           is unchanged and it is still asked only of a STALL: a slice that ended on its quantum still has
           runnable flows, so a provider with nothing to fill THERE means nothing was outstanding, never that
           nothing can be supplied. */
        if (r == ENGINE_STEP_STALLED && filled == 0)
            break;
        /* AND THE PAYMENT WAS COMPLETE, asserted at the seam instead of inferred from a census line six minutes
           later. This provider answers out of its OWN tables — a reply record it builds, a peer's answer it
           stands in for — so unlike the extension's it has no asynchronous half and nothing it may legitimately
           still owe once it has run. A record outstanding here is therefore one it was never handed or one it
           walked past. `blocked` and `owed` cannot say which: they report the LEVEL of unanswered work, and a
           frontier that keeps issuing requests reads identically whether the host is paying promptly or has
           silently skipped a record. This says which, at the moment it happens, and it names the two registers
           separately because they are two questions. */
        DCHECK(!g_provider || *engine_host_requests() == '\0',
               "the smoke host paid and a SYNCHRONOUS request is still outstanding — this provider answers "
               "every record it is handed out of its own tables, so the flow blocked on this one is parked at "
               "a call site nothing is going to resume, and its whole timeline is lost with nothing but a "
               "`blocked` count to say which record it was");
        DCHECK(!g_provider || *engine_pending_urls() == '\0',
               "the smoke host paid and a reply is still owed — the same silence one register over: the flow "
               "that issued this fetch keeps its snapshot and its continuation and is never handed the body, "
               "so everything the page does behind that reply is missing from the run");
        /* Either enough work has happened to be worth a line, or the SEARCH grew — a new candidate is the event
           that changes what the rest of the run will cost, so it is worth saying when it happens. */
        if (engine_work_done() >= next || solve_candidate_count() != last_cands) {
            while (engine_work_done() >= next) next += ENGINE_PROGRESS_EVERY;
            last_cands = solve_candidate_count();
            /* WHAT IS RUNNING, not just how much has run. A run that stops advancing is the one thing this
               stream exists to make visible, and a line of pure counters cannot name the flow it stopped in —
               it says a stall happened and nothing about where, which leaves bisecting the fixture as the only
               way to localise it. A candidate flow is identified by the (sink, payload) it is verifying, so the
               last line before a stall names the search that entered it. */
            {
                long sc = 0, st = 0, sm = 0, hs = 0, he = 0, ds = 0, de = 0;
                cow_swap_stats(&sc, &st, &sm);
                /* AND WHAT THE TWO CHAINS ARE STILL HOLDING. The three numbers above are about the COST of a
                   switch and say nothing about RETENTION, which is the other thing a delta can get wrong: a
                   frontier of four flows whose chains hold tens of thousands of frozen segments is a lifetime
                   bug, and it reads exactly like a healthy run in `installs`/`entries`/`worst`. */
                cow_chain_stats(&hs, &he);
                dom_cow_chain_stats(&ds, &de);
                printf("@SWAP {\"installs\":%ld,\"entries\":%ld,\"worst\":%ld,\"mean\":%.1f,"
                       "\"heapSegs\":%ld,\"heapSegEntries\":%ld,\"domSegs\":%ld,\"domSegEntries\":%ld}\n",
                       sc, st, sm, sc ? (double)st / (double)sc : 0.0, hs, he, ds, de);
            }
            /* WHAT THE FRONTIER'S PARKED SNAPSHOTS ARE MADE OF — the cold tier's census (solver/cold.h). The
               @SWAP line above says what a context SWITCH costs and the @HEAP line below says what the RUNTIME
               holds; between them a parked flow's own state — its decision vector, its delta head, its DOM
               head, its queued work — had no number at all, and that is exactly the thing a pager pages. The
               PER-FLOW rows are what multiply by the frontier's size; the SHARED rows are counted once for the
               whole frontier because a frozen segment is referenced by every flow forked below it. */
            {
                ColdCensus c;
                cold_census(&c);
                /* `owed` BESIDE `blocked`, because the two answer different questions and the GAP between them
                   is the diagnostic. `blocked` asks each flow's REGISTER whether the host owes it anything;
                   `owed` counts the flows that have told the SCHEDULER they cannot progress, which is what the
                   pick actually reads. A fully blocked frontier reporting `blocked: 512, owed: 59` is one whose
                   marks are being cleared faster than the sweep can lay them down — the state that made this
                   document swap COW deltas 1.76 million times instead of reporting STALLED, and which had to be
                   inferred from a switch count because no number named it. On a healthy stall the two agree. */
                /* `finished` AND `deepest` BESIDE THEM, because "not one flow has ever completed" and "the
                   document never reaches its second script" were both being read off numbers that cannot say
                   it: `live == flows` is the frontier's size against the number ever created, and `script` is
                   the CURRENT flow's cursor. Both are facts this engine holds — see g_finished/g_deepest. */
                /* AND `hostAsked`/`hostAnswered` BESIDE THEM, for the question those four leave open: whether
                   a waiting frontier is waiting because of the RANKING or because nobody has paid it. `blocked`
                   and `owed` are LEVELS and cannot say (see g_host_asked); these two are the run's TOTALS, so
                   the gap between them is the answer, and a run whose `hostAnswered` is 0 has never once been
                   paid. THEY ARE NOT TWO NAMES FOR `blocked`, and the register one over is why: a flow parked
                   on a DISCOVERY probe or on a fetch reply is host-OWED without being host-BLOCKED, so `owed`
                   counts a superset of `blocked` — and neither counts what has already been settled, which is
                   the only thing that distinguishes a paid frontier from a starved one. */
                printf("@COLD {\"flows\":%ld,\"framed\":%ld,\"blocked\":%ld,\"owed\":%d,"
                       "\"finished\":%ld,\"deepest\":%d,\"hostAsked\":%ld,\"hostAnswered\":%ld,"
                       "\"decEntries\":%ld,\"decKiB\":%ld,\"headEntries\":%ld,\"headKiB\":%ld,"
                       "\"domHeadEntries\":%ld,\"domHeadKiB\":%ld,\"jobs\":%ld,\"pend\":%ld,\"pendKiB\":%ld,"
                       "\"dynKiB\":%ld,\"miscKiB\":%ld,\"perFlowKiB\":%ld,"
                       "\"segKiB\":%ld,\"domSegKiB\":%ld,\"pinSegs\":%ld,\"pinSegEntries\":%ld,"
                       "\"pinSegKiB\":%ld,\"decSegs\":%ld,\"decSegEntries\":%ld,\"decSegKiB\":%ld,"
                       "\"sharedKiB\":%ld,\"stepMachines\":%d}\n",
                       c.flows, c.framed, c.blocked, flow_host_owed_count(),
                       g_finished, g_deepest, g_host_asked, g_host_answered,
                       c.dec_entries, c.dec_bytes / 1024, c.head_entries, c.head_bytes / 1024,
                       c.dom_head_entries, c.dom_head_bytes / 1024, c.job_count, c.pend_count,
                       c.pend_bytes / 1024, c.dyn_bytes / 1024, c.misc_bytes / 1024,
                       (c.dec_bytes + c.head_bytes + c.dom_head_bytes + c.pend_bytes + c.dyn_bytes +
                        c.misc_bytes) / 1024,
                       c.seg_bytes / 1024, c.dom_seg_bytes / 1024,
                       c.pin_seg_count, c.pin_seg_entries, c.pin_seg_bytes / 1024,
                       c.dec_seg_count, c.dec_seg_entries, c.dec_seg_bytes / 1024,
                       (c.seg_bytes + c.dom_seg_bytes + c.pin_seg_bytes + c.dec_seg_bytes) / 1024,
                       JS_StepMachineCount(JS_GetRuntime(g_sess_ctx)));
            }
            /* CREATED IS NOT LIVE, AND A COUNTER THAT CANNOT TELL THEM APART CANNOT NAME A LEAK. A run whose
               created-flow count climbs with the switch count is either CHURN (each flow finishes and the
               frontier stays small) or ACCUMULATION (the frontier itself grows) — two different defects with
               one number between them, and the whole difference is `live`. Beside it the RUNTIME's own live
               heap, because a frontier that stays small while the heap climbs is a leak somewhere else
               entirely, and that is the third answer this line has to be able to give. `script` names WHICH
               program the running flow is in, which is the only thing that turns "it stopped advancing" into a
               place in the fixture. */
            {
                JSMemoryUsage mem;
                JS_ComputeMemoryUsage(JS_GetRuntime(g_sess_ctx), &mem);
                /* WHAT THE HEAP IS MADE OF, because "it grew" names nothing to fix. The runtime already counts
                   its own allocations by KIND, and the kinds answer different questions: a climbing
                   `allocations` with a flat `objects` is memory no GC object owns (an atom, a string, a
                   property table, a bytecode function), and each of those has a different owner and a
                   different place where the owner forgot to let go. */
                /* A COUNTER IS NAMED AFTER WHAT IT COUNTS, AND THESE TWO WERE NOT. They were emitted as
                   `realmBytes`/`realmParts` on the claim that `memory_used_*` is "the runtime's own walk of its
                   CONTEXT LIST", so a reader asking "is the growth child realms?" read them and got an answer
                   about something else. JS_ComputeMemoryUsage's own body says otherwise: the context walk adds
                   two entries per realm, and then EVERY object's property array, EVERY fast array's element
                   vector, every var_ref, bound function, C-closure record and module entry adds to the same
                   pair. It is the MISCELLANEOUS bucket — everything the typed counters above do not name — and
                   at this fixture's first switch it already reads 5977 parts with one realm in the runtime.
                   So it is called that, and the realm question is answered by the one component that knows the
                   answer: navigable.c's own list of the child realms this agent built, which is precisely the
                   working set its OOM `CHECK` names (one per flow that created a navigable with an address,
                   none reclaimed). A mislabelled counter is worse than a missing one — it is a wrong answer
                   that looks like a measurement. */
                /* AND WHAT THE BYTES ARE, not just how many of each kind there are. `allocations` and the
                   counts cannot be compared against `heapKiB`, so a run whose allocator holds 655 MB with 72k
                   objects has nothing in this line to say where those bytes are; the SIZE fields the runtime
                   already computes do, and `unattributed` is the residual — malloc_size minus every kind the
                   runtime can name, which is the engine's OWN js_malloc'd memory (heap call frames, flow
                   blobs) and is the number that names the next thing to fix when it is the one that climbs. */
                /* AND THE ALLOCATOR UNDER ALL OF IT, because every number above is quickjs's own accounting and
                   the engine is not the only thing in the address space. Lexbor's document arenas, the per-flow
                   COW deltas and every other `malloc` in the host are invisible to `heapKiB`, so a run whose
                   RSS is sixteen times its JS heap has nothing in this line to say what the other fifteen
                   sixteenths are. `cLive` is what the C allocator currently has handed out — quickjs's bytes
                   INCLUDED, since js_malloc routes to malloc — so `cLive - heapKiB` is the host's own live
                   memory, and `arena` is the address space that allocator has ever needed. In wasm those two
                   differ permanently: LINEAR MEMORY ONLY GROWS. A page the allocator hands back stays mapped,
                   so `arena` is a HIGH-WATER MARK and RSS follows it rather than `cLive` — which means a run
                   whose `cLive` is flat while `arena` climbs is not leaking at all, it is fragmenting, and the
                   two have entirely different fixes. Nothing in this engine could tell those apart before. */
                {
                    /* `memory_used_size` IS THE TOTAL, NOT A ROW BESIDE THE OTHERS. JS_ComputeMemoryUsage's last
                       two statements add atoms, strings, objects, properties, shapes, function bytecode and
                       pc2line INTO it, and every fast-array element vector was already added to it in the
                       object walk — so summing those rows again beside it counted the whole named heap TWICE
                       and subtracted it twice from malloc_size. `unattributed` was therefore too small by the
                       size of the entire attributed heap, and could read as a healthy residual while the named
                       part of the heap was the thing growing. The residual is one subtraction: what the
                       allocator holds, minus everything the runtime can name. */
                    long long attributed = (long long)mem.memory_used_size;
                    printf("@HEAP {\"allocations\":%lld,\"atoms\":%lld,\"strings\":%lld,\"objects\":%lld,"
                           "\"shapes\":%lld,\"props\":%lld,\"funcs\":%lld,\"funcCode\":%lld,\"arrays\":%lld,"
                           "\"miscBytes\":%lld,\"miscParts\":%lld,\"childRealms\":%d,"
                           "\"objBytes\":%lld,\"propBytes\":%lld,\"shapeBytes\":%lld,\"strBytes\":%lld,"
                           "\"atomBytes\":%lld,\"funcBytes\":%lld,\"arrayElemBytes\":%lld,"
                           "\"unattributed\":%lld,\"stepMachines\":%d,\"trampFrames\":%d,\"cLiveKiB\":%lld,\"arenaKiB\":%lld}\n",
                           (long long)mem.malloc_count, (long long)mem.atom_count, (long long)mem.str_count,
                           (long long)mem.obj_count, (long long)mem.shape_count, (long long)mem.prop_count,
                           (long long)mem.js_func_count, (long long)mem.js_func_code_size,
                           (long long)mem.array_count,
                           (long long)mem.memory_used_size, (long long)mem.memory_used_count,
                           navigable_realm_count(),
                           (long long)mem.obj_size, (long long)mem.prop_size, (long long)mem.shape_size,
                           (long long)mem.str_size, (long long)mem.atom_size, (long long)mem.js_func_size,
                           (long long)mem.fast_array_elements * (long long)sizeof(JSValue),
                           (long long)mem.malloc_size - attributed,
                           /* HOW MUCH OF THAT RESIDUAL IS SUSPENDED BUILTINS. `unattributed` is by construction
                              everything JS_ComputeMemoryUsage cannot name, and a step machine is the largest
                              thing in it: one per continuation-holding builtin a flow is inside, each carrying
                              its captured arguments, so a frontier of parked flows holds one per parked call.
                              Without this the residual is a number with no decomposition — the same defect
                              `live` fixed for `flows`. */
                           JS_StepMachineCount(JS_GetRuntime(g_sess_ctx)),
                           /* AND THE HEAP CALL STACK, the other half of that residual and the larger one:
                              a parked flow is a SUSPENDED CHAIN, so this is the frontier's depth in frames.
                              Growth here with a flat `live` is a chain nothing unwound. */
                           JS_TrampFrameCount(JS_GetRuntime(g_sess_ctx)),
                           (long long)engine_c_alloc_live() / 1024, (long long)engine_c_alloc_arena() / 1024);
                }
                /* `sold` BETWEEN THEM, because it is the only thing that makes the gap between `flows` and
                   `live` readable. A frontier that stops growing has either finished flows or PAGED them, and
                   those are opposite verdicts on the same two numbers: the first says the document drained,
                   the second says the RAM floor is being traded for cold-tier records. See g_flows_sold. */
                printf("@PROGRESS {\"switches\":%d,\"flows\":%ld,\"live\":%d,\"sold\":%ld,\"objects\":%lld,"
                       "\"heapKiB\":%lld,\"script\":%d,\"candidates\":%d,\"running\":\"%s\",\"forkedAt\":{",
                       g_switches, flow_created_count(), flow_count(), g_flows_sold,
                       (long long)mem.obj_count, (long long)(mem.malloc_size / 1024),
                       g_sess_cur ? g_sess_cur->script_i : -1, last_cands,
                       g_sess_cur && g_sess_cur->cand_sink ? g_sess_cur->cand_sink : "-");
                /* WHERE the frontier is growing, not just that it is. The key holds the separator bytes the
                   constraint is keyed with, which are control characters, so it is escaped like the payload
                   below rather than emitted raw into JSON. */
                {
                    long hits = 0;
                    const char *k;
                    int i;

                    for (i = 0; (k = decide_fork_at(i, &hits)) != NULL; i++) {
                        if (i) printf(",");
                        putchar('"');
                        for (; *k; k++) {
                            if (*k == '"' || *k == '\\') printf("\\%c", *k);
                            else if ((unsigned char)*k < 0x20) printf("\\u%04x", (unsigned char)*k);
                            else putchar(*k);
                        }
                        printf("\":%ld", hits);
                    }
                    printf("},\"forks\":%ld", decide_fork_total());
                }
            }
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
    /* THE SESSION IS CLOSED ON EVERY EXIT, not only on the one that drained. `engine_sched_step` closes it when
       it answers DONE; the OTHER way out of this loop — a stall nobody can supply — left the session LIVE, with
       the scheduler's hooks still installed over a frontier the host was about to tear down and one flow still
       switched in. That is exactly what engine_session_close's own comment says must not differ between the two
       ways a session ends, and the way it ended here was the one nothing said. */
    if (r != ENGINE_STEP_DONE)
        engine_session_close();
}

/* EXPLORE: seed boot + drain the frontier, forking at every concolic branch. */
void engine_run(JSContext *ctx, char **bodies, char **srcs, int n) { run_scheduler(ctx, bodies, srcs, n, 1); }

