/* ANIMATION FRAMES — HTML §8.12 Animation frames. See animation_frame.h for why the map is a heap object and why the
   snapshot-then-recheck is the algorithm rather than an optimisation. */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/idl_name_chain.h"   /* an unknown handle is a name-keyed elimination chain over the map's keys */
#include "solver/concolic.h"
#include "core/realm.h"
#include "core/rendering/animation_frame.h"
#include "solver/rung_entry.h"  /* this rung's denominator — the names a program must spell to ask for it */

/* THE PER-REALM STORE. §8.12 Animation frames gives every Window "a map of animation frame callbacks" and "an animation frame
   callback identifier", and both are per-Window rather than per-agent — a child navigable's rAF handles are
   its own, and a member installed once answers with the realm it was DEFINED in unless the state is reached
   through the realm. The slot holds ONE object for the realm's whole life and is never replaced: what
   time-travels is the object's PROPERTIES, which the heap COW captures, and replacing the slot would put one
   flow's map where every other flow looks. */
static JSClassID g_slot = JS_INVALID_CLASS_ID;
static int g_id_request = -1, g_id_cancel = -1;
static JSAtom g_atom_queue = JS_ATOM_NULL, g_atom_next = JS_ATOM_NULL;
static int g_ready;

/* The store, OWNED. */
static JSValue af_store(JSContext *ctx)
{
    JSValue st;

    DCHECK(g_ready, "a Window's map of animation frame callbacks was reached before §8.12 Animation frames was declared");
    st = realm_value_get(ctx, g_slot);
    DCHECK(JS_IsObject(st),
           "a realm answered §8.12 Animation frames's map of animation frame callbacks with no map — every Window is given one "
           "at creation, so this realm never ran animation_frame_install and its `requestAnimationFrame` "
           "would register into nothing");
    return st;
}

/* The entry list, OWNED. Entry i is a two-element array [handle, callback], or `undefined` for an entry that
   has been taken or cancelled — a TOMBSTONE, because the snapshot is an index range and compacting under it
   would renumber the keys the run is walking. */
static JSValue af_queue(JSContext *ctx)
{
    JSValue st = af_store(ctx), q = JS_GetProperty(ctx, st, g_atom_queue);

    JS_FreeValue(ctx, st);
    DCHECK(JS_IsArray(q), "§8.12 Animation frames's map of animation frame callbacks lost its entry list");
    return q;
}

static uint32_t af_len(JSContext *ctx, JSValueConst q)
{
    JSValue len = JS_GetPropertyStr(ctx, q, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    return n;
}

bool animation_frame_pending(JSContext *ctx)
{
    JSValue q = af_queue(ctx);
    uint32_t i, n = af_len(ctx, q);
    bool any = false;

    for (i = 0; i < n && !any; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, q, i);
        any = JS_IsObject(e);                     /* a tombstone is not a pending callback */
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, q);
    return any;
}

uint32_t animation_frame_snapshot(JSContext *ctx)
{
    JSValue q = af_queue(ctx);
    uint32_t n = af_len(ctx, q);

    JS_FreeValue(ctx, q);
    return n;
}

JSValue animation_frame_take(JSContext *ctx, uint32_t i)
{
    JSValue q = af_queue(ctx), e, cb;

    DCHECK(i < af_len(ctx, q),
           "§8.12 Animation frames's walk asked for an entry past the snapshot it took — the map only ever GROWS during a run, "
           "so an index inside the snapshot cannot fall off the end");
    e = JS_GetPropertyUint32(ctx, q, i);
    if (!JS_IsObject(e)) {   /* step 3's re-check: cancelled by an earlier callback of THIS frame */
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, q);
        return JS_UNDEFINED;
    }
    cb = JS_GetPropertyUint32(ctx, e, 1);
    DCHECK(JS_IsFunction(ctx, cb),
           "§8.12 Animation frames's map held an entry whose callback is not callable — the IDL's FrameRequestCallback brand "
           "check is what puts one there, so nothing else can have");
    /* "remove callbacks[handle]" — BEFORE the invoke, which is what makes a callback that re-registers itself
       get a NEW handle rather than resurrecting the one being run. */
    JS_SetPropertyUint32(ctx, q, i, JS_UNDEFINED);
    JS_FreeValue(ctx, e);
    JS_FreeValue(ctx, q);
    return cb;
}

