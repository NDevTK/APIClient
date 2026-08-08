/* HTML §7.4 — see navigable.h. */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/frame/navigable.h"
#include "core/frame/window_proxy.h"
#include "core/frame/policy_container.h"
#include "core/frame/window_features.h"
#include "core/dom/document.h"
#include "core/url/url.h"
#include "core/idl_args.h"
#include "solver/engine.h"
#include "solver/world.h"

static char *g_origin;   /* this AGENT's origin — what an about:blank child inherits (owned) */
static RealmBuilder g_realm_builder;
static DocumentFetcher g_doc_fetcher;

/* THE REALMS THIS AGENT BUILT, and the agent is what owns them. A child realm is created by §7.4 and referred
   to by the WindowProxy over its navigable, but a proxy is a GC object and a realm is not: releasing one from
   a finalizer would free JSValues during collection, and leaving it to the proxy meant nobody ran the
   DOCUMENT's own teardown at all — the realm's `document`, its Window and its WindowProxy stayed referenced and
   JS_FreeRuntime's gc_obj_list walk counted the whole child page as surviving objects. The agent holds them and
   releases them with itself; a proxy BORROWS. */
static JSContext **g_realms;
static int         g_realms_n, g_realms_cap;

void navigable_set_realm_builder(RealmBuilder b) { g_realm_builder = b; }
void navigable_set_document_fetcher(DocumentFetcher f) { g_doc_fetcher = f; }


/* THE CHILD'S ADDRESS AND ORIGIN, from ONE parse. An `about:blank` navigable — which is what `open()` with no
   URL creates, and what an `<iframe>` with no `src` creates — inherits the CREATOR'S origin; that is the whole
   reason a same-origin popup or frame can be scripted at all. Any other URL contributes its own.
   IT IS RESOLVED HERE, not by the host. `open("/admin")` and `<iframe src=a.html>` are how most real uses are
   written, and a relative reference has neither an origin nor a meaning outside the document that wrote it —
   handing the host the raw text would make it resolve against something, and the only base it could pick is a
   guess. Returns false when the reference does not parse; both outputs are owned on success. */
static bool child_address(JSContext *ctx, const char *url, char **out_url, char **out_origin)
{
    UrlRecord base, rec;
    const char *base_url;
    bool have_base, ok = false;

    /* THE about:blank CASE ANSWERS WITHOUT A BASE, and must be checked FIRST. Reading the document's address
       up here evaluated it even for `open()` with no argument — the one call that needs no address at all —
       and in a host with no document installed that is an assert, not a value. It aborted sixteen spec files
       whose only sin was calling open(). */
    if (!url || !*url || !strcmp(url, "about:blank")) {
        *out_url = strdup("about:blank");
        *out_origin = strdup(g_origin ? g_origin : "null");
        CHECK(*out_url && *out_origin, "navigable: OOM naming an about:blank child");
        return true;
    }
    base_url = document_base_url(ctx);
    url_record_init(&base);
    have_base = base_url && url_parse(&base, base_url, strlen(base_url), NULL);
    url_record_init(&rec);
    if (url_parse(&rec, url, strlen(url), have_base ? &base : NULL)) {
        *out_url = url_serialize(&rec, false);
        *out_origin = url_serialize_origin(&rec);
        ok = *out_url != NULL && *out_origin != NULL;
    }
    url_record_free(&rec);
    url_record_free(&base);
    return ok;
}

/* §7.4's CREATE A NEW NAVIGABLE, as ONE operation with ONE result — see navigable.h for why it is synchronous
   and why the host is told rather than asked. Both callers are here: `window.open` below, and §4.8.5's iframe
   insertion steps. They differed only in which fields they filled into the request, which is exactly the kind
   of duplication that lets two call sites drift into two protocols. */
/* WHERE THE CHILD LIVES, and it is decided by its ORIGIN because an instance is an ORIGIN-KEYED AGENT. An
   opaque origin is same-origin with NOTHING — not even with another opaque one — which is the same rule
   §7.2.5.1's check states and SECURITY.md states for the credentialed-read principal, because it is one
   concept: a sandboxed document must not end up sharing a heap with the document that sandboxed it. */
static bool child_in_this_agent(const char *child_origin)
{
    DCHECK(g_origin != NULL, "a child navigable was created before navigable_install named this agent's origin");
    if (!strcmp(g_origin, "null") || !strcmp(child_origin, "null")) return false;
    return !strcmp(g_origin, child_origin);
}

