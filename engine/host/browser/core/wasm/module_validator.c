/* WebAssembly Core Specification (Release 3.0) §3.5 "Modules": module validation. See module_validator.h. */
#include "core/wasm/module_validator.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/wasm/matching.h"

/* The phases, in valid.ml's check_module order: each rec group's subtypes, then their supertype conditions;
   imports, tags, functions, memories, tables, globals, data, elements; the start function; exports, then the
   disjointness of their names. */
enum {
    PH_ALLOC = 0, PH_TYPES, PH_CANON, PH_TYPES_SUPER, PH_CLIMB, PH_IMPORTS, PH_TAGS, PH_FUNCS, PH_MEMORIES, PH_TABLES,
    PH_GLOBALS, PH_DATAS, PH_ELEMS, PH_START, PH_EXPORTS, PH_NAMES, PH_FINISHED
};

void wasm_module_validator_init(WasmModuleValidator *v, const WasmModule *m, const uint8_t *bytes)
{
    size_t i;

    DCHECK(v != NULL && m != NULL && bytes != NULL, "a WebAssembly validation was begun without its module");
    memset(v, 0, sizeof *v);
    v->module = m;
    v->bytes = bytes;
    for (i = 0; i < m->n_imports; i++) {
        switch (m->imports[i].kind) {
        case WASM_EXTERN_FUNC: v->n_funcs++; break;
        case WASM_EXTERN_TABLE: v->n_tables++; break;
        case WASM_EXTERN_MEMORY: v->n_memories++; break;
        case WASM_EXTERN_GLOBAL: v->n_globals++; break;
        case WASM_EXTERN_TAG: v->n_tags++; break;
        }
    }
    v->n_funcs += m->n_func_types;
    v->n_tables += m->n_tables;
    v->n_memories += m->n_memories;
    v->n_globals += m->n_globals;
    v->n_tags += m->n_tags;
    v->phase = PH_ALLOC;
}

/* The tables sized by the module: a canonical id per type (at most one group per type), and a hash table over
   those ids at most half full. Their size is the input's, so a failed allocation is the NO_ROOM refusal. */
static WasmValidateStatus allocate(WasmModuleValidator *v)
{
    size_t n = v->module->n_types ? v->module->n_types : 1, cap = 8;

    while (cap < 2 * n) {
        if (cap > SIZE_MAX / 4 / sizeof(size_t)) return WASM_VALIDATE_NO_ROOM;
        cap *= 2;
    }
    v->table_imports = calloc(v->n_tables - v->module->n_tables + 1, sizeof *v->table_imports);
    v->group_canon = calloc(n, sizeof *v->group_canon);
    v->canon_off = calloc(n, sizeof *v->canon_off);
    v->canon_len = calloc(n, sizeof *v->canon_len);
    v->canon_hash = calloc(n, sizeof *v->canon_hash);
    v->canon_table = calloc(cap, sizeof *v->canon_table);
    if (!v->table_imports || !v->group_canon || !v->canon_off || !v->canon_len || !v->canon_hash || !v->canon_table)
        return WASM_VALIDATE_NO_ROOM;
    v->cap_canon_table = cap;
    v->types.module = v->module;
    v->types.group_canon = v->group_canon;
    v->phase = PH_TYPES;
    return WASM_VALIDATE_MORE;
}

static WasmValidateStatus unknown(WasmModuleValidator *v, const char *space, uint64_t x)
{
    v->space = space;
    v->index = x;
    return WASM_VALIDATE_UNKNOWN_INDEX;
}

/* §3.2.3's type use: the index names a defined type of the context, here those below `limit`. */
static WasmValidateStatus check_type_index(WasmModuleValidator *v, uint64_t x, size_t limit)
{
    return x < limit ? WASM_VALIDATE_DONE : unknown(v, "type", x);
}

/* §3.2.4-§3.2.6: a value or storage type is valid when its heap type's index, if it has one, is. */
static WasmValidateStatus check_val_type(WasmModuleValidator *v, const WasmValType *t, size_t limit)
{
    if (t->code != WASM_TYPE_REF || t->heap != 0) return WASM_VALIDATE_DONE;
    return check_type_index(v, t->heap_index, limit);
}

