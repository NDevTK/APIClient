/* HTMLIFrameElement's NAVIGABLE — HTML §4.8.5, and the element half of the cross-document machinery.
 *
 * §4.8.5's insertion steps CREATE A CHILD NAVIGABLE when an `<iframe>` is inserted into a document. That is
 * where a browser makes the child and it is where this makes it too — not lazily on the first `contentWindow`,
 * which is observably different: a page may insert a frame and read `window.length` without ever touching it.
 *
 * IT IS ENQUEUED, NOT DONE IN THE DRAIN. One instance is one document, so the child is a peer and only the host
 * can mint its id — which means creating it SUSPENDS. The insertion-steps drain must not: element.c's buffer is
 * a raw per-machine allocation with no clone contract and no way to park, and its own comment states the
 * invariant that lets it be one, "every per-node effect is an enqueue". So the insertion step enqueues THIS
 * machine as a task, and the task is an ordinary flow — preemptible, forkable, parkable — which may suspend for
 * as long as the host takes. The same shape custom-element reactions use, for the same reason.
 *
 * THE NAVIGABLE IS PER-FLOW, and it is kept on the ELEMENT'S WRAPPER rather than in a table beside it. A flow
 * that inserted the frame has one; a sibling that never did must not see it, and a C-side registry would show
 * it to both — silently, because a proxy that exists in the wrong world answers reads perfectly well. A hidden
 * own slot on the wrapper is an ordinary property write on a baseline object, so the heap COW delta isolates it
 * with nothing added here; a forked arm inherits its parent's through the delta and issues no second request,
 * which is also what keeps one create per WORLD rather than one per flow.
 *
 * WHEN IT IS VISIBLE. The creation is a TASK, so a page sees `contentWindow` on the next task — not in a
 * microtask continuation, which §8.1.7 runs BEFORE the next task. That is a browser's ordering, not a
 * limitation of this: `.then` after an append observes no navigable in Chrome either. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "check.h"
#include "quickjs.h"
#include "core/html/html_iframe.h"
#include "core/dom/element.h"
#include "core/dom/document.h"
#include "core/frame/window_proxy.h"
#include "core/frame/policy_container.h"
#include "core/idl_args.h"
#include "solver/engine.h"
#include "solver/world.h"

/* The wrapper slot the navigable lives in. Not a name a page would write, and read through JS_GetOwnSlot so no
   prototype lookup and no page code can intercept it — the same arrangement the custom-element upgrade mark
   uses, and for the same reason. */
static JSAtom g_atom_navigable = JS_ATOM_NULL;
static int    g_create_stepid = -1;
static JSValue g_create_fn = JS_UNDEFINED;

/* This element's navigable IN THIS FLOW, or JS_UNDEFINED. */
static JSValue iframe_navigable(JSContext *ctx, JSValueConst wrap)
{
    JSValue v;
    if (JS_GetOwnSlot(ctx, &v, wrap, g_atom_navigable) <= 0) return JS_UNDEFINED;
    return v;
}

bool iframe_has_navigable(JSContext *ctx, JSValueConst wrap)
{
    JSValue v = iframe_navigable(ctx, wrap);
    bool had = !JS_IsUndefined(v);
    JS_FreeValue(ctx, v);
    return had;
}

/* THE CREATION, as a queued step machine. Its one argument is the iframe's wrapper, which is how the element
   reaches a task: a Lexbor pointer is not a JSValue and cannot be enqueued, and the wrapper is the identity the
   rest of the engine already uses for that node — and the thing the navigable hangs off. */
typedef struct {
    JSStepHdr hdr;    /* FIRST — the driver writes the def and the operand bounds through it */
    uint32_t  req;    /* the outstanding create request; 0 = none in flight */
} CreateState;

static void create_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

/* REQUIRED, not optional: the driver calls fini through the def to release what the state holds and to yield
   the machine's result. A NULL here is a call through a null pointer at teardown. This machine owns nothing
   beyond its header — the wrapper is the driver's captured argument — and a task's result is discarded. */
static JSValue iframe_create_fini(JSContext *ctx, void *st, bool take_result)
{
    (void)ctx; (void)st; (void)take_result;
    return JS_UNDEFINED;
}

