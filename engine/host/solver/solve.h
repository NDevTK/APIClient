/* @S SOLVER — the NOVEL contribution. When a concolic ATTACKER source reaches a code-execution sink, construct
 * a breakout DERIVED from the sink context and VERIFY it by FIRING (an X9 marker must actually call), not a
 * fixed-payload guess. The emitted finding IS a replay-verified PoC; absence is never a "safe" verdict, only
 * search-not-yet-solved. This first cut handles the eval sink (JS context, no DOM); innerHTML/document.write
 * (HTML context, real Lexbor re-parse) follow. */
#ifndef ENGINE_HOST_SOLVER_SOLVE_H
#define ENGINE_HOST_SOLVER_SOLVE_H

#include "quickjs.h"

/* Install the X9 fire-marker + init the @S store. Call once at engine init (after the global exists). */
void solve_init(JSContext *ctx);
void solve_free(void);

/* eval(arg): a JS-context code-execution sink. innerHTML=arg: an HTML-context sink. Detection records the
   source; a candidate re-run executes/re-parses the injected value so firing can be observed. */
void solve_eval_sink(JSContext *ctx, JSValueConst arg);
void solve_html_sink(JSContext *ctx, JSValueConst arg);
void solve_url_sink(JSContext *ctx, JSValueConst arg);

/* After detection, SEARCH breakout candidates for every recorded source: inject each at the source, re-run the
   REAL program (`rerun`), and record the first that FIRES as the replay-verified PoC. */
void solve_verify(JSContext *ctx, void (*rerun)(JSContext *ctx, void *ud), void *ud);

/* @S findings as JSON: { "securitySinks":[ {"sink":..,"source":..,"poc":..}, ... ] } (caller frees). */
char *solve_json(void);
int   solve_count(void);

#endif
