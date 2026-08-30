/* AN ADDRESS THE APPLICATION HAS DECLARED IS A PAGE OF ITSELF — the one work item this engine can add to the
 * frontier that is a DOCUMENT rather than a flow.
 *
 * THE GAP THIS CLOSES. Forced execution walks where the code walks: it reaches a navigation — `window.open`, an
 * `<iframe src>` it computed, a `location` assignment — and core/frame/navigable.c announces the child so the
 * trusted zone can load it. It does NOT reach an address the bundle merely NAMES. A router table entry, an
 * admin path in a lazily-loaded chunk, a URL computed out of a real input and stored: each becomes an @H
 * finding and never becomes a Document anything explores. CLAUDE.md §What-the-tool-produces is entirely about
 * that surface ("what the bundle CAN do but didn't"), and §A-SELF-SEEDED-DOCUMENT makes such an address a work
 * item on the ONE frontier. Nothing produced one.
 *
 * HOW THE ENGINE KNOWS A STRING IS A ROUTE WITHOUT MATCHING ONE. It does not look at the string at all.
 * §RUN-DON'T-MATCH bans scraping a value that LOOKS like a URL, and it bans it in the direction that matters:
 * a matched name is ASSERTED, and one invented route is the example that shapes the next request. What happens
 * here instead is that the page CALLED a platform member whose entire purpose is to say "this address is a page
 * of my application", and the engine RAN it. HTML §7.2.5 "The History interface" gives `pushState(data,
 * unused, url)` and `replaceState` to every client-side router there is, and both "run the shared history
 * push/replace state steps", whose step 10 is HTML §7.4.4 "Non-fragment synchronous \"navigations\""'s URL AND
 * HISTORY UPDATE STEPS — the algorithm that pushes or replaces a SESSION HISTORY ENTRY carrying a URL that no
 * load produced. §7.4.4 says in as many words that it is the one such mechanism and that pushState and
 * replaceState are merely its best-known callers ("various other parts of the standard also need to perform
 * updates to the active history entry, and they use these steps to do so"), which is why the observation site
 * is THAT algorithm and never the History member: a caller added later declares its route through the same
 * step, and there is no second place for the question to be asked from.
 *   THE OBSERVATION IS "THE ADDRESS CHANGED", NOT "A URL ARGUMENT WAS PASSED. §7.2.5's step 4 defaults newURL
 * to the document's own address, and `pushState(s, "", location.href)` is a real thing routers do to rewrite
 * state without moving; both reach §7.4.4 with a URL, and neither declares a page the frontier does not
 * already hold. What is a fact — and what this component is given — is that a Document's address is about to
 * become one it was not, by an algorithm that fetched nothing.
 *
 * WHY IT IS A NOTICE AND NOT A PENDING KIND. solver/pending.h's register is "what the host still owes ONE
 * flow", and every kind in it names a DELIVERY back into that flow: a promise resolved, a program queued, a
 * script slot filled, a rendezvous answered. A seed owes the seeding flow nothing and delivers nothing to it —
 * §7.4.4 "load[s] nothing", so the flow does not suspend, and the page's next line runs. Parking it would be a
 * debt no reply discharges, which is exactly the state pending.h's own `pending_owed_replies` comment
 * describes as spending a credit the host can never pay. What the seed needs is the OTHER channel, the one
 * whose whole definition is an act only the trusted zone can take: solver/engine.h's one-way notice, which is
 * how core/frame/navigable.c already hands that zone an address to load.
 *
 * THE RECORD. `document.seed<TAB><address><TAB><provenance>` — one line, and both fields are asserted here to
 * contain no tab, because a notice splits on one and a shifted field would put an address where a provenance
 * token is read. The ADDRESS is absolute and serialized (§7.4.4 is handed the result of §7.2.5 step 5's
 * encoding-parse against the document's own address, so nothing downstream re-resolves it), and URL §"C0
 * control percent-encode set" makes TAB unrepresentable in one — the assert is what says that is still true.
 * The PROVENANCE is solver/engine.h's vocabulary, spelled there because the request this seed becomes is
 * decided by the same policy every other outbound request is.
 *
 * THE PROVENANCE IS NEVER `observed`, AND THAT IS A FACT ABOUT THE ALGORITHM RATHER THAN ABOUT THE VALUE.
 * CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE composes the answer out of two facts: whether a REAL LOAD of this
 * document makes this request, and whether the path stood on an arm its own concrete example contradicts. The
 * first is answered `false` here by construction — §7.4.4 is reached only by RUNNING the page's code, and no
 * load of anything produces a pushState — so a seed is `derived` on an ordinary path and `forced` on a path
 * that stood on a contradicted arm, and it can never be `observed`. It is NOT composed through
 * pending.h's `pending_prov_compose`: that function's first input is a PARK KIND, and a seed is not a park.
 *
 * IT DOES NOT DEDUP, AND THE ABSENCE IS THE DESIGN. Two forked arms that both reach one `pushState` declare
 * the same address twice, and a router that runs in a loop declares many. §NO BOUNDS names a seen-set, a
 * same-URL check and a page budget as caps wearing a crawler's vocabulary, and §A-SELF-SEEDED-DOCUMENT says
 * what answers them instead: the trusted zone ranks the address by what it has demonstrated per admission, so
 * a repeat sinks beneath work that has not been done rather than being refused. Refusing here would also be
 * this engine holding a policy about somebody else's server, which is the zone's by construction. */
#ifndef ENGINE_HOST_SOLVER_ROUTE_SEED_H
#define ENGINE_HOST_SOLVER_ROUTE_SEED_H

#include "quickjs.h"

/* THE NOTICE'S VERB, in the one place both ends of the wire read it from. */
#define ROUTE_SEED_NOTICE "document.seed"

/* THE APPLICATION HAS JUST DECLARED THAT `address` IS ONE OF ITS PAGES — called from the algorithm that makes
   it a Document's address without a load, with the address the Document is MOVING TO. `previous` is the
   address it is moving FROM, and it is a parameter rather than a read of the Document because the caller is
   mid-algorithm and the two orders are observable: HTML §7.4.4 step 8 sets the URL, so a component that read
   it for itself would be reading whichever side of that line the caller happened to be on.
   NOTHING IS EMITTED WHEN THE TWO ARE EQUAL. That is not a dedup — see the header above — it is the
   observation itself: an algorithm that left the address where it was declared no page. */
void route_seed_declare(JSContext *ctx, const char *previous, const char *address);

#endif
