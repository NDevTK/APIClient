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
typedef struct FpEntry {
    const lxb_dom_element_t *el;   /* NULL is EMPTY; nothing is ever removed, so there are no tombstones */
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
    /* CSS 2.1 §17.4's table box on this table element's wrapper stack. */
    CssPx                    table_box_top;
    /* CSS 2.2 §9.5's first float in the context this box establishes, NULL for none, and the node that
       search examines next while it is stopped. */
    lxb_dom_element_t       *first_float;
    lxb_dom_node_t          *float_scan;
    bool                     has_top;
    bool                     has_origin;
    bool                     has_cb_width;
    bool                     has_anon;
    bool                     has_run_first, has_run_after, has_table_box, has_first_float, has_float_scan;
} FpEntry;


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
static size_t fp_probe(FpEntry *tab, size_t cap, const lxb_dom_element_t *el)
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
    FpEntry *tab = calloc(cap, sizeof *tab);

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
        r->tab[i].el = el;
        r->used++;
    }
    return &r->tab[i];
}

/* The entry `el` holds, or NULL. */
static const FpEntry *fp_entry_peek(const lxb_dom_element_t *el)
{
    FlowPlacementRecord *r = layout_job_placement();
    size_t i;

    if (r == NULL || r->cap == 0) return NULL;
    i = fp_probe(r->tab, r->cap, el);
    return r->tab[i].el == NULL ? NULL : &r->tab[i];
}

void flow_placement_record_release(FlowPlacementRecord *r)
{
    size_t i;

    for (i = 0; i < r->cap; i++)
        if (r->tab[i].el != NULL) free(r->tab[i].anon);
    free(r->tab);
    r->tab = NULL;
    r->cap = 0;
    r->used = 0;
}

bool flow_placement_is_held(void) { return layout_job_placement() != NULL; }

void flow_placement_record(const lxb_dom_element_t *el, CssPx top)
{
    size_t i;
    FlowPlacementRecord *r = layout_job_placement();
    FpEntry *tab;

    DCHECK(el != NULL, "CSS 2.1 §9.4.1's placement record was handed a position with no box to attach it to. "
                       "§9.2.1.1's anonymous block box is the one thing on that stack with no element, and "
                       "the walk skips it here rather than passing a null: an anonymous box is never asked "
                       "for by element, so a record entry for one could only ever be dead weight");
    if (r == NULL) return;
    if (r->used * 2 >= r->cap) fp_grow(r);
    tab = r->tab;
    i = fp_probe(tab, r->cap, el);
    if (tab[i].el == NULL) {
        tab[i].el = el;
        r->used++;
        g_placements++;
    } else if (!tab[i].has_top) {
        /* The box already has an ORIGIN entry and no top yet — one key, two facts, written by two components
           in whichever order the walk reached them. */
        g_placements++;
    } else {
        /* THE SAME BOX PLACED TWICE IN ONE PASS, WHICH IS ORDINARY AND MUST AGREE. §9.4.1's walk over one
           container runs whenever anything asks that container a question — a child's position, its own
           `height: auto`, a baseline — so one pass legitimately walks a container several times and reports
           each child's top each time. The walk is a function of the document, and the document is what this
           span is asserted not to change, so the second report is the first number: a disagreement is either
           that assertion having failed silently or the walk having a mode that moves a box, and both are
           worth more than the position. */
        DCHECKF(fp_same_example(tab[i].top, top),
                "CSS 2.1 §9.4.1's walk placed one box at two different tops inside ONE pass: %g then %g. The "
                "walk reads computed values and the child list and nothing else, so two runs of it over one "
                "document answer the same number — which makes this either a document that changed under the "
                "pass (core/layout/flow_placement.h names the three places that is asserted, and one of them "
                "has let something through) or a walk whose `pass` or static-position argument moves a "
                "position, which core/layout/block_flow.c states it does not",
                tab[i].top.px, top.px);
        g_placements++;
    }
    tab[i].top = top;
    tab[i].has_top = true;
}

/* THE LOOKUP, SHARED BY EVERY READER AND COUNTING NOTHING — see the header for why the census belongs to the
   ASK rather than to the lookup under it. Also where the pass's tree number is checked on the read side, so
   the FIRST served answer after a change is what crashes rather than a picture that is already drawn. */
