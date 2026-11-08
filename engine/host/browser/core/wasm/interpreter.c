/* WebAssembly Core Specification (Release 3.0) §4.6 "Instructions", the numeric and control subset. See
   interpreter.h. */
#include "core/wasm/interpreter.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/wasm/instance.h"
#include "core/wasm/instruction.h"

enum { L_FUNC = 1, L_BLOCK, L_LOOP, L_IF };

#define F32_CANON 0x7FC00000u
#define F64_CANON 0x7FF8000000000000ull

static bool grow(void **p, size_t *cap, size_t n, size_t elem)
{
    size_t c;
    void *q;

    if (n < *cap) return true;
    c = *cap ? *cap * 2 : 64;
    if (c > SIZE_MAX / elem) return false;
    q = realloc(*p, c * elem);
    if (q == NULL) return false;
    *p = q;
    *cap = c;
    return true;
}

static bool push_label(WasmInterpreter *it, uint8_t kind, size_t entry, size_t height, uint32_t arity, size_t cont)
{
    WasmLabel *l;

    if (!grow((void **)&it->labels, &it->cap_labels, it->n_labels, sizeof *it->labels)) return false;
    l = &it->labels[it->n_labels++];
    l->kind = kind;
    l->entry = entry;
    l->height = height;
    l->arity = arity;
    l->cont = cont;
    return true;
}

/* The stack. Validation guarantees every pop has its operand. */
static uint64_t pop(WasmInterpreter *it)
{
    DCHECK(it->n_vals > 0, "a validated WebAssembly body popped an empty operand stack");
    return it->vals[--it->n_vals].lo;
}

static bool push(WasmInterpreter *it, uint64_t lo)
{
    if (!grow((void **)&it->vals, &it->cap_vals, it->n_vals, sizeof *it->vals)) return false;
    it->vals[it->n_vals].lo = lo;
    it->vals[it->n_vals].hi = 0;
    it->n_vals++;
    return true;
}

static WasmFrame *frame(WasmInterpreter *it)
{
    DCHECK(it->n_frames > 0, "a WebAssembly instruction ran with no frame");
    return &it->frames[it->n_frames - 1];
}

/* The current frame's instance, and its module, bytes and side table. */
static WasmInstance *INST(WasmInterpreter *it)
{
    return it->store->instances[frame(it)->inst];
}
#define IM(it) (INST(it)->module)
#define IB(it) (INST(it)->bytes)
#define IS(it) (INST(it)->side)

/* §4.6.5's invocation of the function at store address `func`: a frame in its instance whose locals are the
   `t->n_params` operands at the top of the stack, then each declared local's default (zero for the number types),
   and the function's own label. The arguments leave the stack, which the frame's results replace on return. */
static WasmExecStatus push_frame(WasmInterpreter *it, size_t func, size_t return_pc)
{
    const WasmFuncInstance *fi;
    const WasmModule *m;
    const WasmCode *c;
    const WasmSubType *t;
    WasmFrame *f;
    size_t n, i;

    DCHECK(func < it->store->n_funcs, "a WebAssembly call names a function address past the store's");
    fi = &it->store->funcs[func];
    if (fi->is_host) {
        t = &it->store->types[fi->types]->module->types[fi->type];
        DCHECK(it->n_vals >= t->n_params, "a validated WebAssembly call has fewer operands than its parameters");
        it->host.pending = true;
        it->host.func = func;
        it->host.n_args = (uint32_t)t->n_params;
        it->host.n_results = (uint32_t)t->n_results;
        it->pc = return_pc;
        return WASM_EXEC_HOST_CALL;
    }
    m = it->store->instances[fi->inst]->module;
    c = &m->codes[fi->code];
    t = &m->types[m->func_types[fi->code]];
    DCHECK(it->n_vals >= t->n_params, "a validated WebAssembly call has fewer operands than its parameters");
    if (!grow((void **)&it->frames, &it->cap_frames, it->n_frames, sizeof *it->frames)) return WASM_EXEC_NO_ROOM;
    for (n = t->n_params, i = 0; i < c->n_locals; i++) n += m->locals[c->locals + i].count;
    f = &it->frames[it->n_frames];
    memset(f, 0, sizeof *f);
    f->locals = calloc(n ? n : 1, sizeof *f->locals);
    if (f->locals == NULL) return WASM_EXEC_NO_ROOM;
    it->n_frames++;
    f->inst = fi->inst;
    f->code = fi->code;
    f->end = c->body_offset + c->body_size;
    f->return_pc = return_pc;
    f->n_locals = n;
    f->n_results = (uint32_t)t->n_results;
    f->val_base = it->n_vals - t->n_params;
    if (t->n_params) memcpy(f->locals, it->vals + f->val_base, t->n_params * sizeof *f->locals);
    it->n_vals = f->val_base;
    f->label_base = it->n_labels;
    it->pc = c->body_offset;
    return push_label(it, L_FUNC, SIZE_MAX, f->val_base, f->n_results, f->end) ? WASM_EXEC_MORE
                                                                                 : WASM_EXEC_NO_ROOM;
}

/* §4.6.5's return from the current frame: its results replace its arguments, and the caller continues. */
static WasmExecStatus pop_frame(WasmInterpreter *it)
{
    WasmFrame *f = frame(it);

    it->n_labels = f->label_base;
    it->pc = f->return_pc;
    free(f->locals);
    it->n_frames--;
    return it->n_frames == 0 ? WASM_EXEC_DONE : WASM_EXEC_MORE;
}

/* The store address of function `x` of the current instance, an import's resolved by instantiation. */
static size_t func_addr(WasmInterpreter *it, uint64_t x)
{
    size_t a = INST(it)->funcs[x];

    DCHECKF(a < it->store->n_funcs, "a WebAssembly instance's function %llu has no store address",
            (unsigned long long)x);
    return a;
}

static void begin(WasmInterpreter *it, WasmInstance *inst)
{
    DCHECK(it != NULL && inst != NULL && inst->store != NULL, "a WebAssembly invocation began without its "
                                                              "instance");
    memset(it, 0, sizeof *it);
    it->store = inst->store;
}

