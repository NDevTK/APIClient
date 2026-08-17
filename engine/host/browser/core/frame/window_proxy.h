/* WindowProxy — HTML §7.2.3 "The WindowProxy exotic object". See window_proxy.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_WINDOW_PROXY_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_WINDOW_PROXY_H
#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "core/frame/sandboxing.h"
#include "core/url/origin.h"

/* The ACCESSOR side of §7.2.1's same-origin check is THE AGENT'S OWN ORIGIN, and it is asked of the one
   place that holds it (origin_agent) rather than passed in: an instance is an origin-keyed agent cluster, so a
   caller-supplied one could only ever agree or be wrong. */
void window_proxy_init(JSContext *ctx);
void window_proxy_free(JSContext *ctx);

/* A proxy over a navigable whose active Window is `window` and whose active document's origin is `origin`.
   Both are the binding at this moment; a navigation replaces them, PER FLOW. `doc` names WHICH document that
   Window is — a same-origin child is a second realm in this agent, so "local" is no longer a synonym for "the
   instance root" and the proxy has to say which of this agent's documents it is over. */
/* `url` is the navigable's initial address — what its REALM is built from on the first read that reaches
   through to the active document (see navigable.h). The proxy owns that realm once built; the navigable's own
   members (window/self/frames/parent/top/opener/closed/name) never need it and never build it. */
/* `creator_csp` is §7.4's CLONE of the creating document's policy container, as text — what the navigable's
   INITIAL about:blank Document runs its scripts under, since it has no response of its own to carry one. Every
   navigable is created with that document (§7.4 creates it before step 14 navigates anywhere), so every
   navigable is created with this. Taken HERE rather than at materialization, because that Document's realm is
   built lazily and the realm that builds it need not be the one that created the navigable. A LATER document —
   the one step 14's load brings — has a policy of its own, and the load carries it (navigable.c). */
/* `top_level_url` is HTML §8.1.3.1's TOP-LEVEL CREATION URL for the environments of this navigable's
   documents, and it rides here for the identical reason `creator_csp` does: it is decided by the OPERATION
   that created the navigable, the realm that will read it is built later, and the document that builds it need
   not be the creator. WHICH url it is, is §7.4 read from both ends — a CHILD navigable is nested, so it
   inherits its creator's (core/realm.h), while an AUXILIARY one is its own top-level traversable and its
   documents' top-level creation URL is its OWN address. Never NULL: every environment has one. */
/* `top_level_origin` is HTML §8.1.3.1's TOP-LEVEL ORIGIN, which is a SECOND field beside the one above and not
   a derivation of it: "This is distinct from the top-level creation URL's origin when sandboxing, workers, and
   worklets are involved." §7.3.2.1 sets the pair together and states each separately — "Let topLevelCreationURL
   be about:blank if embedder is null; otherwise embedder's relevant settings object's top-level creation URL.
   Let topLevelOrigin be origin if embedder is null; otherwise embedder's relevant settings object's top-level
   origin" — where `origin` is the origin of the initial about:blank Document the navigable is created with. So
   a CHILD navigable inherits its creator's and an AUXILIARY one is its own top-level environment, exactly as
   the URL does, and the two answers disagree wherever a URL cannot carry an identity (§7.1.1: an opaque origin
   has "no serialization it can be recreated from"). Never NULL for a local proxy. */
/* `creation_sandbox_flags` is §7.1.5's DETERMINE THE CREATION SANDBOXING FLAGS for this navigable, and it is a
   third field of the same kind as the two above: decided by the OPERATION that created the navigable, read
   when a Document of it is finally built. §7.2's create-a-new-browsing-context-and-document hands exactly this
   set to the initial about:blank Document as its ACTIVE SANDBOXING FLAG SET, and §7.4.5 makes it one half of
   the FINAL SANDBOXING FLAG SET a later navigation's Document gets — so it is read twice, by the two places a
   Document of this navigable comes into existence (window_proxy.c's materialization and navigable.c's load).
   A CREATION FACT, like `is_popup`: no flow can change it, so unlike `url` there is nothing here for the delta
   to capture, and the per-flow answer comes from the INPUTS — an `<iframe sandbox>`'s attribute is a DOM read
   in the creating flow's own delta, and the navigable that read produced belongs to that flow. */
