/* What a layout job knows about a box's placement, held in storage the job owns — see
   core/layout/flow_placement.h for why the walk reports rather than the caller remembers, and
   core/layout/layout_job.h for what keeps a record valid. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_length.h"
#include "core/layout/flow_placement.h"
#include "core/layout/flow_position.h"   /* FlowPoint — the second fact a pass holds about a box */
#include "core/layout/layout_job.h"   /* the record of the job whose activation holds the thread */

/* ONE ENTRY PER BOX AND TWO FACTS IN IT, WHICH IS ONE TABLE AND NOT TWO. The two questions a whole-tree
   geometry walk asks about a box are keyed on the same element, go stale on the same events and live in the
   same span, so two tables would be two probe implementations, two growth policies and two key spaces over
   one key — and the origin's own check reads BOTH facts of a box in one lookup, which two tables would make
   two. `has_top` and `has_origin` are separate because the two are written by DIFFERENT components at
   DIFFERENT moments: core/layout/block_flow.c's walk reports a top for every box it places whether or not
   anybody asks its origin, and core/layout/flow_position.c records an origin for boxes §9.4.1 never stacks. */
typedef struct {
    CssPx                    top;
    FlowPoint                origin;
    /* THE CONTAINING BLOCK THE ORIGIN WAS DERIVED FROM, and NULL for every other arm — which is the whole of
       what the origin's check needs to know about how the value was produced. core/layout/flow_position.c's
       §10.1 SECOND case is the only arm with an equation over another box's recorded origin; the root arm,
       the out-of-flow arms and the §17.5 table arms each derive their point some other way, so a non-NULL
       here says "this point is that equation" and a NULL says "this component has no second route to it". */
    const lxb_dom_element_t *origin_cb;
    /* CSS 2.1 §10.1's WIDTH for this box, and the ANCESTOR the arm dispatch chose — NULL for §10.1's first
       case, whose rectangle is no element's box, which is the one thing that record's check needs to know
       about how the number was produced. See core/layout/flow_placement.h's §10.1 section. */
    CssPx                    cb_width;
    const lxb_dom_element_t *cb_width_cb;
    /* CSS 2.2 §9.2.1.1's anonymous block boxes of this box, appended by its stack; owned. `has_anon` is the
       stack having answered, which closes the list. */
    BlockFlowAnonBox        *anon;
    size_t                   anon_n, anon_cap;
    /* The stack's state at the run that starts this container's content (`run_first`, on the container's
       entry) and at the run after this box (`run_after`, on the box's entry). */
    FlowRunStart             run_first, run_after;
    /* The top of the line boxes of the run this box starts or follows (flow_placement.h). */
    CssPx                    run_top_first, run_top_after;
    /* CSS 2.1 §17.4's table box on this table element's wrapper stack. */
    CssPx                    table_box_top;
    /* CSS 2.2 §9.5's floats in the context this box establishes, in document order (owned), closed by
       `has_floats`, and the node that search examines next while it is stopped. */
    lxb_dom_element_t      **floats;
    size_t                   floats_n, floats_cap;
    lxb_dom_node_t          *float_scan;
    /* §9.5.1's margin box of this float, in its context root's content-box frame. */
    FlowFloatBox             float_box;
    /* Where this box's block formatting context puts it, and its top content edge there (flow_placement.h). */
    FlowBfcEntry             bfc_entry;
    CssPx                    bfc_top;
    /* §9.5's placement of a box beside the context's floats (flow_placement.h). */
    FlowFloatAvoid           float_avoid;
    bool                     has_top;
    bool                     has_origin;
    bool                     has_cb_width;
    bool                     has_anon;
    bool                     has_run_first, has_run_after, has_table_box, has_floats, has_float_scan;
    bool                     has_float_box, has_bfc_entry, has_bfc_frame, has_float_avoid;
    bool                     has_run_top_first, has_run_top_after;
} FpEntry;

