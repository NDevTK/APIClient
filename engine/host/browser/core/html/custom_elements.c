/* CUSTOM ELEMENTS — HTML §4.13.
 *
 * WHY THIS MATTERS TO A SOLVER, and why this project's rules name it beside forms: a custom element's code runs
 * ONLY through its lifecycle. A bundle that defines `<app-router>` puts its routing — and the fetches behind it
 * — inside connectedCallback, and nothing else in the program calls that function. Without the upgrade, that
 * code is shipped, reachable, and never executed, which is precisely the surface this engine exists to reach.
 * The rules say it directly: custom elements are learned by EXECUTION, through connectedCallback.
 *
 * THE UPGRADE RE-POINTS THE WRAPPER'S PROTOTYPE, and §4.13.5 does more than that: its step 10.3 CONSTRUCTS the
 * author's class and its step 10.4 requires SameValue(constructResult, element). The prototype swap is the half
 * that makes `el instanceof X` and the class's methods true on the same node a page already holds — which is
 * what makes `el === document.querySelector('app-router')` survive an upgrade — and the CONSTRUCTION is not
 * built: a class whose constructor body registers routes or fetches has that body never run. That is the
 * largest gap in this file and the one the corpus reports loudest; §4.13.5's construction stack, its
 * AlreadyConstructed marking and HTMLElement's own constructor are what it takes.
 *
 * A REACTION IS ENQUEUED, NEVER CALLED. connectedCallback is the page's code with loops and awaits in it, and
 * the insertion that triggers it happens inside appendChild — a plain C body that cannot park. So the reaction
 * goes on an ELEMENT QUEUE, and §4.13.6 decides which one: the top of the agent's custom element REACTIONS
 * STACK when a `[CEReactions]` member is on it, and the BACKUP element queue (drained by a microtask) when the
 * stack is empty. Every reaction used to take the backup arm, because nothing pushed a queue — so a page that
 * appended an element and read state its connectedCallback set ON THE NEXT LINE saw the state unset, which is
 * the ordering the whole of §4.13.6 exists to give. The stack is built here and pushed at the one point every
 * declared member already converges on: idl_args.c, which pushes before the body and INVOKES after it, because
 * invoking a reaction runs the page's code and must be able to park.
 *
 * THE QUEUE IS A JS VALUE, NOT A MALLOC'D LIST, and so is every element's own reaction queue (an own slot on
 * its wrapper under a private symbol). Both are per-flow for free — a reaction enqueued in one forked arm is
 * invisible to its sibling — and both park to the cold tier with the flow that holds them. A C list captured
 * by its head pointer would revert the POINTER on a context switch and leave the nodes reachable from nothing.
 *
 * AN UPGRADE IS A REACTION, WHICH IS WHAT MAKES IT ABLE TO CONSTRUCT AT ALL. The insertion that triggers one
 * happens inside a C tree walk, and §4.13.5 step 8.3 CONSTRUCTS the page's class — so the upgrade cannot run
 * there any more than a connectedCallback can. §4.13.6 says so directly: "try to upgrade" ENQUEUES an upgrade
 * reaction, and the drain switches on the reaction's type and runs §4.13.5 from a place that can park. That is
 * the whole of why the prototype swap that used to stand in for an upgrade is gone: the swap made
 * `el instanceof X` true and left the class's constructor body — the routes it registers, the fetches it makes
 * — unrun, which is exactly the code this engine exists to reach.
 *
 * A REGISTRY IS A NODE'S STATE, NOT THE DOCUMENT'S. HTML's CustomElementRegistry is CONSTRUCTIBLE and can be
 * associated with a Document, a ShadowRoot or an Element, and "look up a custom element definition" takes that
 * registry as its first argument — so two `<x-a>` elements in one document, one of them inside a scoped tree,
 * upgrade with two different classes. Everything here that used to ask "the" registry now asks the NODE:
 * `try to upgrade`, `define`'s three set questions, `whenDefined`, `upgrade`, and §4.13.2's own step 3 through
 * the agent's active custom element constructor map.
 *
 * WHAT IS HONESTLY ABSENT: customized built-ins (`extends`), which §4.13.4 rejects here rather than
 * registering as autonomous — a silently-wrong registration is worse than a named refusal. ALL THREE DOM sites
 * that WRITE a node's registry are now built: `create an element`'s flattened creation options
 * (core/dom/document.c), `attach a shadow root`'s ShadowRootInit member (core/dom/shadow_root.c), and DOM §4.5
 * `adopt`'s re-derivation — custom_elements_node_adopted is its steps 3.2/3.3.2/3.3.3, driven by
 * core/dom/node.c's adopt walk. The registry-less creation entry that stood in for the first two, and the
 * `claimed` latch whose only purpose was to make its DCHECK fire the moment their absence changed an answer,
 * are deleted with them: an entry kept past its callers is a second way to ask a question that now has one. */
#include <string.h>
#include <stdlib.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/dom/node.h"
#include "core/dom/names.h"
#include "core/dom/attr_list.h"   /* §4.9's (namespace, local name) lookup — the old value of THIS attribute */
#include "core/dom/element.h"
#include "core/dom/document.h"
#include "core/dom/shadow_root.h"
#include "core/html/html_element.h"
#include "core/html/custom_elements.h"
#include "core/html/element_internals.h"

/* ---- §4.13.4 A REGISTRY IS AN OBJECT, AND `customElements` IS ONE OF THEM --------------------------------
 *
 * THIS WAS A SET OF PER-REALM SLOTS with the four members installed onto a bare `JS_NewObject`, which IS the
 * whole of what "there is one global registry" means: `new CustomElementRegistry()` did not exist, no node
 * could be ASSOCIATED with a registry, and every lookup answered out of the realm's one set. HTML now defines
 * `CustomElementRegistry` as a CONSTRUCTIBLE interface whose constructor sets its `is scoped` boolean, and DOM
 * gives every Element, Document and ShadowRoot a `custom element registry` that §4.13.3's "look up a custom
 * element definition" is performed AGAINST. So the definition set, the when-defined map and the definition
 * order move ONTO the object, and every lookup asks the node — `element.customElementRegistry`, not "the".
 *
 * THE STATE IS ONE RECORD UNDER ONE SYMBOL. The registry is handed to the page (`window.customElements`), so a
 * plain `defs` own property would be a string key of this engine's invention on an object every bundle
 * touches, and `Object.keys(customElements)` would report it. One symbol-keyed slot holds a null-prototyped
 * record; its own fields are then ordinary atoms because nothing outside can reach the record at all. Every
 * field is a JS VALUE for the reason everything else in this file is: a definition a flow makes is invisible to
 * its siblings and parks with the flow that made it, which a C registry in a module static could be neither.
 *
 * A REGISTRY IS STILL PER WINDOW — the DOCUMENT'S registry is this realm's, held in a realm slot, because
 * "A Window's associated Document is always created with a new CustomElementRegistry object". One set for the
 * agent meant `frame.contentWindow.customElements.define('x-a', C)` defined `x-a` in the PARENT too. */
static JSClassID g_registry_class;
/* §4.13.4's Window `customElements` getter: "this's associated Document's custom element registry". */
static int g_registry_slot = -1;
/* The registry's own record, under a symbol this component minted and never published. */
static JSValue g_reg_key = JS_UNDEFINED;
static JSAtom  g_atom_reg = JS_ATOM_NULL;
/* The record's fields. It is engine-built and null-prototyped, so these are ordinary atoms — nothing of the
   page's is on it and a read of one runs no accessor and no Proxy. */
static JSAtom g_atom_defs = JS_ATOM_NULL;      /* §4.13.4's custom element definition set, keyed by NAME */
static JSAtom g_atom_order = JS_ATOM_NULL;     /* the SAME set in definition order — §4.13.2 step 4's walk */
static JSAtom g_atom_whendef = JS_ATOM_NULL;   /* §4.13.4's when-defined promise map */
static JSAtom g_atom_scoped = JS_ATOM_NULL;    /* §4.13.4's `is scoped`, set by the constructor */
static JSAtom g_atom_docs = JS_ATOM_NULL;      /* §4.13.4's scoped document set */
static JSAtom g_atom_defining = JS_ATOM_NULL;  /* §4.13.4's `element definition is running` */

/* §4.13.2's ACTIVE FUNCTION OBJECT, for the one interface that carries `[HTMLConstructor]` today. Step 5 is
   "the active function object must be HTMLElement" for an autonomous custom element, and that is an IDENTITY
   question about a PER-REALM object — a module static holding one realm's HTMLElement would answer it wrong
   for every other document, which is the defect class §3.7 names. Set by the mint below, which is what
   html_element.c calls to build the interface object. */
static int g_html_ctor_slot = -1;

/* DOM §4.4/§4.8/§4.9's NODE-ASSOCIATED custom element registry, on the node's WRAPPER under its own symbol —
   the same store the element's definition and its custom element state already use, and per-flow for the same
   reason. ABSENT and JS_NULL are DIFFERENT: null is the value DOM's own `customElementRegistry: null` creation
   option writes, and absent means no operation has written one, which is DERIVED below. */
static JSValue g_node_reg_key = JS_UNDEFINED;
static JSAtom  g_atom_node_reg = JS_ATOM_NULL;

/* CONFIGURABLE AND WRITABLE, because §4.13.5 writes the state THREE TIMES on its way through one element
   ("failed", then "precustomized", then "custom") and the definition it writes at step 2 is DELETED again by
   step 8.9.1. A slot defined with no flags is none of those things, and the second write is then a silent
   no-op that leaves an element reporting the state it had two steps ago. The key is a symbol this component
   minted and never published, so nothing outside can reach either slot however they are flagged. */
#define CE_SLOT_FLAGS (JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE)

/* THE REGISTRY'S RECORD, and the BRAND TEST in one: an object with no record slot is not a
   CustomElementRegistry, and the slot's key is a symbol nothing outside can name, so presence IS the brand
   rather than standing in for one. OWNED; JS_UNDEFINED when the receiver is not a registry. */
static JSValue ce_reg_record(JSContext *ctx, JSValueConst reg)
{
    JSValue v;

    if (!JS_IsObject(reg)) return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &v, reg, g_atom_reg) <= 0) return JS_UNDEFINED;
    return v;
}

/* One field of it. OWNED. */
static JSValue ce_reg_field(JSContext *ctx, JSValueConst reg, JSAtom f)
{
    JSValue rec = ce_reg_record(ctx, reg), v;

    DCHECK(JS_IsObject(rec), "a custom element registry field was read off something that is not a registry — "
                             "every member brand-checks its receiver before it reads one");
    v = JS_GetProperty(ctx, rec, f);
    JS_FreeValue(ctx, rec);
    return v;
}

static bool ce_reg_flag(JSContext *ctx, JSValueConst reg, JSAtom f)
{
    JSValue rec = ce_reg_record(ctx, reg), v;
    bool b;

    if (!JS_IsObject(rec)) { JS_FreeValue(ctx, rec); return false; }
    v = JS_GetProperty(ctx, rec, f);
    b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, rec);
    return b;
}

static void ce_reg_set_flag(JSContext *ctx, JSValueConst reg, JSAtom f, bool b)
{
    JSValue rec = ce_reg_record(ctx, reg);

    DCHECK(JS_IsObject(rec), "a custom element registry flag was written onto something that is not a registry");
    JS_SetProperty(ctx, rec, f, b ? JS_TRUE : JS_FALSE);
    JS_FreeValue(ctx, rec);
}

/* A NEW CustomElementRegistry — §4.13.4's five fields at their initial values, and the constructor's own step
   ("set this's is scoped to true") as the argument that decides one of them. OWNED. */
static JSValue ce_registry_new(JSContext *ctx, bool scoped)
{
    JSValue reg, rec, proto;

    proto = JS_GetClassProto(ctx, g_registry_class);
    DCHECK(!JS_IsNull(proto), "a CustomElementRegistry was minted in a realm that never ran its prototype "
                              "install — §3.7 gives every realm its own, and the members on it answer out of "
                              "the realm that defined them");
    reg = JS_NewObjectProtoClass(ctx, proto, g_registry_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(reg), "a CustomElementRegistry could not be allocated");
    rec = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(rec), "a CustomElementRegistry's record could not be allocated");
    {
        /* The definition set keyed by name, and THE SAME SET IN DEFINITION ORDER — because §4.13.2 step 4 looks
           a definition up BY ITS CONSTRUCTOR and a JS object cannot be keyed by one (it is the page's function
           object, and hanging an engine-minted symbol on it would put a slot on a page value that
           `Object.getOwnPropertySymbols` reports). Both are written by the one commit and hold the same
           definition objects; a definition in one and not the other is a DCHECK at the walk. */
        JSValue defs = JS_NewObjectProto(ctx, JS_NULL);
        JSValue order = JS_NewArray(ctx);
        JSValue pending = JS_NewObjectProto(ctx, JS_NULL);
        JSValue docs = JS_NewArray(ctx);

        CHECK(!JS_IsException(defs) && !JS_IsException(order) && !JS_IsException(pending) &&
              !JS_IsException(docs),
              "custom elements: OOM building a registry's definition set — a dropped definition is a class "
              "whose lifecycle code never runs");
        JS_SetProperty(ctx, rec, g_atom_defs, defs);
        JS_SetProperty(ctx, rec, g_atom_order, order);
        JS_SetProperty(ctx, rec, g_atom_whendef, pending);
        JS_SetProperty(ctx, rec, g_atom_docs, docs);
    }
    JS_SetProperty(ctx, rec, g_atom_scoped, scoped ? JS_TRUE : JS_FALSE);
    JS_SetProperty(ctx, rec, g_atom_defining, JS_FALSE);
    JS_DefinePropertyValue(ctx, reg, g_atom_reg, rec, CE_SLOT_FLAGS);
    return reg;
}

/* THIS REALM'S Document's custom element registry — §4.13.4's Window getter, and the "default" every creation
   that names no registry resolves to. OWNED: the caller frees. */
static JSValue ce_document_registry(JSContext *ctx)
{
    DCHECK(g_registry_slot > 0,
           "the Document's custom element registry was reached before custom_elements_init declared its slot");
    return realm_value_get(ctx, g_registry_slot);
}

/* WEB IDL §3.7.5's BRAND CHECK, whose failure is a TypeError thrown before the member's own step 1. The
   receiver itself is what the member goes on using — it is the argument it already holds — so this answers
   only whether it may. With the members on the PROTOTYPE this is the only thing standing between
   `CustomElementRegistry.prototype.define.call({}, …)` and a definition committed onto nothing. */
static bool ce_registry_this(JSContext *ctx, JSValueConst this_val)
{
    JSValue rec = ce_reg_record(ctx, this_val);
    bool ok = JS_IsObject(rec);

    JS_FreeValue(ctx, rec);
    /* THE TWO BRAND TESTS IN THIS FILE MUST AGREE, and this is where that is asserted rather than assumed.
       One asks whether the object carries the class id, the other whether it carries the record; they are the
       same question only because ce_registry_new does both in one place. If they ever disagree, a registry was
       minted without its record, or an object is wearing the class without being one — and the previous version
       of the class-id test disagreed with this one for every registry in existence with nothing to say so. */
    DCHECK(ok == custom_elements_is_registry(this_val),
           "a CustomElementRegistry's class-id brand and its internal-slot record disagree about the same "
           "object — one of the two mints is incomplete");
    if (!ok) JS_ThrowTypeError(ctx, "not a CustomElementRegistry");
    return ok;
}

/* The document a node belongs to — DOM §4.4's "node document", which for a Document is itself. */
static lxb_dom_document_t *ce_node_document(lxb_dom_node_t *n)
{
    if (!n) return NULL;
    if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT) return lxb_dom_interface_document(n);
    return n->owner_document;
}

/* A DOCUMENT'S custom element registry. DOM: "Unless stated otherwise, a document's custom element registry is
   NULL" — and the one document stated otherwise is the Window's, which "is always created with a new
   CustomElementRegistry object". So a document this realm is not the window of (a `createDocument` /
   `DOMParser` / `createHTMLDocument` document) answers NULL, which is exactly why an element parsed into one is
   never upgraded until it is adopted. OWNED. */
static JSValue ce_registry_of_document(JSContext *ctx, const lxb_dom_document_t *doc)
{
    lxb_dom_node_t *dn = node_of(document_object(ctx));

    if (!doc || !dn || ce_node_document(dn) != doc) return JS_NULL;
    return ce_document_registry(ctx);
}

/* §4.13.4's "LOOK UP A CUSTOM ELEMENT REGISTRY, given a node" — an Element's, a ShadowRoot's or a Document's
   own registry, and null for anything else.
 *
 * ABSENT IS DERIVED, NOT NULL, and the derivation is the value the CREATING operation would have written. DOM
 * sets the field at four sites — `create an element` (from the flattened creation options, defaulting to the
 * document's), `attach a shadow root` (from ShadowRootInit, defaulting to the host's node document's),
 * `create an element internal`, and `adopt`, which is the one of the four that now STAMPS
 * (custom_elements_node_adopted). Two of the remaining three live in components this one may not write into
 * (core/dom/document.c, core/dom/shadow_root.c). Deriving is not a stand-in for them: for every registry that
 * is NOT scoped the derived answer IS the stored one, because all four sites write the node document's
 * registry. Only a SCOPED registry is a value no derivation can produce, and that is why it must be STAMPED —
 * by `initialize`, by §4.13.2's constructor, by `adopt` (whose derivation from the PARENT is what makes an
 * element adopted under a scoped tree answer null rather than the new document's), and by the two sites above
 * once they can pass one. OWNED. */
static JSValue ce_registry_of_node(JSContext *ctx, JSValueConst wrap)
{
    lxb_dom_node_t *n, *root;
    JSValue v;

    if (!JS_IsObject(wrap)) return JS_NULL;
    if (JS_GetOwnSlot(ctx, &v, wrap, g_atom_node_reg) > 0) return v;   /* the stamp — JS_NULL included */
    n = node_of(wrap);
    if (!n) return JS_NULL;
    if (n->type != LXB_DOM_NODE_TYPE_ELEMENT && n->type != LXB_DOM_NODE_TYPE_DOCUMENT && !shadow_root_is(n))
        return JS_NULL;                                                /* "Return null" for every other node */
    /* An element in a SHADOW TREE was created by a fragment parse or a `createElement` performed against that
       tree, both of which take the shadow root's registry — so a scoped root's descendants answer scoped
       without every one of them carrying a stamp. Read through the EXISTING wrapper: a root nothing has
       wrapped was never stamped, and minting one here would allocate a wrapper per element on the insertion
       steps' hot path. */
    root = node_root(n);
    if (root && root != n && shadow_root_is(root)) {
        JSValueConst rw = node_wrap_peek(root);
        JSValue rv;

        if (JS_IsObject(rw) && JS_GetOwnSlot(ctx, &rv, rw, g_atom_node_reg) > 0) return rv;
    }
    return ce_registry_of_document(ctx, ce_node_document(n));
}

/* HTML §4.13.4's "Once the custom element registry of a node is initialized to a CustomElementRegistry object,
   it intentionally cannot be changed any further", AS THE INVARIANT DOM §4.5 LEAVES STANDING. The sentence is
   about a SCOPED association: `adopt` step 3.2/3.3.2 re-derive a node's registry every time it crosses into
   another document, and both arms are guarded on the current registry being null or GLOBAL, so what can never
   be replaced is a scoped registry — which is exactly the association a page reasons about and an
   implementation optimizes against. Asserting "never written twice" instead would have made adoption itself
   the violation. */
static void ce_node_set_registry(JSContext *ctx, JSValueConst wrap, JSValueConst reg)
{
    JSValue prev;

    DCHECK(JS_IsObject(wrap), "a custom element registry was associated with something that is not a node");
    if (JS_GetOwnSlot(ctx, &prev, wrap, g_atom_node_reg) > 0) {
        bool same = JS_VALUE_GET_PTR(prev) == JS_VALUE_GET_PTR(reg);
        bool was_scoped = JS_IsObject(prev) && ce_reg_flag(ctx, prev, g_atom_scoped);
        JS_FreeValue(ctx, prev);
        DCHECK(same || !was_scoped,
               "a node's SCOPED custom element registry was replaced — HTML states the association cannot be "
               "changed once it is made, and DOM §4.5 adopt's own re-derivation is guarded against exactly "
               "this, so the second writer is one that read the node's registry wrong");
    }
    JS_DefinePropertyValue(ctx, (JSValue)wrap, g_atom_node_reg, JS_DupValue(ctx, reg), CE_SLOT_FLAGS);
}

/* §4.8's attachShadow AND §4.9's create-an-element BOTH resolve a registry before they can act, and neither
 * lives here — so the questions they ask are exported rather than re-derived. They are the same ones §4.8's
 * attachShadow steps 1-3 ask, in order: what is this document's registry, is a page-supplied one a
 * CustomElementRegistry at all, and is it scoped.
 *
 * THE BRAND TEST IS ONE OF THEM AND NOT A CONVENIENCE. `init["customElementRegistry"]` is a page value: the
 * declaration converts it to an object, and an object that is not one of these must not be associated with a
 * node as though it were — every later lookup would read a record that is not there.
 *
 * THE ASSOCIATION IS EXPORTED AS AN OPERATION rather than as the slot: the once-only rule, the scoped-registry
 * latch and the key are all this component's, so a caller states WHICH node and WHICH registry and nothing
 * else. That is what keeps DOM's "once initialized it cannot be changed" a fact about the write instead of a
 * convention every writer has to remember. */
