/* TrueType OUTLINES, DECODED — see core/fonts/glyph_outline.h for what this produces and for the line it
 * draws between a claim the bytes make and an invariant this file established. */
#include "core/fonts/glyph_outline.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

/* THE FLAG BITS OF A SIMPLE GLYPH, NAMED AS OpenType 'glyf' — Glyph Data's Simple Glyph flags table names
   them. They are spelled out rather than used as bare numbers because every one of them is a place a
   recollection is confidently wrong, and the names are what let a reader check this file against the
   standard without decoding a hexadecimal constant first. Bit 6 (OVERLAP_SIMPLE) says contours "could
   overlap" and bit 7 is reserved; neither changes how a single byte of this table is read, and the fill this
   feeds already resolves overlap by its own rule, so neither is consulted here. */
#define GO_ON_CURVE_POINT   0x01
#define GO_X_SHORT_VECTOR   0x02
#define GO_Y_SHORT_VECTOR   0x04
#define GO_REPEAT_FLAG      0x08
#define GO_X_IS_SAME_OR_POSITIVE_X_SHORT_VECTOR  0x10
#define GO_Y_IS_SAME_OR_POSITIVE_Y_SHORT_VECTOR  0x20

/* THE GLYPH HEADER IS TEN BYTES — an int16 numberOfContours and the four int16 bounding-box values the same
   table lists after it. The bounding box is READ BY NOBODY HERE: the standard says it is "obtained directly
   from the point coordinate data", so it is a restatement of what this file decodes and trusting it would be
   a second copy of one fact free to disagree with the first. */
#define GO_GLYPH_HEADER 10

/* THE FLAG BITS OF A COMPONENT, NAMED AS OpenType 'glyf' — Glyph Data's Component Glyph flags table names
   them, and only the ones this decoder CONSULTS are spelled — which is the same line the simple arm above
   draws, so that a name here is evidence a byte is read and not decoration.
   THE FOUR THAT ARE READ BY NOBODY, AND WHY EACH IS A DECISION RATHER THAN AN OMISSION:
     ROUND_XY_TO_GRID (bit 2) grid-fits the offset vector — "the x and y values rounded to the nearest pixel
       grid line". This engine GRID-FITS NOTHING: the simple arm skips a glyph's instructions for exactly
       that reason, and honouring the one rounding while ignoring every hint that decides where a grid line
       should fall would be a fragment of hinting rather than a smaller amount of it. The shipped face sets
       this bit on essentially every component it has, so consulting it is not a rare path being skipped.
     USE_MY_METRICS (bit 9) forces "the aw and lsb (and rsb) for the composite to be equal to those from this
       component glyph" — which is an ADVANCE and not an outline. This component answers no advances at all;
       'hmtx' does, through core/fonts/open_type_metrics.h, and a bit consulted here could only ever produce
       a second answer to a question another file owns.
     OVERLAP_COMPOUND (bit 10) is the composite's OVERLAP_SIMPLE and is not consulted for its reason: the
       fill this feeds resolves overlap by its own winding rule.
     WE_HAVE_INSTRUCTIONS (bit 8) says a length and a byte run follow the LAST component. Nothing here reads
       past the last component, so there is nothing to skip — and refusing a glyph over bytes this decoder
       never touches would reject faces for a table it does not use.
   BITS 4, 13, 14 AND 15 ARE RESERVED and, like the simple arm's reserved bit, change how no byte is read. */
#define GO_ARG_1_AND_2_ARE_WORDS     0x0001
#define GO_ARGS_ARE_XY_VALUES        0x0002
#define GO_WE_HAVE_A_SCALE           0x0008
#define GO_MORE_COMPONENTS           0x0020
#define GO_WE_HAVE_AN_X_AND_Y_SCALE  0x0040
#define GO_WE_HAVE_A_TWO_BY_TWO      0x0080
#define GO_SCALED_COMPONENT_OFFSET   0x0800
#define GO_UNSCALED_COMPONENT_OFFSET 0x1000

/* THE SMALLEST COMPONENT RECORD — a uint16 flags and a uint16 glyphIndex, which every one of them begins
   with before its arguments. */
#define GO_COMPONENT_HEADER 4

/* ---- readers over a span whose extent the caller has already proved ---------------------------------- */

