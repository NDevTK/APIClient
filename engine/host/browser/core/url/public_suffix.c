/* URL §3.2's PUBLIC SUFFIX, over the PSL's own §Algorithm. See public_suffix.h for why this is a component and
 * why the table is vendored rather than fetched. */
#include <string.h>

#include "check.h"
#include "core/url/public_suffix.h"
#include "core/url/public_suffix_table.h"

#define PSL_N(a) (sizeof (a) / sizeof *(a))

/* A RULE AGAINST A SUFFIX THAT IS NOT NUL-TERMINATED — the whole reason this is not strcmp. The suffixes being
   looked up are the tails of one domain string, so each is (pointer, length) rather than its own allocation:
   `example.com.` asks about `com` while the byte after it is a dot. strncmp already stops at `s`'s NUL if it has
   one; what it cannot say is that a rule which merely STARTS with those n bytes is longer, so that is decided
   after it. Orders the way strcmp does, which is the order pslgen.mjs sorted the arrays in. */
static int psl_cmp(const char *rule, const char *s, size_t n)
{
    int c = strncmp(rule, s, n);

    if (c != 0) return c;
    return rule[n] ? 1 : 0;
}

static bool psl_has(const char *const *rules, size_t nrules, const char *s, size_t n)
{
    size_t lo = 0, hi = nrules;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = psl_cmp(rules[mid], s, n);

        if (c == 0) return true;
        if (c < 0) lo = mid + 1;
        else       hi = mid;
    }
    return false;
}

/* The start of the label AFTER the one beginning at `at`, or `core` when there is none. §Definitions: "A domain
   or rule can be split into a list of labels using the separator `.`". Walked rather than collected into an
   array, because an array of label starts is a length this file would have to bound and §scheduler bans that:
   a domain is whatever the parser produced, at whatever depth. */
static size_t psl_next_label(const char *d, size_t core, size_t at)
{
    while (at < core && d[at] != '.') at++;
    return at < core ? at + 1 : core;
}

const char *public_suffix_of(const UrlHost *h)
{
    const char *d;
    size_t n, core, i, next, last;

    DCHECK(h != NULL, "URL §3.2 was asked for the public suffix of no host");
    /* STEP 1 — "if host is not a domain, then return null". An IPv4 or IPv6 address, an opaque host and the
       empty host have no labels for a rule to match, and this is the answer §7.1.1.2's own table gives for
       `[0::1]`: not a failure, a null. */
    if (h->kind != URL_HOST_DOMAIN) return NULL;
    DCHECK(h->domain != NULL, "a host parsed as a DOMAIN carries no domain string — url.c's §4.2 parser sets "
                              "the two together and a host with one and not the other came from neither");
    d = h->domain;
    n = strlen(d);
    /* STEP 2's trailingDot, held OUT of the matching and back IN by the return. §Note 3: "looking up
       example.com should yield com while looking up example.com. should yield com." — and because the answer is
       a pointer into `d`, the dot comes back for free rather than by building a second string. */
    core = (n > 0 && d[n - 1] == '.') ? n - 1 : n;

    /* §Algorithm's THIRD bullet FIRST: "if more than one rule matches, the prevailing rule is the one which is
       an exception rule". An exception outranks every ordinary and wildcard match at every label depth, so no
       ordinary rule may even be considered until every exception has been. Within this pass the LEFTMOST start
       is the match with the most labels, which is the fourth bullet's tie-break. */
    for (i = 0; i < core; i = next) {
        next = psl_next_label(d, core, i);
        if (psl_has(PSL_EXCEPTION, PSL_N(PSL_EXCEPTION), d + i, core - i)) {
            /* "If the prevailing rule is a exception rule, modify it by removing the leftmost label", and the
               public suffix is then the labels of the domain that match what is left. */
            DCHECK(next < core, "an exception rule matched a domain's LAST label — §Format defines an exception "
                                "as \"an exception to a previous wildcard rule\", so its body always carries "
                                "the label the wildcard consumed and cannot be one label long");
            return d + next;
        }
    }
    /* Then the ordinary and wildcard rules, most labels first for the same reason. */
    for (i = 0, last = 0; i < core; i = next) {
        next = psl_next_label(d, core, i);
        last = i;
        if (psl_has(PSL_NORMAL, PSL_N(PSL_NORMAL), d + i, core - i))
            return d + i;
        /* A WILDCARD RULE `*.X` MATCHES HERE when X is exactly the labels to the RIGHT of this one: §Format
           restricts the wildcard to the leftmost position and to a whole label, so it consumes THIS label and
           nothing else. It therefore needs a label to consume, which is what `next < core` says. */
        if (next < core && psl_has(PSL_WILDCARD, PSL_N(PSL_WILDCARD), d + next, core - next))
            return d + i;
    }
    /* §Algorithm's SECOND bullet: "if no rules match, the prevailing rule is `*`" — one label, so the public
       suffix is the domain's right-most one. This is the answer for every made-up TLD a page can invent, and it
       is why `document.domain = "example"` on the host `example` is allowed while `= "com"` is not. */
    return d + last;
}

/* THE PREVAILING RULE `*` IS WHY THIS PREDICATE ANSWERS TRUE FOR A SINGLE-LABEL HOST. `localhost`, an intranet
   name and every made-up TLD match no rule, so §Algorithm's second bullet makes each its OWN public suffix —
   which is a fact about the list and not an edge case to route around. Its two callers then differ in what
   they do with it, and each is right: HTML §7.1.1.2 Relaxing the same-origin restriction reaches step 4.3 only
   AFTER step 4's equality test has already returned true for a value equal to the host, and RFC 6265 §5.3
   Storage Model's step 5 asks that equality itself, in its own first arm, precisely so that `Domain=localhost`
   on `localhost` is stored host-only rather than dropped. A caller that takes only a reject arm here breaks
   every single-label host there is. */
bool host_is_public_suffix(const UrlHost *h)
{
    const char *ps = public_suffix_of(h);

    return ps != NULL && strcmp(h->domain, ps) == 0;
}
