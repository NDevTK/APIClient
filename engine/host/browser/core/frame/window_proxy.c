/* WindowProxy — HTML §7.2.5.1, and the reason it is not a formality here.
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
 *   - SO IS THE SAME-ORIGIN CHECK. §7.2.5.1's [[Get]] is filtered by whether the ACCESSOR and the target's
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
#include "core/frame/window_proxy.h"
#include "solver/cow.h"
#include "solver/concolic.h"
#include "solver/world.h"
#include "solver/engine.h"
#include "solver/flow.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/html/html_iframe.h"
#include "core/frame/navigable.h"
#include "core/frame/location.h"
#include "core/frame/document_lifecycle.h"
#include "core/dom/document.h"
#include <stdio.h>

/* §7.2.5.1's [[Window]] and the origin its same-origin check reads. BOTH are per-flow — see the file comment —
   which is the whole reason this is a record behind a class rather than a property on an object. */
typedef struct {
    JSValue window;    /* the navigable's active Window, or JS_UNDEFINED when the document is remote (owned) */
    /* THE ACTIVE DOCUMENT'S REALM, when this agent holds it — NULL when the document is a peer's. It is what a
       member that reads THROUGH to the active document is answered from: `length` counts the child navigables
       of THAT document, and asking it of the READING realm counts the wrong document's frames. It is also what
       BORROWED: the AGENT owns every realm it built and releases them with itself (navigable.c). A proxy is a
       GC object and a realm is not, so an owning reference here would have to be released from a finalizer —
       which frees JSValues during collection — and the realm's own Document teardown would still never run.
       NULL UNTIL THE DOCUMENT IS MATERIALIZED — see proxy_realm and navigable.h. `url` is what it is built
       from; a navigable that has been NAVIGATED is materialized at creation, one that still holds its initial
       about:blank Document only when something reads through it. */
    JSContext *realm;
    char      *url;    /* the navigable's address, for a realm not yet materialized (owned) */
    char   *origin;    /* the active document's origin, for §7.2.5.1's same-origin check (owned) */
    /* THE NAVIGABLE'S OWN STATE — see the member table below for why it is here and not in the peer. All four
       are per-flow: a flow that closed the window or renamed it is the only one whose timeline contains that,
       and `parent`/`opener` are values a fork must carry. */
    JSValue parent;    /* the parent navigable's proxy (or the creator's Window); JS_UNDEFINED = top-level */
    JSValue opener;    /* §7.2.5's opener — the navigable that opened this one, or JS_NULL */
    char   *name;      /* §7.2.5's name: the BROWSING CONTEXT's, not the element's (owned; see proxy_of) */
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
    /* §7.2.6's POLICY CONTAINER THE CREATOR HANDED THIS NAVIGABLE, taken at CREATION and kept as text because
       that is what §7.4's "clone a policy container" is (policy_container_clone re-parses this same string).
       It is here rather than read off whichever realm materializes the document because a navigable created
       with no address is materialized LAZILY, on the first read that reaches through it — and the reader may
       be a different same-origin document with a different policy, which would give the child whichever
       document happened to touch it first. A creation fact, so like `is_popup` no flow can change it.
       NULL for a navigable with an address (its policy comes with its response) and for the root. Owned. */
    char   *creator_csp;
    /* HTML §8.1.3.1's TOP-LEVEL CREATION URL for the environments of this navigable's documents — kept beside
       the policy container because it is the same KIND of fact and arrives the same way: decided by the
       operation that created the navigable, read when a realm is finally built. §7.4 makes a CHILD navigable
       inherit its creator's and gives an AUXILIARY one its own address, and §7.11's navigation of a top-level
       traversable moves it to the new address while a nested navigable's stays where its creation put it.
       IT IS PER-FLOW, exactly like `url`: navigating a top-level traversable moves it, so an arm that
       navigated and an arm that did not must not share one. It is a POD pointer inside the bytes proxy_of
       captures, and — like every other string here — it is never freed on a navigation, because a parked
       flow's saved bytes still name the old one. */
    char   *top_level_url;
    /* §7.2.5's `closed` IS TWO FACTS AND THE GETTER IS THEIR OR — "true if this's browsing context is null or
       its is closing is true". They are two because they happen at two TIMES. `close()` sets is-closing at its
       own call site and QUEUES the destruction; §7.3.1's removal of a container queues one without setting
       is-closing at all; and the browsing context does not become null until §7.5.10 step 8 runs, in a task.
       Held as ONE byte those two times collapsed into the removal's, so a frame's WindowProxy reported a
       destruction that had not happened — while its Document, its queued tasks and its entangled ports were
       all still live, with nothing anywhere holding the fact that they had not been dealt with. */
    uint8_t closing;     /* §7.3's IS CLOSING — §7.2.5.2's close(), and only ever a top-level traversable */
    uint8_t destroyed;   /* §7.5.10 step 8 ran on this navigable's active document: its browsing context is null */
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

/* §7.2.5's `closed` GETTER, AS ONE EXPRESSION — "true if this's browsing context is null or its is closing
   is true". Written once here so that no member can ask half of it: a `closed` that read only the destroy
   flag would report a closing popup as open, and one that read only is-closing would report a removed frame
   as open. Both of those were the single byte this replaced, in the two directions it could be wrong. */
static bool wp_closed(const ProxyData *p) { return p->closing != 0 || p->destroyed != 0; }

static JSClassID g_proxy_class;
static JSRuntime *g_wp_rt;
/* THIS DOCUMENT'S ORIGIN — the ACCESSOR side of §7.2.5.1's same-origin check, and the reason the check can
   exist at all. A proxy carries the origin of the navigable's active document; the comparison needs the origin
   of whoever is reading, which is this instance's, and there is exactly one of those. */
static char *g_local_origin;
/* §7.2.5.1's member surface. A WindowProxy has no own properties: it FORWARDS to its navigable, answering from
   the navigable's own state where that is what the member is, and suspending on the host where the member reads
   the active document. See the member table lower down for which is which.
   IT IS PER REALM, in quickjs's own class-proto slot, and that is an ANSWER and not an identity: a member runs
   in the realm that DEFINED it, so one shared object would answer `parent`, `name` and `close()` for every
   document out of whichever realm built it first — and the whole point of these members is that they read the
   asking document's side of a boundary. `a.postMessage === b.postMessage` still holds for two proxies of the
   same realm, which is what §7.2.5.1's shared surface means; two REALMS have two, exactly as two realms have
   two `Array.prototype.map`s. */
static int g_wp_len_getter_id = -1, g_wp_name_setter_id = -1, g_wp_close_id = -1;


#define WP_OFF(f) (uint16_t)offsetof(ProxyData, f)
static const uint16_t PROXY_VALS[] = { WP_OFF(window), WP_OFF(parent), WP_OFF(opener) };
static const CowRecord PROXY_REC = { sizeof(ProxyData), PROXY_VALS, 3 };

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

/* §7.2.5.1's SAME-ORIGIN CHECK, and the one rule that makes it more than a string compare: an OPAQUE origin is
   unique per spec, so it is same-origin with NOTHING — not even with another opaque one, and not with itself.
   A sandboxed frame serializes its origin as "null", and treating two "null"s as equal would let one sandboxed
   document script another. SECURITY.md states the same rule for the credentialed-read principal, and it is the
   same rule because it is the same concept. */
static bool proxy_same_origin(const ProxyData *p)
{
    DCHECK(g_local_origin != NULL, "the same-origin check ran before window_proxy_init named this document's "
                                   "origin — the accessor side of §7.2.5.1 has no value to compare against");
    if (!p->origin || !strcmp(p->origin, "null") || !strcmp(g_local_origin, "null")) return false;
    return !strcmp(p->origin, g_local_origin);
}

bool window_proxy_is(JSValueConst v)
{
    return g_proxy_class != 0 && JS_GetOpaque(v, g_proxy_class) != NULL;
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

static void proxy_finalizer(JSRuntime *rt, JSValue val)
{
    ProxyData *p = JS_GetOpaque(val, g_proxy_class);
    (void)rt;
    if (!p) return;
    JS_FreeValueRT(rt, p->window);
    JS_FreeValueRT(rt, p->parent);
    JS_FreeValueRT(rt, p->opener);
    free(p);   /* the strings are the component's, released at teardown — see proxy_of */
}

static void proxy_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    ProxyData *p = JS_GetOpaque(val, g_proxy_class);
    if (!p) return;
    /* The Window holds the proxy (as `window`) and the proxy holds the Window: a cycle, and one the collector
       has to see or every navigable leaks its global. `parent` and `opener` close cycles of their own — a child
       names its creator's Window, which reaches the child through the element wrapper that holds it. */
    JS_MarkValue(rt, p->window, mark_func);
    JS_MarkValue(rt, p->parent, mark_func);
    JS_MarkValue(rt, p->opener, mark_func);
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
    if (!p->realm) {
        DCHECK(p->url != NULL, "a WindowProxy with no realm and no address was read through — a proxy over a "
                               "realm that already exists is minted by window_proxy_new_self, which carries it");
        /* THE INITIAL about:blank, WHICH CAME FROM NO RESPONSE — so no bytes, and §7.2.6's policy is the clone
           of its creator's this navigable was CREATED with (see creator_csp). Fetching anything here is not
           deferred, it is IMPOSSIBLE: this runs inside the property read that reached through the navigable,
           and a fetch suspends. The document an address serves arrives on the load job instead, which is a
           flow and can park (navigable.c). */
        /* THE ENVIRONMENT'S TOP-LEVEL CREATION URL IS THE NAVIGABLE'S, not the reading realm's — the read that
           materializes an about:blank child may come from any same-origin document, and answering from `ctx`
           would give the child whichever document happened to touch it first. The same sentence as
           `creator_csp` beside it, and the same field for the same reason. */
        p->realm = navigable_realm(ctx, p->doc, p->url, p->top_level_url, p->origin, proxy, NULL, 0,
                                   p->creator_csp);
        p->window = JS_GetGlobalObject(p->realm);
    }
    return p->realm;
}

/* §7.2.5.1's NAVIGATE — see window_proxy.h. Reached from navigable.c, which owns the fetch and the realm. */
void window_proxy_navigate(JSContext *ctx, JSValueConst proxy, JSContext *realm, uint32_t doc,
                           const char *url, const char *top_level_url, const char *origin)
{
    ProxyData *p = proxy_of(proxy);   /* the capture is in the accessor — the WHOLE binding rides the delta */

    DCHECK(p != NULL, "something that is not a WindowProxy was navigated");
    DCHECK(realm != NULL, "a navigable was navigated to no realm — a Document this agent holds IS a realm, and "
                          "a cross-origin destination is a peer's document, which is a host route and not this");
    DCHECK(world_doc_hosted(doc), "a navigable was navigated to a document this agent does not hold");
    /* THE OLD WINDOW'S REFERENCE IS THIS SLOT'S, and the delta already dupped its own copy when the capture
       above ran — so releasing it here leaves the parked arm's copy intact and the live slot free to move. */
    JS_FreeValue(ctx, p->window);
    p->realm  = realm;
    p->window = JS_GetGlobalObject(realm);
    p->doc    = doc;
    /* THE OLD STRINGS ARE NOT FREED. A parked flow's saved bytes still name them (see proxy_of), so freeing
       here would resume that flow onto freed memory; they are the PROXY's and are released with it. */
    p->url    = proxy_strdup(url);
    p->origin = proxy_strdup(origin ? origin : "null");
    /* THE NEW DOCUMENT'S ENVIRONMENT MOVED WITH IT, and the caller states where to — a navigation of a
       TOP-LEVEL traversable puts the environment at the new address, while a nested navigable's stays where
       its creation put it (HTML §8.1.3.1). It is not re-derived here from `url`, because whether this
       navigable is nested is a fact about the OPERATION's target and the realm the caller has ALREADY BUILT
       was built under this exact string: deriving it a second time is the second answer that disagrees. */
    DCHECK(top_level_url != NULL && *top_level_url,
           "a navigable was navigated with no top-level creation URL for the new document's environment — the "
           "realm the caller just built has one, and these two must be the same string");
    p->top_level_url = proxy_strdup(top_level_url);
}

/* THE SELF PROXY'S REALM IS THE ONE ASKING — already built, so there is nothing to materialize. Stated as its
   own step rather than folded into the mint because it is the one case where the realm precedes the proxy. */
static void proxy_adopt_realm(JSContext *ctx, JSValueConst proxy, JSContext *realm)
{
    ProxyData *p = JS_GetOpaque(proxy, g_proxy_class);

    (void)ctx;
    DCHECK(p != NULL, "a realm was handed to something that is not a WindowProxy");
    DCHECK(p->realm == NULL, "a navigable was given a second realm through the ADOPT path — it already has an "
                             "active document, and replacing one is window_proxy_navigate, which moves all "
                             "five facts of the binding at once rather than handing this one a second realm");
    p->realm  = realm;   /* BORROWED — the agent owns its realms */
    p->window = JS_GetGlobalObject(realm);
}

JSValue window_proxy_new(JSContext *ctx, uint32_t doc, const char *url, const char *origin, const char *name,
                         bool is_popup, const char *creator_csp, const char *top_level_url,
                         JSValueConst parent, JSValueConst opener)
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
    p->window = JS_UNDEFINED;   /* materialized by proxy_realm — at creation, or on the first read */
    p->realm  = NULL;
    p->url    = url ? proxy_strdup(url) : NULL;   /* NULL only for the self proxy, whose realm is already built */
    p->origin = proxy_strdup(origin ? origin : "null");
    p->name   = name && *name ? proxy_strdup(name) : NULL;
    /* §7.4 NAMED IT. This mint is the one §7.4's create-a-new-navigable reaches, so the name it was given is
       the navigable's name — including the empty one a `open(url)` with no target gives. */
    p->name_known = 1;
    p->is_popup = is_popup ? 1 : 0;
    p->creator_csp = creator_csp && *creator_csp ? proxy_strdup(creator_csp) : NULL;
    /* EVERY ENVIRONMENT HAS A TOP-LEVEL CREATION URL, so unlike the policy container there is no "none" here
       to spell: §7.4 either nests the navigable (inherit the creator's) or makes it a top-level traversable
       (its own address), and both are addresses. A caller with nothing to pass has not decided which of the
       two it is building, and every member gated on §8.1.3.5 would then be installed on a guess. */
    DCHECK(top_level_url != NULL && *top_level_url,
           "a navigable was created with no TOP-LEVEL CREATION URL — HTML §8.1.3.5 reads it to decide whether "
           "the documents of this navigable are SECURE CONTEXTS, and §7.4 says which url it is: the creator's "
           "for a nested navigable, this navigable's own address for an auxiliary one");
    p->top_level_url = proxy_strdup(top_level_url);
    p->parent = JS_DupValue(ctx, parent);
    p->opener = JS_DupValue(ctx, opener);
    p->doc = doc;
    JS_SetOpaque(obj, p);
    return obj;
}

