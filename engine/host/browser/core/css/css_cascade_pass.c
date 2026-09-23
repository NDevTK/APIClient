/* css-cascade-5 §4.2 "Cascaded Values"' answer for one (element, property), held for one whole-tree render.
   See core/css/css_cascade_pass.h for which of the three multiplier shapes this is, for why the record is
   keyed on the QUESTION rather than on the asker, and for the three places the span's safety is asserted. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_cascade_pass.h"
#include "solver/dom_cow.h"   /* dom_cow_version — the tree number the span is held to, SWAP included */

/* ONE ENTRY PER (ELEMENT, PROPERTY), AND THE PROPERTY IS HELD AS BYTES RATHER THAN AS THE CALLER'S POINTER.
   Most names that reach the cascade are string literals, and a custom property's is not: css-variables-1
   §2's `--*` family lets a page name one anything at all, and that name arrives on the stack of whichever
   substitution frame composed it. A key that stored the pointer would answer for freed bytes one frame
   later; a key that stored a FIXED-SIZE copy would have to truncate one, and two custom properties agreeing
   in their first N bytes would then share an answer — a wrong cascaded value with nothing to say so, and in
   a release build with no assert to catch it. So the name is copied WHOLE, and the copy is what the probe
   compares. */
typedef struct {
    lxb_dom_element_t *el;      /* NULL is EMPTY; nothing is ever removed, so there are no tombstones */
    char              *name;    /* owned, and never NULL in an occupied slot */
    /* THE CASCADED VALUE, OWNED, AND NULL IS ONE OF THE ANSWERS IT CARRIES. css-cascade-5 §4.2 "Cascaded
       Values": "if the output of the cascade is an empty list, there is no cascaded value" — which
       css-cascade-5 §7 "Defaulting" is written for, so it is a value this record must be able to HOLD
       rather than a hole it must treat as a miss. Occupancy is `el != NULL` and never a flag beside this
       pointer, so the two cannot come apart. */
    char              *value;
} CpEntry;

static CpEntry  *g_tab;
static size_t    g_cap;       /* a power of two, or zero when the record has no storage at all */
static size_t    g_used;
static bool      g_open;
static uint64_t  g_ver;       /* the tree version this span is held to, read at open */

/* THE CENSUS, AND IT IS NOW A PUBLISHED ROW — see `css_cascade_pass_census` at the foot of this file and the
   declaration in the header for what each row is a count OF and why each name carries its kind.
   THIS BLOCK USED TO SAY THE OPPOSITE AND IT IS REWRITTEN RATHER THAN DELETED, because the argument it made
   is the one a reader re-derives: it read that the census was READ BY THE IDENTITY BELOW AND BY NOTHING ELSE,
   that it was deliberately not a published row because a number nobody reads is the defect
   core/css/css_style_declaration.h names one level up, and that an assert IS a reader. The last clause is
   still true and it was not enough — the assert that was doing the reading sits inside
   `css_cascade_pass_close`, and that function runs only under a paint, which this component's own residual
   records that nothing outside this process asks for. So the reader existed and never ran, which reads
   exactly like a reader and is not one; `an assert IS a reader` is sound only where the assert is on a path
   somebody takes.
   `asks == served + resolved` remains the one property of the three that says the outcomes were counted at
   the same event, and it is asserted at BOTH events now rather than at one. Those are not two copies of one
   check: the close fires at the end of a paint and the census fires when the shipped path composes its result
   document, and today exactly one of those two happens. */
static long long g_asks;
static long long g_served;
static long long g_resolved;
/* HOW MANY WHOLE-TREE SPANS HAVE OPENED, WHICH IS WHAT MAKES A ZERO IN `g_served` MEAN ONE THING. Without it
   a run that never opened a pass and a run whose every ask was a first ask publish the same number, and those
   two ask for opposite work. Raised at open and never cleared — the close empties the TABLE and releases its
   strings, and touches none of these four. */
static long long g_passes;

/* THE ONE INITIAL CAPACITY, AND IT IS NOT A BOUND ON ANYTHING. §NO BOUNDS forbids deciding that work will
   not happen; this decides only how many pairs fit before the table is rebuilt at twice the size, which is a
   question about when a `realloc` runs and never about which questions get answered. A document that asks
   for more gets a bigger table, without limit, exactly as it gets a bigger DOM. */
