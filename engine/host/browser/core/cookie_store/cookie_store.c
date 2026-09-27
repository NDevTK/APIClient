/* COOKIE STORE API §3 The CookieStore interface — all four of its methods: §3.1's get and §3.2's getAll over
 * §7.1 "Query cookies", §3.3's set over §7.2 "Set a cookie", and §3.4's delete over §7.3 "Delete a cookie",
 * whose step 4 runs §7.2 as well. See cookie_store.h for which standard this is, where it moved to, and what
 * is not here.
 *
 * WHY THE QUERY HALF LANDED ALONE, AND IT IS A SUBPROBLEM ORDER RATHER THAN A CONVENIENCE. §7.2 "Set a cookie"
 * step 12.3 refuses a Domain that "is not a registrable domain suffix of and is not equal to host". That
 * predicate is HTML §7.1.1.2's `is a registrable domain suffix of or is equal to`, and it was PRIVATE — static
 * inside core/dom/document_domain.c, whose header exports only init, install and free — so §7.2 was blocked on
 * extracting it into a component both callers share, while §7.1 was blocked on nothing because it reads RFC
 * 6265 §5.4, which the jar implements whole.
 *
 * THAT BLOCKER IS GONE: the predicate is core/url/registrable_domain.h and it is reached, not re-stated.
 * WRITING A SECOND COPY HERE REMAINS THE ANSWER THAT MUST NOT BE TAKEN, and the reason outlives the blocker —
 * it is one fact stated twice over a 10239-rule table that changes several times a week, which is the shape
 * that drifts, and a wrong answer about which registrable domain a host belongs to is a security answer rather
 * than a near miss.
 *
 * THAT REASON IS THE SECOND ONE THIS COMPONENT HAD, AND THE FIRST IS KEPT BECAUSE A READER WHO RE-DERIVES IT
 * RE-INTRODUCES IT. The first said the Public Suffix List was data this engine is not given, which is what
 * cookie_jar.c's header said at the time and was false when read; it reached a landed commit message before a
 * grep of the clause caught it. The conclusion survived the correction and the argument did not, which is why
 * the argument is written out here rather than summarised: a reader who checks a wrong argument discards the
 * conclusion with it.
 *
 * NOTHING HERE SUSPENDS, AND THAT IS WHY NONE OF IT IS A STEP MACHINE. §3.1 step 6 runs its work "in parallel"
 * and settles a promise; in this engine the work is a walk of an in-memory jar, so it completes within the call
 * and the promise is settled before the member returns. The page cannot observe the difference — a promise
 * settled during the call still delivers its reaction as a JOB, which is a first-class flow on the one WFQ —
 * and a step machine would be a continuation-holding builtin with no continuation to hold. What DOES run the
 * page's code is the argument conversion, and that is the declaration's work rather than this body's: a
 * dictionary member's getter runs as a rest point of §3.2.17's walk, before any line below executes. */
#include <limits.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/cookie_store/cookie_store.h"
#include "core/dom/document.h"
#include "core/events/event_target.h"
#include "core/frame/window_proxy.h"
#include "core/idl_args.h"
#include "core/loader/cookie_jar.h"
#include "core/realm.h"
#include "core/url/origin.h"
#include "core/url/registrable_domain.h"
#include "core/url/url.h"
#include "solver/concolic.h"

#define CS_COMPONENT "cookie_store"
/* THE SOURCE IDENTITY, and it is the member's own name rather than `document.cookie`'s because a candidate run
   substitutes BY identity: a PoC that fires through `cookieStore.get(...).value` is delivered and reproduced
   through this member, and one that fires through `document.cookie` is not. See the declaration in
   cookie_store_init for the character set, which is the SAME RFC 6265 fact stated in a second place. */
#define CS_SOURCE "cookieStore"

static JSClassID g_cs_class;
static JSClassID g_obj_slot = JS_INVALID_CLASS_ID;
static int       g_id_get = -1;
static int       g_id_get_all = -1;
static int       g_id_delete = -1;
static int       g_id_set = -1;

/* Which member this invocation is — §3.1 and §3.2 differ in three steps and share every other, so they are one
   body and one magic rather than two copies of §7.1's caller. */
#define CS_GET     0
#define CS_GET_ALL 1

/* §3's `dictionary CookieStoreGetOptions { USVString name; USVString url; };` — two OPTIONAL members with no
   default, which is what §3.1 step 5's "If options is empty" then reads: an absent member is absent, so a
   dictionary with neither present is the empty one and a `get()` with no argument is a rejection rather than a
   query for everything. Declared in Web IDL §3.2.17's read order. */
static const IdlDictMember COOKIE_STORE_GET_OPTIONS[] = {
    { "name", IDL_USVSTRING },
    { "url",  IDL_USVSTRING },
};

/* §3's `dictionary CookieStoreDeleteOptions { required USVString name; USVString? domain = null; USVString path
   = "/"; boolean partitioned = false; };` — ONE required member and three carrying the IDL's own defaults, so
   §3.2.17 step 4.1.3 throws for `delete({})` and step 4.1.5 PLACES the other three for `delete({name:"x"})`.
   IN WEB IDL §3.2.17's READ ORDER, WHICH IS LEXICOGRAPHIC AND NOT THE IDL's PRINTED ORDER — step 4 is "For each
   dictionary member member declared on dictionary, in lexicographical order", so `domain` is read before
   `name` even though the IDL writes `name` first. The order is OBSERVABLE: a page passing an object of getters
   counts which ran, and this dictionary inherits nothing, so the lexicographic rule is the whole of it.
   Do not re-sort these to match the printed IDL. */
static const IdlDictMember COOKIE_STORE_DELETE_OPTIONS[] = {
    { "domain",      IDL_USVSTRING_NULLABLE, false, NULL, 0, NULL, IDL_DEFAULT_NULL },
    { "name",        IDL_USVSTRING,          true },
    { "partitioned", IDL_BOOLEAN,            false, NULL, 0, NULL, IDL_DEFAULT_FALSE },
    { "path",        IDL_USVSTRING,          false, NULL, 0, NULL, IDL_DEFAULT_STRING, "/" },
};

/* §3's `enum CookieSameSite { "strict", "lax", "none" };` — the three §7.2 step 22 switches on, so a fourth
   string is Web IDL §3.2.18's TypeError at the declaration and never reaches that switch. */
IDL_ENUM_VALUES(COOKIE_SAME_SITE, "strict", "lax", "none");

/* §3's `dictionary CookieInit` — EIGHT members, two of them `required`, IN WEB IDL §3.2.17's LEXICOGRAPHIC READ
   ORDER and not the IDL's printed order, for the reason COOKIE_STORE_DELETE_OPTIONS states above: step 4 reads
   "in lexicographical order", and which getter of a page-supplied object runs first is observable. The IDL
   writes name, value, expires, domain, path, sameSite, partitioned, maxAge; the order below is the read order.
   Do not re-sort these to match the printed IDL.
   `expires` AND `maxAge` ARE THE TWO NULLABLE NUMERICS, and their null is not their zero — see the rows in
   core/idl_args.h, whose whole reason is §7.2 step 13's "If expires is non-null" and §7.3 step 4's maxAge of
   exactly 0, which step 14 reads as NON-null and which is what makes a delete a delete. */
static const IdlDictMember COOKIE_INIT[] = {
    { "domain",      IDL_USVSTRING_NULLABLE,  false, NULL, 0, NULL, IDL_DEFAULT_NULL },
    { "expires",     IDL_DOUBLE_NULLABLE,     false, NULL, 0, NULL, IDL_DEFAULT_NULL },
    { "maxAge",      IDL_LONG_LONG_NULLABLE,  false, NULL, 0, NULL, IDL_DEFAULT_NULL },
    { "name",        IDL_USVSTRING,           true },
    { "partitioned", IDL_BOOLEAN,             false, NULL, 0, NULL, IDL_DEFAULT_FALSE },
    { "path",        IDL_USVSTRING,           false, NULL, 0, NULL, IDL_DEFAULT_STRING, "/" },
    { "sameSite",    IDL_ENUM,                false, COOKIE_SAME_SITE, 0, NULL, IDL_DEFAULT_STRING, "strict" },
    { "value",       IDL_USVSTRING,           true },
};

/* WEB IDL §3.7.7's brand check, and it THROWS rather than asserting. A receiver is PAGE-SUPPLIED INPUT — the
   page writes `CookieStore.prototype.get.call(null)` — so an assert here would hand any document an abort
   switch over this engine, which is the one thing §Offensive-programming's "assert only what you computed"
   forbids. §3.7.7 wraps the brand check in its Try, and this member declares idl_returns_promise, so the
   TypeError becomes a REJECTED promise exactly as that section requires. */
static bool cs_brand(JSContext *ctx, JSValueConst this_val)
{
    if (JS_GetClassID(this_val) == g_cs_class)
        return true;
    JS_ThrowTypeError(ctx, "CookieStore.prototype method called on a receiver that is not a CookieStore");
    return false;
}

/* §3.1 step 4's `settings's creation URL`, as the REQUEST-URI RFC 6265 §5.4 is computed against. It is this
   realm's document address: `cookieStore` is a per-realm object, so `this`'s relevant settings object is the
   settings object of the realm this member was installed on, and a child navigable's store answers for the
   child. Returns false for a document with no usable address; `*rec` is initialised either way and the caller
   ALWAYS frees it. */
