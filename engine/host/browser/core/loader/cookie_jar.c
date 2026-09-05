/* RFC 6265 §5 — the cookie store, its receive and its read. See cookie_jar.h for WHY the store is the agent's.
 *
 * THE REPRESENTATION, AND WHAT IT DOES AND DOES NOT MODEL.
 *
 * The store is ONE JS OBJECT with a null prototype, `key -> entry`, and both halves are STRINGS. That it is a
 * JS value is CLAUDE.md's §State-isolation rule and is load-bearing rather than stylistic: "PLATFORM DATA A
 * FLOW QUEUES IS A JS VALUE, never malloc'd C", because a JS object's mutations are ordinary property writes
 * the per-flow COW delta already captures, and it parks to the IDB cold tier and resumes with the flow it
 * belongs to. So the arm that ran `document.cookie = "session=1"` is the only arm whose later read sees it,
 * while a malloc'd list would be ONE timeline for every flow at once. The object is built at the agent's
 * declaration, which is the PRE-BOOT BASELINE — a store made on first touch would be made inside whichever flow
 * happened to read first, and that flow's jar would become every other flow's.
 *
 * THAT THE ENTRY IS A STRING RATHER THAN AN OBJECT IS THE SAME INVARIANT ONE STEP FURTHER. §5.3 steps 11 and 12
 * REPLACE a cookie rather than edit one, so nothing may ever mutate an entry in place — and a nested object
 * that nobody happens to mutate today is one refactor away from being mutated by a flow that did not create it.
 * A JS string cannot be mutated at all, so every write this component can make is a write to the JAR, which is
 * the one object the delta captures. The impossible state is impossible rather than asserted.
 *
 * WHAT IS MODELLED — §5.3's fields, each because something reads it:
 *   name, value       — §5.4 step 4's serialization.
 *   domain            — §5.3 step 11's identity, and §5.4 step 1's domain-match.
 *   path              — §5.3 step 11's identity, §5.4 step 1's path-match, and step 2's sort key. This is the
 *                       one that MUST arrive with the move to a per-agent store: while the store was a realm's,
 *                       every cookie in it had been set by the only document that could read it, so ignoring
 *                       Path changed no answer. Shared between `/a/x.html` and `/b/y.html` it changes many.
 *   host-only-flag    — §5.3 step 6, and the first branch of §5.4 step 1.
 *   secure-only-flag  — §5.4 step 1's third bullet.
 *   expiry-time       — §5.3's "A cookie is expired if the cookie has an expiry date in the past. The user
 *                       agent MUST evict all expired cookies from the cookie store if, at any time, an expired
 *                       cookie exists in the cookie store."
 *
 * WHAT IS NOT, AND WHY EACH IS A STATEMENT ABOUT THE STANDARD RATHER THAN A GAP:
 *   http-only-flag    — §5.3 step 10 abandons a set-cookie-string carrying HttpOnly when it arrives from a
 *                       "non-HTTP" API, and this API is the only writer this store has, so step 10 runs on
 *                       every arrival. No entry can therefore carry the flag: step 11.2 ("if the old-cookie's
 *                       http-only-flag is set") and §5.4 step 1's fourth bullet are unreachable, and a field
 *                       that is false for every entry is not a field. The moment a Set-Cookie RESPONSE HEADER
 *                       writes this store — a second receive, whose §5.3 step 10 does not fire — the flag and
 *                       both of those steps arrive together, in one diff, because they are one mechanism.
 *   persistent-flag   — its only reader is "When the current session is over, the user agent MUST remove from
 *                       the cookie store all cookies with the persistent-flag set to false". The session and
 *                       this store have the SAME lifetime: the store is the agent's and is released with it, so
 *                       the removal is the deallocation.
 *   creation-time     — §5.3 step 11.3 preserves it across a replacement and §5.4 step 2 breaks a path-length
 *                       tie by it. Both are answered by the store's own PROPERTY ORDER: a key first written
 *                       takes its position then, a re-write of an existing key keeps that position (which is
 *                       exactly step 11.3), and a key is never a numeric index (it begins with a length header
 *                       ending in a colon), so insertion order is the enumeration order. It is not a field
 *                       because it is not a second fact.
 *   last-access-time  — §5.4 step 3 updates it and only §5.3's "remove excess cookies" reads it, which is next.
 *   remove excess     — §5.3's two "the user agent MAY remove excess cookies" paragraphs are a CAP on how much
 *                       distinct state a run may hold, and CLAUDE.md's §NO BOUNDS forbids one. This user agent
 *                       takes the MAY-not.
 *   public suffixes   — §5.3 Storage Model's step 5 is conditional on the user agent being configured to
 *                       reject "public suffixes". THIS ONE IS, and the step is built below. That is a CHOICE
 *                       and it is the one every browser makes: the standard's own note calls the step
 *                       "essential for preventing attacker.com from disrupting the integrity of example.com
 *                       by setting a cookie with a Domain attribute of "com"", and core/url/public_suffix.h
 *                       holds the list it answers over.
 *                       THE REASON THIS LINE GAVE HAS NOW BEEN WRONG TWICE, AND BOTH ARE KEPT because a
 *                       reader who re-derives a retired reason re-introduces it. It first said "the Public
 *                       Suffix List is data this engine is not given", which was FALSE WHEN READ and had
 *                       already reached a landed commit message by way of a lane building the Cookie Store
 *                       API. It then said the step's condition is false because this agent is not configured
 *                       to reject public suffixes — true as written, and a configuration nobody had chosen on
 *                       the merits once the list was present.
 *                       AND THE RESIDUAL THAT REPLACED IT WAS WRONG IN ITS NEXT-DIFF CLAUSE, which is the
 *                       clause §THE-ASYMMETRY-IS-STRUCTURAL says is the half a reader may not check. Its NOT
 *                       COVERED and HOW ITS ABSENCE SHOWS were exact — `Domain=com` was stored here and is
 *                       dropped by every browser. Its remedy said to "take step 5's reject arm when the whole
 *                       domain matches it", and STEP 5 HAS TWO ARMS: a domain-attribute identical to the
 *                       canonicalized request-host is EMPTIED, not refused, which demotes the cookie to
 *                       host-only through step 6's own else-branch. Building the single arm that clause named
 *                       would have dropped `Domain=localhost` on `localhost` — and every intranet
 *                       single-label host with it — because the PSL's prevailing rule `*` makes a
 *                       single-label host its OWN public suffix, so step 5's condition is TRUE for exactly
 *                       the hosts a person develops on. A regression delivered as a fix. The spec half of
 *                       that residual was evidence and its remedy was a hypothesis; the hypothesis yielded.
 *                       AND THE QUOTATION BESIDE IT WAS FLATTENED THE SAME WAY, which is the co-occurrence
 *                       CLAUDE.md's §THE-CHEAPEST-TELL names: this line used to put its quotes round
 *                       `configured to reject public suffixes`, where the RFC quotes only the term — "If the
 *                       user agent is configured to reject "public suffixes" and the domain-attribute is a
 *                       public suffix". The mis-placed span and the collapsed two-arm step were one error
 *                       written twice, three lines apart. */
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/loader/cookie_jar.h"
#include "core/url/public_suffix.h"
#include "core/url/url.h"

