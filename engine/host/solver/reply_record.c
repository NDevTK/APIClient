/* The reply record store and the reply trail (solver/reply_record.h). */
#include "solver/reply_record.h"

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/crypto/secure_hash.h"
#include "solver/pending.h"   /* the PROV_ values a record's reach and grade are stated in */
#include "solver/reclaim.h"

const char *reply_how_token(ReplyHow how) {
    switch (how) {
    case REPLY_HOW_SENT:             return REPLY_HOW_TOKEN_SENT;
    case REPLY_HOW_ANSWERED_LOCALLY: return REPLY_HOW_TOKEN_ANSWERED_LOCALLY;
    case REPLY_HOW_UNKNOWN:          return REPLY_HOW_TOKEN_UNKNOWN;
    }
    CHECK_FAILF("engine: a reply record states the answer kind %d, which is none of sent, answered-locally and "
                "unknown", (int)how);
}

ReplyHow reply_how_of_token(const char *token) {
    CHECK(token != NULL, "engine: a reply was provided without the zone's statement of how it was answered");
    if (!strcmp(token, REPLY_HOW_TOKEN_SENT)) return REPLY_HOW_SENT;
    if (!strcmp(token, REPLY_HOW_TOKEN_ANSWERED_LOCALLY)) return REPLY_HOW_ANSWERED_LOCALLY;
    if (!strcmp(token, REPLY_HOW_TOKEN_UNKNOWN)) return REPLY_HOW_UNKNOWN;
    CHECK_FAILF("engine: a reply was provided with the answer kind `%s`, which is none of sent, "
                "answered-locally and unknown — the zone is writing another vocabulary", token);
}

void reply_record_key_body(ReplyRecordKey *k, const uint8_t *body, size_t n) {
    SecureHash h;

    DCHECK(k != NULL, "a request body was digested into no key");
    memset(k->body_digest, 0, sizeof k->body_digest);
    k->has_body = body != NULL;
    if (!body) {
        DCHECK(n == 0, "a null request body states a length");
        return;
    }
    secure_hash_init(&h, SECURE_HASH_SHA256);
    secure_hash_update(&h, body, n);
    secure_hash_finish(&h, k->body_digest, sizeof k->body_digest);
}

/* The records of one key, in arrival order. */
typedef struct ReplyKeyNode {
    struct ReplyKeyNode *hnext;
    uint32_t hash;
    char *method, *mode, *referrer_policy, *referrer, *url, *credentials;
    int has_body;
    uint8_t body_digest[REPLY_BODY_DIGEST_LEN];
    ReplyRecord **recs;
    long n, cap;
} ReplyKeyNode;

/* Named residual. Not covered: a record has no cross-session name, and its reply stays resident for the life of
   the instance. Next diff builds: the store's writer for the cold tier, which names a record by the digest of
   the bytes it writes and relocates them under RAM pressure. Absence shows as: a trail row another session
   cannot resolve, and instance memory that grows with the bytes of every reply answered. */
struct ReplyRecord {
    uint64_t id;
    ReplyKeyNode *key;
    long seq;
    char *askers;
    int reach, prov, driven;
    ReplyHow how;
    JSValue reply;
};

/* Records by id: `g_recs[id - 1]`. */
static ReplyRecord **g_recs;
static uint64_t g_recs_n, g_recs_cap;
static ReplyKeyNode **g_keys;
static uint32_t g_key_buckets, g_key_n;

static uint32_t reply_hash_bytes(const char *s, size_t n, uint32_t h) {
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}

/* FNV-1a over the six strings, each followed by a TAB so a field boundary is part of the key, then the body
   mark and digest. */
static uint32_t reply_key_hash(const ReplyRecordKey *k) {
    const char *f[] = { k->method, k->mode, k->referrer_policy, k->referrer, k->url, k->credentials };
    uint32_t h = 2166136261u;
    char mark = k->has_body ? '1' : '0';

    for (size_t i = 0; i < sizeof f / sizeof f[0]; i++) {
        h = reply_hash_bytes(f[i], strlen(f[i]), h);
        h = reply_hash_bytes("\t", 1, h);
    }
    h = reply_hash_bytes(&mark, 1, h);
    return reply_hash_bytes((const char *)k->body_digest, sizeof k->body_digest, h);
}

static int reply_key_whole(const ReplyRecordKey *k) {
    return k && k->method && k->mode && k->referrer_policy && k->referrer && k->url && k->credentials;
}

