/* WindowProxy: HTML §7.2.3 "The WindowProxy exotic object".
 *
 * `window`, `self`, `parent`, `top`, `frames[i]`, `opener` and `iframe.contentWindow` are WindowProxy objects, not
 * Window objects: exotic objects that forward to the current active Window of a navigable. A navigable outlives the
 * documents in it, so navigating an iframe replaces its Window while the WindowProxy a page holds stays the same
 * object.
 *
 * Under time travel the proxy is not a formality, even with one navigable:
 *   - the binding is per flow: one forked arm navigates the frame and its `contentWindow` targets the new Window,
 *     its sibling still targets the old one, and both hold the same proxy, so proxy -> Window rides the COW delta
 *     (cow_capture_host_record);
 *   - so is the same-origin check: HTML §7.2.3's [[Get]] is filtered by whether the accessor and the target's active
 *     document are same origin, and the target's origin is whatever the flow navigated it to;
 *   - a parked flow resumes with its delta, so the proxy it holds resolves through that delta, not through whatever
 *     the engine last did. */
/* Cross-instance: a WASM instance is one origin-keyed agent cluster (SECURITY.md), so a proxy may name a Window in
 * another instance. Then the binding is (document, world), not a JSValue, and the world is the sending flow's
 * decision vector, because two arms that navigated a frame differently must not resolve to one Window.
 * The navigable is still this instance's: this engine created, named and nested it and can destroy it; what lives
 * in the peer is the active document. The member table below is organised by that line, which is the difference
 * between `otherW.self` costing nothing and `otherW.self` parking a flow on a host that may never answer. */
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
#include "solver/quantum.h"   /* the slice precondition the navigation asserts rather than branches on */
#include "core/idl_args.h"
#include "core/realm.h"
/* §6.6.6's two Window members: §7.2.1.3.1 puts `focus` and `blur` on the cross-origin list, so §7.2.3's own
   surface owns them too and takes them from the component that owns §6.6.4's steps. */
#include "core/html/focus.h"
#include "core/html/html_iframe.h"
#include "core/frame/navigable.h"
#include "core/frame/window.h"
#include "core/frame/location.h"
#include "core/frame/remote_location.h"   /* §7.2.4's cross-origin arms: `location` of a peer's document */
#include "core/frame/document_lifecycle.h"
#include "core/dom/document.h"
#include <stdio.h>

/* §7.2.3's [[Window]] and the origin its same-origin check reads. Both are per flow (see the file comment), which
   is why this is a record behind a class rather than a property on an object. */
typedef struct {
    JSValue window;    /* the navigable's active Window, or JS_UNDEFINED when the document is remote (owned) */
    /* The active document's realm, when this agent holds it; NULL when the document is a peer's. Members that read
       through to the active document are answered from it: `length` counts the child navigables of that document,
       and the reading realm would count the wrong document's frames.
       Borrowed from `window` above. navigable.c's agent owns no realm (an agent-held reference is an external root
       that would keep the realm, and so this proxy whose collection says the navigable is gone, reachable): its
       list is borrowed pointers, nav_create_finish drops the builder's reference (JS_FreeContext) once the
       navigable has the realm, and navigable_free releases no realm. What keeps a child realm alive is `window`,
       whose C function objects each hold the realm that defined them (js_call_c_function reads
       `p->u.cfunc.realm`), so this pointer is valid only while that field is set. The two are written and cleared
       together, and HTML §7.5.10 "Destroying documents" step 7 clearing them is the reclamation edge
       (window_proxy_set_destroyed). An owning reference here would have to be released from a finalizer, which
       frees JSValues during collection, and the realm's Document teardown would still never run.
       NULL until the document is materialized (see proxy_realm and navigable.h): `url` is what it is built from; a
       navigated navigable is materialized at creation, one still holding its initial about:blank Document only
       when something reads through it. */
    JSContext *realm;
    char      *url;    /* the navigable's address, for a realm not yet materialized (owned) */
    /* The active document's origin, §7.1.1's record, for the §7.1.1 same origin check §7.2.1 makes. Borrowed: an
       origin is immutable and lives for the agent (core/url/origin.h), so it can sit inside the bytes proxy_of
       captures; a navigation replaces this pointer rather than mutating its target, so a parked flow's copy still
       names its world's origin. A record, not a serialization, because §7.1.1's step 1 compares identity: every
       opaque origin serializes to "null", so a string cannot tell one opaque origin read twice from two distinct ones
       (a `data:` document's own about:blank child would read as cross-origin to it). */
    const Origin *origin;
    /* The navigable's own state (see the member table below for why it is here and not in the peer). All of it is
       per flow: a flow that closed or renamed the window is the only one whose timeline contains that, and
       `parent`/`opener` are values a fork must carry. */
    JSValue parent;    /* the parent navigable's proxy; JS_UNDEFINED = top-level */
    JSValue opener;    /* §7.2.2.4's opener: the navigable that opened this one, or JS_NULL */
    /* HTML §7.3.1.3's container: the navigable container element this navigable is presented by, as its wrapper,
       or JS_NULL for one nested through nothing. §7.3.1.3 defines it as a derived relation ("the navigable
       container whose content navigable is navigable"), which a browser answers with a stored back pointer;
       walking the tree for the element whose slot names this proxy would cost the tree, and
       create-a-new-child-navigable is given the element.
       A strong reference that closes a cycle on purpose: the element's wrapper holds this proxy in its own hidden
       slot, which proxy_gc_mark accounts for, as `window` holds the proxy back through the realm's global.
       The reader confirms it against that forward slot (window_proxy_container), which makes it per flow with
       nothing to capture: §7.3.1.6's destroy-a-child-navigable severs the relation by clearing the element's
       content navigable, an ordinary property write the DOM COW delta isolates. */
    JSValue container;
    /* What that container answered, for a navigable whose container is in another heap: Permissions Policy §9.5's
       result for this navigable, in the text core/permissions_policy/permissions_policy.h serializes an inherited
       policy into, or that grammar's "there is no container".
       Not the slot above said twice: that one is an element, which this instance does not have when the navigable
       it is rooted in is a cross-origin `<iframe>` whose element lives in the creating instance (SECURITY.md keys
       an instance on `(browsing context group, origin)`). §9.5's two arguments, that element and this Document's
       origin, are the creator's, so the creator runs §9.5 and its answer travels; a cross-instance read is a
       suspend point, and a Document's install has no flow under it to take one.
       NULL means nobody stated it, not an answer: a navigable whose parent is a remote proxy is a child navigable
       by §7.3.1.3 and has a container, so core/dom/document.c crashes on the absence rather than reading it as
       §9.7 step 1's null container, which would return `Enabled` for every supported feature.
       POD and owned by the component, like `top_level_url`: proxy_strdup'd, inside the bytes proxy_of captures,
       and never freed on a navigation because a parked flow's saved bytes still name it. */
    char   *remote_container;
    /* What §3.1.3's steps answered, for that same navigable only: HTML §3.1.3 "Ancestor origins"' internal ancestor
       origin objects list, as core/dom/document.h serializes one.
       The slot above's sibling: both are results the creating instance computed because their inputs live in its
       heap. A peer given the inputs would run the algorithm wrong: §3.1.3's step 12.1 compares an ancestor against
       "parentDoc's origin", and §7.1.1's same origin decides an opaque origin by identity, which a serialization
       (every opaque origin is `null`) does not carry; steps 6-8 read the container element, which does not cross.
       NULL means nobody stated it, as above: a navigable whose parent is a remote proxy has ancestors, so
       core/dom/document.c crashes on the absence rather than reading it as the empty list, which would tell a
       cross-origin frame it is the top of its own tree.
       POD and owned by the component: proxy_strdup'd into the never-released list, inside the bytes proxy_of
       captures, so a parked flow's saved bytes name live memory. It is not on PROXY_VALS, which names only this
       record's JSValues. */
    char   *remote_ancestor_origins;
    char   *name;      /* §7.2.2.1's name: the browsing context's, not the element's (owned; see proxy_of) */
    /* Whether anyone stated this navigable's name, which decides whether `name` is a computed value or unknown
       external input. §7.4 states it (`open(url, "chan42")` names the navigable it creates, and the popup's own
       `window.name` is that string), so a navigable this engine created knows its name. The navigable the
       instance starts in is the one nobody here created: a real page may have been opened by a cross-origin
       document that set `name` before navigating, which is how window.name became an attacker source, so a host
       that cannot state it leaves it unknown and the read is concolic. A host that can (a runner that loaded the
       document itself) says so and gets the spec's "". The creator supplies this as data; it is never inferred
       from which mint was used. */
    uint8_t name_known;
    /* §7.4's "popup window is requested", decided from the `features` argument at creation and then a fact about
       the navigable for as long as it exists, which every BarProp's `visible` negates. Not per flow: no flow can
       turn a tab into a popup, so unlike `closed` and `name` the delta has nothing to capture. */
    uint8_t is_popup;
    /* HTML §7.1.7's policy container the creator handed this navigable, taken at creation and kept in the form it
       crosses a seam in, which is what §7.1.7's "clone a policy container" is here (policy_container_clone
       re-parses these bytes). Here rather than read off whichever realm materializes the document, because a
       navigable created with no address is materialized lazily on the first read through it, and that reader may
       be a different same-origin document with a different container. A creation fact, so like `is_popup` no
       flow changes it.
       One value rather than one field per item, so an item added to the container reaches every mint that must
       state where its answer comes from. Every string it names is proxy_strdup'd and outlives every flow, so a
       parked flow resuming onto its saved copy of this POD struct resumes onto live memory.
       serialized_policy_container_none for a navigable with an address (its container comes with its response)
       and for the root. */
    SerializedPolicyContainer creator_policy;
    /* HTML §7.3.2.1 "Creating browsing contexts"' `creatorBaseURL` ("Set creatorBaseURL to creator's document base
       URL"), which the create-a-new-browsing-context-and-document steps hand the initial `about:blank` as its
       about base URL. Without it a relative URL in a srcless frame or a bare `open()` resolves against
       `about:blank`, which has an opaque path and cannot be a base, so the parse fails.
       Taken at creation for `creator_policy`'s reason: the reader that lazily materializes the Document need not
       be the creator. It is the creator's base URL, not its address: a creator carrying `<base href>` passes on
       the base.
       NULL for the root navigable (nothing created it) and for a navigable created with a real address (its
       Document comes from a response and §2.4.3's about base URL is null for it). Owned. */
    char   *creator_base_url;
    /* §7.1.5's determine the creation sandboxing flags for this navigable, taken at creation for `creator_policy`'s
       reason: §7.3.2.1 hands this set to the initial about:blank Document as its active sandboxing flag set, and
       the realm that lazily materializes that Document need not be its creator's. A creation fact like
       `is_popup`, and one word, so the byte capture in proxy_of describes it completely. */
    SandboxFlags creation_sandbox_flags;
    /* HTML §7.5.1's opener policy row of this navigable's active document: "opener policy … navigationParams's
     * cross-origin opener policy" in create-and-initialize-a-Document's creation table, beside "policy container"
     * and "active sandboxing flag set" (§7.1.7's container has no such field; see core/frame/opener_policy.h).
     * On the navigable because both readers ask a navigable: §7.1.3.2's obtain-a-browsing-context-to-use-for-a-
     * navigation-response reads "browsingContext's active document's origin" and its opener policy to decide a
     * group switch, and §7.3.2.1's create reads "creator's browsing context's top-level browsing context's active
     * document's opener policy". Neither holds a Document, and a navigable §7.4 created with no address holds an
     * initial about:blank Document whose realm is materialized lazily, so a row kept only on a Document could not
     * be read for the popup §7.1.3.2 is written about.
     * Per flow at no cost: one enum word inside the bytes proxy_of captures, so an arm that navigated this
     * navigable and a sibling that did not each read their own. It moves with the rest of the binding in
     * window_proxy_navigate, since a navigation replaces the active document and this is that document's row. */
    OpenerPolicyValue opener_policy;
    /* HTML §8.1.3.1's top-level creation URL for the environments of this navigable's documents, beside the policy
       container because it is the same kind of fact: decided by the operation that created the navigable, read
       when a realm is built. §7.4 makes a child navigable inherit its creator's and gives an auxiliary one its own
       address, and §7.4.5's navigation of a top-level traversable moves it while a nested navigable's stays where
       its creation put it. Per flow like `url`, since navigating a top-level traversable moves it. A POD pointer
       inside the bytes proxy_of captures, never freed on a navigation because a parked flow's saved bytes still
       name the old one. */
    char   *top_level_url;
    /* HTML §8.1.3.1's top-level origin, the other field of that pair, separate because its definition says "This
       is distinct from the top-level creation URL's origin when sandboxing, workers, and worklets are involved."
       §7.3.2.1 "Creating browsing contexts" answers them differently (the URL is `about:blank` for a top-level
       browsing context while the origin is the initial Document's own), and §4.7 over a URL cannot express the
       third case: §7.3.2.1's determine-the-origin hands one opaque origin to several Documents, and an opaque
       origin has no serialization to recreate it from.
       Permissions §5.1 step 5 reads this one: its key is generated from "settings's top-level origin", and §3.2
       compares two keys with §7.1.1's same origin, whose step 1 is an identity comparison.
       Borrowed and POD like `origin`: an origin lives for the agent and a navigation replaces this pointer, so
       the byte capture in proxy_of describes the binding completely. */
    const Origin *top_level_origin;
    /* The negation of HTML §3.1.1 "The Document object"'s "is initial about:blank" ("Each Document has an is
       initial about:blank, which is a boolean, initially false"), held on the navigable because a navigable
       showing its initial Document has no realm to hold a Document-side field until something reaches through
       it. §7.4.4 "Non-fragment synchronous \"navigations\"" step 4 and §7.1.3.2 "Browsing context group switches
       due to opener policy"'s group-switch check read it, and both ask the spec's flag, not this field's name.
       The name states the first writer only. A navigation replacing the active document (window_proxy_navigate)
       sets it, and so does §8.4.1 "Opening the input stream" step 13, which sets the Document's flag false
       without replacing any document (window_proxy_clear_initial_about_blank; a `document.write` into a fresh
       frame is the commonest arrival). Testing the document's address instead would be wrong: navigable.c's load
       job navigates to `about:blank` for real, and that document is not the initial one.
       Per flow like the rest of this record, through the capture in proxy_of. */
    uint8_t ever_navigated;
    /* HTML §7.3's "is created by web content", a top-level traversable's, one of the two disjuncts of §7.2.2.1's
       script-closable. §7.3's create-a-new-top-level-traversable is reached from `window.open()` (navigable_open),
       so every navigable window_proxy_new mints was created by web content, and the one window_proxy_new_self
       mints (the navigable the instance started in, which the host loaded) was not. A creation fact, so like
       `is_popup` the delta has nothing to capture. */
    uint8_t created_by_web_content;
    /* §7.2.2.1's `closed` is two facts and the getter is their or: "true if this's navigable is null or its is
       closing is true". They happen at two times: `close()` sets is-closing at its own call and queues the
       destruction; §7.3.1.6 "Navigable destruction"'s destroy-a-child-navigable queues one without setting
       is-closing; and the browsing context becomes null only when §7.5.10 step 6 runs, in a task. One byte for
       both would report a destruction that had not happened while the Document, its queued tasks and its
       entangled ports were still live. */
    uint8_t closing;    /* §7.3's is closing: §7.2.2.1's close(), and only ever a top-level traversable */
    uint8_t destroyed;   /* §7.5.10 step 6 ran on this navigable's active document: its browsing context is null */
    /* §7.1.3.2 step 10's swap left this browsing context behind ("browsingContext will not be used by the new
       Document that we are about to create"), so it is null for §7.2.2.1 without any destruction. A second byte
       rather than a second writer of `destroyed`, because the two states differ in everything except `closed`: a
       destroyed navigable has no active document (step 7 released the Window and nulled the realm), while a
       swapped-past one still has the Document it had, so a read through the opener's handle answers about that
       document and not the one the navigation created in another instance. proxy_realm asserts that a navigable
       with a live realm is not `destroyed`. */
    uint8_t bc_discarded;
    /* The subtree wait, counted down per operation: how many of this navigable's child navigables have still to
       report before this one's own body may run, for each operation that walks a subtree (§7.4.2.4's beforeunload
       check, §7.5.9's unload, §7.5.10's destroy). Per flow like `closed` and `name`: two forked arms tearing down
       the same subtree each count in their own world, and a shared integer would let one arm's child complete the
       other's wait. Zero for a navigable not running that operation, which makes a child's report identify the
       operation's root. */
    uint32_t child_wait[WP_SUBTREE_OP_N];
    /* Which document the navigable's active document is: the id the world registry names worlds by, so "is this
       remote?" is one comparison against one identity. A proxy whose doc is this instance's has a live `window`;
       any other doc is in a peer instance and has none. The two are asserted together, because a proxy where they
       disagree would answer a cross-document read with this document's Window, the failure the same-origin check
       exists to prevent. */
    uint32_t doc;
} ProxyData;

/* The two recorded flags of §7.2.2.1's `closed` ("true if this's navigable is null or its is closing is true"),
   each written once so no member asks half of it: reading only the destroy flag would report a closing popup as
   open, and reading only is-closing would report a removed frame as open. A null browsing context has two
   recorded writers, §7.5.10 step 6's destruction and §7.1.3.2 step 10's swap (which discards a browsing context
   without destroying anything); reading either alone, an opener's handle would report as open a window a COOP
   group swap has cut it off from. */
/* These are not the whole of either question. A third moment nulls a navigable's browsing context, and it is the
   one pages read: §7.3.1.6's destroy-a-child-navigable step 3 severs the container relation synchronously, while
   both flags are written by the queued destruction. window_proxy_browsing_context_null is that whole fact, and
   §7.2.2.1's `closed`, §7.2.2.4's `opener` and §7.2.4's relevant Document each read it.
   `wp_closed` answers a different question: this heap holds no active document for this navigable. Its readers
   are §7.2.3's forward to W, `document`, `location`, and §7.2.3.5 step 2's indexed access, and they must not
   take the sever: §7.2.3's internal methods are performed on W, the [[Window]] internal slot, and a Window
   outlives its browsing context (`document-domain-removed-iframe.html` reads `contentWindow.status` on the line
   after `iframe.remove()` and expects the value, not `undefined`). Three questions, three readers. */
static bool wp_bc_null(const ProxyData *p) { return p->destroyed != 0 || p->bc_discarded != 0; }
static bool wp_closed(const ProxyData *p) { return p->closing != 0 || wp_bc_null(p); }

/* This component's name, spelled once for core/platform.c's row and every slot it declares to core/agent_state.h.
   The registry's row pairing is keyed by it and the compiler checks neither spelling, so a literal typed once per
   declaration is the misspelling that file's first walk exists to catch, and a wrong one is a check that never
   runs rather than one that got weaker. */
#define WP_COMPONENT "window_proxy"

static JSClassID g_proxy_class;
static JSRuntime *g_wp_rt;
/* §7.2.3's member surface. It answers from the navigable's own state where that is what the member is, and
   suspends on the host where the member reads the active document (see the member table below for which is
   which).
   Every name on it is an own property of the proxy, because §7.2.3 has no interface object: §7.2.3.5 answers each
   of these names and §7.2.3.10 lists it, so this object is where the descriptor is read from, never what a
   prototype walk reaches for a same-origin W (proxy_surface_desc). The walk does reach it for a cross-origin one,
   which is §7.2.3.5 steps 4-6 and this engine's stand-in for §7.2.1.3.4's per-realm anonymous functions: one
   object, two ways in, one answer.
   It is per realm, in quickjs's own class-proto slot, as an answer rather than an identity: a member runs in the
   realm that defined it, so one shared object would answer `parent`, `name` and `close()` for every document out
   of whichever realm built it first. `a.postMessage === b.postMessage` holds for two proxies of one realm, which
   is what §7.2.3's shared surface means; two realms have two, as they have two `Array.prototype.map`s. */
/* Two getter ids over one step declaration, because a pool entry carries the member magic and the step body reads
   it back with idl_step_magic to know which question it was asked. */
static int g_wp_len_getter_id = -1, g_wp_closed_getter_id = -1;
static int g_wp_name_setter_id = -1, g_wp_opener_setter_id = -1, g_wp_close_id = -1;
/* §7.2.2 "The Window object"'s `location` is [PutForwards=href], and this surface is that interface, so it carries
   the forwarding setter the global's `location` does, minted from Web IDL's declaration
   (idl_setter_id_put_forwards) rather than from a body of this component's. */
static int g_wp_location_setter_id = -1;


#define WP_OFF(f) (uint16_t)offsetof(ProxyData, f)
static const uint16_t PROXY_VALS[] = { WP_OFF(window), WP_OFF(parent), WP_OFF(opener), WP_OFF(container) };
static const CowRecord PROXY_REC = { sizeof(ProxyData), PROXY_VALS, 4 };

/* The capture is in the accessor, as for every other component record: a record a flow has reached is one it may
   write, so there is no write site to miss. `origin` is a POD pointer and `doc` a POD id inside the captured bytes;
   a navigation replaces them rather than mutating their targets, so the byte copy describes the binding completely.
   So the strings are not freed on navigation: a delta's saved bytes still name the old pointer, and freeing it
   would resume a parked flow onto freed memory. The component owns them and releases them at teardown, one string
   per navigation, the only arrangement in which a parked flow's binding stays readable. */
static ProxyData *proxy_of(JSValueConst v)
{
    ProxyData *p = JS_GetOpaque(v, g_proxy_class);
    if (p) cow_capture_host_record(v, p, &PROXY_REC);
    return p;
}

/* Write one of the four slots, never `JS_FreeValue(ctx, p->f); p->f = <build one>;` (see cow.h for the order).
   Releasing the old value is the hazard here: `window` names a global, so giving it back runs the page's own
   platform finalizers, and an allocation in them is a collection (js_trigger_gc's one caller is
   JS_NewObjectFromShape), which reaches this record through proxy_gc_mark and decrefs whatever the slot still
   names. Publishing first keeps the slot correct throughout.
   The record and its layout are bound here, so no site can pass a slot of one record with another's layout. Every
   write goes through it except the two mints: before JS_SetOpaque the record is unreachable by the collector and
   its slots hold nothing to release. The caller's file and line pass through, so the asserts inside name the
   write and not this line. */
static void wp_set_at(JSContext *ctx, ProxyData *p, JSValue *slot, JSValue v,
                      const char *file, int line)
{
    cow_record_set_at(ctx, p, &PROXY_REC, slot, v, file, line);
}
#define wp_set(ctx_, p_, slot_, v_) wp_set_at((ctx_), (p_), (slot_), (v_), __FILE__, __LINE__)

/* The check §7.2.1 makes, §7.1.1's same origin over two records: step 1's nonce comparison and step 2's tuple
 * comparison, both inside origin_same. Step 1 ("If A and B are the same opaque origin, then return true") is an
 * identity comparison that a serialization cannot run, since every opaque origin serializes to "null"; it matters
 * because §7.3.2.1's determine-the-origin hands one opaque origin to several Documents, so a `data:` document's
 * `about:blank` child is same origin with it.
 * The accessor side is the agent's origin: an instance is an origin-keyed agent cluster, so whoever is reading
 * has this agent's origin, and there is one. */
static bool proxy_same_origin(const ProxyData *p)
{
    DCHECK(p->origin != NULL, "a WindowProxy carries no origin — §7.4 gives every navigable's document one "
                              "and §7.3.2.1's determine the origin decides which, so a proxy without one was "
                              "minted somewhere that did not");
    return origin_same(p->origin, origin_agent());
}

/* §7.1.1's same origin-domain over the same pair, §7.3.1.3 "Child navigables"' `content document` filter: a
 * different algorithm from the one above, not a laxer spelling of it.
 * Its accessor side is the asking document's origin, not the agent's, because of §7.1.1.2: every origin in this
 * heap is same origin with the agent's by construction, so proxy_same_origin may compare against the agent's
 * record and be exact, but a domain is per Document. §7.3.1.3's content document filters "container document"
 * against the content document, and a child navigable's Document holds its own tuple record (§7.3.2.1's determine
 * the origin step 5 mints one per address), so a child that has relaxed its domain while its container has not is
 * a pair the standard's table answers ❌ for, where the agent's record would answer ✅. */
static bool proxy_same_origin_domain(JSContext *ctx, const ProxyData *p)
{
    DCHECK(p->origin != NULL, "a WindowProxy carries no origin");
    return origin_same_origin_domain(p->origin, window_proxy_origin(document_window_proxy(ctx)));
}

/* Is this a navigable's proxy: the type test every algorithm that tells a navigable from an ordinary object
   branches on. A zeroed class id is a defect here, not an answer: after window_proxy_free gives the id back, a
   test against it would report every live proxy as something else, silently, at every site. A DCHECK because
   both states are impossible rather than rare: the test is reached only from an algorithm running in a realm the
   declaration pass built, and no collector entry or other component's release calls it (the finalizer and
   gc_mark read their record through JS_GetAnyOpaque, which is why they can run after the id is gone). */
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
    /* The asking realm stands for itself as its global: win_or_proxy's mapping, asked as a type test rather than
       performed as a substitution, so one place decides it. */
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

