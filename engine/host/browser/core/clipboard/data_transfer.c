/* HTML §6.11.3 The DataTransfer interface, §6.11.3.1 The DataTransferItemList interface and §6.11.3.2 The
 * DataTransferItem interface.
 *
 * A DataTransfer is the page's view of §6.11.2's drag data store while the two are associated. The store is a
 * record (its mode, its item list, the drag image) hung off the DataTransfer's slot record, and each item is a
 * record {kind, type, data}; every mutation is a property write, so a store a flow changes is that flow's and
 * parks with it. The item list is replaced, never edited in place, so "the item list still contains the item"
 * (§6.11.3.2's disabled mode) is an identity scan over the list as it stands.
 *
 * The three interfaces are one component because their modes are one fact: the list's and each item's mode is
 * the store's while associated and disabled otherwise.
 *
 * A string argument a page supplies may be unknown input, which crosses Web IDL's DOMString conversion as
 * itself. Data (setData's and add's) is stored as the value it is. A format or type compared against the item
 * list is a refused capability here (dt_format), since that comparison forks once per item type string. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/clipboard/data_transfer.h"
#include "core/dom/element.h"
#include "core/file/blob.h"
#include "core/file/file_list.h"
#include "core/idl_args.h"
#include "core/idl_indexed.h"
#include "core/idl_slots.h"
#include "core/realm.h"
#include "core/timing/task_source.h"
#include "solver/concolic.h"
#include "solver/reclaim.h"
#include <lexbor/core/base.h>   /* lexbor_malloc: the allocator the engine gives its refusal edge */

#define DT_COMPONENT "data_transfer"

static JSClassID g_dt_class, g_list_class, g_item_class;   /* per-realm prototype slots */
static JSValue   g_dt_key = JS_UNDEFINED, g_list_key = JS_UNDEFINED, g_item_key = JS_UNDEFINED;
static JSAtom    g_dt_atom = JS_ATOM_NULL, g_list_atom = JS_ATOM_NULL, g_item_atom = JS_ATOM_NULL;
static int       g_ready;
static int g_ctor_id = -1, g_drop_effect_set = -1, g_effect_allowed_set = -1, g_set_drag_image_id = -1,
           g_get_data_id = -1, g_set_data_id = -1, g_clear_data_id = -1, g_add_id = -1, g_remove_id = -1,
           g_clear_id = -1, g_get_as_string_id = -1, g_get_as_file_id = -1;

/* ---- records ---------------------------------------------------------------------------------------------- */

/* The own slot record under `atom`, owned, or JS_UNDEFINED where `v` carries none. */
static JSValue dt_slot(JSContext *ctx, JSValueConst v, JSAtom atom)
{
    JSValue r;

    if (!JS_IsObject(v) || atom == JS_ATOM_NULL) return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &r, v, atom) <= 0) return JS_UNDEFINED;
    return r;
}

/* A field of a record this component built: a null-prototype object, so the read runs no page code. Owned. */
static JSValue rec_get(JSContext *ctx, JSValueConst rec, const char *name)
{
    return JS_GetPropertyStr(ctx, rec, name);
}

static void rec_set(JSContext *ctx, JSValueConst rec, const char *name, JSValue v)
{
    CHECK(!JS_IsException(v), "DataTransfer: OOM building a drag data store field");
    JS_SetPropertyStr(ctx, rec, name, v);
}

static JSValue rec_new(JSContext *ctx)
{
    JSValue r = idl_slots_new(ctx);

    CHECK(!JS_IsException(r), "DataTransfer: OOM building a record");
    return r;
}

/* The drag data store `dt` is associated with, owned, or JS_UNDEFINED once the association is broken. */
static JSValue dt_store(JSContext *ctx, JSValueConst slots)
{
    return rec_get(ctx, slots, "store");
}

static int dt_mode(JSContext *ctx, JSValueConst store)
{
    JSValue m = rec_get(ctx, store, "mode");
    int32_t mode = -1;

    DCHECK(JS_VALUE_GET_TAG(m) == JS_TAG_INT, "a drag data store carries no mode — the store is built with one");
    JS_ToInt32(ctx, &mode, m);
    return mode;
}

static uint32_t arr_len(JSContext *ctx, JSValueConst arr)
{
    JSValue len = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    return n;
}

static bool item_kind_is(JSContext *ctx, JSValueConst item, const char *kind)
{
    JSValue k = rec_get(ctx, item, "kind");
    const char *s = JS_ToCString(ctx, k);
    bool is = s && !strcmp(s, kind);

    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, k);
    return is;
}

static bool item_type_is(JSContext *ctx, JSValueConst item, const char *type)
{
    JSValue t = rec_get(ctx, item, "type");
    const char *s = JS_ToCString(ctx, t);
    bool is = s && !strcmp(s, type);

    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, t);
    return is;
}

/* §6.11.3's types array, recomputed "when the contents of the DataTransfer object's drag data store item list
   change": each text item's type string, then "Files" if any item is a File. */
static void dt_update_types(JSContext *ctx, JSValueConst slots, JSValueConst items)
{
    JSValue list = JS_NewArray(ctx);
    uint32_t n = JS_IsUndefined(items) ? 0 : arr_len(ctx, items), i, at = 0;
    bool files = false;

    CHECK(!JS_IsException(list), "DataTransfer: OOM building the types array");
    for (i = 0; i < n; i++) {
        JSValue item = JS_GetPropertyUint32(ctx, items, i);

        if (item_kind_is(ctx, item, "text"))
            JS_DefinePropertyValueUint32(ctx, list, at++, rec_get(ctx, item, "type"), JS_PROP_C_W_E);
        else files = true;
        JS_FreeValue(ctx, item);
    }
    if (files) JS_DefinePropertyValueUint32(ctx, list, at++, JS_NewString(ctx, "Files"), JS_PROP_C_W_E);
    CHECK(idl_freeze_array(ctx, list) >= 0, "DataTransfer: the types array could not be frozen");
    rec_set(ctx, slots, "types", list);
}

/* Replace the store's item list with `items` (consumed) and recompute the types array. */
static void dt_set_items(JSContext *ctx, JSValueConst slots, JSValueConst store, JSValue items)
{
    dt_update_types(ctx, slots, items);
    rec_set(ctx, store, "items", items);
}

