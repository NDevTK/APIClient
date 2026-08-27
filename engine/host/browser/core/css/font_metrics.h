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
 * AND THAT FACE CARRIES THE TABLES A MEASUREMENT NEEDS AND NO OTHERS, WHICH IS WHAT SPLITS THIS FILE IN THREE.
 * core/fonts/default_font_data.c is a metrics-only sfnt — 'head', 'maxp', 'hhea', 'hmtx', 'cmap' — and every
 * metric below falls into exactly one of three cases, decided by which table it lives in and by which sentence
 * of which spec answers when that table is absent. This is checkable in one command rather than taken on
 * trust: engine/fontsubset.mjs lists the tables the subset keeps, and 'OS/2' is not among them.
 *   AN ADVANCE MEASURE is 'hmtx', which the face carries, so it is MEASURED — for every Unicode scalar value,
 *     .notdef included.
 *   CSS 2.1 §10.8.1's `A` and `D` are 'OS/2'.sTypoAscender/sTypoDescender by that section's own note, which
 *     names 'hhea'.ascender/descender for "the absence of these metrics" — and the face carries 'hhea' and no
 *     'OS/2', so they are MEASURED too, through the note's own second branch. The absence is asserted rather
 *     than remembered; see below.
 *   THE X-HEIGHT is 'OS/2'.sxHeight, for which §10.8.1's note offers no second table and the face offers no
 *     first one, so it is the one metric here that is still §6.1.1's ASSUMPTION.
 * SO §6.1.1's MUST-ASSUME BRANCH IS STILL LIVE, FOR THE X-HEIGHT ALWAYS AND FOR THE TWO ADVANCES WHEN THE FACE
 * CANNOT SUPPLY THE GLYPH. It states, for three of its metrics, what MUST be assumed when the metric cannot be
 * obtained, and the antecedent it states is literally true here rather than approximately:
 *   `ex`  — "In the cases where it is impossible or impractical to determine the x-height, a value of 0.5em
 *            must be assumed."
 *   `ch`  — "In the cases where it is impossible or impractical to determine the measure of the '0' glyph,
 *            it must be assumed to be 0.5em WIDE BY 1em TALL."
 *   `ic`  — "In the cases where it is impossible or impractical to determine the ideographic advance measure,
 *            it must be assumed to be 1em."
 * Each is a MUST over an antecedent about the user agent's ability to measure. THE FIRST OF THE THREE IS STILL
 * THIS ENGINE'S ANSWER and the other two ARE NOT ANY MORE, and the difference is which table the metric lives
 * in: the x-height is a height and the shipped face carries no 'OS/2', while the "0" and "水" advances are
 * 'hmtx' entries the face does carry — so `ch` and `ic` are measured whenever the face has the glyph and take
 * the assumed value only when it does not. `font_metrics_typical_advance_measure_em` below is where that
 * antecedent is decided, per glyph and per direction, by asking the face rather than by assuming once.
 *
 * WHICH IS ALSO WHY THE X-HEIGHT CARRIES NO ENVIRONMENT FACT, and that is a decision with a test rather than
 * an omission. core/frame/viewport.h's test is whether the model PICKED one point out of a range the
 * environment leaves free. A real user agent's x-height IS such a point — it is the reader's installed fonts,
 * and a script that measures `1ex` against `fontSize` is reading exactly that — but the number below is not
 * picked at all: §6.1.1 fixes it, so its domain is a single point and a concolic there would model an
 * ignorance this engine does not have. Every length built out of it still carries CSS_ENV_DEFAULT_FONT_SIZE
 * through the `em` it scales, because that number IS picked (core/css/font_size_functions.h).
 * THE ADVANCE MEASURE IS THE OTHER SIDE OF THAT TEST AND IT IS OPEN, WHICH IS THE NEXT DECISION THIS FILE
 * OWES. It is no longer a spec constant — it is a measurement of a face THIS ENGINE PICKED, so `1ch` against
 * `fontSize` is a two-member ratio a fingerprinting probe reads exactly as it reads `1cap`, and by
 * viewport.h's test it needs a fact. What is NOT yet decided is how many facts a face is: `A` and `D` are two
 * rows below because a page can subtract them, and by that argument every glyph's advance would be a row of
 * its own — which cannot be right, since all of them are read out of ONE table of ONE face and the arm to
 * explore is "another reader has another face" rather than "another reader has this ch and that ic". Settling
 * that is a change to css_length.h's fact vocabulary and to core/frame/viewport.c's seam, and it comes AFTER
 * the face rather than with it.
 *
 * WHERE THE FACE'S BYTES COME FROM. The face is THIS USER AGENT'S OWN — css-fonts-4 §5.2's "or a USER AGENT'S
 * DEFAULT FONT if none are available" — so it is bytes the engine ships, compiled in, and NOT read from any of
 * the three places that are nearer to hand:
 *   THE HOST'S FILESYSTEM does not exist in a wasm instance, and where it does exist (the native host) it is
 *     the machine's font directory — so the two hosts would answer one question two ways and every layout
 *     number would be a property of the box the gate ran on.
 *   THE HOST REALM'S OWN TEXT MEASUREMENT (`canvas.measureText`, `document.fonts`) is reachable from the
 *     renderer and is still wrong twice over: it answers from the READER'S INSTALLED FONTS, which makes a
 *     finding a function of the environment rather than of the document — the one thing the solver
 *     differential requires it not to be — and it does not exist at all in the host every gate links, so the
 *     capability would be untestable by construction. It is also the wrong SIDE of CLAUDE.md §Architecture's
 *     line only in its second half: the metric of a (face, size, codepoint) triple is a pure function and
 *     could be computed once by a trusted zone, which is why "it is read mid-flow" is NOT the argument. What
 *     differs per forked arm is the STRING, never the metric. The argument is determinism and host parity.
 *   THE NETWORK is where a page's OWN `@font-face` bytes come from, and those arrive later through the fetch
 *     chokepoint and are parsed by the SAME parser — attacker-supplied, which is why core/fonts/
 *     open_type_metrics.c is offensive from its first line rather than hardened afterwards. It is not where
 *     the FIRST AVAILABLE FONT of a document that declares no font comes from, and that is the face this file
 *     is about.
 * The bytes are core/fonts/default_font_data.c, GENERATED by engine/fontsubset.mjs, whose header carries the
 * face, its licence, the command that obtains the input, and the justification for each table the subset
 * drops. This file reads them through core/fonts/open_type_metrics.h and holds no format knowledge itself.
 *
 * WHY IT IS A COMPONENT AND NOT THREE CONSTANTS IN WHOEVER NEEDED THEM FIRST. Two callers already need this
 * table and one of them held its own copy — the defect css_length.h opens by naming, in the same form
 * core/css/font_size_functions.h was created to fix for `medium`. core/css/css_length.c absolutizes §6.1.1's
 * units for an element, and core/css/media_query.c evaluates a font-relative length in a media query, where
 * §6.1.1 says outright that "when used outside the context of an element (such as in media queries), the
 * font-relative lengths units refer to the metrics corresponding to the initial values of the font and
 * line-height properties" — the same assumed face, reached without an element. Two readers, one table.
 *
 * AND THE ASCENT IS THE OTHER KIND OF ANSWER, WHICH IS WHY IT HAS A DIFFERENT SIGNATURE. §6.1.1 states no
 * must-assume value for a cap-height — only that an undeterminable one takes "THE FONT'S ASCENT" — and CSS 2.1
 * §10.8.1 "Leading and half-leading" is where that metric is defined: "CSS assumes that every font has font
 * metrics that specify a characteristic height above the baseline and a depth below it. In this section we use
 * A to mean that height (for a given font at a given size) and D the depth." CSS ASSUMES IT AND FIXES NOTHING
 * ABOUT IT: §10.8 says "the height and depth of the font above and below the baseline are assumed to be
 * metrics that are contained in the font", and §10.8.1's own note says which metrics and in what order — "It
 * is recommended that implementations that use OpenType or TrueType fonts use the metrics 'sTypoAscender' and
 * 'sTypoDescender' from the font's OS/2 table for A and D (after scaling to the current element's font size).
 * In the absence of these metrics, the 'Ascent' and 'Descent' metrics from the HHEA table should be used."
 * SO IT IS READ OFF THE FACE, THROUGH THE NOTE'S OWN SECOND BRANCH. The metrics-only sfnt this engine ships
 * carries 'hhea' and no 'OS/2', which is the note's "absence of these metrics" exactly, so `A` and `D` are
 * 'hhea'.ascender and 'hhea'.descender divided by 'head'.unitsPerEm — a measurement, not a chosen point.
 * THE ABSENCE IS ASSERTED AND NOT ASSUMED: core/fonts/open_type_metrics.h records whether the face carries
 * 'OS/2' at all, and reading the fallback pair off a face that has one CRASHES, naming sTypoAscender and
 * sTypoDescender as the thing to build. That is what keeps "the shipped face has no OS/2" a checked fact
 * rather than a remembered one.
 * IT IS STILL A FACT IN core/frame/viewport.h's SENSE, and that test is what decides the signature rather
 * than whether a human typed the number. The model picked one point out of a range the environment leaves
 * free — the point it picked is now the FACE rather than the ratio — and a page reads the result: `width:
 * 1cap` against `getComputedStyle(el).fontSize` is a two-member ratio a fingerprinting probe asks directly,
 * and once CSS 2 §9.4.2's line boxes exist every text box's height asks it again. So `A` and `D` carry
 * CSS_ENV_FONT_ASCENT and CSS_ENV_FONT_DESCENT and return a `CssPx`, where §6.1.1's assumed x-height above
 * returns a bare `double` and carries nothing.
 * THAT ASYMMETRY IS THE CONTRACT AND NOT AN INCONSISTENCY: a value the SPEC fixes has a domain of one point
 * and a concolic over it would model an ignorance this engine does not have, while a value that depends on
 * which face is installed has a real range behind it and a bare number there deletes the arm another reader's
 * font takes.
 *
 * `D` IS THE OTHER HALF OF THE PAIR AND EACH OF ITS TWO READERS ARRIVED BEFORE THE ENTRY IT NEEDED, which is
 * the order this file's absences are meant to be filled in. §10.8.1 defines the pair together — `A`, `D`, and
 * `AD = A + D`, "the distance from the top to the bottom" — and a `D` with nothing reading it would have been
 * the mirror of the defect CLAUDE.md names, real and asserted and consumed by nothing. Its first reader was
 * `font_metrics_normal_line_height_px`, which is CSS 2.1 §10.8's `normal` and css-values-4 §6.1.1's
 * "converting normal to an absolute length by using only the metrics of the first available font" under two
 * names for one number, and which needs only the SUM. Its second is §10.8.1's half-leading, which needs the
 * two APART because §10.8's step 3 takes a maximum over each side of the baseline separately — and that is
 * the reader `font_metrics_descent_px` exists for.
 * THE SIGN IS FLIPPED WHERE THE FACE IS READ AND NOWHERE ELSE. OpenType's 'hhea'.descender is an FWORD
 * measured UP from the baseline, so an ordinary face's is negative, while §10.8.1's `D` is "a depth below" it
 * and is positive. One negation at the read keeps every assert below a statement about the face rather than
 * about a convention.
 * AND `AD` IS WHERE §10.8's OWN RECOMMENDATION IS AN ASSERT: "we recommend a used value for 'normal' between
 * 1.0 to 1.2" is a statement about the SUM, so it could not be checked over `A` alone and is checked over the
 * pair, where the sum is formed. Now that the pair is measured rather than chosen, that assert stopped being
 * a check on two typed numbers and became a check on the SHIPPED FACE — a face whose `AD` leaves the range is
 * one this user agent should not be defaulting to, and it crashes at the point the number would first be
 * reported. §6.1.1's assumed x-height bounds `A` from below in the same way.
 * WHAT IS STILL ABSENT IS THE LINE GAP. §10.8.1's note names sTypoAscender and sTypoDescender for `A` and `D`
 * and names no third term, so `AD` is the whole of what this file needs for `normal`; a face's 'hhea'.lineGap
 * (or `sTypoLineGap`) would be a third metric with no sentence of §10.8 asking for it and nothing here
 * reading it, which is the same reason `D` waited. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_FONT_METRICS_H