/* valid.ml's func_type: the index names a type of the module whose expansion is a function type. */
static WasmValidateStatus check_func_type(WasmModuleValidator *v, uint32_t x, const WasmSubType **out)
{
    const WasmModule *m = v->module;
    WasmValidateStatus st = check_type_index(v, x, m->n_types);

    if (st != WASM_VALIDATE_DONE) return st;
    if (m->types[x].kind != WASM_COMP_FUNC) {
        v->index = x;
        return WASM_VALIDATE_NOT_FUNCTION;
    }
    if (out != NULL) *out = &m->types[x];
    return WASM_VALIDATE_DONE;
}

/* §3.2.13: a tag's type is a function type with no results. */
static WasmValidateStatus check_tag_type(WasmModuleValidator *v, uint32_t x)
{
    const WasmSubType *t;
    WasmValidateStatus st = check_func_type(v, x, &t);

    if (st != WASM_VALIDATE_DONE) return st;
    return t->n_results == 0 ? WASM_VALIDATE_DONE : WASM_VALIDATE_TAG_RESULT;
}

/* §3.2.12's limits within k: n <= k, and with a maximum m, m <= k and n <= m, in valid.ml's order. A memory's k
   is 2^16 pages for i32 and 2^48 for i64 (§3.2.15), a table's 2^32-1 and 2^64-1 (§3.2.16). */
static WasmValidateStatus check_limits(WasmModuleValidator *v, const WasmLimits *l, bool memory)
{
    uint64_t k = memory ? (l->addr64 ? (uint64_t)1 << 48 : (uint64_t)1 << 16)
                        : (l->addr64 ? UINT64_MAX : (uint64_t)UINT32_MAX);

    v->space = memory ? "memory" : "table";
    v->index = l->addr64;
    if (l->min > k || (l->has_max && l->max > k)) return WASM_VALIDATE_LIMITS_RANGE;
    if (l->has_max && l->min > l->max) return WASM_VALIDATE_LIMITS_ORDER;
    return WASM_VALIDATE_DONE;
}

static WasmValidateStatus check_table_type(WasmModuleValidator *v, const WasmValType *elem, const WasmLimits *l)
{
    WasmValidateStatus st = check_val_type(v, elem, v->module->n_types);

    return st != WASM_VALIDATE_DONE ? st : check_limits(v, l, false);
}

/* The first type index past type `i`'s rec group: §3.2.11 validates a group against the context that holds it. */
static size_t group_end(const WasmModule *m, size_t i)
{
    return m->types[i].rec_start + m->types[i].rec_size;
}

/* The tokens of a rec group's identity: the group, each subtype's head, and its supertypes and value types, a
   reference into the group by its index there and one out of it by the identity of the type it names. */
enum { T_GROUP = 1, T_TYPE, T_REC, T_CANON };

static bool token(WasmModuleValidator *v, uint64_t t)
{
    if (v->n_tokens == v->cap_tokens) {
        size_t cap = v->cap_tokens ? v->cap_tokens * 2 : 64;
        uint64_t *grown;

        if (cap > SIZE_MAX / sizeof *grown) return false;
        grown = realloc(v->tokens, cap * sizeof *grown);
        if (grown == NULL) return false;
        v->tokens = grown;
        v->cap_tokens = cap;
    }
    v->tokens[v->n_tokens++] = t;
    v->group_hash = (v->group_hash ^ t) * 1099511628211u;
    return true;
}

static size_t rec_start_of(const WasmModuleValidator *v, size_t x)
{
    return v->module->types[x].rec_start;
}

static bool token_ref(WasmModuleValidator *v, size_t x, size_t start, size_t size)
{
    if (x >= start && x < start + size) return token(v, T_REC) && token(v, x - start);
    return token(v, T_CANON) && token(v, v->group_canon[rec_start_of(v, x)]) && token(v, x - rec_start_of(v, x));
}

static bool token_val(WasmModuleValidator *v, const WasmValType *t, size_t start, size_t size)
{
    if (!token(v, t->code) || !token(v, t->nullable) || !token(v, t->heap)) return false;
    return t->code != WASM_TYPE_REF || t->heap != 0 || token_ref(v, t->heap_index, start, size);
}

