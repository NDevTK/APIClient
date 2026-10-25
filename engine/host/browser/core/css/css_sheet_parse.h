/* CSS Syntax 3 §5.4.3 "Parse a stylesheet", a work quantum at a time: a run owns its own lexbor parser and
 * selector state, so between two steps all of the parse's state is this record and the sheet it fills, and a
 * driver may return to the scheduler there. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_SHEET_PARSE_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_SHEET_PARSE_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/css/css.h>

typedef struct CssSheetParse CssSheetParse;

/* Starts the parse of `text` into `sst`, whose arena receives the rules, and runs it to its first suspension
   or its end. `text` and `sst` are BORROWED until finish or abort: the tokenizer keeps pointers into the text.
   Each quantum of bytes tokenized is charged as CSS parse work at the suspension that ends it. */
CssSheetParse *css_sheet_parse_begin(lxb_css_stylesheet_t *sst, const char *text, size_t len);
/* The parse has consumed every byte or failed. Stepping past it is a DCHECK. */
bool css_sheet_parse_ended(const CssSheetParse *run);
/* One more quantum of the parse. */
void css_sheet_parse_step(CssSheetParse *run);
/* Charges the bytes not yet charged and frees the run. True when `sst` holds the parsed rules. */
bool css_sheet_parse_finish(CssSheetParse *run);

/* The complete parse, for a caller that cannot rest between quanta: a run driven to its end. */
bool css_sheet_parse(lxb_css_stylesheet_t *sst, const char *text, size_t len);

#endif
