/* HTML §8.1.5.3 "Import map processing model". See import_map.h. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/realm.h"
#include "core/url/url.h"
#include "core/loader/import_map.h"
#include <lexbor/core/base.h>   /* lexbor_realloc: the allocator the engine gives its refusal edge */

/* The per-realm record: a null-prototype object whose four properties are the global's import map's three items
   and its resolved module set. The slot holds one object for the realm's life and is never replaced; what a flow
   writes is the record's properties, which the heap COW captures. Each ordered map is an Array of two-element
   Arrays in map order:
     imports    [specifier key, URL serialization or null]       — a module specifier map
     scopes     [scope prefix (a URL serialization), imports-shaped Array]
     integrity  [URL serialization, integrity metadata string]   — a module integrity map
     resolved   [serialized base URL, specifier, bare-or-special] — specifier resolution records, with
                specifier as a URL replaced by the boolean §8.1.5.3 permits ("the specifier is either bare or
                URL-like that is special"), which is all the merge reads of it. */
static JSClassID g_slot = JS_INVALID_CLASS_ID;
static int       g_ready;

/* ---- the C form the algorithms run over ------------------------------------------------------------------- */

typedef struct { char *key; char *value; } ImEntry;   /* value NULL is the map's null */
typedef struct { ImEntry *e; int n, cap; } ImMap;
typedef struct { char *prefix; ImMap map; } ImScope;
typedef struct { ImScope *s; int n, cap; } ImScopes;
typedef struct { ImMap imports; ImScopes scopes; ImMap integrity; } ImportMap;
typedef struct { char *base; char *specifier; bool bare_or_special; } ImRecord;

static char *im_strdup(const char *s)
{
    char *d = strdup(s);
    CHECK(d != NULL, "import map: OOM copying a string");
    return d;
}

static void im_map_free(ImMap *m)
{
    for (int i = 0; i < m->n; i++) { free(m->e[i].key); free(m->e[i].value); }
    free(m->e);
    m->e = NULL; m->n = m->cap = 0;
}

static void im_scopes_free(ImScopes *s)
{
    for (int i = 0; i < s->n; i++) { free(s->s[i].prefix); im_map_free(&s->s[i].map); }
    free(s->s);
    s->s = NULL; s->n = s->cap = 0;
}

static void im_free(ImportMap *m)
{
    im_map_free(&m->imports);
    im_scopes_free(&m->scopes);
    im_map_free(&m->integrity);
}

static int im_map_find(const ImMap *m, const char *key)
{
    for (int i = 0; i < m->n; i++)
        if (!strcmp(m->e[i].key, key)) return i;
    return -1;
}

/* Infra's set: replace the value of an existing key in place, otherwise append. Takes both strings. */
static void im_map_set(ImMap *m, char *key, char *value)
{
    int at = im_map_find(m, key);

    if (at >= 0) { free(key); free(m->e[at].value); m->e[at].value = value; return; }
    if (m->n == m->cap) {
        int nc = m->cap ? m->cap * 2 : 8;
        ImEntry *ne = lexbor_realloc(m->e, (size_t)nc * sizeof *ne);
        CHECK(ne != NULL, "import map: OOM growing a map");
        m->e = ne; m->cap = nc;
    }
    m->e[m->n].key = key;
    m->e[m->n].value = value;
    m->n++;
}

static void im_map_remove(ImMap *m, int at)
{
    free(m->e[at].key);
    free(m->e[at].value);
    memmove(&m->e[at], &m->e[at + 1], (size_t)(m->n - at - 1) * sizeof m->e[0]);
    m->n--;
}

static void im_map_copy(ImMap *dst, const ImMap *src)
{
    memset(dst, 0, sizeof *dst);
    for (int i = 0; i < src->n; i++)
        im_map_set(dst, im_strdup(src->e[i].key), src->e[i].value ? im_strdup(src->e[i].value) : NULL);
}

static int im_scopes_find(const ImScopes *s, const char *prefix)
{
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->s[i].prefix, prefix)) return i;
    return -1;
}

