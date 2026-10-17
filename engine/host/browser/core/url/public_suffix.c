/* URL §3.2's PUBLIC SUFFIX, with Chromium's matching over Chromium's list. See public_suffix.h for why this is a
 * component and why the table is vendored rather than fetched. */
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

/* Which arrays hold `s` (n bytes) as a rule body: the tags Chromium's list stores for one entry. */
static void psl_tags(const char *s, size_t n, bool *normal, bool *wildcard, bool *exception)
{
    *normal = psl_has(PSL_NORMAL, PSL_N(PSL_NORMAL), s, n);
    *wildcard = psl_has(PSL_WILDCARD, PSL_N(PSL_WILDCARD), s, n);
    *exception = psl_has(PSL_EXCEPTION, PSL_N(PSL_EXCEPTION), s, n);
}

const char *public_suffix_of(const UrlHost *h)
{
    const char *d;
    size_t n, core, i, next, last, match = 0;
    bool found = false, normal = false, wildcard = false, exception = false;

    DCHECK(h != NULL, "URL §3.2 was asked for the public suffix of no host");
    /* STEP 1 — "if host is not a domain, then return null": an IP address, an opaque host and the empty host
       have no labels for a rule to match. */
    if (h->kind != URL_HOST_DOMAIN) return NULL;
    DCHECK(h->domain != NULL, "a host parsed as a DOMAIN carries no domain string — url.c's §4.2 parser sets "
                              "the two together and a host with one and not the other came from neither");
    d = h->domain;
    n = strlen(d);
    /* STEP 2's trailingDot is held out of the matching and comes back with the returned pointer into `d`
       (§Note 3: `example.com.` answers `com.`). */
    core = (n > 0 && d[n - 1] == '.') ? n - 1 : n;
    /* The longest entry of the list that is the whole host or follows a dot, as Chromium's
       LookupSuffixInReversedSet finds it: a wildcard rule `*.X` and an exception rule `!X` are stored as the
       entry X with a tag, so they are matched by their bodies. The leftmost start is the longest. */
    for (i = 0, last = 0; i < core; i = next) {
        next = psl_next_label(d, core, i);
        last = i;
        if (found) continue;
        psl_tags(d + i, core - i, &normal, &wildcard, &exception);
        if (normal || wildcard || exception) { found = true; match = i; }
    }
    /* No rule matches: the last label, as Chromium answers with INCLUDE_UNKNOWN_REGISTRIES, which
       GetDomainAndRegistry uses (the PSL's prevailing rule "*"). */
    if (!found) return d + last;
    /* Chromium's GetRegistryInTrimmedHost (net/base/registry_controlled_domains/registry_controlled_domain.cc),
       in its order. A wildcard entry X: a host that is X is itself a public suffix, which the PSL Format's
       matching would not say (a rule matches only a domain with "as many or more labels than the rule"), and
       Chrome's cookie jar and sites follow Chromium; otherwise the label before X joins it. */
    if (wildcard) {
        if (match == 0) return d;
        for (i = 0; ; i = next) {
            next = psl_next_label(d, core, i);
            if (next == match) return d + i;
        }
    }
    /* An exception entry X: the public suffix is X without its leftmost label. */
    if (exception) {
        next = psl_next_label(d, core, match);
        DCHECK(next < core, "an exception rule matched a domain's last label — an exception names the label its "
                            "wildcard consumed, so its body is at least two labels");
        return d + next;
    }
    return d + match;
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
