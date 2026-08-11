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
 * WHAT IS HONESTLY ABSENT: adoptedCallback (no adoption reaction yet), form-associated custom elements
 * (§4.13.5 step 10) and customized built-ins (`extends`), which §4.13.4 rejects here rather than registering as
 * autonomous — a silently-wrong registration is worse than a named refusal. */
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
#include "core/html/html_element.h"
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
/* §4.13.4: A REGISTRY IS PER WINDOW, AND SO IS ITS DEFINITION SET. One set for the agent meant
   `frame.contentWindow.customElements.define('x-a', C)` defined `x-a` in the PARENT too, and the parent's
   `customElements.get('x-a')` then handed back the child realm's constructor — a cross-realm constructor
   handed out by a member that never crossed a boundary. */
static int g_defs_slot = -1;

/* THE SAME SET, IN DEFINITION ORDER — because §4.13.2 step 3 looks a definition up BY ITS CONSTRUCTOR and the
   object above is keyed by NAME. A JS object cannot be keyed by a constructor (it is the page's function
   object, and hanging an engine-minted symbol on it would put a slot on a page value that
   `Object.getOwnPropertySymbols` reports), so the by-constructor question is answered by walking the set —
   which is what §4.13.2 states it is. The keyed object is the NAME INDEX into this list, not a second set: both
   are written by the one commit and hold the same definition objects, and a definition in one and not the other
   is a DCHECK at the walk. An Array rather than a C list for the reason every queue here is one — it forks per
   flow and parks with the flow that defined it. */
static int g_deflist_slot = -1;

/* §4.13.2's ACTIVE FUNCTION OBJECT, for the one interface that carries `[HTMLConstructor]` today. Step 5 is
   "the active function object must be HTMLElement" for an autonomous custom element, and that is an IDENTITY
   question about a PER-REALM object — a module static holding one realm's HTMLElement would answer it wrong
   for every other document, which is the defect class §3.7 names. Set by the mint below, which is what
   html_element.c calls to build the interface object. */
static int g_html_ctor_slot = -1;

/* §4.13.4's WHEN-DEFINED PROMISE MAP, per realm for exactly the reason the definition set is: the promise
   `whenDefined` hands back is settled by a `define` in THAT window, and one map for the agent would settle a
   parent's promise on a child's registration. A plain null-prototyped object keyed by name, whose values are
   one promise and the two halves of its capability as a three-element Array — a JS value, so it forks per flow
   and parks with the flow that is waiting, which a malloc'd list of pending promises could do neither of. */
static int g_whendef_slot = -1;

/* THIS REALM'S definition set. OWNED: the caller frees. */
static JSValue ce_defs(JSContext *ctx)
{
    DCHECK(g_defs_slot > 0, "a custom-element definition was reached before custom_elements_init declared the set");
    return realm_value_get(ctx, g_defs_slot);
}

/* THIS REALM'S when-defined promise map. OWNED: the caller frees. */
static JSValue ce_whendef(JSContext *ctx)
{
    DCHECK(g_whendef_slot > 0,
           "§4.13.4's when-defined promise map was reached before custom_elements_init declared it");
    return realm_value_get(ctx, g_whendef_slot);
}

