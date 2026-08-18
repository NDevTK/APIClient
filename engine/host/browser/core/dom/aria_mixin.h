/* ARIAMixin — WAI-ARIA 1.3 §"reflection", `Element includes ARIAMixin`.
 *
 * The mixin is FIFTY-TWO members and every one of them is HTML §2.6.1 REFLECTION over a content attribute, so
 * this is one component and not fifty-two: the pair of names (the IDL identifier, the content attribute) is the
 * whole of what each member is, and the behaviour is §2.6.1's processing model for the member's IDL TYPE.
 *
 * TWO TYPES, TWO MECHANISMS, and conflating them is the mistake this header exists to prevent:
 *   - FORTY-FOUR are `DOMString?`. They are ordinary content-attribute reflections and go through the registry
 *     core/dom/element.c already owns, under REFLECT_STRING_NULLABLE — the `?` is not decoration: §2.6.1's
 *     `DOMString` getter reads an absent attribute as "" and its `DOMString?` getter reads it as NULL, and the
 *     setter DELETES the attribute for null rather than writing the four characters "null".
 *   - EIGHT are ELEMENT REFLECTION — one `Element?` and seven `FrozenArray<Element>?`. They are NOT string
 *     reflections with a lookup bolted on: §2.6.1 gives each reflected target an EXPLICITLY SET ATTR-ELEMENT(S)
 *     that a page assigns DIRECTLY (`el.ariaLabelledByElements = [x]` names an element that need not have an
 *     id at all, and the content attribute becomes the empty string), a resolution from the content attribute's
 *     ids that only runs when nothing was explicitly set, a DESCENDANT-OF-A-SHADOW-INCLUDING-ANCESTOR filter on
 *     what was, a cache that keeps `el.ariaLabelledByElements === el.ariaLabelledByElements` true, and
 *     ATTRIBUTE CHANGE STEPS that drop the explicit value when the content attribute is written by any other
 *     spelling. A `FrozenArray<Element>?` member answered as a string is a WRONG member, not a partial one.
 *
 * ElementInternals ALSO includes this mixin and holds its own copy of the 44-name list over §4.13.7.4's
 * internal content attribute map (which is a map and not the element's attributes, so its accessors are its
 * own). ARIA_STRING_MEMBERS is stated here so that list can be ONE list; core/html/element_internals.c is
 * another lane's file and still carries a private copy of it. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_ARIA_MIXIN_H
#define ENGINE_HOST_BROWSER_CORE_DOM_ARIA_MIXIN_H
#include <lexbor/dom/dom.h>
#include "quickjs.h"

/* THE 44 `DOMString?` MEMBERS: X(idl identifier, content attribute). The order is wai-aria.idl's own. */
#define ARIA_STRING_MEMBERS(X) \
    X("role",                        "role") \
    X("ariaAtomic",                  "aria-atomic") \
    X("ariaAutoComplete",            "aria-autocomplete") \
    X("ariaBrailleLabel",            "aria-braillelabel") \
    X("ariaBrailleRoleDescription",  "aria-brailleroledescription") \
    X("ariaBusy",                    "aria-busy") \
    X("ariaChecked",                 "aria-checked") \
    X("ariaColCount",                "aria-colcount") \
    X("ariaColIndex",                "aria-colindex") \
    X("ariaColIndexText",            "aria-colindextext") \
    X("ariaColSpan",                 "aria-colspan") \
    X("ariaCurrent",                 "aria-current") \
    X("ariaDescription",             "aria-description") \
    X("ariaDisabled",                "aria-disabled") \
    X("ariaExpanded",                "aria-expanded") \
    X("ariaHasPopup",                "aria-haspopup") \
    X("ariaHidden",                  "aria-hidden") \
    X("ariaInvalid",                 "aria-invalid") \
    X("ariaKeyShortcuts",            "aria-keyshortcuts") \
    X("ariaLabel",                   "aria-label") \
    X("ariaLevel",                   "aria-level") \
    X("ariaLive",                    "aria-live") \
    X("ariaModal",                   "aria-modal") \
    X("ariaMultiLine",               "aria-multiline") \
    X("ariaMultiSelectable",         "aria-multiselectable") \
    X("ariaOrientation",             "aria-orientation") \
    X("ariaPlaceholder",             "aria-placeholder") \
    X("ariaPosInSet",                "aria-posinset") \
    X("ariaPressed",                 "aria-pressed") \
    X("ariaReadOnly",                "aria-readonly") \
    X("ariaRelevant",                "aria-relevant") \
    X("ariaRequired",                "aria-required") \
    X("ariaRoleDescription",         "aria-roledescription") \
    X("ariaRowCount",                "aria-rowcount") \
    X("ariaRowIndex",                "aria-rowindex") \
    X("ariaRowIndexText",            "aria-rowindextext") \
    X("ariaRowSpan",                 "aria-rowspan") \
    X("ariaSelected",                "aria-selected") \
    X("ariaSetSize",                 "aria-setsize") \
    X("ariaSort",                    "aria-sort") \
    X("ariaValueMax",                "aria-valuemax") \
    X("ariaValueMin",                "aria-valuemin") \
    X("ariaValueNow",                "aria-valuenow") \
    X("ariaValueText",               "aria-valuetext")

/* THE 8 ELEMENT-REFLECTING MEMBERS: X(idl identifier, content attribute, plural).
   `plural` is 1 for the seven `FrozenArray<Element>?` members and 0 for the one `Element?` — which is the whole
   difference between them: the singular resolves the attribute's WHOLE value as one id, the plural splits it on
   ASCII whitespace and resolves each token. */
#define ARIA_ELEMENT_MEMBERS(X) \
    X("ariaActiveDescendantElement", "aria-activedescendant", 0) \
    X("ariaControlsElements",        "aria-controls",         1) \
    X("ariaDescribedByElements",     "aria-describedby",      1) \
    X("ariaDetailsElements",         "aria-details",          1) \
    X("ariaErrorMessageElements",    "aria-errormessage",     1) \
    X("ariaFlowToElements",          "aria-flowto",           1) \
    X("ariaLabelledByElements",      "aria-labelledby",       1) \
    X("ariaOwnsElements",            "aria-owns",             1)

/* DECLARED ONCE PER AGENT, beside Element's own reflections — the string table goes into the same registry. */
void aria_mixin_init(JSContext *ctx);
/* INSTALLED PER REALM onto the prototype of an interface that INCLUDES the mixin. */
void aria_mixin_install(JSContext *ctx, JSValueConst proto);
/* §2.6.1's ATTRIBUTE CHANGE STEPS for the eight element-reflecting members: writing the content attribute by
   any other spelling (setAttribute, the parser, removeAttribute) drops the explicitly set attr-element(s), so
   the next read resolves the ids the attribute now names. Called from §4.9's one attribute-changed hook. */
void aria_mixin_attribute_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local);
void aria_mixin_free(JSRuntime *rt);

#endif
