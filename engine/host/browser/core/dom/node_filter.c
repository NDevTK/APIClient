/* NODEFILTER — DOM §6.3's constants and §6.4's "filter a node". See node_filter.h for why the shared half of
 * §6 is its own component. */
#include <stddef.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/dom/node.h"
#include "core/dom/node_filter.h"
#include "core/idl_args.h"

/* `acceptNode`, interned once for the agent. §6.4 step 6 reads it off a callback-interface object that is not
   itself callable, which is a property lookup and therefore one accessor away from the page's code. */
static JSAtom g_accept_node = JS_ATOM_NULL;

void node_filter_init(JSContext *ctx)
{
    if (g_accept_node != JS_ATOM_NULL) return;   /* one agent, one atom */
    g_accept_node = JS_NewAtom(ctx, "acceptNode");
    CHECK(g_accept_node != JS_ATOM_NULL, "the acceptNode atom could not be interned");
}

void node_filter_free(JSRuntime *rt)
{
    if (g_accept_node == JS_ATOM_NULL) return;
    JS_FreeAtomRT(rt, g_accept_node);
    g_accept_node = JS_ATOM_NULL;
}

void traverser_release(JSRuntime *rt, Traverser *t)
{
    JS_FreeValueRT(rt, t->root);
    JS_FreeValueRT(rt, t->filter);
    t->root = JS_UNDEFINED;
    t->filter = JS_UNDEFINED;
}

void traverser_mark(JSRuntime *rt, Traverser *t, JS_MarkFunc *mark_func)
{
    /* A FILTER IS A PAGE CLOSURE AND THE ROOT IS A NODE WRAPPER, so a traverser reachable only from a filter
       that captured it is a cycle — `document.createTreeWalker(root, 0, function f(){ return w })`. Without
       this the collector cannot see through the record and the runtime's own leak walk reports the whole
       document. */
    JS_MarkValue(rt, t->root, mark_func);
    JS_MarkValue(rt, t->filter, mark_func);
}

void node_filter_call_visit(JSContext *ctx, NodeFilterCall *c, JSStepVisit *v)
{
    int k;
    v->val(ctx, &c->owner);
    v->val(ctx, &c->res);
    for (k = 0; k < 3; k++)
        v->val(ctx, &c->cb[k]);
}

void node_filter_call_unlock(NodeFilterCall *c)
{
    /* §6.4 STEPS 6-7, BOTH OF THEM. This runs on the normal exit (where the flag is already down) and on the
       abrupt one (where the driver tore the machine down without ever re-entering the algorithm), so the
       standard's "set is active to false and rethrow" has exactly one implementation.
       IT IS ITS OWN FUNCTION because a traverser's flag and a phase cursor are not references, so no `visit`
       can name them — and everything that IS a reference here is named by node_filter_call_visit, which is the
       one list a holding machine's teardown discharges. */
    if (c->t) { c->t->active = 0; c->t = NULL; }
    c->phase = 0;
    c->cphase = 0;
}

void node_filter_call_release(JSContext *ctx, NodeFilterCall *c)
{
    node_filter_call_unlock(c);
    node_filter_call_visit(ctx, c, JS_StepFreeVisitor());
}

/* §6.4's abrupt exit. The flag comes down inside the release, which is also where the driver's own teardown
   reaches it — one implementation, so a new exit cannot forget it. */
static int filter_abrupt(JSContext *ctx, Traverser *t, NodeFilterCall *c)
{
    t->active = 0;
    node_filter_call_release(ctx, c);
    return -1;
}