/* One table slot: the key, and the entry held out of line. An entry carries several concolic lengths, each
   with its facts inline, so the table holds pointers: a probe touches only keys, and growing the table moves
   a pointer per box rather than every entry. `el` NULL is empty; nothing is removed, so there are no
   tombstones. The entry is owned by the record. */
typedef struct FpSlot {
    const lxb_dom_element_t *el;
    FpEntry                 *e;
} FpSlot;


static long long g_asks;
static long long g_served;
static long long g_walks;
static long long g_placements;
static long long g_origin_asks;
static long long g_origin_served;
static long long g_origin_derived;
static long long g_cb_width_asks;
static long long g_cb_width_served;
static long long g_cb_width_derived;
static long long g_anon_asks;
static long long g_anon_served;
static long long g_anon_derived;

/* THE ONE INITIAL CAPACITY, AND IT IS NOT A BOUND ON ANYTHING. §NO BOUNDS forbids deciding that work will not
   happen; this decides only how many placements fit before the table is rebuilt at twice the size, which is a
   question about when a `realloc` runs and never about which boxes get recorded. A document with more boxes
   than this gets a bigger table, without limit, exactly as it gets a bigger DOM. */
#define FP_CAP0 ((size_t)64)

/* A POINTER'S SLOT. The key is an ADDRESS, whose low bits are all alignment, so the mix is what stops an
   arena's evenly-spaced nodes from probing one cluster. This is splitmix64's finalizer, used for the property
   it is chosen for everywhere else: every input bit reaches every output bit. */
static size_t fp_slot(const lxb_dom_element_t *el, size_t cap)
{
    uint64_t h = (uint64_t)(uintptr_t)el;

    h ^= h >> 30; h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27; h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return (size_t)h & (cap - 1);
}

/* THE PROBE, SHARED BY THE READ AND THE WRITE so that a key can never be looked up one way and stored another.
   Answers the slot the key occupies, or the first empty slot at which it would be inserted. */
static size_t fp_probe(FpSlot *tab, size_t cap, const lxb_dom_element_t *el)
{
    size_t i = fp_slot(el, cap);

    while (tab[i].el != NULL && tab[i].el != el) i = (i + 1) & (cap - 1);
    return i;
}

/* TWO RECORDED EXAMPLES, COMPARED FOR AN ASSERTION AND FOR NOTHING ELSE. It is deliberately NOT an entry of
   core/css/css_length.h: a public equality over a `CssPx` is a predicate a caller can branch on, and
   §Solver-half forbids a C branch on a concolic's EXAMPLE because it deletes the arm the other world takes.
   Inside a `DCHECK` there is no arm to delete — the condition is side-effect-free and the only thing it can do
   is abort a DEV build — which is why this lives here, private, and reads only the example.
   NaN IS EQUAL TO ITSELF HERE, which ordinary `==` is not: a position that came out NaN twice is two runs of
   one walk agreeing, and reporting that as a disagreement would fire this assert on the one input for which
   both sides are in fact identical. */
static bool fp_same_example(CssPx a, CssPx b)
{
    return a.px == b.px || (a.px != a.px && b.px != b.px);
}

static void fp_grow(FlowPlacementRecord *r)
{
    size_t cap = r->cap ? r->cap * 2 : FP_CAP0, i;
    FpSlot *tab = calloc(cap, sizeof *tab);

    CHECK(tab != NULL, "flow-placement-oom: CSS 2.1 §9.4.1's placement record could not be grown. A dropped "
                       "position is not a slower answer, it is a box placed against a stack this pass has "
                       "already reported differently");
    for (i = 0; i < r->cap; i++)
        if (r->tab[i].el != NULL) tab[fp_probe(tab, cap, r->tab[i].el)] = r->tab[i];
    free(r->tab);
    r->tab = tab;
    r->cap = cap;
}