/* The one WindowProxy per remote document: the importing half of a navigable's identity, a table rather than a
   mint at each call site because `w[0] === w[0]`, `w.frames[0] === iframe.contentWindow` and
   `event.source === event.source` are page-visible identities over one navigable, each false once two sites mint
   their own proxy for it.
   The row borrows, which is what allows the table. core/dom/document.h refuses a registry keyed by document, and
   it is right about the owning kind: a navigable is created per `open()` per flow, so an owning row would be an
   immortal root holding a proxy for every navigable a forced-execution frontier ever created. A borrowed entry
   exists only while something else holds the proxy, and proxy_finalizer takes the row out when the collector
   takes the object, so the table is exactly the set of live remote navigables and names keep nothing alive. */
typedef struct { uint32_t doc; JSValueConst proxy; } RemoteNav;
static RemoteNav *g_remote_navs;
static int        g_remote_navs_n, g_remote_navs_cap;

/* Every WindowProxy this agent has minted, scanned on the proxy's own per-flow `doc`: the (document, world) keying
   a per-document registry row cannot express. A document's realm is a POD field inside the bytes proxy_of
   captures (PROXY_REC above), so two flows that each read through one srcless navigable hold two realms for one
   name and neither is wrong.
   It has no `doc` column: a column would be a second copy of a field that already rides the delta (which
   solver/world.c's record warns against), so the key is read off the row's own record at the scan, under
   whichever delta is applied, and the answer is the asking timeline's. Two timelines each holding a realm for one
   name is then a question with two answers rather than a collision.
   Borrowed, like the remote rows above: proxy_finalizer takes a row out, so this is exactly the set of live
   proxies and names keep nothing alive. */
static JSValueConst *g_live_navs;
static int           g_live_navs_n, g_live_navs_cap;

/* Both mints record, which is why this is a function: a proxy not in the table is a navigable
   live_nav_of_document cannot see, and its two readers would report a document this agent holds as
   unmaterialized, which is right for a navigable nothing has read through and wrong for one that was never
   recorded. One function, so a third mint cannot answer differently by forgetting. */
static void live_nav_record(JSValueConst proxy)
{
    if (g_live_navs_n == g_live_navs_cap) {
        int cap = g_live_navs_cap ? g_live_navs_cap * 2 : 8;
        JSValueConst *g = realloc(g_live_navs, (size_t)cap * sizeof *g);

        CHECK(g != NULL, "window proxy: OOM recording a navigable of this agent — an unrecorded navigable "
                         "answers NO realm for its document, so every cross-instance read through it and every "
                         "program compiled for it would be told the Document was never materialized");
        g_live_navs = g;
        g_live_navs_cap = cap;
    }
    g_live_navs[g_live_navs_n++] = proxy;   /* borrowed: proxy_finalizer takes the row out */
}

/* Which of this agent's navigables is the document named `doc`, in the timeline that is asking; JS_UNDEFINED when
   none is.
   No context and no materialization: every read is a bare JS_GetOpaque, which does not go through proxy_of (so it
   captures nothing) and does not reach proxy_realm (so it builds nothing). That lets it stand inside a DCHECK
   condition, as some of solver/engine.c's callers of the realm lookup do, where a materializing call would be a
   side effect building a realm in dev and not in release. The COW machinery has applied the running flow's delta,
   so the per-flow fields the scan reads are this timeline's without a ctx.
   At most one row matches, asserted here rather than at the record because the key is per flow: a row is recorded
   before its `doc` can be read in any other timeline, so the scan is the only place the invariant is
   expressible. */
static JSValueConst live_nav_of_document(uint32_t doc)
{
    JSValueConst found = JS_UNDEFINED;
    bool hit = false;
    int i;

    DCHECK(doc != 0, "the navigable of document zero was asked for — zero is the world registry's NONE, so the "
                     "caller is holding a handle it never resolved rather than a document");
    for (i = 0; i < g_live_navs_n; i++) {
        ProxyData *p = JS_GetOpaque(g_live_navs[i], g_proxy_class);

        DCHECK(p != NULL, "a row of the live-navigable table names an object with no §7.2.3 record — both mints "
                          "attach one before the object is recorded, so this row names something that is not a "
                          "WindowProxy at all");
        if (p == NULL || p->doc != doc) continue;
        DCHECK(!hit,
               "TWO of this agent's navigables name ONE document IN ONE TIMELINE — a Document has one Window, "
               "so these are two of them wearing one name and `w[0] === w[0]` would decide by which row the "
               "scan reached first. Two TIMELINES each holding a realm for one name is the state this table "
               "exists to answer and is NOT this: one delta is applied for the whole of this scan");
        found = g_live_navs[i];
        hit = true;
    }
    (void)hit;
    return found;
}

/* The record as a collector entry sees it: JS_GetAnyOpaque, not JS_GetOpaque(val, g_proxy_class).
   core/agent_state.h states the rule: window_proxy_free gives the class id back and the collection that finalizes
   this agent's object graph runs after the release column, so a lookup against that id would answer NULL for every
   proxy a page still held and free none of them. The collector dispatched through the class, so it already has
   the id. */
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
    /* The row goes first, unconditionally: it names this object by pointer, so a row left behind would hand the
       next ask for that document a freed proxy. Above the `p` assert because the table's claim is about the
       object, not the record hanging off it. */
    for (i = 0; i < g_remote_navs_n; i++)
        if (JS_VALUE_GET_PTR(g_remote_navs[i].proxy) == JS_VALUE_GET_PTR(val)) {
            g_remote_navs[i] = g_remote_navs[--g_remote_navs_n];
            break;
        }
    /* The live-navigable row, for the same reason and in the same place: a row left behind would have the next
       scan read a freed record's `doc` and answer a realm for a navigable that no longer exists. */
    for (i = 0; i < g_live_navs_n; i++)
        if (JS_VALUE_GET_PTR(g_live_navs[i]) == JS_VALUE_GET_PTR(val)) {
            g_live_navs[i] = g_live_navs[--g_live_navs_n];
            break;
        }
    /* Both mints attach the record with nothing between JS_NewObjectClass and JS_SetOpaque that allocates on the JS
       heap or returns (proxy_strdup is malloc and JS_DupValue a refcount), so a WindowProxy with no record cannot
       exist. */
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
    /* An unmarked child is worse than a leaked parent: it keeps the internal reference gc_decref subtracts, so
       gc_scan reads it as rooted from outside the heap and it is never collected. That is what reading the record
       through a released class id would cost here, silently (core/agent_state.h). */
    DCHECK(p != NULL, "a WindowProxy was marked with no §7.2.3 record — both mints attach one before the "
                      "object can reach a collection");
    /* The Window holds the proxy (as `window`) and the proxy holds the Window: a cycle the collector has to see or
       every navigable leaks its global. `parent` and `opener` close cycles of their own: a child names its
       creator's Window, which reaches the child through the element wrapper that holds it. */
    JS_MarkValue(rt, p->window, mark_func);
    JS_MarkValue(rt, p->parent, mark_func);
    JS_MarkValue(rt, p->opener, mark_func);
    /* §7.3.1.3's container closes a tighter cycle: the element's wrapper holds this proxy in a hidden own slot and
       this record holds the wrapper back, so an unmarked container would leak every `<iframe>` a page inserted
       together with the navigable it presents. */
    JS_MarkValue(rt, p->container, mark_func);
}

/* Materialize the active document: the one place a realm is built. Every member that reads through to the active
   document comes here, and so does §7.4's navigation, which calls window_proxy_realm to materialize the navigable
   it created. See navigable.h for which navigable is materialized when, and why the difference is a spec sentence
   rather than a saving. */
static JSContext *proxy_realm(JSContext *ctx, JSValueConst proxy, ProxyData *p)
{
    /* Same-origin implies hosted, by construction: an instance is an origin-keyed agent, so a navigable this agent
       does not hold has a cross-origin active document, and every same-origin-only member is answerable in this
       turn. Asserted because the member table depends on it: otherwise a same-origin read would fall through to
       the cross-origin path. */
    DCHECK(world_doc_hosted(p->doc),
           "the realm of a navigable whose active document is a PEER's was asked for — a same-origin navigable "
           "is a realm of this agent by construction, so this proxy is cross-origin and the read that reached "
           "here should have been a SecurityError");
    /* A destroyed navigable is never materialized. §7.5.10 step 7 nulled its active document, so a NULL realm here
       means "there is no document", not "not yet", and only this tells the two apart in one field. Otherwise the
       next read through the proxy would build a fresh Document for a navigable whose browsing context is null.
       §7.2.1's cross-origin list is what a page may still read on a destroyed navigable, and proxy_forward_window
       answers all of it from this record without a realm, so a caller that reaches here asked for an active
       document. */
    DCHECK(!p->destroyed,
           "the ACTIVE DOCUMENT of a navigable §7.5.10 destroyed was asked for — that navigable has none "
           "(step 7 nulls it), and materializing one here would build a Document for a browsing context that "
           "is null; the members readable after a destruction are §7.2.1's list, answered from this record");
    if (!p->realm) {
        DCHECK(p->url != NULL, "a WindowProxy with no realm and no address was read through — a proxy over a "
                               "realm that already exists is minted by window_proxy_new_self, which carries it");
        /* The initial about:blank, which came from no response: no bytes, and §7.1.7's container is the clone of
           its creator's this navigable was created with (creator_policy). A fetch here is impossible, not
           deferred: this runs inside the property read that reached through the navigable, and a fetch suspends.
           The document an address serves arrives on the load job instead, a flow that can park (navigable.c). No
           body means no content type: §7.4 makes the initial `about:blank` an HTML document outright, so the NULL
           is that statement, not an unknown. */
        /* The navigable's creation facts, not the reading realm's: the read that materializes an about:blank child
           may come from any same-origin document, and `ctx` would give the child whichever document touched it
           first. That holds for the top-level creation URL, the container and §7.4's about base URL
           (`creatorBaseURL`). §7.3.2.1's create a new browsing context and document gives this Document
           `sandboxFlags`, the navigable's creation sandboxing flags with no CSP-derived half: a sandboxed page's
           popup inherits the creator's flags only when the propagate flag survived the parse, and unioning the
           inherited policy's CSP-derived flags in here would re-sandbox the popup `allow-popups-to-escape-sandbox`
           exists to free. The union belongs to §7.4.5's navigation (core/frame/navigable.c). */
        p->realm = navigable_realm(ctx, p->doc, p->url, p->top_level_url, p->origin, proxy, NULL, 0, NULL, NULL,
                                   p->creator_policy, p->creator_base_url, p->creation_sandbox_flags);
        wp_set(ctx, p, &p->window, JS_GetGlobalObject(p->realm));
    }
    return p->realm;
}

/* §7.4.2.2's navigate (see window_proxy.h), reached from navigable.c, which owns the fetch and the realm. */
void window_proxy_navigate(JSContext *ctx, JSValueConst proxy, JSContext *realm, uint32_t doc,
                           const char *url, const char *top_level_url, const Origin *top_level_origin,
                           const Origin *origin, OpenerPolicyValue opener_policy)
{
    ProxyData *p = proxy_of(proxy);   /* the capture is in the accessor: the whole binding rides the delta */

    DCHECK(p != NULL, "something that is not a WindowProxy was navigated");
    DCHECK(realm != NULL, "a navigable was navigated to no realm — a Document this agent holds IS a realm, and "
                          "a cross-origin destination is a peer's document, which is a host route and not this");
    DCHECK(world_doc_hosted(doc), "a navigable was navigated to a document this agent does not hold");
    /* A destroyed navigable is not navigated. §7.4.2.2 acts on the navigable's active document and §7.5.10 step 7
       nulled it; a §7.4 step 14 load still in flight when the container was removed is what §7.5.10 step 5 takes
       off the queue without running (quickjs.c's JS_DropJobsForContext walks the baseline list for that case).
       Reaching here means one of those stopped holding, and the write below would give a destroyed navigable a
       live Window again. */
    DCHECK(!p->destroyed,
           "a navigable whose active document §7.5.10 destroyed was NAVIGATED — §7.4.2.2 replaces an active "
           "document and this navigable has none, so either the load job outlived step 5's drop or a "
           "destroyed navigable was chosen as a navigation target");
    /* Nor is a swapped-past browsing context. §7.1.3.2's swap moved this navigable's next Document into a browsing
       context in another instance, so a later `w.location = …` names that one, and navigating another agent's
       navigable is a cross-instance operation this tree does not have. Writing the binding here would resurrect
       the window the swap closed. */
    DCHECK(!p->bc_discarded,
           "a browsing context §7.1.3.2's group SWAP discarded was NAVIGATED — the navigable moved to the "
           "instance the swap provisioned, so this record is the one the page's handle reads `closed` off and "
           "never a navigation target. BUILD the cross-instance navigate: a navigation of a navigable another "
           "agent holds is an operation routed to that agent (core/frame/remote_op.h), not a local write");
    /* The old Window's reference is this slot's, and the delta dupped its own copy when the capture above ran, so
       releasing it leaves the parked arm's copy intact. It is published before it is released (wp_set): the
       release runs the old global's finalizers. */
    /* The outgoing name stops resolving at the write below, so whatever still names it is asserted here rather
       than where it is next read. `doc_realm` answers a document name by asking the navigable for its active
       document's realm (window_proxy_realm_of_document), and the write moves that answer while the outgoing
       Document, its realm and every program row queued for it are still alive: a naming failure, not a lifetime
       one, so core/frame/navigable.c's realm-teardown assert does not catch it. A row that survives reaches
       `doc_realm`'s WP_DOC_NO_NAVIGABLE arm in a dev build and, in release, carries its borrowed element pointer
       into a tree this navigable no longer presents.
       The rows are the running flow's because the question is per timeline: a sibling arm that has not navigated
       still holds rows of its navigable's active document (solver/flow.h states the argument). */
    /* Named residual: the outgoing document's rows are asserted absent, not dropped. Not covered: HTML §7.5.11
       "Aborting a document load" aborts a navigated-away Document's fetch controllers, so a browser drops these
       rows, and solver/flow.h has the primitive (`flow_programs_remove_for_document`, §7.5.10 step 5's removal,
       per flow); but not every caller is established as a real navigation, since the initial about:blank handover
       reaches here too and a drop there would discard the rows of the document being navigated to. Next diff
       builds: the drop, once this assert has named which transition the rows belong to. How its absence shows:
       this assert fires on a page that navigates a frame with programs still queued for the outgoing document. */
    /* Asserted, not branched on: solver/quantum.h states that a caller branching on `quantum_slice_open` chooses
       between a scheduled path and an unscheduled one. A navigation is state a page observes, so it happens
       inside a slice or its performer is the defect. A `flow_running() == NULL ||` exemption would be wrong: the
       scheduler leaves that stamp up across the host boundary, so a NULL test is not this question, though
       `flow_programs_unstarted_for_document` needing a `Flow *` invites it. If this assert fires, a navigation
       reached here at host time, a larger finding than the rows; `engine_unload_document`'s residual names that
       population for a different lookup. */
    DCHECK(quantum_slice_open(),
           "a navigable was NAVIGATED at host time — §7.4.2.2 replaces a navigable's active document, which is "
           "state a page observes, so it runs inside a flow's slice and under that flow's own delta. A caller "
           "standing outside one has no timeline to ask about, so neither the document it is leaving nor the "
           "rows queued for it belong to anybody");
    DCHECK(p->doc == doc ||
           flow_programs_unstarted_for_document(flow_running(), p->doc) == 0,
           "a navigable was navigated while this timeline still held UNSTARTED PROGRAM ROWS for the document "
           "it is navigating away from — a row names its document by HANDLE and the write below moves the one "
           "answer that handle has, so every one of those rows is about to become uncompilable AND to be "
           "carrying a borrowed element pointer into a tree this navigable no longer presents. HTML §7.5.11 "
           "aborts a navigated-away Document's fetches, so these rows are what §7.5.10 step 5's removal takes "
           "off the queue without running; they are not work to preserve");
    p->realm  = realm;
    wp_set(ctx, p, &p->window, JS_GetGlobalObject(realm));
    p->doc    = doc;
    /* The old strings are not freed: a parked flow's saved bytes still name them (see proxy_of). */
    p->url    = proxy_strdup(url);
    DCHECK(origin != NULL, "a navigable was navigated to a document with no origin — §7.3.2.1's determine "
                           "the origin answers for every Document, and a navigation's answer is the one the "
                           "LOAD computed, never re-derived here from the address (that second answer is what "
                           "loses an inherited identity)");
    p->origin = origin;   /* borrowed, and replaced rather than mutated; see the field */
    /* The new document's environment moved with it, to where the caller states: a navigation of a top-level
       traversable puts it at the new address, while a nested navigable's stays where its creation put it (HTML
       §8.1.3.1). Not re-derived from `url`: whether this navigable is nested is a fact about the operation's
       target, and the realm the caller built was built under this exact string, so a second derivation could
       disagree. */
    DCHECK(top_level_url != NULL && *top_level_url,
           "a navigable was navigated with no top-level creation URL for the new document's environment — the "
           "realm the caller just built has one, and these two must be the same string");
    p->top_level_url = proxy_strdup(top_level_url);
    /* Its top-level origin moved with it, from the same caller for the same reason: §7.4.5's fetch gives a
       top-level traversable's new environment the new document's origin and a nested navigable's the parent
       environment's, one decision about the operation's target, so moving the URL without the origin would key
       this document's permissions to the document it replaced. */
    DCHECK(top_level_origin != NULL,
           "a navigable was navigated with no top-level origin for the new document's environment — §8.1.3.1 "
           "gives every environment one, and the realm the caller just built was built under it");
    p->top_level_origin = top_level_origin;   /* borrowed, and replaced rather than mutated; see the field */
    /* §7.5.1's opener policy row of the document this navigation creates ("opener policy … navigationParams's
       cross-origin opener policy"). It moves with the binding because it is a fact of the active document and
       this call replaces one; the value is the operation's (from the response the load fetched), never re-derived
       through this proxy, which by now reaches the document being replaced. */
    p->opener_policy = opener_policy;
    /* The navigable no longer shows what §7.4 created it with (see `ever_navigated`), written at the site that
       replaces a navigable's active document. */
    p->ever_navigated = 1;
}

void window_proxy_navigate_to_peer(JSContext *ctx, JSValueConst proxy, uint32_t doc, const char *url,
                                   const Origin *origin)
{
    ProxyData *p = proxy_of(proxy);   /* the capture is in the accessor: the whole binding rides the delta */

    DCHECK(p != NULL, "something that is not a WindowProxy was navigated to a peer's document");
    DCHECK(!world_doc_hosted(doc),
           "a navigable was navigated to a PEER's document that this agent holds — the incoming Document's "
           "origin is not this agent's, so its name must be one this instance minted and never adopted");
    DCHECK(JS_IsUndefined(p->parent),
           "a nested navigable's active document was moved to a peer — the record a peer needs to attach a "
           "Document to a navigable whose parent and container are here is not built");
    DCHECK(!p->destroyed && !p->bc_discarded,
           "a navigable §7.5.10 destroyed or §7.1.3.2 swapped past was navigated to a peer's document");
    DCHECK(quantum_slice_open(),
           "a navigable was NAVIGATED at host time — the binding is per flow, so it moves inside a slice");
    DCHECK(flow_programs_unstarted_for_document(flow_running(), p->doc) == 0 &&
           flow_fetches_for_document(flow_running(), p->doc) == 0,
           "a navigable was navigated to a peer while this timeline still held an unstarted program row or a "
           "fetch of the document it is leaving — both name it by a handle that resolves to nothing after the "
           "write below. HTML §7.5.11 step 2 discards them, and the caller runs it before this");
    /* The binding this agent held is released, not torn down: the outgoing Window's reference is this slot's and
       goes, while the outgoing realm stays the agent's (navigable.c) because a flow parked in it resumes there. A
       remote proxy holds neither, which window_proxy_is_remote asserts on every ask. */
    p->realm = NULL;
    wp_set(ctx, p, &p->window, JS_UNDEFINED);
    p->doc = doc;
    p->url = proxy_strdup(url);       /* the old strings stay: a parked flow's saved bytes still name them */
    DCHECK(origin != NULL, "a navigable was navigated to a peer's document with no origin — the load computed one");
    p->origin = origin;               /* borrowed, and replaced rather than mutated; see the field */
    /* A top-level traversable's environment moves with its document (HTML §8.1.3.1): its address and origin. */
    p->top_level_url = proxy_strdup(url);
    p->top_level_origin = origin;
    p->ever_navigated = 1;
    DCHECK(window_proxy_is_remote(proxy),
           "a navigable navigated to a peer's document still answers as one this agent hosts");
}

/* §7.5.1's opener policy row, read from the navigable's side (see the field). Through proxy_of like every other
   read of this record, so the answer is the running flow's: an arm that navigated this navigable to a
   `same-origin` document and a sibling that did not have two answers. */
OpenerPolicyValue window_proxy_opener_policy(JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);

    DCHECK(p != NULL, "the opener policy of something that is not a WindowProxy was asked for");
    /* A peer's navigable has no answer here, and this crashes rather than reporting `unsafe-none`, a legal value a
       default would make indistinguishable from a measurement. §7.3.2.1's inheritance reaches a remote top-level
       browsing context in one shape (a same-origin document nested through a cross-origin one), and answering it
       needs the cross-instance read window_proxy_window names. */
    DCHECK(world_doc_hosted(p->doc),
           "the §7.5.1 opener policy of a navigable in ANOTHER WASM instance was asked for — §7.1.3.2 and "
           "§7.3.2.1 both read it off a browsing context's ACTIVE DOCUMENT, and that document is a peer's. "
           "Build the cross-instance resolve the same way window_proxy_window names it: the flow SUSPENDS "
           "here, the peer answers in its own scheduled turn under this flow's world, and the flow resumes "
           "with the value");
    return p->opener_policy;
}

/* HTML §7.4.4 step 4's "is initial about:blank", from the navigable's side (see `ever_navigated`), read through
   proxy_of so the answer is the running flow's. */
bool window_proxy_ever_navigated(JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);

    DCHECK(p != NULL, "something that is not a WindowProxy was asked whether its navigable has been navigated");
    return p->ever_navigated != 0;
}

/* HTML §8.4.1 "Opening the input stream" step 13's "Set document's is initial about:blank to false": the flag's
 * second writer (see `ever_navigated`).
 * It is not a navigation: §8.4.1 replaces the Document's content and keeps the Document object, so every other row
 * of the binding window_proxy_navigate moves (realm, Window, document id, address, origin, the environment's
 * top-level pair, opener policy) is unchanged, and routing through that function would give the navigable a second
 * realm for a document it has. This writes the one byte §8.4.1 names.
 * Through proxy_of, so the write lands in the running flow's delta: an arm whose script opened the document and a
 * sibling on the unwritten page are two worlds, and §7.4.4 "Non-fragment synchronous \"navigations\"" step 4
 * answers each its own way. */
void window_proxy_clear_initial_about_blank(JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);

    DCHECK(p != NULL, "§8.4.1 step 13 cleared the is-initial-about:blank of something that is not a WindowProxy");
    /* A peer's navigable cannot be written from here, for window_proxy_opener_policy's reason: the flag is a fact
       about the active document, which is in another agent's heap. §8.4.1 step 4's same-origin check makes a
       cross-origin document unreachable here, so what would arrive is the same origin in another browsing-context
       group, a cross-instance operation (core/frame/remote_op.h), not a local byte. */
    DCHECK(world_doc_hosted(p->doc),
           "§8.4.1 step 13 ran against a navigable in ANOTHER WASM instance — the document open steps replace "
           "the content of a Document this agent does not hold, so the flag they clear lives in the peer's "
           "record. ROUTE the open: the flow suspends here, the peer runs the document open steps on its own "
           "Document in its own scheduled turn under this flow's world, and the flow resumes");
    p->ever_navigated = 1;
}

/* §7.3's is created by web content (see the field), read off the opaque rather than through proxy_of: a creation
   fact no flow can change, so a delta has nothing to isolate. */
bool window_proxy_created_by_web_content(JSValueConst proxy)
{
    ProxyData *p = JS_GetOpaque(proxy, g_proxy_class);

    DCHECK(p != NULL, "something that is not a WindowProxy was asked whether web content created it");
    return p->created_by_web_content != 0;
}

/* The self proxy's realm is the one asking and is already built, so nothing is materialized. Its own step because
   it is the one case where the realm precedes the proxy. */
