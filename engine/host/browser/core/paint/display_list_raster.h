/* THE INK BECOMING PIXELS — the JOIN between core/paint/display_list.h's marks and core/graphics/
 * raster_surface.h's bitmap, and the component that answers WHERE a display list's ink is drawn.
 *
 * THE EXTENT IS AN OPERAND AND IS NOT A FIELD ON A LIST, AND THAT IS THE DECISION THIS COMPONENT EXISTS TO
 * MAKE. The design a reader arrives with is that a `DisplayList` should carry the size of the image it comes
 * to, and THREE separate things refuse it.
 *   - core/paint/display_list.h says so in its own words while arguing the opposite point: "a region is the
 *     SURFACE's and a list is not a surface". What it goes on to argue is why the CANVAS MARK carries a
 *     rectangle — for `display_list_env`'s union and for the world the ink belongs to — and it says in the
 *     same breath that the rectangle is "never the extent of the fill".
 *   - A LIST MAY HOLD SEVERAL STACKING CONTEXTS' INK. core/paint/box_paint.h's entry appends rather than
 *     replaces, expressly "so a caller composing several contexts into one surface keeps the order it
 *     composed them in" — so an extent field would be written once per context and which of those writes
 *     wins is a question nothing answers.
 *   - AND DERIVING ONE FROM THE MARKS IS WRONG RATHER THAN MERELY ABSENT. An extent that is the union of the
 *     ink makes the IMAGE SIZE a function of the INK, so two documents with different ink rasterize at two
 *     sizes and their checksums are not comparable — which destroys the one oracle core/graphics/
 *     rasterizer.h names: "this document must render identically to that one, a comparison inside one
 *     engine". The region is a fact about the DOCUMENT and not about what happens to be painted on it.
 * A LIST WITH NO CANVAS MARK CARRIES NO REGION AT ALL, which is the case that makes this concrete rather
 * than theoretical: a document whose root background is transparent lays no `DISPLAY_MARK_FILL_CANVAS`, and
 * its rendered region is the same one every other document on this medium has. So the region is CSS 2.1
 * §2.3.1 "The canvas"'s, it is answered by the ONE derivation that already answers it —
 * `viewport_canvas_region` — and it arrives here as a SURFACE the caller has already sized. There is
 * deliberately no entry here that reads a viewport.
 *
 * THE CANVAS KIND'S RECTANGLE IS READ BY NOTHING BELOW, AND THAT IS THE DESIGN. CSS 2.1 §2.3.1 says "The
 * canvas is infinite for each dimension of the space, but rendering generally occurs within a finite region
 * of the canvas, established by the user agent according to the target medium", and CSS 2.1 §E.2 "Painting
 * order"'s step 1 asks for "background color of element over the entire canvas" — so the AREA is unbounded
 * and the intersection of an unbounded area with a finite surface is THE WHOLE SURFACE. A canvas mark
 * therefore fills every pixel, whatever its rectangle says, which is core/paint/display_list.h's own rule
 * that "A rasterizer whose own region is larger must EXTEND a canvas fill and must NOT extend a rectangle
 * fill". A reader who "fixes" this by filling the mark's rectangle has made a surface larger than the
 * region come out with an unpainted border, and one smaller come out identical — so the defect is invisible
 * at the one size this host currently establishes.
 *
 * A RECTANGLE IS FILLED THROUGH THE PATH ROAD AND NOT BY A SCANLINE LOOP OF THIS COMPONENT'S OWN, which is
 * the one place a reader is invited to save an allocation and must not. core/graphics/rasterizer.h states
 * the reason about its own two fill rules and it is the same reason here: "Two implementations would be two
 * answers to one question, free to disagree about the pixel where it matters most". A rectangle whose edges
 * do not land on pixel boundaries has FRACTIONAL COVERAGE at every edge pixel, and a coverage written here
 * would be a second answer to the question core/graphics/rasterizer.c already answers analytically. What
 * makes the reuse free is that `raster_path_rect` takes four doubles and no realm — core/graphics/
 * raster_path.h names exactly this consumer, "The display-list consumer has NO JS PATH AT ALL" — so the
 * rectangle is stated directly and the fill is the engine's one fill.
 * THE RULE IS `RASTER_FILL_NONZERO` AND THE CHOICE IS NOT OBSERVABLE HERE: a single rectangle has a crossing
 * count of 0 or ±1 at every point, which HTML §4.12.5.1 "The 2D rendering context"'s two rules answer
 * identically. It is named rather than picked because it is that section's `CanvasFillRule` default.
 * AND THE ARC RESIDUAL core/graphics/raster_path.c CARRIES DOES NOT REACH THIS ROAD. That record narrows the
 * byte-for-byte agreement of two HOSTS to paths with no `CANVAS_PATH_OP_ARC` in them, because a vertex off
 * `cos` and `sin` is a function of the platform's math library. Every shape this component draws is MOVE,
 * three LINEs and a CLOSE — `raster_path_rect`'s own spelling for the two fill kinds, and the same four
 * vertices stated directly for each of a border's mitred wedges — and `raster_path_flatten` takes those three
 * opcodes to `rp_emit` alone, where the `cos`, `sin`, `hypot` and `sqrt` calls sit in `rp_flatten_arc`,
 * `rp_quad_segments` and `rp_bezier_segments`. That is why a BORDER SIDE IS A QUADRILATERAL RATHER THAN AN
 * OUTLINE, and why the first `<border-style>` that needs a curve — css-backgrounds-3 §3.2 "Line Patterns: the
 * border-style properties"' `dotted`, "A series of round dots" — is the first that does not land here.
 *
 * THE SCALE IS AN OPERAND FOR core/graphics/raster_path.h's OWN REASON. That header says "EVERY COORDINATE
 * HERE IS A DEVICE PIXEL AND NOTHING HERE APPLIES A MATRIX ... There is exactly one transform in the road and
 * it is at the rendering context" — and this road has no rendering context, so this is where the one
 * transform is. A display list's rectangles are CSS pixels in CSSOM VIEW §6 "Extensions to the Element
 * Interface"' client coordinates and a surface is device pixels, and the ratio between them is
 * core/frame/viewport.h's `devicePixelRatio`. It is passed rather than read for the reason
 * core/paint/display_list.h gives about the region: a rasterizer that asked a realm for an environment fact
 * would read whichever world it happened to be standing in, when two arms of one fork have two viewports.
 *
 * THE COUNT IS REQUIRED AND NOT OPTIONAL, which is core/paint/box_paint.h's argument for its own `offers`
 * one component over. A surface with no ink on it means three different things a caller must tell apart —
 * the list was empty, every mark was transparent, or every mark fell outside the surface — and those take
 * opposite work. A caller handed only the bitmap cannot separate them, and a number that cannot separate an
 * ABSENCE from a ZERO is the shape CLAUDE.md names by hand. So the entry crashes rather than accepting
 * nowhere to put it.
 *
 * A BORDER'S FOUR SIDES ARE MITRED WEDGES AND THE CORNER IS WHY. Four side RECTANGLES overlap in a square at
 * every corner, so a box with two border colours comes out with one painted over the other in a square whose
 * winner is whichever loop ran last — WRONG ink rather than narrow ink, and the reason the rect-only shortcut
 * could not draw a border at all. A wedge runs from a side's own outer edge to the padding edge and meets its
 * two neighbours on the DIAGONAL through the padding-edge corner, which partitions the border area with no
 * overlap and no gap. THE STANDARD LEAVES THE SHAPE AND SAYS SO: css-backgrounds-3 §3.2 "Line Patterns: the
 * border-style properties" ends "Note: This specification does not define how borders of different styles
 * should be joined in the corner", which is the note that reaches a corner with NO RADIUS and is therefore
 * this component's; css-backgrounds-3 §4.4 "Color and Style Transitions" sits inside §4 "Rounded Corners" and
 * constrains the region for a corner that has radii, then leaves the shape in the same terms — "However it is
 * not defined what these transitions look like or what function maps from this ratio to a point on the
 * curve". At a zero radius §4.4's "smallest rectangle that contains both border radii as well as the center
 * of the inner curve" IS the square between the border-box corner and the padding-edge corner, and the
 * diagonal lies inside it, so the mitre satisfies §4.4's MUST as well. THE PARTITION IS ASSERTED rather than
 * argued: the four wedges' shoelace areas are held to `w*h - (w-l-r)*(h-t-b)`, a number written out of the
 * EXTENTS and sharing no term with the vertices, and four rectangles fail it by exactly (l+r)(t+b).
 *
 * NAMED RESIDUAL — EIGHT OF CSS 2.1 §8.5.3's TEN `<border-style>` VALUES ARE NOT DRAWN.
 * WHAT IS NOT COVERED: every value that is none of `solid`, `double` and a value whose used width is zero.
 * CSS 2.1 §8.5.3 "Border style: 'border-top-style', 'border-right-style', 'border-bottom-style', 'border-left-style',
 * and 'border-style'" makes exactly ONE of its ten a single filled band — `solid`, "The border is a single
 * line segment" — `double` is that band cut into three at a fraction this file names, and `none` and
 * `hidden` compute to a used width of zero. The property that separates the six, rather than a list of them:
 * each needs an operand the standards explicitly leave to the UA — `dotted` and `dashed` a RHYTHM ("There is
 * no control over the spacing of the dots and dashes, nor over the length of the dashes",
 * css-backgrounds-3 §3.2 "Line Patterns: the border-style properties"), and `groove`, `ridge`, `inset` and
 * `outset` a derived COLOUR ("UAs may choose their own algorithm to calculate the actual colors used",
 * CSS 2.1 §8.5.3). `double` needed one too — a line THICKNESS, "The thickness of the lines is not specified,
 * but the sum of the lines and the space must equal border-width" (§3.2) — and what let it land ahead of the
 * six is that its operand is a SINGLE NUMBER this file can state, where a rhythm is two and a colour is an
 * entry another component does not have. The mitre above is a pick too and is a different kind of pick:
 * without SOME partition of the corner no border can be drawn at all, so it is forced by the problem, where
 * each of these is a second choice for a side that would otherwise already be drawable.
 * WHAT THE NEXT DIFF BUILDS: `dashed`, because it is the only one of the six whose whole cost is in this
 * file. Its shape is already this road's — §3.2 gives it as "A series of square-ended dashes", which is
 * quadrilaterals and needs no curve — so what it wants is a dash length and a gap picked from the side's
 * used width and named as this user agent's beside §3.2's note, distributed along the wedge's OUTER edge so
 * that the note's "Implementations are encouraged to choose a spacing that makes the corners symmetrical"
 * is satisfiable, with each dash clipped to its own wedge by `dlr_border_subwedge`'s neighbour in the other
 * axis. THE OTHER FIVE ARE NOT NEXT AND EACH FOR A REASON THAT IS NOT ITS DIFFICULTY. `dotted` is "A series
 * of round dots" (§3.2), so it is `raster_path_ellipse` and therefore `cos` and `sin`, and
 * core/graphics/raster_path.c's own residual narrows two HOSTS' byte-for-byte agreement away from exactly
 * those — a dotted border would widen that record's population from documents containing a canvas arc to
 * every document declaring one, so its answer is owed first. `groove`, `ridge`, `inset` and `outset` want a
 * derived colour, and core/css/css_color.h carries no entry that LIGHTENS or DARKENS at all; their SHAPE is
 * `dlr_border_subwedge` and not a second geometry.
 * HOW ITS ABSENCE WOULD SHOW: a dev build aborts at the first box declaring one of the six, naming that
 * value; a release build draws that SIDE's band nothing and its siblings normally, so a box comes out with
 * some of its four rules present and the rest missing while every background around it is painted — which is
 * the observation, and which side of which box exhibits it is a fact about a document rather than about this
 * component.
 * RETIREMENT: this record loses a clause as each value lands and goes when all ten are drawn.
 *
 * NAMED RESIDUAL — NO FIXTURE IN THIS TREE RASTERIZES A BORDER MARK.
 * WHAT IS NOT COVERED: this entry's border arm has no caller that a run of this host reaches.
 * `display_list_raster` is called from `display_list_raster_selftest` and from `box_paint_selftest` in
 * engine/host/test_forced.c and from nowhere else in the program, and neither rasterizes a list holding a
 * `DISPLAY_MARK_BORDER`: the first builds only fill marks, and the second's own record states that no
 * document this host parses declares a border, so `bp_border` appends nothing. The arm is therefore held by
 * its own asserts — the partition, the two derivations of one box, and the two-counter identity — and by no
 * observation of a pixel.
 * WHAT THE NEXT DIFF BUILDS: rows in `display_list_raster_selftest` beside the two fill kinds', over a mark
 * this file states rather than one a document produced, and asserting the same kind of DERIVABLE quantity
 * those rows assert. THE QUANTITY IS `pixels` AND IT IS NOT THE AREA, which is the trap in writing them: the
 * arm's own assert holds the four wedges to an AREA, and `pixels` counts what each fill HANDED the surface,
 * which core/graphics/rasterizer.h makes every pixel of NONZERO coverage — so a mitre's diagonal, which
 * crosses pixels rather than running along their edges, is handed more pixels than it covers area, and a
 * fixture that asserted the area would fail on a correct arm. Worked, for a 16x8 border box at the surface's
 * own origin with all four used widths 2 and all four styles `solid`: the AREA is 16*8 - 12*4 = 80 and the
 * PIXELS are 88. The top wedge is (0,0) (16,0) (14,2) (2,2), whose row y=0 is handed columns 0 through 15 and
 * whose row y=1 is handed 1 through 14 — 30 — and the bottom is its mirror; the left wedge is (0,8) (0,0)
 * (2,2) (2,6), handed 1, 2, 2, 2, 2, 2, 2, 1 down its eight rows — 14 — and the right is its mirror.
 * 30+30+14+14. FOUR RECTANGLES ARE 96 — two full rows of 16 twice and two full columns of 8 twice — so that
 * is the number that separates this arm from the shortcut it replaced, and `spans` is NOT: both are one run
 * per row of each side, 2+2+8+8 = 20, at either geometry. A second mark with the four sides in four colours
 * and a read of the pixel at each side's own midpoint is what holds the index to top, right, bottom, left.
 * HOW ITS ABSENCE WOULD SHOW: `grep -c '@PAINT'` over a run's output answers the same number before and after
 * any change to the border arm, and no row anywhere reports a span or a pixel that a border laid.
 * RETIREMENT: this record goes when a fixture this host runs rasterizes a border mark and holds it to a
 * derived area. */