/* The entry `el` writes to, inserted when absent; NULL when no record is held. */
static FpEntry *fp_entry_for_write(const lxb_dom_element_t *el)
{
    FlowPlacementRecord *r = layout_job_placement();
    size_t i;

    if (r == NULL) return NULL;
    if (r->used * 2 >= r->cap) fp_grow(r);
    i = fp_probe(r->tab, r->cap, el);
    if (r->tab[i].el == NULL) {
        FpEntry *e = calloc(1, sizeof *e);

        CHECK(e != NULL, "flow-placement-oom: one box's placement entry could not be allocated");
        r->tab[i].el = el;
        r->tab[i].e = e;
        r->used++;
    }
    return r->tab[i].e;
}

/* The entry `el` holds, or NULL. */
static const FpEntry *fp_entry_peek(const lxb_dom_element_t *el)
{
    FlowPlacementRecord *r = layout_job_placement();
    size_t i;

    if (r == NULL || r->cap == 0) return NULL;
    i = fp_probe(r->tab, r->cap, el);
    return r->tab[i].el == NULL ? NULL : r->tab[i].e;
}

void flow_placement_record_release(FlowPlacementRecord *r)
{
    size_t i;

    for (i = 0; i < r->cap; i++) {
        if (r->tab[i].el == NULL) continue;
        free(r->tab[i].e->anon);
        free(r->tab[i].e->floats);
        free(r->tab[i].e);
    }
    free(r->tab);
    r->tab = NULL;
    r->cap = 0;
    r->used = 0;
}

bool flow_placement_is_held(void) { return layout_job_placement() != NULL; }

void flow_placement_record(const lxb_dom_element_t *el, CssPx top)
{
    FpEntry *e;

    DCHECK(el != NULL, "CSS 2.1 §9.4.1's placement record was handed a position with no box to attach it to. "
                       "§9.2.1.1's anonymous block box is the one thing on that stack with no element, and "
                       "the walk skips it here rather than passing a null: an anonymous box is never asked "
                       "for by element, so a record entry for one could only ever be dead weight");
    e = fp_entry_for_write(el);
    if (e == NULL) return;
    /* The same box placed twice in one job is ordinary and must agree: a stack staged mid-step writes that
       step again, and the stack reads computed values and the child list and nothing else. A box may already
       hold an origin and no top: one key, facts written by two components in either order. */
    DCHECKF(!e->has_top || fp_same_example(e->top, top),
            "CSS 2.1 §9.4.1's walk placed one box at two different tops inside ONE pass: %g then %g. The "
            "walk reads computed values and the child list and nothing else, so two runs of it over one "
            "document answer the same number — which makes this either a document that changed under the "
            "pass (core/layout/flow_placement.h names the three places that is asserted, and one of them "
            "has let something through) or a walk whose `pass` argument moves a position",
            e->top.px, top.px);
    g_placements++;
    e->top = top;
    e->has_top = true;
}

/* THE LOOKUP, SHARED BY EVERY READER AND COUNTING NOTHING — see the header for why the census belongs to the
   ASK rather than to the lookup under it. Also where the pass's tree number is checked on the read side, so
   the FIRST served answer after a change is what crashes rather than a picture that is already drawn. */
bool flow_placement_peek(const lxb_dom_element_t *el, CssPx *out)
{
    const FpEntry *e;

    DCHECK(el != NULL, "CSS 2.1 §9.4.1's placement record was asked about no box");
    DCHECK(out != NULL, "CSS 2.1 §9.4.1's placement record was asked with nowhere to put the answer");
    e = fp_entry_peek(el);
    if (e == NULL || !e->has_top) return false;
    *out = e->top;
    return true;
}

bool flow_placement_origin_peek(const lxb_dom_element_t *el, FlowPoint *out, const lxb_dom_element_t **cb_out)
{
    const FpEntry *e;

    DCHECK(el != NULL, "CSS 2 §8.1 \"Box dimensions\"' border-box origin record was asked about no box");
    DCHECK(out != NULL, "CSS 2 §8.1 \"Box dimensions\"' border-box origin record was asked with nowhere "
                        "to put the answer");
    e = fp_entry_peek(el);
    if (e == NULL || !e->has_origin) return false;
    *out = e->origin;
    if (cb_out != NULL) *cb_out = e->origin_cb;
    return true;
}

