/* CSS VALUES AND UNITS §6 — the `<length>`, and the CSS PIXEL every geometric answer in this engine is
 * measured in.
 *
 * WHY IT IS A COMPONENT AND NOT A HELPER INSIDE WHOEVER NEEDED IT FIRST. Four standards already turn a CSS
 * length into a number here, and each one that carries its own copy is one more place the unit table can
 * disagree: the COMPUTED value of every box-model property is defined as "the percentage as specified or THE
 * ABSOLUTE LENGTH" (CSS 2.1 §8.3, §8.4, §10.2, §10.5), CSS 2.1 §10's used values are arithmetic over those
 * absolute lengths, CSSOM §6.7.2 serializes one back out, and Media Queries §4 compares one against the
 * viewport (core/css/media_query.c already had its own, and says why its font-relative answer differs — see
 * below). One fact answered from four places is the defect CLAUDE.md §per-realm names.
 *
 * THE ABSOLUTE UNITS ARE ARITHMETIC AND ARE COMPUTED. CSS Values §6.2 anchors all six of them to the CSS pixel
 * — 1in = 96px by definition, and cm, mm, Q, pt and pc are exact fractions of the inch — so there is no device
 * to consult and nothing to model. §Headless is not the reason any of these would be missing.
 *
 * THE RELATIVE UNITS ARE ABSOLUTIZED HERE, AT COMPUTED-VALUE TIME, WHICH IS WHY THE PARSE TAKES A REALM. CSS
 * 2.1 §4.3.2 makes a relative length's computed value the absolute length it resolves to, so the resolution
 * happens once, before any used value is asked for — and each family resolves against a different thing:
 *
 *   A FONT-RELATIVE unit is TWELVE units and TWO DIFFERENT QUESTIONS, which css-values-4 §6.1.1's own section
 *   title enumerates and its own definitions split: "Font-relative Lengths: the em, rem, ex, rex, cap, rcap,
 *   ch, rch, ic, ric, lh, rlh units". `em` is "the computed value of the font-size property of the element on
 *   which it is used" and `rem` is "the computed value of the em unit on the root element" — a COMPUTED
 *   `font-size` and nothing else. The other ten are FONT METRICS: an x-height, a cap-height, the advance
 *   measure of the "0" and "水" glyphs, and a computed `line-height` with `normal` resolved from the first
 *   available font — each with an `r`-prefixed twin measured on the root element. THE TWO GROUPS ASK DIFFERENT
 *   QUESTIONS AND TAKE ONE TABLE, because every one of the twelve is a base times a multiplier and only the
 *   two differ; what parts company is where the multiplier comes FROM, which is the split below.
 *   THE FIRST TWO RESOLVE. css_computed_value.c derives css-fonts-4 §2.5's computed `font-size` — an absolute
 *   length, out of §2.5.1's keyword table, a percentage of the parent's, `larger`/`smaller`, or a length this
 *   file absolutizes — and CSS Cascade §7.2's inheritance carries it one node at a time to a base case that is
 *   core/css/font_size_functions.h's picked default. WHICH font-size is the WALK, so this file does not
 *   perform it: `CssFontMetrics` below is the pair of questions and the caller holding the tree answers them,
 *   lazily, so a `margin: 0` neither walks to the root nor fails for want of a font it does not use. The one
 *   case that is easy to get wrong is stated in §6.1.1 in so many words — inside a FONT-AFFECTING property on
 *   the element the unit refers to, "the font-relative lengths resolve against the computed metrics of the
 *   PARENT element—or against the computed metrics corresponding to the initial values of the font and
 *   line-height properties, if the element has no parent" — so `div{font-size:1.2em}` is 1.2 x the INHERITED
 *   size and `html{font-size:2rem}` is 2 x the INITIAL one rather than a definition of itself, and that is a
 *   property of the ANSWERS the caller supplies rather than of the arithmetic here.
 *   SIX MORE RESOLVE OUT OF THOSE TWO, and the number they are multiplied by is SPLIT BETWEEN THE SPEC AND
 *   THE FACE — which is a distinction with a table behind it and not a shading. `ex` is a HEIGHT of the first
 *   available font, and the metrics-only sfnt this engine ships carries no 'OS/2', so §6.1.1's "In the cases
 *   where it is impossible or impractical to determine the x-height, a value of 0.5em must be assumed" is
 *   still the answer. `ch` and `ic` are ADVANCE MEASURES, and 'hmtx' is one of the tables the face DOES carry,
 *   so they are measured whenever the face has the glyph and take §6.1.1's assumed value only when it does not
 *   (this user agent's face has "0" and not "水", so today `ch` is a measurement and `ic` is the assumption).
 *   core/css/font_metrics.h owns all three and says which is which. Each `r`-prefixed twin is "the value of
 *   the <unit> unit ON THE ROOT ELEMENT", so it is the same ratio over the `rem` base.
 *   THE LAST FOUR RESOLVE OUT OF THE PICKED FACE'S OWN TABLES, and that is the OTHER kind of multiplier:
 *   §6.1.1 states no must-assume value for a cap-height or for `normal`, so `cap`/`rcap` takes CSS 2.1
 *   §10.8.1's `A` and `lh`/`rlh` takes its `AD`, which core/css/font_metrics.h reads off 'hhea' — §10.8.1's
 *   note's own source in the absence of an 'OS/2'. What the user agent picked is the FACE, not the ratio, so
 *   those four carry an environment fact, and the whole of the difference is which sentence of the spec
 *   answers.
 *   OF THE SIX ABOVE, ONLY `em`/`rem` AND `ex`/`rex` CARRY NONE OF THEIR OWN TODAY, and `ch`/`rch` and
 *   `ic`/`ric` carrying none is an OPEN DECISION rather than a settled one — their multiplier stopped being a
 *   spec constant the day the face landed, so by core/frame/viewport.h's test they need a fact exactly as
 *   `cap` does. What is not settled is how many facts a face is; core/css/font_metrics.h states the question
 *   and why it could not be answered before the face existed.
 *   `lh` is also the one unit whose base is a PROPERTY: §6.1.1 gives it and `rlh` a resolution
 *   rule of their own inside `line-height` (core/css/font_size_functions.h's second predicate), which the
 *   caller answers exactly as it answers the font-affecting one.
 *   media_query.c resolves `em` against the INITIAL font size and is right to, and §6.1.1 says so itself —
 *   "when used outside the context of an element (such as in media queries), the font-relative lengths units
 *   refer to the metrics corresponding to the initial values of the font and line-height properties" — so the
 *   initial value is the spec's own answer THERE rather than a stand-in for a walk. It reads that number from
 *   font_size_functions.h now, because ONE fact answered from two places is this header's opening complaint.
 *
 *   A VIEWPORT-PERCENTAGE unit RESOLVES, against the rectangle core/frame/viewport.h models. §6.1.2's own first
 *   sentence is the whole rule — "the viewport-percentage lengths are relative to the size of the INITIAL
 *   CONTAINING BLOCK" — so `50vw` is half of `viewport_icb_width` and inherits the ICB's environment fact,
 *   which is the entire reason the computed-value path had to stop being text. §6.1.2.1 adds one divergence
 *   (where the root element's `overflow` forces scrollbars unconditionally the units follow the reduced ICB,
 *   and otherwise they assume scrollbars do not exist EVEN IF that diverges from the ICB) and this user agent
 *   renders no scroll bar, so the two rectangles are one — asserted at the resolution rather than assumed, so
 *   the day a scroll bar reduces the ICB the crash stands where `vw` would have to stop following it.
 *   `vi`/`vb` still crash: they are stated in the BOX'S INLINE AXIS, which is `writing-mode` and `direction`,
 *   both inherited. So does every `sv*`/`lv*`/`dv*` variant, and NOT because their number would differ here —
 *   §6.1.2.1's three viewport sizes are separate FACTS the moment a UA interface expands and retracts, and
 *   answering all four families out of one source key would decide `100dvh === 100lvh` on the example and
 *   delete the arm a mobile bundle wrote the comparison for.
 *
 * AND `calc()` IS A THIRD, ANSWERED BY core/css/css_math.h OVER THE SAME TABLE. css-values-4 §10 makes a math
 * function a value in its own right whose result depends on every unit above, and it arrives here as a string
 * that is neither a dimension nor a keyword. The GRAMMAR and the §10.9 type algebra are that component's; the
 * UNITS stay this one's, and `calc(100vw - 2em)` reaches exactly the arms above through the resolver callback
 * §10.10.1's canonical-unit step is asked through — so a `dvh` inside a math function crashes with the same
 * message, in the same place, as a `dvh` written on its own.
 *
 * A LENGTH IN CSS PIXELS IS NOT ALWAYS A NUMBER, WHICH IS WHY `CssPx` BELONGS HERE AND NOT IN WHOEVER COMPUTES
 * ONE. CSS 2.1 §10.1 makes the ROOT ELEMENT's containing block the INITIAL CONTAINING BLOCK, "it has the
 * dimensions of the viewport", and core/frame/viewport.h states what the viewport is: a PICKED environment
 * fact, carried to a page as the EXAMPLE of a concolic, because `innerWidth < 768` is a responsive bundle's
 * mobile gate and a bare 1280 deletes the whole arm behind it. Every computed and used value derived from that
 * width inherits the same domain — `parseInt(getComputedStyle(el).width) < 768` is the SAME gate, asked through
 * a different member — so a length that reaches a page either carries the fact it came from or the fork is lost
 * between the two.
 *
 * IT CARRIES THE FACT AND NOT THE CONCOLIC, which is media_query.h's layering rather than a weaker form of it.
 * The C side answers the EXAMPLE — `viewport_width` is a `double`, Media Queries §4 evaluates
 * `(max-width: 768px)` against the modelled viewport in C, and the concolic is minted at the JS boundary,
 * where the PAGE's own branch is what forks. So every sum and every comparison below runs on `px`, the
 * modelled number, and `env` is what the boundary needs in order to mint the domain the page then forks on.
 * Threading a JSValue through a layout instead would put a concolic exactly where C compares, which is the one
 * thing viewport.h forbids: a C `if` over a concolic silently picks one arm and says nothing about the other.
 *
 * AND A LENGTH IS A FUNCTION OF A SET OF FACTS, WHICH IS ONE DOMAIN AND NOT TWO VALUES. A `width: auto` box
 * with a real border is a function of the initial containing block AND of the device pixel ratio css-values §6
 * snapped that border to; `100vmin` is a function of both viewport axes in one token; a `50vh` inside a
 * percentage-margined box is a function of both axes through the arithmetic. So `env` is a SET, and a sum, a
 * difference and the larger of two lengths carry the UNION of their operands' — solver/concolic.h's JOINT
 * source identity is what that set becomes at the boundary, and it is ONE concolic with ONE identity, never a
 * second value beside the first.
 * IT IS A SET AND NOT AN ORDERED PAIR, for the reason every canonical key exists: `cb - margins - borders` and
 * `cb - borders - margins` are the same dependence, and two spellings of one identity fork the same predicate
 * twice. IT IS A SET AND NOT AN EXPRESSION over the facts, because an expression is the recorded transform
 * CLAUDE.md's §Re-execution forbids — what rides the value here is PROVENANCE (which facts it is a function of),
 * never the operation (how), and the EXAMPLE is right because the arithmetic below actually runs on the modelled
 * numbers. AND IT IS A SET RATHER THAN A PAIR: three facts already meet in one length (a `100vmin` width on a
 * box whose border was snapped), so an arity is one more thing to be wrong about.
 * THE SET IS OVER-REPORTED WHERE THE ARITHMETIC CANCELS — `a - a` over one fact still carries it — which is the
 * direction css_px_max chooses and for its reason: over-reporting the dependence forks a world the page might
 * not have needed, under-reporting it deletes one it did, and CLAUDE.md's §Headless errs toward the first. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_LENGTH_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_LENGTH_H
#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"

/* ONE ENVIRONMENT FACT. Every entry is a fact core/frame/viewport.h has already decided is PICKED rather than
   DERIVED — the test is that component's, not this one's, and a fact that fails it (a scroll position, a screen
   coordinate) must never appear here. A fact is an INDEX: it names one row of the table in that component's
   seam, and it is one BIT of the set below. */
