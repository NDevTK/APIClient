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
 *   A FONT-RELATIVE unit (`em`, `ex`, `ch`, `rem`, `cap`, `ic`, `lh`, `rlh`) resolves against a COMPUTED FONT
 *   SIZE, and this engine's cascade has no font-size chain at all — no inheritance step (css_computed_value.c's
 *   CSS-wide-keyword DFAIL states that gap), so no element has a computed `font-size` for one to resolve
 *   against. It CRASHES by name. media_query.c resolves `em` against the INITIAL font size and is right to:
 *   Media Queries §4 evaluates a query before any element exists to have a font size, so the initial value is
 *   the spec's own answer there and is NOT a stand-in for the missing chain. On an ELEMENT it would be one,
 *   which is why this file crashes instead of borrowing that number.
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
 * AND `calc()` IS A THIRD. css-values §10 makes a math function a value in its own right whose result depends
 * on every unit above; lexbor parses it and serializes it back as text, so it arrives here as a string that is
 * not a dimension, and the honest answer is the grammar this file does not implement.
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

typedef enum {
    CSS_LENGTH_ABSOLUTE = 0,   /* an absolute `<length>`; `px` carries it in CSS pixels */
    CSS_LENGTH_PERCENTAGE,     /* a `<percentage>`; `pct` carries the number, with the `%` removed */
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
    CssPx         px;                             /* CSS_LENGTH_ABSOLUTE only */
    double        pct;                            /* CSS_LENGTH_PERCENTAGE only */
    char          keyword[CSS_LENGTH_KEYWORD_MAX];/* CSS_LENGTH_KEYWORD only */
} CssLength;

/* CLASSIFY AND ABSOLUTIZE a length-valued property's value, as the cascade serialized it — CSS 2.1 §4.3.2's
   computed value for a `<length>`, which is why this is the step core/css/css_computed_value.c performs and
   the only caller it has. A dimension in a unit this engine cannot absolutize CRASHES here rather than being
   reported as a fourth kind — see the header: each group names a DIFFERENT missing component, and a caller
   handed "unresolvable" could only guess which.
   `realm` IS THE REALM THE VIEWPORT IS ANSWERED PER — the ELEMENT's document's active realm, never the running
   one, because an iframe's ICB is 300 CSS pixels wide and its parent's is 1280. It may be NULL for a document
   no navigable presents; only a viewport-percentage unit needs it, and that is the arm that crashes.
   A UNITLESS ZERO is a `<length>` (CSS Values §6 permits the unit to be omitted for zero, and it is how
   lexbor serializes every box-model property's initial value). */
CssLength css_length_parse(JSContext *realm, const char *value);

/* IS THIS TEXT A `<length>` AT ALL — CSS Values §6's production, answered WITHOUT crashing and WITHOUT a realm,
   which is a different question from the one above and exists for a different caller. `css_length_parse` is for
   a value already known to match the grammar (lexbor validated the declaration), and it CRASHES on one that
   does not, deliberately. A shorthand LEXBOR'S REGISTRY DOES NOT CARRY reaches the cascade as raw tokens with
   nothing having validated them, so `border-width: red` is a declaration CSS Syntax DROPS — not an engine gap —
   and its expansion has to be able to ask.
   TRUE for a `<number>` (§6's unitless zero, and the unitless non-zero the parse above asserts on), for a
   dimension in ANY unit §6 defines as a length INCLUDING the relative ones this engine cannot yet absolutize
   (those are a missing component and must reach the crash that names it, never be dropped as invalid), and for
   a FUNCTION — css-values §10 makes a math function a value of whatever type its operands give it, and telling
   `calc()` from `rgb()` is the same unbuilt grammar the parse above crashes for. FALSE for a `<percentage>`,
   for a keyword, and for anything else. */
bool css_length_is_length(const char *value);

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

/* CSSOM §6.7.2's "serialize a CSS value" for an absolute length and for a percentage: the number, then `px` or
   `%`. The number is css-values §serializing's SHORTEST FORM THAT ROUND-TRIPS, which is what makes `4px` come
   back as "4px" and not "4.000000px". OWNED: the caller frees. */
char *css_length_serialize_px(double px);
char *css_length_serialize_pct(double pct);

#endif
