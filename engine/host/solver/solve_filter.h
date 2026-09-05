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
 * segment died and WHERE the rest landed; both are reported here (`at`, `out_at`).
 *
 * WHO READS THEM, STATED RATHER THAN ASSUMED — because this sentence used to end "so that step has its input",
 * which is a claim about a CONSUMER and was true of nobody. The offsets were computed on every observation,
 * checked by this file's own two-sided assert (which RE-READS the bytes at them — the only thing that can say
 * the pair NAMES the run rather than merely being in range), and then discarded with the caller's `FilterObs`
 * local, which reads `run` and `len` and nothing else. §@S: "an observation with a computed writer and no
 * reader is not a mechanism" — the mirror of the read-with-no-writer defect, and harder to see, because
 * nothing was absent and nothing defaulted. They now reach solve.c's search record (`surv_at`/`surv_out`,
 * written in the same ratchet branch as the run they describe) and the parked report (`survivedAt`/
 * `survivedTo`), where a reader acts on the DIRECTION of a near miss: a run at offset 0 is a payload whose
 * TAIL the page cut, one at offset 3 is a payload whose HEAD it ate, and those are opposite mutations that the
 * size alone reports identically.
 *
 * WHAT IS STILL NARROWER THAN §@S(2), NAMED SO THE NEXT DIFF STARTS FROM BUILDING IT RATHER THAN FROM
 * REDISCOVERING IT. The DERIVATION does not read these offsets. solve.c re-derives after a near miss from the
 * context probe's stored WITNESS under the byte-deliverability table below, so it aims by WHICH BYTES arrive
 * and never by WHERE this candidate's own surviving segment landed. That is correct as far as it goes — every
 * escape it constructs is still the sink state's own exit transition — and it is narrower than "breakout bytes
 * are placed where they survive in the surviving form". WHAT THE NEXT DIFF BUILDS is an offset-addressed
 * derivation entry, the shape solve_js_at_source already has (a string plus a position, rather than a string
 * the callee scans for a locator), so a breakout's OWN output can be read at `out_at` as a witness in its own
 * right. HOW ITS ABSENCE SHOWS: a search whose `survivedAt` WALKS — successive candidates losing a different
 * segment each time — while `payloads` stops growing, because the run measured a new gap on every arrival and
 * nothing was constructed toward it; the delivery table did not move, and it is the only thing the derivation
 * is listening to.
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
/* A RUN IS AT LEAST TWO BYTES, OR IT IS NOTHING — the boundary of the sentence above and not a threshold on
   top of it. One character of a payload carries no positional information: every byte a constructed escape is
   made of occurs in ordinary markup and ordinary source on its own, so a run of 1 is exactly the per-character
   tally this field exists not to be. TWO is the only non-arbitrary floor, because it is the shortest length at
   which "contiguous" constrains anything; any larger value would be a tuning knob. A candidate shorter than
   two bytes is reported as itself, since for it the distinction does not arise. */
typedef struct {
    int len;      /* the candidate's own byte length — the denominator, so `run` is readable without it */
    int run;      /* the longest contiguous run of the candidate present in the output; 0, or 2 and up */
    int at;       /* where that run starts IN THE CANDIDATE (which segment lived), or -1 when run == 0 */
    int out_at;   /* where it landed IN THE OUTPUT (where it survived to), or -1 when run == 0 */
} FilterObs;

/* MEASURE `cand` against `out`. Both are NUL-terminated; `out` is the exact string a re-execution handed a
   sink, `cand` the exact bytes that flow injected at its source.
   COST IS O(|out| x |cand|) WITH A PRUNE, and it introduces no suspend obligation this call site does not
   already have: the sinks that call it already run `strstr` over the same string and already re-parse it with
   the real HTML parser. `cand` is bounded by the buffer a constructed breakout is built in, not by the page. */
void solve_filter_survival(const char *out, const char *cand, FilterObs *o);

