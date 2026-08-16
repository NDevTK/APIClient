/* FIRING THE NAVIGATE EVENT — HTML §7.2.6.10.4. See navigate_event_fire.c.
 *
 * WHAT IT IS. §7.2.6.10.1's NavigateEvent and §7.2.6.10.3's NavigationDestination are two objects with no
 * producer until this: every navigation a Document can see dispatches ONE navigate event at its Navigation
 * before it commits, and this is the algorithm that builds it, fires it, and answers whether the navigation
 * that asked may proceed. A router's `navigate` listener is the page's chance to say no — `preventDefault()`
 * on a cancelable one — so the answer is a real boolean and not a formality.
 *
 * IT IS A STEP MACHINE, AND ITS REST POINT IS THE DISPATCH. §2.9's dispatch runs the page's own listeners, so
 * the algorithm cannot be a C call: it parks at step 27 and resumes with dispatchResult in hand, exactly as
 * core/events/report_exception.c parks on `error` and core/frame/navigation.c parks on `currententrychange`.
 * The work record is the CALLING machine's — it visits it (so a fork copies it and a suspension inside a
 * `navigate` listener resumes in the same stage) and its declaration is what releases it.
 *
 * ITS INPUTS RIDE THE RECORD, WHICH IS NOT A CONVENIENCE. §7.2.5's `pushState` computes newURL and
 * serializedData, fires this, and only then runs §7.4.4's URL and history update steps — so between the values
 * being computed and being used the page's listeners run, and every one of them can navigate, replace the
 * document's address, or push another entry. An algorithm that read the destination back off the navigable
 * when the job resumed would resolve `pushState("/a")` against whatever the listener left behind. So
 * `_begin` takes the URL, the same-document flag and the classic history API state WITH the operation at the
 * moment it is created, as a JS string and an ArrayBuffer that park with the flow, and the machine reads
 * nothing else afterwards. That is CLAUDE.md's §7.4-step-14 rule at its own scale.
 *
 * ONE WRAPPER OF THE THREE, AND WHICH ONE IS DECIDED BY WHO CALLS IT. §7.2.6.10.4 declares three — a traverse,
 * a push/replace/reload, and a download request — over one inner algorithm. Only the push/replace/reload one is
 * here, because a wrapper's whole content is WHICH values it takes with it, and that is checkable only at a
 * call site: core/frame/history.c's shared push/replace state steps 7-9 are this one's, and they land with it.
 * The other two are named at the sites that owe them — core/frame/session_history.c's §7.4.6.1 step 5 for the
 * traverse, and a download-initiating navigation for the third, which this build has no algorithm for at all.
 *
 * BOTH OF ITS ENDINGS ARE NOW BUILT. §7.2.6.8's ABORT THE ONGOING NAVIGATION is core/frame/navigation_abort.c,
 * and the two paths that reach it — the wrapper's step 2, where a second navigation supersedes one still
 * ongoing, and inner step 28, where a listener called preventDefault() — DRIVE it rather than crashing at it.
 * Each is a stage of this machine, because aborting fires `abort` at the event's signal and `navigateerror` at
 * the Navigation and both of those are the page's own code.
 *
 * WHAT IS STILL ABSENT AND WHERE IT CRASHES. §7.2.6.10.1's `intercept()` is not installed
 * (core/events/navigate_event.h says why), so the event's INTERCEPTION STATE can never leave "none": steps
 * 29-31, §7.2.6.10.5's finish, the transition and the precommit controller are all unreachable, and each site
 * asserts that against `NavigateEvent.prototype.intercept` rather than carrying a field with one writer. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGATE_EVENT_FIRE_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGATE_EVENT_FIRE_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/events/event_target.h"   /* EventFireCb — the width of the fire request this work parks on */
#include "core/frame/navigation_abort.h"  /* §7.2.6.8's abort, which two of this algorithm's steps perform */
#include "core/structured_clone.h"

/* Declared once per AGENT: the step definition of the commit handler job below, which is a machine the runtime
   must know before anything enqueues one. It builds no per-realm object — every object this algorithm makes is
   the running flow's — so there is no install half. */
void navigate_event_fire_init(JSContext *ctx);
void navigate_event_fire_free(void);

/* THE OPERATION, ONCE IT HAS BEEN CREATED. Every field is either an input taken WITH it or something the
   algorithm built out of those inputs; nothing here is read back off the navigable when the machine resumes. */