/* A match's answer as a step's: a match continues at the next item, a climb is PH_CLIMB's from `back`. */
static WasmValidateStatus match_step(WasmModuleValidator *v, WasmMatch le, int back, WasmValidateStatus fail)
{
    if (le == WASM_MATCH_NO) return fail;
    if (le == WASM_MATCH_CLIMB) {
        v->climb_return = back;
        v->climb_fail = fail;
        v->phase = PH_CLIMB;
        return WASM_VALIDATE_MORE;
    }
    v->sub++;
    return WASM_VALIDATE_MORE;
}

/* §3.3.10's walk: one supertype a step until the walk stands on the target's identity or runs out. */
static WasmValidateStatus climb_step(WasmModuleValidator *v)
{
    WasmMatch le = wasm_climb(&v->types, &v->climb);

    if (le == WASM_MATCH_NO) return v->climb_fail;
    if (le == WASM_MATCH_YES) {
        v->phase = v->climb_return;
        v->sub++;
    }
    return WASM_VALIDATE_MORE;
}

/* §3.2.11's identity of the rec group just checked: probe the canonical groups by hash, comparing one token a step;
   an equal group's id is taken and the new tokens dropped, otherwise the group becomes a canonical id itself. */
static WasmValidateStatus canon_step(WasmModuleValidator *v)
{
    size_t len = v->n_tokens - v->group_tokens, slot = v->canon_table[v->probe], g;

    if (slot != 0) {
        g = slot - 1;
        if (v->canon_hash[g] == v->group_hash && v->canon_len[g] == len) {
            if (v->compare < len) {
                if (v->tokens[v->canon_off[g] + v->compare] == v->tokens[v->group_tokens + v->compare]) {
                    v->compare++;
                    return WASM_VALIDATE_MORE;
                }
            } else {
                v->n_tokens = v->group_tokens;
                v->group_canon[v->item] = g;
                v->phase = PH_TYPES_SUPER;
                return WASM_VALIDATE_MORE;
            }
        }
        v->probe = (v->probe + 1) & (v->cap_canon_table - 1);
        v->compare = 0;
        return WASM_VALIDATE_MORE;
    }
    g = v->n_canon++;
    v->canon_off[g] = v->group_tokens;
    v->canon_len[g] = len;
    v->canon_hash[g] = v->group_hash;
    v->canon_table[v->probe] = g + 1;
    v->group_canon[v->item] = g;
    v->phase = PH_TYPES_SUPER;
    return WASM_VALIDATE_MORE;
}

/* One item of §3.2.11's subtype check for type v->item: its supertype count, then each supertype index, then each
   value or storage type of its composite type (§3.2.10), each appended to the group's identity tokens once it
   holds. The group's last item hands the group to PH_CANON. */
static WasmValidateStatus check_type_item(WasmModuleValidator *v)
{
    const WasmModule *m = v->module;
    const WasmSubType *t = &m->types[v->item];
    size_t limit = group_end(m, v->item), k = v->sub, n_items;
    bool ok = true;
    WasmValidateStatus st;

    n_items = t->n_supers + (t->kind == WASM_COMP_FUNC ? t->n_params + t->n_results : t->n_fields);
    if (k == 0) {
        if (t->n_supers > 1) return WASM_VALIDATE_MULTIPLE_SUPERS;
        if (v->item == t->rec_start) {
            v->group_tokens = v->n_tokens;
            v->group_hash = 1469598103934665603u;
            ok = token(v, T_GROUP) && token(v, t->rec_size);
        }
        ok = ok && token(v, T_TYPE) && token(v, t->final) && token(v, t->n_supers) && token(v, t->kind)
             && token(v, t->n_params) && token(v, t->n_results) && token(v, t->n_fields);
        if (!ok) return WASM_VALIDATE_NO_ROOM;
    }
    if (k >= n_items) {
        v->sub = 0;
        v->item++;
        if (v->item == limit) {
            v->item = t->rec_start;
            v->probe = (size_t)(v->group_hash & (v->cap_canon_table - 1));
            v->compare = 0;
            v->phase = PH_CANON;
        }
        return WASM_VALIDATE_MORE;
    }
    v->sub++;
    if (k < t->n_supers) {
        size_t x = m->type_indices[t->supers + k];

        st = check_type_index(v, x, limit);
        ok = st != WASM_VALIDATE_DONE || token_ref(v, x, t->rec_start, t->rec_size);
    } else if (t->kind == WASM_COMP_FUNC) {
        const WasmValType *vt = &m->val_types[t->params + k - t->n_supers];

        st = check_val_type(v, vt, limit);
        ok = st != WASM_VALIDATE_DONE || token_val(v, vt, t->rec_start, t->rec_size);
    } else {
        const WasmFieldType *f = &m->fields[t->fields + k - t->n_supers];

        st = check_val_type(v, &f->storage, limit);
        ok = st != WASM_VALIDATE_DONE || (token(v, f->mutable_) && token_val(v, &f->storage, t->rec_start, t->rec_size));
    }
    return ok ? st : WASM_VALIDATE_NO_ROOM;
}

