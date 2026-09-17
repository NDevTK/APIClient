/* THE INK — what a mark IS, and the ordered sequence of them that a painted stacking context comes to.
 * core/paint/paint_order.h states WHICH box's marks go down WHEN; this states WHAT a mark is and holds them
 * in the order that walk offered them.
 *
 * WHY A LIST AND NOT A SORTED SET, WHICH IS THE ONE THING A READER ARRIVING FROM core/paint/stacking_order.h
 * WILL REACH FOR. That component answers a total order over BOXES and is the answer for hit-testing and
 * occlusion; it cannot be the frame of a display list, and paint_order.h proves it out of CSS 2.1 §E.2
 * "Painting order"'s own text — step 4 walks the in-flow non-positioned block-level descendants laying each
 * box's background and border, and step 7 walks THE SAME SET laying each box's content, so for two such boxes
 * A and B with A first in tree order CSS 2.1 §E.2 lays A's border, then B's border, and only then A's text. No
 * comparator over boxes can say that. So the order is not a KEY this component could sort on; it is the
 * SEQUENCE the walk produced, and holding it is this component's whole statement. There is deliberately no
 * entry here that reorders, sorts or compares two marks: a second answer to the order question is a second
 * answer that is free to disagree with CSS 2.1 §E.2's.
 *
 * A RECTANGLE IS A `CssPx` AND NOT A `double`, AND THAT IS THE FIELD THAT MAKES A LIST INK FOR A WORLD RATHER
 * THAN INK FOR A MOMENT. core/frame/viewport.h makes the initial containing block's dimensions a PICKED
 * environment fact, and core/css/css_length.h carries the set of such facts a length is a joint function of on
 * the length itself. Every used value in a layout inherits that set, so a rectangle handed to a painter is very
 * often a function of the viewport — and a display list built out of flattened `double`s would be ink whose
 * provenance had been dropped between the layout that derived it and the surface that composites it.
 * core/paint/paint_order.h already names the consumer of that fact in its own words: a replaced element's
 * content is placed "atomically", which is a SURFACE, and "what such an item must additionally carry is the
 * WORLD it was built under, because a surface belongs to a world and not to a moment — two arms of one fork
 * produce two parent surfaces, and composing one against a child surface from a contradicting world fabricates
 * a timeline neither document was in". `display_list_env` is where that world is read off a finished list.
 * NOTHING HERE FORKS AND NOTHING HERE MINTS A CONCOLIC. What rides a mark is the EXAMPLE — the modelled number
 * the rasterizer draws with — and the SET OF FACTS it is a function of, which is exactly the split
 * css_length.h states: the C side computes on the example, and the domain is minted only at a boundary a PAGE
 * reads. A display list is not such a boundary: no page can read one, so there is nothing here to fork on.
 *
 * A COLOUR IS A `CssColor` AND IS NOT QUANTIZED HERE, for core/css/css_color.h's own reason. That component
 * keeps a colour's components unclamped and says why: a colour "can legitimately lie outside the destination's
 * gamut", and CSS Color 4 §11 "Converting Colors" gamut maps only when the destination "is a physical output
 * color space, such as a display". A display list is not a display. The clip and the 8-bit quantization belong
 * to whichever surface finally rasterizes, and doing either here would decide it once for every destination at
 * once — which is the same defect as answering one fact from one place for many agents. What this component
 * DOES require is that the colour has already been through CSS Color 4 §11's conversion to sRGB, because a
 * mark whose space were free to vary would make every consumer ask the cascade a question the painter already
 * asked. That is asserted on append.
 *
 * THE CANVAS IS A SECOND KIND AND NOT A RECTANGLE WITH A FLAG ON IT, BECAUSE ITS FOUR NUMBERS ARE A FACT ABOUT
 * A DIFFERENT THING. A `DISPLAY_MARK_FILL_RECT`'s rectangle IS the area: it came off a box, so a surface that
 * rasterizes somewhere larger leaves it exactly where it is. A `DISPLAY_MARK_FILL_CANVAS`'s rectangle is not
 * the area at all. CSS 2.1 §2.3.1 "The canvas" says "the canvas is infinite for each dimension of the space,
 * but rendering generally occurs within a finite region of the canvas, established by the user agent according
 * to the target medium", and CSS 2.1 §E.2 "Painting order" asks step 1 for the colour "over the entire canvas"
 * — so the AREA is unbounded and what the mark carries is the finite region THIS user agent established for
 * THIS medium. A rasterizer whose own region is larger must EXTEND a canvas fill and must NOT extend a
 * rectangle fill, and one kind carrying a rectangle could not tell it which of the two it was holding.
 *
 * SO WHY CARRY A RECTANGLE AT ALL, rather than leaving the extent off and letting whoever rasterizes supply
 * its own region — which is the design a reader will reach for, since a region is the SURFACE's and a list is
 * not a surface. The first reason is measurable rather than a preference: `display_list_env` is the union over
 * every coordinate of every mark, and `CSS_ENV_NONE` is a POSITIVE statement that this ink is a function of no
 * picked fact. A canvas mark with no coordinates contributes nothing to that union, so a list whose only ink
 * is a page background would report that its ink is determined outright — while the region is CSS 2.1 §10.1's
 * initial containing block, which core/frame/viewport.h makes a PICKED environment fact. The absence would not
 * read as an absence; it would read as the positive statement, and the arm under which this ink is different
 * ink would be gone with nothing to say so. The second is this header's own second paragraph: a list is ink
 * for a WORLD and not for a MOMENT, and a rasterizer that asked a realm for the viewport at raster time would
 * read whichever world it happened to be standing in, when two arms of one fork have two viewports.
 *
 * WHICH FINITE REGION, AND WHY IT SITS AT THE CLIENT ORIGIN WHATEVER THE SCROLL POSITION. CSS 2.1 §E.2 says of
 * the canvas that "It is infinite in extent and contains the root element. Initially, the viewport is anchored
 * with its top left corner at the canvas origin", and CSS 2.1 §10.1 "Definition of "containing block"" says
 * the initial containing block "has the dimensions of the viewport and is anchored at the canvas origin" for
 * continuous media. §2.3.1's own example of a user agent establishing a region is the screen one — such agents
 * "generally impose a minimum width and choose an initial width based on the dimensions of the viewport". So
 * for continuous media the region established is the initial containing block's rectangle, and in the CLIENT
 * coordinates every rectangle here is stated in — whose origin IS the viewport's top left corner — it sits at
 * (0, 0) wherever the viewport has been scrolled to, because the fill covers an infinite area and whatever
 * part of it the viewport shows is inside. The scroll position is not a missing operand here; it is MOOT,
 * which is a different thing, and is why nothing that builds one of these marks reads it.
 *
 * WHAT THE KIND DOES NOT DECIDE IS WHOSE COLOUR IT IS. CSS 2.1 §14.2 "The background" moves the background
 * properties of a root element onto the canvas, and moves a `body` child's onto it instead under a condition
 * §14.2 states; that is a question about a DOCUMENT and it is answered where documents are read, which is
 * core/paint/box_paint.h. This component is handed a colour and never asks where it came from.
 *
 * A MARK IS A VALUE AND THE LIST OWNS ITS ARRAY — there is no pointer on a mark and nothing here holds a
 * borrowed one, so a caller may build a mark on its stack and append it, and freeing the list frees everything
 * the list is. That is not a convenience: core/layout/used_value.h's rule and paint_order.h's are that NOTHING
 * about a painting order is stored, because the order is a function of the running flow's own tree and its own
 * cascade and a cached one would be shared state the COW delta does not swap. A list is therefore built per
 * call and freed by its builder, and a mark that pointed at anything would be a lifetime for someone to get
 * wrong at exactly the moment a flow parks.
 *
 * ALLOCATION IS A `CHECK` AND NOT A `DCHECK`, which is CLAUDE.md's rule for allocation and is also the right
 * one on the merits here: a dropped mark is ink that is silently absent from a sequence whose whole statement
 * is that it is complete and in order, and nothing downstream could tell that list from a shorter document.
 *
 * NAMED RESIDUAL — TWO MARK KINDS, WHICH ARE ONE MARK OVER TWO AREAS RATHER THAN TWO MARKS.
 * WHAT IS NOT COVERED: both kinds below are a SOLID COLOUR over an area, so a list can say that an area is one
 * colour and can say nothing else. CSS 2.1 §E.2's step 1 is TWO items and only the first has a kind here, the
 * second being "background image of element, over the entire canvas, anchored at the origin that would be used
 * if it was painted for the root element"; its step 2 block arm is three marks in a fixed sequence —
 * "background color of element", "background image of element", "border of element" — of which this vocabulary
 * has the first; its step 7.2 sub-list reaches "the text" and the three decoration lines over and under it;
 * and its step 7.1 reaches "the replaced content, atomically", which is a SURFACE rather than a mark.
 * WHAT THE NEXT DIFF BUILDS: the BORDER mark, because it is the one whose operands all exist — CSS 2.1
 * §8.5 "Border properties"' four widths, styles and colours are each in lexbor's property registry, and
 * core/layout/used_value.h's `used_value_leading_border_px` already answers the width per side. The TEXT mark
 * is next and is the one with a contract: core/layout/text_run.h and core/fonts/open_type_metrics.h produced
 * the advances that core/dom/element_view.h reports as geometry, so a text mark carries THOSE advances and a
 * rasterizer that re-measures is comparing two engines rather than painting one. The IMAGE mark is ONE gap and
 * not one per step, because what every image item of every step wants is the same operand: an `<image>` that
 * has become PIXELS. This engine's `<image>` road ends at a validity test — core/css/css_image.h answers
 * whether a component value matches css-images-3 §2 "Image Values: the <image> type" and deliberately keeps
 * the author's own bytes — so nothing anywhere turns a `<url>` into anything a surface could composite, and
 * the diff that lands the image mark is the one that makes such a thing exist.
 * HOW ITS ABSENCE WOULD SHOW: a painted document is flat areas of colour with no borders, no text and no
 * images — every area at the position CSS 2.1 §E.2 puts it and nothing drawn inside any of them.
 * RETIREMENT: this record loses a clause as each kind lands, and goes when every mark CSS 2.1 §E.2's
 * sub-lists name has a kind here. */
