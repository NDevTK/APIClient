/* SLOTS AND SLOTTABLES — DOM §4.2.2 (finding, assigning and signalling), §4.2.9's `Slottable` mixin, and
 * HTML §4.12.4's `<slot>` element.
 *
 * WHAT A SLOT IS FOR. A shadow tree replaces the host's own children in the rendered tree, so a component that
 * wants to KEEP them puts a `<slot>` where they should appear. That is not a rendering nicety here: it is how
 * every component library in the platform composes, and `slot.assignedNodes()` / the `slotchange` event are
 * what a component's own code reads to decide what to build. `HTMLSlotElement` existed in this engine as a
 * NAME in the element-interface table with one reflected attribute and none of its three methods, which is the
 * shape §NO STUBS calls a fidelity gap wearing an interface's name.
 *
 * FOUR ALGORITHM GROUPS, and they are written as the standard writes them rather than as one "recompute":
 *   §4.2.2.3 FINDING       — "find a slot" (a slottable asks which slot it goes in), "find slottables" (a slot
 *                            asks which nodes it holds), "find flattened slottables" (the same, through nested
 *                            slots, which is what `{flatten: true}` means).
 *   §4.2.2.4 ASSIGNING     — "assign slottables" writes a slot's assigned nodes and SIGNALS when they changed,
 *                            "assign slottables for a tree" does that for every slot of a tree, and "assign a
 *                            slot" is the one-slottable entry the mutation algorithms use.
 *   §4.2.2.5 SIGNALLING    — "signal a slot change" appends to the agent's signal slots and queues a mutation
 *                            observer microtask; "notify mutation observers" fires `slotchange` at each.
 *   §4.2.3   THE TRIGGERS  — insert's two slot steps, remove's three, and the two attribute change steps.
 *
 * WHERE THE STATE LIVES. A slot's `assigned nodes` and `manually assigned nodes`, and a slottable's
 * `assigned slot` and `manual slot assignment`, are all JS ARRAYS AND WRAPPER SLOTS — never malloc'd C. Two
 * forked arms disagree about which nodes a slot holds, and a parked flow has to carry that to the cold tier and
 * back; an Array's mutations are property writes the COW delta already captures, and a malloc'd list captured
 * as a pointer would revert the pointer and leak the nodes. That is CLAUDE.md's rule for platform data a flow
 * queues, and a slot's assigned nodes is exactly that.
 *
 * A SLOTTABLE'S `name` IS NOT STORED. §4.2.2's name exists because the standard keeps it in sync with the
 * `slot` content attribute through the attribute change steps; reading the attribute answers the same question
 * with no second copy to drift. The change STEPS still exist here — they run "assign slottables" and "assign a
 * slot", which are real side effects and are the whole reason the standard hooks the attribute at all.
 *
 * WHAT IS HONESTLY ABSENT, BY NAME — see SPEC_STEPS.md §17.6. `assignedNodes({flatten: true})` is a walk of the
 * page's own nesting and is not yet a step machine; the flattened walk is iterative (no C recursion) but a very
 * deep component tree holds the scheduler for the length of one call. */
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"
#include "core/dom/mutation_observer.h"
#include "core/dom/slot.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/idl_args.h"
#include "core/realm.h"

static int g_ready;
static int g_id_assigned_nodes = -1, g_id_assigned_elements = -1, g_id_assign = -1;

/* THE SLOT KEYS — Symbols this component minted and never published. */
static JSValue g_assigned_key = JS_UNDEFINED;    /* slot -> its assigned nodes (an Array) */
static JSAtom  g_atom_assigned = JS_ATOM_NULL;
static JSValue g_manual_key = JS_UNDEFINED;      /* slot -> its manually assigned nodes (an Array) */
static JSAtom  g_atom_manual = JS_ATOM_NULL;
static JSValue g_slot_of_key = JS_UNDEFINED;     /* slottable -> its assigned slot */
static JSAtom  g_atom_slot_of = JS_ATOM_NULL;
static JSValue g_manual_of_key = JS_UNDEFINED;   /* slottable -> its manual slot assignment */
static JSAtom  g_atom_manual_of = JS_ATOM_NULL;
/* §4.2.2.5: "Each similar-origin window agent has SIGNAL SLOTS (a set of slots), which is initially empty."
   The AGENT's, so it is built pre-boot and a flow's appends to it are captured by the heap COW delta — the same
   place and for the same reason as custom_elements.c's backup element queue. */
static JSValue g_signal_slots = JS_UNDEFINED;

#define SLOT_SLOT_FLAGS (JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE)

/* ---- §4.2.2's two kinds ------------------------------------------------------------------------------------- */

bool slot_is(const lxb_dom_node_t *n)
{
    size_t len = 0;
    const lxb_char_t *local;

    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT || n->ns != LXB_NS_HTML) return false;
    local = lxb_dom_element_local_name(lxb_dom_interface_element((lxb_dom_node_t *)n), &len);
    return local != NULL && len == 4 && memcmp(local, "slot", 4) == 0;
}

/* §4.2.2: "A slottable is an Element or a Text node" — the two interfaces whose IDL includes `Slottable`. */
static bool slottable_is(const lxb_dom_node_t *n)
{
    return n != NULL && (n->type == LXB_DOM_NODE_TYPE_ELEMENT || n->type == LXB_DOM_NODE_TYPE_TEXT);
}

