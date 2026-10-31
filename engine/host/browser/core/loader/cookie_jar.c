/* RFC 6265bis-22 §5 "User Agent Requirements" — the cookie store, its receive (§5.6, §5.7) and its read
 * (§5.8.3). See cookie_jar.h for why the store is the agent's.
 *
 * The store is one JS object with a null prototype, `key -> entry`, built at the agent's declaration (the
 * pre-boot baseline). Platform data a flow queues is a JS value: its mutations are property writes the per-flow
 * COW delta captures, so the arm that ran `document.cookie = "s=1"` is the only arm whose later read sees it,
 * and the store parks and resumes with the flow. A store made on first touch would be made inside one flow and
 * become every other flow's baseline.
 *
 * Entries are replaced, never edited (§5.7 step 23), and the entry text is an immutable JS string, so every
 * write this component makes is a write to the jar, the one object the delta captures.
 *
 * Modelled — §5.7's fields, each because something reads it:
 *   name, value       — §5.8.3 step 6's serialization.
 *   domain, host-only — step 23's identity and §5.8.3 step 3's domain requirement.
 *   path              — step 23's identity, §5.8.3 step 3's path-match and step 4's sort key.
 *   secure-only-flag  — steps 13 and 16, and §5.8.3 step 3's secure requirement.
 *   http-only-flag    — set only by an HTTP receive; every read here is "non-HTTP", and a "non-HTTP" write
 *                       abandons an HttpOnly cookie (step 15) or one replacing an HttpOnly cookie (step 23.2).
 *   expiry-time       — §5.7's "The user agent MUST evict all expired cookies from the cookie store if, at any
 *                       time, an expired cookie exists in the cookie store."
 *   creation-time     — the store's property order: a re-write of an existing key keeps its position, which is
 *                       step 23.3, and a key is never an array index, so §5.8.3 step 4's tie-break is free.
 *
 * Not modelled, each a statement about the standard:
 *   persistent-flag   — read only when "the current session is over", and the session and this store end
 *                       together.
 *   last-access-time  — read only by "remove excess cookies", a MAY this user agent does not take: it would cap
 *                       how much distinct state a run may hold.
 *   same-site-flag    — see cookie_store.c's named residual.
 * This user agent rejects public suffixes (step 9, §5.8.3 step 3) over core/url/public_suffix.h. */
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "solver/engine.h"   /* a jar write survives a fresh load of the page */
#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/frame/secure_context.h"
#include "core/loader/cookie_jar.h"
#include "core/url/origin.h"
#include "core/url/public_suffix.h"
#include "core/url/url.h"
#include <lexbor/core/base.h>   /* lexbor_realloc: the allocator the engine gives its refusal edge */
#include "solver/concolic.h"   /* a value keeps the provenance of the string it was set from */
#include "solver/reclaim.h"

/* ---- the agent's store ------------------------------------------------------------------------------------ */

/* key -> entry. The key is cj_key's string. The entry is a pair: [0] cj_entry's string (the flags, the expiry
   and the value's bytes, which every test below reads) and [1] the value as the JS string it was set from, so
   a value cut from an unknown keeps its provenance into the cookie-string (cj_entry_value). */
static JSValue    g_jar;
static JSRuntime *g_rt;
static int        g_ready;
/* Cookie Store API §7.4 "Process changes"' pending change set, and §2.2 "Cookie store"'s one claimant — see
   the header for both. The set is a JS Array built with the store at the PRE-BOOT BASELINE, so every append is
   an ordinary property write the per-flow COW delta captures: the arm that inserted a cookie is the only arm
   whose §7.4 sees the change, and a flow that parks between the insertion and the dispatch parks with it. */
static JSValue    g_changes;
static CookieJarProcessChanges g_process_changes;

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
    DCHECK(g_ready, "the cookie store was reached before cookie_jar_init built it — §5.7's store is the AGENT's "
                    "and is declared with the platform, so a reader that finds none is running in an agent the "
                    "platform list never declared");
    return g_jar;
}

/* ---- COOKIE STORE API §2.2's TRIGGER AND §7.4's CHANGE SET -------------------------------------------------
 *
 * The classification, the three conditions and why the producer is in this file are all in cookie_jar.h. */

/* ONE member of §7.4's change set — « key, entry, type », this store's own two codec strings plus the
   classification.
   Named residual. Not covered: a change carries its value's bytes, not the JS string it was set from, so a
   `change` event's value cut from an unknown is a concrete string. Next diff builds: the entry pair on the
   record. Absence shows as: a `cookiechange` listener's write of a changed value forking nothing. THE TWO STRINGS ARE THE RECORD'S OWN COPY AND NOT A LOOKUP: a DELETED change names a cookie
   that is no longer in the store, so a filter resolving the key against the jar would find nothing to read for
   exactly the half §2.2's second condition produces. */
static void cj_record_change(JSContext *ctx, const char *key, size_t klen,
                             const char *entry, size_t elen, CookieChangeType type)
{
    JSValue rec, k, e, len;
    uint32_t n = 0;

    DCHECK(g_ready, "a cookie change was recorded before cookie_jar_init built the store");
    rec = JS_NewArray(ctx);
    CHECK(!JS_IsException(rec), "OOM building a Cookie Store API §7.4 cookie change");
    k = JS_NewStringLen(ctx, key, klen);
    e = JS_NewStringLen(ctx, entry, elen);
    CHECK(!JS_IsException(k) && !JS_IsException(e),
          "OOM recording a cookie change's own key and entry, and a change nothing recorded is a page that "
          "never learns its cookie moved");
    CHECK(JS_DefinePropertyValueUint32(ctx, rec, 0, k, JS_PROP_C_W_E) >= 0 &&
          JS_DefinePropertyValueUint32(ctx, rec, 1, e, JS_PROP_C_W_E) >= 0 &&
          JS_DefinePropertyValueUint32(ctx, rec, 2, JS_NewInt32(ctx, (int)type), JS_PROP_C_W_E) >= 0,
          "a cookie change refused its own key, entry and type");
    len = JS_GetPropertyStr(ctx, g_changes, "length");
    CHECK(JS_ToUint32(ctx, &n, len) >= 0, "§7.4's change set has no length");
    JS_FreeValue(ctx, len);
    CHECK(JS_DefinePropertyValueUint32(ctx, g_changes, n, rec, JS_PROP_C_W_E) >= 0,
          "§7.4's change set refused a cookie change");
}

/* §2.2's "perform the steps to process cookie changes", at a moment one of its three conditions has occurred.
   THE SET IS CLEARED WHETHER OR NOT ANYONE IS LISTENING, because it is what the CONDITION produced: a set that
   outlived its condition would be re-offered to every Window at the next one, so a page that set two cookies
   would see the first reported twice. The clear is `length = 0`, which is a property write like every other and
   is therefore captured by the delta exactly as the appends were. */
static void cj_process_changes(JSContext *ctx)
{
    JSValue len = JS_GetPropertyStr(ctx, g_changes, "length");
    uint32_t n = 0;

    CHECK(JS_ToUint32(ctx, &n, len) >= 0, "§7.4's change set has no length");
    JS_FreeValue(ctx, len);
    if (n == 0)
        return;
    /* A STORE WITH NO CLAIMANT IS NOT AN ERROR AND IS NOT A SILENT DROP EITHER: core/platform.c declares this
       row BEFORE the component that owns §7.4, so a cookie received while the platform list is still being
       walked has no §7.4 to run and the set is discarded here. Every later condition has one. */
    if (g_process_changes)
        g_process_changes(ctx);
    CHECK(JS_SetPropertyStr(ctx, g_changes, "length", JS_NewInt32(ctx, 0)) >= 0,
          "§7.4's change set refused to be emptied, and a set that outlives its condition reports one cookie "
          "change twice");
}