#ifndef ENGINE_HOST_BROWSER_CORE_PAINT_DISPLAY_LIST_H
#define ENGINE_HOST_BROWSER_CORE_PAINT_DISPLAY_LIST_H

#include <stddef.h>

#include "core/css/css_color.h"
#include "core/css/css_length.h"

/* WHAT A MARK SAYS. The enum is a VOCABULARY and its order carries no meaning whatever — CSS 2.1 §E.2's order
   is the LIST's, and a reader who takes a kind's ordinal for a paint order has read the wrong component. */
typedef enum {
    DISPLAY_MARK_FILL_RECT = 0,  /* one rectangle, filled with one sRGB colour */
    DISPLAY_MARK_FILL_CANVAS     /* the CANVAS, filled with one sRGB colour — the rectangle is the finite
                                    region this user agent established for it, never the extent of the fill */
} DisplayMarkKind;

/* ONE MARK. `rect` is x, y, width and height in CSSOM VIEW §6 "Extensions to the Element Interface"'s CLIENT
   COORDINATES, in that order — the same four numbers and the same order `element_view_bounding_box_px`
   answers, because that is where a box's rectangle comes from and two spellings of one rectangle is one
   rectangle too many. `color` is in sRGB and its alpha is in [0, 1]; both are asserted on append.
   WHAT `rect` MEANS IS THE KIND'S, and the two kinds do not mean the same thing by it: for a
   `DISPLAY_MARK_FILL_RECT` it is the area to fill, and for a `DISPLAY_MARK_FILL_CANVAS` it is the RENDERED
   REGION of an area CSS 2.1 §2.3.1 "The canvas" makes infinite. The header's own paragraph on the second kind
   is why a rectangle rides one at all and why it is anchored at the client origin. */
