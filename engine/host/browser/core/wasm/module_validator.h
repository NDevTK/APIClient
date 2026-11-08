/* WebAssembly Core Specification (Release 3.0) §3.5 "Modules": validation of a decoded WasmModule against the
 * context §3.1.6 builds from it (validation.h), as a step machine. It runs after core/wasm/module_decoder.h
 * answered DONE and reads that WasmModule and its bytes.
 * One step checks one item: a supertype or value type of a §5.3.8 type, one token of a rec group's identity, one
 * field or value type of §3.3's matching or one supertype climbed, an import, tag, function, memory, table,
 * global, data or element segment (each constant expression in it and each of its items), the start function,
 * or an export; a function body or constant expression is the code validator's (code_validator.h), stepped
 * here. The order is interpreter/valid/valid.ml's check_module, so a refusal carries that validator's message.
 * Type identity is §3.3.10's closed defined type: a rec group's types are equal to another's when the groups are
 * token for token equal with references into the group relative and references out of it by identity, so each
 * group gets a canonical id and a type is (its group's id, its index in the group). */
#ifndef ENGINE_HOST_BROWSER_CORE_WASM_MODULE_VALIDATOR_H
#define ENGINE_HOST_BROWSER_CORE_WASM_MODULE_VALIDATOR_H

#include <stddef.h>
#include <stdint.h>

#include "core/wasm/code_validator.h"
#include "core/wasm/matching.h"
#include "core/wasm/validation.h"
#include "core/wasm/wasm_module.h"

/* The validation's whole state. `module` and `bytes` are borrowed and outlive it. */
typedef struct {
    const WasmModule *module;
    const uint8_t *bytes;
    int phase;
    size_t item, sub;           /* the item being checked, and the position inside it */
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
    WasmClimb climb;            /* PH_CLIMB: §3.3.10's supertype walk, and where it continues */
    int climb_return;
    WasmValidateStatus climb_fail;
    WasmContext ctx;            /* §3.1.6's context; its import arrays are owned here */
    size_t passed[5];           /* PH_IMPORTS: the imports of each kind passed so far */
    WasmCodeValidator code;     /* PH_CODE: the expr being validated, and the phase it returns to */
    WasmSideTable side;         /* the bodies' block entries, owned */
    int code_return;
} WasmModuleValidator;

void wasm_module_validator_init(WasmModuleValidator *v, const WasmModule *m, const uint8_t *bytes);
/* Check one item. MORE while the validation continues, then DONE or a refusal; stepping again is misuse. */
WasmValidateStatus wasm_module_validator_step(WasmModuleValidator *v);
WasmValidateStatus wasm_module_validator_run(WasmModuleValidator *v);
/* valid.ml's message for the refusal `st`, written to `buf` (NUL terminated, at most `cap` bytes). */
void wasm_module_validator_message(const WasmModuleValidator *v, WasmValidateStatus st, char *buf, size_t cap);
/* The bodies' side table (code_validator.h), complete once the validation answered DONE; owned by `v`. */
const WasmSideTable *wasm_module_validator_side_table(const WasmModuleValidator *v);
/* Release everything the validation owns, the side table among it. Idempotent. */
void wasm_module_validator_free(WasmModuleValidator *v);

#endif