static JSValue item_new(JSContext *ctx, const char *kind, JSValue type, JSValue data)
{
    JSValue item = rec_new(ctx);

    rec_set(ctx, item, "kind", JS_NewString(ctx, kind));
    rec_set(ctx, item, "type", type);
    rec_set(ctx, item, "data", data);
    return item;
}

/* The item list with `item` (consumed) appended. Owned. */
static JSValue items_append(JSContext *ctx, JSValueConst items, JSValue item)
{
    JSValue out = JS_NewArray(ctx);
    uint32_t n = arr_len(ctx, items), i;

    CHECK(!JS_IsException(out), "DataTransfer: OOM growing the item list");
    for (i = 0; i < n; i++)
        JS_DefinePropertyValueUint32(ctx, out, i, JS_GetPropertyUint32(ctx, items, i), JS_PROP_C_W_E);
    JS_DefinePropertyValueUint32(ctx, out, n, item, JS_PROP_C_W_E);
    return out;
}

/* The item list without the items `drop` answers true for. Owned. */
static JSValue items_without(JSContext *ctx, JSValueConst items, bool (*drop)(JSContext *, JSValueConst,
                                                                              const void *), const void *arg)
{
    JSValue out = JS_NewArray(ctx);
    uint32_t n = arr_len(ctx, items), i, at = 0;

    CHECK(!JS_IsException(out), "DataTransfer: OOM shrinking the item list");
    for (i = 0; i < n; i++) {
        JSValue item = JS_GetPropertyUint32(ctx, items, i);

        if (drop(ctx, item, arg)) JS_FreeValue(ctx, item);
        else JS_DefinePropertyValueUint32(ctx, out, at++, item, JS_PROP_C_W_E);
    }
    return out;
}

static bool drop_text_of_type(JSContext *ctx, JSValueConst item, const void *type)
{
    return item_kind_is(ctx, item, "text") && item_type_is(ctx, item, type);
}

static bool drop_text(JSContext *ctx, JSValueConst item, const void *unused)
{
    (void)unused;
    return item_kind_is(ctx, item, "text");
}

static bool drop_all(JSContext *ctx, JSValueConst item, const void *unused)
{
    (void)ctx; (void)item; (void)unused;
    return true;
}

static bool drop_at(JSContext *ctx, JSValueConst item, const void *target)
{
    (void)ctx;
    return JS_VALUE_GET_PTR(item) == target;
}

/* ---- strings ---------------------------------------------------------------------------------------------- */

static char *ascii_lower_dup(const char *s, size_t n)
{
    char *out = lexbor_malloc(n + 1);

    CHECK(out, "DataTransfer: OOM lowercasing a format");
    for (size_t i = 0; i < n; i++) out[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] - 'A' + 'a') : s[i];
    out[n] = 0;
    return out;
}

/* A format or type string argument "converted to ASCII lowercase", with getData's, setData's and clearData's
   two renamings ("text" is "text/plain", "url" is "text/uri-list") when `rename`. malloc'd. */
static char *dt_format(JSContext *ctx, JSValueConst v, bool rename, bool *is_url)
{
    const char *s;
    size_t n = 0;
    char *f;

    if (is_url) *is_url = false;
    DCHECK(!concolic_is(v),
           "an unknown format reached the drag data store — it is compared against every item's type string, so "
           "the next diff builds that comparison as a fork with one arm per type string in the item list and one "
           "arm matching none");
    s = JS_ToCStringLen(ctx, &n, v);
    CHECK(s != NULL, "DataTransfer: a converted format could not be read");
    f = ascii_lower_dup(s, n);
    JS_FreeCString(ctx, s);
    if (rename && !strcmp(f, "text")) { free(f); f = reclaim_strdup("text/plain"); }
    else if (rename && !strcmp(f, "url")) {
        free(f);
        f = reclaim_strdup("text/uri-list");
        if (is_url) *is_url = true;
    }
    CHECK(f, "DataTransfer: OOM renaming a format");
    return f;
}

/* ---- DataTransfer ----------------------------------------------------------------------------------------- */

bool data_transfer_is(JSContext *ctx, JSValueConst v)
{
    JSValue s = dt_slot(ctx, v, g_dt_atom);
    bool is = !JS_IsUndefined(s);

    JS_FreeValue(ctx, s);
    return is;
}

static JSValue item_list_new(JSContext *ctx, JSValueConst dt);

/* A DataTransfer over `store` (consumed; JS_UNDEFINED for none). */
static JSValue dt_new(JSContext *ctx, JSValue store, const char *drop_effect, const char *effect_allowed)
{
    JSValue proto = JS_GetClassProto(ctx, g_dt_class), dt, slots;

    DCHECK(g_ready && !JS_IsNull(proto), "a DataTransfer was built in a realm that never installed the interface");
    dt = JS_NewObjectProtoClass(ctx, proto, g_dt_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(dt), "DataTransfer: OOM building the object");
    slots = rec_new(ctx);
    rec_set(ctx, slots, "dropEffect", JS_NewString(ctx, drop_effect));
    rec_set(ctx, slots, "effectAllowed", JS_NewString(ctx, effect_allowed));
    JS_DefinePropertyValue(ctx, dt, g_dt_atom, JS_DupValue(ctx, slots), 0);
    if (JS_IsUndefined(store)) dt_update_types(ctx, slots, JS_UNDEFINED);
    else {
        JSValue items = rec_get(ctx, store, "items");

        dt_update_types(ctx, slots, items);
        JS_FreeValue(ctx, items);
    }
    rec_set(ctx, slots, "store", store);
    /* [SameObject] items and files, made with the object. */
    rec_set(ctx, slots, "items", item_list_new(ctx, dt));
    rec_set(ctx, slots, "files", file_list_new_empty(ctx));
    JS_FreeValue(ctx, slots);
    return dt;
}

static JSValue store_new(JSContext *ctx, DragDataStoreMode mode, JSValue items)
{
    JSValue store = rec_new(ctx);

    rec_set(ctx, store, "mode", JS_NewInt32(ctx, mode));
    rec_set(ctx, store, "items", items);
    return store;
}

