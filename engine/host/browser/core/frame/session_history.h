/* HTML §7.4.1's SESSION HISTORY, §7.4.3's TRAVERSAL, and the TWO SYNCHRONOUS HISTORY UPDATES — §7.4.4's URL
 * and history update steps and §7.4.2.3.3's navigate to a fragment — see session_history.c.
 *
 * WHAT WAS HERE BEFORE was nothing at all, and document_lifecycle.c said so at two of its steps: "STEP 9 IS
 * SESSION HISTORY, which this engine does not hold: there is no session history entry to null a document out
 * of". That absence is not a missing accessor. EVERY client-side router — React Router, Vue Router, Angular's,
 * and every hand-rolled one — changes route by calling `history.pushState()`, so a bundle that routes threw on
 * its first navigation and every route it could reach, every lazily-loaded chunk behind a route, and every
 * endpoint those chunks call was unreachable. It is the largest reachability gap the browser half had.
 *
 * IT IS A STATE MACHINE, NOT A STACK OF URLS. §7.4.1.1's session history entry is a struct — a step, a URL, a
 * DOCUMENT STATE, a serialized classic-history-API state, a scroll restoration mode — and the traversable holds
 * the entries and a CURRENT SESSION HISTORY STEP that indexes them. The document state is the part a URL stack
 * cannot express and the part §7.4.4 leans on: contiguous entries SHARE one document state ("all entries that
 * share the same document state — and that are therefore merely different states of one particular document —
 * are contiguous by construction"), which is exactly what makes ten pushState calls ten entries of ONE Document
 * rather than ten documents.
 *
 * THE HISTORY IS PER-FLOW STATE AND IT TIME-TRAVELS. Two forked flows can push different entries and neither
 * may see the other's — a router's `if (flags.admin) history.pushState(...)` forks, and the two arms are two
 * different histories from that line on. So none of this is a malloc'd list: CLAUDE.md's §State-isolation says
 * what happens to one ("a malloc'd list captured as head/tail POINTERS reverts the POINTERS on a context switch
 * and leaves the nodes reachable from nothing: a leak the runtime's own GC walk cannot see"), and the entries
 * are queued platform data in exactly the sense that rule is about. They are ORDINARY JS OBJECTS in a JS ARRAY
 * on a per-realm record — the shape §6.6.7's autofocus candidates and §6.4's activation timestamps already use
 * — so every append, every step assignment and every replacement is a PROPERTY WRITE the heap COW delta already
 * captures, and a half-built history parks to the IDB cold tier and resumes with the flow that built it.
 *
 * A TRAVERSAL IS A JOB, AND IT SUSPENDS. §7.4.3 puts every step of traverse-the-history-by-a-delta after the
 * third inside "APPEND THE FOLLOWING SESSION HISTORY TRAVERSAL STEPS to traversable", so `history.back()`
 * schedules the traversal and does not perform it — and §7.4.6.2 then FIRES popstate at the page's own
 * listeners, which is a §2.9 dispatch and therefore the page's code. So the traversal is a step machine on the
 * ONE frontier: it parks on the dispatch, siblings run while a listener's loop or `await` is in flight, and it
 * resumes at the spec step it rested at. §7.4.6.1's apply-the-history-step is ONE body with two entry points —
 * the push/replace one cannot park and takes the update-only exit, which by the standard's own note is why
 * `popstate` does not fire for `pushState`.
 *
 * THE NAVIGATION API IS §7.2.6 AND IT LIVES NEXT DOOR, in core/frame/navigation.c — but its three fields on
 * §7.4.1.1's entry are HERE, because they are fields of the entry: its navigation API STATE, KEY and ID. This
 * component owns them, hands §7.2.6.5's NavigationHistoryEntry the accessors for them, and calls §7.2.6.4's two
 * entry-list algorithms at the two steps that name them — the document install (initialize the entries for a
 * new document) and §7.4.4 step 11 / §7.4.6.2 step 6.4.2 (update the entries for a same-document navigation).
 * THAT SECOND CALL IS WHY §7.4.4 IS NO LONGER A PLAIN C ALGORITHM: §7.2.6.4 fires `currententrychange` and then
 * `dispose` at every entry a push threw off the forward history, so a `pushState` runs the page's code and the
 * steps are a request its caller parks on.
 * WHAT §7.2.6 IS STILL MISSING is its navigation methods, and navigation.h names them; its NAVIGATE EVENT is
 * built (core/frame/navigate_event_fire.h) and both synchronous history updates fire one.
 * NOR IS A CROSS-DOCUMENT TRAVERSAL OR A RELOAD HERE: both need §7.4.5's populate-a-session-history-entry (a
 * fetch) and §7.4.6.1's deactivate-a-document (pageswap, unload, pagehide), and each is asserted at the step
 * that would reach it rather than approximated. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_SESSION_HISTORY_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_SESSION_HISTORY_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/events/event_target.h"   /* EventFireCb — the width of §7.4.6.2 step 6.4.3's popstate fire */
