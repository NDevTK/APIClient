/* MEDIA QUERIES — MEDIA QUERIES LEVEL 4, the grammar in §3 and the media features in §4.
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

/* THE ANSWER AS A CSSOM MEMBER REPORTS IT — the resolution above carried as the EXAMPLE of a concolic keyed on
   THIS document's answer to THIS query list. See the header note for why one fact is spelled once.
   `media_query_matches_now` is the ENGINE's own non-forking read of that same value: C cannot fork, so it takes
   the arm this flow already committed to (solver/decide.h) and falls back to the modelled example where the
   flow has committed to neither. The cascade and update-the-rendering both read through it. */
JSValue media_query_matches_value(JSContext *ctx, const MediaQuerySet *set);
bool    media_query_matches_now(JSContext *ctx, const MediaQuerySet *set);

#endif
