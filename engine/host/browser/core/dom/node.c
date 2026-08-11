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
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"
#include "core/dom/slot.h"
#include "core/dom/collections.h"
#include "core/dom/range.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/dom/document.h"
#include "solver/engine.h"
#include "core/events/event.h"
#include "core/events/event_target.h"

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
   THE MAP HOLDS ITS WRAPPERS WEAKLY, which is the whole reason it can stay small. It used to keep a
   JS_DupValue of every wrapper and never remove anything — "the map only ever grows" was written here as if it
   were a property rather than a defect. It meant every node ever wrapped was pinned for the runtime's life, so
   a page re-run by two thousand exploration flows accumulated two thousand copies of its DOM, and one doubling
   of a table that size is a single calloc-plus-rehash that measured FIVE SECONDS inside one createElement —
   with no suspend point in it, because it is one C call.
   It was also a correctness trap. A Lexbor node that is destroyed left its entry behind, and a pool allocator
   reuses addresses: the next node at that address found the DEAD node's wrapper and got another node's
   identity and another node's prototype.
   So the entry is weak and the finalizer removes it — the ordinary DOM wrapper-map design. Identity holds for
   exactly as long as someone holds the wrapper, which is the only span in which identity is observable. */
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
void node_set_element_resolver(JSValue (*fn)(JSContext *ctx, lxb_dom_element_t *el)) { g_element_resolver = fn; }

/* THE TREE HOOK IS INSTALLED INTO THE CHOKEPOINT, not called from each mutation site, and the difference is a
   bug this file had: `element_on_inserted` was invoked from appendChild and NOWHERE else, so an element that
   entered the tree by insertBefore, by replaceChild, or by an innerHTML parse was never prepared and never
   upgraded. Eleven sites mutate the tree; one remembered. The chokepoint is the one place that cannot be
   forgotten, which is the same argument that put capture there. */
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
            JSValue first = event_path_first(ctx, ev);
            lxb_dom_node_t *fn = node_of(first);
            bool inside = fn != NULL && node_root(fn) == n;

            JS_FreeValue(ctx, first);
            if (inside)
                return JS_NULL;
        }
        return node_wrap(ctx, lxb_dom_interface_node(shadow_root_host(n)));
    }
    return node_wrap(ctx, n->parent);
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

static const EventTargetTree NODE_EVENT_TREE = { node_get_parent, node_default_passive_target };

/* §4.2.3's INSERTION AND REMOVING STEPS ARE A LIST, not a slot. The standard's own `remove` runs several
   independent things in order — the live-range pre-remove steps, then §6.1's NodeIterator pre-remove steps,
   then each interested party's removing steps — and this was ONE function pointer, so the second component to
   need it would have had to be called from inside the first. That is the hand-copied list CLAUDE.md warns
   about, one layer down: a component added without editing whoever happens to hold the slot is a component
   whose steps silently never run.
   The ORDER is the registration order, which is the standard's order because the components register in it. */
#define NODE_TREE_HOOKS_MAX 6
static NodeTreeHook g_tree_hooks[NODE_TREE_HOOKS_MAX];
static int g_tree_hook_n;