static bool cs_request_uri(JSContext *ctx, UrlRecord *rec)
{
    const char *url = document_url(ctx);

    url_record_init(rec);
    if (!url || !*url)
        return false;
    CHECK(url_parse(rec, url, strlen(url), NULL),
          "a document's own address is not a URL — the host captured something this engine cannot make a "
          "principal out of");
    /* RFC 6265's store is reached for http(s) alone, which is the same condition HTML §3.1.4's cookie-averse
       test applies to `document.cookie` and the same one cookie_jar.c's request-host assert relies on. */
    return rec->scheme && (strcmp(rec->scheme, "http") == 0 || strcmp(rec->scheme, "https") == 0);
}

/* §7.1's `create a CookieListItem` — steps 1 to 3, over one « name, value » pair of the jar's cookie-list.
 *
 * IT RETURNS EXACTLY TWO MEMBERS BECAUSE THE DICTIONARY HAS EXACTLY TWO. §7.1's step 3 is «[ "name" → name,
 * "value" → value ]» and §3's IDL is `dictionary CookieListItem { USVString name; USVString value; };`. The
 * `domain`, `path`, `expires`, `secure` and `sameSite` of earlier drafts are GONE, and the standard's own Note
 * beside that step says so: "One implementation is known to expose information beyond _name_ and _value_." A
 * fifth field added here would be this engine becoming that implementation.
 *
 * THE VALUE IS THE SOURCE AND THE NAME IS NOT, which is a claim about which of the two an attacker writes.
 * A cookie's VALUE is attacker-controlled — RFC 6265 gives a sibling subdomain a write to this jar, and a
 * planted cookie is read by a later load — so it is minted through the one seam solver/concolic.h names and
 * the auth gate a bundle writes over it FORKS. The NAME is not left concrete for want of rigour: for `get(n)`
 * and `getAll(n)` §7.1 step 3.2.3 has already CONTINUED past every cookie whose name is not `n`, so the name
 * on every surviving item is a value this run compared and pinned — wrapping it would make `item.name === n`
 * fork a world in which a cookie the filter just matched does not match it. For the unfiltered `getAll()` the
 * name is a jar key this component enumerated out of the store's own property order. Both are values this
 * codebase computed, which is the line §Offensive-programming draws. */
static JSValue cs_item(JSContext *ctx, JSValueConst pair)
{
    JSValue item = JS_NewObject(ctx);
    JSValue name, value;

    CHECK(!JS_IsException(item), "OOM building a CookieListItem");
    name = JS_GetPropertyUint32(ctx, pair, 0);
    value = JS_GetPropertyUint32(ctx, pair, 1);
    CHECK(!JS_IsException(name) && !JS_IsException(value),
          "a cookie-list entry did not carry the two strings cookie_jar_cookie_list puts on it");
    value = concolic_source_wrap(ctx, "{cookieStore}", CS_SOURCE, value);
    CHECK(JS_SetPropertyStr(ctx, item, "name", name) >= 0 &&
          JS_SetPropertyStr(ctx, item, "value", value) >= 0,
          "a CookieListItem refused its own name and value");
    return item;
}

/* ---- §7.2 "Set a cookie" AND §7.3 "Delete a cookie" -------------------------------------------------------- */

/* §7.2's NINE PARAMETERS, in the standard's own order: "To set a cookie given a URL url, scalar value string
 * name, scalar value string value, DOMHighResTimeStamp-or-null expires, scalar value string-or-null domain,
 * scalar value string path, string sameSite, boolean partitioned, and 64-bit signed integer-or-null maxAge".
 *
 * EACH NULLABLE IS A VALUE AND A FLAG, NEVER A SENTINEL, because every one of them has a value the IDL admits
 * that a sentinel would have to stand for: §7.3 step 4 passes a `maxAge` of exactly 0 and step 14 reads it as
 * NON-null, an `expires` of 0 is the epoch, and an empty `domain` is a string step 12 would run. An absent one
 * is a POSITIVE STATEMENT here, which is §A-FIELD-A-CONSUMER-DEFAULTS' rule arriving at a C boundary.
 *
 * `name`, `value`, `domain` AND `path` ARE THE PAGE'S BYTES AND ARE BORROWED. Nothing below asserts anything
 * about their contents: a cookie's bytes are a page's or a server's, so a DCHECK on one hands a remote party an
 * abort switch over this zone. Every refusal here is §7.2's own `return failure`, and what this file asserts is
 * only what it COMPUTED. */
typedef struct {
    const char *name;   size_t name_len;
    const char *value;  size_t value_len;
    bool        have_expires;   double      expires;   /* MILLISECONDS, per DOMHighResTimeStamp */
    bool        have_domain;    const char *domain; size_t domain_len;
    const char *path;   size_t path_len;
    const char *same_site;                             /* one of CookieSameSite's three strings */
    bool        partitioned;
    /* SECONDS, a delta. `int64_t` IS §7.2's "64-bit signed integer" and is the type JS_ToInt64 writes; the
       jar's own expiry-time is a `long long`, so the widening to it is explicit at the one place they meet.
       On every target this engine builds for the two are both exactly 64-bit signed and the conversion is
       value-preserving — they are nonetheless DISTINCT TYPES, which is what the compiler said when this field
       was declared as the jar's and the address of it was handed to §3.2.4.7's conversion. */
    bool        have_max_age;   int64_t     max_age;
} CsCookie;

/* §2.1 Cookie's "To normalize a cookie name or value given a string input: remove all U+0009 TAB and U+0020
   SPACE that are at the start or end of input" — §7.2 steps 1 and 2.
   IT IS THE SAME TWO CHARACTERS RFC 6265's WSP NAMES AND IT IS A DIFFERENT STANDARD'S ALGORITHM, which is why
   it is stated here rather than reached: the jar trims a set-cookie-string's fields on §5.2 step 5's authority
   over a string IT parsed, and this trims a dictionary member on §2.1's. The two agreeing today is a fact about
   two character sets and not a shared definition, so neither may be routed to the other. */
static void cs_normalize(const char **p, size_t *n)
{
    while (*n && ((*p)[0] == '\t' || (*p)[0] == ' ')) { (*p)++; (*n)--; }
    while (*n && ((*p)[*n - 1] == '\t' || (*p)[*n - 1] == ' ')) (*n)--;
}

/* INFRA's BYTE-LOWERCASED PREFIX TEST, over the ASCII-lowercase literal §7.2 spells — steps 5.3, 6, 12.2 and 17
   each ask it of a name or a value the page wrote. `lower` is this file's own byte string. */
static bool cs_starts_lowered(const char *s, size_t n, const char *lower)
{
    size_t k = strlen(lower), i;

    if (n < k)
        return false;
    for (i = 0; i < k; i++) {
        char c = s[i];

        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != lower[i])
            return false;
    }
    return true;
}

/* §7.2 step 3's CHARACTER REFUSAL, asked of the name and of the value: "If name or value contain U+003B (;), any
   C0 control character except U+0009 TAB, or U+007F DELETE, then return failure." A SCALAR VALUE STRING is
   scanned as BYTES here and that is exact rather than approximate: every code point this refuses is ASCII, and
   UTF-8 encodes every non-ASCII scalar value entirely in bytes >= 0x80, so no multi-byte sequence can contain
   one of these bytes. */
static bool cs_has_refused_byte(const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c == ';' || c == 0x7f)
            return true;
        if (c <= 0x1f && c != '\t')
            return true;
    }
    return false;
}

/* §7.2 step 13.2's `expires (date serialized)` AND §5.3's EXPIRY-TIME, WITHOUT THE COOKIE-DATE BETWEEN THEM.
 *
 * §7 "Algorithms" defines the serialization: "To date serialize a DOMHighResTimeStamp millis, let dateTime be
 * the date and time millis milliseconds after 00:00:00 UTC, 1 January 1970 (assuming that there are exactly
 * 86,400,000 milliseconds per day), and return a byte sequence corresponding to the closest cookie-date
 * representation of dateTime according to Cookies § Dates." Its consumer is RFC 6265 §5.2.1, which PARSES that
 * byte sequence straight back into an expiry-time — so the pair composes to `millis` at the resolution a
 * cookie-date has, which is ONE SECOND. Spelling the cookie-date and re-reading it is the round trip
 * cookie_jar.h's cookie_jar_store declaration argues against at length, and it would buy nothing here: the
 * jar's attribute-list carries the expiry-time ITSELF, in seconds, which is the far end of that trip.
 * "CLOSEST" IS A ROUNDING AND NOT A TRUNCATION, which is the one place the two differ — 1500 ms is second 2 and
 * not second 1 — and it is the standard's own word.
 *
 * AND IT SATURATES, ON RFC 6265 §5.2.1's OWN PERMISSION: "If the expiry-time is later than the last date the
 * user agent can represent, the user agent MAY replace the expiry-time with the last representable date." A
 * `DOMHighResTimeStamp` is a double, so a page may write 1e300 and the seconds it names do not fit a `long
 * long` at all — and a C conversion of an out-of-range double is UNDEFINED rather than merely wrong, so the
 * band is tested BEFORE the cast and never after it. The floor is the same permission read downward, which is
 * also the earliest-representable date §5.2.2 names for a non-positive Max-Age. */