WasmExecStatus wasm_interpreter_begin_expr(WasmInterpreter *it, WasmInstance *inst, WasmExpr e)
{
    WasmFrame *f;

    begin(it, inst);
    it->n_results = 1;
    if (!grow((void **)&it->frames, &it->cap_frames, 0, sizeof *it->frames)) return WASM_EXEC_NO_ROOM;
    f = &it->frames[it->n_frames++];
    memset(f, 0, sizeof *f);
    f->inst = inst->addr;
    f->code = SIZE_MAX;
    f->end = e.offset + e.size;
    f->return_pc = SIZE_MAX;
    f->n_results = 1;
    it->pc = e.offset;
    return push_label(it, L_FUNC, SIZE_MAX, 0, 1, f->end) ? WASM_EXEC_MORE : WASM_EXEC_NO_ROOM;
}

WasmExecStatus wasm_interpreter_begin(WasmInterpreter *it, WasmInstance *inst, size_t func, const WasmValue *args,
                                      size_t n_args)
{
    const WasmFuncInstance *fi;
    const WasmSubType *t;
    size_t i;

    begin(it, inst);
    DCHECKF(func < inst->imported_funcs + inst->module->n_codes, "a WebAssembly invocation names function %zu, "
            "past the instance's %zu", func, inst->imported_funcs + inst->module->n_codes);
    fi = &inst->store->funcs[inst->funcs[func]];
    t = &inst->store->types[fi->types]->module->types[fi->type];
    DCHECKF(n_args == t->n_params, "a WebAssembly invocation passed %zu arguments to a function of %zu "
            "parameters", n_args, t->n_params);
    it->n_results = (uint32_t)t->n_results;
    for (i = 0; i < n_args; i++)
        if (!push(it, args[i].lo)) return WASM_EXEC_NO_ROOM;
    return push_frame(it, inst->funcs[func], SIZE_MAX);
}

/* Bits. */
static float f32_of(uint64_t b) { uint32_t x = (uint32_t)b; float f; memcpy(&f, &x, 4); return f; }
static double f64_of(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }
static uint64_t f32_bits(float f) { uint32_t x; memcpy(&x, &f, 4); return x; }
static uint64_t f64_bits(double d) { uint64_t x; memcpy(&x, &d, 8); return x; }
/* §4.3.3's nans_N under §7.2's deterministic profile: an arithmetic NaN result is the positive canonical one. */
static uint64_t f32_arith(float f) { return isnan(f) ? F32_CANON : f32_bits(f); }
static uint64_t f64_arith(double d) { return isnan(d) ? F64_CANON : f64_bits(d); }

/* §4.3.3's fmin and fmax: a NaN operand gives a NaN, and -0 is below +0. */
static double fmin_wasm(double a, double b, bool *nan)
{
    *nan = isnan(a) || isnan(b);
    if (a == b) return signbit(a) ? a : b;
    return a < b ? a : b;
}

static double fmax_wasm(double a, double b, bool *nan)
{
    *nan = isnan(a) || isnan(b);
    if (a == b) return signbit(a) ? b : a;
    return a > b ? a : b;
}

/* The side table entry of the structured instruction at `at`: the code entry's run is in byte order. */
static size_t block_entry(WasmInterpreter *it, size_t at)
{
    size_t code = frame(it)->code;
    size_t lo = IS(it)->code_first[code], hi = IS(it)->code_first[code + 1];

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;

        if (IS(it)->blocks[mid].at < at) lo = mid + 1;
        else hi = mid;
    }
    CHECKF(lo < IS(it)->code_first[code + 1] && IS(it)->blocks[lo].at == at,
           "no side table entry for the block at %zu, which validation records for every block", at);
    return lo;
}

/* §4.6.2's br to label `l` (0 innermost): the label's arity of values is carried down to its height, and the
   labels inside it go. A branch to the function's label is a return. */
static WasmExecStatus branch(WasmInterpreter *it, uint64_t l)
{
    WasmLabel *t;
    size_t keep;

    DCHECK(l < it->n_labels - frame(it)->label_base, "a validated WebAssembly branch names a label past its "
                                                     "frame's");
    t = &it->labels[it->n_labels - 1 - l];
    DCHECK(it->n_vals >= t->height + t->arity, "a WebAssembly branch has fewer values than its label carries");
    if (t->arity > 0) memmove(it->vals + t->height, it->vals + it->n_vals - t->arity, t->arity * sizeof *it->vals);
    it->n_vals = t->height + t->arity;
    it->pc = t->cont;
    keep = it->n_labels - 1 - l;
    if (t->kind == L_FUNC) return pop_frame(it);
    it->n_labels = t->kind == L_LOOP ? keep + 1 : keep;
    return WASM_EXEC_MORE;
}

static uint32_t clz32(uint32_t x) { return x ? (uint32_t)__builtin_clz(x) : 32; }
static uint32_t ctz32(uint32_t x) { return x ? (uint32_t)__builtin_ctz(x) : 32; }
static uint64_t clz64(uint64_t x) { return x ? (uint64_t)__builtin_clzll(x) : 64; }
static uint64_t ctz64(uint64_t x) { return x ? (uint64_t)__builtin_ctzll(x) : 64; }

/* §4.3.4's trunc: NaN is an invalid conversion, a truncated value outside [lo, hi) an overflow. */
static WasmExecStatus trunc_check(double x, double lo, double hi, double *t)
{
    if (isnan(x)) return WASM_EXEC_TRAP_CONVERSION;
    *t = trunc(x);
    return *t >= lo && *t < hi ? WASM_EXEC_DONE : WASM_EXEC_TRAP_OVERFLOW;
}

#define TWO31 2147483648.0
#define TWO32 4294967296.0
#define TWO63 9223372036854775808.0
#define TWO64 18446744073709551616.0

