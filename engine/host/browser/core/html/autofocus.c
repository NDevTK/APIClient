/* HTML §6.6.7 — THE AUTOFOCUS ATTRIBUTE: a Document's autofocus candidates, its autofocus processed flag, the
 * insertion steps that fill the list, and FLUSH AUTOFOCUS CANDIDATES, which HTML §8.1.7.3 update the rendering
 * step 7 runs.
 *
 * WHAT WAS HERE BEFORE was rendering.c's assertion that step 7 could not be written yet, standing at the step
 * and naming this file's contents as the work: "update the rendering step 7 flushes doc's AUTOFOCUS CANDIDATES
 * (HTML §6.6), whose step 5.11.3 runs the focusing steps and fires blur/focusout/focus/focusin — this build now
 * has focus events, so that list has a producer and step 7 must be written". That is what a two-sided producer
 * assertion is FOR: it stayed silent while §6.6's focus model was absent and fired the moment it landed.
 *
 * WHY IT IS A QUEUE AND NOT A `focus()` CALL AT PARSE TIME, which is the thing every re-implementation of this
 * gets wrong. `<input autofocus>` does NOT focus the input when the parser sees it. The element is APPENDED to a
 * list on the TOP-LEVEL traversable's active document — even when the element is three iframes down — and the
 * list is drained at a RENDERING OPPORTUNITY, at which point most of its entries are discarded: only the first
 * candidate that is still in a fully active document under the same traversable, whose ancestors have not been
 * pointed somewhere else by a `#fragment`, and which is (or yields) a focusable area, is focused. The rest are
 * thrown away, and the flag makes the whole thing happen ONCE per document. Every one of those clauses exists
 * because a real page hits it: a component library that marks three inputs `autofocus`, a lazily-inserted
 * dialog, a frame that navigates while the document is still parsing.
 *
 * WHAT THIS IS WORTH TO THE SOLVER. Focusing an element runs §6.6.4's focus update steps, which fire four
 * events at the page's own listeners — and an `autofocus` control is precisely the one a page hangs its
 * first-interaction handlers on. Without this the whole of that region is code the forced execution never
 * reaches, because nothing ever gives it a focus to react to.
 *
 * THE STATE IS PER DOCUMENT AND IT TIME-TRAVELS. Both pieces live in a per-realm record — the shape document.c's
 * readiness, §6.6's visibility state and §6.6.2's focused area already use — so the list is a JS ARRAY and the
 * flag a property on an ordinary object: their mutations are property writes the heap COW captures, which is
 * what makes a flow that inserted an `<input autofocus>` see a candidate its sibling does not, and what carries
 * a half-drained list to the IDB cold tier and back. A malloc'd list of node pointers could do neither. */
#include <stdbool.h>
#include <stdint.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/dom/document.h"
#include "core/dom/node.h"
#include "core/frame/window_proxy.h"
#include "core/html/autofocus.h"
#include "core/html/focus.h"

static int g_slot = -1;
static int g_id_flush = -1;
static int g_ready;

/* ---- §6.6.7's data model ------------------------------------------------------------------------------------
 *
 * "Each Document has an autofocus candidates list, initially empty" and "each Document has an autofocus
 * processed flag boolean, initially false". Both on one record, because they are written together — every step
 * that empties the list sets the flag in the next line, and a reader that saw one without the other would run
 * the whole algorithm a second time. */

static JSValue af_record(JSContext *ctx)
{
    JSValue rec = realm_value_get(ctx, g_slot);

    DCHECK(JS_IsObject(rec), "a realm answered for its §6.6.7 autofocus state with no record — the record is "
                             "built with the realm by autofocus_install_document");
    return rec;
}

/* §6.6.7's AUTOFOCUS CANDIDATES for this realm's active document. OWNED. */
static JSValue af_candidates(JSContext *ctx)
{
    JSValue rec = af_record(ctx), list = JS_GetPropertyStr(ctx, rec, "candidates");

    JS_FreeValue(ctx, rec);
    DCHECK(JS_IsArray(list), "§6.6.7's autofocus candidates held something that is not a list");
    return list;
}

static bool af_processed(JSContext *ctx)
{
    JSValue rec = af_record(ctx), v = JS_GetPropertyStr(ctx, rec, "processed");
    int b;

    JS_FreeValue(ctx, rec);
    DCHECK(JS_IsBool(v), "§6.6.7's autofocus processed flag held something that is not a boolean");
    b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b > 0;
}

