/* THE AUTHOR PRESENTATIONAL HINT ORIGIN — css-cascade-5 §6.5's "special-purpose author presentational hint
 * origin between the regular user origin and the author origin", holding the declarations HTML §15.2 says a
 * document language's own MARKUP maps into the cascade.
 *
 * IT IS AN ORIGIN AND NOT A STYLE SHEET, which is the whole reason this is a component rather than four rows in
 * the UA table css_style_declaration.c already has. §6.5: "All document language-based styling must be
 * translated to corresponding CSS rules and enter the cascade as rules in either the UA-origin or a
 * special-purpose author presentational hint origin ... For the purpose of cascading this author presentational
 * hint origin is treated as an independent origin", and HTML §15.2 chooses that origin for these ("Some rules
 * are intended for the author-level zero-specificity presentational hints part of the CSS cascade"). So a hint
 * loses to ANY author declaration whatever its specificity, and beats the UA sheet — which a UA row cannot
 * express in either direction.
 *
 * AND IT IS COMPUTED PER ELEMENT FROM THAT ELEMENT'S OWN ATTRIBUTES, which a sheet cannot express at all:
 * §15.3.2's body margins are a table of FOUR ATTRIBUTES read in order with a fallback, not a selector. That is
 * also why the answer is derived per read like every other layer of this cascade — the attributes are per-flow
 * DOM state, and a hint cached anywhere would be shared state the flow machinery does not swap.
 *
 * WHAT IS HERE IS §15.3.2's PAGE MARGINS. Every other hint HTML defines (a `bgcolor`, an `align`, an `<img>`
 * `width`) is honestly ABSENT: it is a row this file does not have yet, and its absence reads as the property
 * being undeclared by this origin rather than as a wrong value. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_PRESENTATIONAL_HINTS_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_PRESENTATIONAL_HINTS_H
#include <lexbor/dom/dom.h>

/* THE DECLARED VALUE this origin gives `name` on `el`, as the text a specified value is read from, or NULL when
   no hint of this origin sets that property on that element. OWNED: the caller frees. */
char *css_presentational_hint(lxb_dom_element_t *el, const char *name);

#endif
