/* MEDIA QUERIES — MEDIA QUERIES LEVEL 4, the grammar in §3 and the media features in §4 THROUGH §7, not §4
 * alone: §4 is the viewport's own dimensions and the display, colour and interaction features are §5, §6 and
 * §7. Several rows of the table are LEVEL 5's outright and two of them MOVED between the levels, so
 * media_query.c names the level per row rather than here.
 *
 * A media query is a PREDICATE OVER THE ENVIRONMENT, and this file is the whole of it: the parser that turns
 * the page's string into that predicate, the serializer CSSOM VIEW §4.2's `MediaQueryList.media` answers with,
 * and the evaluation against the modelled viewport and output device. It knows nothing about `matchMedia`,
 * about MediaQueryList, or about update-the-rendering step 10 — those are CSSOM VIEW's and live in
 * media_query_list.c. One problem per file, and the problem here is the language.
 *
 * WHY THE EVALUATION IS A PLAIN `bool` AND THE CSSOM ANSWER IS NOT. CLAUDE.md states the modelling exactly:
 * `matchMedia` resolves a default viewport for its `.matches` EXAMPLE yet stays CONCOLIC. Those are two
 * different jobs. `media_query_matches` RESOLVES — it runs the real predicate against the modelled environment
 * and answers what MQ4 §4 says a viewport of this size gives — and that is what keeps this component testable
 * against the standard, because a value that is opaque for control flow cannot be compared with the spec's own
 * answer. `media_query_matches_value` is the other job: the CSSOM-facing value, that resolution carried as the
 * EXAMPLE of a concolic keyed on the DOCUMENT.
 * IT IS SPELLED HERE RATHER THAN IN EACH MEMBER THAT REPORTS IT, and that is not a layering slip — it is the
 * only way the answer is ONE fact. `matchMedia('(min-width:600px)').matches` and whether a
 * `@media (min-width:600px)` rule applies to this document are the same question about the same environment, so
 * they must fork on the same key; two spellings of one identity fork one predicate twice and let the arm that
 * answered `true` for the MediaQueryList resolve the cascade as though it were false. It lived in
 * media_query_list.c alone only while `matchMedia` was the one member that asked.
 *
 * THREE-VALUED LOGIC IS THE SPEC AND NOT A REFINEMENT (§3.1). A `<general-enclosed>` — `(bogus: 1)`, `foo(1)` —
 * parses but is UNKNOWN, and unknown propagates by Kleene's rules: `not unknown` is unknown, `unknown and
 * false` is FALSE, `unknown or true` is TRUE. Collapsing unknown to false at the leaf gets those last two
 * backwards, which is a page taking the wrong branch rather than a pedantic difference. A media query that
 * evaluates to unknown does not match.
 *
 * A QUERY THAT DOES NOT MATCH THE GRAMMAR IS REPLACED BY `not all` (§3.1) — the QUERY, not the list, so
 * `@media (min-width: 600px), (bogus))` keeps its first query. That is the spec's forward-compatible parsing
 * rule and it is why nothing here throws: an unparseable media query is not an error, it is a query that never
 * matches, and `matchMedia("(")` returns a MediaQueryList whose `.media` is "not all". */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_MEDIA_QUERY_H
#define ENGINE_HOST_BROWSER_CORE_CSS_MEDIA_QUERY_H

#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"

typedef struct MediaQuerySet MediaQuerySet;

/* Parse a `<media-query-list>`. NEVER NULL for a non-NULL input: every failure mode the grammar has is a query
   replaced by `not all`, so there is no parse error to report and no caller that has to handle one. An EMPTY
   string is a valid list of no queries, which §3.1 says matches everything — `matchMedia("").matches` is true
   in every browser. */
MediaQuerySet *media_query_parse(const char *text);
void           media_query_free(MediaQuerySet *set);

/* CSSOM §"serialize a media query list", as a malloc'd C string the caller frees. */
char *media_query_serialize(const MediaQuerySet *set);

/* MQ4 §3: does this list match the environment of `ctx`'s realm? A list matches when ANY of its queries does,
   and the environment is read PER REALM (viewport.h) because a child navigable's viewport is its own. */
bool media_query_matches(JSContext *ctx, const MediaQuerySet *set);

