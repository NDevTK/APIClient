/* A layout job: the evaluator layout questions (core/layout/layout_question.h) are asked through. It owns
 * the stack of open questions, a memo of answered ones, core/layout/flow_placement.h's placement record,
 * core/css/css_cascade_pass.h's cascaded-value record, core/layout/line_box.h's line box record, core/layout/intrinsic_size.h's intrinsic size record, and the accounting of how many question bodies one C activation hosts. It is the analogue of LayoutNG's
 * layout-result cache, scoped to one job rather than hung on a box, because layout here is per-flow state
 * with no box tree to hang it on. */

/* The stack replaces C frames. A question is pushed when asked. Its body either completes (the entry is
 * popped and, for LAYOUT_Q_BF_BOX, memoized) or asks a question the job defers: that question is pushed
 * suspended and the asking bodies return "not yet" up to the nearest drive, each leaving its entry on the
 * stack as suspended with its stack walk staged (LayoutBfCursor). The drive computes the stack from the top
 * down on a fresh C stack, each body continuing at the step it stopped at, then re-runs its own walk, which
 * finds every completed answer in the memo.
 *
 * The interruption is a typed return, not a non-local exit: frames between an ask and its drive hold heap
 * temporaries and process-wide brackets across their calls (table grids, line fragments, measurement
 * buffers, the paint passes), so only a frame that propagates "not yet" explicitly may be unwound. */

/* Which job. A job its driver entered (`layout_job_enter`) is current for that activation. Otherwise the
 * thread's synchronous job is: it serves every layout entry that does not drive a job, and its memo is
 * emptied when its outermost drive ends, so nothing it holds outlives the activation that computed it.
 *
 * Memo validity. An answer is a function of the document, the cascade and the viewport. No DOM write may
 * happen while a job's activation holds the thread (asserted at solver/dom_cow.c's write entries), and a
 * driven job rests only inside its own flow's member call, where that flow writes nothing and every other
 * flow's writes stay in that flow's delta. A host-time baseline write while a driven job is alive is
 * refused at solver/dom_cow.c for the same reason. */
/* NAMED RESIDUAL, the synchronous job. Not covered: every layout entry but CSSOM VIEW §6's
 * `scrollIntoView` (core/dom/element_view.c) and the paint (core/paint/document_paint.c) runs on the
 * thread's synchronous job, which never rests, so a page's flow cannot be preempted while one of those
 * entries lays out. Next: each such member becomes a step machine that owns a driven job, as
 * `scrollIntoView` does. How its absence shows: a flow's slice overrun inside a layout member that is not
 * `scrollIntoView`, with `_layoutJobRests` flat. */

/* The resting drive. `scrollIntoView`'s step machine is the drive of its scrolling-area walk: a node's
 * border-box origin through CSS 2.1 §10.1's second case and through §9.4.2's line boxes (the establishing box,
 * its anonymous-box enumeration and its origin; core/layout/flow_position.h's and line_box.h's asks)
 * propagates "not yet" to the walk step, the questions it deferred stay suspended on the job across the rest,
 * and the next activation computes them before it retries the node. A resting drive also stops once it has
 * laid out rest_unit_items(REST_UNIT_LAYOUT_BOXES) boxes in one activation, so a node whose derivation is wide
 * rests mid-node, and a body whose child list is long rests mid-walk, rather than only a deep one. */
/* NAMED RESIDUAL, drives that are not rest points. Not covered: core/layout/block_flow.c's auto-height entry
 * is still driven where core/layout/used_value.c sizes a box for a caller inside the walk outside a line (an
 * absolutely positioned box whose positioned ancestor is a table or a flex or grid item, an `inline-block`
 * whose `min-height` or `max-height` is a content keyword, and intrinsic_block_size.c's flex item sizes), so
 * those compute what they deferred inside the same activation. Next: intrinsic_block_size.c's block size as an
 * ask, propagated through the flex line's item sizes. How its absence shows: the REST_UNIT_LAYOUT_DESCENDANTS
 * bracket's CPU assert naming a unit whose heavy node is a flex item. */

