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
 * abolish; an ARGUMENT is the version a host cannot forget, because forgetting it does not compile. */
void realm_install_intrinsics(JSContext *ctx, const char *top_level_creation_url);

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
 * own per-context slot (below), which is the store that is already freed with the realm. */
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
 * A great many algorithm steps drain a work-set that some OTHER interface fills: update-the-rendering's resize
 * steps drain a viewport's change, its step 23 drains the top layer `dialog.showModal()` fills, §6.6.7's flush
 * reads the target element §7.4.6.4's scroll-to-the-fragment sets. Where this engine has none of that
 * interface, the work-set is EMPTY BY CONSTRUCTION and the step has nothing to do — but writing that down as a
 * comment states it once and never checks it again. This asserts it: `path` names the MEMBER whose arrival
 * would give the step work, and the moment somebody lands it, the DCHECK fires AT the step that must then be
 * written, with `what` naming the standard, the step number and the shape of the work.
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