/* HOW MANY QUERIES the list holds, and CSSOM §4.2's SERIALIZE A MEDIA QUERY for ONE of them — §4.4's `length`
   and `item(index)`, which ask about the collection's MEMBERS rather than about its serialization as a whole.
   `media_query_serialize_at` is OWNED, and NULL for an index at or past the count, which is exactly the null
   §4.4's `item` returns there. */
int   media_query_count(const MediaQuerySet *set);
char *media_query_serialize_at(const MediaQuerySet *set, int i);

/* MQ4 §3.1's PARSE A MEDIA QUERY — ONE query, not a list, and NULL where the value does not match the grammar.
   That null is the whole reason it is separate from media_query_parse: the forward-compatible rule replaces a
   bad query IN A LIST with `not all`, while CSSOM §4.4's `appendMedium` and `deleteMedium` are stated over the
   single-query parse and BOTH return early on its null — so a list-shaped parse would append `not all` for
   `appendMedium('!!')` where the spec appends nothing at all. OWNED: media_query_free. */
MediaQuerySet *media_query_parse_one(const char *text);

/* MQ4 §3's `<media-condition>` ALONE — the production, not `<media-query>`, and NULL where the text does not
   match it or does not consume the whole of it. It is a THIRD parse entry rather than a flag on the two above
   because it accepts a strictly SMALLER language than either: a bare `<media-type>` (`screen`, `print`) is a
   valid media query and is NOT a media condition, so a caller that wants a condition and asks for a query
   accepts input the grammar it is implementing rejects.
   HTML §4.8.4.3.11 "Parsing a sizes attribute" step 3.5 is that caller — "Parse the remaining component values
   in unparsed size as a `<media-condition>`" — and a `sizes="screen 100vw"` that this answered TRUE for would
   pick a source size no browser picks. OWNED: media_query_free, like the two above; it is held as a query with
   no media type, which is exactly §3's first arm of `<media-query>`, so the evaluators below answer it with no
   second code path. */
MediaQuerySet *media_query_parse_condition(const char *text);

/* MQ4 §4's `<length>` in CSS pixels, over a DIMENSION's own number and unit — css-values-4 §6.2's absolute
   units, §6.1.1's font-relative ones resolved against the INITIAL values §6.1.1 itself prescribes outside the
   context of an element, §6.1.2's viewport-percentage ones in all four of §6.1.2.1's spellings against THIS
   realm's viewport, and CSS Conditional 5 §7's container-relative ones through §7's own no-eligible-container
   fallback. FALSE for a unit this table does not resolve.
   THAT SET IS NOT THE SAME QUESTION AS `css_length_is_length_unit`, WHICH IS §6's `<length>` PRODUCTION, and a
   production is necessarily the wider of the two: it admits every unit any specification defines as a length
   precisely so that one this engine cannot absolutize is a MISSING COMPONENT and not a syntax error. So the
   two part again the day a specification defines a new unit, and a caller that has to tell "not a length" from
   "a length this engine cannot absolutize yet" asks `css_length_is_length_unit` for the difference — reporting
   the second as the first turns a missing component into the page's own parse error. What the difference IS at
   any revision is derived rather than recorded; media_query.c states the two commands beside the table.
   IT IS EXPORTED FOR HTML §4.8.4.3 "Processing model", which states in one sentence that a source size's units
   "must be interpreted the same as in Media Queries" — so core/html/image_source_set.c asks this table rather
   than carrying a second copy of it. `unit_len` 0 is css-values-4 §6's UNITLESS case, which is a `<length>`
   only for zero. The answer is a plain `double` — the modelled EXAMPLE — for the reason the header above gives
   for `media_query_matches`: the C side resolves and the concolic is minted at the JS boundary. */
bool media_query_length_px(JSContext *ctx, double n, const char *unit, size_t unit_len, double *px);

