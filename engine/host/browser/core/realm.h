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
 * environment at creation, exactly as §7.2.6's policy container is cloned from the creator at creation — so a
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
 * `HTMLDialogElement.prototype.showModal`. A dotted path rather than a bare name because the INTERFACE OBJECT is
 * often not the producer: `HTMLDialogElement` exists in this build while `showModal`, the member that puts an
 * element in the top layer, does not. A path names the INTERFACE OBJECT rather than an instance
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
