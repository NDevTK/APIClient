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
#include "core/dom/document.h"
#include "core/url/url.h"
#include "core/idl_args.h"
#include "solver/engine.h"
#include "solver/world.h"

static char *g_origin;   /* this document's origin — what an about:blank child inherits (owned) */

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
JSValue navigable_create(JSContext *ctx, const char *url, const char *name, bool is_child)
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
    child = world_mint_doc();
    /* THE NOTICE, and every field of it is load-bearing. The CHILD is the name the host provisions an instance
       under; the CREATOR names who made it, which is what the host routes replies through and what a browser
       would decide policy from; the URL is the child's initial address; the ORIGIN is the child's, inherited
       for about:blank; the POLICY is §7.4's CLONE OF THE CREATOR'S, serialized — which the policy container can
       do precisely because it is a flat parse over one owned string, so the clone that crosses an instance and
       the clone that crosses a session are the same operation. */
    n = strlen(world_doc_name(child)) + strlen(world_doc_name(world_local_doc())) +
        strlen(addr) + strlen(origin) + strlen(csp ? csp : "") + 32;
    op = malloc(n);
    CHECK(op != NULL, "navigable: OOM building the create notice");
    snprintf(op, n, "navigable.create\t%s\t%s\t%s\t%s\t%s", world_doc_name(child),
             world_doc_name(world_local_doc()), addr, origin, csp ? csp : "");
    engine_host_notify(ctx, op);
    free(op);
    /* WHO THE NAVIGABLE HANGS OFF, and the two shapes §7.4 distinguishes. A CHILD navigable (§4.8.5's iframe)
       is nested in this one: its `parent` is this Window and it has no opener. An AUXILIARY one (§7.4's popup)
       is a TOP-LEVEL traversable — it is its own parent and its own top — and what links it back here is
       `opener`. Getting this pair backwards is not a detail: `parent`/`top`/`opener` are how a frame and a
       popup tell each other apart, and how testharness.js finds the window it is running in. */
    {
        JSValue g = JS_GetGlobalObject(ctx);
        proxy = window_proxy_new_remote(ctx, child, origin, name,
                                        is_child ? (JSValueConst)g : JS_UNDEFINED,
                                        is_child ? JS_NULL : (JSValueConst)g);
        JS_FreeValue(ctx, g);
    }
    CHECK(!JS_IsException(proxy), "a navigable's WindowProxy could not be allocated");
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
    JSValue r;

    (void)this_val; (void)magic;
    if (argc > 0 && !url) return JS_EXCEPTION;
    /* §7.4's TARGET is the chosen browsing context NAME — except for the keywords, which name a navigable to
       reuse rather than a name to give one. A leading underscore is what tells them apart. */
    r = navigable_create(ctx, url, target && target[0] != '_' ? target : NULL, false);
    if (url) JS_FreeCString(ctx, url);
    if (target) JS_FreeCString(ctx, target);
    /* §7.4 step 4: a URL that does not parse is a SyntaxError, and it is the PAGE's mistake. */
    if (JS_IsUndefined(r))
        return JS_ThrowDOMException(ctx, "SyntaxError", "the URL to open is not a URL");
    return r;
}

void navigable_install(JSContext *ctx, JSValueConst global, const char *origin)
{
    /* §7.4's IDL: open(optional USVString url = "", optional DOMString target = "_blank",
       optional [LegacyNullToEmptyString] DOMString features = "") -> WindowProxy? */
    static const IdlArgType OPEN_ARGS[3] = { IDL_USVSTRING, IDL_DOMSTRING, IDL_DOMSTRING };

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
    idl_install_method(ctx, global, "open", 0, idl_method_id(ctx, OPEN_ARGS, 3, js_win_open, 0));
    idl_optional_from(0);
}

void navigable_free(JSContext *ctx)
{
    (void)ctx;
    free(g_origin);
    g_origin = NULL;
}
