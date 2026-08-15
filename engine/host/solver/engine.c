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
#include "core/frame/navigable.h"   /* @HEAP's realm count: the one component that holds this agent's realms */
#include "solver/dom_cow.h"   /* the DOM half of time-travel — swapped per-flow alongside the heap COW delta */
#include "solver/cold.h"      /* what the frontier's parked snapshots are made of — the cold tier's census */
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

/* THE SESSION'S SCRIPT SEQUENCE — the document's own scripts in order. Entry i is inline (bodies[i] is its text)
   or external (srcs[i] is its URL and bodies[i] is filled when the host replies). Declared here because the
   pending DRAIN writes into it: an external script's text is the DOCUMENT's, shared by every flow. */
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
            DCHECK(!pending_get_int(p, PEND_HAVE_VALUE),
                   "the host answered one request twice — the second answer would overwrite a value the "
                   "asking machine may already have read");
            pending_set(p, PEND_VALUE, JS_DupValue(ctx, value));
            pending_set(p, PEND_COMPLETION, JS_NewInt32(ctx, completion));
            pending_set(p, PEND_HAVE_VALUE, JS_TRUE);
            JS_FreeValue(ctx, p);
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
    DCHECK(world_doc_hosted(world_doc_intern(doc)),
           "a record was routed to an instance that does not hold the document it names — the offscreen is the "
           "only zone that knows which instance holds which document, and it sent this one to the wrong place");
    DCHECK(world_doc_intern(doc) == world_local_doc(),
           "a record was routed to a document this agent hosts but is not its root — delivering it needs the "
           "REALM of that document (document_realm_of), which this router does not select yet: build the "
           "per-document realm lookup here before a child navigable can receive a routed message");
    {
        WorldId w, anc[16];
        int n_anc = world_parse(worlds, &w, anc, (int)(sizeof anc / sizeof anc[0]));
        DCHECK(w.doc != world_local_doc(), "a record was routed back to the instance whose flow sent it — a "
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
    }
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

    DCHECK(flow_running() == f, "a routed delivery was made while another flow was switched in — it would run "
                                "against that flow's delta and its task would land on that flow's queue");
    doc = strchr(dup, '\t');    *doc++ = 0;
    worlds = strchr(doc, '\t'); *worlds++ = 0;
    tail = strchr(worlds, '\t'); *tail++ = 0;
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
        window_message_route(ctx, tail, world_doc_name(w.doc), f->deliver_origin);
    else
        DFAIL("a record was routed with an op no component receives — the sending half emits it, so the "
              "receiving half is the unbuilt one; build it rather than dropping the delivery");
    free(f->deliver); f->deliver = NULL;
    free(f->deliver_origin); f->deliver_origin = NULL;
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
                pending_set(p, PEND_VALUE, JS_DupValue(ctx, value));
                pending_set(p, PEND_HAVE_VALUE, JS_TRUE);
                n++;
            }
            JS_FreeValue(ctx, p);
        }
    }
    return n;
}
/* Resolve every pending fetch this flow issued (the network completed). Returns how many were drained. */
/* Is any of this flow's pending fetches deliverable? A flow with only host-owed entries has no work — it stalls
   rather than spinning on a drain that would resolve nothing. */
