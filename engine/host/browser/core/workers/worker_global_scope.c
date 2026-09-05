/* HTML §10.2.1.1 "The WorkerGlobalScope common interface" and §10.2.1.2 "Dedicated workers and the
 * DedicatedWorkerGlobalScope interface" — the OBJECTS a worker realm is made of, and the members whose value
 * this engine can compute, each on the object Web IDL puts it on.
 *
 * WHAT THIS COMPONENT IS, AND WHY IT IS THE FIRST THING A WORKER REALM OWES. Every member of both sections
 * lands on one of exactly three objects: WorkerGlobalScope.prototype, DedicatedWorkerGlobalScope.prototype, or
 * the global object itself. Web IDL decides which, and it decides it by whether the declaring interface
 * carries [Global] — §3.7.3 Interface prototype object's [Global] conditional reads "If interface is not
 * declared with the [Global] extended attribute, then: Define the regular attributes of interface on
 * interfaceProtoObj, given realm", and §3.8 Platform objects implementing interfaces' internally create a new
 * object takes the other arm: "If interface is declared with the [Global] extended attribute, then: Define
 * the regular operations of interface on instance, given realm. Define the regular attributes of interface on
 * instance, given realm."
 * IT IS NOT §3.7.3's LAST CONDITIONAL, AND THIS SENTENCE SAID IT WAS. The one after it is
 * "If the [LegacyNoInterfaceObject] extended attribute was not specified on interface, then …", which is what
 * defines `constructor` on an interface prototype object — so the paragraph below, which says
 * DedicatedWorkerGlobalScope.prototype "carries nothing but `constructor`", rests on the very conditional
 * this one called last. Corrected rather than deleted, because a reader who re-derives the ordering from the
 * two arms alone will re-introduce it: §3.7.3 has TWO conditionals after the [Unscopable] block, and the
 * [Global] one is the first of them.
 * `WorkerGlobalScope` is `[Exposed=Worker]` and NOT [Global], so its members are on ITS PROTOTYPE;
 * `DedicatedWorkerGlobalScope` is `[Global=(Worker,DedicatedWorker),Exposed=DedicatedWorker]`, so its own
 * members are OWN PROPERTIES OF THE GLOBAL and its prototype carries nothing but `constructor`. None of those
 * three objects existed, so this is not the smallest piece of §10.2.1.1 that could be built — it is the piece
 * every other piece is placed onto.
 *
 * REAL CHROME AGREES WITH BOTH ARMS, MEASURED RATHER THAN ASSUMED — and it is recorded here because the
 * opposite was relayed to this file as a fact. The brief that produced this diff said real Chrome DIVERGES and
 * puts some of §10.2.1.2's members on a prototype; CLAUDE.md's own answer to that ("Real Chrome is
 * CONFIRMATION, never the source of truth — the spec is") would have had the same code written either way, so
 * what the check bought is not the design but the knowledge that there is no divergence to record. Chrome
 * 148.0.7778.167, headless, a dedicated worker over a blob: script, reading
 * `Object.getOwnPropertyDescriptor` on each of the three objects: every member WorkerGlobalScope declares —
 * `self`, `location`, `navigator`, `importScripts`, `onerror`, `onlanguagechange`, `onrejectionhandled`,
 * `onunhandledrejection` — and every member its WindowOrWorkerGlobalScope include brings is an own property of
 * `WorkerGlobalScope.prototype`; every member DedicatedWorkerGlobalScope declares — `name`, `postMessage`,
 * `close` — and every member ITS includes bring — `onmessage`, `onmessageerror` from MessageEventTarget,
 * `requestAnimationFrame`, `cancelAnimationFrame` from AnimationFrameProvider — is an OWN PROPERTY OF THE
 * GLOBAL. `DedicatedWorkerGlobalScope.prototype` carries not one of them. That is §3.7.3's two arms exactly.
 *
 * WHAT MAKES THE FALSE CLAIM PLAUSIBLE IS WORTH MORE THAN THE CORRECTION: most of these names ARE on a
 * prototype, so a reader who checks a few and stops sees a prototype and calls it a divergence. The
 * discriminator is never WHERE a member is, it is WHICH INTERFACE DECLARES IT — and `WorkerGlobalScope` is
 * the not-[Global] one, which is why its members are on a prototype in a browser and must be here too.
 *
 * THE TWO DIVERGENCES THAT MEASUREMENT DID FIND ARE ABOUT EXISTENCE AND NOT ABOUT PLACEMENT: `onoffline` and
 * `ononline` are declared by §10.2.1.1 and are ABSENT from Chrome's WorkerGlobalScope entirely — absent from
 * all three objects — so a build that installs them is stricter than Chrome and correct, and a page's
 * `"ononline" in self` reads `false` there and will read `true` here. The spec is what this engine follows.
 *
 * A WORKER IS A DIFFERENT AGENT, AND NOTHING HERE CROSSES AN INSTANCE BOUNDARY. CLAUDE.md's
 * §AN-INSTANCE-IS-AN-ORIGIN-KEYED-AGENT-CLUSTER makes a worker global a separate instance from the Window that
 * owns it, and the closed set that may cross one is serialized TEXT carrying its TYPE. This file builds
 * objects INSIDE one realm and sends nothing anywhere: the two interface objects, the two prototypes and the
 * members installed below are all reachable only from the realm they were minted in. §10.2.1.2's `postMessage` is the
 * member that would cross, and it is a named residual at the bottom of this file for exactly that reason —
 * there is no peer instance to post to, and this diff provisions none.
 *
 * WHICH REALM IT BUILDS INTO IS §3.3.7 [Exposed] STEP 1's OWN QUESTION, ASKED OF THE CORPUS. core/realm.h runs
 * every declared intrinsic for EVERY realm, Window realms included, and §3.8's define the global property
 * references opens with "Let interfaces be a list that contains every interface that is exposed in realm" — so
 * the gate below is that step and not a predicate this file invented. It is asked through
 * idl_exposed_in_realm, which reads browser/idl_exposure.h's generated rows, so a Window realm declines
 * because `DedicatedWorkerGlobalScope` is IDL_GLOBAL_DEDICATEDWORKER and a Window realm's global names are
 * IDL_GLOBAL_WINDOW — nothing here restates that, and a SharedWorker realm declines by the same arithmetic
 * because §10.2.1.3's SharedWorkerGlobalScope is a different interface this build does not have.
 *
 * WHAT §10.2.1.1 AND §10.2.1.2 STILL OWE, AS NAMED RESIDUALS, is at the bottom of this file. Each states what
 * is not covered, what the next diff builds, and how its absence shows. They are residuals and not `DFAIL`s
 * because the code here is CORRECT and NARROWER than the two sections: a member this engine cannot compute is
 * honestly ABSENT (§NO STUBS), and a page's own `self.location` then throws where a browser answers — which is
 * the forcing function, not a gap to paper over with a value nobody derived. */
