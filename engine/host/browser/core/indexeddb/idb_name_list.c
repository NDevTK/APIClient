/* INDEXED DATABASE §5.12 — create a sorted name list. See idb_name_list.h. */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/html/dom_string_list.h"
#include "core/indexeddb/idb_key.h"
#include "core/indexeddb/idb_name_list.h"

static uint32_t nl_len(JSContext *ctx, JSValueConst names)
{
    JSValue len = JS_GetPropertyStr(ctx, names, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    return n;
}

/* WHERE `s` BELONGS in the sorted prefix `v[0..n)`, found BINARY — the shape core/indexeddb/idb_database.c's
   record search and idb_index.c's already have, and for the same reason: the comparison is the expensive part
   (it materialises both strings as UTF-16 to compare them as code units), so a linear scan would make this
   O(n^2) comparisons over a set a page is free to make as large as it likes. */
static uint32_t nl_place(JSContext *ctx, JSValue *v, uint32_t n, JSValueConst s)
{
    uint32_t lo = 0, hi = n;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int c = idb_code_unit_compare(ctx, v[mid], s);

        DCHECK(c != 0, "§5.12 was given a list holding one name TWICE — the names come from a SET (§2.1's set "
                       "of object stores, §2.2.1's index set, §2.7's scope), whose members are unique by "
                       "definition, so a duplicate means the set itself is broken and the sorted list would "
                       "report a store that is not there");
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return lo;
}

JSValue idb_sorted_name_list(JSContext *ctx, JSValue names)
{
    uint32_t n, i;
    JSValue *v, sorted;

    DCHECK(JS_IsArray(names), "§5.12 was given something that is not a list of names");
    n = nl_len(ctx, names);
    v = n ? js_malloc(ctx, n * sizeof *v) : NULL;
    CHECK(n == 0 || v != NULL, "IndexedDB: §5.12 could not allocate its sorted list");
    for (i = 0; i < n; i++) {
        JSValue s = JS_GetPropertyUint32(ctx, names, i);
        uint32_t at;

        DCHECK(JS_IsString(s), "§5.12 was given a list holding something that is not a name — every caller "
                               "builds it from the NAME field of the records in its set, and a non-string there "
                               "means the record's name is not the string §2.1/§2.2/§2.6 give it");
        at = nl_place(ctx, v, i, s);
        memmove(v + at + 1, v + at, (i - at) * sizeof *v);
        v[at] = s;
    }
    JS_FreeValue(ctx, names);   /* CONSUMED — the entries live on in v */
    sorted = JS_NewArray(ctx);
    CHECK(!JS_IsException(sorted), "IndexedDB: §5.12's sorted list could not be allocated");
    for (i = 0; i < n; i++)
        JS_DefinePropertyValueUint32(ctx, sorted, i, v[i], JS_PROP_C_W_E);
    js_free(ctx, v);
    return dom_string_list_new(ctx, sorted);
}