typedef enum {
    CSS_ENV_ICB_WIDTH = 0, /* CSS 2.1 §10.1's initial containing block, whose dimensions are the viewport's */
    CSS_ENV_ICB_HEIGHT,
    /* CSSOM VIEW §4's `devicePixelRatio`, which css-values §6's SNAP A LENGTH AS A LINE WIDTH divides a
       border width by. `devicePixelRatio > 1` is the retina gate a bundle puts a second image host behind, and
       it reaches a length as well as a member: a `border: 1px solid` is one device pixel at every ratio, so it
       is 1 CSS pixel at 1x and two thirds of one at 1.5x. */
    CSS_ENV_DEVICE_PIXEL_RATIO,
    /* css-fonts-4 §2.5 (Font size: the font-size property)'s `Initial: medium` — the number this user agent
       picks for `medium`, which §2.5.1's table makes the reference every `<absolute-size>` keyword is a ratio
       of and CSS Cascade §7.2 makes the root element's inherited value. core/css/font_size_functions.h owns
       the number and says why it passes core/frame/viewport.h's PICKED-rather-than-DERIVED test: nothing in
       this model determines it, the spec calls it the reader's own preference, and a `rem`-sized layout puts
       its entire responsive ladder behind it. */
    CSS_ENV_DEFAULT_FONT_SIZE,
    /* CSS 2.1 §10.8.1 (Leading and half-leading)'s `A` — "a characteristic height above the baseline" of the
       first available font, which core/css/font_metrics.h picks and says why. It is a SEPARATE fact from the
       default font size beside it and not a refinement of it: the size is the reader's preference and the
       ascent is the installed FACE, so a page that measures `1cap` against `fontSize` is reading the ratio
       between them and would get one arm out of a single key. It is also separate from §6.1.1's assumed
       x-height, which is a spec constant with no freedom in it and therefore no fact at all. */
    CSS_ENV_FONT_ASCENT,
    /* CSS 2.1 §10.8.1's `D` — "a depth below [the baseline]" of the same face, read by the same component
       and a SEPARATE fact from `A` beside it for the same test: a page reads `1cap` for the ascent and `1lh`
       for their sum, so their difference is independently observable and one key for both would decide it on
       the example. §10.8.1 defines the pair together and this engine picks them together; that is why they
       are two rows and not one, not a reason to make them one. */
    CSS_ENV_FONT_DESCENT,
    CSS_ENV_FACT_COUNT
} CssEnvFact;