/* `creator_csp_self_origin` is CSP §2.2's SELF-ORIGIN of that cloned policy list, SERIALIZED, and it rides
   here because §2.2 makes a CSP list a struct of policies AND an origin — carrying only the text would hand
   the initial about:blank a policy with no `'self'` to resolve. It is the CREATOR's and not this navigable's:
   §2.2's own note says the field exists so that a document which inherited its policy resolves `'self'`
   against the origin that policy came FROM, and the initial about:blank is exactly such a document. NULL only
   where `creator_csp` is — the self proxy, whose realm the host already built. */
JSValue window_proxy_new(JSContext *ctx, uint32_t doc, const char *url, const Origin *origin, const char *name,
                         bool is_popup, SandboxFlags creation_sandbox_flags, const char *creator_csp,
                         const char *creator_csp_self_origin, const char *top_level_url,
                         const Origin *top_level_origin, JSValueConst parent, JSValueConst opener);

/* §7.1.5's creation sandboxing flags for this navigable — see window_proxy_new. Asked only of a LOCAL proxy:
   a remote navigable's Documents are a peer instance's to create, so the set that would answer for them lives
   there and a value invented here would be a second answer to the peer's own question. */
SandboxFlags window_proxy_creation_sandbox_flags(JSValueConst proxy);

/* THE TOP-LEVEL CREATION URL THE DOCUMENTS OF THIS NAVIGABLE ARE CREATED UNDER. BORROWED — the proxy owns the
   bytes, and like every other string it has ever held they outlive it (see proxy_of), so a parked flow that
   still names an older one resumes onto live memory. NULL only for a REMOTE proxy, whose documents' realms are
   a peer's to build. */
const char *window_proxy_top_level_url(JSValueConst proxy);

/* HTML §8.1.3.1's TOP-LEVEL ORIGIN of the environments of this navigable's documents — the RECORD, because
   that is what every algorithm reading it compares: Permissions §5.1 step 5 generates its permission key from
   "settings's top-level origin" and §3.2 compares two keys with the default permission key comparison
   algorithm, which is "return key1 is same origin with key2" — §7.1.1, whose step 1 is an identity comparison
   a serialization cannot express. BORROWED: an origin lives for the agent (core/url/origin.h), so it sits
   inside the bytes proxy_of captures exactly as `origin` does, and a navigation REPLACES the pointer rather
   than mutating what it points at. Asked only of a LOCAL proxy: a remote navigable's environments are built by
   the instance that holds its documents, and that instance answers §5.1 for them. */
const Origin *window_proxy_top_level_origin(JSValueConst proxy);

/* §7.4's "popup window is requested" for this navigable — §7.2.2.5's BarProps are the negation of it. */
bool window_proxy_is_popup(JSValueConst proxy);

/* §7.2.3's proxy for the realm that is ASKING — `window`, `self`, and the `source` of every message it
   posts. Its realm is this one and is already built, so nothing about it is deferred.
   `name` is the navigable's, and NULL is the host STATING THAT IT DOES NOT KNOW IT — this is the one navigable
   §7.4 did not create, so the browser may have been handed a name by a cross-origin document that set it before
   navigating, and `window.name` then reads as unknown external input. A host that loaded the document itself
   knows the answer is "" and says so.
   §8.1.3.1's TWO TOP-LEVEL FIELDS ARE READ OFF THE REALM THE HOST ALREADY BUILT, not passed in — the URL
   verbatim (realm_top_level_creation_url) and the ORIGIN by running §7.3.1's determine the origin over it with
   THIS AGENT'S origin as the source origin, which is the only source origin an origin-keyed agent cluster has.
   That is the standard's own answer for the two addresses a URL cannot carry an origin for: an `about:blank`
   top-level environment (§7.3.2.1 creates every top-level browsing context at that address) inherits the
   source, and a real address gives §4.7's tuple. */
