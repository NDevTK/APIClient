/* ECMA-402 §16 NumberFormat Objects: the constructor, its prototype's format, formatToParts and
 * resolvedOptions, and supportedLocalesOf. */
#ifndef ENGINE_HOST_BROWSER_CORE_INTL_NUMBER_FORMAT_H
#define ENGINE_HOST_BROWSER_CORE_INTL_NUMBER_FORMAT_H

#include "quickjs.h"

void number_format_init(JSContext *ctx);
/* This realm's %Intl.NumberFormat%, with its prototype built and set as the class's. */
JSValue number_format_install(JSContext *ctx);
void number_format_free(JSRuntime *rt);

#endif
