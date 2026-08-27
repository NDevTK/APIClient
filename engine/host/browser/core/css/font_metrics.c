/* The first available font's metrics. See font_metrics.h for which face this is, for why three of §6.1.1's
   metrics are the spec's own numbers rather than this component's, for why the ascent is the other kind of
   answer, and for what is deliberately absent. */
#include "check.h"
#include "core/css/css_length.h"
#include "core/css/font_metrics.h"

/* §6.1.1's ASSUMED X-HEIGHT, in the section's own words: "in the cases where it is impossible or impractical
   to determine the x-height, a value of 0.5em must be assumed". The antecedent — that the user agent cannot
   determine the metric — is literally true of a face with no glyph outlines, so this is a citation and not a
   calibration. It is NOT derivable from the assumed "0" glyph's width below even though the two numbers
   coincide: that coincidence is an accident of the section's own prose, and the day a real face lands they
   separate. */
double font_metrics_x_height_em(void)
{
    return 0.5;
}

/* §6.1.1's TWO MUST-ASSUME ADVANCE MEASURES, one row per (GLYPH, DIRECTION) because that is the shape a face's
   own metrics have and because the section states one of them per direction and the other over both:
     "0"  — "in the cases where it is impossible or impractical to determine the measure of the '0' glyph, it
            must be assumed to be 0.5em WIDE BY 1em TALL", which the section then restates as a consequence
            about the unit: "the ch unit falls back to 0.5em in the general case, and to 1em when it would be
            typeset upright". TWO NUMBERS, and which one is the advance measure is the direction.
     "水" — "in the cases where it is impossible or impractical to determine the IDEOGRAPHIC ADVANCE MEASURE,
            it must be assumed to be 1em". The sentence is stated over the advance measure ITSELF, which is
            already the direction-selected quantity, so it fixes the same number whichever direction selects
            it — and the two identical rows below say that on purpose rather than by a missing row.
   WRITING THE `水` ROWS OUT TWICE IS THE POINT, NOT REDUNDANCY. A single direction-independent entry would
   make the caller's obligation to resolve the direction disappear for one glyph and remain for the other,
   which is exactly how the two units came apart before this entry existed. */
static const struct { uint32_t cp; FontMetricsAdvanceDirection dir; double em; } FONT_METRICS_ASSUMED_ADVANCE[] = {
    { 0x0030, FONT_METRICS_ADVANCE_HORIZONTAL, 0.5 },
    { 0x0030, FONT_METRICS_ADVANCE_VERTICAL,   1.0 },
    { 0x6C34, FONT_METRICS_ADVANCE_HORIZONTAL, 1.0 },
    { 0x6C34, FONT_METRICS_ADVANCE_VERTICAL,   1.0 },
};

