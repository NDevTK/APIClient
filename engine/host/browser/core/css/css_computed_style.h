/* One element's computed values (css-cascade-5 §4.4 "Computed Values"), typed per its property's `Computed
 * value:` line and indexed by core/css/css_property_id.h's id. Blink's ComputedStyle under its own name.
 *
 * A layout job holds one per element in its cascade record (core/css/css_cascade_pass.h states why the
 * cascade's inputs do not move while it does), so css-cascade-5 §7.2 "Inheritance" reads the parent's held
 * value instead of climbing, and each (element, property) is derived once per job. The record is re-derivable:
 * it is released with the job, and a forked job starts without one. Every ask is made while a job holds a
 * record (css_computed_style_require_record), so no answer is derived outside one. A value is derived on its first
 * ask, from the element's cascaded style (core/css/css_cascaded_style.h), which is resolved once per element. */

#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_COMPUTED_STYLE_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_COMPUTED_STYLE_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_computed_value.h"
#include "core/css/css_property_id.h"
#include "core/layout/layout_job.h"

/* One computed value; the arm is css_property_shape(id)'s. */
typedef union {
    char            *text;            /* CSS_COMPUTED_SHAPE_TEXT; the record owns it, and NULL is an answer */
    CssLength        length;          /* CSS_COMPUTED_SHAPE_LENGTH */
    CssLineHeight    line_height;     /* CSS_COMPUTED_SHAPE_LINE_HEIGHT */
    CssBorderSpacing border_spacing;  /* CSS_COMPUTED_SHAPE_BORDER_SPACING */
    CssKeyword       keyword;         /* CSS_COMPUTED_SHAPE_KEYWORD */
    CssDisplay       display;         /* CSS_COMPUTED_SHAPE_DISPLAY */
    CssAlign         align;           /* CSS_COMPUTED_SHAPE_ALIGN */
    CssTabSize       tab_size;        /* CSS_COMPUTED_SHAPE_TAB_SIZE */
} CssComputedValue;

typedef struct CssComputedStyle CssComputedStyle;

/* An empty record for `el`, and its release. Owned by the cascade record that holds it. */
CssComputedStyle *css_computed_style_create(lxb_dom_element_t *el);
void css_computed_style_free(CssComputedStyle *s);
lxb_dom_element_t *css_computed_style_element(const CssComputedStyle *s);

/* Every computed value's entry requires a cascade record to be held: a step machine's entered layout job. A read
   with none is a caller that is not yet a step machine owning a job. Always fatal, because the record is what
   the ask's derivation dereferences. Expanded at the entry, so the crash names it. */
#define css_computed_style_require_record() \
    CHECK(css_cascade_record_is_held(), \
          "a computed value was asked with no layout job holding a cascade record. Its caller is a layout or " \
          "cascade reader that is not a step machine owning a job; BUILD it as one (enter the job before the read), " \
          "as CSSOM VIEW §6's members in core/dom/element_view.c do")

/* Inside a hold: answers true and writes `*out` when `el`'s record holds `id`; a text arm is borrowed until the
   record is released. False marks the pair as being derived, and its one caller derives it and reports it
   with `answer` with no return between the two, which takes a text arm's ownership; asking a pair already
   being derived is a derivation that depends on itself, and crashes. */
bool css_computed_style_ask(lxb_dom_element_t *el, CssPropertyId id, CssComputedValue *out);
void css_computed_style_answer(lxb_dom_element_t *el, CssPropertyId id, const CssComputedValue *v);

/* Inside a hold: whether `el`'s record holds `id`, read without marking the pair as being derived, so it is not
   an ask and the census does not count it. An ask's entry reads it to find the ancestors it must derive first,
   in tree order, so css-cascade-5 §7.2's inheritance reads a held parent rather than climbing. */
bool css_computed_style_holds(lxb_dom_element_t *el, CssPropertyId id);
/* Inside a hold: `el`'s value for `id`, which its record must hold (CHECKed, since a value read off a record
   that holds none would be a zero standing for the parent's computed value); a text arm is borrowed. */
CssComputedValue css_computed_style_held(lxb_dom_element_t *el, CssPropertyId id);

/* Lifetime counters of this agent: asks, those a record answered, and those derived. They close. */
void css_computed_style_census(long long *asks, long long *served, long long *derived);

#endif /* ENGINE_HOST_BROWSER_CORE_CSS_CSS_COMPUTED_STYLE_H */