static void proxy_adopt_realm(JSContext *ctx, JSValueConst proxy, JSContext *realm)
{
    ProxyData *p = JS_GetOpaque(proxy, g_proxy_class);

    DCHECK(p != NULL, "a realm was handed to something that is not a WindowProxy");
    DCHECK(p->realm == NULL, "a navigable was given a second realm through the ADOPT path — it already has an "
                             "active document, and replacing one is window_proxy_navigate, which moves all "
                             "five facts of the binding at once rather than handing this one a second realm");
    p->realm  = realm;   /* borrowed: no agent-held reference owns a realm (see the field) */
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
    /* A local proxy names a document whose realm this agent holds, checked because the member table decides
       "answer now" from this fact: a proxy carrying a live Window for a document elsewhere would answer a
       cross-instance read out of the wrong heap. */
    DCHECK(world_doc_hosted(doc),
           "a local WindowProxy was minted for a document whose realm this agent does not hold — a document is "
           "adopted when §7.4 decides to host it (world_doc_adopt), and one that never was belongs to a peer");
    /* JS_NewObjectClass takes the prototype from this realm's class slot, so no mint has a JS_SetPrototype to
       forget. */
    obj = JS_NewObjectClass(ctx, g_proxy_class);
    if (JS_IsException(obj)) return obj;
    p = calloc(1, sizeof *p);
    CHECK(p != NULL, "window proxy: OOM building a WindowProxy");
    /* §7.3's is created by web content: this mint is §7.4's create a new navigable, whose top-level form is
       `window.open()` (see the field). */
    p->created_by_web_content = 1;
    p->window = JS_UNDEFINED;   /* materialized by proxy_realm, at creation or on the first read */
    p->realm  = NULL;
    p->url    = url ? proxy_strdup(url) : NULL;   /* NULL only for the self proxy, whose realm is already built */
    DCHECK(origin != NULL, "a navigable was created with no origin — §7.3.2.1's determine-the-origin "
                           "answers for every Document, including the initial about:blank one §7.4 creates a "
                           "navigable with, and the answer is a RECORD whose identity the same-origin check "
                           "compares");
    p->origin = origin;   /* borrowed: an origin lives for the agent (core/url/origin.h) */
    p->name   = name && *name ? proxy_strdup(name) : NULL;
    /* §7.4 named it: this mint is the one §7.4's create-a-new-navigable reaches, so its name is the navigable's,
       including the empty one `open(url)` with no target gives. */
    p->name_known = 1;
    p->is_popup = is_popup ? 1 : 0;
    /* §7.1.7's clone of the creator's container, copied item by item into storage that outlives the creator's
       frame: the caller's bytes belong to the creating operation, and the initial about:blank is materialized long
       after it returns. A container exists even with no policies (§2.2 gives a CSP list an origin either way, and
       the initial Document is installed with it), so the assert is that there is a container, never that it
       carries policy text. */
    DCHECK(serialized_policy_container_exists(creator_policy),
           "§7.4 created a navigable with no §7.1.7 POLICY CONTAINER to clone — its initial about:blank "
           "Document has no response of its own, so a container is the only thing it can be created with, and "
           "without one it would resolve `'self'` against nothing: the one case §2.2's self-origin exists for");
    /* §7.1.4's two endpoint strings are copied for the same reason, unconditionally where the policy text is copied
       only when non-empty: §7.1.4 spells an absent endpoint as the empty string, so there is no NULL form to
       reproduce, and serialized_embedder_policy refuses a NULL. The two values are scalars and ride the struct. */
    p->creator_policy =
        serialized_policy_container(creator_policy.csp && *creator_policy.csp
                                        ? proxy_strdup(creator_policy.csp) : NULL,
                                    proxy_strdup(creator_policy.self_origin),
                                    serialized_embedder_policy(creator_policy.embedder.value,
                                                               proxy_strdup(creator_policy.embedder.endpoint),
                                                               creator_policy.embedder.report_only_value,
                                                               proxy_strdup(creator_policy.embedder
                                                                                .report_only_endpoint)),
                                    /* …and §7.1.7's integrity policy item, deep-copied like the policy text beside
                                       it: this record outlives the call that built it, so a borrowed item would
                                       dangle by the time a deferred realm materializes. */
                                    creator_policy.integrity_policy && *creator_policy.integrity_policy
                                        ? proxy_strdup(creator_policy.integrity_policy) : NULL,
                                    /* …and its referrer policy item, copied for the same reason. */
                                    creator_policy.referrer_policy
                                        ? proxy_strdup(creator_policy.referrer_policy) : NULL);
    /* §7.4's `let creatorBaseURL be null` is the absence of a creator, so NULL is a real state: the root navigable
       has no creator, and a navigable created with an address takes its Document from a response, which §2.4.3
       gives a null about base URL. */
    p->creator_base_url = creator_base_url && *creator_base_url ? proxy_strdup(creator_base_url) : NULL;
    /* §7.1.5's creation sandboxing flags, decided by §7.4's create (see the field). An empty set is the ordinary
       answer (no `<iframe sandbox>`, no propagating opener), not "not yet known": §7.1.5 says a browsing context's
       popup sandboxing flag set starts empty and an absent `sandbox` attribute is an empty iframe sandboxing flag
       set. */
    p->creation_sandbox_flags = creation_sandbox_flags;
    /* §7.3.2.1's inherited opener policy, decided and stated by the creator: "if creator's origin is same origin
       with creator's relevant settings object's top-level origin, then set document's opener policy to creator's
       browsing context's top-level browsing context's active document's opener policy", and `unsafe-none`
       otherwise, §7.1.3's initial value and a real answer. An argument, like the creator's policy container,
       because which arm applies is a fact about the creating operation that the navigable being created cannot
       recover. */
    p->opener_policy = opener_policy;
    /* Every environment has a top-level creation URL, so there is no "none" to spell: §7.4 either nests the
       navigable (the creator's) or makes it a top-level traversable (its own address). A caller with nothing to
       pass has not decided which it is building, and every member gated on §8.1.3.5 would be installed on a
       guess. */
    DCHECK(top_level_url != NULL && *top_level_url,
           "a navigable was created with no TOP-LEVEL CREATION URL — HTML §8.1.3.5 reads it to decide whether "
           "the documents of this navigable are SECURE CONTEXTS, and §7.4 says which url it is: the creator's "
           "for a nested navigable, this navigable's own address for an auxiliary one");
    p->top_level_url = proxy_strdup(top_level_url);
    /* Every environment has a top-level origin too, asserted separately because it is a separate field with a
       separate answer: §7.3.2.1 gives a top-level browsing context the URL `about:blank` and the origin of the
       initial Document it creates, neither derived from the other. Without one, Permissions §5.1 step 5 would key
       this navigable's grants to a guess. */
    DCHECK(top_level_origin != NULL,
           "a navigable was created with no TOP-LEVEL ORIGIN — HTML §8.1.3.1 gives every environment one and "
           "§7.3.2.1 says which it is: the creator's for a nested navigable, the origin of the initial "
           "about:blank Document for an auxiliary one, which is the origin this same call is being given");
    p->top_level_origin = top_level_origin;   /* borrowed: an origin lives for the agent */
    DCHECK(JS_IsUndefined(parent) || window_proxy_is(parent),
           "a navigable was minted with a §7.3.1.3 parent that is neither a WindowProxy nor JS_UNDEFINED — §7.2.2.4 "
           "makes a parent a navigable, and every walk up the tree (familiar with, `top`, the navigable-null "
           "walk) reads the slot as one");
    DCHECK(JS_IsNull(opener) || window_proxy_is(opener),
           "a navigable was minted with a §7.2.2.4 opener that is neither a WindowProxy nor JS_NULL — an opener "
           "is a browsing context's, and §7.3.2.2's familiarity walk climbs the slot as a navigable");
    p->parent = JS_DupValue(ctx, parent);
    p->opener = JS_DupValue(ctx, opener);
    /* §7.3.1.3: a navigable is created with no container and then given one ("Let navigable be a new navigable …
       Set element's content navigable to navigable"), so this mint states the pre-operation value and §7.4's
       create writes the link (window_proxy_set_container). An auxiliary navigable keeps it: §7.3.1.7 step 8
       creates one from a target name with no element to be presented by. */
    p->container = JS_NULL;
    p->doc = doc;
    JS_SetOpaque(obj, p);
    /* After JS_SetOpaque, because live_nav_of_document reads the record off every row and asserts every row has
       one. */
    live_nav_record(obj);
    return obj;
}

/* §7.2.3's proxy for the realm that is asking: the one `window`, `self` and `e.source` are. The caller stands in
   this realm rather than creating it, so the three differences from a §7.4 child are that the realm is handed over
   from outside, that nobody here stated the navigable's name, and that §7.3.1.3's parent is the host's
   statement. */
JSValue window_proxy_new_self(JSContext *ctx, uint32_t doc, const char *name, OpenerPolicyValue opener_policy,
                              JSValueConst parent, SandboxFlags creation_sandbox_flags)
{
    /* The origin is the agent's and the top-level creation URL is the realm's environment's (the one the host
       handed realm_install_intrinsics), both read where they live rather than passed in, since a caller-supplied
       one could only agree or be wrong. Not a popup: no `open()` created this navigable, so §7.4 decided nothing
       about it and its chrome is whole. No creator's policy: nothing here created it, so there is no container to
       clone; its document's policy is the one it was installed with, the host's response's. */
    JSValue tlu = realm_top_level_creation_url(ctx);
    const char *tlus = JS_ToCString(ctx, tlu);
    const Origin *tlo;
    UrlRecord rec;
    JSValue obj;
    ProxyData *p;

    CHECK(tlus != NULL, "the realm's top-level creation URL would not convert to a C string");
    /* §8.1.3.1's top-level origin for the one navigable no §7.3.2.1 ran for: the instance's root, whose
       environment the host built. The origin is §7.3.2.1 "Creating browsing contexts"' determine the origin over
       that URL with this agent's origin as the source origin, the algorithm that answers whose origin a
       Document at this address has; its two inheritance cases are the addresses §4.7 cannot answer for,
       `about:blank` (where §7.3.2.1 creates every top-level browsing context) and `about:srcdoc`. A host that
       started this instance at a real address gets §4.7's tuple, same origin as this agent's by construction. */
    url_record_init(&rec);
    tlo = url_parse(&rec, tlus, strlen(tlus), NULL) ? origin_determine(&rec, false, origin_agent())
                                                    : origin_determine(NULL, false, NULL);
    url_record_free(&rec);
    /* The one answer §7.3.2.1's determine the origin cannot reach from a URL: a `data:` or `file:` top-level
       creation URL takes its step 5 to §4.7, which mints a new opaque origin, same origin with nothing, so every
       environment of this instance would be keyed to a value no other can equal. Which Document that origin
       belongs to is a fact only the zone that created this instance holds. */
    DCHECK(!origin_is_opaque(tlo) || origin_same(tlo, origin_agent()),
           "this instance's root environment has a TOP-LEVEL CREATION URL whose §4.7 origin is OPAQUE and is "
           "not this agent's own, so §7.3.2.1's determine the origin minted a second one rather than "
           "inheriting: an opaque origin has no serialization it can be recreated from, and a URL therefore "
           "cannot say WHOSE it is. STATE "
           "§8.1.3.1's TOP-LEVEL ORIGIN beside the top-level creation URL from the zone that created this "
           "instance — it is the same statement, made by the same party, for the same reason");
    /* §7.1.5's creation sandboxing flag set, stated by the host and never assumed here, for the parent's reason
       below. For a top-level root the empty set is the standard's answer: determine-the-creation-sandboxing-flags
       gives a browsing context with no embedder its popup sandboxing flag set, which §7.1.5 says is empty at
       creation and which only §7.3.1.7 "Navigable target names"'s rules for choosing a navigable populate. For a
       root that is a child navigable, the `navigable.create` notice carries the set. What the root document's own
       `Content-Security-Policy: sandbox` adds is the other half of §7.4.5's union, added where a Document is
       created. */
    /* No policies to clone: §7.4 did not create this navigable in this agent, so there is no creator's container,
       but there is a container, and CSP §2.2's self-origin of its CSP list is this agent's own (the root Document
       is created from the response at this instance's address, §2.2.2's answer). Nothing reads it through this
       proxy (its realm exists from adoption, so proxy_realm's lazy materialization is unreachable for it); it is
       stated so that every navigable carries the container its Document runs under, as an invariant rather than
       a case at each reader. A cross-instance child does have a creator, and its clone does not come through
       here: the `navigable.create` notice carries §7.1.7's clone and the host hands it to §7.1.7's
       determine-navigation-params-policy-container beside the response's container, so a copy here would be a
       second answer. */
    /* No creator base URL, for the same reason: its Document comes from the response at this instance's address,
       which §2.4.3 gives a null about base URL. §7.1.4's item is "a new embedder policy", the standard's answer
       for a container built without a creator in this agent ("initially a new embedder policy"), not a default.
       The root Document's own embedder policy comes from the response this instance started with and reaches the
       Document through the container document_install is handed, never through this proxy. */
    /* §7.3.1.3's parent, stated by the host and never assumed here. This mint is reached for the navigable an
       instance started in, which is a top-level traversable only when the provisioning zone says so: a
       cross-origin child navigable is provisioned as a peer instance of its own, and its root is a child
       navigable presented by an element in another heap. Checked rather than trusted, because the two absences
       are spelled differently one slot apart: §7.2.2.4 makes a top-level navigable's `parent` itself
       (JS_UNDEFINED in the record) and its `opener` null (JS_NULL), and a JS_NULL here would make
       window_proxy_parent_navigable answer with neither a navigable nor the absence. */
    DCHECK(JS_IsUndefined(parent) || window_proxy_is(parent),
           "a navigable was rooted with a §7.3.1.3 PARENT that is not a navigable — the section defines a child "
           "navigable as one whose parent is non-null, so the only two answers are a WindowProxy (this "
           "instance's own, or a remote one for a parent another instance holds) and JS_UNDEFINED, which is the "
           "positive statement that this navigable is a top-level traversable");
    obj = window_proxy_new(ctx, doc, NULL, origin_agent(), name, false, creation_sandbox_flags, opener_policy,
                           serialized_policy_container(NULL, origin_serialized(origin_agent()),
                                                       serialized_embedder_policy_new(),
                                                       /* a top-level navigable created from no response states
                                                          no integrity policy, which SRI §3.8 answers with "a
                                                          new integrity policy" */
                                                       NULL,
                                                       /* …and no referrer policy: the container's default */
                                                       NULL),
                           NULL, tlus, tlo, parent, JS_NULL);
    JS_FreeCString(ctx, tlus);
    JS_FreeValue(ctx, tlu);

    if (JS_IsException(obj)) return obj;
    p = JS_GetOpaque(obj, g_proxy_class);
    DCHECK(p != NULL, "the self WindowProxy was minted without its record");
    /* The one navigable §7.4 did not create. A NULL name is the host saying it does not know this navigable's
       name, which is right for a page the browser navigated to (see the field). */
    p->name_known = name != NULL;
    proxy_adopt_realm(ctx, obj, ctx);
    return obj;
}


/* A proxy over a navigable whose active document lives in another WASM instance. Static, reached only through
   window_proxy_for_document below: minting one is where a navigable's identity can be lost, so one site does it
   and records what it made. */
static JSValue window_proxy_new_remote(JSContext *ctx, uint32_t doc, const Origin *origin, const char *name,
                                       JSValueConst parent, JSValueConst opener)
{
    JSValue obj;
    ProxyData *p;

    DCHECK(g_wp_rt != NULL, "a WindowProxy was minted before window_proxy_init ran");
    /* A remote proxy naming a hosted document is a local one that lost its Window: it would resolve to
       JS_UNDEFINED for a navigable this instance can answer for. */
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
    p->origin = origin;   /* borrowed, and never same origin with this agent's; see origin_parse */
    p->name   = name && *name ? proxy_strdup(name) : NULL;
    p->name_known = 1;   /* §7.4 named it, in this instance, before the notice crossed */
    DCHECK(JS_IsUndefined(parent) || window_proxy_is(parent),
           "a navigable was minted with a §7.3.1.3 parent that is neither a WindowProxy nor JS_UNDEFINED — §7.2.2.4 "
           "makes a parent a navigable, and every walk up the tree (familiar with, `top`, the navigable-null "
           "walk) reads the slot as one");
    DCHECK(JS_IsNull(opener) || window_proxy_is(opener),
           "a navigable was minted with a §7.2.2.4 opener that is neither a WindowProxy nor JS_NULL — an opener "
           "is a browsing context's, and §7.3.2.2's familiarity walk climbs the slot as a navigable");
    p->parent = JS_DupValue(ctx, parent);
    p->opener = JS_DupValue(ctx, opener);
    p->container = JS_NULL;   /* §7.3.1.3's link is written by the create, exactly as it is for a local one */
    p->doc = doc;
    JS_SetOpaque(obj, p);
    /* A remote proxy is in both tables, which answer two questions. The remote table answers a peer's navigable's
       identity (`w[0] === w[0]` over a document another instance holds); this one answers which navigable of
       this agent names a document in the asking timeline, and a remote one's realm is a NULL that
       `world_doc_hosted` already gates every caller on. Recording it keeps live_nav_of_document's
       at-most-one-row-per-document invariant total. */
    live_nav_record(obj);
    return obj;
}

/* The row that makes a peer's navigable one object: this instance's proxy for document `doc`. Without it the next
   ask mints a second proxy and `w.frames[0] === iframe.contentWindow` is false about one window. A function for
   its two invariants, which hold for every row however it got there: a row for a document this agent hosts is a
   second answer to a question the document's own realm answers, and a second row for one document makes
   `w[0] === w[0]` decide by which row the scan reached first. Both are properties of the table, so they are
   asserted where a row is made. */
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
    g_remote_navs[g_remote_navs_n].proxy = proxy;   /* borrowed: proxy_finalizer takes the row out */
    g_remote_navs_n++;
}

/* Which realm of this agent the document named `doc` is, in the timeline that is asking (see the header, and
   solver/world.c for why no instance-global row answers it). */
JSContext *window_proxy_realm_of_document(uint32_t doc)
{
    JSValueConst proxy = live_nav_of_document(doc);
    ProxyData *p;

    /* No navigable of that name is a real answer and the same as an unmaterialized one, which lets this be
       ctx-free: both mean "this timeline has no Document for that name", and only the caller knows whether it can
       be in that state. A remote navigable answers NULL here too, correctly: its Document is a peer's. */
    if (JS_IsUndefined(proxy)) return NULL;
    p = JS_GetOpaque(proxy, g_proxy_class);
    DCHECK(p != NULL, "the realm of a document was asked for and the navigable answering it carries no §7.2.3 "
                      "record — live_nav_of_document asserts the same thing of every row it walks");
    return p == NULL ? NULL : p->realm;
}

/* What that NULL means, as a partition (see the header). Same scan, same bare JS_GetOpaque, no ctx: it
   materializes and captures nothing, so it stands wherever the realm read above stands, including inside a
   DCHECK condition.
   The realm read's NULL has four readings, and this names which one holds. §7.5.10 "Destroying documents"'
   set-the-Document's-browsing-context-to-null step runs with window_proxy_set_destroyed nulling the navigable's
   realm, so a timeline that ran `window.close()` holds a navigable of that name with no active document. That is
   neither a name no timeline opened nor one this timeline navigated away from (a navigation replaces an active
   document and leaves a realm, which is why window_proxy_navigate refuses a destroyed navigable), and a reader
   that took it for one would materialize a Document for a null browsing context, which proxy_realm refuses.
   The remote arm is not a fourth spelling of absent: a remote navigable is in this table (window_proxy_new_remote
   says why), so the scan finds one, and its realm is NULL because its Document is a peer's and no realm of this
   heap ever will be. */
WindowProxyDocumentState window_proxy_document_state(uint32_t doc)
{
    JSValueConst proxy = live_nav_of_document(doc);
    ProxyData *p;

    if (JS_IsUndefined(proxy)) return WP_DOC_NO_NAVIGABLE;
    p = JS_GetOpaque(proxy, g_proxy_class);
    DCHECK(p != NULL, "the document state of a navigable was asked for and the navigable answering it carries "
                      "no §7.2.3 record — live_nav_of_document asserts the same thing of every row it walks");
    if (p == NULL) return WP_DOC_NO_NAVIGABLE;
    if (!world_doc_hosted(p->doc)) return WP_DOC_REMOTE;
    /* The destroyed test comes first, because `destroyed` and `realm == NULL` are both true of a destroyed
       navigable and only this order tells "there is no document" from "not yet" (proxy_realm's assert pair). */
    if (p->destroyed) return WP_DOC_DESTROYED;
    return p->realm != NULL ? WP_DOC_ACTIVE : WP_DOC_INITIAL;
}

JSValue window_proxy_of_document(JSContext *ctx, uint32_t doc)
{
    int i;

    DCHECK(doc != 0, "the WindowProxy of document zero was asked for — zero is the world registry's NONE, so "
                     "the caller is holding a handle it never resolved rather than a document");
    /* A document this agent hosts is answered by its own navigable, never out of the remote table below, which maps
       a peer's document to the one proxy that resolves it; a row there for a hosted document would be a second
       answer. The navigable is the authority and the realm is read off it (no instance-global document-to-realm
       row is right once two timelines have each materialized one), and the round trip is asserted below: the two
       directions of the binding must name one object. */
    if (world_doc_hosted(doc)) {
        JSValueConst held = live_nav_of_document(doc);
        JSContext *realm;

        DCHECK(!JS_IsUndefined(held),
               "the WindowProxy of a document THIS AGENT HOSTS was asked for and this agent holds no navigable "
               "of that name AT ALL — hosting is decided by §7.4 before any navigable is minted "
               "(world_doc_adopt), so the two statements were made in the wrong order, or a proxy that answered "
               "for this document has been collected while a peer still held a reference into it");
        if (JS_IsUndefined(held)) return JS_UNDEFINED;
        /* The navigable outlives its document, so a realm is not how a navigable is identified. HTML §7.2.3 "The
           WindowProxy exotic object" states the lifetime: "Each browsing context has an associated WindowProxy
           object", and on a navigation "the Window object wrapped by the browsing context's associated
           WindowProxy object is changed". The proxy is the long-lived half, the realm read reaches the active
           document, and this entry hands out the navigable: one with no active document in this timeline still
           answers with `held`.
           Two states reach here with no realm: an initial about:blank this timeline has not materialized (a realm
           is per flow, so a sibling arm that did materialize one can have lent the reference), and a navigable
           whose active document HTML §7.5.10 "Destroying documents" destroyed (its step 6, "Set document's
           browsing context to null", reaches window_proxy_set_destroyed, which nulls the realm while the row
           stays until the collector takes it). What stays readable of either is HTML §7.2.1 "Security
           infrastructure for Window, WindowProxy, and Location objects"' cross-origin list, answered from the
           record `held` names. */
        realm = window_proxy_realm_of_document(doc);
        if (realm == NULL) {
#if APICLIENT_DEV
            /* Which of the five states, asked of the component that partitions it: two are the states answered
               above and the other three are refused by the two tests above, so every arm here is a guard over a
               closed enumeration and names what moved. The state is read fresh beside the realm because the pair
               is where a disagreement is. */
            switch (window_proxy_document_state(doc)) {
            case WP_DOC_INITIAL:
            case WP_DOC_DESTROYED:
                break;                 /* the two states a hosted navigable legitimately has no realm in */
            case WP_DOC_REMOTE:
                DFAIL("the state scan called this navigable's active document a PEER's for a document "
                      "`world_doc_hosted` answered TRUE for — both statements are about one `doc` under one "
                      "applied delta, so the row's own `doc` and the world registry disagree");
                break;
            case WP_DOC_NO_NAVIGABLE:
                DFAIL("the state scan found no navigable for a document the scan one screen up DID find one "
                      "for — two walks of one table under one applied delta cannot disagree unless something "
                      "between them moved the delta");
                break;
            case WP_DOC_ACTIVE:
                DFAIL("the realm read answered NULL for a navigable whose own record says its active document "
                      "IS materialized — both are bare field loads off one row under one applied delta, so "
                      "they cannot disagree unless something between them moved the delta");
                break;
            }
#endif
            return JS_DupValue(ctx, held);
        }
        DCHECK(JS_VALUE_GET_PTR(document_window_proxy(realm)) == JS_VALUE_GET_PTR(held),
               "the realm answering for this document and the navigable the scan found are not one navigable — "
               "§7.2.3 gives a navigable ONE WindowProxy and a realm IS a document (core/dom/document.h), so "
               "the (document -> navigable) and (realm -> navigable) directions of one binding disagree");
        return JS_DupValue(ctx, held);
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
           "lookup above answers for every hosted document THIS AGENT HOLDS A NAVIGABLE FOR, whatever state "
           "that navigable's active document is in, so reaching here means it holds NONE: hosting is decided "
           "by §7.4 before any navigable is minted (world_doc_adopt), and minting would give one navigable a "
           "second proxy that resolves nothing");
    /* The refusal is a guard as well as an assert, because `!world_doc_hosted(doc)` is the mint's own
       precondition (window_proxy_new_remote and remote_nav_record assert it too), and all three asserts are
       compiled out of the shipped build. Minting in release would `live_nav_record` a second row for `doc`,
       breaking live_nav_of_document's at-most-one-row invariant, and hand the next scan a record whose `window`
       is undefined and whose origin is a peer's, so a same-origin read of this agent's own navigable would answer
       cross-origin under §7.2.1's filter. Propagating the absence leaves no such state, and is in the contract
       the arriving-identity caller reads against (core/frame/remote_object.h: a navigable, remote or local, "or
       JS_UNDEFINED"). */
    /* Named residual: the absence this propagates is a refusal, not §7.2.3's answer. Not covered: a document this
       agent hosts and holds no navigable for has a WindowProxy in the standard and none here, so a routed
       delivery gets no `source` and an arriving identity's parent or opener slot falls to its `none`, which
       §7.2.2.4 "Accessing related windows" reads as a top-level navigable, so a cross-origin child presents as
       top-level rather than unknown. Next diff builds: a local navigable rebuildable from an arriving identity,
       which needs the opener policy and §7.1.5 sandboxing flag set window_proxy_new_self takes (fields
       remote_object.c's record does not cross for a hosted document), or the remote reference holding the
       borrowed live-navigable row alive (proxy_finalizer drops the row while a peer still names it, which is
       why this state exists). How its absence shows: in dev, an abort at the hosted-no-navigable assert above on
       an instance whose peer held a reference across a collection; in release, a delivered message whose
       `source` is undefined, or a navigable whose `parent` answers itself while an element in another document
       presents it. */
    if (world_doc_hosted(doc)) return JS_UNDEFINED;
    obj = window_proxy_new_remote(ctx, doc, origin, name, parent, opener);
    if (JS_IsException(obj)) return obj;
    remote_nav_record(obj, doc);
    return obj;
}

