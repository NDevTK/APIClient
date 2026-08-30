/* WindowProxy — HTML §7.2.3 "The WindowProxy exotic object". See window_proxy.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_WINDOW_PROXY_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_WINDOW_PROXY_H
#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "core/frame/opener_policy.h"
/* §7.1.7's POLICY CONTAINER in the form it crosses a seam — a navigable is created with the clone of its
   creator's, so this unit is a direct user of the type. */
#include "core/frame/policy_container.h"
#include "core/frame/sandboxing.h"
#include "core/url/origin.h"

/* The ACCESSOR side of §7.2.1's same-origin check is THE AGENT'S OWN ORIGIN, and it is asked of the one
   place that holds it (origin_agent) rather than passed in: an instance is an origin-keyed agent cluster, so a
   caller-supplied one could only ever agree or be wrong. */
void window_proxy_init(JSContext *ctx);
/* THE AGENT'S HALF UNDONE — a ROW on core/platform.h's third column, which is why it takes the RUNTIME: what
   this gives back is §7.2.3's class, its five pool entries, §7.2.1.3.1's and §7.2.1.3.2's interned names, the
   strings every proxy of this agent recorded and the table of remote navigables, and every one of those is a
   registration in a JSRuntime rather than anything a realm owns. */
void window_proxy_free(JSRuntime *rt);

/* A proxy over a navigable whose active Window is `window` and whose active document's origin is `origin`.
   Both are the binding at this moment; a navigation replaces them, PER FLOW. `doc` names WHICH document that
   Window is — a same-origin child is a second realm in this agent, so "local" is no longer a synonym for "the
   instance root" and the proxy has to say which of this agent's documents it is over. */
/* `url` is the navigable's initial address — what its REALM is built from on the first read that reaches
   through to the active document (see navigable.h). The proxy owns that realm once built; the navigable's own
   members (window/self/frames/parent/top/opener/closed/name) never need it and never build it. */
/* `creator_policy` is HTML §7.1.7's CLONE OF THE CREATING DOCUMENT'S POLICY CONTAINER, serialized — what the
   navigable's INITIAL about:blank Document runs under, since it has no response of its own to carry one. Every
   navigable is created with that document (§7.4 creates it before step 14 navigates anywhere), so every
   navigable is created with this. Taken HERE rather than at materialization, because that Document's realm is
   built lazily and the realm that builds it need not be the one that created the navigable. A LATER document —
   the one step 14's load brings — has a container of its own, and the load carries it (navigable.c).
   IT IS THE CONTAINER AND NOT ITS ITEMS. This used to be two arguments — the CSP list's text and CSP §2.2's
   self-origin — which is a container flattened, and the flattening was the reason §7.1.4's embedder policy
   had nowhere to live: an item added to the container would have arrived at this seam and stopped. One value
   built through one constructor is what makes the next item impossible to drop here. */
/* `top_level_url` is HTML §8.1.3.1's TOP-LEVEL CREATION URL for the environments of this navigable's
   documents, and it rides here for the identical reason `creator_policy` does: it is decided by the OPERATION
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
   when a Document of it is finally built. §7.3.2.1's create-a-new-browsing-context-and-document hands
   exactly this
   set to the initial about:blank Document as its ACTIVE SANDBOXING FLAG SET, and §7.4.5 makes it one half of
   the FINAL SANDBOXING FLAG SET a later navigation's Document gets — so it is read twice, by the two places a
   Document of this navigable comes into existence (window_proxy.c's materialization and navigable.c's load).
   A CREATION FACT, like `is_popup`: no flow can change it, so unlike `url` there is nothing here for the delta
   to capture, and the per-flow answer comes from the INPUTS — an `<iframe sandbox>`'s attribute is a DOM read
   in the creating flow's own delta, and the navigable that read produced belongs to that flow. */
