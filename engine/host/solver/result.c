#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solver/result.h"
#include "solver/endpoint.h"
#include "solver/solve.h"
#include "solver/engine.h"

/* The composition, and nothing else. Each surface serializes itself — endpoint.c walks its deduped endpoints,
   solve.c its fire-verified sinks — and this only decides that they are ONE document and what it is called.
   Keeping that decision in one place is the point: a second caller that wanted "just the endpoints" is how a
   host ends up assembling structure again. */
char *result_json(void) {
    char *eps = endpoint_json_array();
    char *sinks = solve_json_array();
    size_t n;
    char *out;

    if (!eps || !sinks) { free(eps); free(sinks); return NULL; }
    n = strlen(eps) + strlen(sinks) + 96;
    out = malloc(n);
    if (out)
        snprintf(out, n, "{\"fetchCallSites\":%s,\"securitySinks\":%s,\"_switches\":%d}",
                 eps, sinks, engine_switch_count());
    free(eps);
    free(sinks);
    return out;
}
