/* HTML §6.10.1 "Close requests"' CLOSE REQUEST STEPS — the nine steps that JOIN the Fullscreen API Standard §2
 * "Model" to HTML §6.10.2 "Close watcher infrastructure". See close_request.h for why the algorithm is a
 * component of its own, why its two callees do not share a stack, why it is a request a calling machine drives
 * rather than a function. Its PRODUCER is at the bottom of this file: `close_request_flow` is §6.10.1's own
 * preamble — the global task queued on the user interaction task source — and the header's named residual
 * is now the ONE of §6.10.1's two platforms that fires an event.
 *
 * ONE CONVENTION, STATED ONCE. A bare "step N" below is a step of HTML §6.10.1 "Close requests"' own nine, and
 * a sub-number is written only for step 1, which is the one step of the nine that holds a nested list — one
 * list, of two items, so a sub-number under HTML §6.10.1's step 1 names exactly one thing. Every citation of
 * another standard carries that standard's name at the site.
 *
 * THE ONE THING THIS FILE DECIDES THAT THE HEADER DOES NOT. Step 1's second sub-step is a bare "Return.", so
 * the fullscreen arm and the close-watcher arm are EXCLUSIVE and the order between them is the standard's
 * priority rather than an optimisation: a document showing something fullscreen answers a close request by
 * leaving fullscreen and by doing nothing else, however many watchers its page established meanwhile. That is
 * asserted below rather than merely obeyed, because the failure it prevents is silent — a step 7 reached with
 * a fullscreen element would close a popover and leave the document fullscreen, and every value involved would
 * still be well-formed. */
#include <stdbool.h>
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/dom/document.h"
#include "core/dom/node.h"
#include "core/events/event.h"
#include "core/fullscreen/fullscreen.h"
#include "core/html/close_request.h"
#include "core/html/close_watcher.h"

/* These nine steps' own resume points. PHASES and not stages, for close_watcher.c's reason: a stage is what
   the driver asserts a machine is parked at, and this is a SUB-SEQUENCE the calling machine's one stage hosts.
   CRQ_WATCHERS is the only one that can be resumed into — it is the only phase that parks. */
enum { CRQ_START = 0, CRQ_WATCHERS, CRQ_DONE };

void close_request_run_init(CloseRequestRun *r)
{
    r->phase = CRQ_START;
    r->ev = JS_UNDEFINED;
    close_watcher_run_init(&r->cw);
}

void close_request_run_visit(JSContext *ctx, CloseRequestRun *r, JSStepVisit *v)
{
    v->val(ctx, &r->ev);
    close_watcher_run_visit(ctx, &r->cw, v);
}

void close_request_run_unlock(JSContext *ctx, CloseRequestRun *r)
{
    close_watcher_run_unlock(ctx, &r->cw);
}

/* "Whenever the user agent receives a potential close request targeted at a Document document, it must queue a
   global task on the user interaction task source given document's relevant global object to perform the
   following close request steps:", 9 steps. */
