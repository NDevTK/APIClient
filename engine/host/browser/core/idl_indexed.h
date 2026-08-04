/* WEB IDL'S INDEXED PROPERTY GETTER — see idl_indexed.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_IDL_INDEXED_H
#define ENGINE_HOST_BROWSER_CORE_IDL_INDEXED_H
#include <stdint.h>
#include "quickjs.h"

/* What an interface with an indexed property getter must answer: how many supported indices it has, and what
   is at one. `named` is the NAMED property getter beside it (HTMLCollection has one, a token list does not) —
   NULL when the IDL declares none. Both are pure reads of the component's own state, so they are ordinary C:
   a page cannot put code on the path an index lookup takes. */
typedef struct {
    const char *interface_name;                                  /* for the assertions, and only for them */
    uint32_t (*length)(JSContext *ctx, JSValueConst self);
    JSValue  (*item)(JSContext *ctx, JSValueConst self, uint32_t i);   /* owned; JS_UNDEFINED past the end */
    JSValue  (*named)(JSContext *ctx, JSValueConst self, const char *name);   /* owned; JS_UNDEFINED for none */
} IdlIndexedDecl;

void idl_indexed_init(JSContext *ctx);
void idl_indexed_free(JSContext *ctx);

/* A new object with `proto` that answers index lookups through `decl`. The decl is BORROWED and must outlive
   the runtime, which is what makes it a static per-interface constant rather than per-instance state. */
JSValue idl_indexed_new(JSContext *ctx, JSValueConst proto, const IdlIndexedDecl *decl);

/* Web IDL §3.7.10: an interface with an indexed property getter gets %Array.prototype.values% as @@iterator,
   and the value-iterator members beside it. Installed on the PROTOTYPE, which is where the IDL puts them. */
void idl_indexed_install_iterable(JSContext *ctx, JSValueConst proto);

#endif
