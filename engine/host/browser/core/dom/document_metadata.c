/* HTML §3.1.4 RESOURCE METADATA MANAGEMENT and §3.1.5 REPORTING DOCUMENT LOADING STATUS.
 *
 * Four Document members that are facts about the RESOURCE this document came from — `referrer`, `cookie`,
 * `lastModified` — and one about how far its load got — `readyState`. They are one component because they are
 * two adjacent sections of one standard stated over one thing, and because none of them is a question about the
 * tree: document.c owns the tree and this owns what the document arrived WITH.
 *
 * THEY WERE OWN DATA PROPERTIES OF ONE OBJECT, which is three defects and not a placement detail. Web IDL
 * §3.7.6 puts a regular attribute on the INTERFACE PROTOTYPE OBJECT as an accessor, and a value written onto
 * the instance instead is:
 *
 *   - WRONG IN SUBJECT. `Document.prototype.cookie` did not exist, so a page could not feature-detect it, could
 *     not patch it, and `Object.getOwnPropertyDescriptor(Document.prototype, "cookie")` — which analytics shims
 *     and anti-bot code really do read — found nothing. `Object.getOwnPropertyNames(document)` listed four names
 *     a browser lists none of.
 *   - WRONG IN TIME. `readyState` was a string RE-WRITTEN by the lifecycle every time the readiness moved, so
 *     the internal slot and the property were two statements of one fact with a window between them, and
 *     `document.readyState = "complete"` — which the IDL declares READONLY — let a page skip its own
 *     DOMContentLoaded and unblock its rendering. `delete document.cookie` succeeded.
 *   - WRONG IN VALUE, which is the one that matters here. `document.cookie` is an ATTACKER SOURCE (CLAUDE.md
 *     names it beside `location.hash`, `message.origin` and the referrer), and a source is not a value latched
 *     at install: a candidate run substitutes a source with a breakout AT MINT TIME, so a source minted once
 *     before boot could never receive one and its sink would be detected and never fire-verified. It is minted
 *     PER READ, exactly as core/frame/location.c mints `search` and `hash`.
 *
 * WHAT EACH OF THE THREE INPUTS IS, AND WHY THE ENGINE IS NOT LYING ABOUT ANY OF THEM. §solver's rule is that a
 * value is a DOMAIN plus an EXAMPLE when the code pins, computes or learns one. The cookie jar this engine was
 * handed is unknown, so the domain is unconstrained and a branch on it FORKS — and the example is what the page
 * itself put there, which this component stores, because that half IS known. The referrer's example is the empty
 * string for the same reason §3.1.4 gives ("unless it was blocked or there was no such document"): nothing
 * delivered one. Neither is collapsed to a bare concrete "" — that would make every cookie-gated and
 * referrer-gated path unreachable, which is the same mistake as a concrete `undefined` for absent app state. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "check.h"
#include "quickjs.h"
#include "solver/concolic.h"
#include "core/dom/document.h"
#include "core/dom/document_metadata.h"
#include "core/dom/node.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/url/url.h"

/* THE MEMBER LIST, IN ONE PLACE, in the order §3.1.1's partial interface declares them. The getter's magic is
   its index into it and the install walks it, so a member cannot arrive with a hand-written getter. */
#define DM_MEMBERS(X)                       \
    X(REFERRER,      "referrer")            \
    X(COOKIE,        "cookie")              \
    X(LAST_MODIFIED, "lastModified")        \
    X(READY_STATE,   "readyState")

#define DM_ENUM_ONE(id, str) DM_##id,
#define DM_NAME_ONE(id, str) str,

enum { DM_MEMBERS(DM_ENUM_ONE) DM_N };
static const char *const DM_NAME[] = { DM_MEMBERS(DM_NAME_ONE) };

/* §3.1.1 declares `cookie` READ-WRITE and the other three READ-ONLY, which is what decides whether an assignment
   is the setter's algorithm or a TypeError in strict mode. Stated once, beside the names. */
static int g_id_cookie_set = -1;

