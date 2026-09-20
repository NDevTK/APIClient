/* CSS 2.1 §9.4.1 "Block formatting contexts"' stack positions, held for one whole-tree geometry pass.
   See core/layout/flow_placement.h for why the walk REPORTS rather than the caller REMEMBERS, and for the
   three places the span's safety is asserted. */
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
#include "solver/dom_cow.h"   /* dom_cow_version — the tree number the span is held to, SWAP included */

/* ONE ENTRY PER BOX AND TWO FACTS IN IT, WHICH IS ONE TABLE AND NOT TWO. The two questions a whole-tree
   geometry walk asks about a box are keyed on the same element, go stale on the same events and live in the
   same span, so two tables would be two probe implementations, two growth policies and two key spaces over
   one key — and the origin's own check reads BOTH facts of a box in one lookup, which two tables would make
   two. `has_top` and `has_origin` are separate because the two are written by DIFFERENT components at
   DIFFERENT moments: core/layout/block_flow.c's walk reports a top for every box it places whether or not
   anybody asks its origin, and core/layout/flow_position.c records an origin for boxes §9.4.1 never stacks. */
typedef struct {
    const lxb_dom_element_t *el;   /* NULL is EMPTY; nothing is ever removed, so there are no tombstones */
    CssPx                    top;
    FlowPoint                origin;
    /* THE CONTAINING BLOCK THE ORIGIN WAS DERIVED FROM, and NULL for every other arm — which is the whole of
       what the origin's check needs to know about how the value was produced. core/layout/flow_position.c's
       §10.1 SECOND case is the only arm with an equation over another box's recorded origin; the root arm,
       the out-of-flow arms and the §17.5 table arms each derive their point some other way, so a non-NULL
       here says "this point is that equation" and a NULL says "this component has no second route to it". */
    const lxb_dom_element_t *origin_cb;
    bool                     has_top;
    bool                     has_origin;
} FpEntry;

static FpEntry  *g_tab;
static size_t    g_cap;       /* a power of two, or zero when the record has no storage at all */
static size_t    g_used;
static bool      g_open;
static uint64_t  g_ver;       /* the tree version this span is held to, read at open */

static long long g_asks;
static long long g_served;
static long long g_walks;
static long long g_placements;
static long long g_passes;
static long long g_origin_asks;
static long long g_origin_served;
static long long g_origin_derived;

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

static void fp_grow(void)
{
    size_t cap = g_cap ? g_cap * 2 : FP_CAP0, i;
    FpEntry *tab = calloc(cap, sizeof *tab);

    CHECK(tab != NULL, "flow-placement-oom: CSS 2.1 §9.4.1's placement record could not be grown. A dropped "
                       "position is not a slower answer, it is a box placed against a stack this pass has "
                       "already reported differently");
    for (i = 0; i < g_cap; i++)
        if (g_tab[i].el != NULL) tab[fp_probe(tab, cap, g_tab[i].el)] = g_tab[i];
    free(g_tab);
    g_tab = tab;
    g_cap = cap;
}

void flow_placement_pass_open(void)
{
    DCHECK(!g_open,
           "CSS 2.1 §9.4.1's placement pass was opened INSIDE a pass. The two would share one record with two "
           "lifetimes, and the inner close empties a record the outer walk is still reading — so the outer "
           "walk's later asks would run §9.4.1's walk again, which is correct, while its own recorded "
           "positions vanished half way through a picture. THE CASE THAT WILL REACH THIS FIRST IS NOT A "
           "MISTAKE: core/paint/box_paint.c's replaced-content arm names an `iframe`'s rendering as its "
           "child navigable's OWN, and painting one is a second core/paint/document_paint.c walk inside this "
           "one. That nested document needs its own record and not a share of this one — its boxes are "
           "placed by its own §9.4.1 stacks — so what the arm owes is a pass per DOCUMENT rather than a "
           "global one: give this record a stack whose top is the document being walked, and the outer "
           "walk's entries survive the inner close by construction");
    DCHECK(g_used == 0 && g_tab == NULL,
           "CSS 2.1 §9.4.1's placement record holds entries with no pass open. It is emptied at close and its "
           "storage released there, so anything standing here is a close that did not run — which means some "
           "arm of the walk returned without it, and the next pass would answer out of a record taken against "
           "a document nobody has established is still the same one");
    g_open = true;
    g_ver = dom_cow_version();
    g_passes++;
}

