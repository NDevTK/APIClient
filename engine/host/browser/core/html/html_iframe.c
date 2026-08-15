/* HTMLIFrameElement's NAVIGABLE — HTML §4.8.5, and the element half of the cross-document machinery.
 *
 * §4.8.5's insertion steps CREATE A CHILD NAVIGABLE when an `<iframe>` is inserted into a document, and they do
 * it THERE — synchronously, in the same turn as the append. `frame.contentWindow` answers on the very next line
 * in every browser, and the spec files say so directly: `const otherW = document.body.appendChild(frame)
 * .contentWindow` then reads `otherW.self`. It is not lazily on the first `contentWindow` either, which is
 * observably different — a page may insert a frame and read `window.length` without ever touching it.
 *
 * IT WAS A QUEUED TASK, AND THAT WAS A WORKAROUND FOR AN IDENTITY PROBLEM, NOT A SCHEDULING ONE. Creating the
 * child meant asking the host to mint its document id, asking suspends, and the insertion-steps drain may not
 * suspend — so the work was pushed into a task that could. The fix is not a better place to suspend: it is not
 * to suspend. A document is NAMED and a child's name is minted locally (world.h), so creation is a mint, a
 * notice and an object allocation, none of which can block. The task is gone and so is the ordering lie its
 * comment recorded.
 *
 * THE NAVIGABLE IS PER-FLOW, and it is kept on the ELEMENT'S WRAPPER rather than in a table beside it. A flow
 * that inserted the frame has one; a sibling that never did must not see it, and a C-side registry would show
 * it to both — silently, because a proxy that exists in the wrong world answers reads perfectly well. A hidden
 * own slot on the wrapper is an ordinary property write on a baseline object, so the heap COW delta isolates it
 * with nothing added here; a forked arm inherits its parent's through the delta and issues no second create,
 * which is also what keeps one child document per WORLD rather than one per flow. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "check.h"
#include "quickjs.h"
#include "core/html/html_iframe.h"
#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/dom/document.h"
#include "core/frame/document_lifecycle.h"
#include "core/frame/navigable.h"
#include "core/frame/window_proxy.h"
#include "solver/engine.h"
#include "solver/flow.h"
#include "solver/world.h"
#include "core/idl_args.h"

/* The wrapper slot the navigable lives in. Not a name a page would write, and read through JS_GetOwnSlot so no
   prototype lookup and no page code can intercept it — the same arrangement the custom-element upgrade mark
   uses, and for the same reason. */
static JSAtom g_atom_navigable = JS_ATOM_NULL;

/* This element's navigable IN THIS FLOW, or JS_UNDEFINED. */
JSValue iframe_navigable(JSContext *ctx, JSValueConst wrap)
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

void iframe_create_navigable(JSContext *ctx, JSValueConst wrap)
{
    char *src, *name;
    JSValue proxy;

    DCHECK(g_atom_navigable != JS_ATOM_NULL, "an iframe's navigable was created before iframe_init ran");
    DCHECK(JS_IsObject(wrap), "something that is not an element wrapper was given a child navigable");
    if (iframe_has_navigable(ctx, wrap)) return;   /* this flow already has one */

    /* §4.8.5's "process the iframe attributes": `src` is the child's initial address, and an absent or empty
       one is the initial about:blank Document. `name` becomes the BROWSING CONTEXT's name — which is why
       renaming the window later leaves the attribute alone, and why removing the frame empties the name while
       the attribute keeps its value. Both are read through the DOM chokepoint so the read stays in the running
       flow's delta — a flow that set `src` and a sibling that did not create different children, which is the
       whole reason the navigable is per-flow. */
    src  = element_attr_get(ctx, wrap, "src");
    name = element_attr_get(ctx, wrap, "name");
    proxy = navigable_create(ctx, src, name, true, NULL);
    /* §4.8.5 has no "did not parse" branch the way §7.4 does: an `<iframe src="::">` still has a navigable,
       holding the initial about:blank it was created with. */
    if (JS_IsUndefined(proxy)) proxy = navigable_create(ctx, NULL, name, true, NULL);
    free(src);
    free(name);
    DCHECK(!JS_IsUndefined(proxy), "an about:blank child navigable could not be created, which has no failing "
                                   "branch — its address needs no parse and its origin is this document's");
    /* WRITABLE, NOT CONFIGURABLE. The removing steps below CLEAR this slot, and a slot defined with no flags
       at all can be neither rewritten nor deleted — the destroy silently did nothing and a removed frame kept
       answering `contentWindow`. Non-configurable is deliberate: a page that guessed the name still cannot
       delete the navigable out from under the element. */
    JS_DefinePropertyValue(ctx, (JSValue)wrap, g_atom_navigable, proxy, JS_PROP_WRITABLE);
}

