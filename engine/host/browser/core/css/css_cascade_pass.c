/* css-cascade-5 §4.2 "Cascaded Values"' answer for one (element, property), held by a layout job — see
   core/css/css_cascade_pass.h. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_cascade_pass.h"
#include "core/layout/layout_job.h"   /* the record of the job whose activation holds the thread */

/* One entry per (element, property). The name is copied whole: a custom property's name (css-variables-1
   §2's `--*`) arrives on the stack of the frame that composed it, so a stored pointer would answer for freed
   bytes, and a fixed-size copy would let two names agreeing in their first N bytes share an answer. */
typedef struct CpEntry {
    lxb_dom_element_t *el;      /* NULL is empty; nothing is ever removed, so there are no tombstones */
    char              *name;    /* owned, and never NULL in an occupied slot */
    /* The cascaded value, owned. NULL is one of the answers: "if the output of the cascade is an empty list,
       there is no cascaded value" (css-cascade-5 §4.2), which css-cascade-5 §7 "Defaulting" is written for.
       Occupancy is `el != NULL`, never a flag beside this pointer. */
    char              *value;
} CpEntry;

/* The census: lifetime counters of this agent, raised at the question and at its one outcome. */
static long long g_asks;
static long long g_served;
static long long g_resolved;

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

void css_cascade_record_release(CssCascadeRecord *r)
{
    size_t i;

    for (i = 0; i < r->cap; i++) {
        free(r->tab[i].name);
        free(r->tab[i].value);
    }
    free(r->tab);
    r->tab = NULL;
    r->cap = 0;
    r->used = 0;
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
    if (r->tab[i].el == NULL) return false;
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
    DCHECK(r->tab[i].el == NULL,
           "css-cascade-5 §4.2's cascaded-value record was handed a SECOND answer for a pair it already "
           "holds. Its one caller asks before it resolves the cascade and resolves only when the ask came "
           "back empty, so the ask and the record disagreed about which slot the pair occupies, and every "
           "answer served since is filed under some other question");
    r->tab[i].el = el;
    r->tab[i].name = strdup(name);
    CHECK(r->tab[i].name != NULL, "css-cascade-pass-oom: a property name could not be copied into "
                                  "css-cascade-5 §4.2's record");
    if (value != NULL) {
        r->tab[i].value = strdup(value);
        CHECK(r->tab[i].value != NULL, "css-cascade-pass-oom: a cascaded value could not be copied into "
                                       "css-cascade-5 §4.2's record");
    }
    r->used++;
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
    out->asks_life = g_asks;
    out->served_life = g_served;
    out->resolved_life = g_resolved;
}
