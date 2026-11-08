/* WebAssembly Core Specification (Release 3.0) §3.4 "Instructions" by §7.6's algorithm. See code_validator.h. */
#include "core/wasm/code_validator.h"

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/wasm/opcode_signatures.h"

enum { CV_IDLE = 0, CV_LOCALS, CV_CONST, CV_TYPE, CV_BR_SKIP, CV_BR_CHECK, CV_CATCH, CV_END };

/* §7.6.1's frame opcodes: the function's (or a constant expression's) own frame, and each structured one. */
enum { F_FUNC = 1, F_BLOCK, F_LOOP, F_IF, F_ELSE, F_TRY, F_CATCH };

/* The stack operations. POP, PUSH and PEEK run one element of `run` a step (PEEK matches the operand at that
   depth without popping it); MATCH matches `run`'s elements against `run2`'s, refusing with `fail`;
   DEFAULTABLE refuses with `fail` a run element that has no default. The rest are one step each. */
enum {
    M_POP = 1, M_PUSH, M_PEEK, M_MATCH, M_DEFAULTABLE, M_FRAME, M_REFRAME, M_END_CHECK, M_RESET, M_DROP_FRAME,
    M_UNREACHABLE, M_SET_LOCAL
};

enum { RUN_ONE = 0, RUN_POOL, RUN_FIELDS };

/* §7.6.1's Bot: a value type of code 0, and a reference whose heap type is Bot (as pop_ref answers it). */
#define WASM_TYPE_BOT 0
#define WASM_HEAP_BOT 0xFF

void wasm_code_validator_init(WasmCodeValidator *cv, const WasmContext *ctx)
{
    DCHECK(cv != NULL && ctx != NULL, "a WebAssembly code validator was begun without its context");
    memset(cv, 0, sizeof *cv);
    cv->ctx = ctx;
    cv->phase = CV_IDLE;
}

static void begin(WasmCodeValidator *cv, size_t offset, size_t size)
{
    DCHECK(cv->phase == CV_IDLE, "a WebAssembly expr was begun while another was being validated");
    cv->r.in = cv->ctx->bytes;
    cv->r.pos = offset;
    cv->r.len = offset + size;
    cv->start = offset;
    cv->n_vals = cv->n_ctrls = cv->n_inits = 0;
    cv->used_init_set = 0;
    if (cv->init_set != NULL) memset(cv->init_set, 0, cv->cap_init_set * sizeof *cv->init_set);
    cv->n_prog = cv->pc = 0;
    cv->k = 0;
    cv->climbing = false;
    cv->item = 0;
}

void wasm_code_validator_begin_body(WasmCodeValidator *cv, size_t code)
{
    const WasmModule *m = cv->ctx->types.module;

    DCHECK(code < m->n_codes && code < m->n_func_types, "a WebAssembly body was begun past the code entries");
    begin(cv, m->codes[code].body_offset, m->codes[code].body_size);
    cv->constant = false;
    cv->code = code;
    cv->func_type = m->func_types[code];
    cv->phase = CV_LOCALS;
}

void wasm_code_validator_begin_const(WasmCodeValidator *cv, WasmExpr e, const WasmValType *expect,
                                     size_t visible_globals)
{
    begin(cv, e.offset, e.size);
    cv->constant = true;
    cv->expect = *expect;
    cv->visible_globals = visible_globals;
    cv->phase = CV_CONST;
}

/* Runs. */
static WasmTypeRun run_none(void)
{
    WasmTypeRun r = { 0 };

    return r;
}

static WasmTypeRun run_rep(WasmValType t, uint64_t n)
{
    WasmTypeRun r = { 0 };

    r.kind = RUN_ONE;
    r.one = t;
    r.n = n;
    return r;
}

static WasmTypeRun run_one(WasmValType t)
{
    return run_rep(t, 1);
}

static WasmTypeRun run_pool(size_t off, uint64_t n)
{
    WasmTypeRun r = { 0 };

    r.kind = RUN_POOL;
    r.off = off;
    r.n = n;
    return r;
}

static WasmValType unpack(WasmValType t)
{
    if (t.code == WASM_TYPE_I8 || t.code == WASM_TYPE_I16) t.code = WASM_TYPE_I32;
    return t;
}

static WasmValType run_elem(const WasmCodeValidator *cv, const WasmTypeRun *r, uint64_t i)
{
    const WasmModule *m = cv->ctx->types.module;

    DCHECK(i < r->n, "a WebAssembly type run was read past its length");
    switch (r->kind) {
    case RUN_ONE: return r->one;
    case RUN_POOL: return m->val_types[r->off + i];
    case RUN_FIELDS: return unpack(m->fields[r->off + i].storage);
    default: break;
    }
    CHECK_FAIL("a WebAssembly type run of a kind code_validator.c does not define");
}

static WasmValType val(uint8_t code)
{
    WasmValType t = { 0 };

    t.code = code;
    return t;
}

static WasmValType ref(bool nullable, uint8_t heap, uint32_t index)
{
    WasmValType t = { 0 };

    t.code = WASM_TYPE_REF;
    t.nullable = nullable;
    t.heap = heap;
    t.heap_index = heap == 0 ? index : 0;
    return t;
}

static WasmValType addr(const WasmLimits *l)
{
    return val(l->addr64 ? WASM_TYPE_I64 : WASM_TYPE_I32);
}

/* §3.2.5's defaultable: a number, a vector or a nullable reference. */
static bool defaultable(const WasmValType *t)
{
    return t->code != WASM_TYPE_REF || t->nullable;
}

/* Refusals. */
static WasmValidateStatus unknown(WasmCodeValidator *cv, const char *space, uint64_t x)
{
    cv->space = space;
    cv->index = x;
    return WASM_VALIDATE_UNKNOWN_INDEX;
}

static WasmValidateStatus mismatch(WasmCodeValidator *cv, const WasmValType *want, const WasmValType *got)
{
    cv->want = want != NULL ? *want : val(WASM_TYPE_BOT);
    cv->has_got = got != NULL;
    if (got != NULL) cv->got = *got;
    return WASM_VALIDATE_TYPE_MISMATCH;
}

static WasmValidateStatus refuse(WasmCodeValidator *cv, WasmValidateStatus st, uint64_t index)
{
    cv->index = index;
    return st;
}

/* Stacks. Their sizes are the input's, so a failed allocation is the NO_ROOM refusal. */
static bool grow(void **p, size_t *cap, size_t n, size_t elem)
{
    size_t c;
    void *q;

    if (n < *cap) return true;
    c = *cap ? *cap * 2 : 16;
    if (c > SIZE_MAX / elem) return false;
    q = realloc(*p, c * elem);
    if (q == NULL) return false;
    *p = q;
    *cap = c;
    return true;
}

static WasmCtrlFrame *top(WasmCodeValidator *cv)
{
    DCHECK(cv->n_ctrls > 0, "§7.6 typed an instruction with no control frame, which the function's own frame "
                            "rules out");
    return &cv->ctrls[cv->n_ctrls - 1];
}

static WasmValidateStatus push_val(WasmCodeValidator *cv, WasmValType t)
{
    if (!grow((void **)&cv->vals, &cv->cap_vals, cv->n_vals, sizeof *cv->vals)) return WASM_VALIDATE_NO_ROOM;
    cv->vals[cv->n_vals++] = t;
    return WASM_VALIDATE_DONE;
}

static WasmValidateStatus push_ctrl(WasmCodeValidator *cv, uint8_t kind, WasmTypeRun start, WasmTypeRun end)
{
    WasmCtrlFrame *f;

    if (!grow((void **)&cv->ctrls, &cv->cap_ctrls, cv->n_ctrls, sizeof *cv->ctrls)) return WASM_VALIDATE_NO_ROOM;
    f = &cv->ctrls[cv->n_ctrls++];
    f->kind = kind;
    f->start = start;
    f->end = end;
    f->val_height = cv->n_vals;
    f->init_height = cv->n_inits;
    f->unreachable = false;
    return WASM_VALIDATE_DONE;
}