/* An element's content attribute in the NULL namespace, as bytes borrowed from lexbor, or "" when absent —
   which is what both of §4.2.2's names are ("Unless stated otherwise it is the empty string"). */
static const char *attr_or_empty(const lxb_dom_node_t *n, const char *name, size_t *len)
{
    const lxb_char_t *v;
    size_t vl = 0;

    *len = 0;
    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return "";
    v = lxb_dom_element_get_attribute(lxb_dom_interface_element((lxb_dom_node_t *)n),
                                      (const lxb_char_t *)name, strlen(name), &vl);
    if (!v) return "";
    *len = vl;
    return (const char *)v;
}

/* §4.2.2's SLOTTABLE NAME — the `slot` content attribute. A Text node has no attributes and so has the empty
   name, which is exactly why an unnamed `<slot>` collects a host's text children. */
static const char *slottable_name(const lxb_dom_node_t *n, size_t *len)
{
    return attr_or_empty(n, "slot", len);
}

/* HTML §4.12.4's SLOT NAME — the `name` content attribute. */
static const char *slot_name(const lxb_dom_node_t *n, size_t *len)
{
    return attr_or_empty(n, "name", len);
}

static bool bytes_eq(const char *a, size_t na, const char *b, size_t nb)
{
    return na == nb && (na == 0 || memcmp(a, b, na) == 0);
}

/* ---- the per-node lists ------------------------------------------------------------------------------------- */

/* A node's list under `key`, MINTED if it has none and `create` — the Array a flow appends to. Reached through
   the node's WRAPPER, which is where per-flow state belongs (see shadow_root.c). OWNED. */
static JSValue slot_list_of(JSContext *ctx, lxb_dom_node_t *n, JSAtom key, bool create)
{
    JSValue wrap, list;

    if (!n) return JS_UNDEFINED;
    wrap = create ? node_wrap(ctx, n) : JS_DupValue(ctx, node_wrap_peek(n));
    if (!JS_IsObject(wrap)) { JS_FreeValue(ctx, wrap); return JS_UNDEFINED; }
    if (JS_GetOwnSlot(ctx, &list, wrap, key) > 0) { JS_FreeValue(ctx, wrap); return list; }
    if (!create) { JS_FreeValue(ctx, wrap); return JS_UNDEFINED; }
    list = JS_NewArray(ctx);
    CHECK(!JS_IsException(list), "a §4.2.2 node list could not be allocated");
    JS_DefinePropertyValue(ctx, wrap, key, JS_DupValue(ctx, list), SLOT_SLOT_FLAGS);
    JS_FreeValue(ctx, wrap);
    return list;
}

