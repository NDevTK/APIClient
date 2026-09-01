/* CSS Values and Units 4 §10 "Mathematical Expressions" — the math functions, their §10.8 "Syntax" grammar,
 * the §10.9 "Type Checking" algebra that decides what one IS, and §10.10.1 "Simplification"'s reduction of one
 * to a value.
 *
 * WHY IT IS A COMPONENT AND NOT A HELPER INSIDE WHOEVER NEEDED IT FIRST. §10's opening sentence is the whole
 * reason: "A math function represents a numeric value, one of: <length>, <frequency>, <angle>, <time>, <flex>,
 * <resolution>, <percentage>, <number>, <integer> ...or the <length-percentage>/etc mixed types, and can be
 * used wherever such a value would be valid." So `calc()` is not a feature of any one property — it is a value
 * of EVERY numeric type, and three unrelated places in this engine were each crashing on the same absence: the
 * computed-value chain absolutizing a length-valued property (core/css/css_length.c), CSS Properties and
 * Values API 1 §5.1's numeric syntax components (core/css/css_syntax_match.c), and HTML §4.8.4.2.2 "Sizes
 * attributes"'s `<source-size-value>` (core/html/image_source_set.c). One grammar answered from three places
 * is the defect CLAUDE.md §per-realm names; one answered from one place is this file.
 *
 * WHAT LEXBOR PROVIDED AND WHAT IS PORTED HERE. The vendored fork ships CSS Syntax 3's TOKENIZER and nothing
 * about math functions — `calc` does not appear anywhere under its css module, and `lxb_css_unit_t` is a
 * dimension-unit enum that stops short of §6.1.1's `r`-prefixed font metrics, §6.1.2.1's `sv*`/`lv*`/`dv*`
 * viewport families and css-grid-2 §7.2.4's `fr`. So the tokenizer is USED (CLAUDE.md's bind-before-build
 * order stops at "existing Lexbor module" for the token stream) and §10.8's grammar, §10.9's type algebra and
 * §10.10.1's simplification are PORTED. The unit tables are not ported at all: §6's `<length>` set is
 * core/css/css_length.h's and §7's other three families are core/css/css_dimension.h's, which is what keeps
 * this file from becoming a fourth answer to "is `dvmin` a length".
 *
 * THE TYPE ALGEBRA IS NOT ARITHMETIC ON NUMBERS AND THAT IS THE WHOLE DIFFICULTY. §10.9 makes a calculation's
 * type a MAP from base type to integer exponent plus a PERCENT HINT, and the three operations over it — add,
 * multiply, invert — live in CSS Typed OM 1 §4.3.2 "Numeric Value Typing", which §10.9 links to by name rather
 * than restating. `100vw - 20px` is valid because adding «["length" → 1]» to itself succeeds; `100vw * 20px`
 * is NOT invalid at all but resolves to «["length" → 2]», which §10.9's last rule then refuses because it
 * matches none of the productions a math function can resolve to. A hand-rolled "is everything the same unit"
 * check gets both of those wrong in opposite directions, which is why the algebra is written out in full.
 *
 * THE PERCENT HINT IS WHERE THE CALCULATION CONTEXT ENTERS. §10.9.1 "Calculation Contexts": "Math functions
 * always inherit the calculation context from wherever they're used" — so `width: calc(25% + 50px)` types its
 * `25%` as a LENGTH carrying a hint, because §10.9's `<percentage>` terminal rule says a percentage resolved
 * against another type takes that type, while `sizes="calc(50%)"` types the same token as «["percent" → 1]»
 * because HTML §4.8.4.2.2 states "percentages are not allowed in a <source-size-value>". ONE parameter — the
 * production the caller wants — supplies both halves, because in the spec they are the same fact.
 *
 * THE VALUE IS A `CssPx` BECAUSE THAT IS THIS ENGINE'S NUMBER-WITH-ENVIRONMENT-PROVENANCE CARRIER, and the
 * number in it is in §5.4.1 "Compatible Units"'s CANONICAL UNIT for the resolved type — CSS pixels for a
 * `<length>` (§6.2: "All of the absolute length units are compatible, and px is their canonical unit"), which
 * is where that carrier gets its name and is the only family in this engine that can currently acquire an
 * environment fact at all. Degrees for an `<angle>` (§7.1), seconds for a `<time>` (§7.2), hertz for a
 * `<frequency>` (§7.3), dots-per-px for a `<resolution>` (§7.4). Carrying a second numeric struct beside
 * `CssPx` would be a second copy of core/css/css_length.h's union-the-facts arithmetic, which is the one thing
 * that must not be written twice: `calc(100vw - 20px)` is a function of the initial containing block exactly
 * as `50vw` is, and a math component that dropped the set at its own subtraction would delete the mobile arm
 * that `css_px_sub` exists to keep.
 *
 * WHAT A MATH FUNCTION SIMPLIFIES TO HERE, AND WHERE THAT STOPS. §10.10.1 simplifies a calculation tree
 * EAGERLY and bottom-up, so this file simplifies AS IT PARSES and never materializes the tree: each production
 * returns an already-simplified value, which for every math function that resolves to a production CSS admits
 * is a Sum of at most two terms — a number in the canonical unit and a `<percentage>` that §10.11 "Computed
 * Value" leaves unresolved ("Where percentages are not resolved at computed-value time, they are not resolved
 * in math functions, e.g. calc(100% - 100% + 1px) resolves to calc(0% + 1px), not to 1px"). That pair is the
 * `CssMathValue` below. The residues it cannot hold are the ones §10.10.1 leaves as live tree nodes — a
 * Product of two percentage-carrying operands, a Min whose arguments cannot be compared because a percentage
 * "might resolve against a negative basis" — and those CRASH rather than answering, because a plausible number
 * there is exactly the §Architecture defect where a default makes a hole look like a measurement. They crash
 * only when the function is OTHERWISE VALID: `calc(50% * 50%)` types to «["percent" → 2]», which matches no
 * production, so it is refused as invalid CSS and never reaches the crash.
 *
 * §10.9.2's DEGENERATE VALUES ARE IEEE-754 AND ARE MOSTLY FREE, WHICH IS WHY THE EXCEPTIONS ARE WRITTEN OUT.
 * "Math functions follow IEEE-754 semantics", and a C `double` IS IEEE-754, so signed zero and the infinity
 * rules fall out of the arithmetic. The places C's own library DIVERGES from §10 are the places that need
 * code, and each is a real difference rather than a defensive rewrite: §10.5.1's note says NaN "is infectious
 * in every function" while `fmin`/`fmax`/`hypot` return the non-NaN operand and `pow(NaN, 0)` is 1; §10.9.2
 * makes 0⁻ less than 0⁺ for comparison while C leaves `fmin(-0.0, 0.0)` unspecified; §10.5.1 makes `log(A, 1)`
 * NaN while a quotient of logarithms is an infinity. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_MATH_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_MATH_H
#include <stdbool.h>
#include <stddef.h>

#include "core/css/css_length.h"

/* CSS Typed OM 1 §4.3.2 "Numeric Value Typing"'s BASE TYPES, in that section's own order: "The base types are
   "length", "angle", "time", "frequency", "resolution", "flex", and "percent". The ordering of a type's
   entries always matches this base type ordering." The order is kept because the spec states it, so a future
   §10.13 "Serialization" reads the entries out in the order it is defined over rather than re-deriving one. */
