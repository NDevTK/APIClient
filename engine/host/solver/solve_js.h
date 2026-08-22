/* THE @S JS-CONTEXT BREAKOUT, DERIVED FROM THE SINK'S OWN LEXICAL STATE — never picked from a list.
 *
 * CLAUDE.md §@S: "The breakout is DERIVED from the parsed sink context for EVERY sink class — no fixed payload
 * table. […] JS breakout from a real lexical-context detector (solve_js.c → construct_js_breakout: the hole's
 * string/template/comment state decides the minimal escape)." What stood here instead was CANDS_JS, five
 * guessed payloads sprayed at every eval sink, and its defect is the one CANDS_HTML had: a sink whose lexical
 * state none of the five fitted was UNSOLVABLE BY CONSTRUCTION and the search could not say so — it reported
 * `parked, tried 5` with no statement of what it had failed to escape. Two of the five could never fit any
 * state at all (a hole inside a template needs `${`, not a backtick that closes a literal whose terminator is
 * still ahead in the source), and the state that IS a template's substitution got no candidate of its own.
 *
 * WHAT IS SCANNED IS THE SINK'S ACTUAL ARGUMENT, NOT ITS SHAPE. §@S(2) requires the context to be read "per
 * re-execution (never a static transform-expression, which cannot see a config-loaded allowlist)", so the
 * derivation runs on the string a REAL run of the page handed `eval`, with SOLVE_JS_LOCATOR standing where the
 * attacker's bytes survived to. The candidate machinery already provides exactly that: the first candidate flow
 * of an eval sink's search injects the locator at the source instead of a breakout, the page's own filters,
 * concatenations and re-encodings run, and the sink observes the result. The concolic DISPLAY SHAPE
 * (`"'" + {hash} + "'"`) would answer a weaker and different question — it is the transform-expression
 * §Re-execution forbids, and it cannot see the filter that moved, dropped or re-encoded the bytes.
 *
 * WHAT IT ANSWERS IS AN ECMAScript §12 LEXICAL STATE (https://tc39.es/ecma262/multipage/
 * ecmascript-language-lexical-grammar.html), and the escape is the byte sequence that grammar defines as
 * leaving it — `'` for §12.9.4 SingleStringCharacters, `${` for §12.9.6 TemplateCharacters, the asterisk-slash
 * terminator for §12.4's MultiLineComment, `/` for §12.9.5 RegularExpressionBody. Nothing longer is emitted,
 * and the three bytes that are not the exit itself each answer a §12 rule rather than a preference: the `;`
 * because §12.10's automatic semicolon insertion applies only across a LineTerminator (so a same-line resume
 * needs one and the line comment's resume does not), the trailing `//` because §12.4 makes a single-line
 * comment run to the end of the line and the literal's ORIGINAL terminator is on it, and the re-opened
 * MultiLineComment because §12.4 makes that production non-nesting: the page's own terminator is still ahead,
 * and a Script that does not parse cannot fire.
 *
 * THE ENGINE'S OWN TOKENIZER CANNOT ANSWER THIS, which is why a §12 scanner lives here and is not a second
 * implementation of one that exists. quickjs's `next_token` DISCARDS comments (`goto redo` at both comment
 * arms) — a hole inside `//` is precisely a state it never reports — and it does not decide §12.1's goal
 * symbol at all: `js_parse_regexp` is called by the PARSER (`js_parse_primary_expr`), so the tokenizer alone
 * never knows whether a `/` opened a RegularExpressionLiteral. Both facts are what this file must report.
 *
 * A STATE WITH NO ESCAPE RULE CRASHES, NAMING ITSELF. That is the work queue, and it is NOT the same thing as
 * a search that has not solved — a derived escape that does not fire is an ordinary parked @S search (CLAUDE.md
 * forbids asserting on that), while a context this file cannot NAME is an unbuilt capability. */
#ifndef ENGINE_HOST_SOLVER_SOLVE_JS_H
#define ENGINE_HOST_SOLVER_SOLVE_JS_H

#include <stddef.h>

/* THE LOCATOR the eval sink's context probe injects at the source. ASCII alphanumeric and nothing else, which
   is the whole of its design: it opens no §12 state (it is not a quote, a backtick, a slash or a backslash), so
   its presence cannot change the scan it is being used to measure, and it survives every percent-encode set a
   source's browser delivery applies — the transforms that would otherwise make the probe report a context the
   page never produces.
   DISTINCT FROM SOLVE_HTML_LOCATOR, and neither is a substring of the other (solve_init asserts it): a page
   that writes one attacker source into BOTH an eval sink and a markup sink runs one class's probe past the
   other class's sink, and a shared token would make that sink derive a context for a search that never asked
   for it. The two tokens ARE the partition. */
#define SOLVE_JS_LOCATOR "apiclientjsprobe"

/* One constructed breakout. TEXT, never an index into a table: a candidate flow carries its payload as bytes
   all the way to the cold tier and back (cold.c parks `cand_payload` as hex for exactly this reason), so a
   recipe parked this session must still mean the same thing to a build whose tables have changed. */
typedef void (*SolveJsEmit)(void *user, const char *breakout);

/* DERIVE every JS-context breakout for `output` — the concrete string a probe run handed the eval sink, holding
   SOLVE_JS_LOCATOR wherever the attacker's bytes survived to. Calls `emit` once per constructed breakout and
   returns how many.
   THERE IS NO ELIDE TOKEN HERE and that is a property of §12, not an omission: the scanner is addressed by the
   locator's OFFSET, so a page that writes the source twice gets each occurrence scanned in its own state with
   the others left in place — and leaving them in place changes nothing, because an alphanumeric run is an
   IdentifierName in source and an ordinary character everywhere else. solve_html.c must rename them only
   because its `locate` walks a tree and returns the FIRST match. */
int solve_js_breakouts(const char *output, SolveJsEmit emit, void *user);

#endif
