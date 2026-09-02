/* ANIMATION FRAMES — HTML §8.12 Animation frames. See animation_frame.h for why the map is a heap object and why the
   snapshot-then-recheck is the algorithm rather than an optimisation. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/rendering/animation_frame.h"

/* THE PER-REALM STORE. §8.12 Animation frames gives every Window "a map of animation frame callbacks" and "an animation frame
   callback identifier", and both are per-Window rather than per-agent — a child navigable's rAF handles are
   its own, and a member installed once answers with the realm it was DEFINED in unless the state is reached
   through the realm. The slot holds ONE object for the realm's whole life and is never replaced: what
   time-travels is the object's PROPERTIES, which the heap COW captures, and replacing the slot would put one
   flow's map where every other flow looks. */
static int g_slot = -1;
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

/* HTML §8.12 Animation frames: `undefined cancelAnimationFrame(unsigned long handle)` — "remove callbacks[handle]". A handle
   that names nothing does nothing, which is why there is no throw here and no report: a page cancelling a
   frame it already ran is ordinary code. */
static JSValue js_cancel_animation_frame(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                         int magic)
{
    JSValue q;
    uint32_t i, n, want = 0;

    (void)this_val; (void)magic;
    DCHECK(argc >= 1,
           "cancelAnimationFrame reached its body with no handle — §3.6 step 5's TypeError is the "
           "declaration's, not this body's");
    /* AN UNKNOWN HANDLE IS A FORK, AND THE FORK THIS ENGINE ALREADY HAS IS THE WRONG ONE — which is why this
       crashes here rather than reaching for it. §3.2's conversion is a BOUNDARY unknown external input crosses
       AS ITSELF (core/idl_args.h's idl_concolic_rule answers IDL_CONCOLIC_CROSSES for IDL_UNSIGNED_LONG), so
       `cancelAnimationFrame(location.hash.length)` reaches this body still holding the unknown, and both the
       tag assert below and the JS_ToUint32 under it are FALSE for it — the coercion aborting inside ToNumber,
       one frame below this file, where there is no return to check.
       WHAT IS NOT COVERED. §8.12 step 3 is "Remove callbacks[handle]" over "this's target object's map of
       animation frame callbacks", whose KEYS are the handles requestAnimationFrame minted ("Increment target's
       animation frame callback identifier by one, and let handle be the result … Set callbacks[handle] to
       callback"). So the worlds this algorithm tells apart are exactly the keys that map holds, plus ONE
       remainder — a handle the map does not hold, which §8.12 answers by doing nothing, which is why there is
       no throw on this path.
       WHY IT IS NOT core/idl_index_arg.h's CHAIN. That component decomposes a §3.2.4.6 `unsigned long` into
       `npositions` singleton worlds by asking `index == k` for k ASCENDING FROM 0 — its links are POSITIONS,
       and its own banner requires each to be a world the algorithm tells apart. A handle is not a position:
       the map is sparse (every fired frame and every cancel removes a key) and the identifier only ever grows,
       so running that chain with `npositions` set to the animation frame callback identifier would ask about
       handles naming NOTHING while claiming, in the component's own words, to separate worlds §8.12 does not
       separate.
       WHAT THE NEXT DIFF BUILDS, AND THE CLAUSE HAS NOW BEEN WRONG TWICE IN THE TWO DIRECTIONS CLAUDE.md NAMES.
       Its first draft said the handle-keyed chain had to be INVENTED, and the mandated grep found it already
       written — core/timing/timer.c's js_clear_timer asks exactly this question for §8.7 Timers, one identifier
       at a time, ascending, keyed by "is `id` the timer with identifier H" so the completion carries its NAME
       and never its rank in a map the page mutates. Its second said the next diff must LIFT that chain into one
       component both members reach, and that half has now LANDED: core/idl_name_chain.h holds the link — the
       composed key, the naming rule it implements, the truncation refusal and the two-armed ask — and
       core/timing/timer.c and core/idl_index_arg.c both build their chains out of it. So this clause is now
       about what is left, which is this member's own half and nothing shared. §8.12's handles obey the two
       facts that make timer.c's enumeration sound: they are strictly monotone per global and they start at 1
       (see af_reset), so a zeroed cursor is unambiguously before the first and `cancelAnimationFrame(0)` is
       the remainder world both members answer with nothing. WHAT MUST EXIST AFTERWARD is (a) this member as an
       IdlStepBody — declared with idl_method_id_step rather than idl_method_id, because a chain parks and a
       plain C activation has nowhere to — holding an IdlNameChainKey and a cursor on its state, and (b) an
       enumeration over THIS map's keys, which is the part core/idl_name_chain.h deliberately does not own
       (core/idl_index_arg.c counts positions, timer.c walks a live map, and this walks the animation frame
       callback map — three sets, one link).
       HOW ITS ABSENCE WOULD SHOW: exactly this abort — a bundle that cancels a frame at a handle it computed
       from a URL ends the document and every sibling flow parked behind it. Once built, the world that
       answered `handle == h` and the world that exhausted the chain must differ observably at the next frame,
       in whether callback h runs. */
    if (concolic_is(argv[0]))
        DFAIL("cancelAnimationFrame's `handle` is UNKNOWN EXTERNAL INPUT and this member has no fork to ask "
              "over it. HTML §8.12 Animation frames step 3 removes callbacks[handle] from a map whose keys are "
              "the handles requestAnimationFrame minted, so the arm set is GIVEN — one world per key the map "
              "holds, plus one remainder that does nothing — and it is a set of platform-assigned NAMES rather "
              "than a range of positions. core/idl_index_arg.h's elimination chain asks `index == k` ascending "
              "from 0 and does NOT serve this member: the map is sparse and the identifier only grows, so that "
              "chain would ask about handles naming nothing while claiming to separate worlds §8.12 does not "
              "separate. The LINK such a chain is built out of is written and shared — core/idl_name_chain.h, "
              "which composes the constraint key from the member's own NAME and refuses a truncated one, and "
              "which core/timing/timer.c's js_clear_timer already builds §8.7's chain out of, keyed \"is `id` "
              "the timer with identifier H\" one monotone identifier at a time; §8.12's handles are monotone "
              "and start at 1 exactly as §8.7's do. What is missing is this member's own half: make it an "
              "IdlStepBody (idl_method_id_step, so it can park at a link) holding an IdlNameChainKey and a "
              "cursor, and walk THIS map's keys — the enumeration is deliberately the caller's, because the "
              "three askers of that link enumerate three different sets");
    DCHECK(JS_VALUE_GET_TAG(argv[0]) == JS_TAG_INT || JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(argv[0])),
           "cancelAnimationFrame's `handle` reached the body neither converted nor unknown — the IDL "
           "declaration is what converts an `unsigned long`, and that conversion is the page's code; the one "
           "value it does NOT produce a Number for is unknown external input, which the ask above answers");
    JS_ToUint32(ctx, &want, argv[0]);
    q = af_queue(ctx);
    n = af_len(ctx, q);
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, q, i), h;
        uint32_t got = 0;

        if (!JS_IsObject(e)) { JS_FreeValue(ctx, e); continue; }
        h = JS_GetPropertyUint32(ctx, e, 0);
        JS_ToUint32(ctx, &got, h);
        JS_FreeValue(ctx, h);
        JS_FreeValue(ctx, e);
        if (got == want) { JS_SetPropertyUint32(ctx, q, i, JS_UNDEFINED); break; }
    }
    JS_FreeValue(ctx, q);
    return JS_UNDEFINED;
}

