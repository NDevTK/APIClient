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

#endif
