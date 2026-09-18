/* A GLYPH'S OUTLINE, DECODED OUT OF A FACE'S OWN BYTES INTO THE ONE SEGMENT STREAM THIS ENGINE FILLS.
 *
 * WHY THIS EXISTS NOW AND DID NOT BEFORE. core/fonts/open_type_metrics.h reads a face to answer three
 * questions — what is an em worth, which glyph is this scalar, how far does that glyph go — and every one of
 * them is a MEASUREMENT. Nothing in this engine has ever asked a face for a SHAPE, so the shipped subset
 * dropped the two tables that carry one and both of the files that drop them say, in their own words, that
 * nothing here rasterizes. That is no longer true: core/graphics/rasterizer.h fills a path and
 * core/paint/display_list_raster.h already drives it from a display list, so the engine paints boxes, borders
 * and the canvas and cannot paint TEXT. This component is the half of that gap that is DECODING; the half
 * that is DATA was a residual at the foot of this file and is RETIRED: the shipped face now carries the two
 * tables, which core/fonts/default_font_data.h records at the place the record stood. THE POINTER IS
 * REWRITTEN RATHER THAN CUT because a reader who finds none here and reads the paragraph above it will
 * conclude the data half is still open — a retired record's last falsehood is the sentence somewhere else
 * that still points at it.
 *
 * WHAT IT PRODUCES IS A `RasterPath` AND NOT A SECOND PATH REPRESENTATION. core/graphics/raster_path.h says
 * of its own stream that the segment vocabulary is core/canvas/canvas_path.h's and is not restated, because
 * "a second enum here with the same six values would be the second spelling its own paragraph forbids"; the
 * same argument reaches a third producer with more force, since a glyph is made of exactly the segments that
 * vocabulary already has. A TrueType outline is second-order, so every curve in a face is one QUAD op and
 * this component never emits a CUBIC, an ARC or a rect.
 *
 * AND `RasterPath` RATHER THAN `CanvasPath`, WHICH IS A DECISION AND NOT A DEFAULT. core/canvas/canvas_path.h
 * holds its stream in a JS Array because a flow MUTATES a page's path, so that list is per-flow state that
 * has to fork, park and resume. None of that reaches a glyph. A face is BORROWED IMMUTABLE BYTES and a
 * decode is a pure function of (those bytes, a glyph ID, a placement): it mutates no page state, it is not
 * observable half-done, and there is no realm anywhere in it. Building a JS Array here would mint a
 * concolic value to describe a letter, in a road whose own display-list half states that it does not reach a
 * realm at all — and it would put a `JS_GetPropertyUint32` in the inner loop of the glyph cache.
 *
 * THE COORDINATES THIS EMITS ARE DEVICE PIXELS, WHICH IS WHY THE PLACEMENT IS AN ARGUMENT AND NOT A CALLER'S
 * LATER WALK. core/graphics/raster_path.h states as an invariant that "EVERY COORDINATE HERE IS A DEVICE
 * PIXEL AND NOTHING HERE APPLIES A MATRIX", so a `RasterPath` carrying font design units would be a stream
 * that lies about its own type. A glyph's own coordinate system differs from the device's in three ways at
 * once and all three are resolved here, in one multiply-add per coordinate:
 *   SCALE. A face's points are FUnits, which are a fraction of an em; `scale` is device pixels per FUnit,
 *     which a caller computes as a used font-size divided by core/fonts/open_type_metrics.h's
 *     `units_per_em`. It is not stored on the face because one face is drawn at many sizes in one document.
 *   ORIGIN. A glyph's own origin is its pen position, and the y of that origin is the BASELINE — the
 *     TrueType chapter below states the convention and names the 'head' flags bit that asserts it.
 *   DIRECTION. FUnits increase UPWARD and this engine's device rows increase DOWNWARD, so the y term is
 *     SUBTRACTED. A decoder that emitted +y would produce every glyph upside down about its baseline, which
 *     is a defect that looks like a font bug and is an axis bug.
 *
 * THE BYTES ARE NOT THIS CODEBASE'S AND THE SPLIT IS core/fonts/open_type_metrics.h's, DRAWN THE SAME WAY.
 * A face is a format made of OFFSETS AND COUNTS pointing at each other, and a page's own `@font-face` bytes
 * reach the same decoders the shipped face does. So:
 *   A CLAIM THE BYTES MAKE is an `if` that REFUSES — an offset array too short for the glyph count, offsets
 *     that do not ascend, a last offset past the end of the outline table, a contour count that does not fit
 *     the glyph's own extent, a flag run that overruns it, a coordinate array that ends early. None of those
 *     is a should-never-happen; they are what a hostile or merely broken file looks like, and CLAUDE.md is
 *     explicit that a state which must be HANDLED at runtime is an `if` and never a DCHECK. Every one of them
 *     yields this component's declared absence — a MALFORMED result with a `reject` string naming the rule
 *     the bytes broke, so no caller has to infer a reason from a bare false and no caller is handed a
 *     PLAUSIBLE outline built out of bytes that were never a glyph.
 *   A CLAIM A COMPONENT RECORD MAKES is the same `if` and is the place it is easiest to get wrong, because a
 *     component's GLYPH INDEX looks like every other glyph ID in this engine and is not one: every other one
 *     comes out of a character-map walk that has already proved the bound, and this one is simply stated by
 *     the face. Handing it to the always-fatal bound below would put an abort switch for the RELEASE build
 *     inside a page's own font file.
 *   AN INVARIANT THIS COMPONENT ESTABLISHED is a DCHECK — the point total it derived, the cursor it advanced,
 *     the op stream it wrote. Those assert that this file's own logic is correct, which is what a dev-only
 *     abort is for.
 *   A BOUND WHOSE READ HAPPENS IN RELEASE TOO is a CHECK, and this is the one place this file is stricter
 *     than its neighbour. A glyph ID indexes the offset array directly, so the bound on it is load-bearing
 *     in EVERY build and a dev-only guard would leave a release read walking off the table: CLAUDE.md's rule
 *     is that a guard survives in the build where the dereference happens, and this dereference happens in
 *     both. It is the memory-safety arm and not the invariant arm.
 *
 * WHAT MAKES THE LOOKUPS TOTAL. `glyph_outlines_read` walks the WHOLE offset array once and refuses the pair
 * unless every entry ascends and the last one lies inside the outline table. That is what lets
 * `glyph_outline_length` answer without re-checking and what lets the decoder treat its own slice as an
 * extent it may read — the same bargain core/fonts/open_type_metrics.c strikes when it walks an entire
 * 'cmap' subtable so that a glyph lookup can assert rather than clamp. */
