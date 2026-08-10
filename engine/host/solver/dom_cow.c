/* Per-flow DOM COW delta — see dom_cow.h. */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "check.h"        /* CHECK — an OOM here corrupts DOM isolation, fatal in every build */
#include "solver/dom_cow.h"
#include "core/dom/node.h"   /* node_wrap_forget — a destroyed node hands back its wrapper */
#include "core/dom/document.h"   /* document_record_release — a destroyed document hands back its record */
#include "core/dom/attr_list.h"   /* §4.9's attribute-list algorithms — what the delta restores an attribute THROUGH */
#include "solver/attr_shadow.h"   /* the taint shadow rides the attribute delta (per-flow isolation of stashed taint) */
#include <lexbor/dom/dom.h>

typedef struct DomUndo {
    /* kind 0 covers a NAMED STRING SLOT on an element, and `slot` says which kind of slot it is:
       ATTR_SLOT_ATTRIBUTE (a content attribute, whose VALUE lives in the Lexbor tree and whose TAINT lives in
       the shadow) or ATTR_SLOT_PROPERTY (a DOM property like textContent, whose value is already captured as
       Text NODES by kinds 1/2, so only its taint is here). One entry kind because it is one concept — without
       it a `el.textContent = location.hash` in one arm was visible to every other arm, since the shadow write
       bypassed the delta entirely.
       AN ATTRIBUTE ENTRY NAMES THE ATTRIBUTE THE WAY §4.9 DOES: (ns, name) is (namespace, LOCAL name), which is
       the identity every namespace-aware algorithm is keyed on, and `prefix` is what a restore needs to rebuild
       the qualified name when it has to CREATE the attribute again. Keyed on the qualified name instead, a
       revert of `xlink:href` could only write a qualified name back — producing a SECOND attribute in the null
       namespace wearing that name, which is not the attribute the baseline had. `ns`/`prefix` are NULL for the
       null namespace and for a PROPERTY slot, which has neither. */
    int kind; int slot; lxb_dom_element_t *el; char *ns; char *prefix; char *name;
    lxb_char_t *old; size_t old_len; int had;                 /* kind 0: baseline attr VALUE */
    /* kind 0: THE ATTRIBUTE NODE ITSELF, and where in the list it sat. §9.4.7's "remove an attribute" clears
       the attribute's element and NOTHING ELSE — the node survives, and `removeAttributeNode` returns it — so a
       restore RE-ATTACHES THE SAME NODE rather than building a replacement out of (ns, prefix, local, value).
       Rebuilt, `el.getAttributeNode("x")` held across a context switch would come back a different object, and
       `attributes[0] === attributes[0]` would stop holding across a park. `attr_next` is the attribute it sat
       BEFORE at capture time, which is what puts it back at the index the page read it from; a reverse-order
       restore always finds that successor attached, because an attribute removed EARLIER cannot be the
       successor recorded later. */
    lxb_dom_attr_t *attr, *attr_next;
    lxb_char_t *cur; size_t cur_len; int cur_had;             /* kind 0: flow's attr VALUE (valid while parked) */
    lxb_dom_attr_t *cur_attr, *cur_attr_next;                 /* kind 0: the NODE serving it while parked */
    /* WHOSE TAINT SHADOW THIS ENTRY RESTORES. The element for kinds 0, and the ATTR NODE for kind 7 — an
       attribute with no element keys its shadow on itself (see attr_shadow.h), so the field that names the
       shadow's owner is its own rather than `el` reinterpreted. */
    const void *sh_owner;
    JSValue sh_old; int sh_had;                               /* kind 0: baseline attr TAINT shadow (opaque, or none) */
    JSValue sh_cur; int sh_cur_had;                           /* kind 0: flow's attr TAINT shadow (valid while parked) */
    lxb_dom_node_t *node, *parent, *next;                     /* kind 1/2: the node + its position */
    int detached;                                             /* kind 1: currently detached (unapplied) */
    /* kind 2: a node the BASELINE had and this flow REMOVED. The mirror of kind 1 in every direction: revert
       and unapply put it back, apply takes it away again. innerHTML= is what needs it — it REPLACES the
       children, and without a removal capture the old subtree would survive into the new markup and leak
       across a context switch into a sibling flow that never removed it. */
    int reinserted;                                           /* kind 2: currently back in the tree (unapplied) */
    /* kind 6: an ATTRIBUTE this flow created — kind 4's twin, and a separate kind because both halves of what
       a creation entry does differ. "Is it still reachable?" is `owner != NULL` for an attribute and not
       `dom_is_attached` (an Attr's parent is always null, so kind 4's test would call an attribute sitting in
       an element's list unattached and free it out from under that list), and the free is the Attr
       destructor rather than a deep tree walk. The node it names is `node`, reached as an attribute. */
    /* kind 5: a whole DOCUMENT this flow created (§4.5.1's createHTMLDocument / createDocument). Its own field
       rather than `node` reused, because what has to be destroyed is the lxb_html_document_t and not the node
       at its root — a different destructor, over a different allocation, and a type pun through the node would
       compile and free the wrong thing. */
    lxb_html_document_t *doc;
} DomUndo;

static DomUndo *g_dom_undo = NULL;
static int g_dom_undo_n = 0, g_dom_undo_cap = 0;
int g_dom_capture = 0;
static JSContext *g_cow_ctx = NULL;   /* for the shadow's JSValue dup/free (set at init) */

/* The DOM half of the persistent-versioned-heap: a mutable HEAD (g_dom_undo) layered over a chain of IMMUTABLE,
   refcounted, structurally-shared base segments (DomSeg), exactly mirroring the JS heap's CowSeg chain. A fork
   (dom_cow_fork) freezes the head into a segment BOTH the running flow and a snapshot-forked sibling reference
   (refcount 2), so a continuation/sibling SHARES the parent's O(N) DOM delta in O(1) instead of copying it. NULL
   until the first fork -> the base-chain walks are no-ops and behaviour is byte-identical to the flat buffer. */
typedef struct DomSeg { DomUndo *e; int n; struct DomSeg *base; int refcount; } DomSeg;
static DomSeg *g_dom_base = NULL;

/* THE CHAIN THAT IS CURRENTLY APPLIED TO THE DOCUMENT, and the whole reason a switch is not O(delta).
 *
 * The segments are already immutable, refcounted and structurally SHARED — two flows that forked from a common
 * point hold the same suffix by POINTER — and the swap threw all of that away: it unapplied the outgoing flow's
 * entire chain and applied the incoming flow's entire chain, redoing by hand the part they agree on. For a flow
 * that had appended three hundred nodes that is six hundred node detach/re-attach operations per switch, and
 * the scheduler switches often, so the cost of one scheduling DECISION grew with the size of the document —
 * exactly backwards for an engine whose whole point is interleaving flows over a real page.
 *
 * What is applied is a property of the DOCUMENT, not of any flow, so it is tracked here: everything from
 * `g_dom_installed` downward is live in the tree. A switch moves that pointer to the incoming flow's chain by
 * walking to the two chains' lowest common segment and touching only what lies above it. The shared suffix is
 * never unapplied and never re-applied, because both flows agree about it by construction — a segment is frozen
 * at a fork and immutable after, so any two holders read the same values from it.
 * The HEAD is different and is always swapped: it is mutable and private to one flow. */
static DomSeg *g_dom_installed = NULL;
static void dom_unapply_seg(DomSeg *s);   /* fwd: dom_revert (defined earlier) walks the base chain */
static void dom_seg_unref(DomSeg *s);
static void dom_note_created_attr(lxb_dom_attr_t *a);   /* fwd: the chokepoint is above the creation records */

void dom_cow_set_ctx(JSContext *ctx) { g_cow_ctx = ctx; }

/* §4.2.3's insertion/removing steps. Fired from the chokepoint so a tree write cannot reach the tree without
   them: the browser layer registers what they MEAN and this file guarantees they run.
 *
 * THE GUARANTEE WAS `if (g_tree_hook && g_cow_ctx)`, AND THAT SECOND TERM UNDID IT. A host that registered the
 * hook but never named its context ran NO insertion steps and NO removing steps at all — no <script>
 * preparation, no custom-element upgrade, no §4.8.5 child navigable — and nothing said so, because a skipped
 * step looks exactly like a tree with nothing in it that needed one. That is what the WPT runner did for its
 * whole life: `document.body.appendChild(iframe)` produced an element with no navigable, and the failure
 * surfaced three layers away as `contentWindow` being null. The context is now ASSERTED rather than tested, and
 * the hook is called unconditionally, so a host that forgets crashes at the write instead of quietly running a
 * different DOM. */
