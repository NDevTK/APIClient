/* The layout job — see layout_job.h for the stack, the memo, the drive and why the interruption is a typed
   return. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/layout/layout_job.h"
#include "core/layout/layout_question.h"
#include "solver/metrics.h"
#include "solver/rest_unit.h"

typedef enum { LJ_COMPUTING = 1, LJ_SUSPENDED } LjState;

/* `opened` is whether the body has run: an entry pushed deferred has not, and the drive's first run of it
   lays out its box. `staged` is whether `cur` holds the walk the body stopped at. `ctx` is the grid row
   context the question was asked in, which its body runs in wherever a drive computes it; `outer` is the
   context the job returns to when the body answers or suspends. */
typedef struct {
    LayoutQuestion q;
    LjState        state;
    bool           opened, staged;
    unsigned       ctx, outer;
    LayoutBfCursor cur;
} LjEntry;

/* One memo slot, live when `gen` equals its job's. `rest` is the job's rest count when the answer was
   recorded, which is how a hit after a later rest is told apart. The answer is held out of line, in the job's
   answer chunks: it carries concolic lengths with their facts inline, so a slot holding it would make every
   probe and every growth of the table move kilobytes per question. */
typedef struct {
    const lxb_dom_element_t *el;
    LayoutQuestionKind       kind;
    unsigned                 code;
    unsigned                 ctx;
    uint64_t                 gen;
    uint64_t                 rest;
    LayoutBfAnswer          *a;
} LjSlot;

/* One grid row context: `container`'s rows are being sized, inside context `parent` (0 for none). A grid
   area's block size is what its rows' sizing computes and is definite once they are sized (css-grid-1 §11
   "Grid Layout Algorithm"), so an answer that read one differs between the two: the memo keys on the context,
   and each context owns its placement, line box and intrinsic size records. The cascade is the same in every
   context and stays the job's. Held by pointer, so a record handed out survives the table's growth. */
typedef struct {
    unsigned                 parent;
    const lxb_dom_element_t *container;
    FlowPlacementRecord      placement;
    LineBoxRecord            line_boxes;
    IntrinsicSizeRecord      intrinsic;
} LjContext;

/* Answers per chunk of the job's answer storage. Chunks never move, so a slot's pointer survives the table's
   growth; they are reused from the start when the memo is emptied. */
#define LJ_ANSWER_CHUNK ((size_t)64)

struct LayoutJob {
    LjEntry  *stack;
    size_t    n, cap;
    size_t    base;      /* the innermost open drive's mark; entries below it belong to drives further out */
    unsigned  nest;      /* computing LAYOUT_Q_BF_BOX entries above `base` */
    unsigned  drives;    /* drives open on this job */
    size_t    quota;     /* boxes a resting drive may still lay out in this activation */
    size_t    scan;      /* elements a resting drive's float search may still examine in this activation */
    size_t    line_items;   /* items a resting drive's line box fills may still take in this activation */
    size_t    line_metrics; /* box metrics its line box walks may still derive in this activation */
    bool      resting;   /* a resting drive is open, as the job's outermost */
    LjSlot   *memo;
    LayoutBfAnswer **answers;   /* chunks of LJ_ANSWER_CHUNK answers, owned */
    size_t    answer_chunks, answer_cap, answer_used;
    size_t    memo_cap;  /* a power of two, or zero before the first answer */
    size_t    memo_used;
    uint64_t  gen;       /* starts at 1, so a zeroed slot is never live */
    uint64_t  rests;
    const char *drive_file;   /* the innermost open drive's site */
    int         drive_line;
    bool      driven;
    LayoutJobDriver driver;
    LjContext **ctxs;    /* the grid row contexts entered since the memo was last emptied, owned */
    size_t      nctx, ctx_cap;
    unsigned    ctx;     /* the current one, an index into `ctxs` plus one; 0 is none */
    FlowPlacementRecord placement;   /* core/layout/flow_placement.h's three facts, for this job */
    CssCascadeRecord    cascade;     /* core/css/css_cascade_pass.h's cascaded values, for this job */
    LineBoxRecord       line_boxes;  /* core/layout/line_box.h's fills, for this job */
    IntrinsicSizeRecord intrinsic;   /* core/layout/intrinsic_size.h's measurements, for this job */
};