JSValue data_transfer_new_for_drag(JSContext *ctx, JSValueConst files, DragDataStoreMode mode,
                                   const char *drop_effect, const char *effect_allowed)
{
    JSValue items = JS_NewArray(ctx);
    uint32_t n = arr_len(ctx, files), i;

    CHECK(!JS_IsException(items), "DataTransfer: OOM building a drag data store");
    for (i = 0; i < n; i++) {
        JSValue f = JS_GetPropertyUint32(ctx, files, i);
        const char *type = NULL;
        char *lower;

        DCHECK(blob_file_name_of(f) != NULL, "a drag data store File item was built over a value that is not a "
                                             "File");
        blob_bytes_of(f, NULL, &type);
        lower = ascii_lower_dup(type ? type : "", type ? strlen(type) : 0);
        JS_DefinePropertyValueUint32(ctx, items, i, item_new(ctx, "file", JS_NewString(ctx, lower), f),
                                     JS_PROP_C_W_E);
        free(lower);
    }
    return dt_new(ctx, store_new(ctx, mode, items), drop_effect, effect_allowed);
}

/* `new DataTransfer()`: "Set the drag data store's item list to be an empty list. Set the drag data store's
   mode to read/write mode. Set the dropEffect and effectAllowed to \"none\"." */
static JSValue js_dt_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue items;

    (void)argc; (void)argv; (void)magic;
    if (JS_IsUndefined(this_val)) return JS_ThrowTypeError(ctx, "constructor DataTransfer requires 'new'");
    items = JS_NewArray(ctx);
    CHECK(!JS_IsException(items), "DataTransfer: OOM building a drag data store");
    return dt_new(ctx, store_new(ctx, DND_MODE_READ_WRITE, items), "none", "none");
}

/* The receiver's slot record, owned, or JS_UNDEFINED with Web IDL §3.7's TypeError thrown. */
static JSValue dt_this(JSContext *ctx, JSValueConst this_val)
{
    JSValue s = dt_slot(ctx, this_val, g_dt_atom);

    if (JS_IsUndefined(s)) JS_ThrowTypeError(ctx, "not a DataTransfer");
    return s;
}

enum { DT_DROP_EFFECT, DT_EFFECT_ALLOWED, DT_ITEMS, DT_TYPES, DT_FILES };
static const char *const DT_FIELD[] = { "dropEffect", "effectAllowed", "items", "types", NULL };

static JSValue js_dt_files(JSContext *ctx, JSValueConst slots);

static JSValue js_dt_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue slots = dt_this(ctx, this_val), v;

    if (JS_IsUndefined(slots)) return JS_EXCEPTION;
    v = magic == DT_FILES ? js_dt_files(ctx, slots) : rec_get(ctx, slots, DT_FIELD[magic]);
    JS_FreeValue(ctx, slots);
    return v;
}

static bool str_in(const char *s, const char *const *set)
{
    for (; *set; set++) if (!strcmp(s, *set)) return true;
    return false;
}

static const char *const DROP_EFFECTS[] = { "none", "copy", "link", "move", NULL };
static const char *const EFFECTS_ALLOWED[] = { "none", "copy", "copyLink", "copyMove", "link", "linkMove", "move",
                                               "all", "uninitialized", NULL };

/* dropEffect's setter: "if the new value is one of \"none\", \"copy\", \"link\", or \"move\", then the attribute's
   current value must be set to the new value. Other values must be ignored." effectAllowed's adds "if the drag
   data store's mode is the read/write mode". An unknown value is one string the page wrote and the attribute
   holds as one of these words or keeps its value; the setter asks nothing of it here, so it is ignored. */
static JSValue js_dt_set_effect(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    JSValue slots = dt_this(ctx, this_val);
    const char *s;

    if (JS_IsUndefined(slots)) return JS_EXCEPTION;
    DCHECK(!concolic_is(val),
           "an unknown value was assigned to dropEffect or effectAllowed — the setter's answer is one arm per "
           "listed word and one arm leaving the value unchanged, which the next diff builds as an idl_enum_fork");
    s = JS_ToCString(ctx, val);
    CHECK(s != NULL, "DataTransfer: a converted effect could not be read");
    if (magic == DT_DROP_EFFECT && str_in(s, DROP_EFFECTS)) rec_set(ctx, slots, "dropEffect", JS_NewString(ctx, s));
    if (magic == DT_EFFECT_ALLOWED && str_in(s, EFFECTS_ALLOWED)) {
        JSValue store = dt_store(ctx, slots);

        if (!JS_IsUndefined(store) && dt_mode(ctx, store) == DND_MODE_READ_WRITE)
            rec_set(ctx, slots, "effectAllowed", JS_NewString(ctx, s));
        JS_FreeValue(ctx, store);
    }
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, slots);
    return JS_UNDEFINED;
}

/* The store if it is associated and in a mode `ok` admits; JS_UNDEFINED otherwise. Owned. */
static JSValue dt_store_in(JSContext *ctx, JSValueConst slots, bool rw_only)
{
    JSValue store = dt_store(ctx, slots);

    if (JS_IsUndefined(store)) return store;
    if (rw_only ? dt_mode(ctx, store) != DND_MODE_READ_WRITE : dt_mode(ctx, store) == DND_MODE_PROTECTED) {
        JS_FreeValue(ctx, store);
        return JS_UNDEFINED;
    }
    return store;
}

/* setDragImage(image, x, y): no association or not read/write returns; otherwise the store's bitmap is the
   element's image and its hot spot is (x, y). The element is held as the bitmap's source, since this engine
   paints no drag feedback. */
static JSValue js_dt_set_drag_image(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int m)
{
    JSValue slots = dt_this(ctx, this_val), store;

    (void)argc; (void)m;
    if (JS_IsUndefined(slots)) return JS_EXCEPTION;
    store = dt_store_in(ctx, slots, true);
    if (!JS_IsUndefined(store)) {
        rec_set(ctx, store, "bitmap", JS_DupValue(ctx, argv[0]));
        rec_set(ctx, store, "hotSpotX", JS_DupValue(ctx, argv[1]));
        rec_set(ctx, store, "hotSpotY", JS_DupValue(ctx, argv[2]));
    }
    JS_FreeValue(ctx, store);
    JS_FreeValue(ctx, slots);
    return JS_UNDEFINED;
}