static void (*g_tree_hook)(JSContext *ctx, lxb_dom_node_t *n, int inserted);

#define TREE_HOOK_NO_CTX \
    "a tree write reached the insertion/removing steps with no context — dom_cow_set_ctx names the runtime " \
    "they run in, and a host that registers the hook without it runs NO insertion steps at all: no <script> " \
    "preparation, no custom-element upgrade, no child navigable, and nothing to say so"

void dom_cow_set_tree_hook(void (*fn)(JSContext *ctx, lxb_dom_node_t *n, int inserted)) { g_tree_hook = fn; }

static void (*g_attr_hook)(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local,
                           const char *val, size_t val_len);
void dom_cow_set_attr_hook(void (*fn)(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local,
                                      const char *val, size_t val_len)) { g_attr_hook = fn; }

/* the current taint shadow for this slot identity, dup'd (JS_UNDEFINED + *had=0 if none) */
static JSValue shadow_snapshot(const void *owner, int slot, const char *ns, const char *name, int *had) {
    int si = attr_shadow_find(owner, slot, ns, name);
    *had = (si >= 0);
    return (si >= 0 && g_cow_ctx) ? JS_DupValue(g_cow_ctx, attr_shadow_opaque(si)) : JS_UNDEFINED;
}
/* set this slot identity's taint shadow to `v` (borrowed; attr_shadow_set dups it), or clear it when !had */
static void shadow_restore(const void *owner, int slot, const char *ns, const char *name,
                           JSValueConst v, int had) {
    if (g_cow_ctx) attr_shadow_set(g_cow_ctx, owner, slot, ns, name, had ? v : JS_UNDEFINED);
}

/* THE DELTA'S TWO ATTRIBUTE PRIMITIVES, over the identity an entry holds. Both go through §4.9's attribute-list
   algorithms rather than lexbor's qualified-name shortcuts, which is the whole point of storing the identity:
   a restore puts the attribute back where it was, in the namespace it was in. */
static void attr_put(const DomUndo *u, lxb_dom_attr_t *a, lxb_dom_attr_t *before,
                     const lxb_char_t *val, size_t len) {
    DCHECK(a != NULL, "an attribute delta says this identity was PRESENT and names no attribute node to put "
                      "back — presence is recorded with the node that served it, so the two cannot disagree");
    if (!a->owner) {
        /* (namespace, local name) is §4.9's UNIQUENESS key on the list, so whatever is serving this identity
           now has to come out before the restored node goes in. `setAttributeNode` is what makes that a real
           case rather than a defensive one: it REPLACES an attribute with a different Attr object at the same
           identity, and the baseline's node is the one this entry has to put back. */
        lxb_dom_attr_t *occupant = dom_attr_get_ns(u->el, u->ns, u->name);
        if (occupant) dom_attr_detach(occupant);
        dom_attr_attach(u->el, a, (before && before->owner == u->el) ? before : NULL);
    }
    dom_attr_set_value(a, (const char *)(val ? val : (const lxb_char_t *)""), len);
}
static void attr_drop(const DomUndo *u) {
    lxb_dom_attr_t *a = dom_attr_get_ns(u->el, u->ns, u->name);
    if (a) dom_attr_detach(a);   /* §9.4.7: the node survives — the delta that CREATED it is what frees it */
}

static void dom_undo_push(DomUndo u) {
    if (g_dom_undo_n >= g_dom_undo_cap) {
        int nc = g_dom_undo_cap ? g_dom_undo_cap * 2 : 64;
        /* Skipping a DOM capture silently breaks DOM isolation (this flow's DOM write never reverts -> leaks
           into the next flow's baseline -> wrong sinks/taint). OOM is a should-never-happen: CRASH, never skip. */
        DomUndo *n = realloc(g_dom_undo, (size_t)nc * sizeof(DomUndo));
        CHECK(n, "dom-cow-oom: DOM undo-log realloc failed — DOM isolation would be silently corrupted");
        g_dom_undo = n; g_dom_undo_cap = nc;
    }
    g_dom_undo[g_dom_undo_n++] = u;
}
/* Record (element, namespace, local name)'s BASELINE — value and taint together, because a restore that put one
   back without the other would hand a sink either clean bytes that are attacker input or a stale provenance on
   a value that never had it.
   THE PREFIX IS READ OFF THE ATTRIBUTE, NEVER TAKEN FROM THE CALLER. It is only ever consulted by a restore
   that has to CREATE the attribute again, which happens exactly when the baseline HAD one — so the prefix that
   restore must rebuild is that attribute's own, and the caller's is about the attribute it is on its way to
   write. Taken from the caller, `removeAttributeNS(XLINK, "href")` (which has no prefix to pass) reverted to an
   XLink `href` with no prefix at all: the same attribute by §4.9's identity, printing `href` where the page's
   markup said `xlink:href`. */
static void dom_attr_capture(lxb_dom_element_t *el, const char *ns, const char *local) {
    lxb_dom_attr_t *a;
    size_t vl = 0, pl = 0;
    const lxb_char_t *cur, *prefix;
    DomUndo u;

    if (!g_dom_capture) return;
    a = dom_attr_get_ns(el, ns, local);
    cur = a ? lxb_dom_attr_value(a, &vl) : NULL;
    prefix = a ? dom_attr_prefix(a, &pl) : NULL;
    memset(&u, 0, sizeof u);
    u.kind = 0; u.slot = ATTR_SLOT_ATTRIBUTE; u.el = el; u.had = a ? 1 : 0;
    u.attr = a; u.attr_next = a ? a->next : NULL;   /* the node, and the index it sat at */
    u.sh_owner = el;
    u.name = strdup(local);
    CHECK(u.name, "dom-cow-oom: attr local-name strdup failed");
    if (ns) { u.ns = strdup(ns); CHECK(u.ns, "dom-cow-oom: attr namespace strdup failed"); }
    if (prefix) {
        u.prefix = malloc(pl + 1);
        CHECK(u.prefix, "dom-cow-oom: attr prefix copy failed");
        memcpy(u.prefix, prefix, pl); u.prefix[pl] = 0;
    }
    if (cur) { u.old = malloc(vl ? vl : 1); CHECK(u.old, "dom-cow-oom: baseline attr snapshot malloc failed — the delta could not restore its baseline"); memcpy(u.old, cur, vl); u.old_len = vl; }
    u.sh_old = shadow_snapshot(el, ATTR_SLOT_ATTRIBUTE, ns, local, &u.sh_had);   /* baseline TAINT reverts with the value */
    u.sh_cur = JS_UNDEFINED;
    dom_undo_push(u);
}

/* The PROPERTY-slot twin of dom_attr_capture: a DOM property's taint, whose value half is already captured as
   Text nodes. Same entry kind, same revert/unapply/apply arms — only the Lexbor value work is skipped. */
static void dom_prop_taint_capture(lxb_dom_element_t *el, const char *name) {
    if (!g_dom_capture) return;
    DomUndo u; memset(&u, 0, sizeof u);
    u.kind = 0; u.slot = ATTR_SLOT_PROPERTY; u.el = el; u.sh_owner = el; u.name = strdup(name); u.had = 0;
    CHECK(u.name, "dom-cow-oom: property name strdup failed");
    u.sh_old = shadow_snapshot(el, ATTR_SLOT_PROPERTY, NULL, name, &u.sh_had);
    u.sh_cur = JS_UNDEFINED;
    dom_undo_push(u);
}

/* THE PROPERTY-TAINT CHOKEPOINT — capture-then-set, like every other DOM write. `opaque` JS_UNDEFINED clears. */
void dom_cow_set_prop_taint(JSContext *ctx, lxb_dom_element_t *el, const char *name, JSValueConst opaque) {
    dom_prop_taint_capture(el, name);
    attr_shadow_set(ctx, el, ATTR_SLOT_PROPERTY, NULL, name, opaque);
}
void dom_insert_capture(lxb_dom_node_t *node) {
    if (!g_dom_capture) return;
    DomUndo u; memset(&u, 0, sizeof u); u.kind = 1; u.node = node; u.sh_old = u.sh_cur = JS_UNDEFINED; dom_undo_push(u);
}
/* §4.9 "get an attribute by name" RESOLVED TO AN IDENTITY: which attribute a qualified name means is a question
   about the element's current attribute list, so it is asked HERE, once, rather than by each caller inventing a
   key that the delta and the taint shadow then have to agree about.
   An element with no such attribute is the setAttribute case — §4.9 step 5 creates one whose local name IS the
   qualified name, in the null namespace — which is exactly the identity this returns.
   OWNED COPIES, not pointers into the tree: lexbor's intern table stores a string with its length beside it and
   NUL-terminates only the short ones, and the delta's key outlives the attribute anyway (a removal frees it and
   the entry still has to name it on revert). No fixed buffer: a name's length is the page's data. */