/* THE CHILD'S DOCUMENT — §7.4's initial about:blank one, or the parse of the address it was navigated to.
   A REAL LEXBOR PARSE either way, because tree construction always produces <html><head><body> and a child
   whose body is missing is not a document a page can append to.
   THE PARSE IS THE ENGINE'S AND THE BYTES ARE THE HOST'S, which is the whole reason those are two calls: this
   engine owns what a document IS (CLAUDE.md: Lexbor and quickjs own all semantics) and the host owns the
   network. It used to be one host callback holding both, and the host that implemented only the second half
   looked finished. */
static lxb_html_document_t *child_document(JSContext *ctx, const char *url, const char *inherit_csp,
                                           char **pcsp)
{
    static const char EMPTY[] = "<!doctype html><html><head></head><body></body></html>";
    lxb_html_document_t *dom = lxb_html_document_create();
    char *bytes = NULL;
    size_t n = 0;

    *pcsp = NULL;

    CHECK(dom != NULL, "navigable: OOM creating a child navigable's Document");
    /* §7.4 STEP 14's NAVIGATE, for an address there is anything to fetch at — see navigable.h for why an
       `about:` scheme has nothing. A host with no fetcher has not built navigation, and that is named HERE
       rather than answered with the empty document: a popup whose scripts never ran is indistinguishable in
       the output from a page that had none, which is a surface reported as explored and never reached. */
    if (url && *url && strncmp(url, "about:", 6) != 0) {
        DCHECK(g_doc_fetcher != NULL,
               "a navigable was navigated to an address in an agent whose host declared no document fetcher — "
               "§7.4 step 14 fetches it and this host cannot, so the child would silently keep the empty "
               "about:blank Document and its scripts would never run. Build the navigation: the address is a "
               "host-owed request, so the navigating flow parks on it and resumes when the response lands "
               "(navigable_set_document_fetcher)");
        bytes = g_doc_fetcher(ctx, url, &n, pcsp);
    } else {
        /* §7.2.6 + §7.4: A DOCUMENT CREATED FROM NO RESPONSE CLONES ITS CREATOR'S POLICY CONTAINER. It is the
           same test one line up that decides both — there is one question ("did this document come from a
           response?") and it is asked once — and it is not bookkeeping: an `about:blank` child runs its
           scripts under the CREATOR's CSP, so a page that reaches `f.contentWindow.eval(...)` through a
           srcless iframe is governed by the parent's policy. An empty container there reports that as a
           working exploit on a page whose CSP kills it, which is the very false PoC @S must never emit.
           WHOSE POLICY IT IS DEPENDS ON THE OPERATION, so the caller says: CREATING a navigable clones the
           creator's, taken when the navigable was created (window_proxy.h) because a srcless child's realm is
           built later and by whichever document reads through it first; NAVIGATING one to an `about:` address
           clones the INITIATOR's, the document whose script ran. Reading it off `ctx` here would answer the
           first question with the second's document. The clone is BY VALUE — policy_container_clone is itself
           a re-parse of this text — so the new document's policy is its own from the moment it exists. */
        const char *text = inherit_csp;
        if (text) {
            *pcsp = strdup(text);
            CHECK(*pcsp != NULL, "navigable: OOM cloning the inherited policy container for an about: document");
        }
    }
    /* A CHILD WHOSE ADDRESS DOES NOT LOAD KEEPS THE EMPTY DOCUMENT — a browser shows an error page and the
       navigable still exists. That is a failed fetch, not a missing capability, and the two are different. */
    CHECK(lxb_html_document_parse(dom, bytes ? (const lxb_char_t *)bytes : (const lxb_char_t *)EMPTY,
                                  bytes ? n : sizeof EMPTY - 1) == LXB_STATUS_OK,
          "a child navigable's Document did not parse");
    free(bytes);
    return dom;
}

/* BUILD A CHILD NAVIGABLE'S REALM — see navigable.h. Called by the WindowProxy, which is the ONE place a realm
   is materialized: §7.4's navigation asks for it in the creating turn, and a read through a not-yet-materialized
   about:blank navigable asks for it then. */
