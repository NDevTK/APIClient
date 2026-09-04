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
 *     the retina gate a bundle puts different assets and a different image host behind. AND THE READER'S
 *     DEFAULT FONT SIZE, which is not a member of this component and is a fact by this component's test:
 *     css-fonts-4 §2.5 (Font size: the font-size property) leaves what `medium` computes to entirely free —
 *     an `<absolute-size>` keyword "refers to an entry in a table of font sizes COMPUTED AND KEPT BY THE USER
 *     AGENT", and §2.5.1 (Absolute Size Keyword Mapping Table), which is that table, adds that "the user agent
 *     may fine-tune these values for different fonts or different types of display devices" — so nothing in
 *     the model derives it, and a `rem`-sized layout hangs its whole responsive ladder off the one number
 *     `parseFloat(getComputedStyle(document.documentElement).fontSize)` reports.
 *     core/css/font_size_functions.h picks it and owns the arithmetic; the row for it is in the seam below,
 *     because that seam's contract is WHICH facts exist and what each is called rather than where any of them
 *     is measured.
 *   - DERIVED, so CONCRETE: `scrollX`/`scrollY`, whose domain is a single point — with no layout the scrolling
 *     area IS the ICB, so the origin is the only valid scroll position, and forking it would run a page's
 *     scrolled code in a document that cannot scroll. It is also the half of this file that acquires a WRITER
 *     the moment there is a layout (CSSOM VIEW §3's perform-a-scroll), and a fact with a writer is per-flow
 *     STATE in the COW delta, never an environment source; minting one now would leave the writer and the
 *     source both answering it.
 *   - DERIVED FROM PICKED FACTS, so a JOINT SOURCE — a THIRD answer, and the one this test used to be missing.
 *     `screenX`/`screenY` are a POSITION computed from screen.c's available area and this window's own size,
 *     both of which are picked and forkable at their own members. That used to be read as a reason to answer
 *     CONCRETE, in this header's own words and not in CSSOM VIEW's — "a third independent source reaches no
 *     arm those two do not" — and the premise is right while the conclusion is wrong twice.
 *     A joint (solver/concolic.h's `concolic_source_wrap_joint`) is NOT a third
 *     independent source: it adds no free parameter and carries the one example the arithmetic produced, so
 *     the objection was to a design nobody proposed. And the arms are NOT the same arms: `window.screenX < 100`
 *     is its own predicate with its own constraint key, so against a bare `double` it does not fork at all,
 *     and a flow that narrowed `screen.availWidth` has said nothing that could decide it — which is exactly
 *     the non-composition the joint's own paragraph calls the sound direction. So the test is not two-way but
 *     three: a fact the model PICKED is a scalar source; a fact the model COMPUTED FROM PICKED FACTS is a
 *     joint over them; and only a fact the rest of the model leaves ONE value for is concrete.
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

/* THE ONE SEAM A VALUE DERIVED FROM THIS COMPONENT'S FACTS CROSSES to become what the page reads, and the ONE
   table over `CssEnvFact` — so a length that crosses to JS anywhere in this engine mints its domain here or
   not at all. The facts are the ICB's two dimensions and the DEVICE PIXEL RATIO, which css-values §6's
   snap-a-length-as-a-line-width divides a computed `border-*-width` by, so a page measuring a border is asking
   the same question `devicePixelRatio > 1` asks and gets the same source key for it.
   A LENGTH IS A FUNCTION OF A SET OF THEM, AND THE SET IS ONE DOMAIN. The used content width of a `width: auto`
   box with a real border is `containing block − margins − paddings − snapped borders`, so it moves with the ICB
   AND with the ratio, and `100vmin` takes both viewport axes as operands in a single token. Each fact in the
   set contributes one member of solver/concolic.h's JOINT source identity, which is where the ordering and the
   set's own invariants live; this seam contributes only WHICH facts and what each is called — including §4's
   "or zero if there is no viewport", which is asked PER FACT because a rectangle a navigable presents and an
   output device's ratio are absent under different conditions.
   `computed` is the SERIALIZED example the member's own IDL type demanded (CSSOM §6.7.2's `1264px` for a
   resolved value, §6's `long` for a client extent) and is CONSUMED. A length whose `env` is CSS_ENV_NONE is
   handed back unchanged — that is the positive statement css_length.h describes, not a missing domain. */
JSValue viewport_env_derived(CssPx len, JSValue computed);

/* CSSOM VIEW §4's `scrollX`/`scrollY` — "the x-coordinate, relative to the initial containing block origin, of
   the left of the viewport". STORED, not derived: CSSOM VIEW §3.1 "Scrolling"'s perform a scroll writes it
   through `viewport_set_scroll_position` below and nothing else may, so these are the read of what §3.1 wrote.
   IT IS PER-FLOW STATE, held on a per-realm record whose writes are ordinary property writes — so the COW delta
   captures it, two flows that scrolled one document differently read back different numbers, and a parked flow
   resumes standing where it was. viewport.c states why a `double` in a static could not be any of those.
   THE DERIVATION THAT USED TO STAND HERE IS RETIRED and is restated in capitals so nobody re-derives it: NO BOX
   IN THIS MODEL HAS GEOMETRY EXCEPT THE ICB, SO NOTHING EXTENDS §2's SCROLLING AREA OF A VIEWPORT PAST IT, AND
   A SCROLLING BOX WHOSE SCROLLING AREA IS ITS OWN SIZE HAS ONE VALID SCROLL POSITION. §2's viewport row and
   §3.1 retired the two halves of that in turn.
   VisualViewport's `pageLeft`/`pageTop` are the second reader, which is why this is exported rather than
   written into the member. */
double viewport_scroll_x(JSContext *ctx);
double viewport_scroll_y(JSContext *ctx);

/* CSSOM VIEW §3.1's INSTANT SCROLL OF THIS REALM'S VIEWPORT — the ONE writer of the position above, and it is
   exported for exactly one caller (core/dom/perform_scroll.c). It is not `viewport_scroll` below: that is §4's
   thirteen steps, which ask §4's own questions and END here; this is the write those steps decided on, and
   §6.1's route reaches it through the same §3.1 without going through §4 at all.
   `x`/`y` ARE ALREADY CLAMPED by whichever of the two algorithms decided them — they are the same four rows
   over one fact (core/dom/element_scrolling.h) — and the postcondition of that clamp is asserted here rather
   than the clamp being re-run, so a route that skipped it crashes instead of placing the viewport outside its
   own scrolling area. */
void viewport_set_scroll_position(JSContext *ctx, double x, double y);

/* THE `scrollX`/`scrollY` ATTRIBUTES' OWN ANSWER — the derivation above plus §4's own "or zero if there is no
   viewport", which is a DIFFERENT question from the derivation and belongs to the MEMBER. `vertical` false is
   the x axis.
   IT IS HERE BECAUSE THREE ALGORITHMS INVOKE THE ATTRIBUTE BY NAME AND §2 SAYS THEY MUST: "when a method or an
   attribute is said to call another method or attribute, the user agent must invoke its INTERNAL API for that
   attribute" (CSSOM VIEW §2 Terminology). §4's own member is one caller, four steps of §6's scroll members are
   the second, and §10's `pageX`/`pageY` step 2 — "let offset be the value of the scrollX attribute of the
   event's associated Window object, if there is one, or zero otherwise" — is the third. The `viewport_exists`
   test written once per caller is ONE fact with three spellings, and the third spelling is where it goes
   wrong; it had two already. */
double viewport_window_scroll(JSContext *ctx, bool vertical);

/* CSSOM VIEW §2's SCROLLING AREA OF THIS REALM'S VIEWPORT — the ICB extended by the margin edges of all of the
   viewport's descendants' boxes. THE EXTENSION IS COMPUTED, by core/layout/scrolling_area.h's VIEWPORT entry:
   §2's table has two columns, one walk answers both, and this file owns only what the ICB is (CSS 2.2 §10.1
   gives it "the dimensions of the viewport") and hands that in. It ANSWERED THE ICB ALONE for as long as the
   walk had no viewport row, which was wrong for every document taller than its viewport and showed as
   `document.documentElement.scrollHeight` reporting 720 for a 1400-pixel-tall page through the third reader
   below; that is built, so this is a real extreme and the answer grows with the layout.
   Exported because THREE algorithms read it and none of them may state it for itself:
   §4's `scroll()` clamps against it, `scrollX`/`scrollY`'s single valid position is derived from it, and §6's
   `scrollWidth`/`scrollHeight` answer max(it, the viewport) for the root element. */
double viewport_scrolling_area_width(JSContext *ctx);
double viewport_scrolling_area_height(JSContext *ctx);

/* CSSOM VIEW §4's `scroll()` STEPS OVER THIS REALM'S VIEWPORT — the INTERNAL algorithm, which §2 is explicit
   that a member "said to call another method or attribute" must invoke rather than the page-visible member (so
   a page overriding `window.scroll` cannot change what `el.scrollTop = 10` on the root element does).
   `x`/`y` are the REQUESTED position, with §3.2's normalize-non-finite already applied by the caller whose IDL
   type carries it; `behavior` is §4's `ScrollBehavior` keyword the caller's dictionary carried, which step 12
   hands to §3.1 and which is `auto` for the `scrollTop`/`scrollLeft` setter's two window arms because §6 gives
   them no options to carry one. Steps 7 and 8 clamp the request into the viewport's scrolling area — one step
   per axis, each a two-armed switch on §2's OVERFLOW DIRECTIONS, and BOTH arms are written — step 10 aborts
   when the clamped position is the one the viewport already has, and steps 11 to 13 PERFORM A SCROLL of it
   (core/dom/perform_scroll.h). SO THIS MOVES THE VIEWPORT, and the sentence that stood here — NOTHING HERE
   MOVES A BOX YET, AND §3.1 IS THE ONE THING LEFT BETWEEN THIS AND THAT — is what the arrival of §3.1 retired.
   THESE NUMBERS ARE THE THIRTEEN TOP-LEVEL STEPS of the edition engine/specindex/cssomview.json is keyed to.
   They stood one lower from step 4 onward, because §4's clamp is TWO steps each holding a two-armed
   `<dl class="switch">` and a flat count of the arms reads that pair as four.
   IT IS NOT `window.scroll` — that member is a separate question. §4's three Window members are §4's argument
   questions plus a call to this algorithm, so installing them would give a page a second spelling of what
   `documentElement.scrollTop = n` already reaches and would change nothing this file does. That is worth
   stating because six sites in five files used to assert themselves against the NAME `scrollTo` on the global
   as a stand-in for "a scrolling box can be MOVED", and this header used to name four of them as though the
   member were the capability. It is not, and the sites ask core/dom/element_scrolling.h's
   `element_scrolling_box_can_move` instead — WHICH §3.1's ARRIVAL DID NOT RETIRE, and that is worth saying
   because it reads as though it should have. That question is about SLACK, not about whether a perform-a-scroll
   exists: where §2's scrolling area equals the viewport, every arm of the clamp above lands on the origin, so a
   box cannot be anywhere else no matter how many algorithms are written; where it does not, one can. §3.1 is
   what makes that comparison LOAD-BEARING — it used to be the outer of two reasons, with "nothing reaches
   §3.1" behind it — and load-bearing is not retired. */
void viewport_scroll(JSContext *ctx, double x, double y, const char *behavior);

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
