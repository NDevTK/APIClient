/* Hand-built WebAssembly module-validation rows for test_forced.c's wasm_module_validator_selftest; the spec
 * testsuite's are fixture/wasm_testsuite.h's. Each row decodes (core/wasm/module_decoder.h) and is then validated;
 * `expect` is the refusal's message, matched by prefix, and NULL is a valid module. They are built over Release
 * 3.0 §3.2, §3.3 and §3.5, each with interpreter/valid/valid.ml's message by reading; node 22's V8 agrees with
 * every verdict but the table of 2^32 elements, whose u64 limits it does not decode (no table64). The §3.3 rows
 * need a walk over two supertypes, which no testsuite row needs; f3268fa9's reference interpreter agrees. */
#ifndef ENGINE_HOST_FIXTURE_WASM_VALID_H
#define ENGINE_HOST_FIXTURE_WASM_VALID_H

#include "fixture/wasm_binary.h"   /* WasmBinaryRow */

static const WasmBinaryRow WASM_VALIDATE_ROWS[] = {
    { "types, imports of every kind, a tag, a start function and exports", 113, NULL,
      "\000\141\163\155\001\000\000\000\001\031\003\116\002\120\000\137\001\143\001\000\120\001"
      "\000\137\001\143\001\000\140\000\000\140\001\177\000\002\035\004\001\155\001\146\000\002"
      "\001\155\001\164\001\160\000\001\001\155\001\147\003\177\000\001\155\001\145\004\000\003"
      "\003\002\001\002\005\004\001\001\000\001\015\003\001\000\002\007\025\005\001\141\000\001"
      "\001\142\001\000\001\143\002\000\001\144\003\000\001\145\004\001\010\001\001\012\004\001"
      "\002\000\013" },
    { "a function of an unknown type", 24, "unknown type 5",
      "\000\141\163\155\001\000\000\000\001\004\001\140\000\000\003\002\001\005\012\004\001\002"
      "\000\013" },
    { "a function of a struct type", 23, "non-function type 0",
      "\000\141\163\155\001\000\000\000\001\003\001\137\000\003\002\001\000\012\004\001\002\000"
      "\013" },
    { "a subtype with two supertypes", 21, "multiple supertypes",
      "\000\141\163\155\001\000\000\000\001\013\002\120\000\137\000\120\002\000\000\137\000" },
    { "a supertype later in its rec group", 22, "forward use of type 1 in sub type definition",
      "\000\141\163\155\001\000\000\000\001\014\001\116\002\120\001\001\137\000\120\000\137\000" },
    { "a final supertype", 18, "sub type 1 has final super type 0",
      "\000\141\163\155\001\000\000\000\001\010\002\137\000\120\001\000\137\000" },
    { "a reference to a type past the module's", 16, "unknown type 3",
      "\000\141\163\155\001\000\000\000\001\006\001\140\001\144\003\000" },
    { "a reference to a type in a later rec group", 19, "unknown type 1",
      "\000\141\163\155\001\000\000\000\001\011\002\140\001\144\001\000\140\000\000" },
    { "a memory of 65537 pages", 15, "memory size must be at most 2^16 pages (4 GiB) for i32",
      "\000\141\163\155\001\000\000\000\005\005\001\000\201\200\004" },
    { "a memory whose minimum is above its maximum", 14, "size minimum must not be greater than maximum",
      "\000\141\163\155\001\000\000\000\005\004\001\001\002\001" },
    { "a table of 2^32 elements", 18, "table size must be at most 2^32-1 for i32",
      "\000\141\163\155\001\000\000\000\004\010\001\160\000\200\200\200\200\020" },
    { "a tag with a result", 20, "non-empty tag result type",
      "\000\141\163\155\001\000\000\000\001\005\001\140\000\001\177\015\003\001\000\000" },
    { "a start function with a parameter", 28, "start function must not have parameters or results",
      "\000\141\163\155\001\000\000\000\001\005\001\140\001\177\000\003\002\001\000\010\001\000"
      "\012\004\001\002\000\013" },
    { "two exports named a", 35, "duplicate export name \"a\"",
      "\000\141\163\155\001\000\000\000\001\004\001\140\000\000\003\002\001\000\007\011\002\001"
      "\141\000\000\001\141\000\000\012\004\001\002\000\013" },
    { "an export of an unknown function", 15, "unknown function 9",
      "\000\141\163\155\001\000\000\000\007\005\001\001\141\000\011" },
    { "an element segment of an unknown table", 35, "unknown table 1",
      "\000\141\163\155\001\000\000\000\001\004\001\140\000\000\003\002\001\000\011\011\001\002"
      "\001\101\000\013\000\001\000\012\004\001\002\000\013" },
    { "a field whose type matches its supertype's through two supertypes", 40, NULL,
      "\000\141\163\155\001\000\000\000\001\036\005\120\000\137\000\120\001\000\137\000\120\001"
      "\001\137\000\120\000\137\001\143\000\000\120\001\003\137\001\143\002\000" },
    { "a field whose type is above its supertype's", 40, "sub type 4 does not match super type 3",
      "\000\141\163\155\001\000\000\000\001\036\005\120\000\137\000\120\001\000\137\000\120\001"
      "\001\137\000\120\000\137\001\143\002\000\120\001\003\137\001\143\000\000" },
};

