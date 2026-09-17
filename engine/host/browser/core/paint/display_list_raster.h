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
 * `box_paint_canvas_region` — and it arrives here as a SURFACE the caller has already sized. There is
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
 * `cos` and `sin` is a function of the platform's math library; every mark this component draws is a
 * `raster_path_rect`, which is MOVE, three LINEs and a CLOSE and reaches neither call.
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
 * NAMED RESIDUAL — `DISPLAY_MARK_BORDER` CRASHES AND IS NOT DRAWN AS FOUR RECTANGLES.
 * WHAT IS NOT COVERED: a box's border, which core/paint/box_paint.c lays for CSS 2.1 §E.2's step 2 and step
 * 4 block arms whenever a box has a non-zero used border width. TWO separate things are missing and drawing
 * either without the other puts WRONG ink on a page rather than narrow ink. The first is the CORNER: four
 * side rectangles OVERLAP where two sides meet, so a box with two colours would come out with one of them
 * painted over the other in a square whose winner is whichever loop ran last — and css-backgrounds-3 §4.4
 * "Color and Style Transitions" constrains that region without settling it, saying "However it is not
 * defined what these transitions look like or what function maps from this ratio to a point on the curve".
 * The second is the STYLE: CSS 2.1 §8.5.3's `<border-style>` has TEN values and eight of them draw something
 * other than a filled band, so filling every non-`none` side solid paints a `dotted` rule as a `solid` one.
 * WHAT THE NEXT DIFF BUILDS: the four sides as MITRED QUADRILATERALS — each side a trapezoid from its own
 * outer edge to the inner edge, meeting its neighbours on the diagonal through the padding-edge corner,
 * which partitions the border area with no overlap and no gap and is a conforming transition under
 * css-backgrounds-3 §4.4 — laid through `raster_path_move_to`/`raster_path_line_to`, which is why this
 * component needs no new geometry for it; plus `solid` and `double` from CSS 2.1 §8.5.3's own descriptions
 * and one arm per remaining style. `none` and `hidden` draw nothing and are the arm that exists already.
 * HOW ITS ABSENCE WOULD SHOW: a dev build aborts at the first document that declares a border on any box,
 * naming this entry; a release build paints that document's backgrounds and leaves every rule between and
 * around its boxes unpainted, so a page comes out as flat areas of colour with no lines anywhere.
 * RETIREMENT: this record goes when `DISPLAY_MARK_BORDER` has an arm here that a fixture holds to an area. */
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