void animation_frame_run_end(JSContext *ctx, uint32_t consumed)
{
    JSValue q = af_queue(ctx);
    uint32_t n = af_len(ctx, q), i;

    DCHECK(consumed <= n, "§8.12 Animation frames's walk consumed more entries than the map ever held");
    for (i = consumed; i < n; i++)
        JS_SetPropertyUint32(ctx, q, i - consumed, JS_GetPropertyUint32(ctx, q, i));
    JS_SetPropertyStr(ctx, q, "length", JS_NewUint32(ctx, n - consumed));
    JS_FreeValue(ctx, q);
}

/* HTML §8.12 Animation frames: `unsigned long requestAnimationFrame(FrameRequestCallback callback)`.
   1. Let target be this Window. 2. Let handle be target's animation frame callback identifier, incremented.
   3. Set callbacks[handle] to callback. 4. Return handle. No page code runs here: the callback's brand check
   is the declaration's (IDL_CALLBACK), so by the time this body runs there is nothing left to coerce. */
static JSValue js_request_animation_frame(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                          int magic)
{
    JSValue st, q, entry, nv;
    uint32_t handle = 0;

    (void)this_val; (void)magic;
    DCHECK(argc >= 1,
           "requestAnimationFrame reached its body with no callback — §3.6 step 5 throws for a call short of "
           "a member's REQUIRED arguments, and that throw is the declaration's");
    DCHECK(JS_IsFunction(ctx, argv[0]),
           "requestAnimationFrame's callback reached the body uncoerced — Web IDL §3.2.19's brand check belongs to "
           "the declaration (IDL_CALLBACK), and a body that re-tests it is a second answer to one question");
    st = af_store(ctx);
    nv = JS_GetProperty(ctx, st, g_atom_next);
    JS_ToUint32(ctx, &handle, nv);
    JS_FreeValue(ctx, nv);
    DCHECK(handle >= 1, "§8.12 Animation frames's animation frame callback identifier started below 1 — 0 is the handle a page "
                        "gets back for nothing, and `cancelAnimationFrame(0)` must name no entry");
    JS_SetProperty(ctx, st, g_atom_next, JS_NewUint32(ctx, handle + 1));
    JS_FreeValue(ctx, st);

    entry = JS_NewArray(ctx);
    CHECK(!JS_IsException(entry), "animation frames: OOM recording a requestAnimationFrame callback — a "
                                  "dropped one is a frame the page asked for and never gets");
    JS_SetPropertyUint32(ctx, entry, 0, JS_NewUint32(ctx, handle));
    JS_SetPropertyUint32(ctx, entry, 1, JS_DupValue(ctx, argv[0]));
    q = af_queue(ctx);
    JS_SetPropertyUint32(ctx, q, af_len(ctx, q), entry);
    JS_FreeValue(ctx, q);
    return JS_NewUint32(ctx, handle);
}

/* The entry list's handle at slot `i`, or 0 for a hole (a handle is never 0: af_reset starts the identifier at 1). */
static uint32_t af_handle_at(JSContext *ctx, JSValueConst q, uint32_t i)
{
    JSValue e = JS_GetPropertyUint32(ctx, q, i), h;
    uint32_t got = 0;

    if (JS_IsObject(e)) {
        h = JS_GetPropertyUint32(ctx, e, 0);
        JS_ToUint32(ctx, &got, h);
        JS_FreeValue(ctx, h);
        DCHECK(got >= 1, "an entry of §8.12's map of animation frame callbacks is filed under the handle 0");
    }
    JS_FreeValue(ctx, e);
    return got;
}

