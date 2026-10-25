/* css-cascade-5 §6.2 "Cascading Origins"' author origin as one root's style sheets present it to the cascade:
 * every sheet of CSSOM §6.2 "CSS Style Sheet Collections" that applies, emitted for the matcher by
 * core/css/css_rule.h's cascade walk, and the css-cascade-5 §6.4.3 "Layer Ordering" order their
 * walk declares, sealed once the last sheet is emitted.
 *
 * The set is a function of the root's sheet objects, their rules and the realm's viewport (a conditional group
 * rule's condition and a sheet's media), and of nothing about the element being resolved. A layout job holds
 * one per root it resolves under (core/css/css_cascade_pass.h states why those inputs do not move while it
 * does). */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_AUTHOR_SHEETS_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_AUTHOR_SHEETS_H

#include <stdbool.h>
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

/* The set for `root` (a document or a shadow root), in its document's realm, opened empty and built by `run`.
   A sheet that is disabled, whose media does not match, or whose stored text cannot be read back contributes
   nothing. `run` continues from where the last one stopped; with `rests` each rule emitted is taken from the
   resting drive's allowance and the run parks where one is refused, its walk held in the set. True when the
   set is sealed, which the readers below require. The caller owns the set, open or sealed. */
CssAuthorSheets *css_author_sheets_open(lxb_dom_node_t *root);
bool css_author_sheets_run(CssAuthorSheets *s, bool rests);
bool css_author_sheets_sealed(const CssAuthorSheets *s);
void css_author_sheets_free(CssAuthorSheets *s);

const lxb_dom_node_t *css_author_sheets_root(const CssAuthorSheets *s);
/* The sealed order every layer node of the set's views belongs to; a cascade over it reads indices only. */
CssLayerOrder *css_author_sheets_order(CssAuthorSheets *s);
uint32_t css_author_sheets_count(const CssAuthorSheets *s);
const CssAuthorSheet *css_author_sheets_at(const CssAuthorSheets *s, uint32_t i);
/* CSS Properties and Values API 1 §2.1's `@property` half over the set's sheets: the last registration of
   `name` in document order, copied into `*out` (freed by css_property_registration_free), or false with `*out`
   untouched. */
bool css_author_sheets_registration(const CssAuthorSheets *s, const char *name, CssPropertyRegistration *out);

#endif /* ENGINE_HOST_BROWSER_CORE_CSS_CSS_AUTHOR_SHEETS_H */