/* THIS REALM'S definition set in definition order. OWNED: the caller frees. */
static JSValue ce_deflist(JSContext *ctx)
{
    DCHECK(g_deflist_slot > 0, "a custom-element definition was reached before custom_elements_init declared "
                               "the ordered set §4.13.2 step 3 walks");
    return realm_value_get(ctx, g_deflist_slot);
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
   The form-associated four (step 14.12) are a different map, collected only when `formAssociated` converts to
   true — a step this component has not built, so they are honestly absent rather than half-collected here. */
#define CE_LIFECYCLE_CALLBACKS(X) \
    X(CE_CB_CONNECTED,    "connectedCallback") \
    X(CE_CB_DISCONNECTED, "disconnectedCallback") \
    X(CE_CB_ADOPTED,      "adoptedCallback") \
    X(CE_CB_ATTR_CHANGED, "attributeChangedCallback")
#define CE_CB_ID(id, name)   id,
#define CE_CB_NAME(id, name) name,
enum { CE_LIFECYCLE_CALLBACKS(CE_CB_ID) CE_CB_COUNT };
static const char *const CE_CALLBACK_NAMES[CE_CB_COUNT] = { CE_LIFECYCLE_CALLBACKS(CE_CB_NAME) };
static JSAtom g_cb_atoms[CE_CB_COUNT];
static int    g_id_define, g_id_get, g_id_when_defined;   /* declared once per agent — see custom_elements_init */

/* The definition for a name, or JS_UNDEFINED. OWNED by the caller. */
static JSValue ce_find(JSContext *ctx, const char *name, size_t len)
{
    JSAtom a = JS_NewAtomLen(ctx, name, len);
    JSValue def;

    CHECK(a != JS_ATOM_NULL, "custom elements: a name could not be interned");
    {
        JSValue defs = ce_defs(ctx);
        def = JS_GetProperty(ctx, defs, a);
        JS_FreeValue(ctx, defs);
    }
    JS_FreeAtom(ctx, a);
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
static bool ce_name_valid(const char *name, size_t len)
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
        if (tag && len && ce_name_valid((const char *)tag, len)) return CE_STATE_UNDEFINED;
    }
    return CE_STATE_UNCUSTOMIZED;
}

/* CONFIGURABLE AND WRITABLE, because §4.13.5 writes the state THREE TIMES on its way through one element
   ("failed", then "precustomized", then "custom") and the definition it writes at step 2 is DELETED again by
   step 8.9.1. A slot defined with no flags is none of those things, and the second write is then a silent
   no-op that leaves an element reporting the state it had two steps ago. The key is a symbol this component
   minted and never published, so nothing outside can reach either slot however they are flagged. */
#define CE_SLOT_FLAGS (JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE)

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
static JSValue g_backup_fn = JS_UNDEFINED;

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
    DCHECK(JS_IsObject(g_backup_fn),
           "a reaction reached the backup element queue before custom_elements_init built its microtask driver");
    JS_EnqueueCallJob(ctx, g_backup_fn, 0, NULL);
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

void custom_elements_queue_release(JSContext *ctx, CustomElementQueue *q)
{
    int k;
    JS_FreeValue(ctx, q->queue);
    q->queue = JS_UNDEFINED;
    JS_FreeValue(ctx, q->exc);
    q->exc = JS_UNDEFINED;
    q->reporting = 0;
    JS_FreeValue(ctx, q->cur);
    JS_FreeValue(ctx, q->cur_el);
    q->cur = q->cur_el = JS_UNDEFINED;
    report_exception_work_release(ctx, &q->rep);
    for (k = 0; k < 2 + CE_MAX_REACTION_ARGS; k++) {
        JS_FreeValue(ctx, q->cb[k]);
        q->cb[k] = JS_UNDEFINED;
    }
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
           node instead of making a second one. Steps 7.5-7.6's active custom element constructor map is a
           scoped-registry mechanism and there are no scoped registries, so there is nothing to save. */
        stack = JS_GetProperty(ctx, def, g_atom_stack);
        DCHECK(JS_IsObject(stack), "a custom element definition carries no §4.13.2 construction stack");
        ce_array_push(ctx, stack, JS_DupValue(ctx, el));
        JS_FreeValue(ctx, stack);
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
    /* step 11. Step 10's form-associated half is absent with formAssociated itself. */
    ce_set_state(ctx, el, CE_STATE_CUSTOM);
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
    custom_elements_queue_release(ctx, &s->q);
    return JS_UNDEFINED;
}

static int js_ce_backup_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSCeBackup *s = st;
    int r;

    DCHECK(s->hdr.stage >= CEBACKUP_CALLBACK && s->hdr.stage <= CEBACKUP_REPORT,
           "the backup element queue's drain resumed into a stage §4.13.6 does not have");
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

JSValue custom_elements_definition_for_name(JSContext *ctx, const char *name, size_t len)
{
    if (!g_ready) return JS_UNDEFINED;
    return ce_find(ctx, name, len);
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
    /* steps 5.1.4.9-11 set the namespace prefix, the is value and the registry. The prefix is already null
       (the creation passed none), the is value is null for an autonomous element, and scoped registries are
       not built — so all three are already what these steps set, and writing them would be writing the value
       that is there. When any of the three becomes expressible this is where it goes. */
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
    JSValue def;     /* step 3's definition (owned) */
    JSValue proto;   /* step 8's answer (owned) */
} CeHtmlCtorState;

