/* CUSTOM ELEMENTS — HTML §4.13.
 *
 * WHY THIS MATTERS TO A SOLVER, and why this project's rules name it beside forms: a custom element's code runs
 * ONLY through its lifecycle. A bundle that defines `<app-router>` puts its routing — and the fetches behind it
 * — inside connectedCallback, and nothing else in the program calls that function. Without the upgrade, that
 * code is shipped, reachable, and never executed, which is precisely the surface this engine exists to reach.
 * The rules say it directly: custom elements are learned by EXECUTION, through connectedCallback.
 *
 * THE UPGRADE IS A PROTOTYPE SWAP, and that is not a shortcut — it is what §4.13.3 observably does. An element
 * parsed or created before its definition exists is an ordinary HTMLElement; defining the name later makes the
 * SAME node an instance of the class, with the class's methods on it. This engine has one wrapper per node with
 * a prototype chosen by tag, so upgrading is re-pointing that wrapper's prototype at the definition's — the
 * node's identity survives, which is what makes `el === document.querySelector('app-router')` still true after
 * the upgrade.
 *
 * A REACTION IS ENQUEUED, NEVER CALLED. connectedCallback is the page's code with loops and awaits in it, and
 * the insertion that triggers it happens inside appendChild — a plain C body that cannot park. So the reaction
 * goes on the job queue as a first-class flow, the same way the engine's own event firing does, and the
 * callback LOOKUP is a request inside that flow rather than a property read from C.
 *
 * WHAT IS HONESTLY ABSENT: disconnectedCallback and adoptedCallback (no removal or adoption reaction yet), and
 * customized built-ins (`extends`), which §4.13.4 rejects here rather than registering as autonomous — a
 * silently-wrong registration is worse than a named refusal. */
#include <string.h>
#include <stdlib.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/dom/node.h"
#include "core/dom/document.h"
#include "core/html/custom_elements.h"

/* §4.13.4 THE DEFINITION SET IS A HEAP OBJECT, AND THAT IS THE WHOLE ISOLATION STORY. A registry in C globals
   is SHARED state outside the per-flow COW delta, so a flow that forks anywhere BEFORE `define()` runs it in
   both arms — and the second arm throws NotSupportedError ("already defined") on a definition it never made.
   That is not a hypothetical: it aborted the sibling arm and took the rest of that flow's exploration with it.
   A plain JS object keyed by name needs no new primitive: the heap COW already captures a property ADD on a
   baseline object, so one flow's definitions are invisible to its siblings for free, and a parked flow resumes
   with exactly the registry it had. Null-prototyped so a name can never collide with Object.prototype, and
   never exposed to a page, so a read from C runs no page code.
   Each value is one definition object: { ctor, proto }. */
static JSValue g_defs;
static int    g_ready;
static JSAtom g_atom_prototype = JS_ATOM_NULL;
static JSAtom g_atom_connected = JS_ATOM_NULL;
static JSAtom g_atom_ctor = JS_ATOM_NULL;
static JSAtom g_atom_proto = JS_ATOM_NULL;
static int    g_reaction_stepid = -1;
static JSValue g_reaction_fn;

/* The definition for a name, or JS_UNDEFINED. OWNED by the caller. */
static JSValue ce_find(JSContext *ctx, const char *name, size_t len)
{
    JSAtom a = JS_NewAtomLen(ctx, name, len);
    JSValue def;

    CHECK(a != JS_ATOM_NULL, "custom elements: a name could not be interned");
    def = JS_GetProperty(ctx, g_defs, a);
    JS_FreeAtom(ctx, a);
    return def;
}

/* §4.13.1 a VALID CUSTOM ELEMENT NAME contains a hyphen and starts with an ASCII lower alpha. The hyphen is the
   whole point of the rule: it is what guarantees a custom name can never collide with a future built-in. */
static bool ce_name_valid(const char *name, size_t len)
{
    size_t i;
    if (len < 2 || name[0] < 'a' || name[0] > 'z') return false;
    for (i = 0; i < len; i++)
        if (name[i] == '-') return true;
    return false;
}

/* ---- the reaction ---------------------------------------------------------------------------------------- */
/* THE REACTION FLOW: read the callback off the element, and if it is callable, call it with the element as the
   receiver. Both steps are requests — the read because a class could define connectedCallback as an accessor
   or sit behind a Proxy, and the call because the body is the page's code. */
