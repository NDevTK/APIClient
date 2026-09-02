/* CLOSE WATCHERS — HTML §6.10.2 "Close watcher infrastructure". See close_watcher.c.
 *
 * WHAT THIS IS INFRASTRUCTURE FOR. §6.10.1 "Close requests" defines what a user does — its own examples are
 * "The Esc key on desktop platforms", the back button or gesture on Android, "Any assistive technology's
 * dismiss gesture", a game controller's back button — and §6.10.2 defines the ONE per-Window structure every
 * dismissable thing in the platform registers with, so that one such request closes exactly one of them.
 * HTML §6.12 The popover attribute's show popover, §6.10.3 The CloseWatcher interface's `new CloseWatcher()`
 * and §4.11.4 The dialog element's modal `dialog` are three establishers of the SAME struct in the SAME list.
 * That is why this is its own component and not a
 * member of any of them: a second copy of the group algebra, per establisher, is three copies that can
 * disagree about whether one Esc closes one popover or all of them.
 *
 * THE MANAGER IS PER WINDOW, SO IT IS A PER-REALM RECORD. "Each Window has a close watcher manager" — not each
 * Document — and in this engine a Window is a realm, so the record lives in core/realm.h's per-realm value
 * beside the two timestamps HTML §6.4.1 Data model defines, which core/html/user_activation.c holds the same
 * way for the same sentence. It is built WITH the realm rather than on first touch, for the reason that file states: a
 * record minted on the first read is built inside whichever flow happened to ask first, and that flow's
 * baseline becomes every sibling's.
 *
 * ITS GROUPS ARE JS ARRAYS, AND THE STANDARD PICKED THE SHAPE THAT FORCES IT. The manager's groups is "a list
 * of lists of close watchers", and §6.10.2's destroy removes a watcher FROM THE MIDDLE of a group and then
 * removes any group left empty — which is exactly the structure §PLATFORM-DATA-A-FLOW-QUEUES-IS-A-JS-VALUE is
 * about. A malloc'd list captured as head/tail pointers reverts the POINTERS on a context switch and leaves
 * the nodes reachable from nothing, which the runtime's own GC walk cannot see and no gate reports. As Arrays
 * the mutations are property writes the heap COW delta already captures, so one forked arm that established a
 * watcher and one that did not each read back their own manager, and a parked flow resumes with the one it
 * had. core/css/top_layer.c holds §3's two ordered sets this way on the same argument.
 *
 * A WATCHER NAMES ITS THREE ALGORITHMS BY A KIND, NOT BY A CLOSURE. §6.10.2's struct holds a cancel action, a
 * close action and a get enabled state, each "a list of steps". All three terms are §6.10.2's own; what an
 * establisher does is SUPPLY them, and the one HTML §6.12 The popover attribute supplies as its close action
 * is hide a popover, which is a step machine that parks. So a JS function object cannot hold them and a C
 * function pointer cannot survive a park or a cross-session resume. A kind id out of a registry FIXED AT THIS
 * ENUM'S DEFINITION can: §AN-INDEX-NAMES-A-THING-ONLY-WHILE-THE-SET-IS-FIXED permits a position to name a
 * thing exactly where the set is the machine's own and cannot change
 * under a parked flow, which a compile-time enum is and a page-mutated map is not. Blink spells the same fact
 * `CloseWatcher::Delegate`.
 *
 * WHAT IS HONESTLY ABSENT, AND WHY IT IS A RESIDUAL RATHER THAN A CRASH. This component is §6.10.2's manager
 * and the four of its algorithms that touch ONLY the manager — notify the close watcher manager about user
 * activation (3 steps), establish a close watcher (7), destroy a close watcher (3), and the "active"
 * predicate. Not built here: request to close a close watcher (12 steps), close a close watcher (5), process
 * close watchers (4), and §6.10.1's close request steps (9). The split is not a convenience — it is the line
 * between the algorithms that only READ AND WRITE THE MANAGER and the algorithms that RUN THE WATCHER'S THREE
 * ACTIONS, and everything on the far side of it must be a step machine twice over: it runs the page's code (a
 * `cancel` event, a `beforetoggle` event, hide a popover) and it asks §6.4.1's history-action activation,
 * which core/html/user_activation.h answers as a REQUEST that forks because the timestamp behind it is
 * unknown external state.
 *   The code here is therefore RIGHT and NARROWER, which is why it asserts rather than aborts: a watcher this
 * file establishes is a real watcher in a real manager, correctly grouped under §6.10.2's anti-abuse rule.
 *   WHAT THE NEXT DIFF MUST LEAVE BEHIND: request to close / close / process close watchers as step machines,
 * driven from a dispatch over CloseWatcherKind that runs the three actions — for the one kind below, cancel
 * action and get enabled state are both "to return true" and only the close action reaches page code — plus
 * §6.10.3's `CloseWatcher` interface, whose `requestClose()` and `close()` are their first live callers and
 * whose constructor is the first live caller of close_watcher_establish.
 *   HOW ITS ABSENCE SHOWS: nothing ever asks a watcher to close, so a Window's `groups` only ever grows and
 * nothing drains it; a popover that established one hides only through its own `hidePopover()`, never through
 * a close request; and `new CloseWatcher()` is a TypeError because the interface object is not installed.
 *
 * WHERE FULLSCREEN MEETS THIS, AND WHERE IT DOES NOT. §6.10.1's close request steps are 9 steps whose step 1
 * is "If document's fullscreen element is not null", whose two sub-steps are "Fully exit fullscreen given
 * document's node navigable's top-level traversable's active document" and "Return" — so a fullscreen document
 * SHORT-CIRCUITS the whole algorithm and never reaches step 7's "Let closedSomething be the result of
 * processing close watchers". Fullscreen therefore does NOT register a close watcher and does not belong on
 * this stack; it is a higher-priority arm of the same 9 steps. What the two share is that ONE component owns
 * those 9 steps — its step 1 asking the fullscreen model and its step 7 asking this one — and neither half
 * should grow a private copy of them. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_CLOSE_WATCHER_H
#define ENGINE_HOST_BROWSER_CORE_HTML_CLOSE_WATCHER_H

#include <stdbool.h>

#include "quickjs.h"

/* WHICH ALGORITHM TRIPLE A WATCHER CARRIES — see the header note above for why this is an id and not a
   closure. ONE ENTRY, because one establisher is in this build: §6.12 The popover attribute's show popover
   step 15 establishes a watcher "given element's relevant global object, with: cancelAction being to return
   true. closeAction being to hide a popover given element, true, true, false, and null. getEnabledState being
   to return true." §6.10.3 The CloseWatcher interface and §4.11.4 The dialog element each add their own entry
   with their own three when they land. An entry added here WITHOUT its three is what close_watcher_establish's
   range DCHECK and the action dispatch the next diff builds are between them for. */