/* §7.2.5.1's proxy for the REALM THAT IS ASKING — the one `window`, `self` and `e.source` are. Its realm is
   this one, which the caller is standing in rather than creating: the two differences from a §7.4 child are
   that the realm is handed over from the outside, and that nobody here stated the navigable's name. */
JSValue window_proxy_new_self(JSContext *ctx, uint32_t doc, const char *name)
{
    /* THE ORIGIN IS THE AGENT'S, and it is read from where §7.2.5.1's check reads it rather than passed in —
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
    JSValue obj;
    ProxyData *p;

    CHECK(tlus != NULL, "the realm's top-level creation URL would not convert to a C string");
    obj = window_proxy_new(ctx, doc, NULL, g_local_origin, name, false, NULL, tlus, JS_UNDEFINED, JS_NULL);
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


JSValue window_proxy_new_remote(JSContext *ctx, uint32_t doc, const char *origin, const char *name,
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
    p->origin = proxy_strdup(origin ? origin : "null");
    p->name   = name && *name ? proxy_strdup(name) : NULL;
    p->name_known = 1;   /* §7.4 named it, in this instance, before the notice crossed */
    p->parent = JS_DupValue(ctx, parent);
    p->opener = JS_DupValue(ctx, opener);
    p->doc = doc;
    JS_SetOpaque(obj, p);
    return obj;
}