/* THIS DOCUMENT'S COOKIE STORE — see the cookie getter. Per REALM, which is per document that has a browsing
   context: §3.1.4 makes every OTHER Document in the agent (a createHTMLDocument, a DOMParser parse, an XHR
   responseXML) COOKIE-AVERSE, so none of them has a store to be confused with this one's.
   IT IS A JS OBJECT AND NOT A malloc'd LIST, which is §State-isolation's rule and is load-bearing here: a
   cookie the page sets is a per-flow write, so the arm that ran `document.cookie = "session=1"` is the only arm
   whose later read sees it — the store's mutations are ordinary property writes the heap COW already captures,
   and the record parks to the cold tier and resumes with the flow. */
static int g_cookie_slot = -1;

/* WEB IDL §3.7.5's BRAND CHECK. A member reached with a receiver that does not implement the interface is a
   TypeError thrown AT THE READ — the corpus pulls these getters off the prototype and applies them deliberately
   — so it is not an engine invariant and not a DCHECK. Answers the receiver's DOM document, which is what says
   WHICH document the member is about. */
static lxb_dom_document_t *dm_receiver(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_node_t *n = node_of(this_val);

    if (!n || n->type != LXB_DOM_NODE_TYPE_DOCUMENT) {
        JS_ThrowTypeError(ctx, "this is not a Document");
        return NULL;
    }
    return lxb_dom_interface_document(n);
}

/* ---- §3.1.4's cookie-averse test and the origin question in front of the store ---------------------------- */

/* §3.1.4: "A Document object whose browsing context is null" or "a Document whose URL's scheme is not an HTTP(S)
   scheme" is COOKIE-AVERSE — the getter answers the empty string and the setter does nothing. Both halves are
   read off the document itself rather than off the running realm, because the receiver is what names which
   document the member is about. */
static bool dm_cookie_averse(lxb_dom_document_t *dom)
{
    const char *url = document_url_of(dom);
    UrlRecord rec;
    bool http;

    if (JS_IsNull(document_window_of(lxb_dom_interface_node(dom))))
        return true;    /* no browsing context */
    /* A document with a browsing context always has an address — document_install refuses to build one for a
       realm with no URL — so an unparseable one here is a disagreement about what an address is, not a state. */
    if (!url || !*url) return true;
    CHECK(url_parse(&rec, url, strlen(url), NULL),
          "a document's own address is not a URL — the host captured something this engine cannot make a "
          "principal out of");
    http = rec.scheme && (strcmp(rec.scheme, "http") == 0 || strcmp(rec.scheme, "https") == 0);
    url_record_free(&rec);
    return !http;
}

/* §3.1.4's second condition, "the Document's origin is an OPAQUE ORIGIN" — a "SecurityError" from both the
   getter and the setter. A document's origin is opaque when its navigable container sandboxes it (§7.6.2's
   `<iframe sandbox>` without `allow-same-origin`), and this engine's §7.2.6 policy container carries a CSP and
   nothing else, so no document it builds has a sandboxing flag set to read. Not a skipped step: a condition
   whose state cannot exist, evaluated at the step that asks it — the same shape core/html/autofocus.c and
   core/html/html_form.c evaluate the other two sandboxing flags with, and the same three sites the day the flag
   set lands in the policy container. */
static bool dm_origin_is_opaque(lxb_dom_document_t *dom)
{
    (void)dom;
    return false;
}

/* ---- the cookie store ------------------------------------------------------------------------------------ */

/* THE RECEIVER'S STORE, WHICH IS ITS OWN REALM'S AND NEVER THE RUNNING ONE'S. `frame.contentDocument.cookie` is
   read from the PARENT's realm — two same-origin documents are one agent, so that is an ordinary read and not a
   cross-instance one — and answering it out of `ctx` would hand the parent's cookies back for the child's
   document. It is the per-realm-fact defect CLAUDE.md names, and the fix is to ask the document.
   Built WITH the realm (below), so it belongs to the pre-boot BASELINE: a record made on first touch would be
   made inside whichever flow happened to read first, and would be that flow's jar rather than the one every flow
   forks from. */
static JSValue dm_cookie_jar(lxb_dom_document_t *dom)
{
    JSContext *realm = document_active_realm_of(lxb_dom_interface_node(dom));
    JSValue jar;

    DCHECK(realm != NULL,
           "a cookie store was asked for a document that is no realm's ACTIVE document — §3.1.4 makes such a "
           "document COOKIE-AVERSE, so the averse test in front of this is what must have answered first");
    jar = realm_value_get(realm, g_cookie_slot);
    DCHECK(JS_IsObject(jar), "a realm answered for its cookie store with no record — the store is built with "
                             "the realm, so a realm that cannot say what its cookies are never ran the install");
    return jar;   /* OWNED */
}

