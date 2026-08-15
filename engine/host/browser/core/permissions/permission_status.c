/* PERMISSIONS §6.3's PermissionStatus. See permission_status.h for why §6.3.4's trigger is a fork. */
#include <stdio.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"
#include "core/dom/document.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/permissions/permission_status.h"
#include "core/permissions/permission_store.h"
#include "solver/concolic.h"

/* §6.3's INTERNAL SLOTS, as an own property under a private Symbol — the shape core/dom/abort.c uses and for
   its reason: a write to them is an ordinary property write, so the per-flow COW delta captures it with no new
   delta kind, and a status whose state changed in one arm of a fork is unchanged in its sibling.
     `feature` and `aspect` ARE the [[query]] internal slot: §6.3.1 initializes it to the typed descriptor, and
   a descriptor is exactly the feature and its one aspect (permission_store.h). Kept as the pair rather than as
   the page's object, because §6.3.4 re-runs the query algorithm over it and a stored page object would let the
   page rewrite the descriptor a live status answers for.
     `name` is not stored: §6.3.2 returns "the value it was initialized to", which §6.3.1 initializes to
   permissionDesc's name — the feature's registered name, which the registry already holds. Two copies of one
   string is one of them able to disagree.
     `generation` is how many times §6.3.4's update steps have run on this status. It is not the standard's; it
   is what keys each successive awareness question as its OWN predicate, so a flow that has decided the state
   changed once is asked afresh whether it changed again rather than being handed its previous answer. */
#define PS_FEATURE    "feature"
#define PS_ASPECT     "aspect"
#define PS_STATE      "state"
#define PS_GENERATION "generation"
/* §6.3's [[query]] SLOT IS THE WHOLE TYPED DESCRIPTOR, which for the one registered descriptor type that names
   a SUBJECT member includes that member's value. Held here rather than re-read, because §6.3.4's update steps
   run long after §6.2.1's conversion returned and the object the page passed is gone; and held as a slot rather
   than as a C pointer, because a status outlives the algorithm that made it and its state has to park. */
#define PS_SUBJECT    "subject"

static JSValue g_key;
static int     g_ready;
static JSClassID g_status_class;
static int     g_change_stepid = -1;
static JSRuntime *g_rt;

/* THIS STATUS'S SLOT RECORD. Read as an OWN SLOT, never a lookup: a miss on a lookup is the solver's
   absent-state seam and would mint a concolic for an internal slot, which is right for the page's own reads
   and wrong here. JS_UNDEFINED when `v` is not one of this component's objects. */
static JSValue status_slots(JSContext *ctx, JSValueConst v)
{
    JSAtom k;
    JSValue st;

    DCHECK(g_ready, "a PermissionStatus slot was asked for before the key existed");
    if (!JS_IsObject(v))
        return JS_UNDEFINED;
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL)
        return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &st, v, k) <= 0)
        st = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    return st;
}

/* §6.3's [[query]], back as the pair it is. The record is the engine's own, so both reads run none of the
   page's code. */
static void status_descriptor(JSContext *ctx, JSValueConst slots, PermissionDescriptor *d)
{
    JSValue f = JS_GetPropertyStr(ctx, slots, PS_FEATURE);
    JSValue a = JS_GetPropertyStr(ctx, slots, PS_ASPECT);
    JSValue sub = JS_GetPropertyStr(ctx, slots, PS_SUBJECT);
    int32_t feature = -1;

    JS_ToInt32(ctx, &feature, f);
    d->feature = feature;
    d->aspect = JS_ToBool(ctx, a);
    /* BORROWED, and the slot record is what keeps it alive — the record is reachable from the status for as
       long as the status is, and a descriptor is a stack value that never outlives the algorithm reading it. */
    d->subject = sub;
    JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, sub);
    DCHECK(d->feature >= 0, "a PermissionStatus's [[query]] internal slot names no powerful feature — §6.3.1 "
                            "initializes it to a typed descriptor, and every descriptor comes from §4's "
                            "registry");
}

/* WEB IDL §3.7.5's BRAND CHECK. `PermissionStatus.prototype.state` read off a plain object is a TypeError, and
   a page tells that apart from `undefined` — a feature detector that probes the descriptor and applies the
   getter reads the throw as "this is a real interface". A real throw, not an assert, for that reason. */
