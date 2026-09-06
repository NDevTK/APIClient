/* THE PER-REALM INTRINSICS, IN ONE PLACE EVERY REALM GOES THROUGH.
 *
 * §3.7 gives every realm its own interface prototype objects, and in this engine that decides ANSWERS and not
 * just identities: a C member runs in the realm that DEFINED it (js_call_c_function takes `ctx` from the
 * function object), so a prototype shared between documents answers every document's question out of whichever
 * realm happened to build it first. Each such component therefore has an install that builds THIS realm's copy.
 *
 * WHAT THIS FILE EXISTS FOR IS THAT THE LIST OF THEM MUST NOT BE HAND-COPIED. It was: `event_target_install`
 * was written into three hosts' child-realm builders, one line each, and a component added without touching
 * all three would silently share another realm's prototype with nothing to say so. Three components became
 * nine lines the moment a second interface needed the same treatment, which is the point at which a
 * hand-maintained list stops being maintained.
 *
 * SO A COMPONENT DECLARES ITSELF, at agent init, beside the declaration it already makes there — and every
 * realm runs the declared list. A component that declares nothing installs nothing, which is how a host that
 * does not build a given interface stays correct without anybody writing down which host builds what. The
 * ORDER is the declaration order, which is already the dependency order: Event declares before MessageEvent,
 * so a realm's Event.prototype exists before the prototype that chains to it. */
#ifndef ENGINE_HOST_BROWSER_CORE_REALM_H
#define ENGINE_HOST_BROWSER_CORE_REALM_H

#include <stdbool.h>

#include "quickjs.h"

typedef void (*RealmIntrinsic)(JSContext *ctx);

/* Declared ONCE PER AGENT, by the component, from its own `_init`. */
void realm_declare_intrinsic(RealmIntrinsic install);
/* Run for EVERY realm — the agent's first one after its `_init`s, and each child navigable's realm as it is
   built. Exactly once per realm: each component's install asserts its own prototype is not already there.
 *
 * IT ALSO CREATES THE REALM'S ENVIRONMENT, and that is why the one call every realm goes through takes an
 * argument now. HTML §8.1.3.2 creates an environment settings object WITH the realm, and one of its fields is
 * read before any page script runs: §8.1.3.5 decides from the TOP-LEVEL CREATION URL whether this environment
 * is a SECURE CONTEXT, and Web IDL §3.3.13's [SecureContext] members are then INSTALLED OR NOT — so the fact
 * has to exist before the intrinsics do. A component cannot supply it (a realm intrinsic has no document in
 * front of it), and a line each host writes before this call is the hand-copied list this whole file exists to
 * abolish; an ARGUMENT is the version a host cannot forget, because forgetting it does not compile.
 *
 * AND THE TOP-LEVEL CREATION URL IS NULL FOR A WORKER, WHICH IS WHY THE THIRD ARGUMENT ARRIVED. HTML §10.2.6.2
 * "Script settings for workers" sets a worker environment's fields straight out: "creation URL to worker
 * global scope's url, top-level creation URL to null". So the field this call used to REQUIRE is exactly the
 * field a worker environment does not have, and §8.1.3.5 "Secure contexts" is why that costs nothing — its
 * step 1.2 answers for a WorkerGlobalScope and RETURNS before step 2 ever reads the URL. Pass NULL for a
 * worker realm and the value §8.1.3.5 step 1.2.1 reads instead; pass the URL and `false` for a Window one.
 * Each combination is asserted, so the pair a host cannot state wrongly is the only pair that passes. */
void realm_install_intrinsics(JSContext *ctx, const char *top_level_creation_url,
                              const char *global_interface, bool owner_is_secure_context);

/* WEB IDL §3.3.8 [Global]: is this realm's global object a WorkerGlobalScope, or a WorkletGlobalScope — HTML
 * §8.1.3.5 "Secure contexts" step 1.2's and step 1.3's conditions, over the mask above. core/idl_args.h owns
 * the reading of the mask and states why it answers an interface question; these two exist so the ONE reader
 * that needs them does not have to hold a realm and a mask at once. */
bool realm_global_is_worker(JSContext *ctx);
bool realm_global_is_worklet(JSContext *ctx);

