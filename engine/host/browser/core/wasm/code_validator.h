/* WebAssembly Core Specification (Release 3.0) §3.4 "Instructions": validation of one expr, a function body
 * (§3.5.6) or a constant expression (§3.4.13), by §7.6's algorithm over its bytes read in place, as a step
 * machine. A constant expression is first checked for constness, instruction by instruction, then typed.
 * One step reads one instruction (or one element of its list immediate) and turns its typing rule into a short
 * program of stack operations; each later step runs one element of one operation: pops one operand and matches
 * it against what the rule expects (one supertype of §3.3.10's walk a step), pushes one result, or restores one
 * local's initialization. The order of the checks inside an instruction is valid.ml's check_instr, so a
 * refusal carries its message; a type mismatch names the one operand it is about. */
#ifndef ENGINE_HOST_BROWSER_CORE_WASM_CODE_VALIDATOR_H
#define ENGINE_HOST_BROWSER_CORE_WASM_CODE_VALIDATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/wasm/decoder.h"
#include "core/wasm/instruction.h"
#include "core/wasm/matching.h"
#include "core/wasm/validation.h"

/* A list of value types without copying it: `n` of the module's val_types from `off` (POOL), `n` unpacked
   field types from `off` (FIELDS), or `one` repeated `n` times (ONE). */
typedef struct {
    uint8_t kind;
    WasmValType one;
    size_t off;
    uint64_t n;
} WasmTypeRun;

/* §7.6.1's ctrl_frame. `kind` is code_validator.c's F_ frame kind. */
typedef struct {
    uint8_t kind;
    WasmTypeRun start, end;
    size_t val_height, init_height;
    bool unreachable;
} WasmCtrlFrame;

/* One stack operation of an instruction's program (code_validator.c's M_ operations). */
typedef struct {
    uint8_t op;
    uint8_t aux;
    WasmTypeRun run, run2;
    WasmValidateStatus fail;
    uint32_t index;
} WasmStackOp;

#define WASM_CODE_PROGRAM 12

/* The validation's whole state. The context is borrowed; the stacks are owned and kept across exprs. */
typedef struct {
    const WasmContext *ctx;
    int phase;
    bool constant;              /* a constant expression, not a function body */
    WasmDecoder r;              /* the expr's bytes; `r.len` is where its final 0x0B stands */
    size_t start;               /* the expr's first byte, where typing restarts after the constness pass */
    WasmValType expect;         /* a constant expression's type */
    size_t visible_globals;     /* the globals a constant expression may read */
    uint32_t func_type;         /* a body's function type index */
    size_t code;                /* a body's code entry */
    uint64_t *local_ends;       /* per locals entry of the body: the index past its last local */
    size_t cap_local_ends, item;
    WasmValType *vals;          /* §7.6.1's value stack; a code of 0 is Bot */
    size_t n_vals, cap_vals;
    WasmCtrlFrame *ctrls;
    size_t n_ctrls, cap_ctrls;
    uint32_t *inits;            /* §7.6.1's initialization stack */
    size_t n_inits, cap_inits;
    uint64_t *init_set;         /* the locals it holds, open addressing over index plus one */
    size_t cap_init_set, used_init_set;
    WasmStackOp prog[WASM_CODE_PROGRAM];
    int n_prog, pc;
    uint64_t k;                 /* the element of prog[pc] the next step runs */
    bool climbing;
    WasmClimb climb;
    WasmInstruction in;         /* the instruction whose list immediate is being read */
    uint32_t list_left;
    size_t list_at;
    uint64_t arity;
    WasmTypeRun block_start, block_end;
    const char *space;          /* a refusal's index space, index, and the operand types it names */
    uint64_t index;
    WasmValType want, got;
    bool has_got;
} WasmCodeValidator;

void wasm_code_validator_init(WasmCodeValidator *cv, const WasmContext *ctx);
/* Begin code entry `code` of the module, defined function `n_imported funcs + code`. */
void wasm_code_validator_begin_body(WasmCodeValidator *cv, size_t code);
/* Begin a constant expression of type `expect` that may read globals below `visible_globals`. */
void wasm_code_validator_begin_const(WasmCodeValidator *cv, WasmExpr e, const WasmValType *expect,
                                     size_t visible_globals);
/* Run one element of the program or read one instruction. MORE, then DONE or a refusal. */
WasmValidateStatus wasm_code_validator_step(WasmCodeValidator *cv);
void wasm_code_validator_free(WasmCodeValidator *cv);

#endif
