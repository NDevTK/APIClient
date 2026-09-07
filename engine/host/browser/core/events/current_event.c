/* A WINDOW'S CURRENT EVENT — DOM §2.3 "Legacy extensions to the Window interface", and the one member that
 * reads it.
 *
 * WHAT THE STANDARD DECLARES. §2.3's partial interface is one line —
 *     [Replaceable] readonly attribute (Event or undefined) event; // legacy
 * — over one piece of state and one getter: "Each Window object has an associated current event (undefined or
 * an Event object). Unless stated otherwise it is undefined." and "The event getter steps are to return this's
 * current event."
 *
 * WHY THIS INTERFACE AND NOT ANOTHER, AND WHY IT COULD LAND AT ALL. The two committed instruments that ask
 * which platform global names this engine does not answer (testing/probe_globals.mjs) and which of those real
 * frozen bundles reach for (testing/rank_globals.mjs) put `event` in their intersection, and
 * core/events/event_path.h's own named residual named this member by name as the only reader of the event path
 * field it had left unbuilt. The test that decides whether an absent name may be built at all is WHICH
 * ALGORITHM WRITES ITS OBSERVABLES: install an interface whose behaviour lives in a subsystem this engine does
 * not have and `if (window.X)` flips true, the bundle abandons a fallback that was working, and the branch it
 * takes instead cannot complete. This member's single observable is written by DOM §2.9 "Dispatching events"'
 * inner invoke — steps 2.8.1, 2.8.2 and 2.13 — which is an algorithm core/events/event_target.c already runs
 * in full, so there is no subsystem behind it to be missing.
 *
 * AND ITS FALSE ARM WAS NOT A FEATURE GUARD, WHICH IS WORTH SAYING BECAUSE IT IS THE OPPOSITE HAZARD. The
 * reach site in the frozen corpus is `var o = n || window.event` — a FALLBACK rather than a detection — so the
 * absence cost nothing while the handler was called with its event and cost a `TypeError` on the very next
 * property read when it was not. Nothing here flips a guard, and nothing here can therefore abandon a working
 * branch; what it removes is a throw on a path a browser completes.
 *
 * THE STATE IS PER REALM AND PER FLOW, AND BOTH HALVES ARE LOAD-BEARING. Per REALM because "each Window
 * object" is the standard's own scope and because a C member runs in the realm that DEFINED it, so a module
 * static would answer every document with the first realm's event — the defect CLAUDE.md §a-per-realm-fact
 * describes, and the reason this is a `realm_value_declare` slot exactly as HTML §7.2.2.5's `status` is. Per
 * FLOW because a dispatch is what writes it: two arms of a fork dispatch different events, and the value one
 * arm is standing in is not the other's. The record is a null-prototype slot object and the write is an
 * ORDINARY PROPERTY WRITE, so the per-flow COW delta captures it and it parks and resumes with the flow that
 * set it — the same reason every other queued platform datum in this engine is a JS value rather than a C
 * field.
 *
 * THE BASELINE IS BUILT WITH THE REALM AND NOT ON FIRST TOUCH. §2.3's "Unless stated otherwise it is
 * undefined" is the record this install creates, and creating it at install time is what makes `undefined`
 * belong to the BASELINE every flow forks from rather than to whichever flow happened to read first.
 *
 * WHAT A BROWSER ANSWERS, MEASURED RATHER THAN ASSUMED. Chrome 148.0.7778.167, one run each, on a document
 * with no dispatch in flight and then inside one:
 *     window.event outside any dispatch                     -> undefined
 *     window.event inside a listener                        -> that listener's event
 *     window.event after the dispatch returns               -> undefined
 *     a nested dispatch inside a listener, then the outer   -> the inner event, then the OUTER one again
 *     a listener on a node INSIDE a shadow tree             -> undefined
 *     a listener on the shadow HOST, and on an ancestor     -> the event
 *     Object.getOwnPropertyDescriptor(window, "event")      -> an ACCESSOR pair, enumerable, configurable
 *     Object.getOwnPropertyNames(Window.prototype)          -> does NOT contain "event"
 *     window.event = 42, then window.event                  -> 42
 * The last two are Web IDL §3.7.6 "Attributes"' placement — "Regular attributes are exposed on the interface
 * prototype object, unless the attribute is unforgeable or if the interface was declared with the [Global]
 * extended attribute, in which case they are exposed on every object that implements the interface" — and
 * [Replaceable]'s own setter, and both are what idl_install_replaceable already performs. §3.7.6 and not
 * §3.8 "Platform objects implementing interfaces", which is where the property REFERENCE to an interface
 * object goes; this member is an attribute, and §3.7.6 is the section that places one. The shadow readings
 * are §2.9's step 2.8.2 arm and are why that step's condition is not decoration: they are the standard's own
 * note that this attribute "is inaccurate for events dispatched in shadow trees", made observable. */
#include <stdbool.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/events/current_event.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"

/* THE PER-REALM RECORD'S SLOT, declared once per AGENT. A slot is a class id and a class id is a registration
   in the one runtime, so a second declaration would leave every realm built under the first one reading a slot
   nothing sets — which is the assert core/frame/window.c's `status` slot carries for the same reason. */
static int g_slot = -1;

/* THE FIELD NAME ON THAT RECORD. One spelling, read and written here and nowhere else: a name spelled twice is
   a reader and a writer that can come apart, and the value they would then disagree about renders as §2.3's
   own initial `undefined` at the getter — a broken contract wearing the shape of a correct answer. */