/* HTML §8.1.3.5 "Secure contexts" STEP 1.2.1's OPERAND — "If global's owner set[0]'s relevant settings object
 * is a secure context, then return true".
 *
 * IT IS A BOOLEAN AND NOT AN OWNER, AND THAT IS THE DESIGN RATHER THAN A SIMPLIFICATION. A worker is a
 * DIFFERENT AGENT (SECURITY.md's origin-keyed agent cluster), so the owner's environment settings object lives
 * in another instance and a live reference to it crosses nothing — not a process, not an instance, not a park.
 * What crosses is the ANSWER, and the standard says in its own note that the answer is the same whichever
 * owner you ask: "We only need to check the 0th item since they will necessarily all be consistent." So the
 * fact is settled by the creating agent at worker-creation time and INHERITED, exactly as §8.1.3.1's
 * top-level creation URL is inherited by a child navigable rather than looked up.
 *
 * NAMED RESIDUAL. WHAT IS NOT COVERED: §10.2.1.1 "The WorkerGlobalScope common interface"'s owner set itself —
 * "A WorkerGlobalScope object has an associated owner set (a set of Document and WorkerGlobalScope objects)" —
 * is not built, so nothing can ADD an owner, and the only reader of that set this engine has is step 1.2.1,
 * whose answer arrives as this value instead. WHAT THE NEXT DIFF BUILDS: §10.2.1.1's interface, whose state
 * includes that set; step 1.2.1 then reads owner set[0]'s stored answer through it and this accessor becomes
 * the set's first element rather than a field. HOW ITS ABSENCE WOULD SHOW: a NESTED dedicated worker — a
 * worker created by a worker, which §10.2.3 "The worker's lifetime" is explicitly about ("i.e. if we are
 * creating a nested dedicated worker") — has a WorkerGlobalScope in its owner set rather than a Document, and
 * with no set there is nowhere for the outer worker to be recorded; the inner one's answer would have to be
 * re-stated by whoever creates it instead of being read off the outer global. */
bool realm_owner_is_secure_context(JSContext *ctx);

/* WEB IDL §3.3.8 [Global]'s GLOBAL NAMES this realm's global object implements — the REALM side of §3.3.7
 * [Exposed] step 1's "realm.[[GlobalObject]] does not implement an interface that is in construct's exposure
 * set". Resolved ONCE, from the interface name the host stated above, into the bit set
 * browser/idl_exposure.h generates from the corpus's own [Global] annotations.
 *
 * IT IS AN ARGUMENT TO THE CALL ABOVE FOR THE SAME REASON THE TOP-LEVEL CREATION URL IS, and the reason is
 * worth restating because this is the second field to arrive by it: a realm's platform SURFACE is decided
 * before its intrinsics are built, a component cannot supply the fact (a realm intrinsic has no host in front
 * of it), and a line each host writes before the call is the hand-copied list this whole file exists to
 * abolish. An argument is the version a host cannot forget, because forgetting it does not compile.
 *
 * WHY A NAME AND NOT A BIT SET: the vocabulary of global names is the corpus's, so a host spelling one out of
 * a generated enum would be a host that has to include a generated table to build a realm. The interface's own
 * identifier — "Window", "DedicatedWorkerGlobalScope" — is what the IDL calls it and what §3.3.8 keys, and a
 * name the corpus does not declare [Global] aborts at the realm rather than at the first member that would
 * have been wrong about it. */
unsigned realm_global_names(JSContext *ctx);

/* HTML §8.1.3.1's TOP-LEVEL CREATION URL of THIS realm's environment — "a URL that represents the creation
 * URL of the `top-level` environment".
 *
 * IT IS INHERITED, NOT LOOKED UP. A nested navigable's environment takes the field from its PARENT
 * environment at creation, exactly as HTML §7.3.2.1 Creating browsing contexts clones the creator's policy
 * container at creation ("Set document's policy container to a clone of creator's policy container") — so a
 * document does not walk to its top to answer, and a later navigation of the top cannot retroactively change
 * what a child was created under. That inheritance is where the ancestral rule Secure Contexts §4.2 argues for
 * actually lives: an `https` iframe inside an `http` page holds the `http` address here and is correctly NOT a
 * secure context, while an `about:blank` iframe holds its creator's address rather than the free pass §3.2
 * gives `about:blank` at the top.
 *
 * OWNED — a JS string, released by the caller. It is a JS value because a per-realm fact is held in quickjs's
 * own per-context slot (below), which is the store that is already freed with the realm.
 *
 * IT IS AN ERROR TO ASK A WORKER REALM. HTML §10.2.6.2 "Script settings for workers" sets a worker
 * environment's "top-level creation URL to null", so there is no string to return and no reader entitled to
 * one — the field is not written for such a realm at all rather than written with a stand-in, because a
 * stand-in is a plausible datum and this one would decide §8.1.3.5's answer. The two readers this engine has
 * besides §8.1.3.5 both INHERIT the field into a child navigable, which a worker does not have, so the
 * DCHECK names the question rather than guarding a case anybody meant to reach. */