static bool status_brand(JSContext *ctx, JSValueConst this_val)
{
    DCHECK(g_status_class != 0, "a PermissionStatus member ran before permission_status_init declared the "
                                "class — the member is only reachable through a prototype the per-realm "
                                "install builds, so there is no route here that has not run the declaration");
    if (JS_GetClassID(this_val) == g_status_class)
        return true;
    JS_ThrowTypeError(ctx, "a PermissionStatus member was reached on something that is not a PermissionStatus");
    return false;
}

/* §6.3.3: "The state attribute returns the latest value that was set on the current instance." A slot read,
 * and therefore a plain C getter rather than a machine — it reaches none of the page's code and takes no
 * decision.
 *   IT HANDS THE CONCOLIC STRAIGHT OVER, which is the whole design. The value may be the feature's source, and
 * the page's own `status.state === "granted"` is then the branch that forks — one predicate, in the page's own
 * terms, keyed by the source the engine's C-side reads are keyed by. A getter that decided the arm ITSELF
 * would answer one string and delete the other two worlds before the page ever compared it. */
static JSValue js_status_get_state(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue slots, v;

    (void)magic;
    if (!status_brand(ctx, this_val)) return JS_EXCEPTION;
    slots = status_slots(ctx, this_val);
    DCHECK(JS_IsObject(slots), "a branded PermissionStatus carries no slot record — the brand is the class the "
                               "constructor puts on it in the same breath as the record");
    v = JS_GetPropertyStr(ctx, slots, PS_STATE);
    JS_FreeValue(ctx, slots);
    return v;
}

/* §6.3.2: "The name attribute returns the value it was initialized to" — §6.3.1 initialized it to
   permissionDesc's name, which is §4's registered name for the feature. */
static JSValue js_status_get_name(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue slots;
    PermissionDescriptor d;

    (void)magic;
    if (!status_brand(ctx, this_val)) return JS_EXCEPTION;
    slots = status_slots(ctx, this_val);
    DCHECK(JS_IsObject(slots), "a branded PermissionStatus carries no slot record");
    status_descriptor(ctx, slots, &d);
    JS_FreeValue(ctx, slots);
    return JS_NewString(ctx, permission_feature_name(d.feature));
}

/* ---- §6.3.4's UPDATE STEPS, AND THE AWARENESS THAT TRIGGERS THEM -------------------------------------------
 *
 * "Whenever the user agent is aware that the state of a PermissionStatus instance status has changed, it
 *  asynchronously runs the PermissionStatus update steps:
 *    1. If this's relevant global object is a Window object, then:
 *       1. Let document be status's relevant global object's associated Document.
 *       2. If document is null or document is not fully active, terminate this algorithm.
 *    2. Let query be status's [[query]] internal slot.
 *    3. Run query's name's permission query algorithm, passing query and status.
 *    4. Queue a task on the permissions task source to fire an event named change at status."
 *
 * THE CHAIN IS THREE QUESTIONS AND THEN THE STEPS. The awareness itself is the first; the state the user
 * decided on is the second and third (two chained binary questions, because the outcome seam prepares ONE
 * sibling per ask and three completions in one fork would lose an arm); and only then do steps 2-4 run, over a
 * state that is by then a FACT — written into §3.2's store, so step 3's query algorithm reads it back rather
 * than reaching the unknown again, and so does every other status over the same feature.
 *   EACH GENERATION IS ITS OWN PREDICATE. The operation string carries the generation, which is what decide.c
 * keys a fork by along with the source — so "did it change" asked after one change is a different question
 * from the one that produced that change, and the flow is not handed its previous answer. That is the CHAIN
 * shape decide.h describes, and it is unbounded by design: the arm that keeps changing keeps forking, emits
 * less each time, and is outranked and paged by the one WFQ. */
#define PSC_STAGES(X)                                                                                          \
    X(PSC_AWARE, "Permissions §6.3.4 (has the user agent become aware that this PermissionStatus's state has "  \
                 "changed)")                                                                                   \
    X(PSC_DEFAULT, "Permissions §6.3.4 (the state it changed to — is it this feature's default permission "     \
                   "state)")                                                                                   \
    X(PSC_OTHER, "Permissions §6.3.4 (the state it changed to — which of the two non-default states)")          \
    X(PSC_UPDATE, "Permissions §6.3.4 steps 2-4 (run the permission query algorithm over [[query]], then queue " \
                  "a task on the permissions task source to fire an event named change at status)")