/* The operand `i` below the top within the current frame, or Bot past its height. */
static WasmValType peek(WasmCodeValidator *cv, uint64_t i)
{
    WasmCtrlFrame *f = top(cv);

    return cv->n_vals - f->val_height > i ? cv->vals[cv->n_vals - 1 - i] : val(WASM_TYPE_BOT);
}

/* valid.ml's peek_ref: the top operand as a reference, Bot as (ref bot). */
static WasmValidateStatus peek_ref(WasmCodeValidator *cv, WasmValType *out)
{
    WasmValType t = peek(cv, 0);

    if (t.code == WASM_TYPE_BOT) {
        *out = ref(false, WASM_HEAP_BOT, 0);
        return WASM_VALIDATE_DONE;
    }
    if (t.code != WASM_TYPE_REF) return mismatch(cv, NULL, &t);
    *out = t;
    return WASM_VALIDATE_DONE;
}

/* The locals' initialization set (§7.6.1's locals_init for the non-defaultable locals). */
#define INIT_GONE UINT64_MAX

static bool init_has(const WasmCodeValidator *cv, uint32_t x)
{
    size_t at;

    if (cv->cap_init_set == 0) return false;
    for (at = (size_t)(x * 0x9E3779B97F4A7C15u) & (cv->cap_init_set - 1); cv->init_set[at] != 0;
         at = (at + 1) & (cv->cap_init_set - 1))
        if (cv->init_set[at] == (uint64_t)x + 1) return true;
    return false;
}

static bool init_insert(uint64_t *set, size_t cap, uint64_t key)
{
    size_t at = (size_t)((key - 1) * 0x9E3779B97F4A7C15u) & (cap - 1);

    while (set[at] != 0 && set[at] != INIT_GONE) at = (at + 1) & (cap - 1);
    set[at] = key;
    return true;
}

static WasmValidateStatus init_add(WasmCodeValidator *cv, uint32_t x)
{
    if ((cv->used_init_set + 1) * 2 > cv->cap_init_set) {
        size_t cap = cv->cap_init_set ? cv->cap_init_set * 2 : 16, i;
        uint64_t *set;

        if (cap > SIZE_MAX / sizeof *set) return WASM_VALIDATE_NO_ROOM;
        set = calloc(cap, sizeof *set);
        if (set == NULL) return WASM_VALIDATE_NO_ROOM;
        cv->used_init_set = 0;
        for (i = 0; i < cv->cap_init_set; i++) {
            if (cv->init_set[i] == 0 || cv->init_set[i] == INIT_GONE) continue;
            init_insert(set, cap, cv->init_set[i]);
            cv->used_init_set++;
        }
        free(cv->init_set);
        cv->init_set = set;
        cv->cap_init_set = cap;
    }
    if (!grow((void **)&cv->inits, &cv->cap_inits, cv->n_inits, sizeof *cv->inits)) return WASM_VALIDATE_NO_ROOM;
    cv->inits[cv->n_inits++] = x;
    init_insert(cv->init_set, cv->cap_init_set, (uint64_t)x + 1);
    cv->used_init_set++;
    return WASM_VALIDATE_DONE;
}

static void init_remove(WasmCodeValidator *cv, uint32_t x)
{
    size_t at;

    for (at = (size_t)(x * 0x9E3779B97F4A7C15u) & (cv->cap_init_set - 1); cv->init_set[at] != 0;
         at = (at + 1) & (cv->cap_init_set - 1)) {
        if (cv->init_set[at] == (uint64_t)x + 1) {
            cv->init_set[at] = INIT_GONE;
            return;
        }
    }
    DFAIL("a WebAssembly local on the initialization stack is missing from its set");
}

/* §3.4.3's local x of the body: a parameter, then the locals entries' runs, found by their ends. `*track` is
   whether its initialization is tracked (a non-defaultable declared local). */
static WasmValidateStatus local(WasmCodeValidator *cv, uint64_t x, WasmValType *t, bool *track)
{
    const WasmModule *m = cv->ctx->types.module;
    const WasmSubType *ft = &m->types[cv->func_type];
    const WasmCode *c = &m->codes[cv->code];
    size_t lo = 0, hi = c->n_locals;

    if (x < ft->n_params) {
        *t = m->val_types[ft->params + x];
        *track = false;
        return WASM_VALIDATE_DONE;
    }
    if (c->n_locals == 0 || x >= cv->local_ends[c->n_locals - 1]) return unknown(cv, "local", x);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;

        if (cv->local_ends[mid] > x) hi = mid;
        else lo = mid + 1;
    }
    *t = m->locals[c->locals + lo].type;
    *track = !defaultable(t);
    return WASM_VALIDATE_DONE;
}

/* The program. */
static WasmStackOp *emit(WasmCodeValidator *cv, uint8_t op)
{
    WasmStackOp *o;

    DCHECK(cv->n_prog < WASM_CODE_PROGRAM, "a WebAssembly typing rule's program outgrew WASM_CODE_PROGRAM");
    o = &cv->prog[cv->n_prog++];
    memset(o, 0, sizeof *o);
    o->op = op;
    o->fail = WASM_VALIDATE_TYPE_MISMATCH;
    return o;
}

static void op_pop(WasmCodeValidator *cv, WasmTypeRun r) { emit(cv, M_POP)->run = r; }
static void op_push(WasmCodeValidator *cv, WasmTypeRun r) { emit(cv, M_PUSH)->run = r; }
static void op_pop1(WasmCodeValidator *cv, WasmValType t) { op_pop(cv, run_one(t)); }
static void op_push1(WasmCodeValidator *cv, WasmValType t) { op_push(cv, run_one(t)); }

static void op_match(WasmCodeValidator *cv, WasmTypeRun a, WasmTypeRun b, WasmValidateStatus fail)
{
    WasmStackOp *o = emit(cv, M_MATCH);

    o->run = a;
    o->run2 = b;
    o->fail = fail;
}

static void op_frame(WasmCodeValidator *cv, uint8_t kind, WasmTypeRun start, WasmTypeRun end)
{
    WasmStackOp *o = emit(cv, M_FRAME);

    o->aux = kind;
    o->run = start;
    o->run2 = end;
}

/* §7.6.1's pop_ctrl: the frame's end types, the height, its locals' initialization, the frame. */
static void op_pop_ctrl(WasmCodeValidator *cv, WasmTypeRun end)
{
    op_pop(cv, end);
    emit(cv, M_END_CHECK);
    emit(cv, M_RESET);
}

static void begin_program(WasmCodeValidator *cv)
{
    cv->n_prog = cv->pc = 0;
    cv->k = 0;
}

/* The match of operand `a` against expected `e`: Bot on either side holds, a (ref bot) by nullability. */
static WasmMatch match(WasmCodeValidator *cv, const WasmValType *a, const WasmValType *e)
{
    if (e->code == WASM_TYPE_BOT || a->code == WASM_TYPE_BOT) return WASM_MATCH_YES;
    if (a->code == WASM_TYPE_REF && a->heap == WASM_HEAP_BOT)
        return e->code == WASM_TYPE_REF && (e->nullable || !a->nullable) ? WASM_MATCH_YES : WASM_MATCH_NO;
    if (e->code == WASM_TYPE_REF && e->heap == WASM_HEAP_BOT) return WASM_MATCH_NO;
    return wasm_val_le(&cv->ctx->types, a, e, &cv->climb);
}

static void element_done(WasmCodeValidator *cv)
{
    if (++cv->k >= cv->prog[cv->pc].run.n) {
        cv->pc++;
        cv->k = 0;
    }
}

static void op_done(WasmCodeValidator *cv)
{
    cv->pc++;
    cv->k = 0;
}

