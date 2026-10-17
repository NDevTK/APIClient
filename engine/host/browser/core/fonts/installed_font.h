/* css-fonts-4 §5 "Font Matching Algorithm"'s installed fonts — "The set of installed fonts available to the
 * Font Matching Algorithm is explicitly undefined", so it is this user agent's to state, and it is the four
 * faces below, compiled in from core/fonts/installed_font_data.h. Nothing is read from the host's font
 * directory: core/css/font_metrics.h states why. Which family name, generic family and alias each face answers
 * is core/fonts/font_matching.c's; this component owns only the faces and their parsed tables.
 *
 * X(id, family, fact, bytes): `family` is the face's own family name (core/fonts/font_matching.c matches it
 * by css-fonts-4 §5.1 "Localized name matching"), `fact` the suffix of the face's two environment facts
 * (core/frame/viewport.c), and `bytes` the generated array. */
#ifndef ENGINE_HOST_BROWSER_CORE_FONTS_INSTALLED_FONT_H
#define ENGINE_HOST_BROWSER_CORE_FONTS_INSTALLED_FONT_H

#include <stdbool.h>

#include "core/fonts/glyph_outline.h"
#include "core/fonts/open_type_metrics.h"
#include "core/fonts/pair_kerning.h"

#define INSTALLED_FONT_LIST(X)                                                              \
    X(LIBERATION_SERIF, "Liberation Serif", "liberationSerif", LIBERATION_SERIF_SFNT)       \
    X(LIBERATION_SANS,  "Liberation Sans",  "liberationSans",  LIBERATION_SANS_SFNT)        \
    X(DEJAVU_SANS_MONO, "DejaVu Sans Mono", "dejaVuSansMono",  DEJAVU_SANS_MONO_SFNT)       \
    X(DEJAVU_SANS,      "DejaVu Sans",      "dejaVuSans",      DEJAVU_SANS_SFNT)

typedef enum {
#define INSTALLED_FONT_ID(id, family, fact, bytes) INSTALLED_FONT_##id,
    INSTALLED_FONT_LIST(INSTALLED_FONT_ID)
#undef INSTALLED_FONT_ID
    INSTALLED_FONT_COUNT
} InstalledFont;

/* The face's family name, ASCII (asserted by core/fonts/font_matching.c's caseless match). */
const char *installed_font_family(InstalledFont f);

/* The face's OpenType tables, read on first use. The bytes are generated from a pinned input, so a face that
   does not parse is a CHECK. */
const OpenTypeMetrics *installed_font_metrics(InstalledFont f);

/* The face's TrueType outlines, proved on first use. `reject` is non-NULL exactly when the face carries no
   'glyf'/'loca' pair, which is a capability (CFF) and not a broken face. */
const GlyphOutlines *installed_font_outlines(InstalledFont f);

/* The face's 'GPOS' pair kerning, proved on first use; a face with no 'GPOS' kerns nothing. */
const PairKerning *installed_font_kerning(InstalledFont f);

#endif