int close_request_run(JSContext *ctx, JSStepHdr *hdr, CloseRequestRun *r, lxb_dom_node_t *document,
                      JSValue in, bool *palternative, JSValue **out_cb, int *out_argc)
{
    JSContext *dctx;
    int rc;

    DCHECK(document != NULL && document->type == LXB_DOM_NODE_TYPE_DOCUMENT,
           "HTML §6.10.1 Close requests' close request steps were run for something that is not a Document — "
           "its preamble targets a potential close request AT a Document, and step 7 asks that document's "
           "relevant global object for the Window whose close watcher manager decides the answer");
    /* "document's relevant global object" — the Window these steps run in, which is the DOCUMENT's realm and
       never the calling machine's. A Document node this agent holds always has one; a NULL here would send
       step 7 to whichever manager the caller happened to be standing in. */
    dctx = document_realm_of(document);
    DCHECK(dctx != NULL,
           "HTML §6.10.1 Close requests' close request steps were run for a Document with no realm — the "
           "(document -> realm) answer is minted with the document and these steps run in the Window it names");

    if (r->phase == CRQ_START) {
        JSValue fs;

        JS_FreeValue(ctx, in);
        in = JS_UNDEFINED;

        /* STEP 1 — "If document's fullscreen element is not null:" */
        fs = fullscreen_element(dctx, document);
        if (!JS_IsNull(fs)) {
            /* Step 1.1 — "Fully exit fullscreen given document's node navigable's top-level traversable's
               active document." FULLSCREEN §2 "Model" defines it: "To fully exit fullscreen a document
               document, run these steps:". core/fullscreen/fullscreen.h's first named residual names it as
               unbuilt, together with the fullscreen an element that would be the only way to reach here — so
               this arm is UNREACHABLE in this build rather than wrong, exactly as close_watcher.c's popover
               close-action arm is, and it crashes rather than falling through to step 7 because falling
               through is the one outcome HTML §6.10.1's step 1.2, a bare "Return.", forbids. */
            DFAIL("HTML §6.10.1 Close requests' close request steps reached step 1 with a non-null fullscreen "
                  "element — FULLSCREEN §2 Model's fully exit fullscreen is not built, and core/fullscreen/"
                  "fullscreen.h's first named residual is what names it and its unfullscreen-an-element and "
                  "run-the-fullscreen-steps prerequisites; build those, call it here, and set *palternative "
                  "false for HTML §6.10.1 Close requests' step 1.2, which is a bare Return");
        }
        JS_FreeValue(dctx, fs);

        /* STEP 2 — "Optionally, skip to the step labeled alternative processing." DECLINED, and the header
           says why: its own example is a user agent that "detects user frustration at repeated close request
           interception by the current web page", and an agent with no user detects none. Taking the option
           unconditionally would make every close request a no-op and would be indistinguishable from a broken
           manager. There is no code for a declined option — this comment is the decision. */

        /* STEP 3 — "Fire any relevant events, per UI Events or other relevant specifications." NONE ARE FIRED,
           and that is one of the two platforms §6.10.1 itself describes rather than a capability this build is
           missing: "On platforms where a back button is a potential close request, no event is involved, so
           when the back button is pressed, the user agent proceeds directly to process close watchers." The
           OTHER platform — "the "relevant events" that are fired must be the single keydown event" — needs a
           trusted input source this agent does not have, which is the header's named residual. It is NOT the
           same absence core/html/user_activation.h describes: HTML §6.4.2 "Processing model" excludes the Esc
           key from its activation triggering input events BY NAME, so the event that platform fires is exactly
           the event §6.4.2 refuses, and §6.4.1's timestamps have a live producer of their own. */

        /* STEP 4 — "Let event be null if no such events are fired, or the Event object representing one of the
           fired events otherwise." Step 3 fired none, so it is null. Held on the run rather than kept as a C
           local because the keyboard platform's step 3 PARKS on the page's own `keydown` listeners, and a
           value a suspension crosses lives in the record the calling machine visits. */
        r->ev = JS_NULL;

        /* STEP 5 — "If event is not null, and its canceled flag is set, then return." */
        if (!JS_IsNull(r->ev) && event_canceled(dctx, r->ev)) {
            r->phase = CRQ_DONE;
            *palternative = false;
            return 0;
        }

        /* STEP 6 — "If document is not fully active, then return." The term is HTML §7.3.3 Fully active
           documents' walk, asked of the document's own realm. HTML §6.10.1's own note under this step explains
           the ORDER rather than the test: "This step is necessary because, if event is not null, then an event
           listener might have caused document to no longer be fully active." */
        if (!document_fully_active(dctx)) {
            r->phase = CRQ_DONE;
            *palternative = false;
            return 0;
        }
        r->phase = CRQ_WATCHERS;
    }

    if (r->phase == CRQ_WATCHERS) {
        bool closed_something = false;

        /* STEP 7 — "Let closedSomething be the result of processing close watchers on document's relevant
           global object." The one parking step: §6.10.2's process close watchers runs each watcher's cancel
           and close actions, which are the page's own handlers, and asks §6.4.1's history-action activation,
           which is answered by a fork. */
        rc = close_watcher_process_run(dctx, hdr, &r->cw, in, &closed_something, out_cb, out_argc);
        if (rc) return rc;

        r->phase = CRQ_DONE;
        /* STEP 8 — "If closedSomething is true, then return." */
        /* STEP 9 — "Alternative processing: Otherwise, there was nothing watching for a close request. The
           user agent may instead interpret this interaction as some other action, instead of interpreting it
           as a close request." The "may" is the CALLER's, so what this algorithm owes it is the fact that it
           got here — §6.10.1's own worked example of exercising it being the back button, which "the user
           agent can interpret ... in another way, for example as a request to traverse the history by a delta
           of −1". */
        *palternative = !closed_something;
        return 0;
    }

    JS_FreeValue(ctx, in);
    DFAIL("HTML §6.10.1 Close requests' close request steps were re-entered after they had answered — the "
          "cursor is reset by whoever starts a new run of them");
    *palternative = false;
    return 0;
}