/* `creator_base_url` is HTML §7.4's `creatorBaseURL` — "if creator is non-null, set creatorBaseURL to
   creator's document base URL" — which §7.3.2.1's create-a-new-browsing-context-and-document gives the initial
   `about:blank` Document as its ABOUT BASE URL. NULL is the real answer for a navigable with no creator (the
   root) and for one created with an address (its Document comes from a response). It rides here for the
   reason `creator_policy` does: the initial Document is materialized lazily and by whichever same-origin
   document reads through the navigable first, whose base URL is not the creator's. It is the creator's BASE
   URL and not its address — a creator that ships `<base href>` passes on the base.
   WITHOUT IT a relative URL in a srcless `<iframe>` or a bare `open()` resolves against `about:blank`, whose
   path is opaque, so the URL parser FAILS rather than producing a wrong answer. */
/* `opener_policy` is HTML §7.3.2.1's INHERITED OPENER POLICY for the initial about:blank Document this
   navigable is created with: "if creator's origin is same origin with creator's relevant settings object's
   top-level origin, then set document's opener policy to creator's browsing context's TOP-LEVEL browsing
   context's active document's opener policy". §7.1.3's `unsafe-none` otherwise, which is a real answer and not
   an absence — a creator that is cross-origin with its own top inherits nothing. It is the CREATOR's decision
   and is stated here for the same reason `creator_policy` is: the navigable being created cannot be asked, and by
   the time its Document is materialized the creator's top may have been navigated. */