/* Side table rows (code_validator.h's WasmSideTable): each body's blocks with their offsets into the module's
 * bytes and their heights, worked by hand from §7.6.1's push_ctrl and checked valid with node 22's V8. The
 * block holds two operands below it; the if inside it a third, its condition popped. */
typedef struct {
    const char *src;
    size_t len;
    const char *bytes;
    size_t n;
    struct {
        size_t at, else_at, end_at, height;
    } blocks[4];
} WasmSideTableRow;

static const WasmSideTableRow WASM_SIDE_TABLE_ROWS[] = {
    { "a block over two operands, an if with an else inside it over three, then a valued block", 49,
      "\000\141\163\155\001\000\000\000\001\004\001\140\000\000\003\002\001\000\012\035\001\033\000"
      "\101\001\101\002\002\100\101\003\101\004\004\100\001\005\013\032\013\002\177\101\005\013\032\032"
      "\032\013",
      3, { { 27, 0, 39, 2 }, { 33, 36, 37, 3 }, { 40, 0, 44, 2 } } },
};

/* Invocation rows the testsuite's subset does not reach, run by node 22's V8 to the same i32: a return_call
 * inside a callee, which returns to its caller's caller (f calls g, g tail-calls h, f adds 1 to h's 6), two
 * global initializers, the second reading the first (40 + 2, then that times 3), and a table.grow by one, on
 * its example arm (the old size, 0, as V8 answers) and on its assumption arm (-1). `assume` runs every choice
 * the interpreter forks (interpreter.h's WasmChoice) on the assumption arm. */
typedef struct {
    const char *src;
    size_t len;
    const char *bytes;
    const char *export_name;
    uint32_t result;
    bool assume;
} WasmInvokeRow;

static const WasmInvokeRow WASM_INVOKE_ROWS[] = {
    { "a tail call from a callee returns to its caller's caller", 61,
      "\000\141\163\155\001\000\000\000\001\012\002\140\000\001\177\140\001\177\001\177\003\004\003\000"
      "\001\001\007\005\001\001\146\000\000\012\032\003\011\000\101\005\020\001\101\001\152\013\006\000"
      "\040\000\022\002\013\007\000\040\000\101\001\152\013",
      "f", 7 },
    { "a global initializer reads the one before it", 53,
      "\000\141\163\155\001\000\000\000\001\005\001\140\000\001\177\003\002\001\000\006\021\002\177\000"
      "\101\050\101\002\152\013\177\000\043\000\101\003\154\013\007\005\001\001\147\000\000\012\006\001"
      "\004\000\043\001\013",
      "g", 126 },
    { "a table.grow on its example arm", 45,
      "\000\141\163\155\001\000\000\000\001\005\001\140\000\001\177\003\002\001\000\004\004\001\160\000"
      "\000\007\005\001\001\147\000\000\012\013\001\011\000\320\160\101\001\374\017\000\013",
      "g", 0 },
    { "a table.grow on its assumption arm", 45,
      "\000\141\163\155\001\000\000\000\001\005\001\140\000\001\177\003\002\001\000\004\004\001\160\000"
      "\000\007\005\001\001\147\000\000\012\013\001\011\000\320\160\101\001\374\017\000\013",
      "g", 0xFFFFFFFFu, true },
};

