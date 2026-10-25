/* The state an HTML parser holds between two writes into its input stream, taken as a value and put back.
 *
 * HTML §8.4.1 document.open() leaves one lxb_html_parser_t on the Document across script execution, and a fork
 * between two §8.4.3 document.write() calls gives each arm its own continuation of that parser. The parser's
 * state is not in the per-flow delta, so the arm that writes takes its own state back from the snapshot its
 * last write left (core/html/document_open.c keeps that snapshot in a JS slot, which time-travels).
 *
 * A snapshot is an ArrayBuffer, so it is a JS value the engine owns and frees, and it is hot-only: it holds
 * node and function addresses, valid within one heap. The cold tier resumes by replay, which re-derives the
 * parse (solver/cold.h). */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HTML_PARSER_SNAPSHOT_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HTML_PARSER_SNAPSHOT_H

#include <lexbor/html/html.h>

#include "quickjs.h"

/* The tree builder's insertion modes, its three stacks, its form pointer and flags, and the tokenizer's state
   functions and options, as of now. OWNED by the caller. */
JSValue html_parser_snapshot_take(JSContext *ctx, const lxb_html_parser_t *parser);

/* Puts `snap` back into `parser`, replacing whatever another timeline left there. */
void html_parser_snapshot_restore(JSContext *ctx, lxb_html_parser_t *parser, JSValueConst snap);

#endif /* ENGINE_HOST_BROWSER_CORE_HTML_HTML_PARSER_SNAPSHOT_H */