JSValue window_proxy_new(JSContext *ctx, uint32_t doc, const char *url, const Origin *origin, const char *name,
                         bool is_popup, SandboxFlags creation_sandbox_flags, OpenerPolicyValue opener_policy,
                         SerializedPolicyContainer creator_policy, const char *creator_base_url,
                         const char *top_level_url,
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
   verbatim (realm_top_level_creation_url) and the ORIGIN by running §7.3.2.1 "Creating browsing contexts"'
   determine the origin over it with THIS AGENT'S origin as the source origin, which is the only source
   origin an origin-keyed agent cluster has.
   That is the standard's own answer for the two addresses a URL cannot carry an origin for: an `about:blank`
   top-level environment (§7.3.2.1 creates every top-level browsing context at that address) inherits the
   source, and a real address gives §4.7's tuple. */
/* `opener_policy` is §7.5.1's OPENER POLICY ROW of the Document the host built this realm for — §7.1.3's
   policy obtained from the response that created it (core/frame/navigation_params.h). This is the one
   navigable §7.3.2.1 did not create, so there is nothing to inherit: the host states what the response said,
   exactly as it states the address and the header list it came from. */
/* `parent` is HTML §7.3.1.3 "Child navigables"' PARENT NAVIGABLE, and it is an argument because it is the ONE
   fact about this navigable's place in a tree that the instance holding it cannot see. §7.3.1.3 defines the
   term over the link — a navigable "is a child navigable", "which means that its parent is non-null" — so a
   host that passes JS_UNDEFINED is not leaving a field blank, it is STATING that this navigable is a top-level
   traversable, and every algorithm that asks §7.3.1.3's question will answer accordingly.
   THAT STATEMENT USED TO BE MADE BY THIS FUNCTION AND WAS FALSE FOR THE COMMONEST CASE. A cross-origin child
   navigable is provisioned as a peer INSTANCE (SECURITY.md's origin-keyed agent cluster), whose root proxy is
   this mint — so the frames a security tool cares about most presented as top-level pages in the only heap that
   holds them, and §7.1.4.2 step 1, §7.2.2.4's `parent`/`top` and §7.5.9's subtree walks each returned a
   plausible answer about a page that does not exist. The parent travels on the `navigable.create` notice, as
   the identity core/frame/remote_object.h already crosses a navigable in, and reaches here through the host
   entry (engine/host/main.c).
   IT IS A NAVIGABLE OR NOTHING — a WindowProxy, remote or local, or JS_UNDEFINED. JS_NULL is the OPENER slot's
   absence and not this one's (window_proxy.c keeps the two spellings apart because §7.2.2.4 answers `parent`
   with the navigable itself at the top of a tree and `opener` with null).
   AND `creation_sandbox_flags` IS HTML §7.1.5 "Sandboxing"'s SET FOR THAT SAME NAVIGABLE, AN ARGUMENT FOR
   EXACTLY THE PARENT'S REASON. §7.1.5 computes it from the embedder ELEMENT's iframe sandboxing flag set and
   that element's node document's active set, both of which are in the instance that CREATED the navigable —
   so this instance can no more derive it than it can derive its own parent, and the two travel together on
   the `navigable.create` notice. A ZERO STOOD HERE, correct for a top-level traversable (whose §7.1.5 answer
   is a popup sandboxing flag set nothing filled) and correct for a cross-instance child only for as long as
   core/frame/navigable.c REFUSED to announce a sandboxed one; the announcement carries the set now, so the
   zero is a value the host states like the parent beside it. It is read back by §7.4.2.1's snapshot of this
   navigable's target snapshot params at every navigation this instance's root performs — so a peer that took
   the zero navigated a SANDBOXED frame's own document with the sandbox deleted. */
JSValue window_proxy_new_self(JSContext *ctx, uint32_t doc, const char *name, OpenerPolicyValue opener_policy,
                              JSValueConst parent, SandboxFlags creation_sandbox_flags);

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

/* HTML §7.5.10 "Destroying documents" STEPS 8 AND 9, WHICH ARE ONE WRITE HERE.
 *
 * Step 8 is "Set document's browsing context to null" — the half of §7.2.2.1's `closed` that destruction owns,
 * and what §7.5.10 step 5's wait reads off each child navigable. Step 9 is "Set document's node navigable's
 * active session history entry's document state's document to null", and that is the NAVIGABLE letting go of
 * the Document: §7.3.1 "Navigables" says "a navigable's active document is its active session history entry's
 * document" and §7.4.1.1 "Session history entries" says "to get a session history entry's document, return its
 * document state's document", so step 9 makes this navigable's ACTIVE DOCUMENT null.
 *
 * THAT SECOND HALF IS THE ENGINE'S ONLY RECLAMATION EDGE, which is why the two may not be separate calls. A
 * realm is kept alive by its own function objects and those hang off its Window (core/frame/navigable.h), so
 * the navigable's reference to that Window is the ONE counted reference this engine holds to a child document's
 * entire realm. A destroy that set the flag and kept the Window announced a destruction and reclaimed nothing:
 * the count of live child realms could only ever rise, and a session that visits sites continuously has no
 * other way for one to fall. Written together, no site can perform one and forget the other.
 *
 * IT IS A REFERENCE DROP AND NEVER A FREE, and that distinction is what makes it safe with a flow parked inside
 * the document being destroyed. This write is per-flow like every other read of this record (it goes through
 * proxy_of), so a sibling arm that never removed the frame keeps its own binding and its own Window; and a flow
 * SUSPENDED in the destroyed document holds that realm through its own frames, jobs and delta, none of which
 * this touches. The realm therefore lives exactly as long as some world can still reach it, and dies as the
 * garbage CYCLE navigable.h describes when none can — decided by the collector, which is the only thing that
 * can decide it, rather than by whichever flow happened to run the destroy job. Written only by the destroy job
 * (core/frame/document_lifecycle.c). */
void window_proxy_set_destroyed(JSContext *ctx, JSValueConst proxy);
bool window_proxy_destroyed(JSValueConst proxy);

/* HTML §7.2.2 "The Window object"'s "A WINDOW'S NAVIGABLE" — "the navigable whose active document is the
 * Window's associated Document's, or null if there is no such navigable" — answered as "is it null". §7.2.2.4
 * "Accessing related windows" opens `top`, `parent` and `frameElement` with a test over it, so it is ONE
 * question three members ask and it is asked in ONE place.
 *
 * IT IS A FOURTH FACT AND NOT A SPELLING OF THE THREE ABOVE, AND THE DIFFERENCE IS WHEN IT BECOMES TRUE.
 * §7.5.10's step 9 is one writer, and §7.5.10's descendant form runs in parallel and queues a task per
 * document — so `window_proxy_destroyed` is not true until a job has run. §7.3.1.6's destroy-a-child-navigable
 * step 3 is the other, it is SYNCHRONOUS inside the tree mutation, and it is the one a page reads: §7.2.2.4's
 * own closing example asserts `iframeWindow.top === null` on the line after `element.remove()`. So this is a
 * WALK up §7.3.1.3's container relation rather than a field — removing one `<iframe>` severs one link and
 * leaves every container element below it intact, so a grandchild is detached by an ancestor's sever and by
 * nothing of its own. It stops at a link whose container element is in another instance, because what is above
 * that is the peer's tree. Per-flow, side-effect-free, and it runs no page code. */
bool window_proxy_navigable_null(JSContext *ctx, JSValueConst proxy);

/* "THIS NAVIGABLE'S BROWSING CONTEXT IS NULL" — the fact itself, rather than either of the two writes that
 * make it true, and the reader this record was missing.
 *
 * IT HAS TWO WRITERS AND THIS HEADER ALREADY SAYS SO: §7.5.10's destroy above ("set document's browsing
 * context to null") and §7.1.3.2's opener-policy group swap below, whose own paragraph opens by calling itself
 * "THE OTHER WRITER of §7.2.2.1's this's browsing context is null". What did not exist was one place to ASK,
 * so a caller that needed the fact reached for `window_proxy_destroyed` — which reports one writer — and was
 * silently wrong for the other. That is not hypothetical arithmetic: HTML §7.2.4 opens EVERY Location setter
 * and every Location method with "if this's relevant Document is null, then return", and a Location object's
 * relevant Document is null exactly when this is true, so a swapped-out window would have gone on to NAVIGATE
 * where the spec returns.
 *
 * IT IS NOT `window_proxy_closed`, which is a THIRD question: §7.2.2.1's `closed` is also true while a
 * traversable is CLOSING, and a closing traversable still has its browsing context and its documents. Three
 * facts, three readers; the one that folds two of them is the one that answers a member wrongly.
 *
 * IT HAS A THIRD WRITER, AND IT IS THE ONLY ONE PAGES CAN SEE. Both writers named above are the QUEUED
 * destruction and a navigation's swap; §7.3.1.6's destroy-a-child-navigable step 3 severs the container
 * relation SYNCHRONOUSLY, inside the tree mutation, and every observation of this fact in the corpus is made
 * on the line after `iframe.remove()`. `embedded-opener-remove-frame.html` states the difference in its own
 * structure — a removed FRAME's `opener` is checked immediately and a closed POPUP's behind a `step_timeout`.
 * So this takes `ctx` and asks window_proxy_navigable_null for that third disjunct rather than testing a byte,
 * and §7.2.2.1's `closed`, §7.2.2.4's `opener` and §7.2.4's relevant Document all reach it through here.
 * IT IS STILL NOT `window_proxy_destroyed`, which reports one writer of three. */
bool window_proxy_browsing_context_null(JSContext *ctx, JSValueConst proxy);

/* HTML §7.1.3.2 "Browsing context group switches due to opener policy", step 10 — THE OTHER WRITER of
 * §7.2.2.1's "this's browsing context is null", and the one that is not a destruction.
 *
 * Its note is the whole of it: "In this case we are going to perform a browsing context group swap.
 * browsingContext will not be used by the new Document that we are about to create." The navigation goes on to
 * build its Document in a NEW top-level browsing context, in a new group — which SECURITY.md's
 * `(browsing context group, origin)` key makes another WASM instance — and this record is what the page's own
 * handle on the window keeps pointing at. browsing-the-web's MAKE ACTIVE ("Set document's browsing context's
 * WindowProxy's [[Window]] internal slot value to window") goes through the DOCUMENT's browsing context, so on
 * an ordinary navigation the handle follows the navigable and on a swap it does not: it keeps the Window it
 * had, and `closed` is true about it from here on. That is the observable the opener sees, and severing the
 * opener link instead would leave it reading `closed === false` on a window real Chrome has already cut off.
 *
 * IT DESTROYS NOTHING, which is why it is not `window_proxy_set_destroyed`. The Document, its Window and its
 * realm are exactly where they were — a read through the handle still answers about THAT document — and
 * `window_proxy_destroyed` still answers false, so §7.5.10 may later run over this navigable in full and
 * reclaim it. Written by core/frame/browsing_context_group.c, which is where the swap is. */
void window_proxy_discard_browsing_context(JSContext *ctx, JSValueConst proxy);

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
   member).
   THE SURFACE IS NOT ONLY THIS COMPONENT'S. §7.2.1.3.1 CrossOriginProperties' thirteen names must ALL be own
   properties of it (a listed name the surface does not own is answered `undefined`, which §7.2.1.3.2's last
   step rules out), and three of them belong to other components: `postMessage` is window_message.c's and
   §6.6.6's `focus`/`blur` are core/html/focus.c's. Each is installed by its OWNER onto this one object — never
   restated here — and proxy_get_own's steps 4-6 assert per name that the object ended up with all thirteen. */
