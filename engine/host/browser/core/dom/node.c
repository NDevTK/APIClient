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
#include <lexbor/ns/ns.h>

#include "check.h"
#include "quickjs.h"
#include "solver/dom_cow.h"
#include "solver/concolic.h"
#include "solver/attr_shadow.h"
#include "core/dom/node.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/dom/document.h"
#include "core/events/event_target.h"

static const IdlArgType IDL_1NSTR[1] = { IDL_DOMSTRING_NULLABLE };

static JSClassID g_node_class;

/* THE INTERFACE PER NODE TYPE. Indexed by lxb_dom_node_type_t, filled with Node.prototype at init so that a
   node kind no derived component claims is honestly a Node rather than a bare object. */
static JSValue g_protos[LXB_DOM_NODE_TYPE_LAST_ENTRY];
static JSValue g_node_proto;
static int     g_protos_ready;
static JSValue g_chardata_proto;

typedef struct { lxb_dom_node_t *n; JSValue obj; } NodeEntry;
static NodeEntry *g_wraps;
static int        g_wrap_n, g_wrap_cap;

JSClassID node_class_id(void) { return g_node_class; }

lxb_dom_node_t *node_of(JSValueConst v)
{
    return JS_GetOpaque(v, g_node_class);
}

/* §4.4 nodeType — the numeric constants a page switches on. */
static JSValue js_node_get_type(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_node_t *n = node_of(this_val);
    if (!n) return JS_UNDEFINED;
    return JS_NewInt32(ctx, (int)n->type);
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

/* §4.4 childNodes. A STATIC array, not the spec's live NodeList: a page that inserts a child and re-reads
   `.length` will not see the change. Named here rather than papered over — it is the same shape
   querySelectorAll and getElementsByTagName already return, and the live collection is its own component. */
static JSValue js_node_child_nodes(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_node_t *n = node_of(this_val), *c;
    JSValue arr;
    uint32_t i = 0;

    if (!n) return JS_NewArray(ctx);
    arr = JS_NewArray(ctx);
    for (c = n->first_child; c; c = c->next)
        JS_SetPropertyUint32(ctx, arr, i++, node_wrap(ctx, c));
    return arr;
}

/* §4.2.3 appendChild / removeChild — the per-flow chokepoints, and they return the node the spec returns
   (pages chain on it). Any node kind, which is the whole point of this file. */
static JSValue js_node_append_child(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_node_remove_child(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);

/* A newly inserted <script> is PREPARED (HTML 4.12.1) by the element component, which owns that rule; the base
   asks for it through this hook so node.c does not have to know what a script is. */
/* AN ELEMENT'S INTERFACE IS KEYED BY ITS TAG, which is HTML's mapping and not the DOM's — this table is keyed
   by node TYPE and cannot answer it. So the html layer registers the answer and node_wrap ASKS, which keeps it
   the ONE place a wrapper is built. Not a fallback and nothing to select against: there is exactly one answer
   per element, and an element wrapped before the resolver exists is a DCHECK, not a degraded path. */
static JSValueConst (*g_element_resolver)(lxb_dom_element_t *el);
void node_set_element_resolver(JSValueConst (*fn)(lxb_dom_element_t *el)) { g_element_resolver = fn; }

/* THE TREE HOOK IS INSTALLED INTO THE CHOKEPOINT, not called from each mutation site, and the difference is a
   bug this file had: `element_on_inserted` was invoked from appendChild and NOWHERE else, so an element that
   entered the tree by insertBefore, by replaceChild, or by an innerHTML parse was never prepared and never
   upgraded. Eleven sites mutate the tree; one remembered. The chokepoint is the one place that cannot be
   forgotten, which is the same argument that put capture there. */
void node_set_tree_hook(void (*fn)(JSContext *ctx, lxb_dom_node_t *n, int inserted))
{
    dom_cow_set_tree_hook(fn);
}

static JSValue js_node_append_child(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lxb_dom_node_t *n = node_of(this_val), *child;

    if (!n || argc < 1) return JS_UNDEFINED;
    child = node_of(argv[0]);
    if (!child)
        return JS_ThrowTypeError(ctx, "appendChild requires a Node");
    dom_cow_append_child(n, child);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_node_remove_child(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lxb_dom_node_t *n = node_of(this_val), *child;

    if (!n || argc < 1) return JS_UNDEFINED;
    child = node_of(argv[0]);
    if (!child)
        return JS_ThrowTypeError(ctx, "removeChild requires a Node");
    if (child->parent != n)
        return JS_ThrowDOMException(ctx, "NotFoundError", "the node to remove is not a child of this node");
    dom_cow_remove_child(child);
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
    /* §4.2.4: anything that is not a Node is stringified into a Text node. The conversion is the page's code,
       and it has already run — the member declares `any...`, and the ToString below is on a value the
       declaration left alone, so it is done here where the argument is known not to be a Node. */
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
        case 1: case 3: dom_cow_insert_before(ref, added); break;
        case 2: if (ref) dom_cow_insert_before(ref, added); else dom_cow_append_child(parent, added); break;
        case 4: case 6: dom_cow_append_child(n, added); break;
        case 5: if (n->first_child) dom_cow_insert_before(n->first_child, added);
                else dom_cow_append_child(n, added);
                break;
        default: DFAIL("a ChildNode/ParentNode member ran with an unknown magic"); break;
        }
    }
    if (magic == 0 || magic == 3) dom_cow_remove_child(n);
    return JS_UNDEFINED;
}

static bool node_is_chardata(const lxb_dom_node_t *n)
{
    return n->type == LXB_DOM_NODE_TYPE_TEXT || n->type == LXB_DOM_NODE_TYPE_COMMENT;
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

/* The value arrives CONVERTED — a real string, JS_NULL for the nullable member, or a concolic that crossed as
   itself. Nothing here runs the page's code, which is the claim the declaration in node_init makes. */
static JSValue js_cd_set_data(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    const char *s;
    size_t len;

    if (!n) return JS_UNDEFINED;
    if (!node_is_chardata(n)) {
        if (magic == 1)
            return JS_UNDEFINED;   /* §4.4 nodeValue setter: "otherwise: do nothing" */
        return JS_ThrowTypeError(ctx, "CharacterData.data set on a node that holds no character data");
    }
    /* §4.4 the nodeValue setter's own first step; data reaches this through [LegacyNullToEmptyString]. */
    if (JS_IsNull(val)) {
        dom_cow_set_text(n, "", 0);
        return JS_UNDEFINED;
    }
    /* Unknown external input has no bytes: its SHAPE is what the node carries, the same answer textContent
       gives, so a source parked in the tree as text still displays as the source it came from. */
    if (concolic_is(val)) {
        s = concolic_shape_c(val);
        dom_cow_set_text(n, s ? s : "", s ? strlen(s) : 0);
        return JS_UNDEFINED;
    }
    DCHECK(JS_IsString(val), "a CharacterData write reached the body unconverted — the IDL declaration is what "
                             "converts it, and running the page's toString from here is the drive-to-completion "
                             "the flow machinery exists to avoid");
    s = JS_ToCStringLen(ctx, &len, val);
    if (!s) return JS_EXCEPTION;
    dom_cow_set_text(n, s, len);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue js_cd_get_length(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_node_t *n = node_of(this_val);
    if (!n) return JS_NewInt32(ctx, 0);
    if (!node_is_chardata(n))
        return JS_ThrowTypeError(ctx, "CharacterData.length read on a node that holds no character data");
    return JS_NewInt32(ctx, (int)lxb_dom_interface_character_data(n)->data.length);
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

/* §4.4 "root": the topmost inclusive ancestor. Shadow trees do not exist in this engine yet, so a node's root
   and its SHADOW-INCLUDING root are the same node. */
static lxb_dom_node_t *node_root(lxb_dom_node_t *n)
{
    while (n->parent) n = n->parent;
    return n;
}

bool node_is_connected(const lxb_dom_node_t *n)
{
    return n && node_root((lxb_dom_node_t *)n)->type == LXB_DOM_NODE_TYPE_DOCUMENT;
}

/* §4.2 "inclusive ancestor": walk UP from the descendant, which is O(depth) with no allocation, rather than
   down from the ancestor, which is O(subtree). */
static bool node_is_inclusive_ancestor(const lxb_dom_node_t *a, const lxb_dom_node_t *b)
{
    for (; b; b = b->parent)
        if (b == a) return true;
    return false;
}

/* The PRE-ORDER successor within `root`'s subtree, or NULL at the end. This is the one traversal primitive the
   spec's tree-order algorithms need, and having it once is what keeps them from each growing a walker. */
static lxb_dom_node_t *node_next_in(lxb_dom_node_t *n, lxb_dom_node_t *root)
{
    if (n->first_child) return n->first_child;
    while (n != root) {
        if (n->next) return n->next;
        n = n->parent;
        if (!n) return NULL;
    }
    return NULL;
}

/* magic 0 = isConnected, 1 = ownerDocument, 2 = parentElement, 3 = baseURI */
static JSValue js_node_facts(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);

    if (!n) return JS_UNDEFINED;
    switch (magic) {
    case 0:
        /* §4.4: connected iff the shadow-including root is a DOCUMENT. A node a page has created but not yet
           inserted is NOT connected, which is the difference a component-mount check is asking about. */
        return JS_NewBool(ctx, node_root(n)->type == LXB_DOM_NODE_TYPE_DOCUMENT);
    case 1:
        /* §4.4: a Document's ownerDocument is null; every other node's is its node document. */
        if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT) return JS_NULL;
        return node_wrap(ctx, lxb_dom_interface_node(n->owner_document));
    case 2:
        /* §4.4: the parent, but only when it is an ELEMENT — the difference from parentNode is exactly the
           document and the fragment, which is why a walk-up loop uses this one and stops on its own. */
        return (n->parent && n->parent->type == LXB_DOM_NODE_TYPE_ELEMENT) ? node_wrap(ctx, n->parent) : JS_NULL;
    default:
        /* §4.4: the node document's document base URL, serialized. There is one document per engine instance
           and no <base> support yet, so this is its address — asked of the component that owns it rather than
           re-derived here, because two answers to "what is this document's URL" is how they drift apart. */
        DCHECK(magic == 3, "a Node fact was declared with a magic this table does not name");
        return JS_NewString(ctx, document_base_url());
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

static JSValue js_node_is_equal(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lxb_dom_node_t *a = node_of(this_val), *b = argc > 0 ? node_of(argv[0]) : NULL;
    lxb_dom_node_t *ra, *rb;

    if (!a || !b) return JS_FALSE;   /* `Node? otherNode`: null is never equal */
    ra = a; rb = b;
    for (;;) {
        if (!node_shallow_equal(a, b)) return JS_FALSE;
        /* the CHILD COUNTS must match, and comparing them here rather than at the end is what lets the two
           cursors advance in lockstep below without one running off the end of the other. */
        {
            lxb_dom_node_t *ca = a->first_child, *cb = b->first_child;
            while (ca && cb) { ca = ca->next; cb = cb->next; }
            if (ca || cb) return JS_FALSE;
        }
        a = node_next_in(a, ra);
        b = node_next_in(b, rb);
        if (!a || !b) return JS_NewBool(ctx, a == NULL && b == NULL);
    }
}

/* §4.4 compareDocumentPosition — the bitmask a page uses to sort nodes into document order. */
enum {
    NODE_POS_DISCONNECTED = 0x01, NODE_POS_PRECEDING = 0x02, NODE_POS_FOLLOWING = 0x04,
    NODE_POS_CONTAINS = 0x08, NODE_POS_CONTAINED_BY = 0x10, NODE_POS_IMPLEMENTATION_SPECIFIC = 0x20,
};

static JSValue js_node_compare_position(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lxb_dom_node_t *a = node_of(this_val), *b = argc > 0 ? node_of(argv[0]) : NULL, *p;

    if (!a || !b)
        return JS_ThrowTypeError(ctx, "compareDocumentPosition requires a Node");
    if (a == b) return JS_NewInt32(ctx, 0);
    if (node_root(a) != node_root(b))
        /* §4.4: disconnected nodes get a consistent-but-arbitrary order, and it must be CONSISTENT — the same
           pair must answer the same way every time or a page's sort never terminates. Pointer order is stable
           for the lifetime of the two nodes, which is what the spec's "implementation-specific" allows for. */
        return JS_NewInt32(ctx, NODE_POS_DISCONNECTED | NODE_POS_IMPLEMENTATION_SPECIFIC |
                                (a < b ? NODE_POS_FOLLOWING : NODE_POS_PRECEDING));
    if (node_is_inclusive_ancestor(a, b))
        return JS_NewInt32(ctx, NODE_POS_CONTAINED_BY | NODE_POS_FOLLOWING);
    if (node_is_inclusive_ancestor(b, a))
        return JS_NewInt32(ctx, NODE_POS_CONTAINS | NODE_POS_PRECEDING);
    /* Neither contains the other: whichever the pre-order walk reaches first PRECEDES. */
    for (p = node_root(a); p; p = node_next_in(p, NULL)) {
        if (p == a) return JS_NewInt32(ctx, NODE_POS_FOLLOWING);
        if (p == b) return JS_NewInt32(ctx, NODE_POS_PRECEDING);
    }
    DFAIL("compareDocumentPosition walked a shared root without reaching either node — the tree is not a tree");
    return JS_NewInt32(ctx, 0);
}

/* §4.4 getRootNode(optional GetRootNodeOptions options = {}).
 *
 * The answer never depends on the option: `composed` chooses between the root and the SHADOW-INCLUDING root,
 * and with no shadow trees in this engine those are the same node. Reading it is still the page's code —
 * `getRootNode({ get composed(){ … } })` is a getter, and a Proxy makes even a plain object one — so the read
 * is a REQUEST, and the declaration is what performs it: by the time this body runs the dictionary is a plain
 * engine-built object and there is no user code left to reach. Skipping the read because the result would not
 * change is the shortcut this file does not take. It was a whole hand-rolled machine before the IDL dictionary
 * conversion could express a typed member; a second implementation of a request the machine already makes. */
static JSValue js_node_root(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    (void)argc; (void)argv; (void)magic;
    return n ? node_wrap(ctx, node_root(n)) : JS_UNDEFINED;
}

/* §4.4 normalize — remove empty Text nodes and merge adjacent ones. A page calls it before comparing or
   serialising a tree it built piecemeal, and both halves go through the per-flow chokepoints so a forked arm
   normalising its own subtree does not normalise its sibling's. The walk is a cursor over the current node's
   descendants with the next node taken BEFORE the current one can be removed. */
static JSValue js_node_normalize(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lxb_dom_node_t *root = node_of(this_val), *n, *next;

    (void)ctx; (void)argc; (void)argv;
    if (!root) return JS_UNDEFINED;
    for (n = node_next_in(root, root); n; n = next) {
        next = node_next_in(n, root);
        if (n->type != LXB_DOM_NODE_TYPE_TEXT)
            continue;
        {
            lxb_dom_character_data_t *cd = lxb_dom_interface_character_data(n);
            if (cd->data.length == 0) {
                dom_cow_remove_child(n);
                continue;
            }
            /* Absorb the contiguous following Text siblings into this one, then drop them. */
            while (n->next && n->next->type == LXB_DOM_NODE_TYPE_TEXT) {
                lxb_dom_node_t *sib = n->next;
                lxb_dom_character_data_t *sd = lxb_dom_interface_character_data(sib);
                size_t len = cd->data.length + sd->data.length;
                char *buf = malloc(len ? len : 1);
                CHECK(buf != NULL, "normalize: OOM merging two Text nodes — dropping the merge would leave the "
                                   "page a tree it did not build");
                memcpy(buf, cd->data.data, cd->data.length);
                memcpy(buf + cd->data.length, sd->data.data, sd->data.length);
                if (next == sib) next = node_next_in(sib, root);
                dom_cow_set_text(n, buf, len);
                dom_cow_remove_child(sib);
                free(buf);
            }
        }
    }
    return JS_UNDEFINED;
}

/* §4.4 cloneNode. `optional boolean subtree = false` is a ToBoolean, which is total and runs none of the page's
   code, so this is not a coercion the IDL machine has to carry. Lexbor owns the copy — a hand-rolled one here
   would be a second answer to "what is a copy of this node" beside the tree builder's. The clone is NOT
   inserted, so there is nothing to capture: an uninserted node is flow-private until an insert chokepoint puts
   it in the shared tree. */
static JSValue js_node_clone(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    lxb_dom_node_t *n = node_of(this_val), *copy;
    bool deep = argc > 0 && JS_ToBool(ctx, argv[0]);

    if (!n) return JS_UNDEFINED;
    if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT)
        return JS_ThrowDOMException(ctx, "NotSupportedError", "cloning a Document is not supported");
    copy = lxb_dom_node_clone(n, deep);
    CHECK(copy != NULL, "cloneNode: the Lexbor node copy failed — returning nothing would hand the page a null "
                        "it has no way to distinguish from a node it never asked for");
    return node_wrap(ctx, copy);
}

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
        dom_cow_append_child(parent, node);       /* insertBefore(n, null) IS append */
        return JS_DupValue(ctx, argv[0]);
    }
    if (!child || child->parent != parent)
        return JS_ThrowDOMException(ctx, "NotFoundError",
                                    "the reference child is not a child of this node");
    dom_cow_insert_before(child, node);
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
static const JSCFunctionListEntry js_child_node_mixin[] = {
    JS_CFUNC_MAGIC_DEF("remove", 0, js_node_mixin, 0),
    JS_CFUNC_MAGIC_DEF("before", 1, js_node_mixin, 1),
    JS_CFUNC_MAGIC_DEF("after", 1, js_node_mixin, 2),
    JS_CFUNC_MAGIC_DEF("replaceWith", 1, js_node_mixin, 3),
};

/* §4.2.8 ParentNode's insertion half — on Element, Document and DocumentFragment. querySelector and children
   are the same mixin and already live on the interfaces that declare them. */
static const JSCFunctionListEntry js_parent_node_mixin[] = {
    JS_CFUNC_MAGIC_DEF("append", 1, js_node_mixin, 4),
    JS_CFUNC_MAGIC_DEF("prepend", 1, js_node_mixin, 5),
    JS_CFUNC_MAGIC_DEF("replaceChildren", 0, js_node_mixin, 6),
};

void node_install_child_mixin(JSContext *ctx, JSValueConst proto)
{
    JS_SetPropertyFunctionList(ctx, (JSValue)proto, js_child_node_mixin,
                               (int)(sizeof(js_child_node_mixin) / sizeof(js_child_node_mixin[0])));
}

void node_install_parent_mixin(JSContext *ctx, JSValueConst proto)
{
    JS_SetPropertyFunctionList(ctx, (JSValue)proto, js_parent_node_mixin,
                               (int)(sizeof(js_parent_node_mixin) / sizeof(js_parent_node_mixin[0])));
}

static const JSCFunctionListEntry js_node_base[] = {
    JS_CFUNC_DEF("appendChild", 1, js_node_append_child),
    JS_CFUNC_DEF("removeChild", 1, js_node_remove_child),
    JS_CFUNC_MAGIC_DEF("insertBefore", 2, js_node_insert, 0),
    JS_CFUNC_MAGIC_DEF("replaceChild", 2, js_node_insert, 1),
    JS_CFUNC_DEF("cloneNode", 0, js_node_clone),
    JS_CFUNC_DEF("normalize", 0, js_node_normalize),
    JS_CFUNC_DEF("isEqualNode", 1, js_node_is_equal),
    JS_CFUNC_DEF("compareDocumentPosition", 1, js_node_compare_position),
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
    return n->type == LXB_DOM_NODE_TYPE_ELEMENT || n->type == LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT;
}

static JSValue js_node_get_text_content(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    lxb_char_t *txt;
    size_t len = 0;
    JSValue r;
    int si;

    (void)magic;
    if (!n) return JS_NULL;
    if (node_is_chardata(n))
        return js_cd_get_data(ctx, this_val, 1);
    if (!node_has_children_as_text(n))
        return JS_NULL;                        /* §4.4 "otherwise: null" — a Document's is null, not "" */
    if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        si = attr_shadow_find(lxb_dom_interface_element(n), ATTR_SLOT_PROPERTY, "textContent");
        if (si >= 0)
            return JS_DupValue(ctx, attr_shadow_opaque(si));
    }
    txt = lxb_dom_node_text_content(n, &len);
    if (!txt) return JS_NewStringLen(ctx, "", 0);
    r = JS_NewStringLen(ctx, (const char *)txt, len);
    lxb_dom_document_destroy_text(n->owner_document, txt);
    return r;
}

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
JSValueConst node_type_proto(int node_type)
{
    DCHECK(g_protos_ready, "a node type's interface was asked for before node_init built the table");
    DCHECK(node_type > 0 && node_type < LXB_DOM_NODE_TYPE_LAST_ENTRY,
           "a node type the DOM does not have was asked for its interface");
    return g_protos[node_type];
}

JSValueConst node_proto(void)
{
    DCHECK(g_protos_ready, "Node.prototype was asked for before node_init built it");
    return g_node_proto;
}

void node_set_proto(JSContext *ctx, int node_type, JSValue proto)
{
    DCHECK(g_protos_ready, "a derived interface registered its prototype before node_init built Node.prototype");
    DCHECK(node_type > 0 && node_type < LXB_DOM_NODE_TYPE_LAST_ENTRY,
           "a prototype was registered for a node type the DOM does not have");
    DCHECK(JS_VALUE_GET_PTR(g_protos[node_type]) == JS_VALUE_GET_PTR(g_node_proto),
           "two components claimed the same node type's interface — one of them would silently lose");
    JS_FreeValue(ctx, g_protos[node_type]);
    g_protos[node_type] = proto;        /* CONSUMED: the table owns every prototype it holds */
}

JSValue node_wrap(JSContext *ctx, lxb_dom_node_t *n)
{
    JSValue obj;
    int i;

    if (!n)
        return JS_NULL;
    for (i = 0; i < g_wrap_n; i++)
        if (g_wraps[i].n == n)
            return JS_DupValue(ctx, g_wraps[i].obj);

    DCHECK(g_protos_ready, "a node was wrapped before the DOM interfaces existed");
    DCHECK((int)n->type > 0 && (int)n->type < LXB_DOM_NODE_TYPE_LAST_ENTRY,
           "a Lexbor node carries a type the DOM does not define");
    DCHECK(n->type != LXB_DOM_NODE_TYPE_ELEMENT || g_element_resolver != NULL,
           "an Element node was wrapped before the HTML layer registered its interface resolver");
    obj = JS_NewObjectProtoClass(ctx,
              (n->type == LXB_DOM_NODE_TYPE_ELEMENT && g_element_resolver)
                  ? g_element_resolver(lxb_dom_interface_element(n))
                  : g_protos[n->type],
              g_node_class);
    if (JS_IsException(obj))
        return obj;
    JS_SetOpaque(obj, n);

    if (g_wrap_n == g_wrap_cap) {
        int c = g_wrap_cap ? g_wrap_cap * 2 : 16;
        NodeEntry *a = realloc(g_wraps, sizeof(*a) * (size_t)c);
        CHECK(a != NULL, "the node wrapper table allocation failed: a dropped wrapper breaks node identity, and "
                         "every `n === other` the page makes after it is silently false");
        g_wraps = a; g_wrap_cap = c;
    }
    g_wraps[g_wrap_n].n = n;
    g_wraps[g_wrap_n].obj = JS_DupValue(ctx, obj);
    g_wrap_n++;
    return obj;
}

static void node_finalizer(JSRuntime *rt, JSValue val) { (void)rt; (void)val; }

void node_init(JSContext *ctx)
{
    JSClassDef def = { "Node", .finalizer = node_finalizer };
    JSValue cd;
    int i, id_nodevalue, id_textcontent, id_data;

    if (g_protos_ready)
        return;    /* element.c asks for the base before building Element.prototype on top of it */

    JS_NewClassID(JS_GetRuntime(ctx), &g_node_class);
    JS_NewClass(JS_GetRuntime(ctx), g_node_class, &def);

    /* Node.prototype. §4.4 `interface Node : EventTarget`: every node is one, and only the global was — so
       `el.addEventListener(...)` was "not a function" on every element a page wired up, which is where
       testharness.js stopped on eight documents (`getElementById("rerun").addEventListener`). It belongs here
       because it is a BASE member, and here it is one function shared by every node rather than a fresh closure
       on each. */
    g_node_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_node_proto), "Node.prototype could not be allocated");
    JS_SetPropertyFunctionList(ctx, g_node_proto, js_node_base,
                               (int)(sizeof(js_node_base) / sizeof(js_node_base[0])));
    JS_SetPropertyFunctionList(ctx, g_node_proto, js_node_consts,
                               (int)(sizeof(js_node_consts) / sizeof(js_node_consts[0])));
    event_target_install(ctx, g_node_proto);
    g_protos_ready = 1;

    /* Every node kind is a Node until a component claims it. A ProcessingInstruction wrapper answering the Node
       members is honest; a bare object answering none of them is not. */
    for (i = 0; i < LXB_DOM_NODE_TYPE_LAST_ENTRY; i++)
        g_protos[i] = JS_DupValue(ctx, g_node_proto);

    /* `attribute DOMString? nodeValue` and `attribute DOMString? textContent` — both Node members, so both are
       declared and installed here. */
    id_nodevalue = idl_setter_id(ctx, IDL_DOMSTRING_NULLABLE, false, js_cd_set_data, 1);
    idl_install_accessor(ctx, g_node_proto, "nodeValue", js_cd_get_data, 1, id_nodevalue);
    id_textcontent = idl_setter_id(ctx, IDL_DOMSTRING_NULLABLE, false, js_node_set_text_content, 0);
    idl_install_accessor(ctx, g_node_proto, "textContent", js_node_get_text_content, 0, id_textcontent);

    /* CharacterData.prototype — §4.10, `interface CharacterData : Node`, so it INHERITS from Node.prototype
       rather than repeating its members. */
    cd = JS_NewObjectProto(ctx, g_node_proto);
    CHECK(!JS_IsException(cd), "CharacterData.prototype could not be allocated");
    JS_SetPropertyFunctionList(ctx, cd, js_chardata_base,
                               (int)(sizeof(js_chardata_base) / sizeof(js_chardata_base[0])));
    /* `attribute [LegacyNullToEmptyString] DOMString data` — the extended attribute is part of the TYPE, so the
       body never sees a null and never has to remember the rule. */
    id_data = idl_setter_id(ctx, IDL_DOMSTRING, true, js_cd_set_data, 0);
    idl_install_accessor(ctx, cd, "data", js_cd_get_data, 0, id_data);

    /* §4.4 the three namespace lookups. Each takes a `DOMString?`, so each goes on the shared IDL machine —
       `n.lookupPrefix({toString(){ … }})` is the page's code exactly like every other DOMString argument. */
    idl_install_method(ctx, g_node_proto, "lookupPrefix", 1,
                       idl_method_id(ctx, IDL_1NSTR, 1, js_node_lookup_ns, 0));
    idl_install_method(ctx, g_node_proto, "lookupNamespaceURI", 1,
                       idl_method_id(ctx, IDL_1NSTR, 1, js_node_lookup_ns, 1));
    idl_install_method(ctx, g_node_proto, "isDefaultNamespace", 1,
                       idl_method_id(ctx, IDL_1NSTR, 1, js_node_lookup_ns, 2));

    {
        static const IdlArgType ROOT_ARGS[1] = { IDL_DICT };
        static const IdlDictMember ROOT_OPTS[] = { { "composed", IDL_BOOLEAN } };   /* GetRootNodeOptions */
        idl_install_method(ctx, g_node_proto, "getRootNode", 0,
                           idl_method_id_dict(ctx, ROOT_ARGS, 1, ROOT_OPTS,
                                              (int)(sizeof(ROOT_OPTS) / sizeof(ROOT_OPTS[0])),
                                              js_node_root, 0));
    }

    /* Text and Comment are their own interfaces — `interface Text : CharacterData` and `Comment : CharacterData`
       — so a page's `x instanceof Text` and `Text.prototype` have something to name, and the two do not share
       one object that answers for both. */
    {
        JSValue text_proto = JS_NewObjectProto(ctx, cd);
        JSValue comment_proto = JS_NewObjectProto(ctx, cd);
        CHECK(!JS_IsException(text_proto) && !JS_IsException(comment_proto),
              "a CharacterData-derived prototype could not be allocated");
        node_set_proto(ctx, LXB_DOM_NODE_TYPE_TEXT, text_proto);
        node_set_proto(ctx, LXB_DOM_NODE_TYPE_COMMENT, comment_proto);
        /* §4.10: `CharacterData includes ChildNode` — `textNode.remove()` is real, and a page that tears down
           text with it had nothing. ParentNode is NOT included: character data has no children. */
        node_install_child_mixin(ctx, cd);
        g_chardata_proto = cd;   /* HELD: the interface objects below name it, and node_free releases it */
    }
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
    JSValue ctor = JS_NewCFunction2(ctx, js_node_iface_ctor, name, 0, JS_CFUNC_constructor, 0);

    DCHECK(JS_IsObject(proto), "a DOM interface object was installed with no prototype behind it");
    CHECK(!JS_IsException(ctor), "a DOM interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);   /* .prototype and .constructor, both directions, one call */
    /* §4.4 puts its constants on the interface object as well as the prototype. Only Node declares any, and a
       derived interface INHERITS them — `Text.ELEMENT_NODE` is a real read — so the chain carries them rather
       than each object repeating the table. */
    if (JS_VALUE_GET_PTR(proto) == JS_VALUE_GET_PTR(g_node_proto))
        JS_SetPropertyFunctionList(ctx, ctor, js_node_consts,
                                   (int)(sizeof(js_node_consts) / sizeof(js_node_consts[0])));
    else {
        JSValue base = node_interface_object(ctx, global);   /* OWNED by this read, and JS_SetPrototype borrows */
        JS_SetPrototype(ctx, ctor, base);
        JS_FreeValue(ctx, base);
    }
    JS_SetPropertyStr(ctx, (JSValue)global, name, ctor);
}

