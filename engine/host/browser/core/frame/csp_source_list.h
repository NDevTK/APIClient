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
 * AND §6.7.3.3 IS HERE BECAUSE THE ENGINE STOPPED ASKING ONLY THE ATTACKER'S QUESTION. This file used to
 * carry, in place of this paragraph, an argument that §6.7.3.3's remaining arms were unreachable: its nonce
 * arm needs an element carrying a `nonce` the page's own policy lists, its hash arms need the source to hash
 * to a listed digest, and its 'strict-dynamic' arm needs an element that is not parser-inserted — none of
 * which INJECTED content can have. Every word of that is still true and it answered the wrong question the
 * moment a BROWSER component asked. HTML §4.2.6's update a style block runs §4.2.3 upon the page's OWN
 * `<style>` element with its own child text content, and `<style nonce=…>` under `style-src 'nonce-…'` is one
 * of the two shapes modern CSP is actually written in — so answering it from §6.7.3.2 alone refuses a sheet
 * every browser applies, and the whole cascade below it resolves from a document real Chrome does not have.
 * §6.7.3.2 is therefore no longer the answer; it is §6.7.3.3's FIRST STEP, and the caller decides nothing.
 *
 * AND STEP 5.2.2 IS ANSWERED RATHER THAN CRASHED. It needs SHA-256/384/512 over the source, which this engine
 * now has as a component of its own (core/crypto/secure_hash.h, a port of FIPS PUB 180-4), reached through the
 * same one door Web Cryptography §14.3.5's digest() goes through. What stood here instead was a DFAIL naming
 * the primitive to build — deliberately, because "Does Not Match" would have been indistinguishable from a
 * computed answer and wrong for exactly the page whose own inline block the hash was published for. The
 * primitive exists, so the crash is gone with it. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_CSP_SOURCE_LIST_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_CSP_SOURCE_LIST_H
#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

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

/* §6.7.3.2 "does a source list allow all inline behavior for type?" — true for "Allows". It is §6.7.3.3's
   step 1 and is exposed beside it because it is the one arm of that algorithm that reads NOTHING but the list:
   a fixture can state `'unsafe-inline' 'strict-dynamic'` against it with no element and no source to build. */
bool csp_source_list_allows_all_inline(const CspDirective *directive, CspInlineType type);

/* §6.7.3.1 "Is element nonceable?" — "Nonceable" if a nonce-source expression can match `element`.
   NULL IS AN ELEMENT-SHAPED ANSWER AND NOT A HOLE: §4.2.4 runs the inline check "upon null" for a javascript:
   navigation, and the solver asks about markup that has not been inserted anywhere, so both arrive here with
   no element. Step 1 already answers them — a thing with no `nonce` attribute is "Not Nonceable" — so the
   null case needs no branch of its own and gets none.
   §6.7.3.1's step 3, "if element had a duplicate-attribute parse error during tokenization", is NOT
   implemented and cannot be: it needs an HTML tokenizer hook the standard's own note (whatwg/html Issue
   #3257) says does not exist, and Lexbor records no such error on the element. Its absence makes this engine
   MORE permissive than Chrome for malformed markup that repeats an attribute — the mitigation §7.2.1
   dangling markup attacks names, not a policy decision — and it is stated here rather than asserted because
   the condition is not observable from a parsed tree, so there is nothing a DCHECK could read. */
bool csp_element_is_nonceable(const lxb_dom_element_t *element);

/* §6.7.3.3 "Does element match source list for type and source?" — the WHOLE of what decides whether inline
 * content of a given type runs, and the algorithm every directive's §6.1.x inline check ends in.
 *
 * `element` may be NULL — see csp_element_is_nonceable. `source` is §6.7.3.3's source string: the element's
 * child text content for "script"/"style", the attribute's value for the two attribute types, and the URL for
 * "navigation". It is BORROWED, is not NUL-terminated, and is the ONLY input to the hash arm.
 *
 * IT TAKES BYTES AND NOT A JS STRING, because §6.7.3.3 step 5.1 is "UTF-8 encode the result of JavaScript
 * string converting on source" and every caller in this engine already holds UTF-8: Lexbor's text content and
 * attribute values are UTF-8, and so is a serialized URL. A caller holding a JSValue converts at its own site,
 * where the realm that owns the string is in scope. */
