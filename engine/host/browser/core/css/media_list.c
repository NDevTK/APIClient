/* CSSOM §4.4 — MediaList. See media_list.h for why the collection is a JS Array of serialized queries.
 *
 * THE BRAND IS THE OWN SLOT, exactly as core/css/css_rule_list.c's is and for the same reason: an
 * indexed-property object is what anything with an indexed getter is, so the class cannot tell one collection
 * from another and the slot has to. The key is a private Symbol, so a page can neither read the collection nor
 * forge one. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/css/media_list.h"
#include "core/css/media_query.h"
#include "core/idl_args.h"
#include "core/idl_index_arg.h"
#include "core/idl_indexed.h"
#include "core/realm.h"
#include "solver/concolic.h"   /* CSSOM §4.4's `item` takes an index unknown input crosses AS ITSELF */

static JSClassID g_list_class;
static JSValue   g_queries_key = JS_UNDEFINED;
static JSAtom    g_atom_queries = JS_ATOM_NULL;
static int       g_id_set_media_text = -1, g_id_item = -1, g_id_append = -1, g_id_delete = -1;
static int       g_id_to_string = -1, g_id_put_forwards = -1;

/* THE COLLECTION. An own SLOT read, never a property lookup: a lookup walks the prototype chain into the
   solver's absent-state seam and would mint a concolic for a name nobody defined. JS_UNDEFINED for a stranger,
   which is what the brand below tests. */
static JSValue ml_queries(JSContext *ctx, JSValueConst v)
{
    JSValue arr;

    if (!JS_IsObject(v)) return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &arr, v, g_atom_queries) <= 0) return JS_UNDEFINED;
    return arr;
}

bool media_list_is(JSContext *ctx, JSValueConst v)
{
    JSValue arr = ml_queries(ctx, v);
    bool ok = JS_IsArray(arr);

    JS_FreeValue(ctx, arr);
    return ok;
}

