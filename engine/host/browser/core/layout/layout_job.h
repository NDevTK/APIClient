/* A layout job: the evaluator layout questions (core/layout/layout_question.h) are asked through. It owns
 * the stack of open questions, a memo of answered ones, core/layout/flow_placement.h's placement record,
 * core/css/css_cascade_pass.h's cascaded-value record, core/layout/line_box.h's line box record, core/layout/intrinsic_size.h's intrinsic size record, and the accounting of how many question bodies one C activation hosts. It is the analogue of LayoutNG's
 * layout-result cache, scoped to one job rather than hung on a box, because layout here is per-flow state
 * with no box tree to hang it on. */

/* The stack replaces C frames. A question is pushed when asked. Its body either completes (the entry is
 * popped and its answer memoized) or asks a question the job defers: that question is pushed
 * suspended and the asking bodies return "not yet" up to the nearest drive, each leaving its entry on the
 * stack as suspended with its stack walk staged (LayoutBfCursor). The drive computes the stack from the top
 * down on a fresh C stack, each body continuing at the step it stopped at, then re-runs its own walk, which
 * finds every completed answer in the memo.
 *
 * The interruption is a typed return, not a non-local exit: frames between an ask and its drive hold heap
 * temporaries and process-wide brackets across their calls (table grids, line fragments, measurement
 * buffers, the paint passes), so only a frame that propagates "not yet" explicitly may be unwound. */

/* Which job. A job its driver entered (`layout_job_enter`) is current for that activation, and every layout read
 * happens inside one: with none entered the read is refused (lj_current), and a computed value read with no
 * record held is refused (css_computed_style_require_record).
 *
 * Memo validity. An answer is a function of the document, the cascade and the viewport. No DOM write may
 * happen while a job's activation holds the thread (asserted at solver/dom_cow.c's write entries), and a
 * driven job rests only inside its own flow's member call, where that flow writes nothing and every other
 * flow's writes stay in that flow's delta. A host-time baseline write while a driven job is alive is
 * refused at solver/dom_cow.c for the same reason. */
/* NAMED RESIDUAL, the drives that do not rest. Not covered: a reader that cannot pass "not yet" up drives its
 * ask to the answer on a drive that does not rest, inside the entered job: the read twins
 * (flow_border_box_origin, block_flow_auto_height through used_value_px, block_flow_anonymous_boxes,
 * line_box_glyphs, intrinsic_inline_run_sizes, block_flow_establishes_inline_context) and the flex baselines'
 * flex_cross_size_anonymous_lines, where no resting drive is innermost. Update the rendering's paint step reads
 * them after core/paint/box_paint.c's box_paint_ready asked them, so it is served; scrollIntoView's
 * es_determine_position and scrolling_area_walk_viewport read them outside the step machine's resting drive.
 * Next: those reads asked under the caller's resting drive, and each served read asserting it is served. How its
 * absence shows: a flow's slice overrun inside a layout member with `_layoutJobRests` flat. */

/* The resting drive. `scrollIntoView`'s step machine is the drive of its scrolling-area walk: a node's
 * border-box origin through CSS 2.1 §10.1's second case and through §9.4.2's line boxes (the establishing box,
 * its anonymous-box enumeration and its origin; core/layout/flow_position.h's and line_box.h's asks)
 * propagates "not yet" to the walk step, the questions it deferred stay suspended on the job across the rest,
 * and the next activation computes them before it retries the node. A resting drive also stops once it has
 * laid out rest_unit_items(REST_UNIT_LAYOUT_BOXES) boxes in one activation, so a node whose derivation is wide
 * rests mid-node, and a body whose child list is long rests mid-walk, rather than only a deep one. */

/* NAMED RESIDUAL, cycles that cross no question kind. Not covered: the layout call graph has recursion
 * cycles that pass through none of the three kinds (intrinsic sizes, line-box atomic extents, the
 * containing-block origin, margins with a size operand), and their frames still nest on the C stack within
 * one body, as do the synchronous entries whose drive a body reaches. Next: a question kind at each cut of
 * those cycles.
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
#include "core/dom/selector_match.h"
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
    bool   classified, ifc, at_run, run_inline, escaping, placed, has_line_box;
    CssPx  pos, run_pos, run_neg, esc_pos, esc_neg, baseline;
} LayoutBfCursor;

/* What a stacked question answers, one member per kind: `bf` for LAYOUT_Q_BF_BOX and LAYOUT_Q_BF_WALK, `grid`
   for LAYOUT_Q_GRID_AXIS (core/layout/grid_track.c's sized axis, held in the job's answer storage),
   `flex_cross` for LAYOUT_Q_FLEX_CROSS (a content-box extent), `flex_lines` for LAYOUT_Q_FLEX_LINES
   (core/layout/flex_line.c's members and used main sizes, in the answer storage), and `flex_place` for
   LAYOUT_Q_FLEX_PLACE (core/layout/flex_position.c's placement of every element item, in the answer storage). */
