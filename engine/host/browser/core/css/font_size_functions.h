/* THE USER AGENT'S TABLE OF FONT SIZES — css-fonts-4 §2.5.1 "Absolute Size Keyword Mapping Table", and the
 * number `medium` is.
 *
 * WHY THERE IS A TABLE AT ALL, IN THE SPEC'S OWN WORDS. css-fonts-4 §2.5 "Font size: the font-size property"
 * defines `<absolute-size>` as "a keyword [that] refers to an entry in A TABLE OF FONT SIZES COMPUTED AND KEPT
 * BY THE USER AGENT", and §2.5.1 gives that table's scaling factors with `medium` as "the reference middle
 * value". So the eight keywords are not eight constants: they are ONE picked number and eight exact ratios to
 * it, and the ratios are the spec's while the number is this user agent's.
 *
 * WHY IT IS A COMPONENT AND NOT A CONSTANT INSIDE WHOEVER NEEDED IT FIRST. Three readers already need this one
 * number and each would otherwise carry its own copy — the defect CLAUDE.md §per-realm names, in the same
 * plainest form core/frame/viewport.h describes for the viewport:
 *   - core/css/css_computed_value.c derives §2.5's computed `font-size`, whose base case is `medium`
 *     (CSS Cascade §7.2 Inheritance: "for the root element, which has no parent element, the inherited value
 *     is the initial value of the property", and §2.5's `Initial:` line is `medium`).
 *   - core/css/css_length.c absolutizes css-values-4 §6.1.1's `em` and `rem`, whose base case is the same
 *     number by a different sentence — §6.1.1's "the computed metrics corresponding to the initial values of
 *     the font and line-height properties, if the element has no parent".
 *   - core/css/media_query.c evaluates a font-relative length in a media query, where §6.1.1 says outright
 *     that "when used outside the context of an element (such as in media queries), the font-relative lengths
 *     units refer to the metrics corresponding to the initial values of the font and line-height properties".
 * That file held a literal `16.0` of its own, which is exactly the shape its OWN comment already calls out one
 * field along ("this held its own literal 1920 and 1080, which is the same fact answered from two places").
 *
 * THE DEFAULT FONT SIZE IS A PICKED ENVIRONMENT FACT, BY core/frame/viewport.h'S OWN TEST AND NOT BY TASTE.
 * That test is "whether the model PICKED one point out of a range the environment leaves free, or DERIVED the
 * only value the rest of the model permits". Nothing in this engine determines what `medium` is, and the spec
 * does not either: §2.5 makes an `<absolute-size>` keyword "an entry in a table of font sizes COMPUTED AND
 * KEPT BY THE USER AGENT", and §2.5.1 — which is that table — says only that "the medium value is used as the
 * reference middle value" and that "the user agent may fine-tune these values for different fonts or different
 * types of display devices". §2.5 says elsewhere, of the SEPARATE `<relative-size>` ratio, that "a
 * sight-impaired user may request a user agent use a higher ratio than default", which is the same spec
 * treating this family of numbers as the reader's rather than the document's. So the default is picked out of
 * a range that is free, and it reaches a page as a number a page branches on:
 * `parseFloat(getComputedStyle(document.documentElement).fontSize)` is how a bundle asks whether the reader
 * has enlarged their default text, and a `rem`-sized layout puts its whole responsive ladder behind that
 * number. Collapsing it to a bare 16 is the collapse CLAUDE.md §Headless forbids in the same sentence that
 * permits the example.
 *
 * AND THE UA COLOUR THEME IS NOT ONE, WHICH IS THE COMPARISON THAT MAKES THIS CHECKABLE RATHER THAN A
 * PREFERENCE. core/css/css_system_color.h models nineteen equally-picked UA choices as a FIXED table and is
 * right to, because CSS Color 4 §6.2 states the fixed answer as the recommended one in its own text ("user
 * agents may, to mitigate privacy and security risks such as fingerprinting, elect to return fixed values for
 * the used value of system colors which do not reflect customisation or theming choices made by the user").
 * css-fonts-4 has no such sentence for `medium`; it says the opposite. The two components differ because their
 * two specs differ, not because two readers weighed the same evidence differently.
 *
 * WHAT THE FACT IS NOT: §2.5's `<relative-size>` RATIO. `larger` and `smaller` are also a picked point in a
 * range the spec leaves free ("the specific ratio is unspecified, but should be around 1.2–1.5"), and they are
 * deliberately NOT a second row. viewport.h's facts are the ones a page can read TWICE — `devicePixelRatio` is
 * its own fact precisely because a bundle asks the member and measures a border and both must be one question
 * — and no member anywhere reports this ratio. Every length it produces is already a function of
 * CSS_ENV_DEFAULT_FONT_SIZE through the parent chain it scales, so the world where the reader's preferences
 * differ is already forked; a second source key would name a question nothing can ask. What the ratio gets
 * instead is the ASSERTION its range makes possible, over the arithmetic rather than over the constant.
 *
 * THE READABILITY FLOOR IS AN INVARIANT AND NOT A CLAMP. §2.5.1 closes with "to preserve readability, an UA
 * applying these guidelines should nevertheless avoid creating font sizes of less than 9 device pixels per EM
 * unit". A user agent that meets that by CLAMPING has made its table non-linear and its smallest two keywords
 * indistinguishable; a user agent that meets it by CHOOSING a default the table clears asserts it instead, and
 * the assert names §2.5.1 the day someone lowers the default past it. That is the whole value of the number
 * being picked here rather than in three places: there is one place for the guideline to be checked. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_FONT_SIZE_FUNCTIONS_H
#define ENGINE_HOST_BROWSER_CORE_CSS_FONT_SIZE_FUNCTIONS_H
#include <stdbool.h>
#include <stddef.h>

#include "core/css/css_length.h"
#include "quickjs.h"

/* §2.5's `Initial: medium`, which is §2.5.1's reference middle value and therefore the ONE picked number the
   whole table is ratios of. It is also the base case of three separate chains — CSS Cascade §7.2's inherited
   value at the root element, css-values-4 §6.1.1's "initial values of the font ... properties" for an `em` in
   a font-affecting property on an element with no parent, and §6.1.1's same clause for a font-relative length
   used outside the context of an element at all.
   IT CARRIES CSS_ENV_DEFAULT_FONT_SIZE, so every length derived from it reaches the page as the example of a
   concolic whose domain is the reader's font preference (see the header, and core/frame/viewport.c's seam).
   `realm` IS THE ELEMENT'S DOCUMENT'S, never the running one, for the reason css_length.h gives for every
   other fact: the source key is keyed on the document that read it, so a length with no realm has no question
   to be an answer to. A document no navigable presents CRASHES here rather than being handed a bare number. */
