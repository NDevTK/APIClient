/* Novelty against the product — see novelty.h. */
#include "solver/novelty.h"
#include "core/crypto/secure_hash.h"
#include "core/json_buf.h"
#include "solver/compose.h"
#include "solver/flow.h"
#include "check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solver/reclaim.h"

static const char *const novelty_kind_token[NOVELTY_KIND_N] = {
#define NOVELTY_KIND_TOKEN(id, token, counted) [id] = token,
    NOVELTY_KINDS(NOVELTY_KIND_TOKEN)
#undef NOVELTY_KIND_TOKEN
};
static const int novelty_kind_counted[NOVELTY_KIND_N] = {
#define NOVELTY_KIND_COUNTED(id, token, counted) [id] = counted,
    NOVELTY_KINDS(NOVELTY_KIND_COUNTED)
#undef NOVELTY_KIND_COUNTED
};

/* One slot of the open-addressed set. `kind` is kept beside the digest only for the emitted line; the digest
   alone is the identity, since the kind's token is its second digested field. */
enum { SLOT_KNOWN = 1, SLOT_OWN = 2 };
typedef struct { NoveltyId id; uint8_t flags; uint8_t kind; uint32_t known_n, own_n; } NoveltySlot;

/* Linear probing at a load factor of at most one half, so a lookup is O(1) expected at any size; the slot index
   is the digest's low word, which is already uniform. Grown by doubling, never shrunk, released at the agent's
   release. */
static NoveltySlot *g_slots;
static size_t g_cap, g_used;
static long g_known_n, g_own_n, g_own_known_n;
static long g_refused_texts;
static char *g_scope;
/* The credit census, per kind: an ask is one novelty_credit call, which is paid or known-unpaid. */
static long g_paid[NOVELTY_KIND_N], g_unpaid[NOVELTY_KIND_N];
/* Every drive this instance filed, with the product's count it read and its completed units at the seed. */
typedef struct { NoveltyId id; unsigned seen; int64_t visits; } NoveltyDrive;
static NoveltyDrive *g_drives;
static long g_drives_n, g_drives_cap, g_drives_lowered;
static double g_landed;

static int id_eq(NoveltyId a, NoveltyId b) { return a.hi == b.hi && a.lo == b.lo; }

static NoveltySlot *slot_find(NoveltyId id) {
    size_t i;
    if (!g_cap) return NULL;
    for (i = (size_t)id.lo & (g_cap - 1);; i = (i + 1) & (g_cap - 1)) {
        if (!g_slots[i].flags) return &g_slots[i];
        if (id_eq(g_slots[i].id, id)) return &g_slots[i];
    }
}

static void set_grow(void) {
    NoveltySlot *old = g_slots;
    size_t ocap = g_cap, i;
    g_cap = g_cap ? g_cap * 2 : 1024;
    g_slots = reclaim_calloc(g_cap, sizeof *g_slots);
    CHECK(g_slots, "novelty: OOM growing the identity set — a dropped set pays every known emission again");
    for (i = 0; i < ocap; i++) if (old[i].flags) *slot_find(old[i].id) = old[i];
    free(old);
}

/* The slot for `id`, claimed if it was empty. */
static NoveltySlot *slot_claim(NoveltyId id, NoveltyKind kind) {
    NoveltySlot *s;
    if ((g_used + 1) * 2 > g_cap) set_grow();
    s = slot_find(id);
    if (!s->flags) { s->id = id; s->kind = (uint8_t)kind; g_used++; }
    /* One digest is one spelling, and the kind is its second field, so a digest filed under two kinds is two
       spellings colliding in 128 bits or a set line whose kind token was not the one it was digested under. */
    DCHECK(s->kind == (uint8_t)kind, "one identity digest was filed under two emission kinds");
    return s;
}

void novelty_set_scope(const char *scope) {
    DCHECK(scope && *scope, "an instance was given no novelty scope — every identity is qualified by its origin");
    DCHECK(!g_scope || !strcmp(g_scope, scope),
           "an instance's novelty scope changed within one agent — every identity filed so far names the old one");
    if (g_scope) return;
    g_scope = reclaim_strdup(scope);
    CHECK(g_scope, "novelty: OOM copying the scope");
}