/* ---- the agent's store ------------------------------------------------------------------------------------ */

static JSValue    g_jar;      /* key -> entry, both strings; see the header comment for the encoding */
static JSRuntime *g_rt;
static int        g_ready;

#if APICLIENT_DEV
/* THE TWO-SIDED HALF OF "AN INSTANCE IS ONE ORIGIN". Nothing in the algorithms below needs an agent-level host:
   every domain-match and every secure test is made against the REQUEST-URI the caller hands in, which is the
   receiver document's own address. These exist only to assert that those request-URIs never disagree, because
   a store shared by two hosts or two schemes would be one jar answering for two principals — and the report a
   disagreement must produce is what to BUILD, not a number. Recorded at the first touch and compared at every
   one after it; DEV-only in body as well as in verdict, since a release build cannot act on the answer. */
static char *g_agent_host;
static int   g_agent_secure = -1;
#endif

/* BORROWED — the store is the agent's and outlives every read of it, so nothing here takes a reference. */
static JSValueConst cj_jar(void)
{
    DCHECK(g_ready, "the cookie store was reached before cookie_jar_init built it — §5.3's store is the AGENT's "
                    "and is declared with the platform, so a reader that finds none is running in an agent the "
                    "platform list never declared");
    return g_jar;
}

/* ---- §5.1's subcomponent algorithms ------------------------------------------------------------------------ */

/* §5.1.2's CANONICALIZED HOST NAME. url.c's parser has already produced it: a domain reaches URL_HOST_DOMAIN
   lowercased and through domain-to-ASCII, which is §5.1.2's three steps. The second answer is §5.1.3's third
   condition, "the string is a host name (i.e., NOT AN IP ADDRESS)" — the URL record knows which it parsed, so
   the question is answered by the parse rather than re-derived from the text. OWNED. */
static char *cj_request_host(const UrlRecord *u, bool *is_host_name)
{
    char *h;

    DCHECK(u->host.kind != URL_HOST_NULL,
           "§5.3 was handed a request-uri with no host — the only caller is HTML §3.1.4, whose cookie-averse "
           "test has already refused every scheme but http(s), and a special scheme cannot parse without one");
    *is_host_name = (u->host.kind == URL_HOST_DOMAIN);
    h = url_serialize_host(&u->host);
    CHECK(h != NULL, "OOM serializing the canonicalized request-host for §5.1.2");
    return h;
}

/* §5.1.3 DOMAIN MATCHING, both conditions. */
/* §5.3 STEP 5's CONDITION, "the domain-attribute is a public suffix", over PAGE-SUPPLIED BYTES — which is the
   whole of why it is written this way. The domain-attribute is whatever a `Set-Cookie` or a `document.cookie`
   assignment put there, so an empty, malformed, over-long or IP-address value must produce an ANSWER and never
   an abort: this engine's own parser decides whether those bytes name a domain at all, and a value it refuses
   is not a public suffix, which leaves step 6's domain-match to dispose of it exactly as it does today.

   THE PARSE IS THE ENGINE'S AND THE ASSERTION RIDES ON IT: `host_is_public_suffix` is asked about a UrlHost
   this function built, never about the raw attribute, so the list is consulted on a lowercased A-label host in
   the one representation §Algorithm requires both sides to be in. */
static bool cj_domain_attribute_is_public_suffix(const char *dom, size_t dlen)
{
    UrlHost h;
    bool r;

    if (!url_parse_host(&h, dom, dlen, /*is_opaque*/ false)) {
        url_host_free(&h);
        return false;
    }
    r = host_is_public_suffix(&h);
    url_host_free(&h);
    return r;
}

static bool cj_domain_match(const char *s, size_t slen, const char *d, size_t dlen, bool s_is_host_name)
{
    if (slen == dlen && memcmp(s, d, dlen) == 0)
        return true;                                        /* "identical" */
    return s_is_host_name && dlen && slen > dlen &&
           memcmp(s + slen - dlen, d, dlen) == 0 &&          /* "a suffix of the string" */
           s[slen - dlen - 1] == '.';                        /* "the last character ... is a %x2E" */
}

/* §5.1.4's DEFAULT-PATH of a cookie, from the request-uri. OWNED. */
static char *cj_root_path(void)
{
    char *r = strdup("/");

    CHECK(r != NULL, "OOM building §5.1.4's default-path");
    return r;
}