void cookie_jar_set_process_changes(CookieJarProcessChanges run)
{
    /* ONE CLAIMANT, AND NULL IS THE RELEASE — core/events/event_target.h's seams state why the two are one
       call: a second claim silently decides what every cookie change in this agent does. */
    DCHECK(run == NULL || g_process_changes == NULL,
           "a second component claimed Cookie Store API §2.2 \"Cookie store\"'s process-cookie-changes steps — "
           "there is one store and one §7.4, and the second claim would replace the first with nothing to say so");
    DCHECK(run != NULL || g_process_changes != NULL,
           "§2.2's process-cookie-changes steps were released by a component that never claimed them");
    g_process_changes = run;
}

/* ---- §5.1's subcomponent algorithms ------------------------------------------------------------------------ */

/* §5.1.2 "Canonicalized Host Names". url.c's parser has already produced it: a domain reaches URL_HOST_DOMAIN
   lowercased and through domain-to-ASCII. The second answer is §5.1.3's "The string is a host name (i.e., not
   an IP address)", read off the parse rather than the text. OWNED. */
static char *cj_request_host(const UrlRecord *u, bool *is_host_name)
{
    char *h;

    DCHECK(u->host.kind != URL_HOST_NULL,
           "§5.7 was handed a request-uri with no host — the only caller is HTML §3.1.4, whose cookie-averse "
           "test has already refused every scheme but http(s), and a special scheme cannot parse without one");
    *is_host_name = (u->host.kind == URL_HOST_DOMAIN);
    h = url_serialize_host(&u->host);
    CHECK(h != NULL, "OOM serializing the canonicalized request-host for §5.1.2");
    return h;
}

/* "Is a public suffix", for §5.7 step 9's domain-attribute and §5.8.3 step 3's stored domain. The bytes are
   page-supplied, so an empty, malformed or IP-address value answers false rather than aborting, and step 10's
   domain-match disposes of it. The list is consulted on a host this engine's parser built. */
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

/* §5.1.3 "Domain Matching", both conditions. */
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
    char *r = reclaim_strdup("/");

    CHECK(r != NULL, "OOM building §5.1.4's default-path");
    return r;
}

/* §5.1.4's DEFAULT-PATH of a request-uri, and PUBLIC because a SECOND STANDARD names it — see the declaration
   in cookie_jar.h for which one and why a copy beside that caller would be the wrong answer. OWNED. */
char *cookie_jar_default_path(const UrlRecord *u)
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
    out = lexbor_malloc(last);
    CHECK(out != NULL, "OOM building §5.1.4's default-path");
    memcpy(out, p, last - 1);
    out[last - 1] = 0;
    free(p);
    return out;
}

/* §5.1.4's PATH-MATCH, all three conditions. */
static bool cj_path_match(const char *req, size_t rlen, const char *cp, size_t clen)
{
    DCHECK(clen > 0, "§5.1.4's path-match was asked about an EMPTY cookie-path — §5.6.4 gives a cookie whose "
                     "Path is empty or does not begin with a solidus the default-path instead, and §5.1.4's "
                     "default-path is at shortest \"/\", so no cookie in this store can have one");
    if (rlen == clen && memcmp(req, cp, clen) == 0)
        return true;                                         /* "identical" */
    if (clen > rlen || memcmp(req, cp, clen) != 0)
        return false;                                        /* not a prefix at all */
    return cp[clen - 1] == '/' || req[clen] == '/';
}

/* A "secure" connection "(as defined by the user agent)", for §5.7 steps 13 and 16 and §5.8.3 step 3. This
   user agent's definition is Secure Contexts §3.1 "Is origin potentially trustworthy?", which admits https and
   the trusted host localhost the standard's note names. */
static bool cj_secure_scheme(const UrlRecord *u)
{
    return secure_context_origin_potentially_trustworthy(u);
}

#if APICLIENT_DEV
static void cj_assert_one_principal(const UrlRecord *u)
{
    bool is_name;
    char *host = cj_request_host(u, &is_name);
    int secure = u->scheme && strcmp(u->scheme, "https") == 0;

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
           "runs §5.8.3 over its own jar, and the flow resumes with the cookie-string");
    free(host);
}
#else
#define cj_assert_one_principal(u) ((void)(u))
#endif

/* ---- the entry codec ---------------------------------------------------------------------------------------
 *
 * KEY   = "<nlen>:<dlen>:<plen>:<host-only>:" name domain path — RFC 6265bis-22 §5.7 "Storage Model" step
 *         23's (name, domain, host-only-flag, path) identity. Length-prefixed because a name and a path are
 *         free-form: any separator can occur inside one. It begins with a digit and contains a colon, so it is
 *         never an array index and the store's property order stays insertion order.
 * ENTRY = <secure><http-only><expiry> ":" value — one character per flag; the expiry is a decimal epoch second
 *         or "*" for step 6's "latest representable date". */

static char *cj_key(const char *name, size_t nlen, const char *dom, size_t dlen, bool host_only,
                    const char *path, size_t plen, size_t *out_len)
{
    char hdr[64];
    int hn = snprintf(hdr, sizeof hdr, "%zu:%zu:%zu:%c:", nlen, dlen, plen, host_only ? '1' : '0');
    size_t total;
    char *k;

    CHECK(hn > 0 && (size_t)hn < sizeof hdr, "a cookie key's length header did not fit — the three lengths are "
                                             "sizes of one set-cookie-string and cannot be that large");
    total = (size_t)hn + nlen + dlen + plen;
    k = lexbor_malloc(total + 1);
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
                         const char **dom, size_t *dlen, bool *host_only, const char **path, size_t *plen)
{
    unsigned long long a = 0, b = 0, c = 0, h = 0;
    const char *p = k, *end = k + klen;
    int field;

    for (field = 0; field < 4; field++) {
        unsigned long long v = 0;
        const char *start = p;
        while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (unsigned)(*p - '0'); p++; }
        DCHECK(p > start && p < end && *p == ':',
               "a cookie store key has no length header — every key is written by cj_key, so a key without one "
               "came from somewhere that is not this codec");
        p++;
        if (field == 0) a = v; else if (field == 1) b = v; else if (field == 2) c = v; else h = v;
    }
    DCHECK(h <= 1, "a cookie store key's host-only field is not a flag — the two halves of one codec disagree");
    DCHECK((size_t)(end - p) == a + b + c,
           "a cookie store key's length header does not describe its own tail — the two halves of one codec "
           "disagree");
    *name = p;        *nlen = (size_t)a;
    *dom  = p + a;    *dlen = (size_t)b;
    *host_only = h == 1;
    *path = p + a + b; *plen = (size_t)c;
}

/* THE LATEST REPRESENTABLE DATE, as this store spells it. Kept out of the number space so no arithmetic on a
   real epoch can collide with it. */
#define CJ_NEVER "*"