void flow_placement_origin_record(const lxb_dom_element_t *el, FlowPoint origin,
                                  const lxb_dom_element_t *derived_from)
{
    FpEntry *e;

    DCHECK(el != NULL, "CSS 2 §8.1 \"Box dimensions\"' border-box origin record was handed a point with "
                       "no box to attach it to");
    /* The count is taken before the guard: a derivation made with no record held still happened, and
       `origin_derived` counts derivations, not entries. Counting here, once per derivation, is what keeps a
       caller from recording without counting. */
    g_origin_derived++;
    e = fp_entry_for_write(el);
    if (e == NULL) return;
    /* One box's origin computed twice in one pass is unreachable: core/layout/flow_position.c asks this record
       before it computes, so a second computation is a read that missed where a write had landed. The numbers
       are compared rather than the state, because a disagreement is the finding. */
    DCHECKF(!e->has_origin || (fp_same_example(e->origin.x, origin.x) && fp_same_example(e->origin.y, origin.y)),
            "CSS 2 §8.1 \"Box dimensions\"' border-box origin was computed twice in one pass for one box and "
            "the two disagree: (%g, %g) then (%g, %g). core/layout/flow_position.c reads this record before it "
            "computes, so a second computation at all means a read missed a key a write had landed — and "
            "two different answers mean the document moved under the pass on an axis its tree version "
            "cannot see",
            e->origin.x.px, e->origin.y.px, origin.x.px, origin.y.px);
    e->origin = origin;
    e->origin_cb = derived_from;
    e->has_origin = true;
}

bool flow_placement_origin_ask(const lxb_dom_element_t *el, FlowPoint *out, const lxb_dom_element_t **cb_out)
{
    g_origin_asks++;
    if (!flow_placement_origin_peek(el, out, cb_out)) return false;
    g_origin_served++;
    return true;
}

bool flow_placement_cb_width_peek(const lxb_dom_element_t *el, CssPx *out, const lxb_dom_element_t **cb_out)
{
    const FpEntry *e;

    DCHECK(el != NULL, "CSS 2.1 §10.1 \"Definition of 'containing block'\"' width record was asked about no "
                       "box");
    DCHECK(out != NULL, "CSS 2.1 §10.1's width record was asked with nowhere to put the answer");
    e = fp_entry_peek(el);
    if (e == NULL || !e->has_cb_width) return false;
    *out = e->cb_width;
    if (cb_out != NULL) *cb_out = e->cb_width_cb;
    return true;
}

bool flow_placement_cb_width_ask(const lxb_dom_element_t *el, CssPx *out, const lxb_dom_element_t **cb_out)
{
    g_cb_width_asks++;
    if (!flow_placement_cb_width_peek(el, out, cb_out)) return false;
    g_cb_width_served++;
    return true;
}

void flow_placement_cb_width_record(const lxb_dom_element_t *el, CssPx width, const lxb_dom_element_t *cb)
{
    FpEntry *e;

    DCHECK(el != NULL, "CSS 2.1 §10.1's width record was handed a width with no box to attach it to");
    /* The count before the guard, for the origin record's reason. */
    g_cb_width_derived++;
    e = fp_entry_for_write(el);
    if (e == NULL) return;
    /* Derived twice in one pass is unreachable for the origin's reason: core/layout/used_value.c asks this
       record before it derives. Two different answers mean the cascade moved inside the pass. */
    DCHECKF(!e->has_cb_width || fp_same_example(e->cb_width, width),
            "CSS 2.1 §10.1's containing-block width was derived twice in one pass for one box and the two "
            "disagree: %g then %g. core/layout/used_value.c reads this record before it derives, so a "
            "second derivation means a read missed a key a write had landed — and two different answers "
            "mean the document moved under the pass on an axis its tree version cannot see",
            e->cb_width.px, width.px);
    e->cb_width = width;
    e->cb_width_cb = cb;
    e->has_cb_width = true;
}