JSValue realm_top_level_creation_url(JSContext *ctx);

/* WEB IDL §3.8 Platform objects implementing interfaces' DESCRIPTOR **AND** §3.3.7 [Exposed] STEP 1, ASSERTED
 * OVER A REALM'S GLOBAL — see realm.c for what the walk reads and why an assert at §3.8's own entry could not
 * make either statement.
 *
 * IT ASKS TWO QUESTIONS OF ONE FACT, AND THE FACT IS THE GENERATED ROW. For every own string-keyed property of
 * the global that IDL_EXPOSURE names — which is exactly the identifiers §3.8 `define the global property
 * references` puts there, so a §3.7.6 attribute or §3.7.7 operation of the [Global] interface has no row and is
 * not in this population at all — it asserts that the property is NOT enumerable (§3.8's descriptor, which an
 * ordinary [[Set]] gets wrong) and that the identifier IS exposed in this realm (§3.3.7 step 1, which EVERY
 * bypass gets wrong, including one whose descriptor is right). Splitting them is not two auditors: one walk,
 * one table, one site, and the exposure half is asked of core/idl_args' idl_exposed_in_realm rather than
 * re-spelled here, because §3.3.7 step 1 has one statement in this tree and an auditor that restated it would
 * be the second copy.
 *
 * A CALLER MUST BE INSIDE THE REALM'S CONSTRUCTION, and that is a PRECONDITION this function cannot check. The
 * walk asserts over names it assumes THIS CODEBASE wrote; a page can add own properties to its own global, so
 * a caller that ran this after script had executed in the realm being walked would hand that page an abort
 * switch over the engine — an ordinary [[Set]] reaching the descriptor half, an Object.defineProperty reaching
 * the exposure half. Nothing here can see when it was called. realm.c carries the residual with what the next
 * diff builds; until it exists, a new caller is a change to that file and not only to this one.
 *
 * IT HAS TWO CALLERS BECAUSE A REALM'S CONSTRUCTION HAS TWO ENDS, AND THAT IS ROUTING RATHER THAN A COPY.
 * The invariant is about a FINISHED global, and which call finishes one depends on what kind of realm it is: a
 * realm whose global object is a WorkerGlobalScope is finished when realm_install_intrinsics returns, and a
 * WINDOW realm is not — core/platform.c's per-document install column runs afterwards and puts most of this
 * browser's §3.8 property references there. Asked only at the first end it audits a FRACTION of a Window's
 * global and reports it as the whole; MOVED to the second end it stops seeing worker realms altogether, which
 * is the realm kind whose surface §3.3.7 [Exposed] step 1 exists to make different. So it is one function
 * asked at both ends, and running twice over a Window's global costs nothing: the walk allocates no state,
 * decides nothing, and asserts two properties that are both monotone — a name that got there through an
 * ordinary [[Set]] does not stop having been set, and an identifier this realm's global names exclude does not
 * start being exposed.
 *
 * DEV ONLY, like the walk itself: the condition is a full own-property enumeration of the global, which is
 * work rather than a side-effect-free test, so it is the block and not merely the DCHECK that is compiled out.
 * A caller therefore guards its call with APICLIENT_DEV rather than relying on an empty body. */
#if APICLIENT_DEV
void realm_assert_global_property_references(JSContext *ctx);
#endif