static WasmValidateStatus answer(WasmCodeValidator *cv, WasmMatch le, const WasmValType *want,
                                 const WasmValType *got, WasmValidateStatus fail)
{
    if (le == WASM_MATCH_NO) return fail == WASM_VALIDATE_TYPE_MISMATCH ? mismatch(cv, want, got) : fail;
    if (le == WASM_MATCH_CLIMB) {
        cv->climbing = true;
        cv->want = *want;
        cv->got = *got;
        return WASM_VALIDATE_MORE;
    }
    element_done(cv);
    return WASM_VALIDATE_MORE;
}

static WasmValidateStatus climb_one(WasmCodeValidator *cv)
{
    WasmMatch le = wasm_climb(&cv->ctx->types, &cv->climb);
    WasmValidateStatus fail = cv->prog[cv->pc].fail;

    if (le == WASM_MATCH_CLIMB) return WASM_VALIDATE_MORE;
    cv->climbing = false;
    if (le == WASM_MATCH_NO) {
        WasmValType want = cv->want, got = cv->got;

        return fail == WASM_VALIDATE_TYPE_MISMATCH ? mismatch(cv, &want, &got) : fail;
    }
    element_done(cv);
    return WASM_VALIDATE_MORE;
}

static bool run_op(const WasmStackOp *o)
{
    return o->op == M_POP || o->op == M_PUSH || o->op == M_PEEK || o->op == M_MATCH || o->op == M_DEFAULTABLE;
}

/* One element of the program's current operation. */
static WasmValidateStatus step_op(WasmCodeValidator *cv)
{
    WasmStackOp *o = &cv->prog[cv->pc];
    WasmCtrlFrame *f = top(cv);
    WasmValType a, e;
    WasmValidateStatus st;

    if (run_op(o) && cv->k >= o->run.n && !(o->op == M_MATCH && o->run.n != o->run2.n)) {
        op_done(cv);
        return WASM_VALIDATE_MORE;
    }
    switch (o->op) {
    case M_POP:
        e = run_elem(cv, &o->run, o->run.n - 1 - cv->k);
        if (cv->n_vals == f->val_height) {
            if (!f->unreachable) return mismatch(cv, &e, NULL);
            a = val(WASM_TYPE_BOT);
        } else a = cv->vals[--cv->n_vals];
        return answer(cv, match(cv, &a, &e), &e, &a, o->fail);
    case M_PEEK:
        e = run_elem(cv, &o->run, o->run.n - 1 - cv->k);
        if (cv->n_vals - f->val_height <= cv->k) {
            if (!f->unreachable) return mismatch(cv, &e, NULL);
            a = val(WASM_TYPE_BOT);
        } else a = cv->vals[cv->n_vals - 1 - cv->k];
        return answer(cv, match(cv, &a, &e), &e, &a, o->fail);
    case M_PUSH:
        st = push_val(cv, run_elem(cv, &o->run, cv->k));
        if (st != WASM_VALIDATE_DONE) return st;
        element_done(cv);
        return WASM_VALIDATE_MORE;
    case M_MATCH:
        if (o->run.n != o->run2.n) return o->fail == WASM_VALIDATE_TYPE_MISMATCH ? mismatch(cv, NULL, NULL) : o->fail;
        a = run_elem(cv, &o->run, cv->k);
        e = run_elem(cv, &o->run2, cv->k);
        return answer(cv, match(cv, &a, &e), &e, &a, o->fail);
    case M_DEFAULTABLE:
        a = run_elem(cv, &o->run, cv->k);
        if (!defaultable(&a)) return refuse(cv, o->fail, o->index);
        element_done(cv);
        return WASM_VALIDATE_MORE;
    case M_FRAME:
        st = push_ctrl(cv, o->aux, o->run, o->run2);
        if (st != WASM_VALIDATE_DONE) return st;
        break;
    case M_REFRAME:
        f->kind = o->aux;
        f->val_height = cv->n_vals;
        f->init_height = cv->n_inits;
        f->unreachable = false;
        break;
    case M_END_CHECK:
        if (cv->n_vals != f->val_height) return mismatch(cv, NULL, &cv->vals[cv->n_vals - 1]);
        break;
    case M_RESET:
        if (cv->n_inits > f->init_height) {
            init_remove(cv, cv->inits[--cv->n_inits]);
            return WASM_VALIDATE_MORE;
        }
        break;
    case M_DROP_FRAME:
        cv->n_ctrls--;
        break;
    case M_UNREACHABLE:
        cv->n_vals = f->val_height;
        f->unreachable = true;
        break;
    case M_SET_LOCAL:
        if (!init_has(cv, o->index)) {
            st = init_add(cv, o->index);
            if (st != WASM_VALIDATE_DONE) return st;
        }
        break;
    default:
        CHECK_FAIL("a WebAssembly stack operation code_validator.c does not define");
    }
    op_done(cv);
    return WASM_VALIDATE_MORE;
}

/* Index lookups, each refusing with valid.ml's message. */
static WasmValidateStatus check_heap(WasmCodeValidator *cv, const WasmValType *t)
{
    if (t->code != WASM_TYPE_REF || t->heap != 0) return WASM_VALIDATE_DONE;
    return t->heap_index < cv->ctx->types.module->n_types ? WASM_VALIDATE_DONE : unknown(cv, "type", t->heap_index);
}

static WasmValidateStatus comp_type(WasmCodeValidator *cv, uint64_t x, WasmCompKind kind, WasmValidateStatus wrong,
                                    const WasmSubType **out)
{
    const WasmModule *m = cv->ctx->types.module;

    if (x >= m->n_types) return unknown(cv, "type", x);
    if (m->types[x].kind != kind) return refuse(cv, wrong, x);
    *out = &m->types[x];
    return WASM_VALIDATE_DONE;
}

static WasmValidateStatus func_type(WasmCodeValidator *cv, uint64_t x, const WasmSubType **out)
{
    return comp_type(cv, x, WASM_COMP_FUNC, WASM_VALIDATE_NOT_FUNCTION, out);
}

static WasmValidateStatus label(WasmCodeValidator *cv, uint64_t l, WasmTypeRun *out)
{
    const WasmCtrlFrame *f;

    if (l >= cv->n_ctrls) return unknown(cv, "label", l);
    f = &cv->ctrls[cv->n_ctrls - 1 - l];
    *out = f->kind == F_LOOP ? f->start : f->end;
    return WASM_VALIDATE_DONE;
}

static WasmValidateStatus block_type(WasmCodeValidator *cv, const WasmInstruction *in, WasmTypeRun *start,
                                     WasmTypeRun *end)
{
    const WasmSubType *t;
    WasmValidateStatus st;

    *start = *end = run_none();
    if (in->block_kind == WASM_BLOCK_VALUE) {
        st = check_heap(cv, &in->block_type);
        if (st == WASM_VALIDATE_DONE) *end = run_one(in->block_type);
        return st;
    }
    if (in->block_kind == WASM_BLOCK_EMPTY) return WASM_VALIDATE_DONE;
    st = func_type(cv, in->block_index, &t);
    if (st != WASM_VALIDATE_DONE) return st;
    *start = run_pool(t->params, t->n_params);
    *end = run_pool(t->results, t->n_results);
    return WASM_VALIDATE_DONE;
}

static WasmValidateStatus table(WasmCodeValidator *cv, uint64_t x, WasmValType *elem, WasmValType *at)
{
    const WasmLimits *l;

    if (x >= cv->ctx->n_tables) return unknown(cv, "table", x);
    *elem = *wasm_context_table(cv->ctx, x, &l);
    *at = addr(l);
    return WASM_VALIDATE_DONE;
}

static WasmValidateStatus memory(WasmCodeValidator *cv, uint64_t x, WasmValType *at)
{
    if (x >= cv->ctx->n_memories) return unknown(cv, "memory", x);
    *at = addr(wasm_context_memory(cv->ctx, x));
    return WASM_VALIDATE_DONE;
}