#define CP_CAP0 ((size_t)256)

/* A (POINTER, NAME) SLOT. The element is an ADDRESS whose low bits are all alignment, so it is mixed with
   splitmix64's finalizer for the property that is wanted everywhere else — every input bit reaches every
   output bit — and the name is folded in with FNV-1a, whose stream the mix then spreads. Two properties on
   one element and one property on two elements are the two collisions this key space is mostly made of, and
   both halves have to reach the high bits for either to spread. */
static size_t cp_slot(const lxb_dom_element_t *el, const char *name, size_t cap)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    const unsigned char *p;

    for (p = (const unsigned char *)name; *p != '\0'; p++) {
        h ^= (uint64_t)*p;
        h *= 0x100000001b3ULL;
    }
    h ^= (uint64_t)(uintptr_t)el;
    h ^= h >> 30; h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27; h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return (size_t)h & (cap - 1);
}

/* THE PROBE, SHARED BY THE READ AND THE WRITE so that a key can never be looked up one way and stored
   another. Answers the slot the key occupies, or the first empty slot at which it would be inserted. */
static size_t cp_probe(CpEntry *tab, size_t cap, const lxb_dom_element_t *el, const char *name)
{
    size_t i = cp_slot(el, name, cap);

    while (tab[i].el != NULL && !(tab[i].el == el && strcmp(tab[i].name, name) == 0))
        i = (i + 1) & (cap - 1);
    return i;
}

static void cp_grow(void)
{
    size_t cap = g_cap ? g_cap * 2 : CP_CAP0, i;
    CpEntry *tab = calloc(cap, sizeof *tab);

    CHECK(tab != NULL, "css-cascade-pass-oom: css-cascade-5 §4.2's cascaded-value record could not be grown. "
                       "A dropped entry is not a slower answer here: the pass's own premise is that the "
                       "cascade's inputs do not move inside the span, so an answer re-derived after a drop "
                       "would be the same answer and the cost would be the multiplier this record exists to "
                       "remove, paid silently and only on the documents big enough to reach this line");
    for (i = 0; i < g_cap; i++)
        if (g_tab[i].el != NULL) tab[cp_probe(tab, cap, g_tab[i].el, g_tab[i].name)] = g_tab[i];
    free(g_tab);
    g_tab = tab;
    g_cap = cap;
}

void css_cascade_pass_open(void)
{
    DCHECK(!g_open,
           "css-cascade-5 §4.2's cascaded-value pass was opened INSIDE a pass. The two would share one "
           "record with two lifetimes, and the inner close empties a record the outer walk is still reading "
           "— so the outer walk's later asks would resolve the cascade again, which is correct, while the "
           "answers it had already served came out of a table that is gone. THE CASE THAT WILL REACH THIS "
           "FIRST IS NOT A MISTAKE: core/paint/box_paint.c's replaced-content arm names an `iframe`'s "
           "rendering as its child navigable's OWN, and painting one is a second core/paint/"
           "document_paint.c walk inside this one. That nested document needs its own record and not a "
           "share of this one — CSSOM §6.2's list of style sheets is its ROOT's and not this document's — "
           "so what the arm owes is a pass per DOCUMENT rather than a global one: give this record a stack "
           "whose top is the document being walked, and the outer walk's entries survive the inner close by "
           "construction");
    DCHECK(g_used == 0 && g_tab == NULL,
           "css-cascade-5 §4.2's cascaded-value record holds entries with no pass open. It is emptied at "
           "close and its storage released there, so anything standing here is a close that did not run — "
           "which means some arm of the walk returned without it, and the next pass would answer out of a "
           "table taken against a document nobody has established is still the same one");
    g_open = true;
    g_passes++;
    g_ver = dom_cow_version();
}

