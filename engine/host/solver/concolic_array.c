/* The Array interface a concolic value presents — see concolic_array.h. One assertable contract: this list is
 * EXACTLY the set of Array.prototype iterators the prelude self-hosts with a {@iterdone} / __isOpaque loop-back
 * (prelude.c). A member here that the prelude does NOT self-host would route a concolic array to the collapsing
 * C builtin; a self-hosted iterator missing here leaves its concolic branch dead code (e.g. an
 * `allowed.includes(input)` gate over an opaque `allowed` would never fork). Kept in one place so the routing
 * policy and the prelude stay a single spec surface. */
#include "solver/concolic_array.h"
#include <string.h>

int concolic_array_iter_method(const char *name) {
    /* EXACTLY the prelude's self-hosted iterators. NOT flatMap (not self-hosted). includes/indexOf/lastIndexOf/
       sort ARE self-hosted with __isOpaque branches, so they must be here or those branches are unreachable. */
    static const char *const ITER[] = {
        "forEach", "map", "filter", "reduce", "reduceRight", "some", "every",
        "find", "findIndex", "indexOf", "includes", "lastIndexOf", "sort", NULL,
    };
    if (!name)
        return 0;
    for (int i = 0; ITER[i]; i++)
        if (!strcmp(name, ITER[i]))
            return 1;
    return 0;
}
