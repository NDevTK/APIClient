/* CSSOM VIEW §6 — EXTENSIONS TO THE ELEMENT INTERFACE, which is how a page reads an element's geometry, and
 * the file that has to say what geometry this engine HAS.
 *
 * THE BOX MODEL THIS ENGINE ACTUALLY HAS, stated once here because every member below is a branch over it and
 * because two of this engine's components had already answered it in opposite directions:
 *
 *   A BOX EXISTS wherever a user agent would generate one. HTML's "being rendered" is defined AS "has any
 *   associated CSS layout boxes", so §6's "associated box" and HTML's "being rendered" are ONE predicate under
 *   two names, and `element_view_has_box` below is that one predicate. It is decided from facts the TREE
 *   carries — the element is connected, its node document is some navigable's ACTIVE document and is being
 *   presented (viewport.h's `viewport_exists`), and neither it nor an ancestor carries the `hidden` content
 *   attribute, whose §15.3.1 UA-stylesheet rule is `display: none`.
 *
 *   NO BOX HAS GEOMETRY except the INITIAL CONTAINING BLOCK, which viewport.c models. There is no layout, so a
 *   box's padding edge, its border widths, its scrolling area and its fragments do not exist to be read.
 *
 * THOSE ARE TWO DIFFERENT ANSWERS AND THE SPEC ASKS THEM SEPARATELY, which is the whole reason this component
 * can answer anything at all. §6's algorithms use box EXISTENCE as a gate and then, in several branches, route
 * around the box entirely to the VIEWPORT: `clientWidth` on the root element returns the viewport width and
 * never looks at the root's box; `scrollWidth` on the root returns max(the viewport's scrolling area, the
 * viewport) and never looks at a descendant. Those branches are answered here, for real. A branch that reaches
 * the element's OWN geometry has no answer in this model and DFAILs, naming the layout it needs — never a zero
 * standing in for a number this engine does not have, which is the stub §NO STUBS is about.
 *
 * WHAT WAS SAID HERE BEFORE, AND WHICH HALF OF IT WAS WRONG. viewport.c's §2 derivation said "this engine
 * generates no boxes — there is no layout", and core/html/focus.c's §6.6.2 row 1 said a connected element that
 * is not `hidden` IS being rendered. Both cannot be true. The second is the correct one — a UA generates a box
 * for the root element of a document it is presenting, and this engine presents every document it holds
 * (page_visibility.c and focus.c both already commit to that) — so it is the one that survives, and it is now
 * ONE function rather than a private static in the focus model. What survives of the first is the part that was
 * really about GEOMETRY and not about existence: no box in this model has a margin edge, so nothing extends the
 * ICB, and viewport.c's scrolling area is still exactly the ICB.
 *
 * SO THE VIEWPORT HAS ONE VALID SCROLL POSITION AND SO DOES EVERY ELEMENT. viewport.h derives the first: the
 * viewport's scrolling area is the ICB, and a scrolling box whose scrolling area is its own size can only sit
 * at its origin. The second is derived rather than assumed: the origin of a scrolling area is defined AT the
 * element's default scroll position, a scroll position moves only when §3.1's PERFORM A SCROLL runs, and the
 * only route to that for an element is the `scrollTop`/`scrollLeft` setter below — which DFAILs rather than
 * scrolling. So "no element has been scrolled" is true BY CONSTRUCTION, which is what makes the getters' final
 * step a derivation and not a shrug, and the DFAIL is what keeps it true.
 *
 * THE CONCOLIC POLICY IS INHERITED, NOT RE-DECIDED. A member that reports the viewport's size reports a UA
 * CHOICE, so it carries the modelled geometry as the EXAMPLE of a concolic minted through viewport.h's one seam
 * (`viewport_env_value`) — `document.documentElement.clientWidth` is the most common way a bundle asks how wide
 * the viewport is, and a bare number there deletes a responsive bundle's mobile world exactly as a bare
 * `innerWidth` would. It is its OWN source rather than `innerWidth`'s, by 2dbe86d8's own test: `innerWidth`
 * INCLUDES a rendered scroll bar and `clientWidth` EXCLUDES it, so `innerWidth - documentElement.clientWidth` is
 * a bundle measuring the scroll bar, a question with two answers. A member whose answer the model DERIVES —
 * every scroll position here — stays concrete, for the reason viewport.h gives: a fact with a writer is per-flow
 * state in the COW delta, not an environment source. Nothing in this file branches in C on a concolic.
 *
 * WHAT IS HONESTLY ABSENT: `getClientRects`, `getBoundingClientRect`, `checkVisibility`, `scrollIntoView`,
 * `scroll`, `scrollTo`, `scrollBy` and `currentCSSZoom`. The first four need real box FRAGMENTS (a DOMRect per
 * fragment, a flat-tree walk over computed `content-visibility` and `opacity`), which is the layout named
 * below; the three scroll methods are §6's Promise-returning form of the setter below and arrive with the
 * Promise the perform-a-scroll steps return. The IDL audit reports all of them, which is correct. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_ELEMENT_VIEW_H
#define ENGINE_HOST_BROWSER_CORE_DOM_ELEMENT_VIEW_H

#include <stdbool.h>
#include <lexbor/dom/dom.h>
#include "quickjs.h"

/* Declared once per AGENT — the two settable attributes' setter ids. Called from element_init, so no host has a
   line to remember. */
void element_view_init(JSContext *ctx);
/* §6's `partial interface Element`, for ONE realm — installed on the prototype element.c has just built, the
   way §4.9's other partial interfaces are. It is per realm because its answers are: a child navigable's
   viewport is 300 CSS pixels wide and the top-level traversable's is 1280, and a C member runs in the realm
   that DEFINED it. */
void element_view_install(JSContext *ctx, JSValueConst proto);
void element_view_free(void);

/* HTML'S "BEING RENDERED" AND CSSOM VIEW'S "HAS AN ASSOCIATED BOX" — one predicate, one answer, for every
   standard in this engine that asks it. See the header above for what decides it and for what it deliberately
   does NOT ask: a `display: none` written in author CSS or in an inline style is a computed value the CSSOM in
   this build exposes to no C caller, so it does not participate. That is a NARROWER answer than a laying-out
   browser's, never a wider one — an element this says is being rendered may not be, and one it says is not
   never is. `n` must be an element. */
bool element_view_has_box(const lxb_dom_node_t *n);

#endif
