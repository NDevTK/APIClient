/* Intl.NumberFormat / DateTimeFormat / RelativeTimeFormat / Collator / ListFormat / DisplayNames / ... —
 * ECMA-402. Not built into this quickjs; a formatter's output is locale-dependent, unknown headless. Each
 * constructor yields the Intl formatter interface (see intl.c). */
#ifndef ENGINE_HOST_BROWSER_INTL_H
#define ENGINE_HOST_BROWSER_INTL_H
#include "quickjs.h"
JSValue js_intl_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv);   /* new Intl.NumberFormat() / … */
#endif