/* §7.2.2.1's `closed` as this agent's record of it. A fact about the navigable, so `window.closed` and
   `iframe.contentWindow.closed` are one answer, captured through proxy_of so the flow that closed the window is
   the only one whose timeline contains it.
   This agent's record is the right question for the callers: each is an engine walk over navigables this agent
   holds (§7.3.3 "Fully active documents"' ancestor chain, §7.1's named-target search, the tree-order walk)
   asking whether to keep walking this agent's tree. The JS-visible member has a different answer path: a
   traversable whose active document is in another instance is closed by whichever agent ran §7.2.2.1's
   close(), so the member suspends and asks it (proxy_get_step). A caller that wants the standard's `closed`
   about an arbitrary navigable goes through the member, because only it can suspend.
   An or over two facts, the getter's own wording: they are set by two algorithms at two times, and every read
   in this file goes through here so no member asks only one. */
bool window_proxy_closed(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);

    DCHECK(p != NULL, "the closed state of something that is not a WindowProxy was read");
    /* §7.2.2.1's or, over the fact rather than the two recorded flags: a frame removed from the tree has a null
       browsing context from that line on, the disjunct wp_bc_null cannot see (see its definition). `closing`
       stays a flag, written by close() in whichever agent ran it. */
    return p->closing != 0 || window_proxy_browsing_context_null(ctx, proxy);
}

/* The ask §7.5.10 step 7 is, counted where it is performed (see window_proxy.h for why the outcome cannot be
   asserted and this can). A lifetime count per agent, not on the record: it counts asks across arms, so a rewound
   arm that destroys again is a second ask. */
static long long g_step9_releases;

long long window_proxy_destroy_releases(void) { return g_step9_releases; }

/* §7.5.10 steps 6 and 7, the completion of a destruction, written only by the destroy job (see window_proxy.h
   for why the two steps are one write and why the second is this engine's only reclamation edge). Step 6 is also
   what §7.5.10 step 5's wait reads off each child, so it is asked separately from `closed`: a navigable whose
   top-level traversable is only closing has not been destroyed, and a wait that accepted `closed` would finish
   before its subtree had. */
void window_proxy_set_destroyed(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);   /* the capture is in the accessor: the whole binding rides the delta */

    DCHECK(p != NULL, "something that is not a WindowProxy had its browsing context set to null");
    /* Once per document, asserted, because step 7 hands a reference back and a second run would hand back one it
       no longer holds. Every path that could reach here twice refuses to: descend_enqueue returns for a destroyed
       navigable, the fan-out reports a destroyed child instead of queuing it, and §7.3's close chains
       unload-then-destroy over a subtree whose members destroyed themselves at §7.5.9 step 20. A second arrival
       is one of those paths having stopped refusing. */
    DCHECK(!p->destroyed,
           "§7.5.10 destroyed one navigable's active document TWICE — the second run releases a Window "
           "reference this record has already given back, and the first thing that reads the navigable "
           "afterwards reads a freed global");
    /* A peer's navigable has nothing local to release and is not destroyed from here: the agent holding the
       document runs §7.5.10 over it, and this side learns the outcome the way it learns `closed`. */
    DCHECK(world_doc_hosted(p->doc),
           "§7.5.10 was run over a navigable whose ACTIVE DOCUMENT is in another WASM instance — the ports, "
           "the queued tasks and the Window are all the peer's, so the destruction is the peer's to perform "
           "and this side has nothing to null");
    p->destroyed = 1;                                          /* step 6 */
    /* Step 7: the navigable stops naming the Document. The delta already holds its own dup of this Window (the
       capture above ran before the write), so releasing the live slot leaves every parked arm's copy intact and
       rewinding this flow restores the binding exactly, as in window_proxy_navigate. That is what lets a
       destruction give memory back without a flow losing the document it stands in. */
    wp_set(ctx, p, &p->window, JS_UNDEFINED);
    p->realm  = NULL;
    /* `materialized` now answers that this navigable has no active document; proxy_realm asserts the other side:
       a NULL realm on a destroyed navigable means "there is none", never "not yet". */
    DCHECK(p->realm == NULL && JS_IsUndefined(p->window),
           "§7.5.10 step 7 left a destroyed navigable still naming a Document — the realm behind it can then "
           "never be reclaimed, because this reference is the one the collector cannot get past");
    /* The ask is recorded here, after the release rather than at the call that asked for it, so a count taken
       here cannot report a step that returned early; it is the last statement for that reason. */
    g_step9_releases++;
}

/* §7.1.3.2 step 10's discard: the other writer of the state §7.2.2 "The Window object" spells "if this's browsing
   context is null then return null", and it releases nothing. §7.5.10's write above is a destruction and hands
   the Window back; this records that a group swap left a browsing context behind while its Document goes on
   existing as it was. That asymmetry is the spec's: step 10's note says only that the user agent "might destroy
   it at this point", and this engine keeps a superseded document alive on every navigation because a flow parked
   in it resumes there.
   Per flow through proxy_of: a sibling arm whose response carried no such policy never swapped, and its handle
   stays open. */
void window_proxy_discard_browsing_context(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);   /* the capture is in the accessor: the whole binding rides the delta */
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
    /* §7.2.2.1's `closed` reads it, asserted here because this byte and that getter are the whole contract between
       the swap and the page: without the disjunct, the opener's handle would report as reachable a window real
       Chrome has cut off, and the @S half would emit a breakout the browser forbids. */
    DCHECK(window_proxy_closed(ctx, proxy),
           "a browsing context §7.1.3.2's swap has just discarded still answers §7.2.2.1's `closed` as false — "
           "the getter's null browsing context has THREE writers (this swap, §7.5.10's destroy "
           "and §7.3.1.6's synchronous sever) and is reading fewer than all of them");
}

/* The wait's step 3: the number of child navigables this one waits on for this operation, recorded before any of
   them is queued. Through proxy_of, so the count belongs to the flow performing the walk. */
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

/* A child reported: the wait's incrementDestroyed/incrementUnloaded/completedTasks, counted down. Answers
   WP_WAIT_LAST for the last child (the wait is over, the only moment the waiting navigable's own body may be
   queued), WP_WAIT_MORE while siblings are outstanding, and WP_WAIT_NONE when this navigable is not running that
   operation, which tells the reporter it is the operation's root rather than underflowing a count never
   raised. */
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

/* §7.2.2.1 step 3's and §7.3's step 1's test: is closing alone, the half of `closed` that says a close is under way
   rather than finished. Through proxy_of, so the flow that called close() is the only one whose timeline contains
   it. */
bool window_proxy_closing(JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);
    DCHECK(p != NULL, "the is-closing state of something that is not a WindowProxy was read");
    return p->closing != 0;
}

/* §7.3.2.2's familiar with (see window_proxy.h). A is the incumbent realm's browsing context, which is this
   agent's, so every "A's active document's origin" below is origin_agent() and each same-origin test is
   proxy_same_origin; A itself is this realm's own proxy, whose origin is the agent's. Every link of the chain is
   a WindowProxy: the walk starts at one and climbs `parent` and `opener` slots, which both mints assert hold a
   WindowProxy or the slot's absence. The record of each link is dereferenced in release, so each is a CHECK. */
bool window_proxy_familiar_with(JSContext *ctx, JSValueConst proxy)
{
    JSValue b = JS_DupValue(ctx, proxy);
    bool ok = false;

    DCHECK(window_proxy_is(proxy), "§7.3.2.2 was asked about something that is not a navigable's WindowProxy");
    /* The loop is step 3's recursion unrolled: "B is an auxiliary browsing context and A is familiar with B's
       opener browsing context" is the same question about a different B, and an opener link is fixed at creation
       (§7.2.2.4's setter only severs one), so the chain is finite and acyclic: every opener predates the
       navigable it opened. */
    for (;;) {
        ProxyData *p = JS_GetOpaque(b, g_proxy_class);
        JSValue anc;

        CHECK(p != NULL, "§7.3.2.2's B is not a WindowProxy — B is the asked navigable or an opener link");
        if (proxy_same_origin(p)) { ok = true; break; }                     /* step 1 */
        {                                                                   /* step 2: A's top-level is B */
            JSValue top = window_proxy_top_navigable(ctx, document_window_proxy(ctx));
            bool is_b = JS_VALUE_GET_PTR(top) == JS_VALUE_GET_PTR(b);
            JS_FreeValue(ctx, top);
            if (is_b) { ok = true; break; }
        }
        /* Step 4: an ancestor browsing context of B whose active document is same origin with A's, which includes
           A being that ancestor. */
        anc = JS_DupValue(ctx, p->parent);
        while (!JS_IsUndefined(anc)) {
            ProxyData *q = JS_GetOpaque(anc, g_proxy_class);
            JSValue up;

            CHECK(q != NULL, "a §7.3.2.2 ancestor of B is not a WindowProxy — the loop stops at JS_UNDEFINED");
            if (proxy_same_origin(q)) { ok = true; break; }
            up = JS_DupValue(ctx, q->parent);
            JS_FreeValue(ctx, anc);
            anc = up;
        }
        JS_FreeValue(ctx, anc);
        if (ok) break;
        DCHECK(!JS_IsUndefined(p->opener), "a navigable's opener slot holds JS_UNDEFINED — the mints write JS_NULL");
        if (JS_IsNull(p->opener)) break;                                    /* B is not auxiliary */
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
    /* Through proxy_of like every other read of this record: a child's destruction is written into the
       destroying flow's delta, so a wait that read the baseline would never see the child it had just destroyed
       and would park for ever, depending on which flow ran the child's job. */
    ProxyData *p = proxy_of(proxy);
    DCHECK(p != NULL, "the destroyed state of something that is not a WindowProxy was read");
    return p->destroyed != 0;
}

/* Is this record's realm borrow dangling (see window_proxy.h for the question, and why it is neither `destroyed`
   nor a plain "does it still name this realm")? Capture-free and collection-safe, which is why it is a second
   entry point and not a caller of the accessor above.
   Not proxy_of, the distinction the collector entries draw: proxy_of captures the record into the running flow's
   delta because a record a flow has reached is one it may write, and there is no flow here. The caller is
   quickjs's realm-teardown hook, which fires inside a collection, where a capture would dup this record's
   JSValues onto an object being torn down. proxy_finalizer and proxy_gc_mark skip the capture for that reason;
   this read is narrower than theirs, two field loads and no reference touched. */
bool window_proxy_realm_dangling(JSValueConst proxy, JSContext *realm)
{
    JSClassID cid = 0;
    /* proxy_collector_record's call with its second output kept. The collector may discard the class id because
       it was dispatched through the class; this caller was handed a value, so the id is the only thing that can
       say what it holds. JS_GetAnyOpaque for the reason at proxy_collector_record: window_proxy_free gives the
       class id back, and the collection that reaches a realm teardown runs after the release column
       (core/dom/document.c: a child navigable's document is released from that hook, inside JS_RunGC or
       JS_FreeRuntime, both after platform_agent_free). */
    const ProxyData *p = JS_GetAnyOpaque(proxy, &cid);

    DCHECK(realm != NULL, "a WindowProxy was asked whether it dangles at NO realm — the question is asked at "
                          "the teardown of one particular JSContext and is meaningless without it");
    /* The class is checked where there is still a class to check against. window_proxy_free zeroes
       g_proxy_class by design, so after the release column a live proxy carries an id this component cannot
       name, and comparing against zero would report every one as something else. The caller's own argument
       stands in there: core/frame/navigable.c asks this of document_window_proxy(cctx), which core/dom/document.c
       asserts is that realm's own §7.2.3 WindowProxy. A zero id with no record is a proxy the collector already
       finalized, which free_object writes as one pair (u.opaque and class_id are cleared together after the
       finalizer returns). */
    DCHECK(g_wp_rt == NULL || cid == g_proxy_class || (cid == 0 && p == NULL),
           "the realm-borrow question was asked of an object that is not a §7.2.3 WindowProxy — it is answered "
           "by reading a ProxyData off the class opaque, so a foreign class's record would be read with this "
           "record's layout and the answer would be two fields of somebody else's struct");
    /* The collector already took the record, a positive answer: proxy_finalizer frees a ProxyData whole, so a
       proxy with none holds no binding and no realm can be read through it. §7.5.10's note makes this reachable
       ("Even after destruction, the Document object itself might still be accessible to script, in the case where
       we are destroying a child navigable"), so the proxy and the realm are two objects one collection may take
       in either order. */
    if (p == NULL)
        return false;
    /* The tag of `window`, never its target: the Window this slot names may have been finalized earlier in the
       same sweep, so JS_IsUndefined, which reads the JSValue's own tag, is all that may be asked of it. */
    return p->realm == realm && JS_IsUndefined(p->window);
}

bool window_proxy_browsing_context_null(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);   /* through the accessor, for window_proxy_destroyed's reason */

    DCHECK(p != NULL, "the browsing context of something that is not a WindowProxy was asked about");
    /* The third writer, and the only synchronous one. Both flags are set by §7.5.10's queued destruction, and a
       page observes this on the line after `iframe.remove()`, when §7.3.1.6 step 3's sever has happened.
       `embedded-opener-remove-frame.html` shows the difference in its structure: it checks a removed frame's
       `opener` immediately and a closed popup's behind a `step_timeout`, because the first is discarded
       synchronously and the second from a task. */
    return window_proxy_navigable_null(ctx, proxy) || wp_bc_null(p);
}

void window_proxy_disown_opener(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);   /* the capture is in the accessor: this write rides the flow's delta */

    DCHECK(p != NULL, "something that is not a WindowProxy was asked to disown its opener");
    wp_set(ctx, p, &p->opener, JS_NULL);
}

void window_proxy_set_opener(JSContext *ctx, JSValueConst proxy, JSValueConst opener)
{
    ProxyData *p = proxy_of(proxy);   /* the capture is in the accessor: this write rides the flow's delta */

    DCHECK(p != NULL, "something that is not a WindowProxy was given an opener");
    /* §7.2.2.4's `opener` is a navigable's, so the slot stores the opener's proxy, never its Window: a Window here
       would make the `top` walk stop at the proxies and read a scriptable property to continue, and would make a
       popup's `opener` a Window rather than the navigable it belongs to. */
    DCHECK(window_proxy_is(opener),
           "§7.2.2.1 step 16.2 was given something that is not a WindowProxy as the opener browsing context — "
           "the step says \"sourceDocument's browsing context\", which is a NAVIGABLE, and every other reader "
           "of this slot (§7.2.2.4's `opener`, window_proxy_opener_navigable, the familiarity walk) is written "
           "against a proxy");
    /* Null is not this function's to say. §7.2.2.4's null branch disowns, a different state change with a
       different observable (no own property is defined), and has its own entry point above; null here would make
       two spellings of "no opener" that could disagree. */
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
    /* The two facts must agree, read together on every ask, because a proxy where they disagree hands a
       cross-document read this document's Window and nothing downstream can tell. A proxy over a peer's document
       never builds a realm (that would be this agent answering for a document it does not hold); the converse is
       not asserted, since a hosted navigable has no realm until something reads through it. */
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
    /* The seam, which crashes rather than answering. A proxy whose navigable lives in another WASM instance has no
       local Window, and handing back this document's would be a cross-document read that silently succeeded.
       The message names what to build: the binding is (document, world), and resolving it is a host round trip
       because only the host knows which instance holds that document and which of its flows is in that world. */
    DCHECK(world_doc_hosted(p->doc),
           "a WindowProxy names a navigable in ANOTHER WASM instance — build the cross-instance resolve: the "
           "flow SUSPENDS here (the same snapshot path as an await), the peer answers in its own scheduled turn "
           "under this flow's world, and the flow resumes with the value. The host owns the routing because "
           "only it knows which instance holds that document — window_proxy_doc names which one");
    /* JS_GetGlobalObject already returns a reference, so no dup: a second one would leak the Window and, since a
       global roots a whole realm, the entire child page. */
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
    /* A destroyed navigable has no name, the answer the `name` getter gives from the same two fields, so named
       access cannot find a frame the page has removed. */
    return wp_closed(p) || !p->name ? "" : p->name;
}

/* §7.2.2.1's `name` as a value, computed in one place. A Window and its WindowProxy are two spellings of one
   navigable, so `window.name` inside a document and `w.name` from its opener are one attribute of one record:
   HTML §7.2.2.1 says "return this's navigable's target name", and the popup's own script reading it is the
   ordinary case.
   It is an attacker source only where the name is unknown. A navigable §7.4 created carries the name §7.4 gave
   it, which the code that wrote `open(url, "chan42")` determined, so reporting it as unknown would invent
   uncertainty the program does not have. The navigable nobody here created is the one whose name a cross-origin
   opener may have set, and that read is concolic, carrying the spec's "" as its example, so it forks control
   flow without losing the value. */
JSValue window_proxy_name_value(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);

    DCHECK(p != NULL, "the name of something that is not a WindowProxy was read");
    /* HTML §7.2.2 "The Window object": "If this's navigable is null, then return the empty string.", the predicate
       `closed` reads, so a frame removed from the tree answers "" on the next line as it answers `closed`; a
       closing top-level traversable still has its navigable and keeps its name. */
    if (window_proxy_browsing_context_null(ctx, proxy)) return JS_NewStringLen(ctx, "", 0);
    if (p->name_known) return JS_NewString(ctx, p->name ? p->name : "");
    /* The world's: §7.2.2.1's name is written by whoever navigated this navigable, which is another document and
       often an attacker's, and nothing in this document's served bytes states it. */
    return concolic_new(ctx, "{window.name}", "window.name", CONCOLIC_WHOSE_WORLD,
                        JS_NewStringLen(ctx, "", 0));
}

/* §7.2.2.1's `name` setter, the one place it is written. It renames the browsing context, so `frameW.name = "B"`
   leaves the element's `name` content attribute alone (the spec files assert that pair together, and reflecting
   one into the other would pass neither). Writing it makes the name known: a page that set it determined it. */
JSValue window_proxy_name_assign(JSContext *ctx, JSValueConst proxy, JSValueConst v)
{
    ProxyData *p = proxy_of(proxy);
    const char *n;

    DCHECK(p != NULL, "the name of something that is not a WindowProxy was written");
    /* §7.2.2: "If this's navigable is null, then return." */
    if (window_proxy_browsing_context_null(ctx, proxy)) return JS_UNDEFINED;
    /* A concolic name is a capability not yet built, not a value to stringify. `name` is stored as C bytes
       because named access compares it, so a concolic would have to be de-tainted to be stored, which a concolic
       must never survive; the message names what to build. */
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

/* §7.4's popup decision, read back: what §7.2.2.5's six BarProps answer from. A destroyed navigable keeps the
   answer it was created with: `closed` is what a page checks, and a bar that changed its mind on close would be a
   second fact where there is one. */
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

/* Through the capturing accessor rather than the raw opaque: this is per-flow state, and a flow that reads it is
   on its way to building a realm under it. */
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
    /* A remote proxy has no set to give, and answering the empty set would be worse than crashing: an empty set
       means "not sandboxed", so a cross-origin `<iframe sandbox>` would report itself unsandboxed.
       The peer has the set: navigable.c writes §7.1.5's serialized set on the `navigable.create` notice and both
       hosts take it off (wpt_runner.c hands it to the re-executed child, qjs_abi.h declares it on both rooting
       entries). What is missing is this instance's own record of it, which splits by where the proxy came from.
       A remote proxy this instance created is minted on navigable.c's create arm, which ran §7.1.5 just before
       (its inputs, the `<iframe sandbox>` element and this document's active set, do not cross), and the value
       is dropped because window_proxy_new_remote takes no such argument. A remote proxy decoded from a peer's
       navigable identity (core/frame/remote_object.c) or minted for a message's source
       (core/frame/window_message.c) was never computed here, and only that population is a route.
       The arm is a guard, not a work item: every reader is refused upstream by its own `window_proxy_is_remote`
       assert carrying its own remedy (navigable.c's navigate and reload), and the third runs inside the load
       step of a navigable this agent holds. */
    DCHECK(!window_proxy_is_remote(proxy),
           "the CREATION SANDBOXING FLAGS of a navigable whose Documents a PEER instance creates were asked "
           "for — §7.1.5's flag set is handed to a Document at its creation and this instance creates none for "
           "a remote navigable. The PEER HAS THE SET: it crosses on the `navigable.create` notice. What is "
           "absent is this instance's own record of it, and which diff that is depends on where this proxy "
           "came from — see the paragraph above this assert");
    return p->creation_sandbox_flags;
}


/* §7.2.1's member surface, and the distinction that decides how each member is answered.
 *
 * A navigable is not its active document: a proxy outlives the documents in it, and that is why most of the
 * surface below never leaves this instance. `name`, `opener`, `parent`, `top` and the four self-references are
 * properties of the navigable, and a navigable belongs to the instance that created it, which made the child,
 * named it and knows what it was nested in. `otherW.self === otherW` is true by definition about a navigable
 * nobody has to look inside, so answering the navigable's own state locally is where the state is, not an
 * optimisation.
 *
 * `closed` is not one of them. §7.2.2.1 makes `closed` the or of two facts about the top-level traversable, its
 * browsing context being null and its is closing, and `close()` sets is closing in the agent that runs it. A
 * popup that calls `window.close()` on itself writes its own instance's record and the opener's copy of the same
 * traversable never hears, so the opener asks the instance whose record the standard is talking about. */
/* `closed` is monotonic, which makes it answerable: §7.2.2.1's close() step 3 returns early when is closing is
 * already true, and a null browsing context is terminal, so a record that says closed is the answer wherever it
 * is kept and only one that says open leaves the question to the peer. `length` has no such half (it is a live
 * count of the peer's active document, asked every time), so the two members share the step machine and differ
 * in that one branch.
 * The remote half is a step machine because `otherWindow.length` must answer at its own call site and the answer
 * is not available in this turn: the question is posted on the flow's pending register, the step returns
 * JS_STEP_YIELD, and the scheduler parks the flow, byte-identical at any depth, while its siblings run. The host
 * routes it to the instance holding that document, since SECURITY.md makes the trusted zone the only thing that
 * knows which that is. The world travels with it: the question is document 7's `length` in this flow's world, so
 * two arms that framed it differently get different answers.
 * A local proxy forwards everything to its Window with an ordinary read, in this turn: it is the same document,
 * so its Window is authoritative for every member, the navigable's own included. */
enum {
    /* §7.2.2's four names for this navigable's own proxy. `window`, `self` and `frames` are on §7.2.1's
       cross-origin list; `globalThis` is the global object, which is the proxy, and same-origin only. The
       distinction costs nothing here because both answers are the same object. */
    WP_WINDOW, WP_SELF, WP_FRAMES, WP_GLOBALTHIS,
    WP_PARENT, WP_TOP, WP_OPENER,
    /* The first of the two members that leave this instance: the top-level traversable's is-closing, which
       §7.2.2.1's close() writes in whichever agent ran it, so the peer's record is the one the standard names
       whenever this agent's own says open. The enum order is load-bearing (PROXY_MEMBER and PROXY_CROSS_ORIGIN
       are indexed by it). */
    WP_CLOSED,
    WP_NAME,
    WP_LENGTH,   /* the active document's child-navigable count: the other member that leaves this instance */
    /* §7.2.2's `document`, same-origin only. It answers with the other realm's Document object, always in this
       turn: an agent is origin-keyed, so a same-origin navigable is a realm of this agent by construction, and a
       proxy this agent cannot answer for is cross-origin, where the answer is a SecurityError. proxy_realm
       asserts the pairing. */
    WP_DOCUMENT,
    /* §7.2.2's `location`: the active document's Location object, the first thing a page does with a popup it
       just opened. Unlike `document` it is on §7.2.1's cross-origin list: the property is reachable across
       origins though almost every member of the object it returns is not, because `otherW.location.href = url` is
       how one document navigates another. Its cross-origin arm hands back core/frame/remote_location.h's filtered
       Location for a document another instance holds. */
    WP_LOCATION,
    WP_MEMBER_N
};
static const char *const PROXY_MEMBER[WP_MEMBER_N] = {
    "window", "self", "frames", "globalThis", "parent", "top", "opener", "closed", "name", "length",
    "document", "location"
};
/* §7.2.1's cross-origin property names, the fixed list a WindowProxy exposes whatever the origins: window, self,
   location, close, closed, focus, blur, frames, length, top, opener, parent, postMessage. Everything else is
   same-origin only, and reaching it across origins is a SecurityError.
   The flags are declared beside the members rather than checked at each read, so a member cannot be added
   without answering the question; a missing check would leak a cross-origin document with nothing to say so. */