/* The least handle the map holds at or above `from`, or 0 when it holds none. */
static uint32_t af_handle_from(JSContext *ctx, uint32_t from)
{
    JSValue q = af_queue(ctx);
    uint32_t i, n = af_len(ctx, q), best = 0;

    for (i = 0; i < n; i++) {
        uint32_t h = af_handle_at(ctx, q, i);

        if (h && h >= from && (best == 0 || h < best))
            best = h;
    }
    JS_FreeValue(ctx, q);
    return best;
}

/* §8.12 step 3, "remove callbacks[handle]": the entry filed under `want`, if the map holds one. */
static void af_remove(JSContext *ctx, uint32_t want)
{
    JSValue q = af_queue(ctx);
    uint32_t i, n = af_len(ctx, q);

    for (i = 0; i < n; i++)
        if (af_handle_at(ctx, q, i) == want) { JS_SetPropertyUint32(ctx, q, i, JS_UNDEFINED); break; }
    JS_FreeValue(ctx, q);
}

/* HTML §8.12 Animation frames: `undefined cancelAnimationFrame(unsigned long handle)` — "remove callbacks[handle]".
 * A handle that names nothing does nothing, which is why there is no throw here and no report: a page cancelling a
 * frame it already ran is ordinary code.
 * A step machine, because §3.2's conversion hands unknown external input across as itself, and the worlds step 3
 * tells apart over an unknown handle are exactly the keys the map holds plus one remainder in which it names none
 * and nothing is removed. That is a name-keyed elimination chain (core/idl_name_chain.h): one two-armed ask per
 * handle, ascending from the cursor — handles are monotone per global and start at 1, as §8.7's identifiers are
 * (core/timing/timer.c's js_clear_timer walks its map the same way) — keyed by the handle's own name, never its
 * slot. The yes arm removes that entry; the chain's end is the remainder. */
#define AF_PREDICATE "HTML §8.12 Animation frames cancelAnimationFrame (handle is the callback with handle"
IDL_NAME_CHAIN_SPELLS(AF_PREDICATE, IDL_NAME_CHAIN_U32_BYTES);
#define AF_ALGORITHM "HTML §8.12 Animation frames the cancelAnimationFrame(handle) method steps"
#define AF_STAGES(X) X(AF_REMOVE, AF_ALGORITHM " (step 3: remove callbacks[handle])")
enum { IDL_STEP_STAGE_BASE(AF_STAGES) AF_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const AF_STEPS[] = { AF_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    uint32_t        next;   /* the least handle the chain has not yet eliminated */
    IdlNameChainKey key;
} CancelAfState;

static void caf_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

static int js_cancel_animation_frame(JSContext *ctx, JSStepHdr *hdr, void *state, int argc, JSValueConst *argv,
                                     JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    CancelAfState *s = state;
    double want = 0;
    int have;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);   /* this machine makes no request that delivers a value */
    *presult = JS_UNDEFINED;
    DCHECK(hdr->stage == AF_REMOVE, "cancelAnimationFrame resumed into a stage its declaration does not have");
    DCHECK(argc >= 1,
           "cancelAnimationFrame reached its body with no handle — §3.6 step 5's TypeError is the "
           "declaration's, not this body's");
    /* The conversion run on the example, read once so every link decides against one reading of it; 0 for an
       unknown carrying no example, which the chain reads as JS_OUTCOME_REAL_UNSTATED. */
    have = idl_number_of(ctx, IDL_UNSIGNED_LONG, argv[0], &want);
    if (!concolic_is(argv[0])) {
        DCHECK(have, "a known handle's §3.2.4.6 conversion produced no number");
        af_remove(ctx, (uint32_t)want);
        return JS_STEP_DONE;
    }
    for (;;) {
        uint32_t h = af_handle_from(ctx, s->next);
        char name[IDL_NAME_CHAIN_U32_BYTES + 1];
        int real, rc;
        bool yes = false;

        if (h == 0)
            return JS_STEP_DONE;   /* the remainder: the handle names no entry, and nothing is removed */
        real = have ? ((uint32_t)want == h) : JS_OUTCOME_REAL_UNSTATED;
        snprintf(name, sizeof name, "%u", (unsigned)h);
        rc = idl_name_chain_ask(ctx, hdr, &s->key, argv[0], AF_PREDICATE, name, real, AF_ALGORITHM, &yes);
        if (rc)
            return rc;
        if (yes) {
            af_remove(ctx, h);
            return JS_STEP_DONE;
        }
        DCHECK(h < UINT32_MAX, "§8.12's elimination chain answered no at the largest handle a uint32 can hold");
        s->next = h + 1;
    }
}

