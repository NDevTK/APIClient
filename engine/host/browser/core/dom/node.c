/* THE NODE INTERFACE — DOM §4.4, and the CharacterData nodes on top of it (§4.10).
 *
 * WHY THIS EXISTS SEPARATELY FROM element.c. The wrapper table used to be keyed on lxb_dom_element_t, so an
 * ELEMENT was the only thing this engine could hand a page. Everything else in a real tree — Text, Comment —
 * had no wrapper at all, and the consequences were four separate lines in the WPT gap list that were all this
 * one hole: `document.createTextNode` was "not a function"; appendChild and removeChild asserted on "something
 * that is not an element wrapper"; and `Text`/`Comment` were undefined globals. A tree API whose only node kind
 * is Element cannot represent the document it just parsed.
 *
 * IDENTITY IS THE INVARIANT, and it is the reason this is a table rather than a fresh object per call. A page
 * compares nodes constantly (`n === el.firstChild`, a Set of visited nodes, a WeakMap keyed by node), and a
 * fresh wrapper per lookup makes every one of those silently false — the page then re-walks, re-binds and
 * re-inserts, and the engine reports a surface built out of that confusion instead of the page's.
 *
 * ONE CLASS, PROTOTYPE BY TYPE. Every wrapper is the same JS class (so one opaque, one table, one identity
 * rule); which members it carries is decided by the node's Lexbor type. That keeps `node_of` total — any
 * wrapper answers with its node — while an Element-only member still asserts it is on an Element.
 *
 * AND THE PROTOTYPE IS A REAL PROTOTYPE. The members used to be COPIED onto every wrapper — node_wrap ran
 * JS_SetPropertyFunctionList, and element.c registered an INSTALLER callback that ran again per element. That is
 * not what an interface is, and three things followed from it. `a.getAttribute === b.getAttribute` was FALSE for
 * two elements of one document, so a page that lifts a method off one node and applies it to another (which the
 * spec guarantees) was comparing and caching against a different function every time. There was no
 * `Element.prototype`, so `Element.prototype.contains.call(x)` and every feature test spelled that way found
 * nothing. And every member cost one closure per node.
 *
 * So the interfaces are prototype objects and a wrapper is one JS_NewObjectProtoClass with nothing installed on
 * it. Which prototype a node gets is its Lexbor TYPE mapped through the DOM's own interface hierarchy: Element
 * for an element, CharacterData for Text and Comment, Node for everything else. A component that owns a derived
 * interface REGISTERS its prototype here (element.c owns Element) so that node_wrap stays the ONE place a
 * wrapper is built — two builders is two identity tables, which is no identity at all.
 *
 * READS are pure Lexbor and run no page code, so they are ordinary C. WRITES go through the solver's
 * chokepoints (dom_cow_*) because a DOM write is per-flow TIME-TRAVEL state: two forked arms mutate the same
 * tree differently and each reads back its own. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>   /* <template>'s content fragment — see the clone walk */
#include <lexbor/ns/ns.h>

#include "check.h"
#include "quickjs.h"
#include "solver/dom_cow.h"
#include "solver/concolic.h"
#include "solver/attr_shadow.h"
#include "core/dom/mutation_observer.h"
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"
#include "core/dom/slot.h"
#include "core/dom/collections.h"
#include "core/dom/range.h"
#include "core/dom/node_iterator.h"   /* §6.1's pre-remove steps, which §4.2.3's move runs at its step 10 */
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/idl_index_arg.h"   /* §4.10 / §4.11's `unsigned long` operands, known and unknown */
#include "core/realm.h"
#include "core/dom/document.h"
#include "core/dom/element.h"   /* element_create_ns / element_free — §4.4 "clone a node"'s element half */
#include "core/dom/node_ns.h"   /* §4.4's two namespace WALKS, which its three lookup members each call once */
#include "core/dom/attr_list.h"    /* dom_attr_clone / dom_attr_attach — §4.4 step 2's attribute half */
#include "core/loader/data_block.h"   /* HTML §4.12.1: the two doors a data block's own text leaves by */
#include "core/dom/name_intern.h"  /* §4.4's names in the COPY's document — see clone_element_into */
#include "core/dom/node_heap.h"    /* …and whose arenas the node's BYTES are in, which §4.5 also decides */
#include "core/dom/node_interface.h" /* …and which C struct those names mean, on create AND on destroy */
#include "core/dom/text_content.h" /* §4.4's "switching on the interface node implements", answered in ONE place */
/* §4.5 adopt's step 3 arm. The DOM defines a node's custom element registry and the standard states the
   re-derivation right here, in §4.5; HTML owns what a registry IS. shadow_root.c reaches across the same
   boundary for the same reason. */
#include "core/html/custom_elements.h"
#include "core/html/html_script.h"
#include "core/html/nonce_attribute.h"   /* §2.5.6's cloning steps, on §4.4 step 3 beside §4.12.1's */
#include "solver/engine.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/frame/window.h"

static const IdlArgType IDL_1NSTR[1] = { IDL_DOMSTRING_NULLABLE };

static JSClassID g_node_class;

/* THE INTERFACE PER NODE TYPE, AS A CLASS ID RATHER THAN A PROTOTYPE. Indexed by lxb_dom_node_type_t, filled
   with Node's class at init so that a node kind no derived component claims is honestly a Node rather than a
   bare object.
   IT HOLDS CLASS IDS BECAUSE THE PROTOTYPES ARE PER REALM AND THIS TABLE IS NOT. §3.7 gives every realm its own
   interface prototype objects — and here that decides ANSWERS, since a C member runs in the realm that DEFINED
   it — so a table OF PROTOTYPES could only ever hold one document's. A class id is agent-scoped and immutable,
   and quickjs's per-context class-proto slot is where each realm's actual prototype lives. That also dissolves
   the registration ORDER problem: a component claims its node type ONCE per agent, and fills its own realm's
   slot whenever that realm is built, in whatever order realms happen. */
static JSClassID g_type_class[LXB_DOM_NODE_TYPE_LAST_ENTRY];
static JSClassID g_chardata_class, g_text_class, g_comment_class, g_cdata_class, g_pi_class;
static int     g_protos_ready;

/* THE IDENTITY MAP, keyed by the Lexbor node's ADDRESS.
   It was a linear scan, and that is O(document) on the single hottest path in the DOM. Every `parentNode`,
   every `firstChild`, every element a selector matches, every node an insertion step visits goes through
   node_wrap, so a scan made the whole engine O(n^2) in the size of the page it is reading — the same defect the
   live collections had, in the one place that cannot be worked around by caching an index. Adding seventeen
   elements to the fixture's document cost about seventy-five seconds of smoke time, and cutting the markup that
   built them by four made almost no difference, which is what a cost that is not proportional to the work looks
   like.
   OPEN ADDRESSING WITH LINEAR PROBING, and no deletion — a wrapper is never removed, because the map is what
   makes `n === n` true and dropping one silently breaks every identity comparison a page makes afterwards. No
   deletion means no tombstones, so a probe stops at the first empty slot and nothing else.
   FIBONACCI HASHING on the pointer: node addresses come from Lexbor's mraw pool and are therefore highly
   regular — consecutive nodes differ by a fixed stride — so the low bits alone would collide in runs. */
typedef struct { lxb_dom_node_t *n; JSValue obj; } NodeEntry;
static NodeEntry *g_wraps;        /* g_wrap_cap slots, a power of two; a NULL `n` is empty */
static int        g_wrap_n, g_wrap_cap;

/* THE AGENT'S RUNTIME, and it is here because the map is. Every entry holds a strong reference (see
   node_wrap_forget), and the site that has to release one is a node's DESTROY — which reaches this file with a
   lexbor node and nothing else. A realm cannot be recovered from a node being torn down (its document's record
   is released first), and it would be the wrong thing to recover: releasing a reference is a JSRuntime
   operation. The map itself is per AGENT, keyed on an address out of the agent's one node heap
   (core/dom/node_heap.h), so ONE runtime is the honest scope for it and node_init is where the agent says so. */
static JSRuntime *g_agent_rt;

static unsigned node_wrap_slot(const NodeEntry *tab, int cap, const lxb_dom_node_t *n)
{
    /* 2^32 / phi. The multiply spreads the pointer's HIGH bits down, which is where a pool allocator's
       addresses actually differ. */
    unsigned i = (unsigned)(((uintptr_t)n * 2654435769u) >> 16) & (unsigned)(cap - 1);
    while (tab[i].n && tab[i].n != n)
        i = (i + 1) & (unsigned)(cap - 1);
    return i;
}

/* Grow and rehash. At half full, so a probe stays short.
   THE MAP HOLDS ITS WRAPPERS STRONGLY AND THE NODE'S DEATH IS WHAT REMOVES ONE — this paragraph used to say the
   opposite ("the entry is weak and the finalizer removes it"), which was a statement about a design that had
   already been measured and rejected forty lines further down at node_finalizer, where the measurement lives: a
   weak entry is collected whenever no JS reference happens to be live, so the next `el.firstChild` allocates a
   fresh object and re-resolves its prototype, and the smoke fixture went from ~200 s to over 1500 s. The two
   defects that paragraph was written about are real and are fixed by the OTHER half of the pairing: the map
   used to have no removal at all, so every node any of thousands of exploration flows ever wrapped stayed
   pinned (one doubling of a table that size measured FIVE SECONDS inside a single createElement, with no
   suspend point in it), and a destroyed node left its entry behind for a pool allocator to hand out again —
   the next node at that address inheriting the dead node's wrapper and prototype. node_wrap_forget is the
   removal, driven from the one point every node death converges on, and identity then holds for exactly as
   long as the NODE exists, which is what the DOM says it holds for. */
void node_wrap_stats(long *n, long *cap) { if (n) *n = g_wrap_n; if (cap) *cap = g_wrap_cap; }

static void node_wrap_grow(void)
{
    int cap = g_wrap_cap ? g_wrap_cap * 2 : 256, i;
    NodeEntry *tab;
    /* THE COUNT AND THE OCCUPANCY ARE THE SAME NUMBER, and the growth policy is the only thing keeping a probe
       short, so it is worth saying so where it matters. A removal that decremented the count without actually
       freeing its slot would leave the table filling up while it believed itself half empty — and the probe in
       node_wrap_slot walks until it finds an empty slot, so the failure mode is not a slow lookup, it is a walk
       over the whole table on every insert, and finally one that never terminates. */
#if APICLIENT_DEV
    {
        long occ = 0;
        for (i = 0; i < g_wrap_cap; i++) if (g_wraps[i].n) occ++;
        DCHECK(occ == g_wrap_n, "the wrapper table's occupancy and its count disagree — a removal freed the "
                                "count but not the slot, so the probe walks a table that is fuller than the "
                                "load factor claims");
    }
#endif
    tab = calloc((size_t)cap, sizeof *tab);

    CHECK(tab != NULL, "the node wrapper table could not grow: a dropped wrapper breaks node identity, and "
                       "every `n === other` the page makes after it is silently false");
    for (i = 0; i < g_wrap_cap; i++)
        if (g_wraps[i].n)
            tab[node_wrap_slot(tab, cap, g_wraps[i].n)] = g_wraps[i];
    free(g_wraps);
    g_wraps = tab;
    g_wrap_cap = cap;
}

JSClassID node_class_id(void) { return g_node_class; }

lxb_dom_node_t *node_of(JSValueConst v)
{
    return JS_GetOpaque(v, g_node_class);
}

/* §4.7: A ShadowRoot IS A DocumentFragment. Lexbor gives a shadow root its own node type — which is what makes
   "is this a shadow root" answerable with no realm in hand, and which every §4.8 algorithm needs — so the two
   facts are stated once, here, rather than as a `||` at each of the ten places the standard says
   "DocumentFragment" and means both. A rule written over a DocumentFragment covers a shadow root; a rule about
   the SHADOW ROOT asks shadow_root_is. */
bool node_is_document_fragment(const lxb_dom_node_t *n)
{
    return n != NULL && (n->type == LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT ||
                         n->type == LXB_DOM_NODE_TYPE_SHADOW_ROOT);
}

/* THE NODE TYPE THE STANDARD EXPOSES, which is not always lexbor's. §4.4's `nodeType` is an interface-level
   fact and ShadowRoot's interface is DocumentFragment's, so a shadow root answers 11 — lexbor's own 14 is an
   implementation detail of how this engine tells one apart and is never a number a page sees. */
static int node_spec_type(const lxb_dom_node_t *n)
{
    return (n->type == LXB_DOM_NODE_TYPE_SHADOW_ROOT) ? (int)LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT : (int)n->type;
}

/* §4.4 nodeType — the numeric constants a page switches on. */
static JSValue js_node_get_type(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_node_t *n = node_of(this_val);
    if (!n) return JS_UNDEFINED;
    return JS_NewInt32(ctx, node_spec_type(n));
}

/* §4.4 nodeName: an element's is its qualified name uppercased by the HTML serialiser Lexbor already applies;
   a text node's is "#text" and a comment's "#comment", which the spec fixes rather than derives. */
static JSValue js_node_get_name(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_node_t *n = node_of(this_val);
    size_t len = 0;
    const lxb_char_t *s;

    if (!n) return JS_UNDEFINED;
    if (n->type == LXB_DOM_NODE_TYPE_TEXT)    return JS_NewString(ctx, "#text");
    if (n->type == LXB_DOM_NODE_TYPE_COMMENT) return JS_NewString(ctx, "#comment");
    /* §4.4's table gives a DocumentFragment "#document-fragment", and a ShadowRoot IS one — lexbor's
       lxb_dom_node_name has no row for its own shadow-root type and would answer the empty string. */
    if (n->type == LXB_DOM_NODE_TYPE_SHADOW_ROOT) return JS_NewString(ctx, "#document-fragment");
    s = lxb_dom_node_name(n, &len);
    return s ? JS_NewStringLen(ctx, (const char *)s, len) : JS_NewString(ctx, "");
}

/* §4.4 the tree accessors. Pure pointer reads off the tree the running flow sees — which is the flow's own
   tree, because every mutation went through the COW delta. */
static JSValue js_node_tree(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    if (!n) return JS_NULL;
    switch (magic) {
    case 0: return node_wrap(ctx, n->parent);
    case 1: return node_wrap(ctx, n->first_child);
    case 2: return node_wrap(ctx, n->last_child);
    case 3: return node_wrap(ctx, n->next);
    case 4: return node_wrap(ctx, n->prev);
    default: DFAIL("node tree accessor with an unknown magic"); return JS_NULL;
    }
}

/* §4.4 childNodes — a LIVE NodeList, [SameObject], counted off the tree at the moment it is asked. It was a
   static array, which is wrong in the way that costs a page its render: read `.length`, append a row, read it
   again, and the answer did not move. */
static JSValue js_node_child_nodes(JSContext *ctx, JSValueConst this_val)
{
    return collections_child_nodes(ctx, this_val);
}

/* §4.2.3 appendChild / removeChild — the per-flow chokepoints, and they return the node the spec returns
   (pages chain on it). Any node kind, which is the whole point of this file. */
static JSValue js_node_child_op(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic);

/* A newly inserted <script> is PREPARED (HTML 4.12.1) by the element component, which owns that rule; the base
   asks for it through this hook so node.c does not have to know what a script is. */
/* AN ELEMENT'S INTERFACE IS KEYED BY ITS TAG, which is HTML's mapping and not the DOM's — this table is keyed
   by node TYPE and cannot answer it. So the html layer registers the answer and node_wrap ASKS, which keeps it
   the ONE place a wrapper is built. Not a fallback and nothing to select against: there is exactly one answer
   per element, and an element wrapped before the resolver exists is a DCHECK, not a degraded path. */
/* WHICH HTML INTERFACE AN ELEMENT WEARS — the HTML layer's answer, and it takes a REALM because the prototype
   it names is that realm's. OWNED, like every other per-realm prototype read. */
static JSValue (*g_element_resolver)(JSContext *ctx, lxb_dom_element_t *el);
void node_set_element_resolver(JSValue (*fn)(JSContext *ctx, lxb_dom_element_t *el))
{
    /* ONE CLAIMANT, AND NULL GIVES IT BACK. The slot is this file's and the answer is the HTML layer's, so
       html_element_free releases it and node_free asserts that it did — a resolver left pointing into a
       component the cascade has already torn down is the defect core/agent_state.h found in idb_transaction. */
    DCHECK(fn == NULL || g_element_resolver == NULL,
           "a second component claimed the element-interface resolver — there is one answer per element, and "
           "the second claim silently decides the prototype of every wrapper the first was building");
    DCHECK(fn != NULL || g_element_resolver != NULL,
           "the element-interface resolver was released by a component that never registered one");
    g_element_resolver = fn;
}

/* THE TREE HOOK IS INSTALLED INTO THE CHOKEPOINT, not called from each mutation site, and the difference is a
   bug this file had: `element_on_inserted` was invoked from appendChild and NOWHERE else, so an element that
   entered the tree by insertBefore, by replaceChild, or by an innerHTML parse was never prepared and never
   upgraded. Eleven sites mutate the tree; one remembered. The chokepoint is the one place that cannot be
   forgotten, which is the same argument that put capture there. */
/* §4.8's HOST OF A SHADOW ROOT, wrapped — the one climb the standard has that leaves a tree for the thing
   containing it, and TWO §4.8 sentences make it: a shadow root's get the parent answers with its host, and
   retargeting step 2 sets A to its root's host. Written once because the ASSERTION is the reason: node_wrap
   answers JS_NULL for no node, so a hostless shadow root would have made both of them return "nothing above
   this" — an event that silently stops at the shadow boundary, and an object hidden from a listener reported to
   it as null. `n` is already established to be a shadow root by both callers. OWNED. */
static JSValue node_shadow_host_wrap(JSContext *ctx, const lxb_dom_node_t *n)
{
    lxb_dom_element_t *host = shadow_root_host(n);

    DCHECK(host != NULL, "§4.9's `attach a shadow root` sets the new root's host before anything can reach the "
                         "root, so every shadow root has one — a null here is a shadow root this engine built "
                         "without going through that algorithm");
    return node_wrap(ctx, lxb_dom_interface_node(host));
}

/* §2.9's GET THE PARENT, answered for the events layer one step at a time — DOM §4.4: "A node's get the parent
   algorithm, given an event, returns the node's assigned slot, if node is assigned; otherwise node's parent",
   and HTML overrides it for a DOCUMENT: "returns null if event's type attribute value is `load` or document
   does not have a browsing context; otherwise the document's relevant global object."
 *
 * IT REPLACED AN "ANCESTORS" LIST, and the list could not express either override. Its caller appended the
 * running realm's window above whatever it returned, so a DETACHED div's bubbling event propagated to the
 * window, and a document's `load` propagated there too — the one type the spec stops. It could not express the
 * relevant-global half either: the window above a document is THAT DOCUMENT'S, and a list of nodes has nowhere
 * to say so, which is why the append read the realm that happened to be running.
 * NULL for anything that is not a node — an AbortSignal, a Request, a MessagePort — which is §2.7's own default
 * and gives a path of one, and which the list had to spell as "undefined, not an empty array". */
static JSValue node_get_parent(JSContext *ctx, JSValueConst target, JSValueConst ev)
{
    lxb_dom_node_t *n = node_of(target);

    if (!n)
        return JS_NULL;
    if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT) {
        JSValue type = event_type(ctx, ev);
        JSValueConst win;
        const char *t = JS_IsString(type) ? JS_ToCString(ctx, type) : NULL;
        bool is_load = t != NULL && !strcmp(t, "load");

        if (t) JS_FreeCString(ctx, t);
        JS_FreeValue(ctx, type);
        if (is_load)
            return JS_NULL;
        /* "the document's relevant global object" — THIS document's realm, not the running one. A same-origin
           parent dispatching into a child's document is one agent and one flow, so the realm that is executing
           is routinely not the one the event is travelling through. */
        win = document_window_of(n);   /* BORROWED there; get the parent answers OWNED, like node_wrap */
        DCHECK(JS_IsNull(win) || JS_IsObject(win),
               "a document answered its get the parent with something that is not a Window and is not null");
        return JS_DupValue(ctx, win);
    }
    /* §4.8: "A shadow root's get the parent algorithm, given an event, returns null if event's composed flag is
       unset and shadow root is the root of event's path's FIRST event path item's invocation target; otherwise
       shadow root's host." That condition is the whole of what stops a non-composed event escaping the tree it
       was dispatched in — and its "first path item" half is why a non-composed event dispatched AT THE HOST
       still reaches the document: the host's root is not this shadow root. */
    if (shadow_root_is(n)) {
        if (!event_composed(ctx, ev)) {
            JSValue first = event_path_first_invocation_target(ctx, ev);
            lxb_dom_node_t *fn = node_of(first);
            bool inside = fn != NULL && node_root(fn) == n;

            JS_FreeValue(ctx, first);
            if (inside)
                return JS_NULL;
        }
        return node_shadow_host_wrap(ctx, n);
    }
    /* §4.4: "A node's get the parent algorithm, given an event, returns the node's ASSIGNED SLOT, if node is
       assigned; otherwise node's parent." A slotted node's event therefore travels through the shadow tree that
       renders it rather than up its own light-tree ancestors — which is the whole reason §2.9's walk carries a
       `slottable` and a slot-in-closed-tree flag, and why it can assert that the parent it gets back is a slot. */
    {
        lxb_dom_node_t *slot = slot_assigned_slot(ctx, n);
        if (slot)
            return node_wrap(ctx, slot);
    }
    return node_wrap(ctx, n->parent);
}

/* §4.4's ROOT as §2.9 asks it — of an EventTarget, which may be a Window. JS_NULL says "not a node", which is
   the same answer the walk needs for step 6.9.6's "parent is a node and …". OWNED, like node_wrap. */
static JSValue node_event_root(JSContext *ctx, JSValueConst target)
{
    lxb_dom_node_t *n = node_of(target);

    return n ? node_wrap(ctx, node_root(n)) : JS_NULL;
}

static int node_event_shadow_root_mode(JSContext *ctx, JSValueConst target)
{
    lxb_dom_node_t *n = node_of(target);

    (void)ctx;
    if (!n || !shadow_root_is(n))
        return EVENT_TREE_NOT_SHADOW_ROOT;
    return shadow_root_is_open(n) ? EVENT_TREE_SHADOW_OPEN : EVENT_TREE_SHADOW_CLOSED;
}

static bool node_event_is_window(JSContext *ctx, JSValueConst target)
{
    (void)ctx;
    return window_is(target);
}

static bool node_event_is_slot(JSContext *ctx, JSValueConst target)
{
    (void)ctx;
    return slot_is(node_of(target));
}

static bool node_event_is_assigned_slottable(JSContext *ctx, JSValueConst target)
{
    return slot_assigned_slot(ctx, node_of(target)) != NULL;
}

/* §4.2's relation, over EventTargets — the relation itself is shadow_root.c's, because it is a fact about the
   tree and has three callers with nothing else in common. */
static bool node_event_is_shadow_including_inclusive_ancestor(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    (void)ctx;
    return shadow_root_is_shadow_including_inclusive_ancestor(node_of(a), node_of(b));
}

/* §2.7's DEFAULT PASSIVE VALUE, the half that is a tree question: is this target the Window, the node document
   itself, its document element, or its body. The event-TYPE half is the events layer's, which is why this
   answers only about the target. */
static bool node_default_passive_target(JSContext *ctx, JSValueConst target)
{
    lxb_dom_node_t *n = node_of(target);

    if (!n) {
        /* not a node — the only other target the spec's list names is a WINDOW, and a realm's global is the
           one object that is one. */
        JSValue g = JS_GetGlobalObject(ctx);
        bool same = JS_VALUE_GET_PTR(g) == JS_VALUE_GET_PTR(target);
        JS_FreeValue(ctx, g);
        return same;
    }
    return document_is_passive_default_node(n);
}

/* §4.8's HOST OF A SHADOW ROOT — retargeting step 2's "set A to A's root's host", and the only tree question in
   this list whose answer is a node of a DIFFERENT tree than the node it was asked about. `root` climbs INSIDE a
   tree and stops at the shadow root; this is the step that leaves it, so the two together are what make the
   retargeting loop terminate at the document tree.
   JS_NULL for anything that is not a shadow root — the same "not a shadow root" answer node_event_shadow_root_mode
   gives, asked of an EventTarget because §2.9 hands over path entries it has not established are nodes at all.
   OWNED, like node_wrap. */
static JSValue node_event_shadow_host(JSContext *ctx, JSValueConst target)
{
    lxb_dom_node_t *n = node_of(target);

    if (!n || !shadow_root_is(n))
        return JS_NULL;
    return node_shadow_host_wrap(ctx, n);
}

static const EventTargetTree NODE_EVENT_TREE = {
    node_get_parent, node_default_passive_target, node_event_root, node_event_shadow_root_mode,
    node_event_is_window, node_event_is_slot, node_event_is_assigned_slottable,
    node_event_is_shadow_including_inclusive_ancestor, node_event_shadow_host
};

/* §4.2.3's INSERTION AND REMOVING STEPS ARE A LIST, not a slot. The standard's own `remove` runs several
   independent things in order — the live-range pre-remove steps, then §6.1's NodeIterator pre-remove steps,
   then each interested party's removing steps — and this was ONE function pointer, so the second component to
   need it would have had to be called from inside the first. That is the hand-copied list CLAUDE.md warns
   about, one layer down: a component added without editing whoever happens to hold the slot is a component
   whose steps silently never run.
   The ORDER is the registration order, which is the standard's order because the components register in it —
   which is why `insert`'s and `remove`'s OWN steps join it too and are not called around it: step 7.7.3's
   enqueue is numbered between step 7.7.1's insertion steps and step 8's mutation record, and a call placed
   before or after the whole loop cannot be between two of its members. */
#define NODE_TREE_HOOKS_MAX 8
static NodeTreeHook g_tree_hooks[NODE_TREE_HOOKS_MAX];
static int g_tree_hook_n;

/* §4.2.3's CHILDREN CHANGED STEPS — see node.h for why they are their own list. Fired from the three callers
   the standard names: `insert` and `remove` below, and §4.10's `replace data` (node_cd_replace_data, and §4.4
   normalize's step 4, which IS a replace data written out). */
#define NODE_CHILDREN_CHANGED_HOOKS_MAX 4
static NodeChildrenChangedHook g_cc_hooks[NODE_CHILDREN_CHANGED_HOOKS_MAX];
static int g_cc_hook_n;

static void node_children_changed(JSContext *ctx, lxb_dom_node_t *parent)
{
    int i;

    /* "If node's parent is non-null" — `replace data` on a parentless text node changes nobody's children, and
       a removal from a fragment root that is itself detached still has a parent to tell. */
    if (!parent) return;
    for (i = 0; i < g_cc_hook_n; i++)
        g_cc_hooks[i](ctx, parent);
}

void node_add_children_changed_hook(NodeChildrenChangedHook fn)
{
    DCHECK(fn != NULL, "a children-changed-steps hook was registered as nothing");
    CHECK(g_cc_hook_n < NODE_CHILDREN_CHANGED_HOOKS_MAX,
          "more §4.2.3 children-changed-steps hooks were registered than the list holds — a dropped one is a "
          "component that never learns its own contents were rewritten");
    g_cc_hooks[g_cc_hook_n++] = fn;
}

/* DOM §4.2.3 "Mutation algorithms" — `insert`'s AND `remove`'s OWN CUSTOM-ELEMENT STEPS, WHICH ARE NOT
 * INSERTION OR REMOVING STEPS AND THEREFORE MUST NOT TRAVEL WITH THEM.
 *
 * The distinction is the standard's own numbering and it decides the order every lifecycle callback arrives in.
 * Step 7.7 is a loop with four sub-steps and only the FIRST is a hook another standard defines. Step 7.7.1 is
 * "Run the insertion steps with inclusiveDescendant"; 7.7.2 is "If inclusiveDescendant is not connected, then
 * continue"; 7.7.3 opens "If inclusiveDescendant is an element and inclusiveDescendant's custom element
 * registry is non-null"; and under it 7.7.3.2 is "If inclusiveDescendant is custom, then enqueue a custom
 * element callback reaction with inclusiveDescendant, callback name "connectedCallback", and « »" with 7.7.3.3
 * "Otherwise, try to upgrade inclusiveDescendant".
 * `remove` separates the two further still — there the enqueue is a TOP-LEVEL step of its own. Step 11 is "Run
 * the removing steps with node, true, and parent"; step 12 is "Let isParentConnected be parent's connected";
 * step 13 is "If node is custom and isParentConnected is true, then enqueue a custom element callback reaction
 * with node, callback name "disconnectedCallback", and « »"; and step 14 is "For each shadow-including
 * descendant descendant of node, in shadow-including tree order", whose 14.1 is "Run the removing steps with
 * descendant, false, and parent" and whose 14.2 is "If descendant is custom and isParentConnected is true, then
 * enqueue a custom element callback reaction with descendant, callback name "disconnectedCallback", and « »".
 * (Counted with LIST DEPTH TRACKED against the fetched text: `insert` has 12 top-level steps and `remove` 17;
 * step 7.7's four sub-steps and step 14's two are nested lists, not peers of the steps that hold them.)
 *
 * WHY THAT MATTERS HERE AND NOT MERELY ON PAPER: the insertion and removing steps are DEFERRED by this engine,
 * because they can park (a `<script>`'s preparation) and fork (§6.6.7's autofocus asks about activation), and a
 * chokepoint is a C body with no flow base under it. The custom-element enqueue can do neither — it allocates a
 * reaction array and pushes it — so deferring it too bought nothing and COST THE ORDER. §4.5 `adopt` step 3.3.3
 * enqueues `adoptedCallback` from inside the walk, synchronously, because adopt has no deferred half to hide in
 * and needs none: `document.adoptNode(el)` on a parentless node performs NO TREE CHANGE AT ALL, so there is no
 * record for any drain to find. So the two enqueues could not both be deferred even in principle, and while one
 * was and the other was not, every cross-document move produced « adopted, …, disconnected, connected » where
 * the two standards' own numbering gives « disconnected, adopted, connected »: §4.2.3 `insert` puts step 7.1,
 * "Adopt node into parent's node document", ahead of step 7.7, and §4.5 "Interface Document"'s `adopt` puts its
 * step 2, "If node's parent is non-null, then remove node", ahead of its step 3 — so `remove`'s enqueue runs
 * inside adopt step 2, adopt's own runs at its step 3.3.3, and `insert`'s runs last at step 7.7.3.
 *
 * THE PHASES ARE THE STEP NUMBERS, AND SO IS THE REGISTRATION POSITION. On the insert side the enqueue is step
 * 7.7.3: AFTER the recorder that feeds step 7.7.1's drain and BEFORE §4.3's step 8 mutation record, which is a
 * place only a member of the hook list can occupy — a call placed around the loop would sit before every
 * member or after every member, and step 8's record is one of them. On the removal side steps 12-14 come AFTER
 * step 7's detach, which is why this answers at NODE_TREE_REMOVED and not NODE_TREE_REMOVING: step 12 reads
 * `parent`'s connected precisely because the node no longer has one to be asked for, and `parent` is passed to
 * this list for that reason. One walk answers both sides — step 7.7's set is the shadow-including INCLUSIVE
 * descendants, and step 13 (the node) followed by step 14 (its shadow-including DESCENDANTS) is that same set
 * in that same order, written from the other end.
 *
 * THE REALM IS THE NODE'S DOCUMENT'S, never this hook's `ctx`: dom_cow holds ONE context set at init, so using
 * it would answer every document's insertion out of whichever realm happened to run first — CLAUDE.md
 * §A-PER-REALM-FACT. document.h states the same rule for §4.2.3's steps in its own words. A node document with
 * no realm is a parse in progress and has no reaction queue to join. */
static lxb_dom_document_t *node_document_of(lxb_dom_node_t *n);

void node_custom_element_reactions_tree_steps(JSContext *ctx, lxb_dom_node_t *root, lxb_dom_node_t *parent,
                                             int phase)
{
    JSContext *rctx;
    lxb_dom_node_t *n;
    lxb_dom_document_t *doc;

    /* NODE_TREE_REMOVING is `remove` BEFORE step 7's detach. Steps 12-14 are numbered after it, and step 12's
       isParentConnected would have no parent to be read off — so this phase is not one of the two, and the
       other phase of a removal is where the answer is. */
    (void)ctx;
    if (phase == NODE_TREE_REMOVING) return;
    DCHECK(phase == NODE_TREE_INSERTED || phase == NODE_TREE_REMOVED,
           "§4.2.3's custom-element enqueue ran at a phase the tree-steps list does not have");
    if (!root) return;
    /* Insert step 7.7.2 ("If inclusiveDescendant is not connected, then continue") and remove step 12 ("Let
       isParentConnected be parent's connected") are ONE question asked of two different nodes, because by
       NODE_TREE_REMOVED the node is detached and the parent is the only end of the edge still in the tree.
       Every node this walk reaches shares the answer: it is bounded by `root`, and a shadow-including inclusive
       descendant of a connected node is connected. */
    if (phase == NODE_TREE_INSERTED) {
        if (!node_is_connected(root)) return;
    } else {
        if (!parent || !node_is_connected(parent)) return;
        DCHECK(root->parent == NULL,
               "§4.2.3 `remove` step 13 was reached with the node still attached — steps 12-14 are numbered "
               "after step 7's \"remove node from its parent's children\", and running them before it would "
               "read isParentConnected off an edge the algorithm has already cut");
    }
    rctx = document_realm_of(root);
    if (!rctx) return;
    doc = node_document_of(root);
    for (n = root; n; n = shadow_root_next_in_shadow_including(rctx, n, root)) {
        DCHECK(node_document_of(n) == doc,
               "§4.2.3's shadow-including walk left the node document it started in — a shadow root's node "
               "document is its host's, so one realm answers the whole subtree, and a descendant in another "
               "document means this batch needs one realm per node rather than one per walk");
        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (phase == NODE_TREE_INSERTED)
            custom_elements_element_connected(rctx, lxb_dom_interface_element(n));   /* STEP 7.7.3 */
        else
            custom_elements_disconnected(rctx, lxb_dom_interface_element(n));        /* STEPS 13 and 14.2 */
    }
}

static void node_tree_hooks_run(JSContext *ctx, lxb_dom_node_t *n, lxb_dom_node_t *parent, int phase)
{
    int i;
    DCHECK(phase >= NODE_TREE_REMOVED && phase <= NODE_TREE_INSERTED,
           "the tree-steps list was run at a phase §4.2.3 does not have");
    for (i = 0; i < g_tree_hook_n; i++)
        g_tree_hooks[i](ctx, n, parent, phase);
    /* §4.2.3 numbers the children changed steps AFTER the insertion steps and after step 8's mutation record,
       and after the detach on the removal side — which is exactly here, past the whole hook list, on the two
       phases that leave the parent's child list changed. NODE_TREE_REMOVING is the pre-detach phase and the
       children have not changed yet, so it is not one of them. */
    if (phase == NODE_TREE_INSERTED || phase == NODE_TREE_REMOVED)
        node_children_changed(ctx, parent);
}

