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
   TrueType outlines and this has none of them. It is the answer to the three questions
   core/fonts/open_type_metrics.h asks, and core/fonts/open_type_metrics.c is written to reject a face missing
   a table it needs rather than to assume this one's shape.

   THE REASON THAT USED TO BE GIVEN FOR DROPPING THE OUTLINE TABLES IS RETIRED, AND IT IS REWRITTEN RATHER THAN
   DELETED BECAUSE A READER WHO RE-DERIVES IT WILL DROP THEM AGAIN. The sentence that stood here said the face
   is not conformant `because it is not a face anything RENDERS with`, and engine/fontsubset.mjs justified the
   same drop with `outlines — nothing here rasterizes`. Both were true when written and are false now:
   core/graphics/rasterizer.h fills a path, core/paint/display_list_raster.h drives it from a display list, and
   core/fonts/glyph_outline.h turns a glyph description into the segments that fill takes. So the tables are
   absent for one reason only — the generator has not been re-run — and the rule that now holds is that a face
   this engine ships needs whatever the questions the engine ASKS require, which is no longer three.

   NAMED RESIDUAL — THIS FACE CARRIES NO OUTLINES, SO NO DOCUMENT THIS ENGINE PRESENTS CAN PAINT TEXT.
   WHAT IS NOT COVERED: every glyph of every document is MEASURED and not drawn. core/fonts/glyph_outline.h
     decodes a glyph description into path segments and is exercised today only against bytes
     engine/host/test_forced.c states, because there is no outline table in this face for it to read.
   WHAT THE NEXT DIFF BUILDS: nothing in C. engine/fontsubset.mjs already carries the edit that keeps 'glyf'
     and 'loca' — landed UNEXERCISED, with the generated file below unchanged — so what is owed is THREE ACTS
     and they are separable: RE-RUN the generator against the input its header names, BUILD, and INSTALL. Only
     the coordinator who owns builds may perform any of them, and the 109-commit gap measured between the
     installed artifact's stamp and the branch tip is the standing proof that the second and third are not one
     act. A lane meeting this residual runs the observation, gets the same answer, and is right to leave it.
   HOW ITS ABSENCE WOULD SHOW: the generated file's own header prints the list of tables it kept and the list
     it dropped. Read that pair — while 'glyf' and 'loca' stand in the dropped list this residual is live, and
     nothing about the tree or the engine changes that reading. */
extern const unsigned char DEFAULT_FONT_SFNT[];
extern const unsigned int DEFAULT_FONT_SFNT_LEN;

#endif