/* THE BRAND IS THE CLASS ID, because this component deliberately has no opaque to ask about. §4.13.4's five
   fields live in an own slot under a private symbol — the file's own header says why: a write to them is then
   an ordinary property write the per-flow COW delta captures, so a definition committed in one arm of a fork is
   invisible to its sibling for free. Nothing here ever calls JS_SetOpaque.
   So the test this used to make — `JS_GetOpaque(v, g_registry_class) != NULL` — was NULL for every registry
   this file has ever minted, and answered FALSE for the real thing. It was not merely an assert that fired:
   §4.8's attachShadow and §4.9's create-an-element both run it as their step 2 BRAND CHECK, so a page handing
   either one a genuine CustomElementRegistry got a TypeError, and the document's own registry could not pass
   its own step 3. `JS_NewObjectProtoClass` stamps the class id at the mint and nothing in the language can
   forge one, which is the whole of what a brand check needs. */
bool custom_elements_is_registry(JSValueConst v)
{
    return g_registry_class != 0 && JS_GetClassID(v) == g_registry_class;
}

bool custom_elements_registry_is_scoped(JSContext *ctx, JSValueConst reg)
{
    DCHECK(custom_elements_is_registry(reg),
           "§4.13.4's `is scoped` was asked of something that is not a CustomElementRegistry");
    return ce_reg_flag(ctx, reg, g_atom_scoped);
}

void custom_elements_node_associate_registry(JSContext *ctx, JSValueConst wrap, JSValueConst reg)
{
    ce_node_set_registry(ctx, wrap, reg);
}

JSValue custom_elements_document_registry(JSContext *ctx)
{
    return ce_document_registry(ctx);
}

/* A NODE'S OWN CUSTOM ELEMENT REGISTRY, derived where it holds none — the read side of the association above,
   for an algorithm that must PASS a node's registry on rather than look a definition up with it. DOM §4.4's
   clone step 6.2 is the caller: the copy's shadow root takes the ORIGINAL root's registry, so a host inside a
   scoped tree clones into a copy that resolves in the same scoped registry. OWNED. */
JSValue custom_elements_node_registry(JSContext *ctx, JSValueConst wrap)
{
    return ce_registry_of_node(ctx, wrap);
}
static int    g_ready;
static JSAtom g_atom_prototype = JS_ATOM_NULL;
static JSAtom g_atom_ctor = JS_ATOM_NULL;
static JSAtom g_atom_proto = JS_ATOM_NULL;
static JSAtom g_atom_observed = JS_ATOM_NULL;      /* the definition's own key for the list */
static JSAtom g_atom_observed_src = JS_ATOM_NULL;  /* the class's `observedAttributes` */
static JSAtom g_atom_callbacks = JS_ATOM_NULL;     /* the definition's own key for step 14.4's map */
/* §4.13.4 step 15's definition is a RECORD, and three of its fields were missing because nothing yet read
   them. `name` and `local name` are two fields and not one — they are equal for an AUTONOMOUS custom element
   and differ for a customized built-in, and §4.13.2 step 5 tells the two apart by comparing exactly them. The
   CONSTRUCTION STACK is what makes §4.13.5's upgrade and §4.13.2's constructor one algorithm rather than two:
   the constructor answers with a FRESH element when the stack is empty and with the stack's last entry when an
   upgrade put one there, and that is the whole of how `super()` inside an upgrade returns the node the page
   already holds. An Array, so it forks per flow and parks with the flow that is inside the constructor. */
static JSAtom g_atom_name = JS_ATOM_NULL;
static JSAtom g_atom_local = JS_ATOM_NULL;
static JSAtom g_atom_stack = JS_ATOM_NULL;
/* §4.13.4 step 15's THREE BOOLEAN FIELDS, and the two class properties steps 14.7 and 14.11 read them out of.
   They are fields of the DEFINITION and not questions asked of the constructor later, for the same reason
   `observed attributes` is: a class that reassigns `formAssociated` after `define()` must not retroactively
   make its already-registered elements form-associated. Stored as one flags integer under one key, because
   the three are written together by one step list and read together by one predicate — three keys would be
   three chances for a definition to carry two of them. */
static JSAtom g_atom_flags = JS_ATOM_NULL;
static JSAtom g_atom_disabled_src = JS_ATOM_NULL;   /* the class's `disabledFeatures` */
static JSAtom g_atom_form_assoc_src = JS_ATOM_NULL; /* the class's `formAssociated` */

/* §4.13.5 step 2's "set element's custom element definition to definition" — the element's OWN slot, under a
   symbol the page cannot mint, so no string key of this engine's invention appears on a custom element. It
   replaces an `apiclientUpgraded` boolean: the boolean answered "has this been upgraded" and nothing else, and
   the reaction then had to find the definition again BY NAME through the registry. §4.13.6's enqueue reads the
   definition off the ELEMENT, which is what makes the definition an element's own state and not a lookup. */
static JSValue g_def_key = JS_UNDEFINED;
static JSAtom  g_atom_def = JS_ATOM_NULL;

/* DOM §4.9's "custom element state", on the same wrapper and under its own private symbol. It is a SECOND
   field beside the definition and not a reading of it, because three of the five values coexist with a
   non-null definition ("failed", "precustomized", "custom") and the algorithms that branch on them cannot ask
   the definition which one it is. Absent means "the element has never been told", which is not a fourth
   answer: DOM §4.9 gives a freshly created element "undefined" exactly when its local name is one §4.13.1
   would accept and "uncustomized" otherwise, so the absent case is DERIVED from the name rather than written
   at every creation site — one of which is the HTML parser, which creates elements this component never sees. */
static JSValue g_state_key = JS_UNDEFINED;
static JSAtom  g_atom_state = JS_ATOM_NULL;

/* §4.13.4 step 14's `lifecycleCallbacks` map, IN ITS KEY ORDER, AS ONE LIST EXPANDED TWICE — the ids the
   engine enqueues by and the names step 14.4 reads off the prototype, which cannot be two lists for the same
   reason a machine's stages cannot: the ORDER is observable. The prototype may be a Proxy, so the sequence of
   `get`s is part of what the algorithm does and the corpus asserts it exactly.
   `connectedMoveCallback` is NOT here because `moveBefore` is not: §4.13.4 names that key on a platform that
   has the operation which fires it, and collecting one for an operation this engine cannot perform would show
   a page's Proxy a read for a callback nothing will ever invoke. custom_elements_install ASSERTS that pairing.
   THE FORM FOUR ARE A SECOND LIST AND NOT FOUR MORE ENTRIES OF THIS ONE, because §4.13.4 reads them at a
   different STEP under a different condition: step 14.4 walks THIS list unconditionally off the prototype, and
   step 14.13 walks the form list only when step 14.12's `formAssociated` converted to true. Merging them would
   show a page's Proxy four reads the algorithm never makes for a class that is not form-associated — the same
   observability rule that keeps `connectedMoveCallback` out. They index into ONE callback map, contiguously,
   because §4.13.4 step 15's definition holds one map and a reaction names one entry of it. */
#define CE_LIFECYCLE_CALLBACKS(X) \
    X(CE_CB_CONNECTED,    "connectedCallback") \
    X(CE_CB_DISCONNECTED, "disconnectedCallback") \
    X(CE_CB_ADOPTED,      "adoptedCallback") \
    X(CE_CB_ATTR_CHANGED, "attributeChangedCallback")
/* §4.13.4 step 14.13's « formAssociatedCallback, formResetCallback, formDisabledCallback,
   formStateRestoreCallback » — IN THAT ORDER, because the reads are observable in it. */
#define CE_FORM_CALLBACKS(X) \
    X(CE_CB_FORM_ASSOCIATED,    "formAssociatedCallback") \
    X(CE_CB_FORM_RESET,         "formResetCallback") \
    X(CE_CB_FORM_DISABLED,      "formDisabledCallback") \
    X(CE_CB_FORM_STATE_RESTORE, "formStateRestoreCallback")
#define CE_CB_ID(id, name)   id,
#define CE_CB_NAME(id, name) name,
enum { CE_LIFECYCLE_CALLBACKS(CE_CB_ID) CE_FORM_CALLBACKS(CE_CB_ID) CE_CB_COUNT,
       /* WHERE THE SECOND LIST STARTS, stated as the first id of it rather than as the length of the first —
          so step 14.4's loop bound and step 14.13's starting index are the SAME fact and cannot drift. */
       CE_CB_LIFECYCLE_COUNT = CE_CB_FORM_ASSOCIATED };
static const char *const CE_CALLBACK_NAMES[CE_CB_COUNT] = {
    CE_LIFECYCLE_CALLBACKS(CE_CB_NAME) CE_FORM_CALLBACKS(CE_CB_NAME)
};
static JSAtom g_cb_atoms[CE_CB_COUNT];
/* declared once per agent */
static int    g_id_define, g_id_get, g_id_get_name, g_id_when_defined, g_id_upgrade, g_id_initialize;

/* §4.13.3's "LOOK UP A CUSTOM ELEMENT DEFINITION", steps 1 and 3, given the REGISTRY the caller resolved. Step
   1 is "if registry is null, return null", which is why a node in a document that has no registry — a
   `DOMParser` document, a template's contents — has no definitions at all rather than the window's. Step 4's
   `is` arm is the customized-built-in half, which §4.13.4 refuses to register, so no definition can have a
   name that differs from its local name and the arm has nothing to find; it becomes a real read in the diff
   that makes `extends` registrable. OWNED by the caller. */
static JSValue ce_find_in(JSContext *ctx, JSValueConst registry, const char *name, size_t len)
{
    JSAtom a;
    JSValue def;

    if (!JS_IsObject(registry)) return JS_UNDEFINED;             /* step 1 */
    a = JS_NewAtomLen(ctx, name, len);
    CHECK(a != JS_ATOM_NULL, "custom elements: a name could not be interned");
    {
        JSValue defs = ce_reg_field(ctx, registry, g_atom_defs);
        def = JS_GetProperty(ctx, defs, a);
        JS_FreeValue(ctx, defs);
    }
    JS_FreeAtom(ctx, a);
    return def;
}

/* The same lookup performed FOR A NODE: its own registry, its local name. §4.13.3 step 2's "if namespace is not
   the HTML namespace, return null" is the element's, and every element this engine can define one for is in it.
   OWNED. */
static JSValue ce_find_for_node(JSContext *ctx, JSValueConst wrap, const char *name, size_t len)
{
    JSValue reg = ce_registry_of_node(ctx, wrap);
    JSValue def = ce_find_in(ctx, reg, name, len);

    JS_FreeValue(ctx, reg);
    return def;
}

/* §4.13.1 A VALID CUSTOM ELEMENT NAME — all five of its requirements. This was "starts with a-z and contains a
   hyphen", which is two of them, and the three it left out are not pedantry: `a-A` was accepted although no
   parser can round-trip it, `annotation-xml` was accepted although the name belongs to MathML, and every name
   whose later code points the DOM forbids was accepted although `createElement` could never build one. The
   requirement that carries the others is the FIRST one — the name must be a VALID ELEMENT LOCAL NAME, which is
   the DOM's own predicate and lives with its sibling in core/dom/names.c rather than being re-derived here.
   The hyphen requirement is still what guarantees a custom name cannot collide with a future built-in; the
   reserved list is the eight names that already contain one and are already taken. */
bool custom_elements_name_is_valid(const char *name, size_t len)
{
    /* §4.13.1's list, verbatim: the SVG and MathML element names that contain a hyphen. */
    static const char *const RESERVED[] = {
        "annotation-xml", "color-profile", "font-face", "font-face-src", "font-face-uri",
        "font-face-format", "font-face-name", "missing-glyph", NULL
    };
    size_t i;
    bool hyphen = false;

    if (!dom_valid_element_local_name(name, len)) return false;
    if (name[0] < 'a' || name[0] > 'z') return false;      /* the 0th code point is an ASCII lower alpha */
    for (i = 0; i < len; i++) {
        if (name[i] >= 'A' && name[i] <= 'Z') return false;  /* it contains no ASCII upper alphas */
        if (name[i] == '-') hyphen = true;                  /* it contains a U+002D (-) */
    }
    if (!hyphen) return false;
    for (i = 0; RESERVED[i]; i++)
        if (strlen(RESERVED[i]) == len && memcmp(RESERVED[i], name, len) == 0) return false;
    return true;
}

/* DOM §4.9's custom element state for an element, DERIVED when nothing has written one — see g_state_key. The
   wrapper is the store, so this answers for the element the page holds and forks with the flow that changed it. */
static int ce_state_of(JSContext *ctx, JSValueConst wrap)
{
    lxb_dom_node_t *n;
    JSValue v;
    int32_t s = 0;

    if (!JS_IsObject(wrap)) return CE_STATE_UNCUSTOMIZED;
    if (JS_GetOwnSlot(ctx, &v, wrap, g_atom_state) > 0) {
        int ok = JS_ToInt32(ctx, &s, v) == 0;
        JS_FreeValue(ctx, v);
        DCHECK(ok && s >= CE_STATE_UNCUSTOMIZED && s <= CE_STATE_CUSTOM,
               "an element's custom element state slot holds something that is not one of DOM §4.9's five "
               "values — the slot is written by ce_set_state and by nothing else");
        return (int)s;
    }
    n = node_of(wrap);
    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return CE_STATE_UNCUSTOMIZED;
    {
        size_t len = 0;
        const lxb_char_t *tag = lxb_dom_element_local_name(lxb_dom_interface_element(n), &len);
        if (tag && len && custom_elements_name_is_valid((const char *)tag, len)) return CE_STATE_UNDEFINED;
    }
    return CE_STATE_UNCUSTOMIZED;
}

static void ce_set_state(JSContext *ctx, JSValueConst wrap, int state)
{
    DCHECK(JS_IsObject(wrap), "a custom element state was written onto something that is not an element wrapper");
    DCHECK(state >= CE_STATE_UNCUSTOMIZED && state <= CE_STATE_CUSTOM,
           "a custom element state DOM §4.9 does not name was written onto an element");
    JS_DefinePropertyValue(ctx, (JSValue)wrap, g_atom_state, JS_NewInt32(ctx, state), CE_SLOT_FLAGS);
}

/* ---- §4.13.6 the custom element reactions stack ------------------------------------------------------------
   THE QUEUES AND EVERY REACTION ON THEM ARE JS VALUES. A reaction has to fork per flow (two arms of a branch
   that both append an element each have their own connectedCallback pending) and it has to PARK to the cold
   tier with the flow holding it, and a JS Array does both for free: its mutations are property writes the COW
   delta already captures — and an Array a FLOW created is flow-private, so a member's own queue costs the
   delta nothing at all.
   A REACTION is « callback function, arguments… » as one Array; an ELEMENT QUEUE is an Array of element
   wrappers; an element's own REACTION QUEUE is an Array on an own slot of its wrapper under a private symbol,
   so it is per-flow exactly like every other own property of that wrapper.
   THERE IS NO STACK ARRAY, and that is not a shortcut. §4.13.6's reactions stack exists to model the NESTING of
   `[CEReactions]` invocations, and with the trampoline a declared member's own steps run inside exactly one C
   activation of the IDL machine — so the "current element queue" is that machine's, for exactly as long as
   that call lasts, and the nesting is one frame deep by construction. A shared stack Array would be BASELINE
   state written twice per member call, which is a delta entry per DOM API call for bookkeeping no flow needs
   to time-travel: measured at 2927 entries per context switch against 219 without it. */
static CustomElementQueue *g_current;   /* the innermost declared member's queue, while its own steps run */
static JSValue g_ce_backup = JS_UNDEFINED;   /* §4.13.6's backup element queue */
static JSValue g_rq_key = JS_UNDEFINED;      /* the element's reaction-queue slot key (a Symbol) */
static JSAtom  g_atom_rq = JS_ATOM_NULL;
/* THE CONSUMED CURSOR OF A REACTION QUEUE, as a property of the queue itself rather than a C counter: §4.13.6
   step 1.3 repeats "remove the first reaction" until the queue is EMPTY, and a callback may append to the very
   queue being drained. A head index makes the removal O(1) and keeps the append visible, and being a property
   it forks and parks with the flow like the queue it indexes. */
static JSAtom  g_atom_rq_head = JS_ATOM_NULL;
/* §4.13.6's "processing the backup element queue" flag, on the backup queue itself for the same reason. */
static JSAtom  g_atom_backup_flag = JS_ATOM_NULL;
static int     g_backup_stepid = -1;

static uint32_t ce_array_len(JSContext *ctx, JSValueConst arr)
{
    JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, lv);
    JS_FreeValue(ctx, lv);
    return n;
}

/* Append to an Array — one write the COW delta captures, which is the whole reason these are Arrays. */
static void ce_array_push(JSContext *ctx, JSValueConst arr, JSValue v)
{
    JS_SetPropertyUint32(ctx, (JSValue)arr, ce_array_len(ctx, arr), v);
}

static void ce_array_set_len(JSContext *ctx, JSValueConst arr, uint32_t n)
{
    JS_SetPropertyStr(ctx, (JSValue)arr, "length", JS_NewUint32(ctx, n));
}

/* ---- §4.13.4's ACTIVE CUSTOM ELEMENT CONSTRUCTOR MAP -------------------------------------------------------
 * "Each similar-origin window agent has an associated active custom element constructor map, which is a map of
 * constructors to CustomElementRegistry objects." It exists for ONE question, asked in exactly one place:
 * §4.13.2 step 3, where the HTML element constructor must know WHICH registry the definition it is building
 * for came from. Without it a `super()` inside a class registered in a scoped registry would look itself up in
 * the document's set and throw a TypeError — the constructor would be unreachable, which is the whole feature.
 *
 * IT IS A STACK, AND THAT IS THE MAP. The spec's two writers are paired — §4.13.5 steps 7.5-7.6 save
 * `previousRegistry` and set the entry, and the cleanup puts the previous value back or removes the entry —
 * so a walk that finds the LAST entry for a constructor answers exactly what the map holds, and dropping the
 * last entry restores exactly what the map restores. Nesting (a constructor that constructs its own class) is
 * what makes `previousRegistry` non-null and what the stack models directly.
 *
 * AN ARRAY, AGENT-WIDE, in a module static for the reason the backup element queue is one: it is the AGENT's,
 * not a realm's, and its entries are property writes the COW delta captures — so a flow parked inside a
 * constructor carries the map state it was constructed under. */
static JSValue g_active_ctor_map = JS_UNDEFINED;

/* §4.13.2 step 3: "if map[NewTarget] exists, set registry to it". OWNED; JS_UNDEFINED when there is no entry,
   which is the step's "otherwise". */
static JSValue ce_active_registry(JSContext *ctx, JSValueConst ctor)
{
    uint32_t n, i;

    DCHECK(JS_IsObject(g_active_ctor_map),
           "§4.13.4's active custom element constructor map was read before custom_elements_init built it");
    n = ce_array_len(ctx, g_active_ctor_map);
    for (i = n; i > 0; i--) {
        JSValue e = JS_GetPropertyUint32(ctx, g_active_ctor_map, i - 1);
        JSValue c = JS_GetPropertyUint32(ctx, e, 0);
        bool hit = JS_VALUE_GET_PTR(c) == JS_VALUE_GET_PTR(ctor);

        JS_FreeValue(ctx, c);
        if (hit) {
            JSValue r = JS_GetPropertyUint32(ctx, e, 1);
            JS_FreeValue(ctx, e);
            return r;
        }
        JS_FreeValue(ctx, e);
    }
    return JS_UNDEFINED;
}

/* §4.13.5 step 7.6 / DOM §4.9 step 5.1.3: "set map[C] to element's custom element registry". */
static void ce_active_push(JSContext *ctx, JSValueConst ctor, JSValueConst reg)
{
    JSValue e = JS_NewArray(ctx);

    CHECK(!JS_IsException(e), "an active custom element constructor map entry could not be allocated");
    JS_SetPropertyUint32(ctx, e, 0, JS_DupValue(ctx, ctor));
    JS_SetPropertyUint32(ctx, e, 1, JS_DupValue(ctx, reg));
    ce_array_push(ctx, g_active_ctor_map, e);
}

/* §4.13.5's cleanup: restore `previousRegistry`, or remove the entry when there was none. Dropping the last
   entry IS both, because the entry below it is the previous value. */
static void ce_active_pop(JSContext *ctx)
{
    uint32_t n = ce_array_len(ctx, g_active_ctor_map);

    DCHECK(n > 0, "§4.13.4's active custom element constructor map was restored with nothing on it — the set "
                  "and the restore are one pair and only §4.13.5 and DOM §4.9 step 5.1 write them");
    ce_array_set_len(ctx, g_active_ctor_map, n - 1);
}

/* "APPEND … TO THIS'S SCOPED DOCUMENT SET" — a SET, so an append of a document already in it is not a second
   entry: §4.13.4 step 17 walks it once per document and a duplicate would enqueue a second upgrade reaction
   for every candidate. The entries are Document WRAPPERS, which is what makes the set park with the flow that
   built it; a C list of `lxb_dom_document_t *` could name nothing across a session. */