enum { PSC_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const PSC_STEPS[] = { PSC_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr hdr;
    JSValue   over;      /* the feature's source, OWNED across the chain — the fork's operand */
    int       state;     /* the state the awareness resolved to, once the second and third questions answered */
    uint8_t   started;   /* has `over` been taken — a zeroed JSValue is the integer 0, not undefined */
    /* THE FORK'S OPERATION STRING LIVES ON THE STATE, not in a C local. step_fork_run stores the POINTER on the
       header and the driver reads it AFTER this machine has returned, so a stack buffer would dangle exactly
       where the constraint key is built. */
    char      op[128];
} PermChangeState;

static void psc_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    PermChangeState *s = st;

    if (s->started) v->val(ctx, &s->over);
}

/* SEED THE NEXT ASK. §6.3.4 has no "next" — the user agent simply becomes aware again — so what this schedules
   is the QUESTION and not an event: one job per generation, each of which forks or terminates. The status
   travels as closure data because the update steps take no arguments. */
static void psc_queue(JSContext *ctx, JSValueConst status)
{
    JSValue steps;

    DCHECK(g_change_stepid >= 0, "§6.3.4's chain was queued before permission_status_init registered it");
    steps = JS_NewStepClosure(ctx, g_change_stepid, 0, 1, &status);
    CHECK(!JS_IsException(steps),
          "§6.3.4's awareness chain could not be allocated — a PermissionStatus whose chain was never queued "
          "reads as one whose permission the user never touched, which is the constant this component exists "
          "to delete");
    JS_EnqueueCallJob(ctx, steps, 0, NULL);
    JS_FreeValue(ctx, steps);
}

/* THIS STATUS'S GENERATION — how many times §6.3.4's update steps have run on it. An engine-built record read
   with an ordinary property read, so none of the page's code is reachable from here. */
static int psc_generation(JSContext *ctx, JSValueConst slots)
{
    JSValue g = JS_GetPropertyStr(ctx, slots, PS_GENERATION);
    int32_t v = -1;

    JS_ToInt32(ctx, &v, g);
    JS_FreeValue(ctx, g);
    DCHECK(v >= 0, "a PermissionStatus carries no §6.3.4 generation — it is written by §6.3.1's create and by "
                   "nothing else, so an absent one means the status was built somewhere other than there");
    return v;
}