struct GridAxisAnswer;
struct FlexLinesAnswer;
struct FlexPlaceAnswer;
typedef union {
    LayoutBfAnswer                bf;
    const struct GridAxisAnswer  *grid;
    CssPx                         flex_cross;
    const struct FlexLinesAnswer *flex_lines;
    const struct FlexPlaceAnswer *flex_place;
} LayoutAnswer;

/* ---- driven jobs ---------------------------------------------------------------------------------------- */

/* A job a step machine owns across its activations, in its state or for one of its activations. `free` asserts
   the job is not entered, no drive is open and no question is computing. */
LayoutJob *layout_job_new(void);
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

/* Whether a job's activation holds the thread (a job is entered), and how many driven jobs are alive. Read by
   solver/dom_cow.c's write entries, which assert against both; nothing branches on them. */
bool layout_job_holds_thread(void);
size_t layout_job_alive(void);

/* What the driven jobs alive hold: how many, and the C heap bytes (solver/c_alloc.h's chunk bytes) their own
   allocation and their activations left live. A job's activations allocate only for its stack, memo and records,
   up to what the member a machine computes allocates for its answer, so the sum is the jobs' share of `cLiveKiB`
   to within those answers. A gauge. */
typedef struct {
    size_t  jobs;
    int64_t bytes;
    long    sheds;        /* lifetime: the jobs layout_job_shed emptied */
    int64_t shed_bytes;   /* lifetime: the bytes those sheds gave back, measured */
} LayoutJobHeld;
void layout_job_held(LayoutJobHeld *out);

/* A driven job resting between its machine's activations, which RAM pressure may shed: the flow whose step
   entered it last (NULL before its first activation) and the bytes it holds. layout_job_resting answers every
   driven job that holds no unanswered decline and holds at least twice what it held when it was last shed, in a
   malloc'd array of `*n` the caller frees (NULL with `*n` 0 when there is none); asserts no job's activation
   holds the thread. A shed job re-derives what it held before it goes further, so the doubling keeps the bytes
   a job re-derives within the bytes of its walk's own progress, and a walk whose working set does not fit is
   sold rather than re-derived for ever. */
typedef struct {
    LayoutJob  *job;
    const void *flow;
    int64_t     bytes;
} LayoutJobResting;
LayoutJobResting *layout_job_resting(size_t *n);

/* Empties a resting job in place to what the clone answers (an empty stack, memo and records), the
   re-derivable state State isolation sheds: the job's machine re-asks the node its own cursor names at its next
   activation, as a forked arm does. Asserts the job rests (not entered, no drive open, every entry suspended,
   no grid row context) and holds no decline, whose arm is not re-derivable. Returns the C heap bytes the
   release gave back, measured. */
size_t layout_job_shed(LayoutJob *job);

/* The placement record (core/layout/flow_placement.h) of the job that holds it now: a driven job's while it
   is entered, and NULL otherwise. The record is the job's and is released with its memo. */
FlowPlacementRecord *layout_job_placement(void);
/* The same for css-cascade-5 §4.2's cascaded-value record (core/css/css_cascade_pass.h). */
CssCascadeRecord *layout_job_cascade(void);
/* The same for CSS 2.2 §9.4.2's line box record (core/layout/line_box.h). */
LineBoxRecord *layout_job_line_boxes(void);
/* The same for css-sizing-3 §5.1's intrinsic size record (core/layout/intrinsic_size.h). */
IntrinsicSizeRecord *layout_job_intrinsic_sizes(void);

/* Grid row contexts. While `container`'s rows are being sized (core/layout/grid_track.h), its items' grid
   areas have no block size yet: css-grid-1 §11 "Grid Layout Algorithm" makes an area definite for the layout
   that follows track sizing, and css-sizing-3 §5.2.1 "Intrinsic Contributions of Percentage-Sized Boxes"
   resolves a percentage cyclic through it specially in the item's contribution. A question asked inside the
   context carries it: its answer is memoized under it, and its body runs in it wherever a drive computes it,
   with the placement, line box and intrinsic size records that context owns. `enter` makes the current job's
   context `container`'s, inside the current one, and returns the current one, which `leave` restores; they
   pair in one frame. `sizing` answers whether `container`'s rows are being sized in the current context. */
