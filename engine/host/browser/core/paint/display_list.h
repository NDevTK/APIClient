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
 * THE BORDER IS A THIRD KIND AND IT IS ONE MARK FOR THE BOX, NEVER ONE PER SIDE — WHICH IS THE DESIGN A
 * READER WILL REACH FOR, SINCE THE FOUR SIDES HAVE FOUR WIDTHS, FOUR STYLES AND FOUR COLOURS AND NOTHING
 * OBVIOUSLY TIES THEM TOGETHER. What ties them together is that CSS 2.1 §E.2 "Painting order" lists ONE item.
 * Its step 2 block arm is "background color of element unless it is the root element", "background image of
 * element unless it is the root element", "border of element"; its step 4 block arm is the same three without
 * the root clause; and its step 7.2.1 sub-list reaches "border of element" the same way. Three sub-lists, and
 * in each of them the border is a SINGLE item of the sequence. This header's opening paragraph is why that
 * settles it: the order a list holds is its whole statement and there is deliberately no entry that sorts or
 * compares two marks, so FOUR marks where CSS 2.1 §E.2 has one would not be four ways of saying the same
 * thing — they would BE an order over the four sides, stated by whichever loop appended them, and CSS 2.1
 * states none. CSS 2.1 §8.5 "Border properties" agrees from the other side in its own first sentence: the
 * properties "specify the width, color, and style of the border area of a box", which is ONE area belonging
 * to a box rather than four areas belonging to four sides.
 *
 * AND A PER-SIDE RECTANGLE WOULD NOT BE AN AREA CSS 2.1 DEFINES. Two adjacent sides meet in a region that is
 * not a rectangle and belongs to neither of them, and CSS 2.1 says nothing whatever about it — the string
 * "corner" does not occur anywhere in CSS 2.1 §8 "Box model". So a mark carrying one side's rectangle would
 * have to state an area no sentence of CSS 2.1 states, which is the same refusal core/paint/paint_order.h
 * makes about the collapsing model's painting order: where CSS 2.1 states no answer this engine may not
 * invent one. One mark carrying all four sides hands a rasterizer BOTH sides of every corner, which is what
 * resolving one needs, and leaves the resolution where the destination is — exactly as the quantization of a
 * colour is left there.
 *
 * A ZERO-WIDTH SIDE IS AN ANSWER AND A ZERO-WIDTH BOX IS NO MARK, AND THOSE ARE NOT THE SAME DECISION.
 * CSS 2.1 §8.5.3 "Border style: 'border-top-style', 'border-right-style', 'border-bottom-style',
 * 'border-left-style', and 'border-style'" says of `none` that it is "No border; the computed border width
 * is zero", so a zero width is something the cascade COMPUTED and not something nobody derived. Because the
 * mark is per BOX there is no arm here that omits a side: the zero is stated positively beside its three
 * siblings, and that is one more thing the per-side design could not do — under it a zero side would have to
 * be left out, and a consumer could not tell a side that was omitted from a side nobody asked about. A box
 * whose four widths are ALL zero gets NO MARK AT ALL, by the same rule core/paint/box_paint.c already
 * applies to an alpha of zero: ink that changes no pixel is a mark nothing downstream could distinguish from
 * its absence.
 *
 * `none` AND `hidden` ARE BOTH IN THE VOCABULARY THOUGH NEITHER EVER PAINTS, because CSS 2.1 §8.5.3 keeps
 * them apart and says exactly why: `hidden` is "Same as 'none', except in terms of border conflict resolution
 * for table elements". A mark states the style the cascade computed; collapsing two values CSS 2.1
 * distinguishes would be this component answering CSS 2.1 §17.6.2 "The collapsing border model"'s question in
 * advance and getting it wrong for the one case that separates them.
 *
 * THE FOUR WIDTHS ARE UNION-VISIBLE IN `display_list_env` AND THAT IS NOT A DETAIL — it is the same property
 * the canvas kind's rectangle exists for, one field over. core/layout/used_value.h states that a
 * `border: 1px solid` "arrives carrying the DEVICE PIXEL RATIO's" fact, because css-values §6 snaps a border
 * width to a whole number of device pixels. So a border width is very often a function of a PICKED
 * environment fact, and a union taken over `rect` alone would report `CSS_ENV_NONE` for a list whose only ink
 * is a bordered box at determined coordinates — a POSITIVE statement that this ink is the same ink under
 * every arm, made about ink that moves. The widths are therefore `CssPx` and not `double`, exactly as the
 * rectangle is, and the union reads them.
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
 * THE TEXT IS A FOURTH KIND AND IT IS ONE MARK PER CHARACTER, NEVER ONE PER RUN — WHICH IS THE OPPOSITE OF
 * THE CHOICE THE BORDER PARAGRAPH ABOVE MAKES, AND THE TWO ARE DECIDED BY ONE RULE RATHER THAN BY TASTE. That
 * paragraph refuses four marks for one border because CSS 2.1 §E.2 "Painting order" lists ONE item and states
 * NO ORDER over the four sides, so four marks would BE an order the standard does not give. A run of text is
 * the other case of the same rule: §E.2's step 7.2.1 reaches "the text" inside a sub-list that is explicitly
 * ordered — "in tree order" over the element's inline-level children and its runs of text — and the order
 * WITHIN a run is the one core/layout/text_run.h's [UAX14] pass and css-text-3 §4.1.2's trimming already
 * computed and core/layout/line_box.h already states as a position per character. So splitting a run invents
 * nothing: the sequence is read off a derivation this engine performed, where splitting a border would have to
 * invent one. The precedent is about not inventing ORDER and never about cardinality.
 * AND IT IS WHAT KEEPS THE PARAGRAPH BELOW TRUE. A run's advances are VARIABLE-LENGTH — one per character —
 * so a per-run mark could only carry them behind a POINTER, and the next paragraph's rule is that there is no
 * pointer on a mark and nothing here holds a borrowed one. The two could not both hold. A per-character mark
 * carries the ADVANCES AS POSITIONS, which discharges the same contract more strongly than a list of widths
 * would: a consumer handed a pen position per character has no pen to advance and therefore nothing it could
 * re-measure with, so "a rasterizer that re-measures is comparing two engines rather than painting one" is
 * not merely required of it but unreachable by it.
 * WHAT WOULD REFUTE THIS is a paint operation defined over a RUN rather than over a character — a filter, a
 * shadow, or one of the three decoration lines §E.2's step 7.2.1 lists over and under the text, whose geometry
 * spans the run and which a per-character mark cannot state. This vocabulary has none of them and neither does
 * anything upstream of it. WHEN ONE ARRIVES THE ANSWER IS A SECOND KIND carrying that run-level geometry
 * beside these marks, and never a pointer on this one: §E.2 lists each decoration line as its own item of the
 * same sub-list, so a second kind is what the standard's own enumeration already says they are.
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
 * NAMED RESIDUAL — THE KINDS CSS 2.1 §E.2's SUB-LISTS NAME AND THIS VOCABULARY STILL HAS NO WORD FOR.
 * WHAT IS NOT COVERED: every remaining item is an IMAGE, a DECORATION LINE or a SURFACE. CSS 2.1 §E.2's step
 * 1 is TWO items and only the first has a kind here, the second being "background image of element, over the
 * entire canvas, anchored at the origin that would be used if it was painted for the root element"; its step
 * 2 block arm is three marks in a fixed sequence — "background color of element", "background image of
 * element", "border of element" — of which this vocabulary has the first and the LAST and not the middle
 * one; its step 7.2.1 sub-list reaches "the text", which HAS a kind now, and the three decoration lines over
 * and under it, which do not; and its step 7.1 reaches "the replaced content, atomically", which is a SURFACE
 * rather than a mark.
 * WHAT THE NEXT DIFF BUILDS: the IMAGE mark, which is ONE gap and not one per step, because what every image
 * item of every step wants is the same operand: an `<image>` that has become PIXELS. This engine's `<image>`
 * road ends at a validity test — core/css/css_image.h answers whether a component value matches
 * css-images-3 §2 "Image Values: the <image> type" and deliberately keeps the author's own bytes — so
 * nothing anywhere turns a `<url>` into anything a surface could composite, and the diff that lands the
 * image mark is the one that makes such a thing exist.
 * HOW ITS ABSENCE WOULD SHOW: a painted document has its text but no pictures — every background image, every
 * `list-style-image` and every replaced element's content missing, with the area CSS 2.1 §E.2 puts each one
 * in drawn in whatever colour sits under it, and no underline or strikethrough on any text that declares one.
 * RETIREMENT: this record loses a clause as each kind lands, and goes when every mark CSS 2.1 §E.2's
 * sub-lists name has a kind here.
 *
 * RETIRED CLAUSE — THE TEXT MARK, KEPT BECAUSE ITS NEXT-DIFF HALF NAMED A MECHANISM THIS HEADER'S OWN
 * OWNERSHIP RULE FORBIDS, AND A READER WHO RE-DERIVES IT WILL REACH FOR THE SAME ONE. It read, in one run:
 * `WHAT THE NEXT DIFF BUILDS: the TEXT mark, and it is the one with a contract: core/layout/text_run.h and core/fonts/open_type_metrics.h produced the advances that core/dom/element_view.h reports as geometry, so a text mark carries THOSE advances and a rasterizer that re-measures is comparing two engines rather than painting one.`
 * Its SPEC half is exact and is why the kind exists at all. Its MECHANISM half is not buildable as written and
 * was never buildable: a run's advances are ONE PER CHARACTER and therefore VARIABLE-LENGTH, so "a text mark
 * carries THOSE advances" can only mean a mark holding a pointer to an array — and this header's own
 * paragraph three above the type says there is no pointer on a mark and nothing here holds a borrowed one. The
 * two sentences were in one file, several paragraphs apart, and could not both hold. THE CLAUSE WAS FALSE AT
 * BIRTH RATHER THAN STALE: the ownership rule predates it and is unchanged, so nothing about the tree moving
 * made it wrong. What it teaches is the check it skipped — a clause naming a MECHANISM is read against the
 * invariants of the component it is written in, and a per-run mark was refused by an invariant on the same
 * page. The three ways out were `one mark per glyph, fixed size, no pointer`, `one mark per run with an owned
 * pointer, changing display_list_append's copy semantics and display_list_free` and `a fixed-capacity inline
 * array`. The THIRD is a bound and CLAUDE.md's §NO BOUNDS forbids it outright — a run longer than the capacity
 * would be text this engine decided not to draw. The SECOND is the one the clause named, and it rewrites this
 * component's ownership contract for every kind rather than adding one: a mark that owned an allocation would
 * make the per-flow COW delta capture a GRAPH where it captures an array, which is what
 * core/paint/paint_order.h's rule about nothing being stored is protecting. The FIRST is what landed, and the
 * paragraph above states the argument for it in its own right rather than by elimination. */
