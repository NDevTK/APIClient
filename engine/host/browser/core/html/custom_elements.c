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
 * WHAT IS HONESTLY ABSENT: adoptedCallback (no adoption reaction yet) and customized built-ins (`extends`),
 * which §4.13.4 rejects here rather than registering as autonomous — a silently-wrong registration is worse
 * than a named refusal. */
#include <string.h>
#include <stdlib.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/dom/node.h"
#include "core/dom/element.h"
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
/* §4.13.4: A REGISTRY IS PER WINDOW, AND SO IS ITS DEFINITION SET. One set for the agent meant
   `frame.contentWindow.customElements.define('x-a', C)` defined `x-a` in the PARENT too, and the parent's
   `customElements.get('x-a')` then handed back the child realm's constructor — a cross-realm constructor
   handed out by a member that never crossed a boundary. */
static int g_defs_slot = -1;

/* THIS REALM'S definition set. OWNED: the caller frees. */
static JSValue ce_defs(JSContext *ctx)
{
    DCHECK(g_defs_slot > 0, "a custom-element definition was reached before custom_elements_init declared the set");
    return realm_value_get(ctx, g_defs_slot);
}
static int    g_ready;
static JSAtom g_atom_prototype = JS_ATOM_NULL;
static JSAtom g_atom_ctor = JS_ATOM_NULL;
static JSAtom g_atom_proto = JS_ATOM_NULL;
static JSAtom g_atom_observed = JS_ATOM_NULL;      /* the definition's own key for the list */
static JSAtom g_atom_observed_src = JS_ATOM_NULL;  /* the class's `observedAttributes` */
static JSAtom g_atom_callbacks = JS_ATOM_NULL;     /* the definition's own key for step 14.4's map */

/* §4.13.5 step 2's "set element's custom element definition to definition" — the element's OWN slot, under a
   symbol the page cannot mint, so no string key of this engine's invention appears on a custom element. It
   replaces an `apiclientUpgraded` boolean: the boolean answered "has this been upgraded" and nothing else, and
   the reaction then had to find the definition again BY NAME through the registry. §4.13.6's enqueue reads the
   definition off the ELEMENT, which is what makes the definition an element's own state and not a lookup. */
static JSValue g_def_key = JS_UNDEFINED;
static JSAtom  g_atom_def = JS_ATOM_NULL;

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
static int    g_id_define, g_id_get;   /* declared once per agent — see custom_elements_init */

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

    if (JS_GetOwnSlot(ctx, &q, wrap, g_atom_rq) > 0 && JS_IsObject(q)) return q;
    if (JS_GetOwnSlot(ctx, &q, wrap, g_atom_rq) > 0) JS_FreeValue(ctx, q);
    if (!create) return JS_UNDEFINED;
    q = JS_NewArray(ctx);
    CHECK(!JS_IsException(q), "an element's custom element reaction queue could not be allocated");
    JS_SetPropertyUint32(ctx, q, 0, JS_UNDEFINED);        /* materialise the array before the head cursor */
    ce_array_set_len(ctx, q, 0);
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
    for (k = 0; k < 2 + CE_MAX_REACTION_ARGS; k++) q->cb[k] = JS_UNDEFINED;
}

void custom_elements_queue_visit(JSContext *ctx, CustomElementQueue *q, JSStepVisit *v)
{
    int k;
    v->val(ctx, &q->queue);
    for (k = 0; k < 2 + CE_MAX_REACTION_ARGS; k++) v->val(ctx, &q->cb[k]);
}

void custom_elements_queue_release(JSContext *ctx, CustomElementQueue *q)
{
    int k;
    JS_FreeValue(ctx, q->queue);
    q->queue = JS_UNDEFINED;
    for (k = 0; k < 2 + CE_MAX_REACTION_ARGS; k++) {
        JS_FreeValue(ctx, q->cb[k]);
        q->cb[k] = JS_UNDEFINED;
    }
}

/* §4.13.6 "invoke custom element reactions in an element queue", one reaction per entry.
   THE POP HAPPENS FIRST AND IT IS OBSERVABLE: step 3 removes the queue from the stack BEFORE step 4 invokes it,
   so a reaction that itself mutates the DOM enqueues onto whatever is on the stack THEN — an outer
   `[CEReactions]` member's queue, or the backup queue — and never back onto the one being drained.
   Returns JS_STEP_CALL parked on one reaction (the caller returns it), or 0 when the queue is exhausted. */