/* The flag is only ever SET — §6.6.7 has no step that clears it, because a document's autofocus is processed
   once and a new document is a new realm with a new record. */
static void af_mark_processed(JSContext *ctx)
{
    JSValue rec = af_record(ctx);

    JS_SetPropertyStr(ctx, rec, "processed", JS_TRUE);
    JS_FreeValue(ctx, rec);
}

/* ---- INFRA's list operations over a JS Array ---------------------------------------------------------------- */

static uint32_t list_len(JSContext *ctx, JSValueConst list)
{
    JSValue len = JS_GetPropertyStr(ctx, list, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    return n;
}

/* INFRA's "empty a list", and the truncation §6.6.7 spells that way at steps 4.1 and 5.11.1. */
static void list_truncate(JSContext *ctx, JSValueConst list, uint32_t n)
{
    JS_SetPropertyStr(ctx, list, "length", JS_NewUint32(ctx, n));
}

/* INFRA's "remove", which CLOSES THE GAP — that is what makes candidates[0] the algorithm's cursor and why this
   machine needs no cursor of its own to park with. */
static void list_remove_at(JSContext *ctx, JSValueConst list, uint32_t i)
{
    uint32_t n = list_len(ctx, list), k;

    DCHECK(i < n, "§6.6.7 removed an autofocus candidate at an index the list does not have");
    for (k = i + 1; k < n; k++)
        JS_SetPropertyUint32(ctx, list, k - 1, JS_GetPropertyUint32(ctx, list, k));
    list_truncate(ctx, list, n - 1);
}

/* Membership by OBJECT IDENTITY, which is what the insertion steps' "remove the element from" means: the DOM's
   identity table gives a node exactly one wrapper, so two reads of the same element are the same pointer. */
static int64_t list_index_of(JSContext *ctx, JSValueConst list, JSValueConst v)
{
    uint32_t n = list_len(ctx, list), i;

    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, list, i);
        bool same = JS_VALUE_GET_PTR(e) == JS_VALUE_GET_PTR(v);

        JS_FreeValue(ctx, e);
        if (same) return (int64_t)i;
    }
    return -1;
}

/* ---- the navigable facts §6.6.7 asks for -------------------------------------------------------------------- */

/* "target's node navigable's TOP-LEVEL TRAVERSABLE's ACTIVE DOCUMENT", as a realm. NULL only when that document
   lives in another WASM instance, which every caller asserts at its own step rather than answering for a
   document it cannot see. */
static JSContext *top_document_realm(JSContext *docctx)
{
    JSValue top = window_proxy_top_navigable(docctx, document_window_proxy(docctx));
    JSContext *realm = NULL;

    if (JS_IsObject(top) && !window_proxy_is_remote(top))
        realm = window_proxy_realm(docctx, top);
    JS_FreeValue(docctx, top);
    DCHECK(realm != NULL,
           "HTML §6.6.7 asked for the active document of a top-level traversable in ANOTHER WASM instance — the "
           "autofocus candidates list belongs to THAT instance's document, so appending to it is a "
           "cross-instance operation (SECURITY.md's closed set) that suspends the asking flow. Build it in "
           "core/frame/window_proxy.c beside the other cross-instance requests");
    return realm;
}

/* §6.6.7's insertion steps step 4, "target's ACTIVE SANDBOXING FLAG SET has the SANDBOXED AUTOMATIC FEATURES
   BROWSING CONTEXT FLAG". A document's sandboxing flags come from §7.6.2's navigable container — an `<iframe
   sandbox>` — through §7.2.6's policy container, and this engine's policy container carries a CSP and nothing
   else, so no document it builds has a flag set to read. Not a skipped step: a condition whose state cannot
   exist, evaluated at the step that asks it, exactly as core/html/html_form.c evaluates the SANDBOXED FORMS
   flag. The day the flag set lands in the policy container, this and that one are the sites that read it. */
static bool sandboxes_automatic_features(JSContext *docctx)
{
    (void)docctx;
    return false;
}

/* §6.6.7 flush step 4's second disjunct — "topDocument has non-null TARGET ELEMENT".
   A Document's target element is HTML §7.4.6.4's: SCROLL TO THE FRAGMENT sets it from the document's indicated
   part, and no other algorithm in any standard writes it. This build performs no scroll-to-the-fragment — it
   has no scrolling box for one to scroll — so no Document has ever had one set. That is ASSERTED against the
   producer rather than written down as a comment, which is the whole difference between a check and a claim. */