/* WHICH BYTES OF A CANDIDATE REACH A SINK AS THEMSELVES — the OTHER half of §@S's "solved JOINTLY", and the
 * half a derivation was constructing without.
 *
 * §@S(1) is the sink's parse context and §@S(2) is "per-flow CHARACTER/SEGMENT PROVENANCE through the real
 * filter — which bytes survive to which positions and in what form (dropped/escaped/case-folded/re-encoded/
 * moved/split), observed per re-execution […] so breakout bytes are placed where they survive in the surviving
 * form". A derivation that reads only the first constructs escapes out of bytes that cannot arrive: the
 * fragment percent-encode set (URL §1.3 "Percent-encoded bytes") holds SPACE, `"`, `<`, `>` and `` ` ``, so
 * `'><svg onload=X9()>` is unsatisfiable through `location.hash` BY CONSTRUCTION and the search reported it as
 * an escape that merely did not fire. This table is what a derivation asks instead, and every §13.2.5 / §12
 * exit that has ALTERNATIVE spellings is then chosen by it rather than written down once.
 *
 * IT IS OBSERVED, NEVER ASSUMED, WHICH IS WHY IT IS A TABLE AND NOT THE DECLARED PERCENT-ENCODE SET. A page
 * that runs `decodeURIComponent` over its own fragment receives `<` even though the browser encoded it, so the
 * declaration is a PRIOR and not a fact: taking it as the constraint would decline the one markup breakout this
 * engine already fires. What the declaration decides is WHICH BYTES ARE WORTH MEASURING (solve.c builds its
 * delivery probe out of exactly that set); what the run decides is the answer.
 *
 * SOUND-ONLY, IN THE SAME DIRECTION §Solver-half prunes branches: a byte is cleared only by something that
 * CONTRADICTS its arrival, so a source whose probe has not come back yet keeps every arm. An over-tight table
 * declines an escape that would have fired; an over-loose one merely spends a document re-run.
 * TWO THINGS CONTRADICT AN ARRIVAL AND THEY ARE NOT THE SAME KIND OF FACT, which is why the paragraph above
 * refuses the declaration and this one does not contradict it. A RUN is what settles the ENCODE column, for
 * exactly the reason given: the browser percent-encoded the byte on the way in and the page may decode it
 * back, so the declaration is a prior. What a CARRIER REFUSES is settled by the declaration alone — a byte
 * RFC 6265 §4.1.1's cookie-octet excludes and which the plant does not percent-encode either never enters the
 * page's program in ANY form, so there is nothing for a page-side transform to recover and no run can widen
 * it. concolic.c owns that distinction (it owns the column both halves are read from) and states it
 * at one call, concolic_source_carrier_bytes; solve.c asks it where a search learns its root. The declared
 * ENCODE bytes are still never cleared here, and solve.c asserts that after every seed — pre-clearing them
 * would refuse the delivery probe, which is BUILT out of exactly those bytes.
 * A SEEDED TABLE IS NOT A MEASURED ONE, and nothing may read it as one: `deliv_seen` remains the search's
 * answer to whether a run has observed anything, and the report's measured set is emitted only under it.
 * (Neither paragraph above may gain a double quote. The fragment-set listing beside the URL §1.3 citation
 * higher up spells U+0022 inside a code span, which is an UNBALANCED quote to any reader that pairs them, so
 * the next quotation added below it is swallowed as a several-hundred-word continuation of that citation and
 * reported as fabricated. Both paragraphs above therefore say cookie-octet and delivery-probe without
 * quoting either.) */
typedef struct SolveDelivered { unsigned char ok[256]; } SolveDelivered;

/* EVERYTHING DELIVERS — the initial state of a search's table, and all a search can honestly hold before it
   knows its own source: the carrier half is seeded from the declaration the moment the root arrives, and the
   encode half waits for a run. */
void solve_delivered_all(SolveDelivered *d);
/* Does this one byte reach the sink as itself? Used where a derivation CHOOSES between two spellings of one
   §13.2.5 / §12 exit transition. */
int  solve_delivered_byte(const SolveDelivered *d, char c);
/* …and the same question about a whole constructed escape — the gate every emitted breakout passes, and the
   two-sided assert the seeder re-states about what it was handed. */
int  solve_delivered_ok(const SolveDelivered *d, const char *s);

#endif
