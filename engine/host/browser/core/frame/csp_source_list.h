/* CSP §2.3.1's SOURCE LIST — the grammar of a source expression, and the questions a list of them answers.
 * See csp_source_list.c.
 *
 * WHY THE GRAMMAR AND ITS MATCHING ARE ONE COMPONENT. §2.3.1 defines what a source expression IS and §6.7
 * defines what one MATCHES, and the two are one contract read from either end: a recogniser with no matcher
 * classifies for nobody, and a matcher that re-derives its own idea of the grammar is the second reading this
 * engine has just finished deleting (see csp_directive_list.h). They also only ever change together — adding
 * 'wasm-unsafe-eval' to keyword-source is meaningless until some check asks a list for it. So one file, and it
 * grows by ANSWER: what is here is what a caller asks.
 *
 * WHAT ARRIVED WITH §6.7.2, AND WHY IT IS THE SAME FILE. This header used to say the URL-matching half would
 * arrive "with the caller that has a URL to match (Fetch's main fetch, through §4.1.2)", and it has: §4.1.2 is
 * answered from core/frame/policy_container.c and reaches csp_source_list_match_url below. That brought the
 * three §2.3.1 productions the inline half never needed — scheme-source, host-source and its decomposition
 * into scheme-part / host-part / port-part / path-part — because §6.7.2.8 is written entirely in terms of
 * them. They are here rather than next door for the reason at the top: a matcher that re-derived the grammar
 * would be the second reading of it, and §6.7.2.8's arms ARE the grammar's alternatives read one at a time.
 *
 * WHAT §6.7.3.2 MEANS FOR THIS ENGINE, AND WHY IT IS TOTAL. The question the solver asks — would an injected
 * inline handler, script block or javascript: URL actually RUN — used to abort on any source expression the
 * parser did not model, which is every real policy: a nonce, a hash, a host source, `https:`. That crash was
 * not naming a missing capability, it was naming a question the file had INVENTED. §6.7.3.2 classifies a
 * source list by exactly three tests — is this expression a nonce-source or a hash-source, is it
 * 'strict-dynamic', is it 'unsafe-inline' — and every other expression, host source and scheme source
 * included, is IGNORED BY THE ALGORITHM. So the answer is total over every policy the web sends, with nothing
 * left to model and nothing left to crash on.
 *
 * The one step of §6.7.3.3 that can still let inline content run once §6.7.3.2 has said no is its
 * 'strict-dynamic' clause, and it requires an element that is NOT parser-inserted. Injected markup is always
 * parser-inserted — that is what innerHTML and document.write produce — and a javascript: navigation has no
 * element at all, so no sink class in this engine can reach it. The clause becomes reachable the day a sink
 * assigns to `.text` on a script element the PAGE created, and it arrives with that sink. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_CSP_SOURCE_LIST_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_CSP_SOURCE_LIST_H
#include <stdbool.h>

#include "core/frame/csp_directive_list.h"
#include "core/url/url.h"

/* §6.7's TWO ANSWERS, named as the algorithms name them rather than as a bool — the same reason
   core/fetch/port_blocking.h names its two: "Matches" and "Does Not Match" are the spec's own words, and a
   caller reading a bare truth value has an even chance of reading the polarity backwards with nothing to say
   so, which for this question means a page's own scripts silently blocked or an attacker's silently allowed. */
typedef enum {
    CSP_DOES_NOT_MATCH = 0,
    CSP_MATCHES = 1,
} CspMatch;

/* §2.3.1: nonce-source = "'nonce-" base64-value "'". ABNF string literals are CASE-INSENSITIVE (RFC 5234
   §2.3), so `'NONCE-abc'` is one of these too. The base64-value is not decoded and never has to be — §6.7.2.3
   compares nonces as strings, which the standard says explicitly. */
bool csp_source_is_nonce(CspToken expression);

/* §2.3.1: hash-source = "'" hash-algorithm "-" base64-value "'", hash-algorithm = sha256 / sha384 / sha512. */
bool csp_source_is_hash(CspToken expression);

/* "source list contains a source expression which is an ASCII case-insensitive match for the string X" — the
   phrasing §4.4.1, §4.5.1 and §6.7.1.1 are all written in. `keyword` includes its quotes, because the quotes
   are part of the keyword-source production and not punctuation around it. */
bool csp_source_list_contains(const CspDirective *directive, const char *keyword);

/* §6.7.3.2 "does a source list allow all inline behavior for type?" — true for "Allows". This is the whole of
   what decides whether an INJECTED inline handler, script block or javascript: URL executes; see the header
   note above for why the rest of §6.7.3.3 cannot change that answer for content an attacker supplies. */
bool csp_source_list_allows_all_inline(const CspDirective *directive, CspInlineType type);

/* §6.7.2.7 "does url match source list in origin with redirect count?" — the whole of what decides whether a
 * REQUEST may be made, and the algorithm every fetch directive's pre-request check ends in.
 *
 * THE SOURCE LIST IS A DIRECTIVE'S VALUE, so this takes the directive: §2.3's value IS the split this model
 * already holds, and handing over the tokens alone would lose the ability to assert what they came from.
 * `directive` is non-NULL — a policy carrying no governing directive says NOTHING about the request, which is
 * the CALLER's answer to give (§6.8.4 decides that, and it is not "a list that matches nothing").
 *
 * `self_origin` IS THE CSP LIST'S, never the document's — see CspList's field. It is required, because
 * `'self'` is in almost every real policy and an absent origin would silently make every one of them match
 * nothing; §6.7.2.8's arms assert it where they read its components.
 *
 * `redirect_count` IS THE REQUEST'S, and it is the one input here that is not about the policy: §6.7.2.8 drops
 * the PATH comparison entirely once a request has been redirected, because the path a redirect landed on is
 * the server's choice and not the page's. A fresh request has 0, which is what makes `script-src
 * https://cdn/a/b.js` mean the path it says. */
CspMatch csp_source_list_match_url(const CspDirective *directive, const UrlRecord *url,
                                   const Origin *self_origin, int redirect_count);

/* §6.7.2.8 "does url match expression in origin with redirect count?" — ONE source expression, exposed beside
   the list because it is the whole of §6.7.2.7's body and because it is the unit a fixture can state a
   grammar case in (`*`, `https:`, `*.example.com`, `https://x:*/a/`, `'self'`) without building a directive
   around each one. `expression` is one token of §2.3's value. */
CspMatch csp_source_match_url(CspToken expression, const UrlRecord *url, const Origin *self_origin,
                              int redirect_count);

#endif
