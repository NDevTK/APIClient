/* solver/witness_verify.c — the session's witness verification queue (see witness_verify.h). */
#include "solver/witness_verify.h"
#include "solver/concolic.h"
#include "solver/flow.h"
#include "solver/world.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>

#define VFY_TAG "witness-verify"

/* One verification: the key it watches for and the substitution that tries it. `seeded` is set once a flow
   carries it, whether minted here or rebuilt from the cold tier, so a key is tried by one run per session. */
typedef struct { char *key; char *src; char *payload; int seeded; } Verify;
static Verify *g_vfy;
static int g_vfy_n, g_vfy_cap;

char *witness_verify_key(const char *method, const char *path, const char *witness_canon) {
    const char *f[3];
    char *k;

    DCHECK(method && *method && path && witness_canon && *witness_canon,
           "a witness verification key was asked for with no method, path or witness");
    f[0] = method; f[1] = path; f[2] = witness_canon;
    k = concolic_ident_compose(VFY_TAG, f, 3);
    CHECK(k, "witness_verify: OOM composing a verification key");
    return k;
}

/* One `<length>:<bytes>` field at `*p`, copied; advances past it. */
static char *key_field(const char **p, const char *key) {
    char *end, *out;
    unsigned long n = strtoul(*p, &end, 10);

    CHECKF(end != *p && *end == ':' && strlen(end + 1) >= n,
           "witness_verify: `%s` is not a verification key this file composed", key);
    out = malloc(n + 1);
    CHECK(out, "witness_verify: OOM splitting a verification key");
    memcpy(out, end + 1, n);
    out[n] = 0;
    *p = end + 1 + n;
    return out;
}

void witness_verify_key_split(const char *key, char **method, char **path, char **witness_canon) {
    const char *p = key;
    char *tag;

    CHECK(key != NULL, "witness_verify: a verification key was split that is not there");
    tag = key_field(&p, key);
    CHECKF(!strcmp(tag, VFY_TAG), "witness_verify: `%s` carries the tag `%s`, not this file's", key, tag);
    free(tag);
    *method = key_field(&p, key);
    *path = key_field(&p, key);
    *witness_canon = key_field(&p, key);
    CHECKF(*p == 0, "witness_verify: `%s` has bytes past its three fields", key);
}

static Verify *find(const char *key) {
    for (int i = 0; i < g_vfy_n; i++) if (!strcmp(g_vfy[i].key, key)) return &g_vfy[i];
    return NULL;
}

static Verify *append(const char *key, const char *src, const char *payload) {
    Verify *v;

    if (g_vfy_n >= g_vfy_cap) {
        int nc = g_vfy_cap ? g_vfy_cap * 2 : 8;
        Verify *nv = realloc(g_vfy, (size_t)nc * sizeof *nv);
        CHECK(nv, "witness_verify: OOM growing the verification queue");
        g_vfy = nv; g_vfy_cap = nc;
    }
    v = &g_vfy[g_vfy_n++];
    v->key = strdup(key);
    v->src = src ? strdup(src) : NULL;
    v->payload = payload ? strdup(payload) : NULL;
    v->seeded = 0;
    CHECK(v->key && (!src || v->src) && (!payload || v->payload), "witness_verify: OOM queueing a verification");
    return v;
}

void witness_verify_queue(const char *key, const char *src, const char *payload) {
    DCHECK(key && src && *src && payload && *payload,
           "a witness verification was queued without its key, source or payload — concolic_deliver refuses an "
           "empty payload, so the eligibility test must have declined it");
    if (find(key)) return;
    append(key, src, payload);
}

int witness_verify_seeds_owed(void) {
    for (int i = 0; i < g_vfy_n; i++)
        if (!g_vfy[i].seeded) return 1;
    return 0;
}

int witness_verify_seed(JSContext *ctx) {
    int added = 0;

    for (int i = 0; i < g_vfy_n; i++) {
        Verify *v = &g_vfy[i];
        Flow *f;

        if (v->seeded) continue;
        /* A fresh member from the baseline: it is entered like any new flow and explores like one, with the one
           source substituted. No recorded path is installed, because the claim under test is that the value
           alone reaches the request; a path would carry the assumptions being checked. */
        f = flow_add_unseeded(ctx, JS_UNDEFINED, WORLD_NONE);
        f->cand_src = strdup(v->src);
        f->cand_payload = strdup(v->payload);
        CHECK(f->cand_src && f->cand_payload, "witness_verify: OOM minting a verification flow");
        fire_target_request(&f->fire, v->key);
        flow_seed(f);   /* named first: its seed's parks are this tool's act */
        v->seeded = 1;
        added++;
    }
    return added;
}

void witness_verify_resumed(const char *key) {
    Verify *v = find(key);

    if (!v) v = append(key, NULL, NULL);
    v->seeded = 1;
}

void witness_verify_free(void) {
    for (int i = 0; i < g_vfy_n; i++) { free(g_vfy[i].key); free(g_vfy[i].src); free(g_vfy[i].payload); }
    free(g_vfy);
    g_vfy = NULL; g_vfy_n = g_vfy_cap = 0;
}