void node_add_tree_hook(NodeTreeHook fn)
{
    DCHECK(fn != NULL, "a tree-steps hook was registered as nothing");
    CHECK(g_tree_hook_n < NODE_TREE_HOOKS_MAX,
          "more §4.2.3 tree-steps hooks were registered than the list holds — a dropped one is a component "
          "whose insertion or removing steps silently never run");
    g_tree_hooks[g_tree_hook_n++] = fn;
    dom_cow_set_tree_hook(node_tree_hooks_run);
}

/* §4.2.3's MOVING STEPS — see node.h. It is NOT reached from the DOM-mutation chokepoint, and that is the
   whole difference between this list and the one above: the chokepoint fires the insertion and removing steps
   for every tree write there is, and a move is the one tree write that must reach neither. So the move
   algorithm calls this list by name, at its own step 24.2, and nothing else can. */
#define NODE_MOVING_HOOKS_MAX 4
static NodeMovingHook g_moving_hooks[NODE_MOVING_HOOKS_MAX];
static int g_moving_hook_n;

void node_add_moving_hook(NodeMovingHook fn)
{
    DCHECK(fn != NULL, "a moving-steps hook was registered as nothing");
    CHECK(g_moving_hook_n < NODE_MOVING_HOOKS_MAX,
          "more §4.2.3 moving-steps hooks were registered than the list holds — a dropped one is a component "
          "whose moving steps silently never run, which is a state-preserving move that preserves the wrong "
          "state");
    g_moving_hooks[g_moving_hook_n++] = fn;
}

/* ---- DOM §4.5 "ADOPT A NODE" ----------------------------------------------------------------------------
 *
 * ONLY STEP 2 OF IT EXISTED, inline in `node_insert_at`, and step 2 is the one step that is not about
 * adoption at all — it is the removal that makes a move a move. Everything the algorithm is FOR was missing:
 * a node inserted into another document kept the node document it was created in, so `ownerDocument` lied,
 * `document.createElement` on the wrong document's behalf built attributes that disagreed with their element
 * (attr_list.c asserts exactly that pairing), a custom element moved between documents got no
 * `adoptedCallback`, its registry was never re-derived, and a `<template>` carried its contents into a
 * document that is not their owner. Nothing threw for any of it.
 *
 * THE WALK IS ITERATIVE OVER AN EXPLICIT STACK, and not because a rest point would be hard: `node_insert_at`
 * is reached from `appendChild`, from the ChildNode/ParentNode mixins and from §5.5's range machines, and
 * every one of those is a plain C body with no flow base under it, so there is nothing here that COULD
 * suspend. What a page's tree can still do is make the walk as deep as its markup, and C recursion over that
 * is the overflow this project has removed from every other tree algorithm (see `clone a node`'s level stack,
 * declarative_shadow.c's list, §4.2's shadow-including walk). So the depth lives in a heap stack of FRAMES.
 * A frame is one INVOCATION of `adopt`: step 3.4's adopting steps for a `<template>` adopt its contents into a
 * DIFFERENT document, which is a nested adopt with its own `document` and its own `oldDocument`, and the frame
 * carries both. The outer frame's cursor is advanced BEFORE the nested frame is pushed, so the nested walk
 * runs exactly where step 3.4 puts it — between this descendant and the next one — rather than after the
 * outer subtree, which is the order every reaction the walk enqueues is delivered in.
 * WHEN THE CALLER CAN PARK, this becomes a stage block over the same frames and the stack becomes the state;
 * nothing about the algorithm changes. */
typedef struct {
    lxb_dom_node_t     *root;   /* this invocation's `node` — what bounds its shadow-including walk */
    lxb_dom_node_t     *cur;    /* the next inclusive descendant step 3 has still to visit */
    lxb_dom_document_t *doc;    /* this invocation's `document` */
    lxb_dom_document_t *old;    /* step 1's `oldDocument`, read before step 3 wrote a single node document */
} NodeAdoptFrame;

typedef struct {
    NodeAdoptFrame *f;
    int sp, cap;
} NodeAdoptStack;

/* §4.4's NODE DOCUMENT as a Lexbor question: a Document IS its own, and lexbor says so through the same
   field, but reading it through one place is what keeps the assert below honest for both. */
static lxb_dom_document_t *node_document_of(lxb_dom_node_t *n)
{
    if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT) return lxb_dom_interface_document(n);
    return n->owner_document;
}

/* §4.5 adopt STEP 3.1 — "set inclusiveDescendant's node document to document", and step 3.3.1's same write
   over an element's attribute list. THE ONE PLACE A NODE'S NODE DOCUMENT CHANGES, which is what lets the two
   obligations below be stated once instead of at every caller of every mutation algorithm.
   A NODE DOCUMENT IS NOT ONE POINTER. Every NAME the node holds is an id into the hashes of the document it
   is leaving — `lxb_dom_node_t.local_name`, `.ns`, `.prefix`, an element's `qualified_name`, an attribute's
   four, a doctype's `name` — and a non-static id IS the hash entry's address, so re-pointing `owner_document`
   alone leaves the node naming memory `lxb_dom_document_destroy` frees with the other document. The names
   travel with the document, through the one list in core/dom/name_intern.h, BEFORE the pointer moves so the
   source is still the source when its bytes are read. */
static void node_set_node_document(lxb_dom_node_t *n, lxb_dom_document_t *doc)
{
    DCHECK(n->type != LXB_DOM_NODE_TYPE_DOCUMENT,
           "DOM §4.5 adopt step 3.1 tried to set a DOCUMENT's node document — a document IS its own node "
           "document, and `adoptNode` throws NotSupportedError rather than reaching one");
    if (n->owner_document == doc) {
        /* THE SKIP IS ASSERTED, NOT ASSUMED. Step 3's walk visits every shadow-including inclusive descendant
           and this is the one arm that does nothing, so a descendant that is already in `document` while
           holding a name from somewhere else would be the single node the fix below silently misses. */
        DCHECK(dom_names_owned_by(doc, n),
               "a node already in this document holds a name id minted in another one — nothing on the adopt "
               "path will move it now, and it names hash memory that document's destroy will free");
        DCHECK(dom_storage_owned_by(doc, n),
               "a node already in this document is allocated out of arenas this document does not name — "
               "lxb_dom_node_interface_destroy frees through `owner_document->mraw`, so it would hand the "
               "chunk to an allocator that never allocated it");
        return;
    }
    dom_import_node_names(doc, n->owner_document, n);
    /* THROUGH THE CHOKEPOINT, because a node document is shared baseline state exactly like a parent link: the
       delta has an entry kind for it, so a flow that adopts a subtree moves those nodes in ITS timeline and a
       sibling arm that never adopted still reads the document it knew — names included, because the delta's
       swap re-imports them in the direction it moves the pointer. */
    dom_cow_set_node_document(n, doc);
    /* AND THE BYTES DID NOT HAVE TO MOVE, which is a statement about the heap and not an omission. A node's
       struct address IS its identity — its parent, its siblings, the delta's entries and the wrapper map all
       name it — so it cannot be copied into another arena the way its names are re-interned; the fix is that
       every document in this agent allocates out of ONE pair of arenas, so the destination's `mraw` IS the
       source's and no document's destroy can free memory another document's nodes live in. This is the
       assertion that says so at the one line that would otherwise have created the dangling pointer. */
    DCHECK(dom_storage_owned_by(doc, n),
           "DOM §4.5 adopt moved a node into a document whose arenas are not the ones the node's bytes are in "
           "— core/dom/node_heap.h is the agent heap that makes that impossible, so a document reaching here "
           "with private arenas was not built by dom_document_create");
}

static void node_adopt_push(NodeAdoptStack *s, lxb_dom_node_t *root, lxb_dom_document_t *doc)
{
    NodeAdoptFrame *f;

    /* STEP 3's condition, asked where the invocation begins: an adopt into the document a node is already in
       does nothing at all, so the frame is never pushed and the walk is never entered. */
    if (node_document_of(root) == doc) return;
    if (s->sp == s->cap) {
        int want = s->cap ? s->cap * 2 : 4;
        NodeAdoptFrame *n = realloc(s->f, sizeof(NodeAdoptFrame) * (size_t)want);
        CHECK(n != NULL, "DOM §4.5 adopt could not grow its level stack");
        s->f = n;
        s->cap = want;
    }
    f = &s->f[s->sp++];
    f->root = root;
    f->cur = root;                       /* step 3's walk is INCLUSIVE — it starts at the node itself */
    f->doc = doc;
    f->old = node_document_of(root);     /* STEP 1 */
}

/* §4.5's STEP 3.4 — "run the adopting steps with inclusiveDescendant and oldDocument". HTML defines them for
   exactly one element: a `<template>`, whose CONTENTS are a separate tree that no child link reaches, so
   without this an adopted template's markup keeps a node document its element no longer has.
   HTML §4.12.3 adopts them into the new node document's APPROPRIATE TEMPLATE CONTENTS OWNER DOCUMENT — an
   inert Document with no browsing context, created once per document and remembered on it, which is what
   keeps a template's scripts from running.
   IT IS PUSHED ONTO THIS WALK'S STACK, NOT CALLED. A frame is one INVOCATION of adopt, and adopting the
   contents is a second invocation with its own `document` and its own `oldDocument` — so it belongs on the
   stack the walk already has. Calling node_adopt here instead would be C recursion whose depth is the page's
   template nesting, which is the page's to choose; a `<template>` inside a `<template>` inside a `<template>`
   is ordinary markup. The push is also what makes the nested adopt run BETWEEN this walk's descendants rather
   than after all of them, which is the order step 3.4 is stated in. */
static void node_adopting_steps(JSContext *ctx, NodeAdoptStack *s, lxb_dom_node_t *n, lxb_dom_document_t *doc)
{
    lxb_html_template_element_t *t;

    if (n->type != LXB_DOM_NODE_TYPE_ELEMENT || !lxb_html_tree_node_is(n, LXB_TAG_TEMPLATE)) return;
    t = lxb_html_interface_template(n);
    /* A template always HAS its contents — §4.12.3 establishes them when the element is created — so an
       element reaching here without one came from something that built a template interface by hand. */
    DCHECK(t->content != NULL, "a <template> element has no template contents at DOM §4.5 adopt step 3.4");
    if (!t->content) return;
    node_adopt_push(s, &t->content->node,
                    lxb_dom_interface_document(document_template_contents_owner(ctx, doc)));
}

/* DOM §4.5 "ADOPT A NODE", given `node` and `document`. */
void node_adopt(JSContext *ctx, lxb_dom_node_t *node, lxb_dom_document_t *document)
{
    NodeAdoptStack s = { NULL, 0, 0 };

    DCHECK(node != NULL && document != NULL, "DOM §4.5 adopt was asked of nothing");
    /* STEP 2. It is the capturing removal and not a raw detach because it IS a removal: the per-flow delta has
       to see it, and so do §4.2.3's removing steps and §6.1's pre-remove steps. */
    if (node->parent) dom_cow_remove_child(node);
    node_adopt_push(&s, node, document);           /* STEPS 1 and 3's condition */
    /* Step 3 mints wrappers and reads per-flow records off them, so it needs the realm — and only step 3 does,
       which is why an adopt into the document a node is already in is answerable without one. */
    DCHECK(s.sp == 0 || ctx != NULL,
           "DOM §4.5 adopt's step 3 walk ran with no realm — every part of it (the shadow-including walk's "
           "element→shadow-root association, the registry arm, the adoptedCallback reaction) is a per-flow "
           "fact kept on a node's WRAPPER, and there is no realm to build one in");
    while (s.sp > 0) {
        NodeAdoptFrame *f = &s.f[s.sp - 1];
        lxb_dom_node_t *d = f->cur;
        lxb_dom_document_t *doc = f->doc, *old = f->old;

        if (!d) { s.sp--; continue; }
        /* Advanced BEFORE the steps run, so step 3.4's nested invocation lands between this descendant and the
           next rather than after the whole subtree — and so this frame's pointer may be invalidated by the
           push below without the cursor being lost. */
        f->cur = shadow_root_next_in_shadow_including(ctx, d, f->root);

        node_set_node_document(d, doc);                                              /* STEP 3.1 */
        if (d->type == LXB_DOM_NODE_TYPE_ELEMENT) {                                  /* STEP 3.3.1 */
            lxb_dom_attr_t *a;
            for (a = lxb_dom_element_first_attribute(lxb_dom_interface_element(d)); a;
                 a = lxb_dom_element_next_attribute(a))
                node_set_node_document(&a->node, doc);
        }
        custom_elements_node_adopted(ctx, d, doc, old);       /* STEPS 3.2, 3.3.2 and 3.3.3 */
        node_adopting_steps(ctx, &s, d, doc);                 /* STEP 3.4 */
    }
    free(s.f);
}

/* §4.2.3 "insert" — A DocumentFragment IS NOT INSERTED; ITS CHILDREN ARE, in order, and the fragment is left
   empty. That is not a detail: `createDocumentFragment` exists so a page can batch nodes and attach them in one
   call, so every use of it went wrong — the fragment NODE landed in the tree, `frag.childNodes.length` stayed
   what it was, and the page's markup was one level deeper than it built. Nothing threw, because a fragment is a
   node and appending one is a legal thing to do.
   One helper, used by every member that inserts, because "which node actually goes in" is the same question for
   appendChild, insertBefore, replaceChild and the four ChildNode/ParentNode mixins — and a member that forgot to
   ask it would be the one place fragments quietly stopped working again.
   The children come OUT through the capturing chokepoint rather than a private detach: a fragment can be older
   than the fork that is running, so another flow's baseline may hold it. */
/* §4.2.3 insert STEP 7.1 — "Adopt node into parent's node document", which is where the ONE adopt above is
   reached from. The realm is the PARENT'S DOCUMENT'S, because that is the document being adopted into and its
   registry is what step 3 re-derives against; `node_insert_at` carries no JSContext of its own, deliberately,
   and taking the running one instead would answer a second same-origin document's adoption out of whichever
   realm happened to call. */
static void node_insert_adopt(lxb_dom_node_t *parent, lxb_dom_node_t *node)
{
    node_adopt(document_realm_of(parent), node, node_document_of(parent));
}

static bool node_is_chardata(const lxb_dom_node_t *n);

/* §4.2.3 "ensure pre-insert validity" — the ELEVEN steps that keep a node tree a node tree, in ONE place.
 *
 * This engine had exactly ONE of them: step 2's ancestor check, written out separately at appendChild, at
 * insertBefore and at replaceChild and absent from the seven ChildNode/ParentNode mixins entirely. Every other
 * step was missing, and each missing step is a tree a browser refuses to build and this engine built:
 * `document.appendChild(document.createTextNode("x"))` put a Text node beside <html>; `document.appendChild(el)`
 * gave a Document two element children; `el.appendChild(doctype)` put a doctype inside a <div>;
 * `text.appendChild(el)` gave a Text node children. None of them threw, so the page went on running against a
 * tree whose shape no serializer, no selector match and no walk in this engine is written for — the failure
 * surfaces far from the call that caused it, which is the whole reason the standard checks first.
 *
 * `childrenToExclude` is « » for pre-insert and « child » for replace — the ONLY two the standard ever passes —
 * so it is one node pointer rather than a list, and the DCHECK says which two it may be. Steps 9 and 11 are
 * where it is read, and reading it is the whole of the difference between the two algorithms' validity.
 *
 * STEP 2 IS HOST-INCLUDING, NOT PLAIN ANCESTOR (§4.2.2 "Shadow tree": "A is a host-including inclusive ancestor
 * of B if either A is an inclusive ancestor of B, or if B's root has a non-null host and A is a host-including
 * inclusive ancestor of B's root's host"). A shadow root is the only fragment this engine gives a host, so
 * shadow_root_is_shadow_including_inclusive_ancestor IS that relation here — and it is the one that matters:
 * `host.shadowRoot.appendChild(host)` is a cycle a plain parent walk answers `false` for.
 *
 * Returns false HAVING THROWN, so every caller is `if (!…) return JS_EXCEPTION;`. */
bool node_ensure_pre_insert_valid(JSContext *ctx, lxb_dom_node_t *node, lxb_dom_node_t *parent,
                                  lxb_dom_node_t *child, lxb_dom_node_t *exclude)
{
    lxb_dom_node_t *c;
    unsigned n_el = 0;
    bool has_text = false;

    DCHECK(node != NULL && parent != NULL,
           "§4.2.3's pre-insert validity was asked about no node or no parent — both are non-nullable in the "
           "algorithm's own signature, and a member reaching here with one is a member that skipped its "
           "declaration's brand check");
    DCHECK(exclude == NULL || exclude == child,
           "§4.2.3's childrenToExclude is « » for pre-insert and « child » for replace, and nothing else — a "
           "third list would be an algorithm the standard does not have");

    /* STEP 1 */
    if (parent->type != LXB_DOM_NODE_TYPE_DOCUMENT && parent->type != LXB_DOM_NODE_TYPE_ELEMENT &&
        !node_is_document_fragment(parent))
        return JS_ThrowDOMException(ctx, "HierarchyRequestError",
                                    "only a Document, DocumentFragment or Element can be a parent"), false;
    /* STEP 2 */
    if (shadow_root_is_shadow_including_inclusive_ancestor(node, parent))
        return JS_ThrowDOMException(ctx, "HierarchyRequestError",
                                    "a node cannot be inserted into its own descendant"), false;
    /* STEP 3 */
    if (child && child->parent != parent)
        return JS_ThrowDOMException(ctx, "NotFoundError",
                                    "the reference child is not a child of this node"), false;
    /* STEP 4 */
    if (!node_is_document_fragment(node) && node->type != LXB_DOM_NODE_TYPE_DOCUMENT_TYPE &&
        node->type != LXB_DOM_NODE_TYPE_ELEMENT && !node_is_chardata(node))
        return JS_ThrowDOMException(ctx, "HierarchyRequestError",
                                    "only a DocumentFragment, DocumentType, Element or CharacterData node can "
                                    "be inserted"), false;
    /* STEP 5 — everything below this line is about a DOCUMENT parent, whose children the standard constrains
       far more tightly than any other node's. */
    if (parent->type != LXB_DOM_NODE_TYPE_DOCUMENT) {
        if (node->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE)                                  /* STEP 5.1 */
            return JS_ThrowDOMException(ctx, "HierarchyRequestError",
                                        "a doctype's parent can only be a document"), false;
        return true;                                                                        /* STEP 5.2 */
    }
    /* STEP 6. A CDATASection IS a Text node — §4.12 Interface CDATASection is `interface CDATASection : Text`
       — so both are the one case, and core/dom/text_content.h is where that sentence is written down. It was
       spelled out here and again at step 8 below and again at §4.4's textContent walk, and the fourth and
       fifth spellings of it (a `script`'s source text, a document's bundle identity) got it WRONG, which is
       what made one question asked in five places into one component asked from them. */
    if (dom_node_is_text(node))
        return JS_ThrowDOMException(ctx, "HierarchyRequestError",
                                    "a document cannot have a Text node child"), false;
    /* STEP 7 — the CharacterData nodes step 6 did not reject (Comment, ProcessingInstruction) are legal
       children of a Document at any position, so no further constraint applies to them. */
    if (node_is_chardata(node)) return true;
    /* STEP 8 */
    if (node_is_document_fragment(node)) {
        for (c = node->first_child; c; c = c->next) {
            if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) n_el++;
            else if (dom_node_is_text(c)) has_text = true;
        }
        if (n_el > 1 || has_text)                                                           /* STEP 8.1 */
            return JS_ThrowDOMException(ctx, "HierarchyRequestError",
                                        "a document cannot take a fragment holding more than one element or "
                                        "any text"), false;
        if (n_el == 0) return true;                                                         /* STEP 8.2 */
    }
    /* STEP 9 — a document has AT MOST ONE element child, and it must follow the doctype. */
    if (node_is_document_fragment(node) || node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        for (c = parent->first_child; c; c = c->next)
            if (c->type == LXB_DOM_NODE_TYPE_ELEMENT && c != exclude)
                return JS_ThrowDOMException(ctx, "HierarchyRequestError",
                                            "a document already has an element child"), false;
        for (c = child ? child->next : NULL; c; c = c->next)
            if (c->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE)
                return JS_ThrowDOMException(ctx, "HierarchyRequestError",
                                            "an element cannot be inserted before the doctype"), false;
        if (child && child->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE && child != exclude)
            return JS_ThrowDOMException(ctx, "HierarchyRequestError",
                                        "an element cannot be inserted before the doctype"), false;
        return true;                                                                        /* STEP 9.2 */
    }
    /* STEP 10 — the standard asserts this rather than testing it, and so does this engine: steps 4 through 9
       have eliminated every other kind, so a node reaching here that is not a doctype means one of those steps
       is reading the wrong type and the tree it would build is one no later step checks. */
    DCHECK(node->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE,
           "§4.2.3 step 10 asserts the node is a doctype, and it is not — steps 4-9 above have already returned "
           "or thrown for every other node kind, so reaching here with one means a step read the wrong type");
    /* STEP 11 — at most one doctype, and it must precede the element. */
    for (c = parent->first_child; c; c = c->next)
        if (c->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE && c != exclude)
            return JS_ThrowDOMException(ctx, "HierarchyRequestError",
                                        "a document already has a doctype child"), false;
    for (c = child ? child->prev : NULL; c; c = c->prev)
        if (c->type == LXB_DOM_NODE_TYPE_ELEMENT)
            return JS_ThrowDOMException(ctx, "HierarchyRequestError",
                                        "a doctype cannot be inserted after the element"), false;
    if (!child)
        for (c = parent->first_child; c; c = c->next)
            if (c->type == LXB_DOM_NODE_TYPE_ELEMENT && c != exclude)
                return JS_ThrowDOMException(ctx, "HierarchyRequestError",
                                            "a doctype cannot be appended after the element"), false;
    return true;
}

/* §4.2.3 "pre-insert" — validity, then the reference child, then insert. FOUR STEPS AND A RETURN, and the
   reason it is a function rather than three lines at each caller is that the three lines were NOT at each
   caller: seven members inserted with no validity check at all. */
bool node_pre_insert(JSContext *ctx, lxb_dom_node_t *node, lxb_dom_node_t *parent, lxb_dom_node_t *child)
{
    if (!node_ensure_pre_insert_valid(ctx, node, parent, child, NULL)) return false;   /* STEP 1, childrenToExclude « » */
    /* STEPS 2-3 are node_insert_at's `if (ref == node) ref = node->next`, decided there because the fragment
       arm needs the same adjustment and a caller cannot make it after the fragment has been emptied. */
    node_insert_at(parent, node, child);                                        /* STEP 4 */
    return true;
}

/* §4.2.3 "replace" — NOT a pre-insert followed by a remove, which is what `replaceChild` and `replaceWith` each
   wrote out and which is wrong in three ways the standard's own ordering fixes. (a) Its validity runs with
   childrenToExclude « child », so `document.replaceChild(newHtml, document.documentElement)` is legal where a
   pre-insert is not — a pre-insert would see the element child that is about to leave and throw. (b) Its
   reference child is child's NEXT sibling and the removal comes FIRST, so `p.replaceChild(n, n)` is a no-op
   returning n; inserting before n and then removing n took the node OUT of the tree. (c) The removal carries
   `suppressObservers` and the operation queues ONE tree mutation record naming both lists, where
   insert-then-remove queues two. Returns false HAVING THROWN. */
static bool node_replace(JSContext *ctx, lxb_dom_node_t *node, lxb_dom_node_t *child, lxb_dom_node_t *parent)
{
    lxb_dom_node_t *ref;

    DCHECK(child != NULL, "§4.2.3's replace was asked to replace nothing — `child` is the node the algorithm "
                          "removes and returns, and it is non-nullable in both members stated over this");
    if (!node_ensure_pre_insert_valid(ctx, node, parent, child, child)) return false;   /* STEP 1, « child » */
    ref = child->next;                                                          /* STEP 2 */
    if (ref == node) ref = node->next;                                          /* STEP 3 */
    /* STEPS 4-9. `previousSibling` and `referenceChild` are read off the tree by the scope at the moment the
       insertion happens, which — because the removal precedes it — is exactly the pair the standard binds at
       its steps 2 and 4. */
    /* STEP 6 IS BEFORE STEP 7, IT IS OUTSIDE THE SCOPE, AND BOTH OF THOSE ARE OBSERVABLE. Adopt removes `node`
       from the parent it already had, which is a removal of ITS OWN — the standard queues it as a separate
       record with the siblings `node` had at that moment, so it must run before `child` leaves and must not be
       folded into the operation's record. `p.replaceChild(p.lastChild, p.firstChild)` is where the sibling
       matters and `p.replaceChild(n, n)` is where the separation does: adopt takes `n` out, which makes step
       7's condition FALSE — the standard's own note — so that call is two records, one removing and one
       adding, and not one record naming `n` twice. */
    node_insert_adopt(parent, node);                                            /* STEP 6 */
    /* STEP 7's removal is the only one this operation suppresses, and only when there is one to suppress. */
    mutation_observer_batch_begin(parent, child->parent ? child : NULL, NULL);
    if (child->parent) dom_cow_remove_child(child);                             /* STEP 7 */
    node_insert_at(parent, node, ref);                                          /* STEP 9 */
    mutation_observer_batch_end();                                              /* STEP 10 */
    return true;
}

void node_insert_at(lxb_dom_node_t *parent, lxb_dom_node_t *node, lxb_dom_node_t *ref)
{
    if (node_is_document_fragment(node)) {
        lxb_dom_node_t *c, *next;
        /* §4.2.3 INSERT IS ONE OPERATION, and these are its N tree writes. Step 4 removes the fragment's
           children with `suppressObservers` set to TRUE and queues one record for the FRAGMENT; the last step
           queues one record for the PARENT with the whole node list. Without the scope the per-node hook
           queued one record per child at each end — which is what "expected 1 but got 2" reports. */
        mutation_observer_batch_begin(parent, NULL, node);
        for (c = node->first_child; c; c = next) {
            next = c->next;
            dom_cow_remove_child(c);            /* STEP 4, with `suppressObservers` set */
            node_insert_adopt(parent, c);       /* STEP 6.1, per node of `nodes` */
            if (ref) dom_cow_insert_before(ref, c);
            else     dom_cow_append_child(parent, c);
        }
        mutation_observer_batch_end();
        return;
    }
    /* §4.2.3 PRE-INSERT STEP 2: a reference child that IS the node becomes the node's next sibling, decided
       before the node leaves its place. */
    if (ref == node) ref = node->next;
    /* STEP 6.1. Adopt is what unlinks the node from its old parent (its own step 2) — lexbor's insert does NOT
       unlink, it writes the new parent's links over the old ones and leaves the old parent still naming the
       node as its child, so a MOVE (`doc.appendChild(p)` for a `p` that has a parent) built a tree with two
       parents and a sibling chain that loops. Every walk in this engine then runs forever, which is how it
       surfaced: §6.2's TreeWalker hung where the corpus regrafts a subtree. The removal used to be written
       here, under a comment naming §4.5 step 2 — one step of an algorithm, standing in for the algorithm. */
    node_insert_adopt(parent, node);
    if (ref) dom_cow_insert_before(ref, node);
    else     dom_cow_append_child(parent, node);
}

/* ---- DOM §4.2.3 "MOVE" ------------------------------------------------------------------------------------
 *
 * "To MOVE a node node into a node newParent before null or a node child" — the TWENTY-SIX steps, and the
 * algorithm §4.2.6's `moveBefore(node, child)` is three lines on top of.
 *
 * IT IS NOT A REMOVE FOLLOWED BY AN INSERT, and that is the entire feature rather than an optimisation. The
 * standard says so in its own note, at step 24.2: "Because the move algorithm is a separate primitive from
 * insert and remove, it does not invoke the insertion steps or removing steps for inclusiveDescendant." Those
 * are the steps that DESTROY the state a page uses `moveBefore` to keep — HTML §4.8.5's `iframe` removing steps
 * destroy the child navigable (so `frame.contentDocument` becomes a fresh blank document), HTML §2.1.4's
 * removing steps clear the document's focused area, and §4.13.3's disconnected/connected pair resets a custom
 * element. A `moveBefore` written as remove-then-insert therefore ends with the tree in the right SHAPE and the
 * state gone, and nothing throws: the plausible wrong answer, which is why this is its own algorithm down to
 * its own chokepoint pair (solver/dom_cow.h's move_out/move_in, which fire no tree hook at all).
 *
 * ITS VALIDITY IS NOT `ensure pre-insert validity`, AND THE DIFFERENCE IS NOT A SUBSET. Steps 1-6 are the move's
 * own six, and three of them have no pre-insert counterpart while three pre-insert steps have no move
 * counterpart:
 *   - step 1 (shadow-including roots must be the SAME) does not exist for pre-insert at all, and it is what
 *     makes every cross-document and connected↔disconnected move a HierarchyRequestError. The standard's own
 *     note states the consequence: "this has the side effect of ensuring that a move is only performed if
 *     newParent's connected is node's connected."
 *   - step 4 admits ONLY an Element or a CharacterData node, where pre-insert validity step 4 also admits a
 *     DocumentFragment and a DocumentType. `moveBefore(new DocumentFragment(), null)` throws.
 *   - pre-insert validity's step 1 (the parent must be a Document, DocumentFragment or Element) is absent
 *     because §4.2.6 puts `moveBefore` only on the three interfaces that include ParentNode, so there is no
 *     receiver that could fail it; its steps 5.1, 8 and 11 are absent because steps 4 and 5 here have already
 *     rejected every doctype and every fragment those steps are about.
 * So this does NOT go through node_ensure_pre_insert_valid, and calling that entry with a `childrenToExclude` of its
 * own would be the second copy of a check rather than the reuse it looks like: the two algorithms answer
 * different questions and only three of their eleven and six steps coincide.
 *
 * WHAT IT SHARES WITH `remove` IT SHARES BY CALL. Steps 9-10 are §5.5's live-range pre-remove steps and §6.1's
 * NodeIterator pre-remove steps; steps 14-16 are §4.2.2's removal-side slot steps and steps 21-23 its
 * insertion-side ones — each is word-for-word the text its component already implements, so each is that
 * component's function and not a copy. What is NOT shared is remove's step 15 (the transient registered
 * observers), which move's numbering does not contain: a node moved out of a subtree an observer watches with
 * `subtree: true` stops reporting to it, and that is the standard's answer, not an omission.
 *
 * A STRAIGHT-LINE C BODY, DELIBERATELY. Nothing in the twenty-six steps runs the page's code: the slot steps
 * assign and SIGNAL (which queues §4.3's microtask), the custom-element step ENQUEUES a reaction, and the
 * records are queued. The reactions run at the member's own `[CEReactions]` epilogue, which every declared
 * member converges on — so the move is one uninterrupted operation and its chokepoint pair can assert it.
 *
 * Returns false HAVING THROWN. */
static bool node_move(JSContext *ctx, lxb_dom_node_t *node, lxb_dom_node_t *new_parent, lxb_dom_node_t *child)
{
    lxb_dom_node_t *old_parent, *old_prev, *old_next, *new_prev, *d;
    bool new_parent_connected;

    DCHECK(node != NULL && new_parent != NULL,
           "§4.2.3's move was asked about no node or no newParent — both are non-nullable in the algorithm's "
           "own signature, and a member reaching here with one skipped its declaration's interface conversion");

    /* STEP 1 — "if newParent's shadow-including root is not the same as node's shadow-including root, then
       throw a HierarchyRequestError." */
    if (shadow_root_shadow_including_root(new_parent) != shadow_root_shadow_including_root(node))
        return JS_ThrowDOMException(ctx, "HierarchyRequestError",
                                    "a node can only be moved within the tree it is already in"), false;
    /* STEP 2 — host-including inclusive ancestor, which is the ONE step move and pre-insert share verbatim. */
    if (shadow_root_is_shadow_including_inclusive_ancestor(node, new_parent))
        return JS_ThrowDOMException(ctx, "HierarchyRequestError",
                                    "a node cannot be moved into its own descendant"), false;
    /* STEP 3 */
    if (child && child->parent != new_parent)
        return JS_ThrowDOMException(ctx, "NotFoundError",
                                    "the reference child is not a child of this node"), false;
    /* STEP 4 — "if node is not an Element or a CharacterData node". NARROWER THAN PRE-INSERT'S STEP 4: a
       DocumentFragment and a DocumentType are both legal to INSERT and neither can be moved. */
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT && !node_is_chardata(node))
        return JS_ThrowDOMException(ctx, "HierarchyRequestError",
                                    "only an Element or a CharacterData node can be moved"), false;
    /* STEP 5. A CDATASection IS a Text node — §4.12 Interface CDATASection is `interface CDATASection : Text`
       — so both are the one case, exactly as pre-insert validity step 6 has them. */
    if ((node->type == LXB_DOM_NODE_TYPE_TEXT || node->type == LXB_DOM_NODE_TYPE_CDATA_SECTION) &&
        new_parent->type == LXB_DOM_NODE_TYPE_DOCUMENT)
        return JS_ThrowDOMException(ctx, "HierarchyRequestError",
                                    "a document cannot have a Text node child"), false;
    /* STEP 6 — "if newParent is a document, node is an Element node, and either newParent has an element child,
       child is a doctype, or child is non-null and a doctype is following child". A document's ONE element
       child and its position relative to the doctype, which is pre-insert validity step 9's element arm with no
       `childrenToExclude`: move removes nothing on its way in, so there is no child to exclude. */
    if (new_parent->type == LXB_DOM_NODE_TYPE_DOCUMENT && node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        lxb_dom_node_t *c;

        for (c = new_parent->first_child; c; c = c->next)
            if (c->type == LXB_DOM_NODE_TYPE_ELEMENT && c != node)
                return JS_ThrowDOMException(ctx, "HierarchyRequestError",
                                            "a document already has an element child"), false;
        if (child && child->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE)
            return JS_ThrowDOMException(ctx, "HierarchyRequestError",
                                        "an element cannot be moved before the doctype"), false;
        for (c = child ? child->next : NULL; c; c = c->next)
            if (c->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE)
                return JS_ThrowDOMException(ctx, "HierarchyRequestError",
                                            "an element cannot be moved before the doctype"), false;
    }
    old_parent = node->parent;                                                             /* STEP 7 */
    /* STEP 8 — the standard ASSERTS this rather than testing it, and so does this engine. Step 1 is what makes
       it true: a node with no parent is its own shadow-including root, so it can only pass step 1 by BEING
       newParent's root, and step 2 has already rejected that. A null here means step 1 read the wrong root. */
    DCHECK(old_parent != NULL,
           "§4.2.3 move step 8 asserts oldParent is non-null and it is null — step 1's shadow-including-root "
           "equality is what guarantees a parentless node cannot reach here, so one of the two read a root the "
           "other did not");
    /* THE MOVE'S ONE DOCUMENT, asserted rather than adopted. §4.5's adopt is absent from the move's numbering
       because step 1 has already made it unnecessary: one shadow-including root is one tree, and every node in
       a tree in this engine shares its node document (node_insert_at adopts on the way in). A move that found
       two documents would be a move across a boundary step 1 was supposed to refuse. */
    DCHECK(node_document_of(node) == node_document_of(new_parent),
           "§4.2.3's move found `node` and `newParent` in two different node documents — step 1's "
           "shadow-including-root equality is what makes adopt absent from this algorithm, so two documents "
           "here means a tree was built without adopting and `ownerDocument` is already wrong");

    range_pre_remove(ctx, node);                                                           /* STEP 9 */
    node_iterator_pre_remove(ctx, node);                                                   /* STEP 10 */
    old_prev = node->prev;                                                                 /* STEP 11 */
    old_next = node->next;                                                                 /* STEP 12 */
    dom_cow_move_out(node);                                                                /* STEP 13 */
    /* STEPS 14-16 — §4.2.2's removal-side slot steps, WORD FOR WORD the text `remove` runs, which is why they
       are that component's function. They read the tree in the DETACHED state (slot_removed_steps asserts the
       detachment), which is the whole reason step 13 is not fused with steps 19-20. */
    slot_removed_steps(ctx, node, old_parent);
    /* STEP 17 — the live-range offsets in newParent past `child`'s index. It is stated BEFORE the insertion and
       over `child`'s index; range_did_insert states the same arithmetic AFTER it and over the inserted node's
       own index, which is the same number because the node lands exactly at `child`'s index. §4.2.3's insert
       step 5 is the same pair of clauses with a `count`, so this is one implementation and not two.
       Its "if child is non-null" guard is inside that call: with `child` null the node is APPENDED and its
       index is the parent's last, so no offset can be greater than it. */
    new_prev = child ? child->prev : new_parent->last_child;                               /* STEP 18 */
    dom_cow_move_in(new_parent, node, child);                                              /* STEPS 19-20 */
    range_did_insert(ctx, node);                                                           /* STEP 17 */
    /* STEPS 21-23 — §4.2.2's insertion-side slot steps, again the same text `insert` runs. */
    slot_insert_steps(ctx, node, new_parent);
    /* STEP 24 — "for each shadow-including inclusive descendant inclusiveDescendant of node, in shadow-
       including tree order". The walk is the shared shadow-including successor, so `<slot>`s and shadow trees
       inside the moved subtree are visited exactly as §4.5's adopt visits them.
       "newParent is connected" IS READ ONCE, HERE, and not per descendant: it is a fact about the move, and a
       per-node `isConnected` would answer the same thing N times over a tree walk of the page's own depth. */
    new_parent_connected = node_is_connected(new_parent);
    for (d = node; d; d = shadow_root_next_in_shadow_including(ctx, d, node)) {
        bool is_subtree_root = d == node;                                                  /* STEP 24.1 */
        int i;

        for (i = 0; i < g_moving_hook_n; i++)                                              /* STEP 24.2 */
            g_moving_hooks[i](ctx, d, is_subtree_root, old_parent);
        /* STEP 24.3. The realm is the NODE'S DOCUMENT'S, for the reason §4.2.3's other per-node steps resolve
           theirs that way: two same-origin documents are one agent, so the flow performing the move is
           routinely not standing in the realm whose Document the element belongs to, and a reaction enqueued
           out of the mutating realm would name that realm's registry. */
        if (new_parent_connected && d->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            JSContext *realm = document_realm_of(d);

            DCHECK(realm != NULL,
                   "§4.2.3's move reached a connected element in a document no realm was installed for — a "
                   "document that can hold a connected node is a document a flow can run steps in");
            custom_elements_moved(realm, lxb_dom_interface_element(d));
        }
    }
    /* STEPS 25-26 — the TWO tree mutation records, one for each parent, with the siblings bound above. */
    mutation_observer_move_steps(ctx, node, old_parent, old_prev, old_next, new_parent, new_prev, child);
    return true;
}

