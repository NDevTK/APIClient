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

static uint16_t go_end_pt(const unsigned char *d, int c)
{
    return go_u16(d + GO_GLYPH_HEADER + 2u * (size_t)c);
}

/* A POINT IN DEVICE PIXELS. Three facts in two lines, and the subtraction is the one a reader should check:
   font design units increase UPWARD from the baseline and this engine's device rows increase DOWNWARD, so a
   decoder that added here would produce every glyph mirrored about its own baseline. */
static void go_device(const GoPoint *p, double ox, double oy, double sc, double *x, double *y)
{
    *x = ox + (double)p->x * sc;
    *y = oy - (double)p->y * sc;
}

#define GO_MALFORMED(why) do { if (reject) *reject = (why); free(pt); return GLYPH_OUTLINE_MALFORMED; } while (0)

GlyphOutlineResult glyph_outline_append(const GlyphOutlines *g, uint16_t glyph_id,
                                        double origin_x, double origin_y, double scale,
                                        RasterPath *out, const char **reject)
{
    const unsigned char *d;
    GoPoint *pt = NULL;
    size_t off, n, cur;
    uint32_t npts, i, first, last, c_pts, i0, count, k;
    int nc, c;
    int64_t acc;
    size_t subpaths_before;

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

    off = (size_t)go_loca(g, glyph_id);                          /* bounds-checked by the length accessor */
    n = glyph_outline_length(g, glyph_id);
    DCHECK(off <= g->glyf_len && n <= g->glyf_len - off,
           "a glyph's own slice of the outline table lies outside it, though glyph_outlines_read walked the "
           "whole offset array to prove that it does not. That is this file's extent bookkeeping, not the "
           "face's bytes");

    /* NO OUTLINE IS NOT AN ERROR AND IS NOT RARE. The standard says of a glyph with no outline that its
       entry and the next are equal, and names the space character as one; a face says so for every format
       control it covers too. The answer is an empty append and an OK, never a refusal that would make a
       caller treat a word space as a broken font. */
    if (n == 0)
        return GLYPH_OUTLINE_OK;

    d = g->glyf + off;
    if (n < GO_GLYPH_HEADER)
        GO_MALFORMED("a glyph description shorter than the ten-byte header every one of them begins with");

    /* THE SIGN OF THE CONTOUR COUNT IS THE WHOLE DISPATCH: the standard says a value greater than or equal to
       zero is a simple glyph and a negative one is a composite. */
    nc = go_i16(d);
    if (nc < 0)
        return GLYPH_OUTLINE_COMPOSITE;
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
            go_device(&pt[first], origin_x, origin_y, scale, &sx, &sy);
            i0 = first + 1u;
            count = c_pts - 1u;
        } else if (pt[last].flag & GO_ON_CURVE_POINT) {
            go_device(&pt[last], origin_x, origin_y, scale, &sx, &sy);
            i0 = first;
            count = c_pts - 1u;
        } else {
            double ax, ay, bx, by;

            go_device(&pt[last], origin_x, origin_y, scale, &ax, &ay);
            go_device(&pt[first], origin_x, origin_y, scale, &bx, &by);
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

            go_device(&pt[idx], origin_x, origin_y, scale, &px, &py);
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