static char *cj_entry(bool secure, bool http_only, long long expiry, bool persistent,
                      const char *value, size_t vlen, size_t *out_len)
{
    char hdr[48];
    int hn;
    size_t total;
    char *e;

    if (persistent)
        hn = snprintf(hdr, sizeof hdr, "%c%c%lld:", secure ? '1' : '0', http_only ? '1' : '0', expiry);
    else
        hn = snprintf(hdr, sizeof hdr, "%c%c" CJ_NEVER ":", secure ? '1' : '0', http_only ? '1' : '0');
    CHECK(hn > 0 && (size_t)hn < sizeof hdr, "a cookie entry's header did not fit");
    total = (size_t)hn + vlen;
    e = lexbor_malloc(total + 1);
    CHECK(e != NULL, "OOM building a cookie store entry");
    memcpy(e, hdr, (size_t)hn);
    memcpy(e + hn, value, vlen);
    e[total] = 0;
    *out_len = total;
    return e;
}

static void cj_entry_parse(const char *e, size_t elen, bool *secure, bool *http_only,
                           bool *expired, const char **value, size_t *vlen)
{
    const char *p = e + 2, *end = e + elen;
    long long expiry = 0;
    bool persistent;

    DCHECK(elen >= 4 && (e[0] == '0' || e[0] == '1') && (e[1] == '0' || e[1] == '1'),
           "a cookie store entry has no flag header — the two halves of one codec disagree");
    *secure = e[0] == '1';
    *http_only = e[1] == '1';
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
    /* §5.7: "A cookie is "expired" if the cookie has an expiry date in the past." */
    *expired = persistent && expiry <= (long long)time(NULL);
    *value = p;
    *vlen = (size_t)(end - p);
}

/* A jar entry: the pair of cj_entry's string (`text`, `tlen` bytes) and `value` (consumed). Owned. */
static JSValue cj_entry_pair(JSContext *ctx, const char *text, size_t tlen, JSValue value)
{
    JSValue pair = JS_NewArray(ctx), t = JS_NewStringLen(ctx, text, tlen);

    CHECK(!JS_IsException(pair) && !JS_IsException(t), "OOM building a cookie store entry");
    CHECK(JS_DefinePropertyValueUint32(ctx, pair, 0, t, JS_PROP_C_W_E) >= 0 &&
          JS_DefinePropertyValueUint32(ctx, pair, 1, value, JS_PROP_C_W_E) >= 0,
          "a cookie store entry refused its own text and value");
    return pair;
}

/* An entry's cj_entry string (JS_FreeCString it) and, where `value` is not NULL, its value (owned). The entry is
   this store's own write, so a pair of any other shape is the two halves of one codec disagreeing. */
static const char *cj_entry_text(JSContext *ctx, JSValueConst entry, size_t *tlen, JSValue *value)
{
    JSValue t;
    const char *e;

    DCHECK(JS_IsArray(entry), "a cookie store entry is not the [text, value] pair cj_entry_pair writes");
    t = JS_GetPropertyUint32(ctx, entry, 0);
    e = JS_ToCStringLen(ctx, tlen, t);
    JS_FreeValue(ctx, t);
    CHECK(e != NULL, "a stored cookie could not be read as a string");
    if (value) {
        *value = JS_GetPropertyUint32(ctx, entry, 1);
        DCHECK(JS_IsString(*value) || concolic_is(*value),
               "a cookie store entry's value is neither a string nor an unknown — cj_store writes one or the other");
    }
    return e;
}

/* ---- §5.1.1's PARSE A COOKIE-DATE --------------------------------------------------------------------------- */

/* WSP (RFC 5234's SP / HTAB), which RFC 6265bis-22 §5.6 strips from the name, the value and each cookie-av. */
static bool cj_is_wsp(char c) { return c == ' ' || c == '\t'; }

static void cj_trim(const char **p, size_t *n)
{
    while (*n && cj_is_wsp((*p)[0])) { (*p)++; (*n)--; }
    while (*n && cj_is_wsp((*p)[*n - 1])) (*n)--;
}

/* ASCII case-insensitive equality against a lowercase literal — §5.6 and §5.1.1 compare attribute names and
   month names case-insensitively, and this engine may not assume a locale-aware strcasecmp is on the platform. */
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

/* §5.1.1 "Dates": parse a cookie-date. Returns true and fills `*out` with the epoch time; false is the
   standard's "fail to parse the cookie-date", which §5.6.1 step 2 answers by ignoring the cookie-av. */
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

/* ---- §5.6 "The Set-Cookie Header Field" and §5.7 "Storage Model" ------------------------------------------- */

/* §5.5 "Cookie Lifetime Limits": 400 days, the recommended limit, in seconds. Applied at the store, which
   every route with a lifetime reaches (a Set-Cookie, `document.cookie=`, Cookie Store API §7.2). */
#define CJ_AGE_LIMIT 34560000LL

/* A %x00-08 / %x0A-1F / %x7F octet: §5.6 step 1's and §5.7 step 3's CTL characters excluding HTAB. */
static bool cj_has_ctl(const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c <= 0x1F && c != 0x09) || c == 0x7F) return true;
    }
    return false;
}

/* §5.6's unparsed-attributes algorithm and §5.6.1-§5.6.6, appending to `a` (zeroed by the caller). The
   cookie-attribute-list keeps, per attribute-name, what §5.7 reads of its last member. SameSite (§5.6.7) is
   not recognized; see cookie_store.c's residual. */
static void cj_parse_attrs(const char *attrs, size_t attrs_len, CookieJarAttributes *a)
{
    while (attrs_len) {
        const char *piece = attrs, *aeq;
        size_t plen = attrs_len, alen, vlen;
        const char *aname, *aval;
        const char *sep = memchr(attrs, ';', attrs_len);

        /* Steps 2-4: one cookie-av, split at its first "=". The caller has consumed the leading ";". */
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
        cj_trim(&aname, &alen);      /* step 5 */
        cj_trim(&aval, &vlen);
        if (vlen > 1024) continue;   /* step 6 */
        if (cj_ci_eq(aname, alen, "max-age")) {
            /* §5.6.2: steps 1-3 ignore the cookie-av unless it is DIGITs, optionally after one "-". */
            bool neg = vlen && aval[0] == '-';
            size_t k = neg ? 1 : 0;
            bool ok = vlen > k;
            long long secs = 0;
            for (; k < vlen && ok; k++) {
                if (aval[k] < '0' || aval[k] > '9') ok = false;
                else if (secs <= CJ_AGE_LIMIT) secs = secs * 10 + (aval[k] - '0');
            }
            if (ok) {
                /* Step 6's limit is applied at the store; the digits stop accumulating past it. */
                a->max_age_seen = true;
                a->have_expiry = true;
                a->expiry = (neg || secs == 0) ? LLONG_MIN : (long long)time(NULL) + secs;   /* step 7 */
            }
        } else if (cj_ci_eq(aname, alen, "expires")) {
            /* §5.6.1; step 4's limit is applied at the store. §5.7 step 6 reads Expires only when no Max-Age
               is in the list. */
            long long when = 0;
            if (!a->max_age_seen && cj_parse_cookie_date(aval, vlen, &when)) {
                a->have_expiry = true;
                a->expiry = when;
            }
        } else if (cj_ci_eq(aname, alen, "domain")) {
            /* §5.6.3: a leading "." is dropped and the value appended even when empty, so a later empty
               Domain makes the cookie host-only. The lowercasing happens at the store. */
            bool dot = vlen && aval[0] == '.';
            a->domain = dot ? aval + 1 : aval;
            a->domain_len = dot ? vlen - 1 : vlen;
        } else if (cj_ci_eq(aname, alen, "path")) {
            /* §5.6.4: empty, or not beginning with "/", is the default-path. */
            a->have_path = true;
            a->path_is_default = !(vlen && aval[0] == '/');
            a->path = a->path_is_default ? NULL : aval;
            a->path_len = a->path_is_default ? 0 : vlen;
        } else if (cj_ci_eq(aname, alen, "secure")) {
            a->secure = true;                          /* §5.6.5 */
        } else if (cj_ci_eq(aname, alen, "httponly")) {
            a->http_only = true;                       /* §5.6.6 */
        }
        /* Step 7: "attributes with unrecognized attribute-names are ignored". */
    }
}

