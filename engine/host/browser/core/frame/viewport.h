/* THE VIEWPORT — CSS 2.1 §9.1.1 "The viewport" and §10.1's INITIAL CONTAINING BLOCK, which is the one piece of
 * geometry every other geometric answer in the platform is measured against.
 *
 * IT IS A MODELLED UA CHOICE, NOT AN UNKNOWN, and it is the same kind of choice as rendering.c's refresh rate.
 * CSS says the viewport is "a window or other viewing area on the screen through which users consult a
 * document" and that its size is the UA's; it does not say what the size IS, and a UA running with no display
 * still has to answer, because the ICB is what `width: auto` resolves against. §Headless's missing piece is a
 * physical device, and the size of the area a document is laid out in exists without one.
 *
 * WHY IT IS A COMPONENT OF ITS OWN rather than a constant inside whoever needed it first: FOUR standards read
 * this one fact and each would otherwise carry its own copy. Media Queries §4 evaluates `width`, `height`,
 * `aspect-ratio` and `orientation` against it; CSSOM VIEW §7 answers `innerWidth`/`innerHeight` from it;
 * Intersection Observer §3.2 uses it as the implicit root's intersection rectangle; and CSS 2.1 §10.3.3
 * resolves the used width of the root element from it. One fact answered from four places is the defect
 * CLAUDE.md §per-realm names, in its plainest form.
 *
 * AND IT IS PER REALM, which is not pedantry here either. A child navigable's viewport is the viewport of ITS
 * document, and it is a different size from the top-level traversable's: HTML §15.3's UA stylesheet gives an
 * `iframe` no intrinsic dimensions, so CSS 2.1 §10.3.2's default replaced-element size — 300 x 150 — is what
 * the element's content box is, and that content box IS the child document's viewport. So a media query
 * `(min-width: 600px)` is TRUE in the top-level document and FALSE in an iframe of it, from the same page's
 * code, and a module static would answer both with whichever realm asked first.
 *
 * NOTHING IS INSTALLED ON THE GLOBAL BY THIS FILE. `innerWidth`, `innerHeight`, `visualViewport` and
 * `devicePixelRatio` are CSSOM VIEW's Window members, and a member that is not built is honestly ABSENT — the
 * `step_awaits(docctx, "innerWidth", …)` in rendering.c is the assertion that says so, and it fires at
 * update-the-rendering step 8 (the resize steps) the moment one of them arrives, because a viewport that a page
 * can READ is a viewport whose CHANGES the page can observe. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_VIEWPORT_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_VIEWPORT_H

#include "quickjs.h"

/* THIS REALM's viewport, in CSS pixels. Answered for the realm passed, never for a remembered one. */
double viewport_width(JSContext *ctx);
double viewport_height(JSContext *ctx);
/* CSSOM VIEW §7's `devicePixelRatio` — the ratio of device pixels to CSS pixels, which Media Queries §4.7's
   `resolution` feature reports as `dppx`. A modelled 1.0 is a UA with a non-HiDPI display, chosen for the same
   reason the size is. */
double viewport_device_pixel_ratio(JSContext *ctx);

#endif
