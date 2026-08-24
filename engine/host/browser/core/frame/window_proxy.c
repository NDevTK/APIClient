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

/* §7.2.3's [[Window]] and the origin its same-origin check reads. BOTH are per-flow — see the file comment —
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
    /* §7.2.6's POLICY CONTAINER THE CREATOR HANDED THIS NAVIGABLE, taken at CREATION and kept as text because
       that is what §7.4's "clone a policy container" is (policy_container_clone re-parses this same string).
       It is here rather than read off whichever realm materializes the document because a navigable created
       with no address is materialized LAZILY, on the first read that reaches through it — and the reader may
       be a different same-origin document with a different policy, which would give the child whichever
       document happened to touch it first. A creation fact, so like `is_popup` no flow can change it.
       NULL for a navigable with an address (its policy comes with its response) and for the root. Owned. */
    char   *creator_csp;
    /* CSP §2.2's SELF-ORIGIN of that cloned list, SERIALIZED — the other half of one CSP list, and the half
       the text does not contain. It is the CREATOR's origin and not this navigable's: §2.2's note says the
       field exists precisely so that a document which INHERITED its policy resolves `'self'` against the
       origin the policy came from, and the initial about:blank is that document. A creation fact like the
       policy beside it, so like `is_popup` there is nothing here for the delta to capture. Owned. */
    char   *creator_csp_self_origin;
    /* HTML §7.4's `creatorBaseURL` — "if creator is non-null, set creatorBaseURL to creator's DOCUMENT BASE
       URL", which the create-a-new-browsing-context-and-document steps hand the initial `about:blank` as its
       ABOUT BASE URL. Without it a relative URL inside a srcless frame or a bare `open()` resolves against
       `about:blank`, which has an opaque path and cannot be a base, so the parse FAILS outright.
       IT IS TAKEN AT CREATION, for exactly the reason `creator_csp` above is: the Document is materialized
       lazily, by whichever same-origin document reads through this navigable first, and that reader's base
       URL is not the creator's. It is also the CREATOR's base URL and not the creator's ADDRESS — a creator
       carrying `<base href>` passes on the base, which is the whole content of the word in §7.4.
       NULL for the root navigable (nothing created it) and for a navigable created with a real address (its
       Document comes from a response and §2.4.3's about base URL is null for it). Owned. */
    char   *creator_base_url;
    /* §7.1.5's DETERMINE THE CREATION SANDBOXING FLAGS for this navigable, taken at CREATION for exactly the
       reason `creator_csp` above is: §7.2 hands this set to the initial about:blank Document as its ACTIVE
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
       sandboxing, workers, and worklets are involved." §7.3.2.1 states them in one breath and answers them
       differently — the URL is `about:blank` for a top-level browsing context while the origin is the initial
       Document's own — and §4.7 over a URL cannot express the third case at all, because §7.3.1 hands ONE
       opaque origin to several Documents and an opaque origin has no serialization it can be recreated from.
       Permissions §5.1 step 5 reads THIS one: its key is generated from "settings's top-level origin", and
       §3.2 compares two keys with §7.1.1's same origin, whose step 1 is an identity comparison.
       BORROWED and POD, exactly like `origin` beside it: an origin lives for the agent, a navigation REPLACES
       this pointer, and the byte capture in proxy_of is therefore a complete description of the binding. */
    const Origin *top_level_origin;
    /* §7.2.2.1's `closed` IS TWO FACTS AND THE GETTER IS THEIR OR — "true if this's browsing context is null or
       its is closing is true". They are two because they happen at two TIMES. `close()` sets is-closing at its
       own call site and QUEUES the destruction; §7.3.1's removal of a container queues one without setting
       is-closing at all; and the browsing context does not become null until §7.5.10 step 8 runs, in a task.
       Held as ONE byte those two times collapsed into the removal's, so a frame's WindowProxy reported a
       destruction that had not happened — while its Document, its queued tasks and its entangled ports were
       all still live, with nothing anywhere holding the fact that they had not been dealt with. */
    /* HAS THIS NAVIGABLE EVER BEEN NAVIGATED — HTML §7.4.4 step 4's "document's IS INITIAL about:blank", read
       from the navigable's side, which is the side that can answer it. §7.4 creates EVERY navigable with the
       initial about:blank Document, and the only thing that ever replaces a navigable's active document is a
       navigation — window_proxy_navigate below, the one site — so a navigable that has not been navigated is
       still showing the Document §7.4 created it with. The alternative, testing the document's ADDRESS, is
       WRONG in this tree and not merely imprecise: navigable.c's load job navigates to `about:blank` for real
       ("the corpus does it while an initial load is still pending"), and that document is not the initial one.
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
   the window it can no longer reach as open, and the engine would model a page real Chrome has cut off. */
static bool wp_bc_null(const ProxyData *p) { return p->destroyed != 0 || p->bc_discarded != 0; }
static bool wp_closed(const ProxyData *p) { return p->closing != 0 || wp_bc_null(p); }