static WasmValidateStatus func(WasmCodeValidator *cv, uint64_t x, const WasmSubType **t, uint32_t *type)
{
    if (x >= cv->ctx->n_funcs) return unknown(cv, "function", x);
    *type = wasm_context_func(cv->ctx, x);
    *t = &cv->ctx->types.module->types[*type];
    return WASM_VALIDATE_DONE;
}

static WasmValidateStatus elem(WasmCodeValidator *cv, uint64_t x, WasmValType *t)
{
    if (x >= cv->ctx->types.module->n_elems) return unknown(cv, "elem segment", x);
    *t = cv->ctx->types.module->elems[x].type;
    return WASM_VALIDATE_DONE;
}

static WasmValidateStatus data(WasmCodeValidator *cv, uint64_t x)
{
    return x < cv->ctx->types.module->n_datas ? WASM_VALIDATE_DONE : unknown(cv, "data segment", x);
}

static WasmValidateStatus global(WasmCodeValidator *cv, uint64_t x, WasmValType *t, bool *mutable_)
{
    size_t n = cv->constant ? cv->visible_globals : cv->ctx->n_globals;

    if (x >= n) return unknown(cv, "global", x);
    *t = *wasm_context_global(cv->ctx, x, mutable_);
    return WASM_VALIDATE_DONE;
}

/* §7.6.1's top_heap_type. */
static uint8_t top_heap(const WasmCodeValidator *cv, const WasmValType *t)
{
    if (t->heap == 0) {
        WasmCompKind k = cv->ctx->types.module->types[t->heap_index].kind;

        return k == WASM_COMP_FUNC ? WASM_HEAP_FUNC : WASM_HEAP_ANY;
    }
    switch (t->heap) {
    case WASM_HEAP_FUNC: case WASM_HEAP_NOFUNC: return WASM_HEAP_FUNC;
    case WASM_HEAP_EXTERN: case WASM_HEAP_NOEXTERN: return WASM_HEAP_EXTERN;
    case WASM_HEAP_EXN: case WASM_HEAP_NOEXN: return WASM_HEAP_EXN;
    default: return WASM_HEAP_ANY;
    }
}

static WasmValType def_ref(bool nullable, uint64_t x)
{
    return ref(nullable, 0, (uint32_t)x);
}

static bool packed(const WasmValType *t)
{
    return t->code == WASM_TYPE_I8 || t->code == WASM_TYPE_I16;
}

/* A fixed signature from opcode_signatures.h, the address operand (a memory access's first) by its memory. */
static void signature(WasmCodeValidator *cv, const WasmSignature *s, const WasmValType *address)
{
    int i;

    for (i = s->n_params - 1; i >= 0; i--)
        op_pop1(cv, i == 0 && address != NULL ? *address : val(s->params[i]));
    if (s->result != 0) op_push1(cv, val(s->result));
}

/* §3.4.5's memarg: the memory, an alignment no larger than natural, an i32 memory's offset below 2^32, and a
   lane index below the lanes. */
static WasmValidateStatus memory_access(WasmCodeValidator *cv, const WasmInstruction *in, const WasmSignature *s)
{
    WasmValType at;
    WasmValidateStatus st = memory(cv, in->memory, &at);

    DCHECK(s->defined && s->natural != 0, "opcode_signatures.h gives a memory access no natural alignment");
    if (st != WASM_VALIDATE_DONE) return st;
    if (in->align >= 8 || (1u << in->align) > s->natural) return WASM_VALIDATE_ALIGNMENT;
    if (at.code == WASM_TYPE_I32 && in->offset >= (uint64_t)1 << 32) return WASM_VALIDATE_OFFSET_RANGE;
    if (in->shape == WASM_IMM_MEMOP_LANE && in->lane >= s->lanes) return WASM_VALIDATE_LANE_INDEX;
    signature(cv, s, &at);
    return WASM_VALIDATE_DONE;
}

/* The signature opcode_signatures.h gives an opcode, or NULL. */
static const WasmSignature *signature_of(uint32_t prefix, uint32_t op)
{
    const WasmSignature *s = NULL;

    if (prefix == 0 && op < COUNTOF(WASM_SIG_ONE)) s = &WASM_SIG_ONE[op];
    if (prefix == 0xFC && op < COUNTOF(WASM_SIG_FC)) s = &WASM_SIG_FC[op];
    if (prefix == 0xFD && op < COUNTOF(WASM_SIG_FD)) s = &WASM_SIG_FD[op];
    return s != NULL && s->defined ? s : NULL;
}

static const WasmSubType *body_type(const WasmCodeValidator *cv)
{
    return &cv->ctx->types.module->types[cv->func_type];
}

/* valid.ml's match_result_type of a tail call: the callee's results against the caller's. */
static void tail_results(WasmCodeValidator *cv, const WasmSubType *callee)
{
    const WasmSubType *self = body_type(cv);

    op_match(cv, run_pool(callee->results, callee->n_results), run_pool(self->results, self->n_results),
             WASM_VALIDATE_TYPE_MISMATCH);
}