/* RFC 2483 §5's text/uri-list: lines separated by CRLF, a line starting with "#" a comment. The first URL is
   the first line that is not a comment, trimmed of its line break. */
static JSValue first_uri(JSContext *ctx, JSValueConst data)
{
    const char *s = JS_ToCString(ctx, data), *p;
    JSValue r = JS_NewString(ctx, "");

    CHECK(s != NULL, "DataTransfer: a text/uri-list item could not be read");
    for (p = s; *p;) {
        const char *e = p + strcspn(p, "\r\n");

        if (*p != '#' && e > p) { JS_FreeValue(ctx, r); r = JS_NewStringLen(ctx, p, (size_t)(e - p)); break; }
        p = e + strspn(e, "\r\n");
    }
    JS_FreeCString(ctx, s);
    return r;
}

/* getData(format). */
static JSValue js_dt_get_data(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int m)
{
    JSValue slots = dt_this(ctx, this_val), store, items, r = JS_UNDEFINED;
    bool is_url;
    char *format;
    uint32_t n, i;

    (void)argc; (void)m;
    if (JS_IsUndefined(slots)) return JS_EXCEPTION;
    store = dt_store_in(ctx, slots, false);   /* steps 1-2 */
    JS_FreeValue(ctx, slots);
    if (JS_IsUndefined(store)) return JS_NewString(ctx, "");
    format = dt_format(ctx, argv[0], true, &is_url);   /* steps 3-6 */
    items = rec_get(ctx, store, "items");
    n = arr_len(ctx, items);
    for (i = 0; i < n && JS_IsUndefined(r); i++) {
        JSValue item = JS_GetPropertyUint32(ctx, items, i);

        if (drop_text_of_type(ctx, item, format)) r = rec_get(ctx, item, "data");   /* step 8 */
        JS_FreeValue(ctx, item);
    }
    free(format);
    JS_FreeValue(ctx, items);
    JS_FreeValue(ctx, store);
    if (JS_IsUndefined(r)) return JS_NewString(ctx, "");   /* step 7 */
    if (is_url) {   /* step 9 */
        JSValue u;

        DCHECK(!concolic_is(r), "an unknown text/uri-list item reached getData(\"url\") — the next diff runs RFC "
                                "2483 §5's first-URL rule over it as a derivation of the item's value");
        u = first_uri(ctx, r);
        JS_FreeValue(ctx, r);
        return u;
    }
    return r;
}

/* setData(format, data). */
static JSValue js_dt_set_data(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int m)
{
    JSValue slots = dt_this(ctx, this_val), store, items, kept;
    char *format;

    (void)argc; (void)m;
    if (JS_IsUndefined(slots)) return JS_EXCEPTION;
    store = dt_store_in(ctx, slots, true);   /* steps 1-2 */
    if (!JS_IsUndefined(store)) {
        format = dt_format(ctx, argv[0], true, NULL);   /* steps 3-5 */
        items = rec_get(ctx, store, "items");
        kept = items_without(ctx, items, drop_text_of_type, format);   /* step 6 */
        JS_FreeValue(ctx, items);
        dt_set_items(ctx, slots, store,   /* step 7 */
                     items_append(ctx, kept, item_new(ctx, "text", JS_NewString(ctx, format),
                                                      JS_DupValue(ctx, argv[1]))));
        JS_FreeValue(ctx, kept);
        free(format);
    }
    JS_FreeValue(ctx, store);
    JS_FreeValue(ctx, slots);
    return JS_UNDEFINED;
}

/* clearData(optional format). */
static JSValue js_dt_clear_data(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int m)
{
    JSValue slots = dt_this(ctx, this_val), store, items;

    (void)m;
    if (JS_IsUndefined(slots)) return JS_EXCEPTION;
    store = dt_store_in(ctx, slots, true);   /* steps 1-2 */
    if (!JS_IsUndefined(store)) {
        items = rec_get(ctx, store, "items");
        if (!idl_arg_given(argc, argv, 0)) dt_set_items(ctx, slots, store, items_without(ctx, items, drop_text, NULL));
        else {
            char *format = dt_format(ctx, argv[0], true, NULL);   /* steps 4-5 */

            dt_set_items(ctx, slots, store, items_without(ctx, items, drop_text_of_type, format));   /* step 6 */
            free(format);
        }
        JS_FreeValue(ctx, items);
    }
    JS_FreeValue(ctx, store);
    JS_FreeValue(ctx, slots);
    return JS_UNDEFINED;
}

/* files: "a live FileList" of the store's File items, the same File object for a given underlying file, and
   empty with no association or in the protected mode. The [SameObject] list is refilled at each read. */
static JSValue js_dt_files(JSContext *ctx, JSValueConst slots)
{
    JSValue list = rec_get(ctx, slots, "files"), store = dt_store_in(ctx, slots, false), files = JS_NewArray(ctx);

    CHECK(!JS_IsException(files), "DataTransfer: OOM building the file list");
    if (!JS_IsUndefined(store)) {
        JSValue items = rec_get(ctx, store, "items");
        uint32_t n = arr_len(ctx, items), i, at = 0;

        for (i = 0; i < n; i++) {
            JSValue item = JS_GetPropertyUint32(ctx, items, i);

            if (item_kind_is(ctx, item, "file"))
                JS_DefinePropertyValueUint32(ctx, files, at++, rec_get(ctx, item, "data"), JS_PROP_C_W_E);
            JS_FreeValue(ctx, item);
        }
        JS_FreeValue(ctx, items);
    }
    JS_FreeValue(ctx, store);
    file_list_set_files(ctx, list, files);
    return list;
}

/* ---- DataTransferItemList ---------------------------------------------------------------------------------- */

/* The owning DataTransfer's slot record, owned, or JS_UNDEFINED for something that is not a list. */
static JSValue list_owner_slots(JSContext *ctx, JSValueConst list)
{
    JSValue s = dt_slot(ctx, list, g_list_atom), dt, slots;

    if (JS_IsUndefined(s)) return s;
    dt = rec_get(ctx, s, "owner");
    slots = dt_slot(ctx, dt, g_dt_atom);
    DCHECK(!JS_IsUndefined(slots), "a DataTransferItemList's owner is not a DataTransfer");
    JS_FreeValue(ctx, dt);
    JS_FreeValue(ctx, s);
    return slots;
}

