/* HTML script runner — Blink core/html (HTMLScriptRunner / ScriptLoader): run the document's <script> elements
 * like a real browser. eval_page_script runs ONE program (classic global, or ESM with static-import deferral);
 * dom_run_scripts walks the page's <script>s in document order — inline JS executes now, external <script src>
 * is queued for fetch, a data block (json/importmap/template) is parsed-but-not-run. Extracted from main.c so
 * script execution is a browser COMPONENT, not scheduler state. */
#ifndef ENGINE_HOST_BROWSER_HTML_SCRIPT_RUNNER_H
#define ENGINE_HOST_BROWSER_HTML_SCRIPT_RUNNER_H
#include <stddef.h>
#include "quickjs.h"

void eval_page_script(JSContext *ctx, const char *code, size_t len, const char *name, int is_module);
void dom_run_scripts(JSContext *ctx);   /* execute the current document's <script> elements (document order) */
/* Execute ONE CLASSIC boot <script> from its parse-once bytecode (document.currentScript = el, then run) — the
   SINGLE execution path shared by the first boot (dom_run_scripts) and every replay (boot_scripts_run), so a
   replay is byte-identical to the first run. Returns 1 if it threw (exception left PENDING for the caller's throw
   policy: dom_run_scripts why_adds a first-run page error, boot_scripts_run DFAILs a replay COW gap). el = the
   <script> element or JS_NULL; compiled = the bytecode from boot_script_cache (JS_EXCEPTION -> a syntax error). */
int boot_exec_one(JSContext *ctx, JSValueConst el, JSValueConst compiled);

#endif