/* §3.4.1-§3.4.4: parametric, control, variable and table instructions, and the one-byte rest. */
static WasmValidateStatus type_one(WasmCodeValidator *cv, const WasmInstruction *in)
{
    const WasmSubType *t;
    WasmTypeRun start, end, l;
    WasmValType a, b, at;
    WasmValidateStatus st = WASM_VALIDATE_DONE;
    uint32_t type;
    bool track, mut;

    switch (in->opcode) {
    case 0x00: emit(cv, M_UNREACHABLE); break;
    case 0x01: break;
    case 0x02: case 0x03: case 0x04:
        st = block_type(cv, in, &start, &end);
        if (st != WASM_VALIDATE_DONE) return st;
        if (in->opcode == 0x04) op_pop1(cv, val(WASM_TYPE_I32));
        op_pop(cv, start);
        op_frame(cv, in->opcode == 0x02 ? F_BLOCK : in->opcode == 0x03 ? F_LOOP : F_IF, start, end);
        op_push(cv, start);
        break;
    case 0x08:
        if (in->index[0] >= cv->ctx->n_tags) return unknown(cv, "tag", in->index[0]);
        t = &cv->ctx->types.module->types[wasm_context_tag(cv->ctx, in->index[0])];
        op_pop(cv, run_pool(t->params, t->n_params));
        emit(cv, M_UNREACHABLE);
        break;
    case 0x0A:
        op_pop1(cv, ref(true, WASM_HEAP_EXN, 0));
        emit(cv, M_UNREACHABLE);
        break;
    case 0x0C: case 0x0D:
        st = label(cv, in->index[0], &l);
        if (st != WASM_VALIDATE_DONE) return st;
        if (in->opcode == 0x0D) op_pop1(cv, val(WASM_TYPE_I32));
        op_pop(cv, l);
        if (in->opcode == 0x0C) emit(cv, M_UNREACHABLE);
        else op_push(cv, l);
        break;
    case 0x0E:
        op_pop1(cv, val(WASM_TYPE_I32));
        cv->in = *in;
        cv->list_left = in->list_count;
        cv->list_at = cv->r.pos;
        cv->phase = CV_BR_SKIP;
        break;
    case 0x0F:
        t = body_type(cv);
        op_pop(cv, run_pool(t->results, t->n_results));
        emit(cv, M_UNREACHABLE);
        break;
    case 0x10: case 0x12:
        st = func(cv, in->index[0], &t, &type);
        if (st != WASM_VALIDATE_DONE) return st;
        if (in->opcode == 0x12) tail_results(cv, t);
        op_pop(cv, run_pool(t->params, t->n_params));
        if (in->opcode == 0x10) op_push(cv, run_pool(t->results, t->n_results));
        else emit(cv, M_UNREACHABLE);
        break;
    case 0x11: case 0x13:
        /* call_indirect's immediates are the type, then the table; valid.ml looks the table up first. */
        st = table(cv, in->index[1], &a, &at);
        if (st == WASM_VALIDATE_DONE) st = func_type(cv, in->index[0], &t);
        if (st != WASM_VALIDATE_DONE) return st;
        b = ref(true, WASM_HEAP_FUNC, 0);
        if (match(cv, &a, &b) != WASM_MATCH_YES) return mismatch(cv, &b, &a);
        if (in->opcode == 0x13) tail_results(cv, t);
        op_pop1(cv, at);
        op_pop(cv, run_pool(t->params, t->n_params));
        if (in->opcode == 0x11) op_push(cv, run_pool(t->results, t->n_results));
        else emit(cv, M_UNREACHABLE);
        break;
    case 0x14: case 0x15:
        st = func_type(cv, in->index[0], &t);
        if (st != WASM_VALIDATE_DONE) return st;
        if (in->opcode == 0x15) tail_results(cv, t);
        op_pop1(cv, def_ref(true, in->index[0]));
        op_pop(cv, run_pool(t->params, t->n_params));
        if (in->opcode == 0x14) op_push(cv, run_pool(t->results, t->n_results));
        else emit(cv, M_UNREACHABLE);
        break;
    case 0x1A:
        op_pop1(cv, val(WASM_TYPE_BOT));
        break;
    case 0x1B:
        a = peek(cv, 1);
        if (a.code == WASM_TYPE_REF) return mismatch(cv, NULL, &a);
        op_pop1(cv, val(WASM_TYPE_I32));
        op_pop1(cv, a);
        op_pop1(cv, a);
        op_push1(cv, a);
        break;
    case 0x1C:
        if (in->list_count != 1) return WASM_VALIDATE_SELECT_ARITY;
        CHECK(wasm_read_val_type(&cv->r, &a) == WASM_DECODE_DONE, "a select type the decoder accepted did not read");
        st = check_heap(cv, &a);
        if (st != WASM_VALIDATE_DONE) return st;
        op_pop1(cv, val(WASM_TYPE_I32));
        op_pop1(cv, a);
        op_pop1(cv, a);
        op_push1(cv, a);
        break;
    case 0x1F:
        st = block_type(cv, in, &cv->block_start, &cv->block_end);
        if (st != WASM_VALIDATE_DONE) return st;
        op_pop(cv, cv->block_start);
        cv->list_left = in->list_count;
        cv->phase = CV_CATCH;
        break;
    case 0x20: case 0x21: case 0x22:
        st = local(cv, in->index[0], &a, &track);
        if (st != WASM_VALIDATE_DONE) return st;
        if (in->opcode == 0x20) {
            if (track && !init_has(cv, in->index[0])) return WASM_VALIDATE_UNINITIALIZED_LOCAL;
            op_push1(cv, a);
            break;
        }
        op_pop1(cv, a);
        if (in->opcode == 0x22) op_push1(cv, a);
        if (track) emit(cv, M_SET_LOCAL)->index = in->index[0];
        break;
    case 0x23: case 0x24:
        st = global(cv, in->index[0], &a, &mut);
        if (st != WASM_VALIDATE_DONE) return st;
        if (in->opcode == 0x23) op_push1(cv, a);
        else if (!mut) return WASM_VALIDATE_IMMUTABLE_GLOBAL;
        else op_pop1(cv, a);
        break;
    case 0x25: case 0x26:
        st = table(cv, in->index[0], &a, &at);
        if (st != WASM_VALIDATE_DONE) return st;
        if (in->opcode == 0x26) op_pop1(cv, a);
        op_pop1(cv, at);
        if (in->opcode == 0x25) op_push1(cv, a);
        break;
    case 0x3F: case 0x40:
        st = memory(cv, in->index[0], &at);
        if (st != WASM_VALIDATE_DONE) return st;
        if (in->opcode == 0x40) op_pop1(cv, at);
        op_push1(cv, at);
        break;
    case 0x41: op_push1(cv, val(WASM_TYPE_I32)); break;
    case 0x42: op_push1(cv, val(WASM_TYPE_I64)); break;
    case 0x43: op_push1(cv, val(WASM_TYPE_F32)); break;
    case 0x44: op_push1(cv, val(WASM_TYPE_F64)); break;
    case 0xD0:
        st = check_heap(cv, &in->heap[0]);
        if (st != WASM_VALIDATE_DONE) return st;
        op_push1(cv, ref(true, in->heap[0].heap, in->heap[0].heap_index));
        break;
    case 0xD1: case 0xD4:
        st = peek_ref(cv, &a);
        if (st != WASM_VALIDATE_DONE) return st;
        op_pop1(cv, val(WASM_TYPE_BOT));
        op_push1(cv, in->opcode == 0xD1 ? val(WASM_TYPE_I32) : ref(false, a.heap, a.heap_index));
        break;
    case 0xD2:
        /* A constant expression's ref.func is itself part of C.refs; a body's must find its function there. */
        st = func(cv, in->index[0], &t, &type);
        if (st != WASM_VALIDATE_DONE) return st;
        if (cv->constant) wasm_context_declare(cv->ctx, in->index[0]);
        else if (!wasm_context_declared(cv->ctx, in->index[0]))
            return refuse(cv, WASM_VALIDATE_UNDECLARED_FUNC, in->index[0]);
        op_push1(cv, def_ref(false, type));
        break;
    case 0xD3:
        op_pop1(cv, ref(true, WASM_HEAP_EQ, 0));
        op_pop1(cv, ref(true, WASM_HEAP_EQ, 0));
        op_push1(cv, val(WASM_TYPE_I32));
        break;
    case 0xD5:
        st = peek_ref(cv, &a);
        if (st == WASM_VALIDATE_DONE) st = label(cv, in->index[0], &l);
        if (st != WASM_VALIDATE_DONE) return st;
        op_pop1(cv, val(WASM_TYPE_BOT));
        op_pop(cv, l);
        op_push(cv, l);
        op_push1(cv, ref(false, a.heap, a.heap_index));
        break;
    case 0xD6:
        st = label(cv, in->index[0], &l);
        if (st != WASM_VALIDATE_DONE) return st;
        if (l.n == 0) return mismatch(cv, NULL, NULL);
        a = run_elem(cv, &l, l.n - 1);
        if (a.code != WASM_TYPE_REF) return mismatch(cv, NULL, &a);
        l.n--;
        op_pop1(cv, ref(true, a.heap, a.heap_index));
        op_pop(cv, l);
        op_push(cv, l);
        break;
    default: {
        const WasmSignature *s = signature_of(0, in->opcode);

        CHECK(s != NULL, "a one-byte WebAssembly opcode the decoder accepts has no typing rule in code_validator.c");
        if (in->shape == WASM_IMM_MEMOP) return memory_access(cv, in, s);
        signature(cv, s, NULL);
        break;
    }
    }
    return WASM_VALIDATE_DONE;
}