JSValue window_proxy_new_self(JSContext *ctx, uint32_t doc, const char *name);

/* THE ONE WindowProxy FOR A DOCUMENT — this agent's own when it hosts it, and otherwise a proxy over a
   navigable whose active document lives in ANOTHER WASM instance, minted here on the first ask and answered
   from a table on every ask after it. A remote one carries no Window — there is no local object to hold — so
   every read through it that reaches the active document is a cross-document operation the flow suspends on.
   IT IS ONE DOOR BECAUSE IDENTITY IS ONE FACT. `w[0] === w[0]`, `w.frames[0] === iframe.contentWindow` and
   `event.source === event.source` are page-visible identities over one navigable, and every one of them is
   false the moment two call sites each mint their own proxy for it — which is what the two mint sites this
   replaced did, one per created navigable and one per delivered message. A navigable arriving as an IDENTITY
   from a peer (core/frame/remote_object.c) resolves through this same door, so a name that comes home lands on
   the object this agent already had.
   `name` is the BROWSING CONTEXT's name (the iframe element's `name` attribute, or §7.4's target), NULL for
   none. `parent` is the parent navigable — this instance's own WindowProxy for a child navigable, JS_UNDEFINED
   for a top-level one — and `opener` is §7.2.2.4's, JS_NULL when the navigable was not opened by a script. All
   three describe the navigable at its FIRST ask; a later ask answers with the proxy that already exists,
   because the DOCUMENT is the identity and these are its state. */
JSValue window_proxy_for_document(JSContext *ctx, uint32_t doc, const Origin *origin, const char *name,
                                  JSValueConst parent, JSValueConst opener);

/* THE WindowProxy THIS AGENT ALREADY HOLDS FOR `doc`, or JS_UNDEFINED when it holds none — the half of the
   door above that may not mint, for a caller that has a document NAME and nothing else to build one from (an
   arriving identity's parent and opener slots). Owned on a hit. */
JSValue window_proxy_of_document(JSContext *ctx, uint32_t doc);

/* THE NAVIGABLE A VALUE NAMES, whichever of its two spellings it is: the value itself when it IS a
   WindowProxy, and the asking realm's WindowProxy when it is that realm's Window GLOBAL — win_or_proxy's
   mapping read the other way round, stated here so a caller that must not tell the two apart (an encoder
   handing a navigable to another agent) cannot. JS_UNDEFINED when the value is neither. BORROWED. */
JSValueConst window_proxy_navigable_of(JSContext *ctx, JSValueConst v);

/* §7.2.2.1's IS CLOSING. The proxy a page is holding stays the object it was — the spec files check that it
   does — and reports `closed` from here on. Captured into the RUNNING FLOW's delta, so a sibling arm that
   never closed the window still sees it open. Only ever a TOP-LEVEL traversable: ask
   window_proxy_is_top_level first, which is the early return §7.2.2.1 opens with. */
void window_proxy_set_closing(JSContext *ctx, JSValueConst proxy);
/* §7.2.2.1 step 3's and §7.3's own step 1's TEST — is closing ALONE, which is not what `closed` answers. A
   traversable that is closing has not been destroyed yet (its documents are still unloading), and a navigable
   whose document WAS destroyed was never closing at all, so a close path that asked `closed` would refuse to
   close a frame's container's traversable and would re-enter itself for a destroyed one. */
bool window_proxy_closing(JSValueConst proxy);

/* IS THIS NAVIGABLE A TOP-LEVEL TRAVERSABLE — §7.2.2.1's early return and §7.3's is-closing precondition, one
   fact asked from one place so both spellings of close() make the same test. */
bool window_proxy_is_top_level(JSValueConst proxy);

/* §7.5.10 STEP 8: this navigable's active document was destroyed, so its browsing context is null — the half
   of §7.2.2.1's `closed` that destruction owns, and what §7.5.10 step 5's wait reads off each child navigable.
   Written only by the destroy job (core/frame/document_lifecycle.c). */