/* WEB IDL §3.8 Platform objects implementing interfaces' OTHER DIRECTION — every interface object this realm
 * OWES is one some component asked §3.8's door for. The walk above is the PRESENT ⇒ CORRECT half and is blind
 * by construction to the failure below, because it enumerates properties that EXIST: an interface object that
 * was never placed at all is not a property, so there is nothing for it to walk and nothing for it to judge.
 *
 * THE OBSERVATION SITE IS §3.7.3's CLASS STRING, AND IT IS THE ONE THING THAT SURVIVES THE DELETION. The
 * failure this catches is a component whose per-realm install still BUILDS the interface prototype object and
 * no longer asks for the property — a tail call that went with a deleted install thunk, which is how `BarProp`
 * came to answer `[object BarProp]` off a live prototype while `window.BarProp` did not exist. Nothing about
 * the global records that, so the census is taken where the object is still being built: §3.7.3 Interface
 * prototype object is the step that mints it, core/idl_args' idl_interface_tag is this engine's one statement
 * of that step, and it runs on the SAME per-realm path that stopped placing the name. §3.7.3 also makes the
 * population the right one — "There will exist an interface prototype object for every interface defined,
 * regardless of whether the interface was declared with the [LegacyNoInterfaceObject] extended attribute" — so
 * an interface this engine has BUILT is one this census sees, and an interface it has not built is honestly
 * outside the population rather than charged as a gap.
 *
 * THE OTHER SIDE IS THE ASK AND NOT THE PLACEMENT, which is what makes this independent of BOTH of §3.3.7
 * [Exposed]'s steps. §3.8's door refuses on step 1 and core/idl_args' idl_install_interface_object_exposed
 * refuses on step 2 one call above it, and both refusals are CORRECT — so a census of what LANDED would fire
 * on every Window-only interface in a worker realm and on every [SecureContext] one in an insecure realm. What
 * is recorded instead is that a component ASKED, before either gate, and whether the ask is then granted is
 * the door's business and is already asserted from the other side by the walk above.
 *
 * A NO-ROW IDENTIFIER OWES NOTHING, AND THAT NEEDS NO SECOND BIT. browser/idl_exposure.h is keyed by the
 * identifiers §3.8 puts on a global, and its generator emits no row for a construct §3.8 step 3.1 refuses —
 * "If interface is not declared with the [LegacyNoInterfaceObject] or [LegacyNamespace] extended attributes,
 * then:" — so within THIS census's population a missing row means §3.8 places nothing for the identifier,
 * whatever the reason. The reasons do not have to be told apart, because they take the same action. One is a
 * §3.7.4 Named properties object, which is not an interface at all and is tagged here only because §3.7.4
 * gives it a class string built out of one: "The class string of a named properties object is the
 * concatenation of the interface's identifier and the string" — and that string is Properties, which is
 * exactly how core/frame/window.c comes to tag one WindowProperties. The other is an interface §3.8 step 3.1
 * declines to place. Neither owes a property. The third reading a bare no-row carries elsewhere — a [Global] member, an ECMAScript intrinsic, an identifier no corpus declares
 * — cannot arise in this population: idl_interface_tag's own §3.7.3 assertion refuses an identifier
 * browser/idl_inheritance.h has no row for, one call before the census is taken.
 *
 * THE CALLER STATES THAT THE REALM IS FINISHED, AND THAT IS WHY THIS IS ITS OWN ENTRY RATHER THAN A SECOND
 * QUESTION INSIDE THE WALK ABOVE. That walk's two halves are MONOTONE, so asking them at both ends of a
 * realm's construction is free; owed ⇒ asked is not monotone, and at the end of realm_install_intrinsics a
 * WINDOW realm legitimately holds interface prototype objects whose property reference core/platform.c's
 * per-document column has not placed yet. `CSSRuleList` is one: tagged by a per-realm intrinsic, asked for by
 * the document column. So there is no derivation inside this function that could tell the two ends apart —
 * the fact lives in the caller, and the caller states it by calling THIS entry rather than the other one.
 * Derive the split rather than trusting a number here: `node engine/placeaudit.mjs` prints how many identifier
 * placements each column makes.
 *
 * DEV ONLY, and the two note entries with it: they build a per-realm census object per realm, which is work
 * rather than a side-effect-free test, so the block and not merely the DCHECK is compiled out. A caller guards
 * its call with APICLIENT_DEV. */
