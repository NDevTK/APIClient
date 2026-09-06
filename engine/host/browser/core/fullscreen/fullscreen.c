/* FULLSCREEN §2 "Model" and §3 "API". See fullscreen.h for what is here, for why "fullscreen is supported" is
   TRUE in a build with no output device, and for the two named residuals. */
#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/css/top_layer.h"
#include "core/dom/document.h"
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"
#include "core/events/event_target.h"
#include "core/fullscreen/fullscreen.h"
#include "core/idl_args.h"
#include "core/permissions_policy/permissions_policy.h"

/* FULLSCREEN §2 "Model": "Fullscreen is supported if there is no previously-established user preference,
 * security risk, or platform limitation."
 *
 * ALL THREE DISJUNCTS ARE ANSWERED AND NONE IS ASSUMED — fullscreen.h carries the argument in full, and the
 * short form is that §2's three are facts about the ENVIRONMENT and the USER rather than about how much of the
 * standard is written, and that §8 "Security and Privacy Considerations" states outright that a missing device
 * may not move what this API answers: "some platforms might not have a keyboard, where user agents may ignore
 * the keyboard lock state. However, this should not affect the web-observable behavior of the
 * requestFullscreen() method or be exposed in other ways, to avoid fingerprinting."
 *   — NO PREVIOUSLY-ESTABLISHED USER PREFERENCE: this agent has no preference store for one to have been
 *     established in. Not "the user has expressed no preference" — there is no place a preference could live,
 *     which is a different and stronger statement, and the day one exists this predicate reads it.
 *   — NO SECURITY RISK: §8's risk is spoofing, and it is stated entirely in terms of what the END USER SEES
 *     ("User agents should ensure, e.g. by means of an overlay, that the end user is aware something is
 *     displayed fullscreen"). An agent that displays nothing shows nobody a forged environment.
 *   — NO PLATFORM LIMITATION: §2's model is an ordered element stack, per-element flags and two events, all
 *     defined with no output device; the pixels are §5 "Rendering", which is presentation. What this build has
 *     not yet WRITTEN of that model is a gap in this engine and is honestly ABSENT at the member — see
 *     fullscreen.h's first residual — never a limitation of the platform, which is the reading that would make
 *     the gap read as settled and take the permissions-policy conjunct below down with it.
 *
 * IT IS A FUNCTION AND NOT A CONSTANT, and static because FULLSCREEN §3 "API" has exactly one reader of it
 * today. Its other two readers arrive with the model: the `requestFullscreen(options)` method steps' step 5
 * lists "Fullscreen is supported" as one of the five conditions that set `error`, and `fullscreenEnabled` is
 * the second conjunct here. */
static bool fullscreen_is_supported(void)
{
    return true;
}

/* ---- §2's FULLSCREEN FLAG, and the FULLSCREEN ELEMENT it orders --------------------------------------------- */

/* FULLSCREEN §2 "Model": "All elements have an associated fullscreen flag. Unless stated otherwise it is
 * unset."
 *
 * AN OWN SLOT UNDER A PRIVATE SYMBOL, ON THE ELEMENT'S WRAPPER. See fullscreen.h: the wrapper is where per-flow
 * per-node state lives in this engine, because a slot written as a property write is captured by the heap COW
 * delta and a field on the Lexbor node is one answer for every flow. The Symbol is the AGENT's and is never
 * published, so a page cannot name the flag it is not allowed to see. */
static JSValue g_flag_key = JS_UNDEFINED;
static JSAtom  g_flag_atom = JS_ATOM_NULL;

/* "element's fullscreen flag is set". An ABSENT slot is §2's "unset" — the initial value the standard names,
   and the reason this costs nothing for the elements no flow ever fullscreens. */
static bool fs_flag_is_set(JSContext *ctx, JSValueConst element)
{
    JSValue v;
    bool set;

    DCHECK(g_flag_atom != JS_ATOM_NULL,
           "FULLSCREEN §2 Model's fullscreen flag was read before fullscreen_init minted its slot key — the key "
           "is declared once per AGENT from core/dom/document.c's document_init, and a read before that would "
           "answer 'unset' for every element by asking a question with no name");
    if (!JS_IsObject(element)) return false;
    if (JS_GetOwnSlot(ctx, &v, element, g_flag_atom) <= 0) return false;
    set = JS_ToBool(ctx, v) != 0;
    JS_FreeValue(ctx, v);
    return set;
}