bool flow_placement_anon_peek(const lxb_dom_element_t *el, const BlockFlowAnonBox **v, size_t *n)
{
    const FpEntry *e;

    DCHECK(el != NULL && v != NULL && n != NULL,
           "CSS 2.2 §9.2.1.1's anonymous-box record was asked about no box or with nowhere to answer");
    e = fp_entry_peek(el);
    if (e == NULL || !e->has_anon) return false;
    *v = e->anon;
    *n = e->anon_n;
    return true;
}

bool flow_placement_anon_ask(const lxb_dom_element_t *el, const BlockFlowAnonBox **v, size_t *n)
{
    g_anon_asks++;
    if (!flow_placement_anon_peek(el, v, n)) return false;
    g_anon_served++;
    return true;
}

void flow_placement_anon_derived(void) { g_anon_derived++; }

void flow_placement_anon_append(const lxb_dom_element_t *el, BlockFlowAnonBox box)
{
    FpEntry *e;

    DCHECK(el != NULL, "CSS 2.2 §9.2.1.1's anonymous-box record was handed a box with no container");
    e = fp_entry_for_write(el);
    if (e == NULL) return;
    DCHECK(!e->has_anon,
           "an anonymous block box was appended to a container whose stack already answered; the stack is "
           "asked once per job, so a second run of it is a memo probe that missed its own key");
    if (e->anon_n == e->anon_cap) {
        size_t cap = e->anon_cap != 0 ? e->anon_cap * 2 : 4;
        BlockFlowAnonBox *v = realloc(e->anon, cap * sizeof *v);

        CHECK(v != NULL, "out of memory recording one container's anonymous block boxes");
        e->anon = v;
        e->anon_cap = cap;
    }
    e->anon[e->anon_n++] = box;
}

void flow_placement_anon_close(const lxb_dom_element_t *el)
{
    FpEntry *e;

    DCHECK(el != NULL, "CSS 2.2 §9.2.1.1's anonymous-box record was closed for no container");
    e = fp_entry_for_write(el);
    if (e == NULL) return;
    DCHECK(!e->has_anon, "a container's stack answered twice in one job; it is asked once and memoized");
    e->has_anon = true;
}

static bool fp_same_run(FlowRunStart a, FlowRunStart b)
{
    return fp_same_example(a.pos, b.pos) && fp_same_example(a.run_pos, b.run_pos) &&
           fp_same_example(a.run_neg, b.run_neg) && a.escaping == b.escaping;
}

void flow_placement_run_record(const lxb_dom_element_t *container, const lxb_dom_element_t *after,
                               FlowRunStart s)
{
    FpEntry *e;
    FlowRunStart *slot;
    bool *has;

    DCHECK(container != NULL, "a run start was recorded for no container");
    e = fp_entry_for_write(after != NULL ? after : container);
    if (e == NULL) return;
    slot = after != NULL ? &e->run_after : &e->run_first;
    has = after != NULL ? &e->has_run_after : &e->has_run_first;
    DCHECKF(!*has || fp_same_run(*slot, s),
            "one run of CSS 2.2 §9.2.1.1 started at two different stack states in one job (position %g then %g); "
            "the stack reads only the tree and the cascade, so a resumed step writes the state it wrote before",
            slot->pos.px, s.pos.px);
    *slot = s;
    *has = true;
}

bool flow_placement_run_peek(const lxb_dom_element_t *container, const lxb_dom_element_t *after,
                             FlowRunStart *out)
{
    const FpEntry *e;

    DCHECK(container != NULL && out != NULL, "a run start was asked for no container or with nowhere to put it");
    e = fp_entry_peek(after != NULL ? after : container);
    if (e == NULL || !(after != NULL ? e->has_run_after : e->has_run_first)) return false;
    *out = after != NULL ? e->run_after : e->run_first;
    return true;
}

void flow_placement_run_top_record(const lxb_dom_element_t *container, const lxb_dom_element_t *after, CssPx top)
{
    FpEntry *e;
    CssPx *slot;
    bool *has;

    DCHECK(container != NULL, "a run's line box top was recorded for no container");
    e = fp_entry_for_write(after != NULL ? after : container);
    if (e == NULL) return;
    slot = after != NULL ? &e->run_top_after : &e->run_top_first;
    has = after != NULL ? &e->has_run_top_after : &e->has_run_top_first;
    DCHECKF(!*has || fp_same_example(*slot, top),
            "one run's line boxes were put at two tops in one job: %g then %g", slot->px, top.px);
    *slot = top;
    *has = true;
}