/* §4.4 appendChild / removeChild. DECLARED members, like every other one — they were raw JS_CFUNC_DEF entries,
   which is a shape that cannot park at all and, more to the point here, does not pass through the machine every
   declared member converges on. `Node node` is an INTERFACE type, so the IDL coerces nothing and REFUSES
   everything that is not a node; the declaration is what puts them on that path. magic 0 = appendChild,
   1 = removeChild.
   THE NUMBER WAS §4.5 HERE AND AT THREE MORE SITES IN THIS FILE, and §4.5 is "Interface Document" — these four
   members are declared AND given their method steps in §4.4 "Interface Node" ("The appendChild(node) method
   steps are to return the result of appending node to this"), and it is the ALGORITHM they call that lives
   elsewhere, in §4.2.3 "Mutation algorithms". Two sections, and the wrong one read as authoritative. */
static JSValue js_node_child_op(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_node_t *n = node_of(this_val), *child;

    if (!n) return JS_UNDEFINED;
    /* `[CEReactions] Node appendChild(Node node)` / `[CEReactions] Node removeChild(Node child)`. BOTH
       POSITIONS ARE NON-NULLABLE INTERFACE TYPES, so §3.6's arity check and §3.2.15's brand are the
       DECLARATION's and stand in front of this body — the `argc < 1` that answered `undefined` where step 5
       throws, and the hand-written TypeError that stood where the brand belongs, are what they replaced. */
    DCHECK(argc == 1, "a §4.4 child operation reached its body without its declared argument — the position is "
                      "not optional, so §3.6 Overload resolution algorithm's step 5 (\"If S is empty, then "
                      "throw a TypeError\") is what a zero-argument call gets");
    child = node_of(argv[0]);
    DCHECK(child != NULL, "appendChild/removeChild's `Node` argument reached the body as something that is not "
                          "a node — the declaration's IDL_INTERFACE position and its "
                          "idl_iface_brand(node_class_id()) are what make that a TypeError before step 1");
    if (magic) {
        if (child->parent != n)
            return JS_ThrowDOMException(ctx, "NotFoundError", "the node to remove is not a child of this node");
        dom_cow_remove_child(child);
    } else {
        /* §4.4's `appendChild(node)` method steps are `append`, which §4.2.3 Mutation algorithms defines as
           pre-insert with a null child — all eleven validity steps, not the one
           ancestor check that used to stand here. */
        if (!node_pre_insert(ctx, child, n, NULL)) return JS_EXCEPTION;
    }
    return JS_DupValue(ctx, argv[0]);
}

/* §4.2.7/§4.2.8 THE ChildNode AND ParentNode CONVENIENCE MIXINS — `el.remove()`, `parent.append(a, b)`,
   `el.before(node, 'text')`, `el.replaceWith(x)`, `parent.replaceChildren()`. They are on Element, on
   CharacterData and on DocumentFragment, which is why they live here on the base rather than in element.c.
   THEY ARE NOT SUGAR IN THIS ENGINE. A bundle that builds its UI with `append` and tears it down with
   `remove()` had none of it: the page's own call threw, and every fetch behind that render never happened.
   §4.2.4 "convert nodes into a node": a STRING argument becomes a Text node, which is the whole reason these
   take `(Node or DOMString)...` — `el.append('hello')` is the ordinary way to write text.
   Every insertion and removal goes through the per-flow chokepoints, so the tree steps run (a custom element
   appended this way is upgraded) and the whole thing time-travels like any other DOM write.
   magic 0 = remove, 1 = before, 2 = after, 3 = replaceWith, 4 = append, 5 = prepend, 6 = replaceChildren. */
static lxb_dom_node_t *node_from_arg(JSContext *ctx, lxb_dom_node_t *owner, JSValueConst v)
{
    lxb_dom_node_t *n = node_of(v);
    const char *s;
    size_t slen = 0;
    lxb_dom_text_t *text;

    if (n) return n;
    /* §4.2.4 "convert nodes into a node": anything that is not a Node is a DOMString, and became one in the
       DECLARATION — `(Node or DOMString)...` is a variadic union the IDL machine converts, brand-checking each
       argument against the node class. It used to be a JS_ToCStringLen right here, which ran the page's
       toString FROM C with no flow base to park into: `el.append({toString(){ for(;;){} }})` was the
       drive-to-completion this engine exists not to have. */
    DCHECK(JS_IsString(v), "a ChildNode/ParentNode argument reached the body unconverted — the declaration is "
                           "what turns a non-Node into a DOMString, and doing it here runs the page's code "
                           "from C");
    s = JS_ToCStringLen(ctx, &slen, v);
    if (!s) return NULL;
    text = lxb_dom_document_create_text_node(owner->owner_document, (const lxb_char_t *)s, slen);
    JS_FreeCString(ctx, s);
    if (!text) return NULL;
    /* THIS FLOW MADE IT, so the delta owns it and destroys it when the delta is discarded — the same record
       `createTextNode` and `splitText` push at their own creation sites. The INSERTION that follows is a
       different fact (an insertion also covers a baseline node being moved), so it cannot stand in for this:
       without the entry the node was detached on revert and then leaked into the document's arena, where no
       GC walk names it. */
    dom_cow_note_created(lxb_dom_interface_node(text));
    return lxb_dom_interface_node(text);
}

/* §4.2.8's "is one of `nodes`" — the membership test the two viable-sibling steps are written over. A linear
   scan of the argument list is the whole of it: the list is the call's own arguments, so it is as long as the
   page wrote and no shorter, and a set would be a structure to build and free for a walk that is usually one
   step. */
static bool mixin_names_node(int argc, JSValueConst *argv, const lxb_dom_node_t *cand)
{
    int i;

    for (i = 0; i < argc; i++)
        if (node_of(argv[i]) == cand) return true;
    return false;
}

/* §4.2.6 "to convert nodes into a node" — the FIVE steps, which produce ONE node: each string becomes a Text
   node, a single node is returned as itself, and anything else is appended into a fresh DocumentFragment. Every
   ChildNode/ParentNode member is stated over the RESULT of this, which is why they each take
   `(Node or DOMString)...` and each perform exactly one pre-insert. Returns NULL having thrown. */
static lxb_dom_node_t *node_convert_into_a_node(JSContext *ctx, lxb_dom_node_t *owner,
                                                int argc, JSValueConst *argv)
{
    lxb_dom_document_fragment_t *frag;
    lxb_dom_node_t *fnode;
    int i;

    if (argc == 1) return node_from_arg(ctx, owner, argv[0]);          /* STEPS 1-2 */
    frag = lxb_dom_document_fragment_interface_create(owner->owner_document);   /* STEP 3 */
    CHECK(frag != NULL, "§4.2.6's convert-nodes-into-a-node could not create the fragment its result is");
    fnode = lxb_dom_interface_node(frag);
    /* THIS FLOW MADE IT — the same record createDocumentFragment pushes at its own creation site. The fragment
       is emptied by the insertion that consumes it and never becomes reachable from the tree, so without the
       entry it is left in the document's Lexbor arena owned by nothing and invisible to the GC walk. */
    dom_cow_note_created(fnode);
    for (i = 0; i < argc; i++) {                                       /* STEP 4 */
        lxb_dom_node_t *x = node_from_arg(ctx, owner, argv[i]);
        if (!x) return NULL;
        node_insert_at(fnode, x, NULL);       /* "append node to fragment" — §4.2.3's append, not a raw link */
    }
    return fnode;                                                      /* STEP 5 */
}

static JSValue js_node_mixin(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_node_t *n = node_of(this_val), *parent, *ref, *added;

    if (!n) return JS_UNDEFINED;
    parent = n->parent;
    /* §4.2.7: a node with no parent has nothing to be removed from or inserted beside — and that is a no-op,
       not an error, which is what lets a page call `el.remove()` twice. */
    if (magic <= 3 && !parent) return JS_UNDEFINED;

    if (magic == 0) { dom_cow_remove_child(n); return JS_UNDEFINED; }   /* §4.2.8 remove */

    /* §4.2.8 STEP 2 — the VIABLE sibling, computed BEFORE the conversion because the conversion MOVES the
       nodes it is given and a reference child that moves is a reference to nothing. It is the nearest sibling
       in the member's direction that is not one of `nodes`, which is what makes `el.after(a, el.nextSibling)`
       land the pair where the standard says rather than around a sibling that is about to leave. */
    if (magic == 1) {
        for (ref = n->prev; ref && mixin_names_node(argc, argv, ref); ref = ref->prev) {}
    } else if (magic == 2 || magic == 3) {
        for (ref = n->next; ref && mixin_names_node(argc, argv, ref); ref = ref->next) {}
    } else {
        ref = NULL;
    }

    /* §4.2.6 "convert nodes into a node" — ONE node, which is a DocumentFragment when there is more than one.
       This loop used to pre-insert each argument SEPARATELY, which is a different algorithm in three visible
       ways: `document.append(a, b)` must throw for the pair rather than insert `a` and then reject `b`;
       `el.before(a, b)` is ONE tree mutation record, not two; and a validity check made per argument cannot see
       the fragment the standard checks. */
    added = node_convert_into_a_node(ctx, n, argc, argv);
    if (!added) return JS_EXCEPTION;
    /* §4.2.8 before STEP 5, and it is AFTER the conversion on purpose: the conversion moves nodes out of this
       parent, so both `parent's first child` and `viablePreviousSibling's next sibling` are read from the tree
       the insertion will actually happen in. `after` and `replaceWith` have no such step — they pre-insert
       before the viable NEXT sibling as it stands. */
    if (magic == 1) ref = ref ? ref->next : parent->first_child;

    /* §4.2.6 and §4.2.8 STATE EVERY ONE OF THESE OVER "PRE-INSERT" or over "replace", and this loop wrote the
       tree links directly — so the seven convenience members were the seven places §4.2.3's validity was not
       checked at all. `document.append(text)` and `document.prepend(el)` built the trees appendChild refuses. */
    switch (magic) {
    case 1: case 2: if (!node_pre_insert(ctx, added, parent, ref)) return JS_EXCEPTION; break;
    /* §4.2.8 replaceWith STEP 4: `this`'s parent is checked AGAIN, because the conversion above may have moved
       `this` into the fragment; only when it is still there is this the REPLACE algorithm. */
    case 3: if (n->parent == parent) { if (!node_replace(ctx, added, n, parent)) return JS_EXCEPTION; }
            else if (!node_pre_insert(ctx, added, parent, ref)) return JS_EXCEPTION;
            break;
    case 4: if (!node_pre_insert(ctx, added, n, NULL)) return JS_EXCEPTION; break;
    case 5: if (!node_pre_insert(ctx, added, n, n->first_child)) return JS_EXCEPTION; break;
    case 6:
        /* §4.2.6 replaceChildren STEPS 2-3: the validity is ensured BEFORE anything is removed, which is the
           whole point of it being a separate step — this used to empty the parent first, so a call that must
           throw destroyed the children on its way to not throwing at all.
           WHAT IS NOT BUILT HERE is §4.2.3's "replace all" record: the standard removes every child with
           suppressObservers set and queues ONE tree mutation record naming both lists, and these removals still
           queue one record each. That needs a suppress-ALL scope, which mutation_observer.h's scope does not
           have — it suppresses the ONE child `replace` names. */
        if (!node_ensure_pre_insert_valid(ctx, added, n, NULL, NULL)) return JS_EXCEPTION;
        {
            lxb_dom_node_t *c = n->first_child, *next;
            for (; c; c = next) { next = c->next; dom_cow_remove_child(c); }
        }
        node_insert_at(n, added, NULL);
        break;
    default: DFAIL("a ChildNode/ParentNode member ran with an unknown magic"); break;
    }
    return JS_UNDEFINED;
}

/* §4.2.6 `[CEReactions] undefined moveBefore(Node node, Node? child)` — THREE STEPS on top of §4.2.3's move.
 *
 * IT IS NOT ON Node.prototype. §4.2.6 declares it on the ParentNode mixin, so it exists on exactly the three
 * interfaces whose IDL includes ParentNode — Document, DocumentFragment and Element — and `"moveBefore" in
 * document.doctype` is FALSE. That is not a detail of where the installer is called from: a DocumentType and a
 * Text are the two node kinds §4.2.3's move can be given and can never be given TO, so a `moveBefore` on
 * Node.prototype would be a member every node advertises and two thirds of them throw from.
 *
 * ITS TWO ARGUMENTS ARE INTERFACE TYPES AND THE DECLARATION IS WHAT ENFORCES THEM: `Node node` is required and
 * non-nullable, `Node? child` is required and nullable, and neither is optional — so `body.moveBefore(t)` is a
 * TypeError for its arity and `body.moveBefore({}, null)` is a TypeError for its type, both thrown by the one
 * machine every declared member converges on rather than by a test in this body. */
static JSValue js_node_move_before(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_node_t *parent = node_of(this_val), *node, *ref;

    (void)magic;
    DCHECK(argc == 2, "moveBefore reached its body without the two arguments its declaration requires — both "
                      "positions are non-optional, so §3.6 step 5's arity TypeError is what stands in front of "
                      "this body");
    if (!parent)
        return JS_ThrowTypeError(ctx, "moveBefore called on something that is not a Node");
    node = node_of(argv[0]);
    DCHECK(node != NULL, "moveBefore's `Node node` reached the body as something that is not a node — the "
                         "declaration's IDL_INTERFACE position is what makes that a TypeError before step 1");
    ref = node_of(argv[1]);                                             /* STEP 1 */
    /* STEP 2 — "if referenceChild is node, then set referenceChild to node's next sibling", read BEFORE the
       move takes the node out of its place. `a.moveBefore(b, b)` is the no-op the standard's own test names,
       and without this step it would move `b` before itself, which is a position that stops existing at step
       13. §4.2.3's pre-insert has the identical step for the identical reason. */
    if (ref == node) ref = node->next;                                  /* STEP 2 */
    if (!node_move(ctx, node, parent, ref)) return JS_EXCEPTION;        /* STEP 3 */
    return JS_UNDEFINED;                                                /* `undefined` return type */
}

/* §4.10's CharacterData is FOUR node kinds, not two. Text and Comment were the only ones the engine could
   build, so the predicate said so — and the moment §4.5 grew `createCDATASection` and
   `createProcessingInstruction`, `cdata.data` became a TypeError on a node whose whole interface is
   CharacterData's. A CDATASection is a Text, and a ProcessingInstruction is a CharacterData; both carry
   `data`, `length` and §4.10's five splices. */
static bool node_is_chardata(const lxb_dom_node_t *n)
{
    return n->type == LXB_DOM_NODE_TYPE_TEXT || n->type == LXB_DOM_NODE_TYPE_COMMENT ||
           n->type == LXB_DOM_NODE_TYPE_CDATA_SECTION ||
           n->type == LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION;
}

/* §4.4 Node.nodeValue and §4.10 CharacterData.data — THE SAME TEXT THROUGH TWO MEMBERS WITH DIFFERENT RULES,
   which is why one body takes a magic rather than one member aliasing the other (magic 0 = data, 1 = nodeValue).
   They were aliased and both lived on the character-data members, which got the hierarchy and the types wrong in
   three ways at once. nodeValue is a member of NODE, so `element.nodeValue` must answer null and not be absent.
   nodeValue is `DOMString?`, so `n.nodeValue = null` empties the text — writing the four characters `null` into
   the page's DOM, which is what a ToString did, is the setter skipping its own first step. And data is
   `[LegacyNullToEmptyString] DOMString` on CharacterData, so it is brand-checked rather than silently answering
   for an element. */
static JSValue js_cd_get_data(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    lxb_dom_character_data_t *cd;

    if (!n) return JS_UNDEFINED;
    if (!node_is_chardata(n)) {
        if (magic == 1)
            return JS_NULL;    /* §4.4: nodeValue of anything that is not character data is null */
        return JS_ThrowTypeError(ctx, "CharacterData.data read on a node that holds no character data");
    }
    cd = lxb_dom_interface_character_data(n);
    /* …AND THE SECOND DOOR OUT OF A §4.12.1 DATA BLOCK. `el.firstChild.data` and `el.firstChild.nodeValue`
       deliver the same bytes `el.textContent` does, so they ask the same question — an entry that skipped it
       would report a record the SERVER filled as one the bundle composed (HTML §4.12.1 The script element,
       and core/loader/data_block.h).
       ONLY WHEN THIS NODE IS THE WHOLE CONTENT, which is what makes the two doors deliver ONE value under one
       provenance. What an author's code reads out of a data block is DOM §4.11 Interface Text's CHILD TEXT
       CONTENT of the element — "the concatenation of the data of all the Text node children" — so a `script`
       with several Text children (`el.appendChild(document.createTextNode(…))` after a parse) has more of it
       than this node holds, and naming a fragment after the block would give two different values one identity — which is
       one name for several unknowns, and every predicate over either would then decide both. */
    if (n->parent && n->parent->first_child == n && n->next == NULL)
        return data_block_wrap_text(ctx, lxb_dom_interface_element(n->parent),
                                    JS_NewStringLen(ctx, (const char *)cd->data.data, cd->data.length));
    return JS_NewStringLen(ctx, (const char *)cd->data.data, cd->data.length);
}

/* THE SETTER IS "REPLACE DATA", NOT A RAW WRITE, and the difference is a whole class of live ranges going
   stale. §4.10's `data` setter and §4.4's `nodeValue` setter are both defined as *replace data with node this,
   offset 0, count this's length, and data the new value* — so `t.data = "x"` runs the same steps 8-11 that
   `t.replaceData(0, t.length, "x")` does, and every boundary point inside `t` collapses onto 0. Writing the
   bytes straight through skipped those steps: the text changed under a live range and the range kept pointing
   past the end of a node that no longer had that many code units. */
static uint32_t cd_units(const lxb_char_t *s, size_t len);

/* The value arrives CONVERTED — a real string, JS_NULL for the nullable member, or a concolic that crossed as
   itself. Nothing here runs the page's code, which is the claim the declaration in node_init makes. */
static JSValue js_cd_set_data(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    lxb_dom_character_data_t *cd;
    const char *s;
    size_t len;
    JSValue r;

    if (!n) return JS_UNDEFINED;
    if (!node_is_chardata(n)) {
        if (magic == 1)
            return JS_UNDEFINED;   /* §4.4 nodeValue setter: "otherwise: do nothing" */
        return JS_ThrowTypeError(ctx, "CharacterData.data set on a node that holds no character data");
    }
    cd = lxb_dom_interface_character_data(n);
    /* §4.4 the nodeValue setter's own first step; data reaches this through [LegacyNullToEmptyString]. */
    if (JS_IsNull(val))
        return node_cd_replace_data(ctx, n, 0, cd_units(cd->data.data, cd->data.length), "", 0);
    /* Unknown external input has no bytes: its SHAPE is what the node carries, the same answer textContent
       gives, so a source parked in the tree as text still displays as the source it came from. */
    if (concolic_is(val)) {
        s = concolic_shape_c(val);
        return node_cd_replace_data(ctx, n, 0, cd_units(cd->data.data, cd->data.length), s ? s : "",
                               s ? strlen(s) : 0);
    }
    DCHECK(JS_IsString(val), "a CharacterData write reached the body unconverted — the IDL declaration is what "
                             "converts it, and running the page's toString from here is the drive-to-completion "
                             "the flow machinery exists to avoid");
    s = JS_ToCStringLen(ctx, &len, val);
    if (!s) return JS_EXCEPTION;
    r = node_cd_replace_data(ctx, n, 0, cd_units(cd->data.data, cd->data.length), s, len);
    JS_FreeCString(ctx, s);
    return r;
}

/* The byte length of the UTF-8 sequence a lead byte opens. A continuation byte reaching here would be a
   sequence this walk started inside, which Lexbor's storage cannot produce. */
static size_t cd_seq_len(unsigned char c)
{
    return c < 0x80 ? 1 : (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4));
}

/* §4.4's LENGTH IS IN UTF-16 CODE UNITS, AND LEXBOR STORES UTF-8 — so every offset §4.10 takes is a code-unit
   offset into a byte string, and answering `data.length` was answering BYTES. `t.data = "é"; t.length` said 2
   where every engine says 1, and every offset a page computed from that length then addressed the wrong place.
   One conversion, used by `length`, by substringData and by "replace data", because three copies of a UTF-8
   walk is three chances to disagree about where a code point ends. */
static uint32_t cd_units(const lxb_char_t *s, size_t len)
{
    uint32_t u = 0;
    size_t i = 0;

    while (i < len) {
        size_t adv = cd_seq_len(s[i]);
        if (i + adv > len) adv = len - i;   /* a truncated sequence counts as the bytes that are there */
        u += (adv == 4) ? 2 : 1;            /* a code point above the BMP is a SURROGATE PAIR: two units */
        i += adv;
    }
    return u;
}

/* The BYTE offset at which code-unit offset `want` begins, clamped to `len`.
   A `want` that lands BETWEEN the two halves of a surrogate pair answers the byte offset of the WHOLE pair:
   UTF-8 has no representation for half a surrogate, so a split there cannot be stored. That is a limit of the
   store and not of this walk — it is where CharacterData-surrogates diverges from a UTF-16 engine — and it is
   clamped rather than rounded so the split never lands mid-sequence and corrupts the text. */
static size_t cd_byte_of(const lxb_char_t *s, size_t len, uint32_t want)
{
    uint32_t u = 0;
    size_t i = 0;

    while (i < len && u < want) {
        size_t adv = cd_seq_len(s[i]);
        if (i + adv > len) adv = len - i;
        u += (adv == 4) ? 2 : 1;
        i += adv;
    }
    return i;
}

static JSValue js_cd_get_length(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_node_t *n = node_of(this_val);
    lxb_dom_character_data_t *cd;

    if (!n) return JS_NewInt32(ctx, 0);
    if (!node_is_chardata(n))
        return JS_ThrowTypeError(ctx, "CharacterData.length read on a node that holds no character data");
    cd = lxb_dom_interface_character_data(n);
    return JS_NewInt32(ctx, (int)cd_units(cd->data.data, cd->data.length));
}

/* §4.10 "REPLACE DATA", and the four members that ARE it with different operands — appendData is
   (length, 0, data), insertData is (offset, 0, data), deleteData is (offset, count, "") and replaceData is
   itself. Written once because they are one algorithm; four bodies would be four places for the IndexSizeError
   and the count clamp to drift, and the standard states them once for the same reason.
   WHAT IS NOT HERE, and is therefore honestly absent rather than half-done: step 4's characterData mutation
   record (MutationObserver is not built).
   STEPS 8-11 ARE §5.5's AND ARE CALLED, NOT COPIED. They walk the live-range registry, which is range.c's, and
   the operands they need are exactly this algorithm's: the offset and the count AFTER step 3's clamp, and the
   replacement's length in CODE UNITS, never in bytes. That last one is why the call belongs here and not at
   each of the five members — `insertData(0, "é")` moves a boundary point by ONE, and a member handing over
   `data_len` would move it by two. */
JSValue node_cd_replace_data(JSContext *ctx, lxb_dom_node_t *n, uint32_t offset, uint32_t count,
                               const char *data, size_t data_len)
{
    lxb_dom_character_data_t *cd = lxb_dom_interface_character_data(n);
    const lxb_char_t *s = cd->data.data;
    size_t len = cd->data.length, a, b, out_len;
    uint32_t length = cd_units(s, len);
    char *out;

    if (offset > length)                                            /* step 2 */
        return JS_ThrowDOMException(ctx, "IndexSizeError",
                                    "the offset is past the end of the character data");
    if ((uint64_t)offset + count > length)                          /* step 3 */
        count = length - offset;
    a = cd_byte_of(s, len, offset);                                 /* steps 5-7, as one splice */
    b = cd_byte_of(s, len, offset + count);
    out_len = a + data_len + (len - b);
    out = malloc(out_len + 1);
    CHECK(out != NULL, "CharacterData: OOM splicing a node's data — a dropped write is a document that "
                       "disagrees with the program that wrote it");
    memcpy(out, s, a);
    memcpy(out + a, data, data_len);
    memcpy(out + a + data_len, s + b, len - b);
    out[out_len] = 0;
    dom_cow_set_text(n, out, out_len);   /* the chokepoint: capture-then-mutate, per flow */
    free(out);
    /* STEPS 8-11 — §5.5's, over the operands this algorithm settled on. */
    range_replace_data_steps(ctx, n, offset, count, cd_units((const lxb_char_t *)data, data_len));
    /* THE LAST STEP — "if node's parent is non-null, then run the children changed steps for node's parent".
       It is §4.2.3's third caller and the ONLY one no tree hook can stand in for: nothing moved in the tree, so
       `styleEl.firstChild.data = '…'` is invisible to every mutation chokepoint and visible here. */
    node_children_changed(ctx, n->parent);
    return JS_UNDEFINED;
}

/* AN `unsigned long` OPERAND OF §4.10 / §4.11, KNOWN AND UNKNOWN — the elimination chain
   core/idl_index_arg.h holds, reached by the five CharacterData members and by §4.11's `splitText`.

   WHY THESE OPERANDS ARE UNKNOWN AT ALL, AND NOT WITH A JS_ToUint32 OF THIS FILE'S OWN, which is the shape
   core/idl_args.h bans by name: "A BODY MAY NOT CALL JS_ToFloat64 ON ITS OWN ARGUMENT". §3.2's conversion is a
   BOUNDARY that unknown external input crosses AS ITSELF — idl_concolic_rule answers IDL_CONCOLIC_CROSSES for
   every integer type, IDL_UNSIGNED_LONG among them — so `t.substringData(i, 2)` with an unknown `i` reaches
   this body still holding the unknown, and a raw coercion of it owes C a real number it cannot have. That
   coercion does not merely return a wrong number: ToNumber hands a concolic straight back, so the engine
   aborts INSIDE the coercion, one frame below this file. CHECKING THE COERCION'S RETURN VALUE IS NO DEFENCE
   AND NEVER WAS — the abort happens before there is a return to check.

   WHAT STOOD HERE, AND WHAT ITS "NEXT DIFF" CLAUSE PRESCRIBED — recorded at the site that made the claim,
   because a hand-off's mechanism clause is READ ONCE, BY SOMEONE WHO HAS ALREADY DECIDED TO DO THE WORK.
   A `CD_UNSIGNED_LONG` macro DFAILed on an unknown operand and said, in full: "BUILD THE FORK: make this body
   an IdlStepBody (core/idl_args.h, IDL_STEP_FIRST) so it can park, then ask step 2 through step_fork_run over
   the unknown, taking the real arm from idl_number_of's example". The FIRST half was right and is what this
   diff did. THE SECOND HALF WAS WRONG, and following it would have produced a machine that parks and then
   aborts one line later: `step_fork_run` over "is offset greater than length" answers the COMPARISON and
   leaves the operand UNKNOWN on the arm that continues, and every step after step 2 needs the NUMBER —
   substring data step 4's "code units from the offsetth code unit to the offset+countth code unit", replace
   data step 5's "insert data into node's data after offset code units", split a Text node step 3's
   "length − offset". The body would have reached `cd_byte_of` still holding a concolic, which is the very
   defect the clause was written to end, moved two lines down. §3.2.4.6 unsigned long's
   ConvertToInt(V, 32, "unsigned") is TOTAL over [0, 2**32-1], so "is it past the end" and "which position is
   it" are ONE question — and the elimination chain is that question's decomposition, which is why it and not a
   bare two-armed outcome fork is what these members ask.

   WHAT EACH OPERAND'S CHAIN IS DRAWN OVER, per the standards' own bounds (verified against the fetched text):
   `offset` — §4.10 substring data step 2 and replace data step 2 are both "If offset is greater than length,
   then throw an "IndexSizeError" DOMException", and §4.11 split a Text node step 2 is the same sentence. It is
   `>` and not `>=`, because a position at the very END is legal, so the chain runs over `length + 1` positions
   and EXHAUSTION is step 2's throw. That is the same `npositions` parameterization CSSOM §6.4's insert a CSS
   rule needs, which is why the count of positions is the component's parameter and the bound is not.
   `count` — substring data step 3 is "If offset + count is greater than length, then return a string whose
   value is the code units from the offsetth code unit to the end of node's data" and replace data step 3 is
   "If offset + count is greater than length, then set count to length − offset". Every `count` above
   `length − offset` reaches ONE answer, so the chain runs over `length − offset + 1` and exhaustion is that
   answer. See core/idl_index_arg.c's banner for why an operand that names no position is still a member.

   THE PAST-THE-END WORLD IS STATED HERE AND NEVER BY THE COMPONENT — a throw for `offset`, the clamp for
   `count` — which is the one thing idl_index_chain_run refuses to decide for anybody. */
#define CD_SUBSTRING_ALGORITHM "DOM §4.10 Interface CharacterData substringData(offset, count)"
#define CD_APPEND_ALGORITHM    "DOM §4.10 Interface CharacterData appendData(data)"
#define CD_INSERT_ALGORITHM    "DOM §4.10 Interface CharacterData insertData(offset, data)"
#define CD_DELETE_ALGORITHM    "DOM §4.10 Interface CharacterData deleteData(offset, count)"
#define CD_REPLACE_ALGORITHM   "DOM §4.10 Interface CharacterData replaceData(offset, count, data)"
#define CD_SPLIT_ALGORITHM     "DOM §4.11 Interface Text splitText(offset)"

/* ONE STAGE PER MEMBER, AND IT IS ONE BECAUSE THE ALGORITHM IS ONE ENGINE ACTION FROM END TO END. Steps 1-8
   read a length, settle two operands, splice a Lexbor string and walk the live-range registry; none of them
   runs the page's code, so there is no point inside the run at which the engine may have to park that is not
   the chain's own ask — and step_fork_run is where THAT parks, with the machine's state cloned through the
   visit below. The label names the range in those terms, which is what quickjs-step.h's JSTrampStepDef::steps
   requires of a range at all.
   SIX LISTS AND NOT ONE, because `algorithm` and `steps` are the ADDRESS a should-never-happen reports and the
   label a parked flow SAYS: one declaration shared by five members would name substringData for a flow parked
   inside replaceData, which is CLAUDE.md's assert-that-names-a-remedy-but-not-a-site defect arriving through a
   shared declaration instead of a shared helper. The body is still one, because the algorithm is. */