/* The predicate css-position-4 §3's ordered read is given. It is one own-slot read: no page code, no getter,
   no suspension, and it reaches none of css-position-4 §3.3's manipulation algorithms — which is the
   contract top_layer.h states and the length assert on the other side of the walk is what checks. */
static bool fs_flag_pred(JSContext *ctx, JSValueConst el, void *opaque)
{
    (void)opaque;
    return fs_flag_is_set(ctx, el);
}

/* FULLSCREEN §2 "Model": "The fullscreen element is the topmost element in the document's top layer whose
   fullscreen flag is set, if any, and null otherwise." The ORDER is css-position-4 §3 "Top Layer"'s and is
   read through that component's own accessor; the FLAG is this file's. Nothing here indexes the layer. */
JSValue fullscreen_element(JSContext *ctx, lxb_dom_node_t *document)
{
    JSValue doc, el;

    DCHECK(document != NULL && document->type == LXB_DOM_NODE_TYPE_DOCUMENT,
           "FULLSCREEN §2 Model's fullscreen element was asked of something that is not a Document — §2 states "
           "it as a fact ALL DOCUMENTS have, and its one set is the document's top layer, so a non-document "
           "here would be answered out of a top layer that belongs to some other document or to none");
    doc = node_wrap(ctx, document);
    el = top_layer_topmost(ctx, doc, fs_flag_pred, NULL);
    JS_FreeValue(ctx, doc);
    return el;
}

/* WEB IDL §3.7.6 "Attributes" — an attribute getter's steps begin by establishing that `this` is a platform
   object implementing the interface, and a receiver that is not one gets a TypeError. It is a THROW at the read
   and not an engine invariant, for the reason core/dom/document_domain.c's own receiver states: the conformance
   corpus pulls these accessors off the prototype and applies them to the wrong receiver deliberately. */
static lxb_dom_node_t *fs_document_receiver(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_node_t *n = node_of(this_val);

    /* ONE ANSWER TO ONE QUESTION — core/dom/document.h's `document_is` is Web IDL §3.7's implementation-check
       step 3 for this interface, and the node-type test that used to be written out here was a second copy of
       it. The [LegacyLenientSetter] installs below hand that same predicate to §3.7.6's setter, so a member
       reached through the getter and a member reached through the setter agree by construction. */
    if (!document_is(this_val)) {
        JS_ThrowTypeError(ctx, "this is not a Document");
        return NULL;
    }
    return n;
}

/* FULLSCREEN §3 "API": "The fullscreenEnabled getter steps are to return
 * true if this is allowed to use the "fullscreen" feature and fullscreen is supported, and
 * false otherwise."
 *
 * "ALLOWED TO USE" IS HTML §4.8.5 "The `iframe` element"'s ALGORITHM, not a same-origin comparison, and it is
 * asked of the RECEIVER'S document rather than of the running realm. `frame.contentDocument.fullscreenEnabled`
 * read from the parent is a question about the CHILD — its browsing context, its fully-active walk, its policy
 * and its origin — and answering it out of the calling realm would report the parent's permission for every
 * nested document, which is the per-realm-fact defect core/realm.h names.
 *
 * A DOCUMENT WITH NO REALM IS §4.8.5 STEP 1, COMPUTED. "If document's browsing context is null, then return
 * false" — and a Document that is the active document of no navigable (`createHTMLDocument`, a DOMParser parse,
 * XHR's `responseXML`) is exactly that document, which document_active_realm_of answers NULL for. So the false
 * below is step 1's answer rather than a hole where a lookup failed, and there is no second field to consult:
 * §4.8.5's own step 3 could not run for such a document either, since Permissions Policy §9.5 never created a
 * policy for a navigable it has none of. */
static JSValue js_document_fullscreen_enabled(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *doc = fs_document_receiver(ctx, this_val);
    JSContext *docctx;

    (void)magic;
    if (!doc) return JS_EXCEPTION;
    docctx = document_active_realm_of(doc);
    if (!docctx)
        return JS_NewBool(ctx, false);
    return JS_NewBool(ctx, document_allowed_to_use(docctx, PP_FEATURE_FULLSCREEN) && fullscreen_is_supported());
}