/* NAMED RESIDUAL, cycles that cross no question kind. Not covered: the layout call graph has recursion
 * cycles that pass through none of the three kinds (intrinsic sizes, line-box atomic extents, the
 * containing-block origin, margins with a size operand), and their frames still nest on the C stack within
 * one body, unbounded by REST_UNIT_LAYOUT_QUESTIONS. Next: a question kind at each cut of those cycles.
 * How its absence shows: a deep document's C stack grows with its depth along a frame cycle that names no
 * `bf_box`, `uv_sized` or `used_value_px`. */

#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_LAYOUT_JOB_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_LAYOUT_JOB_H
#include <stdbool.h>
#include <stddef.h>

#include "quickjs-step.h"

#include "check.h"
#include "core/css/css_length.h"
#include "core/css/css_cascade_pass.h"
#include "core/layout/flow_placement.h"
#include "core/layout/layout_question.h"
#include "core/layout/intrinsic_size.h"
#include "core/layout/line_box.h"

typedef struct LayoutJob LayoutJob;

/* What LAYOUT_Q_BF_BOX answers: CSS 2.1 §9.4.1's contribution of one box to its parent's stack, with
   CSS 2.1 §8.3.1's two adjoining runs as their extrema and CSS 2.2 §10.8.1's baseline for the asked pass.
   LAYOUT_Q_BF_WALK answers the same shape for the stack of a box's own children, which the contribution's
   body asks for; the stack's body also writes what its other readers take from it into flow_placement.h's
   record. */
typedef struct {
    CssPx content_h, border_h;
    CssPx top_pos, top_neg, bottom_pos, bottom_neg;
    CssPx baseline;
    bool  collapse_through, is_table_wrapper, has_line_box;
} LayoutBfAnswer;

/* What a LAYOUT_Q_BF_WALK body stages before it returns "not yet": its §9.4.1 stack walk's loop state at the
   step it retries (core/layout/block_flow.c's BfWalk, with §8.3.1's runs as their extrema). The two nodes
   are the walk's enumeration cursor and stay valid across a rest by the memo validity argument above. */
typedef struct {
    lxb_dom_node_t *prev, *brk;
    size_t wi;
    bool   ifc, at_run, run_inline, escaping, placed, has_line_box;
    CssPx  pos, run_pos, run_neg, esc_pos, esc_neg, baseline;
} LayoutBfCursor;

/* ---- driven jobs ---------------------------------------------------------------------------------------- */

/* A job a driver owns across its activations. `free` asserts the job is not entered, no drive is open and
   no question is computing. */
typedef enum {
    LAYOUT_JOB_STEP = 1,   /* owned by a step machine's state; it may rest */
    LAYOUT_JOB_PAINT       /* owned by a paint, which runs it to completion in one activation */
} LayoutJobDriver;
LayoutJob *layout_job_new(LayoutJobDriver driver);
void layout_job_free(LayoutJob *job);

/* Make `job` current for one activation, and give the thread back. `enter` asserts no job's activation
   holds the thread; `leave` asserts no drive is open and every entry left is suspended, which is what makes a
   rest point taken after it carry nothing but the memo and questions not yet asked of a body. */
void layout_job_enter(LayoutJob *job);
void layout_job_leave(LayoutJob *job);

/* The driver took a rest point with `job` alive (a step machine returned JS_STEP_YIELD). Answers memoized
   before it and served after it are what the census counts as served across a rest. */
void layout_job_rested(LayoutJob *job);

/* A driven job held in a step state. The clone answers a job with an empty memo, empty records and no
   suspended questions: all are re-derivable, so a forked arm re-derives them rather than sharing or copying
   them, and re-asks the node its walk's cursor still names. A job has no interior
   pointers a machine holds, so a declarer passes no cursors. */
extern const JSStepTreeOps layout_job_step_ops;

/* Whether a job's activation holds the thread (an entered job, or the synchronous job inside a drive or with
   a question open), and how many driven jobs are alive. Read by solver/dom_cow.c's write entries, which
   assert against both; nothing branches on them. */
bool layout_job_holds_thread(void);
size_t layout_job_alive(void);

/* The placement record (core/layout/flow_placement.h) of the job that holds it now: a driven job's while it
   is entered, the synchronous job's while a drive is open, and NULL otherwise. The record is the job's and
   is released with its memo. */
