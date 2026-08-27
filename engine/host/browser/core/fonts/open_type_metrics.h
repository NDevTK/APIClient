/* THE OPENTYPE METRICS TABLES — a face's answer to the only three questions this engine asks it, read out of
 * the face's own bytes. See open_type_metrics.c.
 *
 * THE THREE QUESTIONS, AND THE TABLES THAT ANSWER THEM. OpenType "The OpenType Font File" §Organization of an
 * OpenType Font puts a Table Directory at the head of every sfnt and every one of these behind it:
 *   what is an em worth?          'head' — Font Header Table's `unitsPerEm`
 *   which glyph is this scalar?   'cmap' — Character to Glyph Index Mapping Table
 *   how far does that glyph go?   'hhea' — Horizontal Header Table's `numberOfHMetrics` + 'hmtx' — Horizontal
 *                                 Metrics Table (and 'vhea'/'vmtx' for the other axis), bounded by
 *                                 'maxp' — Maximum Profile's `numGlyphs`
 * Nothing else in a face is read here, which is why core/fonts/default_font_data.c can ship those tables alone.
 *
 * IT IS OFFENSIVE FROM ITS FIRST LINE BECAUSE ITS INPUT IS ATTACKER-SUPPLIED. Today the only bytes handed to
 * it are the ones the engine ships; the moment a page's own `@font-face` bytes arrive through the fetch
 * chokepoint they come here, and a font is a format made of OFFSETS AND COUNTS pointing at each other. So this
 * component draws the line CLAUDE.md's offensive-programming law draws, and draws it once:
 *   A CLAIM THE BYTES MAKE is validated by an `if` that REJECTS — a length that does not fit, an offset past
 *     the end, a `numTables` too large for the file, a Format 4 segment array that does not terminate at
 *     0xFFFF, a glyph ID a subtable maps to that is not a glyph of this face. None of those is a
 *     should-never-happen: they are what a hostile file looks like, and CLAUDE.md is explicit that a state
 *     which must be HANDLED at runtime is an `if` and never a DCHECK. `read` answers false and `reject` names
 *     which rule the bytes broke, so a caller never has to infer the reason from a bare false.
 *   AN INVARIANT THIS COMPONENT ESTABLISHED is a DCHECK — every read below the validation is inside an extent
 *     `read` already proved, every glyph ID a lookup returns is below `num_glyphs` because `read` walked the
 *     whole subtable to prove it. Those assert that this component's OWN logic is correct, which is exactly
 *     what a dev-only abort is for.
 *   THE ENGINE'S OWN SHIPPED FACE FAILING TO READ is a CHECK — see core/css/font_metrics.c, which is where the
 *     shipped bytes are handed in. That one is fatal in release too, because a user agent whose default font
 *     will not parse has no metric for any element of any document and every length it then reports is a
 *     fabrication. It is the data-integrity arm of CLAUDE.md's CHECK, not the invariant arm.
 * The split is not decoration: it is why a page can serve a malformed font without taking the engine down, and
 * why a bug in THIS file takes it down immediately.
 *
 * WHAT `read` PROVES, so that the lookups can be total. It does not merely bounds-check the fields it reads —
 * it walks the entire selected 'cmap' subtable and rejects the face unless EVERY character code the subtable
 * can be asked about maps to a glyph ID below `numGlyphs`. That is what makes `open_type_metrics_glyph_id`
 * assert rather than clamp: a clamp would be a wrong answer with nothing able to see it, and the state it
 * would be clamping is one this component can make impossible instead. */
#ifndef ENGINE_HOST_BROWSER_CORE_FONTS_OPEN_TYPE_METRICS_H
#define ENGINE_HOST_BROWSER_CORE_FONTS_OPEN_TYPE_METRICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ONE FACE, AS THE FIELDS `read` PROVED. Every offset below is a byte offset from the start of `sfnt` and has
   already been shown to lie inside it together with the whole table it names; every count has already been
   shown to fit the table it counts. A reader may take any of them without re-checking — that is the point of
   the type — but only after `read` returned true, which every accessor asserts. */