/* §4.3.4's trunc_sat: NaN is 0, and a value outside the range is the nearest bound. */
static uint64_t trunc_sat(double x, bool is_signed, bool wide)
{
    double t = trunc(x);

    if (isnan(x)) return 0;
    if (!wide && is_signed) return t < -TWO31 ? (uint32_t)INT32_MIN : t >= TWO31 ? (uint32_t)INT32_MAX
                                                                                 : (uint32_t)(int32_t)t;
    if (!wide) return t <= -1.0 ? 0 : t >= TWO32 ? UINT32_MAX : (uint32_t)t;
    if (is_signed) return t < -TWO63 ? (uint64_t)INT64_MIN : t >= TWO63 ? (uint64_t)INT64_MAX : (uint64_t)(int64_t)t;
    return t <= -1.0 ? 0 : t >= TWO64 ? UINT64_MAX : (uint64_t)t;
}

/* §4.3.2's 32-bit integer operations and the i32 comparisons, from 0x45 to 0x78 except the i64 and float tests. */
static WasmExecStatus binop_i32(WasmInterpreter *it, uint32_t op)
{
    uint32_t b = (uint32_t)pop(it), a = (uint32_t)pop(it), r;

    switch (op) {
    case 0x46: r = a == b; break;
    case 0x47: r = a != b; break;
    case 0x48: r = (int32_t)a < (int32_t)b; break;
    case 0x49: r = a < b; break;
    case 0x4A: r = (int32_t)a > (int32_t)b; break;
    case 0x4B: r = a > b; break;
    case 0x4C: r = (int32_t)a <= (int32_t)b; break;
    case 0x4D: r = a <= b; break;
    case 0x4E: r = (int32_t)a >= (int32_t)b; break;
    case 0x4F: r = a >= b; break;
    case 0x6A: r = a + b; break;
    case 0x6B: r = a - b; break;
    case 0x6C: r = a * b; break;
    case 0x6D:
        if (b == 0) return WASM_EXEC_TRAP_DIVIDE_BY_ZERO;
        if (a == (uint32_t)INT32_MIN && b == UINT32_MAX) return WASM_EXEC_TRAP_OVERFLOW;
        r = (uint32_t)((int32_t)a / (int32_t)b);
        break;
    case 0x6E:
        if (b == 0) return WASM_EXEC_TRAP_DIVIDE_BY_ZERO;
        r = a / b;
        break;
    case 0x6F:
        if (b == 0) return WASM_EXEC_TRAP_DIVIDE_BY_ZERO;
        r = b == UINT32_MAX ? 0 : (uint32_t)((int32_t)a % (int32_t)b);
        break;
    case 0x70:
        if (b == 0) return WASM_EXEC_TRAP_DIVIDE_BY_ZERO;
        r = a % b;
        break;
    case 0x71: r = a & b; break;
    case 0x72: r = a | b; break;
    case 0x73: r = a ^ b; break;
    case 0x74: r = a << (b & 31); break;
    case 0x75: r = (uint32_t)((int32_t)a >> (b & 31)); break;
    case 0x76: r = a >> (b & 31); break;
    case 0x77: r = (a << (b & 31)) | (a >> ((32 - (b & 31)) & 31)); break;
    case 0x78: r = (a >> (b & 31)) | (a << ((32 - (b & 31)) & 31)); break;
    default: CHECK_FAIL("an i32 binary opcode interpreter.c does not define");
    }
    return push(it, r) ? WASM_EXEC_MORE : WASM_EXEC_NO_ROOM;
}

static WasmExecStatus binop_i64(WasmInterpreter *it, uint32_t op)
{
    uint64_t b = pop(it), a = pop(it), r;

    switch (op) {
    case 0x51: r = a == b; break;
    case 0x52: r = a != b; break;
    case 0x53: r = (int64_t)a < (int64_t)b; break;
    case 0x54: r = a < b; break;
    case 0x55: r = (int64_t)a > (int64_t)b; break;
    case 0x56: r = a > b; break;
    case 0x57: r = (int64_t)a <= (int64_t)b; break;
    case 0x58: r = a <= b; break;
    case 0x59: r = (int64_t)a >= (int64_t)b; break;
    case 0x5A: r = a >= b; break;
    case 0x7C: r = a + b; break;
    case 0x7D: r = a - b; break;
    case 0x7E: r = a * b; break;
    case 0x7F:
        if (b == 0) return WASM_EXEC_TRAP_DIVIDE_BY_ZERO;
        if (a == (uint64_t)INT64_MIN && b == UINT64_MAX) return WASM_EXEC_TRAP_OVERFLOW;
        r = (uint64_t)((int64_t)a / (int64_t)b);
        break;
    case 0x80:
        if (b == 0) return WASM_EXEC_TRAP_DIVIDE_BY_ZERO;
        r = a / b;
        break;
    case 0x81:
        if (b == 0) return WASM_EXEC_TRAP_DIVIDE_BY_ZERO;
        r = b == UINT64_MAX ? 0 : (uint64_t)((int64_t)a % (int64_t)b);
        break;
    case 0x82:
        if (b == 0) return WASM_EXEC_TRAP_DIVIDE_BY_ZERO;
        r = a % b;
        break;
    case 0x83: r = a & b; break;
    case 0x84: r = a | b; break;
    case 0x85: r = a ^ b; break;
    case 0x86: r = a << (b & 63); break;
    case 0x87: r = (uint64_t)((int64_t)a >> (b & 63)); break;
    case 0x88: r = a >> (b & 63); break;
    case 0x89: r = (a << (b & 63)) | (a >> ((64 - (b & 63)) & 63)); break;
    case 0x8A: r = (a >> (b & 63)) | (a << ((64 - (b & 63)) & 63)); break;
    default: CHECK_FAIL("an i64 binary opcode interpreter.c does not define");
    }
    return push(it, r) ? WASM_EXEC_MORE : WASM_EXEC_NO_ROOM;
}

/* §4.3.3's operations at width N, computed in float for f32 and double for f64. */
static WasmExecStatus binop_f32(WasmInterpreter *it, uint32_t op)
{
    uint64_t bb = pop(it), ab = pop(it), r;
    float a = f32_of(ab), b = f32_of(bb);
    bool nan;
    double m;

    switch (op) {
    case 0x5B: r = a == b; break;
    case 0x5C: r = a != b; break;
    case 0x5D: r = a < b; break;
    case 0x5E: r = a > b; break;
    case 0x5F: r = a <= b; break;
    case 0x60: r = a >= b; break;
    case 0x92: r = f32_arith(a + b); break;
    case 0x93: r = f32_arith(a - b); break;
    case 0x94: r = f32_arith(a * b); break;
    case 0x95: r = f32_arith(a / b); break;
    case 0x96: m = fmin_wasm(a, b, &nan); r = nan ? F32_CANON : f32_bits((float)m); break;
    case 0x97: m = fmax_wasm(a, b, &nan); r = nan ? F32_CANON : f32_bits((float)m); break;
    case 0x98: r = (ab & 0x7FFFFFFFu) | (bb & 0x80000000u); break;
    default: CHECK_FAIL("an f32 binary opcode interpreter.c does not define");
    }
    return push(it, r) ? WASM_EXEC_MORE : WASM_EXEC_NO_ROOM;
}

