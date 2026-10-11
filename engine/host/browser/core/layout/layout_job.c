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
   lays out its box. `staged` is whether `cur` holds the walk the body stopped at. */
typedef struct {
    LayoutQuestion q;
    LjState        state;
    bool           opened, staged;
    LayoutBfCursor cur;
} LjEntry;

/* One memo slot, live when `gen` equals its job's. `rest` is the job's rest count when the answer was
   recorded, which is how a hit after a later rest is told apart. */
typedef struct {
    const lxb_dom_element_t *el;
    LayoutQuestionKind       kind;
    unsigned                 code;
    uint64_t                 gen;
    uint64_t                 rest;
    LayoutBfAnswer           a;
} LjSlot;

struct LayoutJob {
    LjEntry  *stack;
    size_t    n, cap;
    size_t    base;      /* the innermost open drive's mark; entries below it belong to drives further out */
    unsigned  nest;      /* computing LAYOUT_Q_BF_BOX entries above `base` */
    unsigned  drives;    /* drives open on this job */
    size_t    quota;     /* boxes a resting drive may still lay out in this activation */
    bool      resting;   /* a resting drive is open, as the job's outermost */
    LjSlot   *memo;
    size_t    memo_cap;  /* a power of two, or zero before the first answer */
    size_t    memo_used;
    uint64_t  gen;       /* starts at 1, so a zeroed slot is never live */
    uint64_t  rests;
    bool      driven;
    LayoutJobDriver driver;
    FlowPlacementRecord placement;   /* core/layout/flow_placement.h's three facts, for this job */
    CssCascadeRecord    cascade;     /* core/css/css_cascade_pass.h's cascaded values, for this job */
    LineBoxRecord       line_boxes;  /* core/layout/line_box.h's fills, for this job */
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

static size_t lj_hash(const lxb_dom_element_t *el, LayoutQuestionKind kind, unsigned code, size_t cap)
{
    uint64_t h = (uint64_t)(uintptr_t)el * 0x9E3779B97F4A7C15ull ^
                 ((uint64_t)code << 8 | (uint64_t)kind) * 0xC2B2AE3D27D4EB4Full;

    return (size_t)(h ^ (h >> 29)) & (cap - 1);
}

static LjSlot *lj_probe(LjSlot *tab, size_t cap, uint64_t gen, LayoutQuestion q)
{
    size_t i = lj_hash(q.element, q.kind, q.code, cap);

    while (tab[i].gen == gen && (tab[i].el != q.element || tab[i].kind != q.kind || tab[i].code != q.code))
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
                                                      NULL, job->memo[i].code)) = job->memo[i];
    }
    free(job->memo);
    job->memo = tab;
    job->memo_cap = cap;
}

/* Emptying is a generation step, so a job reused across activations pays nothing per slot. */
static void lj_memo_shed(LayoutJob *job)
{
    job->gen++;
    job->memo_used = 0;
    flow_placement_record_release(&job->placement);
    css_cascade_record_release(&job->cascade);
    line_box_record_release(&job->line_boxes);
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
    flow_placement_record_release(&job->placement);
    css_cascade_record_release(&job->cascade);
    line_box_record_release(&job->line_boxes);
    free(job->stack);
    free(job->memo);
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

FlowPlacementRecord *layout_job_placement(void)
{
    if (g_entered != NULL) return &g_entered->placement;
    return g_sync.drives != 0 ? &g_sync.placement : NULL;
}

CssCascadeRecord *layout_job_cascade(void)
{
    if (g_entered != NULL) return &g_entered->cascade;
    return g_sync.drives != 0 ? &g_sync.cascade : NULL;
}

LineBoxRecord *layout_job_line_boxes(void)
{
    if (g_entered != NULL) return &g_entered->line_boxes;
    return g_sync.drives != 0 ? &g_sync.line_boxes : NULL;
}

/* ---- the drive ------------------------------------------------------------------------------------------ */

void layout_job_drive_begin(LayoutJobDrive *d)
{
    LayoutJob *job = lj_current();

    d->job = job;
    d->base = job->base;
    d->nest = job->nest;
    d->rests = false;
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
    job->base = 0;
    job->nest = 0;
    job->drives++;
    job->quota = rest_unit_items(REST_UNIT_LAYOUT_BOXES);
    job->resting = true;
}

bool layout_job_drive_spent(const LayoutJobDrive *d)
{
    DCHECK(d->job == lj_current(), "a drive was asked about under a job other than the one it began on");
    return d->rests && d->job->quota == 0;
}

/* Whether a box is deferred because a resting drive spent its items. Only while the resting drive is the
   innermost: a drive nested inside a body computes what it defers before returning, so deferring there buys
   no rest. */
static bool lj_quota_spent(const LayoutJob *job)
{
    return job->resting && job->drives == 1 && job->quota == 0;
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
        s = lj_probe(job->memo, job->memo_cap, job->gen, q);
        if (s->gen == job->gen) {
            METRIC_ADD(bf_served, 1);
            if (s->rest < job->rests) METRIC_ADD(served_across_rest, 1);
            *out = s->a;
            return LAYOUT_ASK_ANSWERED;
        }
    }
    if (job->nest >= rest_unit_items(REST_UNIT_LAYOUT_QUESTIONS) || lj_quota_spent(job)) {
        *at = lj_push(job, q, LJ_SUSPENDED);
        METRIC_ADD(bf_deferred, 1);
        return LAYOUT_ASK_DEFERRED;
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
    if (job->memo_used * 2 >= job->memo_cap) lj_memo_grow(job);
    s = lj_probe(job->memo, job->memo_cap, job->gen, e->q);
    DCHECK(s->gen != job->gen,
           "a CSS 2.1 §9.4.1 contribution was computed while the memo already held it — the ask consults the "
           "memo before it pushes, so a second body for one question is a probe that missed its own key");
    s->el = e->q.element;
    s->kind = e->q.kind;
    s->code = e->q.code;
    s->gen = job->gen;
    s->rest = job->rests;
    s->a = *answer;
    job->memo_used++;
    job->n--;
    job->nest--;
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
    job->stack[at].state = LJ_SUSPENDED;
    job->nest--;
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
        if (layout_question_same(job->stack[i - 1].q, q)) return up;
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