static void ce_scoped_docs_append(JSContext *ctx, JSValueConst registry, lxb_dom_node_t *n)
{
    lxb_dom_document_t *doc = ce_node_document(n);
    JSValue docs, wrap;
    uint32_t len, i;

    if (!doc) return;
    docs = ce_reg_field(ctx, registry, g_atom_docs);
    wrap = node_wrap(ctx, lxb_dom_interface_node(doc));
    if (!JS_IsObject(wrap)) { JS_FreeValue(ctx, wrap); JS_FreeValue(ctx, docs); return; }
    len = ce_array_len(ctx, docs);
    for (i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, docs, i);
        bool same = JS_VALUE_GET_PTR(e) == JS_VALUE_GET_PTR(wrap);

        JS_FreeValue(ctx, e);
        if (same) { JS_FreeValue(ctx, wrap); JS_FreeValue(ctx, docs); return; }
    }
    ce_array_push(ctx, docs, wrap);
    JS_FreeValue(ctx, docs);
}

/* An element's own reaction queue, created on first use. OWNED by the caller. */
static JSValue ce_reaction_queue(JSContext *ctx, JSValueConst wrap, int create)
{
    JSValue q;

    if (JS_GetOwnSlot(ctx, &q, wrap, g_atom_rq) > 0) {
        if (JS_IsObject(q)) return q;
        JS_FreeValue(ctx, q);
    }
    if (!create) return JS_UNDEFINED;
    q = JS_NewArray(ctx);
    CHECK(!JS_IsException(q), "an element's custom element reaction queue could not be allocated");
    JS_SetProperty(ctx, q, g_atom_rq_head, JS_NewUint32(ctx, 0));
    JS_DefinePropertyValue(ctx, (JSValue)wrap, g_atom_rq, JS_DupValue(ctx, q), 0);
    return q;
}

/* §4.13.6 "enqueue an element on the appropriate element queue". The BRANCH is the whole algorithm: with a
   `[CEReactions]` member running the element joins THAT member's queue and its reactions run before the member
   returns; with none running it joins the backup queue and a microtask drains it. */
static void ce_enqueue_element(JSContext *ctx, JSValueConst wrap)
{
    if (g_current) {                                        /* step 3: the current element queue */
        if (JS_IsUndefined(g_current->queue)) {
            /* CREATED BY THE FIRST ENQUEUE, so a member that touches no custom element allocates nothing — and
               so the Array belongs to the running FLOW and its appends never reach the COW delta. */
            g_current->queue = JS_NewArray(ctx);
            CHECK(!JS_IsException(g_current->queue), "a §4.13.6 element queue could not be allocated");
            g_current->i = 0;
        }
        ce_array_push(ctx, g_current->queue, JS_DupValue(ctx, wrap));
        return;
    }
    /* step 2: the backup element queue, and a microtask to invoke it — queued once, which is what the flag is
       for. The microtask is an ordinary job, so the drain is a first-class flow like every other. */
    ce_array_push(ctx, g_ce_backup, JS_DupValue(ctx, wrap));
    {
        JSValue f = JS_GetProperty(ctx, g_ce_backup, g_atom_backup_flag);
        int set = JS_ToBool(ctx, f);
        JS_FreeValue(ctx, f);
        if (set) return;
        JS_SetProperty(ctx, g_ce_backup, g_atom_backup_flag, JS_TRUE);
    }
    /* THE DRIVER IS MINTED IN THE REALM THAT ENQUEUES IT, and that is not a tidy-up. A C function object
       CARRIES the realm it was defined in — js_call_c_function does `ctx = p->u.cfunc.realm` — so one built
       once at init and held in a module static drains every document's reactions with the AGENT'S FIRST
       realm's ctx, whichever document happened to run init. The backup queue itself is agent-wide and so is
       the step id (an int the runtime registered, with no realm in it); the FUNCTION OBJECT is the only part
       with a realm, which makes it the only part that must not be shared. CLAUDE.md §per-realm-fact names
       exactly this: mint it in the realm that uses it, per call, rather than holding one in a static. */
    DCHECK(g_backup_stepid >= 0,
           "a reaction reached the backup element queue before custom_elements_init registered its driver");
    {
        JSValue fn = JS_NewCFunction2(ctx, NULL, "backupElementQueue", 0, JS_CFUNC_step, g_backup_stepid);
        CHECK(!JS_IsException(fn), "the backup element queue's driver could not be allocated");
        JS_EnqueueCallJob(ctx, fn, 0, NULL);
        JS_FreeValue(ctx, fn);
    }
}

void custom_elements_reactions_push(CustomElementQueue *q)
{
    DCHECK(g_current == NULL, "a declared member began its steps while another member's element queue was "
                              "still current — a member parks by RETURNING, so nothing can run between the "
                              "push and the pop and this nesting cannot exist");
    g_current = q;
}

void custom_elements_reactions_pop(void)
{
    g_current = NULL;
}

void custom_elements_queue_init(CustomElementQueue *q)
{
    int k;
    q->queue = JS_UNDEFINED;
    q->i = 0;
    q->phase = 0;
    q->up_stage = 0;
    q->reporting = 0;
    q->exc = JS_UNDEFINED;
    q->cur = q->cur_el = JS_UNDEFINED;
    report_exception_work_start(&q->rep);
    for (k = 0; k < 2 + CE_MAX_REACTION_ARGS; k++) q->cb[k] = JS_UNDEFINED;
}

void custom_elements_queue_visit(JSContext *ctx, CustomElementQueue *q, JSStepVisit *v)
{
    int k;
    v->val(ctx, &q->queue);
    v->val(ctx, &q->exc);
    v->val(ctx, &q->cur);
    v->val(ctx, &q->cur_el);
    report_exception_work_visit(ctx, &q->rep, v);
    for (k = 0; k < 2 + CE_MAX_REACTION_ARGS; k++) v->val(ctx, &q->cb[k]);
}

void custom_elements_queue_unlock(JSContext *ctx, CustomElementQueue *q)
{
    q->reporting = 0;
    report_exception_work_unlock(ctx, &q->rep);
}

/* Which of §4.13.6 step 1.3.1's arms the drain last parked in — see custom_elements.h. */
int custom_elements_queue_arm(const CustomElementQueue *q)
{
    if (q->reporting) return CE_ARM_REPORT;
    return q->up_stage ? CE_ARM_UPGRADE : CE_ARM_CALLBACK;
}

/* The two enqueues §4.13.5 performs on the element it is upgrading, declared here because the upgrade runs
   above where they are defined — §4.13.6's drain is the only thing that can run §4.13.5, so the algorithm sits
   with the drain and the enqueues sit with the other reactions. */
static void ce_enqueue_args(JSContext *ctx, JSValueConst wrap, JSValueConst def, int callback,
                            int argc, JSValueConst *args);
static bool ce_observes(JSContext *ctx, JSValueConst def, const char *local);

/* ---- HTML §4.13.5 UPGRADE AN ELEMENT ----------------------------------------------------------------------
 *
 * THE ALGORITHM THAT RUNS THE PAGE'S CLASS. Everything else in this file exists to reach step 8.3, which
 * CONSTRUCTS the definition's constructor over the element already in the tree — so `class Router extends
 * HTMLElement { constructor(){ super(); this.routes = … } }` has its body executed on the node the parser
 * built, and `super()` hands back that same node because step 6 put it on the construction stack for
 * §4.13.2's steps 10-15 to find.
 *
 * IT IS A SUB-ALGORITHM OF THE DRAIN, NOT A MACHINE OF ITS OWN. §4.13.6 invokes it from inside the reaction
 * loop and CATCHES what it throws, so a separate step machine would need its own definition, its own stage
 * list and its own park protocol only to be driven by one caller that must inspect its completion — which is
 * what a two-stage cursor on the drain's own state already is. `up_stage` is that cursor; it is zero exactly
 * when no upgrade is in flight, which is what custom_elements_queue_arm reads.
 *
 * Returns JS_STEP_CONSTRUCT parked on the page's constructor, or 0 when the upgrade has finished — either
 * having set the element's state to "custom", or having set `q->reporting` for the throw the drain reports. */
#define CE_UP_IDLE      0
#define CE_UP_CONSTRUCT 1   /* §4.13.5 step 8.3, and step 8.4's SameValue on the way back */

static int ce_upgrade_run(JSContext *ctx, CustomElementQueue *q, JSValueConst el, JSValueConst def,
                          JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    lxb_dom_node_t *node = node_of(el);
    JSValue stack, made = JS_UNDEFINED;
    int r, failed;

    if (q->up_stage == CE_UP_IDLE) {
        lxb_dom_attr_t *a;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* step 1: an element that is not "undefined" or "uncustomized" has already been through this — the
           re-entrancy the spec's own example builds by removing and re-appending a sibling from inside a
           constructor, and equally an element whose upgrade FAILED, which must never be retried. */
        {
            int st = ce_state_of(ctx, el);
            if (st != CE_STATE_UNDEFINED && st != CE_STATE_UNCUSTOMIZED) return 0;
        }
        DCHECK(node && node->type == LXB_DOM_NODE_TYPE_ELEMENT,
               "an upgrade reaction was enqueued for something that is not an element");
        JS_DefinePropertyValue(ctx, (JSValue)el, g_atom_def, JS_DupValue(ctx, def), CE_SLOT_FLAGS);  /* step 2 */
        ce_set_state(ctx, el, CE_STATE_FAILED);                                           /* step 3 */
        /* step 4: EVERY attribute, in order, as an attributeChangedCallback whose old value is null. The
           observed-attributes filter is the enqueue's own (step 4 of "enqueue a custom element callback
           reaction"), so a class watching two attributes does not see the other fifty the parser wrote. */
        for (a = lxb_dom_element_first_attribute(lxb_dom_interface_element(node)); a;
             a = lxb_dom_element_next_attribute(a)) {
            size_t nlen = 0, vlen = 0, slen = 0;
            const lxb_char_t *nm = lxb_dom_attr_local_name(a, &nlen);
            const lxb_char_t *val = lxb_dom_attr_value(a, &vlen);
            const lxb_char_t *ns = dom_attr_ns(a, &slen);
            JSValue args[4];
            int k;

            if (!nm || !nlen) continue;
            args[0] = JS_NewStringLen(ctx, (const char *)nm, nlen);
            args[1] = JS_NULL;
            args[2] = val ? JS_NewStringLen(ctx, (const char *)val, vlen) : JS_NULL;
            args[3] = ns ? JS_NewStringLen(ctx, (const char *)ns, slen) : JS_NULL;
            ce_enqueue_args(ctx, el, def, CE_CB_ATTR_CHANGED, 4, (JSValueConst *)args);
            for (k = 0; k < 4; k++) JS_FreeValue(ctx, args[k]);
        }
        if (node_is_connected(node))                                                      /* step 5 */
            ce_enqueue_args(ctx, el, def, CE_CB_CONNECTED, 0, NULL);
        /* step 6: the element goes on the construction stack, which is how §4.13.2's `super()` returns THIS
           node instead of making a second one. */
        stack = JS_GetProperty(ctx, def, g_atom_stack);
        DCHECK(JS_IsObject(stack), "a custom element definition carries no §4.13.2 construction stack");
        ce_array_push(ctx, stack, JS_DupValue(ctx, el));
        JS_FreeValue(ctx, stack);
        /* steps 7.5-7.6: the agent's active custom element constructor map takes THIS ELEMENT'S registry for
           the duration of the construct, because that is the only way §4.13.2 step 3 can find a definition that
           lives in a SCOPED registry. The push is paired with the pop below, which runs whether or not the
           construction threw — the spec's "then, perform the following steps, regardless". */
        {
            JSValue ctor = JS_GetProperty(ctx, def, g_atom_ctor);
            JSValue reg = ce_registry_of_node(ctx, el);

            ce_active_push(ctx, ctor, reg);
            JS_FreeValue(ctx, reg);
            JS_FreeValue(ctx, ctor);
        }
        ce_set_state(ctx, el, CE_STATE_PRECUSTOMIZED);                                    /* step 8.2 */
        q->up_stage = CE_UP_CONSTRUCT;
    }
    DCHECK(q->up_stage == CE_UP_CONSTRUCT, "HTML §4.13.5 resumed into a stage it does not have");
    {
        JSValue ctor = JS_GetProperty(ctx, def, g_atom_ctor);
        /* step 8.3: constructing C with no arguments. The page's code, so it PARKS — and a throw comes back
           here as JS_EXCEPTION because every machine that drives this drain declares catches_abrupt, which is
           what §4.13.6's "catch it, and report it" requires of them. */
        r = step_construct_run(ctx, &q->phase, q->cb, 2 + CE_MAX_REACTION_ARGS, ctor, 0, NULL,
                               cb_result, &made, out_cb, out_argc);
        JS_FreeValue(ctx, ctor);
        if (r > 0) return r;
    }
    /* step 8.3's own abrupt completion arrives as JS_EXCEPTION (the drivers declare catches_abrupt) or as a
       synchronous -1; step 8.4 is SameValue(constructResult, element), which a constructor that returns a
       different element — or one that never called `super()`, so the marker never replaced the stack entry —
       fails. Both are "the steps threw", which is one condition and so is written as one. */
    failed = (r < 0 || JS_IsException(made));
    if (!failed && JS_VALUE_GET_PTR(made) != JS_VALUE_GET_PTR(el)) {
        JS_ThrowTypeError(ctx, "a custom element constructor returned an element other than the one being "
                               "upgraded");
        failed = 1;
    }
    if (failed) { JS_FreeValue(ctx, made); made = JS_UNDEFINED; }
    /* steps 8.6-8.7: previousRegistry goes back into the active custom element constructor map — the pop that
       the push at step 7.6 is one half of, run whether or not the construction threw. */
    ce_active_pop(ctx);
    /* step 9: the last entry comes off the stack whether the construction threw or not. */
    stack = JS_GetProperty(ctx, def, g_atom_stack);
    {
        uint32_t n = ce_array_len(ctx, stack);
        DCHECK(n > 0, "HTML §4.13.5 step 9 found an empty construction stack — step 6 pushed onto it and only "
                      "this line pops, so an empty one is an entry some other code removed");
        ce_array_set_len(ctx, stack, n - 1);
    }
    JS_FreeValue(ctx, stack);
    q->up_stage = CE_UP_IDLE;
    if (failed) {
        /* step 8's failure arm, and §4.13.6's catch. The state stays "failed" or "precustomized" — the spec
           says so explicitly — so a later insertion never retries this element. */
        JSValue rq;

        JS_DeleteProperty(ctx, (JSValue)el, g_atom_def, 0);            /* step 8.9.1 */
        rq = ce_reaction_queue(ctx, el, 0);                            /* step 8.9.2 */
        if (JS_IsObject(rq)) {
            ce_array_set_len(ctx, rq, 0);
            JS_SetProperty(ctx, rq, g_atom_rq_head, JS_NewUint32(ctx, 0));
        }
        JS_FreeValue(ctx, rq);
        q->exc = JS_GetException(ctx);
        q->reporting = 1;
        return 0;
    }
    JS_FreeValue(ctx, made);
    /* Step 10: a form-associated custom element gets its form owner reset here, with a formAssociatedCallback
       for the form it lands on and a formDisabledCallback when it is disabled — both ENQUEUED, so they run in
       this same drain after the constructor. The algorithm belongs to §4.13.7's component because the owner and
       the disabled question are the FORM layer's; the reaction is this one's, which is the one line joining
       them. */
    element_internals_upgrade_form_steps(ctx, el);
    ce_set_state(ctx, el, CE_STATE_CUSTOM);   /* step 11 */
    return 0;
}

/* §4.13.6 "invoke custom element reactions in an element queue", one reaction per entry.
   THE POP HAPPENS FIRST AND IT IS OBSERVABLE: step 3 removes the queue from the stack BEFORE step 4 invokes it,
   so a reaction that itself mutates the DOM enqueues onto whatever is on the stack THEN — an outer
   `[CEReactions]` member's queue, or the backup queue — and never back onto the one being drained.
   Returns JS_STEP_CALL / JS_STEP_CONSTRUCT parked on one reaction (the caller returns it), or 0 when the queue
   is exhausted. */
int custom_elements_reactions_invoke(JSContext *ctx, CustomElementQueue *q, JSValue cb_result,
                                     JSValue **out_cb, int *out_argc)
{
    /* STEP 3, HERE, BECAUSE IT IS OBSERVABLE AND IT WAS ONLY CLAIMED. This said "step 3 already happened: the
       queue stopped being current the moment the member's own steps returned" — true of a member that PARKS,
       and false of the straight-line case, because js_idl_args_step makes the queue current for the whole of
       its activation and the epilogue runs inside that. So a reaction enqueued BY the drain (§4.13.5 step 4's
       attributeChangedCallbacks, step 5's connectedCallback) went back onto the very queue being drained
       instead of the backup one. Popping here makes the sentence true: for the rest of this drain no queue is
       current, which is exactly what §4.13.6 step 3 means and what sends a drain-time enqueue to the backup
       queue's microtask. The element's OWN reaction queue still receives the reaction, and step 1.3's
       "repeat until reactions is empty" is what runs it in this same drain.
       An empty queue is the overwhelmingly common case — a member that touched no custom element never
       allocated one — and the pop is right for it too. */
    custom_elements_reactions_pop();
    if (!g_ready || JS_IsUndefined(q->queue)) { JS_FreeValue(ctx, cb_result); return 0; }
    for (;;) {
        JSValue target;
        int nargs, k, r, type;
        JSValue args[CE_MAX_REACTION_ARGS], ignored;

        /* §4.13.6 step 1.3.1's report, RESUMED FIRST — it parks inside the `error` event's own dispatch, so a
           re-entry lands here before anything recomputes the cursor. */
        if (q->reporting) {
            r = report_exception_run(ctx, &q->rep, q->exc, cb_result, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r > 0) return r;
            q->reporting = 0;
            JS_FreeValue(ctx, q->exc);
            q->exc = JS_UNDEFINED;
        }
        /* STEP 1.3'S REMOVAL HAPPENS BEFORE THE REACTION RUNS, which is what the spec says and what a
           re-entrant drain requires. The reaction and its element are then held on this state across the park,
           so the resume continues the one whose answer just arrived without re-reading the element's queue —
           and a NESTED drain that dequeues the SAME element (a `[CEReactions]` member called from inside a
           constructor) finds the head already past it and can neither re-run it nor skip past a live one. */
        if (!JS_IsObject(q->cur)) {
            uint32_t n = ce_array_len(ctx, q->queue), head = 0, rn;
            JSValue el, rq, head_v;

            if (q->i >= n) {                              /* step 1: the queue is empty */
                /* EMPTIED, not merely walked past. A member's queue is flow-private and dies with the machine,
                   but the BACKUP queue is the agent's one array forever — an element left on it after its
                   reactions are consumed is a reference nothing ever drops. */
                DCHECK(q->up_stage == CE_UP_IDLE,
                       "the drain finished its element queue while an upgrade was still in flight — §4.13.5 "
                       "completes or reports before the reaction that started it is released");
                ce_array_set_len(ctx, q->queue, 0);
                JS_FreeValue(ctx, cb_result);
                JS_FreeValue(ctx, q->queue);
                q->queue = JS_UNDEFINED;
                q->i = 0;
                return 0;
            }
            el = JS_GetPropertyUint32(ctx, q->queue, q->i);   /* step 1.1: dequeue element */
            rq = ce_reaction_queue(ctx, el, 0);               /* step 1.2: its reaction queue */
            head_v = JS_IsObject(rq) ? JS_GetProperty(ctx, rq, g_atom_rq_head) : JS_UNDEFINED;
            JS_ToUint32(ctx, &head, head_v);
            JS_FreeValue(ctx, head_v);
            rn = JS_IsObject(rq) ? ce_array_len(ctx, rq) : 0;
            if (head >= rn) {                             /* this element's reactions are exhausted */
                if (JS_IsObject(rq)) {                    /* the removal §4.13.6 performs, as one truncation */
                    ce_array_set_len(ctx, rq, 0);
                    JS_SetProperty(ctx, rq, g_atom_rq_head, JS_NewUint32(ctx, 0));
                }
                JS_FreeValue(ctx, rq);
                JS_FreeValue(ctx, el);
                q->i++;
                continue;
            }
            q->cur = JS_GetPropertyUint32(ctx, rq, head);
            q->cur_el = el;                               /* the dequeue's reference, handed over */
            JS_SetProperty(ctx, rq, g_atom_rq_head, JS_NewUint32(ctx, head + 1));   /* removed, now */
            JS_FreeValue(ctx, rq);
            DCHECK(JS_IsObject(q->cur), "an element's reaction queue holds something that is not a reaction");
        }
        {
            JSValue tv = JS_GetPropertyUint32(ctx, q->cur, 0);
            type = -1;
            JS_ToInt32(ctx, &type, tv);
            JS_FreeValue(ctx, tv);
        }
        target = JS_GetPropertyUint32(ctx, q->cur, 1);
        if (type == CE_REACTION_UPGRADE) {
            /* step 1.3.1's upgrade arm. The definition is the reaction's, not the name's current entry — a
               `define` that replaced nothing still has the definition this reaction was made from. */
            r = ce_upgrade_run(ctx, q, q->cur_el, target, cb_result, out_cb, out_argc);
            JS_FreeValue(ctx, target);
            cb_result = JS_UNDEFINED;
            if (r > 0) return r;
            DCHECK(r == 0, "HTML §4.13.5 answered the drain with something that is neither a park nor a "
                           "completion — its throw is reported, so it has no abrupt answer to give");
        } else {
            DCHECK(type == CE_REACTION_CALLBACK,
                   "an element's reaction queue holds a reaction of a type §4.13.6 does not switch on");
            nargs = (int)ce_array_len(ctx, q->cur) - 2;
            DCHECK(nargs >= 0 && nargs <= CE_MAX_REACTION_ARGS,
                   "a lifecycle callback reaction carries more arguments than any of them takes");
            for (k = 0; k < nargs; k++) args[k] = JS_GetPropertyUint32(ctx, q->cur, (uint32_t)(k + 2));
            /* step 1.3.1: invoke the callback function with its arguments and "report", this = element. */
            r = step_call_run(ctx, &q->phase, q->cb, 2 + CE_MAX_REACTION_ARGS, target, q->cur_el, nargs,
                              (JSValueConst *)args, cb_result, &ignored, out_cb, out_argc);
            for (k = 0; k < nargs; k++) JS_FreeValue(ctx, args[k]);
            JS_FreeValue(ctx, target);
            if (r > 0) return JS_STEP_CALL;               /* parked on the page's code */
            /* "and \"report\"": a lifecycle callback that throws is reported and the drain goes on, exactly as
               a throwing event listener is. Without this the throw tore down the member that was draining. */
            if (JS_IsException(ignored)) {
                ignored = JS_UNDEFINED;
                q->exc = JS_GetException(ctx);
                q->reporting = 1;
            }
            JS_FreeValue(ctx, ignored);                   /* §4.13.3: a reaction's return value is discarded */
            cb_result = JS_UNDEFINED;
        }
        JS_FreeValue(ctx, q->cur);
        JS_FreeValue(ctx, q->cur_el);
        q->cur = q->cur_el = JS_UNDEFINED;
    }
}

