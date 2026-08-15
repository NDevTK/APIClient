/* HTML §7.4.1's SESSION HISTORY, and §7.4.4's URL and history update steps — see session_history.c.
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
 * WHAT IS DELIBERATELY NOT HERE IS THE NAVIGATION API (§7.2.7 — `navigation.navigate`, NavigationHistoryEntry,
 * NavigateEvent). It is a separate and larger interface with its own entry list, its own event and its own
 * promise-bearing method trackers; core/rendering/rendering.c already carries a `realm_awaits(docctx,
 * "navigation", …)` probe for it that is correctly silent. Every step of §7.4.4 and §7.4.6 that reads or writes
 * navigation API state is named at its site as belonging to that interface rather than skipped in silence. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_SESSION_HISTORY_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_SESSION_HISTORY_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
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

/* HTML §7.4.4's URL AND HISTORY UPDATE STEPS, given this realm's Document and a new URL.
 *
 * `serialized` is the entry's new CLASSIC HISTORY API STATE — StructuredSerializeForStorage(data), already
 * performed by the caller because that is where a "DataCloneError" is observable — or NULL for a caller that
 * changes only the URL, which is the standard's own default and what step 4's "if serializedData is not null"
 * distinguishes. It is BORROWED: this copies the bytes into the entry.
 * `push` selects §7.4.4's history handling behaviour: true for "push", false for "replace".
 *
 * IT DOES NOT FETCH AND IT FIRES NOTHING. The standard says so twice — the steps are the NON-FRAGMENT
 * SYNCHRONOUS "navigation" section, and its note records that "popstate events fire for fragment navigations,
 * but not for history.pushState() calls" because these steps perform "a few select updates" and omit the call
 * to update-document-for-history-step-application that a fragment navigation makes synchronously. So no page
 * code runs inside this, which is why it is a plain C algorithm and not a step machine. */
void session_history_url_and_history_update(JSContext *ctx, const char *new_url,
                                            const StructuredData *serialized, bool push);

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

#endif
