/* InputDeviceCapabilities — Input Device Capabilities, §"The InputDeviceCapabilities interface".
   See input_device_capabilities.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_EVENTS_INPUT_DEVICE_CAPABILITIES_H
#define ENGINE_HOST_BROWSER_CORE_EVENTS_INPUT_DEVICE_CAPABILITIES_H
#include <stdbool.h>

#include "quickjs.h"

void input_device_capabilities_init(JSContext *ctx);            /* the class + the IDL declarations (agent) */
void input_device_capabilities_install_protos(JSContext *ctx);  /* §3.7: one prototype AND one interface object
                                                                   per REALM */
/* Released through core/platform.c's ONE release column rather than from a host's own list — a
   JSRuntime and not a JSContext because that column is run once the contexts are gone. */
void input_device_capabilities_free(JSRuntime *rt);

/* THE CLASS AN `InputDeviceCapabilities?` POSITION BRANDS AGAINST — Web IDL §3.2.15 Interface types under
 * §3.2.20 Nullable types, which is the type of §"Extensions to the UIEvent interface and UIEventInit
 * dictionary"'s one member. It is public because the BRAND is stated at the DECLARATION (idl_iface_brand) and
 * the declarations are UIEventInit's and those of every dictionary that inherits it, none of which live here.
 *
 * WHICH IS ALSO WHY THIS INTERFACE IS DECLARED BEFORE UIEvent AND ITS SUBCLASSES. `idl_iface_brand` names a
 * class at the moment the dictionary is declared, so a class id that does not exist yet is a zero — which the
 * conversion asserts on rather than silently passing, but the assert is the wrong place to learn it. The order
 * is core/platform.c's row list, and this row sits ahead of `event`. */
JSClassID input_device_capabilities_class(void);

/* §"Extensions to the UIEvent interface and UIEventInit dictionary"'s `InputDeviceCapabilities?` AS A VALUE
 * READ BACK OFF A CONVERTED DICTIONARY. The declaration has already branded the member, so this runs none of
 * the page's code and cannot throw; what it adds is the ONE thing the declaration cannot state — that DOM §2.5
 * Constructing events' create-an-event reaches ui_event_new with NO dictionary at all, and an absent member
 * is then the IDL's `= null`, which the spec states as a POSITIVE fact ("null if no input device was
 * responsible") rather than as an unknown. Answers JS_NULL or an OWNED dup of the instance. */
JSValue input_device_capabilities_of_dict(JSContext *ctx, JSValueConst init, const char *member);

#endif