static void node_tree_hooks_run(JSContext *ctx, lxb_dom_node_t *n, lxb_dom_node_t *parent, int phase)
{
    int i;
    DCHECK(phase >= NODE_TREE_REMOVED && phase <= NODE_TREE_INSERTED,
           "the tree-steps list was run at a phase §4.2.3 does not have");
    for (i = 0; i < g_tree_hook_n; i++)
        g_tree_hooks[i](ctx, n, parent, phase);
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
void node_insert_at(lxb_dom_node_t *parent, lxb_dom_node_t *node, lxb_dom_node_t *ref)
{
    if (node_is_document_fragment(node)) {
        lxb_dom_node_t *c, *next;
        for (c = node->first_child; c; c = next) {
            next = c->next;
            dom_cow_remove_child(c);
            if (ref) dom_cow_insert_before(ref, c);
            else     dom_cow_append_child(parent, c);
        }
        return;
    }
    /* §4.2.3 PRE-INSERT STEP 2: a reference child that IS the node becomes the node's next sibling, decided
       before the node leaves its place. */
    if (ref == node) ref = node->next;
    /* §4.5 "adopt" STEP 2, which §4.2.3's insert reaches at its step 6.1: "if node's parent is non-null, remove
       node". Lexbor's insert does NOT unlink — it writes the new parent's links over the old ones and leaves
       the old parent still naming the node as its child, so a MOVE (`doc.appendChild(p)` for a `p` that has a
       parent) built a tree with two parents and a sibling chain that loops. Every walk in this engine then runs
       forever, which is how it surfaced: §6.2's TreeWalker hung where the corpus regrafts a subtree.
       It goes through the capturing chokepoint rather than a raw detach because it IS a removal — the per-flow
       delta has to see it, and so do §4.2.3's removing steps and §6.1's pre-remove steps. */
    if (node->parent) dom_cow_remove_child(node);
    if (ref) dom_cow_insert_before(ref, node);
    else     dom_cow_append_child(parent, node);
}

/* §4.5 appendChild / removeChild. DECLARED members, like every other one — they were raw JS_CFUNC_DEF entries,
   which is a shape that cannot park at all and, more to the point here, does not pass through the machine every
   declared member converges on. `Node node` is an interface type, so the IDL converts nothing; the declaration
   is what puts them on that path. magic 0 = appendChild, 1 = removeChild. */
static JSValue js_node_child_op(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_node_t *n = node_of(this_val), *child;

    if (!n || argc < 1) return JS_UNDEFINED;
    child = node_of(argv[0]);
    if (!child)
        return JS_ThrowTypeError(ctx, magic ? "removeChild requires a Node" : "appendChild requires a Node");
    if (magic) {
        if (child->parent != n)
            return JS_ThrowDOMException(ctx, "NotFoundError", "the node to remove is not a child of this node");
        dom_cow_remove_child(child);
    } else {
        /* §4.2.3 "pre-insert": the hierarchy check that keeps the tree a tree, which insertBefore already made
           and this one did not — `a.appendChild(a)` and `child.appendChild(ancestor)` both built a CYCLE, and
           every walk in this file loops forever on one. */
        if (node_is_inclusive_ancestor(child, n))
            return JS_ThrowDOMException(ctx, "HierarchyRequestError",
                                        "a node cannot be inserted into its own descendant");
        node_insert_at(n, child, NULL);
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
    return text ? lxb_dom_interface_node(text) : NULL;
}

static JSValue js_node_mixin(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_node_t *n = node_of(this_val), *parent, *ref, *added;
    int i;

    if (!n) return JS_UNDEFINED;
    parent = n->parent;
    /* §4.2.7: a node with no parent has nothing to be removed from or inserted beside — and that is a no-op,
       not an error, which is what lets a page call `el.remove()` twice. */
    if (magic <= 3 && !parent) return JS_UNDEFINED;

    if (magic == 6) {                       /* replaceChildren: empty first, then append */
        lxb_dom_node_t *c = n->first_child, *next;
        for (; c; c = next) { next = c->next; dom_cow_remove_child(c); }
    }
    /* `before`/`after` insert beside THIS node; `replaceWith` does that and then removes it. The reference
       child is fixed BEFORE anything is inserted, because inserting moves `n->next`. */
    ref = (magic == 1 || magic == 3) ? n : (magic == 2 ? n->next : NULL);
    for (i = 0; i < argc; i++) {
        added = node_from_arg(ctx, n, argv[i]);
        if (!added) return JS_EXCEPTION;
        if (added == n) continue;           /* inserting a node beside itself is its own removal first */
        switch (magic) {
        case 1: case 3: node_insert_at(parent, added, ref); break;
        case 2: node_insert_at(parent, added, ref); break;
        case 4: case 6: node_insert_at(n, added, NULL); break;
        case 5: node_insert_at(n, added, n->first_child);
                break;
        default: DFAIL("a ChildNode/ParentNode member ran with an unknown magic"); break;
        }
    }
    if (magic == 0 || magic == 3) dom_cow_remove_child(n);
    return JS_UNDEFINED;
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
    return JS_UNDEFINED;
}

/* §4.10's five members. magic: 0 substringData, 1 appendData, 2 insertData, 3 deleteData, 4 replaceData.
   Every argument arrives CONVERTED — `unsigned long` and `DOMString` are the declaration's work — so nothing
   here runs the page's code and the body is ordinary C. */
static JSValue js_cd_op(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    lxb_dom_character_data_t *cd;
    uint32_t offset = 0, count = 0;
    const char *data = NULL;
    size_t data_len = 0;
    JSValue r;

    if (!n || !node_is_chardata(n))
        return JS_ThrowTypeError(ctx, "a CharacterData method ran on a node that holds no character data");
    cd = lxb_dom_interface_character_data(n);
    switch (magic) {
    case 0: case 3: case 4:
        JS_ToUint32(ctx, &offset, argv[0]);
        JS_ToUint32(ctx, &count, argv[1]);
        break;
    case 2:
        JS_ToUint32(ctx, &offset, argv[0]);
        break;
    default:
        DCHECK(magic == 1, "a CharacterData method was declared with a magic §4.10 does not name");
        break;
    }
    if (magic == 0) {                                               /* substringData */
        uint32_t length = cd_units(cd->data.data, cd->data.length);
        size_t a, b;

        if (offset > length)                                        /* step 2 */
            return JS_ThrowDOMException(ctx, "IndexSizeError",
                                        "the offset is past the end of the character data");
        if ((uint64_t)offset + count > length)                      /* step 3 */
            count = length - offset;
        a = cd_byte_of(cd->data.data, cd->data.length, offset);
        b = cd_byte_of(cd->data.data, cd->data.length, offset + count);
        return JS_NewStringLen(ctx, (const char *)cd->data.data + a, b - a);
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
        if (!data) return JS_EXCEPTION;
        if (concolic_is(v)) data_len = strlen(data);
    } else {
        data = "";
        data_len = 0;
    }
    if (magic == 1)                                                 /* appendData: offset is the length */
        offset = cd_units(cd->data.data, cd->data.length);
    if (magic == 1 || magic == 2)                                   /* append/insert delete nothing */
        count = 0;
    r = node_cd_replace_data(ctx, n, offset, count, data, data_len);
    if (magic != 3 && !concolic_is((magic == 1) ? argv[0] : (magic == 2 ? argv[1] : argv[2])))
        JS_FreeCString(ctx, data);
    return r;
}

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

/* §4.11 `[NewObject] Text splitText(unsigned long offset)`. The offset arrives CONVERTED. */
static JSValue js_text_split(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_node_t *n = node_of(this_val), *split;
    uint32_t offset = 0;

    (void)magic;
    if (!n || n->type != LXB_DOM_NODE_TYPE_TEXT)
        return JS_ThrowTypeError(ctx, "splitText ran on a node that is not a Text node");
    if (argc > 0 && JS_ToUint32(ctx, &offset, argv[0]) < 0) return JS_EXCEPTION;
    split = node_split_text(ctx, n, offset);
    return split ? node_wrap(ctx, split) : JS_EXCEPTION;
}

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
   spec's tree-order algorithms need, and having it once is what keeps them from each growing a walker. */
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
        /* §4.4: THE NODE DOCUMENT's document base URL, serialized — the node's own document and not the realm's
           active one, which stopped being the same question the moment §4.5.1's factories could build a second
           Document. `foreignDoc.createElement("a").baseURI` is `about:blank`, not the page's address. No <base>
           support yet, so it is that document's address — asked of the component that owns it rather than
           re-derived here, because two answers to "what is this document's URL" is how they drift apart. */
        DCHECK(magic == 3, "a Node fact was declared with a magic this table does not name");
        return JS_NewString(ctx, document_url_of(n->owner_document));
    }
}

