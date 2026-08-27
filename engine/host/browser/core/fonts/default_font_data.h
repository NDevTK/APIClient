/* THE BYTES OF THIS USER AGENT'S DEFAULT FONT — css-fonts-4 §5.2 "Matching font styles"'s "or a user agent's
   default font if none are available", which is the whole of the first-available-font answer for a user agent
   that installs no faces and loads none.
   They are DATA and this header is the only thing that is not generated: default_font_data.c is written by
   engine/fontsubset.mjs, whose header carries the face, its licence, the command that obtains the input, and
   the justification for every table the subset drops. Nothing in this file may be hand-edited into agreement
   with that file — the generator is the single writer of both the bytes and the length. */
#ifndef ENGINE_HOST_BROWSER_CORE_FONTS_DEFAULT_FONT_DATA_H
#define ENGINE_HOST_BROWSER_CORE_FONTS_DEFAULT_FONT_DATA_H

/* A METRICS-ONLY sfnt: a table directory over 'head', 'maxp', 'hhea', 'hmtx' and 'cmap'. It is deliberately
   NOT a conformant OpenType font — OpenType requires 'name', 'OS/2', 'post', 'glyf' and 'loca' of a face with
   TrueType outlines and this has none of them — because it is not a face anything RENDERS with. It is the
   answer to the three questions core/fonts/open_type_metrics.h asks, and core/fonts/open_type_metrics.c is
   written to reject a face missing a table it needs rather than to assume this one's shape. */
extern const unsigned char DEFAULT_FONT_SFNT[];
extern const unsigned int DEFAULT_FONT_SFNT_LEN;

#endif
