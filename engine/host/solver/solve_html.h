/* THE @S HTML-CONTEXT BREAKOUT, DERIVED FROM THE SINK'S OWN PARSE — never picked from a list.
 *
 * CLAUDE.md §@S: "The breakout is DERIVED from the parsed sink context for EVERY sink class — no fixed payload
 * table. HTML breakout from the real Lexbor parse context (construct_ctx_breakout)." What stood here instead
 * was CANDS_HTML, five guessed payloads sprayed at every HTML sink, and its defect is not that it was ugly:
 * a sink whose lexical context none of the five fitted was UNSOLVABLE BY CONSTRUCTION and the search could not
 * say so — it reported `parked, tried 5` with no statement of what it had failed to escape.
 *
 * WHAT IS PARSED IS THE SINK'S ACTUAL OUTPUT, NOT A TEMPLATE. §@S(2) requires the context to be read "per
 * re-execution (never a static transform-expression, which cannot see a config-loaded allowlist)", so the
 * derivation runs on the string a REAL run of the page handed the sink, with a locator standing where the
 * attacker's bytes landed. The candidate machinery already provides exactly that: the first candidate flow of
 * an HTML sink's search injects SOLVE_HTML_LOCATOR at the source instead of a breakout, the page's own
 * filters and concatenations run, and the sink observes where the locator survived to. The static shape
 * (`"<img src=" + {hash} + ">"`) would answer a different and weaker question — it is the transform-expression
 * §Re-execution forbids, and it cannot see the filter that moved, dropped or re-encoded the bytes.
 *
 * WHAT IT ANSWERS IS A TOKENIZER STATE (WHATWG HTML §13.2.5), and the escape is the MINIMAL byte sequence that
 * state's own transition table defines as its exit — `-->` for the comment state, `</textarea>` for RCDATA,
 * the opening quote for an attribute value, `]]>` for a CDATA section. The state is read off the REAL Lexbor
 * parse, and the facts the DOM does not record are asked of that same real parser by DIFFERENTIAL RE-PARSE
 * rather than scanned for by hand. There are TWO of them and they are asked the same way — splice in the byte
 * the states disagree about and re-parse:
 *   - WHICH of §13.2.5.36/.37/.38 an attribute value is in. A hand scan backwards for the opening quote is
 *     defeated by the first `<div title="a > b" id={}>` it meets; the tokenizer is not.
 *   - WHETHER FOREIGN TEXT IS INSIDE A CDATA SECTION (§13.2.5.69). Its "Anything else" is "Emit the current
 *     input character as a character token", so the HTML parser never builds a CDATASection node and both
 *     states put their bytes in a TEXT node — the tree cannot tell them apart, and a `<` spliced in behind
 *     the locator becomes an ELEMENT in exactly one of them.
 *
 * A STATE WITH NO ESCAPE RULE CRASHES, NAMING ITSELF. That is the work queue: a DFAIL here says which
 * §13.2.5 state the attacker's bytes are in and therefore exactly what has to be built. It is NOT the same
 * thing as a search that has not solved — a derived escape that does not fire is an ordinary parked @S search
 * (CLAUDE.md forbids asserting on that), while a context this file cannot NAME is an unbuilt capability.
 *
 * AND THE LINE BETWEEN THOSE TWO IS DRAWN BY WHOSE BYTES STATE THE VALUE, NOT BY HOW MUCH IS BUILT. What is
 * parsed here is the string the PAGE'S OWN CODE handed the sink, so the parse's OUTCOME is not a value this
 * engine computed and may not be asserted on: `<div id=a id={hole}>` (§13.2.5.33 removes the duplicate
 * attribute from the token), a DOCTYPE outside the initial insertion mode, and an unterminated tag whose
 * eof-in-tag discards it all put the locator in a token that builds NO NODE, and each is ordinary markup a
 * page emits. Those answer 0 — a parked search — and the residual is named at the site. What still crashes is
 * a node kind this file's own walk produced and does not name, which is a statement about this codebase. */
#ifndef ENGINE_HOST_SOLVER_SOLVE_HTML_H
#define ENGINE_HOST_SOLVER_SOLVE_HTML_H

#include <stddef.h>
#include "solver/solve_filter.h"   /* the DELIVERABILITY half of §@S's joint solve — see that header */

/* THE LOCATOR the context probe injects at the source. ASCII alphanumeric and nothing else, which is the whole
   of its design: it is inert in EVERY §13.2.5 state (it starts no tag, closes no quote, ends no comment), so
   its presence cannot change the parse it is being used to measure, and it survives a filter that strips
   punctuation — the filters that would otherwise make the probe report a context the page never produces. */
#define SOLVE_HTML_LOCATOR "apiclientprobe"

/* THE OTHER OCCURRENCES, when the page writes the source's value more than once. Each occurrence sits in its
   OWN tokenizer state and gets its own derived escape, so each is measured with the others renamed to this —
   same length, so every byte offset in the output still means what it meant. */
#define SOLVE_HTML_ELIDE   "apiclientelide"

/* THE FILLER ATTRIBUTE NAME an attribute-context escape ends on, so the template's own closing quote opens an
   empty attribute value instead of landing inside the injected handler (`onerror=X9()"` is a syntax error;
   `onerror=X9() x9pad=""` is not). */
#define SOLVE_HTML_PAD     "x9pad"

/* One constructed breakout. TEXT, never an index: a candidate flow carries its payload as bytes all the way to
   the cold tier and back, so a recipe parked this session must still mean the same thing to a build whose
   tables have changed (CLAUDE.md §cold: the sink crosses as its NAME for the identical reason). */
typedef void (*SolveHtmlEmit)(void *user, const char *breakout);

/* DERIVE every HTML-context breakout for `output` — the concrete string a probe run handed the sink, holding
   SOLVE_HTML_LOCATOR wherever the attacker's bytes survived to. Calls `emit` once per constructed breakout and
   returns how many. ZERO is a real answer and not a failure: §13.2.5.5's PLAINTEXT state has no exit at all, so
   a sink that writes into one has no HTML breakout to construct and its search is honestly parked.
   `d` IS THE OTHER HALF OF THE SOLVE AND IT IS NOT ADVICE — §@S requires the three observations to be solved
   JOINTLY, so an exit transition is chosen from among the spellings §13.2.5 gives it BY which of them this
   source can actually carry, and nothing is emitted that it cannot. §13.2.5.39 "After attribute value (quoted)
   state" is the worked example: it leaves on whitespace AND on U+002F SOLIDUS, no percent-encode set in URL
   §1.3 "Percent-encoded bytes" contains the solidus, and every set contains SPACE — so the second spelling is
   the one a fragment- or query-carried payload has, and with only the first written down the whole family was
   reported as an escape that did not fire.
   THE SAME CALL IS THE MUTATION STEP. The table is a MEASUREMENT (solve_filter.h), so it changes when a run
   observes a byte failing to arrive; re-calling this on the SAME witness under the changed table is §@S's
   "a near-miss is mutated toward the gap using byte-provenance", and it needs no second search and no retry
   count — what it constructs joins the one search and the next drain seeds it. */
int solve_html_breakouts(const char *output, const SolveDelivered *d, SolveHtmlEmit emit, void *user);

#endif