#define CD_OP_STAGES(X) X(CD_OP_RUN, "DOM §4.10 / §4.11 steps 1-8 over the operands this call settled on")
enum { IDL_STEP_STAGE_BASE(CD_OP_STAGES) CD_OP_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const CD_SUBSTRING_STEPS[] = { CD_SUBSTRING_ALGORITHM " steps 1-4", NULL };
static const char *const CD_APPEND_STEPS[]    = { CD_APPEND_ALGORITHM " (replace data with length, 0, data)", NULL };
static const char *const CD_INSERT_STEPS[]    = { CD_INSERT_ALGORITHM " (replace data with offset, 0, data)", NULL };
static const char *const CD_DELETE_STEPS[]    = { CD_DELETE_ALGORITHM " (replace data with offset, count, \"\")", NULL };
static const char *const CD_REPLACE_STEPS[]   = { CD_REPLACE_ALGORITHM " steps 1-8", NULL };
static const char *const CD_SPLIT_STEPS[]     = { CD_SPLIT_ALGORITHM " steps 1-9", NULL };

/* THE TWO CHAINS ONE §4.10 CALL CAN RUN, IN THE ORDER THE ALGORITHM ASKS THEM. `off` is first because
   core/idl_index_arg.h requires an IdlIndexChain to be the state's FIRST field when a member needs more than
   one, and because `cnt`'s own bound is `length − offset + 1`: the second chain cannot be drawn until the
   first has answered.
   RE-ENTRY RE-ASKS AND THAT IS THE MECHANISM RATHER THAN A COST. step_fork_run's own contract is that "both a
   park and a cross-session resume land back on the ask, which re-derives the same arm from the flow's decision
   vector", so a body that parks inside `cnt` and is resumed re-runs `off` from its own cursor and is answered
   by the record it already made. That is why neither chain needs a stage of its own and why the member has no
   private "which chain am I in" byte — a resume point nothing can check is exactly what IDL_STEP_FIRST's
   banner forbids. */
typedef struct {
    IdlIndexChain off;
    IdlIndexChain cnt;
} CdIndexState;

static void cd_index_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    /* NOTHING IS OWNED. Both fields are IdlIndexChains, which core/idl_index_arg.h declares hold no JSValue —
       a cursor and the buffer its constraint key is spelled into. This is written out rather than pointed at
       idl_index_chain_visit because a member EMBEDDING that state must name the rest of its own, and the rest
       of this one is a second chain that owns nothing either. */
    (void)ctx; (void)st; (void)v;
}

/* §4.10's five members. magic: 0 substringData, 1 appendData, 2 insertData, 3 deleteData, 4 replaceData.
   THE `DOMString` ARGUMENT ARRIVES CONVERTED OR UNKNOWN and the splice below reads both; the `unsigned long`
   operands go through the elimination chain above for the same reason, which is the half this comment used
   to deny. It said, in full: "Every argument arrives CONVERTED — `unsigned long` and `DOMString` are the
   declaration's work — so nothing here runs the page's code and the body is ordinary C." Every clause of that
   is true of the conversion the DECLARATION performs, and the sentence was still read as a licence for a raw
   JS_ToUint32 — on the very type it names first, and the one unknown external input crosses as itself.
   STEPS 2 AND 3 ARE STATED HERE, ONCE, FOR BOTH KINDS OF OPERAND. `node_cd_replace_data` states them too and
   that is not a second copy of this member's: it is the shared §4.10 replace-data algorithm's own, reached by
   `js_cd_set_data` and by `node_split_text`, which have no argument to fork. Read from here they are already
   satisfied — the chain's exhaustion IS step 2's condition over an unknown and `offset > length` is it over a
   converted one — so the two agree by construction rather than by being kept in step. */
static int js_cd_op(JSContext *ctx, JSStepHdr *hdr, void *state, int argc, JSValueConst *argv,
                    JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    CdIndexState *st = state;
    const int magic = idl_step_magic(hdr);
    const char *algorithm = magic == 0 ? CD_SUBSTRING_ALGORITHM : magic == 1 ? CD_APPEND_ALGORITHM
                          : magic == 2 ? CD_INSERT_ALGORITHM    : magic == 3 ? CD_DELETE_ALGORITHM
                                                                : CD_REPLACE_ALGORITHM;
    lxb_dom_node_t *n = node_of(hdr->this_val);
    lxb_dom_character_data_t *cd;
    uint32_t length, offset = 0, count = 0;
    const char *data = NULL;
    size_t data_len = 0;
    bool past_end = false;
    int rc;
    JSValue r;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);   /* this machine makes no request that delivers a value */
    *presult = JS_UNDEFINED;
    DCHECK(hdr->stage == CD_OP_RUN,
           "a §4.10 CharacterData method resumed into a stage its algorithm does not have — every one of them "
           "declares exactly one, and the chain of questions a call may ask is a cursor on this machine's own "
           "state rather than a stage apiece");
    DCHECK(magic >= 0 && magic <= 4, "a CharacterData method was declared with a magic §4.10 does not name");
    if (!n || !node_is_chardata(n)) {
        JS_ThrowTypeError(ctx, "a CharacterData method ran on a node that holds no character data");
        return JS_STEP_ABRUPT;
    }
    cd = lxb_dom_interface_character_data(n);
    /* EVERY ARGUMENT §4.10 DECLARES IS REQUIRED — not one of the five members writes `optional` — so a short
       call is the DECLARATION's to refuse and a body reading past `argc` would be reading a slot the boundary
       never filled. Asserted the way the other indexed members of this engine assert it, and named per member
       because "2" is three different sentences here. */
    DCHECK(argc >= (magic == 1 ? 1 : magic == 4 ? 3 : 2),
           "a §4.10 CharacterData method reached its body with fewer arguments than its IDL declares, and "
           "every one of them is required — the declaration's own argument-count check is what should have "
           "refused the call");
    length = cd_units(cd->data.data, cd->data.length);                  /* STEP 1 */
    /* THE `offset` OPERAND. appendData declares none — §4.10 states it as "replace data with node, node's
       length, 0, and data" — so its offset is a fact about the node and never a fork. */
    if (magic == 1) {
        offset = length;
    } else if (concolic_is(argv[0])) {
        /* `length + 1` POSITIONS, because step 2 is `>` and not `>=`: a position at the very end is legal and
           is the one an insertData or a replaceData appends at. Exhausting them IS step 2's condition. */
        rc = idl_index_chain_run(ctx, hdr, &st->off, argv[0], length + 1, algorithm, &offset, &past_end);
        if (rc)
            return rc;   /* parked at the fork */
    } else {
        offset = idl_index_arg_known(ctx, argv[0], algorithm);
        past_end = offset > length;
    }
    if (past_end) {                                                     /* STEP 2 */
        JS_ThrowDOMException(ctx, "IndexSizeError", "the offset is past the end of the character data");
        return JS_STEP_ABRUPT;
    }
    /* THE `count` OPERAND — the three members that declare one. appendData and insertData delete nothing, so
       §4.10 states their count as the literal 0 and there is nothing here to ask. */
    if (magic == 0 || magic == 3 || magic == 4) {
        past_end = false;
        if (concolic_is(argv[1])) {
            /* `length − offset + 1` POSITIONS: step 3's condition is `offset + count > length`, so every count
               above `length − offset` reaches ONE answer and the remainder is a single world. `offset` is
               already this world's own number — step 2 above answered it — so the bound is a real one. */
            rc = idl_index_chain_run(ctx, hdr, &st->cnt, argv[1], length - offset + 1, algorithm,
                                     &count, &past_end);
            if (rc)
                return rc;   /* parked at the fork */
        } else {
            count = idl_index_arg_known(ctx, argv[1], algorithm);
            past_end = (uint64_t)offset + count > length;
        }
        if (past_end)                                                   /* STEP 3 */
            count = length - offset;
    }
    if (magic == 0) {                                                   /* substringData steps 3-4 */
        size_t a = cd_byte_of(cd->data.data, cd->data.length, offset);
        size_t b = cd_byte_of(cd->data.data, cd->data.length, offset + count);

        *presult = JS_NewStringLen(ctx, (const char *)cd->data.data + a, b - a);
        return JS_IsException(*presult) ? JS_STEP_ABRUPT : JS_STEP_DONE;
    }
    if (magic != 3) {                                               /* the three that take a DOMString */
        JSValueConst v = (magic == 1) ? argv[0] : (magic == 2 ? argv[1] : argv[2]);
        DCHECK(JS_IsString(v) || concolic_is(v),
               "a CharacterData splice reached the body with an unconverted operand — the IDL declaration is "
               "what converts it, and running the page's toString from here is the drive-to-completion the "
               "flow machinery exists to avoid");
        /* Unknown external input has no bytes: its SHAPE is what the node carries, the same answer `data`
           gives, so a source spliced into the tree still displays as the source it came from. */
        data = concolic_is(v) ? concolic_shape_c(v) : JS_ToCStringLen(ctx, &data_len, v);
        if (!data) return JS_STEP_ABRUPT;
        if (concolic_is(v)) data_len = strlen(data);
    } else {
        data = "";
        data_len = 0;
    }
    r = node_cd_replace_data(ctx, n, offset, count, data, data_len);
    if (magic != 3 && !concolic_is((magic == 1) ? argv[0] : (magic == 2 ? argv[1] : argv[2])))
        JS_FreeCString(ctx, data);
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, r);
        return JS_STEP_ABRUPT;
    }
    *presult = r;
    return JS_STEP_DONE;
}

static const IdlStepDecl CD_SUBSTRING_STEP = {
    js_cd_op, sizeof(CdIndexState), cd_index_visit, NULL, CD_SUBSTRING_ALGORITHM, CD_SUBSTRING_STEPS, 0, NULL
};
static const IdlStepDecl CD_APPEND_STEP = {
    js_cd_op, sizeof(CdIndexState), cd_index_visit, NULL, CD_APPEND_ALGORITHM, CD_APPEND_STEPS, 0, NULL
};
static const IdlStepDecl CD_INSERT_STEP = {
    js_cd_op, sizeof(CdIndexState), cd_index_visit, NULL, CD_INSERT_ALGORITHM, CD_INSERT_STEPS, 0, NULL
};
static const IdlStepDecl CD_DELETE_STEP = {
    js_cd_op, sizeof(CdIndexState), cd_index_visit, NULL, CD_DELETE_ALGORITHM, CD_DELETE_STEPS, 0, NULL
};
static const IdlStepDecl CD_REPLACE_STEP = {
    js_cd_op, sizeof(CdIndexState), cd_index_visit, NULL, CD_REPLACE_ALGORITHM, CD_REPLACE_STEPS, 0, NULL
};

/* §4.11 "SPLIT A TEXT NODE" — the concept, exported because §5.5's `insertNode` is stated over it: a range
   whose start node is a Text node splits that node at the start offset and inserts before the second half.
   IT IS NOT `substringData` FOLLOWED BY `insertBefore`. Two things make it its own algorithm: the new node is
   created on `node`'s NODE DOCUMENT (not the running realm's), and steps 7.2-7.5 move the live ranges across
   the split — a boundary point past the split point belongs to the SECOND node afterwards, which no
   composition of the public members does. Returns the new node, or NULL having thrown. */
lxb_dom_node_t *node_split_text(JSContext *ctx, lxb_dom_node_t *node, uint32_t offset)
{
    lxb_dom_character_data_t *cd;
    lxb_dom_node_t *parent, *new_node;
    lxb_dom_text_t *t;
    uint32_t length, count;
    size_t a, b;

    DCHECK(node != NULL && node->type == LXB_DOM_NODE_TYPE_TEXT,
           "§4.11's split was asked for something that is not a Text node — every caller checks the type, so "
           "reaching here means one of them stopped");
    cd = lxb_dom_interface_character_data(node);
    length = cd_units(cd->data.data, cd->data.length);                  /* STEP 1 */
    if (offset > length) {                                              /* STEP 2 */
        JS_ThrowDOMException(ctx, "IndexSizeError", "the split offset is past the end of the Text node");
        return NULL;
    }
    count = length - offset;                                            /* STEP 3 */
    a = cd_byte_of(cd->data.data, cd->data.length, offset);             /* STEP 4 */
    b = cd_byte_of(cd->data.data, cd->data.length, length);
    /* STEP 5 — on `node`'s NODE DOCUMENT, which is what makes a split inside an adopted subtree keep that
       subtree's document rather than acquiring the running realm's. */
    t = lxb_dom_document_create_text_node(node->owner_document, cd->data.data + a, b - a);
    CHECK(t != NULL, "split a Text node: the Lexbor text allocation failed — half a split is a document that "
                     "disagrees with the program that wrote it");
    new_node = lxb_dom_interface_node(t);
    dom_cow_note_created(new_node);   /* this flow made it */
    parent = node->parent;                                              /* STEP 6 */
    if (parent) {                                                       /* STEP 7 */
        node_insert_at(parent, new_node, node->next);                   /* STEP 7.1 */
        range_split_text_steps(ctx, node, new_node, offset);            /* STEPS 7.2-7.5 */
    }
    /* STEP 8. Its own steps 8-11 have nothing left to move: 7.2-7.3 took every boundary point past the split
       point off this node already, which is the whole reason the standard runs them first. */
    {
        JSValue r = node_cd_replace_data(ctx, node, offset, count, "", 0);
        if (JS_IsException(r)) return NULL;
        JS_FreeValue(ctx, r);
    }
    return new_node;                                                    /* STEP 9 */
}

/* §4.11 `[NewObject] Text splitText(unsigned long offset)`.
   IT IS A STEP MACHINE FOR THE ONE REASON EVERY MEMBER OF THIS FAMILY IS: its offset can be unknown external
   input, and split a Text node step 2 ("If offset is greater than length, then throw an "IndexSizeError"
   DOMException") is a comparison over it whose two completions are both feasible. `node_split_text` above is
   the CONCEPT and takes a number, because its other caller is DOM §5.5's `insertNode` over a range offset the
   engine itself computed — a value that was never unknown and has no fork to ask. */
static int js_text_split(JSContext *ctx, JSStepHdr *hdr, void *state, int argc, JSValueConst *argv,
                         JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    CdIndexState *st = state;
    lxb_dom_node_t *n = node_of(hdr->this_val), *split;
    uint32_t offset = 0, length;
    bool past_end = false;
    int rc;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);   /* this machine makes no request that delivers a value */
    *presult = JS_UNDEFINED;
    DCHECK(hdr->stage == CD_OP_RUN,
           "§4.11's `splitText` resumed into a stage the algorithm does not have — it declares exactly one, "
           "and the chain of questions its offset may ask is a cursor on this machine's own state");
    if (!n || n->type != LXB_DOM_NODE_TYPE_TEXT) {
        JS_ThrowTypeError(ctx, "splitText ran on a node that is not a Text node");
        return JS_STEP_ABRUPT;
    }
    /* THE ARGUMENT IS REQUIRED, so its absence is the DECLARATION's to refuse: §4.11 writes
       `[NewObject] Text splitText(unsigned long offset)` with no `optional`, and this member is declared with
       nargs 1, whose argument-count check runs before the body. `argc > 0 &&` stood here instead — a
       consumer-side default over a producer that cannot omit the value, which would have read offset 0 for a
       call the boundary never lets through. */
    DCHECK(argc >= 1, "§4.11's `splitText` reached its body with no argument — its IDL argument is required, "
                      "so the declaration's own argument-count check is what should have refused the call");
    length = cd_units(lxb_dom_interface_character_data(n)->data.data,                     /* STEP 1 */
                      lxb_dom_interface_character_data(n)->data.length);
    if (concolic_is(argv[0])) {
        /* `length + 1` POSITIONS — step 2 is `>` and not `>=`, so splitting at the very end is legal and
           yields an empty second node. `node_split_text` states step 2 again for its own other caller; from
           here it is already satisfied, which is why exhausting the chain is what throws. */
        rc = idl_index_chain_run(ctx, hdr, &st->off, argv[0], length + 1, CD_SPLIT_ALGORITHM,
                                 &offset, &past_end);
        if (rc)
            return rc;   /* parked at the fork */
        if (past_end) {                                                                  /* STEP 2 */
            JS_ThrowDOMException(ctx, "IndexSizeError", "the split offset is past the end of the Text node");
            return JS_STEP_ABRUPT;
        }
    } else {
        offset = idl_index_arg_known(ctx, argv[0], CD_SPLIT_ALGORITHM);
    }
    split = node_split_text(ctx, n, offset);
    if (!split)
        return JS_STEP_ABRUPT;
    *presult = node_wrap(ctx, split);
    return JS_IsException(*presult) ? JS_STEP_ABRUPT : JS_STEP_DONE;
}

static const IdlStepDecl CD_SPLIT_STEP = {
    js_text_split, sizeof(CdIndexState), cd_index_visit, NULL, CD_SPLIT_ALGORITHM, CD_SPLIT_STEPS, 0, NULL
};

/* §4.11 `readonly attribute DOMString wholeText` — the concatenation of the data of THIS node's contiguous
   Text nodes in tree order, which is the run of Text siblings this node is inside. */
static JSValue js_text_whole(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *n = node_of(this_val), *first, *p;
    char *buf = NULL;
    size_t len = 0, cap = 0;
    JSValue r;

    (void)magic;
    if (!n || n->type != LXB_DOM_NODE_TYPE_TEXT)
        return JS_ThrowTypeError(ctx, "wholeText read on a node that is not a Text node");
    for (first = n; first->prev && first->prev->type == LXB_DOM_NODE_TYPE_TEXT; first = first->prev)
        ;
    for (p = first; p && p->type == LXB_DOM_NODE_TYPE_TEXT; p = p->next) {
        lxb_dom_character_data_t *cd = lxb_dom_interface_character_data(p);
        if (len + cd->data.length > cap) {
            size_t want = cap ? cap * 2 : 64;
            char *nb;
            while (want < len + cd->data.length) want *= 2;
            nb = realloc(buf, want);
            CHECK(nb != NULL, "wholeText's buffer could not grow");
            buf = nb;
            cap = want;
        }
        memcpy(buf + len, cd->data.data, cd->data.length);
        len += cd->data.length;
    }
    r = JS_NewStringLen(ctx, buf ? buf : "", len);
    free(buf);
    return r;
}

/* §4.13 `ProcessingInstruction.target` — the only member the interface adds to CharacterData. */
static JSValue js_pi_target(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    lxb_dom_processing_instruction_t *pi;

    (void)magic;
    if (!n || n->type != LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION)
        return JS_ThrowTypeError(ctx, "target read on a node that is not a ProcessingInstruction");
    pi = lxb_dom_interface_processing_instruction(n);
    return JS_NewStringLen(ctx, (const char *)pi->target.data, pi->target.length);
}

/* ---- §4.4 THE NODE ALGORITHMS ---------------------------------------------------------------------------
 *
 * Every one of these is a pure walk over the flow's own tree, so they are ordinary C: no page code is reachable
 * from any of them, and the DOM-mutating pair goes through the per-flow chokepoints like every other write.
 *
 * NONE OF THEM RECURSES. isEqualNode, normalize and cloneNode's subtree are naturally written as tree recursion
 * and every one of them is a page-controlled depth — a document nests as deeply as its author nested it — so a
 * recursive C walk is an unbounded C stack in a engine whose whole point is that the C stack is flat. They are
 * explicit cursor walks with an explicit paired stack. */

/* §4.4 "root": the topmost inclusive ancestor. A node inside a shadow tree roots at the SHADOW ROOT, whose own
   parent is null — climbing out of it is §4.2's shadow-INCLUDING root, which is a different question and lives
   in shadow_root.c beside the host it has to read. */
lxb_dom_node_t *node_root(lxb_dom_node_t *n)
{
    while (n->parent) n = n->parent;
    return n;
}

/* §4.4: "connected: a node is connected if its SHADOW-INCLUDING root is a document". The distinction is the
   whole of what makes a custom element inside a shadow tree receive `connectedCallback`: its own root is the
   shadow root, and only climbing to the host's root reaches the document. */
bool node_is_connected(const lxb_dom_node_t *n)
{
    return n && shadow_root_shadow_including_root((lxb_dom_node_t *)n)->type == LXB_DOM_NODE_TYPE_DOCUMENT;
}

/* §4.2 "inclusive ancestor": walk UP from the descendant, which is O(depth) with no allocation, rather than
   down from the ancestor, which is O(subtree). */
bool node_is_inclusive_ancestor(const lxb_dom_node_t *a, const lxb_dom_node_t *b)
{
    for (; b; b = b->parent)
        if (b == a) return true;
    return false;
}

/* The PRE-ORDER successor within `root`'s subtree, or NULL at the end. This is the one traversal primitive the
   spec's tree-order algorithms need, and having it once is what keeps them from each growing a walker.
   THE ROOT TEST COMES BEFORE THE SIBLING TEST, AND THAT ORDER IS THE WHOLE OF THE BOUND. A hand-rolled advance
   is naturally written the other way round — climb while there is no next sibling, and turn the root into NULL
   ON THE WAY UP — which reads as bounded and is not: a root that HAS a next sibling never enters the climb at
   all, so the walk steps straight out of the subtree and runs to the end of the DOCUMENT in tree order. The
   escape is invisible wherever the root is a document or a detached tree's root, because such a node has no
   next sibling to escape through; it appears the day a caller walks from a node the page just put in the
   middle of a parent's child list. Measured: §4.2.3's insert walk written that way ran insert step 12's
   post-connection steps over every element from the insertion point to the end of the document, so an
   `insertBefore` with a non-null reference child re-prepared and re-executed every following `<script>` and
   created every following `<iframe>`'s child navigable a second time — while the same walk after an
   `appendChild` was correct, because an appended node is a last child. */
lxb_dom_node_t *node_next_in(lxb_dom_node_t *n, lxb_dom_node_t *root)
{
    if (n->first_child) return n->first_child;
    while (n != root) {
        if (n->next) return n->next;
        n = n->parent;
        if (!n) return NULL;
    }
    return NULL;
}

/* AND ITS INVERSE — the pre-order PREDECESSOR within `root`'s subtree, or NULL at the beginning. §6.1's
   `previousNode` walks a node collection backwards and there is no other way to say that: the node before `n`
   in tree order is the LAST DESCENDANT of its previous sibling, or, with no previous sibling, its parent.
   It lives beside its twin for the reason the twin exists — two walkers that disagree at the edges is exactly
   the bug a shared primitive prevents, and a backwards walk written inline gets the last-descendant descent
   wrong the first time. */
lxb_dom_node_t *node_prev_in(lxb_dom_node_t *n, lxb_dom_node_t *root)
{
    if (n == root) return NULL;
    if (n->prev) {
        n = n->prev;
        while (n->last_child) n = n->last_child;
        return n;
    }
    return n->parent;   /* NULL for a node with no parent, which is the start of any collection */
}

/* §4.4's "length": zero for a DocumentType, the data length of a CharacterData node, and the number of children
   for everything else. Every boundary-point check in §5 and every offset §6 hands out is stated against it.
   IT IS IN CODE UNITS, LIKE `CharacterData.length`, AND IT WAS IN BYTES — the same defect §4.4's own length
   getter already carries a paragraph about, one layer down and unfixed. The two answers disagreed for every
   non-ASCII text node: `t.length` said 1 for "é" and this said 2, so `range.setStart(t, 2)` was rejected by
   one rule and accepted by the other, and a Range's offsets were byte offsets into a string the page indexes
   in code units. §5 is stated entirely over this function, so every boundary point in the engine inherited it.
   ONE conversion, shared with §4.10's splices, because a second UTF-8 walk is a second place to disagree about
   where a code point ends. */
uint32_t node_length(const lxb_dom_node_t *n)
{
    const lxb_dom_node_t *c;
    uint32_t k = 0;

    DCHECK(n != NULL, "§4.4's length was asked of no node");
    if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE) return 0;
    if (n->type == LXB_DOM_NODE_TYPE_TEXT || n->type == LXB_DOM_NODE_TYPE_COMMENT ||
        n->type == LXB_DOM_NODE_TYPE_CDATA_SECTION ||
        n->type == LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION) {
        const lxb_dom_character_data_t *cd = (const lxb_dom_character_data_t *)n;
        return cd_units(cd->data.data, cd->data.length);
    }
    for (c = n->first_child; c; c = c->next) k++;
    return k;
}

/* THE BYTE OFFSET AT WHICH CODE-UNIT OFFSET `units` BEGINS in a CharacterData node's stored UTF-8 — the
   translation every §5 algorithm needs to touch the bytes behind an offset node_length handed out. Exported
   rather than re-walked in range.c for the reason above: two walks is two answers. */
size_t node_cd_byte_of(const lxb_dom_node_t *n, uint32_t units)
{
    const lxb_dom_character_data_t *cd;

    DCHECK(n != NULL && (n->type == LXB_DOM_NODE_TYPE_TEXT || n->type == LXB_DOM_NODE_TYPE_COMMENT ||
                         n->type == LXB_DOM_NODE_TYPE_CDATA_SECTION ||
                         n->type == LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION),
           "§4.10's byte offset was asked of a node that holds no character data");
    cd = (const lxb_dom_character_data_t *)n;
    return cd_byte_of(cd->data.data, cd->data.length, units);
}

/* §4.2's "index": how many siblings precede this node. */
uint32_t node_index(const lxb_dom_node_t *n)
{
    const lxb_dom_node_t *c;
    uint32_t k = 0;

    DCHECK(n != NULL, "§4.2's index was asked of no node");
    for (c = n->prev; c; c = c->prev) k++;
    return k;
}

/* magic 0 = isConnected, 1 = ownerDocument, 2 = parentElement, 3 = baseURI */
static JSValue js_node_facts(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);

    if (!n) return JS_UNDEFINED;
    switch (magic) {
    case 0:
        /* §4.4: connected iff the shadow-including root is a DOCUMENT. A node a page has created but not yet
           inserted is NOT connected, which is the difference a component-mount check is asking about. The
           comment said "shadow-including" while the code read the plain root, which for a node inside a shadow
           tree is the shadow root and never a document — one implementation, so the two cannot disagree. */
        return JS_NewBool(ctx, node_is_connected(n));
    case 1:
        /* §4.4: a Document's ownerDocument is null; every other node's is its node document. */
        if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT) return JS_NULL;
        return node_wrap(ctx, lxb_dom_interface_node(n->owner_document));
    case 2:
        /* §4.4: the parent, but only when it is an ELEMENT — the difference from parentNode is exactly the
           document and the fragment, which is why a walk-up loop uses this one and stops on its own. */
        return (n->parent && n->parent->type == LXB_DOM_NODE_TYPE_ELEMENT) ? node_wrap(ctx, n->parent) : JS_NULL;
    default:
        /* §4.4: THE NODE DOCUMENT's DOCUMENT BASE URL, serialized — the node's own document and not the realm's
           active one, which stopped being the same question the moment §4.5.1's factories could build a second
           Document. `foreignDoc.createElement("a").baseURI` is `about:blank`, not the page's address. It is
           HTML §2.4.3's answer and NOT the address: this line read the ADDRESS, so `baseURI` in a page that
           ships `<base href>` reported a URL the page's own markup had replaced. Asked of the component that
           owns it rather than re-derived here, because two answers to "what is this document's base URL" is
           how they drift apart. */
        DCHECK(magic == 3, "a Node fact was declared with a magic this table does not name");
        return JS_NewString(ctx, document_base_url_of(n->owner_document));
    }
}

/* §4.4 hasChildNodes / isSameNode / contains — three one-line predicates that a page uses constantly and that
   each had to be re-implemented by the page when they were absent.
   THE TWO THAT TAKE AN ARGUMENT ARE DECLARED MEMBERS and hasChildNodes is not, which is the split this body
   reads as a magic: `Node? otherNode` and `Node? other` are IDL_INTERFACE_NULLABLE positions, so the IDL null
   and a branded node wrapper are the only two things that can arrive, and `undefined boolean hasChildNodes()`
   declares nothing to convert. */
static JSValue js_node_predicates(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    lxb_dom_node_t *other;

    if (!n) return JS_FALSE;
    if (magic == 0) return JS_NewBool(ctx, n->first_child != NULL);
    DCHECK(argc == 1, "a §4.4 `Node?` predicate reached its body without its declared argument — the position "
                      "is not optional, so §3.6 Overload resolution algorithm's step 5 arity TypeError stands "
                      "in front of this body");
    /* A THIRD VALUE IS WHAT THIS BODY USED TO ANSWER `false` FOR, and false is not one of §3.2.15's two
       outcomes: a body reaching for the argument's node and finding none cannot tell "not this node" from "not
       a Node", so `n.contains(5)` and `n.isSameNode(5)` were false where the standard throws. */
    DCHECK(JS_IsNull(argv[0]) || node_of(argv[0]) != NULL,
           "a `Node?` predicate's argument reached the body as neither the IDL null nor a node — Web IDL "
           "§3.2.20 Nullable types is what makes null and undefined the IDL null and §3.2.15 Interface types "
           "step 2 is what refuses everything else, both at the declaration");
    other = node_of(argv[0]);
    if (magic == 1) return JS_NewBool(ctx, other == n);              /* isSameNode: `Node?`, so null is false */
    DCHECK(magic == 2, "a Node predicate was declared with a magic this table does not name");
    return JS_NewBool(ctx, other && node_is_inclusive_ancestor(n, other));       /* contains: INCLUSIVE */
}

/* §4.4 isEqualNode — STRUCTURAL equality, which is what a page comparing two rendered subtrees means and what
   `===` cannot answer. Written as a paired cursor walk: the two trees advance in lockstep, so a mismatch at any
   depth stops immediately and the C stack never grows with the document's nesting. */
static bool node_shallow_equal(lxb_dom_node_t *a, lxb_dom_node_t *b)
{
    size_t la = 0, lb = 0;
    const lxb_char_t *na, *nb;

    if (a->type != b->type) return false;
    if (a->ns != b->ns) return false;
    if (node_is_chardata(a)) {
        lxb_dom_character_data_t *ca = lxb_dom_interface_character_data(a);
        lxb_dom_character_data_t *cb = lxb_dom_interface_character_data(b);
        return ca->data.length == cb->data.length &&
               memcmp(ca->data.data, cb->data.data, ca->data.length) == 0;
    }
    if (a->type != LXB_DOM_NODE_TYPE_ELEMENT)
        return true;   /* a document or a fragment: the children below carry all of the identity */

    na = lxb_dom_element_qualified_name(lxb_dom_interface_element(a), &la);
    nb = lxb_dom_element_qualified_name(lxb_dom_interface_element(b), &lb);
    if (la != lb || (la && memcmp(na, nb, la) != 0)) return false;

    /* §4.4: the attribute LISTS must match as sets — same count, and each of A's present on B with the same
       value. Order is not part of the comparison, which is why this is two passes and not a zip. */
    {
        lxb_dom_attr_t *at;
        size_t ca = 0, cb = 0;
        for (at = lxb_dom_interface_element(a)->first_attr; at; at = at->next) ca++;
        for (at = lxb_dom_interface_element(b)->first_attr; at; at = at->next) cb++;
        if (ca != cb) return false;
        for (at = lxb_dom_interface_element(a)->first_attr; at; at = at->next) {
            size_t kn = 0, vn = 0, ovn = 0;
            const lxb_char_t *k = lxb_dom_attr_qualified_name(at, &kn);
            const lxb_char_t *v = lxb_dom_attr_value(at, &vn);
            const lxb_char_t *ov = lxb_dom_element_get_attribute(lxb_dom_interface_element(b), k, kn, &ovn);
            if (!ov) return false;
            if (vn != ovn || (vn && memcmp(v, ov, vn) != 0)) return false;
        }
    }
    return true;
}

/* §4.4 isEqualNode — A MACHINE, and the first walk converted to one, because the size of the work is the
   PAGE'S: two whole subtrees, node by node. Running no user code is not what made the old loop acceptable and
   nothing else did either — it held the scheduler for as long as the page's tree was deep, inside one opcode,
   with every other flow waiting. Running no user code only means it needs no REQUEST; it still needs to yield.
   The cursors are the state, so a resume continues at the pair it stopped on and re-walks nothing. */
/* WHERE THIS MACHINE RESTS, AS §4.4 NUMBERS IT. isEqualNode is one sentence — "return true if otherNode is
   non-null and this equals otherNode" — over the `equals` concept, whose five conditions are what the paired
   walk checks: conditions 1-4 at the pair it is standing on, condition 5 by advancing both cursors to the
   child at the identical index. No page code can run anywhere inside it, so the pair is one stage and the
   label says which conditions it covers. */
#define NODE_EQUAL_STAGES(X) \
    X(NODE_EQ_NONNULL, "DOM §4.4 isEqualNode (otherNode is non-null)") \
    X(NODE_EQ_PAIR,    "DOM §4.4 equals conditions 1-4 at one node pair, then condition 5's pair at the " \
                       "identical index")
enum { IDL_STEP_STAGE_BASE(NODE_EQUAL_STAGES) NODE_EQUAL_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const NODE_EQUAL_STEPS[] = { NODE_EQUAL_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    lxb_dom_node_t *a, *b, *ra, *rb;   /* the two cursors and the two roots they are bounded by */
    bool result;
} NodeEqualState;

static void node_equal_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    /* NOTHING OWNED. The cursors are Lexbor nodes, which belong to the document and outlive any flow; a fork
       mid-walk gives both arms the same two positions in the same tree, which is what they should have. */
    (void)ctx; (void)st; (void)v;
}

static int js_node_is_equal(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    NodeEqualState *s = st;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    if (hdr->stage == NODE_EQ_NONNULL) {
        hdr->stage = NODE_EQ_PAIR;
        s->a = node_of(hdr->this_val);
        DCHECK(argc == 1, "isEqualNode reached its body without its declared argument — `Node? otherNode` is "
                          "not optional, so §3.6 Overload resolution algorithm's step 5 arity TypeError is "
                          "what a zero-argument call gets");
        DCHECK(JS_IsNull(argv[0]) || node_of(argv[0]) != NULL,
               "isEqualNode's `Node? otherNode` reached the body as neither the IDL null nor a node — this "
               "body cannot tell that apart from `not equal`, which is why the refusal is the declaration's: "
               "Web IDL §3.2.15 Interface types step 2 under §3.2.20 Nullable types");
        s->b = node_of(argv[0]);
        s->ra = s->a; s->rb = s->b;
        /* `Node? otherNode`: the IDL null is never equal. A value that is neither is a TypeError the
           declaration already threw — it used to be this same `false`, which reported "these two subtrees
           differ" for a call that never named a second subtree. */
        if (!s->a || !s->b) { *presult = JS_FALSE; return JS_STEP_DONE; }
    }
    DCHECK(hdr->stage == NODE_EQ_PAIR, "isEqualNode resumed into a stage §4.4 does not have");
    if (!node_shallow_equal(s->a, s->b)) { *presult = JS_FALSE; return JS_STEP_DONE; }
    /* the CHILD COUNTS must match, and comparing them here rather than at the end is what lets the two
       cursors advance in lockstep below without one running off the end of the other. */
    {
        lxb_dom_node_t *ca = s->a->first_child, *cb = s->b->first_child;
        while (ca && cb) { ca = ca->next; cb = cb->next; }
        if (ca || cb) { *presult = JS_FALSE; return JS_STEP_DONE; }
    }
    s->a = node_next_in(s->a, s->ra);
    s->b = node_next_in(s->b, s->rb);
    if (!s->a || !s->b) {
        *presult = JS_NewBool(ctx, s->a == NULL && s->b == NULL);
        return JS_STEP_DONE;
    }
    /* ONE PAIR PER STEP, and the yield is asked at every one — when no flow is waiting it is a predicted call
       and the walk continues, and when one is it parks here with both cursors intact. */
    return JS_STEP_YIELD;
}

static const IdlStepDecl NODE_EQUAL_STEP = {
    js_node_is_equal, sizeof(NodeEqualState), node_equal_visit, NULL,
    "DOM §4.4 Node.isEqualNode (over the `equals` concept)", NODE_EQUAL_STEPS
};

