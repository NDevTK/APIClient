/* THE NAVIGATION API — HTML §7.2.6. See navigation.c.
 *
 * WHAT IT IS AND WHY IT IS NOT core/frame/history.c. §7.2.6.1 calls it "a successor to the classic location and
 * history APIs", and the difference that matters to this engine is not the spelling of the methods: the classic
 * History object exposes a LENGTH and an INDEX and nothing else, while the navigation API exposes THE ENTRIES
 * THEMSELVES. `navigation.entries()` hands a page a NavigationHistoryEntry per same-origin contiguous session
 * history entry, each with its URL, its key, its id and its own state — so a router can read where it has been,
 * and a great deal of shipped code does exactly that on boot.
 *
 * WHAT IT IS BUILT OVER. Every one of those entries is a VIEW over one of §7.4.1.1's session history entries,
 * which core/frame/session_history.c already holds; this component adds the three fields §7.4.1.1 gives an
 * entry FOR this interface (its navigation API state, key and id), the LIST of wrappers over them, and the
 * events that fire when the list changes. There is no second history here, and there must never be one: two
 * lists over one session history is the one-fact-two-answers defect, and §7.2.6.4's whole job is to keep the
 * one list it does own in step with the entries the session history already moved.
 *
 * THE ENTRY LIST IS PER-FLOW STATE AND IT TIME-TRAVELS, for the reason session_history.h gives for the entries
 * themselves: two forked flows push different entries and neither may see the other's. So the list is an
 * ordinary JS Array on the Navigation object's own slot record — every append, every index assignment and every
 * truncation is a property write the heap COW delta already captures, and a half-built list parks to the IDB
 * cold tier and resumes with the flow that built it.
 *
 * UPDATING THE LIST RUNS THE PAGE'S CODE, WHICH IS WHY IT IS A REQUEST. §7.2.6.4 fires `currententrychange` at
 * the Navigation and then `dispose` at every entry the update threw away — two §2.9 dispatches — so the
 * algorithm cannot be a plain C call, and the two callers that reach it (§7.4.4's URL and history update steps
 * and §7.4.6.2's update-document-for-history-step-application) are step machines because of it. That is a real
 * consequence and not a formality: `history.pushState()` past the end of the forward history disposes every
 * entry beyond it, and a page with an `ondispose` handler runs code in the middle of a pushState.
 *
 * THE NAVIGATE EVENT IS FIRED NOW, and by core/frame/navigate_event_fire.c rather than by this file: §7.2.6.10.4
 * is its own algorithm with its own rest points, and this component is what it reads and writes (the entry
 * list, the current entry, and §7.2.6.8's ongoing navigate event below). §7.2.5's `pushState` and
 * `replaceState` are its one caller so far; core/frame/session_history.c's §7.4.6.1 step 5 still carries a
 * realm_awaits naming the TRAVERSE wrapper it owes.
 *
 * WHAT IS NOT HERE YET IS §7.2.6.7's INITIATING NAVIGATIONS AND THE INTERCEPTION HALF OF §7.2.6.10 —
 * `navigate`, `reload`, `traverseTo`, `back`, `forward`, `transition`, `activation`, `onnavigateerror`,
 * `NavigateEvent.intercept()`/`scroll()`, and the NavigationPrecommitController / NavigationTransition /
 * NavigationActivation interfaces they are made of. They are ABSENT rather than stubbed, so a page's own
 * TypeError names them, and every step of another algorithm that would reach them asserts against the member's
 * arrival instead of saying so in a comment — core/frame/navigate_event_fire.c carries two such assertions over
 * `NavigateEvent.prototype.intercept` and two DFAILs over §7.2.6.8's abort-the-ongoing-navigation.
 * §7.2.6.7's methods additionally need what this build has no algorithm for at all — `navigate` and `reload a
 * navigable` — which core/frame/history.c's `go(0)` already crashes on by name. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGATION_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_NAVIGATION_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/events/event_target.h"   /* EventFireCb — the width of the fire request this work parks on */

void navigation_init(JSContext *ctx);
/* §3.7: THIS REALM's Navigation, its prototype and its interface object — declared into core/realm.h's list. */
void navigation_install_realm(JSContext *ctx);
void navigation_free(JSContext *ctx);

/* §7.2.6.2: "Each Window has an associated NAVIGATION API, which is a Navigation object." OWNED. */
JSValue navigation_object(JSContext *ctx);

/* §7.2.6.3's HAS ENTRIES AND EVENTS DISABLED, of THIS realm's Navigation. Every clause of it is about the
   Document — not fully active, the initial about:blank, an opaque origin — and it is the FIRST step of both
   §7.2.6.10.4's push/replace/reload wrapper and its inner algorithm, which is why the question leaves this
   file: two derivations of one rule is where the second one goes wrong, and this one would go wrong on the
   opaque-origin clause, which is asked through §7.2.5.1 rather than by comparing strings. */
bool navigation_entries_and_events_disabled(JSContext *ctx);

