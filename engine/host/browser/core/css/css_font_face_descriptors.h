/* css-fonts-4 §4.4 "Font property descriptors: the font-style, font-weight, and font-width descriptors": the
 * three `@font-face` descriptors css-fonts-4 §5.2 "Matching font styles" narrows a family's faces by. Each is
 * a range a face supports (`<font-weight-absolute>{1,2}`, `<'font-width'>{1,2}`, `oblique <angle>{1,2}`), which
 * is a different value definition from the property of the same name, so it has its own grammar here.
 *
 * `auto`, the initial value, reads the range from the font's own data. A face that is not loaded has none,
 * and takes the property's initial value: weight 400, width 100%, style normal. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_FONT_FACE_DESCRIPTORS_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_FONT_FACE_DESCRIPTORS_H

#include <stdbool.h>

/* An inclusive range of a weight, a width percentage or an oblique angle in degrees; `lo <= hi`. */
typedef struct { double lo, hi; } CssFontRange;

typedef enum { CSS_FONT_FACE_NORMAL, CSS_FONT_FACE_ITALIC, CSS_FONT_FACE_OBLIQUE } CssFontFaceSlant;
/* A face's `font-style`: `angle` is read only for CSS_FONT_FACE_OBLIQUE. */
typedef struct { CssFontFaceSlant slant; CssFontRange angle; } CssFontFaceStyle;

/* Each descriptor's value into `*out`. False for a value outside the descriptor's grammar, which is an
   invalid declaration. `auto` answers the unloaded face's range stated above. */
bool css_font_face_weight(const char *value, CssFontRange *out);
bool css_font_face_width(const char *value, CssFontRange *out);
bool css_font_face_style(const char *value, CssFontFaceStyle *out);

/* Whether `name` is one of the three descriptors (`font-stretch` is `font-width`'s legacy name alias,
   css-fonts-4 §4.4.1). */
bool css_font_face_property_descriptor(const char *name);

/* The specified value of descriptor `name`: `value` with its keywords lower-cased and its components joined
   by single spaces, or NULL for an invalid declaration. Owned by the caller. */
char *css_font_face_property_descriptor_value(const char *name, const char *value);

#endif