static int psc_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    PermChangeState *s = st;
    JSValueConst status = JS_StepClosureData(&s->hdr, 0);
    PermissionDescriptor d;
    JSValue slots;
    int arm = 0, rc = JS_STEP_DONE, order[PERMISSION_STATE_N], n = 0, i, def, gen;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    slots = status_slots(ctx, status);
    DCHECK(JS_IsObject(slots), "§6.3.4's chain resumed on something that is not a PermissionStatus — the status "
                               "is the closure's own data and nothing else can be put there");
    status_descriptor(ctx, slots, &d);
    gen = psc_generation(ctx, slots);
    /* §4's DEFAULT PERMISSION STATE first, then the other two in the enum's order — the same rotation §5.1's
       own chain uses, so the two questions mean the same thing in both places. */
    def = permission_feature_default(d.feature);
    order[n++] = def;
    for (i = 0; i < PERMISSION_STATE_N; i++)
        if (i != def)
            order[n++] = i;
    DCHECK(n == PERMISSION_STATE_N, "§6.3.4's rotation lost a permission state — each of §3.1's three is a "
                                    "state the user agent may become aware of");
    if (!s->started) {
        s->over = permission_unknown(ctx, d.feature);
        s->started = 1;
    }

    if (s->hdr.stage == PSC_AWARE) {
        /* A HOST WITH NOTHING UNKNOWN HAS NOTHING TO BECOME AWARE OF. `concolic_is` is false in a conformance
           run (no source overlay), so the chain ends here and no `change` is ever fired — which is the only
           correct answer for a user agent nothing has simulated a user on.
           STEP 1: a document that is null or not fully active terminates the algorithm. */
        if (!concolic_is(s->over) || !document_fully_active(ctx))
            goto done;
        snprintf(s->op, sizeof s->op,
                 "Permissions §6.3.4 (the user agent became aware of a change, %d)", gen);
        rc = step_fork_run(ctx, &s->hdr, s->over, s->op, 2, &arm);
        if (rc) goto out;
        if (arm == 0)                      /* nothing has changed in this world */
            goto done;
        s->hdr.stage = PSC_DEFAULT;
    }
    if (s->hdr.stage == PSC_DEFAULT) {
        snprintf(s->op, sizeof s->op,
                 "Permissions §6.3.4 (the state it changed to is this feature's default one, %d)", gen);
        rc = step_fork_run(ctx, &s->hdr, s->over, s->op, 2, &arm);
        if (rc) goto out;
        /* A CHANGE **TO** THE DEFAULT IS A REAL WORLD, not a contradiction: §4's permission lifetime expiry
           and §5.4's revocation both set a permission back to its default state, and the page hears about it
           through this same event. */
        s->state = order[0];
        s->hdr.stage = (arm == 0) ? PSC_UPDATE : PSC_OTHER;
    }
    if (s->hdr.stage == PSC_OTHER) {
        snprintf(s->op, sizeof s->op,
                 "Permissions §6.3.4 (which of the two non-default states it changed to, %d)", gen);
        rc = step_fork_run(ctx, &s->hdr, s->over, s->op, 2, &arm);
        if (rc) goto out;
        s->state = order[1 + arm];
        s->hdr.stage = PSC_UPDATE;
    }
    DCHECK(s->hdr.stage == PSC_UPDATE, "§6.3.4's chain resumed into a stage it does not have");
    /* THE AWARENESS IS NOW KNOWLEDGE, so it goes into §3.2's store — which is §5.2 step 7's own action and
       what makes step 3 below read a fact rather than ask the unknown again. Every other read of this feature
       in this flow answers the same value from here on, including another PermissionStatus's and a powerful
       feature's own C-side ask. */
    permission_store_set(ctx, &d, s->state);
    /* STEPS 2-3: the default permission query algorithm is "set status's state to permissionDesc's permission
       state", performed over the [[query]] slot — which now resolves through the store entry just written. */
    JS_SetPropertyStr(ctx, slots, PS_STATE, permission_state(ctx, &d));
    JS_SetPropertyStr(ctx, slots, PS_GENERATION, JS_NewInt32(ctx, gen + 1));
    JS_FreeValue(ctx, slots);
    /* STEP 4: "queue a task on the permissions task source to fire an event named change at status" —
       event_target_fire is that queued §2.9 dispatch, run as a job so every listener body is a flow. */
    event_target_fire(ctx, status, event_new(ctx, "change", false, false), JS_UNDEFINED);
    /* AND ASK AGAIN. A permission the user changed once is a permission the user may change again, and the
       next generation is its own predicate — see the chain's comment above. */
    psc_queue(ctx, status);
    return JS_STEP_DONE;
done:
    rc = JS_STEP_DONE;
out:
    JS_FreeValue(ctx, slots);
    return rc;
}

static const JSTrampStepDef psc_def = {
    /* No fini: `over` is psc_visit's and `started` is the flag that declaration reads, so clearing it here
       would hide the value from the one list that frees it. */
    sizeof(PermChangeState), psc_step, NULL, 0, .visit = psc_visit,
    .algorithm = "Permissions §6.3.4 the PermissionStatus update steps",
    .steps = PSC_STEPS
};

/* ---- §6.3.1's CREATE ---------------------------------------------------------------------------------------- */

