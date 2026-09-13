/* SVG 2 §9.3.9 "The grammar for path data" — parsing an SVG path data string into a CanvasPath path.
 *
 * IT IS ITS OWN COMPONENT BECAUSE THE GRAMMAR BELONGS TO NEITHER OF ITS CALLERS. HTML §4.12.5.1.7 "Path2D
 * objects" reaches it — "Let svgPath be the result of parsing and interpreting path according to SVG 2's
 * rules for path data" — and so will SVG 2 §9.2's `path` element and the CSS `path()` shape, none of which is
 * a canvas. Writing it inside Path2D would put the grammar behind whichever caller landed first.
 *
 * A PARSE ERROR IS A SHORTER PATH AND NEVER AN EXCEPTION. SVG 2 §9.5.4 "Error handling in path data" is explicit
 * about both halves: "the SVG user agent shall render a `path` element up to (but not including) the path
 * command containing the first error", and "If a path data command contains an incorrect set of parameters,
 * then the given path data command is rendered up to and including the last correctly defined path segment".
 * So the parse stops and what was already built stands. Nothing here throws, and nothing here asserts on the
 * INPUT: the bytes are the page's own string, which CLAUDE.md §WHOSE-BYTES-STATE-THE-VALUE puts outside what a
 * `DCHECK` may ever stand on — an assert here would be a page-held abort switch, and this is precisely the
 * input a forcing solver hands the engine constantly.
 *
 * THE NUMBER GRAMMAR IS MATCHED BEFORE IT IS CONVERTED, WHICH IS THE ONE PLACE `strtod` WOULD BE WRONG.
 * SVG 2 §9.3.9's `number` is `fractional-constant exponent?` with `fractional-constant::= (digit* "." digit+) |
 * digit+`, and the section says SVG 2 "harmonizes number parsing with CSS, disallowing the relaxed grammar for
 * numbers" — no trailing bare decimal point, no sign inside `number` itself, no hexadecimal, no infinity, no
 * leading whitespace. `strtod` accepts every one of those, so `0x10` handed to it straight would be 16 where
 * the grammar says the token is `0` and the parse stops at `x`. The span is therefore matched by the grammar
 * first and converted from exactly those bytes. The same rule is what makes SVG 2 §9.3.9's own worked examples come
 * out right — "M 100-200" is two coordinates because the minus cannot follow a digit inside one, and
 * "M 0.6.5" is 0.6 then .5 because a coordinate allows one decimal point. */
#ifndef ENGINE_HOST_BROWSER_CORE_CANVAS_SVG_PATH_DATA_H
#define ENGINE_HOST_BROWSER_CORE_CANVAS_SVG_PATH_DATA_H

#include "quickjs.h"

/* Parse `d` and build what it describes onto `path`, which must be a canvas_path_new array. `d` is
   NUL-terminated. The path is left holding every segment the grammar matched before the first error, which for
   a well-formed string is all of them and for the empty string is none — SVG 2 §9.3.9's "The EBNF allows the path
   data string … to be empty". */
void svg_path_data_parse(JSContext *ctx, JSValueConst path, const char *d);

#endif /* ENGINE_HOST_BROWSER_CORE_CANVAS_SVG_PATH_DATA_H */
