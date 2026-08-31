/* HTML §8.1.8.1 "Event handlers" — the ATTRIBUTE CHANGE STEPS. See event_handler_attribute.h for why this half
 * of §8.1.8.1 is a file of its own and the other half is in core/events/event_target.c. */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>   /* §8.1.8.2's third table is body and frameset ELEMENTS — the tag test */

#include "check.h"
#include "quickjs.h"
#include "core/dom/attr_list.h"
#include "core/dom/document.h"
#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"
#include "core/events/event_target.h"
#include "core/frame/csp_directive_list.h"
#include "core/frame/policy_container.h"
#include "core/html/event_handler_attribute.h"

/* §8.1.8.2's THIRD table is "exposed on all body and frameset elements", so the membership question needs the
   element's TYPE. It is the same test core/html/html_element.c makes for §8.1.8.1's determine the target of an
   event handler step 1, and it is made twice on purpose rather than shared: this one decides whether the name
   is a CONTENT ATTRIBUTE at all (step 1 of the attribute change steps) and that one decides which OBJECT the
   handler acts upon (step 1 of the determination), and the day a standard adds a third element to either
   table only one of the two moves. */
static bool eha_is_body_or_frameset(const lxb_dom_node_t *n)
{
    return n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT &&
           (lxb_html_tree_node_is((lxb_dom_node_t *)n, LXB_TAG_BODY) ||
            lxb_html_tree_node_is((lxb_dom_node_t *)n, LXB_TAG_FRAMESET));
}

/* §8.1.8.1's ATTRIBUTE CHANGE STEPS 2-5, once step 1 has resolved the name to a row. Both entry points below
   reach it, and that is the whole reason it is one function: the DOM chokepoint arrives with a NUL-terminated
   local name and the parsed walk arrives with a (pointer, length) out of lexbor's attribute list, so step 1 is
   spelled twice and everything after it exactly once. */
static void eha_apply(JSContext *ctx, lxb_dom_element_t *el, lxb_dom_node_t *n, int index,
                      const char *value, size_t value_len)
{
    JSValue exposed, target;

    /* STEP 2 — "let eventTarget be the result of determining the target of an event handler given element and
       localName". THE ELEMENT, not the running realm's anything: `<body onload=…>` acts upon that body's own
       Document's Window, which for an element reached across a same-origin frame boundary is not this one. */
    exposed = element_wrap(ctx, el);
    DCHECK(JS_IsObject(exposed),
           "§8.1.8.1's attribute change steps could not reach the element's own wrapper — step 2 determines "
           "the target OF AN OBJECT, and an element whose wrapper does not exist is one this realm has never "
           "handed the page, which is not a state DOM §4.9's chokepoint can be entered in");
    target = event_target_determine_handler_target(ctx, exposed, index);
    JS_FreeValue(ctx, exposed);

    /* STEP 3 — "if eventTarget is null, then return". Null is the determination's own third outcome, not an
       error: a body element in a `DOMParser` document or an XHR `responseXML` has no Window to act upon, and a
       browser drops the handler silently exactly here. */
    if (JS_IsNull(target))
        return;

    if (value == NULL) {
        /* STEP 4 — "if value is null, then deactivate an event handler given eventTarget and localName". This
           is a REMOVED attribute, which DOM §4.9's "remove an attribute" reaches with a null new value; it is
           not an EMPTY one. `<div onclick="">` is a handler whose body is the empty FunctionBody, which
           compiles and does nothing, and which a browser reports as a function from `div.onclick`. */
        event_target_deactivate_handler(ctx, target, index);
    } else {
        /* STEP 5.1 — "if the Should element's inline behavior be blocked by Content Security Policy? algorithm
           returns Blocked when executed upon element, `script attribute`, and value, then return". CSP §4.2.3
           with type "script attribute", which §6.8.2 maps to `script-src-attr` and §6.8.3 falls back through
           `script-src` to `default-src`, so `script-src 'self'` kills every inline handler on the page while
           leaving its `<script src>` alone — which is exactly the state §@S(a) requires a PoC to be judged
           against rather than assumed away.
           THE POLICY IS THE ELEMENT'S NODE DOCUMENT'S and not the running realm's, for step 2's reason: CSP
           states every one of these questions over a document's global object's csp list, and an attribute set
           on an element of another same-origin document is that document's question.
           RETURNING HERE LEAVES THE HANDLER AS IT WAS, which is what the step says and is not the same as
           deactivating: a blocked write does not remove a handler an earlier allowed write installed. */
        if (!policy_allows_inline(document_policy_of(n->owner_document), CSP_INLINE_SCRIPT_ATTRIBUTE, el,
                                  value, value_len)) {
            JS_FreeValue(ctx, target);
            return;
        }
        /* STEPS 5.2, 5.3, 5.5 AND 5.6 — the handler map, its entry, the internal raw uncompiled handler, and
           activate an event handler. Step 5.4's LOCATION is the one item of §8.1.8.1's tuple that is not
           carried, and core/events/event_target.c's uncompiled_new says why at the mint: it is "the script
           location that TRIGGERED the execution of these steps", which is the address and line of whatever ran
           the setAttribute rather than anything this element knows, and its only consumer is a syntax error
           reported by a compile that does not exist yet. */
        event_target_set_uncompiled_handler(ctx, target, index, value, value_len);
    }
    JS_FreeValue(ctx, target);
}