#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/events/event_target.h"
#include "core/workers/worker_global_scope.h"

/* §10.2.1.2's INTERFACE, AS THE CLASS THE REALM'S GLOBAL OBJECT CARRIES — one class id doing the two jobs
   core/frame/window.c's does, and for the same two reasons.
   AS A BRAND: Web IDL §3.8's "value implements interface" is a question about [[PrimaryInterface]], and a
   class id is what this engine spells that with. The global object is created by the context before any host
   class exists, so it is GIVEN this one at the install below, exactly as §7.2.2's is.
   AS A PER-REALM PROTOTYPE SLOT: §3.7 gives every realm its own DedicatedWorkerGlobalScope.prototype, and
   quickjs's per-context class-proto array is where a realm's copy lives. A module static holding one would be
   the defect core/realm.h exists to abolish — every worker realm in the agent would answer out of whichever
   realm happened to build it first. */
static JSClassID g_dwgs_class;

/* WHY THERE IS NO SECOND CLASS FOR §10.2.1.1's PROTOTYPE. WorkerGlobalScope.prototype is reachable in its
   realm as `WorkerGlobalScope.prototype` and as the [[Prototype]] of the object below it, and nothing in this
   build asks a realm for it by hand — so a class id whose only purpose is a slot nobody reads would be
   plumbing built ahead of its consumer. §10.2.1.3's SharedWorkerGlobalScope is the diff that needs one, and
   it is the diff that should add it. */

bool worker_global_scope_implements(JSValueConst v)
{
    /* §3.8: "value is a platform object and the inclusive inherited interfaces of value.[[PrimaryInterface]]
       contains interface". browser/idl_inheritance.h states the ancestry this reads —
       `{ "DedicatedWorkerGlobalScope", "WorkerGlobalScope", IDL_PROTO_INHERITS }` — so the one class id below
       is the whole of the set today. A value that is not an object is not a platform object, and
       JS_GetClassID of a non-object is not a question worth asking, so the object test comes first. */
    return JS_IsObject(v) && g_dwgs_class != 0 && JS_GetClassID(v) == g_dwgs_class;
}

/* THE SAME BRAND, IN THE SHAPE core/events/event_target.h REGISTERS PREDICATES IN — a named forwarder that
   drops the realm, exactly as core/dom/node.c writes one per tree term. The events layer may not name this
   directory (a host that installs events would then link the worker interfaces), so the fact travels as data
   the owning component states, and the `ctx` the hook carries is the shape every other term of that header
   has rather than something this answer reads: an interface brand is a property of the OBJECT. */
static bool wgs_event_implements(JSContext *ctx, JSValueConst target)
{
    (void)ctx;
    return worker_global_scope_implements(target);
}

/* ---- §10.2.1.1's `self` -----------------------------------------------------------------------------------
 *
 * "The self attribute must return the WorkerGlobalScope object itself."
 *
 * IT IS ON WorkerGlobalScope.prototype AND NOT ON THE GLOBAL, which is the one placement decision in this file
 * a reader is most likely to expect the other way round. `self` is declared by WorkerGlobalScope, and
 * WorkerGlobalScope is not the [Global] interface — DedicatedWorkerGlobalScope is — so Web IDL §3.7.3's
 * "If interface is not declared with the [Global] extended attribute" arm applies to it and its regular
 * attributes are defined on its interface prototype object. §3.7.6 Attributes says the same rule from the
 * member's side: "Regular attributes are exposed on the interface prototype object, unless the attribute is
 * unforgeable or if the interface was declared with the [Global] extended attribute, in which case they are
 * exposed on every object that implements the interface." A page reads the difference:
 * `Object.getOwnPropertyDescriptor(self, "self")` is undefined and
 * `Object.getOwnPropertyDescriptor(WorkerGlobalScope.prototype, "self").get` is a function.
 * §7.2.2's `self` is a DIFFERENT MEMBER with a different placement and a different descriptor — Window's is
 * `[Replaceable]` and Window IS [Global], so core/frame/window.c installs it on the global through
 * idl_install_replaceable_value. This one is a plain readonly attribute; borrowing that spelling would give a
 * worker a member the IDL does not declare. */
static JSValue js_wgs_self(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue js;

    (void)magic;
    /* §3.7.6's create an attribute getter, step 1.1.2.1: "Let jsValue be the this value, if it is not null or
       undefined, or realm's global object otherwise." This is not a convenience — it is what makes a bare
       `self` work at all in a worker, where the reference resolves through the global object's binding and the
       getter is reached with no receiver. */
    if (JS_IsNull(this_val) || JS_IsUndefined(this_val))
        js = JS_GetGlobalObject(ctx);
    else
        js = JS_DupValue(ctx, this_val);
    /* Step 1.1.2.2's security check is HTML §7.2.1's, whose own first step is "If platformObject is not a
       Window or Location object, then return" — a WorkerGlobalScope is neither, so the standard's answer here
       is to do nothing. Stated rather than performed, because reaching for
       core/frame/window_proxy.c from this file would make every host that builds a worker realm link the
       Window layer for a step that is defined to be a no-op over its operand. */
    /* Step 1.1.2.3: "If jsValue does not implement target, then ... throw a TypeError." A RECEIVER IS
       PAGE-SUPPLIED INPUT, so this is a TypeError and never a DCHECK — `WorkerGlobalScope.prototype.self` and
       `Object.getOwnPropertyDescriptor(WorkerGlobalScope.prototype,"self").get.call({})` are two things a
       forcing solver writes constantly, and an assert here would hand any page an abort switch over the
       engine. */
    if (!worker_global_scope_implements(js)) {
        JS_FreeValue(ctx, js);
        return JS_ThrowTypeError(ctx, "'self' called on an object that does not implement interface "
                                      "WorkerGlobalScope");
    }
    /* Step 1.1.3's getter steps, which §10.2.1.1 states in one sentence: the object itself. */
    return js;
}

