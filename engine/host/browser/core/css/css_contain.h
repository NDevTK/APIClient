/* css-contain-2 §2 "Strong Containment: the contain property"'s value as the set of containment types it names.
 * The `Value:` line is parsed here, so the grammar a declaration is admitted by and the set a consumer reads
 * (core/css/css_computed_value.h's `css_computed_contain`) are one. */

#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_CONTAIN_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_CONTAIN_H

#include <stdbool.h>

enum {
    CSS_CONTAIN_SIZE = 1u << 0,
    CSS_CONTAIN_INLINE_SIZE = 1u << 1,
    CSS_CONTAIN_LAYOUT = 1u << 2,
    CSS_CONTAIN_STYLE = 1u << 3,
    CSS_CONTAIN_PAINT = 1u << 4
};

/* `text` against `none | strict | content | [ [size | inline-size] || layout || style || paint ]`, with `strict`
   and `content` expanded as §2 defines them. FALSE for a value outside it, with `*bits` unwritten: such a value
   is page input and is refused, never asserted. */
bool css_contain_parse(const char *text, unsigned *bits);

/* The set as CSSOM §6.7.2 "Serializing CSS Values" writes it. OWNED. */
char *css_contain_serialize(unsigned bits);

#endif