static void cj_store(JSContext *ctx, const UrlRecord *uri, const char *name, size_t name_len,
                     const char *value, size_t value_len, JSValue value_js, const CookieJarAttributes *attrs,
                     bool http_api);

/* §5.6's set-cookie-string algorithm, then §5.7 through cj_store. `sc` is the set-cookie-string as the page's
   value (a String or an unknown) for a "non-HTTP" API, JS_UNDEFINED for a response's field; `s` is its bytes,
   which are its example's where `s_is_example`. */
static void cj_receive(JSContext *ctx, const UrlRecord *uri, const char *s, size_t len, bool http_api,
                       JSValueConst sc, bool s_is_example)
{
    JSValue value_js;
    const char *nv = s, *attrs = NULL, *name, *value, *eq;
    size_t nvlen = len, attrs_len = 0, name_len, value_len;
    const char *semi = memchr(s, ';', len);
    CookieJarAttributes a;

    cj_assert_one_principal(uri);
    if (cj_has_ctl(s, len)) return;   /* step 1 */
    /* Step 2: the name-value-pair string, and the unparsed-attributes after the first ";". */
    if (semi) {
        nvlen = (size_t)(semi - s);
        attrs = semi + 1;
        attrs_len = len - nvlen - 1;
    }
    /* Step 3: with no "=", the name is empty and the whole pair is the value. */
    eq = memchr(nv, '=', nvlen);
    name = nv;
    name_len = eq ? (size_t)(eq - nv) : 0;
    value = eq ? eq + 1 : nv;
    value_len = eq ? nvlen - name_len - 1 : nvlen;
    cj_trim(&name, &name_len);        /* step 4 */
    cj_trim(&value, &value_len);
    if (name_len + value_len > 4096) return;   /* step 5 */

    memset(&a, 0, sizeof a);
    cj_parse_attrs(attrs, attrs_len, &a);
    /* The value as a JS string: its bytes, or, cut from an unknown, the unknown's own piece where the value's
       units lie inside one (concolic_piece_slice), else a derivation of the whole set-cookie-string. */
    if (!concolic_is(sc)) {
        value_js = JS_NewStringLen(ctx, value, value_len);
        CHECK(!JS_IsException(value_js), "OOM holding a cookie's value");
    } else {
        JSValue ex = s_is_example ? JS_NewStringLen(ctx, value, value_len) : JS_UNDEFINED;
        uint32_t from = concolic_utf8_units(s, (size_t)(value - s));
        uint32_t to = from + concolic_utf8_units(value, value_len);

        CHECK(!JS_IsException(ex), "OOM holding a cookie's value");
        value_js = s_is_example ? concolic_piece_slice(ctx, sc, from, to, JS_DupValue(ctx, ex)) : JS_UNINITIALIZED;
        if (JS_IsUninitialized(value_js))
            value_js = concolic_new_derived(ctx, "RFC 6265bis-22 §5.6 cookie-value", &sc, 1, ex);
        else
            JS_FreeValue(ctx, ex);
    }
    /* `name`, `value`, `a.domain` and `a.path` point into the caller's string, which outlives this call. */
    cj_store(ctx, uri, name, name_len, value, value_len, value_js, &a, http_api);
}

void cookie_jar_receive(JSContext *ctx, const UrlRecord *uri, JSValueConst set_cookie)
{
    JSValue ex = JS_UNDEFINED;
    const char *s;
    size_t len = 0;
    bool is_example = false;

    if (concolic_is(set_cookie)) {
        /* An unknown's bytes are its example where this flow holds one; with none, its display, as a Text node
           holds for `textContent =`: never a fabricated concrete cookie. */
        if (concolic_example_present(set_cookie)) ex = concolic_example(ctx, set_cookie);
        is_example = JS_IsString(ex);
        if (is_example) {
            s = JS_ToCStringLen(ctx, &len, ex);
        } else {
            const char *sh = concolic_shape_c(set_cookie);
            JSValue t = JS_NewString(ctx, sh ? sh : "");

            s = JS_ToCStringLen(ctx, &len, t);
            JS_FreeValue(ctx, t);
        }
    } else {
        DCHECK(JS_IsString(set_cookie), "document.cookie= reached the jar unconverted");
        s = JS_ToCStringLen(ctx, &len, set_cookie);
    }
    CHECK(s != NULL, "OOM reading a set-cookie-string");
    cj_receive(ctx, uri, s, len, false, set_cookie, is_example);
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, ex);
}

void cookie_jar_receive_http(JSContext *ctx, const UrlRecord *uri, const char *s, size_t len)
{
    cj_receive(ctx, uri, s, len, true, JS_UNDEFINED, false);
}

void cookie_jar_receive_http_pairs(JSContext *ctx, JSValueConst list)
{
    int64_t n = 0, i;

    CHECK(JS_IsArray(list) && JS_GetLength(ctx, list, &n) >= 0,
          "a Set-Cookie list is not an array — the trusted zone states one on every fetch's answer");
    for (i = 0; i < n; i++) {
        JSValue pair = JS_GetPropertyInt64(ctx, list, i);
        JSValue urlv = JS_GetPropertyUint32(ctx, pair, 0), fieldv = JS_GetPropertyUint32(ctx, pair, 1);
        size_t ulen = 0, flen = 0;
        const char *u = JS_IsString(urlv) ? JS_ToCStringLen(ctx, &ulen, urlv) : NULL;
        const char *f = JS_IsString(fieldv) ? JS_ToCStringLen(ctx, &flen, fieldv) : NULL;
        bool parsed, own;
        UrlRecord rec;

        CHECK(u != NULL && f != NULL, "a Set-Cookie list entry is not a [URL, field] pair of strings — the "
                                      "trusted zone writes each as one");
        url_record_init(&rec);
        parsed = url_parse(&rec, u, ulen, NULL);
        own = parsed && origin_same_as_url(origin_agent(), &rec);
        /* The release arm skips the pair: another origin's field is that origin's agent's to store. */
        DCHECK(own, "a Set-Cookie pair reached the jar of an agent its URL's origin is not — the trusted zone "
                    "routes each pair to the instance of its URL's (group, origin) agent cluster");
        if (own) cookie_jar_receive_http(ctx, &rec, f, flen);
        url_record_free(&rec);
        JS_FreeCString(ctx, u);
        JS_FreeCString(ctx, f);
        JS_FreeValue(ctx, urlv);
        JS_FreeValue(ctx, fieldv);
        JS_FreeValue(ctx, pair);
    }
}