/* The job whose activation holds the thread when a driver entered one, and the thread's synchronous job.
   Both are facts about the current C activation, never about a flow: `layout_job_leave` asserts the
   entered job is empty before the thread is given back, and the synchronous job's memo is emptied when its
   outermost drive ends, so neither carries state from one flow's activation into another's. */
static LayoutJob *g_entered;
static LayoutJob  g_sync = { .gen = 1 };
static size_t     g_alive;

static LayoutJob *lj_current(void)
{
    return g_entered != NULL ? g_entered : &g_sync;
}

/* ---- the memo ------------------------------------------------------------------------------------------- */

static size_t lj_hash(const lxb_dom_element_t *el, LayoutQuestionKind kind, unsigned code, unsigned ctx,
                      size_t cap)
{
    uint64_t h = (uint64_t)(uintptr_t)el * 0x9E3779B97F4A7C15ull ^
                 ((uint64_t)ctx << 40 | (uint64_t)code << 8 | (uint64_t)kind) * 0xC2B2AE3D27D4EB4Full;

    return (size_t)(h ^ (h >> 29)) & (cap - 1);
}

static LjSlot *lj_probe(LjSlot *tab, size_t cap, uint64_t gen, LayoutQuestion q, unsigned ctx)
{
    size_t i = lj_hash(q.element, q.kind, q.code, ctx, cap);

    while (tab[i].gen == gen &&
           (tab[i].el != q.element || tab[i].kind != q.kind || tab[i].code != q.code || tab[i].ctx != ctx))
        i = (i + 1) & (cap - 1);
    return &tab[i];
}

/* The two kinds the memo holds and the stack suspends. */
static bool lj_is_bf(LayoutQuestionKind kind)
{
    return kind == LAYOUT_Q_BF_BOX || kind == LAYOUT_Q_BF_WALK;
}

static void lj_memo_grow(LayoutJob *job)
{
    size_t cap = job->memo_cap != 0 ? job->memo_cap * 2 : 64, i;
    LjSlot *tab = calloc(cap, sizeof *tab);

    CHECK(tab != NULL, "a layout job's memo could not grow");
    for (i = 0; i < job->memo_cap; i++) {
        if (job->memo[i].gen != job->gen) continue;
        *lj_probe(tab, cap, job->gen, layout_question(job->memo[i].kind, (lxb_dom_element_t *)job->memo[i].el,
                                                      NULL, job->memo[i].code), job->memo[i].ctx) = job->memo[i];
    }
    free(job->memo);
    job->memo = tab;
    job->memo_cap = cap;
}

/* Releases every context's records, and forgets the contexts when `forget`. */
static void lj_contexts_release(LayoutJob *job, bool forget)
{
    size_t i;

    for (i = 0; i < job->nctx; i++) {
        flow_placement_record_release(&job->ctxs[i]->placement);
        line_box_record_release(&job->ctxs[i]->line_boxes);
        intrinsic_size_record_release(&job->ctxs[i]->intrinsic);
        if (forget) free(job->ctxs[i]);
    }
    if (forget) job->nctx = 0;
}

/* Emptying is a generation step, so a job reused across activations pays nothing per slot. The contexts are
   forgotten only when none is current and no entry names one; otherwise their records are emptied. */
static void lj_memo_shed(LayoutJob *job)
{
    job->gen++;
    job->memo_used = 0;
    job->answer_used = 0;
    lj_contexts_release(job, job->ctx == 0 && job->n == 0);
    flow_placement_record_release(&job->placement);
    css_cascade_record_release(&job->cascade);
    line_box_record_release(&job->line_boxes);
    intrinsic_size_record_release(&job->intrinsic);
}

/* The storage for the next memoized answer, from the job's chunks; it lives until the memo is emptied. */
static LayoutBfAnswer *lj_answer_alloc(LayoutJob *job)
{
    size_t c = job->answer_used / LJ_ANSWER_CHUNK;

    if (c == job->answer_chunks) {
        if (job->answer_chunks == job->answer_cap) {
            size_t cap = job->answer_cap != 0 ? job->answer_cap * 2 : 16;
            LayoutBfAnswer **v = realloc(job->answers, cap * sizeof *v);

            CHECK(v != NULL, "a layout job's answer storage could not grow");
            job->answers = v;
            job->answer_cap = cap;
        }
        job->answers[c] = malloc(LJ_ANSWER_CHUNK * sizeof **job->answers);
        CHECK(job->answers[c] != NULL, "a layout job's answer storage could not grow");
        job->answer_chunks++;
    }
    return &job->answers[c][job->answer_used++ % LJ_ANSWER_CHUNK];
}

