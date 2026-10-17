/* Which bytes a program name was compiled from — see script_digest.h. */
#include "solver/script_digest.h"
#include "solver/compose.h"
#include "check.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* One name: its digest, or `mixed` once a second byte sequence was compiled under it. Open-addressed by an FNV-1a
   of the name at load <= 1/2, so a program row's note is O(1) expected; grown by doubling, released with the
   agent. A note is not page state: which bytes a name stood for is the same fact in every flow. */
typedef struct { char *name; char digest[65]; int mixed; } ScriptName;
static ScriptName *g_names;
static size_t g_cap, g_used;

static uint64_t name_hash(const char *s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 0x100000001b3ULL; }
    return h;
}

static ScriptName *name_slot(const char *name) {
    size_t i;
    if (!g_cap) return NULL;
    for (i = (size_t)name_hash(name) & (g_cap - 1);; i = (i + 1) & (g_cap - 1))
        if (!g_names[i].name || !strcmp(g_names[i].name, name)) return &g_names[i];
}

static void names_grow(void) {
    ScriptName *old = g_names;
    size_t ocap = g_cap;
    g_cap = g_cap ? g_cap * 2 : 64;
    g_names = calloc(g_cap, sizeof *g_names);
    CHECK(g_names, "script_digest: OOM growing the program-name table");
    for (size_t i = 0; i < ocap; i++) if (old[i].name) *name_slot(old[i].name) = old[i];
    free(old);
}

void script_digest_note(const char *name, const DynBody *body) {
    ScriptName *s;
    const char *d = dyn_body_digest(body);
    DCHECK(name && *name, "a program was compiled under no name — engine.c names every row before its compile");
    if ((g_used + 1) * 2 > g_cap) names_grow();
    s = name_slot(name);
    if (!s->name) {
        s->name = strdup(name);
        CHECK(s->name, "script_digest: OOM copying a program name");
        memcpy(s->digest, d, sizeof s->digest);
        g_used++;
        return;
    }
    if (memcmp(s->digest, d, sizeof s->digest)) s->mixed = 1;
}

char *script_digest_identity(const char *name) {
    const ScriptName *s = name_slot(name);
    char *out = (s && s->name && !s->mixed) ? composef("sha256 %s", s->digest) : composef("address %s", name);
    CHECK(out, "script_digest: OOM composing a script's identity");
    return out;
}

void script_digest_free(void) {
    for (size_t i = 0; i < g_cap; i++) free(g_names[i].name);
    free(g_names); g_names = NULL; g_cap = g_used = 0;
}