#include "core/frame/navigate_event_fire.h" /* §7.4.2.3.3 step 4's push/replace/reload navigate event */
#include "core/frame/navigation.h"      /* NavigationUpdateWork — §7.4.4 step 11 is a request */
#include "core/structured_clone.h"

/* Declared once per AGENT: the per-realm record slot and the realm intrinsic that builds it. */
void session_history_init(JSContext *ctx);
void session_history_free(void);

/* THE NAVIGABLE'S FIRST SESSION HISTORY ENTRY, built with the DOCUMENT rather than with the realm.
 *
 * The record itself is built by the realm intrinsic, at the pre-boot baseline, because a record made on first
 * touch is made inside whichever flow happened to read first (§6.6.7's autofocus record states the same
 * reason). The ENTRY cannot be: an entry holds the document's URL and its document state holds the document's
 * id and origin, and a realm's intrinsics are built before its navigable has a document — which is the same
 * sentence core/frame/location.c gives for reading the address at the call rather than at the install. So this
 * runs from document_install, which is also baseline and is the first moment all three facts exist.
 *
 * It is §7.4.5's populate-a-session-history-entry and §7.4.6's activate-history-entry collapsed to the one
 * shape this engine builds a document in: the fetch already happened, the Document is installed, and the entry
 * that describes it is created and made active. */
void session_history_install_document(JSContext *ctx);

/* HTML §7.4.4's URL AND HISTORY UPDATE STEPS, given this realm's Document and a new URL — IN TWO HALVES,
 * because step 11 runs the page's code.
 *
 * IT STILL DOES NOT FETCH AND IT STILL FIRES NEITHER popstate NOR hashchange. The standard says so twice — the
 * steps are the NON-FRAGMENT SYNCHRONOUS "navigation" section, and its note records that "popstate events fire
 * for fragment navigations, but not for history.pushState() calls" because these steps perform "a few select
 * updates" and omit the call to update-document-for-history-step-application that a fragment navigation makes
 * synchronously. WHAT IT DOES FIRE is §7.2.6.4's `currententrychange`, and `dispose` at every navigation API
 * entry a push throws off the forward history — so a `pushState` after a `back()` runs a page's `ondispose`
 * handler in the middle of itself, and these steps therefore suspend.
 *
 * THE SPLIT IS WHERE THE PAGE'S CODE BEGINS, not an arbitrary one. _begin is steps 1-10 — the new entry, the
 * best-guess length and index, the history object state, the Document's URL, its latest entry and the
 * navigable's active entry — every one of which is engine state and none of which can park. It therefore takes
 * the arguments and the CALLER MAY RELEASE THEM the moment it returns, which is the whole reason it exists:
 * `serialized` is malloc'd bytes and `new_url` a serialized URL, and neither is a JSValue a parked machine
 * could carry. _run is steps 11-13 and is a request.
 *   JS_STEP_CALL = return it, 0 = the steps have finished, -1 = they threw.
 *
 * `serialized` is the entry's new CLASSIC HISTORY API STATE — StructuredSerializeForStorage(data), already
 * performed by the caller because that is where a "DataCloneError" is observable — or NULL for a caller that
 * changes only the URL, which is the standard's own default and what step 3's "if serializedData is not null"
 * distinguishes. It is BORROWED: _begin copies the bytes into the entry.
 * `push` selects §7.4.4's history handling behaviour: true for "push", false for "replace". */