/* §4.8.5's REMOVING STEPS: an <iframe> that leaves a document runs §7.3.1's DESTROY A CHILD NAVIGABLE over the
   navigable it contained. The container's half is SYNCHRONOUS and the document's half is a JOB, which is the
   spec's own split and is why this used to be wrong: the element loses its navigable on this line (step 3, so
   `contentWindow` is null from here on), while step 5's destruction of the active document and everything under
   it is queued — it disentangles that document's ports, drops its queued tasks and only then nulls its browsing
   context, none of which can happen inside a tree mutation.
   WHAT WAS HERE INSTEAD WAS ONE BYTE. Setting `closed` on the proxy announced a destruction that had not
   happened and never would: the child's Document, its Window, its realm, its queued tasks and its whole
   subtree were left exactly as they were, and the announcement is what made that invisible. The proxy a page
   is still holding does stay the same object and does end up reporting `closed` — a WindowProxy outlives the
   navigable it named — but it reports it because the destruction ran, not instead of it. */
void iframe_destroy_navigable(JSContext *ctx, JSValueConst wrap)
{
    JSValue proxy = iframe_navigable(ctx, wrap);

    if (JS_IsUndefined(proxy)) return;   /* this flow never had one — §7.3.1 step 2 */
    document_lifecycle_destroy_child(ctx, proxy);   /* §7.3.1 steps 4-5 */
    JS_FreeValue(ctx, proxy);
    /* CLEARED, not deleted: the slot is non-configurable so it cannot be deleted, and it does not need to be —
       an empty slot is what "this element has no navigable" means everywhere it is read. It is an ordinary
       property write on the wrapper, so the heap COW delta isolates it: a sibling arm that never removed the
       element still sees the frame it knew. */
    JS_DefinePropertyValue(ctx, (JSValue)wrap, g_atom_navigable, JS_UNDEFINED, JS_PROP_WRITABLE);
}

/* §4.8.5 FOR THE ELEMENTS THE PARSER INSERTED. A browser runs the insertion steps during tree construction, so
 * an `<iframe>` in the page's own markup has a navigable before the first script runs — `window.length` is 1 on
 * a document that never scripted anything. This engine's tree comes from a Lexbor parse that does not pass
 * through the DOM chokepoint, so the parsed tree's iframes get their step here, once, when the document is
 * installed. Everything a script appends afterwards goes through the chokepoint and needs nothing from this. */
void iframe_document_parsed(JSContext *ctx)
{
    lxb_dom_node_t *root = document_root_node(ctx), *n = root;

    while (n) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t qn = 0;
            const lxb_char_t *q = lxb_dom_element_qualified_name(lxb_dom_interface_element(n), &qn);
            if (q && qn == 6 && !strncasecmp((const char *)q, "iframe", 6)) {
                JSValue w = node_wrap(ctx, n);
                iframe_create_navigable(ctx, w);
                JS_FreeValue(ctx, w);
            }
        }
        if (n->first_child) { n = n->first_child; continue; }
        while (n && !n->next) n = (n == root) ? NULL : n->parent;
        n = n ? n->next : NULL;
    }
}

/* §7.2.5's DOCUMENT-TREE CHILD NAVIGABLES, in TREE ORDER — the set `window.length` counts and `window[i]`
 * indexes. It is a WALK, never a counter kept beside the tree: the set changes on every insertion, every
 * removal and every reparent, and a page reads `length` after doing all three. A count that a mutation forgot
 * to adjust is wrong in exactly the case the spec files test.
 *
 * IT IS PER-FLOW TWICE OVER, and both halves come for free. The TREE is per-flow (the DOM COW delta), so a
 * flow that appended a frame walks a document containing it and its sibling does not; and the NAVIGABLE is
 * per-flow (the wrapper slot), so an element that is in both flows' trees still only counts for the flow that
 * gave it one. That is why this asks iframe_has_navigable rather than counting <iframe> ELEMENTS: an element
 * whose navigable was destroyed is still in the tree until it is removed, and it is not a child navigable.
 *
 * `want` < 0 counts them all and returns JS_UNDEFINED; otherwise the nth is returned, or JS_UNDEFINED. */
