/* MEDIA QUERIES — MEDIA QUERIES LEVEL 4, the grammar in §3 and the media features in §4.
 *
 * A media query is a PREDICATE OVER THE ENVIRONMENT, and this file is the whole of it: the parser that turns
 * the page's string into that predicate, the serializer CSSOM VIEW §4.2's `MediaQueryList.media` answers with,
 * and the evaluation against the modelled viewport and output device. It knows nothing about `matchMedia`,
 * about MediaQueryList, or about update-the-rendering step 10 — those are CSSOM VIEW's and live in
 * media_query_list.c. One problem per file, and the problem here is the language.
 *
 * WHY THE EVALUATION IS A PLAIN `bool` AND NOT A CONCOLIC. CLAUDE.md states the modelling exactly: `matchMedia`
 * resolves a default viewport for its `.matches` EXAMPLE yet stays CONCOLIC. Those are two different jobs and
 * they belong to two different layers — this file RESOLVES (it runs the real predicate against the modelled
 * environment and answers what the spec says it is), and the member that the page reads is what carries the
 * concolic, with this answer as its example. Minting the concolic here instead would put a solver decision
 * inside the CSS language, and it would also make this component untestable against the standard: a value that
 * is opaque for control flow cannot be compared with the answer MQ4 §4 says a viewport of this size gives.
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

#endif
