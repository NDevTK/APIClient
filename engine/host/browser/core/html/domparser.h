/* HTML §8.5.1's `DOMParser` — the interface that turns a STRING into a Document. See domparser.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_DOMPARSER_H
#define ENGINE_HOST_BROWSER_CORE_HTML_DOMPARSER_H
#include "quickjs.h"

/* The AGENT's half: the class, the constructor and `parseFromString`, declared once. */
void domparser_init(JSContext *ctx);
/* §3.7's prototype for ONE realm — declared into core/realm.h's list by domparser_init. */
void domparser_install_proto(JSContext *ctx);
/* §3.7.1's interface object on this realm's global. */
void domparser_install(JSContext *ctx, JSValueConst global);
/* The AGENT's half, undone — core/platform.h's release column. */
void domparser_free(void);

#endif