static uint32_t list_len(JSContext *ctx, JSValueConst list)
{
    JSValue v;
    uint32_t n = 0;

    if (!JS_IsArray(list)) return 0;
    v = JS_GetPropertyStr(ctx, list, "length");
    JS_ToUint32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

static lxb_dom_node_t *list_node(JSContext *ctx, JSValueConst list, uint32_t i)
{
    JSValue v = JS_GetPropertyUint32(ctx, list, i);
    lxb_dom_node_t *n = node_of(v);

    JS_FreeValue(ctx, v);
    return n;
}

static bool list_contains(JSContext *ctx, JSValueConst list, const lxb_dom_node_t *n)
{
    uint32_t i, len = list_len(ctx, list);

    for (i = 0; i < len; i++)
        if (list_node(ctx, list, i) == n) return true;
    return false;
}

/* A node's own single-valued slot (its assigned slot, its manual slot assignment), or NULL. */
static lxb_dom_node_t *slot_ref_of(JSContext *ctx, const lxb_dom_node_t *n, JSAtom key)
{
    JSValueConst wrap = node_wrap_peek(n);
    JSValue v;
    lxb_dom_node_t *r;

    if (!JS_IsObject(wrap)) return NULL;
    if (JS_GetOwnSlot(ctx, &v, wrap, key) <= 0) return NULL;
    r = node_of(v);
    JS_FreeValue(ctx, v);
    return r;
}

static void slot_ref_set(JSContext *ctx, lxb_dom_node_t *n, JSAtom key, lxb_dom_node_t *slot)
{
    JSValue wrap = node_wrap(ctx, n);

    DCHECK(JS_IsObject(wrap), "a §4.2.2 slot reference was written onto something with no wrapper");
    JS_DefinePropertyValue(ctx, wrap, key, slot ? node_wrap(ctx, slot) : JS_NULL, SLOT_SLOT_FLAGS);
    JS_FreeValue(ctx, wrap);
}

/* ---- §4.2.2.3 finding -------------------------------------------------------------------------------------- */

/* "To FIND A SLOT for a given slottable and an optional boolean open (default false)." */
static lxb_dom_node_t *find_a_slot(JSContext *ctx, lxb_dom_node_t *slottable, bool open)
{
    lxb_dom_node_t *shadow, *n;
    const char *want;
    size_t want_len = 0;

    DCHECK(slottable != NULL, "§4.2.2's find a slot was asked about no node");
    if (!slottable->parent) return NULL;                                            /* step 1 */
    if (slottable->parent->type != LXB_DOM_NODE_TYPE_ELEMENT) return NULL;          /* only an element hosts */
    shadow = shadow_root_of_element(ctx, lxb_dom_interface_element(slottable->parent));
    if (!shadow) return NULL;                                                       /* step 2 */
    if (open && !shadow_root_is_open(shadow)) return NULL;                          /* step 3 */
    if (shadow_root_slot_assignment_is_manual(ctx, shadow)) {                        /* step 4 */
        for (n = node_next_in(shadow, shadow); n; n = node_next_in(n, shadow)) {
            JSValue manual;
            bool has;

            if (!slot_is(n)) continue;
            manual = slot_list_of(ctx, n, g_atom_manual, false);
            has = list_contains(ctx, manual, slottable);
            JS_FreeValue(ctx, manual);
            if (has) return n;
        }
        return NULL;
    }
    /* Step 5: the FIRST slot in tree order whose name is the slottable's name. */
    want = slottable_name(slottable, &want_len);
    for (n = node_next_in(shadow, shadow); n; n = node_next_in(n, shadow)) {
        size_t have_len = 0;
        const char *have;

        if (!slot_is(n)) continue;
        have = slot_name(n, &have_len);
        if (bytes_eq(have, have_len, want, want_len)) return n;
    }
    return NULL;
}

/* "To FIND SLOTTABLES for a given slot slot" — appended into `out` (an Array of wrappers). */
static void find_slottables(JSContext *ctx, lxb_dom_node_t *slot, JSValueConst out)
{
    lxb_dom_node_t *root = node_root(slot), *host, *c;
    uint32_t n = 0;

    if (!shadow_root_is(root)) return;                                              /* step 2 */
    host = lxb_dom_interface_node(shadow_root_host(root));                          /* step 3 */
    if (shadow_root_slot_assignment_is_manual(ctx, root)) {                          /* step 4 */
        JSValue manual = slot_list_of(ctx, slot, g_atom_manual, false);
        uint32_t i, len = list_len(ctx, manual);

        for (i = 0; i < len; i++) {
            lxb_dom_node_t *s = list_node(ctx, manual, i);
            if (s && s->parent == host)
                JS_SetPropertyUint32(ctx, (JSValue)out, n++, node_wrap(ctx, s));
        }
        JS_FreeValue(ctx, manual);
        return;
    }
    for (c = host->first_child; c; c = c->next) {                                   /* step 5 */
        if (!slottable_is(c)) continue;
        if (find_a_slot(ctx, c, false) == slot)
            JS_SetPropertyUint32(ctx, (JSValue)out, n++, node_wrap(ctx, c));
    }
}

/* "To FIND FLATTENED SLOTTABLES for a given slot slot." The standard writes it recursively over NESTED slots,
   and the nesting is the PAGE'S — a component inside a component inside a component — so it is an explicit
   STACK rather than C recursion, exactly like every other page-depth walk in this engine. A stack popped from
   the end, pushed in reverse, visits in the same order the recursion does and needs no splice. */
static void flattened_seed(JSContext *ctx, lxb_dom_node_t *slot, JSValueConst into)
{
    uint32_t n;

    find_slottables(ctx, slot, into);                                               /* step 3 */
    if (list_len(ctx, into) != 0) return;
    /* Step 4: "If slottables is the empty list, then append each slottable CHILD of slot, in tree order" — a
       slot's own children are its FALLBACK content, which is what is rendered when nothing fills it. */
    n = 0;
    {
        lxb_dom_node_t *c;
        for (c = slot->first_child; c; c = c->next)
            if (slottable_is(c)) JS_SetPropertyUint32(ctx, (JSValue)into, n++, node_wrap(ctx, c));
    }
}

static void find_flattened_slottables(JSContext *ctx, lxb_dom_node_t *slot, JSValueConst out)
{
    JSValue stack, seed;
    uint32_t out_n = 0, i, len;

    if (!shadow_root_is(node_root(slot))) return;                                   /* step 2 */
    stack = JS_NewArray(ctx);
    seed = JS_NewArray(ctx);
    CHECK(!JS_IsException(stack) && !JS_IsException(seed),
          "§4.2.2's flattened-slottable walk could not be allocated");
    flattened_seed(ctx, slot, seed);
    len = list_len(ctx, seed);
    for (i = 0; i < len; i++)                                                       /* pushed in REVERSE */
        JS_SetPropertyUint32(ctx, stack, i, JS_GetPropertyUint32(ctx, seed, len - 1 - i));
    JS_FreeValue(ctx, seed);
    while ((len = list_len(ctx, stack)) != 0) {                                     /* step 5 */
        JSValue top = JS_GetPropertyUint32(ctx, stack, len - 1);
        lxb_dom_node_t *n = node_of(top);

        JS_SetPropertyStr(ctx, stack, "length", JS_NewInt32(ctx, (int)len - 1));
        if (n && slot_is(n) && shadow_root_is(node_root(n))) {                      /* step 5.1 */
            JSValue inner = JS_NewArray(ctx);
            uint32_t k, klen;

            CHECK(!JS_IsException(inner), "§4.2.2's nested-slot result could not be allocated");
            flattened_seed(ctx, n, inner);
            klen = list_len(ctx, inner);
            for (k = 0; k < klen; k++)
                JS_SetPropertyUint32(ctx, stack, len - 1 + k,
                                     JS_GetPropertyUint32(ctx, inner, klen - 1 - k));
            JS_FreeValue(ctx, inner);
            JS_FreeValue(ctx, top);
            continue;
        }
        if (n) JS_SetPropertyUint32(ctx, (JSValue)out, out_n++, top);               /* step 5.2 */
        else   JS_FreeValue(ctx, top);
    }
    JS_FreeValue(ctx, stack);
}

/* ---- §4.2.2.5 signalling ------------------------------------------------------------------------------------ */

/* "To SIGNAL A SLOT CHANGE, for a slot slot: append slot to slot's relevant agent's signal slots; queue a
   mutation observer microtask." The set is a SET, so a slot already in it is not appended twice — which is
   what makes several mutations in one task fire one `slotchange` each rather than one per mutation. */
static void signal_a_slot_change(JSContext *ctx, lxb_dom_node_t *slot)
{
    DCHECK(g_ready, "a slot change was signalled before slot_init ran");
    DCHECK(JS_IsArray(g_signal_slots), "§4.2.2.5's signal slots set does not exist");
    if (!list_contains(ctx, g_signal_slots, slot))
        JS_SetPropertyUint32(ctx, g_signal_slots, list_len(ctx, g_signal_slots), node_wrap(ctx, slot));
    /* "QUEUE A MUTATION OBSERVER MICROTASK" — §4.3's, which is the SAME operation with the SAME agent-wide
       flag that a queued mutation record ends in. A second flag here would be a second answer to "is a
       notification already scheduled", and the notification is one algorithm: it delivers every record and
       THEN fires every `slotchange`. */
    mutation_observer_queue_microtask(ctx);
}

/* ---- §4.2.2.4 assigning ------------------------------------------------------------------------------------- */

/* "To ASSIGN SLOTTABLES for a slot slot." */
static void assign_slottables(JSContext *ctx, lxb_dom_node_t *slot)
{
    JSValue found, current;
    uint32_t i, n, m;
    bool same;

    DCHECK(slot_is(slot), "§4.2.2.4's assign slottables was asked about something that is not a slot");
    found = JS_NewArray(ctx);
    CHECK(!JS_IsException(found), "§4.2.2.4's slottable list could not be allocated");
    find_slottables(ctx, slot, found);                                              /* step 1 */
    current = slot_list_of(ctx, slot, g_atom_assigned, true);
    n = list_len(ctx, found);
    m = list_len(ctx, current);
    same = n == m;
    for (i = 0; same && i < n; i++)
        same = list_node(ctx, found, i) == list_node(ctx, current, i);
    if (!same)                                                                      /* step 2 */
        signal_a_slot_change(ctx, slot);
    /* Step 3: "Set slot's assigned nodes to slottables." The Array is REPLACED element-wise rather than
       swapped, so the object identity a parked flow's delta names stays the one it named.
       THE SLOTTABLE THAT LEFT IS UNASSIGNED FIRST, and the standard's own text does not say so: step 4 only
       ever WRITES an assigned slot, so a node removed from the host keeps naming the slot it is no longer in.
       The two halves are one relation — "a slottable is assigned if its assigned slot is non-null", and §4.4's
       get the parent RETURNS that slot instead of the node's parent — so a one-way write puts a detached node's
       event path through a slot that does not hold it. The list being replaced is the previous membership,
       which is exactly the set to check against. */
    for (i = 0; i < m; i++) {
        lxb_dom_node_t *was = list_node(ctx, current, i);
        if (was && !list_contains(ctx, found, was))
            slot_ref_set(ctx, was, g_atom_slot_of, NULL);
    }
    for (i = 0; i < n; i++)
        JS_SetPropertyUint32(ctx, current, i, JS_GetPropertyUint32(ctx, found, i));
    JS_SetPropertyStr(ctx, current, "length", JS_NewInt32(ctx, (int)n));
    for (i = 0; i < n; i++) {                                                       /* step 4 */
        lxb_dom_node_t *s = list_node(ctx, current, i);
        if (s) slot_ref_set(ctx, s, g_atom_slot_of, slot);
    }
    JS_FreeValue(ctx, current);
    JS_FreeValue(ctx, found);
}

/* "To ASSIGN SLOTTABLES FOR A TREE, given a node root: run assign slottables for each slot of root's INCLUSIVE
   descendants, in tree order." */
static void assign_slottables_for_a_tree(JSContext *ctx, lxb_dom_node_t *root)
{
    lxb_dom_node_t *n;

    if (!root) return;
    /* NO GUARD HERE — this is the standard's algorithm and it has none. The optimisation that used to stand at
       this line ("a tree that is not a shadow tree has nothing to assign") is TRUE FOR AN INSERTION and FALSE
       FOR A REMOVAL, which is the one caller that reaches this with a tree outside a shadow root: a slot that
       has just been removed from a shadow tree still HOLDS its assigned nodes, and emptying it is the entire
       point of §4.2.3 remove step 7's second call. The guard skipped exactly that, so the removed slot kept its
       nodes and each of those nodes kept naming it as its assigned slot — which §4.4's get the parent then
       answered with, sending the event path into a detached subtree. The condition belongs at the INSERTION
       call site, where it is a fact about that caller (see slot_insert_steps), and it is stated there. */
    if (slot_is(root)) assign_slottables(ctx, root);
    for (n = node_next_in(root, root); n; n = node_next_in(n, root))
        if (slot_is(n)) assign_slottables(ctx, n);
}

void slot_assign_for_a_tree(JSContext *ctx, lxb_dom_node_t *root)
{
    DCHECK(g_ready, "§4.2.2.4's assign slottables for a tree ran before slot_init");
    DCHECK(shadow_root_is(root), "the parse boundary asked for slot assignment over a tree that is not a shadow "
                                 "tree — the walk below skips every other root, so a caller reaching here with "
                                 "one is asking a question whose answer is empty and does not know it");
    assign_slottables_for_a_tree(ctx, root);
}

/* "To ASSIGN A SLOT, given a slottable slottable." */
static void assign_a_slot(JSContext *ctx, lxb_dom_node_t *slottable)
{
    lxb_dom_node_t *slot = find_a_slot(ctx, slottable, false);

    if (slot) assign_slottables(ctx, slot);
}

/* ---- §4.2.3's triggers -------------------------------------------------------------------------------------- */

/* Does this subtree contain a slot — §4.2.3 remove's step 7 condition, "node has an inclusive descendant that
   is a slot". */
static bool has_inclusive_slot(lxb_dom_node_t *node)
{
    lxb_dom_node_t *n;

    if (slot_is(node)) return true;
    for (n = node_next_in(node, node); n; n = node_next_in(n, node))
        if (slot_is(n)) return true;
    return false;
}

/* Is this slot's assigned-nodes list empty — the condition insert's and remove's "signal a slot change for
   parent" steps share. */
static bool slot_assigned_empty(JSContext *ctx, lxb_dom_node_t *slot)
{
    JSValue list = slot_list_of(ctx, slot, g_atom_assigned, false);
    bool empty = list_len(ctx, list) == 0;

    JS_FreeValue(ctx, list);
    return empty;
}

void slot_insert_steps(JSContext *ctx, lxb_dom_node_t *node, lxb_dom_node_t *parent)
{
    DCHECK(g_ready, "§4.2.3's slot steps ran before slot_init");
    if (!node || !parent) return;
    /* "If parent is a shadow host whose shadow root's slot assignment is 'named' and node is a slottable, then
       ASSIGN A SLOT for node." The manual mode is excluded on purpose: a manually assigned slot's membership is
       decided by `assign()` and nothing else, so an insertion must not silently re-slot the node. */
    if (parent->type == LXB_DOM_NODE_TYPE_ELEMENT && slottable_is(node)) {
        lxb_dom_node_t *shadow = shadow_root_of_element(ctx, lxb_dom_interface_element(parent));
        if (shadow && !shadow_root_slot_assignment_is_manual(ctx, shadow))
            assign_a_slot(ctx, node);
    }
    /* "If parent's root is a shadow root, and parent is a slot whose assigned nodes is the empty list, then run
       SIGNAL A SLOT CHANGE for parent." A slot's own children are its FALLBACK content, so a page that changes
       the fallback of an unfilled slot has changed what is rendered. */
    if (slot_is(parent) && shadow_root_is(node_root(parent)) && slot_assigned_empty(ctx, parent))
        signal_a_slot_change(ctx, parent);
    /* "Run ASSIGN SLOTTABLES FOR A TREE with node's root."
       ONLY WHEN THAT ROOT IS A SHADOW ROOT, and that is a derivation about THE CALL SITES rather than about the
       algorithm, so it is checked once per site: a slot whose root is not a shadow root computes the empty list
       at "find slottables", and a tree being INSERTED holds no slot that already has assigned nodes — an
       insertion is how a node gets into a tree, so nothing in it can have been assigned to anything yet.
       Without the test this runs on every insertion, and a document with no shadow tree in it would walk itself
       once per node.
       THE SECOND SITE IS §4.2.3's MOVE (steps 21-23, which are this text word for word). A moved subtree CAN
       hold a slot that already has assigned nodes, so the derivation had to be re-made for it and it holds for
       a different reason: a slot leaving a shadow tree is cleared by the removal half's step 16 (whose own
       guard is that the OLD parent's root is a shadow root, which is exactly that case), and a slot arriving in
       one has its new root pass this test. So neither direction reaches this line needing what the guard skips. */
    if (shadow_root_is(node_root(node)))
        assign_slottables_for_a_tree(ctx, node_root(node));
}

void slot_removed_steps(JSContext *ctx, lxb_dom_node_t *node, lxb_dom_node_t *parent)
{
    lxb_dom_node_t *assigned;

    DCHECK(g_ready, "§4.2.3's slot steps ran before slot_init");
    if (!node || !parent) return;
    DCHECK(node->parent != parent, "§4.2.3 remove's slot steps ran BEFORE the detach — step 4 recomputes the "
                                   "slot's assigned nodes and would find the node it is removing");
    /* "If node is ASSIGNED, then run assign slottables for node's assigned slot." */
    assigned = slot_ref_of(ctx, node, g_atom_slot_of);
    if (assigned && slot_is(assigned)) assign_slottables(ctx, assigned);
    /* "If parent's root is a shadow root, and parent is a slot whose assigned nodes is the empty list, then run
       signal a slot change for parent." */
    if (slot_is(parent) && shadow_root_is(node_root(parent)) && slot_assigned_empty(ctx, parent))
        signal_a_slot_change(ctx, parent);
    /* "If node has an inclusive descendant that is a slot: run assign slottables for a tree with parent's root,
       and with node." Both, because the removed subtree's slots now hold nothing and the tree it left may have
       a different slot answering for the same name. */
    if (shadow_root_is(node_root(parent)) && has_inclusive_slot(node)) {
        assign_slottables_for_a_tree(ctx, node_root(parent));
        assign_slottables_for_a_tree(ctx, node);
    }
}

void slot_attribute_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el);

    DCHECK(g_ready, "§4.2.2's attribute change steps ran before slot_init");
    if (ns != NULL || !local) return;              /* both steps are stated for the NULL namespace only */
    /* §4.2.2's slottable name change steps: "If element is assigned, then run assign slottables for element's
       assigned slot. Run assign a slot for element." BOTH, and in that order — the first tells the slot it is
       losing the node, the second tells the slot that is gaining it. */
    if (!strcmp(local, "slot") && slottable_is(n)) {
        lxb_dom_node_t *assigned = slot_ref_of(ctx, n, g_atom_slot_of);

        if (assigned && slot_is(assigned)) assign_slottables(ctx, assigned);
        assign_a_slot(ctx, n);
        return;
    }
    /* HTML §4.12.4: a `<slot>`'s `name` decides which slottables it collects, so changing it re-assigns every
       slot of the tree — a rename can move nodes between two slots, and doing only this one leaves the other
       holding a node that is no longer its. */
    if (!strcmp(local, "name") && slot_is(n) && shadow_root_is(node_root(n)))
        assign_slottables_for_a_tree(ctx, node_root(n));
}