/* Cross-instance rows: two modules validated against one type registry, each instantiated in one store; the host
 * (as the JS API's Table.prototype.set would) puts the exporter's function `put` in the caller's table "tab" at 0,
 * then invokes the caller's `call`, whose call_indirect's expected type is the caller's own. Converted from
 * text (WebAssembly/spec f3268fa9's reference interpreter, `-d -o`); types in index order:
 *   exporter: (sub (func (result i32))) $t, (sub $t (func (result i32))) $u, (func (result (ref null $t))) $r,
 *     f42: $t, sub7: $u, wrong: (func (param i32) (result i32)), final: (func (result i32)), rnull: $r.
 *   caller: (func (param f64)), (sub (func (result i32))) $t, (func (result (ref null $t))) $r2, a table "tab"
 *     of one funcref, call0: call_indirect $t of element 0, callr: ref.is_null of call_indirect $r2.
 * Node 22's V8 answers each row as written. */
#define WASM_XMOD_EXPORTER_LEN 150
static const char WASM_XMOD_EXPORTER[] =
      "\000\141\163\155\001\000\000\000\001\234\200\200\200\000\005\120\000\140\000\001\177\120\001\000"
      "\140\000\001\177\140\000\001\143\000\140\001\177\001\177\140\000\001\177\003\206\200\200\200\000"
      "\005\000\001\003\004\002\007\246\200\200\200\000\005\003\146\064\062\000\000\004\163\165\142\067"
      "\000\001\005\167\162\157\156\147\000\002\005\146\151\156\141\154\000\003\005\162\156\165\154\154"
      "\000\004\012\256\200\200\200\000\005\204\200\200\200\000\000\101\052\013\204\200\200\200\000\000"
      "\101\007\013\204\200\200\200\000\000\040\000\013\204\200\200\200\000\000\101\011\013\204\200\200"
      "\200\000\000\320\000\013";
#define WASM_XMOD_CALLER_LEN 114
static const char WASM_XMOD_CALLER[] =
      "\000\141\163\155\001\000\000\000\001\224\200\200\200\000\004\140\001\174\000\120\000\140\000\001"
      "\177\140\000\001\143\001\140\000\001\177\003\203\200\200\200\000\002\003\003\004\204\200\200\200"
      "\000\001\160\000\001\007\227\200\200\200\000\003\003\164\141\142\001\000\005\143\141\154\154\060"
      "\000\000\005\143\141\154\154\162\000\001\012\232\200\200\200\000\002\207\200\200\200\000\000\101"
      "\000\021\001\000\013\210\200\200\200\000\000\101\000\021\002\000\321\013";

typedef struct {
    const char *src;
    const char *put, *call;
    bool trap;          /* the call_indirect's type check traps */
    uint32_t result;
} WasmCrossInstanceRow;

static const WasmCrossInstanceRow WASM_CROSS_INSTANCE_ROWS[] = {
    { "one closed type at different indices of two modules", "f42", "call0", false, 42 },
    { "a subtype of it, climbed in the exporter's module", "sub7", "call0", false, 7 },
    { "a type with other parameters", "wrong", "call0", true, 0 },
    { "a final type is not the open one", "final", "call0", true, 0 },
    { "a type naming another rec group, by that group's identity", "rnull", "callr", false, 1 },
};

#endif