#define ENGINE_HOST_BROWSER_CORE_CSS_FONT_METRICS_H

#include <stdint.h>

#include "core/css/css_length.h"
#include "quickjs.h"

/* §6.1.1's ASSUMED X-HEIGHT, as a MULTIPLE OF THE EM — the form the section states it in, and the form that
   makes it independent of any element's computed `font-size`. The caller multiplies. It is a HEIGHT of the
   face and not an advance measure, so it has no direction to select and does not go through the entry below:
   §6.1.1's `ex` is "the used x-height of the first available font", one number in every writing mode. */
double font_metrics_x_height_em(void);

/* WHICH OF A GLYPH'S OWN TWO ADVANCES THE CALLER RESOLVED THE ADVANCE MEASURE TO — OpenType's 'hmtx'
   advanceWidth or its 'vmtx' advanceHeight, which are the two numbers a face carries for one glyph.
   IT IS NOT THE INLINE AXIS, AND CONFLATING THE TWO IS THE MISTAKE THIS ENUM EXISTS TO PREVENT. css-values-4
   §6.1.1 "Font-relative Lengths: the em, rem, ex, rex, cap, rcap, ch, rch, ic, ric, lh, rlh units" defines the
   ADVANCE MEASURE OF A GLYPH as "its advance width or height, whichever is in the inline axis of the element",
   and then notes that it "depends on writing-mode AND TEXT-ORIENTATION as well as font settings,
   text-transform, and any other properties that affect glyph selection or orientation". A glyph set SIDEWAYS
   in a vertical writing mode is rotated a quarter turn, so the distance it advances along the VERTICAL inline
   axis is its HORIZONTAL advance — the inline axis alone therefore does not select the number, and only the
   caller that holds the element's computed `writing-mode` and `text-orientation` can. What crosses this
   interface is the resolved answer. */
