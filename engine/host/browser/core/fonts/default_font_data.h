/* THE BYTES OF THIS USER AGENT'S DEFAULT FONT — css-fonts-4 §5.2 "Matching font styles"'s "or a user agent's
   default font if none are available", which is the whole of the first-available-font answer for a user agent
   that installs no faces and loads none.
   They are DATA and this header is the only thing that is not generated: default_font_data.c is written by
   engine/fontsubset.mjs, whose header carries the face, its licence, the command that obtains the input, and
   the justification for every table the subset drops. Nothing in this file may be hand-edited into agreement
   with that file — the generator is the single writer of both the bytes and the length. */
#ifndef ENGINE_HOST_BROWSER_CORE_FONTS_DEFAULT_FONT_DATA_H
#define ENGINE_HOST_BROWSER_CORE_FONTS_DEFAULT_FONT_DATA_H

/* AN OUTLINE-CARRYING sfnt: a table directory over 'cmap', 'glyf', 'head', 'hhea', 'hmtx', 'loca' and 'maxp'.
   It is still deliberately NOT a conformant OpenType font — OpenType additionally requires 'name', 'OS/2' and
   'post', and this has none of the three — because what a face this engine ships must carry is whatever the
   questions the engine ASKS require, and no question here reads any of them. It answers the three
   core/fonts/open_type_metrics.h asks and the glyph descriptions core/fonts/glyph_outline.h decodes, and both
   readers are written to reject a face missing a table they need rather than to assume this one's shape.

   TWO REASONS FOR DROPPING THE OUTLINE TABLES ARE RETIRED, AND THEY ARE REWRITTEN RATHER THAN DELETED BECAUSE
   A READER WHO RE-DERIVES EITHER WILL DROP THEM AGAIN. The sentence that stood here called the face
   `A METRICS-ONLY sfnt` that is not conformant `because it is not a face anything RENDERS with`, and
   engine/fontsubset.mjs justified the same drop with `outlines — nothing here rasterizes`. Both were true when
   written and neither is now: core/graphics/rasterizer.h fills a path, core/paint/display_list_raster.h drives
   it from a display list, and core/fonts/glyph_outline.h turns a glyph description into the segments that fill
   takes. A justification that names a capability the tree lacks is one a later landing can falsify without
   ever reading it, which is why both are kept here in the words they were written in.

   THE RESIDUAL THAT STOOD HERE IS RETIRED BY THE ACT IT NAMED, AND THAT IS RECORDED RATHER THAN SIMPLY CUT.
   It read `THIS FACE CARRIES NO OUTLINES, SO NO DOCUMENT THIS ENGINE PRESENTS CAN PAINT TEXT`, and it named
   three separable acts owed by the coordinator who owns builds — RE-RUN the generator, BUILD, INSTALL. Its own
   stated retiring observation was to read the kept and dropped table lists the generated file's header prints,
   and it is discharged on the first of the three: 'glyf' and 'loca' now stand in the KEPT list. The other two
   acts are the ordinary build lag every landing has and are not a narrowing of this component, so they are not
   restated here as a residual — a record with no end is the ratchet this project's own spec forbids.

   WHAT IS STILL NOT DRAWN IS NOT THIS COMPONENT'S GAP AND IS DELIBERATELY NOT RESTATED AS ONE HERE. A face
   with outlines is not a document with text on it: nothing in core/paint/ emits a mark that names a glyph, so
   the road from this data to a painted page runs through the display list rather than through this file. That
   is where a reader should look for it, and a second copy of that gap in this header would be a claim about
   another component that goes stale the day that component moves. */
extern const unsigned char DEFAULT_FONT_SFNT[];
extern const unsigned int DEFAULT_FONT_SFNT_LEN;

#endif