void window_proxy_install_proto(JSContext *ctx);

/* WEB IDL §3.7.6 "Attributes"' `jsValue` FOR A MEMBER WHOSE ANSWER IS A NAVIGABLE — "the this value, if it is
   not null or undefined, or realm's global object otherwise", mapped onto the navigable it names. §3.7.7
   Operations carries the identical step, so `close()` uses it too.
   IT IS THE RECEIVER'S, NEVER THE REALM'S. A member of a [Global] interface installed once and answered out of
   `document_window_proxy(ctx)` answers for whichever navigable its realm has, whoever asks — and HTML §7.2.3.5
   step 3 hands a same-origin WindowProxy the OTHER document's getter with the proxy as receiver, so that is not
   a corner case but the ordinary cross-document read. The realm is the fallback for a MISSING receiver only.
   BORROWED, or JS_UNINITIALIZED with §3.7.6's TypeError pending — a receiver that does not implement Window is
   a throw and not an answer about some other window. */
JSValueConst window_proxy_this_navigable(JSContext *ctx, JSValueConst this_val);

/* THE SAME §3.7.6 SENTENCE, ANSWERED AS THE OBJECT RATHER THAN AS THE NAVIGABLE — the platform object the
   member was invoked on, which is what §7.2.2.4's opener setter's "DefinePropertyOrThrow(this, "opener", …)"
   and [Replaceable]'s CreateDataPropertyOrThrow write to. The two answers DIFFER exactly where the receiver is
   missing: the navigable is this realm's WindowProxy and the object is its GLOBAL, and a define aimed at the
   proxy lands where §7.2.3.5 answers out of §7.2.3's own surface and the page can never read it back.
   OWNED. Both are derived from ONE test of "null or undefined", stated in window_proxy.c. */
