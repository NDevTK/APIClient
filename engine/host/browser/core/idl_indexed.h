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
void idl_indexed_free(JSRuntime *rt);

/* A new object with `proto` that answers index lookups through `decl`. The decl is BORROWED and must outlive
   the runtime, which is what makes it a static per-interface constant rather than per-instance state. */
JSValue idl_indexed_new(JSContext *ctx, JSValueConst proto, const IdlIndexedDecl *decl);

/* WEB IDL §3.9's TWO ALGORITHMS, FOR A CLASS OF ITS OWN — the same [[GetOwnProperty]] and [[OwnPropertyKeys]]
 * the objects above answer with, callable from another class's exotic hooks.
 *
 * WHY THAT IS NEEDED AT ALL: an indexed property getter is not always the whole of what an object is. CSS
 * Animations §6.3.1's CSSKeyframesRule declares `getter CSSKeyframeRule (unsigned long index)` on an interface
 * that is ALSO a CSSRule — it carries that component's record behind that component's class opaque — and one
 * object cannot be two classes. So the class that owns the object owns the hooks, and the ALGORITHM stays
 * here: the index parse, the descriptor, the key enumeration and what `list['01']` means are written once, for
 * the reason this file's own header gives.
 *
 * `decl` is what the calling class resolved for THIS object, and NULL is a real answer meaning "this object is
 * not an indexed interface" — the lookup then reports no own property and no extra keys, which leaves an
 * ordinary property lookup exactly as it was. The return values are quickjs's exotic contract: 1/0 for found
 * or not (and -1 on failure) from the first, 0 or -1 from the second. */
int idl_indexed_own_property(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom prop,
                             const IdlIndexedDecl *decl);
int idl_indexed_own_property_names(JSContext *ctx, JSPropertyEnum **ptab, uint32_t *plen, JSValueConst obj,
                                   const IdlIndexedDecl *decl);

/* Web IDL §3.7.10: an interface with an indexed property getter and an integer `length` gets
   %Array.prototype.values% as its @@iterator. Installed on the PROTOTYPE, which is where the IDL puts it. */
void idl_indexed_install_iterable(JSContext *ctx, JSValueConst proto);

/* §3.7.10's OTHER clause, which is a DIFFERENT statement about a different set of interfaces: an interface that
   declares `iterable<V>` also gets `entries`, `keys`, `values` and `forEach`. NodeList and DOMTokenList declare
   one; HTMLCollection and NamedNodeMap do not, and one answer for all four gave an HTMLCollection a `forEach`
   the standard says it has no such member.
   IT IS ITS OWN CALL, not a flag on the one above: an interface says which members it has by INSTALLING them,
   and a `true` passed at one call site out of five is a statement no reader — the IDL gap audit included — can
   attribute to a prototype, so the four members read as installed on every indexed interface in the engine or
   on none. */
void idl_indexed_install_value_iterator(JSContext *ctx, JSValueConst proto);

#endif
