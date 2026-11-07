/* Web IDL §3.9.7 Abstract operations: the named property visibility algorithm's prototype walk — see idl_named.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_IDL_NAMED_H
#define ENGINE_HOST_BROWSER_CORE_IDL_NAMED_H
#include <stdbool.h>
#include "quickjs.h"

/* Steps 4-5 of the named property visibility algorithm for a legacy platform object `obj` whose interface has no
   [LegacyOverrideBuiltIns]: true when a prototype has an own property `prop`, which hides the supported property
   name. Runs no page code. */
bool idl_named_hidden_by_prototype(JSContext *ctx, JSValueConst obj, JSAtom prop);

#endif