static const bool PROXY_CROSS_ORIGIN[WP_MEMBER_N] = {
    true,  /* window     */ true,  /* self       */ true,  /* frames     */
    false, /* globalThis: the global object, and §7.2.1 does not list it */
    true,  /* parent     */ true,  /* top        */ true,  /* opener     */
    true,  /* closed     */
    false, /* name: a browsing context's name is not on the list; a cross-origin read of it is a SecurityError */
    true,  /* length     */
    false, /* document: §7.2.1 does not list it, so a cross-origin read is a SecurityError */
    true,  /* location: §7.2.1 lists it; the filtering is the Location object's own, not this table's */
};

/* §7.2.1: a read the origins do not permit is a SecurityError thrown at the read, not undefined: a page
   distinguishes the two, and undefined would say this window has no such member when the reader cannot see it. */
static bool proxy_read_permitted(const ProxyData *p, int magic)
{
    if (PROXY_CROSS_ORIGIN[magic])
        return true;   /* the fixed list is answered whatever the origins are, so nothing below is asked */
    return proxy_same_origin(p);
}

/* ---- §7.2.3.5's cross-origin branch: the half of the filter that has no member to hang off ---------------
 *
 * The table above answers a different question. `PROXY_CROSS_ORIGIN` is indexed by member magic and answers "may
 * this installed accessor be read across origins", consulted from inside the accessor, so it is only asked about
 * a name this component installed. Every other name (`alert`, anything a page probes for) reaches no accessor,
 * walks off the end of the prototype chain and answers `undefined`, the one answer §7.2.1.3.2 forbids: its last
 * step is "throw a SecurityError DOMException". A page distinguishes the two (`undefined` says the window has no
 * such member, a fact about the peer's document; the SecurityError says "you may not look"), and a
 * `try`/`catch` around the read is how a page feature-detects a cross-origin window.
 *
 * So the list is the standard's own and not the member table. §7.2.1.3.1 names thirteen cross-origin accessible
 * window property names, and four (`close`, `focus`, `blur`, `postMessage`) are not in the table above: `close`
 * is a method of this component, `postMessage` is window_message.c's, and `focus`/`blur` are core/html/focus.c's,
 * which owns §6.6.6's steps and installs them onto both §7.2.2's global and this surface from one list. An
 * assert at capture ties the two tables together. */
/* Every listed name is answered by an owner, and a listed name with none would fall off the prototype chain to
 * `undefined`, which CrossOriginPropertyFallback's last step rules out; so which surface owns what is a loop, in
 * proxy_get_own's steps 4-6, over this list. The list is asked once, at the object's own [[GetOwnProperty]],
 * because §7.2.3.5 puts it there, before the prototype walk, so a name outside it never reaches a member. */
/* Each entry is a record, not a name: §7.2.1.3.1 returns « { [[Property]]: "window", [[NeedsGetter]]: true,
 * [[NeedsSetter]]: false }, … », and the flags are half of what each entry says. A name-only list answers
 * §7.2.3.5's question (does this name reach a member) and cannot answer §7.2.1.1's (is this access to that name
 * permitted), which is decided by `type` against the flags: `location` is on the list with both flags, so its
 * getter and setter both go through, while Location's `href` has [[NeedsGetter]] false, so its setter goes
 * through and its getter is a SecurityError. The flags live on the one list rather than in a second. */
/* §7.2.1.3.1's Window arm verbatim, in the standard's order. An entry with neither flag is one the standard
   writes as `{ [[Property]]: "close" }`, an operation, which §7.2.1.1's "type is method and e has neither
   [[NeedsGetter]] nor [[NeedsSetter]]" tests for. */
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
/* §7.2.1.3.2 step 1's four names, the exception to the throw: `then` (so a cross-origin WindowProxy is not
   mistaken for a thenable and awaited) and the three well-known symbols an engine touches while doing something
   else. They answer `undefined` as a real property descriptor rather than by falling off the chain, because
   §7.2.3.10 lists them among the own keys. */
#define XO_FALLBACK_N 4
static JSAtom g_xo_fallback[XO_FALLBACK_N];
/* §7.2.1.3.4's getters for the entries above, one array per realm (see window_proxy.h), indexed by the
   CROSS_ORIGIN row so the standard's own list says which slot is which. JS_INVALID_CLASS_ID until
   window_proxy_init declares the slot. */
static JSClassID g_xo_getter_slot = JS_INVALID_CLASS_ID;

/* §7.2.1.3.1 CrossOriginProperties ( O )'s Window arm, asked by name: the one list answering one more caller.
 * Exported for the other end of the cross-instance seam. The asking half of a cross-document read can only emit
 * `PROXY_MEMBER[magic]`, an index into a fixed table; the performing half receives that member as text from
 * another instance, through a routing zone that does not read what it routes, and SECURITY.md makes every WASM
 * instance untrusted. So the name a peer performs `globalThis[name]` for is where an unlisted member would be
 * read across origins and relayed, and this entry is what it asks.
 * A lookup, not a predicate, because the caller needs the record: each entry carries [[NeedsGetter]] and
 * [[NeedsSetter]], and a `bool` would force the flags to be re-derived wherever the second question is asked.
 * NULL for a name the standard does not list, which the caller crashes on: a page gets §7.2.1.3.2's
 * SecurityError, a peer performing a record gets an abort, because a record naming an unlisted member was not
 * written by this engine's asking half. Side-effect-free over a static list, so it needs no realm. */
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

/* Captured once per agent, from this realm's %Symbol%, before any page script runs, as remote_object.c's
   well-known table is: `Symbol` is a global a page may replace, and a well-known symbol resolved after that
   names whatever the page put there. Atoms are the runtime's and an agent is one runtime, so one capture serves
   every realm. */
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
        /* A data property of %Symbol% (ECMAScript 6.1.5 makes every well-known symbol one, non-writable and
           non-configurable), so this read runs none of the page's code, which it must not: it runs from C with no
           flow base under it. */
        JSValue s = JS_GetPropertyStr(ctx, sym_ctor, FALLBACK_SYM[i]);
        CHECK(JS_IsSymbol(s), "window proxy: %Symbol% carries no well-known symbol under one of the three names "
                              "§7.2.1.3.2's fallback is defined over");
        g_xo_fallback[1 + i] = JS_ValueToAtom(ctx, s);
        JS_FreeValue(ctx, s);
        CHECK(g_xo_fallback[1 + i] != JS_ATOM_NULL, "window proxy: a well-known symbol could not be interned");
    }
    JS_FreeValue(ctx, sym_ctor);

    /* The two tables are one fact, checked against each other. A member marked cross-origin-readable whose name
       is not on §7.2.1.3.1's list would be permitted by its accessor and never reached, because [[GetOwnProperty]]
       throws before the prototype walk; a member marked same-origin-only whose name is on the list would be
       reached and then refuse. Both would be silent. */
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
        /* And the entry's [[NeedsGetter]] agrees with how this surface ships the member. Every member of the
           table above is a §7.2.2 attribute installed as an accessor, so a listed one with [[NeedsGetter]] false
           would be a name §7.2.3.5 steps 4-6 hand out and §7.2.1.1 "Integration with IDL" then refuses the
           getter of: a descriptor a page can read and an accessor it cannot call. Location's `href` is where
           that shape is real, because §7.2.4 "The Location interface" lists its `href` for the setter alone;
           nothing on this surface is. The loop above pairs a name against the list; this checks what the list
           says about the name. */
        DCHECKF(!listed || CROSS_ORIGIN[listed_at].needs_get,
                "HTML §7.2.1.3.1 CrossOriginProperties lists `%s` with [[NeedsGetter]] FALSE while §7.2.3 The "
                "WindowProxy exotic object's own surface installs it as an ACCESSOR — so §7.2.3.5 "
                "[[GetOwnProperty]] answers the descriptor for a cross-origin read and §7.2.1.1 Integration "
                "with IDL then throws a "
                "SecurityError out of the getter that descriptor names. Either the record is mistyped or "
                "the member is not an attribute of this surface", PROXY_MEMBER[i]);
    }
}

/* §7.2.1.3.4 CrossOriginGetOwnPropertyHelper ( O, P )'s getters for this realm (see window_proxy.h for what the
 * standard asks for and why an ordinary [[Get]] is not it).
 * It reads the Window's own slot, not the member: §7.2.1.3.4's step is `OrdinaryGetOwnProperty(O, P)`, which is
 * JS_GetOwnPropertyNoUserCode here, the internal method performed with no accessor invoked and no page code run,
 * the call §7.2.3.5 step 3 makes on a Window. A property get would run the getter at capture time, in a C
 * activation with no flow base under it.
 * Nine of the thirteen: the four entries with neither flag are §7.2.1.3.4's operation branch
 * (`{ [[Property]]: "close" }`), whose value is a callable rather than a getter, so their slot stays a hole and
 * window_proxy_cross_origin_getter refuses one by name.
 * The assert is the ordering contract between core/platform.c's rows: the eight `window`'s row installs and the
 * `location` that runs as a realm intrinsic before it must all be present, as accessors, now. A member installed
 * by a later row would be captured as absent, and a peer would answer it out of whatever the page left on its
 * global, so the loop crashes naming the member rather than skipping it. */
void window_proxy_install_window_getters(JSContext *ctx, JSValueConst global)
{
    JSValue getters;
    int i;

    DCHECK(g_xo_getter_slot != JS_INVALID_CLASS_ID,
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

        if (!CROSS_ORIGIN[i].needs_get) continue;   /* §7.2.1.3.4's operation branch: no getter exists */
        has = JS_GetOwnPropertyNoUserCode(ctx, &d, global, g_xo_atom[i]);
        /* A CHECK, not a DCHECK, because of this loop: `JS_GetOwnPropertyNoUserCode` fills the descriptor only on
           1 (its absent and throwing arms leave it untouched), so the three JS_FreeValues below dereference the
           record in release, and the guard must survive in that build. */
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

    /* The two refusals are the caller's to have made: remote_op.c asserts both where the record is born, the only
       place both of its parse's callers reach. They are restated as DCHECKs, not a second CHECK, since a second
       copy of one invariant drifts; what they add is the site, naming a caller that reaches this entry with an
       unlisted or operation-only name rather than dereferencing NULL two lines down. */
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
    /* An ordinary [[Get]] is right here, not the read the header argues against: the array is this component's
       own, minted at the capture and reachable from no script, so no page-owned slot lies between the index and
       the function. */
    getter = JS_GetPropertyUint32(ctx, table, (uint32_t)(e - CROSS_ORIGIN));
    JS_FreeValue(ctx, table);
    DCHECKF(JS_IsFunction(ctx, getter),
            "§7.2.1.3.4's captured getter for `%s` is not a function in this realm — the capture asserts every "
            "listed accessor was one, so a hole here is a table built for a DIFFERENT list than the one this "
            "lookup indexed into", name);
    return getter;
}

/* HTML §7.2.1.1 "Integration with IDL", whole: "When perform a security check is invoked, with a platformObject,
 * identifier, and type, run these steps". Its three steps are below in order.
 * It is the other half of §7.2.1. Everything else here filters a property lookup: §7.2.3.5 [[GetOwnProperty]]
 * decides what a cross-origin WindowProxy hands out, with CROSS_ORIGIN as its list. That is right for
 * `otherW.setTimeout`, which never reaches a member, and says nothing about `setTimeout.call(otherW, f)`, where
 * the function came from the reader's own window and the receiver was carried past the lookup on the call. Web
 * IDL puts the check inside the function for that reason: create an operation function's try-list performs it
 * before the brand TypeError and before §3.6 Overload resolution algorithm, so a cross-origin receiver is refused
 * however it was spelled.
 * Those spellings are ordinary here: `.call`, `.apply`, `Reflect.apply`, a bound callee and the spread are
 * call-site-resolved onto the ultimate target with the receiver intact, so each delivers a foreign `this` to the
 * member's body. `Object.getOwnPropertyDescriptor(self, "opener").get.call(popup)` is a WPT assertion this
 * engine runs, and it is how a bundle probes a frame it cannot read. */
/* Both halves are the assertion, which is why the list carries flags. A filter refusing every cross-origin
 * receiver would pass a test that only checks the throw and be wrong four times on the Window (the four
 * operations §7.2.1.3.1 lists with neither flag), twice on `location` (its getter and setter both go through) and
 * once on a Location (`href`'s setter goes through, its getter does not). Step 2 below is those flags.
 * `platform_object` is the raw `this` value, not Web IDL §3.7.7's resolved `jsValue`, and the two are equivalent: the
 * resolution only affects a missing receiver ("or realm's global object otherwise"), and that global is a Window
 * of this heap, which the Window arm answers same-origin unconditionally. Resolving would also cost, since
 * window_proxy_this_object answers owned and this runs on every declared member of the platform. */
/* Step 1's two arms are asked as three brands, the third absent on purpose (see remote_location.h). A Window (any
 * realm's global; §7.1.1 "Origins"' same origin is constant across an origin-keyed agent cluster) and a
 * WindowProxy (whose origin is per flow, read where §7.2.1.3.3 reads it) are the Window arm; a cross-origin
 * Location is the Location arm. A Location this agent holds takes step 1's return and step 3's pass to the same
 * place, so no observation separates them and there is no brand to ask.
 * Returns 0 where §7.2.1.1 returns, -1 with its "SecurityError" DOMException pending. */
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

    /* Step 1: "If platformObject is not a Window or Location object, then return." */
    if (window_proxy_is(platform_object)) {
        xo = CROSS_ORIGIN;  n = CROSS_ORIGIN_NAME_N;
        /* §7.2.1.3.3 IsPlatformObjectSameOrigin, read here rather than at step 3, because the read captures the
           proxy's record into the running flow's delta and the branch below must not decide whether that
           happens; the answer is per flow either way. */
        same_origin = window_proxy_same_origin_of(platform_object);
    } else if (window_is(platform_object)) {
        xo = CROSS_ORIGIN;  n = CROSS_ORIGIN_NAME_N;
        /* Every Window object in this heap is same origin with the asker, by SECURITY.md's keying: an instance is
           `(browsing context group, origin)`, so a foreign realm's global reached through a same-origin document
           is same origin by construction, and a cross-origin document's Window is not in this heap (it is a
           WindowProxy, taken above). */
        same_origin = true;
    } else if (remote_location_is(platform_object)) {
        /* §7.2.1.3.3 answers false for this object by construction: it exists only for a Document another
           instance holds (remote_location.h). */
        xo = LOCATION_CROSS_ORIGIN;  n = LOCATION_XO_N;
        same_origin = false;
    } else {
        return 0;
    }

    /* Step 2: "For each e of CrossOriginProperties(platformObject): if SameValue(e.[[Property]], identifier) is
       true", then its three sub-steps in the standard's order. SameValue over two strings is `strcmp` here:
       `identifier` is the member's IDL identifier, a static string the declaration owns. */
    for (i = 0; i < n; i++) {
        if (strcmp(xo[i].name, identifier) != 0) continue;
        switch (type) {
        /* 2.1.1: "If type is "method" and e has neither [[NeedsGetter]] nor [[NeedsSetter]], then return."
           Neither, not either: `location` is on the list with both flags and is an attribute, so an operation
           named "location" pulled from another interface and applied to a cross-origin WindowProxy is refused
           rather than let through on a name match. */
        case WP_SEC_METHOD: if (!xo[i].needs_get && !xo[i].needs_set) return 0; break;
        /* 2.1.2: "Otherwise, if type is "getter" and e.[[NeedsGetter]] is true, then return." */
        case WP_SEC_GETTER: if (xo[i].needs_get) return 0; break;
        /* 2.1.3: "Otherwise, if type is "setter" and e.[[NeedsSetter]] is true, then return." */
        case WP_SEC_SETTER: if (xo[i].needs_set) return 0; break;
        default:
            DFAILF("§7.2.1.1 Integration with IDL was invoked with a `type` Web IDL §3.5 Security does not "
                   "define — its three inputs are the platform object, the identifier, and one of \"method\", "
                   "\"getter\" and \"setter\", and a fourth kind of function object is a fourth sub-step to "
                   "write here rather than a value to fall past (%d)", (int)type);
        }
        /* The loop does not stop here. §7.2.1.3.1's lists have no duplicate [[Property]], so a match that did not
           return can only fall to step 3, but the standard writes `for each`, and a `break` would be this file
           deciding otherwise. */
    }

    /* Step 3: "If IsPlatformObjectSameOrigin(platformObject) is false, then throw a "SecurityError"
       DOMException." Not `undefined` and not a TypeError: a page tells all three apart, with
       `try { f.call(otherW) } catch (e) { e.name }`. */
    if (same_origin) return 0;
    JS_ThrowDOMException(ctx, "SecurityError",
                         "the origins do not permit `%s` to be invoked on that object", identifier);
    return -1;
}

/* ECMA-262 6.1.7's array index, spelled out rather than parsed: a canonical numeric string for an integer in
   [0, 2**32-1). "01", "1.0" and " 1" are ordinary property names, all of which strtoul would call indices. The
   index itself is the answer: §7.2.3.5 step 2.1 is `! ToUint32(P)`, the digits this loop has accumulated, and a
   second parse could disagree with this one on exactly the strings it rejects. */
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

/* HTML §7.2.3.5 step 2, the array-index branch, and the whole definition of indexed access: §7.2.2.2 says
 * "indexed access to document-tree child navigables is defined through the [[GetOwnProperty]] internal method of
 * the WindowProxy object", so `frames[0]`, `parent[1]`, `top[0]` and `otherW[0]` are one algorithm with one
 * implementation.
 * It runs before the same-origin question: step 2 precedes step 3's `IsPlatformObjectSameOrigin(W)`, and every
 * path through step 2 returns, so an array index never reaches step 3 (asking step 3 first would answer
 * `undefined` for `frames[0][0]` about a grandchild navigable that exists).
 * Out of range is two answers (step 2.4): `undefined` for a same-origin W says that window has no such frame,
 * and the SecurityError for a cross-origin one says the reader may not look; a page's `try`/`catch` tells them
 * apart. */
/* The list is the target document's, never the reading realm's, the list `length` answers by. Two facts decide
 * it without materializing anything:
 *   a destroyed navigable has no active document, so it has no children;
 *   a navigable whose realm is not materialized shows the initial about:blank Document §7.4 created it with, an
 *     element can only get into that document by script, and script cannot run in a realm that does not exist,
 *     so its child list is empty, computed rather than assumed. Building a realm to count frames in a document
 *     that provably has none would cost a realm per flow per read, which `length` refuses for the same reason.
 * The walk is html_iframe.c's, over the Lexbor tree and each container's per-flow navigable slot, with no page
 * code by construction, which lets this hook keep declaring get_own_property_no_user_code; window.c's own
 * exotic does the same for the global spelling of the same navigable. */
