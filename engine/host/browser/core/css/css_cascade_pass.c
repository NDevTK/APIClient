/* css-cascade-5 §4.2 "Cascaded Values"' answer for one (element, property), held by a layout job — see
   core/css/css_cascade_pass.h. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_author_sheets.h"
#include "core/css/css_cascade_pass.h"
#include "core/css/css_computed_style.h"
#include "core/css/css_match_result.h"
#include "core/layout/layout_job.h"   /* the record of the job whose activation holds the thread */

/* One entry per (element, property), holding two facts about the pair: its cascaded value and its computed
   value (css_computed_value), each with its own presence flag because the two are recorded by different
   callers at different moments. The name is copied whole: a custom property's name (css-variables-1
   §2's `--*`) arrives on the stack of the frame that composed it, so a stored pointer would answer for freed
   bytes, and a fixed-size copy would let two names agreeing in their first N bytes share an answer. */
typedef struct CpEntry {
    lxb_dom_element_t *el;      /* NULL is empty; nothing is ever removed, so there are no tombstones */
    char              *name;    /* owned, and never NULL in an occupied slot */
    /* The cascaded value, owned. NULL is one of the answers: "if the output of the cascade is an empty list,
       there is no cascaded value" (css-cascade-5 §4.2), which css-cascade-5 §7 "Defaulting" is written for. */
    char              *value;
    char              *computed;      /* the computed value, owned */
    bool               has_value;
    bool               has_computed;
} CpEntry;

/* The census: lifetime counters of this agent, raised at the question and at its one outcome. */
static long long g_asks;
static long long g_served;
static long long g_resolved;
static long long g_computed_asks;
static long long g_computed_served;
static long long g_computed_resolved;
static long long g_match_asks;
static long long g_match_served;
static long long g_match_built;

/* The initial capacity, which decides only when the table is rebuilt at twice the size and never which
   questions are answered; a document that asks for more gets a bigger table, without limit. */
#define CP_CAP0 ((size_t)256)

/* An (element, name) slot: the name folded in with FNV-1a and the element's address, whose low bits are all
   alignment, mixed with splitmix64's finalizer so both halves reach the high bits. */
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

/* The probe, shared by the read and the write so a key is never looked up one way and stored another.
   Answers the slot the key occupies, or the first empty slot at which it would be inserted. */
static size_t cp_probe(CpEntry *tab, size_t cap, const lxb_dom_element_t *el, const char *name)
{
    size_t i = cp_slot(el, name, cap);

    while (tab[i].el != NULL && !(tab[i].el == el && strcmp(tab[i].name, name) == 0))
        i = (i + 1) & (cap - 1);
    return i;
}

static void cp_grow(CssCascadeRecord *r)
{
    size_t cap = r->cap ? r->cap * 2 : CP_CAP0, i;
    CpEntry *tab = calloc(cap, sizeof *tab);

    CHECK(tab != NULL, "css-cascade-pass-oom: css-cascade-5 §4.2's cascaded-value record could not be grown");
    for (i = 0; i < r->cap; i++)
        if (r->tab[i].el != NULL) tab[cp_probe(tab, cap, r->tab[i].el, r->tab[i].name)] = r->tab[i];
    free(r->tab);
    r->tab = tab;
    r->cap = cap;
}

/* Claim slot `i` for (el, name) when it is empty; a slot already holding the pair keeps its other fact. */
static void cp_occupy(CssCascadeRecord *r, size_t i, lxb_dom_element_t *el, const char *name)
{
    if (r->tab[i].el != NULL) return;
    r->tab[i].el = el;
    r->tab[i].name = strdup(name);
    CHECK(r->tab[i].name != NULL, "css-cascade-pass-oom: a property name could not be copied into "
                                  "css-cascade-5 §4.2's record");
    r->used++;
}

