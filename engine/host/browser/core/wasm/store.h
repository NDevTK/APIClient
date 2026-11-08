/* WebAssembly Core Specification (Release 3.0) §4.2.3 "Store": the function, global, table, memory, element and
 * data instances of every module instance of one agent, each named by its address (§4.2.4), an index into its
 * list.
 * A module instance (instance.h) maps its index spaces to addresses, so an imported entity and the exporting
 * instance's are one instance, and a function reference is its function address plus one, 0 the null reference.
 * Lists only grow and hold plain values; every cross reference is an address, never a pointer, so the store is
 * a record a parked flow can carry. Every list, and a table's elements, is storage of fixed-size chunks whose
 * entries never move once appended, so a record's address is stable for the store's life (solver/cow.h captures
 * a record by its address). The store is owned by a JS object of an internal class, which a parked flow's delta
 * entries hold; a memory instance is linear_memory.h's owner object, which the store holds a reference to. A list
 * that cannot grow is a CHECK (out of memory); a table's, memory's or segment's storage, whose size is the
 * page's, answers SIZE_MAX instead. */
#ifndef ENGINE_HOST_BROWSER_CORE_WASM_STORE_H
#define ENGINE_HOST_BROWSER_CORE_WASM_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "quickjs.h"
#include "core/wasm/matching.h"
#include "core/wasm/wasm_module.h"

typedef struct WasmInstance WasmInstance;

/* A value's bits: a number in `lo` (an i32 or f32 zero-extended), a vector in both halves, a function
   reference in `lo` as its function address plus one, 0 the null reference. */
typedef struct {
    uint64_t lo, hi;
} WasmValue;

/* §4.2.7's function instance: a defined function of module instance `inst` (its code entry `code`), or a host
   function the embedder numbers `host`; either way its type is index `type` of the store's type space `types`. */
typedef struct {
    bool is_host;
    size_t inst, code;
    uint32_t host;
    size_t types, type;
} WasmFuncInstance;

/* A value type as the store holds it: a concrete heap type is an index into the store's type space `types`,
   SIZE_MAX for a type that names no defined type (a host's abstract one). */
typedef struct {
    WasmValType type;
    size_t types;
} WasmStoreValType;

/* §4.2.10's global instance: its §4.2.2 global type and its value. */
typedef struct {
    WasmValue value;
    WasmStoreValType type;
    bool mutable_;
} WasmGlobalInstance;

/* §4.2.8's table instance: `size` references of type `elem`, `type` its §5.3.13 limits as declared (§4.5.2's
   external type takes `size` as the minimum). */
/* Entries of one size in chunks of WASM_LIST_CHUNK: an entry's address never changes once appended. */
#define WASM_LIST_CHUNK 64u
typedef struct {
    uint8_t **chunks;
    size_t n_chunks, cap_chunks;
    size_t n;                    /* entries appended */
} WasmList;

/* §4.2.8's table instance: `size` references of type `elem` in `elems`, `type` its §5.3.13 limits as declared
   (§4.5.2's external type takes `size` as the minimum). */
typedef struct {
    WasmList elems;              /* WasmValue entries, `size` of them in use */
    uint64_t size;
    WasmLimits type;
    WasmStoreValType elem;
} WasmTableInstance;

/* §4.2.12's element instance: its references, none once dropped. */
typedef struct {
    WasmValue *refs;
    uint64_t n;
} WasmElemInstance;

/* §4.2.13's data instance: its bytes, [offset, offset + n) of instance `inst`'s module bytes, none once
   dropped. */
typedef struct {
    size_t inst, offset;
    uint64_t n;
} WasmDataInstance;

typedef struct WasmStore {
    JSContext *ctx;              /* borrowed: the agent's context, which owns the objects below */
    JSValue owner;               /* the reference wasm_store_new answered, which owns this store */
    WasmList funcs, globals, tables, mems, elems, datas;   /* by address: each its instance's type below */
    WasmList instances;          /* WasmInstance *, borrowed: the module instances, by address */
    WasmList types;              /* const WasmTypes *, borrowed: the type spaces store types index (§4.2.6) */
} WasmStore;

/* A new store owned by a new object of the agent's store class, whose reference is `owner`. */
WasmStore *wasm_store_new(JSContext *ctx);
/* Drop the reference wasm_store_new answered; the store goes when its owner does. */
void wasm_store_release(WasmStore *s);
/* The instance at address `a`, which must exist (asserted). */
WasmFuncInstance *wasm_store_func(const WasmStore *s, size_t a);
WasmGlobalInstance *wasm_store_global(const WasmStore *s, size_t a);
WasmTableInstance *wasm_store_table(const WasmStore *s, size_t a);
JSValueConst wasm_store_mem(const WasmStore *s, size_t a);
WasmElemInstance *wasm_store_elem(const WasmStore *s, size_t a);
WasmDataInstance *wasm_store_data(const WasmStore *s, size_t a);
WasmInstance *wasm_store_instance(const WasmStore *s, size_t a);
const WasmTypes *wasm_store_types(const WasmStore *s, size_t a);
/* Element `i` of table `t`, below its size (asserted). */
WasmValue *wasm_table_elem(const WasmTableInstance *t, uint64_t i);
/* Append `n` null references to `t` (§4.7.1's growtable, which the caller has admitted); storage that cannot be
   had is a CHECK (out of memory). */
void wasm_table_grow(WasmTableInstance *t, uint64_t n);
/* Append an instance and answer its address (SIZE_MAX for a table, memory or segment whose storage cannot be
   had). */
size_t wasm_store_add_func(WasmStore *s, WasmFuncInstance f);
size_t wasm_store_add_global(WasmStore *s, WasmGlobalInstance g);
size_t wasm_store_add_table(WasmStore *s, const WasmLimits *type, WasmStoreValType elem);
size_t wasm_store_add_mem(WasmStore *s, const WasmLimits *type);
size_t wasm_store_add_elem(WasmStore *s, uint64_t n);
size_t wasm_store_add_data(WasmStore *s, WasmDataInstance d);
size_t wasm_store_add_instance(WasmStore *s, WasmInstance *inst);
size_t wasm_store_add_types(WasmStore *s, const WasmTypes *types);
/* The store's class, declared once per agent (core/platform.c's `wasm_store` row), and its release. */
void wasm_store_class_init(JSContext *ctx);
void wasm_store_class_free(void);

#endif