void animation_frame_init(JSContext *ctx)
{
    static const IdlArgType REQUEST_ARGS[1] = { IDL_CALLBACK };
    static const IdlArgType CANCEL_ARGS[1] = { IDL_UNSIGNED_LONG };

    DCHECK(!g_ready, "animation_frame_init ran twice — §8.12 Animation frames's members are declared once per agent");
    g_id_request = idl_method_id(ctx, REQUEST_ARGS, 1, js_request_animation_frame, 0);
    g_id_cancel = idl_method_id(ctx, CANCEL_ARGS, 1, js_cancel_animation_frame, 0);
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
    agent_state_id("animation_frame", &g_slot,
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

void animation_frame_install(JSContext *ctx, JSValueConst global)
{
    DCHECK(g_ready, "§8.12 Animation frames's members were installed before they were declared");
    idl_install_method(ctx, (JSValue)global, "requestAnimationFrame", g_id_request);
    idl_install_method(ctx, (JSValue)global, "cancelAnimationFrame", g_id_cancel);
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
    g_slot = -1;
    /* AND THE TWO MEMBER DECLARATIONS, which this release used to keep. They name entries in a pool
       idl_args_pool_free restarts at 0 and step definitions registered with a runtime that is going away
       with them (core/agent_state.h). */
    g_id_request = g_id_cancel = -1;
}
