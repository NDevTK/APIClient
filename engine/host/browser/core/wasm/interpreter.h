/* WebAssembly Core Specification (Release 3.0) §4.6 "Instructions": execution of an invocation from the
 * module's bytes in place (design note §3a), as a step machine. One step retires one instruction and charges it
 * one engine-work unit (quickjs.h's JS_FlowRetireWork, the unit a JS opcode costs); the caller owns how many
 * steps a rest covers. The whole state is the WasmInterpreter: operand, label and frame stacks of plain values,
 * each frame's locals, and every position an offset into the module's bytes or an index into the validator's
 * side table (code_validator.h), so a parked interpreter is a record with no pointer into itself. A call pushes
 * a frame and never recurses in C; a branch cuts the stack back to its target's side table height from its
 * frame. Floating point is §7.2's deterministic profile: an arithmetic result that is a
 * NaN is the positive canonical NaN, and bit operations (abs, neg, copysign, reinterpret) keep bits.
 * The page's bytes have been decoded and validated, so an instruction the validator accepted that this file
 * does not execute is this codebase's gap (CHECK_FAIL), never a refusal.
 * Named residual. Not covered: host calls and host globals and tables (imports), a call_indirect of another
 * instance's function (the store's cross-module type identity), memory (§4.6.8), and the vector, exception and
 * aggregate instructions and the references other than functions'. Next diff builds: imports. Absence shows
 * as: invoking a function that reaches one of them stops the process at the CHECK_FAIL naming its opcode. */
#ifndef ENGINE_HOST_BROWSER_CORE_WASM_INTERPRETER_H
#define ENGINE_HOST_BROWSER_CORE_WASM_INTERPRETER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/wasm/code_validator.h"
#include "core/wasm/store.h"
#include "core/wasm/wasm_module.h"


typedef enum {
    WASM_EXEC_DONE = 0,               /* the function returned; its results are the stack */
    WASM_EXEC_MORE,                   /* a step ended; step again */
    WASM_EXEC_FORK,                   /* the environment chooses (see WasmChoice); choose before stepping */
    WASM_EXEC_TRAP_UNREACHABLE,       /* §4.6.2's unreachable */
    WASM_EXEC_TRAP_DIVIDE_BY_ZERO,    /* §4.3.2's idiv and irem of a zero divisor */
    WASM_EXEC_TRAP_OVERFLOW,          /* §4.3.2's idiv_s of the minimum by -1, §4.3.4's trunc out of range */
    WASM_EXEC_TRAP_CONVERSION,        /* §4.3.4's trunc of a NaN */
    WASM_EXEC_TRAP_TABLE_BOUNDS,      /* §4.6.7: a table access, or an active segment, past the table's size */
    WASM_EXEC_TRAP_UNDEFINED_ELEMENT, /* §4.6.5: call_indirect's index past the table's size */
    WASM_EXEC_TRAP_UNINITIALIZED,     /* §4.6.5: call_indirect's element is the null reference */
    WASM_EXEC_TRAP_INDIRECT_TYPE,     /* §4.6.5: call_indirect's function does not match the expected type */
    WASM_EXEC_TRAP_NULL_FUNCTION,     /* §4.6.5: call_ref of the null reference */
    WASM_EXEC_TRAP_NULL_REFERENCE,    /* §4.6.9: ref.as_non_null of the null reference */
    WASM_EXEC_NO_ROOM                 /* a stack or the locals could not be allocated (§7.2's resource exhaustion) */
} WasmExecStatus;

/* A label of §4.2.17's stack: a block, loop or if (its side table `entry`), or the function's own. `height` is
   where a branch to it cuts the operand stack, `arity` how many values the branch carries, `cont` where it
   continues: past the block's end, or at a loop's first instruction. */
typedef struct {
    uint8_t kind;
    size_t entry;
    size_t height;
    uint32_t arity;
    size_t cont;
} WasmLabel;

/* §4.2.17's frame of one call: its code entry and where its body ends, the caller's next instruction (SIZE_MAX
   for the invocation's own frame), its locals (owned), and where its operands and labels begin on the
   interpreter's stacks. */
typedef struct {
    size_t inst;                 /* the module instance's store address */
    size_t code, end;
    size_t return_pc;
    WasmValue *locals;
    size_t n_locals;
    size_t val_base, label_base;
    uint32_t n_results;
} WasmFrame;

/* A bulk table instruction in progress, one element a step (§4.6.7's recursive table.fill, table.copy and
   table.init): its opcode, its tables' or segment's store addresses, and its operands left. */
typedef struct {
    uint32_t op;
    size_t table, from;
    uint64_t d, s, n;
    WasmValue ref;
} WasmBulk;

/* An outcome the spec leaves to the implementation, which is an unknown of the environment (§4.7.1's growtable
   may fail): the step answers FORK, the caller runs the example arm or forks the assumption against it, and
   wasm_interpreter_choose resumes with that arm. `kind` is 0 when none is pending. */
enum { WASM_CHOICE_TABLE_GROW = 1 };
typedef struct {
    uint8_t kind;
    size_t table;                /* the table's store address */
    uint64_t n;
} WasmChoice;

typedef struct {
    WasmStore *store;            /* borrowed: every frame's instance is the store's, by its address */
    WasmBulk bulk;
    WasmChoice choice;
    size_t pc;                   /* the current frame's next instruction */
    uint32_t n_results;          /* the invoked function's */
    WasmFrame *frames;           /* owned: one per call, the current last */
    size_t n_frames, cap_frames;
    WasmValue *vals;             /* owned: §4.2.17's operand stack */
    size_t n_vals, cap_vals;
    WasmLabel *labels;           /* owned: its labels, the function's at the bottom */
    size_t n_labels, cap_labels;
    uint64_t retired;            /* instructions retired, each charged one unit of work */
    uint64_t trap_index;         /* the element index a call_indirect trap names */
} WasmInterpreter;

/* Begin §4.7.3's invocation of defined function `func` (an index of the function index space, past the imports)
   of instance `inst` (instance.h), which has answered DONE. `args` are the function type's parameters, in
   order, whose count the caller has matched. NO_ROOM when the locals cannot be allocated. */
WasmExecStatus wasm_interpreter_begin(WasmInterpreter *it, WasmInstance *inst, size_t func, const WasmValue *args,
                                      size_t n_args);
/* Begin evaluating constant expression `e` (§4.6.13) of `inst`'s module, to one value, the way §4.7.2's
   instantiation does. */
WasmExecStatus wasm_interpreter_begin_expr(WasmInterpreter *it, WasmInstance *inst, WasmExpr e);
/* Retire one instruction. MORE, then DONE (the results are vals[0, n_results)) or a trap; stepping again is
   misuse (asserted). */
WasmExecStatus wasm_interpreter_step(WasmInterpreter *it);
/* Resume after FORK with the example arm (the grow succeeds; its storage is a CHECK where it cannot be had) or
   the assumption against it (the grow answers -1). */
WasmExecStatus wasm_interpreter_choose(WasmInterpreter *it, bool example);
/* The reference interpreter's message for a trap (interpreter/exec/eval.ml), which `assert_trap` matches by
   prefix; NULL for DONE and MORE. */
const char *wasm_exec_status_message(WasmExecStatus st);
/* eval.ml's message for the trap `it` answered, with the element index call_indirect's two element traps name. */
void wasm_interpreter_message(const WasmInterpreter *it, WasmExecStatus st, char *buf, size_t cap);
void wasm_interpreter_free(WasmInterpreter *it);

#endif