static int proxy_indexed_get_own(JSContext *ctx, JSPropertyDescriptor *desc, const ProxyData *p, uint32_t idx)
{
    JSValue child = JS_UNDEFINED;

    /* The index is a uint32 and the walk counts in int, so the range above INT32_MAX is out of range, not a
       negative index: no document has that many child navigables, but a wrapping cast would ask for one at some
       other position. */
    if (!wp_closed(p) && idx <= (uint32_t)INT32_MAX) {
        if (!world_doc_hosted(p->doc)) {
            /* §7.2.3.5 step 2.2 for a document in another instance. The answer edge exists: a navigable crosses
               as its identity (document name, origin, browsing-context name, the parent's and opener's document
               names) and resolves on the far side to that agent's own WindowProxy or its one remote proxy per
               document (remote_object.c's nav_encode/nav_decode, window_proxy_for_document above). Two edges do
               not.
               The peer-side program: `windowproxy.get` performs §7.2.1.3.4 CrossOriginGetOwnPropertyHelper's
               getter for one of §7.2.1.3.1 CrossOriginProperties ( O )'s thirteen members, selected in C from a
               per-realm capture, and remote_op.c's parse refuses any other member name, which a decimal index
               is. §7.2.1.3.1: "Indexed properties do not need to be safelisted in this algorithm, as they are
               handled directly by the WindowProxy object", and they are cross-origin accessible under the
               sentence ending "or an array index property name". So an index is a second operation on
               remote_op.c's grammar, its own verb whose program is §7.2.2.2 "Indexed access on the Window
               object"'s step in the peer's realm. */
            /* The suspend: this exotic [[GetOwnProperty]] is a C hook that must return a value, so it cannot park
               the flow the way `length` does (an IDL accessor driven as a step machine). The one read shape this
               interpreter resolves at the operator site and drives on the trampoline is the keyed entry's
               GP_GETOWNPROP (remote_object.c's block comment, and quickjs.c's DCHECK in tramp_walk_continues).
               Routed onto it, step 2 becomes a step machine whose ask stage posts (document, world+ancestry,
               index) as proxy_get_step does and whose answer stage is remote_object_decode of the identity that
               comes back, parked at `otherW[0]` and resumed byte-identical, as an await is. Answering `undefined`
               here would be wrong, and the SecurityError below is right only once the index is known to be out
               of range. */
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
        /* Step 2.5's descriptor exactly: [[Writable]] false, [[Enumerable]] true, [[Configurable]] true, the three
           §7.2.2.2's global spelling reports, because it is the same step. */
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

/* ---- §7.2.3 when §7.1.1's same origin holds: the operation is performed on W ------------------------------
 *
 * Each of §7.2.3's internal methods says the same for a same-origin W, and it is not "answer the same value": it
 * is `W.[[X]](…)`. So `frame.contentWindow.addEventListener` is the other document's Window's own member, and
 * `frame.contentWindow.onunload = f` runs the other document's handler setter and stores its handler map on the
 * other document's Window, which is what makes the handler fire, since that Window is what the event path walks.
 * A forward of the value would pass a round-trip test and leave the listener on a stand-in nothing dispatches to.
 *
 * §7.2.3's own surface is not forwarded, by this engine's Window/WindowProxy duality: `window` in a realm is that
 * realm's global here (win_or_proxy states the mapping once), so `otherW.parent` is answered by the reading
 * realm's member. Forwarded, it would run the other realm's `parent` getter, which maps its own navigable onto its
 * global and hands back a proxy the reading page has never seen, making `frame.contentWindow.parent === window`
 * false. The same holds for `self`, `top`, `frames`, `window` and `globalThis`. */
/* The carve-out is asked of the prototype, which is that surface. A list written out here would be a second copy
 * of window_proxy_install_proto's, and a name it fell behind (`postMessage` is window_message.c's; `focus` and
 * `blur` are focus_install_window_members') would be handed to the other document's Window while this object's
 * own member answered nothing. */
/* §7.2.3.1's [[GetPrototypeOf]] is built (proxy_get_prototype), and is not yet the whole of the chain.
 * Reflection asks it (`Object.getPrototypeOf(otherW)`, `Reflect.getPrototypeOf`, `otherW.__proto__`, 7.3.21
 * OrdinaryHasInstance's walk behind `instanceof`, 20.1.3.3's `isPrototypeOf`), so those answer W's own prototype
 * for a W the reader may see and null for one it may not.
 * The ordinary lookup walks still follow the shape link, this object's class prototype: 10.1.8.1 OrdinaryGet step
 * 3, 10.1.9.2 step 2.a and 10.1.7 step 3 each say `O.[[GetPrototypeOf]]()`, and quickjs's walks read the stored
 * link instead (JS_GetPrototype's comment states it from that side). The link must keep pointing where it does:
 * §7.2.3.5 steps 4-6 let a cross-origin read walk to this surface for §7.2.1.3.1's names, and routing those
 * walks through §7.2.3.1, which answers null there, would take `otherW.postMessage` away from every page. */
/* Named residual: the lookup walks do not ask §7.2.3.1. Not covered: a member of the other document's Window
 * reachable only up its prototype chain is not reached through a same-origin WindowProxy. Next diff builds:
 * window.c declaring §7.2.1.3.1's thirteen names as own properties of the global (a member of an interface
 * declared [Global] is an own property of every object implementing it, as Web IDL §3.7.7 Operations and §3.7.6
 * Attributes state for their member kinds, and §2.3 Interface mixins makes a mixin's members the including
 * interface's), after which the cross-origin surface needs no chain and the walks can ask §7.2.3.1. How its
 * absence shows: a same-origin `otherW.n` for a member the other Window inherits answers from this object's chain
 * rather than the other Window's. EventTarget's three operations are outside it: DOM §2.7 "Interface EventTarget"
 * is not declared [Global], so they are own properties of no global and are always reached up the chain
 * (core/events/event_target.c). */

/* §7.2.3's own surface for one name: an own property of this object, not a prototype hit. HTML §7.2.3 "The
 * WindowProxy exotic object": "There is no WindowProxy interface object." Every name a WindowProxy answers is
 * answered by §7.2.3.5's [[GetOwnProperty]] and listed by §7.2.3.10's [[OwnPropertyKeys]], so a member reachable
 * only up a prototype chain would be one `Object.getOwnPropertyDescriptor(w, n)` answers `undefined` for while
 * `w.n` answers a value, and one `Object.getOwnPropertyNames(w)` never mentions. The Window and this surface both
 * carry §7.2.1.3.1's thirteen names, which proxy_get_own's steps 4-6 assert.
 * The object is still the class proto, a second role rather than a second surface: §7.2.3.5's cross-origin branch
 * (steps 4-6) answers `0` for §7.2.1.3.1's names and lets the walk reach the same members, this engine's stand-in
 * for §7.2.1.3.4 CrossOriginGetOwnPropertyHelper's "anonymous built-in function, created in the current realm".
 * One object, two ways in, one answer.
 * The query runs no page code and no walk: own properties only, because Object.prototype's members are the ones
 * the forward should answer out of the other realm.
 * 1 = the surface answers it (*desc filled and owned when desc != NULL), 0 = it does not. */
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

/* W: the active document's Window that each of §7.2.3's internal methods is performed on, or JS_UNINITIALIZED when
   there is none: a cross-origin proxy (§7.2.1 filters its members instead), or a destroyed navigable, which has
   no active document. It takes no property name, because §7.2.3.10 asks the same question without one (the key
   list is W's whole own-key list).
   Borrowed: the proxy holds it, and its ProxyData rides the COW delta, so the answer is the running flow's: an
   arm that navigated the frame forwards to the Window it navigated to and its sibling to the one it did not. */
static JSValueConst proxy_target_window(JSContext *ctx, JSValueConst obj, ProxyData *p)
{
    if (!proxy_same_origin(p)) return JS_UNINITIALIZED;
    if (wp_closed(p)) return JS_UNINITIALIZED;
    /* Materialized here, including from the capture, on purpose: `delete otherW.x` captures at the head of the
       delete before anything walks, so a capture that refused to build would name this proxy for a delete that
       lands on the Window a moment later, an entry over storage the operation never touches. This is
       proxy_realm's own contract (a navigable still holding its initial about:blank is materialized when
       something reads through it), and a property operation on this object is such a read. It runs no page
       code: the initial about:blank Document has no script. */
    if (!p->realm)
        proxy_realm(ctx, obj, p);
    DCHECK(!JS_IsUndefined(p->window),
           "a same-origin WindowProxy whose realm this agent holds carries no Window — proxy_realm sets the "
           "two together, so one without the other is a binding something else assembled");
    return p->window;
}

/* The same W for a keyed operation, and JS_UNINITIALIZED also for a name §7.2.3's own surface answers, which is
   this object's own and not performed on W. */
static JSValueConst proxy_forward_window(JSContext *ctx, JSValueConst obj, ProxyData *p, JSAtom prop)
{
    if (!proxy_same_origin(p)) return JS_UNINITIALIZED;
    if (proxy_surface_desc(ctx, NULL, prop)) return JS_UNINITIALIZED;
    return proxy_target_window(ctx, obj, p);
}

/* Where a forwarded write lands, for the COW delta (see JSClassExoticMethods.forwarded_object). The delta must
   name the Window the define below writes, never this stand-in: an entry naming the proxy restores its baseline
   onto the proxy as a real own property, which then shadows the Window for every flow. */
/* The receiver of a class hook is not Web IDL §3.7.6's question, so this asserts rather than resolves. A member
 * installed on §7.2.3's surface is an attribute and can be handed any receiver a page can write (proxy_member_get
 * reads that), but a hook in JSClassExoticMethods is reached by quickjs off the object's own class id, so `obj`
 * is a WindowProxy by construction and its opaque was set by a mint. A defaulted `if (!p) return …` would answer
 * "no such property", success, refusal or an empty window, each indistinguishable from the real answer. The
 * finalizer and gc_mark read their record through proxy_collector_record instead of proxy_of, because they run
 * inside a collection. */
static JSValueConst proxy_forwarded_object(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    ProxyData *p = proxy_of(obj);

    DCHECK(p != NULL, "a WindowProxy class hook ran on an object carrying no navigable record");
    return proxy_forward_window(ctx, obj, p, prop);
}

/* HTML §7.2.3.1 [[GetPrototypeOf]] ( ): "Let W be the value of the [[Window]] internal slot of this. If
   IsPlatformObjectSameOrigin(W) is true, then return ! OrdinaryGetPrototypeOf(W). Return null."
   W is proxy_target_window's: HTML §7.2.1.3.3 IsPlatformObjectSameOrigin(O) is "the current settings object's origin
   is same origin-domain with O's relevant settings object's origin", and every internal method here asks it
   through the one proxy_same_origin, so an answer that is wrong is wrong for all of them at once and fixed once.
   A navigable with no active document (destroyed, or discarded by a close) has no W to perform
   OrdinaryGetPrototypeOf on, so the answer is step 3's null, the statement [[Delete]] reads as "nothing here to
   remove" and [[GetOwnProperty]] as "the surface answers".
   No page code: OrdinaryGetPrototypeOf is 10.1.1, a slot read, and proxy_target_window's materialization builds
   the initial about:blank Document, which has no script. That is the contract JSClassExoticMethods.get_prototype
   is declared under, as this class's [[GetOwnProperty]] is. */
static JSValue proxy_get_prototype(JSContext *ctx, JSValueConst obj)
{
    ProxyData *p = proxy_of(obj);
    JSValueConst w;

    DCHECK(p != NULL, "a WindowProxy class hook ran on an object carrying no navigable record");
    w = proxy_target_window(ctx, obj, p);                       /* step 1 */
    if (JS_IsUninitialized(w))
        return JS_NULL;                                         /* step 3 */
    /* Step 2. W is a Window, an ordinary object, so this is 10.1.1 OrdinaryGetPrototypeOf and not a second trip
       through an exotic hook. Asserted: a W with an exotic [[GetPrototypeOf]] would make this recursive, and this
       is the one class in this engine that computes a prototype. */
    DCHECK(!window_proxy_is(w),
           "§7.2.3.1's W is a WindowProxy — [[Window]] holds the Window a navigable's active document IS, and a "
           "proxy there would make OrdinaryGetPrototypeOf recurse through this hook");
    return JS_GetPrototype(ctx, w);
}

/* HTML §7.2.3.6 [[DefineOwnProperty]], the write half of the branch above and how a page writes to a same-origin
   frame's Window; without it an assignment would create an own property on this object, so the read would
   forward and the write would not. Its cross-origin arm is step 3's throw. */
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
    if (proxy_atom_index(ctx, prop, &idx)) {                            /* step 2.1: false for every index */
        /* HTML §7.2.3.6 step 2.1 is "If P is an array index property name, return false.", one word the callers
           turn into four observables, so it is spoken once to the engine rather than decoded here:
           Object.defineProperty throws (ECMAScript §7.3.8 DefinePropertyOrThrow step 2, "If success is false,
           throw a TypeError exception"), Reflect.defineProperty returns the boolean (ECMAScript §28.1.3 step 4),
           and a strict assignment arrives with JS_PROP_THROW_STRICT, for which ECMAScript §6.2.5.6 PutValue step
           3.e ("If succeeded is false and refRecord.[[Strict]] is true, throw a TypeError exception") throws. */
        /* Named residual: §7.2.3.8's own refusal is not built, and this hook is where its absence is paid. HTML
           §7.2.3.8 [[Set]] ( P , V , Receiver ) step 3.1 refuses an index one internal method earlier ("If P is an
           array index property name, then return false"), before OrdinarySet, so a browser consults nothing else.
           This class declares no `set_property`, so an assignment takes the ordinary route and arrives here by two
           roads: in range, §7.2.3.5's non-writable descriptor stops it at ECMAScript §10.1.9.2
           OrdinarySetWithOwnDescriptor step 2.a; out of range, §7.2.3.5 returns undefined for a same-origin W,
           proxy_get_own reports no own property, and step 2.d.ii's CreateDataProperty lands here. Both end in
           `false`. Not covered: the ordinary route walks the prototype chain before either road, and §7.2.3.8
           step 3.1 never does. Next diff builds: a `set_property` entry in PROXY_EXOTIC answering §7.2.3.8 (step
           2's access report, step 3.1's refusal, step 3.2's OrdinarySet on W, step 4's CrossOriginSet). How its
           absence shows: an index-named setter defined on Object.prototype runs when that index is assigned on a
           WindowProxy, a side effect no browser produces. */
        return JS_RefuseOrThrowTypeError(ctx, flags,
                                         "cannot define an indexed property on a WindowProxy");
    }
    w = proxy_forward_window(ctx, obj, p, prop);
    /* §7.2.3's own surface is this object's, so a define of one of those names is the ordinary define on this
       object with the exotic step suppressed, the re-entry win_define_own makes, since an exotic hook replaces
       the ordinary path rather than preceding it. */
    if (JS_IsUninitialized(w))
        return JS_DefineProperty(ctx, obj, prop, val, getter, setter, flags | JS_PROP_NO_EXOTIC);
    return JS_DefineProperty(ctx, w, prop, val, getter, setter, flags);   /* step 2.2 */
}

/* HTML §7.2.3.9 [[Delete]], the same three arms. Reached only after the ordinary own-property scan found nothing,
   which on this object is always, unless a page has replaced one of §7.2.3's own surface names through the define
   above. */
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
        /* A destroyed navigable and a name HTML §7.2.3's own surface does not answer: there is no active document to
           perform §7.2.3.9 step 2.2 on and nothing here to remove, so `true` is truthful. */
        if (!proxy_surface_desc(ctx, NULL, prop)) return 1;
        /* §7.2.3's own surface is an own property (see proxy_surface_desc), so `true` here would leave
           [[GetOwnProperty]] reporting a configurable own property [[Delete]] claimed to remove, and the surface
           object is shared by every proxy in the realm, so no per-proxy entry can be taken out. What closes it is
           what closes §7.2.3.6's local define: route the operation to W (which owns each of these names;
           proxy_own_keys asserts it) and let W's descriptor be what §7.2.3.5 step 3 answers, which needs the
           reading realm's Window/WindowProxy identity mapping applied to the forwarded value (win_or_proxy read
           backwards for a value produced in another realm). */
        DFAIL("`delete` of one of §7.2.3's own surface names on a WindowProxy. The surface answers the name as "
              "an OWN, configurable property and is shared by every proxy in the realm, so nothing here can "
              "remove it and reporting success would leave [[Delete]] and [[GetOwnProperty]] disagreeing. "
              "Build §7.2.3.9 step 2.2's forward: perform the delete on W, and give §7.2.3.5 step 3 the "
              "identity mapping that lets W's own descriptor answer these names in the READING realm");
        return 1;
    }
    return JS_DeleteProperty(ctx, w, prop, 0);                          /* step 2.2 */
}

/* HTML §7.2.3.5 [[GetOwnProperty]]: step 2 for every W, step 3 for a same-origin one, steps 4 through 6 for a
 * cross-origin one. Step 3 is the forward described above, asked before §7.2.1.3.1's list because the standard
 * puts it there: the list belongs to steps 4-6, which a same-origin W never reaches. */
static int proxy_get_own(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom prop)
{
    ProxyData *p = proxy_of(obj);   /* captured: the origin this read is filtered by is per-flow state */
    JSValueConst w;
    uint32_t idx;
    int i;

    DCHECK(p != NULL, "a WindowProxy class hook ran on an object carrying no navigable record");
    /* A name table never filled compares every property against JS_ATOM_NULL and matches none, refusing
       §7.2.1.3.1's whole list, and a filter that throws for everything passes any test that only checks the
       throw. */
    DCHECK(g_xo_atom[0] != JS_ATOM_NULL && g_xo_fallback[0] != JS_ATOM_NULL,
           "a WindowProxy read reached §7.2.3.5 before this agent interned the property names "
           "§7.2.1.3.1 \"CrossOriginProperties ( O )\" lists — the class was registered without "
           "proxy_capture_names running beside it");

    /* Step 2, first because the standard puts it first. Every path through it returns, so an array index reaches
       neither the cross-origin name list nor step 3, and asking it here makes that ordering a fact of the code
       rather than a claim that the thirteen names are non-numeric. */
    if (proxy_atom_index(ctx, prop, &idx))
        return proxy_indexed_get_own(ctx, desc, p, idx);

    /* Step 3: `OrdinaryGetOwnProperty(W, P)`, performed on the other document's Window. */
    w = proxy_forward_window(ctx, obj, p, prop);
    if (!JS_IsUninitialized(w))
        return JS_GetOwnPropertyNoUserCode(ctx, desc, w, prop);
    /* Same origin and not forwarded means §7.2.3's own surface answers the name (or the navigable is destroyed,
       whose readable members are answered out of this record). It is this object's own property: §7.2.3 has no
       interface object, so §7.2.3.5 answers each of these names and §7.2.3.10 lists each, and a prototype-walk
       answer would make `Object.getOwnPropertyDescriptor(popup, "opener")` undefined while `popup.opener` is the
       opener. */
    if (proxy_same_origin(p)) return proxy_surface_desc(ctx, desc, prop);

    /* Steps 4-6, which only a cross-origin W reaches, with the permit side of the filter checked in the same pass
       as the refuse side.
       Returning `0` says "not an own property of this exotic object", letting the ordinary walk reach §7.2.3's
       surface (this object's class prototype) and answer out of it. That is an answer only while the surface
       owns the name: a listed name it does not own walks off the chain to `undefined`, the value §7.2.1.3.2
       CrossOriginPropertyFallback forbids, indistinguishable from a member the peer's document lacks.
       So it is one loop, not two checks: a filter that throws for every unlisted name passes any test that only
       looks for the throw. proxy_capture_names' reconciliation iterates the member table, so it cannot see a
       §7.2.1.3.1 name no member of this component declares; this loop visits the standard's list, so a name is
       refused unless listed and answerable because listed. Side-effect-free: proxy_surface_desc with a NULL
       descriptor reads one own property of an ordinary object and frees what it took. */
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
            /* §7.2.1.3.2 step 1's descriptor exactly: undefined, non-writable, non-enumerable, configurable. */
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

/* §7.2.3's own surface as a key list: the answer for a navigable with no active document to walk. */
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

/* The one-surface invariant, asserted where the two surfaces are reconciled. §7.2.3.10's key list below is W's
   own key list alone, while §7.2.3.5 answers §7.2.3's own surface names out of the reading realm's surface
   object. They are one fact only while every name the surface answers is also an own property of W: a surface
   name W does not own is a property this object reports and never lists. Side-effect-free: it reads two key
   lists and frees them. */
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

/* The index prefix is the range §7.2.3.5 step 2 walks, the other half of the same one-list rule over the same
   document's child navigables. A gap or a duplicate is a frame this object lists twice or not at all, against a
   [[GetOwnProperty]] that answers for each index in the range exactly once. Side-effect-free: it re-reads atoms
   it was handed. */
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

/* HTML §7.2.3.10 [[OwnPropertyKeys]] ( ), the half that makes §7.2.3.5's answers checkable: an object that
 * answers [[GetOwnProperty]] for a name it never lists has `Object.getOwnPropertyNames` and
 * `Object.getOwnPropertyDescriptor` disagreeing, which a page reads directly
 * (`assert_own_property(popup, "opener")`).
 * The list has one source. §7.2.3.10's step 4 is "the concatenation of keys and OrdinaryOwnPropertyKeys(W)" (step
 * 5 is its cross-origin twin), and steps 2-3's `keys` is the range 0 to W's associated Document's document-tree
 * child navigables's size. This engine's Window answers that range: §7.2.2.2 "Indexed access on the Window
 * object" says "indexed access to document-tree child navigables is defined through the [[GetOwnProperty]]
 * internal method of the WindowProxy object", and window.c answers the same range for the global spelling out
 * of the same walk (win_own_names). So the range is already in the list below, and re-deriving it would disagree
 * for a document whose frame count changed between the two walks.
 * What the merge does not give is the order: quickjs appends a class's exotic keys after the object's ordinary
 * ones, while §7.2.3.10 puts the range first, as does ECMA-262 10.1.11.1 OrdinaryOwnPropertyKeys (step 2 lists
 * every integer index in ascending numeric order before any string key). A stable partition plus an ascending
 * sort of the prefix is the whole of it. */
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
        /* §7.2.3.10 step 5, and the one thing it needs that does not exist: step 2's maxProperties, the
           child-navigable count of an active document this instance does not hold, the cross-instance read
           proxy_indexed_get_own names. §7.2.1.3.7 CrossOriginOwnPropertyKeys's list is §7.2.1.3.1's thirteen
           names, all of which this surface owns (asserted per name in proxy_get_own's steps 4-6). An exotic
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
        /* A destroyed navigable has no active document, so steps 2-4 have nothing to walk; what it still owns is
           what §7.2.3.5 still answers out of this record, §7.2.3's own surface. */
        return proxy_surface_keys(ctx, ptab, plen);

    if (JS_GetOwnPropertyNames(ctx, &tab, &len, w, JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) < 0)
        return -1;
    DCHECK(proxy_surface_within(ctx, tab, len),
           "a name §7.2.3's own surface answers is NOT an own property of the Window this proxy forwards to. "
           "The key list is W's and the descriptors are the surface's, so that name is a property this object "
           "reports to `Object.getOwnPropertyDescriptor` and never lists in `Object.getOwnPropertyNames` — one "
           "navigable with two member surfaces, which is the shape §7.2.1.3.1's list and this component's "
           "member table are already tied together by an assert to prevent");

    /* Steps 2-3's `keys` first: a stable partition of the array-index property names to the front. */
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
    /* Ascending, which the partition does not give alone: the shape's own numeric keys and the Window exotic's
       range are two ascending runs laid end to end. An insertion sort over the prefix, whose length j is the
       document's frame count, so this walks the frames and not the whole surface. */
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
    /* And the range is the one §7.2.3.5 step 2 walks: two lists over one document's child navigables, which is why
       this hook takes W's list rather than building its own; a key `Object.getOwnPropertyNames` reports and
       [[GetOwnProperty]] refuses, or the reverse, is the two-answer defect one level down. */
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
   The one of §7.2.3's set needing neither the forward nor the filter: every other hook reaches the Window behind
   this proxy (§7.2.3.5 and §7.2.3.6 through their same-origin arms, §7.2.3.10 through the child-navigable walk),
   and this one answers out of §7.2.3 itself, for every origin, because a WindowProxy is never the object whose
   extensibility a page gets to fix. Its twin, §7.2.3.3 [[IsExtensible]] ( ) ("Return true."), is answered by
   quickjs from the `extensible` flag, which stays true because this refusal is asked before that flag is
   cleared; a successful freeze would make `Object.isExtensible(frames[0])` false. It is the only entry in the
   table below that never resolves the [[Window]] slot, so it has nothing to assert and nothing that could reach
   another agent. */
static int proxy_prevent_extensions(JSContext *ctx, JSValueConst obj)
{
    (void)ctx; (void)obj;
    return 0;
}

/* The six of §7.2.3's internal methods this object answers itself (§7.2.3.1 [[GetPrototypeOf]], §7.2.3.4
   [[PreventExtensions]], §7.2.3.5 [[GetOwnProperty]], §7.2.3.6 [[DefineOwnProperty]], §7.2.3.9 [[Delete]] and
   §7.2.3.10 [[OwnPropertyKeys]]), with §7.2.3.7's and §7.2.3.8's same-origin `W.[[X]]` reached through the
   forward, and one declaration about all of them: no page code, so the engine's own accessor walks may run them
   from C with no flow base. That covers the forward too: the Window it forwards to declares the same of its own
   hook (window.c's WINDOW_EXOTIC, whose named access is a walk of the document tree), and materializing an
   initial about:blank Document runs no script because there is none. §7.2.3.1's is a slot read (10.1.1
   OrdinaryGetPrototypeOf), §7.2.3.4's a constant. */
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

/* §7.2.2.4's `top`: the top-level traversable's proxy, walked rather than stored, because the parent chain is the
   only place the answer lives and a cached one goes stale when a frame is reparented. */
JSValue window_proxy_top_navigable(JSContext *ctx, JSValueConst self)
{
    JSValueConst cur = self;

    for (;;) {
        ProxyData *q = JS_GetOpaque(cur, g_proxy_class);
        if (!q || JS_IsUndefined(q->parent)) return JS_DupValue(ctx, cur);
        /* The chain is proxies all the way up, because §7.2.2.4 says `parent` is one, so the walk never reads
           `top` off a Window (a scriptable property read from a C activation, which this interpreter refuses). */
        DCHECK(window_proxy_is(q->parent),
               "a navigable's parent is not a WindowProxy — §7.2.2.4 says it is one, and a walk that has to ask "
               "what kind of object it reached is a walk that will read a scriptable property to continue");
        cur = q->parent;
    }
}

/* The scriptable `top` and the top navigable are not the same answer. `window.top` of the asking realm's own
   navigable is that realm's global (win_or_proxy below), because `window === window.top` is an identity every
   page rests on; the tree walk HTML §8.1.7.3 step 2 makes wants the top-level traversable's navigable, a
   WindowProxy in every case including that one. A walk given the scriptable answer would reach a Window, find it
   is not a proxy, and collect no documents. */

/* The asking realm stands for itself as its global: the one mapping between the two spellings of a window, used
   by every member that can answer with a navigable. In the spec `window`, `self`, `frames`, `parent` and `top` of
   a top-level navigable are all one object, its WindowProxy. Here the global is what a page holds as `window`, so
   answering with the asking realm's own proxy would make `window === window.parent` false at top level and
   `frame.contentWindow.parent === window` false in the corpus's iframe probe. Another navigable is its proxy;
   this one is the global. Stated once here, because a copy on one side only would answer with a proxy the page
   has never seen. */
static JSValue win_or_proxy(JSContext *ctx, JSValue v)
{
    /* Only an object can name a navigable, and this mapping is asked about values that are not one: §7.2.2.4's
       `opener` is null for a navigable nothing opened, and `parent`/`top` are null for a destroyed one. A JSValue
       carrying no pointer has none to read (JS_MKVAL initialises the union's `int32` and leaves the rest
       unspecified), so comparing one against this realm's proxy would compare whatever that union holds. */
    if (JS_IsObject(v) && JS_VALUE_GET_PTR(v) == JS_VALUE_GET_PTR(document_window_proxy(ctx))) {
        JS_FreeValue(ctx, v);
        return JS_GetGlobalObject(ctx);
    }
    return v;
}

/* HTML §7.2.2 "The Window object": "A Window's navigable is the navigable whose active document is the Window's
 * associated Document's, or null if there is no such navigable." §7.2.2.4 opens `top`, `parent` and
 * `frameElement` with a null test over it, so it is one question three members ask, asked in one place.
 * It becomes true at two times, and pages read the earlier one. §7.5.10 "Destroying documents" step 7 ("set
 * document's node navigable's active session history entry's document state's document to null") makes the
 * reverse lookup fail, and §7.5.10's descendant form performs its steps in parallel and queues a global task per
 * document, so `destroyed` is true only after a job has run. §7.3.1.6 "Navigable destruction"'s
 * destroy-a-child-navigable step 3, "set container's content navigable to null", is synchronous, inside the tree
 * mutation (core/html/html_iframe.c clears the element's slot on the removing steps' own line), and §7.2.2.4's
 * closing example reads it on the next line:
 *     element.remove();
 *     console.assert(iframeWindow.top === null);
 * as html/browsers does twice (window-top-null.html, window-parent-null.html). Asking `destroyed` alone would
 * keep answering the window that used to be above it until a task ran. */
/* So it is a walk: removing one `<iframe>` severs one link, and every navigable below keeps its own container
 * element, since the inner document is untouched. A grandchild is detached by an ancestor's sever and nothing of
 * its own (window-top-null.html's second subtest, `frame2Window.top` after the outer frame is removed).
 * §7.3.1.3's container already answers per flow, so nothing new is recorded here: window_proxy_container confirms
 * the stored container against that element's own content navigable, reporting the running flow's severance,
 * and keeps reporting it after the element is appended back, because §4.8.5's post-connection steps create a new
 * navigable that the element's slot then names. The corpus asserts both halves.
 * A container in another instance is not an absent one. SECURITY.md keys an instance on `(browsing context
 * group, origin)`, so the navigable an instance is rooted in is often a cross-origin `<iframe>` whose element
 * belongs to the creating instance; its §7.3.1.3 link crosses as an answer (window_proxy_remote_container) and
 * its local `container` is null. Reading that null as a sever would report every cross-origin frame's own
 * document as detached from itself, so the walk stops at that link: what is above it is the peer's tree. */
/* Runs no page code: proxy_of is a capture and window_proxy_container is one own-slot read of a wrapper, so this
 * can be asked from a plain C getter with no flow base. §7.2.2.4's `frameElement` asks it about the node
 * navigable, the Window's associated Document's, which is the same navigable and sentence; core/frame/window.c
 * reads it through window_proxy.h's declaration. */
bool window_proxy_navigable_null(JSContext *ctx, JSValueConst proxy)
{
    JSValueConst cur = proxy;

    for (;;) {
        ProxyData *q = proxy_of(cur);   /* captured: severance is per-flow state this read may precede */
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
        if (JS_IsNull(container)) return true;   /* §7.3.1.6 step 3 ran, in this flow */
        JS_FreeValue(ctx, container);
        /* Borrowed, as window_proxy_top_navigable borrows: each link is held by the record of the navigable below
           it, and that record by the object this iteration stands on. */
        cur = q->parent;
    }
}

/* §7.2.3's own surface answering one member, with Web IDL §3.7.6's receiver rather than this object. A receiver it
   cannot map would answer `undefined` where §7.2.2.4 answers `null`, and a page reads the difference
   (`window.opener === null` is how a document asks whether it is a popup). The reach is ordinary: §7.2.3.5 step
   3 hands `Object.getOwnPropertyDescriptor(w, "opener")` an accessor, so `desc.get()` is a call with no
   receiver, which §3.7.6 resolves to the getter's own realm's global. */
static JSValue proxy_member_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValueConst nav = window_proxy_this_navigable(ctx, this_val);
    ProxyData *p;

    DCHECK(magic >= 0 && magic < WP_MEMBER_N,
           "a WindowProxy member was declared with a magic this component does not name");
    if (JS_IsUninitialized(nav)) return JS_EXCEPTION;   /* §3.7.6's TypeError, already thrown */
    p = proxy_of(nav);   /* captured: `closed` and `name` are per-flow state a read may precede */
    DCHECK(p != NULL,
           "§3.7.6's receiver resolved to something that is not a WindowProxy — window_proxy_this_navigable "
           "answers with a navigable or throws, so a third outcome is a mapping that grew an arm without "
           "telling this member what it may now be handed");

    /* A local proxy can still be cross-origin (this agent is one origin, but a proxy it holds may name a navigable
       it navigated elsewhere), so §7.2.1's check comes first either way. */
    if (!proxy_read_permitted(p, magic))
        return JS_ThrowDOMException(ctx, "SecurityError",
                                    "the origins do not permit reading this member of that Window");

    /* One answer for every proxy, local or remote: every member below is the navigable's own state, and a
       navigable belongs to the agent that created it, this one, whether or not its active document does.
       Delegating to the target Window's property of the same name would be wrong twice: `otherW.self` would
       answer the other realm's global where the spec says its WindowProxy, and `closed`/`length` are IDL
       accessors, so reading them would reach a getter from a C activation with no flow base. */
    switch (magic) {
    case WP_WINDOW: case WP_SELF: case WP_FRAMES: case WP_GLOBALTHIS:
        /* The navigable, mapped, not the raw receiver: §7.2.3.7 makes `w.window` the WindowProxy, and for the
           asking realm's own navigable the object a page holds as `window` is the global (win_or_proxy). §3.7.6's
           missing receiver resolves to this realm's navigable, and handing that back unmapped would make
           `windowGet() === window` false for the one realm that can ask it. */
        return win_or_proxy(ctx, JS_DupValue(ctx, nav));
    case WP_PARENT:
        /* HTML §7.2.2.4 "Accessing related windows"' parent getter steps: "Let navigable be this's navigable. If
           navigable is null, then return null. If navigable's parent is not null, then set navigable to
           navigable's parent. Return navigable's active WindowProxy." Step 2 is the null test WP_TOP below
           explains. */
        if (window_proxy_navigable_null(ctx, nav)) return JS_NULL;
        return win_or_proxy(ctx, JS_IsUndefined(p->parent) ? JS_DupValue(ctx, nav)
                                                           : JS_DupValue(ctx, p->parent));
    case WP_TOP:
        /* §7.2.2.4's top getter steps: "If this's navigable is null, then return null. Return this's navigable's
         * top-level traversable's active WindowProxy." Both steps: §7.2.2.4 closes with a worked example whose
         * three asserts are `iframeWindow.top === null`, `iframeWindow.parent === null` and
         * `iframeWindow.frameElement === null` after the container element is removed, and the corpus has it
         * twice (window-top-null.html, window-parent-null.html).
         * A null navigable is neither a closed window nor a null browsing context: §7.2.2.4
         * spells three of its four getters' step 1 three ways, and this record keeps the three facts apart (see
         * window_proxy.h). The navigable's spelling is window_proxy_navigable_null, a walk that goes true
         * synchronously at §7.3.1.6's sever; `closing` is not it (a closing traversable still has its
         * documents), and neither is §7.1.3.2's group swap (`bc_discarded`), which discards the browsing context
         * (the question the `opener` getter asks and `closed` reports) while the Document stays active in this
         * record's navigable.
         * Asked before the walk, not inside it: window_proxy_top_navigable climbs `parent` links, which §7.3.1.6
         * does not sever, so a detached navigable would walk to a live traversable and answer with a window no
         * longer above it. */
        if (window_proxy_navigable_null(ctx, nav)) return JS_NULL;
        return win_or_proxy(ctx, window_proxy_top_navigable(ctx, nav));
    case WP_OPENER:
        /* §7.2.2.4's opener getter steps: "Let current be this's browsing context. If current is null, then return
           null. If current's opener browsing context is null, then return null. Return current's opener browsing
           context's WindowProxy object." Steps 1-2 ask the browsing context, not the navigable, the one getter of
           the four that asks the other question, answered by window_proxy_browsing_context_null.
           `embedded-opener-remove-frame.html` shows the difference in time: it reads a removed frame's `opener`
           on the next line and a closed popup's behind a `step_timeout`, because §7.3.1.6's sever is synchronous
           and §7.2.2.1's close is a task. */
        if (window_proxy_browsing_context_null(ctx, nav)) return JS_NULL;
        return win_or_proxy(ctx, JS_DupValue(ctx, p->opener));
    case WP_NAME:
        return window_proxy_name_value(ctx, nav);
    case WP_DOCUMENT:
        /* §7.2.2's `document`. Reaching it means the read was permitted, which for a same-origin-only member means
           the origins match, which in an origin-keyed agent means this agent holds the realm, so the answer is
           that realm's own Document object, in this turn: two same-origin documents are one heap and the corpus
           appends nodes across this edge. A destroyed navigable has no active document. */
        if (wp_closed(p)) return JS_NULL;
        return JS_DupValue(ctx, document_object(proxy_realm(ctx, nav, p)));
    case WP_LOCATION:
        /* §7.2.4: a navigable has one Location, the active document's, so it is read off that document's realm as
           `document` is. A destroyed navigable has no active document and no Location; the spec files read
           `closed` before touching one.
           The cross-origin half is a different object, and a component rather than a branch. §7.2.1.3.1
           CrossOriginProperties ( O ) lists `location` so a cross-origin document can be navigated through it,
           and what §7.2.4.5 through §7.2.4.10 hand across is a Location filtered to `href`'s setter and `replace`
           over a Document another instance holds (core/frame/remote_location.h). Handing back this document's
           Location would be a cross-origin read that silently succeeded, the failure §7.2.1 "Security
           infrastructure for Window, WindowProxy, and Location objects" exists to prevent, so the two answers
           are told apart here. */
        if (wp_closed(p)) return JS_NULL;
        if (window_proxy_is_remote(nav)) {
            /* HTML §7.2.1.3.3 IsPlatformObjectSameOrigin and this agent holding the active document are one
               condition, which makes this the standard's test: SECURITY.md keys an instance on `(browsing context
               group, origin)`, so a Document this agent hosts is same origin with every realm that could ask. The one
               shape that breaks the pairing is a same-origin document in another browsing context group, a
               separate instance by that key, for which §7.2.4's arms take the ordinary branch over a Document
               this heap lacks; the filtered object would refuse reads the origins permit, so it is asserted. */
            DCHECK(!proxy_same_origin(p),
                   "§7.2.2's `location` was read on a navigable whose active document is SAME ORIGIN and in "
                   "ANOTHER INSTANCE — §7.2.1.3.3 answers TRUE for it, so §7.2.4 wants that document's "
                   "ORDINARY Location, every member of which reads state this heap does not hold. That is a "
                   "cross-instance read of §7.2.4's members, suspended and answered by the peer exactly as "
                   "proxy_get_step answers `length`; the filtered object below is §7.2.4.5 [[GetOwnProperty]]'s "
                   "CROSS-origin arm and would refuse reads the origins permit");
            return remote_location_of_document(ctx, p->doc);
        }
        /* Read off the realm, not its global: §7.2.2 declares `location` as `[LegacyUnforgeable] readonly
           attribute Location`, a non-configurable accessor on the global, and a JS_GetPropertyStr that reaches a
           getter aborts, because a C activation has no flow base under it. location.h's location_object is the
           same answer without the property read, as document_object is above. */
        return location_object(proxy_realm(ctx, nav, p));
    default:
        DFAIL("a WindowProxy member with no navigable-own answer reached this switch — WP_LENGTH, WP_CLOSED "
              "and anything added beside them are answered by the instance holding the navigable's document "
              "and are declared on the step machine below");
        return JS_UNDEFINED;
    }
}

