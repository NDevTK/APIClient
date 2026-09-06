/* WindowProxy — HTML §7.2.3 "The WindowProxy exotic object", and the reason it is not a formality here.
 *
 * WHAT IT IS. `window`, `self`, `parent`, `top`, `frames[i]`, `opener` and `iframe.contentWindow` are not
 * Window objects. Every one of them is a WindowProxy: an exotic object that FORWARDS to the current active
 * Window of a NAVIGABLE. The distinction exists because a navigable outlives the documents in it — navigate an
 * iframe and its Window is replaced, while the WindowProxy a page is holding stays the same object. HTML says
 * so in one line, and everything else in this file follows from it.
 *
 * WHY IT IS NOT A FORMALITY IN THIS ENGINE. With one navigable and no navigation, a WindowProxy that forwards
 * to the one global is observationally identical to that global, and I had it written down as exactly that —
 * a piece of machinery to build when there was a second window to point at. That is wrong, and TIME TRAVEL is
 * what makes it wrong:
 *
 *   - THE BINDING IS PER-FLOW. One forked arm navigates the frame and its `contentWindow` targets the new
 *     Window; its sibling never navigated and still targets the old one. Both hold the SAME proxy object,
 *     because that is what a proxy is for. So proxy -> Window is per-flow state, and it has to ride the COW
 *     delta like every other piece of shared state a flow writes — which is what cow_capture_host_record is,
 *     and which a global "the window is the global" cannot express at all.
 *   - SO IS THE SAME-ORIGIN CHECK. §7.2.3's [[Get]] is filtered by whether the ACCESSOR and the target's
 *     active document are same origin, and the target's origin is whatever the flow navigated it to. A check
 *     against a fixed origin answers for a world the flow is not in.
 *   - AND A FLOW HOLDING ONE MUST BE SUSPENDABLE. A parked flow resumes with its delta, so the proxy it holds
 *     must resolve through that delta rather than through whatever the engine last did. A binding kept
 *     anywhere else is read at resume time from the wrong world.
 *
 * CROSS-INSTANCE. One WASM instance is one document, so a proxy may name a Window in another instance. Then the
 * binding is not a JSValue at all — it is (document, world), and the world is the sending flow's decision
 * vector, because two arms of a fork that navigated a frame differently must not resolve to one Window.
 *
 * AND THE NAVIGABLE IS STILL THIS INSTANCE'S. A remote proxy is not a proxy this engine knows nothing about:
 * this engine CREATED that navigable, named it, nested it and can destroy it. What lives in the peer is the
 * ACTIVE DOCUMENT. The member table below is organised by exactly that line, and it is the difference between
 * `otherW.self` costing nothing and `otherW.self` parking a flow on a host that may never answer. */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/frame/window_proxy.h"
#include "solver/cow.h"
#include "solver/concolic.h"
#include "solver/world.h"
#include "solver/engine.h"
#include "solver/flow.h"
#include "core/idl_args.h"
#include "core/realm.h"
/* §6.6.6's two Window members: §7.2.1.3.1 puts `focus` and `blur` on the cross-origin list, so §7.2.3's own
   surface owns them too and takes them from the component that owns §6.6.4's steps. */
#include "core/html/focus.h"
#include "core/html/html_iframe.h"
#include "core/frame/navigable.h"
#include "core/frame/window.h"
#include "core/frame/location.h"
#include "core/frame/remote_location.h"   /* §7.2.4's cross-origin arms — `location` of a PEER's document */
#include "core/frame/document_lifecycle.h"
#include "core/dom/document.h"
#include <stdio.h>

/* §7.2.3's [[Window]] and the origin its same-origin check reads. BOTH are per-flow — see the file comment —
   which is the whole reason this is a record behind a class rather than a property on an object. */
typedef struct {
    JSValue window;    /* the navigable's active Window, or JS_UNDEFINED when the document is remote (owned) */
    /* THE ACTIVE DOCUMENT'S REALM, when this agent holds it — NULL when the document is a peer's. It is what a
       member that reads THROUGH to the active document is answered from: `length` counts the child navigables
       of THAT document, and asking it of the READING realm counts the wrong document's frames.
       BORROWED, AND WHAT IT IS BORROWED FROM IS THE FIELD ABOVE IT. This said "the AGENT owns every realm it
       built and releases them with itself (navigable.c)", and navigable.c says the OPPOSITE in as many words —
       "THE AGENT MUST NOT OWN ONE, and that is the whole mechanism rather than a preference", because an
       agent-held reference is an EXTERNAL ROOT that would make the realm, and therefore the very WindowProxy
       whose collection says the navigable is gone, permanently reachable. Its list is borrowed pointers,
       nav_create_finish drops the builder's own reference (JS_FreeContext) the moment the navigable has the
       realm, and navigable_free frees the list while explicitly releasing no realm. So NOTHING holds a counted
       reference to a child realm by name: what keeps it alive is `window` — the Window's C function objects
       each hold the realm that defined them (js_call_c_function reads `p->u.cfunc.realm`) — and this pointer
       is a raw borrow that is only ever valid while that field is set. The two are written and cleared
       together for that reason, and HTML §7.5.10 "Destroying documents" step 9 clearing them is the whole of
       this engine's reclamation edge (window_proxy_set_destroyed). A stale claim about WHO OWNS A REALM is not
       a wording detail here: a reader chasing a leak would go looking for the agent's release, find none, and
       conclude the reclamation is unbuilt.
       A proxy is a GC object and a realm is not, so an owning reference here would have to be released from a
       finalizer — which frees JSValues during collection — and the realm's own Document teardown would still
       never run.
       NULL UNTIL THE DOCUMENT IS MATERIALIZED — see proxy_realm and navigable.h. `url` is what it is built
       from; a navigable that has been NAVIGATED is materialized at creation, one that still holds its initial
       about:blank Document only when something reads through it. */
    JSContext *realm;
    char      *url;    /* the navigable's address, for a realm not yet materialized (owned) */
    /* THE ACTIVE DOCUMENT'S ORIGIN — §7.1.1's RECORD, for §7.2.1's same-origin check. BORROWED: an origin is
       immutable and lives for the agent (core/url/origin.h), which is what lets it sit inside the bytes
       proxy_of captures — a navigation REPLACES this pointer rather than mutating what it points at, so a
       parked flow's saved copy still names the origin its world had. It is a RECORD and not a serialization
       because §7.1.1's step 1 compares IDENTITY: every opaque origin serializes to "null", so a string could
       not tell one opaque origin read twice from two distinct ones, and this component guessed "two" — which
       made a `data:` document's own about:blank child cross-origin to it. */
    const Origin *origin;
    /* THE NAVIGABLE'S OWN STATE — see the member table below for why it is here and not in the peer. All four
       are per-flow: a flow that closed the window or renamed it is the only one whose timeline contains that,
       and `parent`/`opener` are values a fork must carry. */
    JSValue parent;    /* the parent navigable's proxy (or the creator's Window); JS_UNDEFINED = top-level */
    JSValue opener;    /* §7.2.2.4's opener — the navigable that opened this one, or JS_NULL */
    /* HTML §7.3.1.3's CONTAINER — the navigable container ELEMENT this navigable is presented by, as its
       wrapper, or JS_NULL for one nested through nothing. §7.3.1.3 defines it as a DERIVED relation ("the
       navigable container whose content navigable is navigable"), which a browser answers with a stored back
       pointer and this engine could otherwise only answer by walking a document tree looking for the element
       whose slot names this proxy — a search whose cost is the tree and whose answer is already known at the
       one place that can state it: create-a-new-child-navigable is GIVEN the element.
       IT IS A STRONG REFERENCE AND IT CLOSES A CYCLE, deliberately: the element's wrapper holds this proxy in
       its own hidden slot, so the two name each other and neither can be collected while the other is reachable
       from a root. That is what proxy_gc_mark is for, and the pair is no different in kind from `window`, which
       holds the proxy back through the realm's global.
       THE READER CONFIRMS IT AGAINST THAT FORWARD SLOT (window_proxy_container) rather than trusting it, which
       is what makes this per-flow with nothing to capture: §7.3.1.6's destroy-a-child-navigable severs the
       relation by clearing the ELEMENT's content navigable, and that clear is an ordinary property write the
       DOM COW delta already isolates. */
    JSValue container;
    /* WHAT THAT CONTAINER ANSWERED, FOR THE ONE NAVIGABLE WHOSE CONTAINER IS IN ANOTHER HEAP — Permissions
       Policy §9.5's result for this navigable, as the text core/permissions_policy/permissions_policy.h
       serializes an inherited policy into, or that grammar's "there is no container".
       IT IS NOT THE SLOT ABOVE SAID TWICE. That one is an ELEMENT, and an element is exactly what this
       instance does not have: SECURITY.md keys an instance on `(browsing context group, origin)`, so the
       navigable an instance is ROOTED in may be a cross-origin `<iframe>` whose element lives in the creating
       instance's tree. §9.5's two arguments are that element and this Document's origin, both of them the
       CREATOR's, so the creator runs §9.5 and its ANSWER travels; nothing here could compute it, and a
       cross-instance read is a SUSPEND POINT that a Document's install has no flow under it to take.
       NULL IS "NOBODY STATED IT" AND IS NOT AN ANSWER. A navigable whose parent is a remote proxy is a child
       navigable by §7.3.1.3 and therefore HAS a container, so the absence is a host that owes a statement —
       core/dom/document.c crashes on it rather than reading it as §9.7 step 1's null container, which would
       return `Enabled` for every supported feature.
       POD AND OWNED BY THE COMPONENT, exactly like `top_level_url`: proxy_strdup'd, inside the bytes proxy_of
       captures, never freed on a navigation because a parked flow's saved bytes still name it. */
    char   *remote_container;
    /* AND WHAT §3.1.3's STEPS ANSWERED FOR THAT SAME NAVIGABLE, FOR THAT SAME NAVIGABLE ONLY — HTML §3.1.3
       "Ancestor origins"' INTERNAL ANCESTOR ORIGIN OBJECTS LIST, as core/dom/document.h serializes one.
       IT IS THE SLOT ABOVE'S SIBLING AND NOT ITS DUPLICATE: both are facts the creating instance computed
       because both of §3.1.3's and §9.5's inputs live in its heap, and both are results rather than inputs for
       the same reason — a peer given the inputs would be a second site running one algorithm, and here it
       would run it WRONG. §3.1.3's step 10 compares an ancestor against "parentDoc's origin" and §7.1.1's same
       origin decides an opaque origin by IDENTITY, which a serialization does not carry; every opaque origin
       is the same three bytes `null`. Steps 6-8 read the CONTAINER ELEMENT, which is in the creator's tree and
       does not cross at all.
       NULL IS "NOBODY STATED IT" AND IS NOT AN ANSWER, exactly as above. A navigable whose parent is a remote
       proxy is a child navigable by §7.3.1.3, so its Document HAS ancestors; core/dom/document.c crashes on the
       absence rather than reading it as the empty list, which would tell a cross-origin frame it is the top of
       its own tree — a fact no page can distinguish from the truth and one nothing else would report.
       POD AND OWNED BY THE COMPONENT, like every other string here: proxy_strdup'd into the never-released
       list, inside the bytes proxy_of captures, so a parked flow's saved bytes still name live memory. It is
       NOT on PROXY_VALS, which names only this record's JSValues. */
    char   *remote_ancestor_origins;
    char   *name;      /* §7.2.2.1's name: the BROWSING CONTEXT's, not the element's (owned; see proxy_of) */
    /* WHETHER ANYONE STATED THIS NAVIGABLE'S NAME, which is what decides whether `name` is a computed value or
       unknown external input. §7.4 STATES it — `open(url, "chan42")` names the navigable it creates, and the
       popup's own `window.name` is that string — so a navigable this engine created knows its name, "" or
       otherwise. The navigable the instance STARTS in is the one nobody here created: a real page may have been
       opened by a cross-origin document that set `name` before navigating, which is exactly how window.name
       became a classic attacker source, so a host that cannot state it leaves it unknown and the read is
       concolic. A host that CAN state it (a runner that loaded the document itself) says so and gets the
       spec's "". The distinction is DATA the creator supplies, never inferred from which mint was used. */
    uint8_t name_known;
    /* §7.4's "popup window is requested", decided from the `features` argument at creation and then a FACT
       ABOUT THE NAVIGABLE for as long as it exists — which is what every BarProp's `visible` is the negation
       of. It is not per-flow: no flow can turn a tab into a popup, so unlike `closed` and `name` there is
       nothing here for the delta to capture. */
    uint8_t is_popup;
    /* HTML §7.1.7's POLICY CONTAINER THE CREATOR HANDED THIS NAVIGABLE, taken at CREATION and kept in the form
       it crosses a seam in — which is what §7.1.7's "clone a policy container" is here, since
       policy_container_clone re-parses these same bytes.
       It is here rather than read off whichever realm materializes the document because a navigable created
       with no address is materialized LAZILY, on the first read that reaches through it — and the reader may
       be a different same-origin document with a different container, which would give the child whichever
       document happened to touch it first. A creation fact, so like `is_popup` no flow can change it.
       ONE VALUE, NOT ONE FIELD PER ITEM. It was the CSP list's two halves as two `char *`, which is a container
       flattened, and an item added to the container reached this struct and stopped with nothing to say so.
       §7.1.4's embedder policy is the item that proved it: it landed on the container and the compiler stopped
       HERE, at a mint that had to state where its answer comes from. Every string the value names is
       proxy_strdup'd and outlives every flow; a parked flow resuming onto its saved copy of this POD struct
       resumes onto live memory.
       serialized_policy_container_none for a navigable with an address (its container comes with its response)
       and for the root. */
    SerializedPolicyContainer creator_policy;
    /* HTML §7.3.2.1 "Creating browsing contexts"' `creatorBaseURL` — "Set creatorBaseURL to creator's
       document base URL", which the create-a-new-browsing-context-and-document steps hand the initial
       `about:blank` as its
       ABOUT BASE URL. Without it a relative URL inside a srcless frame or a bare `open()` resolves against
       `about:blank`, which has an opaque path and cannot be a base, so the parse FAILS outright.
       IT IS TAKEN AT CREATION, for exactly the reason `creator_policy` above is: the Document is materialized
       lazily, by whichever same-origin document reads through this navigable first, and that reader's base
       URL is not the creator's. It is also the CREATOR's base URL and not the creator's ADDRESS — a creator
       carrying `<base href>` passes on the base, which is the whole content of the word in §7.3.2.1.
       NULL for the root navigable (nothing created it) and for a navigable created with a real address (its
       Document comes from a response and §2.4.3's about base URL is null for it). Owned. */
    char   *creator_base_url;
    /* §7.1.5's DETERMINE THE CREATION SANDBOXING FLAGS for this navigable, taken at CREATION for exactly the
       reason `creator_policy` above is: §7.3.2.1 hands this set to the initial about:blank Document as
       its ACTIVE
       SANDBOXING FLAG SET, that Document's realm is materialized LAZILY, and the realm that materializes it
       need not be its creator's. A creation fact like `is_popup` — no flow can re-sandbox a navigable that
       exists — and one WORD, so the byte capture in proxy_of describes it completely. */
    SandboxFlags creation_sandbox_flags;
    /* HTML §7.5.1's OPENER POLICY ROW OF THIS NAVIGABLE'S ACTIVE DOCUMENT — "opener policy … navigationParams's
     * cross-origin opener policy" in create-and-initialize-a-Document's own creation table, beside "policy
     * container" and "active sandboxing flag set", which are three separate items of one creation (§7.1.7's
     * container has no such field — see core/frame/opener_policy.h).
     *
     * IT IS ON THE NAVIGABLE BECAUSE ITS TWO READERS BOTH ASK A NAVIGABLE. §7.1.3.2's obtain-a-browsing-
     * context-to-use-for-a-navigation-response reads "browsingContext's active document's origin" and its
     * opener policy to decide a group switch, and §7.3.2.1's create reads "creator's browsing context's
     * top-level browsing context's ACTIVE DOCUMENT's opener policy". Neither ever holds a Document; both walk
     * from a browsing context to whichever Document is active in it. And a navigable §7.4 created with no
     * address holds the initial about:blank Document whose realm is materialized LAZILY, so a row kept only on
     * a Document object could not be read at all for exactly the popup §7.1.3.2 is written about — the answer
     * would depend on whether something had happened to touch the proxy first.
     *
     * PER-FLOW, AND THAT IS FREE: it is one enum word inside the bytes proxy_of captures, so an arm that
     * navigated this navigable and a sibling that did not each read their own, and a parked flow resumes onto
     * the value its world had. It moves with the rest of the binding in window_proxy_navigate — a navigation
     * replaces the active document, and this is that document's row. */
    OpenerPolicyValue opener_policy;
    /* HTML §8.1.3.1's TOP-LEVEL CREATION URL for the environments of this navigable's documents — kept beside
       the policy container because it is the same KIND of fact and arrives the same way: decided by the
       operation that created the navigable, read when a realm is finally built. §7.4 makes a CHILD navigable
       inherit its creator's and gives an AUXILIARY one its own address, and §7.4.5's navigation of a top-level
       traversable moves it to the new address while a nested navigable's stays where its creation put it.
       IT IS PER-FLOW, exactly like `url`: navigating a top-level traversable moves it, so an arm that
       navigated and an arm that did not must not share one. It is a POD pointer inside the bytes proxy_of
       captures, and — like every other string here — it is never freed on a navigation, because a parked
       flow's saved bytes still name the old one. */
    char   *top_level_url;
    /* HTML §8.1.3.1's TOP-LEVEL ORIGIN — the OTHER field of that pair, and a separate one because the standard
       says so in the field's own definition: "This is distinct from the top-level creation URL's origin when
       sandboxing, workers, and worklets are involved." §7.3.2.1 "Creating browsing contexts" states them in
       one breath and answers them differently — the URL is `about:blank` for a top-level browsing context
       while the origin is the initial Document's own — and §4.7 over a URL cannot express the third case at
       all, because §7.3.2.1's determine-the-origin hands ONE opaque origin to several Documents and an opaque
       origin has no serialization it can be recreated from.
       Permissions §5.1 step 5 reads THIS one: its key is generated from "settings's top-level origin", and
       §3.2 compares two keys with §7.1.1's same origin, whose step 1 is an identity comparison.
       BORROWED and POD, exactly like `origin` beside it: an origin lives for the agent, a navigation REPLACES
       this pointer, and the byte capture in proxy_of is therefore a complete description of the binding. */
    const Origin *top_level_origin;
    /* §7.2.2.1's `closed` IS TWO FACTS AND THE GETTER IS THEIR OR — "true if this's browsing context is null or
       its is closing is true". They are two because they happen at two TIMES. `close()` sets is-closing at its
       own call site and QUEUES the destruction; §7.3.1.6 "Navigable destruction"'s destroy-a-child-navigable
       queues one without setting is-closing at all; and the browsing context does not become null until
       §7.5.10 step 8 runs, in a task.
       Held as ONE byte those two times collapsed into the removal's, so a frame's WindowProxy reported a
       destruction that had not happened — while its Document, its queued tasks and its entangled ports were
       all still live, with nothing anywhere holding the fact that they had not been dealt with. */
    /* THE NEGATION OF HTML §3.1.1 "The Document object"'s "is initial about:blank" — "Each Document has an is
       initial about:blank, which is a boolean, initially false" — held on the NAVIGABLE because a navigable
       showing its initial Document has no realm to hold a Document-side field until something reaches through
       it. §7.4.4 "Non-fragment synchronous \"navigations\"" step 4 and §7.1.3.2's group-switch check are two of
       its readers; both ask the spec's flag, not this field's name. (§7.1.3.2 is "Browsing context group
       switches due to opener policy".)
       ITS NAME STATES THE FIRST WRITER AND THE FIELD IS BIGGER THAN THE NAME. §7.4 creates EVERY navigable with
       the initial about:blank Document, and a navigation is the only thing that ever REPLACES a navigable's
       active document — window_proxy_navigate below — so that write was for a while the whole of the flag. It
       is not: §8.4.1 "Opening the input stream" step 13 sets the Document's flag false without replacing any
       document at all, which is window_proxy_clear_initial_about_blank, and a `document.write` into a fresh
       frame is the commonest arrival at it. TWO WRITERS, ONE FLAG — reading this field as "was navigated"
       rather than as the standard's boolean is what would make that second write look like a lie.
       The alternative storage, testing the document's ADDRESS, is WRONG in this tree and not merely imprecise:
       navigable.c's load job navigates to `about:blank` for real ("the corpus does it while an initial load is
       still pending"), and that document is not the initial one.
       PER-FLOW like everything else in this record: an arm that navigated the frame and a sibling that did not
       must not share the answer, which is what the capture in proxy_of gives it for free. */
    uint8_t ever_navigated;
    /* HTML §7.3's "IS CREATED BY WEB CONTENT" — a top-level traversable's, and one of the two disjuncts of
       §7.2.2.1's SCRIPT-CLOSABLE. §7.3's create-a-new-top-level-traversable is reached from `window.open()`,
       which is this engine's navigable_open, so every navigable minted by window_proxy_new was created by web
       content and the one minted by window_proxy_new_self — the navigable the instance STARTED in, which the
       host loaded — was not. It is a CREATION fact, so like `is_popup` no flow can change it and the delta has
       nothing to capture. */
    uint8_t created_by_web_content;
    uint8_t closing;     /* §7.3's IS CLOSING — §7.2.2.1's close(), and only ever a top-level traversable */
    uint8_t destroyed;   /* §7.5.10 step 8 ran on this navigable's active document: its browsing context is null */
    /* §7.1.3.2 step 10's SWAP LEFT THIS BROWSING CONTEXT BEHIND — "browsingContext will not be used by the new
       Document that we are about to create" — so §7.2.2.1's "this's browsing context is null" is true of it for
       a reason that has nothing to do with destruction. It is a SECOND byte and not a second writer of the one
       above, because the two states differ in everything except `closed`: a destroyed navigable has NO active
       document (step 9 released the Window and nulled the realm), while a swapped-past one still has the exact
       Document it had — which is what makes a read through the opener's handle answer about THAT document
       rather than about the one the navigation went on to create in another instance. Reusing `destroyed` would
       have told proxy_realm that a navigable with a live realm has no document, which is the assert it makes. */
    uint8_t bc_discarded;
    /* THE SUBTREE WAIT, COUNTED DOWN AND PER OPERATION — how many of this navigable's child navigables have
       still to report before this one's own body may run, for each of the operations that walk a subtree
       (§7.4.2.4's beforeunload check, §7.5.9's unload, §7.5.10's destroy). It is here because it is PER-FLOW
       state of a navigable, exactly like `closed` and `name`: two forked arms tearing down the same subtree
       each count in their own world, and a shared integer would let one arm's child complete the other arm's
       wait. Zero for a navigable not running that operation, which is what makes a child's report identify the
       ROOT of the operation rather than being a case its caller has to test for. */
    uint32_t child_wait[WP_SUBTREE_OP_N];
    /* WHICH DOCUMENT the navigable's active document IS — the same id the world registry names worlds by, so
       "is this remote?" is one comparison against one identity rather than a second naming scheme kept beside
       it. A proxy whose doc is this instance's has a live `window`; any other doc is in a peer instance and
       has none, and those two facts are asserted together because a proxy where they disagree would answer a
       cross-document read with THIS document's Window — the exact failure the same-origin check exists to
       prevent. */
    uint32_t doc;
} ProxyData;

/* §7.2.2.1's `closed` GETTER, AS ONE EXPRESSION — "true if this's browsing context is null or its is closing
   is true". Written once here so that no member can ask half of it: a `closed` that read only the destroy
   flag would report a closing popup as open, and one that read only is-closing would report a removed frame
   as open. Both of those were the single byte this replaced, in the two directions it could be wrong.
   "BROWSING CONTEXT IS NULL" HAS TWO WRITERS AND THIS IS WHERE THEY MEET — §7.5.10 step 8's destruction and
   §7.1.3.2 step 10's swap, which discards a browsing context without destroying anything. Reading either alone
   is the same class of half-answer as the two above: after a COOP group swap the opener's handle would report
   the window it can no longer reach as open, and the engine would model a page real Chrome has cut off.
 *
 * AND THE TWO BELOW ARE THE RECORDED FLAGS, WHICH IS NO LONGER THE WHOLE OF EITHER QUESTION. There is a THIRD
 * moment at which a navigable's browsing context is null and it is the one pages actually read — §7.3.1.6's
 * destroy-a-child-navigable step 3, which severs the container relation SYNCHRONOUSLY while both flags above
 * are written by the queued destruction. `window_proxy_browsing_context_null` is that whole fact, and
 * §7.2.2.1's `closed`, §7.2.2.4's `opener` and §7.2.4's relevant Document each read it there.
 *
 * SO WHAT IS LEFT HERE IS A DIFFERENT QUESTION AND THE SPLIT IS DELIBERATE: `wp_closed` now means "this heap
 * holds no ACTIVE DOCUMENT for this navigable", which is what its remaining readers are actually asking —
 * §7.2.3's forward to W, `document`, `location`, `name`, and §7.2.3.5 step 2's indexed access. Those must NOT
 * take the sever, because §7.2.3's internal methods are performed on W, the [[Window]] internal slot, and a
 * Window OUTLIVES its browsing context: `document-domain-removed-iframe.html` reads `contentWindow.status` on
 * the line after `iframe.remove()` and expects the value, not `undefined` (its own comment cites the Chromium
 * bug where those properties went undefined). Folding the sever into this predicate is how that regresses, and
 * it would be silent — the wrong answer is a plausible one. Three questions, three readers. */
static bool wp_bc_null(const ProxyData *p) { return p->destroyed != 0 || p->bc_discarded != 0; }
static bool wp_closed(const ProxyData *p) { return p->closing != 0 || wp_bc_null(p); }

/* THIS COMPONENT'S NAME, spelled once — core/platform.c's row and every slot it declares to
   core/agent_state.h. The registry's row pairing is keyed by it and neither spelling is checked by the
   compiler, so a literal typed out once per declaration is exactly the misspelling that file's first walk
   exists to catch, and one of them going wrong is a check that is never RUN rather than one that got weaker. */
#define WP_COMPONENT "window_proxy"

static JSClassID g_proxy_class;
static JSRuntime *g_wp_rt;
/* §7.2.3's member surface. It answers from the navigable's own state where that is what the member is, and
   suspends on the host where the member reads the active document. See the member table lower down for which
   is which.
   EVERY NAME ON IT IS AN OWN PROPERTY OF THE PROXY, because §7.2.3 has no interface object: §7.2.3.5 answers
   each of these names and §7.2.3.10 lists it, so this object is where the descriptor is READ FROM and never
   what a prototype walk reaches for a same-origin W (proxy_surface_desc). The walk still reaches it for a
   CROSS-ORIGIN one, which is §7.2.3.5 steps 4-6 and this engine's stand-in for §7.2.1.3.4's per-realm
   anonymous functions — one object, two ways in, one answer either way.
   IT IS PER REALM, in quickjs's own class-proto slot, and that is an ANSWER and not an identity: a member runs
   in the realm that DEFINED it, so one shared object would answer `parent`, `name` and `close()` for every
   document out of whichever realm built it first — and the whole point of these members is that they read the
   asking document's side of a boundary. `a.postMessage === b.postMessage` still holds for two proxies of the
   same realm, which is what §7.2.3's shared surface means; two REALMS have two, exactly as two realms have
   two `Array.prototype.map`s. */
/* TWO GETTER IDS OVER ONE STEP DECLARATION, and they are two because a pool entry carries the MEMBER MAGIC —
   the step body reads it back with idl_step_magic to know which question it was asked. */
static int g_wp_len_getter_id = -1, g_wp_closed_getter_id = -1;
static int g_wp_name_setter_id = -1, g_wp_opener_setter_id = -1, g_wp_close_id = -1;
/* §7.2.2 The Window object's `location` is [PutForwards=href], and this surface is that same interface — so it
   carries the same forwarding setter the global's `location` does, minted from Web IDL's own declaration
   (idl_setter_id_put_forwards) rather than from a body of this component's. */
static int g_wp_location_setter_id = -1;


#define WP_OFF(f) (uint16_t)offsetof(ProxyData, f)
static const uint16_t PROXY_VALS[] = { WP_OFF(window), WP_OFF(parent), WP_OFF(opener), WP_OFF(container) };
static const CowRecord PROXY_REC = { sizeof(ProxyData), PROXY_VALS, 4 };

/* THE CAPTURE IS IN THE ACCESSOR, as it is for every other component record: a record a flow has reached is one
   it may write, and there is then no write site to miss. `origin` is a POD pointer and `doc` a POD id inside the
   captured bytes — a navigation REPLACES them rather than mutating what they point at, so the byte copy is a
   complete description of the binding at that moment.
   THE STRINGS ARE THEREFORE NOT FREED ON NAVIGATION. A delta's saved bytes still name the old pointer, and
   freeing it would leave a parked flow resuming onto freed memory. They are owned by the PROXY and released
   with it, which costs one string per navigation and is the only arrangement in which a parked flow's binding
   stays readable. */
static ProxyData *proxy_of(JSValueConst v)
{
    ProxyData *p = JS_GetOpaque(v, g_proxy_class);
    if (p) cow_capture_host_record(v, p, &PROXY_REC);
    return p;
}

/* WRITE ONE OF THE FOUR, and never `JS_FreeValue(ctx, p->f); p->f = <build one>;` — see cow.h for the order and
   the defect. Releasing this record's OLD value is the hazard from the release side and it is the one that
   bites here: `window` names a global, so giving it back runs the page's own platform finalizers, and an
   allocation anywhere in them IS a collection (js_trigger_gc has exactly one caller, JS_NewObjectFromShape) —
   which reaches this record through proxy_gc_mark and decrefs whatever the slot still names. Publishing first
   makes the slot correct throughout.
   The record and its layout are bound HERE rather than at each call, so no site can pass a slot from one record
   with the layout of another. Every write below goes through it; the two MINTS do not, and that is the one
   honest exception: before JS_SetOpaque the record is unreachable by the collector and its slots hold no value
   to release. */
/* THE ADDRESS PASSES THROUGH: the asserts inside are about the SLOT, so they must name the WRITE and not this
   line — see cow.h's THE SITE TRAVELS WITH THE OPERATION. */
static void wp_set_at(JSContext *ctx, ProxyData *p, JSValue *slot, JSValue v,
                      const char *file, int line)
{
    cow_record_set_at(ctx, p, &PROXY_REC, slot, v, file, line);
}
#define wp_set(ctx_, p_, slot_, v_) wp_set_at((ctx_), (p_), (slot_), (v_), __FILE__, __LINE__)

/* §7.2.1's SAME-ORIGIN CHECK — §7.1.1's algorithm over two RECORDS, which is the whole of it: step 1 is the
 * nonce comparison and step 2 is the tuple comparison, both inside origin_same.
 *
 * IT USED TO BE A STRING COMPARE, AND THAT COULD NOT RUN STEP 1. "If A and B are the same opaque origin, then
 * return true" is an IDENTITY comparison, and identity is exactly what §7.1.1's serializer drops — every opaque
 * origin serializes to "null" — so this component could not tell one opaque origin looked at twice from two
 * distinct ones and guessed "two". That is right for two sandboxed frames and wrong for the case step 1 exists
 * for: §7.3.2.1's determine-the-origin hands ONE opaque origin to several Documents on purpose, so a `data:`
 * document's `about:blank` child was refused every member of its own navigable outside the fixed cross-origin
 * list, and its `contentDocument` was null. The origin is a record now (core/url/origin.h) and the guess is
 * gone with the assert that named it.
 *
 * THE ACCESSOR SIDE IS THE AGENT'S ORIGIN. An instance is an origin-keyed agent cluster, so the origin of
 * whoever is reading is this agent's, and there is exactly one of those. */
static bool proxy_same_origin(const ProxyData *p)
{
    DCHECK(p->origin != NULL, "a WindowProxy carries no origin — §7.4 gives every navigable's document one "
                              "and §7.3.2.1's determine the origin decides which, so a proxy without one was "
                              "minted somewhere that did not");
    return origin_same(p->origin, origin_agent());
}