/* §7.2.5's `closed`, READ AND WRITTEN THROUGH THE ONE PLACE IT LIVES. It is a fact about the NAVIGABLE, so
   `window.closed` and `iframe.contentWindow.closed` must be the same answer — they were two bytes, and closing
   through one left the other reporting open. Captured through proxy_of, so the flow that closed the window is
   the only one whose timeline contains it.
   IT IS AN OR OVER TWO FLAGS, which is the getter's own wording rather than a convenience: the two are set by
   two different algorithms at two different times, and every read in this file goes through here so that no
   member can accidentally ask only one of them. */
bool window_proxy_closed(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);
    (void)ctx;
    DCHECK(p != NULL, "the closed state of something that is not a WindowProxy was read");
    return wp_closed(p);
}

/* §7.5.10 STEP 8's BROWSING CONTEXT IS NULL — the completion of a destruction, written only by the destroy
   job. It is also what §7.5.10 step 5's wait reads off each child, which is why it is asked separately from
   `closed`: a navigable whose top-level traversable is merely CLOSING has not been destroyed, and a wait that
   accepted `closed` would finish before its subtree had. */
void window_proxy_set_destroyed(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);
    (void)ctx;
    DCHECK(p != NULL, "something that is not a WindowProxy had its browsing context set to null");
    p->destroyed = 1;
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