/* §7.2.2.4's `parent`, `top` and `opener` are the navigable's, and these are how a Window answers them. A Window
   and its WindowProxy are two spellings of one navigable, so the two are one answer from one record, as `closed`
   is. */
JSValue window_proxy_parent(JSContext *ctx, JSValueConst proxy)
{
    return proxy_member_get(ctx, proxy, WP_PARENT);
}

/* The navigable this one is nested in, for an engine walk: the distinction window_proxy_top_navigable draws, for
   the same reason. §7.2.2.4's `parent` answers this proxy when there is no parent, because `window.parent ===
   window` is an identity a top-level page rests on; a walk up the navigable tree wants "nothing above this", and
   would otherwise report a top-level navigable as its own container and loop, or deliver §7.5.10's
   child-destroyed report back to its sender. JS_UNDEFINED at the top, an auxiliary navigable included, which is
   not a WindowProxy; that is the caller's test. */
JSValue window_proxy_parent_navigable(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);
    DCHECK(p != NULL, "the containing navigable of something that is not a WindowProxy was asked for");
    return JS_DupValue(ctx, p->parent);
}

/* §7.3.1.3's create-a-new-child-navigable step "Set element's content navigable to navigable", from the
   navigable's side. A creation step, not a mutable property: §7.3.1.6 severs the relation by clearing the
   element's slot, never by rewriting this one, so a second write would be a navigable changing which element
   presents it, which no algorithm in §7.3 does. Asserted. */
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

/* The same link for a navigable whose container element is in another WASM instance (see the field). Stated after
   the mint for §7.3.1.3's own order ("Let navigable be a new navigable … Set element's content navigable to
   navigable"), the order window_proxy_set_container is called in; a host rooting an instance stands at the same
   point, one boundary away. Written once, like the element it stands for: a navigable never changes which
   element presents it. */
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

/* HTML §3.1.3 "Ancestor origins"' list for a navigable whose ancestors are in another WASM instance (see the
   field). Stated at the same moment and by the same zone as the remote container, because both are answers the
   creating instance computed from a tree only it holds, and §7.3.1.3's order puts both after the mint. Written
   once: §3.1.3's list is a snapshot of the tree the Document was created in, so a second statement would
   describe a second creation. */
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

/* §7.3.1.3's container of a navigable, as the standard defines it: "the navigable container whose content
   navigable is navigable, or null if there is no such element". The recorded element is the candidate and the
   forward slot is the definition, so the element is asked every time. That is the per-flow answer: the forward
   slot is an ordinary own property on the wrapper, so §7.3.1.6's clear rides the running flow's heap delta, and
   an arm that removed the `<iframe>` has no container while a sibling that did not still has one, out of one
   recorded pointer with nothing to capture. */
JSValue window_proxy_container(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);
    JSValue held;

    DCHECK(p != NULL, "§7.3.1.3's container of something that is not a WindowProxy was asked for");
    if (JS_IsNull(p->container)) return JS_NULL;
    held = iframe_navigable(ctx, p->container);
    if (JS_VALUE_GET_PTR(held) != JS_VALUE_GET_PTR(proxy)) {
        /* The element's content navigable is null or another navigable: §7.3.1.3 answers null, which happens only
           through §7.3.1.6's destroy-a-child-navigable, in this flow. */
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

/* §7.2.2.4's `opener` as the navigable, which an engine walk wants and the member must not give it: win_or_proxy
   maps this document's own navigable onto the global, because `w.opener === window` is the identity an opened
   page rests on, and a Window is not a WindowProxy. The distinction window_proxy_parent_navigable draws, for the
   same reason. */
JSValue window_proxy_opener_navigable(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);
    DCHECK(p != NULL, "the opener navigable of something that is not a WindowProxy was asked for");
    return JS_DupValue(ctx, p->opener);
}

/* The navigable a value names: win_or_proxy's mapping read backwards, in one place for its reason: a Window and
   its WindowProxy are two spellings of one navigable, and a caller that has to treat them as one (an encoder
   handing a navigable to another agent) must not be where that is decided. */
JSValueConst window_proxy_navigable_of(JSContext *ctx, JSValueConst v)
{
    JSValue g;
    bool is_global;

    if (window_proxy_is(v)) return v;
    /* The same non-object rule win_or_proxy states, in this direction: §3.7.6's receiver may be any value a page
       can write (`openerGet.call(42)` is a TypeError, not a question about a global), and a primitive has no
       pointer to compare. */
    if (!JS_IsObject(v)) return JS_UNDEFINED;
    g = JS_GetGlobalObject(ctx);
    is_global = JS_VALUE_GET_PTR(g) == JS_VALUE_GET_PTR(v);
    JS_FreeValue(ctx, g);
    return is_global ? document_window_proxy(ctx) : JS_UNDEFINED;
}

/* §3.7.6's first clause, written once: "if it is not null or undefined". Its two consumers want different answers
   out of it (the navigable the member is about, and the ES object the member is invoked on), and two copies of
   the test could disagree about a missing receiver. Side-effect-free. */
static bool wp_this_absent(JSValueConst this_val)
{
    return JS_IsUndefined(this_val) || JS_IsNull(this_val);
}

/* §3.7.6's `jsValue` itself: the platform object the member was invoked on, what §7.2.2.4's opener setter means
   by "perform ? DefinePropertyOrThrow(this, "opener", …)" and what [Replaceable]'s CreateDataPropertyOrThrow
   writes to. Not the navigable: for a missing receiver the navigable is `document_window_proxy(ctx)` and the
   object is the realm's global, and defining the page's value on the proxy would put it where §7.2.3.5 answers
   out of the surface, which the page cannot read. Owned. */
JSValue window_proxy_this_object(JSContext *ctx, JSValueConst this_val)
{
    if (wp_this_absent(this_val)) return JS_GetGlobalObject(ctx);
    return JS_DupValue(ctx, this_val);
}

/* §3.7.6's second sentence, "If jsValue does not implement target, then ... throw a TypeError", asked for target =
 * Window without looking up a navigable. A member that needs the navigable and one that needs only the object
 * are two questions, and window_proxy_this_navigable answers the first by DCHECKing on a receiver the second can
 * serve: [Replaceable]'s CreateDataPropertyOrThrow lands the page's value on `jsValue` itself without asking
 * which window it is, so a foreign realm's Window is a receiver it can answer.
 * Two kinds of object implement Window here. The realm's global carries the Window class (window_install hands it
 * one through JS_SetGlobalClass), so window_is answers for every realm's global alike, which is the spec's test,
 * since implementing an interface is a property of the object. A WindowProxy is the second, because §7.2.3.5
 * step 3 performs the same-origin OrdinaryGetOwnProperty on W and the accessor it hands back is invoked with the
 * proxy as its receiver; rejecting one would make `frames[0].length` a TypeError. Side-effect-free: it reads two
 * class ids. */
bool window_proxy_implements_window(JSValueConst js_value)
{
    return window_is(js_value) || window_proxy_is(js_value);
}

/* Does this receiver name the realm the member was installed in? Not a Web IDL step, but a question §3.7.6 makes
 * askable: a member whose answer is a value the realm already holds cannot tell a receiver from its realm on its
 * own. §3.7.6 runs the getter steps "with idlObject as this", so an attribute answering out of realm-held data is
 * correct while idlObject is this realm's Window. It normally is: each realm installs its own getter carrying its
 * own value, and js_call_c_function sets ctx to the member's realm, so `frames[0].document` reaches the child's
 * getter with the child's proxy. What does not agree is a receiver from one navigable applied to another realm's
 * accessor by hand (`Object.getOwnPropertyDescriptor(window, "screen").get.call(frames[0])`), where the held
 * value answers the reading realm's question about another window, a wrong answer with a plausible shape that is
 * worth a crash. Side-effect-free: window_proxy_navigable_of and document_window_proxy both borrow. */
bool window_proxy_receiver_is_own_realm(JSContext *ctx, JSValueConst js_value)
{
    JSValueConst nav = window_proxy_navigable_of(ctx, js_value);

    return !JS_IsUndefined(nav) &&
           JS_VALUE_GET_PTR(nav) == JS_VALUE_GET_PTR(document_window_proxy(ctx));
}

/* Web IDL §3.7.6 "Attributes"' `jsValue`, resolved to the navigable the member is about. §3.7.6's
 * create-an-attribute-getter: "Let jsValue be the this value, if it is not null or undefined, or realm's global
 * object otherwise", then "Let R be the result of running the getter steps of attribute with idlObject as this".
 * So a member of a [Global] interface is answered about the receiver, and the realm's own global is only the
 * fallback for a missing one; §3.7.7 Operations carries the same step for a method. A page reads the difference:
 * §7.2.3.5 step 3 performs a same-origin WindowProxy's [[GetOwnProperty]] on W, so the getter it hands back is
 * the other document's and is invoked with the proxy as its receiver, and
 * `Object.getOwnPropertyDescriptor(self, "opener").get.call(popup)` (opener-string.window.js's last assertion)
 * is that call written by hand. Both answer about the receiver's navigable, not the reading document's. */
/* Four arms, each a different fact:
 *   a WindowProxy is the navigable (win_or_proxy's mapping read backwards, window_proxy_navigable_of);
 *   this realm's global is this realm's navigable, the same mapping in the same place;
 *   null or undefined is §3.7.6's fallback to the defining realm's global, `ctx` here, because
 *     js_call_c_function sets it to the member's own realm, which is the realm §3.7.6 names;
 *   anything else does not implement Window and is §3.7.6's TypeError, thrown rather than answered, because a
 *     page distinguishes a throw from an answer about the wrong window.
 * Returns the navigable borrowed, or JS_UNINITIALIZED with a TypeError pending. */
JSValueConst window_proxy_this_navigable(JSContext *ctx, JSValueConst this_val)
{
    JSValueConst nav;

    if (wp_this_absent(this_val))
        return document_window_proxy(ctx);
    nav = window_proxy_navigable_of(ctx, this_val);
    if (!JS_IsUndefined(nav)) return nav;
    /* It is a Window, and not one this mapping can name. §3.7.6's TypeError is for a receiver that does not
       implement the interface, and this one does, so a throw would be a wrong answer with a plausible shape. The
       missing piece is an edge from a Window object to its navigable that does not go through a realm:
       window_proxy_navigable_of reads this realm's global out of `ctx`, and a foreign realm's global is the same
       question asked of a realm it has no handle to. Every other spelling of another navigable here is already
       the proxy (`contentWindow`, `defaultView`, `parent`/`top`/`opener`, and §7.2.3's own
       `window`/`self`/`frames`/`globalThis`), which is why nothing reaches this today. */
    DCHECK(!window_is(this_val),
           "a Web IDL attribute of the Window interface was invoked with ANOTHER realm's Window object as its "
           "receiver. §3.7.6's TypeError is for a receiver that does not implement the interface and this one "
           "does, so the answer is that navigable rather than a throw — build the Window -> navigable edge "
           "that does not go through a realm (document_window_proxy reads THIS realm's global out of its ctx, "
           "which cannot name a foreign one), and route window_proxy_navigable_of onto it");
    JS_ThrowTypeError(ctx, "a Window member was read on an object that is not a Window");
    return JS_UNINITIALIZED;
}

/* §7.2.2's `name` setter. It makes no origin check: `name` is not on §7.2.1.3.1's list, so §7.2.1.1 "Integration
   with IDL" refuses this setter for a cross-origin receiver before its body runs, at the one place every declared
   member converges on. A check here would read PROXY_CROSS_ORIGIN (indexed by this component's member magic)
   while §7.2.1.1 reads the standard's CROSS_ORIGIN, two tables deciding one question. The rename is
   window_proxy_name_assign's, because a Window and its proxy write the one navigable's one name. */
static JSValue proxy_name_set(JSContext *ctx, JSValueConst this_val, JSValueConst v, int magic)
{
    JSValueConst nav = window_proxy_this_navigable(ctx, this_val);

    (void)magic;
    if (JS_IsUninitialized(nav)) return JS_EXCEPTION;   /* §3.7.6's TypeError, already thrown */
    /* window_proxy_name_assign does its own proxy_of, which puts this write in the running flow's delta. */
    return window_proxy_name_assign(ctx, nav, v);
}

/* §7.2.2.4's `opener` setter through the proxy (`attribute any opener`), with the same two branches as window.c's
   spelling, because it is the same member on the same navigable:
     null  -> disown: the navigable's opener link is severed and no own property is defined, so everything that
              reads the navigable sees the cut rather than a `null` only the page can see;
     other -> §7.2.2.4's step 2, DefinePropertyOrThrow(this, "opener", {value, writable, enumerable,
              configurable}); Web IDL's [Replaceable] performs CreateDataPropertyOrThrow, which builds the same
              descriptor, and this member reaches it through that one implementation. The data property lands on
              this object and quickjs finds a shape property before the exotic hook, so §7.2.3.5 answers the
              page's value from then on.
   The navigable is `this_val`'s, not the reading realm's, the one difference from window.c's spelling and why a
   second body exists: that one answers for `document_window_proxy` because the Window it is installed on is its
   realm's navigable, and this one is handed the target. */
static JSValue proxy_opener_set(JSContext *ctx, JSValueConst this_val, JSValueConst v, int magic)
{
    JSValueConst nav = window_proxy_this_navigable(ctx, this_val);
    JSValue js;
    int r;

    (void)magic;
    if (JS_IsUninitialized(nav)) return JS_EXCEPTION;   /* §3.7.6's TypeError, already thrown */
    /* §7.2.1.3.1 lists `opener` with [[NeedsSetter]] false, so a cross-origin write is not a member of the
       cross-origin surface: §7.2.1.3.6 CrossOriginSet ( O, P, V, Receiver ) calls the setter only when the
       descriptor §7.2.1.3.4 built has one, and its last step throws a SecurityError. §7.2.1.1 "Integration with
       IDL" reads that flag off the list for every setter, before any body runs, so no refusal is written here. */
    if (JS_IsNull(v)) {
        window_proxy_disown_opener(ctx, nav);
        return JS_UNDEFINED;
    }
    /* §7.2.2.4's setter step 2 names `this`, §3.7.6's `jsValue`, the object, and the two answers part for a
       missing receiver: the navigable above is this realm's proxy, the define below lands on its global. A define
       on the navigable would write onto an object §7.2.3.5 answers out of §7.2.3's own surface, so the next read
       would return the opener again. */
    js = window_proxy_this_object(ctx, this_val);
    r = idl_replace_with_value(ctx, js, "opener", v);
    JS_FreeValue(ctx, js);
    return r < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

/* §7.2.2.1's `close()`. The whole method is document_lifecycle.c's, and this member is one of its two spellings:
   `w.close()` from an opener and `window.close()` inside the popup are one method on one navigable, which runs
   past step 3's is closing through steps 4-6. The navigable is this agent's whether or not its active document
   is, so there is nothing to delegate and no peer to ask: `this_val` is thisTraversable, and `ctx` is the
   incumbent realm step 6 asks about. */
static JSValue proxy_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    /* §3.7.7 Operations carries §3.7.6's receiver sentence verbatim, so a detached `close` closes the realm's own
       navigable, which is what `window.close.call(undefined)` means. */
    JSValueConst nav = window_proxy_this_navigable(ctx, this_val);

    (void)argc; (void)argv; (void)magic;
    if (JS_IsUninitialized(nav)) return JS_EXCEPTION;   /* §3.7.7's TypeError, already thrown */
    document_lifecycle_window_close(ctx, nav);
    return JS_UNDEFINED;
}

/* Is this navigable a top-level traversable: one fact, asked from one place. §7.2.2.1's close() returns early for
   anything else and §7.3's is closing is only set on one, and both spellings of close() (the Window member and
   the proxy's) must make the same test. */