/* §3.4.6-§3.4.9: the 0xFB reference and aggregate instructions. */
static WasmValidateStatus type_fb(WasmCodeValidator *cv, const WasmInstruction *in)
{
    const WasmModule *m = cv->ctx->types.module;
    const WasmSubType *t, *u;
    WasmValidateStatus st = WASM_VALIDATE_DONE;
    WasmValType a, b, f;
    WasmTypeRun l;
    const WasmFieldType *ft;
    uint32_t x = in->index[0];

    switch (in->opcode) {
    case 0: case 1:
        st = comp_type(cv, x, WASM_COMP_STRUCT, WASM_VALIDATE_NOT_STRUCT, &t);
        if (st != WASM_VALIDATE_DONE) return st;
        if (in->opcode == 0) emit(cv, M_POP)->run = (WasmTypeRun){ RUN_FIELDS, { 0 }, t->fields, t->n_fields };
        else {
            WasmStackOp *o = emit(cv, M_DEFAULTABLE);

            o->run = (WasmTypeRun){ RUN_FIELDS, { 0 }, t->fields, t->n_fields };
            o->fail = WASM_VALIDATE_NOT_DEFAULTABLE;
            o->index = 0;
        }
        op_push1(cv, def_ref(false, x));
        break;
    case 2: case 3: case 4: case 5:
        st = comp_type(cv, x, WASM_COMP_STRUCT, WASM_VALIDATE_NOT_STRUCT, &t);
        if (st != WASM_VALIDATE_DONE) return st;
        if (in->index[1] >= t->n_fields) return unknown(cv, "field", in->index[1]);
        ft = &m->fields[t->fields + in->index[1]];
        f = unpack(ft->storage);
        if (in->opcode == 5) {
            if (!ft->mutable_) return WASM_VALIDATE_IMMUTABLE_FIELD;
            op_pop1(cv, f);
            op_pop1(cv, def_ref(true, x));
            break;
        }
        if ((in->opcode == 2) == packed(&ft->storage)) return refuse(cv, WASM_VALIDATE_FIELD_PACKED, in->opcode == 2);
        op_pop1(cv, def_ref(true, x));
        op_push1(cv, f);
        break;
    case 6: case 7: case 8: case 9: case 10:
        st = comp_type(cv, x, WASM_COMP_ARRAY, WASM_VALIDATE_NOT_ARRAY, &t);
        if (st != WASM_VALIDATE_DONE) return st;
        ft = &m->fields[t->fields];
        f = unpack(ft->storage);
        if (in->opcode == 7 && !defaultable(&f)) return refuse(cv, WASM_VALIDATE_NOT_DEFAULTABLE, 1);
        if (in->opcode == 9) {
            st = data(cv, in->index[1]);
            if (st != WASM_VALIDATE_DONE) return st;
            if (f.code == WASM_TYPE_REF) return WASM_VALIDATE_NOT_NUMERIC;
        }
        if (in->opcode == 10) {
            st = elem(cv, in->index[1], &a);
            if (st != WASM_VALIDATE_DONE) return st;
            op_match(cv, run_one(a), run_one(f), WASM_VALIDATE_TYPE_MISMATCH);
        }
        if (in->opcode == 8) op_pop(cv, run_rep(f, in->index[1]));
        else {
            op_pop1(cv, val(WASM_TYPE_I32));
            if (in->opcode == 6) op_pop1(cv, f);
            if (in->opcode >= 9) op_pop1(cv, val(WASM_TYPE_I32));
        }
        op_push1(cv, def_ref(false, x));
        break;
    case 11: case 12: case 13: case 14: case 16: case 18: case 19:
        st = comp_type(cv, x, WASM_COMP_ARRAY, WASM_VALIDATE_NOT_ARRAY, &t);
        if (st != WASM_VALIDATE_DONE) return st;
        ft = &m->fields[t->fields];
        f = unpack(ft->storage);
        if (in->opcode <= 13) {
            if ((in->opcode == 11) == packed(&ft->storage))
                return refuse(cv, WASM_VALIDATE_ARRAY_PACKED, in->opcode == 11);
            op_pop1(cv, val(WASM_TYPE_I32));
            op_pop1(cv, def_ref(true, x));
            op_push1(cv, f);
            break;
        }
        if (!ft->mutable_) return WASM_VALIDATE_IMMUTABLE_ARRAY;
        if (in->opcode == 18) {
            st = data(cv, in->index[1]);
            if (st != WASM_VALIDATE_DONE) return st;
            if (f.code == WASM_TYPE_REF) return WASM_VALIDATE_NOT_NUMERIC;
        }
        if (in->opcode == 19) {
            st = elem(cv, in->index[1], &a);
            if (st != WASM_VALIDATE_DONE) return st;
            op_match(cv, run_one(a), run_one(f), WASM_VALIDATE_TYPE_MISMATCH);
        }
        if (in->opcode == 14) {
            op_pop1(cv, f);
            op_pop1(cv, val(WASM_TYPE_I32));
        } else if (in->opcode == 16) {
            op_pop1(cv, val(WASM_TYPE_I32));
            op_pop1(cv, f);
            op_pop1(cv, val(WASM_TYPE_I32));
        } else op_pop(cv, run_rep(val(WASM_TYPE_I32), 3));
        op_pop1(cv, def_ref(true, x));
        break;
    case 15:
        op_pop1(cv, ref(true, WASM_HEAP_ARRAY, 0));
        op_push1(cv, val(WASM_TYPE_I32));
        break;
    case 17:
        st = comp_type(cv, x, WASM_COMP_ARRAY, WASM_VALIDATE_NOT_ARRAY, &t);
        if (st == WASM_VALIDATE_DONE) st = comp_type(cv, in->index[1], WASM_COMP_ARRAY, WASM_VALIDATE_NOT_ARRAY, &u);
        if (st != WASM_VALIDATE_DONE) return st;
        if (!m->fields[t->fields].mutable_) return WASM_VALIDATE_IMMUTABLE_ARRAY;
        op_match(cv, run_one(m->fields[u->fields].storage), run_one(m->fields[t->fields].storage),
                 WASM_VALIDATE_ARRAY_TYPES);
        op_pop(cv, run_rep(val(WASM_TYPE_I32), 2));
        op_pop1(cv, def_ref(true, in->index[1]));
        op_pop1(cv, val(WASM_TYPE_I32));
        op_pop1(cv, def_ref(true, x));
        break;
    case 20: case 21: case 22: case 23:
        st = check_heap(cv, &in->heap[0]);
        if (st != WASM_VALIDATE_DONE) return st;
        op_pop1(cv, ref(true, top_heap(cv, &in->heap[0]), 0));
        if (in->opcode <= 21) op_push1(cv, val(WASM_TYPE_I32));
        else op_push1(cv, ref(in->opcode == 23, in->heap[0].heap, in->heap[0].heap_index));
        break;
    case 24: case 25: {
        /* br_on_cast's flags: bit 0 the source's nullability, bit 1 the target's. valid.ml's diff_reftype drops
           the source's null when the target takes it. */
        WasmValType rt1 = ref(in->cast_flags & 1, in->heap[0].heap, in->heap[0].heap_index);
        WasmValType rt2 = ref((in->cast_flags & 2) != 0, in->heap[1].heap, in->heap[1].heap_index);
        WasmValType diff = rt1;

        if (rt2.nullable) diff.nullable = false;
        st = check_heap(cv, &rt1);
        if (st == WASM_VALIDATE_DONE) st = check_heap(cv, &rt2);
        if (st != WASM_VALIDATE_DONE) return st;
        op_match(cv, run_one(rt2), run_one(rt1), WASM_VALIDATE_TYPE_MISMATCH);
        st = label(cv, in->index[0], &l);
        if (st != WASM_VALIDATE_DONE) return st;
        if (l.n == 0) return mismatch(cv, &rt2, NULL);
        a = run_elem(cv, &l, l.n - 1);
        l.n--;
        op_match(cv, run_one(in->opcode == 24 ? rt2 : diff), run_one(a), WASM_VALIDATE_TYPE_MISMATCH);
        op_pop1(cv, rt1);
        op_pop(cv, l);
        op_push(cv, l);
        op_push1(cv, in->opcode == 24 ? diff : rt2);
        break;
    }
    case 26: case 27:
        st = peek_ref(cv, &a);
        if (st != WASM_VALIDATE_DONE) return st;
        b = ref(a.nullable, in->opcode == 26 ? WASM_HEAP_EXTERN : WASM_HEAP_ANY, 0);
        op_pop1(cv, b);
        b.heap = in->opcode == 26 ? WASM_HEAP_ANY : WASM_HEAP_EXTERN;
        op_push1(cv, b);
        break;
    case 28:
        op_pop1(cv, val(WASM_TYPE_I32));
        op_push1(cv, ref(false, WASM_HEAP_I31, 0));
        break;
    case 29: case 30:
        op_pop1(cv, ref(true, WASM_HEAP_I31, 0));
        op_push1(cv, val(WASM_TYPE_I32));
        break;
    default:
        CHECK_FAIL("a 0xFB WebAssembly opcode the decoder accepts has no typing rule in code_validator.c");
    }
    return WASM_VALIDATE_DONE;
}

