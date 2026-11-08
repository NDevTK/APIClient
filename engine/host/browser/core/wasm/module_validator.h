/* WebAssembly Core Specification (Release 3.0) §3.5 "Modules": validation of a decoded WasmModule against the
 * contexts §3.1.6 builds from it, as a step machine. It runs after core/wasm/module_decoder.h answered DONE and
 * reads that WasmModule and its bytes; it owns nothing but its own name set.
 * One step checks one item: a supertype or value type of a §5.3.8 type, one token of a rec group's identity, one
 * field or value type of §3.3's matching or one supertype climbed, an import, tag, function, memory, table,
 * global, data or element segment, the start function, or an export. The order is interpreter/valid/valid.ml's
 * check_module, so a refusal carries that validator's message.
 * Type identity is §3.3.10's closed defined type: a rec group's types are equal to another's when the groups are
 * token for token equal with references into the group relative and references out of it by identity, so each
 * group gets a canonical id and a type is (its group's id, its index in the group).
 * Named residual. Not covered: §3.4's instructions (function bodies and every constant expression, with
 * §3.4.13's constness) and §3.5.13's declared function references. Next diff builds: §3.4's instructions by
 * §7.6's algorithm. Absence shows as: a module whose only fault is an ill-typed instruction or a non-constant
 * initializer is answered WASM_VALIDATE_DONE. */
#ifndef ENGINE_HOST_BROWSER_CORE_WASM_MODULE_VALIDATOR_H
#define ENGINE_HOST_BROWSER_CORE_WASM_MODULE_VALIDATOR_H

#include <stddef.h>
#include <stdint.h>

#include "core/wasm/wasm_module.h"

typedef enum {
    WASM_VALIDATE_DONE = 0,
    WASM_VALIDATE_MORE,
    WASM_VALIDATE_UNKNOWN_INDEX,      /* §3.1.6: an index past its context's list ("unknown <space> <index>") */
    WASM_VALIDATE_NOT_FUNCTION,       /* a type index that must name a function type names another */
    WASM_VALIDATE_MULTIPLE_SUPERS,    /* §3.2.11: a subtype with more than one supertype */
    WASM_VALIDATE_FORWARD_SUPER,      /* §3.2.11: a supertype index not below its subtype's */
    WASM_VALIDATE_FINAL_SUPER,        /* §3.2.11: a supertype that is final */
    WASM_VALIDATE_LIMITS_RANGE,       /* §3.2.12: a memory's or table's bound past its range */
    WASM_VALIDATE_LIMITS_ORDER,       /* §3.2.12: a minimum above its maximum */
    WASM_VALIDATE_TAG_RESULT,         /* §3.2.13: a tag type with results */
    WASM_VALIDATE_START_TYPE,         /* §3.5.10: a start function with parameters or results */
    WASM_VALIDATE_DUPLICATE_EXPORT,   /* §3.5.13: two exports of one name (nm* disjoint) */
    WASM_VALIDATE_SUB_MISMATCH,       /* §3.2.11: a subtype's composite type does not match its supertype's */
    WASM_VALIDATE_ELEM_MISMATCH,      /* §3.5.9: an active segment's type does not match its table's */
    WASM_VALIDATE_NO_ROOM             /* a table the validator keeps could not be allocated; its size is the input's */
} WasmValidateStatus;

/* The validation's whole state. `module` and `bytes` are borrowed and outlive it. */
typedef struct {
    const WasmModule *module;
    const uint8_t *bytes;
    int phase;
    size_t item, sub;           /* the item being checked, and the position inside it */
    size_t n_funcs, n_tables, n_memories, n_globals, n_tags;   /* the index spaces, imports first */
    size_t *names;              /* the export name set: open addressing over export indices plus one */
    size_t cap_names;
    const char *space;          /* for WASM_VALIDATE_UNKNOWN_INDEX: the index space and the index */
    uint64_t index, index2;     /* the indices a refusal names */
    size_t *group_canon;        /* per type index: the canonical id of the rec group starting there */
    uint64_t *tokens;           /* the canonical groups' tokens, then the group being identified */
    size_t n_tokens, cap_tokens;
    size_t group_tokens;        /* where the current group's tokens begin */
    uint64_t group_hash;
    size_t *canon_off, *canon_len;   /* per canonical id: its tokens */
    uint64_t *canon_hash;
    size_t n_canon;
    size_t *canon_table;        /* open addressing over canonical ids plus one, keyed by hash */
    size_t cap_canon_table, probe, compare;   /* PH_CANON: the slot probed and the token compared */
    size_t climb, climb_target; /* PH_CLIMB: §3.3.10's supertype walk, and where it continues */
    int climb_return;
    WasmValidateStatus climb_fail;
    size_t *table_imports;      /* per imported table: its import's index, filled as PH_IMPORTS passes it */
    size_t n_table_imports;
} WasmModuleValidator;

void wasm_module_validator_init(WasmModuleValidator *v, const WasmModule *m, const uint8_t *bytes);
/* Check one item. MORE while the validation continues, then DONE or a refusal; stepping again is misuse. */
WasmValidateStatus wasm_module_validator_step(WasmModuleValidator *v);
WasmValidateStatus wasm_module_validator_run(WasmModuleValidator *v);
/* valid.ml's message for the refusal `st`, written to `buf` (NUL terminated, at most `cap` bytes). */
void wasm_module_validator_message(const WasmModuleValidator *v, WasmValidateStatus st, char *buf, size_t cap);
/* Release the name set. Idempotent. */
void wasm_module_validator_free(WasmModuleValidator *v);

#endif
