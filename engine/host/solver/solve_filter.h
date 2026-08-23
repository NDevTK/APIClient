/* THE @S FILTER-SURVIVAL OBSERVATION — how much of the candidate's own bytes the page's OWN code let through.
 *
 * CLAUDE.md §@S names four fitness rungs the WFQ reads — {filter-survived, sink-reached, context-escaped,
 * handler-fires} — and this file is the FIRST of them. Its two siblings answer the CONTEXT question for the
 * class that owns the sink's language (solve_js.c reads ECMAScript §12 "ECMAScript Language: Lexical Grammar",
 * solve_html.c reads a real Lexbor parse); this one answers the question that comes BEFORE either of them and
 * has no language in it at all: of the bytes a candidate flow was given at the source, which are still here.
 *
 * WHY IT IS A DISTANCE AND NOT A BOOLEAN, WHICH IS THE WHOLE POINT. §@S(2) asks for "per-flow
 * CHARACTER/SEGMENT PROVENANCE through the real filter — which bytes survive to which positions and in what
 * form […] observed per re-execution". What the sinks had instead was `strstr(output, LOCATOR)`: a candidate
 * whose bytes the page's filter mangled by ONE character was indistinguishable from one whose bytes never
 * arrived, and from one that was never scheduled. Three different states, one answer, and the answer was
 * silence — no derivation, no credit, and no statement anywhere that a filter is what ate the candidate.
 *
 * IT OBSERVES, IT DOES NOT PREDICT. The measurement runs on the string a REAL re-execution of the page handed a
 * REAL sink, so every concatenation, every `replace`, every re-encode and every allowlist the page loaded at
 * runtime has already happened to it. It is NOT the recorded transform-expression §Re-execution forbids and it
 * is not a taint tracker: nothing is carried on the value, no operator is hooked, and this file cannot be asked
 * anything until a run has produced an actual string.
 *
 * WHAT "SURVIVED" MEANS IS EXACT, AND THE FORMS §@S(2) LISTS ARE NOT A CHECKLIST THIS FILE OWES. A byte the
 * page re-encoded (`&lt;`, `%3C`) or dropped is a byte that CANNOT break the sink out of its context, so for a
 * fitness the two are one answer and that answer is "did not survive" — reporting `&lt;` as a surviving `<`
 * would be the false-PoC direction. What §@S(2)'s vocabulary is FOR is the mutation step, which reads WHICH
 * segment died and WHERE the rest landed; both are reported here (`at`, `out_at`) so that step has its input.
 * Case folding is deliberately absent: whether a case-folded run still counts is a fact about the SINK's own
 * language (HTML §13.2.5 lowercases tag and attribute names, ECMAScript §12 does not), so it belongs to the
 * class that owns the sink and not to a component that has never heard of either. */
#ifndef ENGINE_HOST_SOLVER_SOLVE_FILTER_H
#define ENGINE_HOST_SOLVER_SOLVE_FILTER_H

#include <stddef.h>

/* THE LONGEST CONTIGUOUS RUN OF THE CANDIDATE THAT IS STILL PRESENT, and the three facts a mutation needs
   beside it. A run and not a count of matching characters: the escape a breakout is made of is a SEQUENCE, so
   twelve of its bytes scattered through the output are worth nothing and four of them adjacent are worth
   something, and a per-character tally cannot tell those apart. */
typedef struct {
    int len;      /* the candidate's own byte length — the denominator, so `run` is readable without it */
    int run;      /* the longest contiguous run of the candidate present in the output, 0 when none is */
    int at;       /* where that run starts IN THE CANDIDATE (which segment lived), or -1 when run == 0 */
    int out_at;   /* where it landed IN THE OUTPUT (where it survived to), or -1 when run == 0 */
} FilterObs;

/* MEASURE `cand` against `out`. Both are NUL-terminated; `out` is the exact string a re-execution handed a
   sink, `cand` the exact bytes that flow injected at its source.
   COST IS O(|out| x |cand|) WITH A PRUNE, and it introduces no suspend obligation this call site does not
   already have: the sinks that call it already run `strstr` over the same string and already re-parse it with
   the real HTML parser. `cand` is bounded by the buffer a constructed breakout is built in, not by the page. */
void solve_filter_survival(const char *out, const char *cand, FilterObs *o);

#endif