/* ---- HTML §6.10.1 "Close requests"' PREAMBLE — the potential close request, AS WORK -------------------------
 *
 * "Whenever the user agent receives a potential close request targeted at a Document document, it must queue a
 * global task on the user interaction task source given document's relevant global object to perform the
 * following close request steps:"
 *
 * WHICH PLATFORM THIS IS, IN THE STANDARD'S OWN WORDS, AND WHY THAT IS THE WHOLE OF THE PRODUCER. HTML §6.10.1
 * "Close requests" spells out two conforming platforms and this entry is the one that needs nothing else, in
 * HTML §6.10.1's own words: "On platforms where a back button is a potential close request, no event is
 * involved, so when the back button is pressed, the user agent proceeds directly to process close watchers."
 * So there is no `keydown` here, no dispatch and no target
 * — the nine steps above are COMPLETE for a gesture that fires no relevant event, which is what this file's
 * paragraph on steps 3 and 4 already says. The KEYBOARD platform is a strictly larger diff and is not this one.
 *   AND IT IS NOT AN ACTIVATION, WHICH IS THE ONE THING THIS ENTRY MUST NOT DO. HTML §6.4.2 "Processing
 * model" defines an activation triggering input event as "any event whose isTrusted attribute is true and
 * whose type is one of" five types, of which the first is "keydown", provided the key is neither the Esc key
 * nor a shortcut key reserved by the user agent. The Esc press §6.10.1's OTHER platform fires is therefore
 * exactly the event §6.4.2 excludes, and a back button fires no event at all — so nothing here performs
 * §6.4.2's activation notification steps, and a build that did would write a `last activation timestamp` for
 * an interaction the standard says did not happen. core/html/user_activation.h's producer is elsewhere.
 *
 * IT IS A FLOW AND NOT A C CALL, for report_exception.c's reason exactly: step 7 runs the page's own `cancel`
 * and `close` actions and asks §6.4.1 "Data model"'s history-action activation, which is answered by a FORK.
 * A `JS_Call` from the scheduler would be a C activation with no flow base under it — the drive-to-completion
 * this engine aborts on — and a second driver beside the one BFS is the cardinal violation.
 *
 * THE MACHINE IS ONE STAGE BECAUSE THE ALGORITHM IS A SUB-SEQUENCE. `CloseRequestRun` keeps §6.10.1's own
 * cursor (close_request.c's CRQ_*) and step 7's request keeps §6.10.2's, so this machine's single stage rests
 * at the QUEUED TASK and the phases below it are where the algorithm rests. Splitting the nine steps across
 * stages here would be a second copy of a cursor this record already holds. */

/* WHAT THE FLOW'S COMPLETION VALUE IS, AND WHY IT IS A VALUE AT ALL. §6.10.1's step 9 is "Alternative
   processing: Otherwise, there was nothing watching for a close request. The user agent may instead interpret
   this interaction as some other action, instead of interpreting it as a close request." The "may" is the
   caller's, and for a MODELLED gesture the caller is the solver — which does not reinterpret it (there is no
   history traversal here to reinterpret it AS) but does need the fact, because it is the standard's own
   statement that nothing in this timeline is watching. A machine that dropped it would leave
   close_request_run's `*palternative` a computed value with no reader, which is the mirror of the defaulted
   field and is the reason this is a boolean completion rather than a comment. */
#define CRQF_STAGES(X)                                                                                        \
    X(CRQF_TASK, "HTML §6.10.1 Close requests' preamble — the global task queued on the user interaction "     \
                 "task source given document's relevant global object that performs the close request steps")
