/* CSS Syntax 3 §5.4.3 "Parse a stylesheet", a quantum at a time — see css_sheet_parse.h.
 *
 * The run owns a parser and a selector record of its own. Lexbor's stylesheet parse refuses a suspendable run
 * whose parser has no selector record (a stack-local one would die with the call that installed it), and a
 * parser shared across runs would hold one run's tokenizer while another's is suspended. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <lexbor/css/css.h>

#include "check.h"
#include "core/css/css_sheet_parse.h"
#include "core/timing/thread_work.h"
#include "solver/rest_unit.h"   /* the bytes a step tokenizes */
#include <lexbor/core/base.h>   /* lexbor_malloc: the allocator the engine gives its refusal edge */

struct CssSheetParse {
    lxb_css_parser_t     *parser;
    lxb_css_stylesheet_t *sst;     /* BORROWED */
    size_t                len;     /* the text's length; the text itself is the tokenizer's, BORROWED */
    size_t                charged; /* how many of those bytes have been charged as CSS parse work */
    lxb_status_t          status;  /* the last answer: LXB_STATUS_SUSPENDED until the run ends */
};

/* The parser's yield hook: between two tokens, once a rest unit of bytes has been tokenized since the last
   charge, they are charged and the run suspends. Charging only there keeps thread_work_charge's per-call
   rounding to one unit per step. */
static bool css_sheet_parse_yield(lxb_css_parser_t *parser, void *ctx)
{
    CssSheetParse *run = ctx;
    size_t at = (size_t)(parser->tkz->in_p - parser->tkz->in_begin), fresh;

    DCHECK(run->parser == parser, "a sheet parse's yield hook was reached by another run's parser");
    DCHECK(at >= run->charged && at <= run->len, "a sheet's tokenizer stands outside the bytes it was handed");
    fresh = at - run->charged;
    if (fresh < rest_unit_items(REST_UNIT_CSS_PARSE_BYTES)) return false;
    thread_work_charge(THREAD_WORK_CSS_PARSE, fresh);
    run->charged = at;
    return true;
}

CssSheetParse *css_sheet_parse_begin(lxb_css_stylesheet_t *sst, const char *text, size_t len)
{
    CssSheetParse *run;

    DCHECK(sst != NULL && (text != NULL || len == 0), "a sheet parse was begun with no sheet or no text");
    run = lexbor_calloc(1, sizeof *run);
    CHECK(run != NULL, "a sheet parse's record could not be allocated");
    run->parser = lxb_css_parser_create();
    CHECK(run->parser != NULL && lxb_css_parser_init(run->parser, NULL) == LXB_STATUS_OK,
          "a sheet parse's CSS parser could not be created");
    CHECK(lxb_css_parser_selectors_init(run->parser) == LXB_STATUS_OK,
          "a sheet parse's selector state could not be allocated");
    run->sst = sst;
    run->len = len;
    run->parser->yield = css_sheet_parse_yield;
    run->parser->yield_ctx = run;
    run->status = lxb_css_stylesheet_parse(sst, run->parser, (const lxb_char_t *)text, len);
    return run;
}

bool css_sheet_parse_ended(const CssSheetParse *run)
{
    DCHECK(run != NULL, "a sheet parse was asked about with no run");
    return run->status != LXB_STATUS_SUSPENDED;
}

void css_sheet_parse_step(CssSheetParse *run)
{
    DCHECK(!css_sheet_parse_ended(run), "a sheet parse was stepped after its run ended");
    run->status = lxb_css_stylesheet_parse_resume(run->sst, run->parser);
}

bool css_sheet_parse_finish(CssSheetParse *run)
{
    bool ok;

    DCHECK(css_sheet_parse_ended(run), "a sheet parse was finished while its run was suspended");
    thread_work_charge(THREAD_WORK_CSS_PARSE, run->len - run->charged);
    ok = run->status == LXB_STATUS_OK;
    lxb_css_parser_selectors_destroy(run->parser);
    lxb_css_parser_destroy(run->parser, true);
    free(run);
    return ok;
}

void css_sheet_parse_abort(CssSheetParse *run)
{
    DCHECK(run != NULL && !css_sheet_parse_ended(run), "a sheet parse was aborted with no run or after it ended");
    lxb_css_parser_selectors_destroy(run->parser);
    lxb_css_parser_destroy(run->parser, true);
    free(run);
}

/* Named residual. Not covered: resting between steps for a caller that cannot park: cssom_parse_rules, which
   a sheet's text set from script reaches, and the cascade's per-sheet parse asked by a reader that cannot rest
   (core/css/css_style_declaration.c's parse table steps its run under a resting drive). Next diff: those
   callers hold the run in a step machine's state and rest between steps. Its absence shows as a single sheet's
   parse charging several rest units of bytes inside one REST_UNIT_LAYOUT_DESCENDANTS bracket. */
bool css_sheet_parse(lxb_css_stylesheet_t *sst, const char *text, size_t len)
{
    ThreadWorkSpan tw;
    CssSheetParse *run;
    bool ok;

    thread_work_begin(&tw);
    run = css_sheet_parse_begin(sst, text, len);
    while (!css_sheet_parse_ended(run)) css_sheet_parse_step(run);
    ok = css_sheet_parse_finish(run);
    thread_work_end(&tw, THREAD_WORK_CSS_PARSE, len);
    return ok;
}
