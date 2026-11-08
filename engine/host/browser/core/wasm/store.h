/* WebAssembly Core Specification (Release 3.0) §4.2.3 "Store": the function, global, table and element
 * instances of every module instance of one agent, each named by its address (§4.2.4), an index into its list.
 * A module instance (instance.h) maps its index spaces to addresses, so an imported entity and the exporting
 * instance's are one instance, and a function reference is its function address plus one, 0 the null reference.
 * Lists only grow and hold plain values; every cross reference is an address, never a pointer, so the store is
 * a record a parked flow can carry. A list that cannot grow is a CHECK (out of memory); a table's or segment's
 * storage, whose size is the page's, answers SIZE_MAX instead. */
#ifndef ENGINE_HOST_BROWSER_CORE_WASM_STORE_H
#define ENGINE_HOST_BROWSER_CORE_WASM_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/wasm/wasm_module.h"

typedef struct WasmInstance WasmInstance;

/* A value's bits: a number in `lo` (an i32 or f32 zero-extended), a vector in both halves, a function
   reference in `lo` as its function address plus one, 0 the null reference. */
typedef struct {
    uint64_t lo, hi;
} WasmValue;

/* §4.2.7's function instance: a defined function of module instance `inst` (its code entry `code`), or a host
   function the embedder numbers `host`. */
typedef struct {
    bool is_host;
    size_t inst, code;
    uint32_t host;
} WasmFuncInstance;

/* §4.2.8's table instance: `size` references, `type` its §5.3.13 limits. */
typedef struct {
    WasmValue *elems;
    uint64_t size, cap;
    WasmLimits type;
} WasmTableInstance;

/* §4.2.12's element instance: its references, none once dropped. */
typedef struct {
    WasmValue *refs;
    uint64_t n;
} WasmElemInstance;

typedef struct {
    WasmFuncInstance *funcs;
    size_t n_funcs, cap_funcs;
    WasmValue *globals;          /* §4.2.10's global instances, by their values */
    size_t n_globals, cap_globals;
    WasmTableInstance *tables;
    size_t n_tables, cap_tables;
    WasmElemInstance *elems;
    size_t n_elems, cap_elems;
    WasmInstance **instances;    /* borrowed: the module instances, by address */
    size_t n_instances, cap_instances;
} WasmStore;

void wasm_store_init(WasmStore *s);
/* Append an instance and answer its address (SIZE_MAX for a table or segment whose storage cannot be had). */
size_t wasm_store_add_func(WasmStore *s, WasmFuncInstance f);
size_t wasm_store_add_global(WasmStore *s, WasmValue v);
size_t wasm_store_add_table(WasmStore *s, const WasmLimits *type);
size_t wasm_store_add_elem(WasmStore *s, uint64_t n);
size_t wasm_store_add_instance(WasmStore *s, WasmInstance *inst);
/* Release every instance's storage; the module instances are their owners'. */
void wasm_store_free(WasmStore *s);

#endif