static int reply_node_is(const ReplyKeyNode *n, uint32_t h, const ReplyRecordKey *k) {
    return n->hash == h && n->has_body == (k->has_body != 0) &&
           !memcmp(n->body_digest, k->body_digest, sizeof n->body_digest) && !strcmp(n->method, k->method) &&
           !strcmp(n->mode, k->mode) && !strcmp(n->referrer_policy, k->referrer_policy) &&
           !strcmp(n->referrer, k->referrer) && !strcmp(n->url, k->url) &&
           !strcmp(n->credentials, k->credentials);
}

static ReplyKeyNode *reply_node_find(const ReplyRecordKey *k, uint32_t h) {
    ReplyKeyNode *n;

    if (!g_key_buckets) return NULL;
    for (n = g_keys[h & (g_key_buckets - 1)]; n; n = n->hnext)
        if (reply_node_is(n, h, k)) return n;
    return NULL;
}

/* Doubles the key table once it holds a node per bucket. The bucket count stays a power of two. */
static void reply_keys_grow(void) {
    uint32_t nb = g_key_buckets ? g_key_buckets * 2 : 64;
    ReplyKeyNode **tab;

    if (g_key_buckets && g_key_n < g_key_buckets) return;
    tab = reclaim_calloc(nb, sizeof *tab);
    CHECK(tab != NULL, "engine: OOM growing the reply record store's key table");
    for (uint32_t i = 0; i < g_key_buckets; i++)
        while (g_keys[i]) {
            ReplyKeyNode *n = g_keys[i];

            g_keys[i] = n->hnext;
            n->hnext = tab[n->hash & (nb - 1)];
            tab[n->hash & (nb - 1)] = n;
        }
    free(g_keys);
    g_keys = tab;
    g_key_buckets = nb;
}

static char *reply_copy(const char *s) {
    char *c = reclaim_strdup(s);

    CHECK(c != NULL, "engine: OOM copying a reply record's key");
    return c;
}

uint64_t reply_record_add(JSContext *ctx, const ReplyRecordKey *k, const char *askers, int reach, int prov,
                          int driven, ReplyHow how, JSValueConst reply) {
    ReplyKeyNode *node;
    ReplyRecord *r;
    uint32_t h;

    DCHECK(reply_key_whole(k), "a reply was recorded under a key missing one of its six strings");
    DCHECK(askers != NULL && *askers, "a reply was recorded with no asking document");
    DCHECK(reach >= PROV_OBSERVED && reach <= PROV_FORCED && prov >= PROV_OBSERVED && prov <= PROV_FORCED,
           "a reply was recorded with a reach or a grade outside the three provenances");
    DCHECK(driven == 0 || driven == 1, "a reply was recorded with a drive mark that is not a bit");
    DCHECK(JS_IsObject(reply) || JS_IsNull(reply),
           "a reply was recorded that is neither the host's record nor the network error");
    (void)reply_how_token(how);
    h = reply_key_hash(k);
    node = reply_node_find(k, h);
    if (!node) {
        reply_keys_grow();
        node = reclaim_calloc(1, sizeof *node);
        CHECK(node != NULL, "engine: OOM recording a reply's key");
        node->hash = h;
        node->method = reply_copy(k->method);
        node->mode = reply_copy(k->mode);
        node->referrer_policy = reply_copy(k->referrer_policy);
        node->referrer = reply_copy(k->referrer);
        node->url = reply_copy(k->url);
        node->credentials = reply_copy(k->credentials);
        node->has_body = k->has_body != 0;
        memcpy(node->body_digest, k->body_digest, sizeof node->body_digest);
        node->hnext = g_keys[h & (g_key_buckets - 1)];
        g_keys[h & (g_key_buckets - 1)] = node;
        g_key_n++;
    }
    r = reclaim_calloc(1, sizeof *r);
    CHECK(r != NULL, "engine: OOM recording a reply");
    r->id = g_recs_n + 1;
    r->key = node;
    r->seq = node->n;
    r->askers = reply_copy(askers);
    r->reach = reach;
    r->prov = prov;
    r->driven = driven;
    r->how = how;
    r->reply = JS_DupValue(ctx, reply);
    if (node->n == node->cap) {
        node->cap = node->cap ? node->cap * 2 : 2;
        node->recs = reclaim_realloc(node->recs, (size_t)node->cap * sizeof *node->recs);
        CHECK(node->recs != NULL, "engine: OOM growing a key's reply records");
    }
    node->recs[node->n++] = r;
    if (g_recs_n == g_recs_cap) {
        g_recs_cap = g_recs_cap ? g_recs_cap * 2 : 64;
        g_recs = reclaim_realloc(g_recs, (size_t)g_recs_cap * sizeof *g_recs);
        CHECK(g_recs != NULL, "engine: OOM growing the reply record store");
    }
    g_recs[g_recs_n++] = r;
    return r->id;
}

uint64_t reply_record_last_id(void) { return g_recs_n; }