/* THE BACKUP QUEUE'S MICROTASK — §4.13.6 step 2.4, and the ONE place a reaction runs when no `[CEReactions]`
   member is on the stack (the parser's own mutations, an engine-driven insertion). It is the same invoke over
   a queue that was never on the stack, so it takes the same machine with the queue handed to it directly. */
/* THREE STAGES AND NOT ONE, because the drain rests at three DIFFERENT spec steps and a resume point that
   cannot say which is a resume point that means three things. §4.13.6 step 1.3.1 SWITCHES on the reaction's
   type — a callback reaction parks inside a lifecycle callback, an upgrade reaction parks inside §4.13.5 step
   8.3's Construct — and a reaction that threw parks inside HTML §8.1.4.6's `error` event. The stage is set from
   custom_elements_queue_arm, so the three cannot drift from the three arms. */
#define CE_BACKUP_STAGES(X) \
    X(CEBACKUP_CALLBACK, "HTML §4.13.6 invoke custom element reactions step 1.3.1, callback reaction (invoke " \
                         "the reaction's callback function with \"report\"), one reaction per step") \
    X(CEBACKUP_UPGRADE,  "HTML §4.13.6 invoke custom element reactions step 1.3.1, upgrade reaction — HTML " \
                         "§4.13.5 step 8.3 (constructing the definition's constructor with no arguments)") \
    X(CEBACKUP_REPORT,   "HTML §4.13.6 invoke custom element reactions step 1.3.1 (reporting the exception a " \
                         "reaction threw), which is HTML §8.1.4.6 report an exception")
enum { CE_BACKUP_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const CE_BACKUP_STEPS[] = { CE_BACKUP_STAGES(JS_STEP_STAGE_LABEL) NULL };
/* The three stages ARE the three arms, in the arm's own order — asserted rather than trusted, because they are
   two enumerations of one thing written in two files. */
typedef char ce_backup_stages_match_arms[
    (CEBACKUP_CALLBACK == CE_ARM_CALLBACK && CEBACKUP_UPGRADE == CE_ARM_UPGRADE &&
     CEBACKUP_REPORT == CE_ARM_REPORT) ? 1 : -1];

typedef struct JSCeBackup {
    JSStepHdr          hdr;    /* FIRST — the driver writes the def and the operand bounds through it */
    /* HAVE THIS STATE'S OWNED FIELDS BEEN PLACED YET. tramp_step_state_new js_mallocz's a machine's state, and
       a ZEROED JSValue is the INTEGER 0 — JS_TAG_INT is 0 — so every value on a fresh state reads as
       "already set" and JS_IsUndefined answers false for all of them. This drain read its "have I taken the
       backup queue yet" off `q.queue`, so it never took it: `custom_elements_reactions_invoke` was handed the
       integer 0 as the element queue, `ce_array_len` answered 0 for it, and the drain returned having invoked
       NOTHING. Every reaction that reaches the backup queue — which is every mutation performed outside a
       `[CEReactions]` member, so the parser's own tree construction and every engine-driven insertion — was
       enqueued and never run, with no throw and no assert to say so.
       The stage cannot answer this the way js_idl_args_step's does: this machine's first stage IS 0. */
    uint8_t            started;
    CustomElementQueue q;
} JSCeBackup;

static void js_ce_backup_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSCeBackup *s = st;
    custom_elements_queue_visit(ctx, &s->q, v);
}

static JSValue js_ce_backup_fini(JSContext *ctx, void *st, bool take_result)
{
    JSCeBackup *s = st;
    (void)take_result;
    /* THE LOCK ONLY. The queue's reactions are references js_ce_backup_visit names and the teardown discharges
       that list; what no declaration can carry is §8.1.4.6 step 5's error-reporting flag, which a backup queue
       abandoned mid-drain would otherwise leave raised on the global forever. */
    custom_elements_queue_unlock(ctx, &s->q);
    return JS_UNDEFINED;
}

static int js_ce_backup_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSCeBackup *s = st;
    int r;

    DCHECK(s->hdr.stage >= CEBACKUP_CALLBACK && s->hdr.stage <= CEBACKUP_REPORT,
           "the backup element queue's drain resumed into a stage §4.13.6 does not have");
    if (!s->started) {
        /* Every owned field placed before the first thing that can fail, which is what the failure path's
           `fini` frees — the same contract js_idl_args_step keeps for the queue it embeds. */
        custom_elements_queue_init(&s->q);
        s->started = 1;
    }
    if (JS_IsUndefined(s->q.queue) && s->q.phase == 0 && !s->q.reporting) {
        /* THE FLAG IS UNSET AS THE DRAIN BEGINS (step 2.4's second half), so a reaction that runs during it and
           enqueues with no member on the stack schedules a NEW microtask rather than joining the batch in
           flight. The ARRAY is not replaced: it is the agent's, held in a C static that no COW delta captures,
           so swapping the static would make one flow's replacement visible to every other. The drain walks it
           by cursor and empties it at the end, which the delta does capture. */
        s->q.queue = JS_DupValue(ctx, g_ce_backup);
        s->q.i = 0;
        JS_SetProperty(ctx, g_ce_backup, g_atom_backup_flag, JS_FALSE);
    }
    r = custom_elements_reactions_invoke(ctx, &s->q, cb_result, out_cb, out_argc);
    /* THE STAGE IS THE ARM THE DRAIN PARKED IN — set before returning the park, because the stage a machine
       leaves behind is the one a cold-tier resume will report. */
    s->hdr.stage = (uint8_t)custom_elements_queue_arm(&s->q);
    return r ? r : JS_STEP_DONE;
}

static const JSTrampStepDef js_ce_backup_def = {
    sizeof(JSCeBackup), js_ce_backup_step, js_ce_backup_fini, 0,
    /* §4.13.6 step 1.3.1 CATCHES what an upgrade reaction throws and reports it, and "invoke … with \"report\""
       says the same for a callback reaction. Both are this drain's own VALUE, so the abrupt completion of the
       call or the construct is delivered back to step() rather than tearing the drain down — without which one
       throwing constructor would silently drop every reaction queued behind it. */
    .catches_abrupt = 1, .visit = js_ce_backup_visit,
    .algorithm = "HTML §4.13.6 invoke custom element reactions in the backup element queue",
    .steps = CE_BACKUP_STEPS
};

/* §4.13.5 step 2's definition, read off the element's OWN slot — no prototype lookup and no page code. UNDEFINED
   for an element that has not been upgraded, which is what "custom element definition is null" means. OWNED. */
static JSValue ce_definition_of(JSContext *ctx, JSValueConst wrap)
{
    JSValue v;

    if (!JS_IsObject(wrap)) return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &v, wrap, g_atom_def) <= 0) return JS_UNDEFINED;
    return v;
}

JSValue custom_elements_definition_lookup_for_element(JSContext *ctx, JSValueConst el_wrap)
{
    lxb_dom_node_t *n = node_of(el_wrap);
    size_t len = 0;
    const lxb_char_t *tag;

    DCHECK(g_ready, "a custom element definition was looked up before custom_elements_init ran");
    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return JS_UNDEFINED;
    /* §4.13.3 step 2: not the HTML namespace, no definition. */
    if (n->ns != LXB_NS_HTML) return JS_UNDEFINED;
    tag = lxb_dom_element_local_name(lxb_dom_interface_element(n), &len);
    if (!tag || !len) return JS_UNDEFINED;
    return ce_find_for_node(ctx, el_wrap, (const char *)tag, len);
}

bool custom_elements_definition_flag(JSContext *ctx, JSValueConst def, CustomElementDefinitionFlag which)
{
    JSValue v;
    int32_t bits = 0;

    if (!JS_IsObject(def)) return false;
    v = JS_GetProperty(ctx, def, g_atom_flags);
    DCHECK(JS_IsNumber(v), "a custom element definition carries no §4.13.4 step 15 flags word — every "
                           "definition this component commits writes one, so a missing word is a definition "
                           "it did not make");
    JS_ToInt32(ctx, &bits, v);
    JS_FreeValue(ctx, v);
    return (bits & (1 << (int)which)) != 0;
}

JSValue custom_elements_definition_of_element(JSContext *ctx, JSValueConst wrap)
{
    DCHECK(g_ready, "an element's custom element definition was asked for before custom_elements_init ran");
    return ce_definition_of(ctx, wrap);
}

int custom_elements_state_of_element(JSContext *ctx, JSValueConst wrap)
{
    DCHECK(g_ready, "an element's custom element state was asked for before custom_elements_init ran");
    return ce_state_of(ctx, wrap);
}

bool custom_elements_is_form_associated(JSContext *ctx, JSValueConst wrap)
{
    JSValue def = custom_elements_definition_of_element(ctx, wrap);
    bool r = custom_elements_definition_flag(ctx, def, CE_DEF_FORM_ASSOCIATED);

    JS_FreeValue(ctx, def);
    return r;
}

JSValue custom_elements_definition_constructor(JSContext *ctx, JSValueConst def)
{
    DCHECK(JS_IsObject(def), "a custom element definition's constructor was asked for on something that is not "
                             "a definition");
    return JS_GetProperty(ctx, def, g_atom_ctor);
}

/* DOM §4.9 STEPS 5.1.4.2-11 — what the page's constructor gave back, checked against what the operation asked
   for. Every one of these is a real page pattern and each has its own subtest: `return {foo:'bar'}` (not a
   node at all), `return document.createTextNode('hi')` (a node of the wrong kind), `super(); this.setAttribute
   ('id','foo')` (an element with an attribute), `super(); this.appendChild(…)` (one with a child).
   STEP 5.1.4.2 IS AN ASSERT AND IT IS ONE HERE TOO, which it was not: it was a TypeError, and that was a
   WRONG ANSWER for the spec's own worked example. The step permits three shapes — "custom" with a non-null
   definition, "precustomized", or neither with a NULL definition — and a constructor that returns
   `document.createElement("p")` takes the THIRD (an uncustomized element with no definition), so the assert
   HOLDS and the algorithm goes on to 5.1.4.8, where the local name differs and a NotSupportedError is thrown.
   Raising a TypeError at 5.1.4.2 answered that case with the wrong error and the wrong class of error. Only a
   UA bug can violate the assert, so it is a DCHECK — which is exactly why it needed the STATE to be a real
   five-valued field before it could be written down at all. */
int custom_elements_created_check(JSContext *ctx, JSValueConst result,
                                  lxb_dom_document_t *doc, const char *local, size_t len)
{
    lxb_dom_node_t *n = node_of(result);
    lxb_dom_element_t *el;

    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        JS_ThrowTypeError(ctx, "a custom element constructor returned something that is not an element");
        return -1;
    }
    {                                              /* step 5.1.4.2, as the assert it is */
        JSValue has = ce_definition_of(ctx, result);
        int st = ce_state_of(ctx, result);
        bool ok = (st == CE_STATE_CUSTOM && JS_IsObject(has)) ||
                  st == CE_STATE_PRECUSTOMIZED ||
                  (st != CE_STATE_CUSTOM && st != CE_STATE_PRECUSTOMIZED && !JS_IsObject(has));
        JS_FreeValue(ctx, has);
        DCHECK(ok, "DOM §4.9 step 5.1.4.2's assert failed — an element's custom element state and its custom "
                   "element definition disagree, which only §4.13.2 and §4.13.5 write, so one of them left a "
                   "state it does not have a definition for");
    }
    el = lxb_dom_interface_element(n);
    if (lxb_dom_element_first_attribute(el)) {     /* step 5.1.4.4 */
        JS_ThrowDOMException(ctx, "NotSupportedError",
                             "a custom element constructor added an attribute to the element it was building");
        return -1;
    }
    if (n->first_child) {                          /* step 5.1.4.5 */
        JS_ThrowDOMException(ctx, "NotSupportedError",
                             "a custom element constructor gave the element it was building a child");
        return -1;
    }
    if (n->parent) {                               /* step 5.1.4.6 */
        JS_ThrowDOMException(ctx, "NotSupportedError",
                             "a custom element constructor inserted the element it was building into a tree");
        return -1;
    }
    if (doc && n->owner_document != doc) {         /* step 5.1.4.7 */
        JS_ThrowDOMException(ctx, "NotSupportedError",
                             "a custom element constructor returned an element of another document");
        return -1;
    }
    {                                              /* step 5.1.4.8 */
        size_t got = 0;
        const lxb_char_t *tag = lxb_dom_element_local_name(el, &got);
        if (!tag || got != len || memcmp(tag, local, len) != 0) {
            JS_ThrowDOMException(ctx, "NotSupportedError",
                                 "a custom element constructor returned an element of another local name");
            return -1;
        }
    }
    /* Steps 5.1.4.9-10 set the namespace prefix and the is value: the prefix is already null (the creation
       passed none) and the is value is null for an autonomous element, so both are already what the steps set.
       STEP 5.1.4.11 IS "set result's custom element registry to REGISTRY" and it is a real write now. The
       registry is the one the construct ran under, which is exactly what the agent's active custom element
       constructor map holds for this constructor — so the element the page's constructor returned is
       associated with the registry that defined its class rather than with whatever its tree implies. With no
       entry (the creation was performed against the document's registry) the derivation already answers that,
       and stamping it would be writing the value that is there. */
    {
        JSValue def = ce_definition_of(ctx, result);

        if (JS_IsObject(def)) {
            JSValue ctor = JS_GetProperty(ctx, def, g_atom_ctor);
            JSValue reg = ce_active_registry(ctx, ctor);

            if (JS_IsObject(reg)) ce_node_set_registry(ctx, result, reg);
            JS_FreeValue(ctx, reg);
            JS_FreeValue(ctx, ctor);
        }
        JS_FreeValue(ctx, def);
    }
    return 0;
}

/* ---- HTML §4.13.2 the [HTMLConstructor] extended attribute ------------------------------------------------
 *
 * WHY THIS IS THE PIECE EVERYTHING ELSE WAITED FOR. A page defines a component by writing
 * `class Router extends HTMLElement { constructor() { super(); … } }`, and every DOM interface object in this
 * engine shared one body that threw "Illegal constructor" — so `super()` threw, so the class could not be
 * constructed, so `document.createElement('x-router')` could not construct it and the UPGRADE could not
 * either. The whole of §4.13's lifecycle hangs off a constructor that could not run: the reactions corpus
 * opens every one of its ~290 subtests with assert_array_equals(log.types(), ['constructed']) and got [].
 *
 * IT IS A STEP MACHINE BECAUSE OF ONE READ. Step 8 is `Get(NewTarget, "prototype")` — off the page's class,
 * which may be a Proxy or carry an accessor, so it is the page's code running in the middle of a constructor.
 * Web IDL §3.7.10's "internally create a new object implementing the interface", which step 7.1 delegates to,
 * makes the SAME read. One stage rests there and the algorithm's two arms continue from it.
 *
 * THE CONSTRUCTION STACK IS THE WHOLE MECHANISM, and it is what makes the two ways a custom element comes into
 * existence ONE algorithm. Reached with an EMPTY stack (`new Router()`, and DOM §4.9 step 5.1.4.1's Construct
 * inside createElement) the constructor MAKES the element. Reached with a NON-EMPTY one (§4.13.5's upgrade
 * pushed the already-parsed node before constructing) it hands back the node the page already holds, so
 * identity survives the upgrade — and it REPLACES that entry with an already-constructed marker, which is how
 * a constructor that calls itself a second time gets an InvalidStateError instead of a second element. */
#define HC_STAGES(X) \
    X(HC_LOOKUP,    "HTML §4.13.2 steps 1-6 (the registry; NewTarget is not the active function object; the " \
                    "definition whose constructor is NewTarget; autonomous vs customized built-in)") \
    X(HC_PROTOTYPE, "HTML §4.13.2 step 8, and Web IDL §3.7.10's same read inside step 7.1 " \
                    "(Get(NewTarget, \"prototype\"))") \
    X(HC_FINISH,    "HTML §4.13.2 steps 7.2-7.9 (a fresh element, for an empty construction stack) or steps " \
                    "9-15 (the stack's last entry, its prototype, and the already-constructed marker)")
enum { IDL_STEP_STAGE_BASE(HC_STAGES) HC_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const HC_STEPS[] = { HC_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSValue registry; /* steps 2-3's registry (owned) — step 7.6 writes it onto the element it builds */
    JSValue def;      /* step 4's definition (owned) */
    JSValue proto;    /* step 8's answer (owned) */
} CeHtmlCtorState;

static void ce_html_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    CeHtmlCtorState *s = st;
    v->val(ctx, &s->registry);
    v->val(ctx, &s->def);
    v->val(ctx, &s->proto);
}

/* §4.13.2 step 4: the entry in REGISTRY's definition set whose CONSTRUCTOR is `ctor`. A walk of the ordered
   set, which is what the spec's own wording is; the name-keyed index cannot answer this question at all.
   UNDEFINED when there is none, which is step 4's TypeError. OWNED. */
static JSValue ce_definition_by_ctor(JSContext *ctx, JSValueConst registry, JSValueConst ctor)
{
    JSValue list, found = JS_UNDEFINED;
    uint32_t n, i;

    if (!JS_IsObject(registry)) return JS_UNDEFINED;
    list = ce_reg_field(ctx, registry, g_atom_order);

    n = ce_array_len(ctx, list);
    for (i = 0; i < n && !JS_IsObject(found); i++) {
        JSValue def = JS_GetPropertyUint32(ctx, list, i);
        JSValue c = JS_GetProperty(ctx, def, g_atom_ctor);
        if (JS_VALUE_GET_PTR(c) == JS_VALUE_GET_PTR(ctor) && JS_IsObject(c)) found = def;
        else JS_FreeValue(ctx, def);
        JS_FreeValue(ctx, c);
    }
    JS_FreeValue(ctx, list);
    return found;
}

/* §4.13.2 step 13's ALREADY CONSTRUCTED MARKER. It is a distinct value from an element, and `true` is the one
   thing the stack can hold that no element ever is — the stack's entries are element wrappers, which are
   objects. Step 11 tests for it and throws InvalidStateError, which is what a constructor calling its own
   class a second time inside itself must see. */
static bool ce_is_already_constructed(JSValueConst v) { return JS_IsBool(v); }