#ifndef ENGINE_HOST_BROWSER_CORE_PAINT_DISPLAY_LIST_RASTER_H
#define ENGINE_HOST_BROWSER_CORE_PAINT_DISPLAY_LIST_RASTER_H

#include <stddef.h>

#include "core/css/css_length.h"
#include "core/graphics/raster_surface.h"
#include "core/paint/display_list.h"

/* THE DEVICE DIMENSIONS OF A SURFACE THAT RENDERS `region`, which is the one place the CSS-pixel-to-device-
   pixel rounding is decided. Only `region[2]` and `region[3]` are read: the extent of a rectangle is the same
   number wherever its origin is, and CSS 2.1 §10.1 "Definition of "containing block"" puts this particular
   one at the canvas origin anyway.
   IT ROUNDS UP. A surface that COVERS the region loses no ink at its right and bottom edges, where one that
   truncated would drop a partly-covered column and a partly-covered row — and the ink that lands there is
   exactly CSS 2.1 §E.2's step 1 fill, which reaches every pixel of the region by construction. */
void display_list_raster_region_size(const CssPx region[4], double device_px_per_css_px,
                                     int *width, int *height);

/* WHAT A RASTERIZATION DID, in three numbers that separate the states a bitmap cannot. `marks` is the marks
   composited, `spans` the runs core/graphics/rasterizer.h's fill emitted over all of them, and `pixels` the
   pixels those runs HANDED to the surface — which is core/graphics/raster_surface.h's own meaning for that
   word and counts a pixel once per mark that covers it, so a list whose marks overlap reports more pixels
   than the surface has. */
typedef struct {
    size_t marks;
    size_t spans;
    size_t pixels;
} DisplayListRasterCount;

/* COMPOSITE EVERY MARK OF `dl` ONTO `surface`, in the order CSS 2.1 §E.2 "Painting order" offered them, at
   `device_px_per_css_px` device pixels per CSS pixel, writing what it did to `count`.
   `surface` IS APPENDED TO RATHER THAN CLEARED — nothing here calls `raster_surface_init` — so a caller
   composing several stacking contexts' lists onto one bitmap gets CSS 2.1 §E.2's sequence across all of
   them, which is the same property core/paint/box_paint.h's entry has over the list itself.
   `count` IS REQUIRED for the reason the header gives, and is written even when nothing is drawn. */
void display_list_raster(const DisplayList *dl, double device_px_per_css_px, RasterSurface *surface,
                         DisplayListRasterCount *count);

#endif /* ENGINE_HOST_BROWSER_CORE_PAINT_DISPLAY_LIST_RASTER_H */
