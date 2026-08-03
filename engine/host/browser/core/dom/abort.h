/* AbortController / AbortSignal — DOM §3.2. See abort.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_ABORT_H
#define ENGINE_HOST_BROWSER_CORE_DOM_ABORT_H
#include "quickjs.h"

void abort_init(JSContext *ctx);                                 /* the private slot key (install time) */
void abort_install(JSContext *ctx, JSValueConst global);         /* AbortController + AbortSignal */
void abort_free(JSContext *ctx);                                 /* release the slot key this component owns */

#endif
