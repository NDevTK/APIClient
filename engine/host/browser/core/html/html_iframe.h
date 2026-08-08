/* HTMLIFrameElement's navigable — HTML §4.8.5. See html_iframe.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_HTML_IFRAME_H
#define ENGINE_HOST_BROWSER_CORE_HTML_HTML_IFRAME_H

#include "quickjs.h"

void iframe_init(JSContext *ctx);
/* Install §4.8.5's `contentWindow` on HTMLIFrameElement's prototype. */
void iframe_install(JSContext *ctx, JSValueConst proto);
/* Does this iframe have a navigable IN THE RUNNING FLOW? Kept on the wrapper, so the heap COW delta isolates
   it: a sibling that never inserted the frame has none. */
bool iframe_has_navigable(JSContext *ctx, JSValueConst wrapper);
/* QUEUE §4.8.5's create-a-child-navigable. Called from the insertion-steps walk, which may not suspend —
   creating the navigable asks the host and therefore must, so it becomes a task and runs as its own flow. */
void iframe_queue_create(JSContext *ctx, JSValueConst wrapper);
void iframe_free(JSContext *ctx);

#endif
