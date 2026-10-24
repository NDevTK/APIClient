/* ECMA-402 §11 DateTimeFormat Objects: the constructor, its prototype's format, formatToParts and
 * resolvedOptions, and supportedLocalesOf. */
#ifndef ENGINE_HOST_BROWSER_CORE_INTL_DATE_TIME_FORMAT_H
#define ENGINE_HOST_BROWSER_CORE_INTL_DATE_TIME_FORMAT_H

#include "quickjs.h"

void date_time_format_init(JSContext *ctx);
/* This realm's %Intl.DateTimeFormat%, with its prototype built and set as the class's. */
JSValue date_time_format_install(JSContext *ctx);
void date_time_format_free(JSRuntime *rt);

#endif
