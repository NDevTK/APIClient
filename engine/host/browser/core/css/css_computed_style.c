/* One element's computed values — see core/css/css_computed_style.h. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_cascade_pass.h"
#include "core/css/css_computed_style.h"
#include "core/css/css_property_id.h"
#include "core/layout/layout_job.h"

typedef enum {
    CSS_COMPUTED_ABSENT = 0,
    CSS_COMPUTED_DERIVING,   /* asked and not yet answered */
    CSS_COMPUTED_HELD
} CssComputedState;

struct CssComputedStyle {
    lxb_dom_element_t *el;
    uint8_t            state[CSS_PROPERTY_COUNT];   /* a CssComputedState per id */
    CssComputedValue   value[CSS_PROPERTY_COUNT];   /* meaningful where `state` is HELD */
};

static long long g_asks;
static long long g_served;
static long long g_derived;

CssComputedStyle *css_computed_style_create(lxb_dom_element_t *el)
{
    CssComputedStyle *s;

    DCHECK(el != NULL, "a computed style record was created for no element");
    s = calloc(1, sizeof *s);
    CHECK(s != NULL, "css-computed-style-oom: an element's computed style record could not be allocated");
    s->el = el;
    return s;
}

void css_computed_style_free(CssComputedStyle *s)
{
    unsigned i;

    DCHECK(s != NULL, "a computed style record was freed through nothing");
    for (i = 0; i < CSS_PROPERTY_COUNT; i++)
        DCHECKF(s->state[i] != CSS_COMPUTED_DERIVING,
                "`%s` was being derived when its element's computed style record was released — the record "
                "goes with its job, and an ask's derivation answers inside the hold that asked it",
                css_property_name((CssPropertyId)i));
    for (i = 0; i < CSS_PROPERTY_COUNT; i++)
        if (s->state[i] == CSS_COMPUTED_HELD && css_property_shape((CssPropertyId)i) == CSS_COMPUTED_SHAPE_TEXT)
            free(s->value[i].text);
    free(s);
}

lxb_dom_element_t *css_computed_style_element(const CssComputedStyle *s)
{
    return s->el;
}

void css_computed_style_hold_begin(CssComputedStyleHold *h)
{
    h->opened = !css_cascade_record_is_held();
    if (h->opened) layout_job_drive_begin(&h->drive);
    DCHECK(css_cascade_record_is_held(),
           "a computed style hold opened a drive and still no cascade record is held — a drive on the "
           "synchronous job is what makes its record the held one");
}

void css_computed_style_hold_end(CssComputedStyleHold *h)
{
    DCHECK(css_cascade_record_is_held(), "a computed style hold ended with no cascade record held");
    if (h->opened) layout_job_drive_end(&h->drive);
}

bool css_computed_style_ask(lxb_dom_element_t *el, CssPropertyId id, CssComputedValue *out)
{
    CssComputedStyle *s = css_cascade_pass_style(el);

    DCHECK(s != NULL, "a computed value was asked outside a computed style hold, so no record could hold it");
    DCHECKF((unsigned)id < CSS_PROPERTY_COUNT, "a computed value was asked for id %u, which is no row",
            (unsigned)id);
    DCHECKF(s->state[id] != CSS_COMPUTED_DERIVING,
            "`%s`'s computed value was asked while it is being derived on the same element: its derivation "
            "depends on itself. css-cascade-5 §7.2's inheritance reads the parent and css-values-4 §6.1.1's "
            "font-relative units inside a font-affecting property read the parent, so a cycle is a rule that "
            "reads this element where the spec says another",
            css_property_name(id));
    g_asks++;
    if (s->state[id] == CSS_COMPUTED_HELD) {
        *out = s->value[id];
        g_served++;
        return true;
    }
    s->state[id] = CSS_COMPUTED_DERIVING;
    return false;
}

void css_computed_style_answer(lxb_dom_element_t *el, CssPropertyId id, const CssComputedValue *v)
{
    CssComputedStyle *s = css_cascade_pass_style(el);

    DCHECK(s != NULL && v != NULL, "a computed value was answered outside a hold or as nothing");
    DCHECKF(s->state[id] == CSS_COMPUTED_DERIVING,
            "`%s`'s computed value was answered without an ask that came back empty — its one caller asks "
            "before it derives",
            css_property_name(id));
    s->value[id] = *v;
    s->state[id] = CSS_COMPUTED_HELD;
    g_derived++;
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