/* §7.2.5.2 step 3's and §7.3's step 1's test — IS CLOSING alone, which is the half of `closed` that says a
   close is under way rather than finished. Through proxy_of like every other read of this record: the flow
   that called close() is the only one whose timeline contains it. */
bool window_proxy_closing(JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);
    DCHECK(p != NULL, "the is-closing state of something that is not a WindowProxy was read");
    return p->closing != 0;
}

/* §7.3.2.2's FAMILIAR WITH — see window_proxy.h. A is the INCUMBENT realm's browsing context, which is this
   agent's, so every "A's active document's origin" below is g_local_origin and every "is A" is this realm's
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
   incumbent's, and the comparison is then §7.2.5.1's one rule: an OPAQUE origin is same origin with nothing,
   not even with itself, so a sandboxed document is not familiar with its own browsing context either. */
static bool link_same_origin_as_incumbent(JSContext *ctx, JSValueConst v)
{
    ProxyData *q = JS_GetOpaque(v, g_proxy_class);

    if (q) return proxy_same_origin(q);
    if (!proxy_is_incumbent_window(ctx, v)) return false;
    return strcmp(g_local_origin, "null") != 0;
}

bool window_proxy_familiar_with(JSContext *ctx, JSValueConst proxy)
{
    JSValue b = JS_DupValue(ctx, proxy);
    bool ok = false;

    DCHECK(window_proxy_is(proxy), "§7.3.2.2 was asked about something that is not a navigable's WindowProxy");
    /* THE LOOP IS STEP 3's RECURSION, UNROLLED — "B is an auxiliary browsing context and A is familiar with
       B's opener browsing context" is the same question about a different B, and an opener link is fixed when
       the navigable is created (§7.2.5's setter only ever SEVERS one), so the chain is finite and acyclic:
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

void window_proxy_disown_opener(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);   /* the capture is in the accessor — this write rides the flow's delta */

    DCHECK(p != NULL, "something that is not a WindowProxy was asked to disown its opener");
    JS_FreeValue(ctx, p->opener);
    p->opener = JS_NULL;
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

const char *window_proxy_name(JSValueConst proxy)
{
    ProxyData *p = JS_GetOpaque(proxy, g_proxy_class);
    DCHECK(p != NULL, "the name of something that is not a WindowProxy was asked for");
    /* A DESTROYED navigable has no name — the same answer the `name` getter gives, from the same two fields,
       so named access cannot find a frame the page has already removed. */
    return wp_closed(p) || !p->name ? "" : p->name;
}

/* §7.11's `name`, AS A VALUE, and the ONE place it is computed. A Window and its WindowProxy are two spellings
   of one navigable, so `window.name` inside a document and `w.name` from its opener are one attribute of one
   record — they were two, computed from two unrelated sources: the proxy answered the navigable's name and the
   global answered a source-only concolic with no example, so `w = open(u,"chan42")` gave "chan42" through the
   proxy and an example-free unknown inside the popup. HTML §7.11 is unambiguous — "return the current name of
   this's navigable" — and the popup's own script reading it is the ordinary case, not a second attribute.
   IT IS STILL AN ATTACKER SOURCE where the name is genuinely unknown, which is not "always": a navigable §7.4
   created carries the name §7.4 gave it, and the code that wrote `open(url, "chan42")` DETERMINED it, so
   reporting it as unknown would be inventing uncertainty the program does not have. The navigable nobody here
   created is the one whose name a cross-origin opener may have set, and that read is concolic — carrying the
   spec's "" as its example, so it forks control flow without losing the value. */
JSValue window_proxy_name_value(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);

    DCHECK(p != NULL, "the name of something that is not a WindowProxy was read");
    /* §7.2.5: a destroyed navigable has no name, and the destruction is what determined that — the spec files
       assert the empty string rather than the name it had. */
    if (wp_closed(p)) return JS_NewStringLen(ctx, "", 0);
    if (p->name_known) return JS_NewString(ctx, p->name ? p->name : "");
    return concolic_new(ctx, "{window.name}", "window.name", JS_NewStringLen(ctx, "", 0));
}