/* ---- §4.3 "notify mutation observers" steps 4-5 and 7 — the signal-slots half of the ONE notification ------ */

void slot_change_work_start(SlotChangeWork *w)
{
    int k;
    w->fphase = 0;
    w->i = 0;
    w->set = w->ev = JS_UNDEFINED;
    STEP_CB_FOREACH(w->cb, k) w->cb[k] = JS_UNDEFINED;
}

void slot_change_work_visit(JSContext *ctx, SlotChangeWork *w, JSStepVisit *v)
{
    int k;
    v->val(ctx, &w->set);
    v->val(ctx, &w->ev);
    STEP_CB_FOREACH(w->cb, k) v->val(ctx, &w->cb[k]);
}

void slot_signal_slots_take(JSContext *ctx, SlotChangeWork *w)
{
    uint32_t i, n;

    DCHECK(g_ready, "§4.3's signal slots were taken before slot_init ran");
    DCHECK(JS_IsUndefined(w->set), "§4.3 step 4's clone was taken twice in one notification");
    w->set = JS_NewArray(ctx);
    CHECK(!JS_IsException(w->set), "§4.3 step 4's signalSet clone could not be allocated");
    w->i = 0;
    n = list_len(ctx, g_signal_slots);
    for (i = 0; i < n; i++)                                                          /* step 4 */
        JS_SetPropertyUint32(ctx, w->set, i, JS_GetPropertyUint32(ctx, g_signal_slots, i));
    JS_SetPropertyStr(ctx, g_signal_slots, "length", JS_NewInt32(ctx, 0));           /* step 5 */
}

