/* HTML §4.2.5.3 "Pragma directives" — the CONTENT SECURITY POLICY STATE (`http-equiv="content-security-policy"`).
 * See html_meta_csp.c.
 *
 * WHY THIS IS A COMPONENT AND NOT A LOOP BODY. A `<meta>` element delivering a policy is an ALGORITHM with
 * five numbered steps, and two of them are refusals nothing that merely scans a tree for `http-equiv` will
 * ever perform: step 1 returns for a `meta` that is not a CHILD OF A `head` ELEMENT, and step 4 REMOVES the
 * `report-uri`, `frame-ancestors` and `sandbox` directives from the policy before it is enforced. Both
 * refusals point the same way — a browser enforces LESS than the markup says — so an engine that skips them
 * enforces a policy no browser applies, and for this project that is the expensive direction: CLAUDE.md §@S
 * requires a breakout to be judged under the page's ACTUAL policy, so an over-strict answer reports a REAL
 * exploit as CSP-blocked and drops it. A body-placed `<meta http-equiv=Content-Security-Policy>` is exactly
 * the shape an HTML injection produces, which is to say the shape that appears on the pages this tool is
 * pointed at.
 *
 * STEP 5 IS DELIBERATELY NOT HERE. §4.2.5.3's last step is "Enforce the policy policy", and WHERE a policy is
 * enforced is the caller's fact: a Document's CSP list is what holds it, and which Document that is follows
 * from the operation that reached this element. Answering the POLICY and leaving the enforcement to the
 * caller is also the seam this engine's document creation order needs — see the note in html_meta_csp.c on
 * §7.5.1 — so that the same one call serves a walk over an already-parsed tree and, once the Document record
 * exists before its parse does, the parser's own insertion step. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HTML_META_CSP_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HTML_META_CSP_H

#include <lexbor/dom/dom.h>

/* §4.2.5.3's CONTENT SECURITY POLICY STATE, steps 1 through 4, for ONE node — the policy this node DELIVERS,
 * §2.2-serialized, or NULL for a node that delivers none.
 *
 * IT TAKES A NODE RATHER THAN A `meta` ELEMENT, and that is the interface and not a convenience. §4.2.5.3 is
 * a PRAGMA DIRECTIVE, which is selected by the element's own `http-equiv` attribute: "is this the content
 * security policy state" is the algorithm's first act, so a caller asked to establish it first would be
 * running the selection this file exists to own. A `<div>`, a text node and a `<meta charset>` are therefore
 * all the same answer — NULL, this node delivers no policy — and the caller is a filter-free line at every
 * site: a walk over a parsed tree today, and the tree builder's own element-insertion step once a Document
 * exists before its bytes are parsed.
 *
 * THE RESULT IS A NUL-TERMINATED STRING THE CALLER FREES, and it is a POLICY rather than a CSP list: §2.2
 * makes U+002C the list delimiter, so a caller composing several `<meta>` policies into one list joins them
 * with a comma (which is what several `<meta>` elements ARE — independently enforced policies), and a caller
 * appending to a live list appends one policy.
 *
 * NAMED RESIDUAL — a directive VALUE containing U+002C survives this function and is then mis-read by
 * whatever re-parses the composed list. §2.2.1 gives a comma no meaning inside a policy, so `foo-src a,b` is
 * one directive here; a container that carries its list as one comma-joined string (policy_container.h) re-
 * parses that as two policies and enforces the tail INDEPENDENTLY, which narrows the document further than
 * its own markup does. The next diff is a container holding CspPolicy values rather than one joined string —
 * §2.2 already models a list as a list of policies, so the transport is the only thing in the way. Its
 * absence shows as a page whose `<meta>` policy contains a comma being blocked by a directive it never
 * declared on its own; step 4 above removes `report-uri`, which is where a comma most often appears, so what
 * is left is narrow rather than absent. */
char *html_meta_csp_policy(const lxb_dom_node_t *node);

#endif