static bool top_document_has_target_element(JSContext *docctx)
{
    realm_awaits(docctx, "scrollTo",
                 "HTML §6.6.7 flush autofocus candidates step 4 skips the whole algorithm — emptying the "
                 "candidates and setting the processed flag — when topDocument has a non-null TARGET ELEMENT, "
                 "which HTML §7.4.6.4's SCROLL TO THE FRAGMENT sets from the document's indicated part. This "
                 "build now has a way to scroll a scrolling box, so §7.4.6.4 can run and a Document can hold a "
                 "target element: store it on the Document and read it here");
    return false;
}

/* §6.6.7 flush steps 5.7-5.8 — "let inclusiveAncestorDocuments be a list consisting of the active document of
   doc's INCLUSIVE ANCESTOR NAVIGABLES; if any Document in inclusiveAncestorDocuments has non-null target
   element, then continue". Same absent state as step 4's, and the walk that reads it is absent with it: there is
   no field for the walk to collect, so writing the climb now would be a climb to nowhere. The assert names BOTH
   halves, so the day the producer lands the reader knows the step is a walk and not a single read. */
static bool an_ancestor_document_has_target_element(JSContext *docctx)
{
    realm_awaits(docctx, "scrollTo",
                 "HTML §6.6.7 flush autofocus candidates step 5.8 SKIPS a candidate when any of the active "
                 "documents of its navigable's INCLUSIVE ANCESTOR NAVIGABLES has a non-null TARGET ELEMENT — a "
                 "`#fragment` in an ancestor frame outranks a descendant's autofocus. This build now has a way "
                 "to scroll a scrolling box, so §7.4.6.4 can set one: store the target element on the Document, "
                 "and write this step as the climb of doc's inclusive ancestor navigables that it is");
    return false;
}

/* §6.6.7 flush step 5.5 — "if doc's SCRIPT-BLOCKING STYLE SHEET SET is not empty, then return", leaving this
   candidate at the head of the list for the next rendering opportunity. HTML §4.2.4's set is filled by creating
   a CSS style sheet that blocks scripts, and this engine creates no style sheet objects at all. */
static bool has_script_blocking_style_sheets(JSContext *docctx)
{
    realm_awaits(docctx, "CSSStyleSheet",
                 "HTML §6.6.7 flush autofocus candidates step 5.5 RETURNS, leaving the head candidate in the "
                 "list for the next rendering opportunity, while doc's SCRIPT-BLOCKING STYLE SHEET SET (HTML "
                 "§4.2.4) is not empty — the candidate is the best one but its document is not ready to be "
                 "focused. This build now has a CSSStyleSheet, so that set has a producer: give the Document "
                 "the set, add each script-blocking sheet to it while it loads, and read it here");
    return false;
}

/* ---- §6.6.7's INSERTION STEPS -------------------------------------------------------------------------------
 *
 * "When an element with the autofocus attribute specified is INSERTED INTO A DOCUMENT, run the following steps."
 * The standard's own note is why the list is not filtered here: "we do not check if an element is a focusable
 * area before storing it in the autofocus candidates list, because even if it is not a focusable area when it is
 * inserted, it could become one by the time flush autofocus candidates sees it." */