static JSClassID g_proxy_class;
static JSRuntime *g_wp_rt;
/* §7.2.3's member surface. A WindowProxy has no own properties: it FORWARDS to its navigable, answering from
   the navigable's own state where that is what the member is, and suspending on the host where the member reads
   the active document. See the member table lower down for which is which.
   IT IS PER REALM, in quickjs's own class-proto slot, and that is an ANSWER and not an identity: a member runs
   in the realm that DEFINED it, so one shared object would answer `parent`, `name` and `close()` for every
   document out of whichever realm built it first — and the whole point of these members is that they read the
   asking document's side of a boundary. `a.postMessage === b.postMessage` still holds for two proxies of the
   same realm, which is what §7.2.3's shared surface means; two REALMS have two, exactly as two realms have
   two `Array.prototype.map`s. */
/* TWO GETTER IDS OVER ONE STEP DECLARATION, and they are two because a pool entry carries the MEMBER MAGIC —
   the step body reads it back with idl_step_magic to know which question it was asked. */
static int g_wp_len_getter_id = -1, g_wp_closed_getter_id = -1;
static int g_wp_name_setter_id = -1, g_wp_close_id = -1;


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

/* §7.2.1's SAME-ORIGIN CHECK — §7.1.1's algorithm over two RECORDS, which is the whole of it: step 1 is the
 * nonce comparison and step 2 is the tuple comparison, both inside origin_same.
 *
 * IT USED TO BE A STRING COMPARE, AND THAT COULD NOT RUN STEP 1. "If A and B are the same opaque origin, then
 * return true" is an IDENTITY comparison, and identity is exactly what §7.1.1's serializer drops — every opaque
 * origin serializes to "null" — so this component could not tell one opaque origin looked at twice from two
 * distinct ones and guessed "two". That is right for two sandboxed frames and wrong for the case step 1 exists
 * for: §7.3.1's determine-the-origin hands ONE opaque origin to several Documents on purpose, so a `data:`
 * document's `about:blank` child was refused every member of its own navigable outside the fixed cross-origin
 * list, and its `contentDocument` was null. The origin is a record now (core/url/origin.h) and the guess is
 * gone with the assert that named it.
 *
 * THE ACCESSOR SIDE IS THE AGENT'S ORIGIN. An instance is an origin-keyed agent cluster, so the origin of
 * whoever is reading is this agent's, and there is exactly one of those. */
static bool proxy_same_origin(const ProxyData *p)
{
    DCHECK(p->origin != NULL, "a WindowProxy carries no origin — §7.4 gives every navigable's document one and "
                              "§7.3.1 decides which, so a proxy without one was minted somewhere that did not");
    return origin_same(p->origin, origin_agent());
}

/* §7.1.1's SAME ORIGIN-DOMAIN over the same pair — §7.3.1's `content document` filter, which is a DIFFERENT
 * algorithm from the one above and not a laxer spelling of it.
 *
 * AND ITS ACCESSOR SIDE IS THE ASKING DOCUMENT'S ORIGIN, NOT THE AGENT'S. That is the one place the two checks
 * must be written differently, and the reason is §7.1.1.2: every origin in this heap is same ORIGIN with the
 * agent's by construction, so proxy_same_origin may compare against the agent's record and be exact — but a
 * DOMAIN is per Document. §7.3.1's content document filters "container document" against the content document,
 * and a child navigable's Document holds its own tuple record (§7.3.1 step 5 mints one per address), so a
 * child that has relaxed its domain while its container has not is a pair the standard's own table answers
 * ❌ for. Reading the agent's record here would have answered ✅ — right until the moment the member it exists
 * to gate became implementable, which is what makes it worth one argument. */