#if APICLIENT_DEV
/* WEB IDL §3.7.3 Interface prototype object — this realm built one for `iface`, and THIS IS THE OBJECT.
   Called from core/idl_args' idl_interface_tag, which is the one statement of that step in this tree, so a
   component cannot build a prototype outside the census without first inventing a second way to give it a
   class string.
   THE OBJECT IS RECORDED AND NOT ONLY THE NAME, and the difference is what makes a whole class of check
   possible. §3.7.3 defines %Symbol.unscopables% BEFORE it defines the interface's members, so an invariant
   over a FINISHED prototype cannot be asked at the call that builds it — and until this carried the object
   there was no way to reach a realm's prototype by identifier at all, so such a check had nowhere to stand.
   It is one record and not two: the fact recorded is that this realm built THIS object for THAT interface, and
   each consumer asks its own question of it — realm_assert_interface_objects_asked reads the KEYS and ignores the
   value, idl_args.c's §3.3.14 [Unscopable] walk reads the VALUE. */
void realm_note_interface_prototype_object(JSContext *ctx, JSValueConst proto, const char *iface);
/* That object back, by identifier. JS_UNDEFINED for an interface this realm never built — the sound arm, not
   a failure: a worker realm has no Element. OWNED by the caller. */
JSValue realm_interface_prototype_object(JSContext *ctx, const char *iface);
/* WEB IDL §3.8's `define the global property references` — a component ASKED this realm's global for `id`.
   Called at the two entries that can refuse: §3.8's door itself, before §3.3.7 step 1, and
   idl_install_interface_object_exposed, before §3.3.7 step 2. Recorded BEFORE either refusal, because a
   refusal is the standard answering and not a component failing to ask. */
void realm_note_property_reference_asked(JSContext *ctx, const char *id);
/* The assertion over the two. ONLY where no further §3.8 placement will happen in this realm — see above. */
void realm_assert_interface_objects_asked(JSContext *ctx);
#endif

/* Agent teardown: the declarations are the agent's. */
void realm_intrinsics_free(void);

/* A PER-REALM VALUE THAT IS NOT A PROTOTYPE.
 *
 * §3.7's intrinsics are not all interface prototypes. `Response.json(data)` serialises with %JSON.stringify% —
 * the INTRINSIC, so a page reassigning `JSON.stringify` must not change it — and that intrinsic belongs to the
 * realm the member runs in. Held in a module static it is one realm's function object answering for every
 * document, which is the same defect as a shared prototype and harder to see because nothing about the value
 * says which realm it came from.
 *
 * THE STORE IS QUICKJS'S OWN PER-CONTEXT SLOT ARRAY, reached by declaring a class whose slot never holds a
 * prototype. That array is what the runtime already frees with the context, so a realm's value is released
 * exactly when the realm is and there is no second lifetime to get wrong. Declared ONCE PER AGENT (from a
 * component's `_init`), set from that component's per-realm install, read wherever the member runs. */
int     realm_value_declare(JSContext *ctx, const char *what);
void    realm_value_set(JSContext *ctx, int slot, JSValue v);   /* CONSUMES v */
JSValue realm_value_get(JSContext *ctx, int slot);              /* OWNED: the caller frees */

