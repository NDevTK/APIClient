/* Time-travel points before a source read (see source_cut.h). */
#include "solver/source_cut.h"
#include "solver/decide.h"    /* decide_cursor: where a delivery stands on the path */
#include "solver/reclaim.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>

/* `standby` is the held member, NULL once shed; `srcs` the sources it stands before, owned. A cut with a standby is
   on the live list, which the shedding walks; a cut is freed when it has neither a standby nor a reference. */
struct SourceCut {
    Flow *standby;
    int refs;
    int served;   /* a search has held it (source_cut_hold_search) */
    char **srcs;
    int n;
    struct SourceCut *prev, *next;
};

/* A delivery log: records in delivery order, each with the decision cursor it was delivered at. */
struct SourceCutLog {
    char **rec;
    int *cur;
    int n;
};

static SourceCut *g_live;
/* How many cuts on the live list nothing refers to: the pick asks for one every iteration, so the walk is paid only
   when there is one. */
static int g_dead_n;
static SourceCutCensus g_census;

static int strv_has(char *const *v, int n, const char *s) {
    for (int i = 0; i < n; i++)
        if (!strcmp(v[i], s)) return 1;
    return 0;
}

static void strv_push(char ***v, int *n, const char *s) {
    char **grown = reclaim_realloc(*v, (size_t)(*n + 1) * sizeof **v);

    CHECK(grown != NULL, "source_cut: OOM recording a source");
    *v = grown;
    (*v)[*n] = reclaim_strdup(s);
    CHECK((*v)[*n] != NULL, "source_cut: OOM recording a source");
    (*n)++;
}

static void strv_free(char ***v, int *n) {
    for (int i = 0; i < *n; i++) free((*v)[i]);
    free(*v);
    *v = NULL;
    *n = 0;
}

static struct SourceCutLog *log_new(void) {
    struct SourceCutLog *l = reclaim_calloc(1, sizeof *l);

    CHECK(l != NULL, "source_cut: OOM making a delivery log");
    return l;
}

static void log_push(struct SourceCutLog *l, const char *rec, int cur) {
    char **r = reclaim_realloc(l->rec, (size_t)(l->n + 1) * sizeof *r);
    int *c;

    CHECK(r != NULL, "source_cut: OOM growing a delivery log");
    l->rec = r;
    c = reclaim_realloc(l->cur, (size_t)(l->n + 1) * sizeof *c);
    CHECK(c != NULL, "source_cut: OOM growing a delivery log");
    l->cur = c;
    l->rec[l->n] = reclaim_strdup(rec);
    CHECK(l->rec[l->n] != NULL, "source_cut: OOM recording a delivery");
    l->cur[l->n] = cur;
    l->n++;
}

static struct SourceCutLog *log_copy(const struct SourceCutLog *src, int from) {
    struct SourceCutLog *l = log_new();

    for (int i = from; src && i < src->n; i++) log_push(l, src->rec[i], src->cur[i]);
    return l;
}

void source_cut_log_free(struct SourceCutLog *l) {
    if (!l) return;
    for (int i = 0; i < l->n; i++) free(l->rec[i]);
    free(l->rec);
    free(l->cur);
    free(l);
}

void source_cut_note_delivery(const char *record) {
    Flow *f = flow_running();

    DCHECK(f != NULL && record != NULL && *record, "a routed delivery was noted with no flow running or no record");
    if (!f->deliv_log) f->deliv_log = log_new();
    log_push(f->deliv_log, record, decide_cursor());
    /* A held record is delivered once: its entry is spent. */
    for (int i = 0; f->hold_on && f->hold && i < f->hold->n; i++)
        if (!strcmp(f->hold->rec[i], record)) {
            free(f->hold->rec[i]);
            f->hold->rec[i] = f->hold->rec[f->hold->n - 1];
            f->hold->cur[i] = f->hold->cur[f->hold->n - 1];
            f->hold->n--;
            g_census.holds_met++;
            break;
        }
}

struct SourceCutLog *source_cut_log_freeze(const Flow *f) {
    return log_copy(f->deliv_log, 0);
}

/* Named residual. Not covered: an arm written to the cold tier and resumed has no hold (cold.c writes none), so it
   delivers its records at its first task boundary. Next diff builds: the hold written beside the candidate's own
   record and rebuilt at the resume. Absence shows as: a cold-resumed candidate in a document that received a
   routed record after its cut leaving the path it replays. */
