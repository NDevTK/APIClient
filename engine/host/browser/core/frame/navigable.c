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
#include "core/html/html_iframe.h"   /* §7.2.5's document-tree child navigables — §7.1's walk descends them */
#include "quickjs-step.h"            /* §7.4 step 14's load is a step machine on the one frontier */
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

/* §7.4 STEP 14's NAVIGATE IS A SCHEDULED WORK ITEM, and it has to be one because NOT ONE of its three callers
 * can wait. §4.8.5's iframe insertion runs inside the tree-construction steps, which are plain C with no flow
 * to suspend; `window.open()` hands back a WindowProxy at its own call site; §4.6.3's activation behaviour runs
 * inside a dispatch. The spec says the same thing from the other end — `open()` RETURNS before the destination
 * loads, and a page that reads `contentDocument` in the creating turn sees the initial about:blank rather than
 * where the frame is going.
 *
 * SO THE LOAD IS A JOB, which in this engine is a call-root FLOW: preemptible, forkable, parkable. That last
 * one is the point. The fetch inside it is still the host's synchronous one, so nothing parks yet; what changed
 * is WHERE the load happens, and it is now somewhere that CAN park — which is what makes the fetch a change to
 * one function rather than to every caller of it.
 *
 * ONE OPERATION, AND THE JOB CARRIES ITS DESTINATION. The job is handed the ADDRESS and the ORIGIN it is
 * loading, never the ones already on the proxy: a navigation's destination is not the navigable's current
 * address, and a first version of this read the address off the proxy whenever the proxy had no realm yet.
 * That is right for §7.4's create — the navigable's address IS the destination there — and silently WRONG for
 * §7.1's choose-an-existing-navigable, where `window.open("post-to-top.html","iExist")` navigates a srcless
 * `<iframe name=iExist>` whose own address is about:blank and whose realm nothing has touched. The destination
 * was discarded and the navigable materialized empty; the test timed out waiting for a message from a document
 * that was never loaded. The address travels with the job.
 *
 * WHAT THE NAVIGABLE'S STATE STILL DECIDES IS ITS DOCUMENT'S IDENTITY, and only that. A navigable with a
 * realm already is being NAVIGATED, and §7.2.5.1 supersedes a document, so the new one gets a NEW name — the
 * world registry names documents rather than navigables, and a flow parked in the superseded one must still be
 * able to say where it is. A navigable with no realm yet is receiving its FIRST document, whose name §7.4
 * already minted and adopted when it created the navigable. Either way the realm is built at the job's address
 * and all five facts of the binding move together.
 *
 * THE CALLEE IS MINTED IN THE ENQUEUING REALM. A C function runs in the realm that DEFINED it, and this one
 * reads §7.2.6's inherited policy off `ctx` — one held in a static would clone the wrong document's. */
typedef struct {
    JSStepHdr hdr;   /* the load is ONE step: everything it needs came in as an argument */
} NavLoadState;

static int js_nav_load_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    NavLoadState *s = st;
    JSValueConst proxy = step_arg(&s->hdr, 0);
    JSValueConst cspv = step_arg(&s->hdr, 3);
    const char *addr, *origin, *csp = NULL;
    JSContext *cctx;
    uint32_t doc;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(window_proxy_is(proxy), "the document-load job was given something that is not a WindowProxy");
    addr = JS_ToCString(ctx, step_arg(&s->hdr, 1));
    if (!addr) return JS_STEP_ABRUPT;
    origin = JS_ToCString(ctx, step_arg(&s->hdr, 2));
    if (!origin) { JS_FreeCString(ctx, addr); return JS_STEP_ABRUPT; }
    if (!JS_IsNull(cspv) && !(csp = JS_ToCString(ctx, cspv))) {
        JS_FreeCString(ctx, origin);
        JS_FreeCString(ctx, addr);
        return JS_STEP_ABRUPT;
    }
    if (window_proxy_materialized(proxy)) {
        doc = world_mint_doc(window_proxy_doc(proxy));
        world_doc_adopt(doc);
    } else {
        doc = window_proxy_doc(proxy);   /* §7.4 minted and adopted it when it created the navigable */
    }
    cctx = navigable_realm(ctx, doc, addr, origin, proxy, csp);
    window_proxy_navigate(ctx, proxy, cctx, doc, addr, origin);
    JS_FreeCString(ctx, csp);
    JS_FreeCString(ctx, origin);
    JS_FreeCString(ctx, addr);
    return JS_STEP_DONE;
}