/* ---- WHERE A MEMBER LANDS, ASSERTED WHERE THE PLACEMENT IS DECIDED --------------------------------------
 *
 * Web IDL §3.7.3 "Interface prototype object" and §3.8 "Platform objects implementing interfaces" put a member
 * on one of three objects and the arm is chosen by whether the DECLARING interface carries [Global]. That is
 * the one decision this file makes over and over, it is made at an INSTALL — a call whose target is an
 * argument — and nothing about a wrong target fails: the member works, `self.onmessage = f` still fires, and
 * only a `getOwnPropertyDescriptor` a page never writes can tell. So it is asserted here rather than left to
 * the fixture, which measures ONE realm built by ONE host and cannot speak for the next member added.
 *
 * IT NAMES BOTH OPERANDS AND NEITHER IS A LITERAL COPIED FROM THE OTHER: the member's IDENTIFIER, which the
 * install was given, and the OBJECT it was given, compared against the three this function was handed. A
 * violated placement therefore reports which object holds the member and which one the arm required, rather
 * than restating the call.
 *
 * EVERY VALUE IT STANDS ON IS THIS ENGINE'S. The three objects were built four lines up and no script has run
 * in this realm — realm intrinsics are installed before any page code exists — so there is no page-supplied
 * byte anywhere in the question, which is what makes a DCHECK the right macro rather than a refusal.
 *
 * [[GetOwnProperty]] IS THE OPERATION AND IT RUNS NONE OF THE PAGE'S CODE, which is JS_GetOwnPropertyNoUserCode's
 * own declared claim (a class with no exotic own-property hook answers out of its shape). A prototype-walking
 * read would answer 1 for all three objects of any chain and assert nothing at all. */
#if APICLIENT_DEV
typedef enum { WGS_ON_GLOBAL, WGS_ON_DWGS_PROTO, WGS_ON_WGS_PROTO } WgsPlacement;

static void wgs_assert_placed(JSContext *ctx, const char *member, WgsPlacement where,
                              JSValueConst g, JSValueConst dwgs_p, JSValueConst wgs_p)
{
    static const char *const WHAT[3] = {
        "the realm's global object, which is Web IDL §3.8's arm for a member DedicatedWorkerGlobalScope "
        "declares, because that interface IS [Global]",
        "DedicatedWorkerGlobalScope.prototype, which §3.7.3 leaves carrying nothing but `constructor`",
        "WorkerGlobalScope.prototype, which is §3.7.3's arm for a member WorkerGlobalScope declares, because "
        "that interface is NOT [Global]"
    };
    JSValueConst obj[3];
    JSAtom a = JS_NewAtom(ctx, member);
    int own[3], i;

    obj[0] = g; obj[1] = dwgs_p; obj[2] = wgs_p;
    DCHECK(a != JS_ATOM_NULL, "a §3.7.3 placement witness could not be interned");
    for (i = 0; i < 3; i++) {
        own[i] = JS_GetOwnPropertyNoUserCode(ctx, NULL, obj[i], a);
        DCHECK(own[i] >= 0,
               "a §3.7.3 placement probe threw — [[GetOwnProperty]] over the three objects this file just "
               "built runs none of the page's code, and no page code has run in this realm at all");
    }
    JS_FreeAtom(ctx, a);
    DCHECKF(own[(int)where] == 1,
            "`%s` is not an own property of %s — that is where Web IDL puts it, so either the install below "
            "was handed the wrong target or it did not run",
            member, WHAT[(int)where]);
    for (i = 0; i < 3; i++)
        DCHECKF(i == (int)where || own[i] != 1,
                "`%s` is an own property of %s AND is owed to %s — a member placed on two of the three objects "
                "answers from whichever the prototype chain reaches first, so the wrong one is invisible to "
                "every read a page makes by writing the member's name",
                member, WHAT[i], WHAT[(int)where]);
}
#define WGS_ASSERT_PLACED(c, m, w, g, d, p) wgs_assert_placed((c), (m), (w), (g), (d), (p))

/* §10.2.1.1's SIX EVENT HANDLER IDL ATTRIBUTES, AUDITED BY DERIVING THE SET FROM THE ROWS THAT CARRY THE BIT
 * rather than by naming them here. The install below passes a MIXIN — one bit — because that is what an
 * install is allowed to know; naming the six in this function would put a second copy of core/events/'s own
 * table in this file, which is the thing every note in that enum argues against and which the `onmessage`
 * pair below does anyway. Walking the rows asks the list that OWNS the set which members the bit named, so a
 * seventh row given the bit is audited the day it lands rather than the day somebody remembers to add a line
 * here for it.
 *
 * HOW MANY MEMBERS THAT BIT NAMES IS NOT ASKED HERE, and the reason is that this function does not run. The
 * exposure gate at the top of the install returns first in a Window realm, so everything below it — this
 * audit included — waits on a realm whose global names contain `DedicatedWorkerGlobalScope`, which HTML
 * §10.2.4 Processing model's run a worker will build and nothing in this build does. A count kept here would
 * therefore be a check that has never executed. It lives in core/events/event_target.c's row validation
 * instead, beside the two §8.1.8.2 table counts it is the same kind of claim as, where it runs at every
 * agent's init today; what is left here is the PLACEMENT question, which needs the three objects this
 * function is holding and can be asked nowhere else. */
static void wgs_assert_handlers_placed(JSContext *ctx, JSValueConst g, JSValueConst dwgs_p, JSValueConst wgs_p)
{
    int n = event_target_handler_attribute_count(), i;

    /* AND THE EMPTY WALK NEEDS NO ASSERT OF ITS OWN, which was written here and removed rather than kept:
       a bit no row carries would make this loop run zero times and pass in silence, and the state that
       produces it CANNOT REACH HERE — event_target_install_handlers aborts on exactly that mask a few lines
       above, in the same dev build this function only exists in, and in a release build neither check is
       compiled at all. An assert whose failing state an earlier assert has already stopped is a check with
       no program behind it, however well its message reads. */
    for (i = 0; i < n; i++) {
        if (!(event_target_handler_attribute_mask(i) & EH_WORKER_GLOBAL_SCOPE))
            continue;
        wgs_assert_placed(ctx, event_target_handler_attribute_at(i), WGS_ON_WGS_PROTO, g, dwgs_p, wgs_p);
    }
}
#define WGS_ASSERT_HANDLERS_PLACED(c, g, d, p) wgs_assert_handlers_placed((c), (g), (d), (p))
#else
#define WGS_ASSERT_PLACED(c, m, w, g, d, p) ((void)0)
#define WGS_ASSERT_HANDLERS_PLACED(c, g, d, p) ((void)0)
#endif

/* ---- THE REALM ---------------------------------------------------------------------------------------- */