int slot_change_work_run(JSContext *ctx, SlotChangeWork *w, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    int r;

    DCHECK(JS_IsArray(w->set), "§4.3 step 7 ran before step 4 cloned the signal slots");
    for (;;) {
        JSValue slot;

        if (JS_IsUndefined(w->ev)) {
            if (w->i >= list_len(ctx, w->set)) { JS_FreeValue(ctx, cb_result); return 0; }
            /* §4.3 step 7: `slotchange` BUBBLES and is not cancelable, and it is the engine's own event, so it
               is trusted — which is what a component's listener reads to tell it from one a page dispatched. */
            w->ev = event_new(ctx, "slotchange", true, false);
            CHECK(!JS_IsException(w->ev), "the slotchange event could not be allocated");
        }
        slot = JS_GetPropertyUint32(ctx, w->set, w->i);
        r = event_target_fire_run(ctx, &w->fphase, STEP_CB(w->cb), slot, w->ev, JS_UNDEFINED, cb_result, NULL,
                                  out_cb, out_argc);
        JS_FreeValue(ctx, slot);
        if (r > 0) return r;
        cb_result = JS_UNDEFINED;
        JS_FreeValue(ctx, w->ev);
        w->ev = JS_UNDEFINED;
        w->i++;
    }
}

/* §4.2.2's slottable, as a value-level brand — what `idl_iface_narrow` asks of every `assign()` argument. */
static bool slottable_value_of(JSValueConst v)
{
    return slottable_is(node_of(v));
}