void window_proxy_set_destroyed(JSContext *ctx, JSValueConst proxy);
bool window_proxy_destroyed(JSValueConst proxy);

/* THE SUBTREE WAIT — §7.4.2.4's totalTasks, §7.5.9 step 5's numberUnloaded and §7.5.10 step 5's
   numberDestroyed are one mechanism, on the navigable that is waiting and counted DOWN. `_set` records how
   many child navigables this one is waiting on, before any of them is queued; `_report` reports one child's
   completion. Per-flow, so two forked arms tearing down one subtree do not count into each other.
   PER OPERATION, because two of them can be in flight over one tree: a page's `unload` listener may remove an
   `<iframe>`, which starts §7.5.10 over a subtree while §7.5.9 is still counting children in it, and one count
   would let that destroy's report empty the unload's wait. `op` is the operation's index — core/frame/
   document_lifecycle.c names them, and WP_SUBTREE_OP_N is the record's capacity, asserted at every call so a
   fourth operation cannot be added without widening it. */
#define WP_SUBTREE_OP_N 3
/* WHAT A REPORT MEANS, and the three answers are three cases rather than a boolean and a fallback:
   LAST is the wait being over — the only moment the waiting navigable's own body may be queued;
   MORE is a sibling still outstanding;
   NONE is "no navigable above is running this operation", which makes the reporter the ROOT it was started at
   and is where the operation's continuation belongs. A frame removed on its own reports NONE to a parent that
   is not going anywhere; a boolean answer could not tell that from MORE, and the two want opposite things. */
enum { WP_WAIT_NONE = 0, WP_WAIT_MORE, WP_WAIT_LAST };
void window_proxy_child_wait_set(JSContext *ctx, JSValueConst proxy, int op, uint32_t n);
int  window_proxy_child_wait_report(JSContext *ctx, JSValueConst proxy, int op);

/* §7.3.2.2's "browsing context A is FAMILIAR WITH browsing context B", asked as "is the INCUMBENT realm's
   browsing context familiar with the one `proxy` names" — which is the only form §7.2.2.1 step 6 uses, and the
   form in which A is a realm this instance is standing in rather than a second proxy to resolve.
   It is the standard's four-part disjunction over the navigable state this record already holds: same origin;
   A's top-level browsing context IS B; B is auxiliary and A is familiar with B's opener; or an ancestor of B is
   same origin with A. The third is what makes `w = open("https://other/"); w.close()` work — a cross-origin
   popup this document opened is familiar through its opener even though the origins differ. */
bool window_proxy_familiar_with(JSContext *ctx, JSValueConst proxy);

/* §7.2.1's member surface FOR ONE REALM, so a component that owns one of the cross-origin-accessible members
   (postMessage) installs it where every proxy of that realm sees it. OWNED: the caller frees. */
JSValue window_proxy_proto(JSContext *ctx);

/* §7.2.1's members on that prototype, built for ONE realm and declared into core/realm.h's list by
   window_proxy_init. A LOCAL proxy answers every one by reading its own Window in this turn. A REMOTE one
   answers the NAVIGABLE's own state — window/self/frames/globalThis/parent/top/opener/closed/name, plus
   close() — in this turn too, because the navigable belongs to the instance that created it; only a member that
   reads through to the ACTIVE DOCUMENT SUSPENDS the flow on a host request carrying (document, world,
   member). */
void window_proxy_install_proto(JSContext *ctx);

/* §7.2.2.1's `closed` — a fact about the NAVIGABLE, so the Window's getter and the proxy's read the same answer.
   It is the spec's OR: true if the browsing context is null (the destruction ran) or is closing is true.
   Per-flow: captured into the running flow's delta, so a sibling arm that never closed it still sees it open. */
bool window_proxy_closed(JSContext *ctx, JSValueConst proxy);

/* IS THIS ONE OF THE PROXY OBJECTS THIS COMPONENT MINTS? An implementation question, asked of values this
   component holds and about to read ProxyData out of. It is NOT the Web IDL type test — see below. */
