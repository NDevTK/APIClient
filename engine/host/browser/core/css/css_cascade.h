/* CSS Cascade §6.1's CASCADE SORTING ORDER, and §7.3.4/§7.3.5/§7.3.6's three ROLL-BACK keywords, which are the
 * same sort run again with part of itself removed.
 *
 * THE CASCADE IS A LIST OF DECLARATIONS THAT GETS SORTED, NOT A SEQUENCE OF LAYERS THAT GET ASKED IN TURN. That
 * distinction is the whole of this component. §6.1 states SIX criteria in descending order of precedence —
 * Origin and Importance, Context, Element-Attached Styles, Layers, Specificity, Order of Appearance — and each
 * one is only reached when every criterion above it TIED. An implementation that asks the inline style first,
 * then the author rules, then the presentational hints, then the UA sheet, and returns the first that answers,
 * is not that order: it has hoisted "element-attached" above "origin and importance", so an important author
 * rule loses to a normal style attribute, which §6.1 says it wins. And once §6.4's layers exist the collapse is
 * total, because Layers sits ABOVE Specificity: `@layer a { #x { color: red } } p { color: blue }` resolves to
 * BLUE on a `<p id=x>`, and no ordering of a flat rule list can express that, since order of appearance is the
 * LAST tiebreak and specificity outranks it.
 *
 * ONE KEY, COMPARED ONCE. The three criteria above Specificity are a single monotone integer — the LEVEL — and
 * that is not a packing trick, it is what §7.3.5 needs: `revert-layer` is defined over a RANGE of the cascade
 * ("as if no rules were specified in the current cascade layer—or BETWEEN its normal and important levels in
 * the cascade"), and a range only exists if the levels are ordered. The level is
 *     (band * 2 + element-attached) * layer-count + layer-key
 * where `band` is §6.1's Origin-and-Importance rank read as a weight, and `layer-key` is §6.4.3's index read in
 * the direction §6.1's Layers criterion asks for: LATEST wins for a normal declaration, EARLIEST for an
 * important one, which is the same inversion §6.3 applies to origins and is why one field serves both.
 *
 * §6.1's CONTEXT CRITERION IS NOT A TERM IN IT, AND THAT IS A FACT ABOUT THE INPUT RATHER THAN A GAP. Every
 * declaration in one resolution comes from ONE encapsulation context: the style sheets are read from the
 * element's own root node and the style attribute is on the element itself, so there is no second context for
 * the criterion to compare against. It becomes a term the day a declaration from an outer tree can reach this
 * list — which is what `::part()`, `::slotted()` and the shadow-including tree order would bring.
 *
 * THE ORIGINS ARE THE ONES THIS ENGINE HAS, AND THE BAND FUNCTION IS §6.1's WHOLE LIST. §6.2 defines three core
 * origins plus Animation and Transition; §6.5 adds the author presentational hint origin "between the regular
 * user origin and the author origin". This engine has an author origin (style sheets and the style attribute),
 * that presentational hint origin, and a user-agent origin; there is no user style sheet, no running animation
 * and no running transition, so those three bands are unoccupied rather than mis-ordered — the band function
 * computes §6.1's list entire and nothing produces a declaration in them.
 *
 * §7.3's THREE ROLL-BACKS ARE ONE MECHANISM: re-run the sort with a part of the cascade removed. They differ
 * only in WHICH part, and each spec sentence names it exactly — §7.3.4's `revert` removes an ORIGIN (and, per
 * §6.5, the presentational hint origin travels with the author origin for this one keyword and not for
 * `revert-layer`), §7.3.5's `revert-layer` removes the LEVEL RANGE above, §7.3.6's `revert-rule` removes the
 * style rule the declaration is in. So they resolve HERE and not in §7's defaulting step: §7.3.4's behaviour
 * "depends on the cascade origin to which the declaration belongs", which is a fact the sort knows and the
 * defaulting step is never told. What crosses to core/css/css_defaulting.h is a cascaded value that is never
 * one of the three — with the single exception §7.3.4 states, a `revert` in the user-agent origin, which "is
 * equivalent to unset" and is answered as that keyword because `unset` is exactly what §7 resolves. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_CASCADE_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_CASCADE_H
#include <stdbool.h>
#include <stdint.h>

#include "core/css/css_layer_order.h"

/* §6.2's cascade origins, plus §6.5's — the ones a declaration in this engine can come from. */
typedef enum {
    CSS_ORIGIN_UA = 0,                /* §6.2's User-Agent Origin */
    CSS_ORIGIN_PRESENTATIONAL_HINT,   /* §6.5's author presentational hint origin */
    CSS_ORIGIN_AUTHOR,                /* §6.2's Author Origin — style sheets and the style attribute */
} CssOrigin;

typedef struct CssCascade CssCascade;

/* A cascade over `layers`, which every declaration's layer node must belong to. The order need not be sealed
   yet — it is walked and sealed while the declarations are collected — but it must be sealed before the value
   is asked for. */
CssCascade *css_cascade_create(CssLayerOrder *layers);
void css_cascade_free(CssCascade *c);

/* ONE DECLARED VALUE for the property being resolved, with everything §6.1 sorts by.
   `element_attached` is §6.1's own criterion — "declarations that are attached directly to an element (such as
   the contents of a style attribute) rather than indirectly mapped by means of a style rule selector".
   `layer` is the §6.4.3 node the declaration's rule is in, and NULL is §6.1's "any declaration not assigned to
   an explicit layer is added to an implicit final layer" for a declaration outside the author origin's layer
   tree entirely.
   `specificity` is Selectors' number for the rule that matched, and is ZERO for a declaration that matched no
   selector (an element-attached one, a presentational hint, a UA default) — §6.1 never compares those against
   a style rule's, because Element-Attached Styles and Origin both sit above Specificity.
   `seq` is §6.1's Order of Appearance: a position in the document-order walk, strictly increasing across the
   whole cascade, which doubles as the identity of the RULE the declaration came from because §6.6's collapse
   leaves one declaration per property per rule. `value` is COPIED. */
void css_cascade_add(CssCascade *c, CssOrigin origin, bool important, bool element_attached,
                     const CssLayerNode *layer, uint32_t specificity, uint32_t seq, const char *value);

/* §6's CASCADED VALUE: the declaration §6.1 sorts highest, with §7.3's roll-backs discharged. OWNED. NULL when
   no declaration was added, which is §7.1 and §7.2's "unless the cascade results in a value" and a real answer
   rather than a missing one. The layer order must be sealed. */
char *css_cascade_value(CssCascade *c);

#endif
