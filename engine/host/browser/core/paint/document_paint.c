/* See core/paint/document_paint.h for why this is a component and not a line at each caller. */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_length.h"
#include "core/frame/viewport.h"
#include "core/graphics/raster_surface.h"
#include "core/paint/box_paint.h"
#include "core/paint/display_list.h"
#include "core/paint/display_list_raster.h"
#include "core/paint/document_paint.h"
#include "quickjs.h"

bool document_paint(JSContext *ctx, lxb_html_document_t *dom, RasterSurface *out, DocumentPaintCount *count)
{
    lxb_dom_element_t     *root;
    DisplayList            dl;
    DisplayListRasterCount rc;
    CssPx                  region[4];
    double                 dpr;
    char                  *display;
    bool                   boxless;
    int                    w = 0, h = 0;

    DCHECK(ctx != NULL, "a document was asked what it looks like with no realm to ask it in — CSS 2.1 §2.3.1 "
                        "\"The canvas\"'s region is a fact about the VIEWPORT this document is presented on, "
                        "and core/frame/viewport.h reads it out of a realm");
    DCHECK(dom != NULL, "a paint was asked of no document");
    DCHECK(out != NULL, "a paint was asked with nowhere to put the image");
    DCHECK(count != NULL,
           "a paint was asked with nowhere to write what it did. core/paint/display_list_raster.h states the "
           "reason and it is unchanged one level up: a surface with no ink on it means the walk reached no "
           "box, or every box painted nothing, or the walk was stopped — three things a caller must tell "
           "apart and a bitmap cannot");

    /* EVERY FIELD, BEFORE ANY ARM CAN RETURN. The count is required rather than optional, so an entry that
       returned without writing it would hand a caller an image and no way to read it — which is the same
       defect as leaving it unwritten on the arm that draws. */
    count->offers = 0;
    count->marks = 0;
    count->spans = 0;
    count->pixels = 0;
    count->complete = false;
    /* AND THE SURFACE, so that the FALSE arm leaves a zero-area surface rather than whatever the caller's
       storage held. core/graphics/raster_surface.h makes that a STATE and not an absence — NULL pixels with
       both dimensions zero — so a caller frees unconditionally and there is no arm to remember. */
    raster_surface_init(out, 0, 0);

    /* CSS 2.1 §2.3.1 "The canvas": "The canvas is infinite for each dimension of the space, but rendering
       generally occurs within a finite region of the canvas, established by the user agent according to the
       target medium". A document no navigable presents has no such region, and that is the one question this
       entry's return answers. It is asked BEFORE the walk because CSS 2.1 §E.2 "Painting order"'s own step 1
       asks the same component the same question: a document with no region walks in full and lays no canvas
       mark, so the walk would be spent to learn what this line already knows. */
    if (!viewport_canvas_region(ctx, region)) return false;

    root = lxb_dom_document_element(lxb_dom_interface_document(dom));
    DCHECK(root != NULL,
           "a document reached CSS 2.1 §E.2 \"Painting order\"'s walk with no root element. Every document "
           "this host holds is parsed from HTML markup and HTML §13.2.6 \"Tree construction\" generates an "
           "`html` element for markup that names none, so a null here is the parse having built no tree "
           "rather than a document shape this walk cannot handle");

    /* THE DEVICE SIZE OF THE IMAGE, which is core/paint/display_list_raster.h's own derivation and the one
       place the CSS-pixel-to-device-pixel rounding is made. The ratio is an OPERAND here for the reason that
       header gives about the region: a rasterizer that asked a realm for an environment fact would read
       whichever world it happened to be standing in, when two arms of one fork have two viewports — so this
       component reads it ONCE, at the top, and the same number reaches every mark below. */
    dpr = viewport_device_pixel_ratio(ctx);
    display_list_raster_region_size(region, dpr, &w, &h);
    raster_surface_init(out, w, h);

    /* DOES THE ROOT GENERATE A BOX AT ALL — css-display-3 §2.5 "Box Generation: the none and contents
       keywords", asked HERE because core/paint/stacking_order.h's stacking-context test is stated over a BOX
       and crashes for an element that generates none, naming this caller as the one that knows the
       difference. A page's own markup selects that arm, so it may never reach an assert.
       IT IS ONE KEYWORD AND NOT §2.5's TWO, and that is css-display-3 §2.8 "The Root Element's Principal Box"
       rather than an abbreviation of the question: "Additionally, a display of contents computes to block on
       the root element". core/css/css_computed_value.c performs that rule, so the DCHECK below asserts THIS
       ENGINE'S OWN computed value and never the document's — a `contents` here is the file that implements
       css-display-3 §2.8 disagreeing with this one. `none` survives the same blockification because
       css-display-3 §2.7 "Automatic Box Type Transformations" says it does: "This has no effect on display
       types that generate no box at all, such as none or contents". */
    display = css_computed_value(root, "display");
    DCHECK(display != NULL,
           "the cascade produced no computed `display` for the root element — the property is in lexbor's "
           "registry with an initial value, so css-cascade-5 §7 \"Defaulting\"'s last layer cannot come back "
           "empty and an absence is this engine's pipeline rather than the document's");
    DCHECKF(strcmp(display, "contents") != 0,
            "the root element's computed `display` is `%s`, and css-display-3 §2.8 \"The Root Element's "
            "Principal Box\" says \"Additionally, a display of contents computes to block on the root "
            "element\" — which core/css/css_computed_value.c performs. So this is not a document that "
            "declared something unusual: it is the computed value disagreeing with the section that computes "
            "it, and the fix is in that file rather than a second arm here",
            display);
    boxless = strcmp(display, "none") == 0;
    free(display);

    if (boxless) {
        /* css-display-3 §2.5's `none`: "The element and its descendants generate no boxes or text sequences".
           There is nothing for CSS 2.1 §E.2 "Painting order" to walk, and the REGION is untouched by that —
           it is a fact about the viewport — so the answer is the surface already sized above with no mark on
           it. `complete` is TRUE because nothing was left unpainted: false is stated for a painter that met an
           operand it could not compute and STOPPED, and this walk was never owed. `offers` staying zero is
           what tells a reader the two apart, because core/paint/paint_order.c offers CSS 2.1 §E.2's step 1 for
           every root it walks and a walk that happened therefore reports at least one. */
        count->complete = true;
    } else {
        display_list_init(&dl);
        /* CSS 2.1 §E.2 "Painting order" over the ROOT's stacking context. CSS 2.1 §9.9.1 "Specifying the stack
           level: the 'z-index' property"'s first sentence makes the root element form the root stacking
           context, which is box_paint's precondition satisfied by the operand rather than by a test here —
           and the one case that precondition does not admit is the arm above. */
        count->complete = box_paint_stacking_context(ctx, root, &dl, &count->offers);

        /* AND THE INK, COMPOSITED, WHETHER OR NOT THE WALK FINISHED. box_paint's contract is that a stopped
           walk leaves every mark it already appended, so the image of a document with one unpaintable box is
           the picture minus that box — CSS 2.1 §E.2's step 1 "background color of element over the entire
           canvas" included, because it is laid first. Rasterizing only a COMPLETE walk would throw that away
           and report the same empty surface for a document that painted nothing and one that painted all but
           its last box. */
        display_list_raster(&dl, dpr, out, &rc);
        count->marks = rc.marks;
        count->spans = rc.spans;
        count->pixels = rc.pixels;
        display_list_free(&dl);
    }

    /* THE EXTENT AND THE IMAGE ARE ONE FACT, ASSERTED FROM BOTH ENDS. core/graphics/raster_surface.h already
       holds a surface to `(bytes == 0) == (px == NULL)`; what is asserted here is the half above it — that a
       region this component accepted produced a surface of THAT region's device size, so a consumer taking
       the byte run is taking the whole of the image and not a prefix of it. */
    DCHECKF(raster_surface_bytes(out) == (size_t)(w > 0 ? w : 0) * (size_t)(h > 0 ? h : 0) * 4u,
            "a %dx%d device surface rendered for a %g x %g CSS-pixel region holds %zu bytes, where four per "
            "pixel of its own extent is the only run a consumer can read — the two were sized by one call and "
            "a disagreement is the surface this entry initialised not being the surface it rasterized onto",
            w, h, region[2].px, region[3].px, raster_surface_bytes(out));
    return true;
}