void source_cut_hold_install(Flow *arm, const Flow *standby, const struct SourceCutLog *log, int path_len) {
    int had = standby->deliv_log ? standby->deliv_log->n : 0;

    DCHECK(arm->hold == NULL && !arm->hold_on, "an arm's delivery hold was installed twice");
    DCHECK(log == NULL || log->n >= had,
           "a frozen path's delivery log is shorter than the snapshot's own history — the path extends the "
           "snapshot's history, so its log extends the snapshot's");
    for (int i = 0; log && i < had; i++)
        DCHECK(!strcmp(log->rec[i], standby->deliv_log->rec[i]) && log->cur[i] == standby->deliv_log->cur[i],
               "a frozen path's delivery log disagrees with the snapshot's history below the snapshot");
    arm->hold = log_copy(log, had);
    arm->hold_end = path_len;
    arm->hold_on = 1;
}

int source_cut_delivery_ready(const Flow *f, const char *record) {
    int at;

    DCHECK(f == flow_running(), "a delivery's readiness was asked of a flow that is not running — the cursor "
                                "read is the running flow's");
    if (!f->hold_on) return 1;
    at = decide_cursor();
    for (int i = 0; i < f->hold->n; i++)
        if (!strcmp(f->hold->rec[i], record)) return at >= f->hold->cur[i];
    return at >= f->hold_end;
}

void source_read_note(const char *src) {
    Flow *f = flow_running();

    if (!f || !src || source_read_has(f, src)) return;
    strv_push(&f->src_read, &f->src_read_n, src);
}

int source_read_has(const Flow *f, const char *src) {
    return strv_has(f->src_read, f->src_read_n, src);
}

static int cut_covers(const SourceCut *c, const char *src) { return strv_has(c->srcs, c->n, src); }

SourceCut *source_cut_of(const Flow *f, const char *src) {
    for (int i = f->cuts_n - 1; i >= 0; i--)
        if (cut_covers(f->cuts[i], src)) return f->cuts[i];
    return NULL;
}

int source_cut_ask(const char *const *srcs, int n) {
    Flow *f = flow_running();

    DCHECK(n > 0 && srcs != NULL, "a read asked for a cut before no source");
    /* Only a member that can open a search wants a snapshot of its own history: a candidate delivers its source, a
       drive's searches stand on a chain of drives the cut cannot seat (solve.c's residual), and a standby never
       runs. A peer's operation or message held by this flow would be held by the standby too, which never answers
       or delivers it, so such a flow reads without a cut. */
    if (!f || f->cand_src || f->orphan || f->standby_of) return 0;
    /* A flow running another agent too resumes whichever agent the scheduler selects first, so a park for the
       window agent's read is not the next thing it does: it reads without a cut. */
    if (f->n_agents > 1) return 0;
    if (f->cut_due) {
        /* The machine was re-entered without the park this ask asked for: no snapshot was taken at this read, so
           none precedes it, and the lineage reads without one. */
        strv_free(&f->cut_due, &f->cut_due_n);
        g_census.unparked++;
        return 0;
    }
    /* A document a peer references takes cuts too: what the peer routes while a standby is held waits on the
       standby's queues for the arms forked off it (engine_route, engine_perform). */
    if (flow_deliver_pending(f) || flow_perform_pending(f) || flow_owes_answer(f)) return 0;
    for (int i = 0; i < n; i++)
        if (!source_read_has(f, srcs[i]) && !source_cut_of(f, srcs[i]) && !strv_has(f->cut_due, f->cut_due_n, srcs[i]))
            strv_push(&f->cut_due, &f->cut_due_n, srcs[i]);
    return f->cut_due_n > 0;
}

int source_cut_due(const Flow *f) { return f->cut_due_n > 0; }

void source_cut_leave(Flow *f, Flow *standby) {
    SourceCut *c;

    DCHECK(f != NULL && standby != NULL && f->cut_due_n > 0 && standby->standby_of == NULL,
           "a cut was made for a flow with no read parked on one, or over a member that is already a standby");
    c = reclaim_calloc(1, sizeof *c);
    CHECK(c != NULL, "source_cut: OOM making a cut");
    c->standby = standby;
    c->srcs = f->cut_due;
    c->n = f->cut_due_n;
    f->cut_due = NULL;
    f->cut_due_n = 0;
    c->next = g_live;
    if (g_live) g_live->prev = c;
    g_live = c;
    standby->standby_of = c;
    /* The standby opens no search, so it holds no reference to the lineage's earlier cuts: one would keep each of
       them alive for as long as this one is. */
    for (int i = 0; i < standby->cuts_n; i++) source_cut_drop(standby->cuts[i]);
    free(standby->cuts);
    standby->cuts = NULL;
    standby->cuts_n = 0;
    flow_set_held(standby);
    /* The parent's reference; it performs the read once it resumes. */
    {
        SourceCut **grown = reclaim_realloc(f->cuts, (size_t)(f->cuts_n + 1) * sizeof *grown);

        CHECK(grown != NULL, "source_cut: OOM giving a lineage its cut");
        f->cuts = grown;
        f->cuts[f->cuts_n++] = c;
        c->refs = 1;
    }
    g_census.taken++;
    if (g_census.taken - g_census.shed > g_census.held_peak) g_census.held_peak = g_census.taken - g_census.shed;
}

