/* THE FIRST AVAILABLE FONT'S METRICS — css-fonts-4 §5.2 "Matching font styles" defines the face, and
 * css-values-4 §6.1.1 "Font-relative Lengths: the em, rem, ex, rex, cap, rcap, ch, rch, ic, ric, lh, rlh
 * units" defines what is measured off it.
 *
 * WHAT THE FIRST AVAILABLE FONT IS, IN THE SPEC'S OWN WORDS. css-fonts-4 §5.2 closes its font-matching
 * algorithm with the definition: "the first available font, used for example in the definition of
 * font-relative lengths such as ex or in the definition of the line-height property, is defined to be the
 * first font for which the character U+0020 (space) is not excluded by a unicode-range, given the font
 * families in the font-family list (or a USER AGENT'S DEFAULT FONT if none are available)". This user agent
 * has no installed faces and loads none, so every element's first available font is its default font — one
 * face, for every element, in every document.
 *
 * AND THAT FACE HAS NO GLYPH OUTLINES, WHICH IS NOT A SHRUG BUT THE SPEC'S OWN NAMED BRANCH. §6.1.1 states,
 * for three of its metrics, what MUST be assumed when the metric cannot be obtained, and the antecedent it
 * states is literally true here rather than approximately:
 *   `ex`  — "In the cases where it is impossible or impractical to determine the x-height, a value of 0.5em
 *            must be assumed."
 *   `ch`  — "In the cases where it is impossible or impractical to determine the measure of the '0' glyph,
 *            it must be assumed to be 0.5em WIDE BY 1em TALL."
 *   `ic`  — "In the cases where it is impossible or impractical to determine the ideographic advance measure,
 *            it must be assumed to be 1em."
 * Each is a MUST over an antecedent about the user agent's ability to measure, so the three numbers below are
 * the SPEC's and not this component's. That is the whole reason this file exists before a font file does:
 * §6.1.1 already computes a real value for a user agent in exactly this position, and CLAUDE.md §NO STUBS
 * forbids a plausible constant only where the spec computes something else.
 *
 * WHICH IS ALSO WHY THERE IS NO ENVIRONMENT FACT HERE, and that is a decision with a test rather than an
 * omission. core/frame/viewport.h's test is whether the model PICKED one point out of a range the environment
 * leaves free. A real user agent's x-height IS such a point — it is the reader's installed fonts, and a script
 * that measures `1ex` against `fontSize` is reading exactly that — but the number below is not picked at all:
 * §6.1.1 fixes it, so its domain is a single point and a concolic there would model an ignorance this engine
 * does not have. Every length built out of it still carries CSS_ENV_DEFAULT_FONT_SIZE through the `em` it
 * scales, because that number IS picked (core/css/font_size_functions.h). THE DAY A REAL FACE LANDS the ratio
 * stops being the spec's constant and becomes the face's own measurement, and THAT is when it needs a row in
 * core/frame/viewport.c's seam — the ascent crash in core/css/css_length.c is where that decision is made,
 * because a face with an ascent is a face with an x-height.
 *
 * WHY IT IS A COMPONENT AND NOT THREE CONSTANTS IN WHOEVER NEEDED THEM FIRST. Two callers already need this
 * table and one of them held its own copy — the defect css_length.h opens by naming, in the same form
 * core/css/font_size_functions.h was created to fix for `medium`. core/css/css_length.c absolutizes §6.1.1's
 * units for an element, and core/css/media_query.c evaluates a font-relative length in a media query, where
 * §6.1.1 says outright that "when used outside the context of an element (such as in media queries), the
 * font-relative lengths units refer to the metrics corresponding to the initial values of the font and
 * line-height properties" — the same assumed face, reached without an element. Two readers, one table.
 *
 * WHAT IS DELIBERATELY ABSENT, because §6.1.1 states no must-assume value for it. `cap`'s fallback is not a
 * constant at all — "in the cases where it is impossible or impractical to determine the cap-height, THE
 * FONT'S ASCENT must be used" — and an ascent is a metric of a face, so it is the first thing a real font
 * record must carry. `lh` is the computed `line-height` "converting normal to an absolute length by using only
 * the metrics of the first available font", which needs that record AND a computed-value rule for a property
 * core/css/css_computed_value.c does not model. Both crash in core/css/css_length.c naming their own half, and
 * neither borrows a number from the three below — which is the one way this file could come to report a
 * measurement nothing measured. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_FONT_METRICS_H
#define ENGINE_HOST_BROWSER_CORE_CSS_FONT_METRICS_H

/* ONE METRIC OF THE FIRST AVAILABLE FONT, as a MULTIPLE OF THE EM — which is the form §6.1.1 states all three
   in, and the form that makes them independent of any element's computed `font-size`. The caller multiplies.
   THE `ch` PAIR IS ONE ASSUMED GLYPH AND TWO DIMENSIONS, because §6.1.1 states it that way ("0.5em wide by 1em
   tall") and because which of them the unit takes is not a fact about the font: §6.1.1 defines the advance
   measure of a glyph as "its advance width or height, WHICHEVER IS IN THE INLINE AXIS of the element", so the
   axis is the element's writing mode and belongs to the caller that holds the element. Folding the two into
   one number here would put a layout question in the font record and would answer `ch` the same in every
   writing mode. */
typedef enum {
    FONT_METRICS_X_HEIGHT = 0,        /* §6.1.1's `ex`: the assumed x-height */
    FONT_METRICS_ZERO_ADVANCE_WIDTH,  /* §6.1.1's `ch`: the assumed "0" glyph's width */
    FONT_METRICS_ZERO_ADVANCE_HEIGHT, /* §6.1.1's `ch`: the same glyph's height */
    FONT_METRICS_WATER_ADVANCE,       /* §6.1.1's `ic`: the assumed "水" glyph's advance measure */
    FONT_METRICS_EM_RATIO_COUNT
} FontMetricsEmRatio;

double font_metrics_em_ratio(FontMetricsEmRatio which);

#endif
