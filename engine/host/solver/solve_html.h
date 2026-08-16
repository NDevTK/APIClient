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
 * the opening quote for an attribute value. The state is read off the REAL Lexbor parse, and the one fact the
 * DOM does not record — WHICH of §13.2.5.36/.37/.38 an attribute value is in — is asked of the same real parser
 * by DIFFERENTIAL RE-PARSE rather than by scanning backwards for a quote by hand (`<div title="a > b" id={}>`
 * defeats every hand scan; it does not defeat the tokenizer).
 *
 * A STATE WITH NO ESCAPE RULE CRASHES, NAMING ITSELF. That is the work queue: a DFAIL here says which
 * §13.2.5 state the attacker's bytes are in and therefore exactly what has to be built. It is NOT the same
 * thing as a search that has not solved — a derived escape that does not fire is an ordinary parked @S search
 * (CLAUDE.md forbids asserting on that), while a context this file cannot NAME is an unbuilt capability. */
#ifndef ENGINE_HOST_SOLVER_SOLVE_HTML_H
#define ENGINE_HOST_SOLVER_SOLVE_HTML_H

#include <stddef.h>

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
   a sink that writes into one has no HTML breakout to construct and its search is honestly parked. */
int solve_html_breakouts(const char *output, SolveHtmlEmit emit, void *user);

#endif