void source_cut_fork(Flow *sib, const Flow *parent) {
    DCHECK(sib->src_read_n == 0 && sib->cuts_n == 0 && sib->cut_due_n == 0 && sib->standby_of == NULL,
           "a fork's arm already carried time-travel state before inheriting its parent's");
    for (int i = 0; i < parent->src_read_n; i++) strv_push(&sib->src_read, &sib->src_read_n, parent->src_read[i]);
    DCHECK(sib->deliv_log == NULL && sib->hold == NULL, "a fork's arm already carried a delivery log");
    if (parent->deliv_log) sib->deliv_log = log_copy(parent->deliv_log, 0);
    /* An arm of an arm replays the same frozen path, so it waits for the same deliveries. */
    if (parent->hold_on) {
        sib->hold = log_copy(parent->hold, 0);
        sib->hold_end = parent->hold_end;
        sib->hold_on = 1;
    }
    if (parent->cand_src || parent->standby_of || parent->cuts_n == 0) return;
    sib->cuts = reclaim_malloc((size_t)parent->cuts_n * sizeof *sib->cuts);
    CHECK(sib->cuts != NULL, "source_cut: OOM giving an arm its lineage's cuts");
    for (int i = 0; i < parent->cuts_n; i++) {
        sib->cuts[i] = parent->cuts[i];
        source_cut_hold(sib->cuts[i]);
    }
    sib->cuts_n = parent->cuts_n;
}

static void cut_unlink(SourceCut *c) {
    if (c->prev) c->prev->next = c->next;
    else g_live = c->next;
    if (c->next) c->next->prev = c->prev;
    c->prev = c->next = NULL;
}

static void cut_free(SourceCut *c) {
    DCHECK(c->refs == 0 && c->standby == NULL, "a cut was freed while a member or search still referred to it");
    strv_free(&c->srcs, &c->n);
    free(c);
}

void source_cut_flow_cold(Flow *f) {
    DCHECK(f->standby_of == NULL, "a held snapshot was sold — a standby is shed, never sold");
    strv_free(&f->cut_due, &f->cut_due_n);
    for (int i = 0; i < f->cuts_n; i++) source_cut_drop(f->cuts[i]);
    free(f->cuts);
    f->cuts = NULL;
    f->cuts_n = 0;
}

void source_cut_flow_release(Flow *f) {
    strv_free(&f->src_read, &f->src_read_n);
    source_cut_log_free(f->deliv_log);
    f->deliv_log = NULL;
    source_cut_log_free(f->hold);
    f->hold = NULL;
    f->hold_on = 0;
    strv_free(&f->cut_due, &f->cut_due_n);
    for (int i = 0; i < f->cuts_n; i++) source_cut_drop(f->cuts[i]);
    free(f->cuts);
    f->cuts = NULL;
    f->cuts_n = 0;
    if (f->standby_of) {
        SourceCut *c = f->standby_of;

        DCHECK(c->standby == f, "a standby was released from a cut that names another member");
        f->standby_of = NULL;
        c->standby = NULL;
        cut_unlink(c);
        g_census.shed++;
        if (c->refs == 0) {
            g_dead_n--;
            cut_free(c);
        }
    }
}

void source_cut_hold(SourceCut *c) {
    DCHECK(c != NULL && c->refs > 0, "a cut was held that nothing refers to — it may already have been shed");
    c->refs++;
}

void source_cut_hold_search(SourceCut *c) {
    source_cut_hold(c);
    if (!c->served) g_census.served++;
    c->served = 1;
}

void source_cut_drop(SourceCut *c) {
    DCHECK(c != NULL && c->refs > 0, "a cut was dropped more times than it was held");
    if (--c->refs > 0) return;
    if (c->standby == NULL) cut_free(c);
    else g_dead_n++;
}

Flow *source_cut_standby(const SourceCut *c) { return c->standby; }

Flow *source_cut_next_shed(int all, const Flow *keep) {
    DCHECK(g_dead_n >= 0, "more cuts were counted as unreferenced than were ever dropped to zero");
    if (!all && g_dead_n == 0) return NULL;
    for (SourceCut *c = g_live; c; c = c->next)
        if ((all || c->refs == 0) && c->standby != keep) return c->standby;
    return NULL;
}

void source_cut_count_arm(void) { g_census.arms++; }

SourceCutCensus source_cut_census(void) { return g_census; }