static bool proxy_same_origin_domain(JSContext *ctx, const ProxyData *p)
{
    DCHECK(p->origin != NULL, "a WindowProxy carries no origin");
    return origin_same_origin_domain(p->origin, window_proxy_origin(document_window_proxy(ctx)));
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

static void proxy_finalizer(JSRuntime *rt, JSValue val)
{
    ProxyData *p = JS_GetOpaque(val, g_proxy_class);
    int i;
    (void)rt;
    /* THE ROW GOES FIRST, and unconditionally: it names this object by POINTER, so a row left behind would
       hand the next ask for that document a freed proxy. Above the `p` test because the table's claim is about
       the OBJECT, not about whether its record survived to be freed here. */
    for (i = 0; i < g_remote_navs_n; i++)
        if (JS_VALUE_GET_PTR(g_remote_navs[i].proxy) == JS_VALUE_GET_PTR(val)) {
            g_remote_navs[i] = g_remote_navs[--g_remote_navs_n];
            break;
        }
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
        /* THE INITIAL about:blank, WHICH CAME FROM NO RESPONSE — so no bytes, and §7.2.6's policy is the clone
           of its creator's this navigable was CREATED with (see creator_csp). Fetching anything here is not
           deferred, it is IMPOSSIBLE: this runs inside the property read that reached through the navigable,
           and a fetch suspends. The document an address serves arrives on the load job instead, which is a
           flow and can park (navigable.c). */
        /* THE ENVIRONMENT'S TOP-LEVEL CREATION URL IS THE NAVIGABLE'S, not the reading realm's — the read that
           materializes an about:blank child may come from any same-origin document, and answering from `ctx`
           would give the child whichever document happened to touch it first. The same sentence as
           `creator_csp` beside it, and the same field for the same reason. */
        /* §7.2's CREATE A NEW BROWSING CONTEXT AND DOCUMENT gives this Document its ACTIVE SANDBOXING FLAG
           SET, and what it gives is `sandboxFlags` — the navigable's CREATION sandboxing flags, with NO
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
                                   p->creator_csp, p->creator_csp_self_origin, p->creator_base_url,
                                   p->creation_sandbox_flags);
        p->window = JS_GetGlobalObject(p->realm);
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
       above ran — so releasing it here leaves the parked arm's copy intact and the live slot free to move. */
    JS_FreeValue(ctx, p->window);
    p->realm  = realm;
    p->window = JS_GetGlobalObject(realm);
    p->doc    = doc;
    /* THE OLD STRINGS ARE NOT FREED. A parked flow's saved bytes still name them (see proxy_of), so freeing
       here would resume that flow onto freed memory; they are the PROXY's and are released with it. */
    p->url    = proxy_strdup(url);
    DCHECK(origin != NULL, "a navigable was navigated to a document with no origin — §7.3.1 answers for every "
                           "Document, and a navigation's answer is the one the LOAD computed, never re-derived "
                           "here from the address (that second answer is what loses an inherited identity)");
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

    (void)ctx;
    DCHECK(p != NULL, "a realm was handed to something that is not a WindowProxy");
    DCHECK(p->realm == NULL, "a navigable was given a second realm through the ADOPT path — it already has an "
                             "active document, and replacing one is window_proxy_navigate, which moves all "
                             "five facts of the binding at once rather than handing this one a second realm");
    p->realm  = realm;   /* BORROWED — the agent owns its realms */
    p->window = JS_GetGlobalObject(realm);
}

JSValue window_proxy_new(JSContext *ctx, uint32_t doc, const char *url, const Origin *origin, const char *name,
                         bool is_popup, SandboxFlags creation_sandbox_flags, OpenerPolicyValue opener_policy,
                         const char *creator_csp,
                         const char *creator_csp_self_origin, const char *creator_base_url,
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
    DCHECK(origin != NULL, "a navigable was created with no origin — §7.3.1's determine-the-origin answers for "
                           "every Document, including the initial about:blank one §7.4 creates a navigable "
                           "with, and the answer is a RECORD whose identity the same-origin check compares");
    p->origin = origin;   /* BORROWED — an origin lives for the agent (core/url/origin.h) */
    p->name   = name && *name ? proxy_strdup(name) : NULL;
    /* §7.4 NAMED IT. This mint is the one §7.4's create-a-new-navigable reaches, so the name it was given is
       the navigable's name — including the empty one a `open(url)` with no target gives. */
    p->name_known = 1;
    p->is_popup = is_popup ? 1 : 0;
    p->creator_csp = creator_csp && *creator_csp ? proxy_strdup(creator_csp) : NULL;
    /* THE SELF-ORIGIN IS REQUIRED EVEN WHERE THE POLICY IS ABSENT, because §2.2 gives a CSP list an origin
       whether or not it holds any policies — and because this navigable's initial Document will be installed
       with it either way. A creator with no CSP still states the origin its (empty) list belongs to. */
    DCHECK(creator_csp_self_origin != NULL && *creator_csp_self_origin,
           "§7.4 created a navigable without the CSP self-origin of the policy container it cloned — the "
           "initial about:blank Document would then resolve `'self'` against nothing, which for an inherited "
           "policy is the one case CSP §2.2's self-origin exists to answer");
    p->creator_csp_self_origin = proxy_strdup(creator_csp_self_origin);
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
    p->doc = doc;
    JS_SetOpaque(obj, p);
    return obj;
}

/* §7.2.3's proxy for the REALM THAT IS ASKING — the one `window`, `self` and `e.source` are. Its realm is
   this one, which the caller is standing in rather than creating: the two differences from a §7.4 child are
   that the realm is handed over from the outside, and that nobody here stated the navigable's name. */
JSValue window_proxy_new_self(JSContext *ctx, uint32_t doc, const char *name, OpenerPolicyValue opener_policy)
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
       environment the HOST built. The URL it built it with is above; the ORIGIN is §7.3.1's determine the
       origin over that URL with THIS AGENT'S origin as the source origin, which is the whole of what an
       origin-keyed agent cluster has to inherit from. That is not a re-derivation dressed up: §7.3.1 IS the
       algorithm that answers "whose origin is a Document at this address", and its two inheritance cases are
       exactly the addresses §4.7 cannot answer for — `about:blank`, which is the address §7.3.2.1 creates
       EVERY top-level browsing context at, and `about:srcdoc`. A host that started this instance at a real
       address gets §4.7's tuple, which is the same origin as this agent's by construction. */
    url_record_init(&rec);
    tlo = url_parse(&rec, tlus, strlen(tlus), NULL) ? origin_determine(&rec, false, origin_agent())
                                                    : origin_determine(NULL, false, NULL);
    url_record_free(&rec);
    /* THE ONE ANSWER §7.3.1 CANNOT REACH FROM A URL, named where it would otherwise be silent. A `data:` or
       `file:` top-level creation URL takes §7.3.1 step 5 to §4.7, which MINTS a new opaque origin — and an
       opaque origin that is not this agent's own record is same origin with nothing, so every environment of
       this instance would be keyed to a value no other environment can ever equal. Which Document that origin
       belongs to is a fact only the zone that created this instance holds. */
    DCHECK(!origin_is_opaque(tlo) || origin_same(tlo, origin_agent()),
           "this instance's root environment has a TOP-LEVEL CREATION URL whose §4.7 origin is OPAQUE and is "
           "not this agent's own, so §7.3.1 minted a second one rather than inheriting: an opaque origin has "
           "no serialization it can be recreated from, and a URL therefore cannot say WHOSE it is. STATE "
           "§8.1.3.1's TOP-LEVEL ORIGIN beside the top-level creation URL from the zone that created this "
           "instance — it is the same statement, made by the same party, for the same reason");
    /* §7.1.5: AN EMPTY CREATION SANDBOXING FLAG SET, and it is the spec's answer rather than a placeholder.
       This is the navigable the INSTANCE STARTED IN — a top-level traversable with no embedder, so
       determine-the-creation-sandboxing-flags returns its POPUP SANDBOXING FLAG SET, which §7.1.5 says is
       empty when a browsing context is created and which only §7.3.1.7 "Navigable target names"'s rules
       for choosing a navigable ever populate. Nothing chose this one. What the ROOT document's own
       `Content-Security-Policy: sandbox` adds
       is the other half of §7.4.5's union, and it is added where a Document is created rather than here. */
    /* NO CREATOR POLICY — this navigable is the one §7.4 did NOT create, so there is nothing to clone — but
       CSP §2.2's SELF-ORIGIN of its document's list is still a real value and is this agent's own: the root
       Document is created from the response at this instance's address, which is §2.2.2's answer. Nothing
       reads it through this proxy (its realm exists from the moment it is adopted, so proxy_realm's lazy
       materialization is unreachable for it), and it is stated all the same because the field is what makes
       "every navigable carries the self-origin of the policy its Document runs under" an invariant rather
       than a case analysis at each reader. */
    /* NO CREATOR BASE URL either, and for the same sentence: §7.4 did not create this navigable, so there is
       no creator whose base URL to pass on. Its Document comes from the response at this instance's address,
       which §2.4.3 gives a null about base URL. */
    obj = window_proxy_new(ctx, doc, NULL, origin_agent(), name, false, 0, opener_policy, NULL,
                           origin_serialized(origin_agent()), NULL, tlus, tlo, JS_UNDEFINED, JS_NULL);
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
    p->doc = doc;
    JS_SetOpaque(obj, p);
    return obj;
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
    if (g_remote_navs_n == g_remote_navs_cap) {
        int cap = g_remote_navs_cap ? g_remote_navs_cap * 2 : 8;
        RemoteNav *g = realloc(g_remote_navs, (size_t)cap * sizeof *g);

        CHECK(g != NULL, "window proxy: OOM recording a remote navigable's identity — an unrecorded navigable "
                         "is minted again on the next ask, and `w[0] === w[0]` is then false about one window");
        g_remote_navs = g;
        g_remote_navs_cap = cap;
    }
    g_remote_navs[g_remote_navs_n].doc = doc;
    g_remote_navs[g_remote_navs_n].proxy = obj;   /* BORROWED — proxy_finalizer takes the row out */
    g_remote_navs_n++;
    return obj;
}

