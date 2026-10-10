/* css-cascade-5 §6.2 "Cascading Origins"' author origin as one root's style sheets present it to the cascade:
 * every sheet of CSSOM §6.2 "CSS Style Sheet Collections" that applies, emitted for the matcher by
 * core/css/css_rule.h's `css_rule_cascade_sheet`, and the css-cascade-5 §6.4.3 "Layer Ordering" order their
 * walk declares, sealed once the last sheet is emitted.
 *
 * The set is a function of the root's sheet objects, their rules and the realm's viewport (a conditional group
 * rule's condition and a sheet's media), and of nothing about the element being resolved. A layout job holds
 * one per root it resolves under (core/css/css_cascade_pass.h states why those inputs do not move while it
 * does). */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_AUTHOR_SHEETS_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_AUTHOR_SHEETS_H

#include <stdint.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_layer_order.h"
#include "core/css/css_rule.h"

/* One applying sheet: `si` is its position in the root's sheet list, and `view` its emission (owned by the
   set; `view.text` is never NULL, since a sheet that emits no rule contributes only layer declarations). */
typedef struct {
    uint32_t            si;
    CssRuleCascadeSheet view;
} CssAuthorSheet;

typedef struct CssAuthorSheets CssAuthorSheets;

/* Build the set for `root` (a document or a shadow root), in its document's realm. A sheet that is disabled,
   whose media does not match, or whose stored text cannot be read back contributes nothing. The caller owns
   the result. */
CssAuthorSheets *css_author_sheets_build(lxb_dom_node_t *root);
void css_author_sheets_free(CssAuthorSheets *s);

const lxb_dom_node_t *css_author_sheets_root(const CssAuthorSheets *s);
/* The sealed order every layer node of the set's views belongs to; a cascade over it reads indices only. */
CssLayerOrder *css_author_sheets_order(CssAuthorSheets *s);
uint32_t css_author_sheets_count(const CssAuthorSheets *s);
const CssAuthorSheet *css_author_sheets_at(const CssAuthorSheets *s, uint32_t i);

#endif /* ENGINE_HOST_BROWSER_CORE_CSS_CSS_AUTHOR_SHEETS_H */
