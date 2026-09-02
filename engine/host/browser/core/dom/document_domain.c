/* HTML §7.1.1.2's `document.domain`. See document_domain.h for why it is its own component. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/document.h"
#include "core/dom/document_domain.h"
#include "core/dom/node.h"
#include "core/frame/agent_cluster.h"
#include "core/frame/sandboxing.h"
#include "core/frame/window_proxy.h"
#include "core/idl_args.h"
#include "core/url/origin.h"
#include "core/url/public_suffix.h"
#include "core/url/url.h"
#include "solver/concolic.h"

/* §3.1.1's `attribute USVString domain` — the only member here, and read-write, which is what decides whether an
   assignment runs the setter's algorithm or is a TypeError in strict mode. */
static int g_id_domain_set = -1;

/* WEB IDL §3.7.5's BRAND CHECK — a TypeError thrown AT THE READ and not an engine invariant, for the reason
   document_metadata.c's own receiver states: the corpus pulls these accessors off the prototype and applies
   them to the wrong receiver deliberately. */
static lxb_dom_document_t *dd_receiver(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_node_t *n = node_of(this_val);

    if (!n || n->type != LXB_DOM_NODE_TYPE_DOCUMENT) {
        JS_ThrowTypeError(ctx, "this is not a Document");
        return NULL;
    }
    return lxb_dom_interface_document(n);
}

/* "THIS'S ORIGIN", AND "THIS'S BROWSING CONTEXT" IN THE SAME ANSWER, because in this engine they are one lookup
   and the setter's step 1 is the null case of it. A Document is the active document of at most one navigable —
   `createHTMLDocument`, a DOMParser parse and XHR's `responseXML` each build a Document that is the active
   document of NOTHING — so document_active_realm_of answering NULL IS "this's browsing context is null", and
   the navigable's WindowProxy is what carries the origin §7.3.1 gave that document.
   IT IS READ OFF THE RECEIVER, never off the running realm: `frame.contentDocument.domain` set from the parent
   is an operation on the CHILD's origin, and a child navigable holds its own tuple record. */
static const Origin *dd_origin(lxb_dom_document_t *dom)
{
    JSContext *cctx = document_active_realm_of(lxb_dom_interface_node(dom));

    if (!cctx) return NULL;
    return window_proxy_origin(document_window_proxy(cctx));
}

/* §7.1.5's SANDBOXED document.domain BROWSING CONTEXT FLAG, which is the setter's step 2 — "this flag prevents
   content from using the document.domain setter". It is one of the two flags §7.1.5's parse-a-sandboxing-
   directive adds UNCONDITIONALLY, with no `unless tokens contains ...` beside it, so ANY `<iframe sandbox>`
   sets it and no keyword can relax it.
   IT IS READ OFF THE RECEIVER'S DOCUMENT, never off the running realm, for the same reason dd_origin above is:
   `frame.contentDocument.domain = "x"` set from the parent is an operation on the CHILD, and the child is the
   Document §7.1.5 gave a flag set to. The old placeholder answered `false` for every document and said the set
   lived in §7.2.6's policy container — it does not: §7.1.7's container has a CSP list, an embedder policy, a
   referrer policy and two integrity policies, and a Document's ACTIVE SANDBOXING FLAG SET is a field of the
   Document, handed to it at creation by §7.2 or §7.4.5. */
static bool dd_sandboxed(lxb_dom_document_t *dom)
{
    JSContext *cctx = document_active_realm_of(lxb_dom_interface_node(dom));

    /* The setter's step 1 has already returned for a Document that is the active document of no navigable, so
       reaching here without a realm would mean step 1 and step 2 disagree about the same fact. */
    DCHECK(cctx != NULL, "§7.1.1.2's setter reached its step 2 for a Document with no browsing context — step 1 "
                         "throws a SecurityError for exactly that Document, so the two steps are reading two "
                         "different answers to `this's browsing context is null`");
    return (document_active_sandbox_flags(cctx) & SANDBOX_DOCUMENT_DOMAIN) != 0;
}

/* "X, PREFIXED BY U+002E (.), MATCHES THE END OF Y" — the phrase §7.1.1.2 uses three times, as one function so
   the three cannot drift. It is a strict test: `example.com` does not match the end of `example.com` (there is
   no room for the dot), which is what makes step 4.3's first disjunct and its second disjunct different
   questions. */