void cookie_jar_store(JSContext *ctx, const UrlRecord *uri, const char *name, size_t name_len,
                      const char *value, size_t value_len, const CookieJarAttributes *attrs)
{
    JSValue v = JS_NewStringLen(ctx, value, value_len);

    CHECK(!JS_IsException(v), "OOM holding a cookie's value");
    cj_store(ctx, uri, name, name_len, value, value_len, v, attrs, false);
}

static void cj_store_cookie(JSContext *ctx, const UrlRecord *uri, const char *name, size_t name_len,
                            const char *value, size_t value_len, JSValueConst value_js,
                            const CookieJarAttributes *attrs, bool http_api);

/* §5.7 for one cookie, from an HTTP API (`http_api`, a response's Set-Cookie) or a "non-HTTP" one. `value_js`
   (consumed) is the value as the JS string it was set from; `value` is its bytes. */
static void cj_store(JSContext *ctx, const UrlRecord *uri, const char *name, size_t name_len,
                     const char *value, size_t value_len, JSValue value_js, const CookieJarAttributes *attrs,
                     bool http_api)
{
    cj_store_cookie(ctx, uri, name, name_len, value, value_len, value_js, attrs, http_api);
    JS_FreeValue(ctx, value_js);
}

/* §5.1.3's "the string is a host name (i.e., not an IP address)", for a domain this store holds. */
static bool cj_is_host_name(const char *d, size_t dlen)
{
    UrlHost h;
    bool r = url_parse_host(&h, d, dlen, /*is_opaque*/ false) && h.kind == URL_HOST_DOMAIN;

    url_host_free(&h);
    return r;
}

/* An ASCII case-insensitive "begins with" against a lowercase literal, for §5.7 steps 20-22. */
static bool cj_ci_prefix(const char *s, size_t n, const char *lower)
{
    size_t m = strlen(lower);

    return n >= m && cj_ci_eq(s, m, lower);
}

/* §5.7 step 16's criteria: a stored cookie named `name` with its secure-only-flag set, whose domain
   domain-matches `dom` or the reverse, and whose path the new cookie's `path` path-matches. */
static bool cj_shadows_secure(JSContext *ctx, const char *name, size_t nlen, const char *dom, size_t dlen,
                              const char *path, size_t plen)
{
    JSValueConst jar = cj_jar();
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0, i;
    bool found = false, new_is_name = cj_is_host_name(dom, dlen);

    CHECK(JS_GetOwnPropertyNames(ctx, &tab, &n, jar, JS_GPN_STRING_MASK) == 0,
          "the cookie store could not be enumerated, and nothing of the page's is on it");
    for (i = 0; i < n && !found; i++) {
        size_t klen = 0, elen = 0, onlen, odlen, oplen, ovlen;
        const char *k = JS_AtomToCStringLen(ctx, &klen, tab[i].atom), *e, *on, *od, *op, *ov;
        bool ohost, osec, ohttp, oexp;
        JSValue v;

        CHECK(k != NULL, "a stored cookie's key could not be read as a string");
        cj_key_parse(k, klen, &on, &onlen, &od, &odlen, &ohost, &op, &oplen);
        if (onlen == nlen && memcmp(on, name, nlen) == 0) {
            v = JS_GetProperty(ctx, jar, tab[i].atom);
            CHECK(!JS_IsException(v), "a stored cookie could not be read back");
            e = cj_entry_text(ctx, v, &elen, NULL);
            cj_entry_parse(e, elen, &osec, &ohttp, &oexp, &ov, &ovlen);
            found = osec &&
                    (cj_domain_match(od, odlen, dom, dlen, cj_is_host_name(od, odlen)) ||
                     cj_domain_match(dom, dlen, od, odlen, new_is_name)) &&
                    cj_path_match(path, plen, op, oplen);
            JS_FreeCString(ctx, e);
            JS_FreeValue(ctx, v);
        }
        JS_FreeCString(ctx, k);
    }
    JS_FreePropertyEnum(ctx, tab, n);
    return found;
}