/* THE SET OF FACTS A LENGTH IS A JOINT FUNCTION OF — see the header for why a set and not a pair, an order or
   an expression. `CSS_ENV_NONE` is the EMPTY set, and it is a POSITIVE statement rather than an absence: the
   length is one this cascade and this layout determined out of the author's own declarations, so its domain is
   a single point and there is no arm to explore. */
typedef unsigned CssEnvSet;
#define CSS_ENV_NONE     ((CssEnvSet)0)
#define CSS_ENV_BIT(f)   (((CssEnvSet)1u) << (f))
#define CSS_ENV_ALL      ((CssEnvSet)((1u << CSS_ENV_FACT_COUNT) - 1u))

typedef struct {
    double      px;      /* THE EXAMPLE — the modelled number, always present, and what C compares */
    CssEnvSet   env;
    JSContext  *realm;   /* the realm whose environment `env` names; NULL exactly when `env` is CSS_ENV_NONE */
} CssPx;

/* A length the cascade and the layout determined. */
CssPx css_px(double px);
/* A length derived from ONE fact in `realm`, carrying `px` as its example — the only way a fact enters a
   length, so the set below is only ever assembled by the arithmetic. */
CssPx css_px_env(CssEnvFact fact, JSContext *realm, double px);

/* ARITHMETIC OVER THE EXAMPLE, PROPAGATING THE SET. The sum of a determined length and a derived one is
   derived; the sum of two lengths derived from the SAME fact is derived from that one fact (a percentage margin
   and a percentage padding both resolve against the containing block's width, and their sum is still one
   function of it); and the sum of two lengths derived from DIFFERENT facts is a function of BOTH, which is the
   union and is what the boundary mints one joint domain from. */