/* §7.2.2.1's `closed` AS THIS AGENT'S RECORD OF IT, read and written through the one place it lives HERE. It
   is a fact about the NAVIGABLE, so `window.closed` and `iframe.contentWindow.closed` must be the same answer
   — they were two bytes, and closing through one left the other reporting open. Captured through proxy_of, so
   the flow that closed the window is the only one whose timeline contains it.
   THIS AGENT'S, AND THE CALLERS ARE WHY THAT IS THE RIGHT QUESTION. Every one of them is an ENGINE WALK over
   navigables this agent holds — §7.3.1's fully-active ancestor chain, §7.1's named-target search, the
   tree-order walk — and each is asking whether to keep walking THIS agent's tree, which is exactly what this
   record answers. The JS-visible member is a different question and has a different answer path: a traversable
   whose active document is in another instance is closed by whichever agent ran §7.2.2.1's close(), so the
   member suspends and asks that one (proxy_get_step). A caller that wants the standard's `closed` about an
   arbitrary navigable must go through the member, because only the member can suspend.
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
    JS_FreeValue(ctx, p->window);
    p->window = JS_UNDEFINED;
    p->realm  = NULL;
    /* AND `materialized` NOW ANSWERS THE TRUTH: this navigable has no active document. proxy_realm asserts the
       other side of that pair — a NULL realm on a DESTROYED navigable means "there is none", never "not yet". */
    DCHECK(p->realm == NULL && JS_IsUndefined(p->window),
           "§7.5.10 step 9 left a destroyed navigable still naming a Document — the realm behind it can then "
           "never be reclaimed, because this reference is the one the collector cannot get past");
}

