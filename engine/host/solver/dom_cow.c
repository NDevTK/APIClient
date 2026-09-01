/* Per-flow DOM COW delta — see dom_cow.h. */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>   /* uintptr_t/uint64_t — the creation-claim index hashes a node's ADDRESS */
#include "solver/reclaim.h"   /* the engine's own allocations ask for a flow back before they fail */
#include "check.h"        /* CHECK — an OOM here corrupts DOM isolation, fatal in every build */
#include "solver/dom_cow.h"
#include "core/dom/document.h"   /* document_record_release — a destroyed document hands back its record */
#include "core/dom/attr_list.h"   /* §4.9's attribute-list algorithms — what the delta restores an attribute THROUGH */
#include "core/dom/name_intern.h"   /* a node's names are per-DOCUMENT state, so kind 8 moves them with the pointer */
#include "core/dom/node_heap.h"     /* …and its BYTES are the agent's, which is why kind 8 asserts and moves nothing */
#include "core/dom/node_interface.h"   /* dom_document_destroy — a document's nodes go back before its arenas do */
#include "core/dom/node.h"   /* node_template_content — §4.12.3's second tree, which a parse also builds into */
#include "solver/attr_shadow.h"   /* the taint shadow rides the attribute delta (per-flow isolation of stashed taint) */
/* DOM §4.4 Interface Node's `clone a node` STEP 3 CLONING STEPS, which a private tree's copy owes exactly as
   clone-a-node does — these are the same two component entries core/dom/node.c calls at its own step 3, and
   the two the list does NOT cover are what the copy crashes on by name. */
#include "core/dom/shadow_root.h"        /* the ELEMENT -> shadow root edge, a WRAPPER slot no C walk can see —
                                            so BOTH halves of a private tree have to ask for it by name */
#include "core/html/html_script.h"       /* §4.12.1's pair: `already started`, which §13.4's Inert mode sets */
#include "core/html/media_element.h"     /* §4.8.11 — state on the wrapper, and no cloning steps at all */
#include "core/html/nonce_attribute.h"   /* §2.5.6's pair, over every HTML element and not one tag */
#include <lexbor/core/avl.h>   /* the node->node map: a pointer map with no structure to invent */
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
    /* kind 8: a node's NODE DOCUMENT. DOM §4.5's adopt writes it for every shadow-including inclusive
       descendant of the node being adopted, and nothing captured it: a flow that adopted a subtree moved those
       nodes into another document for EVERY flow, so a sibling arm that never adopted read `node.ownerDocument`
       as the adopting arm's answer, and `createElement` on it built into the wrong tree.
       TWO FIELDS AND NOT `doc` REUSED, for kind 5's stated reason one step further: kind 5 owns an
       lxb_html_document_t it must DESTROY, this one holds two lxb_dom_document_t it must never destroy — it
       only points a node back at one — and a type pun between them would compile and free a document the page
       is still using. */
    lxb_dom_document_t *doc_old, *doc_cur;
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
static void dom_seg_unref(DomSeg *s);
static void dom_note_created_attr(lxb_dom_attr_t *a);   /* fwd: the chokepoint is above the creation records */

/* WHAT THIS FILE IS HOLDING WHEN IT ALLOCATES — the DOM twin of cow.c's capture scope, and the DIFFERENCE is
 * the whole reason it is its own mechanism rather than the same pair of depths.
 *
 * THE HEAP'S GUARD ANSWERS TWO QUESTIONS AND THIS ONE ANSWERS ONLY THE SECOND. cow.c needs `g_swapping`
 * because the heap's capture edge is a HOOK the runtime calls on every property write, so the engine's own
 * apply/unapply — which writes the heap — would be re-captured into the delta it is restoring out of. This
 * file has no such hook: its capture edge is the CHOKEPOINT (dom_cow.h), an explicit call a browser component
 * makes, and dom_unapply_entry / dom_apply_entry reach lexbor, attr_list.c and attr_shadow.c directly and call
 * no producer. That was read out of the callees rather than assumed — `g_dom_capture` is read at ten sites and
 * every one is in this file, attr_list.c and name_intern.c never name dom_cow, and node_finalizer is empty —
 * so a swap scope here would be a guard with nothing to guard, and dom_cow_fork's `g_dom_capture = 0` round
 * trip, which admitted as much in its own comment ("defensive symmetry"), is DELETED rather than mirrored. It
 * was the fallback shape: a re-capture it would have silently DROPPED now aborts at dom_undo_push.
 *
 * WHAT IS LEFT IS THE ONE FACT ONLY A CAPTURE CAN STATE: this file is holding a RAW POINTER INTO THE DOCUMENT
 * — an element, an attribute node, an `lxb_dom_attr_value` byte pointer, a parent/next link — across an
 * allocation that can SELL A FLOW (solver/reclaim.h). The ten entry producers do it, and so do the two copies
 * that produce no entry (dupn's identity copy, attr_old_value's §9.4.6 old value), which is why the scope is
 * defined over the READ and not over the delta: excluding the two that only copy would make them the sites the
 * proof below does not cover, which is the hand-picked list exactly.
 *
 * THE SALE CANNOT INVALIDATE THAT READ, AND THAT IS TWO FACTS, EACH ALREADY A CONSTRUCTION:
 *   - it never touches the RUNNING flow's document state. engine_reclaim_tail sells flow_worst(flow_running())
 *     and asserts the flow it picked is not the running one, and a sold flow's created nodes were detached at
 *     its switch-out — so every node the sale destroys is one per-flow isolation guarantees the running flow
 *     cannot reach, and a node BOTH of them can reach lives in a segment with a second reference.
 *   - it cannot move the APPLIED CHAIN. `g_dom_installed` is the running flow's own `g_dom_base` (dom_apply
 *     installs it, dom_unapply asserts it); every flow holds ONE counted reference on its base and every
 *     segment holds one on ITS base — dom_cow_fork hands out refcount 2, one for the running flow and one for
 *     the sibling, and transfers the running flow's reference on the old base into `seg->base` — so a segment
 *     the running flow stands on has a refcount of at least 2 the moment any other chain reaches it, and
 *     dom_base_release's dying prefix stops at the first segment whose refcount is not 1. dom_install_chain is
 *     therefore unreachable from inside a sale.
 * The second is a proof about refcounts, which is exactly the kind of statement that stops being true without
 * anyone noticing, so it is held by the DCHECK at dom_install_chain rather than restated at twelve read sites.
 * AND IT IS THE DEREFERENCE THAT IS AT STAKE, NOT ONLY THE POINTER, which is what makes reordering these sites
 * buy nothing: dom_attr_capture resolves the attribute and its value bytes BEFORE it allocates and then
 * memcpy's out of them AFTER, and dom_cow_set_text takes the length before and the bytes after — so the
 * allocation stands between the read and the use in every one of them by construction. The consequence here is
 * worse than the heap's wrong value: a dying segment DESTROYS the nodes its entries created
 * (dom_release_created_all), so a read that spans a chain move can copy out of freed tree, and the entry it
 * writes names tree the next swap re-attaches.
 *
 * A DEPTH, AND IT MAY NOT NEST. cow.c's scopes nest because a swap legitimately runs inside a capture there;
 * here a producer reached from inside another producer could only be the SALE re-entering this file, and that
 * is not a hazard to argue away: dom_undo_push hands `g_dom_undo` to reclaim_realloc, so a nested push that
 * grows the log first leaves the outer retry holding the pointer the nested one freed, and publishing the
 * outer's stale capacity over it writes past the end of the array it just built. Asserted at the OPEN, which
 * is the origin; the depth is what makes the balance checkable at the close. */
static int g_dom_capturing;

static void dom_capture_begin(void) {
    DCHECK(g_dom_capturing == 0,
           "a DOM capture opened inside another one — the only thing that can run between this file's read of "
           "the document and the copy it makes of it is the allocator selling a flow, so this is the sale "
           "re-entering this file, and the nested push grows g_dom_undo and leaves the outer reclaim_realloc "
           "retrying on the pointer the nested one freed");
    g_dom_capturing++;
}
static void dom_capture_end(void) {
    DCHECK(g_dom_capturing > 0,
           "a DOM capture scope was closed that was never opened — from here on this file's reads of the "
           "document are invisible to dom_install_chain's check, so a chain move under one records freed tree "
           "in an entry that every later swap replays");
    g_dom_capturing--;
}

void dom_cow_set_ctx(JSContext *ctx) { g_cow_ctx = ctx; }

/* §4.2.3's insertion/removing steps. Fired from the chokepoint so a tree write cannot reach the tree without
   them: the browser layer registers what they MEAN and this file guarantees they run.
 *
 * THE GUARANTEE WAS `if (g_tree_hook && g_cow_ctx)`, AND THAT SECOND TERM UNDID IT. A host that registered the
 * hook but never named its context ran NO insertion steps and NO removing steps at all — no <script>
 * preparation, no custom-element upgrade, no §7.3.1.3 Child navigables' child navigable — and nothing said so, because a skipped
 * step looks exactly like a tree with nothing in it that needed one. That is what the WPT runner did for its
 * whole life: `document.body.appendChild(iframe)` produced an element with no navigable, and the failure
 * surfaced three layers away as `contentWindow` being null. The context is now ASSERTED rather than tested, and
 * the hook is called unconditionally, so a host that forgets crashes at the write instead of quietly running a
 * different DOM. */
static void (*g_tree_hook)(JSContext *ctx, lxb_dom_node_t *n, lxb_dom_node_t *parent, int phase);

#define TREE_HOOK_NO_CTX \
    "a tree write reached the insertion/removing steps with no context — dom_cow_set_ctx names the runtime " \
    "they run in, and a host that registers the hook without it runs NO insertion steps at all: no <script> " \
    "preparation, no custom-element upgrade, no child navigable, and nothing to say so"

void dom_cow_set_tree_hook(void (*fn)(JSContext *ctx, lxb_dom_node_t *n, lxb_dom_node_t *parent, int phase))
{
    g_tree_hook = fn;
}

static void (*g_attr_hook)(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local,
                           const char *old_val, size_t old_len, const char *val, size_t val_len);
void dom_cow_set_attr_hook(void (*fn)(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local,
                                      const char *old_val, size_t old_len, const char *val, size_t val_len))
{
    g_attr_hook = fn;
}

/* §9.4.6's OLD VALUE, copied out before the write replaces it. The change steps run AFTER the value is stored
   (step 2 sets it, step 3 handles the change), so the element no longer holds the old one by the time they run
   and it has to be carried across. NULL when the attribute was absent, which is the null `oldValue` a page's
   attributeChangedCallback branches on. The caller frees. */
static char *attr_old_value(lxb_dom_element_t *el, const char *ns, const char *local, size_t *len)
{
    lxb_dom_attr_t *a = dom_attr_get_ns(el, ns, local);
    const lxb_char_t *v;
    size_t vl = 0;
    char *copy;

    *len = 0;
    if (!a) return NULL;
    v = lxb_dom_attr_value(a, &vl);
    if (!v) return NULL;
    /* `v` POINTS INTO THE TREE and is held across an allocation that can sell a flow — the same read the
       producers hold, made by the one function here that copies without writing an entry. */
    dom_capture_begin();
    copy = reclaim_malloc(vl + 1);
    CHECK(copy != NULL, "dom-cow-oom: §9.4.6's old attribute value could not be copied — the change steps run "
                        "after the write and the element no longer holds it");
    memcpy(copy, v, vl);
    copy[vl] = 0;
    dom_capture_end();
    *len = vl;
    return copy;
}

static void (*g_cdata_hook)(JSContext *ctx, lxb_dom_node_t *node, const char *old, size_t old_len);
void dom_cow_set_cdata_hook(void (*fn)(JSContext *ctx, lxb_dom_node_t *node, const char *old, size_t old_len))
{ g_cdata_hook = fn; }

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

/* THE LIVE CREATION CLAIMS, INDEXED BY THE ADDRESS THEY CLAIM — the O(1) form of the one question a creation
 * has to ask before it records ownership.
 *
 * A kind-4 entry is an OWNERSHIP CLAIM, and two of them over one address is a second
 * `lxb_dom_node_destroy_deep` of memory the first release already freed. That is why every creation is checked
 * against the claims already standing, and the check WAS A WALK — the running flow's head, then every entry of
 * every segment in its base chain, once per creation. It is a DCHECK, so it is an O(delta) assert sitting
 * inside an O(1) primitive, and for a creation-heavy document the delta IS the creations (one entry each), so
 * a page that builds N nodes pays N²/2 comparisons in the DEV build — which is the build a CPU budget is
 * measured on. A document that builds a hundred thousand elements and gives each of them a shadow root spends
 * its whole budget inside this one assert, and the release build it is compiled out of runs fine, which is the
 * signature of a check that costs more than the thing it guards.
 *
 * SO THE CLAIM IS INDEXED WHERE IT IS MADE AND WHERE IT IS SPENT, and nowhere else has to remember. Made is
 * dom_undo_push — the one line every producer goes through, which is why the capture scope is asserted there
 * too — and spent is dom_release_created, the one place a kind-4 entry's node is nulled. No other operation on
 * a delta brings a claim into existence or ends one: a fork MOVES the head's entries into a segment, a park
 * MOVES the head's array to the flow, and a swap moves neither.
 *
 * WHICH IS WHY IT IS SWITCH-INVARIANT AND NO SWAP TOUCHES IT. A context switch changes which entries are
 * REACHABLE from the running flow (dom_buf_take/dom_buf_load hand the head over, dom_install_chain moves the
 * applied chain) and creates and spends nothing — so an index over EVERY live claim needs no patching at a
 * switch and has no way to drift out of step with one. It is deliberately NOT delta state: a per-flow set
 * carried in the delta would have to be applied, unapplied, forked and parked alongside it, and a malloc'd
 * side structure whose head is reverted by pointer at a switch is exactly the shape that leaves its contents
 * reachable from nothing. Nothing here is captured, so a flow-private creation is as invisible to the delta as
 * it was — the index records the ENTRY, and an entry only exists where one was already going to be written.
 *
 * AND THE QUESTION IT ANSWERS IS STRICTLY STRONGER THAN THE WALK'S, which is the point rather than a side
 * effect. The walk saw the RUNNING flow's chain only, so it answered "no claim" for one parked in another
 * flow's delta — and that is precisely the shape of the second way to reach the assert it serves: an arena
 * address recycled under an unspent claim is a document destroyed while some OTHER delta still named a node
 * inside it, and that delta is a parked one every time the running flow is the one creating. The index holds
 * every live claim there is, so it fires where the walk was silent, and what it names then is a delta that
 * will `destroy_deep` an address the allocator has already handed to somebody else.
 *
 * PLAIN calloc/free AND NOT reclaim_malloc, for re-entrancy and not for taste. A reclaim allocation SELLS A
 * FLOW; a sale releases DOM segments; dom_release_created is where a released segment spends its claims — so
 * an ask from inside this table's own rehash would mutate the table that rehash is standing on, through the
 * pointer it is copying out of. A direct call to the C library cannot ask, so it cannot re-enter. The cost of
 * that choice is that a dev build meets a real memory wall here instead of paging its tail out, which is the
 * right trade for one pointer per node a delta owns, in a structure that vanishes with the assert it serves. */