static int js_ce_html_ctor(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                           JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    CeHtmlCtorState *s = st;
    JSValueConst ntgt = hdr->this_val;   /* JS_CFUNC_step_ctor delivers NEW TARGET in the receiver slot */
    int r;

    (void)argc; (void)argv;
    if (hdr->stage == HC_LOOKUP) {
        JSValue active;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->registry = s->def = s->proto = JS_UNDEFINED;
        /* Web IDL: an interface object is not callable. `HTMLElement()` with no `new` is a TypeError before
           §4.13.2 step 1, which JS_CFUNC_step_ctor states by delivering an UNDEFINED receiver. */
        if (JS_IsUndefined(ntgt)) {
            JS_ThrowTypeError(ctx, "constructor HTMLElement requires 'new'");
            return -1;
        }
        /* step 2: NewTarget must not be the active function object. `new HTMLElement()` builds nothing — the
           element a custom element constructor produces is the DEFINITION's, and there is no definition whose
           constructor is HTMLElement itself. */
        active = realm_value_get(ctx, g_html_ctor_slot);
        DCHECK(JS_IsObject(active), "HTML §4.13.2 ran in a realm whose HTMLElement interface object was never "
                                    "recorded — step 5's \"the active function object is HTMLElement\" is an "
                                    "identity question and there is nothing to compare against");
        if (JS_VALUE_GET_PTR(ntgt) == JS_VALUE_GET_PTR(active)) {
            JS_FreeValue(ctx, active);
            JS_ThrowTypeError(ctx, "Illegal constructor");
            return -1;
        }
        DCHECK(JS_VALUE_GET_PTR(hdr->func_obj) == JS_VALUE_GET_PTR(active),
               "HTML §4.13.2 ran with an active function object that is not HTMLElement — the customized "
               "built-in half of steps 5-6 needs the interface's valid local names, and §4.13.4 refuses "
               "`extends` until it exists, so no other interface may carry this machine yet");
        JS_FreeValue(ctx, active);
        /* STEPS 2-4: THE REGISTRY FIRST, AND THE DEFINITION OUT OF IT. Step 2 is the agent's active custom
           element constructor map — set by §4.13.5 step 7.6 while an upgrade is constructing, and by DOM §4.9
           step 5.1.3 while `create an element` is — and step 3 falls back to "the current global object's
           associated Document's custom element registry" for a bare `new Router()`. Reading the document's set
           unconditionally, which is what this did, made every class registered in a SCOPED registry throw a
           TypeError from its own `super()`: the definition is real, it is just not in the set that was asked. */
        {
            JSValue reg = ce_active_registry(ctx, ntgt);                          /* step 2 */

            if (!JS_IsObject(reg)) {
                JS_FreeValue(ctx, reg);
                reg = ce_document_registry(ctx);                                  /* step 3 */
            }
            s->registry = reg;
        }
        s->def = ce_definition_by_ctor(ctx, s->registry, ntgt);                   /* step 4 */
        if (!JS_IsObject(s->def)) {
            JS_ThrowTypeError(ctx, "this constructor is not a defined custom element constructor");
            return -1;
        }
        /* steps 5-6: autonomous (name == local name) requires the active function object to be HTMLElement,
           which the assert above already established. A definition whose two names DIFFER is a customized
           built-in, and §4.13.4 refuses to make one, so reaching here with one is a definition this component
           did not commit. */
        {
            JSValue nm = JS_GetProperty(ctx, s->def, g_atom_name);
            JSValue lo = JS_GetProperty(ctx, s->def, g_atom_local);
            bool autonomous = JS_VALUE_GET_PTR(nm) == JS_VALUE_GET_PTR(lo) || JS_IsUndefined(lo);
            JS_FreeValue(ctx, nm);
            JS_FreeValue(ctx, lo);
            DCHECK(autonomous, "HTML §4.13.2 reached a definition whose name and local name differ — a "
                               "customized built-in, which §4.13.4 does not register");
        }
        hdr->stage = HC_PROTOTYPE;
    }
    if (hdr->stage == HC_PROTOTYPE) {
        /* step 8 / Web IDL §3.7.10: the page's class may be a Proxy, so this is a request and not a read. */
        r = step_getprop_run(ctx, hdr, ntgt, g_atom_prototype, cb_result, &s->proto, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return -1;
        hdr->stage = HC_FINISH;
    }
    DCHECK(hdr->stage == HC_FINISH, "HTML §4.13.2 resumed into a stage it does not have");
    JS_FreeValue(ctx, cb_result);
    /* step 9: a prototype that is not an Object is the interface prototype object of NewTarget's function
       realm. HTMLElement's own is this realm's, which is what a cross-realm NewTarget makes wrong and what
       makes that a `[S]`-free but realm-sensitive read — the same question §3.7 answers everywhere else. */
    if (!JS_IsObject(s->proto)) {
        JS_FreeValue(ctx, s->proto);
        s->proto = html_element_proto(ctx);
    }
    {
        JSValue stack = JS_GetProperty(ctx, s->def, g_atom_stack);
        uint32_t n = ce_array_len(ctx, stack);
        JSValue el;

        DCHECK(JS_IsObject(stack), "a custom element definition carries no §4.13.2 construction stack");
        if (n == 0) {
            /* steps 7.1-7.9: the constructor MAKES the element. Its local name is the definition's, its
               document is the CURRENT GLOBAL's (step 7.2, not any receiver's), its state is "custom" and its
               definition is this one — which is what the definition slot on the wrapper means. */
            JSValue lo = JS_GetProperty(ctx, s->def, g_atom_local);
            size_t len = 0;
            const char *local = JS_ToCStringLen(ctx, &len, lo);

            JS_FreeValue(ctx, lo);
            JS_FreeValue(ctx, stack);
            if (!local) return -1;
            el = document_create_element_internal(ctx, local, len);
            JS_FreeCString(ctx, local);
            if (JS_IsException(el)) return -1;
            JS_SetPrototype(ctx, el, s->proto);
            /* step 7.6: "set element's custom element registry to registry" — the one steps 2-3 resolved, so an
               element built by `new Router()` where Router lives in a scoped registry carries THAT registry and
               keeps answering out of it for every later lookup. */
            ce_node_set_registry(ctx, el, s->registry);
            /* steps 7.7-7.8: custom element state "custom" and the definition. Both, and in that order — the
               state is what DOM §4.9 step 5.1.4's assert reads back and what a later insertion branches on. */
            ce_set_state(ctx, el, CE_STATE_CUSTOM);
            JS_DefinePropertyValue(ctx, el, g_atom_def, JS_DupValue(ctx, s->def), CE_SLOT_FLAGS);
            *presult = el;
            return 0;
        }
        /* steps 10-15: the element §4.13.5 pushed. */
        el = JS_GetPropertyUint32(ctx, stack, n - 1);
        if (ce_is_already_constructed(el)) {          /* step 11 */
            JS_FreeValue(ctx, el);
            JS_FreeValue(ctx, stack);
            /* A TypeError, which is what §4.13.2 step 11 says and what the corpus asserts — it was an
               InvalidStateError, a DOMException a page's `catch (e) { e instanceof TypeError }` answers false
               for. The two shapes that reach it are a constructor that news its own class before `super()` and
               one that calls `super()` twice. */
            JS_ThrowTypeError(ctx, "this custom element constructor already ran for the element being upgraded");
            return -1;
        }
        JS_SetPrototype(ctx, el, s->proto);                            /* step 12 */
        JS_SetPropertyUint32(ctx, stack, n - 1, JS_TRUE);              /* step 13: the marker */
        JS_FreeValue(ctx, stack);
        *presult = el;
        return 0;
    }
}

static const IdlStepDecl CE_HTML_CTOR_STEP = {
    js_ce_html_ctor, sizeof(CeHtmlCtorState), ce_html_ctor_visit, NULL,
    "HTML §4.13.2 the HTMLElement constructor", HC_STEPS
};
static int g_id_html_ctor = -1;

JSValue custom_elements_html_constructor(JSContext *ctx)
{
    JSValue ctor;

    DCHECK(g_ready, "HTMLElement's interface object was minted before custom_elements_init declared §4.13.2");
    ctor = idl_step_constructor(ctx, "HTMLElement", 0, g_id_html_ctor);
    CHECK(!JS_IsException(ctor), "the HTMLElement interface object could not be allocated");
    /* §4.13.2 step 5's IDENTITY, recorded for THIS realm as the object is made. Asking the global for
       `HTMLElement` instead would read a property the page can reassign, and `window.HTMLElement = X` must not
       change which constructor is legal to extend. */
    realm_value_set(ctx, g_html_ctor_slot, JS_DupValue(ctx, ctor));
    return ctor;
}

/* §4.13.6 "enqueue a custom element callback reaction", steps 1-3 and 5-6. The callback is the one step 14.4
   COLLECTED into this element's definition, and step 3 returns without a reaction when it is null — which is
   why a class that declares no `disconnectedCallback` costs nothing at every removal.
   STEP 5 ADDS IT TO THE ELEMENT'S OWN REACTION QUEUE and step 6 puts the ELEMENT on an element queue. Those are
   two lists and not one, and the difference is observable: §4.13.6's invoke dequeues an element and then drains
   ALL of that element's reactions, so `el.setAttribute(a,1); other.setAttribute(b,2); el.setAttribute(a,3)`
   inside one `[CEReactions]` boundary runs el's two callbacks back to back. A single flat list of reactions
   would interleave them, which is a different program order for the page. */
/* §4.13.6's enqueue STEP 4 — "if callbackName is attributeChangedCallback and definition's observed attributes
   does not contain attrName, return". Over the LOCAL name, which is what §4.13.4 step 14.5 collected. Its two
   callers are the attribute write and §4.13.5 step 4's walk of the whole attribute list, and they must agree:
   a class that observes nothing must be told about nothing, whichever direction the attribute came from. */
static bool ce_observes(JSContext *ctx, JSValueConst def, const char *local)
{
    JSValue observed = JS_GetProperty(ctx, def, g_atom_observed);
    uint32_t n = 0, i;
    bool watched = false;

    if (JS_IsObject(observed)) {
        JSValue lv = JS_GetPropertyStr(ctx, observed, "length");
        JS_ToUint32(ctx, &n, lv);
        JS_FreeValue(ctx, lv);
        for (i = 0; i < n && !watched; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, observed, i);
            const char *s = JS_ToCString(ctx, e);
            if (s && strcmp(s, local) == 0) watched = true;
            if (s) JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, e);
        }
    }
    JS_FreeValue(ctx, observed);
    return watched;
}

static void ce_enqueue_args(JSContext *ctx, JSValueConst wrap, JSValueConst def, int callback,
                            int argc, JSValueConst *args)
{
    JSValue fn, cbs, reaction, rq;
    int i;

    DCHECK(argc <= CE_MAX_REACTION_ARGS,
           "a lifecycle callback was enqueued with more arguments than any of them takes");
    DCHECK(callback >= 0 && callback < CE_CB_COUNT,
           "a reaction was enqueued for a callback §4.13.4 step 14's map does not name");
    if (!JS_IsObject(def)) return;   /* step 1: an element with no definition has no reaction */
    if (!JS_IsObject(wrap)) return;
    cbs = JS_GetProperty(ctx, def, g_atom_callbacks);
    DCHECK(JS_IsObject(cbs), "a custom element definition carries no step 14.4 callback map — every definition "
                             "this component commits builds one, so a missing map is a definition it did not "
                             "make");
    fn = JS_GetPropertyUint32(ctx, cbs, (uint32_t)callback);
    JS_FreeValue(ctx, cbs);
    if (!JS_IsFunction(ctx, fn)) { JS_FreeValue(ctx, fn); return; }   /* step 3: the entry is null */
    if (callback == CE_CB_ATTR_CHANGED) {                             /* step 4 */
        const char *nm;
        bool watched;

        DCHECK(argc == 4, "an attributeChangedCallback reaction was enqueued without §4.13.3's four arguments");
        nm = JS_ToCString(ctx, args[0]);
        watched = nm != NULL && ce_observes(ctx, def, nm);
        if (nm) JS_FreeCString(ctx, nm);
        if (!watched) { JS_FreeValue(ctx, fn); return; }
    }
    reaction = JS_NewArray(ctx);
    CHECK(!JS_IsException(reaction), "a custom element callback reaction could not be allocated");
    JS_SetPropertyUint32(ctx, reaction, 0, JS_NewInt32(ctx, CE_REACTION_CALLBACK));
    JS_SetPropertyUint32(ctx, reaction, 1, fn);
    for (i = 0; i < argc; i++)
        JS_SetPropertyUint32(ctx, reaction, (uint32_t)(i + 2), JS_DupValue(ctx, args[i]));
    rq = ce_reaction_queue(ctx, wrap, 1);   /* step 5 */
    ce_array_push(ctx, rq, reaction);
    JS_FreeValue(ctx, rq);
    ce_enqueue_element(ctx, wrap);          /* step 6 */
}

static void ce_enqueue(JSContext *ctx, JSValueConst wrap, JSValueConst def, int callback)
{
    ce_enqueue_args(ctx, wrap, def, callback, 0, NULL);
}

void custom_elements_enqueue_form_callback(JSContext *ctx, JSValueConst wrap, int which,
                                           int argc, JSValueConst *args)
{
    JSValue def;

    DCHECK(g_ready, "a form lifecycle reaction was enqueued before custom_elements_init ran");
    DCHECK(which >= 0 && which <= CE_FORM_CB_STATE_RESTORE,
           "a form lifecycle reaction was enqueued for a callback §4.13.4 step 14.13 does not name");
    /* The public ids are the step 14.13 list's ORDER, so they index the one callback map by offsetting past
       step 14.4's — one addition rather than a second enum a caller could get out of step with. */
    def = ce_definition_of(ctx, wrap);
    ce_enqueue_args(ctx, wrap, def, CE_CB_LIFECYCLE_COUNT + which, argc, args);
    JS_FreeValue(ctx, def);
}

/* §4.13.6 "enqueue a custom element upgrade reaction". Two steps and both of them matter: the reaction records
   the DEFINITION so the upgrade that eventually runs uses the one that was current when the element was
   reached, and the ELEMENT joins an element queue so the drain finds it. Nothing about the element changes
   here — its prototype, its state and its definition are all §4.13.5's to write, from a place that can park on
   the constructor. */
static void ce_enqueue_upgrade(JSContext *ctx, JSValueConst wrap, JSValueConst def)
{
    JSValue reaction, rq;

    DCHECK(JS_IsObject(def), "an upgrade reaction was enqueued with no definition to upgrade with");
    if (!JS_IsObject(wrap)) return;
    reaction = JS_NewArray(ctx);
    CHECK(!JS_IsException(reaction), "a custom element upgrade reaction could not be allocated");
    JS_SetPropertyUint32(ctx, reaction, 0, JS_NewInt32(ctx, CE_REACTION_UPGRADE));
    JS_SetPropertyUint32(ctx, reaction, 1, JS_DupValue(ctx, def));
    rq = ce_reaction_queue(ctx, wrap, 1);
    ce_array_push(ctx, rq, reaction);
    JS_FreeValue(ctx, rq);
    ce_enqueue_element(ctx, wrap);
}

/* §4.13.5 "try to upgrade an element": look the definition up given THE ELEMENT'S OWN custom element registry,
   its namespace, its local name and its is value, and if there is one, enqueue an upgrade reaction. The
   registry is the element's and not the document's, which is the whole of what a scoped registry is: two
   `<x-a>` elements in one document, one in a scoped tree, upgrade with two different classes. No state is read
   here — §4.13.5 step 1 is the one that decides whether an element already past "undefined" is upgraded again,
   and it is read AT THE UPGRADE because a constructor running between the enqueue and the drain can change the
   answer. */
static void ce_try_upgrade(JSContext *ctx, lxb_dom_element_t *el, JSValueConst wrap)
{
    size_t len = 0;
    const lxb_char_t *tag = lxb_dom_element_local_name(el, &len);
    JSValue def;

    if (!tag || !len) return;
    def = ce_find_for_node(ctx, wrap, (const char *)tag, len);
    if (JS_IsObject(def)) ce_enqueue_upgrade(ctx, wrap, def);
    JS_FreeValue(ctx, def);
}

/* DOM §4.5's "GLOBAL CUSTOM ELEMENT REGISTRY" and "EFFECTIVE GLOBAL CUSTOM ELEMENT REGISTRY" — two definitions
   stated over "null or a CustomElementRegistry object", which is why both take a value that may be JS_NULL
   rather than a registry. A registry is GLOBAL when it is non-null and its `is scoped` is false; a registry's
   EFFECTIVE global is itself when it is global and null otherwise. They are the whole of what `adopt` writes,
   which is what makes adoption unable to hand a node a SCOPED registry it was not created under. */
static bool ce_registry_is_global(JSContext *ctx, JSValueConst reg)
{
    return JS_IsObject(reg) && !ce_reg_flag(ctx, reg, g_atom_scoped);
}

/* CONSUMES `reg` and returns the effective global, OWNED. */
static JSValue ce_registry_effective_global(JSContext *ctx, JSValue reg)
{
    if (ce_registry_is_global(ctx, reg)) return reg;
    JS_FreeValue(ctx, reg);
    return JS_NULL;
}

void custom_elements_node_adopted(JSContext *ctx, lxb_dom_node_t *n, lxb_dom_document_t *document,
                                  lxb_dom_document_t *old_document)
{
    JSValue wrap, reg;

    DCHECK(g_ready, "DOM §4.5 adopt reached the custom element registry arm before custom_elements_init ran");
    DCHECK(n != NULL && document != NULL && old_document != NULL && document != old_document,
           "DOM §4.5 adopt's step 3 arm ran for a node whose document did not change — step 3's own condition "
           "is `document is not oldDocument`, and running the arm anyway rewrites a registry the algorithm "
           "never reaches");
    /* Steps 3.2 and 3.3 name a SHADOW ROOT and an ELEMENT and nothing else; every other node kind carries no
       custom element registry at all (§4.13.4's "look up a custom element registry" returns null for one). */
    if (n->type != LXB_DOM_NODE_TYPE_ELEMENT && !shadow_root_is(n)) return;

    wrap = node_wrap(ctx, n);
    if (!JS_IsObject(wrap)) { JS_FreeValue(ctx, wrap); return; }
    reg = ce_registry_of_node(ctx, wrap);

    if (shadow_root_is(n)) {
        /* STEP 3.2 — a shadow root takes the new document's global registry when its own is null (and it is
           not being kept null) or is itself a global one. A SCOPED registry is left alone, which is the whole
           of HTML §4.13.4's "once the custom element registry of a node is initialized to a
           CustomElementRegistry object, it intentionally cannot be changed any further": the sentence is about
           the association a scoped registry makes, and `adopt` re-deriving a global one is stated by DOM
           itself. */
        /* §4.8's `keep custom element registry null` is the second half of the first condition, and it is a
           real read: its one writer is HTML §13.2.6.4.4's `shadowrootcustomelementregistry` branch, and a root
           that carries it resolves in NOTHING until something associates a registry — which this step would
           otherwise undo on the very first adoption by handing it the new document's. */
        if ((JS_IsNull(reg) && !shadow_root_keep_registry_null(ctx, wrap))
            || (!JS_IsNull(reg) && ce_registry_is_global(ctx, reg))) {
            JSValue want = ce_registry_effective_global(ctx, ce_registry_of_document(ctx, document));

            ce_node_set_registry(ctx, wrap, want);
            JS_FreeValue(ctx, want);
        }
    } else {
        /* STEP 3.3.2 — an element whose registry is null or NOT scoped re-derives one. The derivation is
           stated over the element's PARENT and not over the new document alone, because the walk reaches a
           descendant after its parent: an element adopted into a scoped tree takes that tree's registry's
           effective global (which is NULL, a scoped registry having none), and only a root — or a child of an
           exclusive DocumentFragment, which is a fragment that is not a shadow root — falls back to the
           document's. */
        if (JS_IsNull(reg) || !ce_reg_flag(ctx, reg, g_atom_scoped)) {
            JSValue registry, want;

            if (JS_IsObject(reg) || n->parent == NULL ||
                (node_is_document_fragment(n->parent) && !shadow_root_is(n->parent))) {
                registry = ce_registry_of_document(ctx, document);
            } else {
                JSValue pw = node_wrap(ctx, n->parent);
                registry = ce_registry_of_node(ctx, pw);
                JS_FreeValue(ctx, pw);
            }
            want = ce_registry_effective_global(ctx, registry);
            ce_node_set_registry(ctx, wrap, want);
            JS_FreeValue(ctx, want);
        }
        /* STEP 3.3.3 — the `adoptedCallback` reaction, with « oldDocument, document ». Only a CUSTOM element
           has one: §4.13.6's enqueue takes the element's own definition, and an element that was never
           upgraded carries none. */
        if (ce_state_of(ctx, wrap) == CE_STATE_CUSTOM) {
            JSValue def = ce_definition_of(ctx, wrap);
            JSValue args[2];

            args[0] = node_wrap(ctx, lxb_dom_interface_node(old_document));
            args[1] = node_wrap(ctx, lxb_dom_interface_node(document));
            ce_enqueue_args(ctx, wrap, def, CE_CB_ADOPTED, 2, (JSValueConst *)args);
            JS_FreeValue(ctx, args[1]);
            JS_FreeValue(ctx, args[0]);
            JS_FreeValue(ctx, def);
        }
    }
    JS_FreeValue(ctx, reg);
    JS_FreeValue(ctx, wrap);
}

/* NOTHING IS ALLOCATED FOR AN ORDINARY ELEMENT, and that is what keeps these two on the tree walk's hot path.
   Reading an element's state means minting its WRAPPER, and the parser inserts every node in the document
   through here — so the cheap half of the question is asked first, off the Lexbor name alone: an element whose
   local name is not a valid custom element name can be neither "custom" nor upgraded, because the only kind of
   custom element this engine registers is the AUTONOMOUS kind (ce_define_checks throws NotSupportedError for
   `extends`, so no built-in's name can carry a definition). Building customized built-ins widens this test at
   the same time as it widens that refusal. */