static void hash_field(SecureHash *h, const char *s) {
    char len[24];
    size_t n = strlen(s);
    int w = snprintf(len, sizeof len, "%zu:", n);
    CHECK(w > 0 && (size_t)w < sizeof len, "novelty: a field length did not fit its prefix");
    secure_hash_update(h, (const uint8_t *)len, (size_t)w);
    secure_hash_update(h, (const uint8_t *)s, n);
}

NoveltyId novelty_id(NoveltyKind kind, const char *const *fields, int n) {
    SecureHash h;
    uint8_t d[32];
    NoveltyId id = { 0, 0 };
    DCHECK(kind >= 0 && kind < NOVELTY_KIND_N, "an identity was asked for an emission kind novelty.h does not list");
    DCHECK(g_scope != NULL, "an identity was asked before the instance's scope was set — main.c's qjs_init and "
                            "each fixture host set it beside endpoint_init");
    secure_hash_init(&h, SECURE_HASH_SHA256);
    hash_field(&h, NOVELTY_SPELLING);
    hash_field(&h, novelty_kind_token[kind]);
    hash_field(&h, g_scope);
    for (int i = 0; i < n; i++) {
        DCHECK(fields[i] != NULL, "an identity field is NULL — a kind's fields are its table's, all present");
        hash_field(&h, fields[i]);
    }
    secure_hash_finish(&h, d, sizeof d);
    for (int i = 0; i < 8; i++) { id.hi = id.hi << 8 | d[i]; id.lo = id.lo << 8 | d[8 + i]; }
    return id;
}

int novelty_note_at(NoveltyKind kind, NoveltyId id, const char *file, int line) {
    NoveltySlot *s = slot_claim(id, kind);
    DCHECKF(!(s->flags & SLOT_OWN),
            "%s:%d filed an identity this instance already emitted — the caller's own table answered \"new\" for "
            "it, so that table's identity and novelty_id's fields have parted", file, line);
    s->flags |= SLOT_OWN;
    g_own_n++;
    if (s->flags & SLOT_KNOWN) g_own_known_n++;
    return !(s->flags & SLOT_KNOWN);
}

void novelty_credit(NoveltyKind kind, int novel, int razor) {
    DCHECK(kind >= 0 && kind < NOVELTY_KIND_N && !novelty_kind_counted[kind],
           "a credit was asked for a kind novelty.h does not list as a credit kind — a counted kind is read, not paid");
    if (!novel) { g_unpaid[kind]++; return; }
    g_paid[kind]++;
    {
        /* One credit event seen from both ledgers: the flow ledger's paid value moves by exactly what this site
           counts as landed. */
        double before = flow_credit_paid_value();
        int landed = flow_credit_emit(1.0);
        DCHECKF(flow_credit_paid_value() - before == (landed ? 1.0 : 0.0),
                "a novel credit moved the flow ledger's paid value by %g while flow_credit_emit answered %d — the "
                "Level-1 value and the ledger it is read beside counted one event two ways",
                flow_credit_paid_value() - before, landed);
        if (landed) g_landed += razor ? 1.0 : NOVELTY_STATABLE_WEIGHT;
    }
}

unsigned novelty_drive_note(NoveltyId id, int64_t visits) {
    NoveltySlot *s = slot_claim(id, NOVELTY_DRIVE);
    /* Read before this drive is counted: the product's drives (handed less this instance's own) and this
       instance's earlier ones are disjoint, so their sum counts each drive once. */
    unsigned seen = s->known_n + s->own_n;
    if (!(s->flags & SLOT_OWN)) {
        s->flags |= SLOT_OWN;
        g_own_n++;
        if (s->flags & SLOT_KNOWN) g_own_known_n++;
    }
    s->own_n++;
    if (g_drives_n >= g_drives_cap) {
        g_drives_cap = g_drives_cap ? g_drives_cap * 2 : 16;
        g_drives = realloc(g_drives, (size_t)g_drives_cap * sizeof *g_drives);
        CHECK(g_drives, "novelty: OOM recording a drive");
    }
    g_drives[g_drives_n++] = (NoveltyDrive){ id, seen, visits };
    if (seen) g_drives_lowered++;
    return seen;
}
double novelty_landed_value(void) { return g_landed; }