JSValue window_proxy_this_object(JSContext *ctx, JSValueConst this_val);

/* §3.7.6's OTHER SENTENCE — "If jsValue does not implement target, then ... throw a TypeError" — for
   target = Window, and answered WITHOUT a navigable lookup. window_proxy_this_navigable is the wrong function
   to reach for when all a member needs is the OBJECT: it DCHECKs on a foreign realm's Window because it cannot
   name that window's navigable, and [Replaceable]'s CreateDataPropertyOrThrow never asks which window it is.
   The two objects that implement Window are a Window (any realm's — the global carries the class) and a
   WindowProxy, which §7.2.3.5 step 3 hands the accessor as its receiver. Side-effect-free. */
bool window_proxy_implements_window(JSValueConst js_value);

/* AND WHETHER THAT RECEIVER IS THE ONE THE MEMBER'S OWN REALM ANSWERS FOR. An attribute whose value the realm
   ALREADY HOLDS is correct exactly while §3.7.6's idlObject is this realm's Window — normally true, because
   each realm installs its own getter over its own value and js_call_c_function sets ctx to the member's realm.
   A receiver lifted from another navigable onto this realm's accessor by hand is where the held value becomes
   this realm's answer to a question about a different window, and that is a DCHECK rather than a return.
   Side-effect-free. */
