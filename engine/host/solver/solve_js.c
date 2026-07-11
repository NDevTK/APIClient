/* JS sink lexical-context detector — see solve_js.h. */
#include "solver/solve_js.h"
#include "check.h"    /* DCHECK — the scanner's own invariant (balanced state stack) */
#include <string.h>

/* A minimal, correct lexical-state scanner: walk the sink source up to the hole tracking which string/template/
   comment we are inside. NOT a parser — no AST, no expression grammar — just the finite delimiter states, which
   is exactly what decides the breakout. Template ${} expression nesting uses a small state stack (a hole inside
   ${...} is an EXPRESSION, not a template string). Regex literals are deliberately NOT tracked: the /-is-regex
   vs /-is-division decision needs full grammar, and a mis-detected regex hole simply yields an EXPR breakout that
   eval-verification rejects if wrong (a miss, never a false positive) — the sound conservative choice. */
JsHoleCtx solve_js_ctx_detect(const char *shape) {
    if (!shape) return JHC_EXPR;
    const char *hole = strchr(shape, '{');
    if (!hole) return JHC_EXPR;

    enum { NORMAL, SQ, DQ, TMPL, LINE, BLOCK };
    int st = NORMAL;
    int stack[64]; int sp = 0;   /* template/expr nesting: pushed on ` and ${ */

    for (const char *p = shape; p < hole; p++) {
        char c = *p;
        switch (st) {
        case NORMAL:
            if      (c == '\'') st = SQ;
            else if (c == '"')  st = DQ;
            else if (c == '`')  { if (sp < 64) stack[sp++] = NORMAL; st = TMPL; }
            else if (c == '/' && p[1] == '/') { st = LINE;  p++; }
            else if (c == '/' && p[1] == '*') { st = BLOCK; p++; }
            else if (c == '}' && sp > 0)      { st = stack[--sp]; }   /* close a ${ expression -> back to its template */
            break;
        case SQ: if (c == '\\') p++; else if (c == '\'') st = NORMAL; break;
        case DQ: if (c == '\\') p++; else if (c == '"')  st = NORMAL; break;
        case TMPL:
            if      (c == '\\') p++;
            else if (c == '`')  { st = (sp > 0) ? stack[--sp] : NORMAL; }   /* close the template */
            else if (c == '$' && p[1] == '{') { if (sp < 64) stack[sp++] = TMPL; st = NORMAL; p++; }   /* enter ${ expression */
            break;
        case LINE:  if (c == '\n') st = NORMAL; break;
        case BLOCK: if (c == '*' && p[1] == '/') { st = NORMAL; p++; } break;
        }
    }
    DCHECK(sp >= 0 && sp <= 64, "solve_js_ctx_detect: template nesting stack out of range (corrupt scan)");
    switch (st) {
        case SQ:    return JHC_SQ;
        case DQ:    return JHC_DQ;
        case TMPL:  return JHC_TEMPLATE;
        case LINE:  return JHC_LINE_COMMENT;
        case BLOCK: return JHC_BLOCK_COMMENT;
        default:    return JHC_EXPR;
    }
}
