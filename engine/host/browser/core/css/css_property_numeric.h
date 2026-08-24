/* WHICH NUMERIC PRODUCTION A PROPERTY'S VALUE GRAMMAR NAMES — the one fact that decides whether a math
 * function may be written as that property's value.
 *
 * WHY IT IS A COMPONENT. css-values-4 §10 "Mathematical Expressions" opens by making a math function a value of
 * EVERY numeric type — "A math function represents a numeric value, one of: <length>, <frequency>, <angle>,
 * <time>, <flex>, <resolution>, <percentage>, <number>, <integer> ...or the <length-percentage>/etc mixed
 * types, and can be used wherever such a value would be valid." The clause that does the work here is the last
 * one: WHEREVER SUCH A VALUE WOULD BE VALID. So `calc()` is admitted not by a list of calc-friendly properties
 * but by each property's own grammar naming a numeric production, and the question "may a math function be
 * written here, and as what" is one question with a hundred answers rather than a hundred questions.
 *
 * THE ANSWER IS PER-PROPERTY AND THE NEIGHBOURING PROPERTIES DISAGREE, which is the whole reason this is a
 * table and not a predicate. css-backgrounds-3 §3.3 "Line Thickness: the border-width properties" makes a
 * border width a `<line-width>`, which is "<length [0,∞]> | thin | medium | thick" — no percentage anywhere in
 * it — while css-fonts-4 §2.5 "Font size: the font-size property" makes a font size a `<length-percentage
 * [0,∞]>`. `font-size: calc(50% + 2px)` is a valid declaration and `border-top-width: calc(50% + 2px)` is not,
 * and the two differ by nothing an implementation can see except their grammars. core/css/css_length.h already
 * carries that split as two entries — `css_length_is_length` and `css_length_is_length_percentage` — and this
 * table is what tells a caller which of them its property is asking. A single "is it a length-ish thing"
 * predicate answers one of those two properties wrongly no matter which way it is written, and SILENTLY: the
 * over-wide answer admits an invalid declaration and the narrow one drops a valid one, and CSS reports neither.
 * The same split runs through the non-length families: css-text-3 §7.2 "Tracking: the letter-spacing property"
 * is `<length>` while css-position-3 §3.1 "Box Insets" is `<length-percentage>`; css-color-4 §3.3
 * "Transparency: the opacity property" admits a `<number>` OR a `<percentage>` and neither of those matches the
 * other's production in CSS Typed OM 1 §4.3.2's algebra.
 *
 * SO THE ANSWER IS A SET, NOT A PRODUCTION. A grammar spelled with `|` names as many numeric productions as it
 * has numeric branches, and a math function is valid there when its §10.9 "Type Checking" type matches ANY of
 * them: css-inline-3 §5.1 "Line Spacing: the line-height property" is "normal | <number [0,∞]> |
 * <length-percentage [0,∞]>", so `line-height: calc(1.5)` and `line-height: calc(1em + 2px)` are both valid and
 * resolve to DIFFERENT types. Carrying one production per property would drop whichever of the two was not
 * chosen, and a caller cannot recover the other by widening — `<number>` and `<length>` are disjoint in
 * §4.3.2, deliberately, because `calc(1px + 1)` must remain invalid.
 *
 * IT DOES NOT MODEL THE GRAMMAR, AND THAT BOUNDARY IS WHAT KEEPS IT HONEST. It says which numeric productions a
 * grammar NAMES and whether they are the whole value; it does not say what else the grammar admits, does not
 * validate a keyword, and cannot decide `text-indent: calc(1em) hanging`. A component that tried would be a
 * second copy of every property definition in CSS, which is the one thing a table like this must not become —
 * so the shape below is a POSITIVE statement about where a math function may stand, and every caller that needs
 * more asks the component that owns that grammar.
 *
 * THE RANGE BRACKETS ARE DELIBERATELY NOT HERE. `<length-percentage [0,∞]>` carries a range and this table
 * records only the production, because css-values-4 §10.12 "Range Checking" puts the clamp AFTER the match —
 * "The clamping/rounding behavior of numeric functions is, for math functions, only performed on the results of
 * a top-level calculation" — and a negative `calc()` is therefore a VALID declaration that computes to the
 * clamped value, never a dropped one. Recording the range here would turn §10.12's clamp into a syntax error
 * and drop `width: calc(50% - 100px)` on a narrow viewport, which is the single most common thing calc() is
 * written for. core/css/css_math.h states the same split from the other side. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_PROPERTY_NUMERIC_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_PROPERTY_NUMERIC_H

#include "core/css/css_math.h"

/* WHERE IN THE VALUE A NUMERIC PRODUCTION MAY STAND. The distinction is not decoration: it is exactly the line
   between a value this engine can judge with `css_math_matches` — which parses ONE math function and requires
   the token stream to END after it — and one it cannot. */