/* FULLSCREEN §3 "API": "The fullscreen getter steps are to return false if
   this's fullscreen element is null, and true otherwise." One step, over §2's concept above — and
   `// historical` in the IDL, with the spec's own note beside it: "Use the fullscreenElement attribute
   instead." Installed anyway, because what a page does with a historical member is the solver's business and
   not this file's. */
static JSValue js_document_fullscreen(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *doc = fs_document_receiver(ctx, this_val);
    JSValue el;
    bool some;

    (void)magic;
    if (!doc) return JS_EXCEPTION;
    el = fullscreen_element(ctx, doc);
    some = !JS_IsNull(el);
    JS_FreeValue(ctx, el);
    return JS_NewBool(ctx, some);
}

/* FULLSCREEN §3 "API"'s `DocumentOrShadowRoot` member, over a receiver that is a Document OR a ShadowRoot —
 * ONE implementation, because the two differ only in what step 2 retargets AGAINST and what step 3 compares
 * a tree with, which is exactly how core/html/focus.c holds the same mixin's `activeElement`. Four steps:
 *   1. "If this is a shadow root and its host is not connected, then return null."
 *   2. "Let candidate be the result of retargeting fullscreen element against this."
 *   3. "If candidate and this are in the same tree, then return candidate."
 *   4. "Return null."
 *
 * STEP 2's "fullscreen element" IS WRITTEN WITH NO OWNER, and this file resolves it as THIS'S NODE DOCUMENT'S
 * — stated here rather than assumed, because a bare term is the one thing a citation cannot settle. It is the
 * only reading under which the step has a referent at all: FULLSCREEN §2 "Model" gives the concept to
 * DOCUMENTS ("All documents have an associated fullscreen element"), a shadow root is not a document and has
 * none of its own, and DOM
 * §4.4 "Interface Node" makes the two readings identical for the other receiver — "The node document of a
 * document is that document itself." So one expression serves both arms and there is no second answer for them
 * to disagree about.
 *
 * A NULL FULLSCREEN ELEMENT NEEDS NO ARM OF ITS OWN, which is why there is no early return here: DOM §4.8
 * "Interface ShadowRoot"'s retargeting returns A unchanged when A is not a node, so step 2 answers null, and
 * null is in no tree, so step 3 is false and step 4 answers. That is the operator, not a skipped step — and it
 * is the whole of what this member does today, for the reason fullscreen.h's first residual gives. */
static JSValue js_fullscreen_element(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *n = node_of(this_val), *doc, *cand_node;
    JSValue candidate, retargeted;

    (void)magic;
    if (!n || (n->type != LXB_DOM_NODE_TYPE_DOCUMENT && !shadow_root_is(n)))
        return JS_ThrowTypeError(ctx, "this is not a Document or a ShadowRoot");
    if (shadow_root_is(n)) {                                                            /* step 1 */
        lxb_dom_element_t *host = shadow_root_host(n);

        DCHECK(host != NULL,
               "DOM §4.8 Interface ShadowRoot's attach a shadow root gives every shadow root a HOST, and this "
               "one answered with none — FULLSCREEN §3 API's fullscreenElement step 1 asks whether that host is "
               "connected, so a null here is a shadow root whose host the tree lost and the step would read as "
               "'not connected' for a tree that is in the document");
        if (!node_is_connected(lxb_dom_interface_node(host))) return JS_NULL;
    }
    /* Step 2's operand — this's NODE DOCUMENT's fullscreen element; see the note above for why the term is
       resolved this way and why one expression serves both receivers. */
    doc = n->type == LXB_DOM_NODE_TYPE_DOCUMENT
              ? n : (n->owner_document ? lxb_dom_interface_node(n->owner_document) : NULL);
    DCHECK(doc != NULL,
           "a node reached FULLSCREEN §3 API's fullscreenElement step 2 with no node document — DOM §4.4 "
           "Interface Node states that every node has one at all times, so this is a shadow root the tree "
           "built without an owner document rather than a case the step has an answer for");
    if (!doc) return JS_NULL;
    candidate = fullscreen_element(ctx, doc);
    retargeted = event_target_retarget(ctx, candidate, this_val);                       /* step 2 */
    JS_FreeValue(ctx, candidate);
    cand_node = node_of(retargeted);
    if (cand_node && node_root(cand_node) == n) return retargeted;                      /* step 3 */
    JS_FreeValue(ctx, retargeted);
    return JS_NULL;                                                                     /* step 4 */
}

