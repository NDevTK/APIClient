/* HTML's ENUMERATED GLOBAL ATTRIBUTES — the HTMLElement members that COMPUTE a value from the tree rather than
   mirroring one attribute. See global_attributes.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_GLOBAL_ATTRIBUTES_H
#define ENGINE_HOST_BROWSER_CORE_HTML_GLOBAL_ATTRIBUTES_H
#include "quickjs.h"

/* Once per AGENT — the setter step ids these members are declared with. */
void global_attributes_declare(JSContext *ctx);
/* Onto ONE realm's HTMLElement.prototype: §3.2.6.3's `translate`, §6.8.5's `spellcheck`, §6.8.6's
   `writingSuggestions`, §6.8.7's `autocapitalize`, §6.8.8's `autocorrect`, §6.8.1's `contentEditable` and
   `isContentEditable`, and §6.11.7's `draggable`. */
void global_attributes_install(JSContext *ctx, JSValueConst proto);
void global_attributes_free(void);

#endif