/* WHAT THIS MACHINE OWNS: nothing. Its four arguments are the header's and the flow owns those, and the load
   it performs leaves its results on the navigable rather than in this state. The declaration is still REQUIRED
   and is not a formality — a machine with no visit cannot be FORKED, so a concolic branch reached from inside
   the load would abort the fork instead of exploring both arms, and check_step_visits is what says so before
   anything is compiled. */
static void js_nav_load_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    (void)ctx; (void)st; (void)v;
}

static JSValue js_nav_load_fini(JSContext *ctx, void *st, bool take_result)
{
    (void)ctx; (void)st; (void)take_result;
    return JS_UNDEFINED;   /* the state holds no JSValue of its own — the job's arguments are the header's */
}

static const JSTrampStepDef js_nav_load_def = { sizeof(NavLoadState), js_nav_load_step, js_nav_load_fini, 0,
                                                .visit = js_nav_load_visit };
static int g_nav_load_stepid = -1;

/* `inherit_csp` is §7.2.6's policy for a destination that carries none of its own, and WHOSE it is depends on
   the OPERATION rather than on the navigable's state — the CREATOR's when §7.4 creates a navigable, the
   INITIATOR's when §7.2.5.1 navigates one. So the caller states it and it travels with the job, which is the
   same sentence as the address travelling: everything about where this load is going belongs to the operation
   that started it, and nothing about it can be read back off the navigable being loaded. */
static void navigable_load_enqueue(JSContext *ctx, JSValueConst proxy, const char *addr, const char *origin,
                                   const char *inherit_csp)
{
    JSValueConst argv[4];
    JSValue fn, url, org, csp;

    if (g_nav_load_stepid < 0)
        g_nav_load_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_nav_load_def);
    fn = JS_NewCFunction2(ctx, NULL, "load", 4, JS_CFUNC_step, g_nav_load_stepid);
    CHECK(!JS_IsException(fn), "the document-load job's callee could not be allocated");
    url = JS_NewString(ctx, addr);
    CHECK(!JS_IsException(url), "the document-load job's address could not be allocated");
    org = JS_NewString(ctx, origin ? origin : "null");
    CHECK(!JS_IsException(org), "the document-load job's origin could not be allocated");
    csp = inherit_csp ? JS_NewString(ctx, inherit_csp) : JS_NULL;
    CHECK(!JS_IsException(csp), "the document-load job's inherited policy could not be allocated");
    argv[0] = proxy;
    argv[1] = url;
    argv[2] = org;
    argv[3] = csp;
    JS_EnqueueCallJob(ctx, fn, 4, argv);
    JS_FreeValue(ctx, csp);
    JS_FreeValue(ctx, org);
    JS_FreeValue(ctx, url);
    JS_FreeValue(ctx, fn);
}

/* §7.2.5.1's NAVIGATE, for a navigable this agent already holds. It RESOLVES the destination and ENQUEUES the
 * load; the document arrives later, which is what the spec says from both ends — `open()` hands back a
 * WindowProxy at its own call site, and the navigable it names is still showing what it was showing.
 *
 * THE ADDRESS IS RESOLVED AGAINST THE NAVIGATING DOCUMENT, not against the target's — §4.4's API base URL
 * belongs to the document whose script ran, which for `open("/x", "_self")` happens to be the same document
 * and for `open("/x", "someFrame")` is not. Resolving it HERE and not in the job is that sentence: by the time
 * the job runs, the only document it could resolve against is the one being replaced. */