typedef struct { char *ns; char *prefix; char *local; } AttrIdent;

static char *dupn(const lxb_char_t *s, size_t n) {
    char *p = malloc(n + 1);
    CHECK(p != NULL, "dom-cow-oom: an attribute identity could not be copied");
    memcpy(p, s, n); p[n] = 0;
    return p;
}

static void attr_ident_of(lxb_dom_element_t *el, const char *qname, AttrIdent *id) {
    lxb_dom_attr_t *a = dom_attr_get_qname(el, qname);
    const lxb_char_t *s;
    size_t len = 0;

    id->ns = id->prefix = NULL;
    if (!a) { id->local = dupn((const lxb_char_t *)qname, strlen(qname)); return; }
    if ((s = dom_attr_ns(a, &len)) != NULL) id->ns = dupn(s, len);
    if ((s = dom_attr_prefix(a, &len)) != NULL) id->prefix = dupn(s, len);
    s = lxb_dom_attr_local_name(a, &len);
    id->local = dupn(s, len);
}
/* THE SAME IDENTITY READ OFF AN ATTRIBUTE NODE, which is what the node-valued operations are given: they name
   the attribute by object, and every delta entry and every shadow key is over (namespace, local name). */
static void attr_ident_of_node(const lxb_dom_attr_t *a, AttrIdent *id) {
    const lxb_char_t *s;
    size_t len = 0;

    id->ns = id->prefix = NULL;
    if ((s = dom_attr_ns(a, &len)) != NULL) id->ns = dupn(s, len);
    if ((s = dom_attr_prefix(a, &len)) != NULL) id->prefix = dupn(s, len);
    s = lxb_dom_attr_local_name((lxb_dom_attr_t *)a, &len);
    DCHECK(s != NULL, "an attribute carries no local name — §4.9.1 says a local name is a non-empty string");
    id->local = dupn(s, len);
}
static void attr_ident_free(AttrIdent *id) { free(id->ns); free(id->prefix); free(id->local); }

/* THE DETACHED NODE'S VALUE, as a delta entry over the NODE — kind 3's shape (a value in place on a node whose
   identity must survive the write) over an Attr instead of a CharacterData. Pushed by the DETACH, which is the
   moment a shared attribute's value stops being reachable through its element and starts being reachable only
   through the object the page is holding. */
static void dom_attr_node_value_capture(lxb_dom_attr_t *a, const AttrIdent *id) {
    DomUndo u;
    size_t vl = 0;
    const lxb_char_t *v;

    if (!g_dom_capture) return;
    memset(&u, 0, sizeof u);
    u.kind = 7; u.node = lxb_dom_interface_node(a); u.sh_old = u.sh_cur = JS_UNDEFINED;
    v = lxb_dom_attr_value(a, &vl);
    if (v) {
        u.old = malloc(vl ? vl : 1);
        CHECK(u.old, "dom-cow-oom: a detached attribute's baseline value could not be snapshotted");
        memcpy(u.old, v, vl); u.old_len = vl;
    }
    u.had = 1;
    /* AND ITS TAINT, which is keyed on the NODE while it has no element — the value and its provenance are one
       piece of state and a restore that put one back without the other hands a sink either clean bytes that
       are attacker input or a stale provenance on a value that never had it. */
    u.slot = ATTR_SLOT_ATTRIBUTE; u.sh_owner = a;
    if (id->ns) { u.ns = strdup(id->ns); CHECK(u.ns, "dom-cow-oom: detached attr namespace strdup failed"); }
    u.name = strdup(id->local);
    CHECK(u.name, "dom-cow-oom: detached attr local-name strdup failed");
    u.sh_old = shadow_snapshot(a, ATTR_SLOT_ATTRIBUTE, id->ns, id->local, &u.sh_had);
    dom_undo_push(u);
}

/* THE DOM-mutation CHOKEPOINT (see dom_cow.h): capture the baseline THEN mutate, so a write cannot bypass the
   per-flow delta. Every browser-component attribute write funnels through here — the capture-before-mutate order
   is guaranteed in ONE place, not re-remembered at each call site.
 *
 * VALUE AND TAINT ARE ONE WRITE. They were two calls every caller had to make in agreement, over a key each
 * computed for itself, and a caller that made one and not the other left a stale taint on a fresh value (a sink
 * reading clean bytes as attacker input) or dropped the provenance of a stored source. `toggleAttribute(name,
 * true)` was the second of those: it wrote the empty string and left whatever taint the attribute had. One call
 * resolves the identity once and cannot disagree with itself. `taint` is JS_UNDEFINED for a concrete write. */
void dom_cow_set_attribute_ns(lxb_dom_element_t *el, const char *ns, const char *prefix, const char *local,
                              const char *val, size_t val_len, JSValueConst taint) {
    dom_attr_capture(el, ns, local);   /* baseline into the running flow's delta FIRST (no-op if !g_dom_capture) */
    /* BEFORE the write: the change steps take the OLD value, which the element still holds only until now. */
    if (g_attr_hook && g_cow_ctx) g_attr_hook(g_cow_ctx, el, ns, local, val, val_len);
    {
        bool created = false;
        lxb_dom_attr_t *a = dom_attr_write(el, ns, prefix, local, val, val_len, &created);
        /* AN ATTRIBUTE A FLOW CREATES IS THE FLOW'S, and the write is the one place that knows it made one. */
        if (created) dom_note_created_attr(a);
    }
    if (g_cow_ctx) attr_shadow_set(g_cow_ctx, el, ATTR_SLOT_ATTRIBUTE, ns, local, taint);
}
/* Remove an ATTRIBUTE the baseline may own — the fourth thing a flow can change about the tree, and the one
   that had no chokepoint, so a boolean reflection (`el.hidden = false`, `script.async = false`) had no way to
   unset itself without going around the per-flow delta. Same capture-then-mutate order as the setter, and the
   taint goes with the value because there is no value left for it to describe. */
void dom_cow_remove_attribute_ns(lxb_dom_element_t *el, const char *ns, const char *local) {
    /* §4.9 "remove an attribute by namespace and local name" step 2: the removal — and therefore the change
       steps, the delta entry and the shadow clear — happen ONLY when there is an attribute to remove. The guard
       is here rather than in the by-name wrapper because §4.9's key is this one: a qualified name that matches
       nothing still resolves to an identity, and the question "is there one" is only answerable at the key the
       list is unique over. */
    lxb_dom_attr_t *a = dom_attr_get_ns(el, ns, local);
    if (!a) return;
    dom_cow_remove_attribute_node(a);
}
/* §4.9 "REMOVE AN ATTRIBUTE" OVER THE NODE ITSELF, which is what `removeAttributeNode` and `removeNamedItem`
   are defined on: they find the attribute by IDENTITY (a containment test on the list), not by a name. */
void dom_cow_remove_attribute_node(lxb_dom_attr_t *a) {
    lxb_dom_element_t *el;
    AttrIdent id;

    DCHECK(a != NULL, "an attribute-node removal was asked for no attribute");
    el = a->owner;
    if (!el) return;   /* §9.4.7 over an attribute whose element is already null */
    attr_ident_of_node(a, &id);
    dom_attr_capture(el, id.ns, id.local);
    if (g_attr_hook && g_cow_ctx) g_attr_hook(g_cow_ctx, el, id.ns, id.local, NULL, 0);
    DCHECK(g_cow_ctx != NULL, "an attribute removal ran with no context named — the taint shadow that goes with "
                              "the value is held as JSValues (dom_cow_set_ctx names the runtime)");
    /* THE DETACHED NODE'S VALUE BECOMES REACHABLE STATE, so it is captured HERE and not at the write that
       changes it. §9.4.7 leaves the removed attribute alive with its value, and a page can write that value
       while it sits detached (`const a = el.removeAttributeNode(x); a.value = "..."`). The node is SHARED —
       another flow's world still has it in the element's list — so the write has to revert; recording the
       baseline at the detach makes every later write to it free, and leaves an attribute the flow CREATED
       (which no other flow can name) with no entry at all, which is the invariant that keeps a delta O(shared
       state touched). */
    dom_attr_detach(a);                                /* §9.4.7: the node survives, detached */
    dom_attr_node_value_capture(a, &id);
    /* THE TAINT GOES WITH THE ATTRIBUTE, not with the element it left. `el.removeAttributeNode(x)` hands the
       page an object still holding the source that was written into it, and a later `el.setAttributeNode(x)`
       puts that source back into the DOM — so the provenance is re-keyed onto the node rather than dropped. */
    if (g_cow_ctx) {
        int si = attr_shadow_find(el, ATTR_SLOT_ATTRIBUTE, id.ns, id.local);
        if (si >= 0) attr_shadow_set(g_cow_ctx, a, ATTR_SLOT_ATTRIBUTE, id.ns, id.local, attr_shadow_opaque(si));
        attr_shadow_set(g_cow_ctx, el, ATTR_SLOT_ATTRIBUTE, id.ns, id.local, JS_UNDEFINED);
    }
    attr_ident_free(&id);
}
/* §4.9 "SET AN ATTRIBUTE" STEPS 6 AND 7 — the list write with a NODE the caller supplies, which is what
   `setAttributeNode` and `setNamedItem` do and what no name-keyed setter can express: the page's own Attr
   object has to BE the attribute afterwards, so this cannot go through a create.
   ONE change notification either way — §9.4.8 step 6 reports a REPLACE against the OLD attribute's name and
   namespace with the NEW attribute's value, and the two agree on that identity by construction. */