/* §3.4.4 and §3.4.5's bulk instructions under 0xFC; its 0 to 7 are numeric. */
static WasmValidateStatus type_fc(WasmCodeValidator *cv, const WasmInstruction *in)
{
    WasmValidateStatus st = WASM_VALIDATE_DONE;
    WasmValType a, b, at, bt, low;

    if (in->opcode < 8) {
        signature(cv, signature_of(0xFC, in->opcode), NULL);
        return WASM_VALIDATE_DONE;
    }
    switch (in->opcode) {
    case 8:   /* memory.init's immediates are the data segment, then the memory */
        st = memory(cv, in->index[1], &at);
        if (st == WASM_VALIDATE_DONE) st = data(cv, in->index[0]);
        if (st != WASM_VALIDATE_DONE) return st;
        op_pop(cv, run_rep(val(WASM_TYPE_I32), 2));
        op_pop1(cv, at);
        break;
    case 9: return data(cv, in->index[0]);
    case 10:
        st = memory(cv, in->index[0], &at);
        if (st == WASM_VALIDATE_DONE) st = memory(cv, in->index[1], &bt);
        if (st != WASM_VALIDATE_DONE) return st;
        low = at.code == WASM_TYPE_I64 && bt.code == WASM_TYPE_I64 ? at : val(WASM_TYPE_I32);
        op_pop1(cv, low);
        op_pop1(cv, bt);
        op_pop1(cv, at);
        break;
    case 11:
        st = memory(cv, in->index[0], &at);
        if (st != WASM_VALIDATE_DONE) return st;
        op_pop1(cv, at);
        op_pop1(cv, val(WASM_TYPE_I32));
        op_pop1(cv, at);
        break;
    case 12:  /* table.init's immediates are the element segment, then the table */
        st = table(cv, in->index[1], &a, &at);
        if (st == WASM_VALIDATE_DONE) st = elem(cv, in->index[0], &b);
        if (st != WASM_VALIDATE_DONE) return st;
        op_match(cv, run_one(b), run_one(a), WASM_VALIDATE_TYPE_MISMATCH);
        op_pop(cv, run_rep(val(WASM_TYPE_I32), 2));
        op_pop1(cv, at);
        break;
    case 13: return elem(cv, in->index[0], &a);
    case 14:
        st = table(cv, in->index[0], &a, &at);
        if (st == WASM_VALIDATE_DONE) st = table(cv, in->index[1], &b, &bt);
        if (st != WASM_VALIDATE_DONE) return st;
        op_match(cv, run_one(b), run_one(a), WASM_VALIDATE_TYPE_MISMATCH);
        low = at.code == WASM_TYPE_I64 && bt.code == WASM_TYPE_I64 ? at : val(WASM_TYPE_I32);
        op_pop1(cv, low);
        op_pop1(cv, bt);
        op_pop1(cv, at);
        break;
    case 15: case 16: case 17:
        st = table(cv, in->index[0], &a, &at);
        if (st != WASM_VALIDATE_DONE) return st;
        if (in->opcode == 15) {
            op_pop1(cv, at);
            op_pop1(cv, a);
            op_push1(cv, at);
        } else if (in->opcode == 16) op_push1(cv, at);
        else {
            op_pop1(cv, at);
            op_pop1(cv, a);
            op_pop1(cv, at);
        }
        break;
    default:
        CHECK_FAIL("a 0xFC WebAssembly opcode the decoder accepts has no typing rule in code_validator.c");
    }
    return WASM_VALIDATE_DONE;
}

/* §3.4.11's vector instructions under 0xFD: v128.const, i8x16.shuffle, and the fixed signatures. */
static WasmValidateStatus type_fd(WasmCodeValidator *cv, const WasmInstruction *in)
{
    const WasmSignature *s;
    int i;

    if (in->opcode == 12) {
        op_push1(cv, val(WASM_TYPE_V128));
        return WASM_VALIDATE_DONE;
    }
    if (in->opcode == 13) {
        for (i = 0; i < 16; i++)
            if (cv->ctx->bytes[in->bytes + i] >= 32) return WASM_VALIDATE_LANE_INDEX;
        op_pop(cv, run_rep(val(WASM_TYPE_V128), 2));
        op_push1(cv, val(WASM_TYPE_V128));
        return WASM_VALIDATE_DONE;
    }
    s = signature_of(0xFD, in->opcode);
    CHECK(s != NULL, "a 0xFD WebAssembly opcode the decoder accepts has no typing rule in code_validator.c");
    if (in->shape == WASM_IMM_MEMOP || in->shape == WASM_IMM_MEMOP_LANE) return memory_access(cv, in, s);
    if (in->shape == WASM_IMM_LANE && in->lane >= s->lanes) return WASM_VALIDATE_LANE_INDEX;
    signature(cv, s, NULL);
    return WASM_VALIDATE_DONE;
}

static void read_instruction(WasmCodeValidator *cv, WasmInstruction *in)
{
    uint32_t prefix, op;

    CHECK(wasm_read_opcode(&cv->r, &prefix, &op) == WASM_DECODE_DONE
          && wasm_read_immediates(&cv->r, prefix, op, in) == WASM_DECODE_DONE,
          "an instruction module_decoder.c accepted did not read again");
}

/* §7.6.2's end and else. An if without an else types its absent else arm as an empty one (valid.ml's
   check_block of []), so its start types must give its end types. */
static void structure(WasmCodeValidator *cv, uint8_t op)
{
    WasmCtrlFrame *f = top(cv);

    DCHECK(cv->n_ctrls > 1, "a WebAssembly end or else inside an expr closes no block, which the decoder rules out");
    if (op == 0x05) {
        DCHECK(f->kind == F_IF, "a WebAssembly else outside an if arm, which the decoder rules out");
        op_pop_ctrl(cv, f->end);
        emit(cv, M_REFRAME)->aux = F_ELSE;
        op_push(cv, f->start);
        return;
    }
    if (f->kind == F_IF) {
        op_pop_ctrl(cv, f->end);
        emit(cv, M_REFRAME)->aux = F_ELSE;
        op_push(cv, f->start);
    }
    op_pop_ctrl(cv, f->end);
    emit(cv, M_DROP_FRAME);
    op_push(cv, f->end);
}

static WasmValidateStatus type_step(WasmCodeValidator *cv)
{
    WasmInstruction in;
    WasmValidateStatus st;
    uint8_t b;

    begin_program(cv);
    if (cv->r.pos == cv->r.len) {
        DCHECK(cv->n_ctrls == 1, "a WebAssembly expr ended with a block open, which the decoder rules out");
        op_pop_ctrl(cv, top(cv)->end);
        emit(cv, M_DROP_FRAME);
        cv->phase = CV_END;
        return WASM_VALIDATE_MORE;
    }
    b = cv->r.in[cv->r.pos];
    if (b == 0x0B || b == 0x05) {
        cv->r.pos++;
        structure(cv, b);
        return WASM_VALIDATE_MORE;
    }
    read_instruction(cv, &in);
    switch (in.prefix) {
    case 0: st = type_one(cv, &in); break;
    case 0xFB: st = type_fb(cv, &in); break;
    case 0xFC: st = type_fc(cv, &in); break;
    case 0xFD: st = type_fd(cv, &in); break;
    default: CHECK_FAIL("a WebAssembly opcode prefix wasm_read_opcode does not read");
    }
    return st == WASM_VALIDATE_DONE ? WASM_VALIDATE_MORE : st;
}

/* §7.6.2's br_table, one label a step: the default label first (CV_BR_SKIP reads past the others to it), then
   each label's arity against the default's and its types against the operands in place (CV_BR_CHECK), then the
   default's types are popped and the rest of the block is unreachable. */
