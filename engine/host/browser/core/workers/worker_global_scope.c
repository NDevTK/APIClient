/* HTML §10.2.1.1 "The WorkerGlobalScope common interface" and §10.2.1.2 "Dedicated workers and the
 * DedicatedWorkerGlobalScope interface" — the OBJECTS a worker realm is made of, before any member of either
 * interface has a value.
 *
 * WHAT THIS COMPONENT IS, AND WHY IT IS THE FIRST THING A WORKER REALM OWES. Every member of both sections
 * lands on one of exactly three objects: WorkerGlobalScope.prototype, DedicatedWorkerGlobalScope.prototype, or
 * the global object itself. Web IDL decides which, and it decides it by whether the declaring interface
 * carries [Global] — §3.7.3 Interface prototype object's last conditional is "If interface is NOT declared with
 * the [Global] extended attribute, then: Define the regular attributes of interface on interfaceProtoObj given
 * realm", and §3.8 Platform objects implementing interfaces' internally create a new object takes the other
 * arm: "If interface is declared with the [Global] extended attribute, then: Define the regular operations of
 * interface on instance, given realm. Define the regular attributes of interface on instance, given realm."
 * `WorkerGlobalScope` is `[Exposed=Worker]` and NOT [Global], so its members are on ITS PROTOTYPE;
 * `DedicatedWorkerGlobalScope` is `[Global=(Worker,DedicatedWorker),Exposed=DedicatedWorker]`, so its own
 * members are OWN PROPERTIES OF THE GLOBAL and its prototype carries nothing but `constructor`. None of those
 * three objects existed, so this is not the smallest piece of §10.2.1.1 that could be built — it is the piece
 * every other piece is placed onto.
 *
 * A WORKER IS A DIFFERENT AGENT, AND NOTHING HERE CROSSES AN INSTANCE BOUNDARY. CLAUDE.md's
 * §AN-INSTANCE-IS-AN-ORIGIN-KEYED-AGENT-CLUSTER makes a worker global a separate instance from the Window that
 * owns it, and the closed set that may cross one is serialized TEXT carrying its TYPE. This file builds
 * objects INSIDE one realm and sends nothing anywhere: the two interface objects, two prototypes and one
 * attribute below are all reachable only from the realm they were minted in. §10.2.1.2's `postMessage` is the
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
    /* §3.7.3's last conditional, taken because WorkerGlobalScope is NOT [Global]. `self` is the one member of
       §10.2.1.1 whose value this engine can compute; the residuals at the bottom of this file say what the
       other five are blocked on and how each absence shows. */
    idl_install_accessor(ctx, wgs_p, "self", js_wgs_self, 0, -1);

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
    realm_declare_intrinsic(worker_global_scope_install_realm);
}

void worker_global_scope_free(JSRuntime *rt)
{
    (void)rt;
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
 *     `onrejectionhandled`, `onunhandledrejection`. NOT COVERED: none of the six is installed. NEXT DIFF: a
 *     bit of core/events/event_target.h's §8.1.8.2 mask for the set §10.2.1.1's own table declares, which is
 *     the way that file already takes a mixin's membership; `onerror` additionally needs
 *     `OnErrorEventHandler`'s five-argument invocation, which core/events/event_handler.c has for §8.1.8.1's
 *     Window `onerror` and which is a DIFFERENT member on a different interface. ABSENCE SHOWS AS:
 *     `self.onerror = f` creates an ordinary own data property on the global instead of writing §8.1.8.1's
 *     event handler map, so `Object.getOwnPropertyDescriptor(WorkerGlobalScope.prototype,"onerror")` is
 *     undefined and the handler never runs.
 *
 * (5) §10.2.1.2's `name`, `postMessage` and `close` — EVERY MEMBER OF §10.2.1.2 — AND A SUBPROBLEM UNDERNEATH
 *     ALL THREE. NOT COVERED: nothing is installed on the global object. Three separate things block them and
 *     they are not the same size:
 *       (a) THE SHARED ONE, WHICH IS THE FIRST TO BUILD. Web IDL §3.7.6's create an attribute getter step
 *           1.1.2.3 throws "if jsValue does not implement target", and this engine's one implementation of
 *           that step — core/idl_args.c's idl_attribute_this, which every attribute installed on a realm's
 *           [Global] object is minted through — resolves the receiver through core/frame/window_proxy.c and
 *           throws a TypeError naming "interface Window". A `name` installed on a worker global would
 *           therefore be an attribute whose brand check asks the wrong question of the right object. NEXT
 *           DIFF: give that one step the realm's [Global] INTERFACE instead of a hardcoded Window — the realm
 *           already states it (core/realm.c's global-names slot), so the fact exists and is not being read.
 *           ABSENCE SHOWS AS: the first §10.2.1.2 attribute installed on a worker global throws
 *           "does not implement interface Window" when read from its own realm's global object.
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
 *     `DedicatedWorkerGlobalScope includes AnimationFrameProvider` (HTML §8.12 "Animation frames") and
 *     `includes MessageEventTarget` (HTML §9.4.3 "The MessageEventTarget mixin"). Web IDL §2.3 "Interface mixins"
 *     is what makes each of those a member here and not a separate surface: an includes statement is "used to
 *     declare that all objects implementing an interface I identified by the first identifier must
 *     additionally include the members of interface mixin M identified by the second identifier", so every one
 *     of them is owed exactly as a declared member is. That is roughly twenty names, and it dwarfs (1)-(5) put together.
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
