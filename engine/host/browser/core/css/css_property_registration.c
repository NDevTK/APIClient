/* CSS Properties and Values API 1 §2.1 "Determining the Registration" — see css_property_registration.h. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/css/css_property_registration.h"
#include "core/css/css_rule.h"
#include "core/css/css_style_sheet.h"
#include "core/css/style_sheet_list.h"
#include "core/dom/document.h"

/* §2.1 asks the Document's [[registeredPropertySet]] first, which only `CSS.registerProperty` (§4.1) writes;
   that member is not built, so the set is empty and the `@property` rules decide.
   NAMED RESIDUAL: §2.8 "Shadow DOM" puts every shadow tree's `@property` rules into the same document-wide
   map, and only the document's own sheets are read. The next diff walks each shadow root's sheets as well.
   Its absence shows as a property registered inside a shadow tree reading as unregistered outside it. */
bool css_property_registration_of(lxb_dom_element_t *el, const char *name, CssPropertyRegistration *out)
{
    lxb_dom_node_t *doc;
    JSContext *ctx;
    JSValue sheets;
    uint32_t ns = 0, si;
    bool found = false;

    DCHECK(el != NULL && name != NULL && name[0] == '-' && name[1] == '-',
           "a registration was asked for with no element or for a name that is not a custom property's");
    doc = lxb_dom_interface_node(lxb_dom_interface_node(el)->owner_document);
    DCHECK(doc != NULL, "an element being styled has no node document");
    ctx = document_realm_of(doc);
    DCHECK(ctx != NULL, "a document being styled has no realm record, so its sheet list is nowhere");
    sheets = style_sheet_list_of(ctx, doc);
    if (JS_IsArray(sheets)) {
        JSValue len = JS_GetPropertyStr(ctx, sheets, "length");

        JS_ToUint32(ctx, &ns, len);
        JS_FreeValue(ctx, len);
    }
    for (si = 0; si < ns; si++) {
        JSValue sheet = JS_GetPropertyUint32(ctx, sheets, si);

        DCHECK(css_style_sheet_is(sheet), "CSSOM §6.2's sheet list holds something that is not a CSS style sheet");
        /* The sheets that apply, gated as the author origin gates them (core/css/css_author_sheets.c). */
        if (!css_style_sheet_disabled(sheet) && css_style_sheet_media_matches(ctx, sheet)) {
            JSValue rules = css_style_sheet_rules(ctx, sheet);

            css_rule_property_registration(ctx, rules, name, out, &found);
            JS_FreeValue(ctx, rules);
        }
        JS_FreeValue(ctx, sheet);
    }
    JS_FreeValue(ctx, sheets);
    return found;
}

void css_property_registration_free(CssPropertyRegistration *r)
{
    DCHECK(r != NULL, "a registration was freed through nothing");
    free(r->syntax);
    free(r->initial_value);
    r->syntax = NULL;
    r->initial_value = NULL;
}
