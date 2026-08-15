/* "UPDATE THE RENDERING" — HTML §8.1.7.3. See rendering.h for what this is, why it is not §14.3, and why it
   holds no loop of its own. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/dom/document.h"
#include "core/events/event_target.h"
#include "core/events/report_exception.h"
#include "core/frame/window_proxy.h"
#include "core/frame/navigable.h"
#include "core/css/media_query_list.h"
#include "core/dom/page_visibility.h"
#include "core/rendering/animation_frame.h"
#include "core/rendering/page_reveal.h"
#include "core/rendering/rendering.h"
#include "core/timing/timer.h"
#include "solver/engine.h"

/* THE UA'S REFRESH RATE. See rendering.h: the spec hands this to the user agent, so answering it is modelling
   rather than approximating. */
#define RENDERING_REFRESH_HZ 60.0
#define RENDERING_FRAME_MS   (1000.0 / RENDERING_REFRESH_HZ)

/* §8.1.7.1: the event loop's LAST RENDER OPPORTUNITY TIME. It belongs to the event LOOP, which is the agent's
   — one per similar-origin window agent — and so it lives beside the virtual clock rather than in the heap:
   it is not a document's state and no flow owns it, exactly as `timer_now()` is not a flow's clock. */
static double g_last_render_opportunity;
static int    g_stepid = -1, g_driver_slot = -1;
static int    g_ready;

/* ---- steps 2 to 5 ----------------------------------------------------------------------------------------
 *
 * A STEP OF §8.1.7.3 WHOSE SPECIFICATION IS NOT IN THIS BUILD. Twelve of the twenty-three steps are calls into
 * ANOTHER standard — CSSOM View's resize and scroll steps, Web Animations §4.4, Fullscreen, the canvas context
 * lost steps, Resize Observer §3.4, CSS View Transitions §7.2, Intersection Observer §3.2.2 — and this engine
 * has none of those standards. Their work-sets are therefore EMPTY BY CONSTRUCTION: the list a step drains is
 * filled by an interface that does not exist, so nothing can have filled it, and the page's own ReferenceError
 * on that interface is the forcing function §NO STUBS asks for.
 *
 * THAT IS AN ASSERTION AND NOT A SKIP, which is the whole reason this function exists rather than a comment at
 * each site. It is two-sided: `name` is the member whose arrival would give the step work, so the moment
 * somebody lands ResizeObserver, or a viewport, or Web Animations, THIS DCHECK fires — at the step of
 * update-the-rendering that must then be written, with the standard and the step number in the message. A
 * comment there would say the same thing and never fire. */
/* `path` is read from the doc's GLOBAL, dot by dot — `matchMedia`, `document.exitFullscreen`,
   `HTMLDialogElement.prototype.showModal`. A dotted path rather than a bare name because the INTERFACE OBJECT
   is not the producer: `HTMLDialogElement` exists in this build (html_element.c installs one per tag) while
   `showModal`, the member that puts an element in the TOP LAYER, does not — so naming the interface asserted
   something that was already true and fired on the first page. The producer is a MEMBER, and the path is how
   this says which one. UNDEFINED anywhere along it means the producer is absent; a member that EXISTS and
   answers null (`document.activeElement`) is present, so only undefined counts. */
/* THE LAST SEGMENT IS ASKED WITH [[HasProperty]], NOT [[Get]], and that is the whole of the difference between
   a probe and a call. This walked the path with JS_GetPropertyStr to its end and tested the VALUE, which is
   wrong twice over. It RUNS AN ACCESSOR — the page's code in a C activation with no flow base, which the engine
   aborts on by design — and `document.activeElement` became an accessor the moment HTML §6.6's focus model
   landed, so a probe that only ever wanted a yes or no took the entire run down with it. And a value is the
   wrong evidence anyway: a member whose getter legitimately answers `undefined` reads as ABSENT, so the check
   would go on believing a step had nothing to do long after its producer existed. [[HasProperty]] is the
   question being asked, and it invokes nothing.
   INTERMEDIATE segments still need their value to be traversed, and every one on every path here is an
   interface object or a `prototype` — both plain data properties. A path is written to keep that true
   (`Document.prototype.activeElement`, not `document.activeElement`): the interface object is where the member
   is DECLARED, which is also the more accurate question. Should an intermediate ever become an accessor, the
   engine's own getter abort names the property it stopped on. */