static const IdlStepDecl CAF_DECL = {
    js_cancel_animation_frame, sizeof(CancelAfState), caf_visit, NULL, AF_ALGORITHM, AF_STEPS, 0, NULL
};

void animation_frame_init(JSContext *ctx)
{
    static const IdlArgType REQUEST_ARGS[1] = { IDL_CALLBACK };
    static const IdlArgType CANCEL_ARGS[1] = { IDL_UNSIGNED_LONG };

    DCHECK(!g_ready, "animation_frame_init ran twice — §8.12 Animation frames's members are declared once per agent");
    g_id_request = idl_method_id(ctx, REQUEST_ARGS, 1, js_request_animation_frame, 0);
    g_id_cancel = idl_method_id_step(ctx, CANCEL_ARGS, 1, NULL, 0, &CAF_DECL, 0);
    g_atom_queue = JS_NewAtom(ctx, "animationFrameCallbacks");
    g_atom_next = JS_NewAtom(ctx, "animationFrameCallbackIdentifier");
    CHECK(g_atom_queue != JS_ATOM_NULL && g_atom_next != JS_ATOM_NULL,
          "animation frames: the map's own keys could not be interned");
    g_slot = realm_value_declare(ctx, "§8.12 Animation frames map of animation frame callbacks");
    g_ready = 1;
    realm_declare_intrinsic(animation_frame_install_map);
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — core/agent_state.h. The two POOL ENTRIES are the
       reason this declaration is not a formality: the release below used to give back the two atoms and the
       realm slot and leave `g_id_request` and `g_id_cancel` exactly as this init set them, while the latch it
       consults first is `g_ready` — so a SECOND agent in one process re-declared both members and then
       installed the FIRST agent's ids, which idl_args_pool_free has already reset to index 0 and whose step
       definitions were registered in a runtime that is gone. Nothing could report it: a pool entry is an int,
       so neither of JS_FreeRuntime's censuses has anything to say about one, and the only reader is the id
       idl_install_method hands to a member the next agent's page then calls. */
    agent_state_flag("animation_frame", &g_ready, "the declaration latch");
    agent_state_id("animation_frame", &g_id_request,
                   "HTML §8.12 Animation frames's requestAnimationFrame member declaration");
    agent_state_id("animation_frame", &g_id_cancel,
                   "HTML §8.12 Animation frames's cancelAnimationFrame member declaration");
    agent_state_atom("animation_frame", &g_atom_queue,
                     "HTML §8.12 Animation frames's map-of-animation-frame-callbacks key on a Window's record");
    agent_state_atom("animation_frame", &g_atom_next,
                     "HTML §8.12 Animation frames's animation-frame-callback-identifier key on that record");
    agent_state_realm_slot("animation_frame", &g_slot,
                           "the per-realm slot HTML §8.12 Animation frames's map is held in");
}

/* THE MAP IS BUILT AT REALM INSTALL, which puts it in the pre-boot BASELINE. Built lazily on the first
   `requestAnimationFrame` instead it would be whichever FLOW touched it first that owned it, and every
   sibling would then be registering into an object created inside another flow's delta. */