#ifndef ENGINE_HOST_BROWSER_CORE_PAINT_DISPLAY_LIST_H
#define ENGINE_HOST_BROWSER_CORE_PAINT_DISPLAY_LIST_H

#include <stddef.h>
#include <stdint.h>

#include "core/css/css_color.h"
#include "core/css/css_length.h"

/* WHAT A MARK SAYS. The enum is a VOCABULARY and its order carries no meaning whatever — CSS 2.1 §E.2's order
   is the LIST's, and a reader who takes a kind's ordinal for a paint order has read the wrong component. */
typedef enum {
    DISPLAY_MARK_FILL_RECT = 0,  /* one rectangle, filled with one sRGB colour */
    DISPLAY_MARK_FILL_CANVAS,    /* the CANVAS, filled with one sRGB colour — the rectangle is the finite
                                    region this user agent established for it, never the extent of the fill */
    DISPLAY_MARK_BORDER,         /* ONE box's border, all four sides — the rectangle is its BORDER BOX and the
                                    four sides are drawn INWARD from its four edges. See the header's own
                                    paragraph for why this is one mark and not four */
    DISPLAY_MARK_GLYPH           /* ONE placed character of CSS 2.1 §E.2's step 7.2.1 "the text" — a code
                                    point, an em and a PEN POSITION. It carries no rectangle at all. See the
                                    header's own paragraph for why this is one mark per CHARACTER and not one
                                    per run */
} DisplayMarkKind;