static long long cs_expiry_seconds(double millis)
{
    double secs;

    DCHECK(isfinite(millis),
           "§7.2's `expires` reached the date serialization non-finite — DOMHighResTimeStamp is HR-TIME §5's "
           "`typedef double`, the RESTRICTED type, so Web IDL §3.2.7's conversion has already refused NaN and "
           "the infinities with a TypeError before any step of this algorithm ran");
    secs = millis / 1000.0;
    secs = secs < 0 ? -floor(-secs + 0.5) : floor(secs + 0.5);
    if (secs >= 9.2233720368547748e18)
        return LLONG_MAX;
    if (secs <= -9.2233720368547748e18)
        return LLONG_MIN;
    return (long long)secs;
}

/* §7.2 "Set a cookie", whole. Returns false for the standard's FAILURE, which §3.3 step 6.2 and §3.4 step 6.2
 * both turn into a TypeError on the promise.
 *
 * ITS LAST STEP BUT ONE IS THE SEAM AND NOT A SERIALIZATION. Step 24 is "Perform the steps defined in Cookies
 * § Storage Model for when the user agent 'receives a cookie' with url as request-uri, encodedName as
 * cookie-name, encodedValue as cookie-value, and attributes as cookie-attribute-list", and those four are
 * `cookie_jar_store`'s signature exactly. §5.2 is named nowhere in that step, because its input is a header
 * field and this algorithm's is a dictionary — see cookie_jar.h for what routing through the string form would
 * MANUFACTURE past step 12.3.
 *
 * TWO OF ITS ATTRIBUTES REACH A STORE THAT MODELS NO FIELD FOR THEM, AND THAT IS RFC 6265 §5.2 STEP 6's OWN
 * ANSWER RATHER THAN A DROP — "Notice that attributes with unrecognized attribute-names are ignored". The jar
 * is RFC 6265's store and recognizes Expires, Max-Age, Domain, Path, Secure and HttpOnly; SameSite and
 * Partitioned are neither, so a cookie-attribute-list carrying them stores exactly as one without them does.
 * NAMED RESIDUAL. NOT COVERED: step 22's SameSite and step 23's Partitioned, so `sameSite` and `partitioned`
 * are converted, refused when their types refuse them, and then change nothing about which entry this store
 * holds or which request reads it back. NEXT DIFF: a SameSite flag on `CookieJarAttributes` read by §5.4's
 * collect against the request's own site, and a partition key folded into the store's (name, domain, path) key
 * — which is a change to what a cookie's IDENTITY is and therefore to `cj_key` and to every read, not a field.
 * HOW ITS ABSENCE WOULD SHOW: a reader would observe two `set` calls that differ ONLY in `partitioned`
 * overwriting one another in this store, where a browser keeps a partitioned cookie and an unpartitioned one
 * apart; and §3.4's own two entries, which pass `true` and `partitioned` respectively, deleting the same entry.
 * A field on that struct with no reader would be the write-with-no-reader §A-FIELD-A-CONSUMER-DEFAULTS names,
 * so the attributes are not carried at all rather than carried and ignored. */
static bool cs_set_cookie(JSContext *ctx, const UrlRecord *url, const CsCookie *in)
{
    CsCookie c = *in;
    CookieJarAttributes attrs;
    char *enc_domain = NULL, *def_path = NULL;
    bool ok = false;

    memset(&attrs, 0, sizeof attrs);            /* §7.2 STEP 11: "Let attributes be « »." */
    cs_normalize(&c.name, &c.name_len);         /* STEP 1 */
    cs_normalize(&c.value, &c.value_len);       /* STEP 2 */
    if (cs_has_refused_byte(c.name, c.name_len) || cs_has_refused_byte(c.value, c.value_len))
        return false;                           /* STEP 3 */
    if (memchr(c.name, '=', c.name_len))
        return false;                           /* STEP 4 */
    /* STEP 5, whose three sub-steps are reached ONLY for an empty cookie-name — which §7.2 permits and which
       §5.2 step 5 would have ignored, so this is one of the two divergences cookie_jar.h names as the reason
       this path does not go through a set-cookie-string. With no name, the VALUE is what a serialized cookie
       would begin with, which is why these three ask of it what steps 4 and 6 ask of a name. */
    if (!c.name_len) {
        if (memchr(c.value, '=', c.value_len))
            return false;                       /* STEP 5.1 */
        if (!c.value_len)
            return false;                       /* STEP 5.2 */
        if (cs_starts_lowered(c.value, c.value_len, "__host-") ||
            cs_starts_lowered(c.value, c.value_len, "__host-http-") ||
            cs_starts_lowered(c.value, c.value_len, "__http-") ||
            cs_starts_lowered(c.value, c.value_len, "__secure-"))
            return false;                       /* STEP 5.3 */
    }
    if (cs_starts_lowered(c.name, c.name_len, "__host-http-") ||
        cs_starts_lowered(c.name, c.name_len, "__http-"))
        return false;                           /* STEP 6 */
    /* STEPS 7 AND 8 are the UTF-8 encoding, and these bytes already are it: a USVString reaches this algorithm
       through Web IDL §3.2.12's scalar value conversion and JS_ToCStringLen hands back its UTF-8. So steps 9,
       12.7 and 19 measure `strlen`-style byte lengths and not code-point counts.
       STEP 9's BOUND IS §2.1's, NAMED THERE RATHER THAN HERE: "The combined lengths of the name and value
       fields must not be greater than 4096 bytes (the maximum name/value pair size)." */
    if (c.name_len + c.value_len > 4096)
        return false;                           /* STEP 9 */
    /* STEP 10's `host` is `url`'s, read below through the one predicate step 12.3 names rather than compared
       here — a host is a parsed record and not a string, which is what makes that predicate's answer sound. */
    if (c.have_domain) {                        /* STEP 12 */
        UrlHost parsed;

        if (c.domain_len && c.domain[0] == '.')
            return false;                       /* STEP 12.1 */
        if (cs_starts_lowered(c.name, c.name_len, "__host-"))
            return false;                       /* STEP 12.2 */
        /* STEPS 12.3, 12.4 AND 12.5 ARE ONE CALL. §7.1.1.2's predicate PARSES hostSuffixString to answer at
           all — its own step 2 — and core/url/registrable_domain.h hands that parse back for exactly this
           reason, so asking the host parser again for step 12.4 would be a SECOND answer to one question,
           free to disagree with the first. Both of this call's operands are page-supplied and neither is
           asserted: an empty, unparseable or IP-address `domain` is the predicate's own `false`. */
        if (!registrable_domain_suffix_or_equal(c.domain, c.domain_len, &url->host, &parsed)) {
            url_host_free(&parsed);
            return false;                       /* STEP 12.3 */
        }
        /* STEP 12.5's "Assert: parsedDomain is not failure", and it is an assert THIS FILE MAY MAKE because
           the value is one this file computed: the predicate above returns false for a parse failure at its
           own step 3, so a true answer carrying no host is that component disagreeing with itself. */
        DCHECK(parsed.kind != URL_HOST_NULL,
               "§7.2 step 12.5: the registrable-domain predicate answered TRUE for a `Domain` whose host parse "
               "it reports as failure — HTML §7.1.1.2's step 3 returns false for a failed parse, so a true "
               "answer with no parsed host is core/url/registrable_domain.c contradicting its own step");
        enc_domain = url_serialize_host(&parsed);   /* STEP 12.6 */
        url_host_free(&parsed);
        CHECK(enc_domain != NULL, "OOM serializing §7.2 step 12.6's encodedDomain");
        /* STEP 12.7's bound is §2.1's maximum attribute value size: "The length of every field except the name
           and value fields must not be greater than 1024 bytes." */
        if (strlen(enc_domain) > 1024)
            goto done;                          /* STEP 12.7 */
        attrs.domain = enc_domain;              /* STEP 12.8 */
        attrs.domain_len = strlen(enc_domain);
    }
    if (c.have_expires) {                       /* STEP 13 */
        if (c.have_max_age)
            goto done;                          /* STEP 13.1 */
        attrs.have_expiry = true;               /* STEP 13.2 */
        attrs.expiry = cs_expiry_seconds(c.expires);
    } else if (c.have_max_age) {
        /* STEP 14. The attribute-value §7.2 appends is `ToString(maxAge)`, and RFC 6265 §5.2.2 is what reads
           one: "If delta-seconds is less than or equal to zero (0), let expiry-time be the earliest
           representable date and time. Otherwise, let the expiry-time be the current date and time plus
           delta-seconds seconds." The jar's attribute-list carries that EXPIRY-TIME rather than the delta, so
           this is §5.2.2's arithmetic and not a second reading of it — the same computation the string parse
           performs, at the one field both callers must fill.
           THE ADD SATURATES for the reason the date serialization does: `maxAge` is a `long long` the page
           chose, so `now + maxAge` is free to overflow, and signed overflow is UNDEFINED rather than large. */
        attrs.max_age_seen = true;
        attrs.have_expiry = true;
        if (c.max_age <= 0) {
            attrs.expiry = LLONG_MIN;
        } else {
            long long now = (long long)time(NULL);

            long long delta = (long long)c.max_age;

            attrs.expiry = delta > LLONG_MAX - now ? LLONG_MAX : now + delta;
        }
    }
    if (!c.path_len) {
        /* STEP 15. `serialized cookie default path` is FETCH §3.1.3 "Cookie infrastructure"'s, whose inner
           `cookie default path` is RFC 6265 §5.1.4's — one definition, reached through cookie_jar.h rather
           than restated, because a second copy of a path rule is a second answer about which requests a
           cookie is sent to. The substituted value is what steps 16, 17 and 19 then read, which is why it is
           taken as a VALUE here instead of being left to §5.3 step 7's own default. */
        def_path = cookie_jar_default_path(url);
        CHECK(def_path != NULL, "OOM computing §7.2 step 15's serialized cookie default path");
        c.path = def_path;
        c.path_len = strlen(def_path);
    }
    if (!c.path_len || c.path[0] != '/')
        goto done;                              /* STEP 16 */
    if (!(c.path_len == 1 && c.path[0] == '/') &&
        cs_starts_lowered(c.name, c.name_len, "__host-"))
        goto done;                              /* STEP 17 */
    if (c.path_len > 1024)
        goto done;                              /* STEPS 18 AND 19 */
    attrs.have_path = true;                     /* STEP 20 */
    attrs.path = c.path;
    attrs.path_len = c.path_len;
    /* STEP 21: "Append (`Secure`, ``) to attributes" — UNCONDITIONALLY, for every cookie this API writes. It
       is the whole of why a cookie set here is withheld from an http request-uri by §5.4's secure-only test,
       and it is not a property of the document: a secure context may still have an http address. */
    attrs.secure = true;
    /* STEPS 22 AND 23 append SameSite and Partitioned, which this store recognizes neither of — see this
       function's own named residual. `sameSite` is still READ, because a value outside CookieSameSite's three
       strings is Web IDL §3.2.18's TypeError at the declaration and never reaches here. */
    DCHECK(c.same_site != NULL &&
           (strcmp(c.same_site, "strict") == 0 || strcmp(c.same_site, "lax") == 0 ||
            strcmp(c.same_site, "none") == 0),
           "§7.2 step 22's switch was handed a `sameSite` that is none of CookieSameSite's three members — the "
           "enumeration is checked by the declared type before any step of this algorithm runs, so a fourth "
           "string here is this component composing one rather than a page supplying one");
    (void)c.partitioned;
    /* STEP 24: §5.3 "receive a cookie", over the four things this algorithm computed and no string. */
    cookie_jar_store(ctx, url, c.name, c.name_len, c.value, c.value_len, &attrs);
    ok = true;                                  /* STEP 25 */
done:
    free(enc_domain);
    free(def_path);
    return ok;
}

