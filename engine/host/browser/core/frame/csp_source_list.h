/* CSP §2.3.1's SOURCE LIST — the grammar of a source expression, and the questions a list of them answers.
 * See csp_source_list.c.
 *
 * WHY THE GRAMMAR AND ITS MATCHING ARE ONE COMPONENT. §2.3.1 defines what a source expression IS and §6.7
 * defines what one MATCHES, and the two are one contract read from either end: a recogniser with no matcher
 * classifies for nobody, and a matcher that re-derives its own idea of the grammar is the second reading this
 * engine has just finished deleting (see csp_directive_list.h). They also only ever change together — adding
 * 'wasm-unsafe-eval' to keyword-source is meaningless until some check asks a list for it. So one file, and it
 * grows by ANSWER: what is here is what a caller asks, and the questions §6.7.2's URL matching answers arrive
 * with the caller that has a URL to match (Fetch's main fetch, through §4.1.2), not before.
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

#endif
