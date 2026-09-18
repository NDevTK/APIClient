/* THE DESTINATION — an RGBA bitmap and one source-over composite. See raster_surface.h for why the components
   are non-premultiplied, why the clamp and the quantization belong here rather than to the cascade, and why
   the rounding is stated. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/graphics/raster_surface.h"
#include "core/graphics/rasterizer.h"

void raster_surface_init(RasterSurface *s, int width, int height)
{
    size_t bytes;

    /* A NEGATIVE DIMENSION IS THIS CODEBASE'S OWN CONVERSION AND NOT A PAGE'S NUMBER. HTML's `width` and
       `height` content attributes are `unsigned long`, so whatever a page writes arrives non-negative; an int
       below zero here is the conversion that produced it having gone wrong. */
    DCHECK(width >= 0 && height >= 0, "a raster surface with a negative dimension");
    s->width = width;
    s->height = height;
    s->px = NULL;
    if (width <= 0 || height <= 0) return;
    CHECK((size_t)width <= SIZE_MAX / 4 / (size_t)height,
          "a raster surface is larger than an address can name");
    bytes = (size_t)width * (size_t)height * 4;
    /* §4.12.5.1.22 "Drawing model"'s "infinite transparent black bitmap" — all four components zero. */
    s->px = (uint8_t *)calloc(bytes, 1);
    CHECK(s->px != NULL, "out of memory allocating a raster surface");
}

void raster_surface_free(RasterSurface *s)
{
    free(s->px);
    s->px = NULL;
    s->width = 0;
    s->height = 0;
}

/* TOTAL, AND NaN GOES TO ZERO RATHER THAN THROUGH. `!(v > 0)` rather than `v < 0` is what makes it total: a
   NaN compares false against everything, so the ordinary spelling would pass it to the multiply and store an
   undefined byte. A component is a cascade's number and may legitimately be out of gamut (raster_surface.h),
   so this is the answer and an assert is not. */