int custom_elements_reactions_invoke(JSContext *ctx, CustomElementQueue *q, JSValue cb_result,
                                     JSValue **out_cb, int *out_argc)
{
    /* STEP 3 already happened: the queue stopped being current the moment the member's own steps returned,
       which is what makes a reaction that mutates the DOM enqueue onto the NEXT member's queue (or the backup
       one) and never back onto the one being drained. An empty queue is the overwhelmingly common case — a
       member that touched no custom element never allocated one. */
    if (!g_ready || JS_IsUndefined(q->queue)) { JS_FreeValue(ctx, cb_result); return 0; }
    for (;;) {
        uint32_t n = ce_array_len(ctx, q->queue);
        JSValue el, rq, head_v, reaction, fn;
        uint32_t head, rn;
        int nargs, k, r;
        JSValue args[CE_MAX_REACTION_ARGS], ignored;

        if (q->i >= n) {                                  /* step 1: the queue is empty */
            JS_FreeValue(ctx, cb_result);
            JS_FreeValue(ctx, q->queue);
            q->queue = JS_UNDEFINED;
            return 0;
        }
        el = JS_GetPropertyUint32(ctx, q->queue, q->i);   /* step 1.1: dequeue element */
        rq = ce_reaction_queue(ctx, el, 0);               /* step 1.2: its reaction queue */
        head_v = JS_IsObject(rq) ? JS_GetProperty(ctx, rq, g_atom_rq_head) : JS_UNDEFINED;
        head = 0;
        JS_ToUint32(ctx, &head, head_v);
        JS_FreeValue(ctx, head_v);
        rn = JS_IsObject(rq) ? ce_array_len(ctx, rq) : 0;
        if (head >= rn) {                                 /* step 1.3: this element's reactions are exhausted */
            if (JS_IsObject(rq)) {                        /* the removal §4.13.6 performs, as one truncation */
                ce_array_set_len(ctx, rq, 0);
                JS_SetProperty(ctx, rq, g_atom_rq_head, JS_NewUint32(ctx, 0));
            }
            JS_FreeValue(ctx, rq);
            JS_FreeValue(ctx, el);
            q->i++;
            continue;
        }
        /* PEEKED, NOT REMOVED, UNTIL THE CALL COMPLETES. The invoke parks on the page's code and this function
           is re-entered from the top, so advancing the head before the call would resume on the NEXT reaction
           and drop the one whose answer just arrived. */
        reaction = JS_GetPropertyUint32(ctx, rq, head);
        DCHECK(JS_IsObject(reaction), "an element's reaction queue holds something that is not a reaction");
        fn = JS_GetPropertyUint32(ctx, reaction, 0);
        nargs = (int)ce_array_len(ctx, reaction) - 1;
        DCHECK(nargs >= 0 && nargs <= CE_MAX_REACTION_ARGS,
               "a lifecycle callback reaction carries more arguments than any of them takes");
        for (k = 0; k < nargs; k++) args[k] = JS_GetPropertyUint32(ctx, reaction, (uint32_t)(k + 1));
        /* step 1.3.1: invoke the callback function with its arguments and "report", this = element. */
        r = step_call_run(ctx, &q->phase, q->cb, 2 + CE_MAX_REACTION_ARGS, fn, el, nargs,
                          (JSValueConst *)args, cb_result, &ignored, out_cb, out_argc);
        for (k = 0; k < nargs; k++) JS_FreeValue(ctx, args[k]);
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, reaction);
        if (r > 0) {                                      /* parked on the page's code */
            JS_FreeValue(ctx, rq);
            JS_FreeValue(ctx, el);
            return JS_STEP_CALL;
        }
        JS_FreeValue(ctx, ignored);                       /* §4.13.3: a reaction's return value is discarded */
        cb_result = JS_UNDEFINED;
        JS_SetProperty(ctx, rq, g_atom_rq_head, JS_NewUint32(ctx, head + 1));   /* NOW it is removed */
        JS_FreeValue(ctx, rq);
        JS_FreeValue(ctx, el);
    }
}

/* THE BACKUP QUEUE'S MICROTASK — §4.13.6 step 2.4, and the ONE place a reaction runs when no `[CEReactions]`
   member is on the stack (the parser's own mutations, an engine-driven insertion). It is the same invoke over
   a queue that was never on the stack, so it takes the same machine with the queue handed to it directly. */
#define CE_BACKUP_STAGES(X) \
    X(CEBACKUP_INVOKE, "HTML §4.13.6 enqueue an element on the appropriate element queue step 2.4 (invoke " \
                       "custom element reactions in the backup element queue), one reaction per step")
enum { CE_BACKUP_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const CE_BACKUP_STEPS[] = { CE_BACKUP_STAGES(JS_STEP_STAGE_LABEL) NULL };

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