void flow_placement_pass_close(void)
{
    DCHECK(g_open, "CSS 2.1 §9.4.1's placement pass was closed with none open");
    /* THE TREE NUMBER, ASKED AT THE END AS WELL AS AT EVERY ANSWER. solver/dom_cow.h advances it on an insert,
       on a removal and on the COW SWAP, so this is the one check that speaks for a FLOW SWITCH inside the
       walk: another flow's delta becoming the visible tree is exactly the state in which a position recorded
       by this pass describes a document nobody is standing in any more. */
    DCHECKF(dom_cow_version() == g_ver,
            "CSS 2.1 §9.4.1's placement pass opened at tree version %llu and closed at %llu. The span this "
            "record is sound over is one in which the document does not change, and solver/dom_cow.h's number "
            "moves for an insert, a removal and the COW swap — so this is a page's own mutation reached from "
            "inside a geometry walk, or a flow switch that happened during one. Neither is a slower answer: "
            "every position this pass served after the change was a distance down a stack that no longer "
            "exists. Find which of the two it is before choosing where the pass belongs",
            (unsigned long long)g_ver, (unsigned long long)dom_cow_version());
    free(g_tab);
    g_tab = NULL;
    g_cap = 0;
    g_used = 0;
    g_open = false;
}

bool flow_placement_pass_is_open(void) { return g_open; }

void flow_placement_record(const lxb_dom_element_t *el, CssPx top)
{
    size_t i;

    DCHECK(el != NULL, "CSS 2.1 §9.4.1's placement record was handed a position with no box to attach it to. "
                       "§9.2.1.1's anonymous block box is the one thing on that stack with no element, and "
                       "the walk skips it here rather than passing a null: an anonymous box is never asked "
                       "for by element, so a record entry for one could only ever be dead weight");
    if (!g_open) return;
    if (g_used * 2 >= g_cap) fp_grow();
    i = fp_probe(g_tab, g_cap, el);
    if (g_tab[i].el == NULL) {
        g_tab[i].el = el;
        g_used++;
        g_placements++;
    } else if (!g_tab[i].has_top) {
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
        DCHECKF(fp_same_example(g_tab[i].top, top),
                "CSS 2.1 §9.4.1's walk placed one box at two different tops inside ONE pass: %g then %g. The "
                "walk reads computed values and the child list and nothing else, so two runs of it over one "
                "document answer the same number — which makes this either a document that changed under the "
                "pass (core/layout/flow_placement.h names the three places that is asserted, and one of them "
                "has let something through) or a walk whose `pass` or static-position argument moves a "
                "position, which core/layout/block_flow.c states it does not",
                g_tab[i].top.px, top.px);
        g_placements++;
    }
    g_tab[i].top = top;
    g_tab[i].has_top = true;
}

/* THE LOOKUP, SHARED BY EVERY READER AND COUNTING NOTHING — see the header for why the census belongs to the
   ASK rather than to the lookup under it. Also where the pass's tree number is checked on the read side, so
   the FIRST served answer after a change is what crashes rather than a picture that is already drawn. */
bool flow_placement_peek(const lxb_dom_element_t *el, CssPx *out)
{
    size_t i;

    DCHECK(el != NULL, "CSS 2.1 §9.4.1's placement record was asked about no box");
    DCHECK(out != NULL, "CSS 2.1 §9.4.1's placement record was asked with nowhere to put the answer");
    if (!g_open || g_cap == 0) return false;
    DCHECKF(dom_cow_version() == g_ver,
            "CSS 2.1 §9.4.1's placement record was read at tree version %llu inside a pass opened at %llu. "
            "See this record's close for why the two must agree; asking at the ANSWER as well as at the close "
            "is what makes the first served position after a change the thing that crashes",
            (unsigned long long)dom_cow_version(), (unsigned long long)g_ver);
    i = fp_probe(g_tab, g_cap, el);
    if (g_tab[i].el == NULL || !g_tab[i].has_top) return false;
    *out = g_tab[i].top;
    return true;
}

bool flow_placement_origin_peek(const lxb_dom_element_t *el, FlowPoint *out, const lxb_dom_element_t **cb_out)
{
    size_t i;

    DCHECK(el != NULL, "CSS 2 §8.1 \"Box dimensions\"' border-box origin record was asked about no box");
    DCHECK(out != NULL, "CSS 2 §8.1 \"Box dimensions\"' border-box origin record was asked with nowhere "
                        "to put the answer");
    if (!g_open || g_cap == 0) return false;
    DCHECKF(dom_cow_version() == g_ver,
            "CSS 2 §8.1 \"Box dimensions\"' border-box origin record was read at tree version %llu inside a "
            "pass opened at "
            "%llu — see this record's close",
            (unsigned long long)dom_cow_version(), (unsigned long long)g_ver);
    i = fp_probe(g_tab, g_cap, el);
    if (g_tab[i].el == NULL || !g_tab[i].has_origin) return false;
    *out = g_tab[i].origin;
    if (cb_out != NULL) *cb_out = g_tab[i].origin_cb;
    return true;
}

