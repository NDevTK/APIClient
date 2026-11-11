/* The pick ledger (solver/pick_ledger.h). */
#include "solver/pick_ledger.h"
#include "solver/novelty.h"
#include "check.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How many frontier members a pick's ranks are read against: a uniform sample with replacement, so a row costs
   O(this) however large the frontier is. A rank is then an estimate of the member's place in the frontier with a
   standard error of about 1/(2*sqrt(PICK_LEDGER_SAMPLE)) at the median. */
#define PICK_LEDGER_SAMPLE 32

/* The terms a row ranks, each read off FlowPickTerms; a rank is "gt:eq", how many sampled indexed members read
   strictly more and exactly the same. */
#define PICK_LEDGER_RANKED(X)                                    \
    X(w,          t->w)                                          \
    X(reward,     t->reward)                                     \
    X(famNotch,   (double)t->fam_notch)                          \
    X(x,          t->x)                                          \
    X(served,     (double)t->served)                             \
    X(ikey,       t->ikey)                                       \
    X(turn,       (double)t->turn)                               \
    X(priced,     (double)(t->assumed + t->against_env))         \
    X(seq,        (double)t->seq)                                \
    X(ownSilence, (double)t->own_silence)
enum {
#define PICK_LEDGER_RANK_ENUM(name, expr) PICK_RANK_##name,
    PICK_LEDGER_RANKED(PICK_LEDGER_RANK_ENUM)
#undef PICK_LEDGER_RANK_ENUM
    PICK_RANK_N
};

static double pick_rank_value(const FlowPickTerms *t, int which) {
    switch (which) {
#define PICK_LEDGER_RANK_CASE(name, expr) case PICK_RANK_##name: return (expr);
    PICK_LEDGER_RANKED(PICK_LEDGER_RANK_CASE)
#undef PICK_LEDGER_RANK_CASE
    default: break;
    }
    DFAIL("the pick ledger ranked a term PICK_LEDGER_RANKED does not list");
    return 0.0;
}

static int g_armed = -1;
/* The open tenure: the picked member's terms and the instance's counters at its dispatch. */
static struct {
    int open, displaced, members, sampled, eligible;
    int64_t work;
    long sold;
    FlowPickTerms t;
    int gt[PICK_RANK_N], eq[PICK_RANK_N];
    NoveltyCreditLife life[NOVELTY_KIND_N];
    long sites;
} g_row;
static int64_t g_rows;
/* splitmix64's state: the ledger's own generator, so the sample draws from no stream the engine reads. */
static uint64_t g_rng = 0x9e3779b97f4a7c15ull;

/* Code-site identities first served anywhere in the instance: an open-addressed set of (kind, key) digests at
   load at most one half, and its lifetime count. Plain malloc, since a refusal through the reclaim edge would sell
   a flow. */
static uint64_t *g_sites;
static size_t g_sites_cap, g_sites_n;

