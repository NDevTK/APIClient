/* CSS Properties and Values API 1 §2.1 "Determining the Registration" — see css_property_registration.h. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/css/css_author_sheets.h"
#include "core/css/css_property_registration.h"
#include "core/css/css_style_declaration.h"   /* the document's sheet set, held by the layout job's record */

/* §2.1: "If the Document’s [[registeredPropertySet]] slot contains a record with the custom property’s name,
   the registration is that record. Otherwise, if the Document’s active stylesheets contain at least one valid
   @property rule representing a registration with the custom property’s name, the last such one in document
   order is the registration."
   Named residual. Not covered: the [[registeredPropertySet]] half, which only §4.1's CSS.registerProperty()
   writes and which this engine does not install, so the `@property` half answers alone; and §2.8 "Shadow DOM",
   which puts every shadow tree's `@property` rules into the same document-wide map, where only the document's
   own sheets are read. The next diff builds registerProperty's set, asked here before the sheets, then the
   shadow roots' sets. Its absence shows as a script-registered or shadow-tree property reading as unregistered
   or as its sheet registration. */
bool css_property_registration_of(lxb_dom_element_t *el, const char *name, CssPropertyRegistration *out)
{
    lxb_dom_node_t *doc;

    DCHECK(el != NULL && name != NULL && name[0] == '-' && name[1] == '-',
           "a registration was asked for with no element or for a name that is not a custom property's");
    doc = lxb_dom_interface_node(lxb_dom_interface_node(el)->owner_document);
    DCHECK(doc != NULL, "an element being styled has no node document");
    return css_author_sheets_registration(cssom_author_sheets(doc), name, out);
}

void css_property_registration_entry_free(CssPropertyRegistrationEntry *e)
{
    DCHECK(e != NULL, "a registration entry was freed through nothing");
    free(e->name);
    e->name = NULL;
    css_property_registration_free(&e->reg);
}

void css_property_registration_free(CssPropertyRegistration *r)
{
    DCHECK(r != NULL, "a registration was freed through nothing");
    free(r->syntax);
    free(r->initial_value);
    r->syntax = NULL;
    r->initial_value = NULL;
}