double font_metrics_advance_measure_em(uint32_t codepoint, FontMetricsAdvanceDirection direction)
{
    unsigned i;

    DCHECK(direction == FONT_METRICS_ADVANCE_HORIZONTAL || direction == FONT_METRICS_ADVANCE_VERTICAL,
           "css-values-4 §6.1.1's advance measure was asked for in neither of the two directions a glyph has "
           "an advance in. The enum is OpenType's own pair — 'hmtx' — Horizontal Metrics Table's advanceWidth "
           "and 'vmtx' — Vertical Metrics Table's advanceHeight — so a third value is a caller that resolved "
           "the element's `writing-mode` and `text-orientation` into something that is not an answer");
    DCHECK(codepoint <= 0x10FFFF && !(codepoint >= 0xD800 && codepoint <= 0xDFFF),
           "css-values-4 §6.1.1's advance measure was asked for something that is not a Unicode scalar value — "
           "a surrogate code point or one past U+10FFFF. A glyph is selected for a SCALAR VALUE, and OpenType "
           "'cmap' — Character to Glyph Index Mapping Table maps nothing else, so a lone surrogate here is a "
           "caller that split a string between its two halves");
    for (i = 0; i < sizeof(FONT_METRICS_ASSUMED_ADVANCE) / sizeof(FONT_METRICS_ASSUMED_ADVANCE[0]); i++)
        if (FONT_METRICS_ASSUMED_ADVANCE[i].cp == codepoint && FONT_METRICS_ASSUMED_ADVANCE[i].dir == direction)
            return FONT_METRICS_ASSUMED_ADVANCE[i].em;
    DFAIL("css-values-4 §6.1.1 \"Font-relative Lengths: the em, rem, ex, rex, cap, rcap, ch, rch, ic, ric, lh, "
          "rlh units\" states a MUST-ASSUME advance measure for the \"0\" (U+0030) and \"水\" (U+6C34) glyphs "
          "and for NO OTHER, so this table has no row for the codepoint asked for and must not invent one. A "
          "third number here would not be a rougher answer, it would be a WRONG ONE with no assert able to see "
          "it: every glyph carrying the same advance makes a string's width a function of its LENGTH alone, "
          "which is the input css-text-3 §5 \"Line Breaking and Word Boundaries\" picks a soft wrap "
          "opportunity with — so every break position, every line count, and every block container's height "
          "through CSS 2.2 §10.6.3 comes out wrong while every number in the chain stays finite and plausible."
          "\n"
          "WHAT TO BUILD IS THE FIRST AVAILABLE FONT AS A REAL FACE, and core/css/font_metrics.h states where "
          "its bytes come from and why not from the three nearer places: this user agent's own default face "
          "(css-fonts-4 §5.2 \"Matching font styles\": \"or a USER AGENT'S DEFAULT FONT if none are "
          "available\"), compiled into the engine so that the native host and the wasm host answer one "
          "question one way, with a page's own `@font-face` bytes arriving later through the fetch chokepoint "
          "into the SAME parser — which is why that parser is offensive from its first line: those bytes are "
          "attacker-supplied.\n"
          "THE PARSE IS A FAITHFUL PORT OF NAMED TABLES AND NOT A HAND-ROLL. OpenType \"Organization of an "
          "OpenType Font\" gives the sfnt TableDirectory — `sfntVersion` (0x00010000 for TrueType outlines or "
          "'OTTO' for CFF), `numTables`, then one TableRecord per table carrying `tableTag`, `checksum`, "
          "`offset` and `length`. From it:\n"
          "  'head' — Font Header Table: `unitsPerEm` (16 to 16384), the divisor that turns a design-unit "
          "advance into the multiple of the em this entry returns.\n"
          "  'cmap' — Character to Glyph Index Mapping Table: the EncodingRecord array (`platformID`, "
          "`encodingID`, `subtableOffset`); take the Windows Unicode subtables the spec names — platform 3 "
          "encoding 1 (Unicode BMP, subtable Format 4: Segment mapping to delta values) or platform 3 encoding "
          "10 (Format 12: Segmented coverage, which is the one that reaches U+10000 and above) — to map a "
          "scalar value to a glyph ID.\n"
          "  'hhea' — Horizontal Header Table: `numberOfHMetrics`, and 'hmtx' — Horizontal Metrics Table: "
          "`hMetrics[numberOfHMetrics]` of LongHorMetric{`advanceWidth`, `lsb`}, indexed by glyph ID, where "
          "the LAST record's advance applies to every remaining glyph ID (the remaining count is 'maxp' — "
          "Maximum Profile's `numGlyphs` minus `numberOfHMetrics`). That is FONT_METRICS_ADVANCE_HORIZONTAL.\n"
          "  'vhea' — Vertical Header Table: `numOfLongVerMetrics`, and 'vmtx' — Vertical Metrics Table: the "
          "vMetrics array of {`advanceHeight`, `topSideBearing`} with the same last-record rule. That is "
          "FONT_METRICS_ADVANCE_VERTICAL, and a face LACKING those two tables is a NAMED unbuilt capability "
          "rather than an unanswerable question: css-writing-modes-4 §5.1.1 \"Vertical Typesetting and Font "
          "Features\" says upright typesetting uses \"vertical font metrics\" and that \"the UA MUST "
          "SYNTHESIZE vertical font metrics for fonts that lack them\", while stating in the same breath that "
          "\"this specification does not define heuristics for synthesizing such metrics\". So the lookup "
          "asserts on the missing table and the synthesis is built beside it — never a silent fallback to the "
          "horizontal advance, which is a DIFFERENT number the spec never asked for.\n"
          "AND THE SIGNATURE CHANGES WHEN THE FACE LANDS, which is the reason this entry answers a bare ratio "
          "today. An advance read off a face is a MEASUREMENT of a picked face rather than a number §6.1.1 "
          "fixes, so it becomes what the ascent below already is — a `CssPx` carrying a CSS_ENV_* source key, "
          "taking the realm of the document that read it. A page reads exactly this number through the width "
          "of a text box, which is the largest font-fingerprinting surface on the web, so a bare number there "
          "would delete the arm another reader's face takes");
    return 0.0;
}