/* §7.3 "Delete a cookie" — FOUR STEPS, whose last one is "Return the results of running set a cookie with url,
 * name, value, null, domain, path, "strict", partitioned, and 0". So there is no second storage algorithm here:
 * a delete is a SET whose maxAge is 0, which §7.2 step 14 appends as `Max-Age: 0` and which RFC 6265 §5.2.2
 * turns into the earliest representable date — the expiry the jar's own store step then removes the entry for.
 *
 * STEP 3 IS WHY A NAMELESS COOKIE CAN BE DELETED AT ALL: "If name's length is 0, then set value to any
 * non-empty implementation-defined string." §7.2 step 5.2 refuses an empty value when the name is empty, so a
 * delete of the nameless cookie would refuse itself without this. The string is this engine's to pick and its
 * bytes are never stored — the entry is removed rather than written — so it needs only to satisfy steps 5.1 and
 * 5.3, which is to say it may not contain U+003D (=) and may not begin with one of the four prefixes. */
static bool cs_delete_cookie(JSContext *ctx, const UrlRecord *url, const char *name, size_t name_len,
                             bool have_domain, const char *domain, size_t domain_len,
                             const char *path, size_t path_len, bool partitioned)
{
    CsCookie c;

    memset(&c, 0, sizeof c);
    c.name = name;
    c.name_len = name_len;
    cs_normalize(&c.name, &c.name_len);         /* STEP 1 */
    c.value = "";                               /* STEP 2 */
    c.value_len = 0;
    if (!c.name_len) {                          /* STEP 3 */
        c.value = "deleted";
        c.value_len = 7;
    }
    /* STEP 4's nine arguments, in its own order: `expires` null, `sameSite` "strict" and `maxAge` 0 are the
       algorithm's own constants rather than anything a caller passed, and 0 is NON-NULL — which is the whole
       mechanism of the delete and is why `have_max_age` is true beside it. */
    c.have_expires = false;
    c.have_domain = have_domain;
    c.domain = domain;
    c.domain_len = domain_len;
    c.path = path;
    c.path_len = path_len;
    c.same_site = "strict";
    c.partitioned = partitioned;
    c.have_max_age = true;
    c.max_age = 0;
    return cs_set_cookie(ctx, url, &c);
}

/* §3.1 get and §3.2 getAll, whose only differences are step 5's empty-options rejection and what they settle
 * with. Both reach §7.1 "Query cookies", whose step 1 is RFC 6265 §5.4 with the cookie-string discarded — which
 * is why the jar hands back the intermediate cookie-list and this body never sees a cookie-string at all. */
static JSValue js_cs_query(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst arg = argc > 0 ? argv[0] : JS_UNDEFINED;
    bool by_string;
    JSValue opt_name = JS_UNDEFINED, opt_url = JS_UNDEFINED;
    JSValue list = JS_UNDEFINED, out = JS_UNDEFINED;
    UrlRecord uri;
    const char *want = NULL;
    size_t want_len = 0;
    uint32_t n = 0, i, kept = 0;

    if (!cs_brand(ctx, this_val))
        return JS_EXCEPTION;
    /* IDL_STRING_OR_DICT places a string for Web IDL §3.6 step 12.15's entry and an engine-built dictionary
       for step 12.11's, and places UNKNOWN EXTERNAL INPUT as itself on the forked string world — so a concolic
       here is the string arm, which is the same test core/frame/window_message.c makes and states. */
    by_string = JS_IsString(arg) || concolic_is(arg);
    DCHECK(by_string || JS_IsObject(arg) || JS_IsUndefined(arg),
           "CookieStore's get/getAll reached its body with an argument that is neither a string, an object nor "
           "absent — IDL_STRING_OR_DICT places one of those three, and an OMITTED argument is the `= {}` "
           "dictionary rather than an absent one");
    if (!by_string && !JS_IsUndefined(arg)) {
        opt_name = idl_dict_get(ctx, arg, "name");
        opt_url = idl_dict_get(ctx, arg, "url");
    }
    /* §3.1's get(options) STEP 5, which §3.2's getAll(options) does NOT have: "If options is empty, then return
       a promise rejected with a TypeError." So `cookieStore.get()` rejects and `cookieStore.getAll()` returns
       every cookie — two members that look alike and differ here, stated once. */
    if (magic == CS_GET && !by_string && JS_IsUndefined(opt_name) && JS_IsUndefined(opt_url)) {
        JS_FreeValue(ctx, opt_name);
        JS_FreeValue(ctx, opt_url);
        return JS_ThrowTypeError(ctx, "cookieStore.get() needs a name or a url — Cookie Store API §3.1 rejects "
                                      "an empty CookieStoreGetOptions rather than querying for everything");
    }
    /* §3.1 STEPS 1-4 / §3.2 STEPS 1-4: the relevant settings object's origin and creation URL. The opaque test
       is asked of the AGENT's principal, which is what an instance is keyed by — the same question and the same
       answer `document.cookie`'s own SecurityError is decided by. */
    if (origin_is_opaque(window_proxy_origin(document_window_proxy(ctx)))) {
        JS_FreeValue(ctx, opt_name);
        JS_FreeValue(ctx, opt_url);
        return JS_ThrowDOMException(ctx, "SecurityError",
                                    "a document with an opaque origin has no cookies");
    }
    if (!cs_request_uri(ctx, &uri)) {
        /* A cookie-averse document has no cookie store to query. §7.1 step 1 computes a cookie-string for a
           request-uri, and there is none — the list is empty, which is what a document with no cookies has. */
        url_record_free(&uri);
        JS_FreeValue(ctx, opt_name);
        JS_FreeValue(ctx, opt_url);
        list = JS_NewArray(ctx);
        CHECK(!JS_IsException(list), "OOM building an empty cookie-list");
        return magic == CS_GET ? (JS_FreeValue(ctx, list), JS_NULL) : list;
    }
    /* §3.1 STEP 6 / §3.2 STEP 5: `options["url"]`, which in a Window may only ever name THIS document. The
       standard allows a ServiceWorker to query another URL and this engine has none, so both of that step's
       rejections reduce to one comparison against the creation URL with fragments excluded. */
    if (!JS_IsUndefined(opt_url)) {
        const char *given = JS_ToCString(ctx, opt_url);
        char *mine = url_serialize(&uri, /*exclude_fragment*/ true);
        UrlRecord parsed;
        char *theirs = NULL;
        bool ok;

        const char *base_str = document_base_url(ctx);
        UrlRecord base;
        bool have_base;

        url_record_init(&parsed);
        url_record_init(&base);
        CHECK(mine != NULL, "OOM serializing this document's creation URL for §3.1 step 6.2");
        CHECK(given != NULL, "OOM reading the url member of a CookieStoreGetOptions");
        /* STEP 6.1: "parsing options["url"] with settings's API BASE URL" — which is the document's base URL
           and NOT its creation URL, so a `<base href>` moves what a relative `url` option resolves against
           exactly as it moves every other API-base-relative parse this platform makes. */
        have_base = base_str && *base_str && url_parse(&base, base_str, strlen(base_str), NULL);
        ok = url_parse(&parsed, given, strlen(given), have_base ? &base : NULL);
        if (ok)
            theirs = url_serialize(&parsed, /*exclude_fragment*/ true);
        url_record_free(&base);
        /* STEPS 6.2 AND 6.3 collapse here: equality with fragments excluded implies same origin, so the
           stronger test is the one asked and the weaker one cannot then fail. */
        ok = ok && theirs && strcmp(mine, theirs) == 0;
        free(theirs);
        free(mine);
        JS_FreeCString(ctx, given);
        url_record_free(&parsed);
        if (!ok) {
            url_record_free(&uri);
            JS_FreeValue(ctx, opt_name);
            JS_FreeValue(ctx, opt_url);
            return JS_ThrowTypeError(ctx, "cookieStore's url option must name this document's own URL — Cookie "
                                          "Store API §3.1 step 6.2 rejects any other, and a Window has no "
                                          "second URL it may query");
        }
    }
    JS_FreeValue(ctx, opt_url);

    /* §7.1 STEP 1: RFC 6265 §5.4 with the cookie-string discarded and its intermediate cookie-list kept. */
    list = cookie_jar_cookie_list(ctx, &uri);
    url_record_free(&uri);
    CHECK(!JS_IsException(list), "§7.1 step 1's cookie-list could not be computed");
    /* §7.1 step 3.2's name filter — `name` is the string argument, or the dictionary's member, or absent. */
    if (by_string) {
        want = JS_ToCStringLen(ctx, &want_len, arg);
        CHECK(want != NULL, "OOM reading the name argument of a CookieStore query");
    } else if (!JS_IsUndefined(opt_name)) {
        want = JS_ToCStringLen(ctx, &want_len, opt_name);
        CHECK(want != NULL, "OOM reading the name member of a CookieStoreGetOptions");
    }
    CHECK(JS_ToUint32(ctx, &n, JS_GetPropertyStr(ctx, list, "length")) == 0,
          "the cookie-list did not carry its own length");

    out = JS_NewArray(ctx);
    CHECK(!JS_IsException(out), "OOM building §7.1 step 2's list");
    /* §7.1 STEP 3, over the cookie-list in §5.4 step 2's order. */
    for (i = 0; i < n; i++) {
        JSValue pair = JS_GetPropertyUint32(ctx, list, i);
        JSValue item;

        CHECK(!JS_IsException(pair), "a cookie-list entry could not be read back");
        if (want) {
            /* STEPS 3.2.2 AND 3.2.3: the stored name, UTF-8 decoded, compared with the requested one. */
            JSValue nm = JS_GetPropertyUint32(ctx, pair, 0);
            size_t nlen = 0;
            const char *have = JS_ToCStringLen(ctx, &nlen, nm);
            bool same;

            CHECK(have != NULL, "a cookie-list entry's name could not be read as a string");
            same = nlen == want_len && memcmp(have, want, want_len) == 0;
            JS_FreeCString(ctx, have);
            JS_FreeValue(ctx, nm);
            if (!same) { JS_FreeValue(ctx, pair); continue; }
        }
        item = cs_item(ctx, pair);           /* STEP 3.3 */
        JS_FreeValue(ctx, pair);
        CHECK(JS_DefinePropertyValueUint32(ctx, out, kept, item, JS_PROP_C_W_E) >= 0,
              "§7.1 step 3.4 could not append a CookieListItem");
        kept++;
        /* §3.1 settles with the FIRST item, so `get` stops at one — which is not an optimisation but the
           difference between minting one attacker source and minting one per cookie in the jar. */
        if (magic == CS_GET)
            break;
    }
    if (want)
        JS_FreeCString(ctx, want);
    JS_FreeValue(ctx, opt_name);
    JS_FreeValue(ctx, list);

    if (magic == CS_GET_ALL)
        return out;                          /* §3.2 step 6.3: resolve with the list */
    /* §3.1 steps 6.3 and 6.4: null for an empty list, the first item otherwise. */
    if (!kept) {
        JS_FreeValue(ctx, out);
        return JS_NULL;
    }
    {
        JSValue first = JS_GetPropertyUint32(ctx, out, 0);
        JS_FreeValue(ctx, out);
        return first;
    }
}

