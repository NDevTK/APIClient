/* REAL BOOT — the page's own <script>s, parsed by the REAL Lexbor HTML parser (spec tree construction, not a
 * regex), run in order as the FIRST flow. Scripts share the global scope exactly as in a browser, so a concolic
 * value set in script 1 and branched in script 2 forks across scripts. In the re-run model boot re-executes all
 * scripts each flow; per-flow COW reverts what a run mutated. This is the bridge from the toy single-eval test
 * to real pages. */
#ifndef ENGINE_HOST_SOLVER_BOOT_H
#define ENGINE_HOST_SOLVER_BOOT_H

#include "quickjs.h"
#include <stddef.h>

typedef struct BootProgram BootProgram;

/* Parse HTML with Lexbor + extract the inline <script> sources (in document order). */
BootProgram *boot_parse(const char *html, size_t len);

/* The run_program for engine_run: eval each script in order, sharing globals. */
void boot_run(JSContext *ctx, void *bp);

void boot_free(BootProgram *bp);

#endif