typedef enum {
    CLOSE_WATCHER_KIND_POPOVER = 0,   /* HTML §6.12 The popover attribute — subject is the popover Element */
    CLOSE_WATCHER_KIND_COUNT
} CloseWatcherKind;

/* Declared ONCE PER AGENT, from core/html's declaration point, and it must run BEFORE the first realm is
   built: a realm that missed the install has no manager, and §6.4.2 step 5.2 notifies one on every activation. */
void close_watcher_init(JSContext *ctx);
void close_watcher_free(void);

/* §6.10.2's "To NOTIFY THE CLOSE WATCHER MANAGER ABOUT USER ACTIVATION given a Window window", 3 steps —
   §6.4.2 Processing model's step 5.2, performed for each window of the activation notification's walk. `wctx`
   is the realm of the Window being notified, which is why it is a parameter and not the asking realm:
   §6.4.2's step 5 writes a SET of Windows and every one of them is a different realm in this agent. */
void close_watcher_notify_user_activation(JSContext *wctx);

/* §6.10.2's "To ESTABLISH A CLOSE WATCHER given a Window window, a list of steps cancelAction, a list of steps
   closeAction, and an algorithm that returns a boolean getEnabledState", 7 steps. `wctx` is the Window's
   realm; `subject` is the object the kind's three algorithms act on (HTML §6.12 passes the popover Element).
   OWNED — the caller frees, and the caller is also what holds the watcher for its lifetime (HTML §6.12 keeps it in
   the element's "popover close watcher"); the manager's own reference is dropped by destroy. */
JSValue close_watcher_establish(JSContext *wctx, CloseWatcherKind kind, JSValueConst subject);

/* §6.10.2's "To DESTROY A CLOSE WATCHER closeWatcher", 3 steps. Idempotent — destroying a watcher that is not
   in any group removes it from every group it is not in and compacts nothing, which is what the standard's
   own "remove closeWatcher from group" does for an absent item and what HTML §6.12's hide a popover relies on. */
void close_watcher_destroy(JSContext *wctx, JSValueConst watcher);

/* §6.10.2's "A close watcher closeWatcher is ACTIVE if closeWatcher's window's close watcher manager contains
   any list which contains closeWatcher." Read by request to close's step 1 and close's step 1 when those
   land; read here as the two-sided assert on establish and destroy. */
bool close_watcher_is_active(JSContext *wctx, JSValueConst watcher);

#endif