/* §3.3 "The set() method", both entries. Their step lists are identical but for which nine values reach §7.2.
 *
 * WHICH ENTRY RAN IS READ OFF `argc` AND NOT OFF THE VALUE, which is the whole content of this member's declared
 * type. §3.6 steps 3-4 remove one entry at EVERY arity this member can be called at — `set(CookieInit options)`
 * has a tuple at arity 1 only and `set(USVString name, USVString value)` at arity 2 only — so there is no step
 * 12 here and no value is ever consulted to choose. `argc` is min(maxarg, args), so a three-argument call is the
 * two-string entry with its third argument ignored, exactly as §3.6 step 3 says. */
static JSValue js_cs_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue m[8];
    UrlRecord uri;
    CsCookie c;
    const char *name = NULL, *value = NULL, *domain = NULL, *path = NULL;
    bool two_strings = argc >= 2;
    bool ok;
    int i;

    (void)magic;
    for (i = 0; i < 8; i++)
        m[i] = JS_UNDEFINED;
    if (!cs_brand(ctx, this_val))
        return JS_EXCEPTION;
    memset(&c, 0, sizeof c);
    /* §3.3 STEPS 2 AND 3, asked of the AGENT's principal exactly as §3.1, §3.2 and §3.4 ask it. */
    if (origin_is_opaque(window_proxy_origin(document_window_proxy(ctx))))
        return JS_ThrowDOMException(ctx, "SecurityError",
                                    "a document with an opaque origin has no cookies");
    /* §3.3 STEP 4's `settings's creation URL`, which is §7.2's `url`. */
    if (!cs_request_uri(ctx, &uri)) {
        /* A cookie-averse document has no cookie store to write, which is js_cs_delete's arm and
           js_cs_query's for the same reason and out of the same predicate: there is nothing to write rather
           than a refusal to write, so §3.3 step 6.3 resolves with undefined having changed nothing. */
        url_record_free(&uri);
        return JS_UNDEFINED;
    }
    if (two_strings) {
        /* §3.3's `set(name, value)` STEP 6.1: "set a cookie with url, name, value, null, null, "/", "strict",
           false, and null." Every one of those six constants is the ALGORITHM'S and not a default anybody
           declared — this entry has no dictionary at all — which is why they are written here rather than read. */
        DCHECK(JS_IsString(argv[0]) || concolic_is(argv[0]),
               "§3.3's two-argument entry reached its body with a non-string at position 0 — §3.6 steps 3-4 "
               "removed the dictionary entry at this arity and the surviving entry declares USVString, so the "
               "declaration has already converted it or crossed unknown external input as itself");
        name = JS_ToCStringLen(ctx, &c.name_len, argv[0]);
        value = JS_ToCStringLen(ctx, &c.value_len, argv[1]);
        CHECK(name != NULL && value != NULL, "OOM reading the name and value arguments of a CookieStore set");
        c.name = name;
        c.value = value;
        c.have_expires = false;
        c.have_domain = false;
        c.path = "/";
        c.path_len = 1;
        c.same_site = "strict";
        c.partitioned = false;
        c.have_max_age = false;
    } else {
        /* §3.3's `set(options)` STEP 6.1, over a CookieInit whose every member the declaration has placed:
           `name` and `value` are REQUIRED so §3.2.17 threw without them, and the other six carry the IDL's own
           defaults, so each is READ rather than invented here. */
        JSValueConst arg = argv[0];

        DCHECK(JS_IsObject(arg),
               "§3.3's one-argument entry reached its body with something that is not an object — §3.6 steps "
               "3-4 removed the two-string entry at this arity and §3.2.17 Dictionary types step 1 makes a "
               "primitive that is not undefined or null a TypeError, so the value here is the dictionary this "
               "declaration built");
        m[0] = idl_dict_get(ctx, arg, "domain");
        m[1] = idl_dict_get(ctx, arg, "expires");
        m[2] = idl_dict_get(ctx, arg, "maxAge");
        m[3] = idl_dict_get(ctx, arg, "name");
        m[4] = idl_dict_get(ctx, arg, "partitioned");
        m[5] = idl_dict_get(ctx, arg, "path");
        m[6] = idl_dict_get(ctx, arg, "sameSite");
        m[7] = idl_dict_get(ctx, arg, "value");
        DCHECK(!JS_IsUndefined(m[3]) && !JS_IsUndefined(m[7]),
               "a CookieInit reached this body with `name` or `value` absent — the IDL declares both "
               "`required`, so Web IDL §3.2.17 step 4.1.3 throws a TypeError at the declaration and this body "
               "is not reached at all");
        DCHECK(!JS_IsUndefined(m[4]) && !JS_IsUndefined(m[5]) && !JS_IsUndefined(m[6]),
               "a CookieInit reached this body with `partitioned`, `path` or `sameSite` absent — each carries a "
               "declared default (`false`, `\"/\"` and `\"strict\"`), which §3.2.17 step 4.1.5 PLACES, so an "
               "absence here is a declaration that did not state one rather than a page that omitted a member");
        name = JS_ToCStringLen(ctx, &c.name_len, m[3]);
        value = JS_ToCStringLen(ctx, &c.value_len, m[7]);
        CHECK(name != NULL && value != NULL, "OOM reading the name and value members of a CookieInit");
        c.name = name;
        c.value = value;
        /* `DOMHighResTimeStamp? expires` and `long long? maxAge`: NULL IS THE IDL NULL AND IS NOT ZERO, which
           is what §7.2 steps 13 and 14 branch on — see the two nullable numeric rows in core/idl_args.h. The
           declared type has already refused a non-finite `expires` (§3.2.7) and wrapped `maxAge` modulo 2^64
           (§3.2.4.7), so neither is asked anything here beyond whether it is null. */
        c.have_expires = !JS_IsNull(m[1]);
        if (c.have_expires)
            CHECK(JS_ToFloat64(ctx, &c.expires, m[1]) == 0,
                  "a converted DOMHighResTimeStamp is not a number — §3.2.7's conversion places one or throws");
        c.have_max_age = !JS_IsNull(m[2]);
        if (c.have_max_age)
            CHECK(JS_ToInt64(ctx, &c.max_age, m[2]) == 0,
                  "a converted `long long` is not an integer — §3.2.4.7's conversion places one");
        c.have_domain = !JS_IsNull(m[0]);
        if (c.have_domain) {
            domain = JS_ToCStringLen(ctx, &c.domain_len, m[0]);
            CHECK(domain != NULL, "OOM reading the domain member of a CookieInit");
            c.domain = domain;
        }
        path = JS_ToCStringLen(ctx, &c.path_len, m[5]);
        CHECK(path != NULL, "OOM reading the path member of a CookieInit");
        c.path = path;
        c.partitioned = JS_ToBool(ctx, m[4]) != 0;
        /* §3.2.18's enumeration has already refused anything outside CookieSameSite's three strings, so this
           is one of them and cs_set_cookie's own assert over §7.2 step 22's switch says so. */
        c.same_site = JS_ToCString(ctx, m[6]);
        CHECK(c.same_site != NULL, "OOM reading the sameSite member of a CookieInit");
    }
    ok = cs_set_cookie(ctx, &uri, &c);
    url_record_free(&uri);
    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, value);
    if (!two_strings) {
        if (c.have_domain)
            JS_FreeCString(ctx, domain);
        JS_FreeCString(ctx, path);
        JS_FreeCString(ctx, c.same_site);
    }
    for (i = 0; i < 8; i++)
        JS_FreeValue(ctx, m[i]);
    /* §3.3 STEPS 6.2 AND 6.3: a failure is a TypeError on the promise and success resolves with undefined.
       This member declares idl_returns_promise, so the throw becomes a REJECTED promise. */
    if (!ok)
        return JS_ThrowTypeError(ctx, "cookieStore.set could not write that cookie — Cookie Store API §7.2 "
                                      "\"Set a cookie\" refused the name, value, domain, path or expiry it was "
                                      "given");
    return JS_UNDEFINED;
}