static void ce_html_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    CeHtmlCtorState *s = st;
    v->val(ctx, &s->def);
    v->val(ctx, &s->proto);
}

static void ce_html_ctor_release(JSContext *ctx, void *st)
{
    CeHtmlCtorState *s = st;
    JS_FreeValue(ctx, s->def);
    JS_FreeValue(ctx, s->proto);
    s->def = s->proto = JS_UNDEFINED;
}

/* §4.13.2 step 3: the entry in the definition set whose CONSTRUCTOR is `ctor`. A walk of the ordered set,
   which is what the spec's own wording is; the name-keyed index above cannot answer this question at all.
   UNDEFINED when there is none, which is step 3's TypeError. OWNED. */
static JSValue ce_definition_by_ctor(JSContext *ctx, JSValueConst ctor)
{
    JSValue list = ce_deflist(ctx), found = JS_UNDEFINED;
    uint32_t n, i;

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
        s->def = s->proto = JS_UNDEFINED;
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
        /* step 3: the definition whose constructor is NewTarget. */
        s->def = ce_definition_by_ctor(ctx, ntgt);
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
    js_ce_html_ctor, sizeof(CeHtmlCtorState), ce_html_ctor_visit, ce_html_ctor_release,
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

/* §4.13.5 "try to upgrade an element": look the definition up by the element's local name and, if there is one,
   enqueue an upgrade reaction. No state is read here — §4.13.5 step 1 is the one that decides whether an
   element already past "undefined" is upgraded again, and it is read AT THE UPGRADE because a constructor
   running between the enqueue and the drain can change the answer. */
static void ce_try_upgrade(JSContext *ctx, lxb_dom_element_t *el, JSValueConst wrap)
{
    size_t len = 0;
    const lxb_char_t *tag = lxb_dom_element_local_name(el, &len);
    JSValue def;

    if (!tag || !len) return;
    def = ce_find(ctx, (const char *)tag, len);
    if (JS_IsObject(def)) ce_enqueue_upgrade(ctx, wrap, def);
    JS_FreeValue(ctx, def);
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
    return tag != NULL && len != 0 && ce_name_valid((const char *)tag, len);
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
    }
    JS_FreeValue(ctx, wrap);
}

void custom_elements_element_connected(JSContext *ctx, lxb_dom_element_t *el)
{
    JSValue wrap;

    if (!g_ready || !ce_upgradable_name(el)) return;
    wrap = node_wrap(ctx, lxb_dom_interface_node(el));
    if (!JS_IsObject(wrap)) { JS_FreeValue(ctx, wrap); return; }
    /* DOM §4.2.3's insertion steps, custom-element half: an element that is already CUSTOM gets a connected
       reaction — which is how a page that moves a node around keeps its lifecycle running — and any other
       element is tried for upgrade, whose own step 5 enqueues that same reaction if it succeeds. Doing both
       would run connectedCallback twice for a freshly upgraded element. */
    if (ce_state_of(ctx, wrap) == CE_STATE_CUSTOM) {
        JSValue def = ce_definition_of(ctx, wrap);
        ce_enqueue(ctx, wrap, def, CE_CB_CONNECTED);
        JS_FreeValue(ctx, def);
    } else {
        ce_try_upgrade(ctx, el, wrap);
    }
    JS_FreeValue(ctx, wrap);
}

/* §4.13.4 step 18: define() enqueues an upgrade reaction for every EXISTING matching element, not only the ones
   inserted later — a definition that arrives after the parser is the ordinary case for a deferred bundle. The
   upgrades then run at define()'s own `[CEReactions]` boundary, which is what makes `customElements.define(…)`
   followed by a read of state the constructor set work on the next line. */