CssPx css_px_add(CssPx a, CssPx b);
CssPx css_px_sub(CssPx a, CssPx b);
/* `k` is a pure ratio — a percentage divided by 100 — so it changes no fact. */
CssPx css_px_scale(CssPx a, double k);
/* THE LARGER AND THE SMALLER EXAMPLE OF TWO LENGTHS, each carrying BOTH their facts. Three spec algorithms need
   one of them and every one is stated as a comparison against a length that may itself be viewport-derived:
   css-sizing §5's floor of a content box at zero, CSS 2.1 §10.4 step 3's clamp of a tentative used width by
   `min-width`, and css-values §6.1.2.2's `vmin`/`vmax`, which are the smaller and larger of `vw` and `vh`.
   Deciding WHICH is larger on the modelled viewport is media_query.h's layering — §4 answers
   `(max-width: 768px)` against the same modelled number — and the result carries the UNION of the two operands'
   facts, because the operand that lost at this viewport is the one that wins at another. That is why `100vmin`
   is a function of both viewport axes and not of whichever one is smaller at 1280 x 720. */
CssPx css_px_max(CssPx a, CssPx b);
CssPx css_px_min(CssPx a, CssPx b);
/* A PRODUCT AND A QUOTIENT OF TWO OPERANDS, each carrying BOTH their facts. What a `CssPx` carries is a NUMBER
   and the environment provenance of that number, and the two operations that leave the dimension of a length
   are exactly the two Intersection Observer §3.2.10 states: its steps 9 and 10 take a rectangle's AREA (a
   product of two extents) and its step 12 takes a RATIO of two areas. Both stay in this vocabulary rather than
   collapsing to a bare double, because a bundle writes `if (entry.intersectionRatio > 0.5)` and a ratio derived
   from the initial containing block is the same environment question `innerWidth < 768` is — dropping the set
   at the multiplication would delete the arm the other viewport takes, exactly as `css_px_max` returning the
   winner whole would.
   THE QUOTIENT'S DIVISOR IS NEVER ZERO AT ANY CALLER, which is the divisor's own algorithm asserting it rather
   than this one guessing: §3.2.10 step 12 divides only in the branch where `targetArea` is non-zero, and its
   other branch answers 1 or 0 without dividing. Asserted here so a future caller cannot arrive without having
   made that branch. */
CssPx css_px_mul(CssPx a, CssPx b);
CssPx css_px_div(CssPx a, CssPx b);

/* CSS Values §6.2's ABSOLUTE LENGTH UNITS, as the ONE question a caller outside the computed-value chain has to
   ask: is `unit` one of them, and what is `n` of them in CSS pixels? Intersection Observer §2.2's "parse a
   margin" is that caller — its step 5.1 is "if token is an ABSOLUTE LENGTH dimension token, replace it with an
   equivalent pixel length", and its step 5.3 is "otherwise, return failure", so a `2em` there is a SyntaxError
   rather than the crash `css_length_parse` owes a font-relative unit it cannot absolutize. That is a different
   question from the one `css_length_parse` answers and it must not be asked by copying the table: §6.2's seven
   ratios are one fact, and a second copy is the second answer that is always subtly wrong.
   `unit` is the dimension token's own unit text and need not be NUL-terminated or lowercased; `unit_len` is its
   byte length. Answers false for every other unit, writing nothing. */