JSContext *navigable_realm(JSContext *ctx, uint32_t doc, const char *url, const char *origin,
                           JSValueConst nav_proxy, const char *inherit_csp)
{
    JSContext *cctx;

    DCHECK(g_realm_builder != NULL,
           "a same-origin child navigable was reached in an agent whose host declared no realm builder — a "
           "same-origin document is a second REALM in this heap, and only the host knows which platform "
           "surface a document of this build has; declare it with navigable_set_realm_builder");
    {
        /* THE POLICY TRAVELS WITH THE TREE — the response's, or the creator's cloned for an `about:` child;
           child_document owns which, since it owns the test that decides. Freed here because the builder
           installs a container built from it and keeps no pointer. */
        char *csp = NULL;
        lxb_html_document_t *dom = child_document(ctx, url, inherit_csp, &csp);
        cctx = g_realm_builder(JS_GetRuntime(ctx), dom, url, origin, csp, doc, nav_proxy);
        free(csp);
    }
    CHECK(cctx != NULL, "the host's realm builder produced no realm for a same-origin child navigable");
    if (g_realms_n == g_realms_cap) {
        int cap = g_realms_cap ? g_realms_cap * 2 : 8;
        JSContext **g = realloc(g_realms, (size_t)cap * sizeof *g);
        CHECK(g != NULL, "navigable: OOM recording a child realm — an unrecorded realm is never torn down");
        g_realms = g;
        g_realms_cap = cap;
    }
    g_realms[g_realms_n++] = cctx;
    return cctx;
}

/* §7.4 STEP 14's NAVIGATE, for a navigable that ALREADY HAS an active document — the other end of the sentence
 * navigable_realm implements. Its whole difference from materializing one is that the proxy is already bound:
 * this fetches the new document, builds the realm its scripts run in, and hands both to §7.2.5.1's replace, so
 * the page's handle on the navigable now reaches the new document and the old realm stays where a parked flow
 * left it.
 *
 * WHAT IS NOT HERE YET, and it is a mechanism rather than a line: the fetch is the HOST's synchronous one
 * (child_document), so a host whose network parks the flow — which is every host that is not a test runner —
 * cannot navigate at all, and navigable.h's DCHECK says so at the fetcher. §7.4's navigate becoming a
 * scheduled work item (the navigating flow parks on the response and resumes with it) is that mechanism, and
 * it replaces the ONE line below rather than anything else in this function.
 *
 * THE ADDRESS IS RESOLVED AGAINST THE NAVIGATING DOCUMENT, not against the target's — §4.4's API base URL
 * belongs to the document whose script ran, which for `open("/x", "_self")` happens to be the same document
 * and for `open("/x", "someFrame")` is not. */
JSValue navigable_navigate(JSContext *ctx, JSValueConst proxy, const char *url)
{
    char *addr = NULL, *origin = NULL;
    uint32_t doc;
    JSContext *cctx;

    DCHECK(window_proxy_is(proxy), "something that is not a WindowProxy was navigated");
    if (!child_address(ctx, url, &addr, &origin)) {
        free(addr); free(origin);
        return JS_UNDEFINED;   /* §7.4 step 4: the caller turns this into a SyntaxError */
    }
    /* A CROSS-ORIGIN DESTINATION IS A PEER'S DOCUMENT. An instance is an ORIGIN-KEYED agent cluster, so
       navigating one of this agent's navigables off-origin moves its active document to another instance —
       which is the host route the create path already builds a notice for, and is not this. */
    DCHECK(child_in_this_agent(origin),
           "a navigable was navigated CROSS-ORIGIN — its active document then lives in a peer instance, which "
           "is a host-routed provisioning like §7.4's create notice and not a realm this agent can build");
    /* A NAVIGATION MAKES A NEW DOCUMENT, so it gets a new identity: the world registry names documents, not
       navigables, and a parked flow in the superseded one must still be able to name where it is. */
    doc = world_mint_doc(document_doc(ctx));
    world_doc_adopt(doc);
    /* §7.2.6: a destination with no response inherits the INITIATOR's policy — this document's. */
    cctx = navigable_realm(ctx, doc, addr, origin, proxy, policy_container_csp(document_policy(ctx)));
    window_proxy_navigate(ctx, proxy, cctx, doc, addr, origin);
    free(addr);
    free(origin);
    return JS_DupValue(ctx, proxy);
}

/* §7.1's RULES FOR CHOOSING A NAVIGABLE, for the four KEYWORD targets — the ones whose leading underscore says
 * "reuse a navigable" rather than "name a new one". `_blank` is the odd one: it is the only keyword that means
 * CREATE, which is why it answers JS_UNDEFINED here and every other answer is a navigable to navigate.
 * A NON-KEYWORD name that matches an existing navigable is the fifth rule and is NOT built: it needs the
 * familiar-with walk over the navigable tree, and the tree's children are the iframe elements' — so it is the
 * next piece, and until it is here such a name creates a new navigable and is GIVEN that name, which is what
 * the same rule says for a name that matches nothing. */