/* §7.1.1's SAME ORIGIN-DOMAIN over the same pair — §7.3.1.3 "Child navigables"' `content document` filter,
 * which is a DIFFERENT algorithm from the one above and not a laxer spelling of it.
 *
 * AND ITS ACCESSOR SIDE IS THE ASKING DOCUMENT'S ORIGIN, NOT THE AGENT'S. That is the one place the two checks
 * must be written differently, and the reason is §7.1.1.2: every origin in this heap is same ORIGIN with the
 * agent's by construction, so proxy_same_origin may compare against the agent's record and be exact — but a
 * DOMAIN is per Document. §7.3.1.3's content document filters "container document" against the content
 * document, and a child navigable's Document holds its own tuple record (§7.3.2.1's determine the origin
 * step 5 mints one per address), so a child that has relaxed its domain while its container has not is a
 * pair the standard's own table answers ❌ for. Reading the agent's record here would have answered ✅ —
 * right until the moment the member it exists to gate became implementable, which is what makes it worth
 * one argument. */
static bool proxy_same_origin_domain(JSContext *ctx, const ProxyData *p)
{
    DCHECK(p->origin != NULL, "a WindowProxy carries no origin");
    return origin_same_origin_domain(p->origin, window_proxy_origin(document_window_proxy(ctx)));
}

/* IS THIS A NAVIGABLE'S PROXY — the type test every algorithm in this browser that has to tell a navigable
   from an ordinary object branches on, and the one place a zeroed class id would be read as an ANSWER rather
   than as a defect. The `g_proxy_class != 0 &&` that stood here made "this component is not declared" and
   "that object is not a WindowProxy" one value, and the two became separate states the moment
   window_proxy_free started giving the id back: after the release column every live proxy in the heap would
   report itself as something else, silently, at every one of those sites — the plausible answer, which is
   worse than a crash and indistinguishable from a measurement. It is a DCHECK because both states are
   impossible rather than rare — this test is reached only from
   an algorithm running in a realm the declaration pass built, and no collector entry and no other component's
   release calls it (the finalizer and the gc_mark above read their record through JS_GetAnyOpaque, which is
   the whole reason they can run after this id is gone). */
bool window_proxy_is(JSValueConst v)
{
    DCHECK(g_wp_rt != NULL,
           "§7.2.3's type test was asked before window_proxy_init declared the class or after "
           "window_proxy_free gave it back — with no class there is no answer, and the guard that used to "
           "stand here returned `not a WindowProxy` for a browser that has none and for a live proxy alike");
    return JS_GetOpaque(v, g_proxy_class) != NULL;
}

bool window_proxy_is_window(JSContext *ctx, JSValueConst v)
{
    JSValue g;
    bool same;

    if (window_proxy_is(v))
        return true;
    /* THE ASKING REALM STANDS FOR ITSELF AS ITS GLOBAL — win_or_proxy's mapping, asked as a type test rather
       than performed as a substitution. Same object, same rule, one place it is decided. */
    g = JS_GetGlobalObject(ctx);
    same = JS_VALUE_GET_PTR(g) == JS_VALUE_GET_PTR(v);
    JS_FreeValue(ctx, g);
    return same;
}

/* Every string this proxy has ever held, so a delta that still names an older one resumes onto live memory.
   A list rather than a single slot for the reason the accessor's comment gives. */
typedef struct OwnedStr { char *s; struct OwnedStr *next; } OwnedStr;
static OwnedStr *g_strings;

static char *proxy_strdup(const char *s)
{
    OwnedStr *e;
    char *c;
    if (!s) return NULL;
    c = strdup(s);
    CHECK(c != NULL, "window proxy: OOM recording an origin");
    e = malloc(sizeof *e);
    CHECK(e != NULL, "window proxy: OOM recording an origin");
    e->s = c;
    e->next = g_strings;
    g_strings = e;
    return c;
}

/* THE ONE WindowProxy PER REMOTE DOCUMENT — the importing half of a navigable's IDENTITY, and the reason it is
   a table rather than a mint at each call site: `w[0] === w[0]`, `w.frames[0] === iframe.contentWindow` and
   `event.source === event.source` are page-visible identities over ONE navigable, and every one of them
   answers false the moment two sites each mint their own proxy for it.
   THE ROW BORROWS, AND THAT IS WHAT MAKES THE TABLE ALLOWED TO EXIST. core/dom/document.h refuses a registry
   keyed by document in as many words, and it is right about the OWNING kind: a navigable is created per
   `open()` per flow, so an owning row would be an immortal root holding a proxy for every navigable a
   forced-execution frontier ever created — the same shape as the realm-per-read that cost the frontier 43% of
   its depth. A borrowed row is not that shape at all: an entry exists only while something ELSE holds the
   proxy, and proxy_finalizer takes the row out at the moment the collector takes the object. So the table is
   exactly the set of remote navigables that are still alive, there is no reclamation left to build, and
   nothing is kept alive by being named. */
typedef struct { uint32_t doc; JSValueConst proxy; } RemoteNav;
static RemoteNav *g_remote_navs;
static int        g_remote_navs_n, g_remote_navs_cap;

/* THE RECORD AS A COLLECTOR ENTRY SEES IT. JS_GetAnyOpaque AND NOT JS_GetOpaque(val, g_proxy_class):
   core/agent_state.h states the rule and the reason — window_proxy_free gives the class id back, and the
   collection that finalizes this agent's object graph runs AFTER the release column, so a lookup against that
   id would answer NULL for every proxy a page still held and this component would silently free none of them.
   The collector dispatched THROUGH the class, so the id is a fact it already has and must not look up. */
static ProxyData *proxy_collector_record(JSValueConst val)
{
    JSClassID cid = 0;

    return JS_GetAnyOpaque(val, &cid);
}

static void proxy_finalizer(JSRuntime *rt, JSValue val)
{
    ProxyData *p = proxy_collector_record(val);
    int i;
    (void)rt;
    /* THE ROW GOES FIRST, and unconditionally: it names this object by POINTER, so a row left behind would
       hand the next ask for that document a freed proxy. Above the `p` assert because the table's claim is
       about the OBJECT, not about the record hanging off it. */
    for (i = 0; i < g_remote_navs_n; i++)
        if (JS_VALUE_GET_PTR(g_remote_navs[i].proxy) == JS_VALUE_GET_PTR(val)) {
            g_remote_navs[i] = g_remote_navs[--g_remote_navs_n];
            break;
        }
    /* Both mints attach the record with nothing between JS_NewObjectClass and JS_SetOpaque that allocates on
       the JS heap or returns — proxy_strdup is malloc and JS_DupValue is a refcount — so a WindowProxy with no
       record has never existed, and the `if (!p) return;` that used to stand here was reading a zeroed class
       id rather than an absent record. */
    DCHECK(p != NULL, "a WindowProxy was finalized with no §7.2.3 record — both mints attach one before the "
                      "object can reach a collection");
    JS_FreeValueRT(rt, p->window);
    JS_FreeValueRT(rt, p->parent);
    JS_FreeValueRT(rt, p->opener);
    JS_FreeValueRT(rt, p->container);
    free(p);   /* the strings are the component's, released at teardown — see proxy_of */
}

static void proxy_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    ProxyData *p = proxy_collector_record(val);
    /* AN UNMARKED CHILD IS WORSE THAN A LEAKED PARENT — it keeps the internal reference gc_decref subtracts,
       so gc_scan reads it as rooted from outside the heap and it is never collected at all. That is what a
       record read through a released class id costs here, and it is silent (core/agent_state.h). */
    DCHECK(p != NULL, "a WindowProxy was marked with no §7.2.3 record — both mints attach one before the "
                      "object can reach a collection");
    /* The Window holds the proxy (as `window`) and the proxy holds the Window: a cycle, and one the collector
       has to see or every navigable leaks its global. `parent` and `opener` close cycles of their own — a child
       names its creator's Window, which reaches the child through the element wrapper that holds it. */
    JS_MarkValue(rt, p->window, mark_func);
    JS_MarkValue(rt, p->parent, mark_func);
    JS_MarkValue(rt, p->opener, mark_func);
    /* §7.3.1.3's container closes a cycle of its own, and a tighter one than the three above: the element's
       wrapper holds this proxy in a hidden own slot and this record holds the wrapper back, so an unmarked
       container would leak every `<iframe>` a page ever inserted together with the navigable it presents. */
    JS_MarkValue(rt, p->container, mark_func);
}

/* MATERIALIZE THE ACTIVE DOCUMENT — THE ONE PLACE A REALM IS BUILT. Every member that reads THROUGH to the
   active document goes through here, and so does §7.4's navigation, which calls window_proxy_realm to
   materialize the navigable it just created. One build site, reached two ways; see navigable.h for which
   navigable is materialized WHEN and why the difference is a spec sentence rather than a saving. */
static JSContext *proxy_realm(JSContext *ctx, JSValueConst proxy, ProxyData *p)
{
    /* SAME-ORIGIN IMPLIES HOSTED, by construction: an instance is an ORIGIN-KEYED agent, so a navigable this
       agent does not hold is one whose active document is cross-origin — and every same-origin-only member is
       therefore answerable in this turn. Asserted here because it is the pairing the member table depends on:
       if it ever stopped holding, a same-origin read would silently fall through to the cross-origin path. */
    DCHECK(world_doc_hosted(p->doc),
           "the realm of a navigable whose active document is a PEER's was asked for — a same-origin navigable "
           "is a realm of this agent by construction, so this proxy is cross-origin and the read that reached "
           "here should have been a SecurityError");
    /* AND A DESTROYED NAVIGABLE IS NEVER MATERIALIZED. §7.5.10 step 9 nulled this navigable's active document,
       so a NULL realm on a destroyed navigable means "there is no document" and not "there is not one YET" —
       the two are the same field and only this tells them apart. Without it the next read through the proxy
       would BUILD a fresh Document for a navigable whose browsing context is null: navigable.h's deferral
       running backwards, a realm allocated by the very destruction that was meant to give one back, and a page
       the destroy had torn down answering out of a document nothing had ever run. §7.2.1's cross-origin list is
       what a page may still read on a destroyed navigable, and proxy_forward_window answers every one of those
       from this record without a realm, so a caller that reaches here has asked for an ACTIVE DOCUMENT. */
    DCHECK(!p->destroyed,
           "the ACTIVE DOCUMENT of a navigable §7.5.10 destroyed was asked for — that navigable has none "
           "(step 9 nulls it), and materializing one here would build a Document for a browsing context that "
           "is null; the members readable after a destruction are §7.2.1's list, answered from this record");
    if (!p->realm) {
        DCHECK(p->url != NULL, "a WindowProxy with no realm and no address was read through — a proxy over a "
                               "realm that already exists is minted by window_proxy_new_self, which carries it");
        /* THE INITIAL about:blank, WHICH CAME FROM NO RESPONSE — so no bytes, and §7.1.7's container is the
           clone of its creator's this navigable was CREATED with (see creator_policy). Fetching anything here
           is not deferred, it is IMPOSSIBLE: this runs inside the property read that reached through the navigable,
           and a fetch suspends. The document an address serves arrives on the load job instead, which is a
           flow and can park (navigable.c). */
        /* THE ENVIRONMENT'S TOP-LEVEL CREATION URL IS THE NAVIGABLE'S, not the reading realm's — the read that
           materializes an about:blank child may come from any same-origin document, and answering from `ctx`
           would give the child whichever document happened to touch it first. The same sentence as
           `creator_policy` beside it, and the same field for the same reason. */
        /* §7.3.2.1's CREATE A NEW BROWSING CONTEXT AND DOCUMENT gives this Document its ACTIVE SANDBOXING
           FLAG SET, and what it gives is `sandboxFlags` — the navigable's CREATION sandboxing flags, with NO
           CSP-derived half. That absence is the spec's and it is what `allow-popups-to-escape-sandbox` is
           for: a sandboxed page's popup inherits the creator's flags only when the propagate flag survived
           the parse, and unioning the inherited policy's own CSP-derived flags in here would re-sandbox the
           popup that keyword exists to free. The union belongs to §7.4.5's NAVIGATION (core/frame/navigable.c),
           which is the other place a Document of this navigable is created. */
        /* §7.4's ABOUT BASE URL travels with the creation exactly as the policy container does, and for the
           identical reason stated one field over: this Document IS the initial about:blank, `creatorBaseURL`
           is what §7.4 gives it, and the realm reading through this navigable is not necessarily the one that
           created it. */
        /* NO BODY AND THEREFORE NO CONTENT TYPE — this is §7.2's initial `about:blank`, materialized rather
           than loaded, so there is no response for §7.4.5 to compute a type from and §7.4 makes it an HTML
           document outright. The NULL is that statement, not an unknown. */
        p->realm = navigable_realm(ctx, p->doc, p->url, p->top_level_url, p->origin, proxy, NULL, 0, NULL, NULL,
                                   p->creator_policy, p->creator_base_url, p->creation_sandbox_flags);
        wp_set(ctx, p, &p->window, JS_GetGlobalObject(p->realm));
    }
    return p->realm;
}

/* §7.4.2.2's NAVIGATE — see window_proxy.h. Reached from navigable.c, which owns the fetch and the realm. */
void window_proxy_navigate(JSContext *ctx, JSValueConst proxy, JSContext *realm, uint32_t doc,
                           const char *url, const char *top_level_url, const Origin *top_level_origin,
                           const Origin *origin, OpenerPolicyValue opener_policy)
{
    ProxyData *p = proxy_of(proxy);   /* the capture is in the accessor — the WHOLE binding rides the delta */

    DCHECK(p != NULL, "something that is not a WindowProxy was navigated");
    DCHECK(realm != NULL, "a navigable was navigated to no realm — a Document this agent holds IS a realm, and "
                          "a cross-origin destination is a peer's document, which is a host route and not this");
    DCHECK(world_doc_hosted(doc), "a navigable was navigated to a document this agent does not hold");
    /* A DESTROYED NAVIGABLE IS NOT NAVIGATED. §7.4.2.2 acts on the navigable's active document and §7.5.10
       step 9 nulled it; the §7.4 step 14 load that could still have been in flight when the container was
       removed is exactly what §7.5.10 step 7 takes off the queue without running (quickjs.c's
       JS_DropJobsForContext walks the baseline list for that case by name). Reaching here means one of those
       two stopped holding, and the write below would give a destroyed navigable a live Window again. */
    DCHECK(!p->destroyed,
           "a navigable whose active document §7.5.10 destroyed was NAVIGATED — §7.4.2.2 replaces an active "
           "document and this navigable has none, so either the load job outlived step 7's drop or a "
           "destroyed navigable was chosen as a navigation target");
    /* AND A SWAPPED-PAST BROWSING CONTEXT IS NOT NAVIGATED EITHER, for a different reason with the same shape.
       §7.1.3.2's swap moved this navigable's next Document into a browsing context in ANOTHER instance, so the
       navigable a later `w.location = …` names is that one and not this record — which this instance cannot
       navigate, because navigating another agent's navigable is a cross-instance OPERATION and there is none.
       Writing the binding here instead would resurrect the window the swap closed. */
    DCHECK(!p->bc_discarded,
           "a browsing context §7.1.3.2's group SWAP discarded was NAVIGATED — the navigable moved to the "
           "instance the swap provisioned, so this record is the one the page's handle reads `closed` off and "
           "never a navigation target. BUILD the cross-instance navigate: a navigation of a navigable another "
           "agent holds is an operation routed to that agent (core/frame/remote_op.h), not a local write");
    /* THE OLD WINDOW'S REFERENCE IS THIS SLOT'S, and the delta already dupped its own copy when the capture
       above ran — so releasing it here leaves the parked arm's copy intact and the live slot free to move.
       IT IS PUBLISHED BEFORE IT IS RELEASED (wp_set): the release is what runs the OLD global's finalizers. */
    p->realm  = realm;
    wp_set(ctx, p, &p->window, JS_GetGlobalObject(realm));
    p->doc    = doc;
    /* THE OLD STRINGS ARE NOT FREED. A parked flow's saved bytes still name them (see proxy_of), so freeing
       here would resume that flow onto freed memory; they are the PROXY's and are released with it. */
    p->url    = proxy_strdup(url);
    DCHECK(origin != NULL, "a navigable was navigated to a document with no origin — §7.3.2.1's determine "
                           "the origin answers for every Document, and a navigation's answer is the one the "
                           "LOAD computed, never re-derived here from the address (that second answer is what "
                           "loses an inherited identity)");
    p->origin = origin;   /* BORROWED, and REPLACED rather than mutated — see the field */
    /* THE NEW DOCUMENT'S ENVIRONMENT MOVED WITH IT, and the caller states where to — a navigation of a
       TOP-LEVEL traversable puts the environment at the new address, while a nested navigable's stays where
       its creation put it (HTML §8.1.3.1). It is not re-derived here from `url`, because whether this
       navigable is nested is a fact about the OPERATION's target and the realm the caller has ALREADY BUILT
       was built under this exact string: deriving it a second time is the second answer that disagrees. */
    DCHECK(top_level_url != NULL && *top_level_url,
           "a navigable was navigated with no top-level creation URL for the new document's environment — the "
           "realm the caller just built has one, and these two must be the same string");
    p->top_level_url = proxy_strdup(top_level_url);
    /* AND ITS TOP-LEVEL ORIGIN MOVED WITH IT, from the same caller and for the same reason. §7.4.5's fetch
       gives a top-level traversable's new environment the NEW DOCUMENT's origin and a nested navigable's the
       parent environment's — the pair is one decision about the operation's target, so a navigation that
       moved the URL and left the origin behind would key this document's permissions to the document it
       replaced. */
    DCHECK(top_level_origin != NULL,
           "a navigable was navigated with no top-level origin for the new document's environment — §8.1.3.1 "
           "gives every environment one, and the realm the caller just built was built under it");
    p->top_level_origin = top_level_origin;   /* BORROWED and REPLACED rather than mutated — see the field */
    /* §7.5.1's OPENER POLICY ROW OF THE DOCUMENT THIS NAVIGATION IS CREATING — "opener policy …
       navigationParams's cross-origin opener policy". It moves here with the rest of the binding because it is
       a fact of the ACTIVE DOCUMENT and this call is what replaces one; the value is the OPERATION's (obtained
       from the response the load fetched) and is never re-derived from anything reachable through this proxy,
       which by the time this runs is the document being replaced. */
    p->opener_policy = opener_policy;
    /* THE NAVIGABLE IS NO LONGER SHOWING WHAT §7.4 CREATED IT WITH — see `ever_navigated`. Written here
       because this is the one site that replaces a navigable's active document, which is exactly what makes
       the flag answerable at all. */
    p->ever_navigated = 1;
}

/* §7.5.1's OPENER POLICY ROW, READ FROM THE NAVIGABLE'S SIDE — see the field. THROUGH proxy_of like every
   other read of this record, so the answer is the running flow's: an arm that navigated this navigable to a
   `same-origin` document and a sibling that did not are two worlds with two answers. */
OpenerPolicyValue window_proxy_opener_policy(JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);

    DCHECK(p != NULL, "the opener policy of something that is not a WindowProxy was asked for");
    /* A PEER'S NAVIGABLE HAS NO ANSWER HERE, and this crashes rather than reporting `unsafe-none` — which is a
       legal value, so a default would be indistinguishable from a measurement. §7.3.2.1's inheritance reaches
       a REMOTE top-level browsing context in exactly one shape (a same-origin document nested through a
       cross-origin one), and answering it needs the cross-instance read window_proxy_window names. */
    DCHECK(world_doc_hosted(p->doc),
           "the §7.5.1 opener policy of a navigable in ANOTHER WASM instance was asked for — §7.1.3.2 and "
           "§7.3.2.1 both read it off a browsing context's ACTIVE DOCUMENT, and that document is a peer's. "
           "Build the cross-instance resolve the same way window_proxy_window names it: the flow SUSPENDS "
           "here, the peer answers in its own scheduled turn under this flow's world, and the flow resumes "
           "with the value");
    return p->opener_policy;
}

/* HTML §7.4.4 step 4's "is initial about:blank", from the navigable's side — see `ever_navigated`. Read
   THROUGH proxy_of like every other read of this record, so the answer is the running flow's. */
bool window_proxy_ever_navigated(JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);

    DCHECK(p != NULL, "something that is not a WindowProxy was asked whether its navigable has been navigated");
    return p->ever_navigated != 0;
}

/* HTML §8.4.1 "Opening the input stream" step 13's "Set document's is initial about:blank to false" — the
 * flag's SECOND writer, and the reason the field's comment says the name is smaller than the field.
 *
 * IT IS NOT A NAVIGATION AND MUST NOT BE WRITTEN AS ONE. §8.4.1 replaces the Document's CONTENT while keeping
 * the same Document object, so every other row of the binding window_proxy_navigate moves — the realm, the
 * Window, the document id, the address, the origin, the environment's top-level pair, the opener policy — is
 * unchanged, and routing this through that function would hand the navigable a second realm for a document it
 * already has. This writes the one byte §8.4.1 names and nothing else.
 *
 * THROUGH proxy_of, so the write lands in the RUNNING FLOW'S delta: an arm whose script opened the document
 * and a sibling standing on the unwritten page are two worlds, and §7.4.4 "Non-fragment synchronous
 * \"navigations\"" step 4 must answer each of them its own way. */
void window_proxy_clear_initial_about_blank(JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);

    DCHECK(p != NULL, "§8.4.1 step 13 cleared the is-initial-about:blank of something that is not a WindowProxy");
    /* A PEER'S NAVIGABLE CANNOT BE WRITTEN FROM HERE, for the reason window_proxy_opener_policy states on the
       read side: the flag is a fact about the ACTIVE DOCUMENT and that document is another agent's heap.
       §8.4.1 step 4's same-origin check makes this unreachable for a cross-ORIGIN document, so what would
       arrive here is the same origin in another browsing-context group — which is a cross-instance OPERATION
       (core/frame/remote_op.h) and not a local byte. */
    DCHECK(world_doc_hosted(p->doc),
           "§8.4.1 step 13 ran against a navigable in ANOTHER WASM instance — the document open steps replace "
           "the content of a Document this agent does not hold, so the flag they clear lives in the peer's "
           "record. ROUTE the open: the flow suspends here, the peer runs the document open steps on its own "
           "Document in its own scheduled turn under this flow's world, and the flow resumes");
    p->ever_navigated = 1;
}

/* §7.3's IS CREATED BY WEB CONTENT — see the field. Read off the opaque directly rather than through proxy_of:
   it is a creation fact no flow can change, so there is nothing here for a delta to isolate. */
bool window_proxy_created_by_web_content(JSValueConst proxy)
{
    ProxyData *p = JS_GetOpaque(proxy, g_proxy_class);

    DCHECK(p != NULL, "something that is not a WindowProxy was asked whether web content created it");
    return p->created_by_web_content != 0;
}

/* THE SELF PROXY'S REALM IS THE ONE ASKING — already built, so there is nothing to materialize. Stated as its
   own step rather than folded into the mint because it is the one case where the realm precedes the proxy. */
static void proxy_adopt_realm(JSContext *ctx, JSValueConst proxy, JSContext *realm)
{
    ProxyData *p = JS_GetOpaque(proxy, g_proxy_class);

    DCHECK(p != NULL, "a realm was handed to something that is not a WindowProxy");
    DCHECK(p->realm == NULL, "a navigable was given a second realm through the ADOPT path — it already has an "
                             "active document, and replacing one is window_proxy_navigate, which moves all "
                             "five facts of the binding at once rather than handing this one a second realm");
    p->realm  = realm;   /* BORROWED — the agent owns its realms */
    wp_set(ctx, p, &p->window, JS_GetGlobalObject(realm));
}

JSValue window_proxy_new(JSContext *ctx, uint32_t doc, const char *url, const Origin *origin, const char *name,
                         bool is_popup, SandboxFlags creation_sandbox_flags, OpenerPolicyValue opener_policy,
                         SerializedPolicyContainer creator_policy, const char *creator_base_url,
                         const char *top_level_url,
                         const Origin *top_level_origin, JSValueConst parent, JSValueConst opener)
{
    JSValue obj;
    ProxyData *p;

    DCHECK(g_wp_rt != NULL, "a WindowProxy was minted before window_proxy_init ran");
    /* A LOCAL PROXY NAMES A DOCUMENT THIS AGENT HOLDS THE REALM OF, and that is checked rather than assumed:
       the whole member table below decides "answer now" from exactly this fact, so a proxy that carries a live
       Window for a document living elsewhere would answer a cross-instance read out of the wrong heap. */
    DCHECK(world_doc_hosted(doc),
           "a local WindowProxy was minted for a document whose realm this agent does not hold — a document is "
           "adopted when §7.4 decides to host it (world_doc_adopt), and one that never was belongs to a peer");
    /* JS_NewObjectClass TAKES THE PROTOTYPE FROM THE CLASS SLOT OF THIS REALM — there is no separate
       JS_SetPrototype here, because a mint that had to remember one is a mint that can forget it. */
    obj = JS_NewObjectClass(ctx, g_proxy_class);
    if (JS_IsException(obj)) return obj;
    p = calloc(1, sizeof *p);
    CHECK(p != NULL, "window proxy: OOM building a WindowProxy");
    /* §7.3's IS CREATED BY WEB CONTENT — this mint IS §7.4's create a new navigable, whose top-level form is
       `window.open()`. See the field. */
    p->created_by_web_content = 1;
    p->window = JS_UNDEFINED;   /* materialized by proxy_realm — at creation, or on the first read */
    p->realm  = NULL;
    p->url    = url ? proxy_strdup(url) : NULL;   /* NULL only for the self proxy, whose realm is already built */
    DCHECK(origin != NULL, "a navigable was created with no origin — §7.3.2.1's determine-the-origin "
                           "answers for every Document, including the initial about:blank one §7.4 creates a "
                           "navigable with, and the answer is a RECORD whose identity the same-origin check "
                           "compares");
    p->origin = origin;   /* BORROWED — an origin lives for the agent (core/url/origin.h) */
    p->name   = name && *name ? proxy_strdup(name) : NULL;
    /* §7.4 NAMED IT. This mint is the one §7.4's create-a-new-navigable reaches, so the name it was given is
       the navigable's name — including the empty one a `open(url)` with no target gives. */
    p->name_known = 1;
    p->is_popup = is_popup ? 1 : 0;
    /* §7.1.7's CLONE OF THE CREATOR'S CONTAINER, COPIED ITEM BY ITEM INTO STORAGE THAT OUTLIVES THE CREATOR'S
       FRAME. The caller's bytes belong to the operation that is creating this navigable and the initial
       about:blank is materialized long after it returns, so every item is proxy_strdup'd here. A CONTAINER
       EXISTS even when it holds no policies — §2.2 gives a CSP list an origin whether or not it has any, and
       this navigable's initial Document is installed with it either way — so the assert is that there IS a
       container, never that it carries policy text. */
    DCHECK(serialized_policy_container_exists(creator_policy),
           "§7.4 created a navigable with no §7.1.7 POLICY CONTAINER to clone — its initial about:blank "
           "Document has no response of its own, so a container is the only thing it can be created with, and "
           "without one it would resolve `'self'` against nothing: the one case §2.2's self-origin exists for");
    /* §7.1.4'S TWO ENDPOINT STRINGS ARE COPIED HERE FOR EXACTLY THE SAME SENTENCE, and they are copied
       UNCONDITIONALLY where the policy text is copied only when it is non-empty: §7.1.4 spells an absent
       endpoint as the EMPTY STRING, so there is no NULL form of one for this to reproduce, and
       serialized_embedder_policy refuses a NULL anyway. The two VALUES are scalars and ride the struct. */
    p->creator_policy =
        serialized_policy_container(creator_policy.csp && *creator_policy.csp
                                        ? proxy_strdup(creator_policy.csp) : NULL,
                                    proxy_strdup(creator_policy.self_origin),
                                    serialized_embedder_policy(creator_policy.embedder.value,
                                                               proxy_strdup(creator_policy.embedder.endpoint),
                                                               creator_policy.embedder.report_only_value,
                                                               proxy_strdup(creator_policy.embedder
                                                                                .report_only_endpoint)),
                                    /* …AND §7.1.7's INTEGRITY POLICY ITEM, DEEP-COPIED LIKE THE POLICY TEXT
                                       BESIDE IT. This record outlives the call that built it, so an item that
                                       borrowed the creator's bytes would be a dangling pointer by the time a
                                       deferred realm materializes — which is the same reason `csp` is
                                       proxy_strdup'd here and not kept. */
                                    creator_policy.integrity_policy && *creator_policy.integrity_policy
                                        ? proxy_strdup(creator_policy.integrity_policy) : NULL);
    /* §7.4's `let creatorBaseURL be null` is the ABSENCE of a creator, so NULL here is a real state and not a
       caller that forgot: the root navigable has no creator, and a navigable created with an address takes its
       Document from a response, which §2.4.3 gives a null about base URL. */
    p->creator_base_url = creator_base_url && *creator_base_url ? proxy_strdup(creator_base_url) : NULL;
    /* §7.1.5's creation sandboxing flags, decided by §7.4's create — see the field. An EMPTY set is the
       ordinary answer (no `<iframe sandbox>`, no propagating opener) and is not a "not yet known": §7.1.5
       says a browsing context's popup sandboxing flag set starts empty and an absent `sandbox` attribute is
       an empty iframe sandboxing flag set, so there is no third state here to spell. */
    p->creation_sandbox_flags = creation_sandbox_flags;
    /* §7.3.2.1's INHERITED OPENER POLICY, decided by the CREATOR and stated by it: "if creator's origin is same
       origin with creator's relevant settings object's top-level origin, then set document's opener policy to
       creator's browsing context's top-level browsing context's active document's opener policy" — and
       `unsafe-none` otherwise, which is §7.1.3's initial value and a real answer rather than an absence. It is
       an argument for the same reason the creator's policy container beside it is: which of §7.3.2.1's two arms
       applies is a fact about the OPERATION creating this navigable, and no inspection of the navigable being
       created could recover it. */
    p->opener_policy = opener_policy;
    /* EVERY ENVIRONMENT HAS A TOP-LEVEL CREATION URL, so unlike the policy container there is no "none" here
       to spell: §7.4 either nests the navigable (inherit the creator's) or makes it a top-level traversable
       (its own address), and both are addresses. A caller with nothing to pass has not decided which of the
       two it is building, and every member gated on §8.1.3.5 would then be installed on a guess. */
    DCHECK(top_level_url != NULL && *top_level_url,
           "a navigable was created with no TOP-LEVEL CREATION URL — HTML §8.1.3.5 reads it to decide whether "
           "the documents of this navigable are SECURE CONTEXTS, and §7.4 says which url it is: the creator's "
           "for a nested navigable, this navigable's own address for an auxiliary one");
    p->top_level_url = proxy_strdup(top_level_url);
    /* AND EVERY ENVIRONMENT HAS A TOP-LEVEL ORIGIN, asserted separately from the URL beside it because it is a
       separate field with a separate answer: §7.3.2.1 gives a top-level browsing context the URL `about:blank`
       and the ORIGIN of the initial Document it creates, which are not each other's derivation. A caller with
       nothing to pass has not decided whether it is nesting this navigable or making it a top-level
       traversable, and Permissions §5.1 step 5 would then key this navigable's grants to a guess. */
    DCHECK(top_level_origin != NULL,
           "a navigable was created with no TOP-LEVEL ORIGIN — HTML §8.1.3.1 gives every environment one and "
           "§7.3.2.1 says which it is: the creator's for a nested navigable, the origin of the initial "
           "about:blank Document for an auxiliary one, which is the origin this same call is being given");
    p->top_level_origin = top_level_origin;   /* BORROWED — an origin lives for the agent */
    p->parent = JS_DupValue(ctx, parent);
    p->opener = JS_DupValue(ctx, opener);
    /* §7.3.1.3: A NAVIGABLE IS CREATED WITH NO CONTAINER AND IS GIVEN ONE, in that order and by that
       algorithm — "Let navigable be a new navigable … Set element's content navigable to navigable" — so this
       mint states the pre-operation value and §7.4's create writes the link (window_proxy_set_container).
       It is the state an AUXILIARY navigable keeps: §7.3.1.7 step 8 creates one from a target name and there is
       no element anywhere in that algorithm for it to be presented by. */
    p->container = JS_NULL;
    p->doc = doc;
    JS_SetOpaque(obj, p);
    return obj;
}

/* §7.2.3's proxy for the REALM THAT IS ASKING — the one `window`, `self` and `e.source` are. Its realm is
   this one, which the caller is standing in rather than creating: the three differences from a §7.4 child are
   that the realm is handed over from the outside, that nobody here stated the navigable's name, and that
   §7.3.1.3's PARENT is a statement the HOST makes rather than one this mint is in a position to make. */
