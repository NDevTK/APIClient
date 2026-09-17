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
 * NAMED RESIDUAL — ONE MARK KIND, WHICH IS A VOCABULARY AND NOT A SEQUENCE.
 * WHAT IS NOT COVERED: `DISPLAY_MARK_FILL_RECT` is the only kind, so a list can say that a rectangle is a
 * solid colour and can say nothing else. CSS 2.1 §E.2's step 2 block arm is three marks in a fixed sequence —
 * "background color of element", "background image of element", "border of element" — of which this is the
 * first; its step 7.2 sub-list reaches "the text" and the three decoration lines over and under it; and its
 * step 7.1 reaches "the replaced content, atomically", which is a SURFACE rather than a mark.
 * WHAT THE NEXT DIFF BUILDS: the BORDER mark, because it is the one whose operands all exist — CSS 2.1
 * §8.5 "Border properties"' four widths, styles and colours are each in lexbor's property registry, and
 * core/layout/used_value.h's `used_value_leading_border_px` already answers the width per side. The TEXT mark
 * is next and is the one with a contract: core/layout/text_run.h and core/fonts/open_type_metrics.h produced
 * the advances that core/dom/element_view.h reports as geometry, so a text mark carries THOSE advances and a
 * rasterizer that re-measures is comparing two engines rather than painting one. The IMAGE mark is blocked
 * where core/paint/box_paint.h's residual says it is blocked.
 * HOW ITS ABSENCE WOULD SHOW: a painted document is flat areas of colour with no borders, no text and no
 * images — every rectangle at the position CSS 2.1 §E.2 puts it and nothing inside any of them.
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
    DISPLAY_MARK_FILL_RECT = 0   /* one rectangle, filled with one sRGB colour */
} DisplayMarkKind;

/* ONE MARK. `rect` is x, y, width and height in CSSOM VIEW §6 "Extensions to the Element Interface"'s CLIENT
   COORDINATES, in that order — the same four numbers and the same order `element_view_bounding_box_px`
   answers, because that is where a box's rectangle comes from and two spellings of one rectangle is one
   rectangle too many. `color` is in sRGB and its alpha is in [0, 1]; both are asserted on append. */
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