/* THE ANSWER AS A CSSOM MEMBER REPORTS IT — the resolution above carried as the EXAMPLE of a concolic keyed on
   THIS document's answer to THIS query list. See the header note for why one fact is spelled once.
   `media_query_matches_now` is the ENGINE's own non-forking read of that same value: C cannot fork, so it takes
   the arm this flow already committed to (solver/decide.h) and falls back to the modelled example where the
   flow has committed to neither. The cascade and update-the-rendering both read through it.

   RESIDUAL — THE PIN THAT READ CONSUMES HAS NO PRODUCER BUT THE PAGE.
   NOT COVERED: a media condition no CSSOM member ever hands to page code. This predicate is minted at exactly
   one call, `media_query_matches_value`, and reaches a branch seam only through the two members that return
   one — `MediaQueryList.matches` and `CSSMediaRule.matches` — so a condition reached ONLY from a sheet (an
   `@media` prelude, a `media` content attribute) is read here and asked nowhere. `decide_value_arm` then
   answers -1 for it in every flow of the document, and every one of them resolves it to the same modelled
   example: the arm CLAUDE.md §Solver-half keeps for a domain admitting both outcomes, unkept for want of an
   asker rather than because anything contradicted it.
   THE NEXT DIFF BUILDS THE ASKER, NOT A SECOND READER. Forking at a READ is already refused with its reason
   (core/css/css_style_sheet.h: a forking read there would fork at every computed-value ask); that refusal is
   about the SITE, it is correct, and it stays. What is missing is one ask per document per condition from a
   point that can carry a sibling — `solver_decide_restartable` (solver/decide.h), whose contract is engine
   code re-reached by re-running the flow's scheduler step. This read is then already its consumer.
   AND THIS CLAUSE NAMED A MECHANISM AND NO SITE, WHICH IS HOW IT WAS READ AS NAMING THE WRONG ONE — recorded
   here rather than silently repaired, because a reader who re-derives a site from `one ask per document per
   condition` reaches the same wrong one. The natural reading is HTML §8.1.7.3 update the rendering STEP 10,
   which is where a document's media queries are already walked. IT CANNOT HOLD THIS ASK. That walk is inside
   the update-the-rendering STEP MACHINE, whose driver is an ordinary `JS_CFUNC_step` C function enqueued as a
   call task, and `js_call_c_function` sets `rt->current_stack_frame` before it dispatches — so
   `JS_HasActivation` is TRUE for the whole of that machine and the seam's second precondition fires by name
   (`a restartable fork was asked for while page code was on the stack`, solver/engine.c, whose own text
   already lists `a listener inside a rendering step`). Its FIRST precondition passes, which is what makes the
   wrong site look right: a C activation leaves the flow's frame handle NULL.
   THE SITE THAT DOES HOLD IT is HTML §8.1.7.3's IN-PARALLEL half — core/rendering/rendering.c's
   `rendering_run_opportunity`, which the scheduler calls as a rung of the flow's own step with nothing of the
   flow on any stack. It is not a hopeful candidate: it ALREADY asks this seam, through
   core/timing/event_loop.h's `event_loop_before`, and has done since before this residual was written. The
   ask goes at its TOP, above `event_loop_advance_to` and the task enqueue, because the contract is that the
   whole computation is re-reached by re-running the step — a sibling assembled below those would advance the
   clock twice and queue a second rendering task.
   AND A STEP MACHINE IS NOT WITHOUT A SEAM, WHICH IS THE OTHER HALF A READER NEEDS: it has its own, and a
   different one. `step_tobool_run` (quickjs-step.h) is the BRANCH seam asked from inside a machine — it keys
   on the VALUE'S OWN branch identity, which is exactly the key `decide_value_arm` below reads, and it returns
   JS_STEP_FORK for the driver to snapshot. So a machine-side ask is buildable; it is simply not
   `solver_decide_restartable`, and the two are not interchangeable.
   THE ORDERED SUBPROBLEMS, because this is more than one landing. (1) A media feature's value must be a FACT
   two queries can both narrow, or an asker that mints every condition a sheet holds populates environments no
   user agent has — stated with its own three clauses at `mq_source` in media_query.c, and first because the
   motivating pages carry a `dark` block AND a `light` block. (2) The ENUMERATION: which conditions a document
   holds. Nothing answers it today; the cascade's own walk is the honest set, since a condition no cascade
   consults steers nothing. (3) The ask itself, at the site above. Each of (2) and (3) is inert without the
   other — a recorded set nothing drains is a write with no reader — so they are ONE landing and not two.
   RETIREMENT: this record goes when that ask exists, which retires the whole residual.
   ITS ABSENCE SHOWS wherever every flow of one document emits the same cascade text for a sheet whose only
   environment dependence is an `@media` block over a feature the table in media_query.c gives more than one
   legal value.
   RETIRED BY: that ask existing. */
JSValue media_query_matches_value(JSContext *ctx, const MediaQuerySet *set);
bool    media_query_matches_now(JSContext *ctx, const MediaQuerySet *set);

#endif