/* §3.4 "The delete() method", both entries. Their step lists are IDENTICAL but for which five values reach
 * §7.3, so this is one body for the reason js_cs_query is one for §3.1 and §3.2.
 *
 * ITS TWO ENTRIES DISAGREE ABOUT `partitioned` AND THE STANDARD MEANS THEM TO. `delete(name)` step 6.1 passes
 * `true` and `delete(options)` step 6.1 passes `options["partitioned"]`, whose IDL default is `false` — so
 * `cookieStore.delete("x")` and `cookieStore.delete({name: "x"})` are not two spellings of one call. Both are
 * passed through as written; see cs_set_cookie's named residual for why this store cannot yet tell them apart.
 *
 * STEPS 1 TO 4 ARE §3.1's AND §3.2's, ASKED THE SAME WAY. The opaque test is of the AGENT's principal, which is
 * what an instance is keyed by, and the creation URL is this realm's document address. */
static JSValue js_cs_delete(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst arg = argc > 0 ? argv[0] : JS_UNDEFINED;
    bool by_string;
    JSValue m_name = JS_UNDEFINED, m_domain = JS_UNDEFINED, m_path = JS_UNDEFINED, m_part = JS_UNDEFINED;
    UrlRecord uri;
    const char *name = NULL, *domain = NULL, *path = NULL;
    size_t name_len = 0, domain_len = 0, path_len = 0;
    bool have_domain = false, partitioned, ok;

    (void)magic;
    if (!cs_brand(ctx, this_val))
        return JS_EXCEPTION;
    /* IDL_STRING_OR_DICT places a string for Web IDL §3.6 step 12.15's entry and an engine-built dictionary for
       step 12.11's — the same test js_cs_query makes and states. There is no `undefined` arm here and that is
       the IDL rather than an omission: NEITHER of §3.4's entries declares its one argument optional, so §3.6
       step 4 empties S at an argument count of 0 and step 5 throws a TypeError before this body — which the
       declaration states by naming no optional position at all. */
    by_string = JS_IsString(arg) || concolic_is(arg);
    DCHECK(by_string || JS_IsObject(arg),
           "CookieStore's delete reached its body with an argument that is neither a string nor an object — "
           "IDL_STRING_OR_DICT places one of those two, §3.2.17 refuses a primitive that is not a string, and "
           "an ABSENT argument is §3.6 step 5's TypeError because neither entry declares this position optional");
    /* §3.4 STEPS 2 AND 3 — the same principal question `document.cookie`'s own SecurityError is decided by. */
    if (origin_is_opaque(window_proxy_origin(document_window_proxy(ctx))))
        return JS_ThrowDOMException(ctx, "SecurityError",
                                    "a document with an opaque origin has no cookies");
    /* §3.4 STEP 4's `settings's creation URL`, which is also §7.2's `url` and §7.3's. */
    if (!cs_request_uri(ctx, &uri)) {
        /* A cookie-averse document has no cookie store to write, exactly as it has none to query — see
           js_cs_query's own arm, which answers the empty list for the same reason and out of the same
           predicate. §7.3 reaches no failure here: there is nothing to delete rather than a refusal to
           delete, so the promise RESOLVES having changed nothing, which is also what HTML's `document.cookie`
           setter does for such a document. */
        url_record_free(&uri);
        return JS_UNDEFINED;
    }
    if (by_string) {
        /* §3.4's `delete(name)` STEP 6.1: "delete a cookie with url, name, null, "/", and true." */
        name = JS_ToCStringLen(ctx, &name_len, arg);
        CHECK(name != NULL, "OOM reading the name argument of a CookieStore delete");
        have_domain = false;
        path = "/";
        path_len = 1;
        partitioned = true;
    } else {
        /* §3.4's `delete(options)` STEP 6.1, over a CookieStoreDeleteOptions every member of which the
           declaration has already placed: `name` is REQUIRED so §3.2.17 threw without it, and the other three
           carry the IDL's own defaults, so each is READ rather than defaulted here. */
        m_name = idl_dict_get(ctx, arg, "name");
        m_domain = idl_dict_get(ctx, arg, "domain");
        m_path = idl_dict_get(ctx, arg, "path");
        m_part = idl_dict_get(ctx, arg, "partitioned");
        DCHECK(!JS_IsUndefined(m_name),
               "a CookieStoreDeleteOptions reached this body with no `name` — the IDL declares it `required`, "
               "so Web IDL §3.2.17 step 4.1.3 throws a TypeError at the declaration and this body is not "
               "reached at all");
        DCHECK(!JS_IsUndefined(m_path) && !JS_IsUndefined(m_part),
               "a CookieStoreDeleteOptions reached this body with `path` or `partitioned` absent — both carry a "
               "declared default (`\"/\"` and `false`), which §3.2.17 step 4.1.5 PLACES, so an absence here is "
               "a declaration that did not state one rather than a page that omitted a member");
        name = JS_ToCStringLen(ctx, &name_len, m_name);
        CHECK(name != NULL, "OOM reading the name member of a CookieStoreDeleteOptions");
        /* `USVString? domain = null`: null is the IDL null and is §7.2 step 12's "domain is non-null" being
           false. Anything else is a string whose BYTES are the page's and are never asserted about. */
        have_domain = !JS_IsNull(m_domain);
        if (have_domain) {
            domain = JS_ToCStringLen(ctx, &domain_len, m_domain);
            CHECK(domain != NULL, "OOM reading the domain member of a CookieStoreDeleteOptions");
        }
        path = JS_ToCStringLen(ctx, &path_len, m_path);
        CHECK(path != NULL, "OOM reading the path member of a CookieStoreDeleteOptions");
        partitioned = JS_ToBool(ctx, m_part) != 0;
    }
    ok = cs_delete_cookie(ctx, &uri, name, name_len, have_domain, domain, domain_len,
                          path, path_len, partitioned);
    url_record_free(&uri);
    /* `name` IS OWNED ON BOTH ARMS and `path` ON ONLY ONE — the string arm's path is §3.4's own `"/"` literal,
       which is this file's static and not a JS string. The four member values are released unconditionally
       because JS_UNDEFINED is what the string arm left them at and freeing one is a no-op, which is one
       cleanup rather than a rule about which arm owns what. */
    JS_FreeCString(ctx, name);
    if (!by_string) {
        if (have_domain)
            JS_FreeCString(ctx, domain);
        JS_FreeCString(ctx, path);
    }
    JS_FreeValue(ctx, m_name);
    JS_FreeValue(ctx, m_domain);
    JS_FreeValue(ctx, m_path);
    JS_FreeValue(ctx, m_part);
    /* §3.4 STEPS 6.2 AND 6.3: a failure is a TypeError on the promise, and success resolves with undefined.
       This member declares idl_returns_promise, so the throw becomes a REJECTED promise rather than a
       synchronous exception — which is what §3.4 step 6.2's "reject p with a TypeError" requires. */
    if (!ok)
        return JS_ThrowTypeError(ctx, "cookieStore.delete could not delete that cookie — Cookie Store API §7.2 "
                                      "\"Set a cookie\", which §7.3 step 4 runs, refused the name, domain or "
                                      "path it was given");
    return JS_UNDEFINED;
}

