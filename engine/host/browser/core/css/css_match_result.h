/* The author origin's declarations that apply to one element: every longhand declaration of every style rule in
 * its root's sheet set (core/css/css_author_sheets.h) whose selector matches it, each with what css-cascade-5
 * §6.1 "Cascade Sorting Order" sorts by. It is Blink's MatchResult under its own name.
 *
 * Matching is a function of the element, its tree and the sheet set, and not of the property being resolved,
 * so a layout job's record holds one per element (core/css/css_cascade_pass.h states why those inputs do not
 * move while it does), and the element's cascaded style (core/css/css_cascaded_style.h) is collected from it
 * once. The builder is in core/css/css_style_declaration.c, which owns the parse the selectors and declaration
 * blocks come from. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_MATCH_RESULT_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_MATCH_RESULT_H

#include <stdbool.h>
#include <stdint.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_author_sheets.h"
#include "core/css/css_layer_order.h"

/* One declaration, as css_cascade_add takes it. `seq` is the rule's position in the set's emission counted
   across sheets (css-cascade-5 §6.1's Order of Appearance, and the rule identity §7.3.6 `revert-rule` reads);
   `decl_order` is the declaration's index in its rule's collapsed block. */
typedef struct {
    char               *name;         /* owned; a longhand */
    char               *value;        /* owned; never NULL */
    bool                important;
    const CssLayerNode *layer;        /* borrowed from the sheet set's order, which outlives the result */
    uint32_t            specificity;
    uint32_t            seq;
    uint32_t            decl_order;
} CssMatchedDeclaration;

typedef struct CssMatchResult CssMatchResult;

/* An open result for `el` under `sheets`, both borrowed: the sheet set outlives the result. */
CssMatchResult *css_match_result_create(lxb_dom_element_t *el, const CssAuthorSheets *sheets);
void css_match_result_free(CssMatchResult *m);

/* Append one declaration; `name` and `value` are copied. Only before `finish`; `seq` never decreases. */
void css_match_result_add(CssMatchResult *m, const char *name, const char *value, bool important,
                          const CssLayerNode *layer, uint32_t specificity, uint32_t seq, uint32_t decl_order);

/* Close the result. `span` is how many rules the set emitted: every `seq` is below it, and the cascade's Order
   of Appearance continues from it after the author origin. The readers below need a closed result. */
void css_match_result_finish(CssMatchResult *m, uint32_t span);

lxb_dom_element_t *css_match_result_element(const CssMatchResult *m);
const CssAuthorSheets *css_match_result_sheets(const CssMatchResult *m);
uint32_t css_match_result_span(const CssMatchResult *m);
uint32_t css_match_result_count(const CssMatchResult *m);
const CssMatchedDeclaration *css_match_result_at(const CssMatchResult *m, uint32_t i);

#endif /* ENGINE_HOST_BROWSER_CORE_CSS_CSS_MATCH_RESULT_H */
