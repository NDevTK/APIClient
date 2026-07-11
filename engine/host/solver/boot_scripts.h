/* Boot-script cache + replay — the time-travel substrate that re-runs the page's inline boot <script>s so a
 * cross-flow @S candidate (or a forking boot flow) re-establishes shared state under a concrete input. Extracted
 * from main.c: it owns the cached script texts + compiled bytecode and depends ONLY on the JS context (no
 * scheduler globals) — a clean component, not monolith state. The CALLER sets the branch mode (g_boot_replay
 * fixed-arm for a candidate, g_in_boot_flow forking for a boot flow) and unapplies g_boot_delta first. */
#ifndef ENGINE_HOST_SOLVER_BOOT_SCRIPTS_H
#define ENGINE_HOST_SOLVER_BOOT_SCRIPTS_H
#include <stddef.h>
#include "quickjs.h"

void boot_script_cache(JSContext *ctx, JSValueConst el, const char *txt, size_t len);   /* cache one CLASSIC boot <script> (element for currentScript + body), document order; el = JS_NULL for the preamble/external */
void boot_scripts_run(JSContext *ctx);                  /* re-run every cached CLASSIC boot script through boot_exec_one — the SAME executor the first boot uses */
int  boot_script_count(void);                           /* number cached; 0 -> there is no boot flow to enqueue */
void boot_scripts_free(JSContext *ctx);                 /* free the cached texts + element refs (teardown) */

#endif