#if APICLIENT_DEV
/* A slot is NULL when it has never been used and DOM_CLAIM_SPENT when a claim that lived there was spent —
   the two cannot be one value, because a probe stops at the first NULL and a spent slot must not end a probe
   that was placed past it. */
#define DOM_CLAIM_SPENT ((const lxb_dom_node_t *)(uintptr_t)1)
static const lxb_dom_node_t **g_dom_claim;
static size_t g_dom_claim_cap;    /* a power of two, 0 before the first claim */
static size_t g_dom_claim_live;   /* claims standing */
static size_t g_dom_claim_used;   /* claims standing + spent slots — what the probe actually walks */
static unsigned g_dom_claim_bits;

/* The multiplicative ("Fibonacci") hash over the ADDRESS, taking the TOP `bits` of the product: a node comes
   out of an arena, so consecutive nodes differ by a fixed stride in the LOW bits, which a mask alone would
   turn into a run of collisions — the multiply is what carries that stride up into the bits the mask keeps. */
static size_t dom_claim_hash(const lxb_dom_node_t *n, unsigned bits)
{
    uint64_t h = (uint64_t)(uintptr_t)n * 0x9E3779B97F4A7C15ULL;
    return (size_t)(h >> (64 - bits));
}

/* The slot holding `n`, or NULL when no claim on it is standing. The probe terminates because the table is
   rehashed before it can fill: `used` never reaches `cap`, so a NULL slot always exists to stop at. */
static const lxb_dom_node_t **dom_claim_find(const lxb_dom_node_t *n)
{
    size_t mask, i;

    if (g_dom_claim_cap == 0)
        return NULL;
    mask = g_dom_claim_cap - 1;
    for (i = dom_claim_hash(n, g_dom_claim_bits) & mask; ; i = (i + 1) & mask) {
        const lxb_dom_node_t *k = g_dom_claim[i];
        if (k == NULL) return NULL;
        if (k == n) return &g_dom_claim[i];
    }
}

/* Rebuild the table. It GROWS only when the STANDING claims are what filled it; a table full of spent slots is
   rebuilt at the same size, so a run that creates and discards forever does not climb. */
static void dom_claim_rehash(void)
{
    unsigned bits = g_dom_claim_bits;
    const lxb_dom_node_t **tab;
    size_t cap, i;

    if (bits == 0) bits = 9;                                            /* 512 slots for the first claim */
    else if ((g_dom_claim_live + 1) * 4 > ((size_t)1 << bits) * 3) bits++;
    cap = (size_t)1 << bits;
    tab = calloc(cap, sizeof *tab);
    CHECK(tab != NULL, "dom-cow-oom: the DOM creation-claim index could not be grown — the assert that keeps "
                       "one node from being claimed twice, and so destroyed twice, cannot be answered");
    for (i = 0; i < g_dom_claim_cap; i++) {
        const lxb_dom_node_t *k = g_dom_claim[i];
        size_t j;
        if (k == NULL || k == DOM_CLAIM_SPENT) continue;
        for (j = dom_claim_hash(k, bits) & (cap - 1); tab[j] != NULL; j = (j + 1) & (cap - 1)) ;
        tab[j] = k;
    }
    free(g_dom_claim);
    g_dom_claim = tab; g_dom_claim_cap = cap; g_dom_claim_bits = bits;
    g_dom_claim_used = g_dom_claim_live;   /* the spent slots are gone with the old table */
}

/* Record that a kind-4 entry now claims `n`. */
static void dom_claim_note(const lxb_dom_node_t *n)
{
    size_t mask, i, reuse;

    DCHECK(n != NULL, "a DOM creation claim was indexed for no node — NULL is this table's empty slot, so "
                      "storing one would make every later probe stop at it and report every claim past it as "
                      "absent");
    if ((g_dom_claim_used + 1) * 4 > g_dom_claim_cap * 3)
        dom_claim_rehash();
    mask = g_dom_claim_cap - 1;
    reuse = g_dom_claim_cap;
    for (i = dom_claim_hash(n, g_dom_claim_bits) & mask; ; i = (i + 1) & mask) {
        const lxb_dom_node_t *k = g_dom_claim[i];
        if (k == NULL) break;
        if (k == DOM_CLAIM_SPENT) { if (reuse == g_dom_claim_cap) reuse = i; continue; }
        DCHECK(k != n, "the creation-claim index was asked to record a claim it already holds — dom_undo_push "
                       "is the only writer and dom_cow_note_created asserts against this same index before it "
                       "pushes, so the two disagree and a node is about to be owned by two entries");
    }
    if (reuse != g_dom_claim_cap) { g_dom_claim[reuse] = n; }
    else                          { g_dom_claim[i] = n; g_dom_claim_used++; }
    g_dom_claim_live++;
}

/* The claim on `n` has been spent — its entry is about to be nulled and the node destroyed. */
static void dom_claim_forget(const lxb_dom_node_t *n)
{
    const lxb_dom_node_t **slot = dom_claim_find(n);

    DCHECK(slot != NULL, "a DOM creation claim was spent that the index never held — every kind-4 entry is "
                         "indexed by dom_undo_push, so a claim missing here is one written past that push, "
                         "and the index now under-reports: the next creation at this address passes the "
                         "double-ownership assert and the node is destroyed twice");
    *slot = DOM_CLAIM_SPENT;
    g_dom_claim_live--;
}

static bool dom_claim_holds(const lxb_dom_node_t *n) { return dom_claim_find(n) != NULL; }
#else
/* Release: the index exists only to answer a DCHECK, which is compiled out with it. The query survives as a
   definition because a compiled-out DCHECK still type-checks its condition. */
static void dom_claim_note(const lxb_dom_node_t *n) { (void)n; }
static void dom_claim_forget(const lxb_dom_node_t *n) { (void)n; }
static bool dom_claim_holds(const lxb_dom_node_t *n) { (void)n; return false; }
#endif

static void dom_undo_push(DomUndo u) {
    /* THE ONE LINE EVERY PRODUCER GOES THROUGH, which is why the scope is asserted here and not at each of the
       ten of them: an eleventh entry kind written tomorrow does not get to choose whether it declares the read
       it is standing on, it aborts at its first push. */
    DCHECK(g_dom_capturing > 0,
           "a DOM delta entry was written outside a capture scope — this push's own realloc can SELL A FLOW, "
           "and the entry it is about to store holds raw pointers into the document (an element, an attribute "
           "node, a parent/next link), so nothing would catch the applied chain moving under the read they "
           "came from");
    if (g_dom_undo_n >= g_dom_undo_cap) {
        int nc = g_dom_undo_cap ? g_dom_undo_cap * 2 : 64;
        /* Skipping a DOM capture silently breaks DOM isolation (this flow's DOM write never reverts -> leaks
           into the next flow's baseline -> wrong sinks/taint). OOM is a should-never-happen: CRASH, never skip.
           AND THIS IS THE SITE THAT PROVED THE THIRD ALLOCATOR EXISTS. With the refusal edge in the runtime's
           allocator and in lexbor's, a 2 GB wall run died HERE, with 9457 flows live and not one of them
           paged, because this log is grown by a direct call to the C library and so had nothing to ask. It
           asks now (solver/reclaim.h) and the CHECK stays: the edge makes the failure recoverable, and the
           CHECK is still the answer when the engine has no flow left to sell.
           THE RECLAIM RUNS THIS FILE'S OWN CODE — selling a flow frees DOM segments and destroys the nodes
           they created — so the one-deep latch inside JS_ReclaimMemory is load-bearing here rather than
           defensive: the frees it performs allocate at most plainly and cannot ask again from inside the ask.
           `g_dom_undo` is the RUNNING flow's head and the sale never takes the running flow (asserted in
           engine_reclaim_tail), so the buffer this is growing is not one the sale can free. */
        DomUndo *n = reclaim_realloc(g_dom_undo, (size_t)nc * sizeof(DomUndo));
        CHECK(n, "dom-cow-oom: DOM undo-log realloc failed — DOM isolation would be silently corrupted");
        g_dom_undo = n; g_dom_undo_cap = nc;
    }
    g_dom_undo[g_dom_undo_n++] = u;
    /* AFTER THE STORE AND AFTER EVERY ALLOCATION THAT COULD SELL A FLOW. A sale spends claims through
       dom_release_created, so an index write interleaved with the grow above would be a write into a table a
       nested spend is editing; by here the reclaim_realloc has returned and nothing else on this path asks. */
    if (u.kind == 4)
        dom_claim_note(u.node);
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
    dom_capture_begin();   /* `a`, `cur` and `prefix` are the tree's, held across four allocations that can sell */
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
        u.prefix = reclaim_malloc(pl + 1);
        CHECK(u.prefix, "dom-cow-oom: attr prefix copy failed");
        memcpy(u.prefix, prefix, pl); u.prefix[pl] = 0;
    }
    if (cur) { u.old = reclaim_malloc(vl ? vl : 1); CHECK(u.old, "dom-cow-oom: baseline attr snapshot malloc failed — the delta could not restore its baseline"); memcpy(u.old, cur, vl); u.old_len = vl; }
    u.sh_old = shadow_snapshot(el, ATTR_SLOT_ATTRIBUTE, ns, local, &u.sh_had);   /* baseline TAINT reverts with the value */
    u.sh_cur = JS_UNDEFINED;
    dom_undo_push(u);
    dom_capture_end();
}

/* The PROPERTY-slot twin of dom_attr_capture: a DOM property's taint, whose value half is already captured as
   Text nodes. Same entry kind, same revert/unapply/apply arms — only the Lexbor value work is skipped. */
static void dom_prop_taint_capture(lxb_dom_element_t *el, const char *name) {
    if (!g_dom_capture) return;
    DomUndo u; memset(&u, 0, sizeof u);
    dom_capture_begin();
    u.kind = 0; u.slot = ATTR_SLOT_PROPERTY; u.el = el; u.sh_owner = el; u.name = strdup(name); u.had = 0;
    CHECK(u.name, "dom-cow-oom: property name strdup failed");
    u.sh_old = shadow_snapshot(el, ATTR_SLOT_PROPERTY, NULL, name, &u.sh_had);
    u.sh_cur = JS_UNDEFINED;
    dom_undo_push(u);
    dom_capture_end();
}

/* THE PROPERTY-TAINT CHOKEPOINT — capture-then-set, like every other DOM write. `opaque` JS_UNDEFINED clears. */
void dom_cow_set_prop_taint(JSContext *ctx, lxb_dom_element_t *el, const char *name, JSValueConst opaque) {
    dom_prop_taint_capture(el, name);
    attr_shadow_set(ctx, el, ATTR_SLOT_PROPERTY, NULL, name, opaque);
}
/* THE TWO STRUCTURAL CAPTURE PRIMITIVES — record the write BEFORE it happens, and nothing else. They are the
   whole of what an insert and a removal have to tell the delta, which is why every structural mutation op in
   this file is one of them plus one lexbor call plus whatever §4.2.3 steps that op owes. They are STATIC: an
   exported one had no caller outside this file, and a raw capture reached from outside is a write with no
   mutation beside it — the half of the pair that cannot be checked. */
static void dom_insert_capture(lxb_dom_node_t *node) {
    if (!g_dom_capture) return;
    DomUndo u; memset(&u, 0, sizeof u); u.kind = 1; u.node = node; u.sh_old = u.sh_cur = JS_UNDEFINED;
    dom_capture_begin();
    dom_undo_push(u);
    dom_capture_end();
}
/* The removal twin: the node AND THE POSITION IT SAT AT, read off the live tree before the unlink, because
   `lxb_dom_node_remove` NULLs `parent`, `next` and `prev` and there is no answer to "where was it" afterwards.
   Three callers pushed this identical block; a fourth would have been the one that forgot the next sibling. */