void css_cascade_record_release(CssCascadeRecord *r)
{
    size_t i;

    for (i = 0; i < r->cap; i++) {
        free(r->tab[i].name);
        free(r->tab[i].value);
        free(r->tab[i].computed);
    }
    free(r->tab);
    r->tab = NULL;
    r->cap = 0;
    r->used = 0;
    /* A held match result borrows a held sheet set's layer nodes, so the results go first. */
    for (i = 0; i < r->match_cap; i++)
        if (r->match[i] != NULL) css_match_result_free(r->match[i]);
    free(r->match);
    r->match = NULL;
    r->match_cap = 0;
    r->match_used = 0;
    for (i = 0; i < r->style_cap; i++)
        if (r->style[i] != NULL) css_computed_style_free(r->style[i]);
    free(r->style);
    r->style = NULL;
    r->style_cap = 0;
    r->style_used = 0;
    for (i = 0; i < r->author_n; i++) css_author_sheets_free(r->author[i]);
    free(r->author);
    r->author = NULL;
    r->author_n = 0;
    r->author_cap = 0;
}

bool css_cascade_pass_author_ask(const lxb_dom_node_t *root, struct CssAuthorSheets **out)
{
    CssCascadeRecord *r = layout_job_cascade();
    size_t i;

    DCHECK(root != NULL && out != NULL, "an author sheet set was asked for no root or with nowhere to put it");
    if (r == NULL) return false;
    for (i = 0; i < r->author_n; i++) {
        if (css_author_sheets_root(r->author[i]) != root) continue;
        *out = r->author[i];
        return true;
    }
    return false;
}

bool css_cascade_pass_author_hold(struct CssAuthorSheets *sheets)
{
    CssCascadeRecord *r = layout_job_cascade();
    size_t i;

    DCHECK(sheets != NULL, "an author sheet set was offered to the record as nothing");
    if (r == NULL) return false;
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
    return true;
}

/* An element's home slot in a per-element table: its address, whose low bits are all alignment, through
   splitmix64's finalizer. */
static size_t cp_element_slot(const lxb_dom_element_t *el, size_t cap)
{
    uint64_t h = (uint64_t)(uintptr_t)el;

    h ^= h >> 30; h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27; h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return (size_t)h & (cap - 1);
}

/* The slot holding `el`'s match result or the first empty one, for the read and the write alike. */
static size_t cp_match_probe(struct CssMatchResult **tab, size_t cap, const lxb_dom_element_t *el)
{
    size_t i = cp_element_slot(el, cap);

    while (tab[i] != NULL && css_match_result_element(tab[i]) != el) i = (i + 1) & (cap - 1);
    return i;
}

static void cp_match_grow(CssCascadeRecord *r)
{
    size_t cap = r->match_cap ? r->match_cap * 2 : CP_CAP0, i;
    struct CssMatchResult **tab = calloc(cap, sizeof *tab);

    CHECK(tab != NULL, "css-cascade-pass-oom: the record's match results could not be grown");
    for (i = 0; i < r->match_cap; i++)
        if (r->match[i] != NULL)
            tab[cp_match_probe(tab, cap, css_match_result_element(r->match[i]))] = r->match[i];
    free(r->match);
    r->match = tab;
    r->match_cap = cap;
}

bool css_cascade_pass_match_ask(lxb_dom_element_t *el, const struct CssAuthorSheets *sheets,
                                struct CssMatchResult **out)
{
    CssCascadeRecord *r = layout_job_cascade();
    size_t i;

    DCHECK(el != NULL && sheets != NULL && out != NULL,
           "a match result was asked of the record for no element, under no sheet set or with nowhere to put it");
    g_match_asks++;
    if (r == NULL || r->match_cap == 0) return false;
    i = cp_match_probe(r->match, r->match_cap, el);
    if (r->match[i] == NULL) return false;
    /* The record holds one sheet set per root and no tree write moves an element between roots while it is
       held, so the set this result was matched under is the one its root resolves under now. */
    DCHECK(css_match_result_sheets(r->match[i]) == sheets,
           "the record served an element's match result built under a different sheet set than the one its "
           "root now resolves under — the set a held result borrows was replaced or the element changed roots "
           "while the record was held");
    *out = r->match[i];
    g_match_served++;
    return true;
}