typedef enum {
    FONT_METRICS_ADVANCE_HORIZONTAL = 0, /* OpenType 'hmtx' — Horizontal Metrics Table: LongHorMetric.advanceWidth */
    FONT_METRICS_ADVANCE_VERTICAL        /* OpenType 'vmtx' — Vertical Metrics Table: advanceHeight */
} FontMetricsAdvanceDirection;

/* THE ADVANCE MEASURE OF ONE GLYPH of the first available font, for ANY Unicode scalar value, as a MULTIPLE OF
   THE EM. The caller multiplies, exactly as it does for the x-height above — and that form is the face's own,
   not a stand-in for it: OpenType states every advance in FONT DESIGN UNITS and 'head' — Font Header Table's
   `unitsPerEm` is what the em is worth in them, so the division is the face's own arithmetic.
   IT MEASURES THE GLYPH THAT GETS DRAWN, INCLUDING WHEN THAT GLYPH IS .notdef. A codepoint the face does not
   cover maps to glyph 0 — OpenType 'cmap' — Character to Glyph Index Mapping Table reserves it ("character
   codes that do not correspond to any glyph in the font should be mapped to glyph index 0 ... commonly known
   as .notdef") — and a user agent with no other face draws exactly that, which css-fonts-4 §5.2 "Matching font
   styles" states outright: "If a particular character cannot be displayed using any font, the user agent
   should indicate by some means that a character is not being displayed, displaying either a symbolic
   representation of the missing glyph (e.g. using a Last Resort Font) or using the missing character glyph
   from a default font". So .notdef's own advance is the right answer and a zero would be a wrong one.
   THE CALLER THAT WANTS §6.1.1's `ch` OR `ic` WANTS THE ENTRY BELOW INSTEAD, and the difference is the whole
   reason there are two. This one answers "how far does this run advance", which is what core/layout/line_box.h
   needs per typographic character unit; the one below answers "what is the `ch` unit worth", which §6.1.1
   FIXES when the face cannot supply it. Answering either with the other is a real wrong number in a direction
   nothing can see: a tofu box reported as the width of a digit, or every uncovered character in a paragraph
   reported as exactly one em wide.
   A VERTICAL advance of a face with no 'vhea'/'vmtx' CRASHES here rather than substituting the horizontal one,
   naming css-writing-modes-4 §5.1.1 "Vertical Typesetting and Font Features"'s synthesis as the work. */