/* §7.11's `name` SETTER, and the ONE place it is written. It renames the BROWSING CONTEXT, which is why
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

/* §7.4's popup decision, read back — what §7.2.5.3's six BarProps answer from. A DESTROYED navigable keeps
   the answer it was created with: `closed` is what a page checks, and a bar that changed its mind on close
   would be a second fact where there is one. */
bool window_proxy_is_popup(JSValueConst proxy)
{
    ProxyData *p = JS_GetOpaque(proxy, g_proxy_class);
    DCHECK(p != NULL, "the popup state of something that is not a WindowProxy was asked for");
    return p->is_popup != 0;
}

const char *window_proxy_origin(JSValueConst proxy)
{
    ProxyData *p = JS_GetOpaque(proxy, g_proxy_class);
    DCHECK(p != NULL, "the origin of something that is not a WindowProxy was asked for");
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


/* §7.2.5.1's MEMBER SURFACE, and the one distinction that decides how each member is answered.
 *
 * A NAVIGABLE IS NOT ITS ACTIVE DOCUMENT. That sentence is the reason this interface exists at all — a proxy
 * outlives the documents in it — and it is also the reason most of the surface below never leaves this
 * instance. `closed`, `name`, `opener`, `parent`, `top` and the four self-references are properties of the
 * NAVIGABLE, and a navigable belongs to the instance that CREATED it: this one made the child, named it, knows
 * what it was nested in and knows whether it has been destroyed. Only a member that reads through to the
 * ACTIVE DOCUMENT — `length` counts the child's own child navigables — is a question for the peer.
 *
 * THAT SPLIT WAS THE WHOLE BUG. Every member here used to be a host request, so an iframe's `contentWindow`
 * could answer nothing at all until a host could run a second document; `otherW.self === otherW` — which is
 * true by definition, in any browser, about a navigable nobody has to look inside — parked its flow. Answering
 * the navigable's own state locally is not an optimisation, it is where the state actually is.
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
    /* §7.2.5's four names for THIS navigable's own proxy. `window`, `self` and `frames` are on §7.2.5.1's
       cross-origin whitelist; `globalThis` is the global object, which IS the proxy, and is same-origin only —
       the distinction costs nothing here because both answers are the same object. */
    WP_WINDOW, WP_SELF, WP_FRAMES, WP_GLOBALTHIS,
    WP_PARENT, WP_TOP, WP_OPENER, WP_CLOSED, WP_NAME,
    WP_LENGTH,   /* the ACTIVE DOCUMENT's — the one member below that leaves this instance */
    /* §7.2.5's `document`, SAME-ORIGIN ONLY. It answers with the OTHER realm's Document OBJECT, and it can
       always do so in this turn: an agent is ORIGIN-KEYED, so a same-origin navigable is a realm of this agent
       by construction and a proxy this agent cannot answer for is cross-origin, where the answer is a
       SecurityError rather than a document. That pairing is asserted rather than assumed — see proxy_realm. */
    WP_DOCUMENT,
    /* §7.2.5's `location` — the LOCATION OBJECT OF THE ACTIVE DOCUMENT, and the first thing a page does with a
       popup it just opened. It was missing entirely, so `w.location.pathname` read a property of undefined: 63
       subtests in html/browsers failed on that one line, all of them after §7.4 started running popups.
       IT IS ON §7.2.5.1's CROSS-ORIGIN LIST, which is what makes it different from `document` — the PROPERTY
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
/* §7.2.5.1's CROSS-ORIGIN PROPERTY NAMES — the fixed list a WindowProxy exposes whatever the origins are:
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
    false, /* globalThis — the global OBJECT, and §7.2.5.1 does not list it */
    true,  /* parent     */ true,  /* top        */ true,  /* opener     */
    true,  /* closed     */
    false, /* name — a browsing context's name is NOT on the list; a cross-origin read of it is a SecurityError */
    true,  /* length     */
    false, /* document — §7.2.5.1 does not list it, so a cross-origin read is a SecurityError */
    true,  /* location — §7.2.5.1 DOES list it; the filtering is the Location object's own, not this table's */
};

