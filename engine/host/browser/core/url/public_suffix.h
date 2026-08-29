/* THE PUBLIC SUFFIX LIST — URL §3.2's "obtain the public suffix of a host", over the list itself.
 *
 * WHY IT IS A COMPONENT AND NOT A LINE IN url.c. §3.2 defines the algorithm; the PSL defines the MATCHING (its
 * own §Format and §Algorithm: ordinary rules, wildcard rules that consume one whole leftmost label, exception
 * rules that outrank every other match, the most-labels tie-break, and the prevailing rule `*` when nothing
 * matches at all). That is one assertable contract with one fixture, and the DATA it is stated over is a
 * 10239-rule table that changes several times a week — so it is vendored at a pinned revision and generated
 * into public_suffix_table.h by engine/pslgen.mjs, for the reason §Testing gives about frozen snapshots: a gate
 * whose input can change under it is measuring the day it ran.
 *
 * WHO ASKS. HTML §7.1.1.2's `document.domain` setter, through the "is a registrable domain suffix of or is
 * equal to" algorithm that is the whole of its step 4 — the condition that lets `www.example.com` relax to
 * `example.com` and refuses to let it claim `com`. There is no answer to that without the list: `com`,
 * `github.io`, `*.compute.amazonaws.com` and `!www.ck` are all rules a hand-written table of TLDs gets wrong,
 * and they are precisely the cases the condition exists for.
 *
 * THE TWO SIDES ARE ONE REPRESENTATION. The list ships U-labels in UTF-8; a host arriving here has been through
 * URL §3.3 IDNA's domain parser and is a lowercased A-label. The PSL's formal algorithm requires both sides to be
 * canonicalized "lower-case, Punycode (RFC 3492) - prior to being compared", so pslgen.mjs punycodes every rule
 * at generation time and STOPS on one it cannot. Matching at run time is therefore plain bytes; a rule left as a
 * U-label would have matched nothing, silently, for exactly the IDN domains this matters most for. */
#ifndef ENGINE_HOST_BROWSER_CORE_URL_PUBLIC_SUFFIX_H
#define ENGINE_HOST_BROWSER_CORE_URL_PUBLIC_SUFFIX_H

#include "core/url/url.h"

/* URL §3.2: "To obtain the public suffix of a host host: 1. If host is not a domain, then return null. 2. Let
   trailingDot be `.` if host ends with `.`; otherwise the empty string. 3. Let publicSuffix be the public suffix
   determined by running the Public Suffix List algorithm with host as domain. 4. Assert: publicSuffix is an
   ASCII string that ends with trailingDot. 5. Return publicSuffix."
   NULL is step 1 — an IP address and an opaque host have no public suffix, which is a real answer and the one
   §7.1.1.2's table gives for `[0::1]`.
   OTHERWISE IT POINTS INTO `h`'s OWN DOMAIN STRING and is borrowed for as long as that host lives. The public
   suffix is by construction "the set of labels FROM THE DOMAIN which match the labels of the prevailing rule",
   so it is always a suffix of the input — including its trailing dot, which is how step 4's assertion holds
   without a second string being built (`example.com.` answers `com.`, and §Note 3 says it must). */
const char *public_suffix_of(const UrlHost *h);

#endif