double font_metrics_advance_measure_em(uint32_t codepoint, FontMetricsAdvanceDirection direction);

/* css-values-4 §6.1.1's `ch` AND `ic` — the section's own "TYPICAL advance measure" of the "0" (ZERO, U+0030)
   glyph and of the "水" (CJK water ideograph, U+6C34) glyph, "in the font used to render it".
   THIS IS ONE QUESTION WITH TWO UNITS, AND IT WAS TWO NAMES BEFORE IT WAS AN ENTRY: one capability, two
   codepoints, and the tell that they had come apart was that the `ch` caller resolved the glyph's orientation
   and the `ic` caller did not — so one of the two answered a question it had never asked, and was right only
   because the value §6.1.1 assumes for `水` happens to be the same in both directions.
   IT DIFFERS FROM THE ENTRY ABOVE ONLY WHERE THE FACE CANNOT ANSWER, which is exactly the antecedent §6.1.1
   states its assumed values over — "in the cases where it is impossible or impractical to determine the
   measure of the '0' glyph, it must be assumed to be 0.5em wide by 1em tall", and "in the cases where it is
   impossible or impractical to determine the ideographic advance measure, it must be assumed to be 1em". Two
   things make that antecedent true and the face is asked about both: it may have no glyph for the codepoint,
   and it may have no metrics for the axis. Asking for any OTHER codepoint crashes, because §6.1.1 assumes a
   value for these two and for no others. */