void fullscreen_init(JSContext *ctx)
{
    DCHECK(JS_IsUndefined(g_flag_key),
           "fullscreen_init ran twice — §2 Model's fullscreen-flag slot key is the AGENT's, and a second one "
           "would name a DIFFERENT flag on every element the first key had already written");
    g_flag_key = JS_NewSymbol(ctx, "elementFullscreenFlag", false);
    CHECK(!JS_IsException(g_flag_key), "the §2 fullscreen-flag slot key allocation failed");
    g_flag_atom = JS_ValueToAtom(ctx, g_flag_key);
    CHECK(g_flag_atom != JS_ATOM_NULL, "the §2 fullscreen-flag slot key could not be interned");
    /* The row is `document`, not this file's name: a sub-component names the row whose RELEASE reaches it, and
       document_agent_free is what reaches fullscreen_free. */
    agent_state_value("document", &g_flag_key,
                      "the private Symbol FULLSCREEN §2 Model's per-element fullscreen flag hangs off");
    agent_state_atom("document", &g_flag_atom, "that Symbol, interned");
}

void fullscreen_free(JSRuntime *rt)
{
    /* The slot key is the AGENT's — a Symbol nobody frees is a live GC object the runtime's own walk counts as
       a leak. The FLAGS are the elements' and are released with their wrappers. */
    JS_FreeAtomRT(rt, g_flag_atom);
    g_flag_atom = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_flag_key);
    g_flag_key = JS_UNDEFINED;
}

void fullscreen_install_document_members(JSContext *ctx, JSValueConst document_proto)
{
    /* ALL THREE CARRY WEB IDL §3.4.2 [LegacyLenientSetter], so all three are installed through the form that
       gives §3.7.6 Attributes' no-op setter. Nothing here states WHICH members carry it: engine/idlgen.mjs
       reads the extended attribute off the real .idl and reports a member annotated there and installed
       through the plain form, or installed through this form and not annotated. `document_is` is §3.7.6's
       validThis test — a foreign receiver still gets its TypeError, and only an assignment through a real
       Document is the one §3.4.2 ignores. */
    DCHECK(g_flag_atom != JS_ATOM_NULL,
           "FULLSCREEN §3 API's Document members were installed on a realm's prototype before fullscreen_init "
           "minted §2's flag key — two of the three are stated over the fullscreen element, which cannot be "
           "read without it");
    idl_install_accessor_lenient_setter(ctx, document_proto, "fullscreenEnabled",
                                        js_document_fullscreen_enabled, 0, document_is, "Document");
    idl_install_accessor_lenient_setter(ctx, document_proto, "fullscreen",
                                        js_document_fullscreen, 0, document_is, "Document");
    idl_install_accessor_lenient_setter(ctx, document_proto, "fullscreenElement",
                                        js_fullscreen_element, 0, document_is, "Document");
}

void fullscreen_install_shadow_root_members(JSContext *ctx, JSValueConst shadow_root_proto)
{
    DCHECK(g_flag_atom != JS_ATOM_NULL,
           "FULLSCREEN §3 API's ShadowRoot member was installed before fullscreen_init minted §2's flag key");
    /* THE MIXIN MEMBER, AND THE BRAND IS THIS INTERFACE'S. `fullscreenElement` is declared on the
       DocumentOrShadowRoot mixin, so §3.7.6's validThis test is about whichever interface INCLUDES it —
       Document three lines up, ShadowRoot here — which is why the predicate is an argument to the install
       rather than a property of the member. */
    idl_install_accessor_lenient_setter(ctx, shadow_root_proto, "fullscreenElement",
                                        js_fullscreen_element, 0, shadow_root_is_value, "ShadowRoot");
}