/* THERE IS NO `stage` HERE. §7.4.4 has exactly one rest point — step 11's request — so the CALLER's stage
   names it (core/frame/history.c's HPR_UPDATE) and the sub-cursor inside it belongs to the navigation API's own
   work record. A private byte here would be a resume point the driver's assert cannot see, which is what
   quickjs-step.h's JSTrampStepDef::steps is about. */
typedef struct {
    bool                 push;         /* the history handling behaviour, as step 5 resolved it */
    JSValue              new_entry;    /* owned */
    JSValue              to_replace;   /* owned — JS_NULL for a push, which is step 5's own spelling */
    NavigationUpdateWork nav;          /* step 11's request */
} SessionHistoryUrlUpdate;

void session_history_url_update_start(SessionHistoryUrlUpdate *w);
void session_history_url_update_visit(JSContext *ctx, SessionHistoryUrlUpdate *w, JSStepVisit *v);
void session_history_url_update_release(JSContext *ctx, SessionHistoryUrlUpdate *w);
void session_history_url_update_begin(JSContext *ctx, SessionHistoryUrlUpdate *w, const char *new_url,
                                      const StructuredData *serialized, bool push);
int  session_history_url_update_run(JSContext *ctx, SessionHistoryUrlUpdate *w, JSValue in,
                                    JSValue **out_cb, int *out_argc);

/* ---- §7.4.6.1's CHANGING NAVIGABLE CONTINUATION STATE ---------------------------------------------------------
 *
 * "A struct with: displayed document, target entry, navigable, update only" — plus the four values §7.4.6.1's
 * second half is handed alongside it (the history object length and index, the previous entry, and the
 * navigation type) and §7.4.6.2's own oldURL.
 * THE NAVIGABLE IS NOT A FIELD because there is exactly one and it is the realm's: session_history.c's
 * sh_entries asserts that at the one place a second one would matter. The DISPLAYED DOCUMENT is not one either,
 * for the same reason — §7.4.6.1 compares it against targetEntry's document to decide whether the traversal
 * unloads anything, and that comparison is made against the entry's document state's id.
 *
 * IT IS DECLARED HERE RATHER THAN IN session_history.c BECAUSE `stage` IS A REST POINT SOMEBODY ELSE HOSTS.
 * §7.4.6.2's three rest points (the navigation API update, the popstate dispatch, and the traversal's own
 * resolve) are this field, and §7.4.2.3.3 step 14 CALLS §7.4.6.2 from inside a work record a MEMBER's machine
 * carries — so the struct has to be nameable outside the file that drives it. Nobody outside session_history.c
 * reads or writes a field of it; what a caller does is hold one and hand it back. */
typedef struct {
    uint32_t    target_step;
    uint32_t    begin_step;      /* the traversable's current session history step when this application began */
    uint32_t    length, index;   /* §7.4.6.1's (scriptHistoryLength, scriptHistoryIndex) */
    bool        update_only;
    const char *navigation_type; /* the spec's NavigationType: "push", "replace" or "traverse" */
    uint16_t    stage;
    JSValue     target_entry;    /* owned */
    JSValue     displayed_entry; /* owned — the navigable's active entry when this began; §7.4.6.1's previousEntry */
    JSValue     old_url;         /* owned string — §7.4.6.2 step 6.1's oldURL, held ACROSS the popstate dispatch */
    JSValue     popstate;        /* owned across the dispatch */
    uint8_t     fire_phase;
    EventFireCb fire_cb;
    /* §7.4.6.2 step 6.4.2's request. It sits BESIDE the popstate fire rather than sharing its buffer, because
       the two are different dispatches at different targets and the algorithm runs one strictly before the
       other — a shared phase would resume the navigation API's walk into the popstate's answer. */
    NavigationUpdateWork nav;
} SessionHistoryApply;

