/* A layout job's record of the cascade's per-element answers — see core/css/css_cascade_pass.h. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_author_sheets.h"
#include "core/css/css_cascade_pass.h"
#include "core/css/css_cascaded_style.h"
#include "core/css/css_computed_style.h"
#include "core/css/css_match_result.h"
#include "core/layout/layout_job.h"   /* the record of the job whose activation holds the thread */
#include "solver/flow.h"               /* the running flow's cascade input generation */

/* One element's answers; each is NULL until its first ask, and owned. */
typedef struct CpElement {
    lxb_dom_element_t        *el;         /* NULL is empty; nothing is ever removed, so there are no tombstones */
    struct CssMatchResult    *match;      /* closed, or open where a resting build parked */
    struct CssCascadedStyle  *cascaded;
    struct CssComputedStyle  *style;
} CpElement;

/* The census: lifetime counters of this agent, raised at the question and at its one outcome. */
static long long g_asks;
static long long g_served;
static long long g_resolved;
static long long g_match_asks;
static long long g_match_served;
static long long g_match_built;
static long long g_match_parked;

/* The initial capacity, which decides only when the table is rebuilt at twice the size and never which
   questions are answered; a document that asks for more gets a bigger table, without limit. */
#define CP_CAP0 ((size_t)256)

/* The validity argument in the header, checked at every ask: the record stamps the running flow and its
   cascade input generation at its first ask, and a later ask under another flow, or after that flow wrote an
   input, is a held answer read over inputs that moved. */
static void cp_valid(CssCascadeRecord *r)
{
    uint64_t writes;
    const void *flow = flow_running_cascade_writes(&writes);

    if (!r->stamped) {
        r->flow = flow;
        r->writes = writes;
        r->stamped = true;
        return;
    }
    DCHECKF(r->flow == flow && r->writes == writes,
            "a layout job's cascade record was asked under %s after %llu cascade input writes against %llu at "
            "its first ask. Its answers are valid only while no input moves, and a job rests only inside its "
            "own flow's member call, so a flow ran code between two activations of its job, or a job was "
            "entered by another flow; whatever let it must shed the record first",
            r->flow == flow ? "its own flow" : "another flow", (unsigned long long)writes,
            (unsigned long long)r->writes);
}

static CssCascadeRecord *cp_held(void)
{
    CssCascadeRecord *r = layout_job_cascade();

    DCHECK(r != NULL, "the cascade record was asked with none held — every ask is made inside a computed style "
                      "hold, which opens a drive on the synchronous job when no driven job is entered");
    cp_valid(r);
    return r;
}

/* An element's slot: its address, whose low bits are all alignment, through splitmix64's finalizer, probed
   linearly to the slot holding `el` or the first empty one, for the read and the write alike. */
static size_t cp_probe(const CpElement *tab, size_t cap, const lxb_dom_element_t *el)
{
    uint64_t h = (uint64_t)(uintptr_t)el;
    size_t i;

    h ^= h >> 30; h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27; h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    i = (size_t)h & (cap - 1);
    while (tab[i].el != NULL && tab[i].el != el) i = (i + 1) & (cap - 1);
    return i;
}

static void cp_grow(CssCascadeRecord *r)
{
    size_t cap = r->elem_cap ? r->elem_cap * 2 : CP_CAP0, i;
    CpElement *tab = calloc(cap, sizeof *tab);

    CHECK(tab != NULL, "css-cascade-pass-oom: the record's element table could not be grown");
    for (i = 0; i < r->elem_cap; i++)
        if (r->elem[i].el != NULL) tab[cp_probe(tab, cap, r->elem[i].el)] = r->elem[i];
    free(r->elem);
    r->elem = tab;
    r->elem_cap = cap;
}

/* `el`'s slot in the held record, claimed empty at its first ask. The slot's address is stable only until
   the next claim, so callers read or write it and let go. */
