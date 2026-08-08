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
static JSAtom g_atom_upgraded = JS_ATOM_NULL;
static JSAtom g_atom_ctor = JS_ATOM_NULL;
static JSAtom g_atom_proto = JS_ATOM_NULL;
static JSAtom g_atom_observed = JS_ATOM_NULL;      /* the definition's own key for the list */
static JSAtom g_atom_observed_src = JS_ATOM_NULL;  /* the class's `observedAttributes` */
static int    g_id_define, g_id_get;   /* declared once per agent — see custom_elements_init */
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

/* attributeChangedCallback's (name, oldValue, newValue) — the widest lifecycle callback there is. */
#define CE_MAX_REACTION_ARGS 3

/* ---- the reaction ---------------------------------------------------------------------------------------- */
/* THE REACTION FLOW: read the named callback off the element, and if it is callable, call it with the element
   as the receiver. Both steps are requests — the read because a class could define a callback as an accessor
   or sit behind a Proxy, and the call because the body is the page's code.
   ONE machine for every lifetime callback rather than one per name: what differs between connected and
   disconnected is the NAME, which is data, and the reaction queue carries it as the job's second argument. */
typedef struct JSCeReaction {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t   stage;
    uint8_t   cphase;   /* the call request's own phase */
    JSValue   fn;       /* the callback, once read (owned) */
    JSValue   cb[2 + CE_MAX_REACTION_ARGS];   /* the call request buffer: [this, callback, args…] */
} JSCeReaction;

static void js_ce_reaction_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSCeReaction *s = st;
    int k;
    v->val(ctx, &s->fn);
    for (k = 0; k < 2 + CE_MAX_REACTION_ARGS; k++)
        v->val(ctx, &s->cb[k]);
}