bool flow_placement_run_top_peek(const lxb_dom_element_t *container, const lxb_dom_element_t *after, CssPx *out)
{
    const FpEntry *e;

    DCHECK(container != NULL && out != NULL, "a run's line box top was asked for no container or nowhere");
    e = fp_entry_peek(after != NULL ? after : container);
    if (e == NULL || !(after != NULL ? e->has_run_top_after : e->has_run_top_first)) return false;
    *out = after != NULL ? e->run_top_after : e->run_top_first;
    return true;
}

void flow_placement_table_box_record(const lxb_dom_element_t *table, CssPx top)
{
    FpEntry *e;

    DCHECK(table != NULL, "a table box position was recorded for no table");
    e = fp_entry_for_write(table);
    if (e == NULL) return;
    DCHECKF(!e->has_table_box || fp_same_example(e->table_box_top, top),
            "a table box was placed at two tops on its wrapper's stack in one job: %g then %g",
            e->table_box_top.px, top.px);
    e->table_box_top = top;
    e->has_table_box = true;
}

void flow_placement_floats_append(const lxb_dom_element_t *root, lxb_dom_element_t *f)
{
    FpEntry *e;

    DCHECK(root != NULL && f != NULL, "a context's float was recorded for no context root or no float");
    e = fp_entry_for_write(root);
    if (e == NULL) return;
    DCHECK(!e->has_floats, "a float was appended to a context whose list was already closed");
    if (e->floats_n == e->floats_cap) {
        size_t cap = e->floats_cap != 0 ? e->floats_cap * 2 : 4;
        lxb_dom_element_t **v = realloc(e->floats, cap * sizeof *v);

        CHECK(v != NULL, "out of memory recording one block formatting context's floats");
        e->floats = v;
        e->floats_cap = cap;
    }
    e->floats[e->floats_n++] = f;
}

void flow_placement_floats_close(const lxb_dom_element_t *root)
{
    FpEntry *e;

    DCHECK(root != NULL, "a context's float list was closed for no context root");
    e = fp_entry_for_write(root);
    if (e == NULL) return;
    e->has_floats = true;
}

bool flow_placement_floats_peek(const lxb_dom_element_t *root, lxb_dom_element_t *const **v, size_t *n)
{
    const FpEntry *e;

    DCHECK(root != NULL && v != NULL && n != NULL, "a context's floats were asked for no root or nowhere");
    e = fp_entry_peek(root);
    if (e == NULL || !e->has_floats) return false;
    *v = e->floats;
    *n = e->floats_n;
    return true;
}

void flow_placement_float_box_record(const lxb_dom_element_t *f, FlowFloatBox b)
{
    FpEntry *e;

    DCHECK(f != NULL, "a float's placement was recorded for no float");
    e = fp_entry_for_write(f);
    if (e == NULL) return;
    DCHECK(!e->has_float_box || (fp_same_example(e->float_box.left, b.left) &&
                                 fp_same_example(e->float_box.top, b.top)),
           "one float was placed at two positions in one job; §9.5.1 reads only earlier boxes");
    e->float_box = b;
    e->has_float_box = true;
}

bool flow_placement_float_box_peek(const lxb_dom_element_t *f, FlowFloatBox *out)
{
    const FpEntry *e;

    DCHECK(f != NULL && out != NULL, "a float's placement was asked for no float or with nowhere to put it");
    e = fp_entry_peek(f);
    if (e == NULL || !e->has_float_box) return false;
    *out = e->float_box;
    return true;
}

