/* THE VIEWPORT — CSS 2.1 §9.1.1 "The viewport" and §10.1's INITIAL CONTAINING BLOCK, which is the one piece of
 * geometry every other geometric answer in the platform is measured against, and CSSOM VIEW §4's WINDOW
 * EXTENSIONS, which are how a page reads it.
 *
 * IT IS A MODELLED UA CHOICE, NOT AN UNKNOWN, and it is the same kind of choice as rendering.c's refresh rate.
 * CSS says the viewport is "a window or other viewing area on the screen through which users consult a
 * document" and that its size is the UA's; it does not say what the size IS, and a UA running with no display
 * still has to answer, because the ICB is what `width: auto` resolves against. §Headless's missing piece is a
 * physical device, and the size of the area a document is laid out in exists without one. So every member below
 * answers with a CONCRETE number: the UA knows its own viewport, and a concolic there would model an ignorance
 * this engine does not have.
 *
 * WHY IT IS A COMPONENT OF ITS OWN rather than a constant inside whoever needed it first: FOUR standards read
 * this one fact and each would otherwise carry its own copy. Media Queries §4 evaluates `width`, `height`,
 * `aspect-ratio` and `orientation` against it; CSSOM VIEW §4 answers `innerWidth`/`innerHeight` from it;
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
 * WHICH IS ALSO WHY §4's MEMBERS ARE INSTALLED BY THIS FILE and not by window.c. A C member runs in the realm
 * that DEFINED it — js_call_c_function takes `ctx` off the function object — so a member installed once would
 * answer every document's `innerWidth` out of whichever realm happened to build it first, and the whole point
 * of the paragraph above is that the answers DIFFER. They are declared once per agent and installed per realm,
 * through the ONE list in core/realm.h that every realm goes through, including the agent's first.
 *
 * WHAT STOOD HERE SAID "NOTHING IS INSTALLED ON THE GLOBAL BY THIS FILE", and named the
 * `step_awaits(docctx, "innerWidth", …)` in rendering.c as the two-sided assertion that said so — a probe that
 * stays silent while the producer is absent and FIRES at update-the-rendering step 8 the moment one arrives,
 * "because a viewport that a page can READ is a viewport whose CHANGES the page can observe". It did exactly
 * that. Step 8 is CSSOM VIEW §13.1's RESIZE STEPS, it is written now, and `viewport_resize_changed` below is
 * the half of it this component owns. The probe is GONE rather than relaxed, which is the whole point of the
 * mechanism: a probe that outlives the work it demanded goes on describing an absence that is not there. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_VIEWPORT_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_VIEWPORT_H

#include <stdbool.h>
#include "quickjs.h"

/* Declared once per AGENT: the §4 members' names and the per-realm slot their §13.1 latch lives in. It also
   REGISTERS the per-realm install, so no host has a line to remember. */
void viewport_init(JSContext *ctx);
void viewport_free(void);

/* IS THERE A VIEWPORT — CSSOM VIEW §4's "or zero if there is no viewport", asked once here rather than by each
   member. A viewport is the viewing area a NAVIGABLE presents a document in, so a document that is no longer
   being presented — HTML §7.3.1's not fully active, which is what a removed iframe's document becomes — has
   none, and `frame.contentWindow.innerWidth` after the removal is 0 rather than the size it used to be. */
bool viewport_exists(JSContext *ctx);

/* THIS REALM's viewport, in CSS pixels. Answered for the realm passed, never for a remembered one. */
double viewport_width(JSContext *ctx);
double viewport_height(JSContext *ctx);
/* CSSOM VIEW §4's `devicePixelRatio` — the ratio of device pixels to CSS pixels, which Media Queries §4.7's
   `resolution` feature reports as `dppx`. A modelled 1.0 is a UA with a non-HiDPI display, chosen for the same
   reason the size is. */
double viewport_device_pixel_ratio(JSContext *ctx);

/* CSSOM VIEW §4's `scrollX`/`scrollY` — "the x-coordinate, relative to the initial containing block origin, of
   the left of the viewport". DERIVED, not stored, and the derivation is in viewport.c: with no layout there
   are no descendant boxes, so §2's scrolling area of a viewport is exactly the ICB, and a scrolling box whose
   scrolling area is its own size has one valid scroll position. VisualViewport's `pageLeft`/`pageTop` are the
   second reader, which is why this is exported rather than written into the member. */
double viewport_scroll_x(JSContext *ctx);
double viewport_scroll_y(JSContext *ctx);

/* CSSOM VIEW §13.1 step 1, as the one question the resize steps ask: "has doc's viewport had its width or
   height changed since the last time these steps were run". LATCHES what it saw, so a caller that asks twice
   gets the change once — and the state it latches into is a per-realm record whose writes are ordinary property
   writes, so the COW delta captures them and each flow keeps its own timeline of the answer.
   THE FIRST RUN IN A FLOW IS NEVER A CHANGE. "Since the last time these steps were run" has no meaning before
   there has been one, which is why the record carries whether there HAS been one rather than being seeded with
   the dimensions at install time — a realm's intrinsics are built before its navigable has a document, so
   there is no viewport to seed from, and seeding it later inside whichever flow got there first would make that
   flow's baseline everyone's. */
bool viewport_resize_changed(JSContext *ctx);

#endif