static void lj_answers_free(LayoutJob *job)
{
    size_t i;

    for (i = 0; i < job->answer_chunks; i++) free(job->answers[i]);
    free(job->answers);
    job->answers = NULL;
    job->answer_chunks = job->answer_cap = job->answer_used = 0;
}

/* ---- the stack ------------------------------------------------------------------------------------------ */

static size_t lj_push(LayoutJob *job, LayoutQuestion q, LjState state)
{
    if (job->n == job->cap) {
        size_t cap = job->cap != 0 ? job->cap * 2 : 64;
        LjEntry *v = realloc(job->stack, cap * sizeof *v);

        CHECK(v != NULL, "a layout job's question stack could not grow");
        job->stack = v;
        job->cap = cap;
    }
    job->stack[job->n].q = q;
    job->stack[job->n].state = state;
    job->stack[job->n].opened = state == LJ_COMPUTING;
    job->stack[job->n].staged = false;
    job->stack[job->n].ctx = job->stack[job->n].outer = job->ctx;
    return job->n++;
}

/* Whether every entry is suspended: the one shape a job may keep across a rest. */
static bool lj_all_suspended(const LayoutJob *job)
{
    size_t i;

    for (i = 0; i < job->n; i++)
        if (job->stack[i].state != LJ_SUSPENDED) return false;
    return true;
}

/* ---- driven jobs ---------------------------------------------------------------------------------------- */

LayoutJob *layout_job_new(LayoutJobDriver driver)
{
    LayoutJob *job = calloc(1, sizeof *job);

    CHECK(job != NULL, "a layout job could not be allocated");
    job->gen = 1;
    job->driven = true;
    g_alive++;
    switch (driver) {
    case LAYOUT_JOB_STEP:  METRIC_ADD(jobs_step, 1);  break;
    case LAYOUT_JOB_PAINT: METRIC_ADD(jobs_paint, 1); break;
    }
    job->driver = driver;
    return job;
}

void layout_job_free(LayoutJob *job)
{
    DCHECK(job != NULL && job->driven, "a layout job was freed that no driver owns");
    DCHECK(job != g_entered, "a layout job was freed while its activation still holds the thread");
    DCHECKF(lj_all_suspended(job) && job->drives == 0,
            "a layout job was freed with a computing question on its stack of %zu or %u drives open; a driver "
            "frees its job only after leaving it, and leaving asserts neither", job->n, job->drives);
    DCHECK(g_alive > 0, "a layout job was freed with no driven job alive");
    g_alive--;
    DCHECKF(job->ctx == 0, "a layout job was freed inside grid row context %u", job->ctx);
    lj_contexts_release(job, true);
    free(job->ctxs);
    flow_placement_record_release(&job->placement);
    css_cascade_record_release(&job->cascade);
    line_box_record_release(&job->line_boxes);
    intrinsic_size_record_release(&job->intrinsic);
    free(job->stack);
    free(job->memo);
    lj_answers_free(job);
    free(job);
}

void layout_job_enter(LayoutJob *job)
{
    DCHECK(job != NULL && job->driven, "a layout job was entered that no driver owns");
    DCHECK(g_entered == NULL,
           "a layout job was entered while another job's activation holds the thread — layout runs no page "
           "code, so a second driver can only be reached from inside the first one's activation");
    DCHECKF(g_sync.n == 0 && g_sync.drives == 0,
            "a layout job was entered while the synchronous job has %zu questions and %u drives open, so a "
            "driver was reached from inside an undriven layout entry", g_sync.n, g_sync.drives);
    g_entered = job;
}