bool css_length_absolute_px(const char *unit, size_t unit_len, double n, double *px);

/* IS THIS A `<length>` UNIT AT ALL — §6's whole set (§6.2's seven absolute units, §6.1.1's font-relative ones
   and §6.1.2.2's viewport-percentage ones in all four of §6.1.2.1's viewport families) PLUS CSS Conditional 5
   §7 Container Relative Lengths: the cqw, cqh, cqi, cqb, cqmin, cqmax units' six. ALL of them, because §6's
   production admits every unit a specification defines as a `<length>` and the ones this engine cannot
   absolutize are a MISSING COMPONENT (the crash `css_length_parse` owes them) rather than a syntax error — a
   grammar that answered false for `2em` would report an unbuilt capability as an author's mistake.
   IT IS A DIFFERENT QUESTION FROM `css_length_is_length` AND IS THE HALF THAT SET ANSWERS: that one is handed
   a whole serialized VALUE and decides §6's production over it, and it is written in terms of this. A caller
   holding a DIMENSION TOKEN already has the unit split off and must not have to reassemble the value to ask —
   CSS Properties and Values API 1 §5.1's `<length>` syntax component is exactly that caller. The unit is the
   token's own text, neither NUL-terminated nor lowercased. */
bool css_length_is_length_unit(const char *unit, size_t unit_len);

typedef enum {
    CSS_LENGTH_ABSOLUTE = 0,   /* an absolute `<length>`; `px` carries it in CSS pixels */
    CSS_LENGTH_PERCENTAGE,     /* a `<percentage>`; `pct` carries the number, with the `%` removed */
    /* A LENGTH AND A PERCENTAGE TOGETHER — `width: calc(100% - 2rem)`, where `px` AND `pct` are BOTH the value.
       css-values-4 §10.11 "Computed Value" is why this is a KIND and not a resolution that should already have
       happened: "Where percentages are not resolved at computed-value time, they are not resolved in math
       functions, e.g. calc(100% - 100% + 1px) resolves to calc(0% + 1px), not to 1px." So the computed value
       of one of these IS the pair, and §5.6 "Mixing Percentages and Dimensions" is the production that admits
       it — "width: calc(500px + 50%); is allowed — both values are converted to absolute lengths and added"
       — at USED-value time, against a basis this component does not have and every caller does.
       IT IS NOT THE SAME FACT AS `pct != 0`, which is the whole reason css_math.h carries `pct_term`: §10.10.1
       "Simplification" states that "zero-valued terms cannot be simply removed from a Sum", so `calc(100% -
       100% + 1px)` is this kind with `pct` 0 and NOT an absolute `1px`. A producer that tested the number
       instead would collapse the two, and §10.11's own worked example is that collapse being wrong.
       WHAT IT DOES NOT RECORD is whether the surviving Sum HAS a dimension term — `calc(50%)` and
       `calc(50% + 0px)` reach here identically, because `CssMathValue` has a `pct_term` and no `num_term`.
       That is unobservable in every USED value (the two resolve to the same number against every basis) and
       observable in exactly one place, §10.13 "Serialization", which crashes there naming it. */
    CSS_LENGTH_CALCULATED,
    CSS_LENGTH_KEYWORD         /* not a length at all: `auto`, `none`, `min-content`, `normal`, … */
} CssLengthKind;

/* THE KEYWORD IS CARRIED AS TEXT AND NOT AS AN ENUM, deliberately: every caller that cares compares it against
   the one or two spellings ITS property's grammar admits (`auto` for a margin, `none` for a `max-width`), and
   an enum of every keyword every length-valued property admits would be a second copy of lexbor's own grammar.
   It is carried BY VALUE so a `CssLength` needs no free, and the buffer is asserted rather than truncated: the
   longest keyword any length-valued property's grammar admits is css-sizing's `-webkit-fill-available`, and a
   longer one is a value lexbor validated against a grammar this engine does not know it has. */
#define CSS_LENGTH_KEYWORD_MAX 32

typedef struct {
    CssLengthKind kind;
    CssPx         px;                             /* CSS_LENGTH_ABSOLUTE and CSS_LENGTH_CALCULATED */
    double        pct;                            /* CSS_LENGTH_PERCENTAGE and CSS_LENGTH_CALCULATED */
    char          keyword[CSS_LENGTH_KEYWORD_MAX];/* CSS_LENGTH_KEYWORD only */
} CssLength;

