/* Boot-script cache + replay — see boot_scripts.h. */
#include "boot_scripts.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "check.h"   /* DFAIL — a boot re-run throw is a COW/host gap, fatal in dev */

static char **g_boot_scripts = NULL; static int g_boot_n = 0, g_boot_cap = 0;
/* compiled boot programs (COMPILE-ONCE): a real browser parses a script ONCE and re-runs the bytecode;
   re-JS_Eval'ing the source string per candidate replay was a re-parse SHORTCUT (and leaked per-parse objects). */
static JSValue *g_boot_compiled = NULL;

void boot_script_cache(const char *txt, size_t len) {
    if (g_boot_n >= g_boot_cap) { int nc = g_boot_cap ? g_boot_cap * 2 : 8;
        char **n = realloc(g_boot_scripts, (size_t)nc * sizeof(char *)); if (!n) return; g_boot_scripts = n; g_boot_cap = nc; }
    char *s = malloc(len + 1); if (!s) return; memcpy(s, txt, len); s[len] = 0; g_boot_scripts[g_boot_n++] = s;
}

int boot_script_count(void) { return g_boot_n; }

/* Re-run the inline boot scripts per-script at GLOBAL scope (faithful — top-level var/let/const/function land
   exactly where the browser puts them). The CALLER unapplies g_boot_delta first, so boot's globals (incl. captured
   let/const CREATIONS) are ABSENT: re-declaration compiles cleanly, no block-wrap. A residual throw means an
   UNCAPTURED creation (a COW gap to close at the root) or a host-edge divergence — FATAL: crash with the real
   exception, never swallow it. Branch behaviour is the caller's (g_boot_replay=1 fixed-arm; g_in_boot_flow=1 fork). */
void boot_scripts_run(JSContext *ctx) {
    if (!g_boot_compiled && g_boot_n > 0) {   /* COMPILE ONCE (lazily, on the first replay): parse each boot program to bytecode and keep it */
        g_boot_compiled = malloc((size_t)g_boot_n * sizeof(JSValue));
        if (g_boot_compiled) for (int i = 0; i < g_boot_n; i++)
            g_boot_compiled[i] = JS_Eval(ctx, g_boot_scripts[i], strlen(g_boot_scripts[i]), "<boot-replay>", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    }
    for (int i = 0; i < g_boot_n; i++) {
        JSValue prog = (g_boot_compiled && !JS_IsException(g_boot_compiled[i]))
            ? JS_DupValue(ctx, g_boot_compiled[i])
            : JS_Eval(ctx, g_boot_scripts[i], strlen(g_boot_scripts[i]), "<boot-replay>", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        JSValue v = JS_IsException(prog) ? prog : JS_EvalFunction(ctx, prog);   /* runs + consumes prog */
        if (JS_IsException(v)) {
            JSValue e = JS_GetException(ctx); const char *em = JS_ToCString(ctx, e);
            char rz[300]; snprintf(rz, sizeof rz, "boot script threw on re-run: %s", em ? em : "?");
            if (em) JS_FreeCString(ctx, em); JS_FreeValue(ctx, e);
            DFAIL(rz);   /* DEV: crash at the origin (build the faithful-replay capability). RELEASE: surfaced (exemption). */
        }
        JS_FreeValue(ctx, v);
    }
}

void boot_scripts_free(JSContext *ctx) {
    if (g_boot_compiled) { for (int i = 0; i < g_boot_n; i++) JS_FreeValue(ctx, g_boot_compiled[i]); free(g_boot_compiled); g_boot_compiled = NULL; }
    for (int i = 0; i < g_boot_n; i++) free(g_boot_scripts[i]);
    free(g_boot_scripts); g_boot_scripts = NULL; g_boot_n = g_boot_cap = 0;
}