static char *cj_default_path(const UrlRecord *u)
{
    char *p = url_serialize_path(u), *out;
    size_t n, last;

    CHECK(p != NULL, "OOM serializing the request-uri's path for §5.1.4's default-path");
    n = strlen(p);
    /* STEP 2: empty, or not beginning with a solidus. */
    if (!n || p[0] != '/') { free(p); return cj_root_path(); }
    /* STEP 3: no more than one solidus. */
    for (last = n; last > 0 && p[last - 1] != '/'; last--) { }
    DCHECK(last > 0, "§5.1.4 step 4 found no solidus in a path step 2 has already established begins with one");
    if (last == 1) { free(p); return cj_root_path(); }
    /* STEP 4: up to, but not including, the right-most solidus. */
    out = malloc(last);
    CHECK(out != NULL, "OOM building §5.1.4's default-path");
    memcpy(out, p, last - 1);
    out[last - 1] = 0;
    free(p);
    return out;
}

/* §5.1.4's PATH-MATCH, all three conditions. */
static bool cj_path_match(const char *req, size_t rlen, const char *cp, size_t clen)
{
    DCHECK(clen > 0, "§5.1.4's path-match was asked about an EMPTY cookie-path — §5.2.4 gives a cookie whose "
                     "Path is empty or does not begin with a solidus the default-path instead, and §5.1.4's "
                     "default-path is at shortest \"/\", so no cookie in this store can have one");
    if (rlen == clen && memcmp(req, cp, clen) == 0)
        return true;                                         /* "identical" */
    if (clen > rlen || memcmp(req, cp, clen) != 0)
        return false;                                        /* not a prefix at all */
    return cp[clen - 1] == '/' || req[clen] == '/';
}

/* §5.4 step 1's third bullet: "the request-uri's scheme must denote a 'secure' protocol (as defined by the user
   agent)". The NOTE names the definition every user agent uses, and the only schemes that reach this component
   are the two HTML §3.1.4's cookie-averse test lets through. */
static bool cj_secure_scheme(const UrlRecord *u)
{
    return u->scheme && strcmp(u->scheme, "https") == 0;
}

#if APICLIENT_DEV
static void cj_assert_one_principal(const UrlRecord *u)
{
    bool is_name;
    char *host = cj_request_host(u, &is_name);
    int secure = cj_secure_scheme(u) ? 1 : 0;

    if (!g_agent_host) {
        g_agent_host = host;
        g_agent_secure = secure;
        return;
    }
    DCHECK(strcmp(g_agent_host, host) == 0 && g_agent_secure == secure,
           "one cookie store was reached from two different request-hosts or two different schemes — an "
           "instance is one (browsing-context group, ORIGIN), so two principals mean this document belongs to a "
           "PEER instance and its cookies are that instance's store's. BUILD the cross-instance read: §3.1.4's "
           "getter suspends exactly as a cross-origin property read does, the instance holding that document "
           "runs §5.4 over its own jar, and the flow resumes with the cookie-string");
    free(host);
}
#else
#define cj_assert_one_principal(u) ((void)(u))
#endif

/* ---- the entry codec ---------------------------------------------------------------------------------------
 *
 * KEY   = "<nlen>:<dlen>:<plen>:" name domain path   — §5.3 step 11's (name, domain, path) identity. It is
 *         LENGTH-PREFIXED because a cookie-name and a cookie-path are both free-form: §5.2's parse takes a name
 *         as everything up to the first "=" and a path as everything up to the next ";", so any separator
 *         character can occur inside one and only a length can delimit them. It begins with a digit and
 *         contains a colon, so it is never an array index and the store's property order stays insertion order.
 * ENTRY = <secure><host-only><expiry> ":" value      — the flags are one character each, and the expiry is a
 *         decimal epoch second or "*" for §5.3 step 3's "latest representable date". */

static char *cj_key(const char *name, size_t nlen, const char *dom, size_t dlen,
                    const char *path, size_t plen, size_t *out_len)
{
    char hdr[64];
    int hn = snprintf(hdr, sizeof hdr, "%zu:%zu:%zu:", nlen, dlen, plen);
    size_t total;
    char *k;

    CHECK(hn > 0 && (size_t)hn < sizeof hdr, "a cookie key's length header did not fit — the three lengths are "
                                             "sizes of one set-cookie-string and cannot be that large");
    total = (size_t)hn + nlen + dlen + plen;
    k = malloc(total + 1);
    CHECK(k != NULL, "OOM building a cookie store key");
    memcpy(k, hdr, (size_t)hn);
    memcpy(k + hn, name, nlen);
    memcpy(k + hn + nlen, dom, dlen);
    memcpy(k + hn + nlen + dlen, path, plen);
    k[total] = 0;
    *out_len = total;
    return k;
}

/* The inverse, over a key this component wrote. Every failure here is a disagreement between the two halves of
   one codec, never a state — so each is a DCHECK and none is recoverable control flow. */
static void cj_key_parse(const char *k, size_t klen, const char **name, size_t *nlen,
                         const char **dom, size_t *dlen, const char **path, size_t *plen)
{
    unsigned long long a = 0, b = 0, c = 0;
    const char *p = k, *end = k + klen;
    int field;

    for (field = 0; field < 3; field++) {
        unsigned long long v = 0;
        const char *start = p;
        while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (unsigned)(*p - '0'); p++; }
        DCHECK(p > start && p < end && *p == ':',
               "a cookie store key has no length header — every key is written by cj_key, so a key without one "
               "came from somewhere that is not this codec");
        p++;
        if (field == 0) a = v; else if (field == 1) b = v; else c = v;
    }
    DCHECK((size_t)(end - p) == a + b + c,
           "a cookie store key's length header does not describe its own tail — the two halves of one codec "
           "disagree");
    *name = p;        *nlen = (size_t)a;
    *dom  = p + a;    *dlen = (size_t)b;
    *path = p + a + b; *plen = (size_t)c;
}

/* THE LATEST REPRESENTABLE DATE, as this store spells it. Kept out of the number space so no arithmetic on a
   real epoch can collide with it. */
#define CJ_NEVER "*"