static void ce_upgrade_document(JSContext *ctx, const char *name, size_t nlen, JSValueConst def)
{
    lxb_dom_node_t *root = document_root_node(ctx), *n;
    size_t len = 0;

    if (!root) return;
    for (n = root; n; ) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            const lxb_char_t *tag = lxb_dom_element_local_name(lxb_dom_interface_element(n), &len);
            if (tag && len == nlen && memcmp(tag, name, len) == 0) {
                JSValue wrap = node_wrap(ctx, n);
                ce_enqueue_upgrade(ctx, wrap, def);
                JS_FreeValue(ctx, wrap);
            }
        }
        if (n->first_child) { n = n->first_child; continue; }
        while (n && !n->next) n = (n == root) ? NULL : n->parent;
        n = n ? n->next : NULL;
    }
}

/* §4.13.3 "attribute changed": the reaction runs only for a name the definition declared as OBSERVED, which is
   why observedAttributes is read at define time and stored — a class watching two attributes must not have its
   callback run for the other fifty a page writes. Four arguments, which is what makes the generalised reaction
   carry an argument vector rather than a name alone.
   THE OBSERVED SET IS OVER LOCAL NAMES, and the old value is read by §4.9's own identity: a qualified-name read
   would answer with whichever attribute happens to print that name FIRST, which for a prefixed attribute is a
   different attribute than the one being written. */