/* The list's items when it is not in the disabled mode, owned, or JS_UNDEFINED. */
static JSValue list_items(JSContext *ctx, JSValueConst list, int *mode)
{
    JSValue slots = list_owner_slots(ctx, list), store, items = JS_UNDEFINED;

    *mode = -1;
    if (JS_IsUndefined(slots)) return items;
    store = dt_store(ctx, slots);
    if (!JS_IsUndefined(store)) {
        *mode = dt_mode(ctx, store);
        items = rec_get(ctx, store, "items");
    }
    JS_FreeValue(ctx, store);
    JS_FreeValue(ctx, slots);
    return items;
}

static uint32_t list_indexed_length(JSContext *ctx, JSValueConst self)
{
    int mode;
    JSValue items = list_items(ctx, self, &mode);
    uint32_t n = JS_IsUndefined(items) ? 0 : arr_len(ctx, items);

    JS_FreeValue(ctx, items);
    return n;
}

static JSValue item_object_of(JSContext *ctx, JSValueConst owner, JSValueConst item);

/* §6.11.3.1's indexed property: the DataTransferItem for the indexth item, the same object each time. */
static JSValue list_indexed_item(JSContext *ctx, JSValueConst self, uint32_t i)
{
    int mode;
    JSValue items = list_items(ctx, self, &mode), item, s, owner, r;

    if (JS_IsUndefined(items) || i >= arr_len(ctx, items)) { JS_FreeValue(ctx, items); return JS_UNDEFINED; }
    item = JS_GetPropertyUint32(ctx, items, i);
    s = dt_slot(ctx, self, g_list_atom);
    owner = rec_get(ctx, s, "owner");
    r = item_object_of(ctx, owner, item);
    JS_FreeValue(ctx, owner);
    JS_FreeValue(ctx, s);
    JS_FreeValue(ctx, item);
    JS_FreeValue(ctx, items);
    return r;
}

static const IdlIndexedDecl ITEM_LIST_INDEXED = { "DataTransferItemList", list_indexed_length, list_indexed_item,
                                                  NULL, 0 };

static JSValue item_list_new(JSContext *ctx, JSValueConst dt)
{
    JSValue proto = JS_GetClassProto(ctx, g_list_class), list, s;

    DCHECK(!JS_IsNull(proto), "a DataTransferItemList was built in a realm that never installed the interface");
    list = idl_indexed_new(ctx, proto, &ITEM_LIST_INDEXED);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(list), "DataTransfer: OOM building the item list");
    s = rec_new(ctx);
    rec_set(ctx, s, "owner", JS_DupValue(ctx, dt));
    JS_DefinePropertyValue(ctx, list, g_list_atom, s, 0);
    return list;
}

static JSValue js_list_length(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue s = dt_slot(ctx, this_val, g_list_atom);

    (void)magic;
    if (JS_IsUndefined(s)) return JS_ThrowTypeError(ctx, "not a DataTransferItemList");
    JS_FreeValue(ctx, s);
    return JS_NewUint32(ctx, list_indexed_length(ctx, this_val));
}

/* add(data, type) and add(File data). Web IDL §3.6 steps 3-4 choose the entry by argument count, so one
   argument is the File entry and two the string entry (IDL_INTERFACE_OR_DOMSTRING_BY_ARITY). */
static JSValue js_list_add(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int m)
{
    JSValue s = dt_slot(ctx, this_val, g_list_atom), owner, slots, store, items, item, r;
    int mode;

    (void)m;
    if (JS_IsUndefined(s)) return JS_ThrowTypeError(ctx, "not a DataTransferItemList");
    owner = rec_get(ctx, s, "owner");
    JS_FreeValue(ctx, s);
    slots = dt_slot(ctx, owner, g_dt_atom);
    store = dt_store(ctx, slots);
    mode = JS_IsUndefined(store) ? -1 : dt_mode(ctx, store);
    if (mode != DND_MODE_READ_WRITE) {   /* step 1 */
        JS_FreeValue(ctx, store); JS_FreeValue(ctx, slots); JS_FreeValue(ctx, owner);
        return JS_NULL;
    }
    items = rec_get(ctx, store, "items");
    if (argc >= 2) {
        char *type = dt_format(ctx, argv[1], false, NULL);
        uint32_t n = arr_len(ctx, items), i;

        for (i = 0; i < n; i++) {
            JSValue it = JS_GetPropertyUint32(ctx, items, i);
            bool dup = drop_text_of_type(ctx, it, type);

            JS_FreeValue(ctx, it);
            if (dup) {
                free(type);
                JS_FreeValue(ctx, items); JS_FreeValue(ctx, store); JS_FreeValue(ctx, slots);
                JS_FreeValue(ctx, owner);
                return JS_ThrowDOMException(ctx, "NotSupportedError",
                                            "the drag data store already holds a text item of that type");
            }
        }
        item = item_new(ctx, "text", JS_NewString(ctx, type), JS_DupValue(ctx, argv[0]));
        free(type);
    } else {
        const char *ftype = NULL;
        char *lower;

        DCHECK(blob_file_name_of(argv[0]) != NULL, "add(File) reached its body with a value that is not a File — "
                                                   "the declaration's brand test is the File entry's");
        blob_bytes_of(argv[0], NULL, &ftype);
        lower = ascii_lower_dup(ftype ? ftype : "", ftype ? strlen(ftype) : 0);
        item = item_new(ctx, "file", JS_NewString(ctx, lower), JS_DupValue(ctx, argv[0]));
        free(lower);
    }
    dt_set_items(ctx, slots, store, items_append(ctx, items, JS_DupValue(ctx, item)));
    r = item_object_of(ctx, owner, item);   /* step 3 */
    JS_FreeValue(ctx, item); JS_FreeValue(ctx, items); JS_FreeValue(ctx, store); JS_FreeValue(ctx, slots);
    JS_FreeValue(ctx, owner);
    return r;
}

