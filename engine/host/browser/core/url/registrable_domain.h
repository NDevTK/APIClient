/* HTML §7.1.1.2 Relaxing the same-origin restriction — "is a registrable domain suffix of or is equal to".
 *
 * WHY IT IS ITS OWN COMPONENT AND NOT A STATIC IN THE SETTER THAT FIRST NEEDED IT. The algorithm is DEFINED
 * inside §7.1.1.2 and is not private to it: Cookie Store API §7.2 "Set a cookie" step 12.3 refuses a `Domain`
 * that "is not a registrable domain suffix of and is not equal to host" by citing this algorithm BY NAME, so
 * two standards share one definition and this tree must hold one implementation of it. A second copy is one
 * fact stated twice over a 10239-rule table that changes several times a week, which is the shape that drifts,
 * and a wrong answer about which registrable domain a host belongs to is a security answer rather than a near
 * miss.
 *
 * WHY IT LIVES BESIDE THE PUBLIC SUFFIX LIST AND NOT BESIDE `document.domain`. The algorithm is stated over
 * URL hosts and over the PSL, and NEITHER of its two consumer layers owns the other: `core/dom` runs the
 * setter, `core/cookie_store` and `core/loader` run the cookie steps. Housing it in any one of them makes the
 * others depend on that layer for a question that is about hosts. It is the same place a browser engineer
 * already looks — Chromium keeps `GetDomainAndRegistry` and its registry-controlled-domain predicates in
 * `net/base/registry_controlled_domains/`, next to the list rather than next to the DOM. */
#ifndef ENGINE_HOST_BROWSER_CORE_URL_REGISTRABLE_DOMAIN_H
#define ENGINE_HOST_BROWSER_CORE_URL_REGISTRABLE_DOMAIN_H

#include <stdbool.h>
#include <stddef.h>

#include "core/url/url.h"

/* §7.1.1.2: "To determine if a scalar value string hostSuffixString is a registrable domain suffix of or is
   equal to a host originalHost", verbatim, over `s`/`n` as hostSuffixString and `original` as originalHost.

   `*out_suffix` RECEIVES THE PARSE of hostSuffixString — step 2's `hostSuffix` — because every caller of this
   algorithm needs it after a true answer and re-running the parser for it would be a second answer to one
   question: §7.1.1.2's own step 6 sets the origin's domain to exactly this host. It is ZEROED on entry and is
   the caller's to `url_host_free` on EVERY path, true and false alike, so a caller has one cleanup rather
   than a rule about which answers own memory.

   BOTH ARGUMENTS ARE ANSWERED, NEVER ASSERTED. `s`/`n` is PAGE-SUPPLIED input at every call site there is —
   an assignment to `document.domain`, a `Domain` attribute — so an empty, unparseable or IP-address value is
   step 1's and step 3's own `return false` and never a `DCHECK`. What this component may assert is what it
   COMPUTED: its own parse, and the public suffix table's agreement with the algorithm at step 4.4. */
bool registrable_domain_suffix_or_equal(const char *s, size_t n, const UrlHost *original, UrlHost *out_suffix);

#endif