/* §4.4 hasChildNodes / isSameNode / contains — three one-line predicates that a page uses constantly and that
   each had to be re-implemented by the page when they were absent. */
static JSValue js_node_predicates(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    lxb_dom_node_t *other = argc > 0 ? node_of(argv[0]) : NULL;

    if (!n) return JS_FALSE;
    switch (magic) {
    case 0: return JS_NewBool(ctx, n->first_child != NULL);
    case 1: return JS_NewBool(ctx, other == n);                      /* isSameNode: `Node?`, so null is false */
    default:
        DCHECK(magic == 2, "a Node predicate was declared with a magic this table does not name");
        return JS_NewBool(ctx, other && node_is_inclusive_ancestor(n, other));   /* contains: INCLUSIVE */
    }
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
        s->b = argc > 0 ? node_of(argv[0]) : NULL;
        s->ra = s->a; s->rb = s->b;
        /* `Node? otherNode`: null is never equal. */
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
        s->b = argc > 0 ? node_of(argv[0]) : NULL;
        if (!s->a || !s->b) {
            JS_ThrowTypeError(ctx, "compareDocumentPosition requires a Node");
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

/* §4.4 cloneNode — A MACHINE, because a deep clone is a walk of the page's subtree and a COPY of it. Lexbor's
   `lxb_dom_node_clone(n, true)` runs that walk to completion inside one opcode; it is iterative rather than
   recursive, so it never blew the C stack, and that is exactly the kind of thing that hides how long it holds
   the scheduler. `document.body.cloneNode(true)` is one opcode for the whole document.
   LEXBOR STILL OWNS WHAT A COPY OF ONE NODE IS — `lxb_dom_node_clone(n, false)` is the document's own
   clone_interface, so attributes, namespaces and per-interface fields are copied by the code the tree builder
   uses. What moves here is the WALK, node by node, exactly as §8.4's serialiser did.
   THE COPY IS A PRIVATE TREE. It is built by inserting into itself and it is in no document until the page
   inserts it, so those inserts are declared private rather than captured — capturing them would put the whole
   copy in the running flow's delta, when the delta exists to hold shared state the flow touched. */
/* A LEVEL of the walk — see the `<template>` case below. */
typedef struct { lxb_dom_node_t *src, *dst, *root, *croot, *cnode; } CloneFrame;

typedef struct {
    lxb_dom_node_t *src;     /* the cursor in the original */
    lxb_dom_node_t *root;    /* what bounds the CURRENT level of the walk */
    lxb_dom_node_t *dst;     /* the copy the next child is inserted into */
    lxb_dom_node_t *copy;    /* the copy's root — the answer */
    /* THE PRIVATE-TREE DECLARATION FOR THE CURRENT LEVEL, which is not always `copy`. A `<template>`'s content
       is a SEPARATE tree — reached through the element's `content` field, not through child links — so once the
       walk descends into it, the tree being built into is that fragment. It is private for the same reason the
       copy is: clone_interface made it a moment ago and nothing has ever seen it. */
    lxb_dom_node_t *croot;
    lxb_dom_node_t *cnode;   /* the copy of `src` — what its own children get inserted under */
    CloneFrame *stack;       /* the template levels above this one */
    int sp, scap;
} NodeCloneState;

static void node_clone_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    /* The cursors own nothing — every one is a Lexbor node: the originals belong to the document, and the copy
       belongs to the document's memory pool from the moment clone_interface makes it. The template STACK is
       plain storage, and a forked arm must not share it: each unwinds its own levels. */
    NodeCloneState *s = st;
    v->buf(ctx, (void **)&s->stack, sizeof(CloneFrame) * (size_t)s->scap);
}

static void node_clone_release(JSContext *ctx, void *st)
{
    NodeCloneState *s = st;
    (void)ctx;
    free(s->stack);
    s->stack = NULL;
}

static void clone_push(NodeCloneState *s)
{
    if (s->sp == s->scap) {
        int want = s->scap ? s->scap * 2 : 4;
        CloneFrame *n = realloc(s->stack, sizeof(CloneFrame) * (size_t)want);
        CHECK(n != NULL, "cloneNode could not grow its template stack");
        s->stack = n;
        s->scap = want;
    }
    s->stack[s->sp].src = s->src;
    s->stack[s->sp].dst = s->dst;
    s->stack[s->sp].root = s->root;
    s->stack[s->sp].croot = s->croot;
    s->stack[s->sp].cnode = s->cnode;
    s->sp++;
}

/* A `<template>`'s content fragment, or NULL for anything else. The children of a template are NOT under it —
   they hang off a separate fragment — so a walk that follows first_child copies the template and none of its
   markup, which is exactly what this did before the case was added: `<template><b>tc</b></template>` cloned to
   `<template></template>` and the page got an empty one with no error anywhere. §4.4 states the contents are
   cloned too. A shallow clone of a template already HAS its own empty fragment, because lexbor's
   clone_interface builds the template interface, so both sides have somewhere to go. */
static lxb_dom_node_t *clone_template_content(lxb_dom_node_t *n)
{
    lxb_html_template_element_t *t;
    if (n->type != LXB_DOM_NODE_TYPE_ELEMENT || !lxb_html_tree_node_is(n, LXB_TAG_TEMPLATE)) return NULL;
    t = lxb_html_interface_template(n);
    return t->content ? &t->content->node : NULL;
}

/* THE THREE STAGES, and why the template case forces three rather than two. A `<template>` can have BOTH: the
   parser puts markup in its content fragment, and `t.appendChild(x)` appends to the ELEMENT — §4.10 is explicit
   that only the parser and `t.content` reach the fragment. So a template's copy has two child lists to fill, and
   coming back from the content walk must resume AFTER the content check and BEFORE the ordinary-children one, or
   the template descends into its own content again and the walk never ends.
   The invariant every stage keeps: `src` is the original being handled, `cnode` is its copy, and `dst` is the
   copy of the node the NEXT child goes under. */
/* WHERE THIS MACHINE RESTS, AS THE STANDARDS NUMBER IT. cloneNode is two steps over `clone a node`, whose
   own steps 2, 3 and 5 are exactly the three the walk cycles through per node: clone a single node and append
   it (2 and 4), run the cloning steps other specifications define — which for a template element is HTML
   §4.12.3's, the reason this level exists at all — and then recurse over the children (5). Each is its own
   stage because each is per node and the walk is the page's subtree; none of them can reach the page's code,
   which is why the entry stage may name cloneNode's steps 1-2 together with `clone a node`'s 1-2. */
#define NODE_CLONE_STAGES(X) \
    X(CLONE_ROOT,     "DOM §4.4 cloneNode steps 1-2 → clone a node steps 1-2 (clone a single node: the root of " \
                      "the copy)") \
    X(CLONE_COPY,     "DOM §4.4 clone a node steps 2 and 4 (clone a single node; append copy to parent), one " \
                      "descendant per step") \
    X(CLONE_TEMPLATE, "DOM §4.4 clone a node step 3 (HTML §4.12.3 the template element's cloning steps)") \
    X(CLONE_CHILDREN, "DOM §4.4 clone a node step 5 (for each child of node's children, in tree order)")
enum { IDL_STEP_STAGE_BASE(NODE_CLONE_STAGES) NODE_CLONE_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const NODE_CLONE_STEPS[] = { NODE_CLONE_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_node_clone(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                         JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    NodeCloneState *s = st;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);

    if (hdr->stage == CLONE_ROOT) {
        lxb_dom_node_t *n = node_of(hdr->this_val);
        /* `optional boolean subtree = false` — the declaration converted it, so this is a real boolean. */
        bool deep = argc > 0 && JS_ToBool(ctx, argv[0]);

        if (!n) { *presult = JS_UNDEFINED; return JS_STEP_DONE; }
        if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT) {
            JS_ThrowDOMException(ctx, "NotSupportedError", "cloning a Document is not supported");
            return JS_STEP_ABRUPT;
        }
        s->copy = lxb_dom_node_clone(n, false);
        dom_cow_note_created(s->copy);   /* the clone ROOT only — its descendants are reachable only through it */
        CHECK(s->copy != NULL, "cloneNode: the Lexbor node copy failed — returning nothing would hand the page a "
                               "null it has no way to distinguish from a node it never asked for");
        if (!deep) { *presult = node_wrap(ctx, s->copy); return JS_STEP_DONE; }
        s->root = s->src = n;
        s->cnode = s->dst = s->croot = s->copy;   /* the root is already copied; the walk starts at its children */
        hdr->stage = CLONE_TEMPLATE;
        return JS_STEP_YIELD;
    }

    if (hdr->stage == CLONE_COPY) {
        /* ONE NODE PER STEP: copy it and hang it under the copy of its parent. */
        s->cnode = lxb_dom_node_clone(s->src, false);
        CHECK(s->cnode != NULL, "cloneNode: a descendant's Lexbor copy failed — the page would get a subtree "
                                "with a hole in it and no way to tell");
        dom_cow_insert_private(s->croot, s->dst, s->cnode);
        hdr->stage = CLONE_TEMPLATE;
        return JS_STEP_YIELD;
    }

    if (hdr->stage == CLONE_TEMPLATE) {
        lxb_dom_node_t *content = clone_template_content(s->src);
        hdr->stage = CLONE_CHILDREN;
        if (content && content->first_child) {
            /* Leave this tree for the template's, on both sides at once. The frame is everything to come back
               to; the copy's fragment is its own private tree, made by clone_interface a moment ago. */
            clone_push(s);
            s->root = content;
            s->src = content->first_child;
            s->dst = s->croot = clone_template_content(s->cnode);
            DCHECK(s->dst != NULL, "a cloned <template> has no content fragment to copy into — lexbor's "
                                   "clone_interface built something other than a template interface");
            hdr->stage = CLONE_COPY;
        }
        return JS_STEP_YIELD;
    }

    DCHECK(hdr->stage == CLONE_CHILDREN, "cloneNode resumed into a stage §4.4 does not have");
    if (s->src->first_child) {
        s->src = s->src->first_child;
        s->dst = s->cnode;
        hdr->stage = CLONE_COPY;
        return JS_STEP_YIELD;
    }
    for (;;) {
        /* ASCEND IN LOCKSTEP. The two trees have the same shape by construction, so one loop moves both — and
           it is bounded by the depth already walked, not by the page, which is why it is not its own stage. */
        while (!s->src->next && s->src != s->root) {
            s->src = s->src->parent;
            s->dst = s->dst->parent;
        }
        if (s->src != s->root) { s->src = s->src->next; hdr->stage = CLONE_COPY; return JS_STEP_YIELD; }
        if (s->sp == 0) { *presult = node_wrap(ctx, s->copy); return JS_STEP_DONE; }
        s->sp--;                        /* back to the template that owns this level, PAST its content check */
        s->src = s->stack[s->sp].src;
        s->dst = s->stack[s->sp].dst;
        s->root = s->stack[s->sp].root;
        s->croot = s->stack[s->sp].croot;
        s->cnode = s->stack[s->sp].cnode;
        return JS_STEP_YIELD;
    }
}

static const IdlStepDecl NODE_CLONE_STEP = {
    js_node_clone, sizeof(NodeCloneState), node_clone_visit, node_clone_release,
    "DOM §4.4 Node.cloneNode (over the `clone a node` concept)", NODE_CLONE_STEPS
};

/* §4.2.3 insertBefore / replaceChild — the two remaining mutating tree operations, through the same per-flow
   chokepoints appendChild and removeChild already use. magic 0 = insertBefore, 1 = replaceChild. */
static JSValue js_node_insert(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_node_t *parent = node_of(this_val);
    lxb_dom_node_t *node = argc > 0 ? node_of(argv[0]) : NULL;
    lxb_dom_node_t *child = argc > 1 ? node_of(argv[1]) : NULL;

    if (!parent) return JS_UNDEFINED;
    if (!node)
        return JS_ThrowTypeError(ctx, magic ? "replaceChild requires a Node" : "insertBefore requires a Node");
    /* §4.2.3 "pre-insert": the hierarchy check that keeps the tree a tree. A page that inserts an ancestor into
       its own descendant would build a CYCLE, and every walk in this file loops forever on one. */
    if (node_is_inclusive_ancestor(node, parent))
        return JS_ThrowDOMException(ctx, "HierarchyRequestError",
                                    "a node cannot be inserted into its own descendant");
    if (magic == 0 && !child) {
        node_insert_at(parent, node, NULL);       /* insertBefore(n, null) IS append */
        return JS_DupValue(ctx, argv[0]);
    }
    if (!child || child->parent != parent)
        return JS_ThrowDOMException(ctx, "NotFoundError",
                                    "the reference child is not a child of this node");
    node_insert_at(parent, node, child);
    if (magic == 1) {
        dom_cow_remove_child(child);
        return JS_DupValue(ctx, argv[1]);         /* replaceChild returns the node it REMOVED */
    }
    return JS_DupValue(ctx, argv[0]);
}

/* §4.4 lookupPrefix / lookupNamespaceURI / isDefaultNamespace — the three namespace lookups, one walk. Lexbor
   interns both halves, so this reads the element's own namespace and prefix rather than re-deriving them from
   an xmlns attribute the tree builder already consumed. magic 0 = lookupPrefix, 1 = lookupNamespaceURI,
   2 = isDefaultNamespace. */
static JSValue js_node_lookup_ns(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    const char *arg = NULL;
    JSValue r;

    if (!n) return magic == 2 ? JS_FALSE : JS_NULL;
    /* The argument is `DOMString?` and the declaration converted it, so this reads a string or the IDL null. */
    if (argc > 0 && JS_IsString(argv[0]))
        arg = JS_ToCString(ctx, argv[0]);

    /* §4.4: the lookup starts at the nearest ELEMENT — a Text node's namespace is its parent element's. */
    while (n && n->type != LXB_DOM_NODE_TYPE_ELEMENT)
        n = n->parent;
    if (!n) {
        if (arg) JS_FreeCString(ctx, arg);
        return magic == 2 ? JS_FALSE : JS_NULL;
    }
    {
        lxb_dom_element_t *el = lxb_dom_interface_element(n);
        size_t nl = 0, pl = 0;
        const lxb_char_t *ns = lxb_ns_by_id(n->owner_document->ns, n->ns, &nl);
        const lxb_char_t *px = lxb_dom_element_prefix(el, &pl);

        if (magic == 1) {   /* lookupNamespaceURI(prefix): the namespace of the element carrying that prefix */
            bool match = arg ? (pl == strlen(arg) && pl && memcmp(px, arg, pl) == 0)
                             : (pl == 0);   /* a null prefix asks for the DEFAULT namespace */
            r = (match && ns && nl) ? JS_NewStringLen(ctx, (const char *)ns, nl) : JS_NULL;
        } else if (magic == 0) {   /* lookupPrefix(namespace): the prefix this element uses for it */
            bool match = arg && ns && nl == strlen(arg) && memcmp(ns, arg, nl) == 0;
            r = (match && px && pl) ? JS_NewStringLen(ctx, (const char *)px, pl) : JS_NULL;
        } else {                   /* isDefaultNamespace(namespace) */
            DCHECK(magic == 2, "a namespace lookup was declared with a magic this table does not name");
            if (!arg || !*arg) r = JS_NewBool(ctx, !ns || nl == 0);
            else               r = JS_NewBool(ctx, pl == 0 && ns && nl == strlen(arg) &&
                                                   memcmp(ns, arg, nl) == 0);
        }
    }
    if (arg) JS_FreeCString(ctx, arg);
    return r;
}

/* §4.2.7 ChildNode — on Element, CharacterData and DocumentFragment, which is why it is a table the interfaces
   that DECLARE it ask for rather than members on Node.prototype: `document.remove()` is not a thing. */
/* `undefined before((Node or DOMString)... nodes)` and the three beside it — the union and the variadic tail
   are both DECLARED, so every argument is a Node or a real string by the time the body runs. */
static const IdlArgType MIXIN_NODES[1] = { IDL_STRING_UNLESS_IFACE };
typedef struct { const char *name; int len; int magic; } NodeMixinMember;
static const NodeMixinMember CHILD_NODE_MIXIN[] = {
    { "remove", 0, 0 }, { "before", 1, 1 }, { "after", 1, 2 }, { "replaceWith", 1, 3 },
};

/* §4.2.8 ParentNode's insertion half — on Element, Document and DocumentFragment. querySelector and children
   are the same mixin and already live on the interfaces that declare them. */
static const NodeMixinMember PARENT_NODE_MIXIN[] = {
    { "append", 1, 4 }, { "prepend", 1, 5 }, { "replaceChildren", 0, 6 },
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

static void node_byid_release(JSContext *ctx, void *st)
{
    NodeByIdState *s = st;
    (void)ctx;
    free(s->id);
    s->id = NULL;
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
        s->id = malloc(s->idlen + 1);
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
    js_node_get_element_by_id, sizeof(NodeByIdState), node_byid_visit, node_byid_release,
    "DOM §4.2.4 NonElementParentNode.getElementById", NODE_BYID_STEPS
};

/* §4.2.4, installed on the interfaces whose IDL INCLUDES it: Document and DocumentFragment. Not Element —
   `el.getElementById` is not a member of anything, which is why this is a mixin and not a Node member. */
static int g_id_by_id = -1;

void node_install_nonelement_parent_mixin(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_id_by_id >= 0, "the NonElementParentNode mixin was installed before it was declared");
    idl_install_method(ctx, proto, "getElementById", 1, g_id_by_id);
}

/* The members that WALK, installed through the declaration because they are machines. */
/* DECLARED ONCE PER AGENT, INSTALLED PER REALM. The IDL pool is SEALED after agent init precisely so that a
   per-realm mint is caught (idl_declared_before_seal), and a shared "install these members" helper that mints
   inline is exactly that bug: it works for the first realm and aborts on the second, naming the member. */
static int g_w_equal = -1, g_w_pos = -1, g_w_append = -1, g_w_remove = -1, g_w_insert = -1, g_w_replace = -1,
           g_w_normalize = -1, g_w_clone = -1;

static void node_declare_walkers(JSContext *ctx)
{
    static const IdlArgType ONE_ANY[1] = { IDL_ANY };   /* `Node? otherNode` — an interface type crosses as itself */
    static const IdlArgType TWO_ANY[2] = { IDL_ANY, IDL_ANY };
    static const IdlArgType ONE_BOOL[1] = { IDL_BOOLEAN };

    g_w_equal = idl_method_id_step(ctx, ONE_ANY, 1, NULL, 0, &NODE_EQUAL_STEP, 0);
    g_w_pos = idl_method_id_step(ctx, ONE_ANY, 1, NULL, 0, &NODE_POS_STEP, 0);
    /* §4.5's four MUTATING members. `Node node` / `Node? child` are interface types, so there is nothing to
       coerce — the declaration is not about coercion here, it is about being a declared member at all. */
    g_w_append = idl_method_id(ctx, ONE_ANY, 1, js_node_child_op, 0);
    g_w_remove = idl_method_id(ctx, ONE_ANY, 1, js_node_child_op, 1);
    g_w_insert = idl_method_id(ctx, TWO_ANY, 2, js_node_insert, 0);
    g_w_replace = idl_method_id(ctx, TWO_ANY, 2, js_node_insert, 1);
    /* `undefined normalize()` — no arguments to convert and still three loops' worth of the page's tree. */
    g_w_normalize = idl_method_id_step(ctx, NULL, 0, NULL, 0, &NODE_NORM_STEP, 0);
    /* `[CEReactions] Node cloneNode(optional boolean subtree = false)`. ToBoolean is total and runs none of the
       page's code, but the argument still crosses CONVERTED — a body handed the raw value would have to
       remember to coerce it, which is the per-body mistake one declaration exists to have none of. */
    g_w_clone = idl_method_id_step(ctx, ONE_BOOL, 1, NULL, 0, &NODE_CLONE_STEP, 0);
    idl_optional_from(0);   /* §4.4: `cloneNode(optional deep = false)` */
}

/* The members that WALK, installed through the declaration because they are machines. */
static void node_install_walkers(JSContext *ctx, JSValueConst proto)
{
    idl_install_method(ctx, proto, "isEqualNode", 1, g_w_equal);
    idl_install_method(ctx, proto, "compareDocumentPosition", 1, g_w_pos);
    idl_install_method(ctx, proto, "appendChild", 1, g_w_append);
    idl_install_method(ctx, proto, "removeChild", 1, g_w_remove);
    idl_install_method(ctx, proto, "insertBefore", 2, g_w_insert);
    idl_install_method(ctx, proto, "replaceChild", 2, g_w_replace);
    idl_install_method(ctx, proto, "normalize", 0, g_w_normalize);
    idl_install_method(ctx, proto, "cloneNode", 0, g_w_clone);
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
            g_mixin_id[tab[k].magic] = idl_method_id_ext(ctx, MIXIN_NODES, 1, /*variadic*/ true, node_class_id(),
                                                         js_node_mixin, tab[k].magic);
    }
    g_mixin_declared = 1;
}

