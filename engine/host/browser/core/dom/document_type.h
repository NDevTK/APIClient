/* THE DocumentType INTERFACE — DOM §4.6. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_TYPE_H
#define ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_TYPE_H
#include "quickjs.h"

/* THE AGENT'S HALF: the class, the node-type claim, and the per-realm prototype declaration. */
void document_type_init(JSContext *ctx);
/* §4.6's interface prototype object for ONE realm — declared into core/realm.h's list. */
void document_type_install_proto(JSContext *ctx);
/* The interface OBJECT on a realm's global, so `doctype instanceof DocumentType` holds. */
void document_type_install(JSContext *ctx, JSValueConst global);
/* Reached from document_agent_free — §4.6 is declared by document_init, so it is released by its declarer. */
void document_type_free(void);

#endif