void event_handler_attribute_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local,
                                     const char *value, size_t value_len)
{
    lxb_dom_node_t *n;
    int index;

    DCHECK(el != NULL, "§8.1.8.1's attribute change steps were run for no element — every one of DOM §4.9's "
                       "attribute change steps is given the element the attribute is on");
    DCHECK(local != NULL, "§8.1.8.1's attribute change steps were run with no attribute name — step 1 tests it "
                          "against the event handler content attribute set and there is no name in a null one");

    /* STEP 1, FIRST HALF — "if namespace is not null … then return". A content attribute is the null
       namespace's; a `foo:onclick` set through setAttributeNS is a different attribute entirely, and the same
       test core/html/html_link.c's attribute change steps make. */
    if (ns && *ns)
        return;
    n = lxb_dom_interface_node(el);
    /* STEP 1, SECOND HALF — "or localName is not the name of an event handler content attribute ON ELEMENT".
       BOTH halves of that clause, and the second is not the first: `onmessage` is the name of an event handler
       IDL attribute (§9.4.2's, on MessagePort) and is a content attribute on nothing, and `onunload` is a
       content attribute on a body and a frameset and on no other element — so `<div onunload="x">` is an
       ordinary attribute in every browser. A test that asked only whether the name is in the list would have
       registered a marker listener on the div for a type nothing dispatches at it.
       IT IS ASKED OF EVERY ELEMENT AND NOT ONLY OF HTML ONES, because SVG2 §15.9 "Event attributes" gives SVG
       elements the same attributes — its own example is `<rect onclick="MyClickHandler(evt)" .../>` and its
       text is "the character data content of an event attribute becomes the definition of the ECMAScript
       function which gets invoked in response to the event". `<svg onload=…>` is also the auto-firing position
       solver/solve_html.c's constructed breakouts end in, so an element-namespace filter here would make the
       one markup class the solver derives for unreachable in the engine that has to run it. */
    index = event_target_handler_attribute_index(local, strlen(local));
    if (index < 0 || !event_target_handler_attribute_on_element(index, eha_is_body_or_frameset(n)))
        return;
    eha_apply(ctx, el, n, index, value, value_len);
}

void event_handler_attribute_parsed(JSContext *ctx, lxb_dom_node_t *root)
{
    lxb_dom_node_t *n;

    DCHECK(root != NULL, "§8.1.8.1's parsed walk was given no tree to walk");
    for (n = root; n; n = shadow_root_next_in_shadow_including(ctx, n, root)) {
        lxb_dom_element_t *el;
        lxb_dom_attr_t *a;
        bool body_or_frameset;

        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT)
            continue;
        el = lxb_dom_interface_element(n);
        body_or_frameset = eha_is_body_or_frameset(n);
        /* THE ELEMENT'S OWN ATTRIBUTE LIST, not §8.1.8.2's list of names. Both directions answer the same
           question and they cost differently by two orders of magnitude: an element carries a handful of
           attributes and §8.1.8.2's tables carry over a hundred names, so asking the list of names would put a
           hundred-odd lookups on every node of every parsed tree to find, almost always, nothing. */
        for (a = lxb_dom_element_first_attribute(el); a; a = lxb_dom_element_next_attribute(a)) {
            size_t llen = 0, nlen = 0, vlen = 0;
            const lxb_char_t *l, *v;
            int index;

            /* STEP 1's namespace half, asked of the attribute this walk is standing on rather than of a name
               the caller passed: `dom_attr_ns` answers NULL for the null namespace and the interned namespace
               name otherwise, which is the same operand core/dom/attr_list.c's own lookups compare. */
            if (dom_attr_ns(a, &nlen) != NULL)
                continue;
            l = lxb_dom_attr_local_name(a, &llen);
            if (l == NULL)
                continue;
            index = event_target_handler_attribute_index((const char *)l, llen);
            if (index < 0 || !event_target_handler_attribute_on_element(index, body_or_frameset))
                continue;
            /* THE VALUE IS NEVER NULL ON THIS PATH, so step 4 is unreachable from here and the DCHECK says so
               rather than the walk silently deactivating: an attribute that is ON the element has a value —
               `<div onclick>` is the EMPTY string, which is a handler and not a removal — and a walk that
               handed NULL down would deactivate every handler it was meant to install. */
            v = lxb_dom_attr_value(a, &vlen);
            DCHECK(v != NULL,
                   "a parsed element carries an event handler content attribute with no value at all — DOM "
                   "§4.9's attributes hold a string, `<div onclick>` holds the EMPTY one, and a null here "
                   "would take §8.1.8.1's attribute change steps step 4 and DEACTIVATE the handler this walk "
                   "exists to install");
            eha_apply(ctx, el, n, index, (const char *)v, vlen);
        }
    }
}