void css_cascade_pass_close(void)
{
    size_t i;

    DCHECK(g_open, "css-cascade-5 §4.2's cascaded-value pass was closed with none open");
    /* THE TREE NUMBER, ASKED AT THE END AS WELL AS AT EVERY ANSWER. solver/dom_cow.h advances it on an
       insert, on a removal and on the COW SWAP, so this is the one check that speaks for a FLOW SWITCH
       inside the walk: another flow's delta becoming the visible tree is exactly the state in which a
       cascaded value recorded by this pass describes a document nobody is standing in any more. */
    DCHECKF(dom_cow_version() == g_ver,
            "css-cascade-5 §4.2's cascaded-value pass opened at tree version %llu and closed at %llu. The "
            "span this record is sound over is one in which the cascade's inputs do not move, and "
            "solver/dom_cow.h's number moves for an insert, a removal and the COW swap — so this is a "
            "page's own mutation reached from inside a render, or a flow switch that happened during one. "
            "Neither is a slower answer: every value this pass served after the change was the declaration "
            "that won a cascade over a document that no longer exists. Find which of the two it is before "
            "choosing where the pass belongs",
            (unsigned long long)g_ver, (unsigned long long)dom_cow_version());
    /* THE ONE PROPERTY OF THE PAIR THAT SAYS THE TWO OUTCOMES WERE COUNTED AT THE SAME EVENT. An ask is
       counted at the QUESTION and a resolution at the answer the caller brings back, so a hit raises the
       first two and a miss raises the first and the third — a path that returns without counting is a
       number that reads as a served answer for the rest of the agent's life. */
    DCHECKF(g_asks == g_served + g_resolved,
            "css-cascade-5 §4.2's cascaded-value record was asked %lld times, served %lld and had %lld "
            "resolved for it. Every ask has exactly one outcome, so the two must sum to the first — a "
            "shortfall is an arm of the caller that took an answer without reporting which of the two it "
            "was, and the ratio this record is judged by is then a fact about which arms remembered to "
            "count rather than about the render",
            g_asks, g_served, g_resolved);
    for (i = 0; i < g_cap; i++) {
        free(g_tab[i].name);
        free(g_tab[i].value);
    }
    free(g_tab);
    g_tab = NULL;
    g_cap = 0;
    g_used = 0;
    g_open = false;
}

bool css_cascade_pass_is_open(void) { return g_open; }

bool css_cascade_pass_ask(lxb_dom_element_t *el, const char *name, char **out)
{
    size_t i;

    DCHECK(el != NULL && name != NULL,
           "css-cascade-5 §4.2's cascaded-value record was asked with no element or no property name. The "
           "cascade is a function of BOTH — css-cascade-5 §6 \"Cascading\" sorts \"declared values for a "
           "given property on a given element\" — so neither half is optional and a key missing one names "
           "no question");
    DCHECK(out != NULL,
           "css-cascade-5 §4.2's cascaded-value record was asked with nowhere to put the answer");
    g_asks++;
    if (!g_open || g_cap == 0) return false;
    /* THE TREE NUMBER ON THE READ SIDE, so the FIRST served answer after a change is what crashes rather
       than a picture that is already drawn. See this record's close for why the two must agree. */
    DCHECKF(dom_cow_version() == g_ver,
            "css-cascade-5 §4.2's cascaded-value record was read at tree version %llu inside a pass opened "
            "at %llu — see this record's close",
            (unsigned long long)dom_cow_version(), (unsigned long long)g_ver);
    i = cp_probe(g_tab, g_cap, el, name);
    if (g_tab[i].el == NULL) return false;
    /* THE COPY IS THE CONTRACT AND NOT A CONVENIENCE: the cascade's own answer is OWNED by its caller, and
       every one of them frees it. Handing back the record's pointer would have the first caller free the
       entry and the second read it. */
    if (g_tab[i].value == NULL) {
        *out = NULL;
    } else {
        *out = strdup(g_tab[i].value);
        CHECK(*out != NULL, "css-cascade-pass-oom: a recorded cascaded value could not be copied for its "
                            "caller. The caller owns what the cascade hands back and frees it, so there is "
                            "no arm here that can answer with the record's own bytes");
    }
    g_served++;
    return true;
}