FlowPlacementRecord *layout_job_placement(void);
/* The same for css-cascade-5 §4.2's cascaded-value record (core/css/css_cascade_pass.h). */
CssCascadeRecord *layout_job_cascade(void);
/* The same for CSS 2.2 §9.4.2's line box record (core/layout/line_box.h). */
LineBoxRecord *layout_job_line_boxes(void);
/* The same for css-sizing-3 §5.1's intrinsic size record (core/layout/intrinsic_size.h). */
IntrinsicSizeRecord *layout_job_intrinsic_sizes(void);

/* ---- the drive ------------------------------------------------------------------------------------------ */

/* A drive is a frame that cannot propagate "not yet" to its caller, so it computes every question deferred
   beneath it before it returns. `begin` takes the current job (the synchronous one when none is entered)
   and marks the stack; `next` hands back the topmost suspended question above the mark, now computing, or
   answers false when none is left; `end` asserts the stack is back at the mark. */
typedef struct LayoutJobDrive {
    LayoutJob *job;
    size_t     base;
    unsigned   nest;
    bool       rests;   /* begun by `layout_job_drive_begin_resting` */
} LayoutJobDrive;

void layout_job_drive_begin(LayoutJobDrive *d);
bool layout_job_drive_next(LayoutJobDrive *d, LayoutQuestion *q, size_t *at);
void layout_job_drive_end(LayoutJobDrive *d);

/* A drive whose driver rests (a step machine's, on its entered job). `begin_resting` asserts it is the job's
   outermost drive and takes every suspended entry the last activation left as its own; `spent` answers whether
   this activation has laid out its rest_unit_items(REST_UNIT_LAYOUT_BOXES) boxes, examined its
   rest_unit_items(REST_UNIT_LAYOUT_FLOAT_SCAN) elements, taken its rest_unit_items(REST_UNIT_LAYOUT_LINE_ITEMS)
   line box items or derived its rest_unit_items(REST_UNIT_LAYOUT_LINE_METRICS) box metrics; `end` on a resting drive
   leaves suspended entries in place for the next activation and asserts none is computing. */
void layout_job_drive_begin_resting(LayoutJobDrive *d);
bool layout_job_drive_spent(const LayoutJobDrive *d);

/* Whether a line box fill (core/layout/line_box.h) may take another step in this activation, and the items and
   box metrics a step took. Outside a resting drive, or under a drive nested inside a body, a fill runs to
   completion: `left` answers true and the spends count nothing. */
bool layout_job_line_items_left(void);
/* Whether a resting drive is innermost, the one condition under which the allowances above are spent. */
bool layout_job_rests_innermost(void);
void layout_job_line_items_spend(size_t n);
void layout_job_line_metrics_spend(size_t n);

/* ---- LAYOUT_Q_BF_BOX ------------------------------------------------------------------------------------ */

typedef enum {
    LAYOUT_ASK_ANSWERED,   /* `*out` holds the memoized answer; nothing was pushed */
    LAYOUT_ASK_OPENED,     /* pushed computing at `*at`: the caller runs the body and answers or suspends it */
    LAYOUT_ASK_DEFERRED    /* pushed suspended at `*at`: the caller returns "not yet" */
} LayoutAsk;

/* Ask a BF_BOX or BF_WALK question. Asserts a drive is open: every ask has one beneath it on the C stack. The
   question is deferred when the drive already hosts rest_unit_items(REST_UNIT_LAYOUT_QUESTIONS) computing
   bodies, or when a resting drive is innermost and has spent its boxes. A BF_BOX open takes one box; a stack
   counts its steps instead. */
LayoutAsk layout_job_bf_ask(LayoutQuestion q, LayoutBfAnswer *out, size_t *at);

/* The computing body at `at` completed: asserts it is the top, pops and memoizes it. */
void layout_job_bf_answer(size_t at, const LayoutBfAnswer *answer);

/* The computing body at `at` returned "not yet": asserts every entry above it is suspended and suspends it.
   A body suspends with a deferred question above it, or with a staged walk and nothing above it when its
   walk stopped at a spent quota (`layout_job_bf_step`). A stack always stages; a contribution's body, one ask
   of its stack, re-runs from its start (`_layoutBfReplays`). */
void layout_job_suspend(size_t at);

/* The computing body at `at`, whose question is about `el`, stages its walk before returning "not yet", and
   on its next run takes the staged walk back: `resume` answers whether one was staged, copies it out and
   clears it, so the walk continues at the step it stopped at instead of at its first child. */
