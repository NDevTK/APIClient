/* CSS VALUES AND UNITS §5 — the `<length>`, and the CSS PIXEL every geometric answer in this engine is
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
 * THE ABSOLUTE UNITS ARE ARITHMETIC AND ARE COMPUTED. CSS Values §5.2 anchors all six of them to the CSS pixel
 * — 1in = 96px by definition, and cm, mm, Q, pt and pc are exact fractions of the inch — so there is no device
 * to consult and nothing to model. §Headless is not the reason any of these would be missing.
 *
 * THE RELATIVE UNITS ARE A MISSING COMPONENT AND THEY CRASH BY NAME, which is the whole point of this file
 * being the one place that knows. A font-relative unit (`em`, `ex`, `ch`, `rem`, `cap`, `ic`, `lh`, `rlh`)
 * resolves against a COMPUTED FONT SIZE, and this engine's cascade has no font-size chain at all — no
 * inheritance step (css_computed_value.c's CSS-wide-keyword DFAIL states that gap), so no element has a
 * computed `font-size` for one to resolve against. media_query.c resolves `em` against the INITIAL font size
 * and is right to: Media Queries §4 evaluates a query before any element exists to have a font size, so the
 * initial value is the spec's own answer there and is NOT a stand-in for the missing chain. On an ELEMENT it
 * would be one, which is why this file crashes instead of borrowing that number.
 *
 * A VIEWPORT-RELATIVE UNIT IS A DIFFERENT MISSING PIECE and crashes separately, because the fix is not the
 * same: `vw` resolves against a viewport this engine DOES model (core/frame/viewport.h) but that is answered
 * PER REALM, and this entry is reached through `char *css_resolved_value(element, property)`, which carries no
 * realm — so the plumbing, not the value, is what is absent. Naming them with one message would send the next
 * reader to build the wrong thing.
 *
 * AND `calc()` IS A THIRD. css-values §10 makes a math function a value in its own right whose result depends
 * on every unit above; lexbor parses it and serializes it back as text, so it arrives here as a string that is
 * not a dimension, and the honest answer is the grammar this file does not implement. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_LENGTH_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_LENGTH_H
#include <stdbool.h>

typedef enum {
    CSS_LENGTH_ABSOLUTE = 0,   /* an absolute `<length>`; `px` carries it in CSS pixels */
    CSS_LENGTH_PERCENTAGE,     /* a `<percentage>`; `pct` carries the number, with the `%` removed */
    CSS_LENGTH_KEYWORD         /* not a length at all: `auto`, `none`, `min-content`, `normal`, … */
} CssLengthKind;

typedef struct {
    CssLengthKind kind;
    double        px;    /* CSS_LENGTH_ABSOLUTE only */
    double        pct;   /* CSS_LENGTH_PERCENTAGE only */
} CssLength;

/* Classify a length-valued property's value, as the cascade serialized it. A dimension in a unit this engine
   cannot absolutize CRASHES here rather than being reported as a third kind — see the header: each of the
   three groups names a DIFFERENT missing component, and a caller handed "unresolvable" could only guess which.
   A UNITLESS ZERO is a `<length>` (CSS Values §5.1 permits the unit to be omitted for zero, and it is how
   lexbor serializes every box-model property's initial value). */
CssLength css_length_parse(const char *value);

/* IS THIS TEXT A `<length>` AT ALL — CSS Values §5's production, answered WITHOUT crashing, which is a
   different question from the one above and exists for a different caller. `css_length_parse` is for a value
   already known to match the grammar (lexbor validated the declaration), and it CRASHES on one that does not,
   deliberately. A shorthand LEXBOR'S REGISTRY DOES NOT CARRY reaches the cascade as raw tokens with nothing
   having validated them, so `border-width: red` is a declaration CSS Syntax DROPS — not an engine gap — and
   its expansion has to be able to ask.
   TRUE for a `<number>` (§5.1's unitless zero, and the unitless non-zero the parse above asserts on), for a
   dimension in ANY unit §5 defines as a length INCLUDING the relative ones this engine cannot yet absolutize
   (those are a missing component and must reach the crash that names it, never be dropped as invalid), and for
   a FUNCTION — css-values §10 makes a math function a value of whatever type its operands give it, and telling
   `calc()` from `rgb()` is the same unbuilt grammar the parse above crashes for. FALSE for a `<percentage>`,
   for a keyword, and for anything else. */
bool css_length_is_length(const char *value);

/* CSS Values §6's SNAP A LENGTH AS A BORDER WIDTH (the spec's own alias of "snap a length as a line width"),
   which css-backgrounds-3 §3.3 makes part of a `border-*-width`'s COMPUTED value: a length that is an integer
   number of DEVICE PIXELS is unchanged, one between zero and a single device pixel is rounded AWAY from zero
   to one, and anything larger is rounded TOWARDS zero to a whole number of them. The device pixel is where the
   engine's own plumbing runs out, so a length this cannot answer for crashes rather than reporting the
   unsnapped number. */
double css_length_snap_line_width(double px);

/* CSSOM §6.7.2's "serialize a CSS value" for an absolute length: the number, then `px`. The number is
   css-values §serializing's SHORTEST FORM THAT ROUND-TRIPS, which is what makes `4px` come back as "4px" and
   not "4.000000px". OWNED: the caller frees. */
char *css_length_serialize_px(double px);

#endif
