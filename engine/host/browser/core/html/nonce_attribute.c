/* HTML §2.5.6 "Nonce attributes" — see nonce_attribute.h for why this is a component and not a reflection row. */
#include <stdbool.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/ns/ns.h>

#include "check.h"
#include "quickjs.h"
#include "solver/attr_shadow.h"   /* the per-flow named-slot map [[CryptographicNonce]] lives in */
#include "solver/dom_cow.h"       /* and the write that captures it into the running flow's delta */
/* DOM §4.9 Interface Element's own key — the (null, `nonce`) attribute these steps are defined over. */
#include "core/dom/attr_list.h"
#include "core/dom/node.h"
#include "core/dom/element.h"
#include "core/idl_args.h"
#include "core/html/nonce_attribute.h"

/* §2.5.6's INTERNAL SLOT, under the standard's own name. It is a solver/attr_shadow.h PROPERTY slot, whose
   namespace is DOM property names (`value`, `textContent`) — a name in double square brackets cannot collide
   with one, and spelling it the way §2.5.6 spells it is what makes a reader who greps the standard land here. */
#define NONCE_SLOT "[[CryptographicNonce]]"

/* §2.5.6's `nonce` setter, declared once per AGENT — see nonce_attribute_init. */
static int g_id_set_nonce = -1;

/* WHICH ELEMENTS INCLUDE HTMLOrSVGOrMathMLElement, over the ELEMENT rather than over a wrapper — the attribute
   change steps and the cloning steps both stand where there is no JS value to ask. §4's element-interface table
   maps every HTML-namespace element to an interface inheriting HTMLElement (HTMLUnknownElement included), so
   the namespace is the whole test, exactly as core/html/html_element.c's html_element_is says for the wrapper
   side. SVG and MathML elements are NOT in it, and that is not a narrowing: SVGElement and MathMLElement are
   names on browser/platform_names.h — absent interfaces whose ReferenceError names the component to write — so
   in this agent the mixin has no other carrier for the member to be on either. */
static bool includes_mixin(const lxb_dom_node_t *n)
{
    return n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT && n->ns == LXB_NS_HTML;
}

/* THE SLOT, READ — AND THE CONTENT ATTRIBUTE WHERE NOTHING HAS WRITTEN ONE, WHICH IS NOT A DEFAULT THIS FILE
 * INVENTED BUT THE VALUE §2.5.6'S OWN STEPS PUT THERE IN A BROWSER.
 *
 * OWNED. A CONCOLIC COMES BACK AS ITSELF, which is the whole reason the slot holds a JSValue: `s.nonce =
 * location.hash.slice(1)` is a source stashed in element state, and §@S has to see it at the `<script>` the
 * flow then injects, not a stringified copy of its example. The attribute arm carries the same triple, out of
 * §@S's (element, name) shadow, for the same reason.
 *
 * AN ABSENT ENTRY IS THE POSITIVE STATEMENT "NOTHING HAS OVERRIDDEN THE ATTRIBUTE YET", and it used to answer
 * the empty string instead — which is the standard's sentence for an element that never had a nonce ("Unless
 * otherwise specified, the slot's value is the empty string") read as though it were the sentence for an
 * element whose markup plainly carries one. For that element the standard HAS specified otherwise, one
 * algorithm away: DOM §4.9 "Interface Element"'s append an attribute step 4 is "Handle attribute changes for
 * attribute with element, null, and attribute's value", whose own step 3 is "Run the attribute change steps
 * with element, attribute's local name, oldValue, newValue, and attribute's namespace" — and HTML §13.2.6.1
 * "Creating and inserting nodes"' create an element for a token step 11 is "Append each attribute in the given
 * token to element." So in a browser `<script nonce=abc>` reaches step 4 of the attribute change steps below
 * before the element is ever inserted, and the slot holds `abc`.
 *
 * IT REACHES NEITHER IN THIS ENGINE, AND THAT IS A FACT ABOUT THE PARSER RATHER THAN ABOUT THIS COMPONENT.
 * HTML tree construction writes its attributes through Lexbor's own primitives, which reach that document's
 * `lxb_dom_document_attr_mutation_cb_t` and never solver/dom_cow.h's mutation chokepoint — so
 * core/dom/element.c's element_attr_changed, where the steps below are registered beside every other
 * standard's, does not fire for one single attribute the parser sets. Every component that needs §4.9's steps
 * on the parsed tree has answered that for itself, and the two answers are not equally good: six of them run a
 * hand-written post-parse walk of their own, and a component added without one gets nothing with nothing to
 * say so. THIS is the other answer, and it is the one core/html/html_option.h already argues for `selected` —
 * the read consults the attribute, so the value is right for EVERY road at once (the parser's, HTML §13.4's
 * fragment parse, HTML §8.4.3 document.write()'s write, a clone) instead of for the list of roads somebody
 * remembered.
 *
 * AND IT STAYS RIGHT ONCE §2.5.6'S HIDING STEP EXISTS, which is the one algorithm that deliberately makes the
 * slot and the attribute disagree. Its steps are "Let nonce be element's [[CryptographicNonce]]", "Set an
 * attribute value for element using "nonce" and the empty string", and "Set element's [[CryptographicNonce]]
 * to nonce", under the note "If element's [[CryptographicNonce]] were not restored it would be the empty
 * string at this point." That restore is a WRITE, so after the hiding step the entry is PRESENT and this
 * function returns before it can read the blanked attribute. The note is the standard saying why: the blanking
 * runs the change steps, which is exactly the road that makes the entry present.
 *
 * READ AT DOM §4.9 Interface Element'S OWN KEY — (namespace, local name) — AND NEVER AT A QUALIFIED NAME.
 * What forces that is HTML §2.5.6 Nonce attributes' own step 2, "If localName is not nonce or namespace is not
 * null, then return": an attribute a page made with `setAttributeNS(ns, "nonce", v)` is one these steps refuse,
 * so it must not be read here either — and `getAttribute`, which resolves a QUALIFIED name, matches exactly
 * that attribute. §2.5.6's step 1 is asked for the same reason: an element outside HTMLOrSVGOrMathMLElement has
 * no slot for those steps to write, so its `nonce` content attribute is an ordinary attribute and not a nonce. */
