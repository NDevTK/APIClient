/* HTML §7.4.1's SESSION HISTORY, §7.4.3's TRAVERSAL, and §7.4.4's URL and history update steps — see
 * session_history.c.
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
 * WHAT §7.2.6 IS STILL MISSING is its navigate event and its navigation methods, and navigation.h names them.
 * NOR IS A CROSS-DOCUMENT TRAVERSAL OR A RELOAD HERE: both need §7.4.5's populate-a-session-history-entry (a
 * fetch) and §7.4.6.1's deactivate-a-document (pageswap, unload, pagehide), and each is asserted at the step
 * that would reach it rather than approximated. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_SESSION_HISTORY_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_SESSION_HISTORY_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "quickjs-step.h"
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

#endif