/* RFC 6265 §4.1.1's OWS, which §5.2 step 3 strips from both the name and the value. */
static bool dm_is_wsp(char c) { return c == ' ' || c == '\t'; }

static void dm_trim(const char **p, size_t *n)
{
    while (*n && dm_is_wsp((*p)[0])) { (*p)++; (*n)--; }
    while (*n && dm_is_wsp((*p)[*n - 1])) (*n)--;
}

/* ASCII case-insensitive equality against a lowercase literal — RFC 6265 compares attribute names and month
   names case-insensitively, and this engine may not assume a locale-aware strcasecmp is on the platform. */
static bool dm_ci_eq(const char *s, size_t n, const char *lower)
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

/* RFC 6265 §5.1.1's DELIMITER: %x09 / %x20-2F / %x3B-40 / %x5B-60 / %x7B-7E. */
static bool dm_date_delim(unsigned char c)
{
    return c == 0x09 || (c >= 0x20 && c <= 0x2F) || (c >= 0x3B && c <= 0x40) ||
           (c >= 0x5B && c <= 0x60) || (c >= 0x7B && c <= 0x7E);
}

/* Days from 1970-01-01 to y-m-d (proleptic Gregorian), Howard Hinnant's days_from_civil — the one arithmetic
   that turns a broken-down UTC date into an epoch time without a timegm the platform may not have. */
static long long dm_days_from_civil(int y, int m, int d)
{
    long long yy = y - (m <= 2);
    long long era = (yy >= 0 ? yy : yy - 399) / 400;
    long long yoe = yy - era * 400;                                  /* [0, 399] */
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  /* [0, 365] */
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           /* [0, 146096] */

    return era * 146097 + doe - 719468;
}

/* RFC 6265 §5.1.1 PARSE A COOKIE-DATE. Returns true and fills `*out` with the epoch time; false is the
   standard's "abort these steps" — an unparsable date, which §5.2.1 answers by IGNORING the attribute. */
static bool dm_parse_cookie_date(const char *s, size_t n, long long *out)
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

        while (p < n && dm_date_delim((unsigned char)s[p])) p++;   /* STEP 2's delimiters */
        start = p;
        while (p < n && !dm_date_delim((unsigned char)s[p])) p++;
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
                if (dm_ci_eq(tok, 3, MONTHS[i])) {
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
    *out = dm_days_from_civil(year, month, day) * 86400LL + hour * 3600LL + min * 60LL + sec;
    return true;
}

/* RFC 6265 §5.2 PARSE A SET-COOKIE-STRING, then §5.3's storage model for a "non-HTTP" API — the whole of what
 * §3.1.4's setter means by "act as it would when receiving a set-cookie-string for the document's URL".
 *
 * The attributes this engine can act on are the two that decide whether the cookie EXISTS: §5.2.2's Max-Age,
 * which takes precedence, and §5.2.1's Expires. `Path` and `Domain` are read and DELIBERATELY not acted on:
 * §3.1.4 says so itself — "the path restrictions on cookies are only a tool to help manage which cookies are
 * sent to which parts of the site, and are not in any way a security feature" — and one document has exactly
 * one URL, so every cookie this API can set is one this document's own read is scoped to. `Secure` and
 * `HttpOnly` are likewise not gates a same-document non-HTTP read passes or fails: §5.4 never sends over them,
 * it only refuses to STORE an HttpOnly cookie from a non-HTTP API, which is the one below.
 *
 * Returns 0 and leaves the jar untouched for a string §5.2 says to ignore entirely. */