unsigned layout_job_grid_rows_enter(const lxb_dom_element_t *container);
void layout_job_grid_rows_leave(const lxb_dom_element_t *container, unsigned outer);
bool layout_job_grid_rows_sizing(const lxb_dom_element_t *container);
/* NAMED RESIDUAL, the context key. Not covered: an answer is keyed by the whole chain of row contexts it was
   asked in, though it reads only the contexts whose grid areas its percentages consult. A context is one per
   (container, enclosing context), so nested grids, each sized inside every ancestor's row sizing and outside
   it, make up to 2^depth contexts per job, and a question about an inner grid is answered once in each. Next:
   record the contexts a body consults (`sizing`'s answers, served answers and record entries) and key both the
   memo and the three records under the chain's projection onto them: an answered question vouches for the
   record entries its body filled, so a memo shared across contexts over per-context records is read empty.
   How its absence shows: in nested grids, grid axis bodies per job growing geometrically with the nesting
   while the questions asked grow linearly. */

/* ---- the drive ------------------------------------------------------------------------------------------ */

/* A drive is a frame that cannot propagate "not yet" to its caller, so it computes every question deferred
   beneath it before it returns. `begin` takes the current job (the synchronous one when none is entered)
   and marks the stack; `next` hands back the topmost suspended question above the mark, now computing, or
   answers false when none is left; `end` asserts the stack is back at the mark. */
typedef struct LayoutJobDrive {
    LayoutJob  *job;
    size_t      base;
    unsigned    nest;
    bool        rests;      /* begun by `layout_job_drive_begin_resting` */
    const char *file;       /* the site that began it, and the job's innermost drive site it replaced */
    int         line;
    const char *outer_file;
    int         outer_line;
} LayoutJobDrive;

void layout_job_drive_begin_at(LayoutJobDrive *d, const char *file, int line);
#define layout_job_drive_begin(d) layout_job_drive_begin_at((d), __FILE__, __LINE__)
bool layout_job_drive_next(LayoutJobDrive *d, LayoutQuestion *q, size_t *at);
void layout_job_drive_end(LayoutJobDrive *d);

/* A drive whose driver rests (a step machine's, on its entered job). `begin_resting` asserts it is the job's
   outermost drive and takes every suspended entry the last activation left as its own; `spent` answers whether
   this activation has laid out its rest_unit_items(REST_UNIT_LAYOUT_BOXES) boxes, examined its
   rest_unit_items(REST_UNIT_LAYOUT_ELEMENT_SCAN) elements, taken its rest_unit_items(REST_UNIT_LAYOUT_LINE_ITEMS)
   line box items, derived its rest_unit_items(REST_UNIT_LAYOUT_LINE_METRICS) box metrics, tested its
   rest_unit_items(REST_UNIT_CASCADE_RULES) style rules or emitted its rest_unit_items(REST_UNIT_CASCADE_EMITS)
   rules; `end` on a resting drive
   leaves suspended entries in place for the next activation and asserts none is computing. */
void layout_job_drive_begin_resting(LayoutJobDrive *d);
bool layout_job_drive_spent(const LayoutJobDrive *d);
/* Whether a question is left suspended above the drive's mark: what an ask that answered "not yet" leaves
   when the drive's allowances are not what stopped it. */
bool layout_job_drive_pending(const LayoutJobDrive *d);

/* Whether an ask under the innermost open drive deferred a question: the top of the current job's stack, above
   that drive's mark, is suspended. A body's own entry computes until it returns, so this reads the asks made
   since. */
bool layout_job_deferred(void);

/* Whether a line box fill (core/layout/line_box.h) may take another step in this activation, and the items and
   box metrics a step took. Outside a resting drive, or under a drive nested inside a body, a fill runs to
   completion: `left` answers true and the spends count nothing. */
bool layout_job_line_items_left(void);
/* Whether a resting drive is innermost, the one condition under which the allowances above are spent. */
bool layout_job_rests_innermost(void);
void layout_job_line_items_spend(size_t n);
void layout_job_line_metrics_spend(size_t n);

/* ---- the stacked questions --------------------------------------------------------------------------------- */

