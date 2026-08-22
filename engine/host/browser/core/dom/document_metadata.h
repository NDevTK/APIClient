/* HTML §3.1.4 Resource metadata management and §3.1.5 Reporting document loading status — the Document members
   that are facts about the RESOURCE this document came from and about how far its load got, rather than about
   its tree. See document_metadata.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_METADATA_H
#define ENGINE_HOST_BROWSER_CORE_DOM_DOCUMENT_METADATA_H

#include "quickjs.h"

/* The agent's half: the member declarations and this component's per-realm cookie store, declared once. */
void document_metadata_init(JSContext *ctx);
/* §3.7.6's REGULAR ATTRIBUTES, on Document.prototype — installed by whoever owns the interface object. */
void document_metadata_install(JSContext *ctx, JSValueConst proto);
/* Reached from document_agent_free — §3.1.4 is declared by document_init, so it is released by its declarer. */
void document_metadata_free(void);

#endif