const ReplyRecord *reply_record_by_id(uint64_t id) {
    CHECKF(id >= 1 && id <= g_recs_n,
           "engine: a reply record was asked for by the id %llu and the store has issued %llu",
           (unsigned long long)id, (unsigned long long)g_recs_n);
    return g_recs[id - 1];
}

long reply_record_key_count(const ReplyRecordKey *k) {
    ReplyKeyNode *node;

    DCHECK(reply_key_whole(k), "the reply record store was asked about a key missing one of its six strings");
    node = reply_node_find(k, reply_key_hash(k));
    return node ? node->n : 0;
}

const ReplyRecord *reply_record_by_key(const ReplyRecordKey *k, long seq) {
    ReplyKeyNode *node;

    DCHECK(reply_key_whole(k) && seq >= 0,
           "the reply record store was asked for a negative answer ordinal, or by a key missing a string");
    node = reply_node_find(k, reply_key_hash(k));
    return node && seq < node->n ? node->recs[seq] : NULL;
}

uint64_t    reply_record_id(const ReplyRecord *r)     { return r->id; }
long        reply_record_seq(const ReplyRecord *r)    { return r->seq; }
const char *reply_record_askers(const ReplyRecord *r) { return r->askers; }
int         reply_record_reach(const ReplyRecord *r)  { return r->reach; }
int         reply_record_prov(const ReplyRecord *r)   { return r->prov; }
int         reply_record_driven(const ReplyRecord *r) { return r->driven; }
ReplyHow    reply_record_how(const ReplyRecord *r)    { return r->how; }
JSValue     reply_record_reply(JSContext *ctx, const ReplyRecord *r) { return JS_DupValue(ctx, r->reply); }

struct ReplyTrailRow {
    ReplyTrailRow *up;
    long refs;
    int64_t slot;
    uint64_t id;
    int64_t path_work;
};

static long g_trail_total, g_trail_now;

ReplyTrailRow *reply_trail_push(ReplyTrailRow *head, int64_t slot, uint64_t id, int64_t path_work) {
    ReplyTrailRow *row;

    DCHECK(slot > 0, "a trail row names no park — a park's ordinal on its path starts at 1");
    DCHECK(id >= 1 && id <= g_recs_n, "a trail row names a reply record the store never issued");
    DCHECK(!head || (id > head->id && path_work >= head->path_work),
           "a trail row is older than the row it follows — a flow takes its records in arrival order, at "
           "coordinates that do not fall");
    row = reclaim_calloc(1, sizeof *row);
    CHECK(row != NULL, "engine: OOM appending a reply trail row");
    row->up = head;
    row->refs = 1;
    row->slot = slot;
    row->id = id;
    row->path_work = path_work;
    g_trail_total++;
    g_trail_now++;
    return row;
}

ReplyTrailRow *reply_trail_share(ReplyTrailRow *head) {
    if (head) head->refs++;
    return head;
}

void reply_trail_release(ReplyTrailRow *head) {
    while (head) {
        ReplyTrailRow *up = head->up;

        DCHECK(head->refs > 0, "a reply trail row was released more often than it was held");
        if (--head->refs > 0) return;
        free(head);
        g_trail_now--;
        head = up;
    }
}

const ReplyTrailRow *reply_trail_up(const ReplyTrailRow *row) { return row->up; }
int64_t  reply_trail_slot(const ReplyTrailRow *row)           { return row->slot; }
uint64_t reply_trail_id(const ReplyTrailRow *row)             { return row->id; }
int64_t  reply_trail_path_work(const ReplyTrailRow *row)      { return row->path_work; }

long reply_trail_rows_total(void) { return g_trail_total; }
long reply_trail_rows_now(void)   { return g_trail_now; }

void reply_record_reset(JSContext *ctx) {
    DCHECK(g_trail_now == 0,
           "a reply trail row outlived the frontier — every flow releases its trail when it is released");
    for (uint64_t i = 0; i < g_recs_n; i++) {
        JS_FreeValue(ctx, g_recs[i]->reply);
        free(g_recs[i]->askers);
        free(g_recs[i]);
    }
    for (uint32_t i = 0; i < g_key_buckets; i++)
        while (g_keys[i]) {
            ReplyKeyNode *n = g_keys[i];

            g_keys[i] = n->hnext;
            free(n->method); free(n->mode); free(n->referrer_policy); free(n->referrer); free(n->url);
            free(n->credentials); free(n->recs); free(n);
        }
    free(g_recs);
    free(g_keys);
    g_recs = NULL; g_recs_n = g_recs_cap = 0;
    g_keys = NULL; g_key_buckets = g_key_n = 0;
    g_trail_total = 0;
}