static uint64_t pick_mix(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

int pick_ledger_armed(void) {
    if (g_armed < 0) {
#if defined(__EMSCRIPTEN__)
        g_armed = 0;
#else
        const char *v = getenv("APICLIENT_PICK_LEDGER");
        g_armed = v != NULL && strcmp(v, "1") == 0;
#endif
    }
    return g_armed;
}

static int pick_sites_insert(uint64_t id) {
    size_t i;

    if (2 * (g_sites_n + 1) > g_sites_cap) {
        size_t cap = g_sites_cap ? 2 * g_sites_cap : 1024;
        uint64_t *t = calloc(cap, sizeof *t);

        CHECK(t != NULL, "pick ledger: OOM growing the set of served code sites");
        for (size_t j = 0; j < g_sites_cap; j++) {
            if (g_sites[j] == 0) continue;
            for (i = g_sites[j] & (cap - 1); t[i] != 0; i = (i + 1) & (cap - 1)) {}
            t[i] = g_sites[j];
        }
        free(g_sites);
        g_sites = t; g_sites_cap = cap;
    }
    for (i = id & (g_sites_cap - 1); g_sites[i] != 0; i = (i + 1) & (g_sites_cap - 1))
        if (g_sites[i] == id) return 0;
    g_sites[i] = id;
    g_sites_n++;
    return 1;
}

void pick_ledger_site_served(int kind, uint64_t key) {
    uint64_t id;

    if (!pick_ledger_armed()) return;
    /* Zero marks an empty slot, so the one identity that digests to zero is moved off it. */
    id = pick_mix(key ^ ((uint64_t)(unsigned)kind << 56) ^ 0x2545f4914f6cdd1dull);
    (void)pick_sites_insert(id ? id : 1);
}

/* One row's text is composed into this buffer and written by one call, so a line is never split by another
   writer of the stream. */
typedef struct { char b[4096]; size_t n; } PickLine;

static void pick_put(PickLine *l, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void pick_put(PickLine *l, const char *fmt, ...) {
    va_list ap;
    int k;

    va_start(ap, fmt);
    k = vsnprintf(l->b + l->n, sizeof l->b - l->n, fmt, ap);
    va_end(ap);
    CHECK(k >= 0 && (size_t)k < sizeof l->b - l->n, "pick ledger: a row outgrew its line buffer");
    l->n += (size_t)k;
}

static void pick_header(void) {
    PickLine l = { .n = 0 };

    pick_put(&l, "@PICKCOLS row\twork\tsold\tmembers\tsampled\teligible\tdisplaced\tseq\tfamily\tsiteKind\tsiteKey"
                 "\tcls\tonLedger\tindexed\tw\tcoord\treward\tfamNotch\tx\tserved\tseen\tassumed\tagainstEnv\tikey"
                 "\treplay\tsurv\trung\tturn\townSilence\tvisits\tpicks\tobsX");
#define PICK_LEDGER_RANK_COL(name, expr) pick_put(&l, "\tr_%s", #name);
    PICK_LEDGER_RANKED(PICK_LEDGER_RANK_COL)
#undef PICK_LEDGER_RANK_COL
    pick_put(&l, "\tendWork\tendSold\tendBy\tnewSites");
    for (int k = 0; k < NOVELTY_KIND_N; k++) {
        if (novelty_kind_is_counted((NoveltyKind)k)) continue;
        pick_put(&l, "\tpaid_%s\tlanded_%s", novelty_kind_name((NoveltyKind)k), novelty_kind_name((NoveltyKind)k));
    }
    pick_put(&l, "\n");
    fputs(l.b, stdout);
}

/* Writes the open tenure's row, closed at `work`/`sold` by `by` ('p' the next pick, 'c' the session's close). */
static void pick_emit(int64_t work, long sold, char by) {
    const FlowPickTerms *t = &g_row.t;
    PickLine l = { .n = 0 };

    DCHECK(g_row.open && work >= g_row.work && sold >= g_row.sold && (long)g_sites_n >= g_row.sites,
           "a pick-ledger tenure was closed before it opened, or one of its lifetime counters fell");
    if (g_rows == 1) pick_header();
    pick_put(&l, "@PICK %lld\t%lld\t%ld\t%d\t%d\t%d\t%d\t%llu\t%llu\t%d\t%016llx\t%u\t%d\t%d", (long long)g_rows,
             (long long)g_row.work, g_row.sold, g_row.members, g_row.sampled, g_row.eligible, g_row.displaced,
             (unsigned long long)t->seq, (unsigned long long)t->family, t->site_kind,
             (unsigned long long)t->site_key, t->cls, t->on_ledger, t->indexed);
    pick_put(&l, "\t%.17g\t%.17g\t%.17g\t%lld\t%.17g\t%lld\t%lld\t%d\t%d\t%.17g\t%.9g\t%.9g\t%d\t%lld\t%lld\t%lld\t%lld",
             t->w, t->coord, t->reward, (long long)t->fam_notch, t->x, (long long)t->served, (long long)t->seen,
             t->assumed, t->against_env, t->ikey, t->replay, t->surv, t->rung, (long long)t->turn,
             (long long)t->own_silence, (long long)t->visits, (long long)t->picks);
    /* The family's best indexed observed-class optimism, `-` where no such member stood. */
    if (t->obs_present) pick_put(&l, "\t%.17g", t->obs_x); else pick_put(&l, "\t-");
    for (int r = 0; r < PICK_RANK_N; r++) pick_put(&l, "\t%d:%d", g_row.gt[r], g_row.eq[r]);
    pick_put(&l, "\t%lld\t%ld\t%c\t%ld", (long long)work, sold, by, (long)g_sites_n - g_row.sites);
    for (int k = 0; k < NOVELTY_KIND_N; k++) {
        NoveltyCreditLife now;

        if (novelty_kind_is_counted((NoveltyKind)k)) continue;
        now = novelty_credit_life((NoveltyKind)k);
        DCHECK(now.paid >= g_row.life[k].paid && now.landed >= g_row.life[k].landed,
               "a lifetime novelty credit count fell across one pick-ledger tenure");
        pick_put(&l, "\t%ld\t%ld", now.paid - g_row.life[k].paid, now.landed - g_row.life[k].landed);
    }
    pick_put(&l, "\n");
    fputs(l.b, stdout);
    g_row.open = 0;
}

void pick_ledger_close(int64_t work, long sold) {
    if (!pick_ledger_armed() || !g_row.open) return;
    pick_emit(work, sold, 'c');
}

void pick_ledger_pick(const Flow *best, int displaced, int64_t work, long sold) {
    const long rank_changes = flow_rank_changes();
    int n = flow_count();
    double mine[PICK_RANK_N];

    DCHECK(pick_ledger_armed(), "the pick ledger was handed a pick while it is not armed");
    if (g_row.open) pick_emit(work, sold, 'p');
    /* One row per dispatch, so the ledger's rows and the scheduler's lifetime dispatch count move together. */
    DCHECKF(g_rows == flow_picks_lifetime(),
            "the pick ledger holds %lld row(s) for %lld dispatch(es) — a dispatch reached flow_credit_pick without "
            "passing the ledger's call beside it, or the ledger was armed after the first dispatch",
            (long long)g_rows, (long long)flow_picks_lifetime());
    g_rows++;
    memset(&g_row, 0, sizeof g_row);
    g_row.open = 1;
    g_row.displaced = displaced;
    g_row.work = work;
    g_row.sold = sold;
    g_row.members = n;
    g_row.sites = (long)g_sites_n;
    for (int k = 0; k < NOVELTY_KIND_N; k++)
        if (!novelty_kind_is_counted((NoveltyKind)k)) g_row.life[k] = novelty_credit_life((NoveltyKind)k);
    flow_pick_terms(best, &g_row.t);
    for (int r = 0; r < PICK_RANK_N; r++) mine[r] = pick_rank_value(&g_row.t, r);
    for (int s = 0; s < PICK_LEDGER_SAMPLE && n > 1; s++) {
        const Flow *m;
        FlowPickTerms mt;

        g_rng += 0x9e3779b97f4a7c15ull;
        m = flow_at((int)(pick_mix(g_rng) % (uint64_t)n));
        g_row.sampled++;
        if (m == best || !flow_in_index(m)) continue;
        g_row.eligible++;
        flow_pick_terms(m, &mt);
        for (int r = 0; r < PICK_RANK_N; r++) {
            double v = pick_rank_value(&mt, r);

            if (v > mine[r]) g_row.gt[r]++;
            else if (v == mine[r]) g_row.eq[r]++;
        }
    }
    DCHECK(flow_rank_changes() == rank_changes,
           "the pick ledger's reading of the frontier moved a rank — it must only observe");
}