int autofocus_element_inserted(lxb_dom_element_t *el, JSStepHdr *h, uint8_t *ua_phase)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el);
    JSContext *target, *topctx;
    JSValue wrap, list;
    int64_t at;
    size_t len = 0;
    bool allowed = false;
    int r;

    DCHECK(g_ready, "an element was inserted before autofocus_init declared §6.6.7's state");
    DCHECK(n->type == LXB_DOM_NODE_TYPE_ELEMENT, "§6.6.7's insertion steps ran for a node that is not an "
                                                 "element");
    /* The algorithm is stated over "an element with the AUTOFOCUS ATTRIBUTE SPECIFIED", so the attribute is the
       first question — asked here rather than at the caller, because the caller is DOM §4.2.3's per-node walk
       and every condition an insertion step has belongs to the step. */
    if (!lxb_dom_element_get_attribute(el, (const lxb_char_t *)"autofocus", 9, &len)) return 0;
    /* STEP 1 is the standard's own OPTIONAL return: "if the user has indicated (for example, by starting to
       type in a form control) that they do not wish focus to be changed, then optionally return." It is
       conditioned on an indication that arrives through an input device, and this user agent dispatches no
       trusted input event for one to arrive on — so there is no indication, and the branch not taken is the
       optional one the standard permits either way. */
    /* STEPS 2-3: "let target be the element's node document"; "if target is not fully active, then return". */
    target = n->owner_document ? document_active_realm_of(lxb_dom_interface_node(n->owner_document)) : NULL;
    if (!target || !document_fully_active(target)) return 0;
    if (sandboxes_automatic_features(target)) return 0;                                     /* step 4 */
    /* STEP 5's ALLOW FOCUS STEPS, as the REQUEST they are. Their second clause is §6.4.1's TRANSIENT
       ACTIVATION, which is UNKNOWN EXTERNAL STATE (core/html/user_activation.h), so the answer FORKS: this
       flow takes one arm and a sibling is snapshotted holding the other, and both reach code worth running —
       one returns here with the element never becoming a candidate, the other queues it for §8.1.7.3 step 7's
       flush and the four focus events that fire at the page's own listeners.
       THE FORK IS RETURNED to DOM §4.2.3's walk and forwarded by idl_tree_drain to the driving machine, which
       is what snapshots the flow; the re-entry lands back on this line inside the same node's phase, so
       nothing this node already had done to it happens twice.
       The first clause short-circuits, so a document that is same origin with its top-level document is
       answered without asking anything — which is every document in an ordinary page. Only an `autofocus`
       element inserted into a CROSS-ORIGIN-embedded document reaches the question. */
    r = focus_allow_focus_steps_run(target, h, ua_phase, &allowed);                         /* step 5 */
    if (r) {
        DCHECK(r == JS_STEP_FORK, "§6.6.6's allow focus steps answered §6.6.7's insertion steps with something "
                                  "other than a fork — the only thing they ask is §6.4.1's activation state");
        return r;
    }
    if (!allowed) return 0;
    topctx = top_document_realm(target);                                                    /* step 6 */
    if (!topctx) return 0;
    /* STEP 7: "if topDocument's autofocus processed flag is false, then REMOVE the element from topDocument's
       autofocus candidates, and APPEND the element to topDocument's autofocus candidates." Remove-then-append
       rather than append-if-absent: re-inserting an element MOVES it to the back of the queue, which is what
       makes the last-inserted `autofocus` element the one a page that re-parents its dialog gets. */
    if (af_processed(topctx)) return 0;
    /* The wrapper is minted in the ELEMENT's own realm — the DOM's identity table gives a node one wrapper
       whatever realm asks, and its prototype comes from the realm that first wrapped it, which for an element
       of `target`'s tree is `target`. */
    wrap = node_wrap(target, n);
    list = af_candidates(topctx);
    at = list_index_of(topctx, list, wrap);
    if (at >= 0) list_remove_at(topctx, list, (uint32_t)at);
    JS_SetPropertyUint32(topctx, list, list_len(topctx, list), wrap);   /* CONSUMES wrap */
    JS_FreeValue(topctx, list);
    return 0;
}

/* THE SAME STEPS FOR THE TREE THE PARSER BUILT, and the one caller with NO DRIVING MACHINE: this runs inside
   document install, at the pre-boot COW baseline, where no flow exists to snapshot. The header is therefore
   NULL — and that is not a shape the steps tolerate, it is one focus_allow_focus_steps_run ASSERTS against at
   the moment the question is reached, so a parsed tree that needs the fork names the mechanism to build rather
   than silently taking an arm. Every step before that question is decided without asking, which is why this
   walk is correct for every document whose parsed tree does not reach it. */
void autofocus_document_parsed(JSContext *ctx)
{
    lxb_dom_node_t *root = document_root_node(ctx), *n = root;
    uint8_t ua_phase = 0;

    DCHECK(g_ready, "a parsed document reached §6.6.7 before autofocus_init declared its state");
    while (n) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            int r = autofocus_element_inserted(lxb_dom_interface_element(n), NULL, &ua_phase);

            DCHECK(r == 0, "§6.6.7's insertion steps parked while walking the tree the PARSER built — that walk "
                           "runs inside document install, which has no flow and no machine to park at");
            (void)r;
        }
        if (n->first_child) { n = n->first_child; continue; }
        while (n && !n->next) n = (n == root) ? NULL : n->parent;
        n = n ? n->next : NULL;
    }
}