typedef enum {
    /* The grammar names NO numeric production, so a math function is not a value of this property at all and a
       declaration carrying one is invalid CSS (CSS Syntax 3 §5.5.6 "Consume a declaration"'s last step). This
       is a POSITIVE statement — it is what makes `display: calc(1px)` a dropped declaration rather than an
       unaudited one — and `*productions` is written as zero for it. */
    CSS_NUMERIC_NONE = 0,
    /* Every numeric production the grammar names is the WHOLE value: `width`, `font-size`, `z-index`. A value
       that is one math function is decided here in full, and a value that is anything else is decided by the
       grammar this component does not model. */
    CSS_NUMERIC_WHOLE,
    /* A numeric production appears as ONE COMPONENT of a longer value — css-text-3 §8.1's `text-indent` is
       "[ <length-percentage> ] && hanging? && each-line?", css-fonts-4 §2.4's `font-style` is "... | oblique
       <angle [-90deg,90deg]>?", and every shorthand whose components are numeric. A value that is one math
       function and nothing else is still decided here, because a `{1,4}` or an omitted optional makes the
       one-component spelling legal; a LONGER one is the caller's to split into component values first. */
    CSS_NUMERIC_COMPONENT
} CssNumericShape;

/* One production, as a bit — the set is small and closed (CssMathProduction has ten members), so a bitmask is
   the whole of it and there is no growth path that needs anything else. */
#define CSS_NUMERIC_BIT(p) (1u << (unsigned)(p))

/* WHICH NUMERIC PRODUCTIONS `name`'s value grammar NAMES, and where in the value they may stand. `name` is a
   property name as CSSOM serializes one — lower case, not a custom property.
   `*productions` is a mask of CSS_NUMERIC_BIT(CssMathProduction), written on every path INCLUDING the
   CSS_NUMERIC_NONE one, where it is zero. It is never left untouched: a caller that read a stale mask past a
   NONE answer would admit whatever the previous property admitted, which is the defect a written-nowhere field
   always is.
   IT CRASHES FOR A PROPERTY IT HAS NOT AUDITED, and that is the point of it being exhaustive rather than a
   lookup with a default. A default of NONE would silently call an unaudited property "admits no math function",
   which is indistinguishable from a real answer and is precisely how `font-size: calc(50% + 2px)` would come to
   be dropped by a table that simply had not heard of `font-size`. The audited set is the one lexbor's property
   registry carries, because that registry is what decides which declarations are TYPED (and so can be found
   invalid) rather than handed on as raw tokens — so a lexbor upgrade that adds a property is a crash naming the
   row to write, not a silent widening. */
CssNumericShape css_property_numeric(const char *name, unsigned *productions);

/* DOES THIS COMPONENT RECORD `name`'s GRAMMAR AT ALL — a different question from the one above, and the reason
   it is separate is that the one above must never be defaulted while this one must never crash.
   A caller that HOLDS a property lexbor typed is asking "which production", and an unaudited name there is an
   engine gap that has to crash (see above). A caller that is ASSERTING something about whatever declaration
   happens to be passing — the invariant that a validated value matches its property's production — is asking
   about a property that may never have been validated by anything at all: lexbor's registry stops well short
   of CSS, so `border-radius: calc(4px)` and `gap: calc(1rem)` are handed to the cascade as raw tokens with no
   grammar ever consulted, and there is no producer for such a value to have got wrong. Asserting over those
   would abort on two of the most common declarations on the web, in the name of an invariant that says nothing
   about them. So the assertion is scoped to the audited set, and this is how it asks. */
bool css_property_numeric_audited(const char *name);

/* The table's own invariant — strictly ascending name order, which the lookup is a binary search over. Asserted
   once, beside the other tables cssom_init asserts, for the reason stated at the assertion: an unsorted table
   does not fail loudly on its own, it silently reports a row it holds as a property it has never heard of. */
void css_property_numeric_init(void);

#endif
