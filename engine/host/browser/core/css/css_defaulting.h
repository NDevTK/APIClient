/* CSS Cascade §7 — DEFAULTING: what becomes a property's SPECIFIED value when the cascade did not decide it,
 * or when the declaration that won says explicitly that the cascade should not.
 *
 * IT IS ITS OWN COMPONENT BECAUSE IT IS ITS OWN STEP. §6's cascade takes the declarations and answers which one
 * WON — a question about style sheets, specificity and origins, and core/css/css_style_declaration.h's whole
 * subject. §7 takes that answer, which for most properties on most elements is that there ISN'T one, and turns
 * it into a value: the property's own initial value, or the value the PARENT ELEMENT computed. That second one
 * is a question about the TREE, not about style sheets, and it is why the two cannot be one function — the
 * cascade would have to reach back into the computed value it exists to feed, once per ancestor.
 *
 * THIS COMPONENT DECIDES WHICH, AND NEVER FETCHES EITHER. `css_defaulting_of` answers one of three words; the
 * initial value is core/css/css_style_declaration.h's `cssom_initial_value` (it comes out of lexbor's property
 * registry, beside the cascade that already reads it), and the inherited value is the PARENT'S COMPUTED VALUE,
 * which core/css/css_computed_value.h owns and which is answered in TWO SHAPES — a `char *` for a keyword-valued
 * property and a `CssLength` for a length-valued one. A component that fetched the inherited value would have
 * to pick one of those shapes for both, and picking text is exactly the bug css_computed_value.h describes: the
 * absolute length carries the environment fact it derives from, and serializing it to hand it to a child would
 * drop the domain and delete the fork behind it. So the caller that knows the shape performs the fetch.
 *
 * §7.2's `Inherited:` LINE IS A FACT ABOUT THE PROPERTY, and non-inheritance is CSS's DEFAULT rather than this
 * component's fallback: §7.2 says "SOME properties are inherited properties, as defined in their property
 * definition table", so the enumerable set is the inherited one and every other property is answered by that
 * sentence. The table below is transcribed from those definition tables — CSS 2.1's aural and visual longhands,
 * css-fonts, css-text, css-writing-modes, css-lists, css-tables, css-ui, css-color, css-color-adjust,
 * css-images, css-ruby, mathml-core's three, and SVG's painting and text properties, all of which reach a
 * cascade the moment a page reads one back through getComputedStyle.
 * IT IS A LIST OF LONGHANDS. The cascade is over longhands (css_style_declaration.c asserts it), so a shorthand
 * never reaches this step — `font` and `list-style` are absent from the table not because they do not inherit
 * but because the question is never asked about them. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_DEFAULTING_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_DEFAULTING_H
#include <stdbool.h>

/* What §7 puts in the cascaded value's place — or that it puts nothing there. */
typedef enum {
    CSS_DEFAULTING_DECLARED = 0,   /* the cascaded value IS the specified value: no defaulting step applies */
    CSS_DEFAULTING_INHERITED,      /* §7.2's inherited value: the PARENT element's computed value */
    CSS_DEFAULTING_INITIAL         /* §7.1's initial value */
} CssDefaulting;

/* CSS Cascade §7 over the CASCADED value of `name` — which is NULL when no declaration won, the state §7.1 and
   §7.2 are written for ("unless the cascade results in a value"), and one of §7.3's CSS-wide keywords when the
   declaration that won says which defaulting it wants. Every other cascaded value is DECLARED and stands.
   `cascaded` is BORROWED and not freed: which of the three answers this is decides who owns it, and the caller
   is the one that can act on that. */
CssDefaulting css_defaulting_of(const char *name, const char *cascaded);

/* §7.2's `Inherited:` line for `name` — is this one of the properties whose value propagates from the parent
   element when the cascade does not decide it? */
bool css_property_inherited(const char *name);

/* Is `value` one of CSS Cascade §7.3's CSS-WIDE KEYWORDS — `inherit`, `initial`, `unset`, `revert`,
   `revert-layer`, `revert-rule`? Each is the ENTIRE value of a declaration when present, so this is an
   equality and not a search, and each makes the value a product of §7's DEFAULTING step rather than of the
   declaration.
   EXPORTED because a SHORTHAND carrying one "sets all of its sub-properties to that keyword" (CSS Cascade
   §Shorthand Properties), which core/css/css_shorthand.h has to know BEFORE it tries the shorthand's own
   grammar: `border: initial` is not a `<line-width> || <line-style> || <color>`, and a keyword that reached
   that grammar would be classified as one of its terms. AND because the set is not only about declarations:
   CSS Cascade §6.4.2 reserves exactly these keywords inside a `<layer-name>` ("the CSS-wide keywords are
   reserved for future use, and cause the rule to be invalid at parse time if used as an <ident> in the
   <layer-name>"), which core/css/css_at_rule_prelude.h asks here rather than restating — two copies could
   disagree about `revert-rule`, and one of them did. */
bool css_wide_keyword(const char *value);

/* WHICH OF §7.3's THREE CASCADE-DEPENDENT KEYWORDS a value is, if it is one. §7.3's own opening sentence names
 * them as a set and says what makes them different from the other three: "The keywords revert, revert-layer,
 * and revert-rule are CASCADE-DEPENDENT keywords; some contexts may restrict their use while allowing the other
 * CSS-wide keywords."
 *
 * IT IS ASKED BY THE CASCADE AND NOT BY THIS COMPONENT, which is what the distinction means in code. §7.3.1
 * through §7.3.3 are answered from the property alone — `initial` is its initial value, `inherit` is the
 * parent's computed value, `unset` is one of those two — and that is a question §7's defaulting step can ask.
 * These three are answered from the cascade the declaration SAT IN: §7.3.4's behaviour "depends on the cascade
 * origin to which the declaration belongs", §7.3.5's on its cascade layer, §7.3.6's on its style rule, and none
 * of those three facts survives into a cascaded value. So core/css/css_cascade.h resolves them by re-running
 * its own sort with that origin, layer or rule removed, and what reaches `css_defaulting_of` is never one of
 * them — the assertion there says so from the other side. */
typedef enum {
    CSS_ROLLBACK_NONE = 0,
    CSS_ROLLBACK_ORIGIN,   /* §7.3.4's `revert` */
    CSS_ROLLBACK_LAYER,    /* §7.3.5's `revert-layer` */
    CSS_ROLLBACK_RULE,     /* §7.3.6's `revert-rule` */
} CssRollback;

CssRollback css_rollback_keyword(const char *value);

#endif
