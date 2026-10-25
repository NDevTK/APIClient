/* The person scrolls a target into view — a user act this agent models, performed as CSSOM VIEW §6.1 "Element
   Scrolling Members"' scroll a target into view. See user_scroll.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_USER_SCROLL_H
#define ENGINE_HOST_BROWSER_CORE_DOM_USER_SCROLL_H

#include <lexbor/dom/dom.h>

#include "quickjs.h"

/* A flow base whose one frame is the task the person's scroll is: scroll a target into view over `target`
   (which has an associated box, asserted), with block and inline "nearest" and behavior "auto", run one
   ancestor-walk unit per step. Minted in the target document's realm. Owned by the caller's flow_install_base. */
JSValue *user_scroll_flow(JSContext *ctx, lxb_dom_element_t *target);

#endif