/* Infra's set over the scopes map. Takes `prefix` and the contents of `map`. */
static void im_scopes_set(ImScopes *s, char *prefix, ImMap *map)
{
    int at = im_scopes_find(s, prefix);

    if (at >= 0) {
        free(prefix);
        im_map_free(&s->s[at].map);
        s->s[at].map = *map;
        return;
    }
    if (s->n == s->cap) {
        int nc = s->cap ? s->cap * 2 : 4;
        ImScope *ns = lexbor_realloc(s->s, (size_t)nc * sizeof *ns);
        CHECK(ns != NULL, "import map: OOM growing the scopes map");
        s->s = ns; s->cap = nc;
    }
    s->s[s->n].prefix = prefix;
    s->s[s->n].map = *map;
    s->n++;
}

static bool im_ends_with_slash(const char *s)
{
    size_t n = strlen(s);
    return n > 0 && s[n - 1] == '/';
}

static bool im_prefix_of(const char *prefix, const char *s)
{
    return !strncmp(prefix, s, strlen(prefix));
}

/* One UTF-16 code unit of a UTF-8 (or WTF-8) string, advancing `*p`; `*pending` holds a supplementary code
   point's low surrogate between calls. Returns -1 at the end. */
static int im_next_unit(const unsigned char **p, int *pending)
{
    const unsigned char *s = *p;
    unsigned cp;

    if (*pending >= 0) { int u = *pending; *pending = -1; return u; }
    if (!*s) return -1;
    if (s[0] < 0x80)                     { cp = s[0]; s += 1; }
    else if ((s[0] & 0xE0) == 0xC0 && s[1]) { cp = ((s[0] & 0x1Fu) << 6) | (s[1] & 0x3Fu); s += 2; }
    else if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) {
        cp = ((s[0] & 0x0Fu) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu); s += 3;
    } else if ((s[0] & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
        cp = ((s[0] & 0x07u) << 18) | ((s[1] & 0x3Fu) << 12) | ((s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu); s += 4;
    } else { cp = s[0]; s += 1; }
    *p = s;
    if (cp >= 0x10000) {
        cp -= 0x10000;
        *pending = (int)(0xDC00 + (cp & 0x3FF));
        return (int)(0xD800 + (cp >> 10));
    }
    return (int)cp;
}

/* Infra's code unit less than, over two strings this file holds as UTF-8. */
static bool im_code_unit_less(const char *a, const char *b)
{
    const unsigned char *pa = (const unsigned char *)a, *pb = (const unsigned char *)b;
    int qa = -1, qb = -1;

    for (;;) {
        int ua = im_next_unit(&pa, &qa), ub = im_next_unit(&pb, &qb);
        if (ua != ub) return ua < ub;   /* the end (-1) is less than every unit: a prefix is less */
        if (ua < 0) return false;
    }
}

/* "Sorting in descending order …, with an entry a being less than an entry b if a's key is code unit less than
   b's key" — an insertion sort, stable, over keys that are unique by construction. */
static void im_map_sort_descending(ImMap *m)
{
    for (int i = 1; i < m->n; i++) {
        ImEntry t = m->e[i];
        int j = i - 1;
        while (j >= 0 && im_code_unit_less(m->e[j].key, t.key)) { m->e[j + 1] = m->e[j]; j--; }
        m->e[j + 1] = t;
    }
}

static void im_scopes_sort_descending(ImScopes *s)
{
    for (int i = 1; i < s->n; i++) {
        ImScope t = s->s[i];
        int j = i - 1;
        while (j >= 0 && im_code_unit_less(s->s[j].prefix, t.prefix)) { s->s[j + 1] = s->s[j]; j--; }
        s->s[j + 1] = t;
    }
}

static char *im_serialize(const UrlRecord *u)
{
    char *s = url_serialize(u, false);
    CHECK(s != NULL, "import map: OOM serializing a URL");
    return s;
}

/* HTML §8.1.5.1's resolve a URL-like module specifier. Returns the serialization (owned) or NULL. */
static char *im_resolve_url_like(const char *specifier, const UrlRecord *base)
{
    UrlRecord u;
    char *s = NULL;
    bool ok;

    url_record_init(&u);
    if (!strncmp(specifier, "/", 1) || !strncmp(specifier, "./", 2) || !strncmp(specifier, "../", 3))
        ok = url_parse(&u, specifier, strlen(specifier), base);
    else
        ok = url_parse(&u, specifier, strlen(specifier), NULL);
    if (ok) s = im_serialize(&u);
    url_record_free(&u);
    return s;
}

/* ---- the per-realm record ---------------------------------------------------------------------------------- */

static JSValue im_record(JSContext *ctx)
{
    JSValue r;

    DCHECK(g_ready, "a global's import map was reached before HTML §8.1.5.3 was declared");
    r = realm_value_get(ctx, g_slot);
    DCHECK(JS_IsObject(r),
           "a realm answered its global's import map with no record — every realm is given one at creation, so "
           "this realm never ran import_map_install");
    return r;
}

/* The `which` Array of the record, owned. */
static JSValue im_record_list(JSContext *ctx, const char *which)
{
    JSValue r = im_record(ctx), l = JS_GetPropertyStr(ctx, r, which);

    JS_FreeValue(ctx, r);
    DCHECK(JS_IsArray(l), "an import map record's list is not an Array — import_map_install and the merge write "
                          "only Arrays there");
    return l;
}

static uint32_t im_length(JSContext *ctx, JSValueConst arr)
{
    JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = 0;

    CHECK(JS_ToUint32(ctx, &n, lv) == 0, "import map: an Array this file built has no length");
    JS_FreeValue(ctx, lv);
    return n;
}

/* A string member of a pair this file built, owned C copy, or NULL for the map's null. */
static char *im_pair_string(JSContext *ctx, JSValueConst pair, uint32_t k, bool nullable)
{
    JSValue v = JS_GetPropertyUint32(ctx, pair, k);
    const char *s;
    char *out;

    if (JS_IsNull(v)) {
        DCHECK(nullable, "an import map record holds a null where its layout has a string");
        return NULL;
    }
    DCHECK(JS_IsString(v), "an import map record holds a member that is neither a string nor null");
    s = JS_ToCString(ctx, v);
    CHECK(s != NULL, "import map: OOM reading a record string");
    out = im_strdup(s);
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
    return out;
}

static void im_map_from_js(JSContext *ctx, JSValueConst arr, ImMap *out)
{
    uint32_t n = im_length(ctx, arr);

    memset(out, 0, sizeof *out);
    for (uint32_t i = 0; i < n; i++) {
        JSValue pair = JS_GetPropertyUint32(ctx, arr, i);
        char *key = im_pair_string(ctx, pair, 0, false);
        char *value = im_pair_string(ctx, pair, 1, true);
        JS_FreeValue(ctx, pair);
        im_map_set(out, key, value);
    }
}

/* CreateDataProperty on an Array this file built: a [[Set]] would consult Array.prototype, where a page can
   have put a setter for an index, and this code runs with no flow base to run it on. Consumes `v`. */
static void im_put(JSContext *ctx, JSValueConst arr, uint32_t i, JSValue v)
{
    CHECK(JS_DefinePropertyValueUint32(ctx, arr, i, v, JS_PROP_C_W_E) >= 0,
          "import map: OOM writing a record entry");
}

static JSValue im_string_or_null(JSContext *ctx, const char *s)
{
    JSValue v;

    if (!s) return JS_NULL;
    v = JS_NewString(ctx, s);
    CHECK(!JS_IsException(v), "import map: OOM writing a record string");
    return v;
}

static JSValue im_pair(JSContext *ctx, JSValue a, JSValue b)
{
    JSValue p = JS_NewArray(ctx);

    CHECK(!JS_IsException(p), "import map: OOM writing a record entry");
    im_put(ctx, p, 0, a);
    im_put(ctx, p, 1, b);
    return p;
}

static JSValue im_map_to_js(JSContext *ctx, const ImMap *m)
{
    JSValue arr = JS_NewArray(ctx);

    CHECK(!JS_IsException(arr), "import map: OOM writing a map");
    for (int i = 0; i < m->n; i++)
        im_put(ctx, arr, (uint32_t)i,
                             im_pair(ctx, im_string_or_null(ctx, m->e[i].key), im_string_or_null(ctx, m->e[i].value)));
    return arr;
}

static void im_scopes_from_js(JSContext *ctx, JSValueConst arr, ImScopes *out)
{
    uint32_t n = im_length(ctx, arr);

    memset(out, 0, sizeof *out);
    for (uint32_t i = 0; i < n; i++) {
        JSValue pair = JS_GetPropertyUint32(ctx, arr, i);
        JSValue inner = JS_GetPropertyUint32(ctx, pair, 1);
        char *prefix = im_pair_string(ctx, pair, 0, false);
        ImMap m;

        im_map_from_js(ctx, inner, &m);
        JS_FreeValue(ctx, inner);
        JS_FreeValue(ctx, pair);
        im_scopes_set(out, prefix, &m);
    }
}

static JSValue im_scopes_to_js(JSContext *ctx, const ImScopes *s)
{
    JSValue arr = JS_NewArray(ctx);

    CHECK(!JS_IsException(arr), "import map: OOM writing the scopes map");
    for (int i = 0; i < s->n; i++)
        im_put(ctx, arr, (uint32_t)i,
                             im_pair(ctx, im_string_or_null(ctx, s->s[i].prefix), im_map_to_js(ctx, &s->s[i].map)));
    return arr;
}

static void im_global_map(JSContext *ctx, ImportMap *out)
{
    JSValue l;

    l = im_record_list(ctx, "imports");   im_map_from_js(ctx, l, &out->imports);     JS_FreeValue(ctx, l);
    l = im_record_list(ctx, "scopes");    im_scopes_from_js(ctx, l, &out->scopes);   JS_FreeValue(ctx, l);
    l = im_record_list(ctx, "integrity"); im_map_from_js(ctx, l, &out->integrity);   JS_FreeValue(ctx, l);
}

static void im_set_global_map(JSContext *ctx, const ImportMap *m)
{
    JSValue r = im_record(ctx);

    JS_SetPropertyStr(ctx, r, "imports", im_map_to_js(ctx, &m->imports));
    JS_SetPropertyStr(ctx, r, "scopes", im_scopes_to_js(ctx, &m->scopes));
    JS_SetPropertyStr(ctx, r, "integrity", im_map_to_js(ctx, &m->integrity));
    JS_FreeValue(ctx, r);
}

static ImRecord *im_resolved_from_js(JSContext *ctx, int *pn)
{
    JSValue l = im_record_list(ctx, "resolved");
    uint32_t n = im_length(ctx, l);
    ImRecord *r = n ? calloc(n, sizeof *r) : NULL;

    CHECK(n == 0 || r != NULL, "import map: OOM reading the resolved module set");
    for (uint32_t i = 0; i < n; i++) {
        JSValue rec = JS_GetPropertyUint32(ctx, l, i), b = JS_GetPropertyUint32(ctx, rec, 2);
        r[i].base = im_pair_string(ctx, rec, 0, false);
        r[i].specifier = im_pair_string(ctx, rec, 1, false);
        DCHECK(JS_IsBool(b), "a resolved module set record's third member is not the boolean this file writes");
        r[i].bare_or_special = JS_ToBool(ctx, b);
        JS_FreeValue(ctx, b);
        JS_FreeValue(ctx, rec);
    }
    JS_FreeValue(ctx, l);
    *pn = (int)n;
    return r;
}

static void im_resolved_free(ImRecord *r, int n)
{
    for (int i = 0; i < n; i++) { free(r[i].base); free(r[i].specifier); }
    free(r);
}

/* HTML §8.1.3.3 "Realms, settings objects, and global objects": "A global object has an import map, initially
   an empty import map", and a resolved module set, initially empty. Built at realm creation, so the record is
   in the pre-boot baseline every flow forks from. */
static void import_map_install(JSContext *ctx)
{
    JSValue r = JS_NewObjectProto(ctx, JS_NULL);
    static const char *const lists[] = { "imports", "scopes", "integrity", "resolved" };

    CHECK(!JS_IsException(r), "import map: OOM building a global's record");
    for (size_t i = 0; i < sizeof lists / sizeof lists[0]; i++) {
        JSValue a = JS_NewArray(ctx);
        CHECK(!JS_IsException(a), "import map: OOM building a global's record");
        JS_SetPropertyStr(ctx, r, lists[i], a);
    }
    realm_value_set(ctx, g_slot, r);
}

void import_map_init(JSContext *ctx)
{
    g_slot = realm_value_declare(ctx, "HTML §8.1.5.3's import map and resolved module set of the global");
    g_ready = 1;
    realm_declare_intrinsic(import_map_install);
    agent_state_flag("import_map", &g_ready, "the declaration latch");
    agent_state_realm_slot("import_map", &g_slot, "the per-realm slot a global's import map is held in");
}

void import_map_free(JSRuntime *rt)
{
    (void)rt;
    DCHECK(g_ready, "HTML §8.1.5.3 was released in an agent that never declared it");
    agent_state_undo("import_map");
}

/* "For now, only Window global objects have their import map modified" — a global that is neither a worker's
   nor a worklet's is a Window in this engine. */
static bool im_global_is_window(JSContext *ctx)
{
    return !realm_global_is_worker(ctx) && !realm_global_is_worklet(ctx);
}

/* ---- parse an import map string --------------------------------------------------------------------------- */

/* An Infra ordered map is what "parsing a JSON string to an Infra value" makes of a JSON object, and of nothing
   else: an Array is a list and every other value is a primitive. */
static bool im_is_ordered_map(JSContext *ctx, JSValueConst v)
{
    return JS_IsObject(v) && !JS_IsArray(v);
}

/* The entries of a parsed JSON object, in [[OwnPropertyKeys]] order, which is the order Infra's conversion of a
   JSON-derived value takes them in. */
typedef struct { JSPropertyEnum *tab; uint32_t n; } ImKeys;

static ImKeys im_keys(JSContext *ctx, JSValueConst obj)
{
    ImKeys k = { NULL, 0 };

    CHECK(JS_GetOwnPropertyNames(ctx, &k.tab, &k.n, obj, JS_GPN_STRING_MASK) == 0,
          "import map: OOM enumerating a parsed JSON object");
    return k;
}

static char *im_key_string(JSContext *ctx, JSAtom a)
{
    const char *s = JS_AtomToCString(ctx, a);
    char *out;

    CHECK(s != NULL, "import map: OOM reading a JSON key");
    out = im_strdup(s);
    JS_FreeCString(ctx, s);
    return out;
}

/* A JSON string value as an owned C string, or NULL when the value is not a string. */
static char *im_json_string(JSContext *ctx, JSValueConst v)
{
    const char *s;
    char *out;

    if (!JS_IsString(v)) return NULL;
    s = JS_ToCString(ctx, v);
    CHECK(s != NULL, "import map: OOM reading a JSON string");
    out = im_strdup(s);
    JS_FreeCString(ctx, s);
    return out;
}

/* Normalize a specifier key: null (NULL) for the empty string, the serialization of a URL-like key, otherwise
   the key itself. Owned. */
static char *im_normalize_specifier_key(const char *key, const UrlRecord *base)
{
    char *url;

    if (!*key) return NULL;
    url = im_resolve_url_like(key, base);
    return url ? url : im_strdup(key);
}

/* Sort and normalize a module specifier map. An entry whose address is not a string, does not resolve, or
   lacks the trailing slash its key has is set to null rather than dropped, so resolving that specifier throws
   instead of falling through to a less specific rule. */
static void im_sort_and_normalize_map(JSContext *ctx, JSValueConst original, const UrlRecord *base, ImMap *out)
{
    ImKeys k = im_keys(ctx, original);

    memset(out, 0, sizeof *out);
    for (uint32_t i = 0; i < k.n; i++) {
        char *key = im_key_string(ctx, k.tab[i].atom);
        char *nkey = im_normalize_specifier_key(key, base);
        JSValue v;
        char *value, *address;

        if (!nkey) { free(key); continue; }
        v = JS_GetProperty(ctx, original, k.tab[i].atom);
        value = im_json_string(ctx, v);
        JS_FreeValue(ctx, v);
        if (!value) { free(key); im_map_set(out, nkey, NULL); continue; }
        address = im_resolve_url_like(value, base);
        free(value);
        if (address && im_ends_with_slash(key) && !im_ends_with_slash(address)) { free(address); address = NULL; }
        free(key);
        im_map_set(out, nkey, address);
    }
    JS_FreePropertyEnum(ctx, k.tab, k.n);
    im_map_sort_descending(out);
}

/* Sort and normalize scopes. Returns -1 with a TypeError thrown when a scope's value is not a JSON object. */
static int im_sort_and_normalize_scopes(JSContext *ctx, JSValueConst original, const UrlRecord *base,
                                        ImScopes *out)
{
    ImKeys k = im_keys(ctx, original);
    int rc = 0;

    memset(out, 0, sizeof *out);
    for (uint32_t i = 0; i < k.n && rc == 0; i++) {
        char *prefix = im_key_string(ctx, k.tab[i].atom);
        JSValue potential = JS_GetProperty(ctx, original, k.tab[i].atom);
        UrlRecord u;

        if (!im_is_ordered_map(ctx, potential)) {
            JS_ThrowTypeError(ctx, "the value of the import map scope with prefix \"%s\" needs to be a JSON "
                                   "object", prefix);
            rc = -1;
        } else {
            url_record_init(&u);
            if (url_parse(&u, prefix, strlen(prefix), base)) {
                ImMap m;
                im_sort_and_normalize_map(ctx, potential, base, &m);
                im_scopes_set(out, im_serialize(&u), &m);
            }
            url_record_free(&u);
        }
        free(prefix);
        JS_FreeValue(ctx, potential);
    }
    JS_FreePropertyEnum(ctx, k.tab, k.n);
    if (rc) { im_scopes_free(out); return rc; }
    im_scopes_sort_descending(out);
    return 0;
}

/* Normalize a module integrity map: a key that does not resolve as a URL-like specifier, or a value that is not
   a string, is skipped. */
static void im_normalize_integrity(JSContext *ctx, JSValueConst original, const UrlRecord *base, ImMap *out)
{
    ImKeys k = im_keys(ctx, original);

    memset(out, 0, sizeof *out);
    for (uint32_t i = 0; i < k.n; i++) {
        char *key = im_key_string(ctx, k.tab[i].atom);
        char *url = im_resolve_url_like(key, base);
        JSValue v;
        char *value;

        free(key);
        if (!url) continue;
        v = JS_GetProperty(ctx, original, k.tab[i].atom);
        value = im_json_string(ctx, v);
        JS_FreeValue(ctx, v);
        if (!value) { free(url); continue; }
        im_map_set(out, url, value);
    }
    JS_FreePropertyEnum(ctx, k.tab, k.n);
}

/* parsed["key"] when it exists, else JS_UNDEFINED. Owned. */
static JSValue im_member(JSContext *ctx, JSValueConst parsed, const char *key, bool *exists)
{
    JSAtom a = JS_NewAtom(ctx, key);
    JSValue v = JS_UNDEFINED;
    int has;

    CHECK(a != JS_ATOM_NULL, "import map: OOM interning a top-level key");
    /* A JSON-derived object holds data properties only, so the slot read is Infra's map lookup. */
    has = JS_GetOwnSlot(ctx, &v, parsed, a);
    JS_FreeAtom(ctx, a);
    CHECK(has >= 0, "import map: reading a parsed JSON object's own property threw");
    *exists = has > 0;
    return v;
}

/* Parse an import map string. Returns 0 with `out` filled, or -1 with the exception thrown. */
static int im_parse(JSContext *ctx, const char *input, size_t len, const UrlRecord *base, ImportMap *out)
{
    static const char *const names[] = { "imports", "scopes", "integrity" };
    JSValue parsed, v;
    bool exists;
    int rc = 0;

    memset(out, 0, sizeof *out);
    parsed = JS_ParseJSON(ctx, input, len, "<import map>");
    if (JS_IsException(parsed)) return -1;
    if (!im_is_ordered_map(ctx, parsed)) {
        JS_FreeValue(ctx, parsed);
        JS_ThrowTypeError(ctx, "the top-level value of an import map needs to be a JSON object");
        return -1;
    }
    for (size_t i = 0; i < sizeof names / sizeof names[0] && rc == 0; i++) {
        v = im_member(ctx, parsed, names[i], &exists);
        if (!exists) continue;
        if (!im_is_ordered_map(ctx, v)) {
            JS_ThrowTypeError(ctx, "the value for the \"%s\" top-level key of an import map needs to be a JSON "
                                   "object", names[i]);
            rc = -1;
        } else if (i == 0) {
            im_sort_and_normalize_map(ctx, v, base, &out->imports);
        } else if (i == 1) {
            rc = im_sort_and_normalize_scopes(ctx, v, base, &out->scopes);
        } else {
            im_normalize_integrity(ctx, v, base, &out->integrity);
        }
        JS_FreeValue(ctx, v);
    }
    JS_FreeValue(ctx, parsed);
    if (rc) im_free(out);
    return rc;
}

/* ---- merge existing and new import maps ------------------------------------------------------------------- */

/* Merge module specifier maps: `old`'s rules, then each of `new_map`'s whose specifier `old` lacks. Fills `out`. */
static void im_merge_maps(const ImMap *new_map, const ImMap *old, ImMap *out)
{
    im_map_copy(out, old);
    for (int i = 0; i < new_map->n; i++) {
        if (im_map_find(old, new_map->e[i].key) >= 0) continue;
        im_map_set(out, im_strdup(new_map->e[i].key), new_map->e[i].value ? im_strdup(new_map->e[i].value) : NULL);
    }
}

/* Merge existing and new import maps, given this realm's global and `nm`, which it consumes. */
static void im_merge_into_global(JSContext *ctx, ImportMap *nm)
{
    ImportMap old;
    ImRecord *rs;
    int nr;

    im_global_map(ctx, &old);
    rs = im_resolved_from_js(ctx, &nr);
    for (int si = 0; si < nm->scopes.n; si++) {
        const char *scope_prefix = nm->scopes.s[si].prefix;
        ImMap *scope_imports = &nm->scopes.s[si].map;
        int at;

        for (int ri = 0; ri < nr; ri++) {
            if (strcmp(scope_prefix, rs[ri].base) &&
                !(im_ends_with_slash(scope_prefix) && im_prefix_of(scope_prefix, rs[ri].base)))
                continue;
            for (int k = 0; k < scope_imports->n;) {
                const char *key = scope_imports->e[k].key;
                if (!strcmp(key, rs[ri].specifier) ||
                    (im_ends_with_slash(key) && im_prefix_of(key, rs[ri].specifier) && rs[ri].bare_or_special))
                    im_map_remove(scope_imports, k);
                else
                    k++;
            }
        }
        at = im_scopes_find(&old.scopes, scope_prefix);
        if (at >= 0) {
            ImMap merged;
            im_merge_maps(scope_imports, &old.scopes.s[at].map, &merged);
            im_scopes_set(&old.scopes, im_strdup(scope_prefix), &merged);
        } else {
            ImMap copy;
            im_map_copy(&copy, scope_imports);
            im_scopes_set(&old.scopes, im_strdup(scope_prefix), &copy);
        }
    }
    for (int i = 0; i < nm->integrity.n; i++) {
        if (im_map_find(&old.integrity, nm->integrity.e[i].key) >= 0) continue;
        im_map_set(&old.integrity, im_strdup(nm->integrity.e[i].key), im_strdup(nm->integrity.e[i].value));
    }
    for (int ri = 0; ri < nr; ri++)
        for (int k = 0; k < nm->imports.n;) {
            if (im_prefix_of(rs[ri].specifier, nm->imports.e[k].key)) im_map_remove(&nm->imports, k);
            else k++;
        }
    {
        ImMap merged;
        im_merge_maps(&nm->imports, &old.imports, &merged);
        im_map_free(&old.imports);
        old.imports = merged;
    }
    im_set_global_map(ctx, &old);
    im_resolved_free(rs, nr);
    im_free(&old);
    im_free(nm);
}

int import_map_create_and_register(JSContext *ctx, const char *input, size_t input_len, const char *base_url)
{
    UrlRecord base;
    ImportMap nm;
    int rc;

    DCHECK(input != NULL && base_url != NULL, "an import map was registered with no text or no base URL");
    DCHECK(im_global_is_window(ctx),
           "an import map was registered on a global that is not a Window — only a Document has `script` "
           "elements, and register an import map is given a Window global");
    url_record_init(&base);
    CHECK(url_parse(&base, base_url, strlen(base_url), NULL),
          "an import map's base URL does not parse — it is the element's node document's document base URL, a "
          "serialized URL this engine computed");
    rc = im_parse(ctx, input, input_len, &base, &nm);
    url_record_free(&base);
    if (rc) return -1;
    im_merge_into_global(ctx, &nm);
    return 0;
}

/* ---- resolution ------------------------------------------------------------------------------------------ */

/* HTML §8.1.5.1's resolve an imports match. Returns 1 with `out` filled, 0 for null, -1 with a TypeError. */
static int im_resolve_imports_match(JSContext *ctx, const char *normalized, const UrlRecord *as_url,
                                    const ImMap *map, UrlRecord *out)
{
    bool bare_or_special = as_url == NULL || url_scheme_is_special(as_url->scheme);

    for (int i = 0; i < map->n; i++) {
        const char *key = map->e[i].key, *result = map->e[i].value;

        if (!strcmp(key, normalized)) {
            if (!result) {
                JS_ThrowTypeError(ctx, "resolution of \"%s\" was blocked by a null entry in the import map", key);
                return -1;
            }
            CHECK(url_parse(out, result, strlen(result), NULL),
                  "an import map entry's address is not a URL — the parse stored a serialization");
            return 1;
        }
        if (im_ends_with_slash(key) && im_prefix_of(key, normalized) && bare_or_special) {
            UrlRecord rr;
            char *ser;
            bool ok;

            if (!result) {
                JS_ThrowTypeError(ctx, "resolution of \"%s\" was blocked by a null entry in the import map", key);
                return -1;
            }
            DCHECK(im_ends_with_slash(result),
                   "an import map entry for a key ending in \"/\" holds an address that does not — the parse sets "
                   "such an entry to null");
            url_record_init(&rr);
            CHECK(url_parse(&rr, result, strlen(result), NULL),
                  "an import map entry's address is not a URL — the parse stored a serialization");
            ok = url_parse(out, normalized + strlen(key), strlen(normalized + strlen(key)), &rr);
            url_record_free(&rr);
            if (!ok) {
                JS_ThrowTypeError(ctx, "resolution of \"%s\" was blocked since the part after the prefix \"%s\" "
                                       "could not be URL-parsed relative to \"%s\"", normalized, key, result);
                return -1;
            }
            ser = im_serialize(out);
            ok = im_prefix_of(result, ser);
            free(ser);
            if (!ok) {
                JS_ThrowTypeError(ctx, "resolution of \"%s\" was blocked due to it backtracking above its "
                                       "prefix \"%s\"", normalized, key);
                return -1;
            }
            return 1;
        }
    }
    return 0;
}

int import_map_resolve(JSContext *ctx, const char *serialized_base_url, const char *normalized_specifier,
                       const UrlRecord *as_url, UrlRecord *out)
{
    ImportMap map;
    int r = 0;

    DCHECK(serialized_base_url != NULL && normalized_specifier != NULL,
           "an import map was asked to resolve with no base URL or no specifier");
    /* Steps 4-5: an empty import map unless the global is a Window. */
    if (!im_global_is_window(ctx)) return 0;
    im_global_map(ctx, &map);
    /* Step 10: the scopes, most specific first. */
    for (int i = 0; i < map.scopes.n && r == 0; i++) {
        const char *prefix = map.scopes.s[i].prefix;
        if (!strcmp(prefix, serialized_base_url) ||
            (im_ends_with_slash(prefix) && im_prefix_of(prefix, serialized_base_url))) {
            r = im_resolve_imports_match(ctx, normalized_specifier, as_url, &map.scopes.s[i].map, out);
            /* "If scopeImportsMatch is not null, then set result to scopeImportsMatch, and break." */
            if (r != 0) break;
        }
    }
    /* Step 11. */
    if (r == 0) r = im_resolve_imports_match(ctx, normalized_specifier, as_url, &map.imports, out);
    im_free(&map);
    return r;
}

void import_map_add_resolved(JSContext *ctx, const char *serialized_base_url, const char *normalized_specifier,
                             const UrlRecord *as_url)
{
    bool bare_or_special = as_url == NULL || url_scheme_is_special(as_url->scheme);
    JSValue l, rec;
    ImRecord *rs;
    int nr;

    if (!im_global_is_window(ctx)) return;
    /* A set: an equal record is already a member. */
    rs = im_resolved_from_js(ctx, &nr);
    for (int i = 0; i < nr; i++)
        if (!strcmp(rs[i].base, serialized_base_url) && !strcmp(rs[i].specifier, normalized_specifier) &&
            rs[i].bare_or_special == bare_or_special) {
            im_resolved_free(rs, nr);
            return;
        }
    im_resolved_free(rs, nr);
    l = im_record_list(ctx, "resolved");
    rec = JS_NewArray(ctx);
    CHECK(!JS_IsException(rec), "import map: OOM recording a resolved specifier");
    im_put(ctx, rec, 0, im_string_or_null(ctx, serialized_base_url));
    im_put(ctx, rec, 1, im_string_or_null(ctx, normalized_specifier));
    im_put(ctx, rec, 2, JS_NewBool(ctx, bare_or_special));
    im_put(ctx, l, im_length(ctx, l), rec);
    JS_FreeValue(ctx, l);
}

char *import_map_module_integrity(JSContext *ctx, const char *url)
{
    JSValue l = im_record_list(ctx, "integrity");
    ImMap m;
    int at;
    char *out;

    im_map_from_js(ctx, l, &m);
    JS_FreeValue(ctx, l);
    at = im_map_find(&m, url);
    out = im_strdup(at >= 0 ? m.e[at].value : "");
    im_map_free(&m);
    return out;
}
