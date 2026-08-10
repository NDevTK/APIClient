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
    /* AN INDEX CACHE the backing keeps PER OBJECT. `length` and `item` are pure reads, but "pure" says nothing
       about how LONG: a live collection has to count or walk the child list, so `list[i]` is O(i) and the loop
       every page writes over one is O(n²) in the page's own markup. Blink carries CollectionIndexCache for
       exactly this, and a cache is per-collection state, which a static per-interface decl cannot hold.
       So the interface declares how many BYTES of scratch it wants and this allocates them with the object,
       zeroed, freed with it. It holds NO JSValues and no atoms — nothing here is traced, and a backing that
       needs to own a reference is asking for the wrong mechanism. 0 = no cache, which is right for a static
       list whose item() is already an array read. */
    size_t   cache_size;
} IdlIndexedDecl;

/* This object's cache scratch, or NULL when the interface declared none. Zeroed at construction, so an
   all-zero state must mean "nothing cached" — and it does, because the tree version it is keyed on starts
   at 1 and only grows. */
void *idl_indexed_cache(JSValueConst obj);

void idl_indexed_init(JSContext *ctx);
void idl_indexed_free(JSContext *ctx);

/* A new object with `proto` that answers index lookups through `decl`. The decl is BORROWED and must outlive
   the runtime, which is what makes it a static per-interface constant rather than per-instance state. */
JSValue idl_indexed_new(JSContext *ctx, JSValueConst proto, const IdlIndexedDecl *decl);

/* Web IDL §3.7.10: an interface with an indexed property getter and an integer `length` gets
   %Array.prototype.values% as its @@iterator. `declares_iterable` is whether the interface ALSO declares
   `iterable<V>`, which is what adds `entries`, `keys`, `values` and `forEach` — NodeList and DOMTokenList do,
   HTMLCollection and NamedNodeMap do not, and one answer for all four gave an HTMLCollection a `forEach` the
   standard says it has no such member. Installed on the PROTOTYPE, which is where the IDL puts them. */
void idl_indexed_install_iterable(JSContext *ctx, JSValueConst proto, bool declares_iterable);

#endif