void dom_cow_put_attribute_node(lxb_dom_element_t *el, lxb_dom_attr_t *a, JSValueConst taint) {
    lxb_dom_attr_t *old;
    AttrIdent id;
    size_t vl = 0;
    const lxb_char_t *v;

    DCHECK(el && a, "an attribute-node write was asked for no element or no attribute");
    DCHECK(a->owner == NULL || a->owner == el,
           "an attribute already on ANOTHER element reached the list write — §4.9's \"set an attribute\" step 2 "
           "throws InUseAttributeError for that, so it never gets here");
    attr_ident_of_node(a, &id);
    old = dom_attr_get_ns(el, id.ns, id.local);
    if (old != a) {
        dom_attr_capture(el, id.ns, id.local);
        v = lxb_dom_attr_value(a, &vl);
        if (g_attr_hook && g_cow_ctx)
            g_attr_hook(g_cow_ctx, el, id.ns, id.local, (const char *)(v ? v : (const lxb_char_t *)""), vl);
        if (old) dom_attr_replace(old, a);             /* step 6 — in place, keeping the index */
        else dom_attr_attach(el, a, NULL);             /* step 7 */
        if (old) dom_attr_node_value_capture(old, &id);   /* the displaced node is reachable and now detached */
        if (g_cow_ctx) {
            /* THE ATTRIBUTE BRINGS ITS OWN TAINT. `a.value = location.hash` on a detached Attr keyed the
               source on the node; attaching it is what puts that source into the DOM, so the node's shadow is
               what this identity carries afterwards — the caller's `taint` is only for an attribute that never
               had one. It is not cleared off the node: a revert detaches `a` again, and its provenance has to
               still be there when it does. */
            int si = attr_shadow_find(a, ATTR_SLOT_ATTRIBUTE, id.ns, id.local);
            attr_shadow_set(g_cow_ctx, el, ATTR_SLOT_ATTRIBUTE, id.ns, id.local,
                            si >= 0 ? attr_shadow_opaque(si) : taint);
        }
    }
    attr_ident_free(&id);
}
/* §4.9.2 "create an attribute" — the delta owns what the flow creates, exactly as it owns a created node. */
lxb_dom_attr_t *dom_cow_create_attribute(lxb_dom_document_t *doc, const char *ns, const char *prefix,
                                         const char *local) {
    lxb_dom_attr_t *a = dom_attr_create(doc, ns, prefix, local);
    dom_note_created_attr(a);
    return a;
}
/* §4.9.2 "set an existing attribute value" step 1/4's arm — the attribute's element is NULL. No capture: a
   detached attribute is either one this flow CREATED (which no other flow can name) or one this flow DETACHED
   from an element, and the detach already recorded its baseline value. */
void dom_cow_set_detached_attr_value(lxb_dom_attr_t *a, const char *val, size_t val_len, JSValueConst taint) {
    AttrIdent id;

    DCHECK(a != NULL, "a detached attribute value write was asked for no attribute");
    DCHECK(a->owner == NULL, "a detached-attribute write was made to an attribute that is ON an element — its "
                             "value is keyed state there, and the write has to go through the identity");
    dom_attr_set_value(a, val, val_len);
    attr_ident_of_node(a, &id);
    if (g_cow_ctx) attr_shadow_set(g_cow_ctx, a, ATTR_SLOT_ATTRIBUTE, id.ns, id.local, taint);
    attr_ident_free(&id);
}


void dom_cow_set_attribute(lxb_dom_element_t *el, const char *name, const char *val, size_t val_len,
                           JSValueConst taint) {
    AttrIdent id;

    attr_ident_of(el, name, &id);
    dom_cow_set_attribute_ns(el, id.ns, id.prefix, id.local, val, val_len, taint);
    attr_ident_free(&id);
}
void dom_cow_remove_attribute(lxb_dom_element_t *el, const char *name) {
    AttrIdent id;

    attr_ident_of(el, name, &id);   /* §4.9 "remove an attribute by name" step 1 — the identity that name means */
    dom_cow_remove_attribute_ns(el, id.ns, id.local);
    attr_ident_free(&id);
}
/* The taint a flow can see at this attribute — BORROWED, like attr_shadow_opaque, and JS_UNDEFINED when the
   attribute carries none. The read's twin of the fused write: it resolves the identity the same way, so a
   qualified-name read finds what a qualified-name write stored. */
JSValue dom_cow_attr_taint_ns(lxb_dom_element_t *el, const char *ns, const char *local) {
    int si = attr_shadow_find(el, ATTR_SLOT_ATTRIBUTE, ns, local);
    return si >= 0 ? attr_shadow_opaque(si) : JS_UNDEFINED;
}
JSValue dom_cow_attr_taint(lxb_dom_element_t *el, const char *name) {
    AttrIdent id;
    JSValue t;

    attr_ident_of(el, name, &id);
    t = dom_cow_attr_taint_ns(el, id.ns, id.local);
    attr_ident_free(&id);
    return t;
}

/* THE TREE VERSION — Blink's Document::dom_tree_version, and it is here rather than on the document for the
   reason every other piece of this file is: the tree a flow sees is the baseline PLUS its delta, so "did the
   tree change" has to include the swap. A live collection caches an index into the child list, and that cache
   is valid exactly while this number is unchanged.
   MONOTONIC AND GLOBAL, deliberately. A number that only ever grows can be wrong in one direction — it can say
   "changed" when a particular flow's tree did not — and a spurious invalidation costs a re-walk, while a missed
   one is a wrong answer. Bumping it on the SWAP is what makes a C-side cache sound without being per-flow: a
   cache another flow filled is invalid the moment the swap that let this flow run happened. */
static uint64_t g_dom_version = 1;

uint64_t dom_cow_version(void) { return g_dom_version; }

/* Remove a node that the baseline may own. Capture its position FIRST, then detach — the same order the
   attribute and insert chokepoints use, so a removal cannot bypass the per-flow delta either. */
void dom_cow_remove_child(lxb_dom_node_t *node) {
    if (!node) return;
    g_dom_version++;
    /* BEFORE the detach, because "was it connected" has no answer afterwards. */
    DCHECK(!g_tree_hook || g_cow_ctx, TREE_HOOK_NO_CTX);
    if (g_tree_hook) g_tree_hook(g_cow_ctx, node, 0);
    if (g_dom_capture) {
        DomUndo u; memset(&u, 0, sizeof u);
        u.kind = 2; u.node = node;
        u.parent = node->parent; u.next = node->next;
        u.sh_old = u.sh_cur = JS_UNDEFINED;
        dom_undo_push(u);
    }
    lxb_dom_node_remove(node);
}

/* A PRIVATE TREE — the DOM's half of the invariant the heap COW already keeps: a delta captures only SHARED
   baseline state, because state the running flow created cannot be observed by another flow, and capturing it
   would make the delta O(everything the flow built) instead of O(shared state it touched).
   Two callers need it and they need it for opposite directions. The fragment parse builds a detached tree and
   moves nodes OUT of it into the real one — the move is a shared write and goes through the chokepoint, but the
   detach from the parse's own tree, and destroying that tree afterwards, are writes to something nothing else
   has ever seen. cloneNode builds a detached tree by inserting INTO it, node by node.
   THE DECLARATION IS A PARAMETER, not a scope. It was a scope — a `begin`/`end` pair around a global — and that
   was wrong the moment a MACHINE needed it: a clone of the page's subtree parks in the middle of building the
   copy, and a scope held in a global is open while another flow runs and opens its own. Passing the root makes
   the declaration part of the call, so it survives a suspension because there is nothing to survive.
   NO PREDICATE ON THE NODE WOULD DO. "Unparented" is what a detached tree and a subtree REMOVED from the
   document both look like, and the second must never be written raw — another flow's baseline still holds it,
   and that flow's unapply re-inserts what this one changed or freed. So the declared root is checked against
   every live removal entry, which is the difference between the two. */

