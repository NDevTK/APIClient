/* solver/witness_verify.c — the session's witness verification queue (see witness_verify.h). */
#include "solver/witness_verify.h"
#include "solver/concolic.h"
#include "solver/flow.h"
#include "solver/world.h"
#include "check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solver/reclaim.h"

#define VFY_TAG "witness-verify"
#define VFY_GATES_TAG "witness-verify-gates"

/* One verification: the key it watches for and the substitution and further sources (`gsrc`/`gval`/`ghead`,
   `ng` of them, a head NULL for none) that try it. `seeded` is set once a flow carries it, whether minted here or
   rebuilt from the cold tier, so a key is tried by one run per session. */
typedef struct {
    char *key; char *src; char *payload; char *head;
    char **gsrc, **gval, **ghead; int ng, gcap;
    int seeded;
} Verify;
static Verify *g_vfy;
static int g_vfy_n, g_vfy_cap;

/* `<length>:<bytes>` for `field` appended to `*out` (owned, grown). */
static void field_put(char **out, size_t *len, const char *field) {
    size_t n = strlen(field);
    char pre[24];
    int pn = snprintf(pre, sizeof pre, "%zu:", n);
    char *g = reclaim_realloc(*out, *len + (size_t)pn + n + 1);

    CHECK(g, "witness_verify: OOM spelling a verification's further sources");
    memcpy(g + *len, pre, (size_t)pn);
    memcpy(g + *len + pn, field, n);
    *len += (size_t)pn + n;
    g[*len] = 0;
    *out = g;
}

/* concolic_ident_compose's fields, spelled here because that composer digests a long spelling, which
   witness_verify_key_split could not read back. */
char *witness_verify_key(const char *method, const char *path, const char *witness_canon) {
    char *k = NULL;
    size_t len = 0;

    DCHECK(method && *method && path && witness_canon && *witness_canon,
           "a witness verification key was asked for with no method, path or witness");
    field_put(&k, &len, VFY_TAG);
    field_put(&k, &len, method);
    field_put(&k, &len, path);
    field_put(&k, &len, witness_canon);
    return k;
}

