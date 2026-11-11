/* css-fonts-4 §5 "Font Matching Algorithm" over this user agent's installed fonts (core/fonts/installed_font.h).
 *
 * §5.2 "Matching font styles" iterates "over the font families named by the font-family property", resolving
 * a generic family to "the appropriate font family name to be used" and any other name "among fonts defined
 * via @font-face rules and then among available installed fonts (this may include font aliases)" by §5.1
 * "Localized name matching". The `@font-face` half and the load it starts are core/css/css_font_selector.h's;
 * a face it loads is not yet one this walk can select, so the list here holds installed faces only. Every
 * installed face is one regular face of its family, so the family walk gives an ordered set of faces:
 * FontFallbackList, Blink's name for it. A character takes the first face whose character map contains it,
 * then §5.2's "installed font fallback procedure", then the missing glyph of the first available font. */
/* NAMED RESIDUAL. Not covered: `font-weight`/`font-style`/`font-width` selection within an installed family,
 * since no installed family has a second face. Next diff builds: a bold and an italic face per family with
 * §5.2's narrowing steps. Absence shows as bold or italic text measuring at its regular face's advances in
 * this engine where Chrome's are wider. */
#ifndef ENGINE_HOST_BROWSER_CORE_FONTS_FONT_MATCHING_H
#define ENGINE_HOST_BROWSER_CORE_FONTS_FONT_MATCHING_H

#include <stdint.h>

#include "core/fonts/installed_font.h"

/* The distinct installed faces a computed `font-family` names, in list order, or the default font alone when
   it names none. A face appears once, so the list is bounded by the installed set, not by the family list. */
typedef struct {
    unsigned n;
    InstalledFont face[INSTALLED_FONT_COUNT];
} FontFallbackList;

/* §5.2's family walk over `computed_family`, an element's computed `font-family` (css-fonts-4 §2.1, "as
   specified"), into `*out`. A value outside §2.1's grammar is this codebase's computed value having gone
   wrong, and crashes. */
void font_matching_resolve(const char *computed_family, FontFallbackList *out);

/* §5.2's installed half for one item of the family list: the face a generic family resolves to, or the face
   a family name or alias names by §5.1. Exactly one of `generic` and `name` is non-NULL (asserted). False
   when the item names no installed face. */
bool font_matching_installed_family(const char *generic, const char *name, InstalledFont *out);

/* css-fonts-4 §4.3.3.1 "Local font fallback": the installed face a `local()` item names. A face's own name
   only, by §5.1's caseless match; an alias is a `font-family` substitution and names no face. */
bool font_matching_installed_face_named(const char *name, InstalledFont *out);

/* §5.2's first available font: "the first font for which the character U+0020 (space) is not excluded by a
   unicode-range, given the font families in the font-family list (or a user agent's default font if none are
   available)". Installed faces have no unicode-range, so it is the list's first face. */
InstalledFont font_matching_first_available(const FontFallbackList *l);

/* The face §5.2 renders `cp` with: the first face of `l` whose character map contains it, else the installed
   font fallback, else the first available font, whose .notdef is §5.2's "missing character glyph from a
   default font". `cp` is a Unicode scalar value (asserted). */
InstalledFont font_matching_font_for_character(const FontFallbackList *l, uint32_t cp);

#endif