/* remove(index) (`magic` 0) and clear() (`magic` 1). */
static JSValue js_list_remove(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue slots = list_owner_slots(ctx, this_val), store, items;
    int mode;

    (void)argc;
    if (JS_IsUndefined(slots)) return JS_ThrowTypeError(ctx, "not a DataTransferItemList");
    store = dt_store(ctx, slots);
    mode = JS_IsUndefined(store) ? -1 : dt_mode(ctx, store);
    if (mode != DND_MODE_READ_WRITE) {
        JS_FreeValue(ctx, store);
        JS_FreeValue(ctx, slots);
        /* remove's step 1 throws; clear "must do nothing". */
        return magic ? JS_UNDEFINED
                     : JS_ThrowDOMException(ctx, "InvalidStateError", "the item list is not in the read/write mode");
    }
    items = rec_get(ctx, store, "items");
    if (magic) dt_set_items(ctx, slots, store, items_without(ctx, items, drop_all, NULL));
    else {
        uint32_t i;

        DCHECK(!concolic_is(argv[0]), "an unknown index reached DataTransferItemList's remove — the next diff "
                                      "asks core/idl_index_arg.h's elimination chain over the list's length");
        CHECK(JS_ToUint32(ctx, &i, argv[0]) == 0, "DataTransfer: a converted index could not be read");
        if (i < arr_len(ctx, items)) {   /* step 2 */
            JSValue target = JS_GetPropertyUint32(ctx, items, i);

            dt_set_items(ctx, slots, store, items_without(ctx, items, drop_at, JS_VALUE_GET_PTR(target)));
            JS_FreeValue(ctx, target);
        }
    }
    JS_FreeValue(ctx, items);
    JS_FreeValue(ctx, store);
    JS_FreeValue(ctx, slots);
    return JS_UNDEFINED;
}

/* ---- DataTransferItem -------------------------------------------------------------------------------------- */

/* The DataTransferItem for `item` of `owner`'s store, the one object made for it. Owned. */
static JSValue item_object_of(JSContext *ctx, JSValueConst owner, JSValueConst item)
{
    JSValue obj = rec_get(ctx, item, "object"), proto, s;

    if (!JS_IsUndefined(obj)) return obj;
    proto = JS_GetClassProto(ctx, g_item_class);
    DCHECK(!JS_IsNull(proto), "a DataTransferItem was built in a realm that never installed the interface");
    obj = JS_NewObjectProtoClass(ctx, proto, g_item_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "DataTransfer: OOM building an item");
    s = rec_new(ctx);
    rec_set(ctx, s, "owner", JS_DupValue(ctx, owner));
    rec_set(ctx, s, "item", JS_DupValue(ctx, item));
    JS_DefinePropertyValue(ctx, obj, g_item_atom, s, 0);
    rec_set(ctx, item, "object", JS_DupValue(ctx, obj));
    return obj;
}

/* The item record when the DataTransferItem is not in the disabled mode, owned, with its mode; JS_UNDEFINED
   when disabled. `*bad` is set for a receiver that is not a DataTransferItem. */
static JSValue item_live(JSContext *ctx, JSValueConst obj, int *mode, bool *bad)
{
    JSValue s = dt_slot(ctx, obj, g_item_atom), owner, slots, store, items, item, r = JS_UNDEFINED;
    uint32_t n, i;

    *mode = -1;
    *bad = JS_IsUndefined(s);
    if (*bad) return r;
    owner = rec_get(ctx, s, "owner");
    item = rec_get(ctx, s, "item");
    slots = dt_slot(ctx, owner, g_dt_atom);
    store = dt_store(ctx, slots);
    if (!JS_IsUndefined(store)) {
        items = rec_get(ctx, store, "items");
        n = arr_len(ctx, items);
        for (i = 0; i < n && JS_IsUndefined(r); i++) {
            JSValue it = JS_GetPropertyUint32(ctx, items, i);

            if (JS_VALUE_GET_PTR(it) == JS_VALUE_GET_PTR(item)) { r = JS_DupValue(ctx, item); *mode = dt_mode(ctx, store); }
            JS_FreeValue(ctx, it);
        }
        JS_FreeValue(ctx, items);
    }
    JS_FreeValue(ctx, store); JS_FreeValue(ctx, slots); JS_FreeValue(ctx, item); JS_FreeValue(ctx, owner);
    JS_FreeValue(ctx, s);
    return r;
}

enum { ITEM_KIND, ITEM_TYPE };

/* kind and type: the empty string in the disabled mode; kind is "string" for text and "file" for a File. */
static JSValue js_item_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    int mode;
    bool bad;
    JSValue item = item_live(ctx, this_val, &mode, &bad), r;

    if (bad) return JS_ThrowTypeError(ctx, "not a DataTransferItem");
    if (JS_IsUndefined(item)) return JS_NewString(ctx, "");
    r = magic == ITEM_TYPE ? rec_get(ctx, item, "type")
                           : JS_NewString(ctx, item_kind_is(ctx, item, "text") ? "string" : "file");
    JS_FreeValue(ctx, item);
    return r;
}

/* getAsString(callback): with a non-null callback, an item in the read/write or read-only mode and of kind
   text, queue a task to invoke callback with the item's data. Web IDL §3.2.20 makes undefined and null the IDL
   null and §3.2.19 refuses a non-callable. */