/* ---- HTML §7.4.2.2's SAME-DOCUMENT TEST, AND §7.4.2.3.3's NAVIGATE TO A FRAGMENT -------------------------------
 *
 * THE TEST IS §7.4.2.2 "Beginning navigation"'s, verbatim, and it is FOUR conjuncts: "documentResource is null;
 * response is null; url equals navigable's ACTIVE SESSION HISTORY ENTRY's URL with EXCLUDE FRAGMENTS SET TO
 * TRUE; and url's fragment is non-null". When they hold, navigate runs §7.4.2.3.3's NAVIGATE TO A FRAGMENT and
 * RETURNS — it never fetches and never builds a second Document.
 *
 * THE FIRST TWO ARE STRUCTURAL HERE and the last two are the question. `documentResource` is a POST resource
 * (§4.10.22's form submission is its only producer) and `response` is what §7.4.2.3.1's cross-document case
 * carries; every caller of this predicate is navigating to a URL and has neither, which is asserted at the
 * caller rather than restated as a parameter nobody can pass anything but null for.
 *
 * IT IS ONE COMPONENT WITH N CALLERS AND NEVER AN `if` AT ONE OF THEM. A dispatch deciding WHICH ALGORITHM a
 * destination gets must be asked at every entry that navigates, or the entry that skips it reports an unrelated
 * subsystem failing on input that subsystem should never have been shown — here, a fragment-only destination
 * handed to the cross-document loader, which fetches the page again and installs a SECOND Document over the one
 * whose script is mid-flight. §7.4.2.2 compares against the ACTIVE SESSION HISTORY ENTRY's URL and not against
 * the Document's address, which is why the answer belongs to this component: the two agree for a navigable
 * showing one document and they are different fields with different writers, and reading the wrong one is how a
 * same-document navigation silently becomes a reload.
 * `url` is a SERIALIZED URL. */
bool session_history_is_fragment_navigation(JSContext *ctx, const char *url);

/* §7.4.2.3.3's NAVIGATE TO A FRAGMENT, as the machine its steps 4 and 14 make it.
 *
 * IT IS NOT §7.4.4 WITH EXTRAS, and the standard closes its own §7.4.4 with the note that says why: "although
 * both fragment navigation and the URL and history update steps perform synchronous history updates, only
 * fragment navigation contains a synchronous call to UPDATE DOCUMENT FOR HISTORY STEP APPLICATION. The URL and
 * history update steps instead perform a few select updates … For example, this means that popstate events fire
 * for fragment navigations, but not for history.pushState() calls." So this algorithm fires `popstate` and
 * queues `hashchange`, and §7.4.4 fires neither; and this one's entry takes the active entry's NAVIGATION API
 * state while §7.4.4's takes §7.4.1.1's initial value. Neither is a refinement of the other.
 *
 * TWO REST POINTS, BOTH THE PAGE'S OWN CODE. Step 4 fires a push/replace/reload navigate event at the
 * Navigation, which a router's `navigate` listener may cancel; step 14's update-document fires
 * `currententrychange`, `dispose` and then `popstate`. A member that reaches this therefore cannot be a plain C
 * body — `location.hash = "#/route"` suspends inside its own assignment, siblings run, and it resumes.
 *
 * ITS DESTINATION RIDES THE RECORD. Between step 4 and step 14 every `navigate` listener the page has runs, and
 * each of them may push an entry or change the address — so an algorithm that read the destination back off the
 * navigable when it resumed would resolve this navigation against whatever a listener left behind. `_begin`
 * takes the URL as a JS string, which is what parks; nothing below reads the navigable for it again.
 *
 *   `url` is the destination, SERIALIZED and WITH its fragment; COPIED.
 *   `history_handling` is §7.4.2.2's resolved historyHandling — "push" or "replace", a static string, because
 *      it outlives a park and because §7.4.2.3.3 hands it on as the NavigationType at three separate steps.
 * §7.4.2.3.3's other four arguments are at values a script-initiated navigation gives them and are not
 * parameters, for the reason navigate_event_fire.h states about the same four: userInvolvement is "none",
 * sourceElement is null, navigationAPIState is null (so step 3 leaves destinationNavigationAPIState at the
 * active entry's, which is the standard's "for other fragment navigations … the navigation API state is carried
 * over from the previous entry"), and navigationId is the browser's own and is read by nothing here.
 *
 * `_run` answers JS_STEP_CALL/JS_STEP_YIELD to be returned, 0 when the algorithm has finished, JS_STEP_ABRUPT
 * when it threw. A navigation a `navigate` listener CANCELED also answers 0: §7.4.2.3.3 step 5 returns, and the
 * member that asked for it answers exactly as it does when the navigation succeeded. */