/* §3.2.11's conditions on type v->item's supertype: below its own index, not final, and §3.3.8's composite match,
   one field or value type a step (v->sub is 0 for the head, then 1 + the item matched). */
static WasmValidateStatus check_type_super(WasmModuleValidator *v)
{
    const WasmModule *m = v->module;
    const WasmSubType *t = &m->types[v->item], *s;
    size_t x = v->item, k;

    if (t->n_supers == 0 || (v->sub > 0 && v->sub - 1 >= (t->kind == WASM_COMP_FUNC
                                                            ? t->n_params + t->n_results
                                                            : m->types[m->type_indices[t->supers]].n_fields))) {
        v->sub = 0;
        v->item++;
        if (v->item == group_end(m, x)) v->phase = PH_TYPES;
        return WASM_VALIDATE_MORE;
    }
    v->index = x;
    v->index2 = m->type_indices[t->supers];
    s = &m->types[v->index2];
    if (v->sub == 0) {
        if (v->index2 >= x) return WASM_VALIDATE_FORWARD_SUPER;
        if (s->final) return WASM_VALIDATE_FINAL_SUPER;
        if (t->kind != s->kind) return WASM_VALIDATE_SUB_MISMATCH;
        if (t->kind == WASM_COMP_FUNC && (t->n_params != s->n_params || t->n_results != s->n_results))
            return WASM_VALIDATE_SUB_MISMATCH;
        if (t->n_fields < s->n_fields) return WASM_VALIDATE_SUB_MISMATCH;
        v->sub = 1;
        return WASM_VALIDATE_MORE;
    }
    k = v->sub - 1;
    if (t->kind != WASM_COMP_FUNC)
        return match_step(v, wasm_field_le(&v->types, &m->fields[t->fields + k], &m->fields[s->fields + k], &v->climb),
                          PH_TYPES_SUPER, WASM_VALIDATE_SUB_MISMATCH);
    if (k < t->n_params)   /* parameters are contravariant */
        return match_step(v, wasm_val_le(&v->types, &m->val_types[s->params + k], &m->val_types[t->params + k],
                                         &v->climb), PH_TYPES_SUPER, WASM_VALIDATE_SUB_MISMATCH);
    k -= t->n_params;
    return match_step(v, wasm_val_le(&v->types, &m->val_types[t->results + k], &m->val_types[s->results + k],
                                     &v->climb), PH_TYPES_SUPER, WASM_VALIDATE_SUB_MISMATCH);
}

/* §3.5.11: an import's external type is valid (§3.2.17). */
static WasmValidateStatus check_import(WasmModuleValidator *v, const WasmImport *im)
{
    switch (im->kind) {
    case WASM_EXTERN_FUNC: return check_func_type(v, im->index, NULL);
    case WASM_EXTERN_TABLE: return check_table_type(v, &im->type, &im->limits);
    case WASM_EXTERN_MEMORY: return check_limits(v, &im->limits, true);
    case WASM_EXTERN_GLOBAL: return check_val_type(v, &im->type, v->module->n_types);
    case WASM_EXTERN_TAG: return check_tag_type(v, im->index);
    }
    CHECK_FAIL("a decoded WebAssembly import has a kind module_decoder.h does not produce");
}