static uint32_t ml_length(JSContext *ctx, JSValueConst self)
{
    JSValue arr = ml_queries(ctx, self), len;
    uint32_t n = 0;

    if (!JS_IsArray(arr)) { JS_FreeValue(ctx, arr); return 0; }
    len = JS_GetPropertyStr(ctx, arr, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    JS_FreeValue(ctx, arr);
    return n;
}

/* Web IDL §3.9 Legacy platform objects' INDEXED PROPERTY GETTER — JS_UNDEFINED past the end, which is what a
   lookup outside CSSOM §4.4 The MediaList Interface's supported property indices is ("the numbers in the range
   zero to one less than the number of media queries in the collection of media queries represented by the
   collection"). `item()` below turns that into the null its IDL declares.
   BOTH STANDARDS ARE NAMED HERE and neither used to be, which is what made every bare `§4.4` in this file
   unattributable: `supported property indices` is a Web IDL term, so this site read as naming Web IDL §4.4 —
   which is DOMException — and citegen then inferred Web IDL for the whole file's bare numbers off it. A file
   whose convention is one standard still has to say so at any site where a term belongs to another. */
static JSValue ml_item(JSContext *ctx, JSValueConst self, uint32_t i)
{
    JSValue arr = ml_queries(ctx, self), q;

    if (!JS_IsArray(arr)) { JS_FreeValue(ctx, arr); return JS_UNDEFINED; }
    q = JS_GetPropertyUint32(ctx, arr, i);
    JS_FreeValue(ctx, arr);
    DCHECK(JS_IsUndefined(q) || JS_IsString(q),
           "a MediaList held something that is not a serialized media query — its collection is filled only by "
           "the three members below, and every one of them appends what media_query_serialize_at produced");
    return q;
}

static const IdlIndexedDecl ML_INDEXED = { "MediaList", ml_length, ml_item, NULL, 0 };

/* ---- §4.2's SERIALIZE A MEDIA QUERY LIST, over the collection ---------------------------------------------- */

/* "If the media query list is empty, then return the empty string. Serialize each media query in the list, in
   the same order as they appear, and then serialize the list" — a comma-separated list, which CSSOM §2.1's
   ("Common Serializing Idioms") serialize-a-comma-separated-list joins with ", ". Never NULL. */
static char *ml_serialize(JSContext *ctx, JSValueConst self)
{
    uint32_t n = ml_length(ctx, self), i;
    JSValue arr = ml_queries(ctx, self);
    size_t cap = 1, at = 0;
    char *out;

    out = malloc(cap);
    CHECK(out != NULL, "cssom: OOM serializing a media query list");
    out[0] = '\0';
    for (i = 0; i < n; i++) {
        JSValue q = JS_GetPropertyUint32(ctx, arr, i);
        size_t len = 0;
        const char *c = JS_ToCStringLen(ctx, &len, q);
        char *grown;

        JS_FreeValue(ctx, q);
        if (!c) { free(out); JS_FreeValue(ctx, arr); return NULL; }   /* the conversion threw */
        cap = at + len + 3;
        grown = realloc(out, cap);
        CHECK(grown != NULL, "cssom: OOM serializing a media query list");
        out = grown;
        if (at) { memcpy(out + at, ", ", 2); at += 2; }
        memcpy(out + at, c, len);
        at += len;
        out[at] = '\0';
        JS_FreeCString(ctx, c);
    }
    JS_FreeValue(ctx, arr);
    return out;
}

char *media_list_text(JSContext *ctx, JSValueConst list)
{
    DCHECK(media_list_is(ctx, list), "a media query list serialization was read off something that is not a "
                                     "MediaList");
    return ml_serialize(ctx, list);
}

MediaQuerySet *media_list_query_set(JSContext *ctx, JSValueConst list)
{
    char *text = media_list_text(ctx, list);
    MediaQuerySet *set;

    /* A NULL text is a `toString` that threw inside the collection, which cannot happen — the collection holds
       engine-built strings — so it is an invariant rather than a branch. */
    DCHECK(text != NULL, "a MediaList's collection held a value that would not convert to a string");
    set = media_query_parse(text ? text : "");
    free(text);
    return set;
}

/* ---- §4.4's members ---------------------------------------------------------------------------------------- */

/* The receiver, brand-checked. Every member is on the PROTOTYPE, so a page can apply one to anything at all and
   the answer — Web IDL §3.7.6 Attributes' for `mediaText` and `length`, §3.7.7 Operations' for `item`,
   `appendMedium` and `deleteMedium` — is a TypeError rather than a read of nothing. */
static bool ml_here(JSContext *ctx, JSValueConst v, const char *member)
{
    if (media_list_is(ctx, v)) return true;
    JS_ThrowTypeError(ctx, "MediaList.prototype.%s was reached on something that is not a MediaList", member);
    return false;
}

/* "The mediaText attribute, on getting, must return a serialization of the collection of media queries." */
static JSValue js_ml_media_text(JSContext *ctx, JSValueConst this_val, int magic)
{
    char *text;
    JSValue out;

    (void)magic;
    if (!ml_here(ctx, this_val, "mediaText")) return JS_EXCEPTION;
    text = ml_serialize(ctx, this_val);
    if (!text) return JS_EXCEPTION;
    out = JS_NewString(ctx, text);
    free(text);
    return out;
}

/* Replace the collection with the queries `text` parses to. §4.4's setter, entire:
     1. Empty the collection of media queries.
     2. If the given value is the empty string, then return.
     3. Append all the media queries as a result of parsing the given value to the collection.
   Step 1 runs BEFORE step 2's early return, which is the whole reason `mediaText = ''` empties the list rather
   than leaving it alone. */
static void ml_set_text(JSContext *ctx, JSValueConst self, const char *text)
{
    JSValue arr = ml_queries(ctx, self);
    MediaQuerySet *set;
    int n, i;

    DCHECK(JS_IsArray(arr), "a MediaList's collection was replaced on an object that has none");
    JS_SetPropertyStr(ctx, arr, "length", JS_NewUint32(ctx, 0));      /* STEP 1 */
    if (!text || !*text) { JS_FreeValue(ctx, arr); return; }          /* STEP 2 */
    set = media_query_parse(text);                                    /* STEP 3 */
    n = media_query_count(set);
    for (i = 0; i < n; i++) {
        char *one = media_query_serialize_at(set, i);

        CHECK(one != NULL, "cssom: OOM appending a parsed media query to a MediaList");
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewString(ctx, one));
        free(one);
    }
    media_query_free(set);
    JS_FreeValue(ctx, arr);
}