static JSValue nonce_get_slot(JSContext *ctx, lxb_dom_element_t *el)
{
    int i = attr_shadow_find(el, ATTR_SLOT_PROPERTY, NULL, NONCE_SLOT);
    lxb_dom_attr_t *a;
    JSValue taint;
    const lxb_char_t *v;
    size_t vl = 0;

    if (i >= 0) return JS_DupValue(ctx, attr_shadow_opaque(i));
    if (!includes_mixin(lxb_dom_interface_node(el))) return JS_NewStringLen(ctx, "", 0);
    a = dom_attr_get_ns(el, NULL, "nonce");
    /* NOW the standard's own sentence applies, and it is the only place it does: an element with no
       null-namespace `nonce` attribute and nothing written into the slot is an element nothing has specified
       otherwise for. */
    if (a == NULL) return JS_NewStringLen(ctx, "", 0);
    taint = dom_cow_attr_taint_ns(el, NULL, "nonce");   /* BORROWED */
    if (!JS_IsUndefined(taint)) return JS_DupValue(ctx, taint);
    v = lxb_dom_attr_value(a, &vl);
    /* DOM §4.9 Interface Element: an attribute's value is a string, and `<script nonce>` with no `=` has the
       EMPTY one rather than none — which is what step 4 of the steps below would have copied, and is not
       step 3's null. */
    return JS_NewStringLen(ctx, v ? (const char *)v : "", v ? vl : 0);
}

/* THE SLOT, WRITTEN — through the COW write, so a nonce one arm of a fork assigned is that arm's alone and its
   sibling still reads the markup's. `v` is BORROWED; the shadow dups it. */
static void nonce_set_slot(JSContext *ctx, lxb_dom_element_t *el, JSValueConst v)
{
    /* solver/dom_cow.h gives JS_UNDEFINED the meaning "clear this entry", and §2.5.6 has no step that clears
       the slot — its two writes are "the empty string" and "value", and a cleared entry would read back as the
       empty string by a DIFFERENT route (the absent-entry default above) that a later restore cannot tell from
       a slot nobody ever wrote. Every caller here passes a string or a concolic, so this is an invariant of
       this file rather than a case to handle. */
    DCHECK(!JS_IsUndefined(v),
           "§2.5.6's [[CryptographicNonce]] was written with undefined — the section's only two writes are the "
           "empty string and the given value, and the shadow reads undefined as CLEAR, which is a third state "
           "the standard does not have");
    dom_cow_set_prop_taint(ctx, el, NONCE_SLOT, v);
}

/* WEB IDL §3.7.6's brand check, a THROW and not an assert: the member sits on HTMLElement.prototype and a page
   reaches an accessor off a prototype with `.call` on anything at all. */
static lxb_dom_element_t *nonce_receiver(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_node_t *n = node_of(this_val);

    if (includes_mixin(n)) return lxb_dom_interface_element(n);
    JS_ThrowTypeError(ctx, "HTMLElement.nonce was reached on something that is not an HTML element");
    return NULL;
}

