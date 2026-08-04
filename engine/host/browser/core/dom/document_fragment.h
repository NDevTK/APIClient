/* THE DocumentFragment INTERFACE — DOM §4.7. See document_fragment.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_FRAGMENT_H
#define ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_FRAGMENT_H
#include "quickjs.h"

void document_fragment_init(JSContext *ctx);
void document_fragment_install(JSContext *ctx, JSValueConst global);
void document_fragment_free(JSContext *ctx);
/* DocumentFragment.prototype, borrowed — what template.content's wrapper wears. */
JSValueConst document_fragment_proto(void);

#endif