static bool dot_suffix_of(const char *x, const char *y)
{
    size_t xn = strlen(x), yn = strlen(y);

    return yn > xn && y[yn - xn - 1] == '.' && memcmp(y + yn - xn, x, xn) == 0;
}

/* §7.1.1.2: "To determine if a scalar value string hostSuffixString is a registrable domain suffix of or is
 * equal to a host originalHost", verbatim. `*out_suffix` receives the PARSE of hostSuffixString on every path
 * that gets one — step 6 sets the origin's domain to exactly this host, and re-running the parser for it would
 * be a second answer to one question — and is the caller's to url_host_free either way.
 *
 * THE PUBLIC SUFFIX IS THE POINT OF THE ALGORITHM. Steps 4.1 and 4.2 alone would let `www.example.com` claim
 * `com`, which is the shared-hosting attack §7.1.1.2's own warning is about; step 4.3 is what refuses it, and it
 * cannot be answered without the list (core/url/public_suffix.h). Its two disjuncts are different refusals:
 * the first refuses a value that IS a public suffix (`com`, `github.io`, `bar.ck` under `*.ck`), the second
 * refuses one that sits inside the original host's public suffix without being the whole of it. */
static bool registrable_domain_suffix_or_equal(const char *s, size_t n, const UrlHost *original,
                                               UrlHost *out_suffix)
{
    const char *sd, *od, *ps;

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
    /* STEP 4.3's two disjuncts. `original` is a domain, so URL §3.2 gives it a public suffix rather than null;
       so is `out_suffix`, for the same reason. */
    ps = public_suffix_of(out_suffix);
    DCHECK(ps != NULL, "URL §3.2 answered null for a host §7.1.1.2 step 4.1 has already established is a "
                       "domain — step 1 of §3.2 is the only null it has");
    if (strcmp(sd, ps) == 0) return false;
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

static JSValue js_doc_domain(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_document_t *dom = dd_receiver(ctx, this_val);
    const Origin *o;
    const UrlHost *eff;
    char *s;
    JSValue v;

    (void)magic;
    if (!dom) return JS_EXCEPTION;
    /* §7.1.1.2's GETTER: "1. Let effectiveDomain be this's origin's effective domain. If effectiveDomain is
       null, then return the empty string. 2. Return effectiveDomain, serialized."
       A DOCUMENT WITH NO BROWSING CONTEXT still has an origin by the spec and this engine cannot name it — its
       navigable is what holds the record — but the getter's answer is the same either way: such a document was
       created by `createDocument`/`DOMParser`, which §4.5's create-and-initialize gives a NEW OPAQUE ORIGIN, and
       an opaque origin's effective domain is null. So the empty string is a computed answer here, not a hole. */
    o = dd_origin(dom);
    eff = o ? origin_effective_domain(o) : NULL;
    if (!eff) return JS_NewStringLen(ctx, "", 0);
    s = url_serialize_host(eff);
    CHECK(s != NULL, "document.domain: OOM serializing an effective domain");
    /* THE PRINCIPAL, CONCRETE. A bundle compares this against a literal and builds cookie domains out of it, and
       a shape here would lose every endpoint behind the comparison — the same rule location.origin follows. */
    v = JS_NewString(ctx, s);
    free(s);
    return v;
}

static JSValue js_doc_set_domain(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    lxb_dom_document_t *dom = dd_receiver(ctx, this_val);
    const Origin *o;
    const UrlHost *eff;
    UrlHost parsed;
    const char *s;
    size_t n = 0;
    bool ok;

    (void)magic;
    if (!dom) return JS_EXCEPTION;
    o = dd_origin(dom);
    /* STEP 1 — "if this's browsing context is null, then throw a SecurityError". */
    if (!o)
        return JS_ThrowDOMException(ctx, "SecurityError",
                                    "a Document with no browsing context cannot relax its origin's domain");
    /* STEP 2 — the sandboxed document.domain browsing context flag. */
    if (dd_sandboxed(dom))
        return JS_ThrowDOMException(ctx, "SecurityError",
                                    "this Document is sandboxed against the document.domain setter");
    /* STEP 3 — "let effectiveDomain be this's origin's effective domain. If effectiveDomain is null, then throw
       a SecurityError." Null is §7.1.1's step 1: an OPAQUE origin, which is every `data:`, `file:` and
       sandboxed document. */
    eff = origin_effective_domain(o);
    if (!eff)
        return JS_ThrowDOMException(ctx, "SecurityError",
                                    "a Document with an opaque origin has no effective domain to relax");
    if (concolic_is(val)) {
        /* THE CAPABILITY THIS SETTER DOES NOT HAVE, NAMED. Step 4 is a decision over the assigned value's
           BYTES, and an unknown external value has none — it has a DOMAIN. §solver's rule is that such a value
           must never force a branch, so the honest answer is not to pick one: it is to FORK, running the
           throwing arm and the relaxing arm as siblings, with the relaxing arm's domain narrowed by step 4 to
           the registrable-domain suffixes of the effective domain (a set the PSL makes finite and small — for
           `a.b.example.com` it is `a.b.example.com`, `b.example.com` and `example.com`). That is a solver fork
           at a browser member, and it is not built. */
        DFAIL("document.domain = <unknown external value>: §7.1.1.2 step 4 must FORK — a throwing arm and one "
              "relaxing arm per registrable-domain suffix of this document's effective domain — and this "
              "engine can only decide the step on concrete bytes. Build the fork here; do not decide it");
    }
    DCHECK(JS_IsString(val), "document.domain= reached the body unconverted — the IDL declaration is what "
                             "converts it, and running the page's toString from here is the "
                             "drive-to-completion the flow machinery exists to avoid");
    s = JS_ToCStringLen(ctx, &n, val);
    if (!s) return JS_EXCEPTION;
    /* STEP 4 — "if the given value is not a registrable domain suffix of and is not equal to effectiveDomain,
       then throw a SecurityError". */
    ok = registrable_domain_suffix_or_equal(s, n, eff, &parsed);
    JS_FreeCString(ctx, s);
    if (!ok) {
        url_host_free(&parsed);
        return JS_ThrowDOMException(ctx, "SecurityError",
                                    "the value is neither equal to nor a registrable domain suffix of this "
                                    "document's effective domain");
    }
    /* STEP 5 — "if the surrounding agent's agent cluster's is origin-keyed is true, then return." The condition
       is a fact about THIS AGENT and it is computed, not assumed (core/frame/agent_cluster.h): a page reads the
       same fact back through `window.originAgentCluster`, and the two must be one answer. Note the ORDER — the
       standard puts this AFTER step 4, so an origin-keyed document still throws for a value it may not name and
       silently does nothing for one it may, and a page can tell those apart. */
    if (agent_cluster_is_origin_keyed()) {
        url_host_free(&parsed);
        return JS_UNDEFINED;
    }
    /* STEP 6 — "set this's origin's domain to the result of parsing the given value", which is the host step 2
       already parsed. origin_set_domain is the platform's only mutation of an origin: it captures the slot into
       the running flow's COW delta, keeps the domain alive for the agent, and asserts the instance boundary. */
    origin_set_domain(ctx, this_val, o, &parsed);
    url_host_free(&parsed);
    return JS_UNDEFINED;
}

void document_domain_init(JSContext *ctx)
{
    DCHECK(g_id_domain_set < 0, "document_domain_init ran twice — the setter's declaration is the AGENT's");
    g_id_domain_set = idl_setter_id(ctx, IDL_USVSTRING, false, js_doc_set_domain, 0);
}

void document_domain_install(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_id_domain_set >= 0, "Document's `domain` was installed before it was declared — the declaration is "
                                 "the AGENT's and the install is the REALM's");
    idl_install_accessor(ctx, proto, "domain", js_doc_domain, 0, g_id_domain_set);
}

/* RELEASED BY ITS DECLARER — §7.1.1.2's `domain` is declared from document_init, so document_agent_free gives
   it back. The accessor is each REALM's; the setter's pool entry is the agent's. */
void document_domain_free(void)
{
    DCHECK(g_id_domain_set >= 0, "§7.1.1.2's `domain` was released in an agent that never declared it");
    g_id_domain_set = -1;
}
