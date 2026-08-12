/* HTML §3.2.6 — THE DIRECTIONALITY OF AN ELEMENT.
 *
 * `dir` is not a style shorthand. §3.2.6 computes a value — 'ltr' or 'rtl' — from the attribute's enumerated
 * state, from the element's ANCESTORS when the state is Undefined, and from the FIRST STRONG CHARACTER of the
 * element's own text when the state is Auto. Four separate algorithms read that value, and one of them is a
 * form submission: a control with a `dirname` attribute submits its directionality as a second entry, so the
 * bytes a server receives depend on this computation. That is why it is a component rather than a getter — it
 * is the only reason a headless engine needs it at all, and it is a real one.
 *
 * THE UNICODE DATA IS GENERATED, NOT GUESSED. "The first character of bidirectional character type L, AL, or
 * R" is Bidi_Class, an ENUMERATED Unicode property, and neither quickjs's libunicode (ECMAScript's binary
 * properties only) nor lexbor's unicode module (IDNA, with its own bidi check marked not implemented) has it.
 * So it comes from the UCD, through engine/gen_bidi_class.mjs into bidi_class.h — checked in beside the
 * generator, the way libunicode's own tables are made. Deciding "strong RTL" by a hand-written range guess is
 * how an engine reports Arabic text as left-to-right for the code points nobody thought of.
 *
 * THE WALKS ARE ITERATIVE, and that is not a style choice: §3.2.6's Undefined case is defined by RECURSION
 * into the parent's directionality, and its Auto case walks the element's descendants in tree order. Both
 * depths are the PAGE's to choose, so a self-call would be C recursion a page controls. What remains is that
 * the descendant walk is O(subtree) inside whatever step its caller is resting at; making it rest per NODE is
 * the entry-list machine's own conversion, since a walk can only rest where its caller can. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_DIRECTIONALITY_H
#define ENGINE_HOST_BROWSER_CORE_HTML_DIRECTIONALITY_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"

/* §3.2.6's two answers. There is no third: "the directionality of an element is either 'ltr' or 'rtl'". The
   NULL that `auto directionality` can answer is not one of these — it means "this element's own text decided
   nothing", and every caller of it turns that into 'ltr' at its own step. */
enum { DIRECTION_LTR = 0, DIRECTION_RTL = 1 };

/* THE DIRECTIONALITY OF `el`, per §3.2.6 — the enumerated `dir` state, the ancestor chain, and Auto's
   first-strong scan, as one answer. Never fails: an element with no ancestor that decides anything is 'ltr',
   which is the algorithm's own last step. */
int directionality_of(JSContext *ctx, lxb_dom_element_t *el);

/* §3.2.6's DIRECTIONALITY OF AN ATTRIBUTE, for a directionality-capable attribute whose text is rendered.
   `value`/`len` are the attribute's value; the answer is the first-strong scan over it when the element's
   `dir` is Auto, and the element's own directionality otherwise. */
int directionality_of_attribute(JSContext *ctx, lxb_dom_element_t *el, const char *value, size_t len);

/* 'ltr' or 'rtl' as the string a `dirname` entry submits and `document.dir` reflects — one spelling, so two
   callers cannot disagree about the case of it. Static storage. */
const char *directionality_name(int dir);

#endif