static WasmExecStatus binop_f64(WasmInterpreter *it, uint32_t op)
{
    uint64_t bb = pop(it), ab = pop(it), r;
    double a = f64_of(ab), b = f64_of(bb), m;
    bool nan;

    switch (op) {
    case 0x61: r = a == b; break;
    case 0x62: r = a != b; break;
    case 0x63: r = a < b; break;
    case 0x64: r = a > b; break;
    case 0x65: r = a <= b; break;
    case 0x66: r = a >= b; break;
    case 0xA0: r = f64_arith(a + b); break;
    case 0xA1: r = f64_arith(a - b); break;
    case 0xA2: r = f64_arith(a * b); break;
    case 0xA3: r = f64_arith(a / b); break;
    case 0xA4: m = fmin_wasm(a, b, &nan); r = nan ? F64_CANON : f64_bits(m); break;
    case 0xA5: m = fmax_wasm(a, b, &nan); r = nan ? F64_CANON : f64_bits(m); break;
    case 0xA6: r = (ab & 0x7FFFFFFFFFFFFFFFull) | (bb & 0x8000000000000000ull); break;
    default: CHECK_FAIL("an f64 binary opcode interpreter.c does not define");
    }
    return push(it, r) ? WASM_EXEC_MORE : WASM_EXEC_NO_ROOM;
}

/* The one-operand numeric instructions: tests, unary operations and §4.3.4's conversions. */
static WasmExecStatus unop(WasmInterpreter *it, uint32_t op)
{
    uint64_t a = pop(it), r;
    WasmExecStatus st;
    double t;

    switch (op) {
    case 0x45: r = (uint32_t)a == 0; break;
    case 0x50: r = a == 0; break;
    case 0x67: r = clz32((uint32_t)a); break;
    case 0x68: r = ctz32((uint32_t)a); break;
    case 0x69: r = (uint32_t)__builtin_popcount((uint32_t)a); break;
    case 0x79: r = clz64(a); break;
    case 0x7A: r = ctz64(a); break;
    case 0x7B: r = (uint64_t)__builtin_popcountll(a); break;
    case 0x8B: r = a & 0x7FFFFFFFu; break;
    case 0x8C: r = a ^ 0x80000000u; break;
    case 0x8D: r = f32_arith(ceilf(f32_of(a))); break;
    case 0x8E: r = f32_arith(floorf(f32_of(a))); break;
    case 0x8F: r = f32_arith(truncf(f32_of(a))); break;
    case 0x90: r = f32_arith(rintf(f32_of(a))); break;
    case 0x91: r = f32_arith(sqrtf(f32_of(a))); break;
    case 0x99: r = a & 0x7FFFFFFFFFFFFFFFull; break;
    case 0x9A: r = a ^ 0x8000000000000000ull; break;
    case 0x9B: r = f64_arith(ceil(f64_of(a))); break;
    case 0x9C: r = f64_arith(floor(f64_of(a))); break;
    case 0x9D: r = f64_arith(trunc(f64_of(a))); break;
    case 0x9E: r = f64_arith(rint(f64_of(a))); break;
    case 0x9F: r = f64_arith(sqrt(f64_of(a))); break;
    case 0xA7: r = (uint32_t)a; break;
    case 0xA8: case 0xA9: case 0xAA: case 0xAB: case 0xAE: case 0xAF: case 0xB0: case 0xB1: {
        bool from32 = op == 0xA8 || op == 0xA9 || op == 0xAE || op == 0xAF;
        bool is_signed = op == 0xA8 || op == 0xAA || op == 0xAE || op == 0xB0, wide = op >= 0xAE;
        double x = from32 ? (double)f32_of(a) : f64_of(a);

        /* An unsigned trunc's bound below is 0, which a truncated value in (-1, 0), -0, meets. */
        st = trunc_check(x, is_signed ? (wide ? -TWO63 : -TWO31) : 0.0,
                         is_signed ? (wide ? TWO63 : TWO31) : (wide ? TWO64 : TWO32), &t);
        if (st != WASM_EXEC_DONE) return st;
        if (!wide) r = is_signed ? (uint32_t)(int32_t)t : (uint32_t)t;
        else r = is_signed ? (uint64_t)(int64_t)t : (uint64_t)t;
        break;
    }
    case 0xAC: r = (uint64_t)(int64_t)(int32_t)(uint32_t)a; break;
    case 0xAD: r = (uint32_t)a; break;
    case 0xB2: r = f32_bits((float)(int32_t)(uint32_t)a); break;
    case 0xB3: r = f32_bits((float)(uint32_t)a); break;
    case 0xB4: r = f32_bits((float)(int64_t)a); break;
    case 0xB5: r = f32_bits((float)a); break;
    case 0xB6: r = f32_arith((float)f64_of(a)); break;
    case 0xB7: r = f64_bits((double)(int32_t)(uint32_t)a); break;
    case 0xB8: r = f64_bits((double)(uint32_t)a); break;
    case 0xB9: r = f64_bits((double)(int64_t)a); break;
    case 0xBA: r = f64_bits((double)a); break;
    case 0xBB: r = f64_arith((double)f32_of(a)); break;
    case 0xBC: case 0xBE: r = (uint32_t)a; break;
    case 0xBD: case 0xBF: r = a; break;
    case 0xC0: r = (uint32_t)(int32_t)(int8_t)(uint8_t)a; break;
    case 0xC1: r = (uint32_t)(int32_t)(int16_t)(uint16_t)a; break;
    case 0xC2: r = (uint64_t)(int64_t)(int8_t)(uint8_t)a; break;
    case 0xC3: r = (uint64_t)(int64_t)(int16_t)(uint16_t)a; break;
    case 0xC4: r = (uint64_t)(int64_t)(int32_t)(uint32_t)a; break;
    default: CHECK_FAIL("a one-operand numeric opcode interpreter.c does not define");
    }
    return push(it, r) ? WASM_EXEC_MORE : WASM_EXEC_NO_ROOM;
}

