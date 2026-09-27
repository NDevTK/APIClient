/* COOKIE STORE API §7.4 "Process changes" — the whole of that section's WINDOW arm: `To process cookie changes`
 * step 1, `To fire a change event named type with changes at target`, and `To prepare lists from changes`. See
 * process_changes.c.
 *
 * WHICH STANDARD THIS IS, BECAUSE IT MOVED. The Cookie Store API was developed in the W3C WICG and is now a
 * WHATWG Living Standard at https://cookiestore.spec.whatwg.org/, whose own intellectual-property boilerplate
 * records that move; both old addresses 404. That is cookie_store.h's argument and it is REPORTED rather than
 * quoted here for that header's reason: front matter sits outside every numbered section, so a quotation of it
 * checked against §7.4 is truthfully not found there and stays a finding for ever at a comment that is right.
 *
 * WHY IT IS ITS OWN COMPONENT. §7.4 reads nothing §3's four methods read and shares no state with them: it
 * walks this agent's navigable tree, mints an event and queues a task, where §3's members declare IDL arguments
 * and settle promises. It is exercisable with ONE fixture — write a cookie, observe the event — and every
 * invariant below is about the walk rather than about an argument, which is CLAUDE.md's own metric for where a
 * file ends. What it shares with §3 is three ALGORITHMS, declared in cookie_store.h with the reason at each.
 *
 * WHAT IT IS NOT, AND IT IS A HALF OF §7.4 RATHER THAN A HALF OF THE ENGINE. §7.4's step 2 is "For every service
 * worker registration registration", and it fires a functional event using §5.2 "The ExtendableCookieChangeEvent
 * interface" on a registration, over the subscriptions §4 "The CookieStoreManager interface" holds. See the
 * named residual in process_changes.c, which states what that arm needs and why landing any of it now would be
 * a producer with no consumer. */
#ifndef ENGINE_HOST_BROWSER_CORE_COOKIE_STORE_PROCESS_CHANGES_H
#define ENGINE_HOST_BROWSER_CORE_COOKIE_STORE_PROCESS_CHANGES_H

#include "quickjs.h"

/* Declared ONCE PER AGENT, from cookie_store.c's own init — this is a sub-component of that platform row, the
   way core/events/cookie_change_event.c is one of `event`'s. What it declares is a CLAIM on Cookie Store API
   §2.2 "Cookie store"'s process-cookie-changes steps, which core/loader/cookie_jar.h holds as a one-claimant
   hook. There is no per-realm install: §7.4 is an ALGORITHM and installs no member anywhere. */
void cookie_store_process_changes_init(void);
/* Undone ONCE PER AGENT, from cookie_store_free. It gives the claim back, which is what makes a second agent in
   one process able to claim it again. */
void cookie_store_process_changes_free(void);

#endif