void custom_elements_attribute_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local,
                                       const char *val, size_t val_len)
{
    JSValue wrap, def, args[4];
    size_t old_len = 0;
    const lxb_char_t *old;
    lxb_dom_attr_t *old_attr;
    uint32_t i;

    if (!g_ready) return;
    wrap = node_wrap(ctx, lxb_dom_interface_node(el));
    def = ce_definition_of(ctx, wrap);
    if (!JS_IsObject(def)) { JS_FreeValue(ctx, def); JS_FreeValue(ctx, wrap); return; }
    /* §4.13.3's arguments: (localName, oldValue, newValue, namespace). An attribute that was absent has a NULL
       old value and an attribute being removed a NULL new one, and the page's code branches on exactly that;
       the namespace is null for every attribute an HTML page writes and a URI for the ones the parser moved. */
    old_attr = dom_attr_get_ns(el, ns, local);
    old = old_attr ? lxb_dom_attr_value(old_attr, &old_len) : NULL;
    args[0] = JS_NewString(ctx, local);
    args[1] = old ? JS_NewStringLen(ctx, (const char *)old, old_len) : JS_NULL;
    args[2] = val ? JS_NewStringLen(ctx, val, val_len) : JS_NULL;
    args[3] = ns ? JS_NewString(ctx, ns) : JS_NULL;
    ce_enqueue_args(ctx, wrap, def, CE_CB_ATTR_CHANGED, 4, (JSValueConst *)args);
    for (i = 0; i < 4; i++) JS_FreeValue(ctx, args[i]);
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
    JSAtom a;

    (void)this_val; (void)magic; (void)argc;
    nm = JS_ToCStringLen(ctx, &nlen, argv[0]);   /* a real string by now: the declaration converted it */
    if (!nm) return JS_EXCEPTION;
    if (!ce_name_valid(nm, nlen)) {              /* step 1: a REJECTED promise, never a synchronous throw */
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
    def = ce_find(ctx, nm, nlen);                /* step 2: already defined — resolved with the constructor */
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
    map = ce_whendef(ctx);
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
static void ce_when_defined_resolve(JSContext *ctx, const char *name, size_t nlen, JSValueConst ctor)
{
    JSAtom a = JS_NewAtomLen(ctx, name, nlen);
    JSValue map, entry;

    CHECK(a != JS_ATOM_NULL, "custom elements: a name could not be interned");
    map = ce_whendef(ctx);
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
    X(CE_CHECKS,    "HTML §4.13.4 steps 1-7 (IsConstructor; a valid custom element name; the name is not " \
                    "already defined; `extends`)") \
    X(CE_PROTOTYPE, "HTML §4.13.4 steps 14.1-14.2 (Get(constructor, \"prototype\"); it must be an Object)") \
    X(CE_CALLBACKS, "HTML §4.13.4 step 14.4 (Get(prototype, callbackName) for each key of lifecycleCallbacks, " \
                    "in the map's order, converting each to the Function callback type), one key per step") \
    X(CE_OBSERVED,  "HTML §4.13.4 step 14.5.1 (Get(constructor, \"observedAttributes\"), reached only when " \
                    "step 14.4 collected an attributeChangedCallback)") \
    X(CE_SEQUENCE,  "HTML §4.13.4 step 14.5.2 (converting it to a sequence<DOMString>), one entry per step") \
    X(CE_COMMIT,    "HTML §4.13.4 steps 15-16 and 18 (the definition, and the upgrade reaction for each " \
                    "candidate)")
enum { IDL_STEP_STAGE_BASE(CE_DEFINE_STAGES) CE_DEFINE_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const CE_DEFINE_STEPS[] = { CE_DEFINE_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    uint32_t i, n;      /* THE RESUME POINT: step 14.4's callback key, then the observed-attribute entry */
    JSValue  proto;     /* step 14.1's answer (owned) */
    JSValue  callbacks; /* step 14.4's map, indexed by CE_CB_* (owned) */
    JSValue  raw;       /* what the observedAttributes getter answered (owned) */
    JSValue  names;     /* the converted sequence<DOMString> (owned) */
} CeDefineState;

static void ce_define_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    CeDefineState *s = st;
    v->val(ctx, &s->proto);
    v->val(ctx, &s->callbacks);
    v->val(ctx, &s->raw);
    v->val(ctx, &s->names);
}

static void ce_define_release(JSContext *ctx, void *st)
{
    CeDefineState *s = st;
    JS_FreeValue(ctx, s->proto);
    JS_FreeValue(ctx, s->callbacks);
    JS_FreeValue(ctx, s->raw);
    JS_FreeValue(ctx, s->names);
    s->proto = s->callbacks = s->raw = s->names = JS_UNDEFINED;
}

/* §4.13.4 STEPS 1-7 — everything the spec decides BEFORE it touches the page's object. Its own function
   because its own STAGE: running it after the reads, which is where it used to live, made the page's getters
   observe a call the spec had already rejected. Returns <0 having thrown. */
static int ce_define_checks(JSContext *ctx, int argc, JSValueConst *argv)
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
    /* steps 6-7: customized built-ins. Rejected rather than registered as autonomous — quietly treating
       `{extends:'button'}` as a new tag would define something the page never asked for and leave the button
       it did ask for un-upgraded. */
    ext = idl_dict_get(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, "extends");
    if (JS_IsString(ext)) {
        JS_FreeValue(ctx, ext);
        JS_ThrowDOMException(ctx, "NotSupportedError",
                             "a customized built-in (`extends`) is not modelled: this engine has no built-in "
                             "element to customize yet, and registering it as an autonomous element would "
                             "define a tag the page never asked for");
        return -1;
    }
    JS_FreeValue(ctx, ext);

    nm = JS_ToCStringLen(ctx, &nlen, argv[0]);   /* a real string by now: the declaration converted it */
    if (!nm) return -1;
    if (!ce_name_valid(nm, nlen)) {              /* step 2 */
        JS_FreeCString(ctx, nm);
        JS_ThrowDOMException(ctx, "SyntaxError", "not a valid custom element name");
        return -1;
    }
    prev = ce_find(ctx, nm, nlen);               /* step 3 */
    taken = JS_IsObject(prev);
    JS_FreeValue(ctx, prev);
    JS_FreeCString(ctx, nm);
    if (taken) {
        JS_ThrowDOMException(ctx, "NotSupportedError", "this name is already defined");
        return -1;
    }
    return 0;
}

/* §4.13.4 STEPS 15-16 AND 18 — the definition, the definition set, and the upgrade reactions. Plain C, and it
   stays that way: every value it needs is already real, and this is the part that touches only the component's
   own state. `proto` is step 14.1's answer, read as a request rather than here. */
static JSValue ce_define_commit(JSContext *ctx, JSValueConst *argv, JSValueConst names, JSValueConst proto,
                                JSValueConst callbacks)
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
        a = JS_NewAtomLen(ctx, nm, nlen);
        CHECK(a != JS_ATOM_NULL, "custom elements: a name could not be interned");
        {
            JSValue defs = ce_defs(ctx);
            JSValue list = ce_deflist(ctx);
            /* ONE SET, TWO READINGS: the name index the lookups use, and the definition ORDER §4.13.2 step 3
               walks to answer "which definition has this constructor". Written together, here, because a
               definition in one and not the other is a definition half the platform can see. */
            JS_SetProperty(ctx, defs, a, JS_DupValue(ctx, def));
            ce_array_push(ctx, list, JS_DupValue(ctx, def));
            JS_FreeValue(ctx, list);
            JS_FreeValue(ctx, defs);
        }
        JS_FreeAtom(ctx, a);
        ce_upgrade_document(ctx, nm, nlen, def);
        ce_when_defined_resolve(ctx, nm, nlen, argv[1]);
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

    if (hdr->stage == CE_CHECKS) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* EVERY owned field before the first thing that can throw: the failure path tears this state down
           through ce_define_release, which frees exactly what the state holds and nothing else. */
        s->proto = s->raw = JS_UNDEFINED;
        s->callbacks = JS_NewArray(ctx);
        s->names = JS_NewArray(ctx);
        CHECK(!JS_IsException(s->callbacks) && !JS_IsException(s->names),
              "custom elements: OOM building §4.13.4 step 14's callback map — a definition with no callbacks "
              "is a class whose lifecycle code never runs");
        /* `define(name, constructor, optional options)` declares two required arguments, and Web IDL's own
           count check has already thrown for a call with fewer — the body's copy of it was unreachable. */
        DCHECK(argc >= 2, "customElements.define reached its algorithm with fewer than the two arguments its "
                          "declaration requires — the count check belongs to Web IDL and it did not run");
        if (ce_define_checks(ctx, argc, argv) < 0) return -1;
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
        while (s->i < CE_CB_COUNT) {
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
            hdr->stage = CE_COMMIT;
        } else {
            r = step_getprop_run(ctx, hdr, argv[1], g_atom_observed_src, cb_result, &s->raw, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r > 0) return r;
            if (r < 0) return -1;
            hdr->stage = CE_SEQUENCE;
            /* §4.13.4: absent observedAttributes is not an error, it is no observed attributes. A present one is
               a sequence, whose length is itself a read — of an engine-visible array in every real case, and of
               the page's object when it is not, which is why the whole walk is on the trampoline. */
            if (JS_IsObject(s->raw)) {
                JSValue lv = JS_GetPropertyStr(ctx, s->raw, "length");
                JS_ToUint32(ctx, &s->n, lv);
                JS_FreeValue(ctx, lv);
            }
        }
    }
    if (hdr->stage == CE_SEQUENCE) {
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
        hdr->stage = CE_COMMIT;
    }
    DCHECK(hdr->stage == CE_COMMIT, "customElements.define resumed into a stage §4.13.4 does not have");
    JS_FreeValue(ctx, cb_result);
    *presult = ce_define_commit(ctx, argv, s->names, s->proto, s->callbacks);
    if (JS_IsException(*presult)) { *presult = JS_UNDEFINED; return -1; }
    return 0;
}