static double rs_clamp01(double v)
{
    if (!(v > 0.0)) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

static uint8_t rs_quantize(double v)
{
    double q = floor(rs_clamp01(v) * 255.0 + 0.5);

    DCHECK(q >= 0.0 && q <= 255.0,
           "a quantized component outside a byte — the operand is clamped to [0, 1] on the line above, so "
           "this is this file's own arithmetic and never the number a cascade computed");
    return (uint8_t)q;
}

void raster_paint_init(RasterPaint *p, RasterSurface *s, double r, double g, double b, double a)
{
    DCHECK(s != NULL, "a paint with no surface under it composites into nothing");
    p->surface = s;
    p->r = r;
    p->g = g;
    p->b = b;
    p->a = a;
    p->spans = 0;
    p->pixels = 0;
}

void raster_paint_span(void *user, int y, int x, int len, double coverage)
{
    RasterPaint *p = (RasterPaint *)user;
    const RasterSurface *s;
    double sr, sg, sb, as;
    uint8_t *row;
    int i;

    DCHECK(p != NULL && p->surface != NULL, "a paint span with no surface under it");
    s = p->surface;
    /* WHAT THE FILL HANDED THIS SINK IS THIS CODEBASE'S OWN ARITHMETIC, so it is asserted rather than
       tolerated: a span outside the region it was given the region's dimensions for is core/graphics/
       rasterizer.c having got its clipping wrong, which is exactly the defect HTML §4.12.5.1.22 "Drawing
       model"'s "When compositing onto the output bitmap, pixels that would fall outside of the output bitmap
       must be discarded" is discharged by.
       THE BOUNDS ARE A `CHECK` AND THE COVERAGE IS A `DCHECK`, AND THE LINE BETWEEN THEM IS WHAT THE NEXT
       STATEMENT DOES. The loop below WRITES THROUGH `x` and `y` in release as well as in dev, so a guard on
       them that is compiled out leaves an out-of-bounds heap write with nothing in front of it — which is
       CLAUDE.md §`CHECK`'s data integrity, a thing production must not proceed past. A coverage out of range
       is a wrong PIXEL and not a wrong ADDRESS, so it stays where an assertion about this engine's own logic
       belongs. Neither operand is anybody's page input: a span is a number core/graphics/rasterizer.c
       computed from a region this codebase allocated. */
    CHECKF(y >= 0 && y < s->height && x >= 0 && len >= 1 && x <= s->width - len,
           "a span outside the surface it is being composited into — row %d, columns %d..%d of a %dx%d "
           "surface", y, x, x + len - 1, s->width, s->height);
    DCHECKF(coverage > 0.0 && coverage <= 1.0,
            "a span of coverage %g — a run of zero coverage is ink nothing downstream could tell from its "
            "absence and is not emitted, and a coverage above one is a fold that did not saturate", coverage);
    if (s->px == NULL) return;

    p->spans++;
    p->pixels += (size_t)len;

    /* SOURCE-OVER ON STRAIGHT ALPHA, which is §4.12.5.1.17 "Compositing"'s initial operator. With
       non-premultiplied components the output colour is the alpha-weighted mean of the two colours divided by
       the output alpha, and the division is what premultiplied storage would have avoided — see
       raster_surface.h for why this pays it. */
    sr = rs_clamp01(p->r);
    sg = rs_clamp01(p->g);
    sb = rs_clamp01(p->b);
    as = rs_clamp01(p->a) * coverage;
    if (as <= 0.0) return;      /* a fully transparent paint changes no byte, whatever the coverage */

    row = s->px + ((size_t)y * (size_t)s->width + (size_t)x) * 4;
    for (i = 0; i < len; i++) {
        uint8_t *q = row + (size_t)i * 4;
        double ad = (double)q[3] / 255.0;
        double keep = ad * (1.0 - as);
        double ao = as + keep;

        if (ao <= 0.0) {
            q[0] = q[1] = q[2] = q[3] = 0;
            continue;
        }
        q[0] = rs_quantize((sr * as + ((double)q[0] / 255.0) * keep) / ao);
        q[1] = rs_quantize((sg * as + ((double)q[1] / 255.0) * keep) / ao);
        q[2] = rs_quantize((sb * as + ((double)q[2] / 255.0) * keep) / ao);
        q[3] = rs_quantize(ao);
    }
}

void raster_surface_get(const RasterSurface *s, int x, int y, uint8_t rgba[4])
{
    DCHECKF(x >= 0 && x < s->width && y >= 0 && y < s->height,
            "a read outside a %dx%d surface at (%d, %d) — a coordinate handed to this entry is an index this "
            "codebase composed", s->width, s->height, x, y);
    if (s->px == NULL) { rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0; return; }
    memcpy(rgba, s->px + ((size_t)y * (size_t)s->width + (size_t)x) * 4, 4);
}

size_t raster_surface_bytes(const RasterSurface *s)
{
    size_t n = 0;

    /* THE DIMENSIONS ARE THIS CODEBASE'S OWN, on `raster_surface_init`'s argument above: HTML's `width` and
       `height` content attributes are `unsigned long`, so a negative here is a conversion that went wrong
       rather than a number a page wrote. */
    DCHECKF(s->width >= 0 && s->height >= 0,
            "a %dx%d surface was asked for its byte extent — a negative dimension is this codebase's own "
            "conversion and never a page's number", s->width, s->height);
    if (s->width > 0 && s->height > 0) {
        /* A DCHECK AND NOT A CHECK, WHICH IS THE WHOLE DIFFERENCE BETWEEN THIS LINE AND `raster_surface_init`'s.
           There the product is a page's dimensions arriving for the first time and the refusal is always
           fatal; here the surface already EXISTS, so init has refused every un-representable one and this
           asserts that this codebase's own logic held. The zero-area arm above is what keeps the divisor
           non-zero. */
        DCHECKF((size_t)s->width <= SIZE_MAX / 4 / (size_t)s->height,
                "a %dx%d surface holds more bytes than an address can name, which raster_surface_init refuses "
                "outright — so this operand was not allocated by it", s->width, s->height);
        n = (size_t)s->width * (size_t)s->height * 4;
    }
    /* THE TWO-SIDED HALF, and it is the reason this entry is not one multiply. The struct's own declaration
       says a zero-area surface holds NULL rather than an empty allocation; asserted from BOTH ends, an extent
       of zero and an absent `px` stop being two facts that may drift, and every reader of the run below may
       take this bound without a NULL test of its own. */
    DCHECKF((n == 0) == (s->px == NULL),
            "a %dx%d surface reports %zu byte(s) of pixels and %s — the extent and the allocation are one "
            "fact, so a bound this entry hands a reader would be a run over memory that is not there",
            s->width, s->height, n, s->px == NULL ? "holds none" : "holds an allocation");
    return n;
}

uint64_t raster_surface_checksum(const RasterSurface *s)
{
    uint64_t h = UINT64_C(14695981039346656037);   /* FNV-1a 64-bit offset basis */
    /* THE BOUND IS ASKED FOR AND NOT DERIVED, and the `s->px == NULL` return that opened this function is
       DELETED rather than kept in front of it: the extent's own assert makes a zero bound and an absent
       allocation the same state, so a loop that does not run is the whole of what the test was buying. */
    size_t n = raster_surface_bytes(s), i;

    for (i = 0; i < n; i++) {
        h ^= (uint64_t)s->px[i];
        h *= UINT64_C(1099511628211);              /* FNV-1a 64-bit prime */
    }
    return h;
}
