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

/* THE WRITE HALF OF THE SAME TWO ALGORITHMS — Web IDL §3.9.3 [[DefineOwnProperty]] and §3.9.4 [[Delete]], for
 * the same `decl` and under the same NULL contract as the pair above.
 *
 * A READER OF THIS HEADER SHOULD KNOW WHY THEY ARE NOT OPTIONAL. §3.9.1's descriptor is non-writable, so a
 * [[Set]] of a SUPPORTED index is already refused by ECMAScript 10.1.9.2 OrdinarySetWithOwnDescriptor step 2.a
 * without any hook at all. An index PAST the end is a different path entirely: there is no own descriptor to be
 * non-writable, the prototype walk finds nothing, and step 2.d.ii's CreateDataProperty performs
 * [[DefineOwnProperty]] on the receiver — which without this hook is the ORDINARY define and CREATES a real own
 * property that shadows the getter at that index for the rest of the object's life. §3.9.3 step 1.2's answer is
 * `false`, and it is the same answer Object.defineProperty gets.
 *
 * `flags`, `val`, `getter` and `setter` are quickjs's exotic define_own_property contract; the return values are
 * that contract's too — true/false, or -1 with an exception pending, which is what a refusal becomes for
 * Object.defineProperty and for a STRICT-mode assignment (see JS_RefuseOrThrowTypeError). */
int idl_indexed_define_own_property(JSContext *ctx, JSValueConst obj, JSAtom prop, JSValueConst val,
                                    JSValueConst getter, JSValueConst setter, int flags,
                                    const IdlIndexedDecl *decl);
int idl_indexed_delete_property(JSContext *ctx, JSValueConst obj, JSAtom prop, const IdlIndexedDecl *decl);

/* Web IDL §3.7.9 Iterable declarations' define the iteration methods, step 1.1: an interface with an indexed
   property getter and an integer `length` gets %Array.prototype.values% as its @@iterator. Installed on the
   PROTOTYPE, which is where the IDL puts it.
   TWO SECTIONS CARRY THIS TITLE AND THE TITLE DOES NOT TELL THEM APART, so every citation of it in this
   engine names the ALGORITHM beside the number: §2.5.9 Iterable declarations is the `iterable<>` DECLARATION
   and the terminology it mints ("value pairs to iterate over", "pair iterator", "value iterator"), while
   §3.7.9 Iterable declarations is the JavaScript binding — the "define the iteration methods" algorithm whose
   steps put members on a prototype. A bare title matches both, and a term resolves to whichever section mints
   it rather than to the one whose steps the code runs.
   IT IS NOT §3.7.10, WHICH SAYS SO ITSELF: that section is "Asynchronous iterable declarations", and its own
   step 2 asserts that a definition reaching it "does not have an indexed property getter or an iterable
   declaration" — so no arm of it can ever be the clause implemented here.
   STEP NUMBERS IN THIS FILE AND IN core/idl_iter.* ARE TOP-LEVEL `<li>`s WITH LIST DEPTH TRACKED, counted off
   the fetched text. The count matters here because this algorithm's shape is not the one a flat reading
   gives: step 1 holds a nested list, so the value-iterator clause below is 1.2 and NOT a step 2. */
void idl_indexed_install_iterable(JSContext *ctx, JSValueConst proto);

/* §3.7.9's OTHER clause — its step 1.2, NESTED INSIDE step 1 rather than beside it, which is itself the
   statement that these four members belong only to an interface that ALSO has an indexed property getter.
   It is a DIFFERENT statement about a different set of interfaces: an interface that
   declares `iterable<V>` also gets `entries`, `keys`, `values` and `forEach`. NodeList and DOMTokenList declare
   one; HTMLCollection and NamedNodeMap do not, and one answer for all four gave an HTMLCollection a `forEach`
   the standard says it has no such member.
   IT IS ITS OWN CALL, not a flag on the one above: an interface says which members it has by INSTALLING them,
   and a `true` passed at one call site out of five is a statement no reader — the IDL gap audit included — can
   attribute to a prototype, so the four members read as installed on every indexed interface in the engine or
   on none. */
void idl_indexed_install_value_iterator(JSContext *ctx, JSValueConst proto);

#endif