typedef struct {
    uint8_t             stage;            /* SHFRAG_STAGES in session_history.c */
    const char         *history_handling; /* "push" or "replace" */
    JSValue             url;              /* owned string — the destination, taken WITH the operation */
    JSValue             history_entry;    /* owned — step 6's historyEntry */
    JSValue             to_replace;       /* owned — step 7's entryToReplace, JS_NULL for a push */
    NavigateEventFireWork fire;           /* step 4 */
    SessionHistoryApply   apply;          /* step 14's update-document, and the three rest points inside it */
} SessionHistoryFragmentNav;

void session_history_fragment_nav_start(SessionHistoryFragmentNav *w);
void session_history_fragment_nav_visit(JSContext *ctx, SessionHistoryFragmentNav *w, JSStepVisit *v);
void session_history_fragment_nav_begin(JSContext *ctx, SessionHistoryFragmentNav *w, const char *url,
                                        const char *history_handling);
int  session_history_fragment_nav_run(JSContext *ctx, SessionHistoryFragmentNav *w, JSValue in,
                                      JSValue **out_cb, int *out_argc);

/* HTML §7.4.3's TRAVERSE THE HISTORY BY A DELTA, given this realm's navigable's traversable and an integer.
 *
 * It APPENDS the traversal to the traversable rather than performing it — the standard's steps 4.1-4.5 are
 * inside "append the following session history traversal steps", and here that is a job on the ONE frontier — so
 * §7.2.5's `go`, `back` and `forward` all return to their caller with the traversal scheduled. THE DELTA IS
 * RESOLVED WHEN THE JOB RUNS, which is what makes `history.back(); history.back();` go back two.
 * `delta` is NEVER 0: §7.2.5's delta traverse step 4 answers a zero delta with a RELOAD of the navigable and
 * returns, which is a different algorithm and core/frame/history.c's to reach. Asserted here. */
void session_history_traverse_by_delta(JSContext *ctx, int32_t delta);

/* §7.2.5's `length` — "the number of overall session history entries for the current traversable navigable",
   which the History object holds as its own `length` and which §7.4.6 keeps in step with the traversable. */
uint32_t session_history_length(JSContext *ctx);
/* §7.2.5's `state` — the CLASSIC HISTORY API STATE of the active entry, deserialized into this realm. Held on
   the History object rather than deserialized per read, because §7.4.4 step 7's "restore the history object
   state" is what writes it and a page compares `history.state === history.state`. OWNED. */
JSValue session_history_state(JSContext *ctx);

/* §7.4.1.1's SCROLL RESTORATION MODE of the navigable's active session history entry — what §7.2.5's
   `scrollRestoration` getter returns and its setter writes. The string is the IDL enumeration's value, so the
   setter's argument has already been validated by the declared IDL_ENUM type. BORROWED on the way out. */
const char *session_history_scroll_restoration(JSContext *ctx);
void        session_history_set_scroll_restoration(JSContext *ctx, const char *mode);

/* ---- what §7.2.6's navigation API reads off a §7.4.1.1 entry ------------------------------------------------
 *
 * The three fields §7.4.1.1 gives an entry FOR that interface, and the two facts §7.2.6.5's members ask about
 * one. They are functions rather than property names because the fields are this component's: a second reader
 * spelling the atom itself is a second place the field's name lives, and the first rename breaks the one that
 * is not checked.
 *
 * THE KEY AND THE ID ARE DERIVED, NOT DRAWN AT RANDOM — the same rule and the same reason core/file/blob.c
 * gives for a blob URL's UUID. §7.4.1.1 says each is "the result of generating a random UUID"; this engine is
 * deterministic on purpose, because a time-travel resume must produce the byte-identical key or a flow that
 * stored one and a flow that resumes to traverse by it disagree about which entry they mean. A counter on the
 * per-realm record is unique by construction rather than with high probability, and because the counter is a
 * property the COW delta captures, two arms of a fork minting an entry at the same source line mint the same
 * key — which is what makes two timelines comparable rather than accidentally distinct. */