/* ---- HTML §4.12.4's three members and §4.2.9's one ------------------------------------------------------------ */

/* `assignedNodes(options)` / `assignedElements(options)` — magic 0 and 1. Both read `options["flatten"]`, which
   is a dictionary member and therefore the page's code; the declaration performs that read. */
static JSValue js_slot_assigned(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    JSValue out, src;
    uint32_t i, len, k = 0;
    bool elements_only = magic == 1, flatten;

    if (!slot_is(n))
        return JS_ThrowTypeError(ctx, "an HTMLSlotElement member was called on something that is not a slot");
    flatten = argc > 0 && idl_dict_bool(ctx, argv[0], "flatten");
    out = JS_NewArray(ctx);
    CHECK(!JS_IsException(out), "assignedNodes' result could not be allocated");
    if (flatten) {
        src = JS_NewArray(ctx);
        CHECK(!JS_IsException(src), "the flattened slottable list could not be allocated");
        find_flattened_slottables(ctx, n, src);
    } else {
        src = slot_list_of(ctx, n, g_atom_assigned, false);
        if (!JS_IsArray(src)) { JS_FreeValue(ctx, src); return out; }
    }
    len = list_len(ctx, src);
    for (i = 0; i < len; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, src, i);
        lxb_dom_node_t *item = node_of(v);

        if (elements_only && (!item || item->type != LXB_DOM_NODE_TYPE_ELEMENT)) { JS_FreeValue(ctx, v); continue; }
        JS_SetPropertyUint32(ctx, out, k++, v);
    }
    JS_FreeValue(ctx, src);
    return out;
}