/* `stringifier attribute [LegacyNullToEmptyString] CSSOMString mediaText` — the null-to-empty is part of the
   TYPE and is the declaration's, so `ml.mediaText = null` empties the collection rather than parsing the four
   characters "null". */
static JSValue js_ml_set_media_text(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    const char *v;

    (void)magic;
    if (!ml_here(ctx, this_val, "mediaText")) return JS_EXCEPTION;
    v = JS_ToCString(ctx, val);   /* a real string by now: the declaration converted it */
    if (!v) return JS_EXCEPTION;
    ml_set_text(ctx, this_val, v);
    JS_FreeCString(ctx, v);
    return JS_UNDEFINED;
}

static JSValue js_ml_length(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!ml_here(ctx, this_val, "length")) return JS_EXCEPTION;
    return JS_NewUint32(ctx, ml_length(ctx, this_val));
}

/* "The item(index) method must return a serialization of the media query in the collection of media queries
   given by index, or null, if index is greater than or equal to the number of media queries in the collection
   of media queries." That null is the whole difference from the indexed getter, which is why both exist. */
/* IT IS A STEP MACHINE BECAUSE ITS ONE ARGUMENT CAN BE UNKNOWN. A `JS_ToUint32` of `argv[0]` with its return
 * discarded stood here under a comment saying the declaration had already converted it — the shape
 * core/idl_args.h bans by name — and Web IDL §3.2's conversion is a BOUNDARY unknown external input crosses AS
 * ITSELF (idl_concolic_rule answers IDL_CONCOLIC_CROSSES for every integer type, IDL_UNSIGNED_LONG among
 * them), so `ml.item(location.hash.length)` reached that line still holding the unknown. The coercion does not
 * return a wrong number: ToNumber hands a concolic straight back and the engine aborts INSIDE it, one frame
 * below this file, which is why checking the return is no defence — there is no return to check.
 * THE FORK IS core/idl_index_arg.h's elimination chain, and asking it is what a plain C activation has nowhere
 * to park for. */
#define ML_ITEM_ALGORITHM "CSSOM §4.4 The MediaList Interface item(index)"
#define ML_ITEM_STAGES(X)                                                                                     \
    X(ML_ITEM_READ, ML_ITEM_ALGORITHM " (return a serialization of the media query given by index, or null)")