/* A STEP OF A STANDARD WHOSE PRODUCER IS NOT IN THIS BUILD — the two-sided assertion, and the reason it is a
 * function rather than a comment at each site.
 *
 * A great many algorithm steps drain a work-set that some OTHER INTERFACE fills: update-the-rendering's step 23
 * drains the top layer `dialog.showModal()` fills, and a media element's steps drain the track lists a
 * `TextTrackList` would populate. Where this engine has none of that interface, the work-set is EMPTY BY
 * CONSTRUCTION and the step has nothing to do — but writing that down as a comment states it once and never
 * checks it again. This asserts it: `path` names the MEMBER whose arrival would give the step work, and the
 * moment somebody lands it, the DCHECK fires AT the step that must then be written, with `what` naming the
 * standard, the step number and the shape of the work.
 *
 * ITS DOMAIN IS A MEMBER, AND THAT IS A LIMIT AND NOT A DETAIL. `path` is a [[HasProperty]] walk from a global,
 * so this can only ever answer "is this MEMBER INSTALLED" — and a great many producers are not members at all.
 * An INTERNAL ALGORITHM has no name on any global: CSSOM VIEW §3.1 "Scrolling"'s perform a scroll is what fills
 * §13.2's pending scroll events, and HTML §7.4.6.4 "Scrolling to a fragment" is what sets a Document's target
 * element, and neither is reachable from a global by any path. SIX SITES IN FIVE FILES ASKED FOR `scrollTo`
 * ANYWAY, as the nearest member-shaped name to those two, and the proxy was wrong in BOTH directions at once:
 * `Element.prototype.scrollTo` was installed in this build and could move nothing, so installing CSSOM VIEW
 * §4's three Window members would have fired all six on a capability that had not arrived — a probe REPORTING
 * A CAPABILITY AS PRESENT, which is worse than an absent probe because the next reader builds on it — while
 * building §7.4.6.4's three non-scrolling effects would have left every one of them silent. So the rule is
 * mechanical: **if the producer is not a member a page could name, this is the wrong instrument.** Ask the
 * component that owns the capability instead — core/dom/element_scrolling.h answers the first of those two and
 * core/dom/document.h the second — which is the same cure core/timing/hr_time.c records for
 * `crossOriginIsolated`, where the probe fired on the first coarsen of every run one turn after it was written.
 *
 * `path` is resolved from this realm's GLOBAL, dot by dot — `ResizeObserver`, `Document.prototype.activeElement`,
 * `HTMLDialogElement.prototype.showModal`. A dotted path rather than a bare name because the INTERFACE OBJECT and
 * the MEMBER are different questions: a bare `HTMLDialogElement` answers whether the interface is EXPOSED, which
 * Web IDL §3.7.3 puts on the global whole, while a step is owed work by a MEMBER, which §3.7.6/§3.7.7 declare on
 * the prototype and which arrives on its own schedule. That gap is why the dot is not decoration — and stating
 * it as a property of the two SECTIONS rather than as an example from this build is deliberate: this sentence
 * used to read "`HTMLDialogElement` exists in this build while `showModal` ... does not", which was true when it
 * was written, went false the day §4.11.4's methods landed, and then taught every reader of this paragraph with
 * an example that had gone the other way. An illustration drawn from what the tree HAS is a claim about the tree
 * in the one paragraph whose job is to teach somebody how to aim a probe.
 *
 * AND ITS DOMAIN BEING A MEMBER IS ALSO A LIMIT IN THE PRESENT DIRECTION, which is the harder half to see. The
 * paragraph above records the ABSENT direction — six sites naming `scrollTo` for producers that are internal
 * algorithms. This is the mirror: a step's operand can have SEVERAL writers, and a probe names ONE. HTML declares
 * a previously focused element on EVERY HTML element ("Each HTML element has a previously focused element which
 * is null or an element, and it is initially null"), and §6.12's show popover writes it just as §4.11.4's show a
 * modal dialog does — so a probe naming only the dialog member reports a REACHABLE step as unreachable for as
 * long as the other producer exists, and reports nothing when it lands. Before writing a path, ask what else
 * WRITES the operand, not what would naturally be said to produce it; where the answer is more than one member,
 * this is the wrong instrument for the same reason an internal algorithm is.
 *
 * A path names the INTERFACE OBJECT rather than an instance
 * (`Document.prototype.activeElement`, not `document.activeElement`) because that is where the member is
 * declared, and because resolving through an instance means traversing whatever accessors the instance carries.
 *
 * THE LAST SEGMENT IS ASKED WITH [[HasProperty]], NOT [[Get]], and that is the whole of the difference between a
 * probe and a call. Walking to the end with a property GET runs an ACCESSOR — the page's code in a C activation
 * with no flow base, which the engine aborts on by design — and `document.activeElement` became an accessor the
 * moment HTML §6.6's focus model landed, so a probe that only ever wanted a yes or no took the entire run down
 * with it. And a value is the wrong evidence anyway: a member whose getter legitimately answers `undefined`
 * reads as ABSENT, so the check would go on believing a step had nothing to do long after its producer existed.
 * INTERMEDIATE segments still need their value to be traversed, and every one on every path is an interface
 * object or a `prototype` — both plain data properties. Should an intermediate ever become an accessor, the
 * engine's own getter abort names the property it stopped on.
 *
 * IT LIVES HERE because "is this member installed in THIS realm" is a question about a realm, and because it was
 * about to be answered in two places at once: it was a static of core/rendering/rendering.c, written for the
 * twelve update-the-rendering steps whose standards are absent, and HTML §6.6.7's flush autofocus candidates has
 * two conditions of its own with the same shape. A second copy of a two-sided assertion is worse than a second
 * copy of ordinary code, because the copy that is not maintained goes on being SILENT. */
void realm_awaits(JSContext *ctx, const char *path, const char *what);

#endif