JSValue window_proxy_new_self(JSContext *ctx, uint32_t doc, const char *name, OpenerPolicyValue opener_policy,
                              JSValueConst parent, SandboxFlags creation_sandbox_flags)
{
    /* THE ORIGIN IS THE AGENT'S, and it is read from where §7.2.1's check reads it rather than passed in —
       an agent is origin-keyed, so a caller-supplied one could only ever agree or be wrong. */
    /* THE NAVIGABLE THE INSTANCE STARTS IN IS NOT A POPUP: no `open()` created it, so §7.4 decided
       nothing about it and its chrome is whole. */
    /* NO CREATOR'S POLICY EITHER, for the same reason: nothing here created this navigable, so there is no
       container to clone. Its document's policy is the one it was installed with — the host's response. */
    /* THE REALM IS ALREADY BUILT, SO ITS ENVIRONMENT ALREADY HAS THE ANSWER — and it is read from there rather
       than passed in, for the same reason the origin is: a caller-supplied one could only ever agree or be
       wrong. This is the navigable the HOST started the instance in, so the top-level creation URL its
       documents were created under is the one the host handed realm_install_intrinsics. */
    JSValue tlu = realm_top_level_creation_url(ctx);
    const char *tlus = JS_ToCString(ctx, tlu);
    const Origin *tlo;
    UrlRecord rec;
    JSValue obj;
    ProxyData *p;

    CHECK(tlus != NULL, "the realm's top-level creation URL would not convert to a C string");
    /* §8.1.3.1's TOP-LEVEL ORIGIN FOR THE ONE NAVIGABLE NO §7.3.2.1 RAN FOR — the instance's root, whose
       environment the HOST built. The URL it built it with is above; the ORIGIN is §7.3.2.1 "Creating
       browsing contexts"' determine the origin over that URL with THIS AGENT'S origin as the source origin,
       which is the whole of what an origin-keyed agent cluster has to inherit from. That is not a
       re-derivation dressed up: determine the origin IS the algorithm that answers "whose origin is a
       Document at this address", and its two inheritance cases are exactly the addresses §4.7 cannot answer
       for — `about:blank`, which is the address §7.3.2.1 creates EVERY top-level browsing context at, and
       `about:srcdoc`. A host that started this instance at a real
       address gets §4.7's tuple, which is the same origin as this agent's by construction. */
    url_record_init(&rec);
    tlo = url_parse(&rec, tlus, strlen(tlus), NULL) ? origin_determine(&rec, false, origin_agent())
                                                    : origin_determine(NULL, false, NULL);
    url_record_free(&rec);
    /* THE ONE ANSWER §7.3.2.1'S DETERMINE THE ORIGIN CANNOT REACH FROM A URL, named where it would otherwise
       be silent. A `data:` or `file:` top-level creation URL takes its step 5 to §4.7, which MINTS a new
       opaque origin — and an opaque origin that is not this agent's own record is same origin with nothing,
       so every environment of this instance would be keyed to a value no other environment can ever equal.
       Which Document that origin belongs to is a fact only the zone that created this instance holds. */
    DCHECK(!origin_is_opaque(tlo) || origin_same(tlo, origin_agent()),
           "this instance's root environment has a TOP-LEVEL CREATION URL whose §4.7 origin is OPAQUE and is "
           "not this agent's own, so §7.3.2.1's determine the origin minted a second one rather than "
           "inheriting: an opaque origin has no serialization it can be recreated from, and a URL therefore "
           "cannot say WHOSE it is. STATE "
           "§8.1.3.1's TOP-LEVEL ORIGIN beside the top-level creation URL from the zone that created this "
           "instance — it is the same statement, made by the same party, for the same reason");
    /* §7.1.5's CREATION SANDBOXING FLAG SET, STATED BY THE HOST AND NEVER ASSUMED HERE — for the parent's
       reason one line along, and it USED TO BE A ZERO. The zero was two claims wearing one value. For a
       TOP-LEVEL root it is the standard's own answer: determine-the-creation-sandboxing-flags gives a browsing
       context with no embedder its POPUP SANDBOXING FLAG SET, which §7.1.5 says is empty at creation and which
       only §7.3.1.7 "Navigable target names"'s rules for choosing a navigable ever populate, and nothing chose
       this one. For a root that is a CHILD NAVIGABLE it was not an answer at all — it held only because
       core/frame/navigable.c REFUSED to announce a cross-instance child whose creation flags were non-empty.
       The notice carries the set now, so both roots state it: the empty one in §7.1.5's own word for the empty
       set, the sandboxed one as its flags. What the ROOT document's own `Content-Security-Policy: sandbox`
       adds is the other half of §7.4.5's union, and it is added where a Document is created rather than here. */
    /* NO POLICIES TO CLONE — this navigable is the one §7.4 did not create IN THIS AGENT, so there is no
       creator here whose container to clone — but there IS a container, and CSP §2.2's SELF-ORIGIN of its CSP
       list is this agent's own: the root Document is created from the response at this instance's address,
       which is §2.2.2's answer. Nothing reads it through this proxy (its realm exists from the moment it is
       adopted, so proxy_realm's lazy materialization is unreachable for it), and it is stated all the same
       because the field is what makes "every navigable carries the container its Document runs under" an
       invariant rather than a case analysis at each reader.
       A CROSS-INSTANCE CHILD DOES HAVE A CREATOR AND ITS CLONE DOES NOT COME THROUGH HERE. The `navigable.create`
       notice carries §7.1.7's clone and the HOST hands it to §7.1.7's own determine-navigation-params-policy-
       container beside the response's container, so the Document is created with the right one; this field
       would be that clone's second copy, and a second copy of a container is a second answer waiting to be read
       by the lazy materialization that cannot reach this navigable anyway. */
    /* NO CREATOR BASE URL either, and for the same sentence: §7.4 did not create this navigable, so there is
       no creator whose base URL to pass on. Its Document comes from the response at this instance's address,
       which §2.4.3 gives a null about base URL. */
    /* AND §7.1.4's ITEM IS "A NEW EMBEDDER POLICY" HERE, WHICH IS THE STANDARD'S ANSWER AND NOT A DEFAULT.
       This field is the CREATOR's clone and this container is the one built without a creator in THIS agent —
       §7.1.7 gives such a container an embedder policy "initially a new embedder policy", which is what this
       states, and the creator's own item reaches the Document through the host's container as the paragraph
       above describes rather than through a second copy here. The ROOT
       DOCUMENT's own embedder policy is a different fact and comes from a different place: it is obtained from
       the response this instance was started with and reaches the Document through the container
       document_install is handed, never through this proxy (whose realm exists from the moment it is adopted,
       so proxy_realm's lazy materialization is unreachable for it). */
    /* §7.3.1.3's PARENT, STATED BY THE HOST AND NEVER ASSUMED HERE. This mint is reached for the navigable an
       instance STARTED in, and that navigable is a top-level traversable only when the zone that provisioned
       the instance says so: a cross-origin child navigable is provisioned as a peer instance of its own, and
       its root is a CHILD navigable presented by an element in another heap. The value is checked rather than
       trusted, because the two absences are spelled differently one slot apart — §7.2.2.4 makes a top-level
       navigable's `parent` itself (JS_UNDEFINED in the record) and its `opener` null (JS_NULL beside it), and a
       JS_NULL arriving here would make window_proxy_parent_navigable answer with something that is not a
       navigable and is not the absence either. */
    DCHECK(JS_IsUndefined(parent) || window_proxy_is(parent),
           "a navigable was rooted with a §7.3.1.3 PARENT that is not a navigable — the section defines a child "
           "navigable as one whose parent is non-null, so the only two answers are a WindowProxy (this "
           "instance's own, or a remote one for a parent another instance holds) and JS_UNDEFINED, which is the "
           "positive statement that this navigable is a top-level traversable");
    obj = window_proxy_new(ctx, doc, NULL, origin_agent(), name, false, creation_sandbox_flags, opener_policy,
                           serialized_policy_container(NULL, origin_serialized(origin_agent()),
                                                       serialized_embedder_policy_new(),
                                                       /* a top-level navigable created from NO response
                                                          states no integrity policy, which SRI §3.8 answers
                                                          with "a new integrity policy" */
                                                       NULL),
                           NULL, tlus, tlo, parent, JS_NULL);
    JS_FreeCString(ctx, tlus);
    JS_FreeValue(ctx, tlu);

    if (JS_IsException(obj)) return obj;
    p = JS_GetOpaque(obj, g_proxy_class);
    DCHECK(p != NULL, "the self WindowProxy was minted without its record");
    /* THE ONE NAVIGABLE §7.4 DID NOT CREATE. A NULL name is the host saying it does not know this navigable's
       name, which is the honest answer for a page the browser navigated to — see the field's comment. */
    p->name_known = name != NULL;
    proxy_adopt_realm(ctx, obj, ctx);
    return obj;
}


/* A proxy over a navigable whose active document lives in ANOTHER WASM instance. STATIC, and reached only
   through window_proxy_for_document below: minting one is exactly where a navigable's identity can be lost,
   so there is one site that does it and it records what it made. */
static JSValue window_proxy_new_remote(JSContext *ctx, uint32_t doc, const Origin *origin, const char *name,
                                       JSValueConst parent, JSValueConst opener)
{
    JSValue obj;
    ProxyData *p;

    DCHECK(g_wp_rt != NULL, "a WindowProxy was minted before window_proxy_init ran");
    /* A REMOTE PROXY THAT NAMES THIS DOCUMENT is a local one that lost its Window — it would resolve to
       JS_UNDEFINED for a navigable this instance can actually answer for. */
    DCHECK(doc != 0 && !world_doc_hosted(doc),
           "a remote WindowProxy was minted for a document whose realm this agent holds — use window_proxy_new, "
           "which carries the live Window");
    obj = JS_NewObjectClass(ctx, g_proxy_class);
    if (JS_IsException(obj)) return obj;
    p = calloc(1, sizeof *p);
    CHECK(p != NULL, "window proxy: OOM building a remote WindowProxy");
    p->window = JS_UNDEFINED;   /* it lives in another instance; there is nothing local to hold */
    DCHECK(origin != NULL, "a remote navigable was minted with no origin — the peer's principal is what every "
                           "cross-origin filter over this proxy compares, and it is stated by the trusted zone");
    p->origin = origin;   /* BORROWED, and never same origin with this agent's — see origin_parse */
    p->name   = name && *name ? proxy_strdup(name) : NULL;
    p->name_known = 1;   /* §7.4 named it, in this instance, before the notice crossed */
    p->parent = JS_DupValue(ctx, parent);
    p->opener = JS_DupValue(ctx, opener);
    p->container = JS_NULL;   /* §7.3.1.3's link is written by the create, exactly as it is for a local one */
    p->doc = doc;
    JS_SetOpaque(obj, p);
    return obj;
}

/* THE ROW THAT MAKES A PEER'S NAVIGABLE ONE OBJECT — "this instance's proxy for document `doc`". It exists
   because identity is what a page tests: without the row the next ask MINTS a second proxy and
   `w.frames[0] === iframe.contentWindow` is false about one window.
   ONE CALLER, AND THE ASSERTS ARE WHY THIS IS A FUNCTION. An append that is three statements does not need
   extracting; two INVARIANTS about the table do, because they hold for every row however it got there. A row
   for a document this agent HOSTS is a second answer to a question the document's own realm already answers,
   and a SECOND row for one document makes `w[0] === w[0]` decide by which row the scan reached first — both
   are properties of the table, so they are asserted where a row is made rather than where a caller happens
   to be. A future operation that also records a row (moving a proxy this instance held onto a peer's
   document is the obvious one) inherits them by calling this; it is not described here, because CLAUDE.md
   §Fix the ROOT is explicit that a comment is not a valid follow-up — such an operation must be built, and
   until it is, this file must not read as though it exists. */
static void remote_nav_record(JSValueConst proxy, uint32_t doc)
{
    int i;

    DCHECK(!world_doc_hosted(doc),
           "a remote-navigable row was recorded for a document whose realm this agent HOLDS — the row is what "
           "window_proxy_of_document answers a peer's identity from, and a hosted document is answered by its "
           "own realm instead, so a row for one is a second answer to a question that already has one");
    for (i = 0; i < g_remote_navs_n; i++)
        DCHECK(g_remote_navs[i].doc != doc,
               "a SECOND remote-navigable row was recorded for one document — the table is a map from a peer's "
               "document to the one proxy that resolves it, so two rows make `w[0] === w[0]` decide by which "
               "row the scan reached first");
    if (g_remote_navs_n == g_remote_navs_cap) {
        int cap = g_remote_navs_cap ? g_remote_navs_cap * 2 : 8;
        RemoteNav *g = realloc(g_remote_navs, (size_t)cap * sizeof *g);

        CHECK(g != NULL, "window proxy: OOM recording a remote navigable's identity — an unrecorded navigable "
                         "is minted again on the next ask, and `w[0] === w[0]` is then false about one window");
        g_remote_navs = g;
        g_remote_navs_cap = cap;
    }
    g_remote_navs[g_remote_navs_n].doc = doc;
    g_remote_navs[g_remote_navs_n].proxy = proxy;   /* BORROWED — proxy_finalizer takes the row out */
    g_remote_navs_n++;
}

JSValue window_proxy_of_document(JSContext *ctx, uint32_t doc)
{
    int i;

    DCHECK(doc != 0, "the WindowProxy of document zero was asked for — zero is the world registry's NONE, so "
                     "the caller is holding a handle it never resolved rather than a document");
    /* A DOCUMENT THIS AGENT HOSTS IS ANSWERED BY ITS OWN REALM, never out of the table below: the realm holds
       the one proxy for its navigable (core/dom/document.h), and a row here for a hosted document would be a
       second answer to a question that already has one. */
    if (world_doc_hosted(doc)) {
        JSContext *realm = world_doc_realm(doc);

        DCHECK(realm != NULL,
               "the WindowProxy of a document THIS AGENT HOSTS was asked for before that document's realm was "
               "materialized. The navigable exists — whatever created it holds its proxy — but a hosted "
               "navigable has no row here, so there is nowhere else to answer from. A peer can only name a "
               "document it has a reference into, and a reference into a document whose realm was never built "
               "cannot have been lent, so a name reaching this state came from somewhere that is not a lend");
        if (!realm) return JS_UNDEFINED;
        return JS_DupValue(ctx, document_window_proxy(realm));
    }
    for (i = 0; i < g_remote_navs_n; i++)
        if (g_remote_navs[i].doc == doc) return JS_DupValue(ctx, g_remote_navs[i].proxy);
    return JS_UNDEFINED;
}

JSValue window_proxy_for_document(JSContext *ctx, uint32_t doc, const Origin *origin, const char *name,
                                  JSValueConst parent, JSValueConst opener)
{
    JSValue held = window_proxy_of_document(ctx, doc);
    JSValue obj;

    if (!JS_IsUndefined(held)) return held;
    DCHECK(!world_doc_hosted(doc),
           "a REMOTE WindowProxy was about to be minted for a document whose realm this agent holds — the "
           "lookup above answers for every hosted document, so reaching here means it could not, and minting "
           "would give one navigable a second proxy that resolves nothing");
    obj = window_proxy_new_remote(ctx, doc, origin, name, parent, opener);
    if (JS_IsException(obj)) return obj;
    remote_nav_record(obj, doc);
    return obj;
}

/* §7.2.2.1's `closed` AS THIS AGENT'S RECORD OF IT, read and written through the one place it lives HERE. It
   is a fact about the NAVIGABLE, so `window.closed` and `iframe.contentWindow.closed` must be the same answer
   — they were two bytes, and closing through one left the other reporting open. Captured through proxy_of, so
   the flow that closed the window is the only one whose timeline contains it.
   THIS AGENT'S, AND THE CALLERS ARE WHY THAT IS THE RIGHT QUESTION. Every one of them is an ENGINE WALK over
   navigables this agent holds — §7.3.3 "Fully active documents"' ancestor chain, §7.1's named-target
   search, the tree-order walk — and each is asking whether to keep walking THIS agent's tree, which is
   exactly what this record answers. The JS-visible member is a different question and has a different answer
   path: a traversable whose active document is in another instance is closed by whichever agent ran
   §7.2.2.1's close(), so the
   member suspends and asks that one (proxy_get_step). A caller that wants the standard's `closed` about an
   arbitrary navigable must go through the member, because only the member can suspend.
   IT IS AN OR OVER TWO FLAGS, which is the getter's own wording rather than a convenience: the two are set by
   two different algorithms at two different times, and every read in this file goes through here so that no
   member can accidentally ask only one of them. */
bool window_proxy_closed(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);

    DCHECK(p != NULL, "the closed state of something that is not a WindowProxy was read");
    /* §7.2.2.1's OR, over the FACT rather than over the two recorded flags — a frame removed from the tree has
       a null browsing context from that line onward, which is the disjunct wp_bc_null cannot see (see its
       definition). `closing` stays a flag: it is written by close() in whichever agent ran it. */
    return p->closing != 0 || window_proxy_browsing_context_null(ctx, proxy);
}

/* §7.5.10 STEPS 8 AND 9 — the completion of a destruction, written only by the destroy job. See window_proxy.h
   for why the two steps are one write and why the second of them is this engine's only reclamation edge.
   Step 8 is also what §7.5.10 step 5's wait reads off each child, which is why it is asked separately from
   `closed`: a navigable whose top-level traversable is merely CLOSING has not been destroyed, and a wait that
   accepted `closed` would finish before its subtree had. */
void window_proxy_set_destroyed(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);   /* the capture is in the accessor — the WHOLE binding rides the delta */

    DCHECK(p != NULL, "something that is not a WindowProxy had its browsing context set to null");
    /* ONCE PER DOCUMENT, ASSERTED, because step 9 hands a reference back and a second run would hand back one
       it no longer holds. Every path that could reach here twice already refuses to: descend_enqueue returns
       for a destroyed navigable, the fan-out reports a destroyed child instead of queuing it, and §7.3's
       close chains unload-then-destroy over a subtree whose members destroyed themselves at §7.5.9 step 20.
       So a second arrival is one of those paths having stopped refusing, and it is silent without this. */
    DCHECK(!p->destroyed,
           "§7.5.10 destroyed one navigable's active document TWICE — the second run releases a Window "
           "reference this record has already given back, and the first thing that reads the navigable "
           "afterwards reads a freed global");
    /* A PEER'S NAVIGABLE HAS NOTHING LOCAL TO RELEASE, and it is not destroyed from here either: the agent that
       HOLDS the document runs §7.5.10 over it, and this side learns the outcome the way it learns `closed`. */
    DCHECK(world_doc_hosted(p->doc),
           "§7.5.10 was run over a navigable whose ACTIVE DOCUMENT is in another WASM instance — the ports, "
           "the queued tasks and the Window are all the peer's, so the destruction is the peer's to perform "
           "and this side has nothing to null");
    p->destroyed = 1;                                          /* step 8 */
    /* STEP 9 — the navigable stops naming the Document. THE DELTA ALREADY HOLDS ITS OWN DUP of this Window
       (the capture above ran before the write), so releasing the live slot leaves every parked arm's copy
       intact and rewinding this flow restores the binding exactly. That is the same argument
       window_proxy_navigate makes one screen up, for the same field, and it is the whole reason a destruction
       can give memory back at all without a flow losing the document it is standing in. */
    wp_set(ctx, p, &p->window, JS_UNDEFINED);
    p->realm  = NULL;
    /* AND `materialized` NOW ANSWERS THE TRUTH: this navigable has no active document. proxy_realm asserts the
       other side of that pair — a NULL realm on a DESTROYED navigable means "there is none", never "not yet". */
    DCHECK(p->realm == NULL && JS_IsUndefined(p->window),
           "§7.5.10 step 9 left a destroyed navigable still naming a Document — the realm behind it can then "
           "never be reclaimed, because this reference is the one the collector cannot get past");
}

/* §7.1.3.2 STEP 10's DISCARD — the OTHER writer of the state §7.2.2 "The Window object" spells
   "if this's browsing context is null then return null", and it releases nothing.
   §7.5.10's write above is a destruction and hands the Window back; this one records that a browsing context
   was LEFT BEHIND by a group swap while its Document goes on existing exactly as it was. That asymmetry is the
   spec's, not a shortcut: step 10's note says only that the user agent "might destroy it at this point", and
   this engine keeps a superseded document alive on every navigation because a flow parked inside it resumes
   there.
   PER FLOW through proxy_of, like every other write to this record: a sibling arm whose response carried no
   such policy never swapped, and its handle stays open. */
void window_proxy_discard_browsing_context(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);   /* the capture is in the accessor — the WHOLE binding rides the delta */
    (void)ctx;

    DCHECK(p != NULL, "something that is not a WindowProxy had its browsing context discarded");
    DCHECK(!p->destroyed,
           "§7.1.3.2's group swap discarded the browsing context of a navigable §7.5.10 had already destroyed — "
           "a destroyed navigable has no active document to navigate, so the load that reached the swap was "
           "running over a document that no longer exists");
    DCHECK(!p->bc_discarded,
           "§7.1.3.2's group swap discarded one browsing context TWICE — one navigation swaps once, and the "
           "second announcement provisions a second instance for a document that already has one");
    p->bc_discarded = 1;
    /* AND §7.2.2.1's `closed` READS IT — asserted here because this byte and that getter are the whole contract
       between the swap and the page. A `closed` that did not include this disjunct would leave the opener's own
       handle reporting a window real Chrome has already cut off as one it can still reach, and §@S would emit a
       breakout the browser forbids: a byte written where nothing reads it, which is silent everywhere else. */
    DCHECK(window_proxy_closed(ctx, proxy),
           "a browsing context §7.1.3.2's swap has just discarded still answers §7.2.2.1's `closed` as false — "
           "the getter's \"this's browsing context is null\" has THREE writers (this swap, §7.5.10's destroy "
           "and §7.3.1.6's synchronous sever) and is reading fewer than all of them");
}

/* THE WAIT'S STEP 3 — the number of child navigables this one is waiting on for this operation, recorded
   BEFORE any of them is queued. Through proxy_of, so the count belongs to the flow performing the walk. */
void window_proxy_child_wait_set(JSContext *ctx, JSValueConst proxy, int op, uint32_t n)
{
    ProxyData *p = proxy_of(proxy);
    (void)ctx;
    DCHECK(p != NULL, "something that is not a WindowProxy was given a subtree-operation count");
    DCHECK(op >= 0 && op < WP_SUBTREE_OP_N,
           "a subtree operation outside the WindowProxy record's capacity reported a count — widen "
           "WP_SUBTREE_OP_N with the operation");
    DCHECK(p->child_wait[op] == 0,
           "a navigable's outstanding child count for one operation was set while it already had one — two "
           "walks of the SAME kind over the same navigable are in flight, and the second one's children will "
           "report into the first one's wait");
    p->child_wait[op] = n;
}

/* A CHILD REPORTED — the wait's incrementDestroyed/incrementUnloaded/completedTasks, counted down. Answers
   WP_WAIT_LAST for the last child (the wait being over, and the only moment the waiting navigable's own body
   may be queued), WP_WAIT_MORE while siblings are outstanding, and WP_WAIT_NONE when this navigable is not
   running that operation at all — which is how the reporter learns it is the ROOT of the operation rather than
   underflowing a count that was never raised. */
int window_proxy_child_wait_report(JSContext *ctx, JSValueConst proxy, int op)
{
    ProxyData *p = proxy_of(proxy);
    (void)ctx;
    DCHECK(p != NULL, "something that is not a WindowProxy was told a child navigable had finished");
    DCHECK(op >= 0 && op < WP_SUBTREE_OP_N,
           "a subtree operation outside the WindowProxy record's capacity reported a child — widen "
           "WP_SUBTREE_OP_N with the operation");
    if (p->child_wait[op] == 0) return WP_WAIT_NONE;
    return --p->child_wait[op] == 0 ? WP_WAIT_LAST : WP_WAIT_MORE;
}

/* §7.2.2.1 step 3's and §7.3's step 1's test — IS CLOSING alone, which is the half of `closed` that says a
   close is under way rather than finished. Through proxy_of like every other read of this record: the flow
   that called close() is the only one whose timeline contains it. */
bool window_proxy_closing(JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);
    DCHECK(p != NULL, "the is-closing state of something that is not a WindowProxy was read");
    return p->closing != 0;
}

/* §7.3.2.2's FAMILIAR WITH — see window_proxy.h. A is the INCUMBENT realm's browsing context, which is this
   agent's, so every "A's active document's origin" below is origin_agent() and every "is A" is this realm's
   own Window or its proxy. */
static bool proxy_is_incumbent_window(JSContext *ctx, JSValueConst v)
{
    JSValue g = JS_GetGlobalObject(ctx);
    bool same = JS_VALUE_GET_PTR(g) == JS_VALUE_GET_PTR(v);

    JS_FreeValue(ctx, g);
    return same || JS_VALUE_GET_PTR(v) == JS_VALUE_GET_PTR(document_window_proxy(ctx));
}

/* IS THIS LINK OF A NAVIGABLE CHAIN SAME ORIGIN WITH THE INCUMBENT? A link is either another navigable's proxy
   or this agent's own Window, which is where the chain ENDS — window_proxy_new is handed the creator's Window
   as a child or auxiliary navigable's parent. An agent is ORIGIN-KEYED, so its Window's origin IS the
   incumbent's, and §7.1.1 step 1 then answers the end of the chain TRUE whatever that origin is: an origin is
   same origin with itself, opaque included, which is why this is `true` and not a test against "null". */
static bool link_same_origin_as_incumbent(JSContext *ctx, JSValueConst v)
{
    ProxyData *q = JS_GetOpaque(v, g_proxy_class);

    if (q) return proxy_same_origin(q);
    return proxy_is_incumbent_window(ctx, v);
}

bool window_proxy_familiar_with(JSContext *ctx, JSValueConst proxy)
{
    JSValue b = JS_DupValue(ctx, proxy);
    bool ok = false;

    DCHECK(window_proxy_is(proxy), "§7.3.2.2 was asked about something that is not a navigable's WindowProxy");
    /* THE LOOP IS STEP 3's RECURSION, UNROLLED — "B is an auxiliary browsing context and A is familiar with
       B's opener browsing context" is the same question about a different B, and an opener link is fixed when
       the navigable is created (§7.2.2.4's setter only ever SEVERS one), so the chain is finite and acyclic:
       every opener predates the navigable it opened. */
    for (;;) {
        ProxyData *p = JS_GetOpaque(b, g_proxy_class);
        JSValue anc;

        if (link_same_origin_as_incumbent(ctx, b)) { ok = true; break; }    /* step 1 */
        /* Step 3's chain has reached a link that is not a navigable's proxy, which is this agent's own Window
           — A itself, and step 1 has just answered for it. */
        if (!p) break;
        {                                                                   /* step 2: A's top-level IS B */
            JSValue top = window_proxy_top_navigable(ctx, document_window_proxy(ctx));
            bool is_b = JS_VALUE_GET_PTR(top) == JS_VALUE_GET_PTR(b);
            JS_FreeValue(ctx, top);
            if (is_b) { ok = true; break; }
        }
        /* Step 4: an ANCESTOR browsing context of B whose active document is same origin with A's — which
           includes the case where A is that ancestor, since the chain ends at this agent's Window. */
        anc = JS_DupValue(ctx, p->parent);
        while (!JS_IsUndefined(anc)) {
            ProxyData *q = JS_GetOpaque(anc, g_proxy_class);
            JSValue up;

            if (link_same_origin_as_incumbent(ctx, anc)) { ok = true; break; }
            if (!q) break;                       /* the chain's end, and it was not same origin */
            up = JS_DupValue(ctx, q->parent);
            JS_FreeValue(ctx, anc);
            anc = up;
        }
        JS_FreeValue(ctx, anc);
        if (ok) break;
        if (JS_IsNull(p->opener) || JS_IsUndefined(p->opener)) break;       /* B is not auxiliary */
        {
            JSValue o = JS_DupValue(ctx, p->opener);
            JS_FreeValue(ctx, b);
            b = o;                                                          /* step 3 */
        }
    }
    JS_FreeValue(ctx, b);
    return ok;
}

bool window_proxy_destroyed(JSValueConst proxy)
{
    /* THROUGH proxy_of, LIKE EVERY OTHER READ OF THIS RECORD — the destruction of a child is written into the
       destroying FLOW's delta, so a wait that read the baseline would never see the child it had just
       destroyed and would park for ever. A read that reaches a record captures it; that is the contract, and
       reading around it here would make the wait depend on which flow happened to run the child's job. */
    ProxyData *p = proxy_of(proxy);
    DCHECK(p != NULL, "the destroyed state of something that is not a WindowProxy was read");
    return p->destroyed != 0;
}

/* IS THIS RECORD'S REALM BORROW DANGLING — see window_proxy.h for the question and for why it is neither
   `destroyed` nor a plain "does it still name this realm". CAPTURE-FREE AND COLLECTION-SAFE, which is the
   whole reason it is a second entry point and not a caller of the accessor one line up.
   NOT proxy_of, AND THAT IS THE SAME DISTINCTION THE COLLECTOR ENTRIES ALREADY DRAW. proxy_of captures the
   record into the running flow's delta, because a record a flow has REACHED is one it may write — and there is
   no flow here at all: the caller is quickjs's realm-teardown hook, which fires from inside a collection, where
   a capture would dup this record's JSValues onto an object being torn down. proxy_finalizer and proxy_gc_mark
   are excluded from the capture for exactly that reason; this is the third caller with the same standing, and
   the read it makes is strictly narrower than theirs — two field loads and no reference touched at all. */
bool window_proxy_realm_dangling(JSValueConst proxy, JSContext *realm)
{
    JSClassID cid = 0;
    /* proxy_collector_record's CALL WITH ITS SECOND OUTPUT KEPT. The collector may discard the class id
       because it was DISPATCHED through the class and therefore already knows what it is holding; this caller
       was handed a value and does not, so the id is the only thing that can say. It is JS_GetAnyOpaque and not
       JS_GetOpaque(proxy, g_proxy_class) for the reason stated at proxy_collector_record — window_proxy_free
       gives the class id back, and the collection that reaches a realm teardown runs AFTER the release column
       (core/dom/document.c states it: a child navigable's document is released from that hook, "inside
       JS_RunGC or inside JS_FreeRuntime, and BOTH of those are after platform_agent_free"). */
    const ProxyData *p = JS_GetAnyOpaque(proxy, &cid);

    DCHECK(realm != NULL, "a WindowProxy was asked whether it dangles at NO realm — the question is asked at "
                          "the teardown of one particular JSContext and is meaningless without it");
    /* AND THE CLASS IS CHECKED WHERE THERE IS STILL A CLASS TO CHECK AGAINST. `g_wp_rt == NULL` is not an
       exemption of convenience: window_proxy_free zeroes g_proxy_class by design, so after the release column
       a LIVE proxy carries an id this component can no longer name, and comparing against zero would report
       every one of them as something else. What stands in for the comparison there is the caller's own
       argument — core/frame/navigable.c asks this of document_window_proxy(cctx), which core/dom/document.c
       asserts is that realm's own §7.2.3 WindowProxy. A zero id with no record is the collector having already
       finalized the proxy, which free_object writes as one pair (u.opaque and class_id are cleared together
       after the finalizer returns). */
    DCHECK(g_wp_rt == NULL || cid == g_proxy_class || (cid == 0 && p == NULL),
           "the realm-borrow question was asked of an object that is not a §7.2.3 WindowProxy — it is answered "
           "by reading a ProxyData off the class opaque, so a foreign class's record would be read with this "
           "record's layout and the answer would be two fields of somebody else's struct");
    /* THE COLLECTOR ALREADY TOOK THE RECORD — a POSITIVE ANSWER and not a hole a default fills. proxy_finalizer
       frees a ProxyData whole, so a proxy with none holds no binding at all and no realm can be read through
       it; §7.5.10's own note is why this is reachable rather than defensive ("Even after destruction, the
       Document object itself might still be accessible to script, in the case where we are destroying a child
       navigable"), so the proxy and the realm are two objects one collection may take in either order. */
    if (p == NULL)
        return false;
    /* THE TAG OF `window` AND NEVER ITS TARGET. The Window this slot names may itself have been finalized
       earlier in the same sweep, so JS_IsUndefined — which reads the JSValue's own tag — is the whole of what
       may be asked of it here. */
    return p->realm == realm && JS_IsUndefined(p->window);
}

bool window_proxy_browsing_context_null(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);   /* through the accessor, for window_proxy_destroyed's reason */

    DCHECK(p != NULL, "the browsing context of something that is not a WindowProxy was asked about");
    /* THE THIRD WRITER, AND THE ONLY ONE THAT IS SYNCHRONOUS. Both flags are set by §7.5.10's queued
       destruction, and every page that observes this observes it on the line after `iframe.remove()` —
       §7.3.1.6 step 3's sever is what has happened by then. `embedded-opener-remove-frame.html` states the
       difference in its own structure: it checks a removed FRAME's `opener` immediately and a closed POPUP's
       behind a `step_timeout`, because the first is discarded synchronously and the second from a task. */
    return window_proxy_navigable_null(ctx, proxy) || wp_bc_null(p);
}