/* §7.2.5.1: a read the origins do not permit is a SecurityError, and it is thrown at the READ rather than
   answered with undefined — a page distinguishes the two, and undefined would say "this window has no such
   member" about one it cannot see. */
static bool proxy_read_permitted(const ProxyData *p, int magic)
{
    return PROXY_CROSS_ORIGIN[magic] || proxy_same_origin(p);
}

/* §7.2.5's `top`: the TOP-LEVEL traversable's proxy. Walked rather than stored, because a navigable's parent
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
        /* THE CHAIN IS PROXIES ALL THE WAY UP, because §7.2.5 says `parent` IS one. It used to hold the
           creator's GLOBAL, so this walk left the proxies at the top and read `top` off a Window — a
           scriptable property read from a C activation, which is the one thing this interpreter refuses the
           moment that property stops being a frozen value. Storing what the spec says deletes the branch. */
        DCHECK(window_proxy_is(q->parent),
               "a navigable's parent is not a WindowProxy — §7.2.5 says it is one, and a walk that has to ask "
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
    if (JS_VALUE_GET_PTR(v) == JS_VALUE_GET_PTR(document_window_proxy(ctx))) {
        JS_FreeValue(ctx, v);
        return JS_GetGlobalObject(ctx);
    }
    return v;
}

static JSValue proxy_member_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    ProxyData *p = proxy_of(this_val);   /* CAPTURED: `closed` and `name` are per-flow state a read may precede */

    DCHECK(magic >= 0 && magic < WP_MEMBER_N,
           "a WindowProxy member was declared with a magic this component does not name");
    if (!p) return JS_UNDEFINED;

    /* A LOCAL PROXY CAN STILL BE CROSS-ORIGIN — this agent is one ORIGIN, but a proxy it holds may name a
       navigable it navigated elsewhere — so §7.2.5.1's check comes first either way. */
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
        return JS_DupValue(ctx, this_val);
    case WP_PARENT:
        /* §7.2.5: the parent navigable's proxy, or THIS one when there is no parent. */
        return win_or_proxy(ctx, JS_IsUndefined(p->parent) ? JS_DupValue(ctx, this_val)
                                                           : JS_DupValue(ctx, p->parent));
    case WP_TOP:
        return win_or_proxy(ctx, window_proxy_top_navigable(ctx, this_val));
    case WP_OPENER:
        return win_or_proxy(ctx, JS_DupValue(ctx, p->opener));
    case WP_CLOSED:
        return JS_NewBool(ctx, wp_closed(p));
    case WP_NAME:
        return window_proxy_name_value(ctx, this_val);
    case WP_DOCUMENT:
        /* §7.2.5's `document`, and reaching it means the read was PERMITTED — which for a same-origin-only
           member means the origins match, which in an origin-keyed agent means this agent holds the realm. So
           the answer is that realm's own Document object, handed back in this turn: two same-origin documents
           are one heap and the corpus appends nodes across exactly this edge. A destroyed navigable has no
           active document. */
        if (wp_closed(p)) return JS_NULL;
        return JS_DupValue(ctx, document_object(proxy_realm(ctx, this_val, p)));
    case WP_LOCATION:
        /* §7.2.5.1: a navigable has ONE Location, and it is the ACTIVE DOCUMENT's — so it is read off that
           document's realm, exactly as `document` is. A destroyed navigable has no active document and so no
           Location; the spec files read `closed` before touching one, and answering with the address it used
           to have would be a document that no longer exists.
           SAME-ORIGIN ONLY, SO FAR. The cross-origin half is REAL — §7.2.5.1 puts `location` on the list
           precisely so a cross-origin document can be navigated through it — but it needs a Location whose own
           members are filtered to `href`'s setter and `replace`, over an object in another instance. Handing
           back THIS document's Location instead would be a cross-origin read that silently succeeded, which is
           the one failure the check exists to prevent, so proxy_realm's assert stops here and names it. */
        if (wp_closed(p)) return JS_NULL;
        /* READ OFF THE REALM, not off its global. `location` is an IDL accessor now — §7.2.2 declares it
           `[LegacyUnforgeable] readonly attribute Location`, so it is a non-configurable accessor on the
           global — and a JS_GetPropertyStr that reaches a getter aborts, because a C activation has no flow
           base under it. location.h's location_object is the same answer without the property read, exactly as
           document_object is above. */
        return location_object(proxy_realm(ctx, this_val, p));
    default:
        DFAIL("a WindowProxy member with no navigable-own answer reached this switch — WP_LENGTH and anything "
              "added beside it read the ACTIVE DOCUMENT and are declared on the step machine below");
        return JS_UNDEFINED;
    }
}

/* §7.2.5's `parent`, `top` and `opener` ARE THE NAVIGABLE'S, and these are how a Window answers them. A Window
   and its WindowProxy are two spellings of one navigable, so the two must be one answer from one record — the
   same unification `closed` already has. window.c answered all three with FIXED values behind two comments
   saying no embedder could exist and nothing had opened this document; both were true exactly while one
   instance was one document. */
JSValue window_proxy_parent(JSContext *ctx, JSValueConst proxy)
{
    return proxy_member_get(ctx, proxy, WP_PARENT);
}