/* §4.4 compareDocumentPosition — the bitmask a page uses to sort nodes into document order. */
enum {
    NODE_POS_DISCONNECTED = 0x01, NODE_POS_PRECEDING = 0x02, NODE_POS_FOLLOWING = 0x04,
    NODE_POS_CONTAINS = 0x08, NODE_POS_CONTAINED_BY = 0x10, NODE_POS_IMPLEMENTATION_SPECIFIC = 0x20,
};

/* A MACHINE, and it has THREE walks in it rather than one, which is why the stages are numbered rather than
   folded together. Finding a node's root is O(depth); each "is one an inclusive ancestor of the other" is
   O(depth) again; and if neither contains the other the answer comes from a pre-order walk of the WHOLE shared
   tree looking for whichever comes first. Only the last is obviously of the page's size, and that is exactly
   why the other two are converted too — "a document is never that deep" is a bound, and a bound nobody wrote
   down is the kind that is wrong on the one page that matters.
   ONE CURSOR serves all of them: each stage sets it and the next stage consumes it, so a resume comes back to
   the position the walk was at and never to the start of a stage it already finished. */
typedef struct {
    lxb_dom_node_t *a, *b;      /* node2 and node1: `this` and `other`, in §4.4's own names */
    lxb_dom_node_t *ra, *rb;    /* their roots, once each walk has found them */
    lxb_dom_node_t *p;          /* the cursor the running stage is advancing */
} NodePosState;

static void node_pos_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    /* NOTHING OWNED — every field is a Lexbor node, which belongs to the document and outlives any flow. A fork
       mid-walk gives both arms the same position in the same tree, which is what they should have. */
    (void)ctx; (void)st; (void)v;
}

/* WHERE THIS MACHINE RESTS, AS §4.4 NUMBERS IT — and the numbering caught a real disagreement with the spec:
   the two containment tests ran in the order "is node1 a DESCENDANT of node2" then "is node1 an ANCESTOR of
   node2", which is step 8 before step 7. They are mutually exclusive for the nodes this engine builds, so no
   answer changed; the ORDER of a spec's steps is the spec, and a machine whose stages run them backwards is a
   machine whose stage numbers cannot name the step they are at. They now run 7 then 8.
   §4.4 numbers `this` node2 and `other` node1, which is the opposite of how they read, so the state's two
   pointers carry that mapping in their comment rather than in each reader's head. */
#define NODE_POS_STAGES(X) \
    X(NODEPOS_SAME,       "DOM §4.4 steps 1-3 (this is other → zero; node1 is other and node2 is this)") \
    X(NODEPOS_ROOT2,      "DOM §4.4 step 6 (node2's root: one ancestor per step)") \
    X(NODEPOS_ROOT1,      "DOM §4.4 step 6 (node1's root, then the DISCONNECTED answer when the two roots " \
                          "differ)") \
    X(NODEPOS_ANCESTOR,   "DOM §4.4 step 7 (node1 is an ancestor of node2: one ancestor per step)") \
    X(NODEPOS_DESCENDANT, "DOM §4.4 step 8 (node1 is a descendant of node2: one ancestor per step)") \
    X(NODEPOS_ORDER,      "DOM §4.4 steps 9-10 (which of the two the shared root's tree order reaches first)")
enum { IDL_STEP_STAGE_BASE(NODE_POS_STAGES) NODE_POS_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const NODE_POS_STEPS[] = { NODE_POS_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_node_compare_position(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                    JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    NodePosState *s = st;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);

    switch (hdr->stage) {
    case NODEPOS_SAME:
        s->a = node_of(hdr->this_val);
        /* `unsigned short compareDocumentPosition(Node other)` — NON-nullable, so §3.2.15 Interface types
           refuses null, undefined and every non-node at the declaration. What is left here is the RECEIVER,
           which §3.7.7 Operations brands before any argument is converted and which this engine still asks
           for at the body: the two used to share one condition and one message, so a page that called the
           member on a plain object and a page that passed one got the same sentence. */
        DCHECK(argc == 1, "compareDocumentPosition reached its body without its declared argument — `Node "
                          "other` is not optional, so §3.6 Overload resolution algorithm's step 5 arity "
                          "TypeError is what a zero-argument call gets");
        s->b = node_of(argv[0]);
        DCHECK(s->b != NULL, "compareDocumentPosition's `Node other` reached the body as something that is not "
                             "a node — the declaration's IDL_INTERFACE position and its "
                             "idl_iface_brand(node_class_id()) are what make that a TypeError before step 1");
        if (!s->a) {
            JS_ThrowTypeError(ctx, "compareDocumentPosition called on something that is not a Node");
            return JS_STEP_ABRUPT;
        }
        if (s->a == s->b) { *presult = JS_NewInt32(ctx, 0); return JS_STEP_DONE; }   /* step 1 */
        s->p = s->a;                 /* step 2: node2 is `this` */
        hdr->stage = NODEPOS_ROOT2;
        return JS_STEP_YIELD;

    case NODEPOS_ROOT2:
        if (s->p->parent) { s->p = s->p->parent; return JS_STEP_YIELD; }
        s->ra = s->p;
        s->p = s->b;
        hdr->stage = NODEPOS_ROOT1;
        return JS_STEP_YIELD;

    case NODEPOS_ROOT1:
        if (s->p->parent) { s->p = s->p->parent; return JS_STEP_YIELD; }
        s->rb = s->p;
        if (s->ra != s->rb)
            /* §4.4 step 6: disconnected nodes get a consistent-but-arbitrary order, and it must be CONSISTENT —
               the same pair must answer the same way every time or a page's sort never terminates. Pointer
               order is stable for the lifetime of the two nodes, which is what the spec's note allows for. */
            return *presult = JS_NewInt32(ctx, NODE_POS_DISCONNECTED | NODE_POS_IMPLEMENTATION_SPECIFIC |
                                               (s->a < s->b ? NODE_POS_FOLLOWING : NODE_POS_PRECEDING)),
                   JS_STEP_DONE;
        s->p = s->a;                 /* step 7: walk UP from node2 looking for node1 — O(depth), not O(subtree) */
        hdr->stage = NODEPOS_ANCESTOR;
        return JS_STEP_YIELD;

    case NODEPOS_ANCESTOR:
        if (s->p == s->b)
            return *presult = JS_NewInt32(ctx, NODE_POS_CONTAINS | NODE_POS_PRECEDING), JS_STEP_DONE;
        if (s->p) { s->p = s->p->parent; return JS_STEP_YIELD; }
        s->p = s->b;                 /* step 8: walk UP from node1 looking for node2 */
        hdr->stage = NODEPOS_DESCENDANT;
        return JS_STEP_YIELD;

    case NODEPOS_DESCENDANT:
        if (s->p == s->a)
            return *presult = JS_NewInt32(ctx, NODE_POS_CONTAINED_BY | NODE_POS_FOLLOWING), JS_STEP_DONE;
        if (s->p) { s->p = s->p->parent; return JS_STEP_YIELD; }
        s->p = s->ra;                /* neither contains the other: tree order decides */
        hdr->stage = NODEPOS_ORDER;
        return JS_STEP_YIELD;

    case NODEPOS_ORDER:
        /* Steps 9-10: whichever the pre-order walk reaches first PRECEDES. */
        if (s->p == s->a) return *presult = JS_NewInt32(ctx, NODE_POS_FOLLOWING), JS_STEP_DONE;
        if (s->p == s->b) return *presult = JS_NewInt32(ctx, NODE_POS_PRECEDING), JS_STEP_DONE;
        s->p = node_next_in(s->p, NULL);
        DCHECK(s->p != NULL, "compareDocumentPosition walked a shared root without reaching either node — the "
                             "tree is not a tree");
        return JS_STEP_YIELD;
    }
    DFAIL("compareDocumentPosition resumed into a stage §4.4 does not have");
    return JS_STEP_ABRUPT;
}

static const IdlStepDecl NODE_POS_STEP = {
    js_node_compare_position, sizeof(NodePosState), node_pos_visit, NULL,
    "DOM §4.4 Node.compareDocumentPosition", NODE_POS_STEPS
};

/* §4.4 getRootNode(optional GetRootNodeOptions options = {}) — "return this's shadow-including root if
 * options["composed"] is true; otherwise this's root".
 *
 * Reading the option is the page's code — `getRootNode({ get composed(){ … } })` is a getter, and a Proxy makes
 * even a plain object one — so the read is a REQUEST, and the declaration is what performs it: by the time this
 * body runs the dictionary is a plain engine-built object and there is no user code left to reach. */
static JSValue js_node_root(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    (void)magic;
    if (!n) return JS_UNDEFINED;
    if (argc > 0 && idl_dict_bool(ctx, argv[0], "composed"))
        return node_wrap(ctx, shadow_root_shadow_including_root(n));
    return node_wrap(ctx, node_root(n));
}

/* §4.4 normalize — remove empty Text nodes and merge adjacent ones. A page calls it before comparing or
   serialising a tree it built piecemeal, and both halves go through the per-flow chokepoints so a forked arm
   normalising its own subtree does not normalise its sibling's.
   A MACHINE with TWO cursors and two stages, because there are two loops and both are of the page's size: the
   walk over the subtree, and the absorption of a run of adjacent Text siblings into the first of them. A page
   that built its text a chunk at a time has runs as long as the number of chunks, so folding the inner loop
   into one step would be a walk hiding inside a step of a walk.
   `next` is taken BEFORE the current node can be removed, and it lives in the state for the same reason the
   cursor does — the merge stage adjusts it when it absorbs the very node `next` was pointing at. */
/* WHERE THIS MACHINE RESTS, AS §4.4 NUMBERS IT. normalize()'s steps 1-7 are stated FOR EACH descendant
   exclusive Text node, so the iteration itself is a rest point (there is no step number for "the next node",
   and the label says so), steps 1-2 are the empty-node case at the node the walk is standing on, and steps 3-4
   with 7 are the absorption — one contiguous exclusive Text sibling per step, because a page that built its
   text a chunk at a time has as many of them as it has chunks. Nothing here runs the page's code. */
#define NODE_NORM_STAGES(X) \
    X(NODE_NORM_EACH,   "DOM §4.4 normalize() (the iteration: this's first descendant exclusive Text node)") \
    X(NODE_NORM_AT,     "DOM §4.4 normalize() steps 1-2 (length; remove the node when it is zero)") \
    X(NODE_NORM_ABSORB, "DOM §4.4 normalize() steps 3-4 and 7 (absorb one contiguous exclusive Text sibling " \
                        "and remove it)")
enum { IDL_STEP_STAGE_BASE(NODE_NORM_STAGES) NODE_NORM_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const NODE_NORM_STEPS[] = { NODE_NORM_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    lxb_dom_node_t *root, *n, *next;
} NodeNormState;

static void node_norm_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    /* NOTHING OWNED — Lexbor nodes, which belong to the document. The MUTATIONS this makes are captured by the
       chokepoints into the running flow's delta, which is what a forked arm's isolation is made of; the cursors
       themselves are just positions. */
    (void)ctx; (void)st; (void)v;
}

static int js_node_normalize(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                             JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    NodeNormState *s = st;
    lxb_dom_character_data_t *cd;

    (void)argc; (void)argv; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);

    if (hdr->stage == NODE_NORM_EACH) {
        s->root = node_of(hdr->this_val);
        if (!s->root) { *presult = JS_UNDEFINED; return JS_STEP_DONE; }
        s->n = node_next_in(s->root, s->root);
        hdr->stage = NODE_NORM_AT;
        return JS_STEP_YIELD;
    }

    if (hdr->stage == NODE_NORM_AT) {
        if (!s->n) { *presult = JS_UNDEFINED; return JS_STEP_DONE; }
        s->next = node_next_in(s->n, s->root);
        if (s->n->type != LXB_DOM_NODE_TYPE_TEXT) { s->n = s->next; return JS_STEP_YIELD; }
        cd = lxb_dom_interface_character_data(s->n);
        if (cd->data.length == 0) {
            dom_cow_remove_child(s->n);
            s->n = s->next;
            return JS_STEP_YIELD;
        }
        hdr->stage = NODE_NORM_ABSORB;
        return JS_STEP_YIELD;
    }

    DCHECK(hdr->stage == NODE_NORM_ABSORB, "normalize resumed into a stage §4.4 does not have");
    /* ONE SIBLING PER STEP. Absorb it into `n`, drop it, and come back here for the next one. */
    if (s->n->next && s->n->next->type == LXB_DOM_NODE_TYPE_TEXT) {
        lxb_dom_node_t *sib = s->n->next;
        lxb_dom_character_data_t *sd = lxb_dom_interface_character_data(sib);
        size_t len;
        char *buf;
        cd = lxb_dom_interface_character_data(s->n);
        len = cd->data.length + sd->data.length;
        buf = malloc(len ? len : 1);
        CHECK(buf != NULL, "normalize: OOM merging two Text nodes — dropping the merge would leave the page a "
                           "tree it did not build");
        memcpy(buf, cd->data.data, cd->data.length);
        memcpy(buf + cd->data.length, sd->data.data, sd->data.length);
        /* the successor was computed before this sibling was absorbed; if it WAS this sibling, move it on. */
        if (s->next == sib) s->next = node_next_in(sib, s->root);
        /* §4.4's own step 4 is `replace data of node with length, 0, data` — an APPEND, whose live-range steps
           8-11 can move nothing (no offset is greater than the length it is being appended to), which is why
           the raw write is the same algorithm here and the boundary points are §4.4's own steps 6.1-6.4. They
           run with the length node had BEFORE this sibling, and while the sibling is still in the tree. */
        {
            uint32_t before = node_length(s->n);
            dom_cow_set_text(s->n, buf, len);
            range_normalize_absorb_steps(ctx, s->n, sib, before);
            /* §4.10 replace data's last step, for the replace data §4.4 step 4 writes out here. The removal
               below fires it a second time through the chokepoint, which costs a recomputation and cannot
               change an answer — see node.h on why every registrant of this family is idempotent. */
            node_children_changed(ctx, s->n->parent);
        }
        dom_cow_remove_child(sib);
        free(buf);
        return JS_STEP_YIELD;
    }
    s->n = s->next;
    hdr->stage = NODE_NORM_AT;
    return JS_STEP_YIELD;
}

static const IdlStepDecl NODE_NORM_STEP = {
    js_node_normalize, sizeof(NodeNormState), node_norm_visit, NULL,
    "DOM §4.4 Node.normalize()", NODE_NORM_STEPS
};

/* §4.4 `clone a node` — A MACHINE, because a deep clone is a walk of the page's subtree and a COPY of it.
   Lexbor's `lxb_dom_node_clone(n, true)` runs that walk to completion inside one opcode; it is iterative rather
   than recursive, so it never blew the C stack, and that is exactly the kind of thing that hides how long it
   holds the scheduler. `document.body.cloneNode(true)` is one opcode for the whole document.
   §4.4's "clone a single node" IS A PER-INTERFACE SWITCH in the standard itself — an element, a document, a
   doctype, an attribute and a CharacterData each have their own step — and clone_a_single_node below is that
   switch. LEXBOR OWNS EVERY ARM BUT THE ELEMENT'S: `lxb_dom_document_import_node(doc, n, false)` is the
   document's own clone_interface, so a Text's data, a doctype's three names and every per-interface field are
   copied by the code the tree builder uses. What moves here is the WALK, node by node, exactly as §8.4's
   serialiser did.
   IT IS THE ALGORITHM AND NOT THE MEMBER: cloneNode is two steps over it and §5.5's extract and
   clone-the-contents are six more, so the state, the stage list and the body are all exported (node.h) and the
   member below is one caller of them. A second copier beside this one is a second answer to "what is a copy of
   this node", missing step 3 and step 6.
   THE COPY IS A PRIVATE TREE. It is built by inserting into itself and it is in no document until the page
   inserts it, so those inserts are declared private rather than captured — capturing them would put the whole
   copy in the running flow's delta, when the delta exists to hold shared state the flow touched. */
/* THE PHASES, FROM THE SAME X-LIST THE CALLERS' LABELS COME FROM. A caller's block holds the six in this order
   and the body is written against the OFFSET into it, so the body names no caller's constants and the order
   cannot drift from the labels — they are one list. */
enum { NODE_CLONE_ALGO_STAGES(JS_STEP_STAGE_ENUM, NODE_CLONE_PHASE, "") NODE_CLONE_PHASE_N };

void node_clone_visit_state(JSContext *ctx, NodeCloneState *s, JSStepVisit *v)
{
    /* The cursors own nothing — every one is a Lexbor node: the originals belong to the document, and the copy
       belongs to the document's memory pool from the moment clone_interface makes it. The template STACK is
       plain storage, and a forked arm must not share it: each unwinds its own levels. */
    v->buf(ctx, (void **)&s->stack, sizeof(NodeCloneFrame) * (size_t)s->scap);
}

static void node_clone_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    NodeCloneState *s = st;
    node_clone_visit_state(ctx, s, v);
}

/* Leave this level for one below it, remembering the stage this one resumes at.
   THE RUNTIME'S ALLOCATOR, BECAUSE THE DECLARATION'S IS: node_clone_visit_state declares this stack to the
   fork, which copies it with js_malloc and whose teardown discharges it with js_free, so a stack grown with
   the C library's realloc is one a forked arm hands to the wrong allocator. */
static void clone_push(JSContext *ctx, NodeCloneState *s, int resume_stage)
{
    if (s->sp == s->scap) {
        int want = s->scap ? s->scap * 2 : 4;
        NodeCloneFrame *n = js_realloc(ctx, s->stack, sizeof(NodeCloneFrame) * (size_t)want);
        CHECK(n != NULL, "cloneNode could not grow its level stack");
        s->stack = n;
        s->scap = want;
    }
    s->stack[s->sp].src = s->src;
    s->stack[s->sp].dst = s->dst;
    s->stack[s->sp].root = s->root;
    s->stack[s->sp].croot = s->croot;
    s->stack[s->sp].cnode = s->cnode;
    s->stack[s->sp].deep = s->deep;
    s->stack[s->sp].stage = resume_stage;
    s->sp++;
}

/* A `<template>`'s content fragment, or NULL for anything else. The children of a template are NOT under it —
   they hang off a separate fragment — so a walk that follows first_child copies the template and none of its
   markup, which is exactly what this did before the case was added: `<template><b>tc</b></template>` cloned to
   `<template></template>` and the page got an empty one with no error anywhere. §4.4 states the contents are
   cloned too. A shallow clone of a template already HAS its own empty fragment, because lexbor's
   clone_interface builds the template interface, so both sides have somewhere to go.
   EXPORTED, because a second tree walk needed the same question: core/html/tree_construction.c copies a
   partial parse and has to descend into the same second tree for the same reason. Two spellings of "where a
   template's markup actually is" is how one of them silently stops covering it. */
lxb_dom_node_t *node_template_content(const lxb_dom_node_t *n)
{
    lxb_html_template_element_t *t;
    if (n->type != LXB_DOM_NODE_TYPE_ELEMENT || !lxb_html_tree_node_is(n, LXB_TAG_TEMPLATE)) return NULL;
    t = lxb_html_interface_template((lxb_dom_node_t *)(uintptr_t)n);
    return t->content ? &t->content->node : NULL;
}

/* THE SAME QUESTION ASKED BACKWARDS — the `<template>` element whose template contents `n` IS, or NULL for
   every other node. It is the way OUT of the one tree a parse builds that no child link reaches: §4.12.3's
   contents are a DocumentFragment with no parent, and DOM §4.7 "Interface DocumentFragment" gives it "an
   associated host (null or an element in a DIFFERENT node tree)" — which is the only pointer back.
   IT IS THE ROUND TRIP THROUGH node_template_content AND NOT A TYPE TEST, because a shadow root is a
   DocumentFragment with a host too: `n` is this template's contents only if the template says so, so the two
   directions cannot answer differently about one node. Written here beside the forward question for the reason
   that one is exported — THREE callers had transcribed this round trip (solver/dom_cow.c's parse-root walk,
   core/html/tree_construction.c's partial-parse copy, and HTML §14.2's XML placement in core/xml/xml_tree.c),
   and three spellings of one rule is two of them one edit away from stopping covering it. */
lxb_dom_node_t *node_template_content_host(const lxb_dom_node_t *n)
{
    lxb_dom_element_t *host;

    if (n->type != LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT) return NULL;
    host = lxb_dom_interface_document_fragment((lxb_dom_node_t *)(uintptr_t)n)->host;
    if (host == NULL) return NULL;
    return node_template_content(lxb_dom_interface_node(host)) == n ? lxb_dom_interface_node(host) : NULL;
}

/* THE STAGES, and why the template case forces a LEVEL rather than a stage. A `<template>` can have BOTH: the
   parser puts markup in its content fragment, and `t.appendChild(x)` appends to the ELEMENT — §4.10 is explicit
   that only the parser and `t.content` reach the fragment. So a template's copy has two child lists to fill, and
   coming back from the content walk must resume AFTER the content check and BEFORE the ordinary-children one, or
   the template descends into its own content again and the walk never ends.
   The invariant every stage keeps: `src` is the original being handled, `cnode` is its copy, and `dst` is the
   copy of the node the NEXT child goes under. */
/* WHERE THIS MACHINE RESTS, AS THE STANDARDS NUMBER IT. `clone a node`'s own steps 2, 3, 5, 6 and 7 are exactly
   what the walk cycles through per node: clone a single node and append it (2 and 4), run the cloning steps
   other specifications define — which for a template element is HTML §4.12.3's, the reason the template LEVEL
   exists at all — recurse over the children (5), clone a clonable shadow root (6), and return (7). Each is its
   own stage because each is per node and the walk is the page's subtree.
   LEAVING A NODE IS ONE REST POINT, AND THAT IS WHAT STEP 7 IS. The cursor is flat over two trees, so the
   moment "this node's subtree is finished" used to arrive in two unrelated places — the `src = src->next`
   transition and the ascend loop — and neither was a stage, so there was nowhere to put a step that runs AFTER
   step 5 (which is what step 6 is). Step 7 is that moment stated once: the walk arrives at the LEAVE phase
   exactly when `clone a node` would RETURN for `src`, whether `src` is a leaf, the last child of its parent, or
   the root of the whole clone. The ascend that used to be a `while` inside one opcode is now one rest point per
   ancestor, because a page's tree is as deep as the page says and a loop bounded by "the depth already walked"
   is bounded by the page.
   The stage LIST is node.h's, because §5.5 declares it inside its own block too; what is here is the MEMBER's
   two steps around it. */
#define NODE_CLONE_STAGES(X) \
    X(CN_CHECK,  "DOM §4.4 cloneNode step 1 (a ShadowRoot receiver throws NotSupportedError)") \
    NODE_CLONE_ALGO_STAGES(X, CN, "DOM §4.4 cloneNode step 2") \
    X(CN_RETURN, "DOM §4.4 cloneNode step 2 (return the `clone a node` result)")
enum { IDL_STEP_STAGE_BASE(NODE_CLONE_STAGES) NODE_CLONE_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const NODE_CLONE_STEPS[] = { NODE_CLONE_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* §4.4 "CLONE A SINGLE NODE", THE DOCUMENT CASE: "creating a document that implements the same interfaces as
   node, given document's relevant realm", followed by the additional requirements the standard lists for
   Document — "set copy's encoding, content type, URL, origin, type, mode, and allow declarative shadow roots to
   those of node".
   THE REALM IS THE SOURCE DOCUMENT'S, not the running one. §4.4 names `document`'s relevant realm and
   `document` here is the node's own node document, so `otherDoc.cloneNode()` run from a second same-origin
   realm of this agent builds the copy in the realm that owns `otherDoc` — which is also what makes every
   wrapper of the copy's tree resolve its prototype there (node_wrap reads the document's realm).
   WHICH OF THE SEVEN FIELDS THIS ENGINE HAS: the URL and the mode (lexbor's compat_mode, which
   `document.compatMode` reads) are copied; the ORIGIN is the realm's and the copy is made in the source's
   realm, so it is the same origin by construction; the ENCODING is UTF-8 for every document this engine builds
   (document.c's characterSet answers exactly that), so there is nothing that could differ; "allow declarative
   shadow roots" is a PARSE parameter here rather than stored state — declarative_shadow.c takes it per parse —
   and no parse runs into a copy. THREE ARE REAL PER-DOCUMENT STATE AND ARE READ BACK OFF THE SOURCE'S RECORD,
   because each is one a copy could differ in. The CONTENT TYPE: `new DOMParser().parseFromString(s,
   "text/xml").cloneNode()` is an XML document, and a copy built with this engine's html default would answer
   "text/html" for a document that never was one. The TYPE — §4.5's `xml`/`html`, which is what the list above
   means by it — for the same reason, and not lexbor's dtype: a `text/plain` document is an HTML document whose
   content type is not "text/html" (HTML §7.5.4 "Loading text documents"), so re-deriving one from the other
   answers backwards. And the INTERFACE SET, which is NOT the type and is not in the seven-field list at all —
   it is the algorithm's own first clause, "creating a document that implements the same interfaces as node".
   The comment that stood here called the TYPE the interface set, and said the two documents agreed about it
   because both were made the same way. Both halves were wrong: they are two facts (core/dom/document.h's
   DocumentInterface), and the copy is NOT made the same way as the source — it is made HERE, by one entry,
   for sources that six different algorithms created. */
static lxb_dom_node_t *clone_a_document(lxb_dom_document_t *src)
{
    JSContext *realm = document_realm_of(lxb_dom_interface_node(src));
    lxb_html_document_t *copy;
    lxb_dom_document_t *cd;
    JSValue w;

    DCHECK(realm != NULL, "§4.4 creates a document copy in the SOURCE document's relevant realm, and this "
                          "document has no realm — it came from neither document_install nor document_new, so "
                          "there is no realm for its copy's prototypes either");
    copy = dom_document_create();
    CHECK(copy != NULL, "clone a single node: OOM creating the Document copy");
    cd = lxb_dom_interface_document(copy);
    cd->compat_mode = src->compat_mode;      /* "…and MODE": the quirks flag compatMode reports */
    /* A LEXBOR INVARIANT AND NOT §4.4's INTERFACE CLAUSE, which is what this assert's message used to claim.
       lexbor's `dtype` says which of ITS document structs this is; every document in this engine is built by
       lxb_html_document_create, so the two agree, and a disagreement means one of them came from somewhere
       else and the field copies below are being written into a struct of another shape. DOM §4.5's INTERFACE
       is carried explicitly, at document_new below. */
    DCHECK(cd->type == src->type, "a Document copy's lexbor dtype is not the source's — one of the two was "
                                  "built by something other than lxb_html_document_create, so the record and "
                                  "the tree beneath it do not have the shape this copy is about to write");
    /* §4.4 "clone a single node" copies BOTH of §4.5's creation facts and names them in one list — "set
       copy's encoding, content type, URL, origin, TYPE, mode, and allow declarative shadow roots to those of
       node" — so the copy takes the source's type as well as its content type. Taking only the content type
       and re-deriving the type from it would make a clone of a `text/plain` document an XML document, which
       is the derivation DocumentKind exists to make unspellable. */
    /* AND THE INTERFACE IS THE SOURCE'S, WHICH IS THE ALGORITHM'S OWN FIRST CLAUSE AND NOT ONE OF THE SEVEN
       FIELDS ABOVE: "if node is a document, set copy to the result of creating a document that implements THE
       SAME INTERFACES AS NODE, given document's relevant realm". So a clone of a §4.5.1 createDocument is an
       XMLDocument and a clone of a DOMParser XML parse is not — the pair that a re-derivation from `type` would
       collapse, and the pair dom/nodes/Node-cloneNode-XMLDocument.html asserts by reading `clone.constructor`.
       It is READ OFF THE SOURCE'S RECORD for the same reason the content type below it is: the interface is
       real per-document state that a copy could differ in, and there is nothing about an empty Document to
       inspect that would recover it. */
    w = document_new(realm, copy, document_url_of(src), document_interface_of(src),
                     document_kind(document_is_xml_of(src), document_content_type_of(src)));
    CHECK(JS_IsObject(w), "clone a single node: the Document copy's wrapper allocation failed");
    /* The RECORD holds the wrapper (document_new dup'd it) and the record lives as long as the document, so
       this reference has nothing left to do: the answer this algorithm returns is the NODE, and the member
       that wants an object asks node_wrap for it exactly as it does for every other copy. */
    JS_FreeValue(realm, w);
    return lxb_dom_interface_node(copy);
}

/* DOM §4.4 "clone a single node", THE ELEMENT ARM: "let copy be the result of CREATING AN ELEMENT, given
 * document, node's local name, node's namespace, node's namespace prefix and node's `is` value, with
 * synchronous custom elements set to false … for each attribute in node's attribute list, append a clone of it".
 * The standard says "creating an element", so the copy is created from the source's three NAMES and not
 * produced by copying an element struct — and both halves of that turned out to matter.
 *
 * WHY IT IS NOT lxb_dom_document_import_node's, MEASURED. Its lxb_dom_element_interface_copy reaches
 * lxb_dom_node_interface_copy, which takes an early return when the two documents are the same (an exact id
 * copy — so a SAME-document clone was never folded, and the handover that sent me here was wrong about that)
 * and otherwise re-appends the namespace through lxb_ns_append and the prefix through lxb_ns_prefix_append,
 * both of which lower-case. So `document.cloneNode(true)` — whose copy IS made in a new document, because
 * §4.4 step 2 sets `document` to the copy — returned a whole tree of namespaces and prefixes the page had
 * never written, while `el.cloneNode(true)` returned the right ones. One member, two answers, decided by
 * something the page cannot see.
 *
 * AND IT DROPPED THE QUALIFIED NAME IN BOTH. lxb_dom_element_interface_copy never assigns dst->qualified_name
 * at all, and lxb_dom_element_qualified_name falls back to the LOCAL name when that field is zero — so a clone
 * of `createElementNS(SVG_NS, "svg:rect")` reported `tagName` and `nodeName` as `rect`, and serialized as
 * `<rect>`, in every document including its own. That is why this arm exists rather than a namespace repair
 * bolted onto lexbor's copy: the names are the thing being copied, so they are all copied in one place.
 *
 * THE INTERFACE IS STILL LEXBOR'S. lxb_dom_document_create_interface picks the element struct from (tag id,
 * namespace id) exactly as element_create_ns does, and it is handed the IMPORTED ids — which is the same
 * ordering §4.5's storage step needs and for the same reason 0962568a gives: a folded namespace id chooses the
 * wrong struct, and an element whose struct disagrees with its namespace is a state this engine must not
 * reach. */
static lxb_dom_node_t *clone_element_into(lxb_dom_document_t *doc, lxb_dom_element_t *src)
{
    lxb_dom_document_t *from = lxb_dom_interface_node(src)->owner_document;
    lxb_tag_id_t tag = dom_import_tag(doc, from, src->node.local_name);
    lxb_ns_id_t ns = dom_import_namespace(doc, from, src->node.ns);
    lxb_dom_element_t *el;
    lxb_dom_attr_t *a;

    /* §4.4 also passes node's `is` VALUE to create-an-element, and nothing in this engine has ever set one —
       element_create_ns does not take one and lexbor's own element_create is not called. So the copy having
       none is exact today, and this is the assert that says so rather than a silent omission: the day `is`
       becomes real, this fires at the one site that would have dropped it. */
    DCHECK(src->is_value == NULL,
           "an element carries an `is` value and §4.4's clone must pass it to create-an-element — build the "
           "copy of it here, in the destination document's arena, beside the three names");
    el = lxb_dom_interface_element(dom_element_interface_create(doc, tag, ns));
    CHECK(el != NULL, "clone a single node: the element copy's interface could not be created");
    /* THE NAMES ARE COPIED AND THEN IMPORTED AS A SET, through name_intern.h's one list of what names an
       element has — the same list §4.5's adopt moves, so an element that grows a fifth name grows it once.
       The two ids above stay separate only because dom_element_interface_create is CHOSEN by (tag id,
       namespace id) and so cannot wait for them; the import that follows re-answers them identically, since
       interning bytes a document already holds gives back the entry it already had. */
    el->node.local_name = src->node.local_name;
    el->node.ns = src->node.ns;
    el->node.prefix = src->node.prefix;
    el->qualified_name = src->qualified_name;
    dom_import_node_names(doc, from, &el->node);
    DCHECK(el->node.local_name == tag && el->node.ns == ns,
           "§4.4's element copy was created from one (local name, namespace) pair and imported into another — "
           "dom_element_interface_create picked the C struct from the first, so the copy's struct no longer "
           "matches its names and node_interface.c will destroy it as something it is not");
    /* §4.13.3: a copy is created with synchronous custom elements FALSE, so it is not upgraded here — it
       becomes "custom" when §4.13's upgrade reaction runs, which the insertion steps enqueue. `undefined` is
       what §4.13.1 calls an element that has not been through that, and lexbor spells it UNCUSTOMIZED. */
    el->custom_state = LXB_DOM_ELEMENT_CUSTOM_STATE_UNCUSTOMIZED;
    for (a = lxb_dom_element_first_attribute(src); a; a = lxb_dom_element_next_attribute(a))
        dom_attr_attach(el, dom_attr_clone(doc, a), NULL);
    return lxb_dom_interface_node(el);
}

/* §4.4 "clone a single node" — the per-interface switch the standard is written as. The default arm is
   lexbor's clone_interface, and the ASSERTION IS WHY THAT IS EXACT rather than a hope: lxb_dom_node_interface_
   copy re-appends a namespace or a prefix through its case-folding entry only for an id AT OR ABOVE
   __LAST_ENTRY, and copies a STATIC one verbatim because the static tables are one shared array. Every node
   kind that reaches here — Text, Comment, CDATASection, ProcessingInstruction, DocumentType,
   DocumentFragment — is created by lexbor with LXB_NS_HTML and no prefix, both static. A page cannot give one
   of them a namespace of its own, so an id above the table means this engine built the node, and the fold this
   file exists to avoid would be back. The doctype's own name goes through the RAW qualified-name append, so it
   is exact too. */
static lxb_dom_node_t *clone_a_single_node(lxb_dom_document_t *doc, lxb_dom_node_t *n)
{
    lxb_dom_node_t *copy;

    if (n->type == LXB_DOM_NODE_TYPE_ELEMENT)
        return clone_element_into(doc, lxb_dom_interface_element(n));
    DCHECK(n->ns < LXB_NS__LAST_ENTRY && n->prefix < LXB_NS__LAST_ENTRY,
           "a node that is not an element carries a non-static namespace or prefix — lexbor's clone re-appends "
           "exactly those through its case-folding entry, so this kind needs an arm of its own above");
    copy = lxb_dom_document_import_node(doc, n, false);
    CHECK(copy != NULL, "clone a node: the Lexbor node copy failed — returning nothing would hand the page a "
                        "null it has no way to distinguish from a node it never asked for");
    return copy;
}

void node_clone_start(JSStepHdr *hdr, NodeCloneState *s, lxb_dom_node_t *node, bool subtree, int base, int after)
{
    DCHECK(node != NULL, "`clone a node` was started on no node");
    DCHECK(s->src == NULL, "a second `clone a node` was started while one was still walking — the state holds "
                           "ONE walk, and the caller resumes at its `after` stage with the copy in hand");
    s->src = node;
    s->deep = subtree;
    s->after = after;
    hdr->stage = base + NODE_CLONE_PHASE_ROOT;
}

int node_clone_run(JSContext *ctx, JSStepHdr *hdr, NodeCloneState *s, int base)
{
    int phase = hdr->stage - base;

    DCHECK(phase >= 0 && phase < NODE_CLONE_PHASE_N,
           "`clone a node` was resumed at a stage outside the block its caller declared for it");

    if (phase == NODE_CLONE_PHASE_ROOT) {
        lxb_dom_node_t *n = s->src;

        /* STEP 1: "Assert: node is not a document or node is document." The `document` argument defaults to
           node's node document, and lexbor makes a document its own owner_document, so the assert is about
           this entry being the DEFAULTED one. */
        DCHECK(n->type != LXB_DOM_NODE_TYPE_DOCUMENT || lxb_dom_interface_node(n->owner_document) == n,
               "§4.4 step 1: a document is cloned only as its OWN `document` argument");
        if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT) {
            /* STEP 2 for a document, and the line that makes the rest of the walk build the copy's own tree:
               "if node is a document, then set document to copy". Every descendant's `clone a single node`
               creates its copy in the COPY, so the cloned tree's node document is the clone and not the
               original — which is what makes `document.cloneNode(true).body` a node of the copy. */
            s->copy = clone_a_document(lxb_dom_interface_document(n));
            s->doc = lxb_dom_interface_document(s->copy);
        } else {
            s->doc = n->owner_document;      /* the argument's default: node's node document */
            s->copy = clone_a_single_node(s->doc, n);
            dom_cow_note_created(s->copy);   /* the clone ROOT only — its descendants are reachable through it */
        }
        /* NO EARLY RETURN FOR A SHALLOW CLONE. `subtree` gates step 5 and HTML §4.12.3, and step 6 is not
           conditioned on it at all: a clonable shadow root is cloned — deeply, because 6.7 passes TRUE — for
           `host.cloneNode()` exactly as for `host.cloneNode(true)`. So the walk runs either way and each step
           asks the flag itself. */
        s->root = n;
        s->cnode = s->dst = s->croot = s->copy;   /* the root is copied; the walk starts at its children */
        hdr->stage = base + NODE_CLONE_PHASE_TEMPLATE;
        return JS_STEP_YIELD;
    }

    if (phase == NODE_CLONE_PHASE_COPY) {
        /* ONE NODE PER STEP: copy it into `document` and hang it under the copy of its parent. */
        s->cnode = clone_a_single_node(s->doc, s->src);
        dom_cow_insert_private(s->croot, s->dst, s->cnode);
        hdr->stage = base + NODE_CLONE_PHASE_TEMPLATE;
        return JS_STEP_YIELD;
    }

    if (phase == NODE_CLONE_PHASE_TEMPLATE) {
        /* HTML §4.12.3's cloning steps, step 1: "If subtree is false, then return." */
        lxb_dom_node_t *content = s->deep ? node_template_content(s->src) : NULL;
        /* STEP 3 IS EVERY ELEMENT'S CLONING STEPS AND NOT ONLY `<template>`'s. HTML §4.12.1 states another
           pair on this same step — "the cloning steps for script elements given node, copy, and subtree are to
           set copy's already started to node's already started" — and it is not bookkeeping: a script the
           fragment parse marked inert would otherwise have a live CLONE, so
           `host.appendChild(parsed.cloneNode(true))` runs exactly the code §13.4's Inert mode exists to stop.
           It is unconditional on `subtree` because §4.12.1's steps are, unlike §4.12.3's above. */
        html_script_cloned(ctx, s->src, s->cnode);
        /* AND HTML §2.5.6 Nonce attributes STATES A THIRD PAIR ON THIS SAME STEP, over a far wider set than
           either of the two above: "The cloning steps for elements that include HTMLOrSVGOrMathMLElement given
           node, copy, and subtree are to set copy's [[CryptographicNonce]] to node's [[CryptographicNonce]]" —
           every HTML element, not one tag. It is not made redundant by the attribute the clone already copied:
           §2.5.6 exists precisely to make the slot and the attribute stop agreeing, so a copy whose nonce came
           from the attribute is the STALE one, and under a `script-src 'nonce-…'` policy that is the difference
           between a cloned script that runs and one that does not. Unconditional on `subtree`, like §4.12.1's
           above and unlike §4.12.3's. */
        nonce_attribute_cloned(ctx, s->src, s->cnode);
        hdr->stage = base + NODE_CLONE_PHASE_CHILDREN;
        if (content && content->first_child) {
            /* Leave this tree for the template's, on both sides at once. The frame is everything to come back
               to; the copy's fragment is its own private tree, made by clone_interface a moment ago. The
               template ELEMENT's own children are step 5's and still to come, so that is where it resumes. */
            clone_push(ctx, s, base + NODE_CLONE_PHASE_CHILDREN);
            s->root = content;
            s->src = content->first_child;
            s->dst = s->croot = node_template_content(s->cnode);
            s->deep = true;         /* §4.12.3: "subtree set to true" for every content child */
            DCHECK(s->dst != NULL, "a cloned <template> has no content fragment to copy into — lexbor's "
                                   "clone_interface built something other than a template interface");
            hdr->stage = base + NODE_CLONE_PHASE_COPY;
        }
        return JS_STEP_YIELD;
    }

    if (phase == NODE_CLONE_PHASE_CHILDREN) {
        /* Step 5: "If subtree is true, then for each child of node's children, in tree order …" */
        if (s->deep && s->src->first_child) {
            s->src = s->src->first_child;
            s->dst = s->cnode;
            hdr->stage = base + NODE_CLONE_PHASE_COPY;
        } else {
            /* A node with no children finishes step 5 the moment it starts it. */
            hdr->stage = base + NODE_CLONE_PHASE_SHADOW;
        }
        return JS_STEP_YIELD;
    }

    if (phase == NODE_CLONE_PHASE_SHADOW) {
        /* STEP 6, AT THE ONE MOMENT STEP 5 IS FINISHED FOR `src`. Steps 6.1-6.7 belong to §4.8's record and
           are shadow_root.c's; what is this machine's is 6.7, which is `clone a node` over each shadow child
           and therefore this same walk one level down. */
        JSValue sr = shadow_root_clone_onto(ctx, s->src, s->cnode);
        lxb_dom_node_t *shadow, *from;

        if (JS_IsException(sr)) return JS_STEP_ABRUPT;
        hdr->stage = base + NODE_CLONE_PHASE_LEAVE;
        shadow = node_of(sr);
        if (shadow) {
            from = shadow_root_of_element(ctx, lxb_dom_interface_element(s->src));
            DCHECK(shadow_root_is(from), "§4.4 step 6.7 has no shadow tree to clone FROM, and step 6.4 just "
                                         "attached one onto the copy — the two are read from one association");
            if (from->first_child) {
                /* The second tree reached other than through child links, and the same descent the template
                   content level makes. `subtree` is TRUE here whatever the caller asked for, and the host
                   resumes at step 7: steps 5 and 6 are both behind it. */
                clone_push(ctx, s, base + NODE_CLONE_PHASE_LEAVE);
                s->root = from;
                s->src = from->first_child;
                s->dst = s->croot = shadow;
                s->deep = true;
                hdr->stage = base + NODE_CLONE_PHASE_COPY;
            }
        }
        JS_FreeValue(ctx, sr);
        return JS_STEP_YIELD;
    }

    DCHECK(phase == NODE_CLONE_PHASE_LEAVE, "`clone a node` resumed into a phase §4.4 does not have");
    /* STEP 7, "RETURN COPY" — `src` is cloned, its subtree is cloned, and this is the ONE place the walk says
       so. Everything a specification defines to happen after step 5 happens here and nowhere earlier. */
    if (s->src == s->root) {
        /* THE LEVEL IS FINISHED. Only the OUTERMOST level's root is itself a copy — it is the node the
           algorithm was started on, and its step 7 is the answer. An inner level's root is a `<template>`'s
           content fragment or a shadow root, neither of which is CLONED: `clone a node` is invoked on their
           CHILDREN, into a tree the copy already has, so arriving at one is the level ending and nothing else. */
        if (s->sp == 0) {
            s->src = NULL;              /* the walk is over: the state is idle and may be started again */
            hdr->stage = s->after;
            return JS_STEP_YIELD;
        }
        /* §4.2.2.4 "assign slottables for a tree" for a shadow tree this walk just built. A browser reaches it
           through §4.2.3's insertion steps as each shadow child is inserted; these inserts are PRIVATE — the
           copy is a tree nothing has seen — so the slots have never been asked what they hold, exactly as
           HTML §13.2.6.4.4's parsed shadow root had not. What they hold is the copy host's light children,
           which step 5 cloned into place before step 6 ran. */
        if (shadow_root_is(s->croot)) slot_assign_for_a_tree(ctx, s->croot);
        s->sp--;                        /* back to the node that owns this level */
        s->src = s->stack[s->sp].src;
        s->dst = s->stack[s->sp].dst;
        s->root = s->stack[s->sp].root;
        s->croot = s->stack[s->sp].croot;
        s->cnode = s->stack[s->sp].cnode;
        s->deep = s->stack[s->sp].deep;
        hdr->stage = s->stack[s->sp].stage;
        return JS_STEP_YIELD;
    }
    if (s->src->next) {
        /* Step 5's loop advances: the next child of the parent whose copy `dst` still is. */
        s->src = s->src->next;
        hdr->stage = base + NODE_CLONE_PHASE_COPY;
        return JS_STEP_YIELD;
    }
    /* ASCEND ONE, IN LOCKSTEP. The last child of a parent finishing means the PARENT's step 5 is finished too,
       so the parent rests here in its turn — one rest point per ancestor rather than a loop over a depth the
       page chose. `dst` is the copy of `src`'s parent, which is exactly the copy the parent becomes. */
    DCHECK(s->src->parent != NULL && s->dst != NULL,
           "the clone walk ran out of ancestors before reaching its level root — the two trees are walked in "
           "lockstep and the copy is shorter than the original it was built from");
    s->src = s->src->parent;
    s->cnode = s->dst;
    s->dst = s->dst->parent;
    /* The parent's step 6 comes next, because its step 5 has just finished — unless the parent is an inner
       level's ROOT, which is the one node in the level that is not being cloned and so has neither step. */
    hdr->stage = s->src == s->root && s->sp > 0 ? base + NODE_CLONE_PHASE_LEAVE : base + NODE_CLONE_PHASE_SHADOW;
    return JS_STEP_YIELD;
}

static int js_node_clone(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                         JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    NodeCloneState *s = st;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);

