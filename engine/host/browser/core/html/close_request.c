/* HTML §6.10.1 "Close requests"' CLOSE REQUEST STEPS — the nine steps that JOIN the Fullscreen API Standard §2
 * "Model" to HTML §6.10.2 "Close watcher infrastructure". See close_request.h for why the algorithm is a
 * component of its own, why its two callees do not share a stack, why it is a request a calling machine drives
 * rather than a function, and for the named residual that says plainly it has no producer in this build.
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
           trusted input source this agent does not have, which is the header's named residual and is the same
           absent source core/html/user_activation.h names for §6.4.1's timestamps. */

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