typedef enum {
    CSS_MATH_LENGTH = 0,
    CSS_MATH_ANGLE,
    CSS_MATH_TIME,
    CSS_MATH_FREQUENCY,
    CSS_MATH_RESOLUTION,
    CSS_MATH_FLEX,
    CSS_MATH_PERCENT,
    CSS_MATH_BASE_COUNT
} CssMathBase;

/* §4.3.2: "The percent hint is either null or a base type." NULL is a POSITIVE statement and not an absence —
   it says the calculation carries no percentage that has been re-expressed as another type — which is why the
   matching rules below test it rather than defaulting past it. */
#define CSS_MATH_HINT_NULL (-1)

/* §4.3.2's TYPE: "a map of base types to integers (denoting the exponent of each type, so a <length>², such as
   from calc(1px * 1em), is «["length" → 2]»), and an associated percent hint". The map is DENSE here because
   there are seven keys and the spec fixes their order: an absent key and a zero exponent are the same fact in
   every rule §4.3.2 states ("all the entries of type1 with NON-ZERO values are contained in type2 with the
   same value"), so a sparse map would carry a distinction no algorithm reads. */
typedef struct {
    int exp[CSS_MATH_BASE_COUNT];
    int hint;                       /* CSS_MATH_HINT_NULL, or one of CssMathBase */
} CssMathType;

/* THE PRODUCTION A CALLER WANTS, which is BOTH §10.9.1's calculation context and §10.9's last rule's question.
   They are one parameter because in the spec they are one fact: a math function "always inherits the
   calculation context from wherever it is used", so the place that decides whether `25%` may be written is the
   same place that decides whether the answer is a `<length>`.
   §10.9's own note is why `<integer>` is here beside `<number>` rather than being a separate grammar:
   "math functions that resolve to <number> can be used in any place that only accepts <integer>; the value is
   rounded to the nearest integer as it resolves." */
