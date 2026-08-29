/* The dispatch loop — see engine.h. */
#include "core/fetch/fetch.h"
#include "solver/engine.h"
#include "quickjs-step.h"   /* a host answer is TAKEN by a step machine, and a throw ends it JS_STEP_ABRUPT */
#include "core/html/unhandled_rejection.h"
#include "core/events/report_exception.h"   /* HTML §8.1.4.4 step 8: what a classic script's abrupt completion
                                               owes the page, as the frame the scheduler runs it as */
#include "core/idl_args.h"   /* the one point every Web API member passes through — see idl_slowest_step */   /* HTML §8.1.7.5: what the browser half owes this checkpoint */
#include "solver/result.h"
#include "solver/solve.h"
#include "solver/flow.h"
#include "solver/dyn_body.h"  /* a queued program's SOURCE TEXT — one buffer, every timeline that holds it */
#include "solver/decide.h"
#include "solver/concolic.h"
#include "solver/cow.h"
#include "solver/world.h"   /* the routed record's world vector: whose timeline a delivery belongs to */
#include "core/dom/document.h"   /* which DOCUMENT a parked program belongs to: the realm it is compiled in */
#include "core/dom/document_current_script.h"   /* §4.12.1.1's bracket around the classic arm of a row */
#include "core/dom/node.h"       /* the ELEMENT a row is the program of, crossing a park as its wrapper */
#include "core/html/html_script.h"   /* §4.12.1.1's encoding-parse of `src` against §4.4's API base URL — the ONE
                                        statement of it, and the only thing this file needed core/url/url.h for */
#include "core/loader/script_fetch.h"   /* HTML §8.1.4.2: where a fetched body becomes a script's source text */
#include "core/frame/window_message.h"   /* the receiving half of a routed `windowproxy.post` */
#include "core/frame/remote_op.h"        /* the receiving half of a cross-agent OPERATION: what performs it */
#include "core/frame/remote_object.h"    /* …and the grammar its completion crosses back in */
#include "core/frame/navigable.h"   /* @HEAP's realm count: the one component that holds this agent's realms */
#include "core/frame/document_lifecycle.h"   /* HTML §7.4.6.1: what a navigation does to the Document it replaces */
#include "solver/dom_cow.h"   /* the DOM half of time-travel — swapped per-flow alongside the heap COW delta */
#include "solver/cold.h"      /* what the frontier's parked snapshots are made of — the cold tier's census */
#include "solver/quantum.h"   /* the cooperative quantum's asynchronous edge, and what THIS host can measure */
#include "solver/reclaim.h"   /* …and its RAM edge: which allocators can sell a flow rather than fail */
#include "solver/reply_decode.h" /* …and what the reply BODY itself teaches — see engine_provide */
#include "solver/endpoint.h"    /* the @H surface, and one of the three tables no leak walk in this runtime can see */
#include "solver/attr_shadow.h" /* …and the taint shadow, whose entries ARE GC objects and are leaked by two hosts */
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

/* ONE CLAIMANT, AND NULL GIVES IT BACK. The slot is the frontier's and the answer is the DOM's, so the DOM
   layer releases it at its own release — which the whole platform runs before solver_agent_free — and the
   assert down there is what makes that ordering a checked fact rather than a remembered one. */
void engine_set_wrap_stats(void (*fn)(long *n, long *cap))
{
    DCHECK(fn == NULL || g_wrap_stats == NULL,
           "a second component claimed the wrapper-census hook — there is one identity map, and the second "
           "claim silently decides what every diagnostic line reports");
    g_wrap_stats = fn;
}


/* FETCH-AWAIT parking: a host fetch registers its resolve capability on THE RUNNING FLOW. A flow that awaits it
   suspends its async body; the flow's own pending register is delivered from when the reply arrives — each awaiting
   async body's reaction enqueues as a job in that flow's queue — and it resumes. Per-flow (not global) so one
   flow's delivery never resolves another flow's fetch (which would route the reaction to the wrong flow's COW — a
   leak + contamination).
   THERE IS ONE FORM, and it names a URL. A second form used to take the value UP FRONT, for an edge that
   already had the bytes; nothing in the engine ever did — the Fetch component and the module loader both owe
   theirs to the trusted host — and its only caller was a fixture stub standing in for a host that did not
   answer. With the host answering, that stub and this form both go. */
/* The same park, with the URL the HOST must fetch. The value arrives later through engine_provide; until it
   does the flow cannot finish, which is what keeps the reply-gated code reachable. */
void engine_pending_fetch_url(JSContext *ctx, JSValueConst resolve, JSValueConst value, const FetchRequest *req) {
    Flow *f = flow_running();
    /* THE METHOD AND THE URL ARE THE REQUEST'S IDENTITY, and both are what this park is keyed on: the join
       lists the PAIR and engine_provide delivers against it (engine.h). The rest of the request — headers and
       body — rides the record for the host that will issue it: `safeFetch` decides SOP/CORS/method/credentials,
       and it cannot decide about a method it was never told. */
    const char *url = req ? req->url : NULL;
    JSValue e;
    /* A PARK NEEDS A FLOW TO PARK ON, AND THE ENTRIES THAT HAVE ONE ARE THE ONES THAT RUN AS FLOWS. The
       register this pushes onto is `f->pending` — per-flow, because §State-isolation forbids a global one (one
       flow's delivery resolving another flow's fetch routes the reaction into the wrong COW) — so a caller
       with no flow has nowhere to be owed an answer and nowhere to resume.
       THE LINE HERE USED TO CLAIM THE PRECONDITION ALWAYS HOLDS: "a live fetch is ALWAYS issued from a running
       flow — both explore and @S verify are the ONE scheduler now (run_scheduler), so flow_running() is set".
       That is true of script and of every enqueued job, and it is FALSE of the pre-boot BASELINE, which is
       where this engine performs the steps a browser performs during TREE CONSTRUCTION. Three lines of HTML
       falsify it — a document whose whole content is one `<link rel=preload as=font>` and no script at all
       aborts here — because core/dom/document.c's parsed-tree walk runs at baseline (its own comment: "this
       line is still the pre-boot BASELINE") and HTML §4.2.4.3 "Fetching and processing a resource from a link
       element" ends in "Fetch request", with no task and no microtask anywhere in it.
       WHY `<img>` DOES NOT REACH THIS AND `<link>` DOES, WHICH IS THE WHOLE OF THE DIFFERENCE: §4.8.4.3.5's
       "update the image data" queues the SPEC'S OWN microtask before it fetches, and a queued job is a flow, so
       the image walk hands its fetch to a flow by the standard's own structure. The link has no such step. That
       is an accident of two algorithms, not a design, and it is why the crash belongs at the entry rather than
       here: this park is right to refuse, and a second park that answered without a flow would be the global
       register §State-isolation bans.
       SO THE FIX IS AT THE BASELINE ENTRY — the parsed-tree walk may RECORD an address (an endpoint is not a
       request) but may not ISSUE one; the issue belongs to the boot flow, beside where that document's parsed
       scripts are driven. core/html/html_link.c asserts that at its own baseline entry and names it. */
    DCHECK(f != NULL, "engine_pending_fetch_url: a live fetch issued outside a running flow — see the comment "
                      "above this line: the callers that HAVE a flow are script and enqueued jobs, and the "
                      "caller that does not is the pre-boot baseline tree walk performing a step HTML defines "
                      "over tree construction. Issue it from the boot flow, not from the walk");
    e = pending_push(&f->pending, FLOW_PENDING_RESOLVE);
    pending_set(e, PEND_RESOLVE, JS_DupValue(ctx, resolve));
    pending_set(e, PEND_VALUE, JS_DupValue(ctx, value));
    if (url) pending_set(e, PEND_URL, JS_NewString(ctx, url));
    /* AND THE METHOD IS NOT OPTIONAL WHERE THERE IS AN ADDRESS. Fetch §2.2 Requests: "A request has an
       associated method (a method). Unless stated otherwise it is `GET`" — so every request HAS one, and a
       producer that reaches this park without stating it is a component that dropped a field, not a request
       that lacks one. Answering "assume GET" here is exactly the wrong answer this seam was keyed on the pair
       to stop: it would file a POST's park under a GET and collect the GET's body. */
    DCHECK(!url || (req && req->method && *req->method),
           "a fetch parked on an ADDRESS without stating its METHOD — the reply seam is keyed on the pair "
           "(engine.h), so an unnamed method would park this request under another one's identity and settle "
           "it with that request's body. Name the method at the component that built the request");
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
   owed SOURCE TEXT, so the delivery settles `resolve` with the reply's body rather than with the reply record a
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
    /* HTML §8.1.4.2 Fetching scripts, "fetch a classic script", creates a potential-CORS request and never sets
       a method, so it is Fetch §2.2's `GET`. STATED, because the seam is keyed on the pair and a park that does
       not say is a park the join cannot list. */
    pending_set(e, PEND_METHOD, JS_NewString(ctx, "GET"));
    pending_set_int(e, PEND_SCRIPT_I, script_i);
    JS_FreeValue(ctx, e);
}

/* PARK ON AN INJECTED SCRIPT. `document.body.appendChild(s)` with `s.src` set is the other way a page loads code
   conditionally, and it has no promise for the reply to settle — the reply IS more program. The flow parks on
   the URL exactly as a fetch does (same register, same dedup, same stall accounting) and the delivery queues the
   body as this flow's next script, so the loaded code runs in the world that injected it: its COW delta, its
   pins, its position in the BFS. A sibling that never took that branch never sees the script. */
/* …AND THE `set of scripts that will execute as soon as possible` PARKS THE SAME WAY, which is why this is one
   entry and no longer takes the flow as a parameter. A member of that set has no POSITION to hold — §13.2.7
   waits for the set only before the load event — so its reply becomes a program whenever it is delivered,
   exactly as an injected script's does. A script whose position IS fixed takes a slot instead
   (engine_queue_docscript_url), and the second caller this used to have, which registered a joined document's
   own <script src> against a flow that was not running, went with that. */
/* …AND IT CARRIES THE ELEMENT'S TYPE, because the reply is a PROGRAM and §4.12.1.1's "execute the script
   element" switches on that type. It used to be decoded and evaluated as CLASSIC unconditionally, which was a
   restatement of the gap this park sat behind rather than a fact about these entries: `<script type=module src>`
   injected by page code is how a modern bundle loads a chunk, and it was the shape that aborted. */
/* …AND IT CARRIES THE ELEMENT, which is the fact a park is where you LOSE. The flow reaches this line with
   the node in hand (core/html/html_script.c's insertion half) and comes back to a URL and a reply, so without
   this the element is gone by the time the reply is a program — and HTML §4.12.1.1 "Processing model"'s
   "execute the script element" is a switch on EL whose classic arm sets §3.1.7's `currentScript` to it. It
   crosses as the node's WRAPPER (solver/pending.h's `scriptEl`), because this record is made of JS values. */
void engine_pending_script_url(JSContext *ctx, const char *url, ScriptType stype, lxb_dom_element_t *el) {
    Flow *f = flow_running();
    JSValue e;
    DCHECK(f != NULL, "a <script src> was parked on outside a running flow");
    DCHECK(url != NULL && *url, "a <script src> was parked on with no URL");
    DCHECK(script_type_executes(stype),
           "a <script src> was parked on for an element whose type executes nothing — §4.12.1.1 fires an error "
           "event at an `importmap` or `speculationrules` element with a `src` and returns without fetching, "
           "so no such element ever reaches a park");
    e = pending_push(&f->pending, FLOW_PENDING_SCRIPT);
    pending_set(e, PEND_URL, JS_NewString(ctx, url));
    pending_set_int(e, PEND_SCRIPT_TYPE, (int)stype);
    /* §8.1.4.2's fetch, whose decode and whose evaluation entry the type above decides. */
    pending_set(e, PEND_METHOD, JS_NewString(ctx, "GET"));
    /* AND WHICH DOCUMENT'S PROGRAM THE REPLY WILL BE. The element was inserted into a tree, and the realm this
       chokepoint was entered with is that tree's document — the reply is compiled there rather than in
       whichever realm the session happens to be rooted at. */
    pending_set_int(e, PEND_DOC, (int)document_doc(ctx));
    /* AND WHOSE ELEMENT IT IS. NULL is not a shape this park can have — every caller reaches it from
       §4.12.1.1's own steps over an element — so it is asserted rather than defaulted past. */
    DCHECK(el != NULL, "a <script src> was parked on with no element — §4.12.1.1 reaches this destination only "
                       "from `prepare the script element`, whose whole subject is EL, and a row without one "
                       "would run the fetched program with this document's currentScript left null");
    pending_set(e, PEND_SCRIPT_EL, node_wrap(ctx, lxb_dom_interface_node(el)));
    JS_FreeValue(ctx, e);
}

/* A PEER HOLDS A REFERENCE INTO THIS DOCUMENT — see engine.h's engine_set_referenced. It is a property of the
   INSTANCE and not of a session, so it is set once by the host that provisioned this instance and survives
   every session boundary; it is read by flow_step (a timeline that may not finish) and by the slice (a
   frontier that may not be declared exhausted), which are the two halves of one statement. */
static int g_referenced;

/* The browser layer's document-load lifecycle, asked when a flow has run everything the document gave it. */
static int (*g_docdone_hook)(JSContext *ctx);
void engine_set_document_done_hook(int (*fn)(JSContext *ctx))
{
    /* THE ASSERT COULD NOT BE WRITTEN UNTIL THE CLAIM MOVED, and that is why it was not. This slot was being
       claimed from core/dom/document.c's per-DOCUMENT install, so a page with one <iframe> claimed it twice and
       an assert here would have aborted every multi-document run before the release existed to make it true —
       the crash without the fix. The claim is document_init's now and it is given back at document_agent_free,
       so both halves hold: one claimant, and NULL is the release. A second claim on a one-entry slot silently
       stops the first from running, which is a member doing half its job with nothing to say so. */
    DCHECK(fn == NULL || g_docdone_hook == NULL,
           "a second component claimed §13.2.7's document-load lifecycle step — one call of it answers for "
           "EVERY document of this agent (it walks navigable_tree_order), so the claim is one per agent and was "
           "being re-made once per DOCUMENT until core/dom/document.c moved the line into its declaration");
    g_docdone_hook = fn;
}

/* THE EVENT LOOP'S TIMER STEP, registered by the timer component for the reason the document hook is: naming
   it here would make the scheduler depend on the browser half. Asked only where this flow has nothing else to
   run, which is the one moment virtual time may move — see timer.h. */
static int (*g_timer_hook)(JSContext *ctx);
void engine_set_timer_hook(int (*fn)(JSContext *ctx))
{
    DCHECK(fn == NULL || g_timer_hook == NULL,
           "a second component claimed §8.1.7's timer step — there is ONE event loop per agent, and this was "
           "being re-claimed once per REALM until core/timing/timer.c moved the line into its declaration");
    g_timer_hook = fn;
}

/* THE EVENT LOOP'S OTHER CLOCK-DRIVEN SOURCE — §8.1.7.3's in-parallel half, which queues an update-the-
   rendering task on the rendering task source when a navigable has a rendering opportunity. Registered by the
   rendering component for the reason the timer step is: naming it here would make the scheduler depend on the
   browser half. It is asked immediately BEFORE the timer step and yields to a timer that expires first, so the
   ONE virtual clock still runs its two sources in the order their moments fall. */
static int (*g_rendering_hook)(JSContext *ctx);
void engine_set_rendering_hook(int (*fn)(JSContext *ctx))
{
    DCHECK(fn == NULL || g_rendering_hook == NULL,
           "a second component claimed §8.1.7.3's in-parallel half — there is one rendering task source per "
           "event loop, and the second claim silently decides every frame the first was queueing");
    g_rendering_hook = fn;
}

/* THE END OF A MICROTASK CHECKPOINT — HTML §8.1.7.3, registered by the browser component that owns what HTML
   invokes there. See engine.h; the one caller today is Indexed Database §2.7.1's transaction cleanup. */
static void (*g_checkpoint_hook)(JSContext *ctx);
void engine_set_checkpoint_hook(void (*fn)(JSContext *ctx))
{
    DCHECK(fn == NULL || g_checkpoint_hook == NULL,
           "a second component claimed the end-of-microtask-checkpoint step — HTML invokes ONE list there and "
           "this slot holds one entry, so the second claim silently stops the first from running");
    g_checkpoint_hook = fn;
}

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
/* WHICH DOCUMENT THE SEQUENCE BELOW BELONGS TO, said rather than implied by `g_sess_ctx`. A session is opened
   over the instance's ROOT document (world.h's world_local_doc — the one the host named the agent for), and its
   scripts are that document's; every other program a flow runs names its own document, which is what lets a
   child navigable's scripts be work items of this frontier at all. It is the handle and not the realm for the
   reason a queued program's is: a handle survives a park and a realm does not. */
static uint32_t g_sess_doc;
/* THE ROOT DOCUMENT'S SCRIPT INVENTORY, AND IT IS A SEED RATHER THAN A SEQUENCE — which is the whole of this
   change and the reason four parallel arrays and an offset are gone.
   WHAT WAS WRONG WITH THE OTHER SHAPE. A flow's cursor used to walk TWO representations: this document's
   scripts, held here in `bodies`/`srcs`/`types` and addressed as [0, n), and then the flow's OWN rows, held in
   the seven-column dyn table and addressed as [n, n + dyn_n) through a `- n` offset. Two representations of one
   thing is exactly one thing too many, and the position that fell between them was not hypothetical: HTML
   §4.12.1.1 "Processing model" ends "prepare the script element" with "Otherwise, immediately execute the
   script element el, even if other scripts are already executing", so a program a page `eval`s or injects from
   its SECOND-to-last <script> belongs at the slot after that script — a slot INSIDE the static half, which the
   dyn table could not name. engine_queue_into carried a `CHECK` there rather than a DCHECK, so a real page that
   did it aborted the engine in RELEASE as well as in dev, and a real page did.
   WHAT IT IS NOW. There is ONE representation: every flow's programs are rows of the dyn table, and this is
   only the template each flow of this document is SEEDED from at the moment it is created. Nothing reads it at
   a compile, at a drain or at a cursor — engine_seed_scripts is its one reader — so the cursor has one address
   space and `- n` has nothing left to subtract.
   THE ADDRESS IS RESOLVED ONCE, HERE, and that is §4.12.1.1's own sentence: "Let url be the result of
   encoding-parsing a URL given src, relative to el's node document" is computed once per ELEMENT, and both of
   the things this engine asks it for — §8.1.4.2's fetch and, for a module, the record's identity — must be the
   same answer. Resolving it per seed instead would ask the question again mid-session, under whichever flow's
   delta happened to be applied, so a `<base href>` one timeline wrote would silently re-point another
   timeline's bundle.
   `body` AND `url` ARE TWO INDEPENDENT ITEMS OF ONE SCRIPT, NOT TWO SPELLINGS OF ONE. HTML §8.1.4.1 "Scripts"
   says so of the base URL field itself: "Null or a base URL used for resolving module specifiers. When
   non-null, this will either be the URL from which the script was obtained, for external scripts, or the
   document base URL of the containing document, for inline scripts." So `body` answers HAVE I GOT THE SOURCE
   TEXT (NULL until a reply carries it, which is what parks a flow on the row) and `url` answers WHERE DID THE
   BYTES COME FROM (NULL for a `<script>` with no `src`, whose base URL §4.12.1.1 takes from the document).
   A row with BOTH is the ordinary end state of an external script — §8.1.4.2 "Fetching scripts": "Let script
   be the result of creating a classic script given sourceText, settingsObject, response's URL, options,
   mutedErrors, and url" — and it is what a host that already holds the response hands over. An exactly-one-of
   invariant stood here and it was WRONG in exactly that case: it read "the source has not arrived yet" as "the
   script has no address", so the address of every pre-fetched external script was DROPPED at this build and
   the program compiled under its DOCUMENT's name. For a module that name is the module map KEY, so a document
   with two `<script type=module src>` had one module.
   A row that is NEITHER is not a row: a `src` that does not encoding-parse becomes NO ROW AT ALL —
   §4.12.1.1's "if url is failure … fire an event named error at el, and return" means that element runs no
   script, so there is nothing for the sequence to hold. Both strings are this table's own: it is built in
   engine_sched_begin and freed in engine_session_close, which is the one place a session ends. */
typedef struct RootScript {
    /* THIS ROW'S SOURCE TEXT, or NULL while the flow is still owed it — and ONE buffer for the whole session,
       because the table is read once per BASE flow (engine_seed_root_flow is the seed hook, so the boot flow,
       every cold-resumed replay and every @S candidate session read it) and a document's own bundle is the
       largest program this engine holds. It was a `char *` the seed strdup'd into every one of them. */
    DynBody *body;
    char *url;           /* §4.12.1.1's encoding-parsed address, or NULL for a `<script>` with no `src` */
    ScriptType type;     /* which of §8.1.4.4 "Calling scripts"'s two algorithms evaluates it */
    /* THE `script` ELEMENT THE ROW IS THE PROGRAM OF — §4.12.1.1's "execute the script element" is a switch on
       EL, and its "classic" arm sets that document's HTML §3.1.7 `currentScript` to it for the whole of the
       run. It is here for the reason `type` is: by the time the scheduler holds a body the element is behind
       it, and this is the last table that still knows.
       BORROWED, unlike the two strings above — it names a node of the tree this session was opened over, and
       the tree outlives the session. It is also why this pointer may never be PARKED: the cold tier stores a
       recipe and replays the document from its first script, so the rows are rebuilt and a node's address is
       never asked to mean anything outside the session that read it. */
    lxb_dom_element_t *el;
} RootScript;
static RootScript *g_root_scripts;
static int g_root_n;
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
/* …AND HOW MANY ARRIVED WHEN THERE WAS NOTHING LEFT TO CONSUME THEM. A peer answers BY RUNNING A PROGRAM, so
 * its completion is produced on ITS timeline and relayed by a zone that cannot know what became of the asker in
 * the meantime — and §Time-travel's Level-1 eviction makes "the peer outlives the asking instance's park" the
 * ORDINARY case rather than the exotic one. So an answer arriving after this session closed is not a bug in
 * anybody's routing: the flow that asked is written down as a recipe, and the replay RE-ISSUES the request and
 * is answered with today's value. It is REFUSED rather than written (see engine_host_answer) and counted here,
 * because a refusal nobody can see is a drop: `hostAnswersLate` climbing while `hostAnswered` does not is a
 * host paying an instance that has already stopped listening. */
static long g_host_answers_late;

/* WHAT A SOLD FLOW TOOK WITH IT — the same family as the three above and sitting with them, because every one
 * of these is a fact about a debt the host is carrying and what became of the flow that was owed it; the pager
 * far below is only where the number is WRITTEN. Two counts and not one, because a paged flow owes TWO
 * different debts settled through different doors: a fetch reply is paired by (method, url) against the
 * host's own pending list, and a synchronous request is routed BY ID to a call site. One count for both was
 * a real defect and a silent one — a sold HOSTREQ entry inflated the fetch side, so the very next reply the
 * host genuinely mispaired spent that credit and the assert written to catch it (main.c's provide pairing)
 * said nothing.
 * Split at the sale, by kind, so each door consumes only its own — and on this side by what is still OWED,
 * because an ANSWERED fetch entry sold with a flow is a reply the host has already sent and can never send
 * again (pending.h, pending_owed_replies): the residue of the same over-credit, wearing the other half of the
 * register.
 *
 * A flow BLOCKED on the host is the cheapest member to page — its recipe re-issues the request in the session
 * that resumes it and gets today's answer, which is §Time-travel's whole point — so the tail is very often a
 * flow the host is mid-fetch for. The reply then arrives for a URL nobody is parked on any more, and that is
 * not the host mispairing its list: it is a park that happened between the ask and the answer. Counted rather
 * than assumed, so the pairing assert stays armed for the case it was written for and is answered exactly once
 * per reply the sale made unnecessary. */
static long g_paged_owed;
/* …AND THE REQUEST SIDE, WHICH IS REPORTED RATHER THAN SPENT. It is not a credit: one request may still be
 * owed N answers by N peer timelines, so a per-entry credit is a number that runs out and then aborts under
 * the message written for a DIFFERENT door — the exact "right crash for the wrong reason" this file is being
 * corrected for. It is stated in the abort that cannot tell those doors apart (engine_host_answer) and on the
 * @COLD census line, and it decides nothing. */
static long g_paged_reqs;

/* HOW MANY FLOWS THIS SESSION ACTUALLY SOLD. The pager's whole claim is that `live` stops tracking `flows`
   under pressure, and until this counter existed that claim had no WRITER: `live < flows` is equally the
   signature of flows FINISHING, and `_park`'s record count is two record kinds (segments and flows) added
   together, so neither of the two numbers a run already printed can answer "did it page". CLAUDE.md's rule is
   that a consumer never defaults a producer's field; the same rule read forwards is that a measurement gets a
   producer before it gets quoted. */
static long g_flows_sold;

int engine_take_paged_owed(void) {
    /* BELOW ZERO IS THE IMPOSSIBLE STATE, and it is the only one this function can see. The credit is a count
       of entries the host was shown and has not answered (engine_reclaim_tail) and this is its ONLY spender,
       so a negative debt means either a second spender exists or one reply spent two credits — and from then
       on the number is no longer "replies owed for flows this instance sold", so every later answer to "is
       this reply explained by a sale?" is a guess wearing a measurement's name. */
    DCHECK(g_paged_owed >= 0,
           "the paged-reply debt went NEGATIVE — it is a count of unanswered entries that left with sold "
           "flows and this is its only spender, so more replies have been excused as sales than were ever "
           "sold, and qjs_provide's pairing assert is now excusing the host's real mispairings");
    /* ZERO IS NOT A BROKEN INVARIANT, IT IS THE ANSWER. This is the question qjs_provide asks to tell a SALE
       from the host's pairing being off, and "no sale explains this reply" is precisely the case its DFAIL
       exists for — so aborting here would report the host's mispairing under the pager's message, which is the
       "right crash for the wrong reason" g_paged_reqs was split off to stop. The test is `== 0` because the
       assert above has made `< 0` unreachable. */
    if (g_paged_owed == 0)
        return 0;
    g_paged_owed--;
    return 1;
}

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

/* Deliver an answer. Routed by id to the call site that asked — never broadcast the way a fetched body is,
   because the answer was computed under the ASKING FLOW'S world.
 *
 * A SECOND ANSWER TO ONE QUESTION IS NOT A DUPLICATE — it is a SECOND PEER TIMELINE, and the asking side's half
 * of that is the fork this records for. A peer's document state IS its flows, so a cross-agent operation is
 * performed by every live timeline it has and each one completes with its own answer: `otherW.length` has N
 * answers for N peer timelines and they are all true. It is not hypothetical and it is not the peer host's
 * oversight — engine_sibling_assemble gives a sibling the answering program's row, rendezvous token and all
 * (flow.h's `dyn_token`), on purpose, because a branch inside the answering program is a real peer timeline in
 * which the answer differs, so N answers under one token is what that fork was built to produce. Taking the first and asserting on the rest picks a timeline; overwriting
 * picks the last, after the machine may already have read the first.
 *
 * WHICH IS WHY EVERY PEER ANSWER NAMES THE TIMELINE THAT COMPUTED IT, and why this entry takes that name as a
 * parameter rather than deriving anything from the arrival ORDER. "Another timeline answered" and "one
 * timeline's answer was delivered twice" are two different events with identical shapes, and until the world
 * rode the answer no zone and no engine could tell them apart: the first is a fork this flow owes, the second
 * is a relay defect that forks an arm into a peer timeline that is already being explored. Both were happening
 * — the routing zone kept one answer per token in a one-slot map and dropped the rest — and the symptom was a
 * page reading `w.closed` twice in one expression and being answered out of two contradictory timelines of one
 * document. The name is world_serialize's, so it is the same name the asking side already writes on its
 * requests and the peer already keys its segments on. A HOST answer names NO timeline (NULL): the trusted zone
 * computed it itself and there is exactly one of it, which is what `source` says.
 *
 * SO THE ANSWER IS RECORDED HERE AND THE FORK IS TAKEN IN flow_step, and that split is the whole design. This
 * entry runs BETWEEN scheduler steps: `flow_running()` still names whichever flow was last switched in,
 * `cow_delta_fork` would freeze the delta CURRENTLY APPLIED and `dom_cow_fork` would fork the LIVE DOM head, so
 * a fork on this line would clone a stranger's timeline and call it the asker's — the exact "two timelines
 * wearing one name" the world registry exists to prevent, with nothing to say so. What CAN be done between
 * steps is to write the completion onto the entry as a JS value, which is a property write the COW delta and
 * the cold tier already carry; flow_answer_fork then builds the arm with that flow switched in.
 *
 * A HOST-COMPUTED ANSWER GENUINELY HAS ONE ANSWER — a fetch this zone performed — and answering one of those
 * twice is the zone's bug, which is what `source` is for and why it is not sniffed from the request's text.
 *
 * THE RETURN VALUE IS A STATEMENT ABOUT DELIVERY, AND IT USED TO BE A LIE. 1 means the answer was written onto
 * a register entry of a flow that CAN STILL CONSUME IT; 0 means it was not delivered. Those are not the same as
 * "an entry naming this id was found", which is what this used to answer: after a park closes the session the
 * flows are still allocated — cold_park writes their recipes and the instance teardown frees them — so the walk
 * below found the entry, wrote the value, cleared the flow's host-owed mark and reported success onto a flow
 * whose scheduler no longer exists (qjs_step latches g_done). The host was told its answer landed; nothing
 * would ever read it. */
int engine_host_answer(JSContext *ctx, uint32_t req, const char *world, JSValueConst value, int completion,
                       int source) {
    int extra = 0, fixed = 0;

    DCHECK(req != 0, "the host answered a request with no id");
    DCHECK(completion == ENGINE_COMPLETION_NORMAL || completion == ENGINE_COMPLETION_THROW,
           "the host answered a request with a completion type that is neither normal nor a throw");
    DCHECK(source == ENGINE_ANSWER_HOST || source == ENGINE_ANSWER_PEER,
           "an answer arrived without saying who computed it — a value this zone computed has exactly one "
           "answer and a peer's has one per timeline, and only the deliverer knows which this is");
    /* THE TWO SOURCES DIFFER EXACTLY IN WHETHER A TIMELINE PRODUCED THE ANSWER, so the name is required of one
       and forbidden of the other. An anonymous PEER answer is the state this whole field exists to end: it
       cannot be told from a duplicate, so the check below could not exist and the fork it guards would be taken
       over a timeline that is already an arm. */
    DCHECK((source == ENGINE_ANSWER_PEER) == (world != NULL && *world),
           "a cross-instance answer disagreed with its own source about whether a peer TIMELINE computed it — "
           "a peer answers BY RUNNING A PROGRAM on one of its flows and must name that flow's world, and a "
           "value the trusted zone computed itself has no flow to name");
    /* PARSED ONLY WHERE THE ASSERT IS COMPILED. world_parse INTERNS the document names it reads, which is a
       side effect and therefore may not sit inside a DCHECK's condition — and in release there is no reader for
       the result, so the whole block goes with the check rather than leaving a name interned for nobody. */
#if APICLIENT_DEV
    if (world) {
        WorldId w;
        const WorldId *anc;
        (void)world_parse(world, &w, &anc);
        /* THE ANSWERING TIMELINE BELONGS TO A PEER. A world of a document THIS agent holds means the answer was
           produced in this heap and relayed back in as if it had crossed — a routing loop, which would deliver
           one of this instance's own flows' state to a call site that asked another agent, and would then be
           forked over as a peer timeline that does not exist. It is the mirror of engine_perform's assert on
           the ASKING world, and it is the same sentence: an operation on this agent's own object never leaves. */
        DCHECK(!world_doc_hosted(w.doc),
               "a cross-instance answer was computed in a world of a document THIS agent holds — the operation "
               "was routed back to the instance whose flow asked it, so the value came from this heap and the "
               "arm forked over it would be a timeline of the asking agent wearing the peer's name");
    }
#endif
    /* THE SESSION IS OVER, SO THERE IS NOTHING HERE THAT CAN CONSUME AN ANSWER — and this is a HANDLED state,
       not an asserted one, which is the whole difference between it and the two aborts at the bottom. The
       registers below still exist (a park writes recipes and leaves the flows for the teardown to free), so the
       walk would find the entry and write onto it; what does not exist is a scheduler to run the flow that
       would read it. Refusing BEFORE the walk is what makes the return value true — the entry is not touched,
       the flow's host-owed mark is not cleared, and `hostAnswered` is not credited for a delivery that did not
       happen. It is not a drop either: the parked flow's recipe RE-ISSUES the request in the session that
       resumes it and is answered with today's value (§Time-travel-resume), which is why the peer outliving the
       asker's park is the ordinary shape of Level-1 eviction rather than a failure of it. Counted so the
       refusal is visible — see g_host_answers_late and the @COLD census. */
    if (!g_sess_live) {
        g_host_answers_late++;
        return 0;
    }
    for (int k = 0; ; k++) { Flow *f = flow_at(k); if (!f) break;
        for (int i = 0, n = pending_count(f->pending); i < n; i++) {
            JSValue p = pending_entry(f->pending, i);
            if (pending_get_int(p, PEND_KIND) != FLOW_PENDING_HOSTREQ ||
                (uint32_t)pending_get_int(p, PEND_REQ) != req) { JS_FreeValue(ctx, p); continue; }
            if (!pending_get_int(p, PEND_HAVE_VALUE)) {
                DCHECK(!pending_get_int(p, PEND_ANSWER_FIXED),
                       "a request whose answer was FIXED by an answer fork is UNANSWERED — the arm is built from "
                       "the answer it took, so the two are written together and one without the other is an arm "
                       "that would wait for a question nobody was asked");
                pending_set(p, PEND_VALUE, JS_DupValue(ctx, value));
                pending_set(p, PEND_COMPLETION, JS_NewInt32(ctx, completion));
                /* THE TIMELINE IS WRITTEN WITH THE VALUE, in the bracket the completion is written in and for
                   the same reason: a flow that has taken an answer without knowing whose it is cannot address
                   its NEXT operation to that same timeline, and cannot refuse the same one arriving again. */
                pending_set(p, PEND_ANSWER_WORLD, world ? JS_NewString(ctx, world) : JS_NULL);
                pending_set(p, PEND_HAVE_VALUE, JS_TRUE);
                JS_FreeValue(ctx, p);
                /* AND THE FLOW IS ASKABLE AGAIN. This is the event a flow parked on a synchronous request is
                   waiting for — the one thing that can change the answer it gave the scheduler — so the mark
                   comes off HERE, on the flow the answer reached, and not on a slice boundary that means
                   nothing to it (flow.h). Without this line the answer would sit on the register while the pick
                   kept skipping the flow that asked for it.
                   THE FIRST ANSWER STOPS AT THE FIRST FLOW because an unanswered SYNCHRONOUS request is the one
                   record a fork does NOT share: engine_sibling_assemble calls pending_unshare on it and mints a
                   fresh rendezvous id, because its answer is computed under the ASKING FLOW'S world. So exactly
                   one flow's register can name this id and the return below reaches every flow there is.
                   THIS PARAGRAPH USED TO SAY THE OPPOSITE — that stopping here is right BECAUSE a fork shares
                   records and "every arm that inherited this request observes it already" — which is the exact
                   reasoning that made engine_provide skip the mark of every arm but the first, and left those
                   timelines out of the pick for good. Sharing is the reason a per-record fact may not decide a
                   per-flow one; it is never the reason a per-flow clear may stop early. */
                flow_clear_host_owed(f);
                g_host_answered++;   /* the PAY half of the rate above — every delivery that reached a flow */
                return 1;
            }
            /* A `CHECK` AND NOT A DCHECK, and it became one when the answer started carrying its TIMELINE. A
               zone-computed answer names no timeline (there is no flow of anybody's behind it), and everything
               below this line is about recording ANOTHER TIMELINE'S answer — so a second HOST answer reaching
               it would be recorded as a peer timeline with no name, and the asking flow would fork an arm into
               a timeline of the answering document that does not exist. In release a DCHECK is not compiled at
               all, so this would have been the one path where the missing name became a live record instead of
               an abort. It is what CLAUDE.md's rule names: not-proceeding is required in production too. */
            CHECK(source == ENGINE_ANSWER_PEER,
                  "the trusted zone answered one request TWICE. A value this zone computed — §7.4 step 14's "
                  "load, XHR §3.5.6's fetch — has exactly one answer, so the second is a delivery this zone "
                  "made after it had already made one, and the flow has by now run on the first");
            /* AN ARM'S OWN ANSWER IS FIXED, and skipping it here is what keeps the frontier from doubling per
               answer: the arm holds the SAME request id (the id lives in the step state inside the frame it is
               a clone of), so without this a third answer would fork from the arm as well as from the issuer,
               producing a timeline that answered B and then C at one call site. */
            if (pending_get_int(p, PEND_ANSWER_FIXED)) { fixed++; JS_FreeValue(ctx, p); continue; }
            /* AND THE SAME TIMELINE MAY NOT ANSWER ONE QUESTION TWICE. A peer performs the operation ONCE per
               live flow (engine_perform attaches it to each) and spends the token off the row as it answers
               (flow_answer_perform), so one timeline has exactly one completion for one rendezvous — a second
               under that name is a RELAY that duplicated, and it is silent in every other way: the value is a
               real value of a real timeline, so an arm forked over it explores a timeline an arm is already in
               and the frontier grows a twin nothing can distinguish. This is the assert the world on the answer
               exists to make possible, and it is why a duplicate must never be quietly dropped here — dropping
               it would hide the relay defect that produced it, which is exactly how the routing zone came to
               keep one answer per token and discard the peer's other timelines. */
            DCHECK(!pending_answer_world_seen(p, world),
                   "one peer TIMELINE answered one cross-instance request TWICE. Its flow spends the rendezvous "
                   "token off its row as it answers, so this instance was handed the same completion twice by "
                   "the zone that routes them — and the arm this would fork is a second flow exploring a "
                   "timeline of the answering document that another arm already holds");
            /* …AND AN ANSWER BEYOND THE FIRST DOES NOT STOP AT THE FIRST FLOW, because it may not be written
               onto a SHARED record. Every issuing timeline still holding this request — the flow that asked and
               each of its BRANCH siblings, all of which observed the first answer — must fork over this peer
               timeline's answer, and each of them DRAINS its own list to do it, so one shared list would let
               whichever ran first take an answer the others were going to explore. The record therefore stops
               being shared here, per flow, which is the same thing the branch fork does to the one field two
               arms must disagree about. */
            JS_FreeValue(ctx, p);
            p = pending_unshare(f->pending, i);
            pending_extra_add(p, completion, JS_DupValue(ctx, value), world);
            JS_FreeValue(ctx, p);
            flow_clear_host_owed(f);
            g_host_answered++;
            extra++;
        }
    }
    if (extra) return 1;
    /* THE SESSION IS LIVE AND NO LIVE REGISTER TOOK THIS ANSWER. Three distinct states reach here and they had
       ONE message between them, which is the failure this file is otherwise built to prevent: a reader sent to
       the wrong door by a `(or …)` in an abort. They are separated below because they are observed differently,
       even where two of them name one thing to build. */

    /* (1) EVERY ENTRY NAMING IT IS AN ARM. The request IS on a register — it was deliberately skipped, because
       an arm's answer is FIXED (it is the timeline that took answer k) — so what is missing is the ISSUER's
       entry, and only the issuer's. It left at the take while the arms it forked live on, which is exactly the
       shape that says the arms are being built correctly and the record they were forked from is not being
       kept. Reachable for a PEER alone: a HOST answering an already-answered entry aborts one loop above. */
    if (fixed)
        DFAIL("a PEER's answer arrived for a request whose only live entries are ARMS — each of those was "
              "forked over an earlier answer and its answer is FIXED, so the entry that could still take one is "
              "the ISSUER's, and that one left the register when the issuing flow took its first answer. This "
              "peer timeline has nowhere to land and the arm it would have forked is silently missing. Keep the "
              "issuer's entry alive past the take while the peer still holds timelines that may answer, and "
              "fork from it as flow_answer_fork already does");

    /* (2) NOTHING NAMES IT AT ALL, and for a value THIS ZONE computed that is innocent: the asking flow is gone
       and nobody is waiting on the answer. */
    if (source == ENGINE_ANSWER_HOST)
        return 0;

    /* (3) …and for a PEER's it is the one gap, reached through THREE doors: the asking flow TOOK its first
       answer (engine_host_take removes the entry), or it FINISHED, or it was SOLD to the cold tier while the
       request was outstanding. The fix is the same one in all three — the entry has to outlive the departure of
       the flow that held it, for as long as the peer holds timelines that may still answer — but the reader
       standing here cannot tell which door it was, and the sale is the one this engine can say something about,
       so it says it in the abort rather than leaving the reader to guess. (The zone does not drop a live
       asker's rendezvous — bridge.js keeps the token until the instance is finalized precisely so every
       completion is relayed — so this is not a stale relay.) */
    DFAILF("a PEER's answer arrived for a request NO register holds — the asking flow left and took the "
           "entry with it (it took its first answer, it finished, or it was paged out), so this timeline "
           "of the answering document has nowhere to land and the arm it would have forked is silently "
           "missing. Keep the entry alive past the flow's departure while the peer still holds timelines "
           "that may answer, and fork from it as flow_answer_fork already does. This session sold %ld "
           "flow(s) owing %ld synchronous request(s), which is the only one of the three doors this "
           "engine can still see from here", g_flows_sold, g_paged_reqs);
    return 0;
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

/* THE DEATH OF A WORLD, ANNOUNCED — see engine.h. One notice per name, written HERE and nowhere else: two
   spellings of this record would be two peers releasing different things, which is the same sentence
   world_serialize carries about the vector it writes. */
void engine_notify_worlds_gone(JSContext *ctx, const char *const *names, int n)
{
    int i;

    DCHECK(n == 0 || names != NULL, "worlds were announced dead by a list that is not there");
    for (i = 0; i < n; i++) {
        /* "world.gone" + TAB + the name + NUL. Sized from the name for world_gone_push's reason: a document
           name nests one component per navigable depth, so a constant here is a cap on the frame tree. */
        size_t cap = strlen(names[i]) + 12;
        char *rec = malloc(cap);
        CHECK(rec != NULL, "engine: OOM announcing a world's death — a peer never told holds that world's "
                           "segment for the rest of its process, and cannot park while it does");
        snprintf(rec, cap, "world.gone\t%s", names[i]);
        engine_host_notify(ctx, rec);
        free(rec);
    }
}

/* See engine.h. The vector and not a bare name, because it is what the RESUME hands back to world_segment —
   the ancestry is half of how a segment is materialized, so a record carrying only the head names a world the
   rebuild would start from the baseline for. */
void engine_notify_worlds_parked(JSContext *ctx, const char *const *vectors, int n)
{
    int i;

    DCHECK(n == 0 || vectors != NULL, "the worlds a park carries were announced by a list that is not there");
    for (i = 0; i < n; i++) {
        size_t cap = strlen(vectors[i]) + 14;   /* "world.parked" + TAB + the vector + NUL */
        char *rec = malloc(cap);
        CHECK(rec != NULL, "engine: OOM announcing a foreign segment this park carries — the zone never told "
                           "holds no death for that world while this document is cold, so the instance that "
                           "resumes rebuilds a segment for a timeline that has ended and keeps it forever");
        snprintf(rec, cap, "world.parked\t%s", vectors[i]);
        engine_host_notify(ctx, rec);
        free(rec);
    }
}

/* WHAT THE HAND-BACK DID — counted at the retraction itself, so the numbers cannot disagree with the notices
   that left. See engine.h for what each one is a statement about. */
static long g_retract_flows, g_retract_started, g_retract_back;

void engine_retract_census(long *flows, long *started, long *handed_back)
{
    DCHECK(flows != NULL && started != NULL && handed_back != NULL,
           "the retraction census was asked to fill nothing");
    *flows = g_retract_flows; *started = g_retract_started; *handed_back = g_retract_back;
}

/* HOW MANY PROGRAM ROWS ACROSS THE FRONTIER STILL CARRY A RENDEZVOUS TOKEN — see engine.h. Scanned, for
   flow_owes_answer's reason. */
long engine_operations_started(void)
{
    Flow *f;
    int i, k;
    long n = 0;

    for (i = 0; (f = flow_at(i)) != NULL; i++)
        for (k = 0; k < f->dyn_n; k++)
            if (f->dyn_token[k]) n++;
    return n;
}

/* IS THIS QUESTION STILL SOMEBODY'S TO ANSWER — asked of every member of the frontier, of BOTH places a
   question lives (queued on the arrival slot, or started with its token on a program's row).
 *
 * IT IS THE WHOLE OF WHY A HAND-BACK IS NOT PER FLOW. One operation is attached to EVERY live timeline
 * (engine_perform), because a document's state is its flows; the QUESTION is still one question and the zone
 * routes by token. So a notice sent while another timeline still holds the operation tells the zone to FORGET
 * a token that timeline is about to answer under — and the zone's own assert is what fires ("a peer answered a
 * cross-agent operation under a rendezvous token this zone never minted"), one seam away from the engine that
 * caused it. So the notice belongs to the LAST holder leaving, and this is the question that decides it —
 * asked AFTER the strip, which is why it takes no exclusion: a flow the hand-back has already emptied answers
 * `no` about itself.
 *
 * SCANNED RATHER THAN COUNTED, for flow_owes_answer's reason exactly: a refcount is a second representation of
 * this fact and it drifts at the three sites hardest to keep in step — the fork (perform_q_fork shares the
 * entries, engine_sibling_assemble strdups the row's token), the start (the token MOVES from the queue to the
 * row) and the answer. */
static int perform_token_held(JSContext *ctx, const char *token)
{
    Flow *f;
    int i, k;

    for (i = 0; (f = flow_at(i)) != NULL; i++) {
        for (k = 0; k < flow_perform_pending(f); k++) {
            JSValue e = JS_GetPropertyUint32(ctx, f->perform_q, (uint32_t)k);
            JSValue tv = JS_GetPropertyUint32(ctx, e, 1);
            const char *t = JS_ToCString(ctx, tv);
            int hit;

            CHECK(t != NULL, "engine: OOM reading a queued cross-agent operation's token while deciding whether "
                             "a question may be handed back — a token that cannot be read is one this walk "
                             "would report as absent, and the hand-back would race a timeline still answering");
            hit = !strcmp(t, token);
            JS_FreeCString(ctx, t);
            JS_FreeValue(ctx, tv);
            JS_FreeValue(ctx, e);
            if (hit) return 1;
        }
        for (k = 0; k < f->dyn_n; k++)
            if (f->dyn_token[k] && !strcmp(f->dyn_token[k], token)) return 1;
    }
    return 0;
}

/* THE DISTINCT QUESTIONS A HAND-BACK HAS TAKEN OFF ITS FLOWS — collected during the strip so the notices can
   be decided once per QUESTION rather than once per holder. See engine_retract_span for why that is not an
   optimisation but the difference between linear and quadratic in the size of the frontier. */
static void retract_seen_add(char ***seen, int *n, int *cap, const char *token)
{
    int j;

    for (j = 0; j < *n; j++)
        if (!strcmp((*seen)[j], token)) return;
    if (*n == *cap) {
        int c = *cap ? *cap * 2 : 8;
        char **g = realloc(*seen, (size_t)c * sizeof *g);
        CHECK(g != NULL, "engine: OOM collecting the operations a park hands back");
        *seen = g;
        *cap = c;
    }
    (*seen)[*n] = strdup(token);
    CHECK((*seen)[*n] != NULL, "engine: OOM naming an operation a park hands back");
    (*n)++;
}

/* HAND BACK EVERY QUESTION A SET OF FLOWS IS HOLDING — one member (`only`), or every member (`only` NULL). The
 * two callers are the two shapes of a park: the PAGER sells one flow while the instance keeps running, and the
 * whole-frontier park writes the residue and leaves. `only` selects WHICH members, never which implementation
 * — there is one implementation and no second body to fall back to.
 *
 * BOTH HALVES OF THE DEBT, AND THE STARTED ONE IS NOT A DIFFERENT KIND. cold.c's refusal asked for the
 * difference to be named: "state why a half-run peer program is different from every other suspended frame this
 * tier regenerates". It is not different, and the reason is OWNERSHIP. A started operation's partial work is
 * its flow's own COW delta, and the delta LEAVES WITH THE FLOW — so a program abandoned here has produced
 * nothing that outlives the park, exactly as every other suspended program's partial work does not. What the
 * peer is owed is not a stored value either: HTML §7.2.1.3.5 "CrossOriginGet ( O, P, Receiver )" ends "Return ?
 * Call(getter, Receiver)", so what this instance owes is a CALL, and a call abandoned before it completes has
 * been made zero times. The zone re-asks, the call is made once, and §7.2.2.2 "Indexed access on the Window
 * object" ("The length getter steps are to return this's associated Document's document-tree child navigables's
 * size") is why answering it later is the ordinary answer rather than a stale one — the value is read off the
 * active document at the moment of the call, which is §Time-travel-resume's "re-derives example VALUES from
 * CURRENT sources" said by the spec.
 *
 * A ROW KEEPS ITS KIND AND LOSES ITS TOKEN, which is the state flow_answer_perform already leaves behind after
 * an answer — so nothing downstream learns a new shape, and a row that somehow ran anyway aborts in
 * flow_answer_perform naming the missing token rather than emitting an answer under a name the zone forgot.
 *
 * STRIP, THEN DECIDE, AND THE ORDER IS THE WHOLE COST. The notice belongs to the LAST holder leaving, and
 * asking that question per HOLDER means a walk of the frontier per flow — quadratic, on a frontier that is
 * routinely thousands, for an answer that is `no` every time but once. Asked per distinct QUESTION after the
 * strip, it is one walk per outstanding peer operation, and that number is not a function of the frontier's
 * size (engine_perform attaches ONE question to every timeline; the questions are the peer's, not ours). */
static void engine_retract_span(JSContext *ctx, Flow *only)
{
    char **seen = NULL;
    int seen_n = 0, seen_cap = 0, i, k;
    Flow *f;

    for (i = 0; (f = flow_at(i)) != NULL; i++) {
        int n, held = 0;

        if (only && f != only) continue;
        /* THE FLOW MAY NOT BE THE ONE HOLDING THE THREAD. Both callers switch out first (the park before it
           walks, the pager by construction), and it is asserted rather than assumed because the failure is
           silent: a switched-in flow can still run its program to completion, and the row's token has just
           been returned to the zone. */
        DCHECK(f != flow_running(),
               "a cross-agent operation was handed back on the flow the scheduler is switched into — that flow "
               "can still run its program to completion, and the completion would find a row whose token this "
               "call has already given back");
        n = flow_perform_pending(f);
        for (k = 0; k < n; k++) {
            JSValue e = JS_GetPropertyUint32(ctx, f->perform_q, (uint32_t)k);
            JSValue tv = JS_GetPropertyUint32(ctx, e, 1);
            const char *tok = JS_ToCString(ctx, tv);

            CHECK(tok != NULL, "engine: OOM reading a cross-agent operation's token at the park — a question "
                               "handed back without its name leaves the flow that asked, in another instance, "
                               "suspended on an answer nothing will send");
            held = 1;
            retract_seen_add(&seen, &seen_n, &seen_cap, tok);
            JS_FreeCString(ctx, tok);
            JS_FreeValue(ctx, tv);
            JS_FreeValue(ctx, e);
        }
        if (n) {
            /* AND THE QUEUE IS EMPTY, which is what keeps cold.c's assert a statement rather than a tolerance.
               Through the engine's write bracket for perform_q_take's reason — this is the scheduler's record
               about a flow, written from outside any flow's delta, and a delta that captured it would restore
               the entries the moment a sibling switched in. */
            cow_engine_write_begin();
            JS_SetPropertyStr(ctx, f->perform_q, "length", JS_NewInt32(ctx, 0));
            cow_engine_write_end();
            DCHECK(flow_perform_pending(f) == 0,
                   "a flow's operation queue survived the retraction that emptied it — the park is about to "
                   "abort on a question this instance has already told the zone it is handing back");
        }
        for (k = 0; k < f->dyn_n; k++) {
            if (!f->dyn_token[k]) continue;
            held = 1;
            g_retract_started++;
            retract_seen_add(&seen, &seen_n, &seen_cap, f->dyn_token[k]);
            free(f->dyn_token[k]);
            f->dyn_token[k] = NULL;
        }
        g_retract_flows += held;
        /* THE POSTCONDITION, ASSERTED WHERE IT IS PRODUCED rather than only where cold.c reads it. Both
           disjuncts of flow_owes_answer are addressed above, so a flow that still owes one here holds a token
           in some THIRD place — and nothing downstream would ever find it. */
        DCHECK(!flow_owes_answer(f),
               "a flow still owed a peer an answer after every question it held was handed back — a rendezvous "
               "token survives somewhere that is neither the arrival slot nor a program's row, so this flow "
               "parks with a debt the zone has been told is returned");
    }
    for (k = 0; k < seen_n; k++) {
        /* THE LAST HOLDER, ASKED AFTER THE STRIP — every flow this call was given holds nothing by now, so a
           hit is a member that is STAYING and will answer under this token. */
        if (!perform_token_held(ctx, seen[k])) {
            size_t cap = strlen(seen[k]) + 20;   /* "remoteop.retracted" + TAB + the token + NUL */
            char *rec = malloc(cap);

            CHECK(rec != NULL, "engine: OOM announcing an operation a park hands back");
            snprintf(rec, cap, "remoteop.retracted\t%s", seen[k]);
            engine_host_notify(ctx, rec);
            free(rec);
            g_retract_back++;
        }
        free(seen[k]);
    }
    free(seen);
}

/* ONE MEMBER — the pager's shape. */
static void engine_retract_flow(JSContext *ctx, Flow *f)
{
    DCHECK(f != NULL, "a question was handed back on no flow at all");
    engine_retract_span(ctx, f);
}

/* See engine.h. Placed with the other two park-time announcements because the three are one statement about
   what this instance is handing over: the residue (world.parked), the names it will never use again
   (world.gone), and the questions it was asked and did not answer (this). */
void engine_retract_operations(JSContext *ctx)
{
    engine_retract_span(ctx, NULL);
}

/* THE INBOUND HALF OF IT — see engine.h. It is deliberately NOT engine_route: a delivery becomes a work item of
   every live timeline and a death is the end of one, so there is nothing to seed, no sender origin to stamp
   (nothing here runs page code) and no target document (the zone broadcasts, because the sender does not track
   which peers a flow reached). */
void engine_world_gone(JSContext *ctx, const char *world)
{
    WorldId w;
    const WorldId *anc;
    int n_anc;

    DCHECK(world != NULL && *world,
           "a world-gone notice carried no name — world_parse would answer out of whatever this instance last "
           "parsed, and the segment released would be a live peer's timeline");
    n_anc = world_parse(world, &w, &anc);
    (void)anc;
    /* A DEATH NAMES ONE WORLD. An ancestor is alive exactly while any descendant of it is, so a chain here
       would release the very segments a still-running sibling arm's next arrival forks from — silently, since
       the peer would then materialize an empty one and the arm would read a document missing its own writes. */
    DCHECK(n_anc == 0,
           "a world-gone notice carried an ANCESTRY — a death names ONE world, and its ancestors are alive "
           "exactly while any descendant is; releasing them here drops the segments a live sibling arm's next "
           "arrival is told to fork from");
    DCHECK(!world_doc_hosted(w.doc),
           "a world-gone notice came back to the instance that minted the world — a local flow's world is the "
           "flow's own and this instance has never held a foreign segment for it, so the zone broadcast a "
           "death to its own sender");
    world_release(ctx, w);
}

/* THE WORLD FIELD OF A ROUTED RECORD, copied out NUL-terminated. Every routed record is
   `<op>\t<doc>\t<worlds>\t<tail>` (engine_route takes the same three fields apart), and a queued entry holds
   the whole text, so the one thing a second reader of it needs is stated once here. Caller frees. */
static char *record_worlds_dup(const char *record) {
    const char *doc, *worlds, *tail;
    char *out;

    doc = strchr(record, '\t');
    DCHECK(doc != NULL, "a routed record named no target document");
    worlds = strchr(doc + 1, '\t');
    DCHECK(worlds != NULL, "a routed record carried no world vector");
    worlds++;
    tail = strchr(worlds, '\t');
    DCHECK(tail != NULL, "a routed record carried nothing after its transport fields");
    out = malloc((size_t)(tail - worlds) + 1);
    CHECK(out != NULL, "engine: OOM reading a routed record's world vector");
    memcpy(out, worlds, (size_t)(tail - worlds));
    out[tail - worlds] = 0;
    return out;
}

/* MAY THIS RECEIVING TIMELINE STILL HEAR FROM THE WORLD `vec`? — the question the commitment record beside a
 * flow's delivery queue exists to answer (flow.h's `deliver_world_q`), and the reason it is a fact about the
 * FLOW rather than about the queue.
 *
 * IT REPLACES A PAIRWISE CHECK OVER THE QUEUE, WHICH WAS THE RIGHT TEST ASKED AT THE WRONG PLACE. That one
 * compared an arriving record against every record still QUEUED, so the identical pair was refused when both
 * were outstanding at once and accepted when the first had already been delivered — the schedule-dependent
 * answer §Testing's differential exists to catch, on the shortest path there is: a router that hands over one
 * record and pumps before handing over the next. What a timeline has RECEIVED is what constrains it, and a
 * queue forgets that the instant it is consumed.
 *
 * TWO KINDS OF COMMITMENT AND THEY REFUSE DIFFERENT THINGS.
 *   - RECEIVED (taken): this timeline heard from that world, so it may not also hear from an arm that
 *     CONTRADICTS it — two arms of one sender branch delivered in sequence hand the page a timeline neither
 *     sender was in, which is the state-merging §Solver-half bans, performed on the receiving side.
 *   - FORECLOSED: this timeline is the ARM minted where its parent took that world, so nothing AT OR UNDER it
 *     is this timeline's. That message is the parent's and is delivered there, which is why refusing it here
 *     drops nothing.
 * INDEPENDENT IS NOT A REFUSAL and that is the half a boolean could not state: two peers posting to one page
 * are in two forests that were never one (world.h), and a receiver split over that pair would be two timelines
 * each missing one sender's messages. */
static int deliver_admits(JSContext *ctx, const Flow *f, const char *vec)
{
    int n = flow_world_commits(f), i, admits = 1;

    for (i = 0; admits && i < n; i++) {
        JSValue e = flow_world_commit_at(f, i);
        JSValue cv = JS_GetPropertyUint32(ctx, e, 0);
        JSValue tv = JS_GetPropertyUint32(ctx, e, 1);
        const char *c = JS_ToCString(ctx, cv);
        WorldRel rel;

        CHECK(c != NULL, "engine: OOM reading which sending timeline a receiving flow is in — a commitment "
                         "that cannot be read is a timeline about to accept the arm it did not take");
        DCHECK(JS_VALUE_GET_TAG(tv) == JS_TAG_INT,
               "a delivery-world commitment carried no small-integer RECEIVED/FORECLOSED flag — the two kinds "
               "refuse different things, so a missing one would be read as whichever the first test asked");
        rel = world_vec_relate(vec, c);
        if (JS_VALUE_GET_INT(tv)) {
            if (rel == WORLD_REL_CONTRADICT) {
                /* AND THE ARM THAT SHOULD TAKE IT EXISTS — asserted HERE because this is the one line that
                   knows the record is being refused, and a refusal with no arm is a peer's message that no
                   timeline of this document ever receives. That arm was minted at the delivery of `c` itself,
                   or at the delivery of an earlier world on the same branch when this timeline had already
                   taken its side there (deliver_committed_at). Either way it exists exactly when `c` came
                   through a BRANCH, so `c` naming a fork point is the necessary condition and the vector's own
                   shape is the whole of the test.
                   A vector that names none is a ROOT world — a flow its instance created from the baseline —
                   and the sending document has two of those the moment the cold tier rebuilds one beside the
                   boot flow: park_flow_add passes WORLD_NONE, so a resumed timeline is a fresh root rather
                   than a child of the world its recipe was parked under. Those two roots contradict and name
                   each other nowhere, so there is no branch at which this receiver could have taken the other
                   side. Give a rebuilt flow the world its recipe was written under, so every timeline of one
                   document meets every other at a fork point. */
                DCHECK(strchr(c, ',') != NULL,
                       "a routed delivery CONTRADICTS a world this timeline already received whose vector "
                       "names no fork point, so the arm that should receive it was never minted and no "
                       "timeline of this document will: the two sending worlds are ROOTS of one document "
                       "(its boot flow beside a cold-resumed one — park_flow_add mints WORLD_NONE) and roots "
                       "name each other nowhere. Rebuild a parked flow as a CHILD of the world its recipe was "
                       "written under, so that pair has a branch between them to fork at");
                admits = 0;
            }
        } else if (rel == WORLD_REL_SAME || rel == WORLD_REL_DESCENDANT) {
            admits = 0;   /* at or under a subtree this arm foreclosed: the parent that took it delivers it */
        }
        JS_FreeCString(ctx, c);
        JS_FreeValue(ctx, cv);
        JS_FreeValue(ctx, tv);
        JS_FreeValue(ctx, e);
    }
    return admits;
}

/* HAS THIS TIMELINE ALREADY TAKEN A SIDE AT `fork_vec`? — the one thing that stops the fork below from minting
 * an arm that is no timeline at all.
 *
 * A fork mints a child for EXACTLY TWO arms and retires the point it branched at, so a receiver that has
 * already committed at a branch has foreclosed the only other side there is; forking again there would produce
 * an arm that rejects both, which no sender was ever in. STRICT DESCENT is the test: a commitment BELOW the
 * fork point is a side taken at it, while a commitment ON it is the sender's own pre-branch world — received
 * by both arms, and by definition not a choice between them. */
static int deliver_committed_at(JSContext *ctx, const Flow *f, const char *fork_vec)
{
    int n = flow_world_commits(f), i, committed = 0;

    for (i = 0; !committed && i < n; i++) {
        JSValue e = flow_world_commit_at(f, i);
        JSValue cv = JS_GetPropertyUint32(ctx, e, 0);
        const char *c = JS_ToCString(ctx, cv);

        CHECK(c != NULL, "engine: OOM reading a receiving timeline's commitment while deciding whether it has "
                         "already taken a side at a sender's branch");
        if (world_vec_relate(fork_vec, c) == WORLD_REL_ANCESTOR) committed = 1;
        JS_FreeCString(ctx, c);
        JS_FreeValue(ctx, cv);
        JS_FreeValue(ctx, e);
    }
    return committed;
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
    /* TWO WAYS A SESSION ENDS AND THEY ARE TWO DIFFERENT FAILURES, so they are two asserts. One message for
       both is how a reader is sent to the wrong file: the states are told apart by whether the frontier was
       WRITTEN OUT, which this engine knows, and the fix is in a different place for each.
       PARKED — the frontier is a set of recipes and this delivery is a work item that is not in any of them.
       It cannot be re-derived by a replay the way an owed REQUEST can (the asking code re-issues that; nothing
       in this document re-sends someone else's message), so it must either park as what it is or be refused
       while one is in flight — which is exactly the pair cold.c's per-flow park already asserts for a delivery
       a flow is HOLDING, arriving here from the other side. */
    DCHECK(g_sess_live || !engine_frontier_paged(),
           "a record was routed into an instance whose frontier was PARKED — the session is closed and its "
           "flows are recipes, so there is nothing to seed the delivery on and no replay that re-derives it: a "
           "message is not re-sent by the document that receives it. Build the inbound half of the cross-"
           "instance park — the record and the sender origin the trusted zone stamped are both TEXT and cross "
           "as text — or have the trusted zone hold the record until the instance it names is resumed");
    /* DRAINED — every timeline of this document finished, which is a document that can no longer receive at
       all. That is the state engine_set_referenced exists to prevent: a document a peer still holds a
       WindowProxy for keeps its last timeline waiting instead of finishing, so a delivery has somewhere to
       arrive. Reaching here means nothing told this instance it was still referenced. */
    DCHECK(g_sess_live,
           "a record was routed into an instance whose frontier had DRAINED — every timeline of the document "
           "finished, so there is no flow to seed the delivery on and no scheduler to run it. A document a peer "
           "can still post to is a REFERENCED document and engine_set_referenced holds its last timeline open "
           "for exactly this, but nothing outside wpt_runner.c sets it: there is no `qjs_set_referenced` in "
           "main.c's ABI at all, so the trusted zone — the only zone that knows a peer still holds a "
           "WindowProxy for this document — has no way to say so. Add the entry and set it from the create "
           "notice the zone already acts on");

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
        WorldId w;
        const WorldId *anc;
        int n_anc = world_parse(worlds, &w, &anc);
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
        /* APPENDED, BECAUSE TWO MESSAGES FROM ONE WORLD ARE SEQUENTIAL. HTML §9.3.3 "Posting messages" ends
           the window post message steps by queueing a global task on the posted message task source, and
           §8.1.7.1 "Definitions" gives a task a source in order to "group and serialize related tasks" — so
           the receiving page observes one sender's two posts IN ORDER, and a slot that refused the second was
           an abort on the shortest path there is. (The section cited here used to be §9.4.4, which is "Message
           ports" — a whole section away from the one that defines this method.)
           AND SENDERS WHOSE WORLDS CONTRADICT ARE NOT REFUSED HERE ANY MORE, because the thing the refusal
           asked for is built. It said this pair "needs a SIBLING FORK, taken at the DELIVERY where the
           receiving flow is switched in", and that is where it is now taken (flow_deliver): the queue is
           APPENDED TO unconditionally, and each timeline decides at its own delivery step which of the arms is
           its own, minting the sibling that takes the other. Two reasons it could not have stayed here even as
           an assert. It runs BETWEEN scheduler steps with no flow applied, so it can neither fork nor read a
           timeline's state; and it compared the arrival against the records still QUEUED, which is the same
           pair as the one already DELIVERED and gives the opposite answer — so a router that pumps between two
           handovers passed it and a router that hands over both first did not, from one program. What a
           timeline has received is written on the timeline (flow.h's `deliver_world_q`) and is asked where
           that timeline is switched in. */
        flow_deliver_push(ctx, f, record, sender_origin);
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

/* THE FLOW A QUEUED CALLBACK BELONGS TO WHEN THE QUEUER IS THE USER AGENT — set ONLY inside
   engine_unload_document's per-flow bracket, which is the same shape flow_job_external_begin/_end already
   brackets the other host-time conversion with.
   IT IS NOT A DISCRIMINATOR AND IT IS NOT A FALLBACK. `flow_running()` is the answer everywhere a PROGRAM
   queued the callback, which is every enqueue but one; the exception is an operation the TRUSTED ZONE reports
   between two scheduler steps, where the flow the stamp names did not cause this callback and so cannot be its
   owner. So the owner is NAMED, and the naming WINS — this is one question with one answer rather than a
   preference between two mechanisms.
   AND "NO FLOW IS RUNNING" IS NOT WHY, which is where the version of this paragraph that said so sent the
   assert below and the whole product path with it. Between two steps `flow_running()` STILL NAMES whichever
   flow was last switched in — engine_sched_step returns the cooperative-quantum yield without switching it
   out, because §scheduler requires that flow to resume byte-identically, and this file states the same fact
   plainly at engine_host_answer ("`cow_delta_fork` would freeze the delta CURRENTLY APPLIED"). Two paragraphs
   of one file disagreed about one register; the one that was wrong was the one an assert rested on, so it
   aborted every instance that was still working when a navigation arrived. What makes the named owner correct
   is CAUSATION, not vacancy: the host's operation is a task of the flow the bracket names and of no other. */
static Flow *g_enqueue_owner;

/* HTML §7.4.6.1 "Updating the traversable"'s DEACTIVATE A DOCUMENT FOR A CROSS-DOCUMENT NAVIGATION, made a work
 * item of EVERY LIVE TIMELINE of this instance — the third seam with engine_route's and engine_perform's shape,
 * and the one with the least room to be anything else.
 *
 * A DESTRUCTION IS PER-FLOW BECAUSE EVERY PIECE OF IT IS STATE A PAGE OBSERVES. §7.5.9 fires `pagehide` and
 * `unload` at listeners a SCRIPT registered, so they live in the COW delta of the flow that ran that script and
 * in no baseline; its "clear window's map of active timers" clears the timers the timeline that set them holds;
 * §7.5.10 step 8's browsing-context-null is a write to the WindowProxy record, which that record's own COW
 * capture makes per-flow — which is the same reason document_lifecycle.h gives for the subtree wait being a
 * count on the proxy rather than a shared integer. Run in ONE timeline this would destroy the document there
 * and leave every other flow running a document the browser replaced: exactly the two-tops state the trusted
 * zone aborts on, moved inside the engine where nothing would say so.
 *
 * NOTHING RUNS INSIDE THIS CALL. Each flow performs its own unload when the scheduler next runs it, under its
 * own delta and at its own rate — so one arm's `pagehide` listener and another's `unload` listener are two
 * suspensions of two flows rather than one drive to completion.
 *
 * THE OWNER IS NAMED RATHER THAN CURRENT, and that bracket is the whole of what this seam adds to the machine
 * it drives. §7.5.9 and §7.5.10 queue GLOBAL TASKS, so the operation is expressed the way every other queued
 * task in this engine is (JS_EnqueueCallTask); that path asks the scheduler WHICH flow owns the callback and
 * the scheduler's answer is `flow_running()`, which between two steps is whichever flow the last slice left
 * switched in — a flow that did not cause this operation and holds no more claim to it than any other member,
 * which is precisely why the owner has to be stated. Both alternatives are wrong
 * and neither is loud: letting the hook DECLINE puts the task on quickjs's global list, which nothing in this
 * engine drains — a destruction that simply never happens while the document goes on reporting — and seeding a
 * FRESH flow from the baseline is the shape engine_route names as silently wrong, a timeline in which the
 * page's own listeners were never registered.
 *
 * NO FLOW IS DROPPED, STARVED OR PAGED TO MAKE THIS HAPPEN, and none has to be. A flow suspended inside the
 * replaced document keeps its snapshot and its place on the ONE frontier; §7.5.10 step 7 removes QUEUED TASKS,
 * which is work that has not started; and what keeps the realm alive under a suspended continuation is the
 * counted references that continuation already holds (core/frame/navigable.c's teardown states the argument
 * beside the assert that rests on it). §NO BOUNDS is untouched: nothing here is a scheduler decision.
 *
 * THE INCOMING DOCUMENT IS NOT AN ARGUMENT, AND THAT IS THE STANDARD'S ANSWER RATHER THAN A SIMPLIFICATION.
 * §7.5.9 "Unloading documents"' unload-a-document-and-its-descendants step 6 queues the operation's global task
 * on `document`'s RELEVANT GLOBAL OBJECT, where `document` is the one being unloaded, and the per-document body
 * asserts the same fact from the other end at its step 1 ("Assert: this is running as part of a task queued on
 * oldDocument's relevant agent's event loop"). §7.5.9's optional `newDocument` decides nothing about the queue:
 * its whole use is the document unload timing info (step 2 creates it, steps 3-4 null it, steps 11 and 13 stamp
 * it, step 22 hands it on), a structure this user agent does not carry, and step 3 is the standard's own answer
 * for an absent one — the answer §7.5.9's descendant walk already passes for every child navigable.
 * THIS ENTRY USED TO TAKE IT AND QUEUE IN ITS REALM, which made the operation unperformable for the navigation
 * that needs it most: an instance is an ORIGIN-KEYED AGENT CLUSTER, so a navigation whose incoming Document is
 * cross-origin loads it into a PEER, and there is no such realm on this side to queue anything in. The outgoing
 * Document is local by construction — it is the one this agent has been running — so asking IT for the queue
 * home is the one question that has an answer for every navigation.
 * §7.5.10 "Destroying documents" STEP 7 DOES NOT EAT THE OPERATION. It removes queued tasks "without running
 * those tasks", and the unload reaches it from inside its own body (§7.5.9 step 20), by which time the task has
 * left the queue; the other half of that is the SCOPE of the removal, which is the timeline performing the
 * destruction and not every timeline of this instance (engine_drop_jobs). */
void engine_unload_document(uint32_t doc)
{
    JSContext *dctx;
    int n, i;

    DCHECK(g_sess_live,
           "a Document was reported REPLACED to an instance with no live session — the unload is a task of "
           "every timeline of that document and there is no scheduler to run one, so the destruction would be "
           "queued onto nothing and the replaced document would go on answering for this instance");
    DCHECK(world_doc_hosted(doc),
           "a Document this agent does not hold was reported replaced — the trusted zone is the only zone that "
           "knows which instance holds which document, and it named this one to the wrong instance; unloading "
           "here would destroy a document the browser did not navigate away from");
    dctx = doc_realm(doc);
    n = flow_count();
    DCHECK(n > 0,
           "a Document was replaced while every timeline of this instance had already finished — there is no "
           "flow to run its unload in, so its `pagehide` and `unload` listeners fire in no timeline at all and "
           "§7.5.10's destruction happens nowhere. A document a navigation can still replace is a document "
           "whose flows are still live, which is the same statement engine_set_referenced makes for a document "
           "a peer still holds a reference into");
    for (i = 0; i < n; i++) {
        Flow *f = flow_at(i);

        /* THE BRACKET, AND IT IS CLOSED ON EVERY PATH THROUGH THE BODY BECAUSE THE BODY HAS ONE. Anything that
           aborts between these two lines aborts the process, so there is no unwind that could leave a stale
           owner naming a flow the registry has since freed. */
        g_enqueue_owner = f;
        document_lifecycle_unload_replaced(dctx);
        g_enqueue_owner = NULL;
        /* ASKABLE AGAIN, for engine_route's reason exactly: a flow that reported host-owed is out of the pick
           until the host does something for it, and this IS that something. A flow holding the unload of its
           own document and never picked is a destruction that never runs. */
        flow_clear_host_owed(f);
    }
}

/* THE SIBLING THIS DELIVERY FORECLOSES — the arm CLAUDE.md §Security asks for by name ("two arms of a fork post
 * two messages belonging to two contradictory worlds, and merging them fabricates a timeline neither sender was
 * in"), minted at the DELIVERY because that is the one moment the receiving flow is switched in.
 *
 * IT IS NOT SPECULATION AND IT IS NOT A DUPLICATE. A world with a FORK POINT above it is one arm of a branch
 * whose other arm was minted at the same instant (world_mint_child mints a child for both and retires the
 * point), so a sending timeline that did NOT send this message exists whether or not it ever sends one of its
 * own. The receiver therefore has a timeline that did not receive it, and that timeline is this arm — receiver
 * state up to this delivery (which it inherits through the delta fork) conjoined with the other side of the
 * sender's branch, which is exactly "receiver-baseline ∧ the sending flow's vector" for the sibling vector.
 * A ROOT world names no fork point and gets NO ARM, which is the difference between this and a snapshot at
 * every delivery: there was no branch, so there is no other side to be on, and minting one would fabricate a
 * timeline that dropped a message.
 *
 * ONE ARM, NOT ONE PER ANCESTOR. The arm forecloses the whole SUBTREE at `vec`, so anything diverging higher up
 * the sender's chain is admitted by it too; the arms for those higher branches were minted at the deliveries of
 * the worlds ON them, each by this same line. Its decision state is decide_fork_same_path's — no arm is
 * recorded because no PREDICATE was asked: the receiving program did not choose, a message arrived. That blob
 * carries the honest limit decide.h names for the answer fork (a decision vector cannot record WHICH peer
 * timeline a fork was over), and here it costs the same thing: the SET of arms is regenerated on a replay while
 * which sender arm each arm took is not, so the mapping is carried by the commitment record (flow.h) across the
 * park and by nothing at all across a re-run of the document.
 * `f` GOES ON TO MAKE THE DELIVERY in this same step, which is why this returns nothing to the scheduler: there
 * is at most ONE arm per delivery, so there is no second one for a later step to mint. */
static Flow *engine_sibling_assemble(JSContext *ctx, Flow *parent, JSValue *clone,
                                     void *dec_blob, void *pin_blob);
/* AND WHAT THE STEP NAMES ITSELF, whose definition is with the scheduler below. A refusal is a step that
   consumed a record and delivered nothing, and it says so: a mechanism whose work is indistinguishable from
   the work it replaces is one no reader can tell has ever run. */
static const char *g_step_unit;

static void deliver_fork_arm(JSContext *ctx, Flow *f, const char *vec)
{
    char *fork_point = world_vec_fork_point(vec);
    Flow *sib;

    if (fork_point == NULL) return;                    /* a ROOT world: no branch, so no other side to be on */
    if (deliver_committed_at(ctx, f, fork_point)) { free(fork_point); return; }   /* a side already taken */
    free(fork_point);
    DCHECK(f->frame == NULL,
           "a delivery-time fork was taken by a flow INSIDE a program — a delivery is made between programs "
           "(flow_step reaches it only with no frame), so a parent holding one means this ran somewhere else "
           "and the arm would be marked hot with nothing to resume");
    DCHECK(!JS_HasActivation(JS_GetRuntime(ctx)),
           "a delivery-time fork was taken with page code on the stack — the arm re-enters its scheduler step "
           "and re-reaches this delivery, which is only true of a fork asked between tasks");
    sib = engine_sibling_assemble(ctx, f, NULL,
                                  decide_fork_same_path("(a cross-document message arrived from one arm of a "
                                                        "sender's branch — no predicate was asked)"),
                                  concolic_pins_suspend());
    /* THE ONE THING THAT MAKES THE ARM A DIFFERENT TIMELINE, written AFTER the assembly so the arm's inherited
       copy of the record is its parent's as of the instant before this delivery — the arm never received it. */
    flow_world_commit_push(ctx, sib, vec, 0);
}

/* AND THIS TIMELINE'S OWN HALF OF IT: the world it is about to hear from is one it IS in from here on.
   A COMMITMENT ALREADY AT OR BELOW THIS WORLD MAKES THE PUSH REDUNDANT, and that is exact rather than a
   saving. A world's ancestors are totally ordered, so a set of pairwise-comparable worlds is a chain and being
   comparable with its deepest member implies being comparable with every member — a world some existing
   commitment already descends from therefore refuses nothing that commitment does not already refuse. Skipping
   it is what keeps this record O(the sender's distinct worlds) rather than O(the messages it sent), which is
   the difference between a record that grows with the sender's SHAPE and one that grows with its traffic.
   IT IS NOT A SEEN-SET: nothing is refused because of it, and a world arriving a second time is admitted
   exactly as the first was — what is skipped is a duplicate ROW, not a duplicate delivery. The reverse case
   (this world DESCENDS from an entry already here) leaves the shallower entry standing rather than replacing
   it: both are true of this timeline, the deeper one is the one that decides every test, and a removal would
   be a mutation of an Array whose entries the arms of this flow share. */
static void deliver_commit_taken(JSContext *ctx, Flow *f, const char *vec)
{
    int n = flow_world_commits(f), i, subsumed = 0;

    for (i = 0; !subsumed && i < n; i++) {
        JSValue e = flow_world_commit_at(f, i);
        JSValue cv = JS_GetPropertyUint32(ctx, e, 0);
        JSValue tv = JS_GetPropertyUint32(ctx, e, 1);
        const char *c = JS_ToCString(ctx, cv);
        WorldRel rel;

        CHECK(c != NULL, "engine: OOM reading a receiving timeline's commitments while recording a new one");
        rel = world_vec_relate(vec, c);
        if (JS_VALUE_GET_INT(tv) && (rel == WORLD_REL_SAME || rel == WORLD_REL_ANCESTOR)) subsumed = 1;
        JS_FreeCString(ctx, c);
        JS_FreeValue(ctx, cv);
        JS_FreeValue(ctx, tv);
        JS_FreeValue(ctx, e);
    }
    if (!subsumed) flow_world_commit_push(ctx, f, vec, 1);
}

/* THE DELIVERY ITSELF, made by the receiving flow's own step — so it runs with that flow switched in, under its
   delta, and the task it enqueues lands on that flow's own queue like every other job. This is the dispatch on
   the op, and the ONLY place a routed op is turned into a call: an op with no component here is a transport
   carrying something nothing receives. */
/* THE TWO OUTCOMES OF A ROUTED RECORD MEETING A TIMELINE — see engine.h for why neither is readable alone and
   why a host that has only its own routed count cannot state the invariant at all. Counted at the two lines
   below that ARE those outcomes, so the pair cannot drift from the branch it describes. */
static long g_routed_delivered, g_routed_refused;

void engine_routed_census(long *delivered, long *refused) {
    DCHECK(delivered != NULL && refused != NULL,
           "the routed-delivery census was asked for one of its two numbers — a delivery count with no refusal "
           "count beside it cannot say whether the rest of the frontier declined the record or never saw it, "
           "which is the whole of what the pair is for");
    *delivered = g_routed_delivered; *refused = g_routed_refused;
}

/* WHAT BECAME OF THE TASKS THOSE DELIVERIES QUEUED — see engine.h for why one number could not say. Counted
   here rather than in window_message.c because the pair above is counted here and the sum of these four is
   compared against it; two files each holding half of one conservation law is how the halves drift. */
static long g_routed_task_end[ROUTED_TASK_END_N];

void engine_routed_task_end(int end) {
    DCHECK(end >= 0 && end < ROUTED_TASK_END_N,
           "§9.3.3 step 8's task reported an end this census does not name — the ends are declared in engine.h "
           "and a fifth is a path through the delivery task that nothing counts, which is exactly the hole the "
           "census exists to close");
    g_routed_task_end[end]++;
}

void engine_routed_task_census(long *ends) {
    int i;

    DCHECK(ends != NULL,
           "the routed-task census was asked for nothing to write into — all four ends or none, because a "
           "fired count with no declined count beside it cannot tell a delivery the spec refused from one the "
           "scheduler lost");
    for (i = 0; i < ROUTED_TASK_END_N; i++) ends[i] = g_routed_task_end[i];
}

static void flow_deliver(JSContext *ctx, Flow *f)
{
    JSValue entry, rv, ov;
    const char *record, *origin;
    char *dup, *doc, *worlds, *tail, *vec;
    WorldId w;
    const WorldId *anc;
    uint32_t doc_id;
    int n_anc;
    CowDelta *seg;
    JSContext *rctx;

    /* ASSERTED BEFORE THE QUEUE IS TOUCHED, because the take MUTATES: an abort after it would have consumed a
       peer's message on the way out, and the frontier's residue would then be missing the very work item the
       crash is about. */
    DCHECK(flow_running() == f, "a routed delivery was made while another flow was switched in — it would run "
                                "against that flow's delta and its task would land on that flow's queue");
    /* WHOSE TIMELINE THE OLDEST RECORD BELONGS TO IS DECIDED BEFORE IT IS TAKEN, because two of the three
       answers are not a delivery: this timeline may already have foreclosed the sending world, and taking a
       record only to discover that would have consumed it out of the queue the residue is written from. Read,
       decide, and only then consume. */
    entry = flow_deliver_entry(f, 0);
    rv = JS_GetPropertyUint32(ctx, entry, 0);
    record = JS_ToCString(ctx, rv);
    CHECK(record != NULL, "engine: OOM reading a routed delivery's record — a record that cannot be read is a "
                          "peer's message this document never receives");
    vec = record_worlds_dup(record);
    JS_FreeCString(ctx, record);
    JS_FreeValue(ctx, rv);
    JS_FreeValue(ctx, entry);
    if (!deliver_admits(ctx, f, vec)) {
        /* NOT THIS TIMELINE'S MESSAGE, AND THAT IS NOT A DROPPED WORK ITEM. It is a message belonging to the
           other side of a sender branch this timeline has taken a side at, and the flow on that side holds its
           own copy of the same entry (engine_route attaches to EVERY live flow, and a fork hands the arm its
           own Array naming the same entries) — deliver_admits asserts that side exists before answering no.
           Consuming it HERE is what stops this timeline delivering it, and consuming is the whole of the work:
           §9.3.3's ordering is per receiving timeline, and this one is not in the sender's. */
        free(vec);
        entry = flow_deliver_take(ctx, f);
        JS_FreeValue(ctx, entry);
        /* …AND IT IS COUNTED, because "this timeline declined it" and "this timeline was never offered it" are
           the two readings of a delivery count that is lower than a host expected, and they take opposite
           actions (engine.h). */
        g_routed_refused++;
        g_step_unit = "routed-delivery-not-this-timeline";
        return;
    }
    /* THE ARM THAT DOES NOT RECEIVE IT, MINTED BEFORE IT IS RECEIVED — and this timeline's commitment to
       receiving it, in that order, so the arm inherits a record that does not yet name this world. */
    deliver_fork_arm(ctx, f, vec);
    deliver_commit_taken(ctx, f, vec);
    free(vec);
    /* THE OLDEST UNMADE DELIVERY, taken from the front and owned here. ONE per step: the record becomes a
       task at the receiving Window (window_message_route), the step returns, and the next step takes the next
       one — so two posts from one sender enqueue two tasks in the order they were posted, which is what
       §9.3.3's task source guarantees the page. */
    entry = flow_deliver_take(ctx, f);
    rv = JS_GetPropertyUint32(ctx, entry, 0);
    ov = JS_GetPropertyUint32(ctx, entry, 1);
    record = JS_ToCString(ctx, rv);
    origin = JS_ToCString(ctx, ov);
    CHECK(record != NULL && origin != NULL,
          "engine: OOM reading a routed delivery off its queue — a record that cannot be read is a peer's "
          "message this document never receives");
    dup = strdup(record);
    CHECK(dup != NULL, "engine: OOM splitting a routed delivery's transport fields");
    doc = strchr(dup, '\t');    *doc++ = 0;
    worlds = strchr(doc, '\t'); *worlds++ = 0;
    tail = strchr(worlds, '\t'); *tail++ = 0;
    /* THE RECEIVING DOCUMENT'S REALM, which is what "delivered to that document" means: the event is
       constructed in it, `event.source` is minted in it, and the task is enqueued at ITS Window. Delivering
       into this instance's root instead would fire the message at a document the sender never named — a
       message the page never received arriving as if it had, which is the same failure the routing check
       crashes on one level up. */
    /* …AND IT IS ONE THIS AGENT HOLDS, asked HERE and not only at the arrival, because a delivery that came
       off the COLD TIER never passed engine_route in this session and nothing else has checked it. It is not a
       restatement of that check: a resumed flow replays the document from its FIRST SCRIPT, so a record naming
       a CHILD navigable is taken off the queue before the replay has re-created it, and world_doc_intern mints
       a handle for a document nobody has adopted. Said here rather than left to doc_realm one line down, whose
       own DFAIL is about a navigable that EXISTS and has no realm yet and would send the reader to build the
       node-navigable direction — a true instruction for the wrong cause. What this one names is the ORDER: a
       parked delivery has to be made where the replay has reached the point the message arrived at, and today
       it is made before the flow's first program. */
    doc_id = world_doc_intern(doc);
    DCHECK(world_doc_hosted(doc_id),
           "a routed delivery names a document this session does not hold — the record survived the cold tier "
           "but the DOCUMENT it names has not been re-created yet, because a resumed flow replays from its "
           "first script and this delivery is taken off the queue before any of it runs. Make a parked "
           "delivery at the position in the replay where its message arrived, rather than ahead of the "
           "programs that build the navigable it names");
    rctx = doc_realm(doc_id);
    /* THE SENDING DOCUMENT IS THE HEAD OF THE WORLD VECTOR — a world is minted by a flow of exactly one
       document, so the vector already names the sender and a second field for it could disagree with it. */
    n_anc = world_parse(worlds, &w, &anc);
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
    /* THE ONE PLACE WORK ENTERS THIS FLOW FROM OUTSIDE ITS OWN PROGRAM, and the tasks the dispatch below
       enqueues are marked as such. Every other job on a flow's queue is caused by code the flow ran — a
       `.then` it attached, a timer it set, a fetch it issued — so a replay of that code re-causes it, which is
       the whole of cold.h's claim for the queue. A ROUTED RECORD was handed to this timeline by the trusted
       zone; nothing in the replayed program produces it, so the task it becomes is the one work item on this
       queue a recipe does not regenerate. Bracketed here rather than recognised at the park, because WHERE the
       work came from is knowable only at the moment it arrives. */
    /* §9.3.3 "Posting messages" STEP 8 IS ONE TASK, AND THIS IS WHERE THAT BECOMES CHECKABLE. The step ends
       the window post message steps by "queue a global task on the posted message task source given
       targetWindow", singular — so a record that becomes ZERO tasks is a peer's message this document never
       receives, and one that becomes TWO is a page whose handler runs twice for one message. Both are silent
       everywhere else: the page cannot tell a message it never got from one that was never sent, and a second
       handler run is indistinguishable from a second message.
       IT IS AN EQUALITY AND NOT A CEILING, and the reason is that the origin check is NOT here. §9.3.3 step
       8.1's targetOrigin comparison happens inside the delivery task itself (core/frame/window_message.c: "not
       same origin, so nothing is delivered"), because the target may have navigated since the post — so the
       ENQUEUE is unconditional and a delivery that reaches this line always produces exactly one task. A `<=`
       would have been the one-sided form of this assert, and the cost of that form is on record one file over:
       solver/flow.c's arrival rule guarded itself with `<=` while assigning three of the four tags its own
       prose claimed it assigned, and the omission — which a one-sided guard can only ever be satisfied by —
       placed every @S candidate session below the entire frontier for whole runs with nothing firing.
       WHY IT IS ASKED HERE RATHER THAN COUNTED AT THE END. A route that fires a page's handler more often
       than it should is measured today by a host comparing its own routed count against the handler's
       invocations, and that comparison is not a statement about this engine at all: the record is attached to
       EVERY live flow, so N timelines that admit it legitimately produce N invocations. The number a host can
       compare is `g_routed_delivered` below; the DUPLICATE it was trying to catch is this line's. */
    {
        /* READ ON ITS OWN LINE AND VOIDED BELOW, for the reason every other reading this file takes for a
           DCHECK is: the condition vanishes in release, and a value read only inside one is a variable the
           compiler is right to call set-and-unused. */
        int jobs0 = flow_job_pending(f);
        flow_job_external_begin();
        if (!strcmp(dup, "windowproxy.post"))
            window_message_route(rctx, tail, world_doc_name(w.doc), origin);
        else
            DFAIL("a record was routed with an op no component receives — the sending half emits it, so the "
                  "receiving half is the unbuilt one; build it rather than dropping the delivery");
        flow_job_external_end();
        DCHECK(flow_job_pending(f) == jobs0 + 1,
               "a routed delivery did not become exactly ONE task on the receiving timeline's queue — HTML "
               "§9.3.3 Posting messages step 8 queues one global task on the posted message task source, so "
               "zero is a peer's message this document never receives (and cannot know it did not) and two is "
               "one message the page's handler runs for twice, which no count outside this line can tell from "
               "two messages");
        (void)jobs0;
        /* AND THE DELIVERY IS COUNTED WHERE IT HAPPENED — one per task queued, which is one per handler
           invocation, which is the only quantity a host may compare its own routed count's CONSEQUENCES
           against (engine.h). */
        g_routed_delivered++;
    }
    free(dup);
    JS_FreeCString(ctx, record);
    JS_FreeCString(ctx, origin);
    JS_FreeValue(ctx, rv);
    JS_FreeValue(ctx, ov);
    JS_FreeValue(ctx, entry);
}

/* ---- ASKED TO PERFORM ONE, WHICH IS THE HALF WITH AN ANSWER ---------------------------------------------- */

/* ─── THE UNSTARTED-OPERATION FIFO (flow.h's perform_q) ──────────────────────────────────────────────────
 * A JS Array of immutable two-element [record, token] Arrays. Three operations and nothing else, because an
 * entry is never edited after it is pushed: append at arrival, take from the FRONT at the start, and a fork
 * gives an arm its own Array naming the same entries. Every mutation runs inside cow_engine_write_begin/end
 * for the reason pending.h states about the register beside it — this is the SCHEDULER's record about a flow,
 * the host writes it from outside any flow's delta (engine_perform walks every flow), and a delta that
 * captured it would truncate an entry away the moment a sibling switched in. */
static void perform_q_push(JSContext *ctx, Flow *f, const char *record, const char *token) {
    JSValue e = JS_NewArray(ctx);
    CHECK(!JS_IsException(e), "engine: OOM allocating a cross-agent operation record — a dropped record is a "
                              "peer's flow suspended at the read that asked, for the rest of its process");
    cow_engine_write_begin();
    JS_SetPropertyUint32(ctx, e, 0, JS_NewString(ctx, record));
    JS_SetPropertyUint32(ctx, e, 1, JS_NewString(ctx, token));
    if (!JS_IsObject(f->perform_q)) {
        JS_FreeValue(ctx, f->perform_q);
        f->perform_q = JS_NewArray(ctx);
        CHECK(!JS_IsException(f->perform_q), "engine: OOM allocating a flow's operation queue");
    }
    JS_SetPropertyUint32(ctx, f->perform_q, (uint32_t)flow_perform_pending(f), e);
    cow_engine_write_end();
}

/* TAKE THE OLDEST, which is what makes this a queue rather than the register's SET. The asking side parks on
   each operation in turn, so the order they arrived in is the order the answers are expected in; a swap-remove
   here would answer the second question first and the peer would read one timeline's answers as another's.
   The entry is OWNED by the caller. */
static JSValue perform_q_take(JSContext *ctx, Flow *f) {
    int n = flow_perform_pending(f), i;
    JSValue e;

    DCHECK(n > 0, "an operation was taken from a flow whose queue is empty — the caller tested the queue to "
                  "get here, so the two reads have come apart");
    e = JS_GetPropertyUint32(ctx, f->perform_q, 0);
    DCHECK(JS_IsObject(e), "a flow's operation queue held something that is not a [record, token] pair");
    cow_engine_write_begin();
    for (i = 1; i < n; i++)
        JS_SetPropertyUint32(ctx, f->perform_q, (uint32_t)(i - 1), JS_GetPropertyUint32(ctx, f->perform_q, (uint32_t)i));
    JS_SetPropertyStr(ctx, f->perform_q, "length", JS_NewInt32(ctx, n - 1));
    cow_engine_write_end();
    return e;
}

/* THE ARM'S OWN QUEUE — a new Array naming the parent's ENTRIES, for pending_fork's reason exactly: each arm
   takes an entry when IT starts that operation, so two flows sharing one Array would each see the other's
   consumption; the entries themselves never change after they are pushed, so they are shared.
   IT ASKS flow_perform_pending FOR THE LENGTH RATHER THAN READING IT, which is what makes flow.h's claim that
   the queue's shape has ONE reader true. It read the `length` slot itself and took JS_VALUE_GET_INT off it
   WITHOUT the tag check its twin makes one line before the same read — so the one place a non-int length could
   arrive was the one place nothing would have said so, and the arm would have inherited a count read out of a
   float's payload. Two readers of a shape is two readers, and the second is always the one missing the assert.
   AN EMPTY QUEUE FORKS AS JS_UNDEFINED, not as an empty Array: a parent whose entries have all been started
   has nothing to give, and the flow that has never been asked an operation is the common case the tag test
   exists for. perform_q_push mints the Array again if the arm is ever asked one. */
static JSValue perform_q_fork(JSContext *ctx, const Flow *parent) {
    int n = flow_perform_pending(parent), i;
    JSValue out;

    if (n == 0) return JS_UNDEFINED;
    out = JS_NewArray(ctx);
    CHECK(!JS_IsException(out), "engine: OOM forking a flow's operation queue — an arm that lost it runs a "
                                "peer's operation and tells nobody");
    cow_engine_write_begin();
    for (i = 0; i < n; i++)
        JS_SetPropertyUint32(ctx, out, (uint32_t)i,
                             JS_GetPropertyUint32(ctx, parent->perform_q, (uint32_t)i));
    cow_engine_write_end();
    return out;
}


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
        WorldId w;
        const WorldId *anc;
        int n_anc = world_parse(remote_op_worlds(op), &w, &anc);
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
        /* APPENDED, BECAUSE THEY ARE SEQUENTIAL. A second operation arriving before this flow has started the
           first is an ordinary second question — the asking side parks on each in turn — so it goes on the
           queue behind it, and each is answered under its own token because the token rides the program's row
           from the moment the operation starts (flow.h). It is not a fork: two CONTRADICTORY askers would need
           one, and two questions from the same peer in turn do not. */
        perform_q_push(ctx, f, record, token);
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

/* IS THIS A METHOD — Fetch §2.2.1 Methods, "a byte sequence that matches the method token production", whose
   production is RFC 9110 §5.6.2 Tokens. Asked of what the HOST hands back at the provide edge, because the one
   thing a delivery keyed on the pair cannot survive is a key that is not the shape the join emitted: a host
   that sends an ADDRESS where the method goes matches nothing and settles nobody, which is silent.
   NOT `#if APICLIENT_DEV`: a DCHECK's condition still has to TYPE-CHECK in release (check.h compiles it to
   `sizeof(cond)`), so a helper only a DCHECK calls must still be declared there. */
static inline int method_is_token(const char *m) {
    static const char TCHAR_EXTRA[] = "!#$%&'*+-.^_`|~";
    if (!m || !*m) return 0;
    for (; *m; m++) {
        unsigned char c = (unsigned char)*m;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            memchr(TCHAR_EXTRA, c, sizeof TCHAR_EXTRA - 1))
            continue;
        return 0;
    }
    return 1;
}

const char *engine_pending_fetches(void) {
    static char *join;
    static size_t cap;
    size_t n_out = 0;
    Flow *f;

    /* ONE PASS AND ONE CONVERSION, for the reason engine_host_requests states. The DEDUP IS OVER THE PAIR, which
       is what makes it a set of REQUESTS: several flows park on the same request (a candidate re-fire re-runs
       the exploring flow's fetches) and engine_provide fills every entry naming it, so listing it twice makes
       the host provide twice and the second call finds nothing left. Deduping over the URL alone did the
       opposite of that and worse — it deleted the POST from a list that already held the GET, so the host never
       issued it and the engine settled the POST's promise with the GET's body. */
    if (!join) { cap = 256; join = malloc(cap); CHECK(join, "engine: OOM joining the pending requests"); }
    join[0] = 0;
    for (int k = 0; (f = flow_at(k)) != NULL; k++)
        for (int i = 0, n = pending_count(f->pending); i < n; i++) {
            JSValue pe = pending_entry(f->pending, i);
            JSValue uv = pending_get(pe, PEND_URL);
            JSValue mv = pending_get(pe, PEND_METHOD);
            size_t ul = 0, ml = 0;
            const char *u = JS_IsString(uv) ? JS_ToCStringLen(pending_ctx(), &ul, uv) : NULL;
            const char *m = JS_IsString(mv) ? JS_ToCStringLen(pending_ctx(), &ml, mv) : NULL;
            int skip = (!u || pending_get_int(pe, PEND_HAVE_VALUE));
            if (!skip) {
                const char *q = join, *stop = join + n_out;
                /* THE PRODUCER'S HALF, ASSERTED WHERE THE RECORD IS READ rather than where it is written: a park
                   that named an address and no method is a component that dropped a field, and the host would be
                   handed a line the split cannot make sense of. */
                DCHECK(m != NULL,
                       "an outstanding request carries an ADDRESS and no METHOD — the reply seam is keyed on the "
                       "pair, so this entry cannot be listed at all. The park that created it must state its "
                       "method (Fetch §2.2 Requests: unless stated otherwise it is `GET`)");
                /* AND THE GRAMMAR HOLDS BY CONSTRUCTION, WHICH IS WHY IT IS CHECKED. URL Standard §4.4 URL
                   parsing removes all ASCII tab or newline from its input, so a serialized URL has neither; a
                   method is a token and RFC 9110 §5.6.2 Tokens excludes both. An entry that breaks that is a URL
                   that never went through the parser — a concolic SHAPE carried through as an address — and the
                   line it makes silently splits into two records the host then fetches. */
                DCHECK(!memchr(u, '\t', ul) && !memchr(u, '\n', ul),
                       "an outstanding request's URL holds a TAB or a NEWLINE — URL Standard §4.4 URL parsing "
                       "removes both from its input, so this string never went through the parser and the joined "
                       "line splits into records nobody parked on");
                DCHECK(method_is_token(m),
                       "an outstanding request's METHOD is not a token — Fetch §2.2.1 Methods, and RFC 9110 "
                       "§5.6.2 Tokens is the production; the joined line would not split back into the pair");
                while (q < stop) {
                    const char *e = memchr(q, '\n', (size_t)(stop - q));
                    size_t l = e ? (size_t)(e - q) : (size_t)(stop - q);
                    if (l == ml + 1 + ul && !memcmp(q, m, ml) && q[ml] == '\t' &&
                        !memcmp(q + ml + 1, u, ul)) { skip = 1; break; }
                    if (!e) break;
                    q = e + 1;
                }
            }
            if (!skip) {
                while (n_out + ml + ul + 3 > cap) {
                    cap *= 2;
                    join = realloc(join, cap);
                    CHECK(join, "engine: OOM growing the pending-request join");
                }
                memcpy(join + n_out, m, ml); n_out += ml;
                join[n_out++] = '\t';
                memcpy(join + n_out, u, ul); n_out += ul;
                join[n_out++] = '\n';
                join[n_out] = 0;
            }
            if (u) JS_FreeCString(pending_ctx(), u);
            if (m) JS_FreeCString(pending_ctx(), m);
            JS_FreeValue(pending_ctx(), mv);
            JS_FreeValue(pending_ctx(), uv);
            JS_FreeValue(pending_ctx(), pe);
        }
    return join;
}

void engine_pending_split(char *line, const char **method, const char **url) {
    char *tab;

    DCHECK(line && method && url, "a pending line was split with nowhere to put its halves");
    tab = strchr(line, '\t');
    /* A `CHECK`, NOT A DCHECK, because the release path has no defined answer: the two halves are what a reply
       is delivered against, and a line that is not the shape this engine joined would key a delivery on a method
       nobody asked for — which is the wrong answer this seam exists to make impossible, in the one build where
       nothing else would say so. */
    CHECK(tab != NULL,
          "engine: a host split a pending line that carries no TAB — engine_pending_fetches joins "
          "`METHOD<TAB>URL` lines and this one is neither half of that");
    *tab = 0;
    *method = line;
    *url = tab + 1;
}

/* IS ANYTHING OUTSTANDING ON THE WHOLE FRONTIER — the third answer over the same two walks above, and the one
 * the scheduler consults before it may call a frontier exhausted.
 *
 * IT USED TO BE A HOST CALLBACK (`engine_set_stall_hook`), justified as "the scheduler holds no idea of what a
 * reply is, and the host holds no idea of what a flow is". That was false in the only direction that matters:
 * the two lists a host would answer from are engine_pending_fetches() and engine_host_requests(), which are THIS
 * FILE's walks of THIS FILE's registers, so every host implemented the hook by restating an engine fact — and
 * a fact restated in three places is a fact three places can get wrong. One of them did. main.c — the SHIPPED
 * host, the one §Testing says is the one that rots — asked `*engine_pending_fetches() != '\0'` and never the
 * synchronous register, while test_forced.c and wpt_runner.c asked both (wpt_runner's own comment spells out
 * why: "a flow parked on either is a flow that has not finished"). So a frontier whose every member was
 * suspended inside a cross-instance read, with no fetch outstanding, reported NOT STALLED to the extension and
 * to engine/route.mjs: the slice fell straight past the report the host reads, seeded candidates over it and
 * declared the session exhausted, killing every one of those continuations. engine/route.mjs aborts on exactly
 * that, at the `!flow_blocked` assert a few lines past the report — a §7.2.1 `w.length` read, which is the
 * one member of the cross-origin twelve that only the peer instance can answer.
 *
 * ASKED OF THE FLOWS, not of the joins, because the joins BUILD TEXT and this needs a yes or no — and because
 * `pending_outstanding` is already the exact predicate ("is the host still owed anything on this register"),
 * so the two cannot drift the way three copies of a condition did. The DCHECK in the loop is what keeps the
 * two answers the same one: an outstanding entry the host cannot be TOLD about would make this say STALLED
 * over work no host will ever be handed, which is the livelock the callback's failure was the mirror of. */
int engine_host_owes(void) {
    for (int i = 0; i < flow_count(); i++) {
        Flow *f = flow_at(i);
        if (!pending_outstanding(f->pending)) continue;
#if APICLIENT_DEV
        for (int j = 0, m = pending_count(f->pending); j < m; j++) {
            JSValue e = pending_entry(f->pending, j);
            JSValue uv = pending_get(e, PEND_URL);
            int tellable = pending_get_int(e, PEND_HAVE_VALUE) ||
                           JS_IsString(uv) ||
                           pending_get_int(e, PEND_KIND) == FLOW_PENDING_HOSTREQ;
            JS_FreeValue(pending_ctx(), uv);
            JS_FreeValue(pending_ctx(), e);
            DCHECK(tellable,
                   "a flow is waiting on a register entry the host can never be shown — engine_pending_fetches "
                   "lists the unanswered entries that carry an ADDRESS and engine_host_requests the unanswered "
                   "SYNCHRONOUS ones, so an entry that is neither makes this report a stall the host is handed "
                   "no record for, and the frontier waits on it for the rest of the session");
        }
#endif
        return 1;
    }
    return 0;
}

/* Deliver a body for the request `(method, url)` into every flow parked on it. The value lands on the flow's OWN
   pending entry, so the reaction the resolve enqueues belongs to that flow and to its COW delta — which is why
   this is here and not in a register beside it. Returns how many entries it filled.
   THE METHOD IS HALF THE KEY, and this matched on the URL alone. `waiting` was `strcmp(u, url) == 0`, so a page
   that issued a GET and a POST to one address had the first reply written onto BOTH entries and both promises
   settled with it: the POST's flow resumed reading a body the server produced for a request it never made, and
   every @H example, every branch over that body and every @S verdict downstream of it came from that. It is the
   same correction SECURITY.md §Network records for the XHR seam, made at the seam it names as still open. */
int engine_provide(JSContext *ctx, const char *method, const char *url, JSValueConst value) {
    int n = 0, matched = 0;
    DCHECK(url != NULL, "a body was provided for no URL");
    /* WHAT THE HOST SENDS BACK IS WHAT THE JOIN EMITTED, and the shape is asserted rather than assumed: an
       absent method is a host that has not been converted to the pair (the JS bridge's `Provide` carries one),
       and an address in the method's place is the same host with its operands shifted. Both match nothing, and
       matching nothing is silent — the flow that IS parked simply waits for the rest of the session. */
    DCHECK(method != NULL,
           "a reply was provided with NO METHOD — the reply seam is keyed on `(method, url)`; the host that "
           "fetched it is the only zone that knows which request it answers, and it is handed both halves by "
           "engine_pending_fetches. A host still sending an address alone has not been converted");
    DCHECK(method_is_token(method),
           "a reply was provided with a METHOD that is not a token — Fetch §2.2.1 Methods, RFC 9110 §5.6.2 "
           "Tokens. A host sending the URL where the method goes has its operands shifted by one");
    for (int k = 0; ; k++) { Flow *f = flow_at(k); if (!f) break;
        for (int i = 0, m = pending_count(f->pending); i < m; i++) {
            JSValue p = pending_entry(f->pending, i);
            JSValue uv = pending_get(p, PEND_URL);
            JSValue mv = pending_get(p, PEND_METHOD);
            const char *u = JS_IsString(uv) ? JS_ToCString(ctx, uv) : NULL;
            const char *pm = JS_IsString(mv) ? JS_ToCString(ctx, mv) : NULL;
            /* TWO QUESTIONS OF TWO DIFFERENT THINGS, and they were one predicate until a fork stopped copying
               records. `waiting` is about the FLOW — its register names this address, so this delivery is an
               event that can change the answer it gave the scheduler. `fill` is about the RECORD — it has no
               answer on it yet, so this call is the one that writes one. A fork SHARES records (pending.h), so
               ONE record is named by every arm that inherited it and `haveValue` is therefore a fact about the
               record and not about any of them.
               WHAT ASKING ONE QUESTION COST: the first arm reached filled the record and had its host-owed mark
               taken off; every LATER arm naming that same record read `haveValue` as already set, fell out of
               the `hit` branch, and was never un-marked. A marked flow is out of the pick until a host event
               clears it (flow.h) — and no further host event is coming, because its register is fully answered
               — so that timeline left the frontier for good with its whole exploration unrun. The stall assert
               at the end of the slice reports the state one document later ("a member owed the host NOTHING")
               and names the symptom rather than this line; `pin_and_shape.html` reaches it under three of the
               solver gate's four schedules, its `/api/roles` record having been forked between the two arms of
               the `limit > 5` branch that follows the fetch. */
            /* THE KEY IS THE PAIR. An entry with no method is one no join could have listed, so it cannot be
               what this reply answers whatever its URL says. */
            int waiting = u && pm && strcmp(u, url) == 0 && strcmp(pm, method) == 0;
            int fill = waiting && !pending_get_int(p, PEND_HAVE_VALUE);
            matched += waiting;
            if (u) JS_FreeCString(ctx, u);
            if (pm) JS_FreeCString(ctx, pm);
            JS_FreeValue(ctx, mv);
            JS_FreeValue(ctx, uv);
            if (fill) {
                /* THE PRODUCER'S HALF OF THE CONTRACT THE DRAIN CHECKS, asked HERE so the two together say
                   WHICH of two very different things went wrong. The delivery asserts that a fetch entry carries a
                   §2.2.6 URL list at the moment it is delivered; this asserts that it carried one at the moment
                   it was WRITTEN. One assert alone cannot separate "the host built a bad record" from "a good
                   record was changed after it landed" — and the second is a COW/lifetime bug in this file
                   rather than a host bug, with a different fix and a different blast radius. Two asserts, one
                   contract, and whichever fires names the half.
                   Only for the FETCH kind: a docscript, an injected <script src> and a module load are owed
                   BYTES, and their deliveries read `body` off the same record without ever asking for a list. */
#if APICLIENT_DEV
                if ((int)pending_get_int(p, PEND_KIND) == FLOW_PENDING_RESOLVE && JS_IsObject(value)) {
                    JSValue ul = JS_GetPropertyStr(ctx, value, "urlList");

                    DCHECKF(JS_IsArray(ul),
                            "a reply with no `urlList` is being written onto a fetch entry — the HOST built "
                            "this record, so the producer is the trusted zone's reply path and not this "
                            "file's register. request=%s %s", method, url);
                    JS_FreeValue(ctx, ul);
                }
#endif
                pending_set(p, PEND_VALUE, JS_DupValue(ctx, value));
                pending_set(p, PEND_HAVE_VALUE, JS_TRUE);
                n++;
            }
            /* AND EVERY FLOW WAITING ON THIS ADDRESS IS ASKABLE AGAIN — the reply it parked on is on its
               register now, which is the event its host-owed mark was waiting for and the only kind of thing
               that can change the answer it gave the scheduler (flow.h). Outside the fill, because the mark is
               per FLOW and the fill is per RECORD: N arms share one record, one of them writes the answer, and
               all N observe it.
               A CLEAR THAT IS EARLY BY ONE STEP IS THE ONLY WAY THIS CAN BE WRONG, and it is the trade this
               file already takes twice — engine_set_referenced clears the WHOLE frontier's marks on a fact that
               names no flow, and the shared document-script slot inside flow_deliver_one_reply does the same. A
               flow re-marked for some other unanswered entry between two deliveries of this address is picked
               once, reports host-owed again and is re-marked; nothing is dropped, skipped or reordered. A mark
               kept after the fact it rested on has gone costs the flow its entire timeline, which is the
               failure this line exists to make impossible. */
            if (waiting) flow_clear_host_owed(f);
            JS_FreeValue(ctx, p);
        }
    }
    /* LEARNING FROM REPLIES IS THE POINT (CLAUDE.md §Solver), and this is the one point every fetched reply
       crosses exactly once — a URL two flows parked on is answered here once — so the learning happens here
       rather than in the per-flow delivery, where it would run once per waiter. What the body says about the
       addresses the page will go on to fetch (solver/reply_decode.h) is a fact about the SERVER, not about a
       flow's world, so it is not per-flow state and takes no COW capture, exactly as the endpoint surface
       does not.
       AN API'S OWN REJECTION IS READ IN THE TRUSTED ZONE, NOT HERE. `google.rpc.Status` names the endpoint's
       fields, its canonical service and method and the OAuth scopes it wants — but it is a reply to a
       DELIBERATELY MALFORMED REQUEST, which is a request this engine cannot make: its only network edge is
       this register and the host performs a GET through safeFetch. A reader on this side would file whatever
       rejection a GET happened to provoke under the identity of an endpoint nobody probed. It is
       extension/lib/req2proto.js, which issues the probe as the page. */
    if (n) reply_decode_learn(ctx, method, url, value);
    /* A REQUEST ANSWERED TWICE, TOLD APART FROM ONE ANSWERED FOR NOBODY. Every entry naming this request already
       carries a reply, so this call wrote none — and the two numbers are what make that a different failure
       from `n == 0` with nothing matched at all, which is the host's pairing being off and is the CALLER's
       assert (it owns the paged-sale credit that legitimately explains it). This one no credit can excuse: a
       request leaves the join the moment it is answered, so the host was never shown it a second time. The
       shape it catches is a host with TWO lists that overlap — the extension's `GetChunks` names URLs that are
       already parked module loads and would answer each of them again. */
    if (matched && !n)
        DFAILF("a reply was provided for a request every parked entry has ALREADY been answered for — the "
               "join drops a request as soon as it carries a value, so the host is answering one it was shown "
               "once, twice. request=%s %s", method, url);
    return n;
}
/* WHAT KIND OF PROGRAM a queued body is. It is ONE queue because they are one thing — code the page caused to
   run — and the kind decides exactly two questions, both of them at the ends of that program's life: may it
   fail to COMPILE, and does anything read its COMPLETION VALUE. */
/* A CROSS-AGENT OPERATION is a fourth kind and answers those two questions differently from all three: its
   program is the ENGINE's own text, so a compile failure is this engine's bug and not the page's; and its
   completion is not merely read, it is the ANSWER a peer's flow is parked on. */
/* AN EXTERNAL SCRIPT'S ADDRESS, HOLDING THE POSITION ITS PROGRAM WILL RUN AT — the fifth kind, and the only one
   whose payload is not a program yet. The entry carries the URL, the flow WAITS at it (flow_step below), and the
   reply REPLACES the address with the source text and this kind with DYN_PAGE_SCRIPT (flow_deliver_one_reply), after
   which it is an ordinary program of the sequence.
   IT IS WHAT GIVES EVERY DOCUMENT OF THIS AGENT §4.12.1's ORDER. The SESSION's document already had it — a slot
   per script INDEX that the flow stops at — and no other document did: a child navigable's (core/frame/
   navigable.c) and a joined one's (engine_join_document) external scripts could only park on their replies and
   become programs when those DRAINED, which is after everything queued in that pass and in ARRIVAL order among
   the replies. So an inline script written after a parser-blocking `<script src>` ran before the bundle it is
   written after — HTML §13.2.6.4.8 'The "text" insertion mode' blocks the tokenizer and spins the event loop
   until that script is `ready to be parser-executed` — and two ordered externals ran in whichever order the
   network answered. Both were named aborts. What they were missing is a POSITION, and a queue entry IS one. */
typedef enum { DYN_PAGE_SCRIPT = 0, DYN_CANDIDATE, DYN_JAVASCRIPT_URL, DYN_CROSS_AGENT_OP,
               DYN_SCRIPT_SRC } DynKind;

/* Deliver ONE of this flow's answered pending entries (the network completed) — see flow_deliver_one_reply. */
/* Is any of this flow's pending fetches deliverable? A flow with only host-owed entries has no work — it stalls
   rather than spinning on a delivery that would resolve nothing. */
static int flow_pending_ready(const Flow *f) { return pending_ready(f->pending); }

/* HTML §8.1.4.2'S processResponseConsumeBody STEP THAT MAKES SOURCE TEXT — the point at which a fetched BYTE
   SEQUENCE becomes a script's text, and the one this engine ran neither half of. The bytes used to go from the
   host's reply record to the compiler unchanged: one byte no UTF-8 sequence contains made a whole minified
   bundle a SyntaxError no browser produces (and every endpoint in that chunk was lost with it), while a
   three-byte surrogate compiled into a string where Encoding's decoder answers U+FFFD.
   WHICH ALGORITHM RUNS IS §8.1.4.2's OWN SPLIT and not a parameter of one algorithm: a classic script decodes
   with the response's `Content-Type` charset as a LABEL (Fetch §3.5's legacy extract an encoding, then Encoding
   §6.1's decode, whose BOM sniff can overrule that label), and a module script is UTF-8 whatever the response
   says. `doc_ctx` is the realm of the document the program belongs to, and it is read ONLY on the classic arm —
   §4.12.1 says so about the element too: "if el's type is `module`, this encoding will be ignored."
   AND `bodyBytes` IS NOW ACTUALLY BYTES. This read was `JS_ToCStringLen` over a record field that every
   producer had already run a decode to build — the extension's `resp.text()` and, one step earlier, C's own
   `JS_NewStringLen` — so the classic entry's whole reason to exist, honouring the response's charset LABEL,
   was being asked to honour it over bytes that label had never touched. §2.2.5's body is a byte sequence and
   crosses as one (fetch.h); this reads it back without a transform in between.
   Answers malloc'd source text the caller frees; `*out_n` is its length. */
static char *reply_source_text(JSContext *ctx, JSValueConst reply, ScriptType stype, JSContext *doc_ctx,
                               size_t *out_n)
{
    JSValue bv = fetch_reply_body(ctx, reply);
    size_t body_len = 0;
    const uint8_t *body = fetch_body_bytes(ctx, bv, &body_len);
    char *src;

    if (stype == SCRIPT_TYPE_MODULE) {
        src = script_fetch_module_source_text((const char *)body, body_len, out_n);
    } else {
        HeaderList hl = { 0 };
        char *content_type;

        fetch_reply_header_list(ctx, reply, &hl);
        content_type = header_list_get(&hl, "content-type");   /* NULL is Fetch's "values is null" = failure */
        src = script_fetch_classic_source_text((const char *)body, body_len, content_type,
                                               document_encoding(doc_ctx), out_n);
        free(content_type);
        header_list_free(&hl);
    }
    JS_FreeValue(ctx, bv);
    return src;
}

/* WHAT THE PROGRAM QUEUE CAN CARRY IS `len` BYTES, WHICHEVER OF THEM ARE NUL. A `REPLY_SOURCE_WHOLE` macro
   stood here and asserted `strlen(src) == n` at each of the two decodes below, naming the fix — "build the
   queue over a LENGTH". It is built: `dyn_body_adopt` takes the decode's own `(src, src_n)`, the row keeps
   both, and flow_step hands both to JS_FlowNew / JS_FlowEvalModule, which have taken a length all along. The
   assertion is DELETED rather than relaxed, because what it was guarding no longer has a way to go wrong here:
   the length never becomes a `strlen` between this line and the compiler. The invariant that survives is
   dyn_body_adopt's, and it is about the GUARD byte rather than the length.
   THE MEASUREMENT THAT MADE THIS URGENT: over a 30-site frozen mirror, two of the three sites in this defect's
   cluster aborted HERE — one shipping 125 U+0000s in a single script, the other 2309. A page whose script
   carries one is not a broken page (ECMAScript §11.1 "Source Text"), and running it to the first NUL is a
   silently shorter program with every endpoint and sink past that byte unreachable. */

/* A REPLY THAT IS A PROGRAM JOINS THE SEQUENCE, which is why the delivery below reaches the queue that is
   defined past it. Declared rather than moved: the queue belongs beside the other queue entry points and
   the delivery belongs beside the register it scans. */
static void engine_queue(uint32_t doc, const char *body, size_t body_n, DynKind kind, ScriptType stype,
                         const char *url, DynPos pos);
/* …and the same entry for a row an ELEMENT put there — see engine_queue_el below. */
static void engine_queue_el(uint32_t doc, const char *body, size_t body_n, DynKind kind, ScriptType stype,
                            const char *url, DynPos pos, lxb_dom_element_t *el);
/* …and the one a caller reaches when it ALREADY holds the decoded text as a shared body, which is every reply
   that arrives as a program: the delivery below adopts the decode and hands it over without a second copy. */
static void engine_queue_el_body(uint32_t doc, DynBody *body, DynKind kind, ScriptType stype, const char *url,
                                 DynPos pos, lxb_dom_element_t *el);

/* ONE ANSWERED ENTRY, THEN RETURN — and the loop below is the SEARCH for it, not a drain.
 *
 * IT WAS A DRAIN, and the drain settled every answered entry in one pass. That is the shape §Every runtime job
 * is a scheduler flow forbids in as many words — "There is NO `while(JS_ExecutePendingJob)` loop — the
 * scheduler IS the job pump" — and flow_step's own header says the same thing about itself ("flow_step is a
 * step, and it used to be a drain"). This was the last drain left inside it.
 *
 * AND IT REORDERED THE PAGE'S MICROTASKS, which is the reason it could not merely be tidied. Every delivery
 * below runs through JS_CallAsFlow, which builds a CALL-ROOT FLOW: the native resolving function is a step
 * machine and offers a park at every re-entry, and 27.5.1.3 "CreateResolvingFunctions ( toResolve )"'s resolveSteps step 9's
 * `Get(resolution, "then")` is a read on an object whose prototype the page owns, so the settle of reply A can
 * PARK part-way. The drain did not stop for that. It went on and delivered reply B, whose settle ran to
 * completion — so B's promise reached 27.5.1.4 "FulfillPromise" step 7's TriggerPromiseReactions FIRST, and
 * 27.5.1.8 "TriggerPromiseReactions ( reactions, arg )" step 1.b enqueued B's reaction jobs ahead of A's.
 * §9.5.5 "HostEnqueuePromiseJob ( job, realm )" then requires that "Jobs must run in the same order as the
 * HostEnqueuePromiseJob invocations that scheduled them", and the queue honours that faithfully — so the
 * page's `.then` handlers ran B before A, in an order the forced preempt alone decided. A park is only ever
 * legal because it is TRANSPARENT (flow_park says so, and JS_CallInternal, JS_ExecutePendingJob and
 * JS_FlowFree each assert it); here the transparency was broken not by the park but by the walk continuing
 * across it.
 *
 * IT WAS UNREACHABLE UNTIL THE PARK RECORD MOVED ONTO THE BASE. While the runtime held ONE park slot, the
 * second park aborted at flow_park before the ordering question could be asked at all. The runtime owns a FIFO
 * of them now, so the walk ran to the end and the reordering became reachable — which is what makes this the
 * moment for the fix rather than a defect that was always live.
 *
 * ONE PER STEP IS ALSO WHAT THE EVENT LOOP DOES. A completed fetch is delivered by a TASK, and HTML §8.1.7.3
 * "Processing model" performs a microtask checkpoint at the end of each one — so two replies are two tasks
 * with a checkpoint between them, never one pass that settles both. flow_step's arms are that model: the
 * parked continuation is resumed by the arm at the top of the loop before anything else this flow could do,
 * and flow_checkpoint_due runs the microtasks the completed settle enqueued, both of them BEFORE control
 * reaches this function again for the next reply. Nothing is dropped, starved, skipped or forgotten: the entry
 * stays on the register until the step that delivers it, and the flow reports progress, so it is re-ranked and
 * comes back for the rest exactly as it comes back for its programs and its jobs.
 *
 * THE FIRST ANSWERED ENTRY IN REGISTER ORDER, which is a property worth having on purpose: delivery order is
 * then a function of the order the flow ISSUED its requests and not of the order the host happened to answer
 * them, which is the invariance §Testing's solver differential asks for ("replies answered tail-first must
 * emit the same findings"). */
static void flow_deliver_one_reply(JSContext *ctx, Flow *f) {
    int i = 0;
    while (i < pending_count(f->pending)) {
        JSValue p = pending_entry(f->pending, i);
        JSValue pv;
        int kind;
        if (!pending_get_int(p, PEND_HAVE_VALUE)) {   /* the host still owes this one */
            JS_FreeValue(ctx, p);
            i++;
            continue;
        }
        /* AND A SYNCHRONOUS REQUEST'S ANSWER IS NOT THIS WALK'S TO TAKE, which is the same sentence the `else`
           branch below states as an assertion — said HERE, where the entry is still on the register, because
           there is no way to state it below without the answer having already been removed. `pending_ready`
           decides WHETHER this runs and asks the same two questions this scan asks (solver/pending.h); this
           decides which entry it TOUCHES once it is running, and both are needed: a register can hold a fetch
           reply and an answered HOSTREQ at the same instant, and it is the fetch reply that brought the scan
           here. The answer stays where the machine parked at the call site will take it (engine_host_take), so
           it is skipped exactly as an unanswered entry is — never swap-removed, or the flow that asked resumes
           into a rendezvous whose record is gone and waits at that line for the rest of the session. */
        if (pending_get_int(p, PEND_KIND) == FLOW_PENDING_HOSTREQ) {
            JS_FreeValue(ctx, p);
            i++;
            continue;
        }
        /* NO ENTRY IS DELIVERED WHILE A SETTLE OF THIS REGISTER IS STILL PARKED — the invariant this function
           was rewritten around, asserted where the next delivery would begin rather than left as a property
           somebody maintains. A settle that parked has not reached 27.5.1.4 "FulfillPromise" step 7 yet, so its
           reactions are not on the queue; anything delivered in front of it enqueues first and §9.5.5
           "HostEnqueuePromiseJob ( job, realm )" then runs them in that order — the page observes an order the
           preempt chose. Both homes of the park are asked, because a park rides the Flow while it is switched
           OUT and sits in the runtime's FIFO while it is switched IN, and an assert that can only be true is
           the read-side of the defect a defaulted field is (this is the form flow_answer_fork and flow_finish
           already use). It cannot fire from THIS function's own structure — the delivery below is the last
           thing it does — so what it catches is a caller that reached here with a continuation outstanding,
           which is exactly the state flow_step's resume arm exists to make impossible. */
        DCHECK(!JS_HasParkedFlow(JS_GetRuntime(ctx)) && f->parked == NULL,
               "a reply is about to be delivered while a settle from this flow's register is still PARKED — "
               "the parked settle has not triggered its promise's reactions yet, so this delivery's reactions "
               "would be enqueued in front of them and the page would observe an order the forced preempt "
               "chose. Resume the parked continuations first (flow_step's resume arm)");
        /* AND UNDER THIS FLOW'S OWN DELTA, because the delivery below runs the PAGE's code: the settle would
           otherwise read and write whichever world happened to be applied, and the reaction jobs it enqueues
           would land on that flow's queue. flow_deliver asserts the same thing about a routed message for the
           same reason. */
        DCHECK(flow_running() == f,
               "a reply was delivered while another flow was switched in — the page's resolving function would "
               "run against that flow's delta and the reactions it triggers would be queued on that flow");
        /* TAKEN OFF THE REGISTER BEFORE IT IS DELIVERED, and that ordering is the record's own lifetime. The
           delivery below runs the PAGE's code — 27.5.1.3 "CreateResolvingFunctions ( toResolve )"'s resolveSteps step 9 reads
           `Get(resolution, "then")` off an object whose prototype the page owns — and that code can issue
           another fetch, which appends to this very register. As a C array the walk held a `FlowPending *` into
           storage the append could realloc out from under it; as a JS record the reference here is what keeps
           it alive, so an append cannot move it and the slot it occupied cannot be read twice. The removal is a
           swap-remove and the scan does not resume past it: this call ends at the delivery, and the next step
           scans from the front again over a register the delivery may have changed. */
        pending_remove(&f->pending, i);
        kind = (int)pending_get_int(p, PEND_KIND);
        pv = pending_get(p, PEND_VALUE);
        if (kind == FLOW_PENDING_DOCSCRIPT) {
            /* AN EXTERNAL SCRIPT OF SOME DOCUMENT OF THIS AGENT, AT ITS POSITION IN THIS FLOW'S SEQUENCE
               (DYN_SCRIPT_SRC). ONE branch, where there were two: the session document's own external scripts
               used to be filled into a slot SHARED by every flow, with a whole-frontier clear behind it,
               because the sequence they lived in was the document's rather than any flow's. They are rows of
               this flow's own table now, so this reply fills THIS timeline's row — and the sharing that branch
               provided is already provided one layer down: engine_provide fills every flow whose register names
               the address and un-marks each of them, so one host fetch still answers every waiting member. */
            int di = (int)pending_get_int(p, PEND_SCRIPT_I);
            size_t src_n = 0;
            char *src;
            DCHECK(di >= 0 && di < f->dyn_n,
                   "an external document script replied for a sequence position this flow does not have — the "
                   "entry was queued on one flow and the reply is being delivered into another");
            DCHECK(f->dyn_cand[di] == DYN_SCRIPT_SRC,
                   "an external document script replied for a sequence position that is not awaiting one — the "
                   "slot holds a program already, so this reply is a second answer to one request and the "
                   "program it overwrites would never run");
            /* WHICH OF §8.1.4.2 "Fetching scripts"'S TWO DECODES RUNS IS THE ROW'S OWN ANSWER. It used to be
               hardcoded CLASSIC here, and that was only ever true because the two seams that queue these rows
               (core/frame/navigable.c and engine_join_document) ABORTED on a `<script type=module>` rather than
               queue one — so the hardcode was a restatement of a gap, not a fact about these entries. With the
               row carrying its type, a module's bytes are UTF-8 whatever the response says and a classic
               script's go through the response's charset label. The encoding realm is the ENTRY's document's. */
            DCHECK(script_type_executes((ScriptType)f->dyn_type[di]),
                   "an external document script replied for a row whose type executes nothing — only a "
                   "`<script>` element of an executing type ever becomes a DYN_SCRIPT_SRC row, so this row's "
                   "type column was written by something that is not one of the two seams that queue them");
            src = reply_source_text(ctx, pv, (ScriptType)f->dyn_type[di], doc_realm(f->dyn_doc[di]), &src_n);
            CHECK(src, "engine: OOM storing an external document script");
            /* THE ADDRESS IS TAKEN OUT OF THE BODY COLUMN RATHER THAN DISCARDED, and this is the ONE moment at
               which it can be: the row's body IS the URL until this line, and everything the address decides
               happens after it. §8.1.4.2 creates the script with the RESPONSE'S URL as its base — "let script
               be the result of creating a classic script given sourceText, settingsObject, response's URL,
               options, mutedErrors, and url" — so a nested `import('./chunk.js')` inside a bundle served from
               `/assets/app.js` resolves to `/assets/chunk.js`, and for a MODULE that address is additionally
               the module map KEY. Dropped here instead, two `<script type=module src>` of one document were
               named by the document they share and were therefore ONE module: the second found the first's
               record and evaluated nothing.
               IT IS COPIED, WHERE IT USED TO BE A POINTER MOVE. The move was right while the body column was
               this flow's own `char *`; the text is SHARED now (solver/dyn_body.h), so a fork of a flow parked
               on this row leaves both arms naming the same address buffer and each takes its own row — what
               this row keeps has to be its own. An address is tens of bytes; the megabyte is on the line
               after it, and that one is ADOPTED rather than copied. */
            DCHECK(f->dyn_url[di] == NULL,
                   "an external document script's row already held an address before its reply arrived — the "
                   "row's body is its URL until this delivery takes it, so a second one means the row was queued "
                   "with an address column it may not have or a reply was delivered into it twice");
            {
                /* THE ROW NAMES A LIVE BODY AT EVERY POINT OF THIS SWAP, which is why the replacement is built
                   BEFORE the old one is released rather than after: between an unref and the store the column
                   would hold a pointer whose buffer is gone, and anything that walked this flow's rows in
                   between — a census, a release, a sale of the frontier's tail — would read it. */
                DynBody *nb = dyn_body_adopt(src, src_n);   /* the decode above already made this buffer */
                CHECK(nb, "engine: OOM adopting an external document script's source text");
                /* AND THE ONE PLACE A BODY IS READ AS A C STRING IS THE ONE PLACE THAT SAYS SO. A DYN_SCRIPT_SRC
                   row's body is its ADDRESS, and an address is a NUL-terminated string every URL API takes —
                   which is exactly the read the rest of this file no longer makes of a body (a PROGRAM is
                   (text, len) and may hold a U+0000). The two shapes live in one column, so the assertion is
                   HERE, at the read that assumes the narrower one: an address whose stored length is not its
                   `strlen` is a body that got into this column by some route other than the two that queue an
                   address, and the base URL every nested `import()` of this bundle resolves against would be a
                   prefix of it. */
                DCHECK(strlen(dyn_body_text(f->dyn[di])) == dyn_body_len(f->dyn[di]),
                       "an external document script's row holds an address with a U+0000 in it — the row's "
                       "body is its URL until this delivery takes it across, and a URL is the one body in this "
                       "column that is read as a NUL-terminated string, so this row was filled by something "
                       "that is not engine_queue_docscript_url or the seed");
                f->dyn_url[di] = strdup(dyn_body_text(f->dyn[di]));
                CHECK(f->dyn_url[di], "engine: OOM keeping an external script's address as its base URL");
                dyn_body_unref(f->dyn[di]);
                f->dyn[di] = nb;
            }
            f->dyn_cand[di] = DYN_PAGE_SCRIPT;
        } else if (kind == FLOW_PENDING_SCRIPT) {
            /* the reply is PROGRAM: it joins this flow's script sequence, and the one BFS runs it */
            /* AN INJECTED `<script src>` IS A CLASSIC SCRIPT, which is a statement about this entry and not a
               default: the element was inserted by page code and its reply becomes one of this flow's programs,
               and every program a FLOW adds is compiled classic (see the compile below). The document is the one
               the element was inserted into — the park recorded it — so the encoding that decodes these bytes is
               that document's and not the session's. */
            uint32_t doc = (uint32_t)pending_get_int(p, PEND_DOC);
            ScriptType st = (ScriptType)pending_get_int(p, PEND_SCRIPT_TYPE);
            JSValue uv = pending_get(p, PEND_URL);
            const char *u;
            size_t src_n = 0;
            char *src;
            DCHECK(script_type_executes(st),
                   "an injected <script src> replied for a park carrying a type that executes nothing — the "
                   "park writes the element's type and §4.12.1.1 fetches for exactly the two that run, so a "
                   "third answer means this record was pushed by something that is not that park");
            /* THE STRING AND NOT WHATEVER COERCES TO ONE: `JS_ToCString` answers "undefined" for an absent
               field, and an address of "undefined" is a base URL the parser would accept and every nested
               specifier would then resolve against. The park writes a real string, so a record without one is
               a record something else pushed. */
            DCHECK(JS_IsString(uv),
                   "an injected <script src> replied for a park with no address — the park writes the resolved "
                   "URL and the reply is matched on it, so a record without one names no script and the "
                   "program below would be created with its document's base URL as its own");
            u = JS_ToCString(ctx, uv);
            CHECK(u != NULL, "engine: OOM reading an injected script's address off its park");
            src = reply_source_text(ctx, pv, st, doc_realm(doc), &src_n);
            /* THE SAME `CHECK` THE DOCUMENT-SCRIPT BRANCH ABOVE MAKES, which this branch did not: the decode
               answers NULL when it cannot allocate, and the adopt below would take a NULL buffer. */
            CHECK(src, "engine: OOM storing an injected script");
            /* §8.1.4.2's created script is based on the RESPONSE'S URL, so the program carries the address its
               bytes came from — the base a nested `import('./chunk.js')` resolves against and, for a module,
               the module map key that keeps two injected chunks two modules. */
            {
                /* …AND THE ELEMENT THE PARK CARRIED, back onto the row. §4.12.1.1's "execute the script
                   element" runs when the sequence reaches this row, and the element is what it switches on. */
                JSValue ev = pending_get(p, PEND_SCRIPT_EL);
                lxb_dom_node_t *en = node_of(ev);
                /* ADOPTED RATHER THAN COPIED. This queued the decoded text and then freed it, so an injected
                   chunk cost the instance two of itself for the length of one call; the body is shared now
                   (solver/dyn_body.h) and the row is its first holder. */
                DynBody *b;
                DCHECK(en != NULL && en->type == LXB_DOM_NODE_TYPE_ELEMENT,
                       "an injected <script src> replied for a park carrying no element — the park writes the "
                       "element's wrapper and this is the only reader, so a record without one is a record "
                       "something that is not that park pushed");
                b = dyn_body_adopt(src, src_n);
                CHECK(b, "engine: OOM adopting an injected script's source text");
                engine_queue_el_body(doc, b, DYN_PAGE_SCRIPT, st, u, DYN_POS_APPEND,
                                     lxb_dom_interface_element(en));
                dyn_body_unref(b);
                JS_FreeValue(ctx, ev);
            }
            JS_FreeCString(ctx, u);
            JS_FreeValue(ctx, uv);
        } else if (kind == FLOW_PENDING_MODULE) {
            /* the reply is a MODULE's SOURCE: settle the load's promise with the text, and the import's own
               reaction compiles, links and continues the flow that wrote `await import(...)`. The promise is
               settled with the SOURCE TEXT and not with the reply's bytes — "let sourceText be the result of
               UTF-8 decoding bodyBytes" is the step that stands between them, and the compiler on the other side
               of this promise is the one that refuses an ill-formed byte. */
            size_t src_n = 0;
            char *src = reply_source_text(ctx, pv, SCRIPT_TYPE_MODULE, ctx, &src_n);
            JSValue sv = JS_NewStringLen(ctx, src, src_n);
            JSValue resolve = pending_get(p, PEND_RESOLVE);
            free(src);
            if (JS_CallAsFlow(ctx, resolve, sv) < 0) {
                JSValue exc = JS_GetException(ctx);
                JS_FreeValue(ctx, exc);   /* a rejected load is the page's to observe, not this step's */
            }
            JS_FreeValue(ctx, resolve);
            JS_FreeValue(ctx, sv);
        } else {
            /* A SYNCHRONOUS ANSWER IS TAKEN, NEVER DRAINED, and that is asserted here because this branch is
               where it would land if it were not. The machine that asked resumes through its park and consumes
               the answer with engine_host_take; the entry is gone before any delivery sees it. A HOSTREQ that
               reached this line would be settled as if it were a fetch — through a `resolve` capability it does
               not have, since nothing on that path ever made a promise. */
            DCHECK(kind == FLOW_PENDING_RESOLVE,
                   "a synchronous host request's answer reached the reply delivery — its asking machine never "
                   "resumed to take it, so its parked continuation is the thing to look for");
            /* AS A FLOW, not a JS_Call. The delivery settles the page's promise, and 27.5.1.3's resolveSteps "Promise
               Resolve Functions" step 9 reads `Get(resolution, "then")` off the Response — an ordinary object
               whose prototype the page owns, so `Object.prototype.then = { get(){…} }` makes that read the
               page's code. Out of a plain call it ran in a C activation with no flow base, which is the
               drive-to-completion this engine aborts on; prototype pollution is a gadget class the solver
               exists to RUN rather than assume away.
               IT IS ALSO WHERE THE SETTLE CAN PARK, which is why this function delivers exactly one entry: the
               resolving function is a step machine that RESTS at that read (js_promise_resolvefn_step's
               PRF_THEN), so a forced preempt suspends the settle there with the promise still pending. */
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
            /* AND IT IS A REPLY RECORD, ASKED HERE RATHER THAN THREE FRAMES LATER. `JS_IsObject` was the whole
               of the shape test, and an object is exactly what the two WRONG values on this path also are — a
               peer's `{body, csp}` navigation answer, a pending entry read for the wrong field. The delivery
               machine then reads §2.2.6's URL list off it, finds nothing, and aborts in fetch.c with the record
               in hand but NO WAY TO SAY WHICH ENTRY PRODUCED IT: not the URL, not the kind, not the flow. That
               is what made a payment-schedule change read as a fetch bug.
               `urlList` is the field to ask for because it is the one only the trusted zone can answer and the
               one every producer of this record must therefore fill (fetch.h) — a record missing it was not
               built by fetch_reply_new, whoever built it. Asked in a DEV block rather than inside the DCHECK's
               condition, because the read allocates and a condition that is unevaluated in release may not own
               a reference nothing frees. */
#if APICLIENT_DEV
            if (JS_IsObject(pv)) {
                JSValue ul = JS_GetPropertyStr(ctx, pv, "urlList");
                JSValue uv2 = pending_get(p, PEND_URL);
                const char *u2 = JS_IsString(uv2) ? JS_ToCString(ctx, uv2) : NULL;
                char fields[320];
                size_t fi = 0;
                JSAtom cn = JS_ATOM_NULL;
                const char *cns = NULL;
                /* AND WHAT THE RECORD ACTUALLY IS, because "it has no urlList" names a hole and the FIELDS name
                   the object. Every candidate on this path is identifiable by its own property list in one
                   glance and by nothing else: `body,csp` is the peer's §7.4 navigation answer, `resolve,value,
                   url,…` is a pending ENTRY that reached the value slot, `urlList,status,…` minus the list is a
                   record that was built right and CHANGED afterwards, and a bare `{}` is an object nobody
                   filled. Guessing between those cost a full round trip already.
                   AND THE CLASS, WHICH IS THE SPLIT THE FIELD LIST CANNOT MAKE. `tag=-1` is JS_TAG_OBJECT, so
                   an EMPTY field list does not mean "not an object" — it means an object with no own string
                   properties, and that is the signature of an entire FAMILY rather than of one mistake: a
                   platform object keeps its whole state in a C opaque and has none by design (a Response, a
                   Promise, a Headers), while a plain `Object` with none is either one nobody filled or one
                   whose shape is no longer what it was. The first is a Fetch question and the second is a
                   MEMORY question, and they have nothing in common but the symptom. The pointer rides along so
                   the address can be matched against an allocator report. */
                if (JS_IsObject(pv)) {
                    cn = JS_GetClassName(JS_GetRuntime(ctx), JS_GetClassID(pv));
                    cns = JS_AtomToCString(ctx, cn);
                }
                /* THE FIELD LIST IS DATA, so it is composed as data and the assert's own message is a format
                   like every other. It is the one thing here with no bound of its own — an object can carry
                   any number of own properties — so it says WHERE it stopped rather than ending on whichever
                   name happened to fit, which would read as the complete list of a different object. */
                memcpy(fields, "(not an object)", 16);
                if (JS_IsObject(pv)) {
                    JSPropertyEnum *tab = NULL;
                    uint32_t pn = 0, pi;
                    fields[0] = '\0';
                    if (JS_GetOwnPropertyNames(ctx, &tab, &pn, pv, JS_GPN_STRING_MASK) == 0) {
                        for (pi = 0; pi < pn; pi++) {
                            const char *nm = JS_AtomToCString(ctx, tab[pi].atom);
                            int w = snprintf(fields + fi, sizeof fields - fi, "%s%s", pi ? "," : "",
                                             nm ? nm : "?");

                            if (nm) JS_FreeCString(ctx, nm);
                            if (w < 0 || (size_t)w >= sizeof fields - fi) break;
                            fi += (size_t)w;
                        }
                        if (pi < pn) {
                            const char more[] = ",…";
                            size_t at = fi + sizeof more <= sizeof fields ? fi : sizeof fields - sizeof more;

                            memcpy(fields + at, more, sizeof more);
                        }
                        JS_FreePropertyEnum(ctx, tab, pn);
                    }
                    if (pn == 0) memcpy(fields, "(none)", 7);
                }
                DCHECKF(JS_IsArray(ul),
                        "a fetch reply carrying no `urlList` is about to be delivered — the record on "
                        "this entry was not built by fetch_reply_new, so some other writer reached a "
                        "FLOW_PENDING_RESOLVE entry. url=%s kind=%d tag=%d class=%s ptr=%p fields=%s",
                        u2 ? u2 : "(none)", kind, (int)JS_VALUE_GET_TAG(pv),
                        cns ? cns : "(not an object)",
                        JS_IsObject(pv) ? JS_VALUE_GET_PTR(pv) : NULL, fields);
                if (cns) JS_FreeCString(ctx, cns);
                if (cn != JS_ATOM_NULL) JS_FreeAtom(ctx, cn);
                if (u2) JS_FreeCString(ctx, u2);
                JS_FreeValue(ctx, uv2);
                JS_FreeValue(ctx, ul);
            }
#endif
            if (JS_CallAsFlow(ctx, resolve, pv) < 0) {
                JSValue exc = JS_GetException(ctx);
                JS_FreeValue(ctx, exc);   /* a rejected delivery is the page's to observe, not this step's */
            }
            JS_FreeValue(ctx, resolve);
        }
        JS_FreeValue(ctx, pv);
        JS_FreeValue(ctx, p);
        /* ONE, AND THE STEP IS OVER. The settle above may have PARKED (the resolving function is a step
           machine, and the `then` read is the page's code), and that is precisely the state in which nothing
           else may run: flow_step's next pass resumes the continuation before anything else this flow could
           do. It may equally have completed and enqueued reactions, and then the checkpoint arm runs them
           first — which is what §8.1.7.3 "Processing model" does at the end of the task that delivered it. */
        return;
    }
    /* AND THE TWO QUESTIONS AGREE, asserted at the one point they can disagree. `pending_ready` is what both
       call sites consult to decide that this function has something to do, and the scan above is what finds
       it; they ask the same pair of things about the same register with nothing running in between, so
       reaching here means one of them has been changed and the other has not — and the symptom of that would
       be a flow reporting progress every step while delivering nothing, which is a livelock that looks exactly
       like slowness. */
    DFAIL("a reply delivery found nothing to deliver — pending_ready answered that this flow had an answered "
          "non-HOSTREQ entry and the scan over the same register did not find one");
}

/* Snapshot-fork handoff: solver_decide stashes the sibling's hot decision + pins here at a forking branch;
   the interpreter then clones the frame and calls engine_fork_finalize, which assembles the sibling flow. */
static void *g_fork_dec = NULL, *g_fork_pins = NULL;
/* AND WHO IS ASKING, WHICH IS THE HALF THE SEAM COULD NOT SEE. The interpreter and the step driver both hold a
   resume point and clone it a moment after they ask, and a plain C body holds none — but they call the SAME
   two symbols, so nothing at the seam could tell them apart and every C-body fork was stashed for a consumer
   that never came. engine.c installs the hooks, so engine.c is where the difference can be stated: the two
   wrappers below raise this across their call and nothing else does. It is a declaration by the one caller
   that can make it, not an inference from the flow's state — and it is not a routing table either, because
   there is nothing to look up: raised means "a clone is coming", clear means "there is no activation here". */
static int g_fork_snapshot_owed;
/* …AND WHETHER THIS SESSION FORKS AT ALL — the explore/verify bit, written beside JS_SetFlowControlHooks. */
static int g_sess_forking;
/* AND IT IS READ BY THE SEAM ITSELF, which is what turns the assert below from a report into a mechanism. The
   hook table carries the policy into the INTERPRETER and into the step driver, and both already have their own
   non-forking answer for the absence of a hook (-1 then ToBool, and outcome 0). A caller that asks the decision
   seam BY SYMBOL consults neither, so decide.c has to be able to ask — and it asks HERE, at the one place the
   session's policy is written, rather than being handed a second copy of the bit that could disagree with the
   hook table. It is not a routing predicate: it selects no implementation, it answers a question about the
   session. */
int engine_session_forks(void) { return g_sess_forking; }
/* (engine_sibling_assemble is declared above the delivery fork, which is the first of its three callers in
   this file; a second declaration here would be a second place to keep its signature in step.) */
/* THE HANDOFF IS FILLED AND EMPTIED WITHIN ONE FORK, AND THAT IS ASSERTED HERE RATHER THAN HOPED FOR. Two
   pointers held between a `prepare` and a `finalize` are a slot with exactly one legal occupant: a second
   prepare arriving with the first still in it means the interpreter took the FORKED bit and never reached its
   fork hook, and the assignment then overwrites a decision blob and a pin blob that nothing else names — a leak
   per unconsumed fork, of the shared decision chain reference the sibling was going to stand on, so the whole
   frozen prefix under it stays alive too. It stays as the BACKSTOP it always was; what it can no longer be is
   the diagnosis, because the two ways to reach it without a consumer now crash where they are born. */
int engine_prepare_fork(JSContext *ctx, void *dec_blob, void *pin_blob, const char *asked, int restartable) {
    Flow *f = flow_running();

    DCHECK(f != NULL, "a sibling was prepared with no flow running — the blobs were frozen off whatever "
                      "decision state the last switched-in flow left behind, so they describe no timeline");
    DCHECK(g_sess_forking,
           "a fork was prepared inside a NON-FORKING session — §@S's candidate re-fire is ONE concrete path, "
           "so no member of the frontier may be minted from inside one. The seam now ASKS the session "
           "(engine_session_forks) and answers a new question with the arm the SITE declared, so reaching this "
           "line means the answer was declared and the seam forked anyway — decide_arm's non-forking branch is "
           "the one that must have been taken");
    if (!g_fork_snapshot_owed) {
        /* NOBODY IS GOING TO CLONE A FRAME, so the sibling's resume point has to be the flow's own state — and
           it is, for exactly one kind of caller: engine code that is re-reached by RE-RUNNING the flow's
           scheduler step. HTML §8.1.7.3 Processing model step 2.1 makes that choice "in an
           implementation-defined manner", between tasks, with the flow switched in and nothing of it on any
           stack; the sibling is assembled with NO frame and its step re-runs the same walk, where the arm
           recorded for it here replays. Assembled HERE and not stashed, because there is no later moment at
           which the parent is still the switched-in flow. */
        if (restartable) {
            DCHECK(f->frame == NULL,
                   "a restartable fork was asked for by a flow holding a live program frame — the caller "
                   "promised its computation is re-reached by re-running the flow's step, and a flow inside a "
                   "program re-enters that frame instead, so the sibling would never re-reach the ask");
            DCHECK(!JS_HasActivation(JS_GetRuntime(ctx)),
                   "a restartable fork was asked for while page code was on the stack — the caller was "
                   "reached THROUGH an activation (a queued job's callback, a listener inside a rendering "
                   "step), which the flow's frame handle cannot show because it is NULL for those too. Its "
                   "sibling would resume at a scheduler step that re-reaches nothing, and the arm recorded "
                   "for it would be replayed by whatever question that step asks first");
            engine_sibling_assemble(ctx, f, NULL, dec_blob, pin_blob);
            return 0;
        }
        /* AND A C BODY MID-ALGORITHM HAS NEITHER, WHICH IS AN UNBUILT DECLARATION AND NOT A REASON TO ASK
           LESS. It is already inside its own activation, so there is no machine state for the other arm to be
           snapshotted at and no way to re-reach the ask by re-running anything. What it needs is to become a
           step machine — JS_CFUNC_STEP_DEF at its definition, the ask moved into step_fork_run — after which
           the driver holds the resume point and takes the first branch above. */
#if APICLIENT_DEV
        {
            char name[192];
            size_t i;
            /* THE KEY IS LENGTH-PREFIXED AND ITS FIELD SEPARATORS ARE CONTROL BYTES (decide.c's
               concolic_ident_compose), so the one thing the reader needs from it — the name of the predicate
               that forked — is copied through printable-only. NOT for the record's sake: check.h escapes what
               it emits, so a raw separator would arrive intact and legal. It is for the READER's, who is
               looking for an identifier and would be handed a run of escapes in the middle of it. */
            for (i = 0; i + 1 < sizeof name && asked && asked[i]; i++)
                name[i] = (asked[i] >= 0x20 && asked[i] < 0x7f) ? asked[i] : '.';
            name[i] = '\0';
            DFAILF("a C builtin forked over unknown input from inside its own activation, so the sibling has "
                   "nowhere to resume: nothing will clone a frame for it and re-running the flow's step does "
                   "not re-reach the ask. Declare that builtin a step machine (JS_CFUNC_STEP_DEF) and move "
                   "this question into its step_fork_run, so the driver snapshots the machine AT the ask. "
                   "The question was: %s", asked ? name : "(no source identity)");
        }
#endif
        (void)asked;
    }
    DCHECK(g_fork_dec == NULL && g_fork_pins == NULL,
           "a sibling's snapshot state was prepared while a PREVIOUS one was still unconsumed — the branch that "
           "prepared it never reached its fork hook, and this assignment drops that flow's decision and pin "
           "blobs on the floor with nothing naming them");
    g_fork_dec = dec_blob; g_fork_pins = pin_blob;
    return 1;
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

/* ASSEMBLE A SIBLING FROM A PARENT AND A FRAME CLONE — everything a new timeline of this flow IS, and nothing
 * about WHY it exists. That split is the point of this function existing separately from the branch hook below.
 *
 * A FORK IS NOT ONLY A BRANCH. A flow forks over a PREDICATE (the interpreter's concolic OP_if, whose sibling
 * carries the other arm) and it forks over a VALUE it did not choose — a peer document's state IS its flows, so
 * one cross-instance read has N true answers and the asking flow explores one arm per DISTINCT ANSWER
 * (flow_answer_fork). Both need the identical construction: the frame clone, the O(1) delta and DOM-segment
 * fork, the generator swaps, the inherited chunks, jobs and register, the world minted as a CHILD of the
 * parent's so a peer can materialize the arm's segment by forking the asker's. What differs is exactly the
 * decision state, which is why it is a PARAMETER here rather than read from a static: a branch has an arm to
 * record and an answer fork has none, and that is the whole of the difference between them.
 *
 * AND IT FORKS OVER CODE THE PAGE NEVER RAN — the third caller (engine_orphan_fork), which is where `clone`
 * stops being a clone and the parameter's name stops being the whole truth. What a sibling needs is a FRAME TO
 * RESUME, and a branch arm's happens to be a snapshot of its parent's while a driven orphan's is a fresh call
 * of a function nothing has called (JS_FlowNewCall). Every line below is identical for the two, which is the
 * statement worth making: an orphan drive is not a kind of flow this scheduler can distinguish — same delta
 * fork, same DOM segment, same child world, same inherited queues and register, same rank.
 * `dec_blob` and `pin_blob` are CONSUMED — they become the sibling's. */
static Flow *engine_sibling_assemble(JSContext *ctx, Flow *parent, JSValue *clone,
                                     void *dec_blob, void *pin_blob) {
    DCHECK(parent != NULL, "a sibling was assembled with no parent flow — every field below is copied from one");
    DCHECK(dec_blob != NULL, "a sibling was assembled with no decision state — it would resume its parent's "
                             "frame standing on nothing, and every branch its parent had already taken would "
                             "be re-asked as a new one");
    /* A SIBLING RESUMES WHERE ITS PARENT STANDS, AND A FRAME IS ONLY HALF OF WHERE THAT IS. The claim here used
       to be "a sibling without a snapshot is not a sibling", which was the right worry stated as the wrong
       invariant: what makes a frameless sibling dangerous is not the missing handle, it is a parent that IS
       inside a program — `started` is set below, so such a sibling is hot with nothing to resume, and
       flow_step compiles the row at `script_i` again and REPLAYS side effects the parent already performed.
       A parent with NO frame is a flow between programs, and a sibling of it is a flow between programs too:
       its cursor is past the sequence, so there is nothing to recompile, and it re-enters its scheduler step
       exactly where its parent was. That is what a fork asked between tasks needs (engine_prepare_fork), and
       it is the state the cold tier's own rebuilds already have (flow.h: started, no frame, replaying arms).
       The orphan drive is the third shape and passes a FRESH call frame over a frameless parent, which this
       permits for the same reason: what is asserted is that a frameless SIBLING never stands over a parent
       that was mid-program. */
    DCHECK(clone != NULL || parent->frame == NULL,
           "a fork of a flow that is INSIDE a program arrived with no frame snapshot — the sibling would be "
           "marked hot with nothing to resume, and its first step would compile that program's row again and "
           "replay every side effect the parent has already performed");
    /* AN UNSTARTED CROSS-AGENT OPERATION AND A ROUTED DELIVERY ARE NOT ASSERTED HERE, and their assertions have
       moved to the BRANCH hook where they are true claims: they are statements about a flow being AT A BRANCH
       (nothing routed can be outstanding, because a delivery and an operation are both turned into work before
       any code runs), not about assembling a sibling. An answer fork happens at a PARK, where the trusted zone
       may perfectly well have attached one meanwhile, and it says so at its own site. */
    /* THE SIBLING IS A MEMBER OF THE FRONTIER BEFORE IT IS A FLOW, so nothing may page it out while this
       assembles it. flow_add publishes it into the registry on its first line and everything below is the
       CONSTRUCTION — a dozen allocations, any one of which can reach the allocator's refusal edge. A newborn
       flow carries the full optimism bonus and so is near the TOP of the ranking rather than the tail, which
       makes this a hazard that would fire rarely and read as a corrupt frontier when it did: the reclaim would
       free `sib` and every line after it would write through a dangling pointer. Same primitive, same reason
       and same shape as the job-drop walk: the engine is holding a position in its own frontier. */
    int prev_reclaim = engine_reclaim_set(0);
    /* BOTH ARMS GET A CHILD WORLD AND THE FORK POINT IS RETIRED — the sibling's by flow_add, the parent's on
     * the line after it, from the SAME parent name captured before either. The edge is recorded so another
     * instance that already holds a segment for the fork point can materialize each arm's by forking it — the
     * same O(1) shared-base-segment fork this function performs locally, performed there.
     *
     * THE PARENT ARM USED TO KEEP THE FORK POINT'S NAME, and that is what made this a fork in the flow graph
     * and not in the world graph. Two consequences, and the second one is silent data loss:
     *   - a peer asked "do these two senders contradict?" saw the two arms as ancestor-and-descendant, which is
     *     the same relation a flow's two SEQUENTIAL posts have — §9.3.3 "Posting messages" queues each on the
     *     posted message task source, which §8.1.7.1 "Definitions" serializes, so it must deliver them in
     *     order (the number here read §9.4.4, which is "Message ports": a whole section away). One
     *     question, two opposite answers, no way to tell which;
     *   - a peer materializes an arm's segment by forking the nearest ancestor it holds, and cow_delta_fork
     *     FREEZES that ancestor's head at that instant. The primary went on writing into the fork point's
     *     segment, so whether the sibling inherited the primary's post-branch writes depended on which arm
     *     reached that peer first. Retired, every ancestor is a world no flow holds, and what a peer forks
     *     cannot change under it — which is the invariant world_ancestry now asserts on every field it writes.
     * ONE EXTRA MINTED ROW PER FORK is the whole cost; the vector does not grow with it, because world_ancestry
     * names only ancestors that have themselves crossed the seam. */
    WorldId fork_point = parent->world;
    /* UNSEEDED, because an arm INHERITS its parent's sequence rather than starting one: the copy below is this
       flow's programs, and a seeded arm would have the root document's rows allocated onto it and then leaked
       under that copy — and would have them a second time, since the parent's own copy already holds them. */
    Flow *sib = flow_add_unseeded(ctx, parent->fn, fork_point);
    parent->world = world_mint_child(fork_point);
    /* AND ITS ACCOUNT IS A CHILD OF THE PARENT'S TOO — the WFQ's two terms, taken over at the branch like every
       other field of the parent's history copied below. It is FIRST because it is what decides where this flow
       enters the queue, and everything after this line is the construction of a flow that is already ranked. */
    flow_fork_inherit(sib, parent);
    sib->started = 1;                 /* HOT: resume from the blobs (and the cloned frame, when there is one) */
    sib->frame = clone;               /* the frame snapshot taken AT the branch — NULL for a fork between tasks */
    sib->script_i = parent->script_i; /* same position in the script sequence */
    /* …AND WHAT THAT FRAME IS. A branch taken inside an `error` listener is a branch inside its parent's
       §8.1.4.4 step 8 report, standing on the same row of the sequence: the arm has to finish the report and
       take the row's completion out of it, exactly as the parent would have. Left at zero the arm would read
       its own report frame as the row's PROGRAM — advancing the cursor a second time and skipping the next
       script, and restoring `document.currentScript` at a step §4.12.1.1 does not restore it at. */
    sib->reporting = parent->reporting;
    /* AND HOW FAR THE SEQUENCE HAS ALREADY BEEN RUN, which is what makes the no-replay assert at the compile
       site mean anything for an arm. Every sibling inherited -1, so a sibling that reached the compile with a
       cursor its parent had already compiled passed a check that only ever compared against "nothing yet" —
       and a FRAMELESS sibling is the shape that can actually reach it. The obligation is the same one every
       field above and below carries: an arm is its parent's timeline continued, so it has run what its parent
       had run. */
    sib->last_compiled = parent->last_compiled;
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
    sib->dec_blob = dec_blob;
    sib->pin_blob = pin_blob;
    /* AND SO DOES THE @S SUBSTITUTION, which is the same sentence about a different identity: this sibling
       resumes the same candidate session's frame from the fork point, in a heap where the injected payload has
       ALREADY been read, and it carries on into the other arm of the branch. Three fields were missing here and
       the failure was silent in both directions at once.
         - solve_flow_begin installs the running flow's substitution UNCONDITIONALLY (its own comment says why),
           so a sibling with no `cand_src` re-reads the attacker source as the ordinary concolic value from its
           very next switch-in. Its world is then half-injected: the prefix ran under the payload, the suffix
           does not, and nothing anywhere describes that state.
         - solve_flow_end records a PoC only for a flow that HAS a `cand_src`, so a breakout that fires in the
           sibling's arm — which is precisely the arm a gate was hiding — fired and was never reported. §@S says
           only firing proves it; this dropped the proof.
         - endpoint_suppress is keyed on the same field, so the sibling recorded @H endpoints built out of a
           fabricated breakout string as if they were observed. §Attacker-sources: COMPUTE OR SHAPE, NEVER
           INVENT.
       `cand_fired` DOES travel, unlike at the cold tier where dropping it is what keeps a finding an
       observation: a fire before the branch is a thing BOTH timelines performed, and record_sink dedups the two
       reports. `cand_verifying` does not, for the reason it does not cross the tier either — solve_flow_begin
       re-derives it from `cand_src` on every switch-in. */
    if (parent->cand_src) {
        DCHECK(parent->cand_payload && parent->cand_sink,
               "a candidate session forked holding only part of its substitution — the source, the payload and "
               "the sink class are one identity and the arm would inject nothing or be unable to say what fired");
        sib->cand_src = strdup(parent->cand_src);
        sib->cand_payload = strdup(parent->cand_payload);
        CHECK(sib->cand_src && sib->cand_payload, "engine: OOM forking an @S candidate session's substitution");
        sib->cand_sink = parent->cand_sink;   /* solve.c's static table text; not owned, so not copied */
        sib->cand_fired = parent->cand_fired;
    }
    /* AND SO DOES BEING A DRIVEN ORPHAN, which is the same sentence about the third thing a flow can be: an arm
       of an orphan drive is that drive continued, so it is no more reproducible from a decision vector than its
       parent is and the cold tier must refuse it for the same reason. A sibling that lost the mark would be
       written out as an ordinary flow and come back next session as a document replay that calls nothing. */
    sib->orphan = parent->orphan;
    /* AND THE NAME OF THE FUNCTION IT IS THE DRIVE OF, by the same sentence: the arm's recipe has to say which
       body to re-take, and it is the same body. An arm that inherited the MARK without the LOCATOR would be
       written out as a drive of nothing and refused at the park by the assert that says a drive has a name. */
    sib->orphan_hash = parent->orphan_hash;
    /* AND SO DOES THE WAIT, WHICH IS NOT OBVIOUS AND IS THE HALF THAT BREAKS IF IT IS LEFT OUT. A resumed drive
       does not begin as a call: it REPLAYS THE DOCUMENT first, exactly like the flow record it is written
       beside, and a replay branches. So a flow that is still waiting for its function forks arms while it
       waits, and each arm is the same drive of the same body — an arm that inherited the MARK and the LOCATOR
       without the WAIT would be a drive that never builds a call and never says so, and the park would then
       write an 'o' for it and assert against a `fn` it does not have.
       ONE FUNCTION SERVES ANY NUMBER OF THEM. What the take consumes is the `entered` BIT, not the function
       object, so N arms of one drive each call it; that is also what the original session did — one function,
       N flows whose frames were clones of one call. `fn` itself is carried by flow_add_unseeded above, and
       `orphan_argc` is a fact about that same body. */
    sib->orphan_want = parent->orphan_want;
    sib->orphan_argc = parent->orphan_argc;
    /* A FIELD ADDED TO THE CANDIDATE IDENTITY IS AN OBLIGATION HERE, and this is what says so — the same shape
       as the queued job's realm below, and for the same reason: the three fields are copied out one by one
       precisely so a fourth is visible, which is exactly how these three came to be missing. */
    DCHECK(!!sib->cand_src == !!parent->cand_src && !!sib->cand_payload == !!parent->cand_payload &&
           (sib->cand_sink != NULL) == (parent->cand_sink != NULL),
           "an arm of an @S candidate session left the fork without the substitution that makes it one — it "
           "would explore the other arm as an ordinary flow inside a heap the payload has already been read "
           "into, report its requests as observed endpoints, and be unable to record a fire");
    /* THE ANSWER TOKEN TRAVELS, AND IT TRAVELS WITH THE QUEUE — there is nothing to copy here any more. This
       sibling resumes the same operation's program from the fork point and completes it in its own timeline,
       so it owes the same peer an answer of its own: that is the multiplicity §7.2.1 has when a document's
       state is its flows, and it is what engine_host_answer records extras for at the other end. The token is
       on the row of the program being resumed (flow.h), so the queue copy below carries it — along with the
       document the operation was asked of, which is that row's `dyn_doc`. Copied here instead, the two were a
       second statement of one fact that the queue was already making. */
    if (parent->dyn_n) {              /* inherit the lazy chunks loaded up to the branch */
        sib->dyn = malloc((size_t)parent->dyn_n * sizeof(DynBody *)); CHECK(sib->dyn, "engine: OOM fork dyn");
        /* THE FLAGS COME WITH THE BODIES. A field added to the queue is an obligation at every clone, free and
           finish site; the sibling inheriting bodies without knowing which are candidates would re-arm the
           page-script assert on a dead breakout it inherited. */
        sib->dyn_cand = malloc((size_t)parent->dyn_n); CHECK(sib->dyn_cand, "engine: OOM fork dyn flags");
        /* AND SO DOES WHICH OF §8.1.4.4 "Calling scripts"'s TWO ALGORITHMS RUNS EACH, by the same sentence: an
           arm that inherited a `<script type=module>`'s body without its type would hand the module to the
           classic entry and take the page's own `import` back as a SyntaxError — on one arm and not the other,
           from one element. */
        sib->dyn_type = malloc((size_t)parent->dyn_n); CHECK(sib->dyn_type, "engine: OOM fork dyn script types");
        /* AND THE ADDRESS EACH PROGRAM'S BYTES CAME FROM, by the same sentence: it is §8.1.4.2's created-script
           base URL and a module row's MODULE MAP KEY, so an arm that inherited a bundle without it would
           resolve that bundle's nested `import('./chunk.js')` against the document instead of against the
           bundle, and register the module under a name it shares with every other module of that document. */
        sib->dyn_url = malloc((size_t)parent->dyn_n * sizeof(char *));
        CHECK(sib->dyn_url, "engine: OOM fork dyn script addresses");
        /* AND THE `script` ELEMENT EACH ROW IS THE PROGRAM OF, by the same sentence: §4.12.1.1's "execute the
           script element" is a switch on EL, so an arm that inherited a row without it would run the same
           `<script>` with that document's §3.1.7 `currentScript` left null on one timeline and set on the
           other, from one element. It is the one column that is COPIED AND NOT DUPLICATED — an element is not
           a per-arm thing, it is the same node in the same tree, and the sibling reaches it for the same
           reason it reaches every other baseline node: it holds a reference on the DOM base segment the
           parent's fork froze. */
        sib->dyn_el = malloc((size_t)parent->dyn_n * sizeof(lxb_dom_element_t *));
        CHECK(sib->dyn_el, "engine: OOM fork dyn script elements");
        /* AND SO DOES THE DOCUMENT EACH BELONGS TO, by the same sentence: an arm that inherited the bodies
           without them would compile a child navigable's script in the session's realm and define that
           document's globals on the creator's Window. */
        sib->dyn_doc = malloc((size_t)parent->dyn_n * sizeof(uint32_t));
        CHECK(sib->dyn_doc, "engine: OOM fork dyn documents");
        /* AND THE RENDEZVOUS TOKEN OF ANY ROW THAT STILL OWES AN ANSWER, by the same sentence and for the
           reason above: the arm is the operation's program continued, so it answers the same peer under the
           same token. An arm that inherited the row without it would run a peer's operation and tell nobody. */
        sib->dyn_token = malloc((size_t)parent->dyn_n * sizeof(char *));
        CHECK(sib->dyn_token, "engine: OOM fork dyn tokens");
        /* AND WHETHER EACH ROW IS A TASK OR THE SYNCHRONOUS TAIL OF THE PROGRAM THAT CAUSED IT, by the same
           sentence: the arm inherits the parent's cursor, so it inherits the parent's owed microtask
           checkpoint, and a row whose position it did not inherit would be ordered against that checkpoint the
           other way round — an inline <script> the parent's program inserted deferred behind a promise
           reaction on one arm and not on the other, from one insertion. */
        sib->dyn_pos = malloc((size_t)parent->dyn_n);
        CHECK(sib->dyn_pos, "engine: OOM fork dyn positions");
        /* THE SEVEN ARRAYS ARE ONE TABLE WITH ONE LENGTH, asserted rather than defaulted past. This read used to
           be `parent->dyn_cand ? parent->dyn_cand[i] : 0`, and a zero there is DYN_PAGE_SCRIPT — a real kind
           belonging to a real entry — so a parent whose flags were somehow absent handed the arm a queue of
           page scripts. The seven are allocated, grown and freed together, which makes the `? :` a claim about
           a state this file makes impossible; now the arm CRASHES where that state would be born instead of
           compiling a candidate as a page script, or an ADDRESS (DYN_SCRIPT_SRC) as a program. */
        DCHECK(parent->dyn_cand != NULL && parent->dyn_type != NULL && parent->dyn_url != NULL &&
               parent->dyn_el != NULL && parent->dyn_doc != NULL && parent->dyn_token != NULL &&
               parent->dyn_pos != NULL,
               "a flow holds queued programs with no kind, script type, address, document, token or position "
               "column — the seven arrays are one table and are allocated together, so the arm would inherit "
               "bodies whose kind, evaluation algorithm, resolution base, realm, waiting peer or place against "
               "the microtask checkpoint are lost");
        for (int i = 0; i < parent->dyn_n; i++) {
            /* THE PROGRAM TEXT IS REFERENCED, NOT COPIED — a program's bytes are the same on every timeline
               that holds it and no flow can write them (solver/dyn_body.h), so the arm's whole sequence costs
               `dyn_n` pointers. This line was `strdup`, which made a fork cost O(TOTAL SCRIPT BYTES): measured
               on a real single-page app whose module bundle is 2.1 MB, that is 2.1 MB per arm of every branch,
               and the run ended here at `CHECK(sib->dyn[i], "engine: OOM fork dyn body")` with the page's
               entire learned API surface as nothing. It is the same conversion solver/pending.h records for
               the register two fields over, on the one column that still paid for every byte. */
            sib->dyn[i] = dyn_body_ref(parent->dyn[i]);
            sib->dyn_cand[i] = parent->dyn_cand[i];
            sib->dyn_type[i] = parent->dyn_type[i];
            sib->dyn_el[i] = parent->dyn_el[i];
            sib->dyn_doc[i] = parent->dyn_doc[i];
            sib->dyn_pos[i] = parent->dyn_pos[i];
            sib->dyn_url[i] = NULL;
            if (parent->dyn_url[i]) {
                sib->dyn_url[i] = strdup(parent->dyn_url[i]);
                CHECK(sib->dyn_url[i], "engine: OOM forking an external script's address");
            }
            sib->dyn_token[i] = NULL;
            if (parent->dyn_token[i]) {
                sib->dyn_token[i] = strdup(parent->dyn_token[i]);
                CHECK(sib->dyn_token[i], "engine: OOM forking a cross-agent operation's rendezvous token");
            }
        }
        sib->dyn_n = sib->dyn_cap = parent->dyn_n;
    }
    /* THE LIFECYCLE IS NOT COPIED HERE ANY MORE: it lives on each Document as a heap write, so the sibling
       inherits every document's readiness through the delta it forks, along with everything else the flow had
       written. A field copied here could only ever have carried ONE document's. */
    /* THE REPLIES STILL IN FLIGHT ARE INHERITED TOO. A flow that forks while a request is outstanding — a
       fetch whose `.then` has not run, an injected <script src> whose body has not arrived — was leaving the
       sibling with an empty register, so the reply reached exactly one world and everything behind it was
       silently missing from the other. Both arms wait on the same REQUEST (engine_pending_fetches dedups the
       pair, and engine_provide fills every entry naming it), and each delivers on its OWN timeline: the resolve
       function is shared, but its already_resolved latch and the promise's settlement are per-flow state the
       COW delta captures, which is precisely what lets both arms settle one capability. */
    /* THE QUEUED JOBS ARE INHERITED FOR THE SAME REASON THE REPLIES ARE, and their absence was the same bug
       one layer down: a flow that forks with reactions still queued — a `.then` attached before the branch, a
       custom-element reaction, a listener task — left the sibling with an EMPTY queue, so that arm silently
       never ran them. Every one of those is a first-class flow in the one BFS, and dropping a work item is the
       thing the WFQ is forbidden to do; a fork that drops them is the same violation with a different spelling.
       It surfaced as a rejection reported unhandled in the arm whose `.catch` job never arrived — the report
       was right about its own world, and its world was missing a job. */
    /* SAME SPLIT AS THE THREE QUEUES BELOW, AND THERE IS NOTHING LEFT TO COPY BY HAND. This used to be a
       field-by-field deep copy — a malloc for the arm's entry array, a malloc for each job's arguments and a
       dup per argument — under a comment saying that a field added to the struct was an obligation here that
       nothing but that comment enforced. The struct is gone: the arm gets its own ARRAY naming the parent's
       RECORDS, so the inheritance costs one refcount per job and there is no field to forget. */
    sib->jobs = flow_job_fork(ctx, parent);
    /* THE ARRAY IS COPIED AND THE RECORDS ARE SHARED. The array has to be per-flow: the host walks EVERY
       flow's register from outside any flow's delta (engine_provide fills whichever flows parked on a REQUEST,
       engine_host_requests joins what is outstanding across all of them), and each arm removes an entry when
       IT delivers. The records do not, because a record never changes after it is pushed except for the
       ANSWER, and an answer is something both arms wait on and both observe. */
    sib->pending = pending_fork(parent->pending);
    /* …AND THE UNSTARTED OPERATIONS BY THE SAME SENTENCE. The arm is that timeline continued, so a question a
       peer asked of this document before the branch was asked of the arm too, and it owes an answer of its own
       under its own delta — which is the multiplicity §7.2.1 has when a document's state is its flows. Same
       split as the register above and for the same reason: the ARRAY is per-flow (each arm starts its own copy
       of the operation), the ENTRIES are shared (a [record, token] pair is never edited after it is pushed). */
    sib->perform_q = perform_q_fork(ctx, parent);
    /* …AND THE UNMADE DELIVERIES BY THE SAME SENTENCE AGAIN, which is the mechanism two asserts used to stand
       in for: a message that arrived in this timeline before the branch arrived in the arm too, so the arm
       makes its own delivery under its own delta. That is not one message delivered twice — it is one message
       arriving in two timelines of a document whose state IS its flows. Same split as the two above: the
       ARRAY is per-flow (each arm consumes at its own rate), the ENTRIES are shared. */
    sib->deliver_q = flow_deliver_fork(ctx, parent);
    /* …AND WHICH SENDING TIMELINES THE ARM IS IN, WHICH IS NOT A QUEUE AND IS THE REASON THE QUEUE ABOVE IS
       SAFE TO SHARE. The arm is this timeline continued, so every sender arm its parent had committed to is one
       the arm is in too; an arm that inherited the QUEUE without the COMMITMENTS would re-admit a message its
       own timeline had already foreclosed and deliver both arms of one sender branch — the exact fabrication
       the delivery-time fork exists to prevent, re-created by the fork itself. Same split as the three queues
       above: the ARRAY is per-flow (each arm commits on its own from here), the ENTRIES are shared (a
       [vector, taken] pair is never edited after it is pushed). A field added to that pair is an obligation at
       this line and at flow_release, which is why it is copied here by name rather than by a struct copy. */
    sib->deliver_world_q = flow_world_commit_fork(ctx, parent);
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
    DCHECK(g_genfork_n == 0,
           "a fork finished with generator-state swaps still in the stash — the sibling's cloned gen_data was "
           "never recorded on its delta, so it is owned by nobody and its body frame is unreachable");
    return sib;
}

/* JSFlowControlHooks.fork — the BRANCH's fork, which is the assembly above plus the one thing only a branch
   has: an arm to record. The handoff statics are drained BEFORE the assembly rather than inside it, which is
   what retires the assertion that used to stand at the end of this function: an early return could leave a
   decision blob owned by nobody (and with it the whole frozen prefix chain under it), and there is now no
   window in which one is both filled and unconsumed. */
static void engine_fork_finalize(JSContext *ctx, JSValue *clone) {
    Flow *parent = flow_running();
    void *dec, *pins;

    DCHECK(parent != NULL && g_fork_dec != NULL, "engine_fork_finalize: fork without a running flow / prepared state");
    /* AN UNMADE DELIVERY IS INHERITED RATHER THAN REFUSED, and the assert that refused it is gone with the
       SLOT it was about — the same correction, and for the same reason, as the unstarted operation two
       paragraphs down. It claimed no flow could be at a branch still holding a record ("a delivery is made
       before any code runs"), which was never true of a flow the zone routed to while it was suspended inside
       a frame: engine_route attaches to EVERY live flow, and a mid-frame one branches with its queue intact.
       The arm is that timeline continued, so the message that arrived in it arrived in the arm too, and the
       assembly carries the queue (flow_deliver_fork). Two timelines each delivering once is not one message
       delivered twice. */
    /* THE SAME SENTENCE FOR AN UNSTARTED OPERATION and the OPPOSITE one for a running answer. A cross-agent
       operation is turned into a program before any code runs, so no flow can be at a branch still holding the
       RECORD — two flows would then perform one operation twice and the peer would get two answers for a
       question it asked of one timeline. But the ANSWER TOKEN is different in kind: the program that answers is
       the page's own code, so a branch inside it is a real peer timeline in which the answer differs, and the
       sibling must carry the token and answer too (which the assembly does). */
    /* AN UNSTARTED OPERATION IS INHERITED RATHER THAN REFUSED, and the assert that refused it is gone with the
       slot it was about. It said the sibling "would perform the peer's one operation a second time under the
       same token" — which is exactly what the token inheritance below does on purpose for a STARTED one, and
       for the same reason: the arm is that timeline continued, so it held the question too and owes the peer an
       answer of its own. N answers under one token is what engine_host_answer records extras for at the other
       end. The refusal was an artifact of there being one slot, not a rule about operations. */
    dec = g_fork_dec; g_fork_dec = NULL;
    pins = g_fork_pins; g_fork_pins = NULL;
    engine_sibling_assemble(ctx, parent, clone, dec, pins);
}

/* ONE ARM PER DISTINCT ANSWER — the asking side of a peer document that has more than one timeline, and the
 * reason engine_host_answer only RECORDS.
 *
 * A cross-instance operation is performed by every live timeline the peer has (engine_perform attaches it to
 * each), so `otherW.length` comes back N times and every one of those numbers is the child-navigable count of
 * that document in one of its timelines. They are not a race to resolve and not duplicates to drop: they are a
 * value the asking flow must explore all of, which is the same primitive as a branch fork over a concolic
 * value, taken over an ANSWER instead of over a predicate.
 *
 * WHY HERE AND NOWHERE ELSE. This runs from flow_step with `f` SWITCHED IN — its COW delta applied, its DOM
 * head live, its decision state loaded — which is the only moment `cow_delta_fork`, `dom_cow_fork` and
 * `decide_fork_same_path` all describe the same flow. On engine_host_answer's line, between scheduler steps,
 * every one of those three would freeze whichever flow the scheduler last ran, and the arm would be a stranger's
 * timeline wearing the asker's name. It also runs BEFORE the flow resumes its frame, because the frame is what
 * the arm is a clone OF: once the machine is re-entered it takes the answer off the register and the call site
 * the arm must resume at is gone.
 *
 * ONE ARM PER STEP, like every other branch of flow_step. The remaining answers stay on the register and the
 * next step forks the next one, so a peer with a hundred timelines does not build a hundred flows inside one
 * step — the scheduler re-ranks between each, which is what it is for.
 *
 * THE ARM'S WORLD is a CHILD of the asking flow's (flow_add mints and records the edge), so a peer that already
 * holds a segment for the asker materializes the arm's by forking it. AND THE ARM NOW NAMES THE PEER TIMELINE
 * IT BELONGS TO: the completion crosses back carrying the answering flow's world (flow_answer_perform), the
 * delivery records it beside the answer (PEND_ANSWER_WORLD), and the arm takes it with the answer it was forked
 * over. THE HALF THAT IS STILL MISSING IS THE PIN, and it is the next thing here and nothing else: a SECOND
 * operation from this arm is still written with no addressee, so the peer performs it in every one of its
 * timelines again and this arm forks over all of them — the cross-product, of which every off-diagonal member
 * is a timeline neither agent was ever in (world_relation calls that pair CONTRADICT, and merging one is the
 * same fabrication as merging two senders' worlds at a delivery). What that needs, in order: the asking flow
 * carries the world it took an answer from (it is on the entry here, and engine_host_take drops the entry, so
 * it has to move onto the flow); the record grammar gains an ADDRESSEE field beside the target document
 * (remote_op.c's fields are positional, so it is one index and every operand shifts); and engine_perform
 * attaches an addressed record to the ONE flow whose world it names instead of to all of them, crashing where
 * that timeline is gone rather than answering out of another. */
static int flow_answer_fork(JSContext *ctx, Flow *f) {
    int n = pending_count(f->pending), i;

    for (i = 0; i < n; i++) {
        JSValue e = pending_entry(f->pending, i), av = JS_UNDEFINED, aw = JS_UNDEFINED, se;
        JSValue *clone;
        void *parked;
        int in_program, in_job;
        Flow *sib;
        int completion;

        if (pending_extra_count(e) == 0) { JS_FreeValue(ctx, e); continue; }
        DCHECK(flow_running() == f, "an answer fork was taken while another flow was switched in — the arm would "
                                    "clone that flow's delta and DOM head and call the result the asker's");
        /* WHERE THIS FLOW'S SUSPENSION ACTUALLY IS, ASKED OF BOTH HOMES — and it was asked of one, which is
           what made a correct assert fire on a correct fixture. A flow suspended inside a PROGRAM holds its
           call site in `frame`; a flow suspended inside a JOB holds no frame AT ALL, because flow_step runs a
           job only under `!f->frame`, and its call site is the PARKED CONTINUATION of the activation that job
           entered. Both are one activation held at one point, and which of the two it is is a fact about what
           the flow was running rather than about whether it may branch. The frame-only reading called the
           second "a flow that has already left the call site", which is the opposite of the truth: a `.then`
           handler that makes a cross-instance read is exactly the shape that reaches this — the read is
           answered once per peer timeline, so it is also the shape with the MOST arms to build.
           The park rides the Flow while it is switched OUT (flow_switch_out's JS_TakeParkedFlows) and sits in
           the RUNTIME's pump queue while it is switched IN, and this runs switched in — so the runtime is the
           home that answers here and `f->parked` is empty whatever the flow is holding. Both are asked, so a
           set that reached the flow's own slot without a switch-out says so instead of being read as none. */
        in_program = f->frame != NULL;
        in_job = JS_HasParkedFlow(JS_GetRuntime(ctx));
        DCHECK(f->parked == NULL,
               "a flow holds parked continuations on its own slot while it is SWITCHED IN — the switch hands "
               "the whole set to the runtime and clears the slot, so a set here belongs to a switch-out that "
               "did not happen and the arm below would fork the runtime's set instead of this flow's");
        DCHECK(in_program || in_job,
               "a flow holding a peer's second answer is suspended in NEITHER of the two homes — it has no "
               "program frame and no parked continuation, so the call site the arm exists to resume is gone "
               "and the answer belongs to a timeline that cannot be re-entered");
        /* AND NOT IN BOTH, which is an invariant of flow_step rather than a convenience: a job runs only while
           the flow holds no frame, and the resume arm at the top of that loop drains a parked continuation
           BEFORE any program is compiled, so a flow can never carry a park into a program. If this fires the
           two homes have become one flow's two live suspensions and the arm needs both cloned — which is
           buildable (each clone is independent) and must not be guessed at. */
        DCHECK(!(in_program && in_job),
               "a flow is suspended in BOTH homes at once — it holds a program frame AND a parked "
               "continuation, so the arm below would resume one of two live suspensions and silently abandon "
               "the other. Clone both onto the arm, in the order flow_step resumes them");
        /* A ROUTED DELIVERY IS NOT REFUSED HERE ANY MORE, because the thing the refusal asked for is built:
           it said "the arm is that timeline continued, so the message that arrived in it arrived in the arm
           too, and the assembly does not carry the record. Give the arm its own copy" — engine_sibling_assemble
           forks the delivery queue, so it does. */
        /* TAKEN FROM THE PARENT FIRST, so the arm inherits a list that no longer names it: the arm's copy is
           cleared below in any case, and the parent's must not fork over this answer a second time. */
        completion = pending_extra_pop(e, &av, &aw);
        JS_FreeValue(ctx, e);

        /* THE ARM IS A CLONE OF WHICHEVER HOME HOLDS THE SUSPENSION. Neither call is conditional on the other
           being absent — the asserts above have already established that exactly one is present — and each is
           a CHECK because a clone this engine cannot build is a peer timeline dropped off the frontier. */
        clone = in_program ? JS_FlowClone(ctx, (JSValue *)f->frame) : NULL;
        parked = in_job ? JS_CloneParkedFlows(ctx) : NULL;
        CHECK(!in_program || clone != NULL,
              "engine: a flow suspended on a cross-instance read could not be cloned — the peer's other "
              "timeline has an answer and no arm to carry it");
        CHECK(!in_job || parked != NULL,
              "engine: a flow PARKED on a cross-instance read inside a job could not have its continuation "
              "cloned — the peer's other timeline has an answer and no activation to resume with it");
        sib = engine_sibling_assemble(ctx, f, clone,
                                      decide_fork_same_path("(a peer's answer arrived — no predicate was "
                                                            "asked)"),
                                      concolic_pins_suspend());
        /* …AND ITS OWN CONTINUATION, ON THE SLOT A SWITCHED-OUT FLOW KEEPS IT IN. The arm is not switched in,
           so its set belongs on the Flow and not in the runtime; its first switch-in hands it to the pump and
           flow_step's resume arm re-enters it before anything else the arm could do — which is where it reads
           the answer written below off its own register. The assembly does not take it because a frame is what
           the assembly is handed: the two homes are the same fact and only one of them has a parameter, which
           is a shape to correct the day a third caller needs a park (an orphan drive and a branch arm both
           carry none, and a branch inside a job is assembled frameless by design). */
        DCHECK(sib->parked == NULL,
               "a freshly assembled arm already holds parked continuations — the assembly builds a flow that "
               "has never run, so a set on it came from somewhere that is not this fork and the write below "
               "would drop it");
        sib->parked = parked;
        /* THE ARM'S OWN ANSWER, AND THE ONE RECORD IT CANNOT SHARE. pending_fork shares an ANSWERED entry
           deliberately — an answer that arrived before a fork was computed in a world both arms were in — and
           this is the case that is not: the two arms hold DIFFERENT answers to one question, which is exactly
           the disagreement that stops a record being shared. */
        se = pending_unshare(sib->pending, i);
        pending_set(se, PEND_VALUE, av);
        pending_set_int(se, PEND_COMPLETION, completion);
        /* …AND WHOSE ANSWER IT IS, taken from the same triple. It is what makes this arm a flow of a NAMED pair
           of timelines rather than of the asker's alone, and it is what the pin above will address this arm's
           next operation with. */
        pending_set(se, PEND_ANSWER_WORLD, aw);
        /* …AND NO ANSWERS BEYOND ITS OWN, in both directions. The arm inherited the parent's remaining list
           through the STRUCT copy, and leaving it would make the arm fork over answers the parent is still
           going to fork over; and its answer is FIXED, so a LATER answer to this request belongs to the flow
           that issued it and never to this timeline, which took answer k and cannot also have taken k+1. The
           two together are why a peer with N timelines costs exactly N-1 arms. */
        pending_set(se, PEND_EXTRA, JS_NULL);
        pending_set(se, PEND_ANSWER_FIXED, JS_TRUE);
        DCHECK(pending_get_int(se, PEND_HAVE_VALUE) != 0,
               "an arm was forked over a peer's answer onto a request that is still UNANSWERED — the assembly "
               "re-issues an unanswered request under a fresh id, so this arm would be waiting on a question "
               "nobody has been asked");
        JS_FreeValue(ctx, se);
        return 1;
    }
    return 0;
}

/* The frame-agnostic REPLAY fork is DELETED: re-running a nested/deep flow from its start is BANNED (not
   byte-identical — shared state can differ between the run and the re-run). A concolic branch inside an async
   body on the tramp chain now DFAILs in the engine (see branch_arm_fork) until the sound async-frame snapshot
   is built; there is no re-run fallback to hide that gap. */

/* ORPHAN-INVOKE — the flows that make a page's UNCALLED code run, which is the whole of "learn the logged-in
 * API surface while logged out". An SPA ships one bundle to everybody, so the admin handler, the billing route
 * and the lazy chunk's export are all in the bytes a logged-out visitor receives; nothing calls them, so a
 * sniffer sees nothing and a solver that only follows what the page does sees nothing either.
 *
 * IT IS NOT A PHASE AND NOT A DRIVER. Each orphan becomes one ORDINARY FLOW, assembled by the same
 * engine_sibling_assemble an answer-fork arm uses — same delta fork, same DOM segment, same child world, same
 * inherited queues, same WFQ rank — differing in exactly one thing: the frame it resumes is a CALL of the
 * orphan rather than a clone of its parent's. So it is preemptible at every opcode, its branches fork siblings
 * like any other flow's, it is ranked and starved and paged by the one policy, and nothing anywhere loops over
 * orphans.
 *
 * WHEN. Here, at the ONE point where "nothing called this function" is a fact rather than a guess: the parent
 * has no program left, no job, no timer, no rendering opportunity and no reply outstanding, so everything it
 * was ever going to call, it has called. Taking them earlier — at a program's completion, say — would drive a
 * function the NEXT program was about to call, with unknowns in place of the arguments the page had for it.
 * Taking them later is what a phase would be.
 *
 * WITH WHAT ARGUMENTS. An orphan's parameters are unknown external input and are minted exactly as any other
 * unknown is (concolic_new): domain-carrying, example-free, so a gate inside the body FORKS both arms instead
 * of collapsing on a fabricated value. EACH ARGUMENT GETS ITS OWN SOURCE IDENTITY, and that is not decoration:
 * the per-flow constraint tracker is keyed by source, so `function(a,b){ if (a=='x' && b=='y') sink(); }` with
 * one shared identity records ==x and ==y against the SAME source, which is a contradiction, which PRUNES the
 * arm that reaches the sink. The receiver is one more unknown for the same reason — a method driven with
 * `undefined` throws on its first `this.field` in strict code and explores nothing.
 * HOW MANY is the callee's OWN declared formal parameter count, which is the page's statement of how many
 * unknowns its caller supplies; the old implementation passed a fixed eight to everything.
 *
 * WHAT STOPS IT RE-DRIVING FOREVER is that a body is TAKEN once (JS_OrphanTakeOne consumes the bit that
 * defines it) and that the flow it becomes is ranked like everything else: an orphan that emits nothing
 * accumulates service, sinks below the productive and the unrun, and is paged to the cold tier. No seen-set, no
 * counter, no cap — the old implementation had all three (a 4096-entry buffer compared by pointer) and the cap
 * silently truncated the surface it existed to find.
 *
 * ONE PER STEP, WHICH IS THE SEAM. This took the WHOLE heap's worth in one call and turned every one of them
 * into a flow inside one C loop: no back-edge, no preempt check, so the scheduler could not re-rank, could not
 * return the thread to its host and could not page its tail until the last of them was assembled. It is the
 * shape engine_sched_step's seam assertion exists to name, and it named it — 3890 units of work in one step
 * with `points asked=0`, on a real bundle. The fix is not a suspend point inside the loop (a C loop has no
 * continuation to park), it is that THERE IS NO LOOP: the take hands over ONE, this seeds ONE flow, and the
 * scheduler's own loop is the only thing that iterates. Every seeded flow is then ranked against its already-
 * seeded siblings and against the parent BEFORE the next is taken, which is what the caller's comment claims
 * and what a burst made false; nothing is buffered between two calls, so nothing forks wrong, serializes wrong
 * or is dropped by a park.
 * WHICH DELTA AN ORPHAN FORKS IS THEREFORE THE DISCOVERING FLOW'S, and that is the same sentence it always was
 * rather than a consequence of taking one: this is reached by EVERY flow that runs out of work, so a batch take
 * merely meant whichever flow arrived first forked all of them. Now the flow that reaches exhaustion next takes
 * the next one, under its own timeline — which is the timeline in which "nothing called this function" is the
 * fact this site asserts, so it is the right one, and there is more of it.
 *
 * Returns 1 if it seeded a flow; 0 means there was nothing to take, which is what lets the caller finish. */
typedef struct {
    JSValue fn;    /* the taken function object (an owned ref) */
    int     argc;  /* its declared formal parameter count */
    int     n;     /* how many the take handed over — one, asserted */
} OrphanTake;

/* THE VISITOR, AND IT ONLY RECORDS. JS_OrphanTakeOne calls it from inside the walk of the runtime's object
   list, so creating a GC object here would insert into the list being iterated; the dup below is a refcount
   bump, which allocates nothing. JS_OrphanTakeOne asserts exactly that. */
static void engine_orphan_record(JSContext *ctx, JSValueConst fn, int arg_count, void *opaque) {
    OrphanTake *t = (OrphanTake *)opaque;

    /* THE SEAM, ASSERTED AT THE ONE PLACE IT CAN BE LOST. A second hand-over in one call is a take that went
       back to enumerating the set, and the caller below turns each into a flow — so it would mint an unbounded
       chain of flows inside one scheduler step again, with nothing between them for the preempt hook to be
       consulted at. It is not a tautology about the callee: it is this consumer's requirement on it, stated
       where a change to the walk would break it rather than in the seam verdict six thousand lines away. */
    DCHECK(t->n == 0,
           "the orphan take handed over a SECOND function in one call — each one becomes a flow, so the step "
           "that seeds them is a loop again with no suspend point on it, which is the stretch the scheduler's "
           "seam assertion aborts on");
    t->fn = JS_DupValue(ctx, fn);
    t->argc = arg_count;
    t->n = 1;
}

/* HOW MANY ORPHANS THIS DOCUMENT HAS DRIVEN. Reported in the result document so a run can say how much of the
   page's code was reached by nothing but forced invocation.
   IT IS NO LONGER EACH DRIVE'S NAME, and that is the correction this comment carries. It used to be: an
   argument's source identity was `{orphan<ordinal>.arg0}`, chosen because an ordinal was the only thing
   available that was unique (a function's `name` is empty for half of a minified bundle and duplicated across
   the rest). An ordinal is unique WITHIN a session and means nothing across one — and the moment a drive's
   recipe crossed the tier that became the difference between reproducing the drive and reproducing its PATH.
   decide.c's replay compares the QUESTION each recorded arm answers, and a question is composed from the source
   identities in it, so a resumed drive of the same body under a different ordinal asks a different question at
   its very first branch: every arm the drive recorded inside the function is discarded as a divergence and the
   body is re-explored from there. The LOCATOR is unique per body and stable across sessions, so it is what
   names the unknowns now, and the recorded arms line up. Two drives of one body share the name, which is
   correct rather than the aliasing the ordinal was avoiding: they are the same call of the same function with
   the same unknowns, and a fork already shares its parent's argument objects. */
static long g_orphans_driven;
/* AND HOW MANY TIMES A FLOW GOT AS FAR AS ASKING — see the raise site for why the pair is read together and
   never one of them. It is the ORPHAN surface's half of what solver/solve.h's arrival census is for the @S
   surface: a zero that cannot say whether the engine looked and found nothing or never looked. */
static long g_orphan_asks;

/* THE ORPHAN SURFACE'S OWN CENSUS — see engine.h, and see the raise sites for what each number is counted at.
   IT LIVES BESIDE THE COUNTERS AND NOT BESIDE engine_routed_census, which is the other census this file
   exports: an accessor placed with its siblings instead of with its data reads statics declared two
   thousand lines below it, which does not compile — and the version of this that tried is why the note is
   here rather than being rediscovered.
   `driven` EXISTED AND REACHED NOTHING A CENSUS CAN READ. It was written into the heap/progress line only, and
   §Testing is explicit that the renderer does not tee its stdout — "a console scrape is the wrong surface by
   construction" — so the ONE number that says whether this engine ever drove code the page shipped and never
   ran was, for every session there has been, computed and thrown away. §What-the-tool-produces makes that
   surface the headline ("Surface INTERESTING UNUSED endpoints ... A sniffer shows what FIRED; this shows what
   the bundle CAN do but didn't"), so the product's own proposition had no measurement on the shipped path.
   BOTH OR NEITHER, for solve_arrival_census's reason exactly: `driven == 0` alone is three different findings
   — the bundle ships no uncalled code, no flow ever reached the end of its own work, or the walk ran and the
   heap had none — and only the pair tells the middle one (a scheduling result to act on) from the outer two
   (facts about the page). */
void engine_orphan_census(long *driven, long *asked) {
    DCHECK(driven != NULL && asked != NULL,
           "the orphan census was asked for one of its two numbers — a drive count with no ask count beside it "
           "cannot say whether the frontier ever reached the question, which is the difference between a page "
           "that ships no uncalled code and a scheduler that never got to it");
    *driven = g_orphans_driven; *asked = g_orphan_asks;
}

/* AND THE GENERATION AT WHICH THIS DOCUMENT LAST WALKED THE HEAP. Creating a function object is the only event
   that can add to the orphan set (JS_OrphanGen), so a walk at an unchanged generation can only find what the
   previous one already took — and the walk is O(live objects) while a frontier has one finishing flow after
   another asking. Not a memo about any orphan: it is the answer to "has anything happened", and being wrong in
   the only direction it can be wrong (a 2^32 wrap) costs one redundant walk. */
static uint32_t g_orphan_gen_seen;
static int      g_orphan_gen_valid;
/* THE ROUND TRIP'S TWO NUMBERS. How many waits a take has SATISFIED, and how many waiting flows FINISHED
   without ever being handed a body. The third, how many were rebuilt, is the cold tier's own
   (cold_resumed().orphans) and is not restated here.
   THE UNMET COUNT IS THE OBSERVABLE, and it is counted at the finish rather than derived from the other two
   because the other two do not subtract: a waiting drive forks arms while it replays, so more flows can be
   satisfied than there were records, and a difference of counts would report that healthy multiplicity as a
   loss. A drive that finished still waiting is the loss, exactly, with nothing else in it — the bundle no
   longer holds the body its recipe named, or the locator does not name what it was written for. On a document
   whose bytes did not change between two sessions it is ZERO, and nothing else in the run would say so: a
   resumed frontier whose most expensive members drive nothing looks identical to one that worked. */
static long g_orphan_claims_met, g_orphan_claims_unmet;
/* …AND WHETHER ANY CLAIM IS STILL OPEN. It is a LATCH set by a walk that found none, not a counter maintained
   at every site that could clear one: a claimant can also leave the frontier by being finished or paged, and a
   counter would then have to be decremented at flow_remove, at flow_release and at the park — three obligations
   for a fact one walk answers exactly. The walk is O(members) and runs once per orphan TAKEN, only in a session
   that resumed drives at all, and it stops for good the first time it finds the frontier holds none. */
static int g_orphan_claims_closed;

/* HAND THIS BODY TO EVERY RESUMED DRIVE THAT WAS WAITING FOR IT, and answer how many there were.
   EVERY ONE OF THEM AND NOT THE FIRST. What a take consumes is the `entered` BIT; the function object itself
   is not consumed by being called, and the session that recorded these flows had exactly this shape — ONE
   function and N flows whose frames were clones of one call of it. Handing it to the first claimant and
   leaving the rest waiting would starve every arm of a drive but one, which is the same silent drop as
   dropping the record.
   IT IS A WALK OF THE FRONTIER AND NOT A TABLE KEYED BY HASH, for the reason cold.c gives about its own
   deleted pointer-keyed index one level over: the value would be a `Flow *`, a flow leaves the frontier by
   several different doors, and an entry that outlives its flow answers a later take with a dangling pointer.
   The registry is the one structure that cannot be stale about which flows exist, so it is what is asked. */
static int engine_orphan_route(JSContext *ctx, JSValueConst fn, int argc, uint64_t hash) {
    Flow *fl;
    int i, open = 0, n = 0;

    for (i = 0; (fl = flow_at(i)) != NULL; i++) {
        if (!fl->orphan_want || !JS_IsUndefined(fl->fn)) continue;
        open++;
        if (fl->orphan_hash != hash) continue;
        DCHECK(fl->orphan,
               "a flow is waiting for an orphan's body without being marked as a drive — the three fields are "
               "one identity, written together by the cold tier's rebuild, so this flow was assembled by "
               "something that set part of it");
        /* IT BUILDS THE CALL ITSELF, LATER, IN ITS OWN TIMELINE. Only the function crosses here, because the
           receiver and the arguments are concolic OBJECTS: minted under this flow's stamp they would be this
           flow's private state for the rest of the session, so no delta would ever capture the claimant's
           writes to them and no rewind would restore them. */
        fl->fn = JS_DupValue(ctx, fn);
        fl->orphan_argc = argc;
        n++;
    }
    /* THE LATCH, SET BY THE WALK ITSELF. Only cold_resume opens a claim and it runs once, before any flow is
       picked, so a frontier that holds none holds none for the rest of the session. `open` is counted before
       this pass satisfies anything, so a pass that satisfies the last of them latches on the NEXT take. */
    if (!open) g_orphan_claims_closed = 1;
    g_orphan_claims_met += n;
    return n;
}

/* THE CALL A DRIVEN ORPHAN IS. Mints one concolic per declared parameter and one for the receiver — an orphan's
 * arguments are unknown external input by definition, since nothing in the page ever supplied any — and returns
 * the flow base that calls `fn` with them.
 * IT RUNS IN THE TIMELINE OF THE FLOW THAT WILL DRIVE IT, which is why it is a function rather than two copies:
 * the receiver and every argument are objects, so they are stamped with the running flow's generation and are
 * that flow's private state for the rest of the session. Minting them under one flow and handing them to
 * another would put a second flow's writes into the first flow's private set, where no delta captures them and
 * no rewind restores them.
 * THE UNKNOWNS ARE NAMED BY THE BODY'S LOCATOR, not by an ordinal — see g_orphans_driven for why that changed
 * and what it costs to get wrong: the name is part of every question this call's branches ask, and a question
 * asked under a name the recording session did not use is a divergence at the drive's first branch. */
static JSValue *engine_orphan_call(JSContext *ctx, JSValueConst fn, int argc, uint64_t hash) {
    JSValue *args = NULL, self, *base;
    char id[64];
    int k;

    DCHECK(hash != 0, "a driven orphan's call was minted with no locator — every unknown it supplies would be "
                      "named after a body that does not exist, and two drives of two different functions would "
                      "then share one identity");
    if (argc > 0) {
        args = (JSValue *)malloc((size_t)argc * sizeof(JSValue));
        CHECK(args, "engine: OOM minting a driven orphan's arguments");
    }
    for (k = 0; k < argc; k++) {
        snprintf(id, sizeof id, "{orphan%016llx.arg%d}", (unsigned long long)hash, k);
        args[k] = concolic_new(ctx, id, id, JS_UNDEFINED);
    }
    snprintf(id, sizeof id, "{orphan%016llx.this}", (unsigned long long)hash);
    self = concolic_new(ctx, id, id, JS_UNDEFINED);

    base = JS_FlowNewCall(ctx, fn, self, argc, (JSValueConst *)args);
    CHECK(base != NULL, "engine: a driven orphan's call frame could not be allocated — the bit that made "
                        "this function an orphan is already consumed, so there is no second chance at it");
    /* JS_FlowNewCall dup'd the receiver and every argument into the frame; the mints are ours to release. */
    JS_FreeValue(ctx, self);
    for (k = 0; k < argc; k++) JS_FreeValue(ctx, args[k]);
    free(args);
    return base;
}

/* THE THREE THINGS AN ORPHAN STEP CAN DO, all of them PROGRESS and all of them distinct work: it seeds a fresh
   drive of a body nothing has called, it hands a body back to the drives a park recorded for it, or it builds
   the call of a drive that has been handed one. 0 is "there was nothing to take", which is what lets the
   caller finish. */
#define ORPHAN_STEP_SEEDED   1
#define ORPHAN_STEP_ROUTED   2
#define ORPHAN_STEP_RESUMED  3

static int engine_orphan_fork(JSContext *ctx, Flow *f) {
    OrphanTake t = { JS_UNDEFINED, 0, 0 };
    uint32_t gen = JS_OrphanGen(JS_GetRuntime(ctx));
    uint64_t hash;
    JSValue *base;
    Flow *sib;

    /* A SESSION THAT EXPLORES NOTHING DRIVES NOTHING, AND THAT IS THE SAME SENTENCE engine_prepare_fork'S ASSERT
     * MAKES — said one mechanism over, where nothing was saying it.
     *
     * §scheduler is explicit that a drive of a function the page never called is "just another BFS flow", and
     * the line below MINTS one: engine_sibling_assemble, over decide_fork_same_path, adds a member to the
     * frontier. A session declared non-forking is one that must not gain members from inside itself — §@S's
     * candidate re-fire is ONE concrete path, and a conformance run measuring the browser half against a spec
     * oracle is measuring a browser, which never invokes a function nobody called. The fork seam is asserted
     * for exactly this and this path went around it, through a different door onto the same frontier.
     *
     * IT IS ALSO WHERE THE ONE UNANSWERABLE ORDER COMES FROM. engine_orphan_call mints each argument and the
     * receiver with NO EXAMPLE — correctly: nothing in the page ever supplied one, and §Solver forbids
     * inventing one. Those are the only example-free unknowns a host without the source overlay can hold, so
     * they are what reaches §8.7's `setTimeout` as a timeout nothing computed, becomes an expiry nothing
     * computed, and leaves core/timing/event_loop.c's order with two feasible arms and no fact to choose
     * between them. Not driving them in such a session removes the question rather than answering it wrong.
     *
     * A DRIVE THAT IS ALREADY A MEMBER IS THE SAME STATEMENT, NOT AN EXCEPTION. A resumed drive (the branch
     * below) is a residue record standing on a recorded path, which only a session seeded from recipes can
     * hold; a non-forking session that held one would be a residue seeded into a session that cannot serve it,
     * so it is asserted rather than served. */
    if (!g_sess_forking) {
        DCHECK(!f->orphan_want,
               "a flow waiting to adopt an orphan's body is a member of a session that explores nothing — its "
               "recipe was recorded by a session that forks, and this one can neither drive it nor fork the "
               "arms its path is made of, so the residue was seeded into a session that cannot serve it");
        return 0;
    }

    /* A RESUMED DRIVE THAT HAS ITS FUNCTION BACK BUILDS ITS OWN CALL, and it builds it HERE rather than at the
     * moment it was handed one. This is the point at which a flow has nothing left to run, which is where the
     * drive belonged in the session that recorded it — so the recorded arms the replay has been consuming run
     * out exactly here, and the arms the CALL then takes are the next ones in the same chain. Adopting earlier
     * would put the call in front of programs the flow had still to replay and consume their arms with it.
     * IT IS THIS FLOW AND NOT A SIBLING. The recipe named ONE flow, the reward on it is the drive's, and the
     * decision chain under it is the drive's path; seeding a sibling here would leave this flow standing on an
     * orphan's chain with nothing to run and hand the work to a member with no record behind it. */
    if (f->orphan_want && !JS_IsUndefined(f->fn)) {
        DCHECK(f->orphan && f->orphan_hash != 0,
               "a flow is waiting to adopt an orphan without being one — `orphan_want` is written only by the "
               "cold tier's rebuild and only beside the mark and the locator, so this flow was assembled by "
               "something that set one third of an identity");
        DCHECK(flow_running() == f,
               "a resumed orphan drive built its call frame while another flow was switched in — the receiver "
               "and every argument are objects stamped with the running flow's generation, so they would be a "
               "stranger's private state for the rest of the session and no delta would capture the writes");
        g_orphans_driven++;
        base = engine_orphan_call(ctx, f->fn, f->orphan_argc, f->orphan_hash);
        f->frame = base;
        /* THE WAIT IS OVER AND IT IS NOT COUNTED AGAIN HERE. `orphanClaimsMet` counts HAND-OVERS BY A TAKE,
           which is the event the round trip is about; an arm that inherited the function from a waiting parent
           reaches this line too, and counting frames built would add those to a number compared against the
           records. */
        f->orphan_want = 0;
        return ORPHAN_STEP_RESUMED;
    }

    /* A FLOW RAN OUT OF WORK AND ASKED WHETHER THE PAGE SHIPPED CODE IT NEVER RAN — counted HERE, which is
       the one point that event happens, and counted BEFORE the memo below rather than after it. §scheduler
       makes the drive of an uncalled function an ordinary BFS flow, and `driven` alone cannot say whether the
       frontier ever GOT to the question: `driven == 0` is "this bundle ships no uncalled code", "no flow ever
       reached the end of its own work", and "the walk happened and the heap had none" at once, and the three
       take opposite actions. The first is a fact about the page, the second is a scheduling result worth
       acting on, and only the pair separates them.
       BEFORE THE MEMO ON PURPOSE. The memo answers the question from a previous walk's result; a flow that
       reaches it HAS asked, and counting past it would make this number a fact about the memo instead of
       about the frontier — the same one-number-two-mechanisms defect solver/solve.h's arrival census exists
       to end, in the surface §What-the-tool-produces calls the headline. */
    g_orphan_asks++;
    if (g_orphan_gen_valid && gen == g_orphan_gen_seen) return 0;
    /* THE MEMO IS RECORDED ONLY BY AN EMPTY WALK, and that is what the take taking ONE changes about it. It
       used to be written before the walk, which was right for a call that drained the set: after it, the answer
       to "is there anything at this generation" really was no. A one-at-a-time take leaves the rest of the set
       standing at the SAME generation — nothing about seeding a flow creates a function object — so writing it
       here would say "nothing to take" about a heap full of untaken orphans and every one of them would be lost
       for the session. It states exactly what it is: the generation at which a walk found NOTHING. */
    if (!JS_OrphanTakeOne(ctx, engine_orphan_record, &t)) {
        g_orphan_gen_seen = gen; g_orphan_gen_valid = 1;
        DCHECK(JS_IsUndefined(t.fn), "an orphan take that reported nothing still left a function on the record");
        return 0;
    }
    DCHECK(t.n == 1 && !JS_IsUndefined(t.fn),
           "an orphan take reported success without handing over a function — the bit that made it an orphan "
           "is already consumed, so it can never be driven again in this session");

    DCHECK(flow_running() == f,
           "orphans were taken while another flow was switched in — the arms below fork THIS flow's delta, DOM "
           "head and decision state, so they would be assembled out of a stranger's timeline and called the "
           "discovering flow's");
    DCHECK(f->frame == NULL,
           "a flow took an orphan with a suspended frame still live — the take happens where the flow has "
           "nothing left to run, which is the only point at which 'nothing called this function' is a fact, and "
           "a live frame means the flow was still going to call something");

    /* WHAT THIS BODY IS CALLED ACROSS SESSIONS, computed once per body because a body is taken once. It is
       needed unconditionally — every drive this session creates has to be parkable, and the locator IS its
       recipe — and the routing below is the second reader of the same number rather than a reason to compute
       it. Its cost is one pass over one function's source text, so the whole session pays one pass over the
       bundle. */
    hash = JS_OrphanHash(ctx, t.fn);

    /* AND WHETHER A DRIVE THIS SESSION INHERITED WAS WAITING FOR EXACTLY THIS BODY. The recorded flow carries
     * the path, the rank and the arms; what it could not carry is the function, because a heap reference dies
     * with the session that held it. So the take routes: the function goes to the flow whose recipe named it,
     * and that flow builds the call in its own timeline at its own next turn (the branch at the top).
     * WITHOUT THIS THE RECIPE IS NEARLY INERT, and that is the whole reason it exists rather than each claimant
     * taking its own. Every flow that runs out of work reaches this line, so a resumed frontier of thousands of
     * members races for the same heap: the first arrival takes the body a claimant was recorded for and drives
     * it from a fresh path, and the claimant — whose recorded arms are the gates that made that function
     * reachable in the first place — finds nothing left to take and finishes having driven nothing. The
     * function would still be driven and the WORK would not look lost, which is exactly what makes it the kind
     * of loss nothing reports.
     * IT IS SKIPPED ENTIRELY WHEN NO CLAIM CAN BE OPEN, so a session with no residue pays one comparison. */
    if (!g_orphan_claims_closed && engine_orphan_route(ctx, t.fn, t.argc, hash)) {
        /* NO FRESH DRIVE IS SEEDED BESIDE THEM. The body is being driven — by flows standing on the recorded
           paths that made it reachable, which is strictly more than a fresh drive from the baseline would
           reach — and a sibling assembled here as well would be a second timeline calling the same function
           for the same reason, ranked with the discovering flow's account and standing on nothing. */
        JS_FreeValue(ctx, t.fn);
        return ORPHAN_STEP_ROUTED;   /* progress exactly as seeding one is */
    }

    g_orphans_driven++;
    base = engine_orphan_call(ctx, t.fn, t.argc, hash);
    /* THE OTHER SIDE OF THE GATE AT THE TOP OF THIS FUNCTION, asserted where the member is actually minted
       rather than only where the mechanism is entered — the two are separated by a take, a hash and a route,
       any of which could grow its own way in. */
    DCHECK(g_sess_forking,
           "a driven orphan was about to be added to the frontier of a session that explores nothing — the "
           "gate at the top of this walk returns before the take, so reaching this line means a second entry "
           "into the drive was built that does not go through it");
    sib = engine_sibling_assemble(ctx, f, base,
                                  decide_fork_same_path("(a function the page never called was driven — no "
                                                        "predicate was asked)"),
                                  concolic_pins_suspend());
    sib->orphan = 1;
    /* AND THE FUNCTION IT RE-DRIVES, which is what flow.h says `fn` IS and what nothing had put there:
       every flow_add in this engine passed JS_UNDEFINED, so the field's comment described a flow kind that
       did not exist. It is what makes a walk of the frontier able to say which functions are being driven at
       all. The assembly dup'd the PARENT's (undefined), so that reference is given back here. */
    JS_FreeValue(ctx, sib->fn);
    sib->fn = JS_DupValue(ctx, t.fn);
    /* …AND THE NAME THAT OUTLIVES IT, which is what the park writes and what a later session finds this same
       body by. Stamped HERE, at the one place a drive is created, so no drive can exist without one; an arm of
       it inherits the field with the mark (engine_sibling_assemble). */
    sib->orphan_hash = hash;
    /* WHERE IT ENTERS THE QUEUE, ASSERTED AT THE ONE ENTRY THAT MANUFACTURES ITS OWN WORK. An orphan drive is
       work this timeline CREATES, so it is exactly the shape flow.c's arrival rule exists for: a flow that
       could enter at virtual time zero would carry the full optimism bonus and stand ahead of the entire
       backlog, and a bundle with thousands of uncalled functions would promote thousands of them there. It does
       not take flow_arrive_at_virtual_time's path — it is assembled as a SIBLING, so flow_fork_inherit hands it
       the discovering flow's account — and the two agree here by construction rather than by coincidence: the
       DCHECK above says this flow is the one in service, and SFQ's v(t) IS the service of the flow in service.
       Asserted because the two are reached through different functions and only this site knows they must
       meet. */
    /* EVERY COORDINATE, because the virtual time a flow enters at has as many as the weight has terms: the
       completed-unit count (the optimism term's), its own service and its FAMILY's (the aging term's two
       halves). `born` used to be the second half of this assert and it was the same quantity as the first; it
       is gone with the per-arm charge. An orphan drive that founded a family of its own would carry the
       discovering flow's reward with none of its aging, and one entering at zero units would carry the full
       optimism bonus — either is the promotion this line exists to forbid, and a bundle ships thousands of
       uncalled functions to do it with. */
    DCHECK(sib->cpu == f->cpu && sib->visits == f->visits && sib->family == f->family,
           "a driven orphan entered the frontier at a virtual time that is not the running flow's — it was "
           "ranked against a clock nobody chose, and a page with many uncalled functions can then promote the "
           "work it manufactures above every flow already waiting");

    JS_FreeValue(ctx, t.fn);   /* the take's reference; the sibling holds its own dup */
    return ORPHAN_STEP_SEEDED;
}

/* WHAT THAT STEP JUST DID, for the seam assertion — which can say a step ran too long but not what it was
   doing, so a label naming one of three unrelated things is not the localisation it exists to be. */
static const char *engine_orphan_unit(int r) {
    switch (r) {
    case ORPHAN_STEP_SEEDED:  return "seed-one-orphan-flow";
    case ORPHAN_STEP_ROUTED:  return "hand-a-parked-drive-its-function";
    case ORPHAN_STEP_RESUMED: return "resume-a-parked-orphan-drive";
    }
    DFAIL("the orphan step reported an outcome it does not have — the caller returns PROGRESS on anything "
          "non-zero, so a fourth outcome is a step whose work nothing in the run can name");
    return NULL;
}


/* `doc` IS WHICH DOCUMENT'S PROGRAM THIS IS, and it is a parameter at every entry rather than a fact the
   scheduler assumes about itself. It used to be assumed: every program a flow ran was compiled with the
   SESSION's ctx, with one `? :` for the cross-agent operation — so a document of this agent that is not the
   one the session was rooted in had no way onto the frontier at all, and the host that had such programs (a
   same-origin child navigable's classic scripts) kept a queue of its own and ran them itself. */
/* WHICH FLOW OWNS THE PROGRAM IS A PARAMETER HERE and the running flow's identity is asked one level up: a
   program the page CAUSED to run belongs to the flow that ran the code, and a JOINED document's own scripts
   belong to the boot flow this engine mints for that document before any of it has run. Both are members of the
   one frontier; only one of them has the thread. */
/* AND WHERE IN THAT SEQUENCE IT LANDS IS A PARAMETER FOR THE IDENTICAL REASON (engine.h's DynPos): the slot a
   caused program takes is a step of the SPEC of the operation that caused it, so only the caller knows it, and
   a queue that appended by default gave every one of them the same answer — the tail — including the two the
   spec runs before anything else in the sequence. */
/* AND WHICH OF §8.1.4.4's TWO ALGORITHMS EVALUATES IT, AND THE ADDRESS ITS BYTES CAME FROM, for the same
   reason again: both are facts about the ELEMENT (or about the absence of one), which only the caller has.
   `stype` is SCRIPT_TYPE_CLASSIC for every row that is not an element's — a `setTimeout` string, a
   `javascript:` URL, a lazy chunk, a cross-agent operation's program — and that is §8.1.4.4's answer for those
   rather than a default. `url` is NULL for an INLINE row, whose base URL §4.12.1.1 states as the document's. */
/* `el` IS THE `script` ELEMENT THE ROW IS THE PROGRAM OF, or NULL for a row no element caused — see
   solver/flow.h's `dyn_el`, which states why the element is a fact about the ROW rather than something the
   completion could re-derive, and why NULL is a positive statement rather than a hole. */
/* `body` IS THE SHARED PROGRAM TEXT AND THIS ENTRY TAKES A REFERENCE ON IT — the caller keeps its own and
   releases it. That is what makes the row's cost O(1) at every seed and every fork: the bytes belong to the
   program, not to the timeline holding it (solver/dyn_body.h). */
static void engine_queue_into(Flow *f, uint32_t doc, DynBody *body, DynKind kind, ScriptType stype,
                              const char *url, char *token, DynPos pos, lxb_dom_element_t *el) {
    int at;
    /* A PROGRAM QUEUED WITH NO FLOW IS A DROPPED PROGRAM, and it used to leave silently. There is no global
       queue to fall back to — the frontier IS the queue — so the caller is the one that has to name the flow
       whose sequence this program joins: an injected <script>'s insertion, a document's own load job, a fired
       PoC, a joined document's boot. A document whose scripts vanished here is indistinguishable from a
       document that had none, which is exactly what this file's own routed-record asserts exist to prevent. */
    DCHECK(f != NULL, "a program was queued naming no flow — a program is a work item of the ONE frontier and "
                      "there is no member to give it to, so it would be dropped without a trace");
    DCHECK(body != NULL, "a program was queued with no body — the caller has nothing to run and the queue "
                         "entry would be a slot the compile below dereferences");
    /* THE `if (!body || !f) return;` THAT STOOD HERE IS GONE. It was a silent drop past the two asserts above,
       and this function's own comment says what that costs: "a document whose scripts vanished here is
       indistinguishable from a document that had none". Both conditions are established by every caller — the
       seed asserts its flow, the two running-flow entries assert `flow_running()`, and each entry asserts its
       body — so what the guard could still do in release was make a program disappear rather than crash. */
    DCHECK(doc != 0, "a program was queued naming no document — the realm it is compiled in is a fact about "
                     "the document it belongs to, and a program with none would be compiled in whichever realm "
                     "the session happens to be rooted at and read that Window's globals as its own");
    /* THE KIND AND THE TOKEN ARE ONE STATEMENT, ASSERTED IN BOTH DIRECTIONS. A cross-agent operation's row owes
       the peer parked on it an answer and carries its rendezvous token (flow.h); every other kind owes nobody
       anything. A row of that kind with no token is a peer suspended at the line that asked for the rest of the
       session with nothing in this engine that could say so; a token on any other kind is an answer that will
       never be sent, because only that kind's completion is read as one. Stated here, at the ONE site that
       creates a row, so neither can be forgotten at a call site. */
    DCHECK((kind == DYN_CROSS_AGENT_OP) == (token != NULL),
           "a queued program's kind and its rendezvous token disagree — a cross-agent operation's row must "
           "carry the token of the peer waiting on its completion, and no other kind may carry one, because "
           "only that kind's completion is ever read as an answer");
    /* THE SEQUENCE HOLDS ONLY EXECUTABLE PROGRAMS, asserted at the ONE site that creates a row rather than at
       the compile that would then have to pick an algorithm for a program that has none. §4.12.1.1's null type
       and its two data types run nothing at all, so an import map or a set of speculation rules never becomes
       a row — document_exec_scripts and html_script_prepare both drop them before they reach here. */
    DCHECK(script_type_executes(stype),
           "a program was queued with a script type that does not execute — §4.12.1.1's null type, an import "
           "map and a set of speculation rules are registered or ignored rather than evaluated, so none of "
           "them is a program, and §8.1.4.4 has no entry to run this row with");
    /* AN ADDRESS BELONGS TO A ROW WHOSE BYTES CAME FROM ONE. §8.1.4.2 creates a script based on the RESPONSE'S
       URL, so a row that was fetched has one and a row the page wrote inline does not — and §4.12.1.1 answers
       the inline case from the document instead ("let base URL be el's node document's document base URL"),
       which is what the compile reads when this column is NULL. A DYN_SCRIPT_SRC row is the one shape that is
       an address and not yet a program, so its address is its own body until the reply arrives. */
    DCHECK(kind != DYN_SCRIPT_SRC || url == NULL,
           "an entry holding a script's ADDRESS was queued with a second address beside it — the row IS the "
           "URL until flow_deliver_one_reply replaces it with the source text, and that delivery is what moves "
           "it into the address column, so a caller writing both is naming one script two ways");
    if (f->dyn_n >= f->dyn_cap) {
        f->dyn_cap = f->dyn_cap ? f->dyn_cap * 2 : 8;
        f->dyn = realloc(f->dyn, (size_t)f->dyn_cap * sizeof(DynBody *));
        f->dyn_cand = realloc(f->dyn_cand, (size_t)f->dyn_cap);
        f->dyn_type = realloc(f->dyn_type, (size_t)f->dyn_cap);
        f->dyn_url = realloc(f->dyn_url, (size_t)f->dyn_cap * sizeof(char *));
        f->dyn_el = realloc(f->dyn_el, (size_t)f->dyn_cap * sizeof(lxb_dom_element_t *));
        f->dyn_doc = realloc(f->dyn_doc, (size_t)f->dyn_cap * sizeof(uint32_t));
        f->dyn_token = realloc(f->dyn_token, (size_t)f->dyn_cap * sizeof(char *));
        f->dyn_pos = realloc(f->dyn_pos, (size_t)f->dyn_cap);
        CHECK(f->dyn && f->dyn_cand && f->dyn_type && f->dyn_url && f->dyn_el && f->dyn_doc && f->dyn_token &&
              f->dyn_pos,
              "engine: OOM dynamic-script queue");
    }
    at = f->dyn_n;
    if (pos == DYN_POS_IMMEDIATE) {
        /* "IMMEDIATELY" IS A POSITION RELATIVE TO THE RUNNING PROGRAM, so it is meaningless for any other
           flow: the slot is the one after the cursor of the flow that HAS the thread. Queued into a sibling it
           would name a position in a sequence that flow is nowhere near. */
        DCHECK(f == flow_running(),
               "a program was queued to run IMMEDIATELY into a flow that is not the running one — "
               "`immediately` names the slot after the program that caused it, and the causing program is in "
               "the running flow, so this row would interpose into a stranger's sequence at its cursor");
        /* THE CURSOR ITSELF WHEN THERE IS NO PROGRAM, THE SLOT AFTER IT WHEN THERE IS. `frame` is exactly
           "inside a program" (flow.h) — flow_step runs a job only under `!f->frame` — so a script element a
           page inserts from a `.then` reaction is at the cursor, where the sequence has nothing yet, and one
           inserted from a running <script> is at the slot after it. One expression, both cases, and neither is
           the tail. */
        at = f->frame ? f->script_i + 1 : f->script_i;
        /* AND EVERY SLOT THE CURSOR CAN BE AT IS NOW NAMEABLE, which is what deleted the `CHECK` that used to
           stand here. The cursor used to walk the session document's static sequence [0, n) before this flow's
           own rows, so `at` was `cursor - n` and went NEGATIVE for a page that `eval`d or injected from any
           <script> but its LAST — the one position the representation could not express, and a `CHECK` rather
           than a DCHECK because a negative index is a memmove outside the arrays, so a real page aborted the
           engine in release. The session document's scripts are rows of THIS table now (engine_seed_scripts),
           so `at` is an index into the one sequence the cursor indexes and there is no offset left to go
           negative. What survives is the upper bound, which is a different claim entirely: it is about the
           cursor and the queue agreeing, not about a position the queue lacks. */
        CHECK(at <= f->dyn_n,
              "a program's IMMEDIATE slot is past the end of the flow's own queue — the cursor names the row "
              "the flow is at, so the slot after it is at most one past the last row, and a larger index "
              "means the cursor and the queue disagree about how many programs this flow has");
        /* A ROW THAT SHIFTS IS A ROW SOMEBODY MAY BE HOLDING THE INDEX OF. engine_pending_docscript records an
           ABSOLUTE sequence position on the register and flow_deliver_one_reply writes the fetched source into
           `f->dyn[scriptI]`, so an interposition below an outstanding one would deliver a document's
           script text into the wrong row — a program silently replaced by another document's bytes. It cannot
           happen (a flow parks on one of those with NO frame and cannot leave that slot until the reply fills
           it, and `frame` is the same predicate the position above is derived from), which is exactly why it is
           asserted rather than handled. */
        DCHECK(at == f->dyn_n || pending_count_kind(f->pending, FLOW_PENDING_DOCSCRIPT) == 0,
               "a program was interposed into a flow that is still owed an external document script — that "
               "reply names its slot by ABSOLUTE index, so the shift would deliver the fetched source into "
               "whichever row moved into that position");
    }
    if (at < f->dyn_n) {
        size_t tail = (size_t)(f->dyn_n - at);
        memmove(&f->dyn[at + 1],       &f->dyn[at],       tail * sizeof(DynBody *));
        memmove(&f->dyn_cand[at + 1],  &f->dyn_cand[at],  tail);
        memmove(&f->dyn_type[at + 1],  &f->dyn_type[at],  tail);
        memmove(&f->dyn_url[at + 1],   &f->dyn_url[at],   tail * sizeof(char *));
        memmove(&f->dyn_el[at + 1],    &f->dyn_el[at],    tail * sizeof(lxb_dom_element_t *));
        memmove(&f->dyn_doc[at + 1],   &f->dyn_doc[at],   tail * sizeof(uint32_t));
        memmove(&f->dyn_token[at + 1], &f->dyn_token[at], tail * sizeof(char *));
        memmove(&f->dyn_pos[at + 1],   &f->dyn_pos[at],   tail);
    }
    /* ONE REFERENCE, NOT ONE COPY — and therefore nothing here can fail for want of memory. The row now costs
       a pointer whatever the program's size, which is what makes a document's bundle cost the instance one
       buffer instead of one per timeline that runs it. */
    f->dyn[at] = dyn_body_ref(body);
    f->dyn_cand[at] = (unsigned char)kind;
    f->dyn_type[at] = (unsigned char)stype;
    f->dyn_url[at] = NULL;
    if (url) { f->dyn_url[at] = strdup(url); CHECK(f->dyn_url[at], "engine: OOM queued script address"); }
    /* THE ELEMENT IS BORROWED AND NOT COPIED — there is no second element to make, and nothing to fail. */
    f->dyn_el[at] = el;
    f->dyn_doc[at] = doc;
    f->dyn_token[at] = token;   /* MOVED: one allocation from engine_perform to flow_answer_perform */
    /* THE POSITION IS KEPT, NOT CONSUMED. `at` says where the row went; this says WHY, and the microtask
       checkpoint reads it (flow_checkpoint_due) because §4.12.1.1's immediate program ran inside the causing
       program and §8.1.4.4's checkpoint is owed only once that program's stack has emptied. */
    f->dyn_pos[at] = (unsigned char)pos;
    f->dyn_n++;
}

/* THE ROW A CALLER HOLDS ALREADY-DECODED BYTES FOR. `body` is the shared text and this entry does NOT take it
   over: the caller keeps its reference and releases it, exactly as engine_queue_into's does, so a caller that
   queues one program into several places pays for the bytes once. */
static void engine_queue_el_body(uint32_t doc, DynBody *body, DynKind kind, ScriptType stype, const char *url,
                                 DynPos pos, lxb_dom_element_t *el) {
    Flow *f = flow_running();   /* the running flow owns the lazy chunk it loads */
    DCHECK(f != NULL, "a program was queued with no flow running — a program is a work item of the ONE "
                      "frontier and there is no member to give it to, so it would be dropped without a trace");
    engine_queue_into(f, doc, body, kind, stype, url, NULL, pos, el);
}

/* …AND THE ROW A CALLER HOLDS AS BYTES IT HAS NOT SHARED — a `setTimeout` body, a `javascript:` URL, an @S
   candidate, a cross-agent operation's program. The one copy those need is made HERE and released HERE, so the
   sharing is the only thing the rest of the engine has to know about.
   IT IS `(body, body_n)` AND NOT A C STRING, and the callers are why rather than symmetry: a `javascript:` URL
   percent-decodes to a byte sequence in which `%00` is an ordinary byte, an @S candidate is a payload built
   from attacker input, and an injected `<script>`'s text is whatever a page assigned to `.textContent`. None of
   those went through HTML §13.2.5.4 "Script data state", which is the one thing that would have made a NUL
   impossible (it emits a U+FFFD instead), so each of them can hold one and each of them was being read to the
   first. */
static void engine_queue_el(uint32_t doc, const char *body, size_t body_n, DynKind kind, ScriptType stype,
                            const char *url, DynPos pos, lxb_dom_element_t *el) {
    DynBody *b;

    DCHECK(body != NULL, "a program was queued with no body — the caller has nothing to run and the queue "
                         "entry would be a slot the compile below dereferences");
    b = dyn_body_new(body, body_n);
    CHECK(b, "engine: OOM dynamic-script body");
    engine_queue_el_body(doc, b, kind, stype, url, pos, el);
    dyn_body_unref(b);
}

/* …AND THE ROWS NO ELEMENT CAUSED. §4.12.1.1's "execute the script element" never runs for these — a §8.6
   string handler, a lazy chunk's reply, §7.4.2.3.2's `javascript:` URL, an @S candidate and a cross-agent
   operation's program are classic scripts §8.1.4.4 evaluates with no element behind them — so the document's
   §3.1.7 `currentScript` stays null while they run, which is that section's own answer and not a default this
   entry picks. It is a separate entry rather than a NULL at each call site so that a caller that DOES hold an
   element cannot pass nothing by omission. */
static void engine_queue(uint32_t doc, const char *body, size_t body_n, DynKind kind, ScriptType stype,
                         const char *url, DynPos pos) {
    engine_queue_el(doc, body, body_n, kind, stype, url, pos, NULL);
}

/* A DOCUMENT'S SCRIPT INVENTORY, SEEDED AS THE ROWS OF ONE FLOW'S SEQUENCE — the ONE thing that turns a
   document's <script> elements into work items, used by the root document's seeding (flow_set_seed_hook) and
   by a joined document's boot flow, because those are the same operation about two documents.
   §4.12.1's ORDER IS THE SEQUENCE'S ORDER, and both halves are positions in it: a row whose SOURCE TEXT is
   here is a program queued in place, a row that is still owed its bytes is a DYN_SCRIPT_SRC row queued in
   place holding its address until the reply fills it and stopping the flow there. `urls` are already
   §4.12.1.1's encoding-parsed addresses — the resolution is the CALLER's because it belongs to the document
   whose elements these are, and it is computed once per element rather than once per flow.
   WHICH OF THE TWO A ROW IS, IS ASKED OF ITS BODY AND NOT OF ITS ADDRESS. They are the two independent items
   HTML §8.1.4.1 "Scripts" gives a script — source text, and "a base URL … either the URL from which the script
   was obtained, for external scripts, or the document base URL of the containing document, for inline
   scripts" — so a host that already holds an external script's response hands over a row with BOTH, and that
   row is a program AT its own address.
   A ROW THAT IS NEITHER IS NOT A ROW. §4.12.1.1: "If url is failure, then queue an element task on the DOM
   manipulation task source given el to fire an event named error at el, and return" — that element runs no
   script, so it takes no position. What is still owed is the error event, which needs a task on the document
   rather than anything here. */
static void engine_seed_scripts(Flow *f, uint32_t doc, const RootScript *rows, int n) {
    int i;

    DCHECK(f != NULL, "a document's scripts were seeded onto no flow");
    DCHECK(n == 0 || rows != NULL,
           "a document was seeded with a script count and no table to read it from");
    DCHECK(f->dyn_n == 0 && f->script_i == 0,
           "a document's scripts were seeded onto a flow that already holds programs or has moved past its "
           "first — a flow is seeded once, at creation, so a second seeding would interleave two documents' "
           "program orders into one sequence and leave the cursor pointing into neither");
    for (i = 0; i < n; i++) {
        DCHECK(script_type_executes(rows[i].type),
               "a script table holds a row whose type executes nothing — §4.12.1.1's null type, an import map "
               "and a set of speculation rules are registered or ignored rather than evaluated, so "
               "document_exec_scripts drops all three before they become rows");
        DCHECK(rows[i].body != NULL || rows[i].url != NULL,
               "a script table row holds neither source text nor an address — §4.12.1.1's \"if url is failure "
               "… fire an event named error at el, and return\" means such an element runs no script and takes "
               "no position, so a row that is nothing would park the flow on nothing for the rest of the "
               "session");
        /* THE POSITION A ROW IS QUEUED AT IS THE POSITION IT IS ADDRESSED AT, which is the whole point of the
           one sequence: `i` is this element's place in §4.12.1's document order and the cursor indexes the same
           table, so the two cannot come apart. Asserted per row rather than described once. */
        DCHECK(f->dyn_n == i, "a document's scripts left the seed out of document order — the row about to be "
                              "queued is not the one the sequence is at, so the flow's cursor would reach this "
                              "element at some other element's position");
        /* AND THE ELEMENT TRAVELS WITH THE ROW, at both positions: §4.12.1.1's "execute the script element"
           is a switch on EL whichever half of §4.12.1 put the row here, and an EXTERNAL row is that element's
           program from the moment it takes its slot rather than from the moment its bytes arrive.
           NULL IS A STATEMENT AND NOT A HOLE — a host driving a SYNTHESIZED program list (wpt_runner.c's
           harness prologue and epilogue) has rows no `<script>` produced, and §3.1.7's answer while one of
           those runs is null, which is the truth about a document that is not executing a script element. */
        /* …AND SO DOES THE ADDRESS, WHEN THE ROW HAS BOTH. A row whose source text is already here is a
           PROGRAM (DYN_PAGE_SCRIPT), and §8.1.4.2 "Fetching scripts" says what its base URL is — "creating a
           classic script given sourceText, settingsObject, response's URL, …" — so the address rides into the
           row's own address column and flow_dyn_url answers it at the compile. Passing NULL here is what made
           every pre-fetched external script compile under its document's name, which for a module is the
           module map key: §8.1.4.2's own note, "the base URL for the module script is set to the response
           URL … used for URL resolution". A row with only an address is still the DYN_SCRIPT_SRC shape, whose
           body IS its URL until flow_deliver_one_reply takes it across. */
        /* THE PROGRAM ROW REFERENCES THE TABLE'S OWN BODY — this is the seeding that used to copy a document's
           whole bundle into every flow it created. The ADDRESS row still makes a body, because its body IS the
           address until the reply replaces it and that string is the table's, not this row's; it is tens of
           bytes and it is released here, the row keeping the reference engine_queue_into took. */
        if (rows[i].body) {
            engine_queue_into(f, doc, rows[i].body, DYN_PAGE_SCRIPT, rows[i].type, rows[i].url,
                              NULL, DYN_POS_APPEND, rows[i].el);
        } else {
            /* AN ADDRESS IS THE ONE BODY WHOSE LENGTH IS ITS `strlen`, and this is where that is stated. It
               came out of script_src_absolute, which serializes a parsed URL record (URL §4.5 "URL
               serializing"), and every component of one has been through a percent-encode set that contains
               U+0000: URL §1.3 "Percent-encoded bytes" defines the C0 control percent-encode set as "C0
               controls and all code points greater than U+007E", and every other set in that section is
               defined as containing it. So a serialized URL holds `%00` where a NUL was, never the byte, and
               the two agree by construction here — flow_deliver_one_reply ASSERTS they still do at the one read
               that depends on it. */
            DynBody *addr = dyn_body_new(rows[i].url, strlen(rows[i].url));
            CHECK(addr, "engine: OOM seeding an external script's address as its row's body");
            engine_queue_into(f, doc, addr, DYN_SCRIPT_SRC, rows[i].type, NULL, NULL, DYN_POS_APPEND,
                              rows[i].el);
            dyn_body_unref(addr);
        }
    }
}

/* …AND THE ROOT DOCUMENT'S, which is what a fresh timeline of THIS instance's document is made of. It is the
   seed hook flow_add asks (solver/flow.h), so the boot flow, every cold-resumed replay and every @S candidate
   session get the same sequence by construction rather than by a line copied into each of them. */
static void engine_seed_root_flow(Flow *f) {
    DCHECK(g_sess_live, "a flow was seeded with the root document's programs outside a live session — the "
                        "table it reads is built when a session opens and given back when one closes");
    engine_seed_scripts(f, g_sess_doc, g_root_scripts, g_root_n);
}

/* WHICH KIND THE PROGRAM AT `script_i` IS, asked at the two places that need it — the compile and the resume.
   It is RE-DERIVED from the cursor rather than latched in a field, because the cursor is what already says
   which program is running and a second copy of that fact is a second copy that can be behind.
   THE CURSOR INDEXES ONE TABLE. It used to walk the session document's static sequence first and this flow's
   own rows after it, through a `- n` offset that these four accessors each restated; the document's scripts
   are rows of the same table now, so there is one address space and no half to be in. Past the end is still a
   real position — the accessors are read before the step decides what the flow is doing — and it is the same
   answer it always was. */
static DynKind flow_dyn_kind(const Flow *f) {
    if (f->script_i >= f->dyn_n) return DYN_PAGE_SCRIPT;
    return (DynKind)f->dyn_cand[f->script_i];
}

/* AND WHICH DOCUMENT IT BELONGS TO, re-derived from the same cursor and for the same reason. Past the end of
   the sequence there is no row to ask, and the flow's own document is the session's. */
static uint32_t flow_dyn_doc(const Flow *f) {
    if (f->script_i >= f->dyn_n) return g_sess_doc;
    return f->dyn_doc[f->script_i];
}

/* AND WHICH OF §8.1.4.4 "Calling scripts"'s TWO ALGORITHMS EVALUATES IT, re-derived from the same cursor for
   the same reason. It is the ROW that answers for every position now — the session document's own <script
   type=module> included, which is what it could not say while its type lived in a parallel array the dyn
   table's rows had no column for. */
static ScriptType flow_dyn_type(const Flow *f) {
    if (f->script_i >= f->dyn_n) return SCRIPT_TYPE_CLASSIC;
    return (ScriptType)f->dyn_type[f->script_i];
}

/* AND THE ADDRESS ITS BYTES CAME FROM — §8.1.4.2 "Fetching scripts"'s created-script base URL, and a module
   row's module map key. NULL for an INLINE row and for a cursor past the end of the sequence, which is
   §4.12.1.1's "let base URL be el's node document's document base URL" and is read as that by the compile. */
static const char *flow_dyn_url(const Flow *f) {
    if (f->script_i >= f->dyn_n) return NULL;
    return f->dyn_url[f->script_i];
}

/* AND THE `script` ELEMENT IT IS THE PROGRAM OF — HTML §4.12.1.1 "Processing model"'s "execute the script
   element", which is a switch on EL and whose "classic" arm brackets the whole run with the document's §3.1.7
   `currentScript`. Re-derived from the cursor for the reason the four above are, and it is the fact the
   BRACKET could not otherwise have: the run is a WORK ITEM that starts in one scheduler step and completes in
   another, with siblings running in between, so the element the completion restores has to be ANSWERABLE at
   the completion rather than remembered across it in a C local.
   NULL PAST THE END OF THE SEQUENCE, which is where a driven orphan's CALL frame sits, and NULL for every row
   no element caused. Both are §3.1.7's own answer — a document running one of those is a document that is not
   executing a script element — and neither is a hole this accessor fills. */
static lxb_dom_element_t *flow_dyn_el(const Flow *f) {
    if (f->script_i >= f->dyn_n) return NULL;
    return f->dyn_el[f->script_i];
}

/* EVERY SOURCE THAT REACHES THIS ONE IS A TASK — a lazy chunk's reply, a §8.6 string handler — so the tail IS
   its position, and the entry cannot be asked for another (engine.h's DynPos).
   AND EVERY ONE OF THEM IS A CLASSIC SCRIPT, which is §8.1.4.4's answer for a program that has no `<script>`
   element behind it rather than a default this entry happens to pick: §8.6's string handler is evaluated as a
   classic script, and a lazy chunk's reply is the body an already-running program asked for. An entry that DOES
   have an element behind it says which of the two algorithms runs it — engine_queue_element_script below. */
/* THE ONE ENTRY THAT STILL TAKES A NUL-TERMINATED BODY, AND IT IS NOT THIS FILE'S TO FIX. It is §8.6's string
   handler sink (`timer_set_script_sink`), so its shape is that hook's `void (*)(uint32_t, const char *)`, and
   the length is lost one level UP — at the `JS_ToCString` in core/timing/timer.c that turns the handler
   DOMString into C. Widening this signature without widening that hook and that conversion would move the
   truncation rather than remove it, so the length goes end to end there or not at all: a `setTimeout("\0…")`
   is still read to the first NUL. The `strlen` is written HERE, once, rather than left implicit in
   engine_queue, so that the site carrying the remaining gap is the site that names it. */
void engine_queue_script(uint32_t doc, const char *body) {
    DCHECK(body != NULL, "a §8.6 string handler was queued with no source at all — the sink is called with "
                         "the handler's DOMString and there is no program in a null one");
    engine_queue(doc, body, strlen(body), DYN_PAGE_SCRIPT, SCRIPT_TYPE_CLASSIC, NULL, DYN_POS_APPEND);
}

/* …AND THE ROW A `<script>` ELEMENT PUT THERE, which is the same position and one more fact: HTML §4.12.1.1
   "Processing model"'s "execute the script element" ends in a switch on the ELEMENT's type, so the row carries
   it and flow_step routes to §8.1.4.4 "Calling scripts"'s run-a-classic-script or run-a-module-script
   accordingly. Three seams reach it and they are the three ways a Document of this agent that is NOT the
   session's gets its inline programs: a child navigable's (core/frame/navigable.c), a joined one's
   (engine_join_document) and an element page code INSERTED (core/html/html_script.c). Each of those used to
   ABORT on `<script type=module>` — a DCHECK apiece, all three naming this column — because the only route was
   the classic entry above and the page's own `import` would have come back a SyntaxError.
   THE TAIL IS ITS POSITION and that is §4.12.1.1's own answer for every destination an element with a MODULE
   type or a `src` reaches: the `list of scripts that will execute when the document has finished parsing` and
   the `list of scripts that will execute in order as soon as possible` both hold their elements in order, and
   the `set of scripts that will execute as soon as possible` has no position at all (§13.2.7 waits for it only
   before the load event), so arrival order is a correct order for it. The ONE destination that is not the tail
   is `immediately execute the script element`, which §4.12.1.1 reaches only for an inline CLASSIC script and
   which has its own entry below. */
/* …AND IT CARRIES THE ELEMENT, because "execute the script element" is a switch on EL and its classic arm
   brackets the run with that document's §3.1.7 `currentScript`. */
void engine_queue_element_script(uint32_t doc, const char *body, size_t body_n, ScriptType stype,
                                 lxb_dom_element_t *el) {
    DCHECK(el != NULL, "a `<script>` element's program was queued with no element — this entry is the one an "
                       "ELEMENT reaches (the other is engine_queue_script), so a caller with nothing to pass "
                       "is a caller at the wrong entry");
    engine_queue_el(doc, body, body_n, DYN_PAGE_SCRIPT, stype, NULL, DYN_POS_APPEND, el);
}

/* …AND THE ONE SCRIPT SOURCE THAT IS NOT A TASK. HTML §4.12.1.1 "Processing model" ends "prepare the script
   element" with "Otherwise, immediately execute the script element el, even if other scripts are already
   executing" — so an inline classic script a page INSERTED runs at the slot after the program that inserted it,
   and everything the sequence already holds runs after it. This engine had the classification (html_script.c
   computes SCRIPT_SCHED_IMMEDIATE and its own DCHECK names this very step) and then queued the result at the
   tail, which is the position of the one destination §4.12.1 says it does NOT take. */
/* THE TYPE IS CLASSIC AND IS NOT A PARAMETER, because §4.12.1.1 reaches this step for no other: "If el's type
   is `classic` and el has a src attribute, OR el's type is `module`" sends every module — inline or not — to
   one of the three lists above, and only what falls past that switch reaches "Otherwise, immediately execute
   the script element el". An inline module has a graph to load before its result exists, which is exactly why
   the standard does not run it here. */
void engine_queue_script_immediate(uint32_t doc, const char *body, size_t body_n, lxb_dom_element_t *el) {
    DCHECK(el != NULL, "an inline classic script was queued to run IMMEDIATELY with no element — §4.12.1.1 "
                       "reaches `immediately execute the script element` from `prepare the script element`, "
                       "whose whole subject is EL");
    engine_queue_el(doc, body, body_n, DYN_PAGE_SCRIPT, SCRIPT_TYPE_CLASSIC, NULL, DYN_POS_IMMEDIATE, el);
}

/* …AND ITS EXTERNAL SIBLING, which takes the same position with only an ADDRESS — see DYN_SCRIPT_SRC. The
   caller resolved the URL because §4.4's API base URL belongs to the document whose element it is.
   APPEND, and that is §4.12.1's own answer rather than a default: this entry is the `list of scripts that will
   execute in order as soon as possible`, whose elements hold their places against one another, so a new one
   goes behind the ones already there. */
void engine_queue_docscript_url(uint32_t doc, const char *url, ScriptType stype, lxb_dom_element_t *el) {
    /* THE ADDRESS IS THE ROW'S BODY, NOT ITS ADDRESS COLUMN — the row IS the URL until the reply replaces it
       with the source text, and flow_deliver_one_reply is what MOVES it into the address column at that moment.
       Writing both here would name one script two ways and engine_queue_into asserts against it. */
    DCHECK(el != NULL, "an external document script took its slot with no element — the row is that element's "
                       "program from the moment it takes the position, not from the moment its bytes arrive, "
                       "and §4.12.1.1's `execute the script element` is a switch on EL");
    /* AND ITS LENGTH IS ITS `strlen`, WHICH IS A FACT ABOUT AN ADDRESS AND NOT A DEFAULT. The caller resolved
       this with script_src_absolute, so it is a serialized URL (URL §4.5 "URL serializing") and every one of
       its components went through a percent-encode set built on URL §1.3's C0 control percent-encode set —
       "C0 controls and all code points greater than U+007E" — which contains U+0000. flow_deliver_one_reply
       asserts the pair again at the read that turns this body back into a C string. */
    engine_queue_el(doc, url, strlen(url), DYN_SCRIPT_SRC, stype, NULL, DYN_POS_APPEND, el);
}

/* AN @S CANDIDATE, queued as the program it would be if it fired. It is the same queue because it IS the same
   thing — code the page caused to run — but it carries the one difference that matters: it is allowed not to
   compile. Most breakouts do not fit most sink contexts, which is exactly why the solver tries several and
   keeps whichever FIRES; a candidate that does not parse simply never fires. */
/* THE DOCUMENT IS THE SESSION'S, and that is what a candidate IS: the same document re-run with one attacker
   value substituted for one source. It is stated here rather than taken as a parameter because there is no
   other document it could be — a breakout that fired in a child navigable is a candidate seeded against that
   document, which is a session of that document's instance. */
/* AND `pos` IS THE SINK'S OWN SEMANTICS — see engine.h. An eval sink's code is ECMAScript §19.2.1.1
   PerformEval, which evaluates the body inside the call expression; a markup sink's auto-firing handler and a
   URL sink's `javascript:` navigation are tasks. The caller knows which sink fired, so the caller says. */
/* AND IT IS `(body, body_n)` FOR A REASON THAT IS THIS HALF OF THE PROJECT'S OWN: a candidate is BUILT out of
   attacker-shaped bytes — a percent-decoded `%00` in a hash, a `\0` in a JSON string a reply carried — and the
   solver's whole claim is that the candidate it fires is the bytes it constructed. Read to the first NUL, a
   candidate carrying one fires a DIFFERENT program from the one the search decided on, and the verdict
   ("no hit") would be about a payload nobody chose. */
void engine_queue_candidate(const char *body, size_t body_n, DynPos pos) {
    engine_queue(g_sess_doc, body, body_n, DYN_CANDIDATE, SCRIPT_TYPE_CLASSIC, NULL, pos);
}

/* HTML §7.4.2.3.2's EVALUATE A JAVASCRIPT: URL, steps 6-7 — "let script be the result of creating a classic
   script given scriptSource … let evaluationStatus be the result of running the classic script script". The
   source is the page's own code, so it is a program of the running flow like a lazy chunk: preemptible,
   forkable and parkable, which a C `JS_Eval` under the live flow could never be.
   `doc` IS THE TARGET NAVIGABLE'S ACTIVE DOCUMENT, which step 5 states outright — "let settings be
   targetNavigable's active document's relevant settings object" — and that document is not always the
   session's: a `<form action="javascript:…" target=frame>` runs its program in the FRAME's realm, where the
   globals it writes are the ones a later script of that document reads. */
/* APPEND, and HTML §7.4.2.2 "Beginning navigation" is why: it reaches this case by "Queue a global task on the
   navigation and traversal task source given navigable's active window to navigate to a javascript: URL". The
   evaluation this row runs is therefore a TASK, and it takes the tail like every other one. */
/* AND `body_n` IS STEP 3's OWN ANSWER. "Let scriptSource be the UTF-8 decoding of the percent-decoding of
   encodedScriptSource" — URL §1.3's percent-decode reaches all 256 byte values, so `javascript:a=%00` decodes
   to a source text with a U+0000 in it, which ECMAScript §11.1 "Source Text" permits ("All Unicode code point
   values from U+0000 to U+10FFFF … may occur in ECMAScript source text where permitted by the ECMAScript
   grammars"). navigable.c used to assert that this could not happen, naming the length as the thing to build;
   the length is the parameter. */
void engine_queue_javascript_url(uint32_t doc, const char *body, size_t body_n) {
    /* CLASSIC, and §7.4.2.3.2 step 6 says so in as many words — "let script be the result of creating a CLASSIC
       script given scriptSource" — so there is nothing here to parameterise. */
    engine_queue(doc, body, body_n, DYN_JAVASCRIPT_URL, SCRIPT_TYPE_CLASSIC, NULL, DYN_POS_APPEND);
}

/* THE OPERATION BECOMES THIS FLOW'S NEXT PROGRAM. Not a call: a peer answers by RUNNING a program, and every one
   of these is the page's own code — an IDL getter, a page's setter, a page's function — which a C activation
   has no flow base under. Queued with the flow switched in, so the operands the program reads are written into
   THIS flow's delta and no sibling sees them. */
/* AND IT RUNS IN THE REALM OF THE DOCUMENT THE PEER NAMED. Every operand is installed there and the program is
   compiled there (flow_step), because that is what the operation IS: §7.2.1's member is read of the OTHER
   navigable's active document, and a getter answered out of this instance's root would count the root's child
   navigables and hand them back as the child's. The document is carried as the HANDLE that crossed the wire
   rather than as a JSContext, for the reason every other queued platform datum is a name and not a pointer: a
   handle survives a park and a realm does not — and it is carried ON THE ROW, because `dyn_doc` is already the
   field that says which realm a queued program is compiled in. */
static void flow_perform(JSContext *ctx, Flow *f)
{
    JSValue e, rv, tv;
    const char *record, *token;
    RemoteOp *op;
    WorldId w;
    const WorldId *anc;
    int n_anc;
    CowDelta *seg;
    JSContext *rctx;
    uint32_t doc;

    DCHECK(flow_running() == f, "a cross-agent operation was performed while another flow was switched in — its "
                                "operands would be written into that flow's delta and its program would run "
                                "against that flow's document");
    e  = perform_q_take(ctx, f);
    rv = JS_GetPropertyUint32(ctx, e, 0);
    tv = JS_GetPropertyUint32(ctx, e, 1);
    DCHECK(JS_IsString(rv) && JS_IsString(tv),
           "a queued cross-agent operation is missing its record or its rendezvous token — the pair is written "
           "in one bracket at arrival and never edited, so half of one is a queue something outside this file "
           "has written to");
    record = JS_ToCString(ctx, rv);
    token  = JS_ToCString(ctx, tv);
    CHECK(record != NULL && token != NULL,
          "engine: OOM reading a queued cross-agent operation — a record this instance cannot read is a peer's "
          "flow suspended at the read that asked, with nothing left that knows what it asked");
    op = remote_op_parse(record);
    doc = world_doc_intern(remote_op_doc(op));
    rctx = doc_realm(doc);
    n_anc = world_parse(remote_op_worlds(op), &w, &anc);
    /* ASKED AGAIN AT THE MOMENT IT RUNS, for the reason flow_deliver asks it again: the scheduler has run other
       flows since the record arrived, and which world holds writes here is a property of the run. */
    seg = world_segment(ctx, w, anc, n_anc);
    (void)seg;
    DCHECK(cow_delta_empty(seg),
           "a cross-agent operation ran in the answering flow's timeline alone while the asking world holds "
           "writes in this instance — it answers about a document missing everything the asking flow did here. "
           "Build the join of the two deltas that engine_route names");
    /* THE ENTRY IS SPENT HERE AND WHAT SURVIVES IT IS THE TOKEN. The record has become a program and has
       nothing left to say; the token is COPIED out of the queue entry onto that program's row, because the
       answer is the program's COMPLETION and the row is what says which completion. `strdup` and not a move:
       the entry is a JS value that a forked ARM may still name, so the row owns a C string of its own and the
       entry's own reference dies with the JS_FreeValue below. Nothing about this operation is left on the
       flow, which is what lets the next entry start on a flow that is already performing one. */
    {
        char *own = strdup(token);
        /* THE PROGRAM IS READ BEFORE THE ROW IS MADE because reading it is what SETS the operation's operands
           on the answering realm's global (remote_op.c) — one call, and the row's body is what it answered. */
        /* AND ITS LENGTH IS ITS `strlen` BECAUSE THIS ENGINE WROTE IT. remote_op_program answers one of this
           file's own fixed programs for a §7.2.1 member, not page text and not anything an attacker reached,
           so there is no U+0000 to lose — which is the whole content of stating the length at the site rather
           than letting an entry point compute it out of sight. */
        const char *prog_src = remote_op_program(rctx, op);
        DynBody *prog = dyn_body_new(prog_src, strlen(prog_src));
        CHECK(own != NULL, "engine: OOM moving a cross-agent operation's rendezvous token onto its program");
        CHECK(prog != NULL, "engine: OOM making the body of a cross-agent operation's program");
        /* NO ELEMENT: this program is the ENGINE'S own text, not a `<script>`, so §4.12.1.1's "execute the
           script element" is not the algorithm that runs it and the peer document's §3.1.7 `currentScript`
           stays null while it does — which is the truth about a document answering a cross-agent read. */
        engine_queue_into(f, doc, prog, DYN_CROSS_AGENT_OP, SCRIPT_TYPE_CLASSIC,
                          NULL, own, DYN_POS_APPEND, NULL);
        dyn_body_unref(prog);
    }
    remote_op_free(op);
    JS_FreeCString(ctx, record);
    JS_FreeCString(ctx, token);
    JS_FreeValue(ctx, rv);
    JS_FreeValue(ctx, tv);
    JS_FreeValue(ctx, e);
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
    /* THE ROW THE FLOW IS STANDING ON IS THE QUESTION IT IS ANSWERING — the cursor IS the row index now, so
       there is no session-script count to subtract. No bounds guard, because the caller has already read this
       row's KIND to get here and only a row inside the queue has one. */
    int row = f->script_i;
    char *token;
    char *enc, *rec;
    size_t cap;
    /* THE ANSWER SAYS WHICH TIMELINE COMPUTED IT, and it is not a diagnostic. This document's state IS its
       flows, so the operation was performed by every one of them and the asking instance receives N completions
       under ONE rendezvous token — which, with the timelines unnamed, is N interchangeable claims about one
       question. Measured before this field existed: the routing zone held them in a one-slot map keyed by the
       token, kept whichever arrived last and dropped the rest, and one page's `(typeof w.closed) + ":" +
       w.closed` came back `true` at one read and `false` at the next out of two CONTRADICTORY timelines of this
       document. The name is world_serialize's, the ONE spelling of a world on the wire (solver/world.h), so the
       asker can compare it, record it beside the answer it belongs to, and refuse a second delivery of it. */
    char world[1024];

    DCHECK(row >= 0 && row < f->dyn_n,
           "a cross-agent operation was answered from a cursor that is not on the queue — the kind that "
           "selected this call is read from that same row, so the two cursors have come apart");
    token = f->dyn_token[row];
    DCHECK(token != NULL,
           "a cross-agent operation's program completed with no rendezvous token on its row — the completion "
           "names no question, so the flow that asked would park on it forever. Two operations differing only "
           "in the asking WORLD are two rows and nothing else tells them apart, so this cannot be recovered "
           "from the flow");
    if (JS_IsException(cv)) {
        thrown = JS_GetException(ctx);
        completion = ENGINE_COMPLETION_THROW;
        cv = thrown;
    }
    /* ENCODED IN THE REALM THE PROGRAM RAN IN, because the value is that realm's — §3.7 gives every realm its
       own intrinsics, and a value converted through another document's is converted by a platform that is not
       the one that produced it. It is the same realm the program was compiled in, asked the same way. */
    enc = remote_completion_encode(doc_realm(flow_dyn_doc(f)), completion, cv);
    /* world_serialize CRASHES on its own truncation rather than sending a prefix, which is what makes the name
       on this notice the same name every other record of this world carries. */
    world_serialize(f->world, world, sizeof world);
    cap = strlen(token) + strlen(world) + strlen(enc) + 24;
    rec = malloc(cap);
    CHECK(rec != NULL, "engine: OOM writing a cross-agent operation's answer — a dropped answer parks the "
                       "asking flow on a question nothing will answer again");
    /* THE WORLD SITS BEFORE THE COMPLETION because the completion is the record's REMAINDER: a value grammar
       may contain a tab, and a world vector may not (world_serialize's fields are ':' and ','). */
    snprintf(rec, cap, "remoteop.answer\t%s\t%s\t%s", token, world, enc);
    engine_host_notify(ctx, rec);
    free(rec);
    free(enc);
    JS_FreeValue(ctx, thrown);
    /* THE ROW STOPS OWING, and it is the row rather than the flow that stops: this program's question has been
       answered and the flow's other rows are other questions, each still holding its own token. The kind stays
       DYN_CROSS_AGENT_OP — it is what the program IS, and a completion is read once (the cursor advances) —
       so what an answered row asserts from here is that nothing asks it for a token again. */
    free(f->dyn_token[row]); f->dyn_token[row] = NULL;
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
   WHAT IS CACHED IS THE RIVAL'S IDENTITY AND NOT ITS WEIGHT, and the difference is a defect this file used to
   assert its way past. The sentence that stood here was: "only the RUNNING flow's weight moves between
   generation bumps — a parked flow burns no CPU, and an emit both changes `val` and bumps the generation — so
   the RIVAL's weight is constant across the cache's key." The premise is FALSE, and it became false the moment
   the aging term acquired a FAMILY half. `flow_silence_notch` reads `f->cpu + fam_us`, and `flow_age_running`
   writes `family->fam_us` after EVERY step without raising the generation — so a parked flow burns no CPU OF
   ITS OWN and its weight moves anyway, because the quantity is shared with whichever arm holds the thread. On
   a real page the whole frontier is ONE family (flow.c: every flow descends from the boot flow), so this is
   not an edge: it is every parked member of the frontier, on every step.
   AND THE STALENESS IS DIRECTIONAL, WHICH IS WHY IT COST AN ABORT AND NOT A MISSED YIELD. The notch is a FLOOR
   of `(cpu_i + fam_us) / S`, and each member straddles its own boundary at its own instant because each has
   its own `cpu_i`. So a step can carry the RIVAL's floor across while the running flow's stands still: the
   rival's true weight drops by a whole point, the cache keeps the value from before the drop, and the hook
   then yields to a flow that is no longer better while every quantity the seam assertion snapshots about the
   running flow is correctly unchanged. That is the assertion's own "a rival recomputed against a stale cache",
   fired by the cache this comment declared could not go stale.
   SO THE WEIGHT IS RECOMPUTED AT EVERY CONSULTATION, both sides of it, which is what makes the comparison the
   same question the pick answers rather than a different one. The O(flows) SCAN — finding WHICH flow is the
   rival — is what the generation key is for and still happens only on a change; evaluating one weight is two
   divisions beside a clock read the hook already performs.
   AND IT IS NOT THE FITNESS TERM, WHICH IS WORTH WRITING DOWN BECAUSE THE TWO ARRIVED TOGETHER AND THE OTHER
   ONE IS A CLAUSE IN THE ASSERTION BELOW. A `cand_dist` write cannot fire that assertion: it is made only on
   the RUNNING flow, only through flow_set_distance, and that raises the generation — so clause one is true for
   the whole remainder of the turn and the disjunction cannot be false. Watching the distance is the discipline
   the snapshot block states for ANY summand of the weight and is right on that ground alone; it is not the
   mechanism that aborts, and a reader who takes it for one will conclude this cache is sound. ONE READING
   tells the two apart: a fire with the generation EQUAL to the ranked one, which a distance write makes
   impossible and which a family-notch straddle leaves untouched.
   The cached POINTER is safe across the key for a
   reason the key itself supplies: a flow leaving the frontier is a membership change and raises the
   generation, so the rescan re-runs before a departed member could be read. That is asserted where the pointer
   is taken rather than where it is read, because membership is an O(flows) scan and the read is per-opcode. */
static unsigned g_seen_gen = 0; static Flow *g_seen_cur = NULL; static Flow *g_rival = NULL;
/* WHAT THE FLOW HOLDING THE THREAD WAS RANKED ON WHEN IT TOOK IT — the quantities the value yield's
   verdict is a pure function of, recorded at the switch-in and read by the assertion in the hook's value
   clause. They are not policy and they are not a cache: nothing is decided from them, and the hook's answer is
   identical with them removed. They exist so that the sentence "a top-ranked flow runs on at ~zero switch
   cost" is a check rather than a claim, at the one point where it can be false. */
static unsigned g_ranked_gen = 0; static double g_ranked_val = 0.0;
/* …AND THE AGING TERM'S NOTCH — this flow's silence, its own thread time since its last emission plus its fork
   family's since any arm of it last emitted, in whole COOPERATIVE QUANTA (solver/flow.h's flow_silence_notch).
   Two separate snapshots stood here, the flow's service quantum and its family's, and they are ONE now because
   the weight reads one quantity: the sum, at the price FLOW_AGE_RATE states. Keeping two would be keeping the
   coordinates of a formula this file no longer computes.
   THE UNIT IS THE ONE THE THREAD IS HANDED OUT IN, and it used to be one whole emitted FINDING — 83 quanta —
   which is the correction this snapshot carries. Between two of those the running flow's rank could not move
   on aging at all, so this variable stood still across eighty-two consecutive picks and the assertion below
   permitted nothing it should have; see the charge's own resolution assertion in engine_sched_slice. */
static int64_t g_ranked_silence = 0;
/* …AND THE COMPLETED-UNIT COUNT, which is the optimism term's whole quantity (solver/flow.h's `visits`). It
   moves at an instant no clock names — the scheduler credits it after a step that left the flow between units,
   and a flow can be re-picked without a switch-in, so this snapshot can be several units old while the silence
   notch stands still. Omitting it would make the assertion below fire on the most ordinary rank change there
   is: a flow finished a program, its bonus halved, and a sibling passed it. */
static int64_t g_ranked_visits = 0;
/* …AND THE FITNESS DISTANCE, WHICH IS A TERM OF THE WEIGHT AND SO HAS TO BE A COORDINATE OF IT. `flow_weight`
   sums FOUR quantities — `reward + dist + ucb - silence` — and this snapshot set watched three of them, so the
   one it did not watch could move the rank while the assertion below read the state as unchanged and called a
   correct yield a swap for nothing. That is not a hypothetical: it aborted the smoke fixture on the first build
   after the distance became a term, at a switch count in the ten thousands, with @S searches live.
   THE RULE THE OMISSION BROKE IS THE ONE THIS BLOCK EXISTS FOR: these are "the coordinates the verdict is a
   pure function of", so a term added to `flow_weight` and not added here makes that sentence false, and it
   makes it false in the direction that fires — a real rank change reported as an unchanged one. Anything summed
   into the weight belongs here, and the assertion is what enforces that it was not forgotten. */
static double g_ranked_dist = 0.0;
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
       THE CACHE MAY NOT LAG A MARK, AND THE PARAGRAPH THAT SAID IT COULD IS WHAT HID A REAL DEFECT FOR AS
       LONG AS IT STOOD. It read: "Marks are made during a slice and cleared only at its top, so a cached rival
       can be one that has SINCE been marked — a yield the loop then declines by re-picking the same flow, at
       the cost of one iteration. A mark can never make a flow wrongly ELIGIBLE, which is the direction that
       would cost a yield that mattered." Both halves were false. Its premise — that clears happen only at the
       top of a slice — describes a line that had already been DELETED; marks are now cleared by whatever host
       event answers them, including during a step (flow_deliver_one_reply settles the shared document-script slot
       for every flow waiting on one address). And its conclusion reasoned about marks being LAID DOWN and
       never about them being CLEARED, which is precisely the direction it declared impossible: a clear makes a
       flow ELIGIBLE that the loop's pick could not consider, so the hook's next rescan returns a rival the pick
       was never shown, and the yield fires against a flow the scheduler chose one step earlier with nothing
       whatever having changed about it. That aborted the smoke at the value-yield assertion below, which is the
       assertion doing its job. The fix is at the origin: a mark change IS a ranking change and now raises the
       frontier generation (flow.c), so this rescan condition covers it and the eligible set the hook ranks
       against is the same one the pick used. */
    if (flow_frontier_gen() != g_seen_gen || cur != g_seen_cur) {   /* (1) rescan for the rival only on change */
        g_seen_gen = flow_frontier_gen(); g_seen_cur = cur;
        g_rival = cur ? flow_rival_of(cur) : NULL;
        /* THE CACHED POINTER'S WHOLE SAFETY ARGUMENT, ASSERTED WHERE IT IS ESTABLISHED. It is read at every
           consultation until the generation moves, so it is live only because a member LEAVING the frontier is
           a membership change that raises the generation and forces this rescan. Asked here and not at the
           read: membership is an O(flows) scan and the read is per-opcode, so putting it there would make the
           dev build's hot path linear in the frontier — the same reason flow_weight may not walk. */
        DCHECK(g_rival == NULL || flow_is_member(g_rival),
               "the value yield cached a rival that is not in the frontier — the pointer is read until the "
               "generation next moves, so a departure that did not raise the generation leaves this hook "
               "ranking against freed memory");
    }
    /* (0) BLOCKED BEATS BOTH RANKINGS. A flow holding an unanswered synchronous host request cannot make
       progress no matter how it ranks, and the answer cannot arrive while it holds the thread — the host is
       only asked between steps. Deciding this by weight would re-enter it immediately and spin. */
    if (cur && flow_blocked(cur)) return 1;
    /* BOTH SIDES EVALUATED HERE, WHICH IS WHAT MAKES THIS THE SAME QUESTION THE PICK ANSWERS. What the cache
       holds is WHICH flow the rival is; what this line computes is what the two are worth right now. The
       rival's weight was cached once, on the reasoning that a parked flow's weight cannot move — see the
       cache's own declaration for why that is false, and for the abort it produced. */
    if (cur && g_rival && flow_weight(g_rival) > flow_weight(cur)) {   /* value yield */
        /* THE VALUE YIELD MAY ONLY FIRE ON A RANK CHANGE, AND THIS IS WHERE THAT IS EITHER TRUE OR A SENTENCE
           IN CLAUDE.md. §scheduler says the yield fires "the moment a parked flow outranks (or on an emit/fork/
           suspension that changes ranks)" and that "a top-ranked flow runs on at ~zero switch cost" — so a flow
           that was picked to run, and against which NOTHING has since changed, must still be running.
           WHAT CAN CHANGE THE ANSWER IS EXACTLY WHAT IS SNAPSHOTTED AT THE SWITCH-IN: the
           frontier GENERATION (a fork or a finish added or removed a member, and an emission bumps it too), the
           running flow's SILENCE NOTCH (its own thread time since its last emission plus its fork family's,
           crossing a whole COOPERATIVE QUANTUM's worth — flow.c), its COMPLETED-UNIT COUNT (the optimism term's
           whole quantity, which moves at no clock's instants), and its own REWARD. Nothing else moves either
           side of the comparison: a parked flow
           burns no CPU, and its `val` cannot change without it running, which cannot happen while this one
           holds the thread. So the comparison is a pure function of those, and a yield with all of them
           unchanged means the WFQ answered two different things about one unchanged state.
           THE LIST IS RE-DERIVED EVERY TIME THE FORMULA MOVES, AND EACH REVISION IS WHY THIS ASSERTION IS WORTH
           HAVING. It was three clauses when the aging read the running flow's OWN service, four when the aging
           moved to the fork family (one arm's charge advances the family's notch on a schedule of its own, so
           the weight could move with the arm's own notch standing still), and it is four again now that the two
           service clauses have collapsed into ONE silence notch and the optimism term has become a count of
           completed units. Each of those edits would have made this fire on a perfectly legitimate rank change
           had the clause not moved with it, which is precisely the discipline it exists to enforce: the
           assertion is about a STATE, and a state is whatever the weight is currently a function of.
           AND ONE TERM IS NOT A WEIGHT AT ALL — IT IS THE ELIGIBLE SET, which is why counting weight
           terms could never have found it. This clause list is complete for `flow_weight(cur)`, which is a pure
           function of `val`, the completed-unit count and the silence notch; the comparison it appears in is
           `best-eligible-OTHER > cur`, and the SET that "eligible" ranges over is a term of that answer exactly
           as the three weights are. A host-owed mark decides membership of it, and neither laying one nor
           clearing one raised the frontier generation — so a clear during the running flow's first step handed
           the rescan above a rival the loop's pick had never been shown, and this fired with all four terms
           correctly unchanged. It is FIXED AT THE ORIGIN rather than named as a fifth clause here: a mark change
           now calls frontier_rank_changed() (flow.c), so it arrives through the generation, which is clause one.
           A fifth clause would have made this assertion pass while the HOOK went on ranking against a set the
           scheduler had not used — the assertion would have been silenced and the defect kept.
           IT FIRES ON THE TREE THIS FIXES, which is the whole reason it is worth writing. The optimism term
           quantised service with a CEILING, so the first MICROSECOND a flow was ever charged moved its notch
           from 0 to 1 and cost it half the entire bonus — the notch changed, so this assertion would have
           permitted it, and the flow was then strictly outranked by every never-run sibling at its next
           back-edge. What this catches is the version of that defect with no published change at all: a tie
           handed on by the pick's registry order, a rival recomputed against a stale cache, a weight term that
           moves with something this list does not name. Any of those is a swap of two COW deltas bought with
           nothing, and at 512 flows that was 1.28 million of them for one document. */
        DCHECK(flow_frontier_gen() != g_ranked_gen ||
               flow_silence_notch(cur) != g_ranked_silence ||
               cur->visits != g_ranked_visits || cur->val != g_ranked_val ||
               cur->cand_dist != g_ranked_dist,
               "the VALUE YIELD fired on a flow whose rank nothing changed since the scheduler switched it in — "
               "same frontier generation, same silence notch, same completed-unit count, same reward and same "
               "fitness distance on both "
               "sides of the comparison, so the pick and the hook are answering one unchanged state two "
               "different ways and every swap this buys is a COW delta swap for no ranking decision at all");
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
/* …AND THE PRECONDITION FOR RUNNING ONE, WHICH IS WHY `jobsRun: 0` NEEDED A SECOND NUMBER BESIDE IT. Every job
   arm of flow_step is under `frame == NULL` — HTML §8.1.4.4 "Calling scripts" step 3 of clean up after running
   script, "if the JavaScript execution context stack is now empty" — so a run with jobs queued and none run has
   TWO opposite readings and the pair above states neither: either flows are reaching that boundary and finding
   nothing to do, or NO FLOW EVER REACHES IT, which means no program in the document has finished and the
   reaction pump was never eligible to run at all. Those need opposite fixes and one zero was the evidence for
   both. Measured on four live sites at 90 s each: jobsQueued climbed past 29000 with jobsRun flat at 0, and
   nothing in the document could say which of the two it was.
   IT ANSWERED, AND THE ANSWER WAS THE FIRST ONE: excalidraw.com over three runs reported 3500-4067 units
   against 2657-2923 jobs queued and jobsRun 0, so the boundary was reached thousands of times per run by flows
   that held nothing to do there. The reading the pair made possible is that the flows REACHING the boundary
   and the flows HOLDING the jobs were disjoint sets — and they were disjoint because the credit itself was
   what separated them: it is a demotion (the optimism term is 1/(1+visits)) taken at the exact instant a
   flow's own queued reactions became eligible. flow_credit_visit now asserts the rest of §8.1.4.4's boundary
   at the origin so that cannot be reintroduced.
   IT IS THE SAME QUANTITY THE OPTIMISM TERM COUNTS — flow_credit_visit's "completed unit of work" — counted
   once for the whole instance rather than per flow, and incremented at that credit's own site so the two
   cannot come to mean different things. A boundary reached with a checkpoint still owed is deliberately NOT
   one of them: the turn is not over there, and the step that follows it is the one that runs the job, so it is
   `jobsRun` that reads it. */
static long g_units_done;
long engine_units_done(void) { return g_units_done; }

void engine_orphan_claims(long *met, long *unmet) {
    DCHECK(met != NULL && unmet != NULL,
           "the orphan round trip was asked for one of its two numbers — the pair is the statement, because a "
           "session that satisfied many waits and lost one has a nonzero count on both");
    *met = g_orphan_claims_met;
    *unmet = g_orphan_claims_unmet;
}

static int engine_enqueue_job(JSContext *ctx, JSJobFunc *fn, int argc, JSValueConst *argv, bool is_task,
                              JSTaskHandle handle) {
    /* THE OWNER IS NAMED WHEN THE USER AGENT IS THE QUEUER — see g_enqueue_owner, declared beside the one
       bracket that sets it. Everywhere else the callback belongs to the flow whose program queued it. */
    Flow *f = g_enqueue_owner;
    g_jobs_q++;
    if (!f)
        f = flow_running();
    else
        /* HOST TIME IS "NO SLICE OPEN", NEVER "NO FLOW RUNNING", AND THOSE ARE NOT THE SAME STATEMENT.
           This asserted `flow_running() == NULL` on the premise that a bracket set between two steps names an
           owner "precisely because there is none" — and engine_sched_step's contract says the opposite in its
           own words one screen down: the cooperative-quantum yield RETURNS WITHOUT SWITCHING THE RUNNING FLOW
           OUT, because §scheduler requires the same flow to resume byte-identically on the frontier it left.
           The stamp therefore stays up across the host's own time BY DESIGN, so the premise is false on the
           only return a busy engine ever makes, and an assert is false exactly when the engine is working.
           WHAT IT COST IS THE PRODUCT PATH IT STOOD ON. A navigation reported to a HOT engine aborted the
           instance and wedged the one scheduler for the rest of the session, so continuous browsing survived
           exactly ONE navigation per browser — the second one died, on unrelated origins alike, which is the
           shape of a false invariant rather than of a site.
           THE MISROUTING IT NAMED IS REAL AND IS ALREADY FORECLOSED ABOVE, which is why the answer is a truer
           assert and not a deleted one: a callback landing on the wrong timeline's queue is prevented by
           PRECEDENCE — a named owner wins outright, so the queue reached is the named flow's whatever
           `flow_running()` says. The three marks the host's time does have to have down (the flow stamp, the
           DOM capture, the capture route) are suspended and restored by engine_sched_step's wrapper and
           re-asserted at the ABI boundary by qjs_step; `g_running` is deliberately not one of them, because a
           yielded flow's COW delta is still APPLIED to the heap and clearing the stamp would be a claim about
           that heap which is not true.
           WHAT IS LEFT TO ASSERT IS THE PROPERTY THAT WOULD ACTUALLY BREAK THIS: not that no flow holds the
           thread, but that none is EXECUTING. A slice open here means the interpreter reached this line on the
           scheduler's own time while a host-time bracket named an owner — two queuers claiming one callback,
           which is the overlap the old reason was reaching for and the only form of it that can happen. It is
           the same predicate preempt_hook asks above and qjs_step asserts on every exit, so host time has ONE
           spelling in this file instead of two that disagree. */
        DCHECK(!quantum_slice_open(),
               "a host-time bracket named the owner of a queued callback while a scheduler SLICE was open — an "
               "owner is named only between two steps, so an open slice means a flow is EXECUTING and two "
               "queuers claim one callback: the named one wins and the running flow's own reaction is filed "
               "under a timeline that never caused it");
    /* THERE IS NO GLOBAL DRAIN. Declining here hands the job to quickjs's global list, and nothing in this
       engine ever runs that list — so the job is not "deferred to the default", it is DROPPED. Every task
       source goes through here: a window message, a port delivery, a broadcast, a timer callback, a custom
       element reaction. A dropped one is a handler the page registered and this engine never entered, which is
       invisible from the outside and looks exactly like a page that does nothing on message. */
    DCHECK(f != NULL, "a job was enqueued with no flow running — there is no global drain, so it would be "
                      "dropped: seed it as a flow on the frontier instead of declining it here");
    if (!f) return 0;
    DCHECK(ctx != NULL, "a job was enqueued with no realm — §7.5.10 step 7 removes a destroyed document's tasks "
                        "by comparing this, so a job without one outlives its document");
    /* THE NAME ARRIVES WITH THE JOB AND IS RECORDED, never defaulted: the host that takes ownership is the only
       thing that can find this callback again, so a record that dropped it is a task no tracker can remove and
       a `toggle` event that fires twice. Every runtime enqueue path allocates one (js_enqueue asserts it), so a
       job arriving here without one is the runtime's own contract broken and not a shape to tolerate. */
    DCHECK(handle != JS_TASK_HANDLE_NONE,
           "a job reached the scheduler under the never-issued handle — quickjs mints one at every enqueue and "
           "carries it across the baseline handover, so a job without a name has come from a path that does "
           "not, and nothing could ever take it back off this flow's queue");
    flow_job_push(ctx, f, fn, argc, argv, is_task, handle);
    return 1;   /* host owns it */
}

/* THE THIRD PART OF THAT OWNERSHIP (installed as JS_SetJobRemoveHook): HTML §4.11.4 "The dialog element"'s
   "Remove element's dialog toggle task tracker's task from its task queue" — §4.11.1 "The details element"
   and §6.12 "The popover attribute" hold the same tracker and the same sentence — for a job THIS scheduler
   took. quickjs's own two queues and its baseline list are walked by JS_RemoveQueuedTask before this is asked;
   what this scheduler holds is what nothing else can reach.
   IT IS THE RUNNING FLOW'S QUEUE AND CANNOT BE A GLOBAL SEARCH. A fork gives every arm its own Array naming
   the SAME records, so after a branch N flows hold N queued copies of ONE handle — N timelines' copies of one
   queued task, each of which the corresponding arm's own tracker names. Sweeping every flow would delete a
   sibling's task out of the sibling's timeline on the strength of a removal made in this one, which is exactly
   the shared-state bug the per-flow queues exist to make impossible. §7.5.10's drop is the running flow's for
   the SAME reason and not the opposite one — a destruction is a fact in the timeline that performed it, and
   the version of this sentence that said a destroyed document is destroyed in every timeline at once was
   wrong: engine_unload_document exists precisely because a destruction every timeline must perform has to be
   fanned out to each of them, one operation per flow.
   FINDING NOTHING IS ORDINARY — the task already ran, is running now (a `toggle` listener that closes the
   dialog again asks for the removal of the very task dispatching it), or went with its document. */
static int engine_remove_job(JSTaskHandle handle) {
    Flow *f = flow_running();

    DCHECK(f != NULL,
           "a queued task was removed by name with no flow running — a tracker's removal is made by state that "
           "only ever changes inside a flow, so a caller here is a platform edge reaching for a queue that "
           "belongs to a timeline it is not in");
    if (!f) return 0;   /* release path under the assert: nothing this hook can honestly reach */
    return flow_job_remove(f, handle);
}

/* THE OTHER HALF OF engine_enqueue_job (installed as JS_SetJobDropHook): HTML §7.5.10 "Destroying documents"
   step 7, for the jobs this scheduler TOOK. Nothing else can do it — declining to register this hook would
   leave every destroyed document's reactions queued on whichever flow enqueued them, and each one would later
   run in a Document whose browsing context is null.
   IT IS THE RUNNING FLOW'S QUEUE AND NO OTHER, for the reason JSJobRemoveHook's own contract states one line
   down in quickjs.h: a flow IS a timeline, a fork gives each arm its own copy of the parent's queued jobs, and
   a destruction is a fact in the timeline that performed it. §7.5.10 step 7 removes tasks from the event loop
   whose task it is running inside; a sibling flow is a different timeline in which this Document may not be
   destroyed at all — a `<iframe>` removed in one arm is still in the document in the other — so sweeping its
   queue deletes work from a world where the removal never happened. That is the shared-state write this
   engine's per-flow queues exist to make impossible, and it is not a small one: it is also what would make the
   STANDARD'S queue home unusable, since §7.5.9 step 6 puts every timeline's unload task on the OUTGOING
   document's own global and the first flow to reach its step 20 would take all the others with it.
   A DESTRUCTION EVERY TIMELINE MUST PERFORM IS FANNED OUT AT THE SEAM THAT REPORTS IT, never here:
   engine_unload_document gives each flow its own unload, and each one's step 7 then removes its own queue's
   tasks. That is one operation per timeline rather than one timeline's operation applied to all of them. */
static int engine_drop_jobs(JSContext *ctx) {
    Flow *f = flow_running();
    int dropped;

    DCHECK(f != NULL,
           "§7.5.10 step 7 removed a destroyed Document's queued tasks with no flow running — a destruction is "
           "performed by a timeline (§7.5.9's unload task is a job of one), so a caller here is a platform edge "
           "reaching for a queue that belongs to a timeline it is not in");
    if (!f) return 0;   /* release path under the assert: nothing this hook can honestly reach */
    /* NOT WHILE THIS WALK HOLDS THE FLOW. It runs inside a flow step, where the allocator's refusal edge is
       armed to page a member of the frontier out — and the registry removes by swapping its last member into
       the hole, so a reclaim inside this walk can move the Flow record it is holding an index into. A refusal
       across this walk is simply a refusal; the step's next allocation is armed again. */
    int prev_reclaim = engine_reclaim_set(0);
    dropped = flow_job_drop_realm(ctx, f, ctx);
    engine_reclaim_set(prev_reclaim);
    return dropped;
}

/* Run ONE of the flow's queued jobs under its currently-applied COW; free its args + result.
   THE PICK IS HTML 8.1.7's MICROTASK CHECKPOINT, not a FIFO pop. Within a queue the order is arrival order, but
   a TASK may not begin while this flow still holds a microtask — a plain FIFO ran `setTimeout(f, 0)` in the
   middle of a promise chain, which is the one ordering the event loop exists to forbid. */
static void flow_run_one_job(JSContext *ctx, Flow *f) {
    JSValue job = flow_job_take(ctx, f);   /* the pick IS the checkpoint rule — see flow.h */
    g_jobs_run++;
    JSValue r = flow_job_run(ctx, job);    /* the reaction runs in this flow's timeline */
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
    /* THE RECORD'S REFERENCE, NOT ITS CONTENTS. A fork gives every arm an Array naming the same records, so
       the arguments belong to the record and go when the LAST arm's reference does — an arm that freed them
       here would empty a job its sibling has not run yet. */
    JS_FreeValue(ctx, job);
}

/* IS A MICROTASK CHECKPOINT DUE BEFORE THIS FLOW'S NEXT PROGRAM?
   HTML §8.1.4.4 "Calling scripts", clean up after running script step 3: "If the JavaScript execution context
   stack is now empty, perform a microtask checkpoint." `Flow::frame` IS that stack here ("the current script's
   live preemptible frame, NULL between scripts", solver/flow.h) and the caller has already established that it
   is NULL, so what is left to decide is only WHICH boundaries are boundaries.
   §4.12.1.1 "Processing model" ends "prepare the script element" with "Otherwise, immediately execute the
   script element el, even if other scripts are already executing" — that program ran INSIDE the one that
   inserted it, so the stack never emptied across it and the checkpoint the inserting program owes falls AFTER
   its row. Every OTHER program the flow has left is a task: the document's next <script>, a lazy chunk, a
   `javascript:` URL, a §8.6 string handler, a peer's operation. §8.1.7.3 "Processing model" performs the
   checkpoint at the END of each task, so all of them come after it, and that is the whole of the answer.
   THIS USED TO BE ASKED ONLY AFTER THE SEQUENCE WAS EXHAUSTED, which is why it is a function now. The job arm
   sat below the arms that compile a program, so it was reachable only once the cursor had passed the last row
   — no microtask of any kind ran until every document script and every queued row had run, and
   `<script>Promise.resolve().then(() => fetch('/a'))</script><script>fetch('/b')</script>` produced /b before
   /a. No browser does that, and nothing in the engine could say so: the checkpoint END hook (engine_step)
   already read §8.1.4.4's precondition correctly while the checkpoint itself did not run there at all. */
static int flow_checkpoint_due(const Flow *f) {
    int row;

    if (!flow_job_microtask(f)) return 0;
    row = f->script_i;
    if (row < f->dyn_n) {
        DCHECK(f->dyn_pos != NULL,
               "a flow holds queued programs with no position column — the row the cursor names cannot say "
               "whether it is a task or the synchronous tail of the program that queued it, and the microtask "
               "checkpoint is placed against exactly that");
        if (f->dyn_pos[row] == DYN_POS_IMMEDIATE) return 0;
    }
    return 1;
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

/* NO COMPLETION CROSSES A SCHEDULER BOUNDARY — ONE STATEMENT, ASKED AT EVERY BOUNDARY THERE IS.
 *
 * `rt->current_exception` is per-RUNTIME while a completion is per-EVALUATION (ECMA-262 §6.2.4 The Completion
 * Record Specification Type; §5.2.4.3 Shorthands for Unwrapping Completion Records — "?" propagates an abrupt
 * completion TO THE CALLER, never to whatever runs next). Under an interleaving host those are different
 * timelines, so a throw left standing anywhere the scheduler passes is delivered to a flow that did not produce
 * it. The engine refuses to START a flow over one (JS_FlowResume's entry DCHECK) — but that is the VICTIM's
 * end: by the time it fires, whatever produced the completion has returned and nothing records what it was.
 *
 * SO THE BOUNDARIES ARE ASKED, AND THEY PARTITION THE WHOLE OF THE SCHEDULER'S OWN TIME. There are exactly
 * four, and between them there is no unwatched region left:
 *   slice-entry  — the HOST's own time between two slices (a fetch reply parsed into a record, a delivery
 *                  routed, a synchronous answer written) plus, on the first slice, everything the session
 *                  setup did before any flow existed;
 *   pre-step     — the scheduler's own pick: solve_seed_candidates, flow_next_to_run, the context switch
 *                  (flow_switch_out/in, the COW and DOM delta swap, decide/pins resume) and solve_flow_begin;
 *   post-step    — the arm flow_step just ran (this one has fired for nobody, which is what established that
 *                  every arm in flow_step takes its own completion);
 *   slice-exit   — the scheduler's tail after the step: HTML §8.1.7.3's end-of-checkpoint hook, the aging
 *                  charge, and flow_finish.
 * Whichever one fires NAMES the segment, and the four together are a checked contract rather than a probe:
 * they say, permanently, that the scheduler's own time never carries a page's completion.
 *
 * The throw is TAKEN to describe it, which is sound because DFAIL does not return — the state after that call
 * is an abort, so nothing observes the emptied slot. Compiled out entirely in release. */
#if APICLIENT_DEV
static void engine_no_stray_completion(JSContext *ctx, const char *where, int detail)
{
    JSValue le;
    char et[320];

    if (ctx == NULL || !JS_HasException(ctx))
        return;
    le = JS_GetException(ctx);
    result_error_text(ctx, le, et, sizeof et);
    /* THE PARK STATE IS PRINTED BESIDE THE UNIT because it separates two readings of one segment that are
       otherwise identical from outside. `JS_CallAsFlow` answers 0 for a settle that PARKED as well as for one
       that COMPLETED — its own contract is "0 = done, -1 = it threw" and has no third answer — so a delivery
       that ran page code which suspended looks exactly like one that ran it to the end. A park standing here
       says the work is suspended and the completion was never this segment's to take; no park says the segment
       finished and dropped it. */
    JS_FreeValue(ctx, le);
    DFAILF("a completion was still live in the runtime at a SCHEDULER BOUNDARY — at `%s`, after the `%s` "
           "step unit. The exception slot is per-RUNTIME and a completion is per-EVALUATION (§6.2.4), so the "
           "next flow resumed would receive a throw §5.2.4.3 says belongs to whoever asked for the "
           "evaluation that produced it. Take it where it is produced: a program's through JS_FlowResume's "
           "`pres`, a job's at the job, a parked continuation's through JS_ResumeParkedFlow's `pres`, a "
           "delivery's at JS_CallAsFlow. parked=%d detail=%d. The throw was: %s",
           where, g_step_unit, JS_HasParkedFlow(JS_GetRuntime(ctx)) ? 1 : 0, detail,
           *et ? et : "(a throw this engine could not describe)");
}
#define ENGINE_NO_STRAY(ctx, where, detail) engine_no_stray_completion((ctx), (where), (detail))
#else
#define ENGINE_NO_STRAY(ctx, where, detail) ((void)0)
#endif

/* DECLARED ABOVE THE ONE FUNCTION THAT WRITES THEM, because flow_step is where a program is compiled and
   where a flow is finished — the two events these count. The commentary on what each MEANS stays with the
   accessors that publish them; this is the definition, and it has to precede its first use. */
static long g_finished;
static int  g_deepest = -1;
/* AND THE OTHER END OF THE SAME PROGRAM — the highest index this document has ever run to COMPLETION. It is a
   separate fact from `g_deepest` and the difference between them is the whole diagnosis, which is exactly why
   one number could not carry both: `deepest 1` says a flow STARTED the second <script>, and it was read — in
   two independent analyses of this engine, and in the sentence that commissioned this one — as "no flow has
   ever begun the second <script>". Neither reading is checkable against the other from one number. With both,
   `deepest 1, completed 0` states it exactly: some flow finished program 0, no flow has ever finished program
   1, and therefore nothing this document loads AFTER program 1 — a lazy chunk, an injected <script>, a fired
   PoC, the whole surface §What-the-tool-produces calls the headline moat — has ever been compiled by anything.
   `deepest` alone cannot say that, because a flow that starts program i proves only that SOME flow completed
   i-1, and says nothing about how many programs the document has left to run. */
static int  g_completed = -1;

/* HTML §8.1.4.4 "Calling scripts", "run a module script" step 8: "If preventErrorReporting is false, then upon rejection of
 * evaluationPromise with reason, report an exception given by reason for script's settings object's global
 * object." A module completes as a PROMISE — that is the whole difference between running a module script and
 * running a classic one, and it is why a top-level `await` is observable — so its failure is not a completion
 * the compile site can read the way it reads a classic script's throw. The reaction below is that step, and it
 * ends in the same place a thrown script does: a page error naming the capability the page needed.
 *
 * IT IS ATTACHED WITH PerformPromiseThen AND NOT WITH `.then`, which is what the spec's "upon rejection of"
 * means and matters twice over. Reading `then` off the promise runs whatever the page put on
 * Promise.prototype, and it constructs through @@species; and the attach MARKS THE PROMISE HANDLED, which is
 * observable — a page's `unhandledrejection` handler must NOT see a module that threw, because HTML has
 * already claimed that rejection for the error report. Dropping the promise instead would have delivered every
 * module's top-level throw as an unhandled rejection, which is a different event with a different name. */
static JSValue module_eval_rejected(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    DCHECK(argc == 1, "the rejection reaction of a module script's evaluation promise ran without a reason — "
                      "a promise reaction job calls its handler with exactly the settled value, so an argc "
                      "other than 1 means this function was reached from something that is not one");
    result_page_error_value(ctx, argv[0]);
    return JS_UNDEFINED;
}

/* Attach that reaction. `ctx` is the PROGRAM's realm — the module's settings object's global is the document
   the module belongs to, and a reaction function is a runtime-lifetime object that carries the realm it was
   minted in, so it is minted here per evaluation rather than held in a static that would answer every
   document's rejection with the first document's ctx. */
static void module_report_rejection(JSContext *ctx, JSValueConst eval_promise) {
    JSValue on_rejected = JS_NewCFunction(ctx, module_eval_rejected, "reportModuleScriptError", 1);
    JSValue derived;
    CHECK(!JS_IsException(on_rejected),
          "the reaction that reports a module script's evaluation error could not be allocated — without it a "
          "module that threw is silently delivered as an unhandled rejection instead of a page error");
    derived = JS_PerformPromiseThen(ctx, eval_promise, JS_UNDEFINED, on_rejected);
    CHECK(!JS_IsException(derived),
          "HTML §8.1.4.4 \"Calling scripts\" step 8's rejection reaction could not be attached to a module "
          "promise — the module's own failure would then have no reader at all");
    /* The derived promise has no reader BY CONSTRUCTION: the handler returns undefined, so it fulfils, and a
       fulfilled promise nobody reads is not an event. It is freed rather than kept because §8.1.4.4 returns
       evaluationPromise to its caller and this one is only the capability PerformPromiseThen must produce. */
    JS_FreeValue(ctx, derived);
    JS_FreeValue(ctx, on_rejected);
}

/* THE PROGRAMS ARE THE FLOW'S OWN, so this takes no inventory. It used to be handed the session document's
   `bodies` and its length and to walk them ahead of the flow's rows; both halves are rows of `f->dyn` now. */
static int flow_step(JSContext *ctx, Flow *f) {
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
        /* ONE ARM PER DISTINCT ANSWER, BEFORE ANYTHING ELSE THIS FLOW COULD DO — because everything else it
           could do consumes the very thing the arm is made of. A peer document's state IS its flows, so a
           cross-instance read has an answer per peer timeline; the arm is a clone of the frame this flow is
           SUSPENDED in, and the first resume below takes the answer off the register and leaves that call site
           behind. It is also the first moment in the whole round at which a fork is even meaningful: the flow
           is switched in, so its delta, its DOM head and its decision state are the ones an arm inherits. */
        g_step_unit = "fork-over-a-peer-answer";
        if (flow_answer_fork(ctx, f)) return 0;
        /* THE PARKED CONTINUATION OUTRANKS EVERYTHING ELSE THIS FLOW COULD DO — that is the park's whole
           contract: a forced preempt must be transparent to observable ordering, so the flow resumes BEFORE any
           job it has queued. Yielding after one resume keeps the scheduler in charge of fairness; the park (if
           it parks again immediately) rides the switch-out with the flow. Without this the solver host never
           pumped the slot at all: the continuation sat there until a second flow parked and asserted. */
        g_step_unit = "resume-parked-continuation";
        /* Resuming a parked continuation is progress ONLY if the flow can still get somewhere: a flow blocked
           on the host resumes into the same wait, so reporting progress spins it against the park. */
        /* AND ITS COMPLETION IS TAKEN HERE, which is the ONE place it can be. A parked continuation is the LATE
           half of a job this flow queued: flow_run_one_job reports the throw of a job that finished inside its
           own call, and the half that PARKED — which is every job long enough to be preempted, and since a step
           machine offers a park at every re-entry that is most of them — had no reporting path at all. So an
           uncaught throw out of one was dropped twice over: never recorded as this page's error, and never
           taken out of `rt->current_exception`, which is per-RUNTIME. This engine interleaves flows, so what
           found it next was the first evaluation of whichever flow the scheduler chose — another flow's
           timeline receiving a completion §6.2.4 says belongs to this one. §8.1.7.5's report hook is the same
           one a script and a job already use, because it is the same event. */
        {
            JSValue pcv = JS_UNDEFINED;
            if (JS_ResumeParkedFlow(JS_GetRuntime(ctx), &pcv)) {
                if (JS_IsException(pcv)) {
                    JSValue e = JS_GetException(ctx);
                    result_page_error_value(ctx, e);
                    JS_FreeValue(ctx, e);
                }
                JS_FreeValue(ctx, pcv);
                return flow_blocked(f) ? FLOW_STEP_OWED : 0;
            }
            JS_FreeValue(ctx, pcv);
        }
        if (!f->frame) {
            const char *body;
            /* AND ITS LENGTH, WHICH THE BODY ALREADY KNOWS. Both compiles below took `strlen(body)`, which is
               a pass over every byte of the program each time one starts — on a real single-page app's module
               bundle that is 2.1 MB walked to learn a number the row was already holding (solver/dyn_body.h). */
            size_t body_n = 0;
            /* WHICH KIND the program about to be compiled is — only a page <script> must parse. */
            DynKind kind = DYN_PAGE_SCRIPT;
            /* AND WHICH OF §8.1.4.4 "Calling scripts"'S TWO ALGORITHMS RUNS IT — run a classic script, or run
               a module script. CLASSIC is the answer for every entry a FLOW added, and that is a statement
               about those entries rather than a default waiting to be overridden: §7.4.2.3.2's `javascript:`
               URL evaluates a classic script, a lazy chunk and a `setTimeout` string are classic scripts, and a
               cross-agent operation's program is this engine's own classic text.
               IT IS THE ROW THAT ANSWERS, NOT THE HALF OF THE SEQUENCE THE CURSOR IS IN. This used to read the
               session document's types column in one branch and stay CLASSIC everywhere else, which was
               correct only while the DOCUMENTS THAT ARE NOT THE SESSION'S had no way to state a type at all —
               and the way they said so was to abort: three DCHECKs, one per seam, on `<script type=module>`.
               A child navigable's Document, a joined one and an INJECTED element each put their element's type
               on the row now (flow.h's `dyn_type`), so this is one question with one answer for every position
               in the sequence. */
            ScriptType stype = flow_dyn_type(f);
            /* WHICH OF THE ORPHAN STEP'S THREE OUTCOMES HAPPENED — held so the branch can name its own work
               rather than labelling three unrelated things with the name of one of them. */
            int orphan_step;
            /* THE ROUTED DELIVERIES THIS FLOW HAS BEEN HANDED, and they are first because the task each one
               enqueues is what every branch below then finds on the queue. ONE PER STEP, oldest first: each
               becomes a §9.3.3 task at the receiving Window and the step returns, so the tasks are enqueued in
               the order the messages were posted and the scheduler re-ranks between them. The queue empties as
               they are taken, so a flow with none left falls through to its jobs. */
            if (flow_deliver_pending(f)) { g_step_unit = "routed-delivery"; flow_deliver(ctx, f); return 0; }
            /* AND THE OPERATION A PEER IS PARKED ON, before this flow's own programs for the reason the
               delivery is: it is a work item another agent's flow is suspended at, and it becomes one of THIS
               flow's programs — after which the loop below runs it like any other. */
            if (flow_perform_pending(f)) { g_step_unit = "cross-agent-operation"; flow_perform(ctx, f); return 0; }
            /* THE MICROTASK CHECKPOINT, BEFORE THE NEXT PROGRAM AND NOT AFTER THE LAST ONE — HTML §8.1.4.4
               "Calling scripts" step 3 of clean up after running script, decided by flow_checkpoint_due above.
               It is the FIRST thing the flow does with an empty stack because that is what the sentence says:
               the checkpoint belongs to the program that just finished, so nothing the sequence still holds
               may run in front of it. Everything below this line is a task.
               A JOB CAN PARK, and until this was reported the park was invisible to the scheduler. A queued
               step machine that suspends on a synchronous host request is parked by reaction_flow_step
               and this returned PROGRESS — so the flow was resumed, parked again, resumed again,
               forever, and never reported host-owed. The host was therefore never asked, and the answer that
               would have let it finish could not arrive: a livelock that looks exactly like slowness, because
               every turn is "progress". It is the same rule the mid-frame yield already keeps: a blocked flow
               has no work, whatever it just did. OWED is the register the scheduler already has for
               waiting-not-finished. */
            if (flow_checkpoint_due(f)) {
                g_step_unit = "microtask-checkpoint";
                flow_run_one_job(ctx, f);
                return flow_blocked(f) ? FLOW_STEP_OWED : 0;
            }
            /* THE FLOW'S SEQUENCE, AND THERE IS ONLY ONE OF THEM. Two arms stood here: the SESSION document's
               static scripts, read out of a borrowed `bodies` array at `script_i`, and then this flow's own
               rows at `script_i - n`. They differed in nothing a program cares about — both are §4.12.1
               positions of some document of this agent, both can be inline or external, both carry a type and
               an address — and the split cost this engine the one position §4.12.1.1's "immediately execute the
               script element" needs (engine_queue_into). One arm now, over one table. */
            if (f->script_i < f->dyn_n) {
                body = dyn_body_text(f->dyn[f->script_i]);
                body_n = dyn_body_len(f->dyn[f->script_i]);
                kind = flow_dyn_kind(f);
                /* THE SEQUENCE HOLDS ONLY EXECUTABLE SCRIPTS — engine_queue_into asserts it at the one site
                   that creates a row, and this is the read that would otherwise have to pick an algorithm for a
                   program that has none. */
                DCHECK(script_type_executes(stype),
                       "a row of this flow's script sequence carries a type that does not execute — every row "
                       "is CLASSIC or MODULE, so a third answer means the type column was never written");
                if (kind == DYN_SCRIPT_SRC) {
                    /* AN EXTERNAL SCRIPT OF SOME DOCUMENT OF THIS AGENT, AT ITS POSITION. The entry holds its
                       ADDRESS — §4.12.1.1's encoding-parsed url, never the raw markup attribute, because the
                       host is a different zone with a different base and would resolve a relative `src` against
                       its own — so the flow WAITS here: §4.12.1 fixes this script's position against the scripts
                       written around it, and running what comes after a bundle before the bundle is a different
                       program. The reply REPLACES this entry and the next pass compiles it.
                       A reply that has ALREADY arrived is delivered first — parking without checking leaves the
                       flow owed forever on a URL the host has answered. ONE of them, like every other unit of
                       work in this function: if several are answered the flow comes back here for the next,
                       and the row this arm is waiting on is filled by whichever delivery names it. */
                    if (flow_pending_ready(f)) {
                        g_step_unit = "deliver-one-reply";
                        flow_deliver_one_reply(ctx, f);
                        return 0;
                    }
                    engine_pending_docscript(ctx, body, f->script_i);
                    return FLOW_STEP_OWED;
                }
                /* AND A ROW THAT REACHES THE COMPILE HOLDS A PROGRAM, not an address. The branch above is what
                   makes that true and this is what says so, because the two are one row apart and a body that
                   is still a URL would be compiled as one. */
                DCHECK(body != NULL,
                       "a row of this flow's sequence reached the compile with no body — a row is created with "
                       "one (engine_queue_into asserts it) and only a DYN_SCRIPT_SRC row is ever without a "
                       "program, which the branch above holds the flow at until its reply arrives");
            }
            else if (flow_job_pending(f) > 0) {
                /* WHAT IS LEFT ON THE QUEUE HERE IS A TASK — HTML §8.1.7.3 "Processing model" step 1, run the
                   oldest runnable task. The checkpoint above is the only thing that consumes a microtask and it
                   runs before every program, so a microtask on the queue at this point is one it declined to
                   run, and the only reason it declines is the immediate row that the arm above this one would
                   have compiled. There is no such row here (the cursor is past the end of the sequence), which
                   is why this is an assertion and not a second pick rule. */
                DCHECK(!flow_job_microtask(f),
                       "a task was about to begin while this flow still held a microtask — the checkpoint runs "
                       "before every program in the sequence, so reaching the end of the sequence with one "
                       "outstanding means a program ran in front of the checkpoint it owed");
                g_step_unit = "run-a-task";
                flow_run_one_job(ctx, f);
                return flow_blocked(f) ? FLOW_STEP_OWED : 0;
            }
            else if (flow_pending_ready(f)) {
                /* FETCH-AWAIT: this flow's programs are done and its microtasks are run, and a suspended async
                   body is awaiting a LIVE fetch (a pending promise). The network has completed, so ONE answered
                   entry is delivered — the awaiting body's reaction is enqueued as a job in this flow's queue
                   (we are switched in, flow_running == f) — and the step ends there. The checkpoint arm above
                   runs that reaction on the next pass and this arm delivers the next reply after it, which is
                   §8.1.7.3 "Processing model"'s task-then-checkpoint and not a pass that settles them all. */
                g_step_unit = "deliver-one-reply";
                flow_deliver_one_reply(ctx, f);
                return 0;
            }
            /* NOTHING QUEUED AND NOTHING DELIVERABLE. What follows is what becomes due when the flow has
               nothing else, in the order it becomes due: first the load lifecycle, which is already due (a
               parser finishing waits on no clock), and only then the two CLOCK-DRIVEN sources — and by the
               time control reaches here everything that was already due (this flow's jobs above, a reply that
               has arrived) has been offered a turn.
             *
             * AND THE LINE THROUGH THE MIDDLE OF THIS LADDER IS "DUE NOW" VERSUS "FINISHED", WHICH IS WHERE THE
             * HOST-OWED RETURN BELONGS AND NOT ONE RUNG HIGHER. Everything from here down to that return is
             * work HTML §8.1.7.3 "Processing model" would run on this turn whatever else is in flight — its
             * step 2 asks only "If the event loop has a task queue with at least one runnable task", and a
             * `fetch()` is not on a task queue at all until it completes and queues its own task. Everything
             * BELOW that return is the opposite kind of claim: that no handler will ever be attached to a
             * rejection, that nothing ever called a function. An outstanding reply is exactly what can still
             * falsify those two and cannot falsify anything above them, so it gates them and nothing else. */
            /* A FLOW SUSPENDED MID-EXPRESSION RUNS NOTHING ELSE OF ITS OWN, AND THAT IS ASKED ONCE, HERE, FOR
               EVERYTHING BELOW IT. flow_blocked is an unanswered synchronous cross-instance read (a HOSTREQ):
               the flow is parked AT that operand, so a `load` listener, a rendering opportunity or a timer
               callback run now would interleave two program points of one flow — which is not a scheduling
               preference but the same thing as resuming a generator into the middle of its own frame.
               IT USED TO BE TRUE BY ACCIDENT BELOW THIS ARM, and an accident is not an invariant. Only the
               lifecycle arm asked it; the clock-driven arms were protected because the host-owed return sat in
               front of them and a blocked flow always has an unanswered entry, so the two conditions coincided
               without either one meaning the other. Moving that return down to where it belongs would have
               taken the protection with it silently — which is the shape of every route that goes missing —
               so the question is stated positively at the one place that owes the answer, and an arm added
               below inherits it instead of having to remember it.
               IT RETURNS OWED because only the host can change it: the peer's answer arrives through
               engine_host_answer and nothing this flow does can produce it. */
            else if (flow_blocked(f))
                return FLOW_STEP_OWED;
            /* A DOCUMENT FINISHED LOADING, in this flow's world — DOMContentLoaded across the agent's
               documents in tree order, then `load` innermost-first, one per turn. It comes BEFORE the two
               clock-driven sources and that is the spec's order rather than a preference: the parser
               completing is not a timer and not a frame, it is already due, and everything that is due runs
               before the clock may move. It used to sit AFTER them, so a `setTimeout(f, 0)` a parse-time
               script set ran before DOMContentLoaded — which no browser does — and a rendering opportunity
               would have preceded it too. A page's real work is behind these events: the half of a bundle
               that touches the DOM and calls the API runs here.
             *
             * AND IT NOW COMES BEFORE THE FLOW PARKS ON WHAT THE HOST STILL OWES, which is the half that was
             * wrong. The OWED return used to sit ABOVE this arm, so the lifecycle was reachable only by a flow
             * with NOTHING outstanding — quiescence standing in for HTML §13.2.7 "The end" step 8, "spin the
             * event loop until there is nothing that DELAYS THE LOAD EVENT in the Document". Those are not the
             * same condition and the difference is not academic: nothing in HTML makes a `fetch()` or a
             * dynamic `import()` delay the load event, so a bundle with ONE request still in the air when its
             * parse ended never reached `load` — nor even DOMContentLoaded, since the same return gated stage 0
             * too — and every listener behind those events never ran. That is the majority of a real
             * application page (an SPA bootstrap, a router, a lazy-loader, an analytics beacon all hang off
             * `load`), and it is measurable as a page that parses, learns some endpoints and then finishes
             * having run none of the code that was waiting to be told the document was ready.
             * WHAT STILL HOLDS THE FLOW HERE IS STATED POSITIVELY, per kind, from the spec:
             *   - A `<script src>` DOES delay it, both the document's own (DOCSCRIPT, which the sequence arm
             *     far above already holds the flow at) and one a script INJECTED (SCRIPT) — §4.12.1.1
             *     "Processing model": "Whenever a script element el's delaying the load event is true, the
             *     user agent must delay the load event of el's preparation-time document."
             *   - A HOSTREQ is not a delay source at all and is a stronger thing — the flow is SUSPENDED
             *     mid-expression — so it is not this condition's business: the arm directly above answers it
             *     for this arm and for every arm below, which is why flow_blocked no longer appears here.
             *   - A `fetch()` (RESOLVE) and a dynamic `import()` (MODULE) delay nothing, and are exactly what
             *     this arm now runs in front of.
             * The remaining sources of step 8 are per-ELEMENT flags — `img`, the media elements, `script` — and
             * they belong to those components rather than to this register; core/dom/document.c's
             * document_load_event_delayed names them at the gate they are missing from. */
            else if (!pending_outstanding_kind(f->pending, FLOW_PENDING_SCRIPT) &&
                     !pending_outstanding_kind(f->pending, FLOW_PENDING_DOCSCRIPT) &&
                     g_docdone_hook && g_docdone_hook(ctx)) {
                g_step_unit = "document-lifecycle-stage"; return 0; }
            /* §8.1.7.3's IN-PARALLEL HALF, asked first of the two clock-driven sources because it is the one
               that can defer: it compares the next rendering opportunity with the earliest timer expiry and
               yields when the timer is earlier. Without a rendering opportunity there is no
               requestAnimationFrame, no ResizeObserver delivery, no IntersectionObserver task, no
               scroll/resize/pagereveal and no Web Animations microtask checkpoint — a large fraction of a real
               page's code hangs off exactly those. */
            else if (g_rendering_hook && g_rendering_hook(ctx)) {
                g_step_unit = "queue-rendering-opportunity"; return 0; }
            /* AND THE TIMER TASK SOURCE — §8.7 "Timers"'s timer initialization steps end by "queues a global
               task on the timer task source given global to run task", so a due timer IS a runnable task and
               §8.1.7.3 step 2 runs it. */
            else if (g_timer_hook && g_timer_hook(ctx)) { g_step_unit = "fire-due-timer"; return 0; }
            /* ONLY HOST-OWED REPLIES REMAIN: no progress, and NOT finished.
             *
             * BELOW THE TWO CLOCK-DRIVEN SOURCES, which is the same sentence the lifecycle arm makes one rung
             * up, said about the clock instead of about `load`: a debt no task source waits on must not decide
             * when a task source runs. §8.1.7.3 step 2 conditions the whole loop on "a task queue with at
             * least one runnable task" and on nothing else; a `fetch()` runs in parallel and is not on a queue
             * until it completes. Held ABOVE them, ONE request in the air took a flow out of the pick — a
             * marked flow is not picked again until a HOST EVENT clears it — and with it went every due timer,
             * every rendering opportunity, and therefore every requestAnimationFrame and observer delivery,
             * for the rest of the session.
             * THE SHAPE IT PRODUCED IS THE ONE THAT NAMES IT. testharness.js sets `all_loaded` inside a
             * `setTimeout(…, 0)` queued from its own `load` handler, so a document could run every one of its
             * subtests, pass every one, fire `load`, run the handler — and then never get the zero-delay timer
             * that is the last thing between it and `Tests.all_done()`. Every subtest reported, `num_pending`
             * 0, completion never. A flow that keeps its work and is never picked again is STARVED, and
             * §scheduler's razor makes no distinction there: "drops, starves, skips, reorders, or forgets ANY
             * flow — it is a CAP, banned".
             * AND IT IS STILL ABOVE THE TWO ARMS BELOW, for the reason the preamble gives: those two claim the
             * run is FINISHED (no handler will ever be attached; nothing ever called this function) and an
             * outstanding reply's continuation can still attach the handler and still make the call. */
            else if (pending_count(f->pending) > 0)
                return FLOW_STEP_OWED;
            /* HTML §8.1.4.7 "Unhandled promise rejections" — its "notify about rejected promises". The flow
               has nothing left to run, so every rejection still on its list is one no handler will ever be
               attached to. The browser half keeps
               the lists and fires `unhandledrejection`; those fires are JOBS, so the flow has work again and
               the loop picks them up like any other. Only what the page did not cancel comes back through the
               report hook — and what it means then is this half's answer, the same thing a script that threw
               means: a capability the page needed. Notifying clears the list, so the next pass finds none. */
            else if ((g_step_unit = "unhandled-rejection-notify", unhandled_rejection_notify(ctx))) return 0;
            /* AND THE CODE THE PAGE SHIPPED AND NEVER RAN. Everything above this line is work the page ARRANGED
               — a program, a job, a lifecycle event, a timer, a frame — and it is all done, which makes this
               the first instant at which "nothing called this function" is a fact about this timeline rather
               than a guess about a run that has not finished. Each such function becomes one ordinary flow of
               this frontier (engine_orphan_fork); the seeding is progress like any other step, so the
               scheduler re-ranks before any of them runs, and this flow asks again next time and finishes when
               there is nothing left to take.
               ONE PER STEP, WHICH IS WHAT MAKES THE SENTENCE ABOVE TRUE. It seeded the whole heap's worth in
               one call, so "the scheduler re-ranks before any of them runs" described a burst of thousands of
               flows minted inside a single step with no suspend point on it — the stretch engine_sched_step's
               seam assertion aborts on, and did. The unit's name said `drive-orphans`, which is why that abort
               reads as an orphan DRIVER: there has never been one, and this is a seed. */
            else if ((orphan_step = engine_orphan_fork(ctx, f)) != 0) {
                g_step_unit = engine_orphan_unit(orphan_step); return 0; }
            else {
                /* A FLOW MAY NOT FINISH HOLDING WORK. Every branch above claims to have offered its queue a
                   turn, so reaching here with a job still on it means one of them returned first and the job
                   is about to be dropped with the flow — silently, because a dropped reaction looks exactly
                   like a page that registered no handler. Asserted at the one place "finished" is decided. */
                DCHECK(flow_job_pending(f) == 0, "a flow finished holding queued jobs — a promise reaction, a timer "
                                     "callback or a delivered message would be dropped with it");
                /* …AND A FLOW OF A REFERENCED DOCUMENT MAY NOT FINISH AT ALL — engine.h's engine_set_referenced.
                   Having nothing left to run is not being done when a peer can still ask this timeline
                   something; it is waiting on the host for the next operation, which is exactly what OWED
                   means. The flow keeps its snapshot, its delta and its rank and is out of the pick until the
                   host has something for it. */
                if (g_referenced) return FLOW_STEP_OWED;
                /* …AND A RESUMED DRIVE THAT NEVER GOT ITS BODY BACK SAYS SO ON ITS WAY OUT. The line above it
                   has just established that this document holds no untaken orphan, so the body this flow's
                   recipe named is not in this session's heap: the bundle moved under the residue, or the
                   locator does not name what it was written for. That is a legitimate outcome and NOT a
                   should-never-happen — §Time-travel has a resumed flow re-deriving from CURRENT sources, and
                   the code itself is one — so it is COUNTED rather than asserted, at the one place it can be
                   distinguished from a drive that ran. Without the count a residue whose every drive missed
                   looks exactly like one whose every drive landed. */
                if (f->orphan_want && JS_IsUndefined(f->fn)) g_orphan_claims_unmet++;
                return 1;   /* all scripts + chunks + microtask jobs + live fetches + load listeners done */
            }
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
            /* IN THE REALM OF THE DOCUMENT THE PROGRAM BELONGS TO — asked of the program, never of the
               session. A program compiled here is closed over the compiling realm's global (JS_FlowNew), so
               the realm is not a detail of where it happens to run: `globalThis[member]` compiled in the root
               reads the ROOT Window's member, a child document's classic script defines the child's globals on
               its creator, and a peer's operation answers about the wrong document. Every one of those is one
               question — which document is this a program OF — and it is answered by the queue entry rather
               than by a `? :` per kind: the kinds that used to need one are exactly the kinds whose document is
               not the session's, and there is no upper limit on those (a same-origin agent cluster holds a
               realm per document). */
            JSContext *prog_ctx = doc_realm(flow_dyn_doc(f));
            /* AN INLINE PROGRAM'S NAME IS ITS DOCUMENT'S ADDRESS, which is HTML §4.12.1.1's "let base URL be el's
               node document's document base URL" for a script with no `src`, and is what a relative
               `import('./chunk.js')` inside it resolves against — the moat's whole lazy-chunk surface. This used
               to be NULL under a comment saying the document URL was something "this host does not model yet",
               and that claim had stopped being true: document_base_url is in a header this file already
               includes, and it is the ONE component that owns what a document's address is. A name that is not
               the document's is also not a name two documents can differ by, and the compile above is explicitly
               per-document. */
            const char *prog_name = document_base_url(prog_ctx);
            /* …AND AN EXTERNAL ONE'S NAME IS ITS OWN ADDRESS, which is a different question with a different
               answer and was being given this one. §4.12.1.1 hands its resolved `url` to §8.1.4.2 "Fetching
               scripts" — "Fetch a classic script given url, …" and "Fetch an external module script graph given
               url, …" — and what that section creates is a script based on the address the bytes came FROM:
               "let script be the result of creating a classic script given sourceText, settingsObject,
               RESPONSE'S URL, options, mutedErrors, and url". Never the document's.
               It decides answers in both of §8.1.4.4's entries: a nested `import('./chunk.js')` inside a bundle
               served from `/assets/app.js` is `/assets/chunk.js` and not `/chunk.js`, and for a MODULE the name
               is additionally the module map KEY — so two `<script type=module src>` in one document, named by
               the address they share, are ONE module, and the second one's graph finds the first one's record
               and evaluates nothing. A modern app with several module entry points loses all but one of them.
               AND IT IS ASKED OF THE ROW, for the reason the type above is: the session document was not the
               only document of this agent with external scripts, it was only the one whose external scripts had
               anywhere to keep their address. A child navigable's and a joined document's rows carried theirs
               in the BODY column and lost it the moment the reply overwrote it, so every one of those bundles
               compiled under its document's name — one module map key shared by the lot.
               THE COMPANION ASSERT MOVED WITH THE SEQUENCE. It used to ask "was this row external?" of a
               parallel array that only the session document's half of the old sequence had; the row itself is
               what answers now, and it is asserted where the answer is WRITTEN —
               flow_deliver_one_reply moves the address out of the body column at the one moment it can, and
               asserts the column was empty before it did. */
            int src_flags = 0;
            {
                const char *ext = flow_dyn_url(f);
                if (ext) prog_name = ext;
                /* …AND WHICH HALF OF THE DOCUMENT THIS PROGRAM IS. HTML §4.12.1 "The script element" splits a
                   document's programs by one attribute — `src` "denotes that instead of using the element's
                   child text content as the script content, the script will be fetched from the specified
                   URL" — so a page-script row with no ADDRESS is an INLINE script and its source text came in
                   the document's own response, rendered against this visitor's credentials. An external row's
                   bytes are a subresource served identically to everybody, which is the whole reason its code
                   contains the logged-in surface a logged-out visit never runs.
                   THE KIND IS ASKED TOO, because an address is not the only thing a row can lack: a candidate
                   body, a `javascript:` URL and a driven orphan are all address-free and none of them is a
                   `<script>` the server rendered. The solver reads this at the one place it decides whether a
                   missing member of a present record is unknown INPUT or `undefined` — see
                   solver/absent.h. */
                if (!ext && flow_dyn_kind(f) == DYN_PAGE_SCRIPT)
                    src_flags = JS_EVAL_FLAG_INLINE_SCRIPT;
            }
            /* A MODULE IS A DIFFERENT ALGORITHM, NOT A FLAG — §8.1.4.4 has two entries and this is where they
               part. A CLASSIC script is a program: JS_FlowNew wraps it in a preemptible frame the scheduler
               resumes, and it completes with a VALUE. A MODULE is a graph: it is linked and evaluated, its body
               is an async function on the flow machinery's own seam (so a top-level `await` and a loop
               back-edge PARK it into the slot JS_ResumeParkedFlow drains at the top of this loop), and Evaluate
               completes with a PROMISE — which is exactly why `await` at a module's top level is observable and
               is a SyntaxError in the classic entry. Compiling every entry with JS_EVAL_TYPE_GLOBAL is how
               `<script type=module>` reached this line indistinguishable from a classic one and came back a
               SyntaxError from a parser that is fine. */
            int started;   /* did the program START? — a classic gets a frame, a module has already evaluated */
            if (stype == SCRIPT_TYPE_MODULE) {
                JSValue ev;
                /* `prog_name` IS THIS MODULE'S RECORD IDENTITY — see the name above. JS_FlowEvalModule asserts
                   it is non-empty for the same reason: it keys the module map, it is `import.meta.url`, and it
                   is the base the loader registers each fetched dependency under. */
                /* §4.12.1.1 "Processing model", "execute the script element", the MODULE arm's first step:
                   "Assert: document's currentScript attribute is null." It is the standard's own assertion and
                   it is BEHAVIOUR here rather than an exemption — the module arm has no set/restore bracket at
                   all, so `import.meta` is what a module reads instead, and a non-null slot at this line means
                   a classic script's bracket leaked past its own completion. */
                DCHECK(document_current_script_is_null(prog_ctx),
                       "§4.12.1.1's module arm asserts this document's currentScript is null and it is not — "
                       "a classic script's §3.1.7 bracket has outlived the program it belonged to, so this "
                       "module would run with some other script element globally exposed");
                ev = JS_FlowEvalModule(prog_ctx, body, body_n, prog_name, src_flags);
                started = !JS_IsException(ev);
                if (started) module_report_rejection(prog_ctx, ev);   /* §8.1.4.4 step 8 */
                JS_FreeValue(prog_ctx, ev);
            } else {
                f->frame = JS_FlowNew(prog_ctx, body, body_n, prog_name, src_flags);   /* classic non-strict global */
                started = (f->frame != NULL);
                /* §4.12.1.1's CLASSIC arm, steps 1-2, and the reason they are HERE and not around a call: the
                   arm's third step ("run the classic script") is the JS_FlowNew above plus every JS_FlowResume
                   that follows it, spread over an unbounded number of scheduler steps with sibling flows
                   running in between. A C save/restore bracket would set the slot for whichever flow happened
                   to be running when the NEXT program started — CLAUDE.md §scheduler's "an operation that
                   becomes a work item takes its inputs with it". The write instead rides this flow's COW
                   delta (core/dom/document_current_script.c), so a context switch unapplies it, a park carries
                   it, and a resume brings it back byte-identical.
                   ONLY IF THE PROGRAM STARTED: a compile that failed runs no script, which is what
                   §4.12.1.1's own "if el's result is null, then fire an event named error at el, and return"
                   says one step earlier — so there is nothing to bracket and the restore below is not owed. */
                if (started && flow_dyn_el(f)) document_current_script_set(prog_ctx, flow_dyn_el(f));
            }
            if (!started) {
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
                /* FOR A MODULE THIS IS THE COMPILE AND ONLY THE COMPILE. It used to name linking too, and that
                   stopped being true when loading became its own phase: §8.1.4.4's module entry now LOADS the
                   graph (16.2.1.6.1.1, asynchronous — the host fetches each specifier) and links it only upon
                   that load's fulfilment, so a graph that fails to load or to link REJECTS the evaluation
                   promise and is reported by module_report_rejection, exactly as HTML's "set moduleScript's
                   error to rethrow" says. What can still fail before a module starts is 16.2.1.7.1 ParseModule,
                   which is the page's own SyntaxError. */
                /* A CROSS-AGENT OPERATION'S PROGRAM IS THE ENGINE'S OWN TEXT (core/frame/remote_op.c), so it
                   parses or this engine wrote it wrong — and skipping it would leave the peer's flow parked on
                   an answer that is now never coming.
                   BOTH ASSERTS CARRY THE SyntaxError, WHICH IS THE ONLY PART A READER CAN ACT ON. They named
                   the EVENT and freed the description of it one line below, and the cost was measured: five of
                   eleven real production bundles (developer.mozilla.org, vuejs.org) abort here, and the abort
                   said nothing about which construct the parser refused or where. `exc` already carries the
                   message and, through the error's stack slot, the position — so the DCHECK's own text is where
                   that belongs, exactly as the reader of a `@WHY` gets a file:line and no stack. */
#if APICLIENT_DEV
                if (kind == DYN_PAGE_SCRIPT || kind == DYN_CROSS_AGENT_OP) {
                    char et[320];

                    result_error_text(ctx, exc, et, sizeof et);
                    DFAILF("%s — the compile reported: %s",
                           kind == DYN_PAGE_SCRIPT
                               ? "flow_step: a page <script>/chunk did not start — its source did not COMPILE "
                                 "(a classic script's program, or a module script's 16.2.1.7.1 ParseModule)"
                               : "the program that performs a cross-agent operation did not compile — it is "
                                 "this engine's own text, and skipping it parks the asking flow on an answer "
                                 "nothing will send",
                           *et ? et : "(a throw this engine could not describe)");
                }
#endif
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
            /* A MODULE HAS ALREADY RUN AS FAR AS IT GOES IN THIS UNIT, so this entry is finished here and the
               flow leaves with no frame of its own. There is nothing for the resume below to hold: a module
               body is an async activation the flow machinery owns, not a program handle — it parked itself into
               the slot the top of this loop drains (a top-level `await`, or a loop back-edge under the forced
               preempt), and the scheduler resumes it there like every other parked continuation. Its evaluation
               promise settling is likewise a FLOW resuming and not a drain: the reaction is a microtask on THIS
               flow's queue, run by the checkpoint above, which is why nothing here waits for it. */
            if (stype == SCRIPT_TYPE_MODULE) { f->script_i++; return 0; }
        }
        /* HTML §8.1.4.4 "Calling scripts", run a classic script step 8's third bullet — built at the completion
           below and INSTALLED after that completion's frame has been freed, which is the only reason it is
           declared out here rather than beside the completion it belongs to: the report takes the slot, so it
           cannot be put there while the slot is still occupied. NULL for every completion that owes none, which
           is every completion but a classic script's abrupt one. */
        JSValue *report = NULL;
        {
            /* A <script>'s completion value is not observable to the page (only an eval API surfaces one), so it is
               taken and released here — never DISCARDED by the engine, which would hide a live value from the host. */
            JSValue cv = JS_UNDEFINED;
            /* IS THE FRAME ABOUT TO RUN A PROGRAM OR A CALL — asked of the FRAME, before it can complete and be
               freed, because the two completions mean different things below and nothing else can tell them
               apart. A driven orphan's flow runs its call and then goes on to run whatever that call queued
               (a lazy chunk it loaded is an ordinary program of this flow's sequence), so `f->orphan` says what
               the FLOW is and this says what THIS completion is. The position in the sequence cannot answer it:
               a row queued by the running call moves the cursor back inside the sequence while the call frame
               is still live. */
            int is_call = JS_FlowIsCall((JSValue *)f->frame);
            g_step_unit = "resume-program";
            int r = JS_FlowResume(ctx, (JSValue *)f->frame, &cv);
            /* A CROSS-AGENT OPERATION'S COMPLETION IS AN ANSWER, AND IT IS ASKED FIRST because the two readings
               of a throw are mutually exclusive: this program is another agent's operation, so its throw
               belongs to the flow that ASKED — reported here as this document's page error it would be lost and
               the peer would resume with `undefined` where the spec propagates a throw. */
            if (r == 0 && flow_dyn_kind(f) == DYN_CROSS_AGENT_OP)
                flow_answer_perform(ctx, f, cv);
            /* THE REPORT ITSELF COMPLETING ABRUPTLY IS THIS ENGINE'S DEFECT, and it is asked FIRST because a
               report frame is a call root and so is a driven orphan's — the arm below would read it as the
               exploration surface and swallow it, which is the one reading that hides an engine bug inside the
               one category of throw this engine deliberately ignores. Nothing the PAGE writes reaches here:
               DOM §2.9's inner invoke step 2.11 CATCHES a listener's throw and reports it (event_target.c
               declares catches_abrupt for exactly that), and HTML §8.1.4.6 step 6's error reporting mode is
               what stops the report of a report. So what completed abruptly is a step of §8.1.4.6 itself. */
            else if (JS_IsException(cv) && f->reporting) {
                JSValue e = JS_GetException(ctx);
                /* THE THROWN VALUE'S OWN TEXT, RECORDED BEFORE THE ABORT DISCARDS IT. `msg` reaches the
                   assertion line unescaped (check.h says so), so the exception's message cannot be put in it —
                   and an assert that names a failure while freeing the value describing it names a problem
                   nobody can act on (result.h states the same rule and the same cost). */
                result_page_error_value(ctx, e);
                JS_FreeValue(ctx, e);
                /* THE FLAG IS NOT CLEARED HERE, AND THAT IS WHAT KEEPS THE RELEASE BUILD HONEST. This DFAIL
                   compiles out in release, so control falls through to the tail — where the flag is what says
                   this frame was the row's remaining work and therefore owes §4.12.1.1 step 4's restore.
                   Clearing it at the abort would leave `document.currentScript` set for the rest of the
                   session in exactly the build that cannot report why. */
                DFAIL("HTML §8.1.4.4 \"Calling scripts\" step 8's report of a classic script's abrupt "
                      "completion ITSELF completed abruptly. DOM §2.9 inner invoke step 2.11 catches a "
                      "listener's throw and §8.1.4.6 step 6's error reporting mode stops a report of a report, "
                      "so this is a step of §8.1.4.6 throwing rather than the page — the value's own text is "
                      "the last entry of the result document's `pageErrors`, and what it names is built in "
                      "core/events/report_exception.c");
            }
            /* A DRIVEN ORPHAN THAT THREW IS THE EXPLORATION SURFACE AND NOT A PAGE ERROR — the one completion
               here that is nobody's defect. The page never called this function; this engine did, with unknown
               input in place of every argument and the receiver, so `this.config.url` throwing a TypeError is
               what forced execution ON UNKNOWN INPUT looks like when it works. CLAUDE.md names it in the list
               of things that are deliberately not a `@WHY` ("a forced-exec flow THROWING on opaque/attacker
               input"), and recording it as the page's would report a document error per uncalled function —
               attributing to the bundle a throw that only this engine's invocation could produce. The throw is
               CONSUMED rather than left live, which is the half that is not optional: the next thing to ask the
               context anything would find it. */
            else if (JS_IsException(cv) && f->orphan && is_call) {
                JSValue e = JS_GetException(ctx);
                JS_FreeValue(ctx, e);
            }
            /* HTML §8.1.4.4 "Calling scripts", RUN A CLASSIC SCRIPT STEP 8, THIRD BULLET: "Otherwise, rethrow
               errors is false. Perform the following steps: 1. Report an exception given by
               evaluationStatus.[[Value]] for script's settings object's global object. …"
               THIS IS AN `error` EVENT AT THE GLOBAL AND NOT A DIAGNOSTIC, and the difference is a capability
               the engine claimed and did not have: a page that installs `window.onerror` for its own routing or
               telemetry took one path in a browser and another here, and the message that named the missing
               capability reached the result document without ever reaching the page. The console line is still
               written — §8.1.4.6 step 7.3, at the one place the standard puts it, behind step 7's notHandled,
               through the hook registered beside the unhandled-rejection one — so a script that throws with
               nothing listening reports exactly as it did, and one whose listener calls `preventDefault()` now
               correctly reports nowhere.
               THE OTHER TWO BULLETS OF STEP 8 ARE `rethrow errors is true`, and this engine has exactly one
               caller that wants the throw back: a peer's operation, whose completion the arm at the top of this
               chain hands to the agent that ASKED. That arm is step 8's first bullet, reached before this one
               and for the reason the standard separates them — so the third bullet is unconditional here.
               THE REALM IS THE PROGRAM'S. "script's settings object's global object" is the document the row
               belongs to, which is the realm the program was COMPILED in — not whichever realm the scheduler
               happens to be holding, and not the session's. */
            else if (JS_IsException(cv)) {
                JSValue e = JS_GetException(ctx);
                report = report_exception_flow(doc_realm(flow_dyn_doc(f)), e);
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
            if (r == 0 && JS_IsString(cv) && flow_dyn_kind(f) == DYN_JAVASCRIPT_URL)
                DFAIL("a `javascript:` URL evaluated to a STRING — HTML §7.4.2.3.2 step 9 turns that into a new "
                      "Document that REPLACES the target navigable's active document, built from a synthesized "
                      "`text/html;charset=utf-8` response whose body is the string. §7.4's navigate can only "
                      "load an address the host FETCHES (navigable.c's js_nav_load_step asks "
                      "`document.fetch\\t<url>`), so build the navigate that takes a RESPONSE THE ENGINE ALREADY "
                      "HAS and route this through it");
            JS_FreeValue(ctx, cv);
            /* §8.1.4.4 step 8 IS ABOUT AN evaluationStatus, and a frame that suspended or detached has none.
               Both returns below advance past this row without ever reaching the install, so a report built on
               one of those paths would be dropped silently — and the exception it carries would have come from
               somewhere that is not this program's completion. */
            DCHECK(report == NULL || r == 0,
                   "§8.1.4.4 step 8's report was built for a frame that has not COMPLETED — a suspended or "
                   "detached base has no evaluation status to be abrupt, so the exception this report carries "
                   "belongs to something other than this program's run");
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
                /* AND THAT IS THE ONE COMPLETION PATH §4.12.1.1's CLASSIC RESTORE CANNOT REACH, so it is
                   asserted rather than left to be discovered. A detached base is a MODULE body's continuation
                   — a module has no bracket — and a classic program that detached would leave this document's
                   §3.1.7 currentScript set for ever, with the next program's set asserting on it one step
                   later and naming the wrong site. Named here, where it would be born. */
                DCHECK(flow_dyn_el(f) == NULL || document_current_script_is_null(doc_realm(flow_dyn_doc(f))),
                       "a classic script's program DETACHED its base while §4.12.1.1's currentScript bracket "
                       "was open — the restore is at the program's completion and a detached base never "
                       "reaches one, so this document's currentScript would stay set for the rest of the "
                       "session. Route the restore through whatever drives the detached continuation");
                /* THIS ROW IS OVER, WHICH THE CURSOR ALREADY SAYS AND THE FLAG MUST SAY WITH IT. A frame this
                   flow no longer owns cannot be §8.1.4.4 step 8's report of the row the cursor is about to
                   leave; left standing, the flag would make the NEXT program's completion read as a report. */
                f->reporting = 0;
                f->frame = NULL; f->script_i++;
                return 0;
            }
            /* THE PROGRAM RAN TO ITS END — the ONE event that moves this document's completed depth, recorded
               here because this is the only line in the engine at which a program's own completion is the fact
               in hand. The three other sites that advance `script_i` are not completions and must not be
               counted as ones: a module has evaluated to a PROMISE rather than a value, a detached base has
               handed its continuation to an awaited promise, and a compile failure never started. See
               g_completed.
               A DRIVEN ORPHAN'S CALL IS A FOURTH, and the first one that is not a program at all: its frame
               holds no row of the sequence, so its cursor is one PAST the last program and counting it would
               report this document as having run a program it does not have. */
            if (!is_call && f->script_i > g_completed) g_completed = f->script_i;
            /* §4.12.1.1's CLASSIC arm, step 4: "Set document's currentScript attribute to oldCurrentScript."
               THIS is the other end of the bracket the compile opened, and it is placed at the ONE line a
               program's own completion is the fact in hand — the same line `g_completed` is written at, and for
               the same reason. It covers a program that RAN TO ITS END and one that THREW alike, because
               §8.1.4.4's run-a-classic-script REPORTS the exception rather than propagating it and the restore
               is after the run either way.
               AND "AFTER THE RUN" IS NOW A DIFFERENT LINE, WHICH IS THE WHOLE OF WHAT THE REPORT MOVED. That
               sentence was written while the report did not exist, so "after the run" and "after the throw"
               were the same instant; they are not. §4.12.1.1 restores at STEP 4, after step 3's run a classic
               script, and §8.1.4.4 step 8's report is a step INSIDE that run — so a page's `error` listener
               reads the throwing `<script>` out of `document.currentScript`, exactly as it does in a browser,
               and restoring at the throw would hand it null. A program that owes a report has therefore NOT
               finished step 3, and the restore travels to the report frame's own completion.
               A PARKED PROGRAM NEVER GETS HERE, which is the whole design: a mid-frame yield returned above
               with the frame still live, and the slot went with the flow's delta.
               `is_call` IS THE GUARD BECAUSE THE CURSOR IS NOT ONE. A driven orphan's call frame sits past the
               end of the sequence — where flow_dyn_el answers NULL — but a row the running call QUEUED moves
               the cursor back inside it, so without this the completion of a CALL would restore a row whose
               program has not started. `f->reporting` is what tells the ONE call frame that IS this row's
               remaining work from the one that is somebody else's — see flow.h. */
            if (!report && (f->reporting || !is_call) && flow_dyn_el(f))
                document_current_script_restore(doc_realm(flow_dyn_doc(f)), flow_dyn_el(f));
        }
        JS_FlowFree(ctx, (JSValue *)f->frame);
        /* §8.1.4.4 STEP 8.3.1 TAKES THE SLOT STEP 8'S PROGRAM JUST VACATED, and the cursor does NOT move: the
           report is the rest of this row's run-a-classic-script, so the row is finished by the report's
           completion and advanced exactly once, there. Everything the `if (!f->frame)` block above would do —
           step 8.3.2's microtask checkpoint first among them — is skipped for as long as this frame lives,
           which is the order §8.1.4.4 states and the reason the report is a frame at all. */
        if (report) { f->frame = report; f->reporting = 1; return 0; }
        f->reporting = 0;
        f->frame = NULL; f->script_i++;   /* this script done -> next */
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
 * makes the two agree by construction rather than by two people remembering to.
 *
 * AND A THIRD CONSUMER, which is why engine.h declares it: a HOST that reports on its own run needs the same
 * quantity to decide WHEN to report, and a host carrying a cadence of its own would be the second definition
 * this paragraph exists to prevent. */
long engine_work_done(void) {
    return decide_fork_total() + flow_created_count() + g_jobs_run + g_switches;
}

static void flow_switch_out(JSContext *ctx, Flow *f) {   /* pause f: snapshot its solver state, restore baseline */
    /* the PARKED CONTINUATIONS travel with the flow, for the reason the delta does: each resumes a suspended
       async activation of THIS flow, under THIS flow's heap. Left in the runtime they would be resumed by
       whichever flow the scheduler picked next — against the wrong delta — or left behind entirely.
       IT IS THE WHOLE SET, and it always should have been: a step reaches as many bases as it reaches, and a
       take that moved one park would leave the others in the runtime for the next flow. It could not be
       written that way while the runtime held a single slot, and this line's own comment used to say the
       second park hit that slot's assertion — which is the defect the record moving onto the base ended. */
    f->parked = JS_TakeParkedFlows(JS_GetRuntime(ctx));
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
    JS_PutParkedFlows(JS_GetRuntime(ctx), f->parked);
    f->parked = NULL;
    if (!f->delta) f->delta = cow_delta_new();
    cow_set_current((CowDelta *)f->delta);
    cow_apply(ctx, (CowDelta *)f->delta);
    dom_buf_load(f->dom, f->dom_n, f->dom_cap);   /* attach this flow's DOM head (NULL/0 for a fresh flow = empty) */
    dom_base_load(f->dom_base);                   /* ...and its base chain, BEFORE dom_apply walks it */
    dom_apply();                                  /* DOM twin of cow_apply: replay this flow's document writes */
    /* A FRESH FLOW CARRIES NO RECORDED PATH, asserted here because this is the line that would DROP one. The
       two branches below are told apart by `started` alone, so a flow whose installer set `dec_blob` and left
       the bit clear takes decide_enter, replays from nothing, and never reads the path it was given — and the
       pointer is still live at that flow's first suspend, where the line `f->dec_blob = decide_suspend()`
       overwrites it and leaks the blob together with its reference on the frozen segment, keeping the whole
       prefix under it alive. Ignored and leaked, in silence, with the only symptom a feature that does not
       work. A flow standing on a recorded path is installed as a TRIPLE — `started`, `dec_blob`, `pin_blob` —
       by cold.c's 'f' and 'c' arms and by solve.c's @S re-injection, and this is where setting fewer than all
       three stops being invisible. */
    DCHECK(f->started || f->dec_blob == NULL,
           "a flow that has never run carries a recorded decision path — decide_enter is about to ignore it "
           "and the flow's first suspend will overwrite the pointer, so the path is dropped AND its segment "
           "reference leaked. Whatever installed it set `dec_blob` without `started`");
    if (!f->started) { f->started = 1; decide_enter(ctx, f); }   /* fresh flow: replay from cursor 0 */
    else {                                                        /* paused flow: restore where it left off */
        decide_resume(f->dec_blob, f->fn);   decide_blob_free(f->dec_blob); f->dec_blob = NULL;
        concolic_pins_resume(f->pin_blob);   concolic_pins_blob_free(f->pin_blob); f->pin_blob = NULL;
    }
    flow_set_running(f);
    /* WHAT THIS FLOW WAS RANKED ON WHEN IT TOOK THE THREAD. Recorded HERE because this is the moment the WFQ's
       answer was "this one" — the pick that led here compared it against every runnable member and found none
       strictly better, so from this instant the value yield may only fire if one of these moves. The
       hook's assertion reads them; nothing decides from them. */
    g_ranked_gen = flow_frontier_gen(); g_ranked_val = f->val;
    g_ranked_silence = flow_silence_notch(f);  /* the aging term — see the assertion in preempt_hook */
    g_ranked_visits = f->visits;               /* …and the optimism term's, which is a count and not a clock */
    g_ranked_dist = f->cand_dist;              /* …and the fitness term's, which is a reading and not a payment */
}

static void flow_finish(JSContext *ctx, Flow *f) {   /* f completed: tear down its interleaving state + remove */
    g_finished++;   /* the one place a flow ever COMPLETES — see g_finished */
    /* "all scripts, chunks, jobs and fetches are done" cannot be true with a continuation still parked — the
       loop above resumes one before it can answer that. Asserting it here is what keeps the park inside the
       no-work-item-is-ever-dropped rule rather than merely intending to. */
    DCHECK(!JS_HasParkedFlow(JS_GetRuntime(ctx)) && f->parked == NULL,
           "a flow finished with a continuation still parked — that flow's async activation is dropped");
    /* A FINISHED FLOW HAS NO LIVE FRAME. `frame` is the JS_FlowNew handle holding this flow's heap frame chain
       — every activation, closure and local it is suspended across — so one left behind at finish retains the
       whole execution graph, and the runtime's leak walk reports it as thousands of anonymous Functions with no
       hint of the owner. Asserted rather than freed defensively: if a flow can reach here with one, the finish
       path ran while it was still suspended and freeing it silently would hide that. */
    DCHECK(f->frame == NULL, "a flow finished with a live preemptible frame — its whole activation chain, and "
                             "everything those frames close over, is retained by a handle nothing will free");
    DCHECK(flow_deliver_pending(f) == 0,
           "a flow finished still holding routed deliveries — each is a message a peer sent and this document "
           "never received, and a page that never receives one is indistinguishable from a page that "
           "registered no handler");
    /* AND THE SAME FOR AN OPERATION, at both ends of it: a record never turned into a program is a question
       nobody performed, and a token never spent is a peer's flow parked at the line that asked, forever. */
    DCHECK(!flow_owes_answer(f),
           "a flow finished holding a cross-agent operation — either the record was never performed, or its "
           "program's completion was never sent, and either way the flow that ASKED is suspended at the line "
           "that asked it with nothing coming. Asked of the QUEUE as well as the arrival slot, because a "
           "started operation's token is on the row of the program that answers it");
    /* THE QUEUE AND THE PENDING LIST ARE EMPTY HERE, AND THAT IS ASSERTED RATHER THAN CLEANED UP AFTER. Both
       used to be walked and freed "defensively" right here, which is the fallback shape: the walk can only ever
       run when a work item is being DROPPED, and freeing it quietly is precisely how that drop stays invisible.
       Neither is reachable — flow_step decides "finished" only after offering the job queue a turn (the
       microtask checkpoint before every program runs one, the task arm below the sequence runs the rest of
       them), and only when the register is empty, since a pending entry with a value drains and one without
       reports host-owed. The release below DOES free both, because an EVICTED flow legitimately holds them (the
       cold tier's recipe re-enqueues the reactions and re-issues the requests as it replays). So the assertion
       has to stand HERE, where "finished" is the claim being made, and not there. */
    DCHECK(flow_job_pending(f) == 0, "a finishing flow still held queued jobs — its promise reactions and timer callbacks "
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
/* THE TWO ASKS THAT COME WITH A RESUME POINT — and this is the whole of the wrapper. An interpreter at an
 * OP_if and a step driver holding a machine that yielded JS_STEP_FORK both clone their activation a moment
 * after they ask; a C builtin calling the same symbol by name does not. Nothing at the seam could tell those
 * apart, so every C-body fork was stashed for a consumer that never came and the diagnosis landed at the NEXT
 * fork anywhere in the agent. The declaration is made HERE because this is the one place that knows: these two
 * function pointers ARE the interpreter's way in.
 * IT IS NOT A NESTING COUNTER. Neither seam runs page code, so nothing can ask again underneath one; the
 * assertion says so rather than leaving a saved-and-restored depth to imply it. */
static int engine_branch_hook(JSContext *ctx, JSValueConst cond) {
    int r;
    DCHECK(!g_fork_snapshot_owed,
           "the interpreter asked for a branch arm while another snapshot-owning ask was still open — the "
           "decision seam runs no page code, so a second one means something re-entered it and the inner "
           "fork would be built against the outer ask's activation");
    g_fork_snapshot_owed = 1;
    /* NO NON-FORKING ARM, AND THE TABLE IS WHY. This wrapper is FC_EXPLORE's `branch` and nothing else installs
       it, so a session that does not fork never reaches this line: the interpreter finds no hook, reads -1, and
       the ordinary ToBool decides the `if` — which IS the interpreter's non-forking answer, made where the
       absence is rather than passed down as a value. SOLVER_NO_NONFORKING_ARM states that, so a session that
       somehow got here with forking off crashes at the seam naming the predicate. */
    r = solver_decide(ctx, cond, SOLVER_NO_NONFORKING_ARM);
    g_fork_snapshot_owed = 0;
    return r;
}
static int engine_outcome_hook(JSContext *ctx, JSValueConst over, const char *op, int n) {
    int r;
    DCHECK(!g_fork_snapshot_owed,
           "the step driver asked for an outcome arm while another snapshot-owning ask was still open — see "
           "engine_branch_hook");
    g_fork_snapshot_owed = 1;
    r = solver_outcome(ctx, over, op, n);
    g_fork_snapshot_owed = 0;
    return r;
}
static const JSFlowControlHooks FC_EXPLORE = { .branch = engine_branch_hook, .outcome = engine_outcome_hook,
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
    JS_SetJobRemoveHook(NULL);
    JS_SetFlowControlHooks(&FC_OFF);
    g_sess_forking = 0;   /* …and the same bit for the callers that ask the seam by symbol — see engine_sched_begin */
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
    /* AND NO FLOW MAY BE SEEDED FROM A DOCUMENT THIS SESSION NO LONGER HAS. The hook is taken down BEFORE the
       table it reads is freed, so a flow created after this line fails the seeding assert in flow_add rather
       than reading a table that has been given back. */
    flow_set_seed_hook(NULL);
    /* AND THE ROOT DOCUMENT'S SEED TABLE, which is this engine's own from end to end: the host's inventory is
       three borrowed columns freed by whoever built them (doc_scripts_free), and this table is the copy plus
       §4.12.1.1's encoding-parsed addresses that engine_sched_begin computed ABOUT it. Given back at the one
       point a session ends, which is the same argument this function's own comment makes about the hooks. */
    if (g_root_scripts) {
        /* THE BODY GIVES A REFERENCE BACK. This table holds one on each of its programs and every flow seeded
           from it holds one of its own, so the buffer outlives the session exactly as long as some flow still
           holds the row — which is the whole point of sharing it. */
        for (int i = 0; i < g_root_n; i++) {
            if (g_root_scripts[i].body) dyn_body_unref(g_root_scripts[i].body);
            free(g_root_scripts[i].url);
        }
        free(g_root_scripts);
        g_root_scripts = NULL;
    }
    g_root_n = 0;
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
/* …AND THE DELTA THE SCHEDULER HOLDS ACROSS IT, for the identical reason and beside it deliberately: the two
   are one fact about the running flow (which generation it is at, and where its captures land), so a boundary
   that put one down and left the other up was recording the HOST's construction of a fresh object into a
   stranger's head. See engine_sched_step. NULL between slices means the host's time captures into nothing,
   which is what `!g_current` has always meant (cow.h). */
static CowDelta *g_slice_delta;

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
    /* THE DEBT LEAVES WITH THE FLOW, AND IT IS TWO DEBTS. Counted by KIND rather than whole (see g_paged_owed):
       a synchronous request is answered by ID at a call site and a fetch reply is paired by (method, url), so
       a register counted whole hands the fetch side a credit that belongs to the request side, and the next
       reply the host genuinely mispairs is excused by it.
       AND ON THE REPLY SIDE BY WHAT IS STILL OWED, WHICH IS NOT SYMMETRIC WITH THE REQUEST SIDE. The two doors
       have different arity (pending.h, pending_owed_replies): the reply door answers a request ONCE and drops
       it from the join the moment it carries a value, so an ANSWERED entry sold here is a reply already
       delivered and crediting it is the same over-count as crediting a HOSTREQ was — the surplus is spent by
       the next reply the host genuinely mispaired, and qjs_provide's pairing assert goes quiet again. The
       request door answers one id once per PEER TIMELINE, so an answered HOSTREQ entry is still a rendezvous
       more answers may arrive for and stays in the reported count. */
    g_paged_reqs += pending_count_kind(tail->pending, FLOW_PENDING_HOSTREQ);
    g_paged_owed += pending_owed_replies(tail->pending);
    /* AND THE DEBT THAT LEAVES BY BEING GIVEN BACK, which is where the refusal that stood here used to abort.
       A recipe cannot carry a rendezvous token (engine.h: it is the zone's name, it has no generation, and it
       does not outlive that zone's session), so the question is RETURNED rather than parked — and returned per
       FLOW here rather than per frontier, because this is a PARTIAL park: the instance keeps running and the
       operation is attached to every other timeline too, so the notice belongs to the last holder leaving and
       usually to nobody at all. Selling a flow that is one of many holders costs the peer one timeline's
       answer, which is exactly what a timeline that never existed would have cost it, and the remaining
       holders still answer under the token the zone still has. */
    engine_retract_flow(g_sess_ctx, tail);
    cold_park_flow(tail);
    flow_release(g_sess_ctx, tail);
    g_flows_sold++;
    return 1;
}

void engine_sched_begin(JSContext *ctx, char **bodies, char **srcs, const ScriptType *types,
                        lxb_dom_element_t **els, int n, int forking, const char *recipes) {
    DCHECK(!g_sess_live, "engine_sched_begin while a session is already running — one scheduler, one session");
    /* A JOB THAT WAS ENQUEUED BEFORE THIS LINE IS A DROPPED WORK ITEM, and this is the one place it can be
       caught. The enqueue hook is installed BELOW, so anything queued earlier went to quickjs's own global
       list — which nothing in this engine ever drains (engine_enqueue_job says so at the other end), so it is
       not deferred to a default, it is lost. It is not hypothetical: §4.8.5's insertion steps run for every
       <iframe> in the INITIAL MARKUP when the document is installed, and an <iframe src> there enqueues §7.4
       step 14's LOAD — so a frame in a page's own markup never loads, with no assert, no notice and nothing in
       the output to distinguish it from a page that had none. Every host installs its document before it opens
       a session, so every host has it.
       WHAT TO BUILD, because the ordering alone does not fix it: seeding the session first does not help — the
       flow is not switched in until the first step, so the hook would find no running flow — and the job cannot
       be run here (that is page code entered from the host with no flow base). The initial markup's insertion
       steps are the FIRST FLOW'S work, which is what §Boot already says: boot is the forking first flow and its
       creations are the baseline. Give the document install a flow to run in, or give this call a way to adopt
       what the runtime is already holding. */
    DCHECK(!JS_IsJobPending(JS_GetRuntime(ctx)),
           "a job was enqueued BEFORE this session began — it is on quickjs's global list, which this engine "
           "never drains, so it is a work item the frontier will never run: the document was installed before "
           "the scheduler existed and §4.8.5's insertion steps enqueued §7.4's load for an <iframe src> in the "
           "initial markup");
    g_sess_ctx = ctx;
    g_sess_cur = NULL; g_sess_live = 1;
    /* THE THREE ARRAYS ARE ONE INVENTORY, so a session opened with a length and only two of them is a table
       whose third column the seeding below would read off the end. Asserted where the borrow is taken, because
       that is the last moment the caller that built them is still on the stack. */
    DCHECK(n == 0 || (bodies != NULL && srcs != NULL && types != NULL),
           "a session was opened over a script inventory missing one of its three columns — bodies, srcs and "
           "types are one table with one length, and the seed below reads all three");
    /* THE DOCUMENT THIS SESSION IS OF. A session is opened over the instance's ROOT document, and its script
       sequence is that document's — asserted against the realm rather than assumed, because everything below
       compiles by asking the DOCUMENT for its realm and a session whose ctx is some other document's realm
       would run its own scripts somewhere else. */
    g_sess_doc = world_local_doc();
    DCHECK(doc_realm(g_sess_doc) == ctx,
           "a session was opened with a realm that is not the root document's — the session's scripts are that "
           "document's programs, so they would be compiled in one realm and belong to another");
    /* HTML §4.12.1.1 "Processing model": "Let url be the result of encoding-parsing a URL given src, relative to
       el's node document." RESOLVED ONCE, HERE, because the standard computes this url ONCE per element and then
       uses that one value for both of the things this engine asks it for — §8.1.4.2's fetch of the script and,
       for a module, the record's [[HostDefined]] identity. Two call sites deriving an address separately is two
       addresses for one script: the host would be asked for one and the module map keyed by the other.
       AND IT IS THE DOCUMENT'S REALM THAT ANSWERS, not `ctx` incidentally: the assert above has just established
       that they are the same realm, which is what makes reading the base here the same read the compile makes.
       The whole inventory belongs to g_sess_doc — every other document's scripts arrive through
       engine_join_document, which resolves against ITS realm.
       AND ONCE PER SESSION RATHER THAN ONCE PER FLOW, which is what makes it §4.12.1.1's "once per element".
       Every flow of this document is seeded from the table built here, and a flow is created throughout the
       session — a cold-resumed replay, an @S candidate — so resolving at the seed instead would ask the
       question again under whichever timeline's delta happened to be applied, and a `<base href>` one flow
       wrote would re-point another flow's bundle. */
    DCHECK(g_root_scripts == NULL && g_root_n == 0,
           "a session opened while the previous session's root script table was still allocated — the table is "
           "the ENGINE's, so the build below would leak every string in it; engine_session_close is the one "
           "place it is given back and it is the one place a session ends");
    g_root_scripts = n ? (RootScript *)calloc((size_t)n, sizeof(RootScript)) : NULL;
    CHECK(n == 0 || g_root_scripts != NULL,
          "engine: OOM building the root document's script table — every flow of this document is seeded from "
          "it, so without it the document's own bundle would silently not run in any timeline");
    for (int i = 0; i < n; i++) {
        /* A ROW IS SOMETHING, asserted at the row rather than at the read. The two columns are INDEPENDENT
           facts about one script and not two spellings of one — see RootScript — so what cannot happen is a
           row that is NEITHER: it would park the flow on nothing for the rest of the session. BOTH is the
           ordinary shape of an external script whose response the host already holds, and the exactly-one-of
           invariant that stood here rejected it: document_scripts.h's own contract says of an external entry
           that `bodies[i]` is "NULL until the host supplies it", and a host that supplies it was aborting the
           engine on the row it had just filled. */
        DCHECK(bodies[i] != NULL || srcs[i] != NULL,
               "a row of the root document's script inventory holds neither source text nor an address — "
               "§4.12.1.1's \"if url is failure … fire an event named error at el, and return\" means such an "
               "element runs no script and takes no position, so a row that is nothing is a row the seed would "
               "park a flow on and never fill");
        DCHECK(script_type_executes(types[i]),
               "a row of the root document's script inventory holds a type that executes nothing — an import "
               "map and a set of speculation rules are registered rather than run and §4.12.1.1's null type "
               "runs nothing at all, so document_exec_scripts drops all three before they reach a session");
        /* AND THE ELEMENT THE ROW IS THE PROGRAM OF — §4.12.1.1's "execute the script element" is a switch on
           EL and its classic arm sets this document's §3.1.7 `currentScript` to it. BORROWED: it names a node
           of the tree this session was opened over, which outlives the session, and it is the one column of
           this table that is not copied because there is no second element to make.
           AN ABSENT COLUMN IS A POSITIVE STATEMENT — "no program in this sequence came from a `<script>`" —
           and it is what a host driving a SYNTHESIZED list says (wpt_runner.c builds its own prologue and
           epilogue). §3.1.7's answer while such a program runs is null, which is that section's own answer for
           a document that is not executing a script element, and not a hole this line fills. */
        g_root_scripts[g_root_n].el = els ? els[i] : NULL;
        g_root_scripts[g_root_n].type = types[i];
        /* THE ADDRESS FIRST, AND FOR EVERY ROW THAT HAS ONE — including a row that also has its source text.
           §4.12.1.1's OWN NEXT STEP for a `src` that does not encoding-parse — "If url is failure, then queue
           an element task on the DOM manipulation task source given el to fire an event named error at el, and
           return" — so that element runs NO script and therefore takes NO position in the sequence, whether or
           not somebody handed bytes over for it: the only reason a host could hold those bytes is that it
           fetched the address this parse has just refused. Resolved BEFORE the body is copied so that refusal
           costs nothing. What is still owed is that error event, which needs a task on this document. */
        if (srcs[i]) {
            g_root_scripts[g_root_n].url = script_src_absolute(ctx, srcs[i], strlen(srcs[i]));
            if (!g_root_scripts[g_root_n].url) continue;
        }
        if (bodies[i]) {
            /* COPIED RATHER THAN BORROWED, because this table outlives the call and is read every time a flow
               of this document is created — which is throughout the session, not only at its start. The host's
               three columns are freed by whoever built them, and a table that pointed into them would hand a
               later seeding whatever that free left behind.
               ONCE, THOUGH, AND NOT ONCE PER FLOW: the copy is the shared body every timeline of this document
               then references (solver/dyn_body.h), so the document's own bundle is in this instance exactly one
               time however many flows the frontier grows.
               AND ITS LENGTH IS ITS `strlen`, WHICH IS THE TOKENIZER'S GUARANTEE AND NOT AN ASSUMPTION. This
               column is an INLINE `<script>`'s text as document_exec_scripts scanned it out of a parsed tree,
               and HTML §13.2.5.4 "Script data state" is the state that produced those characters: its U+0000
               NULL row reads "This is an unexpected-null-character parse error. Emit a U+FFFD REPLACEMENT
               CHARACTER character token" — so a NUL in the document's bytes is a U+FFFD by the time it is
               text, and `&#0;` is the same answer one state later (§13.2.5.84 "Numeric character reference end
               state": "If the number is 0x00 … Set the character reference code to 0xFFFD"). An inline script
               is therefore the one program shape in this engine that provably cannot carry one, which is why
               this seam still crosses as `char **` while every seam that CAN carry one now crosses as a pair. */
            g_root_scripts[g_root_n].body = dyn_body_new(bodies[i], strlen(bodies[i]));
            CHECK(g_root_scripts[g_root_n].body,
                  "engine: OOM copying a program of the root document into its seed table");
        }
        g_root_n++;
    }
    /* AND EVERY FLOW OF THIS DOCUMENT IS SEEDED FROM IT — installed BEFORE the frontier is seeded below,
       because the boot flow and every cold-resumed one are created by that seeding and each needs its programs
       at the moment it is created. */
    flow_set_seed_hook(engine_seed_root_flow);
    g_parked = 0;
    /* WHAT A SALE LEFT BEHIND IS THE INSTANCE'S TOO, AND A ZERO HERE WOULD FORGET IT RATHER THAN RESET IT.
       These three were assigned 0 at this line. Every host opens exactly ONE session per instance (main.c,
       wpt_runner.c's two alternative entries, test_forced.c's engine_run), so the assignment could only ever
       write 0 over 0 — but what it SAID was that the debt belongs to a session, and that is the opposite of
       true: the reply is owed by the HOST that was shown the request, the sold flow is on the cold tier
       re-issuing next session, and a reply still in flight across a session boundary must find its credit
       still there. A zero would hand the first such reply to qjs_provide's pairing DFAIL. So the claim is
       asserted instead of performed, and a second session opened on one instance crashes here rather than
       silently dropping the debt it must CARRY. Same reason as the line below. */
    DCHECK(g_paged_owed == 0 && g_paged_reqs == 0 && g_flows_sold == 0,
           "a session began on an instance that had already sold flows — the debt those sales left is owed by "
           "the HOST that was shown the requests, so it crosses a session boundary with them; it must be "
           "carried into this session, never zeroed, or the first reply that arrives for a sold flow is "
           "reported as the host's pairing being off");
    /* NOT RESET: g_host_asked/g_host_answered/g_host_answers_late are the INSTANCE's totals, not a session's —
       an instance opens one session, and a rate that restarted would be a level again (see g_host_asked). */
    /* WHAT AN UNCANCELLED REJECTION MEANS is this half's answer: the browser half fires the event and honours
       preventDefault, and a reason that survives that is a page error exactly like a script that threw. */
    unhandled_rejection_set_report_hook(result_page_error_value);
    /* AND THE SAME SENTENCE ONE SECTION UP — HTML §8.1.4.6 step 7.3, "the user agent may report exception to a
       developer console", reached only when step 7's notHandled is true. `pageErrors` IS that console: one
       line per distinct message, carried out of the run, which is exactly what a console is for a host whose
       output is a document. It is registered HERE rather than performed at the throw so that every caller of
       report-an-exception writes one — a §2.9 listener that threw, a custom element reaction, an
       animation-frame or IntersectionObserver callback — where before only a classic script did, and so that a
       page that cancels the `error` event stops it, which is what cancelling means. */
    report_exception_set_console_hook(result_page_error_value);
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
    /* …AND NO FLOW IS RUNNING YET, so the first slice opens with captures routed nowhere. Stated rather than
       left to the static's zero-initialisation, because a host that opens a SECOND session in one process
       would otherwise start it on the delta the previous session's last slice left behind — a pointer into a
       frontier that engine_session_close has already released. */
    g_slice_delta = NULL;
    dom_cow_set_ctx(ctx);                   /* the DOM delta needs ctx for the attribute taint-shadow dup/free */
    cow_set_ctx(ctx);                       /* …and the heap delta needs one for the component records it captures */
    JS_SetFlowControlHooks(forking ? &FC_EXPLORE : &FC_VERIFY);   /* preempt ALWAYS on; fork only when exploring */
    /* THE SAME ONE BIT, WRITTEN WHERE THE CALLERS THAT DO NOT GO THROUGH THAT TABLE CAN BE HELD TO IT. The
       hook table carries the explore/verify policy into the INTERPRETER; a C builtin asking the decision seam
       by symbol (core/dom/abort.c, core/timing/timer.c) never consults it, so a candidate re-fire — which
       §@S defines as ONE concrete path — could mint frontier members from inside its own verification. It is
       asserted rather than answered because what a non-forking answer MEANS differs per site (the interpreter
       falls through to ToBool, the step driver takes outcome 0, and the timer order has no third answer), and
       a seam that picked one for all of them would be choosing a page's timer order at random. */
    g_sess_forking = forking;
    JS_SetJobEnqueueHook(engine_enqueue_job);   /* ASYNC-AS-FLOW: reactions route to the enqueuing flow's queue */
    JS_SetJobDropHook(engine_drop_jobs);        /* …and §7.5.10 step 7 takes them back off it */
    JS_SetJobRemoveHook(engine_remove_job);     /* …and a toggle task tracker takes ONE back off, by name */
    /* THE FRONTIER IS THE ENGINE'S RESERVE, and this is what lets an allocator spend it. Installed with the
       session because that is exactly when there is a frontier to page: a refusal asks engine_reclaim_tail,
       which sells the lowest-weight member to the cold tier and answers "retry".
       EVERY ALLOCATOR, NOT THE RUNTIME'S ALONE — which is what solver/reclaim.h is, and it is there because
       measurement said so: with the edge in the runtime's allocator only, the fixture under a 2 GB wall paged
       nothing and aborted inside LEXBOR's, holding 315 MB of frozen document segments against 23 MB of heap
       ones. Most of this engine's memory is the HTML parser's, so most refusals are its. */
    reclaim_install(JS_GetRuntime(ctx), engine_reclaim_tail, NULL);
}

/* A SECOND DOCUMENT OF THIS AGENT, APPENDED TO THE FRONTIER THAT IS ALREADY RUNNING — see engine.h.
 *
 * IT IS ONE FLOW AND NOT A SESSION, and that is §scheduler's own sentence: a new document APPENDS its flows to
 * the one continuous frontier and does not start a scheduler, a run, an attention or a counter. engine_sched_
 * begin above is the ROOT document's seeding and asserts an EMPTY frontier because a boot flow and a parked
 * residue are alternatives for one document; this is a DIFFERENT document, so it adds to whatever is there.
 *
 * THE FLOW IS ADDED UNSEEDED, and that is what makes it this document's boot rather than a second boot of the
 * root's. A flow's cursor indexes ONE table — its own rows — and the ordinary flow_add seeds that table with
 * the ROOT document's programs, which is what a fresh timeline of this instance's document is. This member is
 * a timeline of a DIFFERENT document, so it takes none of them and everything queued below is its whole
 * program order, at cursor 0. It used to say the same thing by starting the cursor at the root's script count,
 * because the root's scripts lived in a separate array the cursor walked first; there is no such array and no
 * such offset now, so the statement is made by which entry creates the flow.
 *
 * THE ORDER IS THE DOCUMENT'S ORDER, and both halves of it are now positions in one sequence: an INLINE script
 * is a program queued in place and an EXTERNAL one is a DYN_SCRIPT_SRC entry queued in place, holding its
 * address until the reply fills it and stopping the flow there. It used to be a reply this flow was merely OWED,
 * joining the sequence when the register drained — after everything queued in this pass — so an inline script
 * FOLLOWING an external one ran before the bundle it is written after, and that was a named abort.
 * WHAT IS STILL NOT EXPRESSED IS §4.12.1's SCHEDULE, because it does not arrive: the inventory reaching this
 * entry carries bodies, srcs and types and not `sched`, so every external here takes its PARSE POSITION — right
 * for a `pending parsing-blocking script`, and an over-ordering for an `async` one, which belongs to a SET that
 * §13.2.7 waits for only before the load event. The hosts all compute the column (DocScripts.sched) and drop it
 * at this call; core/frame/navigable.c, which has it, sorts by it. */
void engine_join_document(JSContext *cctx, uint32_t doc, char **bodies, char **srcs,
                          const ScriptType *types, lxb_dom_element_t **els, int n) {
    Flow *f;
    RootScript *rows;
    int i, rown = 0;

    DCHECK(g_sess_live, "a document was joined to an instance with no live session — the flow its scripts are "
                        "the programs of would have no scheduler to run it, so the document would be parsed, "
                        "given a realm, and never execute a line");
    DCHECK(cctx != NULL, "a document was joined with no realm to compile its programs in");
    DCHECK(doc != 0, "a document was joined naming no document — zero is the world registry's NONE, so the "
                     "programs below would be compiled in whichever realm the session happens to be rooted at");
    DCHECK(doc != g_sess_doc,
           "the document this session was opened over was joined to its own agent a second time — its scripts "
           "are already the frontier's, and a second boot flow over them explores the un-forked path twice");
    DCHECK(doc_realm(doc) == cctx,
           "a document was joined naming a realm that is not the one it answers with — the queue carries the "
           "NAME and the compile resolves it, so the two disagreeing compiles this document's code in another "
           "document's Window");
    DCHECK(n == 0 || (bodies != NULL && srcs != NULL && types != NULL && els != NULL),
           "a document was joined with a script inventory missing one of its four columns — bodies, srcs, "
           "types and els are one table with one length, and the seeding below reads all four");

    /* THE JOINED DOCUMENT'S BOOT FLOW: an empty decision vector over this agent's baseline, minted with a root
       world of its own because it stands on no other flow's decisions. Every timeline this document ever has
       is a fork of it, exactly as every timeline of the root is a fork of the flow engine_sched_begin adds. */
    f = flow_add_unseeded(g_sess_ctx, JS_UNDEFINED, WORLD_NONE);
    /* THE SAME TABLE SHAPE THE ROOT DOCUMENT'S SEED HAS, built here because §4.12.1.1's encoding-parse belongs
       to THIS document: §4.4's API base URL is the joined document's, so `<script src=app.js>` in a document at
       `/app/child.html` is `/app/app.js`, and asking the realm the session happens to be rooted at is how a
       joined document's own bundle comes to be fetched from another document's directory. `cctx` — the joined
       document's realm — is therefore what the parse is asked of. One row shape, one seeding function, one set
       of asserts; the loop that used to queue these rows by hand is gone with the second representation it was
       the other half of. */
    rows = n ? (RootScript *)calloc((size_t)n, sizeof(RootScript)) : NULL;
    CHECK(n == 0 || rows != NULL,
          "engine: OOM building a joined document's script table — without it the document is parsed, given a "
          "realm and never executes a line, which is indistinguishable from a document that had no scripts");
    for (i = 0; i < n; i++) {
        DCHECK(script_type_executes(types[i]),
               "a joined document's script inventory holds a row whose type executes nothing — an import map "
               "and a set of speculation rules are registered rather than run and §4.12.1.1's null type runs "
               "nothing at all, so document_exec_scripts drops all three before they become rows and a fourth "
               "answer here means the types column was never written for this row");
        DCHECK(bodies[i] != NULL || srcs[i] != NULL,
               "a joined document's script inventory holds an entry with neither source text nor an address — "
               "§4.12.1.1's \"if url is failure … fire an event named error at el, and return\" means such an "
               "element runs no script and takes no position, so a row that is nothing is one this document's "
               "boot flow would park on and never fill");
        /* AND THE ELEMENT, borrowed from the tree the host handed over — §4.12.1.1's "execute the script
           element" is a switch on EL, and a joined document's `currentScript` is as observable as the root's. */
        DCHECK(els[i] != NULL,
               "a joined document's script inventory holds a row with no `script` element — every row is one "
               "element of one parsed tree");
        rows[rown].el = els[i];
        rows[rown].type = types[i];
        /* THE ADDRESS FIRST, AND FOR EVERY ROW THAT HAS ONE — the same shape as the root document's build and
           for the same reason: source text and base URL are two independent items of one script (§8.1.4.1
           "Scripts"), so a row a host handed over WITH its response still carries the address §8.1.4.2 makes
           that script's base URL.
           §4.12.1.1's OWN NEXT STEP for a `src` that does not parse — "fire an event named error at el, and
           return" — so the element runs no script and takes no position. It is the standard's answer and not a
           skip: what is still owed is that error event, which needs a task on this document. */
        if (srcs[i]) {
            rows[rown].url = script_src_absolute(cctx, srcs[i], strlen(srcs[i]));
            if (!rows[rown].url) continue;
        }
        /* THE COPY IS MADE ONCE, HERE, and every timeline of this document then references it — the row's body
           is the shared program text (solver/dyn_body.h) rather than a borrowed pointer into the host's
           inventory, so this document's own bundle is in the instance exactly one time however many arms its
           boot flow forks. Borrowing was sound only because the seed copied immediately; it no longer does.
           THE LENGTH IS THE `strlen` HERE FOR THE ROOT DOCUMENT'S REASON — this column is an inline
           `<script>`'s text off a parsed tree, and HTML §13.2.5.4 "Script data state" turns a U+0000 into a
           U+FFFD before it can become one. See engine_sched_begin's own note. */
        if (bodies[i]) {
            rows[rown].body = dyn_body_new(bodies[i], strlen(bodies[i]));
            CHECK(rows[rown].body,
                  "engine: OOM copying a joined document's program into its script table");
        }
        rown++;
    }
    engine_seed_scripts(f, doc, rows, rown);
    /* THE TABLE IS THIS CALL'S, NOT THE FLOW'S. engine_queue_into copies the address it is given into the
       flow's own row and takes a REFERENCE on the body, so both are given back here — the addresses resolved
       above are freed and the bodies released, each surviving exactly as long as some flow still holds the row
       that names it. The root document's table outlives this pattern because it seeds a flow every time one is
       created, and this one seeds exactly one. */
    for (i = 0; i < rown; i++) {
        free(rows[i].url);
        if (rows[i].body) dyn_body_unref(rows[i].body);
    }
    free(rows);
}

/* One QUANTUM. Returns ENGINE_STEP_DONE when the frontier is empty (the session is closed and its hooks are
   uninstalled) and ENGINE_STEP_YIELD when the slice expired with the frontier intact. The slice is a budget of
   CPU actually consumed (solver/quantum.h says what each host can measure) and it is NOT a cap: nothing is
   dropped, starved, reordered or forgotten across it — the next call resumes the same top flow on the same
   frontier, which is the razor §scheduler states. */
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

/* THE HOST'S ONE INPUT — see engine.h for what it is and for the two justifications this line used to rest on,
   both of which were false about this tree. The pick is the SCHEDULER'S OWN, with no incumbent to defend: a
   weight the host ranks an engine by has to be a weight the engine could actually convert into work, and
   flow_best answers over members that have told the scheduler they cannot run. A frontier whose every member
   is host-owed burns no CPU, so its weight never ages, so the evictee picker would never reach it.
   AND -Infinity IS THE ANSWER RATHER THAN A NEW BRANCH: it is already what an empty frontier publishes, so a
   stalled engine sorts last through the ordering the host already has. */
double engine_top_weight(void) {
    Flow *b = flow_next_to_run(NULL);
    /* WHY THE PICK CAME BACK EMPTY, ASSERTED AT THE ONE PLACE THE HOST IS TOLD. -inf from a NON-EMPTY frontier
       is a claim on the host — "I can convert no slice into work until you act" — so the thing that must be
       true is that there is something for the host TO act on: an unanswered entry on somebody's register, or a
       peer holding a reference into one of this agent's documents. Those are the same two the stall path
       answers STALLED for, and this is the same claim made at the instant the LEVEL-1 ordering reads it.
       IT IS NOT `flow_host_owed_count() == flow_count()`, which is what this line said first and which is a
       TAUTOLOGY: the pick's only filter IS the mark, so an empty pick already means every member is marked and
       the comparison could not fail for any input. The mark is a claim ABOUT THE HOST, and the failure worth
       catching is a mark that outlived the thing it rested on — a register whose every entry has been answered,
       which no host event can ever clear again. flow_set_host_owed asserts that per flow at the instant a mark
       is made; nothing asserted it of the FRONTIER at the instant the mark becomes an answer the host ranks on,
       and a `-inf` that means "nobody can ever wake me" is indistinguishable from one that means "pay me". */
    DCHECK(b != NULL || flow_count() == 0 || engine_host_owes() || g_referenced,
           "this engine reported -inf — it can hand the thread to nobody — while the host owes it NOTHING and "
           "no peer holds a reference into it: every member's mark rests on a register that has already been "
           "answered, so no host event can clear one, the Level-1 order will rank this document last forever "
           "and every timeline it holds is unexplored with nothing naming it");
    return b ? flow_weight(b) : -1.0 / 0.0;
}

#if APICLIENT_DEV
/* A PRINTABLE, BOUNDED EXCERPT of page bytes for a diagnostic. The source is unbounded — a payload or a
   script body is whatever the page shipped — so a message quotes an excerpt, and an excerpt that stopped SAYS
   so: bytes that simply end read as the whole of them, which is a different claim from the true one. Walked by
   LENGTH rather than to a terminator, because ECMAScript §11.1 "Source Text" permits a U+0000 in a program and
   a body that carries one would otherwise be quoted as the empty string. */
static void excerpt(char *out, size_t cap, const char *src, size_t n)
{
    size_t i;

    for (i = 0; i + 1 < cap && i < n && src != NULL; i++)
        out[i] = (src[i] >= 0x20 && src[i] < 0x7f) ? src[i] : '.';
    out[i] = '\0';
    if (src != NULL && i < n) {
        const char more[] = "…";
        size_t at = i + sizeof more <= cap ? i : cap - sizeof more;

        memcpy(out + at, more, sizeof more);
    }
}
#endif

static int engine_sched_slice(void) {
    JSContext *ctx = g_sess_ctx;
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
       the PARK that writes the residue to the cold tier. A wrapper that folded DONE into YIELD would send the
       host straight back in here with nothing live, and the abort would read as a bug in whatever the park had
       just done rather than in the fold. No layer folds anything now — the ABI entry used to fold STALLED into
       YIELD "because the bridge speaks two values", and that cost a driver 10.8 million no-op steps against a
       peer that had said it was owed a reply. So every code is propagated unchanged by every layer above this
       (engine_sched_step brackets
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
        /* THE QUESTIONS THIS INSTANCE WAS ASKED AND DID NOT START ARE HANDED BACK FIRST, before the residue is
           written, because they are not this instance's work to save: a token is the ZONE's name for a flow
           suspended in ANOTHER instance, it carries no generation, and it does not outlive the zone session —
           so it may never enter a recipe (engine.h). Returning them here is what lets cold_park's assert keep
           naming the one shape that genuinely has no recipe yet: an operation already STARTED. */
        engine_retract_operations(ctx);
        cold_park();
        /* AND THE FOREIGN SEGMENTS THAT RESIDUE CARRIES, announced before the deaths below because they are the
           opposite statement and the zone acts on them in that order: these worlds belong to PEERS and outlive
           this session in the park document, so the zone has to hold their deaths for the instance that
           resumes this document; the ones below are OURS and end here. */
        {
            const char *const *carried;
            int n_carried = world_segments_park(&carried);
            engine_notify_worlds_parked(ctx, carried, n_carried);
        }
        /* AND EVERY WORLD THIS SESSION EVER PUT ON THE WIRE IS DEAD TO ITS PEERS — announced HERE, between the
           park and the close, because this is the last point at which a notice of ours is still drained. The
           frontier is recipes from the line above and a resumed session mints in a disjoint generation
           (world.h), so no name this session sent will ever be used again, while the peer that never left
           memory still holds a segment for each of them. The flows themselves are released by the teardown the
           host takes after this returns, and they find nothing left to announce. */
        {
            const char *const *gone;
            int n_gone = world_session_gone(&gone);
            engine_notify_worlds_gone(ctx, gone, n_gone);
        }
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
       inside flow_deliver_one_reply). */
    for (;;) {
        /* THE SEARCH'S OWN WORK ITEMS, CREATED WHERE THEY COME INTO EXISTENCE. §@S: an @S candidate re-fire is a
           FLOW on this ONE frontier, so a detected sink becomes members of the frontier the way a fork does —
           when it happens.
           IT WAS ASKED ONLY WHERE THE FRONTIER HAD DRAINED, and that gate is a claim §NO BOUNDS forbids: a
           frontier need never drain. MEASURED on this tree's own minimal document, whose opaque-length walk
           makes every length a world — fifteen minutes, 4813 flows, three @S sinks in the document and
           `candidates: 0`, so every @S verdict it can state was unreachable by construction and nothing said so.
           IT IS IDEMPOTENT BY CURSOR (solve.h), so the ask costs one walk of the detected-sink list and adds
           only what detection has appended since the last one. At the PICK rather than once per slice, because
           that is what makes the exhausted answer below honest: a sink detected by the LAST runnable flow is
           seeded before the frontier can be declared empty, so there is no seeding site left below and no
           second one to keep in step.
           A BLOCKED FLOW'S REPLY IS NOT DEFERRED BY THIS, which was the whole of the old ordering's argument.
           Its premise is gone: every driver pays the host at EVERY slice (run_scheduler below, and main.c's
           qjs_step at the ABI) rather than only at a stall, so a flow suspended inside a read is answered at the
           next slice whatever else the frontier has to run. */
        solve_seed_candidates(ctx);
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
            /* THE SCHEDULER'S OWN PICK IS BEHIND THIS LINE — solve_seed_candidates, flow_next_to_run, the
               context switch and solve_flow_begin have all run, and the flow is switched in but has not
               executed an instruction. `detail` is the switch count, which says whether this iteration
               performed one: a stray completion with the count unchanged is the seeding or the pick, and one
               with it just incremented is the swap. */
            ENGINE_NO_STRAY(ctx, "pre-step: the scheduler's pick, the context switch and the delta swap",
                            g_switches);
            int r = flow_step(ctx, cur);
            engine_reclaim_set(prev_reclaim);
            ENGINE_NO_STRAY(ctx, "post-step: the arm flow_step just ran", r);
            /* HTML §8.1.7.3's END OF A MICROTASK CHECKPOINT. The flow has run a unit of work — a script, a
               microtask, a task — and if it holds no microtask the queue has drained, which is the moment
               HTML runs the steps other standards register there. It is asked HERE rather than inside
               flow_run_one_job because a SCRIPT that queued nothing at all still ends in a checkpoint, and
               that path never reaches the job pump. Running it more often than a browser would costs nothing
               and changes nothing: every consumer's steps are idempotent by their own definition (Indexed
               Database §2.7.1 empties its set and then answers false), which is the same property that lets
               HTML itself re-enter the checkpoint from clean-up-after-running-script.
               A FINISHED FLOW IS NOT ASKED, and that is not a hole: the steps registered here QUEUE work (a
               deactivated transaction with an empty request list commits, which is a database task), and a
               task enqueued on a flow that has finished is a dropped work item — §scheduler's razor. Nothing
               is left behind either, because the flow's own COW delta carries every transaction it created
               and unapplies with it.
               AND A FLOW SUSPENDED INSIDE A PROGRAM IS NOT ASKED EITHER — `!cur->frame` — because "the flow has
               run a unit of work" was the claim above and a PREEMPT is not the end of one: flow_step returns on
               a back-edge preempt too. HTML §8.1.4.4 Calling scripts states the precondition exactly — clean up
               after running script performs the checkpoint only "if the JavaScript execution context stack is
               now EMPTY" — and `Flow::frame` IS that stack here ("the current script's live preemptible frame,
               NULL between scripts", solver/flow.h). Without it the checkpoint ran at EVERY back-edge, and its
               one registered consumer is HTML §8.1.7.3 Processing model's "Cleanup Indexed Database
               transactions" step, whose steps are STATE TRANSITIONS and not observation: deactivate an active
               transaction, and commit it when its request list is empty. So `db.transaction('s','readwrite')`
               was deactivated and committed under the very statement that created it, and a member that had
               already made §4.5's own "is the transaction active" check resumed to place its request against a
               transaction that was no longer active — which is what core/indexeddb/idb_object_store.c's
               os_active_across_suspension says in its own words and aborts on. The idempotence argument above
               is untouched: running these steps MORE often than a browser is free, running them EARLIER is not.
               THAT STACK HAS TWO REPRESENTATIONS HERE AND `frame` IS ONLY ONE OF THEM — which is why this
               guard was necessary and NOT sufficient, and the assert above went on firing after it landed.
               `Flow::frame` is a page SCRIPT's suspended frame chain; a flow that preempts inside JOB-DRIVEN
               code parks instead (quickjs.h's pump: "a flow that preempts inside job-driven code parks"
               there), and `frame` is NULL for the whole of that. Every Indexed Database member that matters
               runs inside a job — `upgradeneeded`, `success`, `abort` are all event handlers — so the script
               half of this test never covered them. And the return above is what exposes it: resuming a parked
               continuation returns from here immediately, and its own comment says the continuation may park
               AGAIN straight away, so this line was reached with a member suspended between §4.5's "is the
               transaction active" check and the step that places its request. `JS_HasParkedFlow` is the other
               half of the same sentence, asked of the runtime because the flow is switched IN here and the
               queue is its own (JS_TakeParkedFlows/JS_PutParkedFlows carry it across a switch). */
            if (g_checkpoint_hook && r != FLOW_STEP_DONE && !cur->frame &&
                !JS_HasParkedFlow(JS_GetRuntime(ctx)) && !flow_job_microtask(cur))
                g_checkpoint_hook(ctx);
            /* THE CHARGE, AND IT IS CHARGED TO THE FLOW THAT RAN. `flow_age_running` bills whoever the registry
               says is running, and that is only the flow this step advanced while nothing between the switch-in
               above and here has changed it — a step that returned with a different flow running would bill the
               wrong one, and the symptom would be a monopolizer that never ages and a sibling demoted for time
               it never spent. The finish path clears it, which is why this stands BEFORE the r == DONE branch. */
            DCHECK(flow_running() == cur,
                   "the flow the scheduler stepped is not the one it is about to charge for that step's thread "
                   "time — the aging term would demote a flow that did not burn it and leave the one that did "
                   "holding its rank");
            /* WHAT THE ORDER SAID ABOUT THIS FLOW BEFORE IT WAS CHARGED — held for the resolution assertion
               below, and only in a build that evaluates it. It is `#if`-guarded WITH its assertion rather than
               declared unconditionally, because check.h's release DCHECK still TYPE-CHECKS its condition
               (`(void)sizeof(cond)`) and would therefore make an unguarded reading a per-step call in a build
               that reads nothing. The seam census above is guarded the same way for the same reason. */
#if APICLIENT_DEV
            int64_t age_notch0 = flow_silence_notch(cur);
#endif
            now = quantum_thread_us();
            flow_age_running(now - t0);
#if APICLIENT_DEV
            /* §scheduler'S MONOPOLIZER HALF, ASSERTED AT THE SEAM WHERE THE CHARGE MEETS THE PICK — and it is
               the one claim in the WFQ that nothing was checking. Its twin — "a UCB optimism bonus ∝
               1/(visits+1) so a NEVER-RUN FLOW IS NEVER STARVED" — is a floor on the weight flow_pick returns.
               The other half — "CPU-AGING so a monopolizer that burns CPU without emitting sinks below
               productive+unrun flows" — has a bound in flow_pick too, but that bound is DERIVED from
               flow_weight, so it can only catch an EDIT to the formula and says nothing about whether the term
               it is derived from can be OBSERVED. This is the observability, and it is a different claim: not
               how far a flow sinks, but whether consuming a slice of the thread moves its rank AT ALL at the
               granularity the thread is handed out in. The next statement of this loop is the pick.
               IT IS AN IDENTITY UNDER TODAY'S UNIT AND IT FIRES ON THE ONE IT REPLACES. `flow_silence_notch`
               divides by the cooperative quantum and the charge adds the same microseconds to BOTH of its
               summands, so a full quantum moves it by at least two notches. That doubling is also why the unit
               it replaces was 41.7 quanta of CPU rather than the 83.3 quanta of SILENCE a whole second names:
               with it, a full quantum moved this by ZERO, forty picks out of every forty-one, and the
               only term left with finer resolution was the optimism bonus, whose sole mover is COMPLETING a
               unit of work — which is also the sole precondition for reaching a queued job (`frame == NULL`,
               the predicate three lines below and the predicate every job arm of flow_step is under). So the
               one flow that could run a promise reaction was the only flow paying anything between two whole
               seconds. Measured on one login page, three runs, a fresh browser each: `jobsQueued` 373/401/417
               and `jobsRun` 14/14/14 — an integral, document-determined number with no network in it.
               A CHARGE UNDER ONE QUANTUM IS EXEMPT because it is below the resolution of anything this
               scheduler decides: a step that ended early is not a slice consumed, and the charges TELESCOPE
               across the quantum (the reading above is carried), so nothing is lost by not asserting on one. */
            DCHECK(now - t0 < (int64_t)ENGINE_QUANTUM_MS * 1000 || flow_silence_notch(cur) > age_notch0,
                   "a flow consumed a whole COOPERATIVE QUANTUM of the thread and its rank did not move — the "
                   "aging term is quantised coarser than the granularity at which the thread is handed over, "
                   "so the pick that immediately follows this charge is made against a weight this charge did "
                   "not touch, and a monopolizer is invisible to the ordering for as many picks as that unit "
                   "is quanta wide");
#endif
            /* …AND THE OTHER TERM'S CHARGE, WHICH IS A COUNT AND NOT A CLOCK. §scheduler's optimism bonus is
               "∝ 1/(visits+1)", and a VISIT is a completed unit of work — a program that ended, a job that ran,
               a delivery, a lifecycle stage — never a slice of thread time. The two are charged here together
               and stay two calls because they measure two quantities: the line above bills every microsecond,
               this one bills only a step that finished what it was doing.
               THE PREDICATE IS HTML §8.1.4.4 "Calling scripts"'s, step 3 of clean up after running script: "if
               the JavaScript execution context stack is now empty". `Flow::frame` is that stack for a page
               script and the runtime's parked slot is the other half of it (solver/flow.h, and the checkpoint
               hook three lines above asks the identical pair for the identical reason), so a flow preempted in
               the middle of a program is NOT credited — it is the same trial, still running. That is the whole
               of the fix: charged per quantum instead, a flow was strictly outranked by every arm it had forked
               as soon as it crossed one, so no flow ever reached the end of a program, and flow_step can only
               reach a flow's queued jobs with `frame == NULL`. 217 jobs queued and none run, on a page whose
               fetch surface is entirely promise reactions.
               A FINISHED FLOW IS CREDITED TOO and that is deliberate: `r == FLOW_STEP_DONE` is the largest
               completed unit there is, and the flow is about to leave the frontier, so the count is read only
               by the census that reports what the order was made of. Excluding it would mean the ONE step whose
               completion is certain is the one step not counted as one. */
            /* AND THE INSTANCE-WIDE COUNT OF THE SAME EVENT, taken HERE so it cannot drift from the credit it
               reports: `_unitsDone` beside `_jobsRun` is what tells "flows reach the between-units boundary and
               have no jobs" from "no flow has ever reached it", which is the pair engine.c's declaration of
               g_units_done argues for. It is not a second predicate — it is this one, counted. */
            /* AND THE THIRD HALF OF §8.1.4.4'S BOUNDARY, WHICH THE CHECKPOINT HOOK THREE LINES ABOVE WAS
               ALREADY ASKING AND THIS WAS NOT. Both lines claim to answer "is this flow's turn over"; the hook
               asked `!flow_job_microtask(cur)` and the credit asked a strictly weaker predicate, so two
               readings of one spec sentence stood one screen apart and disagreed. Step 3 of clean up after
               running script performs a microtask checkpoint when the execution context stack empties — the
               emptying is the checkpoint's TRIGGER, and the checkpoint runs "While the event loop's microtask
               queue is not empty", so the unit ends when the queue does. Crediting at the return DEMOTED the
               flow (the optimism term is 1/(1+visits)) at the one instant its queued reactions became eligible,
               and every job arm of flow_step is under this same `frame == NULL`: the flow then had to win the
               whole frontier again to run its own checkpoint. flow_credit_visit asserts it at the origin so no
               future credit site can reintroduce it; this is the predicate that satisfies the assert. */
            if (!cur->frame && !JS_HasParkedFlow(JS_GetRuntime(ctx)) && !flow_job_microtask(cur)) {
                flow_credit_visit(cur); g_units_done++; }
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
                    /* SIZED FOR THE WHOLE LINE, and 256 was not: the fixed text alone is over 200 bytes before
                       quantum_measure()'s 54, so `unit=` and `script_i=` — the two fields that say WHERE — were
                       the first things off the end. */
                    DFAILF("%d ms of THREAD CPU consumed in one step with NO suspend point offered (measure: "
                           "%s; %ld units of work, which is why the work verdict cannot see this one) — this "
                           "stretch has no suspend/resume seam; unit=%s script_i=%d",
                           (int)((now - t0) / 1000), quantum_measure(), work, g_step_unit,
                           cur ? cur->script_i : -1);
                }
                if (work > ENGINE_SEAMLESS_WORK && g_preempt_asked == pa0) {
                    /* THE TWO EXCERPTS ARE THE ONLY BOUNDED THINGS HERE, and they are bounded because their
                       SOURCES are unbounded — a payload and a script body are page bytes of any length, so a
                       diagnostic quotes an excerpt and says it is one. Everything else on this line is the
                       assert's message and is sized by check.h, which is what deleted the clamp that used to
                       stand here: `wi` took snprintf's return — the length it WOULD have written — so a
                       truncated prefix left an index past the end and the body append computed `why + 900`
                       with `sizeof why - 900` underflowing to ~2^64, writing the terminator outside a 640-byte
                       stack buffer. The diagnostic that names a missing seam smashed the stack at the moment
                       it named it. There is no index to get wrong now. */
                    char pl_txt[512], body_txt[1024];
                    const char *sk = cur && cur->cand_sink ? cur->cand_sink : "(exploration flow)";
                    const char *pl = cur ? cur->cand_payload : NULL;
                    /* WHICH PROGRAM. "a resume ran 5s" is still a symptom until the JS it was running is named,
                       and the flow already knows: script_i indexes the ONE sequence — the document's scripts and
                       the flow's own dynamic bodies (a lazy chunk, an injected <script>, a fired PoC) are rows of
                       the same table (solver/flow.h). Without this the only way
                       left to find the code is bisecting the fixture by hand, which is the thing this assertion
                       exists to replace. */
                    const char *bodytxt = NULL;
                    size_t bodyn = 0;
                    int si = cur ? cur->script_i : -1;
                    if (cur && si >= 0 && si < cur->dyn_n) {
                        bodytxt = dyn_body_text(cur->dyn[si]);
                        bodyn = dyn_body_len(cur->dyn[si]);
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
                    /* BOTH EXCERPTS ARE WALKED BY LENGTH, NOT TO A TERMINATOR — this diagnostic exists to NAME
                       the program a stuck flow is running, and a bundle carrying a U+0000 (ECMAScript §11.1
                       "Source Text" permits one) would have named the empty string or a prefix of itself. The
                       byte filter replaces every non-printable with '.', so a NUL prints as one character like
                       every other control. It is NOT for the record's sake — check.h escapes what it emits —
                       it is so a reader gets one line of source text instead of a run of escapes. */
                    excerpt(pl_txt, sizeof pl_txt, pl, pl ? strlen(pl) : 0);
                    excerpt(body_txt, sizeof body_txt, bodytxt, bodyn);
                    DFAILF("%ld units of work (forks+flows+jobs) with NO suspend point offered "
                           "(wall gap %d ms, step ran %d ms — reported, NOT the verdict; the slice's own "
                           "measure here is %s; points asked=%llu, preempts wanted=%llu fired=%llu; slowest "
                           "Web API member step: %s %dms of %dms over %ld member steps; wrapper map "
                           "%ld/%ld; live objects %lld, heap %lld KiB) — this stretch has no "
                           "suspend/resume seam; unit=%s script_i=%d "
                           "flow=%s payload=%s body=%s",
                           work, (int)gap, (int)spent, quantum_measure(),
                           (unsigned long long)(g_preempt_asked - pa0),
                           (unsigned long long)(pq - pq0),
                           (unsigned long long)(pf - pf0), slow_name, (int)slow_ms,
                           (int)steps_ms, steps_n, wrap_n, wrap_cap,
                           (long long)mem.obj_count, (long long)(mem.malloc_size / 1024),
                           g_step_unit, si, sk, pl_txt, body_txt);
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
                /* AND THE MARK IS A CLAIM ABOUT THE HOST, ASSERTED WHERE IT IS MADE. A marked flow leaves the
                   pick until a HOST EVENT clears it, so the mark is only ever true if there is something the
                   host has actually been shown and can still answer: an entry on this flow's register with no
                   value — which is in engine_pending_fetches or engine_host_requests by construction, since both
                   walk every flow's register and select exactly the unanswered — or the one case with no entry
                   at all, a document a peer holds a reference into. Anything else is a flow that has left the
                   run queue for good, and NOTHING would say so: `live` still counts it, `blocked` and `owed`
                   still report it as waiting, and the whole timeline behind it simply never runs again.
                   THE FRONTIER-WIDE FORM OF THIS EXISTS ALREADY AND CANNOT SEE IT (the stall claim below).
                   That one is checked only when the pick finds NO runnable member, so a single permanently
                   marked flow is invisible for as long as its siblings keep the loop busy — which is exactly
                   the state in which a document stops getting deeper while every number about it looks
                   healthy. It is also the STRICTER predicate: `pending_count > 0` is satisfied by a register
                   whose every entry has already been ANSWERED, and a flow stuck on one of those is precisely
                   the shape no host event can ever clear. Asked per flow, at the instant the mark is made. */
                DCHECK(pending_outstanding(cur->pending) || g_referenced,
                       "a flow was marked host-owed while the host owes it NOTHING — every entry on its "
                       "register has already been answered, so no host event can clear this mark and the flow "
                       "is out of the pick for the rest of the session with its whole timeline unexplored");
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
        /* VALUE: a better DOCUMENT is waiting — same lossless yield. A FINITE weight only, and that is the
           whole of the difference between the two answers this engine can give a host. engine_top_weight is
           now the RUNNABLE pick, so -inf means this frontier can hand the thread to nobody at all — which is
           not "the other document is worth more", it is the STALL, and the host has to be told which because
           they ask for opposite things: a yield asks to be outranked, a stall asks to be PAID. The stall is one
           iteration away (the pick at the top of the loop returns NULL and breaks to it), so the guard costs a
           comparison and keeps the two verdicts from collapsing into the weaker one. */
        {
            double top = engine_top_weight();
            if (top > -1.0 / 0.0 && top < g_yield_floor) {
                g_sess_cur = cur;
                return ENGINE_STEP_YIELD;
            }
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
        /* OUTSTANDING, NOT MERELY PRESENT — the same correction the per-flow assert at the mark carries, and
           the reason it is a correction rather than a tightening: `pending_count > 0` is true of a register
           whose every entry has already been ANSWERED, and that is not a member waiting on the host, it is a
           member nothing can wake. Counted, it read as a healthy stall; asked this way it names itself. */
        DCHECK(pending_outstanding(flow_at(i)->pending) || g_referenced,
               "the frontier reported a STALL while one of its members owed the host NOTHING — its mark says it "
               "cannot progress and the host owes it nothing, so it was marked while it still had work to do "
               "and the exploration of that timeline stops here for no reason at all");
    }
    /* STALLED, not exhausted: the run-queue is empty but flows are parked on something only the host can
       supply. Asked BEFORE closing — the session and every parked snapshot stay live, and the host steps again
       once it has provided. It is the ENGINE's own question over the ENGINE's own registers now; it used to be
       a host callback, and the shipped host answered half of it. See engine_host_owes. */
    if (engine_host_owes())
        return ENGINE_STEP_STALLED;
    /* NO SEEDING HERE, AND THE FRONTIER IS NOT ASKED WHETHER IT IS BLOCKED. Both used to stand at this line —
       the @S search was seeded exactly where the frontier had drained, guarded by an assert that no member was
       parked on the host — and the whole of that is now one ask at the PICK above, where a candidate becomes a
       member the moment its sink exists. What stood here could only ever have run for a document that DRAINS,
       which §NO BOUNDS says need never happen. */
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
        DCHECK(flow_job_pending(flow_at(i)) == 0,
               "the frontier was declared exhausted while a live flow still held queued jobs — its promise "
               "reactions, timer callbacks and delivered messages die with the session");
        /* AND THE SAME RULE FOR A ROUTED RECORD, which is a work item exactly as a job is. flow_finish asserts
           it for the flow that RUNS OUT of work; a flow that leaves this loop alive (every member host-owed, so
           none is pickable) had nothing checking it, and the record then dies in flow_registry_free — the
           peer's message, dropped, indistinguishable from a page that registered no handler. A flow suspended
           inside a live frame is the shape that reaches here holding one: the delivery is made only where
           flow_step has no frame, so if this fires, the enqueue belongs earlier than that branch. */
        DCHECK(!flow_owes_answer(flow_at(i)),
               "the frontier was declared exhausted while a live flow still owed a peer the answer to a "
               "cross-agent operation — the asking flow, in another instance, is suspended at the line that "
               "asked and this session is about to end without ever telling it anything");
        DCHECK(flow_deliver_pending(flow_at(i)) == 0,
               "the frontier was declared exhausted while a live flow still held routed records — a peer's "
               "message this document never received, dropped with the session");
    }
    /* AND THE CONSERVATION LAW OVER THE DELIVERIES THAT DID BECOME TASKS. The four asserts above are each about
       ONE queue at ONE moment; this is the only line that can say the queued §9.3.3 tasks all reached an end,
       because a task's end is a fact about a run and not about a queue. flow_deliver asserts the delivery
       became exactly one task (§9.3.3 step 8 queues one global task, singular); nothing asserted that the task
       RUNS, and a task that never ran leaves the page unable to tell a message it did not get from one that was
       never sent. The four ends are engine.h's; the inequality is its too — a fork gives the arm its own Array
       naming the parent's job records, so a timeline that branches between the enqueue and the run delivers in
       both arms and the sum legitimately exceeds the count of tasks queued. Below it, a delivery vanished. */
    {
        long ends[ROUTED_TASK_END_N], sum = 0;

        engine_routed_task_census(ends);
        for (int i = 0; i < ROUTED_TASK_END_N; i++) sum += ends[i];
        DCHECK(sum >= g_routed_delivered,
               "the frontier was declared exhausted with FEWER §9.3.3 delivery-task ends than deliveries — a "
               "routed record became a task on a receiving timeline's queue (flow_deliver asserts exactly one) "
               "and that task never ran, so a peer's message was dropped by the SCHEDULER rather than reaching "
               "any of the three ends on which delivering nothing is correct (§9.3.3 step 8.1's targetOrigin "
               "check, a target Document §7.5.10 destroyed, and the task's own abrupt completion). It is the "
               "one outcome that is a defect: find the queue it is standing on and why that flow was never "
               "picked");
        (void)sum;
    }
    engine_session_close();
    return ENGINE_STEP_DONE;
}

/* THE OTHER END OF THE `begin`/`step` PAIR — see engine.h for why every host calls it unconditionally at the
   point it stops stepping, and for what is still live inside a session that was left open. There is nothing
   here beside the close: the whole of "the session is over" is already one function, and this is the seam that
   lets a host outside this file reach it rather than a second copy of its list. */
void engine_sched_end(void) {
    /* A SESSION THAT ALREADY CLOSED IS ALREADY ENDED. This is the answer to the question, not a guard against
       a caller who asked it wrongly: `engine_sched_step` closes the session itself when it answers DONE, and a
       host that stopped stepping BECAUSE of that DONE is in exactly the state this call establishes. */
    if (!g_sess_live) return;
    engine_session_close();
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
       quantum: the body has seven exits (the park's DONE, three yields, STALLED twice — a reply the host owes
       and a peer's question — and the exhausted path that closes the session), and a mark cleared at six of
       them would look correct forever. One bracket around one call cannot be got wrong. The enumeration is
       spelled out because it was WRONG while the count was right: it named a seeded-candidate yield that is
       gone (the seeding is at the pick now) and neither of the two exits that had been added since.
       AND IT IS SUSPEND/RESTORE, NEVER CLEAR/RE-ENTER. The generation is monotonic — every delta's fork_gen is
       a point on that one line — so the next slice resumes at the number this one left, and a restart at 1
       would make a later object compare as older than an earlier fork. */
    /* AND THE CAPTURE ROUTE IS THE THIRD, WHICH THE PARAGRAPH ABOVE LEFT OUT — the one that made the stamp line
       below not merely incomplete but the thing that ARMED the corruption.
       The three marks are one statement in three registers: the STAMP says what generation an object is born
       at, `g_dom_capture` says whether the Lexbor tree is being recorded, and the DELTA says WHOSE head a
       capture lands in. The DOM half turned its recording OFF for the host's time; the JS half turned its STAMP
       to 0 and left the route pointed at whichever flow the slice last switched in — and `cow_set_current` is
       never called between slices, because §scheduler requires the yield to keep the running flow switched in.
       So across the host's own time the capture hook was LIVE and aimed at a stranger's delta, and stamping the
       host's objects BASELINE is exactly what removed the only thing that had been skipping them: a baseline
       object is `flow_gen 0 <= d->fork_gen`, which is the SHARED arm of cow_capture_hook's test.
       WHAT THAT DID, and it is the whole of the `urlList` abort: every property the host wrote on an object it
       had just created was recorded as a CREATION (existed=0) in that flow's head, and the next context switch
       away from it ran cow_unapply_entries, which DELETES a creation. A reply record built by
       fetch_reply_new — or parsed by qjs_provide — therefore arrived at its delivery as `{}`: no urlList, no
       status, no headers, no body, with its refcount intact and nothing to say what had happened. The same
       sentence covers every other object the host builds between slices (qjs_host_answer's parsed answer,
       qjs_route's and qjs_perform's delivery records), which is the list the DCHECK in qjs_step already names.
       SUSPEND/RESTORE for the same reason the generation is: the slice left the route pointed at the flow it is
       still holding across the yield, and the next slice must resume with that flow's captures going where they
       went — clearing it would be a flow running with its writes recorded nowhere. */
    JS_SetFlowGen(g_slice_flow_gen);
    g_dom_capture = 1;
    cow_set_current(g_slice_delta);
    /* THE HOST'S OWN TIME IS BEHIND THIS LINE, and on the first slice so is everything the session setup did
       before any flow existed — the two cases the other three boundaries cannot see. Between two slices the
       host parses a fetch reply into a record, routes a delivery and writes a synchronous answer, all of which
       build JS values; a throw any of them leaves standing belongs to no flow at all, and the slice below would
       hand it to whichever flow it resumes first. `detail` is the session's live flag, which distinguishes
       "before the session was opened" from "between two of its slices". */
    ENGINE_NO_STRAY(g_sess_ctx, "slice-entry: the host's own time between slices (and the session setup before "
                                "the first one)", g_sess_live);
    r = engine_sched_slice();
    /* …AND THE SCHEDULER'S TAIL, which is the fourth segment: HTML §8.1.7.3's end-of-checkpoint hook, the aging
       charge and flow_finish all run after the post-step boundary inside the loop, so this is the only line
       that watches them. Asked BEFORE the marks below are put back, so what it reports is the state the slice
       actually ended in. */
    ENGINE_NO_STRAY(g_sess_ctx, "slice-exit: the scheduler's tail after the last step "
                                "(§8.1.7.3's end-of-checkpoint hook, the aging charge, flow_finish)", r);
    g_slice_flow_gen = JS_FlowGen();   /* the forks this slice took moved it; carry them, never restart */
    JS_SetFlowGen(0);                  /* the host's own time is baseline: what it creates is shared by construction */
    g_dom_capture = 0;
    /* …and it is recorded into NO delta, which is what makes the line above safe rather than merely tidy. The
       save reads whatever the slice left: the flow it is still holding across a yield, or NULL after a close —
       engine_session_close switches its last flow out, and flow_switch_out un-currents as it unapplies, which
       is the same reason that function's own clear of the live stamp is enough for the line above. */
    g_slice_delta = cow_current();
    cow_set_current(NULL);
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

/* …AND WHETHER THIS HOST WANTS THE FRONTIER'S RESIDUE — see engine.h. A seam, not a policy: the answer is the
   host's and the moment the park is TAKEN at is the engine's. */
static int (*g_park_hook)(void);
void engine_set_park_hook(int (*want_park)(void)) { g_park_hook = want_park; }

static void run_scheduler(JSContext *ctx, char **bodies, char **srcs, const ScriptType *types,
                          lxb_dom_element_t **els, int n, int forking, const char *recipes) {
    int next = ENGINE_PROGRESS_EVERY, last_cands = -1, r;
    /* THE RESIDUE THIS HOST WAS HANDED, or NULL. This line used to say the cold tier's resume "belongs to the
       host that has an IndexedDB", and that was a claim about STORAGE standing in for a claim about the
       SCHEDULER: seeding from a residue is engine_sched_begin's own alternative to seeding a boot flow, and a
       host with a file is as much a store as a host with an object store. Passing NULL from here made the
       resume path unreachable from every host but the extension's, which is exactly the shape §SECURITY.md
       names one level up — a mechanism that can be written, reviewed and self-tested with no process able to
       run it. The store is the caller's business; the choice is the scheduler's, and it is made in one place. */
    engine_sched_begin(ctx, bodies, srcs, types, els, n, forking, recipes);
    for (;;) {
        /* DOES THIS HOST WANT THE RESIDUE? Level-1 eviction is the HOST's decision — it is the only zone that
           can see the other documents' engines and the summed working set — so it is a SEAM here exactly as
           the stall payment is, and for the same reason: the scheduler holds no idea of what memory pressure
           is, and the host holds no idea of what a flow is. A host that never evicts installs nothing, which
           is every existing caller.
           IT TRUNCATES NOTHING, which is what keeps it out of §NO BOUNDS. The park WRITES every member of the
           frontier and the caller stores it, so what this seam decides is WHEN the residue leaves memory and
           never how much of it survives — which is precisely the difference between paging and a cap.
           ASKED AT THE TOP OF THE LOOP, so a host that answers yes gets the park taken at the next step
           boundary with no flow switched in, which is the only moment every flow's decision state is in its
           own blob rather than in decide.c's globals. */
        if (g_park_hook && g_park_hook())
            engine_request_park();
        r = engine_sched_step();
        if (r == ENGINE_STEP_DONE)
            break;
        /* THE HOST PAYS EVERY SLICE. It used to pay only at a STALL, and that was recorded here as a known
           defect with the fix written out in order; this is that fix, and the reason it could not be taken
           before is one line in solver/pending.h that has now been corrected.
           WHAT THE OLD SCHEDULE COST, and it is the answer to why this document never got deeper than its
           second <script>. A stall is reached only when the pick finds NO runnable member, so under a stall-only
           seam a flow's reply is conditional on EVERY OTHER FLOW in the document also becoming blocked. One
           flow's progress is then a function of 511 siblings' states — a cross-flow coupling this scheduler
           forbids everywhere else — and the coupling gets WORSE as exploration succeeds, because every fork adds
           a member that must also block before anybody is paid, and a fork RE-ISSUES its parent's unanswered
           synchronous request under a fresh id (engine_sibling_assemble), so the set to be blocked on is
           regenerated by the very thing that makes progress. The leading flow — the one furthest into the
           document, the only one that can reach a lazily-loaded chunk — is the flow this hurts most: it is the
           first to block on the host and the last to be answered.
           THE PRODUCT NEVER HAD IT, which is why this host was the odd one out rather than the canary. The
           extension's bridge pulls qjs_pending and qjs_host_requests after EVERY non-DONE return — a stall and
           an ordinary quantum are paid on the same schedule there, which is why main.c carrying the stall code
           separately changes what the host KNOWS and not when it pays — so in the extension a blocked flow is
           answered at the next quantum. This driver now speaks the same schedule,
           which is also what makes it a fair oracle for the product: a difference in findings between the two
           hosts should be a difference in the ENGINE, and a payment schedule is not one.
           WHY IT ABORTED WHEN IT WAS TRIED, and it was neither the provider nor the reply record. `pending_ready`
           answered YES for an ANSWERED HOSTREQ, so a synchronous answer arriving between two slices made the
           register look deliverable, flow_step called the reply delivery, and it swap-removed the rendezvous
           record and pushed it through a `resolve` capability it does not have. At a stall that state cannot
           exist, because the asking machine consumes its answer on the very next step; at a quantum boundary
           nothing guarantees the asking machine runs next. Both halves are fixed at the root — the predicate
           asks the KIND (solver/pending.h) and the delivery LEAVES a synchronous answer where its machine will
           take it (flow_deliver_one_reply) — so the shape this branch was avoiding no longer exists to be
           avoided. */
        {
            /* THE HOST'S OWN TIME, ASSERTED WHERE IT BEGINS — the same claim qjs_step makes at the ABI boundary,
               made here because this driver has no ABI and pays the provider directly. A provider BUILDS
               objects (a reply record, a peer's answer) and the three marks that decide what happens to those
               objects belong to the flow that just ran, not to the host: with the stamp up they are stamped as
               a flow's and no later write to them is ever captured, and with the capture ROUTE up every field
               the provider writes is recorded as a creation in that flow's head and DELETED by its next
               unapply — which is how a reply record reached its delivery as `{}`. Two registers, one fact, and
               the failure mode of each is silent in the opposite direction. */
            DCHECK(JS_FlowGen() == 0 && cow_current() == NULL,
                   "the host is being paid with the last flow's generation or its capture route still up — "
                   "every object the provider now builds either belongs to that flow (so nobody else's write "
                   "to it is ever captured) or has its every field recorded as a CREATION in that flow's head, "
                   "which the next context switch deletes: the record arrives at its delivery empty");
            int filled = g_provider ? g_provider(ctx) : 0;
            /* AND THE PROVIDER IS ITS OWN SEGMENT, which is the split the slice-entry boundary's own report
               asked for. That boundary watches the WHOLE of the host's time as one region, and the host's time
               has two halves that fail in different places: the PROVIDER, which builds JS values (a reply
               record, a peer's answer, a cross-agent operation's text), and everything else this loop does
               between two steps — the census lines, the progress emission, the cold-tier preview — which
               reads. A completion standing here belongs to the first; one that reaches slice-entry with this
               silent belongs to the second, and there is no third half. `detail` is what the payment filled,
               so a leak on a payment that answered nothing reads differently from one that answered records. */
            ENGINE_NO_STRAY(ctx, "host-provider: the host's reply/answer/operation payment", filled);
            /* AND THE PAYMENT WAS COMPLETE, asserted at the seam instead of inferred from a census line six
               minutes later. This provider answers out of its OWN tables — a reply record it builds, a peer's
               answer it stands in for — so unlike the extension's it has no asynchronous half and nothing it
               may legitimately still owe once it has run. A record outstanding here is therefore one it was
               never handed or one it walked past. `blocked` and `owed` cannot say which: they report the LEVEL
               of unanswered work, and a frontier that keeps issuing requests reads identically whether the host
               is paying promptly or has silently skipped a record. This says which, at the moment it happens,
               and it names the two registers separately because they are two questions.
               UNCONDITIONAL NOW, rather than inside the stall branch: the claim is about what the provider
               leaves behind and is equally true of a slice at which nothing was outstanding (both registers are
               empty either way), so guarding it by the stall would only have narrowed where it can fire. */
            if (g_provider) {
                DCHECK(*engine_host_requests() == '\0',
                       "the smoke host paid and a SYNCHRONOUS request is still outstanding — this provider "
                       "answers every record it is handed out of its own tables, so the flow blocked on this "
                       "one is parked at a call site nothing is going to resume, and its whole timeline is lost "
                       "with nothing but a `blocked` count to say which record it was");
                DCHECK(*engine_pending_fetches() == '\0',
                       "the smoke host paid and a reply is still owed — the same silence one register over: the "
                       "flow that issued this fetch keeps its snapshot and its continuation and is never handed "
                       "the body, so everything the page does behind that reply is missing from the run");
            }
            /* NOBODY CAN SUPPLY IT, so this driver stops driving — and STOPPING IS NOT THE SAME AS THE SESSION
               ENDING, which is why the close is below rather than here. The frontier that remains is real
               (every flow parked on a reply that never came), and it is handed to the teardown as SNAPSHOTS.
               The condition is unchanged: a STALL the payment could not move. A stall with nothing filled is a
               frontier waiting on something outside this host's tables — in the smoke driver, the referenced-
               document case, which no reply can answer. */
            if (r == ENGINE_STEP_STALLED && filled == 0)
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
                ColdResumed resumed;
                cold_census(&c);
                cold_resumed(&resumed);
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
                /* AND `completed` BESIDE `deepest`, because one number was carrying two facts and the readings
                   of it disagreed — see g_completed. `deepest` is the highest program STARTED; this is the
                   highest program this document has ever run to its END. */
                /* AND `orphans` BESIDE THE THREE OF THEM, because they are all coverage facts about the
                   DOCUMENT and this is the one that says how much of its code was reached by nothing but
                   forced invocation. `deepest`/`completed` measure the sequence the page arranged; this counts
                   the functions the page shipped and never called, each of which became a flow of this
                   frontier. A page with a large bundle and a zero here is one whose uncalled surface is not
                   being reached at all, which is the headline capability failing silently — see
                   engine_orphan_fork. */
                /* AND THE THREE ORPHAN-CLAIM ROWS BESIDE IT, because `orphans` counts what this session
                   STARTED and says nothing about what it INHERITED. A park writes an 'o' record per drive it is
                   holding; a resume rebuilds one flow per record, each waiting for a body the document's own
                   replay has to re-create. `orphanClaims` is how many were rebuilt, `orphanClaimsMet` how many
                   waits a take satisfied, and `orphanClaimsUnmet` how many waiting flows FINISHED never having
                   been handed one.
                   THE LAST IS THE VERDICT AND THE FIRST TWO ARE CONTEXT. Met can legitimately EXCEED the
                   records, because a waiting drive forks arms while it replays and every arm of it is the same
                   drive of the same body; so met-minus-claims is not a loss and must not be read as one.
                   Unmet is the loss, exactly: on a document whose bytes did not change between two sessions it
                   is ZERO, and a resumed frontier whose most expensive members drive nothing is otherwise
                   indistinguishable from one that worked. */
                /* AND `hostAnswersLate`/`pagedReqs` BESIDE THEM, because a REFUSAL nobody can see is a drop.
                   The first counts the answers that arrived after this session closed and were refused rather
                   than written onto a flow that can never run again; the second is how many synchronous
                   requests this session's sales took with them. Both are what a reader standing at
                   engine_host_answer's remaining abort needs in order to tell which door the asking flow left
                   by, and neither decides anything. */
                printf("@COLD {\"flows\":%ld,\"framed\":%ld,\"blocked\":%ld,\"owed\":%d,"
                       "\"finished\":%ld,\"deepest\":%d,\"completed\":%d,\"orphans\":%ld,"
                       "\"orphanClaims\":%ld,\"orphanClaimsMet\":%ld,\"orphanClaimsUnmet\":%ld,"
                       "\"hostAsked\":%ld,\"hostAnswered\":%ld,\"hostAnswersLate\":%ld,\"pagedReqs\":%ld,"
                       "\"decEntries\":%ld,\"decKiB\":%ld,\"headEntries\":%ld,\"headKiB\":%ld,"
                       "\"domHeadEntries\":%ld,\"domHeadKiB\":%ld,\"jobs\":%ld,\"pend\":%ld,\"pendKiB\":%ld,"
                       "\"miscKiB\":%ld,\"perFlowKiB\":%ld,"
                       /* `dynKiB` MOVED FROM THE PER-FLOW HALF TO THE SHARED ONE and took its count with it —
                          a program's text is one buffer however many timelines hold that program
                          (solver/dyn_body.h), so it is priced like a frozen segment and not like a delta head.
                          Summed per flow it reported the sharing as if it did not exist, which is the one
                          thing this line's own comment says a pager must not do. */
                       "\"segKiB\":%ld,\"domSegKiB\":%ld,\"pinSegs\":%ld,\"pinSegEntries\":%ld,"
                       "\"pinSegKiB\":%ld,\"decSegs\":%ld,\"decSegEntries\":%ld,\"decSegKiB\":%ld,"
                       "\"dynBodies\":%ld,\"dynKiB\":%ld,"
                       "\"sharedKiB\":%ld,\"stepMachines\":%d}\n",
                       c.flows, c.framed, c.blocked, flow_host_owed_count(),
                       g_finished, g_deepest, g_completed, g_orphans_driven,
                       resumed.orphans, g_orphan_claims_met, g_orphan_claims_unmet,
                       g_host_asked, g_host_answered,
                       g_host_answers_late, g_paged_reqs,
                       c.dec_entries, c.dec_bytes / 1024, c.head_entries, c.head_bytes / 1024,
                       c.dom_head_entries, c.dom_head_bytes / 1024, c.job_count, c.pend_count,
                       c.pend_bytes / 1024, c.misc_bytes / 1024,
                       (c.dec_bytes + c.head_bytes + c.dom_head_bytes + c.pend_bytes +
                        c.misc_bytes) / 1024,
                       c.seg_bytes / 1024, c.dom_seg_bytes / 1024,
                       c.pin_seg_count, c.pin_seg_entries, c.pin_seg_bytes / 1024,
                       c.dec_seg_count, c.dec_seg_entries, c.dec_seg_bytes / 1024,
                       c.dyn_count, c.dyn_bytes / 1024,
                       (c.seg_bytes + c.dom_seg_bytes + c.pin_seg_bytes + c.dec_seg_bytes +
                        c.dyn_bytes) / 1024,
                       JS_StepMachineCount(JS_GetRuntime(g_sess_ctx)));
            }
            /* AND WHAT THE ORDER ITSELF IS MADE OF (solver/flow.h's WfqCensus). @PROGRESS says how much work is
               happening and @COLD says how much of it RETIRES, and a run in which both climb while the
               fixture's probe table stops advancing is one whose entire frontier is doing something that emits
               nothing — a state neither line can explain, because neither reads either term the pick is made
               of. THE READING IS `valMax - valMin` AGAINST 1.0, the optimism term's entire range: a spread
               wider than that is a frontier whose ends the bonus can no longer reorder, so the order is the
               reward's and the bottom waits on the aging term alone (one point per second of unproductive
               thread time, per member ahead of it). `valZero` names who is down there — a from-baseline flow
               enters at reward 0, which is every candidate session and every joined document's boot flow — and
               `selfEmit` says whether anything in the frontier has emitted since it was born at all. */
            {
                WfqCensus w;
                flow_wfq_census(&w);
                printf("@WFQ {\"members\":%ld,\"valMin\":%.1f,\"valMax\":%.1f,\"valTop\":%.1f,"
                       "\"valZero\":%ld,\"selfEmit\":%ld,\"unrun\":%ld,\"svcMax\":%lld,"
                       /* …AND THE FORK FAMILY'S OWN SERVICE BESIDE IT (solver/flow.h's svc_fam_max), which is
                          the one reading that says whether the REWARD SCALE is what a run is stuck on: `val`
                          is copied at every fork and the aging that cancels it is charged per arm, so
                          svcFamMax / svcMax is the factor by which this frontier over-charges itself for one
                          family's silence. It decides nothing — it is here so the next change to flow_weight
                          starts from a number instead of from this paragraph. */
                       "\"svcFamMax\":%lld,"
                       /* …AND THE OPTIMISM TERM'S OWN COORDINATE, WHICH IS THE ROW THIS LINE HAD NO ANALOGUE OF
                          AND NEEDED MOST. Everything else here is thread time; `visMax` is COMPLETED UNITS OF
                          WORK, and `visMax == 0` on a frontier of thousands says that no member has reached the
                          end of a program — so no queued job can have run, whatever `switches` and `forks` say.
                          That state is what a busy engine with an empty API surface looks like from inside, and
                          before this row nothing in any stream could distinguish it from healthy interleaving.
                          Read `visMin` beside it: equal to `visMax` is turns being handed round, far below it
                          is the order concentrating, and both at 0 is nothing finishing at all. */
                       "\"visMin\":%lld,\"visMax\":%lld,"
                       "\"cands\":%ld,\"candUnrun\":%ld,\"candSvcMax\":%lld,\"candDecMax\":%ld,"
                       "\"decMax\":%ld,\"wTop\":%.3f,\"wMin\":%.3f,\"candWMax\":%.3f}\n",
                       w.members, w.val_min, w.val_max, w.val_top,
                       w.val_zero, w.self_emit, w.unrun, (long long)w.svc_max,
                       (long long)w.svc_fam_max,
                       (long long)w.vis_min, (long long)w.vis_max,
                       w.cand_members, w.cand_unrun, (long long)w.cand_svc_max, w.cand_dec_max,
                       w.dec_max, w.w_top, w.w_min, w.cand_w_max);
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
       ways a session ends, and the way it ended here was the one nothing said.
       THE `r != ENGINE_STEP_DONE` THAT STOOD HERE IS GONE, and its absence is the point: it made "did this exit
       already close the session?" a question every host answers for itself, which is one hand-copied condition
       per driver and a §NO-STUBS-grade hole in whichever one gets it wrong. The wpt runner got it wrong the
       first time it grew a second exit, and 26 files aborted on flow_release. `engine_sched_end` answers it
       once, so a host's rule is now "call it when you stop stepping" with nothing to get right. */
    engine_sched_end();
}

/* EXPLORE: seed boot OR a parked residue, then drain the frontier, forking at every concolic branch. */
void engine_run(JSContext *ctx, char **bodies, char **srcs, const ScriptType *types,
                lxb_dom_element_t **els, int n, const char *recipes) {
    run_scheduler(ctx, bodies, srcs, types, els, n, 1, recipes);
}

/* See solver/engine.h — the solver half's release column, in reverse dependency order. */
void solver_agent_free(JSContext *ctx)
{
    DCHECK(ctx != NULL, "the solver's agent state was released against no realm — the frontier's flows hold "
                        "JSValues and its deltas hold the writes those flows made, so there is a realm they "
                        "belong to and a release with none would drop them against nothing");
    /* NOTHING OF THE BROWSER HALF IS STILL REGISTERED HERE, AND THAT IS AN ORDERING STATEMENT RATHER THAN A
       TIDINESS ONE. Each of these slots holds a C function pointer INTO a browser component, and every browser
       component was released by platform_agent_free, which runs BEFORE this call — so a slot still set is the
       scheduler holding a callback into state that has already been given back. That is exactly the defect
       core/agent_state.h records for Indexed Database §2.7.1's cleanup: found by reading, invisible to both of
       JS_FreeRuntime's censuses, because a stale handle gave its reference back and then kept the number.
       The claimant releases; this asserts that it did. */
    DCHECK(g_timer_hook == NULL,
           "§8.1.7's timer step was still registered when the solver's agent state was released — "
           "core/timing/timer.c claimed it and gives it back at timer_free, which is a row on "
           "core/platform.h's release column and therefore runs first");
    DCHECK(g_rendering_hook == NULL,
           "§8.1.7.3's in-parallel half was still registered when the solver's agent state was released — "
           "core/rendering/rendering.c claimed it and gives it back at rendering_free");
    DCHECK(g_checkpoint_hook == NULL,
           "the end-of-microtask-checkpoint step was still registered when the solver's agent state was "
           "released — core/indexeddb/idb_transaction.c claimed it and gives it back at idb_transaction_free");
    DCHECK(g_wrap_stats == NULL,
           "the wrapper-census hook was still registered when the solver's agent state was released — "
           "core/dom/node.c claimed it and gives it back at node_free, under the `element` row's cascade");
    DCHECK(g_docdone_hook == NULL,
           "§13.2.7's document-load lifecycle step was still registered when the solver's agent state was "
           "released — core/dom/document.c claimed it at document_init and gives it back at "
           "document_agent_free, which is a row on core/platform.h's release column and therefore runs first");
    /* THE @S SEARCHES GO BACK BEFORE THE FRONTIER, AND THE ORDER IS THE WHOLE OF WHY. A search takes a
       reference on the DECISION CHAIN at the moment its sink is detected — add_pending's decide_freeze_path,
       the re-injection point every later candidate is seeded from — and that reference is neither a flow's nor
       the running flow's globals'. flow_registry_free ends in decide_free, whose `g_dec_seg_live == 0` is a
       statement that every such reference has already been given back, and solve_free is the give-back: with
       it AFTER, any session that detected a sink while standing on a non-empty decision path aborted the
       renderer at teardown. That is every real page, because a page that has taken one branch before its first
       tainted sink is the ordinary case rather than the corner — measured on testing/corpus/control, which
       forks on `window.__FLAGS.admin` before its first markup sink and hit that assert on every run, while the
       same sink with no branch in front of it froze an EMPTY path, held no segment, and completed.
       IT IS THE SHAPE core/platform.h STATES AND THE ONE concolic_free's OWN ASSERT RELIES ON: the claimant
       releases at its own release, and the holder asserts that it did — so the fix is the position of the
       give-back, never a softer assert. Nothing in the frontier's teardown reads this file's store (flow_release
       does not call solve_flow_end; the scheduler does), so the claim can be given back before it. */
    solve_free();
    flow_registry_free(ctx);
    /* THE ORPHAN COUNTERS AND THE GENERATION THEY LAST WALKED AT, given back with the frontier they describe.
       The ordinal names an argument's source identity ({orphan7.arg0}) and the generation is a fact about ONE
       runtime's heap, so an agent that started a second session on top of the first would mint identities that
       collide with the previous session's constraints and would skip a walk over a heap that is not the one it
       walked. All are single words: the release is what makes them a session's rather than a process's.
       `g_orphan_asks` IS ON THIS LINE AND WAS NOT, which was a defect in the commit that introduced it rather
       than an omission with no consequence. The hosts that take a runtime down and bring another up per file
       run many sessions in one process, so a counter left standing reports the PROCESS's total under a name
       the result document spells per document — and this one in particular: the whole of what `asked` is for
       is that `asked == 0` means NO FLOW IN THIS SESSION ever reached the end of its own work. A carried-over
       count makes that read `asked > 0` for a session that never asked at all, which is the exact reading the
       pair exists to distinguish, inverted, in the direction that looks healthy. */
    g_orphans_driven = 0; g_orphan_asks = 0;
    g_orphan_gen_seen = 0; g_orphan_gen_valid = 0;
    /* …AND THE ROUND TRIP'S THREE NUMBERS BESIDE THEM, for the same sentence: what an inherited drive did with
       its recipe is a fact about ONE session's residue, and the latch is a fact about ONE frontier's members.
       A second session that kept any of them would report the first session's round trip as its own and would
       skip the routing walk on a frontier that has just been rebuilt with claims in it. */
    g_orphan_claims_met = 0; g_orphan_claims_unmet = 0; g_orphan_claims_closed = 0;
    attr_shadow_free(ctx);
    endpoint_free();
    /* THE CONCOLIC VALUE COMPONENT IS LAST, and the position is the argument. Its SOURCE REGISTRY is what a
       report asks for a source's browser delivery — the encode set, the address component, the reproduction
       mechanism — so every line above may still read it while it renders and releases what it holds. It is
       also the one registry in this half that BROWSER components write into: each row is a claim given back by
       its claimant on core/platform.h's release column, which platform_agent_free ran before this call, so the
       assert at its first line is a checked statement about ITS CLAIMANTS rather than about this component —
       and it names the one that did not finish out of the row, which is why nothing here has to list them. */
    concolic_free();
}