void flow_placement_bfc_entry_record(const lxb_dom_element_t *el, FlowBfcEntry b)
{
    FpEntry *e;

    DCHECK(el != NULL, "a box's place in its formatting context was recorded for no box");
    e = fp_entry_for_write(el);
    if (e == NULL) return;
    DCHECKF(!e->has_bfc_entry || (fp_same_example(e->bfc_entry.base, b.base) &&
                                  fp_same_example(e->bfc_entry.run_pos, b.run_pos) &&
                                  fp_same_example(e->bfc_entry.run_neg, b.run_neg)),
            "one box was put at two places in its formatting context in one job: base %g then %g",
            e->bfc_entry.base.px, b.base.px);
    e->bfc_entry = b;
    e->has_bfc_entry = true;
}

bool flow_placement_bfc_entry_peek(const lxb_dom_element_t *el, FlowBfcEntry *out)
{
    const FpEntry *e;

    DCHECK(el != NULL && out != NULL, "a box's place in its formatting context was asked for no box or nowhere");
    e = fp_entry_peek(el);
    if (e == NULL || !e->has_bfc_entry) return false;
    *out = e->bfc_entry;
    return true;
}

void flow_placement_bfc_frame_record(const lxb_dom_element_t *el, CssPx top)
{
    FpEntry *e;

    DCHECK(el != NULL, "a box's content edge in its formatting context was recorded for no box");
    e = fp_entry_for_write(el);
    if (e == NULL) return;
    DCHECKF(!e->has_bfc_frame || fp_same_example(e->bfc_top, top),
            "one box's top content edge was put at two places in its formatting context in one job: %g then %g",
            e->bfc_top.px, top.px);
    e->bfc_top = top;
    e->has_bfc_frame = true;
}

bool flow_placement_bfc_frame_peek(const lxb_dom_element_t *el, CssPx *top)
{
    const FpEntry *e;

    DCHECK(el != NULL && top != NULL, "a box's content edge in its formatting context was asked for no box or nowhere");
    e = fp_entry_peek(el);
    if (e == NULL || !e->has_bfc_frame) return false;
    *top = e->bfc_top;
    return true;
}

void flow_placement_float_avoid_record(const lxb_dom_element_t *el, FlowFloatAvoid a)
{
    FpEntry *e;

    DCHECK(el != NULL, "a box's placement beside floats was recorded for no box");
    e = fp_entry_for_write(el);
    if (e == NULL) return;
    DCHECKF(!e->has_float_avoid || (fp_same_example(e->float_avoid.left, a.left) &&
                                    fp_same_example(e->float_avoid.width, a.width)),
            "one box was placed beside floats at two places in one job: left %g then %g", e->float_avoid.left.px,
            a.left.px);
    e->float_avoid = a;
    e->has_float_avoid = true;
}

bool flow_placement_float_avoid_peek(const lxb_dom_element_t *el, FlowFloatAvoid *out)
{
    const FpEntry *e;

    DCHECK(el != NULL && out != NULL, "a box's placement beside floats was asked for no box or nowhere");
    e = fp_entry_peek(el);
    if (e == NULL || !e->has_float_avoid) return false;
    *out = e->float_avoid;
    return true;
}

void flow_placement_float_scan_hold(const lxb_dom_element_t *root, lxb_dom_node_t *next)
{
    FpEntry *e;

    DCHECK(root != NULL && next != NULL, "a float search's place was held for no context or no node");
    e = fp_entry_for_write(root);
    CHECK(e != NULL, "a float search stopped with no placement record to hold its place; it stops only under a "
                     "resting drive, whose job holds one");
    DCHECK(!e->has_floats, "a float search held its place after its context's answer was recorded");
    e->float_scan = next;
    e->has_float_scan = true;
}

bool flow_placement_float_scan_peek(const lxb_dom_element_t *root, lxb_dom_node_t **out)
{
    const FpEntry *e;

    DCHECK(root != NULL && out != NULL, "a float search's place was asked for no context or nowhere to put it");
    e = fp_entry_peek(root);
    if (e == NULL || !e->has_float_scan) return false;
    *out = e->float_scan;
    return true;
}