/* WHICH FONT SIZE A §6.1.1 FONT-RELATIVE UNIT RESOLVES AGAINST, AS A QUESTION THIS COMPONENT ASKS RATHER THAN
   ANSWERS. `em` is "the computed value of the font-size property of the element on which it is used" and `rem`
   is "the computed value of the em unit on the root element", and BOTH of those are CSS Cascade §7.2's
   inheritance walk — a walk over the flattened element tree, which is core/css/css_computed_value.c's and not
   this file's. Threading an element and a tree through here to answer it would put the cascade inside the unit
   table; handing the two ANSWERS in would be worse still, for the reason below. */
/* AND THE SAME SPLIT COVERS §6.1.1's FONT-METRIC UNITS, which is why they are rows of this enumeration rather
   than a second seam. Each of `ex`, `ch` and `ic` is core/css/font_metrics.h's ratio — measured off the face
   for the two advances, §6.1.1's assumption for the x-height — times one of the two font sizes above: the
   LOCAL units against the element's, the `r`-prefixed twins against the root's,
   because §6.1.1 defines each twin as "the value of the <unit> unit ON THE ROOT ELEMENT" and so carries the
   whole computation there, including the orientation question. So the caller answering this callback answers
   one question it already answers (which element's computed `font-size`) and one it alone can — WHICH OF THE
   GLYPH'S TWO ADVANCES is its advance measure on that element, which `ch` and `ic` both ask because §6.1.1
   defines both of them over the advance measure and defines that as "its advance width or height, whichever
   is in the inline axis of the element". It is not the inline axis alone that decides: css-writing-modes-4
   §5.1.1 "Vertical Typesetting and Font Features" typesets a sideways character "using HORIZONTAL METRICS"
   even though its inline axis is vertical, so the answer is the resolved ORIENTATION and only the layer
   holding the element's computed `writing-mode` and `text-orientation` has it. This file's unit table stays a
   table.
   `cap` IS HERE ON THE SAME ARITHMETIC BUT NOT ON THE SAME FOOTING: §6.1.1 states no must-assume value for a
   cap-height, only that an undeterminable one takes "the font's ascent", and CSS 2.2 §10.8.1's note says to
   read that ascent off the face — so it carries CSS_ENV_FONT_ASCENT. Of the three above, `ex` carries nothing
   of its own because §6.1.1 FIXES it; `ch` and `ic` carry nothing of their own only because the fact vocabulary
   has not yet been extended to the face's advances, which core/css/font_metrics.h states as an open decision.
   The row is the same shape because the multiplication is.
   `lh` IS THE TWELFTH AND IT IS A PROPERTY RATHER THAN A METRIC, which is why it is the last row and why it
   changes nothing about this table's shape: §6.1.1 makes it "the computed value of the line-height property of
   the element on which it is used, converting normal to an absolute length by using only the metrics of the
   first available font", so the caller answers a computed `line-height` (core/css/css_computed_value.h) with
   §10.8.1's `AD` substituted for `normal` (core/css/font_metrics.h), and this file multiplies as it does for
   every other row. WHICH element's line-height is the caller's second question and §6.1.1 gives these two
   units a rule of their own for it — core/css/font_size_functions.h's second predicate — which is the same
   shape as the font-affecting one and lands in the same place, so nothing here has to know. */
typedef enum {
    CSS_FONT_METRIC_EM = 0,   /* §6.1.1's `em`  — the element the unit is used on */
    CSS_FONT_METRIC_REM,      /* §6.1.1's `rem` — the root element */
    CSS_FONT_METRIC_EX,       /* §6.1.1's `ex`  — the used x-height */
    CSS_FONT_METRIC_REX,      /* §6.1.1's `rex` */
    CSS_FONT_METRIC_CH,       /* §6.1.1's `ch`  — the advance measure of the "0" glyph */
    CSS_FONT_METRIC_RCH,      /* §6.1.1's `rch` */
    CSS_FONT_METRIC_IC,       /* §6.1.1's `ic`  — the advance measure of the "水" glyph */
    CSS_FONT_METRIC_RIC,      /* §6.1.1's `ric` */
    CSS_FONT_METRIC_CAP,      /* §6.1.1's `cap` — the used cap-height, which falls back to the font's ASCENT */
    CSS_FONT_METRIC_RCAP,     /* §6.1.1's `rcap` */
    CSS_FONT_METRIC_LH,       /* §6.1.1's `lh`  — the computed `line-height`, with `normal` converted */
    CSS_FONT_METRIC_RLH       /* §6.1.1's `rlh` */
} CssFontMetric;

/* IT IS ASKED LAZILY, AND THAT IS THE POINT OF THE INDIRECTION RATHER THAN AN OPTIMISATION. Resolving both
   sides EAGERLY for every length would walk to the root of the tree to absolutize `margin: 0` — and, worse,
   would CRASH it: the base case of both chains is the default font size, which needs a realm to key its
   environment fact on (core/css/font_size_functions.h), and a `margin: 0` in a document no navigable presents
   has an answer CSS computes with no font and no viewport at all. A value that needs no font must not pay for
   one, and must not fail for want of one. So the pair is a CALLBACK invoked at most once, only on the arm that
   found the unit. */
