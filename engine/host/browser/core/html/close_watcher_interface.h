/* THE CloseWatcher INTERFACE — HTML §6.10.3. See close_watcher_interface.c.
 *
 * WHY IT IS NOT PART OF core/html/close_watcher.c, WHICH IS THE SAME §6.10. close_watcher.h's own opening
 * argument decides this: §6.12 The popover attribute's show popover, §6.10.3's `new CloseWatcher()` and §4.11.4
 * The dialog element's modal `dialog` are three ESTABLISHERS of one struct in one list, and the infrastructure
 * is its own component precisely so that no establisher grows a copy of the group algebra. §6.12's establisher
 * lives in core/html/popover.c; this is §6.10.3's, and it lives beside it rather than inside the thing all
 * three of them call. The dependency runs one way and only one way: this file calls establish, destroy,
 * request to close and close, and close_watcher.c names this interface nowhere — what it knows is a KIND.
 *
 * WHAT THE INTERFACE IS FOR, WHICH IS NOT DECORATION. §6.10.1 Close requests defines a close request by its
 * examples — "The Esc key on desktop platforms", the Android back button or gesture, "Any assistive
 * technology's dismiss gesture" — and before this interface a page had exactly two ways to participate: a
 * modal `dialog` and a popover. `new CloseWatcher()` is the primitive under both, handed to the page, and a
 * bundle that builds its own picker, drawer or lightbox uses it to become dismissable the same way. For a
 * forced-execution run that matters for one concrete reason: the `cancel` and `close` handlers a bundle
 * registers on one are a POPULATION OF CODE with no other entry point, and `requestClose()` is a door into it
 * that the page's own code opens.
 *
 * THE PUBLISHED IDL, WHICH CARRIES NO EXTENDED ATTRIBUTE ON ANY MEMBER:
 *
 *     [Exposed=Window]
 *     interface CloseWatcher : EventTarget {
 *       constructor(optional CloseWatcherOptions options = {});
 *
 *       undefined requestClose();
 *       undefined close();
 *       undefined destroy();
 *
 *       attribute EventHandler oncancel;
 *       attribute EventHandler onclose;
 *     };
 *     dictionary CloseWatcherOptions { AbortSignal signal; };
 *
 * NOT ONE of the three operations carries `[CEReactions]`, `[NewObject]` or anything else, and a member
 * declared with a bracket its IDL does not write runs a step the standard does not — core/html/popover.c's own
 * header records the same reading for the same reason.
 *
 * TWO OF THE THREE OPERATIONS ARE STEP MACHINES AND THE THIRD IS NOT, and the split is exactly §6.10.2's own.
 * `requestClose()` and `close()` delegate to algorithms that RUN this watcher's actions, and both of those
 * actions fire an event at `this` — the page's own handlers, a loop, an `await`, a DOM mutation — so a C
 * activation hosting them is the drive-to-completion this engine aborts on. `destroy()` delegates to destroy a
 * close watcher, which only reads and writes the manager, so it is a plain call. The constructor is a plain
 * body too: Web IDL converted its dictionary before the body ran, and establishing a watcher, reading a
 * signal's aborted state and registering an abort algorithm are each engine work with no page code in them. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_CLOSE_WATCHER_INTERFACE_H
#define ENGINE_HOST_BROWSER_CORE_HTML_CLOSE_WATCHER_INTERFACE_H

#include "quickjs.h"

/* Declared ONCE PER AGENT — the class, the internal-close-watcher slot key, and the four members' pool ids.
   It declares its own per-realm install into core/realm.h's one list, so every realm this agent builds gets
   §3.7.1's interface object and §3.7.3's interface prototype object; there is no second, hand-copied list for
   a realm to be missing from. Called from close_watcher_init, because §6.10 is that file's section. */
void close_watcher_interface_declare(JSContext *ctx);
void close_watcher_interface_free(JSRuntime *rt);

#endif
