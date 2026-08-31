/* An address the application declared is a page of itself — see solver/route_seed.h. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "check.h"
#include "quickjs.h"
#include "solver/route_seed.h"
#include "solver/engine.h"   /* the one-way notice, the provenance vocabulary both ends of it read, and the
                                ONE composition of that vocabulary from the running path */
#include "solver/flow.h"     /* …and that there IS a path here at all, which is this file's own invariant */

void route_seed_declare(JSContext *ctx, const char *previous, const char *address)
{
    Flow *f;
    const char *prov;
    char *op;
    size_t n;

    DCHECK(address != NULL && *address,
           "an application declared a page of itself with no ADDRESS — HTML §7.4.4 \"Non-fragment synchronous "
           "\\\"navigations\\\"\" step 2 defaults its newURL to the Document's own address, so every caller "
           "holds a serialized absolute URL by the time it declares one and an empty string is a caller that "
           "stopped stating it rather than an algorithm that moved a Document to nowhere");
    DCHECK(previous != NULL && *previous,
           "a route declaration was made with no PREVIOUS address to compare against — every Document has one "
           "(document_install gives the initial about:blank its own and document_url asserts it), so an empty "
           "one is a caller that read the field somewhere other than off the Document, and the comparison "
           "below would then declare EVERY §7.4.4 update a new page including the ones that moved nothing");
    /* THE OBSERVATION ITSELF, AND NOT A DEDUP — see the header. An algorithm that left the address where it
       was declared no page; `history.pushState(state, "")` and `pushState(state, "", location.href)` are both
       that, and both are ordinary things a router does. */
    if (!strcmp(previous, address)) return;

    /* WHOSE PATH THIS WAS. §7.4.4 is reached by RUNNING the page's code, so there is always a flow standing
       here, and without one there is no path for the declaration to be a fact about — the seed would go out
       claiming a provenance belonging to nobody, and the zone's firing decision is made on exactly that field.
       The same assert core/html/html_link.c makes for the same reason, on the other side of the boundary. */
    f = flow_running();
    (void)f;   /* the assert below is this value's only reader, and a DCHECK is compiled out in release */
    DCHECK(f != NULL,
           "an application declared a page of itself with no flow running — HTML §7.2.5 \"The History "
           "interface\" and every other caller of §7.4.4's URL and history update steps are the page's own "
           "code, so a declaration made outside a flow has no path to say whether a gate was forced to reach "
           "it, and the trusted zone decides whether to LOAD this address on precisely that answer");
    /* CLAUDE.md §A-REQUEST-CARRIES-THE-PROVENANCE, with its first conjunct answered by the ALGORITHM rather
       than by this call: a real load of this document makes no pushState, so `observed` is unreachable here.
       What is left is the PATH's own fact — an address computed past an arm the concrete example contradicted
       exists only because a gate was forced, and a reply to a navigation to it is evidence about what a server
       says to a request no client makes.
       THE COMPOSITION IS SOLVER/ENGINE.H'S AND NOT THIS FILE'S, and it used to be a ternary here. That was the
       whole rule, written down a second time, one file away from the vocabulary it spells — and it was about
       to become a THIRD copy in core/frame/navigable.c, where a NAVIGATION asks the identical question about
       the identical fact. One composition, one place; this site keeps only what is ITS own, which is the
       assert directly above (a declaration with no path is a broken invariant HERE and is an ordinary
       document-install navigation there). */
    prov = engine_provenance_of_running_path();

    /* THE RECORD SPLITS ON TAB, so a tab in either field shifts the other one — and the field that would be
       read in the wrong place is the PROVENANCE, which is the zone's whole firing decision. URL §3.2 "Host
       miscellaneous" makes TAB a forbidden host code point and the C0 control percent-encode set escapes it
       everywhere else in a serialization, so a tab here is that grammar having changed under a record that
       splits on one rather than an address a server could send. */
    DCHECK(strchr(address, '\t') == NULL,
           "a declared page's ADDRESS contains a TAB and this notice is tab-delimited — a URL serialization "
           "percent-encodes one everywhere it can appear, so a byte that reached here is a serializer that "
           "stopped doing so, and the trusted zone would read the tail of this address as the provenance token "
           "its firing decision is made on");
    n = strlen(ROUTE_SEED_NOTICE) + strlen(address) + strlen(prov) + 3;
    op = malloc(n);
    CHECK(op != NULL, "route seed: OOM building the notice that declares a page of this application");
    snprintf(op, n, "%s\t%s\t%s", ROUTE_SEED_NOTICE, address, prov);
    engine_host_notify(ctx, op);
    free(op);
}
