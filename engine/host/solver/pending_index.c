/* The frontier's outstanding requests — see pending_index.h. */
#include "solver/pending_index.h"
#include "solver/pending.h"   /* the session's context; this file allocates no realm state of its own */
#include "check.h"

#include <stdlib.h>
#include <string.h>

/* A RECORD THIS INDEX IS TRACKING. `namers` is how many REGISTERS name it, which is the one fact neither the
   record nor the register carries: a fork shares the record, so the record cannot say how many arms hold it and
   any one register only knows about itself. It is counted here because here is where the answer is needed —
   the last drop is what removes the request from the set the host is shown.
   `node` is NULL until the record carries both halves of its identity. A record is pushed with neither and the
   park sets them one at a time, so there is a window in which a tracked record has no pair; it is not a state
   to be defaulted past, it is the record being built. */
typedef struct PendIndexMember {
    JSValue                 rec;    /* OWNED — so a namer count that ever goes wrong is a LEAK the runtime's
                                       own gc_obj_list walk reports, and never a read through a freed record */
    int                     namers;
    PendIndexNode          *node;
    struct PendIndexMember *hnext;  /* the by-record chain */
} PendIndexMember;

/* A REQUEST — the (method, url) pair the reply seam is keyed on (engine.h), with the records still parked on it.
   IT OUTLIVES ITS MEMBERS. `answered` is what tells a host that answered one request twice from a host whose
   pairing is off, and both of those are silent in every other way. The count of distinct pairs a session issues
   is the endpoint surface, which this engine already keeps whole. */
struct PendIndexNode {
    char             *method;
    char             *url;
    uint32_t          hash;
    PendIndexMember **mem;
    int               n_mem, cap_mem;
    long              answered;
    PendIndexNode    *hnext;   /* the by-pair chain */
    PendIndexNode    *anext;   /* every node, in creation order — the join's walk */
};

/* TWO LOOKUPS OVER ONE SET, because the two doors ask different questions of it: the reply door has a PAIR and
   wants the records, and every membership mutation has a RECORD and wants its member. Open chaining, doubled
   when the load reaches one, which is the shape quickjs's own tables have. */
static PendIndexMember **g_by_rec;
static uint32_t          g_rec_buckets;
static uint32_t          g_rec_n;
static PendIndexNode   **g_by_pair;
static uint32_t          g_pair_buckets;
static uint32_t          g_pair_n;
static PendIndexNode    *g_nodes;       /* the walk's head */
static PendIndexNode    *g_nodes_tail;  /* appended in creation order, so the join's line order is stable */

/* THE JOIN'S ORDER IS AN OBSERVABLE OF THE HOST, so the walk is in creation order rather than in whatever
   order a hash happens to bucket. A zone's firing decision reading a list whose order moved between two runs
   of one document is the same defect as a value read two ways on two runs. */

