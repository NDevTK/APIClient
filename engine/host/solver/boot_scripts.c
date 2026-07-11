/* Boot-script cache + replay — see boot_scripts.h. */
#include "solver/boot_scripts.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "check.h"   /* DFAIL — a boot re-run throw is a COW/host gap, fatal in dev */
#include "core/html/html_script_runner.h"   /* boot_exec_one — the ONE per-script executor, shared with the first boot */

/* A cached CLASSIC boot <script>: its element (for document.currentScript, faithful on replay) + its COMPILED
   bytecode (parse-ONCE, like a real browser — every replay re-runs the bytecode, never re-parses the source).
   Only CLASSIC scripts are cached — a module is a run-once singleton whose effects live in g_boot_delta, never
   re-evaluated (caching one would throw on replay: `export`/`import` at global scope). */
typedef struct { JSValue el; JSValue compiled; } BootScript;
static BootScript *g_boot_scripts = NULL; static int g_boot_n = 0, g_boot_cap = 0;

/* Compile a classic boot script ONCE, cache it (element + bytecode), and hand the bytecode BACK (borrowed, owned
   by the cache) so the caller runs it via boot_exec_one — the first boot and every replay share that one path. A
   compile (syntax) error is returned as JS_EXCEPTION (pending) and NOT cached; the caller surfaces it. */
JSValueConst boot_script_cache(JSContext *ctx, JSValueConst el, const char *txt, size_t len) {
    JSValue compiled = JS_Eval(ctx, txt, len, "<script>", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(compiled)) return JS_EXCEPTION;   /* syntax error: exception left pending for the caller */
    if (g_boot_n >= g_boot_cap) { int nc = g_boot_cap ? g_boot_cap * 2 : 8;
        BootScript *n = realloc(g_boot_scripts, (size_t)nc * sizeof(BootScript)); if (!n) { JS_FreeValue(ctx, compiled); return JS_EXCEPTION; } g_boot_scripts = n; g_boot_cap = nc; }
    g_boot_scripts[g_boot_n].el = JS_DupValue(ctx, el); g_boot_scripts[g_boot_n].compiled = compiled;
    return g_boot_scripts[g_boot_n++].compiled;
}

int boot_script_count(void) { return g_boot_n; }

/* Re-run the inline boot scripts per-script at GLOBAL scope (faithful — top-level var/let/const/function land
   exactly where the browser puts them). The CALLER unapplies g_boot_delta first, so boot's globals (incl. captured
   let/const CREATIONS) are ABSENT: re-declaration compiles cleanly, no block-wrap. A residual throw means an
   UNCAPTURED creation (a COW gap to close at the root) or a host-edge divergence — FATAL: crash with the real
   exception, never swallow it. Branch behaviour is the caller's (g_boot_replay=1 fixed-arm; g_in_boot_flow=1 fork). */
void boot_scripts_run(JSContext *ctx) {
    for (int i = 0; i < g_boot_n; i++) {
        if (boot_exec_one(ctx, g_boot_scripts[i].el, g_boot_scripts[i].compiled)) {   /* SAME executor as the first boot; runs the parse-once bytecode */
            JSValue e = JS_GetException(ctx); const char *em = JS_ToCString(ctx, e);
            char rz[300]; snprintf(rz, sizeof rz, "boot script threw on re-run: %s", em ? em : "?");
            if (em) JS_FreeCString(ctx, em); JS_FreeValue(ctx, e);
            DFAIL(rz);   /* a REPLAY throw (the first run did not) = an UNCAPTURED creation / COW re-establishment gap — fatal in dev, build the root fix. RELEASE: surfaced (exemption). */
        }
    }
}

void boot_scripts_free(JSContext *ctx) {
    for (int i = 0; i < g_boot_n; i++) { JS_FreeValue(ctx, g_boot_scripts[i].compiled); JS_FreeValue(ctx, g_boot_scripts[i].el); }
    free(g_boot_scripts); g_boot_scripts = NULL; g_boot_n = g_boot_cap = 0;
}