/* §7.4.1.4's GET SESSION HISTORY ENTRIES FOR THE NAVIGATION API, at the traversable's CURRENT session history
   step — the same-origin contiguous run around the entry this document is at, which is the only part of the
   session history §7.2.6 ever exposes. A new Array of the entries themselves; OWNED. */
JSValue session_history_entries_for_navigation_api(JSContext *ctx);

JSValue session_history_entry_nav_key(JSContext *ctx, JSValueConst e);     /* the key string, OWNED */
JSValue session_history_entry_nav_id(JSContext *ctx, JSValueConst e);      /* the id string, OWNED */
/* StructuredDeserialize of the entry's navigation API state, freshly into this realm — which is what
   §7.2.6.5's `getState()` returns and why `entry.getState() !== entry.getState()`. OWNED. */
JSValue session_history_entry_nav_state(JSContext *ctx, JSValueConst e);
/* §7.2.6.6's updateCurrentEntry step 4 and §7.2.6.8's notify-about-the-committed-to-entry are the writers.
   `d` is BORROWED; this copies the bytes into the entry. */
void    session_history_entry_set_nav_state(JSContext *ctx, JSValueConst e, const StructuredData *d);
/* The entry's URL, as the string §7.2.6.5's `url` returns. OWNED. */
JSValue session_history_entry_url(JSContext *ctx, JSValueConst e);
/* Does this entry's §7.4.1.2 document state name THIS realm's Document — §7.2.6.5's `sameDocument`, and the
   first conjunct of its `url` getter's step 4. */
bool    session_history_entry_is_this_document(JSContext *ctx, JSValueConst e);

/* §7.4.4 STEP 4's "document's IS INITIAL about:blank" — the conjunction of the address and the fact that this
   navigable has never been navigated (see session_history.c). §7.2.6.3's has-entries-and-events-disabled asks
   it as its third clause, which is the only reader outside this component. */
bool    session_history_is_initial_about_blank(JSContext *ctx);

/* ---- WHAT §7.4.3 "Reloading and traversing" READS OFF THE ACTIVE SESSION HISTORY ENTRY ------------------------
 *
 * A RELOAD IS DEFINED OVER THE ENTRY AND NEVER OVER THE DOCUMENT, which is the whole reason these three are
 * here rather than being read off `document_url` at the caller. §7.4.3 step 1.4 sets the navigate event's
 * destinationURL from "navigable's ACTIVE SESSION HISTORY ENTRY's URL", and §7.4.5 then re-populates THAT
 * entry — so after `history.pushState(s, "", "/x")` a reload fetches `/x` and carries `s` forward, and a
 * caller that asked the Document instead would be reading a field with a different writer that happens to
 * agree today. The distinction is exactly the one session_history_is_fragment_navigation exists for, one
 * algorithm along.
 *
 * THE STATE COMES BACK AS THE SERIALIZED BYTES and not as a deserialized value, unlike
 * session_history_entry_nav_state above: §7.2.6.10.4 step 11 sets the DESTINATION's state to it and
 * §7.2.6.10.3 deserializes afresh per `getState()` call, so deserializing here would mint a value the
 * destination would have to re-serialize. OWNED (an ArrayBuffer).
 *
 * THE STEP IS §7.4.1.1's, and its one reader is the assertion §7.4.3's reload makes about this build's
 * collapsed populate — core/frame/navigable.c states what the two disagree about. */
JSValue  session_history_active_entry_url(JSContext *ctx);              /* OWNED string */
JSValue  session_history_active_entry_navigation_state(JSContext *ctx); /* OWNED ArrayBuffer */
uint32_t session_history_active_entry_step(JSContext *ctx);

#endif