typedef struct {
    DisplayMarkKind kind;
    CssPx           rect[4];
    CssColor        color;
} DisplayMark;

/* A SEQUENCE OF MARKS, in the order CSS 2.1 §E.2 "Painting order" offered them. A zeroed struct is a valid
   EMPTY list — `display_list_init` states that rather than arranging it — so a list that has never been
   appended to and a list whose builder found nothing to paint are the same object, which is correct: both are
   a document with no ink and neither is an absence of an answer. */
typedef struct {
    DisplayMark *v;
    size_t       n;
    size_t       cap;
} DisplayList;

void display_list_init(DisplayList *dl);

/* Releases the array. Leaves an EMPTY list rather than a freed one, so a double free is not reachable and a
   builder that frees on its own failure path and again at its exit is correct. */
void display_list_free(DisplayList *dl);

/* APPENDS A COPY of `mark` to the end — which is the ONLY way ink enters a list, so the sequence a list holds
   is the sequence its builder produced and there is no second road for a mark to arrive out of order. */
void display_list_append(DisplayList *dl, const DisplayMark *mark);

/* THE UNION OF THE ENVIRONMENT FACTS EVERY MARK'S GEOMETRY IS A FUNCTION OF — the WORLD this ink belongs to,
   read off a finished list. `CSS_ENV_NONE` is a POSITIVE statement and not an absence, exactly as it is on a
   single length: the list is ink this cascade and this layout determined outright, so there is no arm of any
   environment fact under which it would be different ink. */
CssEnvSet display_list_env(const DisplayList *dl);

#endif