/* §7.1.3.2 STEP 10's DISCARD — the OTHER writer of "this's browsing context is null", and it releases nothing.
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
    DCHECK(wp_closed(p),
           "a browsing context §7.1.3.2's swap has just discarded still answers §7.2.2.1's `closed` as false — "
           "the getter's \"this's browsing context is null\" has two writers and is reading only one of them");
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
 * of the table above — `close` and `postMessage` are installed as methods by this component and
 * window_message.c, and `focus`/`blur` by core/frame/window.c (§6.6.6's `Window.focus()` through
 * focus_install_window_members, and `blur` beside it). A name on this list is therefore answered by whatever
 * owns it, INCLUDING nothing: a member this engine has not built is `undefined` at the end of the prototype
 * chain, which is the truthful answer for a member the origins permit. The two tables are tied to each other by
 * an assert at capture rather than by a reader keeping them in step.
 *
 * THIS PARAGRAPH SAID `focus`/`blur` WERE "honestly ABSENT", AND THEY ARE NOT: window.c installs both on the
 * Window, and `Object.getOwnPropertyNames(self)` reports them. That is the stale-claim failure CLAUDE.md names
 * — true when written, wrong about this tree — and it was load-bearing rather than decorative, because it is
 * the reason given for the WindowProxy's own surface not having them, which is to say for the two surfaces
 * disagreeing about two members. Measured: the Window owns all thirteen of §7.2.1.3.1's names, this component's
 * prototype owns eleven.
 *
 * IT IS DECLARED HERE AND ASKED ONCE, at the object's own [[GetOwnProperty]], because that is where §7.2.3.5
 * puts it — before the prototype walk, so a name outside the list never reaches a member at all. Asking it
 * per-member is what produced the silence above. */
static const char *const CROSS_ORIGIN_NAME[] = {
    "window", "self", "location", "close", "closed", "focus", "blur", "frames",
    "length", "top", "opener", "parent", "postMessage"
};
#define CROSS_ORIGIN_NAME_N ((int)(sizeof CROSS_ORIGIN_NAME / sizeof CROSS_ORIGIN_NAME[0]))
static JSAtom g_xo_atom[CROSS_ORIGIN_NAME_N];
/* §7.2.1.3.2 step 1's FOUR NAMES, which are the exception to the throw: `then` (so a cross-origin WindowProxy
   is not mistaken for a thenable and awaited), and the three well-known symbols an engine touches while doing
   something else entirely. They answer `undefined` as a real property descriptor rather than by falling off the
   chain, because §7.2.3.10 lists them among the own keys. */