void window_proxy_disown_opener(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);   /* the capture is in the accessor — this write rides the flow's delta */

    DCHECK(p != NULL, "something that is not a WindowProxy was asked to disown its opener");
    wp_set(ctx, p, &p->opener, JS_NULL);
}

void window_proxy_set_opener(JSContext *ctx, JSValueConst proxy, JSValueConst opener)
{
    ProxyData *p = proxy_of(proxy);   /* the capture is in the accessor — this write rides the flow's delta */

    DCHECK(p != NULL, "something that is not a WindowProxy was given an opener");
    /* §7.2.2.4's `opener` IS A NAVIGABLE'S, so what is stored is the opener's PROXY and never its Window. The
       two are not interchangeable here and the difference is exactly the bug window_proxy_new's own comment
       records: a Window in this slot makes the `top` walk stop at the proxies and read a scriptable property
       to continue, and makes a popup's `opener` a Window rather than the navigable it belongs to. */
    DCHECK(window_proxy_is(opener),
           "§7.2.2.1 step 16.2 was given something that is not a WindowProxy as the opener browsing context — "
           "the step says \"sourceDocument's browsing context\", which is a NAVIGABLE, and every other reader "
           "of this slot (§7.2.2.4's `opener`, window_proxy_opener_navigable, the familiarity walk) is written "
           "against a proxy");
    /* NULL IS NOT THIS FUNCTION'S TO SAY. §7.2.2.4's null branch DISOWNS, which is a different state change
       with a different observable (no own property is defined), and it has its own entry point above. Reaching
       this one with null would make the two spellings of "no opener" two, able to disagree. */
    wp_set(ctx, p, &p->opener, JS_DupValue(ctx, opener));
}

bool window_proxy_materialized(JSValueConst proxy)
{
    ProxyData *p = JS_GetOpaque(proxy, g_proxy_class);
    DCHECK(p != NULL, "something that is not a WindowProxy was asked whether its document is materialized");
    return p->realm != NULL;
}

bool window_proxy_is_remote(JSValueConst proxy)
{
    ProxyData *p = JS_GetOpaque(proxy, g_proxy_class);
    DCHECK(p != NULL, "something that is not a WindowProxy was asked whether it is remote");
    /* THE TWO FACTS MUST AGREE. Read together on every ask, because a proxy where they disagree hands a
       cross-document read this document's Window and nothing downstream can tell. */
    /* A PROXY OVER A PEER'S DOCUMENT MUST NEVER HAVE BUILT A REALM — a realm here would mean this agent
       answered for a document it does not hold, which is the exact failure the same-origin check prevents. The
       converse is NOT asserted: a hosted navigable legitimately has no realm until something reads through it. */
    DCHECK(world_doc_hosted(p->doc) || (p->realm == NULL && JS_IsUndefined(p->window)),
           "a WindowProxy over a PEER's document carries a local realm — a cross-instance read would then be "
           "answered out of this heap");
    return !world_doc_hosted(p->doc);
}

uint32_t window_proxy_doc(JSValueConst proxy)
{
    ProxyData *p = JS_GetOpaque(proxy, g_proxy_class);
    DCHECK(p != NULL, "the document of something that is not a WindowProxy was asked for");
    return p->doc;
}

JSValue window_proxy_window(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);
    DCHECK(p != NULL, "the Window of something that is not a WindowProxy was asked for");
    /* THE SEAM, and it crashes rather than answering. A proxy whose navigable lives in another WASM instance
       has no local Window to hand back, and handing back this document's would be a cross-document read that
       silently succeeded — the one failure mode the same-origin check exists to prevent. What has to be built
       is named in the message: the binding is (document, world), and resolving it is a host round trip because
       only the host knows which instance holds that document and which of its flows is in that world. */
    DCHECK(world_doc_hosted(p->doc),
           "a WindowProxy names a navigable in ANOTHER WASM instance — build the cross-instance resolve: the "
           "flow SUSPENDS here (the same snapshot path as an await), the peer answers in its own scheduled turn "
           "under this flow's world, and the flow resumes with the value. The host owns the routing because "
           "only it knows which instance holds that document — window_proxy_doc names which one");
    /* JS_GetGlobalObject ALREADY RETURNS A REFERENCE. Duplicating it took a second one and returned only one,
       so every read through here leaked a Window — and with it, since a global roots a whole realm, the entire
       child page: ~4000 objects surviving JS_FreeRuntime's walk with a single external ref on the Window
       holding them. It went unseen while the only realm was the root's, because the root's global is freed
       with the runtime whatever its count; it surfaced the moment §7.4 materialized a second one. */
    return JS_GetGlobalObject(proxy_realm(ctx, proxy, p));
}

JSContext *window_proxy_realm(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);
    DCHECK(p != NULL, "the realm of something that is not a WindowProxy was asked for");
    return proxy_realm(ctx, proxy, p);
}

bool window_proxy_same_origin_of(JSValueConst proxy)
{
    ProxyData *p = JS_GetOpaque(proxy, g_proxy_class);
    DCHECK(p != NULL, "the origin of something that is not a WindowProxy was compared");
    return proxy_same_origin(p);
}

bool window_proxy_same_origin_domain_of(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = JS_GetOpaque(proxy, g_proxy_class);
    DCHECK(p != NULL, "the origin of something that is not a WindowProxy was compared");
    return proxy_same_origin_domain(ctx, p);
}

bool window_proxy_same_origin_with_top(JSContext *ctx)
{
    JSValue top = window_proxy_top_navigable(ctx, document_window_proxy(ctx));
    bool same = JS_IsObject(top) && window_proxy_same_origin_of(top);

    JS_FreeValue(ctx, top);
    return same;
}

const char *window_proxy_name(JSValueConst proxy)
{
    ProxyData *p = JS_GetOpaque(proxy, g_proxy_class);
    DCHECK(p != NULL, "the name of something that is not a WindowProxy was asked for");
    /* A DESTROYED navigable has no name — the same answer the `name` getter gives, from the same two fields,
       so named access cannot find a frame the page has already removed. */
    return wp_closed(p) || !p->name ? "" : p->name;
}

/* §7.2.2.1's `name`, AS A VALUE, and the ONE place it is computed. A Window and its WindowProxy are two spellings
   of one navigable, so `window.name` inside a document and `w.name` from its opener are one attribute of one
   record — they were two, computed from two unrelated sources: the proxy answered the navigable's name and the
   global answered a source-only concolic with no example, so `w = open(u,"chan42")` gave "chan42" through the
   proxy and an example-free unknown inside the popup. HTML §7.2.2.1 is unambiguous — "return
   this's navigable's target name" — and the popup's own script reading it is the ordinary case, not a second attribute.
   IT IS STILL AN ATTACKER SOURCE where the name is genuinely unknown, which is not "always": a navigable §7.4
   created carries the name §7.4 gave it, and the code that wrote `open(url, "chan42")` DETERMINED it, so
   reporting it as unknown would be inventing uncertainty the program does not have. The navigable nobody here
   created is the one whose name a cross-origin opener may have set, and that read is concolic — carrying the
   spec's "" as its example, so it forks control flow without losing the value. */
JSValue window_proxy_name_value(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);

    DCHECK(p != NULL, "the name of something that is not a WindowProxy was read");
    /* §7.2.2.1: a destroyed navigable has no name, and the destruction is what determined that — the spec files
       assert the empty string rather than the name it had. */
    if (wp_closed(p)) return JS_NewStringLen(ctx, "", 0);
    if (p->name_known) return JS_NewString(ctx, p->name ? p->name : "");
    return concolic_new(ctx, "{window.name}", "window.name", JS_NewStringLen(ctx, "", 0));
}

/* §7.2.2.1's `name` SETTER, and the ONE place it is written. It renames the BROWSING CONTEXT, which is why
   `frameW.name = "B"` leaves the element's `name` content attribute alone — the spec files assert that pair
   together, and an implementation that reflected one into the other would pass neither. Writing it is also what
   makes the name KNOWN: a page that set it determined it, whatever it was before. */
JSValue window_proxy_name_assign(JSContext *ctx, JSValueConst proxy, JSValueConst v)
{
    ProxyData *p = proxy_of(proxy);
    const char *n;

    DCHECK(p != NULL, "the name of something that is not a WindowProxy was written");
    if (wp_closed(p)) return JS_UNDEFINED;   /* a destroyed navigable has nothing to rename */
    /* A CONCOLIC NAME IS A CAPABILITY THAT DOES NOT EXIST YET, not a value to stringify. `name` is stored as C
       bytes because named access compares it, so a concolic here would have to be de-tainted to be stored —
       which is the one thing a concolic must never survive. Storing the triple instead is what to build. */
    DCHECK(!concolic_is(v),
           "a navigable was renamed to unknown external input — `name` is held as C bytes so named access can "
           "compare it, and a concolic cannot be stored there without de-tainting it; hold the navigable's name "
           "as a JSValue so the triple survives the write");
    n = JS_ToCString(ctx, v);
    if (!n) return JS_EXCEPTION;
    p->name = proxy_strdup(n);
    p->name_known = 1;
    JS_FreeCString(ctx, n);
    return JS_UNDEFINED;
}

/* §7.4's popup decision, read back — what §7.2.2.5's six BarProps answer from. A DESTROYED navigable keeps
   the answer it was created with: `closed` is what a page checks, and a bar that changed its mind on close
   would be a second fact where there is one. */
bool window_proxy_is_popup(JSValueConst proxy)
{
    ProxyData *p = JS_GetOpaque(proxy, g_proxy_class);
    DCHECK(p != NULL, "the popup state of something that is not a WindowProxy was asked for");
    return p->is_popup != 0;
}

const Origin *window_proxy_origin(JSValueConst proxy)
{
    ProxyData *p = JS_GetOpaque(proxy, g_proxy_class);
    DCHECK(p != NULL, "the origin of something that is not a WindowProxy was asked for");
    DCHECK(p->origin != NULL, "a WindowProxy carries no origin");
    return p->origin;
}

/* Through the capturing accessor rather than the raw opaque: this is per-flow state, and a flow that reads it
   is on its way to building a realm under it. */
const char *window_proxy_top_level_url(JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);
    DCHECK(p != NULL, "the top-level creation URL of something that is not a WindowProxy was asked for");
    return p->top_level_url;
}

/* Through the capturing accessor for the same reason: an arm that navigated a top-level traversable moved this
   field, and its sibling that did not must still read the origin its own world was created under. */
const Origin *window_proxy_top_level_origin(JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);
    DCHECK(p != NULL, "the top-level origin of something that is not a WindowProxy was asked for");
    DCHECK(p->top_level_origin != NULL,
           "the TOP-LEVEL ORIGIN of a navigable whose active document lives in a PEER instance was asked for — "
           "§8.1.3.1's field belongs to an ENVIRONMENT, this instance builds none for a remote navigable, and "
           "the instance that does is the one that answers Permissions §5.1 for its documents");
    return p->top_level_origin;
}

/* §7.1.5's creation sandboxing flags for this navigable — through the capturing accessor like every other
   field of the binding, so an arm that created the navigable and an arm that did not read their own. */
SandboxFlags window_proxy_creation_sandbox_flags(JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);

    DCHECK(p != NULL, "the creation sandboxing flags of something that is not a WindowProxy were asked for");
    /* A REMOTE PROXY HAS NONE TO GIVE, and answering the empty set would be worse than crashing: an empty set
       is a real answer meaning "not sandboxed", so a cross-origin `<iframe sandbox>` would report itself
       unsandboxed to whichever algorithm asked. §7.1.5's set belongs to the Document, the peer instance is
       what creates that Document, and this is a question to route there rather than to guess. */
    DCHECK(!window_proxy_is_remote(proxy),
           "the CREATION SANDBOXING FLAGS of a navigable whose Documents a PEER instance creates were asked "
           "for — §7.1.5's flag set is handed to a Document at its creation, this instance creates none for a "
           "remote navigable, and the notice that provisions the peer does not carry the set yet: add it "
           "beside the policy the `navigable.create` notice already sends (core/frame/navigable.c)");
    return p->creation_sandbox_flags;
}


/* §7.2.1's MEMBER SURFACE, and the one distinction that decides how each member is answered.
 *
 * A NAVIGABLE IS NOT ITS ACTIVE DOCUMENT. That sentence is the reason this interface exists at all — a proxy
 * outlives the documents in it — and it is also the reason most of the surface below never leaves this
 * instance. `name`, `opener`, `parent`, `top` and the four self-references are properties of the NAVIGABLE,
 * and a navigable belongs to the instance that CREATED it: this one made the child, named it and knows what it
 * was nested in.
 *
 * THAT SPLIT WAS THE WHOLE BUG. Every member here used to be a host request, so an iframe's `contentWindow`
 * could answer nothing at all until a host could run a second document; `otherW.self === otherW` — which is
 * true by definition, in any browser, about a navigable nobody has to look inside — parked its flow. Answering
 * the navigable's own state locally is not an optimisation, it is where the state actually is.
 *
 * AND `closed` IS NOT ONE OF THEM, WHICH IS THE HALF OF THAT SPLIT THAT WAS WRONG. This comment used to name
 * it beside `name` and `opener` and say the creator "knows whether it has been destroyed", and that sentence
 * was true of every navigable except the one the member exists for. §7.2.2.1 opening and closing windows makes
 * `closed` the OR of two facts about the TOP-LEVEL TRAVERSABLE — its browsing context being null, and its is
 * closing — and `close()` sets is closing IN THE AGENT THAT RUNS IT. A popup that calls `window.close()` on
 * itself runs that in its OWN instance and writes its OWN record; the opener's copy of the same traversable
 * never hears, so `w.closed` answered false about a window that had closed itself, forever, in the one
 * direction a page actually polls. That is CLAUDE.md's ONE FACT ANSWERED FROM MANY PLACES exactly: two C
 * records for one traversable, and the fix is never to write the second from more places but to ASK the
 * instance whose record is the one the standard is talking about.
 *
 * IT IS MONOTONIC, AND THAT IS WHAT MAKES IT ANSWERABLE AT ALL. §7.2.2.1's close() step 3 returns early when
 * is closing is already true, and a null browsing context is terminal, so `closed` never goes back to false:
 * a record that says CLOSED is the answer wherever it is kept, and only a record that says OPEN leaves the
 * question to the peer. `length` has no such half — it is a live count of the peer's active document and must
 * be asked every time — so the two members share the step machine and differ in exactly this one branch.
 *
 * WHY THE REMOTE HALF IS A STEP MACHINE. `otherWindow.length` must answer at its own call site, and the answer
 * is not available in this turn. There is exactly one shape in this engine that can suspend and answer at the
 * same call site: post the question on the flow's pending register, return JS_STEP_YIELD, and the scheduler
 * parks the flow — byte-identical, at any depth — while its siblings run. The host routes it to the instance
 * holding that document, because SECURITY.md makes the trusted zone the only thing that knows which that is.
 *
 * THE WORLD TRAVELS WITH IT. The question is not "what is document 7's `length`" but "what is it in THIS
 * FLOW'S world" — two arms of a fork that framed it differently must get different answers, which is the same
 * reason a fork re-issues an unanswered request under its own world rather than sharing the id.
 *
 * A LOCAL PROXY forwards everything to its Window with an ordinary read, in this turn: it IS the same document,
 * so its Window is authoritative for every member including the navigable's own. */
enum {
    /* §7.2.2's four names for THIS navigable's own proxy. `window`, `self` and `frames` are on §7.2.1's
       cross-origin whitelist; `globalThis` is the global object, which IS the proxy, and is same-origin only —
       the distinction costs nothing here because both answers are the same object. */
    WP_WINDOW, WP_SELF, WP_FRAMES, WP_GLOBALTHIS,
    WP_PARENT, WP_TOP, WP_OPENER,
    /* THE FIRST OF THE TWO MEMBERS THAT LEAVE THIS INSTANCE — the TOP-LEVEL TRAVERSABLE's is-closing, which
       §7.2.2.1's close() writes in whichever agent ran it, so the peer's record is the one the standard names
       whenever this agent's own says OPEN. The enum ORDER is load-bearing (PROXY_MEMBER and
       PROXY_CROSS_ORIGIN are indexed by it), so this sits where it always sat. */
    WP_CLOSED,
    WP_NAME,
    WP_LENGTH,   /* the ACTIVE DOCUMENT's child-navigable count — the other member that leaves this instance */
    /* §7.2.2's `document`, SAME-ORIGIN ONLY. It answers with the OTHER realm's Document OBJECT, and it can
       always do so in this turn: an agent is ORIGIN-KEYED, so a same-origin navigable is a realm of this agent
       by construction and a proxy this agent cannot answer for is cross-origin, where the answer is a
       SecurityError rather than a document. That pairing is asserted rather than assumed — see proxy_realm. */
    WP_DOCUMENT,
    /* §7.2.2's `location` — the LOCATION OBJECT OF THE ACTIVE DOCUMENT, and the first thing a page does with a
       popup it just opened. It was missing entirely, so `w.location.pathname` read a property of undefined: 63
       subtests in html/browsers failed on that one line, all of them after §7.4 started running popups.
       IT IS ON §7.2.1's CROSS-ORIGIN LIST, which is what makes it different from `document` — the PROPERTY
       is reachable across origins even though almost every member of the object it returns is not, because
       `otherW.location.href = url` is how one document navigates another. This engine has no cross-origin
       Location object to hand back, so that arm names itself rather than answering with this document's. */
    WP_LOCATION,
    WP_MEMBER_N
};
static const char *const PROXY_MEMBER[WP_MEMBER_N] = {
    "window", "self", "frames", "globalThis", "parent", "top", "opener", "closed", "name", "length",
    "document", "location"
};
/* §7.2.1's CROSS-ORIGIN PROPERTY NAMES — the fixed list a WindowProxy exposes whatever the origins are:
   window, self, location, close, closed, focus, blur, frames, length, top, opener, parent, postMessage.
   Everything else is same-origin ONLY, and reaching it across origins is a SecurityError.
 *
 * THE LIST IS DECLARED BESIDE THE MEMBERS rather than checked at each read, because the failure mode of the
 * alternative is silence: this surface had NO check at all, and it was safe only by the accident that the
 * members installed so far happen to be nearly the whole allowlist. The first member added without one —
 * `document`, `location`'s same-origin half, anything reached through them — would have leaked a cross-origin
 * document with nothing to say so. A member declared here cannot be added without answering the question. */
static const bool PROXY_CROSS_ORIGIN[WP_MEMBER_N] = {
    true,  /* window     */ true,  /* self       */ true,  /* frames     */
    false, /* globalThis — the global OBJECT, and §7.2.1 does not list it */
    true,  /* parent     */ true,  /* top        */ true,  /* opener     */
    true,  /* closed     */
    false, /* name — a browsing context's name is NOT on the list; a cross-origin read of it is a SecurityError */
    true,  /* length     */
    false, /* document — §7.2.1 does not list it, so a cross-origin read is a SecurityError */
    true,  /* location — §7.2.1 DOES list it; the filtering is the Location object's own, not this table's */
};

/* §7.2.1: a read the origins do not permit is a SecurityError, and it is thrown at the READ rather than
   answered with undefined — a page distinguishes the two, and undefined would say "this window has no such
   member" about one it cannot see. */
static bool proxy_read_permitted(const ProxyData *p, int magic)
{
    if (PROXY_CROSS_ORIGIN[magic])
        return true;   /* the fixed list is answered whatever the origins are, so nothing below is asked */
    return proxy_same_origin(p);
}

/* ---- §7.2.3.5's CROSS-ORIGIN BRANCH — the half of the filter that has no member to hang off ---------------
 *
 * THE TABLE ABOVE ANSWERS A DIFFERENT QUESTION FROM THIS ONE, and that is why both exist rather than one being
 * a copy of the other. `PROXY_CROSS_ORIGIN` is indexed by MEMBER MAGIC: it answers "may this INSTALLED accessor
 * be read across origins", and it is consulted from inside the accessor, which means it is only ever asked
 * about a name this component installed. Every other name — `alert`, `document.cookie`'s owner, anything a page
 * probes for — reaches no accessor at all, walks off the end of the prototype chain, and answers `undefined`.
 *
 * AND `undefined` IS THE ONE ANSWER §7.2.1.3.2 FORBIDS. Its last step is "throw a SecurityError DOMException",
 * and a page distinguishes the two: `undefined` says "that window has no such member", which is a fact about
 * the peer's document, while the SecurityError says "you may not look" — a `try`/`catch` around the read is how
 * a page feature-detects a cross-origin window at all. So the surface was not merely incomplete, it was
 * answering a security question with a datum, and it was silent by exactly the mechanism §CLAUDE.md's
 * defaulted-field rule names: the wrong value is a plausible one.
 *
 * SO THE LIST IS THE STANDARD'S OWN, AND IT IS NOT THE MEMBER TABLE. §7.2.1.3.1 names THIRTEEN cross-origin
 * accessible window property names, and four of them (`close`, `focus`, `blur`, `postMessage`) are not members
 * of the table above: `close` is installed as a method by this component, `postMessage` by window_message.c,
 * and `focus`/`blur` by core/html/focus.c, which owns §6.6.6's steps and installs its two Window members onto
 * BOTH §7.2.2's global and this surface from one list. The two tables are tied to each other by an assert at
 * capture rather than by a reader keeping them in step.
 *
 * THE THIRTEEN ARE ANSWERED FROM TWO ENDS AND BOTH ENDS ARE ASSERTED, WHICH TOOK TWO SEPARATE CORRECTIONS OF
 * THIS PARAGRAPH TO REACH — the shape is worth more than the incident. It first claimed `focus`/`blur` were
 * "honestly ABSENT", which was false about the Window; corrected, it then said a listed name is answered "by
 * whatever owns it, INCLUDING nothing", which reads as a design allowance and is not one: `undefined` at the
 * end of the prototype chain is the exact answer §7.2.1.3.2 CrossOriginPropertyFallback's last step rules out,
 * and it is what `otherW.focus` returned while both sentences stood. A claim about which surface owns what is
 * therefore not prose here — it is a loop, in proxy_get_own's steps 4-6, over this list.
 *
 * IT IS DECLARED HERE AND ASKED ONCE, at the object's own [[GetOwnProperty]], because that is where §7.2.3.5
 * puts it — before the prototype walk, so a name outside the list never reaches a member at all. Asking it
 * per-member is what produced the silence above. */
/* AND EACH ENTRY IS A RECORD, NOT A NAME — §7.2.1.3.1 returns « { [[Property]]: "window", [[NeedsGetter]]:
 * true, [[NeedsSetter]]: false }, … » and the two flags are half of what each entry says. A name-only list
 * answers §7.2.3.5's question (does this name reach a member at all) and CANNOT answer §7.2.1.1's (is this
 * ACCESS to that name permitted), which is decided by `type` against those very flags: `location` is on the
 * list with BOTH flags so its getter and its setter both go through, while Location's `href` is on the list
 * with [[NeedsGetter]] FALSE so its SETTER goes through and its getter is a SecurityError. Two lists — one of
 * names here and one of flags wherever the second question was asked — is the second copy this file's own
 * capture-time loop exists to make impossible, so the flags live on the one list. */
/* §7.2.1.3.1's Window arm VERBATIM, in the standard's own order. An entry written with neither flag is one
   the standard writes as `{ [[Property]]: "close" }` — an OPERATION, which §7.2.1.1's "type is method and e
   has neither [[NeedsGetter]] nor [[NeedsSetter]]" is exactly the test for. */
static const CrossOriginProperty CROSS_ORIGIN[] = {
    { "window",      true,  false },
    { "self",        true,  false },
    { "location",    true,  true  },
    { "close",       false, false },
    { "closed",      true,  false },
    { "focus",       false, false },
    { "blur",        false, false },
    { "frames",      true,  false },
    { "length",      true,  false },
    { "top",         true,  false },
    { "opener",      true,  false },
    { "parent",      true,  false },
    { "postMessage", false, false },
};
#define CROSS_ORIGIN_NAME_N ((int)(sizeof CROSS_ORIGIN / sizeof CROSS_ORIGIN[0]))
static JSAtom g_xo_atom[CROSS_ORIGIN_NAME_N];
/* §7.2.1.3.2 step 1's FOUR NAMES, which are the exception to the throw: `then` (so a cross-origin WindowProxy
   is not mistaken for a thenable and awaited), and the three well-known symbols an engine touches while doing
   something else entirely. They answer `undefined` as a real property descriptor rather than by falling off the
   chain, because §7.2.3.10 lists them among the own keys. */
#define XO_FALLBACK_N 4
static JSAtom g_xo_fallback[XO_FALLBACK_N];
/* §7.2.1.3.4's getters for the entries above, ONE ARRAY PER REALM — see window_proxy.h. The array is indexed
   by the CROSS_ORIGIN row, so the standard's own list is what says which slot is which and there is no second
   ordering to keep in step. `-1` and not 0: realm_value_set's own assert is `slot > 0`, so a slot that was
   never declared must not read as one that was. */
static int g_xo_getter_slot = -1;

/* §7.2.1.3.1 CrossOriginProperties ( O )'s WINDOW ARM, ASKED BY NAME — the one list answering one more caller.
 *
 * IT IS EXPORTED BECAUSE THE OTHER END OF THE CROSS-INSTANCE SEAM HAD NO WAY TO ASK IT. Everything above
 * decides what THIS agent hands out for a name a page wrote, and the asking half of a cross-document read is
 * correct by construction — it can only emit `PROXY_MEMBER[magic]`, an index into a fixed table. The
 * PERFORMING half receives that member as TEXT from another instance, through a routing zone whose whole
 * contract is that it does not read what it routes, and SECURITY.md makes every WASM instance untrusted. So
 * the name a peer performs `globalThis[name]` for is the one place on this seam where an unlisted member
 * would be read across origins and relayed, and until this entry existed there was nothing there to ask.
 *
 * IT IS A LOOKUP AND NOT A PREDICATE because the caller needs the RECORD: §7.2.1.3.1 writes each entry with
 * its [[NeedsGetter]] and [[NeedsSetter]], and a `bool` here would be the second copy this file's own
 * capture-time loop exists to make impossible — the flags would then have to be re-derived wherever the
 * second question is asked. NULL for a name the standard does not list, which is the caller's to crash on:
 * what a refusal MEANS differs by end (a page gets §7.2.1.3.2's SecurityError; a peer performing a record
 * gets an abort, because a record naming an unlisted member was written by something that is not this
 * engine's asking half). Side-effect-free, and the list is static, so it is answerable with no realm. */
const CrossOriginProperty *window_proxy_cross_origin_property(const char *name)
{
    int i;

    DCHECK(name != NULL,
           "§7.2.1.3.1 CrossOriginProperties ( O ) was asked about no name — its own step is a SameValue "
           "against each entry's [[Property]], so an unnamed member would match nothing and be reported "
           "unlisted, which is the answer that reads as a refusal rather than as a broken caller");
    for (i = 0; i < CROSS_ORIGIN_NAME_N; i++)
        if (!strcmp(CROSS_ORIGIN[i].name, name)) return &CROSS_ORIGIN[i];
    return NULL;
}

/* CAPTURED ONCE PER AGENT, from THIS realm's %Symbol%, before any page script runs — the same rule and the same
   reason as remote_object.c's well-known table: `Symbol` is a global a page may replace, and a well-known
   symbol resolved after that names whatever the page put there. Atoms are the RUNTIME's, and an agent is one
   runtime, so one capture serves every realm this agent builds. */
static void proxy_capture_names(JSContext *ctx)
{
    static const char *const FALLBACK_SYM[XO_FALLBACK_N - 1] = { "toStringTag", "hasInstance",
                                                                 "isConcatSpreadable" };
    JSValue global, sym_ctor;
    int i;

    for (i = 0; i < CROSS_ORIGIN_NAME_N; i++) {
        g_xo_atom[i] = JS_NewAtom(ctx, CROSS_ORIGIN[i].name);
        CHECK(g_xo_atom[i] != JS_ATOM_NULL,
              "window proxy: §7.2.1.3.1's cross-origin property names could not be interned — without them "
              "every cross-origin read is decided by comparing against nothing");
    }
    g_xo_fallback[0] = JS_NewAtom(ctx, "then");
    CHECK(g_xo_fallback[0] != JS_ATOM_NULL, "window proxy: §7.2.1.3.2's `then` could not be interned");

    global = JS_GetGlobalObject(ctx);
    sym_ctor = JS_GetPropertyStr(ctx, global, "Symbol");
    JS_FreeValue(ctx, global);
    CHECK(JS_IsObject(sym_ctor),
          "window proxy: this realm has no %Symbol% — §7.2.1.3.2's fallback is three well-known symbols, and a "
          "realm without the intrinsic can recognise none of them");
    for (i = 0; i < XO_FALLBACK_N - 1; i++) {
        /* A DATA PROPERTY of %Symbol% (6.1.5 makes every well-known symbol one, non-writable and
           non-configurable), so this read runs none of the page's code — which it must not, since it runs from
           C with no flow base under it. */
        JSValue s = JS_GetPropertyStr(ctx, sym_ctor, FALLBACK_SYM[i]);
        CHECK(JS_IsSymbol(s), "window proxy: %Symbol% carries no well-known symbol under one of the three names "
                              "§7.2.1.3.2's fallback is defined over");
        g_xo_fallback[1 + i] = JS_ValueToAtom(ctx, s);
        JS_FreeValue(ctx, s);
        CHECK(g_xo_fallback[1 + i] != JS_ATOM_NULL, "window proxy: a well-known symbol could not be interned");
    }
    JS_FreeValue(ctx, sym_ctor);

    /* THE TWO TABLES ARE ONE FACT AND ARE CHECKED AGAINST EACH OTHER. A member marked cross-origin-readable
       whose NAME is not on §7.2.1.3.1's list would be permitted by its own accessor and never reached, because
       [[GetOwnProperty]] throws before the prototype walk; a member marked same-origin-only whose name IS on
       the list would be reached and then refuse. Both are silent, and neither can survive this loop. */
    for (i = 0; i < WP_MEMBER_N; i++) {
        bool listed = false;
        int k;
        int listed_at = -1;
        for (k = 0; k < CROSS_ORIGIN_NAME_N; k++)
            if (!strcmp(PROXY_MEMBER[i], CROSS_ORIGIN[k].name)) { listed = true; listed_at = k; break; }
        DCHECK(listed == PROXY_CROSS_ORIGIN[i],
               "a WindowProxy member's own cross-origin flag disagrees with §7.2.1.3.1's list of cross-origin "
               "accessible window property names. The list decides whether the read reaches a member at all "
               "(§7.2.3.5 throws before the prototype walk) and the flag decides what the member then answers, "
               "so a disagreement is either a member that can never be read across origins however it is "
               "marked, or one that is reached and then refuses — both of them silent");
        /* AND THE ENTRY'S [[NeedsGetter]] AGREES WITH HOW THIS SURFACE SHIPS THE MEMBER. Every member of
           the table above is a §7.2.2 ATTRIBUTE, installed here as an accessor, so a listed one whose
           record says [[NeedsGetter]] false would be a name §7.2.3.5 steps 4-6 hand out and §7.2.1.1
           Integration with IDL then refuses the getter of — a descriptor a page can read and an accessor
           it cannot call, which is neither of the two answers the standard has. Location's `href` is the
           entry that shape is real for, and it is real THERE because §7.2.4 The Location interface writes its
           `href` for the SETTER alone; nothing on this surface is.
           IT IS THE HALF THE LOOP ABOVE CANNOT SEE: that one pairs a NAME against the list, and the flags
           are what the list says about the name once it is found. */
        DCHECKF(!listed || CROSS_ORIGIN[listed_at].needs_get,
                "HTML §7.2.1.3.1 CrossOriginProperties lists `%s` with [[NeedsGetter]] FALSE while §7.2.3 The "
                "WindowProxy exotic object's own surface installs it as an ACCESSOR — so §7.2.3.5 "
                "[[GetOwnProperty]] answers the descriptor for a cross-origin read and §7.2.1.1 Integration "
                "with IDL then throws a "
                "SecurityError out of the getter that descriptor names. Either the record is mistyped or "
                "the member is not an attribute of this surface", PROXY_MEMBER[i]);
    }
}