/* THE ONE PICKED NUMBER IN THIS FILE — CSS 2.1 §10.8.1's `A` as a multiple of the em, which is the form every
   font metric takes because §10.8.1 states it "for a given font AT A GIVEN SIZE" and a face's metrics scale
   with the size. It is the only value here a reader has to weigh rather than read off the spec.
   WHY THIS POINT AND NOT ANOTHER, stated so the next reader can disagree with a reason. One spec-fixed number
   bounds it from below and one recommendation bounds the face from above. §6.1.1 assumes an x-height of 0.5em
   when it cannot be measured, and no face's characteristic height above the baseline is below its x-height, so
   `A` is at least 0.5em — asserted below, which is what keeps the picked number and the spec-fixed one ONE
   COHERENT FACE rather than two. CSS 2.1 §10.8 recommends "a used value for 'normal' between 1.0 to 1.2", and
   §10.8.1 makes that value a function of `AD = A + D`; a Western text face spends roughly a quarter of that
   sum below the baseline, so an `A` near four fifths of the em is the middle of the range that satisfies both
   once `D` exists to be checked against it. THAT CHECK IS WRITTEN NOW, over the pair, in
   `font_metrics_normal_line_height_px` below — which is where the sum is formed and therefore the only place
   §10.8's sentence is about something this file holds.
   THE TWO NUMBERS ARE ONE FACE AND WERE PICKED TOGETHER. `D` is 0.3em, which puts `AD` at 1.1em — the exact
   midpoint of §10.8's recommended 1.0 to 1.2 — and puts 0.273 of the pair below the baseline, which is the
   "roughly a quarter" this paragraph reasoned from before `D` existed. A reader who wants to move either has
   two asserts to satisfy and both name their section. */
#define FONT_METRICS_ASCENT_EM  0.8
#define FONT_METRICS_DESCENT_EM 0.3

CssPx font_metrics_ascent_px(JSContext *realm, CssPx font_size)
{
    if (realm == NULL)
        DFAIL("CSS 2.1 §10.8.1 (Leading and half-leading)'s `A` was asked for with no realm. The NUMBER needs "
              "none — this user agent's installed face is the same face whether or not the document is being "
              "presented, which is why its row in core/frame/viewport.c's seam is not a presented one — but "
              "the SOURCE KEY the fact is minted under is keyed on the document that read it, so a child "
              "navigable's environment and its parent's stay two questions rather than one. The caller is a "
              "css-values-4 §6.1.1 unit resolving on an element, and every element has a node document; one "
              "that is no navigable's ACTIVE document reaches this exactly as core/css/font_size_functions.c's "
              "`medium` does and takes the same answer");
    DCHECK(FONT_METRICS_ASCENT_EM >= font_metrics_x_height_em(),
           "the picked ascent of the first available font is BELOW the x-height css-values-4 §6.1.1 assumes "
           "for it. No face has a characteristic height above the baseline smaller than the height of its own "
           "lowercase letters, so the two numbers no longer describe one face — and the consequence is visible "
           "in the units §6.1.1 defines over them: an undeterminable cap-height falls back to the ascent, so a "
           "`1cap` box would come out SHORTER than a `1ex` one");
    /* THE PRODUCT IS FORMED HERE so the two facts are unioned by css_length.h's arithmetic rather than by
       whichever caller remembered to: the answer is a joint function of the reader's font size, which the
       operand already carries, and of this user agent's face. */
    return css_px_mul(font_size, css_px_env(CSS_ENV_FONT_ASCENT, realm, FONT_METRICS_ASCENT_EM));
}