static void worker_global_scope_install_realm(JSContext *ctx)
{
    JSValue wgs_p, dwgs_p, g, prev;

    /* WEB IDL §3.8's define the global property references STEP 1 — "Let interfaces be a list that contains
       every interface that is exposed in realm" — asked of the ONE generated table that states it. A Window
       realm runs this intrinsic like every other and has nothing to build here; that is the algorithm
       answering, not a fallback being selected, and deleting the thing it selects against would leave the
       question still to be asked. */
    if (!idl_exposed_in_realm(ctx, "DedicatedWorkerGlobalScope")) return;

    DCHECK(g_dwgs_class != 0,
           "a realm asked for DedicatedWorkerGlobalScope.prototype before worker_global_scope_init declared "
           "the class");
    prev = JS_GetClassProto(ctx, g_dwgs_class);
    DCHECK(JS_IsNull(prev),
           "worker_global_scope_install_realm ran twice in one realm — Web IDL §3.7 gives a realm ONE "
           "DedicatedWorkerGlobalScope.prototype, and a second would leave the global object chained to an "
           "object the realm has thrown away");
    JS_FreeValue(ctx, prev);

    /* §3.7.3's PROTO STEP, TWICE, AND EACH LINK IS ESTABLISHED AT CONSTRUCTION. "Otherwise, if interface is
       declared to inherit from another interface, then set proto to the interface prototype object in realm of
       that inherited interface" — `WorkerGlobalScope : EventTarget` and
       `DedicatedWorkerGlobalScope : WorkerGlobalScope`, so the chain is
       global -> DedicatedWorkerGlobalScope.prototype -> WorkerGlobalScope.prototype -> EventTarget.prototype
       -> Object.prototype. Built parent-first so there is no instant at which either object exists outside its
       chain — which is also what lets idl_interface_tag assert §3.7.3's proto step, since it reads the class
       string the PARENT already carries against browser/idl_inheritance.h's row. */
    wgs_p = event_target_derived_proto(ctx);
    idl_interface_tag(ctx, wgs_p, "WorkerGlobalScope");
    /* Web IDL §3.7.3 Interface prototype object's [Global] CONDITIONAL, taken in its NOT arm because
       WorkerGlobalScope is not the [Global] interface: "If interface is not declared with the [Global]
       extended attribute, then: Define the regular attributes of interface on interfaceProtoObj, given
       realm". It is NOT §3.7.3's last conditional — the one after it is [LegacyNoInterfaceObject]'s, which is
       what puts `constructor` on both prototypes here, and this comment called the [Global] one last while
       relying on that next one two objects further down.
       WHAT THIS ENGINE CAN COMPUTE OF §10.2.1.1 is `self` and the six event handler IDL attributes below; the
       residuals at the bottom of this file say what each remaining member is blocked on and how its absence
       shows. THE COUNT THAT STOOD HERE — "the other five" — matched no enumeration: §10.2.1.1 declares TEN
       members, so nine were owed when it was written and three are owed now, and the number is dropped rather
       than corrected because a count beside a list is a number nobody adds up. */
    idl_install_accessor(ctx, wgs_p, "self", js_wgs_self, 0, -1);
    /* HTML §10.2.1.1 The WorkerGlobalScope common interface's OWN EVENT HANDLER TABLE — "The following are
       the event handlers (and their corresponding event handler event types) that must be supported, as
       event handler IDL attributes, by objects implementing the WorkerGlobalScope interface" — installed by
       the ONE bit that names exactly that set.
       Spelling the six names here instead would be the hand-copied list core/realm.h exists to abolish, and a
       second copy of core/events/event_target.c's table; what this file states is WHICH SET, and the audit
       below reads back which members that set turned out to name.
       ON `wgs_p` AND NOT ON THE GLOBAL, which is the same §3.7.3 arm `self` takes and the opposite object
       from §10.2.1.2's `onmessage` pair further down. The discriminator is never where a handler ends up in a
       browser, it is WHICH INTERFACE DECLARES IT: these six are WorkerGlobalScope's, and WorkerGlobalScope is
       not [Global].
       IT REACHES §8.1.8.1's SPECIAL ERROR EVENT HANDLING, which is what the member buys beyond its own
       descriptor. That algorithm's step 4 — "Let special error event handling be true if event is an
       ErrorEvent object, event's type is `error`, and event's currentTarget implements the
       WindowOrWorkerGlobalScope mixin" — reads the mixin through
       event_target_implements_window_or_worker_global_scope, whose WorkerGlobalScope half this file registers
       at its own init; until now no worker global held an `onerror` entry in its event handler map, so that
       third conjunct was never asked with one and the five-argument invocation was unreachable. The map is
       written on the object the page assigns THROUGH — §8.1.8.1's determine the target of an event handler
       returns its argument for anything that is not a body or frameset element — so `self.onerror = f` in a
       worker writes the GLOBAL's map through an accessor that lives on the prototype, which is the object
       HTML §8.1.4.6 Runtime script errors fires `error` at. */
    event_target_install_handlers(ctx, wgs_p, EH_WORKER_GLOBAL_SCOPE);

    dwgs_p = JS_NewObjectProto(ctx, wgs_p);
    CHECK(!JS_IsException(dwgs_p), "DedicatedWorkerGlobalScope.prototype could not be allocated");
    idl_interface_tag(ctx, dwgs_p, "DedicatedWorkerGlobalScope");
    /* NOTHING IS INSTALLED ON dwgs_p, AND THAT IS §3.7.3 RATHER THAN AN OMISSION: DedicatedWorkerGlobalScope
       IS declared with [Global], so the "If interface is not declared with the [Global] extended attribute"
       arm is not taken and its prototype object gets its @@toStringTag and its `constructor` and no member at
       all. Its members belong on the global object, by §3.8's create-instance arm — see the residual below for
       why none of them is placed there yet. */

    g = JS_GetGlobalObject(ctx);
    /* THE BRAND. §3.8 gives a platform object a [[PrimaryInterface]]; the global object was created by the
       context before any host class existed, so it is given one here — the same thing core/frame/window.c
       does at §7.2.2's install, and for the same reason. The class owns no per-object data, which is what
       JS_SetGlobalClass requires of a class handed to an already-built object. */
    CHECK(JS_SetGlobalClass(ctx, g_dwgs_class) == 0,
          "the global object would not take the DedicatedWorkerGlobalScope class — Web IDL §3.8's "
          "[[PrimaryInterface]] is what every brand check in this realm reads, so a global without it "
          "implements nothing and §10.2.1.1's members would throw at their own realm's global object");
    JS_SetPrototype(ctx, g, dwgs_p);
    /* ECMAScript gives THE GLOBAL OBJECT an own @@toStringTag of "global", and it shadows the §3.7.3 interface
       tag on the prototype — so `Object.prototype.toString.call(self)` would answer "[object global]" where a
       browser answers "[object DedicatedWorkerGlobalScope]". That own property belongs to a plain host global,
       and this one is a platform object implementing an interface. The identical delete is at §7.2.2's
       install for the identical reason. */
    JS_DeleteProperty(ctx, g, JS_WellKnownSymbolAtom(JS_WKS_TO_STRING_TAG), 0);

    /* §10.2.1.1's `self` LANDED WHERE §3.7.3 PUT IT — asserted at the install, over the three objects this
       function has in hand, because the target of an install is an argument and a wrong one is silent. */
    WGS_ASSERT_PLACED(ctx, "self", WGS_ON_WGS_PROTO, g, dwgs_p, wgs_p);
    /* …AND §10.2.1.1's SIX, over whichever rows the bit turned out to name rather than over a list restated
       here. It runs at the same place and for the same reason `self`'s does: the target of an install is an
       ARGUMENT, a wrong one is silent, and this function is where all three objects are in hand. */
    WGS_ASSERT_HANDLERS_PLACED(ctx, g, dwgs_p, wgs_p);

    /* ---- §10.2.1.2's MEMBERS: THE OTHER ARM OF THE SAME SPLIT, ON THE INSTANCE -----------------------------
       WEB IDL §2.3 "Interface mixins" MAKES A MIXIN'S MEMBERS THIS INTERFACE'S OWN: "An includes statement is
       a definition (matching IncludesStatement) used to declare that all objects implementing an interface I
       (identified by the first identifier) must additionally include the members of interface mixin M
       (identified by the second identifier)." So `DedicatedWorkerGlobalScope includes MessageEventTarget`,
       which §10.2.1.2 writes directly beneath its IDL block, declares `onmessage` and `onmessageerror` ON THE
       [Global] INTERFACE. HTML §9.4.3 "The MessageEventTarget mixin" states them for the OBJECTS rather than
       for a prototype: "The following are the event handlers (and their corresponding event handler event
       types) that must be supported, as event handler IDL attributes, by objects implementing the
       MessageEventTarget interface".
       So §3.8's create-instance arm is the one that applies and they are OWN PROPERTIES OF THE GLOBAL — the
       opposite object from `self`, installed out of the same two sections of one standard, and the two land
       apart because of which interface DECLARES each and for no other reason.

       IT RUNS BEFORE THE TWO PROPERTY REFERENCES BELOW BECAUSE §3.8 PUTS IT THERE. Its create-instance steps
       read "Define the regular attributes of interface on instance, given realm" and, three steps later,
       "Define the global property references on instance, given realm" — the interface's own members first,
       the interface objects after. Nothing in this realm can observe the difference today, and the order is
       the algorithm's rather than this file's taste: a later member whose install reads the global's own shape
       would be the first thing that could.

       THE MASK IS A SET AND NOT A LIST OF NAMES. core/events/event_target.h's EH_PORT is the bit for exactly
       {`onmessage`, `onmessageerror`} — that header's EH_MESSAGE_PORT note says so in its own words, and says
       why `onclose` had to leave it — so it is §9.4.3's whole set and nothing else. Spelling the two names out
       here instead would be the hand-copied list core/realm.h exists to abolish, and it would put a second
       copy of §9.4.3's table in this file beside event_target.c's.
       ITS NAME SAYS MessagePort AND ITS POPULATION IS FOUR INTERFACES, WHICH IS NOT A DEBT — the reflex on
       seeing that is to ask for it to be renamed EH_MESSAGE_EVENT_TARGET, and that rename would be WRONG.
       Three of the four reach the pair through the mixin (`MessagePort includes MessageEventTarget`, `Worker
       includes MessageEventTarget`, and this one); HTML §9.5 "Broadcasting to other browsing contexts"
       declares the same two names in BroadcastChannel's OWN IDL, with no includes statement anywhere — so a
       bit named after the mixin would be carried by an interface that does not include it. What the bit is is
       the NAME SET, which is what a bitmask over event handler names is for and why it can serve all four.

       AND THE HANDLERS ARE INSTALLABLE ON A [Global] OBJECT TODAY, WHICH IS THE PART THE RESIDUAL BELOW USED
       TO DENY — see (5)(a), which is corrected rather than deleted. event_target_install_handlers goes through
       `idl_install_accessor_step`, whose getter and setter are POOL MEMBERS; only the PLAIN-C forms are minted
       through core/idl_args.c's idl_mint_plain_getter, which is the mint that wraps a global attribute in
       §3.7.6's opening steps against the Window brand. Two doors, one of them already carrying the receiver's
       interface as data. */
    event_target_install_handlers(ctx, g, EH_PORT);
    WGS_ASSERT_PLACED(ctx, "onmessage", WGS_ON_GLOBAL, g, dwgs_p, wgs_p);
    WGS_ASSERT_PLACED(ctx, "onmessageerror", WGS_ON_GLOBAL, g, dwgs_p, wgs_p);

    /* §3.8's define the global property references, step 3.1 — for both interfaces, because both are exposed
       in this realm and §3.8's population is every interface that is, not every interface that has a member
       here. `WorkerGlobalScope` is `[Exposed=Worker]` and this realm's global names contain `Worker`;
       `DedicatedWorkerGlobalScope` is `[Exposed=DedicatedWorker]` and they contain that too. Neither declares
       a constructor — §3.3.8 [Global] forbids one on the second ("the interface must not define a constructor
       operation") — so idl_interface_object's illegal-constructor mint is §3.7.1's object for both, and
       `new WorkerGlobalScope()` is the TypeError a browser throws.
       THE EXPOSURE IS THE DOOR'S ANSWER: idl_define_global_property_reference asks §3.3.7 step 1 itself,
       keyed by the identifier it is handed, so nothing at this site re-derives what the corpus states. */
    idl_define_global_property_reference(ctx, g, "WorkerGlobalScope",
                                         idl_interface_object(ctx, "WorkerGlobalScope", wgs_p));
    idl_define_global_property_reference(ctx, g, "DedicatedWorkerGlobalScope",
                                         idl_interface_object(ctx, "DedicatedWorkerGlobalScope", dwgs_p));
    JS_FreeValue(ctx, g);
    JS_SetClassProto(ctx, g_dwgs_class, dwgs_p);   /* the realm owns it from here */
    JS_FreeValue(ctx, wgs_p);
}