JSValue navigable_navigate(JSContext *ctx, JSValueConst proxy, const char *url)
{
    char *addr = NULL, *origin = NULL;

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
    /* §7.2.6: a destination with no response inherits the INITIATOR's policy — this document's. */
    navigable_load_enqueue(ctx, proxy, addr, origin, policy_container_csp(document_policy(ctx)));
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
/* §7.1'S FIFTH RULE — a target that is a NAME rather than a keyword names a navigable to REUSE when one with
 * that name is FAMILIAR WITH the source. What follows is that search, and both halves of it are the spec's.
 *
 * WHERE IT LOOKS. A navigable is reachable two ways and neither is a registry keyed by document: DOWN the
 * document tree (a navigable's children are its active document's iframes, walked from the tree on every ask so
 * the answer is this flow's), and ACROSS the browsing context group (the top-level traversables this agent has
 * opened, which no document holds a reference to — a page may drop `open()`'s return value and the window is
 * still there to be targeted, so the GROUP holds them, exactly as a browser does).
 *
 * THE GROUP LIST IS A JS ARRAY, and that is load-bearing rather than convenient. A navigable one forked arm
 * opened must be invisible to its sibling — otherwise `open(url, "x")` in arm A is found and navigated by arm
 * B, which is two timelines sharing a window. An array's mutations are property writes, so the COW delta
 * captures them per flow for free and the list parks and resumes with the flow like everything else; a
 * malloc'd list would have needed its own delta kind and would have leaked its entries past every unapply.
 *
 * FAMILIAR WITH reduces to "in this agent" here, and that is not a simplification: an instance IS an
 * origin-keyed agent cluster (SECURITY.md), so every navigable this walk can reach is same origin with the
 * source by construction, which is §7.1's first familiar-with clause. A cross-origin navigable is a peer's and
 * is not in this heap to be found.
 *
 * IT DOES NOT MATERIALIZE ANYTHING. An unmaterialized navigable holds the about:blank Document §7.4 created it
 * with, which has no iframes, so there is nothing below it to walk — see window_proxy.h. */
static JSValue g_group = JS_UNDEFINED;   /* the browsing context group's top-level traversables (baseline) */

/* THE WALK IS ITERATIVE, over an explicit worklist, and that is the rule rather than a preference: a tree walk
   written as a self-call is C-to-C recursion whose depth is the PAGE's iframe nesting, which is the page's to
   choose — the same reason every call in this engine trampolines onto the heap. The worklist IS the stack, in a
   place a walk can be measured and, when this becomes a step machine, suspended.
   BREADTH-FIRST, which is also the spec's answer rather than an accident of the loop: §7.1 wants the navigable
   NEAREST the source, so a name on a child must win over the same name three frames down another branch. */
static JSValue nav_find_in_tree(JSContext *ctx, JSValueConst root, const char *name)
{
    JSValue queue = JS_NewArray(ctx), hit = JS_UNDEFINED;
    uint32_t head = 0, tail = 0;

    CHECK(!JS_IsException(queue), "navigable: OOM walking the navigable tree");
    JS_SetPropertyUint32(ctx, queue, tail++, JS_DupValue(ctx, root));
    while (head < tail && JS_IsUndefined(hit)) {
        JSValue proxy = JS_GetPropertyUint32(ctx, queue, head++);
        const char *nm;

        if (!window_proxy_is(proxy) || window_proxy_closed(ctx, proxy)) { JS_FreeValue(ctx, proxy); continue; }
        /* §7.4 gave this navigable its name and §7.11 lets a document rename its own — one record, read here. */
        nm = window_proxy_name(proxy);
        if (nm && !strcmp(nm, name)) { hit = proxy; break; }
        if (window_proxy_materialized(proxy)) {
            JSContext *realm = window_proxy_realm(ctx, proxy);
            int i, n = iframe_child_navigable_count(realm);
            for (i = 0; i < n; i++)
                JS_SetPropertyUint32(ctx, queue, tail++, iframe_child_navigable(realm, i));
        }
        JS_FreeValue(ctx, proxy);
    }
    JS_FreeValue(ctx, queue);
    return hit;
}

static JSValue navigable_choose_name(JSContext *ctx, const char *name)
{
    JSValue top = window_proxy_top_of(ctx, document_window_proxy(ctx));
    JSValue hit = nav_find_in_tree(ctx, top, name);
    uint32_t i, n = 0;
    JSValue len;

    JS_FreeValue(ctx, top);
    if (!JS_IsUndefined(hit)) return hit;   /* the source's own tree first — the nearest match is the spec's */
    DCHECK(JS_IsArray(g_group), "the browsing context group's list was read before navigable_init built it");
    len = JS_GetPropertyStr(ctx, g_group, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    for (i = 0; i < n; i++) {
        JSValue other = JS_GetPropertyUint32(ctx, g_group, i);
        hit = nav_find_in_tree(ctx, other, name);
        JS_FreeValue(ctx, other);
        if (!JS_IsUndefined(hit)) return hit;
    }
    return JS_UNDEFINED;
}

/* A TOP-LEVEL TRAVERSABLE JOINS THE GROUP. A child navigable does not: it is reachable down its parent's
   document tree, and putting it here as well would make the walk visit it twice. */
static void navigable_group_add(JSContext *ctx, JSValueConst proxy)
{
    uint32_t n = 0;
    JSValue len;

    DCHECK(JS_IsArray(g_group), "a navigable was created before navigable_init built the group's list");
    len = JS_GetPropertyStr(ctx, g_group, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    JS_SetPropertyUint32(ctx, g_group, n, JS_DupValue(ctx, proxy));
}

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
        /* §7.4 CREATES THE NAVIGABLE WITH THE INITIAL about:blank, and step 14 navigates it afterwards — so
           the navigable's own address is about:blank and the DESTINATION belongs to the navigation, never to
           the creation. Recording the destination here instead was not a shortcut with the same answer: the
           realm is materialized lazily, so a read reaching through this navigable before the load job ran
           would have built the DESTINATION document early, and the job would then have found a materialized
           navigable and loaded the same address a second time into a second document — two fetches, two
           realms, one navigable, and nothing to say so. With about:blank here that early read materializes
           the document §7.4 actually created (no response, no fetch) and the job supersedes it, which is both
           what the spec describes and the only version with one load in it. */
        proxy = window_proxy_new(ctx, child, "about:blank", origin, name, feat && feat->is_popup,
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
        /* §7.1's search reaches a CHILD down its parent's document tree; a top-level traversable is reachable
           from nothing, so the group holds it — see navigable_choose_name. */
        if (!is_child) navigable_group_add(ctx, proxy);
        /* §7.4 STEP 14: NAVIGATE THE NEW NAVIGABLE TO url — and navigating is what makes the document RUN. It
           is a JOB rather than a call, for the reason js_nav_load_step states, but it is ENQUEUED here and
           unconditionally: a navigable's own scripts are an observable that owes nothing to the creator, and a
           popup posts back to its opener without the opener ever touching the proxy. Not scheduling this at
           all was twelve files in html/browsers reporting nothing — not a saving, a popup that never ran.
           A navigable with NO address enqueues NOTHING, and that is the same sentence read the other way: it
           holds the initial about:blank Document §7.4 created it with, that Document has no scripts by
           construction, and the ONLY way to observe it is a read through this proxy — which builds it then.
           The deferral has no observable there, and it is load-bearing: a forced-execution frontier runs this
           `open()` once per flow, so materializing every never-touched about:blank exhausted the heap at ~2030
           flows with the initial parse failing to allocate. The line between the two is what a navigable DOES,
           not what it costs.
           THE TEST IS "IS THERE ANYTHING TO FETCH", which for §7.4 is the `about:` scheme: about:blank has no
           response and no content, so navigating to it produces the Document the navigable already has. It is
           the same test the RealmBuilder applies to decide whether to fetch, and it is stated in both places
           because it is one spec fact about the scheme rather than a protocol between them. */
        if (strncmp(addr, "about:", 6) != 0)
            navigable_load_enqueue(ctx, proxy, addr, origin, csp);   /* §7.2.6: the CREATOR's, for §7.4's create */
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

/* §7.4 STEPS 6 AND 14, AS ONE OPERATION — choose a navigable for `target` and navigate it to `url`, creating
 * one when nothing answers to the name. It is its own function because it has TWO callers and they are not
 * variants of each other: `window.open()` reaches it after parsing a features string, and §4.6.3's FOLLOWING A
 * HYPERLINK reaches it from an `<a>`'s activation behaviour with `noopener` read off `rel` instead. The rules
 * for choosing a navigable are one algorithm; a second copy in the hyperlink path would be the second answer
 * that is always subtly wrong.
 * `feat` carries what only §7.4 supplies (the popup decision) and what both supply (`noopener`). */
JSValue navigable_open(JSContext *ctx, const char *url, const char *target, const WindowFeatures *feat)
{
    /* §7.1's FIRST rule is `noopener`, and it comes before every other: a request that must not be able to
       script its opener cannot be answered with a navigable the opener already holds, so it always CREATES. */
    if (target && *target && !(feat && feat->noopener)) {
        JSValue chosen = (target[0] == '_') ? navigable_choose_keyword(ctx, target)
                                            : navigable_choose_name(ctx, target);
        if (window_proxy_is(chosen)) {
            /* §7.4 step 14 over the chosen navigable. An absent url is the empty string, which resolves
               against the document's own address — so `open("", "_self")` reloads. */
            JSValue r = navigable_navigate(ctx, chosen, url ? url : "");
            JS_FreeValue(ctx, chosen);
            return r;
        }
        JS_FreeValue(ctx, chosen);
    }
    /* §7.1's LAST rule: create one, and GIVE it the name — unless the name was a keyword, which names no
       navigable at all. */
    return navigable_create(ctx, url, target && *target && target[0] != '_' ? target : NULL, false, feat);
}

/* §7.4's `window.open`, AS A STEP MACHINE — and it is one again for a reason that did not exist when the note
 * here said the opposite. That note read "nothing to ask and nothing to suspend for", which was true while the
 * only thing `open()` did was mint a name: §7.4 step 14 NAVIGATES, navigating FETCHES, and a fetch is a
 * host-owed answer that suspends the asking flow. A plain C body cannot suspend, so it can only reach a
 * SYNCHRONOUS fetch — which is why navigable.h's DCHECK says the shipped host, whose network is the trusted
 * zone's, cannot navigate at all. The machine is what removes that sentence.
 * IT DOES NOT PARK YET. The fetch below is still the host's synchronous one; what changed is the substrate
 * under it, so making that fetch a host request is a change to child_document and to nothing here. */
typedef struct {
    uint8_t stage;
    JSValue result;   /* the chosen navigable's proxy (owned) */
    uint8_t noopener; /* §7.4's last step needs it after the navigable is made */
} OpenState;

static void open_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    v->val(ctx, &((OpenState *)st)->result);
}

static void open_release(JSContext *ctx, void *st)
{
    OpenState *s = st;
    JS_FreeValue(ctx, s->result);
    s->result = JS_UNDEFINED;
}

static int js_win_open_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    OpenState *s = st;
    const char *url, *target, *features;
    WindowFeatures feat;

    (void)hdr; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(s->stage == 0, "window.open resumed, and it has only one stage — the machine exists for the fetch "
                          "inside step 14, which does not park yet");
    s->result = JS_UNDEFINED;
    url    = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    target = argc > 1 ? JS_ToCString(ctx, argv[1]) : NULL;
    /* §3.6.2: an OPTIONAL argument given `undefined` is ABSENT. `open(url, name, undefined)` is how the
       corpus spells "no features", and stringifying it produced the literal "undefined" — one token,
       a non-empty map, and therefore a popup where the spec has a tab. */
    features = (argc > 2 && !JS_IsUndefined(argv[2])) ? JS_ToCString(ctx, argv[2]) : NULL;
    if (argc > 0 && !url) return JS_STEP_ABRUPT;
    /* §7.4's THIRD ARGUMENT decides whether the new navigable is a POPUP — what §7.2.5.3's six BarProps answer
       from — and whether it gets an OPENER at all. */
    feat = window_features_parse(features);
    s->noopener = feat.noopener ? 1 : 0;
    s->result = navigable_open(ctx, url, target, &feat);
    if (url) JS_FreeCString(ctx, url);
    if (target) JS_FreeCString(ctx, target);
    if (features) JS_FreeCString(ctx, features);
    /* §7.4 step 4: a URL that does not parse is a SyntaxError, and it is the PAGE's mistake. */
    if (JS_IsUndefined(s->result)) {
        JS_ThrowDOMException(ctx, "SyntaxError", "the URL to open is not a URL");
        return JS_STEP_ABRUPT;
    }
    /* §7.4's LAST STEP: with `noopener`, `open()` RETURNS NULL. The navigable is created and navigated all the
       same — a page that opens a window it cannot script still opened one — and what the caller loses is the
       handle, which is the whole point of the feature. */
    if (s->noopener) { JS_FreeValue(ctx, s->result); s->result = JS_NULL; }
    *presult = s->result;
    s->result = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static const IdlStepDecl OPEN_DECL = { js_win_open_step, sizeof(OpenState), open_visit, open_release };

static int g_id_open;

/* §7.4's IDL: open(optional USVString url = "", optional DOMString target = "_blank",
   optional [LegacyNullToEmptyString] DOMString features = "") -> WindowProxy?
   DECLARED ONCE PER AGENT — a member has one pool entry, and every realm's global installs that same one. */
void navigable_init(JSContext *ctx)
{
    static const IdlArgType OPEN_ARGS[3] = { IDL_USVSTRING, IDL_DOMSTRING, IDL_DOMSTRING };

    g_id_open = idl_method_id_step(ctx, OPEN_ARGS, 3, NULL, 0, &OPEN_DECL, 0);
    idl_optional_from(0);
    /* THE BROWSING CONTEXT GROUP'S LIST, built at INIT so it belongs to the pre-boot BASELINE: a flow that
       opens a window writes into it, and the COW delta captures that write for that flow alone. An array
       allocated lazily inside a flow would be that flow's own object and no sibling would ever see it. */
    if (JS_IsUndefined(g_group)) {
        g_group = JS_NewArray(ctx);
        CHECK(!JS_IsException(g_group), "the browsing context group's list could not be allocated");
    }
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

    /* THE GROUP'S LIST BEFORE THE REALMS. It holds a reference to every top-level traversable's proxy, and a
       proxy keeps its Window; released here so the realms below are torn down with nothing left pointing in. */
    JS_FreeValue(ctx, g_group);
    g_group = JS_UNDEFINED;
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
