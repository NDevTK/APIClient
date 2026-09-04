/* "UPDATE THE RENDERING" — HTML §8.1.7.3. See rendering.h for what this is, why it is not §14.3, and why it
   holds no loop of its own. */
#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/dom/document.h"
#include "core/dom/element_scrolling.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/events/report_exception.h"
#include "core/frame/window_proxy.h"
#include "core/frame/navigable.h"
#include "core/frame/navigation.h"
#include "core/frame/viewport.h"
#include "core/frame/visual_viewport.h"
#include "core/html/autofocus.h"
#include "core/html/focus.h"
#include "core/css/media_query_list.h"
#include "core/css/top_layer.h"
#include "core/dom/page_visibility.h"
#include "core/dom/scroll_events.h"
#include "core/intersection_observer/intersection_observer.h"
#include "core/rendering/animation_frame.h"
#include "core/rendering/page_reveal.h"
#include "core/rendering/rendering.h"
#include "core/timing/event_loop.h"
#include "core/timing/hr_time.h"
#include "core/timing/timer.h"
#include "solver/engine.h"

/* THE UA'S REFRESH RATE. See rendering.h: the spec hands this to the user agent, so answering it is modelling
   rather than approximating. */
#define RENDERING_REFRESH_HZ 60.0
#define RENDERING_FRAME_MS   (1000.0 / RENDERING_REFRESH_HZ)

/* §8.1.7.1's LAST RENDER OPPORTUNITY TIME LIVES WITH THE CLOCK IT IS A MOMENT ON — core/timing/event_loop.c.
   It belongs to the event LOOP, which is the agent's, and this file used to say exactly that while holding it
   in a `static double` "rather than in the heap: it is not a document's state and no flow owns it, exactly as
   `timer_now()` is not a flow's clock". Both halves of that were wrong the same way. A flow IS a timeline of
   this event loop: it queues its own rendering task and advances its own clock, so a frame flow A took moved
   flow B's next frame and, through the clock move below it, B's clock with it — a parked flow resuming into a
   moment another timeline reached, which §Time-travel-resume's razor calls a cap. The record it moved to is a heap
   object whose property writes the per-flow COW delta captures, so the field is still ONE per agent and is
   now also one per timeline. */
static int    g_stepid = -1, g_driver_slot = -1;
static int    g_ready;

/* ---- steps 2 to 5 ----------------------------------------------------------------------------------------
 *
 * A STEP OF §8.1.7.3 WHOSE SPECIFICATION IS NOT IN THIS BUILD. Eleven of the twenty-three steps are calls into
 * ANOTHER standard — CSSOM View's resize and scroll steps, Web Animations §4.4, Fullscreen, the canvas context
 * lost steps, Resize Observer §3.4, CSS View Transitions §7.2, Intersection Observer §3.2.2 — and this engine
 * has none of those standards. Their work-sets are therefore EMPTY BY CONSTRUCTION: the list a step drains is
 * filled by an interface that does not exist, so nothing can have filled it, and the page's own ReferenceError
 * on that interface is the forcing function §NO STUBS asks for.
 *
 * THAT IS AN ASSERTION AND NOT A SKIP, and the mechanism is `realm_awaits` — core/realm.h, which states the
 * whole of how it works and why. It is two-sided: the path names the member whose arrival would give the step
 * work, so the moment somebody lands ResizeObserver, or a viewport, or Web Animations, the DCHECK fires AT the
 * step of update-the-rendering that must then be written. It stood here as a static of this file; §6.6.7's
 * flush autofocus candidates needed the same assertion for two conditions of its OWN, so it moved to the one
 * place every realm goes through rather than being copied — a two-sided assertion's second copy is worse than
 * ordinary duplication, because the copy nobody maintains goes on being silent.
 *
 * STEP 7 IS NO LONGER ONE OF THEM. Its producer — §6.6's focus model, and the FocusEvent its focusing steps
 * fire — landed, this assertion fired at exactly this place in the order, and the step it named was built:
 * core/html/autofocus.c is HTML §6.6.7, and UR_AUTO below is the step that flushes it. NEITHER IS STEP 19:
 * Intersection Observer landed, the DCHECK that named it fired here, and UR_INTERSECT below is that step. */

/* Does this document give §8.1.7.3 anything to do? Step 4 removes a doc "for which the user agent believes
   updating the rendering would have no visible effect AND whose map of animation frame callbacks is empty" —
   a document that has never been revealed has a visible effect pending by definition, so the two clauses are
   these two. Asked of the doc's OWN realm, because both pieces of state are per-Window.
   INTERSECTION OBSERVER §3.4.2 ADDS ONE TERM TO THIS STEP BY NAME, and it is not decoration: it says the
   "Unnecessary rendering" step "should be modified to add an additional requirement for skipping the rendering
   update — the document does NOT HAVE PENDING INITIAL IntersectionObserver TARGETS". Without it a document
   whose only pending work is an observation is removed here, no frame is ever queued for it, and step 19 is
   written and UNREACHABLE — which is what CSSOM VIEW §4.2's own term one line up already exists to prevent.
   CSSOM VIEW §13.2's TERM IS HERE FOR EXACTLY THAT REASON AND THE STANDARD DOES NOT SPELL IT. §13.2 gives a
   Document a list of pending scroll events and step 9 below is its only drain, so a document whose only
   pending work is a queued `scroll` is removed by this step, no frame is ever queued for it, and the page's
   `scroll` listener never runs — the same shape as the observer term above, arrived at from the same place.
   It is a VISIBLE EFFECT in the step's own words: a scrolled document that has not been re-rendered is the
   canonical one. */
static bool doc_has_rendering_work(JSContext *docctx)
{
    return page_reveal_pending(docctx) || animation_frame_pending(docctx)
        || media_query_list_pending(docctx) || intersection_observer_pending(docctx)
        || scroll_events_pending(docctx);
}

/* Steps 2-5, as ONE walk, and the fusion is the spec's rather than a shortcut: step 2 collects the fully
   active documents of this event loop in container-precedes-contained order, and steps 3, 4 and 5 REMOVE from
   that list. Collecting only what survives is the same list.
 *
 * THE ORDER IS THE NAVIGABLE TREE's, and the walk that produces it belongs to navigable.c because the tree
 * does — the per-document LOAD LIFECYCLE needs the same order for a different reason, and a second copy of a
 * tree walk is the second answer that is always subtly wrong. Step 3's last clause (a navigable with no
 * rendering opportunity) is what removes the ones that walk does not report: an unmaterialized navigable holds
 * the initial about:blank Document §7.4 created it with, which has no scripts, so nothing in it can have
 * called `requestAnimationFrame` and the only listener that could observe its `pagereveal` would have to be
 * registered by code in it — which is what materializing it means.
 *
 * A RENDER-BLOCKED DOCUMENT IS REMOVED BY STEP 3's first clause, and in this engine that is the document whose
 * parser has not finished — see document_render_blocked. It is what puts the first rendering opportunity after
 * DOMContentLoaded, where a browser puts it. */
