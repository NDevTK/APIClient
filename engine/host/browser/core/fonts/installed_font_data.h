/* The bytes of this user agent's installed faces (core/fonts/installed_font.h). Each `*_data.c` beside this
   header is written by engine/fontsubset.mjs, whose header carries the face, its licence, the command that
   obtains the input and why each dropped table is sound to drop; this header is the only part not generated.
   The licences ship beside the data: core/fonts/LICENSE.dejavu for the two DejaVu faces and
   core/fonts/LICENSE.liberation (SIL Open Font License 1.1) for the two Liberation faces. */
#ifndef ENGINE_HOST_BROWSER_CORE_FONTS_INSTALLED_FONT_DATA_H
#define ENGINE_HOST_BROWSER_CORE_FONTS_INSTALLED_FONT_DATA_H

extern const unsigned char LIBERATION_SERIF_SFNT[];
extern const unsigned int LIBERATION_SERIF_SFNT_LEN;
extern const unsigned char LIBERATION_SANS_SFNT[];
extern const unsigned int LIBERATION_SANS_SFNT_LEN;
extern const unsigned char DEJAVU_SANS_MONO_SFNT[];
extern const unsigned int DEJAVU_SANS_MONO_SFNT_LEN;
extern const unsigned char DEJAVU_SANS_SFNT[];
extern const unsigned int DEJAVU_SANS_SFNT_LEN;

#endif