typedef struct {
    CssPx (*resolve)(void *ctx, CssFontMetric which);
    void  *ctx;
} CssFontMetrics;

/* CLASSIFY AND ABSOLUTIZE a length-valued property's value, as the cascade serialized it — CSS 2.1 §4.3.2's
   computed value for a `<length>`, which is why this is the step core/css/css_computed_value.c performs and
   the only caller it has. A dimension in a unit this engine cannot absolutize CRASHES here rather than being
   reported as a fourth kind — see the header: each group names a DIFFERENT missing component, and a caller
   handed "unresolvable" could only guess which.
   `realm` IS THE REALM THE VIEWPORT IS ANSWERED PER — the ELEMENT's document's active realm, never the running
   one, because an iframe's ICB is 300 CSS pixels wide and its parent's is 1280. It may be NULL for a document
   no navigable presents; only a viewport-percentage unit needs it, and that is the arm that crashes.
   `font` ANSWERS §6.1.1's TWO FONT-SIZE-RELATIVE UNITS and is required — every caller here holds an element,
   and the one question this file cannot answer for itself is which element's computed `font-size` a unit means
   (see above). It is consulted only when an `em` or a `rem` is actually present.
   A UNITLESS ZERO is a `<length>` (CSS Values §6 permits the unit to be omitted for zero, and it is how
   lexbor serializes every box-model property's initial value). */
CssLength css_length_parse(JSContext *realm, const CssFontMetrics *font, const char *value);

/* IS THIS TEXT A `<length>` AT ALL — CSS Values §6's production, answered WITHOUT crashing and WITHOUT a realm,
   which is a different question from the one above and exists for a different caller. `css_length_parse` is for
   a value already known to match the grammar (lexbor validated the declaration), and it CRASHES on one that
   does not, deliberately. A shorthand LEXBOR'S REGISTRY DOES NOT CARRY reaches the cascade as raw tokens with
   nothing having validated them, so `border-width: red` is a declaration CSS Syntax DROPS — not an engine gap —
   and its expansion has to be able to ask.
   TRUE for a `<number>` (§6's unitless zero, and the unitless non-zero the parse above asserts on), for a
   dimension in ANY unit §6 defines as a length INCLUDING the relative ones this engine cannot yet absolutize
   (those are a missing component and must reach the crash that names it, never be dropped as invalid), and for
   a MATH FUNCTION whose css-values-4 §10.9 "Type Checking" type matches `<length>` — which is what
   core/css/css_math.h answers, and is why `calc(2em)` is a length here while `rgb(1, 2, 3)` and
   `calc(1px + 1s)` are not. FALSE for a `<percentage>`, for a keyword, and for anything else. */
bool css_length_is_length(const char *value);

/* §5.6 "Mixing Percentages and Dimensions"'s `<length-percentage>`, which is the SAME walk with two arms
   widened: a literal `<percentage>` is one, and a math function is one when its §10.9 type matches
   `<length-percentage>` — `calc(100% - 2em)`, which §10.9.1's calculation context makes a `<length>` in every
   property that resolves percentages against one.
   IT IS A SEPARATE ENTRY AND NOT A FLAG BECAUSE THE TWO ARE DIFFERENT PRODUCTIONS IN THE SPEC, and the callers
   split the same way: css-backgrounds-3 §3.3's `<line-width>` admits no percentage at all, while css-fonts-4
   §2.5's `<'font-size'>` is `<length-percentage [0,∞]>` and a `calc(50% + 2px)` there is a valid declaration
   that must not be dropped. A caller that asks the wrong one turns one into the other silently. */
bool css_length_is_length_percentage(const char *value);

/* CSS Values §6's SNAP A LENGTH AS A LINE WIDTH, which css-backgrounds-3 §3.3 makes part of a
   `border-*-width`'s COMPUTED value ("absolute length, snapped as a border width"): a length that is an integer
   number of DEVICE PIXELS is unchanged, one whose absolute value is between zero and a single device pixel is
   rounded AWAY from zero to one, and anything larger is rounded TOWARDS zero to a whole number of them.
   THE DEVICE PIXEL IS `realm`'s, and the answer therefore DERIVES from the device pixel ratio — which is the
   whole reason this takes a realm and answers a `CssPx`. A ratio of 1 leaves every whole number of CSS pixels
   alone and a ratio of 1.5 does not, so the result is a function of a fact viewport.h models as a forkable
   environment SOURCE, and a length that lost that fact would report `1px` as an author's own number. A border
   width that is ITSELF viewport-derived (`border-width: 1vw`) is a function of the ratio AND of the viewport,
   and the answer carries both. */
CssPx css_length_snap_line_width(JSContext *realm, CssPx len);