/* CSS 2.1 §8.5.3 "Border style: 'border-top-style', 'border-right-style', 'border-bottom-style',
   'border-left-style', and 'border-style'"' `<border-style>` VALUE TYPE, entire and in that section's own
   order, which is also the order core/css/css_shorthand.c's grammar lists it in.
   IT IS A VOCABULARY AND NOT A SECOND GRAMMAR, which is the line between this enum and that list. That one
   decides what the PARSER admits, so that a `border-style: nope` is dropped by CSS Syntax instead of reaching
   a computed value; this one is what a MARK SAYS, which is this component's to define exactly as
   `DisplayMarkKind` is. They are two jobs over one set of ten names and they are kept from drifting by the
   assert at the mapping in core/paint/box_paint.c: a computed style outside these ten has come THROUGH that
   grammar, so it is this engine's two lists disagreeing rather than anything a document declared.
   `none` AND `hidden` BOTH PAINT NOTHING AND ARE BOTH HERE — CSS 2.1 §8.5.3 gives `none` as "No border; the
   computed border width is zero" and `hidden` as "Same as 'none', except in terms of border conflict
   resolution for table elements", so the two differ in exactly one place and this vocabulary is not where
   that difference is decided. */
typedef enum {
    DISPLAY_BORDER_STYLE_NONE = 0,
    DISPLAY_BORDER_STYLE_HIDDEN,
    DISPLAY_BORDER_STYLE_DOTTED,
    DISPLAY_BORDER_STYLE_DASHED,
    DISPLAY_BORDER_STYLE_SOLID,
    DISPLAY_BORDER_STYLE_DOUBLE,
    DISPLAY_BORDER_STYLE_GROOVE,
    DISPLAY_BORDER_STYLE_RIDGE,
    DISPLAY_BORDER_STYLE_INSET,
    DISPLAY_BORDER_STYLE_OUTSET
} DisplayBorderStyle;

