/* WebAssembly Core Specification (Release 3.0) §3.2.11's rec group identity across every module of one agent: a
 * canonical id per distinct closed rec group, so §3.3.10's closed defined types of two modules are one type
 * exactly when their groups have one id and the types one position in it. A group is its token string (built by
 * module_validator.c, references out of the group by canonical id), interned by a step machine: one probe, one
 * token compared, or one entry of the table's incremental rehash a step, so no step walks state the page grew.
 * Intern-only: an id, once given, means one group for the registry's lifetime. Interleaved interning is correct:
 * an insertion is one step, and a rehash moves the generation so a probe begun before it starts over. */
#ifndef ENGINE_HOST_BROWSER_CORE_WASM_TYPE_REGISTRY_H
#define ENGINE_HOST_BROWSER_CORE_WASM_TYPE_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t *tokens;                /* every canonical group's tokens, in id order */
    size_t n_tokens, cap_tokens;
    size_t *off, *len;               /* per id: its tokens */
    uint64_t *hash;
    size_t n, cap;
    size_t *table, cap_table;        /* open addressing over ids plus one, at most half full */
    size_t *old, cap_old, moved;     /* a rehash in progress: the table being emptied and its next slot */
    size_t place;                    /* the slot probed for old[moved], or SIZE_MAX before its first */
    uint64_t generation;             /* moves when `table` is replaced */
} WasmTypeRegistry;

typedef enum {
    WASM_INTERN_DONE = 0,            /* `id` is the group's canonical id */
    WASM_INTERN_MORE,
    WASM_INTERN_NO_ROOM              /* the registry could not grow; its size is the input's */
} WasmInternStatus;

/* One interning: the group's tokens (borrowed until DONE) and where its probe stands. */
typedef struct {
    const uint64_t *tokens;
    size_t len;
    uint64_t hash;
    size_t probe, compare;
    uint64_t generation;
    size_t id;
} WasmTypeIntern;

void wasm_type_registry_init(WasmTypeRegistry *r);
void wasm_type_registry_free(WasmTypeRegistry *r);
/* FNV-1a over a group's tokens, accumulated by their builder one token at a time. */
static inline uint64_t wasm_type_hash_start(void) { return 1469598103934665603u; }
static inline uint64_t wasm_type_hash_add(uint64_t h, uint64_t token) { return (h ^ token) * 1099511628211u; }
/* `hash` is the tokens' hash by the two functions above. */
void wasm_type_intern_begin(WasmTypeIntern *in, const uint64_t *tokens, size_t len, uint64_t hash);
WasmInternStatus wasm_type_intern_step(WasmTypeRegistry *r, WasmTypeIntern *in);

#endif
