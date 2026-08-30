/* BarProp — HTML §7.2.2.5 "Historical browser interface element APIs". See bar_prop.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_BAR_PROP_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_BAR_PROP_H

#include "quickjs.h"

void bar_prop_init(JSContext *ctx);
/* Install §7.2.2.5's six bars — locationbar, menubar, personalbar, scrollbars, statusbar, toolbar. Six
   DISTINCT objects: `window.locationbar !== window.menubar` is what they are, and what the spec tests assert. */
void bar_prop_install(JSContext *ctx, JSValueConst global);
/* THE AGENT'S HALF, UNDONE — §7.2.2.5's class id, given back against the runtime it was registered in. There
   is no row of its own on core/platform.c's list for this: a sub-component is released by the row that reaches
   it, which here is `window`, and that is also the name its slots are declared under (core/agent_state.h). */
void bar_prop_free(JSRuntime *rt);

#endif
