/* Time-travel points before a source read (see source_cut.h). */
#include "solver/source_cut.h"
#include "solver/engine.h"    /* engine_referenced: a peer may route into this document */
#include "solver/reclaim.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>

/* `standby` is the held member, NULL once shed; `srcs` the sources it stands before, owned. A cut with a standby is
   on the live list, which the shedding walks; a cut is freed when it has neither a standby nor a reference. */
struct SourceCut {
    Flow *standby;
    int refs;
    char **srcs;
    int n;
    struct SourceCut *prev, *next;
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
    /* Named residual. Not covered: a document a peer references takes no cut, because a record or operation a
       peer routes here after the snapshot reaches only the members live when it arrives (engine_route skips a
       held member), so an arm forked off the standby later would never receive it. Next diff builds: what a peer
       routes while a standby is held is kept with its cut and given to each arm forked off it. Absence shows as:
       `_sourceCuts.taken` zero, and `cutRefused: "no-cut"` on every search, in a document a peer references that
       reads a declared source. */
    if (engine_referenced() || flow_deliver_pending(f) || flow_perform_pending(f) || flow_owes_answer(f)) return 0;
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
}

void source_cut_fork(Flow *sib, const Flow *parent) {
    DCHECK(sib->src_read_n == 0 && sib->cuts_n == 0 && sib->cut_due_n == 0 && sib->standby_of == NULL,
           "a fork's arm already carried time-travel state before inheriting its parent's");
    for (int i = 0; i < parent->src_read_n; i++) strv_push(&sib->src_read, &sib->src_read_n, parent->src_read[i]);
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

void source_cut_flow_release(Flow *f) {
    strv_free(&f->src_read, &f->src_read_n);
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