/* ---- FLUSH AUTOFOCUS CANDIDATES, as a machine ---------------------------------------------------------------
 *
 * IT RESTS ONCE PER CANDIDATE, and that is not decoration: step 5 is a `while` over a list a page controls the
 * length of, and a stage that walked it to the end would be a span the scheduler cannot preempt. The walk needs
 * NO CURSOR to be resumable, because every iteration of step 5 either removes candidates[0] or returns — the
 * list is the cursor, and a resume looks at its head again. */
#define FLUSH_STAGES(X) \
    X(AF_START,     "HTML §6.6.7 flush autofocus candidates steps 1-4 (the processed flag, the candidates, the " \
                    "empty check, and the early return that empties the list when topDocument's focus is "       \
                    "already placed)")                                                                          \
    X(AF_CANDIDATE, "HTML §6.6.7 flush autofocus candidates step 5 (one candidate per rest: its document's "     \
                    "fully-active and top-level-traversable tests, its script-blocking style sheets, the "       \
                    "ancestor target-element skip, and getting its focusable area)")                             \
    X(AF_FOCUS,     "HTML §6.6.7 flush autofocus candidates step 5.11.3 (run HTML §6.6.4's focusing steps for "  \
                    "the candidate — which fires blur/focusout down the old focus chain and focus/focusin up "   \
                    "the new one)")
enum { IDL_STEP_STAGE_BASE(FLUSH_STAGES) FLUSH_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const FLUSH_STEPS[] = { FLUSH_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    uint8_t fphase;                       /* step 5.11.3's focusing-steps request */
    JSValue target;                       /* the candidate being focused, held across it (owned) */
    JSValue cb[FOCUS_ELEMENT_CB_SLOTS];   /* that request's buffer — its width is focus.h's, not a number */
} FlushState;

static void flush_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    FlushState *s = st;
    int k;

    v->val(ctx, &s->target);
    STEP_CB_FOREACH(s->cb, k) v->val(ctx, &s->cb[k]);
}

static void flush_release(JSContext *ctx, void *st)
{
    FlushState *s = st;
    int k;

    JS_FreeValue(ctx, s->target);
    s->target = JS_UNDEFINED;
    STEP_CB_FOREACH(s->cb, k) {
        JS_FreeValue(ctx, s->cb[k]);
        s->cb[k] = JS_UNDEFINED;
    }
}

/* The realm whose ACTIVE DOCUMENT a candidate's node document is. §6.6.7 step 5.4 admits a candidate from any
   document under topDocument's top-level traversable, so this is routinely a CHILD realm and never assumed to
   be the flushing one. */
static JSContext *candidate_realm(JSValueConst el)
{
    lxb_dom_node_t *n = node_of(el);

    DCHECK(n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "§6.6.7's autofocus candidates held something that is not an element — the insertion steps append "
           "the element they ran for, and nothing else reaches the list");
    if (!n || !n->owner_document) return NULL;
    return document_active_realm_of(lxb_dom_interface_node(n->owner_document));
}