static void step_awaits(JSContext *ctx, const char *path, const char *what)
{
    JSValue cur = JS_GetGlobalObject(ctx);
    const char *p = path;
    bool present = false;

    for (;;) {
        const char *dot = strchr(p, '.');
        size_t n = dot ? (size_t)(dot - p) : strlen(p);
        char name[64];
        JSAtom atom;
        int has;

        DCHECK(n > 0 && n < sizeof(name), "an update-the-rendering step named a producer path this cannot read");
        if (!JS_IsObject(cur)) break;            /* the path died on the way; the producer is absent */
        memcpy(name, p, n);
        name[n] = 0;

        atom = JS_NewAtomLen(ctx, name, n);
        has = JS_HasProperty(ctx, cur, atom);
        CHECK(has >= 0, "an update-the-rendering producer probe threw — [[HasProperty]] over an engine global "
                        "runs no page code and has nothing to throw with");
        if (!has) { JS_FreeAtom(ctx, atom); break; }
        if (!dot) { JS_FreeAtom(ctx, atom); present = true; break; }   /* the member exists: producer is built */

        {
            JSValue next = JS_GetProperty(ctx, cur, atom);
            JS_FreeAtom(ctx, atom);
            JS_FreeValue(ctx, cur);
            cur = next;
        }
        p = dot + 1;
    }
    JS_FreeValue(ctx, cur);
    DCHECK(!present, what);
}

/* Does this document give §8.1.7.3 anything to do? Step 4 removes a doc "for which the user agent believes
   updating the rendering would have no visible effect AND whose map of animation frame callbacks is empty" —
   a document that has never been revealed has a visible effect pending by definition, so the two clauses are
   these two. Asked of the doc's OWN realm, because both pieces of state are per-Window. */