static bool ce_upgradable_name(lxb_dom_element_t *el)
{
    size_t len = 0;
    const lxb_char_t *tag = lxb_dom_element_local_name(el, &len);
    return tag != NULL && len != 0 && custom_elements_name_is_valid((const char *)tag, len);
}

void custom_elements_disconnected(JSContext *ctx, lxb_dom_element_t *el)
{
    JSValue wrap, def;

    if (!g_ready || !ce_upgradable_name(el)) return;
    wrap = node_wrap(ctx, lxb_dom_interface_node(el));
    /* §4.13.3: only an element whose upgrade SUCCEEDED has a disconnected reaction, and the definition it was
       upgraded WITH is the one that supplies the callback. Asking the registry by name instead would fire for
       an element that was never upgraded — one created before its definition and removed before it. */
    if (ce_state_of(ctx, wrap) == CE_STATE_CUSTOM) {
        def = ce_definition_of(ctx, wrap);
        ce_enqueue(ctx, wrap, def, CE_CB_DISCONNECTED);
        JS_FreeValue(ctx, def);
        /* §4.10.18.3: "the form owner is also reset by the HTML element removing steps" — which is what drops
           a `form=`-associated element's owner when it leaves the document, because step 4's condition
           includes "and is connected". */
        element_internals_reset_form_owner(ctx, wrap, ELEMENT_INTERNALS_ATTR_UNCHANGED, 0);
    }
    JS_FreeValue(ctx, wrap);
}

void custom_elements_element_connected(JSContext *ctx, lxb_dom_element_t *el)
{
    JSValue wrap;

    if (!g_ready || !ce_upgradable_name(el)) return;
    wrap = node_wrap(ctx, lxb_dom_interface_node(el));
    if (!JS_IsObject(wrap)) { JS_FreeValue(ctx, wrap); return; }
    /* DOM §4.2.3 step 7's FIRST clause, which precedes the two below it: "if inclusiveDescendant's custom
       element registry's is scoped is true, then append inclusiveDescendant's node document to that registry's
       scoped document set". That set is what §4.13.4 step 17 walks when a scoped registry finally defines the
       name — an element inserted into a second document under a scoped registry must be upgraded there too,
       and a set the insertion never appended to would leave it un-upgraded forever. */
    {
        JSValue reg = ce_registry_of_node(ctx, wrap);

        if (JS_IsObject(reg) && ce_reg_flag(ctx, reg, g_atom_scoped))
            ce_scoped_docs_append(ctx, reg, lxb_dom_interface_node(el));
        JS_FreeValue(ctx, reg);
    }
    /* DOM §4.2.3's insertion steps, custom-element half: an element that is already CUSTOM gets a connected
       reaction — which is how a page that moves a node around keeps its lifecycle running — and any other
       element is tried for upgrade, whose own step 5 enqueues that same reaction if it succeeds. Doing both
       would run connectedCallback twice for a freshly upgraded element. */
    if (ce_state_of(ctx, wrap) == CE_STATE_CUSTOM) {
        JSValue def = ce_definition_of(ctx, wrap);
        ce_enqueue(ctx, wrap, def, CE_CB_CONNECTED);
        JS_FreeValue(ctx, def);
        /* §4.10.18.3: "the form owner is also reset by the HTML element insertion steps". Only for an element
           that is ALREADY custom — one being tried for upgrade has §4.13.5 step 10 ahead of it, and resetting
           here as well would enqueue a formAssociatedCallback the upgrade is about to enqueue again. */
        element_internals_reset_form_owner(ctx, wrap, ELEMENT_INTERNALS_ATTR_UNCHANGED, 0);
    } else {
        ce_try_upgrade(ctx, el, wrap);
    }
    JS_FreeValue(ctx, wrap);
}

/* §4.13.4's "UPGRADE PARTICULAR ELEMENTS WITHIN A DOCUMENT", given a registry, a document, a definition and a
   local name. Two things it is not: it is not "every element with this name" — the candidates are the ones
   whose CUSTOM ELEMENT REGISTRY IS THIS ONE, which is what stops a scoped `define` from upgrading the
   document's own `<x-a>` elements — and the walk is SHADOW-INCLUDING, which is what reaches a component's own
   shadow tree, where the elements a page built before their definition arrived actually are.
   §4.13.4 step 17: define() enqueues an upgrade reaction for every EXISTING matching element, not only the
   ones inserted later — a definition that arrives after the parser is the ordinary case for a deferred bundle.
   The upgrades then run at define()'s own `[CEReactions]` boundary, which is what makes
   `customElements.define(…)` followed by a read of state the constructor set work on the next line. */
static void ce_upgrade_particular(JSContext *ctx, JSValueConst registry, lxb_dom_node_t *root,
                                  const char *name, size_t nlen, JSValueConst def)
{
    lxb_dom_node_t *n;
    size_t len = 0;

    if (!root) return;
    for (n = root; n; n = shadow_root_next_in_shadow_including(ctx, n, root)) {
        const lxb_char_t *tag;
        JSValue wrap, reg;
        bool mine;

        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        tag = lxb_dom_element_local_name(lxb_dom_interface_element(n), &len);
        if (!tag || len != nlen || memcmp(tag, name, len) != 0) continue;
        wrap = node_wrap(ctx, n);
        reg = ce_registry_of_node(ctx, wrap);
        mine = JS_VALUE_GET_PTR(reg) == JS_VALUE_GET_PTR(registry) && JS_IsObject(reg);
        JS_FreeValue(ctx, reg);
        if (mine) ce_enqueue_upgrade(ctx, wrap, def);
        JS_FreeValue(ctx, wrap);
    }
}

/* §4.13.4 step 17's TWO ARMS. A scoped registry walks its own SCOPED DOCUMENT SET — every document one of its
   nodes has been inserted into — and a global one walks the relevant global's associated Document, which is
   this realm's. */
static void ce_upgrade_candidates(JSContext *ctx, JSValueConst registry, const char *name, size_t nlen,
                                  JSValueConst def)
{
    if (ce_reg_flag(ctx, registry, g_atom_scoped)) {
        JSValue docs = ce_reg_field(ctx, registry, g_atom_docs);
        uint32_t n = ce_array_len(ctx, docs), i;

        for (i = 0; i < n; i++) {
            JSValue d = JS_GetPropertyUint32(ctx, docs, i);
            ce_upgrade_particular(ctx, registry, node_of(d), name, nlen, def);
            JS_FreeValue(ctx, d);
        }
        JS_FreeValue(ctx, docs);
        return;
    }
    ce_upgrade_particular(ctx, registry, document_root_node(ctx), name, nlen, def);
}

/* §4.13.3 "attribute changed": the reaction runs only for a name the definition declared as OBSERVED, which is
   why observedAttributes is read at define time and stored — a class watching two attributes must not have its
   callback run for the other fifty a page writes. Four arguments, which is what makes the generalised reaction
   carry an argument vector rather than a name alone.
   THE OBSERVED SET IS OVER LOCAL NAMES, and the old value is read by §4.9's own identity: a qualified-name read
   would answer with whichever attribute happens to print that name FIRST, which for a prefixed attribute is a
   different attribute than the one being written. */
void custom_elements_attribute_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local,
                                       const char *old, size_t old_len, const char *val, size_t val_len)
{
    JSValue wrap, def, args[4];
    uint32_t i;

    if (!g_ready) return;
    wrap = node_wrap(ctx, lxb_dom_interface_node(el));
    def = ce_definition_of(ctx, wrap);
    if (!JS_IsObject(def)) { JS_FreeValue(ctx, def); JS_FreeValue(ctx, wrap); return; }
    /* §4.13.3's arguments: (localName, oldValue, newValue, namespace). An attribute that was absent has a NULL
       old value and an attribute being removed a NULL new one, and the page's code branches on exactly that;
       the namespace is null for every attribute an HTML page writes and a URI for the ones the parser moved. */
    args[0] = JS_NewString(ctx, local);
    args[1] = old ? JS_NewStringLen(ctx, old, old_len) : JS_NULL;
    args[2] = val ? JS_NewStringLen(ctx, val, val_len) : JS_NULL;
    args[3] = ns ? JS_NewString(ctx, ns) : JS_NULL;
    ce_enqueue_args(ctx, wrap, def, CE_CB_ATTR_CHANGED, 4, (JSValueConst *)args);
    for (i = 0; i < 4; i++) JS_FreeValue(ctx, args[i]);
    /* §4.10.18.3: "when a listed form-associated element's `form` attribute is set, changed, or removed, the
       user agent must reset the form owner of that element". The new value is handed over because it is the
       OPERATION's input, which is the rule for every input of a step that could become a job — not because
       the element cannot be asked. */
    if (!ns && strcmp(local, "form") == 0)
        element_internals_reset_form_owner(ctx, wrap, val, val_len);
    JS_FreeValue(ctx, def);
    JS_FreeValue(ctx, wrap);
}

/* §4.13.4 whenDefined(name) — the promise a page awaits before it uses a tag whose bundle may not have loaded
   yet. It is the reason a lazily-registered component is reachable at all: `await customElements.whenDefined
   ('x-app'); document.createElement('x-app')` is the ordinary shape, and with no such member the await threw
   and the code after it never ran.
   NOT A STEP MACHINE, because it runs no author code: the name is a DOMString the declaration has already
   converted, the map is this component's own object, and settling a promise enqueues a job rather than calling
   into the page. The settle still goes through JS_CallAsFlow — a settle has a flow base under it, which is not
   a per-call judgement about whether this one happens to need one. */
static JSValue js_ce_when_defined(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    const char *nm;
    size_t nlen;
    JSValue map, entry, promise, resolving[2], def;
    JSValueConst reg = this_val;
    JSAtom a;

    (void)magic; (void)argc;
    if (!ce_registry_this(ctx, this_val)) return JS_EXCEPTION;
    nm = JS_ToCStringLen(ctx, &nlen, argv[0]);   /* a real string by now: the declaration converted it */
    if (!nm) return JS_EXCEPTION;
    if (!custom_elements_name_is_valid(nm, nlen)) {              /* step 1: a REJECTED promise, never a synchronous throw */
        JSValue exc;

        JS_FreeCString(ctx, nm);
        promise = JS_NewPromiseCapability(ctx, resolving);
        if (JS_IsException(promise)) return promise;
        JS_ThrowDOMException(ctx, "SyntaxError", "not a valid custom element name");
        exc = JS_GetException(ctx);
        if (JS_CallAsFlow(ctx, resolving[1], exc) < 0) JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, resolving[0]);
        JS_FreeValue(ctx, resolving[1]);
        return promise;
    }
    def = ce_find_in(ctx, reg, nm, nlen);        /* step 2: already defined — resolved with the constructor */
    if (JS_IsObject(def)) {
        JSValue ctor = JS_GetProperty(ctx, def, g_atom_ctor);

        JS_FreeValue(ctx, def);
        JS_FreeCString(ctx, nm);
        promise = JS_NewPromiseCapability(ctx, resolving);
        if (JS_IsException(promise)) { JS_FreeValue(ctx, ctor); return promise; }
        if (JS_CallAsFlow(ctx, resolving[0], ctor) < 0) JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, ctor);
        JS_FreeValue(ctx, resolving[0]);
        JS_FreeValue(ctx, resolving[1]);
        return promise;
    }
    JS_FreeValue(ctx, def);
    a = JS_NewAtomLen(ctx, nm, nlen);
    CHECK(a != JS_ATOM_NULL, "custom elements: a whenDefined name could not be interned");
    JS_FreeCString(ctx, nm);
    map = ce_reg_field(ctx, reg, g_atom_whendef);
    entry = JS_GetProperty(ctx, map, a);
    if (!JS_IsObject(entry)) {                   /* step 3: map[name] does not exist — a NEW promise */
        JS_FreeValue(ctx, entry);
        promise = JS_NewPromiseCapability(ctx, resolving);
        if (JS_IsException(promise)) { JS_FreeValue(ctx, map); JS_FreeAtom(ctx, a); return promise; }
        entry = JS_NewArray(ctx);
        CHECK(!JS_IsException(entry), "custom elements: a when-defined map entry could not be allocated");
        JS_SetPropertyUint32(ctx, entry, 0, promise);          /* the promise, and the two halves that settle */
        JS_SetPropertyUint32(ctx, entry, 1, resolving[0]);
        JS_SetPropertyUint32(ctx, entry, 2, resolving[1]);
        JS_SetProperty(ctx, map, a, JS_DupValue(ctx, entry));
    }
    JS_FreeValue(ctx, map);
    JS_FreeAtom(ctx, a);
    promise = JS_GetPropertyUint32(ctx, entry, 0);             /* step 4: return map[name] */
    JS_FreeValue(ctx, entry);
    return promise;
}

/* §4.13.4's last step: "If this's when-defined promise map[name] exists, resolve it with constructor and remove
   it." Reached from the commit, after the upgrade reactions the same step list enqueues — a page that awaits
   whenDefined and then reads state its constructor set must find the constructors already run. */
static void ce_when_defined_resolve(JSContext *ctx, JSValueConst registry, const char *name, size_t nlen,
                                    JSValueConst ctor)
{
    JSAtom a = JS_NewAtomLen(ctx, name, nlen);
    JSValue map, entry;

    CHECK(a != JS_ATOM_NULL, "custom elements: a name could not be interned");
    map = ce_reg_field(ctx, registry, g_atom_whendef);
    entry = JS_GetProperty(ctx, map, a);
    if (JS_IsObject(entry)) {
        JSValue resolve = JS_GetPropertyUint32(ctx, entry, 1);

        JS_DeleteProperty(ctx, map, a, 0);
        if (JS_CallAsFlow(ctx, resolve, ctor) < 0) JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, resolve);
    }
    JS_FreeValue(ctx, entry);
    JS_FreeValue(ctx, map);
    JS_FreeAtom(ctx, a);
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
/* WHERE THIS MACHINE RESTS, AS §4.13.4 NUMBERS IT — and writing the numbers down is what showed the steps were
   running in the WRONG ORDER. The whole of the validation lived at the END, inside the registration, so
   `customElements.define("not a name", notAConstructor)` ran the page's `observedAttributes` getter (step
   14.5.1) BEFORE throwing the TypeError step 1 states. A page with a getter can see exactly that, and nothing
   in the code said which step anything was. The checks now run first, each read of the page's object is its own
   stage, and the registration is the last one.
   `Get(constructor, "prototype")` is its own stage for the same reason `observedAttributes` is: a Proxy makes
   it the page's code, and it was a JS_GetProperty from C — a C activation hosting the page's loops, which is
   the one thing this declaration surface exists to remove. */
#define CE_DEFINE_STAGES(X) \
    X(CE_CHECKS,    "HTML §4.13.4 steps 1-7 (IsConstructor; a valid custom element name; the name and the " \
                    "constructor are not already defined; `extends`)") \
    X(CE_PROTOTYPE, "HTML §4.13.4 steps 14.1-14.2 (Get(constructor, \"prototype\"); it must be an Object)") \
    X(CE_CALLBACKS, "HTML §4.13.4 step 14.4 (Get(prototype, callbackName) for each key of lifecycleCallbacks, " \
                    "in the map's order, converting each to the Function callback type), one key per step") \
    X(CE_OBSERVED,  "HTML §4.13.4 step 14.5.1 (Get(constructor, \"observedAttributes\"), reached only when " \
                    "step 14.4 collected an attributeChangedCallback)") \
    X(CE_SEQUENCE,  "HTML §4.13.4 step 14.5.2 (converting it to a sequence<DOMString>), one entry per step") \
    X(CE_DISABLED,  "HTML §4.13.4 step 14.7 (Get(constructor, \"disabledFeatures\"))") \
    X(CE_DISABLED_SEQ, "HTML §4.13.4 step 14.8 (converting it to a sequence<DOMString>), one entry per step") \
    X(CE_FORM_FLAG, "HTML §4.13.4 step 14.11 (Get(constructor, \"formAssociated\"); step 14.12's ToBoolean " \
                    "runs no code, and steps 14.9-14.10 read the sequence step 14.8 built)") \
    X(CE_FORM_CB,   "HTML §4.13.4 step 14.13 (Get(prototype, callbackName) for each of « " \
                    "formAssociatedCallback, formResetCallback, formDisabledCallback, formStateRestoreCallback " \
                    "», reached only when step 14.12 answered true), one key per step") \
    X(CE_COMMIT,    "HTML §4.13.4 steps 15-16 and 18 (the definition, and the upgrade reaction for each " \
                    "candidate)")
enum { IDL_STEP_STAGE_BASE(CE_DEFINE_STAGES) CE_DEFINE_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const CE_DEFINE_STEPS[] = { CE_DEFINE_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    uint32_t i, n;      /* THE RESUME POINT: step 14.4's callback key, then the sequence entry */
    JSValue  proto;     /* step 14.1's answer (owned) */
    JSValue  callbacks; /* step 14.4's and step 14.13's map, indexed by CE_CB_* (owned) */
    JSValue  raw;       /* what the observedAttributes / disabledFeatures getter answered (owned) */
    JSValue  names;     /* step 14.5.2's converted sequence<DOMString> (owned) */
    JSValue  features;  /* step 14.8's converted sequence<DOMString> (owned) */
    /* THE RECEIVER, HELD — `define` is a method of ONE registry and every step from 3 onwards reads or writes
       that registry's own state, so a resume must find the same one rather than the realm's. Owned, because a
       parked machine's receiver is not kept alive by the call that is no longer on any stack. */
    JSValue  registry;
    uint32_t flags;     /* step 15's three booleans, as CE_DEF_* bit positions */
} CeDefineState;

static void ce_define_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    CeDefineState *s = st;
    v->val(ctx, &s->proto);
    v->val(ctx, &s->callbacks);
    v->val(ctx, &s->raw);
    v->val(ctx, &s->names);
    v->val(ctx, &s->features);
    v->val(ctx, &s->registry);
}

/* §4.13.4 step 14's "then, regardless of whether the above steps threw an exception or not: set this's element
   definition is running to false". The teardown IS that "regardless": it runs on the throw path and on the
   completion path, and on nothing in between, which is what a park in the middle of step 14.5 requires.
   IT IS THE WHOLE OF THIS HOOK, and it is also why the hook runs BEFORE the declaration is discharged: the flag
   lives on `s->registry`, so lowering it READS a value this state owns. Reading one here is correct; freeing
   one is not — every value below the flag is named by ce_define_visit, which is the one list. */
static void ce_define_release(JSContext *ctx, void *st)
{
    CeDefineState *s = st;
    if (JS_IsObject(s->registry) && ce_reg_flag(ctx, s->registry, g_atom_defining))
        ce_reg_set_flag(ctx, s->registry, g_atom_defining, false);
}

/* §4.13.4'S TWO `sequence<DOMString>` CONVERSIONS AS ONE WALK — step 14.5.2's observedAttributes and step
   14.8's disabledFeatures are the same conversion over the same cursor, and writing it twice is how the second
   one gets a `length` read the first one does not. `dest` receives the converted entries; `s->raw` is the
   value the getter answered and `s->i`/`s->n` are the resume point. Returns >0 parked on ONE entry's ToString
   (the caller returns it), 0 when the sequence is exhausted, or -1 with a throw live. */
static int ce_sequence_run(JSContext *ctx, JSStepHdr *hdr, CeDefineState *s, JSValueConst dest,
                           JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    while (s->i < s->n) {
        JSValue entry = JS_GetPropertyUint32(ctx, s->raw, s->i);
        JSValue item = JS_UNDEFINED;
        int r = step_tostring_run(ctx, hdr, entry, cb_result, &item, out_cb, out_argc);

        JS_FreeValue(ctx, entry);
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;          /* parked ON THIS ENTRY; the resume comes back to it */
        if (r < 0) return -1;
        JS_SetPropertyUint32(ctx, (JSValue)dest, s->i, item);
        s->i++;
    }
    JS_FreeValue(ctx, cb_result);
    return 0;
}

/* THE LENGTH OF WHAT A GETTER ANSWERED WITH, for the walk above. §4.13.4 converts a
   `sequence<DOMString>`, whose length is itself a read — of an engine-visible array in every real case. */
static uint32_t ce_sequence_length(JSContext *ctx, JSValueConst raw)
{
    uint32_t n = 0;

    if (JS_IsObject(raw)) {
        JSValue lv = JS_GetPropertyStr(ctx, raw, "length");
        JS_ToUint32(ctx, &n, lv);
        JS_FreeValue(ctx, lv);
    }
    return n;
}

/* Infra's "list contains" over a converted sequence — steps 14.9 and 14.10's test on disabledFeatures. */
static bool ce_sequence_contains(JSContext *ctx, JSValueConst seq, const char *want)
{
    uint32_t n = ce_sequence_length(ctx, seq), i;

    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, seq, i);
        const char *s = JS_ToCString(ctx, e);
        bool hit = s != NULL && strcmp(s, want) == 0;

        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, e);
        if (hit) return true;
    }
    return false;
}

/* §4.13.4 STEPS 1-7 — everything the spec decides BEFORE it touches the page's object. Its own function
   because its own STAGE: running it after the reads, which is where it used to live, made the page's getters
   observe a call the spec had already rejected. Returns <0 having thrown. */
