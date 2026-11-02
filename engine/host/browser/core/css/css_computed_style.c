/* One element's computed values — see core/css/css_computed_style.h. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_cascade_pass.h"
#include "core/css/css_computed_style.h"
#include "core/css/css_property_id.h"
#include "core/layout/layout_job.h"
#include <lexbor/core/base.h>   /* lexbor_malloc: the allocator the engine gives its refusal edge */

/* The values are packed in id order, one per id whose bit `held` sets: layout asks an element for the few dozen
   properties it lays out, so a slot per id (css-cascade-5 §4.4 gives every property a computed value) would be
   mostly empty in every record a job holds. `deriving` marks an id asked and not yet answered, which holds no
   value. A value's slot is the count of held ids below its own. */
#define CCS_WORDS (((unsigned)CSS_PROPERTY_COUNT + 63u) / 64u)

struct CssComputedStyle {
    lxb_dom_element_t *el;
    uint64_t           held[CCS_WORDS];
    uint64_t           deriving[CCS_WORDS];
    CssComputedValue  *value;   /* `n` values in id order, room for `cap`, owned */
    unsigned           n, cap;
};

static long long g_asks;
static long long g_served;
static long long g_derived;

static bool ccs_bit(const uint64_t *w, unsigned id)
{
    return (w[id >> 6] >> (id & 63u)) & 1u;
}

static unsigned ccs_slot(const CssComputedStyle *s, unsigned id)
{
    unsigned k, r = 0;

    for (k = 0; k < (id >> 6); k++) r += (unsigned)__builtin_popcountll(s->held[k]);
    return r + (unsigned)__builtin_popcountll(s->held[id >> 6] & ((UINT64_C(1) << (id & 63u)) - 1u));
}

CssComputedStyle *css_computed_style_create(lxb_dom_element_t *el)
{
    CssComputedStyle *s;

    DCHECK(el != NULL, "a computed style record was created for no element");
    s = lexbor_calloc(1, sizeof *s);
    CHECK(s != NULL, "css-computed-style-oom: an element's computed style record could not be allocated");
    s->el = el;
    return s;
}

void css_computed_style_free(CssComputedStyle *s)
{
    unsigned i, k = 0;

    DCHECK(s != NULL, "a computed style record was freed through nothing");
    for (i = 0; i < CSS_PROPERTY_COUNT; i++) {
        DCHECKF(!ccs_bit(s->deriving, i),
                "`%s` was being derived when its element's computed style record was released — the record "
                "goes with its job, and an ask's derivation answers inside the hold that asked it",
                css_property_name((CssPropertyId)i));
        if (!ccs_bit(s->held, i)) continue;
        if (css_property_shape((CssPropertyId)i) == CSS_COMPUTED_SHAPE_TEXT) free(s->value[k].text);
        k++;
    }
    DCHECKF(k == s->n, "a computed style record holds %u values against %u held ids", s->n, k);
    free(s->value);
    free(s);
}

lxb_dom_element_t *css_computed_style_element(const CssComputedStyle *s)
{
    return s->el;
}

bool css_computed_style_ask(lxb_dom_element_t *el, CssPropertyId id, CssComputedValue *out)
{
    CssComputedStyle *s = css_cascade_pass_style(el);

    DCHECK(s != NULL, "a computed value was asked outside a computed style hold, so no record could hold it");
    DCHECKF((unsigned)id < CSS_PROPERTY_COUNT, "a computed value was asked for id %u, which is no row",
            (unsigned)id);
    DCHECKF(!ccs_bit(s->deriving, id),
            "`%s`'s computed value was asked while it is being derived on the same element: its derivation "
            "depends on itself. css-cascade-5 §7.2's inheritance reads the parent and css-values-4 §6.1.1's "
            "font-relative units inside a font-affecting property read the parent, so a cycle is a rule that "
            "reads this element where the spec says another",
            css_property_name(id));
    g_asks++;
    if (ccs_bit(s->held, id)) {
        *out = s->value[ccs_slot(s, id)];
        g_served++;
        return true;
    }
    s->deriving[id >> 6] |= UINT64_C(1) << (id & 63u);
    return false;
}

void css_computed_style_answer(lxb_dom_element_t *el, CssPropertyId id, const CssComputedValue *v)
{
    CssComputedStyle *s = css_cascade_pass_style(el);
    unsigned at;

    DCHECK(s != NULL && v != NULL, "a computed value was answered outside a hold or as nothing");
    DCHECKF(ccs_bit(s->deriving, id) && !ccs_bit(s->held, id),
            "`%s`'s computed value was answered without an ask that came back empty — its one caller asks "
            "before it derives",
            css_property_name(id));
    if (s->n == s->cap) {
        unsigned cap = s->cap != 0 ? s->cap * 2 : 16;
        CssComputedValue *grown = lexbor_realloc(s->value, cap * sizeof *grown);

        CHECK(grown != NULL, "css-computed-style-oom: an element's computed values could not grow");
        s->value = grown;
        s->cap = cap;
    }
    at = ccs_slot(s, id);
    memmove(&s->value[at + 1], &s->value[at], (s->n - at) * sizeof *s->value);
    s->value[at] = *v;
    s->n++;
    s->deriving[id >> 6] &= ~(UINT64_C(1) << (id & 63u));
    s->held[id >> 6] |= UINT64_C(1) << (id & 63u);
    g_derived++;
}

bool css_computed_style_holds(lxb_dom_element_t *el, CssPropertyId id)
{
    CssComputedStyle *s = css_cascade_pass_style(el);

    DCHECK(s != NULL, "a computed value's presence was read outside a computed style hold");
    DCHECKF((unsigned)id < CSS_PROPERTY_COUNT, "a computed value's presence was read for id %u, which is no row",
            (unsigned)id);
    return ccs_bit(s->held, id);
}

CssComputedValue css_computed_style_held(lxb_dom_element_t *el, CssPropertyId id)
{
    CssComputedStyle *s = css_cascade_pass_style(el);

    CHECK(s != NULL && (unsigned)id < CSS_PROPERTY_COUNT && ccs_bit(s->held, id),
          "a held computed value was read off a record that does not hold it — the ask's entry derives the "
          "element's ancestors in tree order before the element, so a parent it reads is held");
    return s->value[ccs_slot(s, id)];
}

void css_computed_style_census(long long *asks, long long *served, long long *derived)
{
    DCHECKF(g_asks == g_served + g_derived,
            "the computed style census does not close: %lld asks against %lld served and %lld derived. Every "
            "ask is answered by its record or derived and answered with no return between, so a gap is an arm "
            "that derived without answering", g_asks, g_served, g_derived);
    *asks = g_asks;
    *served = g_served;
    *derived = g_derived;
}