/* IS THIS NODE ACTUALLY IN ITS PARENT'S CHILD LIST? Not the same question as "does it have a parent pointer",
   which is why this is a function and not `n->parent != NULL`. Lexbor's fragment parse ENDS by pointing its
   detached root's `parent` at the document — `parser->root->parent = &doc->node` — without ever linking it as a
   child, so the root can reach an owner document while being in nobody's tree. A linked child is either the
   first one or has a previous sibling; a node that is neither is unattached however its parent pointer reads. */
static bool dom_is_attached(const lxb_dom_node_t *n) {
    return n->parent && (n->parent->first_child == n || n->prev != NULL);
}

/* The tree `n` belongs to, following only REAL attachment for the reason above. */
static lxb_dom_node_t *dom_root_of(lxb_dom_node_t *n) {
    while (dom_is_attached(n)) n = n->parent;
    return n;
}

/* Is this node parked in some flow's delta as REMOVED? Then it is shared-tree state on loan, not private. The
   whole chain, not just the head: a sibling's base segment holds its removals too. Dev-only and O(delta). */
static bool dom_delta_removed(lxb_dom_node_t *n) {
    for (int i = 0; i < g_dom_undo_n; i++)
        if (g_dom_undo[i].kind == 2 && g_dom_undo[i].node == n) return true;
    for (DomSeg *s = g_dom_base; s; s = s->base)
        for (int i = 0; i < s->n; i++)
            if (s->e[i].kind == 2 && s->e[i].node == n) return true;
    return false;
}

/* The declaration, asserted. Every operation below runs it on the root it was handed, so a caller cannot
   establish privacy once and then drift into a tree that stopped being private. */
static void dom_private_check(lxb_dom_node_t *root) {
    DCHECK(root && !dom_is_attached(root), "a private tree was declared for a node that is in someone's tree");
    DCHECK(!dom_delta_removed(root),
           "a private tree was declared for a subtree that a flow REMOVED from the document — another flow's "
           "baseline still holds it and that flow's unapply re-inserts it, so writing it raw is invisible to "
           "that flow and destroying it frees live tree");
}

/* Take a node OUT of the private tree. No capture, because nothing shared changes: the node is about to be
   handed to a capturing insert, and that insert is the write another flow could see. */
void dom_cow_take_private(lxb_dom_node_t *root, lxb_dom_node_t *node) {
    dom_private_check(root);
    DCHECK(node && dom_root_of(node) == root,
           "dom_cow_take_private on a node outside the declared private tree — a detach of shared tree state "
           "must be captured, or the flow that still has it in its baseline re-inserts freed memory on unapply");
    lxb_dom_node_remove(node);
    /* It was reachable only through the private root, which the parse destroys; out of that tree it stands on
       its own, so this is where it becomes a node the FLOW owns. Parsed markup enters the document exactly
       here, which is why the entry belongs at this seam rather than at each caller. */
    dom_cow_note_created(node);
}

/* Insert INTO the private tree — what building a clone is. The child must be nowhere at all: a node that is in
   any tree is one some flow can reach, and moving it here would be a structural change with no delta entry. */
void dom_cow_insert_private(lxb_dom_node_t *root, lxb_dom_node_t *parent, lxb_dom_node_t *child) {
    dom_private_check(root);
    DCHECK(parent && dom_root_of(parent) == root,
           "dom_cow_insert_private into a parent outside the declared private tree");
    DCHECK(child && !dom_is_attached(child),
           "dom_cow_insert_private with a child that is already in a tree — that is a MOVE, which is a "
           "structural change to wherever it came from and needs the capturing chokepoint");
    lxb_dom_node_insert_child(parent, child);
}

/* Destroy the tree. For the fragment parse its children must already be gone — destroying one that still holds
   nodes would free tree the caller is about to insert — so the caller says which it means. */
/* Every node in the subtree about to be freed, so each can hand back the wrapper the identity map holds for
   it. The walk carries no C recursion for the reason every other tree walk here does not: the depth is the
   page's data, so it descends by first_child and climbs by parent, never above the root it was given. */
/* Every node in the subtree, and NOTHING outside it. The bound is `root`, not root's PARENT: this walk runs on
   a DETACHED subtree, whose root can perfectly well have siblings — a fragment's children are siblings of each
   other — and stopping at the parent let the climb step past root into them. It then handed back wrappers for
   nodes that were never destroyed, leaving live nodes holding freed JSValues, which is an out-of-bounds access
   the moment one of them is touched again. Climbing only while `n != root` cannot leave the subtree.
   No C recursion: depth here is the page's data. */
static void dom_forget_wrappers(JSContext *ctx, lxb_dom_node_t *root)
{
    lxb_dom_node_t *n = root;

    for (;;) {
        node_wrap_forget(ctx, n);
        if (n->first_child) { n = n->first_child; continue; }
        while (n != root && !n->next) n = n->parent;
        if (n == root) return;
        n = n->next;
    }
}

/* THE NODE OWNERSHIP CONTRACT: a node a flow CREATED dies with that flow's delta.
 *
 * dom_buf_free said it outright — "its nodes stay detached, owned by the doc" — and that is the leak underneath
 * everything else. Nothing ever destroyed them, so every node any of thousands of exploration flows ever built
 * stayed alive for the whole run, and the wrapper identity map that names them grew with it until one doubling
 * was a five-second calloc-and-rehash inside a single createElement, with no suspend point in it. The stall was
 * the symptom; this is the cause.
 *
 * WHICH NODES ARE OURS TO DESTROY is the whole question, and the delta already answers it without a per-node
 * generation. A kind-1 entry records an INSERTION, which covers two different things: a node this flow created,
 * and a BASELINE node this flow moved (a move is a kind-2 removal plus a kind-1 insertion). Destroying the
 * second would free tree the baseline still owns. But by the time a delta is discarded it has been unapplied,
 * and unapply is exactly what tells them apart: the moved node was RE-INSERTED where the baseline had it, so it
 * is attached; the created node was detached and nothing put it back. So `attached` is the discriminator, and
 * it is a property the tree already carries rather than bookkeeping that could drift out of step.
 * The DCHECK covers the one way that reasoning could be wrong — a detached node that some other delta holds
 * parked as removed is the baseline's on loan, and freeing it would hand that flow freed memory on resume.
 *
 * DEEP, because a created subtree has ONE entry: its interior was built through dom_cow_insert_private, which
 * captures nothing precisely because nothing shared changed. Deep is safe here for the same reason `attached`
 * is the right test — a baseline node parked under a created one was pulled back out by the reverse-order
 * revert before this runs. */
static void dom_release_created(DomUndo *u)
{
    if (u->kind == 6 && u->node) {   /* an ATTRIBUTE this flow created — see the entry's own comment */
        lxb_dom_attr_t *a = lxb_dom_interface_attr(u->node);
        DCHECK(a->owner == NULL,
               "a flow's created attribute was still ON an element when its delta was discarded — a write of "
               "it was never unapplied, so freeing it would leave that element's list naming freed memory");
        DCHECK(g_cow_ctx != NULL, "a created attribute was freed with no context named — an Attr is a WRAPPED "
                                  "node and the identity map has to be told (dom_cow_set_ctx names the runtime)");
        /* AND THE TAINT SHADOW, for the same reason and in the same breath as the wrapper: a detached attribute
           keys its shadow on ITSELF, lexbor hands attributes out of a pool, and an entry left naming this
           address is inherited by the next attribute allocated at it. */
        attr_shadow_forget(g_cow_ctx, a);
        dom_attr_destroy(g_cow_ctx, a);
        u->node = NULL;   /* the entry has spent its claim; nothing may act on it twice */
        return;
    }
    if (u->kind != 4 || !u->node)
        return;
    /* Nothing may still hold it. After a discard the document is back at its baseline, so a node this flow
       created is detached by construction; one that is not means an insertion was never unapplied, and freeing
       it would take live tree out of the document under whoever is still reading it. */
    DCHECK(!dom_is_attached(u->node),
           "a flow's created node was still IN THE TREE when its delta was discarded — an insertion of it was "
           "never unapplied, so freeing it would remove live tree from the document");
    /* A kind-2 entry for this node is NOT a reason to keep it, and asserting that it was is a mistake this
       DCHECK caught on its first run: kind-2 records that a REMOVAL was captured, not that the baseline owned
       the node. A flow that creates a node, inserts it and then removes it records all three, and the node is
       still entirely its own. The creation entry is the authoritative statement of ownership, which is exactly
       why ownership is recorded at creation rather than inferred from positions.
       A SIBLING cannot lose tree this way either, and for a structural reason rather than a check: a creation
       made before a fork lives in the frozen segment BOTH flows reference, so its refcount holds it alive until
       the last of them is gone. Only a creation genuinely private to the discarded delta is reached here. */
    if (g_cow_ctx)
        dom_forget_wrappers(g_cow_ctx, u->node);
    lxb_dom_node_destroy_deep(u->node);
    u->node = NULL;   /* the entry has spent its claim; nothing may act on it twice */
}

