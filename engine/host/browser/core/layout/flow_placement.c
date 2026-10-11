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
    /* CSS 2.2 §9.2.1.1's anonymous block boxes of this box, as block_flow_anonymous_boxes enumerates them;
       owned, NULL when there are none. */
    BlockFlowAnonBox        *anon;
    size_t                   anon_n;
    bool                     has_top;
    bool                     has_origin;
    bool                     has_cb_width;
    bool                     has_anon;
    bool                     anon_served;   /* the anonymous boxes have been served at least once */
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

bool flow_placement_anon_first_serve(const lxb_dom_element_t *el)
{
    size_t i;
    FlowPlacementRecord *r = layout_job_placement();

    DCHECK(r != NULL && r->cap != 0, "a first serve was asked of a placement record that holds nothing");
    i = fp_probe(r->tab, r->cap, el);
    DCHECK(r->tab[i].el == el && r->tab[i].has_anon,
           "a first serve was asked for a container whose anonymous boxes the record does not hold");
    if (r->tab[i].anon_served) return false;
    r->tab[i].anon_served = true;
    return true;
}

void flow_placement_anon_record(const lxb_dom_element_t *el, const BlockFlowAnonBox *v, size_t n)
{
    size_t i;
    FlowPlacementRecord *r = layout_job_placement();
    FpEntry *tab;

    DCHECK(el != NULL && (n == 0 || v != NULL),
           "CSS 2.2 §9.2.1.1's anonymous-box record was handed boxes with no container or no array");
    /* Counted before the guard, for the origin record's reason. */
    g_anon_derived++;
    if (r == NULL) return;
    if (r->used * 2 >= r->cap) fp_grow(r);
    tab = r->tab;
    i = fp_probe(tab, r->cap, el);
    if (tab[i].el == NULL) {
        tab[i].el = el;
        r->used++;
    }
    DCHECK(!tab[i].has_anon,
           "a container's CSS 2.2 §9.2.1.1 anonymous boxes were enumerated twice in one job; the enumeration "
           "asks this record first, so a second derivation is a probe that missed a key a write had landed");
    tab[i].anon = NULL;
    if (n != 0) {
        tab[i].anon = malloc(n * sizeof *tab[i].anon);
        CHECK(tab[i].anon != NULL, "out of memory recording one container's anonymous block boxes");
        memcpy(tab[i].anon, v, n * sizeof *tab[i].anon);
    }
    tab[i].anon_n = n;
    tab[i].has_anon = true;
    tab[i].anon_served = false;
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

bool flow_placement_agrees(const lxb_dom_element_t *el, CssPx top)
{
    CssPx rec;

    DCHECK(el != NULL, "CSS 2.1 §9.4.1's placement record was asked to agree about no box");
    if (!flow_placement_peek(el, &rec)) return false;
    return fp_same_example(rec, top);
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