bool flow_placement_peek(const lxb_dom_element_t *el, CssPx *out)
{
    size_t i;
    FlowPlacementRecord *r = layout_job_placement();
    FpEntry *tab;

    DCHECK(el != NULL, "CSS 2.1 §9.4.1's placement record was asked about no box");
    DCHECK(out != NULL, "CSS 2.1 §9.4.1's placement record was asked with nowhere to put the answer");
    if (r == NULL || r->cap == 0) return false;
    tab = r->tab;
    i = fp_probe(tab, r->cap, el);
    if (tab[i].el == NULL || !tab[i].has_top) return false;
    *out = tab[i].top;
    return true;
}

bool flow_placement_origin_peek(const lxb_dom_element_t *el, FlowPoint *out, const lxb_dom_element_t **cb_out)
{
    size_t i;
    FlowPlacementRecord *r = layout_job_placement();
    FpEntry *tab;

    DCHECK(el != NULL, "CSS 2 §8.1 \"Box dimensions\"' border-box origin record was asked about no box");
    DCHECK(out != NULL, "CSS 2 §8.1 \"Box dimensions\"' border-box origin record was asked with nowhere "
                        "to put the answer");
    if (r == NULL || r->cap == 0) return false;
    tab = r->tab;
    i = fp_probe(tab, r->cap, el);
    if (tab[i].el == NULL || !tab[i].has_origin) return false;
    *out = tab[i].origin;
    if (cb_out != NULL) *cb_out = tab[i].origin_cb;
    return true;
}

void flow_placement_origin_record(const lxb_dom_element_t *el, FlowPoint origin,
                                  const lxb_dom_element_t *derived_from)
{
    size_t i;
    FlowPlacementRecord *r = layout_job_placement();
    FpEntry *tab;

    DCHECK(el != NULL, "CSS 2 §8.1 \"Box dimensions\"' border-box origin record was handed a point with "
                       "no box to attach it to");
    /* The count is taken before the guard: a derivation made with no record held still happened, and
       `origin_derived` counts derivations, not entries. Counting here, once per derivation, is what keeps a
       caller from recording without counting. */
    g_origin_derived++;
    if (r == NULL) return;
    if (r->used * 2 >= r->cap) fp_grow(r);
    tab = r->tab;
    i = fp_probe(tab, r->cap, el);
    if (tab[i].el == NULL) {
        tab[i].el = el;
        r->used++;
    } else {
        /* ONE BOX'S ORIGIN COMPUTED TWICE IN ONE PASS, WHICH THE MEMO MAKES UNREACHABLE AND WHICH IS THEREFORE
           WORTH ASSERTING RATHER THAN ALLOWING. core/layout/flow_position.c asks this record before it
           computes and records every point it computes, so a second computation means a read that missed
           where a write had already landed — a probe that answers two slots for one key. The numbers are
           compared rather than the state, because a disagreement is the finding and an agreement under a
           broken probe would be a coincidence worth knowing about too. */
        DCHECKF(!tab[i].has_origin ||
                    (fp_same_example(tab[i].origin.x, origin.x) && fp_same_example(tab[i].origin.y, origin.y)),
                "CSS 2 §8.1 \"Box dimensions\"' border-box origin was computed twice in one pass for one box and "
                "the two "
                "disagree: (%g, %g) then (%g, %g). core/layout/flow_position.c reads this record before it "
                "computes, so a second computation at all means a read missed a key a write had landed — and "
                "two different answers mean the document moved under the pass on an axis its tree version "
                "cannot see",
                tab[i].origin.x.px, tab[i].origin.y.px, origin.x.px, origin.y.px);
    }
    tab[i].origin = origin;
    tab[i].origin_cb = derived_from;
    tab[i].has_origin = true;
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
    size_t i;
    FlowPlacementRecord *r = layout_job_placement();
    FpEntry *tab;

    DCHECK(el != NULL, "CSS 2.1 §10.1 \"Definition of 'containing block'\"' width record was asked about no "
                       "box");
    DCHECK(out != NULL, "CSS 2.1 §10.1's width record was asked with nowhere to put the answer");
    if (r == NULL || r->cap == 0) return false;
    tab = r->tab;
    i = fp_probe(tab, r->cap, el);
    if (tab[i].el == NULL || !tab[i].has_cb_width) return false;
    *out = tab[i].cb_width;
    if (cb_out != NULL) *cb_out = tab[i].cb_width_cb;
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
    size_t i;
    FlowPlacementRecord *r = layout_job_placement();
    FpEntry *tab;

    DCHECK(el != NULL, "CSS 2.1 §10.1's width record was handed a width with no box to attach it to");
    /* The count before the guard, for the origin record's reason. */
    g_cb_width_derived++;
    if (r == NULL) return;
    if (r->used * 2 >= r->cap) fp_grow(r);
    tab = r->tab;
    i = fp_probe(tab, r->cap, el);
    if (tab[i].el == NULL) {
        tab[i].el = el;
        r->used++;
    } else {
        /* ONE BOX'S CONTAINING-BLOCK WIDTH DERIVED TWICE IN ONE PASS, WHICH THE ASK MAKES UNREACHABLE AND
           WHICH IS THEREFORE ASSERTED RATHER THAN ALLOWED — the same guard the origin above carries and for
           the same reason: core/layout/used_value.c asks this record before it derives and records every
           width it derives, so a second derivation at all means a read missed a key a write had landed,
           which is this file's probe and nothing else. Two DIFFERENT answers mean the cascade moved inside a
           span the pass's tree version cannot see move. */
        DCHECKF(!tab[i].has_cb_width || fp_same_example(tab[i].cb_width, width),
                "CSS 2.1 §10.1's containing-block width was derived twice in one pass for one box and the two "
                "disagree: %g then %g. core/layout/used_value.c reads this record before it derives, so a "
                "second derivation means a read missed a key a write had landed — and two different answers "
                "mean the document moved under the pass on an axis its tree version cannot see",
                tab[i].cb_width.px, width.px);
    }
    tab[i].cb_width = width;
    tab[i].cb_width_cb = cb;
    tab[i].has_cb_width = true;
}

