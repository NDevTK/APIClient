/* Performance — Blink core/timing/Performance. window.performance: now / timeOrigin / mark / measure /
 * getEntriesByType / … See performance.c. */
#ifndef ENGINE_HOST_BROWSER_PERFORMANCE_H
#define ENGINE_HOST_BROWSER_PERFORMANCE_H
#include "quickjs.h"
JSValue js_performance_make(JSContext *ctx);   /* window.performance */
#endif