static WasmValidateStatus br_table_step(WasmCodeValidator *cv)
{
    WasmTypeRun l;
    WasmValidateStatus st;
    uint32_t x;

    begin_program(cv);
    CHECK(wasm_read_label(&cv->r, &x) == WASM_DECODE_DONE, "a br_table label the decoder accepted did not read");
    if (cv->list_left > 0) {
        cv->list_left--;
        if (cv->phase == CV_BR_SKIP) return WASM_VALIDATE_MORE;
        st = label(cv, x, &l);
        if (st != WASM_VALIDATE_DONE) return st;
        if (l.n != cv->arity) return mismatch(cv, NULL, NULL);
        emit(cv, M_PEEK)->run = l;
        return WASM_VALIDATE_MORE;
    }
    st = label(cv, x, &l);
    if (st != WASM_VALIDATE_DONE) return st;
    if (cv->phase == CV_BR_SKIP) {
        cv->arity = l.n;
        cv->list_left = cv->in.list_count;
        cv->r.pos = cv->list_at;
        cv->phase = CV_BR_CHECK;
        return WASM_VALIDATE_MORE;
    }
    op_pop(cv, l);
    emit(cv, M_UNREACHABLE);
    cv->phase = CV_TYPE;
    return WASM_VALIDATE_MORE;
}

/* §7.6.2's try_table, one catch clause a step: the tag's parameters (and for a _ref clause an exnref) are what
   the clause's label receives, checked by a frame of their own; then the try_table's frame. */
static WasmValidateStatus catch_step(WasmCodeValidator *cv)
{
    const WasmSubType *t = NULL;
    WasmTypeRun l;
    WasmCatch c;
    WasmValidateStatus st;

    begin_program(cv);
    if (cv->list_left == 0) {
        op_frame(cv, F_TRY, cv->block_start, cv->block_end);
        op_push(cv, cv->block_start);
        cv->phase = CV_TYPE;
        return WASM_VALIDATE_MORE;
    }
    cv->list_left--;
    CHECK(wasm_read_catch(&cv->r, &c) == WASM_DECODE_DONE, "a catch clause the decoder accepted did not read");
    if (c.kind == WASM_CATCH || c.kind == WASM_CATCH_REF) {
        if (c.tag >= cv->ctx->n_tags) return unknown(cv, "tag", c.tag);
        t = &cv->ctx->types.module->types[wasm_context_tag(cv->ctx, c.tag)];
    }
    st = label(cv, c.label, &l);
    if (st != WASM_VALIDATE_DONE) return st;
    op_frame(cv, F_CATCH, run_none(), l);
    if (t != NULL) op_push(cv, run_pool(t->params, t->n_params));
    if (c.kind == WASM_CATCH_REF || c.kind == WASM_CATCH_ALL_REF) op_push1(cv, ref(false, WASM_HEAP_EXN, 0));
    op_pop_ctrl(cv, l);
    emit(cv, M_DROP_FRAME);
    return WASM_VALIDATE_MORE;
}

/* §3.5.6-§3.5.7: one locals entry of the body a step, its type valid and its run's end recorded; then the
   function's frame, whose label and results are the function type's results. */
static WasmValidateStatus locals_step(WasmCodeValidator *cv)
{
    const WasmModule *m = cv->ctx->types.module;
    const WasmCode *c = &m->codes[cv->code];
    const WasmSubType *ft = body_type(cv);
    const WasmLocals *e;
    WasmValidateStatus st;

    if (cv->item == c->n_locals) {
        st = push_ctrl(cv, F_FUNC, run_none(), run_pool(ft->results, ft->n_results));
        if (st != WASM_VALIDATE_DONE) return st;
        cv->phase = CV_TYPE;
        return WASM_VALIDATE_MORE;
    }
    if (cv->item == 0 && c->n_locals > cv->cap_local_ends) {
        uint64_t *ends;

        if (c->n_locals > SIZE_MAX / sizeof *ends) return WASM_VALIDATE_NO_ROOM;
        ends = realloc(cv->local_ends, c->n_locals * sizeof *ends);
        if (ends == NULL) return WASM_VALIDATE_NO_ROOM;
        cv->local_ends = ends;
        cv->cap_local_ends = c->n_locals;
    }
    e = &m->locals[c->locals + cv->item];
    st = check_heap(cv, &e->type);
    if (st != WASM_VALIDATE_DONE) return st;
    cv->local_ends[cv->item] = (cv->item == 0 ? ft->n_params : cv->local_ends[cv->item - 1]) + e->count;
    cv->item++;
    return WASM_VALIDATE_MORE;
}

/* §3.4.13's constant instructions, as valid.ml's is_const reads them: the constants, i32 and i64 add, sub and
   mul, ref.null, ref.func, ref.i31, struct.new, array.new and array.new_fixed with their _default forms, the
   extern conversions, and global.get of an immutable global. */
static WasmValidateStatus const_step(WasmCodeValidator *cv)
{
    WasmInstruction in;
    WasmValType t;
    bool mut, ok;
    WasmValidateStatus st;

    if (cv->r.pos == cv->r.len) {
        cv->r.pos = cv->start;
        st = push_ctrl(cv, F_FUNC, run_none(), run_one(cv->expect));
        if (st != WASM_VALIDATE_DONE) return st;
        cv->phase = CV_TYPE;
        return WASM_VALIDATE_MORE;
    }
    if (cv->r.in[cv->r.pos] == 0x0B || cv->r.in[cv->r.pos] == 0x05) return WASM_VALIDATE_NOT_CONSTANT;
    read_instruction(cv, &in);
    switch (in.prefix) {
    case 0:
        if (in.opcode == 0x23) {
            st = global(cv, in.index[0], &t, &mut);
            if (st != WASM_VALIDATE_DONE) return st;
            ok = !mut;
            break;
        }
        ok = (in.opcode >= 0x41 && in.opcode <= 0x44) || (in.opcode >= 0x6A && in.opcode <= 0x6C)
          || (in.opcode >= 0x7C && in.opcode <= 0x7E) || in.opcode == 0xD0 || in.opcode == 0xD2;
        break;
    case 0xFB:
        ok = in.opcode <= 1 || (in.opcode >= 6 && in.opcode <= 8) || (in.opcode >= 26 && in.opcode <= 28);
        break;
    case 0xFD: ok = in.opcode == 12; break;
    default: ok = false; break;
    }
    return ok ? WASM_VALIDATE_MORE : WASM_VALIDATE_NOT_CONSTANT;
}

WasmValidateStatus wasm_code_validator_step(WasmCodeValidator *cv)
{
    WasmValidateStatus st;

    DCHECK(cv != NULL && cv->phase != CV_IDLE, "a WebAssembly code validator was stepped with no expr begun");
    if (cv->climbing) st = climb_one(cv);
    else if (cv->pc < cv->n_prog) st = step_op(cv);
    else {
        switch (cv->phase) {
        case CV_LOCALS: st = locals_step(cv); break;
        case CV_CONST: st = const_step(cv); break;
        case CV_TYPE: st = type_step(cv); break;
        case CV_BR_SKIP: case CV_BR_CHECK: st = br_table_step(cv); break;
        case CV_CATCH: st = catch_step(cv); break;
        case CV_END:
            DCHECK(cv->n_ctrls == 0, "a WebAssembly expr's typing ended with a frame open");
            st = WASM_VALIDATE_DONE;
            break;
        default: CHECK_FAIL("a WebAssembly code validation stands in a phase code_validator.c does not define");
        }
    }
    if (st != WASM_VALIDATE_MORE) cv->phase = CV_IDLE;
    return st;
}

void wasm_code_validator_free(WasmCodeValidator *cv)
{
    DCHECK(cv != NULL, "a WebAssembly code validator was freed with no validator");
    free(cv->local_ends);
    free(cv->vals);
    free(cv->ctrls);
    free(cv->inits);
    free(cv->init_set);
    memset(cv, 0, sizeof *cv);
}