static char *cj_entry(bool secure, bool host_only, long long expiry, bool persistent,
                      const char *value, size_t vlen, size_t *out_len)
{
    char hdr[48];
    int hn;
    size_t total;
    char *e;

    if (persistent)
        hn = snprintf(hdr, sizeof hdr, "%c%c%lld:", secure ? '1' : '0', host_only ? '1' : '0', expiry);
    else
        hn = snprintf(hdr, sizeof hdr, "%c%c" CJ_NEVER ":", secure ? '1' : '0', host_only ? '1' : '0');
    CHECK(hn > 0 && (size_t)hn < sizeof hdr, "a cookie entry's header did not fit");
    total = (size_t)hn + vlen;
    e = malloc(total + 1);
    CHECK(e != NULL, "OOM building a cookie store entry");
    memcpy(e, hdr, (size_t)hn);
    memcpy(e + hn, value, vlen);
    e[total] = 0;
    *out_len = total;
    return e;
}

static void cj_entry_parse(const char *e, size_t elen, bool *secure, bool *host_only, bool *expired,
                           const char **value, size_t *vlen)
{
    const char *p = e + 2, *end = e + elen;
    long long expiry = 0;
    bool persistent;

    DCHECK(elen >= 4 && (e[0] == '0' || e[0] == '1') && (e[1] == '0' || e[1] == '1'),
           "a cookie store entry has no flag header — the two halves of one codec disagree");
    *secure = e[0] == '1';
    *host_only = e[1] == '1';
    persistent = *p != CJ_NEVER[0];
    if (persistent) {
        const char *start = p;
        bool neg = (*p == '-');
        if (neg) p++;
        while (p < end && *p >= '0' && *p <= '9') { expiry = expiry * 10 + (*p - '0'); p++; }
        DCHECK(p > start, "a cookie store entry's expiry is neither a number nor the latest representable date");
        if (neg) expiry = -expiry;
    } else {
        p++;
    }
    DCHECK(p < end && *p == ':', "a cookie store entry's header is unterminated");
    p++;
    /* §5.3: "A cookie is 'expired' if the cookie has an expiry date in the past." */
    *expired = persistent && expiry <= (long long)time(NULL);
    *value = p;
    *vlen = (size_t)(end - p);
}

/* ---- §5.1.1's PARSE A COOKIE-DATE --------------------------------------------------------------------------- */

/* RFC 6265 §4.1.1's OWS, which §5.2 step 4 strips from both the name and the value. */
static bool cj_is_wsp(char c) { return c == ' ' || c == '\t'; }

static void cj_trim(const char **p, size_t *n)
{
    while (*n && cj_is_wsp((*p)[0])) { (*p)++; (*n)--; }
    while (*n && cj_is_wsp((*p)[*n - 1])) (*n)--;
}

/* ASCII case-insensitive equality against a lowercase literal — RFC 6265 compares attribute names and month
   names case-insensitively, and this engine may not assume a locale-aware strcasecmp is on the platform. */
static bool cj_ci_eq(const char *s, size_t n, const char *lower)
{
    size_t i;

    if (n != strlen(lower)) return false;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != lower[i]) return false;
    }
    return true;
}

/* §5.1.1's DELIMITER: %x09 / %x20-2F / %x3B-40 / %x5B-60 / %x7B-7E. */
static bool cj_date_delim(unsigned char c)
{
    return c == 0x09 || (c >= 0x20 && c <= 0x2F) || (c >= 0x3B && c <= 0x40) ||
           (c >= 0x5B && c <= 0x60) || (c >= 0x7B && c <= 0x7E);
}

/* Days from 1970-01-01 to y-m-d (proleptic Gregorian), Howard Hinnant's days_from_civil — the one arithmetic
   that turns a broken-down UTC date into an epoch time without a timegm the platform may not have. */
static long long cj_days_from_civil(int y, int m, int d)
{
    long long yy = y - (m <= 2);
    long long era = (yy >= 0 ? yy : yy - 399) / 400;
    long long yoe = yy - era * 400;                                  /* [0, 399] */
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  /* [0, 365] */
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           /* [0, 146096] */

    return era * 146097 + doe - 719468;
}

/* §5.1.1 PARSE A COOKIE-DATE. Returns true and fills `*out` with the epoch time; false is the standard's
   "abort these steps" — an unparsable date, which §5.2.1 answers by IGNORING the attribute. */