/* `assign(...nodes)` — HTML §4.12.4. Its arguments are `(Element or Text)...`, so a non-slottable is a
   TypeError from the conversion and never reaches here. */
static JSValue js_slot_assign(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    JSValue mine;
    uint32_t i, len, k = 0;
    int a;

    (void)magic;
    if (!slot_is(n))
        return JS_ThrowTypeError(ctx, "assign() was called on something that is not a slot");
    mine = slot_list_of(ctx, n, g_atom_manual, true);
    /* Step 1: every node this slot currently holds manually loses its manual slot assignment. */
    len = list_len(ctx, mine);
    for (i = 0; i < len; i++) {
        lxb_dom_node_t *s = list_node(ctx, mine, i);
        if (s) slot_ref_set(ctx, s, g_atom_manual_of, NULL);
    }
    JS_SetPropertyStr(ctx, mine, "length", JS_NewInt32(ctx, 0));
    /* Steps 2-3: a new ORDERED SET — so a node passed twice appears once — and each node is taken away from
       whichever slot held it manually before. */
    for (a = 0; a < argc; a++) {
        lxb_dom_node_t *node = node_of(argv[a]);
        lxb_dom_node_t *prev;

        DCHECK(node != NULL && slottable_is(node),
               "assign() reached its body with something that is not a slottable — the declaration brands every "
               "argument against `(Element or Text)`, so a Comment is a TypeError before step 1");
        if (list_contains(ctx, mine, node)) continue;
        prev = slot_ref_of(ctx, node, g_atom_manual_of);
        if (prev && prev != n && slot_is(prev)) {
            JSValue other = slot_list_of(ctx, prev, g_atom_manual, true);
            uint32_t j, olen = list_len(ctx, other), w = 0;

            for (j = 0; j < olen; j++) {
                JSValue v = JS_GetPropertyUint32(ctx, other, j);
                if (node_of(v) == node) { JS_FreeValue(ctx, v); continue; }
                JS_SetPropertyUint32(ctx, other, w++, v);
            }
            JS_SetPropertyStr(ctx, other, "length", JS_NewInt32(ctx, (int)w));
            JS_FreeValue(ctx, other);
        }
        slot_ref_set(ctx, node, g_atom_manual_of, n);
        JS_SetPropertyUint32(ctx, mine, k++, node_wrap(ctx, node));
    }
    JS_FreeValue(ctx, mine);
    /* Step 5: "Run assign slottables for a tree for this's root." Every slot of that tree, because a node this
       call took away from another slot has to leave that slot's assigned nodes too. */
    assign_slottables_for_a_tree(ctx, node_root(n));
    return JS_UNDEFINED;
}

/* §4.2.9's ONE member: "The assignedSlot getter steps are to return the result of FIND A SLOT given this and
   TRUE." A fresh lookup with the open flag, never the stored assigned slot — a slottable inside a CLOSED
   shadow tree answers null from script while the flattened tree still uses the real slot. */
static JSValue js_slottable_assigned_slot(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *n = node_of(this_val), *slot;

    (void)magic;
    if (!slottable_is(n))
        return JS_ThrowTypeError(ctx, "assignedSlot was read on something that is not a slottable");
    slot = find_a_slot(ctx, n, true);
    return slot ? node_wrap(ctx, slot) : JS_NULL;
}

/* §4.2.2's ASSIGNED SLOT — the STORED one, which is what "a slottable IS ASSIGNED" is defined over ("a
   slottable is assigned if its assigned slot is non-null"). It is deliberately NOT `assignedSlot`: §4.2.9's
   getter re-runs "find a slot" with the open flag so that a closed tree stays hidden from script, and the two
   answers differ for exactly the slottables §2.9's event path cares about most. The callers are engine
   algorithms — a node's get the parent, which returns this slot rather than the parent when it is non-null, and
   the dispatch walk's slot-in-closed-tree bookkeeping — and neither is script. */
lxb_dom_node_t *slot_assigned_slot(JSContext *ctx, const lxb_dom_node_t *n)
{
    if (!n || !slottable_is(n))
        return NULL;
    return slot_ref_of(ctx, n, g_atom_slot_of);
}

/* ---- declaration and installation ----------------------------------------------------------------------------- */