bool css_cascade_pass_match_hold(struct CssMatchResult *m)
{
    CssCascadeRecord *r = layout_job_cascade();
    const struct CssAuthorSheets *sheets;
    bool set_held = false;
    size_t i;

    DCHECK(m != NULL, "a match result was offered to the record as nothing");
    g_match_built++;
    if (r == NULL) return false;
    sheets = css_match_result_sheets(m);
    for (i = 0; i < r->author_n; i++)
        if (r->author[i] == sheets) set_held = true;
    DCHECK(set_held,
           "a match result was offered to the record under a sheet set the record does not hold — its layer "
           "nodes would be freed with that set at the end of the one resolution while the result is served on");
    if (r->match_used * 2 >= r->match_cap) cp_match_grow(r);
    i = cp_match_probe(r->match, r->match_cap, css_match_result_element(m));
    DCHECK(r->match[i] == NULL,
           "a second match result was offered for an element the record already holds one for — its one "
           "caller asks before it builds");
    r->match[i] = m;
    r->match_used++;
    return true;
}

/* The same for the computed style table. */
static size_t cp_style_probe(struct CssComputedStyle **tab, size_t cap, const lxb_dom_element_t *el)
{
    size_t i = cp_element_slot(el, cap);

    while (tab[i] != NULL && css_computed_style_element(tab[i]) != el) i = (i + 1) & (cap - 1);
    return i;
}

static void cp_style_grow(CssCascadeRecord *r)
{
    size_t cap = r->style_cap ? r->style_cap * 2 : CP_CAP0, i;
    struct CssComputedStyle **tab = calloc(cap, sizeof *tab);

    CHECK(tab != NULL, "css-cascade-pass-oom: the record's computed style table could not be grown");
    for (i = 0; i < r->style_cap; i++)
        if (r->style[i] != NULL)
            tab[cp_style_probe(tab, cap, css_computed_style_element(r->style[i]))] = r->style[i];
    free(r->style);
    r->style = tab;
    r->style_cap = cap;
}

struct CssComputedStyle *css_cascade_pass_style(lxb_dom_element_t *el)
{
    CssCascadeRecord *r = layout_job_cascade();
    size_t i;

    DCHECK(el != NULL, "a computed style record was asked of the cascade record for no element");
    if (r == NULL) return NULL;
    if (r->style_used * 2 >= r->style_cap) cp_style_grow(r);
    i = cp_style_probe(r->style, r->style_cap, el);
    if (r->style[i] == NULL) {
        r->style[i] = css_computed_style_create(el);
        r->style_used++;
    }
    return r->style[i];
}

bool css_cascade_record_is_held(void) { return layout_job_cascade() != NULL; }

bool css_cascade_pass_ask(lxb_dom_element_t *el, const char *name, char **out)
{
    CssCascadeRecord *r = layout_job_cascade();
    size_t i;

    DCHECK(el != NULL && name != NULL,
           "css-cascade-5 §4.2's cascaded-value record was asked with no element or no property name. The "
           "cascade is a function of both — css-cascade-5 §6 \"Cascading\" sorts \"declared values for a "
           "given property on a given element\" — so a key missing one names no question");
    DCHECK(out != NULL,
           "css-cascade-5 §4.2's cascaded-value record was asked with nowhere to put the answer");
    g_asks++;
    if (r == NULL || r->cap == 0) return false;
    i = cp_probe(r->tab, r->cap, el, name);
    if (r->tab[i].el == NULL || !r->tab[i].has_value) return false;
    /* The copy is the contract: the cascade's own answer is owned by its caller, who frees it. */
    if (r->tab[i].value == NULL) {
        *out = NULL;
    } else {
        *out = strdup(r->tab[i].value);
        CHECK(*out != NULL, "css-cascade-pass-oom: a recorded cascaded value could not be copied for its "
                            "caller, who owns and frees what the cascade hands back");
    }
    g_served++;
    return true;
}

void css_cascade_pass_record(lxb_dom_element_t *el, const char *name, const char *value)
{
    CssCascadeRecord *r = layout_job_cascade();
    size_t i;

    DCHECK(el != NULL && name != NULL,
           "css-cascade-5 §4.2's cascaded-value record was handed an answer with no element or no property "
           "name to file it under");
    g_resolved++;
    if (r == NULL) return;
    if (r->used * 2 >= r->cap) cp_grow(r);
    i = cp_probe(r->tab, r->cap, el, name);
    /* One answer per key: the one caller asks before it resolves and resolves only on a miss, so a key
       already here is a probe that looked one way and stored another. */
    DCHECK(r->tab[i].el == NULL || !r->tab[i].has_value,
           "css-cascade-5 §4.2's cascaded-value record was handed a SECOND answer for a pair it already "
           "holds. Its one caller asks before it resolves the cascade and resolves only when the ask came "
           "back empty, so the ask and the record disagreed about which slot the pair occupies, and every "
           "answer served since is filed under some other question");
    cp_occupy(r, i, el, name);
    r->tab[i].has_value = true;
    if (value != NULL) {
        r->tab[i].value = strdup(value);
        CHECK(r->tab[i].value != NULL, "css-cascade-pass-oom: a cascaded value could not be copied into "
                                       "css-cascade-5 §4.2's record");
    }
}