static bool cj_parse_cookie_date(const char *s, size_t n, long long *out)
{
    static const char *const MONTHS[12] = { "jan", "feb", "mar", "apr", "may", "jun",
                                            "jul", "aug", "sep", "oct", "nov", "dec" };
    int hour = 0, min = 0, sec = 0, day = 0, month = 0, year = 0, i;
    bool found_time = false, found_day = false, found_month = false, found_year = false;
    size_t p = 0;

    while (p < n) {
        size_t start;
        const char *tok;
        size_t tlen;

        while (p < n && cj_date_delim((unsigned char)s[p])) p++;   /* STEP 2's delimiters */
        start = p;
        while (p < n && !cj_date_delim((unsigned char)s[p])) p++;
        tok = s + start;
        tlen = p - start;
        if (!tlen) continue;

        /* STEP 2.1: the TIME production — 1*2DIGIT ":" 1*2DIGIT ":" 1*2DIGIT, and the rest of the token is
           ignored (that is what lets "10:12:14 GMT" and "10:12:14.5" both parse). */
        if (!found_time) {
            int h = 0, m = 0, se = 0, k = 0, part[3] = { 0, 0, 0 }, np = 0, digits = 0;
            bool ok = true;
            for (k = 0; k < (int)tlen && np < 3; k++) {
                char c = tok[k];
                if (c >= '0' && c <= '9') {
                    if (++digits > 2) { ok = false; break; }
                    part[np] = part[np] * 10 + (c - '0');
                } else if (c == ':' && digits) {
                    np++;
                    digits = 0;
                } else {
                    break;
                }
            }
            if (ok && np == 2 && digits) {
                h = part[0]; m = part[1]; se = part[2];
                if (h <= 23 && m <= 59 && se <= 59) {
                    hour = h; min = m; sec = se;
                    found_time = true;
                    continue;
                }
            }
        }
        /* STEP 2.2: the DAY-OF-MONTH production — 1*2DIGIT, the rest of the token ignored. */
        if (!found_day && tlen && tok[0] >= '0' && tok[0] <= '9') {
            int d = tok[0] - '0', k = 1;
            if (tlen > 1 && tok[1] >= '0' && tok[1] <= '9') { d = d * 10 + (tok[1] - '0'); k = 2; }
            if (!(k < (int)tlen && tok[k] >= '0' && tok[k] <= '9') && d >= 1 && d <= 31) {
                day = d;
                found_day = true;
                continue;
            }
        }
        /* STEP 2.3: the MONTH production — the first three characters, case-insensitively. */
        if (!found_month && tlen >= 3) {
            for (i = 0; i < 12; i++) {
                if (cj_ci_eq(tok, 3, MONTHS[i])) {
                    month = i + 1;
                    found_month = true;
                    break;
                }
            }
            if (found_month) continue;
        }
        /* STEP 2.4: the YEAR production — 2*4DIGIT, the rest of the token ignored. */
        if (!found_year && tlen && tok[0] >= '0' && tok[0] <= '9') {
            int y = 0, k = 0;
            while (k < (int)tlen && k < 4 && tok[k] >= '0' && tok[k] <= '9') { y = y * 10 + (tok[k] - '0'); k++; }
            if (k >= 2 && !(k < (int)tlen && tok[k] >= '0' && tok[k] <= '9')) {
                year = y;
                found_year = true;
                continue;
            }
        }
    }
    /* STEPS 3-5: the two-digit-year rule, then the ranges the standard rejects outright. */
    if (year >= 70 && year <= 99) year += 1900;
    else if (year <= 69) year += 2000;
    if (!found_time || !found_day || !found_month || !found_year) return false;
    if (day < 1 || day > 31 || year < 1601 || hour > 23 || min > 59 || sec > 59) return false;
    *out = cj_days_from_civil(year, month, day) * 86400LL + hour * 3600LL + min * 60LL + sec;
    return true;
}

/* ---- §5.2's PARSE A SET-COOKIE-STRING and §5.3's STORAGE MODEL --------------------------------------------- */

/* The cookie-attribute-list, as the four attributes §5.3 reads out of it. `have_expiry` is §5.3 step 3's
   "contains an attribute with an attribute-name of Max-Age / Expires", and Max-Age's presence is what makes
   Expires unread — the precedence is the standard's, stated once here. */
typedef struct {
    bool        max_age_seen;
    bool        have_expiry;
    long long   expiry;
    const char *domain;  size_t domain_len;   /* §5.2.3's cookie-domain, lowercased below */
    const char *path;    size_t path_len;     /* §5.2.4's attribute-value; empty means "use the default-path" */
    bool        have_path;
    bool        secure;
    bool        http_only;
} CjAttrs;

static void cj_parse_attrs(const char *attrs, size_t attrs_len, CjAttrs *a)
{
    while (attrs_len) {
        const char *piece = attrs, *aeq;
        size_t plen = attrs_len, alen, vlen;
        const char *aname, *aval;
        const char *sep = memchr(attrs, ';', attrs_len);

        /* STEPS 3-4 of the unparsed-attributes parse: one cookie-av, split at its FIRST "=". */
        if (sep) {
            plen = (size_t)(sep - attrs);
            attrs = sep + 1;
            attrs_len -= plen + 1;
        } else {
            attrs += attrs_len;
            attrs_len = 0;
        }
        aeq = memchr(piece, '=', plen);
        aname = piece;
        alen = aeq ? (size_t)(aeq - piece) : plen;
        aval = aeq ? aeq + 1 : piece + plen;
        vlen = aeq ? plen - alen - 1 : 0;
        cj_trim(&aname, &alen);      /* STEP 5 */
        cj_trim(&aval, &vlen);
        if (cj_ci_eq(aname, alen, "max-age")) {
            /* §5.2.2: the first character must be a DIGIT or "-" and the rest DIGITs, or the attribute is
               IGNORED — an ignored cookie-av is never appended to the cookie-attribute-list, which is why an
               invalid Max-Age leaves an Expires beside it readable. A non-positive delta-seconds is the
               earliest representable date; a positive one is `now + delta`. */
            bool neg = vlen && aval[0] == '-';
            size_t k = neg ? 1 : 0;
            bool ok = vlen > k;
            long long secs = 0;
            for (; k < vlen && ok; k++) {
                if (aval[k] < '0' || aval[k] > '9') ok = false;
                else if (secs < 1000000000LL) secs = secs * 10 + (aval[k] - '0');
            }
            if (ok) {
                a->max_age_seen = true;               /* §5.3 step 3: MAX-AGE WINS, whatever the order */
                a->have_expiry = true;
                /* §5.2.2: "If delta-seconds is less than or equal to zero (0), let expiry-time be the earliest
                   representable date and time.  Otherwise, let the expiry-time be the current date and time
                   plus delta-seconds seconds." */
                a->expiry = (neg || secs == 0) ? LLONG_MIN : (long long)time(NULL) + secs;
            }
        } else if (cj_ci_eq(aname, alen, "expires")) {
            long long when = 0;
            if (!a->max_age_seen && cj_parse_cookie_date(aval, vlen, &when)) {
                a->have_expiry = true;
                a->expiry = when;
            }
        } else if (cj_ci_eq(aname, alen, "domain")) {
            /* §5.2.3: an empty attribute-value SHOULD be ignored entirely; a leading "." is dropped. The
               lowercasing happens at the store, where the buffer that holds it lives. */
            if (vlen) {
                a->domain = aval[0] == '.' ? aval + 1 : aval;
                a->domain_len = aval[0] == '.' ? vlen - 1 : vlen;
            }
        } else if (cj_ci_eq(aname, alen, "path")) {
            /* §5.2.4: empty, or not beginning with "/", means the default-path. */
            a->have_path = vlen && aval[0] == '/';
            if (a->have_path) { a->path = aval; a->path_len = vlen; }
        } else if (cj_ci_eq(aname, alen, "secure")) {
            a->secure = true;                          /* §5.2.5 */
        } else if (cj_ci_eq(aname, alen, "httponly")) {
            a->http_only = true;                       /* §5.2.6 */
        }
        /* §5.2 step 6's parenthetical: "attributes with unrecognized attribute-names are ignored". */
    }
}