static int flush_step(JSContext *ctx, JSStepHdr *hdr, void *state, int argc, JSValueConst *argv,
                      JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    FlushState *s = state;
    JSValue list;
    int k, r;

    (void)argc; (void)argv;
    *presult = JS_UNDEFINED;
    /* THE DELIVERED VALUE BELONGS TO THE ONE REQUEST THIS MACHINE MAKES, and `fphase` is exactly the flag for
       whether one is outstanding — so it is released here, once, on every other re-entry. */
    if (s->fphase == 0) {
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
    }

    if (hdr->stage == AF_START) {
        /* EVERY OWNED FIELD IS ON THE STATE BEFORE ANYTHING THAT CAN FAIL — the failure path tears this state
           down through flush_release, which frees exactly what the state holds and nothing else. */
        s->target = JS_UNDEFINED;
        s->fphase = 0;
        STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;

        DCHECK(g_ready, "§6.6.7's flush ran before autofocus_init declared its state");
        DCHECK(window_proxy_is_top_level(document_window_proxy(ctx)),
               "§6.6.7's flush autofocus candidates ran for a document that is not a top-level traversable's "
               "active document — §8.1.7.3 step 7 states that as its condition, and the insertion steps append "
               "only to the TOP document's list, so any other document's is empty by construction and flushing "
               "it would be a walk that can never do anything");
        if (af_processed(ctx)) return 0;                                                    /* step 1 */
        list = af_candidates(ctx);                                                          /* step 2 */
        if (list_len(ctx, list) == 0) { JS_FreeValue(ctx, list); return 0; }                /* step 3 */
        /* STEP 4: "if topDocument's focused area is not topDocument itself, or topDocument has non-null target
           element" — the page, a script or a fragment has already placed the focus, and an autofocus must not
           take it back. Both halves discard the list and mark it processed, so this document never tries again. */
        if (!focus_focused_area_is_viewport(ctx) || top_document_has_target_element(ctx)) {
            list_truncate(ctx, list, 0);                                                    /* step 4.1 */
            af_mark_processed(ctx);                                                         /* step 4.2 */
            JS_FreeValue(ctx, list);
            return 0;                                                                       /* step 4.3 */
        }
        JS_FreeValue(ctx, list);
        STEP_GOTO(hdr->stage, AF_CANDIDATE, &s->fphase, NULL);
        return JS_STEP_YIELD;
    }

    if (hdr->stage == AF_CANDIDATE) {
        JSContext *elctx;
        JSValue el;

        list = af_candidates(ctx);
        if (list_len(ctx, list) == 0) {          /* step 5's `while candidates is not empty` is over */
            JS_FreeValue(ctx, list);
            return 0;
        }
        el = JS_GetPropertyUint32(ctx, list, 0);                                            /* step 5.1 */
        elctx = candidate_realm(el);                                                        /* step 5.2 */
        /* STEP 5.3 — a document that is no realm's ACTIVE document is not the active document of a navigable
           either, so §7.3.1's first conjunct already fails for it. */
        if (!elctx || !document_fully_active(elctx)) {
            list_remove_at(ctx, list, 0);
            JS_FreeValue(ctx, el);
            JS_FreeValue(ctx, list);
            return JS_STEP_YIELD;
        }
        /* STEP 5.4: "if doc's node navigable's top-level traversable is not the same as topDocument's node
           navigable" — topDocument IS its traversable's active document (AF_START asserted it), so comparing
           the two documents' realms compares the two navigables. */
        if (top_document_realm(elctx) != ctx) {
            list_remove_at(ctx, list, 0);
            JS_FreeValue(ctx, el);
            JS_FreeValue(ctx, list);
            return JS_STEP_YIELD;
        }
        /* STEP 5.5 RETURNS WITHOUT REMOVING — the candidate stays at the head and the flag stays false, so the
           next rendering opportunity tries the same one again. */
        if (has_script_blocking_style_sheets(elctx)) {
            JS_FreeValue(ctx, el);
            JS_FreeValue(ctx, list);
            return 0;
        }
        list_remove_at(ctx, list, 0);                                                       /* step 5.6 */
        if (an_ancestor_document_has_target_element(elctx)) {                          /* steps 5.7-5.8 */
            JS_FreeValue(ctx, el);
            JS_FreeValue(ctx, list);
            return JS_STEP_YIELD;
        }
        /* STEPS 5.9-5.10: target is the element, or the focusable area it yields. */
        if (!focus_focusable_area_exists(elctx, el)) {
            JS_FreeValue(ctx, el);
            JS_FreeValue(ctx, list);
            return JS_STEP_YIELD;
        }
        /* STEP 5.11.1-5.11.2, BEFORE the focusing steps run and not after: the algorithm commits to this
           candidate first, so a `focus` listener that inserts another `autofocus` element finds the flag
           already set and appends nothing (the insertion steps' own step 7). */
        list_truncate(ctx, list, 0);                                                        /* step 5.11.1 */
        af_mark_processed(ctx);                                                             /* step 5.11.2 */
        JS_FreeValue(ctx, list);
        s->target = el;                          /* ownership moves onto the state for the request below */
        STEP_GOTO(hdr->stage, AF_FOCUS, &s->fphase, NULL);
    }

    DCHECK(hdr->stage == AF_FOCUS, "§6.6.7's flush resumed into a stage the algorithm does not have");
    {
        /* STEP 5.11.3: "run the focusing steps for target." The realm is re-derived rather than remembered
           because a `JSContext *` is not a value a snapshot can carry across a park; the ELEMENT is, and it is
           what the state holds. */
        JSContext *elctx = candidate_realm(s->target);

        DCHECK(elctx != NULL,
               "§6.6.7 step 5.11.3 reached a candidate whose node document is no realm's active document — step "
               "5.3 admitted it as fully active, and nothing between the two runs a line of the page's code");
        r = focus_element_run(elctx, s->target, &s->fphase, STEP_CB(s->cb), cb_result, out_cb, out_argc);
        if (r > 0) return r;                     /* parked on the page's focus listeners */
        DCHECK(r == 0, "§6.6.4's focusing steps answered §6.6.7 with neither a park nor a completion");
        JS_FreeValue(ctx, s->target);
        s->target = JS_UNDEFINED;
        /* Step 5.11.1 emptied the list, so step 5's `while` has nothing left to look at. */
        return 0;
    }
}