/* §4.6.3's block, loop and if, and §4.6.2's else and end. */
static WasmExecStatus structured(WasmInterpreter *it, const WasmInstruction *in, size_t at)
{
    const WasmModule *m = IM(it);
    size_t e = block_entry(it, at), height;
    uint32_t params = 0, results = 0;
    const WasmBlockEntry *b;

    if (in->block_kind == WASM_BLOCK_VALUE) results = 1;
    if (in->block_kind == WASM_BLOCK_INDEX) {
        params = (uint32_t)m->types[in->block_index].n_params;
        results = (uint32_t)m->types[in->block_index].n_results;
    }
    b = &IS(it)->blocks[e];
    height = frame(it)->val_base + b->height;
    if (in->opcode == 0x04 && pop(it) == 0) {
        /* The false arm: past the else, or straight to the end, whose label pop this push is for. */
        it->pc = b->else_at != 0 ? b->else_at + 1 : b->end_at;
    }
    DCHECK(it->n_vals == height + params, "a WebAssembly block's entry height is not the side table's");
    if (!push_label(it, in->opcode == 0x03 ? L_LOOP : in->opcode == 0x04 ? L_IF : L_BLOCK, e, height,
                    in->opcode == 0x03 ? params : results, in->opcode == 0x03 ? it->pc : b->end_at + 1))
        return WASM_EXEC_NO_ROOM;
    return WASM_EXEC_MORE;
}

/* §4.6.2's br_table: the label at the operand's index, or the default past the list. The labels it reads past
   are host work in the opcode unit. */
static WasmExecStatus br_table(WasmInterpreter *it, const WasmInstruction *in)
{
    WasmDecoder r = { IB(it), frame(it)->end, it->pc };
    uint32_t i = (uint32_t)pop(it), k, l = 0, target = 0;

    for (k = 0; k <= in->list_count; k++) {
        CHECK(wasm_read_label(&r, &l) == WASM_DECODE_DONE, "a br_table label the decoder accepted did not read");
        if (k == i || k == in->list_count) {
            target = l;
            break;
        }
    }
    JS_FlowRetireWork(k);
    it->retired += k;
    return branch(it, target);
}

static WasmExecStatus local(WasmInterpreter *it, uint32_t op, uint32_t x)
{
    WasmFrame *f = frame(it);

    DCHECK(x < f->n_locals, "a validated WebAssembly local index is past the frame's locals");
    if (op == 0x20) return push(it, f->locals[x].lo) ? WASM_EXEC_MORE : WASM_EXEC_NO_ROOM;
    DCHECK(it->n_vals > 0, "a validated WebAssembly local.set popped an empty operand stack");
    f->locals[x] = it->vals[it->n_vals - 1];
    if (op == 0x21) it->n_vals--;
    return WASM_EXEC_MORE;
}

/* §4.6.6's global.get and global.set, of the global instance at the index's store address. */
static WasmExecStatus global(WasmInterpreter *it, uint32_t op, uint32_t x)
{
    size_t a = INST(it)->globals[x];

    DCHECKF(a < it->store->n_globals, "a WebAssembly instance's global %u has no store address", x);
    if (op == 0x23) return push(it, it->store->globals[a].value.lo) ? WASM_EXEC_MORE : WASM_EXEC_NO_ROOM;
    it->store->globals[a].value.lo = pop(it);
    return WASM_EXEC_MORE;
}

/* §4.6.5's call and return_call of a defined function. A tail call returns from the current frame first, so
   its arguments replace the caller's frame and the callee returns to the caller's caller. */
static WasmExecStatus call(WasmInterpreter *it, size_t func, bool tail)
{
    const WasmFuncInstance *fi = &it->store->funcs[func];
    size_t return_pc = it->pc;

    if (tail) {
        WasmFrame *f = frame(it);
        size_t n = it->store->types[fi->types]->module->types[fi->type].n_params;

        DCHECK(it->n_vals >= f->val_base + n, "a validated WebAssembly return_call has fewer operands than "
                                              "its callee's parameters");
        if (n > 0) memmove(it->vals + f->val_base, it->vals + it->n_vals - n, n * sizeof *it->vals);
        it->n_vals = f->val_base + n;
        return_pc = f->return_pc;
        it->n_labels = f->label_base;
        free(f->locals);
        it->n_frames--;
    }
    return push_frame(it, func, return_pc);
}

/* The store address of table `x` of the current instance, and its table instance. */
static size_t table_addr(WasmInterpreter *it, uint64_t x)
{
    size_t a = INST(it)->tables[x];

    DCHECKF(a < it->store->n_tables, "a WebAssembly instance's table %llu has no store address",
            (unsigned long long)x);
    return a;
}

static WasmTableInstance *table_of(WasmInterpreter *it, uint64_t x)
{
    return &it->store->tables[table_addr(it, x)];
}

/* §4.6.5's call_indirect and return_call_indirect (type, then table): the element at the operand's index must
   exist, be a function, and match the expected type by §3.3.10's walk from its own, over its own instance's
   module (one supertype a unit of work); the function may be another instance's. */
