/* THE DocumentFragment INTERFACE — DOM §4.7. See document_fragment.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_FRAGMENT_H
#define ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_FRAGMENT_H
#include "quickjs.h"

void document_fragment_init(JSContext *ctx);
void document_fragment_install(JSContext *ctx, JSValueConst global);
/* Reached from document_agent_free — §4.7 is declared by document_init, so it is released by its declarer. */
void document_fragment_free(void);
/* DocumentFragment.prototype, borrowed — what template.content's wrapper wears. */
/* §4.7's prototype for ONE realm — declared into core/realm.h's list. */
void document_fragment_install_proto(JSContext *ctx);
/* PER REALM. OWNED: the caller frees. */
JSValue document_fragment_proto(JSContext *ctx);

#endif