/* THE NAVIGABLE THIS ONE IS NESTED IN, FOR AN ENGINE WALK — the same distinction window_proxy_top_navigable
   draws, and for the same reason. §7.2.5's `parent` answers THIS proxy when there is no parent, because
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

JSValue window_proxy_top_of(JSContext *ctx, JSValueConst proxy)
{
    return proxy_member_get(ctx, proxy, WP_TOP);
}

JSValue window_proxy_opener(JSContext *ctx, JSValueConst proxy)
{
    return proxy_member_get(ctx, proxy, WP_OPENER);
}

/* §7.2.5.1's WRITE SIDE of `name` — the origin check, which is the whole of what this member adds over the
   write itself: `name` is not a cross-origin property, so setting it across origins is a SecurityError rather
   than a silent no-op. The rename is window_proxy_name_assign's, because a Window and its proxy write the one
   navigable's one name. */
static JSValue proxy_name_set(JSContext *ctx, JSValueConst this_val, JSValueConst v, int magic)
{
    ProxyData *p = proxy_of(this_val);

    (void)magic;
    if (!p) return JS_UNDEFINED;
    if (!proxy_read_permitted(p, WP_NAME))
        return JS_ThrowDOMException(ctx, "SecurityError",
                                    "the origins do not permit setting the name of that Window");
    return window_proxy_name_assign(ctx, this_val, v);
}

/* §7.2.5.2's `close()`. THE WHOLE METHOD IS document_lifecycle.c's, and this member is one of its two
   spellings: `w.close()` from an opener and `window.close()` inside the popup are one method on one navigable,
   and each carried a body of its own that stopped at step 3 — is closing went true and nothing else happened,
   so `closed` reported a closed window over a document that was still running its timers, still holding its
   subframes, and that had never fired a beforeunload or an unload listener. Steps 4-6 are the rest of it.
   The navigable is this agent's whether or not its active document is, so there is nothing to delegate and
   nothing to ask a peer: `this_val` IS thisTraversable, and `ctx` is the INCUMBENT realm step 6 asks about. */
static JSValue proxy_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)argc; (void)argv; (void)magic;
    if (!window_proxy_is(this_val)) return JS_UNDEFINED;
    document_lifecycle_window_close(ctx, this_val);
    return JS_UNDEFINED;
}

/* IS THIS NAVIGABLE A TOP-LEVEL TRAVERSABLE — one fact, asked from one place. §7.2.5.2's close() returns early
   for anything else and §7.3's is-closing is only ever set on one, and both spellings of close() (the Window
   member and the proxy's) have to make the SAME test or one of them closes something the other would not. */
bool window_proxy_is_top_level(JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);
    DCHECK(p != NULL, "something that is not a WindowProxy was asked whether it is a top-level traversable");
    return JS_IsUndefined(p->parent);
}

/* §7.2.5.2's IS CLOSING, for the Window spelling of `close()` — window.c's member and the proxy's are the one
   method on the one navigable, so they write the one flag through here. §7.2.5.2 RETURNS EARLY for a navigable
   that is not a top-level traversable, and that early return is the method's, not this setter's: a caller that
   has already made that test states it, and one that has not would otherwise silently mark a frame closing. */
void window_proxy_set_closing(JSContext *ctx, JSValueConst proxy)
{
    ProxyData *p = proxy_of(proxy);
    (void)ctx;
    DCHECK(p != NULL, "something that is not a WindowProxy was closed");
    DCHECK(JS_IsUndefined(p->parent),
           "§7.2.5.2's is closing was set on a navigable that is not a TOP-LEVEL traversable — the flag is only "
           "ever true for one, and close() returns early for anything else");
    p->closing = 1;
}

typedef struct { uint32_t req; } ProxyGetState;

static void proxy_get_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

/* WHERE THIS MACHINE RESTS. §7.2.5.1 answers each cross-origin member from the OTHER navigable's active
   document, so the read is one step of the standard performed in another instance — and the wait for that
   peer is a sub-sequence inside it (`req` is its cursor), not a step of its own. One stage: a flow parked
   here is parked at the read it made, whichever member it asked for. */
#define PROXY_GET_STAGES(X) \
    X(PROXY_GET_ASK = IDL_STEP_FIRST, \
      "HTML §7.2.5.1 (the cross-origin member's value, resolved by the instance that holds the active " \
      "document — the flow suspends on the line that made the read and resumes with the answer)")
enum { PROXY_GET_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const PROXY_GET_STEPS[] = { PROXY_GET_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int proxy_get_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                          JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    ProxyGetState *s = st;
    JSValueConst answer;
    int magic = idl_step_magic(hdr);
    ProxyData *p;

    (void)argc; (void)argv; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(magic >= 0 && magic < WP_MEMBER_N,
           "a WindowProxy member was declared with a magic this component does not name");

    p = JS_GetOpaque(hdr->this_val, g_proxy_class);
    if (!p) { *presult = JS_UNDEFINED; return JS_STEP_DONE; }
    if (!proxy_read_permitted(p, magic)) {
        JS_ThrowDOMException(ctx, "SecurityError",
                             "the origins do not permit reading this member of that Window");
        return JS_STEP_ABRUPT;
    }