static JSValue navigable_choose_keyword(JSContext *ctx, const char *target)
{
    JSValueConst self = document_window_proxy(ctx);

    if (!target || !*target || !strcmp(target, "_self")) return JS_DupValue(ctx, self);
    if (!strcmp(target, "_parent")) {
        /* §7.1: a navigable with no parent is its own parent — the keyword never reaches past the top. */
        JSValue p = window_proxy_parent(ctx, self);
        if (window_proxy_is(p)) return p;
        JS_FreeValue(ctx, p);
        return JS_DupValue(ctx, self);
    }
    if (!strcmp(target, "_top")) return window_proxy_top_of(ctx, self);
    return JS_UNDEFINED;   /* `_blank`, and any other `_`-prefixed token, CREATE */
}

JSValue navigable_create(JSContext *ctx, const char *url, const char *name, bool is_child,
                         const WindowFeatures *feat)
{
    const char *csp = policy_container_csp(document_policy(ctx));
    char *addr = NULL, *origin = NULL;
    uint32_t child;
    JSValue proxy;
    char *op;
    size_t n;

    if (!child_address(ctx, url, &addr, &origin)) {   /* the reference does not parse; the caller decides what that means */
        free(addr); free(origin);
        return JS_UNDEFINED;
    }
    /* MINTED FROM THE CREATOR, not from the instance root: this agent holds one realm per same-origin
       document, and naming every child "<root>.<n>" would collide the moment two realms both created one. */
    child = world_mint_doc(document_doc(ctx));
    /* WHO THE NAVIGABLE HANGS OFF, and the two shapes §7.4 distinguishes. A CHILD navigable (§4.8.5's iframe)
       is nested in this one: its `parent` is this Window and it has no opener. An AUXILIARY one (§7.4's popup)
       is a TOP-LEVEL traversable — it is its own parent and its own top — and what links it back here is
       `opener`. Getting this pair backwards is not a detail: `parent`/`top`/`opener` are how a frame and a
       popup tell each other apart, and how testharness.js finds the window it is running in. */
    if (child_in_this_agent(origin)) {
        /* A SECOND REALM IN THIS HEAP — HTML's similar-origin window agent, and the reason it is not a second
           instance: the corpus moves LIVE NODES across this boundary (a subframe created here and appended to
           the child's body, whose `parentNode` is then a node of the child) and assigns LIVE CLOSURES into the
           child's event handlers. Neither is a value that can be named across a transport; they are one object
           graph, and a browser's own model says so. */
        world_doc_adopt(child);   /* this agent holds it */
        /* §7.2.5: `parent` and `opener` ARE WindowProxies. They held the creator's GLOBAL, which made the
           `top` walk leave the proxies at the top and read a scriptable property to continue — and a popup's
           opener a Window rather than the navigable it belongs to. The creator's own proxy is what both are. */
        /* §7.4: `noopener` means the new navigable HAS NO OPENER — not that the opener is hidden, that there
           is not one — so the link is never made rather than made and filtered. A CHILD navigable (§4.8.5's
           iframe) has no opener in the first place and no features to ask. */
        proxy = window_proxy_new(ctx, child, addr, origin, name, feat && feat->is_popup,
                                 /* §7.2.6/§7.4: a navigable created with no address has no response to carry a
                                    policy, so it CLONES THE CREATOR'S — the SAME `csp` this function already
                                    sends to a PEER instance for a cross-origin child, which is where the gap
                                    was: the remote path carried the creator's policy and the local one did
                                    not. Taken now rather than at materialization, because a srcless child's
                                    realm is built later and by whichever same-origin document reads through
                                    it first, which need not be its creator. */
                                 csp,
                                 is_child ? document_window_proxy(ctx) : JS_UNDEFINED,
                                 (is_child || (feat && feat->noopener)) ? JS_NULL
                                                                        : document_window_proxy(ctx));
        CHECK(!JS_IsException(proxy), "a navigable's WindowProxy could not be allocated");
        /* §7.4's DECISION IS A FACT ABOUT THE NAVIGABLE, so it is read back where it was written. A popup that
           does not know it is one answers `true` from all six of §7.2.5.3's BarProps, which is precisely how
           the corpus tells a popup from a tab — and a value written and then not there is worth catching at
           the write rather than 51 subtests downstream in another realm. */
        DCHECK(window_proxy_is_popup(proxy) == (feat != NULL && feat->is_popup),
               "a navigable did not keep §7.4's popup decision across its own creation");
        /* §7.4 STEP 14: NAVIGATE THE NEW NAVIGABLE TO url — and navigating is what makes the document RUN.
           A navigable that was given an address is materialized HERE, in the creating turn, because its
           document's own scripts are an observable that owes nothing to the creator: a popup posts back to its
           opener without the opener ever touching the proxy. Deferring this was twelve files in html/browsers
           reporting nothing at all — not a saving, a popup that never ran.
           A navigable with NO address is NOT materialized here, and that is the same sentence read the other
           way: it holds the initial about:blank Document §7.4 created it with, that Document has no scripts by
           construction, and the ONLY way to observe it is a read through this proxy — which builds it. The
           deferral has no observable there, and it is load-bearing: a forced-execution frontier runs this
           `open()` once per flow, so materializing every never-touched about:blank exhausted the heap at ~2030
           flows with the initial parse failing to allocate. The line between the two is what a navigable DOES,
           not what it costs. */
        /* THE TEST IS "IS THERE ANYTHING TO FETCH", which for §7.4 is the `about:` scheme: about:blank has no
           response and no content, so navigating to it produces the Document the navigable already has. It is
           the same test the RealmBuilder applies to decide whether to fetch, and it is stated in both places
           because it is one spec fact about the scheme rather than a protocol between them. */
        if (strncmp(addr, "about:", 6) != 0)
            (void)window_proxy_realm(ctx, proxy);
    } else {
        /* THE NOTICE, and every field of it is load-bearing. The CHILD is the name the host provisions an
           instance under; the CREATOR names who made it, which is what the host routes replies through and what
           a browser would decide policy from; the URL is the child's initial address; the ORIGIN is the
           child's; the POLICY is §7.4's CLONE OF THE CREATOR'S, serialized — which the policy container can do
           precisely because it is a flat parse over one owned string, so the clone that crosses an instance and
           the clone that crosses a session are the same operation. */
        n = strlen(world_doc_name(child)) + strlen(world_doc_name(document_doc(ctx))) +
            strlen(addr) + strlen(origin) + strlen(csp ? csp : "") + 32;
        op = malloc(n);
        CHECK(op != NULL, "navigable: OOM building the create notice");
        snprintf(op, n, "navigable.create\t%s\t%s\t%s\t%s\t%s", world_doc_name(child),
                 world_doc_name(document_doc(ctx)), addr, origin, csp ? csp : "");
        engine_host_notify(ctx, op);
        free(op);
        proxy = window_proxy_new_remote(ctx, child, origin, name,
                                        is_child ? document_window_proxy(ctx) : JS_UNDEFINED,
                                        (is_child || (feat && feat->noopener)) ? JS_NULL
                                                                               : document_window_proxy(ctx));
        CHECK(!JS_IsException(proxy), "a navigable's WindowProxy could not be allocated");
    }
    free(addr);
    free(origin);
    return proxy;
}