void layout_job_leave(LayoutJob *job)
{
    DCHECK(g_entered == job, "a layout job was left by a driver that did not enter it");
    DCHECKF(lj_all_suspended(job) && job->drives == 0,
            "a layout job was left with a computing question on its stack of %zu or %u drives open; a rest "
            "point taken now would carry live C-stack state, where only suspended questions may cross one",
            job->n, job->drives);
    DCHECKF(job->ctx == 0, "a layout job was left inside grid row context %u; a row sizing ends in the frame "
            "that began it", job->ctx);
    g_entered = NULL;
}

void layout_job_rested(LayoutJob *job)
{
    DCHECK(job != NULL && job->driven && job != g_entered,
           "a layout job was reported resting while it is entered — a driver leaves its job before it yields");
    DCHECK(job->driver == LAYOUT_JOB_STEP,
           "a paint's layout job was reported resting — a paint runs its job to completion");
    job->rests++;
    METRIC_ADD(job_rests, 1);
}

static void *lj_step_clone(JSContext *ctx, void *root, void **cursors[], int ncursors)
{
    LayoutJob *src = root;

    (void)ctx; (void)cursors;
    DCHECK(ncursors == 0, "a layout job holds no interior pointer a step machine may keep");
    DCHECK(src != g_entered && lj_all_suspended(src),
           "a layout job was forked while its activation holds the thread — a job is entered only inside one "
           "step re-entry, and no fork is taken inside layout");
    /* The clone is a new job with an empty memo and records: all are re-derivable, so the arm re-derives them. */
    DCHECK(src->driver == LAYOUT_JOB_STEP,
           "a paint's layout job was forked — a paint is host time and holds no step state");
    return layout_job_new(LAYOUT_JOB_STEP);
}

static void lj_step_destroy(JSContext *ctx, void *root)
{
    (void)ctx;
    layout_job_free(root);
}

const JSStepTreeOps layout_job_step_ops = { lj_step_clone, lj_step_destroy };

bool layout_job_holds_thread(void)
{
    return g_entered != NULL || g_sync.n != 0 || g_sync.drives != 0;
}

size_t layout_job_alive(void)
{
    return g_alive;
}

/* The job whose records are held now, or NULL; and the current grid row context of a job, or NULL. */
static LayoutJob *lj_held(void)
{
    if (g_entered != NULL) return g_entered;
    return g_sync.drives != 0 ? &g_sync : NULL;
}

static LjContext *lj_context(const LayoutJob *job)
{
    return job->ctx != 0 ? job->ctxs[job->ctx - 1] : NULL;
}

FlowPlacementRecord *layout_job_placement(void)
{
    LayoutJob *job = lj_held();

    if (job == NULL) return NULL;
    return lj_context(job) != NULL ? &lj_context(job)->placement : &job->placement;
}

CssCascadeRecord *layout_job_cascade(void)
{
    if (g_entered != NULL) return &g_entered->cascade;
    return g_sync.drives != 0 ? &g_sync.cascade : NULL;
}

LineBoxRecord *layout_job_line_boxes(void)
{
    LayoutJob *job = lj_held();

    if (job == NULL) return NULL;
    return lj_context(job) != NULL ? &lj_context(job)->line_boxes : &job->line_boxes;
}

IntrinsicSizeRecord *layout_job_intrinsic_sizes(void)
{
    LayoutJob *job = lj_held();

    if (job == NULL) return NULL;
    return lj_context(job) != NULL ? &lj_context(job)->intrinsic : &job->intrinsic;
}

/* ---- grid row contexts ---------------------------------------------------------------------------------- */

unsigned layout_job_grid_rows_enter(const lxb_dom_element_t *container)
{
    LayoutJob *job = lj_current();
    unsigned outer = job->ctx;
    size_t i;

    DCHECK(container != NULL, "a grid row context was entered for no container");
    for (i = 0; i < job->nctx; i++)
        if (job->ctxs[i]->parent == outer && job->ctxs[i]->container == container) break;
    if (i == job->nctx) {
        if (job->nctx == job->ctx_cap) {
            size_t cap = job->ctx_cap != 0 ? job->ctx_cap * 2 : 8;
            LjContext **v = realloc(job->ctxs, cap * sizeof *v);

            CHECK(v != NULL, "a layout job's grid row contexts could not grow");
            job->ctxs = v;
            job->ctx_cap = cap;
        }
        job->ctxs[i] = calloc(1, sizeof **job->ctxs);
        CHECK(job->ctxs[i] != NULL, "a layout job's grid row context could not be allocated");
        job->ctxs[i]->parent = outer;
        job->ctxs[i]->container = container;
        job->nctx++;
    }
    job->ctx = (unsigned)i + 1;
    return outer;
}