/* §7.2.1.3.4 CrossOriginGetOwnPropertyHelper ( O, P )'s GETTERS FOR THIS REALM — see window_proxy.h for what
 * the standard asks for and why an ordinary [[Get]] is not it.
 *
 * IT READS THE WINDOW'S OWN SLOT AND NOT THE MEMBER. §7.2.1.3.4's step is `OrdinaryGetOwnProperty(O, P)`, which
 * is JS_GetOwnPropertyNoUserCode here — the internal method, performed with no accessor invoked and no page
 * code run, which is the same call §7.2.3.5 step 3 already makes on a Window one file's-worth of lines up. A
 * property GET would run the getter at capture time, in a C activation with no flow base under it.
 *
 * NINE OF THE THIRTEEN. The four entries the standard writes with neither flag are §7.2.1.3.4's OPERATION
 * branch — `{ [[Property]]: "close" }` — whose value is a callable rather than a getter, so they have nothing
 * to capture and their slot stays a hole. window_proxy_cross_origin_getter refuses one by name.
 *
 * WHAT ITS ASSERT IS FOR. This is the whole of the ordering contract between core/platform.c's rows: the eight
 * that `window`'s row installs and the `location` that runs as a realm intrinsic before it must all be present,
 * as accessors, at this instant. A member installed by a LATER row would be captured as absent — and then a
 * peer would answer that one member out of whatever the page had left on its global, silently, which is the
 * exact defect this capture exists to end. So the loop crashes naming the member rather than skipping it. */
void window_proxy_install_window_getters(JSContext *ctx, JSValueConst global)
{
    JSValue getters;
    int i;

    DCHECK(g_xo_getter_slot > 0,
           "§7.2.1.3.4's getters were captured for a realm before window_proxy_init declared the slot to hold "
           "them — the declaration is core/platform.c's declare column and this is its install column, so "
           "reaching here without it is an install into an agent that was never brought up");
    DCHECK(JS_IsObject(global),
           "§7.2.1.3.4's getters were captured off something that is not the global object — the standard reads "
           "them off O, which for this seam is the peer document's Window");
    getters = JS_NewArray(ctx);
    CHECK(!JS_IsException(getters),
          "window proxy: §7.2.1.3.4's per-realm getter table could not be allocated — without it every "
          "cross-instance read of this document falls back to nothing at all");
    for (i = 0; i < CROSS_ORIGIN_NAME_N; i++) {
        JSPropertyDescriptor d;
        int has;

        if (!CROSS_ORIGIN[i].needs_get) continue;   /* §7.2.1.3.4's operation branch — no getter exists */
        has = JS_GetOwnPropertyNoUserCode(ctx, &d, global, g_xo_atom[i]);
        /* A CHECK AND NOT A DCHECK, AND THE PROMOTION BELONGS TO THIS LOOP RATHER THAN TO ANYBODY'S JUDGEMENT
           ABOUT HOW SERIOUS THE GAP IS. `JS_GetOwnPropertyNoUserCode` fills the descriptor ONLY on 1 — its
           absent arm returns without touching it, and its throwing arm the same — so the three JS_FreeValues
           below are the FIRST release-mode dereference of a record a dev-only assert was covering. The
           question §Offensive-programming makes mechanical is which build the guard survives in, and the
           answer changed because these lines exist. */
        CHECKF(has == 1,
               "HTML §7.2.1.3.1 CrossOriginProperties ( O ) lists `%s` with [[NeedsGetter]] true and this "
               "realm's Window has no own property of that name at the instant §7.2.1.3.4's getters are "
               "captured — core/platform.c's `window_proxy` row runs after the rows that install the nine, so "
               "either a member moved to a later row or it is not installed at all, and a peer asked for it "
               "would answer out of whatever the page leaves on its global",
               CROSS_ORIGIN[i].name);
        DCHECKF((d.flags & JS_PROP_GETSET) && JS_IsFunction(ctx, d.getter),
                "HTML §7.2.1.3.1 CrossOriginProperties ( O ) lists `%s` with [[NeedsGetter]] true while this "
                "realm's Window carries it as something other than an accessor with a callable getter — Web IDL "
                "§3.7.6 Attributes makes every attribute an accessor, and §7.2.1.3.4 "
                "CrossOriginGetOwnPropertyHelper has a getter's STEPS to perform or it has nothing",
                CROSS_ORIGIN[i].name);
        JS_SetPropertyUint32(ctx, getters, (uint32_t)i, JS_DupValue(ctx, d.getter));
        JS_FreeValue(ctx, d.value);
        JS_FreeValue(ctx, d.getter);
        JS_FreeValue(ctx, d.setter);
    }
    realm_value_set(ctx, g_xo_getter_slot, getters);   /* asserts this realm captured exactly once */
}

JSValue window_proxy_cross_origin_getter(JSContext *ctx, const char *name)
{
    const CrossOriginProperty *e = window_proxy_cross_origin_property(name);
    JSValue table, getter;

    /* THE TWO REFUSALS ARE THE CALLER'S TO HAVE ALREADY MADE — remote_op.c asserts both where the record is
       BORN, which is the only place both of its parse's callers reach. They are re-stated here as DCHECKs and
       not as a second CHECK for the reason that file's own comment gives: a second copy of one invariant is a
       copy that drifts. What these two add is the SITE — a caller that reaches this entry with an unlisted or
       operation-only name is named here rather than dereferencing a NULL two lines down. */
    DCHECKF(e != NULL,
            "§7.2.1.3.4 CrossOriginGetOwnPropertyHelper's getter was asked for `%s`, which HTML §7.2.1.3.1 "
            "CrossOriginProperties ( O ) does not list among the cross-origin accessible window property names",
            name);
    DCHECKF(e == NULL || e->needs_get,
            "§7.2.1.3.4 CrossOriginGetOwnPropertyHelper's getter was asked for `%s`, whose §7.2.1.3.1 record "
            "carries neither [[NeedsGetter]] nor [[NeedsSetter]] — that is §7.2.1.3.4 "
            "CrossOriginGetOwnPropertyHelper ( O, P )'s OPERATION branch, whose answer is §7.2.1.3.4's \"an "
            "anonymous built-in function, created in the current realm, that performs the same steps as the "
            "IDL operation P on object O\" and not a getter's result. BUILD THE OPERATION BRANCH: a function "
            "crosses this seam "
            "only once core/frame/remote_object.c can name one, and until then this member has no answer that "
            "is not a different member's",
            name);
    table = realm_value_get(ctx, g_xo_getter_slot);   /* asserts this realm ran the capture */
    /* AN ORDINARY [[Get]] IS EXACTLY RIGHT HERE and is not the read the header argues against: the array is
       this component's own, minted at the capture and reachable from no script, so there is no page-owned slot
       between the index and the function. */
    getter = JS_GetPropertyUint32(ctx, table, (uint32_t)(e - CROSS_ORIGIN));
    JS_FreeValue(ctx, table);
    DCHECKF(JS_IsFunction(ctx, getter),
            "§7.2.1.3.4's captured getter for `%s` is not a function in this realm — the capture asserts every "
            "listed accessor was one, so a hole here is a table built for a DIFFERENT list than the one this "
            "lookup indexed into", name);
    return getter;
}

/* HTML §7.2.1.1 "Integration with IDL", WHOLE — "When perform a security check is invoked, with a
 * platformObject, identifier, and type, run these steps". Three steps, and each one is below in order.
 *
 * IT IS THE OTHER HALF OF §7.2.1 AND IT WAS THE MISSING ONE. Everything else in this file filters a PROPERTY
 * LOOKUP: §7.2.3.5 [[GetOwnProperty]] decides what a cross-origin WindowProxy hands out, and CROSS_ORIGIN is
 * the list it decides with. That is exactly right for `otherW.setTimeout`, which never reaches a member — and
 * it says nothing whatever about `setTimeout.call(otherW, f)`, where the function came from the READER's own
 * window and the receiver was carried past the lookup on the call. Web IDL puts the check INSIDE the function
 * for that reason: create an operation function's try-list performs it before the brand TypeError and before
 * §3.6 Overload resolution algorithm, so a cross-origin receiver is refused whatever the spelling was.
 *
 * THE SPELLINGS THAT REACH IT ARE THE ONES §C-stack MAKES ORDINARY. `.call`, `.apply`, `Reflect.apply`, a
 * bound callee and the spread are call-site-resolved onto the ultimate target with the receiver INTACT, so
 * every one of them delivers a foreign `this` to the member's body. That is not a corner of the language: it
 * is how `Object.getOwnPropertyDescriptor(self, "opener").get.call(popup)` is written, which is a WPT
 * assertion this engine already runs, and how a bundle probes a frame it cannot read.
 *
 * WHAT WAS THERE INSTEAD WAS ONE MEMBER'S ACCIDENT. core/timing/timer.c's §8.7 Timers receiver resolution
 * reaches proxy_realm for a cross-origin navigable, and proxy_realm's hosted-document DCHECK fires — a crash
 * naming a missing capability, not a check: it is one member, it is dev-only, and it says nothing about
 * `close`, `focus`, `blur` and `postMessage`, which must go THROUGH.
 *
 * BOTH HALVES ARE THE ASSERTION, AND THAT IS WHY THE LIST CARRIES FLAGS. A filter that refused every
 * cross-origin receiver would pass a test that only checks the throw, and it would be wrong four times over on
 * the Window (the four operations §7.2.1.3.1 lists with neither flag), twice on `location` (both its getter
 * and its setter go through) and once on a Location (`href`'s SETTER goes through while its GETTER does not).
 * Step 2 below is those flags and nothing else.
 *
 * `platform_object` IS THE RAW `this` VALUE AND NOT §3.7.7's RESOLVED `jsValue`, and that is an equivalence
 * rather than a shortcut. The resolution's only effect is on a MISSING receiver — "or realm's global object
 * otherwise" — and that global is a Window of THIS heap, which the Window arm below answers same-origin for
 * unconditionally, so resolving it would reach step 3's return by a longer road. It is also the arm that costs:
 * window_proxy_this_object ANSWERS OWNED, and this runs on every declared member of the platform.
 *
 * STEP 1's TWO ARMS ARE ASKED AS THREE BRANDS, and the third is absent on purpose — see remote_location.h.
 * A Window (any realm's global; §7.1.1 Origins' same origin is a constant across an ORIGIN-KEYED AGENT
 * CLUSTER) and a WindowProxy (whose origin is per-flow, read where §7.2.1.3.3 reads it) are the Window arm;
 * a CROSS-ORIGIN Location is the Location arm. A Location this agent holds takes step 1's return and step 3's
 * pass to the same place, so there is no observation that separates them and no brand to ask.
 *
 * Returns 0 where §7.2.1.1 RETURNS, -1 with its "SecurityError" DOMException pending. */
int window_proxy_security_check(JSContext *ctx, JSValueConst platform_object, const char *identifier,
                                WindowProxySecurityType type)
{
    const CrossOriginProperty *xo;
    int n, i;
    bool same_origin;

    DCHECK(identifier != NULL,
           "§7.2.1.1 Integration with IDL was invoked with no `identifier` — its step 2 is a SameValue against "
           "each entry's [[Property]], so a member reaching here unnamed would match nothing on the list and "
           "every cross-origin access to it would be refused, the four operations included");

    /* STEP 1 — "If platformObject is not a Window or Location object, then return." */
    if (window_proxy_is(platform_object)) {
        xo = CROSS_ORIGIN;  n = CROSS_ORIGIN_NAME_N;
        /* §7.2.1.3.3 IsPlatformObjectSameOrigin, read here rather than at step 3, because reading it is what
           captures the proxy's record into the running flow's delta and the branch below must not decide
           whether that happens: the answer is per-flow either way. */
        same_origin = window_proxy_same_origin_of(platform_object);
    } else if (window_is(platform_object)) {
        xo = CROSS_ORIGIN;  n = CROSS_ORIGIN_NAME_N;
        /* EVERY Window OBJECT IN THIS HEAP IS SAME ORIGIN WITH THE ASKER, and that is SECURITY.md's own
           keying rather than an assumption: an instance is `(browsing context group, origin)`, so a foreign
           realm's global reached through a same-origin document is same origin by construction and a
           cross-origin document's Window is not in this heap at all — it is a WindowProxy, taken above. */
        same_origin = true;
    } else if (remote_location_is(platform_object)) {
        /* §7.2.1.3.3 answers FALSE for this object by construction — it exists only for a Document another
           instance holds (remote_location.h), which is the same condition. */
        xo = LOCATION_CROSS_ORIGIN;  n = LOCATION_XO_N;
        same_origin = false;
    } else {
        return 0;
    }

    /* STEP 2 — "For each e of CrossOriginProperties(platformObject): if SameValue(e.[[Property]], identifier)
       is true", and then its three sub-steps in the standard's order. SameValue over two strings is `strcmp`
       here: `identifier` is the member's IDL identifier, a static string the declaration owns. */
    for (i = 0; i < n; i++) {
        if (strcmp(xo[i].name, identifier) != 0) continue;
        switch (type) {
        /* 2.1.1 — "If type is "method" and e has neither [[NeedsGetter]] nor [[NeedsSetter]], then return."
           NEITHER, not either: `location` is on the list with both flags and is an ATTRIBUTE, so a page that
           pulled some operation named "location" out of another interface and applied it to a cross-origin
           WindowProxy is refused rather than let through on a name match. */
        case WP_SEC_METHOD: if (!xo[i].needs_get && !xo[i].needs_set) return 0; break;
        /* 2.1.2 — "Otherwise, if type is "getter" and e.[[NeedsGetter]] is true, then return." */
        case WP_SEC_GETTER: if (xo[i].needs_get) return 0; break;
        /* 2.1.3 — "Otherwise, if type is "setter" and e.[[NeedsSetter]] is true, then return." */
        case WP_SEC_SETTER: if (xo[i].needs_set) return 0; break;
        default:
            DFAILF("§7.2.1.1 Integration with IDL was invoked with a `type` Web IDL §3.5 Security does not "
                   "define — its three inputs are the platform object, the identifier, and one of \"method\", "
                   "\"getter\" and \"setter\", and a fourth kind of function object is a fourth sub-step to "
                   "write here rather than a value to fall past (%d)", (int)type);
        }
        /* AND THE LOOP DOES NOT STOP HERE. §7.2.1.3.1's lists have no duplicate [[Property]], so a match that
           did not return can only fall to step 3 — but the standard's `for each` is what is written, and a
           `break` would be this file deciding that. */
    }

    /* STEP 3 — "If IsPlatformObjectSameOrigin(platformObject) is false, then throw a "SecurityError"
       DOMException." A SecurityError and not `undefined` and not a TypeError: a page tells all three apart,
       and `try { f.call(otherW) } catch (e) { e.name }` is how it does. */
    if (same_origin) return 0;
    JS_ThrowDOMException(ctx, "SecurityError",
                         "the origins do not permit `%s` to be invoked on that object", identifier);
    return -1;
}

/* ECMA-262 6.1.7's ARRAY INDEX, spelled out rather than parsed: a canonical numeric string for an integer in
   [0, 2**32-1). "01", "1.0" and " 1" are ordinary property names and a strtoul would call all three indices.
   THE INDEX ITSELF IS THE ANSWER and not merely whether there is one. §7.2.3.5 step 2.1 is `! ToUint32(P)`,
   which is the same digits this loop has already accumulated — deriving it a second time with a second parse
   is the second answer that disagrees with the first for exactly the strings this one exists to reject. */
static bool proxy_atom_index(JSContext *ctx, JSAtom prop, uint32_t *pidx)
{
    JSValue v = JS_AtomToValue(ctx, prop);
    const char *s;
    uint64_t n = 0;
    bool idx = false;

    DCHECK(pidx != NULL, "§7.2.3.5's array-index test was asked without anywhere to put the index — the two "
                         "are one answer, and a caller that wants only the boolean is about to re-parse it");
    if (!JS_IsString(v)) { JS_FreeValue(ctx, v); return false; }   /* a symbol is never an index */
    s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s) return false;
    if (s[0] && (s[0] != '0' || s[1] == 0)) {
        int i;
        idx = true;
        for (i = 0; s[i]; i++) {
            if (i >= 10 || s[i] < '0' || s[i] > '9') { idx = false; break; }
            n = n * 10 + (uint64_t)(s[i] - '0');
        }
        if (idx && n >= 4294967295u) idx = false;
    }
    JS_FreeCString(ctx, s);
    if (idx) *pidx = (uint32_t)n;
    return idx;
}

/* HTML §7.2.3.5 STEP 2 — THE ARRAY-INDEX BRANCH, and it is the whole definition of indexed access rather than
 * one of two: §7.2.2.2 says so in one sentence — "indexed access to document-tree child navigables is defined
 * through the [[GetOwnProperty]] internal method of the WindowProxy object" — so `frames[0]`, `parent[1]`,
 * `top[0]` and `otherW[0]` are one algorithm with one implementation, which is this.
 *
 * IT RUNS BEFORE THE SAME-ORIGIN QUESTION, and that ordering is the bug this branch was in. Step 2 precedes
 * step 3's `IsPlatformObjectSameOrigin(W)`, and this hook asked step 3 first and returned "no own property"
 * for a same-origin W — so `frames[0][0]` walked off the end of the prototype chain and answered `undefined`
 * about a grandchild navigable that exists, with nothing to say so. Every path through step 2 RETURNS, so an
 * array index never reaches step 3 at all.
 *
 * THE LIST IS THE TARGET DOCUMENT'S, NEVER THE READING REALM'S — the same sentence `length` is answered by,
 * because it is the same list. Two facts decide it without materializing anything:
 *   a DESTROYED navigable has no active document, so it has no children;
 *   a navigable whose realm has NOT been materialized is showing the initial about:blank Document §7.4 created
 *     it with, an element can only get into that document by script, and script cannot run in a realm that
 *     does not exist — so its child list is EMPTY, computed rather than assumed. Building a realm here to
 *     count the frames in a document that provably has none is what made a read through a proxy cost a realm
 *     per flow, which `length` already refuses for the identical reason.
 * The walk itself is html_iframe.c's, over the Lexbor tree and each container's per-flow navigable slot — no
 * page code, by construction, which is what lets this hook keep declaring get_own_property_no_user_code and is
 * exactly what window.c's own exotic does for the global spelling of the same navigable.
 *
 * OUT OF RANGE IS TWO ANSWERS AND THEY ARE NOT INTERCHANGEABLE (step 2.4): `undefined` for a same-origin W
 * says "that window has no such frame", and the SecurityError for a cross-origin one says "you may not look".
 * A page tells them apart with a `try`/`catch`, which is how it feature-detects a cross-origin window at all. */
static int proxy_indexed_get_own(JSContext *ctx, JSPropertyDescriptor *desc, const ProxyData *p, uint32_t idx)
{
    JSValue child = JS_UNDEFINED;

    /* THE INDEX IS A uint32 AND THE WALK COUNTS IN int, so the range above INT32_MAX is out of range rather
       than a negative index. A document with that many child navigables is not reachable — but a cast that
       wraps would ask for one, and the walk would answer for a frame at some other position. */
    if (!wp_closed(p) && idx <= (uint32_t)INT32_MAX) {
        if (!world_doc_hosted(p->doc)) {
            /* §7.2.3.5 step 2.2 OF A DOCUMENT IN ANOTHER INSTANCE, and ONE thing is missing now rather than
               two. This block named both, and the OTHER one is built: a navigable crosses as its IDENTITY —
               document name, origin, browsing-context name, the parent's and the opener's document names —
               and resolves on the far side to that agent's own WindowProxy or to the one remote proxy it
               keeps per document (remote_object.c's nav_encode/nav_decode, window_proxy_for_document above).
               So the ANSWER can be sent.
               TWO THINGS ARE MISSING, AND THE SECOND ONE USED TO BE WRITTEN HERE AS ALREADY BUILT. This block
               said the peer-side program existed — that `windowproxy.get` would carry the decimal index in its
               member field, because that operation ran `globalThis[key]` and §7.2.2.2 Indexed access on the
               Window object's indexed access on the peer's global IS this same step performed there. It does
               not run that program any more: `windowproxy.get` performs §7.2.1.3.4 CrossOriginGetOwnProperty-
               Helper's GETTER for a member of §7.2.1.3.1 CrossOriginProperties ( O )'s thirteen, selected in C
               from a per-realm capture, and remote_op.c's parse REFUSES any member name outside that list —
               which a decimal index is. §7.2.1.3.1 says so itself: "Indexed properties do not need to be
               safelisted in this algorithm, as they are handled directly by the WindowProxy object", and they
               are cross-origin accessible under the separate sentence that ends "or an array index property
               name". An index is therefore a SECOND OPERATION on remote_op.c's grammar — its own verb, whose
               program is §7.2.2.2's own step in the peer's realm — and not a member field that happens to
               parse as a number.
               THE FIRST IS THE SUSPEND. This is an exotic [[GetOwnProperty]] — a C hook that must return a
               value — so it cannot park the flow the way `length` does, and `length` is only able to because
               it is an IDL ACCESSOR driven as a step machine. There is exactly one read shape this interpreter
               resolves at the operator site and drives on the trampoline (remote_object.c's block comment, and
               quickjs.c's own DCHECK in tramp_walk_continues names the route: the keyed entry's
               GP_GETOWNPROP). Route this class onto it and step 2 becomes a step machine whose ASK stage posts
               (document, world+ancestry, index) exactly as proxy_get_step does and whose ANSWER stage is
               remote_object_decode of the identity that comes back — parked at `otherW[0]`, resumed
               byte-identical, exactly as an await is. Answering `undefined` here is what this hook exists to
               stop, and the SecurityError below is right only once the index is known to be out of range. */
            DFAIL("an INDEXED read of a WindowProxy whose active document is in ANOTHER INSTANCE. The ANSWER "
                  "edge exists — a navigable crosses back as its IDENTITY and resolves to one WindowProxy per "
                  "document (remote_object.c) — and TWO edges do not. (1) The PEER-SIDE PROGRAM: an array "
                  "index is not one of §7.2.1.3.1 CrossOriginProperties ( O )'s thirteen entries (the standard "
                  "excludes them by name: \"Indexed properties do not need to be safelisted in this "
                  "algorithm\"), so `windowproxy.get` cannot carry one and remote_op.c's parse refuses it — "
                  "build a SECOND VERB whose program is §7.2.2.2 Indexed access on the Window object's own "
                  "step in the peer's realm. (2) The SUSPEND, which an exotic [[GetOwnProperty]] cannot do "
                  "because it is a C hook that must return a value. Route this class onto the keyed entry's "
                  "GP_GETOWNPROP — quickjs.c's tramp_walk_continues names that route in its own DCHECK — so "
                  "step 2 is driven on the trampoline as a step machine, the way every other cross-instance "
                  "read in this engine is");
        } else if (p->realm) {
            child = iframe_child_navigable(p->realm, (int)idx);
        }
    }
    if (!JS_IsUndefined(child)) {                                  /* step 2.3 */
        if (!desc) { JS_FreeValue(ctx, child); return 1; }
        /* Step 2.5's descriptor exactly: [[Writable]] FALSE, [[Enumerable]] true, [[Configurable]] true — the
           same three §7.2.2.2's global spelling reports, because it is the same step. */
        desc->flags  = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE;
        desc->getter = JS_UNDEFINED;
        desc->setter = JS_UNDEFINED;
        desc->value  = child;
        return 1;
    }
    if (proxy_same_origin(p)) return 0;                            /* step 2.4.1 */
    JS_ThrowDOMException(ctx, "SecurityError",                     /* step 2.4.2 */
                         "the origins do not permit reading an indexed property of that Window");
    return -1;
}

/* ---- §7.2.3's SAME-ORIGIN BRANCH — the operation is PERFORMED ON W -----------------------------------------
 *
 * EVERY ONE of §7.2.3's internal methods says the same thing for a same-origin W, and it is not "answer the
 * same value": it is `W.[[X]](…)`. So `frame.contentWindow.addEventListener` is the OTHER document's Window's
 * own member, `frame.contentWindow.onunload = f` runs the OTHER document's handler setter, and the handler map
 * that setter stores lands on the OTHER document's Window — which is what makes the handler FIRE, since that
 * Window is what the event path walks. A forward of the VALUE would have passed the round-trip test and left
 * the listener on a stand-in nothing dispatches to.
 *
 * WHAT IS NOT FORWARDED IS §7.2.3'S OWN SURFACE, and that carve-out is this engine's Window/WindowProxy
 * duality rather than a softening of the standard. `window` in a realm IS that realm's global here (win_or_proxy
 * states the mapping once), so `otherW.parent` has to be answered by the READING realm's member — forwarded, it
 * would run the other realm's `parent` getter, which maps ITS own navigable onto ITS global and hands back a
 * proxy the reading page has never seen, making `frame.contentWindow.parent === window` false. The same
 * sentence covers `self`, `top`, `frames`, `window` and `globalThis`.
 *
 * THE CARVE-OUT IS ASKED OF THE PROTOTYPE, WHICH IS THAT SURFACE. A list written out here would be a second
 * copy of window_proxy_install_proto's — one that `postMessage` (window_message.c installs it) was never on,
 * and the first name it fell behind is a name this component would hand to the other document's Window while
 * its own member sat one link up the chain answering nothing. `focus` and `blur` were the proof: they are on
 * §7.2.1.3.1's list, this surface had NEITHER, and a same-origin read of them forwarded to the Window's own
 * method while a CROSS-ORIGIN read — which cannot forward, since there is no local W — walked to this object
 * and found nothing. They are on the surface now (focus_install_window_members), so both spellings answer, and
 * the carve-out below covers them like every other name this object owns.
 *
 * §7.2.3.1's [[GetPrototypeOf]] IS BUILT (proxy_get_prototype), AND IT IS NOT YET THE WHOLE OF THE CHAIN.
 * REFLECTION asks it — `Object.getPrototypeOf(otherW)`, `Reflect.getPrototypeOf`, `otherW.__proto__`,
 * 7.3.21 OrdinaryHasInstance's walk behind `instanceof`, and 20.1.3.3's `isPrototypeOf` — so those answer
 * W's own prototype for a W the reader may see and null for one it may not, which is the standard's answer and
 * was the READING realm's surface object before.
 * THE ORDINARY LOOKUP WALKS STILL FOLLOW THE SHAPE LINK, which is this object's class prototype: 10.1.8.1
 * OrdinaryGet step 3, 10.1.9.2 step 2.a and 10.1.7 step 3 each say `O.[[GetPrototypeOf]]()` and quickjs's
 * walks read the stored link instead (JS_GetPrototype's own comment states it from that side). That is why a
 * member of the other document reached ONLY up a chain is still not reached, and why the link must keep
 * pointing where it does for now: §7.2.3.5 steps 4-6 let a CROSS-ORIGIN read walk to this same surface for
 * §7.2.1.3.1's names, so routing those walks through §7.2.3.1 — which answers null there — would take
 * `otherW.postMessage` away from every page. Which members are affected is window.c's answer and not this
 * one's: Web IDL §3.7.3 Interface prototype object makes every member of a [Global] object an OWN property of
 * it (which is why `window.hasOwnProperty("addEventListener")` is true in every browser, and why
 * event_target.c places its three there), and window.c still declares part of Window's surface on
 * Window.prototype instead. That placement is what closes this: with every member own, the cross-origin
 * surface no longer needs to be reachable up a chain, and the walks can ask §7.2.3.1 like everything else. */

/* §7.2.3's OWN SURFACE FOR ONE NAME, AND IT IS AN OWN PROPERTY OF THIS OBJECT — not a prototype hit.
 *
 * §7.2.3 The WindowProxy exotic object says it in one line: "There is no WindowProxy interface object." Every
 * name a WindowProxy answers is answered by §7.2.3.5's own [[GetOwnProperty]] and listed by §7.2.3.10's own
 * [[OwnPropertyKeys]], so a member reachable ONLY up a prototype chain is a member
 * `Object.getOwnPropertyDescriptor(w, n)` answers `undefined` for while `w.n` answers a value, and one
 * `Object.getOwnPropertyNames(w)` never mentions. That is ONE FACT ANSWERED FROM TWO PLACES — the Window owns
 * all thirteen of §7.2.1.3.1's names and this object's surface owned eleven of them, missing `focus` and
 * `blur` — and the two had already disagreed about `name` once (window.c's own comment records it). Both
 * surfaces carry the thirteen now, and proxy_get_own's steps 4-6 assert it rather than this sentence claiming
 * it: a count in a comment is a measurement that stops being re-taken the moment it is written down.
 *
 * THE OBJECT IS STILL THE CLASS PROTO, and that is a second ROLE rather than a second surface: §7.2.3.5's
 * cross-origin branch (steps 4-6) answers `0` for §7.2.1.3.1's names and lets the walk reach the same members,
 * which is this engine's stand-in for §7.2.1.3.4 CrossOriginGetOwnPropertyHelper's "anonymous built-in
 * function, created in the current realm". One object, two ways in, one answer either way.
 *
 * The query runs none of the page's code and no walk: its OWN properties only, because Object.prototype's
 * members are exactly the ones the forward should be answering out of the other realm.
 * 1 = the surface answers it (*desc filled and OWNED when desc != NULL), 0 = it does not. */
static int proxy_surface_desc(JSContext *ctx, JSPropertyDescriptor *desc, JSAtom prop)
{
    JSValue surface = JS_GetClassProto(ctx, g_proxy_class);
    JSPropertyDescriptor d;
    int has;

    DCHECK(JS_IsObject(surface),
           "a WindowProxy property was read in a realm that never ran window_proxy_install_proto — §7.2.3's "
           "surface is per realm, so a realm without one cannot say which names it answers");
    has = JS_GetOwnSlotDesc(ctx, &d, surface, prop);
    JS_FreeValue(ctx, surface);
    DCHECK(has >= 0,
           "§7.2.3's own surface threw on an own-property read of an ORDINARY object — the surface is built by "
           "window_proxy_install_proto and holds nothing but installed members, so a throw here means the "
           "class-proto slot holds something that is not that object");
    if (has <= 0) return 0;
    if (desc) {
        *desc = d;
    } else {
        JS_FreeValue(ctx, d.value);
        JS_FreeValue(ctx, d.getter);
        JS_FreeValue(ctx, d.setter);
    }
    return 1;
}

/* W — the ACTIVE DOCUMENT'S Window every one of §7.2.3's internal methods is performed on, or JS_UNINITIALIZED
   when there is none to perform anything on: a cross-origin proxy (§7.2.1 filters its members instead), or a
   destroyed navigable, which has no active document.
   IT ASKS NO PROPERTY NAME, because §7.2.3.10 asks this same question without one — the key list is W's whole
   own-key list and there is no name to test it against.
   BORROWED — the proxy holds it, and the ProxyData record it lives in rides the COW delta, so the answer is the
   RUNNING FLOW's: an arm that navigated the frame forwards to the Window it navigated to and its sibling to the
   one it did not. */
static JSValueConst proxy_target_window(JSContext *ctx, JSValueConst obj, ProxyData *p)
{
    if (!proxy_same_origin(p)) return JS_UNINITIALIZED;
    if (wp_closed(p)) return JS_UNINITIALIZED;
    /* MATERIALIZED HERE, INCLUDING FROM THE CAPTURE, and that uniformity is deliberate: `delete otherW.x`
       captures at the head of the delete before anything walks, so a capture that refused to build would name
       this proxy for a delete that lands on the Window a moment later — an entry over storage the operation
       never touches. This is proxy_realm's own contract (a navigable with an address is materialized at
       creation, one still holding its initial about:blank when something reads THROUGH it), and a property
       operation on this object is exactly such a read. It runs no page code: the initial about:blank Document
       has no script to run. */
    if (!p->realm)
        proxy_realm(ctx, obj, p);
    DCHECK(!JS_IsUndefined(p->window),
           "a same-origin WindowProxy whose realm this agent holds carries no Window — proxy_realm sets the "
           "two together, so one without the other is a binding something else assembled");
    return p->window;
}

/* The same W, for a KEYED operation — JS_UNINITIALIZED additionally for a name §7.2.3's own surface answers,
   which is this object's own and is not performed on W. */
static JSValueConst proxy_forward_window(JSContext *ctx, JSValueConst obj, ProxyData *p, JSAtom prop)
{
    if (!proxy_same_origin(p)) return JS_UNINITIALIZED;
    if (proxy_surface_desc(ctx, NULL, prop)) return JS_UNINITIALIZED;
    return proxy_target_window(ctx, obj, p);
}

/* WHERE A FORWARDED WRITE LANDS, for the COW delta — see JSClassExoticMethods.forwarded_object. The delta must
   name the Window the define below writes, never this stand-in: an entry naming the proxy restores its baseline
   ONTO the proxy as a real own property, which from then on shadows the Window for every flow. */