static CpElement *cp_slot(CssCascadeRecord *r, lxb_dom_element_t *el)
{
    size_t i;

    DCHECK(el != NULL, "the cascade record was asked about no element");
    if (r->elem_used * 2 >= r->elem_cap) cp_grow(r);
    i = cp_probe(r->elem, r->elem_cap, el);
    if (r->elem[i].el == NULL) {
        r->elem[i].el = el;
        r->elem_used++;
    }
    return &r->elem[i];
}

void css_cascade_record_release(CssCascadeRecord *r)
{
    size_t i;

    /* A cascaded style and a match result borrow a held sheet set's layer nodes, so they go first. */
    for (i = 0; i < r->elem_cap; i++) {
        if (r->elem[i].el == NULL) continue;
        if (r->elem[i].cascaded != NULL) css_cascaded_style_free(r->elem[i].cascaded);
        if (r->elem[i].match != NULL) css_match_result_free(r->elem[i].match);
        if (r->elem[i].style != NULL) css_computed_style_free(r->elem[i].style);
    }
    free(r->elem);
    r->elem = NULL;
    r->elem_cap = 0;
    r->elem_used = 0;
    for (i = 0; i < r->author_n; i++) css_author_sheets_free(r->author[i]);
    free(r->author);
    r->author = NULL;
    r->author_n = 0;
    r->author_cap = 0;
    r->flow = NULL;
    r->writes = 0;
    r->stamped = false;
}

bool css_cascade_record_is_held(void) { return layout_job_cascade() != NULL; }

void css_cascade_input_written(void) { flow_running_cascade_written(); }

bool css_cascade_pass_author_ask(const lxb_dom_node_t *root, struct CssAuthorSheets **out)
{
    CssCascadeRecord *r = cp_held();
    size_t i;

    DCHECK(root != NULL && out != NULL, "an author sheet set was asked for no root or with nowhere to put it");
    for (i = 0; i < r->author_n; i++) {
        if (css_author_sheets_root(r->author[i]) != root) continue;
        *out = r->author[i];
        return true;
    }
    return false;
}

void css_cascade_pass_author_hold(struct CssAuthorSheets *sheets)
{
    CssCascadeRecord *r = cp_held();
    size_t i;

    DCHECK(sheets != NULL, "an author sheet set was offered to the record as nothing");
    for (i = 0; i < r->author_n; i++)
        DCHECK(css_author_sheets_root(r->author[i]) != css_author_sheets_root(sheets),
               "a second author sheet set was offered for a root the record already holds one for — its one "
               "caller asks before it builds");
    if (r->author_n == r->author_cap) {
        size_t cap = r->author_cap ? r->author_cap * 2 : 4;
        struct CssAuthorSheets **grown = realloc(r->author, cap * sizeof *grown);

        CHECK(grown != NULL, "css-cascade-pass-oom: the record's author sheet sets could not be grown");
        r->author = grown;
        r->author_cap = cap;
    }
    r->author[r->author_n++] = sheets;
}

static bool cp_sheets_held(const CssCascadeRecord *r, const struct CssAuthorSheets *sheets)
{
    size_t i;

    for (i = 0; i < r->author_n; i++)
        if (r->author[i] == sheets) return true;
    return false;
}

bool css_cascade_pass_match_ask(lxb_dom_element_t *el, const struct CssAuthorSheets *sheets,
                                struct CssMatchResult **out)
{
    CpElement *slot = cp_slot(cp_held(), el);

    DCHECK(sheets != NULL && out != NULL, "a match result was asked of the record under no sheet set or with "
                                          "nowhere to put it");
    g_match_asks++;
    *out = NULL;
    if (slot->match == NULL) return false;
    if (!css_match_result_closed(slot->match)) {
        DCHECK(css_match_result_sheets(slot->match) == sheets,
               "the record handed back a parked match result opened under a different sheet set than the one "
               "its root now resolves under — the set it borrows was replaced while the record was held");
        *out = slot->match;
        slot->match = NULL;
        return false;
    }
    /* The record holds one sheet set per root and no tree write moves an element between roots while it is
       held, so the set this result was matched under is the one its root resolves under now. */
    DCHECK(css_match_result_sheets(slot->match) == sheets,
           "the record served an element's match result built under a different sheet set than the one its "
           "root now resolves under — the set a held result borrows was replaced or the element changed roots "
           "while the record was held");
    *out = slot->match;
    g_match_served++;
    return true;
}