typedef struct {
    uint8_t     stage;           /* NEF_STAGES in navigate_event_fire.c — which step of §7.2.6.10.4 this is at */
    uint8_t     phase;           /* event_target_fire_run's, for the dispatch in flight */
    /* §7.2.6.3's NavigationType. A static string of the standard's own value space, exactly as
       core/frame/session_history.c's SHApply holds one: the values are "push", "replace", "reload" and
       "traverse" and no other string can ever be here. */
    const char *navigation_type;
    bool        same_document;   /* the wrapper's isSameDocument, which becomes destination's is same document */
    JSValue     url;             /* the destination URL, serialized (owned string) */
    JSValue     classic;         /* §7.2.6.10.1's classic history API state: the bytes (ArrayBuffer) or JS_NULL */
    JSValue     destination;     /* §7.2.6.10.3's, built at step 7 of the wrapper (owned) */
    JSValue     event;           /* the NavigateEvent, held across the dispatch (owned) */
    /* §7.2.6.8's ABORT, which this algorithm performs at TWO of its steps — the wrapper's step 2 (as the loop)
       and inner step 28.2 (as the single abort). ONE record for both, because the two are steps of the same
       navigation and can never be in flight at the same moment: step 2 has answered before the event step 28
       is about has been built. */
    NavigationAbortWork abort;
    EventFireCb cb;
} NavigateEventFireWork;

void navigate_event_fire_work_start(NavigateEventFireWork *w);
void navigate_event_fire_work_visit(JSContext *ctx, NavigateEventFireWork *w, JSStepVisit *v);
void navigate_event_fire_work_release(JSContext *ctx, NavigateEventFireWork *w);

/* §7.2.6.10.4's FIRE A PUSH/REPLACE/RELOAD NAVIGATE EVENT, at the moment the operation is CREATED — it takes
 * its values and performs nothing else, because every step of the wrapper past its arguments is a step that can
 * observe the page's state and therefore belongs to the machine's own first stage.
 *   `navigation_type` is one of §7.2.6.3's four and must be a static string (it outlives a park).
 *   `destination_url` is the wrapper's destinationURL, already serialized; COPIED.
 *   `is_same_document` is the wrapper's isSameDocument.
 *   `classic_state` is its classicHistoryAPIState — the serialized bytes a `pushState` carries, or NULL for
 *      every navigation that has none. BORROWED; the bytes are copied.
 * THE WRAPPER'S OTHER FIVE ARGUMENTS ARE AT THE STANDARD'S OWN DEFAULTS and are not parameters, because a
 * parameter whose only caller passes the default is a field with one writer: userInvolvement is "none" (a
 * script called this, not the browser UI), sourceElement is null (no link, form or submit button is
 * responsible), formDataEntryList is null (a POST form submission is what fills it), navigationAPIState is
 * StructuredSerializeForStorage(null), and apiMethodTracker is null (§7.2.6.7's `navigate`/`reload` are its
 * only producers). Each becomes a parameter with the caller that passes something else. */
void navigate_event_fire_push_replace_reload_begin(JSContext *ctx, NavigateEventFireWork *w,
                                                   const char *navigation_type, const char *destination_url,
                                                   bool is_same_document, const StructuredData *classic_state);

/* THE ALGORITHM, DRIVEN. JS_STEP_CALL or JS_STEP_YIELD = return it (it has parked on the page's `navigate`
   listeners, or at one of its own rest points), 0 = it has ANSWERED and `*pcontinue` holds §7.2.6.10.4's
   return value — false meaning the navigation that asked must not proceed. JS_STEP_ABRUPT = it threw. */
int navigate_event_fire_run(JSContext *ctx, NavigateEventFireWork *w, JSValue in, JSValue **out_cb,
                            int *out_argc, bool *pcontinue);

/* §7.2.6.10.4's NAVIGATE EVENT INTERCEPT COMMIT HANDLER STEPS, whose one caller is §7.2.6.4's
   update-the-navigation-API-entries step 14. They are what ENDS a navigate event: they clear the Navigation's
   ongoing navigate event and fire `navigatesuccess`, and they do it in a MICROTASK rather than in the caller's
   turn, because the standard reaches them through "wait for all". `event` is BORROWED. */
void navigate_event_intercept_commit(JSContext *ctx, JSValueConst event);

#endif