#ifndef ENGINE_HOST_BROWSER_CORE_FONTS_GLYPH_OUTLINE_H
#define ENGINE_HOST_BROWSER_CORE_FONTS_GLYPH_OUTLINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/graphics/raster_path.h"   /* the ONE segment stream a fill consumes */

/* THE OUTLINE TABLES OF ONE FACE, AS `glyph_outlines_read` PROVED THEM. Both spans are BORROWED and must
   outlive this struct, exactly as a face's bytes are borrowed by `OpenTypeMetrics`.
   THE TWO TABLES ARE TAKEN AS SPANS RATHER THAN FOUND IN AN sfnt, and that is deliberate rather than
   unfinished: locating them is the table directory's job, and a component that both FOUND and DECODED them
   could not be exercised on bytes a fixture states.
   THIS CLAUSE ALSO READ `this engine's shipped face carries neither of them yet` AND IS REWRITTEN RATHER THAN
   CUT, because a reader who re-derives the span argument from the state of the face will write it again: the
   face carries BOTH now, and the argument for spans never rested on that — it rests on the split between
   finding and decoding, which is what makes the sentence survive the fact going the other way.
   WHAT CLOSES IT IS NOT A RESIDUAL HERE AND USED TO BE. The pair is LOCATED by
   core/fonts/open_type_metrics.h, which records where the two tables are and reads the offset format out of
   the face's own header table, and core/css/font_metrics.h joins that to this decoder for the ONE face this
   user agent has. So the caller that hands these spans over is a component and no longer a gap — and this
   entry still takes them as spans, for the reason above, so that a fixture can state bytes of its own. */
typedef struct {
    const unsigned char *glyf;      /* the outline table */
    size_t               glyf_len;
    const unsigned char *loca;      /* the offset array — glyf_len bytes of glyph data indexed by it */
    size_t               loca_len;

    uint16_t num_glyphs;            /* 'maxp' — Maximum Profile's numGlyphs, >= 1 */
    /* 'head' — Font Header Table's indexToLocFormat: false for the 16-bit offset array, true for the 32-bit
       one. Getting this wrong does not fail — it reads a DIFFERENT, self-consistent-looking offset array out
       of the same bytes, which is why it is a field of the validated struct and not a parameter of the
       lookup: the whole array is walked against it ONCE, so a wrong format has to survive an ascending check
       over every entry and a bound on the last, rather than being noticed at one glyph. */
    bool     long_loca;

    /* NULL exactly when this pair is usable. Otherwise a string literal naming the rule the bytes broke — a
       POSITIVE statement, for `OpenTypeMetrics.reject`'s reason. */
    const char *reject;
} GlyphOutlines;

/* WHAT HAPPENED TO ONE GLYPH. THREE STATES AND NOT A BOOL, because two of them take opposite work and a
   caller that could not tell them apart would be reading one answer for two questions: a VALID glyph this
   component does not build yet is a capability to add, and a malformed one is a file that lied. Merging them
   would report a good font as a broken font.
   THE MIDDLE OUTCOME USED TO BE SPELLED `GLYPH_OUTLINE_COMPOSITE` AND IS RENAMED RATHER THAN RETIRED. A
   composite glyph is now DECODED — it is the ordinary shape of every accented letter and the decoder walks
   its components — so a name that said "composite" would name a case that answers OK, and the member beside
   it would read as the only one left. What the outcome was always FOR is the category and not that one
   member: a glyph the face is entitled to carry and this decoder cannot yet build. Exactly one construct is
   in it today and the residual at `glyph_outline_append` names it, which is the same thing this enum's third
   state has always meant and is now called. */