static JSValue js_ce_reaction_fini(JSContext *ctx, void *st, bool take_result)
{
    JSCeReaction *s = st;
    int k;
    (void)take_result;
    JS_FreeValue(ctx, s->fn);
    s->fn = JS_UNDEFINED;
    for (k = 0; k < 2 + CE_MAX_REACTION_ARGS; k++) {
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
    JSAtom name;
    int r;

    if (s->stage == 0) {
        s->fn = JS_UNDEFINED;
        for (r = 0; r < 2 + CE_MAX_REACTION_ARGS; r++) s->cb[r] = JS_UNDEFINED;
        s->stage = 1;
        if (!JS_IsObject(el)) { JS_FreeValue(ctx, cb_result); return JS_STEP_DONE; }
    }
    if (s->stage == 1) {
        name = JS_ValueToAtom(ctx, step_arg(&s->hdr, 1));
        CHECK(name != JS_ATOM_NULL, "a custom-element reaction was queued with no callback name");
        r = step_getprop_run(ctx, &s->hdr, el, name, cb_result, &s->fn, out_cb, out_argc);
        JS_FreeAtom(ctx, name);
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        s->stage = 2;
        /* §4.13.3: a definition that does not declare this callback simply has no reaction to run. */
        if (!JS_IsFunction(ctx, s->fn)) return JS_STEP_DONE;
    }
    /* Everything past the element and the callback NAME is the callback's own arguments. */
    r = step_call_run(ctx, &s->cphase, STEP_CB(s->cb), s->fn, el, s->hdr.argc - 2,
                      s->hdr.argc > 2 ? (JSValueConst *)s->hdr.argv + 2 : NULL,
                      cb_result, &ignored, out_cb, out_argc);
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
/* §4.13.3 "enqueue a custom element callback reaction". A JOB, so the callback runs as a call-root flow:
   preemptible, forkable and parkable like any other program. Calling it here would be a C activation hosting
   the page's loops, which is the drive-to-completion this engine aborts on. */
static void ce_enqueue_args(JSContext *ctx, JSValueConst wrap, const char *callback,
                            int argc, JSValueConst *args)
{
    JSValueConst argv[2 + CE_MAX_REACTION_ARGS];
    JSValue name = JS_NewString(ctx, callback);
    int i;

    DCHECK(JS_IsObject(g_reaction_fn),
           "a custom-element reaction was enqueued before custom_elements_init built its driver");
    DCHECK(argc <= CE_MAX_REACTION_ARGS,
           "a lifecycle callback was enqueued with more arguments than any of them takes");
    argv[0] = wrap;
    argv[1] = name;
    for (i = 0; i < argc; i++) argv[2 + i] = args[i];
    JS_EnqueueCallJob(ctx, g_reaction_fn, 2 + argc, argv);
    JS_FreeValue(ctx, name);
}

static void ce_enqueue(JSContext *ctx, JSValueConst wrap, const char *callback)
{
    ce_enqueue_args(ctx, wrap, callback, 0, NULL);
}

static void ce_upgrade(JSContext *ctx, lxb_dom_element_t *el, JSValueConst def)
{
    JSValue wrap = node_wrap(ctx, lxb_dom_interface_node(el));
    JSValue proto;

    if (!JS_IsObject(wrap)) { JS_FreeValue(ctx, wrap); return; }
    proto = JS_GetProperty(ctx, def, g_atom_proto);
    JS_SetPrototype(ctx, wrap, proto);
    JS_FreeValue(ctx, proto);
    /* §4.13: an element is upgraded ONCE. The mark rides the WRAPPER, so it is per-flow like everything else
       about that element — and it is what tells a re-insertion (which must fire connectedCallback again) from
       a second upgrade (which must not). */
    JS_DefinePropertyValue(ctx, (JSValue)wrap, g_atom_upgraded, JS_NewBool(ctx, true), 0);
    ce_enqueue(ctx, wrap, "connectedCallback");
    JS_FreeValue(ctx, wrap);
}

/* Has this element been upgraded — read off its wrapper's own slot, so no prototype lookup and no page code. */
static bool ce_is_upgraded(JSContext *ctx, JSValueConst wrap)
{
    JSValue v;
    bool up;

    if (!JS_IsObject(wrap)) return false;
    if (JS_GetOwnSlot(ctx, &v, wrap, g_atom_upgraded) <= 0) return false;
    up = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return up;
}

void custom_elements_disconnected(JSContext *ctx, lxb_dom_element_t *el)
{
    JSValue wrap;

    if (!g_ready) return;
    wrap = node_wrap(ctx, lxb_dom_interface_node(el));
    /* §4.13.3: only an UPGRADED element has a disconnected reaction. Asking the registry by name instead would
       fire for an element that was never upgraded — one created before its definition and removed before it. */
    if (ce_is_upgraded(ctx, wrap)) ce_enqueue(ctx, wrap, "disconnectedCallback");
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
    if (JS_IsObject(def)) {
        JSValue wrap = node_wrap(ctx, lxb_dom_interface_node(el));
        /* §4.13.3: an element already upgraded is not upgraded again — but it DOES get a connected reaction
           every time it re-enters a document, which is how a page that moves a node around keeps its
           lifecycle running. Two different things behind one insertion. */
        if (ce_is_upgraded(ctx, wrap)) ce_enqueue(ctx, wrap, "connectedCallback");
        else ce_upgrade(ctx, el, def);
        JS_FreeValue(ctx, wrap);
    }
    JS_FreeValue(ctx, def);
}

/* §4.13.4 define() upgrades every EXISTING matching element, not only the ones inserted later — a definition
   that arrives after the parser is the ordinary case for a deferred bundle. */
static void ce_upgrade_document(JSContext *ctx, const char *name, size_t nlen, JSValueConst def)
{
    lxb_dom_node_t *root = document_root_node(ctx), *n;
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

/* §4.13.3 "attribute changed": the reaction runs only for a name the definition declared as OBSERVED, which is
   why observedAttributes is read at define time and stored — a class watching two attributes must not have its
   callback run for the other fifty a page writes. Four arguments, which is what makes the generalised reaction
   carry an argument vector rather than a name alone. */
void custom_elements_attribute_changed(JSContext *ctx, lxb_dom_element_t *el, const char *name,
                                       const char *val, size_t val_len)
{
    JSValue wrap, def, observed, args[3];
    size_t len = 0, old_len = 0;
    const lxb_char_t *tag, *old;
    uint32_t n = 0, i;
    bool watched = false;

    if (!g_ready) return;
    wrap = node_wrap(ctx, lxb_dom_interface_node(el));
    if (!ce_is_upgraded(ctx, wrap)) { JS_FreeValue(ctx, wrap); return; }
    tag = lxb_dom_element_local_name(el, &len);
    def = tag && len ? ce_find(ctx, (const char *)tag, len) : JS_UNDEFINED;
    observed = JS_IsObject(def) ? JS_GetProperty(ctx, def, g_atom_observed) : JS_UNDEFINED;
    JS_FreeValue(ctx, def);
    if (JS_IsObject(observed)) {
        JSValue lv = JS_GetPropertyStr(ctx, observed, "length");
        JS_ToUint32(ctx, &n, lv);
        JS_FreeValue(ctx, lv);
        for (i = 0; i < n && !watched; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, observed, i);
            const char *s = JS_ToCString(ctx, e);
            if (s && strcmp(s, name) == 0) watched = true;
            if (s) JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, e);
        }
    }
    JS_FreeValue(ctx, observed);
    if (!watched) { JS_FreeValue(ctx, wrap); return; }
    /* §4.13.3's arguments: (name, oldValue, newValue). An attribute that was absent has a NULL old value and an
       attribute being removed a NULL new one, and the page's code branches on exactly that. */
    old = lxb_dom_element_get_attribute(el, (const lxb_char_t *)name, strlen(name), &old_len);
    args[0] = JS_NewString(ctx, name);
    args[1] = old ? JS_NewStringLen(ctx, (const char *)old, old_len) : JS_NULL;
    args[2] = val ? JS_NewStringLen(ctx, val, val_len) : JS_NULL;
    ce_enqueue_args(ctx, wrap, "attributeChangedCallback", 3, (JSValueConst *)args);
    for (i = 0; i < 3; i++) JS_FreeValue(ctx, args[i]);
    JS_FreeValue(ctx, wrap);
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

/* §4.13.4 step 10 reads `constructor.observedAttributes` and converts it to a sequence<DOMString> — a static
   GETTER and then an index read and a ToString per entry, all of it the page's code, all of it AFTER every
   declared argument is already a real value. So the body is a STEP: the declaration converts the arguments and
   this continues where it left off, parking on the getter and on each entry. */
typedef struct {
    uint8_t  stage;
    uint32_t i, n;      /* THE RESUME POINT: the observed-attribute entry being converted */
    JSValue  raw;       /* what the getter answered (owned) */
    JSValue  names;     /* the converted sequence<DOMString> (owned) */
} CeDefineState;

static void ce_define_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    CeDefineState *s = st;
    v->val(ctx, &s->raw);
    v->val(ctx, &s->names);
}

static void ce_define_release(JSContext *ctx, void *st)
{
    CeDefineState *s = st;
    JS_FreeValue(ctx, s->raw);
    JS_FreeValue(ctx, s->names);
    s->raw = s->names = JS_UNDEFINED;
}

/* The registration itself, once every value it needs is real. Plain C, and it stays that way: this is the part
   that touches only the component's own state. */
static JSValue ce_register(JSContext *ctx, int argc, JSValueConst *argv, JSValueConst names)
{
    JSValueConst ctor = argv[1];
    JSValue ext;
    const char *nm;
    size_t nlen;

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
        JS_SetProperty(ctx, def, g_atom_observed, JS_DupValue(ctx, names));
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

static int js_ce_define(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                        JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    CeDefineState *s = st;
    JSValue item;
    int r;

    if (s->stage == 0) {
        s->raw = s->names = JS_UNDEFINED;
        s->stage = 1;
        if (argc < 2) {
            JS_FreeValue(ctx, cb_result);
            JS_ThrowTypeError(ctx, "customElements.define requires a name and a constructor");
            return -1;
        }
    }
    if (s->stage == 1) {
        r = step_getprop_run(ctx, hdr, argv[1], g_atom_observed_src, cb_result, &s->raw, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return -1;
        s->stage = 2;
        s->names = JS_NewArray(ctx);
        s->i = 0;
        s->n = 0;
        /* §4.13.4: absent observedAttributes is not an error, it is no observed attributes. A present one is a
           sequence, whose length is itself a read — of an engine-visible array in every real case, and of the
           page's object when it is not, which is why the whole walk is on the trampoline. */
        if (JS_IsObject(s->raw)) {
            JSValue lv = JS_GetPropertyStr(ctx, s->raw, "length");
            JS_ToUint32(ctx, &s->n, lv);
            JS_FreeValue(ctx, lv);
        }
    }
    while (s->i < s->n) {
        JSValue entry = JS_GetPropertyUint32(ctx, s->raw, s->i);
        item = JS_UNDEFINED;
        r = step_tostring_run(ctx, hdr, entry, cb_result, &item, out_cb, out_argc);
        JS_FreeValue(ctx, entry);
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;          /* parked ON THIS ENTRY; the resume comes back to it */
        if (r < 0) return -1;
        JS_SetPropertyUint32(ctx, s->names, s->i, item);
        s->i++;
    }
    JS_FreeValue(ctx, cb_result);
    *presult = ce_register(ctx, argc, argv, s->names);
    if (JS_IsException(*presult)) { *presult = JS_UNDEFINED; return -1; }
    return 0;
}

static const IdlStepDecl CE_DEFINE_STEP = {
    js_ce_define, sizeof(CeDefineState), ce_define_visit, ce_define_release
};

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
    g_atom_upgraded = JS_NewAtom(ctx, "apiclientUpgraded");
    g_atom_ctor = JS_NewAtom(ctx, "ctor");
    g_atom_proto = JS_NewAtom(ctx, "proto");
    g_atom_observed = JS_NewAtom(ctx, "observed");
    g_atom_observed_src = JS_NewAtom(ctx, "observedAttributes");
    CHECK(g_atom_prototype != JS_ATOM_NULL &&
          g_atom_upgraded != JS_ATOM_NULL && g_atom_ctor != JS_ATOM_NULL && g_atom_proto != JS_ATOM_NULL &&
          g_atom_observed != JS_ATOM_NULL && g_atom_observed_src != JS_ATOM_NULL,
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
    /* §4.13.4's two members, DECLARED once per agent: `customElements` is a per-realm object, so installing
       from a fresh declaration would mint the pair again for a second realm's registry. */
    g_id_define = idl_method_id_step(ctx, CE_DEFINE_ARGS, 3, CE_DEFINE_OPTS,
                                     (int)(sizeof(CE_DEFINE_OPTS) / sizeof(CE_DEFINE_OPTS[0])),
                                     &CE_DEFINE_STEP, 0);
    idl_optional_from(2);   /* §4.13.4: `define(name, constructor, optional ElementDefinitionOptions options)` */
    {
        static const IdlArgType ONE_STR[1] = { IDL_DOMSTRING };
        g_id_get = idl_method_id(ctx, ONE_STR, 1, js_ce_get, 0);
    }
    g_ready = 1;
}

void custom_elements_install(JSContext *ctx, JSValueConst global)
{
    JSValue reg;

    DCHECK(g_ready, "customElements was installed before custom_elements_init ran");
    reg = JS_NewObject(ctx);
    CHECK(!JS_IsException(reg), "the CustomElementRegistry allocation failed");
    idl_install_method(ctx, reg, "define", 2, g_id_define);
    idl_install_method(ctx, reg, "get", 1, g_id_get);
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
    JS_FreeAtom(ctx, g_atom_upgraded);
    JS_FreeAtom(ctx, g_atom_ctor);
    JS_FreeAtom(ctx, g_atom_proto);
    JS_FreeAtom(ctx, g_atom_observed);
    JS_FreeAtom(ctx, g_atom_observed_src);
    g_atom_prototype = g_atom_upgraded = JS_ATOM_NULL;
    g_atom_ctor = g_atom_proto = g_atom_observed = g_atom_observed_src = JS_ATOM_NULL;
    g_reaction_stepid = -1;
    g_ready = 0;
}