bool window_proxy_is(JSValueConst v);

/* THE Web IDL TYPE `WindowProxy`, which is a different question and had only one answer for both. A page's own
   `window` is a WindowProxy — §7.2.2 says `window`, `self` and `frames` all return one — but in this engine
   that object is the realm's GLOBAL, because a navigable's proxy answering for ITSELF would make
   `window === window.parent` false at top level and `frame.contentWindow.parent === window` false (win_or_proxy
   states that mapping once, and this is the same mapping asked as a type test). So the class brand alone
   rejected the one WindowProxy every page actually holds: `new MessageEvent('m', {source: window})` threw a
   TypeError saying `window` was not a WindowProxy. Takes ctx because "is it a window" and "is it THIS realm's
   window" are the same question here. */
bool window_proxy_is_window(JSContext *ctx, JSValueConst v);

/* IS THE NAVIGABLE'S ACTIVE DOCUMENT IN ANOTHER INSTANCE? Asked of the world registry as "does this agent hold
   that document's realm" — never as "is it the document I am", which answers `true` for a same-origin sibling
   sitting in the same runtime. Asserts that the proxy's document and its Window agree, because a proxy where
   they disagree answers a cross-instance read out of the wrong heap. */
bool window_proxy_is_remote(JSValueConst proxy);

/* WHICH DOCUMENT the navigable's active document is — what the host routes a cross-document request by. */
uint32_t window_proxy_doc(JSValueConst proxy);

/* The navigable's CURRENT active Window, as this flow sees it (owned). Crashes for a proxy whose navigable is
   in another WASM instance — that resolve is a host round trip and is not built; see window_proxy.c. */
JSValue window_proxy_window(JSContext *ctx, JSValueConst proxy);

/* THE ACTIVE DOCUMENT'S REALM, MATERIALIZED IF IT IS NOT YET (navigable.h). Only for a navigable this agent
   HOLDS — it crashes for a peer's, which is a suspend and not a realm. A same-origin document is in this heap
   and every read of it is answered in the asking turn, because the spec is synchronous there and a suspend
   would be OBSERVABLE. */
JSContext *window_proxy_realm(JSContext *ctx, JSValueConst proxy);

/* HAS THIS NAVIGABLE'S ACTIVE DOCUMENT BEEN MATERIALIZED YET? Asked by a walk over the navigable TREE, which
   must not build a realm just to look at it: an unmaterialized navigable holds the initial about:blank Document
   §7.4 created it with, and that Document has NO child navigables by construction — so "not materialized" and
   "no children" are the same answer, and materializing every navigable a forced-execution frontier ever created
   in order to ask is the heap exhaustion navigable.c's deferral exists to avoid. */
bool window_proxy_materialized(JSValueConst proxy);

/* §7.2.2.4's `parent`, `top` and `opener` — the NAVIGABLE's, so a Window answers them from the same record its
   own WindowProxy does. One navigable, one answer, whether a page reads `parent` or `otherW.parent`. Owned. */
JSValue window_proxy_parent(JSContext *ctx, JSValueConst proxy);
/* THE NAVIGABLE THIS ONE IS NESTED IN, for an ENGINE walk rather than for `window.parent`: JS_UNDEFINED at the
   top instead of the proxy itself, because a walk up the tree wants "nothing above this". Owned. */
JSValue window_proxy_parent_navigable(JSContext *ctx, JSValueConst proxy);
/* AND §7.2.2.4's `opener` AS THE NAVIGABLE IT IS, for the same reason: `opener` maps this document's own
   navigable onto the GLOBAL, which is the right answer for a page reading the member and the wrong one for an
   engine walk — a Window is not a WindowProxy, so a walk handed it asks "is this a proxy", is told no, and
   silently walks nothing. JS_NULL when the navigable was not opened by a script. Owned. */