double font_metrics_typical_advance_measure_em(uint32_t codepoint, FontMetricsAdvanceDirection direction);

/* CSS 2.1 §10.8.1's `A` — the first available font's "characteristic height above the baseline" — for a face
   rendered at `font_size`, in CSS pixels. It is the used value css-values-4 §6.1.1's `cap` unit falls back to.
   IT TAKES THE SIZE RATHER THAN ANSWERING A RATIO because the ascent is a METRIC and not a multiplier: a
   caller holding the ratio would have to know to multiply, and the two facts — the reader's font size and this
   user agent's face — would then be unioned by whichever caller remembered to. The product is formed here, so
   the returned length is a joint function of both and css_length.h's arithmetic carries the pair.
   `realm` IS THE ELEMENT'S DOCUMENT'S, never the running one, for the reason every other picked fact takes one
   (core/frame/viewport.h): the source key is keyed on the document that read it. A document no navigable
   presents CRASHES rather than being handed a bare number, exactly as core/css/font_size_functions.h does. */
CssPx font_metrics_ascent_px(JSContext *realm, CssPx font_size);

/* CSS 2.1 §10.8.1's `AD = A + D` — the first available font's height above the baseline plus its depth below,
   at `font_size`, in CSS pixels. It is CSS 2.1 §10.8's used value for `line-height: normal` ("tells user
   agents to set the used value to a 'reasonable' value BASED ON THE FONT of the element"), which css-inline-3
   §5.1 states as "determine the preferred line height automatically based on the metrics of the used font",
   and it is css-values-4 §6.1.1's `lh` conversion ("converting normal to an absolute length by using only the
   metrics of the first available font"). Three sentences, one number, one entry — so a page cannot read two
   different answers for `line-height: normal` through `getComputedStyle` and through `1lh`. */
CssPx font_metrics_normal_line_height_px(JSContext *realm, CssPx font_size);

/* CSS 2.2 §10.8.1 "Leading and half-leading"'s `D` — the first available font's "depth below the baseline" —
   at `font_size`, in CSS pixels. IT IS THE HALF OF THE PAIR §10.8.1 NEEDS APART, and its reader is the reason
   it exists: "half the leading is added above A and the other half below D, giving the glyph and its leading a
   total height above the baseline of A' = A + L/2 and a total depth of D' = D + L/2", and §10.8's step 3
   takes the line box height as "the distance between the uppermost box top and the lowermost box bottom" —
   which is max(A') plus max(D') over the boxes on the line, two maxima that cannot be taken over a sum. So
   `AD` above answers `line-height: normal` and this answers where the BASELINE sits inside it; a caller that
   subtracted one from the other would be re-deriving a picked number through arithmetic instead of reading
   the face. core/layout/line_box.h is that caller. */
CssPx font_metrics_descent_px(JSContext *realm, CssPx font_size);

#endif