/* ONE SIDE OF ONE BORDER — the three things CSS 2.1 §8.5 "Border properties" says a border has, in its own
   words: the properties "specify the width, color, and style of the border area of a box".
   `width` IS THE USED WIDTH and is a `CssPx` for the header's own reason — core/layout/used_value.h states
   that a `border: 1px solid` carries the DEVICE PIXEL RATIO's fact, so a `double` here would drop a picked
   fact between the layout that derived it and `display_list_env`. Zero is a legitimate answer and means CSS
   2.1 §8.5.3's `none` or `hidden`, or a width the cascade computed to zero; which of those it was is the
   `style` beside it and is never inferred from the number. */
typedef struct {
    CssPx              width;
    DisplayBorderStyle style;
    CssColor           color;
} DisplayBorderSide;

/* ONE PLACED CHARACTER — the three things a consumer needs to draw it and no fourth. `origin` is the PEN
   POSITION in the same CLIENT COORDINATES every rectangle here is stated in, and `origin_y` is the BASELINE
   rather than a top edge, which is the field a reader is most likely to take for the other thing: CSS 2.2
   §10.8's step 3 makes a line box "the distance between the uppermost box top and the lowermost box bottom"
   and its baseline the line's own maximum `A'` below that top, so the baseline is the one coordinate every
   box on the line hangs from and core/layout/line_box.h reports exactly it.
   `em` IS THE USED `font-size` AND IS A `CssPx` FOR THE HEADER'S OWN REASON — css-values-4 §6.1.1
   "Font-relative Lengths: the em, rem, ex, rex, cap, rcap, ch, rch, ic, ric, lh, rlh units" defines the `em`
   as the element's computed `font-size`, which css-fonts-4 §2.5's percentage and its keywords make a function
   of the ROOT element's and therefore of whatever picked facts that one is a function of. A `double` here
   would drop that between the cascade that derived it and `display_list_env`, exactly as it would on a
   rectangle, and the union reads all three of these lengths.
   THERE IS NO GLYPH ID AND NO FACE. css-fonts-4 §5.2 "Matching font styles"' first available font is
   core/css/font_metrics.h's ONE face and the 'cmap' that selects a glyph for a code point is that file's, so
   a mark carrying an id would be a second holder of the selection this engine answers from one place — and a
   mark that named a face would be holding a pointer, which the header forbids outright. What rides is what
   the DOCUMENT said, and which glyph it draws is answered where the face is. */
