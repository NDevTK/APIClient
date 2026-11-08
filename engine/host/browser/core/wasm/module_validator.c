/* WebAssembly Core Specification (Release 3.0) §3.5 "Modules": module validation. See module_validator.h. */
#include "core/wasm/module_validator.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

/* The phases, in valid.ml's check_module order: each rec group's subtypes, then their supertype conditions;
   imports, tags, functions, memories, tables, globals, data, elements; the start function; exports, then the
   disjointness of their names. */
enum {
    PH_TYPES = 0, PH_TYPES_SUPER, PH_IMPORTS, PH_TAGS, PH_FUNCS, PH_MEMORIES, PH_TABLES, PH_GLOBALS, PH_DATAS,
    PH_ELEMS, PH_START, PH_EXPORTS, PH_NAMES, PH_FINISHED
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
    v->phase = PH_TYPES;
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

/* One item of §3.2.11's subtype check for type v->item: its supertype count, then each supertype index, then each
   value or storage type of its composite type (§3.2.10). */
static WasmValidateStatus check_type_item(WasmModuleValidator *v)
{
    const WasmModule *m = v->module;
    const WasmSubType *t = &m->types[v->item];
    size_t limit = group_end(m, v->item), k = v->sub, n_items;

    n_items = t->n_supers + (t->kind == WASM_COMP_FUNC ? t->n_params + t->n_results : t->n_fields);
    if (k == 0 && t->n_supers > 1) return WASM_VALIDATE_MULTIPLE_SUPERS;
    if (k >= n_items) {
        v->sub = 0;
        v->item++;
        /* The group's subtypes are all checked: its supertype conditions come next, from its first type. */
        if (v->item == limit) {
            v->item = t->rec_start;
            v->phase = PH_TYPES_SUPER;
        }
        return WASM_VALIDATE_MORE;
    }
    v->sub++;
    if (k < t->n_supers) return check_type_index(v, m->type_indices[t->supers + k], limit);
    k -= t->n_supers;
    if (t->kind == WASM_COMP_FUNC) return check_val_type(v, &m->val_types[t->params + k], limit);
    return check_val_type(v, &m->fields[t->fields + k].storage, limit);
}

/* §3.2.11's conditions on type v->item's supertype: below its own index and not final. Its composite type's match
   against the supertype's is the header's residual. */
static WasmValidateStatus check_type_super(WasmModuleValidator *v)
{
    const WasmModule *m = v->module;
    const WasmSubType *t = &m->types[v->item];
    size_t x = v->item;

    v->item++;
    if (v->item == group_end(m, x)) v->phase = PH_TYPES;
    if (t->n_supers == 0) return WASM_VALIDATE_MORE;
    v->index = x;
    v->index2 = m->type_indices[t->supers];
    if (v->index2 >= x) return WASM_VALIDATE_FORWARD_SUPER;
    if (m->types[v->index2].final) return WASM_VALIDATE_FINAL_SUPER;
    return WASM_VALIDATE_MORE;
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
        /* §3.5.9: the segment's reference type is valid and an active segment's table exists. */
        if (list_done(v, m->n_elems, PH_START)) return WASM_VALIDATE_MORE;
        st = check_val_type(v, &m->elems[i].type, m->n_types);
        if (st == WASM_VALIDATE_DONE && m->elems[i].mode == WASM_SEGMENT_ACTIVE && m->elems[i].table >= v->n_tables)
            st = unknown(v, "table", m->elems[i].table);
        break;
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
    case WASM_VALIDATE_NO_ROOM: snprintf(buf, cap, "out of memory"); return;
    }
    CHECK_FAIL("a WebAssembly validation status this file does not define");
}

void wasm_module_validator_free(WasmModuleValidator *v)
{
    DCHECK(v != NULL, "a WebAssembly validation was freed with no validator");
    free(v->names);
    v->names = NULL;
    v->cap_names = 0;
}