    STEP_DISPATCH(NODE_CLONE_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(CN_CHECK);
    {
        lxb_dom_node_t *n = node_of(hdr->this_val);
        /* `optional boolean subtree = false` — the declaration converted it, so this is a real boolean. */
        bool deep = argc > 0 && JS_ToBool(ctx, argv[0]);

        if (!n) { *presult = JS_UNDEFINED; return JS_STEP_DONE; }
        /* STEP 1: "If this is a shadow root, then throw a NotSupportedError." A shadow root is created by
           `attach a shadow root` and by nothing else — a copy of one would be a second root for a host that
           already has one, which is why the standard refuses rather than answering. A DOCUMENT is not refused
           here and never was in the standard: §4.4 defines what its copy is, and clone_a_document builds it. */
        if (shadow_root_is(n)) {
            JS_ThrowDOMException(ctx, "NotSupportedError", "cloneNode on a ShadowRoot");
            return JS_STEP_ABRUPT;
        }
        node_clone_start(hdr, s, n, deep, CN_ROOT, CN_RETURN);      /* STEP 2: `clone a node` given this */
        return JS_STEP_YIELD;
    }

    /* STEP 2's `clone a node`, WHOSE SIX REST POINTS ARE THIS MEMBER'S SIX STAGES. The walk is shared, so each
       of its stages is named here and all six enter it — `case A: case B:` and nothing else. This was
       `if (hdr->stage != CN_RETURN)`, a negation that is a claim about every stage that is not the last: the
       day §4.4 gains a rest point AFTER the return — or this member gains a step of its own after step 2 — the
       new stage falls into the walk, which re-enters `clone a node` on a state whose answer is already built.
       Named individually, a stage added to NODE_CLONE_ALGO_STAGES does not compile until it has an arm here. */
    STEP_ARM(CN_ROOT);
    STEP_ARM(CN_COPY);
    STEP_ARM(CN_TEMPLATE);
    STEP_ARM(CN_CHILDREN);
    STEP_ARM(CN_SHADOW);
    STEP_ARM(CN_LEAVE);
    return node_clone_run(ctx, hdr, s, CN_ROOT);

    STEP_ARM(CN_RETURN);
    /* STEP 2's "return the result": the algorithm left the copy on the state and pointed the stage here. */
    DCHECK(s->copy != NULL, "`clone a node` finished without a copy — its step 7 is what sets the answer, and "
                            "the caller is only ever resumed from there");
    *presult = node_wrap(ctx, s->copy);
    return JS_STEP_DONE;
}

static const IdlStepDecl NODE_CLONE_STEP = {
    /* No release: the level stack is node_clone_visit_state's, and the teardown discharges that one list. */
    js_node_clone, sizeof(NodeCloneState), node_clone_visit, NULL,
    "DOM §4.4 Node.cloneNode (over the `clone a node` concept)", NODE_CLONE_STEPS
};

/* §4.4 insertBefore / replaceChild — the two remaining mutating tree operations, through the same per-flow
   chokepoints appendChild and removeChild already use. §4.4 is where the members and their method steps are
   ("The insertBefore(node, child) method steps are to return the result of pre-inserting node into this before
   child"); §4.2.3 Mutation algorithms is where `pre-insert` and `replace` themselves are written.
   magic 0 = insertBefore, 1 = replaceChild. */
static JSValue js_node_insert(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_node_t *parent = node_of(this_val), *node, *child;

    if (!parent) return JS_UNDEFINED;
    /* §4.4's IDL is `insertBefore(Node node, Node? child)` and `replaceChild(Node node, Node child)`, so the
       SECOND argument's nullability is the whole of what these two differ in before the algorithms start: an
       omitted or null reference child means APPEND for one and is a TypeError for the other. THAT IS THE
       DECLARATION'S NOW — two type arrays rather than one, because one shared array could state only one of
       the two rules and the body then re-derived the other from the magic. Anything that is neither the IDL
       null nor a node fails §3.2.15 Interface types' step 2 in both, in argument order. */
    DCHECK(argc == 2, "insertBefore/replaceChild reached its body without its two declared arguments — neither "
                      "position is optional, so §3.6 Overload resolution algorithm's step 5 arity TypeError is "
                      "what a shorter call gets");
    node = node_of(argv[0]);
    DCHECK(node != NULL, "insertBefore/replaceChild's `Node node` reached the body as something that is not a "
                         "node — the declaration's IDL_INTERFACE position is what makes that a TypeError "
                         "before step 1");
    child = node_of(argv[1]);
    DCHECK(magic == 0 || child != NULL,
           "replaceChild's `Node child` reached the body as something that is not a node — its second position "
           "is NOT nullable in §4.4, which is what IDL_INTERFACE there states and IDL_INTERFACE_NULLABLE at "
           "insertBefore's does not");
    DCHECK(child != NULL || JS_IsNull(argv[1]),
           "a reference child reached the body as neither a node nor the IDL null — §3.2.20 Nullable types is "
           "what turns null and undefined into that null at the declaration");
    if (magic == 0) {
        if (!node_pre_insert(ctx, node, parent, child)) return JS_EXCEPTION;
        return JS_DupValue(ctx, argv[0]);
    }
    if (!node_replace(ctx, node, child, parent)) return JS_EXCEPTION;   /* §4.2.3 "replace", STEPS 1-10 */
    return JS_DupValue(ctx, argv[1]);             /* STEP 11 — replaceChild returns the node it REMOVED */
}

/* DOM §4.4 "Interface Node"'s lookupPrefix / lookupNamespaceURI / isDefaultNamespace — THE THREE MEMBERS, AND
   THE ALGORITHM IS NOT HERE. §4.4 defines two walks ("locate a namespace", "locate a namespace prefix") and
   then three members that
   are each a coercion plus one call into one of them, so that is what these are; core/dom/node_ns.h is the
   walks, and its head comment says why they are a component rather than this body with a magic.
   WHAT THIS FILE USED TO DO INSTEAD, because none of it was visible as a crash: it climbed to the NEAREST
   ANCESTOR ELEMENT (§4.4 says PARENT element, one step, and only if that parent is an element) and then read
   that one element's interned namespace and prefix and stopped. So there was no ancestor walk — step 6 of the
   Element arm — and no step 4 at all, which is the `xmlns:p="…"` / `xmlns="…"` ATTRIBUTE lookup that is the
   only thing that answers for a prefix the context element does not itself carry. `<r xmlns:p="urn:x"><q/></r>`
   answered `q.lookupNamespaceURI("p")` as null, and an `xmlns=""` undeclaration inherited the very binding it
   exists to remove. The two prefixes Namespaces in XML §3 binds BY DEFINITION (`xml`, `xmlns`) were absent
   too, so `lookupNamespaceURI("xml")` was null on every element in every document.
   magic 0 = lookupPrefix, 1 = lookupNamespaceURI, 2 = isDefaultNamespace. */
static JSValue js_node_lookup_ns(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    /* TWO NAMES FOR ONE STRING, and they are not redundant. `owned` is what JS_ToCStringLen handed back and is
       what must be freed; `arg` is the STANDARD'S ARGUMENT, which each member's step 1 may set to null without
       the string ceasing to exist. Freeing through `arg` after that coercion frees nothing and leaks the
       CString on every `lookupNamespaceURI("")` a page makes. */
    const char *owned = NULL, *arg = NULL;
    size_t arg_len = 0, out_len = 0;
    JSValue r;

    if (!n) return magic == 2 ? JS_FALSE : JS_NULL;
    /* The argument is `DOMString?` and the declaration converted it, so this reads a string or the IDL null.
       LEN AND NOT strlen: a namespace name is a page-supplied DOMString and may contain a U+0000, which
       `strlen` would truncate at — and a truncated namespace matches a DIFFERENT declaration rather than
       failing to match, which is a wrong answer that looks like a right one. */
    if (argc > 0 && JS_IsString(argv[0]))
        owned = arg = JS_ToCStringLen(ctx, &arg_len, argv[0]);

    switch (magic) {
    /* "The lookupNamespaceURI(prefix) method steps are: 1. If prefix is the empty string, then set it to null.
       2. Return the result of running locate a namespace for this using prefix." STEP 1 IS THE MEMBER'S and is
       why the walk is handed a coerced argument rather than asked to coerce: the empty string and null are
       different arguments to the walk, and only this member maps one onto the other. */
    case 1: {
        const char *ns;

        if (arg != NULL && arg_len == 0) arg = NULL;
        ns = node_locate_namespace(n, arg, arg_len, &out_len);
        r = ns ? JS_NewStringLen(ctx, ns, out_len) : JS_NULL;
        break;
    }

    /* "The lookupPrefix(namespace) method steps are: 1. If namespace is null or the empty string, then return
       null. 2. Switch on the interface this implements: …" — and every arm of that switch reaches the same
       algorithm over some element, which is what node_ns_start_element answers. */
    case 0: {
        lxb_dom_element_t *el;
        const char *px;

        if (arg == NULL || arg_len == 0) { r = JS_NULL; break; }
        el = node_ns_start_element(n);
        px = el ? node_locate_namespace_prefix(el, arg, arg_len, &out_len) : NULL;
        r = px ? JS_NewStringLen(ctx, px, out_len) : JS_NULL;
        break;
    }

    /* "The isDefaultNamespace(namespace) method steps are: 1. If namespace is the empty string, then set it to
       null. 2. Let defaultNamespace be the result of running locate a namespace for this using NULL. 3. Return
       true if defaultNamespace is the same as namespace; otherwise false."
       THE COMPARISON INCLUDES THE NULL CASE, which is why it is written as one test over two possibly-null
       slices rather than as `if (!arg)` beside it: `isDefaultNamespace(null)` is TRUE exactly when the element
       is in no default namespace, and that is the same sentence, not an exception to it. */
    default: {
        const char *ns;

        DCHECK(magic == 2, "a namespace lookup was declared with a magic this table does not name");
        if (arg != NULL && arg_len == 0) arg = NULL;
        ns = node_locate_namespace(n, NULL, 0, &out_len);
        r = JS_NewBool(ctx, (ns == NULL && arg == NULL) ||
                            (ns != NULL && arg != NULL && out_len == arg_len &&
                             memcmp(ns, arg, out_len) == 0));
        break;
    }
    }
    if (owned) JS_FreeCString(ctx, owned);
    return r;
}

/* DOM §4.2.8 Mixin ChildNode — `Element includes ChildNode`, `CharacterData includes ChildNode` and
   `DocumentType includes ChildNode`, which is why it is a table the interfaces that DECLARE it ask for rather
   than members on Node.prototype: `document.remove()` is not a thing.
   THE NUMBER AND THE INTERFACE LIST WERE BOTH WRONG HERE — "§4.2.7" is Mixin NonDocumentTypeChildNode (built
   below), and the third includer is DocumentType, not DocumentFragment (a fragment has no parent to be
   removed from). document_type.c really does install this table, so the comment was describing a program
   nobody wrote; the title beside the number is what makes the next reader able to see that. */
/* `undefined before((Node or DOMString)... nodes)` and the three beside it — the union and the variadic tail
   are both DECLARED, so every argument is a Node or a real string by the time the body runs. */
static const IdlArgType MIXIN_NODES[1] = { IDL_STRING_UNLESS_IFACE };
/* THE TABLE NO LONGER CARRIES A `length`, AND THAT COLUMN IS WHY THIS WAS WRONG. Every row here reaches ONE
   declaration — the single idl_method_id_ext in mixin_declare below — so the members' Web IDL §3.7.7
   Operations `length` is one number, and a per-row copy of it is a fact seven rows were each asked to
   remember. They did not agree: `remove` and `replaceChildren` said 0 while `before`, `after`, `replaceWith`,
   `append` and `prepend` said 1, for the same arity. See mixin_install for the derivation. */
/* AND `remove` IS NOT ONE OF THE VARIADIC ONES — DOM §4.2.8 Mixin ChildNode writes `undefined remove();`, with
   no argument at all, where its three siblings write `((Node or DOMString)... nodes)`. That was a comment here
   and a shared declaration below, so `remove` was declared with the tail as well, and Web IDL converts every
   argument a declaration LISTS: `el.remove({toString(){ … }})` ran the page's `toString` where a browser reads
   nothing and runs nothing. Its §3.7.7 Operations `length` came out 0 either way (a variadic tail is optional),
   which is exactly why the wrong declaration survived a length audit — the arity and the length are two facts
   and only one of them was visible from outside. `takes_nodes` is that fact, per row, in the IDL's own terms. */
typedef struct { const char *name; int magic; bool takes_nodes; } NodeMixinMember;
static const NodeMixinMember CHILD_NODE_MIXIN[] = {
    { "remove", 0, false }, { "before", 1, true }, { "after", 2, true }, { "replaceWith", 3, true },
};

/* DOM §4.2.6 Mixin ParentNode, its insertion half — `Document includes ParentNode`, `DocumentFragment
   includes ParentNode`, `Element includes ParentNode`. querySelector and children are the same mixin and
   already live on the interfaces that declare them. (The number read "§4.2.8", which is Mixin ChildNode:
   this file cited two different sections for ParentNode and neither reader could tell which was the typo.) */
static const NodeMixinMember PARENT_NODE_MIXIN[] = {
    { "append", 4, true }, { "prepend", 5, true }, { "replaceChildren", 6, true },
};

/* §4.2.6 THE ParentNode MIXIN'S READS AND LOOKUPS, over the node the mixin is ON.
   They were TWO implementations: element.c's, which took `elem_of(this_val)`, and document.c's, which ignored
   its receiver entirely and scoped every lookup to the global document's root element. Two consequences, both
   silent. `otherDoc.querySelector(s)` searched THIS document. And Document had children / firstElementChild /
   lastElementChild / childElementCount not at all — §4.2.6 puts them on the mixin, so a page reading
   `document.children` got undefined and took the branch behind it.
   ONE implementation, on the receiver, is also what makes DocumentFragment's members exist rather than being a
   third copy: the mixin is what the IDL says these are, so it is what installs them.
   The RECEIVER is any node the mixin is included by — Element, Document, DocumentFragment. Anything else is a
   page calling a mixin member on a Text node, which the spec answers by simply not having the member there. */
static bool node_is_parent_node(const lxb_dom_node_t *n)
{
    return n && (n->type == LXB_DOM_NODE_TYPE_ELEMENT ||
                 n->type == LXB_DOM_NODE_TYPE_DOCUMENT ||
                 node_is_document_fragment(n));
}

/* magic 0 = children, 1 = firstElementChild, 2 = lastElementChild, 3 = childElementCount. `children` is a LIVE
   HTMLCollection with a NAMED getter, which is how a great deal of older code reaches its own markup
   (`form.children.email`); the rest are plain reads of the tree. */
static JSValue js_node_element_children(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *n = node_of(this_val), *c, *first = NULL, *last = NULL;
    uint32_t count = 0;

    if (!node_is_parent_node(n))
        return magic == 3 ? JS_NewInt32(ctx, 0) : (magic == 0 ? JS_UNDEFINED : JS_NULL);
    if (magic == 0) return collections_children(ctx, this_val);
    for (c = n->first_child; c; c = c->next) {
        if (c->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (!first) first = c;
        last = c;
        count++;
    }
    switch (magic) {
    case 1: return node_wrap(ctx, first);
    case 2: return node_wrap(ctx, last);
    default: return JS_NewInt32(ctx, (int)count);
    }
}

/* §4.5/§4.9 getElementsByTagName / getElementsByClassName — over the RECEIVER, and LIVE. Document's copy did
   neither: it searched a global root and answered with a static Array, which its own comment named as a
   fidelity gap ("the spec's collection re-walks the tree on every read... this does not"). It also did not
   exist on Element at all, where §4.9 puts it. The gap closed by the collection component growing a descendant
   kind rather than by a second walk here. magic 0 = by tag name, 1 = by class name. */
static JSValue js_node_by_name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    const char *name;
    JSValue r;

    if (!node_is_parent_node(n) || argc < 1) return JS_UNDEFINED;
    name = concolic_name_cstr(ctx, argv[0]);   /* the declaration passes UNKNOWN input through as itself, so an unknown name denotes its SHAPE */
    if (!name) return JS_EXCEPTION;
    r = collections_by_name(ctx, this_val, name, magic != 0);
    JS_FreeCString(ctx, name);
    return r;
}

/* §4.5/§4.9 getElementsByTagNameNS — the OTHER by-name algorithm, and a different one rather than the same one
   with an extra argument: it matches the LOCAL name case-sensitively (no HTML lowercasing) and the NAMESPACE,
   with `*` meaning "any" in each position independently. The walk lives in the collection component beside its
   sibling; what belongs here is §4.5's step 1. */
static JSValue js_node_by_tag_ns(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    const char *ns = NULL, *local;
    JSValue r;

    (void)magic;
    if (!node_is_parent_node(n) || argc < 2) return JS_UNDEFINED;
    /* §4.5 step 1: THE EMPTY STRING IS THE NULL NAMESPACE. Performed here rather than inside the walk so that
       `getElementsByTagNameNS("", "x")` and `getElementsByTagNameNS(null, "x")` are one query by construction —
       the standard states the conversion once and everything after it relies on having been given null. */
    if (!JS_IsNull(argv[0])) {
        ns = JS_ToCString(ctx, argv[0]);   /* a real string by now: the declaration converted it */
        if (!ns) return JS_EXCEPTION;
        if (!*ns) { JS_FreeCString(ctx, ns); ns = NULL; }
    }
    local = concolic_name_cstr(ctx, argv[1]);
    if (!local) { if (ns) JS_FreeCString(ctx, ns); return JS_EXCEPTION; }
    r = collections_by_tag_ns(ctx, this_val, ns, local);
    JS_FreeCString(ctx, local);
    if (ns) JS_FreeCString(ctx, ns);
    return r;
}

static const JSCFunctionListEntry js_parent_node_reads[] = {
    JS_CGETSET_MAGIC_DEF("children", js_node_element_children, NULL, 0),
    JS_CGETSET_MAGIC_DEF("firstElementChild", js_node_element_children, NULL, 1),
    JS_CGETSET_MAGIC_DEF("lastElementChild", js_node_element_children, NULL, 2),
    JS_CGETSET_MAGIC_DEF("childElementCount", js_node_element_children, NULL, 3),
};

/* DOM §4.2.7 Mixin NonDocumentTypeChildNode — the sibling half of the tree walk, and the ONE §4.2 mixin this
   engine had none of. Its two getters "return the first preceding/following sibling that is an element;
   otherwise null", so they are the same walk as §4.2.6's firstElementChild in the other direction, over a tree
   this engine already holds: no layout, no device, no unknown.
   IT IS THE SILENT KIND OF GAP, which is why it is built rather than left on a list. An absent ATTRIBUTE is
   `undefined` — JS_GetPropertyInternal's absent hook fires only for the global object, so a miss on an element
   is a plain undefined with nothing to say so — and real code reads these two through `?.` and `=== null`,
   both of which swallow it: `n.nextElementSibling?.nextElementSibling` yields undefined and the feature behind
   it returns early, and `a = t.nextElementSibling; if (a === null) throw` does NOT throw on undefined and then
   treats it as an element. Neither leaves an error for the frontier to learn from; the flow simply explores
   less than the page can do.
   NOT on DocumentType, which is the whole reason this is a separate mixin from §4.2.8's ChildNode beside it —
   §4.2.7's own note: "Web compatibility prevents the previousElementSibling and nextElementSibling attributes
   from being exposed on doctypes (and therefore on ChildNode)."
   magic 0 = previousElementSibling, 1 = nextElementSibling. */
static JSValue js_node_element_sibling(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *n = node_of(this_val), *c;

    if (!n) return JS_NULL;
    /* `Element includes NonDocumentTypeChildNode` and `CharacterData includes NonDocumentTypeChildNode` are
       the whole of §4.2.7's includes, so a receiver that is neither is a prototype this mixin was installed on
       and the spec does not put it on — a doctype answering `nextElementSibling` is the exact compatibility
       break the section exists to prevent, and it would be invisible without this. */
    DCHECK(n->type == LXB_DOM_NODE_TYPE_ELEMENT || node_is_chardata(n),
           "§4.2.7's element-sibling getter ran on a node that is neither an Element nor a CharacterData — "
           "the mixin has been installed on a prototype §4.2.7 does not include (DocumentType is the one the "
           "section names by hand)");
    for (c = magic ? n->next : n->prev; c; c = magic ? c->next : c->prev)
        if (c->type == LXB_DOM_NODE_TYPE_ELEMENT)
            return node_wrap(ctx, c);
    return JS_NULL;
}

static const JSCFunctionListEntry js_non_doctype_child_reads[] = {
    JS_CGETSET_MAGIC_DEF("previousElementSibling", js_node_element_sibling, NULL, 0),
    JS_CGETSET_MAGIC_DEF("nextElementSibling", js_node_element_sibling, NULL, 1),
};

void node_install_non_doctype_child_mixin(JSContext *ctx, JSValueConst proto)
{
    JS_SetPropertyFunctionList(ctx, proto, js_non_doctype_child_reads,
                               (int)(sizeof(js_non_doctype_child_reads) /
                                     sizeof(js_non_doctype_child_reads[0])));
}

/* §4.2.4 THE NonElementParentNode MIXIN — getElementById, and nothing else is in it.
   IT WAS TWO IMPLEMENTATIONS, and the second one's comment argued that it had to be: "same algorithm, different
   scope". That was a rationalisation of a duplicate. Different scope is exactly what a mixin member over its
   RECEIVER already is, which is what the ParentNode consolidation established — and the two had drifted in both
   of the ways duplicates do. Document's ignored its receiver entirely and searched a global, so
   `otherDoc.getElementById(x)` searched this one. And it reached for lxb_dom_elements_by_attr, which collects
   EVERY match into a collection and then takes the first, so it walked the whole document after already having
   the answer — for a member whose entire definition is "the FIRST element in tree order".
   A MACHINE, because that walk is the document's size. One node per step, and the first match ends it. */
/* WHERE THIS MACHINE RESTS. §4.2.4 states the member as one sentence — "the first element, in tree order,
   within this's descendants, whose ID is elementId" — so the two stages are its two halves: fixing what is
   being searched for, and the walk that answers it, which rests once per node. */
#define NODE_BYID_STAGES(X) \
    X(BYID_START, "DOM §4.2.4 getElementById (the id to search for; this's descendants, in tree order)") \
    X(BYID_WALK,  "DOM §4.2.4 getElementById (the first element whose ID is elementId), one node per step")
enum { IDL_STEP_STAGE_BASE(NODE_BYID_STAGES) NODE_BYID_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const NODE_BYID_STEPS[] = { NODE_BYID_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    lxb_dom_node_t *root, *cursor;
    char   *id;
    size_t  idlen;
} NodeByIdState;

static void node_byid_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    NodeByIdState *s = st;
    /* The cursors are Lexbor nodes, which belong to the document. The id is this machine's own copy — the
       JSString it came from is released before the first suspension, and two forked arms must not share one
       buffer that either of them frees. */
    v->buf(ctx, (void **)&s->id, s->id ? s->idlen + 1 : 0);
}

static int js_node_get_element_by_id(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                     JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    NodeByIdState *s = st;
    lxb_dom_node_t *n;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);

    if (hdr->stage == BYID_START) {
        lxb_dom_node_t *self = node_of(hdr->this_val);
        const char *id;

        *presult = JS_NULL;
        if (!self || argc < 1) return JS_STEP_DONE;
        id = concolic_name_cstr(ctx, argv[0]);   /* the declaration passes UNKNOWN input through as itself, so an unknown name denotes its SHAPE */
        if (!id) return JS_STEP_ABRUPT;
        s->idlen = strlen(id);
        s->id = js_malloc(ctx, s->idlen + 1);
        CHECK(s->id != NULL, "getElementById could not copy the id it was asked for");
        memcpy(s->id, id, s->idlen + 1);
        JS_FreeCString(ctx, id);
        s->root = self;
        s->cursor = self->first_child;
        hdr->stage = BYID_WALK;
        return JS_STEP_YIELD;
    }

    DCHECK(hdr->stage == BYID_WALK, "getElementById resumed into a stage §4.2.4 does not have");
    n = s->cursor;
    if (!n) { *presult = JS_NULL; return JS_STEP_DONE; }
    if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        size_t vlen = 0;
        const lxb_char_t *v = lxb_dom_element_get_attribute(lxb_dom_interface_element(n),
                                                            (const lxb_char_t *)"id", 2, &vlen);
        if (v && vlen == s->idlen && memcmp(v, s->id, s->idlen) == 0) {
            *presult = node_wrap(ctx, n);   /* the FIRST in tree order — the walk stops here */
            return JS_STEP_DONE;
        }
    }
    s->cursor = node_next_in(n, s->root);
    return JS_STEP_YIELD;
}