static int flow_pending_ready(const Flow *f) { return pending_ready(f->pending); }

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
                const char *body = JS_ToCString(ctx, pv);
                DCHECK(body != NULL, "an external document script's body did not arrive as text");
                g_sess_bodies[si] = body ? strdup(body) : strdup("");
                CHECK(g_sess_bodies[si], "engine: OOM storing an external document script");
                if (body) JS_FreeCString(ctx, body);
            }
        } else if (kind == FLOW_PENDING_SCRIPT) {
            /* the reply is PROGRAM: it joins this flow's script sequence, and the one BFS runs it */
            const char *body = JS_ToCString(ctx, pv);
            DCHECK(body != NULL, "an injected script's body did not arrive as text");
            if (body) { engine_queue_script(body); JS_FreeCString(ctx, body); }
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
            /* THE REPLY, in the one shape every host delivers. This host is handed the body by the trusted
               zone; the status and headers safeFetch saw are what it owes next, and building the reply here is
               what makes that a change in one place rather than a second delivery shape. */
            size_t rlen = 0;
            const char *rbody = JS_ToCStringLen(ctx, &rlen, pv);
            JSValue reply = fetch_reply_new(ctx, 200, "OK", NULL, rbody ? rbody : "", rbody ? rlen : 0);
            JSValue resolve = pending_get(p, PEND_RESOLVE);
            if (rbody) JS_FreeCString(ctx, rbody);
            if (JS_CallAsFlow(ctx, resolve, reply) < 0) {
                JSValue exc = JS_GetException(ctx);
                JS_FreeValue(ctx, exc);   /* a rejected delivery is the page's to observe, not this drain's */
            }
            JS_FreeValue(ctx, resolve);
            JS_FreeValue(ctx, reply);
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
    /* A DELIVERY IS MADE BEFORE ANY CODE RUNS, so no flow can be at a branch while still holding its record. If
       one is, the record would be inherited by the sibling and delivered TWICE — the same message arriving in
       two timelines of one document, which no peer sent. */
    DCHECK(parent->deliver == NULL, "a flow forked while still holding a routed record — the sibling would "
                                    "inherit it and deliver the peer's one message a second time");
    /* THE SIBLING'S WORLD IS A CHILD OF THE PARENT'S, and the edge is recorded so another instance that
   already holds a segment for the parent can materialize the sibling's by forking it — the same O(1)
   shared-base-segment fork this line performs locally, performed there. */
    Flow *sib = flow_add(ctx, parent->fn, NULL, 0, parent->world);
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
            dj->fn = sj->fn;
            dj->argc = sj->argc;
            dj->task = sj->task;
            dj->argv = sj->argc ? malloc((size_t)sj->argc * sizeof(JSValue)) : NULL;
            CHECK(!sj->argc || dj->argv, "engine: OOM inheriting a queued job's arguments at a fork");
            for (int a = 0; a < sj->argc; a++) dj->argv[a] = JS_DupValue(ctx, sj->argv[a]);
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
}

/* The frame-agnostic REPLAY fork is DELETED: re-running a nested/deep flow from its start is BANNED (not
   byte-identical — shared state can differ between the run and the re-run). A concolic branch inside an async
   body on the tramp chain now DFAILs in the engine (see branch_arm_fork) until the sound async-frame snapshot
   is built; there is no re-run fallback to hide that gap. */

/* WHAT KIND OF PROGRAM a queued body is. It is ONE queue because they are one thing — code the page caused to
   run — and the kind decides exactly two questions, both of them at the ends of that program's life: may it
   fail to COMPILE, and does anything read its COMPLETION VALUE. */
typedef enum { DYN_PAGE_SCRIPT = 0, DYN_CANDIDATE, DYN_JAVASCRIPT_URL } DynKind;

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
/* AND THE LINE IT DESCRIBES WAS THE COUNT. The paragraph above was written when the clock replaced
   `(++g_qtick % FLOW_QUANTUM) == 0`; the tree this file now lives in was rebuilt from a restore point that
   carried the prose and not the code, so the comment was accurate about §scheduler and wrong about the line
   under it — authoritative-sounding text over the banned counter, which is the failure mode a stale DFAIL has.
   The slice's start is set by engine_sched_step, the one place that knows when a slice begins, and the hook
   already reads the clock on every consultation for the gap census below, so asking it costs nothing. */
static int64_t engine_now_ms(void);   /* the gap clock below; defined with the session */
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
    /* (0) BLOCKED BEATS BOTH RANKINGS. A flow holding an unanswered synchronous host request cannot make
       progress no matter how it ranks, and the answer cannot arrive while it holds the thread — the host is
       only asked between steps. Deciding this by weight would re-enter it immediately and spin. */
    if (cur && flow_blocked(cur)) return 1;
    if (g_outranked) return 1;                        /* value yield */
    /* (2) COOPERATIVE-QUANTUM floor — thread-sharing, not value, and measured on the clock the slice is bounded
       by. Nothing is dropped, starved or reordered across it: the flow parks and the SAME flow resumes
       byte-identically unless the WFQ says otherwise. */
    return now - g_slice_start >= ENGINE_QUANTUM_MS;
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