void cookie_jar_receive(JSContext *ctx, const UrlRecord *uri, const char *s, size_t len)
{
    const char *nv = s, *attrs = NULL, *name, *value, *eq;
    size_t nvlen = len, attrs_len = 0, name_len, value_len, dlen = 0, plen, klen, hostlen;
    const char *semi = memchr(s, ';', len);
    CjAttrs a;
    char *host, *dom = NULL, *path = NULL, *key;
    bool host_is_name, host_only;
    JSValueConst jar = cj_jar();
    JSAtom atom;

    cj_assert_one_principal(uri);
    /* §5.2 STEP 1: the name-value-pair string, and the unparsed-attributes after the first ";". */
    if (semi) {
        nvlen = (size_t)(semi - s);
        attrs = semi + 1;
        attrs_len = len - nvlen - 1;
    }
    eq = memchr(nv, '=', nvlen);
    if (!eq) return;                  /* STEP 2: no "=" — ignore the set-cookie-string entirely */
    name = nv;                        /* STEP 3 */
    name_len = (size_t)(eq - nv);
    value = eq + 1;
    value_len = nvlen - name_len - 1;
    cj_trim(&name, &name_len);        /* STEP 4 */
    cj_trim(&value, &value_len);
    if (!name_len) return;            /* STEP 5: an empty name — ignore the set-cookie-string entirely */

    memset(&a, 0, sizeof a);
    cj_parse_attrs(attrs, attrs_len, &a);

    /* §5.3 STEP 10, hoisted to where it costs nothing: a set-cookie-string carrying HttpOnly is ABANDONED when
       it arrives through a "non-HTTP" API, which is the only kind of arrival this store has. */
    if (a.http_only) return;

    host = cj_request_host(uri, &host_is_name);
    hostlen = strlen(host);
    /* §5.3 STEP 4 — the domain-attribute, which §5.2.3 has already stripped of a leading U+002E and which is
       lowercased here. Steps 5 and 6 both read it, and step 5 may EMPTY it. */
    if (a.domain_len) {
        size_t i;
        dom = malloc(a.domain_len + 1);
        CHECK(dom != NULL, "OOM lowercasing §5.2.3's cookie-domain");
        for (i = 0; i < a.domain_len; i++) {
            char c = a.domain[i];
            dom[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        dom[a.domain_len] = 0;
        dlen = a.domain_len;
    }
    /* §5.3 Storage Model's STEP 5, whose condition is "If the user agent is configured to reject "public
       suffixes" and the domain-attribute is a public suffix". THIS USER AGENT IS SO CONFIGURED, which is a
       choice and is the one every browser makes: the standard's own note calls the step "essential for
       preventing attacker.com from disrupting the integrity of example.com by setting a cookie with a Domain
       attribute of "com"", and the engine holds the list (core/url/public_suffix.h) that answers it.

       IT HAS TWO ARMS AND BOTH ARE BUILT, because the reject arm alone is not this step. "If the domain-
       attribute is identical to the canonicalized request-host: let the domain-attribute be the empty string.
       Otherwise: ignore the cookie entirely and abort these steps." The first arm demotes the cookie to
       host-only through step 6's own else-branch below; it is not a refusal at all. Taking only the second
       would drop `Domain=localhost` on `localhost` — and every intranet single-label host with it — because
       §Algorithm's prevailing rule `*` makes a single-label host its OWN public suffix, so the condition above
       is TRUE for exactly the hosts a person develops on. */
    if (dom && cj_domain_attribute_is_public_suffix(dom, dlen)) {
        if (dlen != hostlen || memcmp(dom, host, dlen) != 0) {
            free(dom);
            free(host);
            return;                   /* "Ignore the cookie entirely and abort these steps." */
        }
        free(dom);                    /* "Let the domain-attribute be the empty string." */
        dom = NULL;
        dlen = 0;
    }
    /* §5.3 STEP 6, branching on "if the domain-attribute is non-empty" — which step 5 may just have made
       false for a value that arrived non-empty. */
    if (dom) {
        if (!cj_domain_match(host, hostlen, dom, dlen, host_is_name)) {
            free(dom);
            free(host);
            return;                   /* "Ignore the cookie entirely and abort these steps." */
        }
        host_only = false;
    } else {
        dom = host;                   /* "Set the cookie's domain to the canonicalized request-host." */
        host = NULL;
        dlen = strlen(dom);
        host_only = true;
    }
    /* §5.3 STEP 7. */
    if (a.have_path) {
        path = malloc(a.path_len + 1);
        CHECK(path != NULL, "OOM copying §5.2.4's cookie-path");
        memcpy(path, a.path, a.path_len);
        path[a.path_len] = 0;
        plen = a.path_len;
    } else {
        path = cj_default_path(uri);
        plen = strlen(path);
    }

    key = cj_key(name, name_len, dom, dlen, path, plen, &klen);
    atom = JS_NewAtomLen(ctx, key, klen);
    CHECK(atom != JS_ATOM_NULL, "document.cookie: a cookie's store key could not be interned");
    /* §5.3 STEPS 11 and 12, then the standard's eviction rule in the one place a cookie can become expired
       without time passing: "The user agent MUST evict all expired cookies from the cookie store if, at any
       time, an expired cookie exists in the cookie store." Step 11.4 removes the old cookie either way; when
       the new one is already expired, the eviction removes it in the same breath, and the observable result is
       the deletion every page writes `expires=Thu, 01 Jan 1970` to get.
       A WRITE THAT SURVIVES IS JS_SetProperty ON AN EXISTING KEY, which keeps the key's position in the
       store's property order — which is §5.3 step 11.3's "update the creation-time of the newly created cookie
       to match the creation-time of the old-cookie", since that order IS this store's creation-time. */
    if (a.have_expiry && a.expiry <= (long long)time(NULL)) {
        JS_DeleteProperty(ctx, jar, atom, 0);
    } else {
        size_t elen;
        char *entry = cj_entry(a.secure, host_only, a.expiry, a.have_expiry, value, value_len, &elen);
        JSValue v = JS_NewStringLen(ctx, entry, elen);
        free(entry);
        CHECK(!JS_IsException(v), "document.cookie: a cookie store entry could not be allocated");
        /* An ordinary property write on the agent's one jar — which is exactly why the store is a JS object:
           the heap COW captures it, so the arm that set this cookie is the only arm that reads it back. */
        CHECK(JS_SetProperty(ctx, jar, atom, v) >= 0,
              "document.cookie: the cookie store refused a write, and nothing of the page's is on it");
    }
    JS_FreeAtom(ctx, atom);
    free(key);
    free(path);
    free(dom);
    free(host);
}

/* ---- §5.4's COOKIE HEADER ---------------------------------------------------------------------------------- */

/* One member of §5.4 step 1's cookie-list, holding the two C strings it was read out of so that step 4 can
   serialize from them and this component can free exactly what it allocated. */
typedef struct {
    const char *key, *entry;      /* OWNED — JS_FreeCString */
    const char *name;  size_t nlen;
    const char *value; size_t vlen;
    size_t      plen;             /* §5.4 step 2's sort key: the cookie-path's length */
} CjHit;

/* §5.4 STEPS 1 AND 2 — THE PART TWO STANDARDS SHARE. Collected and sorted once, with step 4's serialization
   and Cookie Store API §7.1's step 3 both reading the result.

   THE SPLIT IS THE OTHER STANDARD'S OWN WORDS AND NOT A REFACTOR FOR ITS CONVENIENCE. Cookie Store API §7.1
   "Query cookies" step 1 says to perform the steps of this section "to compute the `cookie-string from a given
   cookie store` with url as request-uri", and then states exactly which half it wants: "The cookie-string
   itself is ignored, but the INTERMEDIATE COOKIE-LIST is used in subsequent steps." So the cookie-list is a
   thing that standard names and reads, and the cookie-string is one of two answers computed FROM it.

   WHICH IS WHY THE LIST IS WHAT THIS FILE EXPOSES. The other way round — §7.1 taking the string and splitting
   it back into pairs — is a SECOND PARSER for a syntax this component just serialized, and its inverse would
   have to re-derive a codec that is private to this file. Two answers to one question is the shape that
   drifts, and the drift would be silent: a cookie whose value contains "=" round-trips through the first
   split correctly today and stops the day §5.4 step 4's join changes. The list has no syntax to disagree
   about. Returns the cookie-list, `*out_nhit` long, OWNED — released by cj_hits_free. */
static CjHit *cj_collect(JSContext *ctx, const UrlRecord *uri, uint32_t *out_nhit)
{
    JSValueConst jar = cj_jar();
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0, i;
    CjHit *hits;
    uint32_t nhit = 0;
    char *host, *req_path;
    size_t hostlen, req_len;
    bool host_is_name, secure_ok;

    cj_assert_one_principal(uri);
    host = cj_request_host(uri, &host_is_name);
    hostlen = strlen(host);
    req_path = url_serialize_path(uri);
    CHECK(req_path != NULL, "OOM serializing the request-uri's path for §5.4's path-match");
    req_len = strlen(req_path);
    secure_ok = cj_secure_scheme(uri);

    CHECK(JS_GetOwnPropertyNames(ctx, &tab, &n, jar, JS_GPN_STRING_MASK) == 0,
          "the cookie store could not be enumerated, and nothing of the page's is on it");
    hits = n ? calloc(n, sizeof *hits) : NULL;
    CHECK(n == 0 || hits != NULL, "OOM collecting §5.4 step 1's cookie-list");
    /* STEP 1: the set of cookies that meet all of the requirements. */
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetProperty(ctx, jar, tab[i].atom);
        size_t klen = 0, elen = 0;
        const char *k = JS_AtomToCStringLen(ctx, &klen, tab[i].atom);
        const char *e;
        const char *name, *dom, *path, *value;
        size_t nlen, dlen, plen, vlen;
        bool secure, host_only, expired, keep;

        CHECK(!JS_IsException(v), "a stored cookie could not be read back");
        e = JS_ToCStringLen(ctx, &elen, v);
        JS_FreeValue(ctx, v);
        CHECK(k != NULL && e != NULL, "a stored cookie could not be read as a string");
        cj_key_parse(k, klen, &name, &nlen, &dom, &dlen, &path, &plen);
        cj_entry_parse(e, elen, &secure, &host_only, &expired, &value, &vlen);
        keep = !expired &&
               (host_only ? (hostlen == dlen && memcmp(host, dom, dlen) == 0)
                          : cj_domain_match(host, hostlen, dom, dlen, host_is_name)) &&
               cj_path_match(req_path, req_len, path, plen) &&
               (!secure || secure_ok);
        /* §5.3's eviction rule, at the one moment an expired cookie is known to exist. It is a property write
           on the jar like any other, so it belongs to the flow that made the read and to no sibling. */
        if (expired)
            JS_DeleteProperty(ctx, jar, tab[i].atom, 0);
        if (!keep) {
            JS_FreeCString(ctx, k);
            JS_FreeCString(ctx, e);
            continue;
        }
        hits[nhit].key = k;
        hits[nhit].entry = e;
        hits[nhit].name = name;   hits[nhit].nlen = nlen;
        hits[nhit].value = value; hits[nhit].vlen = vlen;
        hits[nhit].plen = plen;
        nhit++;
    }
    JS_FreePropertyEnum(ctx, tab, n);
    free(req_path);
    free(host);

    /* STEP 2: "Cookies with longer paths are listed before cookies with shorter paths. Among cookies that have
       equal-length path fields, cookies with earlier creation-times are listed before cookies with later
       creation-times." An INSERTION SORT because it is stable, and the order it is stable in is the store's own
       property order, which is creation order — so the second bullet needs no comparison at all.
       Step 3's last-access-time update has no reader in this engine; see the file header. */
    for (i = 1; i < nhit; i++) {
        CjHit h = hits[i];
        uint32_t j = i;
        while (j > 0 && hits[j - 1].plen < h.plen) { hits[j] = hits[j - 1]; j--; }
        hits[j] = h;
    }
    *out_nhit = nhit;
    return hits;
}

static void cj_hits_free(JSContext *ctx, CjHit *hits, uint32_t nhit)
{
    uint32_t i;

    for (i = 0; i < nhit; i++) {
        JS_FreeCString(ctx, hits[i].key);
        JS_FreeCString(ctx, hits[i].entry);
    }
    free(hits);
}

JSValue cookie_jar_cookie_string(JSContext *ctx, const UrlRecord *uri)
{
    uint32_t nhit = 0, i;
    CjHit *hits = cj_collect(ctx, uri, &nhit);
    char *acc = NULL;
    size_t acc_len = 0;
    JSValue out;

    /* STEP 4: `name=value`, joined by "; ". */
    for (i = 0; i < nhit; i++) {
        size_t add = (i ? 2 : 0) + hits[i].nlen + 1 + hits[i].vlen;
        char *grown = realloc(acc, acc_len + add + 1);

        CHECK(grown != NULL, "document.cookie: OOM building the cookie string");
        acc = grown;
        if (i) { memcpy(acc + acc_len, "; ", 2); acc_len += 2; }
        memcpy(acc + acc_len, hits[i].name, hits[i].nlen); acc_len += hits[i].nlen;
        acc[acc_len++] = '=';
        memcpy(acc + acc_len, hits[i].value, hits[i].vlen); acc_len += hits[i].vlen;
        acc[acc_len] = 0;
    }
    cj_hits_free(ctx, hits, nhit);
    out = JS_NewStringLen(ctx, acc ? acc : "", acc_len);
    free(acc);
    CHECK(!JS_IsException(out), "document.cookie: the cookie string could not be allocated");
    return out;
}

JSValue cookie_jar_cookie_list(JSContext *ctx, const UrlRecord *uri)
{
    uint32_t nhit = 0, i;
    CjHit *hits = cj_collect(ctx, uri, &nhit);
    JSValue list = JS_NewArray(ctx);

    CHECK(!JS_IsException(list), "OOM building §5.4 step 1's cookie-list");
    for (i = 0; i < nhit; i++) {
        JSValue pair = JS_NewArray(ctx);
        JSValue name, value;

        CHECK(!JS_IsException(pair), "OOM building a cookie-list entry");
        name = JS_NewStringLen(ctx, hits[i].name, hits[i].nlen);
        value = JS_NewStringLen(ctx, hits[i].value, hits[i].vlen);
        CHECK(!JS_IsException(name) && !JS_IsException(value),
              "OOM building a cookie-list entry's name and value");
        /* §5.3's stored fields, decoded where the standard that reads them decodes: Cookie Store API §7.1's
           "create a CookieListItem" runs UTF-8 decode without BOM on each of the two, and JS_NewStringLen is
           that decode. The other five stored fields are deliberately not here — see cookie_jar.h. */
        CHECK(JS_DefinePropertyValueUint32(ctx, pair, 0, name, JS_PROP_C_W_E) >= 0 &&
              JS_DefinePropertyValueUint32(ctx, pair, 1, value, JS_PROP_C_W_E) >= 0,
              "a cookie-list entry refused its own name and value");
        CHECK(JS_DefinePropertyValueUint32(ctx, list, i, pair, JS_PROP_C_W_E) >= 0,
              "§5.4 step 1's cookie-list refused an entry");
    }
    cj_hits_free(ctx, hits, nhit);
    return list;
}

/* ---- the agent's declaration and teardown ------------------------------------------------------------------ */

void cookie_jar_init(JSContext *ctx)
{
    DCHECK(!g_ready, "cookie_jar_init ran twice — §5.3's store is the AGENT's, and a second one would give the "
                     "documents declared after it a jar the documents declared before it cannot see");
    g_rt = JS_GetRuntime(ctx);
    /* A NULL PROTOTYPE, so a cookie named `toString` or `__proto__` is a cookie and not a collision with
       Object.prototype — §5.2's cookie-name is any run of bytes without "=" or ";", which includes both. */
    g_jar = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(g_jar), "§5.3's cookie store could not be allocated");
    g_ready = 1;
    agent_state_flag("cookie_jar", &g_ready, "the declaration latch");
    agent_state_value("cookie_jar", &g_jar, "RFC 6265 §5.3's cookie store");
    agent_state_ptr("cookie_jar", &g_rt, "the runtime §5.3's store was allocated in");
#if APICLIENT_DEV
    agent_state_ptr("cookie_jar", &g_agent_host, "the agent host the dev-only same-host assertion compares");
#endif
}

void cookie_jar_free(void)
{
    if (!g_ready)
        return;
    DCHECK(g_rt != NULL, "§5.3's store was built without recording the runtime that owns its strings");
    JS_FreeValueRT(g_rt, g_jar);
    g_jar = JS_UNDEFINED;
    g_rt = NULL;
    g_ready = 0;
#if APICLIENT_DEV
    free(g_agent_host);
    g_agent_host = NULL;
    g_agent_secure = -1;
#endif
}
