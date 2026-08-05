/* @S SOLVER — the NOVEL contribution. When a concolic ATTACKER source reaches a code-execution sink, construct
 * a breakout DERIVED from the sink context and VERIFY it by FIRING (an X9 marker must actually call), not a
 * fixed-payload guess. The emitted finding IS a replay-verified PoC; absence is never a "safe" verdict, only
 * search-not-yet-solved. This first cut handles the eval sink (JS context, no DOM); innerHTML/document.write
 * (HTML context, real Lexbor re-parse) follow. */
#ifndef ENGINE_HOST_SOLVER_SOLVE_H
#define ENGINE_HOST_SOLVER_SOLVE_H

#include "quickjs.h"
struct Flow;

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
/* SEED one candidate flow per (detected sink, breakout) onto the ONE frontier — the re-fire is a FLOW, never a
   driver that runs the program to completion beside the BFS. */
int  solve_seed_candidates(JSContext *ctx);   /* seeds the not-yet-seeded sinks; returns how many flows it added */
/* How many candidate flows this document has seeded in total. Each one RE-RUNS the page, so this number times
   the page's cost is most of what an @S search spends — and it is what says whether a run got slower because
   there were more searches or because each search grew. */
int  solve_candidate_count(void);
void solve_flow_begin(struct Flow *f);
void solve_flow_end(struct Flow *f);

/* EVERY DETECTED SINK as a JSON ARRAY (caller frees). Two entry shapes, because a sink is in one of two states
   and they must never be confused:
     fire-verified  `{"sink":..,"source":..,"poc":..}`
     parked search  `{"sink":..,"source":..,"search":"parked","tried":N[,"sourceEncodes":".."]}`
   The parked shape exists because absence is never a "safe" verdict: a sink an attacker source REACHES is
   reported whether or not its breakout has been solved, and it carries how far the search got plus the bytes
   the source's component percent-encodes. There is deliberately no "verified":false — the entry states what was
   searched, never that the sink is safe. An array for the same reason the @H surface is one — result.h owns the
   document. */
char *solve_json_array(void);
int   solve_count(void);

#endif