void layout_job_grid_rows_leave(const lxb_dom_element_t *container, unsigned outer)
{
    LayoutJob *job = lj_current();
    const LjContext *c = lj_context(job);

    DCHECKF(c != NULL && c->container == container && c->parent == outer,
            "a grid row context was left that is not the current one (current %u, leaving the one entered from "
            "%u); row sizings nest, so they end in reverse order", job->ctx, outer);
    job->ctx = outer;
}

bool layout_job_grid_rows_sizing(const lxb_dom_element_t *container)
{
    const LayoutJob *job = lj_current();
    unsigned c;

    for (c = job->ctx; c != 0; c = job->ctxs[c - 1]->parent)
        if (job->ctxs[c - 1]->container == container) return true;
    return false;
}

/* ---- the drive ------------------------------------------------------------------------------------------ */

void layout_job_drive_begin_at(LayoutJobDrive *d, const char *file, int line)
{
    LayoutJob *job = lj_current();

    d->job = job;
    d->base = job->base;
    d->nest = job->nest;
    d->rests = false;
    d->file = file;
    d->line = line;
    d->outer_file = job->drive_file;
    d->outer_line = job->drive_line;
    job->drive_file = file;
    job->drive_line = line;
    job->base = job->n;
    job->nest = 0;
    job->drives++;
}

void layout_job_drive_begin_resting(LayoutJobDrive *d)
{
    LayoutJob *job = g_entered;

    DCHECK(job != NULL && job->driver == LAYOUT_JOB_STEP,
           "a resting drive was begun with no step machine's job entered; only a driver that yields can rest");
    DCHECKF(job->drives == 0 && lj_all_suspended(job),
            "a resting drive was begun under %u open drives or over a computing question; it is its job's "
            "outermost drive, and every entry it finds is a question the last activation left", job->drives);
    d->job = job;
    d->base = job->base;
    d->nest = job->nest;
    d->rests = true;
    d->file = __FILE__;
    d->line = __LINE__;
    d->outer_file = job->drive_file;
    d->outer_line = job->drive_line;
    job->drive_file = d->file;
    job->drive_line = d->line;
    job->base = 0;
    job->nest = 0;
    job->drives++;
    job->quota = rest_unit_items(REST_UNIT_LAYOUT_BOXES);
    job->scan = rest_unit_items(REST_UNIT_LAYOUT_FLOAT_SCAN);
    job->line_items = rest_unit_items(REST_UNIT_LAYOUT_LINE_ITEMS);
    job->line_metrics = rest_unit_items(REST_UNIT_LAYOUT_LINE_METRICS);
    job->resting = true;
}

/* Whether one of the activation's four allowances is spent. */
static bool lj_allowance_spent(const LayoutJob *job)
{
    return job->quota == 0 || job->scan == 0 || job->line_items == 0 || job->line_metrics == 0;
}

bool layout_job_drive_pending(const LayoutJobDrive *d)
{
    DCHECK(d->job == lj_current(), "a drive was asked about under a job other than the one it began on");
    return d->job->n > d->job->base;
}

bool layout_job_drive_spent(const LayoutJobDrive *d)
{
    DCHECK(d->job == lj_current(), "a drive was asked about under a job other than the one it began on");
    return d->rests && lj_allowance_spent(d->job);
}

/* Whether a box is deferred because a resting drive spent its items. Only while the resting drive is the
   innermost: a drive nested inside a body computes what it defers before returning, so deferring there buys
   no rest. */
static bool lj_quota_spent(const LayoutJob *job)
{
    return job->resting && job->drives == 1 && lj_allowance_spent(job);
}

bool layout_job_rests_innermost(void)
{
    LayoutJob *job = lj_current();

    return job->resting && job->drives == 1;
}

bool layout_job_line_items_left(void)
{
    LayoutJob *job = lj_current();

    if (!(job->resting && job->drives == 1)) return true;
    return !lj_quota_spent(job);
}