bool flow_placement_anon_peek(const lxb_dom_element_t *el, const BlockFlowAnonBox **v, size_t *n)
{
    size_t i;
    FlowPlacementRecord *r = layout_job_placement();

    DCHECK(el != NULL && v != NULL && n != NULL,
           "CSS 2.2 §9.2.1.1's anonymous-box record was asked about no box or with nowhere to answer");
    if (r == NULL || r->cap == 0) return false;
    i = fp_probe(r->tab, r->cap, el);
    if (r->tab[i].el == NULL || !r->tab[i].has_anon) return false;
    *v = r->tab[i].anon;
    *n = r->tab[i].anon_n;
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

void flow_placement_first_float_record(const lxb_dom_element_t *root, lxb_dom_element_t *f)
{
    FpEntry *e;

    DCHECK(root != NULL, "a context's first float was recorded for no context root");
    e = fp_entry_for_write(root);
    if (e == NULL) return;
    DCHECK(!e->has_first_float || e->first_float == f,
           "one block formatting context reported two different first floats in one job; the search reads "
           "only the tree and the cascade");
    e->first_float = f;
    e->has_first_float = true;
}

bool flow_placement_first_float_peek(const lxb_dom_element_t *root, lxb_dom_element_t **out)
{
    const FpEntry *e;

    DCHECK(root != NULL && out != NULL, "a context's first float was asked for no root or with nowhere to put it");
    e = fp_entry_peek(root);
    if (e == NULL || !e->has_first_float) return false;
    *out = e->first_float;
    return true;
}

void flow_placement_float_scan_hold(const lxb_dom_element_t *root, lxb_dom_node_t *next)
{
    FpEntry *e;

    DCHECK(root != NULL && next != NULL, "a float search's place was held for no context or no node");
    e = fp_entry_for_write(root);
    CHECK(e != NULL, "a float search stopped with no placement record to hold its place; it stops only under a "
                     "resting drive, whose job holds one");
    DCHECK(!e->has_first_float, "a float search held its place after its context's answer was recorded");
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
    size_t i;

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
