/* FULLSCREEN §2 "Model" and §3 "API". See fullscreen.h for what is here, for why "fullscreen is supported" is
   TRUE in a build with no output device, and for the two named residuals. */
#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/document.h"
#include "core/dom/node.h"
#include "core/fullscreen/fullscreen.h"
#include "core/idl_args.h"
#include "core/permissions_policy/permissions_policy.h"

/* §2: "Fullscreen is supported if there is no previously-established user preference, security risk, or
 * platform limitation."
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
 * IT IS A FUNCTION AND NOT A CONSTANT, and static because §3 has exactly one reader of it today. Its other two
 * readers arrive with the model: the `requestFullscreen(options)` method steps' step 5 lists "Fullscreen is
 * supported" as one of the five conditions that set `error`, and `fullscreenEnabled` is the second conjunct
 * here. */
static bool fullscreen_is_supported(void)
{
    return true;
}

/* WEB IDL §3.7.6 "Attributes" — an attribute getter's steps begin by establishing that `this` is a platform
   object implementing the interface, and a receiver that is not one gets a TypeError. It is a THROW at the read
   and not an engine invariant, for the reason core/dom/document_domain.c's own receiver states: the conformance
   corpus pulls these accessors off the prototype and applies them to the wrong receiver deliberately. */
static lxb_dom_node_t *fs_document_receiver(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_node_t *n = node_of(this_val);

    if (!n || n->type != LXB_DOM_NODE_TYPE_DOCUMENT) {
        JS_ThrowTypeError(ctx, "this is not a Document");
        return NULL;
    }
    return n;
}

/* §3: "The fullscreenEnabled getter steps are to return true if this is allowed to use the "fullscreen"
 * feature and fullscreen is supported, and false otherwise."
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

void fullscreen_install_document_members(JSContext *ctx, JSValueConst document_proto)
{
    /* `[LegacyLenientSetter] readonly attribute boolean fullscreenEnabled` — installed with NO setter, which is
       fullscreen.h's second named residual: Web IDL §3.4.2's no-op setter is a platform-wide installer form
       this engine does not have, and a readonly accessor throws in strict mode where a browser ignores. */
    idl_install_accessor(ctx, document_proto, "fullscreenEnabled", js_document_fullscreen_enabled, 0, -1);
}