static const IdlStepDecl FLUSH_STEP = {
    flush_step, sizeof(FlushState), flush_visit, flush_release,
    "HTML §6.6.7 flush autofocus candidates, run by §8.1.7.3 update the rendering step 7",
    FLUSH_STEPS
};

/* THE INTERNAL DOOR, MINTED IN topDocument's OWN REALM, for core/html/focus.c's reason: a step function carries
   its DEFINING realm, and this machine reads the candidates list, the focused area and the traversable off that
   ctx. It is minted through idl_step_function like every declared member, which is what keeps the pool's name
   for it — a hand-written JS_NewCFunction2 leaves the machine anonymous in every diagnostic. OWNED. */
static JSValue flush_door_new(JSContext *ctx)
{
    JSValue fn;

    DCHECK(g_id_flush >= 0, "the rendering algorithm reached §6.6.7's flush before autofocus_init declared it");
    fn = idl_step_function(ctx, "flushAutofocusCandidates", 0, g_id_flush);
    CHECK(!JS_IsException(fn), "autofocus: §6.6.7's flush door could not be allocated");
    return fn;
}

int autofocus_flush_run(JSContext *ctx, uint8_t *phase, JSValue *cb, int cb_cap, JSValue in,
                        JSValue **out_cb, int *out_argc)
{
    JSValue out = JS_UNDEFINED;
    int r;

    /* ASKED ON BOTH LEGS, because the resume leg forwards the same capacity and a caller that got the first one
       right by accident must not get the second one wrong in silence. */
    DCHECK(cb_cap >= AUTOFOCUS_FLUSH_CB_SLOTS,
           "§8.1.7.3 step 7's flush request was handed a buffer narrower than step_call_run's [this, func] "
           "shape");
    if (*phase == 0) {
        JSValue fn = flush_door_new(ctx);

        /* step_call_run DUPS the callee into the request buffer, which is what holds it across the suspension —
           so this realm's door is released here and the parked call still owns one. */
        r = step_call_run(ctx, phase, cb, cb_cap, fn, JS_UNDEFINED, 0, NULL, in, &out, out_cb, out_argc);
        JS_FreeValue(ctx, fn);
        DCHECK(r == JS_STEP_CALL, "§8.1.7.3 step 7's flush request answered without parking");
        return r;
    }
    r = step_call_run(ctx, phase, cb, cb_cap, JS_UNDEFINED, JS_UNDEFINED, 0, NULL, in, &out, out_cb, out_argc);
    DCHECK(r == 0, "§6.6.7's flush resumed into something other than its answer");
    DCHECK(JS_IsUndefined(out),
           "§6.6.7's flush answered with a value — the algorithm has no result, so a value here is a member's "
           "return leaking through the door");
    JS_FreeValue(ctx, out);
    return 0;
}

/* ---- declaration and install -------------------------------------------------------------------------------- */

void autofocus_init(JSContext *ctx)
{
    DCHECK(!g_ready, "autofocus_init ran twice — §6.6.7's machine is declared once per agent");
    g_slot = realm_value_declare(ctx, "HTML §6.6.7 autofocus candidates and processed flag");
    g_id_flush = idl_method_id_step(ctx, NULL, 0, NULL, 0, &FLUSH_STEP, 0);
    g_ready = 1;
}

void autofocus_install_document(JSContext *ctx)
{
    JSValue rec, list;

    DCHECK(g_ready, "§6.6.7's per-document state was installed before autofocus_init ran");
    rec = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(rec), "autofocus: this realm's §6.6.7 record could not be allocated");
    list = JS_NewArray(ctx);
    CHECK(!JS_IsException(list), "autofocus: this realm's §6.6.7 candidates list could not be allocated");
    JS_SetPropertyStr(ctx, rec, "candidates", list);
    JS_SetPropertyStr(ctx, rec, "processed", JS_FALSE);
    realm_value_set(ctx, g_slot, rec);
}