/* §6.1 The Window interface's `[SameObject] readonly attribute CookieStore cookieStore` — THIS realm's. */
static JSValue cs_get_cookie_store(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)this_val;
    (void)magic;
    return realm_value_get(ctx, g_obj_slot);
}

static void cs_install_realm(JSContext *ctx)
{
    /* §3's `interface CookieStore : EventTarget`, so the prototype is CREATED over this realm's
       EventTarget.prototype rather than re-parented after the fact — see core/events/event_target.h. */
    JSValue proto = event_target_derived_proto(ctx);
    JSValue global, obj;

    CHECK(!JS_IsException(proto), "this realm's CookieStore.prototype could not be allocated");
    /* §3.7.3's @@toStringTag, which also ASSERTS §3.7.3's proto step against browser/idl_inheritance.h — that
       table already carries { "CookieStore", "EventTarget", IDL_PROTO_INHERITS }, so this is what checks that
       the prototype above really was built over this realm's EventTarget.prototype rather than merely named as
       though it had been. It is also what engine/idl_installed.mjs reads to decide which INTERFACE this file's
       installs belong to: without it the audit sees two installed members it cannot attribute and reports them
       as undecided rather than crediting them to CookieStore. */
    idl_interface_tag(ctx, proto, "CookieStore");
    /* §3's interface and its Window member are both `[SecureContext]`, so Web IDL §3.3.13 REMOVES them in a
       non-secure realm rather than making them throw: `window.cookieStore ? … : document.cookie` takes the
       fallback there, which is the branch a bundle writes it to take, and `"cookieStore" in window` is false.
       §3.3 set, §3.4 delete, §3.1 get and §3.2 getAll are all here; what is not is the CHANGE-EVENT half, and
       that is this component's stated narrowing rather than an exposure decision — see the file header.

       NAMED RESIDUAL. NOT COVERED: §5.1 "The CookieChangeEvent interface", §7.4 "Process changes" and the
       `onchange` handler §3's IDL writes as `[Exposed=Window] attribute EventHandler onchange` — so a page may
       now read, create and delete a cookie through this API and cannot be TOLD that one changed. NEXT DIFF:
       §5.1's interface (a constructor, `changed` and `deleted` as FrozenArrays of CookieListItem, and a
       CookieChangeEventInit) and §7.4's "fire a change event" over its "prepare lists from changes", whose step
       3.3.1 sets a deleted item's `value` to undefined — then the accessor. `idl_interface_tag` names no
       CookieChangeEvent today, so the interface is the first half and the handler is the last.
       INSTALLING THE ACCESSOR ALONE IS THE SHAPE §NO STUBS FORBIDS TWICE OVER: it flips `"onchange" in
       cookieStore` true and abandons nothing, because `CookieStore : EventTarget` already answers
       `addEventListener("change", f)` and no change is ever dispatched to either — so a bundle that tests for
       the handler would take a branch this engine cannot complete while the listener path it would otherwise
       have used is equally silent. HOW ITS ABSENCE WOULD SHOW: a reader would observe `cookieStore.onchange`
       undefined and `cookieStore.set(...)` resolving with no "change" event dispatched to a listener registered
       through EventTarget, and `node engine/idlgen.mjs` reporting `onchange` as the member CookieStore does not
       install.
       WHAT CHANGED ABOUT THIS RESIDUAL'S BLOCKER, recorded because the retired reason is the one a reader
       re-derives: it used to be that there was nothing to notify about until this API could write. §7.4's
       observable changes now have a producer here — cs_set_cookie reaches the store on both the create and the
       delete path — so the blocker is no longer the absence of a writer, it is the absence of the EVENT.

       TWO RESIDUALS THAT STOOD HERE ARE DISCHARGED BY THE DIFFS THAT LANDED set AND delete, AND THEIR WRONG
       CLAUSES ARE KEPT BECAUSE BOTH WERE WRONG IN THE SAME PLACE AND A READER WILL RE-DERIVE EITHER. The first
       named `cookie_jar_receive` as §7.2's seam: that entry takes a set-cookie-string and runs §5.2's parse over
       it, and §7.2 refuses U+003B (;) in the NAME and the VALUE and in NOTHING ELSE — its own Note leaves open
       whether the restriction "should also apply to expires, domain, path, and sameSite as well" — so
       `path: "/;Domain=example"` spelled as a set-cookie-string is split at that semicolon into a Domain
       attribute the page never wrote and step 12.3 never judged. The round trip does not lose an attribute, it
       MANUFACTURES one past the check that clause existed to reach, and drops one too, since §7.2 permits an
       empty cookie-name and §5.2 step 5 ignores a set-cookie-string carrying one. §7.2's own step 24 names the
       right seam, which is `cookie_jar_store`. The second named IDL_USVSTRING_OR_DICT for §3.3's position 0:
       that row is Window's `postMessage`, whose SHORTER entry declares its dictionary position OPTIONAL, so its
       two entries MEET at one arity and §3.6 step 12 reads the page's VALUE there. Both of §3.3's entries
       declare every argument REQUIRED, so step 4 removes one at EVERY arity and no value is ever looked at;
       declaring that row would have read `cookieStore.set("x")` as a cookie name where §3.2.17 step 1 makes a
       String a TypeError. What it needed was IDL_USVSTRING_OR_DICT_BY_ARITY, which is the row this diff added.
       THREE TIMES NOW THE SPEC HALF OF A CLAUSE HERE WAS EXACT AND ITS MECHANISM HALF YIELDED — the section, the
       step, the predicate and the arities all checked out every time, and the part that was a claim about THIS
       TREE did not. That is the split CLAUDE.md measures, and the rate at this one site is what makes it worth
       writing down rather than the individual errors. */
    idl_install_method_exposed(ctx, proto, "get", g_id_get, IDL_SECURE_CONTEXT);
    idl_install_method_exposed(ctx, proto, "getAll", g_id_get_all, IDL_SECURE_CONTEXT);
    idl_install_method_exposed(ctx, proto, "set", g_id_set, IDL_SECURE_CONTEXT);
    idl_install_method_exposed(ctx, proto, "delete", g_id_delete, IDL_SECURE_CONTEXT);
    JS_SetClassProto(ctx, g_cs_class, JS_DupValue(ctx, proto));

    global = JS_GetGlobalObject(ctx);
    idl_install_interface_object_exposed(ctx, global, "CookieStore", proto, IDL_SECURE_CONTEXT);

    /* BUILT WITH THE REALM AND NOT ON FIRST READ, which is core/realm.h's rule and matters here for the reason
       it matters everywhere: an object minted inside whichever FLOW happened to read first would be that flow's
       private creation, so a sibling arm would get a different `cookieStore` and `===` would answer false where
       `[SameObject]` requires true. */
    obj = JS_NewObjectProtoClass(ctx, proto, g_cs_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "this realm's CookieStore could not be allocated");
    realm_value_set(ctx, g_obj_slot, obj);

    idl_install_accessor_exposed(ctx, global, "cookieStore", cs_get_cookie_store, 0, -1, IDL_SECURE_CONTEXT);
    JS_FreeValue(ctx, global);
}

