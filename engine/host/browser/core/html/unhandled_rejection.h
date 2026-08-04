/* UNHANDLED PROMISE REJECTIONS — HTML §8.1.7.5: the two lists, and what is still unhandled at a checkpoint. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_UNHANDLED_REJECTION_H
#define ENGINE_HOST_BROWSER_CORE_HTML_UNHANDLED_REJECTION_H
#include "quickjs.h"

/* Install the runtime's rejection tracker and build the baseline list. */
void unhandled_rejection_init(JSContext *ctx);
void unhandled_rejection_free(JSContext *ctx);

/* §8.1.7.5 "notify about rejected promises": TAKE the reasons of every rejection that is still unhandled and
   clear the list. Returns an owned Array — empty when there is nothing to notify about. Taking rather than
   reading is what makes a second checkpoint report nothing twice. */
JSValue unhandled_rejection_take(JSContext *ctx);

#endif