static void dm_receive_set_cookie(JSContext *ctx, JSValueConst jar, const char *s, size_t len)
{
    const char *nv = s, *attrs = NULL, *name, *value, *eq;
    size_t nvlen = len, attrs_len = 0, name_len, value_len;
    bool have_expiry = false, remove = false, http_only = false;
    const char *semi = memchr(s, ';', len);

    if (semi) {
        nvlen = (size_t)(semi - s);
        attrs = semi + 1;
        attrs_len = len - nvlen - 1;
    }
    /* STEP 2: no "=" in the name-value-pair — ignore the set-cookie-string entirely. */
    eq = memchr(nv, '=', nvlen);
    if (!eq) return;
    name = nv;
    name_len = (size_t)(eq - nv);
    value = eq + 1;
    value_len = nvlen - name_len - 1;
    dm_trim(&name, &name_len);        /* STEP 3 */
    dm_trim(&value, &value_len);
    if (!name_len) return;            /* STEP 4 */

    /* §5.2's attribute loop: split on ";", then on the first "=" of each piece. */
    while (attrs_len) {
        const char *piece = attrs, *aeq;
        size_t plen = attrs_len, alen, vlen2;
        const char *aname, *aval;
        const char *sep = memchr(attrs, ';', attrs_len);

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
        vlen2 = aeq ? plen - alen - 1 : 0;
        dm_trim(&aname, &alen);
        dm_trim(&aval, &vlen2);
        if (dm_ci_eq(aname, alen, "max-age")) {
            /* §5.2.2: the first character must be a DIGIT or "-" and the rest DIGITs, or the attribute is
               ignored. A non-positive delta-seconds is "the earliest representable date", which §5.3 step 11
               turns into a removal; a positive one is in the future, so the cookie stays. */
            bool neg = vlen2 && aval[0] == '-';
            size_t k = neg ? 1 : 0;
            bool ok = vlen2 > k;
            long long secs = 0;
            for (; k < vlen2 && ok; k++) {
                if (aval[k] < '0' || aval[k] > '9') ok = false;
                else if (secs < 1000000000LL) secs = secs * 10 + (aval[k] - '0');
            }
            if (ok) {
                have_expiry = true;
                remove = neg || secs == 0;   /* MAX-AGE WINS over Expires — §5.3 step 3 */
            }
        } else if (!have_expiry && dm_ci_eq(aname, alen, "expires")) {
            long long when = 0;
            if (dm_parse_cookie_date(aval, vlen2, &when)) {
                have_expiry = true;
                remove = when <= (long long)time(NULL);
            }
        } else if (dm_ci_eq(aname, alen, "httponly")) {
            http_only = true;
        }
    }
    /* §5.4's non-HTTP refusal: a set-cookie-string carrying HttpOnly is ABANDONED when it arrives through an
       API rather than through a Set-Cookie header, which `document.cookie` is. */
    if (http_only) return;
    {
        JSAtom a = JS_NewAtomLen(ctx, name, name_len);

        CHECK(a != JS_ATOM_NULL, "document.cookie: a cookie name could not be interned");
        if (remove) {
            JS_DeleteProperty(ctx, jar, a, 0);
        } else {
            JSValue v = JS_NewStringLen(ctx, value, value_len);
            CHECK(!JS_IsException(v), "document.cookie: a cookie value could not be allocated");
            /* An ordinary property write — which is exactly why the store is a JS object: the heap COW captures
               it, so the arm that set this cookie is the only arm that reads it back. */
            CHECK(JS_SetProperty(ctx, jar, a, v) >= 0,
                  "document.cookie: the cookie store refused a write, and nothing of the page's is on it");
        }
        JS_FreeAtom(ctx, a);
    }
}

/* §3.1.4's "the COOKIE-STRING for the document's URL" — RFC 6265 §5.4's serialization of the cookies in the
   store, `name=value` joined by "; ", in the order they were first set. It is the EXAMPLE the concolic source
   carries and never the whole answer: the cookies a SERVER set are not in this jar and are exactly what the
   unconstrained domain stands for. Returns an owned JS string. */