void worker_global_scope_init(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    /* No finalizer and no gc_mark: the object this class is given is the realm's global, whose union has never
       held opaque data — which is exactly what JS_SetGlobalClass refuses a class that would touch it. */
    JSClassDef d = { "DedicatedWorkerGlobalScope", NULL, NULL, NULL, NULL };

    JS_NewClassID(rt, &g_dwgs_class);
    CHECK(JS_NewClass(rt, g_dwgs_class, &d) == 0,
          "the DedicatedWorkerGlobalScope class could not be registered");
    agent_state_class("worker_global_scope", &g_dwgs_class,
                      "HTML §10.2.1.2's DedicatedWorkerGlobalScope — the class the worker realm's global "
                      "object carries and the per-realm slot its prototype lives in");
    /* HTML §8.2 The WindowOrWorkerGlobalScope mixin's SECOND includes statement, handed to the component that
       asks it. §8.1.8.1 Event handlers' event handler processing algorithm step 4 reads "event's
       currentTarget implements the WindowOrWorkerGlobalScope mixin", and the events layer holds the Window
       half through its tree; this is the other half, and without it that step answers the mixin question with
       the Window brand and denies a worker global the five-argument invocation §10.2.1.1's `attribute
       OnErrorEventHandler onerror` gives it.
       AGENT-SCOPED, NOT PER REALM: the brand is the class id declared on the line above, which is the agent's,
       so a second registration per worker realm would be the second claimant that entry aborts on. */
    event_target_set_worker_global_scope_terms(wgs_event_implements);
    realm_declare_intrinsic(worker_global_scope_install_realm);
}

