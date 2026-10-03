/* css-counter-styles-3 §5 "Extending list-style-type, counter(), and counters()" — `<counter-style> =
 * <counter-style-name> | <symbols()>`.
 *
 * WHY IT IS A COMPONENT AND NOT A LINE IN A SHORTHAND, which is css-images-3 §2's argument at
 * core/css/css_image.h word for word and is stronger here, because §5's own TITLE names the consumers: the
 * `list-style-type` property and the `counter()` and `counters()` notations. css-counter-styles-3 §3
 * "Defining Custom Counter Styles: the @counter-style rule" adds two more — its own prelude is a
 * `<counter-style-name>` and §3.7 "Defining fallback: the fallback descriptor"' value is one — and
 * core/css/css_rule.c already names `@counter-style` as a rule kind this engine parses the prelude of and
 * declares no rule object for. A copy per consumer is what disagrees about `symbols(cyclic "*")` the day one
 * of them is edited.
 *
 * THE ANSWER IS A BOOLEAN AND NOT A COUNTER STYLE, and that is the split rather than a narrowing. §5 says what
 * a name DENOTES — "If a <counter-style-name> is used that does not refer to any existing counter style, it
 * must act identically to the decimal counter style (but does not compute to decimal)" — so the name does not
 * resolve at parse time at all, and §3.4's `Computed value:` line is `specified value`. What a caller asking
 * this question needs is therefore the VALIDITY of the component value and nothing else; generating the marker
 * STRING a name stands for is css-counter-styles-3 §3.1 "Counter algorithms: the system descriptor"'s
 * algorithms over an `@counter-style` registry, which is a different component and a later one.
 *
 * THE CANONICAL SPELLING IS A SEPARATE ENTRY BECAUSE §3 MAKES IT A PROPERTY OF THE NAME AND NOT OF THE SPAN:
 * "Counter style names are case-sensitive. However, the names defined in this specification are ASCII
 * lowercased on parse wherever they are used as counter styles, e.g. in the list-style set of properties, in
 * the @counter-style rule, and in the counter() functions." So `UPPER-ROMAN` and `upper-roman` are ONE value
 * and `MyStyle` and `mystyle` are TWO, which no single case-folding comparison can express.
 *
 * WHAT IS NOT COVERED, BY NAME. §4 "Defining Anonymous Counter Styles: the symbols() function" states one
 * CONDITIONAL arm this entry does not ask: "If the system is alphabetic or numeric, there must be at least two
 * <string>s or <image>s, or else the function is invalid" — that one IS asked, below — and §4's `<image>` arm
 * is css-images-3 §2's production, so the four css-images-4 arms core/css/css_image.h records as absent are
 * absent from a `symbols()` too and for the same reason. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_COUNTER_STYLE_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_COUNTER_STYLE_H
#include <stdbool.h>
#include <stddef.h>

/* Does the ONE component value `text`/`len` match §5's `<counter-style>`? The span is a single component value
   — CSS Syntax §4 "Tokenization" makes a function token one however many spaces its arguments carry — neither
   NUL-terminated nor lowercased.
   FALSE for `none`, which is NOT a `<counter-style>`: §3 states the exclusion on the name itself ("a
   <custom-ident> that is not an ASCII case-insensitive match for none"), and css-lists-3 §3.4 "Text-based
   Markers: the list-style-type property" writes `none` as its own term of `<counter-style> | <string> | none`.
   TRUE for `disc`, `decimal` and the other four §3 calls the non-overridable counter-style names, because that
   exclusion is stated for the DEFINING use alone: "When used here, to define a counter style, it also cannot
   be any of the non-overridable counter-style names (in other uses that merely reference a counter style, such
   as the extend system, these are allowed)." A property value is such a reference. */
bool css_counter_style_is_counter_style(const char *text, size_t len);

/* §3's CANONICAL SPELLING of the `<counter-style-name>` `text`/`len`, or NULL when the span is not one of the
   names css-counter-styles-3 defines — which is not a refusal: §3 makes an author's own name CASE-SENSITIVE,
   so a caller that gets NULL keeps the author's own bytes. BORROWED: the answer is a static string.
   IT IS ASKED OF A NAME AND NOT OF A `<counter-style>`, because a `symbols()` has no name to canonicalize and
   is kept verbatim for the reason core/css/css_image.h gives for an `<image>`: the specified value is what the
   author wrote, and inventing a spelling for it is this engine deciding something css-counter-styles-3 has
   not. */
const char *css_counter_style_canonical_name(const char *text, size_t len);

#endif