/* THE RECEIVER OF A CLASS HOOK IS NOT §3.7.6's QUESTION, AND THAT IS WHY THIS IS AN ASSERT RATHER THAN A
 * RESOLUTION. A member installed on §7.2.3's surface is a Web IDL attribute and can be handed any receiver a
 * page can write, which is the sentence proxy_member_get reads. A hook in JSClassExoticMethods is reached by
 * quickjs off the object's OWN class id, so `obj` is a WindowProxy by construction and its opaque was set by
 * the only function that mints one — there is no fourth thing it can be, and `if (!p) return <default>` was
 * five more of the plausible datum this component has just been cleaned of: [[GetOwnProperty]] reporting "no
 * such property", [[Delete]] reporting success, [[DefineOwnProperty]] reporting refusal and [[OwnPropertyKeys]]
 * reporting an empty window, each indistinguishable from the real answer. The finalizer and the gc_mark keep
 * their test: those two DO run against an object whose opaque may never have been set, which is exactly why
 * they reach it through JS_GetOpaque instead of proxy_of. */
static JSValueConst proxy_forwarded_object(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    ProxyData *p = proxy_of(obj);

    DCHECK(p != NULL, "a WindowProxy class hook ran on an object carrying no navigable record");
    return proxy_forward_window(ctx, obj, p, prop);
}

/* HTML §7.2.3.1 [[GetPrototypeOf]] ( ) — three lines, and every one of them is above: "Let W be the value of
   the [[Window]] internal slot of this. If IsPlatformObjectSameOrigin(W) is true, then return !
   OrdinaryGetPrototypeOf(W). Return null."
   W IS proxy_target_window's, WHICH IS THE POINT. §7.2.1.3.3 IsPlatformObjectSameOrigin(O) is "the current
   settings object's origin is same origin-domain with O's relevant settings object's origin", and every one of
   §7.2.3's internal methods here asks it through the one proxy_same_origin. A sixth spelling written out in
   this function would be a second opinion about one fact — and the one thing it could not do is disagree
   usefully: if that answer is ever wrong it is wrong for [[GetOwnProperty]], [[Delete]], [[DefineOwnProperty]]
   and [[OwnPropertyKeys]] in the same breath, and it is fixed once.
   THAT ALSO SETTLES A NAVIGABLE WITH NO ACTIVE DOCUMENT (destroyed, or discarded by a close): there is no W to
   perform OrdinaryGetPrototypeOf on, so the answer is step 3's null, which is the same statement [[Delete]]
   reads as "nothing here to remove" and [[GetOwnProperty]] reads as "the surface answers".
   NO PAGE CODE: OrdinaryGetPrototypeOf is 10.1.1, a slot read, and the materialization proxy_target_window may
   do builds the initial about:blank Document, which has no script in it. That is the contract
   JSClassExoticMethods.get_prototype is declared under and the same one this class's [[GetOwnProperty]] already
   declares. */
static JSValue proxy_get_prototype(JSContext *ctx, JSValueConst obj)
{
    ProxyData *p = proxy_of(obj);
    JSValueConst w;

    DCHECK(p != NULL, "a WindowProxy class hook ran on an object carrying no navigable record");
    w = proxy_target_window(ctx, obj, p);                       /* step 1 */
    if (JS_IsUninitialized(w))
        return JS_NULL;                                         /* step 3 */
    /* step 2. W is a Window — an ORDINARY object, so this is 10.1.1 OrdinaryGetPrototypeOf and not a second
       trip through an exotic hook. Asserted rather than assumed: a W whose own [[GetPrototypeOf]] were exotic
       would make this recursive, and the one class in this engine that computes a prototype is this one. */
    DCHECK(!window_proxy_is(w),
           "§7.2.3.1's W is a WindowProxy — [[Window]] holds the Window a navigable's active document IS, and a "
           "proxy there would make OrdinaryGetPrototypeOf recurse through this hook");
    return JS_GetPrototype(ctx, w);
}

/* HTML §7.2.3.6 [[DefineOwnProperty]] — the write half of the branch above, and the reason a page can write to
   a same-origin frame's Window at all. Without it the assignment created an own property ON THIS OBJECT: the
   read forwarded, the write did not, and the two disagreed silently.
   ITS CROSS-ORIGIN ARM IS A THROW, which is step 3 and which nothing here used to say: with no hook at all, a
   cross-origin `Object.defineProperty(otherW, …)` SUCCEEDED, quietly, on the proxy. */
static int proxy_define_own(JSContext *ctx, JSValueConst obj, JSAtom prop, JSValueConst val,
                            JSValueConst getter, JSValueConst setter, int flags)
{
    ProxyData *p = proxy_of(obj);
    JSValueConst w;
    uint32_t idx;

    DCHECK(p != NULL, "a WindowProxy class hook ran on an object carrying no navigable record");
    if (!proxy_same_origin(p)) {                                        /* step 3 */
        JS_ThrowDOMException(ctx, "SecurityError",
                             "the origins do not permit defining a member of that Window");
        return -1;
    }
    if (proxy_atom_index(ctx, prop, &idx)) {                            /* step 2.1: FALSE for every index */
        /* HTML §7.2.3.6 step 2.1 IS "If P is an array index property name, return false." — one word, which
           the CALLERS turn into four different observables, so it is spoken once to the engine rather than
           decoded here. `flags & JS_PROP_THROW` decoded two of them: Object.defineProperty (ECMAScript §7.3.8
           DefinePropertyOrThrow step 2, "If success is false, throw a TypeError exception") and
           Reflect.defineProperty (ECMAScript §28.1.3 step 4, whose result IS the boolean). A STRICT assignment
           arrives carrying JS_PROP_THROW_STRICT, which that test does not read, so ECMAScript §6.2.5.6
           PutValue step 3.e — "If succeeded is false and refRecord.[[Strict]] is true, throw a TypeError
           exception" — never fired.
           RESIDUAL — §7.2.3.8's OWN REFUSAL IS NOT BUILT, AND THIS HOOK IS WHERE ITS ABSENCE IS PAID FOR.
           HTML §7.2.3.8 [[Set]] ( P , V , Receiver ) step 3.1 refuses an index one internal method EARLIER
           than this one — "If P is an array index property name, then return false" — and does it BEFORE
           OrdinarySet, so a browser consults nothing else. This class declares no `set_property`, so an
           assignment takes the ordinary route and arrives at THIS hook by two different roads: in range,
           §7.2.3.5's non-writable descriptor stops it at ECMAScript §10.1.9.2 OrdinarySetWithOwnDescriptor
           step 2.a; out of range, §7.2.3.5 leaves `value` undefined and returns undefined for a same-origin W,
           so proxy_get_own reports no own property and step 2.d.ii's CreateDataProperty lands here. Both roads
           end in `false`, which is why the code is right rather than unfinished.
           WHAT IS NOT COVERED: the ordinary route WALKS THE PROTOTYPE CHAIN before it reaches either road, and
           §7.2.3.8 step 3.1 never does.
           WHAT THE NEXT DIFF BUILDS: a `set_property` entry in PROXY_EXOTIC answering §7.2.3.8 — its step 2
           access-report, step 3.1's refusal, step 3.2's OrdinarySet on W, and step 4's CrossOriginSet.
           HOW ITS ABSENCE WOULD SHOW: define an index-named SETTER on Object.prototype and assign to that
           index on a WindowProxy. A browser refuses at step 3.1 and the setter never runs; here the prototype
           walk finds it and CALLS it, so the page observes a side effect no browser produces. */
        return JS_RefuseOrThrowTypeError(ctx, flags,
                                         "cannot define an indexed property on a WindowProxy");
    }
    w = proxy_forward_window(ctx, obj, p, prop);
    /* §7.2.3's own surface is this object's, so a define of one of those names is the ORDINARY define on this
       object with the exotic step suppressed — the same re-entry win_define_own makes, and for the same reason:
       an exotic hook REPLACES the ordinary path rather than preceding it. */
    if (JS_IsUninitialized(w))
        return JS_DefineProperty(ctx, obj, prop, val, getter, setter, flags | JS_PROP_NO_EXOTIC);
    return JS_DefineProperty(ctx, w, prop, val, getter, setter, flags);   /* step 2.2 */
}

/* HTML §7.2.3.9 [[Delete]] — the same three arms. Reached only after the ordinary own-property scan found
   nothing, which on this object is always unless a page has already replaced one of §7.2.3's own surface names
   through the define above. */
static int proxy_delete(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    ProxyData *p = proxy_of(obj);
    JSValueConst w;
    uint32_t idx;

    DCHECK(p != NULL, "a WindowProxy class hook ran on an object carrying no navigable record");
    if (!proxy_same_origin(p)) {                                        /* step 3 */
        JS_ThrowDOMException(ctx, "SecurityError",
                             "the origins do not permit deleting a member of that Window");
        return -1;
    }
    if (proxy_atom_index(ctx, prop, &idx)) return 0;                    /* step 2.1 */
    w = proxy_forward_window(ctx, obj, p, prop);
    if (JS_IsUninitialized(w)) {
        /* A DESTROYED navigable and a name §7.2.3's own surface does not answer: there is no active document
           to perform step 2.2 on and nothing here to remove, which is the truthful `true`. */
        if (!proxy_surface_desc(ctx, NULL, prop)) return 1;
        /* §7.2.3's OWN SURFACE, AND IT IS NOW AN OWN PROPERTY (see proxy_surface_desc), so `true` here is the
           answer that used to be true and is not any more: [[GetOwnProperty]] keeps reporting a CONFIGURABLE
           own property that [[Delete]] just claimed to remove. The surface object is shared by every proxy in
           the realm, so there is no per-proxy entry a delete could take out of it. What closes this is the same
           thing that closes §7.2.3.6's local define: route the operation to W (which owns every one of these
           names — proxy_own_keys asserts it) and let W's own descriptor be what §7.2.3.5 step 3 answers, which
           needs the reading realm's Window/WindowProxy identity mapping applied to the forwarded VALUE
           (win_or_proxy read backwards for a value produced in ANOTHER realm). */
        DFAIL("`delete` of one of §7.2.3's own surface names on a WindowProxy. The surface answers the name as "
              "an OWN, configurable property and is shared by every proxy in the realm, so nothing here can "
              "remove it and reporting success would leave [[Delete]] and [[GetOwnProperty]] disagreeing. "
              "Build §7.2.3.9 step 2.2's forward: perform the delete on W, and give §7.2.3.5 step 3 the "
              "identity mapping that lets W's own descriptor answer these names in the READING realm");
        return 1;
    }
    return JS_DeleteProperty(ctx, w, prop, 0);                          /* step 2.2 */
}

/* HTML §7.2.3.5 [[GetOwnProperty]] — STEP 2 FOR EVERY W, STEP 3 for a same-origin one, and steps 4 through 6
 * for a cross-origin one. Step 3 is the forward described above, and it is asked BEFORE §7.2.1.3.1's list
 * because the standard puts it there: the list belongs to steps 4-6, which a same-origin W never reaches. */
static int proxy_get_own(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom prop)
{
    ProxyData *p = proxy_of(obj);   /* CAPTURED: the origin this read is filtered by is per-flow state */
    JSValueConst w;
    uint32_t idx;
    int i;

    DCHECK(p != NULL, "a WindowProxy class hook ran on an object carrying no navigable record");
    /* A NAME TABLE THAT WAS NEVER FILLED COMPARES EVERY PROPERTY AGAINST JS_ATOM_NULL and matches none, which
       would refuse §7.2.1.3.1's whole list rather than answer it — a filter that throws for everything passes
       any test that only checks the throw. */
    DCHECK(g_xo_atom[0] != JS_ATOM_NULL && g_xo_fallback[0] != JS_ATOM_NULL,
           "a WindowProxy read reached §7.2.3.5 before this agent interned §7.2.1.3.1's property names — the "
           "class was registered without proxy_capture_names running beside it");

    /* STEP 2, WHICH IS FIRST BECAUSE THE STANDARD PUTS IT FIRST. Every path through it returns, so an array
       index reaches neither the cross-origin name list nor step 3 — and asking it here rather than after the
       name loop is what makes that ordering a fact of the code instead of a claim about the thirteen names
       being non-numeric. */
    if (proxy_atom_index(ctx, prop, &idx))
        return proxy_indexed_get_own(ctx, desc, p, idx);

    /* STEP 3: `OrdinaryGetOwnProperty(W, P)`, performed on the other document's Window. */
    w = proxy_forward_window(ctx, obj, p, prop);
    if (!JS_IsUninitialized(w))
        return JS_GetOwnPropertyNoUserCode(ctx, desc, w, prop);
    /* Same origin and NOT forwarded means §7.2.3's own surface answers the name (or the navigable is
       destroyed, whose readable members are answered out of this record). IT IS THIS OBJECT'S OWN PROPERTY:
       §7.2.3 has no interface object, so §7.2.3.5 is where every one of these names is answered and
       §7.2.3.10 is where every one is listed. Reporting `0` here and letting the prototype walk answer is
       what made `Object.getOwnPropertyDescriptor(popup, "opener")` undefined while `popup.opener` was the
       opener, and what left `Object.getOwnPropertyNames(popup)` empty for an object with fourteen members. */
    if (proxy_same_origin(p)) return proxy_surface_desc(ctx, desc, prop);

    /* STEPS 4-6, WHICH ONLY A CROSS-ORIGIN W REACHES — AND THE PERMIT SIDE OF THE FILTER, CHECKED IN THE SAME
       PASS AS THE REFUSE SIDE.
     *
     * Returning `0` here says "not an own property of this exotic object", which lets the ordinary walk reach
     * §7.2.3's surface — this object's class prototype — and answer out of it. That is only an ANSWER while the
     * surface OWNS the name: a listed name the surface does not own walks off the end of the chain and yields
     * `undefined`, which is precisely the value §7.2.1.3.2 CrossOriginPropertyFallback exists to forbid, and it
     * is indistinguishable from a member the peer's document does not have.
     *
     * THE REFUSAL WAS COMPLETE AND THE PERMISSION WAS NOT, WHICH IS WHY THIS IS ONE LOOP AND NOT TWO CHECKS. A
     * filter that throws for every name outside the list passes any test that only looks for the throw; the two
     * members that were missing (`focus` and `blur`) were on the list, refused nothing, and answered nothing.
     * The reconciliation in proxy_capture_names cannot see that class of gap at all — it iterates the MEMBER
     * TABLE, so a §7.2.1.3.1 name that no member of this component declares is not a row it visits. This loop
     * visits the STANDARD'S list, so a name is checked from both ends: it is refused unless listed, and it is
     * answerable because listed. Side-effect-free — proxy_surface_desc with a NULL descriptor reads one own
     * property of an ordinary object and frees what it took. */
    for (i = 0; i < CROSS_ORIGIN_NAME_N; i++)
        if (prop == g_xo_atom[i]) {
            DCHECKF(proxy_surface_desc(ctx, NULL, prop) == 1,
                    "HTML §7.2.1.3.1 CrossOriginProperties lists `%s` among the cross-origin accessible window "
                    "property names, and §7.2.3's own surface for this realm does not own it — so this read is "
                    "PERMITTED and then answered with `undefined` by a prototype walk that finds nothing, which "
                    "is the one answer §7.2.1.3.2 CrossOriginPropertyFallback rules out (its last step is the "
                    "SecurityError, and a page tells the two apart). §7.2.1.3.4 CrossOriginGetOwnPropertyHelper "
                    "runs OrdinaryGetOwnProperty on the Window, so the member exists and only this surface is "
                    "missing it: install it from the component that owns the member onto the object "
                    "window_proxy_install_proto builds, the way `close`, `postMessage` and §6.6.6's `focus` and "
                    "`blur` are", CROSS_ORIGIN[i].name);
            return 0;
        }

    for (i = 0; i < XO_FALLBACK_N; i++)
        if (prop == g_xo_fallback[i]) {
            if (!desc) return 1;
            /* §7.2.1.3.2 step 1's descriptor exactly: undefined, non-writable, non-enumerable, CONFIGURABLE. */
            desc->flags = JS_PROP_CONFIGURABLE;
            desc->value = JS_UNDEFINED;
            desc->getter = JS_UNDEFINED;
            desc->setter = JS_UNDEFINED;
            return 1;
        }

    /* §7.2.1.3.2 step 2. */
    JS_ThrowDOMException(ctx, "SecurityError",
                         "the origins do not permit reading this member of that Window");
    return -1;
}

/* §7.2.3's OWN SURFACE AS A KEY LIST — the answer for a navigable with no active document to walk. */
static int proxy_surface_keys(JSContext *ctx, JSPropertyEnum **ptab, uint32_t *plen)
{
    JSValue surface = JS_GetClassProto(ctx, g_proxy_class);
    int r;

    DCHECK(JS_IsObject(surface),
           "§7.2.3's own surface was listed in a realm that never ran window_proxy_install_proto");
    r = JS_GetOwnPropertyNames(ctx, ptab, plen, surface, JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK);
    JS_FreeValue(ctx, surface);
    return r;
}

/* THE ONE-SURFACE INVARIANT, ASSERTED WHERE THE TWO SURFACES ARE RECONCILED. §7.2.3.10's key list below is W's
   own key list and nothing else contributes to it, while §7.2.3.5 answers §7.2.3's own surface names out of the
   reading realm's surface object. Those are two member surfaces for one navigable, and they are only ONE fact
   as long as every name the surface answers is also an own property of W: a name on the surface that W does not
   own is a property this object REPORTS and never LISTS — the JS-visible half of the defect that had the
   Window owning all thirteen of §7.2.1.3.1's names while this component's surface owned eleven, and that had
   already made the two disagree about `name`. Side-effect-free: it reads two key lists and frees them. */
static bool proxy_surface_within(JSContext *ctx, const JSPropertyEnum *tab, uint32_t len)
{
    JSPropertyEnum *stab = NULL;
    uint32_t slen = 0, i, j;
    bool ok = true;

    if (proxy_surface_keys(ctx, &stab, &slen) < 0) return false;
    for (i = 0; i < slen && ok; i++) {
        ok = false;
        for (j = 0; j < len; j++)
            if (stab[i].atom == tab[j].atom) { ok = true; break; }
    }
    JS_FreePropertyEnum(ctx, stab, slen);
    return ok;
}

/* AND THE INDEX PREFIX IS THE RANGE §7.2.3.5 STEP 2 WALKS — the other half of the same one-list rule, over the
   same document's child navigables. A gap or a duplicate in it is a frame this object lists twice or not at
   all, against a [[GetOwnProperty]] that answers for every index in the range exactly once. Side-effect-free:
   it re-reads atoms it was handed. */
static bool proxy_index_prefix_is_range(JSContext *ctx, const JSPropertyEnum *tab, uint32_t len, uint32_t n)
{
    uint32_t i;

    if (len < n) return false;
    for (i = 0; i < n; i++) {
        uint32_t idx = 0;
        if (!proxy_atom_index(ctx, tab[i].atom, &idx) || idx != i) return false;
    }
    return true;
}

/* HTML §7.2.3.10 [[OwnPropertyKeys]] ( ) — and it is not decoration beside §7.2.3.5, it is the half that makes
 * §7.2.3.5's answers checkable. An object that answers [[GetOwnProperty]] for a name it never LISTS is one
 * whose `Object.getOwnPropertyNames` and whose `Object.getOwnPropertyDescriptor` disagree, and a page reads the
 * difference directly: `assert_own_property(popup, "opener")`.
 *
 * THE LIST HAS ONE SOURCE, WHICH IS THE POINT. §7.2.3.10's step 4 is "the concatenation of keys and
 * OrdinaryOwnPropertyKeys(W)" (step 5 is its cross-origin twin), and steps 2-3's `keys` is the range 0 to W's
 * associated Document's document-tree child navigables's size. THIS ENGINE'S WINDOW ANSWERS THAT RANGE: §7.2.2.2 Indexed
 * access on the Window object says "indexed access to document-tree child navigables is defined through the
 * [[GetOwnProperty]] internal method of the WindowProxy object", and window.c answers the same range for the
 * GLOBAL spelling of the same navigable out of the same walk (win_own_names). So the range is already IN the
 * list below, and re-deriving it here would be the second answer that disagrees with the first for exactly the
 * documents whose frame count changed between the two walks.
 *
 * WHAT §7.2.3.10 STATES THAT THE MERGE DOES NOT IS THE ORDER. quickjs appends a class's exotic keys AFTER the
 * object's ordinary ones, so the range comes out last; §7.2.3.10 puts it first, and so does ECMA-262
 * 10.1.11.1 OrdinaryOwnPropertyKeys, whose step 2 lists every integer index in ascending numeric order before
 * any string key. A stable partition plus an ascending sort of the prefix is the whole of it. */
static int proxy_own_keys(JSContext *ctx, JSPropertyEnum **ptab, uint32_t *plen, JSValueConst obj)
{
    ProxyData *p = proxy_of(obj);
    JSValueConst w;
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0, i, j, k, n;

    *ptab = NULL;
    *plen = 0;
    DCHECK(p != NULL, "a WindowProxy class hook ran on an object carrying no navigable record");

    if (!proxy_same_origin(p)) {
        /* §7.2.3.10 step 5, AND THE ONE THING IT NEEDS THAT DOES NOT EXIST. This used to name TWO, and the
           second is CLOSED: §7.2.1.3.7 CrossOriginOwnPropertyKeys's list is §7.2.1.3.1's thirteen names, of
           which this surface owned eleven — `focus` and `blur` were the Window's alone — and it owns all
           thirteen now, asserted per name in proxy_get_own's steps 4-6 rather than restated here. What remains
           is step 2's maxProperties: the child-navigable count of an active document this instance does not
           hold, which is the same cross-instance read proxy_indexed_get_own already names. An exotic
           [[OwnPropertyKeys]] is a C hook that must return a list, so it cannot park the flow the way an IDL
           accessor driven as a step machine can, and a list published without that count would state a frame
           range this side invented. */
        DFAIL("§7.2.3.10 [[OwnPropertyKeys]] of a CROSS-ORIGIN WindowProxy. ONE thing is missing and it is "
              "named: step 2's maxProperties is the child-navigable count of an active document ANOTHER "
              "instance holds. Route this onto the keyed entry's GP_GETOWNPROP so the flow suspends, exactly "
              "as proxy_indexed_get_own names for §7.2.3.5 step 2, and then concatenate that index range with "
              "§7.2.1.3.7 CrossOriginOwnPropertyKeys's thirteen names — which this surface now owns in full, "
              "so every published key has a descriptor behind it. (The `focus`/`blur` half of this crash's "
              "old reason is BUILT; if you are reading it as an absence, re-read the loop in proxy_get_own.)");
        return 0;
    }

    w = proxy_target_window(ctx, obj, p);
    if (JS_IsUninitialized(w))
        /* A DESTROYED navigable has no active document, so steps 2-4 have nothing to walk. What it still owns
           is exactly what §7.2.3.5 still answers out of this record: §7.2.3's own surface. */
        return proxy_surface_keys(ctx, ptab, plen);

    if (JS_GetOwnPropertyNames(ctx, &tab, &len, w, JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) < 0)
        return -1;
    DCHECK(proxy_surface_within(ctx, tab, len),
           "a name §7.2.3's own surface answers is NOT an own property of the Window this proxy forwards to. "
           "The key list is W's and the descriptors are the surface's, so that name is a property this object "
           "reports to `Object.getOwnPropertyDescriptor` and never lists in `Object.getOwnPropertyNames` — one "
           "navigable with two member surfaces, which is the shape §7.2.1.3.1's list and this component's "
           "member table are already tied together by an assert to prevent");

    /* Steps 2-3's `keys` FIRST: a stable partition of the array-index property names to the front. */
    for (i = 0, j = 0; i < len; i++) {
        uint32_t idx;
        if (!proxy_atom_index(ctx, tab[i].atom, &idx)) continue;
        if (i != j) {
            JSPropertyEnum t = tab[i];
            memmove(&tab[j + 1], &tab[j], (size_t)(i - j) * sizeof *tab);
            tab[j] = t;
        }
        j++;
    }
    /* ASCENDING, which the partition does not give on its own: the shape's own numeric keys and the Window
       exotic's range are two ascending runs laid end to end. An insertion sort over the prefix — j is the
       document's frame count, so this is a walk of the frames and not of the whole surface. */
    for (i = 1; i < j; i++) {
        JSPropertyEnum t = tab[i];
        uint32_t ti = 0, ki;
        proxy_atom_index(ctx, t.atom, &ti);
        for (k = i; k > 0; k--) {
            ki = 0;
            proxy_atom_index(ctx, tab[k - 1].atom, &ki);
            if (ki <= ti) break;
            tab[k] = tab[k - 1];
        }
        tab[k] = t;
    }
    /* AND THE RANGE IS THE ONE §7.2.3.5 STEP 2 WALKS. Two lists over one document's child navigables, and the
       whole reason this hook takes W's rather than building its own: a key `Object.getOwnPropertyNames` reports
       and `[[GetOwnProperty]]` refuses (or the reverse) is the two-answer defect one level down. */
    n = (uint32_t)iframe_child_navigable_count(p->realm);
    DCHECK(proxy_index_prefix_is_range(ctx, tab, len, n),
           "§7.2.3.10 steps 2-3's `keys` is not the range 0 to maxProperties at the head of the key list — the "
           "range and §7.2.3.5 step 2's indexed answers are one walk of one document's child navigables, so a "
           "gap, a duplicate or a short list here is a frame this object answers for and never lists");
    *ptab = tab;
    *plen = len;
    return 0;
}

/* HTML §7.2.3.4 [[PreventExtensions]] ( ): "Return false."
   IT IS THE ONE OF §7.2.3's SET THAT NEEDS NEITHER THE FORWARD NOR THE FILTER. Every other hook below reaches
   the Window behind this proxy — §7.2.3.5 and §7.2.3.6 through their same-origin arms, §7.2.3.10 through the
   child-navigable walk — and this one answers out of §7.2.3 itself, for every origin, because a WindowProxy is
   never the object whose extensibility a page gets to fix. Its twin is §7.2.3.3 [[IsExtensible]] ( ) — "Return
   true." — which quickjs answers from the `extensible` flag, and which is only STILL true because this refusal
   is asked BEFORE that flag is cleared: a freeze that succeeded would have made `Object.isExtensible(frames[0])`
   false, which §7.2.3.3 says it never is. It is also the only entry in the table below that never resolves the
   [[Window]] slot — every other one opens on proxy_of, proxy_target_window or proxy_realm — which is what makes
   it the one hook here with nothing to assert and nothing that could reach another agent. */
static int proxy_prevent_extensions(JSContext *ctx, JSValueConst obj)
{
    (void)ctx; (void)obj;
    return 0;
}

/* THE SIX OF §7.2.3's INTERNAL METHODS THIS OBJECT ANSWERS ITSELF — §7.2.3.1 [[GetPrototypeOf]], §7.2.3.4
   [[PreventExtensions]], §7.2.3.5 [[GetOwnProperty]], §7.2.3.6 [[DefineOwnProperty]], §7.2.3.9 [[Delete]] and
   §7.2.3.10 [[OwnPropertyKeys]], with §7.2.3.7's and §7.2.3.8's same-origin `W.[[X]]` reached through the
   forward — and one declaration about all of them: no page code, so the engine's own accessor walks may run
   them from C with no flow base under them. That claim covers the FORWARD as well as the filter, and it still
   holds — the Window it forwards to declares the same thing of its own hook (window.c's WINDOW_EXOTIC, whose
   named access is a walk of the document tree), and materializing an initial about:blank Document runs no
   script because there is none to run. §7.2.3.1 declares it for the plainest reason of the set: 10.1.1
   OrdinaryGetPrototypeOf is a slot read, and §7.2.3.4's is plainer still: a constant. */
static const JSClassExoticMethods PROXY_EXOTIC = {
    .get_own_property = proxy_get_own,
    .get_own_property_names = proxy_own_keys,
    .delete_property = proxy_delete,
    .define_own_property = proxy_define_own,
    .get_prototype = proxy_get_prototype,
    .prevent_extensions = proxy_prevent_extensions,
    .get_own_property_no_user_code = true,
    .forwarded_object = proxy_forwarded_object,
};

/* §7.2.2.4's `top`: the TOP-LEVEL traversable's proxy. Walked rather than stored, because a navigable's parent
   chain is the only place the answer lives and a cached one goes stale the moment a frame is reparented. */
JSValue window_proxy_top_navigable(JSContext *ctx, JSValueConst self)
{
    JSValueConst cur = self;

    for (;;) {
        ProxyData *q = JS_GetOpaque(cur, g_proxy_class);
        if (!q || JS_IsUndefined(q->parent)) return JS_DupValue(ctx, cur);
        /* THE CHAIN LEAVES THE PROXIES at this instance's own Window, which is not one — it is the global, and
           the global answers `top` for itself. Following it is what makes a grandchild's `top` this document
           rather than its parent frame. */
        /* THE CHAIN IS PROXIES ALL THE WAY UP, because §7.2.2.4 says `parent` IS one. It used to hold the
           creator's GLOBAL, so this walk left the proxies at the top and read `top` off a Window — a
           scriptable property read from a C activation, which is the one thing this interpreter refuses the
           moment that property stops being a frozen value. Storing what the spec says deletes the branch. */
        DCHECK(window_proxy_is(q->parent),
               "a navigable's parent is not a WindowProxy — §7.2.2.4 says it is one, and a walk that has to ask "
               "what kind of object it reached is a walk that will read a scriptable property to continue");
        cur = q->parent;
    }
}

/* THE SCRIPTABLE `top` AND THE TOP NAVIGABLE ARE NOT THE SAME ANSWER, and this is the one place that is true
   rather than pedantry. `window.top` of the asking realm's own navigable is that realm's GLOBAL (win_or_proxy
   below), because `window === window.top` is an identity every page rests on; the TREE WALK that HTML
   §8.1.7.3 step 2 makes wants the top-level traversable's NAVIGABLE, which is a WindowProxy in every case
   including that one. A walk given the scriptable answer reaches a Window, asks "is this a proxy", is told no,
   and silently collects NO documents — which is exactly what happened: the rendering loop found nothing to
   render on every page. */

/* THE ASKING REALM STANDS FOR ITSELF AS ITS GLOBAL — the one mapping between the two spellings of a window,
   stated once and used by every member that can answer with a navigable.
   In the spec `window`, `self`, `frames`, `parent` and `top` of a top-level navigable are ALL one object, its
   WindowProxy. Here the global is what a page holds as `window`, so answering with the asking realm's OWN
   proxy would make `window === window.parent` false at top level and `frame.contentWindow.parent === window`
   false in the corpus's own iframe probe — an identity every page rests on. Another navigable is its PROXY;
   this one is the global. Putting the rule anywhere but here means writing it twice, and the second copy is
   the one that gets forgotten: the first attempt at this had it only on the Window side and the proxy side
   answered with a proxy the page had never seen. */
static JSValue win_or_proxy(JSContext *ctx, JSValue v)
{
    /* ONLY AN OBJECT CAN NAME A NAVIGABLE, and this mapping is asked about values that are not one: §7.2.2.4's
       `opener` is null for a navigable nothing opened, and `parent`/`top` are null for a destroyed one. A
       JSValue carrying no pointer has no pointer to read — JS_MKVAL initialises the union's `int32` and leaves
       the rest of it unspecified — so comparing one against this realm's proxy is a comparison against
       whatever the other half of that union happens to hold. */
    if (JS_IsObject(v) && JS_VALUE_GET_PTR(v) == JS_VALUE_GET_PTR(document_window_proxy(ctx))) {
        JS_FreeValue(ctx, v);
        return JS_GetGlobalObject(ctx);
    }
    return v;
}

