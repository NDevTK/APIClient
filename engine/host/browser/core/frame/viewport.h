/* THE VIEWPORT — CSS 2.1 §9.1.1 "The viewport" and §10.1's INITIAL CONTAINING BLOCK, which is the one piece of
 * geometry every other geometric answer in the platform is measured against, and CSSOM VIEW §4's WINDOW
 * EXTENSIONS, which are how a page reads it.
 *
 * IT IS A MODELLED UA CHOICE, NOT AN UNKNOWN, and it is the same kind of choice as rendering.c's refresh rate.
 * CSS says the viewport is "a window or other viewing area on the screen through which users consult a
 * document" and that its size is the UA's; it does not say what the size IS, and a UA running with no display
 * still has to answer, because the ICB is what `width: auto` resolves against. §Headless's missing piece is a
 * physical device, and the size of the area a document is laid out in exists without one.
 *
 * SO EVERY MEMBER HAS A REAL COMPUTED ANSWER — AND THE ONES REPORTING THE CHOICE CARRY IT AS THE EXAMPLE OF A
 * CONCOLIC. What stood here said the opposite: that every member "answers with a CONCRETE number" because "a
 * concolic there would model an ignorance this engine does not have". That is the collapse CLAUDE.md's
 * §Headless line forbids in the same sentence that permits the example — "never collapse a modelable value to
 * bare-concrete either; that deletes the fork and its coverage" — and it put ONE fact under two opposite
 * policies, because media_query_list.c had already made it concolic: `matchMedia('(max-width: 768px)').matches`
 * forked while `innerWidth < 768` did not, and both read `viewport_width` below. `innerWidth` is at least as
 * common a mobile gate as `matchMedia`, and the arm a concrete answer deletes is a responsive bundle's entire
 * mobile world — its router, its asset host, frequently its API base.
 *
 * WHICH MEMBERS, AND WHY NOT ALL OF THEM. The test is whether the model PICKED one point out of a range the
 * environment leaves free, or DERIVED the only value the rest of the model permits:
 *   - PICKED, so an environment SOURCE: the viewport's size (`innerWidth`/`innerHeight`); the client window's
 *     size (`outerWidth`/`outerHeight` — a UA can present the same viewport inside more or less chrome, which
 *     is exactly what makes `outerHeight - innerHeight` a question with two answers, and it is its own source
 *     for the reason screen.c gives for `availHeight` beside `height`); and `devicePixelRatio`, where `> 1` is
 *     the retina gate a bundle puts different assets and a different image host behind.
 *   - DERIVED, so CONCRETE: `scrollX`/`scrollY`, whose domain is a single point — with no layout the scrolling
 *     area IS the ICB, so the origin is the only valid scroll position, and forking it would run a page's
 *     scrolled code in a document that cannot scroll. It is also the half of this file that acquires a WRITER
 *     the moment there is a layout (CSSOM VIEW §3's perform-a-scroll), and a fact with a writer is per-flow
 *     STATE in the COW delta, never an environment source; minting one now would leave the writer and the
 *     source both answering it. And `screenX`/`screenY`, which are a POSITION derived from two facts already
 *     forkable at their own members — screen.c's available area and the client window's size — so a third
 *     independent source reaches no arm those two do not, while permitting a world where the window sits off
 *     the screen, which is the state `viewport_client_screen_x` asserts a UA cannot present.
 *
 * THE C SIDE ANSWERS THE EXAMPLE, DELIBERATELY, and that is the media_query.h layering for the reason it gives:
 * a C `if` over a concolic silently picks one arm. Every geometry function below answers a `double` — the
 * modelled viewport — and the concolic is minted at the JS boundary, by `viewport_env_value`, which is the
 * only function here that returns a JSValue and the only place a fork can be born. So every assert in this
 * component is over the example, or over a property that holds for the whole domain (`vp_long`'s integrality
 * is one: the members it guards are IDL `long`, so no member of the domain is fractional).
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
#include "core/css/css_length.h"

/* Declared once per AGENT: the §4 members' names and the per-realm slot their §13.1 latch lives in. It also
   REGISTERS the per-realm install, so no host has a line to remember. */
void viewport_init(JSContext *ctx);
void viewport_free(void);

/* IS THERE A VIEWPORT — CSSOM VIEW §4's "or zero if there is no viewport", asked once here rather than by each
   member. A viewport is the viewing area a NAVIGABLE presents a document in, so a document that is no longer
   being presented — HTML §7.3.1's not fully active, which is what a removed iframe's document becomes — has
   none, and `frame.contentWindow.innerWidth` after the removal is 0 rather than the size it used to be. */
bool viewport_exists(JSContext *ctx);

/* THE ONE SEAM a viewport-derived value crosses to become what the PAGE reads, and the ONE place its source
   key is spelled. `member` is the IDL name as a page writes it ("innerWidth", "visualViewport.width") and is
   the finding's SHAPE; `computed` is the modelled answer and is CONSUMED, becoming the concolic's EXAMPLE.
   THE DOCUMENT IS PART OF THE KEY, for media_query_list.c's reason: a child navigable's viewport is 300 CSS
   pixels wide and the top-level traversable's is 1280, so `innerWidth` is a different question in each and one
   key would let a branch taken in the parent decide the iframe's.
   Where no source overlay is installed — a conformance host — it hands `computed` back unchanged, which is
   what keeps these members testable against what CSSOM VIEW says a viewport of this size reports. */