    DCHECK(s->hdr.stage == CEBACKUP_INVOKE,
           "the backup element queue's drain resumed into a stage §4.13.6 does not have");
    if (JS_IsUndefined(s->q.queue) && s->q.phase == 0) {
        /* THE QUEUE IS TAKEN WHOLE, and the flag is unset with it (step 2.4's second half): a reaction that
           runs during the drain and enqueues onto an empty stack must schedule a NEW microtask, not append to
           the batch already in flight. */
        s->q.queue = JS_DupValue(ctx, g_ce_backup);
        s->q.i = 0;
        g_ce_backup = JS_NewArray(ctx);
        CHECK(!JS_IsException(g_ce_backup), "the backup element queue could not be replaced");
        JS_SetProperty(ctx, g_ce_backup, g_atom_backup_flag, JS_FALSE);
    }
    r = custom_elements_reactions_invoke(ctx, &s->q, cb_result, out_cb, out_argc);
    return r ? r : JS_STEP_DONE;
}

static const JSTrampStepDef js_ce_backup_def = {
    sizeof(JSCeBackup), js_ce_backup_step, js_ce_backup_fini, 0, .visit = js_ce_backup_visit,
    .algorithm = "HTML §4.13.6 invoke custom element reactions in the backup element queue",
    .steps = CE_BACKUP_STEPS
};

/* ---- the upgrade ------------------------------------------------------------------------------------------ */
/* §4.13.3 "upgrade": give the element the definition's prototype, then enqueue its connected reaction. The
   wrapper is the SAME object it always was, so every identity a page holds survives the upgrade. */
/* §4.13.5 step 2's definition, read off the element's OWN slot — no prototype lookup and no page code. UNDEFINED
   for an element that has not been upgraded, which is what "custom element definition is null" means. OWNED. */
static JSValue ce_definition_of(JSContext *ctx, JSValueConst wrap)
{
    JSValue v;

    if (!JS_IsObject(wrap)) return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &v, wrap, g_atom_def) <= 0) return JS_UNDEFINED;
    return v;
}

/* §4.13.6 "enqueue a custom element callback reaction", steps 1-3 and 5-6. The callback is the one step 14.4
   COLLECTED into this element's definition, and step 3 returns without a reaction when it is null — which is
   why a class that declares no `disconnectedCallback` costs nothing at every removal.
   STEP 5 ADDS IT TO THE ELEMENT'S OWN REACTION QUEUE and step 6 puts the ELEMENT on an element queue. Those are
   two lists and not one, and the difference is observable: §4.13.6's invoke dequeues an element and then drains
   ALL of that element's reactions, so `el.setAttribute(a,1); other.setAttribute(b,2); el.setAttribute(a,3)`
   inside one `[CEReactions]` boundary runs el's two callbacks back to back. A single flat list of reactions
   would interleave them, which is a different program order for the page. */
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
    reaction = JS_NewArray(ctx);
    CHECK(!JS_IsException(reaction), "a custom element callback reaction could not be allocated");
    JS_SetPropertyUint32(ctx, reaction, 0, fn);
    for (i = 0; i < argc; i++)
        JS_SetPropertyUint32(ctx, reaction, (uint32_t)(i + 1), JS_DupValue(ctx, args[i]));
    rq = ce_reaction_queue(ctx, wrap, 1);   /* step 5 */
    ce_array_push(ctx, rq, reaction);
    JS_FreeValue(ctx, rq);
    ce_enqueue_element(ctx, wrap);          /* step 6 */
}

static void ce_enqueue(JSContext *ctx, JSValueConst wrap, JSValueConst def, int callback)
{
    ce_enqueue_args(ctx, wrap, def, callback, 0, NULL);
}

static void ce_upgrade(JSContext *ctx, lxb_dom_element_t *el, JSValueConst def)
{
    JSValue wrap = node_wrap(ctx, lxb_dom_interface_node(el));
    JSValue proto;

    if (!JS_IsObject(wrap)) { JS_FreeValue(ctx, wrap); return; }
    proto = JS_GetProperty(ctx, def, g_atom_proto);
    JS_SetPrototype(ctx, wrap, proto);
    JS_FreeValue(ctx, proto);
    /* §4.13.5 step 2: the element's custom element definition. It rides the WRAPPER, so it is per-flow like
       everything else about that element — and its presence is also what tells a re-insertion (which must fire
       connectedCallback again) from a second upgrade (which must not). */
    JS_DefinePropertyValue(ctx, (JSValue)wrap, g_atom_def, JS_DupValue(ctx, def), 0);
    ce_enqueue(ctx, wrap, def, CE_CB_CONNECTED);
    JS_FreeValue(ctx, wrap);
}