typedef struct JSCeReaction {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t   stage;
    uint8_t   cphase;   /* the call request's own phase */
    JSValue   fn;       /* the callback, once read (owned) */
    JSValue   cb[2];    /* the call request buffer: [this, callback] — the reaction takes no arguments */
} JSCeReaction;

static void js_ce_reaction_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSCeReaction *s = st;
    int k;
    v->val(ctx, &s->fn);
    for (k = 0; k < 2; k++)
        v->val(ctx, &s->cb[k]);
}

static JSValue js_ce_reaction_fini(JSContext *ctx, void *st, bool take_result)
{
    JSCeReaction *s = st;
    int k;
    (void)take_result;
    JS_FreeValue(ctx, s->fn);
    s->fn = JS_UNDEFINED;
    for (k = 0; k < 2; k++) {
        JS_FreeValue(ctx, s->cb[k]);
        s->cb[k] = JS_UNDEFINED;
    }
    return JS_UNDEFINED;
}

static int js_ce_reaction_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSCeReaction *s = st;
    JSValueConst el = step_arg(&s->hdr, 0);
    JSValue ignored;
    int r;

    if (s->stage == 0) {
        s->fn = JS_UNDEFINED;
        s->cb[0] = s->cb[1] = JS_UNDEFINED;
        s->stage = 1;
        if (!JS_IsObject(el)) { JS_FreeValue(ctx, cb_result); return JS_STEP_DONE; }
    }
    if (s->stage == 1) {
        r = step_getprop_run(ctx, &s->hdr, el, g_atom_connected, cb_result, &s->fn, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        s->stage = 2;
        /* §4.13.3: a definition with no connectedCallback simply has no reaction to run. */
        if (!JS_IsFunction(ctx, s->fn)) return JS_STEP_DONE;
    }
    r = step_call_run(ctx, &s->cphase, s->cb, s->fn, el, 0, NULL, cb_result, &ignored, out_cb, out_argc);
    if (r > 0) return r;
    JS_FreeValue(ctx, ignored);   /* §4.13.3: a reaction's return value is discarded */
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_ce_reaction_def = {
    sizeof(JSCeReaction), js_ce_reaction_step, js_ce_reaction_fini, 0, .visit = js_ce_reaction_visit
};

/* ---- the upgrade ------------------------------------------------------------------------------------------ */
/* §4.13.3 "upgrade": give the element the definition's prototype, then enqueue its connected reaction. The
   wrapper is the SAME object it always was, so every identity a page holds survives the upgrade. */
static void ce_upgrade(JSContext *ctx, lxb_dom_element_t *el, JSValueConst def)
{
    JSValue wrap = node_wrap(ctx, lxb_dom_interface_node(el));
    JSValue proto;
    JSValueConst argv[1];

    if (!JS_IsObject(wrap)) { JS_FreeValue(ctx, wrap); return; }
    proto = JS_GetProperty(ctx, def, g_atom_proto);
    JS_SetPrototype(ctx, wrap, proto);
    JS_FreeValue(ctx, proto);
    DCHECK(JS_IsObject(g_reaction_fn),
           "a custom element upgraded before custom_elements_init built its reaction driver");
    /* A JOB, so the callback runs as a call-root flow: preemptible, forkable and parkable like any other
       program. Calling it here would be a C activation hosting the page's loops, which is the
       drive-to-completion this engine aborts on. */
    argv[0] = wrap;
    JS_EnqueueCallJob(ctx, g_reaction_fn, 1, argv);
    JS_FreeValue(ctx, wrap);
}

void custom_elements_try_upgrade(JSContext *ctx, lxb_dom_element_t *el)
{
    size_t len = 0;
    const lxb_char_t *tag;
    JSValue def;

    if (!g_ready) return;
    tag = lxb_dom_element_local_name(el, &len);
    if (!tag || !len) return;
    def = ce_find(ctx, (const char *)tag, len);
    if (JS_IsObject(def)) ce_upgrade(ctx, el, def);
    JS_FreeValue(ctx, def);
}

/* §4.13.4 define() upgrades every EXISTING matching element, not only the ones inserted later — a definition
   that arrives after the parser is the ordinary case for a deferred bundle. */
static void ce_upgrade_document(JSContext *ctx, const char *name, size_t nlen, JSValueConst def)
{
    lxb_dom_node_t *root = document_root_node(), *n;
    size_t len = 0;

    if (!root) return;
    for (n = root; n; ) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            const lxb_char_t *tag = lxb_dom_element_local_name(lxb_dom_interface_element(n), &len);
            if (tag && len == nlen && memcmp(tag, name, len) == 0)
                ce_upgrade(ctx, lxb_dom_interface_element(n), def);
        }
        if (n->first_child) { n = n->first_child; continue; }
        while (n && !n->next) n = (n == root) ? NULL : n->parent;
        n = n ? n->next : NULL;
    }
}