/* §2.5.6: "The nonce IDL attribute must, on getting, return the value of this element's
   [[CryptographicNonce]]". The content attribute is not consulted — which is the whole difference from the
   reflection row this replaced, and the reason `<script nonce=abc>` still answers "abc" is the ATTRIBUTE CHANGE
   STEPS below, not this getter. */
static JSValue js_nonce_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el = nonce_receiver(ctx, this_val);

    (void)magic;
    if (!el) return JS_EXCEPTION;
    return nonce_get_slot(ctx, el);
}

/* §2.5.6: "and on setting, set this element's [[CryptographicNonce]] to the given value", plus the note that
   makes this member's setter unlike every reflection's — "Note how the setter for the nonce IDL attribute does
   not update the corresponding content attribute." Writing the attribute here would re-open the side channel
   the section exists to close: an attribute is readable by a CSS attribute selector and the slot is not, and
   §2.5.6 cites the issue where that reasoning was introduced. */
static JSValue js_nonce_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    lxb_dom_element_t *el = nonce_receiver(ctx, this_val);

    (void)magic;
    if (!el) return JS_EXCEPTION;
    nonce_set_slot(ctx, el, val);
    return JS_UNDEFINED;
}

JSValue nonce_attribute_current(JSContext *ctx, lxb_dom_element_t *el)
{
    DCHECK(el != NULL, "§2.5.6's [[CryptographicNonce]] was read off no element — a slot is an element's, and "
                       "a caller with no element has no nonce to ask about rather than an empty one");
    /* An element outside the mixin has no slot to hold one, so it reads back as the same empty string §2.5.6
       gives an element nothing has written. That is nonce_get_slot's own step 1 test and not a second one
       here: the mixin decides whether a `nonce` content attribute is a nonce at all, so the question has to be
       asked wherever the attribute is read, which is there. */
    return nonce_get_slot(ctx, el);
}

void nonce_attribute_attr_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local)
{
    JSValue wrap, value;

    /* Step 1: "If element does not include HTMLOrSVGOrMathMLElement, then return."
       Step 2: "If localName is not nonce or namespace is not null, then return." */
    if (!includes_mixin(lxb_dom_interface_node(el))) return;
    if (ns != NULL || !local || strcmp(local, "nonce") != 0) return;

    /* THE STEPS' `value` IS READ BACK OFF THE ATTRIBUTE RATHER THAN TAKEN AS BYTES, and that is not a detour.
       This seam hands its neighbours a `const char *`, and §2.5.6's value goes straight into a slot a source
       can be stashed in — so taking the bytes would ToString the provenance away at the one write that carries
       it into element state. The hook runs AFTER the write (core/dom/element.c says so at the seam), so the
       attribute now holds the change's own value, and DOM §4.9's "value is null" for a REMOVAL is exactly the
       JS_NULL element_attr_get_value answers with for an absent attribute. */
    wrap = element_wrap(ctx, el);
    if (!JS_IsObject(wrap)) { JS_FreeValue(ctx, wrap); return; }
    value = element_attr_get_value(ctx, wrap, "nonce");
    JS_FreeValue(ctx, wrap);
    if (JS_IsException(value)) { JS_FreeValue(ctx, value); return; }

    if (JS_IsNull(value)) {
        /* Step 3: "If value is null, then set element's [[CryptographicNonce]] to the empty string." */
        JSValue empty = JS_NewStringLen(ctx, "", 0);
        CHECK(!JS_IsException(empty), "§2.5.6's empty-string nonce could not be allocated");
        nonce_set_slot(ctx, el, empty);
        JS_FreeValue(ctx, empty);
    } else {
        /* Step 4: "Otherwise, set element's [[CryptographicNonce]] to value." */
        nonce_set_slot(ctx, el, value);
    }
    JS_FreeValue(ctx, value);
}

void nonce_attribute_cloned(JSContext *ctx, lxb_dom_node_t *src, lxb_dom_node_t *copy)
{
    JSValue v;

    if (!includes_mixin(src)) return;
    DCHECK(includes_mixin(copy),
           "DOM §4.4 clone a node produced a copy of an HTML element that is not one — §2.5.6's cloning steps "
           "are stated over a copy of the same element, so a pair that disagrees means step 2's clone a single "
           "node built the wrong interface");
    /* "Set copy's [[CryptographicNonce]] to node's [[CryptographicNonce]]." The empty string is the copy's
       initial value and there is no slot to clear, so an unwritten source has nothing to do — and the copy's
       OWN attribute change steps have already run for the attribute the clone carried across, which is exactly
       the answer this overwrites when the two have stopped agreeing. */
    if (attr_shadow_find(lxb_dom_interface_element(src), ATTR_SLOT_PROPERTY, NULL, NONCE_SLOT) < 0) return;
    v = nonce_get_slot(ctx, lxb_dom_interface_element(src));
    nonce_set_slot(ctx, lxb_dom_interface_element(copy), v);
    JS_FreeValue(ctx, v);
}

