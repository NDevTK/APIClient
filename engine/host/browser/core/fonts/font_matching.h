/* css-fonts-4 §5 "Font Matching Algorithm" over this user agent's installed fonts (core/fonts/installed_font.h).
 *
 * §5.2 "Matching font styles" iterates "over the font families named by the font-family property", resolving
 * a generic family to "the appropriate font family name to be used" and any other name against the installed
 * fonts "(this may include font aliases)" by §5.1 "Localized name matching". Every installed face is one
 * regular face of its family, so §5.2's narrowing by width, style and weight has one candidate and the result
 * of the family walk is an ordered set of faces: FontFallbackList, Blink's name for it. A character takes the
 * first face whose character map contains it, then §5.2's "installed font fallback procedure", then the
 * missing glyph of the first available font.
 *
 * NAMED RESIDUAL. Not covered: @font-face families (§4) and `font-weight`/`font-style`/`font-width` selection
 * within a family, since no installed family has a second face. Next diff builds: a bold and an italic face per
 * family with §5.2's narrowing steps. Absence shows as bold or italic text measuring at its regular face's
 * advances in this engine where Chrome's are wider. */
#ifndef ENGINE_HOST_BROWSER_CORE_FONTS_FONT_MATCHING_H
#define ENGINE_HOST_BROWSER_CORE_FONTS_FONT_MATCHING_H

#include <stdint.h>

#include "core/fonts/installed_font.h"

/* The distinct installed faces a computed `font-family` names, in list order. A face appears once, so the
   list is bounded by the installed set rather than by the family list. */
typedef struct {
    unsigned n;
    InstalledFont face[INSTALLED_FONT_COUNT];
} FontFallbackList;

/* §5.2's family walk over `computed_family`, an element's computed `font-family` (css-fonts-4 §2.1, "as
   specified"), into `*out`. A value outside §2.1's grammar is this codebase's computed value having gone
   wrong, and crashes. */
void font_matching_resolve(const char *computed_family, FontFallbackList *out);

/* §5.2's first available font: "the first font for which the character U+0020 (space) is not excluded by a
   unicode-range, given the font families in the font-family list (or a user agent's default font if none are
   available)". Installed faces have no unicode-range, so it is the list's first face or the default font. */
InstalledFont font_matching_first_available(const FontFallbackList *l);

/* The face §5.2 renders `cp` with: the first face of `l` whose character map contains it, else the installed
   font fallback, else the first available font, whose .notdef is §5.2's "missing character glyph from a
   default font". `cp` is a Unicode scalar value (asserted). */
InstalledFont font_matching_font_for_character(const FontFallbackList *l, uint32_t cp);

#endif