static void mixin_install(JSContext *ctx, JSValueConst proto, const NodeMixinMember *tab, unsigned n)
{
    unsigned k;
    DCHECK(g_mixin_declared, "a node mixin was installed before its members were declared");
    for (k = 0; k < n; k++)
        idl_install_method(ctx, proto, tab[k].name, tab[k].len, g_mixin_id[tab[k].magic]);
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

void node_install_parent_mixin(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_id_qs >= 0, "the ParentNode mixin was installed before it was declared");
    mixin_install(ctx, proto, PARENT_NODE_MIXIN,
                  (unsigned)(sizeof(PARENT_NODE_MIXIN) / sizeof(PARENT_NODE_MIXIN[0])));
    JS_SetPropertyFunctionList(ctx, proto, js_parent_node_reads,
                               (int)(sizeof(js_parent_node_reads) / sizeof(js_parent_node_reads[0])));
    idl_install_method(ctx, proto, "querySelector", 1, g_id_qs);
    idl_install_method(ctx, proto, "querySelectorAll", 1, g_id_qsa);
    /* Not part of ParentNode in the IDL — §4.5 puts these on Document and §4.9 on Element, which between them
       is every interface that includes ParentNode except DocumentFragment. Installed here because that is one
       place rather than two, and a fragment answering them is a superset nothing can observe as wrong: its
       subtree is exactly what the walk would search. */
    idl_install_method(ctx, proto, "getElementsByTagName", 1, g_id_by_tag);
    idl_install_method(ctx, proto, "getElementsByClassName", 1, g_id_by_class);
    idl_install_method(ctx, proto, "getElementsByTagNameNS", 2, g_id_by_tag_ns);
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
    JS_CFUNC_MAGIC_DEF("isSameNode", 1, js_node_predicates, 1),
    JS_CFUNC_MAGIC_DEF("contains", 1, js_node_predicates, 2),
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
   Lexbor's lxb_dom_node_text_content walks it TWICE — once to measure, once to copy — into a buffer from the
   document's memory. One pass into a growing buffer is what a resumable walk needs anyway, and it is strictly
   less work; what it has to reproduce exactly is WHICH nodes count, which is Text nodes and nothing else, over
   child links — so a `<template>`'s content is not part of its element's text. */
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

static void node_text_append(NodeTextState *s, const char *data, size_t len)
{
    if (s->out_len + len + 1 > s->out_cap) {
        size_t want = s->out_cap ? s->out_cap * 2 : 128;
        char *n;
        while (want < s->out_len + len + 1) want *= 2;
        n = realloc(s->out, want);
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

static void node_text_release(JSContext *ctx, void *st)
{
    NodeTextState *s = st;
    (void)ctx;
    free(s->out);
    s->out = NULL;
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
        *presult = JS_NewStringLen(ctx, s->out ? s->out : "", s->out_len);
        return JS_STEP_DONE;
    }
    if (s->cursor->type == LXB_DOM_NODE_TYPE_TEXT) {
        lxb_dom_character_data_t *cd = lxb_dom_interface_character_data(s->cursor);
        node_text_append(s, (const char *)cd->data.data, cd->data.length);
    }
    s->cursor = node_next_in(s->cursor, s->root);
    return JS_STEP_YIELD;
}

static const IdlStepDecl NODE_TEXT_STEP = {
    js_node_get_text_content, sizeof(NodeTextState), node_text_visit, node_text_release,
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
        if (text)
            dom_cow_append_child(n, lxb_dom_interface_node(text));
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
        proto = (n->type == LXB_DOM_NODE_TYPE_ELEMENT && g_element_resolver)
                    ? g_element_resolver(rctx, lxb_dom_interface_element(n))
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
   node's address inherited its wrapper and its prototype. */
void node_wrap_forget(JSContext *ctx, lxb_dom_node_t *n)
{
    unsigned slot;

    if (!n || !g_wrap_cap)
        return;
    slot = node_wrap_slot(g_wraps, g_wrap_cap, n);
    if (g_wraps[slot].n != n)
        return;                       /* never wrapped: the common case for a node no script ever touched */
    /* NEUTER IT FIRST. The map's reference is not necessarily the last one — page code in the flow being
       discarded may still hold the wrapper, and its refcount keeps the JSObject alive after this. Leaving the
       freed node in its opaque makes the next property access a use-after-free that reads as an out-of-bounds
       somewhere else entirely; nulling it makes that access hit the DCHECK the accessors already carry, at the
       site that made it. */
    JS_SetOpaque(g_wraps[slot].obj, NULL);
    JS_FreeValue(ctx, g_wraps[slot].obj);
    node_wrap_remove(n);
}

/* THE AGENT'S DECLARATIONS: the classes, the IDL pool entries, and which class each node type wears. The
   PROTOTYPES those classes name are built per realm, in node_install_protos. */
static int g_id_cd[5] = { -1, -1, -1, -1, -1 };   /* §4.10's five splice members */
static int g_id_nodevalue = -1, g_id_textcontent = -1, g_id_textcontent_get = -1, g_id_data = -1,
           g_id_lookup_prefix = -1, g_id_lookup_ns = -1, g_id_default_ns = -1, g_id_root = -1,
           g_id_split_text = -1;

void node_init(JSContext *ctx)
{
    JSClassDef def = { "Node", .finalizer = node_finalizer };
    JSClassDef cd_def = { "CharacterData" }, tx_def = { "Text" }, cm_def = { "Comment" },
               cs_def = { "CDATASection" }, pi_def = { "ProcessingInstruction" };
    static const IdlArgType ROOT_ARGS[1] = { IDL_DICT };
    static const IdlDictMember ROOT_OPTS[] = { { "composed", IDL_BOOLEAN } };   /* GetRootNodeOptions */
    int i;

    if (g_protos_ready)
        return;    /* element.c asks for the base before declaring Element on top of it */
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
        g_id_cd[0] = idl_method_id(ctx, CD_UL_UL,     2, js_cd_op, 0);   /* substringData */
        g_id_cd[1] = idl_method_id(ctx, CD_STR,       1, js_cd_op, 1);   /* appendData    */
        g_id_cd[2] = idl_method_id(ctx, CD_UL_STR,    2, js_cd_op, 2);   /* insertData    */
        g_id_cd[3] = idl_method_id(ctx, CD_UL_UL,     2, js_cd_op, 3);   /* deleteData    */
        g_id_cd[4] = idl_method_id(ctx, CD_UL_UL_STR, 3, js_cd_op, 4);   /* replaceData   */
        /* §4.11 `[NewObject] Text splitText(unsigned long offset)` — one `unsigned long`, and the declaration
           is what converts it, so an object argument runs its `valueOf` on the machine like every other. */
        g_id_split_text = idl_method_id(ctx, CD_UL_UL, 1, js_text_split, 0);
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
    node_p = JS_NewObject(ctx);
    CHECK(!JS_IsException(node_p), "Node.prototype could not be allocated");
    idl_interface_tag(ctx, node_p, "Node");
    node_install_walkers(ctx, node_p);
    JS_SetPropertyFunctionList(ctx, node_p, js_node_base,
                               (int)(sizeof(js_node_base) / sizeof(js_node_base[0])));
    JS_SetPropertyFunctionList(ctx, node_p, js_node_consts,
                               (int)(sizeof(js_node_consts) / sizeof(js_node_consts[0])));
    /* §4.4: `Node : EventTarget`. The three members come down the chain from EventTarget.prototype, which is
       where §2.7 declares them — installing copies onto Node.prototype said they were declared here. */
    event_target_chain(ctx, node_p);
    idl_install_accessor(ctx, node_p, "nodeValue", js_cd_get_data, 1, g_id_nodevalue);
    idl_install_accessor_step(ctx, node_p, "textContent", g_id_textcontent_get, g_id_textcontent);
    idl_install_method(ctx, node_p, "lookupPrefix", 1, g_id_lookup_prefix);
    idl_install_method(ctx, node_p, "lookupNamespaceURI", 1, g_id_lookup_ns);
    idl_install_method(ctx, node_p, "isDefaultNamespace", 1, g_id_default_ns);
    idl_install_method(ctx, node_p, "getRootNode", 0, g_id_root);
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
        static const char *const CD_NAMES[5] = { "substringData", "appendData", "insertData",
                                                 "deleteData", "replaceData" };
        static const int CD_ARGC[5] = { 2, 1, 2, 2, 3 };
        int k;
        for (k = 0; k < 5; k++)
            idl_install_method(ctx, cd, CD_NAMES[k], CD_ARGC[k], g_id_cd[k]);
    }
    /* §4.10: `CharacterData includes ChildNode` — `textNode.remove()` is real, and a page that tears down
       text with it had nothing. ParentNode is NOT included: character data has no children. */
    node_install_child_mixin(ctx, cd);
    {
        JSValue text_proto = JS_NewObjectProto(ctx, cd);
        JSValue comment_proto = JS_NewObjectProto(ctx, cd);
        CHECK(!JS_IsException(text_proto) && !JS_IsException(comment_proto),
              "a CharacterData-derived prototype could not be allocated");
        idl_interface_tag(ctx, text_proto, "Text");
        /* §4.11's OWN two members. `Text : CharacterData` adds exactly these, and a Text prototype with
           nothing on it is what made `splitText` absent while `nodeType === 3` answered — the shape this
           engine treats as worst, because nothing throws until the page's own call does. */
        idl_install_method(ctx, text_proto, "splitText", 1, g_id_split_text);
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
    /* §4.4 puts its constants on the interface object as well as the prototype. Only Node declares any, and a
       derived interface INHERITS them — `Text.ELEMENT_NODE` is a real read — so the chain carries them rather
       than each object repeating the table. */
    {
        JSValue base_proto = node_proto(ctx);
        bool is_node = JS_VALUE_GET_PTR(proto) == JS_VALUE_GET_PTR(base_proto);
        JS_FreeValue(ctx, base_proto);
        if (is_node)
            JS_SetPropertyFunctionList(ctx, ctor, js_node_consts,
                                       (int)(sizeof(js_node_consts) / sizeof(js_node_consts[0])));
        else {
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
        node_install_interface(ctx, global, "Node", np);
        node_install_interface(ctx, global, "CharacterData", cdp);
        node_install_interface(ctx, global, "Text", tp);
        JSValue csp = node_type_proto(ctx, LXB_DOM_NODE_TYPE_CDATA_SECTION);
        JSValue pip = node_type_proto(ctx, LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION);
        node_install_interface(ctx, global, "Comment", cmp);
        node_install_interface(ctx, global, "CDATASection", csp);
        node_install_interface(ctx, global, "ProcessingInstruction", pip);
        JS_FreeValue(ctx, np); JS_FreeValue(ctx, cdp);
        JS_FreeValue(ctx, tp); JS_FreeValue(ctx, cmp);
        JS_FreeValue(ctx, csp); JS_FreeValue(ctx, pip);
    }
}


void node_free(JSContext *ctx)
{
    int i;
    for (i = 0; i < g_wrap_cap; i++)
        if (g_wraps[i].n)
            JS_FreeValue(ctx, g_wraps[i].obj);
    free(g_wraps);
    g_wraps = NULL; g_wrap_n = g_wrap_cap = 0;
    /* The prototypes are the REALMS' — each is released with its context. What the AGENT holds is the table of
       CLASS IDS, which is not a reference to anything. */
    g_protos_ready = 0;
}