JSValue viewport_env_value(JSContext *ctx, const char *member, JSValue computed);

/* THIS REALM's viewport, in CSS pixels — the MODELLED ANSWER, which is the example the member above carries.
   Answered for the realm passed, never for a remembered one. */
double viewport_width(JSContext *ctx);
double viewport_height(JSContext *ctx);
/* CSSOM VIEW §4's `devicePixelRatio` — the ratio of device pixels to CSS pixels, which Media Queries §4.7's
   `resolution` feature reports as `dppx`. A modelled 1.0 is a UA with a non-HiDPI display, chosen for the same
   reason the size is. */
double viewport_device_pixel_ratio(JSContext *ctx);

/* CSS 2.1 §10.1's INITIAL CONTAINING BLOCK — "the containing block in which the root element lives is a
   rectangle called the initial containing block. For continuous media, IT HAS THE DIMENSIONS OF THE VIEWPORT
   and is anchored at the canvas origin". So this is not a second geometry beside the two above, it IS them,
   stated in §10's own vocabulary because that is the vocabulary a layout asks in: core/layout/used_value.h's
   containing-block chain is a recursion, and it bottoms out here and nowhere else.
   IT IS ITS OWN ENVIRONMENT SOURCE ALL THE SAME, by element_view.h's test rather than in spite of it.
   `innerWidth` INCLUDES a rendered scroll bar and the ICB does not, so
   `innerWidth - parseInt(getComputedStyle(document.documentElement).width)` is a bundle measuring the scroll
   bar — a question with two answers, which is what makes them different facts even in a UA that renders no
   scroll bar and reports the same number for both. */
CssPx viewport_icb_width(JSContext *ctx);
CssPx viewport_icb_height(JSContext *ctx);

/* THE ONE SEAM A VALUE DERIVED FROM THE ICB CROSSES to become what the page reads, and the ONE switch over
   `CssEnvFact` — so a length that crosses to JS anywhere in this engine mints its domain here or not at all.
   `computed` is the SERIALIZED example the member's own IDL type demanded (CSSOM §6.7.2's `1264px` for a
   resolved value, §6's `long` for a client extent) and is CONSUMED. A length whose `env` is CSS_ENV_NONE is
   handed back unchanged — that is the positive statement css_length.h describes, not a missing domain. */
JSValue viewport_icb_derived(CssPx len, JSValue computed);

/* CSSOM VIEW §4's `scrollX`/`scrollY` — "the x-coordinate, relative to the initial containing block origin, of
   the left of the viewport". DERIVED, not stored, and the derivation is in viewport.c: no box in this model has
   GEOMETRY except the ICB itself (core/dom/element_view.h states the box model), so nothing extends §2's
   scrolling area of a viewport past the ICB, and a scrolling box whose scrolling area is its own size has one
   valid scroll position. VisualViewport's `pageLeft`/`pageTop` are the second reader, which is why this is
   exported rather than written into the member. */
double viewport_scroll_x(JSContext *ctx);
double viewport_scroll_y(JSContext *ctx);

/* CSSOM VIEW §2's SCROLLING AREA OF THIS REALM'S VIEWPORT — the ICB extended by the margin edges of all of the
   viewport's descendants' boxes, which is the ICB itself while no box in the model has geometry (see
   core/dom/element_view.h). Exported because THREE algorithms read it and none of them may state it for itself:
   §4's `scroll()` clamps against it, `scrollX`/`scrollY`'s single valid position is derived from it, and §6's
   `scrollWidth`/`scrollHeight` answer max(it, the viewport) for the root element. */
double viewport_scrolling_area_width(JSContext *ctx);
double viewport_scrolling_area_height(JSContext *ctx);

/* CSSOM VIEW §4's `scroll()` STEPS OVER THIS REALM'S VIEWPORT — the INTERNAL algorithm, which §2 is explicit
   that a member "said to call another method or attribute" must invoke rather than the page-visible member (so
   a page overriding `window.scroll` cannot change what `el.scrollTop = 10` on the root element does).
   `x`/`y` are the REQUESTED position, with §3.2's normalize-non-finite already applied by the caller whose IDL
   type carries it. The steps clamp that request into the viewport's scrolling area and abort at step 11 when
   the clamped position is the one the viewport already has — which is every request, while the scrolling area
   is the ICB, and viewport.c asserts exactly that rather than assuming it. So the write is not ignored: it is
   RUN, and the spec's own clamp is what makes it a no-op.
   IT IS NOT `window.scroll` — that member is still absent, and four unwritten steps across three files assert
   themselves against its arrival (rendering.c's update-the-rendering step 9, autofocus.c's §6.6.7 steps 4 and
   5.8, focus.c's §6.6.6 step 4) because it is the member whose existence would mean a scrolling box can
   actually be MOVED. Nothing here moves one. */
void viewport_scroll(JSContext *ctx, double x, double y);

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