static JSValue js_item_get_as_string(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int m)
{
    int mode;
    bool bad;
    JSValue item, data;

    (void)argc; (void)m;
    if (JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {   /* step 1 */
        JSValue s = dt_slot(ctx, this_val, g_item_atom);
        bool is = !JS_IsUndefined(s);

        JS_FreeValue(ctx, s);
        return is ? JS_UNDEFINED : JS_ThrowTypeError(ctx, "not a DataTransferItem");
    }
    if (!JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "argument 1 is not a FunctionStringCallback");
    item = item_live(ctx, this_val, &mode, &bad);
    if (bad) return JS_ThrowTypeError(ctx, "not a DataTransferItem");
    if (!JS_IsUndefined(item) && (mode == DND_MODE_READ_WRITE || mode == DND_MODE_READ_ONLY) &&   /* step 2 */
        item_kind_is(ctx, item, "text")) {   /* step 3 */
        data = rec_get(ctx, item, "data");
        JS_EnqueueCallTask(ctx, argv[0], 1, (JSValueConst *)&data, TASK_SOURCE_UNNAMED_BY_ITS_STANDARD);
        JS_FreeValue(ctx, data);
    }
    JS_FreeValue(ctx, item);
    return JS_UNDEFINED;
}

/* getAsFile(): "Return a new File object representing the actual data" of a File item, in the read/write or
   read-only mode; null otherwise. The new File carries the item's bytes, name, type, timestamp and source. */
static JSValue js_item_get_as_file(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int m)
{
    int mode;
    bool bad;
    JSValue item = item_live(ctx, this_val, &mode, &bad), f, r;
    const char *bytes, *type = NULL, *name, *shape, *src;
    size_t len = 0;

    (void)argc; (void)argv; (void)m;
    if (bad) return JS_ThrowTypeError(ctx, "not a DataTransferItem");
    if (JS_IsUndefined(item) || (mode != DND_MODE_READ_WRITE && mode != DND_MODE_READ_ONLY) ||   /* step 1 */
        !item_kind_is(ctx, item, "file")) {   /* step 2 */
        JS_FreeValue(ctx, item);
        return JS_NULL;
    }
    f = rec_get(ctx, item, "data");
    JS_FreeValue(ctx, item);
    bytes = blob_bytes_of(f, &len, &type);
    name = blob_file_name_of(f);
    DCHECK(bytes != NULL && name != NULL, "a drag data store File item holds a value that is not a File");
    r = file_new(ctx, bytes, len, type ? type : "", type ? strlen(type) : 0, name, strlen(name),
                 blob_last_modified_of(f));
    CHECK(!JS_IsException(r), "DataTransfer: OOM building a File for getAsFile()");
    if (blob_source_of(f, &shape, &src)) blob_set_source(ctx, r, shape, src);
    JS_FreeValue(ctx, f);
    return r;
}

/* ---- install ---------------------------------------------------------------------------------------------- */

static bool dt_element_is(JSContext *ctx, JSValueConst v) { (void)ctx; return element_is(v); }
static bool dt_file_is(JSContext *ctx, JSValueConst v) { (void)ctx; return blob_file_name_of(v) != NULL; }

static void key_new(JSContext *ctx, const char *desc, JSValue *key, JSAtom *atom)
{
    *key = JS_NewSymbol(ctx, desc, false);
    CHECK(!JS_IsException(*key), "DataTransfer: a slot key could not be allocated");
    *atom = JS_ValueToAtom(ctx, *key);
    CHECK(*atom != JS_ATOM_NULL, "DataTransfer: a slot key could not be interned");
}

static void class_new(JSContext *ctx, JSClassID *id, const char *name)
{
    JSClassDef d = { name };

    JS_NewClassID(JS_GetRuntime(ctx), id);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), *id, &d) == 0, "DataTransfer: a prototype slot could not be declared");
}

void data_transfer_init(JSContext *ctx)
{
    static const IdlArgType IMAGE_ARGS[3] = { IDL_INTERFACE, IDL_LONG, IDL_LONG };
    static const IdlArgType ONE_STRING[1] = { IDL_DOMSTRING };
    static const IdlArgType TWO_STRINGS[2] = { IDL_DOMSTRING, IDL_DOMSTRING };
    static const IdlArgType ADD_ARGS[2] = { IDL_INTERFACE_OR_DOMSTRING_BY_ARITY, IDL_DOMSTRING };
    static const IdlArgType ONE_ULONG[1] = { IDL_UNSIGNED_LONG };
    static const IdlArgType ONE_ANY[1] = { IDL_ANY };

    DCHECK(!g_ready, "data_transfer_init ran twice — the interfaces are declared once per agent");
    class_new(ctx, &g_dt_class, "DataTransfer");
    class_new(ctx, &g_list_class, "DataTransferItemList");
    class_new(ctx, &g_item_class, "DataTransferItem");
    key_new(ctx, "dataTransferSlots", &g_dt_key, &g_dt_atom);
    key_new(ctx, "dataTransferItemListSlots", &g_list_key, &g_list_atom);
    key_new(ctx, "dataTransferItemSlots", &g_item_key, &g_item_atom);
    g_ctor_id = idl_method_id(ctx, NULL, 0, js_dt_ctor, 0);
    g_drop_effect_set = idl_setter_id(ctx, IDL_DOMSTRING, false, js_dt_set_effect, DT_DROP_EFFECT);
    g_effect_allowed_set = idl_setter_id(ctx, IDL_DOMSTRING, false, js_dt_set_effect, DT_EFFECT_ALLOWED);
    g_set_drag_image_id = idl_method_id(ctx, IMAGE_ARGS, 3, js_dt_set_drag_image, 0);
    idl_arg_iface(0, dt_element_is, "Element");
    g_get_data_id = idl_method_id(ctx, ONE_STRING, 1, js_dt_get_data, 0);
    g_set_data_id = idl_method_id(ctx, TWO_STRINGS, 2, js_dt_set_data, 0);
    g_clear_data_id = idl_method_id(ctx, ONE_STRING, 1, js_dt_clear_data, 0);
    idl_optional_from(0);
    g_add_id = idl_method_id(ctx, ADD_ARGS, 2, js_list_add, 0);
    idl_arg_iface(0, dt_file_is, "File");
    idl_optional_from(1);                  /* the File entry's only position, one past which none is optional */
    idl_overload_split_optional_from(2);   /* the string entry's two positions are both required */
    g_remove_id = idl_method_id(ctx, ONE_ULONG, 1, js_list_remove, 0);
    g_clear_id = idl_method_id(ctx, NULL, 0, js_list_remove, 1);
    g_get_as_string_id = idl_method_id(ctx, ONE_ANY, 1, js_item_get_as_string, 0);
    g_get_as_file_id = idl_method_id(ctx, NULL, 0, js_item_get_as_file, 0);
    g_ready = 1;
    agent_state_flag(DT_COMPONENT, &g_ready, "HTML §6.11.3's declaration latch");
    agent_state_class(DT_COMPONENT, &g_dt_class, "HTML §6.11.3 DataTransfer's per-realm prototype slot");
    agent_state_class(DT_COMPONENT, &g_list_class, "HTML §6.11.3.1 DataTransferItemList's per-realm prototype slot");
    agent_state_class(DT_COMPONENT, &g_item_class, "HTML §6.11.3.2 DataTransferItem's per-realm prototype slot");
    agent_state_value(DT_COMPONENT, &g_dt_key, "the private Symbol a DataTransfer's slot record hangs off");
    agent_state_value(DT_COMPONENT, &g_list_key, "the private Symbol a DataTransferItemList's slot record hangs off");
    agent_state_value(DT_COMPONENT, &g_item_key, "the private Symbol a DataTransferItem's slot record hangs off");
    agent_state_id(DT_COMPONENT, &g_ctor_id, "HTML §6.11.3's DataTransfer() constructor");
    agent_state_id(DT_COMPONENT, &g_drop_effect_set, "HTML §6.11.3's dropEffect setter");
    agent_state_id(DT_COMPONENT, &g_effect_allowed_set, "HTML §6.11.3's effectAllowed setter");
    agent_state_id(DT_COMPONENT, &g_set_drag_image_id, "HTML §6.11.3's setDragImage(image, x, y)");
    agent_state_id(DT_COMPONENT, &g_get_data_id, "HTML §6.11.3's getData(format)");
    agent_state_id(DT_COMPONENT, &g_set_data_id, "HTML §6.11.3's setData(format, data)");
    agent_state_id(DT_COMPONENT, &g_clear_data_id, "HTML §6.11.3's clearData(format)");
    agent_state_id(DT_COMPONENT, &g_add_id, "HTML §6.11.3.1's add()");
    agent_state_id(DT_COMPONENT, &g_remove_id, "HTML §6.11.3.1's remove(index)");
    agent_state_id(DT_COMPONENT, &g_clear_id, "HTML §6.11.3.1's clear()");
    agent_state_id(DT_COMPONENT, &g_get_as_string_id, "HTML §6.11.3.2's getAsString(callback)");
    agent_state_id(DT_COMPONENT, &g_get_as_file_id, "HTML §6.11.3.2's getAsFile()");
    realm_declare_intrinsic(data_transfer_install_protos);
}