static int hex_digit(int c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* One set line, `<token> <32 hex>`, of `n` bytes; asserted well-formed, since the zone stores only lines this
   file emitted. */
static void learn_line(const char *p, size_t n) {
    NoveltyId id = { 0, 0 };
    NoveltySlot *s;
    int kind = -1;
    unsigned long count = 0;
    for (int k = 0; k < NOVELTY_KIND_N; k++) {
        size_t t = strlen(novelty_kind_token[k]);
        if (n >= t + 33 && !strncmp(p, novelty_kind_token[k], t) && p[t] == ' ') { kind = k; break; }
    }
    if (kind >= 0) {
        size_t t = strlen(novelty_kind_token[kind]);
        if (novelty_kind_counted[kind]) {
            /* A counted line is `<token> <32 hex> <count>`, the count a positive decimal. */
            char *end = NULL;
            int ok = n > t + 34 && p[t + 33] == ' ' && p[t + 34] >= '1' && p[t + 34] <= '9';
            if (ok) count = strtoul(p + t + 34, &end, 10);
            ok = ok && end == p + n && count <= UINT32_MAX;
            DCHECKF(ok, "a counted known-identity line carries no count after its digest: [%.*s]", (int)n, p);
            if (!ok) return;
        } else if (n != t + 33) {
            kind = -1;
        }
        if (kind >= 0) p += t + 1;
    }
    DCHECKF(kind >= 0, "a known-identity line names no emission kind novelty.h lists, or is not 32 hex digits: "
                       "[%.*s]", (int)n, p);
    if (kind < 0) return;
    for (int i = 0; i < 32; i++) {
        int v = hex_digit((unsigned char)p[i]);
        DCHECK(v >= 0, "a known-identity line's digest is not lowercase hex — novelty_json writes it that way");
        if (v < 0) return;
        if (i < 16) id.hi = id.hi << 4 | (uint64_t)v; else id.lo = id.lo << 4 | (uint64_t)v;
    }
    s = slot_claim(id, (NoveltyKind)kind);
    if (count > s->known_n) s->known_n = (uint32_t)count;
    if (s->flags & SLOT_KNOWN) return;
    s->flags |= SLOT_KNOWN;
    g_known_n++;
    if (s->flags & SLOT_OWN) g_own_known_n++;
}

void novelty_learn(const char *text) {
    const char *p = text, *nl;
    size_t sp = strlen(NOVELTY_SPELLING);
    DCHECK(text != NULL, "the zone handed no known-identity text");
    /* The spelling line is the zone's record of which build emitted these lines. A set under another spelling is
       a persisted set from another build: its digests match nothing, so it is refused whole, and counted. */
    if (strncmp(p, NOVELTY_SPELLING, sp) || p[sp] != '\n') { g_refused_texts++; return; }
    for (p += sp + 1; *p; p = nl + 1) {
        nl = strchr(p, '\n');
        DCHECK(nl != NULL, "a known-identity text does not end its last line — each line is newline-terminated");
        if (!nl) return;
        learn_line(p, (size_t)(nl - p));
    }
}

/* A count, as a JSON number. */
static void json_long(JsonBuf *b, long v) {
    char num[24];
    int w = snprintf(num, sizeof num, "%ld", v);
    CHECK(w > 0 && (size_t)w < sizeof num, "novelty: a census figure did not fit its buffer");
    json_buf_raw(b, num);
}

/* A weighted value, exact: %.17g round-trips a double, so the zone reads the number the account states. */
static void json_value(JsonBuf *b, double v) {
    char num[32];
    int w = snprintf(num, sizeof num, "%.17g", v);
    CHECK(w > 0 && (size_t)w < sizeof num, "novelty: a value did not fit its buffer");
    json_buf_raw(b, num);
}

char *novelty_json(void) {
    JsonBuf b = { 0 };
    long asks = 0, paid = 0, wrote = 0;
    json_buf_raw(&b, "{"); json_buf_key(&b, "spelling"); json_buf_str(&b, NOVELTY_SPELLING);
    json_buf_raw(&b, ","); json_buf_key(&b, "scope");
    if (g_scope) json_buf_str(&b, g_scope); else json_buf_raw(&b, "null");
    json_buf_raw(&b, ","); json_buf_key(&b, "kinds"); json_buf_raw(&b, "[");
    for (int k = 0; k < NOVELTY_KIND_N; k++) {
        if (novelty_kind_counted[k]) continue;
        if (b.n && b.b[b.n - 1] != '[') json_buf_raw(&b, ",");
        json_buf_raw(&b, "{"); json_buf_key(&b, "kind"); json_buf_str(&b, novelty_kind_token[k]);
        json_buf_raw(&b, ","); json_buf_key(&b, "asks"); json_long(&b, g_paid[k] + g_unpaid[k]);
        json_buf_raw(&b, ","); json_buf_key(&b, "paid"); json_long(&b, g_paid[k]);
        json_buf_raw(&b, ","); json_buf_key(&b, "knownUnpaid"); json_long(&b, g_unpaid[k]);
        json_buf_raw(&b, "}");
        asks += g_paid[k] + g_unpaid[k];
        paid += g_paid[k];
    }
    json_buf_raw(&b, "],"); json_buf_key(&b, "asks"); json_long(&b, asks);
    json_buf_raw(&b, ","); json_buf_key(&b, "paid"); json_long(&b, paid);
    json_buf_raw(&b, ","); json_buf_key(&b, "landed"); json_value(&b, g_landed);
    json_buf_raw(&b, ","); json_buf_key(&b, "known"); json_long(&b, g_known_n);
    json_buf_raw(&b, ","); json_buf_key(&b, "own"); json_long(&b, g_own_n);
    json_buf_raw(&b, ","); json_buf_key(&b, "ownKnown"); json_long(&b, g_own_known_n);
    json_buf_raw(&b, ","); json_buf_key(&b, "refusedTexts"); json_long(&b, g_refused_texts);
    /* Every identity this instance filed, known ones included: the zone's union is idempotent, and a known line
       is what says this instance reached it too. */
    json_buf_raw(&b, ","); json_buf_key(&b, "identities"); json_buf_raw(&b, "[");
    for (size_t i = 0; i < g_cap; i++) {
        char line[64];
        int w;
        if (!(g_slots[i].flags & SLOT_OWN)) continue;
        w = novelty_kind_counted[g_slots[i].kind]
            ? snprintf(line, sizeof line, "%s %016llx%016llx %u", novelty_kind_token[g_slots[i].kind],
                       (unsigned long long)g_slots[i].id.hi, (unsigned long long)g_slots[i].id.lo, g_slots[i].own_n)
            : snprintf(line, sizeof line, "%s %016llx%016llx", novelty_kind_token[g_slots[i].kind],
                       (unsigned long long)g_slots[i].id.hi, (unsigned long long)g_slots[i].id.lo);
        CHECK(w > 0 && (size_t)w < sizeof line, "novelty: an identity line did not fit its buffer");
        if (wrote++) json_buf_raw(&b, ",");
        json_buf_str(&b, line);
    }
    DCHECK(wrote == g_own_n, "the identity set's own flags do not number the identities this instance filed");
    json_buf_raw(&b, "],"); json_buf_key(&b, "drives"); json_buf_raw(&b, "{");
    json_buf_key(&b, "seeded"); json_long(&b, g_drives_n);
    json_buf_raw(&b, ","); json_buf_key(&b, "lowered"); json_long(&b, g_drives_lowered);
    json_buf_raw(&b, ","); json_buf_key(&b, "list"); json_buf_raw(&b, "[");
    for (long i = 0; i < g_drives_n; i++) {
        char line[64];
        int w = snprintf(line, sizeof line, "drive %016llx%016llx", (unsigned long long)g_drives[i].id.hi,
                         (unsigned long long)g_drives[i].id.lo);
        CHECK(w > 0 && (size_t)w < sizeof line, "novelty: a drive line did not fit its buffer");
        if (i) json_buf_raw(&b, ",");
        json_buf_raw(&b, "{"); json_buf_key(&b, "identity"); json_buf_str(&b, line);
        json_buf_raw(&b, ","); json_buf_key(&b, "seen"); json_long(&b, (long)g_drives[i].seen);
        json_buf_raw(&b, ","); json_buf_key(&b, "visits"); json_long(&b, (long)g_drives[i].visits);
        json_buf_raw(&b, "}");
    }
    json_buf_raw(&b, "]}}");
    return json_buf_take(&b);
}

void novelty_free(void) {
    free(g_slots); g_slots = NULL; g_cap = g_used = 0;
    g_known_n = g_own_n = g_own_known_n = g_refused_texts = 0;
    free(g_scope); g_scope = NULL;
    memset(g_paid, 0, sizeof g_paid);
    memset(g_unpaid, 0, sizeof g_unpaid);
    free(g_drives); g_drives = NULL; g_drives_n = g_drives_cap = g_drives_lowered = 0;
    g_landed = 0.0;
}