    /* THE HOSTED CASE, ANSWERED IN THIS TURN AND OUT OF THE TARGET'S OWN DOCUMENT. §7.2.5.1's `length` is the
       child-navigable count of the ACTIVE DOCUMENT, so it is a walk of THAT document's tree — the reading
       realm would count this document's frames and call them the other's.
       A NAVIGABLE WHOSE REALM HAS NOT BEEN MATERIALIZED COUNTS ZERO, and that is the computed answer rather
       than a stand-in for one: its active document is the empty about:blank Document §7.4 created, an element
       can only get into it by script, and script cannot run in a realm that does not exist. Building a whole
       platform to count the iframes in a document that provably has none is what made this read cost a realm
       per flow — 4000 flows in and the frontier hit the RAM floor at ~57% of the depth it reaches without it. */
    if (world_doc_hosted(p->doc)) {
        DCHECK(magic == WP_LENGTH, "a navigable-own member reached the step machine — it is answered by "
                                   "proxy_member_get, in this turn, with no host round trip");
        *presult = JS_NewInt32(ctx, p->realm ? iframe_child_navigable_count(p->realm) : 0);
        return JS_STEP_DONE;
    }
    /* A DESTROYED NAVIGABLE HAS NO ACTIVE DOCUMENT to ask, and §7.2.5 answers for it here rather than parking
       a flow on an instance the host has already torn down. */
    if (wp_closed(p)) { *presult = JS_NewInt32(ctx, 0); return JS_STEP_DONE; }

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
        n += world_serialize(f->world, op + n, sizeof op - (size_t)n);
        snprintf(op + n, sizeof op - (size_t)n, "\t%s", PROXY_MEMBER[magic]);
        s->req = engine_host_request(ctx, op);
        return JS_STEP_YIELD;   /* park; siblings run until the peer answers */
    }
    if (!engine_host_answered(s->req, &answer))
        return JS_STEP_YIELD;
    /* THE PEER'S COMPLETION, NOT ITS VALUE. §7.2.5.1's member is an IDL getter the peer RUNS, so it can throw
       — a SecurityError from a member outside the cross-origin list, or the page's own accessor — and the
       throw belongs at the line that read the member. */
    {
        int r = engine_host_take_completion(ctx, s->req, presult);
        s->req = 0;
        return r;
    }
}

static const IdlStepDecl PROXY_GET_DECL = { proxy_get_step, sizeof(ProxyGetState), proxy_get_visit, NULL,
                                           "HTML §7.2.5.1 a cross-origin WindowProxy member's value",
                                           PROXY_GET_STEPS };

/* §7.2.5.1's MEMBER SURFACE FOR ONE REALM. Declaration order in core/realm.h's list matters here in one
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
    for (i = 0; i < WP_MEMBER_N; i++) {
        if (i == WP_LENGTH)
            idl_install_accessor_step(ctx, proto, PROXY_MEMBER[i], g_wp_len_getter_id, -1);
        else if (i == WP_NAME)
            idl_install_accessor(ctx, proto, PROXY_MEMBER[i], proxy_member_get, i, g_wp_name_setter_id);
        else
            idl_install_accessor(ctx, proto, PROXY_MEMBER[i], proxy_member_get, i, -1);
    }
    idl_install_method(ctx, proto, "close", 0, g_wp_close_id);
    JS_SetClassProto(ctx, g_proxy_class, proto);
}

void window_proxy_init(JSContext *ctx, const char *origin)
{
    JSClassDef d = { "WindowProxy", .finalizer = proxy_finalizer, .gc_mark = proxy_gc_mark };
    JSRuntime *rt = JS_GetRuntime(ctx);

    DCHECK(g_wp_rt == NULL || g_wp_rt == rt, "WindowProxy was installed into a second runtime");
    if (g_wp_rt == rt) return;
    g_wp_rt = rt;
    free(g_local_origin);
    g_local_origin = strdup(origin ? origin : "null");
    CHECK(g_local_origin != NULL, "window proxy: OOM recording this document's origin");
    JS_NewClassID(rt, &g_proxy_class);
    JS_NewClass(rt, g_proxy_class, &d);
    /* THE POOL ENTRIES ARE THE AGENT'S — a declaration is a runtime registration, and every realm's members
       carry the same ids. Only the OBJECTS below are per realm. */
    g_wp_len_getter_id  = idl_getter_id_step(ctx, &PROXY_GET_DECL, WP_LENGTH);
    g_wp_name_setter_id = idl_setter_id(ctx, IDL_DOMSTRING, false, proxy_name_set, WP_NAME);
    g_wp_close_id       = idl_method_id(ctx, NULL, 0, proxy_close, 0);
    realm_declare_intrinsic(window_proxy_install_proto);
}

JSValue window_proxy_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_proxy_class);
    DCHECK(!JS_IsNull(proto),
           "the WindowProxy prototype was asked for in a realm that never ran window_proxy_install_proto");
    return proto;   /* OWNED */
}

void window_proxy_free(JSContext *ctx)
{
    OwnedStr *e = g_strings;
    if (!g_wp_rt) return;
    while (e) { OwnedStr *n = e->next; free(e->s); free(e); e = n; }
    g_strings = NULL;
    /* the prototypes are the REALMS' — released with their contexts */
    free(g_local_origin);
    g_local_origin = NULL;
    g_wp_rt = NULL;
}