int node_filter_run(JSContext *ctx, JSStepHdr *hdr, JSValueConst owner, Traverser *t, NodeFilterCall *c,
                    JSValueConst node, JSValue in, int *pres, JSValue **out_cb, int *out_argc)
{
    /* THE CALLEE AND ITS RECEIVER ARE LOCALS, deliberately: §6.4 reaches the call in the same entry that read
       the function, and step_call_run holds both across the suspension itself (it dups them into `cb`). A field
       for them would be a second owner of the same reference with nothing to keep the two in step. */
    JSValue fn = JS_UNDEFINED, this_arg = JS_UNDEFINED;
    double num = 0;
    int r;

    DCHECK(g_accept_node != JS_ATOM_NULL, "§6.4's filter ran before node_filter_init interned acceptNode");

    if (c->phase == 0) {
        lxb_dom_node_t *n = node_of(node);
        int bit;

        JS_FreeValue(ctx, in);
        /* STEP 1. A filter that calls back into the traverser it is filtering for. */
        if (t->active) {
            JS_ThrowDOMException(ctx, "InvalidStateError",
                                 "the NodeFilter is already running for this traverser");
            return filter_abrupt(ctx, t, c);
        }
        /* STEPS 2-3. `n` is nodeType − 1, and whatToShow's bit `n` decides. A node type this engine has no
           wrapper for cannot reach here — every caller walks the tree it was handed. */
        DCHECK(n != NULL, "§6.4's filter was handed something that is not a node");
        bit = (int)n->type - 1;
        DCHECK(bit >= 0 && bit < 32, "a node reported a type outside §4.4's twelve");
        if (!(t->what & (uint32_t)(1u << bit))) {
            *pres = NODE_FILTER_SKIP;
            return 0;
        }
        /* STEP 4. */
        if (JS_IsNull(t->filter) || JS_IsUndefined(t->filter)) {
            *pres = NODE_FILTER_ACCEPT;
            return 0;
        }
        /* STEP 5. Set before anything that can suspend, because that is what the guard is FOR — and recorded
           on the call, so the teardown can bring it down again when the page's filter throws. */
        t->active = 1;
        c->t = t;
        c->owner = JS_DupValue(ctx, owner);
        /* STEP 6 / Web IDL "call a user object's operation": a CALLABLE filter is the operation itself and its
           `this` is undefined; anything else is a callback-interface object whose `acceptNode` is read and
           called with the object as its receiver. */
        if (JS_IsFunction(ctx, t->filter)) {
            fn = JS_DupValue(ctx, t->filter);
            this_arg = JS_UNDEFINED;
            c->phase = 2;
        } else {
            c->phase = 1;
        }
        in = JS_UNDEFINED;
    }

    if (c->phase == 1) {
        /* Web IDL step 10.1: Get(O, "acceptNode") — the page's getter or Proxy trap. */
        r = step_getprop_run(ctx, hdr, t->filter, g_accept_node, in, &fn, out_cb, out_argc);
        if (r > 0) return r;                       /* parked ON THE READ; the resume comes back here */
        if (r < 0) return filter_abrupt(ctx, t, c);
        /* Web IDL step 10.4: a non-callable `acceptNode` is a TypeError, and step 10.5 makes the OBJECT the
           receiver. */
        if (!JS_IsFunction(ctx, fn)) {
            JS_FreeValue(ctx, fn);
            JS_ThrowTypeError(ctx, "the NodeFilter's acceptNode is not callable");
            return filter_abrupt(ctx, t, c);
        }
        this_arg = t->filter;
        c->phase = 2;
        in = JS_UNDEFINED;
    }

    if (c->phase == 2) {
        /* Web IDL step 12: Call(X, thisArg, « node »). A CALL REQUEST, so the filter is ordinary preemptible
           page code and the walk that asked for it parks at the step it is on. */
        r = step_call_run(ctx, &c->cphase, STEP_CB(c->cb), fn, this_arg, 1, &node, in, &c->res,
                          out_cb, out_argc);
        JS_FreeValue(ctx, fn);
        fn = JS_UNDEFINED;
        if (r > 0) return r;                       /* parked INSIDE the page's filter */
        if (r < 0) return filter_abrupt(ctx, t, c);
        c->phase = 3;
        in = JS_UNDEFINED;
    }

    DCHECK(c->phase == 3, "§6.4's filter resumed in a phase it never parks in");
    /* Web IDL step 14: convert the result to the operation's return type, `unsigned short` — ToNumber (the
       page's `valueOf`) and then §3.2's modulo. A filter answering 65537 accepts exactly as one answering 1. */
    r = step_todouble_run(ctx, hdr, c->res, in, &num, out_cb, out_argc);
    if (r > 0) return r;                           /* parked ON THE CONVERSION */
    if (r < 0) return filter_abrupt(ctx, t, c);
    *pres = (int)idl_integer_of(IDL_UNSIGNED_SHORT, num);
    /* STEP 7. */
    t->active = 0;
    node_filter_call_release(ctx, c);
    return 0;
}

/* §6.3's CONSTANTS. Web IDL §3.7.2: a callback interface with constants has a callback interface object on the
   global carrying them, non-writable, non-enumerable and non-configurable — which is what the corpus reads. */
static const JSCFunctionListEntry js_node_filter_consts[] = {
    JS_PROP_INT32_DEF("FILTER_ACCEPT", NODE_FILTER_ACCEPT, 0),
    JS_PROP_INT32_DEF("FILTER_REJECT", NODE_FILTER_REJECT, 0),
    JS_PROP_INT32_DEF("FILTER_SKIP",   NODE_FILTER_SKIP,   0),
    /* SHOW_ALL is 0xFFFFFFFF, which is not an int32 — it is an `unsigned long` constant and the page compares
       it against a number. JS_PROP_INT64_DEF is what carries it without wrapping to −1. */
    JS_PROP_INT64_DEF("SHOW_ALL",                    0xFFFFFFFF, 0),
    JS_PROP_INT32_DEF("SHOW_ELEMENT",                0x001, 0),
    JS_PROP_INT32_DEF("SHOW_ATTRIBUTE",              0x002, 0),
    JS_PROP_INT32_DEF("SHOW_TEXT",                   0x004, 0),
    JS_PROP_INT32_DEF("SHOW_CDATA_SECTION",          0x008, 0),
    /* The three §6.3 marks "legacy" are still in the IDL and still on the object: a page that ORs them into a
       whatToShow gets a number, not a TypeError, and the corpus asserts each one's value. */
    JS_PROP_INT32_DEF("SHOW_ENTITY_REFERENCE",       0x010, 0),
    JS_PROP_INT32_DEF("SHOW_ENTITY",                 0x020, 0),
    JS_PROP_INT32_DEF("SHOW_PROCESSING_INSTRUCTION", 0x040, 0),
    JS_PROP_INT32_DEF("SHOW_COMMENT",                0x080, 0),
    JS_PROP_INT32_DEF("SHOW_DOCUMENT",               0x100, 0),
    JS_PROP_INT32_DEF("SHOW_DOCUMENT_TYPE",          0x200, 0),
    JS_PROP_INT32_DEF("SHOW_DOCUMENT_FRAGMENT",      0x400, 0),
    JS_PROP_INT32_DEF("SHOW_NOTATION",               0x800, 0),
};

void node_filter_install(JSContext *ctx, JSValueConst global)
{
    JSValue obj = JS_NewObject(ctx);

    CHECK(!JS_IsException(obj), "the NodeFilter interface object could not be allocated");
    JS_SetPropertyFunctionList(ctx, obj, js_node_filter_consts,
                               (int)(sizeof(js_node_filter_consts) / sizeof(js_node_filter_consts[0])));
    JS_SetPropertyStr(ctx, (JSValue)global, "NodeFilter", obj);
}
