/* ECMA-402 §10 Collator Objects: the constructor, its prototype's compare and resolvedOptions, and
 * supportedLocalesOf. */
#ifndef ENGINE_HOST_BROWSER_CORE_INTL_COLLATOR_H
#define ENGINE_HOST_BROWSER_CORE_INTL_COLLATOR_H

#include "quickjs.h"

void collator_init(JSContext *ctx);
/* This realm's %Intl.Collator%, with its prototype built and set as the class's. */
JSValue collator_install(JSContext *ctx);
void collator_free(JSRuntime *rt);

#endif