/* Ask a BF_BOX, BF_WALK, GRID_AXIS, FLEX_CROSS, FLEX_LINES or FLEX_PLACE question. Asserts a drive is open: every ask has one beneath it on
   the C stack. True with `*out` written when the memo holds the answer; otherwise the question is pushed suspended
   and the caller returns "not yet". No body runs inside its asker's frame: the drive loop is the one caller of a
   body (`layout_job_drive_next`), so the C stack a body uses does not grow with the depth of the box tree, and a
   question is answered once per job however many readers ask it. A contribution's body takes one box; a stack counts
   its steps, and a grid axis or a flex container's cross size counts what its own asks take. A drive nested inside a
   resting job runs to completion in the activation, so the step machine owning the job could not rest there: taking
   a body there is a DFAIL naming the nested drive's site, whose caller is to become an ask propagated to the resting
   drive. */
bool layout_job_ask(LayoutQuestion q, LayoutAnswer *out);

/* The computing body at `at` completed: asserts it is the top, pops and memoizes it. */
void layout_job_answer(size_t at, const LayoutAnswer *answer);

/* `bytes` of storage for an answer's out-of-line parts, owned by the current job's memo and released when it
   is emptied, which no memoized answer outlives. */
void *layout_job_answer_storage(size_t bytes);

/* The computing body at `at` returned "not yet": asserts every entry above it is suspended and suspends it.
   A body suspends with a deferred question above it, or with nothing above it when its drive's allowance is
   spent (a stack's walk stages first, `layout_job_bf_step`). A stack always stages; every other body re-runs
   from its start, its finished asks served from the memo (`_layoutBfReplays`). */
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

/* A scan of core/layout/block_flow.h (CSS 2.2 §9.5's float search, §9.4.2's establishing condition) is about to
   examine one element. It answers false when a resting drive is innermost and has spent its items, and the scan
   then holds its place and answers "not yet"; otherwise it takes one element when a resting drive is
   innermost. */
bool layout_job_scan_take(void);

/* A read its caller states is served: everything it reads was answered by an ask `asker` made earlier in the
   job. `end` asserts the read laid out no box, took no line item and tested no style rule (the census kinds
   core/timing/thread_work.h charges for that work), naming `asker` and the read's site. Dev-only; the record is
   empty in release. */
typedef struct {
    const char *asker;
    unsigned long long box, line, rules;
} LayoutJobServed;
void layout_job_served_begin(LayoutJobServed *s, const char *asker);
void layout_job_served_end_at(const LayoutJobServed *s, const char *file, int line);
#define layout_job_served_end(s) layout_job_served_end_at((s), __FILE__, __LINE__)

/* A resting build of an author match (core/css/css_style_declaration.h's cssom_style_ask) is about to test one
   style rule. Asserts a resting drive is innermost; answers false when it has spent its items, and the build
   then parks its position and answers "not yet"; otherwise it takes one rule. */
bool layout_job_rule_take(void);

/* The same for one rule a resting emission of the author origin (core/css/css_author_sheets.h) is about to
   write: false when the drive has spent its items, and the walk then parks; otherwise it takes one. */
bool layout_job_emit_take(void);

/* Whether a resting build may run one more step of a style sheet's parse (core/css/css_sheet_parse.h), a rest
   unit of bytes whose work the drive's work allowance counts: false when any allowance is spent, and the
   build then parks; it takes no count of its own. */
bool layout_job_parse_take(void);

/* ---- forks over the cascade ---------------------------------------------------------------------------- */

/* Selectors 4 §17.3 answered UNKNOWN for a rule a resting author match tested (core/dom/selector_match.h),
   and the arm is kept by the step machine that owns the job, which is the resume point a fork needs. `decline`
   records the forkable predicate (its `over` borrowed from the attribute shadow, which the job's flow does not
   write while the job is alive) and stops the drive: every allowance reads spent until it is answered, and the
   match parks at the untested rule. Asserts a resting drive is innermost. */
void layout_job_decline(const SelectorUndet *u);
/* Whether `job` holds an unanswered decline. */
bool layout_job_declined(const LayoutJob *job);
/* The owning step machine's ask: with a decline held, forks the flow over the predicate (step_fork_run, two
   completions, "does not hold" first, its constraint key spelled by selector_undet_key into the job), and on
   the answered entry settles the arm (selector_pred_settle); the match reads it back from the flow's
   constraint when it runs again. Returns step_fork_run's nonzero code for the machine to return, or 0 when
   nothing was held or the arm is settled. Called with the job left, at the stage the decline was taken in, and
   first on every entry, since a held decline is consumed before the drive runs again. */
int layout_job_fork(JSContext *ctx, JSStepHdr *hdr, LayoutJob *job);

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