enum { IDL_STEP_STAGE_BASE(ML_ITEM_STAGES) ML_ITEM_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const ML_ITEM_STEPS[] = { ML_ITEM_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_ml_item(JSContext *ctx, JSStepHdr *hdr, void *state, int argc, JSValueConst *argv,
                      JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    IdlIndexChain *s = state;
    uint32_t i = 0;
    bool past_end = false;
    JSValue q;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);   /* this machine makes no request that delivers a value */
    *presult = JS_UNDEFINED;
    DCHECK(hdr->stage == ML_ITEM_READ,
           "§4.4's `item` resumed into a stage the algorithm does not have — it is ONE sentence, and the chain "
           "of questions it may ask is a cursor on this machine's own state rather than a stage apiece");
    DCHECK(argc == 1,
           "§4.4's `item` reached its body with an argument count its declaration does not produce — its one "
           "`unsigned long index` is required, so §3.6's argument-count check refuses a shorter call first");
    if (!ml_here(ctx, hdr->this_val, "item"))
        return JS_STEP_ABRUPT;   /* §3.7.7 Operations' TypeError on a receiver that is not a MediaList */
    if (concolic_is(argv[0])) {
        int rc = idl_index_chain_run(ctx, hdr, s, argv[0], ml_length(ctx, hdr->this_val),
                                     ML_ITEM_ALGORITHM, &i, &past_end);
        if (rc)
            return rc;   /* parked at the fork */
        if (past_end) {
            /* §4.4's OWN past-the-end answer: null "if index is greater than or equal to the number of media
               queries in the collection of media queries". */
            *presult = JS_NULL;
            return JS_STEP_DONE;
        }
    } else {
        i = idl_index_arg_known(ctx, argv[0], ML_ITEM_ALGORITHM);
    }
    q = ml_item(ctx, hdr->this_val, i);
    *presult = JS_IsUndefined(q) ? JS_NULL : q;
    return JS_STEP_DONE;
}

static const IdlStepDecl ML_ITEM_DECL = {
    js_ml_item, sizeof(IdlIndexChain), idl_index_chain_visit, NULL,
    ML_ITEM_ALGORITHM, ML_ITEM_STEPS, 0, NULL
};

/* §4.4's "COMPARING" two media queries, which the spec names and does not define. It is the CANONICAL FORM that
   decides: both sides have been through the same parse and the same §4.2 serializer, so `(MIN-WIDTH:5PX)` and
   `(min-width: 5px)` are one query and a byte comparison of the two serializations says so. Comparing the
   page's own spellings instead would make `deleteMedium` miss the very query `appendMedium` had just put in. */
static int ml_index_of(JSContext *ctx, JSValueConst self, const char *one)
{
    uint32_t n = ml_length(ctx, self), i;
    JSValue arr = ml_queries(ctx, self);
    int found = -1;

    for (i = 0; i < n && found < 0; i++) {
        JSValue q = JS_GetPropertyUint32(ctx, arr, i);
        const char *c = JS_ToCString(ctx, q);

        if (c && strcmp(c, one) == 0) found = (int)i;
        if (c) JS_FreeCString(ctx, c);
        JS_FreeValue(ctx, q);
    }
    JS_FreeValue(ctx, arr);
    return found;
}

/* §4.4's appendMedium:
     1. Let m be the result of parsing the given value.
     2. If m is null, then return.
     3. If comparing m with any of the media queries in the collection returns true, then return.
     4. Append m to the collection.
   Step 1 is MQ4 §3.1's parse a media QUERY and not a list, which is why an unparseable medium appends nothing
   where a list-shaped parse would have appended `not all`. */
static JSValue js_ml_append(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    const char *v;
    MediaQuerySet *m;
    char *one;

    (void)magic;
    if (!ml_here(ctx, this_val, "appendMedium")) return JS_EXCEPTION;
    DCHECK(argc >= 1, "§4.4's `appendMedium` reached its body with no medium — its IDL argument is required");
    v = JS_ToCString(ctx, argv[0]);
    if (!v) return JS_EXCEPTION;
    m = media_query_parse_one(v);
    JS_FreeCString(ctx, v);
    if (!m) return JS_UNDEFINED;                                      /* STEP 2 */
    one = media_query_serialize_at(m, 0);
    media_query_free(m);
    CHECK(one != NULL, "cssom: OOM serializing a medium being appended to a MediaList");
    if (ml_index_of(ctx, this_val, one) < 0) {                        /* STEP 3 */
        JSValue arr = ml_queries(ctx, this_val);

        JS_SetPropertyUint32(ctx, arr, ml_length(ctx, this_val), JS_NewString(ctx, one));   /* STEP 4 */
        JS_FreeValue(ctx, arr);
    }
    free(one);
    return JS_UNDEFINED;
}

/* §4.4's deleteMedium:
     1. Let m be the result of parsing the given value.
     2. If m is null, then return.
     3. Remove any media query from the collection for which comparing the media query with m returns true. If
        nothing was removed, then throw a NotFoundError exception.
   "ANY media query", plural: a collection cannot hold two equal queries (appendMedium refuses one and the
   setter's parse produces the list the page wrote), yet the step is stated over all of them, so the loop
   removes every match rather than stopping at the first. */
static JSValue js_ml_delete(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    const char *v;
    MediaQuerySet *m;
    char *one;
    JSValue arr;
    uint32_t n, i, kept = 0;
    bool removed = false;

    (void)magic;
    if (!ml_here(ctx, this_val, "deleteMedium")) return JS_EXCEPTION;
    DCHECK(argc >= 1, "§4.4's `deleteMedium` reached its body with no medium — its IDL argument is required");
    v = JS_ToCString(ctx, argv[0]);
    if (!v) return JS_EXCEPTION;
    m = media_query_parse_one(v);
    JS_FreeCString(ctx, v);
    if (!m) return JS_UNDEFINED;                                      /* STEP 2 */
    one = media_query_serialize_at(m, 0);
    media_query_free(m);
    CHECK(one != NULL, "cssom: OOM serializing a medium being deleted from a MediaList");
    arr = ml_queries(ctx, this_val);
    n = ml_length(ctx, this_val);
    for (i = 0; i < n; i++) {
        JSValue q = JS_GetPropertyUint32(ctx, arr, i);
        const char *c = JS_ToCString(ctx, q);
        bool match = c && strcmp(c, one) == 0;

        if (c) JS_FreeCString(ctx, c);
        if (match) { removed = true; JS_FreeValue(ctx, q); continue; }
        JS_SetPropertyUint32(ctx, arr, kept++, q);
    }
    JS_SetPropertyStr(ctx, arr, "length", JS_NewUint32(ctx, kept));
    JS_FreeValue(ctx, arr);
    free(one);
    if (!removed)
        return JS_ThrowDOMException(ctx, "NotFoundError", "the media query is not in this media list");
    return JS_UNDEFINED;
}

/* §4.4's STRINGIFIER IS `mediaText` — the same attribute under the operation's name, so it reads the same thing
   rather than formatting one. Its own body because a getter and a method have different shapes, and casting one
   to the other reads `magic` out of `argc`. */
static JSValue js_ml_to_string(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)argc; (void)argv;
    return js_ml_media_text(ctx, this_val, magic);
}