static int ce_define_checks(JSContext *ctx, JSValueConst registry, int argc, JSValueConst *argv)
{
    JSValue ext;
    const char *nm;
    size_t nlen;
    bool taken;
    JSValue prev;

    /* step 1: IsConstructor(constructor). */
    if (!JS_IsFunction(ctx, argv[1])) {
        JS_ThrowTypeError(ctx, "customElements.define requires a constructor");
        return -1;
    }
    nm = JS_ToCStringLen(ctx, &nlen, argv[0]);   /* a real string by now: the declaration converted it */
    if (!nm) return -1;
    if (!custom_elements_name_is_valid(nm, nlen)) {              /* step 2 */
        JS_FreeCString(ctx, nm);
        JS_ThrowDOMException(ctx, "SyntaxError", "not a valid custom element name");
        return -1;
    }
    prev = ce_find_in(ctx, registry, nm, nlen);  /* step 3 */
    taken = JS_IsObject(prev);
    JS_FreeValue(ctx, prev);
    JS_FreeCString(ctx, nm);
    if (taken) {
        JS_ThrowDOMException(ctx, "NotSupportedError", "this name is already defined");
        return -1;
    }
    /* step 4: the definition set already contains an item with THIS CONSTRUCTOR. It is a second question and
       not the same one — `define('a-x', C); define('b-x', C)` passes step 3 both times — and answering it is
       what the definition ORDER exists for, because a set keyed by name cannot be asked about a constructor.
       IT IS THIS REGISTRY'S SET: the same class may be defined once in the document's registry and once in a
       scoped one, which is exactly what a scoped registry is for. */
    {
        JSValue list = ce_reg_field(ctx, registry, g_atom_order);
        uint32_t n = ce_array_len(ctx, list), i;
        bool dup = false;

        for (i = 0; i < n && !dup; i++) {
            JSValue d = JS_GetPropertyUint32(ctx, list, i);
            JSValue c = JS_GetProperty(ctx, d, g_atom_ctor);

            dup = JS_VALUE_GET_PTR(c) == JS_VALUE_GET_PTR(argv[1]);
            JS_FreeValue(ctx, c);
            JS_FreeValue(ctx, d);
        }
        JS_FreeValue(ctx, list);
        if (dup) {
            JS_ThrowDOMException(ctx, "NotSupportedError", "this constructor is already defined");
            return -1;
        }
    }
    /* steps 6-7: customized built-ins. Rejected rather than registered as autonomous — quietly treating
       `{extends:'button'}` as a new tag would define something the page never asked for and leave the button
       it did ask for un-upgraded.
       IT IS ASKED LAST, WHICH IS WHERE §4.13.4 ASKS IT. This ran FIRST, so
       `define('not a name', C, {extends:'button'})` answered NotSupportedError where the standard answers
       step 2's SyntaxError — a page's `catch` tells those apart, and so does the corpus. Reading the
       already-converted dictionary runs none of the page's code, so the only thing the order decided was
       WHICH exception, which is exactly the thing a reordering is invisible in until someone catches it. */
    ext = idl_dict_get(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, "extends");
    if (JS_IsString(ext)) {
        JS_FreeValue(ctx, ext);
        /* step 7.1 is its OWN refusal and comes first: "if this's is scoped is true, then throw a
           NotSupportedError". A scoped registry may not define a customized built-in AT ALL, whatever the name
           is, and saying so separately is what keeps the two reasons distinguishable once `extends` is real. */
        if (ce_reg_flag(ctx, registry, g_atom_scoped)) {
            JS_ThrowDOMException(ctx, "NotSupportedError",
                                 "a scoped CustomElementRegistry cannot define a customized built-in");
            return -1;
        }
        JS_ThrowDOMException(ctx, "NotSupportedError",
                             "a customized built-in (`extends`) is not modelled: this engine has no built-in "
                             "element to customize yet, and registering it as an autonomous element would "
                             "define a tag the page never asked for");
        return -1;
    }
    JS_FreeValue(ctx, ext);
    /* STEPS 8-9: "element definition is running". It exists because everything after it READS THE PAGE'S
       OBJECT — a static `observedAttributes` getter that calls `define` again would otherwise run this whole
       algorithm re-entrantly and commit two definitions for one name. The flag is cleared by
       ce_define_release, which is this machine's teardown and therefore runs on BOTH exits, which is exactly
       what step 14's "regardless of whether the above steps threw an exception or not" asks for. */
    if (ce_reg_flag(ctx, registry, g_atom_defining)) {
        JS_ThrowDOMException(ctx, "NotSupportedError",
                             "customElements.define was re-entered while this registry was already defining");
        return -1;
    }
    ce_reg_set_flag(ctx, registry, g_atom_defining, true);
    return 0;
}

/* §4.13.4 STEPS 15-16 AND 18 — the definition, the definition set, and the upgrade reactions. Plain C, and it
   stays that way: every value it needs is already real, and this is the part that touches only the component's
   own state. `proto` is step 14.1's answer, read as a request rather than here. */
static JSValue ce_define_commit(JSContext *ctx, JSValueConst registry, JSValueConst *argv, JSValueConst names,
                                JSValueConst proto, JSValueConst callbacks, uint32_t flags)
{
    const char *nm;
    size_t nlen;

    nm = JS_ToCStringLen(ctx, &nlen, argv[0]);
    if (!nm) return JS_EXCEPTION;
    {
        JSValue def = JS_NewObjectProto(ctx, JS_NULL);
        JSAtom a;

        CHECK(!JS_IsException(def), "custom elements: OOM allocating a definition — a dropped definition is a "
                                    "class whose lifecycle code never runs");
        JS_SetProperty(ctx, def, g_atom_ctor, JS_DupValue(ctx, argv[1]));
        /* §4.13.4 step 15's NAME and LOCAL NAME. Two fields, equal for an autonomous custom element and
           different for a customized built-in — §4.13.2 step 5 tells them apart by comparing exactly these,
           so folding them into one would make every definition look autonomous the moment `extends` lands.
           The SAME string value in both, so the identity comparison there is the answer and not a strcmp. */
        JS_SetProperty(ctx, def, g_atom_name, JS_DupValue(ctx, argv[0]));
        JS_SetProperty(ctx, def, g_atom_local, JS_DupValue(ctx, argv[0]));
        /* §4.13.2's CONSTRUCTION STACK, empty. It is per definition and it is an Array, so it forks with the
           flow that is inside a constructor and parks with it — a C list would revert its head POINTER on a
           context switch and leave the element being upgraded reachable from nothing. */
        {
            JSValue stack = JS_NewArray(ctx);
            CHECK(!JS_IsException(stack), "a §4.13.2 construction stack could not be allocated");
            JS_SetProperty(ctx, def, g_atom_stack, stack);
        }
        /* The class's `prototype` is what the upgrade installs. Read ONCE, at step 14.1, so a page that
           reassigns it afterwards does not retroactively change what its already-defined elements are. */
        JS_SetProperty(ctx, def, g_atom_proto, JS_DupValue(ctx, proto));
        JS_SetProperty(ctx, def, g_atom_observed, JS_DupValue(ctx, names));
        /* §4.13.4 step 15's definition holds the lifecycle callbacks step 14.4 collected — the definition IS
           where a reaction reads its callback from, which is what makes a later `X.prototype.connectedCallback
           = other` change nothing about the elements already defined. */
        JS_SetProperty(ctx, def, g_atom_callbacks, JS_DupValue(ctx, callbacks));
        /* §4.13.4 step 15's form-associated, disable internals and disable shadow. */
        JS_SetProperty(ctx, def, g_atom_flags, JS_NewUint32(ctx, flags));
        a = JS_NewAtomLen(ctx, nm, nlen);
        CHECK(a != JS_ATOM_NULL, "custom elements: a name could not be interned");
        {
            JSValue defs = ce_reg_field(ctx, registry, g_atom_defs);
            JSValue list = ce_reg_field(ctx, registry, g_atom_order);
            /* ONE SET, TWO READINGS: the name index the lookups use, and the definition ORDER §4.13.2 step 4
               walks to answer "which definition has this constructor". Written together, here, because a
               definition in one and not the other is a definition half the platform can see. */
            JS_SetProperty(ctx, defs, a, JS_DupValue(ctx, def));
            ce_array_push(ctx, list, JS_DupValue(ctx, def));
            JS_FreeValue(ctx, list);
            JS_FreeValue(ctx, defs);
        }
        JS_FreeAtom(ctx, a);
        ce_upgrade_candidates(ctx, registry, nm, nlen, def);   /* step 17 */
        ce_when_defined_resolve(ctx, registry, nm, nlen, argv[1]);
        JS_FreeValue(ctx, def);
    }
    JS_FreeCString(ctx, nm);
    return JS_UNDEFINED;
}

static int js_ce_define(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                        JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    CeDefineState *s = st;
    int r;

    if (hdr->stage == CE_CHECKS) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* EVERY owned field before the first thing that can throw: the failure path tears this state down
           through ce_define_release, which frees exactly what the state holds and nothing else. */
        s->proto = s->raw = s->registry = JS_UNDEFINED;
        s->flags = 0;
        s->callbacks = JS_NewArray(ctx);
        s->names = JS_NewArray(ctx);
        s->features = JS_NewArray(ctx);
        CHECK(!JS_IsException(s->callbacks) && !JS_IsException(s->names) && !JS_IsException(s->features),
              "custom elements: OOM building §4.13.4 step 14's callback map — a definition with no callbacks "
              "is a class whose lifecycle code never runs");
        /* `define(name, constructor, optional options)` declares two required arguments, and Web IDL's own
           count check has already thrown for a call with fewer — the body's copy of it was unreachable. */
        DCHECK(argc >= 2, "customElements.define reached its algorithm with fewer than the two arguments its "
                          "declaration requires — the count check belongs to Web IDL and it did not run");
        if (!ce_registry_this(ctx, hdr->this_val)) return -1;
        s->registry = JS_DupValue(ctx, hdr->this_val);
        if (ce_define_checks(ctx, s->registry, argc, argv) < 0) return -1;
        hdr->stage = CE_PROTOTYPE;
    }
    if (hdr->stage == CE_PROTOTYPE) {
        r = step_getprop_run(ctx, hdr, argv[1], g_atom_prototype, cb_result, &s->proto, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return -1;
        if (!JS_IsObject(s->proto)) {   /* step 14.2 */
            JS_ThrowTypeError(ctx, "a custom element constructor's prototype must be an object");
            return -1;
        }
        hdr->stage = CE_CALLBACKS;
        s->i = 0;
    }
    if (hdr->stage == CE_CALLBACKS) {
        /* §4.13.4 step 14.4 — "for each callbackName of the keys of lifecycleCallbacks". IN ORDER, one key per
           step, off the PROTOTYPE, which is where a class puts them and where a Proxy can be. Collecting them
           HERE rather than reading one off the element per reaction is the whole point: a browser answers a
           reaction with the function this loop saw, so reassigning the prototype's property afterwards changes
           nothing, and an own property on the element is not a lifecycle callback at all. */
        while (s->i < CE_CB_LIFECYCLE_COUNT) {
            JSValue v = JS_UNDEFINED;
            r = step_getprop_run(ctx, hdr, s->proto, g_cb_atoms[s->i], cb_result, &v, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r > 0) return r;          /* parked ON THIS KEY; the resume comes back to it */
            if (r < 0) return -1;
            /* step 14.4.2: a value that is not undefined is converted to the Web IDL Function callback type,
               which throws for anything not callable — and the loop STOPS there, so the keys after it are
               never read. `null` is not undefined: it is a value that fails the conversion. */
            if (!JS_IsUndefined(v)) {
                if (!JS_IsFunction(ctx, v)) {
                    JS_FreeValue(ctx, v);
                    JS_ThrowTypeError(ctx, "a custom element's %s is not callable", CE_CALLBACK_NAMES[s->i]);
                    return -1;
                }
                JS_SetPropertyUint32(ctx, s->callbacks, s->i, v);
            } else {
                JS_FreeValue(ctx, v);
            }
            s->i++;
        }
        hdr->stage = CE_OBSERVED;
    }
    if (hdr->stage == CE_OBSERVED) {
        /* §4.13.4 step 14.5 is CONDITIONAL: observedAttributes is read only when step 14.4 collected an
           attributeChangedCallback. A class with no such callback never observes an attribute, so reading the
           property would run a getter the algorithm never asks for — which a page's Proxy sees, and which is
           the difference between rethrowing a page's error and never provoking it. */
        JSValue seen = JS_GetPropertyUint32(ctx, s->callbacks, CE_CB_ATTR_CHANGED);
        bool observes = JS_IsFunction(ctx, seen);
        JS_FreeValue(ctx, seen);
        s->i = 0;
        s->n = 0;
        if (!observes) {
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            hdr->stage = CE_DISABLED;
        } else {
            r = step_getprop_run(ctx, hdr, argv[1], g_atom_observed_src, cb_result, &s->raw, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r > 0) return r;
            if (r < 0) return -1;
            hdr->stage = CE_SEQUENCE;
            /* §4.13.4: absent observedAttributes is not an error, it is no observed attributes. A present one is
               a sequence, whose length is itself a read — of an engine-visible array in every real case, and of
               the page's object when it is not, which is why the whole walk is on the trampoline. */
            s->n = ce_sequence_length(ctx, s->raw);
        }
    }
    if (hdr->stage == CE_SEQUENCE) {
        r = ce_sequence_run(ctx, hdr, s, s->names, cb_result, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return -1;
        hdr->stage = CE_DISABLED;
    }
    if (hdr->stage == CE_DISABLED) {
        /* §4.13.4 step 14.7 — UNCONDITIONAL, unlike step 14.5's observedAttributes: `disabledFeatures` is read
           off every constructor whether or not the class declares one, and a page's static getter sees exactly
           that. Step 14.6's empty sequence is the Array the entry stage allocated. */
        JS_FreeValue(ctx, s->raw);
        s->raw = JS_UNDEFINED;
        r = step_getprop_run(ctx, hdr, argv[1], g_atom_disabled_src, cb_result, &s->raw, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return -1;
        s->i = 0;
        /* step 14.8: an absent (undefined) iterable leaves the sequence empty. */
        s->n = JS_IsUndefined(s->raw) ? 0 : ce_sequence_length(ctx, s->raw);
        hdr->stage = CE_DISABLED_SEQ;
    }
    if (hdr->stage == CE_DISABLED_SEQ) {
        r = ce_sequence_run(ctx, hdr, s, s->features, cb_result, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return -1;
        /* steps 14.9 and 14.10 — two reads of the one sequence, no page code between them. */
        if (ce_sequence_contains(ctx, s->features, "internals")) s->flags |= 1u << CE_DEF_DISABLE_INTERNALS;
        if (ce_sequence_contains(ctx, s->features, "shadow")) s->flags |= 1u << CE_DEF_DISABLE_SHADOW;
        hdr->stage = CE_FORM_FLAG;
    }
    if (hdr->stage == CE_FORM_FLAG) {
        JSValue raw = JS_UNDEFINED;

        r = step_getprop_run(ctx, hdr, argv[1], g_atom_form_assoc_src, cb_result, &raw, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;
        if (r < 0) return -1;
        /* step 14.12: converting to a Web IDL boolean is ToBoolean, which runs nothing — the READ above is the
           page's code, and it has already happened. */
        if (JS_ToBool(ctx, raw)) s->flags |= 1u << CE_DEF_FORM_ASSOCIATED;
        JS_FreeValue(ctx, raw);
        s->i = CE_CB_LIFECYCLE_COUNT;
        hdr->stage = (s->flags & (1u << CE_DEF_FORM_ASSOCIATED)) ? CE_FORM_CB : CE_COMMIT;
    }
    if (hdr->stage == CE_FORM_CB) {
        /* §4.13.4 step 14.13 — the same collection step 14.4 makes, over the second list, and reached only for
           a form-associated class. One key per step, off the PROTOTYPE, in the list's order. */
        while (s->i < CE_CB_COUNT) {
            JSValue v = JS_UNDEFINED;
            r = step_getprop_run(ctx, hdr, s->proto, g_cb_atoms[s->i], cb_result, &v, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r > 0) return r;          /* parked ON THIS KEY; the resume comes back to it */
            if (r < 0) return -1;
            if (!JS_IsUndefined(v)) {
                if (!JS_IsFunction(ctx, v)) {
                    JS_FreeValue(ctx, v);
                    JS_ThrowTypeError(ctx, "a custom element's %s is not callable", CE_CALLBACK_NAMES[s->i]);
                    return -1;
                }
                JS_SetPropertyUint32(ctx, s->callbacks, s->i, v);
            } else {
                JS_FreeValue(ctx, v);
            }
            s->i++;
        }
        hdr->stage = CE_COMMIT;
    }
    DCHECK(hdr->stage == CE_COMMIT, "customElements.define resumed into a stage §4.13.4 does not have");
    JS_FreeValue(ctx, cb_result);
    *presult = ce_define_commit(ctx, s->registry, argv, s->names, s->proto, s->callbacks, s->flags);
    if (JS_IsException(*presult)) { *presult = JS_UNDEFINED; return -1; }
    return 0;
}

static const IdlStepDecl CE_DEFINE_STEP = {
    js_ce_define, sizeof(CeDefineState), ce_define_visit, ce_define_release,
    "HTML §4.13.4 CustomElementRegistry.define", CE_DEFINE_STEPS
};

/* §4.13.4 get(name) — the constructor a name is defined as IN THIS REGISTRY, or undefined. */
static JSValue js_ce_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    const char *nm;
    size_t nlen;
    JSValue def, r;
    JSValueConst reg = this_val;

    (void)magic;
    if (!ce_registry_this(ctx, this_val)) return JS_EXCEPTION;
    if (argc < 1) return JS_UNDEFINED;
    nm = JS_ToCStringLen(ctx, &nlen, argv[0]);   /* a real string by now: the declaration converted it */
    if (!nm) return JS_EXCEPTION;
    def = ce_find_in(ctx, reg, nm, nlen);
    r = JS_IsObject(def) ? JS_GetProperty(ctx, def, g_atom_ctor) : JS_UNDEFINED;
    JS_FreeValue(ctx, def);
    JS_FreeCString(ctx, nm);
    return r;
}

/* §4.13.4 getName(constructor) — the NAME a constructor is defined as, or null. The inverse reading of the
   same set, and the reason the definition ORDER exists: a set keyed by name cannot be asked about a
   constructor. It was ABSENT, which for a page whose bundle registers a class in one module and asks for its
   tag in another is a `TypeError: not a function` on the line that asks. */
static JSValue js_ce_get_name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue def, r;

    (void)magic; (void)argc;
    if (!ce_registry_this(ctx, this_val)) return JS_EXCEPTION;
    def = ce_definition_by_ctor(ctx, this_val, argv[0]);
    r = JS_IsObject(def) ? JS_GetProperty(ctx, def, g_atom_name) : JS_NULL;
    JS_FreeValue(ctx, def);
    return r;
}

/* §4.13.4 `[CEReactions] undefined upgrade(Node root)` — the member a page calls to force the upgrade of a
   subtree it built while the definition was not yet registered. Two steps: collect root's inclusive descendant
   elements in tree order, and TRY TO UPGRADE each. Nothing here constructs — "try to upgrade" enqueues an
   upgrade reaction, and the `[CEReactions]` epilogue every declared member ends through is what drains it,
   which is exactly why this is an ordinary body and not a machine.
   THE DESCENDANTS ARE SHADOW-INCLUDING, in shadow-including tree order, which the standard says and which the
   walk here now does. It used to be a plain descendant walk with a comment saying the two were the same thing
   because this engine had no shadow trees; it has them, and the difference is the case the member exists for —
   a component's own shadow tree is precisely where the elements a page built before their definition arrived
   are, so `customElements.upgrade(host)` upgraded everything except them. */
static JSValue js_ce_upgrade(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_node_t *root, *n;
    JSValueConst reg = this_val;

    (void)magic; (void)argc;
    if (!ce_registry_this(ctx, this_val)) return JS_EXCEPTION;
    root = node_of(argv[0]);
    if (!root) return JS_ThrowTypeError(ctx, "customElements.upgrade requires a Node");
    for (n = root; n; n = shadow_root_next_in_shadow_including(ctx, n, root)) {
        lxb_dom_element_t *el;
        JSValue wrap, nreg;
        bool mine;

        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT)     /* step 1.1 */
            continue;
        el = lxb_dom_interface_element(n);
        /* The same cheap name test the insertion steps make, and for the same reason: reading an element's
           state means minting its WRAPPER, and `upgrade(document)` walks every node in the document. */
        if (!ce_upgradable_name(el))
            continue;
        wrap = node_wrap(ctx, n);
        /* step 1.2: "if candidate's custom element registry is not this, then continue". Without it a page
           holding both registries could upgrade a scoped tree's elements out of the document's set — the
           definitions are different classes and the wrong one would run. */
        nreg = ce_registry_of_node(ctx, wrap);
        mine = JS_VALUE_GET_PTR(nreg) == JS_VALUE_GET_PTR(reg) && JS_IsObject(nreg);
        JS_FreeValue(ctx, nreg);
        if (mine) ce_try_upgrade(ctx, el, wrap);      /* step 1.3 */
        JS_FreeValue(ctx, wrap);
    }
    return JS_UNDEFINED;
}

/* §4.13.4 `[CEReactions] undefined initialize(Node root)` — THE MEMBER THAT ASSOCIATES A SCOPED REGISTRY WITH A
   SUBTREE. It is what makes a scoped registry reach nodes that were created without one: DOM says a node's
   registry "intentionally cannot be changed any further" once it is set, so the only nodes this can claim are
   the ones whose registry is NULL — a Document that is not a Window's, a shadow root attached with
   `customElementRegistry: null`, an element created with the same option. Claiming a node whose registry is
   already this one still TRIES TO UPGRADE it, which is what makes `initialize` idempotent and useful after a
   later `define`.
   Nothing here constructs: "try to upgrade" enqueues an upgrade reaction and the `[CEReactions]` epilogue every
   declared member ends through is what drains it, which is exactly why this is an ordinary body. */
static JSValue js_ce_initialize(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst reg = this_val;
    lxb_dom_node_t *root, *n;
    bool scoped;

    (void)magic; (void)argc;
    if (!ce_registry_this(ctx, this_val)) return JS_EXCEPTION;
    root = node_of(argv[0]);
    if (!root) return JS_ThrowTypeError(ctx, "CustomElementRegistry.initialize requires a Node");
    scoped = ce_reg_flag(ctx, reg, g_atom_scoped);
    /* Step 1: a GLOBAL registry may only initialize within its own document, and never a Document node —
       a global registry is already every node's answer there, so the call could only ever be a no-op or a
       claim on a document that has one. */
    if (!scoped) {
        JSValue doc_reg = ce_registry_of_document(ctx, ce_node_document(root));
        bool ours = JS_VALUE_GET_PTR(doc_reg) == JS_VALUE_GET_PTR(reg) && JS_IsObject(doc_reg);

        JS_FreeValue(ctx, doc_reg);
        if (root->type == LXB_DOM_NODE_TYPE_DOCUMENT || !ours)
            return JS_ThrowDOMException(ctx, "NotSupportedError",
                                        "a non-scoped CustomElementRegistry can only initialize a node of its "
                                        "own document");
    }
    /* Steps 2-3: a Document or a ShadowRoot whose registry is null takes this one. Both are the same write on
       the same slot; which node kind it is decides only which of the two steps it is. */
    if (root->type == LXB_DOM_NODE_TYPE_DOCUMENT || shadow_root_is(root)) {
        JSValue wrap = node_wrap(ctx, root);
        JSValue cur = ce_registry_of_node(ctx, wrap);

        if (JS_IsNull(cur)) ce_node_set_registry(ctx, wrap, reg);
        JS_FreeValue(ctx, cur);
        JS_FreeValue(ctx, wrap);
    }
    /* Step 4: INCLUSIVE DESCENDANTS IN TREE ORDER — not shadow-including, which the spec is deliberate about:
       a shadow tree gets its own registry from its own `attachShadow`, and reaching into one here would
       overwrite the boundary the tree was built with. */
    for (n = root; n; n = node_next_in(n, root)) {
        JSValue wrap, cur;
        bool mine;

        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;   /* step 4.1 */
        wrap = node_wrap(ctx, n);
        cur = ce_registry_of_node(ctx, wrap);
        if (JS_IsNull(cur)) {                                 /* step 4.2 */
            ce_node_set_registry(ctx, wrap, reg);
            if (scoped) ce_scoped_docs_append(ctx, reg, n);
            mine = true;
        } else {
            mine = JS_VALUE_GET_PTR(cur) == JS_VALUE_GET_PTR(reg) && JS_IsObject(cur);
        }
        JS_FreeValue(ctx, cur);
        if (mine && ce_upgradable_name(lxb_dom_interface_element(n)))
            ce_try_upgrade(ctx, lxb_dom_interface_element(n), wrap);   /* steps 4.3-4.4 */
        JS_FreeValue(ctx, wrap);
    }
    return JS_UNDEFINED;
}

/* §4.13.4's CONSTRUCTOR: "the new CustomElementRegistry() constructor steps are to set this's is scoped to
   true". It runs none of the page's code — no argument, no read — so it is an ordinary constructor body and
   not a machine. It did not exist at all, which is what made the registry a single global one. */
static JSValue js_ce_registry_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    if (JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "constructor CustomElementRegistry requires 'new'");
    return ce_registry_new(ctx, true);
}