void node_install_interfaces(JSContext *ctx, JSValueConst global)
{
    DCHECK(g_protos_ready, "the DOM interface objects were installed before node_init built their prototypes");
    node_install_interface(ctx, global, "Node", g_node_proto);
    node_install_interface(ctx, global, "CharacterData", g_chardata_proto);
    node_install_interface(ctx, global, "Text", g_protos[LXB_DOM_NODE_TYPE_TEXT]);
    node_install_interface(ctx, global, "Comment", g_protos[LXB_DOM_NODE_TYPE_COMMENT]);
}


void node_free(JSContext *ctx)
{
    int i;
    for (i = 0; i < g_wrap_n; i++)
        JS_FreeValue(ctx, g_wraps[i].obj);
    free(g_wraps);
    g_wraps = NULL; g_wrap_n = g_wrap_cap = 0;
    /* Each derived prototype is held once per node type that maps to it; the base is held by the table too. */
    for (i = 0; i < LXB_DOM_NODE_TYPE_LAST_ENTRY; i++) {
        JS_FreeValue(ctx, g_protos[i]);
        g_protos[i] = JS_UNDEFINED;
    }
    JS_FreeValue(ctx, g_node_proto);
    JS_FreeValue(ctx, g_chardata_proto);
    g_node_proto = g_chardata_proto = JS_UNDEFINED;
    g_protos_ready = 0;
}