void nonce_attribute_init(JSContext *ctx)
{
    DCHECK(g_id_set_nonce < 0, "nonce_attribute_init ran twice in one runtime — the setter is declared once "
                               "per AGENT and installed once per realm");
    /* §2.5.6 declares `attribute DOMString nonce` on the HTMLOrSVGOrMathMLElement mixin, so the value crosses
       Web IDL §3.2.10 DOMString before the body sees it — and unknown external input crosses as ITSELF
       (idl_concolic_rule), which is what lets a stashed source reach the slot with its triple intact. */
    g_id_set_nonce = idl_setter_id(ctx, IDL_DOMSTRING, false, js_nonce_set, 0);
}

/* The agent-lifetime release. The only agent state here is the setter's POOL ID — an int, so there is no
   JSRuntime to hand anything back to — and it is RESET for the reason core/html/html_base_element.c resets its
   one: a second runtime in one process would install a member out of a pool that no longer holds it. */
void nonce_attribute_free(void)
{
    g_id_set_nonce = -1;
}

void nonce_attribute_install(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_id_set_nonce >= 0,
           "§2.5.6's `nonce` was installed before nonce_attribute_init declared its setter");
    idl_install_accessor(ctx, proto, "nonce", js_nonce_get, 0, g_id_set_nonce);
}

/* NAMED RESIDUAL — HTML §2.5.6 Nonce attributes' LAST ALGORITHM IS NOT HERE, and the reason is a fact about
 * the CSP list rather than about this component.
 *
 * WHAT IS NOT COVERED: "Whenever an element including HTMLOrSVGOrMathMLElement becomes browsing-context
 * connected, the user agent must execute the following steps on the element: Let CSP list be element's
 * shadow-including root's policy container's CSP list. If CSP list contains a header-delivered Content Security
 * Policy, and element has a nonce content attribute whose value is not the empty string: Let nonce be element's
 * [[CryptographicNonce]]. Set an attribute value for element using `nonce` and the empty string. Set element's
 * [[CryptographicNonce]] to nonce." Everything else in §2.5.6 IS built: the slot, both directions of the IDL
 * attribute, the attribute change steps and the cloning steps. What this leaves out is the HIDING — the
 * attribute keeps the nonce the markup wrote instead of being blanked once the element connects.
 *
 * WHY IT IS A RESIDUAL AND NOT A CRASH: the condition it turns on is UNASKABLE HERE, not unimplemented. It is
 * "CSP list contains a HEADER-DELIVERED Content Security Policy", and core/frame/csp_directive_list.h's
 * CspPolicy carries no source — that header states it in its own words ("A policy also has a disposition and a
 * source; neither is here … the source that would distinguish the two is a field that cannot survive §7.4's
 * clone, which travels as TEXT, so it arrives with that transport rather than before it"), and
 * core/dom/document.c's merge of the `<meta>` policies into the header list is where the two become one string.
 * Asserting on a document that has any CSP at all would fire for a meta-only policy, which is a document where
 * §2.5.6 blanks NOTHING — a crash on the case the engine handles correctly.
 *
 * WHAT THE NEXT DIFF BUILDS: a `source` on CspPolicy, set by core/frame/policy_container.c's parse for the
 * policies that arrived in the response's header and by core/dom/document.c's meta merge for the ones that did
 * not, and a `csp_list_has_header_delivered` question over it. THESE STEPS ARE THE ONLY CALLER THAT NEEDS IT
 * TODAY; the sandboxing-flags DCHECK in document.c is the other site the field would change, and it is written
 * as an over-the-whole-list check precisely because the field is missing.
 *
 * HOW ITS ABSENCE WOULD SHOW: on a document served with a `Content-Security-Policy` header,
 * `document.querySelector("script[nonce]").getAttribute("nonce")` answers the nonce where a browser answers
 * the empty string, and a CSS attribute selector `script[nonce="…"]` matches where a browser's does not — while
 * `.nonce` itself is correct either way, because the slot holds the value in both worlds. It is exactly the
 * exfiltration channel §2.5.6 names, still open by one attribute, on header-CSP documents only. */
