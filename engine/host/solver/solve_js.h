/* JS sink lexical-context detector — the JS-side analogue of solve_html.c's parse-context probe. A JS sink
 * (eval/Function/setTimeout(string)) is CODE; the tainted hole sits in one lexical context (an expression, a
 * '/"/`-delimited string, or a //- or /*-comment), and the MINIMAL breakout is DERIVED from that context, not
 * guessed from a fixed candidate list. This is a lexical-state SCANNER (string/template/comment nesting up to the
 * hole), not a parser/AST — bounded, isolation-testable (feed a shape, assert the context), with its own DCHECK.
 * construct_js_breakout (solve.c) turns the detected context into the derived candidate(s), still eval-verified. */
#ifndef ENGINE_HOST_SOLVER_SOLVE_JS_H
#define ENGINE_HOST_SOLVER_SOLVE_JS_H

/* The lexical context the tainted hole sits in within a JS sink string. */
typedef enum {
    JHC_EXPR,           /* expression / statement position (unquoted): `var t=<hole>` , `f(<hole>)` */
    JHC_SQ,             /* inside a '...'  single-quoted string */
    JHC_DQ,             /* inside a "..."  double-quoted string */
    JHC_TEMPLATE,       /* inside a `...`  template literal (string part, not a ${} expression) */
    JHC_LINE_COMMENT,   /* after // on the hole's line */
    JHC_BLOCK_COMMENT   /* inside a /* ... *\/ block comment */
} JsHoleCtx;

/* Scan `shape` up to its first `{` hole marker, tracking string/template/comment state, and return the hole's
   lexical context. Template ${} expression nesting is tracked, so a hole inside ${...} reports JHC_EXPR. */
JsHoleCtx solve_js_ctx_detect(const char *shape);

#endif