void custom_elements_init(JSContext *ctx)
{
    int k;

    DCHECK(!g_ready, "custom_elements_init ran twice — one instance is one document");
    g_atom_prototype = JS_NewAtom(ctx, "prototype");
    g_atom_ctor = JS_NewAtom(ctx, "ctor");
    g_atom_proto = JS_NewAtom(ctx, "proto");
    g_atom_observed = JS_NewAtom(ctx, "observed");
    g_atom_observed_src = JS_NewAtom(ctx, "observedAttributes");
    g_atom_callbacks = JS_NewAtom(ctx, "callbacks");
    g_atom_name = JS_NewAtom(ctx, "name");
    g_atom_local = JS_NewAtom(ctx, "local");
    g_atom_stack = JS_NewAtom(ctx, "stack");
    g_atom_flags = JS_NewAtom(ctx, "flags");
    g_atom_disabled_src = JS_NewAtom(ctx, "disabledFeatures");
    g_atom_form_assoc_src = JS_NewAtom(ctx, "formAssociated");
    CHECK(g_atom_flags != JS_ATOM_NULL && g_atom_disabled_src != JS_ATOM_NULL &&
          g_atom_form_assoc_src != JS_ATOM_NULL,
          "a §4.13.4 step 15 definition-flag atom could not be interned");
    CHECK(g_atom_prototype != JS_ATOM_NULL &&
          g_atom_ctor != JS_ATOM_NULL && g_atom_proto != JS_ATOM_NULL &&
          g_atom_observed != JS_ATOM_NULL && g_atom_observed_src != JS_ATOM_NULL &&
          g_atom_callbacks != JS_ATOM_NULL && g_atom_name != JS_ATOM_NULL &&
          g_atom_local != JS_ATOM_NULL && g_atom_stack != JS_ATOM_NULL,
          "a custom-element atom could not be interned");
    /* §4.13.5 step 2's slot key: a symbol the page cannot mint, so the element's definition is not a string
       property of this engine's invention sitting on every custom element. */
    g_def_key = JS_NewSymbol(ctx, "customElementDefinition", false);
    CHECK(!JS_IsException(g_def_key), "the custom-element definition slot key allocation failed");
    g_atom_def = JS_ValueToAtom(ctx, g_def_key);
    CHECK(g_atom_def != JS_ATOM_NULL, "the custom-element definition slot key could not be interned");
    g_state_key = JS_NewSymbol(ctx, "customElementState", false);
    CHECK(!JS_IsException(g_state_key), "the custom element state slot key allocation failed");
    g_atom_state = JS_ValueToAtom(ctx, g_state_key);
    CHECK(g_atom_state != JS_ATOM_NULL, "the custom element state slot key could not be interned");
    for (k = 0; k < CE_CB_COUNT; k++) {
        g_cb_atoms[k] = JS_NewAtom(ctx, CE_CALLBACK_NAMES[k]);
        CHECK(g_cb_atoms[k] != JS_ATOM_NULL, "a §4.13.4 step 14 lifecycle callback name could not be interned");
    }
    /* §4.13.4's registry as a real interface: a CLASS, so §3.7 gives every realm its own prototype through
       quickjs's own per-context slot, and the state under a symbol on the instance. */
    {
        JSClassDef d = { "CustomElementRegistry" };
        JS_NewClassID(JS_GetRuntime(ctx), &g_registry_class);
        CHECK(JS_NewClass(JS_GetRuntime(ctx), g_registry_class, &d) == 0,
              "the CustomElementRegistry class could not be declared");
    }
    g_reg_key = JS_NewSymbol(ctx, "customElementRegistryRecord", false);
    CHECK(!JS_IsException(g_reg_key), "the CustomElementRegistry record slot key allocation failed");
    g_atom_reg = JS_ValueToAtom(ctx, g_reg_key);
    g_node_reg_key = JS_NewSymbol(ctx, "nodeCustomElementRegistry", false);
    CHECK(!JS_IsException(g_node_reg_key), "the node custom element registry slot key allocation failed");
    g_atom_node_reg = JS_ValueToAtom(ctx, g_node_reg_key);
    g_atom_defs = JS_NewAtom(ctx, "defs");
    g_atom_order = JS_NewAtom(ctx, "order");
    g_atom_whendef = JS_NewAtom(ctx, "whenDefined");
    g_atom_scoped = JS_NewAtom(ctx, "scoped");
    g_atom_docs = JS_NewAtom(ctx, "docs");
    g_atom_defining = JS_NewAtom(ctx, "defining");
    CHECK(g_atom_reg != JS_ATOM_NULL && g_atom_node_reg != JS_ATOM_NULL && g_atom_defs != JS_ATOM_NULL &&
          g_atom_order != JS_ATOM_NULL && g_atom_whendef != JS_ATOM_NULL && g_atom_scoped != JS_ATOM_NULL &&
          g_atom_docs != JS_ATOM_NULL && g_atom_defining != JS_ATOM_NULL,
          "a §4.13.4 CustomElementRegistry field name could not be interned");
    g_registry_slot = realm_value_declare(ctx, "§4.13.4 the Document's CustomElementRegistry");
    g_html_ctor_slot = realm_value_declare(ctx, "§4.13.2's active function object (HTMLElement)");
    /* §4.13.4's active custom element constructor map is the AGENT's, not a realm's — a class defined in one
       realm's scoped registry and constructed from another must find the same entry. */
    g_active_ctor_map = JS_NewArray(ctx);
    CHECK(!JS_IsException(g_active_ctor_map),
          "§4.13.4's active custom element constructor map could not be allocated");
    /* §4.13.6's stack, its backup queue and the two private keys the queues are read through. Built here, in
       the agent's own pre-boot realm, so a flow's push is captured by the heap COW rather than being that
       flow's private object. */
    g_rq_key = JS_NewSymbol(ctx, "customElementReactionQueue", false);
    CHECK(!JS_IsException(g_rq_key), "the custom element reaction queue slot key allocation failed");
    g_atom_rq = JS_ValueToAtom(ctx, g_rq_key);
    g_atom_rq_head = JS_NewAtom(ctx, "head");
    g_atom_backup_flag = JS_NewAtom(ctx, "processing");
    CHECK(g_atom_rq != JS_ATOM_NULL && g_atom_rq_head != JS_ATOM_NULL && g_atom_backup_flag != JS_ATOM_NULL,
          "a §4.13.6 element queue key could not be interned");
    g_ce_backup = JS_NewArray(ctx);
    CHECK(!JS_IsException(g_ce_backup), "the backup element queue could not be allocated");
    JS_SetProperty(ctx, g_ce_backup, g_atom_backup_flag, JS_FALSE);
    g_backup_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_ce_backup_def);
    /* The backup drain is a step function object nobody installs, so a page can neither see it nor replace
       it — the same reason the internal event dispatcher is not on any prototype. */
    /* §4.13.4's SIX members, DECLARED once per agent and installed on the per-realm PROTOTYPE: a declaration
       made where they are installed would mint them again for every realm — and now also for every scoped
       registry a page constructs, which is what makes the prototype the only place they can live. */
    g_id_define = idl_method_id_step(ctx, CE_DEFINE_ARGS, 3, CE_DEFINE_OPTS,
                                     (int)(sizeof(CE_DEFINE_OPTS) / sizeof(CE_DEFINE_OPTS[0])),
                                     &CE_DEFINE_STEP, 0);
    idl_optional_from(2);   /* §4.13.4: `define(name, constructor, optional ElementDefinitionOptions options)` */
    {
        static const IdlArgType ONE_STR[1] = { IDL_DOMSTRING };
        g_id_get = idl_method_id(ctx, ONE_STR, 1, js_ce_get, 0);
        g_id_when_defined = idl_method_id(ctx, ONE_STR, 1, js_ce_when_defined, 0);
    }
    {
        /* `getName(CustomElementConstructor constructor)` — a callback function type, whose conversion IS a
           brand check and nothing more (Web IDL §3.2.19): a non-callable is a TypeError before step 1. */
        static const IdlArgType ONE_CB[1] = { IDL_CALLBACK };
        g_id_get_name = idl_method_id(ctx, ONE_CB, 1, js_ce_get_name, 0);
    }
    {
        /* `upgrade(Node root)` / `initialize(Node root)` — the argument's INTERFACE type is what makes
           `customElements.upgrade({})` a TypeError before step 1, rather than a body's hand-written check. */
        static const IdlArgType ONE_NODE[1] = { IDL_INTERFACE };
        g_id_upgrade = idl_method_id(ctx, ONE_NODE, 1, js_ce_upgrade, 0);
        idl_iface_brand(node_class_id());
        g_id_initialize = idl_method_id(ctx, ONE_NODE, 1, js_ce_initialize, 0);
        idl_iface_brand(node_class_id());
    }
    realm_declare_intrinsic(custom_elements_install_proto);
    /* §4.13.2, DECLARED ONCE PER AGENT and minted per realm: HTMLElement is a per-realm interface object, so a
       declaration made where it is installed would mint the member again for every document. */
    g_id_html_ctor = idl_method_id_step(ctx, NULL, 0, NULL, 0, &CE_HTML_CTOR_STEP, 0);
    g_ready = 1;
}

void custom_elements_mark_failed(JSContext *ctx, JSValueConst wrap)
{
    DCHECK(g_ready, "an element was marked failed before custom_elements_init declared the state slot");
    ce_set_state(ctx, wrap, CE_STATE_FAILED);
}

void custom_elements_install(JSContext *ctx, JSValueConst global)
{
    JSValue reg;

    DCHECK(g_ready, "customElements was installed before custom_elements_init ran");
    /* §4.13.4 step 14's map NAMES `connectedMoveCallback` exactly on a platform that has `moveBefore` — the two
       move together, and neither half is safe alone: a key collected for an operation this engine cannot
       perform shows a page's Proxy a read for a callback nothing will invoke, and the operation without the key
       silently drops the reaction it is supposed to fire. Asserted where Element's prototype exists, so
       building moveBefore CRASHES here rather than leaving the pairing to a comment. */
    {
        JSValue ep = element_proto(ctx), mb;
        JSAtom a = JS_NewAtom(ctx, "moveBefore");
        int has;
        CHECK(a != JS_ATOM_NULL, "custom elements: `moveBefore` could not be interned");
        DCHECK(JS_IsObject(ep), "customElements was installed before Element.prototype existed — §4.13.4's "
                                "callback list is paired with what Element can do, and that pairing cannot be "
                                "checked against a prototype that is not there yet");
        has = JS_GetOwnSlot(ctx, &mb, ep, a);
        if (has > 0) JS_FreeValue(ctx, mb);
        JS_FreeAtom(ctx, a);
        JS_FreeValue(ctx, ep);
        DCHECK(has <= 0, "Element has moveBefore, so HTML §4.13.4 step 14's lifecycle callback map must name "
                         "`connectedMoveCallback` between disconnectedCallback and adoptedCallback — add it to "
                         "CE_LIFECYCLE_CALLBACKS and enqueue it from the move");
    }
    /* §4.13.4: "A Window's associated Document is ALWAYS created with a new CustomElementRegistry object", and
       this is that creation. Built for the agent's own realm while it is still pre-boot, so a definition a flow
       adds to it is captured by the heap COW rather than being that flow's private object. `is scoped` is
       false: this is the one registry the constructor cannot produce. */
    reg = ce_registry_new(ctx, false);
    realm_value_set(ctx, g_registry_slot, JS_DupValue(ctx, reg));
    /* §4.13.4's Window `customElements` getter returns "this's associated Document's custom element registry",
       and a realm has exactly one document for its whole life — so the value is fixed and the accessor would
       compute the same answer forever. */
    JS_SetPropertyStr(ctx, (JSValue)global, "customElements", reg);
    /* §3.7.1's INTERFACE OBJECT — constructible now, which is the whole of `new CustomElementRegistry()`. */
    {
        JSValue ctor = JS_NewCFunction2(ctx, (JSCFunction *)js_ce_registry_ctor, "CustomElementRegistry", 0,
                                        JS_CFUNC_constructor, 0);
        JSValue proto = JS_GetClassProto(ctx, g_registry_class);

        CHECK(!JS_IsException(ctor), "the CustomElementRegistry interface object could not be allocated");
        DCHECK(!JS_IsNull(proto), "CustomElementRegistry was installed into a realm that never ran its proto "
                                  "build");
        JS_SetConstructor(ctx, ctor, proto);
        JS_FreeValue(ctx, proto);
        JS_SetPropertyStr(ctx, (JSValue)global, "CustomElementRegistry", ctor);
    }
}

/* §4.13.4's INTERFACE PROTOTYPE OBJECT, FOR ONE REALM. The members live HERE and not on the instance, which is
   what Web IDL says and what a second registry made this observable: installed per object, every
   `new CustomElementRegistry()` would carry its own function objects, `customElements.define ===
   otherRegistry.define` would be false, and a page patching the prototype would reach none of them. */
void custom_elements_install_proto(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_registry_class != 0, "a realm asked for CustomElementRegistry.prototype before the class existed");
    prev = JS_GetClassProto(ctx, g_registry_class);
    DCHECK(JS_IsNull(prev), "custom_elements_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "CustomElementRegistry.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "CustomElementRegistry");
    idl_install_method(ctx, proto, "define", 2, g_id_define);
    idl_install_method(ctx, proto, "get", 1, g_id_get);
    idl_install_method(ctx, proto, "getName", 1, g_id_get_name);
    idl_install_method(ctx, proto, "whenDefined", 1, g_id_when_defined);
    idl_install_method(ctx, proto, "upgrade", 1, g_id_upgrade);
    idl_install_method(ctx, proto, "initialize", 1, g_id_initialize);
    JS_SetClassProto(ctx, g_registry_class, proto);
}

void custom_elements_free(JSRuntime *rt)
{
    int k;

    if (!g_ready) return;
    /* the registries are the REALMS' — released with their contexts */
    JS_FreeValueRT(rt, g_ce_backup);
    JS_FreeValueRT(rt, g_rq_key);
    JS_FreeValueRT(rt, g_active_ctor_map);
    JS_FreeValueRT(rt, g_reg_key);
    JS_FreeValueRT(rt, g_node_reg_key);
    g_ce_backup = g_rq_key = JS_UNDEFINED;
    g_active_ctor_map = g_reg_key = g_node_reg_key = JS_UNDEFINED;
    JS_FreeAtomRT(rt, g_atom_reg);
    JS_FreeAtomRT(rt, g_atom_node_reg);
    JS_FreeAtomRT(rt, g_atom_defs);
    JS_FreeAtomRT(rt, g_atom_order);
    JS_FreeAtomRT(rt, g_atom_whendef);
    JS_FreeAtomRT(rt, g_atom_scoped);
    JS_FreeAtomRT(rt, g_atom_docs);
    JS_FreeAtomRT(rt, g_atom_defining);
    g_atom_reg = g_atom_node_reg = g_atom_defs = g_atom_order = g_atom_whendef = JS_ATOM_NULL;
    g_atom_scoped = g_atom_docs = g_atom_defining = JS_ATOM_NULL;
    g_current = NULL;
    JS_FreeAtomRT(rt, g_atom_rq);
    JS_FreeAtomRT(rt, g_atom_rq_head);
    JS_FreeAtomRT(rt, g_atom_backup_flag);
    g_atom_rq = g_atom_rq_head = g_atom_backup_flag = JS_ATOM_NULL;
    g_backup_stepid = -1;
    JS_FreeAtomRT(rt, g_atom_prototype);
    JS_FreeAtomRT(rt, g_atom_ctor);
    JS_FreeAtomRT(rt, g_atom_proto);
    JS_FreeAtomRT(rt, g_atom_observed);
    JS_FreeAtomRT(rt, g_atom_observed_src);
    JS_FreeAtomRT(rt, g_atom_callbacks);
    JS_FreeAtomRT(rt, g_atom_name);
    JS_FreeAtomRT(rt, g_atom_local);
    JS_FreeAtomRT(rt, g_atom_stack);
    JS_FreeAtomRT(rt, g_atom_flags);
    JS_FreeAtomRT(rt, g_atom_disabled_src);
    JS_FreeAtomRT(rt, g_atom_form_assoc_src);
    g_atom_flags = g_atom_disabled_src = g_atom_form_assoc_src = JS_ATOM_NULL;
    g_atom_name = g_atom_local = g_atom_stack = JS_ATOM_NULL;
    JS_FreeAtomRT(rt, g_atom_def);
    JS_FreeValueRT(rt, g_def_key);
    g_def_key = JS_UNDEFINED;
    JS_FreeAtomRT(rt, g_atom_state);
    g_atom_state = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_state_key);
    g_state_key = JS_UNDEFINED;
    for (k = 0; k < CE_CB_COUNT; k++) {
        JS_FreeAtomRT(rt, g_cb_atoms[k]);
        g_cb_atoms[k] = JS_ATOM_NULL;
    }
    g_atom_prototype = g_atom_def = JS_ATOM_NULL;
    g_atom_ctor = g_atom_proto = g_atom_observed = g_atom_observed_src = JS_ATOM_NULL;
    g_atom_callbacks = JS_ATOM_NULL;
    g_ready = 0;
}