/* EVERY CREATION IN A DELTA GOES BACK, NODES FIRST AND DOCUMENTS AFTER — see dom_cow_note_created_document for
 * why the order is load-bearing rather than tidy. One function because the order is a property of the RELEASE
 * and not of any one caller: three of them discard a delta, and an order restated three times is an order two
 * of them can lose. */
static void dom_release_created_all(DomUndo *e, int n)
{
    int i;

    for (i = 0; i < n; i++) dom_release_created(&e[i]);
    for (i = 0; i < n; i++) {
        if (e[i].kind != 5 || !e[i].doc)
            continue;
        dom_cow_destroy_document(e[i].doc);
        e[i].doc = NULL;   /* the entry has spent its claim; nothing may act on it twice */
    }
}

/* THIS FLOW CREATED THIS NODE — the delta's third kind of record, beside a mutation and a position.
 * §state-isolation says the delta captures MUTATIONS and CREATIONS, and only the first half was built: a node's
 * creation was inferred from the INSERTION that put it somewhere, which is a different fact. An insertion also
 * covers a baseline node being MOVED, and one node can be inserted, removed and re-inserted, so the inference
 * both over-claimed (freeing tree the baseline owns) and double-counted (freeing the same node twice — an
 * out-of-bounds access on the first run that tried it).
 * Recorded where the node is actually made, it is exact: one entry per node, no entry for a node the flow
 * merely moved, and no entry at all when capture is off — which is precisely the boot flow, whose creations
 * ARE the baseline and must outlive every delta. The entry is inert to apply/unapply (creating a node changes
 * nothing shared; its VISIBILITY is the insertion's job) and rides the existing head/fork/free machinery, so
 * sharing and refcounting need no new mechanism. */
/* THIS FLOW CREATED THIS ATTRIBUTE — kind 4's twin over an Attr node, and separate for the two reasons the
 * entry's own comment gives: reachability is `owner != NULL` rather than dom_is_attached, and the free is the
 * Attr destructor. Called from the attribute chokepoint, which is the only place that learns a write had to
 * make one. */
static void dom_note_created_attr(lxb_dom_attr_t *a)
{
    DomUndo u;
    if (!g_dom_capture || !a)
        return;
    memset(&u, 0, sizeof u);
    u.kind = 6; u.node = lxb_dom_interface_node(a); u.sh_old = u.sh_cur = JS_UNDEFINED;
    dom_undo_push(u);
}

void dom_cow_note_created(lxb_dom_node_t *node)
{
    DomUndo u;
    if (!g_dom_capture || !node)
        return;
    memset(&u, 0, sizeof u);
    u.kind = 4; u.node = node; u.sh_old = u.sh_cur = JS_UNDEFINED;
    dom_undo_push(u);
}

void dom_cow_destroy_document(lxb_html_document_t *dom)
{
    DCHECK(dom != NULL, "a document destroy was asked for no document");
    /* THE RECORD NAMES THE TREE, so it cannot outlive it — and it holds the document's wrapper and its
       DOMImplementation, which is one struct and two references per document any exploration arm ever built. */
    document_record_release(dom);
    /* BEFORE the free, because after it the nodes are gone and the identity map would be left naming freed
       memory — which is the state a pool allocator turns into another node inheriting this one's wrapper. */
    if (g_cow_ctx)
        dom_forget_wrappers(g_cow_ctx, lxb_dom_interface_node(dom));
    lxb_html_document_destroy(dom);
}

/* THIS FLOW CREATED THIS DOCUMENT — the delta's fifth kind of record, and the reason it is a KIND rather than a
 * fourth-kind entry over the document's root node: a document is destroyed by lxb_html_document_destroy, which
 * frees the memory arena every node in it came out of, and kind 4's lxb_dom_node_destroy_deep would free the
 * root and leave the arena.
 * THAT ORDER IS ALSO WHY THE RELEASE IS TWO PASSES. A node this flow created INSIDE a document this flow
 * created has its own kind-4 entry, and its memory belongs to that document's arena — so every node goes back
 * before any document does, or the second free reads an arena that is already gone. */
bool dom_cow_note_created_document(lxb_html_document_t *dom)
{
    DomUndo u;
    DCHECK(dom != NULL, "a created document was noted for no document");
    if (!g_dom_capture)
        return false;
    memset(&u, 0, sizeof u);
    u.kind = 5; u.doc = dom; u.sh_old = u.sh_cur = JS_UNDEFINED;
    dom_undo_push(u);
    return true;
}

void dom_cow_destroy_private(lxb_dom_node_t *root, bool with_children) {
    dom_private_check(root);
    DCHECK(with_children || root->first_child == NULL,
           "a private tree was destroyed with children still in it — those nodes are about to be freed under "
           "whatever took a reference to them");
    (void)with_children;
    /* BEFORE the free, because after it the nodes are gone and the map would be left naming freed memory —
       which is the state a pool allocator turns into another node inheriting this one's wrapper. */
    if (g_cow_ctx)
        dom_forget_wrappers(g_cow_ctx, root);
    lxb_dom_node_destroy(root);
}

/* A CHARACTER-DATA node's VALUE (§4.10 `data`). The third thing a flow can change about the tree, after an
   attribute and a node's presence: `text.data = x` mutates bytes the baseline owns, in place, on a node whose
   IDENTITY must survive the write — so it cannot be modelled as a remove+insert of a replacement node the way
   textContent legitimately is. Same shape as the attribute entry, over the node instead of the element. */
void dom_cow_set_text(lxb_dom_node_t *node, const char *val, size_t val_len) {
    lxb_dom_character_data_t *cd;
    if (!node) return;
    DCHECK(node->type == LXB_DOM_NODE_TYPE_TEXT || node->type == LXB_DOM_NODE_TYPE_COMMENT,
           "dom_cow_set_text on a node that holds no character data");
    cd = lxb_dom_interface_character_data(node);
    if (g_dom_capture) {
        DomUndo u; memset(&u, 0, sizeof u);
        u.kind = 3; u.node = node; u.sh_old = u.sh_cur = JS_UNDEFINED;
        u.had = 1; u.old_len = cd->data.length;
        u.old = malloc(u.old_len ? u.old_len : 1);
        CHECK(u.old != NULL, "dom-cow-oom: the character-data baseline snapshot failed — unapply would lose the "
                             "text the baseline had");
        memcpy(u.old, cd->data.data, u.old_len);
        dom_undo_push(u);
    }
    lxb_dom_character_data_replace(cd, (const lxb_char_t *)val, val_len, 0, cd->data.length);
}

void dom_cow_append_child(lxb_dom_node_t *parent, lxb_dom_node_t *child) {
    g_dom_version++;
    dom_insert_capture(child);   /* record the insertion FIRST so it reverts per-flow (detached on unapply) */
    lxb_dom_node_insert_child(parent, child);
    DCHECK(!g_tree_hook || g_cow_ctx, TREE_HOOK_NO_CTX);
    if (g_tree_hook) g_tree_hook(g_cow_ctx, child, 1);   /* AFTER: connectedness is the new tree's */
}
/* §4.2.3 "insert before": the same capture, at a POSITION. The insert entry remembers where it landed at
   unapply time rather than at capture time, so this differs from append only in the Lexbor call — which is
   exactly why insertBefore must come through here and not reach lxb_dom_node_insert_before directly. */