/* ---- define() -------------------------------------------------------------------------------------------- */
/* Every step that can reach the page's code is DECLARED, so the body is ordinary C: `name` is a DOMString
   (ToString on whatever was passed) and `options` is an ElementDefinitionOptions whose `extends` member is a
   property READ an accessor or a Proxy turns into a call. Both are requests the shared IDL machine performs
   before this runs — it was a hand-rolled machine here only because the dictionary conversion could not yet
   express a typed member, and a second implementation of a request the machine already makes is exactly the
   duplication that machine exists to remove. */
static const IdlArgType CE_DEFINE_ARGS[3] = { IDL_DOMSTRING, IDL_ANY, IDL_DICT };
static const IdlDictMember CE_DEFINE_OPTS[] = { { "extends", IDL_DOMSTRING } };   /* ElementDefinitionOptions */

static JSValue js_ce_define(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst ctor = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue ext;
    const char *nm;
    size_t nlen;

    (void)this_val; (void)magic;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "customElements.define requires a name and a constructor");
    /* §4.13.4 step 2: the constructor must be a constructor. */
    if (!JS_IsFunction(ctx, ctor))
        return JS_ThrowTypeError(ctx, "customElements.define requires a constructor");
    /* §4.13.4 step 5: customized built-ins. Rejected rather than registered as autonomous — quietly treating
       `{extends:'button'}` as a new tag would define something the page never asked for and leave the button
       it did ask for un-upgraded. */
    ext = idl_dict_get(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, "extends");
    if (JS_IsString(ext)) {
        JS_FreeValue(ctx, ext);
        return JS_ThrowDOMException(ctx, "NotSupportedError",
                                    "a customized built-in (`extends`) is not modelled: this engine has no "
                                    "built-in element to customize yet, and registering it as an autonomous "
                                    "element would define a tag the page never asked for");
    }
    JS_FreeValue(ctx, ext);

    nm = JS_ToCStringLen(ctx, &nlen, argv[0]);   /* a real string by now: the declaration converted it */
    if (!nm) return JS_EXCEPTION;
    if (!ce_name_valid(nm, nlen)) {
        JS_FreeCString(ctx, nm);
        return JS_ThrowDOMException(ctx, "SyntaxError", "not a valid custom element name");
    }
    {
        JSValue prev = ce_find(ctx, nm, nlen), def;
        JSAtom a;
        bool taken = JS_IsObject(prev);

        JS_FreeValue(ctx, prev);
        if (taken) {
            JS_FreeCString(ctx, nm);
            return JS_ThrowDOMException(ctx, "NotSupportedError", "this name is already defined");
        }
        def = JS_NewObjectProto(ctx, JS_NULL);
        CHECK(!JS_IsException(def), "custom elements: OOM allocating a definition — a dropped definition is a "
                                    "class whose lifecycle code never runs");
        JS_SetProperty(ctx, def, g_atom_ctor, JS_DupValue(ctx, ctor));
        /* The class's `prototype` is what the upgrade installs. Read ONCE here rather than per upgrade: §4.13.4
           reads it at definition time, so a page that reassigns it afterwards does not retroactively change
           what its already-defined elements are. */
        JS_SetProperty(ctx, def, g_atom_proto, JS_GetProperty(ctx, ctor, g_atom_prototype));
        a = JS_NewAtomLen(ctx, nm, nlen);
        CHECK(a != JS_ATOM_NULL, "custom elements: a name could not be interned");
        JS_SetProperty(ctx, g_defs, a, JS_DupValue(ctx, def));
        JS_FreeAtom(ctx, a);
        ce_upgrade_document(ctx, nm, nlen, def);
        JS_FreeValue(ctx, def);
    }
    JS_FreeCString(ctx, nm);
    return JS_UNDEFINED;
}