typedef struct {
    uint32_t cp;                 /* the code point, as css-text-3 §4.1.1's Phase I left it */
    CssPx    origin_x, origin_y; /* the PEN POSITION; `origin_y` is the BASELINE */
    CssPx    em;                 /* css-values-4 §6.1.1's em — the used `font-size` this glyph is scaled to */
} DisplayGlyph;

/* ONE MARK. `rect` is x, y, width and height in CSSOM VIEW §6 "Extensions to the Element Interface"'s CLIENT
   COORDINATES, in that order — the same four numbers and the same order `element_view_bounding_box_px`
   answers, because that is where a box's rectangle comes from and two spellings of one rectangle is one
   rectangle too many. `color` is in sRGB and its alpha is in [0, 1]; both are asserted on append.
   WHAT `rect` MEANS IS THE KIND'S, and the two kinds do not mean the same thing by it: for a
   `DISPLAY_MARK_FILL_RECT` it is the area to fill, and for a `DISPLAY_MARK_FILL_CANVAS` it is the RENDERED
   REGION of an area CSS 2.1 §2.3.1 "The canvas" makes infinite. The header's own paragraph on the second kind
   is why a rectangle rides one at all and why it is anchored at the client origin. For a
   `DISPLAY_MARK_BORDER` it is the BORDER BOX — the same four numbers `element_view_bounding_box_px` answers
   for the background, because CSS 2.1 §8.1 "Box dimensions"' border edge is the outer edge of both — and each
   of `side`'s four widths is measured INWARD from the corresponding edge of it.
   `side` IS INDEXED top, right, bottom, left, which is the order every four-side rule in CSS states and the
   order CSS 2.1 §8.5.1 defines its own shorthand over; it is also the order
   `used_value_border_widths_px` writes, so the index cannot come apart from the derivation by a rotation.
   A `DISPLAY_MARK_GLYPH` USES NO `rect` AT ALL, which is the one kind for which that is true and is not an
   omission. Its ink extent is a property of the FACE's own contours — core/css/font_metrics.h answers the
   shape and this component may not — and CSS 2.1 §E.2's step 7.2.1 asks for "the text" rather than for a box
   around it, so a rectangle here would be a second answer to a question the face already owns. The two rect
   invariants below are therefore asked over a SWITCH like every other field's, and not of every mark.
   WHICH FIELDS A MARK USES IS ITS KIND'S, exactly as what `rect` MEANS is: `color` is the two FILL kinds' AND
   THE GLYPH KIND'S — CSS 2.1 §14.1 "Foreground color: the 'color' property" gives it as "the foreground
   color of an element's text content" — `side` is the border kind's, `rect` is every kind's but the
   glyph's, and `glyph` is the
   glyph kind's alone; `display_list_append` asserts over a SWITCH so that each kind is held to the fields it
   actually uses. A field a kind does not use is therefore one no consumer of that kind may
   read — which is why this is a struct rather than a union: a union would make reading the wrong member
   undefined where a plain field leaves it merely unasserted and unread, and `-Wswitch` over the kind is what
   names every consumer the day a third set of fields arrives. */
typedef struct {
    DisplayMarkKind   kind;
    CssPx             rect[4];
    CssColor          color;
    DisplayBorderSide side[4];
    DisplayGlyph      glyph;
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