enum { CRQF_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const CRQF_STEPS[] = { CRQF_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct JSCloseRequestFlow {
    JSStepHdr       hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t         started;
    bool            alternative;   /* step 9's one fact, yielded by fini */
    CloseRequestRun rq;       /* §6.10.1's nine steps, as the request this machine drives */
} JSCloseRequestFlow;

static void js_close_request_flow_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    close_request_run_visit(ctx, &((JSCloseRequestFlow *)st)->rq, v);
}

static JSValue js_close_request_flow_fini(JSContext *ctx, void *st, bool take_result)
{
    JSCloseRequestFlow *s = st;

    /* §6.10.2's "is running cancel action", GIVEN BACK if this task was abandoned holding it — a flow dropped
       inside the page's own `cancel` handler would otherwise leave that watcher refusing every later request
       for the rest of the session. Not a reference, so no declaration names it and the visit above is what
       releases the record's references. */
    close_request_run_unlock(ctx, &s->rq);
    return take_result ? JS_NewBool(ctx, s->alternative) : JS_UNDEFINED;
}

static int js_close_request_flow_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb,
                                      int *out_argc)
{
    JSCloseRequestFlow *s = st;
    lxb_dom_node_t *document;
    int r;

    DCHECK(s->hdr.stage == CRQF_TASK,
           "HTML §6.10.1 Close requests' queued task resumed into a stage it does not have — this machine is "
           "ONE task and delegates the whole of the nine steps to the request it holds, which keeps its own "
           "cursor");
    if (!s->started) {
        /* EVERY OWNED FIELD PLACED BEFORE THE FIRST THING THAT CAN FAIL — the failure path tears this state
           down through `fini`, which gives back exactly what the state holds and nothing else. */
        close_request_run_init(&s->rq);
        s->started = 1;
    }
    /* THE DOCUMENT THE POTENTIAL CLOSE REQUEST WAS TARGETED AT, taken from the frame's own argument rather than
       from a field of this state: a Lexbor node is not a JSValue, so a raw pointer held here would be a
       reference no `visit` can carry across a fork and no park can serialize, while the WRAPPER is an ordinary
       argument the frame dup'd and the delta already knows how to keep alive. */
    document = node_of(step_arg(&s->hdr, 0));
    DCHECK(document != NULL,
           "HTML §6.10.1 Close requests' queued task was built over an argument that is not a node — the frame "
           "is minted with the target Document's own wrapper and nothing else can be at that position");
    r = close_request_run(ctx, &s->hdr, &s->rq, document, cb_result, &s->alternative, out_cb, out_argc);
    if (r)
        return r;   /* parked inside a watcher's cancel/close action, or forked at history-action activation */
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_close_request_flow_def = {
    sizeof(JSCloseRequestFlow), js_close_request_flow_step, js_close_request_flow_fini, 0,
    .visit = js_close_request_flow_visit,
    .algorithm = "HTML §6.10.1 Close requests — the close request steps, as the task its preamble queues",
    .steps = CRQF_STEPS
};

/* The id JS_RegisterStepDef handed out for the machine above. Runtime-lifetime, so it is registered on first
   use exactly as report_exception.c's is; the DEFINITION is static data and outlives the runtime. */
static int g_close_request_stepid = -1;

JSValue *close_request_flow(JSContext *ctx, lxb_dom_node_t *document)
{
    JSContext *dctx;
    JSValue fn, arg;
    JSValue *base;

    DCHECK(document != NULL && document->type == LXB_DOM_NODE_TYPE_DOCUMENT,
           "HTML §6.10.1 Close requests' task was queued for something that is not a Document — its preamble "
           "targets a potential close request AT a Document, and the task is queued on that document's "
           "relevant global object");
    /* "given document's relevant global object" — the realm the TASK runs in, derived from the node here and
       never taken from the caller, which is the same split close_request_run makes for the same sentence. */
    dctx = document_realm_of(document);
    DCHECK(dctx != NULL,
           "HTML §6.10.1 Close requests' task was queued for a Document with no realm — the (document -> realm) "
           "answer is minted with the document and this task runs in the Window it names");
    if (g_close_request_stepid < 0) {
        g_close_request_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_close_request_flow_def);
        CHECK(g_close_request_stepid >= 0,
              "HTML §6.10.1 Close requests' task machine could not be registered with this runtime");
    }
    /* MINTED IN THE REALM THAT USES IT, per task. `js_call_c_function` takes the CALLEE's own realm, so this
       object is what decides which global these steps run in — a function held in a static would answer every
       document's close request with the first document's ctx, which is the per-realm defect a shared prototype
       is. There is nothing to cache: a close request is one arrival. */
    fn = JS_NewCFunction2(dctx, NULL, "closeRequestSteps", 1, JS_CFUNC_step, g_close_request_stepid);
    CHECK(!JS_IsException(fn), "HTML §6.10.1 Close requests' task callee could not be allocated");
    arg = node_wrap(dctx, document);
    CHECK(!JS_IsException(arg), "HTML §6.10.1 Close requests' target Document could not be wrapped");
    /* take_result TRUE — AND IT IS THE WHOLE REASON THIS IS A FLOW WITH A COMPLETION VALUE AT ALL. The task's
       completion is step 9's one fact (the machine's `fini` above yields it), and the caller LATCHES it: a
       `false` here would free that boolean one frame below the reader and hand the reader JS_UNDEFINED, which
       `JS_ToBool` turns into the POSITIVE claim that something was watching — a claim no run made, and one
       that stops the arrival ever being answered, so this document's close request is modelled again on every
       later step of the flow. */
    base = JS_FlowNewCall(dctx, fn, JS_UNDEFINED, 1, (JSValueConst *)&arg, true);
    JS_FreeValue(dctx, arg);   /* JS_FlowNewCall dup'd the callee and the argument into the frame */
    JS_FreeValue(dctx, fn);
    CHECK(base != NULL, "HTML §6.10.1 Close requests' task frame could not be allocated");
    return base;
}
