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
#include "solver/dom_cow.h"   /* dom_cow_version — the tree number the span is held to, SWAP included */

typedef struct {
    const lxb_dom_element_t *el;   /* NULL is EMPTY; nothing is ever removed, so there are no tombstones */
    CssPx                    top;
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
           "positions vanished half way through a picture. The entry that opens a pass is the one that "
           "performs a whole-tree geometry walk, and there is exactly one of those: a second opener is the "
           "thing to find");
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
    i = fp_probe(g_tab, g_cap, el);
    if (g_tab[i].el == NULL) return false;
    *out = g_tab[i].top;
    g_served++;
    return true;
}

bool flow_placement_agrees(const lxb_dom_element_t *el, CssPx top)
{
    size_t i;

    DCHECK(el != NULL, "CSS 2.1 §9.4.1's placement record was asked to agree about no box");
    if (!g_open || g_cap == 0) return false;
    i = fp_probe(g_tab, g_cap, el);
    return g_tab[i].el != NULL && fp_same_example(g_tab[i].top, top);
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
    out->asks = g_asks;
    out->served = g_served;
    out->walks = g_walks;
    out->placements = g_placements;
    out->passes = g_passes;
}