static void cj_store_cookie(JSContext *ctx, const UrlRecord *uri, const char *name, size_t name_len,
                            const char *value, size_t value_len, JSValueConst value_js,
                            const CookieJarAttributes *attrs, bool http_api)
{
    size_t dlen = 0, plen, klen, hostlen;
    char *host, *dom = NULL, *path = NULL, *key;
    bool host_is_name, host_only, secure_conn = cj_secure_scheme(uri);
    long long now = (long long)time(NULL), expiry = attrs->expiry;
    JSValueConst jar = cj_jar();
    JSAtom atom;

    /* Asserted here as well as at the parse: this is a second public entry and the request-uri is its own
       argument. */
    cj_assert_one_principal(uri);
    /* Both callers bound Domain and Path at 1024 octets: §5.6 step 6 and Cookie Store API §7.2 steps 12.7
       and 18-19. */
    DCHECK(attrs->domain_len <= 1024 && attrs->path_len <= 1024,
           "a cookie-attribute-list reached §5.7 with a Domain or Path over 1024 octets — both callers bound them");
    DCHECK(!attrs->path_is_default || attrs->have_path,
           "a cookie-attribute-list's Path is the default-path with no Path attribute in the list");
    /* Step 1 is the user agent's choice, which this one never makes. */
    if (!name_len && !value_len) return;                                    /* step 2 */
    if (cj_has_ctl(name, name_len) || cj_has_ctl(value, value_len)) return; /* step 3 */
    if (name_len + value_len > 4096) return;                                /* step 4 */
    /* Step 5 creates the cookie; its creation-time is the store's property order (step 23.3). Step 6's
       expiry-time, held to §5.5's limit. Step 7: the domain-attribute, lowercased (§5.6.3 step 3). */
    if (attrs->have_expiry && expiry > now + CJ_AGE_LIMIT) expiry = now + CJ_AGE_LIMIT;
    host = cj_request_host(uri, &host_is_name);
    hostlen = strlen(host);
    if (attrs->domain_len) {
        size_t i;
        dom = lexbor_malloc(attrs->domain_len + 1);
        CHECK(dom != NULL, "OOM lowercasing §5.6.3's cookie-domain");
        for (i = 0; i < attrs->domain_len; i++) {
            char c = attrs->domain[i];
            dom[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        dom[attrs->domain_len] = 0;
        dlen = attrs->domain_len;
    }
    /* Step 8: a character that is not in CHAR (%x01-7F). */
    for (size_t i = 0; i < dlen; i++) {
        if (dom[i] == 0 || (unsigned char)dom[i] > 0x7F) goto ignore;
    }
    /* Step 9. This user agent rejects public suffixes, as every browser does. The first arm demotes the cookie
       to host-only: without it `Domain=localhost` on localhost is dropped, since the list's `*` rule makes a
       single-label host its own public suffix. */
    if (dom && cj_domain_attribute_is_public_suffix(dom, dlen)) {
        if (dlen != hostlen || memcmp(dom, host, dlen) != 0) goto ignore;
        free(dom);
        dom = NULL;
        dlen = 0;
    }
    /* Step 10. */
    if (dom) {
        if (!cj_domain_match(host, hostlen, dom, dlen, host_is_name)) goto ignore;
        host_only = false;
    } else {
        dom = host;
        host = NULL;
        dlen = strlen(dom);
        host_only = true;
    }
    /* Step 11. */
    if (attrs->have_path && !attrs->path_is_default) {
        path = lexbor_malloc(attrs->path_len + 1);
        CHECK(path != NULL, "OOM copying §5.6.4's cookie-path");
        memcpy(path, attrs->path, attrs->path_len);
        path[attrs->path_len] = 0;
        plen = attrs->path_len;
    } else {
        path = cookie_jar_default_path(uri);
        plen = strlen(path);
    }
    if (!secure_conn && attrs->secure) goto ignore;                         /* steps 12-13 */
    if (!http_api && attrs->http_only) goto ignore;                         /* steps 14-15 */
    if (!attrs->secure && !secure_conn &&
        cj_shadows_secure(ctx, name, name_len, dom, dlen, path, plen)) goto ignore;   /* step 16 */
    /* Steps 17-19 are SameSite, which this store does not model; see cookie_store.c's residual. */
    if (cj_ci_prefix(name, name_len, "__secure-") && !attrs->secure) goto ignore;      /* step 20 */
    if (cj_ci_prefix(name, name_len, "__host-") &&
        !(attrs->secure && host_only && attrs->have_path && plen == 1 && path[0] == '/')) goto ignore; /* 21 */
    if (!name_len && (cj_ci_prefix(value, value_len, "__secure-") ||
                      cj_ci_prefix(value, value_len, "__host-"))) goto ignore;         /* step 22 */

    key = cj_key(name, name_len, dom, dlen, host_only, path, plen, &klen);
    atom = JS_NewAtomLen(ctx, key, klen);
    CHECK(atom != JS_ATOM_NULL, "document.cookie: a cookie's store key could not be interned");
    /* Step 23.2: a "non-HTTP" API never replaces an HttpOnly cookie. */
    if (!http_api) {
        JSValue old = JS_GetProperty(ctx, jar, atom);
        bool old_http_only = false;

        CHECK(!JS_IsException(old), "a stored cookie could not be read back");
        if (!JS_IsUndefined(old)) {
            size_t olen = 0, ovlen;
            const char *oe = cj_entry_text(ctx, old, &olen, NULL), *ov;
            bool osec, oexp;

            cj_entry_parse(oe, olen, &osec, &old_http_only, &oexp, &ov, &ovlen);
            JS_FreeCString(ctx, oe);
        }
        JS_FreeValue(ctx, old);
        if (old_http_only) {
            JS_FreeAtom(ctx, atom);
            free(key);
            goto ignore;
        }
    }
    /* Steps 23 and 24, then §5.7's eviction rule, "The user agent MUST evict all expired cookies from the
       cookie store if, at any time, an expired cookie exists in the cookie store": a cookie inserted already
       expired is removed at once. A write that survives is JS_SetProperty on the key, which keeps an existing
       key's position in the property order — step 23.3's preserved creation-time. */
    {
        size_t elen;
        char *entry = cj_entry(attrs->secure, http_api && attrs->http_only, expiry,
                               attrs->have_expiry, value, value_len, &elen);
        bool immediately_evicted = attrs->have_expiry && expiry <= now;

        if (immediately_evicted) {
            JS_DeleteProperty(ctx, jar, atom, 0);
        } else {
            JSValue v = cj_entry_pair(ctx, entry, elen, JS_DupValue(ctx, value_js));

            /* A property write on the agent's one jar, so the heap COW captures it for this flow alone. */
            CHECK(JS_SetProperty(ctx, jar, atom, v) >= 0,
                  "document.cookie: the cookie store refused a write, and nothing of the page's is on it");
        }
        /* The jar is the user agent's, so this write survives a fresh load of the page: the load-again act's
           precondition (solver/engine.h). */
        engine_note_persistent_write();
        /* Cookie Store API §2.2's first condition, "A newly-created cookie is inserted into the cookie store",
           classified by §7.4's bullets on whether the new cookie survived: "not immediately evicted is
           considered changed", "immediately evicted is considered deleted". The replaced old cookie records
           nothing: "A cookie which is removed due to an insertion of another cookie with the same name,
           domain, and path is ignored." */
        cj_record_change(ctx, key, klen, entry, elen,
                         immediately_evicted ? COOKIE_CHANGE_DELETED : COOKIE_CHANGE_CHANGED);
        free(entry);
    }
    JS_FreeAtom(ctx, atom);
    free(key);
    free(path);
    free(dom);
    free(host);
    /* §2.2: "perform the steps to process cookie changes", after the frees: §7.4 walks every Window of this
       agent and needs none of this call's storage. */
    cj_process_changes(ctx);
    return;
ignore:
    /* "Abort this algorithm and ignore the cookie entirely." */
    free(path);
    free(dom);
    free(host);
}

/* ---- §5.8.3 "Retrieval Algorithm" ------------------------------------------------------------------------- */

/* One member of §5.8.3 step 3's cookie-list, holding the two C strings it was read out of so that step 4 can
   serialize from them and this component can free exactly what it allocated. */
typedef struct {
    const char *key, *entry;      /* OWNED — JS_FreeCString */
    const char *name;  size_t nlen;
    const char *value; size_t vlen;
    JSValue     val;              /* OWNED — the value as the JS string it was set from */
    size_t      plen;             /* step 4's sort key: the cookie-path's length */
} CjHit;

/* The retrieval's URI, as §5.8.3 step 3's requirements read it: its canonicalized host, its path and whether it
   denotes a secure connection. */
typedef struct {
    char  *host;   size_t hostlen;   bool host_is_name;   /* OWNED */
    char  *path;   size_t path_len;                       /* OWNED */
    bool   secure;
} CjRetrieval;

static void cj_retrieval_init(CjRetrieval *r, const UrlRecord *uri)
{
    cj_assert_one_principal(uri);
    r->host = cj_request_host(uri, &r->host_is_name);     /* steps 1-2 */
    r->hostlen = strlen(r->host);
    r->path = url_serialize_path(uri);
    CHECK(r->path != NULL, "OOM serializing the retrieval's URI's path for §5.8.3's path-match");
    r->path_len = strlen(r->path);
    r->secure = cj_secure_scheme(uri);
}

static void cj_retrieval_free(CjRetrieval *r)
{
    free(r->host);
    free(r->path);
}

/* §5.8.3 step 3's requirements for one stored cookie, for a "non-HTTP" retrieval, which every read of this
   store is: the http-only bullet excludes an HttpOnly cookie from all of them. The same-site bullet is
   unmodelled; see cookie_store.c's residual. Expiry is not among the requirements. */
static bool cj_retrievable(const CjRetrieval *r, const char *dom, size_t dlen, bool host_only,
                           const char *path, size_t plen, bool secure, bool http_only)
{
    bool domain_ok = host_only ? (r->hostlen == dlen && memcmp(r->host, dom, dlen) == 0)
                               : cj_domain_match(r->host, r->hostlen, dom, dlen, r->host_is_name) &&
                                 !cj_domain_attribute_is_public_suffix(dom, dlen);

    return domain_ok && cj_path_match(r->path, r->path_len, path, plen) && (!secure || r->secure) && !http_only;
}

/* §5.8.3 STEPS 1-4 — THE PART TWO STANDARDS SHARE. Collected and sorted once, with step 6's serialization
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
   split correctly today and stops the day §5.8.3 step 6's join changes. The list has no syntax to disagree
   about. Returns the cookie-list, `*out_nhit` long, OWNED — released by cj_hits_free. */
static CjHit *cj_collect(JSContext *ctx, const UrlRecord *uri, uint32_t *out_nhit)
{
    JSValueConst jar = cj_jar();
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0, i;
    CjHit *hits;
    uint32_t nhit = 0;
    CjRetrieval r;

    cj_retrieval_init(&r, uri);

    CHECK(JS_GetOwnPropertyNames(ctx, &tab, &n, jar, JS_GPN_STRING_MASK) == 0,
          "the cookie store could not be enumerated, and nothing of the page's is on it");
    hits = n ? lexbor_calloc(n, sizeof *hits) : NULL;
    CHECK(n == 0 || hits != NULL, "OOM collecting §5.8.3 step 3's cookie-list");
    /* Step 3: the set of cookies that meet all of the requirements. */
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetProperty(ctx, jar, tab[i].atom);
        size_t klen = 0, elen = 0;
        const char *k = JS_AtomToCStringLen(ctx, &klen, tab[i].atom);
        const char *e;
        const char *name, *dom, *path, *value;
        size_t nlen, dlen, plen, vlen;
        bool secure, host_only, http_only, expired, keep;

        JSValue val;

        CHECK(!JS_IsException(v), "a stored cookie could not be read back");
        e = cj_entry_text(ctx, v, &elen, &val);
        JS_FreeValue(ctx, v);
        CHECK(k != NULL, "a stored cookie's key could not be read as a string");
        cj_key_parse(k, klen, &name, &nlen, &dom, &dlen, &host_only, &path, &plen);
        cj_entry_parse(e, elen, &secure, &http_only, &expired, &value, &vlen);
        keep = !expired && cj_retrievable(&r, dom, dlen, host_only, path, plen, secure, http_only);
        /* §5.7's eviction rule, at the one moment an expired cookie is known to exist. It is a property write
           on the jar like any other, so it belongs to the flow that made the read and to no sibling.
           COOKIE STORE API §2.2's SECOND CONDITION, "A user agent evicts expired cookies from the cookie
           store", whose cookie §7.4 classifies under its last bullet: "A cookie which is otherwise evicted or
           removed is considered deleted." */
        if (expired) {
            JS_DeleteProperty(ctx, jar, tab[i].atom, 0);
            cj_record_change(ctx, k, klen, e, elen, COOKIE_CHANGE_DELETED);
        }
        if (!keep) {
            JS_FreeCString(ctx, k);
            JS_FreeCString(ctx, e);
            JS_FreeValue(ctx, val);
            continue;
        }
        hits[nhit].key = k;
        hits[nhit].entry = e;
        hits[nhit].val = val;
        hits[nhit].name = name;   hits[nhit].nlen = nlen;
        hits[nhit].value = value; hits[nhit].vlen = vlen;
        hits[nhit].plen = plen;
        nhit++;
    }
    JS_FreePropertyEnum(ctx, tab, n);
    cj_retrieval_free(&r);

    /* Step 4: "Cookies with longer paths are listed before cookies with shorter paths. Among cookies that have
       equal-length path fields, cookies with earlier creation-times are listed before cookies with later
       creation-times." An INSERTION SORT because it is stable, and the order it is stable in is the store's own
       property order, which is creation order — so the second bullet needs no comparison at all.
       Step 5's last-access-time update has no reader in this engine; see the file header. */
    for (i = 1; i < nhit; i++) {
        CjHit h = hits[i];
        uint32_t j = i;
        while (j > 0 && hits[j - 1].plen < h.plen) { hits[j] = hits[j - 1]; j--; }
        hits[j] = h;
    }
    /* §2.2: "perform the steps to process cookie changes", for whichever expired cookies the walk above
       evicted. ONE trigger for the batch, because the condition §2.2 names is "evicts expired cookieS" — the
       eviction rule is one sentence about every expired cookie in the store, not one occurrence per cookie.
       IT IS SAFE HERE AND THAT IS WORTH STATING, because §7.4 runs during what is otherwise a pure read: its
       step 1.4 QUEUES a global task rather than dispatching one (core/events/event_target.h's queued reach), so
       no listener body runs before this function returns and nothing re-enters this walk. What it does read is
       §7.4's own change set, never the jar. */
    cj_process_changes(ctx);
    *out_nhit = nhit;
    return hits;
}

static void cj_hits_free(JSContext *ctx, CjHit *hits, uint32_t nhit)
{
    uint32_t i;

    for (i = 0; i < nhit; i++) {
        JS_FreeCString(ctx, hits[i].key);
        JS_FreeCString(ctx, hits[i].entry);
        JS_FreeValue(ctx, hits[i].val);
    }
    free(hits);
}

JSValue cookie_jar_cookie_string(JSContext *ctx, const UrlRecord *uri)
{
    uint32_t nhit = 0, i;
    CjHit *hits = cj_collect(ctx, uri, &nhit);
    JSValue out = JS_NewStringLen(ctx, "", 0);

    CHECK(!JS_IsException(out), "document.cookie: the cookie string could not be allocated");
    /* Step 6: a non-empty name and "=", the value, and "; " before every cookie but the first. Joined as
       strings, so a value set from an unknown stays that unknown's piece of the result; an empty value adds
       nothing either way. */
    for (i = 0; i < nhit; i++) {
        size_t plen = (i ? 2 : 0) + hits[i].nlen + (hits[i].nlen ? 1 : 0);
        char *pre = lexbor_malloc(plen + 1);
        JSValue p;

        CHECK(pre != NULL, "document.cookie: OOM building the cookie string");
        memcpy(pre, "; ", i ? 2 : 0);
        memcpy(pre + (i ? 2 : 0), hits[i].name, hits[i].nlen);
        if (hits[i].nlen) pre[plen - 1] = '=';
        p = JS_NewStringLen(ctx, pre, plen);
        free(pre);
        CHECK(!JS_IsException(p), "document.cookie: OOM building the cookie string");
        out = concolic_string_concat(ctx, out, p);
        out = concolic_string_concat(ctx, out, JS_DupValue(ctx, hits[i].val));
    }
    cj_hits_free(ctx, hits, nhit);
    return out;
}

JSValue cookie_jar_cookie_list(JSContext *ctx, const UrlRecord *uri)
{
    uint32_t nhit = 0, i;
    CjHit *hits = cj_collect(ctx, uri, &nhit);
    JSValue list = JS_NewArray(ctx);

    CHECK(!JS_IsException(list), "OOM building §5.8.3 step 3's cookie-list");
    for (i = 0; i < nhit; i++) {
        JSValue pair = JS_NewArray(ctx);
        JSValue name, value;

        CHECK(!JS_IsException(pair), "OOM building a cookie-list entry");
        name = JS_NewStringLen(ctx, hits[i].name, hits[i].nlen);
        value = JS_DupValue(ctx, hits[i].val);
        CHECK(!JS_IsException(name) && !JS_IsException(value),
              "OOM building a cookie-list entry's name and value");
        /* §5.7's stored fields, decoded where the standard that reads them decodes: Cookie Store API §7.1's
           "create a CookieListItem" runs UTF-8 decode without BOM on each of the two, and JS_NewStringLen is
           that decode; the value is the JS string it was set from, which that decode of its bytes is. The
           other five stored fields are deliberately not here — see cookie_jar.h. */
        CHECK(JS_DefinePropertyValueUint32(ctx, pair, 0, name, JS_PROP_C_W_E) >= 0 &&
              JS_DefinePropertyValueUint32(ctx, pair, 1, value, JS_PROP_C_W_E) >= 0,
              "a cookie-list entry refused its own name and value");
        CHECK(JS_DefinePropertyValueUint32(ctx, list, i, pair, JS_PROP_C_W_E) >= 0,
              "§5.8.3 step 3's cookie-list refused an entry");
    }
    cj_hits_free(ctx, hits, nhit);
    return list;
}

/* Cookie Store API §7.4's `observable changes for url` — RFC 6265bis-22 §5.8.3 step 3's requirements over §7.4's
   change set rather than over the store. Why there is no expiry test, why the http-only requirement is vacuous here and
   what the returned shape is are all in cookie_jar.h. */
JSValue cookie_jar_observable_changes(JSContext *ctx, const UrlRecord *uri)
{
    JSValue out = JS_NewArray(ctx), len;
    CjRetrieval r;
    uint32_t i, n = 0, nout = 0;

    CHECK(!JS_IsException(out), "OOM building §7.4's observable changes");
    cj_retrieval_init(&r, uri);

    len = JS_GetPropertyStr(ctx, g_changes, "length");
    CHECK(JS_ToUint32(ctx, &n, len) >= 0, "§7.4's change set has no length");
    JS_FreeValue(ctx, len);
    for (i = 0; i < n; i++) {
        JSValue rec = JS_GetPropertyUint32(ctx, g_changes, i);
        JSValue kv, ev, tv, pair, nameval, valueval;
        size_t klen = 0, elen = 0;
        const char *k, *e;
        const char *name, *dom, *path, *value;
        size_t nlen, dlen, plen, vlen;
        bool secure, host_only, http_only, expired;
        int32_t type = 0;

        CHECK(JS_IsArray(rec), "a member of §7.4's change set is not a « key, entry, type » record — every "
                               "member is written by cj_record_change");
        kv = JS_GetPropertyUint32(ctx, rec, 0);
        ev = JS_GetPropertyUint32(ctx, rec, 1);
        tv = JS_GetPropertyUint32(ctx, rec, 2);
        JS_FreeValue(ctx, rec);
        k = JS_ToCStringLen(ctx, &klen, kv);
        e = JS_ToCStringLen(ctx, &elen, ev);
        CHECK(k != NULL && e != NULL && JS_ToInt32(ctx, &type, tv) >= 0,
              "a cookie change could not be read back as the key, entry and type it was recorded with");
        JS_FreeValue(ctx, kv);
        JS_FreeValue(ctx, ev);
        JS_FreeValue(ctx, tv);
        DCHECK(type == COOKIE_CHANGE_CHANGED || type == COOKIE_CHANGE_DELETED,
               "a cookie change carries a type that is neither of §7.4's two — the enumeration is this store's "
               "own and cj_record_change is the only writer of it");
        cj_key_parse(k, klen, &name, &nlen, &dom, &dlen, &host_only, &path, &plen);
        cj_entry_parse(e, elen, &secure, &http_only, &expired, &value, &vlen);
        /* `expired` is deliberately not consulted — see cookie_jar.h. */
        if (!cj_retrievable(&r, dom, dlen, host_only, path, plen, secure, http_only)) {
            JS_FreeCString(ctx, k);
            JS_FreeCString(ctx, e);
            continue;
        }
        pair = JS_NewArray(ctx);
        CHECK(!JS_IsException(pair), "OOM building an observable change");
        /* §5.7's stored fields, decoded where the standard that reads them decodes — §7.1's `create a
           CookieListItem` runs UTF-8 decode without BOM on each of the two, and JS_NewStringLen is that
           decode. Same answer, same reason, as cookie_jar_cookie_list's entries. */
        nameval = JS_NewStringLen(ctx, name, nlen);
        valueval = JS_NewStringLen(ctx, value, vlen);
        CHECK(!JS_IsException(nameval) && !JS_IsException(valueval),
              "OOM building an observable change's name and value");
        CHECK(JS_DefinePropertyValueUint32(ctx, pair, 0, nameval, JS_PROP_C_W_E) >= 0 &&
              JS_DefinePropertyValueUint32(ctx, pair, 1, valueval, JS_PROP_C_W_E) >= 0 &&
              JS_DefinePropertyValueUint32(ctx, pair, 2, JS_NewInt32(ctx, type), JS_PROP_C_W_E) >= 0,
              "an observable change refused its own name, value and type");
        CHECK(JS_DefinePropertyValueUint32(ctx, out, nout++, pair, JS_PROP_C_W_E) >= 0,
              "§7.4's observable changes refused an entry");
        JS_FreeCString(ctx, k);
        JS_FreeCString(ctx, e);
    }
    cj_retrieval_free(&r);
    return out;
}

/* ---- the agent's declaration and teardown ------------------------------------------------------------------ */

void cookie_jar_init(JSContext *ctx)
{
    DCHECK(!g_ready, "cookie_jar_init ran twice — §5.7's store is the AGENT's, and a second one would give the "
                     "documents declared after it a jar the documents declared before it cannot see");
    g_rt = JS_GetRuntime(ctx);
    /* A NULL PROTOTYPE, so a cookie named `toString` or `__proto__` is a cookie and not a collision with
       Object.prototype — §5.6's cookie-name is any run of bytes without "=" or ";", which includes both. */
    g_jar = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(g_jar), "§5.7's cookie store could not be allocated");
    /* §7.4's change set, built HERE for the reason the store is: a per-flow COW delta captures a property write
       on an object that exists at the pre-boot baseline, and an array minted inside whichever flow first
       inserted a cookie would be that flow's private creation. */
    g_changes = JS_NewArray(ctx);
    CHECK(!JS_IsException(g_changes), "Cookie Store API §7.4's change set could not be allocated");
    g_ready = 1;
    agent_state_flag("cookie_jar", &g_ready, "the declaration latch");
    agent_state_value("cookie_jar", &g_jar, "RFC 6265bis-22 §5.7's cookie store");
    agent_state_value("cookie_jar", &g_changes, "Cookie Store API §7.4's pending cookie-change set");
    agent_state_ptr("cookie_jar", &g_rt, "the runtime §5.7's store was allocated in");
#if APICLIENT_DEV
    agent_state_ptr("cookie_jar", &g_agent_host, "the agent host the dev-only same-host assertion compares");
#endif
}

void cookie_jar_free(void)
{
    if (!g_ready)
        return;
    DCHECK(g_rt != NULL, "§5.7's store was built without recording the runtime that owns its strings");
    JS_FreeValueRT(g_rt, g_jar);
    g_jar = JS_UNDEFINED;
    JS_FreeValueRT(g_rt, g_changes);
    g_changes = JS_UNDEFINED;
    /* NOT given back here. The claimant is a component whose OWN release calls
       cookie_jar_set_process_changes(NULL) — core/events/event_target.h's seams are released the same way, and
       a store that cleared the pointer itself would leave that component's release asserting against a claim it
       really did make. */
    g_rt = NULL;
    g_ready = 0;
#if APICLIENT_DEV
    free(g_agent_host);
    g_agent_host = NULL;
    g_agent_secure = -1;
#endif
}