void cookie_store_init(JSContext *ctx)
{
    /* §3.1's `get(USVString name)` and `get(optional CookieStoreGetOptions options = {})` — Web IDL §3.6's
       effective overload set has an entry of length 0 and TWO OF LENGTH 1, so at the one arity step 4 removes
       NEITHER and the whole decision is step 12's clause chain at the distinguishing argument.

       WHICH IS WHY THIS IS NOT IDL_USVSTRING_OR_DICT, AND THAT ROW'S OWN NAME IS THE TRAP. This member's two
       entries END AT THE SAME POSITION; IDL_USVSTRING_OR_DICT is idl_args.c's LENGTH-DIFFERING split, "a
       declared type whose two overload entries END AT DIFFERENT POSITIONS", which is HTML §7.2.2's
       `window.postMessage` (2 against 3) and not this. Declaring it here set `split_at` on a member with no
       longer entry to describe, and idl_args.c's seal aborted the build by name.
       THE REMEDY THAT ABORT NAMES IS WRONG FOR THIS MEMBER. It says to state where the longer entry's optional
       arguments begin with idl_overload_split_optional_from — correct for a member that HAS a longer entry, and
       here it would silence the assert while leaving a split that rewrites this position to a plain USVString at
       an arity that cannot occur, throwing the dictionary arm away. The spec half of that crash is right and its
       remedy clause is a hypothesis; this is the case where the hypothesis does not hold.
       IDL_SEQUENCE_OBJECT_OR_DICT is the same shape one arm over — MessagePort's two entries are both two long,
       "the arity shortcut IDL_USVSTRING_OR_DICT leans on has nothing to shortcut" — so the same-length rows are
       what a step-12-decided overload takes, and IDL_STRING_OR_DICT is the one of those with a string arm.

       NAMED RESIDUAL. NOT COVERED: IDL_STRING_OR_DICT's string arm is a DOMString (§3.2.10) and this IDL writes
       USVString (§3.2.12), so a lone surrogate in a queried name is not replaced with U+FFFD before the
       comparison in js_cs_query. Every OTHER value resolves identically, because §3.6 step 12's clause chain
       does not consult the arm's string kind. NEXT DIFF: a same-length row with a USVString arm in
       core/idl_args.{c,h} — listed by idl_type_is_dictionary, answering idl_concolic_rule IDL_CONCOLIC_FORKS,
       and NOT listed by idl_type_is_length_split, which is the one predicate that separates these two families;
       that file has a live lane in it, so it is sequenced rather than raced. HOW ITS ABSENCE SHOWS:
       `cookieStore.get('\uD800')` compares un-replaced surrogate bytes against the jar where a browser
       compares U+FFFD, so a cookie actually named U+FFFD is not found.
       THIS CLAUSE NUMBERED DOMString §3.2.11 UNTIL THE DIFF THAT LANDED §3.4, AND §3.2.11 IS "ByteString" —
       recorded rather than quietly repaired because the wrong number is one a reader COPIES: it travelled into
       the delete declaration below in the same diff that corrected it, and both sites named a real section of
       the right standard about the wrong type, which is the one axis a resolver and a title check cannot see.
       What found it was this tree DISAGREEING WITH ITSELF — engine/host/SPEC_STEPS.md writes "§3.2.10 DOMString,
       §3.2.11 ByteString, §3.2.12 USVString" and core/fetch/headers.c cites §3.2.11 for ByteString's range
       refusal at eight sites — so the sibling diff settled it without a fetch. DOMString is §3.2.10. */
    static const IdlArgType QUERY_ARGS[1] = { IDL_STRING_OR_DICT };
    static const IdlArgType SET_ARGS[2] = { IDL_USVSTRING_OR_DICT_BY_ARITY, IDL_USVSTRING };
    const int NOPT = (int)(sizeof(COOKIE_STORE_GET_OPTIONS) / sizeof(COOKIE_STORE_GET_OPTIONS[0]));
    const int NDEL = (int)(sizeof(COOKIE_STORE_DELETE_OPTIONS) / sizeof(COOKIE_STORE_DELETE_OPTIONS[0]));
    const int NSET = (int)(sizeof(COOKIE_INIT) / sizeof(COOKIE_INIT[0]));
    JSClassDef d = { "CookieStore" };

    DCHECK(g_obj_slot == JS_INVALID_CLASS_ID, "cookie_store_init ran twice — the class and the slot are declared once per AGENT");
    JS_NewClassID(JS_GetRuntime(ctx), &g_cs_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_cs_class, &d) == 0,
          "CookieStore: the per-realm prototype slot could not be declared");
    g_obj_slot = realm_value_declare(ctx, "Cookie Store API §6.1 this realm's CookieStore object");

    g_id_get = idl_method_id_dict(ctx, QUERY_ARGS, 1, COOKIE_STORE_GET_OPTIONS, NOPT, js_cs_query, CS_GET);
    idl_optional_from(0);
    idl_returns_promise();
    g_id_get_all = idl_method_id_dict(ctx, QUERY_ARGS, 1, COOKIE_STORE_GET_OPTIONS, NOPT, js_cs_query,
                                      CS_GET_ALL);
    idl_optional_from(0);
    idl_returns_promise();

    /* §3.4's `delete(USVString name)` and `delete(CookieStoreDeleteOptions options)` — TWO ENTRIES OF LENGTH
       ONE WITH NEITHER ARGUMENT OPTIONAL, so Web IDL §2.5.8 Overloading gives the effective overload set one
       tuple per entry AT ARITY ONE AND NONE AT ARITY ZERO. §3.6 steps 3-4 therefore remove neither at the one
       arity the member can be called at, and the whole decision is step 12's clause chain at the distinguishing
       argument — which is IDL_STRING_OR_DICT's family and not a length-differing split. `delete()` is step 5's
       TypeError, which is why NO idl_optional_from is stated: the position is required, so the pool's own
       required-count check is what refuses the zero-argument call.
       NAMED RESIDUAL. NOT COVERED: IDL_STRING_OR_DICT's string arm is a DOMString (§3.2.10) and this IDL writes
       USVString (§3.2.12), so a lone surrogate in a deleted name is not replaced with U+FFFD before the key is
       built. This is the SAME residual the query declaration above states, at its second site, and its next
       diff is the same one — a same-length row with a USVString arm in core/idl_args.{c,h}, listed by
       idl_type_is_dictionary, answering idl_concolic_rule IDL_CONCOLIC_FORKS, and NOT listed by
       idl_type_is_length_split. HOW ITS ABSENCE WOULD SHOW: a reader would observe `cookieStore.delete()` and
       `cookieStore.set()` agreeing about every argument but a lone surrogate, where a browser replaces it — so
       a cookie whose name really is U+FFFD survives a delete aimed at it. */
    g_id_delete = idl_method_id_dict(ctx, QUERY_ARGS, 1, COOKIE_STORE_DELETE_OPTIONS, NDEL, js_cs_delete, 0);
    idl_returns_promise();

    /* §3.3's `set(USVString name, USVString value)` and `set(CookieInit options)` — a §3.6 LENGTH-DIFFERING
       SPLIT AT POSITION 0 WHOSE TWO ENTRIES NEVER COEXIST AT ONE ARITY. Every argument of both entries is
       REQUIRED, so Web IDL §2.5.8 Overloading gives the effective overload set one tuple per entry, at arity 2
       and at arity 1, and §3.6 step 4 removes one at every arity: the position resolves from the ARGUMENT COUNT
       and no value is ever looked at. That is IDL_USVSTRING_OR_DICT_BY_ARITY and NOT IDL_USVSTRING_OR_DICT,
       whose entries DO meet at one arity because its shorter one declares the dictionary position optional —
       declaring that row here would read `cookieStore.set("x")` as a name where §3.2.17 step 1 makes a String a
       TypeError. The row in core/idl_args.h carries that argument in full.
       THE TWO OPTIONAL INDICES ARE THE TWO ENTRIES' OWN AND MUST BOTH BE STATED. `idl_optional_from(1)` is the
       SHORTER entry's "there are none" — one past its only position, which is `split_at + 1` and what
       idl_args_seal asserts — and `idl_overload_split_optional_from(2)` is the LONGER entry's, whose two
       positions are both required. Without the second, §3.6 step 15.3's optionality would be measured against
       the shorter entry at arity 2 and `cookieStore.set("x", undefined)` would read position 1 as an ABSENT
       optional where the surviving entry requires the string "undefined". §3.7.7 Operations' length is then
       min(1, 2) = 1, which is the shortest tuple in the set. */
    g_id_set = idl_method_id_dict(ctx, SET_ARGS, 2, COOKIE_INIT, NSET, js_cs_set, 0);
    idl_optional_from(1);
    idl_overload_split_optional_from(2);
    idl_returns_promise();

    /* THE ATTACKER SOURCE, with the constraint that makes a PoC through it reproduce. The excluded set is RFC
       6265 §4.1.1's cookie-value production — a value cannot carry whitespace, a double quote, a comma, a
       semicolon or a backslash — and the delivery is a PLANT because a cookie is not carried by the victim's
       load: it has to be in the jar BEFORE it.
       IT IS THE SAME RFC FACT core/dom/document_metadata.c STATES FOR `document.cookie`, and the two are two
       statements of one thing; see the residual below. */
    concolic_declare_source(CS_COMPONENT, CS_SOURCE, " \",;\\", 0, SRC_DELIVER_PLANT);

    agent_state_realm_slot(CS_COMPONENT, &g_obj_slot, "§6.1's per-realm CookieStore slot, and the declaration latch");
    agent_state_id(CS_COMPONENT, &g_id_get, "Cookie Store API §3.1's get");
    agent_state_id(CS_COMPONENT, &g_id_get_all, "Cookie Store API §3.2's getAll");
    agent_state_id(CS_COMPONENT, &g_id_set, "Cookie Store API §3.3's set");
    agent_state_id(CS_COMPONENT, &g_id_delete, "Cookie Store API §3.4's delete");
    agent_state_class(CS_COMPONENT, &g_cs_class, "Cookie Store API §3 CookieStore's per-realm slot and brand");
    realm_declare_intrinsic(cs_install_realm);
}

/* THE REFERENCES THIS COMPONENT HOLDS, GIVEN BACK — and ONLY those. Every HANDLE it declared is undone by the
   last line, out of the declarations themselves, because a release written as a second copy of the declaration
   list is a list kept in step by whoever remembers: this one reset THREE of its four slots and left
   `g_cs_class`, which agent_state_check_released reports as a component that "reports itself built and whose
   every other handle is null" in the next agent of the process. See core/agent_state.h's agent_state_undo for
   why the two halves are split — only the component knows a slot holds a source claim rather than a handle,
   and only the registry can be relied on to name all four.
   THE CLASS ID WAS THE ONE THAT WENT MISSING, AND ITS SHAPE IS WHY. It is the only handle here with no
   reference to free beside it — a realm-value id, an atom or a JSValue puts the author's eye on its own line
   by needing a `_free` call there, and a class id owns nothing, so nothing in this body ever pointed at it.
   It is not a process-lifetime handle to be carried: it names a JS_NewClass registration in the runtime that
   is going away with it, and a second agent reading a non-zero `g_cs_class` would answer §3.7.7's brand check
   in cs_brand against a class of the dead runtime.
   THE ORDER IS PART OF THE CONTRACT and the undo is LAST. The attacker-source claim above is keyed by the
   component NAME and reads none of these four slots, so it is free to run first; a line that read one — a
   release handing another component's claim back, or asserting a claimant already has — would HAVE to. */
void cookie_store_free(void)
{
    concolic_undeclare_sources(CS_COMPONENT);
    agent_state_undo(CS_COMPONENT);
}