/* HTML §7.2.2 "The Window object": "A Window's navigable is the navigable whose ACTIVE DOCUMENT IS the
 * Window's associated Document's, or null if there is no such navigable." §7.2.2.4 opens `top`, `parent` and
 * `frameElement` with a null test over it, so it is ONE question three members ask, asked in one place.
 *
 * IT BECOMES TRUE AT TWO DIFFERENT TIMES AND THE EARLIER ONE IS THE ONE PAGES READ. §7.5.10 "Destroying
 * documents" step 9 — "set document's node navigable's active session history entry's document state's
 * document to null" — is what makes the reverse lookup fail, and §7.5.10's descendant form performs its steps
 * IN PARALLEL and queues a global task per document to run them. So `destroyed` is not true until a job has
 * run. But §7.3.1.6 "Navigable destruction"'s destroy-a-child-navigable step 3, "set container's content
 * navigable to null", is SYNCHRONOUS, inside the tree mutation (core/html/html_iframe.c clears the element's
 * slot on the removing steps' own line): from that instant nothing in the navigable tree names the navigable,
 * and §7.2.2.4's own closing example reads exactly that on the NEXT LINE —
 *
 *     element.remove();
 *     console.assert(iframeWindow.top === null);
 *
 * which html/browsers carries twice over (window-top-null.html, window-parent-null.html). A test that asked
 * `destroyed` alone would keep answering the window that used to be above it until a task ran, which is a real
 * object in place of the null and the one shape a page cannot tell from an answer.
 *
 * SO IT IS A WALK, AND IT HAS TO BE. Removing one `<iframe>` severs ONE link; every navigable below it keeps
 * its own container element, because the inner document is not touched at all. A grandchild is therefore
 * detached by an ANCESTOR's sever and by nothing of its own, and window-top-null.html's second subtest is that
 * case exactly (`frame2Window.top` after the OUTER frame is removed).
 *
 * §7.3.1.3's CONTAINER ALREADY ANSWERS PER FLOW, which is why nothing new is recorded here. window_proxy_container
 * confirms the navigable's stored container against THAT ELEMENT's own content navigable, so it reports the
 * running flow's severance rather than some other arm's — and it keeps reporting it after the removed element
 * is appended back, because §4.8.5's post-connection steps create a NEW navigable and the element's slot then
 * names that one, so the old proxy's confirm fails for ever. The corpus asserts both halves.
 *
 * A CONTAINER IN ANOTHER INSTANCE IS NOT AN ABSENT ONE. SECURITY.md keys an instance on
 * `(browsing context group, origin)`, so the navigable an instance is ROOTED in is routinely a cross-origin
 * `<iframe>` whose element belongs to the CREATING instance — its §7.3.1.3 link crosses as an ANSWER rather
 * than an object (window_proxy_remote_container), and its local `container` is null because there is no local
 * element to hold. Reading that null as a sever would report every cross-origin frame's own document as
 * detached from itself. The walk stops at that link instead: what is above it is the peer's tree, and the peer
 * is the agent that can answer for it.
 *
 * Runs no page code: proxy_of is a capture and window_proxy_container is one own-slot read of a wrapper, which
 * is what lets this be asked from a plain C getter with no flow base under it.
 *
 * §7.2.2.4's `frameElement` asks it about the NODE NAVIGABLE, which is the Window's associated Document's and
 * so the same navigable and the same sentence — core/frame/window.c reads it through the declaration in
 * window_proxy.h rather than testing a field of its own. */
bool window_proxy_navigable_null(JSContext *ctx, JSValueConst proxy)
{
    JSValueConst cur = proxy;

    for (;;) {
        ProxyData *q = proxy_of(cur);   /* CAPTURED: severance is per-flow state this read may precede */
        JSValue container;

        DCHECK(q != NULL,
               "§7.2.2's \"a Window's navigable\" was walked through something that is not a WindowProxy — "
               "every link this climbs is `parent`, and §7.2.2.4 says a navigable's parent is one");
        if (q->destroyed) return true;
        /* §7.3.1.3 "Child navigables": a navigable "is a child navigable", "which means that its parent is
           non-null". A top-level traversable is at the top of the tree, so it is attached by being one. */
        if (!window_proxy_is(q->parent)) return false;
        if (window_proxy_remote_container(cur) != NULL) return false;
        container = window_proxy_container(ctx, cur);
        if (JS_IsNull(container)) return true;   /* §7.3.1.6 step 3 ran, in THIS flow */
        JS_FreeValue(ctx, container);
        /* BORROWED, exactly as window_proxy_top_navigable borrows: each link is held by the record of the
           navigable below it, and that record is held by the object this iteration is standing on. */
        cur = q->parent;
    }
}

/* §7.2.3's OWN SURFACE ANSWERING ONE MEMBER — and its receiver is §3.7.6's, not this object.
 *
 * IT RETURNED `JS_UNDEFINED` FOR A RECEIVER IT COULD NOT MAP, and that is the defaulted-field defect performed
 * on the ES receiver rule: a member of §7.2.1.3.1's cross-origin list answered `undefined` where §7.2.2.4
 * answers `null`, and a page reads the difference directly — `window.opener === null` is how a document asks
 * whether it is a popup, and `undefined` sends it down the other arm. The reach is not exotic: §7.2.3.5 step 3
 * hands `Object.getOwnPropertyDescriptor(w, "opener")` an ACCESSOR, so `desc.get()` is an ordinary call with no
 * receiver, which §3.7.6 resolves to the getter's own realm's global and this resolved to nothing at all. */
static JSValue proxy_member_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValueConst nav = window_proxy_this_navigable(ctx, this_val);
    ProxyData *p;

    DCHECK(magic >= 0 && magic < WP_MEMBER_N,
           "a WindowProxy member was declared with a magic this component does not name");
    if (JS_IsUninitialized(nav)) return JS_EXCEPTION;   /* §3.7.6's TypeError, already thrown */
    p = proxy_of(nav);   /* CAPTURED: `closed` and `name` are per-flow state a read may precede */
    DCHECK(p != NULL,
           "§3.7.6's receiver resolved to something that is not a WindowProxy — window_proxy_this_navigable "
           "answers with a navigable or throws, so a third outcome is a mapping that grew an arm without "
           "telling this member what it may now be handed");

    /* A LOCAL PROXY CAN STILL BE CROSS-ORIGIN — this agent is one ORIGIN, but a proxy it holds may name a
       navigable it navigated elsewhere — so §7.2.1's check comes first either way. */
    if (!proxy_read_permitted(p, magic))
        return JS_ThrowDOMException(ctx, "SecurityError",
                                    "the origins do not permit reading this member of that Window");

    /* ONE ANSWER FOR EVERY PROXY, LOCAL OR REMOTE. Every member below is the NAVIGABLE'S OWN STATE, and a
       navigable belongs to the agent that created it — this one — whether or not its active document does.
       There used to be a local branch that DELEGATED to the target Window's property of the same name, and it
       was wrong twice over: `otherW.self` answered the other realm's GLOBAL where the spec says its
       WindowProxy (this object), and `closed`/`length` are IDL ACCESSORS, so reading them reached a getter
       from a C activation — which has no flow base under it and is the one thing this engine refuses. That
       delegation only ever ran for a same-origin child, which is why it survived until one existed. */
    switch (magic) {
    case WP_WINDOW: case WP_SELF: case WP_FRAMES: case WP_GLOBALTHIS:
        /* THE NAVIGABLE, MAPPED — not the raw receiver. §7.2.3.7 makes `w.window` the WindowProxy, and for the
           ASKING realm's own navigable the object a page holds as `window` is the global (win_or_proxy). A raw
           `this_val` was indistinguishable from both while a receiver was always a proxy; §3.7.6's missing one
           resolves to this realm's navigable, and handing that back unmapped would make `windowGet() === window`
           false for the one realm that can ask it. */
        return win_or_proxy(ctx, JS_DupValue(ctx, nav));
    case WP_PARENT:
        /* HTML §7.2.2.4 "Accessing related windows"' parent getter steps: "Let navigable be this's navigable.
           If navigable is null, then return null. If navigable's parent is not null, then set navigable to
           navigable's parent. Return navigable's active WindowProxy." STEP 2 IS THE ONE THAT WAS MISSING —
           see the paragraph on WP_TOP below, which is the same sentence and the same omission. */
        if (window_proxy_navigable_null(ctx, nav)) return JS_NULL;
        return win_or_proxy(ctx, JS_IsUndefined(p->parent) ? JS_DupValue(ctx, nav)
                                                           : JS_DupValue(ctx, p->parent));
    case WP_TOP:
        /* §7.2.2.4's top getter steps: "If this's navigable is null, then return null. Return this's
         * navigable's top-level traversable's active WindowProxy." BOTH STEPS, and the first is not a corner
         * case the section mentions in passing — §7.2.2.4 closes with a worked example whose three asserts are
         * `iframeWindow.top === null`, `iframeWindow.parent === null` and `iframeWindow.frameElement === null`
         * after the container element is removed, and the corpus has that example twice over
         * (window-top-null.html, window-parent-null.html).
         *
         * "THIS'S NAVIGABLE IS NULL" IS NOT "THIS IS CLOSED", AND IT IS NOT "THIS'S BROWSING CONTEXT IS NULL"
         * EITHER — §7.2.2.4 spells three of its four getters' step 1 three different ways on purpose, and this
         * record keeps the three facts apart for exactly that reason (see window_proxy.h). The navigable's
         * spelling is window_proxy_navigable_null above, which is a WALK and goes true SYNCHRONOUSLY at
         * §7.3.1.6's sever; `closing` is not it (a traversable that is closing still has its documents), and
         * neither is §7.1.3.2's group swap (`bc_discarded`), which discards the BROWSING CONTEXT — the question
         * the `opener` getter below asks and the one `closed` reports — while the Document stays active in the
         * navigable this record names, which is the whole reason a read through the opener's handle still
         * answers about THAT document.
         *
         * IT IS ASKED BEFORE THE WALK RATHER THAN INSIDE IT. window_proxy_top_navigable climbs `parent` links,
         * which §7.3.1.6 does not sever, so a detached navigable walks to a live traversable and answers with a
         * window that is no longer above it — a real object in place of the null, which is the one shape that
         * cannot be told from an answer. */
        if (window_proxy_navigable_null(ctx, nav)) return JS_NULL;
        return win_or_proxy(ctx, window_proxy_top_navigable(ctx, nav));
    case WP_OPENER:
        /* §7.2.2.4's opener getter steps: "Let current be this's browsing context. If current is null, then
           return null. If current's opener browsing context is null, then return null. Return current's opener
           browsing context's WindowProxy object." STEPS 1-2 ARE THE BROWSING CONTEXT and not the navigable —
           the one getter of the four that asks the other question, and window_proxy_browsing_context_null is
           where that one lives. `embedded-opener-remove-frame.html` is the corpus's own statement of the
           difference in TIME: it reads a removed FRAME's `opener` on the next line and a closed POPUP's behind
           a `step_timeout`, because §7.3.1.6's sever is synchronous and §7.2.2.1's close is a task. */
        if (window_proxy_browsing_context_null(ctx, nav)) return JS_NULL;
        return win_or_proxy(ctx, JS_DupValue(ctx, p->opener));
    case WP_NAME:
        return window_proxy_name_value(ctx, nav);
    case WP_DOCUMENT:
        /* §7.2.2's `document`, and reaching it means the read was PERMITTED — which for a same-origin-only
           member means the origins match, which in an origin-keyed agent means this agent holds the realm. So
           the answer is that realm's own Document object, handed back in this turn: two same-origin documents
           are one heap and the corpus appends nodes across exactly this edge. A destroyed navigable has no
           active document. */
        if (wp_closed(p)) return JS_NULL;
        return JS_DupValue(ctx, document_object(proxy_realm(ctx, nav, p)));
    case WP_LOCATION:
        /* §7.2.4: a navigable has ONE Location, and it is the ACTIVE DOCUMENT's — so it is read off that
           document's realm, exactly as `document` is. A destroyed navigable has no active document and so no
           Location; the spec files read `closed` before touching one, and answering with the address it used
           to have would be a document that no longer exists.
           THE CROSS-ORIGIN HALF IS A DIFFERENT OBJECT, AND IT IS A COMPONENT RATHER THAN A BRANCH. §7.2.1.3.1
           CrossOriginProperties ( O ) lists `location` precisely so a cross-origin document can be navigated
           through it, and what §7.2.4.5 through §7.2.4.10 hand across is a Location filtered to `href`'s setter
           and `replace` over a Document ANOTHER INSTANCE holds — core/frame/remote_location.h. Handing back
           THIS document's Location instead would be a cross-origin read that silently succeeded, which is the
           one failure §7.2.1 Security infrastructure for Window, WindowProxy, and Location objects exists to
           prevent, so the two answers are told apart HERE and neither arm can reach the other's object. */
        if (wp_closed(p)) return JS_NULL;
        if (window_proxy_is_remote(nav)) {
            /* §7.2.1.3.3 IsPlatformObjectSameOrigin AND "this agent holds the active document" ARE ONE
               CONDITION, which is what makes this test the standard's rather than an implementation detail:
               SECURITY.md keys an instance on `(browsing context group, origin)`, so a Document this agent
               hosts is same origin with every realm that could be asking. The one shape that breaks the
               pairing is a SAME-ORIGIN document in ANOTHER browsing context group — a separate instance by
               that same key — for which §7.2.4's arms take the ORDINARY branch over a Document this heap does
               not contain. Answering that with the filtered object would refuse reads the origins PERMIT, so
               it is asserted rather than silently taken. */
            DCHECK(!proxy_same_origin(p),
                   "§7.2.2's `location` was read on a navigable whose active document is SAME ORIGIN and in "
                   "ANOTHER INSTANCE — §7.2.1.3.3 answers TRUE for it, so §7.2.4 wants that document's "
                   "ORDINARY Location, every member of which reads state this heap does not hold. That is a "
                   "cross-instance read of §7.2.4's members, suspended and answered by the peer exactly as "
                   "proxy_get_step answers `length`; the filtered object below is §7.2.4.5 [[GetOwnProperty]]'s "
                   "CROSS-origin arm and would refuse reads the origins permit");
            return remote_location_of_document(ctx, p->doc);
        }
        /* READ OFF THE REALM, not off its global. `location` is an IDL accessor now — §7.2.2 declares it
           `[LegacyUnforgeable] readonly attribute Location`, so it is a non-configurable accessor on the
           global — and a JS_GetPropertyStr that reaches a getter aborts, because a C activation has no flow
           base under it. location.h's location_object is the same answer without the property read, exactly as
           document_object is above. */
        return location_object(proxy_realm(ctx, nav, p));
    default:
        DFAIL("a WindowProxy member with no navigable-own answer reached this switch — WP_LENGTH, WP_CLOSED "
              "and anything added beside them are answered by the instance holding the navigable's document "
              "and are declared on the step machine below");
        return JS_UNDEFINED;
    }
}

/* §7.2.2.4's `parent`, `top` and `opener` ARE THE NAVIGABLE'S, and these are how a Window answers them. A Window
   and its WindowProxy are two spellings of one navigable, so the two must be one answer from one record — the
   same unification `closed` already has. window.c answered all three with FIXED values behind two comments
   saying no embedder could exist and nothing had opened this document; both were true exactly while one
   instance was one document. */
JSValue window_proxy_parent(JSContext *ctx, JSValueConst proxy)
{
    return proxy_member_get(ctx, proxy, WP_PARENT);
}

/* THE NAVIGABLE THIS ONE IS NESTED IN, FOR AN ENGINE WALK — the same distinction window_proxy_top_navigable
   draws, and for the same reason. §7.2.2.4's `parent` answers THIS proxy when there is no parent, because
   `window.parent === window` is an identity a top-level page rests on; a walk UP the navigable tree wants
   "nothing above this" and would otherwise report a top-level navigable as its own container and loop, or
   deliver §7.5.10's child-destroyed report back to the navigable that sent it. JS_UNDEFINED at the top, and
   the creator's Window for an auxiliary navigable — neither is a WindowProxy, which is the caller's test. */
JSValue window_proxy_parent_navigable(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);
    DCHECK(p != NULL, "the containing navigable of something that is not a WindowProxy was asked for");
    return JS_DupValue(ctx, p->parent);
}

/* §7.3.1.3's create-a-new-child-navigable step "Set element's content navigable to navigable", from the
   navigable's side. It is a WRITE OF A CREATION STEP and not a mutable property: §7.3.1.6 severs the relation
   by clearing the ELEMENT's slot, never by rewriting this one, so a second write here would be a navigable
   changing which element presents it — which no algorithm in §7.3 does. Asserted rather than assumed. */
void window_proxy_set_container(JSContext *ctx, JSValueConst proxy, JSValueConst element)
{
    ProxyData *p = proxy_of(proxy);

    DCHECK(p != NULL, "§7.3.1.3's container was set on something that is not a WindowProxy");
    DCHECK(JS_IsObject(element),
           "§7.3.1.3's create-a-new-child-navigable was given something that is not an element wrapper as its "
           "navigable container — the algorithm takes an ELEMENT and every step of it reads that element");
    DCHECK(JS_IsNull(p->container),
           "a navigable was given a SECOND §7.3.1.3 container — the relation is created once, and §7.3.1.6's "
           "destroy-a-child-navigable severs it by clearing the ELEMENT's content navigable rather than by "
           "moving this one, so a navigable never changes which element presents it");
    wp_set(ctx, p, &p->container, JS_DupValue(ctx, element));
}

/* THE SAME LINK FOR THE NAVIGABLE WHOSE CONTAINER ELEMENT IS IN ANOTHER WASM INSTANCE — see the field.
   IT IS STATED AFTER THE MINT FOR §7.3.1.3'S OWN REASON, not for a host's convenience: the section creates a
   navigable and then links it ("Let navigable be a new navigable … Set element's content navigable to
   navigable"), which is exactly the order window_proxy_set_container above is called in. A host that roots an
   instance is standing at the same point, one boundary away.
   WRITTEN ONCE, like the element it stands for: a navigable never changes which element presents it. */
void window_proxy_set_remote_container(JSValueConst proxy, const char *serialized_policy)
{
    ProxyData *p = proxy_of(proxy);

    DCHECK(p != NULL, "§7.3.1.3's remote container was stated for something that is not a WindowProxy");
    DCHECK(serialized_policy != NULL && *serialized_policy != '\0',
           "§7.3.1.3's container of a navigable in another instance was stated as NOTHING — §9.5's answer or "
           "that grammar's \"there is no container\" are the two things a provisioning record can say, and an "
           "empty field is neither");
    DCHECK(JS_IsNull(p->container),
           "a navigable that is presented by an element IN THIS HEAP was also given a container in another "
           "instance — the element itself answers §9.5's every argument, so the second statement is about a "
           "different navigable and one of the two is wrong about which");
    DCHECK(p->remote_container == NULL,
           "a navigable was given a SECOND cross-instance §7.3.1.3 container — the relation is created once, "
           "and a navigable never changes which element presents it");
    p->remote_container = proxy_strdup(serialized_policy);
}

const char *window_proxy_remote_container(JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);

    DCHECK(p != NULL, "§7.3.1.3's remote container of something that is not a WindowProxy was asked for");
    return p->remote_container;
}

/* HTML §3.1.3 "Ancestor origins"' LIST FOR A NAVIGABLE WHOSE ANCESTORS ARE IN ANOTHER WASM INSTANCE — see the
   field. It is stated at the same moment and by the same zone as the remote container beside it, because both
   are answers the creating instance computed out of a tree only it holds; §7.3.1.3's own order puts both after
   the mint, which is where a host rooting an instance stands.
   WRITTEN ONCE, like the two links it is composed from: §3.1.3's list is a SNAPSHOT of the tree the Document
   was created in, so a second statement would be about a second creation. */
void window_proxy_set_remote_ancestor_origins(JSValueConst proxy, const char *serialized_list)
{
    ProxyData *p = proxy_of(proxy);

    DCHECK(p != NULL, "§3.1.3's ancestor origins were stated for something that is not a WindowProxy");
    DCHECK(serialized_list != NULL && *serialized_list != '\0',
           "§3.1.3's internal ancestor origin objects list for a navigable in another instance was stated as "
           "NOTHING — the composed list and that grammar's word for the EMPTY one are the two things a "
           "provisioning record can say, and an empty field is neither");
    DCHECK(p->remote_ancestor_origins == NULL,
           "a navigable was given a SECOND cross-instance §3.1.3 ancestor origins list — the list is a "
           "snapshot taken when its Document was created, so a second statement describes a second creation");
    p->remote_ancestor_origins = proxy_strdup(serialized_list);
}

const char *window_proxy_remote_ancestor_origins(JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);

    DCHECK(p != NULL, "§3.1.3's remote ancestor origins of something that is not a WindowProxy were asked for");
    return p->remote_ancestor_origins;
}

/* §7.3.1.3's CONTAINER OF A NAVIGABLE, as the standard defines it: "the navigable container whose content
   navigable is navigable, or null if there is no such element". The recorded element is the CANDIDATE and the
   forward slot is the DEFINITION, so the answer is asked of the element every time.
   THAT IS NOT BELT-AND-BRACES, IT IS THE PER-FLOW ANSWER. The forward slot is an ordinary own property on the
   wrapper, so §7.3.1.6's clear rides the running flow's heap delta: an arm that removed the `<iframe>` has no
   container from that point on and its sibling, which never removed it, still has one — out of ONE recorded
   pointer, with nothing here to capture and no second write to keep in step. */
JSValue window_proxy_container(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);
    JSValue held;

    DCHECK(p != NULL, "§7.3.1.3's container of something that is not a WindowProxy was asked for");
    if (JS_IsNull(p->container)) return JS_NULL;
    held = iframe_navigable(ctx, p->container);
    if (JS_VALUE_GET_PTR(held) != JS_VALUE_GET_PTR(proxy)) {
        /* The element's content navigable is null, or is some other navigable: §7.3.1.3 answers null, and this
           is the ONLY way that happens — §7.3.1.6's destroy-a-child-navigable, in this flow. */
        JS_FreeValue(ctx, held);
        return JS_NULL;
    }
    JS_FreeValue(ctx, held);
    return JS_DupValue(ctx, p->container);
}

JSValue window_proxy_top_of(JSContext *ctx, JSValueConst proxy)
{
    return proxy_member_get(ctx, proxy, WP_TOP);
}

JSValue window_proxy_opener(JSContext *ctx, JSValueConst proxy)
{
    return proxy_member_get(ctx, proxy, WP_OPENER);
}

/* §7.2.2.4's `opener` AS THE NAVIGABLE, which is what an engine walk wants and what the member must not give it
   — win_or_proxy maps this document's own navigable onto the GLOBAL, because `w.opener === window` is the
   identity an opened page rests on, and a Window is not a WindowProxy. Same distinction, same reason, as
   window_proxy_parent_navigable above. */
JSValue window_proxy_opener_navigable(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);
    DCHECK(p != NULL, "the opener navigable of something that is not a WindowProxy was asked for");
    return JS_DupValue(ctx, p->opener);
}

/* THE NAVIGABLE A VALUE NAMES — win_or_proxy's mapping read backwards, and in one place for the reason that
   one is: a Window and its WindowProxy are two spellings of ONE navigable, and a caller that has to treat
   them as one (an encoder handing a navigable to another agent) must not be the place that decides it. */
JSValueConst window_proxy_navigable_of(JSContext *ctx, JSValueConst v)
{
    JSValue g;
    bool is_global;

    if (window_proxy_is(v)) return v;
    /* AND THE SAME NON-OBJECT RULE win_or_proxy STATES, read in this direction: §3.7.6's receiver may be any
       value a page can write — `openerGet.call(42)` is a TypeError, not a question about a global — and a
       primitive has no pointer to compare against one. */
    if (!JS_IsObject(v)) return JS_UNDEFINED;
    g = JS_GetGlobalObject(ctx);
    is_global = JS_VALUE_GET_PTR(g) == JS_VALUE_GET_PTR(v);
    JS_FreeValue(ctx, g);
    return is_global ? document_window_proxy(ctx) : JS_UNDEFINED;
}

/* §3.7.6's FIRST CLAUSE, WRITTEN ONCE — "if it is not null or undefined". The sentence has TWO consumers that
   want DIFFERENT answers out of it (the navigable the member is about, and the ES object the member is invoked
   on), and writing the test twice is how they come to disagree: one of them would keep answering `undefined`
   for a missing receiver while the other resolved it. Side-effect-free. */
static bool wp_this_absent(JSValueConst this_val)
{
    return JS_IsUndefined(this_val) || JS_IsNull(this_val);
}

/* §3.7.6's `jsValue` ITSELF — the PLATFORM OBJECT the member was invoked on, which is what §7.2.2.4's opener
   setter means by "perform ? DefinePropertyOrThrow(this, "opener", …)" and what [Replaceable]'s
   CreateDataPropertyOrThrow writes to. It is NOT the navigable: for a missing receiver the navigable is
   `document_window_proxy(ctx)` and the object is the realm's GLOBAL, and defining the page's value on the
   proxy instead would put it where §7.2.3.5 answers out of the surface and nothing the page can read.
   OWNED. */
JSValue window_proxy_this_object(JSContext *ctx, JSValueConst this_val)
{
    if (wp_this_absent(this_val)) return JS_GetGlobalObject(ctx);
    return JS_DupValue(ctx, this_val);
}

/* §3.7.6's SECOND SENTENCE — "If jsValue does not implement target, then ... throw a TypeError" — asked for
 * target = Window, and asked WITHOUT looking up a navigable. That distinction is the whole reason this is its
 * own statement rather than a use of window_proxy_this_navigable: a member that needs the navigable and a
 * member that needs only the OBJECT are two different questions, and the four-arm function answers the first
 * by DCHECKing on a receiver the second can serve perfectly well. [Replaceable]'s CreateDataPropertyOrThrow
 * lands the page's value on `jsValue` itself; it never asks which window that is, so a foreign realm's Window
 * is a receiver it can answer and not a capability gap.
 *
 * THE TWO OBJECTS THAT IMPLEMENT Window HERE, and there is no third. The realm's global carries the Window
 * class (window_install hands it one through JS_SetGlobalClass), so window_is answers for this realm's global
 * and for every other realm's alike — which is exactly the spec's test, since implementing an interface is a
 * property of the OBJECT and not of who is asking. A WindowProxy is the second, because §7.2.3.5 step 3
 * performs the same-origin case's OrdinaryGetOwnProperty on W and the accessor it hands back is then invoked
 * with the PROXY as its receiver: rejecting one would make `frames[0].length` a TypeError.
 * Side-effect-free — it reads two class ids and allocates nothing. */
bool window_proxy_implements_window(JSValueConst js_value)
{
    return window_is(js_value) || window_proxy_is(js_value);
}

/* DOES THIS RECEIVER NAME THE REALM THE MEMBER WAS INSTALLED IN? Not a Web IDL step — a question §3.7.6 makes
 * askable and this engine has to ask, because a member whose ANSWER is a value the realm already holds cannot
 * tell a receiver apart from its realm on its own.
 *
 * §3.7.6 runs the getter steps "with idlObject as this", so an attribute answering out of realm-held data is
 * correct exactly while idlObject IS this realm's Window. It normally is, and for a reason worth stating: each
 * realm installs its OWN getter carrying its OWN value, and js_call_c_function sets ctx to the member's realm,
 * so `frames[0].document` reaches the CHILD's getter with the child's proxy and both halves agree. What does
 * not agree is a receiver taken from one navigable and applied to another realm's accessor by hand —
 * `Object.getOwnPropertyDescriptor(window, "screen").get.call(frames[0])` — where the held value answers the
 * READING realm's question about a window that is not it. That is a wrong answer with a plausible shape, which
 * is the one thing worth a crash rather than a return.
 * Side-effect-free: window_proxy_navigable_of and document_window_proxy both borrow. */
bool window_proxy_receiver_is_own_realm(JSContext *ctx, JSValueConst js_value)
{
    JSValueConst nav = window_proxy_navigable_of(ctx, js_value);

    return !JS_IsUndefined(nav) &&
           JS_VALUE_GET_PTR(nav) == JS_VALUE_GET_PTR(document_window_proxy(ctx));
}

/* WEB IDL §3.7.6 "Attributes"' `jsValue`, RESOLVED TO THE NAVIGABLE THE MEMBER IS ABOUT — and it is a
 * MECHANISM rather than a convenience, because the thing it replaces is a whole class of wrong answer.
 *
 * §3.7.6's create-an-attribute-getter says it in one sentence: "Let jsValue be the this value, if it is not
 * null or undefined, or realm's global object otherwise", and then "Let R be the result of running the getter
 * steps of attribute with idlObject as this". So a member of a [Global] interface is answered about the
 * RECEIVER, and the realm's own global is what a MISSING receiver falls back to — one arm of the rule, not
 * the whole of it. §3.7.7 Operations carries the identical step for a method.
 *
 * WINDOW.C ANSWERED EVERY RECEIVER OUT OF ITS REALM, which is the null/undefined arm applied to all of them:
 * `document_window_proxy(ctx)` with `(void)this_val` beside it. That is CLAUDE.md's ONE FACT ANSWERED FROM
 * ONE PLACE FOR MANY AGENTS with the agents being RECEIVERS instead of realms — a member installed once
 * answering for whichever navigable its realm has, whoever asks. A page reads the difference directly:
 * §7.2.3.5 step 3 performs a same-origin WindowProxy's [[GetOwnProperty]] ON W, so the getter it hands back
 * is the OTHER document's and is invoked with the proxy as its receiver, and
 * `Object.getOwnPropertyDescriptor(self, "opener").get.call(popup)` — opener-string.window.js's last
 * assertion — is that same call written out by hand. Both answered about the READING document's navigable.
 *
 * FOUR ARMS AND EACH IS A DIFFERENT FACT:
 *   a WindowProxy IS the navigable (win_or_proxy's mapping read backwards, window_proxy_navigable_of);
 *   this realm's GLOBAL is this realm's navigable, which is the same mapping and the same one place;
 *   null or undefined is §3.7.6's fallback to the DEFINING realm's global — `ctx` here, because
 *     js_call_c_function sets it to the member's own realm, which is exactly the realm §3.7.6 names;
 *   anything else does not implement Window and is §3.7.6's TypeError, thrown rather than answered, because
 *     a page distinguishes a throw from an answer about the wrong window.
 * Returns the navigable BORROWED, or JS_UNINITIALIZED with a TypeError pending. */
JSValueConst window_proxy_this_navigable(JSContext *ctx, JSValueConst this_val)
{
    JSValueConst nav;

    if (wp_this_absent(this_val))
        return document_window_proxy(ctx);
    nav = window_proxy_navigable_of(ctx, this_val);
    if (!JS_IsUndefined(nav)) return nav;
    /* IT IS A Window, AND NOT ONE THIS MAPPING CAN NAME. §3.7.6's TypeError is for a receiver that does not
       IMPLEMENT the interface, and this one does — so throwing here would be a wrong answer with a plausible
       shape, which is the one thing the arm below exists to avoid. The missing piece is an edge from a Window
       object to its navigable that does not go through a realm: `window_proxy_navigable_of` reads THIS realm's
       global out of `ctx`, and a foreign realm's global is the same question asked of a realm it has no handle
       to. Every other spelling of another navigable in this engine already IS the proxy (`contentWindow`,
       `defaultView`, `parent`/`top`/`opener`, and §7.2.3's own `window`/`self`/`frames`/`globalThis`), which
       is why nothing reaches this today and why the day something does it must say so rather than throw. */
    DCHECK(!window_is(this_val),
           "a Web IDL attribute of the Window interface was invoked with ANOTHER realm's Window object as its "
           "receiver. §3.7.6's TypeError is for a receiver that does not implement the interface and this one "
           "does, so the answer is that navigable rather than a throw — build the Window -> navigable edge "
           "that does not go through a realm (document_window_proxy reads THIS realm's global out of its ctx, "
           "which cannot name a foreign one), and route window_proxy_navigable_of onto it");
    JS_ThrowTypeError(ctx, "a Window member was read on an object that is not a Window");
    return JS_UNINITIALIZED;
}

/* §7.2.2's `name` SETTER. THE ORIGIN CHECK IS NO LONGER HERE, and its removal is the point of the mechanism
   above: `name` is not on §7.2.1.3.1's list, so §7.2.1.1 Integration with IDL refuses this setter for a
   cross-origin receiver BEFORE its body runs, at the one place every declared member converges on. The line
   that stood here asked the SAME question off a SECOND table — proxy_read_permitted reads PROXY_CROSS_ORIGIN,
   which is indexed by this component's member magic, while §7.2.1.1 reads the standard's own CROSS_ORIGIN — so
   keeping it would be two tables deciding one member's one question, and the day they disagreed the answer
   would depend on which spelling a page used. The rename is window_proxy_name_assign's, because a Window and
   its proxy write the one navigable's one name. */
static JSValue proxy_name_set(JSContext *ctx, JSValueConst this_val, JSValueConst v, int magic)
{
    JSValueConst nav = window_proxy_this_navigable(ctx, this_val);

    (void)magic;
    if (JS_IsUninitialized(nav)) return JS_EXCEPTION;   /* §3.7.6's TypeError, already thrown */
    /* The record lookup and its capture went with the check: window_proxy_name_assign does its own proxy_of,
       which is what puts this write in the running flow's delta, and a second one here read a record only the
       deleted branch had anything to ask. */
    return window_proxy_name_assign(ctx, nav, v);
}

