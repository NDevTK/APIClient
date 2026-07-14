/* HTML script runner — Blink core/html (HTMLScriptRunner / ScriptLoader): run the document's <script> elements
 * like a real browser. dom_run_scripts walks the page's <script>s in document order — inline classic JS runs now
 * (run_classic_body), an inline module links via the module map (link_inline_module), external <script src> is
 * queued for fetch, a data block (json/importmap/template) is parsed-but-not-run. Extracted from main.c so
 * script execution is a browser COMPONENT, not scheduler state. */
#ifndef ENGINE_HOST_BROWSER_HTML_SCRIPT_RUNNER_H
#define ENGINE_HOST_BROWSER_HTML_SCRIPT_RUNNER_H
#include <stddef.h>
#include "quickjs.h"

int dom_run_scripts(JSContext *ctx);    /* COLLECT the document's <script>s in order (CSP, kinds); the scheduler then drives each classic script's program as a flow via document_script_next. Returns 0 (no synchronous run) */
JSValue document_script_next(JSContext *ctx);  /* yield the NEXT classic script's compiled program (OWNED) for the scheduler to dispatch as a flow; JS_NULL = all yielded; JS_UNINITIALIZED = PARKED on an unfetched sync external */
int dom_script_has_more(void);          /* more document scripts remain to drive */
int dom_script_parked_is(const char *url);/* 1 if url is the sync classic external the document cursor is parked on (vs a module a script imported) */
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