/* §4.13.4 get(name) — the constructor a name is defined as, or undefined. */
static JSValue js_ce_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    const char *nm;
    size_t nlen;
    JSValue def, r;

    (void)this_val; (void)magic;
    if (argc < 1) return JS_UNDEFINED;
    nm = JS_ToCStringLen(ctx, &nlen, argv[0]);   /* a real string by now: the declaration converted it */
    if (!nm) return JS_EXCEPTION;
    def = ce_find(ctx, nm, nlen);
    r = JS_IsObject(def) ? JS_GetProperty(ctx, def, g_atom_ctor) : JS_UNDEFINED;
    JS_FreeValue(ctx, def);
    JS_FreeCString(ctx, nm);
    return r;
}

void custom_elements_init(JSContext *ctx)
{
    DCHECK(!g_ready, "custom_elements_init ran twice — one instance is one document");
    g_atom_prototype = JS_NewAtom(ctx, "prototype");
    g_atom_connected = JS_NewAtom(ctx, "connectedCallback");
    g_atom_ctor = JS_NewAtom(ctx, "ctor");
    g_atom_proto = JS_NewAtom(ctx, "proto");
    CHECK(g_atom_prototype != JS_ATOM_NULL && g_atom_connected != JS_ATOM_NULL &&
          g_atom_ctor != JS_ATOM_NULL && g_atom_proto != JS_ATOM_NULL,
          "a custom-element atom could not be interned");
    /* Built HERE, at init, so it belongs to the pre-boot BASELINE: a write to it during a flow is captured by
       the heap COW. A registry allocated lazily inside a flow would be that flow's private object and every
       sibling would see an empty one. */
    g_defs = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(g_defs), "the custom-element definition set could not be allocated");
    g_reaction_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_ce_reaction_def);
    /* The reaction driver is a step function object nobody installs, so a page can neither see it nor replace
       it — the same reason the internal event dispatcher is not on any prototype. */
    g_reaction_fn = JS_NewCFunction2(ctx, NULL, "connectedReaction", 1, JS_CFUNC_step, g_reaction_stepid);
    CHECK(!JS_IsException(g_reaction_fn), "the custom-element reaction driver could not be allocated");
    g_ready = 1;
}

void custom_elements_install(JSContext *ctx, JSValueConst global)
{
    JSValue reg;

    DCHECK(g_ready, "customElements was installed before custom_elements_init ran");
    reg = JS_NewObject(ctx);
    CHECK(!JS_IsException(reg), "the CustomElementRegistry allocation failed");
    idl_install_method(ctx, reg, "define", 2,
                       idl_method_id_dict(ctx, CE_DEFINE_ARGS, 3, CE_DEFINE_OPTS,
                                          (int)(sizeof(CE_DEFINE_OPTS) / sizeof(CE_DEFINE_OPTS[0])),
                                          js_ce_define, 0));
    {
        static const IdlArgType ONE_STR[1] = { IDL_DOMSTRING };
        idl_install_method(ctx, reg, "get", 1, idl_method_id(ctx, ONE_STR, 1, js_ce_get, 0));
    }
    JS_SetPropertyStr(ctx, (JSValue)global, "customElements", reg);
}

void custom_elements_free(JSContext *ctx)
{
    if (!g_ready) return;
    JS_FreeValue(ctx, g_defs);
    g_defs = JS_UNDEFINED;
    JS_FreeValue(ctx, g_reaction_fn);
    g_reaction_fn = JS_UNDEFINED;
    JS_FreeAtom(ctx, g_atom_prototype);
    JS_FreeAtom(ctx, g_atom_connected);
    JS_FreeAtom(ctx, g_atom_ctor);
    JS_FreeAtom(ctx, g_atom_proto);
    g_atom_prototype = g_atom_connected = JS_ATOM_NULL;
    g_atom_ctor = g_atom_proto = JS_ATOM_NULL;
    g_reaction_stepid = -1;
    g_ready = 0;
}