static const IdlStepDecl CE_DEFINE_STEP = {
    js_ce_define, sizeof(CeDefineState), ce_define_visit, ce_define_release,
    "HTML §4.13.4 CustomElementRegistry.define", CE_DEFINE_STEPS
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
    g_defs_slot = realm_value_declare(ctx, "§4.13.4 definition set");
    g_deflist_slot = realm_value_declare(ctx, "§4.13.4 definition set, in definition order");
    g_whendef_slot = realm_value_declare(ctx, "§4.13.4 when-defined promise map");
    g_html_ctor_slot = realm_value_declare(ctx, "§4.13.2's active function object (HTMLElement)");
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
    g_backup_fn = JS_NewCFunction2(ctx, NULL, "backupElementQueue", 0, JS_CFUNC_step, g_backup_stepid);
    CHECK(!JS_IsException(g_backup_fn), "the backup element queue's driver could not be allocated");
    /* §4.13.4's two members, DECLARED once per agent: `customElements` is a per-realm object, so installing
       from a fresh declaration would mint the pair again for a second realm's registry. */
    g_id_define = idl_method_id_step(ctx, CE_DEFINE_ARGS, 3, CE_DEFINE_OPTS,
                                     (int)(sizeof(CE_DEFINE_OPTS) / sizeof(CE_DEFINE_OPTS[0])),
                                     &CE_DEFINE_STEP, 0);
    idl_optional_from(2);   /* §4.13.4: `define(name, constructor, optional ElementDefinitionOptions options)` */
    {
        static const IdlArgType ONE_STR[1] = { IDL_DOMSTRING };
        g_id_get = idl_method_id(ctx, ONE_STR, 1, js_ce_get, 0);
        g_id_when_defined = idl_method_id(ctx, ONE_STR, 1, js_ce_when_defined, 0);
    }
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
    /* Built with the REGISTRY it belongs to, and for the agent's own realm that is still pre-boot — so a write
       to it during a flow is captured by the heap COW rather than being that flow's private object. */
    {
        JSValue defs = JS_NewObjectProto(ctx, JS_NULL);
        JSValue order = JS_NewArray(ctx);
        JSValue pending = JS_NewObjectProto(ctx, JS_NULL);
        CHECK(!JS_IsException(defs), "the custom-element definition set could not be allocated");
        CHECK(!JS_IsException(order), "the custom-element definition ORDER could not be allocated");
        CHECK(!JS_IsException(pending), "the §4.13.4 when-defined promise map could not be allocated");
        realm_value_set(ctx, g_defs_slot, defs);
        realm_value_set(ctx, g_deflist_slot, order);
        realm_value_set(ctx, g_whendef_slot, pending);
    }
    reg = JS_NewObject(ctx);
    CHECK(!JS_IsException(reg), "the CustomElementRegistry allocation failed");
    idl_install_method(ctx, reg, "define", 2, g_id_define);
    idl_install_method(ctx, reg, "get", 1, g_id_get);
    idl_install_method(ctx, reg, "whenDefined", 1, g_id_when_defined);
    JS_SetPropertyStr(ctx, (JSValue)global, "customElements", reg);
}