/* One `<length>:<bytes>` field at `*p`, copied; advances past it. */
static char *key_field(const char **p, const char *key) {
    char *end, *out;
    unsigned long n = strtoul(*p, &end, 10);

    CHECKF(end != *p && *end == ':' && strlen(end + 1) >= n,
           "witness_verify: `%s` is not a verification key this file composed", key);
    out = reclaim_malloc(n + 1);
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

static Verify *append(const char *key, const char *src, const char *payload, const char *head) {
    Verify *v;

    if (g_vfy_n >= g_vfy_cap) {
        int nc = g_vfy_cap ? g_vfy_cap * 2 : 8;
        Verify *nv = realloc(g_vfy, (size_t)nc * sizeof *nv);
        CHECK(nv, "witness_verify: OOM growing the verification queue");
        g_vfy = nv; g_vfy_cap = nc;
    }
    v = &g_vfy[g_vfy_n++];
    v->key = reclaim_strdup(key);
    v->src = src ? reclaim_strdup(src) : NULL;
    v->payload = payload ? reclaim_strdup(payload) : NULL;
    v->head = head ? reclaim_strdup(head) : NULL;
    v->gsrc = v->gval = v->ghead = NULL;
    v->ng = v->gcap = 0;
    v->seeded = 0;
    CHECK(v->key && (!src || v->src) && (!payload || v->payload) && (!head || v->head),
          "witness_verify: OOM queueing a verification");
    return v;
}

/* One further source onto `v`, each field copied (`head` NULL for none). */
static void gate_append(Verify *v, const char *src, const char *val, const char *head) {
    DCHECK(src && *src && val && *val, "a verification's further source has no name or value");
    if (v->ng >= v->gcap) {
        int nc = v->gcap ? v->gcap * 2 : 2;
        char **s = realloc(v->gsrc, (size_t)nc * sizeof *s), **l, **h;

        CHECK(s, "witness_verify: OOM growing a verification's further sources");
        v->gsrc = s;
        l = realloc(v->gval, (size_t)nc * sizeof *l);
        CHECK(l, "witness_verify: OOM growing a verification's further sources");
        v->gval = l;
        h = realloc(v->ghead, (size_t)nc * sizeof *h);
        CHECK(h, "witness_verify: OOM growing a verification's further sources");
        v->ghead = h;
        v->gcap = nc;
    }
    v->gsrc[v->ng] = reclaim_strdup(src);
    v->gval[v->ng] = reclaim_strdup(val);
    v->ghead[v->ng] = head ? reclaim_strdup(head) : NULL;
    CHECK(v->gsrc[v->ng] && v->gval[v->ng] && (!head || v->ghead[v->ng]),
          "witness_verify: OOM queueing a verification's further source");
    v->ng++;
}

void witness_verify_queue(const char *key, const char *src, const char *payload, const char *head,
                          const WitnessVerifyGate *gates, int ngate) {
    Verify *v;

    DCHECK(key && src && *src && payload && *payload && ngate >= 0 && (ngate == 0 || gates),
           "a witness verification was queued without its key, source or payload — concolic_deliver refuses an "
           "empty payload, so the eligibility test must have declined it");
    if (find(key)) return;
    v = append(key, src, payload, head);
    for (int i = 0; i < ngate; i++) gate_append(v, gates[i].src, gates[i].val, gates[i].head);
}

int witness_verify_gates(const char *key, char *const **srcs, char *const **vals, char *const **heads) {
    const Verify *v = find(key);

    DCHECK(v != NULL, "the further sources of a verification this session never queued or resumed were asked for");
    *srcs = v ? v->gsrc : NULL;
    *vals = v ? v->gval : NULL;
    *heads = v ? v->ghead : NULL;
    return v ? v->ng : 0;
}

/* key_field's fields, never digested: the tag, then each source, its value and its head (empty for none, which
   concolic_url_head never composes). */
char *witness_verify_gates_text(const char *key) {
    const Verify *v = find(key);
    char *out = NULL;
    size_t len = 0;

    DCHECK(v != NULL, "the further sources of a verification this session never queued or resumed were spelled");
    field_put(&out, &len, VFY_GATES_TAG);
    for (int i = 0; v && i < v->ng; i++) {
        field_put(&out, &len, v->gsrc[i]);
        field_put(&out, &len, v->gval[i]);
        field_put(&out, &len, v->ghead[i] ? v->ghead[i] : "");
    }
    return out;
}

const char *witness_verify_head(const char *key) {
    const Verify *v = find(key);

    DCHECK(v != NULL, "the head of a verification this session never queued or resumed was asked for");
    return v ? v->head : NULL;
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
        f->cand_src = reclaim_strdup(v->src);
        f->cand_payload = reclaim_strdup(v->payload);
        CHECK(f->cand_src && f->cand_payload, "witness_verify: OOM minting a verification flow");
        flow_observed_class_moved(f);
        fire_target_request(&f->fire, v->key);
        flow_seed(f);   /* named first: its seed's parks are this tool's act */
        v->seeded = 1;
        added++;
    }
    return added;
}

void witness_verify_resumed(const char *key, const char *head, const char *gates_text) {
    Verify *v = find(key);
    const char *p = gates_text;
    char *tag;

    CHECK(gates_text != NULL, "witness_verify: a rebuilt verification carried no further-sources text");
    if (v) { v->seeded = 1; return; }
    v = append(key, NULL, NULL, head);
    v->seeded = 1;
    tag = key_field(&p, gates_text);
    CHECKF(!strcmp(tag, VFY_GATES_TAG), "witness_verify: `%s` is not a further-sources text this file spelled",
           gates_text);
    free(tag);
    while (*p) {
        char *src = key_field(&p, gates_text), *val = key_field(&p, gates_text), *h = key_field(&p, gates_text);

        gate_append(v, src, val, *h ? h : NULL);
        free(src); free(val); free(h);
    }
}

void witness_verify_free(void) {
    for (int i = 0; i < g_vfy_n; i++) {
        free(g_vfy[i].key); free(g_vfy[i].src); free(g_vfy[i].payload); free(g_vfy[i].head);
        for (int k = 0; k < g_vfy[i].ng; k++) { free(g_vfy[i].gsrc[k]); free(g_vfy[i].gval[k]); free(g_vfy[i].ghead[k]); }
        free(g_vfy[i].gsrc); free(g_vfy[i].gval); free(g_vfy[i].ghead);
    }
    free(g_vfy);
    g_vfy = NULL; g_vfy_n = g_vfy_cap = 0;
}