static JSValue dm_cookie_string(JSContext *ctx, JSValueConst jar)
{
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0, i;
    char *acc = NULL;
    size_t acc_len = 0;
    JSValue out;

    CHECK(JS_GetOwnPropertyNames(ctx, &tab, &n, jar, JS_GPN_STRING_MASK) == 0,
          "document.cookie: the cookie store could not be enumerated, and nothing of the page's is on it");
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetProperty(ctx, jar, tab[i].atom);
        size_t nlen = 0, vlen = 0, add;
        const char *nm = JS_AtomToCStringLen(ctx, &nlen, tab[i].atom);
        const char *vs;
        char *grown;

        CHECK(!JS_IsException(v), "document.cookie: a stored cookie could not be read back");
        vs = JS_ToCStringLen(ctx, &vlen, v);
        CHECK(nm != NULL && vs != NULL, "document.cookie: a stored cookie could not be serialized");
        add = (i ? 2 : 0) + nlen + 1 + vlen;
        grown = realloc(acc, acc_len + add + 1);
        CHECK(grown != NULL, "document.cookie: OOM building the cookie string");
        acc = grown;
        if (i) { memcpy(acc + acc_len, "; ", 2); acc_len += 2; }
        memcpy(acc + acc_len, nm, nlen); acc_len += nlen;
        acc[acc_len++] = '=';
        memcpy(acc + acc_len, vs, vlen); acc_len += vlen;
        acc[acc_len] = 0;
        JS_FreeCString(ctx, nm);
        JS_FreeCString(ctx, vs);
        JS_FreeValue(ctx, v);
    }
    JS_FreePropertyEnum(ctx, tab, n);
    out = JS_NewStringLen(ctx, acc ? acc : "", acc_len);
    free(acc);
    CHECK(!JS_IsException(out), "document.cookie: the cookie string could not be allocated");
    return out;
}

/* ---- the members ----------------------------------------------------------------------------------------- */

/* §3.1.4's `lastModified`: "the date and time of the Document's source file's last modification, in the user's
   local time zone", as MM/DD/YYYY hh:mm:ss with every component but the year zero-padded to two digits — "if
   the last modification date and time are not known, the attribute must return the CURRENT date and time in the
   above format". No response this engine is handed carries a `Last-Modified`, so the second sentence is the
   answer, and it is the standard's own answer rather than a placeholder. */
static JSValue dm_last_modified(JSContext *ctx)
{
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    char buf[32];

    CHECK(lt != NULL, "lastModified: the platform could not break the current time down into a local date, and "
                      "§3.1.4 defines the member's value in the user's local time zone");
    snprintf(buf, sizeof buf, "%02d/%02d/%04d %02d:%02d:%02d", lt->tm_mon + 1, lt->tm_mday, lt->tm_year + 1900,
             lt->tm_hour, lt->tm_min, lt->tm_sec);
    return JS_NewString(ctx, buf);
}

static JSValue js_doc_metadata(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_document_t *dom = dm_receiver(ctx, this_val);

    if (!dom) return JS_EXCEPTION;
    DCHECK(magic >= 0 && magic < DM_N, "a Document metadata getter was installed with a magic that is not a "
                                       "member index — the magic IS the index into the one member X-list");
    switch (magic) {
    /* §3.1.4: "The referrer attribute must return the document's referrer." A document this engine builds is
       given no referrer, so the concrete answer is the empty string the section names for exactly that case —
       and the value is still the SOURCE, because the referrer is attacker-influenced (the attacker links to the
       page from a document they control) and a branch on it must fork. Its delivery is declared in _init. */
    case DM_REFERRER:
        return concolic_source_wrap(ctx, "{document.referrer}", "document.referrer",
                                    JS_NewStringLen(ctx, "", 0));
    /* §3.1.4's cookie GETTER, in the standard's own order: cookie-averse first, then the opaque-origin throw,
       then the cookie-string. MINTED PER READ, never once at the install — see the file header. */
    case DM_COOKIE: {
        JSValue jar, str;

        if (dm_cookie_averse(dom)) return JS_NewStringLen(ctx, "", 0);
        if (dm_origin_is_opaque(dom))
            return JS_ThrowDOMException(ctx, "SecurityError",
                                        "a document with an opaque origin has no cookies");
        jar = dm_cookie_jar(dom);
        str = dm_cookie_string(ctx, jar);
        JS_FreeValue(ctx, jar);
        return concolic_source_wrap(ctx, "{document.cookie}", "document.cookie", str);
    }
    case DM_LAST_MODIFIED:
        return dm_last_modified(ctx);
    /* §3.1.5: "The readyState getter steps are to return this's current document readiness." It is the
       DOCUMENT's, so it is asked of the component that owns the load lifecycle rather than reflected into a
       property beside it — two statements of one fact is how they came apart. */
    default:
        DCHECK(magic == DM_READY_STATE, "a Document metadata getter reached a member with no case to answer it");
        return JS_NewString(ctx, document_readiness_of(lxb_dom_interface_node(dom)));
    }
}

/* §3.1.4's cookie SETTER. `attribute USVString cookie`, so the value is a real string by the time this runs —
   the declaration converted it, and a page's `toString` ran as a rest point of the conversion rather than
   inside this C activation. */