void layout_job_line_items_spend(size_t n)
{
    LayoutJob *job = lj_current();

    if (!(job->resting && job->drives == 1)) return;
    job->line_items = n < job->line_items ? job->line_items - n : 0;
    METRIC_ADD(line_items, n);
}

void layout_job_line_metrics_spend(size_t n)
{
    LayoutJob *job = lj_current();

    if (!(job->resting && job->drives == 1)) return;
    job->line_metrics = n < job->line_metrics ? job->line_metrics - n : 0;
}

bool layout_job_scan_take(void)
{
    LayoutJob *job = lj_current();

    if (!(job->resting && job->drives == 1)) return true;
    if (lj_quota_spent(job)) return false;
    job->scan--;
    return true;
}

static void lj_quota_take(LayoutJob *job)
{
    if (job->resting && job->drives == 1) job->quota--;
}

bool layout_job_drive_next(LayoutJobDrive *d, LayoutQuestion *q, size_t *at)
{
    LayoutJob *job = d->job;
    LjEntry *e;

    DCHECK(job == lj_current(), "a drive was continued under a job other than the one it began on");
    if (job->n == job->base) return false;
    e = &job->stack[job->n - 1];
    DCHECKF(e->state == LJ_SUSPENDED && lj_is_bf(e->q.kind),
            "a drive found a %s entry of kind %d at the top of its stack — only a deferred CSS 2.1 §9.4.1 "
            "contribution or stack is left above a drive's mark, because every other entry is closed by its "
            "own frame",
            e->state == LJ_COMPUTING ? "computing" : "suspended", (int)e->q.kind);
    DCHECK(!layout_job_drive_spent(d),
           "a resting drive was continued after it spent its boxes; its driver rests instead");
    e->state = LJ_COMPUTING;
    e->outer = job->ctx;
    job->ctx = e->ctx;
    job->nest++;
    /* A resumed body continues a box already counted; a deferred contribution lays out its box now. A stack
       counts its steps rather than itself. */
    if (!e->opened && e->q.kind == LAYOUT_Q_BF_BOX) lj_quota_take(job);
    e->opened = true;
    *q = e->q;
    *at = job->n - 1;
    return true;
}

void layout_job_drive_end(LayoutJobDrive *d)
{
    LayoutJob *job = d->job;

    DCHECK(job == lj_current(), "a drive was ended under a job other than the one it began on");
    DCHECKF((d->rests ? lj_all_suspended(job) : job->n == job->base) && job->nest == 0,
            "a drive ended with %zu entries above its mark and %u bodies computing — a drive returns only "
            "after its own walk answered, which leaves nothing it deferred, and a resting drive leaves only "
            "suspended questions", job->n - job->base, job->nest);
    DCHECK(job->drives > 0, "a drive was ended with none open");
    job->base = d->base;
    job->nest = d->nest;
    job->drives--;
    job->drive_file = d->outer_file;
    job->drive_line = d->outer_line;
    if (d->rests) job->resting = false;
    if (job == &g_sync && job->drives == 0) lj_memo_shed(job);
}

/* ---- LAYOUT_Q_BF_BOX ------------------------------------------------------------------------------------ */

