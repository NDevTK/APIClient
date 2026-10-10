/* One element's cascaded values (css-cascade-5 §4.2 "Cascaded Values") for every property any origin declares
 * on it, from one collection of its declarations: the author origin's match result, its style attribute, the
 * presentational hint origin and the UA origin. Blink's StyleCascade under its own name.
 *
 * Resolution runs once per element and in two phases, because css-logical-1 §4 "Flow-Relative Box Model
 * Properties" pairs a group's physical and flow-relative members "using the element's own computed writing
 * mode" and requires `writing-mode`, `direction` and `text-orientation` "be computed as a prerequisite". Those
 * three are in no group, so phase one resolves every property in no group when the collection closes, and
 * phase two, at the first ask of a grouped property, resolves each group pair by cascading both members'
 * declarations together, asking the pairing of this element's phase-one values. Phase two waits for that ask
 * because the first ask of an element may be its own `writing-mode`'s derivation, which phase two would need.
 * A layout job's cascade record holds one per element (core/css/css_cascade_pass.h states why its inputs do not
 * move while it does). */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_CASCADED_STYLE_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_CASCADED_STYLE_H

#include <stdbool.h>
#include <stdint.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_cascade.h"
#include "core/css/css_layer_order.h"

typedef struct CssCascadedStyle CssCascadedStyle;

/* An open collection for `el`, sorted under `layers`, both borrowed: the layer order outlives the record. */
CssCascadedStyle *css_cascaded_style_create(lxb_dom_element_t *el, CssLayerOrder *layers);
void css_cascaded_style_free(CssCascadedStyle *s);
lxb_dom_element_t *css_cascaded_style_element(const CssCascadedStyle *s);

/* One declaration of the longhand `name`, as css_cascade_add takes it; `name` and `value` are copied. Only
   before `resolve`. The hint and UA origins declare a property at most once each. */
void css_cascaded_style_add(CssCascadedStyle *s, const char *name, CssOrigin origin, bool important,
                            bool element_attached, const CssLayerNode *layer, uint32_t specificity, uint32_t seq,
                            uint32_t decl_order, const char *value);

/* Close the collection and resolve every declared property in no logical group. The record must already be
   findable by its element: phase two's pairing asks this element's phase-one values back. */
void css_cascaded_style_resolve(CssCascadedStyle *s);

/* The cascaded value of `name`, borrowed until the record is released, or NULL when the cascade's output is
   empty — no origin declares the property or its group partner, or §7.3's roll-backs removed every
   declaration. The first grouped ask runs phase two; a grouped ask during phase two crashes, its answer not
   yet whole. */
const char *css_cascaded_style_value(CssCascadedStyle *s, const char *name);

#endif /* ENGINE_HOST_BROWSER_CORE_CSS_CSS_CASCADED_STYLE_H */