CssPx font_metrics_normal_line_height_px(JSContext *realm, CssPx font_size)
{
    CssPx descent;

    if (realm == NULL)
        DFAIL("CSS 2.1 §10.8.1 (Leading and half-leading)'s `AD` was asked for with no realm — the same "
              "question the ascent above answers, for the same reason: the two metrics are a source key each, "
              "and a key is keyed on the document that read it");
    /* §10.8's OWN RECOMMENDATION, ASSERTED OVER THE PAIR — "we recommend a used value for 'normal' between
       1.0 to 1.2". It is a statement about the SUM, which is why it could not be checked where `A` was picked
       and is checked here, at the one place the sum is formed. A user agent is permitted to disagree with a
       recommendation; what it must not do is disagree by ACCIDENT, which is what an unasserted pair of picked
       numbers drifting apart would be. The bounds are inclusive because the spec's "between 1.0 to 1.2" names
       both endpoints as acceptable values rather than as strict limits. */
    DCHECK(FONT_METRICS_ASCENT_EM + FONT_METRICS_DESCENT_EM >= 1.0 &&
               FONT_METRICS_ASCENT_EM + FONT_METRICS_DESCENT_EM <= 1.2,
           "the first available font's picked `A` and `D` sum to a `normal` line height OUTSIDE the range CSS "
           "2.1 §10.8 recommends — \"we recommend a used value for 'normal' between 1.0 to 1.2\". Under 1.0 "
           "successive lines overlap at the default font size and over 1.2 every block of text on every page "
           "is loosely set, and either is a user agent disagreeing with the spec's own guidance by accident "
           "rather than by decision. Move one of the two numbers in core/css/font_metrics.c, or state the "
           "disagreement here on purpose");
    /* §10.8.1: "we also define AD = A + D, the distance from the top to the bottom". The sum is formed over
       the two lengths rather than over the two ratios so that BOTH facts reach the result — a page reads the
       ascent through `1cap` and the sum through `1lh`, and a domain naming only one of them would leave the
       arm where the other reader's face differs unexplored. */
    descent = font_metrics_descent_px(realm, font_size);
    return css_px_add(font_metrics_ascent_px(realm, font_size), descent);
}

CssPx font_metrics_descent_px(JSContext *realm, CssPx font_size)
{
    if (realm == NULL)
        DFAIL("CSS 2.2 §10.8.1 (Leading and half-leading)'s `D` was asked for with no realm — the same "
              "question the ascent above answers, for the same reason: the two metrics are a source key each, "
              "and a key is keyed on the document that read it");
    /* THE SUM'S BOUND IS ASSERTED WHERE THE SUM IS FORMED and not here, because §10.8's recommendation is a
       statement about `normal` and therefore about `AD` — a bound restated over one term would be this file
       deciding how the pair splits, which is exactly the free choice the two picked numbers ARE. What is
       asserted about `D` alone is the one thing §10.8.1's own definition fixes: it is a DEPTH BELOW the
       baseline, so a face whose depth is negative has its baseline outside its own em box and every A' / D'
       split below it comes out on the wrong side of the line. */
    DCHECK(FONT_METRICS_DESCENT_EM >= 0.0,
           "the first available font's picked `D` is NEGATIVE. CSS 2.2 §10.8.1 defines it as \"a depth below\" "
           "the baseline, so a negative one puts the baseline below the bottom of the face and makes every "
           "half-leading split in core/layout/line_box.c place a box above a line it sits below");
    return css_px_mul(font_size, css_px_env(CSS_ENV_FONT_DESCENT, realm, FONT_METRICS_DESCENT_EM));
}