void slot_init(JSContext *ctx)
{
    static const IdlArgType ONE_DICT[1] = { IDL_DICT };
    static const IdlArgType NODES[1] = { IDL_INTERFACE };
    static const IdlDictMember ASSIGNED_OPTS[] = { { "flatten", IDL_BOOLEAN } };  /* AssignedNodesOptions */

    DCHECK(!g_ready, "slot_init ran twice — §4.2.2 is declared once per AGENT");
    g_assigned_key = JS_NewSymbol(ctx, "slot assigned nodes", true);
    g_manual_key = JS_NewSymbol(ctx, "slot manually assigned nodes", true);
    g_slot_of_key = JS_NewSymbol(ctx, "slottable assigned slot", true);
    g_manual_of_key = JS_NewSymbol(ctx, "slottable manual slot assignment", true);
    CHECK(JS_IsSymbol(g_assigned_key) && JS_IsSymbol(g_manual_key) && JS_IsSymbol(g_slot_of_key) &&
          JS_IsSymbol(g_manual_of_key), "§4.2.2's slot keys could not be minted");
    g_atom_assigned = JS_ValueToAtom(ctx, g_assigned_key);
    g_atom_manual = JS_ValueToAtom(ctx, g_manual_key);
    g_atom_slot_of = JS_ValueToAtom(ctx, g_slot_of_key);
    g_atom_manual_of = JS_ValueToAtom(ctx, g_manual_of_key);
    /* THE AGENT'S SIGNAL SLOTS, built pre-boot so it belongs to the BASELINE: a flow's appends are then
       captured by the heap COW delta. A set allocated lazily inside a flow would be that flow's private object
       and no sibling would ever see a slot it signalled. */
    g_signal_slots = JS_NewArray(ctx);
    CHECK(!JS_IsException(g_signal_slots), "§4.2.2.5's signal slots set could not be allocated");
    /* HTML §4.12.4 The slot element writes both of these with the argument OPTIONAL —
       `sequence<Node> assignedNodes(optional AssignedNodesOptions options = {})` — and that word was in the
       comment beside ASSIGNED_OPTS above and in no code. The declaration therefore said position 0 was
       REQUIRED, and Web IDL §3.6 Overload resolution algorithm's step 5 ("If S is empty, then throw a
       TypeError", reached because step 4 removes every entry whose type list is not of length argcount) makes
       that a throw: `slot.assignedNodes()` — the ordinary call, the one every page writes — was a TypeError in
       this engine and is a sequence in every browser.
       It is also the number Web IDL §3.7.7 Operations' `length` is computed from, so the two members' 0 on
       their prototypes was right by accident while their declaration said 1. */
    g_id_assigned_nodes = idl_method_id_dict(ctx, ONE_DICT, 1, ASSIGNED_OPTS, 1, js_slot_assigned, 0);
    idl_optional_from(0);
    g_id_assigned_elements = idl_method_id_dict(ctx, ONE_DICT, 1, ASSIGNED_OPTS, 1, js_slot_assigned, 1);
    idl_optional_from(0);
    /* `undefined assign((Element or Text)... nodes)` — a VARIADIC interface-typed tail, so the declaration
       brands every argument against the node class and the body's own test narrows it to §4.2.2's two kinds. */
    g_id_assign = idl_method_id_ext(ctx, NODES, 1, true, node_class_id(), js_slot_assign, 0);
    /* `(Element or Text)` is narrower than "a Node", and a class id cannot say so — §4.2.2's slottable is those
       two kinds and a Comment passed to `assign()` is a TypeError before step 1, not something the body sorts
       out afterwards. */
    idl_iface_narrow(slottable_value_of);
    g_ready = 1;
}

void slot_install_slot_members(JSContext *ctx, JSValueConst slot_proto)
{
    DCHECK(g_ready, "HTML §4.12.4's members were installed before slot_init ran");
    idl_install_method(ctx, slot_proto, "assignedNodes", 0, g_id_assigned_nodes);
    idl_install_method(ctx, slot_proto, "assignedElements", 0, g_id_assigned_elements);
    idl_install_method(ctx, slot_proto, "assign", 0, g_id_assign);
}

void slot_install_slottable_mixin(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_ready, "§4.2.9's mixin was installed before slot_init ran");
    idl_install_accessor(ctx, proto, "assignedSlot", js_slottable_assigned_slot, 0, -1);
}

/* RELEASED BY ITS DECLARER — §4.2.2 is declared from document_init, and was being released from element_free's
   cascade instead. It is reached from document_agent_free now. */
void slot_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;` — the release is the inverse of a declaration that is unconditional, so the
       test could never be true and could only hide a release that had not finished. */
    DCHECK(g_ready, "§4.2.2's slot machinery was released in an agent that never declared it");
    JS_FreeValueRT(rt, g_signal_slots);
    g_signal_slots = JS_UNDEFINED;
    JS_FreeAtomRT(rt, g_atom_assigned);
    JS_FreeAtomRT(rt, g_atom_manual);
    JS_FreeAtomRT(rt, g_atom_slot_of);
    JS_FreeAtomRT(rt, g_atom_manual_of);
    g_atom_assigned = g_atom_manual = g_atom_slot_of = g_atom_manual_of = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_assigned_key);
    JS_FreeValueRT(rt, g_manual_key);
    JS_FreeValueRT(rt, g_slot_of_key);
    JS_FreeValueRT(rt, g_manual_of_key);
    g_assigned_key = g_manual_key = g_slot_of_key = g_manual_of_key = JS_UNDEFINED;
    g_id_assigned_nodes = g_id_assigned_elements = g_id_assign = -1;
    g_ready = 0;
}