bool window_proxy_receiver_is_own_realm(JSContext *ctx, JSValueConst js_value);

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
/* HTML §7.3.1.3 "Child navigables"' CONTAINER OF A NAVIGABLE — "the navigable container whose content
   navigable is navigable, or null if there is no such element". The container ELEMENT's wrapper (owned), or
   JS_NULL for a navigable nested through nothing (a top-level traversable, and every auxiliary one §7.3.1.7
   step 8 creates).
   IT IS RECORDED AT THE CREATE AND CONFIRMED AGAINST THE FORWARD EDGE, and both halves of that are what make
   it §7.3.1.3's relation rather than a second copy of it. RECORDED, because create-a-new-child-navigable is
   handed the element ("To create a new child navigable, given an element element") and sets the link as one of
   its own steps — so the reverse edge is written there, by the operation that has the element in hand, and is
   never searched for down a document tree afterwards. CONFIRMED, because §7.3.1.6's destroy-a-child-navigable
   severs the relation by clearing the ELEMENT's content navigable, which in this engine is a per-flow write on
   the wrapper (core/html/html_iframe.c) — so a stored pointer READ ALONE would keep naming an element the
   running flow has already detached, and would keep naming it in a sibling arm that detached it while this one
   did not. Reading the forward slot back IS the definition above, it costs one own-property read, and it is
   the only spelling in which the answer is per-flow without a second write to capture. */
JSValue window_proxy_container(JSContext *ctx, JSValueConst proxy);
/* §7.3.1.3's create-a-new-child-navigable step "Set element's content navigable to navigable", from the
   NAVIGABLE's side — the half html_iframe.c's slot on the wrapper cannot hold. ONE WRITER: §7.4's create
   (core/frame/navigable.c), which is the one algorithm that is handed a container element, and which asserts
   the pairing between having one and being a child navigable so a child cannot be created without it. */
void window_proxy_set_container(JSContext *ctx, JSValueConst proxy, JSValueConst element);

/* THE SAME LINK WHEN THE ELEMENT IS IN ANOTHER WASM INSTANCE, and it carries an ANSWER rather than an object.
 * SECURITY.md keys an instance on `(browsing context group, origin)`, so the navigable an instance is ROOTED
 * in is routinely a cross-origin `<iframe>` whose element belongs to the CREATING instance's tree — a member
 * whose value is an OBJECT does not cross an instance boundary at all until the remote-object handle exists,
 * and answering one by serializing it is how a cross-document read starts returning something that is not the
 * thing. What crosses instead is what that container ANSWERED: Permissions Policy §9.5's result for this
 * navigable, in the text core/permissions_policy/permissions_policy.h serializes an inherited policy into, or
 * that grammar's PERMISSIONS_POLICY_SERIALIZED_NO_CONTAINER.
 *
 * WHY THE ANSWER AND NOT THE ELEMENT'S FACTS: §9.5's two arguments are the container element and the origin of
 * the Document being created, and both of them belong to the creator — so §9.5 runs ONCE, where they are. A
 * peer re-running §9.7 would be a second site evaluating one algorithm, and it would need the container
 * document's whole permissions policy (its §9.8 steps read the DECLARED allowlists, at two origins), its
 * origin, and the `allow` attribute's bytes. A LIVE read is not the alternative either: §Security makes a
 * cross-instance read a suspend point, and a Document's install has no flow under it to suspend.
 *
 * STATED THROUGH core/frame/navigable.h's navigable_root_container, which is where the pairing with §7.3.1.3's
 * PARENT is asserted. NULL from the reader is "nobody stated it", which is not an answer: core/dom/document.c
 * crashes on it rather than reading it as §9.7 step 1's null container. BORROWED. */
void window_proxy_set_remote_container(JSValueConst proxy, const char *serialized_policy);
const char *window_proxy_remote_container(JSValueConst proxy);