static JSValue child_navigables(JSContext *ctx, int want, int *out_n)
{
    lxb_dom_node_t *root = document_root_node(ctx);
    lxb_dom_node_t *n = root;
    int seen = 0;

    if (out_n) *out_n = 0;
    while (n) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t qn = 0;
            const lxb_char_t *q = lxb_dom_element_qualified_name(lxb_dom_interface_element(n), &qn);
            if (q && qn == 6 && !strncasecmp((const char *)q, "iframe", 6)) {
                JSValue w = node_wrap(ctx, n);
                JSValue nav = iframe_navigable(ctx, w);
                JS_FreeValue(ctx, w);
                if (!JS_IsUndefined(nav)) {
                    if (want == seen) { if (out_n) *out_n = seen + 1; return nav; }
                    seen++;
                }
                JS_FreeValue(ctx, nav);
            }
        }
        if (n->first_child) { n = n->first_child; continue; }
        while (n && !n->next) n = (n == root) ? NULL : n->parent;
        n = n ? n->next : NULL;
    }
    if (out_n) *out_n = seen;
    return JS_UNDEFINED;
}

int iframe_child_navigable_count(JSContext *ctx)
{
    int n = 0;
    JSValue v = child_navigables(ctx, -1, &n);
    JS_FreeValue(ctx, v);
    return n;
}

JSValue iframe_child_navigable(JSContext *ctx, int index)
{
    return index < 0 ? JS_UNDEFINED : child_navigables(ctx, index, NULL);
}

/* §4.8.5 `contentDocument`, and the read that made the cross-agent reference necessary. It is the child
 * navigable's ACTIVE DOCUMENT, filtered by §7.2.5.1's same-origin check — which is the whole of the spec's
 * text, because a browser has the two documents in one agent cluster and the answer is a pointer.
 *
 * HERE IT IS IN ANOTHER INSTANCE, so the flow SUSPENDS: the peer exports its `document` and answers with the
 * NAME of it, and this side resolves that name to the one reference for it. `contentDocument` is therefore a
 * step machine where `contentWindow` is a plain accessor — a WindowProxy names a NAVIGABLE, which this agent
 * created and knows, and a Document is the peer's object. */
/* WHERE THIS MACHINE RESTS, AS THE STANDARD NUMBERS IT. §4.8.5's getter is one sentence over §7.3.1's
   `content document`, whose four steps are: no content navigable → null; its active document; §7.2.5.1's same
   origin-domain filter; return it. Steps 1-3 are one stage — they are decided here, and no page code runs
   between them. Step 4 is its OWN stage whenever the document lives in another instance, because that answer
   arrives from a peer's scheduled turn and this flow is parked until it does; the resume point was a request
   handle being non-zero, which is a stage nothing could name.
   Same-origin in THIS agent the two documents are one heap, so step 4 is answered in the same turn and the
   machine never rests at the second stage — a suspend there would be observable, which makes it a fidelity bug
   rather than extra rigor. */
#define CONTENT_DOC_STAGES(X) \
    X(CONTENTDOC_RESOLVE, "HTML §4.8.5 contentDocument → §7.3.1 content document steps 1-3 (the content " \
                          "navigable's active document, filtered by §7.2.5.1's same origin-domain check)") \
    X(CONTENTDOC_ANSWER,  "HTML §7.3.1 content document step 4 (the answer, from the instance that holds the " \
                          "document)")
enum { IDL_STEP_STAGE_BASE(CONTENT_DOC_STAGES) CONTENT_DOC_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const CONTENT_DOC_STEPS[] = { CONTENT_DOC_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct { uint32_t req; } ContentDocState;

static void iframe_cd_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

static int iframe_content_document_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                        JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    ContentDocState *s = st;
    JSValueConst answer;

    (void)argc; (void)argv; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);