void worker_global_scope_free(JSRuntime *rt)
{
    (void)rt;
    /* GIVEN BACK BEFORE core/events/event_target.c's own release, which asserts it: this row is declared AFTER
       `event_target` in core/platform.c's list, and its reverse-declaration order is what makes that ordering
       a checked fact rather than a remembered one. */
    event_target_set_worker_global_scope_terms(NULL);
    /* The prototype is the REALM's — quickjs frees the per-context class-proto array with the context — so
       there is no reference here to give back, only the handle. THE LAST LINE, per core/agent_state.h. */
    agent_state_undo("worker_global_scope");
}

/* ---- WHAT §10.2.1.1 AND §10.2.1.2 STILL OWE ---------------------------------------------------------------
 *
 * Each of these is a NAMED RESIDUAL and not a `DFAIL`: the code above is correct for what it does and narrower
 * than the two sections, so there is no state to crash on — the crash would fire on a realm this file builds
 * correctly. Each states what is not covered, what the next diff builds, and how its absence shows.
 *
 * (1) §10.2.1.1's `location`. NOT COVERED: `readonly attribute WorkerLocation location`, whose value
 *     §10.2.1.1 states as "the WorkerLocation object whose associated WorkerGlobalScope object is the
 *     WorkerGlobalScope object". NEXT DIFF: HTML §10.3.3 "The WorkerLocation interface" as a component under
 *     this directory, over the WorkerGlobalScope's own §10.2.1.1 `url` — which is itself unset, because
 *     §10.2.6 "Creating workers" is what sets it. ABSENCE SHOWS AS: `self.location` is `undefined` in a worker
 *     realm and `"location" in self` is false, where a browser answers a WorkerLocation.
 *
 * (2) §10.2.1.1's `navigator`. NOT COVERED: `readonly attribute WorkerNavigator navigator`. NEXT DIFF: HTML
 *     §10.3.2 "The WorkerNavigator interface". ABSENCE SHOWS AS: `self.navigator` is `undefined`, so a
 *     bundle's `navigator.userAgent` read throws a TypeError on its first line in a worker.
 *
 * (3) §10.2.1.1's `importScripts`. NOT COVERED: `undefined importScripts((TrustedScriptURL or USVString)...
 *     urls)`. NEXT DIFF: HTML §10.3.1 "Importing scripts and libraries", which is a fetch and an execution per
 *     URL and therefore a step machine on the one frontier rather than a C loop. ABSENCE SHOWS AS:
 *     `typeof importScripts` is `"undefined"`, so a classic worker's first line throws.
 *
 * (4) §10.2.1.1's SIX EVENT HANDLER IDL ATTRIBUTES — `onerror`, `onlanguagechange`, `onoffline`, `ononline`,
 *     `onrejectionhandled`, `onunhandledrejection` — ARE BUILT, and this entry is kept because two of the
 *     three things it said are now false and a reader who re-derives them will re-introduce them. It said
 *     "none of the six is installed" and that the NEXT DIFF was a new EH_WORKER_GLOBAL_SCOPE bit; that bit
 *     exists, it is carried by exactly those six rows of core/events/event_target.c's handler list, and the
 *     install is at §3.7.3's not-[Global] arm above with a placement audit that DERIVES the set from those
 *     rows. Its third claim was RIGHT and is why the member was worth building: `onerror` needed nothing
 *     beyond the bit, because §8.1.8.1's step 4 had already stopped asking the Window brand.
 *     WHAT IS STILL OWED IS NOT A MEMBER AND IT IS NOT THE REALM EITHER, AND THE SENTENCE THAT STOOD HERE
 *     SAYING OTHERWISE WAS FALSE WHEN IT WAS WRITTEN. It said that no host in this build calls
 *     realm_install_intrinsics with `DedicatedWorkerGlobalScope`, so that the exposure gate at the top of
 *     worker_global_scope_install_realm returns before anything it builds is built, and that the install
 *     itself had never executed in any realm. (Its words are not reproduced as a quotation because a quoted
 *     run spanning several comment lines is read by engine/citegen.mjs as a QUOTATION OF THE SPEC the
 *     nearest citation names, and this tree's own retired prose then reports as a fabricated one.)
 *     test_forced.c's exposure_selftest has made that call TWICE, for two realms, since the commit that added
 *     the §8.1.3.5 branch — which is an ANCESTOR of the commit that wrote the sentence. So the gate does not return, and the six accessors, the `self` accessor, the
 *     two prototypes, the two interface objects and every placement assert in this file have run on every
 *     build since.
 *     IT IS KEPT RATHER THAN DELETED BECAUSE THE METHOD IS WHAT THE NEXT READER WOULD COPY, and the method is
 *     the whole finding: this was not a claim that went stale as the tree moved — the tree had not moved —
 *     it is a claim that was never checked, because its author asked WHICH HOST BUILDS A WORKER REALM by
 *     picturing the shipped entry point instead of grepping the call. A fixture IS a host here: `main.c` and
 *     `test_forced.c` are ALTERNATIVES, each owning the program's entry, and the smoke the build runs is the
 *     second one. The check was one `git grep realm_install_intrinsics`, and it answers at the writing
 *     revision and not only today.
 *     WHAT WAS ACTUALLY OWED WAS EXECUTION. A realm existed and no SCRIPT had ever run in one, so every
 *     member's BODY was unexercised: `self`'s answer, its Web IDL §3.7.6 Attributes brand check, the class
 *     string a page reads through `Object.prototype.toString`, the brand a page reads through `instanceof`,
 *     and the six accessors' getter and setter. Those are facts about what a CALL answers and no
 *     descriptor read can decide one. test_forced.c's exposure_selftest now runs a program in that realm as
 *     a flow, under two schedules, and asserts all seven.
 *     THE ORDERED REMAINDER IS HTML §10.2.4 Processing model's run a worker, IN THAT SECTION'S OWN ORDER, and
 *     it is FIVE subproblems rather than one — the sentence above called it ONE and that is the other half of
 *     what it got wrong:
 *       (i)   THE AGENT. "Run the rest of these steps in that agent". A dedicated worker is a DIFFERENT AGENT
 *             from its owner, so it is a separate instance and the closed set that may cross the boundary is
 *             serialized TEXT carrying its type — never a live JSValue. Nothing here provisions a peer, and a
 *             cross-instance design nothing provisions has never run.
 *       (ii)  THE SCRIPT FETCH. §10.2.4's obtain-script step, whose classic arm fetches a classic worker
 *             script — one load through the one chokepoint, stating its destination and its credentials mode,
 *             with no second transport and no carve-out.
 *       (iii) RUNNING IT. "If script is a classic script, then run the classic script script" — a preemptible
 *             frame on the one frontier, which is the shape test_forced.c now drives, with the source coming
 *             from (ii) instead of from a fixture.
 *       (iv)  THE PORT PAIR. §10.2.4's inside-port steps, which entangle a MessagePort in the worker's realm
 *             with the outside one. HTML §9.2 exists in this build; what does not is an entanglement whose
 *             two ends are in two instances.
 *       (v)   HTML §10.2.6.3 Dedicated workers and the Worker interface's `Worker`, LAST — "Returns a new
 *             Worker object. scriptURL will be fetched and executed in the background, creating a new global
 *             environment for which worker represents the communication channel". It is last because
 *             installing the interface object before (i)-(iv) exist flips a bundle's `if (window.Worker)`
 *             guard TRUE and abandons the fallback branch that was working, which is worse than the absence in
 *             both arms — so the first four are not a decomposition OF (v), they are its precondition.
 *     HOW ITS ABSENCE SHOWS: `Worker` is not a declared name in a Window realm, so a page's `new Worker(u)`
 *     throws ReferenceError on its first line and there is no object for any of this to be a member of.
 *     AND ONE ARM OF HTML §8.1.8.1 Event handlers STAYS UNEXERCISED FOR A THIRD REASON AGAIN, which is worth
 *     separating because all of them look alike from outside. Special error event handling — its
 *     five-argument invocation and its inverted reading of the handler's return value — needs an ErrorEvent
 *     of type `error` whose currentTarget is a worker global HOLDING an `onerror` entry. Both halves of that
 *     are now possible: the entry is installable, and a script DOES run in a worker realm — test_forced.c
 *     drives one there as a flow and assigns `self.onerror` through the accessor. THE REASON THAT USED TO BE
 *     GIVEN — `no script runs in a worker realm yet` — is retired rather than deleted, because a reader who
 *     re-derives it will re-introduce it. What is missing now is the FIRE: HTML §8.1.4.6 Runtime script
 *     errors fires at JS_GetGlobalObject of the realm it is running in, so the algorithm is present and the
 *     CALLER is not — nothing routes a worker-realm flow's uncaught exception into it. So the member is a
 *     necessary step and not a sufficient one, and a report that says the arm is now reachable is
 *     over-claiming by exactly one caller.
 *     WHAT IS NOT A RESIDUAL HERE, stated because the reflex is to read it as one: `languagechange`,
 *     `offline` and `online` are dispatched by no algorithm in this build — but nor are they for `Window`,
 *     where the identical three rows have carried EH_WINDOW all along, so installing them on a second
 *     declaring interface adds no shape-only member that was not already this list's standing state. The
 *     three that DO have a producer reach the worker global unchanged: core/events/report_exception.c fires
 *     `error` and core/html/unhandled_rejection.c fires `unhandledrejection` and `rejectionhandled`, each at
 *     JS_GetGlobalObject of the running realm rather than at a Window.
 *
 * (5) §10.2.1.2's `name`, `postMessage` and `close` — the three members the interface DECLARES, as against the
 *     two its MessageEventTarget include brings, which are installed above. NOT COVERED: none of the three.
 *     Three separate things block them and they are not the same size:
 *       (a) THE SHARED ONE, WHOSE SCOPE THIS ENTRY GOT WRONG AND WHICH IS WHY `onmessage` LANDED WITHOUT IT.
 *           Web IDL §3.7.6's create an attribute getter step 1.1.2.3 throws "if jsValue does not implement
 *           target", and core/idl_args.c's idl_attribute_this resolves the receiver through
 *           core/frame/window_proxy.c and throws a TypeError naming interface Window. THE CLAIM THAT USED TO
 *           STAND HERE — that this is the step every attribute installed on a realm's [Global] object is
 *           minted through — IS FALSE, and it is the shape CLAUDE.md's §AN-OVER-CLAIM-IS-REFUTABLE names: an
 *           absolute about every member of a population, written from one door. There are TWO doors.
 *           idl_mint_plain_getter takes the Window-branded wrapper only `if (on_global)`, and it is reached
 *           only by the PLAIN-C-getter forms — idl_install_accessor, idl_install_accessor_no_user_code,
 *           idl_install_replaceable and idl_install_replaceable_value. `idl_install_accessor_step`, whose
 *           getter and setter are POOL MEMBERS, mints through idl_mint_accessor and never touches it; a pool
 *           member's receiver brand is per-member DATA (`idl_this_iface`), which is the very repair this entry
 *           says the next diff must build and which that half of the machine already has. §10.2.1.2's
 *           `onmessage`/`onmessageerror` are step accessors, so they are installed above and this blocks
 *           nothing about them.
 *           WHAT IT DOES STILL BLOCK is every PLAIN-C attribute on a worker global, which is `name` here
 *           (`[Replaceable]`, so idl_install_replaceable) and every WindowOrWorkerGlobalScope attribute in
 *           residual (7).
 *           NEXT DIFF: §3.7.6 step 1.1.2.3's `target` becomes THIS realm's [Global] interface, with the
 *           receiver resolution and the brand coming from that interface rather than from
 *           core/frame/window_proxy.c. idl_attribute_this now asserts exactly that gap at its own head —
 *           a DCHECKF over idl_global_names_are_window(realm_global_names(ctx)) — so the crash names the
 *           repair from inside the file that owes it, and the retired argument beside it (that Window was the
 *           only [Global] interface here) is recorded there rather than left standing.
 *           AND THE LAYERING IS THE REASON IT IS NOT ONE DIFF WITH THIS ONE. The brand that `target` would
 *           have to be for a worker realm is worker_global_scope_implements, which lives in THIS directory,
 *           and core/idl_args.c may not include core/workers/ — core/ depending on workers/ inverts the
 *           dependency and would make every host that installs an attribute link the worker layer. So the
 *           brand travels as DATA the realm's [Global] interface STATES, registered where the realm is built,
 *           the way core/events/event_target.h's event_target_set_click_terms already hands a component's
 *           predicate to a layer that must not name it. That is the shape; it is not built, and this residual
 *           is the whole of what is claimed about it.
 *           ABSENCE SHOWS AS: a PLAIN-C attribute installed on a worker global reaches that DCHECKF in a dev
 *           build, and in a release build throws "does not implement interface Window" when read from its own
 *           realm's global object — and NEITHER happens for a step accessor, which is what makes the two doors
 *           distinguishable by a one-line probe rather than by reading the mint.
 *       (b) `name` HAS NO PRODUCER. §10.2.1.1 says "A WorkerGlobalScope object has an associated name (a
 *           string). It is set during creation", and the algorithm that does the setting is HTML §10.2.4
 *           "Processing model"'s run a worker, whose own step is "Set worker global scope's name to
 *           options["name"]" — which this build does not have, so a value here would be invented rather than
 *           computed, which §RUN-DON'T-MATCH forbids.
 *           THE NUMBER HERE WAS §10.2.6 "Creating workers", WHOSE TITLE IS EXACTLY RIGHT AND WHICH DEFINES NO
 *           SUCH ALGORITHM: §10.2.6.3 Dedicated workers and the Worker interface is where the constructor
 *           CALLS run a worker, and §10.2.4 is where run a worker IS. citegen caught it; a reader who looked
 *           §10.2.6 up would have found a real heading about creating workers and stopped. NEXT DIFF: §10.2.4's
 *           run a worker, which is what states it. ABSENCE SHOWS AS: `self.name`
 *           is `undefined` where a browser answers the string the `Worker` constructor was given.
 *       (c) `postMessage` HAS NO PEER, AND NOTHING IN THIS DIFF PROVISIONS ONE. §10.2.1.2 defines it as the
 *           port's own postMessage, and the other end of that port is in the OWNER's instance — a different
 *           agent by CLAUDE.md's §AN-INSTANCE-IS-AN-ORIGIN-KEYED-AGENT-CLUSTER, so what crosses is serialized
 *           TEXT carrying its type and never a live JSValue. NEXT DIFF: §10.2.6.3's `Worker` constructor, and
 *           a host that PROVISIONS the second instance — a design that has never run is what
 *           §A-HOST-THAT-CANNOT-PROVISION-A-SECOND-INSTANCE is about, and building the member before the peer
 *           would be exactly that. ABSENCE SHOWS AS: `typeof self.postMessage` is `"undefined"`, so a worker
 *           cannot answer its owner at all.
 *       (d) `close` NEEDS A FLAG THAT HAS NOWHERE TO LIVE. §10.2.1.2's "The close() method steps are to close
 *           a worker given this", and close a worker's two steps are to "discard any tasks that have been
 *           added to workerGlobal's relevant agent's event loop's task queues" and to set the closing flag,
 *           which §10.2.2 "The event loop" declares: "Each WorkerGlobalScope object has a closing flag which
 *           must be initially false". Both operands are the WORKER's event loop, which HTML §10.2.4
 *           "Processing model"'s run a worker creates. NEXT DIFF: §10.2.4's run a worker, then this member.
 *           ABSENCE SHOWS AS: `typeof
 *           self.close` is `"undefined"`.
 *
 (7) THE MIXINS BOTH INTERFACES INCLUDE, WHICH IS THE LARGEST OF THESE AND THE ONE THIS LIST DID NOT NAME
 *     UNTIL engine/idlgen.mjs WAS RUN. NOT COVERED: `WorkerGlobalScope includes WindowOrWorkerGlobalScope`
 *     (HTML §8.2 "The WindowOrWorkerGlobalScope mixin") and `includes FontFaceSource`, and
 *     `DedicatedWorkerGlobalScope includes AnimationFrameProvider` (HTML §8.12 "Animation frames"). Its fourth
 *     include, `MessageEventTarget`, is BUILT — see the install above; this entry named it while it was not,
 *     and it is struck here rather than left to read as owed. Web IDL §2.3 "Interface mixins"
 *     is what makes each of those a member here and not a separate surface: "An includes statement is a
 *     definition (matching IncludesStatement) used to declare that all objects implementing an interface I
 *     (identified by the first identifier) must additionally include the members of interface mixin M
 *     (identified by the second identifier)." So every one of them is owed exactly as a declared member is. That is roughly twenty names, and it dwarfs (1)-(5) put together.
 *     NEXT DIFF: none of them alone — each mixin member belongs to the component that already owns it for
 *     Window (core/timing/timer.c's four timer names, core/fetch/fetch.c's `fetch`, core/structured_clone.c's
 *     `structuredClone`), so what this owes is a per-realm install in each of those components rather than a
 *     line here; the subproblem underneath every one of them is (5)(a), because each is an own property of a
 *     [Global] object and would be minted through the receiver resolution that names Window.
 *     ABSENCE SHOWS AS: `typeof setTimeout` is `"undefined"` in a worker realm, so the very first line of most
 *     bundled worker code throws — and, in the auditor rather than the engine, as the ABSENT counts idlgen
 *     prints against these two interfaces, which were ZERO before this component existed because an interface
 *     with no §3.7.3 tag anywhere is outside its audited set entirely.
 *
 * (8) A COST THIS DIFF IMPOSES ON THE AUDITOR AND NOT ON THE ENGINE, recorded because it will be read as a
 *     regression by whoever runs idlgen next. Making DedicatedWorkerGlobalScope the SECOND [Global] interface
 *     in this tree leaves idlgen unable to attribute the members core/frame/window.c installs on its `g`: it
 *     reports them UNPROVEN, saying in its own words that the receiver is reached conditionally from two
 *     tagged prototypes — which is the auditor's message and not any standard's — because both [Global]
 *     interfaces declare names like `name`, `postMessage` and `close` and a static walk cannot say which
 *     global a variable is. Nothing about those installs changed. NEXT DIFF: nothing here — the repair is in
 *     the auditor, which must attribute a [Global] install by the realm the install site states rather than by
 *     the member's name, and engine/idlgen.mjs is outside this component. HOW IT SHOWS: the UNPROVEN category
 *     in idlgen's verdict, with every row naming a core/frame/window.c line and no row naming this file.
 *
 * (6) §3.7.3's IMMUTABLE PROTOTYPE. NOT COVERED: §3.7.3 gives a [Global] interface's prototype object — and
 *     every prototype object in its inheritance chain — "the internal methods ... specific to immutable
 *     prototype exotic objects", which the two objects above are not. NEXT DIFF: an immutable-prototype form
 *     of the two mints here, which core/frame/window.c owes at §7.2.2 for the identical reason and by the
 *     identical §3.7.3 sentence, so it is ONE mechanism and not two. ABSENCE SHOWS AS:
 *     `Object.setPrototypeOf(WorkerGlobalScope.prototype, null)` succeeds in this engine and throws a
 *     TypeError in a browser. */