static WasmExecStatus call_indirect(WasmInterpreter *it, const WasmInstruction *in)
{
    WasmTableInstance *t = table_of(it, in->index[1]);
    uint64_t i = pop(it), f;
    const WasmFuncInstance *fi;
    const WasmTypes *callee;
    WasmClimb climb;
    WasmValType a = { 0 }, b = { 0 };
    WasmMatch le;

    it->trap_index = i;
    if (i >= t->size) return WASM_EXEC_TRAP_UNDEFINED_ELEMENT;
    if (t->elems[i].lo == 0) return WASM_EXEC_TRAP_UNINITIALIZED;
    f = t->elems[i].lo - 1;
    fi = &it->store->funcs[f];
    callee = it->store->types[fi->types];
    a.code = b.code = WASM_TYPE_REF;
    a.heap_index = fi->type;
    b.heap_index = in->index[0];
    le = wasm_heap_le(callee, &a, INST(it)->types, &b, &climb);
    while (le == WASM_MATCH_CLIMB) {
        JS_FlowRetireWork(1);
        it->retired++;
        le = wasm_climb(callee, &climb);
    }
    if (le != WASM_MATCH_YES) return WASM_EXEC_TRAP_INDIRECT_TYPE;
    return call(it, (size_t)f, in->opcode == 0x13);
}

/* §4.6.7's table.get and table.set. */
static WasmExecStatus table_access(WasmInterpreter *it, const WasmInstruction *in)
{
    WasmTableInstance *t = table_of(it, in->index[0]);
    uint64_t r = in->opcode == 0x26 ? pop(it) : 0, i = pop(it);

    if (i >= t->size) return WASM_EXEC_TRAP_TABLE_BOUNDS;
    if (in->opcode == 0x26) {
        t->elems[i].lo = r;
        return WASM_EXEC_MORE;
    }
    return push(it, t->elems[i].lo) ? WASM_EXEC_MORE : WASM_EXEC_NO_ROOM;
}

/* §4.6.7's table.size, table.grow, elem.drop, and table.fill, table.copy and table.init, whose bounds are
   checked here and whose elements then move one a step (WasmBulk). A grow past the maximum, or past what can be
   allocated, answers -1 (§4.7.1's growtable fails). */
static WasmExecStatus table_bulk(WasmInterpreter *it, const WasmInstruction *in)
{
    WasmBulk *b = &it->bulk;
    WasmTableInstance *t, *u;
    WasmElemInstance *e;
    uint64_t n, s, d, bound;

    switch (in->opcode) {
    case 13:
        it->store->elems[INST(it)->elems[in->index[0]]].n = 0;
        return WASM_EXEC_MORE;
    case 16:
        return push(it, table_of(it, in->index[0])->size) ? WASM_EXEC_MORE : WASM_EXEC_NO_ROOM;
    case 15:
        /* §4.7.1's growtable either succeeds or fails at the implementation's choice, which is an unknown of
           the environment: the step answers FORK with the success arm as the example (wasm_interpreter_choose).
           Past the maximum, past the address type, or past what a size_t can count, only failure exists. */
        t = table_of(it, in->index[0]);
        n = pop(it);
        b->ref.lo = pop(it);
        bound = t->type.addr64 ? UINT64_MAX : UINT32_MAX;
        if (n > bound - t->size || (t->type.has_max && t->size + n > t->type.max)
            || t->size + n > SIZE_MAX / sizeof *t->elems)
            return push(it, bound) ? WASM_EXEC_MORE : WASM_EXEC_NO_ROOM;
        it->choice.kind = WASM_CHOICE_TABLE_GROW;
        it->choice.table = table_addr(it, in->index[0]);
        it->choice.n = n;
        return WASM_EXEC_FORK;
    case 17:
        t = table_of(it, in->index[0]);
        n = pop(it);
        b->ref.lo = pop(it);
        d = pop(it);
        if (d > t->size || n > t->size - d) return WASM_EXEC_TRAP_TABLE_BOUNDS;
        b->op = 17;
        b->table = table_addr(it, in->index[0]);
        b->d = d;
        b->n = n;
        return WASM_EXEC_MORE;
    case 14:
        t = table_of(it, in->index[0]);
        u = table_of(it, in->index[1]);
        n = pop(it);
        s = pop(it);
        d = pop(it);
        if (d > t->size || n > t->size - d || s > u->size || n > u->size - s) return WASM_EXEC_TRAP_TABLE_BOUNDS;
        b->op = 14;
        b->table = table_addr(it, in->index[0]);
        b->from = table_addr(it, in->index[1]);
        b->d = d;
        b->s = s;
        b->n = n;
        return WASM_EXEC_MORE;
    case 12:
        /* table.init's immediates are the element segment, then the table. */
        t = table_of(it, in->index[1]);
        e = &it->store->elems[INST(it)->elems[in->index[0]]];
        n = pop(it);
        s = pop(it);
        d = pop(it);
        if (d > t->size || n > t->size - d || s > e->n || n > e->n - s) return WASM_EXEC_TRAP_TABLE_BOUNDS;
        b->op = 12;
        b->table = table_addr(it, in->index[1]);
        b->from = INST(it)->elems[in->index[0]];
        b->d = d;
        b->s = s;
        b->n = n;
        return WASM_EXEC_MORE;
    default: break;
    }
    CHECK_FAIL("a 0xFC table opcode interpreter.c does not define");
}

WasmExecStatus wasm_interpreter_choose(WasmInterpreter *it, bool example)
{
    WasmChoice c = it->choice;
    WasmTableInstance *t;
    WasmBulk *b = &it->bulk;
    WasmValue *grown;

    DCHECK(c.kind == WASM_CHOICE_TABLE_GROW, "a WebAssembly choice was made with none pending");
    it->choice.kind = 0;
    t = &it->store->tables[c.table];
    if (!example) return push(it, t->type.addr64 ? UINT64_MAX : UINT32_MAX) ? WASM_EXEC_MORE : WASM_EXEC_NO_ROOM;
    if (t->size + c.n > t->cap) {
        grown = realloc(t->elems, (size_t)(t->size + c.n ? t->size + c.n : 1) * sizeof *t->elems);
        CHECK(grown != NULL, "a WebAssembly table could not grow: out of memory");
        t->elems = grown;
        t->cap = t->size + c.n;
    }
    if (!push(it, t->size)) return WASM_EXEC_NO_ROOM;
    b->op = 15;
    b->table = c.table;
    b->d = t->size;
    b->n = c.n;
    t->size += c.n;
    return WASM_EXEC_MORE;
}

/* One element of the bulk table instruction in progress. table.copy moves backward when the destination lies
   above the source, as §4.6.7's d > s arm does. */
