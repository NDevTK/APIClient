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
 * itself put there, which core/loader/cookie_jar.c stores, because that half IS known. The referrer's example is
 * the empty string for the same reason §3.1.4 gives ("unless it was blocked or there was no such document"):
 * nothing delivered one. Neither is collapsed to a bare concrete "" — that would make every cookie-gated and
 * referrer-gated path unreachable, which is the same mistake as a concrete `undefined` for absent app state.
 *
 * THE STORE ITSELF IS NOT HERE, and that is the other half of "one problem per file": §3.1.4 is two sentences
 * of HTML over an algorithm that belongs to a different standard, and the standard's own scope for that
 * algorithm is the USER AGENT rather than the document. So RFC 6265 §5's store, its receive and its read are
 * core/loader/cookie_jar.c — one per AGENT — and what stays here is exactly what HTML says: the cookie-averse
 * test, the opaque-origin throw, the request-uri to run those algorithms against, and the source the value is
 * minted as. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "check.h"
#include "quickjs.h"
#include "solver/concolic.h"
#include "core/dom/document.h"
#include "core/dom/document_metadata.h"
#include "core/dom/node.h"
#include "core/idl_args.h"
#include "core/loader/cookie_jar.h"
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

/* §3.1.4's COOKIE-AVERSE test AND the REQUEST-URI in one, because they are one question asked of one thing.
   "A Document object whose browsing context is null" or "a Document whose URL's scheme is not an HTTP(S) scheme"
   is cookie-averse — the getter answers the empty string and the setter does nothing. Everything else IS the
   document's URL, which is what RFC 6265 §5.3 and §5.4 mean by the request-uri: its host is the canonicalized
   request-host, its path is what the default-path and every path-match are computed from, and its scheme is
   what the Secure attribute is measured against. So the parse that answers the averse test is the parse the
   store needs, and doing it twice would be two answers to one question.
   Read off the DOCUMENT rather than off the running realm, because the receiver is what names which document
   the member is about — `frame.contentDocument.cookie` read from the parent is about the CHILD's URL.
   Returns false for a cookie-averse document. `*rec` is initialised either way and the caller ALWAYS frees it. */
static bool dm_cookie_request_uri(lxb_dom_document_t *dom, UrlRecord *rec)
{
    const char *url = document_url_of(dom);

    url_record_init(rec);
    if (JS_IsNull(document_window_of(lxb_dom_interface_node(dom))))
        return false;    /* no browsing context */
    /* A document with a browsing context always has an address — document_install refuses to build one for a
       realm with no URL — so an unparseable one here is a disagreement about what an address is, not a state. */
    if (!url || !*url) return false;
    CHECK(url_parse(rec, url, strlen(url), NULL),
          "a document's own address is not a URL — the host captured something this engine cannot make a "
          "principal out of");
    return rec->scheme && (strcmp(rec->scheme, "http") == 0 || strcmp(rec->scheme, "https") == 0);
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
       then "the cookie-string for the document's URL for a 'non-HTTP' API".
       THE SOURCE IS MINTED PER READ, never once at the install — see the file header — and the AGENT-WIDE store
       does not change that by one line: the identity is `document.cookie`, the source a candidate run
       substitutes, and it is minted HERE, in the realm whose code made the read, out of a cookie-string computed
       for THIS document's URL. Two documents sharing a jar therefore share cookies and share nothing else: each
       read is its own concolic, carrying its own document's example. */
    case DM_COOKIE: {
        UrlRecord uri;
        JSValue str;

        if (!dm_cookie_request_uri(dom, &uri)) {
            url_record_free(&uri);
            return JS_NewStringLen(ctx, "", 0);
        }
        if (dm_origin_is_opaque(dom)) {
            url_record_free(&uri);
            return JS_ThrowDOMException(ctx, "SecurityError",
                                        "a document with an opaque origin has no cookies");
        }
        str = cookie_jar_cookie_string(ctx, &uri);
        url_record_free(&uri);
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
    UrlRecord uri;
    const char *s;
    size_t len = 0;
    bool owned = false;

    (void)magic;
    if (!dom) return JS_EXCEPTION;
    if (!dm_cookie_request_uri(dom, &uri)) {                           /* "must do nothing" */
        url_record_free(&uri);
        return JS_UNDEFINED;
    }
    if (dm_origin_is_opaque(dom)) {
        url_record_free(&uri);
        return JS_ThrowDOMException(ctx, "SecurityError", "a document with an opaque origin has no cookies");
    }
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
        if (!s) { url_record_free(&uri); return JS_EXCEPTION; }
        owned = true;
    }
    /* "act as it would when receiving a set-cookie-string for the document's URL via a 'non-HTTP' API". */
    cookie_jar_receive(ctx, &uri, s, len);
    url_record_free(&uri);
    if (owned) JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

/* ---- the declaration and the per-realm install ------------------------------------------------------------ */

void document_metadata_init(JSContext *ctx)
{
    DCHECK(g_id_cookie_set < 0, "document_metadata_init ran twice — the setter's declaration and the two "
                                "sources are declared once per AGENT");
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
    /* §3.1.1's `attribute USVString cookie` — the only read-write member here, and its type is what performs
       §3.2.11's scalar value conversion before the body ever sees the string. */
    g_id_cookie_set = idl_setter_id(ctx, IDL_USVSTRING, false, js_doc_set_cookie, 0);
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