typedef struct {
    const unsigned char *sfnt;
    size_t               len;
    /* NULL exactly when this face is readable. Otherwise a string literal naming the rule the bytes broke —
       a POSITIVE statement, so that a caller is never left inferring a reason from a bare false. */
    const char *reject;

    uint16_t units_per_em;         /* 'head'.unitsPerEm, 16..16384 */
    uint16_t num_glyphs;           /* 'maxp'.numGlyphs, >= 1 */

    uint16_t number_of_h_metrics;  /* 'hhea'.numberOfHMetrics, 1..num_glyphs */
    size_t   hmtx;                 /* 'hmtx', >= 4*number_of_h_metrics + 2*(num_glyphs-number_of_h_metrics) */

    /* 'hhea'.ascender and 'hhea'.descender, in FONT DESIGN UNITS — OpenType's own FWORDs, so the descender is
       NEGATIVE for a face whose glyphs go below the baseline and this pair is not a CSS metric until a caller
       divides by units_per_em and flips the sign of the second.
       WHY THIS TABLE AND NOT 'OS/2'. CSS 2.2 §10.8.1 "Leading and half-leading"'s note states the preference
       and the fallback in one sentence: "It is recommended that implementations that use OpenType or TrueType
       fonts use the metrics 'sTypoAscender' and 'sTypoDescender' from the font's OS/2 table for A and D (after
       scaling to the current element's font size). In the absence of these metrics, the 'Ascent' and 'Descent'
       metrics from the HHEA table should be used." OpenType's own 'hhea' chapter agrees from the other side —
       "the ascender, descender and linegap values in this table are Apple specific ... The sTypoAscender,
       sTypoDescender and sTypoLineGap fields in the OS/2 table are used on the Windows platform, and are
       recommended for new text-layout implementations". So these two are the FALLBACK, correct only for a face
       with no 'OS/2', which is why the flag below exists rather than a comment saying to remember. */
    int16_t  hhea_ascender;
    int16_t  hhea_descender;

    /* DOES THIS FACE CARRY 'OS/2' — OS/2 and Windows Metrics Table. It is the one table this component notices
       and does not read, and the asymmetry is deliberate: a reader of the pair above is taking §10.8.1's note's
       FALLBACK, and it may only do that "in the absence of these metrics". A face that has 'OS/2' therefore
       makes that reader wrong, and this flag is what lets it CRASH instead of quietly answering off the second
       choice. Reading sTypoAscender/sTypoDescender is the work that flag names; nothing here reads them yet,
       and a field written with no reader would be the mirror defect. */
    bool     has_os2;

    uint16_t cmap_format;          /* 4 (Segment mapping to delta values) or 12 (Segmented coverage) */
    size_t   cmap_sub;             /* the selected subtable */
    size_t   cmap_sub_len;         /* its own `length` field, validated against the 'cmap' extent */

    /* 'vhea'/'vmtx' are OPTIONAL in OpenType and absent from most Latin faces. A face without them has no
       vertical advance to report, and css-writing-modes-4 §5.1.1 "Vertical Typesetting and Font Features"
       makes that a capability rather than a substitution: "The UA must synthesize vertical font metrics for
       fonts that lack them. (This specification does not define heuristics for synthesizing such metrics.)"
       So this flag is READ by the caller and the lookup crashes on a face that lacks them — never a silent
       fallback to the horizontal advance, which is a different number no spec asked for. */
    bool     has_vertical;
    uint16_t num_of_long_ver_metrics;  /* 'vhea'.numOfLongVerMetrics, 1..num_glyphs */
    size_t   vmtx;
} OpenTypeMetrics;

/* READ a face out of `sfnt[0..len)`, validating every claim the bytes make. Returns false with `m->reject` set
   for anything that is not a face this engine can measure; `m` is otherwise untouched-by-guesswork (zeroed),
   so a caller that ignores the result reads zeros rather than plausible numbers — and every accessor asserts
   the read succeeded, so it cannot ignore it quietly. The bytes are BORROWED: `sfnt` must outlive `m`. */
bool open_type_metrics_read(OpenTypeMetrics *m, const unsigned char *sfnt, size_t len);

/* THE GLYPH this face uses for a Unicode scalar value, through the 'cmap' subtable `read` selected. A codepoint
   the face does not cover is glyph 0 and NOT an error: OpenType 'cmap' §Table overview says "character codes
   that do not correspond to any glyph in the font should be mapped to glyph index 0. The glyph at this location
   must be a special glyph representing a missing character, commonly known as .notdef", and css-fonts-4 §5.2
   "Matching font styles" says a user agent with no other face renders it — "If a particular character cannot be
   displayed using any font, the user agent should indicate by some means that a character is not being
   displayed, displaying either a symbolic representation of the missing glyph (e.g. using a Last Resort Font)
   or using the missing character glyph from a default font". So .notdef's advance is a REAL measurement of the
   glyph that really gets drawn, which is why this returns 0 rather than failing: the caller that wants
   "does this face cover it?" is asking a different question and asks it below. */
uint16_t open_type_metrics_glyph_id(const OpenTypeMetrics *m, uint32_t codepoint);

/* DOES THIS FACE COVER THIS SCALAR VALUE — glyph 0 is the answer to two different questions and this is the one
   that tells them apart. css-values-4 §6.1.1's `ch` and `ic` are stated over the "0" and "水" glyph "in the
   font used to render it", with a MUST-ASSUME value "in the cases where it is impossible or impractical to
   determine" the measure; a face with no such glyph is that case exactly, and it must not be answered with
   .notdef's advance. Both callers of the pair are in core/css/font_metrics.c and the difference between them is
   the whole reason this entry exists. */
bool open_type_metrics_covers(const OpenTypeMetrics *m, uint32_t codepoint);

/* THE ADVANCE WIDTH of a glyph in FONT DESIGN UNITS — OpenType 'hmtx' — Horizontal Metrics Table's
   LongHorMetric.advanceWidth. Divide by `units_per_em` for the multiple of the em CSS wants. */
uint16_t open_type_metrics_advance_width(const OpenTypeMetrics *m, uint16_t glyph_id);

/* THE ADVANCE HEIGHT of a glyph in font design units — 'vmtx' — Vertical Metrics Table's advanceHeight. ONLY
   for a face with `has_vertical`; asking a face without one crashes, naming css-writing-modes-4 §5.1.1's
   synthesis as the thing to build. */
uint16_t open_type_metrics_advance_height(const OpenTypeMetrics *m, uint16_t glyph_id);

#endif