void dom_cow_insert_before(lxb_dom_node_t *ref, lxb_dom_node_t *child) {
    g_dom_version++;
    dom_insert_capture(child);
    lxb_dom_node_insert_before(ref, child);
    DCHECK(!g_tree_hook || g_cow_ctx, TREE_HOOK_NO_CTX);
    if (g_tree_hook) g_tree_hook(g_cow_ctx, child, 1);
}
void dom_revert(void) {   /* DISCARD the running flow's DOM writes -> baseline (reverse order); empties the delta */
    g_dom_version++;
    for (int i = g_dom_undo_n - 1; i >= 0; i--) {
        DomUndo *u = &g_dom_undo[i];
        if (u->kind == 0) {   /* named slot: restore old value (attributes only) + old taint shadow */
            if (u->slot == ATTR_SLOT_ATTRIBUTE) {
                /* PRESENCE decides, not whether there were BYTES. `had && old` treated an attribute with an
                   empty value as one the baseline never had, so a revert REMOVED it — and `<input required>`
                   is exactly that attribute. */
                if (u->had) attr_put(u, u->attr, u->attr_next, u->old, u->old_len);
                else attr_drop(u);
            }
            shadow_restore(u->sh_owner, u->slot, u->ns, u->name, u->sh_old, u->sh_had);
            if (g_cow_ctx) { JS_FreeValue(g_cow_ctx, u->sh_old); JS_FreeValue(g_cow_ctx, u->sh_cur); }
            free(u->ns); free(u->prefix); free(u->name); free(u->old); free(u->cur);
        } else if (u->kind == 3) {   /* character data: put the baseline's text back */
            lxb_dom_character_data_t *cd = lxb_dom_interface_character_data(u->node);
            lxb_dom_character_data_replace(cd, u->old, u->old_len, 0, cd->data.length);
            free(u->old); free(u->cur);
        } else if (u->kind == 7) {   /* a DETACHED attribute's value: put the baseline's bytes back */
            dom_attr_set_value(lxb_dom_interface_attr(u->node), (const char *)(u->old ? u->old : (lxb_char_t *)""),
                               u->old_len);
            shadow_restore(u->sh_owner, u->slot, u->ns, u->name, u->sh_old, u->sh_had);
            if (g_cow_ctx) { JS_FreeValue(g_cow_ctx, u->sh_old); JS_FreeValue(g_cow_ctx, u->sh_cur); }
            free(u->ns); free(u->name); free(u->old); free(u->cur);
        } else if (u->kind == 1 && !u->detached) {   /* inserted node: detach it (baseline had none) */
            lxb_dom_node_remove(u->node);
        } else if (u->kind == 2 && !u->reinserted) {   /* removed node: the baseline HAD it — put it back */
            if (u->next) lxb_dom_node_insert_before(u->next, u->node);
            else if (u->parent) lxb_dom_node_insert_child(u->parent, u->node);
        }
    }
    /* The head's creations die here, BEFORE the base segments below: a head node inserted into a segment's
       node is the child, and a child must be freed before the parent it hangs under is freed deep. */
    dom_release_created_all(g_dom_undo, g_dom_undo_n);
    g_dom_undo_n = 0;
    /* A DISCARD, so the document goes all the way back to the baseline and NOTHING stays installed — the
       segments this flow held may be freed below, and an installed pointer into freed memory is what the next
       switch would walk. */
    dom_unapply_seg(g_dom_installed);
    g_dom_installed = NULL;
    dom_seg_unref(g_dom_base); g_dom_base = NULL;   /* drop this flow's reference; base freed iff no sibling holds it */
}
/* per-entry UNAPPLY (flow -> parked): stash the flow's value/taint into cur, restore the baseline. */
static void dom_unapply_entry(DomUndo *u) {
    g_dom_version++;
    if (u->kind == 0) {
        if (u->slot == ATTR_SLOT_ATTRIBUTE) {
            lxb_dom_attr_t *a = dom_attr_get_ns(u->el, u->ns, u->name);
            size_t vl = 0; const lxb_char_t *c = a ? lxb_dom_attr_value(a, &vl) : NULL;
            free(u->cur); u->cur = NULL; u->cur_len = 0; u->cur_had = a ? 1 : 0;   /* stash the flow's attr value */
            /* AND THE NODE, for the same reason the baseline side holds one: the attribute the FLOW put here
               may be a different Attr from the baseline's, and the page may be holding it. */
            u->cur_attr = a; u->cur_attr_next = a ? a->next : NULL;
            if (c) { u->cur = malloc(vl ? vl : 1); CHECK(u->cur, "dom-cow-oom: parked flow attr snapshot malloc failed — apply would lose the flow's DOM write"); memcpy(u->cur, c, vl); u->cur_len = vl; }
        }
        if (g_cow_ctx) JS_FreeValue(g_cow_ctx, u->sh_cur);
        u->sh_cur = shadow_snapshot(u->sh_owner, u->slot, u->ns, u->name, &u->sh_cur_had);   /* stash the flow's taint shadow */
        if (u->slot == ATTR_SLOT_ATTRIBUTE) {
            if (u->had) attr_put(u, u->attr, u->attr_next, u->old, u->old_len);
            else attr_drop(u);
        }
        shadow_restore(u->sh_owner, u->slot, u->ns, u->name, u->sh_old, u->sh_had);   /* restore the baseline taint */
    } else if (u->kind == 7) {
        lxb_dom_attr_t *a = lxb_dom_interface_attr(u->node);
        size_t vl = 0; const lxb_char_t *c = lxb_dom_attr_value(a, &vl);
        free(u->cur); u->cur = NULL; u->cur_len = 0; u->cur_had = 1;       /* stash the flow's bytes */
        if (c) { u->cur = malloc(vl ? vl : 1);
                 CHECK(u->cur, "dom-cow-oom: parked detached-attribute snapshot malloc failed");
                 memcpy(u->cur, c, vl); u->cur_len = vl; }
        if (g_cow_ctx) JS_FreeValue(g_cow_ctx, u->sh_cur);
        u->sh_cur = shadow_snapshot(u->sh_owner, u->slot, u->ns, u->name, &u->sh_cur_had);
        dom_attr_set_value(a, (const char *)(u->old ? u->old : (lxb_char_t *)""), u->old_len);   /* baseline back */
        shadow_restore(u->sh_owner, u->slot, u->ns, u->name, u->sh_old, u->sh_had);
    } else if (u->kind == 3) {
        lxb_dom_character_data_t *cd = lxb_dom_interface_character_data(u->node);
        free(u->cur); u->cur_len = cd->data.length; u->cur_had = 1;       /* stash the flow's text */
        u->cur = malloc(u->cur_len ? u->cur_len : 1);
        CHECK(u->cur != NULL, "dom-cow-oom: the parked character-data snapshot failed — apply would lose the "
                              "flow's text write");
        memcpy(u->cur, cd->data.data, u->cur_len);
        lxb_dom_character_data_replace(cd, u->old, u->old_len, 0, cd->data.length);   /* baseline back */
    } else if (u->kind == 1 && !u->detached) {
        u->parent = lxb_dom_interface_node(u->node)->parent;              /* remember re-insert position */
        u->next = lxb_dom_interface_node(u->node)->next;
        lxb_dom_node_remove(u->node); u->detached = 1;
    } else if (u->kind == 2 && !u->reinserted) {
        /* the baseline HAD this node and this flow removed it: parking restores the baseline. */
        if (u->next) lxb_dom_node_insert_before(u->next, u->node);
        else if (u->parent) lxb_dom_node_insert_child(u->parent, u->node);
        u->reinserted = 1;
    }
}
/* per-entry APPLY (parked -> flow): restore the flow's value/taint over the baseline. */
static void dom_apply_entry(DomUndo *u) {
    g_dom_version++;
    if (u->kind == 0) {
        if (u->slot == ATTR_SLOT_ATTRIBUTE) {
            if (u->cur_had) attr_put(u, u->cur_attr, u->cur_attr_next, u->cur, u->cur_len);
            else attr_drop(u);
        }
        shadow_restore(u->sh_owner, u->slot, u->ns, u->name, u->sh_cur, u->sh_cur_had);   /* restore the flow's taint */
    } else if (u->kind == 7 && u->cur_had) {
        dom_attr_set_value(lxb_dom_interface_attr(u->node),
                           (const char *)(u->cur ? u->cur : (lxb_char_t *)""), u->cur_len);
        shadow_restore(u->sh_owner, u->slot, u->ns, u->name, u->sh_cur, u->sh_cur_had);
    } else if (u->kind == 3 && u->cur_had) {
        lxb_dom_character_data_t *cd = lxb_dom_interface_character_data(u->node);
        lxb_dom_character_data_replace(cd, u->cur, u->cur_len, 0, cd->data.length);   /* the flow's text back */
    } else if (u->kind == 2 && u->reinserted) {
        /* resuming the flow: it had removed this node, so take it back out. */
        lxb_dom_node_remove(u->node); u->reinserted = 0;
    } else if (u->kind == 1 && u->detached) {
        if (u->next) lxb_dom_node_insert_before(u->next, u->node);
        else if (u->parent) lxb_dom_node_insert_child(u->parent, u->node);
        u->detached = 0;
    }
}
/* apply a base chain FORWARD (deepest ancestor first, then up); unapply is the mirror. NULL-safe (flat delta). */
/* THE BASE CHAIN IS WALKED, NOT RECURSED. These two ran on every context switch, once per shared base segment,
   and the chain's depth is the fork depth — so a deeply forked frontier put an unbounded C recursion on the
   hottest path the scheduler has. All recursion is banned here for one reason: C stack cannot be suspended,
   parked or resumed, and a switch that cannot be interrupted mid-way is a switch that cannot time-travel. That
   neither of these runs any of the page's code makes no difference.
   UNAPPLY is head-first, so it is a plain loop. APPLY is deepest-ancestor-first, which the recursion got by
   unwinding; it gets it here by REVERSING the base pointers in place, walking forward, and reversing them back
   — O(depth), no allocation, and nothing to fail on a path where a failed allocation would corrupt the swap. */