/* A RAW JSTrampStepDef's step takes the STATE and finds everything else through the header the state begins
   with — an IdlStepBody's wider signature is the args machine's own, and using it here hands the driver's
   operand pointers to the wrong parameters. */
static int iframe_create_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    CreateState *s = st;
    JSValueConst wrap = step_arg(&s->hdr, 0);   /* the invocation is on the header; a resumed machine has no stack */
    JSValueConst answer;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);

    if (!JS_IsObject(wrap) || iframe_has_navigable(ctx, wrap))
        return JS_STEP_DONE;   /* gone, or this flow already has one */

    if (s->req == 0) {
        /* §4.8.5's create-a-child-navigable. The CREATOR names who is asking; the child's initial document is
           about:blank so it inherits this document's address; and the POLICY is §7.4's clone of the creator's,
           serialized because the clone crosses an instance. The same request `window.open` issues, because it
           is the same operation on the same seam. */
        const char *csp = policy_container_csp(document_policy());
        const char *base = document_base_url();
        size_t n = strlen(csp ? csp : "") + strlen(base ? base : "") + 64;
        char *op = malloc(n);

        CHECK(op != NULL, "iframe: OOM building the create request");
        snprintf(op, n, "navigable.create\t%u\t\t%s\t%s", world_local_doc(), base ? base : "", csp ? csp : "");
        s->req = engine_host_request(ctx, op);
        free(op);
        return JS_STEP_YIELD;   /* park: this is a flow of its own, so suspending costs nothing else */
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
        /* §4.8.5: NO NAVIGABLE is a real answer — `contentWindow` is then null, which is what a host that
           cannot host another document truthfully reports. */
        if (child != 0) {
            JSValue proxy = window_proxy_new_remote(ctx, child, document_base_url());
            CHECK(!JS_IsException(proxy), "an iframe's WindowProxy could not be allocated");
            JS_DefinePropertyValue(ctx, (JSValue)wrap, g_atom_navigable, proxy, 0);
        }
    }
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_iframe_create_def = {
    sizeof(CreateState), iframe_create_step, iframe_create_fini, 0, .visit = create_visit
};

void iframe_queue_create(JSContext *ctx, JSValueConst wrapper)
{
    DCHECK(JS_IsObject(g_create_fn), "an iframe's navigable was queued before iframe_init ran");
    JS_EnqueueCallTask(ctx, g_create_fn, 1, &wrapper);
}

/* §4.8.5 `contentWindow`: this flow's child navigable, or null when there is none. Reading THROUGH it is what
   suspends; this read does not, because the proxy is a local object naming a remote document. */
static JSValue js_iframe_content_window(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue v = iframe_navigable(ctx, this_val);
    (void)magic;
    if (JS_IsUndefined(v)) return JS_NULL;
    return v;
}

void iframe_init(JSContext *ctx)
{
    DCHECK(g_create_stepid < 0, "iframe_init ran twice — one instance is one document");
    g_atom_navigable = JS_NewAtom(ctx, "apiclientNavigable");
    CHECK(g_atom_navigable != JS_ATOM_NULL, "the iframe navigable slot could not be interned");
    g_create_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_iframe_create_def);
    /* A step function object nobody installs, so a page can neither see it nor replace it — the same reason
       the custom-element reaction driver is on no prototype. */
    g_create_fn = JS_NewCFunction2(ctx, NULL, "createChildNavigable", 1, JS_CFUNC_step, g_create_stepid);
    CHECK(!JS_IsException(g_create_fn), "the child-navigable driver could not be allocated");
}

void iframe_install(JSContext *ctx, JSValueConst proto)
{
    idl_install_accessor(ctx, proto, "contentWindow", js_iframe_content_window, 0, -1);
}

void iframe_free(JSContext *ctx)
{
    JS_FreeValue(ctx, g_create_fn);
    g_create_fn = JS_UNDEFINED;
    JS_FreeAtom(ctx, g_atom_navigable);
    g_atom_navigable = JS_ATOM_NULL;
    g_create_stepid = -1;
}