static uint16_t go_u16(const unsigned char *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static int16_t go_i16(const unsigned char *p)
{
    return (int16_t)go_u16(p);
}

static uint32_t go_u32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* ONE ENTRY OF THE OFFSET ARRAY, IN BYTES. OpenType 'loca' — Index to Location gives the two forms and the
   difference between them is not only the width: the short form stores "The local offset divided by 2", so
   reading it without the multiply yields an array that ascends, fits, and addresses the wrong half of every
   glyph in the face. `i` may be `num_glyphs` — the array has one more entry than the face has glyphs,
   which is what gives the last glyph a length. */
static uint32_t go_loca(const GlyphOutlines *g, uint32_t i)
{
    DCHECK(i <= (uint32_t)g->num_glyphs,
           "the offset array was indexed past its own last entry. It has numGlyphs + 1 entries and every "
           "caller here has already bounded the glyph ID, so this is this file's own arithmetic and not the "
           "face's");
    return g->long_loca ? go_u32(g->loca + (size_t)i * 4)
                        : (uint32_t)go_u16(g->loca + (size_t)i * 2) * 2u;
}

#define GO_READY(g)                                                                                          \
    DCHECK((g) != NULL && (g)->reject == NULL && (g)->glyf != NULL && (g)->loca != NULL,                      \
           "an outline table pair that glyph_outlines_read REJECTED (or never read at all) was decoded. The " \
           "struct is zeroed on failure precisely so that this cannot return a plausible shape: a NULL span " \
           "of length zero is not a face's outlines. Whoever ignored the false is one frame up")

static bool go_reject(GlyphOutlines *g, const char *why)
{
    g->reject = why;
    return false;
}

bool glyph_outlines_read(GlyphOutlines *g, const unsigned char *glyf, size_t glyf_len,
                         const unsigned char *loca, size_t loca_len,
                         uint16_t num_glyphs, bool long_loca)
{
    size_t entry, need;
    uint32_t i, prev = 0, cur;

    DCHECK(g != NULL, "glyph_outlines_read was handed no struct to fill");
    memset(g, 0, sizeof *g);
    g->glyf = glyf;
    g->glyf_len = glyf_len;
    g->loca = loca;
    g->loca_len = loca_len;
    g->num_glyphs = num_glyphs;
    g->long_loca = long_loca;

    if (glyf == NULL || loca == NULL)
        return go_reject(g, "an outline table pair with a missing half — a face that has one of 'glyf' and "
                            "'loca' and not the other cannot be decoded, and the standard says in as many "
                            "words that the outline table must always be used in conjunction with the other");
    if (num_glyphs == 0)
        return go_reject(g, "a face claiming zero glyphs. 'maxp' — Maximum Profile's numGlyphs is at least "
                            "one for any face at all, because glyph zero is the missing-character glyph "
                            "every face has");

    /* THE ARRAY MUST BE LONG ENOUGH FOR ITS OWN DECLARED LENGTH. `>=` and not `==`: a table's recorded
       length is its true length while its DATA is padded to a four-byte boundary, so a face whose offset
       array ends on an odd two-byte entry legitimately carries up to two bytes this reader never looks at. */
    entry = long_loca ? 4u : 2u;
    need = ((size_t)num_glyphs + 1u) * entry;
    if (loca_len < need)
        return go_reject(g, "the offset array is shorter than the one entry per glyph plus one the standard "
                            "requires of it. Its length is a function of numGlyphs and indexToLocFormat, so "
                            "a short one is a face whose three tables disagree about how many glyphs it has");

    /* THE WHOLE ARRAY IS WALKED ONCE, WHICH IS WHAT MAKES EVERY LATER LOOKUP TOTAL. Two rules, and the
       standard states both: entries ascend, and a glyph's length is the difference between consecutive
       entries. A descending pair would make that difference negative — as an unsigned subtraction, an
       enormous length pointing at bytes that are not this glyph's — so this is the check that lets the
       decoder treat its slice as an extent rather than re-deriving one per glyph. */
    for (i = 0; i <= (uint32_t)num_glyphs; i++) {
        cur = long_loca ? go_u32(loca + (size_t)i * 4) : (uint32_t)go_u16(loca + (size_t)i * 2) * 2u;
        if (i > 0 && cur < prev)
            return go_reject(g, "the offset array does not ascend. The standard requires each entry to be no "
                                "smaller than the one before it, because a glyph's length IS the difference "
                                "between consecutive entries and a descending pair names a negative one");
        prev = cur;
    }
    if ((size_t)prev > glyf_len)
        return go_reject(g, "the offset array's last entry lies past the end of the outline table. That entry "
                            "is the one that gives the LAST glyph its length, so a face where it overruns is "
                            "one whose final glyph description was never in the file");
    return true;
}

size_t glyph_outline_length(const GlyphOutlines *g, uint16_t glyph_id)
{
    GO_READY(g);
    /* A CHECK AND NOT A DCHECK, WHICH IS THE ONE PLACE THIS FILE IS STRICTER THAN ITS NEIGHBOUR. The index
       below reads the offset array in EVERY build, so a dev-only guard would leave a release build walking
       off the table on a glyph ID nobody produced from this face's own character map. CLAUDE.md's rule is
       that a guard survives in the build where the dereference happens; this one happens in both, so this is
       the memory-safety arm rather than the invariant arm. */
    CHECK(glyph_id < g->num_glyphs,
          "a glyph outline was asked for a glyph ID at or above 'maxp' — Maximum Profile's numGlyphs. Every "
          "glyph ID in this engine comes out of a character-map lookup against THIS face, which has already "
          "proved the same bound, so this is a caller that invented one or mixed two faces");
    return (size_t)(go_loca(g, (uint32_t)glyph_id + 1u) - go_loca(g, glyph_id));
}

/* ---- one glyph -------------------------------------------------------------------------------------- */

/* ONE DECODED POINT. The coordinates are accumulated as 64-bit integers and not as the 16-bit deltas they
   arrive as: the standard says the first coordinate is relative to the origin and every other is relative to
   its predecessor, so a face may legitimately walk a long way from zero, and a glyph with the largest point
   count the format can express and the largest delta it can express reaches exactly the magnitude a 32-bit
   accumulator overflows at. An overflow there is undefined behaviour driven by a page's own font bytes,
   which is the one arithmetic in this file a hostile file could reach. */
typedef struct {
    int64_t x, y;
    uint8_t flag;
} GoPoint;

/* THE AFFINE A COMPONENT IS INCORPORATED UNDER, IN THE FACE'S OWN UNITS AND NEVER IN DEVICE PIXELS.
   OpenType 'glyf' — Glyph Data's "Composite glyph description" writes the matrix out in full and it is
   copied here letter for letter, because the OFF-DIAGONAL TERMS CROSS:
       "x' = xscale * x + scale10 * y"
       "y' = scale01 * x + yscale * y"
   The value stored THIRD (`scale10`) multiplies y on the way to x', and the one stored SECOND (`scale01`)
   multiplies x on the way to y'. A decoder that fills a matrix in storage order and then multiplies the
   obvious way TRANSPOSES every rotation in the face — which leaves every glyph the right size, in the right
   place, and turning the wrong way, and is the single place in this description a recollection is most
   confidently wrong.
   THE TOP-LEVEL GLYPH IS DECODED UNDER THE IDENTITY, which is why this is a value and not a special case:
   there is ONE contour emitter and a simple glyph is the composite path's leaf with nothing composed onto
   it. Under the identity the two multiply-adds below are `x` and `y` EXACTLY in IEEE 754 — 1.0*v is v and
   0.0*v is a signed zero that adds away — so a face with no composite in it produces the byte-identical
   stream it produced before there was a transform here at all. */
typedef struct {
    double a, b, c, d;   /* (a, b, c, d) = (xscale, scale01, scale10, yscale) */
    double dx, dy;       /* the translation, already expressed in the PARENT's design units */
} GoXform;

static const GoXform GO_IDENTITY = { 1.0, 0.0, 0.0, 1.0, 0.0, 0.0 };

/* COMPOSE THE CHILD'S OWN PLACEMENT WITH ITS PARENT'S — `p` after `c`, so a point of the child's design
   space lands where the whole nesting chain puts it. A chain therefore carries ONE matrix per frame rather
   than a list to replay, which is what makes the depth-first walk below hold no geometry.
   THE ORDER IS NOT A DETAIL AND IS INVISIBLE ON THE SHIPPED FACE. Composition commutes when every component
   only translates, and MEASURED over all 6253 glyphs of this engine's own face, not one component sets any
   of the three scale flags — so a decoder that composed the other way round would agree with this one on
   every character this user agent can currently draw and disagree on the first face that scales a
   component, which is exactly the shape a fixture has to state bytes of its own to catch. */
static GoXform go_compose(const GoXform *p, const GoXform *c)
{
    GoXform r;

    r.a = p->a * c->a + p->c * c->b;
    r.b = p->b * c->a + p->d * c->b;
    r.c = p->a * c->c + p->c * c->d;
    r.d = p->b * c->c + p->d * c->d;
    r.dx = p->a * c->dx + p->c * c->dy + p->dx;
    r.dy = p->b * c->dx + p->d * c->dy + p->dy;
    return r;
}

/* ONE F2DOT14, WHOSE BINARY POINT IS THE ONE THING TO READ RATHER THAN ASSUME. OpenType's data-type chapter
   defines it as a "16-bit signed fixed number with the low 14 bits of fraction (2.14)" and then gives the
   table that settles the sign: 0xFFFF is -0.000061, which is the ordinary two's-complement value over 16384
   and NOT the "integer -1 plus a fraction of 16383/16384" a reader can talk themselves into. So the whole
   conversion is one signed divide, and the representable range is [-2, 2). */
static double go_f2dot14(const unsigned char *p)
{
    return (double)go_i16(p) / 16384.0;
}

static uint16_t go_end_pt(const unsigned char *d, int c)
{
    return go_u16(d + GO_GLYPH_HEADER + 2u * (size_t)c);
}

/* A POINT IN DEVICE PIXELS. Four facts in three lines, and the subtraction is the one a reader should check:
   font design units increase UPWARD from the baseline and this engine's device rows increase DOWNWARD, so a
   decoder that added here would produce every glyph mirrored about its own baseline. The component transform
   is resolved FIRST and in the face's own units, because a composite places its children in the design grid
   and the device placement is applied once to the result. */
static void go_device(const GoPoint *p, const GoXform *xf, double ox, double oy, double sc,
                      double *x, double *y)
{
    double fx = xf->a * (double)p->x + xf->c * (double)p->y + xf->dx;
    double fy = xf->b * (double)p->x + xf->d * (double)p->y + xf->dy;

    *x = ox + fx * sc;
    *y = oy - fy * sc;
}

#define GO_MALFORMED(why) do { if (reject) *reject = (why); free(pt); return GLYPH_OUTLINE_MALFORMED; } while (0)

/* ONE SIMPLE GLYPH'S CONTOURS, APPENDED UNDER `xf`. The caller has already selected the slice and decided
   that this description is a simple one, so this answers OK or MALFORMED and never the third outcome. */
static GlyphOutlineResult go_simple(const GlyphOutlines *g, size_t off, size_t n, const GoXform *xf,
                                    double origin_x, double origin_y, double scale,
                                    RasterPath *out, const char **reject)
{
    const unsigned char *d = g->glyf + off;
    GoPoint *pt = NULL;
    size_t cur;
    uint32_t npts, i, first, last, c_pts, i0, count, k;
    int nc, c;
    int64_t acc;
    size_t subpaths_before;

    DCHECK(n >= GO_GLYPH_HEADER,
           "a simple glyph was emitted from a description shorter than the ten-byte header, which the caller "
           "that selected this slice has already refused");
    nc = go_i16(d);
    DCHECK(nc >= 0, "go_simple was handed a description whose numberOfContours is negative, which is the "
                    "sign the caller dispatches on — so this is the dispatch and not the face");
    if (nc == 0)
        return GLYPH_OUTLINE_OK;   /* "If a glyph has zero contours, no additional glyph data ... is required" */

    cur = GO_GLYPH_HEADER;
    if ((size_t)nc > (n - cur) / 2u)
        GO_MALFORMED("a contour count larger than the glyph's own bytes can hold end points for");

    /* END POINTS ASCEND STRICTLY, WHICH IS WHAT MAKES EVERY CONTOUR A NON-EMPTY RANGE. The standard calls the
       array one "in increasing numeric order", and a pair that does not increase names a contour with no
       points at all (equal) or one whose range runs backwards (smaller). Refusing both is the spec-faithful
       reading and it costs at most ONE GLYPH, because this refusal is per glyph and not per face — which is
       the only reason it is safe to be strict here rather than tolerant. */
    for (c = 1; c < nc; c++) {
        if (go_end_pt(d, c) <= go_end_pt(d, c - 1))
            GO_MALFORMED("the end points of this glyph's contours do not increase, so at least one contour "
                         "of it names an empty or a backwards range of points");
    }
    npts = (uint32_t)go_end_pt(d, nc - 1) + 1u;   /* "the number of points is determined by the last entry" */
    cur += 2u * (size_t)nc;

    if (n - cur < 2)
        GO_MALFORMED("a glyph description that ends before its instruction length");
    {
        uint16_t instr = go_u16(d + cur);
        cur += 2;
        if ((size_t)instr > n - cur)
            GO_MALFORMED("an instruction length longer than the glyph description that carries it");
        /* THE INSTRUCTIONS ARE SKIPPED AND THAT IS A STATEMENT, not an omission: hinting refines a
           rasterized result at a particular size and resolution, and this engine grid-fits nothing. Reading
           past them is the whole of what a non-hinting decoder does with them. */
        cur += instr;
    }

    /* ONE ALLOCATION, PROPORTIONAL TO THIS GLYPH AND NOT TO THE FACE. The contour walk below needs random
       access — the first point of a contour is decided by looking at its LAST one — so the points cannot be
       streamed. */
    pt = malloc((size_t)npts * sizeof *pt);
    CHECK(pt != NULL, "out of memory decoding one glyph's points");

    /* THE FLAG ARRAY IS RUN-LENGTH ENCODED AND ITS LENGTH IS NOT STATED ANYWHERE. The standard says the
       stored size "must be determined by parsing the flags array entries", so the terminating condition is
       the POINT TOTAL and the only way to be sure of the coordinate arrays' start is to walk this one. */
    i = 0;
    while (i < npts) {
        uint8_t f;

        if (cur >= n)
            GO_MALFORMED("the flag array ends before every point of this glyph has one");
        f = d[cur++];
        pt[i++].flag = f;
        if (f & GO_REPEAT_FLAG) {
            uint8_t rep;

            if (cur >= n)
                GO_MALFORMED("a repeat flag with no count byte after it");
            /* "the next byte (read as unsigned) specifies the number of additional times this flag byte is
               to be repeated" — ADDITIONAL, so the entry already appended above is not one of them. */
            rep = d[cur++];
            if ((uint32_t)rep > npts - i)
                GO_MALFORMED("a flag repeat count that runs past this glyph's own point total");
            while (rep-- > 0)
                pt[i++].flag = f;
        }
    }
    DCHECK(i == npts, "the flag walk ended at a point count other than the one the end points named");

    /* THE TWO COORDINATE ARRAYS ARE SEPARATE AND CONSECUTIVE, and the y array's start is wherever the x
       array happened to end — which is a function of the flags and of nothing else, so it cannot be
       computed without this walk either. Each delta is one of three shapes and the pair of flag bits that
       selects them is the place in this format a reader is most likely to be confidently wrong: the short
       bit says ONE BYTE and the same-or-positive bit is then its SIGN, while with the short bit clear that
       same bit means the delta is ZERO and its absence means a signed 16-bit one. */
    acc = 0;
    for (i = 0; i < npts; i++) {
        uint8_t f = pt[i].flag;

        if (f & GO_X_SHORT_VECTOR) {
            if (cur >= n)
                GO_MALFORMED("the x-coordinate array ends before every point of this glyph has one");
            acc += (f & GO_X_IS_SAME_OR_POSITIVE_X_SHORT_VECTOR) ? (int64_t)d[cur] : -(int64_t)d[cur];
            cur += 1;
        } else if (!(f & GO_X_IS_SAME_OR_POSITIVE_X_SHORT_VECTOR)) {
            if (n - cur < 2)
                GO_MALFORMED("the x-coordinate array ends inside a 16-bit delta");
            acc += (int64_t)go_i16(d + cur);
            cur += 2;
        }
        pt[i].x = acc;
    }
    acc = 0;
    for (i = 0; i < npts; i++) {
        uint8_t f = pt[i].flag;

        if (f & GO_Y_SHORT_VECTOR) {
            if (cur >= n)
                GO_MALFORMED("the y-coordinate array ends before every point of this glyph has one");
            acc += (f & GO_Y_IS_SAME_OR_POSITIVE_Y_SHORT_VECTOR) ? (int64_t)d[cur] : -(int64_t)d[cur];
            cur += 1;
        } else if (!(f & GO_Y_IS_SAME_OR_POSITIVE_Y_SHORT_VECTOR)) {
            if (n - cur < 2)
                GO_MALFORMED("the y-coordinate array ends inside a 16-bit delta");
            acc += (int64_t)go_i16(d + cur);
            cur += 2;
        }
        pt[i].y = acc;
    }
    DCHECK(cur <= n, "the point decode read past this glyph's own slice of the outline table");

    /* ---- the contours, as segments ------------------------------------------------------------------ */
    subpaths_before = out->nsub;
    first = 0;
    for (c = 0; c < nc; c++) {
        double sx, sy, cx = 0, cy = 0;
        bool have_ctrl;

        last = (uint32_t)go_end_pt(d, c);
        DCHECK(last >= first && last < npts,
               "a contour's point range is empty or runs past the glyph's point total, though the strictly "
               "ascending end points above make both impossible");
        c_pts = last - first + 1u;

        /* WHERE A CONTOUR STARTS IS NOT WHERE ITS FIRST POINT IS. A path begins at an ON-CURVE point and a
           contour is a CYCLE whose first stored point may be a control point, so the start is the first
           point if that is on the curve, the LAST point if that one is, and otherwise a point the face never
           stored at all — the midpoint of the two, which is the implied on-curve point Apple's TrueType
           Reference Manual chapter "Digitizing Letterform Designs" defines when it says an on-curve point
           "is not required in TrueType if the tangency point is midway between the flanking off-curve
           points". Getting this wrong does not crash: it rotates the contour's segments by one and closes it
           with a straight line where a curve belongs, which reads as a font that renders slightly wrong. */
        if (pt[first].flag & GO_ON_CURVE_POINT) {
            go_device(&pt[first], xf, origin_x, origin_y, scale, &sx, &sy);
            i0 = first + 1u;
            count = c_pts - 1u;
        } else if (pt[last].flag & GO_ON_CURVE_POINT) {
            go_device(&pt[last], xf, origin_x, origin_y, scale, &sx, &sy);
            i0 = first;
            count = c_pts - 1u;
        } else {
            double ax, ay, bx, by;

            go_device(&pt[last], xf, origin_x, origin_y, scale, &ax, &ay);
            go_device(&pt[first], xf, origin_x, origin_y, scale, &bx, &by);
            sx = (ax + bx) * 0.5;
            sy = (ay + by) * 0.5;
            i0 = first;
            count = c_pts;
        }

        raster_path_move_to(out, sx, sy);
        have_ctrl = false;
        for (k = 0; k < count; k++) {
            uint32_t idx = first + (((i0 - first) + k) % c_pts);
            double px, py;

            go_device(&pt[idx], xf, origin_x, origin_y, scale, &px, &py);
            if (pt[idx].flag & GO_ON_CURVE_POINT) {
                if (have_ctrl) {
                    raster_path_quadratic_curve_to(out, cx, cy, px, py);
                    have_ctrl = false;
                } else {
                    /* "Straight lines are defined by two consecutive on curve points" — TrueType
                       Fundamentals, and it is the only segment in a face that is not a curve. */
                    raster_path_line_to(out, px, py);
                }
            } else {
                /* TWO OFF-CURVE POINTS IN A ROW ARE TWO CURVES WITH AN UNSTORED JOIN BETWEEN THEM, at the
                   midpoint — the omission the chapter above describes, read back. */
                if (have_ctrl)
                    raster_path_quadratic_curve_to(out, cx, cy, (cx + px) * 0.5, (cy + py) * 0.5);
                cx = px;
                cy = py;
                have_ctrl = true;
            }
        }
        /* THE CYCLE CLOSES ONTO THE START POINT, and if a control point is still in hand the closing segment
           is a CURVE and not the straight line the path's own implicit close would draw. */
        if (have_ctrl)
            raster_path_quadratic_curve_to(out, cx, cy, sx, sy);
        raster_path_close_path(out);

        first = last + 1u;
    }
    /* EVERY CONTOUR COST EXACTLY TWO SUBPATHS and the arithmetic is the path component's, not this one's: a
       move opens one, and a close appends the CLOSE opcode and then opens another at the same first point,
       which is what a rendering context's own close does. A contour that emitted a different number is a
       contour this file failed to begin or failed to end. */
    DCHECK(out->nsub == subpaths_before + 2u * (size_t)nc,
           "a glyph's contours did not each open and close exactly one subpath");

    free(pt);
    return GLYPH_OUTLINE_OK;
}

#undef GO_MALFORMED

/* ---- a composite glyph's components ------------------------------------------------------------------ */

/* ONE GLYPH WHOSE COMPONENT LIST IS PART WAY WALKED. The cursor and the end are absolute offsets into the
   outline table rather than into the glyph, so that a frame needs no pointer into a slice its own parent
   also holds. */
typedef struct {
    uint16_t glyph;   /* whose list this is — the ANCESTRY, which is what the acyclicity rule is checked over */
    size_t   cur;     /* the next component record */
    size_t   end;     /* one past this description's last byte */
    bool     more;    /* the last record read said MORE_COMPONENTS, so the list is not finished */
    GoXform  xf;      /* from THIS glyph's design units to the top-level glyph's */
} GoFrame;

#define GO_COMP_MALFORMED(why) \
    do { if (reject) *reject = (why); free(stk); return GLYPH_OUTLINE_MALFORMED; } while (0)

/* THE WHOLE OF ONE GLYPH, SIMPLE OR NOT. A composite "describes an outline indirectly by referencing other
 * glyphs", and the standard says the graph they form "must be acyclic, with every path through the graph
 * leading to a simple glyph as a leaf node" — so this walks that graph depth first and emits at the leaves.
 *
 * THE WALK HOLDS ITS OWN STACK ON THE HEAP AND DOES NOT RECURSE IN C, which is a decision about whose bytes
 * choose the depth. This engine's trampolined heap stack is the JS interpreter's and reaches nothing here:
 * a browser component called from core/paint/display_list_raster.c is ordinary C, so C recursion here would
 * be C stack frames whose COUNT a page's own `@font-face` bytes name. The standard is explicit that there is
 * no ceiling to lean on — "There is no minimum nesting depth that must be supported", and a face's
 * maxComponentDepth is that face's own claim about itself — so the depth is whatever the bytes say, and a
 * C stack overflow driven by them is a trap with no name on it rather than a refusal anybody can read.
 * ON THE HEAP THE SAME BYTES BUY AN ALLOCATION, and an allocation that cannot be served is this file's
 * existing always-fatal CHECK, which is the honest floor and the same one the point array already stands on.
 *
 * NO DEPTH CAP IS IMPOSED AND NONE IS NEEDED, which is CLAUDE.md's §NO BOUNDS rather than an oversight. What
 * terminates the walk is the standard's OWN acyclicity requirement, checked as a claim the bytes make: a
 * component naming a glyph already on the ancestry path is a face that broke that rule, and refusing it is
 * a refusal and not a truncation. Depth is then bounded by the face's own glyph count, because no glyph can
 * appear twice on one path — and that bound is derived from the check rather than imposed beside it.
 *
 * EVERY CLAIM A COMPONENT MAKES IS AN `if`. A component's glyph index comes out of the FACE and not out of a
 * character-map lookup, which is the whole reason it may not simply be handed to `glyph_outline_length`:
 * that accessor's bound is an always-fatal CHECK whose own message says every glyph ID reaching it has
 * already been proved by a 'cmap' walk against this face. A composite's index has not, so it is bounded
 * HERE, by an `if` that yields a named refusal — otherwise a page's own font would hold an abort switch for
 * the release build, which is exactly what §WHOSE-BYTES-STATE-THE-VALUE forbids. */
static GlyphOutlineResult go_expand(const GlyphOutlines *g, uint16_t glyph_id,
                                    double origin_x, double origin_y, double scale,
                                    RasterPath *out, const char **reject)
{
    GoFrame *stk = NULL;
    size_t depth = 0, cap = 0;
    uint16_t child = glyph_id;        /* the glyph resolved on the next turn of the loop */
    GoXform childxf = GO_IDENTITY;
    bool pending = true;

    for (;;) {
        if (pending) {
            size_t off, n;

            pending = false;
            /* THE INDEX IS ALREADY BOUNDED: the top-level one by the accessor's own CHECK, and a component's
               by the `if` below before it is ever named here. */
            off = (size_t)go_loca(g, child);
            n = glyph_outline_length(g, child);
            DCHECK(off <= g->glyf_len && n <= g->glyf_len - off,
                   "a glyph's own slice of the outline table lies outside it, though glyph_outlines_read "
                   "walked the whole offset array to prove that it does not. That is this file's extent "
                   "bookkeeping, not the face's bytes");

            /* NO OUTLINE IS NOT AN ERROR AND IS NOT RARE. The standard says of a glyph with no outline that
               its entry and the next are equal, and names the space character as one; a face says so for
               every format control it covers too, and a COMPONENT may legitimately be one. The answer is an
               empty append, never a refusal that would make a caller treat a word space as a broken font. */
            if (n == 0) {
                /* nothing to emit for this glyph */
            } else if (n < GO_GLYPH_HEADER) {
                GO_COMP_MALFORMED("a glyph description shorter than the ten-byte header every one of them "
                                  "begins with");
            } else if (go_i16(g->glyf + off) >= 0) {
                /* THE SIGN OF THE CONTOUR COUNT IS THE WHOLE DISPATCH: the standard says a value greater
                   than or equal to zero is a simple glyph and a negative one is a composite. A simple glyph
                   is this graph's LEAF and is where every contour in the face is actually emitted. */
                GlyphOutlineResult r = go_simple(g, off, n, &childxf, origin_x, origin_y, scale, out, reject);

                if (r != GLYPH_OUTLINE_OK) {
                    free(stk);
                    return r;
                }
            } else {
                size_t k;

                /* "This graph must be acyclic, with every path through the graph leading to a simple glyph
                   as a leaf node." The ancestry IS the stack, so the rule is one scan of it — and a face
                   that breaks it would otherwise be a walk with no end, driven by a stranger's bytes. */
                for (k = 0; k < depth; k++) {
                    if (stk[k].glyph == child)
                        GO_COMP_MALFORMED("a composite glyph whose components lead back to a glyph already "
                                          "being expanded. OpenType requires the component graph to be "
                                          "ACYCLIC with every path ending at a simple glyph, so a cycle is a "
                                          "face that named an outline which does not exist at any depth");
                }
                if (depth == cap) {
                    size_t ncap = cap ? cap * 2u : 8u;
                    GoFrame *ns = realloc(stk, ncap * sizeof *ns);

                    CHECK(ns != NULL, "out of memory expanding a composite glyph's components");
                    stk = ns;
                    cap = ncap;
                }
                stk[depth].glyph = child;
                stk[depth].cur = off + GO_GLYPH_HEADER;
                stk[depth].end = off + n;
                stk[depth].more = true;   /* the record list is a do-while: there is always a first one */
                stk[depth].xf = childxf;
                depth++;
            }
            continue;
        }

        if (depth == 0)
            break;

        {
            GoFrame *f = &stk[depth - 1];
            const unsigned char *r;
            uint16_t flags, cgid;
            size_t argw, tw;
            int arms;
            int32_t a1, a2;
            GoXform local = GO_IDENTITY;

            if (!f->more) {
                depth--;
                continue;
            }
            DCHECK(f->cur <= f->end, "a component cursor walked past the description that bounds it, which "
                                     "is this file's own bookkeeping and never the face's bytes");

            if (f->end - f->cur < GO_COMPONENT_HEADER)
                GO_COMP_MALFORMED("a composite glyph description that ends inside a component's flags and "
                                  "glyph index. The component list is a do-while over MORE_COMPONENTS, so a "
                                  "description whose last record sets that bit and then stops is one whose "
                                  "own terminator was never written");
            r = g->glyf + f->cur;
            flags = go_u16(r);
            cgid = go_u16(r + 2);
            f->cur += GO_COMPONENT_HEADER;
            f->more = (flags & GO_MORE_COMPONENTS) != 0;

            /* THE ARGUMENT WIDTH IS BIT 0 AND THEIR MEANING IS BIT 1, and the two are independent: "If this
               is set, the arguments are 16-bit (uint16 or int16); otherwise, they are bytes (uint8 or
               int8)" for the first, and "If this is set, the arguments are signed xy values; otherwise, they
               are unsigned point numbers" for the second. The width is decided before the meaning because
               the record has to be STEPPED OVER either way. */
            argw = (flags & GO_ARG_1_AND_2_ARE_WORDS) ? 4u : 2u;
            if (f->end - f->cur < argw)
                GO_COMP_MALFORMED("a composite glyph description that ends inside a component's placement "
                                  "arguments");
            if (flags & GO_ARG_1_AND_2_ARE_WORDS) {
                a1 = go_i16(r + GO_COMPONENT_HEADER);
                a2 = go_i16(r + GO_COMPONENT_HEADER + 2);
            } else {
                a1 = (int32_t)(int8_t)r[GO_COMPONENT_HEADER];
                a2 = (int32_t)(int8_t)r[GO_COMPONENT_HEADER + 1];
            }
            f->cur += argw;

            /* THE THREE SCALE FLAGS ARE MUTUALLY EXCLUSIVE — "no more than one of these may be set" — and a
               record setting two names two different transform widths for one run of bytes, so there is no
               reading of it under which the rest of the list is even located. */
            arms = ((flags & GO_WE_HAVE_A_SCALE) != 0) + ((flags & GO_WE_HAVE_AN_X_AND_Y_SCALE) != 0) +
                   ((flags & GO_WE_HAVE_A_TWO_BY_TWO) != 0);
            if (arms > 1)
                GO_COMP_MALFORMED("a component setting more than one of WE_HAVE_A_SCALE, "
                                  "WE_HAVE_AN_X_AND_Y_SCALE and WE_HAVE_A_TWO_BY_TWO. OpenType makes the "
                                  "three MUTUALLY EXCLUSIVE and each names a different number of F2DOT14 "
                                  "values, so a record setting two does not say where the next one begins");
            tw = (flags & GO_WE_HAVE_A_SCALE) ? 2u
               : (flags & GO_WE_HAVE_AN_X_AND_Y_SCALE) ? 4u
               : (flags & GO_WE_HAVE_A_TWO_BY_TWO) ? 8u : 0u;
            if (f->end - f->cur < tw)
                GO_COMP_MALFORMED("a composite glyph description that ends inside a component's transform");
            {
                const unsigned char *t = g->glyf + f->cur;

                if (flags & GO_WE_HAVE_A_SCALE) {
                    /* "the transformation is even further constrained by xscale and yscale both being set to
                       the single appended value, scale" */
                    local.a = local.d = go_f2dot14(t);
                } else if (flags & GO_WE_HAVE_AN_X_AND_Y_SCALE) {
                    /* "the appended xscale and yscale values are used as above, but the transformation is
                       constrained by scale01 and scale10 both being set to zero" */
                    local.a = go_f2dot14(t);
                    local.d = go_f2dot14(t + 2);
                } else if (flags & GO_WE_HAVE_A_TWO_BY_TWO) {
                    /* "in order, xscale, scale01, scale10, and yscale" — the storage order, which is NOT the
                       order they appear in the two equations. */
                    local.a = go_f2dot14(t);
                    local.b = go_f2dot14(t + 2);
                    local.c = go_f2dot14(t + 4);
                    local.d = go_f2dot14(t + 6);
                }
            }
            f->cur += tw;

            /* THE COMPONENT'S GLYPH INDEX IS THE FACE'S CLAIM AND NOT THIS ENGINE'S, so it is bounded by an
               `if` here rather than by the always-fatal CHECK inside the length accessor. */
            if (cgid >= g->num_glyphs)
                GO_COMP_MALFORMED("a component naming a glyph index at or above 'maxp' — Maximum Profile's "
                                  "numGlyphs. A component's index is stated by the FACE and proved by "
                                  "nothing, unlike a glyph ID that came out of a character-map walk, so a "
                                  "face whose composite references a glyph it does not have names an outline "
                                  "that was never in the file");

            /* THE OTHER PLACEMENT IS A DIFFERENT ALGORITHM AND IS A NAMED RESIDUAL, NOT A REFUSAL. */
            if (!(flags & GO_ARGS_ARE_XY_VALUES)) {
                if (reject) *reject = NULL;
                free(stk);
                return GLYPH_OUTLINE_UNSUPPORTED;
            }

            /* WHETHER THE OFFSET IS TRANSFORMED IS A FLAG AND IS THE CLASSIC PLACE A COMPONENT LANDS SUBTLY
               WRONG. "If the SCALED_COMPONENT_OFFSET flag is set, then the x and y offset values are deemed
               to be in the component glyph's coordinate system, and the scale transformation is applied to
               both values. If the UNSCALED_COMPONENT_OFFSET flag is set, then the x and y offset values are
               deemed to be in the current glyph's coordinate system, and the scale transformation is not
               applied to either value. If neither flag is set, then the rasterizer may apply a default
               behavior. On Microsoft and Apple platforms, the default behavior is the same as when the
               UNSCALED_COMPONENT_OFFSET flag is set; this behavior is recommended for all rasterizer
               implementations."
               SO THE DEFAULT IS THE UNSCALED ONE AND `local.dx`/`local.dy` ARE ALREADY IN THE PARENT'S
               UNITS, which is what lets `go_compose` treat every local transform alike.
               AND A FACE SETTING BOTH IS NOT REFUSED, because the standard says what to do with it in its
               own words — "If a font has both flags set, this is invalid; the rasterizer should use its
               default behavior for this case" — so a refusal here would be this file being stricter than the
               document it implements. */
            local.dx = (double)a1;
            local.dy = (double)a2;
            if ((flags & GO_SCALED_COMPONENT_OFFSET) && !(flags & GO_UNSCALED_COMPONENT_OFFSET)) {
                local.dx = local.a * (double)a1 + local.c * (double)a2;
                local.dy = local.b * (double)a1 + local.d * (double)a2;
            }

            child = cgid;
            childxf = go_compose(&f->xf, &local);
            pending = true;
        }
    }

    free(stk);
    if (reject) *reject = NULL;
    return GLYPH_OUTLINE_OK;
}

#undef GO_COMP_MALFORMED

GlyphOutlineResult glyph_outline_append(const GlyphOutlines *g, uint16_t glyph_id,
                                        double origin_x, double origin_y, double scale,
                                        RasterPath *out, const char **reject)
{
    if (reject) *reject = NULL;
    GO_READY(g);
    DCHECK(out != NULL, "a glyph outline was decoded into no path");
    /* THE PLACEMENT IS THIS CODEBASE'S OWN ARITHMETIC AND NOT A CLAIM THE FACE MAKES, which is what makes an
       assert on it correct rather than an abort switch handed to whoever supplied the font: `scale` is a used
       font-size divided by a unitsPerEm the face reader has already bounded, and the origin is a pen position
       this engine computed. It is asserted HERE because the path builders below drop a non-finite coordinate
       SILENTLY by their own contract — so without this the failure would be a glyph missing some of its
       segments, which looks like a decoding bug and is a caller's number. */
    DCHECK(isfinite(origin_x) && isfinite(origin_y) && isfinite(scale),
           "a glyph was placed at a coordinate or a scale that is not a finite number");

    return go_expand(g, glyph_id, origin_x, origin_y, scale, out, reject);
}