typedef enum {
    CSS_MATH_PROD_NUMBER = 0,
    CSS_MATH_PROD_INTEGER,
    CSS_MATH_PROD_PERCENTAGE,
    CSS_MATH_PROD_LENGTH,
    CSS_MATH_PROD_ANGLE,
    CSS_MATH_PROD_TIME,
    CSS_MATH_PROD_FREQUENCY,
    CSS_MATH_PROD_RESOLUTION,
    CSS_MATH_PROD_FLEX,
    CSS_MATH_PROD_LENGTH_PERCENTAGE
} CssMathProduction;

/* §10.8's TWENTY-ONE FUNCTIONAL NOTATIONS, as one question. `rgb()` is a function and is not one of them, so a
   caller holding a FUNCTION token asks this before it asks anything else: a math function is a value of the
   numeric type its operands give it, and every other function is simply not that type. The name is the
   FUNCTION token's own text, neither NUL-terminated nor lowercased — CSS Syntax 3 §4 makes a function name an
   ident sequence and CSS compares those ASCII case-insensitively. */
bool css_math_is_function(const char *name, size_t len);

/* §10.9 Type Checking's TERMINAL RULE FOR A DIMENSION — which base type a unit IDENTIFIER names — over the
   unit tables the components that define them own (core/css/css_length.h for §6's `<length>`, core/css/
   css_dimension.h for §7's angles, times, frequencies and resolutions). FALSE is §10.9's "anything else",
   which for a calculation is "The calculation's type is failure". The unit is a span, neither NUL-terminated
   nor lowercased, because a dimension token names its unit inside the buffer it was tokenized from and CSS
   compares unit identifiers ASCII case-insensitively.
   IT IS PUBLIC BECAUSE A SECOND STANDARD ASKS THE SAME QUESTION OF THE SAME TABLES, AND THE TWO QUESTIONS ARE
   NOT THE SAME QUESTION. CSS Typed OM 1 §4.3.2 Numeric Value Typing's "create a type from a string unit" has
   nine branches, and seven of them ARE this: "unit is a <length> unit → «[ "length" → 1 ]»", and the same
   sentence for <angle>, <time>, <frequency>, <resolution> and <flex>, with "anything else → Return failure".
   Its other two — "unit is "number" → Return «[ ]»" and "unit is "percent" → Return «[ "percent" → 1 ]»" —
   are NOT dimension units and must not be answered here: CSS Syntax 3 §4 tokenizes a percentage as its own
   token type, and `calc(5number)` IS a dimension token whose unit is the ident `number`, which §10.9 refuses.
   So the TABLE is one fact and each specification asks its own question of it; §4.3.2's two extra branches
   stand at ITS site (core/css/css_numeric_value.h) and this entry keeps §10.9's answer exactly. A second copy
   of the table here would be the copy that disagrees about `dvmin` the day one of them is edited. */
bool css_math_unit_base(const char *unit, size_t len, CssMathBase *base);

/* THE TWO TYPES §4.3.2's create-a-type BRANCHES END AT — a type with no entries at all and a null percent
   hint (§4.3.2: "unit is "number" → Return «[ ]» (empty map)", and §10.9's own terminal rule for a
   `<number>`), and a type whose sole entry is one base type at exponent 1 ("unit is a <length> unit → Return
   «[ "length" → 1 ]»", and the same sentence for the other five productions and for "percent").
   THEY ARE PUBLIC FOR THE SAME REASON `css_math_unit_base` IS: the algebra below is §4.3.2's, this file is
   where §10.9 links to it, and CSS Typed OM 1 §4.3.1's `type()` needs the map that the nine-branch entry at
   core/css/css_numeric_value.h assembles. A second pair of constructors there would be a second statement of
   "in all cases the associated percent hint is null" — the one sentence every rule in §4.3.2 is written
   against — in a file that does not own the struct. */
CssMathType css_math_type_number(void);
CssMathType css_math_type_of(CssMathBase base);

/* §10.9's LAST RULE, answered over the text of one math function: "A math function resolves to <number>,
   <length>, <angle>, <time>, <frequency>, <resolution>, <flex>, or <percentage> according to which of those
   productions its type matches. (These categories are mutually exclusive.) If it can't match any of these, the
   math function is invalid."
   TYPE ONLY — NO LEAF IS RESOLVED, so this needs no realm, no font metrics and no viewport. That is not an
   optimisation, it is what §10.9 is: `calc(5px + 1em)` is a `<length>` whether or not anything in this process
   knows what an `em` is on this element, and CSS Properties and Values API 1 §5.1's syntax components are
   asked at `@property` registration time where there IS no element. FALSE for a math function whose type is
   failure, for one whose type matches a different production, and for text that is not a math function. */
bool css_math_matches(const char *text, size_t len, CssMathProduction want);