LayoutAsk layout_job_bf_ask(LayoutQuestion q, LayoutBfAnswer *out, size_t *at)
{
    LayoutJob *job = lj_current();
    LjSlot *s;

    DCHECK(lj_is_bf(q.kind) && q.element != NULL && q.code != 0,
           "a CSS 2.1 §9.4.1 contribution or stack was asked with a question of another kind, no element or "
           "no pass");
    DCHECK(job->drives > 0,
           "a CSS 2.1 §9.4.1 contribution was asked with no drive open — its asker is the stack walk, whose "
           "callers are either a contribution body or a drive, so a drive is always beneath it");
    METRIC_ADD(bf_asks, 1);
    if (job->memo_cap != 0) {
        s = lj_probe(job->memo, job->memo_cap, job->gen, q, job->ctx);
        if (s->gen == job->gen) {
            METRIC_ADD(bf_served, 1);
            if (s->rest < job->rests) METRIC_ADD(served_across_rest, 1);
            *out = *s->a;
            return LAYOUT_ASK_ANSWERED;
        }
    }
    if (job->nest >= rest_unit_items(REST_UNIT_LAYOUT_QUESTIONS) || lj_quota_spent(job)) {
        *at = lj_push(job, q, LJ_SUSPENDED);
        METRIC_ADD(bf_deferred, 1);
        return LAYOUT_ASK_DEFERRED;
    }
    if (job->resting && job->drives > 1) {
        char ask[LAYOUT_QUESTION_ASK_CAP], chain[LAYOUT_QUESTION_CHAIN_CAP];
        bool cut = false;

        DFAILF("%s:%d opens a drive inside a resting layout job, and %s would run to completion under it, so the "
               "step machine owning the job cannot rest there (open: %s). BUILD that caller as an ask that "
               "answers \"not yet\" to the resting drive", job->drive_file, job->drive_line,
               layout_question_ask(q, ask, sizeof ask), layout_question_chain(8, chain, sizeof chain, &cut));
    }
    *at = lj_push(job, q, LJ_COMPUTING);
    job->nest++;
    if (q.kind == LAYOUT_Q_BF_BOX) lj_quota_take(job);
    return LAYOUT_ASK_OPENED;
}

void layout_job_bf_answer(size_t at, const LayoutBfAnswer *answer)
{
    LayoutJob *job = lj_current();
    LjEntry *e;
    LjSlot *s;

    DCHECKF(at + 1 == job->n, "a CSS 2.1 §9.4.1 contribution answered at entry %zu of a stack of %zu — a "
            "body answers only when every question it asked is closed", at, job->n);
    e = &job->stack[at];
    DCHECK(e->state == LJ_COMPUTING && lj_is_bf(e->q.kind),
           "a CSS 2.1 §9.4.1 contribution or stack answered an entry that is neither, computing");
    DCHECK(job->nest > 0, "a contribution body answered with no body computing above its drive");
    DCHECK(!e->staged,
           "a contribution body answered with a staged walk it never resumed; the body's run took a path to "
           "its answer that does not pass through the walk it stopped in");
    DCHECKF(job->ctx == e->ctx, "a CSS 2.1 §9.4.1 body answered in grid row context %u, asked in %u; a row "
            "sizing its body began ends before the body answers", job->ctx, e->ctx);
    if (job->memo_used * 2 >= job->memo_cap) lj_memo_grow(job);
    s = lj_probe(job->memo, job->memo_cap, job->gen, e->q, e->ctx);
    DCHECK(s->gen != job->gen,
           "a CSS 2.1 §9.4.1 contribution was computed while the memo already held it — the ask consults the "
           "memo before it pushes, so a second body for one question is a probe that missed its own key");
    s->el = e->q.element;
    s->kind = e->q.kind;
    s->code = e->q.code;
    s->ctx = e->ctx;
    s->gen = job->gen;
    s->rest = job->rests;
    s->a = lj_answer_alloc(job);
    *s->a = *answer;
    job->memo_used++;
    job->n--;
    job->nest--;
    job->ctx = e->outer;
}

void layout_job_suspend(size_t at)
{
    LayoutJob *job = lj_current();
    size_t i;

    DCHECKF(at < job->n && at >= job->base, "a body suspended entry %zu outside its drive's range [%zu, %zu)",
            at, job->base, job->n);
    DCHECK(job->stack[at].state == LJ_COMPUTING && lj_is_bf(job->stack[at].q.kind),
           "a body suspended an entry that is not a computing CSS 2.1 §9.4.1 contribution or stack");
    for (i = at + 1; i < job->n; i++)
        DCHECK(job->stack[i].state == LJ_SUSPENDED,
               "a body returned \"not yet\" while a question above it is still computing — \"not yet\" "
               "propagates from the deferred ask down, so every entry above a suspending body is suspended");
    DCHECK(at + 1 < job->n || job->stack[at].staged,
           "a body returned \"not yet\" with no deferred question above it and no walk staged; only a walk "
           "stopped by a spent quota suspends with nothing above it, and it stages first");
    DCHECKF(job->ctx == job->stack[at].ctx, "a CSS 2.1 §9.4.1 body suspended in grid row context %u, asked in "
            "%u; a row sizing its body began ends before the body returns", job->ctx, job->stack[at].ctx);
    job->stack[at].state = LJ_SUSPENDED;
    job->nest--;
    job->ctx = job->stack[at].outer;
    DCHECK(job->stack[at].q.kind != LAYOUT_Q_BF_WALK || job->stack[at].staged,
           "a §9.4.1 stack returned \"not yet\" without staging its walk; every return of \"not yet\" in "
           "block_flow.c's bf_layout stages, so its next run continues rather than restarts");
    if (!job->stack[at].staged) METRIC_ADD(bf_replays, 1);
}

