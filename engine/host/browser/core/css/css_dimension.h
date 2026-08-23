/* CSS Values and Units 4 §7 "Other Quantities" — the unit identifiers that name an `<angle>`, a `<time>` or a
 * `<resolution>`, and §7.4's conversion of a resolution to its canonical unit.
 *
 * WHY IT IS NOT IN core/css/css_length.h. §6's `<length>` is the one family whose absolutization needs a
 * REALM — a font-relative unit resolves against a computed `font-size` and a viewport-percentage one against
 * an initial containing block — which is why that component takes a `JSContext *` and answers a `CssPx` that
 * carries the environment fact it derived from. §7's three families need none of that: `deg`, `s` and `dppx`
 * are each their family's canonical unit by the spec's own sentence and every other member is an exact ratio
 * to it, so this is arithmetic over a unit identifier and nothing else. Putting them behind css_length.h's
 * realm-taking interface would mean every caller that only wants to know whether `turn` is an angle unit had
 * to have a realm to ask.
 *
 * IT IS A COMPONENT BECAUSE THE SET WAS ALREADY ANSWERED TWICE. Media Queries 5 §4's `resolution` feature
 * converts its operand to a comparable number, and CSS Properties and Values API 1 §5.1's `<resolution>`
 * syntax component asks whether a dimension token is one at all — two questions whose answers are the SAME
 * four unit identifiers, and a second copy is what disagrees about `x` the day one of them is edited.
 *
 * A UNIT IS ASCII CASE-INSENSITIVE (CSS Syntax 3 §4 makes a dimension token's unit an ident, and CSS matches
 * unit identifiers ASCII case-insensitively), so every entry below takes a SPAN and lowercases it rather than
 * requiring its caller to have produced a NUL-terminated lowercased copy first — a tokenizer hands out a span,
 * and the copy would be the allocation every failure path then has to free. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_DIMENSION_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_DIMENSION_H

#include <stdbool.h>
#include <stddef.h>

/* §7.1 "Angle Units: the <angle> type and deg, grad, rad, turn units" — "Angle values are <dimension>s denoted
   by <angle>. The angle unit identifiers are: deg … grad … rad … turn". A BARE ZERO IS NOT ONE and this entry
   is asked about a unit, never about a number, which is the same section's note: "For legacy reasons, some
   uses of <angle> allow a bare 0 to mean 0deg. This is not true in general, however, and will not occur in
   future uses of the <angle> type." A caller that is one of those legacy uses states that for itself. */
bool css_angle_unit(const char *unit, size_t unit_len);

/* §7.2 "Duration Units: the <time> type and s, ms units" — "Time values are dimensions denoted by <time>. The
   time unit identifiers are: s … ms". */
bool css_time_unit(const char *unit, size_t unit_len);

/* §7.4 "Resolution Units: the <resolution> type and dpi, dpcm, dppx units" — "Resolution units are dimensions
   denoted by <resolution>. The resolution unit identifiers are: dpi … dpcm … dppx … x". `x` is in that list
   beside `dppx` and shares its definition ("Dots per px unit"), which is why four identifiers name three
   quantities. */
bool css_resolution_unit(const char *unit, size_t unit_len);

/* §7.4's CANONICAL UNIT — "All <resolution> units are compatible, and dppx is their canonical unit" — with the
   two ratios the same section states: "due to the 1:96 fixed ratio of CSS in to CSS px, 1dppx is equivalent to
   96dpi", and a dpcm is a dpi over the inch's 2.54 centimetres. False for a unit that is not §7.4's, writing
   nothing; `n` is the dimension token's number. */
bool css_resolution_dppx(const char *unit, size_t unit_len, double n, double *dppx);

#endif
