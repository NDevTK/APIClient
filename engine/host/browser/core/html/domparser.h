/* HTML §8.5.1's `DOMParser` — the interface that turns a STRING into a Document. See domparser.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_DOMPARSER_H
#define ENGINE_HOST_BROWSER_CORE_HTML_DOMPARSER_H
#include "quickjs.h"

/* The AGENT's half: the class, the constructor and `parseFromString`, declared once. */
void domparser_init(JSContext *ctx);
/* §3.7's prototype AND §3.8's global property reference for ONE realm — declared into core/realm.h's list by
   domparser_init. ONE entry because §3.8 `define the global property references` is given a REALM and names no
   Document; the interface object used to be a second, per-document entry, so a realm that is not a Window's
   carried the prototype and no `DOMParser` property naming it. */
void domparser_install_realm(JSContext *ctx);
/* The AGENT's half, undone — core/platform.h's release column. */
void domparser_free(void);

#endif