/* §10.8 "Syntax" WITHOUT §10.9's last rule: is `text` ONE math function and nothing else, whatever type it
   resolves to? TRUE for `calc(1px + 1s)`, whose type is FAILURE and which `css_math_matches` therefore refuses
   for every production — and that pair is the whole reason this exists. A caller that must tell "this value is
   a math function this property does not admit" (an authoring mistake, and a dropped declaration) from "this
   value is not a lone math function at all" (which may be a longer value with a math function INSIDE it, and
   so a grammar this component cannot judge) cannot get that from `css_math_matches`, which answers false to
   both and to `calc(2em) hanging` besides.
   IT IS THE SAME WALK, which is the point: §10.8's nesting, comma arities and `<calc-value>` productions are
   answered once and the type gate is simply not applied, so there is no second parser to drift. */
bool css_math_is_lone_function(const char *text, size_t len);

/* §10.10.1's "if root is a dimension that is not expressed in its canonical unit, and there is enough
   information available to convert it to the canonical unit, do so" — asked of the CALLER, because WHICH
   number a dimension is worth is a fact about the caller's context and not about §10's arithmetic. The pattern
   is core/css/css_length.h's `CssFontMetrics`, and the reason is the same one that header gives.
   IT IS ASKED FOR ONE FAMILY ONLY. §7's angles, times, frequencies and resolutions are exact ratios to their
   canonical units and are converted in this file through core/css/css_dimension.h, whose own header states the
   split and why: "§6's `<length>` is the one family whose absolutization needs a REALM". So what is left to
   ask is §6's `<length>`, and it is asked because an `em` is the computed `font-size` of the element the unit
   is used on inside a cascade and the INITIAL `font-size` inside a media query (css-values-4 §6.1.1's own last
   clause), and neither answer belongs to §10.
   IT NEVER RETURNS A STAND-IN. A resolver handed a unit it cannot absolutize CRASHES with ITS OWN message,
   naming the component that is missing FROM ITS OWN SIDE — a `sizes` attribute's `dvh` is core/css/
   media_query.c's table and a computed `border-width`'s `dvh` is core/frame/viewport.c's viewport families,
   and this file can name neither. It is called only for a unit this file has already decided is one of §6's,
   so there is no "not a length" answer for it to give.
   `realm` is required only by §10.3's `line-width` <rounding-strategy>, which snaps against a device pixel;
   every other production leaves it untouched, and the crash for an absent one stands where it is needed. */
typedef struct {
    CssPx     (*length_px)(void *ctx, double n, const char *unit, size_t unit_len);
    void       *ctx;
    JSContext  *realm;
} CssMathResolver;

/* WHAT A MATH FUNCTION SIMPLIFIES TO — §10.10.1's residue, which for every function CSS admits is a Sum of at
   most two terms (see the header). `num` is the non-percentage term in §5.4.1's canonical unit for `type`,
   carrying the environment facts it derives from; `pct` is the surviving `<percentage>` term as a number of
   percent, which §10.11 leaves unresolved because the basis is a used value.
   `pct_term` IS A POSITIVE STATEMENT AND NOT A ZERO TEST, for §10.10.1's own stated reason: "Zero-valued terms
   cannot be simply removed from a Sum; they can only be combined with other values that have identical units.
   (This is because the mere presence of a unit, even with a zero value, can sometimes imply a change in
   behavior.)" So `calc(100% - 100% + 1px)` answers pct 0 WITH a percentage term, which is exactly the
   `calc(0% + 1px)` §10.11 says it computes to, and a caller that read `pct != 0` instead would call it `1px`. */
typedef struct {
    CssMathType type;
    CssPx       num;
    double      pct;
    bool        pct_term;
} CssMathValue;

/* §10.10 "Internal Representation" + §10.10.1 "Simplification" + §10.9.2's top-level censoring, over the text
   of one math function. TRUE with `*out` written when the function is valid AND its type matches `want`.
   FALSE — writing nothing — for text that is not a math function, for one whose type is failure, and for one
   that resolves to a different production; all three are "this value does not match this grammar", which is
   the answer a page's own invalid declaration deserves and is never a crash.
   IT CRASHES for a valid math function this component cannot reduce to the pair above (see the header) and for
   one whose leaves need a capability nothing in this engine has (css-grid-2 §7.2.4's `fr`), because those are
   unbuilt mechanisms rather than authoring mistakes.
   §10.12 "Range Checking" IS THE CALLER'S: "The clamping/rounding behavior of numeric functions is, for math
   functions, only performed on the results of a top-level calculation" — and the range is the PROPERTY's
   (`width` is [0,∞], `opacity` is [0,1]), which this component does not know. §10.9.2's context-free half is
   done here: NaN "is censored into a zero value" and a signed zero "does not escape a top-level calculation".
   An infinity is returned AS an infinity, for the caller's §10.12 step to clamp. */
bool css_math_eval(const char *text, size_t len, const CssMathResolver *res,
                   CssMathProduction want, CssMathValue *out);

#endif