/* How deep a chain is — the number of FORKS behind it, never the number of entries. Both walks below are
   O(depth) for that reason, and depth is the shape of the frontier rather than the size of the page. */
static int dom_seg_depth(DomSeg *s) { int d = 0; for (; s; s = s->base) d++; return d; }

/* The deepest segment BOTH chains hold. Pointer identity is the whole test: a segment is frozen once and never
   written again, so two chains that reach the same pointer agree about everything from there down. */
static DomSeg *dom_seg_common(DomSeg *a, DomSeg *b)
{
    int da = dom_seg_depth(a), db = dom_seg_depth(b);
    while (da > db) { a = a->base; da--; }
    while (db > da) { b = b->base; db--; }
    while (a != b) { a = a->base; b = b->base; }
    return a;
}

/* Apply `s` down to (not including) `stop`, deepest-first. `stop` stands in for NULL as the end of the part
   being reversed, so the shared suffix below it is not touched at all — not read, not written, not walked.
   UNAPPLY is head-first, so it is a plain loop. APPLY is deepest-ancestor-first, which a recursion would get by
   unwinding; it gets it here by REVERSING the base pointers in place, walking forward, and reversing them back
   — O(depth), no allocation, and nothing to fail on a path where a failed allocation would corrupt the swap.
   All recursion is banned here for one reason: C stack cannot be suspended, parked or resumed, and a switch
   that cannot be interrupted mid-way is a switch that cannot time-travel. */
static void dom_apply_seg_until(DomSeg *s, DomSeg *stop)
{
    DomSeg *prev = stop, *cur = s, *next;

    while (cur != stop) { next = cur->base; cur->base = prev; prev = cur; cur = next; }
    cur = prev; prev = stop;
    while (cur != stop) {
        for (int i = 0; i < cur->n; i++) dom_apply_entry(&cur->e[i]);
        next = cur->base; cur->base = prev; prev = cur; cur = next;
    }
}

/* Make `want` the installed chain: the ONE place the document's applied state changes, and the reason a switch
   between two related flows costs what they DIVERGED by rather than everything either of them has done. */
static void dom_install_chain(DomSeg *want)
{
    DomSeg *common = dom_seg_common(g_dom_installed, want);
    DomSeg *s;

    for (s = g_dom_installed; s != common; s = s->base)      /* head-first, as unapply always is */
        for (int i = s->n - 1; i >= 0; i--) dom_unapply_entry(&s->e[i]);
    dom_apply_seg_until(want, common);
    g_dom_installed = want;
}

static void dom_unapply_seg(DomSeg *s)
{
    for (; s; s = s->base)
        for (int i = s->n - 1; i >= 0; i--) dom_unapply_entry(&s->e[i]);
}
/* drop a chain reference: refcount--, free the segment's entries (parked: old/cur held) when it hits 0, recurse. */
static void dom_seg_unref(DomSeg *s) {
    while (s && --s->refcount <= 0) {
        DomSeg *base = s->base;
        dom_release_created_all(s->e, s->n);
        for (int i = 0; i < s->n; i++) { DomUndo *u = &s->e[i];
            free(u->ns); free(u->prefix); free(u->name); free(u->old); free(u->cur);
            if (g_cow_ctx) { JS_FreeValue(g_cow_ctx, u->sh_old); JS_FreeValue(g_cow_ctx, u->sh_cur); } }
        free(s->e); free(s);
        s = base;
    }
}
/* UNAPPLY (flow -> parked): head reverse, then the shared base chain. Restores the baseline DOM+taint. */
void dom_unapply(void) {
    /* ONLY THE HEAD. The base chain stays applied and `g_dom_installed` says so — the incoming flow's apply
       decides how much of it actually has to move, which for a sibling is none of it. */
    for (int i = g_dom_undo_n - 1; i >= 0; i--) dom_unapply_entry(&g_dom_undo[i]);
    DCHECK(g_dom_installed == g_dom_base,
           "the applied chain is not the running flow's — a base was loaded or taken without going through "
           "dom_apply, so the document is showing a chain nobody is running");
}
/* APPLY (parked -> flow): base chain forward (deepest first), then the head on top. */
void dom_apply(void) {
    dom_install_chain(g_dom_base);
    for (int i = 0; i < g_dom_undo_n; i++) dom_apply_entry(&g_dom_undo[i]);
}
/* FORK the DOM delta: freeze the running flow's HEAD into a shared immutable base segment that BOTH the running
   flow and a snapshot-forked sibling reference (refcount 2) — the sibling SHARES the parent's O(N) DOM delta
   instead of copying it. Head is applied, so UNAPPLY it to parked state (values -> cur, DOM -> baseline), freeze
   that, then RE-APPLY so the running flow continues byte-identically. Capture is suspended across the round-trip
   (defensive symmetry with the heap fork — the internal set/remove goes straight to Lexbor, not the capturing
   host-edge, so no re-capture, but the guard documents+enforces the invariant). Returns the shared base. */
void *dom_cow_fork(void) {
    int sv = g_dom_capture; g_dom_capture = 0;
    for (int i = g_dom_undo_n - 1; i >= 0; i--) dom_unapply_entry(&g_dom_undo[i]);
    DomSeg *seg = malloc(sizeof(DomSeg));
    CHECK(seg, "dom-cow-oom: fork segment alloc failed — a shared DOM delta would be corrupted");
    seg->e = g_dom_undo; seg->n = g_dom_undo_n; seg->base = g_dom_base; seg->refcount = 2;   /* running flow + sibling */
    g_dom_undo = NULL; g_dom_undo_n = 0; g_dom_undo_cap = 0;   /* fresh empty head for the running flow */
    g_dom_base = seg;
    g_dom_installed = seg;   /* the frozen head belongs to the applied chain now, not to anyone's head */
    for (int i = 0; i < seg->n; i++) dom_apply_entry(&seg->e[i]);   /* re-apply head -> running flow continues */
    g_dom_capture = sv;
    return seg;
}
/* Take / install the shared BASE chain alongside the head (a flow's full DOM delta is head + base chain). */
void *dom_base_take(void) { void *b = g_dom_base; g_dom_base = NULL; return b; }
void dom_base_load(void *base) { g_dom_base = (DomSeg *)base; }
void dom_base_free(void *base) { if (base) dom_seg_unref((DomSeg *)base); }
void dom_base_ref(void *base) { if (base) ((DomSeg *)base)->refcount++; }   /* each orphan forks the document flow's shared DOM delta */
/* Free a parked DOM delta buffer — and the nodes the flow CREATED go with it. The comment here used to read
   "its nodes stay detached, owned by the doc", which was the leak stated as if it were a design. */
void dom_buf_free(void *buf, int n) {
    DomUndo *b = (DomUndo *)buf;
    dom_release_created_all(b, n);
    for (int i = 0; i < n; i++) {
        free(b[i].ns); free(b[i].prefix); free(b[i].name); free(b[i].old); free(b[i].cur);
        if (g_cow_ctx) { JS_FreeValue(g_cow_ctx, b[i].sh_old); JS_FreeValue(g_cow_ctx, b[i].sh_cur); }
    }
    free(b);
}
/* dom_buf_snapshot (the O(delta) selective COPY of a continuation's DOM attribute delta) is DELETED: its sole
   caller (flow_defer_callback) now SHARES the delta via dom_cow_fork's refcounted immutable base segment —
   O(1), and it carries the inserted-node (kind-1) entries the copy dropped as pointer-fragile. */
void *dom_buf_take(int *n, int *cap) { void *b = g_dom_undo; *n = g_dom_undo_n; *cap = g_dom_undo_cap; g_dom_undo = NULL; g_dom_undo_n = 0; g_dom_undo_cap = 0; return b; }
void dom_buf_load(void *buf, int n, int cap) { g_dom_undo = (DomUndo *)buf; g_dom_undo_n = n; g_dom_undo_cap = cap; }