int media_list_put_forwards_setter(void)
{
    DCHECK(g_id_put_forwards >= 0,
           "Web IDL §3.3.10's [PutForwards=mediaText] setter was asked for before media_list_init declared it "
           "— every prototype that carries a `media` attribute is installed after this component's init runs");
    return g_id_put_forwards;
}

/* ---- the interface ------------------------------------------------------------------------------------------ */

JSValue media_list_new(JSContext *ctx, const char *text)
{
    JSValue proto, obj, arr;

    DCHECK(g_list_class != 0, "a MediaList was built before media_list_init declared the interface");
    proto = JS_GetClassProto(ctx, g_list_class);
    DCHECK(!JS_IsNull(proto), "a MediaList was built in a realm that never ran its prototype install");
    obj = idl_indexed_new(ctx, proto, &ML_INDEXED);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "a MediaList could not be allocated");
    arr = JS_NewArray(ctx);
    CHECK(!JS_IsException(arr), "a MediaList's collection could not be allocated");
    /* THE SLOT NEVER MOVES — every member below mutates the Array IN PLACE, `mediaText =` included, because
       that is what makes each mutation a property write the per-flow COW delta already captures. So the slot is
       defined once and unwritable, exactly as core/css/css_rule_list.c's is. */
    JS_DefinePropertyValue(ctx, obj, g_atom_queries, arr, 0);
    /* §4.4's create step 2 — "set its mediaText attribute to text", which is the parse, not a copy. */
    ml_set_text(ctx, obj, text);
    return obj;
}