static int flow_step(JSContext *ctx, Flow *f, char **bodies, int n) {
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
            f->frame = JS_FlowNew(ctx, body, strlen(body), NULL, 0);   /* page <script>/chunk: classic non-strict global */
            if (f->frame == NULL) {
                /* WHAT ACTUALLY FAILED, read before anything is decided from it. A compile can fail two ways
                   and they are not the same event: a SyntaxError is the program's, and OUT OF MEMORY is the
                   physical floor — the frontier could not hold another flow. Reporting the second as the first
                   sends every reader looking for a parse bug in code that parses; it cost most of a session. */
                JSValue exc = JS_GetException(ctx);
                bool oom = JS_IsOutOfMemoryError(ctx, exc);

                /* OOM IS A `CHECK`, in dev and in release alike: a dropped flow corrupts the frontier, and
                   there is no version of this the engine may proceed past. It is also the honest name for the
                   RAM→disk floor this build has no cold tier under. */
                CHECK(!oom, "the frontier could not hold another flow — this is the physical RAM floor, and the "
                            "cold tier that pages the lowest-value tail to disk is what carries past it");
                /* AN @S CANDIDATE THAT DOES NOT PARSE is a dead candidate and nothing more — the search tries
                   several breakouts per sink precisely because most do not fit most contexts. A `javascript:`
                   URL that does not parse is HTML §7.4.2.3.2's abrupt evaluation, which produces no Document and
                   no navigation — `<a href="javascript:{{{">` is a link that does nothing, not an engine bug. A
                   PAGE script that does not compile is a different thing entirely and still asserts. */
                DCHECK(kind != DYN_PAGE_SCRIPT, "flow_step: a page <script>/chunk did not compile");
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
            /* A SCRIPT THAT THREW names a capability the page needed and this engine does not have. Ending the
               flow there is intentional; losing WHICH capability was not. */
            if (JS_IsException(cv)) {
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
}

static void flow_finish(JSContext *ctx, Flow *f) {   /* f completed: tear down its interleaving state + remove */
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
    /* THE QUEUE AND THE PENDING LIST ARE EMPTY HERE, AND THAT IS ASSERTED RATHER THAN CLEANED UP AFTER. Both
       used to be walked and freed "defensively", which is the fallback shape: the walk can only ever run when
       a work item is being DROPPED, and freeing it quietly is precisely how that drop stays invisible. Neither
       is reachable — flow_step decides "finished" only after offering the job queue a turn (njob > 0 runs one),
       and only when the register is empty, since a pending entry with a value drains and one without reports
       host-owed.
       So the loops were dead, and the one state that would have entered them is the bug. The ARRAY still has to
       be freed: it is this flow's allocation whether or not it ever held an entry. */
    DCHECK(f->njob == 0, "a finishing flow still held queued jobs — its promise reactions and timer callbacks "
                         "are being dropped, and freeing them here is what used to hide that");
    free(f->jobs); f->jobs = NULL; f->jobcap = 0;
    DCHECK(pending_count(f->pending) == 0,
           "a finishing flow still held pending host replies — a flow awaiting one is parked, not finished, "
           "and releasing them here is what used to hide that");
    pending_free(ctx, &f->pending);
    flow_set_running(NULL);
    flow_remove(ctx, f);
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

/* The quantum's wall clock. CLOCK_MONOTONIC, because the slice is about elapsed thread time and a wall-clock
   adjustment must not shorten or extend it. */
static int64_t engine_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

void engine_sched_begin(JSContext *ctx, char **bodies, char **srcs, int n, int forking) {
    DCHECK(!g_sess_live, "engine_sched_begin while a session is already running — one scheduler, one session");
    g_sess_ctx = ctx; g_sess_bodies = bodies; g_sess_srcs = srcs; g_sess_n = n; g_sess_cur = NULL; g_sess_live = 1;
    /* WHAT AN UNCANCELLED REJECTION MEANS is this half's answer: the browser half fires the event and honours
       preventDefault, and a reason that survives that is a page error exactly like a script that threw. */
    unhandled_rejection_set_report_hook(result_page_error_value);
    flow_add(ctx, JS_UNDEFINED, NULL, 0, WORLD_NONE);   /* the first flow: the page's scripts, empty decision vector */
    JS_SetFlowLocalMark(1);                 /* objects created while a flow runs are flow-local (discarded) */
    dom_cow_set_ctx(ctx);                   /* the DOM delta needs ctx for the attribute taint-shadow dup/free */
    cow_set_ctx(ctx);                       /* …and the heap delta needs one for the component records it captures */
    g_dom_capture = 1;                       /* record DOM writes into the running flow's delta (twin of FlowLocalMark) */
    JS_SetFlowControlHooks(forking ? &FC_EXPLORE : &FC_VERIFY);   /* preempt ALWAYS on; fork only when exploring */
    JS_SetJobEnqueueHook(engine_enqueue_job);   /* ASYNC-AS-FLOW: reactions route to the enqueuing flow's queue */
    JS_SetJobDropHook(engine_drop_jobs);        /* …and §7.5.10 step 7 takes them back off it */
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
    int owed = 0;   /* consecutive picks that could not progress; == flow_count() means every member is waiting */
    engine_slice_begin();   /* the hook's floor measures THIS slice, so it starts when the slice does */
    DCHECK(g_sess_live, "engine_sched_step with no live session");
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
            /* AGING IS CHARGED IN MILLISECONDS OF CPU, not in steps, and the difference is what a step MEANS.
               `flow_age_running(1)` was written when a step was a whole drain — a long, roughly comparable
               chunk of work — so one unit per step approximated CPU. A step is now ONE unit of work, so the
               same charge bills a flow the same amount for twelve milliseconds of execution as for advancing a
               single script index, and the running flow loses its rank the instant it is preemptible. Adding
               suspend points made flows preemptible everywhere, which turned that approximation into a
               round-robin with a COW delta swap per switch: 108000 switches on a fixture that explores 7168
               flows, with the switch count tracking elapsed time instead of exploration.
               §scheduler says the term demotes "a monopolizer that burns CPU without emitting", so it is CPU
               that must be measured. The step is already timed for the seam assertion; this is that number. */
#if APICLIENT_DEV
            int64_t t0 = engine_now_ms();
            uint64_t pq0 = 0, pf0 = 0, pa0 = g_preempt_asked;
            /* THE WORK THIS STEP PERFORMS, sampled beside the clock and for a reason the clock cannot serve —
               see the verdict below. Forks, flows and jobs are things the engine DID; no amount of being
               descheduled can inflate them. */
            long w0 = engine_work_done();
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
                   CPU TIME WOULD BE THE RIGHT MEASURE AND THIS HOST DOES NOT HAVE ONE. Emscripten's WASI
                   clock_time_get answers every non-realtime clock — CLOCK_MONOTONIC, and CLOCK_PROCESS_CPUTIME_ID
                   / CLOCK_THREAD_CPUTIME_ID, which is what ISO C `clock()` is built on — from
                   emscripten_get_now(), i.e. wall time. So there is no process-CPU source to switch to, and
                   leaving a wall clock in the verdict while calling it CPU would be worse than saying so.
                   WHAT IS MEASURED INSTEAD IS WHAT THE STEP DID. Forks, flows created and jobs run are work the
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
                   `for(;;);`. Nothing inside the process can distinguish that from being descheduled without a
                   CPU clock, so it is named here rather than papered over with a threshold that would fire on
                   both. An outside observer (the host's own watchdog) is where that one belongs. */
                long work = engine_work_done() - w0;
                if (work > ENGINE_SEAMLESS_WORK && g_preempt_asked == pa0) {
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
                                   "(wall gap %d ms, step ran %d ms — reported, NOT the verdict: this host has "
                                   "no CPU clock; points asked=%llu, preempts wanted=%llu fired=%llu; slowest "
                                   "Web API member step: %s %dms of %dms over %ld member steps; wrapper map "
                                   "%ld/%ld; live objects %lld, heap %lld KiB) — this stretch has no "
                                   "suspend/resume seam; unit=%s script_i=%d "
                                   "flow=%s payload=",
                                   work, (int)gap, (int)spent, (unsigned long long)(g_preempt_asked - pa0),
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
    /* THE SAME RULE ONE LEVEL UP, over the FLOWS' OWN queues. flow_step asserts that a flow may not FINISH
       holding work, and that covers the flow that runs out of work — but the loop above can also LEAVE with a
       flow still alive: every member answers host-owed in a row, the loop breaks on that, and this line closes
       the session over the survivors. A reaction still on one of their queues is dropped exactly as it would
       be at finish, and only the finish case was being checked, so the wider one was invisible. */
    for (int i = 0; i < flow_count(); i++) {
        DCHECK(flow_at(i)->njob == 0,
               "the frontier was declared exhausted while a live flow still held queued jobs — its promise "
               "reactions, timer callbacks and delivered messages die with the session");
        /* AND THE SAME RULE FOR A ROUTED RECORD, which is a work item exactly as a job is. flow_finish asserts
           it for the flow that RUNS OUT of work; a flow that leaves this loop alive (every member host-owed in
           a row) had nothing checking it, and the record then dies in flow_registry_free — the peer's message,
           dropped, indistinguishable from a page that registered no handler. A flow suspended inside a live
           frame is the shape that reaches here holding one: the delivery is made only where flow_step has no
           frame, so if this fires, the enqueue belongs earlier than that branch. */
        DCHECK(flow_at(i)->deliver == NULL,
               "the frontier was declared exhausted while a live flow still held a routed record — a peer's "
               "message this document never received, dropped with the session");
    }
    JS_SetJobEnqueueHook(NULL);
    JS_SetJobDropHook(NULL);
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
    engine_sched_begin(ctx, bodies, srcs, n, forking);
    for (;;) {
        r = engine_sched_step();
        if (r == ENGINE_STEP_DONE)
            break;
        /* THE HOST OWES A REPLY, so this is where it pays. Without this seam the loop had nowhere to answer a
           stall from and a request the trusted host must make simply ended the run: every flow that fetched
           stopped at its fetch, and its continuation — the part that reads the reply — never ran at all. A
           provider that fills nothing ends the run, which is the honest answer to "nobody can supply this". */
        if (r == ENGINE_STEP_STALLED) {
            if (!g_provider || g_provider(ctx) == 0)
                break;
        }
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
                printf("@COLD {\"flows\":%ld,\"framed\":%ld,\"blocked\":%ld,"
                       "\"decEntries\":%ld,\"decKiB\":%ld,\"headEntries\":%ld,\"headKiB\":%ld,"
                       "\"domHeadEntries\":%ld,\"domHeadKiB\":%ld,\"jobs\":%ld,\"pend\":%ld,\"pendKiB\":%ld,"
                       "\"dynKiB\":%ld,\"miscKiB\":%ld,\"perFlowKiB\":%ld,"
                       "\"segKiB\":%ld,\"domSegKiB\":%ld,\"pinSegs\":%ld,\"pinSegEntries\":%ld,"
                       "\"pinSegKiB\":%ld,\"decSegs\":%ld,\"decSegEntries\":%ld,\"decSegKiB\":%ld,"
                       "\"sharedKiB\":%ld,\"stepMachines\":%d}\n",
                       c.flows, c.framed, c.blocked,
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
                    long long attributed = (long long)mem.atom_size + mem.str_size + mem.obj_size +
                        mem.prop_size + mem.shape_size + mem.js_func_size + mem.js_func_code_size +
                        mem.js_func_pc2line_size + mem.memory_used_size +
                        (long long)mem.fast_array_elements * (long long)sizeof(JSValue);
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
                printf("@PROGRESS {\"switches\":%d,\"flows\":%ld,\"live\":%d,\"objects\":%lld,"
                       "\"heapKiB\":%lld,\"script\":%d,\"candidates\":%d,\"running\":\"%s\",\"forkedAt\":{",
                       g_switches, flow_created_count(), flow_count(),
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
}

/* EXPLORE: seed boot + drain the frontier, forking at every concolic branch. */
void engine_run(JSContext *ctx, char **bodies, char **srcs, int n) { run_scheduler(ctx, bodies, srcs, n, 1); }