/* §3.5.12: an export's index names an entity of its kind. */
static WasmValidateStatus check_export(WasmModuleValidator *v, const WasmExport *e)
{
    static const char *const SPACE[] = { "function", "table", "memory", "global", "tag" };
    size_t n[] = { v->n_funcs, v->n_tables, v->n_memories, v->n_globals, v->n_tags };

    CHECK(e->kind <= WASM_EXTERN_TAG, "a decoded WebAssembly export has a kind module_decoder.h does not produce");
    return e->index < n[e->kind] ? WASM_VALIDATE_DONE : unknown(v, SPACE[e->kind], e->index);
}

static uint64_t name_hash(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603u;   /* FNV-1a */
    size_t i;

    for (i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211u;
    return h;
}

/* §3.5.13's nm* disjoint: export v->item's name against those before it, through an open-addressed set of
   export indices plus one. */
static WasmValidateStatus check_name(WasmModuleValidator *v)
{
    const WasmModule *m = v->module;
    const WasmExport *e = &m->exports[v->item];
    size_t at;

    if (v->names == NULL) {
        size_t cap = 8;

        while (cap < 2 * m->n_exports) {
            if (cap > SIZE_MAX / 4 / sizeof *v->names) return WASM_VALIDATE_NO_ROOM;
            cap *= 2;
        }
        v->names = calloc(cap, sizeof *v->names);
        if (v->names == NULL) return WASM_VALIDATE_NO_ROOM;
        v->cap_names = cap;
    }
    at = (size_t)(name_hash(v->bytes + e->name_offset, e->name_size) & (v->cap_names - 1));
    while (v->names[at] != 0) {
        const WasmExport *o = &m->exports[v->names[at] - 1];

        if (o->name_size == e->name_size
            && memcmp(v->bytes + o->name_offset, v->bytes + e->name_offset, e->name_size) == 0) {
            v->index = v->item;
            return WASM_VALIDATE_DUPLICATE_EXPORT;
        }
        at = (at + 1) & (v->cap_names - 1);
    }
    v->names[at] = v->item + 1;
    v->item++;
    return WASM_VALIDATE_MORE;
}

/* Table `x`'s element type: an imported table's, then a defined one's. */
static const WasmValType *table_type(const WasmModuleValidator *v, size_t x)
{
    size_t imported = v->n_tables - v->module->n_tables;

    DCHECK(x < v->n_tables && v->n_table_imports == imported, "a WebAssembly table's type was asked before its "
                                                               "imports were read, or past the tables");
    if (x < imported) return &v->module->imports[v->table_imports[x]].type;
    return &v->module->tables[x - imported].type;
}

/* Move to `next` when the current list of `n` items is done; otherwise answer whether v->item may be checked. */
static bool list_done(WasmModuleValidator *v, size_t n, int next)
{
    if (v->item < n) return false;
    v->item = 0;
    v->phase = next;
    return true;
}

static WasmValidateStatus step_list(WasmModuleValidator *v)
{
    const WasmModule *m = v->module;
    WasmValidateStatus st = WASM_VALIDATE_DONE;
    size_t i = v->item;

    switch (v->phase) {
    case PH_IMPORTS:
        if (list_done(v, m->n_imports, PH_TAGS)) return WASM_VALIDATE_MORE;
        st = check_import(v, &m->imports[i]);
        if (m->imports[i].kind == WASM_EXTERN_TABLE) v->table_imports[v->n_table_imports++] = i;
        break;
    case PH_TAGS:
        if (list_done(v, m->n_tags, PH_FUNCS)) return WASM_VALIDATE_MORE;
        st = check_tag_type(v, m->tags[i]);
        break;
    case PH_FUNCS:
        if (list_done(v, m->n_func_types, PH_MEMORIES)) return WASM_VALIDATE_MORE;
        st = check_func_type(v, m->func_types[i], NULL);
        break;
    case PH_MEMORIES:
        if (list_done(v, m->n_memories, PH_TABLES)) return WASM_VALIDATE_MORE;
        st = check_limits(v, &m->memories[i], true);
        break;
    case PH_TABLES:
        if (list_done(v, m->n_tables, PH_GLOBALS)) return WASM_VALIDATE_MORE;
        st = check_table_type(v, &m->tables[i].type, &m->tables[i].limits);
        break;
    case PH_GLOBALS:
        if (list_done(v, m->n_globals, PH_DATAS)) return WASM_VALIDATE_MORE;
        st = check_val_type(v, &m->globals[i].type, m->n_types);
        break;
    case PH_DATAS:
        /* §3.5.8: an active segment's memory exists. */
        if (list_done(v, m->n_datas, PH_ELEMS)) return WASM_VALIDATE_MORE;
        if (m->datas[i].mode == WASM_SEGMENT_ACTIVE && m->datas[i].memory >= v->n_memories)
            st = unknown(v, "memory", m->datas[i].memory);
        break;
    case PH_ELEMS:
        /* §3.5.9: the segment's reference type is valid, and an active segment's table exists and its element type
           matches the segment's (v->sub is 1 once it has). */
        if (list_done(v, m->n_elems, PH_START)) return WASM_VALIDATE_MORE;
        if (v->sub == 0) {
            st = check_val_type(v, &m->elems[i].type, m->n_types);
            if (st != WASM_VALIDATE_DONE) return st;
            if (m->elems[i].mode == WASM_SEGMENT_ACTIVE) {
                if (m->elems[i].table >= v->n_tables) return unknown(v, "table", m->elems[i].table);
                v->index = i;
                return match_step(v, wasm_val_le(&v->types, &m->elems[i].type, table_type(v, m->elems[i].table),
                                                 &v->climb), PH_ELEMS, WASM_VALIDATE_ELEM_MISMATCH);
            }
        }
        v->sub = 0;
        v->item++;
        return WASM_VALIDATE_MORE;
    case PH_EXPORTS:
        if (list_done(v, m->n_exports, PH_NAMES)) return WASM_VALIDATE_MORE;
        st = check_export(v, &m->exports[i]);
        break;
    default:
        CHECK_FAIL("a WebAssembly validation list phase this file does not define");
    }
    v->item++;
    return st == WASM_VALIDATE_DONE ? WASM_VALIDATE_MORE : st;
}

/* §3.5.10: the start function exists and its type is [] -> []. */
static WasmValidateStatus check_start(WasmModuleValidator *v)
{
    const WasmModule *m = v->module;
    size_t x = m->start, i, k = 0;
    uint32_t type = 0;

    v->phase = PH_EXPORTS;
    if (!m->has_start) return WASM_VALIDATE_MORE;
    if (x >= v->n_funcs) return unknown(v, "function", x);
    for (i = 0; i < m->n_imports; i++) {
        if (m->imports[i].kind != WASM_EXTERN_FUNC) continue;
        if (k++ == x) type = m->imports[i].index;
    }
    if (x >= k) type = m->func_types[x - k];
    if (m->types[type].n_params != 0 || m->types[type].n_results != 0) return WASM_VALIDATE_START_TYPE;
    return WASM_VALIDATE_MORE;
}

WasmValidateStatus wasm_module_validator_step(WasmModuleValidator *v)
{
    WasmValidateStatus st;

    DCHECK(v != NULL && v->phase != PH_FINISHED, "a WebAssembly validation was stepped after it answered");
    switch (v->phase) {
    case PH_ALLOC:
        st = allocate(v);
        break;
    case PH_CANON:
        st = canon_step(v);
        break;
    case PH_CLIMB:
        st = climb_step(v);
        break;
    case PH_TYPES:
        if (v->item == v->module->n_types) {
            v->item = 0;
            v->phase = PH_IMPORTS;
            st = WASM_VALIDATE_MORE;
            break;
        }
        st = check_type_item(v);
        if (st == WASM_VALIDATE_DONE) st = WASM_VALIDATE_MORE;   /* an item that holds is not the answer */
        break;
    case PH_TYPES_SUPER:
        st = check_type_super(v);
        break;
    case PH_START:
        st = check_start(v);
        break;
    case PH_NAMES:
        st = v->item == v->module->n_exports ? WASM_VALIDATE_DONE : check_name(v);
        break;
    default:
        st = step_list(v);
        break;
    }
    if (st != WASM_VALIDATE_MORE) v->phase = PH_FINISHED;
    return st;
}

WasmValidateStatus wasm_module_validator_run(WasmModuleValidator *v)
{
    WasmValidateStatus st;

    do st = wasm_module_validator_step(v);
    while (st == WASM_VALIDATE_MORE);
    return st;
}

/* valid.ml's string_of_reftype: `(ref null? <heap type>)`. */
static void ref_text(const WasmValType *t, char *buf, size_t cap)
{
    static const char *const HEAP[] = { "exn", "array", "struct", "i31", "eq", "any", "extern", "func", "none",
                                        "noextern", "nofunc", "noexn" };

    if (t->heap != 0) snprintf(buf, cap, "(ref %s%s)", t->nullable ? "null " : "", HEAP[t->heap - WASM_HEAP_EXN]);
    else snprintf(buf, cap, "(ref %s%u)", t->nullable ? "null " : "", (unsigned)t->heap_index);
}

void wasm_module_validator_message(const WasmModuleValidator *v, WasmValidateStatus st, char *buf, size_t cap)
{
    const WasmExport *e;

    DCHECK(v != NULL && buf != NULL && cap > 0, "a WebAssembly validation's message was asked with nowhere to write");
    switch (st) {
    case WASM_VALIDATE_DONE:
    case WASM_VALIDATE_MORE: snprintf(buf, cap, "%s", ""); return;
    case WASM_VALIDATE_UNKNOWN_INDEX:
        snprintf(buf, cap, "unknown %s %llu", v->space, (unsigned long long)v->index);
        return;
    case WASM_VALIDATE_NOT_FUNCTION:
        snprintf(buf, cap, "non-function type %llu", (unsigned long long)v->index);
        return;
    case WASM_VALIDATE_MULTIPLE_SUPERS: snprintf(buf, cap, "multiple supertypes"); return;
    case WASM_VALIDATE_FORWARD_SUPER:
        snprintf(buf, cap, "forward use of type %llu in sub type definition", (unsigned long long)v->index2);
        return;
    case WASM_VALIDATE_FINAL_SUPER:
        snprintf(buf, cap, "sub type %llu has final super type %llu", (unsigned long long)v->index,
                 (unsigned long long)v->index2);
        return;
    case WASM_VALIDATE_LIMITS_RANGE:
        if (strcmp(v->space, "memory") == 0)
            snprintf(buf, cap, "memory size must be at most %s",
                     v->index ? "2^48 pages (256 TiB) for i64" : "2^16 pages (4 GiB) for i32");
        else snprintf(buf, cap, "table size must be at most %s", v->index ? "2^64-1 for i64" : "2^32-1 for i32");
        return;
    case WASM_VALIDATE_LIMITS_ORDER: snprintf(buf, cap, "size minimum must not be greater than maximum"); return;
    case WASM_VALIDATE_TAG_RESULT: snprintf(buf, cap, "non-empty tag result type"); return;
    case WASM_VALIDATE_START_TYPE: snprintf(buf, cap, "start function must not have parameters or results"); return;
    case WASM_VALIDATE_DUPLICATE_EXPORT:
        e = &v->module->exports[v->index];
        snprintf(buf, cap, "duplicate export name \"%.*s\"", (int)e->name_size,
                 (const char *)v->bytes + e->name_offset);
        return;
    case WASM_VALIDATE_SUB_MISMATCH:
        snprintf(buf, cap, "sub type %llu does not match super type %llu", (unsigned long long)v->index,
                 (unsigned long long)v->index2);
        return;
    case WASM_VALIDATE_ELEM_MISMATCH: {
        char a[64], b[64];

        ref_text(&v->module->elems[v->index].type, a, sizeof a);
        ref_text(table_type(v, v->module->elems[v->index].table), b, sizeof b);
        snprintf(buf, cap, "type mismatch: element segment's type %s does not match table's element type %s", a, b);
        return;
    }
    case WASM_VALIDATE_NO_ROOM: snprintf(buf, cap, "out of memory"); return;
    }
    CHECK_FAIL("a WebAssembly validation status this file does not define");
}

void wasm_module_validator_free(WasmModuleValidator *v)
{
    DCHECK(v != NULL, "a WebAssembly validation was freed with no validator");
    free(v->names);
    free(v->group_canon);
    free(v->tokens);
    free(v->canon_off);
    free(v->canon_len);
    free(v->canon_hash);
    free(v->canon_table);
    free(v->table_imports);
    memset(v, 0, sizeof *v);
}