void layout_job_bf_stage(size_t at, const lxb_dom_element_t *el, const LayoutBfCursor *c);
bool layout_job_bf_resume(size_t at, const lxb_dom_element_t *el, LayoutBfCursor *c);

/* The body at `at`'s walk is about to lay out a box with no question of its own (CSS 2.2 §9.2.1.1's anonymous
   block box). It answers false when a resting drive is innermost and has spent its items, and the body then
   stages and suspends; otherwise it takes one item when a resting drive is innermost. */
bool layout_job_bf_step(size_t at);

/* CSS 2.2 §9.5's float search (core/layout/block_flow.h) is about to examine one element. It answers false
   when a resting drive is innermost and has spent its items, and the search then holds its place and answers
   "not yet"; otherwise it takes one element when a resting drive is innermost. */
bool layout_job_scan_take(void);

/* ---- the open chain, for the kinds that are not stacked in release ------------------------------------- */

/* Push a computing question that its own frame pops, and pop it. The kinds asked through these never
   suspend: no frame between them and their drive propagates "not yet". */
size_t layout_job_open(LayoutQuestion q);
void layout_job_close(size_t at);
bool layout_job_top_is(size_t at);

/* How many entries down the identical question is already on the current job's stack, 1 for the top, 0 for
   none; and the stack rendered top first, stopping after `frames` entries. Both are total: a DFAILF message
   reaches them. A question with no element is never a repeat, because its own entry asserts its operands. */
unsigned layout_question_repeat(LayoutQuestion q);
const char *layout_question_chain(unsigned frames, char *buf, size_t cap, bool *cut);

/* The cycle test, expanded at each asking site so the DFAILF names that site. `remedy` is the kind's own
   sentence about what to do with a cycle in it. */
#if APICLIENT_DEV
#define LAYOUT_QUESTION_REPEAT_CHECK(question, remedy) do {                                                  \
    LayoutQuestion lq_q_ = (question);                                                                       \
    unsigned lq_up_ = layout_question_repeat(lq_q_);                                                         \
                                                                                                             \
    if (lq_up_ != 0) {                                                                                       \
        char lq_ask_[LAYOUT_QUESTION_ASK_CAP];                                                               \
        char lq_chain_[LAYOUT_QUESTION_CHAIN_CAP];                                                           \
        bool lq_cut_ = false;                                                                                \
        const char *lq_at_ = layout_question_ask(lq_q_, lq_ask_, sizeof lq_ask_);                            \
        const char *lq_c_ = layout_question_chain(lq_up_, lq_chain_, sizeof lq_chain_, &lq_cut_);            \
                                                                                                             \
        DFAILF("%s is its own input: the identical question is already open %u entries down the job's "     \
               "stack, so there is no fixed point to reach. The stack, top first: %s%s. %s A depth cap "     \
               "would answer nothing: a deep box tree passes this check, and the repetition is the defect",  \
               lq_at_, lq_up_, lq_c_, lq_cut_ ? " <- …" : "", (remedy));                                     \
    }                                                                                                        \
} while (0)
#else
#define LAYOUT_QUESTION_REPEAT_CHECK(question, remedy) ((void) 0)
#endif

/* A question its own frame opens and closes, stacked in dev builds only. `at` is a `size_t` on the asking
   frame. The close is checked at the site that opened, so a frame that returned without closing is named. */
#if APICLIENT_DEV
#define LAYOUT_QUESTION_OPEN(at, question, remedy) do {                                                      \
    LAYOUT_QUESTION_REPEAT_CHECK((question), (remedy));                                                      \
    (at) = layout_job_open((question));                                                                      \
} while (0)
#define LAYOUT_QUESTION_CLOSE(at) do {                                                                       \
    DCHECK(layout_job_top_is((at)),                                                                          \
           "a layout question is being closed by a frame whose entry is not the top of the job's stack, so " \
           "a frame between them returned without closing its own; close at every return of the entry that " \
           "opened");                                                                                        \
    layout_job_close((at));                                                                                  \
} while (0)
#else
#define LAYOUT_QUESTION_OPEN(at, question, remedy)  ((void) 0)
#define LAYOUT_QUESTION_CLOSE(at)                   ((void) 0)
#endif

#endif