/* AND HTML §3.1.3 "Ancestor origins"' INTERNAL ANCESTOR ORIGIN OBJECTS LIST FOR THAT SAME NAVIGABLE, when its
 * ancestors are in another instance — the sibling of the pair above and not a second spelling of it.
 *
 * IT IS A THIRD CROSS-INSTANCE ANSWER RATHER THAN A DERIVATION OF THE FIRST TWO. §3.1.3 takes a Document and a
 * referrer policy and reads three things this instance does not hold: the PARENT DOCUMENT's own internal list
 * (its step 5), that document's ORIGIN RECORD (its steps 9 and 10), and the CONTAINER ELEMENT (its step 6).
 * The parent identity on the provisioning record is not a substitute for any of them — it names a navigable,
 * and §3.1.3 reads a Document's recorded ancestry, which no identity carries.
 *
 * AND THE RECORD'S ORIGINS ARE NOT ENOUGH TO RUN THE STEPS AT THIS END, which is the sharp half. Step 10 asks
 * whether an ancestor "is same origin with parentDoc's origin"; §7.1.1 decides an opaque origin by IDENTITY
 * and every opaque origin serializes to the same three bytes `null`, so a peer comparing text would mask an
 * entry that is not the parent's the moment the parent is itself opaque — which is not an exotic case but the
 * `data:`-iframe one, where the child is in a peer instance precisely BECAUSE its origin is opaque. So the
 * creator runs §3.1.3 once, with all three inputs in one heap, and the finished list crosses as text.
 *
 * STATED THROUGH core/frame/navigable.h's navigable_root_ancestor_origins, which is where the pairing with
 * §7.3.1.3's PARENT is asserted: a navigable with a remote parent is a child navigable and its list is
 * non-empty; one with no parent is a top-level traversable and its list is empty. NULL from the reader is
 * "nobody stated it", which is not an answer — core/dom/document.c crashes on it rather than installing an
 * empty list, which would make a cross-origin frame report itself top-level. BORROWED. */
void window_proxy_set_remote_ancestor_origins(JSValueConst proxy, const char *serialized_list);
const char *window_proxy_remote_ancestor_origins(JSValueConst proxy);

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
/* HTML §7.2.2.1 "Opening and closing windows", the window open steps, step 16.2 — "if noopener is false, then
 * set targetNavigable's active browsing context's OPENER BROWSING CONTEXT to sourceDocument's browsing
 * context". THE OTHER WRITER of the same link `window_proxy_new` sets at creation, and the one an EXISTING
 * navigable needs.
 *
 * WHY IT DID NOT EXIST AND WHAT ITS ABSENCE LOOKED LIKE. Every opener this engine had was set at CREATION, by
 * the create that mints an auxiliary navigable — which is right for `open(url)` and answers nothing at all for
 * `open(url, name)` that finds a navigable ALREADY THERE. §7.3.1.7's rules answer that call with an existing
 * navigable and windowType "existing or none", and step 16.2 is the only step that links it back; with no
 * setter there was no step, so `window.open("/x", "<an existing iframe's name>")` navigated the frame and left
 * its `opener` null for ever. `embedded-opener-remove-frame.html` fails on that line — its FIRST assert,
 * before any of the removal behaviour it is named for — which is why the file reads as a detach test and is
 * really an opener test that never gets past its own setup.
 *
 * IT IS A STATE CHANGE ON THE NAVIGABLE, not a value assignment, which is the same statement `disown` above
 * makes and the reason the pair lives here rather than in §7.2.2.4's Web IDL setter: that setter's non-null
 * branch is [Replaceable]'s DEFINE (it shadows the accessor with an own data property on the receiver and
 * changes no browsing context at all), while this one is what a page then READS through it.
 * `opener` is BORROWED — this dups it. Per-flow: captured into the running flow's delta through the accessor,
 * so an arm that opened a named window links an opener its sibling does not have. */
void window_proxy_set_opener(JSContext *ctx, JSValueConst proxy, JSValueConst opener);

/* IS THE NAVIGABLE'S ACTIVE DOCUMENT SAME ORIGIN WITH THIS ONE? §7.2.1's check — §7.1.1's SAME ORIGIN over
   two origin RECORDS (core/url/origin.h), so its step 1 is a nonce comparison and an OPAQUE origin is same
   origin with ITSELF and with nothing else. A `data:` document's `about:blank` child is that case, and
   §7.3.2.1's determine the origin makes the pair on purpose.
   IT IS NOT THE "IS THIS ORIGIN OPAQUE" QUESTION, and it used to be reached as one: a serialized comparison
   answered false for every opaque origin, so §7.2.6.3's disabled clause, Storage's storage key and the file
   picker's first check all got the answer they wanted from the wrong predicate. They ask origin_is_opaque
   directly now — see each of those files. */