void css_cascade_pass_record(lxb_dom_element_t *el, const char *name, const char *value)
{
    size_t i;

    DCHECK(el != NULL && name != NULL,
           "css-cascade-5 §4.2's cascaded-value record was handed an answer with no element or no property "
           "name to file it under");
    g_resolved++;
    if (!g_open) return;
    if (g_used * 2 >= g_cap) cp_grow();
    i = cp_probe(g_tab, g_cap, el, name);
    /* ONE ANSWER PER KEY PER PASS, AND A SECOND IS NOT A DISAGREEMENT TO CHECK — IT CANNOT HAPPEN. The one
       caller asks before it resolves and resolves only on a miss, so a key that is already here was served
       rather than re-derived, and an `agrees` assert over a second write would guard a state no path
       reaches. What could put a key here twice is a probe that looked one way and stored another, and that
       is what the SHARED probe above removes rather than what a comparison here would catch. */
    DCHECK(g_tab[i].el == NULL,
           "css-cascade-5 §4.2's cascaded-value record was handed a SECOND answer for a pair it already "
           "holds. Its one caller asks this record before it resolves the cascade and resolves only when "
           "the ask came back empty, so a key standing here means an ask and a record disagreed about "
           "which slot the pair occupies — which is the probe reading one way and writing another, and "
           "every answer this table has served since is a value filed under some other question");
    g_tab[i].el = el;
    g_tab[i].name = strdup(name);
    CHECK(g_tab[i].name != NULL, "css-cascade-pass-oom: a property name could not be copied into "
                                 "css-cascade-5 §4.2's record. The key is the name's BYTES — see the entry "
                                 "— so a NULL here is a slot that answers for every name at once");
    if (value != NULL) {
        g_tab[i].value = strdup(value);
        CHECK(g_tab[i].value != NULL, "css-cascade-pass-oom: a cascaded value could not be copied into "
                                      "css-cascade-5 §4.2's record");
    }
    g_used++;
}

/* THE CENSUS, ASSERTING BEFORE IT COPIES — core/layout/flow_placement.c's `flow_placement_census` is the
   arrangement and this is it over this component's four rows. The header states what each row counts,
   why every one of them is a LIFETIME count and says so in its own name, and why the record's LIVE size is
   deliberately not among them. */
void css_cascade_pass_census(CssCascadePassCensus *out)
{
    DCHECK(out != NULL, "css-cascade-5 §4.2's cascaded-value census was asked for with nowhere to write it");
    /* THE ONE PROPERTY OF THE THREE THAT SAYS THE OUTCOMES WERE COUNTED AT THE SAME EVENT, ASSERTED HERE AND
       NOT ONLY AT THE CLOSE. The close asserts the identical equality and runs at the end of a paint; this
       runs when the shipped path composes its result document, and today that is the only one of the two that
       happens — so this is the call that ARMS the check rather than a second copy of it. A shortfall means
       the ask at core/css/css_style_declaration.c returned between the ask and the record, and the ratio a
       reader takes off these rows would then be a fact about which arms remembered to count rather than
       about the render. */
    DCHECKF(g_asks == g_served + g_resolved,
            "css-cascade-5 §4.2's cascaded-value census does not close: %lld asks against %lld served and "
            "%lld resolved. Every ask has exactly one outcome — this record answered it, or its one caller "
            "ran the cascade and reported the answer immediately with no return between the two — so a gap "
            "is an arm that took an answer without saying which outcome it was, and every share a reader "
            "divides out of these rows is over a denominator that is not the population",
            g_asks, g_served, g_resolved);
    /* …AND THE DISCRIMINATOR'S OWN PRECONDITION, WHICH IS CHECKABLE ON ONE CENSUS AND WAS ASSERTED
       NOWHERE. `g_served` is raised only past the `!g_open` gate in the ask, and `g_open` is set only by the
       open that raises `g_passes` — so a served answer implies a span, and the implication costs one
       comparison and needs no series. It is not decoration on the identity above: the header rests the whole
       readability of `served_life` on `passes_life` telling an absent CALLER apart from keys that do not
       repeat, and that row is the one this file's own residual names as the open question, so a SECOND opener
       is the change most likely to be made here. An opener that set the record open without counting a span
       would leave this row at zero while answers were served out of its spans — the discriminator lying
       about exactly the split it exists to make, with the identity above still closing. */
    DCHECKF(g_served == 0 || g_passes > 0,
            "css-cascade-5 §4.2's cascaded-value record served %lld answers across %lld spans. An answer is "
            "served only inside an open pass and a pass is opened only by the call that counts one, so a "
            "served answer with no span counted is an opener that set this record open without raising that "
            "count. `passes_life` is the one row that says whether a zero in `served_life` means there was no "
            "caller or means the keys do not repeat, and those ask for opposite work — so every reader of "
            "that split would be told there was no caller while this record was answering out of its spans",
            g_served, g_passes);
    out->asks_life = g_asks;
    out->served_life = g_served;
    out->resolved_life = g_resolved;
    out->passes_life = g_passes;
}