bool css_cascade_pass_computed_ask(lxb_dom_element_t *el, const char *name, char **out)
{
    CssCascadeRecord *r = layout_job_cascade();
    size_t i;

    DCHECK(el != NULL && name != NULL && out != NULL,
           "a computed value was asked of the record with no element, no property name or nowhere to put it");
    g_computed_asks++;
    if (r == NULL || r->cap == 0) return false;
    i = cp_probe(r->tab, r->cap, el, name);
    if (r->tab[i].el == NULL || !r->tab[i].has_computed) return false;
    if (r->tab[i].computed == NULL) {
        *out = NULL;
    } else {
        *out = strdup(r->tab[i].computed);
        CHECK(*out != NULL, "css-cascade-pass-oom: a recorded computed value could not be copied for its "
                            "caller, who owns and frees what css_computed_value hands back");
    }
    g_computed_served++;
    return true;
}

void css_cascade_pass_computed_record(lxb_dom_element_t *el, const char *name, const char *value)
{
    CssCascadeRecord *r = layout_job_cascade();
    size_t i;

    DCHECK(el != NULL && name != NULL,
           "a computed value was handed to the record with no element or no property name to file it under");
    g_computed_resolved++;
    if (r == NULL) return;
    if (r->used * 2 >= r->cap) cp_grow(r);
    i = cp_probe(r->tab, r->cap, el, name);
    DCHECK(r->tab[i].el == NULL || !r->tab[i].has_computed,
           "the record was handed a SECOND computed value for a pair it already holds. css_computed_value asks "
           "before it computes and computes only on a miss, so the ask and the record disagreed about which "
           "slot the pair occupies");
    cp_occupy(r, i, el, name);
    r->tab[i].has_computed = true;
    if (value != NULL) {
        r->tab[i].computed = strdup(value);
        CHECK(r->tab[i].computed != NULL, "css-cascade-pass-oom: a computed value could not be copied into "
                                          "the record");
    }
}

void css_cascade_pass_census(CssCascadePassCensus *out)
{
    DCHECK(out != NULL, "css-cascade-5 §4.2's cascaded-value census was asked for with nowhere to write it");
    /* Every ask has exactly one outcome — the record answered it, or its one caller ran the cascade and
       reported the answer with no return between the two — so the counts close. */
    DCHECKF(g_asks == g_served + g_resolved,
            "css-cascade-5 §4.2's cascaded-value census does not close: %lld asks against %lld served and "
            "%lld resolved. A gap is an arm that took an answer without saying which outcome it was, and "
            "every share a reader divides out of these rows is over a denominator that is not the population",
            g_asks, g_served, g_resolved);
    DCHECKF(g_computed_asks == g_computed_served + g_computed_resolved,
            "the computed-value census does not close: %lld asks against %lld served and %lld resolved. "
            "css_computed_value asks the record and on a miss computes and records with no return between, so "
            "a gap is an arm that answered without saying which outcome it was",
            g_computed_asks, g_computed_served, g_computed_resolved);
    DCHECKF(g_match_asks == g_match_served + g_match_built,
            "the match-result census does not close: %lld asks against %lld served and %lld built. "
            "cssom_cascaded_value asks the record and on a miss builds and offers the result with no return "
            "between, so a gap is an arm that matched without saying which outcome it was",
            g_match_asks, g_match_served, g_match_built);
    out->asks_life = g_asks;
    out->served_life = g_served;
    out->resolved_life = g_resolved;
    out->computed_asks_life = g_computed_asks;
    out->computed_served_life = g_computed_served;
    out->computed_resolved_life = g_computed_resolved;
}