static const IdlStepDecl NODE_BYID_STEP = {
    /* No release: the id copy is node_byid_visit's, discharged with the rest. */
    js_node_get_element_by_id, sizeof(NodeByIdState), node_byid_visit, NULL,
    "DOM §4.2.4 NonElementParentNode.getElementById", NODE_BYID_STEPS
};

/* §4.2.4, installed on the interfaces whose IDL INCLUDES it: Document and DocumentFragment. Not Element —
   `el.getElementById` is not a member of anything, which is why this is a mixin and not a Node member. */
static int g_id_by_id = -1;

void node_install_nonelement_parent_mixin(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_id_by_id >= 0, "the NonElementParentNode mixin was installed before it was declared");
    idl_install_method(ctx, proto, "getElementById", g_id_by_id);
}

/* The members that WALK, installed through the declaration because they are machines. */
/* DECLARED ONCE PER AGENT, INSTALLED PER REALM. The IDL pool is SEALED after agent init precisely so that a
   per-realm mint is caught (idl_declared_before_seal), and a shared "install these members" helper that mints
   inline is exactly that bug: it works for the first realm and aborts on the second, naming the member. */
static int g_w_equal = -1, g_w_pos = -1, g_w_append = -1, g_w_remove = -1, g_w_insert = -1, g_w_replace = -1,
           g_w_normalize = -1, g_w_clone = -1, g_w_same = -1, g_w_contains = -1;

/* EVERY ONE OF §4.4's INTERFACE-TYPED POSITIONS, STATED AS THE TYPE IT IS — and the nullability is not a
   detail these members share, it is what tells them apart. §4.4 "Interface Node" writes
     boolean isEqualNode(Node? otherNode);          boolean isSameNode(Node? otherNode);
     unsigned short compareDocumentPosition(Node other);   boolean contains(Node? other);
     [CEReactions] Node insertBefore(Node node, Node? child);   [CEReactions] Node appendChild(Node node);
     [CEReactions] Node replaceChild(Node node, Node child);    [CEReactions] Node removeChild(Node child);
   so `insertBefore`'s reference child is nullable and `replaceChild`'s is NOT, and one shared two-position
   array could express neither. Web IDL §3.2.15 Interface types is two steps — "If V implements I, then return
   the IDL interface type value that represents a reference to that platform object", then "Throw a TypeError"
   — and §3.2.20 Nullable types is what puts null and undefined in front of it for the four that carry a `?`.
   THESE WERE `IDL_ANY` AND THE REFUSAL WAS EACH BODY'S, which is the consumer-performs-the-type's-refusal
   split, and here it did not merely move the TypeError — it LOST one: `n.isEqualNode(5)` answered `false`,
   because a body that reaches for the argument's node and finds none has no way left to tell "not equal" from
   "not a Node". The type answers that before the algorithm's step 1 and no body can forget it.
   THE BRAND IS THE NODE WRAPPER CLASS. node_wrap builds EVERY node kind with `g_node_class` and uses the
   claimed per-type class only to pick the PROTOTYPE, so one class id is exactly the granularity `Node` needs —
   which is why `idl_iface_brand(node_class_id())` is what §4.2.6's moveBefore, §4.5's adoptNode and §5.5's
   Range members already brand against. */
static void node_declare_walkers(JSContext *ctx)
{
    static const IdlArgType ONE_NODE[1] = { IDL_INTERFACE };
    static const IdlArgType ONE_NODE_OR_NULL[1] = { IDL_INTERFACE_NULLABLE };
    static const IdlArgType NODE_THEN_NULLABLE[2] = { IDL_INTERFACE, IDL_INTERFACE_NULLABLE };
    static const IdlArgType NODE_THEN_NODE[2] = { IDL_INTERFACE, IDL_INTERFACE };
    static const IdlArgType ONE_BOOL[1] = { IDL_BOOLEAN };

    g_w_equal = idl_method_id_step(ctx, ONE_NODE_OR_NULL, 1, NULL, 0, &NODE_EQUAL_STEP, 0);
    idl_iface_brand(node_class_id());
    g_w_pos = idl_method_id_step(ctx, ONE_NODE, 1, NULL, 0, &NODE_POS_STEP, 0);
    idl_iface_brand(node_class_id());
    /* §4.4's two other `Node?` predicates. They were `JS_CFUNC_MAGIC_DEF` rows on the base table, which is a
       member with no declaration at all — so `n.contains(5)` and `n.isSameNode(5)` answered `false` where
       §3.2.15 step 2 throws, the same lost refusal isEqualNode had one row up. hasChildNodes() declares no
       argument and stays on the table, because a member with nothing to convert has nothing to declare. */
    g_w_same = idl_method_id(ctx, ONE_NODE_OR_NULL, 1, js_node_predicates, 1);
    idl_iface_brand(node_class_id());
    g_w_contains = idl_method_id(ctx, ONE_NODE_OR_NULL, 1, js_node_predicates, 2);
    idl_iface_brand(node_class_id());
    /* §4.4's four MUTATING members — the section that DECLARES them and states their method steps; §4.2.3
       Mutation algorithms is where the algorithms those steps call are written. */
    g_w_append = idl_method_id(ctx, ONE_NODE, 1, js_node_child_op, 0);
    idl_iface_brand(node_class_id());
    g_w_remove = idl_method_id(ctx, ONE_NODE, 1, js_node_child_op, 1);
    idl_iface_brand(node_class_id());
    g_w_insert = idl_method_id(ctx, NODE_THEN_NULLABLE, 2, js_node_insert, 0);
    idl_iface_brand(node_class_id());
    g_w_replace = idl_method_id(ctx, NODE_THEN_NODE, 2, js_node_insert, 1);
    idl_iface_brand(node_class_id());
    /* `undefined normalize()` — no arguments to convert and still three loops' worth of the page's tree. */
    g_w_normalize = idl_method_id_step(ctx, NULL, 0, NULL, 0, &NODE_NORM_STEP, 0);
    /* `[CEReactions] Node cloneNode(optional boolean subtree = false)`. ToBoolean is total and runs none of the
       page's code, but the argument still crosses CONVERTED — a body handed the raw value would have to
       remember to coerce it, which is the per-body mistake one declaration exists to have none of.
       AND OVER UNKNOWN EXTERNAL INPUT THE DECLARATION IS WHAT FORKS IT. §7.1.2 ToBoolean's last step is
       "Return true", so `cloneNode(cfg.deep)` read from a plain body would copy the subtree in every world
       and the shallow clone would never exist; IDL_BOOLEAN is IDL_CONCOLIC_FORKS, so the coercion is asked at
       the branch seam at the boundary and both documents are explored. */
    g_w_clone = idl_method_id_step(ctx, ONE_BOOL, 1, NULL, 0, &NODE_CLONE_STEP, 0);
    idl_optional_from(0);   /* §4.4: `cloneNode(optional deep = false)` */
}

/* The members that WALK, installed through the declaration because they are machines. */
static void node_install_walkers(JSContext *ctx, JSValueConst proto)
{
    idl_install_method(ctx, proto, "isEqualNode", g_w_equal);
    idl_install_method(ctx, proto, "isSameNode", g_w_same);
    idl_install_method(ctx, proto, "contains", g_w_contains);
    idl_install_method(ctx, proto, "compareDocumentPosition", g_w_pos);
    idl_install_method(ctx, proto, "appendChild", g_w_append);
    idl_install_method(ctx, proto, "removeChild", g_w_remove);
    idl_install_method(ctx, proto, "insertBefore", g_w_insert);
    idl_install_method(ctx, proto, "replaceChild", g_w_replace);
    idl_install_method(ctx, proto, "normalize", g_w_normalize);
    idl_install_method(ctx, proto, "cloneNode", g_w_clone);
}

/* ONE DECLARATION PER MIXIN MEMBER, minted at agent init and keyed by the member's own MAGIC — which is what
   makes the id reusable across every prototype that includes the mixin, and across every realm. */
static int g_mixin_id[32];
static int g_mixin_declared;

static void node_declare_mixins(JSContext *ctx);

static void mixin_declare(JSContext *ctx, const NodeMixinMember *tab, unsigned n)
{
    unsigned k;
    for (k = 0; k < n; k++) {
        DCHECK(tab[k].magic >= 0 && tab[k].magic < (int)(sizeof(g_mixin_id) / sizeof(g_mixin_id[0])),
               "a node mixin member's magic is outside the declaration table — the table is indexed BY magic so "
               "that one member is one declaration however many mixins include it");
        if (g_mixin_id[tab[k].magic] < 0)
            /* TWO DECLARATIONS BECAUSE THE IDL WRITES TWO SIGNATURES — see NodeMixinMember. A member that
               declares no arguments converts none, so `remove`'s body is reached with the page's value
               untouched and unread, which is what §4.2.8's `remove()` means. */
            g_mixin_id[tab[k].magic] = tab[k].takes_nodes
                ? idl_method_id_ext(ctx, MIXIN_NODES, 1, /*variadic*/ true, node_class_id(),
                                    js_node_mixin, tab[k].magic)
                : idl_method_id(ctx, NULL, 0, js_node_mixin, tab[k].magic);
    }
    g_mixin_declared = 1;
}

/* WEB IDL §3.7.7 Operations' `length` FOR EVERY MEMBER OF BOTH MIXINS, WHICH IS 0, DERIVED AND NOT REMEMBERED.
   §3.7.7's last three steps are verbatim: "Compute the effective overload set for regular operations … with
   identifier id on target and with argument count 0, and let S be the result. Let length be the length of the
   shortest argument list in the entries in S. Let F be CreateBuiltinFunction(steps, length, id, « », realm)."
   §2.5.8 Overloading's compute-the-effective-overload-set is what puts a length-0 entry in S here, and the
   load-bearing half is its trailing loop rather than its variadic expansion: step 5.7 only ever appends
   LONGER tuples (i runs n to max − 1), while step 5.9.1 breaks only when "arguments[i] is not optional (i.e.,
   it is not marked as optional and is not a final, variadic argument)" — so for a member whose only argument
   IS a final variadic one the loop does not break, step 5.9.5 appends the tuple its own note spells out
   ("if i is 0, this means to add to S the tuple (X, « », « »)"), and the shortest argument list in S is empty.
   Every row of both tables is that member: `(Node or DOMString)... nodes`, plus §4.2.8's `remove()`, which
   declares no argument at all and is therefore 0 by step 5.6.
   SO `el.append()` IS A LEGAL CALL AND `Element.prototype.append.length` IS 0, and the 1 that stood here made
   the second a lie a bundle can branch on: feature detection and polyfill shims read `.length` off these
   members precisely because the mixin post-dates the interfaces it is included by.
   THE NUMBER IS NO LONGER WRITTEN HERE AT ALL — idl_install_method derives it (idl_member_length_of), so the
   paragraph above is a statement of what the pool computes rather than of what this call passes. */
static void mixin_install(JSContext *ctx, JSValueConst proto, const NodeMixinMember *tab, unsigned n)
{
    unsigned k;
    DCHECK(g_mixin_declared, "a node mixin was installed before its members were declared");
    for (k = 0; k < n; k++)
        idl_install_method(ctx, proto, tab[k].name, g_mixin_id[tab[k].magic]);
}

void node_install_child_mixin(JSContext *ctx, JSValueConst proto)
{
    mixin_install(ctx, proto, CHILD_NODE_MIXIN,
                  (unsigned)(sizeof(CHILD_NODE_MIXIN) / sizeof(CHILD_NODE_MIXIN[0])));
}

/* THE WHOLE MIXIN, in one call. An interface that includes ParentNode gets everything §4.2.6 lists — not the
   three insertion members while its reads and lookups are re-declared per interface, which is how Document
   ended up without `children` and with a querySelector that ignored its receiver. */
static int g_id_qs = -1, g_id_qsa = -1, g_id_by_tag = -1, g_id_by_class = -1, g_id_by_tag_ns = -1;
/* §4.2.6's `moveBefore` is NOT one of the mixin table's entries above, and the reason is the table's own shape:
   every member in it is variadic `(Node or DOMString)... nodes` over §4.2.6's "convert nodes into a node".
   `moveBefore(Node node, Node? child)` converts nothing and inserts nothing — it is §4.2.3's move — so it is
   its own declaration on the same three prototypes. */
static int g_id_move_before = -1;

void node_install_parent_mixin(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_id_qs >= 0, "the ParentNode mixin was installed before it was declared");
    mixin_install(ctx, proto, PARENT_NODE_MIXIN,
                  (unsigned)(sizeof(PARENT_NODE_MIXIN) / sizeof(PARENT_NODE_MIXIN[0])));
    JS_SetPropertyFunctionList(ctx, proto, js_parent_node_reads,
                               (int)(sizeof(js_parent_node_reads) / sizeof(js_parent_node_reads[0])));
    idl_install_method(ctx, proto, "querySelector", g_id_qs);
    idl_install_method(ctx, proto, "querySelectorAll", g_id_qsa);
    idl_install_method(ctx, proto, "moveBefore", g_id_move_before);
    /* Not part of ParentNode in the IDL — §4.5 puts these on Document and §4.9 on Element, which between them
       is every interface that includes ParentNode except DocumentFragment. Installed here because that is one
       place rather than two, and a fragment answering them is a superset nothing can observe as wrong: its
       subtree is exactly what the walk would search. */
    idl_install_method(ctx, proto, "getElementsByTagName", g_id_by_tag);
    idl_install_method(ctx, proto, "getElementsByClassName", g_id_by_class);
    idl_install_method(ctx, proto, "getElementsByTagNameNS", g_id_by_tag_ns);
}

/* Every mixin's declarations, once per agent — see node_declare_walkers for why this is split at all. */
static void node_declare_mixins(JSContext *ctx)
{
    static const IdlArgType ONE_STR[1] = { IDL_DOMSTRING };

    mixin_declare(ctx, CHILD_NODE_MIXIN, (unsigned)(sizeof(CHILD_NODE_MIXIN) / sizeof(CHILD_NODE_MIXIN[0])));
    mixin_declare(ctx, PARENT_NODE_MIXIN, (unsigned)(sizeof(PARENT_NODE_MIXIN) / sizeof(PARENT_NODE_MIXIN[0])));
    g_id_by_id = idl_method_id_step(ctx, ONE_STR, 1, NULL, 0, &NODE_BYID_STEP, 0);
    g_id_qs = idl_method_id_step(ctx, ONE_STR, 1, NULL, 0, document_qs_decl(), 0);
    g_id_qsa = idl_method_id_step(ctx, ONE_STR, 1, NULL, 0, document_qs_decl(), 1);
    {
        /* §4.2.6: `[CEReactions] undefined moveBefore(Node node, Node? child)`. BOTH POSITIONS ARE INTERFACE
           TYPES AND NEITHER IS OPTIONAL, and stating that here is what makes the two TypeErrors the standard's
           own test asks for come out of §3.6 rather than out of this component: `moveBefore(text)` fails step
           5's arity check, `moveBefore({}, null)` and `moveBefore(node, {})` fail the interface conversion, and
           `moveBefore(node, null)` and `moveBefore(node, undefined)` are the nullable position's IDL null. The
           brand is the node wrapper class — one class for every node kind, which is exactly the granularity
           `Node` needs. */
        static const IdlArgType NODE_AND_NULLABLE_NODE[2] = { IDL_INTERFACE, IDL_INTERFACE_NULLABLE };

        g_id_move_before = idl_method_id(ctx, NODE_AND_NULLABLE_NODE, 2, js_node_move_before, 0);
        idl_iface_brand(node_class_id());
    }
    g_id_by_tag = idl_method_id(ctx, ONE_STR, 1, js_node_by_name, 0);
    g_id_by_class = idl_method_id(ctx, ONE_STR, 1, js_node_by_name, 1);
    {
        /* §4.5: `getElementsByTagNameNS(DOMString? namespace, DOMString localName)` — the first is
           NULLABLE, which is what makes `null` a namespace rather than the four characters. */
        static const IdlArgType NS_STR[2] = { IDL_DOMSTRING_NULLABLE, IDL_DOMSTRING };
        g_id_by_tag_ns = idl_method_id(ctx, NS_STR, 2, js_node_by_tag_ns, 0);
    }
}

static const JSCFunctionListEntry js_node_base[] = {
    JS_CFUNC_MAGIC_DEF("hasChildNodes", 0, js_node_predicates, 0),
    JS_CGETSET_DEF("nodeType", js_node_get_type, NULL),
    JS_CGETSET_DEF("nodeName", js_node_get_name, NULL),
    JS_CGETSET_DEF("childNodes", js_node_child_nodes, NULL),
    JS_CGETSET_MAGIC_DEF("parentNode", js_node_tree, NULL, 0),
    JS_CGETSET_MAGIC_DEF("firstChild", js_node_tree, NULL, 1),
    JS_CGETSET_MAGIC_DEF("lastChild", js_node_tree, NULL, 2),
    JS_CGETSET_MAGIC_DEF("nextSibling", js_node_tree, NULL, 3),
    JS_CGETSET_MAGIC_DEF("previousSibling", js_node_tree, NULL, 4),
    JS_CGETSET_MAGIC_DEF("isConnected", js_node_facts, NULL, 0),
    JS_CGETSET_MAGIC_DEF("ownerDocument", js_node_facts, NULL, 1),
    JS_CGETSET_MAGIC_DEF("parentElement", js_node_facts, NULL, 2),
    JS_CGETSET_MAGIC_DEF("baseURI", js_node_facts, NULL, 3),
};

/* §4.4 the nodeType and DOCUMENT_POSITION_* constants. Web IDL puts a `const` on BOTH the interface object and
   the prototype, so one table installs both — and a page writes `n.nodeType === Node.ELEMENT_NODE` far more
   often than it writes the number. */
static const JSCFunctionListEntry js_node_consts[] = {
    JS_PROP_INT32_DEF("ELEMENT_NODE", 1, 0),
    JS_PROP_INT32_DEF("ATTRIBUTE_NODE", 2, 0),
    JS_PROP_INT32_DEF("TEXT_NODE", 3, 0),
    JS_PROP_INT32_DEF("CDATA_SECTION_NODE", 4, 0),
    JS_PROP_INT32_DEF("ENTITY_REFERENCE_NODE", 5, 0),
    JS_PROP_INT32_DEF("ENTITY_NODE", 6, 0),
    JS_PROP_INT32_DEF("PROCESSING_INSTRUCTION_NODE", 7, 0),
    JS_PROP_INT32_DEF("COMMENT_NODE", 8, 0),
    JS_PROP_INT32_DEF("DOCUMENT_NODE", 9, 0),
    JS_PROP_INT32_DEF("DOCUMENT_TYPE_NODE", 10, 0),
    JS_PROP_INT32_DEF("DOCUMENT_FRAGMENT_NODE", 11, 0),
    JS_PROP_INT32_DEF("NOTATION_NODE", 12, 0),
    JS_PROP_INT32_DEF("DOCUMENT_POSITION_DISCONNECTED", NODE_POS_DISCONNECTED, 0),
    JS_PROP_INT32_DEF("DOCUMENT_POSITION_PRECEDING", NODE_POS_PRECEDING, 0),
    JS_PROP_INT32_DEF("DOCUMENT_POSITION_FOLLOWING", NODE_POS_FOLLOWING, 0),
    JS_PROP_INT32_DEF("DOCUMENT_POSITION_CONTAINS", NODE_POS_CONTAINS, 0),
    JS_PROP_INT32_DEF("DOCUMENT_POSITION_CONTAINED_BY", NODE_POS_CONTAINED_BY, 0),
    JS_PROP_INT32_DEF("DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC", NODE_POS_IMPLEMENTATION_SPECIFIC, 0),
};


/* §4.4 Node.textContent — a NODE member, which is where it now lives. It was an Element one, so
   `textNode.textContent` was undefined and `document.textContent` answered a string the spec says is null; and
   because Element was the only kind that had it, the same body had to be both the element algorithm and the
   character-data one. Here each branch is the spec's own case, and the shared "string replace all" is written
   once.
   NOT A MARKUP SINK — the setter creates ONE Text node and the getter concatenates descendant text — which is
   exactly why a page told to stop using innerHTML uses it, and why an engine lacking it saw those pages build
   nothing. The taint travels the way an attribute's does: the assigned concolic is recorded on the element's
   property slot, so a source parked in the DOM as text and later read back into a real sink is still solved. */
static bool node_has_children_as_text(const lxb_dom_node_t *n)
{
    return n->type == LXB_DOM_NODE_TYPE_ELEMENT || node_is_document_fragment(n);
}

/* §4.4 textContent's READ — A MACHINE, and the same shape as §8.4's serialiser: the answer is the concatenated
   data of every Text node under this one, so the work is the SUBTREE, and it was a plain C accessor.
   `document.body.textContent` on a real page is the whole document inside one opcode.
   THE COMPLETING SPELLING OF THE SAME CONCATENATION IS core/dom/text_content.h, and this walk cannot call it:
   that one measures and copies in two passes and returns, while this one must REST between nodes, so the two
   share the PREDICATE (dom_node_is_text) and nothing else. That is the whole of what they have to agree about
   — which nodes count — and it is exactly what they disagreed about while this file spelled the test itself:
   DOM §4.12 "Interface CDATASection" makes a CDATA section a Text node and a nodeType comparison does not.
   The traversal is over CHILD LINKS, which is DOM §1.1's descendant relation — so a `<template>`'s content,
   reached only through §4.7's host, is not part of its element's text. */
/* WHERE THIS MACHINE RESTS. §4.4's textContent getter is `get text content`, which SWITCHES on the interface
   `this` implements — every arm but one is O(1) and answered in the first stage — and the Element and
   DocumentFragment arm is `descendant text content`, a concatenation over every Text descendant in tree order.
   That concatenation is the page's subtree, so it is its own stage and rests once per node. */
#define NODE_TEXT_STAGES(X) \
    X(NODE_TEXT_SWITCH,      "DOM §4.4 get text content (switch on the interface this implements)") \
    X(NODE_TEXT_DESCENDANTS, "DOM §4.4 descendant text content (concatenate each Text descendant's data, in " \
                             "tree order), one node per step")
enum { IDL_STEP_STAGE_BASE(NODE_TEXT_STAGES) NODE_TEXT_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const NODE_TEXT_STEPS[] = { NODE_TEXT_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    lxb_dom_node_t *root, *cursor;
    char   *out;
    size_t  out_len, out_cap;
} NodeTextState;

/* THE RUNTIME'S ALLOCATOR, BECAUSE THE DECLARATION'S IS — node_text_visit hands the sibling a js_malloc'd copy
   of this accumulator and the teardown discharges it with js_free. */
static void node_text_append(JSContext *ctx, NodeTextState *s, const char *data, size_t len)
{
    if (s->out_len + len + 1 > s->out_cap) {
        size_t want = s->out_cap ? s->out_cap * 2 : 128;
        char *n;
        while (want < s->out_len + len + 1) want *= 2;
        n = js_realloc(ctx, s->out, want);
        CHECK(n != NULL, "textContent could not grow its accumulator");
        s->out = n;
        s->out_cap = want;
    }
    memcpy(s->out + s->out_len, data, len);
    s->out_len += len;
}

static void node_text_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    NodeTextState *s = st;
    /* The cursors are Lexbor nodes, which belong to the document. The accumulator is this machine's own: two
       forked arms each append their own remaining text, so neither can share the other's buffer. */
    v->buf(ctx, (void **)&s->out, s->out_cap);
}

static int js_node_get_text_content(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                    JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    NodeTextState *s = st;
    lxb_dom_node_t *n;

    (void)argc; (void)argv; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);

    if (hdr->stage == NODE_TEXT_SWITCH) {
        n = node_of(hdr->this_val);
        *presult = JS_NULL;
        if (!n) return JS_STEP_DONE;
        /* Every O(1) answer is given here rather than walked to: a CharacterData node IS its data, and §4.4
           says a node with no children-as-text has textContent null — a Document's is null, not "". */
        if (node_is_chardata(n)) {
            *presult = js_cd_get_data(ctx, hdr->this_val, 1);
            return JS_STEP_DONE;
        }
        if (!node_has_children_as_text(n)) return JS_STEP_DONE;
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            int si = attr_shadow_find(lxb_dom_interface_element(n), ATTR_SLOT_PROPERTY, NULL, "textContent");
            if (si >= 0) {
                /* A source parked here as TEXT comes back as the same concolic, not as the bytes its shape
                   wrote into the tree — that is what keeps it solvable at a later sink. */
                *presult = JS_DupValue(ctx, attr_shadow_opaque(si));
                return JS_STEP_DONE;
            }
        }
        s->root = n;
        s->cursor = node_next_in(n, n);
        hdr->stage = NODE_TEXT_DESCENDANTS;
        return JS_STEP_YIELD;
    }

    DCHECK(hdr->stage == NODE_TEXT_DESCENDANTS, "textContent resumed into a stage §4.4 does not have");
    if (!s->cursor) {
        /* §4.4's answer, and then the one question a data block's content owes — HTML §4.12.1 The script
           element: the concatenated Text data under a `<script>` the user agent does not process is the
           DOCUMENT'S OWN bytes, so what leaves here is the solver triple carrying them rather than a bare
           string (core/loader/data_block.h).
           `s->root` is the node whose descendants were walked, which is the element these bytes belong to. */
        *presult = data_block_wrap_text(ctx, lxb_dom_interface_element(s->root),
                                        JS_NewStringLen(ctx, s->out ? s->out : "", s->out_len));
        return JS_STEP_DONE;
    }
    /* §4.4's own words for what this walk counts: "To get text content with a node node, return the following,
       switching on the interface node implements" — an INTERFACE, so a CDATASection is a Text node and its
       data is part of §4.11's descendant text content. This tested the nodeType, which is a different question
       that happens to agree on every document holding no CDATA section. */
    if (dom_node_is_text(s->cursor)) {
        lxb_dom_character_data_t *cd = lxb_dom_interface_character_data(s->cursor);
        node_text_append(ctx, s, (const char *)cd->data.data, cd->data.length);
    }
    s->cursor = node_next_in(s->cursor, s->root);
    return JS_STEP_YIELD;
}

static const IdlStepDecl NODE_TEXT_STEP = {
    /* No release: the accumulator is node_text_visit's, discharged with the rest. */
    js_node_get_text_content, sizeof(NodeTextState), node_text_visit, NULL,
    "DOM §4.4 Node.textContent getter (over `get text content`)", NODE_TEXT_STEPS
};