    if (hdr->stage == CONTENTDOC_RESOLVE) {
        JSValue nav = iframe_navigable(ctx, hdr->this_val);
        Flow *f = flow_running();
        char op[1024];
        int n;

        /* §4.8.5: no navigable, no document. */
        if (JS_IsUndefined(nav)) { *presult = JS_NULL; return JS_STEP_DONE; }
        /* §7.3.1's content document step 3 filters BEFORE asking: a cross-origin child's document is null, and
           asking the peer for it would both leak and suspend a flow on a question whose answer is already
           known. THE FILTER IS SAME ORIGIN-DOMAIN, which is §7.1.1's other algorithm and the one this step
           names — it is the same answer as same origin until `document.domain` has been set, and asking the
           one the standard names is what keeps that member implementable in one place. */
        if (!window_proxy_same_origin_domain_of(nav)) {
            JS_FreeValue(ctx, nav);
            *presult = JS_NULL;
            return JS_STEP_DONE;
        }
        /* SAME-ORIGIN AND IN THIS AGENT: one heap, so the answer is the child realm's own `document` object and
           it is handed back in THIS turn. §4.8.5 is synchronous here — `frame.contentDocument.body` reads on
           the line after the append — so a suspend would be observable, which makes it a fidelity bug rather
           than extra rigor. It is also what the corpus does with the result: window_length.html appends a node
           THIS document created into that body, and afterwards `subframe.parentNode` is a node of the other
           document while the wrapper stays the same object. There is no message that carries that. */
        if (!window_proxy_is_remote(nav)) {
            JSContext *cctx = window_proxy_realm(ctx, nav);
            *presult = JS_DupValue(ctx, document_object(cctx));
            JS_FreeValue(ctx, nav);
            return JS_STEP_DONE;
        }
        DCHECK(f != NULL, "a cross-document read was issued outside a flow — there would be nothing to suspend");
        /* THE WORLD VECTOR IS world_serialize'S AND NOBODY ELSE'S (solver/world.h), and this site was the
           second spelling of it — the head written by hand, then a hand-rolled loop over world_ancestry. Two
           spellings are two peers materializing different segments for one flow, which is the reason that
           function exists; and this one ALSO had the exact failure its CHECK is there to prevent, twice over.
           `snprintf` returns the length it WOULD have written, so once the record filled `op` the accumulated
           `n` ran PAST `sizeof op` and `sizeof op - (size_t)n` UNDERFLOWED to a huge size_t — a write past the
           end of a stack buffer, reached by nothing louder than a deep enough frame tree or a long enough fork
           chain. The quiet half is the loop guard `n < (int)sizeof op`, which permitted a truncated ancestry
           to be SENT: a prefix makes the peer fork a more distant ancestor than the sender named and silently
           lose every write in between, which is world.h's stated reason for crashing rather than sending one. */
        n = snprintf(op, sizeof op, "windowproxy.get\t%s\t", world_doc_name(window_proxy_doc(nav)));
        CHECK(n > 0 && (size_t)n < sizeof op,
              "a cross-document read's target document name did not fit its record — a truncated name reaches "
              "no instance, and the asking flow parks on a question nothing will ever be asked");
        n += world_serialize(f->world, op + n, sizeof op - (size_t)n);
        n += snprintf(op + n, sizeof op - (size_t)n, "\tdocument");
        CHECK((size_t)n < sizeof op,
              "a cross-document read's member name did not fit its record — the peer would run a program for a "
              "TRUNCATED member, answering a different question as if it were this one");
        JS_FreeValue(ctx, nav);
        s->req = engine_host_request(ctx, op);
        hdr->stage = CONTENTDOC_ANSWER;
        return JS_STEP_YIELD;
    }
    DCHECK(hdr->stage == CONTENTDOC_ANSWER, "contentDocument resumed into a stage §7.3.1 does not have");
    DCHECK(s->req != 0, "contentDocument is parked on step 4 with no request outstanding — the stage says a "
                        "peer was asked and nothing was");
    if (!engine_host_answered(s->req, &answer))
        return JS_STEP_YIELD;
    /* The peer answered with a COMPLETION: §7.3.1's read runs the peer's own program, and a throw from it is
       raised here, at the `iframe.contentDocument` that parked. */
    {
        int r = engine_host_take_completion(ctx, s->req, presult);
        s->req = 0;
        return r;
    }
}

static const IdlStepDecl CONTENT_DOC_DECL = { iframe_content_document_step, sizeof(ContentDocState),
                                              iframe_cd_visit, NULL,
                                              "HTML §4.8.5 HTMLIFrameElement.contentDocument "
                                              "(over §7.3.1's content document)", CONTENT_DOC_STEPS };

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
    DCHECK(g_atom_navigable == JS_ATOM_NULL, "iframe_init ran twice — one instance is one document");
    g_atom_navigable = JS_NewAtom(ctx, "apiclientNavigable");
    CHECK(g_atom_navigable != JS_ATOM_NULL, "the iframe navigable slot could not be interned");
}

/* Declared once per AGENT, installed per realm — see hyperlink.c for why the split exists at all. */
static int g_content_doc_id = -1;

void iframe_declare(JSContext *ctx)
{
    g_content_doc_id = idl_getter_id_step(ctx, &CONTENT_DOC_DECL, 0);
}

void iframe_install(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_content_doc_id >= 0, "§4.8.5's members were installed before they were declared");
    idl_install_accessor(ctx, proto, "contentWindow", js_iframe_content_window, 0, -1);
    idl_install_accessor_step(ctx, proto, "contentDocument", g_content_doc_id, -1);
}

void iframe_free(JSContext *ctx)
{
    (void)ctx;
    JS_FreeAtom(ctx, g_atom_navigable);
    g_atom_navigable = JS_ATOM_NULL;
}