CssPx css_default_font_size(JSContext *realm);

/* IS THIS TOKEN ONE OF §2.5's EIGHT `<absolute-size>` KEYWORDS — the membership half, asked WITHOUT a realm
   because a grammar deciding whether `font: x-large serif` parses has no element and wants no number. The span
   need not be NUL-terminated or lowercased; a CSS keyword is ASCII case-insensitive and surrounding white
   space a serialization may have left is ignored. */
bool css_absolute_size_keyword(const char *kw, size_t n);

/* §2.5.1's ENTRY for one of those eight — the scaling factor times the default font size above, carrying that
   fact. `kw` must be one of them (the membership entry decides), and the answer is ABSOLUTE: an
   `<absolute-size>` keyword does NOT depend on the parent element, which is exactly what distinguishes it from
   §2.5's `<relative-size>` below. */
CssPx css_absolute_size_px(JSContext *realm, const char *kw);

/* §2.5's `<relative-size>` — `larger` and `smaller`, "interpreted relative to the computed font-size of the
   PARENT element and possibly the table of font sizes".
   THE SPEC OFFERS TWO IMPLEMENTATIONS AND ONLY ONE OF THEM IS EXPRESSIBLE IN A COMPUTED VALUE. The first walks
   §2.5.1's table ("if the parent element has a KEYWORD font size in the absolute size keyword mapping table,
   larger may compute the font size to the next entry in the table") and so needs the parent's computed value
   to have remembered that it came from a keyword — which §2.5's own `Computed value:` line forbids it from
   carrying, since an absolute length is a number and nothing else. The second is stated as the alternative in
   the same paragraph ("instead of using next and previous items in the previous keyword table, user agents may
   instead use a simple ratio to increase or decrease the font size relative to the parent element"), needs
   nothing but the parent's number, and is therefore the one a computed value can actually produce. */
CssPx css_relative_size_px(CssPx parent, bool larger);

/* css-values-4 §6.1.1's `font-affecting properties` — "properties that affect the font size or font metrics of
   an element". It is a DEFINITION and not a closed list, and the spec says so itself ("most properties defined
   in [css-fonts-4] are font-affecting properties, as is math-style and math-depth. (This isn't necessarily an
   exhaustive list.)"), so what this answers is that definition applied to the properties whose computed value
   core/css/css_computed_value.c derives — and it gains a row when that set does, never a default standing in
   for a property nobody has weighed.
   IT IS THE PREDICATE THAT STOPS A DEFINITION BEING OF ITSELF, which is why it is a component's entry and not
   an `if` at the one site that reads it: §6.1.1 resolves a font-relative length inside a font-affecting
   property against the PARENT's metrics, so `div { font-size: 1.2em }` is 1.2 times the INHERITED size.
   Answering false for `font-size` would make the unit resolve against the element's own computed font-size,
   which is the value being computed — an unbounded walk, not a wrong number — so the site that computes a
   font-size asserts this answers true rather than assuming it. */
bool css_font_affecting_property(const char *name);

#endif
