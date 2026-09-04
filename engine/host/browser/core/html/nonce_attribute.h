/* HTML §2.5.6 "Nonce attributes" — the `[[CryptographicNonce]]` internal slot, and the four algorithms that
 * write it. See nonce_attribute.c.
 *
 * ONE PROBLEM: `element.nonce` IS NOT A REFLECTION, AND IT WAS DECLARED AS ONE. §2.5.6 states the member in a
 * single sentence — "The nonce IDL attribute must, on getting, return the value of this element's
 * [[CryptographicNonce]]; and on setting, set this element's [[CryptographicNonce]] to the given value" — and
 * then says the quiet part in a note: "Note how the setter for the nonce IDL attribute does not update the
 * corresponding content attribute." A `REFLECT_STRING` row is wrong in BOTH directions against that, which is
 * rarer than the usual wrong-kind defect and is why this file exists rather than a new kind in the reflection
 * enum. The getter answered the content attribute, so an element whose nonce a browser hands back reads as the
 * empty string the moment §2.5.6's own hiding step has blanked that attribute; and the setter WROTE the content
 * attribute, which is precisely the exfiltration channel §2.5.6 exists to close — "meant to prevent
 * exfiltration of the nonce value through mechanisms that can easily read content attributes, such as
 * selectors". A page that does `s.nonce = n` under a CSP got a `[nonce=…]` attribute selector could read.
 *
 * WHY IT MATTERS TO A SOLVER RATHER THAN ONLY TO A CONFORMANCE RUN. The nonce is what decides whether a
 * `<script>` a flow injects RUNS: §@S measures every breakout against the document's CSP, and a `script-src
 * 'nonce-…'` policy admits exactly the elements carrying that value. So which bytes `el.nonce` answers with is
 * the difference between a PoC this engine reports as blocked and one a browser executes, and it is a value an
 * attacker string can be stashed in (`s.nonce = location.hash.slice(1)`), which is why the slot holds a JS
 * VALUE and not bytes: the triple has to survive the round trip like every other DOM string slot's does.
 *
 * WHERE THE SLOT LIVES, AND WHY IT IS NOT THE WRAPPER. It is the per-flow named-slot shadow
 * (solver/attr_shadow.h's ATTR_SLOT_PROPERTY), written through solver/dom_cow.h — the same place §4.10.5.1's
 * element's value lives, and for the same three reasons: the value is a STRING a source can be stashed in, it
 * must TIME-TRAVEL so a forked arm's assignment is that arm's alone, and the shadow is keyed on the Lexbor
 * element so the attribute change steps below can write it with no wrapper in hand. A slot on the wrapper is
 * right for a BOOLEAN nothing can taint (§4.12.1's `already started` is one); it is the wrong home for a value
 * a page computes. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_NONCE_ATTRIBUTE_H
#define ENGINE_HOST_BROWSER_CORE_HTML_NONCE_ATTRIBUTE_H

#include <lexbor/dom/dom.h>

#include "quickjs.h"

/* Once per AGENT — the `nonce` setter's step id. */
void nonce_attribute_init(JSContext *ctx);
/* Onto ONE realm's HTMLElement.prototype. HTMLElement is the only interface in this agent that includes
   HTMLOrSVGOrMathMLElement: SVGElement and MathMLElement are names on browser/platform_names.h, so reading one
   is the ReferenceError that names the component to write, and the mixin has no other carrier to be on. */
void nonce_attribute_install(JSContext *ctx, JSValueConst proto);
void nonce_attribute_free(void);

/* §2.5.6's ATTRIBUTE CHANGE STEPS — "If element does not include HTMLOrSVGOrMathMLElement, then return. If
   localName is not nonce or namespace is not null, then return. If value is null, then set element's
   [[CryptographicNonce]] to the empty string. Otherwise, set element's [[CryptographicNonce]] to value."
   Registered on core/dom/element.c's element_attr_changed beside html_script_attr_changed, and there rather
   than in the member's own setter for the reason every one of its neighbours is: a content attribute has more
   than one spelling (`setAttribute('nonce', n)`, `attributes.nonce.value = n`, the parser's own write) and an
   IDL setter answers for exactly one of them — and here for NONE of them, because §2.5.6's setter deliberately
   does not touch the attribute at all. These steps are the ONLY road from the markup to the slot. */
void nonce_attribute_attr_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local);

/* §2.5.6's CLONING STEPS — "The cloning steps for elements that include HTMLOrSVGOrMathMLElement given node,
   copy, and subtree are to set copy's [[CryptographicNonce]] to node's [[CryptographicNonce]]." Run from DOM
   §4.4 clone a node's step 3, beside html_script_cloned, which is where every node of a clone passes.
   THEY ARE NOT REDUNDANT WITH THE ATTRIBUTE COPY. A clone copies the content attribute, so the change steps
   above would give the copy the ATTRIBUTE's bytes — and the whole point of §2.5.6 is that those two stop
   agreeing: after the hiding step the attribute is the empty string while the slot holds the nonce, and after
   any `el.nonce = v` the slot holds v while the attribute holds whatever the markup said. Without this the
   copy's nonce is the stale attribute, which under a `script-src 'nonce-…'` policy is the difference between a
   cloned script that runs and one that does not.
   `src` and `copy` may be any node kind; a pair that is not two elements including the mixin is a no-op, for
   the same reason §4.12.1's cloning steps beside it tolerate one. */
void nonce_attribute_cloned(JSContext *ctx, lxb_dom_node_t *src, lxb_dom_node_t *copy);

/* §2.5.6's [[CryptographicNonce]], READ BY A COMPONENT RATHER THAN BY THE MEMBER — "the current value of el's
 * [[CryptographicNonce]] internal slot", which is exactly what HTML §4.2.4.4 "Processing `Link` headers"'
 * create link options from element takes for its options' cryptographic nonce metadata, and which §4.2.4.3
 * "Fetching and processing a resource from a link element"'s create a link request then places on the request
 * itself. The two algorithms are one step apart and live in different sections, which reads like an editorial
 * accident and is the document's; a citation that puts them both under one number is wrong about a section a
 * reader can open.
 *
 * IT IS NOT THE `nonce` CONTENT ATTRIBUTE AND THAT IS THE WHOLE REASON IT EXISTS. The two stop agreeing the
 * moment §2.5.6's hiding step blanks the attribute or a page does `el.nonce = v`, and a request built from the
 * attribute would then carry bytes the element no longer has — under a `script-src 'nonce-…'` policy that is
 * the difference between a subresource CSP admits and one it refuses. A caller reaching for
 * lxb_dom_element_get_attribute here is reading the wrong half of §2.5.6.
 *
 * OWNED, and it may be a CONCOLIC rather than a string: the slot deliberately holds a JS value so that
 * `el.nonce = location.hash.slice(1)` survives as a source. A caller that needs BYTES therefore has to decide
 * what an unknown nonce means for its own algorithm before it coerces one. `el` may be any element; one that
 * does not include HTMLOrSVGOrMathMLElement has no slot and answers the empty string §2.5.6 gives an element
 * nothing has written. */
JSValue nonce_attribute_current(JSContext *ctx, lxb_dom_element_t *el);

#endif
