/* Boot-script cache + replay — see boot_scripts.h. */
#include "solver/boot_scripts.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "check.h"   /* DFAIL — a boot re-run throw is a COW/host gap, fatal in dev */
#include "core/html/html_script_runner.h"   /* boot_exec_one — the ONE per-script executor, shared with the first boot */

/* A cached CLASSIC boot <script>: its element (for document.currentScript, faithful on replay) + its text. Only
   CLASSIC scripts are cached — a module is a run-once singleton whose effects live in g_boot_delta, never
   re-evaluated (caching one would throw on replay: `export`/`import` at global scope). */
typedef struct { JSValue el; char *txt; size_t len; } BootScript;
static BootScript *g_boot_scripts = NULL; static int g_boot_n = 0, g_boot_cap = 0;

void boot_script_cache(JSContext *ctx, JSValueConst el, const char *txt, size_t len) {
    if (g_boot_n >= g_boot_cap) { int nc = g_boot_cap ? g_boot_cap * 2 : 8;
        BootScript *n = realloc(g_boot_scripts, (size_t)nc * sizeof(BootScript)); if (!n) return; g_boot_scripts = n; g_boot_cap = nc; }
    char *s = malloc(len + 1); if (!s) return; memcpy(s, txt, len); s[len] = 0;
    g_boot_scripts[g_boot_n].el = JS_DupValue(ctx, el); g_boot_scripts[g_boot_n].txt = s; g_boot_scripts[g_boot_n].len = len; g_boot_n++;
}

int boot_script_count(void) { return g_boot_n; }

/* Re-run the inline boot scripts per-script at GLOBAL scope (faithful — top-level var/let/const/function land
   exactly where the browser puts them). The CALLER unapplies g_boot_delta first, so boot's globals (incl. captured
   let/const CREATIONS) are ABSENT: re-declaration compiles cleanly, no block-wrap. A residual throw means an
   UNCAPTURED creation (a COW gap to close at the root) or a host-edge divergence — FATAL: crash with the real
   exception, never swallow it. Branch behaviour is the caller's (g_boot_replay=1 fixed-arm; g_in_boot_flow=1 fork). */
void boot_scripts_run(JSContext *ctx) {
    for (int i = 0; i < g_boot_n; i++) {
        if (boot_exec_one(ctx, g_boot_scripts[i].el, g_boot_scripts[i].txt, g_boot_scripts[i].len)) {   /* SAME executor as the first boot */
            JSValue e = JS_GetException(ctx); const char *em = JS_ToCString(ctx, e);
            char rz[300]; snprintf(rz, sizeof rz, "boot script threw on re-run: %s", em ? em : "?");
            if (em) JS_FreeCString(ctx, em); JS_FreeValue(ctx, e);
            DFAIL(rz);   /* a REPLAY throw (the first run did not) = an UNCAPTURED creation / COW re-establishment gap — fatal in dev, build the root fix. RELEASE: surfaced (exemption). */
        }
    }
}

void boot_scripts_free(JSContext *ctx) {
    for (int i = 0; i < g_boot_n; i++) { free(g_boot_scripts[i].txt); JS_FreeValue(ctx, g_boot_scripts[i].el); }
    free(g_boot_scripts); g_boot_scripts = NULL; g_boot_n = g_boot_cap = 0;
}
