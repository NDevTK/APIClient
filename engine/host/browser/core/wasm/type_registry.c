/* WebAssembly Core Specification (Release 3.0) §3.2.11's rec group identity. See type_registry.h. */
#include "core/wasm/type_registry.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

void wasm_type_registry_init(WasmTypeRegistry *r)
{
    DCHECK(r != NULL, "a WebAssembly type registry was initialized with no record");
    memset(r, 0, sizeof *r);
    r->place = SIZE_MAX;
}

void wasm_type_registry_free(WasmTypeRegistry *r)
{
    DCHECK(r != NULL, "a WebAssembly type registry was freed with no record");
    free(r->tokens);
    free(r->off);
    free(r->len);
    free(r->hash);
    free(r->table);
    free(r->old);
    memset(r, 0, sizeof *r);
}

void wasm_type_intern_begin(WasmTypeIntern *in, const uint64_t *tokens, size_t len, uint64_t hash)
{
    DCHECK(in != NULL && tokens != NULL && len > 0, "a WebAssembly rec group was interned with no tokens");
    memset(in, 0, sizeof *in);
    in->tokens = tokens;
    in->len = len;
    in->hash = hash;
    in->probe = SIZE_MAX;
}

/* One slot of the rehash: old[moved] probed into `table` one slot a step, an empty old slot skipped. */
static void rehash_step(WasmTypeRegistry *r)
{
    size_t slot = r->old[r->moved];

    if (slot == 0) {
        r->moved++;
    } else if (r->place == SIZE_MAX) {
        r->place = (size_t)(r->hash[slot - 1] & (r->cap_table - 1));
    } else if (r->table[r->place] == 0) {
        r->table[r->place] = slot;
        r->place = SIZE_MAX;
        r->moved++;
    } else {
        r->place = (r->place + 1) & (r->cap_table - 1);
    }
    if (r->moved == r->cap_old) {
        free(r->old);
        r->old = NULL;
        r->cap_old = r->moved = 0;
    }
}

/* Double the table and start emptying the old one into it; the generation moves, so every probe starts over. */
static bool grow_table(WasmTypeRegistry *r)
{
    size_t cap = r->cap_table ? r->cap_table * 2 : 8;
    size_t *table;

    if (cap > SIZE_MAX / sizeof *table) return false;
    table = calloc(cap, sizeof *table);
    if (table == NULL) return false;
    r->old = r->table;
    r->cap_old = r->old ? r->cap_table : 0;
    r->moved = 0;
    r->place = SIZE_MAX;
    r->table = table;
    r->cap_table = cap;
    r->generation++;
    return true;
}

/* Grow `*p`, an array of `*cap` items of `size` bytes, to at least `need` by doubling. A growth that fails leaves
   the array as it was; one that succeeds may leave a sibling array larger than its recorded capacity. */
static bool reserve(void **p, size_t cap, size_t need, size_t size, size_t *grown_cap)
{
    size_t n = cap ? cap : 8;
    void *grown;

    while (n < need) {
        if (n > SIZE_MAX / 2 / size) return false;
        n *= 2;
    }
    if (n == cap) {
        *grown_cap = cap;
        return true;
    }
    grown = realloc(*p, n * size);
    if (grown == NULL) return false;
    *p = grown;
    *grown_cap = n;
    return true;
}

/* A new canonical id for the group, at the empty slot its probe reached: the tokens are copied in this one step
   (a copy of what its builder made one token a step), so a reader never sees an id whose tokens are partial. */
static WasmInternStatus insert(WasmTypeRegistry *r, WasmTypeIntern *in)
{
    size_t cap_tokens, cap = r->cap;

    if (in->len > SIZE_MAX - r->n_tokens
        || !reserve((void **)&r->tokens, r->cap_tokens, r->n_tokens + in->len, sizeof *r->tokens, &cap_tokens))
        return WASM_INTERN_NO_ROOM;
    r->cap_tokens = cap_tokens;
    if (!reserve((void **)&r->off, r->cap, r->n + 1, sizeof *r->off, &cap)
        || !reserve((void **)&r->len, r->cap, r->n + 1, sizeof *r->len, &cap)
        || !reserve((void **)&r->hash, r->cap, r->n + 1, sizeof *r->hash, &cap))
        return WASM_INTERN_NO_ROOM;
    r->cap = cap;
    memcpy(r->tokens + r->n_tokens, in->tokens, in->len * sizeof *in->tokens);
    r->off[r->n] = r->n_tokens;
    r->len[r->n] = in->len;
    r->hash[r->n] = in->hash;
    r->n_tokens += in->len;
    r->table[in->probe] = r->n + 1;
    in->id = r->n++;
    return WASM_INTERN_DONE;
}

WasmInternStatus wasm_type_intern_step(WasmTypeRegistry *r, WasmTypeIntern *in)
{
    size_t slot, g;

    DCHECK(r != NULL && in != NULL && in->tokens != NULL, "a WebAssembly rec group was interned with no registry");
    if (r->old != NULL) {
        rehash_step(r);
        return WASM_INTERN_MORE;
    }
    if (r->cap_table == 0 || (r->n + 1) > r->cap_table / 2) {
        DCHECK(r->n <= r->cap_table / 2, "a WebAssembly type registry's table is more than half full");
        return grow_table(r) ? WASM_INTERN_MORE : WASM_INTERN_NO_ROOM;
    }
    if (in->probe == SIZE_MAX || in->generation != r->generation) {
        in->probe = (size_t)(in->hash & (r->cap_table - 1));
        in->compare = 0;
        in->generation = r->generation;
        return WASM_INTERN_MORE;
    }
    slot = r->table[in->probe];
    if (slot == 0) return insert(r, in);
    g = slot - 1;
    if (r->hash[g] == in->hash && r->len[g] == in->len) {
        if (in->compare == in->len) {
            in->id = g;
            return WASM_INTERN_DONE;
        }
        if (r->tokens[r->off[g] + in->compare] == in->tokens[in->compare]) {
            in->compare++;
            return WASM_INTERN_MORE;
        }
    }
    in->probe = (in->probe + 1) & (r->cap_table - 1);
    in->compare = 0;
    return WASM_INTERN_MORE;
}