static WasmExecStatus bulk_step(WasmInterpreter *it)
{
    WasmBulk *b = &it->bulk;
    WasmTableInstance *t = &it->store->tables[b->table];
    uint64_t k;

    switch (b->op) {
    case 15: case 17:
        t->elems[b->d++] = b->ref;
        break;
    case 14: {
        WasmTableInstance *u = &it->store->tables[b->from];

        if (b->d <= b->s) t->elems[b->d++] = u->elems[b->s++];
        else {
            k = b->n - 1;
            t->elems[b->d + k] = u->elems[b->s + k];
        }
        break;
    }
    case 12:
        t->elems[b->d++] = it->store->elems[b->from].refs[b->s++];
        break;
    default:
        CHECK_FAIL("a WebAssembly bulk table operation interpreter.c does not define");
    }
    b->n--;
    return WASM_EXEC_MORE;
}

static WasmExecStatus execute(WasmInterpreter *it, const WasmInstruction *in, size_t at)
{
    uint64_t c, b, a;
    uint32_t x;

    if (in->prefix == 0xFC && in->opcode >= 12 && in->opcode <= 17) return table_bulk(it, in);
    if (in->prefix == 0xFC) {
        bool from32 = in->opcode == 0 || in->opcode == 1 || in->opcode == 4 || in->opcode == 5;

        CHECKF(in->opcode <= 7, "the WebAssembly instruction 0xFC %u is not built in interpreter.c", in->opcode);
        a = pop(it);
        return push(it, trunc_sat(from32 ? (double)f32_of(a) : f64_of(a), (in->opcode & 1) == 0, in->opcode >= 4))
               ? WASM_EXEC_MORE : WASM_EXEC_NO_ROOM;
    }
    CHECKF(in->prefix == 0, "the WebAssembly instruction 0x%02X %u is not built in interpreter.c", in->prefix,
           in->opcode);
    switch (in->opcode) {
    case 0x00: return WASM_EXEC_TRAP_UNREACHABLE;
    case 0x01: return WASM_EXEC_MORE;
    case 0x02: case 0x03: case 0x04: return structured(it, in, at);
    case 0x0C: return branch(it, in->index[0]);
    case 0x0D: return pop(it) != 0 ? branch(it, in->index[0]) : WASM_EXEC_MORE;
    case 0x0E: return br_table(it, in);
    case 0x0F: return branch(it, it->n_labels - 1 - frame(it)->label_base);
    case 0x10: case 0x12: return call(it, func_addr(it, in->index[0]), in->opcode == 0x12);
    case 0x11: case 0x13: return call_indirect(it, in);
    case 0x14: case 0x15:
        a = pop(it);
        if (a == 0) return WASM_EXEC_TRAP_NULL_FUNCTION;
        return call(it, (size_t)(a - 1), in->opcode == 0x15);
    case 0x25: case 0x26: return table_access(it, in);
    case 0xD0: return push(it, 0) ? WASM_EXEC_MORE : WASM_EXEC_NO_ROOM;
    case 0xD1: return push(it, pop(it) == 0) ? WASM_EXEC_MORE : WASM_EXEC_NO_ROOM;
    case 0xD2: return push(it, (uint64_t)func_addr(it, in->index[0]) + 1) ? WASM_EXEC_MORE : WASM_EXEC_NO_ROOM;
    case 0xD4:
        if (it->vals[it->n_vals - 1].lo == 0) return WASM_EXEC_TRAP_NULL_REFERENCE;
        return WASM_EXEC_MORE;
    case 0xD5:
        a = pop(it);
        if (a == 0) return branch(it, in->index[0]);
        return push(it, a) ? WASM_EXEC_MORE : WASM_EXEC_NO_ROOM;
    case 0xD6:
        a = pop(it);
        if (a == 0) return WASM_EXEC_MORE;
        if (!push(it, a)) return WASM_EXEC_NO_ROOM;
        return branch(it, in->index[0]);
    case 0x1A: pop(it); return WASM_EXEC_MORE;
    case 0x1B: case 0x1C:
        if (in->opcode == 0x1C) {
            WasmDecoder r = { IB(it), frame(it)->end, it->pc };
            WasmValType t;

            CHECK(wasm_read_val_type(&r, &t) == WASM_DECODE_DONE, "a select type the decoder accepted did not read");
            it->pc = r.pos;
        }
        c = pop(it);
        b = pop(it);
        a = pop(it);
        return push(it, c != 0 ? a : b) ? WASM_EXEC_MORE : WASM_EXEC_NO_ROOM;
    case 0x20: case 0x21: case 0x22: return local(it, in->opcode, in->index[0]);
    case 0x23: case 0x24: return global(it, in->opcode, in->index[0]);
    case 0x41: return push(it, (uint32_t)(int32_t)in->value) ? WASM_EXEC_MORE : WASM_EXEC_NO_ROOM;
    case 0x42: return push(it, (uint64_t)in->value) ? WASM_EXEC_MORE : WASM_EXEC_NO_ROOM;
    case 0x43:
        x = (uint32_t)IB(it)[in->bytes] | (uint32_t)IB(it)[in->bytes + 1] << 8
          | (uint32_t)IB(it)[in->bytes + 2] << 16 | (uint32_t)IB(it)[in->bytes + 3] << 24;
        return push(it, x) ? WASM_EXEC_MORE : WASM_EXEC_NO_ROOM;
    case 0x44: {
        uint64_t v = 0;
        int i;

        for (i = 7; i >= 0; i--) v = v << 8 | IB(it)[in->bytes + i];
        return push(it, v) ? WASM_EXEC_MORE : WASM_EXEC_NO_ROOM;
    }
    default: break;
    }
    if ((in->opcode >= 0x46 && in->opcode <= 0x4F) || (in->opcode >= 0x6A && in->opcode <= 0x78))
        return binop_i32(it, in->opcode);
    if ((in->opcode >= 0x51 && in->opcode <= 0x5A) || (in->opcode >= 0x7C && in->opcode <= 0x8A))
        return binop_i64(it, in->opcode);
    if ((in->opcode >= 0x5B && in->opcode <= 0x60) || (in->opcode >= 0x92 && in->opcode <= 0x98))
        return binop_f32(it, in->opcode);
    if ((in->opcode >= 0x61 && in->opcode <= 0x66) || (in->opcode >= 0xA0 && in->opcode <= 0xA6))
        return binop_f64(it, in->opcode);
    CHECKF(in->opcode >= 0x45 && in->opcode <= 0xC4, "the WebAssembly instruction 0x%02X is not built in "
           "interpreter.c", in->opcode);
    return unop(it, in->opcode);
}