typedef enum {
    GLYPH_OUTLINE_OK = 0,      /* appended — possibly nothing, for a glyph that has no outline */
    GLYPH_OUTLINE_UNSUPPORTED, /* a valid glyph built in a way this decoder does not have; see the residual */
    GLYPH_OUTLINE_MALFORMED    /* the bytes broke a rule, which `reject` names */
} GlyphOutlineResult;

/* READ the outline table pair, validating every claim the offset array makes. Returns false with `g->reject`
   set for anything that is not a pair this engine can decode; `g` is otherwise zeroed, so a caller that
   ignores the result reads zeros rather than plausible spans, and every accessor asserts the read succeeded.
   `long_loca` is 'head' — Font Header Table's indexToLocFormat and `num_glyphs` is 'maxp' — Maximum
   Profile's numGlyphs; both come from the face's own tables and neither is inferred here. */
bool glyph_outlines_read(GlyphOutlines *g, const unsigned char *glyf, size_t glyf_len,
                         const unsigned char *loca, size_t loca_len,
                         uint16_t num_glyphs, bool long_loca);

/* HOW MANY BYTES OF GLYPH DESCRIPTION THIS GLYPH HAS. Zero is not an error and is not rare: it is how a face
   says a glyph has no outline at all, which is what the space character and most format controls are. A
   caller that wants "is there ink here" asks this; a caller that wants the ink calls the appender, which
   answers OK and appends nothing for the same glyph. */
size_t glyph_outline_length(const GlyphOutlines *g, uint16_t glyph_id);

/* APPEND one glyph's contours to `out`, placed by (`origin_x`, `origin_y`) and `scale`.
   A COMPOSITE GLYPH IS ONE OF THEM AND NOT A SECOND ENTRY. OpenType 'glyf' — Glyph Data's "Composite glyph
   description" makes a composite "a directed graph" whose every path ends at a simple glyph, so this walks
   that graph and emits at its leaves — which is why one call answers for a letter, an accent, and the letter
   made of both. The walk holds its own stack on the heap and never recurses in C, because the DEPTH of that
   graph is stated by the face and the standard puts no ceiling on it ("There is no minimum nesting depth
   that must be supported"); what ENDS the walk is the standard's own requirement that the graph be acyclic,
   refused here as a claim the bytes make.
   NAMED RESIDUAL — A COMPONENT PLACED BY POINT ALIGNMENT. WHAT IS NOT COVERED: a component record whose
   ARGS_ARE_XY_VALUES flag is CLEAR, which OpenType says makes "argument1 a point number in the parent glyph
   (from contours incoporated and re-numbered from previous component glyphs)" and argument2 "a point number
   (prior to re-numbering) from the child component glyph", the child then being positioned "by aligning the
   two points". That is a different algorithm and not a missing branch of this one: it needs the parent's
   ACCUMULATED POINT LIST in design units, renumbered as each child is incorporated, where this decoder keeps
   no points at all between components and emits device-pixel segments as it goes; and the standard adds that
   "Phantom points from the parent or the child may be referenced", which are derived from 'hmtx' and are not
   in the outline tables this component is given. WHAT THE NEXT DIFF BUILDS: the retained per-composite point
   array with the spec's renumbering, so that argument1 indexes it — which means the leaf emitter stops
   writing to the path directly and writes points a second pass turns into segments. HOW ITS ABSENCE WOULD
   SHOW: a caller sees `GLYPH_OUTLINE_UNSUPPORTED` for a glyph whose description this decoder read far enough
   to know it is a composite, with `reject` NULL, and the path holds whatever components preceded the
   point-matched one.
   `origin` is the pen position in device pixels and its y is the BASELINE; `scale` is device pixels per font
   design unit, which is a used font-size divided by the face's unitsPerEm. The placement is this codebase's
   own arithmetic over a length CSS has already refused a non-finite value for, so a non-finite one is a
   DCHECK and not a refusal — and it is asserted here rather than left to the path builders, whose own
   contract is to drop a non-finite coordinate SILENTLY, which would delete segments and leave a glyph that
   is subtly the wrong shape rather than a crash anybody can act on.
   APPENDS: `out` is not cleared, so a caller may lay a whole run of glyphs into one path and fill it once.
   ON ANYTHING BUT OK, `out` MAY ALREADY HAVE BEEN APPENDED TO — a malformed glyph is detected part way
   through its own contours, and unwinding would mean either a second path or a truncation point this
   component cannot name. A caller that must not paint a partial glyph fills into its own path and discards
   it; a caller laying a run may simply stop. That is stated because it is the one thing about this entry a
   reader could get wrong silently.
   `reject` is set to a string literal on MALFORMED and to NULL otherwise; it may be NULL if the caller does
   not want the reason. */
GlyphOutlineResult glyph_outline_append(const GlyphOutlines *g, uint16_t glyph_id,
                                        double origin_x, double origin_y, double scale,
                                        RasterPath *out, const char **reject);

#endif /* ENGINE_HOST_BROWSER_CORE_FONTS_GLYPH_OUTLINE_H */
