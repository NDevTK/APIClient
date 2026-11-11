/* css-fonts-4 §4.5 "Character range: the unicode-range descriptor": the `@font-face` descriptor's value, a
 * comma-separated list of `<unicode-range-token>`s whose union is "the set of codepoints for which the
 * corresponding font may be used". The set is parsed once and asked per code point by css-fonts-4 §5.2
 * "Matching font styles" (core/css/css_font_selector.h). */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_UNICODE_RANGE_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_UNICODE_RANGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One inclusive interval of code points, `lo <= hi <= 0x10FFFF` (asserted where it is read). */
typedef struct { uint32_t lo, hi; } CssUnicodeRange;

/* §4.5's list in the page's order. It grows: the list has no upper bound. */
typedef struct { CssUnicodeRange *v; size_t n, cap; } CssUnicodeRangeSet;

/* §4.5's value into the empty `out`. False, with `out` left empty, for a value outside the grammar: "Ranges
   that do not fit one of these forms are invalid and cause the declaration to be ignored." Release with
   css_unicode_range_free. */
bool css_unicode_range_parse(const char *value, CssUnicodeRangeSet *out);
void css_unicode_range_free(CssUnicodeRangeSet *s);

/* Whether `cp` is in the union of the set's intervals. */
bool css_unicode_range_includes(const CssUnicodeRangeSet *s, uint32_t cp);

/* The descriptor's specified value, serialized as `U+<lo>` or `U+<lo>-<hi>` in upper-case hexadecimal, comma
   separated, in the page's order, or NULL for an invalid declaration. Owned by the caller. */
char *css_unicode_range_descriptor_value(const char *value);

#endif