static JSValue rendering_collect_docs(JSContext *ctx)
{
    JSValue docs = JS_NewArray(ctx), all = navigable_tree_order(ctx), len;
    uint32_t ndocs = 0, n = 0, i;

    CHECK(!JS_IsException(docs), "update the rendering: OOM collecting this event loop's documents");
    len = JS_GetPropertyStr(ctx, all, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    for (i = 0; i < n; i++) {
        JSValue proxy = JS_GetPropertyUint32(ctx, all, i);
        JSContext *docctx = window_proxy_realm(ctx, proxy);

        /* STEP 3's VISIBILITY CLAUSE — "remove any doc whose visibility state is `hidden`". ONE fact, two
           readers, no second answer: the page reads it through §6.6's members and a branch there FORKS, while
           this read is C and cannot, so it concretizes on what the flow already decided (page_visibility.h).
           A flow exploring the backgrounded world therefore gets no rendering opportunity, which is what a
           browser does to a hidden tab. */
        if (page_visibility_hidden(docctx)) { JS_FreeValue(ctx, proxy); continue; }
        if (!document_render_blocked(docctx) && doc_has_rendering_work(docctx))
            JS_SetPropertyUint32(ctx, docs, ndocs++, JS_DupValue(ctx, proxy));
        JS_FreeValue(ctx, proxy);
    }
    JS_FreeValue(ctx, all);
    return docs;
}

/* ---- the machine ------------------------------------------------------------------------------------------ */

#define UPDATE_RENDERING_STAGES(X)                                                                             \
    X(UR_DOCS,   "HTML §8.1.7.3 update the rendering steps 1-5 (frameTimestamp is the event loop's last "       \
                 "render opportunity time; docs is every fully active Document of this event loop, a "          \
                 "container before what it contains; remove the non-renderable, the unnecessary and the "       \
                 "user-agent-skipped)")                                                                        \
    X(UR_REVEAL, "HTML §8.1.7.3 update the rendering step 6 (for each doc: reveal doc — HTML §7.4.6.3 fires "   \
                 "pagereveal at its relevant global object), one document per rest")                           \
    X(UR_AUTO,   "HTML §8.1.7.3 update the rendering step 7 (for each doc whose node navigable is a "           \
                 "TOP-LEVEL TRAVERSABLE: flush autofocus candidates — HTML §6.6.7 drains the document's "       \
                 "candidates and runs §6.6.4's focusing steps for the first one that is still focusable, "      \
                 "firing blur/focusout/focus/focusin), one document per rest")                                 \
    X(UR_RESIZE, "HTML §8.1.7.3 update the rendering step 8 (for each doc: run the resize steps — CSSOM VIEW "  \
                 "§13.1 fires `resize` at the Window when doc's viewport has had its width or height changed "  \
                 "since the last run, and at the VisualViewport when its scale, width or height has), one "     \
                 "target per rest")                                                                            \
    X(UR_SCROLL, "HTML §8.1.7.3 update the rendering step 9 (for each doc: run the SCROLL STEPS — CSSOM VIEW " \
                 "§13.2 appends a `scrollend` for each scrolling box that was scrolled, then fires each "     \
                 "(target, type) pair of doc's PENDING SCROLL EVENTS in the order they were added, then "     \
                 "empties the list), one pair per rest")                                                      \
    X(UR_MEDIA,  "HTML §8.1.7.3 update the rendering step 10 (for each doc: evaluate media queries and report " \
                 "changes — CSSOM VIEW §4.2 fires `change` at each MediaQueryList whose matches state has "     \
                 "changed, in the order they were created, oldest first), one MediaQueryList per rest")         \
    X(UR_FRAMES, "HTML §8.1.7.3 update the rendering step 14 (for each doc: run the animation frame callbacks " \
                 "with the relative high resolution time given frameTimestamp and doc's relevant global "       \
                 "object — HTML §8.12 Animation frames), one callback per "                                                      \
                 "rest")                                                                                       \
    X(UR_FOCUS,  "HTML §8.1.7.3 update the rendering step 17, the FOCUS FIXUP RULE (for each doc: if doc's "    \
                 "focused area is no longer a focusable area, run HTML §6.6.4's focusing steps for doc's "      \
                 "viewport — which fires `blur` and `focusout` down the old focus chain and `focus` and "       \
                 "`focusin` up the new one), one document per rest")                                           \
    X(UR_INTERSECT, "HTML §8.1.7.3 update the rendering step 19 (for each doc: run the UPDATE INTERSECTION "   \
                 "OBSERVATIONS steps — INTERSECTION OBSERVER §3.2.10 computes each target's intersection "     \
                 "rectangle, decides its threshold index and QUEUES an IntersectionObserverEntry, which "      \
                 "QUEUES A TASK on the IntersectionObserver task source; the callbacks run from that task and " \
                 "NOT from this step), one OBSERVER per rest")                                                 \
    X(UR_PAINT,  "HTML §8.1.7.3 update the rendering steps 20-23 (record rendering time; mark paint timing; "  \
                 "update the rendering or user interface; process top layer removals)")
enum { UPDATE_RENDERING_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const UPDATE_RENDERING_STEPS[] = { UPDATE_RENDERING_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct JSUpdateRendering {
    JSStepHdr hdr;          /* FIRST — the driver writes the def and the operand bounds through it */
    JSValue   docs;         /* step 2's surviving list, as each document's WindowProxy (owned) */
    JSValue   ev;           /* the event a per-document step is holding across its dispatch (owned) */
    JSValue   target;       /* step 10's MediaQueryList, held across its dispatch (owned) */
    JSValue   fn;           /* step 14's callback, taken from the map and held across its call (owned) */
    EventFireCb   cb;        /* the request buffer: a fire needs [this, fn, target, event], a call three */
    /* THE TWO MOMENTS ARE VALUES AND NOT `double`s, and they are OWNED (js_update_rendering_visit's list is
       what discharges them). §8.7's `timeout` can be unknown external input, so firing that timer moves the
       event loop's clock to a moment nothing computed — see core/timing/event_loop.h — and both of these are
       read straight off that clock. A `double` here would have had to pick a number for it. */
    JSValue   frame_ts;     /* step 1 (owned) */
    JSValue   layout_start; /* step 15's unsafeStyleAndLayoutStartTime (owned) */
    uint32_t  ndocs, i;     /* the cursor every "for each doc of docs" step shares */
    uint32_t  nframe, k;    /* §8.12 Animation frames step 2's key snapshot for the current doc, and step 3's cursor */
    uint32_t  nmedia, m;    /* §4.2's collection snapshot for the current doc, and its cursor */
    /* §13.2's step 2 EXTENT for the current doc, and its cursor. The extent is measured once per document,
       by step 1, because a `scroll` listener that scrolls again APPENDS — and an append cannot move a pair
       already in the list, so an index below the extent names one pair for the whole walk. */
    uint32_t  nsev, sv;
    uint32_t  nobs, ob;     /* §3.2.10 step 1's observer list for the current doc, and step 2's cursor */
    uint8_t   obsnapped;    /* that list has been counted for docs[i] */
    uint8_t   fphase;       /* the fire request steps 6, 8 and 10 park on */
    /* WHICH OF STEP 8's TWO FIRES docs[i] is on — CSSOM VIEW §13.1 is two ordered conditions over two
       different targets, the Window and its VisualViewport, and each fire runs the page's listeners. So it is a
       cursor like `m` is over step 10's MediaQueryLists rather than two stages: the walk rests between them. */
    uint8_t   rhalf;
    /* The CALL request steps 7, 14 and 17 park on. One byte for all three because one call is ever in flight —
       a stage leaves its loop only with the request finished, which is what makes it also the flag that says
       whether a resume is landing inside the step's call or at the top of its walk. */
    uint8_t   cphase;
    uint8_t   snapped;      /* §8.12 Animation frames step 2 has been taken for docs[i] */
    uint8_t   msnapped;     /* §4.2's collection snapshot has been taken for docs[i] */
    uint8_t   ssnapped;     /* §13.2's step 1 has run and its step 2's extent has been taken for docs[i] */
    bool      not_canceled;
    /* §8.1.4.6's REPORT AN EXCEPTION, for a callback that threw. The work record is THIS machine's, the way
       abort.h's AbortSignalWork and the dispatch machine's own are their callers' — reporting fires an `error`
       event, so it parks, and the state it parks with belongs to whoever is doing the reporting. */
    ReportExceptionWork rep;
    JSValue   exc;          /* the exception being reported, taken off the context (owned) */
    uint8_t   reporting;    /* the report is in flight and must be resumed before the walk continues */
} JSUpdateRendering;

static void js_update_rendering_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSUpdateRendering *s = st;
    int k;

    v->val(ctx, &s->docs);
    v->val(ctx, &s->ev);
    v->val(ctx, &s->target);
    v->val(ctx, &s->fn);
    v->val(ctx, &s->exc);
    v->val(ctx, &s->frame_ts);
    v->val(ctx, &s->layout_start);
    report_exception_work_visit(ctx, &s->rep, v);
    STEP_CB_FOREACH(s->cb, k)
        v->val(ctx, &s->cb[k]);
}

static JSValue js_update_rendering_fini(JSContext *ctx, void *st, bool take_result)
{
    JSUpdateRendering *s = st;

    (void)take_result;
    /* §8.1.4.6 step 5's FLAG, if an animation-frame callback's report was abandoned holding it. Not a
       reference, so no declaration names it; the record's references are js_update_rendering_visit's. */
    report_exception_work_unlock(ctx, &s->rep);
    return JS_UNDEFINED;
}

/* The realm of docs[i] — the document these per-document steps are FOR, which is where their events are
   dispatched and their callbacks invoked. A member run in the machine's own realm instead would fire every
   child document's `pagereveal` at the root window, which is the defect §3.7 exists to prevent one link up. */
static JSContext *doc_realm(JSContext *ctx, JSUpdateRendering *s)
{
    JSValue proxy = JS_GetPropertyUint32(ctx, s->docs, s->i);
    JSContext *docctx;

    DCHECK(window_proxy_is(proxy), "update the rendering: step 2's list held something that is not a document");
    docctx = window_proxy_realm(ctx, proxy);
    JS_FreeValue(ctx, proxy);
    return docctx;
}

/* STEP 7 IS WRITTEN — it is UR_AUTO, below, and its assertion is GONE rather than relaxed. That is the whole
   point of the mechanism: HTML §6.6's focus model and its FocusEvent arrived, the DCHECK fired at this exact
   place in the order, and the step it named was built (core/html/autofocus.c is HTML §6.6.7). A probe that
   outlived the work it demanded would be a claim that the step is still unwritten.

   STEP 8 IS WRITTEN TOO — it is UR_RESIZE, below, and its assertion is gone for the same reason. It named
   `innerWidth` and said "this build now models a viewport, so step 8 must compare it and fire": the members
   arrived (core/frame/viewport.c installs CSSOM VIEW §4's), the DCHECK fired here, and CSSOM VIEW §13.1's two
   conditions were built. What this component does NOT own is the comparison — `viewport_resize_changed` and
   `visual_viewport_resize_changed` are the two halves, each latching in the component that owns the geometry,
   because a viewport this file remembered for itself would be a second answer to the one fact.

   STEP 9 IS WRITTEN TOO — it is UR_SCROLL, below, and its assertion is GONE rather than relaxed, which is
   this mechanism working for the fourth time in this file. It asked `realm_awaits(docctx, "scrollTo", …)`
   first and that was the wrong instrument: this step's producer is not a member, because CSSOM VIEW §13.2
   "Scrolling" fills doc's list when a viewport or an element GETS SCROLLED, which is §3.1's perform a scroll
   and is an INTERNAL ALGORITHM with no name on any global to probe for. Asked instead of the component that
   owns the scrolling box, the assert fired the day §2's viewport row gave the clamp somewhere to land, and
   the step it named is now built: core/dom/scroll_events.c is §13.2 and UR_SCROLL is its drain.
   THE ARGUMENT THAT MADE THE NAME TEST LOOK SOUND IS RETIRED AND IS RESTATED IN CAPITALS SO NOBODY
   RE-DERIVES IT: §4's WINDOW MEMBERS ARE §4's ARGUMENT QUESTIONS PLUS A CALL TO `viewport_scroll`, WHOSE
   CLAMP LANDS ON THE POSITION THE VIEWPORT ALREADY HAS, SO THEY MOVE NOTHING. §2's viewport row retired the
   clamp half and core/dom/perform_scroll.c retired the rest. What survives is the shape of the mistake and
   not its arithmetic: A MEMBER IS NOT A CAPABILITY, and core/timing/hr_time.c records what the name test cost
   the last time. */

/* STEP 10 IS WRITTEN — it is UR_MEDIA, below. Its assert is gone rather than relaxed, which is the whole point
   of the mechanism: the producer (`matchMedia`) arrived, the DCHECK fired at this exact place in the order, and
   the step it named was built here.

   Steps 11 to 13, asserted in order at the place the algorithm runs them. */
static void steps_11_to_13(JSContext *docctx)
{
    realm_awaits(docctx, "Animation",
                "update the rendering step 11 UPDATES ANIMATIONS AND SENDS EVENTS (WEB ANIMATIONS §4.4): "
                "update the timelines, remove replaced animations, PERFORM A FULL MICROTASK CHECKPOINT (§4.4 "
                "step 3, inside this step), stable-sort the pending animation events and dispatch each — this "
                "build now has Animation, so step 11 must be written, and its internal checkpoint cannot be "
                "one stage with anything around it");
    realm_awaits(docctx, "Document.prototype.exitFullscreen",
                "update the rendering step 12 runs the FULLSCREEN STEPS, firing fullscreenchange/"
                "fullscreenerror and resolving requestFullscreen()'s promise — this build now has fullscreen, "
                "so step 12 must be written");
    realm_awaits(docctx, "CanvasRenderingContext2D",
                "update the rendering step 13 runs the CONTEXT LOST STEPS for each 2D context whose backing "
                "storage was lost: reset the context, fire `contextlost` CANCELABLE (its return value steers "
                "the algorithm), and fire `contextrestored` on a successful restore — this build now has a 2D "
                "context, so step 13 must be written");
}

/* Step 16, likewise. */
static void step_16(JSContext *docctx)
{
    realm_awaits(docctx, "ResizeObserver",
                "update the rendering step 16 is a `while (true)` around recalculate-styles-and-update-layout "
                "that GATHERS ACTIVE RESIZE OBSERVATIONS at an increasing depth and BROADCASTS them (RESIZE "
                "OBSERVER §3.4.1/§3.4.5), re-entering style and layout after every author callback, then "
                "delivers the resize loop error (§3.4.6) for any skipped observation — this build now has "
                "ResizeObserver, so step 16 must be written, and its loop cannot be one stage");
}

/* STEP 17 IS WRITTEN — it is UR_FOCUS, below, and its assert is GONE rather than relaxed. §6.6's focus model
   landed, the producer the probe named existed, the DCHECK fired at this exact place in the order, and the step
   it asked for was built. A probe that outlived the work it demanded would go on describing an absence that is
   no longer there, which is the one way this mechanism can lie.

   Step 18, still asserted at the place the algorithm runs it. */
static void step_18(JSContext *docctx)
{
    realm_awaits(docctx, "ViewTransition",
                "update the rendering step 18 PERFORMS PENDING TRANSITION OPERATIONS (CSS VIEW TRANSITIONS "
                "§7.2): setup view transition calls the author's ViewTransitionUpdateCallback and settles "
                "ready/updateCallbackDone/finished — this build now has ViewTransition, so step 18 must be "
                "written");
}

/* STEP 19 IS WRITTEN — it is UR_INTERSECT, below, and its assertion is GONE rather than relaxed. It named
   INTERSECTION OBSERVER "§3.2.2" for the update intersection observations steps, and §3.2.2 is OBSERVE A
   TARGET ELEMENT; the steps it described are §3.2.10, and they are now run there. The thing it was easiest to
   get backwards is the part that survived the check: those steps run NO author callbacks — they compute
   geometry, QUEUE an IntersectionObserverEntry (§3.2.6) and QUEUE A TASK on the IntersectionObserver task
   source (§3.2.4), and the observer's callback is invoked from THAT later task (§3.2.5 step 3.5). That is the
   opposite of ResizeObserver in step 16, and swapping them changes observable ordering.

   Steps 20 to 23. */
static void steps_20_to_23(JSContext *docctx)
{
    realm_awaits(docctx, "PerformanceObserver",
                "update the rendering steps 20 and 21 RECORD RENDERING TIME and MARK PAINT TIMING, which queue "
                "performance entries — this build now has a performance timeline to queue them on, so both "
                "steps must be written");
    /* STEP 22 — "update the rendering or user interface of doc and its node navigable to reflect the current
       state". THE PAINT, and the only step of the twenty-three with no headless equivalent: everything before
       it computes values that exist whether or not anything is drawn, and this one draws. There is no
       scriptable result for it to produce and no interface whose arrival would give it one, so it is a
       documented no-effect rather than an assertion — the one place in this component where that is the
       honest answer. */
    /* STEP 23 — "process top layer removals given doc", which is CSS Positioned Layout Level 4 §3.3 Top Layer
       Manipulation's own algorithm and whose note names this step as its ONE caller: "this is intended to be
       called during the 'Update the Rendering' step of HTML's rendering algorithm. It is not intended to be
       called by other algorithms."
       IT IS A CALL AND NOT A `realm_awaits` ANY MORE, and the probe that stood here is deleted rather than
       re-aimed because its sentence has become false: it said the top layer "is filled by `dialog.showModal()`
       and by fullscreen", and neither of those is in this build while the layer is filled anyway — HTML §6.12
       The popover attribute's show popover step 18 adds an element to it and its hide a popover step 12
       REQUESTS a removal, which is exactly the pending set this step drains. */
    top_layer_process_removals(docctx, document_object(docctx));
}

static int js_update_rendering_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSUpdateRendering *s = st;
    JSContext *docctx;
    int r, k;

    /* THE PAGE'S CODE THREW, AND THAT IS NOT THIS TASK'S COMPLETION. §8.12 Animation frames invokes an animation frame callback
       with "report" and §2.9 says the same of a listener: the exception is REPORTED and the walk CONTINUES. So
       this machine DECLARES catches_abrupt and the throw arrives here as a value instead of tearing the
       machine down — which is what used to happen, and it cost more than it looked like: steps 15 through 23
       of that frame were skipped, the remaining callbacks were left for the NEXT frame, and the exception
       itself was swallowed with nothing anywhere to say a page had thrown.
       REPORTING IT IS §8.1.4.6, and it is a COMPONENT — core/events/report_exception.c, which fires `error` at
       the global carrying an ErrorEvent. This site named it as absent and it was built for exactly this caller
       (error_event.c's own comment says so); DOM §2.9's throwing listener is the second caller and RESIZE
       OBSERVER §3.4.6's loop error is the third, all through the one operation.
       THE REPORT PARKS, because firing that event runs the page's code — so the work record is this machine's
       and the walk resumes at `report_throw` with the report finished. Reporting a throw by RESUMING rather
       than by returning is what keeps steps 15-23 of the frame running, which is what "report and continue"
       means. */
    if (JS_IsException(cb_result)) {
        cb_result = JS_UNDEFINED;
        s->exc = JS_GetException(ctx);
        s->reporting = 1;
        /* The abandoned request's buffer is this machine's, so it is released HERE rather than left for a new
           request to overwrite — step_call_run only frees what it placed when it is RESUMED, and this one
           never will be. */
        STEP_CB_FOREACH(s->cb, k) { JS_FreeValue(ctx, s->cb[k]); s->cb[k] = JS_UNDEFINED; }
        if (s->hdr.stage == UR_REVEAL) {
            s->fphase = 0;
            JS_FreeValue(ctx, s->ev);
            s->ev = JS_UNDEFINED;
            s->i++;                      /* this document is revealed; the reveal is not retried */
        } else if (s->hdr.stage == UR_MEDIA) {
            s->fphase = 0;
            JS_FreeValue(ctx, s->ev);
            JS_FreeValue(ctx, s->target);
            s->ev = s->target = JS_UNDEFINED;
            s->m++;                      /* §4.2 latched the new state before the fire: on to the next target */
        } else if (s->hdr.stage == UR_RESIZE) {
            s->fphase = 0;
            JS_FreeValue(ctx, s->ev);
            JS_FreeValue(ctx, s->target);
            s->ev = s->target = JS_UNDEFINED;
            /* §13.1 latched the new dimensions before the fire — the condition is "changed since the last time
               these steps were RUN", and they ran — so the same size never fires twice and the walk moves to
               the other half of the step rather than re-asking. */
            if (++s->rhalf > 1) { s->rhalf = 0; s->i++; }
        } else if (s->hdr.stage == UR_SCROLL) {
            s->fphase = 0;
            JS_FreeValue(ctx, s->ev);
            JS_FreeValue(ctx, s->target);
            s->ev = s->target = JS_UNDEFINED;
            /* §13.2's step 2 walks the list ONCE and step 3 empties it, so a pair whose fire completed
               abruptly is spent either way — the walk moves to the next pair rather than re-firing one the
               page has already been given. The list is emptied at the end of this document's walk exactly as
               it would have been. */
            s->sv++;
        } else if (s->hdr.stage == UR_FRAMES) {
            s->cphase = 0;
            JS_FreeValue(ctx, s->fn);
            s->fn = JS_UNDEFINED;
            s->k++;                      /* §8.12 Animation frames removed the callback before invoking it: on to the next */
        } else if (s->hdr.stage == UR_AUTO) {
            /* AN ABRUPT COMPLETION HERE IS THIS ENGINE'S, NOT THE PAGE'S, for UR_FOCUS's reason one arm down:
               §6.6.7 states no step that throws, and a `focus` listener's own exception is REPORTED inside
               §2.9's dispatch. What is left is an allocation failure or a capability of §6.6.7's machine that
               is not built — and the candidates list was emptied and the flag set before the focusing steps
               ran (§6.6.7 steps 5.11.1-5.11.2), so this document's autofocus is spent either way and the walk
               moves on rather than re-flushing a list that is now empty. */
            DFAIL("update the rendering step 7's FLUSH AUTOFOCUS CANDIDATES completed abruptly — §6.6.7 states "
                  "no step that throws and §2.9 reports a focus listener's own exception inside the dispatch, "
                  "so this is the engine's own failure and doc's autofocus candidates were left half-drained");
            s->cphase = 0;
            s->i++;
        } else if (s->hdr.stage == UR_FOCUS) {
            /* AN ABRUPT COMPLETION HERE IS THIS ENGINE'S, NOT THE PAGE'S — which is what makes it a crash and
               not a report. §6.6.4 states no step that throws, and a `blur` listener that throws is REPORTED
               inside §2.9's dispatch (that machine declares catches_abrupt and reports there), so a listener's
               exception never reaches this far. What is left is an allocation failure or a capability of the
               focus machine that is not built, and the fixup then did NOT happen: the focused area is still
               designating something that is not a focusable area, so the next frame runs the same repair and
               fails the same way. A release build cannot build the missing capability, so there the exception
               takes §8.1.4.6's path above with every other one that escapes a task. */
            DFAIL("update the rendering step 17's FOCUSING STEPS completed abruptly — §6.6.4 states no step "
                  "that throws and §2.9 reports a listener's own exception inside the dispatch, so this is "
                  "the engine's own failure and doc's focused area was left unrepaired");
            s->cphase = 0;
            s->i++;
        }
    }

    /* §8.1.4.6, PERFORMED AND RESUMED. The report fires an `error` event, so it parks inside its own dispatch
       and this machine is re-entered with that dispatch's answer — which is why the flag is sticky and the
       resume is here rather than in the branch that set it. Reporting by RESUMING is the whole of what "report
       and continue" buys: steps 15-23 of the frame still run, and the remaining callbacks are still this
       frame's rather than the next one's. */
    if (s->reporting) {
        int rr = report_exception_run(ctx, &s->rep, s->exc, cb_result, out_cb, out_argc);

        cb_result = JS_UNDEFINED;
        if (rr > 0)
            return rr;
        s->reporting = 0;
        JS_FreeValue(ctx, s->exc);
        s->exc = JS_UNDEFINED;
    }

    if (s->hdr.stage == UR_DOCS) {
        /* EVERY OWNED FIELD IS ON THE STATE BEFORE ANYTHING THAT CAN FAIL — the failure path tears the machine
           down through fini, which frees exactly what the state holds. */
        s->docs = s->ev = s->target = s->fn = s->exc = JS_UNDEFINED;
        s->frame_ts = s->layout_start = JS_UNDEFINED;
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
        /* The report's own record, zeroed before anything can throw — a step state arrives with the INTEGER 0
           in every JSValue slot, and the report's teardown frees exactly what it holds. */
        report_exception_work_start(&s->rep);
        s->reporting = 0;
        s->i = s->ndocs = s->nframe = s->k = s->nmedia = s->m = s->nobs = s->ob = 0;
        s->nsev = s->sv = 0;
        s->fphase = s->cphase = s->snapped = s->msnapped = s->obsnapped = s->ssnapped = 0;
        STEP_GOTO(s->hdr.stage, UR_REVEAL, &s->cphase, &s->fphase, NULL);
        /* STEP 1: frameTimestamp is the event loop's LAST RENDER OPPORTUNITY TIME — the moment the in-parallel
           loop recorded when it decided there was one, not the moment this task happens to run. */
        s->frame_ts = event_loop_last_render(ctx);
        /* STEPS 2-5 */
        s->docs = rendering_collect_docs(ctx);
        {
            JSValue len = JS_GetPropertyStr(ctx, s->docs, "length");
            JS_ToUint32(ctx, &s->ndocs, len);
            JS_FreeValue(ctx, len);
        }
        JS_FreeValue(ctx, cb_result);
        /* The walk is O(the page's frame tree), which is the page's to choose — so the machine offers the
           scheduler the chance to take the flow before the first document's author code runs. */
        return JS_STEP_YIELD;
    }

    if (s->hdr.stage == UR_REVEAL) {
        /* STEP 6: "For each doc of docs: reveal doc." */
        for (;;) {
            if (s->i >= s->ndocs) {
                s->i = 0;
                STEP_GOTO(s->hdr.stage, UR_AUTO, &s->cphase, &s->fphase, NULL);
                break;
            }
            docctx = doc_realm(ctx, s);
            if (s->fphase == 0 && JS_IsUndefined(s->ev)) {
                s->ev = page_reveal_begin(docctx);   /* §7.4.6.3 steps 1-3, and mint the event */
                if (JS_IsUndefined(s->ev)) { s->i++; continue; }   /* step 1: already revealed */
            }
            {
                JSValue g = JS_GetGlobalObject(docctx);
                r = event_target_fire_run(docctx, &s->fphase, STEP_CB(s->cb), g, s->ev, JS_UNDEFINED, cb_result,
                                          &s->not_canceled, out_cb, out_argc);
                JS_FreeValue(docctx, g);
            }
            if (r > 0) return r;                     /* parked on the page's listeners */
            cb_result = JS_UNDEFINED;
            JS_FreeValue(docctx, s->ev);
            s->ev = JS_UNDEFINED;
            s->i++;
        }
    }

    if (s->hdr.stage == UR_AUTO) {
        /* STEP 7: "For each doc of docs, FLUSH AUTOFOCUS CANDIDATES for doc IF ITS NODE NAVIGABLE IS A
           TOP-LEVEL TRAVERSABLE."
           THE CONDITION IS THE STEP'S, so it is asked here rather than inside the algorithm — and it is not
           decoration. §6.6.7's insertion steps append to the TOP-LEVEL traversable's active document's list
           however deeply nested the element is, so every other document's list is empty by construction;
           flushing one would set a processed flag on a document whose candidates all live somewhere else.
           §6.6.7 asserts the same condition at its own entry, from the other side.
           IT IS ITS OWN STAGE, AND ONE DOCUMENT PER REST, because §6.6.7 step 5.11.3 runs §6.6.4's focusing
           steps at the page's `blur`, `focusout`, `focus` and `focusin` listeners — a stage that spanned two
           documents would be a stage the scheduler cannot preempt between. The WALK OF THE CANDIDATES is
           §6.6.7's own machine and rests once per candidate there, which is what makes a document whose markup
           marked a hundred controls resumable mid-list rather than drained in one step. */
        for (;;) {
            if (s->i >= s->ndocs) {
                s->i = 0;
                s->rhalf = 0;
                STEP_GOTO(s->hdr.stage, UR_RESIZE, &s->cphase, &s->fphase, NULL);
                break;
            }
            docctx = doc_realm(ctx, s);
            if (s->cphase == 0 && !window_proxy_is_top_level(document_window_proxy(docctx))) {
                s->i++;
                continue;
            }
            r = autofocus_flush_run(docctx, &s->cphase, STEP_CB(s->cb), cb_result, out_cb, out_argc);
            if (r > 0) return r;                     /* parked on the page's focus listeners */
            cb_result = JS_UNDEFINED;
            s->i++;
        }
    }

    if (s->hdr.stage == UR_RESIZE) {
        /* STEP 8: "For each doc of docs: run the RESIZE STEPS for doc." CSSOM VIEW §13.1 is those steps, and it
           is TWO ordered conditions over two different targets:
             1. if doc's viewport has had its width or height changed since the last time these steps were run,
                fire an event named `resize` at the Window object associated with doc;
             2. if the VisualViewport associated with doc has had its scale, width or height changed since the
                last time these steps were run, fire `resize` at the VisualViewport.
           NEITHER COMPARISON IS MADE HERE. The viewport is viewport.c's fact and the visual viewport is
           visual_viewport.c's, and each latches what it saw into a per-realm record whose writes the COW delta
           captures — so a flow that reached this frame has its own answer to "since the last time", which is
           what makes the question meaningful when several flows are exploring one document. A width remembered
           in this machine's state would be a second answer to the one fact, and it would be the frame's rather
           than the flow's.
           `resize` IS A PLAIN Event, not bubbling and not cancelable: §13.1 says "fire an event named resize",
           and DOM §2.4's fire uses the Event interface and leaves both flags unset unless the caller says
           otherwise. It is TRUSTED, because the user agent is what fired it.
           IT IS ITS OWN STAGE, AND ONE TARGET PER REST, because each fire runs the page's `resize` listeners —
           and a page's resize handler is characteristically the most expensive one it has. */
        for (;;) {
            if (s->i >= s->ndocs) {
                s->i = 0;
                s->ssnapped = 0;
                STEP_GOTO(s->hdr.stage, UR_SCROLL, &s->cphase, &s->fphase, NULL);
                break;
            }
            docctx = doc_realm(ctx, s);
            if (s->fphase == 0 && JS_IsUndefined(s->ev)) {
                bool changed = s->rhalf == 0 ? viewport_resize_changed(docctx)
                                             : visual_viewport_resize_changed(docctx);
                if (!changed) {
                    if (++s->rhalf > 1) { s->rhalf = 0; s->i++; }
                    continue;
                }
                s->ev = event_new(docctx, "resize", false, false);
                CHECK(!JS_IsException(s->ev), "the resize steps could not allocate their event");
                /* THE TARGET IS HELD ACROSS THE FIRE, because the fire parks: §13.1's two targets are the
                   Window and the object `visualViewport` answers with, and the second is only reachable while
                   doc is fully active — which it is, because step 2 collected only fully active documents. */
                s->target = s->rhalf == 0 ? JS_GetGlobalObject(docctx) : visual_viewport_object(docctx);
                DCHECK(JS_IsObject(s->target),
                       "the resize steps found no target for a document step 2 collected — step 2's list is "
                       "fully active documents, and a fully active document has both a Window and the "
                       "VisualViewport §4 says its `visualViewport` answers with");
            }
            r = event_target_fire_run(docctx, &s->fphase, STEP_CB(s->cb), s->target, s->ev, JS_UNDEFINED,
                                      cb_result, &s->not_canceled, out_cb, out_argc);
            if (r > 0) return r;                     /* parked on the page's `resize` listeners */
            cb_result = JS_UNDEFINED;
            JS_FreeValue(docctx, s->ev);
            JS_FreeValue(docctx, s->target);
            s->ev = s->target = JS_UNDEFINED;
            if (++s->rhalf > 1) { s->rhalf = 0; s->i++; }
        }
    }

    if (s->hdr.stage == UR_SCROLL) {
        /* STEP 9: "For each doc of docs: run the SCROLL STEPS for doc." CSSOM VIEW §13.2 "Scrolling" is those
           steps, and they are three: step 1 turns each scrolling box that was scrolled into a (target,
           `scrollend`) pair, step 2 fires every pair of doc's pending scroll events IN THE ORDER THEY WERE
           ADDED, and step 3 empties the list.
           NONE OF THAT IS DECIDED HERE. The list, the appends and step 2's four branches over a pair belong to
           core/dom/scroll_events.c, which is where §13.2 is; what this machine owns is the FIRING, because a
           fire runs the page's `scroll` listeners and therefore parks. So this is a stage with ONE PAIR PER
           REST, on the same terms as step 8 above and for the same reason.
           STEPS 1 AND 3 ARE THE WALK'S TWO ENDS FOR EACH DOCUMENT, and `ssnapped` is what keeps them there: a
           resume landing mid-drain must not re-run step 1 (which would append a second `scrollend`) and must
           not re-measure step 2's extent. */
        for (;;) {
            if (s->i >= s->ndocs) {
                s->i = 0;
                s->msnapped = 0;
                STEP_GOTO(s->hdr.stage, UR_MEDIA, &s->cphase, &s->fphase, NULL);
                break;
            }
            docctx = doc_realm(ctx, s);
            if (!s->ssnapped) {
                s->nsev = scroll_events_scroll_steps_begin(docctx);   /* step 1, and step 2's extent */
                s->sv = 0;
                s->ssnapped = 1;
            }
            if (s->sv >= s->nsev) {
                scroll_events_scroll_steps_end(docctx);               /* step 3 */
                s->ssnapped = 0;
                s->i++;
                continue;
            }
            if (s->fphase == 0 && JS_IsUndefined(s->ev)) {
                /* THE TARGET IS HELD ACROSS THE FIRE for step 10's reason one stage down: the pair names it,
                   and the fire parks between naming it and dispatching at it. */
                scroll_events_item(docctx, s->sv, &s->target, &s->ev);
                DCHECK(JS_IsObject(s->target),
                       "§13.2's scroll steps step 2 produced a pair whose target is not an object — every "
                       "append in core/dom/scroll_events.c places an `EventTarget`, which is what §13.2 says "
                       "the pair's first half is");
            }
            r = event_target_fire_run(docctx, &s->fphase, STEP_CB(s->cb), s->target, s->ev, JS_UNDEFINED,
                                      cb_result, &s->not_canceled, out_cb, out_argc);
            if (r > 0) return r;                     /* parked on the page's `scroll` listeners */
            cb_result = JS_UNDEFINED;
            JS_FreeValue(docctx, s->ev);
            JS_FreeValue(docctx, s->target);
            s->ev = s->target = JS_UNDEFINED;
            s->sv++;
        }
    }

    if (s->hdr.stage == UR_MEDIA) {
        /* STEP 10: "For each doc of docs: evaluate media queries and report changes for doc." CSSOM VIEW §4.2's
           own walk is the inner loop — every MediaQueryList that has doc as its document, IN THE ORDER THEY
           WERE CREATED, oldest first — and it fires at each one whose matches state has changed.
           IT IS ITS OWN STAGE, AND ONE TARGET PER REST, because firing runs the page's `change` listeners: a
           stage that spanned two targets would be a stage the scheduler cannot preempt between, which is the
           unsuspendable span CLAUDE.md forbids wherever author code runs. */
        for (;;) {
            if (s->i >= s->ndocs) {
                for (s->i = 0; s->i < s->ndocs; s->i++)
                    steps_11_to_13(doc_realm(ctx, s));
                s->i = 0;
                s->snapped = 0;
                STEP_GOTO(s->hdr.stage, UR_FRAMES, &s->cphase, &s->fphase, NULL);
                break;
            }
            docctx = doc_realm(ctx, s);
            if (!s->msnapped) {                      /* the collection, snapshotted — see media_query_list.h */
                s->nmedia = media_query_list_count(docctx);
                s->m = 0;
                s->msnapped = 1;
            }
            if (s->m >= s->nmedia) {
                s->msnapped = 0;
                s->i++;
                continue;
            }
            if (s->fphase == 0 && JS_IsUndefined(s->ev)) {
                s->ev = media_query_list_change(docctx, s->m, &s->target);   /* §4.2, including its latch */
                if (JS_IsUndefined(s->ev)) { s->m++; continue; }   /* this target's state has not changed */
            }
            r = event_target_fire_run(docctx, &s->fphase, STEP_CB(s->cb), s->target, s->ev, JS_UNDEFINED, cb_result,
                                      &s->not_canceled, out_cb, out_argc);
            if (r > 0) return r;                     /* parked on the page's `change` listeners */
            cb_result = JS_UNDEFINED;
            JS_FreeValue(docctx, s->ev);
            JS_FreeValue(docctx, s->target);
            s->ev = s->target = JS_UNDEFINED;
            s->m++;
        }
    }

    if (s->hdr.stage == UR_FRAMES) {
        /* STEP 14: "For each doc of docs: run the animation frame callbacks for doc, passing the relative high
           resolution time of frameTimestamp." HTML §8.12 Animation frames's own three steps are the inner loop. */
        for (;;) {
            if (s->i >= s->ndocs) {
                /* STEP 15: unsafeStyleAndLayoutStartTime. It is the moment step 20's "record rendering time"
                   measures from, so it is READ here from the one clock rather than invented at the step that
                   consumes it — and the two are asserted against each other, which is the only thing that
                   makes reading it worth anything while the consumer is absent.
                   IT IS READ HERE BECAUSE STEP 15 IS HERE. It used to be read at UR_PAINT, after steps 16-18,
                   which was invisible while every one of those was an assertion and stopped being invisible
                   the moment step 17 became an algorithm that runs the page's `blur` listeners: the interval
                   step 20 reports would then have excluded the style and layout it is named after and included
                   none of it. */
                JS_FreeValue(ctx, s->layout_start);
                s->layout_start = event_loop_now(ctx);
                /* READ FROM THE CLOCK'S OWN ORDER, NOT RE-COMPUTED — over two known moments this is exactly
                   the `>=` that stood here, and over an unknown one a comparison is an arm to take rather
                   than a fact to test, so it asks what this flow already DECIDED and fires on a decided
                   contradiction (core/timing/event_loop.h). */
                DCHECK(event_loop_before_decided(ctx, s->layout_start, s->frame_ts) != 1,
                       "update the rendering step 15 recorded a style-and-layout start time BEFORE the "
                       "frameTimestamp its task was queued with — both come from the ONE virtual clock, and "
                       "step 20 records the interval between them");
                for (s->i = 0; s->i < s->ndocs; s->i++)
                    step_16(doc_realm(ctx, s));
                s->i = 0;
                STEP_GOTO(s->hdr.stage, UR_FOCUS, &s->cphase, &s->fphase, NULL);
                break;
            }
            docctx = doc_realm(ctx, s);
            if (!s->snapped) {                       /* §8.12 Animation frames step 2: the keys, snapshotted */
                s->nframe = animation_frame_snapshot(docctx);
                s->k = 0;
                s->snapped = 1;
            }
            if (s->k >= s->nframe) {
                animation_frame_run_end(docctx, s->nframe);
                s->snapped = 0;
                s->i++;
                continue;
            }
            if (s->cphase == 0 && JS_IsUndefined(s->fn)) {
                s->fn = animation_frame_take(docctx, s->k);   /* §8.12 Animation frames step 3, including its re-check */
                if (JS_IsUndefined(s->fn)) { s->k++; continue; }
            }
            {
                /* THE RELATIVE HIGH RESOLUTION TIME GIVEN frameTimestamp AND doc's RELEVANT GLOBAL OBJECT —
                   HR-TIME §4's operation, and the argument list of this step names it rather than describing
                   it, so it is COMPUTED here and not approximated. frameTimestamp is an UNSAFE moment (step 1
                   takes it from the event loop's last render opportunity time, and step 15 takes
                   unsafeStyleAndLayoutStartTime off the same clock); what a callback receives is that moment
                   coarsened and measured from ITS OWN document's time origin. This used to pass frameTimestamp
                   itself under a comment claiming it was already the relative time — equal only while every
                   environment's origin is zero, which stops being true the moment a page has a second
                   document, and wrong in exactly the direction that makes a child frame's animation clock run
                   ahead of the document it is in.
                   IT IS ASKED OF docctx, because the step says doc's relevant global object: a conversion done
                   in the machine's own realm would measure every child document's frame from the top-level
                   traversable's origin, which is the defect §3.7 exists to prevent one link up.
                   Web IDL §3.12 invokes a callback with thisArg UNDEFINED when none is given, which is what
                   §8.12 Animation frames gives; a sloppy callback still sees the global because that substitution is the
                   language's, and a strict one must see undefined. */
                JSValue now = hr_time_relative(docctx, s->frame_ts), out;
                JSValueConst argv[1];

                argv[0] = now;
                r = step_call_run(docctx, &s->cphase, STEP_CB(s->cb), s->fn, JS_UNDEFINED, 1, argv, cb_result,
                                  &out, out_cb, out_argc);
                JS_FreeValue(docctx, now);
                if (r > 0) return r;                 /* parked on the page's animation callback */
                /* §8.12 Animation frames invokes with "report": the callback's return value is discarded. */
                JS_FreeValue(docctx, out);
            }
            cb_result = JS_UNDEFINED;
            JS_FreeValue(docctx, s->fn);
            s->fn = JS_UNDEFINED;
            s->k++;
        }
    }

    if (s->hdr.stage == UR_FOCUS) {
        /* STEP 17, THE FOCUS FIXUP RULE: "For each doc of docs, if the focused area of doc is not a focusable
           area, then run the focusing steps for doc's viewport, and set doc's relevant global object's
           navigation API's focus changed during ongoing navigation to false."
           THE CONDITION IS ASKED AT EACH DOCUMENT'S TURN and never snapshotted for the walk, because repairing
           docs[0] can move the focus through docs[1]: the focusing steps designate a focused area in every
           document of the new focus chain, so a later document's answer is decided by an earlier one's repair.
           A snapshot taken up front would fix up a document the walk had already fixed.
           IT IS ITS OWN STAGE, AND ONE DOCUMENT PER REST, because the focusing steps fire `blur`, `focusout`,
           `focus` and `focusin` at the page's listeners — a stage that spanned two documents would be a stage
           the scheduler cannot preempt between. */
        for (;;) {
            if (s->i >= s->ndocs) {
                for (s->i = 0; s->i < s->ndocs; s->i++)
                    step_18(doc_realm(ctx, s));
                s->i = 0;
                s->obsnapped = 0;
                STEP_GOTO(s->hdr.stage, UR_INTERSECT, &s->cphase, &s->fphase, NULL);
                break;
            }
            docctx = doc_realm(ctx, s);
            if (s->cphase == 0) {
                if (focus_focused_area_is_focusable(docctx)) { s->i++; continue; }
                /* THE STEP'S SECOND HALF: "and set doc's relevant global object's NAVIGATION API's `focus
                   changed during ongoing navigation` to false" (HTML §7.2.6.8's flag). It is INSIDE the branch,
                   where the standard puts it — the repair and the clear are one conditional, and §6.6.4's focus
                   update steps step 4.1.1 is what SET the flag in the first place. */
                navigation_set_focus_changed(docctx, false);
            }
            r = focus_viewport_run(docctx, &s->cphase, STEP_CB(s->cb), cb_result, out_cb, out_argc);
            if (r > 0) return r;                     /* parked on the page's focus listeners */
            cb_result = JS_UNDEFINED;
            s->i++;
        }
    }

    if (s->hdr.stage == UR_INTERSECT) {
        /* STEP 19: "For each doc of docs, run the UPDATE INTERSECTION OBSERVATIONS steps for doc, passing in
           the relative high resolution time given now and doc's relevant global object as the timestamp."
           WHAT THE HTML TEXT CALLS `now` IS BOUND NOWHERE IN THIS ALGORITHM — the twenty-three steps bind
           `frameTimestamp` (step 1, the event loop's last render opportunity time) and
           `unsafeStyleAndLayoutStartTime` (step 15) and no `now` at all, which is checkable against the
           standard's own text and is an editorial defect in it rather than a value this engine is failing to
           produce. `frameTimestamp` is the moment passed: it is the one frame moment the algorithm binds and
           the one step 14 passes to the animation frame callbacks, so an entry's `time` and a rAF callback's
           argument name the same frame — which is what a bundle that measures both expects.
           THE CONVERSION IS NOT MADE HERE. §2.3 makes an entry's `time` relative to the time origin of "the
           global object associated with the IntersectionObserver INSTANCE", which is the observer's own realm
           and not `doc`'s — those differ for the ordinary case of an implicit-root observer constructed inside
           an iframe, whose root is the top-level browsing context's document. So the UNSAFE moment is handed
           over and §3.2.6 converts it against the realm that owns the entry.
           ONE OBSERVER PER REST, and not because author code runs — these steps run none, which is the whole
           point of the stage sitting here and not inside step 16's shape. It is because the walk is O(the
           page's observers x their targets) and every one of those is geometry: a machine that walks a
           structure of the PAGE'S SIZE has to be able to rest, and step 2 is the granularity §3.2.10 itself
           iterates at. */
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        for (;;) {
            if (s->i >= s->ndocs) {
                s->i = 0;
                STEP_GOTO(s->hdr.stage, UR_PAINT, &s->cphase, &s->fphase, NULL);
                break;
            }
            docctx = doc_realm(ctx, s);
            if (!s->obsnapped) {
                s->nobs = intersection_observer_count(docctx);
                s->ob = 0;
                s->obsnapped = 1;
            }
            if (s->ob >= s->nobs) {
                s->obsnapped = 0;
                s->i++;
                continue;
            }
            intersection_observer_update(docctx, s->ob++, s->frame_ts);
            return JS_STEP_YIELD;
        }
    }

    DCHECK(s->hdr.stage == UR_PAINT,
           "update the rendering resumed into a stage §8.1.7.3 does not have");
    /* STEPS 20-23 */
    for (s->i = 0; s->i < s->ndocs; s->i++)
        steps_20_to_23(doc_realm(ctx, s));
    JS_FreeValue(ctx, cb_result);
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_update_rendering_def = {
    sizeof(JSUpdateRendering), js_update_rendering_step, js_update_rendering_fini, 0,
    .catches_abrupt = 1,   /* §8.12 Animation frames step 3 and §2.9 invoke the page's code with "report": a throw is a VALUE */
    .visit = js_update_rendering_visit,
    .algorithm = "HTML §8.1.7.3 update the rendering — the rendering task source's task",
    .steps = UPDATE_RENDERING_STEPS
};

/* ---- the in-parallel loop --------------------------------------------------------------------------------- */

/* Its step 1, "wait until at least one navigable ... might have a rendering opportunity", over the same walk
   step 2 makes. It is asked BEFORE the clock moves, so it must not itself move it. */
static bool rendering_any_opportunity(JSContext *ctx)
{
    JSValue docs = rendering_collect_docs(ctx), len;
    uint32_t n = 0;

    len = JS_GetPropertyStr(ctx, docs, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    JS_FreeValue(ctx, docs);
    return n > 0;
}

int rendering_run_opportunity(JSContext *ctx)
{
    JSValue now, last, ms, next;
    JSContext *topctx;
    JSValue driver;

    if (!g_ready)
        return 0;
    if (!rendering_any_opportunity(ctx))
        return 0;                                    /* step 1: nothing might have one */
    now = event_loop_now(ctx);
    last = event_loop_last_render(ctx);
    ms = JS_NewFloat64(ctx, RENDERING_FRAME_MS);
    next = event_loop_moment_plus(ctx, last, ms);
    JS_FreeValue(ctx, ms);
    JS_FreeValue(ctx, last);
    /* HAS THE CLOCK ALREADY MOVED PAST THIS FRAME while work was due? — a comparison of two moments on one
       clock, and a FORK wherever either is unknown, which either can be once a timer with an unknown expiry
       has fired. `if (next < now) next = now;` read as arithmetic and was a DECISION: taking the arm where
       the frame is still ahead and the arm where it is behind are two different programs — in one the page's
       animation callbacks see the frame they asked for, in the other they see the moment the timer left. */
    if (event_loop_before(ctx, next, now)) {
        JS_FreeValue(ctx, next);
        next = JS_DupValue(ctx, now);
    }
    /* ONE CLOCK, TWO SOURCES. §8.1.7.3 step 2.1 gives the scheduler exactly one freedom — which task queue to
       take a task from — and it is not a freedom to run a task before the moment it becomes due. A timer that
       expires before the next frame is ahead of it on the same clock, so this yields and the timer source
       runs; the frame is still there to be taken on the next pass, at its own moment.
       THE COMPARISON IS ASKED OF THE TIMER SOURCE RATHER THAN COMPUTED FROM A MOMENT IT HANDS OVER. An expiry
       is not always a number — §8.7's `timeout` can be unknown external input — and where it is not, the order
       between the two sources is a FORK with two real arms rather than a value to read. Asked there, this
       caller gets the answer either way; asked here, it would have had a `double` that could not represent one
       of them. */
    if (timer_due_before(ctx, next)) {
        JS_FreeValue(ctx, next);
        JS_FreeValue(ctx, now);
        return 0;
    }
    /* AND THE OTHER SOURCE IS NOT ON THIS CLOCK AT ALL — the jump's own premise, asked because the frame is
       reached by MOVING the clock and §8.1.7.3 Processing model reaches it by WAITING: its in-parallel list
       step 1 is "Wait until at least one navigable ... might have a rendering opportunity", and every read of
       the unsafe shared current time in that section is a read. A reply the host still owes this flow is an
       event the wait would have been interrupted by and the jump cannot order, because it has no moment here;
       jumped anyway, the frame wins deterministically and the page's animation callbacks run at a moment
       manufactured ahead of an answer already in the socket. core/timing/event_loop.h owns the whole of that
       reasoning and this asks it.
       THE STRICTLY-AHEAD QUESTION IS ASKED RATHER THAN INFERRED, for the reason the clamp above gives about
       itself: over an unknown moment `next == now` and `next > now` are two different programs, and only the
       second manufactures time. Where the clamp fired, `next` IS `now` and this costs no predicate (a moment
       is not before itself). Where it did not, this is the read that discharges advance_to's premise
       invariant — a relation's key is its name and BOTH operands, so the clamp's `next < now` is a different
       predicate and cannot answer for it.
       NOTHING IS DROPPED: the frame is not consumed, `last render opportunity time` is not moved, and the
       honest answer is the one this function already spells for a timer that is due first — no opportunity
       ran. The ladder below finds the flow's outstanding reply and parks it host-owed, and the frame is taken
       on a later pass at its own moment, which is the wait the section states. */
    if (event_loop_before(ctx, now, next) && !event_loop_may_advance()) {
        JS_FreeValue(ctx, next);
        JS_FreeValue(ctx, now);
        return 0;
    }
    /* The timer source is the other thing due on this clock and it is NOT due before `next` — which is what
       the question above established, so nothing is already due to move past and the assert is told so
       (JS_UNDEFINED is "no other source is due", which the `-1` sentinel used to say). */
    event_loop_advance_to(ctx, next, JS_UNDEFINED);
    event_loop_set_last_render(ctx, next);           /* step 2 */
    JS_FreeValue(ctx, next);
    JS_FreeValue(ctx, now);

    /* STEP 3: "queue a global task on the rendering task source given that navigable's active window to update
       the rendering". The task is given the TOP-LEVEL TRAVERSABLE's window, which is whose driver runs the
       algorithm; the algorithm's own per-document steps then run in each document's realm. */
    {
        JSValue top = window_proxy_top_navigable(ctx, document_window_proxy(ctx));
        DCHECK(window_proxy_materialized(top),
               "the top-level traversable is not materialized — this document is IN it, so it is the one "
               "navigable that cannot be deferred");
        topctx = window_proxy_realm(ctx, top);
        JS_FreeValue(ctx, top);
    }
    driver = realm_value_get(topctx, g_driver_slot);
    DCHECK(JS_IsFunction(topctx, driver),
           "the rendering task source has no driver in this realm — rendering_install_driver is what mints "
           "one, and a realm without it would queue a task nothing can run");
    JS_EnqueueCallTask(topctx, driver, 0, NULL);
    JS_FreeValue(topctx, driver);
    return 1;
}

void rendering_init(JSContext *ctx)
{
    DCHECK(!g_ready, "rendering_init ran twice — one event loop per agent");
    g_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_update_rendering_def);
    CHECK(g_stepid >= 0, "no step id for update-the-rendering");
    g_driver_slot = realm_value_declare(ctx, "§8.1.7.3 updateTheRendering");
    g_ready = 1;
    realm_declare_intrinsic(rendering_install_driver);
    /* THE DRIVER MUST ASK, so it is told where to — registered exactly as the timer step is, and for the same
       reason: the scheduler may not depend on the browser half by name. A host that drives its own pump asks
       rendering_run_opportunity directly. */
    engine_set_rendering_hook(rendering_run_opportunity);
    agent_state_flag("rendering", &g_ready, "the declaration latch");
    agent_state_id("rendering", &g_stepid, "§8.1.7.3's update-the-rendering machine");
    agent_state_id("rendering", &g_driver_slot, "the per-realm slot the task source's driver is held in");
}

void rendering_install_driver(JSContext *ctx)
{
    JSValue fn;

    DCHECK(g_ready, "a realm asked for the rendering task source's driver before it was declared");
    /* Running twice in one realm is asserted by realm_value_set, which is where the first value is standing. */
    /* A step function nobody installs on any object: a page can neither see it nor replace it, and it carries
       THIS realm, which is what makes the task a GLOBAL task on this navigable's window. */
    fn = JS_NewCFunction2(ctx, NULL, "updateTheRendering", 0, JS_CFUNC_step, g_stepid);
    CHECK(!JS_IsException(fn), "the rendering task source's driver could not be allocated");
    realm_value_set(ctx, g_driver_slot, fn);
}

void rendering_free(JSRuntime *rt)
{
    (void)rt;
    /* NOT `if (!g_ready) return;`. The release is the inverse of the DECLARATION and rides the same row of
       core/platform.c's one list, whose declare pass is unconditional. */
    DCHECK(g_ready, "§8.1.7.3's rendering machinery was released in an agent that never declared it");
    g_ready = 0;
    /* §8.1.7.3'S IN-PARALLEL HALF IS GIVEN BACK BY THE COMPONENT THAT CLAIMED IT. The slot lives on the ONE
       frontier (solver/engine.c) and names `rendering_run_opportunity` in this file — the defect
       core/agent_state.h found in idb_transaction. solver_agent_free asserts it. */
    engine_set_rendering_hook(NULL);
    /* AND THE STEP ID, which this release used to keep. It names a registration in the runtime that is going
       away with it, and a second agent's declaration would have re-registered over the top of a number issued
       by a dead one — core/agent_state.h's fetch defect, asserted by the registry. */
    g_stepid = -1;
    g_driver_slot = -1;   /* the drivers are the REALMS' — each goes with its context */
}