static void dom_remove_capture(lxb_dom_node_t *node) {
    if (!g_dom_capture) return;
    DomUndo u; memset(&u, 0, sizeof u);
    dom_capture_begin();   /* the position is read off the live tree and written down by the push */
    u.kind = 2; u.node = node;
    u.parent = node->parent; u.next = node->next;
    u.sh_old = u.sh_cur = JS_UNDEFINED;
    dom_undo_push(u);
    dom_capture_end();
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
    char *p;
    /* `s` IS THE INTERN TABLE'S OWN BYTES — the callers read a namespace, a prefix and a local name off a live
       attribute and hand each one straight here, so the read is held across this allocation exactly as a
       producer's is. One scope at the copy covers every identity read rather than one per caller. */
    dom_capture_begin();
    p = reclaim_malloc(n + 1);
    CHECK(p != NULL, "dom-cow-oom: an attribute identity could not be copied");
    memcpy(p, s, n); p[n] = 0;
    dom_capture_end();
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
    dom_capture_begin();   /* `v` is the detached attribute's own storage, held across the copy below */
    u.kind = 7; u.node = lxb_dom_interface_node(a); u.sh_old = u.sh_cur = JS_UNDEFINED;
    v = lxb_dom_attr_value(a, &vl);
    if (v) {
        u.old = reclaim_malloc(vl ? vl : 1);
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
    dom_capture_end();
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
    size_t old_len = 0;
    char *old = (g_attr_hook && g_cow_ctx) ? attr_old_value(el, ns, local, &old_len) : NULL;

    dom_attr_capture(el, ns, local);   /* baseline into the running flow's delta FIRST (no-op if !g_dom_capture) */
    {
        bool created = false;
        lxb_dom_attr_t *a = dom_attr_write(el, ns, prefix, local, val, val_len, &created);
        /* AN ATTRIBUTE A FLOW CREATES IS THE FLOW'S, and the write is the one place that knows it made one. */
        if (created) dom_note_created_attr(a);
    }
    if (g_cow_ctx) attr_shadow_set(g_cow_ctx, el, ATTR_SLOT_ATTRIBUTE, ns, local, taint);
    /* AFTER the write — §9.4.6 step 2 stores the value and step 3 handles the change, in that order, so a
       page's attributeChangedCallback reading the attribute back sees the value it was told about and
       §4.2.2's slot steps re-derive a slottable's name from the attribute that is now there. It used to fire
       BEFORE, with a comment claiming the standard says so; it does not. */
    if (g_attr_hook && g_cow_ctx) g_attr_hook(g_cow_ctx, el, ns, local, old, old_len, val, val_len);
    free(old);
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
    /* §9.4.7 step 4: handle attribute changes with the attribute's value as the OLD one and NULL as the new,
       AFTER step 2 took it out of the list. The value is still on the detached node, which is what §9.4.7
       leaves alive. */
    if (g_attr_hook && g_cow_ctx) {
        size_t vl = 0;
        const lxb_char_t *v = lxb_dom_attr_value(a, &vl);
        g_attr_hook(g_cow_ctx, el, id.ns, id.local, (const char *)v, v ? vl : 0, NULL, 0);
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
        size_t old_len = 0;
        char *old_val = (g_attr_hook && g_cow_ctx) ? attr_old_value(el, id.ns, id.local, &old_len) : NULL;

        dom_attr_capture(el, id.ns, id.local);
        v = lxb_dom_attr_value(a, &vl);
        if (old) dom_attr_replace(old, a);             /* step 6 — in place, keeping the index */
        else dom_attr_attach(el, a, NULL);             /* step 7 */
        /* §9.4.8 step 6: ONE change notification, reported against the OLD attribute's name and namespace with
           the NEW attribute's value — and after the list write, like every other. */
        if (g_attr_hook && g_cow_ctx)
            g_attr_hook(g_cow_ctx, el, id.ns, id.local, old_val, old_len,
                        (const char *)(v ? v : (const lxb_char_t *)""), vl);
        free(old_val);
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
    if (g_tree_hook) g_tree_hook(g_cow_ctx, node, node->parent, 0);
    dom_remove_capture(node);
    {
        /* §4.2.3 remove step 3 detaches, and steps 4-7 run AFTER it — the slot steps recompute a slot's
           assigned nodes, and doing that while the node is still a child finds it again and reports no change.
           The old parent is carried across the detach because the node no longer has one to be asked for. */
        lxb_dom_node_t *was = node->parent;

        lxb_dom_node_remove(node);
        if (g_tree_hook) g_tree_hook(g_cow_ctx, node, was, -1);
    }
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

/* THE TREE A PARSE BUILDS, WHICH IS NOT THE SAME WALK, because a parse builds MORE THAN ONE tree and only one
   of them is reached by child links. HTML §13.2.6.4.4 'The "in head" insertion mode' gives a `<template>` its
   own DocumentFragment for §4.12.3's template contents and tree construction inserts the markup into THAT — so
   `<template><b>x</b></template>` puts `b` in a root whose parent is null, reachable from the document only
   through the host element. A walk that stopped there would call every template's contents a tree of its own,
   and a parse's declared root would match nothing inside one.
   THE STEP THROUGH THE HOST IS core/dom/node.h's `node_template_content_host`, which is that header's round
   trip and not a type test: a shadow root is a DocumentFragment with a host too, and a parse produces none
   (§13.2.6.4.4's declarative conversion runs at the parse BOUNDARY over the finished fragment), so one
   appearing under a parse is a tree that parse never built and must not be climbed out of. */
static lxb_dom_node_t *dom_parse_root_of(lxb_dom_node_t *n) {
    for (;;) {
        lxb_dom_node_t *host;

        while (dom_is_attached(n)) n = n->parent;
        host = node_template_content_host(n);
        if (host == NULL) return n;
        n = host;
    }
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
 * handed to a capturing insert, and that insert is the write another flow could see.
 *
 * AND THIS IS THE ONE SEAM AT WHICH A PARSE'S NODE BECOMES THE FLOW'S — THE OTHER HALF OF A RULE THAT ONLY
 * WORKS IF BOTH HALVES ARE KEPT. A parse declared PRIVATE records NOTHING: its nodes are owned by the private
 * ROOT (a Document the same operation created, which the delta owns as a kind-5 entry, or a fragment the parse
 * itself destroys), so there is one owner for the whole tree and no per-node claim to make. A node's own claim
 * is made HERE and only here, because leaving the private tree is exactly the moment the root stops being able
 * to free it. The two conventions are not interchangeable and they cannot be MIXED: a parse that records per
 * node AND a placement that records at this seam gives every placed node two kind-4 entries, and
 * dom_release_created destroys through the first and then destroys freed memory through the second. That is
 * what dom_cow_note_created's own assert now refuses. */
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
   any tree is one some flow can reach, and moving it here would be a structural change with no delta entry.
   THE DECLARED ROOT IS THE IMMEDIATE TREE and not the whole of what the caller is building: a clone walk that
   descends into a `<template>`'s content fragment re-declares THAT fragment as its root for the length of the
   descent, because the fragment is a root with no parent and its own tree in every walk's terms. HTML
   §13.2.6's members ask a different containment question over the same trees and state it themselves. */
void dom_cow_insert_private(lxb_dom_node_t *root, lxb_dom_node_t *parent, lxb_dom_node_t *child) {
    dom_private_check(root);
    DCHECK(parent && dom_root_of(parent) == root,
           "dom_cow_insert_private into a parent outside the declared private tree");
    DCHECK(child && !dom_is_attached(child),
           "dom_cow_insert_private with a child that is already in a tree — that is a MOVE, which is a "
           "structural change to wherever it came from and needs the capturing chokepoint");
    lxb_dom_node_insert_child(parent, child);
}

/* Is `a` `b` or one of its ancestors — the cycle question the move below asks. Dev-only, and it climbs by
   parent because that is the direction with a bound: the depth already built. */
static bool dom_is_inclusive_ancestor(const lxb_dom_node_t *a, const lxb_dom_node_t *b) {
    for (; b; b = b->parent)
        if (a == b) return true;
    return false;
}

/* MOVE between two private trees — see dom_cow.h for why this is neither of the two above. Both roots are
   checked, and the child is checked against the one it is leaving: a child that is not in `from_root` is a
   node this caller does not own, and moving one of those is a structural change to a tree some flow's baseline
   still holds. Nothing is captured, because nothing shared changes at either end. */
void dom_cow_move_private(lxb_dom_node_t *from_root, lxb_dom_node_t *to_root,
                          lxb_dom_node_t *parent, lxb_dom_node_t *child) {
    dom_private_check(from_root);
    dom_private_check(to_root);
    DCHECK(child && dom_root_of(child) == from_root,
           "dom_cow_move_private with a child outside the private tree it was declared to be leaving");
    DCHECK(parent && dom_root_of(parent) == to_root,
           "dom_cow_move_private into a parent outside the declared destination tree");
    DCHECK(!dom_is_inclusive_ancestor(child, parent),
           "dom_cow_move_private would put a node inside its own descendant — that is a cycle, and every tree "
           "walk in this engine loops on one forever");
    lxb_dom_node_remove(child);
    lxb_dom_node_insert_child(parent, child);
}

/* INSERT BEFORE A SIBLING, inside one private tree — the positional form of dom_cow_insert_private.
 *
 * IT EXISTS BECAUSE POSITION IS THE WHOLE POINT OF THE OPERATION THAT NEEDED IT. HTML §8.6.4 step 1.5.2.5
 * replaces an element with its children IN ITS PLACE, and an append would put them at the END of their new
 * parent — the sanitizer would reorder the page's markup while claiming only to have removed an element, which
 * is a wrong answer that looks like a correct one. Every other private operation happens to be an append
 * because a parse builds in order; this one is not building, it is splicing.
 *
 * `ref` IS A CHILD OF `parent`, not merely in the tree, and that is asserted rather than trusted: lexbor's
 * insert-before writes links relative to `ref`, so a `ref` under a different parent silently detaches a subtree
 * from somewhere else in the same private tree.
 * Nothing is captured, for the reason none of these capture: the tree is the running flow's own and no other
 * flow can observe it. */
void dom_cow_insert_before_private(lxb_dom_node_t *root, lxb_dom_node_t *ref, lxb_dom_node_t *child) {
    dom_private_check(root);
    DCHECK(ref && dom_root_of(ref) == root,
           "dom_cow_insert_before_private before a reference node outside the declared private tree");
    DCHECK(ref->parent != NULL,
           "dom_cow_insert_before_private before the ROOT of the private tree — there is no position before it "
           "and lexbor would link the child to a null parent");
    DCHECK(child && !dom_is_attached(child),
           "dom_cow_insert_before_private with a child that is already in a tree — that is a MOVE, which is a "
           "structural change to wherever it came from and needs the capturing chokepoint");
    DCHECK(!dom_is_inclusive_ancestor(child, ref),
           "dom_cow_insert_before_private would put a node inside its own descendant — that is a cycle, and "
           "every tree walk in this engine loops on one forever");
    lxb_dom_node_insert_before(ref, child);
}

/* MOVE a node to a POSITION in a private tree — dom_cow_move_private's positional twin, and the one §8.6.4
   step 1.5.2.5 actually performs: the children being spliced into their grandparent are already in the tree,
   so they are moved rather than inserted. Both ends are the same declared private tree here, which is what
   distinguishes it from move_private's two-root form. */
void dom_cow_move_before_private(lxb_dom_node_t *root, lxb_dom_node_t *ref, lxb_dom_node_t *child) {
    dom_private_check(root);
    DCHECK(ref && dom_root_of(ref) == root,
           "dom_cow_move_before_private before a reference node outside the declared private tree");
    DCHECK(ref->parent != NULL,
           "dom_cow_move_before_private before the ROOT of the private tree — there is no position before it");
    DCHECK(child && dom_root_of(child) == root,
           "dom_cow_move_before_private with a child outside the private tree it was declared to move within");
    DCHECK(!dom_is_inclusive_ancestor(child, ref),
           "dom_cow_move_before_private would put a node inside its own descendant — that is a cycle");
    lxb_dom_node_remove(child);
    lxb_dom_node_insert_before(ref, child);
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
    /* THE WRAPPERS AND THE TAINT GO BACK INSIDE THE DESTROY, not in a sweep this caller performs first. A walk
       here would be one of a list of callers that must remember, and this file HELD that list — four of them,
       while a document's whole tree died through dom_document_destroy with none. core/dom/node_interface.c's
       dispatcher is what every `lxb_dom_node_destroy_deep` reaches, per node, so there is nothing to remember. */
    /* THE CLAIM IS SPENT BEFORE THE NODE IS, so the index never names an address the allocator has taken
       back — a stale key would make the next node handed out at that address fail the double-ownership assert
       for a claim nobody holds. */
    dom_claim_forget(u->node);
    lxb_dom_node_destroy_deep(u->node);
    u->node = NULL;   /* the entry has spent its claim; nothing may act on it twice */
}

/* EVERY CREATION IN A DELTA GOES BACK, NODES FIRST AND DOCUMENTS AFTER — see dom_cow_note_created_document for
 * why the order is load-bearing rather than tidy. One function because the order is a property of the RELEASE
 * and not of any one caller: three of them discard a delta, and an order restated three times is an order two
 * of them can lose. */
/* AN ATTRIBUTE THIS FLOW CREATED — kind 4's twin, released BEFORE it. See the ordering in
 * dom_release_created_all: an attribute lives in an ELEMENT's list, and lexbor's element destroy frees every
 * attribute the element still holds. */
static void dom_release_created_attr(DomUndo *u)
{
    lxb_dom_attr_t *a;

    if (u->kind != 6 || !u->node)
        return;
    a = lxb_dom_interface_attr(u->node);
    DCHECK(a->owner == NULL,
           "a flow's created attribute was still ON an element when its delta was discarded — the revert runs "
           "first and detaches it through its kind-0 entry, so an attached one means that entry is missing");
    /* THE WRAPPER AND THE TAINT SHADOW GO BACK INSIDE dom_attr_destroy, which is the point an Attr's death
       converges on — a detached attribute keys its shadow on ITSELF, and lexbor hands attributes out of the
       agent's pool, so an entry left naming this address is inherited by the next attribute allocated at it. */
    dom_attr_destroy(a);
    u->node = NULL;   /* the entry has spent its claim; nothing may act on it twice */
}

static void dom_release_created_all(DomUndo *e, int n)
{
    int i;

    /* ATTRIBUTES BEFORE NODES, for the same reason nodes come before documents below and one level down: an
       attribute lives in an ELEMENT's attribute list, and lexbor's `lxb_dom_element_interface_destroy` walks
       that list and destroys every attribute still on it. A flow that clones an element and then writes an
       attribute on the copy records the element (kind 4) before the attribute (kind 6), so releasing in entry
       order freed the element first and the attribute's own release then read memory that was already gone —
       `Node-cloneNode.html` SIGSEGV. */
    for (i = 0; i < n; i++) dom_release_created_attr(&e[i]);
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
    dom_capture_begin();
    u.kind = 6; u.node = lxb_dom_interface_node(a); u.sh_old = u.sh_cur = JS_UNDEFINED;
    dom_undo_push(u);
    dom_capture_end();
}

void dom_cow_note_created(lxb_dom_node_t *node)
{
    DomUndo u;
    if (!g_dom_capture || !node)
        return;
    /* ONE CLAIM PER NODE, ASSERTED WHERE THE CLAIM IS MADE — which is the only place it can be asserted once
       rather than at each of the twenty-odd callers. A second entry over one node is not a duplicate record,
       it is a second `lxb_dom_node_destroy_deep` at discard: dom_release_created nulls ITS OWN entry and knows
       nothing of the other, so the second call reads a freed node's `first_child` and frees it again.
       THE TWO WAYS TO REACH IT ARE BOTH CONVENTION MISTAKES, and the message names both because the fix
       differs. (a) A PRIVATE parse that records per created node, whose product is then placed through
       dom_cow_take_private — that parse must record nothing and let this seam make the claim (see
       dom_cow_take_private). (b) A node whose address lexbor has RECYCLED out of an arena while some delta
       still holds a live claim on the old occupant — which is a document freed out from under an unspent
       entry, and the entry, not this creation, is the bug.
       THE ANSWER COMES FROM THE CLAIM INDEX AND NOT FROM A WALK OF THIS FLOW'S DELTA, which is what makes (b)
       reachable at all: the delta holding the stale claim is a PARKED one every time the running flow is the
       one doing the creating, and a walk of the running chain says nothing whatever about those. */
    DCHECK(!dom_claim_holds(node),
           "a node was recorded as a creation while a live delta entry — in ANY delta, running or parked — "
           "already claims that address. Ownership "
           "is recorded ONCE: a parse declared DOM_PARSE_ROOT_PRIVATE records nothing and its nodes are owned "
           "by the private root, and a node's own claim is made where it LEAVES that root "
           "(dom_cow_take_private) — so a parse that also records per node double-claims every node it places, "
           "and dom_release_created destroy_deeps freed memory on the second entry. The other way here is an "
           "arena address recycled under an unspent claim, which is a document destroyed while a delta still "
           "named a node inside it");
    memset(&u, 0, sizeof u);
    dom_capture_begin();
    u.kind = 4; u.node = node; u.sh_old = u.sh_cur = JS_UNDEFINED;
    dom_undo_push(u);
    dom_capture_end();
}

void dom_cow_destroy_document(lxb_html_document_t *dom)
{
    DCHECK(dom != NULL, "a document destroy was asked for no document");
    /* THE RECORD NAMES THE TREE, so it cannot outlive it — and it holds the document's wrapper and its
       DOMImplementation, which is one struct and two references per document any exploration arm ever built. */
    document_record_release(dom);
    /* The tree's wrappers and taint go back inside dom_document_destroy — per node through the destroy
       dispatcher, and the document's own node in that function itself. This entry point is left holding only
       the fact no destroy could know: that the flow's delta owned the document. */
    dom_document_destroy(dom);
}

/* THIS FLOW CREATED THIS DOCUMENT — the delta's fifth kind of record, and the reason it is a KIND rather than a
 * fourth-kind entry over the document's root node: destroying a document is `dom_document_destroy`, which frees
 * every node the document still owns and hands the agent's arenas back, and kind 4's lxb_dom_node_destroy_deep
 * would free the root and leave the rest of the tree.
 * THAT ORDER IS ALSO WHY THE RELEASE IS TWO PASSES. A node this flow created INSIDE a document this flow
 * created is still a child of it, so the document's own destroy would free it a second time — every node goes
 * back before any document does, and the document then finds nothing of the flow's left to free. */
bool dom_cow_note_created_document(lxb_html_document_t *dom)
{
    DomUndo u;
    DCHECK(dom != NULL, "a created document was noted for no document");
    if (!g_dom_capture)
        return false;
    memset(&u, 0, sizeof u);
    dom_capture_begin();
    u.kind = 5; u.doc = dom; u.sh_old = u.sh_cur = JS_UNDEFINED;
    dom_undo_push(u);
    dom_capture_end();
    return true;
}

/* THE ONE ELEMENT OF THIS TREE WHOSE SECOND TREE THE FREE BELOW CANNOT REACH, or NULL. It is the condition of
   the assertion under it and nothing else, so it runs in a dev build and vanishes with the assert — the same
   shape dom_delta_removed and dom_parse_root_of already have in this file.
   IT WALKS EXACTLY WHAT THE FREE FREES, because an assertion over a different set answers a different
   question: child links, and a `<template>`'s CONTENT fragment, which is not a child and which
   core/dom/node_interface.c's destroy dispatcher follows for every node it is handed. A SHADOW ROOT is the
   third tree and that dispatcher does not follow it — DOM §4.9 Interface Element's `attach a shadow root`
   writes the element -> shadow root edge as a slot on the HOST'S WRAPPER, so no C walk can see it and the
   dispatcher holds no realm to read it with (core/dom/node_heap.c's teardown DCHECK says exactly that). This
   is where there IS a realm, which is why the question is asked here and why this entry takes one. */
static lxb_dom_element_t *dom_private_shadow_host_in(JSContext *ctx, lxb_dom_node_t *root)
{
    lxb_dom_node_t *n = root, *content, *host;

    for (;;) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT &&
            shadow_root_of_element(ctx, lxb_dom_interface_element(n)) != NULL)
            return lxb_dom_interface_element(n);
        /* The content fragment BEFORE the element's own children, so the climb back out of it lands on those
           children and neither list is walked twice. HTML §4.12.3 keeps them as two separate lists. */
        content = node_template_content(n);
        if (content != NULL && content->first_child != NULL) { n = content->first_child; continue; }
        if (n->first_child != NULL) { n = n->first_child; continue; }
        for (;;) {
            if (n == root) return NULL;      /* asked FIRST: a detached root's own sibling links are nobody's */
            if (n->next != NULL) { n = n->next; break; }
            if (n->parent != NULL) { n = n->parent; continue; }
            host = node_template_content_host(n);
            DCHECK(host != NULL,
                   "a private tree's shadow-host scan walked out of the tree it was given — the only root it "
                   "enters other than the top is a <template>'s content, which is reached through its host");
            n = host;
            if (n->first_child != NULL) { n = n->first_child; break; }
        }
    }
}

void dom_cow_destroy_private(JSContext *ctx, lxb_dom_node_t *root, bool with_children) {
    dom_private_check(root);
    /* THE SECOND TREE THIS FREE CANNOT REACH, asserted before anything is freed. A shadow root is not a child
       of its host, so `lxb_dom_node_destroy_deep` walks straight past it and the host goes while the root
       stays — reachable from a wrapper whose node is gone, with `sr->host` naming freed bytes from that
       instant until whatever else claims it gets round to it. It is not a leak today and that is the whole
       problem: `attach a shadow root` records the root as a CREATION on the running flow's delta, so a second
       owner frees it later, and the private-tree convention (dom_cow.h: one owner, the private root) is
       broken with nothing anywhere to say so. Build the single owner — see the shadow-host DCHECK in
       dom_private_copy_one, which is this same contract read forwards, and core/html/sanitizer.c's removal
       DCHECK, which is it read from a third side. */
    DCHECK(dom_private_shadow_host_in(ctx, root) == NULL,
           "a flow-private DOM tree is about to be freed with a SHADOW HOST still in it, and DOM §4.8's "
           "shadow root is not one of the host's children — so this free walks past it and the root outlives "
           "the host it names. Give a private tree ONE owner that covers its shadow roots (the forwards half "
           "of this is the shadow-host DCHECK in dom_private_copy_one) instead of leaving `attach a shadow "
           "root`'s creation entry to free it out of another flow's delta at another time");
    DCHECK(with_children || root->first_child == NULL,
           "a private tree was destroyed with children still in it — those nodes are about to be freed under "
           "whatever took a reference to them");
    /* `with_children` HAS TO MEAN SOMETHING. It was `(void)`'d and the free was lxb_dom_node_destroy, which
       frees exactly ONE node — so a caller that passed true had every child leaked with its parent link naming
       freed memory, while a sweep that ran here had already walked the WHOLE subtree and dropped those children
       out of the identity map, so a JS wrapper for one of them survived pointing at a node the map no longer
       knew. Nothing had caught it because the only caller passing true destroys a childless element.
       The argument was a claim about a capability that did not exist, which is the same defect as a comment
       where a DCHECK belongs. lxb_dom_node_destroy_deep is what a tree destroy is, and it is what the delta's
       own creation release already uses for a subtree a flow built — and it is also what now hands every one of
       those nodes' wrappers back, one per node, from inside the destroy. */
    if (with_children) lxb_dom_node_destroy_deep(root);
    else               lxb_dom_node_destroy(root);
}

/* THE NODE MAP'S TWO OPERATIONS. lexbor's AVL is keyed on a `size_t`, which a node ADDRESS is; both sides of
   every entry are pointers into the same document's arena. */
static void dom_private_map_put(lexbor_avl_t *avl, lexbor_avl_node_t **avl_root,
                                const lxb_dom_node_t *src, lxb_dom_node_t *dst)
{
    DCHECK(src != NULL && dst != NULL, "a private-tree copy's node map was given half of a pair");
    /* A SECOND ENTRY FOR ONE SOURCE NODE IS A WALK THAT REACHED IT TWICE, and the later one is what a lookup
       would answer — so a cursor would be re-pointed at a copy nothing is linked to. The walk reaches a
       `<template>`'s content through its element and every other node through child links exactly once. */
    DCHECK(lexbor_avl_search(avl, *avl_root, (size_t)(uintptr_t)src) == NULL,
           "a private-tree copy reached one source node twice — the second copy is what a cursor would be "
           "re-pointed at, and nothing is linked to it");
    CHECK(lexbor_avl_insert(avl, avl_root, (size_t)(uintptr_t)src, dst) != NULL,
          "a fork could not record a node in the map for the private tree it is copying");
}

static lxb_dom_node_t *dom_private_map_get(lexbor_avl_t *avl, lexbor_avl_node_t *avl_root,
                                           const lxb_dom_node_t *src)
{
    lexbor_avl_node_t *e;
    if (src == NULL) return NULL;
    e = lexbor_avl_search(avl, avl_root, (size_t)(uintptr_t)src);
    return e != NULL ? (lxb_dom_node_t *)e->value : NULL;
}

/* ONE NODE OF A PRIVATE TREE, COPIED — lexbor's own clone for this document, which is the code that MADE the
   node, so attributes, namespaces and every per-interface field are copied by it and not by a second answer
   written here; then DOM §4.4 Interface Node's step 3 cloning steps, which are what carries the per-flow state
   that lives on the node's WRAPPER rather than on the node.
   THE TWO THIS CANNOT ANSWER ARE ASKED FIRST AND CRASH — see the banner below. */
static lxb_dom_node_t *dom_private_copy_one(JSContext *ctx, lxb_dom_document_t *doc, const lxb_dom_node_t *src)
{
    lxb_dom_node_t *c;

    if (src->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        DCHECK(shadow_root_of_element(ctx, lxb_dom_interface_element(src)) == NULL,
               "a private tree a fork must copy holds a SHADOW HOST. THE REMEDY THAT STOOD HERE IS RETIRED "
               "AND IT WAS SPEC-WRONG: it said to give DOM §4.4 Interface Node's clone-a-node step 6 a caller "
               "that can be abrupt. Step 6 runs only \"If node is an element, node is a shadow host, and "
               "node's shadow root's clonable is true\", and DOM §4.8 Interface ShadowRoot says of that field "
               "\"Shadow roots have an associated clonable (a boolean). It is initially set to false\" — so a "
               "fork built on step 6 would SILENTLY DROP every non-clonable shadow root from the sibling's "
               "copy, which is the corruption a fork abort exists to prevent, arriving through the fix. The "
               "throw is a SYMPTOM of calling a page-visible clone: step 6.4 is `attach a shadow root` and "
               "the refusals are DOM §4.9 Interface Element's. A fork's copy is not a clone — it is a copy "
               "the sibling must be unable to tell it took — so a faithful copy calls no attach and there is "
               "nothing left for a visit to have to carry out. BUILD IT IN THIS ORDER. "
               "(1) THE OWNER, which is the subproblem and is not the walk: a shadow root attached to a host "
               "inside a private tree has TWO candidate owners — `attach a shadow root` records a creation on "
               "the running flow's delta (kind 4) while every other node of that tree is owned by the private "
               "root, which is the convention mixing dom_cow_note_created's own assert refuses one node at a "
               "time and cannot see here, because the private root makes no claim to collide with. The delta "
               "cannot own a COPY either: a step machine's tree is cloned BEFORE dom_cow_fork freezes the "
               "head, so the claim would land in a base segment BOTH arms reference. One owner — the private "
               "root owns what is under it, shadow roots included, and no creation is recorded for a node "
               "inside a declared private tree, which needs dom_cow to be able to answer WHICH trees are "
               "declared rather than only asserting privacy per call. "
               "(2) THE COPY, once there is an owner: a ShadowRoot node in this same document whose host is "
               "the copy and whose mode is the original's, §4.8's record copied FIELD FOR FIELD (`available "
               "to element internals` COPIED and never recomputed — §4.9's attach derives it from the host's "
               "custom element state, and a fork copies state rather than deriving it), the registry "
               "association carried over, and the shadow children copied by this same walk with the new root "
               "as its `croot` and the climb out through the host. THE DESTROY OWES THE SAME ROOT COMING "
               "BACK and now crashes for it in dom_cow_destroy_private (core/html/sanitizer.c's removal "
               "DCHECK is that half from a third side). A fragment parse reaches this because "
               "declarative_shadow_parsed runs at its FRAG_FEED boundary, before the placement and before "
               "§8.6.4's walk");
        DCHECK(!media_element_is(src),
               "a private tree a fork must copy holds a MEDIA ELEMENT, whose §4.8.11 state is on its wrapper "
               "and whose resource-selection job is already enqueued NAMING THE ORIGINAL's wrapper — and "
               "§4.8.11 states no cloning steps, so there is nothing to call and the sibling arm would get a "
               "media element in a network state nothing put it in. BUILD THE FORK'S ANSWER for a pending job "
               "that names a node the fork has just copied; media_element_parsed at the FRAG_FEED boundary is "
               "what enqueues it");
    }
    c = lxb_html_interface_clone(doc, src);
    CHECK(c != NULL, "a fork could not copy a node of the flow-private DOM tree a step machine declared");
    /* THE COPY IS IN THE SAME DOCUMENT AS THE ORIGINAL. lxb_dom_node_interface_copy takes its
       `dst->owner_document == src->owner_document` path and assigns local_name, ns and prefix verbatim, so no
       name is re-interned and a placement out of the copy still needs no §4.5 adopt. The moment that stops
       being true every tag and attribute id lands in another hash, silently and correctly-looking. */
    DCHECK(c->owner_document == src->owner_document,
           "a copied flow-private node belongs to a different document than the node it was copied from — its "
           "interned tag and attribute ids are then meaningless against the document it will be placed into");
    /* §4.4 STEP 3's CLONING STEPS, which are these two component entries and not a list restated here — the
       same two core/dom/node.c's clone-a-node machine calls at its own step 3. HTML §4.12.1's pair carries a
       `<script>`'s `already started`, without which a fork undoes §13.4's Inert mode; HTML §2.5.6's carries
       [[CryptographicNonce]] for every HTML element, and the attribute the clone above already copied is the
       STALE half of that pair by design. Both are unconditional on §4.4's `subtree`, so both run per node. */
    html_script_cloned(ctx, (lxb_dom_node_t *)src, c);
    nonce_attribute_cloned(ctx, (lxb_dom_node_t *)src, c);
    /* AND NOTHING IS NOTED CREATED. The whole copy has ONE owner — the sibling's own declared tree slot, which
       its teardown destroys — so a per-node creation claim would be a second owner for every node, and the
       discard would destroy through the first and then through freed memory. That is dom_cow.h's one
       ownership convention over a private tree, kept here as every parse that feeds one keeps it. */
    return c;
}

/* The tree a copied node belongs to — the copy's top, or a `<template>`'s content fragment. Both are roots
   with no parent, which is exactly the question dom_cow's private-tree declaration asks. */
static lxb_dom_node_t *dom_private_root_of(lxb_dom_node_t *n)
{
    while (n->parent != NULL) n = n->parent;
    return n;
}

/* WHERE THE WALK GOES AFTER `n`'s SUBTREE IS FINISHED. Iterative, and it climbs by `parent` and by a content
   fragment's `host` rather than carrying a level stack: those two links are the only way into or out of a
   level and the tree already holds both, so the depth here is the page's markup and none of it is on the C
   stack. `croot` changes at the one transition that changes it — leaving a `<template>`'s content for the
   template element's OWN children, which HTML §4.12.3 keeps as two separate child lists. */
static lxb_dom_node_t *dom_private_walk_next(lxb_dom_node_t *n, const lxb_dom_node_t *top,
                                             lexbor_avl_t *avl, lexbor_avl_node_t *avl_root,
                                             lxb_dom_node_t **croot)
{
    for (;;) {
        lxb_dom_node_t *up, *host, *host_copy;

        if (n->next != NULL) return n->next;
        up = n->parent;
        if (up == NULL) {
            /* A CONTENT FRAGMENT HAS NO PARENT — it is reached through its host, which is the one link out of
               it. Anything else with no parent is a second tree this walk was never given. */
            host = node_template_content_host(n);
            DCHECK(host != NULL,
                   "a private-tree copy walked out of the tree it was given — the only root it enters other "
                   "than the top is a <template>'s content fragment, which is reached through its host");
            host_copy = dom_private_map_get(avl, avl_root, host);
            DCHECK(host_copy != NULL,
                   "a private-tree copy left a <template>'s content and its host has no copy — the host is "
                   "copied before its content is walked, so the map cannot be missing it");
            *croot = dom_private_root_of(host_copy);
            n = host;
            if (n->first_child != NULL) return n->first_child;
            continue;
        }
        if (up == top) return NULL;
        DCHECK(dom_private_map_get(avl, avl_root, up) != NULL,
               "a private-tree copy climbed into a node it never copied — the only trees it enters are the one "
               "it was given and a <template>'s content, and it copies every node of both");
        n = up;
    }
}

/* THE TWO SIDES OF A PRIVATE TREE A STEP MACHINE DECLARES — see dom_cow.h. Written as a pair because they are
 * one contract read in two directions: whatever the clone gives the sibling is exactly what the sibling's own
 * teardown will destroy, and a field either side names alone is a leak or a double free.
 *
 * THE MAP IS lexbor's OWN AVL, keyed on the source node's ADDRESS and carrying the copy — a pointer map with
 * no structure to invent, in the pinned tree, allocated out of a dobject it destroys with itself. It records a
 * copy that has just been made and nothing about it parks, so it is a STACK-LIFETIME artifact of this one call
 * and is not a field of anything. That is also why the cursors are an argument here rather than a slot kind of
 * their own: the only moment at which "which node does this cursor now name" has an answer is while this map
 * is alive.
 *
 * WHAT A COPY OWES BEYOND ITS SHAPE IS DOM §4.4 Interface Node's STEP 3 CLONING STEPS, and that list is the thing a
 * subtree copy is silently wrong without. A node's PER-FLOW state does not live on the lexbor node at all — it
 * lives on the node's JS WRAPPER, because §3.7 makes it a per-flow fact — so a walk that copied structure and
 * stopped would hand the sibling arm a `<script>` whose `already started` is FALSE for markup this parse
 * marked dead, and §13.4's Inert scripting mode would be undone by taking a fork. The steps are not this
 * file's to state: they are the component entries core/dom/node.c's clone-a-node machine calls at its own step
 * 3, called here from the one other walk that copies nodes, so there is one list and not two.
 *
 * THE TWO THE LIST DOES NOT COVER CRASH BY NAME rather than being skipped, because skipping is exactly the
 * shape that is wrong in silence. §4.4 step 6's SHADOW ROOT is the one this file used to describe as an
 * ABRUPTNESS problem, and that framing was wrong in a way worth keeping: step 6 is conditioned on the
 * original root's `clonable`, so it is a LOSSY, page-visible clone and a fork that ran it would drop a
 * non-clonable root from the sibling's copy in silence. A fork's copy is not a clone, so it calls no `attach
 * a shadow root` and inherits none of its refusals; what it is actually blocked on is WHO OWNS a shadow root
 * inside a private tree, which the DCHECK at dom_private_copy_one states and dom_cow_destroy_private states
 * back. §4.8.11's MEDIA ELEMENT keeps its state on the wrapper AND has already enqueued a resource-selection
 * job naming the ORIGINAL's wrapper, and §4.8.11 states no cloning steps at all, so there is nothing to call
 * and the sibling would silently get a media element in the wrong network state. Both are cases a seam BEFORE
 * the fork created (core/html/fragment_parser.c's FRAG_FEED boundary runs declarative_shadow_parsed and
 * media_element_parsed on the tree it hands on), which is why they are asked here per node and not once at
 * the root. */
static void *dom_private_tree_clone(JSContext *ctx, void *root, void **cursors[], int ncursors)
{
    lxb_dom_node_t *src = root, *top, *croot, *n;
    lexbor_avl_t *avl;
    lexbor_avl_node_t *avl_root = NULL;
    int i;

    dom_private_check(src);
    DCHECK(src->owner_document != NULL,
           "a flow-private DOM node with no owner document was declared to a fork — the copy is made against "
           "that document so its interned tag and attribute ids stay meaningful, and there is no other "
           "document this operation could name");
    avl = lexbor_avl_create();
    CHECK(avl != NULL && lexbor_avl_init(avl, 128, sizeof(lexbor_avl_node_t)) == LXB_STATUS_OK,
          "a fork could not create the node map for the flow-private tree a step machine declared");

    /* THE TOP IS COPIED HERE AND NOT BY THE LOOP, which is the whole difference between this walk and the one
       core/html/tree_construction.c performs over a partial parse: that one is handed the two temporary
       DOCUMENT nodes and starts at `src_top->first_child`, so a DETACHED root — which is what every private
       tree a step machine owns is — has no entry point into it at all. */
    top = dom_private_copy_one(ctx, src->owner_document, src);
    dom_private_map_put(avl, &avl_root, src, top);
    croot = top;
    n = src->first_child;
    while (n != NULL) {
        lxb_dom_node_t *parent = dom_private_map_get(avl, avl_root, n->parent);
        lxb_dom_node_t *copy, *content, *ccontent;

        DCHECK(parent != NULL,
               "a private-tree copy reached a node whose parent it has not copied — the walk is pre-order over "
               "both trees, so a parent is always in the map before its children");
        copy = dom_private_copy_one(ctx, src->owner_document, n);
        dom_cow_insert_private(croot, parent, copy);
        dom_private_map_put(avl, &avl_root, n, copy);

        /* HTML §4.12.3's CLONING STEPS — a `<template>`'s markup is not under the element, it is in a separate
           fragment reached through it, and lexbor's clone gives the copy its own EMPTY one because the
           template interface's constructor makes it. The fragment goes in the map like every element, because
           it is the insertion parent its children are looked up by, and `croot` follows it because a content
           fragment is a root with no parent and therefore its own private tree in dom_cow's terms. */
        content  = node_template_content(n);
        ccontent = node_template_content(copy);
        if (content != NULL) {
            DCHECK(ccontent != NULL,
                   "a copied <template> has no content fragment — lexbor's clone built something other than a "
                   "template interface for a node whose tag is template");
            dom_private_map_put(avl, &avl_root, content, ccontent);
            if (content->first_child != NULL) { croot = ccontent; n = content->first_child; continue; }
        } else {
            DCHECK(ccontent == NULL,
                   "the copy of a non-template node has a template content fragment — the two sides of this "
                   "walk have stopped being the same kind of node");
        }
        if (n->first_child != NULL) { n = n->first_child; continue; }
        n = dom_private_walk_next(n, src, avl, avl_root, &croot);
    }

    for (i = 0; i < ncursors; i++) {
        lxb_dom_node_t *c = *cursors[i], *cc;

        if (c == NULL) continue;   /* a cursor the algorithm has not taken yet */
        cc = dom_private_map_get(avl, avl_root, c);
        DCHECK(cc != NULL,
               "a cursor into a flow-private tree names a node this copy does not know — every cursor a "
               "machine declares beside its tree must name a node OF that tree, and one that does not is left "
               "aimed at the arm this fork was taken from");
        *cursors[i] = cc;
    }
    lexbor_avl_destroy(avl, true);
    return top;
}

static void dom_private_tree_destroy(JSContext *ctx, void *root)
{
    dom_cow_destroy_private(ctx, root, /*with_children*/ true);
}

const JSStepTreeOps dom_cow_private_tree_ops = { dom_private_tree_clone, dom_private_tree_destroy };

/* DESTROY ONE NODE OF a private tree — see dom_cow.h. `lxb_dom_node_destroy` detaches before it frees, and it
   frees through the DOCUMENT's per-interface destructor, which for a `<template>` is what releases the template
   contents fragment along with the element AND, since core/dom/node_interface.c owns that dispatcher, the
   markup HTML §4.12.3 put inside it — which the `first_child == NULL` assertion below does not cover and
   never could, because a template's contents are not its children. */
void dom_cow_discard_private(lxb_dom_node_t *root, lxb_dom_node_t *node) {
    dom_private_check(root);
    DCHECK(node && node != root && dom_root_of(node) == root,
           "dom_cow_discard_private on a node outside the declared private tree — a node of the shared tree "
           "freed here is memory another flow's baseline still names");
    DCHECK(node->first_child == NULL,
           "dom_cow_discard_private on a node that still has children — they would be freed with it, and this "
           "operation exists because what was under it went somewhere else");
    lxb_dom_node_destroy(node);
}

/* A CHARACTER-DATA node's VALUE (§4.10 `data`). The third thing a flow can change about the tree, after an
   attribute and a node's presence: `text.data = x` mutates bytes the baseline owns, in place, on a node whose
   IDENTITY must survive the write — so it cannot be modelled as a remove+insert of a replacement node the way
   textContent legitimately is. Same shape as the attribute entry, over the node instead of the element. */
/* DOM §4.5 ADOPT's WRITE OF A NODE'S NODE DOCUMENT — the chokepoint entry, because a node document is shared
 * baseline state exactly like a parent link or an attribute value.
 *
 * IT WAS THE ONE PIECE OF TREE STATE WITH NO ENTRY KIND. adopt sets the node document of every
 * shadow-including inclusive descendant it walks, and the delta had kinds for tree structure, attributes and
 * character data and none for this — so a flow that adopted a subtree moved those nodes into another document
 * for EVERY flow. A sibling arm that never adopted read `node.ownerDocument` as the adopting arm's answer, and
 * anything it derived from that (the document a `createElement` on it builds into, §2.4.3 Document base URLs' base URL, which
 * registry §4.13 looks a definition up in) followed the wrong document. Nothing said so: node.c's write was a
 * plain field assignment that DCHECKed capture was OFF, which is honest about the gap and is exactly the shape
 * this entry closes.
 *
 * NOTHING IS DESTROYED ON REVERT. The entry points a node back at a document it did not create — kind 5 is the
 * one that owns a document, and this one only ever moves a pointer. */
void dom_cow_set_node_document(lxb_dom_node_t *node, lxb_dom_document_t *doc) {
    DCHECK(node != NULL, "dom_cow_set_node_document on no node");
    DCHECK(doc != NULL, "a node was given a NULL node document — §4.5 adopts INTO a document, and every node "
                        "has one");
    if (g_dom_capture && node->owner_document != doc) {
        DomUndo u; memset(&u, 0, sizeof u);
        dom_capture_begin();   /* the node's CURRENT document is read off the node and written down by the push */
        u.kind = 8; u.node = node; u.sh_old = u.sh_cur = JS_UNDEFINED;
        u.doc_old = node->owner_document;
        u.doc_cur = doc;
        dom_undo_push(u);
        dom_capture_end();
    }
    node->owner_document = doc;
}

void dom_cow_set_text(lxb_dom_node_t *node, const char *val, size_t val_len) {
    lxb_dom_character_data_t *cd;
    if (!node) return;
    DCHECK(node->type == LXB_DOM_NODE_TYPE_TEXT || node->type == LXB_DOM_NODE_TYPE_COMMENT,
           "dom_cow_set_text on a node that holds no character data");
    cd = lxb_dom_interface_character_data(node);
    /* BEFORE the write and before the capture, because §4.10's replace data queues its record with the node's
       CURRENT data and there is no answer to that once the bytes are gone. */
    DCHECK(!g_cdata_hook || g_cow_ctx, TREE_HOOK_NO_CTX);
    if (g_cdata_hook)
        g_cdata_hook(g_cow_ctx, node, (const char *)cd->data.data, cd->data.length);
    if (g_dom_capture) {
        DomUndo u; memset(&u, 0, sizeof u);
        /* `cd->data` is the node's own storage: the LENGTH is read before the allocation and the BYTES are
           copied out after it, so the sale stands between the two (see the scope's comment). */
        dom_capture_begin();
        u.kind = 3; u.node = node; u.sh_old = u.sh_cur = JS_UNDEFINED;
        u.had = 1; u.old_len = cd->data.length;
        u.old = reclaim_malloc(u.old_len ? u.old_len : 1);
        CHECK(u.old != NULL, "dom-cow-oom: the character-data baseline snapshot failed — unapply would lose the "
                             "text the baseline had");
        memcpy(u.old, cd->data.data, u.old_len);
        dom_undo_push(u);
        dom_capture_end();
    }
    lxb_dom_character_data_replace(cd, (const lxb_char_t *)val, val_len, 0, cd->data.length);
}

/* HTML §13.2.6.1 "Creating and inserting nodes"'s "insert a character" step 3 — see dom_cow.h for why this is
   its own entry point and why unapplying it is a value restore rather than a node removal. */
void dom_cow_append_text_data(lxb_dom_node_t *node, const char *data, size_t len) {
    lxb_dom_character_data_t *cd;
    lexbor_mraw_t *text;

    DCHECK(node != NULL, "§13.2.6.1's insert-a-character was asked to append to no node — step 3 reaches here "
                         "only having FOUND a Text node immediately before the insertion location");
    DCHECK(node->type == LXB_DOM_NODE_TYPE_TEXT,
           "§13.2.6.1's insert-a-character merged into a node that is not a Text node — step 3's test is "
           "`there is a Text node immediately before insertionLocation`, so anything else means the parser "
           "found the previous sibling and never asked what it was");
    DCHECK(node->owner_document != NULL,
           "a character merge reached a Text node with no node document — its data is allocated out of that "
           "document's text arena, so there is nowhere for the appended bytes to live");
    cd = lxb_dom_interface_character_data(node);
    /* THE CAPTURE IS UNCONDITIONAL UNDER THE GATE, exactly like dom_cow_set_text's, and that is the SOUND
       direction rather than the lazy one: over-capturing a Text node the running flow itself built costs one
       entry and reverts to the same bytes, while under-capturing a node the baseline owns leaves a page's text
       written by one flow and visible to every other. §13.2.6.1's own example — "the parser appends to the
       Text node created by the script" — is the second case, and no property of the node distinguishes them.
       `cd->data.length` is read BEFORE the allocation and `cd->data.data` copied out AFTER it, so the sale
       that allocation can trigger stands between the read and the use (see the capture scope's comment). */
    if (g_dom_capture) {
        DomUndo u; memset(&u, 0, sizeof u);
        dom_capture_begin();
        u.kind = 3; u.node = node; u.sh_old = u.sh_cur = JS_UNDEFINED;
        u.had = 1; u.old_len = cd->data.length;
        u.old = reclaim_malloc(u.old_len ? u.old_len : 1);
        CHECK(u.old != NULL, "dom-cow-oom: the character-merge baseline snapshot failed — unapply would leave "
                             "the parser's appended text in a Text node the baseline owns");
        /* A Text node whose storage was never allocated has a NULL `data` and a zero length, and memcpy from a
           null source is undefined however small the count — the allocation above is still made, so the entry
           holds an empty baseline rather than a missing one and the unapply arm needs no case for it. */
        if (u.old_len) memcpy(u.old, cd->data.data, u.old_len);
        dom_undo_push(u);
        dom_capture_end();
    }
    /* The append itself, out of the node's OWN document's text arena — the one §13.2.6.1 used and the one
       lxb_dom_character_data_replace reaches for on the restore, so a captured node and its undo allocate from
       one place. The empty arm is a Text node whose storage was never allocated: initialise at `len` and the
       append below fills it. */
    text = node->owner_document->text;
    if (cd->data.data == NULL) {
        CHECK(lexbor_str_init(&cd->data, text, len) != NULL,
              "dom-cow-oom: §13.2.6.1's character merge could not initialise the Text node's storage");
    }
    CHECK(lexbor_str_append(&cd->data, text, (const lxb_char_t *)data, len) != NULL,
          "dom-cow-oom: §13.2.6.1's character merge could not append to the Text node's data");
}

/* ---- HTML §13.2.6 "Tree construction"'s DOM writes, as this file's own — see dom_cow.h ---------------------
 *
 * ONE RECORD PER PARSE IN FLIGHT, keyed on the `lxb_html_tree_t` every one of §13.2.6's writes carries. That is
 * the whole of the context these members have and the whole of what they need: the caller that OPENED the parse
 * knows whose tree it is, and nothing downstream of it can recover that.
 * THE SCAN IS OVER PARSES, NEVER OVER THE DELTA — a fragment parse yields between bytes, so a flow parked
 * mid-`innerHTML =` holds one entry, and the walk below is bounded by how many of those exist rather than by
 * anything the page's markup decides. */
typedef struct { lxb_html_tree_t *tree; lxb_dom_node_t *root; DomParseRootKind kind; } DomParseDecl;
static DomParseDecl *g_dom_parse = NULL;
static int g_dom_parse_n = 0, g_dom_parse_cap = 0;

static DomParseDecl *cow_tc_decl_of(lxb_html_tree_t *tree)
{
    int i;
    for (i = 0; i < g_dom_parse_n; i++)
        if (g_dom_parse[i].tree == tree)
            return &g_dom_parse[i];
    return NULL;
}

void dom_cow_parse_declare(lxb_html_tree_t *tree, lxb_dom_node_t *root, DomParseRootKind kind)
{
    DCHECK(tree != NULL,
           "a §13.2.6 parse was declared with no tree builder — the tree is the KEY every one of tree "
           "construction's writes arrives with, so a declaration without one names no parse at all");
    /* THE ROOT OF A PARSE IS ITS DOCUMENT NODE, for both kinds. §13.4 "Parsing HTML fragments" step 3 makes a
       Document for the fragment and hangs the `html` element it creates under it; a document parse writes the
       Document it was opened on. Asserting it is what makes the declaration's own privacy check a ONE-TIME
       cost: a Document node is never inserted anywhere and never removed from anywhere, so the root cannot
       drift out of the state dom_private_check just verified, and re-asking per write would buy nothing. */
    DCHECK(root != NULL && root->type == LXB_DOM_NODE_TYPE_DOCUMENT,
           "a §13.2.6 parse declared a root that is not a Document node — both parse kinds build a Document's "
           "tree (§13.4 step 3 makes one for a fragment), and a root that is an ELEMENT is one that can be "
           "moved and removed, which is exactly what this declaration may not have to re-check");
    DCHECK(cow_tc_decl_of(tree) == NULL,
           "a §13.2.6 parse was declared on a tree builder that already has a live declaration — either a "
           "parse was opened twice, or an earlier parse's release never ran and this tree's address now names "
           "somebody else's document");
    if (kind == DOM_PARSE_ROOT_PRIVATE)
        dom_private_check(root);
    if (g_dom_parse_n >= g_dom_parse_cap) {
        int nc = g_dom_parse_cap ? g_dom_parse_cap * 2 : 8;
        DomParseDecl *n;
        /* The read this stands on is `root`, held across an allocation that can SELL A FLOW — the same
           statement every producer in this file makes, and made here even though no delta entry is written,
           because the scope is defined over the READ (see the capture scope's own comment). */
        dom_capture_begin();
        n = reclaim_realloc(g_dom_parse, (size_t)nc * sizeof(DomParseDecl));
        CHECK(n, "dom-cow-oom: the parse-declaration table could not grow — a §13.2.6 write with no declaration "
                 "cannot tell the page's own tree from a parse's private one, and guessing either way is a "
                 "silently wrong document");
        g_dom_parse = n; g_dom_parse_cap = nc;
        dom_capture_end();
    }
    g_dom_parse[g_dom_parse_n].tree = tree;
    g_dom_parse[g_dom_parse_n].root = root;
    g_dom_parse[g_dom_parse_n].kind = kind;
    g_dom_parse_n++;
}

void dom_cow_parse_release(lxb_html_tree_t *tree)
{
    DomParseDecl *d = cow_tc_decl_of(tree);

    DCHECK(d != NULL,
           "a §13.2.6 parse was released that was never declared — the declaration is made where the parse is "
           "opened and released where it ends, so an unmatched release is a second close of one parse or a "
           "close of a parse some other entry opened");
    /* `d->root` is deliberately not touched: §13.4's temporary document is destroyed by
       `lxb_html_parse_fragment_chunk_end`, so by the time a fragment parse says it is finished the root it
       declared is already freed. Order-preserving compaction, so the table reads in the order parses opened. */
    memmove(d, d + 1, (size_t)(&g_dom_parse[g_dom_parse_n] - (d + 1)) * sizeof(DomParseDecl));
    g_dom_parse_n--;
}

/* WHICH PARSE THIS WRITE BELONGS TO — and a write whose parse nobody declared CRASHES rather than picking a
   side. Both sides are wrong in different ways: called private, a shared write builds the page's document with
   no entry and every sibling flow reads a parse it never ran; called shared, a fragment parse puts its whole
   internal structure in the delta. A crash here names the parse entry that has to declare. */
static DomParseDecl *cow_tc_decl(lxb_html_tree_t *tree)
{
    DomParseDecl *d;

    DCHECK(tree != NULL, "a §13.2.6 tree-construction write arrived with no tree builder — the tree is what "
                         "names the parse, and without it this file cannot tell whose tree is being written");
    d = cow_tc_decl_of(tree);
    DCHECK(d != NULL,
           "HTML §13.2.6 tree construction wrote through a parse that never declared whose tree it builds. "
           "Whoever opens the parse states it — dom_cow_parse_declare with the Document node it writes into "
           "and DOM_PARSE_ROOT_PRIVATE for a tree the same operation created (§13.4's temporary document, a "
           "scratch Document) or DOM_PARSE_ROOT_SHARED for the ACTIVE document — and releases it where the "
           "parse ends. There is no default: guessing private writes the page's tree with no delta entry");
    return d;
}

/* §13.2.6 MADE A NODE — THE OTHER HALF OF A SHARED PARSE'S ISOLATION, and the half a mutation interface cannot
 * express. The three writes below say what to PUT BACK when a flow's delta is unapplied; this says what to
 * DESTROY when that delta is discarded, and without it a `document.write()` into the page's own tree left every
 * element, comment, Text node and DocumentType it built owned by nobody: detached by the undo, freed by no one,
 * and reachable from the wrapper identity map for the rest of the run. That is the leak dom_release_created's
 * own comment traces to a five-second calloc inside one `createElement`.
 *
 * IT IS NOTED AT THE CREATION AND NOT AT THE INSERT, which is the whole reason the hook had to exist rather
 * than being folded into cow_tc_insert_child. §13.2.6.4.7 'The "in body" insertion mode's adoption agency
 * removes a node and inserts it again — several times, for one node — so an ownership record taken at the
 * insert is taken more than once, and two records that each `destroy_deep` it is a double free at discard. A
 * creation happens exactly once.
 *
 * ONLY A SHARED PARSE. §13.4's fragment parse builds a tree no other flow can reach, and §State-isolation's
 * invariant is that flow-private state is never captured — its ownership is answered at dom_cow_take_private,
 * where the node LEAVES the private tree for the real one, and a node that never leaves would carry a creation
 * entry the discard then asserts against.
 *
 * A SHARED PARSE'S SUBTREE IS N ENTRIES AND NOT ONE, unlike the created-subtree case dom_release_created
 * describes: every one of this parse's inserts goes through the capturing chokepoint below, so the unapply
 * detaches every node from every other before any of them is released, and each is childless by the time its
 * own entry destroys it. The interior of a subtree built with dom_cow_insert_private is captured by nothing,
 * which is why that one has a single entry and this one does not.
 * LEXBOR'S OWN FAILURE PATHS DESTROY A NODE IT HAS ALREADY MADE — `create_element_for_token` on a failed
 * attribute append, `insert_foreign_element` on a failed open-elements push, `create_document_type_from_token`
 * on a failed doctype parse — which would leave an entry naming freed memory. All three are ALLOCATION
 * failures and nothing else, and this engine does not continue past one: the status reaches the parse entry's
 * own CHECK, which aborts, so no delta holding such an entry can ever be discarded. */
static void cow_tc_create(lxb_html_tree_t *tree, lxb_dom_node_t *node)
{
    DomParseDecl *d = cow_tc_decl(tree);

    DCHECK(node != NULL, "§13.2.6 announced a node it did not make — the factory calls this only on a node it "
                         "built, so a NULL here is a caller that is not that factory");
    DCHECK(node->parent == NULL,
           "§13.2.6 announced a CREATION for a node that is already in a tree — the hook is called by the node "
           "factory before the node is anywhere, so a parented node here is an ownership record about to be "
           "taken for the second time, which is a double free at discard");
    if (d->kind == DOM_PARSE_ROOT_PRIVATE)
        return;
    dom_cow_note_created(node);
}

static void cow_tc_insert_child(lxb_html_tree_t *tree, lxb_dom_node_t *to, lxb_dom_node_t *node)
{
    DomParseDecl *d = cow_tc_decl(tree);

    DCHECK(to != NULL && node != NULL, "§13.2.6 inserted nothing, or inserted into nothing");
    /* EVERY §13.2.6 INSERT IS OF A PARENTLESS NODE, and it is asserted rather than assumed: the insertion
       modes insert elements, comments, Text nodes and the DocumentType they created a statement earlier, and
       §13.2.6.4.7's adoption agency removes a node in its own step before every append it performs. A node
       that still has a parent here is one being MOVED with its removal seen by nothing. */
    DCHECK(node->parent == NULL,
           "§13.2.6 inserted a node that is still in a tree — that is a move, and the removal half of it "
           "reached neither the removing steps nor any delta");
    /* THE PARSE'S OWN CONTAINMENT QUESTION, asked over dom_parse_root_of: an insertion point inside a
       `<template>`'s content fragment is still this parse's tree, and the private-tree family's own walk —
       which stops at the fragment, because a clone declares it as a root in its own right — would call it
       another tree. dom_private_check ran ONCE at the declaration and is not re-asked: a Document node is
       never inserted anywhere and never removed from anywhere, so nothing about the root can have changed. */
    DCHECK(dom_parse_root_of(to) == d->root,
           "§13.2.6 inserted into a tree its parse never declared — the insertion point climbed out to a root "
           "that is not the Document this parse was opened on, so either the parse walked into somebody else's "
           "document or a `<template>`'s content fragment lost the host that reaches it");
    g_dom_version++;
    if (d->kind == DOM_PARSE_ROOT_PRIVATE) {
        lxb_dom_node_insert_child(to, node);
        return;
    }
    dom_insert_capture(node);
    lxb_dom_node_insert_child(to, node);
}

static void cow_tc_insert_before(lxb_html_tree_t *tree, lxb_dom_node_t *to, lxb_dom_node_t *node)
{
    DomParseDecl *d = cow_tc_decl(tree);

    DCHECK(to != NULL && node != NULL, "§13.2.6 inserted nothing, or inserted before nothing");
    DCHECK(node->parent == NULL,
           "§13.2.6 foster-parented a node that is still in a tree — that is a move, and the removal half of "
           "it reached neither the removing steps nor any delta");
    /* §13.2.6.1's foster-parented position is "inside last table's parent node, immediately before last
       table", so the reference node has a parent by construction; without one lexbor links the child to a null
       parent and the fostered content is reachable from nothing. */
    DCHECK(to->parent != NULL,
           "§13.2.6.1's appropriate place for inserting a node gave a BEFORE position at a node with no "
           "parent — there is no position before a tree's root");
    DCHECK(dom_parse_root_of(to) == d->root,
           "§13.2.6.1 foster-parented into a tree its parse never declared — the reference node climbed out to "
           "a root that is not the Document this parse was opened on");
    g_dom_version++;
    if (d->kind == DOM_PARSE_ROOT_PRIVATE) {
        lxb_dom_node_insert_before(to, node);
        return;
    }
    dom_insert_capture(node);
    lxb_dom_node_insert_before(to, node);
}

static void cow_tc_remove(lxb_html_tree_t *tree, lxb_dom_node_t *node)
{
    DomParseDecl *d = cow_tc_decl(tree);

    DCHECK(node != NULL, "§13.2.6 removed nothing");
    DCHECK(node->parent != NULL,
           "§13.2.6.4.7's adoption agency removed a node that is in no tree — its own steps guard every "
           "remove with `if lastNode's parent is non-null`, so reaching here past that guard means the "
           "algorithm lost track of where the node was");
    DCHECK(dom_parse_root_of(node) == d->root,
           "§13.2.6.4.7's adoption agency removed a node from a tree its parse never declared — a detach of "
           "state some other flow's baseline holds must be captured, or that flow's unapply re-inserts memory "
           "this parse is about to reuse");
    g_dom_version++;
    if (d->kind == DOM_PARSE_ROOT_PRIVATE) {
        /* A BARE UNLINK, and no creation record either way: the node stays this parse's own and the algorithm
           puts it back a step later. §13.2.6.4.7 says "If lastNode's parent is non-null, then remove lastNode"
           and "Insert lastNode at the appropriate place for inserting a node" as two separate steps over the
           tree it is repairing, so this is neither dom_cow_take_private (which declares the node has LEFT for
           the real tree) nor dom_cow_destroy_private (which frees it). */
        lxb_dom_node_remove(node);
        return;
    }
    dom_remove_capture(node);
    lxb_dom_node_remove(node);
}

static lxb_status_t cow_tc_append_data(lxb_html_tree_t *tree, lxb_dom_character_data_t *chrs,
                                       const lxb_char_t *data, size_t len)
{
    lxb_dom_node_t *node = lxb_dom_interface_node(chrs);
    DomParseDecl *d = cow_tc_decl(tree);

    /* NO KIND SPLIT, and that is the difference this member exists to state: the merge captures whichever tree
       it lands in, so a live parse on the page's own document makes nothing here unsound. Over-capturing a
       Text node the parse itself built costs one entry that reverts to the same bytes; under-capturing one the
       baseline owns leaves a page's text written by one flow and visible to every other.
       WHAT THE DECLARATION IS STILL FOR HERE IS WHERE THE MERGE LANDED. §13.2.6.1's "insert a character" step
       3 appends to "a Text node immediately before insertionLocation", which is a node of the tree this parse
       is building — so a target outside the declared root is a parse writing a document it never named. */
    DCHECK(dom_parse_root_of(node) == d->root,
           "§13.2.6.1's character merge appended to a Text node outside the tree its parse declared — step 3's "
           "target is the node immediately before the insertion location, so a node in another tree means the "
           "insertion location itself belongs to a document this parse never named");
    dom_cow_append_text_data(node, (const char *)data, len);
    return LXB_STATUS_OK;
}

static const lxb_html_tree_dom_cb_t g_cow_tc_ops = {
    cow_tc_create,
    cow_tc_insert_child,
    cow_tc_insert_before,
    cow_tc_remove,
    cow_tc_append_data
};

void dom_cow_install_tree_construction(void)
{
    lxb_html_tree_dom_set(&g_cow_tc_ops);
}

bool dom_cow_owns_tree_construction(void)
{
    return lxb_html_tree_dom() == &g_cow_tc_ops;
}

void dom_cow_append_child(lxb_dom_node_t *parent, lxb_dom_node_t *child) {
    g_dom_version++;
    dom_insert_capture(child);   /* record the insertion FIRST so it reverts per-flow (detached on unapply) */
    lxb_dom_node_insert_child(parent, child);
    DCHECK(!g_tree_hook || g_cow_ctx, TREE_HOOK_NO_CTX);
    /* THE SUBTREE THE HOOK IS TOLD ABOUT IS THE ONE THIS CALL PLACED, ASSERTED AT THE PLACEMENT. DOM §4.2.3
       "Mutation algorithms"' insert step 7.7 walks "each shadow-including inclusive descendant of node", so
       every registrant's walk is bounded by the node named here and by nothing else — the bound is only as
       good as this argument. Asked at the chokepoint because this is the one place that knows both what was
       asked for and where Lexbor put it; a registrant can only re-read what it was handed. */
    DCHECK(child->parent == parent && child->next == NULL,
           "§4.2.3's append chokepoint is about to name a node that is not where it just put it — the tree "
           "hooks run the insertion steps over this node's subtree, so a node reported before it is the "
           "parent's last child would run them over a tree nobody built");
    if (g_tree_hook) g_tree_hook(g_cow_ctx, child, child->parent, 1);   /* AFTER: connectedness is the new tree's */
}
/* §4.2.3 "insert before": the same capture, at a POSITION. The insert entry remembers where it landed at
   unapply time rather than at capture time, so this differs from append only in the Lexbor call — which is
   exactly why insertBefore must come through here and not reach lxb_dom_node_insert_before directly. */
void dom_cow_insert_before(lxb_dom_node_t *ref, lxb_dom_node_t *child) {
    g_dom_version++;
    dom_insert_capture(child);
    lxb_dom_node_insert_before(ref, child);
    DCHECK(!g_tree_hook || g_cow_ctx, TREE_HOOK_NO_CTX);
    /* THE SAME ASSERTION AS THE APPEND ABOVE, AND THIS IS THE SIDE THAT MATTERS. `child->next` is `ref` rather
       than NULL here, which is exactly the shape a registrant's walk must not read as licence to keep going:
       a subtree root WITH a next sibling is what an insert-before produces and an append never does, so a
       walker whose bound is tested only on the climb out escapes on this path and on no other. Naming the node
       and its landing place here is what lets that walk be judged against something. */
    DCHECK(child->parent == ref->parent && child->next == ref,
           "§4.2.3's insert-before chokepoint is about to name a node that is not where it just put it — the "
           "tree hooks run the insertion steps over this node's subtree, so a node reported before it sits "
           "immediately before the reference child would run them over a tree nobody built");
    if (g_tree_hook) g_tree_hook(g_cow_ctx, child, child->parent, 1);
}

/* DOM §4.2.3 "MOVE" STEPS 13 AND 19-20 — the same two captures the remove and insert chokepoints push, and NO
   TREE HOOK. See dom_cow.h for why the absence of the hook is the whole operation rather than an omission.
   THE PAIR IS ASSERTED, in dev, through one file-static: a `_move_out` whose `_move_in` never came is a node
   taken out of the tree with no removing steps run for it, which is a leak of live tree from every walk in the
   engine and shows up nowhere. Nothing between the two can run the page's code (§4.2.3's steps 14-18 are slot
   assignment and live-range arithmetic, neither of which calls out), so ONE pointer is the whole state the
   invariant needs — and it is compiled out with the check it exists for. */
#if APICLIENT_DEV
static lxb_dom_node_t *g_move_in_flight;
#endif

void dom_cow_move_out(lxb_dom_node_t *node) {
    DCHECK(node != NULL, "§4.2.3's move was asked to take nothing out of a tree — `node` is non-nullable in the "
                         "algorithm's own signature");
    DCHECK(node->parent != NULL,
           "§4.2.3 move step 8 asserts oldParent is non-null and it is null — step 1's shadow-including-root "
           "test is what guarantees it, so reaching here with a parentless node means the member skipped it");
#if APICLIENT_DEV
    DCHECK(g_move_in_flight == NULL,
           "§4.2.3's move took a second node out of the tree while the first was still detached — the pair is "
           "one uninterrupted operation, so a nested move is a caller running two algorithms at once");
    g_move_in_flight = node;
#endif
    g_dom_version++;
    dom_remove_capture(node);
    lxb_dom_node_remove(node);
}

void dom_cow_move_in(lxb_dom_node_t *parent, lxb_dom_node_t *node, lxb_dom_node_t *ref) {
    DCHECK(parent != NULL && node != NULL, "§4.2.3's move was asked to put nothing anywhere");
    DCHECK(node->parent == NULL,
           "§4.2.3 move step 19/20 ran on a node that is still in a tree — step 13's detach is what makes the "
           "slot steps between them recompute anything, so a move that skipped it inserts a node with two "
           "parents and a sibling chain that loops");
    DCHECK(ref == NULL || ref->parent == parent,
           "§4.2.3's move was given a reference child of another parent — step 3's NotFoundError is what keeps "
           "that out, and reaching here past it inserts into a tree the member never named");
#if APICLIENT_DEV
    DCHECK(g_move_in_flight == node,
           "§4.2.3's move put back a node its own step 13 never took out — the two halves are one operation "
           "over one node, and a mismatch is a removal whose removing steps nobody ran");
    g_move_in_flight = NULL;
#endif
    g_dom_version++;
    dom_insert_capture(node);
    if (ref) lxb_dom_node_insert_before(ref, node);
    else     lxb_dom_node_insert_child(parent, node);
}
/* dom_revert — the "DISCARD the running flow's writes -> baseline" twin of dom_unapply — is DELETED, and this
 * note is here because the deletion is the point rather than a tidy-up. It was the ONE caller's (flow_finish's)
 * private discard: the same per-entry restore dom_unapply already does, minus the stash into `cur`, plus
 * dom_release_created_all over the head and a blanket unapply of the whole installed chain. So a finishing flow
 * and an evicted one tore their document down through two different code paths that had to agree — and they had
 * already stopped agreeing, because the blanket unapply reverted the document all the way to the baseline even
 * when a SIBLING still held the chain, which the next switch-in then replayed in full.
 * A finish is now a switch-out followed by a release (engine.c's flow_finish), so the head goes through
 * dom_unapply + dom_buf_take and its creations die in dom_buf_free, and the chain through dom_base_release,
 * which walks the document down to the deepest segment that actually survives. ONE path, exercised by every
 * flow that finishes rather than only by an eviction no test reaches. */
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
            if (c) { u->cur = reclaim_malloc(vl ? vl : 1); CHECK(u->cur, "dom-cow-oom: parked flow attr snapshot malloc failed — apply would lose the flow's DOM write"); memcpy(u->cur, c, vl); u->cur_len = vl; }
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
        if (c) { u->cur = reclaim_malloc(vl ? vl : 1);
                 CHECK(u->cur, "dom-cow-oom: parked detached-attribute snapshot malloc failed");
                 memcpy(u->cur, c, vl); u->cur_len = vl; }
        if (g_cow_ctx) JS_FreeValue(g_cow_ctx, u->sh_cur);
        u->sh_cur = shadow_snapshot(u->sh_owner, u->slot, u->ns, u->name, &u->sh_cur_had);
        dom_attr_set_value(a, (const char *)(u->old ? u->old : (lxb_char_t *)""), u->old_len);   /* baseline back */
        shadow_restore(u->sh_owner, u->slot, u->ns, u->name, u->sh_old, u->sh_had);
    } else if (u->kind == 3) {
        lxb_dom_character_data_t *cd = lxb_dom_interface_character_data(u->node);
        free(u->cur); u->cur_len = cd->data.length; u->cur_had = 1;       /* stash the flow's text */
        u->cur = reclaim_malloc(u->cur_len ? u->cur_len : 1);
        CHECK(u->cur != NULL, "dom-cow-oom: the parked character-data snapshot failed — apply would lose the "
                              "flow's text write");
        memcpy(u->cur, cd->data.data, u->cur_len);
        lxb_dom_character_data_replace(cd, u->old, u->old_len, 0, cd->data.length);   /* baseline back */
    } else if (u->kind == 8) {
        /* Stash the flow's node document, restore the baseline's — the same shape every other kind has, and the
           reason the entry holds two: a parked flow resumes into the document IT adopted the node into. */
        u->doc_cur = u->node->owner_document;
        u->node->owner_document = u->doc_old;
        /* AND THE NAMES, because a name id is per-document state that the pointer alone does not carry. A
           flow's `d = createHTMLDocument(); d.body.appendChild(el)` interns `el`'s namespace, prefix, local
           name and every attribute's four into `d`'s hashes; parking restores `el` to the baseline document
           and kind 5 then DESTROYS `d`, hashes and all. Without this the baseline's own element would name
           freed entries — the delta's revert would be what created the dangling id. Re-interning the bytes
           dedupes to the entries the baseline already held, so the restore is byte-identical. */
        dom_import_node_names(u->doc_old, u->doc_cur, u->node);
        /* THE BYTES NEED NO MIRROR, and this is the assertion that states it rather than the silence that
           would look identical. The names had to move because a name id is an address into ONE document's
           hashes; a node's own storage does not, because every document in this agent allocates out of the
           SAME arenas (core/dom/node_heap.h) — so the kind-5 destroy of `d` cannot free the bytes the baseline
           element sits in, whichever document the pointer above names. */
        DCHECK(dom_storage_owned_by(u->doc_old, u->node),
               "the delta's revert put a node back in a document whose arenas are not the ones its bytes are "
               "in — kind 5 then destroys the document the flow created, and this node would be freed with it");
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
    } else if (u->kind == 8) {
        u->node->owner_document = u->doc_cur;
        dom_import_node_names(u->doc_cur, u->doc_old, u->node);   /* the mirror — see the unapply arm */
        DCHECK(dom_storage_owned_by(u->doc_cur, u->node),
               "the delta's apply moved a node into a document whose arenas are not the ones its bytes are in");
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

    /* THE DOCUMENT MAY NOT MOVE UNDER A CAPTURE'S READ, AND THIS IS THE ONE PLACE IT COULD.
       A producer reads an element, an attribute node or a byte pointer off the live tree and writes it into an
       entry, and the ONE thing that can run between those two is a SALE: dom_undo_push and the copies around
       it ask reclaim_malloc/realloc, which pages the frontier's tail out, and dom_base_release walks the chain
       down to the deepest segment that survives it. Reaching here from inside one is impossible — the segment
       the document is SHOWING is the running flow's own base, every flow holds one counted reference on its
       base and every segment one on its base, so a segment two chains reach has a refcount of at least 2, and
       a release's dying prefix stops at the first segment whose refcount is not 1. That is a proof about
       refcounts, which is exactly the kind of statement that stops being true without anyone noticing, so it
       is held here rather than restated at each read. If this fires, the entry that producer is about to write
       names a node from a timeline the document no longer shows — and, because a dying segment DESTROYS the
       nodes its entries created, quite possibly one that no longer exists, which the next swap re-attaches. */
    DCHECK(g_dom_capturing == 0,
           "the applied DOM chain moved while this file was standing on a read of the document — the entry "
           "about to be written names tree from another flow's timeline, or tree the dying segment just "
           "destroyed, and every context switch from then on replays it");
    for (s = g_dom_installed; s != common; s = s->base)      /* head-first, as unapply always is */
        for (int i = s->n - 1; i >= 0; i--) dom_unapply_entry(&s->e[i]);
    dom_apply_seg_until(want, common);
    g_dom_installed = want;
}

/* drop a chain reference: refcount--, free the segment's entries (parked: old/cur held) when it hits 0, recurse. */
/* WHAT THE DOCUMENT'S CHAIN IS HOLDING — the same pair the heap chain reports, counted at the same two points
   (the fork that freezes a segment and this unref), and reported beside it because a per-flow delta that is
   never released looks identical in the two halves and has to be told apart by which number climbs. */
static long g_dom_seg_live, g_dom_seg_entries_live;
void dom_cow_chain_stats(long *segs, long *entries) {
    if (segs) *segs = g_dom_seg_live;
    if (entries) *entries = g_dom_seg_entries_live;
}
long dom_cow_chain_bytes(void) {
    return g_dom_seg_live * (long)sizeof(DomSeg) + g_dom_seg_entries_live * (long)sizeof(DomUndo);
}
long dom_cow_head_bytes(int cap) { return (long)cap * (long)sizeof(DomUndo); }

static void dom_seg_unref(DomSeg *s) {
    while (s && --s->refcount <= 0) {
        DomSeg *base = s->base;
        /* THE DOCUMENT MAY NOT BE SHOWING WHAT IS ABOUT TO BE FREED — the heap half asserts the same thing at
           the same point and for the same reason. `g_dom_installed` holds no reference, so a refcount reaching
           zero says nothing about whether the segment is applied, and here the consequence is worse than a
           dangling pointer: dom_release_created_all destroys the nodes a segment's entries created, and a
           segment still applied has those nodes IN THE TREE. dom_base_release is what brings the document down
           to the deepest survivor first. */
        DCHECK(s != g_dom_installed,
               "a frozen DOM segment was freed while the document was still SHOWING it — the nodes it created "
               "are live tree, its writes stay standing in the baseline, and the installed chain points into "
               "freed memory");
        g_dom_seg_live--; g_dom_seg_entries_live -= s->n;
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
    /* THE HEAD IS THE OTHER HALF OF dom_install_chain'S CHECK, and it needs its own because the head is taken
       down WITHOUT going through the chain walk: a producer standing on a read while its own flow's writes are
       reverted would record the baseline it is being restored to as the value it saw. Unreachable — a producer
       is a straight line through this file with no scheduler entry in it — which is why it is asserted rather
       than arranged. */
    DCHECK(g_dom_capturing == 0,
           "the running flow's DOM head was unapplied while this file was standing on a read of the document — "
           "the entry about to be written records the baseline the unapply just restored as the value the flow "
           "had, and the apply that resumes this flow writes it back as its own");
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
   that, then RE-APPLY so the running flow continues byte-identically.
   CAPTURE IS NO LONGER SUSPENDED ACROSS THE ROUND TRIP, and the deletion is the point. It was `g_dom_capture =
   0` restored at the end, justified as "defensive symmetry with the heap fork ... so no re-capture, but the
   guard documents+enforces the invariant" — a guard for a hazard the same sentence says does not exist, and
   one that ENFORCED nothing: had the round trip ever reached a producer, clearing the flag would have made the
   entry silently vanish. The invariant is now the assert at dom_undo_push, which crashes there instead.
   Returns the shared base. */
void *dom_cow_fork(void) {
    /* ASKED BEFORE THE UNAPPLY, because this allocation can SELL A FLOW (solver/reclaim.h) and a sale walks
       the frozen document chain. Between the unapply below and the re-apply at the end, the running flow's
       head is half-taken-down and the document shows a tree no entry describes; nothing in this file is
       written to meet a re-entry there. The seg's three fields are all readable now, so there is nothing the
       later position bought. */
    DomSeg *seg = reclaim_malloc(sizeof(DomSeg));
    CHECK(seg, "dom-cow-oom: fork segment alloc failed — a shared DOM delta would be corrupted");
    for (int i = g_dom_undo_n - 1; i >= 0; i--) dom_unapply_entry(&g_dom_undo[i]);
    seg->e = g_dom_undo; seg->n = g_dom_undo_n; seg->base = g_dom_base; seg->refcount = 2;   /* running flow + sibling */
    g_dom_seg_live++; g_dom_seg_entries_live += seg->n;
    g_dom_undo = NULL; g_dom_undo_n = 0; g_dom_undo_cap = 0;   /* fresh empty head for the running flow */
    g_dom_base = seg;
    g_dom_installed = seg;   /* the frozen head belongs to the applied chain now, not to anyone's head */
    for (int i = 0; i < seg->n; i++) dom_apply_entry(&seg->e[i]);   /* re-apply head -> running flow continues */
    return seg;
}
/* Take / install the shared BASE chain alongside the head (a flow's full DOM delta is head + base chain). */
void *dom_base_take(void) { void *b = g_dom_base; g_dom_base = NULL; return b; }
void dom_base_load(void *base) { g_dom_base = (DomSeg *)base; }
/* RELEASE one flow's reference on the document's frozen chain — the DOM twin of cow_delta_release, and the same
   walk for the same reason. The document comes back down to the deepest segment that SURVIVES the release and
   no further: a segment a sibling still holds stays applied (a switch to that sibling then costs nothing), and a
   dying one is unapplied BEFORE dom_seg_unref destroys the nodes its entries created — which, unlike the heap
   half, is not merely a dangling pointer but live tree being freed out of the document. */
void dom_base_release(void *base) {
    DomSeg *surv, *s;

    if (!base) return;
    for (surv = (DomSeg *)base; surv && surv->refcount == 1; surv = surv->base) ;
    for (s = (DomSeg *)base; s != surv; s = s->base)
        if (s == g_dom_installed) { dom_install_chain(surv); break; }
    dom_seg_unref((DomSeg *)base);
}
/* dom_base_ref — "add ONE ref (each orphan forks the document flow's shared DOM delta)" — is DELETED, and it
 * is deleted BECAUSE of the proof above rather than because it was unused. No orphan does that: an orphan flow
 * is seeded with no base and takes one only by forking, which is dom_cow_fork, so the sentence was accurate
 * about the design and wrong about this tree — and nothing in the engine has ever called it. What it left
 * behind was a hole in the arithmetic dom_install_chain now asserts on: with it gone, a DomSeg's refcount is
 * exactly the number of flows whose `dom_base` names it plus the number of segments whose `base` names it, and
 * both of those are written in this file. An exported function that can raise a refcount from outside makes
 * that equation something no reader can check. */
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