static JSValue js_node_set_text_content(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    lxb_dom_node_t *n = node_of(this_val), *node, *next;
    lxb_dom_text_t *text;
    const char *str;
    size_t len = 0;
    bool owned_cstr = false;

    (void)magic;
    if (!n) return JS_UNDEFINED;
    if (node_is_chardata(n))
        return js_cd_set_data(ctx, this_val, val, 1);   /* §4.4 CharacterData: "replace data" */
    if (!node_has_children_as_text(n))
        return JS_UNDEFINED;                            /* §4.4 "otherwise: do nothing" */

    if (n->type == LXB_DOM_NODE_TYPE_ELEMENT)
        dom_cow_set_prop_taint(ctx, lxb_dom_interface_element(n), "textContent",
                               concolic_is(val) ? val : JS_UNDEFINED);

    if (JS_IsNull(val)) {
        str = "";                     /* `DOMString?`: null is the empty string for "string replace all" */
    } else if (concolic_is(val)) {
        /* Unknown external input has no bytes: the SHAPE is what the Text node carries while the shadow above
           carries the value. */
        str = concolic_shape_c(val);
        if (!str) str = "";
        len = strlen(str);
    } else {
        DCHECK(JS_IsString(val), "textContent= reached the body unconverted — the IDL declaration is what "
                                 "converts it, and running the page's toString from here is the "
                                 "drive-to-completion the flow machinery exists to avoid");
        str = JS_ToCStringLen(ctx, &len, val);
        if (!str) return JS_EXCEPTION;
        owned_cstr = true;
    }

    /* §4.4 "string replace all": every child goes — through the per-flow chokepoint, so a forked arm that sets
       different text reads back its own — and then ONE Text node, ONLY IF the string is not empty. Appending an
       empty Text node unconditionally gave `el.textContent = ""` a child the spec says it has none of, which a
       page checking `firstChild` or `childNodes.length` reads as a tree it never built. */
    for (node = n->first_child; node; node = next) {
        next = node->next;
        dom_cow_remove_child(node);
    }
    if (len) {
        text = lxb_dom_document_create_text_node(n->owner_document, (const lxb_char_t *)str, len);
        DCHECK(text != NULL, "textContent= produced no Text node — the page's text would silently not be there");
        if (text) {
            dom_cow_note_created(lxb_dom_interface_node(text));   /* this flow made it: the delta owns it */
            dom_cow_append_child(n, lxb_dom_interface_node(text));
        }
    }
    if (owned_cstr)
        JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_chardata_base[] = {
    JS_CGETSET_DEF("length", js_cd_get_length, NULL),
};

/* The interface a node TYPE wears, borrowed — what a component installing its interface object names. */
JSValue node_type_proto(JSContext *ctx, int node_type)
{
    JSValue proto;

    DCHECK(g_protos_ready, "a node type's interface was asked for before node_init declared the table");
    DCHECK(node_type > 0 && node_type < LXB_DOM_NODE_TYPE_LAST_ENTRY,
           "a node type the DOM does not have was asked for its interface");
    proto = JS_GetClassProto(ctx, g_type_class[node_type]);
    DCHECK(!JS_IsNull(proto),
           "a node type's interface was asked for in a realm that never built it — the component that claimed "
           "the type declares a per-realm install (core/realm.h), and a realm that skipped it has no prototype "
           "for nodes of that kind to wear");
    return proto;   /* OWNED */
}

JSValue node_proto(JSContext *ctx)
{
    JSValue proto;

    DCHECK(g_protos_ready, "Node.prototype was asked for before node_init declared the interface");
    proto = JS_GetClassProto(ctx, g_node_class);
    DCHECK(!JS_IsNull(proto), "Node.prototype was asked for in a realm that never ran node_install_protos");
    return proto;   /* OWNED */
}

void node_claim_type(int node_type, JSClassID cls)
{
    DCHECK(g_protos_ready, "a derived interface claimed a node type before node_init declared Node");
    DCHECK(node_type > 0 && node_type < LXB_DOM_NODE_TYPE_LAST_ENTRY,
           "a node type the DOM does not have was claimed");
    DCHECK(cls != 0, "a node type was claimed by no class — the claim is what names the per-realm prototype");
    DCHECK(g_type_class[node_type] == g_node_class,
           "two components claimed the same node type's interface — one of them would silently lose");
    g_type_class[node_type] = cls;
}

JSValue node_wrap(JSContext *ctx, lxb_dom_node_t *n)
{
    JSValue obj;
    int i;

    if (!n)
        return JS_NULL;
    if (g_wrap_cap) {
        unsigned slot = node_wrap_slot(g_wraps, g_wrap_cap, n);
        if (g_wraps[slot].n)
            return JS_DupValue(ctx, g_wraps[slot].obj);
    }

    DCHECK(g_protos_ready, "a node was wrapped before the DOM interfaces existed");
    DCHECK((int)n->type > 0 && (int)n->type < LXB_DOM_NODE_TYPE_LAST_ENTRY,
           "a Lexbor node carries a type the DOM does not define");
    DCHECK(n->type != LXB_DOM_NODE_TYPE_ELEMENT || g_element_resolver != NULL,
           "an Element node was wrapped before the HTML layer registered its interface resolver");
    /* THE PROTOTYPE IS THE NODE'S DOCUMENT'S REALM'S, and the distinction is the whole reason a wrapper is
       cached: there is ONE wrapper per node — two same-origin documents are one agent and both see the same
       object, which is what makes `frame.contentDocument.body === frame.contentDocument.body` hold across the
       boundary — so the realm cannot be "whichever flow touched it first". A member reached through that
       prototype runs in the realm that DEFINED it, so a child document's `body.tagName` answered out of the
       parent's realm for no reason but the order of the reads.
       It is OWNED, which is the one cost this conversion adds to the hottest path in the DOM: a refcount pair
       per wrap. */
    {
        JSContext *rctx = document_realm_of(n);
        JSValue proto;

        DCHECK(rctx != NULL,
               "a node was wrapped in a document no realm was installed for — its prototype is that document's "
               "and there is none, so build the realm rather than lending it the wrapping flow's");
        /* TWO NODE KINDS ANSWER THIS FROM THE NODE AND NOT FROM THE TYPE TABLE, and they are the two the
           standards say so about. An ELEMENT's interface is HTML §3.2.2 "Elements in the DOM"'s tag mapping,
           which the html layer registers above. A DOCUMENT's is DOM §4.5 "Interface Document"'s "create a
           document that implements an interface" — the interface its creating algorithm NAMED, which for
           §4.5.1's createDocument is `XMLDocument` and for every other creator is `Document`. Neither question
           is answerable by a table keyed on node TYPE, which is exactly why both are asked here rather than
           there: `node_claim_type` admits ONE claimant per type, and it is right to, because the type is not
           what decides. */
        proto = (n->type == LXB_DOM_NODE_TYPE_ELEMENT && g_element_resolver)
                    ? g_element_resolver(rctx, lxb_dom_interface_element(n))
              : (n->type == LXB_DOM_NODE_TYPE_DOCUMENT)
                    ? document_interface_proto(rctx, lxb_dom_interface_document(n))
                    : node_type_proto(rctx, (int)n->type);
        obj = JS_NewObjectProtoClass(ctx, proto, g_node_class);
        JS_FreeValue(ctx, proto);
    }
    if (JS_IsException(obj))
        return obj;
    JS_SetOpaque(obj, n);

    if ((g_wrap_n + 1) * 2 > g_wrap_cap)
        node_wrap_grow();
    {
        unsigned slot = node_wrap_slot(g_wraps, g_wrap_cap, n);
        DCHECK(g_wraps[slot].n == NULL, "a node was wrapped twice — the lookup above missed an entry the insert "
                                        "then found, which is two JS objects for one node and every identity "
                                        "comparison between them false");
        g_wraps[slot].n = n;
        g_wraps[slot].obj = JS_DupValue(ctx, obj);   /* the map holds it; node_wrap_forget releases it */
        g_wrap_n++;
    }
    return obj;
}

/* THE WRAPPER THIS NODE ALREADY HAS, or JS_UNDEFINED — the same lookup node_wrap opens with, without the
   allocation that follows it. A component asking a question whose answer is kept on a wrapper (§4.9's shadow
   root association) must not MINT one to find out: the parser inserts every node in the document through the
   tree walk that asks, and minting there would put a wrapper in the identity map for every node in the page.
   BORROWED — the map's own reference outlives any caller, because it is released only when the node dies. */
JSValueConst node_wrap_peek(const lxb_dom_node_t *n)
{
    unsigned slot;

    if (!n || !g_wrap_cap)
        return JS_UNDEFINED;
    slot = node_wrap_slot(g_wraps, g_wrap_cap, n);
    return g_wraps[slot].n ? g_wraps[slot].obj : JS_UNDEFINED;
}

/* REMOVE an entry, keeping the probe chains intact. Open addressing with linear probing cannot simply blank a
   slot: every entry after it that probed PAST it would become unreachable, and a lookup would miss a node that
   is still in the table and hand out a second wrapper for it. Backward-shift deletion moves each following
   entry that belongs at or before the hole into it, which restores the invariant without tombstones — and no
   tombstones is what keeps the table from degrading back into the thing this replaced. */
static void node_wrap_remove(const lxb_dom_node_t *n)
{
    unsigned i, j, k, mask;

    if (!g_wrap_cap)
        return;
    mask = (unsigned)(g_wrap_cap - 1);
    i = node_wrap_slot(g_wraps, g_wrap_cap, n);
    if (g_wraps[i].n != n)
        return;                      /* already gone: a wrapper outliving its node is the flow's teardown order */
    g_wraps[i].n = NULL;
    g_wrap_n--;
    for (j = (i + 1) & mask; g_wraps[j].n; j = (j + 1) & mask) {
        /* its HOME slot — where it WOULD hash to — not the slot it currently occupies, which is the whole
           question being asked. node_wrap_slot answers the second and would make this always continue. */
        k = (unsigned)(((uintptr_t)g_wraps[j].n * 2654435769u) >> 16) & mask;
        if ((i <= j) ? (i < k && k <= j) : (i < k || k <= j))
            continue;                /* j is reachable from its home without passing the hole — leave it */
        g_wraps[i] = g_wraps[j];
        g_wraps[j].n = NULL;
        i = j;
    }
}

/* THE NODE OWNS ITS WRAPPER, so nothing is released here — the entry holds a reference and the DOM releases it
   when the NODE dies (node_wrap_forget, from the destroy chokepoint). A purely weak map was the other way round
   and it is measurably worse: the wrapper is then collected whenever no JS reference happens to be live, so the
   next `el.firstChild` allocates a fresh object and re-resolves its prototype, and a DOM-heavy page pays that
   on nearly every access. Measured on the smoke fixture: the same exploration went from ~200 s to over 1500 s
   of execution. Identity is also cheaper to reason about this way — a node's wrapper is the same object for as
   long as the node exists, which is what the DOM says. */
static void node_finalizer(JSRuntime *rt, JSValue val) { (void)rt; (void)val; }

/* THE NODE IS GONE, so its wrapper entry goes with it. This is what was missing: the map only ever grew, so
   every node any of thousands of exploration flows ever created stayed in it forever — the table reached a size
   where ONE doubling was a five-second calloc-and-rehash inside a single createElement, with no suspend point
   in it. It was also a correctness trap, because a pool allocator reuses addresses and the next node at a dead
   node's address inherited its wrapper and its prototype.
   IT IS CALLED FROM THE NODE'S DEATH AND NOT FROM WHOEVER CAUSED IT. Every node in this engine is freed through
   core/dom/node_interface.c's destroy dispatcher (an Attr is the one exception, and attr_list.c's
   dom_attr_destroy is the point that plays the dispatcher's role for it), so the call sits inside the free
   rather than in a sweep each caller of a destroy has to remember to run first. It used to be the second of
   those, and the list of callers that remembered was missing the biggest one — a whole document's tree. */
void node_wrap_forget(lxb_dom_node_t *n)
{
    unsigned slot;

    if (!n || !g_wrap_cap)
        return;
    slot = node_wrap_slot(g_wraps, g_wrap_cap, n);
    if (g_wraps[slot].n != n)
        return;                       /* never wrapped: the common case for a node no script ever touched */
    /* AN ENTRY EXISTS, SO THE RUNTIME MUST — the two-sided half of node_agent_runtime's contract, asked after
       the lookup rather than before it because "no runtime" is legitimate exactly while the map is empty. */
    DCHECK(g_agent_rt != NULL,
           "a wrapped node was destroyed with no agent runtime to release its wrapper to — the map is holding "
           "a strong reference this cannot free, so either the wrap happened before node_init named the "
           "runtime or a document is being torn down after JS_FreeRuntime with the map still live");
    /* NEUTER IT FIRST. The map's reference is not necessarily the last one — page code in the flow being
       discarded may still hold the wrapper, and its refcount keeps the JSObject alive after this. Leaving the
       freed node in its opaque makes the next property access a use-after-free that reads as an out-of-bounds
       somewhere else entirely; nulling it makes that access hit the DCHECK the accessors already carry, at the
       site that made it. */
    JS_SetOpaque(g_wraps[slot].obj, NULL);
    JS_FreeValueRT(g_agent_rt, g_wraps[slot].obj);
    node_wrap_remove(n);
}

/* NULL IS AN ANSWER HERE AND IT IS A POSITIVE ONE: "this agent has no runtime", which is true before node_init
   declares the DOM and true again after node_free hands the last of it back. It is not a hole a caller fills —
   the two side maps this exists for are EMPTY in both of those spans (node_free walks its own table above;
   solver/attr_shadow.c's attr_shadow_free runs in the same cascade), so there is nothing to release and a
   runtime would have nothing to do. The assertion that this is so belongs at each map, where the count that
   makes it true can be read: attr_shadow_forget crashes if it is handed no runtime while it still holds
   entries, and node_wrap_forget's own check sits after the lookup for exactly the same reason.
   main.c is why this is not hypothetical — its teardown destroys the page's document AFTER JS_FreeRuntime, so
   the last document in the process is torn down with no runtime in existence. */
JSRuntime *node_agent_runtime(void)
{
    return g_agent_rt;
}

/* THE AGENT'S DECLARATIONS: the classes, the IDL pool entries, and which class each node type wears. The
   PROTOTYPES those classes name are built per realm, in node_install_protos. */
static int g_id_cd[5] = { -1, -1, -1, -1, -1 };   /* §4.10's five splice members */
static int g_id_nodevalue = -1, g_id_textcontent = -1, g_id_textcontent_get = -1, g_id_data = -1,
           g_id_lookup_prefix = -1, g_id_lookup_ns = -1, g_id_default_ns = -1, g_id_root = -1,
           g_id_split_text = -1, g_id_text_ctor = -1, g_id_comment_ctor = -1;

/* DOM §4.11 Interface Text's and DOM §4.14 Interface Comment's CONSTRUCTORS — "The new Text(data) constructor
 * steps are to set this's data to data and this's node document to current global object's associated
 * Document", and DOM §4.14's sentence is the same one with Comment in it. TWO SENTENCES, ONE BODY, because
 * they differ only in which Lexbor factory runs; the magic says which.
 *
 * THEY WERE THE SHARED `js_node_iface_ctor` THROW, and nothing said so. Web IDL §3.7.1 Interface object gives
 * every exposed interface a property on the global and gives it [[Construct]] steps only where the interface
 * is declared with a constructor operation — so `Text` and `Comment` were on the global, answered
 * `instanceof`, carried DOM §4.4's constants, and threw "Illegal constructor" on the one thing a page writes
 * them for. `node_install_interface` is for the interfaces that declare NO constructor, which is what
 * CDATASection (DOM §4.12 declares none) and ProcessingInstruction still reach it as — the second of those
 * for the reason stated at its install below, not because it declares none.
 *
 * THE DEFAULT IS THE SPEC'S AND NOT `undefined`. `constructor(optional DOMString data = "")`, so a page
 * writing `new Text()` gets a node whose data is the EMPTY STRING. Stringifying the absent argument would give
 * it the nine characters "undefined" — a real value, plausible, and wrong, which is the shape this engine
 * treats as worst. `idl_optional_from(0)` at the declaration is what makes Web IDL §3.6 Overload resolution
 * algorithm stop short of position 0 rather than convert an absent one, and the `argc >= 1` here is what
 * supplies DOM §4.11's own default in its place.
 *
 * The node is DETACHED and belongs to the flow that made it, exactly as document.c's createTextNode says of
 * its own: nothing shared has changed until the page inserts it, and the creation entry is what gives the
 * node's Lexbor bytes an owner — without it every node a page ever constructs stays in the document's arena
 * for the life of the instance, invisible to the runtime's gc_obj_list walk, which sees only GC objects.
 * magic 0 = Text (DOM §4.11), 1 = Comment (DOM §4.14). */
static JSValue js_cd_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_node_t *root = document_root_node(ctx), *made;
    const char *s = "";
    size_t len = 0;
    JSValue r;

    (void)new_target;
    DCHECK(magic == 0 || magic == 1, "§4.11's and §4.14's shared constructor body was reached with a magic "
                                     "naming neither Text nor Comment");
    DCHECK(root != NULL, "a CharacterData constructor ran before the document existed — its own step is \"set "
                         "this's node document to current global object's associated Document\", and there is "
                         "no document for that step to name");
    /* Already a STRING by the time it arrives: the position is declared IDL_DOMSTRING, so §3.2.10 DOMString's
       ToString — and any `toString` the page hung on the argument — ran on the IDL machine, where it can
       suspend, before this body was entered.
       UNLESS IT IS UNKNOWN INPUT, WHICH IS THE OTHER HALF AND IS WHY THIS ASKS. idl_concolic_rule answers
       CROSSES for IDL_DOMSTRING — an unknown value reaches the body UNCONVERTED so that a later branch on it
       still forks — and `JS_ToCStringLen` on one runs ToString into ToPrimitive and collapses the very thing
       that was preserved. `new Text(location.hash.slice(1))` is the case: the taint has to survive into the
       node, so an unknown denotes its SHAPE (concolic_name_cstr, the same accessor a selector, an attribute
       name and a class token reach for) and everything else converts normally.
       THE TWO ARMS ARE NOT ONE CALL, and the length is why. A DOMString may contain U+0000, so the concrete
       arm needs the REAL byte count `JS_ToCStringLen` reports and `strlen` would truncate at the NUL; a shape
       carries none by construction, so `strlen` is exact for it. Both are OWNED and both are freed with
       JS_FreeCString. */
    if (argc >= 1) {
        if (concolic_is(argv[0])) {
            s = concolic_name_cstr(ctx, argv[0]);
            if (!s) return JS_EXCEPTION;
            len = strlen(s);
        } else {
            s = JS_ToCStringLen(ctx, &len, argv[0]);
            if (!s) return JS_EXCEPTION;
        }
    }
    if (magic == 0) {
        lxb_dom_text_t *t = lxb_dom_document_create_text_node(root->owner_document,
                                                             (const lxb_char_t *)s, len);
        CHECK(t != NULL, "new Text(): the Lexbor node allocation failed — handing back a null the page cannot "
                         "tell from a node it never asked for is not an option");
        made = lxb_dom_interface_node(t);
    } else {
        lxb_dom_comment_t *c = lxb_dom_document_create_comment(root->owner_document,
                                                              (const lxb_char_t *)s, len);
        CHECK(c != NULL, "new Comment(): the Lexbor node allocation failed — handing back a null the page "
                         "cannot tell from a node it never asked for is not an option");
        made = lxb_dom_interface_node(c);
    }
    dom_cow_note_created(made);   /* this flow made it; detached until the page inserts it */
    if (argc >= 1) JS_FreeCString(ctx, s);
    r = node_wrap(ctx, made);
    return r;
}

void node_init(JSContext *ctx)
{
    JSClassDef def = { "Node", .finalizer = node_finalizer };
    JSClassDef cd_def = { "CharacterData" }, tx_def = { "Text" }, cm_def = { "Comment" },
               cs_def = { "CDATASection" }, pi_def = { "ProcessingInstruction" };
    static const IdlArgType ROOT_ARGS[1] = { IDL_DICT };
    static const IdlDictMember ROOT_OPTS[] = { { "composed", IDL_BOOLEAN } };   /* GetRootNodeOptions */
    int i;

    if (g_protos_ready) {
        /* ONE AGENT IS ONE RUNTIME, and this is where that is a statement rather than an assumption. The
           identity map below is keyed on an address out of the agent's ONE node heap, so two runtimes sharing
           it would have each other's nodes in one table and would release each other's wrappers. */
        DCHECK(g_agent_rt == JS_GetRuntime(ctx),
               "the DOM was declared a second time from a DIFFERENT runtime — the wrapper identity map is one "
               "table keyed on a raw node address, so two runtimes would be holding references into each "
               "other's heaps through it");
        return;    /* element.c asks for the base before declaring Element on top of it */
    }
    g_agent_rt = JS_GetRuntime(ctx);
    /* §2.9's dispatch walks the tree, and this is the file that has one. */
    event_target_set_tree(&NODE_EVENT_TREE);
    engine_set_wrap_stats(node_wrap_stats);

    JS_NewClassID(JS_GetRuntime(ctx), &g_node_class);
    JS_NewClass(JS_GetRuntime(ctx), g_node_class, &def);
    JS_NewClassID(JS_GetRuntime(ctx), &g_chardata_class);
    JS_NewClass(JS_GetRuntime(ctx), g_chardata_class, &cd_def);
    JS_NewClassID(JS_GetRuntime(ctx), &g_text_class);
    JS_NewClass(JS_GetRuntime(ctx), g_text_class, &tx_def);
    JS_NewClassID(JS_GetRuntime(ctx), &g_comment_class);
    JS_NewClass(JS_GetRuntime(ctx), g_comment_class, &cm_def);
    JS_NewClassID(JS_GetRuntime(ctx), &g_cdata_class);
    JS_NewClass(JS_GetRuntime(ctx), g_cdata_class, &cs_def);
    JS_NewClassID(JS_GetRuntime(ctx), &g_pi_class);
    JS_NewClass(JS_GetRuntime(ctx), g_pi_class, &pi_def);

    /* Every node kind is a Node until a component claims it. A ProcessingInstruction wrapper answering the Node
       members is honest; a bare object answering none of them is not. */
    for (i = 0; i < LXB_DOM_NODE_TYPE_LAST_ENTRY; i++)
        g_type_class[i] = g_node_class;
    for (i = 0; i < (int)(sizeof(g_mixin_id) / sizeof(g_mixin_id[0])); i++)
        g_mixin_id[i] = -1;
    g_protos_ready = 1;
    /* Text and Comment are their own interfaces — `interface Text : CharacterData` and `Comment : CharacterData`
       — so a page's `x instanceof Text` and `Text.prototype` have something to name, and the two do not share
       one object that answers for both. */
    node_claim_type(LXB_DOM_NODE_TYPE_TEXT, g_text_class);
    node_claim_type(LXB_DOM_NODE_TYPE_COMMENT, g_comment_class);
    /* §4.12 and §4.13. Unclaimed, both wore Node.prototype — a node answering `nodeType === 4` with no `data`
       on it, which is the shape this engine treats as worst because nothing throws until the page's own read. */
    node_claim_type(LXB_DOM_NODE_TYPE_CDATA_SECTION, g_cdata_class);
    node_claim_type(LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION, g_pi_class);

    /* `attribute DOMString? nodeValue` and `attribute DOMString? textContent` — both Node members. */
    g_id_nodevalue = idl_setter_id(ctx, IDL_DOMSTRING_NULLABLE, false, js_cd_set_data, 1);
    g_id_textcontent = idl_setter_id(ctx, IDL_DOMSTRING_NULLABLE, false, js_node_set_text_content, 0);
    g_id_textcontent_get = idl_getter_id_step(ctx, &NODE_TEXT_STEP, 0);
    /* `attribute [LegacyNullToEmptyString] DOMString data` — the extended attribute is part of the TYPE, so the
       body never sees a null and never has to remember the rule. */
    g_id_data = idl_setter_id(ctx, IDL_DOMSTRING, true, js_cd_set_data, 0);
    {
        /* §4.10's five members, and their IDL is the whole of the argument handling: `unsigned long offset`,
           `unsigned long count`, `DOMString data`. Declared here, installed per realm, like every other. */
        static const IdlArgType CD_UL_UL[2]     = { IDL_UNSIGNED_LONG, IDL_UNSIGNED_LONG };
        static const IdlArgType CD_STR[1]       = { IDL_DOMSTRING };
        static const IdlArgType CD_UL_STR[2]    = { IDL_UNSIGNED_LONG, IDL_DOMSTRING };
        static const IdlArgType CD_UL_UL_STR[3] = { IDL_UNSIGNED_LONG, IDL_UNSIGNED_LONG, IDL_DOMSTRING };
        /* EACH DECLARES ITS OWN ALGORITHM AND STEP LIST OVER THE ONE BODY, which is what the magic is for and
           what keeps a parked flow able to say which of the five it is inside. */
        g_id_cd[0] = idl_method_id_step(ctx, CD_UL_UL,     2, NULL, 0, &CD_SUBSTRING_STEP, 0);
        g_id_cd[1] = idl_method_id_step(ctx, CD_STR,       1, NULL, 0, &CD_APPEND_STEP,    1);
        g_id_cd[2] = idl_method_id_step(ctx, CD_UL_STR,    2, NULL, 0, &CD_INSERT_STEP,    2);
        g_id_cd[3] = idl_method_id_step(ctx, CD_UL_UL,     2, NULL, 0, &CD_DELETE_STEP,    3);
        g_id_cd[4] = idl_method_id_step(ctx, CD_UL_UL_STR, 3, NULL, 0, &CD_REPLACE_STEP,   4);
        /* §4.11 `[NewObject] Text splitText(unsigned long offset)` — one `unsigned long`, and the declaration
           is what converts it, so an object argument runs its `valueOf` on the machine like every other. */
        g_id_split_text = idl_method_id_step(ctx, CD_UL_UL, 1, NULL, 0, &CD_SPLIT_STEP, 0);
        /* §4.11's and §4.14's `constructor(optional DOMString data = "")` — see js_cd_ctor. One declared
           position, optional FROM position 0, so `new Text()` is a zero-argument call that reaches the body
           with argc 0 rather than one whose absent argument was stringified to "undefined". */
        g_id_text_ctor = idl_method_id(ctx, CD_STR, 1, js_cd_ctor, 0);
        idl_optional_from(0);
        g_id_comment_ctor = idl_method_id(ctx, CD_STR, 1, js_cd_ctor, 1);
        idl_optional_from(0);
    }
    /* §4.4 the three namespace lookups. Each takes a `DOMString?`, so each goes on the shared IDL machine —
       `n.lookupPrefix({toString(){ … }})` is the page's code exactly like every other DOMString argument. */
    g_id_lookup_prefix = idl_method_id(ctx, IDL_1NSTR, 1, js_node_lookup_ns, 0);
    g_id_lookup_ns = idl_method_id(ctx, IDL_1NSTR, 1, js_node_lookup_ns, 1);
    g_id_default_ns = idl_method_id(ctx, IDL_1NSTR, 1, js_node_lookup_ns, 2);
    g_id_root = idl_method_id_dict(ctx, ROOT_ARGS, 1, ROOT_OPTS,
                                   (int)(sizeof(ROOT_OPTS) / sizeof(ROOT_OPTS[0])), js_node_root, 0);
    idl_optional_from(0);   /* §4.4: `getRootNode(optional GetRootNodeOptions options = {})` */
    node_declare_walkers(ctx);
    node_declare_mixins(ctx);
    realm_declare_intrinsic(node_install_protos);
}

/* §4.4's INTERFACE PROTOTYPE OBJECTS, FOR ONE REALM — Node, CharacterData, Text and Comment. */
void node_install_protos(JSContext *ctx)
{
    JSValue node_p, cd, prev;

    DCHECK(g_protos_ready, "a realm asked for Node.prototype before node_init declared the interface");
    prev = JS_GetClassProto(ctx, g_node_class);
    DCHECK(JS_IsNull(prev), "node_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);

    /* Node.prototype. §4.4 `interface Node : EventTarget`: every node is one, and only the global was — so
       `el.addEventListener(...)` was "not a function" on every element a page wired up, which is where
       testharness.js stopped on eight documents. It belongs here because it is a BASE member, and here it is
       one function shared by every node rather than a fresh closure on each. */
    /* §4.4: `Node : EventTarget`. The three members come down the chain from EventTarget.prototype, which is
       where §2.7 declares them — installing copies onto Node.prototype said they were declared here. Web IDL
       §3.7.3 Interface prototype object builds the object OVER that parent, so the chain is established here
       rather than patched on afterwards. */
    node_p = event_target_derived_proto(ctx);
    idl_interface_tag(ctx, node_p, "Node");
    node_install_walkers(ctx, node_p);
    JS_SetPropertyFunctionList(ctx, node_p, js_node_base,
                               (int)(sizeof(js_node_base) / sizeof(js_node_base[0])));
    JS_SetPropertyFunctionList(ctx, node_p, js_node_consts,
                               (int)(sizeof(js_node_consts) / sizeof(js_node_consts[0])));
    idl_install_accessor(ctx, node_p, "nodeValue", js_cd_get_data, 1, g_id_nodevalue);
    idl_install_accessor_step(ctx, node_p, "textContent", g_id_textcontent_get, g_id_textcontent);
    idl_install_method(ctx, node_p, "lookupPrefix", g_id_lookup_prefix);
    idl_install_method(ctx, node_p, "lookupNamespaceURI", g_id_lookup_ns);
    idl_install_method(ctx, node_p, "isDefaultNamespace", g_id_default_ns);
    idl_install_method(ctx, node_p, "getRootNode", g_id_root);
    JS_SetClassProto(ctx, g_node_class, JS_DupValue(ctx, node_p));

    /* CharacterData.prototype — §4.10, `interface CharacterData : Node`, so it INHERITS from Node.prototype
       rather than repeating its members. */
    cd = JS_NewObjectProto(ctx, node_p);
    CHECK(!JS_IsException(cd), "CharacterData.prototype could not be allocated");
    idl_interface_tag(ctx, cd, "CharacterData");
    JS_SetPropertyFunctionList(ctx, cd, js_chardata_base,
                               (int)(sizeof(js_chardata_base) / sizeof(js_chardata_base[0])));
    idl_install_accessor(ctx, cd, "data", js_cd_get_data, 0, g_id_data);
    {
        /* THE `length` COLUMN IS GONE FROM THIS TABLE because Web IDL §3.7.7 Operations computes it — it read
           `{ 2, 1, 2, 2, 3 }`, which is each member's DECLARED ARITY and is what §3.7.7 explicitly is not:
           §4.10's `replaceData(unsigned long offset, unsigned long count, DOMString data)` has three REQUIRED
           arguments so the two numbers happen to agree here, and they stop agreeing the moment one of these
           members gains an optional argument. See idl_member_length_of. */
        static const char *const CD_NAMES[5] = { "substringData", "appendData", "insertData",
                                                 "deleteData", "replaceData" };
        int k;
        for (k = 0; k < 5; k++)
            idl_install_method(ctx, cd, CD_NAMES[k], g_id_cd[k]);
    }
    /* DOM §4.2.8 Mixin ChildNode: `CharacterData includes ChildNode` — `textNode.remove()` is real, and a page
       that tears down text with it had nothing. ParentNode is NOT included: character data has no children.
       (This cited "§4.10", which is Interface CharacterData and states no `includes` at all — every one of
       them is written in the MIXIN's own section, which is why the number moves and the title does not.) */
    node_install_child_mixin(ctx, cd);
    /* DOM §4.2.7 Mixin NonDocumentTypeChildNode: `CharacterData includes NonDocumentTypeChildNode` — a Text
       node's `nextElementSibling` is real, which is how a page walks off a text node to the element after it,
       and how lit-html finds a part's node from a marker comment. */
    node_install_non_doctype_child_mixin(ctx, cd);
    {
        JSValue text_proto = JS_NewObjectProto(ctx, cd);
        JSValue comment_proto = JS_NewObjectProto(ctx, cd);
        CHECK(!JS_IsException(text_proto) && !JS_IsException(comment_proto),
              "a CharacterData-derived prototype could not be allocated");
        idl_interface_tag(ctx, text_proto, "Text");
        /* §4.11's OWN two members. `Text : CharacterData` adds exactly these, and a Text prototype with
           nothing on it is what made `splitText` absent while `nodeType === 3` answered — the shape this
           engine treats as worst, because nothing throws until the page's own call does. */
        idl_install_method(ctx, text_proto, "splitText", g_id_split_text);
        idl_install_accessor(ctx, text_proto, "wholeText", js_text_whole, 0, -1);
        /* §4.2.9: `Text includes Slottable`, and the mixin is one member — `assignedSlot`. A Text node is a
           slottable exactly like an element, which is what makes an unnamed `<slot>` collect a host's text. */
        slot_install_slottable_mixin(ctx, text_proto);
        idl_interface_tag(ctx, comment_proto, "Comment");
        {
            /* §4.12 `interface CDATASection : Text` — no members of its own. §4.13
               `interface ProcessingInstruction : CharacterData` adds exactly `target`. */
            JSValue cdata_proto = JS_NewObjectProto(ctx, text_proto);
            JSValue pi_proto = JS_NewObjectProto(ctx, cd);
            CHECK(!JS_IsException(cdata_proto) && !JS_IsException(pi_proto),
                  "a CharacterData-derived prototype could not be allocated");
            idl_interface_tag(ctx, cdata_proto, "CDATASection");
            idl_interface_tag(ctx, pi_proto, "ProcessingInstruction");
            idl_install_accessor(ctx, pi_proto, "target", js_pi_target, 0, -1);
            JS_SetClassProto(ctx, g_cdata_class, cdata_proto);
            JS_SetClassProto(ctx, g_pi_class, pi_proto);
        }
        JS_SetClassProto(ctx, g_text_class, text_proto);
        JS_SetClassProto(ctx, g_comment_class, comment_proto);
    }
    JS_SetClassProto(ctx, g_chardata_class, cd);
    JS_FreeValue(ctx, node_p);
}

JSValue node_chardata_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_chardata_class);
    DCHECK(!JS_IsNull(proto), "CharacterData.prototype was asked for in a realm that never ran its install");
    return proto;   /* OWNED */
}

/* THE INTERFACE OBJECTS — `Node`, `CharacterData`, `Text`, `Comment` as globals with their prototypes. Without
   them `Node.ELEMENT_NODE` (which is how a page spells a nodeType test) and `x instanceof Text` had nothing to
   read, and the platform-names list made reading the global a THROW rather than app state, so a page that
   feature-tested this way stopped there. §4.4's constants live on the interface object as well as the
   prototype, which is why one table installs both.
   None of the four is CONSTRUCTIBLE — the DOM gives Node and CharacterData no constructor at all, and Text and
   Comment take a `DOMString data` this engine creates through document.createTextNode. Calling one is a
   TypeError, which is what an interface object with no [Constructor] does. */
static JSValue js_node_iface_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv)
{
    (void)nt; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

/* §4.4 an interface object INHERITS from its parent interface's object, which is what makes `Text.ELEMENT_NODE`
   read. Node's is installed first and read back here rather than cached, so there is one of it. */
static JSValue node_interface_object(JSContext *ctx, JSValueConst global)
{
    JSValue n = JS_GetPropertyStr(ctx, (JSValue)global, "Node");
    DCHECK(JS_IsObject(n), "a derived DOM interface object was installed before `Node` was");
    return n;
}

void node_install_interface(JSContext *ctx, JSValueConst global, const char *name, JSValueConst proto)
{
    node_install_interface_ctor(ctx, global, name, proto,
                                JS_NewCFunction2(ctx, js_node_iface_ctor, name, 0, JS_CFUNC_constructor, 0));
}

void node_install_interface_ctor(JSContext *ctx, JSValueConst global, const char *name, JSValueConst proto,
                                 JSValue ctor)
{
    DCHECK(JS_IsObject(proto), "a DOM interface object was installed with no prototype behind it");
    CHECK(!JS_IsException(ctor), "a DOM interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);   /* .prototype and .constructor, both directions, one call */
    /* Web IDL §3.7.1 Interface object: an interface object whose interface INHERITS another has THAT
       interface's object as its [[Prototype]], which is what makes `Text.ELEMENT_NODE` read §4.4's constants
       off Node's object rather than each object repeating the table. §4.4's Node is the base and keeps
       %Function.prototype%.
       WHICH CONSTANTS AN OBJECT CARRIES IS NOT DECIDED HERE ANY MORE. §3.7.5 Constants puts them on the
       interface object as well as the prototype, and the install stood in this shared helper under the
       run-time `is_node` pointer comparison below — so the interface the eighteen §4.4 constants landed on was
       decided at run time and the Web IDL gap audit could attribute none of them. It is stated by the caller
       that names the interface (node_install_interfaces), where the object is Node's and nothing else's. */
    {
        JSValue base_proto = node_proto(ctx);
        bool is_node = JS_VALUE_GET_PTR(proto) == JS_VALUE_GET_PTR(base_proto);
        JS_FreeValue(ctx, base_proto);
        if (!is_node) {
            JSValue base = node_interface_object(ctx, global);   /* OWNED by this read; JS_SetPrototype borrows */
            JS_SetPrototype(ctx, ctor, base);
            JS_FreeValue(ctx, base);
        }
    }
    JS_SetPropertyStr(ctx, (JSValue)global, name, ctor);
}

void node_install_interfaces(JSContext *ctx, JSValueConst global)
{
    DCHECK(g_protos_ready, "the DOM interface objects were installed before node_init built their prototypes");
    {
        JSValue np = node_proto(ctx), cdp = node_chardata_proto(ctx);
        JSValue tp = node_type_proto(ctx, LXB_DOM_NODE_TYPE_TEXT);
        JSValue cmp = node_type_proto(ctx, LXB_DOM_NODE_TYPE_COMMENT);
        /* §4.4's constants, on the ONE interface object Web IDL §3.7.5 Constants also puts them on — built
           here by the same §3.7.1 call every other interface object in this engine is built by, so that which
           interface they belong to is the argument beside them rather than a run-time comparison inside a
           shared helper. media_element.c states §4.8.11's the same way. */
        JSValue node_ctor = idl_interface_object(ctx, "Node", np);
        JS_SetPropertyFunctionList(ctx, node_ctor, js_node_consts,
                                   (int)(sizeof(js_node_consts) / sizeof(js_node_consts[0])));
        node_install_interface_ctor(ctx, global, "Node", np, node_ctor);
        node_install_interface(ctx, global, "CharacterData", cdp);
        /* §4.11 AND §4.14 DECLARE CONSTRUCTORS, so their interface objects are built with §3.7.1's
           [[Construct]] steps rather than with the shared throw — see js_cd_ctor. Everything else about the
           object is unchanged: node_install_interface_ctor is the same call the throwing ones reach, so
           `Text.ELEMENT_NODE` still reads §4.4's constants off Node's interface object exactly as before.
           CharacterData, CDATASection and the rest keep the throw because their IDL declares no constructor,
           which is what makes `node_install_interface` the right call for them and not a default. */
        DCHECK(g_id_text_ctor >= 0 && g_id_comment_ctor >= 0,
               "Text and Comment were installed before node_init declared §4.11's and §4.14's constructors");
        node_install_interface_ctor(ctx, global, "Text", tp,
                                    idl_step_constructor(ctx, "Text", g_id_text_ctor));
        JSValue csp = node_type_proto(ctx, LXB_DOM_NODE_TYPE_CDATA_SECTION);
        JSValue pip = node_type_proto(ctx, LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION);
        node_install_interface_ctor(ctx, global, "Comment", cmp,
                                    idl_step_constructor(ctx, "Comment", g_id_comment_ctor));
        node_install_interface(ctx, global, "CDATASection", csp);
        /* DOM §4.12 Interface CDATASection declares no constructor, so its throw is the SPEC. DOM §4.13
           Interface ProcessingInstruction's is NOT — a NAMED RESIDUAL, and the code here is right for what it
           does rather than unfinished.
           WHAT IS NOT COVERED: `new ProcessingInstruction(target, data)`, whose DOM §4.13 steps are "Set
           this's node document to current global object's associated Document" and then "Initialize this with
           target and data".
           WHAT THE NEXT DIFF BUILDS: that second step is a NAMED algorithm DOM §4.13 shares between this
           constructor and DOM §4.5's createProcessingInstruction — "To initialize a ProcessingInstruction node
           pi, with target and data" — and this engine has it INLINE in document.c's js_doc_create_xml_node
           under `magic == 1`, so the constructor cannot be written without lifting it out to the one place
           both reach. A second copy here is what CLAUDE.md forbids, and it would be a copy that is ALSO
           missing the algorithm's last step ("Update attributes from data given pi"), which the inline one
           does not run either — so the lift has to carry that step, not just move what is there.
           HOW ITS ABSENCE SHOWS: the audit's own `interfaces a page cannot new` category still names
           ProcessingInstruction, and a page writing `new ProcessingInstruction("xml-stylesheet", "href='x'")`
           gets a TypeError where every browser gives it a node. */
        node_install_interface(ctx, global, "ProcessingInstruction", pip);
        JS_FreeValue(ctx, np); JS_FreeValue(ctx, cdp);
        JS_FreeValue(ctx, tp); JS_FreeValue(ctx, cmp);
        JS_FreeValue(ctx, csp); JS_FreeValue(ctx, pip);
    }
}


void node_free(JSRuntime *rt)
{
    int i;
    /* THE THREE SLOTS THIS FILE CLAIMED IN OTHER COMPONENTS, GIVEN BACK. Each is a C function pointer that
       another component holds and that names code in THIS one, so a release that kept it would leave the
       solver and the events layer calling into a DOM group the cascade above has already torn down — the
       defect core/agent_state.h found in idb_transaction, three more times. The claimant releases, the
       receiver asserts; core/platform.c's reverse-declaration order is what runs `element` before
       `event_target`, and solver_agent_free runs after the whole platform. */
    dom_cow_set_tree_hook(NULL);
    event_target_set_tree(NULL);
    engine_set_wrap_stats(NULL);
    DCHECK(g_element_resolver == NULL,
           "the element-interface resolver was still registered when the node layer was released — "
           "core/html/html_element.c claimed it and gives it back at html_element_free, which the DOM group's "
           "own cascade runs first");
    g_tree_hook_n = 0;
    /* BEFORE the walk, because the walk is what would do the damage: every entry below holds a reference
       belonging to the runtime that declared this layer, and freeing them through a different one is a heap
       corruption the walk cannot report afterwards. `g_agent_rt == NULL` is the idempotent second call
       element_free's own comment records. */
    DCHECK(g_agent_rt == NULL || g_agent_rt == rt,
           "the node layer is being released by a runtime that is not the one that declared it — the identity "
           "map holds a reference per node belonging to that other runtime, and this release would free them "
           "through this one");
    g_agent_rt = NULL;
    for (i = 0; i < g_wrap_cap; i++)
        if (g_wraps[i].n)
            JS_FreeValueRT(rt, g_wraps[i].obj);
    free(g_wraps);
    g_wraps = NULL; g_wrap_n = g_wrap_cap = 0;
    /* The prototypes are the REALMS' — each is released with its context. What the AGENT holds is the table of
       CLASS IDS, which is not a reference to anything. */
    g_protos_ready = 0;
}