/* css-values-4 §10.11 "Computed Value"'s LAST SENTENCE, performed: "The calculation tree is again simplified at
   used value time; with used value time information, a math function always simplifies down to a single numeric
   value." The information that arrives at used-value time is the PERCENTAGE BASIS, and this is the one place
   the two terms meet it — for BOTH kinds that carry a percentage, because §10.11 makes them one value and §5.6
   "Mixing Percentages and Dimensions" makes the sum one production ("both values are converted to absolute
   lengths and added").
   THE BASIS IS THE CALLER'S AND IS NOT ONE FACT. Each property names its own in its own section, and they are
   different measures on the two axes and different rules when the measure does not exist: CSS 2.1 §8.3 "Margin
   properties" and §8.4 "Padding properties" resolve against the containing block's WIDTH on BOTH axes ("note
   that this is true for 'margin-top' and 'margin-bottom' as well"), §10.2 "Content width: the 'width' property"
   and §10.4 "Minimum and maximum widths: 'min-width' and 'max-width'" against that same width, and §10.7
   "Minimum and maximum heights: 'min-height' and 'max-height'" against a HEIGHT whose non-existence is a RULE
   rather than an omission. So this entry takes the answer and never asks for it — the same layering
   `CssFontMetrics` above has, for the same reason.
   THE TWO TERMS ARE RESOLVED IN ONE STEP, which is the shape of the arithmetic and not a convenience: the
   scaled basis and the length term each carry their own environment facts and the result is a joint function of
   BOTH, so `calc(100% - 2rem)` on a viewport-derived containing block is a function of the INITIAL CONTAINING
   BLOCK and of the reader's DEFAULT FONT SIZE at once. Resolving the percentage to a number first and adding a
   length to it afterwards would produce the same example and the wrong DOMAIN at whichever of the two steps
   dropped the other's set.
   IT DOES NOT CLAMP. css-values-4 §9.1 "Numeric Functions" puts the range check AFTER the resolution — "the
   value of a numeric function is clamped to the range allowed in the context it is used at computed value time
   if possible, and at USED VALUE TIME otherwise" — and the range is the PROPERTY's, which this component does
   not know: a `margin` may be negative and a `padding` may not. So the caller that named the basis clamps too. */
CssPx css_length_resolve_pct(CssLength len, CssPx basis);

/* CSSOM §6.7.2's "serialize a CSS value" for an absolute length and for a percentage: the number, then `px` or
   `%`. The number is css-values §serializing's SHORTEST FORM THAT ROUND-TRIPS, which is what makes `4px` come
   back as "4px" and not "4.000000px". OWNED: the caller frees. */
char *css_length_serialize_px(double px);
char *css_length_serialize_pct(double pct);

/* THE SAME OPERATION WITH THE UNIT LEFT TO THE CALLER — CSSOM §6.7.2's SERIALIZE A `<number>` from `v`, then
   `unit` appended. The two entries above are this one with their suffix written in; it is public because a
   SECOND standard states its serialization over the same sentence: CSS Typed OM 1 §6.4 CSSUnitValue
   Serialization step 2 is "Set s to the result of serializing a <number> from value, per CSSOM § 6.7.2
   Serializing CSS Values", and its step 3 appends the unit.
   SO THE VALUE NEED NOT BE A LENGTH, and that is not this component overreaching. §6.7.2's number rule is
   stated once for every CSS value that carries one, and this file holds the only implementation of it in the
   engine — a `<time>` or an `<angle>` printed by a second shortest-round-trip search is the copy that
   disagrees about `0.1 + 0.2` while producing a string that looks like CSS either way. `unit` is written
   VERBATIM: §6.4's own "percent" arm appends `%` rather than the word, and choosing between them is the
   caller's step, not this printer's. OWNED: the caller frees. */
char *css_length_serialize_number(double v, const char *unit);

/* css-values-4 §10.13 "Serialization" for §10.11's two-term residue, whose shape that section pins exactly.
   The root is a Sum, so "serialize a math function" wraps it in `calc(`; "serialize a calculation tree"'s Sum
   branch SORTS the children first, and "sort a calculation's children nodes" puts the PERCENTAGE ahead of every
   dimension — then joins them with " + ", except that "if child is a negative numeric value, append ' - ' to s,
   then serialize the NEGATION of child". §10.13's own example is the whole answer read back: "A value like
   calc(20px + 0%) would serialize as calc(0% + 20px), maintaining both terms in the serialized value."
   IT DOES NOT CLAMP, and that is §10.13's own ordering rather than an omission: the clamp step is stated only
   for a root that "is a numeric value (number, percentage, or dimension)" — a single term, which is
   `css_length_serialize_px`'s case — because a Sum with an unresolved percentage has no number to check against
   a range yet. §9.1 "Numeric Functions" is the same sentence from the other side: at used value time otherwise.
   OWNED: the caller frees. */
char *css_length_serialize_calc(double pct, double px);

#endif