/* §7.2.2.4's `opener` SETTER, THROUGH THE PROXY — `attribute any opener`, and it had none here at all, so
   `popup.opener = "blah"` was a silent no-op through a WindowProxy while the same write on the Window spelling
   of the same navigable disowned or replaced. It is the SAME two branches window.c's spelling has, because it
   is the same member on the same navigable:
     null  -> DISOWN: the navigable's opener link is severed and NO own property is defined, so everything that
              reads the navigable sees the cut rather than a `null` that only the page can see;
     other -> §7.2.2.4's step 2, DefinePropertyOrThrow(this, "opener", {value, writable, enumerable,
              configurable}) — Web IDL's [Replaceable] performs CreateDataPropertyOrThrow, which builds the same
              descriptor, and this member reaches it through that one implementation. It is what makes the write
              OBSERVABLE:
              the data property lands on THIS object and quickjs finds a shape property before it reaches the
              exotic hook, so §7.2.3.5 answers the page's value from then on.
   THE NAVIGABLE IS `this_val`'s, not the reading realm's — the one difference from window.c's spelling, and
   the whole reason a second body exists rather than a shared one: that one answers for `document_window_proxy`
   because the Window it is installed on IS its realm's navigable, and this one is handed the target. */
static JSValue proxy_opener_set(JSContext *ctx, JSValueConst this_val, JSValueConst v, int magic)
{
    JSValueConst nav = window_proxy_this_navigable(ctx, this_val);
    JSValue js;
    int r;

    (void)magic;
    if (JS_IsUninitialized(nav)) return JS_EXCEPTION;   /* §3.7.6's TypeError, already thrown */
    /* §7.2.1.3.1 lists `opener` with [[NeedsSetter]] FALSE, so a cross-origin write of it is not a member of
       the cross-origin surface at all — §7.2.1.3.6 CrossOriginSet ( O, P, V, Receiver ) calls the setter only
       when the descriptor §7.2.1.3.4 built HAS one, and its last step throws a SecurityError.
       THAT REFUSAL IS NOT WRITTEN HERE ANY MORE. It was `!proxy_same_origin(p)` — §7.2.1.1 Integration with
       IDL's step 3 with this member's half of step 2 folded into the choice of function, which is the shape
       that goes wrong silently the next time a setter is added and nobody remembers. §7.2.1.1 now reads the
       [[NeedsSetter]] FALSE off the list itself, for every setter at once, before any body runs. */
    if (JS_IsNull(v)) {
        window_proxy_disown_opener(ctx, nav);
        return JS_UNDEFINED;
    }
    /* §7.2.2.4's setter step 2 names `this` — §3.7.6's `jsValue`, the OBJECT — and the two answers part company
       for a missing receiver: the navigable above is this realm's proxy, the define below lands on its GLOBAL.
       Aiming the define at the navigable instead would write the page's value onto an object §7.2.3.5 answers
       out of §7.2.3's own surface, so the very next read would return the opener again. */
    js = window_proxy_this_object(ctx, this_val);
    r = idl_replace_with_value(ctx, js, "opener", v);
    JS_FreeValue(ctx, js);
    return r < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

/* §7.2.2.1's `close()`. THE WHOLE METHOD IS document_lifecycle.c's, and this member is one of its two
   spellings: `w.close()` from an opener and `window.close()` inside the popup are one method on one navigable,
   and each carried a body of its own that stopped at step 3 — is closing went true and nothing else happened,
   so `closed` reported a closed window over a document that was still running its timers, still holding its
   subframes, and that had never fired a beforeunload or an unload listener. Steps 4-6 are the rest of it.
   The navigable is this agent's whether or not its active document is, so there is nothing to delegate and
   nothing to ask a peer: `this_val` IS thisTraversable, and `ctx` is the INCUMBENT realm step 6 asks about. */
static JSValue proxy_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    /* §3.7.7 Operations carries §3.7.6's receiver sentence verbatim, so a detached `close` closes the realm's
       own navigable rather than returning quietly — which is what `window.close.call(undefined)` means and what
       the silent no-op here made unobservable. */
    JSValueConst nav = window_proxy_this_navigable(ctx, this_val);

    (void)argc; (void)argv; (void)magic;
    if (JS_IsUninitialized(nav)) return JS_EXCEPTION;   /* §3.7.7's TypeError, already thrown */
    document_lifecycle_window_close(ctx, nav);
    return JS_UNDEFINED;
}

/* IS THIS NAVIGABLE A TOP-LEVEL TRAVERSABLE — one fact, asked from one place. §7.2.2.1's close() returns early
   for anything else and §7.3's is-closing is only ever set on one, and both spellings of close() (the Window
   member and the proxy's) have to make the SAME test or one of them closes something the other would not. */
bool window_proxy_is_top_level(JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);
    DCHECK(p != NULL, "something that is not a WindowProxy was asked whether it is a top-level traversable");
    return JS_IsUndefined(p->parent);
}

/* §7.2.2.1's IS CLOSING, for the Window spelling of `close()` — window.c's member and the proxy's are the one
   method on the one navigable, so they write the one flag through here. §7.2.2.1 RETURNS EARLY for a navigable
   that is not a top-level traversable, and that early return is the method's, not this setter's: a caller that
   has already made that test states it, and one that has not would otherwise silently mark a frame closing. */
void window_proxy_set_closing(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);
    (void)ctx;
    DCHECK(p != NULL, "something that is not a WindowProxy was closed");
    DCHECK(JS_IsUndefined(p->parent),
           "§7.2.2.1's is closing was set on a navigable that is not a TOP-LEVEL traversable — the flag is only "
           "ever true for one, and close() returns early for anything else");
    p->closing = 1;
}

typedef struct { uint32_t req; } ProxyGetState;

static void proxy_get_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

/* §7.2.2 The Window object DECLARES WHAT AN ANSWER MAY BE — `[Replaceable] readonly attribute unsigned long
   length` and `readonly attribute boolean closed`. A peer resolves the read by RUNNING the member's getter, so
   a normal completion carries one of those two types and nothing else, and this is the only place anything can
   say so: remote_object.h's grammar exists because `otherW.length === 0` distinguishes the number 0 from the
   string "0", and a relay that encoded a number as `s…` or lost it to `u` hands the page a plausible value with
   nothing whatever to say what happened. The two LOCAL arms above are typed by construction (JS_NewInt32,
   JS_NewBool); this one is whatever arrived.
   A PREDICATE RATHER THAN AN EXPRESSION AT THE CALL SITE, because check.h compiles a DCHECK's condition to
   `sizeof` in release and the condition must still TYPE-CHECK there. Side-effect-free: it reads the value's tag
   and nothing else.
   A MEMBER THIS FUNCTION DOES NOT NAME ANSWERS FALSE, deliberately. A third member routed onto the remote path
   is one whose IDL type nothing has stated, and an unstated type is an unchecked one. */
static int proxy_answer_matches_idl(JSValueConst v, int magic)
{
    if (magic == WP_CLOSED) return JS_IsBool(v);
    if (magic != WP_LENGTH) return 0;
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) return JS_VALUE_GET_INT(v) >= 0;
    if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(v))) {
        double d = JS_VALUE_GET_FLOAT64(v);
        return d >= 0 && d <= 4294967295.0 && d == (double)(uint32_t)d;
    }
    return 0;
}

/* WHERE THIS MACHINE RESTS. Both members it drives are read off state the OTHER instance keeps — §7.2.3's
   `length` off that navigable's active document, §7.2.2.1's `closed` off the top-level traversable whichever
   agent ran close() wrote — so the read is one step of the standard performed in another instance, and the
   wait for that peer is a sub-sequence inside it (`req` is its cursor), not a step of its own. One stage: a
   flow parked here is parked at the read it made, whichever member it asked for. */
#define PROXY_GET_STAGES(X) \
    X(PROXY_GET_ASK = IDL_STEP_FIRST, \
      "HTML §7.2.3 the WindowProxy exotic object (the cross-instance member's value, resolved by the instance " \
      "that holds the navigable — the flow suspends on the line that made the read and resumes with the answer)")
enum { PROXY_GET_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const PROXY_GET_STEPS[] = { PROXY_GET_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int proxy_get_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                          JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    ProxyGetState *s = st;
    JSValueConst answer, nav;
    int magic = idl_step_magic(hdr);
    ProxyData *p;

    (void)argc; (void)argv; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(magic >= 0 && magic < WP_MEMBER_N,
           "a WindowProxy member was declared with a magic this component does not name");

    /* §3.7.6's RECEIVER, RESOLVED BY THE ONE PLACE THAT RESOLVES IT — the same sentence proxy_member_get reads,
       and it has to be read here too: `closed` and `length` are on §7.2.1.3.1's cross-origin list, so a page
       reaches both of them through a descriptor's `.get` with no receiver exactly as it reaches `opener`.
       A `JS_UNDEFINED` here was `otherW.closed` answering neither true nor false. */
    nav = window_proxy_this_navigable(ctx, hdr->this_val);
    if (JS_IsUninitialized(nav)) return JS_STEP_ABRUPT;   /* §3.7.6's TypeError, already thrown */
    /* JS_GetOpaque AND NOT proxy_of, which is this machine's own existing choice and is kept: neither member
       below WRITES the record, and a capture here would put an entry in the running flow's delta for storage
       the read never touches. proxy_member_get captures because `name`'s SETTER shares its record. */
    p = JS_GetOpaque(nav, g_proxy_class);
    DCHECK(p != NULL, "§3.7.6's receiver resolved to something that is not a WindowProxy");
    if (!proxy_read_permitted(p, magic)) {
        JS_ThrowDOMException(ctx, "SecurityError",
                             "the origins do not permit reading this member of that Window");
        return JS_STEP_ABRUPT;
    }

    /* THE HOSTED CASE, ANSWERED IN THIS TURN AND OUT OF THE TARGET'S OWN RECORD. §7.2.3's `length` is the
       child-navigable count of the ACTIVE DOCUMENT, so it is a walk of THAT document's tree — the reading
       realm would count this document's frames and call them the other's. §7.2.2.1's `closed` is the top-level
       traversable's, and the agent that holds the active document is the agent whose `close()` writes it.
       A NAVIGABLE WHOSE REALM HAS NOT BEEN MATERIALIZED COUNTS ZERO, and that is the computed answer rather
       than a stand-in for one: its active document is the empty about:blank Document §7.4 created, an element
       can only get into it by script, and script cannot run in a realm that does not exist. Building a whole
       platform to count the iframes in a document that provably has none is what made this read cost a realm
       per flow — 4000 flows in and the frontier hit the RAM floor at ~57% of the depth it reaches without it. */
    if (world_doc_hosted(p->doc)) {
        switch (magic) {
        case WP_LENGTH:
            *presult = JS_NewInt32(ctx, p->realm ? iframe_child_navigable_count(p->realm) : 0);
            break;
        case WP_CLOSED:
            /* §7.2.2.1's whole OR, which is window_proxy_closed's — never the recorded flags, for the reason
               written at wp_bc_null: a frame removed from the tree is closed on that line. */
            *presult = JS_NewBool(ctx, window_proxy_closed(ctx, nav));
            break;
        default:
            DFAIL("a navigable-own member reached the step machine — it is answered by proxy_member_get, in "
                  "this turn, with no host round trip");
            *presult = JS_UNDEFINED;
            break;
        }
        return JS_STEP_DONE;
    }
    /* THIS AGENT'S RECORD ALREADY SAYS CLOSED, AND `closed` NEVER GOES BACK. §7.2.2.1's close() step 3 returns
       early once is closing is true and a null browsing context is terminal, so a local TRUE is the standard's
       answer and asking a peer could only confirm it — which is also what keeps this read answerable after the
       host has torn that instance down. A local FALSE says nothing about the peer, which is the whole reason
       the branch below exists.
       AND FOR `length` THE SAME STATE IS A DIFFERENT ANSWER: a destroyed navigable has no active document to
       count, so §7.2.3 answers 0 here rather than parking a flow on an instance that is gone.
       THE WHOLE OR, INCLUDING §7.3.1.6's SEVER — a cross-origin child removed from THIS document's tree is
       detached here on that line, and parking a flow on the peer to be told about a navigable this agent
       created and has just detached is a round trip whose answer this side already holds. */
    if (window_proxy_closed(ctx, nav)) {
        *presult = magic == WP_CLOSED ? JS_TRUE : JS_NewInt32(ctx, 0);
        return JS_STEP_DONE;
    }
    /* AND A NESTED NAVIGABLE'S `closed` IS THIS AGENT'S EITHER WAY, so it is answered here rather than asked.
       §7.3.1 navigables declares is closing with the sentence "This is only ever set to true for top-level
       traversable navigables" — which window_proxy_set_closing already asserts — so the only half of §7.2.2.1's
       OR that a nested navigable can satisfy is a null browsing context, and that is written by §7.3.1.6
       navigable destruction's destroy a child navigable, run by the agent holding the CONTAINER ELEMENT. This
       one. A peer asked about its own document would answer out of ITS root proxy, which is the record for a
       navigable it believes is top-level — the one answer that is confidently wrong. */
    if (magic == WP_CLOSED && !JS_IsUndefined(p->parent)) { *presult = JS_FALSE; return JS_STEP_DONE; }

    if (s->req == 0) {
        char op[1024];
        Flow *f = flow_running();
        int n;

        DCHECK(f != NULL, "a cross-document read was issued outside a flow — there would be nothing to suspend");
        /* (document, world+ancestry, member), and every document by NAME: a `uint32_t doc` is this instance's
           handle into its own name table and means a different document in the peer's.
           THE ANCESTRY TRAVELS WITH THE WORLD, and it is not optional. A peer materializes its segment for a
           world it has never seen by FORKING THE NEAREST ANCESTOR IT ALREADY HOLDS (world.h) — so a request
           that carried only the world's name would make every peer start from an empty segment, which is the
           truth for a flow that has never written there and a LIE for one that forked after writing. Nearest
           first, because the scan stops at the first hit and a further ancestor silently drops the nearer
           one's writes. */
        n = snprintf(op, sizeof op, "windowproxy.get\t%s\t", world_doc_name(p->doc));
        /* EVERY FIELD'S FIT IS ASSERTED, not just the world vector's. world_serialize crashes on its own
           truncation (a prefix makes the peer fork a more distant ancestor and lose the nearer writes), and the
           two fields AROUND it had no such check — a truncated document name reaches no instance and parks this
           flow forever, and a truncated member makes the peer run a program for a DIFFERENT question and answer
           it as if it were this one. Both are silent, and both are the same sentence as world_serialize's. */
        CHECK(n > 0 && (size_t)n < sizeof op,
              "a cross-document read's target document name did not fit its record — a truncated name reaches "
              "no instance, and the asking flow parks on a question nothing will ever be asked");
        n += world_serialize(f->world, op + n, sizeof op - (size_t)n);
        n += snprintf(op + n, sizeof op - (size_t)n, "\t%s", PROXY_MEMBER[magic]);
        CHECK((size_t)n < sizeof op,
              "a cross-document read's member name did not fit its record — the peer would run a program for a "
              "TRUNCATED member, answering a different question as if it were this one");
        s->req = engine_host_request(ctx, op);
        return JS_STEP_YIELD;   /* park; siblings run until the peer answers */
    }
    if (!engine_host_answered(s->req, &answer))
        return JS_STEP_YIELD;
    /* THE PEER'S COMPLETION, NOT ITS VALUE. §7.2.1's member is an IDL getter the peer RUNS, so it can throw
       — a SecurityError from a member outside the cross-origin list, or the page's own accessor — and the
       throw belongs at the line that read the member. */
    {
        int r = engine_host_take_completion(ctx, s->req, presult);
        s->req = 0;
        /* AND AGAINST §7.2.2's IDL TYPE FOR THE MEMBER THAT WAS ASKED. A throw is exempt: the thrown value is
           an Error OBJECT and crosses as a name, which is the point of layering the completion over the value
           grammar rather than beside it.
           WHAT A HIT MEANS IS NOW ONE THING, AND THE SECOND READING IS RETIRED RATHER THAN FORGOTTEN. This
           assert used to name an ordered pair, the second half of which was that the peer answered by an
           ORDINARY [[Get]] of its own global — which is what `windowproxy.get` used to perform. It performs
           §7.2.1.3.4 CrossOriginGetOwnPropertyHelper's getter now (remote_op.c's OPS table, over the table
           window_proxy_install_window_getters captures), so a page's `window.length = 5` no longer reaches
           this answer at all and there is nothing here for a type test to catch: Web IDL §3.3.11
           [Replaceable]'s shadow is a NUMBER, indistinguishable from a real child-navigable count by any test
           on the value, which is precisely why that half had to be fixed at the peer instead of detected here.
           WHAT IS LEFT IS THE SEAM. A hit means the completion lost its TYPE crossing the boundary — the
           failure core/frame/remote_object.h's leading byte exists to make impossible — and that is a fact
           about the transport rather than about either document, which is the one thing this side can still
           know on its own. */
        DCHECK(r != JS_STEP_DONE || proxy_answer_matches_idl(*presult, magic),
               "a peer answered a cross-instance WindowProxy member with a value §7.2.2 The Window object does "
               "not declare for it — `length` is an unsigned long and `closed` is a boolean, so the page's "
               "`otherW.length === 0` is being decided against something that is not a number. The peer runs "
               "§7.2.1.3.4's getter, whose result IS of the declared type, so the value changed shape between "
               "that call and this line: core/frame/remote_object.c's encoding is what carries the type");
        return r;
    }
}

static const IdlStepDecl PROXY_GET_DECL = { proxy_get_step, sizeof(ProxyGetState), proxy_get_visit, NULL,
                                           "HTML §7.2.3 a cross-instance WindowProxy member's value",
                                           PROXY_GET_STEPS };

/* §7.2.1's MEMBER SURFACE FOR ONE REALM. Declaration order in core/realm.h's list matters here in one
   direction only: window_message.c's `postMessage` goes onto this object, so its entry is declared after this
   one and finds it already built. */
void window_proxy_install_proto(JSContext *ctx)
{
    JSValue proto, prev;
    int i;

    DCHECK(g_wp_rt != NULL, "a realm asked for the WindowProxy prototype before window_proxy_init ran");
    prev = JS_GetClassProto(ctx, g_proxy_class);
    DCHECK(JS_IsNull(prev), "window_proxy_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "the WindowProxy prototype could not be allocated");
    /* §7.2.3 The WindowProxy exotic object: "There is no WindowProxy interface object." The surface below is
       the [[Window]]'s — §7.2.3.7's same-origin [[Get]] is an OrdinaryGet answering out of it — so Web IDL
       §3.7.3 Interface prototype object's class string on this object is WINDOW's identifier and not a name of
       its own. It is also the answer a page reads: `Object.prototype.toString.call(w)` is "[object Window]"
       for a window it may see, and for one it may not §7.2.3.1's [[GetPrototypeOf]] returns null while
       §7.2.1.3.2's CrossOriginPropertyFallback answers %Symbol.toStringTag% with undefined before any walk, so
       a cross-origin read never reaches this object at all.
       Without the statement every member installed here belonged to no interface: the Web IDL gap audit could
       attribute none of the twelve below, nor `close`, nor window_message.c's `postMessage` — §9.3.3 Posting
       messages, and NOT §9.4.4, which is Message ports and defines nothing on this interface — which goes onto
       this same object, 50 installed members whose target interface it could not decide.
       IT IS idl_class_string AND NOT idl_interface_tag, because this object is not §3.7.3's Window interface
       prototype object — that one is core/frame/window.c's, built over §3.7.4's named properties object, and
       idl_args.c asserts THAT chain against the IDL at every tag. Asking §3.7.3's proto-step question about
       this object would be asking it about an object §3.7.3 does not define. Attribution is unchanged: both
       forms seed engine/idl_installed.mjs's table, so the members below are still Window's. */
    idl_class_string(ctx, proto, "Window");
    for (i = 0; i < WP_MEMBER_N; i++) {
        if (i == WP_LENGTH)
            idl_install_accessor_step(ctx, proto, PROXY_MEMBER[i], g_wp_len_getter_id, -1);
        else if (i == WP_CLOSED)
            idl_install_accessor_step(ctx, proto, PROXY_MEMBER[i], g_wp_closed_getter_id, -1);
        else if (i == WP_NAME)
            idl_install_accessor(ctx, proto, PROXY_MEMBER[i], proxy_member_get, i, g_wp_name_setter_id);
        else if (i == WP_OPENER)
            idl_install_accessor(ctx, proto, PROXY_MEMBER[i], proxy_member_get, i, g_wp_opener_setter_id);
        else if (i == WP_LOCATION)
            idl_install_accessor(ctx, proto, PROXY_MEMBER[i], proxy_member_get, i, g_wp_location_setter_id);
        else
            idl_install_accessor(ctx, proto, PROXY_MEMBER[i], proxy_member_get, i, -1);
    }
    idl_install_method(ctx, proto, "close", g_wp_close_id);
    /* §7.2.1.3.1's LAST TWO NAMES, and the ones this surface did not have. `focus` and `blur` are on the
       thirteen-name cross-origin list and were installed on the Window GLOBAL only, so §7.2.3.5's cross-origin
       branch answered `0` for them, the walk reached this object, found nothing, and `otherW.focus` was
       `undefined` — where §7.2.1.3.4 CrossOriginGetOwnPropertyHelper runs OrdinaryGetOwnProperty on the Window,
       which DOES own it, and returns a callable. `otherW.focus()` therefore threw a TypeError instead of
       running §6.6.6's steps, and no assert could see it: proxy_capture_names's reconciliation iterates the
       MEMBER TABLE, so a name with no member entry is structurally out of its reach. The loop in
       proxy_get_own's steps 4-6 is the other direction, added with this line.
       IT IS §6.6.6's OWN INSTALL, not two lines written out here — the same one window_install calls, so the
       global and this surface cannot come to hold different sets. */
    focus_install_window_members(ctx, proto);
    JS_SetClassProto(ctx, g_proxy_class, proto);
}

void window_proxy_init(JSContext *ctx)
{
    /* §7.2.3.5's cross-origin branch is the CLASS's, not the prototype's: it runs BEFORE the prototype walk,
       which is the whole reason a name that is not a member can be refused at all. */
    JSClassDef d = { "WindowProxy", .finalizer = proxy_finalizer, .gc_mark = proxy_gc_mark,
                     .exotic = &PROXY_EXOTIC };
    JSRuntime *rt = JS_GetRuntime(ctx);
    int i;

    /* ONE INSTANCE IS ONE AGENT, so this runs ONCE and a second call is a defect rather than a no-op. The
       `if (g_wp_rt == rt) return;` that used to stand under the assert made a re-declaration silent, which is
       the one shape core/agent_state.h's latch argument is about: a component that reports itself built is
       exactly what a stale handle produces, so the state must be unreachable rather than tolerated. */
    DCHECK(g_wp_rt == NULL, "window_proxy_init ran twice — one instance is one agent, and a second declaration "
                            "would re-mint the class id every live proxy is already branded with");
    g_wp_rt = rt;
    /* THE ACCESSOR SIDE OF THE CHECK IS READ, NOT KEPT — origin_agent() is the one record for this agent, and
       asserting it exists HERE says so at the declaration rather than at the first read that decides. */
    DCHECK(origin_agent() != NULL, "§7.2.1's surface was declared in an agent with no origin");
    JS_NewClassID(rt, &g_proxy_class);
    JS_NewClass(rt, g_proxy_class, &d);
    proxy_capture_names(ctx);
    /* THE POOL ENTRIES ARE THE AGENT'S — a declaration is a runtime registration, and every realm's members
       carry the same ids. Only the OBJECTS below are per realm. */
    g_wp_len_getter_id    = idl_getter_id_step(ctx, &PROXY_GET_DECL, WP_LENGTH);
    g_wp_closed_getter_id = idl_getter_id_step(ctx, &PROXY_GET_DECL, WP_CLOSED);
    g_wp_name_setter_id = idl_setter_id(ctx, IDL_DOMSTRING, false, proxy_name_set, WP_NAME);
    /* §7.2.2.4's `attribute any opener` — IDL_ANY, so the value reaches the body unconverted: every one of
       opener-setter.window.js's seven values (a symbol among them) is stored as it stands. */
    g_wp_opener_setter_id = idl_setter_id(ctx, IDL_ANY, false, proxy_opener_set, WP_OPENER);
    g_wp_close_id       = idl_method_id(ctx, NULL, 0, proxy_close, 0);
    /* §7.2.2's `[PutForwards=href, LegacyUnforgeable] readonly attribute Location location` — the SAME IDL line
       location.c installs on the global, so it carries the same setter here. Without it `frames[0].location = u`
       and `otherW.location = u` reached NOTHING while `window.location = u` navigated: one member answering
       differently depending on which of the interface's two surfaces the assignment was written against, which
       is the shape a shared declaration exists to make unwritable. The forwarding's Get lands on WP_LOCATION
       above, so the same-origin arm reaches the active document's real Location and the cross-origin arm
       reaches §7.2.4.5's filtered one — `href`'s setter is the one member BOTH of them have, which is why
       §7.2.1.3.1 CrossOriginProperties ( O ) lists `location` at all. */
    g_wp_location_setter_id = idl_setter_id_put_forwards(ctx, "location", "href");
    /* §7.2.1.3.4's per-realm getter table, DECLARED HERE AND FILLED PER REALM — the slot is a class id, which
       belongs to a runtime and therefore to the agent, while the ARRAY it holds is each realm's own. */
    g_xo_getter_slot = realm_value_declare(ctx,
                                           "HTML §7.2.1.3.4 CrossOriginGetOwnPropertyHelper's getters for this "
                                           "realm's Window, captured before any page script ran");
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — core/agent_state.h. THE POOL ENTRIES are why the
       declaration is worth making rather than merely tidy: the release gave back the atoms, the string
       list and the remote-navigable table and left every id exactly as this function set them, so a second
       agent in one process would have installed §7.2.3's whole member surface out of a pool that no longer has
       those entries — a wrong answer with a live-looking number behind it, which is the one failure neither of
       JS_FreeRuntime's censuses can report. The class id is given back for the reason that header states, and
       the two collector entries above read the record through JS_GetAnyOpaque because of it. */
    agent_state_ptr(WP_COMPONENT, &g_wp_rt, "the runtime §7.2.3's class and pool entries were declared in");
    agent_state_class(WP_COMPONENT, &g_proxy_class,
                      "§7.2.3 The WindowProxy exotic object's per-realm prototype slot and brand");
    agent_state_id(WP_COMPONENT, &g_wp_len_getter_id,
                   "§7.2.2.2 Indexed access on the Window object's `length` getter declaration");
    agent_state_id(WP_COMPONENT, &g_wp_closed_getter_id,
                   "§7.2.2.1 Opening and closing windows' `closed` getter declaration");
    agent_state_id(WP_COMPONENT, &g_wp_name_setter_id,
                   "§7.2.2.1 Opening and closing windows' `name` setter declaration");
    agent_state_id(WP_COMPONENT, &g_wp_opener_setter_id,
                   "§7.2.2.4 Accessing related windows' `opener` setter declaration");
    agent_state_id(WP_COMPONENT, &g_wp_close_id,
                   "§7.2.2.1 Opening and closing windows' `close` declaration");
    agent_state_id(WP_COMPONENT, &g_wp_location_setter_id,
                   "§7.2.2 The Window object's `location` [PutForwards=href] setter declaration");
    agent_state_id(WP_COMPONENT, &g_xo_getter_slot,
                   "the realm-value slot holding §7.2.1.3.4 CrossOriginGetOwnPropertyHelper's captured getters");
    for (i = 0; i < CROSS_ORIGIN_NAME_N; i++)
        agent_state_atom(WP_COMPONENT, &g_xo_atom[i],
                         "one of §7.2.1.3.1 CrossOriginProperties ( O )'s names, interned");
    for (i = 0; i < XO_FALLBACK_N; i++)
        agent_state_atom(WP_COMPONENT, &g_xo_fallback[i],
                         "one of §7.2.1.3.2 CrossOriginPropertyFallback ( P )'s names, interned");
    agent_state_ptr(WP_COMPONENT, &g_strings, "every origin and name string a proxy of this agent recorded");
    agent_state_ptr(WP_COMPONENT, &g_remote_navs, "the one WindowProxy per REMOTE document of this agent");
    realm_declare_intrinsic(window_proxy_install_proto);
}

JSValue window_proxy_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_proxy_class);
    DCHECK(!JS_IsNull(proto),
           "the WindowProxy prototype was asked for in a realm that never ran window_proxy_install_proto");
    return proto;   /* OWNED */
}

/* THE AGENT'S HALF, UNDONE — core/platform.h's third column, and it takes the RUNTIME because that is what an
   agent is. It took a JSContext until this diff, which is the whole of what kept it off that column and made
   it a hand-written line in three hosts instead: every one of them wrote `remote_object_free;
   window_proxy_free; remote_location_free;`, which releases the BASE whose §7.2.3 prototype every cross-agent
   reference chains to BEFORE the component built over it. Reverse declaration order is what decides the
   sequence now, and nobody has to agree with anybody about it. JS_FreeAtomRT reaches the same atoms
   JS_FreeAtom did — JS_FreeAtom is JS_FreeAtomRT(ctx->rt, a). */
void window_proxy_free(JSRuntime *rt)
{
    OwnedStr *e = g_strings;
    int i;

    /* NOT `if (!g_wp_rt) return;`. This runs from a release column that runs only where platform_agent_init
       ran, and this component's declaration is unconditional on that list — so a null runtime here is a host
       that tore down a browser it never built, and the silent return made that indistinguishable from a
       release that worked (core/agent_state.h). */
    DCHECK(g_wp_rt != NULL,
           "§7.2.3's surface was released in an agent that never declared it — window_proxy_init is a row on "
           "core/platform.c's declare column, so reaching here without it is a teardown of a browser that was "
           "never brought up");
    DCHECK(g_wp_rt == rt,
           "§7.2.3's surface was released against a RUNTIME other than the one it was declared in — its class "
           "and its five pool entries are registrations in that runtime, and giving them back against another "
           "frees atoms of a heap this component never interned into");
    while (e) { OwnedStr *n = e->next; free(e->s); free(e); e = n; }
    g_strings = NULL;
    /* §7.2.1.3.1 CrossOriginProperties ( O )'s and §7.2.1.3.2 CrossOriginPropertyFallback ( P )'s names,
       released with the agent that interned them. */
    for (i = 0; i < CROSS_ORIGIN_NAME_N; i++) { JS_FreeAtomRT(rt, g_xo_atom[i]); g_xo_atom[i] = JS_ATOM_NULL; }
    for (i = 0; i < XO_FALLBACK_N; i++) { JS_FreeAtomRT(rt, g_xo_fallback[i]); g_xo_fallback[i] = JS_ATOM_NULL; }
    /* THE REMOTE-NAVIGABLE ROWS BORROW, so this frees the TABLE and nothing in it. Emptying it here is what
       keeps a finalizer running later in the teardown from scanning freed storage: the loop then reads n == 0
       and touches nothing. */
    free(g_remote_navs);
    g_remote_navs = NULL;
    g_remote_navs_n = g_remote_navs_cap = 0;
    /* THE FIVE POOL ENTRIES COME BACK, and they are the slots this release used to keep. A declaration is a
       registration in a RUNTIME that is going away, so a carried id is an index into a pool the next agent has
       not built — read by window_proxy_install_proto at the first realm that agent creates, which installs
       §7.2.3's member surface out of whatever now sits at those indices. */
    g_wp_len_getter_id = g_wp_closed_getter_id = -1;
    g_wp_name_setter_id = g_wp_opener_setter_id = g_wp_close_id = g_wp_location_setter_id = -1;
    /* AND THE REALM-VALUE SLOT, for the same sentence one line up: it is a class id in a runtime that is going
       away, so a carried number is an index into a pool the next agent has not built — read by the first
       cross-instance member read that agent performs, which would then run whatever function now sits there. */
    g_xo_getter_slot = -1;
    /* AND THE CLASS ID, for the reason core/agent_state.h states: a class is registered in a runtime, the id
       doubles as no latch here but names a class that is gone, and every JS_GetOpaque against it would answer
       about whichever class the next agent's runtime hands that number to. proxy_finalizer and proxy_gc_mark
       read the record through JS_GetAnyOpaque precisely because this line runs before the collection that
       reaches them. */
    g_proxy_class = 0;
    /* the prototypes are the REALMS' — released with their contexts, and the ORIGINS are the AGENT's, released
       by origin_release with everything else that outlives a realm */
    g_wp_rt = NULL;
}