/* A trap ends the invocation as a return does: no frame is left to step. A FORK leaves it standing. */
static WasmExecStatus finish(WasmInterpreter *it, WasmExecStatus st)
{
    if (st != WASM_EXEC_MORE && st != WASM_EXEC_FORK && st != WASM_EXEC_HOST_CALL)
        while (it->n_frames > 0) free(it->frames[--it->n_frames].locals);
    return st;
}

WasmExecStatus wasm_interpreter_return(WasmInterpreter *it, const WasmValue *results, size_t n)
{
    size_t i;

    DCHECK(it != NULL && it->host.pending, "a WebAssembly host call was answered with none pending");
    DCHECKF(n == it->host.n_results && (n == 0 || results != NULL), "a WebAssembly host call of %u results was "
            "answered with %zu", it->host.n_results, n);
    DCHECK(it->n_vals >= it->host.n_args, "a WebAssembly host call's arguments left the stack before its answer");
    it->host.pending = false;
    it->n_vals -= it->host.n_args;
    for (i = 0; i < n; i++) {
        if (!grow((void **)&it->vals, &it->cap_vals, it->n_vals, sizeof *it->vals))
            return finish(it, WASM_EXEC_NO_ROOM);
        it->vals[it->n_vals++] = results[i];
    }
    return it->n_frames == 0 ? WASM_EXEC_DONE : WASM_EXEC_MORE;
}

WasmExecStatus wasm_interpreter_step(WasmInterpreter *it)
{
    WasmInstruction in;
    uint32_t prefix, op;
    size_t at;
    uint8_t b;
    WasmDecoder r;

    DCHECK(it != NULL && it->n_frames > 0, "a WebAssembly invocation was stepped after it answered");
    DCHECK(it->choice.kind == 0, "a WebAssembly invocation was stepped with a choice pending");
    DCHECK(!it->host.pending, "a WebAssembly invocation was stepped with a host call pending");
    JS_FlowRetireWork(1);
    it->retired++;
    if (it->bulk.n > 0) return bulk_step(it);
    if (it->pc == frame(it)->end)   /* the body's own end returns */
        return finish(it, branch(it, it->n_labels - 1 - frame(it)->label_base));
    at = it->pc;
    b = IB(it)[at];
    if (b == 0x0B) {
        /* An end inside the body closes its innermost block; the values above its height are its results. */
        DCHECK(it->n_labels > frame(it)->label_base + 1, "a WebAssembly end inside a body closes no block, which "
                                                         "validation rules out");
        it->n_labels--;
        it->pc++;
        return WASM_EXEC_MORE;
    }
    if (b == 0x05) {
        /* The true arm finished: on to the if's end, which pops its label. */
        DCHECK(it->labels[it->n_labels - 1].kind == L_IF, "a WebAssembly else reached outside an if's true arm");
        it->pc = IS(it)->blocks[it->labels[it->n_labels - 1].entry].end_at;
        return WASM_EXEC_MORE;
    }
    r.in = IB(it);
    r.len = frame(it)->end;
    r.pos = at;
    CHECK(wasm_read_opcode(&r, &prefix, &op) == WASM_DECODE_DONE
          && wasm_read_immediates(&r, prefix, op, &in) == WASM_DECODE_DONE,
          "an instruction module_decoder.c accepted did not read again");
    it->pc = r.pos;
    return finish(it, execute(it, &in, at));
}

const char *wasm_exec_status_message(WasmExecStatus st)
{
    switch (st) {
    case WASM_EXEC_DONE:
    case WASM_EXEC_MORE:
    case WASM_EXEC_FORK:
    case WASM_EXEC_HOST_CALL:           return NULL;
    case WASM_EXEC_TRAP_UNREACHABLE:    return "unreachable executed";
    case WASM_EXEC_TRAP_DIVIDE_BY_ZERO: return "integer divide by zero";
    case WASM_EXEC_TRAP_OVERFLOW:       return "integer overflow";
    case WASM_EXEC_TRAP_CONVERSION:     return "invalid conversion to integer";
    case WASM_EXEC_TRAP_TABLE_BOUNDS:   return "out of bounds table access";
    case WASM_EXEC_TRAP_UNDEFINED_ELEMENT: return "undefined element";
    case WASM_EXEC_TRAP_UNINITIALIZED:  return "uninitialized element";
    case WASM_EXEC_TRAP_INDIRECT_TYPE:  return "indirect call type mismatch";
    case WASM_EXEC_TRAP_NULL_FUNCTION:  return "null function reference";
    case WASM_EXEC_TRAP_NULL_REFERENCE: return "null reference";
    case WASM_EXEC_NO_ROOM:             return "call stack exhausted";
    case WASM_EXEC_LINK_INCOMPATIBLE:   return "incompatible import type";
    }
    CHECK_FAIL("a WebAssembly execution status interpreter.c does not define");
}

void wasm_interpreter_message(const WasmInterpreter *it, WasmExecStatus st, char *buf, size_t cap)
{
    const char *m = wasm_exec_status_message(st);

    DCHECK(it != NULL && buf != NULL && cap > 0, "a WebAssembly trap's message was asked with nowhere to write it");
    if (st == WASM_EXEC_TRAP_UNDEFINED_ELEMENT || st == WASM_EXEC_TRAP_UNINITIALIZED)
        snprintf(buf, cap, "%s %llu", m, (unsigned long long)it->trap_index);
    else snprintf(buf, cap, "%s", m != NULL ? m : "");
}

void wasm_interpreter_free(WasmInterpreter *it)
{
    DCHECK(it != NULL, "a WebAssembly invocation was freed with no interpreter");
    while (it->n_frames > 0) free(it->frames[--it->n_frames].locals);
    free(it->frames);
    free(it->vals);
    free(it->labels);
    memset(it, 0, sizeof *it);
}
