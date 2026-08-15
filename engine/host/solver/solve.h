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
     fire-verified  `{"sink":..,"source":..,"poc":..,"firesOn":..[,"cspBlocks":".."][,"trustedTypes":"script"]
                      [,"sourceEncodes":".."][,"delivery":".."][,"deliveryPrefix":"#"]}`
     parked search  `{"sink":..,"source":..,"search":"parked","tried":N
                      [,"sourceEncodes":".."][,"delivery":".."][,"deliveryPrefix":"#"]}`
   The parked shape exists because absence is never a "safe" verdict: a sink an attacker source REACHES is
   reported whether or not its breakout has been solved, and it carries how far the search got plus the source's
   own declaration. There is deliberately no "verified":false — the entry states what was searched, never that
   the sink is safe. An array for the same reason the @H surface is one — result.h owns the document.

   THE FIRED ENTRY IS §S(d)'s REPRODUCTION ENVELOPE, and every field of it is a POSITIVE statement whose ABSENCE
   is equally positive — never a gap to be read as a default:
     `firesOn`      what makes the PoC RUN, off the sink's own fire oracle: "sink-evaluates" (the sink executes
                    the string where it stands), "parse-insert" (an auto-firing handler in markup, at insertion,
                    no interaction), "navigation" (a `javascript:` URL, when the navigation happens). ALWAYS
                    present — a PoC that does not say how it fires is not reproducible.
     `cspBlocks`    the page's serialized CSP, present only when it kills THIS vector. Absent = policy_allows
                    said yes.
     `trustedTypes` the CSP sink GROUP required at this sink, present only when the document requires one — the
                    assignment throws before the markup is parsed. Absent = no requirement applies, which
                    covers both "the document requires none" and "the standard makes this no TT sink".
     `delivery`     HOW an attacker puts bytes in this source, from the source's own declaration in the
                    component that owns it: "address" (the victim's own URL, at `deliveryPrefix`), "plant"
                    (§S(b)'s TWO-STAGE plant-then-load — there is no separate `stored` flag because being
                    stored is not a fact beside the mechanism, it IS the mechanism), "referring-address" (the
                    payload rides the address the victim arrives FROM), "user-file". ABSENT = the source declared
                    none, which is what server-injected page state is: the attacker writes it directly and no
                    component carries or transforms it. A consumer states that; it never guesses a vector.
   The engine owns this whole vocabulary. A delivery layer switches on these tokens and may say it cannot
   PERFORM one, but it never decides which source uses which — that was a `{hash}|{search}|{pm}|{reply}` table
   in the offscreen matching a display shape this engine has never emitted, which is why live verify could not
   build a PoC for any finding it produces. */
char *solve_json_array(JSContext *ctx);
int   solve_count(void);

#endif