JSValue window_proxy_opener_navigable(JSContext *ctx, JSValueConst proxy);
JSValue window_proxy_top_of(JSContext *ctx, JSValueConst proxy);
/* THE TOP-LEVEL TRAVERSABLE'S NAVIGABLE, which is NOT what `top` answers and that difference is load-bearing.
   `window.top` of the asking realm's own navigable is that realm's GLOBAL, because `window === window.top` is
   an identity every page rests on; an ENGINE walk over the navigable tree (HTML §8.1.7.3 step 2's document
   list) wants the WindowProxy in every case including that one. A walk handed the scriptable answer reaches a
   Window, asks "is this a proxy", is told no, and silently walks NOTHING. Owned. */
JSValue window_proxy_top_navigable(JSContext *ctx, JSValueConst proxy);
JSValue window_proxy_opener(JSContext *ctx, JSValueConst proxy);
/* §7.2.2.4's `opener` SETTER, null branch: DISOWN the opener — the link is severed on the NAVIGABLE, so every
   later read of it (this Window's `opener`, the proxy's) answers null and no own property is defined. It is
   the half of that setter that is not Web IDL's replace-with-a-value, and it is a real state change rather
   than a value assignment, which is why it lives on the navigable. Per-flow: captured into the running flow's
   delta, so a sibling arm that did not disown still has its opener. */
void window_proxy_disown_opener(JSContext *ctx, JSValueConst proxy);

/* IS THE NAVIGABLE'S ACTIVE DOCUMENT SAME ORIGIN WITH THIS ONE? §7.2.1's check — §7.1.1's SAME ORIGIN over
   two origin RECORDS (core/url/origin.h), so its step 1 is a nonce comparison and an OPAQUE origin is same
   origin with ITSELF and with nothing else. A `data:` document's `about:blank` child is that case, and §7.3.1
   makes the pair on purpose.
   IT IS NOT THE "IS THIS ORIGIN OPAQUE" QUESTION, and it used to be reached as one: a serialized comparison
   answered false for every opaque origin, so §7.2.6.3's disabled clause, Storage's storage key and the file
   picker's first check all got the answer they wanted from the wrong predicate. They ask origin_is_opaque
   directly now — see each of those files. */
bool window_proxy_same_origin_of(JSValueConst proxy);

/* IS THE NAVIGABLE'S ACTIVE DOCUMENT SAME ORIGIN-DOMAIN WITH THIS ONE? §7.1.1's OTHER algorithm, and §7.3.1's
   `content document` filters `iframe.contentDocument` by THIS one rather than by the check above. They differ
   exactly where `document.domain` has been set, which is now a member (core/dom/document_domain.c).
   IT TAKES A REALM WHERE THE SAME-ORIGIN CHECK DOES NOT, and that asymmetry is the spec's: same ORIGIN may be
   answered against the agent's one record because every origin in this heap is same origin with it, while a
   DOMAIN belongs to a DOCUMENT — so this compares against the ASKING realm's Document's origin. */
bool window_proxy_same_origin_domain_of(JSContext *ctx, JSValueConst proxy);

/* IS THIS ENVIRONMENT'S ORIGIN SAME ORIGIN WITH ITS TOP-LEVEL ORIGIN? — the question HTML §4.10.5.4's
   showPicker() step 2, Permissions §5.1 step 4's default 'self' allowlist and File System Access §2.2's and
   §3.1's SecurityError checks each ask of the CURRENT realm, and which each of them had written out for
   itself: fetch the top-level traversable's navigable and ask §7.2.1 of it. Three copies of one sentence is
   three chances for one case to be read differently.
   A TOP-LEVEL DOCUMENT IS ITS OWN TOP, so the answer is true for one — INCLUDING one whose origin is opaque,
   because §7.1.1 step 1 says an opaque origin is same origin with itself. A caller that also wants the opaque
   case refused asks for it, which is what File System Access §3.1's verify does: its steps 1 and 2 are two
   checks, and they were one only while this predicate answered false for an opaque origin. */
bool window_proxy_same_origin_with_top(JSContext *ctx);

/* THE BROWSING CONTEXT'S NAME, as this flow sees it — "" when it has none. §7.3.3's named access on the Window
   matches against it, so the walk that answers `window.myFrameName` needs to read it. BORROWED. */
