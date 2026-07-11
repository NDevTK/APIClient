/* REPLY FETCH REGISTRY — see reply_registry.h. The pending-fetch list of reply URLs, extracted from main.c. */
#include "core/loader/reply_registry.h"
#include <string.h>
#include <stdlib.h>

/* A parked r.json()/r.text() resolves its reply in the LIVE flow (concolic) — NO promise resolve fn is held
   across the fetch (that persistent async state would outlive the flow's COW revert). So the registry is only
   the url the host must fetch; is_json is the parse tag delivered to the park (solver/reply.c), retained on the
   entry for the registration contract though the fetch LIST emits only urls. */
typedef struct { char *url; int is_json; } Pending;
static Pending *g_pending = NULL; static int g_pending_n = 0, g_pending_cap = 0;

void reply_fetch_register(const char *url, int is_json) {
    if (!url) return;
    for (int i = 0; i < g_pending_n; i++) if (g_pending[i].url && strcmp(g_pending[i].url, url) == 0) return;   /* dedup: one fetch per url */
    if (g_pending_n >= g_pending_cap) {
        int nc = g_pending_cap ? g_pending_cap * 2 : 32;
        Pending *n = realloc(g_pending, (size_t)nc * sizeof(Pending));
        if (!n) return;
        g_pending = n; g_pending_cap = nc;
    }
    g_pending[g_pending_n].url = strdup(url); g_pending[g_pending_n].is_json = is_json;
    g_pending_n++;
}
int reply_pending_count(void) { return g_pending_n; }
const char *reply_pending_list(void) {
    static char *buf = NULL; static size_t cap = 0;
    size_t need = 1;
    for (int i = 0; i < g_pending_n; i++) if (g_pending[i].url) need += strlen(g_pending[i].url) + 1;
    if (need > cap) { char *n = realloc(buf, need); if (!n) return ""; buf = n; cap = need; }
    size_t off = 0;
    for (int i = 0; i < g_pending_n; i++) {
        if (!g_pending[i].url) continue;
        size_t l = strlen(g_pending[i].url);
        memcpy(buf + off, g_pending[i].url, l); off += l; buf[off++] = '\n';   /* add() already dedups, so no re-scan here */
    }
    buf[off] = 0;
    return buf;
}
void reply_pending_drop(const char *url) {
    if (!url) return;
    for (int i = 0; i < g_pending_n; i++)
        if (g_pending[i].url && strcmp(g_pending[i].url, url) == 0) { free(g_pending[i].url); g_pending[i].url = NULL; }
    int w = 0; for (int i = 0; i < g_pending_n; i++) if (g_pending[i].url) g_pending[w++] = g_pending[i];   /* compact out the dropped entry */
    g_pending_n = w;
}
void reply_registry_free(void) {
    for (int i = 0; i < g_pending_n; i++) free(g_pending[i].url);
    free(g_pending); g_pending = NULL; g_pending_n = g_pending_cap = 0;
}