CspMatch csp_element_match_source_list(const CspDirective *directive, const lxb_dom_element_t *element,
                                       CspInlineType type, const char *source, size_t source_len);

/* §6.7.2.3 "Does nonce match source list?" AND §6.7.2.4 "Does integrity metadata match source list?", ASKED OF
 * THE LIST ALONE — the half of each algorithm that is settled before the request is looked at.
 *
 * NEITHER IS THE WHOLE ALGORITHM AND NEITHER STANDS IN FOR ONE. §6.7.2.3's step 3 iterates the list looking
 * for an expression that "matches the nonce-source grammar", and its step 4 is "Return Does Not Match" —
 * so a list carrying no nonce-source answers "Does Not Match" for EVERY nonce, and the request's cryptographic
 * nonce metadata cannot change it. §6.7.2.4 says the same thing one step earlier and out loud: its step 2 is
 * "Let integrity expressions be the set of source expressions in source list that match the hash-source
 * grammar" and its step 3 is "If integrity expressions is empty, return Does Not Match".
 *
 * THEY ARE FACTS ABOUT THE LIST, AND THE CALLER ASKS ITS OWN QUESTION OF THEM. What a pre-request check asks
 * here is "is this step's answer settled without a request field I do not carry"; where the answer is `true`
 * the field is REQUIRED and the caller must have it. That is why the bit is exposed rather than a ready-made
 * verdict: a `csp_nonce_matches(list, "")` would take an empty string nobody computed and hand back a
 * plausible "Does Not Match" for exactly the policy whose answer depends on bytes the request never carried. */
bool csp_source_list_has_nonce_source(const CspDirective *directive);
bool csp_source_list_has_hash_source(const CspDirective *directive);

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
   grammar case in (`*`, `https:`, `*.example.com`, a port wildcard `https://x:*`, a path `https://x/a/`,
   `'self'`) without building a directive around each one. `expression` is one token of §2.3's value.
   THE PORT WILDCARD AND THE PATH ARE NAMED SEPARATELY, and not as the one example that shows both, because
   `https://x:*` immediately followed by `/a/` spells `*` `/` inside a block comment — which ENDS IT. That is
   not a style note: it is why this file did not compile, and the three errors it produced named a type `a`,
   an unexpected `§`, and an undeclared `csp_source_match_url` — none of which mentions a comment. */
CspMatch csp_source_match_url(CspToken expression, const UrlRecord *url, const Origin *self_origin,
                              int redirect_count);

/* §2.3.1's `scheme-source / host-source` — is this token one of those two productions? It is the RECOGNISER
 * half of §6.7.2.8's step 2/3 arm, exposed because ANOTHER STANDARD names that pair as its own production and
 * has to ask before it stores: Permissions Policy §5.1 "HTML attribute serialization" defines
 * `permissions-source-expression = scheme-source / host-source`, and its §9.2 "Construct policy from
 * dictionary and origin" appends an element to an allowlist only "if element is a valid
 * permissions-source-expression".
 *
 * IT IS EXPORTED RATHER THAN RE-DERIVED FOR THIS FILE'S OWN REASON — one reading of the grammar. A second
 * recogniser next door would not merely duplicate: it would be a recogniser whose author has not read
 * §6.7.2.8, and the two would disagree in the direction that WIDENS a policy. The concrete case is the
 * quoted keywords, which are the whole point of asking: `'self'`, `'src'` and `'none'` are §5.1
 * allow-list-values and are NOT permissions-source-expressions, and this predicate refuses all three (a `'` is
 * neither a scheme's first character nor a host-char) — while §6.7.2.8's step 4 would happily match `'self'`
 * if such a string were stored as an expression, turning a keyword the caller already handled into a second,
 * unintended match. */
bool csp_source_is_scheme_or_host_source(CspToken expression);

#endif