void animation_frame_install_map(JSContext *ctx)
{
    JSValue st;

    DCHECK(g_ready, "a realm asked for §8.12 Animation frames's map before the interface was declared");
    /* Running twice in one realm is asserted by realm_value_set, which is where the first value is standing. */
    st = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(st), "animation frames: this Window's map could not be allocated");
    {
        JSValue q = JS_NewArray(ctx);
        CHECK(!JS_IsException(q), "animation frames: this Window's entry list could not be allocated");
        JS_SetProperty(ctx, st, g_atom_queue, q);
    }
    JS_SetProperty(ctx, st, g_atom_next, JS_NewUint32(ctx, 1));   /* §8.12 Animation frames: handles start at 1 */
    realm_value_set(ctx, g_slot, st);
}

/* THE NAMES THIS COMPONENT INSTALLS ITSELF UNDER, SPELLED ONCE AND USED TWICE — at the install below, and in
   the table this file declares to the @COLD census so that a document which NAMES this rung's work and whose
   arm of `stepUnitRuns` reads zero is a FINDING there rather than a zero indistinguishable from a document that
   names none. ONE constant rather than two literals: the census's copy and the install's copy would otherwise be
   free to drift, and drift here is silent in the worst direction — a name that stopped matching reads as a
   program that never asked. §Fix-the-ROOT prefers making the state impossible to asserting it is absent, and
   sharing the constant does that, so no check on the pair exists or is needed. */
static const char RAF_REQUEST_NAME[] = "requestAnimationFrame";
/* …AND THE RENDERING RUNG'S ENTRY TABLE, WHICH IS A LIST AND IS SHORTER THAN THE RUNG'S OWN WORK. solver/engine.c
   names ResizeObserver delivery, IntersectionObserver, scroll/resize/pagereveal and the Web Animations
   checkpoint as work that same arm runs; each belongs here when the component that owns it is built, which is
   why the denominator this publishes RISES as those land and is a floor until they do. `cancelAnimationFrame` is
   NOT here: cancelling asks the rung for nothing, so counting it would raise a denominator on a program that
   only ever tears a callback down. */
static const char *const RENDERING_ENTRY_NAMES[] = { RAF_REQUEST_NAME, NULL };

void animation_frame_install(JSContext *ctx, JSValueConst global)
{
    DCHECK(g_ready, "§8.12 Animation frames's members were installed before they were declared");
    idl_install_method(ctx, (JSValue)global, RAF_REQUEST_NAME, g_id_request);
    idl_install_method(ctx, (JSValue)global, "cancelAnimationFrame", g_id_cancel);
    rung_entry_declare(STEP_UNIT_RENDERING, RENDERING_ENTRY_NAMES);
}

/* THE RUNTIME, NOT A REALM, and it is core/platform.c's release column that calls it — see core/platform.h.
   What this holds is AGENT state (two interned keys, a realm-value slot and two member declarations), so the
   thing it is released against is the agent, which is a JSRuntime; taking a JSContext is what made it a line
   each of three hosts had to remember, which is the drift that column exists to end. */
void animation_frame_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;`. The release is the inverse of the DECLARATION and rides the same row of
       core/platform.c's one list, whose declare pass is unconditional and whose table asserts that a release
       row has a declare — so reaching here undeclared is a host that tore this component down with something
       that is not the platform's one list, and an early return is what would hide it. */
    DCHECK(g_ready,
           "§8.12 Animation frames's members were released in an agent that never declared them");
    g_ready = 0;
    /* The MAPS are the realms' — each is released with its context, which is what the per-realm slot array
       is for, and each flow's own entries go with the delta that holds them. What this owns is the two
       interned keys. */
    JS_FreeAtomRT(rt, g_atom_queue);
    JS_FreeAtomRT(rt, g_atom_next);
    g_atom_queue = g_atom_next = JS_ATOM_NULL;
    g_slot = JS_INVALID_CLASS_ID;
    /* AND THE TWO MEMBER DECLARATIONS, which this release used to keep. They name entries in a pool
       idl_args_pool_free restarts at 0 and step definitions registered with a runtime that is going away
       with them (core/agent_state.h). */
    g_id_request = g_id_cancel = -1;
}