static JSValue js_doc_set_cookie(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    lxb_dom_document_t *dom = dm_receiver(ctx, this_val);
    JSValue jar;
    const char *s;
    size_t len = 0;
    bool owned = false;

    (void)magic;
    if (!dom) return JS_EXCEPTION;
    if (dm_cookie_averse(dom)) return JS_UNDEFINED;                    /* "must do nothing" */
    if (dm_origin_is_opaque(dom))
        return JS_ThrowDOMException(ctx, "SecurityError", "a document with an opaque origin has no cookies");
    if (concolic_is(val)) {
        /* UNKNOWN EXTERNAL INPUT has no bytes. The SHAPE is what the store carries, exactly as a Text node
           carries it for `textContent =`: the jar is an EXAMPLE store, so what it holds for an unknown value is
           the unknown's own display, never a fabricated concrete cookie. */
        s = concolic_shape_c(val);
        if (!s) s = "";
        len = strlen(s);
    } else {
        DCHECK(JS_IsString(val), "document.cookie= reached the body unconverted — the IDL declaration is what "
                                 "converts it, and running the page's toString from here is the "
                                 "drive-to-completion the flow machinery exists to avoid");
        s = JS_ToCStringLen(ctx, &len, val);
        if (!s) return JS_EXCEPTION;
        owned = true;
    }
    jar = dm_cookie_jar(dom);
    dm_receive_set_cookie(ctx, jar, s, len);
    JS_FreeValue(ctx, jar);
    if (owned) JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

/* ---- the declaration and the per-realm install ------------------------------------------------------------ */

/* THIS REALM'S COOKIE STORE, built WITH the realm so it is the pre-boot BASELINE every flow forks from. */
static void document_metadata_install_realm(JSContext *ctx)
{
    JSValue jar = JS_NewObjectProto(ctx, JS_NULL);

    CHECK(!JS_IsException(jar), "this realm's cookie store could not be allocated");
    realm_value_set(ctx, g_cookie_slot, jar);
}

void document_metadata_init(JSContext *ctx)
{
    DCHECK(g_cookie_slot < 0, "document_metadata_init ran twice — the store's slot and the two sources are "
                              "declared once per AGENT");
    /* THE TWO ATTACKER SOURCES THIS COMPONENT OWNS, declared with their INTRINSIC BROWSER CONSTRAINTS — the
       thing CLAUDE.md says a source without is a PoC generator that does not reproduce.
       A COOKIE VALUE cannot carry the bytes RFC 6265 §4.1.1's cookie-value production excludes: whitespace, the
       double quote, the comma, the semicolon and the backslash. A `;` does not arrive escaped, it TERMINATES
       the cookie, so a candidate containing one is delivered as something else entirely — which is exactly the
       false-PoC shape the declaration exists to prevent.
       A REFERRER is a SERIALIZED URL with its fragment stripped, so the bytes a URL serialization
       percent-encodes never reach the page: space, `"`, `<`, `>`, the backtick — and `#`, because there is no
       fragment left for one to introduce. The apostrophe survives, which is what makes a JS-context breakout
       through a referrer real where an HTML-context one is not. */
    concolic_declare_source("document.cookie", " \",;\\", 0);
    concolic_declare_source("document.referrer", " \"<>`#", 0);
    g_cookie_slot = realm_value_declare(ctx, "RFC 6265 the cookie store of this realm's document");
    /* §3.1.1's `attribute USVString cookie` — the only read-write member here, and its type is what performs
       §3.2.11's scalar value conversion before the body ever sees the string. */
    g_id_cookie_set = idl_setter_id(ctx, IDL_USVSTRING, false, js_doc_set_cookie, 0);
    realm_declare_intrinsic(document_metadata_install_realm);
}

void document_metadata_install(JSContext *ctx, JSValueConst proto)
{
    int i;

    DCHECK(g_id_cookie_set >= 0, "Document's metadata members were installed before they were declared — the "
                                 "declarations are the AGENT's and the installs are the REALM's");
    for (i = 0; i < DM_N; i++)
        idl_install_accessor(ctx, proto, DM_NAME[i], js_doc_metadata, i,
                             i == DM_COOKIE ? g_id_cookie_set : -1);
}
