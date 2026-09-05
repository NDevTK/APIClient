/* HTML §7.1.1.2 Relaxing the same-origin restriction's "is a registrable domain suffix of or is equal to".
 * See registrable_domain.h for why this is a component and why it sits beside the list rather than beside the
 * `document.domain` setter that first needed it. */
#include <string.h>

#include "check.h"
#include "core/url/public_suffix.h"
#include "core/url/registrable_domain.h"
#include "core/url/url.h"

/* "X, PREFIXED BY U+002E (.), MATCHES THE END OF Y" — the phrase §7.1.1.2 uses three times, as one function so
   the three cannot drift. It is a strict test: `example.com` does not match the end of `example.com` (there is
   no room for the dot), which is what makes step 4.3's first disjunct and its second disjunct different
   questions. */
static bool dot_suffix_of(const char *x, const char *y)
{
    size_t xn = strlen(x), yn = strlen(y);

    return yn > xn && y[yn - xn - 1] == '.' && memcmp(y + yn - xn, x, xn) == 0;
}

/* THE PUBLIC SUFFIX IS THE POINT OF THE ALGORITHM. Steps 4.1 and 4.2 alone would let `www.example.com` claim
   `com`, which is the shared-hosting attack §7.1.1.2's own warning is about; step 4.3 is what refuses it, and
   it cannot be answered without the list (core/url/public_suffix.h). Its two disjuncts are different refusals:
   the first refuses a value that IS a public suffix (`com`, `github.io`, `bar.ck` under `*.ck`), the second
   refuses one that sits inside the original host's public suffix without being the whole of it. */
bool registrable_domain_suffix_or_equal(const char *s, size_t n, const UrlHost *original, UrlHost *out_suffix)
{
    const char *sd, *od, *ps;

    DCHECK(original != NULL, "§7.1.1.2's algorithm was asked about no originalHost — every caller reads one off "
                             "a record this engine built, so a null is this codebase's own logic and not a "
                             "page's value");
    DCHECK(out_suffix != NULL, "§7.1.1.2's algorithm was given nowhere to put step 2's parse, which every "
                               "caller needs after a true answer and none may re-derive");
    memset(out_suffix, 0, sizeof *out_suffix);
    if (n == 0) return false;                                        /* step 1 */
    if (!url_parse_host(out_suffix, s, n, /*is_opaque*/ false))      /* steps 2-3 */
        return false;
    if (url_host_equal(out_suffix, original)) return true;           /* step 4's condition, and step 5 */
    /* STEP 4.1 — "if hostSuffix or originalHost is not a domain, then return false. This excludes hosts that
       are IP addresses." An IP is equal to itself and to nothing else, which is why step 4's `0.0.0.0` row
       passes above and never reaches here. */
    if (out_suffix->kind != URL_HOST_DOMAIN || original->kind != URL_HOST_DOMAIN) return false;
    sd = out_suffix->domain;
    od = original->domain;
    if (!dot_suffix_of(sd, od)) return false;                        /* step 4.2 */
    /* STEP 4.3's FIRST DISJUNCT — "hostSuffix equals hostSuffix's public suffix" — which is the question RFC
       6265 §5.3 Storage Model's step 5 also asks about a cookie's `Domain`, so it is asked through the ONE
       spelling the list component exports rather than re-composed here out of public_suffix_of and a strcmp. */
    if (host_is_public_suffix(out_suffix)) return false;
    /* STEP 4.3's SECOND DISJUNCT. `original` is a domain, so URL §3.2 gives it a public suffix rather than
       null — which is why the string is taken directly here and not through the predicate above. */
    ps = public_suffix_of(original);
    DCHECK(ps != NULL, "URL §3.2 answered null for the original host after §7.1.1.2 step 4.1 established it is "
                       "a domain");
    if (dot_suffix_of(sd, ps)) return false;
    /* STEP 4.4's ASSERT, which the standard states and which is a real invariant of the three tests above: the
       value is a strict dotted suffix of the original (4.2), it is not itself a public suffix (4.3a), and it
       does not sit inside the original's public suffix (4.3b) — so the original's public suffix is a strict
       dotted suffix of it. A failure here is the PSL and this algorithm disagreeing, not a page's input. */
    DCHECK(dot_suffix_of(ps, sd),
           "§7.1.1.2 step 4.4's assert failed: the original host's public suffix is not a dotted suffix of the "
           "value, after the three tests that make it one. The public suffix table and this algorithm disagree "
           "— re-read engine/pslgen.mjs's matching against §Algorithm before trusting either");
    return true;                                                     /* step 5 */
}