static LjEntry *lj_body(LayoutJob *job, size_t at, const lxb_dom_element_t *el)
{
    LjEntry *e;

    DCHECKF(at < job->n && at >= job->base, "a body named entry %zu outside its drive's range [%zu, %zu)",
            at, job->base, job->n);
    e = &job->stack[at];
    DCHECK(e->state == LJ_COMPUTING && e->q.kind == LAYOUT_Q_BF_WALK && e->q.element == el,
           "a stack walk named an entry that is not the computing CSS 2.1 §9.4.1 stack of the element it "
           "walks; a stack's body passes its own entry to its walk");
    return e;
}

void layout_job_bf_stage(size_t at, const lxb_dom_element_t *el, const LayoutBfCursor *c)
{
    LjEntry *e = lj_body(lj_current(), at, el);

    DCHECK(!e->staged, "a body staged its walk twice in one run; it stages once, as it returns \"not yet\"");
    e->cur = *c;
    e->staged = true;
}

bool layout_job_bf_resume(size_t at, const lxb_dom_element_t *el, LayoutBfCursor *c)
{
    LjEntry *e = lj_body(lj_current(), at, el);

    if (!e->staged) return false;
    *c = e->cur;
    e->staged = false;
    return true;
}

bool layout_job_bf_step(size_t at)
{
    LayoutJob *job = lj_current();

    DCHECKF(at + 1 == job->n && job->stack[at].state == LJ_COMPUTING,
            "a stack walk counted a box for entry %zu of a stack of %zu; only the computing body on top "
            "walks", at, job->n);
    if (lj_quota_spent(job)) return false;
    lj_quota_take(job);
    return true;
}

/* ---- the open chain ------------------------------------------------------------------------------------- */

size_t layout_job_open(LayoutQuestion q)
{
    DCHECK(!lj_is_bf(q.kind), "a CSS 2.1 §9.4.1 contribution or stack is asked through layout_job_bf_ask");
    return lj_push(lj_current(), q, LJ_COMPUTING);
}

bool layout_job_top_is(size_t at)
{
    LayoutJob *job = lj_current();

    return at + 1 == job->n && job->stack[at].state == LJ_COMPUTING;
}

void layout_job_close(size_t at)
{
    LayoutJob *job = lj_current();

    DCHECK(at + 1 == job->n, "a layout question was closed that is not the top of its job's stack");
    job->n--;
}

unsigned layout_question_repeat(LayoutQuestion q)
{
    LayoutJob *job = lj_current();
    unsigned up = 0;
    size_t i;

    if (q.element == NULL) return 0;
    for (i = job->n; i > 0; i--) {
        up++;
        if (job->stack[i - 1].ctx == job->ctx && layout_question_same(job->stack[i - 1].q, q)) return up;
    }
    return 0;
}

const char *layout_question_chain(unsigned frames, char *buf, size_t cap, bool *cut)
{
    LayoutJob *job = lj_current();
    char abuf[LAYOUT_QUESTION_ASK_CAP];
    size_t at = 0, i;
    unsigned k = 0;

    if (cut != NULL) *cut = false;
    if (buf == NULL || cap == 0) return "(nowhere to compose an open chain)";
    buf[0] = '\0';
    for (i = job->n; i > 0 && k < frames; i--, k++) {
        const LjEntry *e = &job->stack[i - 1];
        int w = snprintf(buf + at, cap - at, "%s%s%s", at == 0 ? "" : " <- ",
                         layout_question_ask(e->q, abuf, sizeof abuf),
                         e->state == LJ_SUSPENDED ? " (suspended)" : "");

        if (w < 0 || (size_t) w >= cap - at) { if (cut != NULL) *cut = true; break; }
        at += (size_t) w;
    }
    return buf;
}