JSValue permission_status_new(JSContext *ctx, const PermissionDescriptor *d)
{
    JSValue status, slots, proto;
    JSAtom k;

    DCHECK(g_ready, "§6.3.1 created a PermissionStatus before permission_status_init declared the interface");
    /* §6.3.1 step 2: "Assert: The feature identified by name is supported by the user agent." §6.2.1 step 4
       rejects an unsupported name before this is reached, so a descriptor here always names a registry row. */
    DCHECK(d->feature >= 0, "§6.3.1's assert: a PermissionStatus was created for a feature this user agent does "
                            "not support — §6.2.1 step 4 rejects that name with a TypeError first");
    proto = permission_status_proto(ctx);
    status = JS_NewObjectProtoClass(ctx, proto, g_status_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(status), "the PermissionStatus allocation failed");
    slots = idl_slots_new(ctx);
    CHECK(!JS_IsException(slots), "the PermissionStatus slot record allocation failed");
    /* §6.3.1 steps 3.1-3.2: [[query]] is the typed descriptor and `name` is its name. */
    JS_SetPropertyStr(ctx, slots, PS_FEATURE, JS_NewInt32(ctx, d->feature));
    JS_SetPropertyStr(ctx, slots, PS_ASPECT, JS_NewBool(ctx, d->aspect));
    JS_SetPropertyStr(ctx, slots, PS_SUBJECT, JS_DupValue(ctx, d->subject));
    JS_SetPropertyStr(ctx, slots, PS_GENERATION, JS_NewInt32(ctx, 0));
    /* THE DEFAULT PERMISSION QUERY ALGORITHM — "set status's state to permissionDesc's permission state". It
       is §6.2.1 step 8.3 and §6.3.4 step 3 both, so it is performed in the one place that creates a status. */
    JS_SetPropertyStr(ctx, slots, PS_STATE, permission_state(ctx, d));
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(k != JS_ATOM_NULL, "the PermissionStatus slot key could not be interned");
    JS_SetProperty(ctx, status, k, slots);
    JS_FreeAtom(ctx, k);
    /* §6.3.4's FIRST QUESTION, asked of the world this status now exists in. */
    psc_queue(ctx, status);
    return status;
}

/* ---- the interface, per realm --------------------------------------------------------------------------- */

JSValue permission_status_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_status_class);

    DCHECK(!JS_IsNull(proto), "PermissionStatus.prototype was asked for in a realm that never ran its install");
    return proto;   /* OWNED */
}

static void permission_status_install_realm(JSContext *ctx)
{
    JSValue proto, prev, global;

    prev = JS_GetClassProto(ctx, g_status_class);
    DCHECK(JS_IsNull(prev), "permission_status_install_realm ran twice in one realm — everything already "
                            "holding the first PermissionStatus.prototype would answer out of a discarded "
                            "object");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "PermissionStatus.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "PermissionStatus");
    /* §6.3: `interface PermissionStatus : EventTarget`. addEventListener and the `onchange` handler attribute
       are reached through the chain rather than copied onto each status — which is also what makes
       `status instanceof EventTarget` true and what a page's prototype patch reaches. */
    event_target_chain(ctx, proto);
    event_target_install_handlers(ctx, proto, EH_PERMISSION_STATUS);
    idl_install_accessor(ctx, proto, "state", js_status_get_state, 0, -1);
    idl_install_accessor(ctx, proto, "name", js_status_get_name, 0, -1);
    JS_SetClassProto(ctx, g_status_class, JS_DupValue(ctx, proto));

    /* §3.7.1's INTERFACE OBJECT on THIS realm's global. §6.3 declares no constructor, so
       `new PermissionStatus()` is a TypeError — and its PRESENCE is what a feature-detecting bundle reads. */
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "PermissionStatus",
                      idl_interface_object(ctx, "PermissionStatus", proto));
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, proto);
}

void permission_status_init(JSContext *ctx)
{
    JSClassDef def = { "PermissionStatus" };
    JSRuntime *rt = JS_GetRuntime(ctx);

    DCHECK(!g_ready, "permission_status_init ran twice — the class and the slot key are the AGENT's");
    g_rt = rt;
    g_key = JS_NewSymbol(ctx, "permissionStatusState", false);
    CHECK(!JS_IsException(g_key), "the PermissionStatus slot key allocation failed");
    /* THE CLASS IS BOTH THE PER-REALM PROTOTYPE SLOT AND THE BRAND: the object WEARS it, so §3.7.5's check is
       a class-id comparison and a page cannot forge one. */
    JS_NewClassID(rt, &g_status_class);
    CHECK(JS_NewClass(rt, g_status_class, &def) == 0,
          "PermissionStatus: the per-realm prototype slot could not be declared");
    g_change_stepid = JS_RegisterStepDef(rt, &psc_def);
    g_ready = 1;
    realm_declare_intrinsic(permission_status_install_realm);
}

void permission_status_free(void)
{
    if (!g_ready)
        return;
    DCHECK(g_rt != NULL, "PermissionStatus was declared without recording the runtime its key belongs to");
    /* The prototypes and interface objects are the REALMS' — each is released with its context. What the agent
       holds is the slot key, and a component that mints a runtime-lifetime value owns it. */
    JS_FreeValueRT(g_rt, g_key);
    g_key = JS_UNDEFINED;
    g_change_stepid = -1;
    g_ready = 0;
    g_rt = NULL;
}
