/* ECMA-402 §8 The Intl Object: the namespace, its two functions and the three service constructors this engine
 * builds, installed on every realm's global.
 *
 * Named residual. Not covered: Intl.DisplayNames, DurationFormat, ListFormat, Locale, PluralRules,
 * RelativeTimeFormat and Segmenter; the range formatters (formatRange, formatRangeToParts); the normative
 * optional constructor mode of ECMA-402 §4.3 API Conventions' Note 1; and ECMA-402 §20 Locale Sensitive Functions of the
 * ECMAScript Language Specification (ECMA-402 §20.1.1 String.prototype.localeCompare, ECMA-402 §20.2.1
 * Number.prototype.toLocaleString, ECMA-402 §20.4.1 Date.prototype.toLocaleString and their siblings), which
 * still answer from quickjs's own code.
 * Next diff builds: each member as its own service over core/intl/intl_service.h and the host op list, the
 * locale-sensitive functions as calls into these services, one per diff. Absence shows as: `typeof
 * Intl.PluralRules` reading "undefined" where a browser answers "function", and toLocaleString answering
 * quickjs's fixed format where the same call through Intl.NumberFormat answers the locale's. */
#ifndef ENGINE_HOST_BROWSER_CORE_INTL_INTL_H
#define ENGINE_HOST_BROWSER_CORE_INTL_INTL_H

#include "quickjs.h"

/* Once per agent: the step machines, the service classes, and the realm intrinsic that builds %Intl%. */
void intl_init(JSContext *ctx);
void intl_free(JSRuntime *rt);

#endif