void media_list_init(JSContext *ctx)
{
    JSClassDef d = { "MediaList" };
    static const IdlArgType ONE_ULONG[1] = { IDL_UNSIGNED_LONG };
    static const IdlArgType ONE_STR[1] = { IDL_DOMSTRING };

    if (g_list_class) return;   /* one AGENT, one class and one set of pool entries */
    JS_NewClassID(JS_GetRuntime(ctx), &g_list_class);
    JS_NewClass(JS_GetRuntime(ctx), g_list_class, &d);
    g_queries_key = JS_NewSymbol(ctx, "mediaListQueries", false);
    CHECK(!JS_IsException(g_queries_key), "the MediaList slot key allocation failed");
    g_atom_queries = JS_ValueToAtom(ctx, g_queries_key);
    CHECK(g_atom_queries != JS_ATOM_NULL, "the MediaList slot key could not be interned");
    g_id_set_media_text = idl_setter_id(ctx, IDL_DOMSTRING, /*null_to_empty*/ true, js_ml_set_media_text, 0);
    /* §4.4's `item` IS A MACHINE — a declaration and not a dispatch, since there is no second body for
       anything to select against. Its one `unsigned long index` can be unknown external input. */
    g_id_item = idl_method_id_step(ctx, ONE_ULONG, 1, NULL, 0, &ML_ITEM_DECL, 0);
    g_id_append = idl_method_id(ctx, ONE_STR, 1, js_ml_append, 0);
    g_id_delete = idl_method_id(ctx, ONE_STR, 1, js_ml_delete, 0);
    g_id_to_string = idl_method_id(ctx, ONE_STR, 1, js_ml_to_string, 0);
    idl_optional_from(0);   /* §4.4's stringifier takes NO arguments — the declared one is `mediaText`'s */
    /* Web IDL §3.3.10's [PutForwards=mediaText], whose five steps are §3.7.6 Attributes' and are declared once
       for the whole platform. This component states only the PAIR — see media_list.h. */
    g_id_put_forwards = idl_setter_id_put_forwards(ctx, "media", "mediaText");
    realm_declare_intrinsic(media_list_install_proto);
}

void media_list_install_proto(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_list_class != 0, "a realm asked for MediaList.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_list_class);
    DCHECK(JS_IsNull(prev), "media_list_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "MediaList.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "MediaList");
    idl_install_accessor(ctx, proto, "mediaText", js_ml_media_text, 0, g_id_set_media_text);
    idl_install_accessor_no_user_code(ctx, proto, "length", js_ml_length, 0, -1);
    idl_install_method(ctx, proto, "item", g_id_item);
    idl_install_method(ctx, proto, "appendMedium", g_id_append);
    idl_install_method(ctx, proto, "deleteMedium", g_id_delete);
    idl_install_method(ctx, proto, "toString", g_id_to_string);
    /* Web IDL §3.7.9 step 1.1: an indexed property getter plus an integer `length` gets %Array.prototype.values% as
       @@iterator. §4.4 declares no `iterable<>`, so it gets that and NOT `entries`/`keys`/`forEach`. */
    idl_indexed_install_iterable(ctx, proto);
    JS_SetClassProto(ctx, g_list_class, proto);
}

void media_list_install(JSContext *ctx, JSValueConst global)
{
    JSValue proto = JS_GetClassProto(ctx, g_list_class);

    DCHECK(!JS_IsNull(proto), "MediaList was installed in a realm that never ran its prototype install");
    /* §4.4 declares no constructor, so the interface object's call and construct both throw. */
    JS_SetPropertyStr(ctx, (JSValue)global, "MediaList", idl_interface_object(ctx, "MediaList", proto));
    JS_FreeValue(ctx, proto);
}

void media_list_free(JSRuntime *rt)
{
    if (!g_list_class) return;   /* the prototype is the REALM's — released with its context */
    JS_FreeAtomRT(rt, g_atom_queries);
    g_atom_queries = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_queries_key);
    g_queries_key = JS_UNDEFINED;
    g_id_item = g_id_append = g_id_delete = g_id_to_string = -1;
    g_id_set_media_text = g_id_put_forwards = -1;
}