void custom_elements_free(JSContext *ctx)
{
    int k;

    if (!g_ready) return;
    /* the definition sets are the REALMS' — released with their contexts */
    JS_FreeValue(ctx, g_backup_fn);
    JS_FreeValue(ctx, g_ce_backup);
    JS_FreeValue(ctx, g_rq_key);
    g_backup_fn = g_ce_backup = g_rq_key = JS_UNDEFINED;
    g_current = NULL;
    JS_FreeAtom(ctx, g_atom_rq);
    JS_FreeAtom(ctx, g_atom_rq_head);
    JS_FreeAtom(ctx, g_atom_backup_flag);
    g_atom_rq = g_atom_rq_head = g_atom_backup_flag = JS_ATOM_NULL;
    g_backup_stepid = -1;
    JS_FreeAtom(ctx, g_atom_prototype);
    JS_FreeAtom(ctx, g_atom_ctor);
    JS_FreeAtom(ctx, g_atom_proto);
    JS_FreeAtom(ctx, g_atom_observed);
    JS_FreeAtom(ctx, g_atom_observed_src);
    JS_FreeAtom(ctx, g_atom_callbacks);
    JS_FreeAtom(ctx, g_atom_name);
    JS_FreeAtom(ctx, g_atom_local);
    JS_FreeAtom(ctx, g_atom_stack);
    g_atom_name = g_atom_local = g_atom_stack = JS_ATOM_NULL;
    JS_FreeAtom(ctx, g_atom_def);
    JS_FreeValue(ctx, g_def_key);
    g_def_key = JS_UNDEFINED;
    JS_FreeAtom(ctx, g_atom_state);
    g_atom_state = JS_ATOM_NULL;
    JS_FreeValue(ctx, g_state_key);
    g_state_key = JS_UNDEFINED;
    for (k = 0; k < CE_CB_COUNT; k++) {
        JS_FreeAtom(ctx, g_cb_atoms[k]);
        g_cb_atoms[k] = JS_ATOM_NULL;
    }
    g_atom_prototype = g_atom_def = JS_ATOM_NULL;
    g_atom_ctor = g_atom_proto = g_atom_observed = g_atom_observed_src = JS_ATOM_NULL;
    g_atom_callbacks = JS_ATOM_NULL;
    g_ready = 0;
}