const char *window_proxy_name(JSValueConst proxy);

/* §7.2.2.1's `name`, READ AND WRITTEN THROUGH THE ONE PLACE IT LIVES. A Window and its WindowProxy are two
   spellings of one navigable, so `window.name` inside a document and `w.name` from its opener are one attribute
   of one record — window.c answers the global's accessor from here rather than from a second source. The value
   is CONCRETE where the navigable's name was stated (§7.4 states it) and CONCOLIC where it was not, which is
   the navigable the instance started in. */
JSValue window_proxy_name_value(JSContext *ctx, JSValueConst proxy);
JSValue window_proxy_name_assign(JSContext *ctx, JSValueConst proxy, JSValueConst v);

/* The active document's ORIGIN RECORD, as this flow sees it — what §7.2.1's same-origin check reads, and
   what a caller asking "is this origin opaque" (Storage's storage key, §7.2.6.3's disabled clause) asks
   origin_is_opaque of. BORROWED: an origin lives for the agent (core/url/origin.h). */
const Origin *window_proxy_origin(JSValueConst proxy);

/* NAVIGATE — REPLACE THE NAVIGABLE'S ACTIVE DOCUMENT while the proxy object stays the same, which is the whole
   reason a WindowProxy exists: a page holding `iframe.contentWindow` across a navigation holds the same object
   and reaches the NEW document through it.
   ALL SEVEN FACTS MOVE AT ONCE — realm, Window, document id, address, top-level creation URL, top-level
   origin, origin — because they are one binding. An earlier attempt replaced the Window and the origin and
   left the REALM behind, so the two halves of one navigable named different documents; that is why they are
   one call and not seven setters. `top_level_url` and `top_level_origin` are the environment the CALLER built
   the new realm under: §7.4.2.2's navigate gives a TOP-LEVEL traversable's new environment `currentURL` and the
   new document's own origin, and leaves a nested navigable's pair where its creation put it ("If navigable is
   not a top-level traversable ... set topLevelCreationURL to parentEnvironment's top-level creation URL and
   topLevelOrigin to parentEnvironment's top-level origin"). The caller is the one that knows which, because it
   is a fact about the operation's target.
   PER FLOW: the whole record is captured into the running flow's delta at the accessor, so a sibling arm that
   never navigated still resolves this proxy to the document it knew, and a parked flow resumes into its own.
   `realm` is BORROWED — the agent owns every realm it built (navigable.c) — and the superseded one is NOT torn
   down here: a flow parked inside it resumes there, which is what makes it a time-travel entity rather than a
   page a browser could throw away. */
void window_proxy_navigate(JSContext *ctx, JSValueConst proxy, JSContext *realm, uint32_t doc,
                           const char *url, const char *top_level_url, const Origin *top_level_origin,
                           const Origin *origin);

/* HAS THIS NAVIGABLE'S ACTIVE DOCUMENT BEEN REPLACED BY A NAVIGATION — HTML §7.4.4 step 4's "document's IS
   INITIAL about:blank", asked of the navigable because that is what can answer it. §7.4 creates every navigable
   with the initial about:blank Document and window_proxy_navigate above is the ONE site that replaces it, so
   `false` here plus an `about:blank` address IS the standard's fact. Testing the address alone is wrong in this
   tree: §7.4 step 14's load navigates to `about:blank` for real, and that document is not the initial one.
   PER FLOW, through the same capture every other read of this record goes through. */
bool window_proxy_ever_navigated(JSValueConst proxy);

/* HTML §7.3's "IS CREATED BY WEB CONTENT" for a top-level traversable — one of the two disjuncts of §7.2.2.1's
   SCRIPT-CLOSABLE, and the one that decides whether `w = open(...); w.close()` closes a window whose session
   history has grown past a single entry. True for every navigable §7.4 created (window_proxy_new), false for
   the one the instance started in (window_proxy_new_self), which the host loaded. */
bool window_proxy_created_by_web_content(JSValueConst proxy);

#endif