void flow_placement_origin_record(const lxb_dom_element_t *el, FlowPoint origin,
                                  const lxb_dom_element_t *derived_from)
{
    size_t i;

    DCHECK(el != NULL, "CSS 2 §8.1 \"Box dimensions\"' border-box origin record was handed a point with "
                       "no box to attach it to");
    /* THE COUNT IS TAKEN BEFORE THE STORE'S GUARD, AND THE ORDER OF THESE TWO LINES IS THE WHOLE OF A DEFECT
       THIS FILE SHIPPED. It stood below the early return, so ONE `if (!g_open)` was answering two questions —
       "is there anywhere to put this point" and "did this agent derive a point" — and §A-PREDICATE-THAT-
       ANSWERS-TWO-QUESTIONS' rule held exactly: the stricter question won, the looser one was refused with
       nothing to say it had been asked, and the cost landed on the census. The ask above it is counted
       whether or not a pass is open, so every derivation made OUTSIDE a pass went into the numerator and
       into neither denominator. It is not a rounding error: a fixture that reads geometry through CSSOM
       VIEW's own members rather than through a paint makes nearly EVERY ask an outside one, and the identity
       broke 283 against 7 and 4 on the first build that ran one.
       THE GUARD IS RIGHT FOR THE STORE AND HAS NOTHING TO SAY ABOUT THE COUNT. With no pass there is no
       table, so a point has nowhere to go; the DERIVATION still happened, and `origin_derived` is a count of
       derivations performed by this agent rather than of entries in any table.
       WHY THIS PAIR COUNTS INSIDE ITS RECORD AND §9.4.1's PAIR COUNTS IN AN ENTRY OF ITS OWN, since the
       asymmetry is the first thing a reader will want to remove: `flow_placement_record` is called once per
       box the WALK passes, so it fires many times for one ask and a count there would not be a count of
       asks-that-missed at all — which is why `flow_placement_walked` exists. This entry is called exactly
       once per derivation, so the count belongs with it, and putting it here is what makes a caller unable to
       record without counting. Making the two symmetric would re-introduce a call a caller can forget. */
    g_origin_derived++;
    if (!g_open) return;
    if (g_used * 2 >= g_cap) fp_grow();
    i = fp_probe(g_tab, g_cap, el);
    if (g_tab[i].el == NULL) {
        g_tab[i].el = el;
        g_used++;
    } else {
        /* ONE BOX'S ORIGIN COMPUTED TWICE IN ONE PASS, WHICH THE MEMO MAKES UNREACHABLE AND WHICH IS THEREFORE
           WORTH ASSERTING RATHER THAN ALLOWING. core/layout/flow_position.c asks this record before it
           computes and records every point it computes, so a second computation means a read that missed
           where a write had already landed — a probe that answers two slots for one key. The numbers are
           compared rather than the state, because a disagreement is the finding and an agreement under a
           broken probe would be a coincidence worth knowing about too. */
        DCHECKF(!g_tab[i].has_origin ||
                    (fp_same_example(g_tab[i].origin.x, origin.x) && fp_same_example(g_tab[i].origin.y, origin.y)),
                "CSS 2 §8.1 \"Box dimensions\"' border-box origin was computed twice in one pass for one box and "
                "the two "
                "disagree: (%g, %g) then (%g, %g). core/layout/flow_position.c reads this record before it "
                "computes, so a second computation at all means a read missed a key a write had landed — and "
                "two different answers mean the document moved under the pass on an axis its tree version "
                "cannot see",
                g_tab[i].origin.x.px, g_tab[i].origin.y.px, origin.x.px, origin.y.px);
    }
    g_tab[i].origin = origin;
    g_tab[i].origin_cb = derived_from;
    g_tab[i].has_origin = true;
}

bool flow_placement_origin_ask(const lxb_dom_element_t *el, FlowPoint *out, const lxb_dom_element_t **cb_out)
{
    g_origin_asks++;
    if (!flow_placement_origin_peek(el, out, cb_out)) return false;
    g_origin_served++;
    return true;
}

bool flow_placement_ask(const lxb_dom_element_t *el, CssPx *out)
{
    size_t i;

    DCHECK(el != NULL, "CSS 2.1 §9.4.1's placement was asked for with no box");
    DCHECK(out != NULL, "CSS 2.1 §9.4.1's placement was asked for with nowhere to put the answer");
    g_asks++;
    if (!g_open || g_cap == 0) return false;
    DCHECKF(dom_cow_version() == g_ver,
            "CSS 2.1 §9.4.1's placement record was asked a question at tree version %llu inside a pass opened "
            "at %llu. See this record's close for why the two must agree; asking at the ANSWER as well as at "
            "the close is what makes the first served position after a change the thing that crashes, rather "
            "than a picture that is already drawn",
            (unsigned long long)dom_cow_version(), (unsigned long long)g_ver);
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
    out->passes = g_passes;
    out->origin_asks = g_origin_asks;
    out->origin_served = g_origin_served;
    out->origin_derived = g_origin_derived;
}