/* §7.4's `window.open`. NOT a step machine any more: the child's name is minted here, so there is nothing to
   ask and nothing to suspend for — which is also what the spec says, since `open()` hands back a WindowProxy at
   its own call site. */
static JSValue js_win_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    const char *url = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    const char *target = argc > 1 ? JS_ToCString(ctx, argv[1]) : NULL;
    /* §3.6.2: an OPTIONAL argument given `undefined` is ABSENT. `open(url, name, undefined)` is how the
       corpus spells "no features", and stringifying it produced the literal "undefined" — one token,
       a non-empty map, and therefore a popup where the spec has a tab. */
    const char *features = (argc > 2 && !JS_IsUndefined(argv[2])) ? JS_ToCString(ctx, argv[2]) : NULL;
    WindowFeatures feat;
    JSValue r;

    (void)this_val; (void)magic;
    if (argc > 0 && !url) return JS_EXCEPTION;
    /* §7.4's THIRD ARGUMENT, which was read and dropped. It decides whether the new navigable is a POPUP —
       what §7.2.5.3's six BarProps answer from — and whether it gets an OPENER at all. */
    feat = window_features_parse(features);
    /* §7.4 step 6: APPLY THE RULES FOR CHOOSING A NAVIGABLE. A keyword target names a navigable to REUSE, and
       reusing one means NAVIGATING it — which this used to skip entirely: `open(url, "_self")` dropped the
       keyword, fell through to create, and answered with a SECOND window where the spec navigates the one the
       script is running in. A wrong answer that looks like it worked, which is the shape this whole file's
       asserts exist to prevent. `_blank` (and any keyword this does not know) still CREATES. */
    /* §7.1's FIRST RULE IS `noopener`, and it comes before the keywords: a request that must not be able to
       script its opener cannot be answered with a navigable the opener already holds, so noopener always
       CREATES — `open(url, "_self", "noopener")` is a new window, not this one navigated. */
    if (target && target[0] == '_' && !feat.noopener) {
        JSValue chosen = navigable_choose_keyword(ctx, target);
        if (window_proxy_is(chosen)) {
            /* §7.4 step 14 over the chosen navigable. `url` may be absent — `open("", "_self")` navigates to
               the empty string, which §7.4 resolves against the document's own address, so it reloads. */
            r = navigable_navigate(ctx, chosen, url ? url : "");
            JS_FreeValue(ctx, chosen);
            goto done;
        }
        JS_FreeValue(ctx, chosen);
    }
    r = navigable_create(ctx, url, target && target[0] != '_' ? target : NULL, false, &feat);
