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
int dom_run_scripts(JSContext *ctx);    /* run scripts in document order; 1 = PARKED on an unfetched sync external, 0 = all ran */
int dom_boot_resume(JSContext *ctx);    /* resume the boot cursor after a parked external was provided */
int dom_boot_parked_is(const char *url);/* 1 if url is the sync classic external the boot cursor is parked on (vs a module a boot script imported) */
/* An external <script>'s load kind, RECORDED when it is requested (so qjs_provide routes it without re-parsing —
   a JS parse with COW active aborts in the parked-boot state). SYNC blocks document order; ASYNC/MODULE do not. */
typedef enum { SK_SYNC = 0, SK_ASYNC = 1, SK_MODULE = 2 } ScriptKind;
int dom_script_kind(const char *url);   /* the recorded kind of a requested external (SK_SYNC if unknown) */
void dom_script_kind_set(const char *url, int kind);   /* record a kind for a dynamically-inserted <script src> / dyn-import chunk */
/* Execute ONE CLASSIC boot <script> from its parse-once bytecode (document.currentScript = el, then run) — the
   SINGLE execution path shared by the first boot (dom_run_scripts) and every replay (boot_scripts_run), so a
   replay is byte-identical to the first run. Returns 1 if it threw (exception left PENDING for the caller's throw
   policy: dom_run_scripts why_adds a first-run page error, boot_scripts_run DFAILs a replay COW gap). el = the
   <script> element or JS_NULL; compiled = the bytecode from boot_script_cache (JS_EXCEPTION -> a syntax error). */
int boot_exec_one(JSContext *ctx, JSValueConst el, JSValueConst compiled);

#endif