void css_cascade_pass_match_hold(struct CssMatchResult *m)
{
    CssCascadeRecord *r = cp_held();
    CpElement *slot;

    DCHECK(m != NULL && css_match_result_closed(m), "a match result was offered to the record as nothing or open");
    DCHECK(cp_sheets_held(r, css_match_result_sheets(m)),
           "a match result was offered to the record under a sheet set the record does not hold — its layer "
           "nodes would be freed with that set while the result is served on");
    slot = cp_slot(r, css_match_result_element(m));
    DCHECK(slot->match == NULL, "a second match result was offered for an element the record already holds one "
                                "for — its one caller asks before it builds");
    slot->match = m;
    g_match_built++;
}

void css_cascade_pass_match_park(struct CssMatchResult *m)
{
    CssCascadeRecord *r = cp_held();
    CpElement *slot;

    DCHECK(m != NULL && !css_match_result_closed(m), "a match result was parked as nothing or closed");
    DCHECK(cp_sheets_held(r, css_match_result_sheets(m)),
           "a match result was parked under a sheet set the record does not hold — its layer nodes would be "
           "freed with that set while the build is parked");
    slot = cp_slot(r, css_match_result_element(m));
    DCHECK(slot->match == NULL, "a match result was parked for an element whose slot holds one — the ask that "
                                "missed handed the parked one back to its builder");
    slot->match = m;
    g_match_parked++;
}

struct CssCascadedStyle *css_cascade_pass_cascaded_ask(lxb_dom_element_t *el)
{
    CpElement *slot = cp_slot(cp_held(), el);

    g_asks++;
    if (slot->cascaded == NULL) return NULL;
    g_served++;
    return slot->cascaded;
}

void css_cascade_pass_cascaded_hold(struct CssCascadedStyle *s)
{
    CpElement *slot;

    DCHECK(s != NULL, "a cascaded style was offered to the record as nothing");
    slot = cp_slot(cp_held(), css_cascaded_style_element(s));
    DCHECK(slot->cascaded == NULL, "a second cascaded style was offered for an element the record already holds "
                                   "one for — its one caller asks before it collects");
    slot->cascaded = s;
    g_resolved++;
}

struct CssComputedStyle *css_cascade_pass_style(lxb_dom_element_t *el)
{
    CssCascadeRecord *r = layout_job_cascade();
    CpElement *slot;

    if (r == NULL) return NULL;
    cp_valid(r);
    slot = cp_slot(r, el);
    if (slot->style == NULL) slot->style = css_computed_style_create(el);
    return slot->style;
}

void css_cascade_pass_census(CssCascadePassCensus *out)
{
    DCHECK(out != NULL, "css-cascade-5 §4.2's cascaded-value census was asked for with nowhere to write it");
    /* Every ask has exactly one outcome — the element's held cascaded style answered it, or its one caller
       collected and resolved one first with no return between the two — so the counts close. */
    DCHECKF(g_asks == g_served + g_resolved,
            "css-cascade-5 §4.2's cascaded-value census does not close: %lld asks against %lld served and "
            "%lld resolved. A gap is an arm that took an answer without saying which outcome it was, and "
            "every share a reader divides out of these rows is over a denominator that is not the population",
            g_asks, g_served, g_resolved);
    DCHECKF(g_match_asks == g_match_served + g_match_built + g_match_parked,
            "the match-result census does not close: %lld asks against %lld served, %lld built and %lld "
            "parked. Every miss builds on and either offers the closed result or parks the open one with no "
            "return between, so a gap is an arm that matched without saying which outcome it was",
            g_match_asks, g_match_served, g_match_built, g_match_parked);
    out->asks_life = g_asks;
    out->served_life = g_served;
    out->resolved_life = g_resolved;
    css_computed_style_census(&out->computed_asks_life, &out->computed_served_life,
                              &out->computed_resolved_life);
}