done:
    if (url) JS_FreeCString(ctx, url);
    if (target) JS_FreeCString(ctx, target);
    if (features) JS_FreeCString(ctx, features);
    /* §7.4 step 4: a URL that does not parse is a SyntaxError, and it is the PAGE's mistake. */
    if (JS_IsUndefined(r))
        return JS_ThrowDOMException(ctx, "SyntaxError", "the URL to open is not a URL");
    /* §7.4's LAST STEP: with `noopener`, `open()` RETURNS NULL. The navigable is created and navigated all the
       same — a page that opens a window it cannot script still opened one — and what the caller loses is the
       handle, which is the whole point of the feature. */
    if (feat.noopener) { JS_FreeValue(ctx, r); return JS_NULL; }
    return r;
}

static int g_id_open;

/* §7.4's IDL: open(optional USVString url = "", optional DOMString target = "_blank",
   optional [LegacyNullToEmptyString] DOMString features = "") -> WindowProxy?
   DECLARED ONCE PER AGENT — a member has one pool entry, and every realm's global installs that same one. */
void navigable_init(JSContext *ctx)
{
    static const IdlArgType OPEN_ARGS[3] = { IDL_USVSTRING, IDL_DOMSTRING, IDL_DOMSTRING };

    g_id_open = idl_method_id(ctx, OPEN_ARGS, 3, js_win_open, 0);
    idl_optional_from(0);
}

void navigable_install(JSContext *ctx, JSValueConst global, const char *origin)
{
    /* THE ORIGIN IS THE AGENT'S, NOT THE DOCUMENT'S. An agent is origin-keyed, so every document installed into
       this instance has the same one and a second install is a second DOCUMENT, not a contradiction — but a
       DIFFERENT origin arriving here would mean two principals behind one instance, which SECURITY.md's
       one-principal-per-instance rule forbids and which would make an about:blank child inherit the wrong one. */
    DCHECK(g_origin == NULL || !strcmp(g_origin, origin ? origin : "null"),
           "a second document was installed into this agent with a DIFFERENT origin — an agent is origin-keyed, "
           "so a cross-origin document is a second INSTANCE and never a second realm in this one");
    if (!g_origin) {
        g_origin = strdup(origin ? origin : "null");
        CHECK(g_origin != NULL, "navigable: OOM recording this agent's origin");
    }
    idl_install_method(ctx, global, "open", 0, g_id_open);
}

void navigable_free(JSContext *ctx)
{
    int i;

    (void)ctx;
    /* EACH CHILD REALM'S DOCUMENT FIRST, then the realm: document_free is what releases the references the
       Document holds across its lifecycle — its `document` object, its Window and its WindowProxy — and a
       realm freed without it leaves that graph referenced by nothing the GC can see. */
    for (i = 0; i < g_realms_n; i++) {
        document_free(g_realms[i]);
        JS_FreeContext(g_realms[i]);
    }
    free(g_realms);
    g_realms = NULL;
    g_realms_n = g_realms_cap = 0;
    g_realm_builder = NULL;
    free(g_origin);
    g_origin = NULL;
}
