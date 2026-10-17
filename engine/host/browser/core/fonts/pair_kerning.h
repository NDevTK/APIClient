/* A face's PAIR KERNING: OpenType 'GPOS' — Glyph Positioning Table's horizontal 'kern' feature, as real Chrome
 * applies it. Blink shapes text with HarfBuzz, and Chromium 0842759f
 * third_party/blink/renderer/platform/fonts/shaping/font_features.cc leaves 'kern' at HarfBuzz's default, which
 * is on, for `font-kerning: auto` and `normal`. HarfBuzz applies a feature's lookups in LookupList order, each
 * over the whole run, so a glyph's adjustment is the sum of every lookup's.
 * What is read: the ScriptList's 'latn' script, else 'DFLT', its default language system, and that system's
 * 'kern' lookups — LookupType 2, Pair Adjustment, in either subtable format, the first glyph's ValueRecord an
 * XAdvance and the second's empty. IgnoreMarks skips glyphs 'GDEF' — Glyph Definition Table classes as marks
 * (class 3) when finding the second glyph. Anything else a kern lookup could hold is refused at the read with
 * a reason, so a face whose kerning this cannot state is never kerned partly. */

/* Named residual. Not covered: script and language selection. Every run is kerned with the 'latn' (else 'DFLT')
 * default language system, where HarfBuzz itemizes runs by Unicode script and takes the language system
 * the content language names. Next diff: script itemization at core/layout/text_run.c's segment boundary,
 * with the script tag passed here. How its absence shows: a Greek or Cyrillic line whose face kerns that
 * script differently from Latin measures differently from real Chrome by those pairs. */
#ifndef ENGINE_HOST_BROWSER_CORE_FONTS_PAIR_KERNING_H
#define ENGINE_HOST_BROWSER_CORE_FONTS_PAIR_KERNING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The kern lookups a face holds, located and proved by `pair_kerning_read`. An installed face's kern feature
   names at most this many; a face with more is refused with a reason naming the constant. */
#define PAIR_KERNING_LOOKUP_MAX 8

typedef struct {
    const unsigned char *gpos;
    size_t gpos_len;
    const unsigned char *gdef;     /* NULL when the face has no 'GDEF' */
    size_t gdef_len;
    size_t glyph_class_def;        /* GDEF's glyphClassDef offset from `gdef`, or 0 for none */
    int n;                         /* kern lookups, in LookupList order */
    size_t lookup[PAIR_KERNING_LOOKUP_MAX];   /* each Lookup table's offset from `gpos` */
    bool ignore_marks[PAIR_KERNING_LOOKUP_MAX];
    /* NULL exactly when the tables are readable; otherwise the rule they broke. */
    const char *reject;
} PairKerning;

/* Read and prove `gpos[0..gpos_len)` (NULL for a face with no 'GPOS', which kerns nothing) and `gdef`. The bytes
   are BORROWED and must outlive `k`. False with `k->reject` set for tables this cannot state. */
bool pair_kerning_read(PairKerning *k, const unsigned char *gpos, size_t gpos_len, const unsigned char *gdef,
                       size_t gdef_len);

/* The glyph `k` places after the left one (k = 0 is the next), or -1 past the end of the run. */
typedef int32_t (*PairKerningGlyphAfter)(void *arg, size_t k);

/* The left glyph's XAdvance adjustment in font design units: the sum over the kern lookups of the pair it
   forms with the next glyph that lookup does not skip. */
int32_t pair_kerning_x_advance(const PairKerning *k, uint16_t left, PairKerningGlyphAfter after, void *arg);

#endif