static bool doc_has_rendering_work(JSContext *docctx)
{
    return page_reveal_pending(docctx) || animation_frame_pending(docctx)
        || media_query_list_pending(docctx);
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
    X(UR_MEDIA,  "HTML §8.1.7.3 update the rendering step 10 (for each doc: evaluate media queries and report " \
                 "changes — CSSOM VIEW §4.2 fires `change` at each MediaQueryList whose matches state has "     \
                 "changed, in the order they were created, oldest first), one MediaQueryList per rest")         \
    X(UR_FRAMES, "HTML §8.1.7.3 update the rendering step 14 (for each doc: run the animation frame callbacks " \
                 "with the relative high resolution time of frameTimestamp — HTML §8.9), one callback per "     \
                 "rest")                                                                                       \
    X(UR_PAINT,  "HTML §8.1.7.3 update the rendering steps 15 and 19-23 (record the style-and-layout start "    \
                 "time; run the update intersection observations steps; record rendering time; mark paint "     \
                 "timing; update the rendering or user interface; process top layer removals)")
enum { UPDATE_RENDERING_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const UPDATE_RENDERING_STEPS[] = { UPDATE_RENDERING_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct JSUpdateRendering {
    JSStepHdr hdr;          /* FIRST — the driver writes the def and the operand bounds through it */
    JSValue   docs;         /* step 2's surviving list, as each document's WindowProxy (owned) */
    JSValue   ev;           /* the event a per-document step is holding across its dispatch (owned) */
    JSValue   target;       /* step 10's MediaQueryList, held across its dispatch (owned) */
    JSValue   fn;           /* step 14's callback, taken from the map and held across its call (owned) */
    EventFireCb   cb;        /* the request buffer: a fire needs [this, fn, target, event], a call three */
    double    frame_ts;     /* step 1 */
    double    layout_start; /* step 15's unsafeStyleAndLayoutStartTime */
    uint32_t  ndocs, i;     /* the cursor every "for each doc of docs" step shares */
    uint32_t  nframe, k;    /* §8.9 step 2's key snapshot for the current doc, and step 3's cursor */
    uint32_t  nmedia, m;    /* §4.2's collection snapshot for the current doc, and its cursor */
    uint8_t   fphase;       /* step 6's fire request */
    uint8_t   cphase;       /* step 14's call request */
    uint8_t   snapped;      /* §8.9 step 2 has been taken for docs[i] */
    uint8_t   msnapped;     /* §4.2's collection snapshot has been taken for docs[i] */
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
    report_exception_work_visit(ctx, &s->rep, v);
    STEP_CB_FOREACH(s->cb, k)
        v->val(ctx, &s->cb[k]);
}

static JSValue js_update_rendering_fini(JSContext *ctx, void *st, bool take_result)
{
    JSUpdateRendering *s = st;
    int k;

    (void)take_result;
    JS_FreeValue(ctx, s->docs);
    JS_FreeValue(ctx, s->ev);
    JS_FreeValue(ctx, s->target);
    JS_FreeValue(ctx, s->fn);
    JS_FreeValue(ctx, s->exc);
    report_exception_work_release(ctx, &s->rep);
    s->docs = s->ev = s->target = s->fn = s->exc = JS_UNDEFINED;
    STEP_CB_FOREACH(s->cb, k) {
        JS_FreeValue(ctx, s->cb[k]);
        s->cb[k] = JS_UNDEFINED;
    }
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

/* Steps 7 to 9, asserted in order at the place the algorithm runs them. */
static void steps_7_to_9(JSContext *docctx)
{
    step_awaits(docctx, "FocusEvent",
                "update the rendering step 7 flushes doc's AUTOFOCUS CANDIDATES (HTML §6.6), whose step 5.11.3 "
                "runs the focusing steps and fires blur/focusout/focus/focusin — this build now has focus "
                "events, so that list has a producer and step 7 must be written");
    step_awaits(docctx, "innerWidth",
                "update the rendering step 8 runs the RESIZE STEPS (CSSOM VIEW §13.1), which fire `resize` at "
                "the Window when the viewport's width or height changed since the last run — this build now "
                "models a viewport, so step 8 must compare it and fire");
    step_awaits(docctx, "scrollTo",
                "update the rendering step 9 runs the SCROLL STEPS (CSSOM VIEW §13.2) over doc's pending "
                "scroll events, firing scroll/scrollend/scrollsnapchange in the order they were added — this "
                "build now has a way to scroll a scrolling box, so that list has a producer and step 9 must be "
                "written");
}

/* STEP 10 IS WRITTEN — it is UR_MEDIA, below. Its assert is gone rather than relaxed, which is the whole point
   of the mechanism: the producer (`matchMedia`) arrived, the DCHECK fired at this exact place in the order, and
   the step it named was built here.

   Steps 11 to 13, asserted in order at the place the algorithm runs them. */
static void steps_11_to_13(JSContext *docctx)
{
    step_awaits(docctx, "Animation",
                "update the rendering step 11 UPDATES ANIMATIONS AND SENDS EVENTS (WEB ANIMATIONS §4.4): "
                "update the timelines, remove replaced animations, PERFORM A FULL MICROTASK CHECKPOINT (§4.4 "
                "step 3, inside this step), stable-sort the pending animation events and dispatch each — this "
                "build now has Animation, so step 11 must be written, and its internal checkpoint cannot be "
                "one stage with anything around it");
    step_awaits(docctx, "Document.prototype.exitFullscreen",
                "update the rendering step 12 runs the FULLSCREEN STEPS, firing fullscreenchange/"
                "fullscreenerror and resolving requestFullscreen()'s promise — this build now has fullscreen, "
                "so step 12 must be written");
    step_awaits(docctx, "CanvasRenderingContext2D",
                "update the rendering step 13 runs the CONTEXT LOST STEPS for each 2D context whose backing "
                "storage was lost: reset the context, fire `contextlost` CANCELABLE (its return value steers "
                "the algorithm), and fire `contextrestored` on a successful restore — this build now has a 2D "
                "context, so step 13 must be written");
}

/* Steps 16 to 18, likewise. */
static void steps_16_to_18(JSContext *docctx)
{
    step_awaits(docctx, "ResizeObserver",
                "update the rendering step 16 is a `while (true)` around recalculate-styles-and-update-layout "
                "that GATHERS ACTIVE RESIZE OBSERVATIONS at an increasing depth and BROADCASTS them (RESIZE "
                "OBSERVER §3.4.1/§3.4.5), re-entering style and layout after every author callback, then "
                "delivers the resize loop error (§3.4.6) for any skipped observation — this build now has "
                "ResizeObserver, so step 16 must be written, and its loop cannot be one stage");
    step_awaits(docctx, "Document.prototype.activeElement",
                "update the rendering step 17 runs the FOCUSING STEPS for doc's viewport when doc's focused "
                "area is no longer focusable — the spec's own note is that this usually fires `blur` and "
                "possibly `change` — this build now tracks a focused area, so step 17 must be written");
    step_awaits(docctx, "ViewTransition",
                "update the rendering step 18 PERFORMS PENDING TRANSITION OPERATIONS (CSS VIEW TRANSITIONS "
                "§7.2): setup view transition calls the author's ViewTransitionUpdateCallback and settles "
                "ready/updateCallbackDone/finished — this build now has ViewTransition, so step 18 must be "
                "written");
}

/* Steps 19 to 23. Step 19 is the one it is easiest to get backwards and the one SPEC_STEPS.md flags: the
   update intersection observations steps run NO author callbacks — they compute geometry, QUEUE an
   IntersectionObserverEntry and QUEUE A TASK on the IntersectionObserver task source, and the observer's
   callback is invoked from THAT later task (§3.2.5 step 3.5). That is the opposite of ResizeObserver in step
   16, and swapping them changes observable ordering. */
static void steps_19_to_23(JSContext *docctx)
{
    step_awaits(docctx, "IntersectionObserver",
                "update the rendering step 19 runs the UPDATE INTERSECTION OBSERVATIONS steps (INTERSECTION "
                "OBSERVER §3.2.2): compute each target's intersection rectangle, decide its threshold index "
                "and QUEUE an IntersectionObserverEntry, which QUEUES A TASK on the IntersectionObserver task "
                "source — the callbacks run from that task and NOT from this step. This build now has "
                "IntersectionObserver, so step 19 must be written that way round");
    step_awaits(docctx, "PerformanceObserver",
                "update the rendering steps 20 and 21 RECORD RENDERING TIME and MARK PAINT TIMING, which queue "
                "performance entries — this build now has a performance timeline to queue them on, so both "
                "steps must be written");
    /* STEP 22 — "update the rendering or user interface of doc and its node navigable to reflect the current
       state". THE PAINT, and the only step of the twenty-three with no headless equivalent: everything before
       it computes values that exist whether or not anything is drawn, and this one draws. There is no
       scriptable result for it to produce and no interface whose arrival would give it one, so it is a
       documented no-effect rather than an assertion — the one place in this component where that is the
       honest answer. */
    step_awaits(docctx, "HTMLDialogElement.prototype.showModal",
                "update the rendering step 23 PROCESSES TOP LAYER REMOVALS, and the top layer is filled by "
                "`dialog.showModal()` and by fullscreen — this build now has showModal, so the top layer has a "
                "producer and step 23 must be written");
}

static int js_update_rendering_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSUpdateRendering *s = st;
    JSContext *docctx;
    int r, k;

    /* THE PAGE'S CODE THREW, AND THAT IS NOT THIS TASK'S COMPLETION. §8.9 invokes an animation frame callback
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
        } else if (s->hdr.stage == UR_FRAMES) {
            s->cphase = 0;
            JS_FreeValue(ctx, s->fn);
            s->fn = JS_UNDEFINED;
            s->k++;                      /* §8.9 removed the callback before invoking it: on to the next */
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
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
        /* The report's own record, zeroed before anything can throw — a step state arrives with the INTEGER 0
           in every JSValue slot, and the report's teardown frees exactly what it holds. */
        report_exception_work_start(&s->rep);
        s->reporting = 0;
        s->i = s->ndocs = s->nframe = s->k = s->nmedia = s->m = 0;
        s->fphase = s->cphase = s->snapped = s->msnapped = 0;
        s->hdr.stage = UR_REVEAL;
        /* STEP 1: frameTimestamp is the event loop's LAST RENDER OPPORTUNITY TIME — the moment the in-parallel
           loop recorded when it decided there was one, not the moment this task happens to run. */
        s->frame_ts = g_last_render_opportunity;
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
                for (s->i = 0; s->i < s->ndocs; s->i++)
                    steps_7_to_9(doc_realm(ctx, s));
                s->i = 0;
                s->msnapped = 0;
                s->hdr.stage = UR_MEDIA;
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
                s->hdr.stage = UR_FRAMES;
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
           resolution time of frameTimestamp." HTML §8.9's own three steps are the inner loop. */
        for (;;) {
            if (s->i >= s->ndocs) {
                for (s->i = 0; s->i < s->ndocs; s->i++)
                    steps_16_to_18(doc_realm(ctx, s));
                s->i = 0;
                s->hdr.stage = UR_PAINT;
                break;
            }
            docctx = doc_realm(ctx, s);
            if (!s->snapped) {                       /* §8.9 step 2: the keys, snapshotted */
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
                s->fn = animation_frame_take(docctx, s->k);   /* §8.9 step 3, including its re-check */
                if (JS_IsUndefined(s->fn)) { s->k++; continue; }
            }
            {
                /* THE RELATIVE HIGH RESOLUTION TIME OF frameTimestamp — a real DOMHighResTimeStamp off the one
                   virtual clock, not a placeholder: §Headless's missing piece is a display, and the moment a
                   frame is served exists without one. Web IDL §3.12 invokes a callback with thisArg UNDEFINED
                   when none is given, which is what §8.9 gives; a sloppy callback still sees the global
                   because that substitution is the language's, and a strict one must see undefined. */
                JSValue now = JS_NewFloat64(docctx, s->frame_ts), out;
                JSValueConst argv[1];

                argv[0] = now;
                r = step_call_run(docctx, &s->cphase, STEP_CB(s->cb), s->fn, JS_UNDEFINED, 1, argv, cb_result,
                                  &out, out_cb, out_argc);
                JS_FreeValue(docctx, now);
                if (r > 0) return r;                 /* parked on the page's animation callback */
                /* §8.9 invokes with "report": the callback's return value is discarded. */
                JS_FreeValue(docctx, out);
            }
            cb_result = JS_UNDEFINED;
            JS_FreeValue(docctx, s->fn);
            s->fn = JS_UNDEFINED;
            s->k++;
        }
    }

    DCHECK(s->hdr.stage == UR_PAINT,
           "update the rendering resumed into a stage §8.1.7.3 does not have");
    /* STEP 15: unsafeStyleAndLayoutStartTime. It is the moment step 20's "record rendering time" measures
       from, so it is READ here from the one clock rather than invented at the step that consumes it — and the
       two are asserted against each other, which is the only thing that makes reading it worth anything while
       the consumer is absent. */
    s->layout_start = timer_now();
    DCHECK(s->layout_start >= s->frame_ts,
           "update the rendering step 15 recorded a style-and-layout start time BEFORE the frameTimestamp its "
           "task was queued with — both come from the ONE virtual clock, and step 20 records the interval "
           "between them");
    /* STEPS 19-23 */
    for (s->i = 0; s->i < s->ndocs; s->i++)
        steps_19_to_23(doc_realm(ctx, s));
    JS_FreeValue(ctx, cb_result);
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_update_rendering_def = {
    sizeof(JSUpdateRendering), js_update_rendering_step, js_update_rendering_fini, 0,
    .catches_abrupt = 1,   /* §8.9 step 3 and §2.9 invoke the page's code with "report": a throw is a VALUE */
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
    double now, next, due;
    JSContext *topctx;
    JSValue driver;

    if (!g_ready)
        return 0;
    if (!rendering_any_opportunity(ctx))
        return 0;                                    /* step 1: nothing might have one */
    now = timer_now();
    next = g_last_render_opportunity + RENDERING_FRAME_MS;
    if (next < now)
        next = now;                                  /* the clock moved past this frame while work was due */
    /* ONE CLOCK, TWO SOURCES. §8.1.7.3 step 2.1 gives the scheduler exactly one freedom — which task queue to
       take a task from — and it is not a freedom to run a task before the moment it becomes due. A timer that
       expires before the next frame is ahead of it on the same clock, so this yields and the timer source
       runs; the frame is still there to be taken on the next pass, at its own moment. */
    due = timer_next_due();
    if (due >= 0 && due < next)
        return 0;
    timer_advance_to(next);
    g_last_render_opportunity = next;                /* step 2 */

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
    g_last_render_opportunity = 0;
    g_ready = 1;
    realm_declare_intrinsic(rendering_install_driver);
    /* THE DRIVER MUST ASK, so it is told where to — registered exactly as the timer step is, and for the same
       reason: the scheduler may not depend on the browser half by name. A host that drives its own pump asks
       rendering_run_opportunity directly. */
    engine_set_rendering_hook(rendering_run_opportunity);
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

void rendering_free(JSContext *ctx)
{
    (void)ctx;
    if (!g_ready) return;
    g_ready = 0;
    g_driver_slot = -1;
    g_last_render_opportunity = 0;   /* the drivers are the REALMS' — each goes with its context */
}