void custom_elements_disconnected(JSContext *ctx, lxb_dom_element_t *el)
{
    JSValue wrap, def;

    if (!g_ready) return;
    wrap = node_wrap(ctx, lxb_dom_interface_node(el));
    /* §4.13.3: only an UPGRADED element has a disconnected reaction, and the definition it was upgraded WITH is
       the one that supplies the callback. Asking the registry by name instead would fire for an element that
       was never upgraded — one created before its definition and removed before it. */
    def = ce_definition_of(ctx, wrap);
    ce_enqueue(ctx, wrap, def, CE_CB_DISCONNECTED);
    JS_FreeValue(ctx, def);
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
        JSValue had = ce_definition_of(ctx, wrap);
        /* §4.13.3: an element already upgraded is not upgraded again — but it DOES get a connected reaction
           every time it re-enters a document, which is how a page that moves a node around keeps its
           lifecycle running. Two different things behind one insertion. The reaction uses the definition the
           element WAS upgraded with, which is the element's own state and not this name's current entry. */
        if (JS_IsObject(had)) ce_enqueue(ctx, wrap, had, CE_CB_CONNECTED);
        else ce_upgrade(ctx, el, def);
        JS_FreeValue(ctx, had);
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
    size_t old_len = 0;
    const lxb_char_t *old;
    uint32_t n = 0, i;
    bool watched = false;

    if (!g_ready) return;
    wrap = node_wrap(ctx, lxb_dom_interface_node(el));
    def = ce_definition_of(ctx, wrap);
    if (!JS_IsObject(def)) { JS_FreeValue(ctx, def); JS_FreeValue(ctx, wrap); return; }
    observed = JS_GetProperty(ctx, def, g_atom_observed);
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
    /* §4.13.6's enqueue step 4: a name the definition does not observe is not a reaction. */
    if (!watched) { JS_FreeValue(ctx, def); JS_FreeValue(ctx, wrap); return; }
    /* §4.13.3's arguments: (name, oldValue, newValue). An attribute that was absent has a NULL old value and an
       attribute being removed a NULL new one, and the page's code branches on exactly that. */
    old = lxb_dom_element_get_attribute(el, (const lxb_char_t *)name, strlen(name), &old_len);
    args[0] = JS_NewString(ctx, name);
    args[1] = old ? JS_NewStringLen(ctx, (const char *)old, old_len) : JS_NULL;
    args[2] = val ? JS_NewStringLen(ctx, val, val_len) : JS_NULL;
    ce_enqueue_args(ctx, wrap, def, CE_CB_ATTR_CHANGED, 3, (JSValueConst *)args);
    for (i = 0; i < 3; i++) JS_FreeValue(ctx, args[i]);
    JS_FreeValue(ctx, def);
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
            JS_SetProperty(ctx, defs, a, JS_DupValue(ctx, def));
            JS_FreeValue(ctx, defs);
        }
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
    CHECK(g_atom_prototype != JS_ATOM_NULL &&
          g_atom_ctor != JS_ATOM_NULL && g_atom_proto != JS_ATOM_NULL &&
          g_atom_observed != JS_ATOM_NULL && g_atom_observed_src != JS_ATOM_NULL &&
          g_atom_callbacks != JS_ATOM_NULL,
          "a custom-element atom could not be interned");
    /* §4.13.5 step 2's slot key: a symbol the page cannot mint, so the element's definition is not a string
       property of this engine's invention sitting on every custom element. */
    g_def_key = JS_NewSymbol(ctx, "customElementDefinition", false);
    CHECK(!JS_IsException(g_def_key), "the custom-element definition slot key allocation failed");
    g_atom_def = JS_ValueToAtom(ctx, g_def_key);
    CHECK(g_atom_def != JS_ATOM_NULL, "the custom-element definition slot key could not be interned");
    for (k = 0; k < CE_CB_COUNT; k++) {
        g_cb_atoms[k] = JS_NewAtom(ctx, CE_CALLBACK_NAMES[k]);
        CHECK(g_cb_atoms[k] != JS_ATOM_NULL, "a §4.13.4 step 14 lifecycle callback name could not be interned");
    }
    g_defs_slot = realm_value_declare(ctx, "§4.13.4 definition set");
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
    }
    g_ready = 1;
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
        CHECK(!JS_IsException(defs), "the custom-element definition set could not be allocated");
        realm_value_set(ctx, g_defs_slot, defs);
    }
    reg = JS_NewObject(ctx);
    CHECK(!JS_IsException(reg), "the CustomElementRegistry allocation failed");
    idl_install_method(ctx, reg, "define", 2, g_id_define);
    idl_install_method(ctx, reg, "get", 1, g_id_get);
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
    JS_FreeAtom(ctx, g_atom_def);
    JS_FreeValue(ctx, g_def_key);
    g_def_key = JS_UNDEFINED;
    for (k = 0; k < CE_CB_COUNT; k++) {
        JS_FreeAtom(ctx, g_cb_atoms[k]);
        g_cb_atoms[k] = JS_ATOM_NULL;
    }
    g_atom_prototype = g_atom_def = JS_ATOM_NULL;
    g_atom_ctor = g_atom_proto = g_atom_observed = g_atom_observed_src = JS_ATOM_NULL;
    g_atom_callbacks = JS_ATOM_NULL;
    g_ready = 0;
}