static uint32_t pend_hash_bytes(const char *s, uint32_t h)
{
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

static uint32_t pend_hash_pair(const char *method, const char *url)
{
    uint32_t h = pend_hash_bytes(method, 2166136261u);
    h = pend_hash_bytes("\t", h);       /* the separator is part of the key: `GET` + `\tx` is not `GET\t` + `x` */
    return pend_hash_bytes(url, h);
}

static uint32_t pend_hash_ptr(JSValueConst rec)
{
    uintptr_t p = (uintptr_t)JS_VALUE_GET_PTR(rec);
    uint64_t x = (uint64_t)p;
    /* A POINTER'S LOW BITS ARE ITS ALIGNMENT AND ITS HIGH BITS ARE THE ALLOCATOR'S — neither distinguishes two
       records on their own, so the two halves are mixed before the mask takes the low ones. */
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    return (uint32_t)x;
}

static void pend_rec_grow(void)
{
    uint32_t nb = g_rec_buckets ? g_rec_buckets * 2 : 64, i;
    PendIndexMember **tab = calloc(nb, sizeof *tab);
    CHECK(tab != NULL, "engine: OOM growing the frontier's outstanding-request index");
    for (i = 0; i < g_rec_buckets; i++) {
        PendIndexMember *m = g_by_rec[i];
        while (m) {
            PendIndexMember *next = m->hnext;
            uint32_t b = pend_hash_ptr(m->rec) & (nb - 1);
            m->hnext = tab[b];
            tab[b] = m;
            m = next;
        }
    }
    free(g_by_rec);
    g_by_rec = tab;
    g_rec_buckets = nb;
}

static void pend_pair_grow(void)
{
    uint32_t nb = g_pair_buckets ? g_pair_buckets * 2 : 64, i;
    PendIndexNode **tab = calloc(nb, sizeof *tab);
    CHECK(tab != NULL, "engine: OOM growing the frontier's outstanding-request index");
    for (i = 0; i < g_pair_buckets; i++) {
        PendIndexNode *n = g_by_pair[i];
        while (n) {
            PendIndexNode *next = n->hnext;
            uint32_t b = n->hash & (nb - 1);
            n->hnext = tab[b];
            tab[b] = n;
            n = next;
        }
    }
    free(g_by_pair);
    g_by_pair = tab;
    g_pair_buckets = nb;
}

static PendIndexMember *pend_member(JSValueConst rec)
{
    PendIndexMember *m;
    if (!g_rec_buckets) return NULL;
    for (m = g_by_rec[pend_hash_ptr(rec) & (g_rec_buckets - 1)]; m; m = m->hnext)
        if (JS_VALUE_GET_PTR(m->rec) == JS_VALUE_GET_PTR(rec)) return m;
    return NULL;
}

/* TAKE THE RECORD OUT OF ITS PAIR — the members array is a SET, so the hole is filled by the last one, exactly
   as pending_remove's swap-remove is and for the same reason. */
static void pend_unkey(PendIndexMember *m)
{
    PendIndexNode *n = m->node;
    int i;
    DCHECK(n != NULL, "a record was unkeyed from a pair it was never keyed under");
    for (i = 0; i < n->n_mem; i++)
        if (n->mem[i] == m) {
            n->mem[i] = n->mem[n->n_mem - 1];
            n->n_mem--;
            m->node = NULL;
            return;
        }
    DFAIL("a record names a pair whose member list does not hold it — the two halves of one membership have "
          "come apart, and the record would then be listed to the host by a walk that can never remove it");
}

static void pend_untrack(PendIndexMember *m)
{
    uint32_t b = pend_hash_ptr(m->rec) & (g_rec_buckets - 1);
    PendIndexMember **pp = &g_by_rec[b];
    if (m->node) pend_unkey(m);
    while (*pp && *pp != m) pp = &(*pp)->hnext;
    DCHECK(*pp == m, "a tracked record is not in the bucket its own pointer hashes to — the record moved, "
                     "which a JS object does not do, or the table was rebuilt without it");
    *pp = m->hnext;
    g_rec_n--;
    JS_FreeValue(pending_ctx(), m->rec);
    free(m);
}

void pending_index_track(JSValueConst rec)
{
    PendIndexMember *m;
    uint32_t b;

    DCHECK(JS_IsObject(rec), "the outstanding-request index was asked to track something that is not a record");
    DCHECK(pend_member(rec) == NULL,
           "a pending record was tracked twice — tracking begins at the ONE push that creates a record, so a "
           "second one is a record built outside pending_push or a pointer the runtime has already reused for "
           "an object this index never let go of");
    if (g_rec_n >= g_rec_buckets) pend_rec_grow();
    m = calloc(1, sizeof *m);
    CHECK(m != NULL, "engine: OOM tracking a pending record — an untracked outstanding request is one the host "
                     "is never shown, and the flow parked on it waits for the rest of the session");
    m->rec = JS_DupValue(pending_ctx(), rec);
    m->namers = 1;   /* the register it was just pushed onto; a fork adds the rest */
    b = pend_hash_ptr(rec) & (g_rec_buckets - 1);
    m->hnext = g_by_rec[b];
    g_by_rec[b] = m;
    g_rec_n++;
}

int pending_index_tracked(JSValueConst rec) { return pend_member(rec) != NULL; }

int pending_index_keyed(JSValueConst rec)
{
    PendIndexMember *m = pend_member(rec);
    return m != NULL && m->node != NULL;
}

void pending_index_ref(JSValueConst rec)
{
    PendIndexMember *m = pend_member(rec);
    if (!m) return;   /* answered, synchronous, or already dropped by every register — see pending_index.h */
    DCHECK(m->namers > 0, "a record with no register naming it was inherited by a fork — it should have left "
                          "this index at the drop that took its last namer");
    m->namers++;
}

void pending_index_unref(JSValueConst rec)
{
    PendIndexMember *m = pend_member(rec);
    if (!m) return;
    DCHECK(m->namers > 0, "a register gave back a naming of a record it does not hold — the count is one per "
                          "register slot, so this is a second free of one slot or a drop of a record that "
                          "was never on this register");
    if (--m->namers == 0) pend_untrack(m);
}

void pending_index_key(JSValueConst rec, const char *method, const char *url)
{
    PendIndexMember *m = pend_member(rec);
    PendIndexNode *n;
    uint32_t h, b;

    DCHECK(method != NULL && url != NULL,
           "a request was keyed into the outstanding set with half a pair — the reply seam is keyed on "
           "(method, url) and a half key would collect another request's body");
    DCHECK(m != NULL, "an untracked record was keyed — tracking begins at the push and a record that reaches "
                      "this line without it is one the last register already dropped");
    DCHECK(m->node == NULL, "a record was keyed twice — a request's identity is written once, at the park, so "
                            "a second key is either a rewritten address or a member the first key lost");
    h = pend_hash_pair(method, url);
    if (!g_pair_buckets) pend_pair_grow();
    for (n = g_by_pair[h & (g_pair_buckets - 1)]; n; n = n->hnext)
        if (n->hash == h && !strcmp(n->method, method) && !strcmp(n->url, url)) break;
    if (!n) {
        if (g_pair_n >= g_pair_buckets) pend_pair_grow();
        n = calloc(1, sizeof *n);
        CHECK(n != NULL, "engine: OOM recording an outstanding request");
        n->method = strdup(method);
        n->url = strdup(url);
        CHECK(n->method && n->url, "engine: OOM recording an outstanding request's identity");
        n->hash = h;
        b = h & (g_pair_buckets - 1);
        n->hnext = g_by_pair[b];
        g_by_pair[b] = n;
        g_pair_n++;
        if (g_nodes_tail) g_nodes_tail->anext = n; else g_nodes = n;
        g_nodes_tail = n;
    }
    if (n->n_mem == n->cap_mem) {
        int nc = n->cap_mem ? n->cap_mem * 2 : 4;
        PendIndexMember **mm = realloc(n->mem, (size_t)nc * sizeof *mm);
        CHECK(mm != NULL, "engine: OOM parking a record on an outstanding request");
        n->mem = mm;
        n->cap_mem = nc;
    }
    n->mem[n->n_mem++] = m;
    m->node = n;
}

void pending_index_answered(JSValueConst rec)
{
    PendIndexMember *m = pend_member(rec);

    DCHECK(m != NULL, "a reply landed on a record this index is not tracking — a fetch record is tracked from "
                      "its push until it is answered, so this is a second answer to one request or a write "
                      "onto a record every register has already dropped");
    if (m->node) m->node->answered++;
    /* AND IT STOPS BEING TRACKED, NOT MERELY UNKEYED. An answered record can never re-enter the outstanding
       set — nothing ever clears `haveValue` — so keeping a member for it would grow this index with the very
       thing it exists to stop walking. The namer count goes with it, which is why the two calls above are
       documented as no-ops on an untracked record rather than as errors. */
    pend_untrack(m);
}

PendIndexNode *pending_index_first(void) { return g_nodes; }
PendIndexNode *pending_index_next(PendIndexNode *n)
{
    DCHECK(n != NULL, "the outstanding-request walk was advanced past its end");
    return n->anext;
}
const char *pending_index_node_method(const PendIndexNode *n)
{
    DCHECK(n != NULL, "an outstanding request's method was read off no node");
    return n->method;
}
const char *pending_index_node_url(const PendIndexNode *n)
{
    DCHECK(n != NULL, "an outstanding request's address was read off no node");
    return n->url;
}
int pending_index_node_count(const PendIndexNode *n)
{
    DCHECK(n != NULL, "an outstanding request's member count was read off no node");
    return n->n_mem;
}
JSValue pending_index_node_member(const PendIndexNode *n, int i)
{
    DCHECK(n != NULL && i >= 0 && i < n->n_mem,
           "an outstanding request's member was asked for past the end of its list");
    return JS_DupValue(pending_ctx(), n->mem[i]->rec);
}
long pending_index_node_answered(const PendIndexNode *n)
{
    DCHECK(n != NULL, "a request's answered count was read off no node");
    return n->answered;
}

PendIndexNode *pending_index_find(const char *method, const char *url)
{
    uint32_t h;
    PendIndexNode *n;
    DCHECK(method != NULL && url != NULL, "the outstanding set was searched with half a request identity");
    if (!g_pair_buckets) return NULL;
    h = pend_hash_pair(method, url);
    for (n = g_by_pair[h & (g_pair_buckets - 1)]; n; n = n->hnext)
        if (n->hash == h && !strcmp(n->method, method) && !strcmp(n->url, url)) return n;
    return NULL;
}

void pending_index_reset(JSContext *ctx)
{
    PendIndexNode *n;
    uint32_t i;

    /* EVERY REGISTER IS GONE BY HERE, SO EVERY NAMING IS TOO — flow_registry_free releases each flow's register
       before it reaches this line, and each release gives back one naming per entry. A member still standing is
       therefore a register that was freed without giving its namings back, which is the ONE way the count can
       be wrong in the direction that matters: it would have kept a request in the host's list for ever. */
    DCHECK(g_rec_n == 0,
           "the frontier's outstanding-request index still names records after every register was released — a "
           "naming was taken at a fork or a push and never given back, so this index was holding requests no "
           "flow was parked on and the host was being shown them every slice");
    for (i = 0; i < g_rec_buckets; i++)
        while (g_by_rec[i]) {
            PendIndexMember *m = g_by_rec[i];
            g_by_rec[i] = m->hnext;
            if (m->node) pend_unkey(m);
            JS_FreeValue(ctx, m->rec);
            free(m);
        }
    free(g_by_rec); g_by_rec = NULL; g_rec_buckets = 0; g_rec_n = 0;
    n = g_nodes;
    while (n) {
        PendIndexNode *next = n->anext;
        free(n->method);
        free(n->url);
        free(n->mem);
        free(n);
        n = next;
    }
    free(g_by_pair); g_by_pair = NULL; g_pair_buckets = 0; g_pair_n = 0;
    g_nodes = NULL; g_nodes_tail = NULL;
}