static JSValue proto_new(JSContext *ctx, JSClassID id, const char *name)
{
    JSValue prev = JS_GetClassProto(ctx, id), proto;

    DCHECK(JS_IsNull(prev), "a DataTransfer interface prototype was installed twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "DataTransfer: an interface prototype could not be allocated");
    idl_interface_tag(ctx, proto, name);
    return proto;
}

static void expose(JSContext *ctx, const char *name, JSValue ctor)
{
    JSValue global = JS_GetGlobalObject(ctx);

    CHECK(!JS_IsException(ctor), "DataTransfer: an interface object could not be allocated");
    idl_define_global_property_reference(ctx, global, name, ctor);
    JS_FreeValue(ctx, global);
}

void data_transfer_install_protos(JSContext *ctx)
{
    JSValue proto, ctor;

    DCHECK(g_ready, "a realm asked for DataTransfer before data_transfer_init declared it");
    proto = proto_new(ctx, g_dt_class, "DataTransfer");
    idl_install_accessor(ctx, proto, "dropEffect", js_dt_get, DT_DROP_EFFECT, g_drop_effect_set);
    idl_install_accessor(ctx, proto, "effectAllowed", js_dt_get, DT_EFFECT_ALLOWED, g_effect_allowed_set);
    idl_install_accessor(ctx, proto, "items", js_dt_get, DT_ITEMS, -1);
    idl_install_method(ctx, proto, "setDragImage", g_set_drag_image_id);
    idl_install_accessor(ctx, proto, "types", js_dt_get, DT_TYPES, -1);
    idl_install_method(ctx, proto, "getData", g_get_data_id);
    idl_install_method(ctx, proto, "setData", g_set_data_id);
    idl_install_method(ctx, proto, "clearData", g_clear_data_id);
    idl_install_accessor(ctx, proto, "files", js_dt_get, DT_FILES, -1);
    JS_SetClassProto(ctx, g_dt_class, JS_DupValue(ctx, proto));
    ctor = idl_step_constructor(ctx, "DataTransfer", g_ctor_id);
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    expose(ctx, "DataTransfer", ctor);

    proto = proto_new(ctx, g_list_class, "DataTransferItemList");
    idl_install_accessor_no_user_code(ctx, proto, "length", js_list_length, 0, -1);
    idl_install_method(ctx, proto, "add", g_add_id);
    idl_install_method(ctx, proto, "remove", g_remove_id);
    idl_install_method(ctx, proto, "clear", g_clear_id);
    idl_indexed_install_iterable(ctx, proto);
    JS_SetClassProto(ctx, g_list_class, JS_DupValue(ctx, proto));
    ctor = idl_interface_object(ctx, "DataTransferItemList", proto);
    JS_FreeValue(ctx, proto);
    expose(ctx, "DataTransferItemList", ctor);

    proto = proto_new(ctx, g_item_class, "DataTransferItem");
    idl_install_accessor(ctx, proto, "kind", js_item_get, ITEM_KIND, -1);
    idl_install_accessor(ctx, proto, "type", js_item_get, ITEM_TYPE, -1);
    idl_install_method(ctx, proto, "getAsString", g_get_as_string_id);
    idl_install_method(ctx, proto, "getAsFile", g_get_as_file_id);
    JS_SetClassProto(ctx, g_item_class, JS_DupValue(ctx, proto));
    ctor = idl_interface_object(ctx, "DataTransferItem", proto);
    JS_FreeValue(ctx, proto);
    expose(ctx, "DataTransferItem", ctor);
}

void data_transfer_free(JSRuntime *rt)
{
    DCHECK(g_ready, "HTML §6.11.3 was released in an agent that never declared it");
    JS_FreeAtomRT(rt, g_dt_atom); JS_FreeAtomRT(rt, g_list_atom); JS_FreeAtomRT(rt, g_item_atom);
    g_dt_atom = g_list_atom = g_item_atom = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_dt_key); JS_FreeValueRT(rt, g_list_key); JS_FreeValueRT(rt, g_item_key);
    g_dt_key = g_list_key = g_item_key = JS_UNDEFINED;
    g_dt_class = g_list_class = g_item_class = 0;
    g_ctor_id = g_drop_effect_set = g_effect_allowed_set = g_set_drag_image_id = g_get_data_id = -1;
    g_set_data_id = g_clear_data_id = g_add_id = g_remove_id = g_clear_id = -1;
    g_get_as_string_id = g_get_as_file_id = -1;
    g_ready = 0;
}
