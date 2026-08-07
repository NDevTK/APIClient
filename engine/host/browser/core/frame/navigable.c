/* HTML §7.4 — see navigable.h. */
#include <stdio.h>
#include <stdlib.h>
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

/* §7.4 IS A SUSPEND, so it is a machine: `open()` has to hand back a WindowProxy at its own call site, and the
   child's document id can only come from the host. `req` is the outstanding question; `origin` is the child's,
   computed before the suspension because the C locals are gone when the machine resumes. */
typedef struct {
    uint32_t req;
    char    *origin;
} OpenState;

static void open_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

static void open_release(JSContext *ctx, void *st)
{
    OpenState *s = st;
    (void)ctx;
    free(s->origin);
    s->origin = NULL;
}

/* The child's origin. An `about:blank` navigable — which is what `open()` with no URL creates — inherits the
   CREATOR'S origin; that is the whole reason a same-origin popup can be scripted at all. Any other URL
   contributes its own, and a URL that does not parse is the page's mistake. */
static char *child_origin(const char *url)
{
    UrlRecord base, rec;
    const char *base_url;
    char *o = NULL;
    bool have_base;

    /* THE about:blank CASE ANSWERS WITHOUT A BASE, and must be checked FIRST. Reading the document's address
       up here evaluated it even for `open()` with no argument — the one call that needs no address at all —
       and in a host with no document installed that is an assert, not a value. It aborted sixteen spec files
       whose only sin was calling open(). */
    if (!url || !*url || !strcmp(url, "about:blank"))
        return g_origin ? strdup(g_origin) : strdup("null");
    base_url = document_base_url();
    /* RESOLVED AGAINST THE DOCUMENT'S BASE, because `open("/admin")` is how most of the real uses of this
       method are written and a relative URL has no origin of its own. */
    url_record_init(&base);
    have_base = base_url && url_parse(&base, base_url, strlen(base_url), NULL);
    url_record_init(&rec);
    if (url_parse(&rec, url, strlen(url), have_base ? &base : NULL))
        o = url_serialize_origin(&rec);
    url_record_free(&rec);
    url_record_free(&base);
    return o;
}

static int open_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                     JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    OpenState *s = st;
    JSValueConst answer;

    (void)hdr; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);

    if (s->req == 0) {
        const char *url = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
        const PolicyContainer *pc = document_policy();
        const char *csp = policy_container_csp(pc);
        char *op;
        size_t n;

        if (argc > 0 && !url) return JS_STEP_ABRUPT;
        s->origin = child_origin(url);
        if (!s->origin) {
            /* §7.4 step 4: a URL that does not parse is a SyntaxError, and it is the PAGE's mistake — thrown
               here at the call rather than carried into a request the host cannot answer. */
            if (url) JS_FreeCString(ctx, url);
            JS_ThrowDOMException(ctx, "SyntaxError", "the URL to open is not a URL");
            return JS_STEP_ABRUPT;
        }
        /* THE REQUEST, and every field of it is load-bearing. The CREATOR names who is asking (the host routes
           and, for a real browser, decides); the ORIGIN is the child's, inherited for about:blank; the POLICY
           is §7.4's CLONE OF THE CREATOR'S, serialized — which the policy container can do precisely because
           it is a flat parse over one owned string, so the clone that crosses an instance and the clone that
           crosses a session are the same operation. */
        n = strlen(url ? url : "") + strlen(s->origin) + strlen(csp ? csp : "") + 64;
        op = malloc(n);
        CHECK(op != NULL, "navigable: OOM building the create request");
        snprintf(op, n, "navigable.create\t%u\t%s\t%s\t%s",
                 world_local_doc(), url ? url : "", s->origin, csp ? csp : "");
        if (url) JS_FreeCString(ctx, url);
        s->req = engine_host_request(ctx, op);
        free(op);
        return JS_STEP_YIELD;   /* park; the flow resumes when the host has minted the child's id */
    }

    if (!engine_host_answered(s->req, &answer))
        return JS_STEP_YIELD;
    {
        JSValue got = engine_host_take(ctx, s->req);
        const char *txt = JS_ToCString(ctx, got);
        uint32_t child = txt ? (uint32_t)strtoul(txt, NULL, 10) : 0;

        if (txt) JS_FreeCString(ctx, txt);
        JS_FreeValue(ctx, got);
        s->req = 0;
        /* §7.4 RETURNS NULL WHEN THE NAVIGABLE IS NOT CREATED — a popup blocker, a sandboxed context, or a
           host that will not host another document. A page checks the result and takes a different path, so
           this is a real answer rather than a failure to report. */
        if (child == 0) { *presult = JS_NULL; return JS_STEP_DONE; }
        DCHECK(child != world_local_doc(),
               "the host answered a navigable creation with THIS document's id — the child is a new document "
               "and cannot be the one that asked for it");
        *presult = window_proxy_new_remote(ctx, child, s->origin);
    }
    return JS_STEP_DONE;
}

static const IdlStepDecl OPEN_DECL = { open_step, sizeof(OpenState), open_visit, open_release };

void navigable_install(JSContext *ctx, JSValueConst global, const char *origin)
{
    /* §7.4's IDL: open(optional USVString url = "", optional DOMString target = "_blank",
       optional [LegacyNullToEmptyString] DOMString features = "") -> WindowProxy? */
    static const IdlArgType OPEN_ARGS[3] = { IDL_USVSTRING, IDL_DOMSTRING, IDL_DOMSTRING };

    DCHECK(g_origin == NULL, "window.open was installed twice — one instance is one document");
    free(g_origin);
    g_origin = strdup(origin ? origin : "null");
    CHECK(g_origin != NULL, "navigable: OOM recording this document's origin");
    idl_install_method(ctx, global, "open", 0,
                       idl_method_id_step(ctx, OPEN_ARGS, 3, NULL, 0, &OPEN_DECL, 0));
    idl_optional_from(0);
}

void navigable_free(JSContext *ctx)
{
    (void)ctx;
    free(g_origin);
    g_origin = NULL;
}
