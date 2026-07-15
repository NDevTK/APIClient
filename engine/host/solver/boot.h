/* DOCUMENT SCRIPTS — the page's own <script>s, parsed by the REAL Lexbor HTML parser (spec tree construction,
 * not a regex). This component is ONLY the browser-side parse + source extraction; there is NO separate boot
 * executor. The page's scripts are just the FIRST FLOW: the scheduler runs boot_source() through the one
 * JS_FlowNew path, exactly like every other flow. Scripts share the global scope (var/function -> global). */
#ifndef ENGINE_HOST_SOLVER_BOOT_H
#define ENGINE_HOST_SOLVER_BOOT_H

#include "quickjs.h"
#include <stddef.h>

typedef struct BootProgram BootProgram;

/* Parse HTML with Lexbor + extract the inline <script> sources (in document order). */
BootProgram *boot_parse(const char *html, size_t len);

/* The concatenated script source, run as the first flow by the scheduler (lazily built, owned by bp). */
const char *boot_source(BootProgram *bp);

void boot_free(BootProgram *bp);

#endif