#define XO_FALLBACK_N 4
static JSAtom g_xo_fallback[XO_FALLBACK_N];

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
        g_xo_atom[i] = JS_NewAtom(ctx, CROSS_ORIGIN_NAME[i]);
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
        for (k = 0; k < CROSS_ORIGIN_NAME_N; k++)
            if (!strcmp(PROXY_MEMBER[i], CROSS_ORIGIN_NAME[k])) { listed = true; break; }
        DCHECK(listed == PROXY_CROSS_ORIGIN[i],
               "a WindowProxy member's own cross-origin flag disagrees with §7.2.1.3.1's list of cross-origin "
               "accessible window property names. The list decides whether the read reaches a member at all "
               "(§7.2.3.5 throws before the prototype walk) and the flag decides what the member then answers, "
               "so a disagreement is either a member that can never be read across origins however it is "
               "marked, or one that is reached and then refuses — both of them silent");
    }
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
               So the ANSWER can be sent, and the PEER-SIDE PROGRAM was already built before that:
               `windowproxy.get` runs `globalThis[__apiclientKey]` in the named document's own realm
               (remote_op.c, engine.c's flow_perform), and §7.2.2.2's indexed access on that realm's global IS
               this same step 2 performed there — so the member field carries the decimal index and the peer
               needs nothing new to compute it.
               WHAT IS MISSING IS THE SUSPEND, AND ONLY THAT. This is an exotic [[GetOwnProperty]] — a C hook
               that must return a value — so it cannot park the flow the way `length` does, and `length` is
               only able to because it is an IDL ACCESSOR driven as a step machine. There is exactly one read
               shape this interpreter resolves at the operator site and drives on the trampoline
               (remote_object.c's block comment, and quickjs.c's own DCHECK in tramp_walk_continues names the
               route: the keyed entry's GP_GETOWNPROP). Route this class onto it and step 2 becomes a step
               machine whose ASK stage posts (document, world+ancestry, decimal index) exactly as
               proxy_get_step does and whose ANSWER stage is remote_object_decode of the identity that comes
               back — parked at `otherW[0]`, resumed byte-identical, exactly as an await is. Answering
               `undefined` here is what this hook exists to stop, and the SecurityError below is right only
               once the index is known to be out of range. */
            DFAIL("an INDEXED read of a WindowProxy whose active document is in ANOTHER INSTANCE. Every edge "
                  "around it now exists: the peer computes the answer (`windowproxy.get` with the decimal "
                  "index as the member runs §7.2.2.2's indexed access in that document's own realm) and a "
                  "navigable crosses back as its IDENTITY and resolves to one WindowProxy per document "
                  "(remote_object.c). What is left is the SUSPEND, which an exotic [[GetOwnProperty]] cannot "
                  "do because it is a C hook that must return a value. Route this class onto the keyed "
                  "entry's GP_GETOWNPROP — quickjs.c's tramp_walk_continues names that route in its own "
                  "DCHECK — so step 2 is driven on the trampoline as a step machine, the way every other "
                  "cross-instance read in this engine is");
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
 * its own member sat one link up the chain answering nothing. `focus` and `blur` are the proof it matters in
 * the other direction too: they are on §7.2.1.3.1's list and this component installs NEITHER, so they forward
 * and `otherW.focus()` reaches the Window's own method.
 *
 * WHAT IS STILL MISSING, NAMED: §7.2.3.1's [[GetPrototypeOf]], which for a same-origin W is
 * `W.[[GetPrototypeOf]]()`. There is no exotic prototype hook, so the chain past this object is still the
 * READING realm's WindowProxy prototype and then ITS Object.prototype — the other document's members that are
 * reached only up a chain are not reached at all. Which members those are is window.c's answer and not this
 * one's: Web IDL §3.7.3 makes every member of a [Global] object an OWN property of it (which is why
 * `window.hasOwnProperty("addEventListener")` is true in every browser, and why event_target.c places its
 * three there), and window.c still declares part of Window's surface on Window.prototype instead. So the
 * forward below answers every member that is where the standard puts it, and the two gaps close in either
 * order: a class-level [[GetPrototypeOf]] here, or the [Global] placement there. */

/* Does §7.2.3's own surface answer this name? The PROTOTYPE is that surface — an ordinary object, so the
   query runs none of the page's code and no walk: its OWN properties only, because Object.prototype's members
   are exactly the ones the forward should be answering out of the other realm. */
static bool proxy_surface_owns(JSContext *ctx, JSAtom prop)
{
    JSValue proto = JS_GetClassProto(ctx, g_proxy_class);
    JSPropertyDescriptor d;
    int has;

    DCHECK(JS_IsObject(proto),
           "a WindowProxy property was read in a realm that never ran window_proxy_install_proto — §7.2.3's "
           "surface is the prototype, so a realm without one cannot say which names are its own");
    has = JS_GetOwnSlotDesc(ctx, &d, proto, prop);
    if (has > 0) {
        JS_FreeValue(ctx, d.value);
        JS_FreeValue(ctx, d.getter);
        JS_FreeValue(ctx, d.setter);
    }
    JS_FreeValue(ctx, proto);
    return has > 0;
}

/* W — the ACTIVE DOCUMENT'S Window this operation is performed on, or JS_UNINITIALIZED when the operation is
   this object's own: a cross-origin proxy (§7.2.1 filters its members instead), a name §7.2.3's own surface
   answers, or a destroyed navigable, which has no active document to perform anything on.
   BORROWED — the proxy holds it, and the ProxyData record it lives in rides the COW delta, so the answer is the
   RUNNING FLOW's: an arm that navigated the frame forwards to the Window it navigated to and its sibling to the
   one it did not. */
static JSValueConst proxy_forward_window(JSContext *ctx, JSValueConst obj, ProxyData *p, JSAtom prop)
{
    if (!proxy_same_origin(p)) return JS_UNINITIALIZED;
    if (proxy_surface_owns(ctx, prop)) return JS_UNINITIALIZED;
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

/* WHERE A FORWARDED WRITE LANDS, for the COW delta — see JSClassExoticMethods.forwarded_object. The delta must
   name the Window the define below writes, never this stand-in: an entry naming the proxy restores its baseline
   ONTO the proxy as a real own property, which from then on shadows the Window for every flow. */
static JSValueConst proxy_forwarded_object(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    ProxyData *p = proxy_of(obj);

    if (!p) return JS_UNINITIALIZED;
    return proxy_forward_window(ctx, obj, p, prop);
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

    if (!p) return 0;
    if (!proxy_same_origin(p)) {                                        /* step 3 */
        JS_ThrowDOMException(ctx, "SecurityError",
                             "the origins do not permit defining a member of that Window");
        return -1;
    }
    if (proxy_atom_index(ctx, prop, &idx)) {                            /* step 2.1: FALSE for every index */
        if (flags & JS_PROP_THROW) {
            JS_ThrowTypeError(ctx, "cannot define an indexed property on a WindowProxy");
            return -1;
        }
        return 0;
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
   nothing, which on this object is always: a WindowProxy has no own properties of its own. */
static int proxy_delete(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    ProxyData *p = proxy_of(obj);
    JSValueConst w;
    uint32_t idx;

    if (!p) return 1;
    if (!proxy_same_origin(p)) {                                        /* step 3 */
        JS_ThrowDOMException(ctx, "SecurityError",
                             "the origins do not permit deleting a member of that Window");
        return -1;
    }
    if (proxy_atom_index(ctx, prop, &idx)) return 0;                    /* step 2.1 */
    w = proxy_forward_window(ctx, obj, p, prop);
    if (JS_IsUninitialized(w)) return 1;   /* the surface is the PROTOTYPE's: nothing here to delete, which is true */
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

    if (!p) return 0;
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
    /* Same origin and NOT forwarded means §7.2.3's own surface owns the name (or the navigable is destroyed):
       no own property here, and the prototype's member answers exactly as it always has. */
    if (proxy_same_origin(p)) return 0;

    /* STEPS 4-6, WHICH ONLY A CROSS-ORIGIN W REACHES. A name on the standard's list is answered by whatever
       owns it — including nothing at all, which is the truthful `undefined` for a member the origins permit
       and this engine has not built. */
    for (i = 0; i < CROSS_ORIGIN_NAME_N; i++)
        if (prop == g_xo_atom[i]) return 0;

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

/* THE FOUR OF §7.2.3's INTERNAL METHODS THIS OBJECT ANSWERS, and one declaration about all of them: no page
   code, so the engine's own accessor walks may run them from C with no flow base under them. That claim now
   covers the FORWARD as well as the filter, and it still holds — the Window it forwards to declares the same
   thing of its own hook (window.c's WINDOW_EXOTIC, whose named access is a walk of the document tree), and
   materializing an initial about:blank Document runs no script because there is none to run.
   §7.2.3.10's [[OwnPropertyKeys]] is NOT here: `Object.keys(otherW)` still answers this object's own keys
   rather than the Window's supported indices plus its own — a gap this component had before the forward and
   still has, named here because the forward is what makes the two answers disagree. */
static const JSClassExoticMethods PROXY_EXOTIC = {
    .get_own_property = proxy_get_own,
    .delete_property = proxy_delete,
    .define_own_property = proxy_define_own,
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
        return JS_DupValue(ctx, this_val);
    case WP_PARENT:
        /* §7.2.2.4: the parent navigable's proxy, or THIS one when there is no parent. */
        return win_or_proxy(ctx, JS_IsUndefined(p->parent) ? JS_DupValue(ctx, this_val)
                                                           : JS_DupValue(ctx, p->parent));
    case WP_TOP:
        return win_or_proxy(ctx, window_proxy_top_navigable(ctx, this_val));
    case WP_OPENER:
        return win_or_proxy(ctx, JS_DupValue(ctx, p->opener));
    case WP_NAME:
        return window_proxy_name_value(ctx, this_val);
    case WP_DOCUMENT:
        /* §7.2.2's `document`, and reaching it means the read was PERMITTED — which for a same-origin-only
           member means the origins match, which in an origin-keyed agent means this agent holds the realm. So
           the answer is that realm's own Document object, handed back in this turn: two same-origin documents
           are one heap and the corpus appends nodes across exactly this edge. A destroyed navigable has no
           active document. */
        if (wp_closed(p)) return JS_NULL;
        return JS_DupValue(ctx, document_object(proxy_realm(ctx, this_val, p)));
    case WP_LOCATION:
        /* §7.2.4: a navigable has ONE Location, and it is the ACTIVE DOCUMENT's — so it is read off that
           document's realm, exactly as `document` is. A destroyed navigable has no active document and so no
           Location; the spec files read `closed` before touching one, and answering with the address it used
           to have would be a document that no longer exists.
           SAME-ORIGIN ONLY, SO FAR. The cross-origin half is REAL — §7.2.1 puts `location` on the list
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
    g = JS_GetGlobalObject(ctx);
    is_global = JS_VALUE_GET_PTR(g) == JS_VALUE_GET_PTR(v);
    JS_FreeValue(ctx, g);
    return is_global ? document_window_proxy(ctx) : JS_UNDEFINED;
}

/* §7.2.1's WRITE SIDE of `name` — the origin check, which is the whole of what this member adds over the
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

/* §7.2.2.1's `close()`. THE WHOLE METHOD IS document_lifecycle.c's, and this member is one of its two
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
            *presult = JS_NewBool(ctx, wp_closed(p));
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
       count, so §7.2.3 answers 0 here rather than parking a flow on an instance that is gone. */
    if (wp_closed(p)) {
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
           WHAT A HIT MEANS, AND IT IS ONE ORDERED PAIR RATHER THAN A LIST. Either the completion lost its TYPE
           crossing the seam — the failure remote_object.h's leading byte exists to make impossible — or the
           peer answered by an ORDINARY [[Get]] of its own global, which is what `windowproxy.get` performs
           today (remote_op.c's OPS table: `globalThis[__apiclientKey]`). §7.2.1.3.4 CrossOriginGetOwnProperty-
           Helper does not read a property: for a member with [[NeedsGetter]] it calls "an anonymous built-in
           function, created in the current realm, that performs the same steps as the getter of the IDL
           attribute P on object O". `length` is [Replaceable], so a peer page that assigns `window.length =
           "0"` changes what an ordinary [[Get]] reads while a real browser still answers the child-navigable
           count — and THIS agent answers the identical read out of the navigable's own tree in the hosted
           branch above, so one fact is answered by the getter's steps locally and by whatever the page left on
           its global remotely. The fix has this file's own idiom one file over: remote_op.c already captures
           %Reflect.set% and %Reflect.apply% as realm intrinsics BEFORE any page script runs, precisely so a
           peer performs an internal method through the intrinsic rather than through something the page owns;
           capture §7.2.1.3.1's accessor members' original getters the same way and CALL one. */
        DCHECK(r != JS_STEP_DONE || proxy_answer_matches_idl(*presult, magic),
               "a peer answered a cross-instance WindowProxy member with a value §7.2.2 The Window object does "
               "not declare for it — `length` is an unsigned long and `closed` is a boolean, so the page's "
               "`otherW.length === 0` is being decided against something that is not a number");
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
       this same object, 50 installed members whose target interface it could not decide. */
    idl_interface_tag(ctx, proto, "Window");
    for (i = 0; i < WP_MEMBER_N; i++) {
        if (i == WP_LENGTH)
            idl_install_accessor_step(ctx, proto, PROXY_MEMBER[i], g_wp_len_getter_id, -1);
        else if (i == WP_CLOSED)
            idl_install_accessor_step(ctx, proto, PROXY_MEMBER[i], g_wp_closed_getter_id, -1);
        else if (i == WP_NAME)
            idl_install_accessor(ctx, proto, PROXY_MEMBER[i], proxy_member_get, i, g_wp_name_setter_id);
        else
            idl_install_accessor(ctx, proto, PROXY_MEMBER[i], proxy_member_get, i, -1);
    }
    idl_install_method(ctx, proto, "close", 0, g_wp_close_id);
    JS_SetClassProto(ctx, g_proxy_class, proto);
}

void window_proxy_init(JSContext *ctx)
{
    /* §7.2.3.5's cross-origin branch is the CLASS's, not the prototype's: it runs BEFORE the prototype walk,
       which is the whole reason a name that is not a member can be refused at all. */
    JSClassDef d = { "WindowProxy", .finalizer = proxy_finalizer, .gc_mark = proxy_gc_mark,
                     .exotic = &PROXY_EXOTIC };
    JSRuntime *rt = JS_GetRuntime(ctx);

    DCHECK(g_wp_rt == NULL || g_wp_rt == rt, "WindowProxy was installed into a second runtime");
    if (g_wp_rt == rt) return;
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
    int i;
    if (!g_wp_rt) return;
    while (e) { OwnedStr *n = e->next; free(e->s); free(e); e = n; }
    g_strings = NULL;
    /* §7.2.1.3.1's and §7.2.1.3.2's names, released with the agent that interned them. */
    for (i = 0; i < CROSS_ORIGIN_NAME_N; i++) { JS_FreeAtom(ctx, g_xo_atom[i]); g_xo_atom[i] = JS_ATOM_NULL; }
    for (i = 0; i < XO_FALLBACK_N; i++) { JS_FreeAtom(ctx, g_xo_fallback[i]); g_xo_fallback[i] = JS_ATOM_NULL; }
    /* THE REMOTE-NAVIGABLE ROWS BORROW, so this frees the TABLE and nothing in it. Emptying it here is what
       keeps a finalizer running later in the teardown from scanning freed storage: the loop then reads n == 0
       and touches nothing. */
    free(g_remote_navs);
    g_remote_navs = NULL;
    g_remote_navs_n = g_remote_navs_cap = 0;
    /* the prototypes are the REALMS' — released with their contexts, and the ORIGINS are the AGENT's, released
       by origin_release with everything else that outlives a realm */
    g_wp_rt = NULL;
}