bool window_proxy_same_origin_of(JSValueConst proxy);

/* IS THE NAVIGABLE'S ACTIVE DOCUMENT SAME ORIGIN-DOMAIN WITH THIS ONE? §7.1.1's OTHER algorithm, and
   §7.3.1.3 "Child navigables"' `content document` filters `iframe.contentDocument` by THIS one rather than
   by the check above. They differ exactly where `document.domain` has been set, which is now a member
   (core/dom/document_domain.c).
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
/* `opener_policy` is §7.5.1's OPENER POLICY ROW of the Document this navigation creates — "opener policy …
   navigationParams's cross-origin opener policy", obtained from the response the load fetched. It is the
   OPERATION's like every other argument here and is never read back off the navigable, which at this moment
   still holds the document being replaced. */
void window_proxy_navigate(JSContext *ctx, JSValueConst proxy, JSContext *realm, uint32_t doc,
                           const char *url, const char *top_level_url, const Origin *top_level_origin,
                           const Origin *origin, OpenerPolicyValue opener_policy);

/* HTML §7.5.1's OPENER POLICY ROW of this navigable's ACTIVE DOCUMENT — the value §7.1.3.2's group-switch
   check and §7.3.2.1's creation inheritance both read, and the reason the row is held on the navigable rather
   than on a Document object is in window_proxy.c beside the field: both readers walk from a browsing context
   to whichever Document is active in it, and a navigable holding the initial about:blank has no realm to hold
   one until something touches it. PER FLOW. Asked of a REMOTE proxy this CRASHES rather than answering
   `unsafe-none`, which is a legal value and would be indistinguishable from an answer. */
OpenerPolicyValue window_proxy_opener_policy(JSValueConst proxy);

/* THE NEGATION OF HTML §3.1.1 "The Document object"'s "is initial about:blank", asked of the navigable because
   a navigable still showing the Document §7.4 created it with has no realm to hold a Document-side field. §7.4
   creates every navigable with that Document, so `false` here plus an `about:blank` address IS the standard's
   fact. Testing the address alone is wrong in this tree: §7.4 step 14's load navigates to `about:blank` for
   real, and that document is not the initial one.
   THE NAME IS THE FIRST WRITER'S AND THE FLAG HAS TWO — window_proxy_navigate above, which replaces the active
   document, and window_proxy_clear_initial_about_blank below, which replaces no document at all. Read this as
   the standard's boolean, never as "was this navigable navigated", or the second writer reads as a lie.
   PER FLOW, through the same capture every other read of this record goes through. */
bool window_proxy_ever_navigated(JSValueConst proxy);

/* HTML §8.4.1 "Opening the input stream" step 13's "Set document's is initial about:blank to false" — the ONE
   byte, and never routed through window_proxy_navigate: §8.4.1 replaces a Document's CONTENT while keeping the
   same Document object, so every other row of the binding a navigation moves is unchanged here. PER FLOW, so
   an arm whose script opened the document and a sibling standing on the unwritten page get different answers
   out of §7.4.4 "Non-fragment synchronous \"navigations\"" step 4. Called for a navigable a PEER holds this
   CRASHES: the flag is a fact about a Document in another agent's heap, and clearing it is a routed operation
   rather than a local write. */
void window_proxy_clear_initial_about_blank(JSValueConst proxy);

/* HTML §7.3's "IS CREATED BY WEB CONTENT" for a top-level traversable — one of the two disjuncts of §7.2.2.1's
   SCRIPT-CLOSABLE, and the one that decides whether `w = open(...); w.close()` closes a window whose session
   history has grown past a single entry. True for every navigable §7.4 created (window_proxy_new), false for
   the one the instance started in (window_proxy_new_self), which the host loaded. */
bool window_proxy_created_by_web_content(JSValueConst proxy);

#endif