bool window_proxy_is_top_level(JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);
    DCHECK(p != NULL, "something that is not a WindowProxy was asked whether it is a top-level traversable");
    return JS_IsUndefined(p->parent);
}

/* §7.2.2.1's is closing, for the Window spelling of `close()`: window.c's member and the proxy's are one method on
   one navigable, writing one flag through here. §7.2.2.1's early return for a navigable that is not a top-level
   traversable is the method's, not this setter's: a caller states the test it already made, and one that had not
   would otherwise mark a frame closing silently. */
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

/* §7.2.2 "The Window object" declares what an answer may be: `[Replaceable] readonly attribute unsigned long
   length` and `readonly attribute boolean closed`. A peer resolves the read by running the member's getter, so a
   normal completion carries one of those two types, and this is the only place that can say so: remote_object.h's
   grammar exists because `otherW.length === 0` distinguishes the number 0 from the string "0", and a relay that
   encoded a number as `s…` or lost it to `u` would hand the page a plausible value. The local arms are typed by
   construction (JS_NewInt32, JS_NewBool); this one is whatever arrived.
   A predicate rather than an expression at the call site, because check.h compiles a DCHECK's condition to
   `sizeof` in release and it must still type-check there. Side-effect-free: it reads the value's tag.
   A member it does not name answers false: a third member routed onto the remote path is one whose IDL type
   nothing has stated. */
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

/* Where this machine rests. Both members it drives are read off state the other instance keeps (§7.2.3's `length`
   off that navigable's active document, §7.2.2.1's `closed` off the top-level traversable whichever agent ran
   close() wrote), so the read is one step of the standard performed in another instance, and the wait for that
   peer is a sub-sequence inside it (`req` is its cursor), not a step of its own. One stage: a flow parked here is
   parked at the read it made. */
/* §7.2.1's two record-answerable members, computed once for both sides of the seam: proxy_get_step's hosted arm
   answers a read made in this instance, and the receiving half of a `windowproxy.get`
   (window_proxy_record_member_of_document) answers one made in another, so both call this rather than spelling
   the answer twice.
   1 and `*out` when the member is one of the two, 0 and `*out` untouched otherwise; the refusal is the caller's
   to spell, so this returns a verdict instead of crashing. A DFAIL here would stamp this line for both callers
   and could name neither the step machine nor the arriving record; each caller knows what a third member means.
   Why these two: HTML §7.2.2.2 "Indexed access on the Window object" makes `length` a count ("The length getter
   steps are to return this's associated Document's document-tree child navigables's size") and §7.2.2.1 "Opening
   and closing windows" makes `closed` a pair of flags this agent writes ("The closed getter steps are to return
   true if this's navigable is null or its is closing is true; otherwise false."). Both are facts the §7.2.3
   record holds, so neither needs the active document's realm. Every other member of §7.2.1.3.1's thirteen names
   an object of that realm (`location`, and §7.2.2's `document`) or is a navigable of this agent proxy_member_get
   answers in turn. */
/* A navigable whose realm is not materialized counts zero, the computed answer: its active document is the empty
   about:blank Document §7.4 created, an element can only get into it by script, and script cannot run in a realm
   that does not exist, so building a realm to count its frames would cost a realm per flow per read. A destroyed
   navigable counts zero too: §7.5.10 "Destroying documents"' set-the-Document's-browsing-context-to-null step
   reaches window_proxy_set_destroyed, which nulls this record's realm, so there is no active document to count.
   `realm ? … : 0` answers both, and window_proxy_document_state is the entry that partitions them.
   JS_GetOpaque, not proxy_of: neither arm writes the record, and a capture would put an entry in the running
   flow's delta for storage the read never touches (window_proxy_closed captures through its own accessor, as
   that member's contract). */
static int proxy_record_member(JSContext *ctx, JSValueConst nav, int magic, JSValue *out)
{
    ProxyData *p = JS_GetOpaque(nav, g_proxy_class);

    DCHECK(p != NULL, "§7.2.1's record-answerable members were asked of something that is not a WindowProxy");
    DCHECK(out != NULL, "§7.2.1's record-answerable members were asked with nowhere to put the answer");
    switch (magic) {
    case WP_LENGTH:
        *out = JS_NewInt32(ctx, p->realm ? iframe_child_navigable_count(p->realm) : 0);
        return 1;
    case WP_CLOSED:
        /* §7.2.2.1's whole or, which is window_proxy_closed's, never the recorded flags (see wp_bc_null): a frame
           removed from the tree is closed on that line. */
        *out = JS_NewBool(ctx, window_proxy_closed(ctx, nav));
        return 1;
    default:
        return 0;
    }
}

/* Which member a name is, over the one table that spells them. PROXY_MEMBER is also what the asking side writes
   into a `windowproxy.get` record (proxy_get_step), so both halves of the seam read one spelling. -1 for a name
   this surface does not install, which is not the same set as §7.2.1.3.1's thirteen (`close`, `focus`, `blur`
   and `postMessage` are on that list and are not accessors of this object); the caller decides what that absence
   means. */
static int proxy_member_of_name(const char *name)
{
    int i;

    DCHECK(name != NULL, "a WindowProxy member was looked up by no name at all");
    for (i = 0; i < WP_MEMBER_N; i++)
        if (!strcmp(PROXY_MEMBER[i], name)) return i;
    return -1;
}

/* The same two answers for a caller holding a document name and a member name (see window_proxy.h). */
int window_proxy_record_member_of_document(JSContext *ctx, uint32_t doc, const char *member, JSValue *out)
{
    JSValue held;
    int magic, answered;

    DCHECK(member != NULL,
           "§7.2.1's record-answerable members were asked of a document with no member named — the caller holds "
           "a record whose verb it has not read, and `windowproxy.get` is the only one of the six that carries a "
           "member at all");
    DCHECK(out != NULL, "§7.2.1's record-answerable members were asked with nowhere to put the answer");
    magic = proxy_member_of_name(member);
    if (magic < 0) return 0;
    /* The partition is asked of the entry that owns it, and the two states answering 0 here are the two whose own
       aborts name the work: a document whose active Document is a peer's (no realm of this heap is it, now or
       ever) and a name this timeline holds no navigable of. Answering `length` 0 for either would report a count
       of a document this agent knows nothing about as real, and the caller's own realm lookup aborts on both by
       name. The other three states mean this agent holds the navigable, which is all the two members read. */
    switch (window_proxy_document_state(doc)) {
    case WP_DOC_ACTIVE: case WP_DOC_DESTROYED: case WP_DOC_INITIAL: break;
    case WP_DOC_REMOTE: case WP_DOC_NO_NAVIGABLE: return 0;
    }
    held = window_proxy_of_document(ctx, doc);
    /* Reachable only in release, so a guard rather than an assert: window_proxy_of_document DCHECKs a hosted
       document it holds no navigable for, and the partition has excluded every state in which it holds none, so
       a JS_UNDEFINED here is that assert compiled out, and the caller's lookup names it. */
    if (JS_IsUndefined(held)) return 0;
    answered = proxy_record_member(ctx, held, magic, out);
    JS_FreeValue(ctx, held);
    return answered;
}

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

    /* §3.7.6's receiver, resolved by the one place that resolves it, as in proxy_member_get: `closed` and `length`
       are on §7.2.1.3.1's cross-origin list, so a page reaches both through a descriptor's `.get` with no
       receiver, as it reaches `opener`. */
    nav = window_proxy_this_navigable(ctx, hdr->this_val);
    if (JS_IsUninitialized(nav)) return JS_STEP_ABRUPT;   /* §3.7.6's TypeError, already thrown */
    /* JS_GetOpaque, not proxy_of: neither member below writes the record, and a capture would put an entry in the
       running flow's delta for storage the read never touches. proxy_member_get captures because `name`'s setter
       shares its record. */
    p = JS_GetOpaque(nav, g_proxy_class);
    DCHECK(p != NULL, "§3.7.6's receiver resolved to something that is not a WindowProxy");
    if (!proxy_read_permitted(p, magic)) {
        JS_ThrowDOMException(ctx, "SecurityError",
                             "the origins do not permit reading this member of that Window");
        return JS_STEP_ABRUPT;
    }

    /* The hosted case, answered in this turn out of the target's own record. §7.2.3's `length` is the
       child-navigable count of the active document, a walk of that document's tree (the reading realm would
       count its own frames), and §7.2.2.1's `closed` is the top-level traversable's, written by the agent that
       holds the active document. proxy_record_member computes both, with the reasoning for counting an
       unmaterialized realm as zero. The refusal is this site's: a third member reaching this step machine is one
       proxy_member_get answers in turn, a fact about this surface the shared computation cannot state, so the
       verdict comes back and the crash is spelled here. */
    if (world_doc_hosted(p->doc)) {
        if (!proxy_record_member(ctx, nav, magic, presult)) {
            DFAIL("a navigable-own member reached the step machine — it is answered by proxy_member_get, in "
                  "this turn, with no host round trip");
            *presult = JS_UNDEFINED;
        }
        return JS_STEP_DONE;
    }
    /* This agent's record already says closed, and `closed` never goes back: §7.2.2.1's close() step 3 returns
       early once is closing is true and a null browsing context is terminal, so a local true is the standard's
       answer, and it stays answerable after the host has torn that instance down. A local false says nothing
       about the peer, which is why the branch below exists. For `length` the same state answers 0: a destroyed
       navigable has no active document to count. It is the whole or, §7.3.1.6's sever included: a cross-origin
       child removed from this document's tree is detached here on that line, so a round trip to the peer would
       ask what this side already holds. */
    if (window_proxy_closed(ctx, nav)) {
        *presult = magic == WP_CLOSED ? JS_TRUE : JS_NewInt32(ctx, 0);
        return JS_STEP_DONE;
    }
    /* A nested navigable's `closed` is this agent's either way, so it is answered here. HTML §7.3.1 declares is
       closing with "This is only ever set to true for top-level traversable navigables" (window_proxy_set_closing
       asserts it), so the only half of §7.2.2.1's or a nested navigable can satisfy is a null browsing context,
       written by §7.3.1.6's destroy a child navigable, run by the agent holding the container element: this one. A
       peer asked about its own document would answer out of its root proxy, the record of a navigable it believes is
       top-level. */
    if (magic == WP_CLOSED && !JS_IsUndefined(p->parent)) { *presult = JS_FALSE; return JS_STEP_DONE; }

    if (s->req == 0) {
        /* Two world vectors fit here: the record carries the asking flow's world and the peer timeline this flow
           is already in, each a head plus a filtered ancestry written by world_serialize's arithmetic. The CHECKs
           below enforce the fit (a truncated vector makes the peer fork a more distant ancestor and silently lose
           the writes in between, world.h's reason for crashing rather than sending one); the size keeps the
           ordinary two-vector record well inside it. */
        char op[2048];
        Flow *f = flow_running();
        char *addr;
        int n;

        DCHECK(f != NULL, "a cross-document read was issued outside a flow — there would be nothing to suspend");
        /* (document, world+ancestry, member), every document by name: a `uint32_t doc` is this instance's handle
           into its own name table and means a different document in the peer's. The ancestry travels with the
           world: a peer materializes its segment for a world it has never seen by forking the nearest ancestor
           it already holds (world.h), so a request carrying only the world's name would make every peer start
           from an empty segment, true for a flow that never wrote there and false for one that forked after
           writing. Nearest first, because the scan stops at the first hit and a further ancestor would drop the
           nearer one's writes. */
        /* Which of that document's timelines this flow is already in, read before the record is composed because
           it is a field of it (solver/engine.h). A flow that has taken no answer from that peer addresses nobody
           and says so with a token rather than an empty field: any of the peer's timelines may answer it
           truthfully. One that has taken an answer is in that sending world from then on, so the peer's other
           arms are timelines this flow was never in, and a read answered out of one would be the off-diagonal
           member of a cross-product, the fabrication solver/flow.c's commitment record aborts on. */
        addr = engine_flow_addressee(ctx, f, world_doc_name(p->doc));
        /* Every field's fit is asserted: a truncated record reaches no instance and parks this flow for ever, and
           a truncated member makes the peer answer a different question as if it were this one, both silently. */
        {
            char *head = engine_record_head("windowproxy.get", world_doc_name(p->doc), f);

            n = snprintf(op, sizeof op, "%s\t%s\t%s", head, addr ? addr : ENGINE_ADDRESSEE_NONE,
                         PROXY_MEMBER[magic]);
            free(head);
        }
        free(addr);
        /* The addressee's fit is this check's too: a truncated addressee is a world vector naming a timeline this
           flow did not commit to, and the peer would refuse the flows that one contradicts, dropping real
           timelines on a name nobody wrote. It fails with a truncated member, for the same reason. */
        CHECK((size_t)n < sizeof op,
              "a cross-document read's addressee and member did not fit its record — a TRUNCATED member makes "
              "the peer run a program for a different question and answer it as if it were this one, and a "
              "TRUNCATED addressee names a timeline this flow never committed to, so the peer refuses every "
              "flow that one contradicts and this read is answered out of worlds neither agent was in");
        s->req = engine_host_request(ctx, op);
        return JS_STEP_YIELD;   /* park; siblings run until the peer answers */
    }
    if (!engine_host_answered(s->req, &answer))
        return JS_STEP_YIELD;
    /* The peer's completion, not its value: §7.2.1's member is an IDL getter the peer runs, so it can throw (a
       SecurityError from a member outside the cross-origin list, or the page's own accessor), and the throw
       belongs at the line that read the member. */
    {
        int r = engine_host_take_completion(ctx, s->req, presult);
        s->req = 0;
        /* And against §7.2.2's IDL type for the member asked. A throw is exempt: the thrown value is an Error
           object and crosses as a name, which is why the completion is layered over the value grammar rather
           than beside it. The peer performs §7.2.1.3.4 CrossOriginGetOwnPropertyHelper's getter (remote_op.c's
           OPS table, over the table window_proxy_install_window_getters captures), not an ordinary [[Get]] of
           its global, so a page's `window.length = 5` (Web IDL §3.3.11 [Replaceable]'s shadow, a number no test
           on the value could tell from a count) never reaches this answer. A hit means the completion lost its
           type crossing the boundary, the failure core/frame/remote_object.h's leading byte exists to make
           impossible: a fact about the transport, which this side can know on its own. */
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

/* §7.2.1's member surface for one realm. Declaration order in core/realm.h's list matters in one direction:
   window_message.c's `postMessage` goes onto this object, so its entry is declared after this one and finds it
   built. */
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
    /* HTML §7.2.3 "The WindowProxy exotic object": "There is no WindowProxy interface object." The surface below is
       the [[Window]]'s (§7.2.3.7's same-origin [[Get]] is an OrdinaryGet answering out of it), so Web IDL §3.7.3
       "Interface prototype object"'s class string on this object is Window's identifier. It is also what a page
       reads: `Object.prototype.toString.call(w)` is "[object Window]" for a window it may see, and for one it
       may not §7.2.3.1's [[GetPrototypeOf]] returns null while §7.2.1.3.2's CrossOriginPropertyFallback answers
       %Symbol.toStringTag% with undefined before any walk. The statement also lets the Web IDL gap audit
       attribute the members installed here, and window_message.c's `postMessage` (§9.3.3 "Posting messages"),
       to Window.
       idl_class_string, not idl_interface_tag: this object is not §3.7.3's Window interface prototype object,
       which is core/frame/window.c's, built over §3.7.4's named properties object, whose chain idl_args.c
       asserts against the IDL at every tag. Both forms seed engine/idl_installed.mjs's table, so the members
       are attributed to Window either way. */
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
    /* §7.2.1.3.1's `focus` and `blur` are on the thirteen-name cross-origin list, and §7.2.1.3.4
       CrossOriginGetOwnPropertyHelper runs OrdinaryGetOwnProperty on the Window, which owns them, so this surface
       carries them too; otherwise `otherW.focus` would be `undefined` once §7.2.3.5's cross-origin branch answers
       `0` and the walk reaches this object. proxy_capture_names's reconciliation iterates the member table and
       cannot see a name with no member entry; the loop in proxy_get_own's steps 4-6 checks this direction. It is
       §6.6.6's own install, the one window_install calls, so the global and this surface hold the same set. */
    focus_install_window_members(ctx, proto);
    JS_SetClassProto(ctx, g_proxy_class, proto);
}

void window_proxy_init(JSContext *ctx)
{
    /* §7.2.3.5's cross-origin branch is the class's, not the prototype's: it runs before the prototype walk, which
       is why a name that is not a member can be refused at all. */
    JSClassDef d = { "WindowProxy", .finalizer = proxy_finalizer, .gc_mark = proxy_gc_mark,
                     .exotic = &PROXY_EXOTIC };
    JSRuntime *rt = JS_GetRuntime(ctx);
    int i;

    /* One instance is one agent, so this runs once and a second call is a defect, not a no-op: a component that
       reports itself built is what a stale handle produces (core/agent_state.h's latch argument), so the state is
       made unreachable rather than tolerated. */
    DCHECK(g_wp_rt == NULL, "window_proxy_init ran twice — one instance is one agent, and a second declaration "
                            "would re-mint the class id every live proxy is already branded with");
    g_wp_rt = rt;
    /* The accessor side of the check is read, not kept: origin_agent() is the one record for this agent, asserted
       to exist here at the declaration rather than at the first read that decides. */
    DCHECK(origin_agent() != NULL, "§7.2.1's surface was declared in an agent with no origin");
    JS_NewClassID(rt, &g_proxy_class);
    JS_NewClass(rt, g_proxy_class, &d);
    proxy_capture_names(ctx);
    /* The pool entries are the agent's (a declaration is a runtime registration, and every realm's members carry
       the same ids); only the objects are per realm. */
    g_wp_len_getter_id    = idl_getter_id_step(ctx, &PROXY_GET_DECL, WP_LENGTH);
    g_wp_closed_getter_id = idl_getter_id_step(ctx, &PROXY_GET_DECL, WP_CLOSED);
    g_wp_name_setter_id = idl_setter_id(ctx, IDL_DOMSTRING, false, proxy_name_set, WP_NAME);
    /* §7.2.2.4's `attribute any opener`: IDL_ANY, so the value reaches the body unconverted, and each of
       opener-setter.window.js's seven values (a symbol among them) is stored as it stands. */
    g_wp_opener_setter_id = idl_setter_id(ctx, IDL_ANY, false, proxy_opener_set, WP_OPENER);
    g_wp_close_id       = idl_method_id(ctx, NULL, 0, proxy_close, 0);
    /* §7.2.2's `[PutForwards=href, LegacyUnforgeable] readonly attribute Location location`, the same IDL line
       location.c installs on the global, so it carries the same setter here and `frames[0].location = u` and
       `otherW.location = u` navigate as `window.location = u` does. The forwarding's Get lands on WP_LOCATION
       above, so the same-origin arm reaches the active document's real Location and the cross-origin arm
       §7.2.4.5's filtered one; `href`'s setter is the one member both have, which is why §7.2.1.3.1
       CrossOriginProperties ( O ) lists `location` at all. */
    g_wp_location_setter_id = idl_setter_id_put_forwards(ctx, "location", "href");
    /* §7.2.1.3.4's per-realm getter table, declared here and filled per realm: the slot is a class id, which
       belongs to a runtime and so to the agent, while the array it holds is each realm's own. */
    g_xo_getter_slot = realm_value_declare(ctx,
                                           "HTML §7.2.1.3.4 CrossOriginGetOwnPropertyHelper's getters for this "
                                           "realm's Window, captured before any page script ran");
    /* What this component holds for the agent, declared (core/agent_state.h). A release that gave back the atoms,
       the string list and the remote-navigable table but kept the ids would let a second agent in one process
       install §7.2.3's member surface out of a pool without those entries, a wrong answer with a live-looking
       number behind it that neither of JS_FreeRuntime's censuses can report. The class id is given back for the
       reason that header states, which is why the two collector entries read the record through
       JS_GetAnyOpaque. */
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
    agent_state_realm_slot(WP_COMPONENT, &g_xo_getter_slot,
                           "the realm-value slot holding §7.2.1.3.4 CrossOriginGetOwnPropertyHelper's captured getters");
    for (i = 0; i < CROSS_ORIGIN_NAME_N; i++)
        agent_state_atom(WP_COMPONENT, &g_xo_atom[i],
                         "one of §7.2.1.3.1 CrossOriginProperties ( O )'s names, interned");
    for (i = 0; i < XO_FALLBACK_N; i++)
        agent_state_atom(WP_COMPONENT, &g_xo_fallback[i],
                         "one of §7.2.1.3.2 CrossOriginPropertyFallback ( P )'s names, interned");
    agent_state_ptr(WP_COMPONENT, &g_strings, "every origin and name string a proxy of this agent recorded");
    agent_state_ptr(WP_COMPONENT, &g_remote_navs, "the one WindowProxy per REMOTE document of this agent");
    agent_state_ptr(WP_COMPONENT, &g_live_navs, "every live WindowProxy of this agent, scanned on its per-flow doc");
    realm_declare_intrinsic(window_proxy_install_proto);
}

JSValue window_proxy_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_proxy_class);
    DCHECK(!JS_IsNull(proto),
           "the WindowProxy prototype was asked for in a realm that never ran window_proxy_install_proto");
    return proto;   /* owned */
}

/* The agent's half, undone: core/platform.h's third column. It takes the runtime because that is what an agent is,
   which puts it on that column, so reverse declaration order releases the components built over this one
   (remote_location's references chain to §7.2.3's prototype) before it. JS_FreeAtomRT reaches the same atoms
   JS_FreeAtom would (JS_FreeAtom is JS_FreeAtomRT(ctx->rt, a)). */
void window_proxy_free(JSRuntime *rt)
{
    OwnedStr *e = g_strings;
    int i;

    /* Not `if (!g_wp_rt) return;`: this runs from a release column that runs only where platform_agent_init ran,
       and this component's declaration is unconditional on that list, so a null runtime is a host tearing down a
       browser it never built, which a silent return would make indistinguishable from a release that worked
       (core/agent_state.h). */
    DCHECK(g_wp_rt != NULL,
           "§7.2.3's surface was released in an agent that never declared it — window_proxy_init is a row on "
           "core/platform.c's declare column, so reaching here without it is a teardown of a browser that was "
           "never brought up");
    DCHECK(g_wp_rt == rt,
           "§7.2.3's surface was released against a RUNTIME other than the one it was declared in — its class "
           "and its six pool entries are registrations in that runtime, and giving them back against another "
           "frees atoms of a heap this component never interned into");
    while (e) { OwnedStr *n = e->next; free(e->s); free(e); e = n; }
    g_strings = NULL;
    /* §7.2.1.3.1 CrossOriginProperties ( O )'s and §7.2.1.3.2 CrossOriginPropertyFallback ( P )'s names,
       released with the agent that interned them. */
    for (i = 0; i < CROSS_ORIGIN_NAME_N; i++) { JS_FreeAtomRT(rt, g_xo_atom[i]); g_xo_atom[i] = JS_ATOM_NULL; }
    for (i = 0; i < XO_FALLBACK_N; i++) { JS_FreeAtomRT(rt, g_xo_fallback[i]); g_xo_fallback[i] = JS_ATOM_NULL; }
    /* The remote-navigable rows borrow, so this frees the table and nothing in it. Emptying it keeps a finalizer
       running later in the teardown from scanning freed storage: the loop reads n == 0 and touches nothing. */
    free(g_remote_navs);
    g_remote_navs = NULL;
    g_remote_navs_n = g_remote_navs_cap = 0;
    /* The live-navigable table borrows likewise and is emptied for the same reason: a finalizer running later in
       this teardown scans it, and a freed table with a nonzero count is storage it would walk. */
    free(g_live_navs);
    g_live_navs = NULL;
    g_live_navs_n = g_live_navs_cap = 0;
    /* The step-9 ask is a census of one agent, like navigable.h's three, so it goes back with the agent that
       performed the releases rather than accumulating across a host that builds a second browser. */
    g_step9_releases = 0;
    /* The pool entries come back: a declaration is a registration in a runtime that is going away, so a carried
       id would be an index into a pool the next agent has not built, read by window_proxy_install_proto at that
       agent's first realm. */
    g_wp_len_getter_id = g_wp_closed_getter_id = -1;
    g_wp_name_setter_id = g_wp_opener_setter_id = g_wp_close_id = g_wp_location_setter_id = -1;
    /* The realm-value slot likewise: a class id in a runtime that is going away, which the next agent's first
       cross-instance member read would otherwise index, running whatever function sits there. */
    g_xo_getter_slot = JS_INVALID_CLASS_ID;
    /* The class id, for core/agent_state.h's reason: a class is registered in a runtime and the id names a class
       that is gone, so every JS_GetOpaque against it would answer about whichever class the next agent's runtime
       hands that number to. proxy_finalizer and proxy_gc_mark read the record through JS_GetAnyOpaque because
       this line runs before the collection that reaches them. */
    g_proxy_class = 0;
    /* the prototypes are the realms', released with their contexts, and the origins are the agent's, released by
       origin_release with everything else that outlives a realm */
    g_wp_rt = NULL;
}