#define CURRENT_EVENT_FIELD "event"

/* THIS REALM'S §2.3 RECORD. OWNED — the caller frees. */
static JSValue current_event_record(JSContext *ctx)
{
    JSValue rec = realm_value_get(ctx, g_slot);

    DCHECK(JS_IsObject(rec),
           "a realm answered for DOM §2.3's current event with no record — current_event_install builds it "
           "with the realm so §2.3's initial `undefined` is BASELINE, and a realm that reached this member "
           "without running that install is one nothing put a Window in");
    return rec;
}

JSValue current_event_get(JSContext *ctx)
{
    JSValue rec = current_event_record(ctx);
    JSValue v = JS_GetPropertyStr(ctx, rec, CURRENT_EVENT_FIELD);

    JS_FreeValue(ctx, rec);
    /* §2.3's type is `(Event or undefined)`, and the union has no third arm. An object here is not brand-tested
       against Event: the only writer is §2.9's inner invoke, which is handed the event it is dispatching. */
    DCHECK(JS_IsObject(v) || JS_IsUndefined(v),
           "DOM §2.3's current event holds neither an Event nor undefined — its declared type is "
           "`(Event or undefined)` and its only writer is DOM §2.9's inner invoke, so a third thing here is a "
           "second writer this member does not have");
    return v;
}

void current_event_set(JSContext *ctx, JSValueConst v)
{
    JSValue rec = current_event_record(ctx);

    DCHECK(JS_IsObject(v) || JS_IsUndefined(v),
           "DOM §2.9's inner invoke set a current event that is neither an Event nor undefined — step 2.8.2 "
           "sets it to the event being dispatched and step 2.13 puts back the undefined-or-Event step 2.8.1 "
           "saved, and neither can produce anything else");
    JS_SetPropertyStr(ctx, rec, CURRENT_EVENT_FIELD, JS_DupValue(ctx, v));
    JS_FreeValue(ctx, rec);
}

/* §2.3's GETTER STEPS — "return this's current event". `ctx` names the Window and the receiver does not: a C
   member runs in the realm that DEFINED it, so this is the Window whose current event this is, for a bare
   `event`, for `window.event`, and for `frame.contentWindow.event` where HTML §7.2.3's forward hands the read
   to the FRAME's own global and therefore to the FRAME's own getter. That is the same reading — and the same
   remaining edge, a foreign same-origin realm's Window passed as the receiver — that core/frame/window.c's
   `status` states at length; the two are one question about every Window member and are not restated here. */
static JSValue js_current_event_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)this_val;
    (void)magic;
    return current_event_get(ctx);
}

void current_event_init(JSContext *ctx)
{
    DCHECK(g_slot < 0,
           "current_event_init ran twice — DOM §2.3's per-realm slot is declared once per agent, and a second "
           "declaration would leave every realm built under the first one reading a slot nothing sets");
    g_slot = realm_value_declare(ctx, "DOM §2.3 the Window's current event");
    agent_state_id("current_event", &g_slot,
                   "the per-realm slot DOM §2.3 Legacy extensions to the Window interface' current event "
                   "record lives in");
}

void current_event_install(JSContext *ctx, JSValueConst global)
{
    JSValue rec = idl_slots_new(ctx);

    CHECK(!JS_IsException(rec), "this realm's DOM §2.3 current event record could not be allocated");
    /* §2.3: "Unless stated otherwise it is undefined." */
    JS_SetPropertyStr(ctx, rec, CURRENT_EVENT_FIELD, JS_UNDEFINED);
    realm_value_set(ctx, g_slot, rec);
    /* [Replaceable] IS THE IDL's AND NOT A CHOICE HERE — §2.3 declares the attribute with it, so a page's
       `window.event = x` must REPLACE the accessor with an own data property rather than throw or be
       swallowed, which is what idl_install_replaceable performs and what Chrome was measured doing.
       AND ITS EXPOSURE IS DECIDED FOR THIS INSTALL RATHER THAN BY IT: §2.3's own note says the attribute "is
       not available in workers or worklets", and browser/idl_exposure.h — generated from @webref/idl — already
       carries `{ "event", IDL_GLOBAL_WINDOW }`, so Web IDL §3.7.6's continue-step inside this entry refuses the
       install in a realm whose [Global] interface is not Window. That is the note enforced by the corpus
       rather than restated by a condition here, which is why there is no condition here. The record above is
       still built for such a realm and is simply never read: its only reader is the getter this line did not
       install, and DOM §2.9's writer is gated on the same "global is a Window object" question. */
    idl_install_replaceable(ctx, global, "event", js_current_event_get, 0);
}

void current_event_free(JSRuntime *rt)
{
    (void)rt;
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT IS ONE SLOT ID, and it is a slot rather than a reference: the
       RECORD it names is a REALM's object and dies with that realm's class-proto slot, the same shape
       core/frame/window.c's `status` has. So there is nothing to free above this and the undo is the whole
       release, given back from the ONE list that already names the handle — see core/agent_state.h's
       agent_state_undo for why writing `g_slot = -1` here would be a second copy of the declaration rather
       than its inverse, even at one line long. Undoing it is also what lets a second agent in one process
       declare it again, and what makes current_event_init's assert fire on a real double-declaration instead
       of on an orderly teardown. */
    agent_state_undo("current_event");
}