/* §7.2.6.8's ONGOING NAVIGATE EVENT — "a NavigateEvent or null, initially null" — of THIS realm's Navigation.
 *
 * IT IS THE ONE PIECE OF §7.2.6.8's ONGOING-NAVIGATION STATE THIS BUILD HOLDS, and it is here rather than in
 * core/frame/navigate_event_fire.c for the reason the focus-changed flag below is: the state belongs to the
 * Navigation, so it rides the Navigation's own slot record and the per-flow COW delta captures it as a property
 * write — two forked flows navigate independently and neither sees the other's ongoing event.
 *   Its writers are §7.2.6.10.4's inner algorithm step 24 (which sets it to the event it is about to dispatch)
 * and its commit handler success steps step 4 (which null it out), plus §7.2.6.8's abort when that is built.
 * Its readers are the wrapper's step 2, the inner algorithm's step 23 assert, and §7.2.6.4 step 10.
 * The getter is OWNED and answers JS_NULL for the standard's null; the setter takes JS_NULL for it. */
JSValue navigation_ongoing_navigate_event(JSContext *ctx);
void    navigation_set_ongoing_navigate_event(JSContext *ctx, JSValueConst ev);

/* §7.2.6.3's GET THE NAVIGATION API ENTRY INDEX of a session history entry within THIS realm's Navigation —
   the index of the NavigationHistoryEntry whose session history entry is `she`, or −1 when the list holds
   none. It is what §7.2.6.5's `index` returns and what §7.2.6.4 sets the current entry index from. */
int64_t navigation_entry_index_of(JSContext *ctx, JSValueConst she);

/* §7.2.6.4's INITIALIZE THE NAVIGATION API ENTRIES FOR A NEW DOCUMENT, given the list of session history
   entries §7.4.1.4's get-session-history-entries-for-the-navigation-API produced and the entry the document is
   being activated with. It runs NONE of the page's code — it mints wrappers and sets an index — which is why
   it is a plain call where the update below is a request.
   Its two asserts are the standard's own: the entry list is empty and the current entry index is −1, so this
   is the ONE moment in a Document's life it may run. `new_shes` and `initial_she` are BORROWED. */
void navigation_initialize_entries(JSContext *ctx, JSValueConst new_shes, JSValueConst initial_she);

/* §7.2.6.4's UPDATE THE NAVIGATION API ENTRIES FOR A SAME-DOCUMENT NAVIGATION, AS A REQUEST.
 *
 * The work record is the CALLING machine's, like every other request in this engine: the caller visits it (so a
 * fork copies it and a suspension inside a `dispose` listener resumes in the same stage) and the caller
 * releases it. `stage` is a SUB-SEQUENCE cursor inside ONE of the caller's stages — the caller's own label
 * names this whole algorithm, exactly as core/dom/abort.h's signal-abort request is named at its callers.
 *   JS_STEP_CALL = return it, 0 = the algorithm has finished, -1 = it threw. */
typedef struct {
    uint8_t     stage;
    uint8_t     phase;      /* event_target_fire_run's, for whichever dispatch is in flight */
    uint32_t    i;          /* how far through disposedNHEs the step-13 walk is */
    JSValue     disposed;   /* §7.2.6.4 step 3's disposedNHEs (owned) */
    JSValue     ev;         /* the event held across the dispatch (owned) */
    /* §7.2.6.4 STEP 10's navigateEvent, READ AT STEP 10 AND USED AT STEP 14 (owned; JS_NULL for none). It is a
       field and not a read at the step that needs it because the standard reads it early on purpose: steps 12
       and 13 dispatch `currententrychange` and then `dispose` at each thrown-away entry, and any of those
       listeners can start a navigation — which replaces the Navigation's ongoing navigate event with a
       different one. Ending THAT navigation at step 14 would end a navigation that has not been committed. */
    JSValue     navigate_event;
    EventFireCb cb;
} NavigationUpdateWork;

void navigation_update_work_start(NavigationUpdateWork *w);
void navigation_update_work_visit(JSContext *ctx, NavigationUpdateWork *w, JSStepVisit *v);
void navigation_update_work_release(JSContext *ctx, NavigationUpdateWork *w);
/* `destination_she` is the session history entry the navigation arrived at; `navigation_type` is one of
   §7.2.6.3's NavigationType values and is never null here (§7.4.6.2 step 6.4.1 asserts that separately).
   Both are BORROWED and must stay valid across the suspension — the caller holds the entry, and the type is a
   static string of the standard's own value space. */
int navigation_update_entries_run(JSContext *ctx, NavigationUpdateWork *w, JSValueConst destination_she,
                                  const char *navigation_type, JSValue in, JSValue **out_cb, int *out_argc);

/* §7.2.6.8's FOCUS CHANGED DURING ONGOING NAVIGATION — a boolean on the Navigation, set by HTML §6.6.4's focus
   update steps step 4.1.1 when they designate a focused area, and cleared by §8.1.7.3's update-the-rendering
   step 17 when it repairs one. It is a field of the navigation API rather than of the focus model, which is
   why it is reached from core/html/focus.c and core/rendering/rendering.c through here. */
void navigation_set_focus_changed(JSContext *ctx, bool changed);

#endif