bool flow_placement_table_box_peek(const lxb_dom_element_t *table, CssPx *out)
{
    const FpEntry *e;

    DCHECK(table != NULL && out != NULL, "a table box position was asked for no table or with nowhere to put it");
    e = fp_entry_peek(table);
    if (e == NULL || !e->has_table_box) return false;
    *out = e->table_box_top;
    return true;
}

bool flow_placement_ask(const lxb_dom_element_t *el, CssPx *out)
{
    DCHECK(el != NULL, "CSS 2.1 §9.4.1's placement was asked for with no box");
    DCHECK(out != NULL, "CSS 2.1 §9.4.1's placement was asked for with nowhere to put the answer");
    g_asks++;
    if (!flow_placement_peek(el, out)) return false;
    g_served++;
    return true;
}

void flow_placement_walked(void) { g_walks++; }

void flow_placement_census(FlowPlacementCensus *out)
{
    DCHECK(out != NULL, "the placement census was asked for with nowhere to write it");
    /* THE TWO OUTCOMES WERE COUNTED AT THE SAME EVENT, ASSERTED RATHER THAN LEFT TO A READER. An ask is either
       answered out of the record or pays for §9.4.1's walk, and there is no third arm — so a total that does
       not close is one of the two having been counted somewhere the other is not, which is the defect that
       makes a ratio of them mean nothing while looking exactly like a measurement. */
    DCHECKF(g_asks == g_served + g_walks,
            "CSS 2.1 §9.4.1's placement census does not close: %lld asks against %lld served and %lld walks. "
            "The caller that asks is the caller that reports the walk, immediately, on the one arm where the "
            "record did not answer — so a gap is a return between the two, and the ratio a reader takes off "
            "these rows would be a share of a denominator that is not the population",
            g_asks, g_served, g_walks);
    /* AND THE ORIGIN PAIR, WHOSE SHORTFALL IS A DERIVATION AND NOT A WALK, which is why it is named `derived`
       and not `walks`: a missed origin ask runs CSS 2 §10.1's own step for that box — one equation over its
       containing block's recorded point — and the RECURSION it used to pay is what the record removes. The
       two close the same way the pair above does and for the same reason. */
    DCHECKF(g_origin_asks == g_origin_served + g_origin_derived,
            "CSS 2 §8.1 \"Box dimensions\"' border-box origin census does not close: %lld asks against %lld "
            "served and %lld "
            "derived. Every ask is answered out of this record or derives the point and records it, with no "
            "third arm — so a gap is a return between the ask and the record, and the share a reader takes "
            "off these rows would be over a denominator that is not the population",
            g_origin_asks, g_origin_served, g_origin_derived);
    out->asks = g_asks;
    out->served = g_served;
    out->walks = g_walks;
    out->placements = g_placements;
    out->origin_asks = g_origin_asks;
    out->origin_served = g_origin_served;
    out->origin_derived = g_origin_derived;
    /* AND CSS 2.1 §10.1's WIDTH PAIR, WHOSE SHORTFALL IS A DERIVATION FOR THE ORIGIN'S REASON — a missed ask
       runs §10.1's own arm over an ancestor this pass has usually already answered, never a walk. */
    DCHECKF(g_cb_width_asks == g_cb_width_served + g_cb_width_derived,
            "CSS 2.1 §10.1 \"Definition of 'containing block'\"' width census does not close: %lld asks "
            "against %lld served and %lld derived. Every ask is answered out of this record or derives the "
            "width and records it, with no third arm — so a gap is a return between the ask and the record, "
            "and the share a reader takes off these rows would be over a denominator that is not the "
            "population",
            g_cb_width_asks, g_cb_width_served, g_cb_width_derived);
    out->cb_width_asks = g_cb_width_asks;
    out->cb_width_served = g_cb_width_served;
    out->cb_width_derived = g_cb_width_derived;
    DCHECKF(g_anon_asks == g_anon_served + g_anon_derived,
            "CSS 2.2 §9.2.1.1's anonymous-box census does not close: %lld asks against %lld served and %lld "
            "derived; every answered ask is served from the record or derives and records",
            g_anon_asks, g_anon_served, g_anon_derived);
}
