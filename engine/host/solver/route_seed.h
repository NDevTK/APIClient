/* Route seeding: an address the page's own code declares as one of its pages becomes a document on the frontier.
 * The observation site is HTML §7.4.4 "Non-fragment synchronous \"navigations\""'s URL and history update steps,
 * which pushState and replaceState (§7.2.5 "The History interface") and every other caller run; it fires when a
 * document's address changes without a fetch, never on a URL argument alone (step 4 defaults it to the current
 * address). It is a one-way notice, not a pending kind, because the seeding flow is owed nothing and §7.4.4 loads
 * nothing.
 * Record: `document.seed<TAB><address><TAB><provenance><TAB><driven><TAB><pinned><TAB><issuer>`. The address is
 * absolute and serialized (no re-resolution downstream) and asserted to hold no TAB, which the C0 control
 * percent-encode set already excludes. `driven` and `pinned` are engine.h's PENDING_DRIVEN_* and PENDING_PINNED_*
 * for the declaring flow; `issuer` is the declaring document's world name, whose record the trusted zone judges
 * the seed by. Provenance is `derived`, or `forced` on a contradicted arm, and never `observed`: no load of a
 * document produces a pushState. It is not composed through pending_prov_compose, whose input is a park kind.
 * Seeds are not deduplicated: a repeat is ranked below new work by the zone, never refused here. */
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
