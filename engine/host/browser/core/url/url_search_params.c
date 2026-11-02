/* THE URLSearchParams INTERFACE — WHATWG URL §6, over §5.1's `application/x-www-form-urlencoded` list.
 *
 * It is a LIST OF PAIRS and not a map, for the reason the header list is: `?a=1&a=2` has two entries, `getAll`
 * reads them both back, and `get` answers with the first. A map keyed by name would answer `get` and lose
 * every repeat.
 *
 * IT WRITES BACK. §6.2 URLSearchParams class gives the object an associated URL, and every mutation runs
 * the UPDATE STEPS: serialize the list and set the URL's query to it, or to null when the list is empty.
 * That is what makes `u.searchParams.set("a", "1")` change `u.href`, which is the entire reason the
 * accessor exists rather than
 * the page building a query string itself. The link is [SameObject] in both directions — one object per URL,
 * held by the URL, and holding the URL back so the update steps have something to write to. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/url/url.h"
#include "core/url/url_search_params.h"
#include "core/idl_args.h"
#include "core/agent_state.h"
#include "core/realm.h"
#include "core/idl_iter.h"
#include "core/idl_name_chain.h"
#include "solver/concolic.h"
#include "solver/cow.h"
#include <lexbor/core/base.h>   /* lexbor_malloc: the allocator the engine gives its refusal edge */

/* A PAIR CARRIES ITS LENGTHS, because a name or a value may contain U+0000 — `?a=b%00c` is one pair whose
   value is three characters, and every strlen in this file would have made it one. */
/* §5.1's list is the URL Standard's and lives with it; this file is the INTERFACE over one. */
typedef UrlEncodedPair UspPair;
typedef UrlEncodedList UspList;
#define usp_list_free    url_encoded_list_free
#define usp_list_append  url_encoded_list_append
#define usp_parse        url_encoded_parse
#define usp_serialize    url_encoded_serialize
#define usp_name_cmp     url_encoded_name_cmp
#define usp_strdup       url_encoded_strdup

/* The class opaque. `owner` is the URL wrapper this belongs to, or JS_UNDEFINED — see the file comment.
   `vals` is §6.2's list itself: an Array of 2n USVStrings, each tuple's name then its value, where a half that
   is unknown input is that unknown, so a read hands it back and a comparison with it forks. `list` is §5.1's
   byte list projected from `vals` (usp_project), which the serializers read.
   `unknown` is the unknown string the constructor parsed, or JS_UNDEFINED: when set, `vals` is the parse of its
   example (empty where it has none); get and has(name) fork over whether it holds the tuple (usp_lookup_answer)
   and every other read answers a derivation over it (usp_derive).
   The list is per-flow state and is never changed in place: a write builds a new `vals` and its projection in a
   new box (usp_commit), so the capture restores a sibling's three fields together and no arm sees another's
   write. `mutated` says a §6.2 mutation has made `unknown` the derivation it ran (usp_rederive), so the list
   is no longer the parse of a source's own bytes. */
typedef struct { JSValue list_box; UspList *list; JSValue vals; JSValue owner; JSValue unknown; int unknown_example;
                 int mutated; } UspObj;
#define USP_OFF(f) (uint16_t)offsetof(UspObj, f)
static const uint16_t USP_VALS[] = { USP_OFF(list_box), USP_OFF(vals), USP_OFF(owner), USP_OFF(unknown) };
static const CowRecord USP_REC = { sizeof(UspObj), USP_VALS, (int)(sizeof USP_VALS / sizeof *USP_VALS) };

/* This component's claimant name in the source registry: core/platform.c's row for it. */
#define USP_COMPONENT "url_search_params"

static JSClassID g_usp_class;
static JSRuntime *g_usp_rt;
static int       g_usp_ctor_stepid = -1;
static int       g_usp_pair_handle = -1;

/* ---- the object ------------------------------------------------------------------------------------------ */

/* NEITHER OF THESE MAY LOOK THE CLASS ID UP, because the collector runs AFTER core/platform.h's release
   column — every host's teardown is platform_agent_free(), JS_RunGC, JS_FreeRuntime in that order — and
   §6.2's class id is agent state the `url_search_params` row's release now puts back at 0. The two fail
   differently on a NULL: the finalizer would leak the list and every name/value pair in it, and the MARK
   would leave the cycle below unseen, so the URL each object holds keeps the internal reference gc_decref
   subtracts and gc_scan reads it as rooted from OUTSIDE the heap. The id is not needed — the collector
   dispatched here THROUGH the class. See core/agent_state.h's closing paragraph. */
static void usp_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    UspObj *u = JS_GetAnyOpaque(val, &id);

    (void)id;
    if (u) {
        JS_FreeValueRT(rt, u->list_box);
        JS_FreeValueRT(rt, u->vals);
        JS_FreeValueRT(rt, u->owner);
        JS_FreeValueRT(rt, u->unknown);
        js_free_rt(rt, u);
    }
}

static void usp_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID id = 0;
    UspObj *u = JS_GetAnyOpaque(val, &id);

    (void)id;
    /* The URL holds this object and this object holds the URL — a real cycle, which is what gc_mark is for. */
    if (u) {
        JS_MarkValue(rt, u->list_box, mark_func);
        JS_MarkValue(rt, u->vals, mark_func);
        JS_MarkValue(rt, u->owner, mark_func);
        JS_MarkValue(rt, u->unknown, mark_func);
    }
}

/* The box's release: the list and the block that held it. */
static void usp_list_box_free(JSRuntime *rt, void *opaque, void *ptr)
{
    (void)rt; (void)opaque;
    usp_list_free(ptr);
    free(ptr);
}

/* A GC-owned payload for one list (an ArrayBuffer no page can reach, whose release is usp_list_box_free),
   taking `l`, which is malloc'd. Owned. */
static JSValue usp_list_box(JSContext *ctx, UspList *l)
{
    JSValue b = JS_NewArrayBuffer(ctx, (uint8_t *)l, sizeof *l, usp_list_box_free, NULL, false);

    CHECK(!JS_IsException(b), "urlsearchparams: OOM boxing a list");
    return b;
}

/* The record's accessor and therefore its capture point: a flow that reaches the record may write it. */
static UspObj *usp_rec(JSValueConst v)
{
    UspObj *u = g_usp_class ? JS_GetOpaque(v, g_usp_class) : NULL;

    if (u) cow_capture_host_record(v, u, &USP_REC);
    return u;
}

/* How many tuples the §6.2 list `vals` holds. */
static int usp_vals_n(JSContext *ctx, JSValueConst vals)
{
    int64_t n = 0;

    CHECK(JS_GetLength(ctx, vals, &n) == 0, "urlsearchparams: a list's length could not be read");
    DCHECK(n % 2 == 0 && n / 2 <= INT32_MAX, "a §6.2 list holds a name with no value");
    return (int)(n / 2);
}

/* Half `h` (0 the name, 1 the value) of tuple `i` of `vals`: a String, or the unknown that half is. Owned. */
static JSValue usp_half(JSContext *ctx, JSValueConst vals, int i, int h)
{
    JSValue v = JS_GetPropertyUint32(ctx, vals, 2 * (uint32_t)i + (uint32_t)h);

    DCHECK(JS_IsString(v) || concolic_is(v),
           "a §6.2 list half is neither a USVString nor unknown input — every writer converts it first");
    return v;
}

/* Append the tuple (`name`, `value`) (both borrowed) to the Array `vals` this write is building. */
static void usp_vals_push(JSContext *ctx, JSValueConst vals, JSValueConst name, JSValueConst value)
{
    uint32_t at = 2 * (uint32_t)usp_vals_n(ctx, vals);

    CHECK(JS_DefinePropertyValueUint32(ctx, vals, at, JS_DupValue(ctx, name), JS_PROP_C_W_E) >= 0 &&
          JS_DefinePropertyValueUint32(ctx, vals, at + 1, JS_DupValue(ctx, value), JS_PROP_C_W_E) >= 0,
          "urlsearchparams: OOM holding a tuple");
}

static JSValue usp_vals_new(JSContext *ctx)
{
    JSValue a = JS_NewArray(ctx);

    CHECK(!JS_IsException(a), "urlsearchparams: OOM holding a list");
    return a;
}

/* The §6.2 list of the §5.1 parse `l`, whose halves are all bytes. Owned. */
static JSValue usp_vals_of_list(JSContext *ctx, const UspList *l)
{
    JSValue vals = usp_vals_new(ctx);
    int i;

    for (i = 0; i < l->n; i++) {
        JSValue n, v;

        DCHECK(!l->e[i].nhole && !l->e[i].vhole, "a parse of bytes produced a half that is a hole");
        n = JS_NewStringLen(ctx, l->e[i].name, l->e[i].nlen);
        v = JS_NewStringLen(ctx, l->e[i].value, l->e[i].vlen);
        CHECK(!JS_IsException(n) && !JS_IsException(v), "urlsearchparams: OOM holding a parsed tuple");
        usp_vals_push(ctx, vals, n, v);
        JS_FreeValue(ctx, n);
        JS_FreeValue(ctx, v);
    }
    return vals;
}

/* §5.1's byte list of the §6.2 list `vals` (borrowed): each String half's bytes, and an unknown half's display
   shape with its hole bit, which §5.2's serializer copies verbatim (see UrlEncodedPair). Malloc'd, owned. */
static UspList *usp_project(JSContext *ctx, JSValueConst vals)
{
    UspList *l = lexbor_calloc(1, sizeof *l);
    int i, h, n = usp_vals_n(ctx, vals);

    CHECK(l != NULL, "urlsearchparams: OOM projecting a list");
    for (i = 0; i < n; i++) {
        const char *b[2];
        size_t bl[2] = { 0, 0 };
        int hole[2];

        for (h = 0; h < 2; h++) {
            JSValue v = usp_half(ctx, vals, i, h);

            hole[h] = concolic_is(v);
            b[h] = hole[h] ? concolic_name_cstr(ctx, v) : JS_ToCStringLen(ctx, &bl[h], v);
            CHECK(b[h] != NULL, "urlsearchparams: OOM projecting a tuple's bytes");
            if (hole[h]) bl[h] = strlen(b[h]);
            JS_FreeValue(ctx, v);
        }
        usp_list_append(l, b[0], bl[0], hole[0], b[1], bl[1], hole[1]);
        JS_FreeCString(ctx, b[0]);
        JS_FreeCString(ctx, b[1]);
    }
    return l;
}

/* Make `vals` (taken) the record's list, with its projection, through the record's writes; the caller's site
   travels to the write's checks. */
static void usp_commit_at(JSContext *ctx, UspObj *u, JSValue vals, const char *file, int line)
{
    UspList *l = usp_project(ctx, vals);

    cow_record_set_at(ctx, u, &USP_REC, &u->vals, vals, file, line);
    cow_record_set_at(ctx, u, &USP_REC, &u->list_box, usp_list_box(ctx, l), file, line);
    u->list = l;
}
#define usp_commit(ctx_, u_, vals_) usp_commit_at((ctx_), (u_), (vals_), __FILE__, __LINE__)

const UrlEncodedList *usp_list_of(JSValueConst v)
{
    UspObj *u = usp_rec(v);
    return u ? u->list : NULL;
}

/* Declared below beside the reads it serves. */
static JSValue usp_derive(JSContext *ctx, UspObj *u, const char *op, JSValue r, int argc, JSValueConst *argv);

JSValue usp_unknown_serialization(JSContext *ctx, JSValueConst v)
{
    UspObj *u = usp_rec(v);
    char *q;

    DCHECK(u != NULL, "a body asked for the serialization of something that is not a URLSearchParams");
    if (JS_IsUndefined(u->unknown)) return JS_UNDEFINED;
    q = usp_serialize(u->list, NULL);
    {
        JSValue r = JS_NewString(ctx, q);
        free(q);
        return usp_derive(ctx, u, "URLSearchParams.toString", r, 0, NULL);
    }
}

static UspObj *usp_of(JSContext *ctx, JSValueConst v)
{
    UspObj *u = usp_rec(v);
    if (!u) JS_ThrowTypeError(ctx, "not a URLSearchParams");
    return u;
}

/* §6.2 URLSearchParams class's UPDATE STEPS: serialize the list onto the associated URL's query, and set
   that query to NULL rather than to the empty string when the list is empty — `u.searchParams.delete("a")`
   on `?a=1` gives `u.href` with no `?` at all, which is exactly the null/empty distinction the record keeps. */
static void usp_update(JSContext *ctx, UspObj *u)
{
    size_t n = 0;
    char *q;

    if (JS_IsUndefined(u->owner)) return;
    q = usp_serialize(u->list, &n);
    if (!n) { free(q); q = NULL; }
    url_query_assign(ctx, u->owner, q);
}

/* A URLSearchParams over the §6.2 list `vals` and `unknown` (both taken), initialised before the record is
   reachable. */
static JSValue usp_new_with(JSContext *ctx, JSValueConst owner, JSValue vals, JSValue unknown, int unknown_example)
{
    UspObj *u;
    JSValue obj;
    UspList *l;

    DCHECK(g_usp_class != 0, "a URLSearchParams was built before the class existed");
    DCHECK(JS_IsUndefined(unknown) || concolic_is(unknown),
           "a URLSearchParams list was built over an `unknown` that is neither absent nor unknown input — every "
           "read and mutation derives from that field whenever it is not undefined, so a concrete value there "
           "asks for a derivation over nothing");
    {
        JSValue proto = JS_GetClassProto(ctx, g_usp_class);
        DCHECK(!JS_IsNull(proto), "a URLSearchParams was minted in a realm that never ran its install");
        obj = JS_NewObjectProtoClass(ctx, proto, g_usp_class);
        JS_FreeValue(ctx, proto);
    }
    u = JS_IsException(obj) ? NULL : js_mallocz(ctx, sizeof(*u));
    if (!u) { JS_FreeValue(ctx, obj); JS_FreeValue(ctx, unknown); JS_FreeValue(ctx, vals); return JS_EXCEPTION; }
    l = usp_project(ctx, vals);
    u->owner = JS_DupValue(ctx, owner);
    u->unknown = unknown;
    u->unknown_example = unknown_example;
    u->vals = vals;
    u->list = l;
    u->list_box = usp_list_box(ctx, l);
    JS_SetOpaque(obj, u);
    return obj;
}

/* The §6.2 list §5.1's parse of `query` (`len` bytes, or NULL for none) gives. Owned. */
static JSValue usp_vals_parse(JSContext *ctx, const char *query, size_t len)
{
    UspList *l = lexbor_calloc(1, sizeof *l);
    JSValue vals;

    CHECK(l != NULL, "urlsearchparams: OOM building a list");
    if (query) usp_parse(l, query, len);
    vals = usp_vals_of_list(ctx, l);
    usp_list_box_free(NULL, NULL, l);
    return vals;
}

JSValue usp_new(JSContext *ctx, JSValueConst owner, const char *query, size_t query_len)
{
    return usp_new_with(ctx, owner, usp_vals_parse(ctx, query, query_len), JS_UNDEFINED, 0);
}

void usp_reset(JSContext *ctx, JSValueConst usp, const char *query, size_t query_len)
{
    UspObj *u = usp_rec(usp);

    DCHECK(u != NULL, "a URL's search setter re-initialised something that is not a URLSearchParams");
    usp_commit(ctx, u, usp_vals_parse(ctx, query, query_len));
}

/* ---- §6.2's members --------------------------------------------------------------------------------------- */

enum { USP_APPEND = 0, USP_DELETE, USP_GET, USP_GETALL, USP_HAS, USP_SET, USP_SORT, USP_TOSTRING,
       USP_MEMBER_N };
/* THE AGENT'S POOL ENTRIES, one per §6.2 operation — the OBJECTS they are installed as are each realm's. */
static int g_usp_id[USP_MEMBER_N];

/* What an unknown half cannot do yet, said once. §6.2's list holds an unknown USVString as itself (UspObj's
   `vals`), so a read hands it back and §5.2 serializes its shape verbatim. "A tuple whose name is name" is an
   equality, though, and over an unknown both outcomes are worlds: get and has fork it (usp_lookup_step), but
   the other members are not step machines, and comparing the shape's bytes would decide it for the flow. */
#define USP_HOLE_WHY(what)                                                                                 \
    "a URLSearchParams " what " — an equality over unknown input has a world for each outcome and this "   \
    "member cannot fork. Build it as a step machine that forks the comparison as usp_lookup_step does"

/* A read of a list parsed from an unknown string, answered as the unknown that read derives — named by the
   operation, the string and the read's own arguments — with the read over the example's parse as its example
   where the string had one. The answer `r` is consumed. */
static JSValue usp_derive(JSContext *ctx, UspObj *u, const char *op, JSValue r, int argc, JSValueConst *argv)
{
    JSValueConst ops[3];
    int n = 1, i;
    JSValue d;

    ops[0] = u->unknown;
    for (i = 0; i < argc && n < 3; i++) if (!JS_IsUndefined(argv[i])) ops[n++] = argv[i];
    if (!u->unknown_example) { JS_FreeValue(ctx, r); r = JS_UNDEFINED; }
    d = concolic_new_derived(ctx, op, ops, n, r);
    DCHECK(!JS_IsUninitialized(d), "a URLSearchParams list parsed from an unknown derived no unknown read");
    return d;
}

/* §6.2's mutations over a list parsed from an unknown string: the real operation has run on the example's parse,
   and the list becomes the unknown that operation derives — named by the operation, the previous list and the
   arguments — with the updated example list's serialization as its example. Every later read derives from it,
   so a read after a mutation is about the mutated list. The list has no associated URL (only the constructor's
   string arm sets `unknown`, and that arm makes none), so §6.2's update steps have nothing to write. */
static void usp_rederive(JSContext *ctx, UspObj *u, const char *op, int argc, JSValueConst *argv)
{
    JSValueConst ops[3];
    JSValue ex = JS_UNDEFINED, d;
    int n = 1, i;

    DCHECK(JS_IsUndefined(u->owner),
           "a URLSearchParams list derived from unknown input has an associated URL — only the constructor's "
           "string arm sets `unknown`, and it creates no URL object, so §6.2's update steps would write the "
           "example's serialization onto a URL as if it were the input");
    ops[0] = u->unknown;
    for (i = 0; i < argc && n < 3; i++) if (!JS_IsUndefined(argv[i])) ops[n++] = argv[i];
    if (u->unknown_example) {
        char *q = usp_serialize(u->list, NULL);
        ex = JS_NewString(ctx, q);
        free(q);
    }
    d = concolic_new_derived(ctx, op, ops, n, ex);
    DCHECK(concolic_is(d),
           "a mutation of a URLSearchParams list parsed from an unknown derived no unknown list — the field it "
           "writes is the list's unknown, so a concrete answer here (a candidate delivered at the list's source) "
           "needs the list re-parsed from the delivered value and made concrete; build that rather than store it");
    cow_record_set(ctx, u, &USP_REC, &u->unknown, d);
    u->mutated = 1;   /* a POD field of the record usp_rec captured */
}

/* Whether half `h` of tuple `i` of `vals` is `want`, both Strings: §6.2's comparisons are of code units. */
static bool usp_half_is(JSContext *ctx, JSValueConst vals, int i, int h, JSValueConst want)
{
    JSValue v = usp_half(ctx, vals, i, h);
    bool eq = JS_IsStrictEqual(ctx, v, want);

    JS_FreeValue(ctx, v);
    return eq;
}

/* Whether any half of the projected list `l` is unknown input. */
static bool usp_any_hole(const UspList *l)
{
    int i;

    for (i = 0; i < l->n; i++)
        if (l->e[i].nhole || l->e[i].vhole) return true;
    return false;
}

/* §5.2's serialization of `vals` run on its halves' examples: each unknown half's example in its place, or
   JS_UNDEFINED where one has none. Owned. */
static JSValue usp_example_serialization(JSContext *ctx, JSValueConst vals)
{
    JSValue ex = usp_vals_new(ctx), r = JS_UNDEFINED;
    int i, h, n = usp_vals_n(ctx, vals);
    bool all = true;

    for (i = 0; i < n && all; i++) {
        JSValue half[2];

        for (h = 0; h < 2; h++) {
            JSValue v = usp_half(ctx, vals, i, h);

            half[h] = concolic_is(v) ? concolic_example_string(ctx, v) : JS_DupValue(ctx, v);
            JS_FreeValue(ctx, v);
            all = all && !JS_IsUndefined(half[h]);
        }
        if (all) usp_vals_push(ctx, ex, half[0], half[1]);
        JS_FreeValue(ctx, half[0]);
        JS_FreeValue(ctx, half[1]);
    }
    if (all) {
        UspList *l = usp_project(ctx, ex);
        char *q = usp_serialize(l, NULL);

        r = JS_NewString(ctx, q);
        free(q);
        usp_list_box_free(NULL, NULL, l);
    }
    JS_FreeValue(ctx, ex);
    return r;
}

/* §6.2's stringification of a list holding unknown halves: §5.2's serialization is a function of every half,
   so it is the unknown they derive, with the serialization run on their examples as its example. `r` (the
   projection's serialization, consumed) is the answer when no half is unknown. */
static JSValue usp_derive_halves(JSContext *ctx, UspObj *u, const char *op, JSValue r)
{
    JSValue d;

    if (!usp_any_hole(u->list)) return r;
    JS_FreeValue(ctx, r);
    d = concolic_new_derived_list(ctx, op, u->vals, usp_example_serialization(ctx, u->vals));
    DCHECK(concolic_is(d), "a URLSearchParams list holding an unknown half derived no unknown serialization");
    return d;
}

static JSValue js_usp_member(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    UspObj *u = usp_of(ctx, this_val);
    JSValueConst name = argc > 0 ? argv[0] : JS_UNDEFINED, value = JS_UNDEFINED;
    JSValue vals = JS_UNDEFINED, r = JS_UNDEFINED;
    bool with_value, found = false;
    int i, n;

    if (!u) return JS_EXCEPTION;
    n = usp_vals_n(ctx, u->vals);
    if (magic == USP_TOSTRING) {
        char *s = usp_serialize(u->list, NULL);
        r = JS_NewString(ctx, s);
        free(s);
        CHECK(!JS_IsException(r), "urlsearchparams: OOM serializing a list");
        return JS_IsUndefined(u->unknown) ? usp_derive_halves(ctx, u, "URLSearchParams.toString", r)
                                          : usp_derive(ctx, u, "URLSearchParams.toString", r, 0, NULL);
    }
    if (magic == USP_SORT) {
        /* §6.2 sort(): by the NAMES' code units, and STABLE — the relative order of equal names is preserved,
           which is why this is an insertion sort and not qsort. It orders the projection's tuples, which are
           the list's in the same positions. */
        int *ord = malloc(sizeof *ord * (size_t)(n ? n : 1)), j, k;

        CHECK(ord != NULL, "urlsearchparams: OOM ordering a list");
        for (i = 0; i < n; i++) {
            DCHECK(!u->list->e[i].nhole, USP_HOLE_WHY("sort() ordered a name that is unknown"));
            ord[i] = i;
        }
        for (i = 1; i < n; i++) {
            k = ord[i];
            for (j = i - 1; j >= 0 && usp_name_cmp(&u->list->e[ord[j]], &u->list->e[k]) > 0; j--)
                ord[j + 1] = ord[j];
            ord[j + 1] = k;
        }
        vals = usp_vals_new(ctx);
        for (i = 0; i < n; i++) {
            JSValue a = usp_half(ctx, u->vals, ord[i], 0), b = usp_half(ctx, u->vals, ord[i], 1);
            usp_vals_push(ctx, vals, a, b);
            JS_FreeValue(ctx, a);
            JS_FreeValue(ctx, b);
        }
        free(ord);
        usp_commit(ctx, u, vals);
        usp_update(ctx, u);
        if (!JS_IsUndefined(u->unknown)) usp_rederive(ctx, u, "URLSearchParams.sort", 0, NULL);
        return JS_UNDEFINED;
    }

    /* §6.2's members take USVStrings, and the Web IDL declaration passes UNKNOWN external input through as
       itself (§7.1.19 ToString over an unknown is the identity), so `append` and `set` store an unknown half as
       itself — `vscode.dev` sets `navigator.language.toLowerCase()` this way one line before it assigns the
       URL to `location.href` — and §6.2's update steps serialize its shape. §6.2: `delete` takes an OPTIONAL
       value, and its presence changes what it matches, so undefined must reach here as undefined. */
    with_value = magic == USP_APPEND || magic == USP_SET ||
                 (magic == USP_DELETE && argc > 1 && !JS_IsUndefined(argv[1]));
    if (with_value) value = argv[1];
    DCHECK((JS_IsString(name) || concolic_is(name)) && (!with_value || JS_IsString(value) || concolic_is(value)),
           "a URLSearchParams member was handed an argument its USVString declaration did not convert");
    /* The equality barrier (USP_HOLE_WHY): `set` compares names to find the tuple to overwrite, so only its
       VALUE may be unknown; `delete` compares both. */
    DCHECK(!concolic_is(name) || magic == USP_APPEND,
           USP_HOLE_WHY("member matched an unknown NAME against the stored names"));
    DCHECK(!(magic == USP_DELETE && with_value && concolic_is(value)),
           USP_HOLE_WHY("delete() matched an unknown VALUE against the stored values"));

    if (magic == USP_GETALL) {
        uint32_t k = 0;

        r = usp_vals_new(ctx);
        for (i = 0; i < n; i++) {
            DCHECK(!u->list->e[i].nhole, USP_HOLE_WHY("getAll() matched a stored NAME that is unknown"));
            if (!usp_half_is(ctx, u->vals, i, 0, name)) continue;
            CHECK(JS_DefinePropertyValueUint32(ctx, r, k++, usp_half(ctx, u->vals, i, 1), JS_PROP_C_W_E) >= 0,
                  "urlsearchparams: OOM answering getAll()");
        }
        return JS_IsUndefined(u->unknown) ? r : usp_derive(ctx, u, "URLSearchParams.getAll", r, argc, argv);
    }

    /* Every write builds a new list (see UspObj): the tuples this member keeps, in order. */
    vals = usp_vals_new(ctx);
    for (i = 0; i < n; i++) {
        JSValue a = usp_half(ctx, u->vals, i, 0), b = usp_half(ctx, u->vals, i, 1);
        bool match = false;

        if (magic != USP_APPEND) {
            DCHECK(!concolic_is(a), USP_HOLE_WHY("delete() or set() matched a stored NAME that is unknown"));
            DCHECK(!(magic == USP_DELETE && with_value && concolic_is(b)),
                   USP_HOLE_WHY("delete() matched a stored VALUE that is unknown"));
            match = JS_IsStrictEqual(ctx, a, name) &&
                    (magic != USP_DELETE || !with_value || JS_IsStrictEqual(ctx, b, value));
        }
        /* §6.2 set(): the FIRST tuple with this name keeps its position and takes the new value, and every
           other tuple with that name is removed; delete() removes every match. */
        if (!match) usp_vals_push(ctx, vals, a, b);
        else if (magic == USP_SET && !found) usp_vals_push(ctx, vals, a, value);
        found = found || match;
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
    }
    if (magic == USP_APPEND || (magic == USP_SET && !found)) usp_vals_push(ctx, vals, name, value);
    usp_commit(ctx, u, vals);
    usp_update(ctx, u);
    if (!JS_IsUndefined(u->unknown))
        usp_rederive(ctx, u, magic == USP_APPEND ? "URLSearchParams.append" : magic == USP_DELETE
                                 ? "URLSearchParams.delete" : "URLSearchParams.set", argc, argv);
    return JS_UNDEFINED;
}

/* The first tuple of `l` whose name is the `nn` bytes at `name` and, where `value` is non-NULL, whose value is
   the `vn` bytes at `value`; -1 when there is none. */
static int usp_find(const UspList *l, const char *name, size_t nn, const char *value, size_t vn)
{
    int i;

    for (i = 0; i < l->n; i++) {
        DCHECK(!l->e[i].nhole, USP_HOLE_WHY("get()/has() over an unknown string's list compared a stored NAME "
                                            "that is unknown by its shape's bytes"));
        if (l->e[i].nlen != nn || memcmp(l->e[i].name, name, nn)) continue;
        DCHECK(!value || !l->e[i].vhole,
               USP_HOLE_WHY("has() over an unknown string's list compared a stored VALUE that is unknown by its "
                            "shape's bytes"));
        if (value && (l->e[i].vlen != vn || memcmp(l->e[i].value, value, vn))) continue;
        return i;
    }
    return -1;
}

/* URL §5.2 application/x-www-form-urlencoded serializing of one tuple value: the urlencoded percent-encode set.
   The delivery of a candidate over a query parameter runs it (concolic_set_tuple_serializer). Owned. */
static char *usp_serialize_value(const char *s, size_t n)
{
    return url_percent_encode(s, n, URL_SET_URLENCODED);
}

/* URL §6.2's get(name) and has(name, value) over a list parsed from an unknown string ask one question of the
   string: whether its list holds a tuple whose name is `name`. It is asked through the name chain, keyed by the
   name spelled in §1.3's urlencoded percent-encode set (injective, so a name holding U+0000 keys apart). */
#define USP_TUPLE_PREDICATE "URL §6.2 URLSearchParams class (this's list holds a tuple whose name is"
#define USP_LOOKUP_ALGORITHM "URL §6.2 URLSearchParams class the get(name) and has(name, value) method steps"
#define UL_STAGES(X)                                                                                          \
    X(UL_LOOKUP, "URL §6.2 URLSearchParams class the get(name) and has(name, value) method steps (a tuple "    \
                 "whose name is name in this's list)")
enum { IDL_STEP_STAGE_BASE(UL_STAGES) UL_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const UL_STEPS[] = { UL_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* The ask's name at step_tobool_run; the fork's key is the equality's own identity. */
#define USP_EQUALS_OP "URL §6.2 a tuple whose name is name (and value is value) in this's list"

typedef struct {
    IdlNameChainSuppliedKey key;
    JSValue pred;    /* the equality the walk is asking, held across its fork while `asking` */
    int     i;       /* the tuple the walk stands on */
    uint8_t half;    /* the half of it being compared: 0 its name, 1 its value (has(name, value)) */
    uint8_t asking;
} UspLookupState;

static void usp_lookup_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    UspLookupState *s = st;

    idl_name_chain_supplied_visit(ctx, &s->key, v);
    v->val(ctx, &s->pred);
}

/* URL §6.2's get(name) and has(name, value) over a list of USVStrings: the first tuple whose name is `name`
   and, given `value`, whose value is `value`. An unknown on either side of a comparison makes it a question
   with a world per outcome, asked as §7.2.14 IsStrictlyEqual's own predicate (concolic_strict_equal), so the
   holding arm pins the unknown and the other walks on. get's found arm answers the tuple's value itself, which
   is that unknown where it is one. */
static int usp_lookup_walk(JSContext *ctx, JSStepHdr *hdr, UspLookupState *s, UspObj *u, int magic,
                           JSValueConst name, JSValueConst value, bool with_value, JSValue *presult)
{
    int n = usp_vals_n(ctx, u->vals), want = with_value ? 2 : 1;

    for (; s->i < n; s->i++, s->half = 0) {
        while (s->half < want) {
            int arm, r;

            if (!s->asking) {
                JSValue v = usp_half(ctx, u->vals, s->i, s->half);

                s->pred = concolic_strict_equal(ctx, v, s->half ? value : name);
                s->asking = 1;
                JS_FreeValue(ctx, v);
            }
            r = step_tobool_run(ctx, hdr, s->pred, USP_EQUALS_OP, &arm);
            if (r) return r;
            JS_FreeValue(ctx, s->pred);
            s->pred = JS_UNDEFINED;
            s->asking = 0;
            if (!arm) break;
            s->half++;
        }
        if (s->half == want) {
            *presult = magic == USP_HAS ? JS_TRUE : usp_half(ctx, u->vals, s->i, 1);
            return JS_STEP_DONE;
        }
    }
    *presult = magic == USP_HAS ? JS_FALSE : JS_NULL;
    return JS_STEP_DONE;
}

/* Over an unknown list, get's found arm answers the tuple's value as an unknown String
   (concolic_new_urlencoded_value), with the example's value as its example where the example holds the tuple;
   its other arm is null. has(name) answers the same question as a boolean. has(name, value) over an unknown
   list derives, as getAll does. */
static int usp_lookup_answer(JSContext *ctx, JSStepHdr *hdr, UspLookupState *s, UspObj *u, int magic,
                             int argc, JSValueConst *argv, const char *name, size_t nn, const char *value,
                             size_t vn, JSValue *presult)
{
    int i = usp_find(u->list, name, nn, value, vn), real, rc;
    JSValue ex = JS_UNDEFINED;
    bool yes = false;
    char *member;

    DCHECK(!JS_IsUndefined(u->unknown), "the name chain was asked of a list derived from no unknown string");
    if (value) {
        *presult = usp_derive(ctx, u, "URLSearchParams.has", JS_NewBool(ctx, i >= 0), argc, argv);
        return JS_STEP_DONE;
    }
    real = u->unknown_example ? (i >= 0) : JS_OUTCOME_REAL_UNSTATED;
    member = url_percent_encode(name, nn, URL_SET_URLENCODED);
    CHECK(member != NULL, "URLSearchParams: OOM spelling a name for its constraint key");
    rc = idl_name_chain_ask_supplied(ctx, hdr, &s->key, u->unknown, USP_TUPLE_PREDICATE, member, real,
                                     USP_LOOKUP_ALGORITHM, &yes);
    if (rc || magic == USP_HAS || !yes) {
        free(member);
        if (rc) return rc;
        *presult = magic == USP_HAS ? JS_NewBool(ctx, yes) : JS_NULL;
        return JS_STEP_DONE;
    }
    if (u->unknown_example && i >= 0 && !u->list->e[i].vhole)   /* a hole's bytes are a shape, not an example */
        ex = JS_NewStringLen(ctx, u->list->e[i].value, u->list->e[i].vlen);
    CHECK(!JS_IsException(ex), "URLSearchParams: OOM taking a tuple's example value");
    if (u->mutated) {
        JSValueConst ops[2] = { u->unknown, argv[0] };
        *presult = concolic_new_derived(ctx, "URLSearchParams.get", ops, 2, ex);
    } else {
        *presult = concolic_new_urlencoded_value(ctx, "URLSearchParams.get", u->unknown, argv[0], member, ex);
    }
    free(member);
    concolic_state_string(*presult);   /* §6.2: the found arm's value is a USVString */
    return JS_STEP_DONE;
}

static int usp_lookup_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                           JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    int magic = idl_step_magic(hdr), rc;
    UspObj *u;
    const char *name, *value = NULL;
    size_t nn = 0, vn = 0;
    bool with_value;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);   /* this machine makes no request that delivers a value */
    *presult = JS_UNDEFINED;
    DCHECK(hdr->stage == UL_LOOKUP, "URL §6.2's get/has resumed into a stage its one step does not have");
    DCHECK(magic == USP_GET || magic == USP_HAS, "the get/has machine was declared for another §6.2 member");
    u = usp_of(ctx, hdr->this_val);
    if (!u) return JS_STEP_ABRUPT;
    with_value = magic == USP_HAS && idl_arg_given(argc, argv, 1);
    if (JS_IsUndefined(u->unknown))
        return usp_lookup_walk(ctx, hdr, st, u, magic, argv[0], with_value ? argv[1] : JS_UNDEFINED, with_value,
                               presult);
    /* The name chain keys its question by the name's bytes, so over a list parsed from an unknown string an
       unknown argument has no key to ask under. */
    DCHECK(!concolic_is(argv[0]), "URLSearchParams get()/has() asked an unknown string's list about an unknown "
                                  "NAME, which the name chain cannot key — build a chain keyed by the "
                                  "argument's identity");
    DCHECK(!with_value || !concolic_is(argv[1]),
           "URLSearchParams has(name, value) compared an unknown VALUE with an unknown string's list, which its "
           "derivation names by bytes — build the derivation over the argument's identity");
    name = JS_ToCStringLen(ctx, &nn, argv[0]);
    if (!name) return JS_STEP_ABRUPT;
    if (with_value) {
        value = JS_ToCStringLen(ctx, &vn, argv[1]);
        if (!value) { JS_FreeCString(ctx, name); return JS_STEP_ABRUPT; }
    }
    rc = usp_lookup_answer(ctx, hdr, st, u, magic, argc, argv, name, nn, value, vn, presult);
    JS_FreeCString(ctx, name);
    if (value) JS_FreeCString(ctx, value);
    return rc;
}

static const IdlStepDecl USP_LOOKUP_DECL = {
    usp_lookup_step, sizeof(UspLookupState), usp_lookup_visit, NULL, USP_LOOKUP_ALGORITHM, UL_STEPS
};

static JSValue js_usp_get_size(JSContext *ctx, JSValueConst this_val, int magic)
{
    UspObj *u = usp_of(ctx, this_val);
    (void)magic;
    if (!u) return JS_EXCEPTION;
    if (!JS_IsUndefined(u->unknown))
        return usp_derive(ctx, u, "URLSearchParams.size", JS_NewInt32(ctx, u->list->n), 0, NULL);
    return JS_NewInt32(ctx, u->list->n);
}

/* The two operations §3.7.9's binding needs of an `iterable<K, V>` interface — the count and the i-th of
   §2.5.9's "value pairs to iterate over" — over the pair list as it stands right now. */
static int usp_pair_count(JSContext *ctx, JSValueConst target)
{
    UspObj *u = usp_rec(target);
    (void)ctx;
    return u ? u->list->n : -1;
}

/* The unknown the list derives from, and how many pairs its example's parse holds (-1 without an example). */
static JSValueConst usp_pair_unknown(JSContext *ctx, JSValueConst target, int *example_n)
{
    UspObj *u = usp_rec(target);
    (void)ctx;
    DCHECK(u != NULL, "a URLSearchParams pair iterator asked its target's unknown of something that is not one");
    *example_n = u->unknown_example ? u->list->n : -1;
    return u->unknown;
}

/* One half of the i-th pair of a list derived from unknown input: a derivation over that unknown and the
   index, whose example is the example parse's half where that pair exists and is not itself a hole. */
static JSValue usp_unknown_half(JSContext *ctx, UspObj *u, const char *op, int i, int name_half)
{
    JSValueConst ops[2];
    JSValue idx = JS_NewInt32(ctx, i), ex = JS_UNDEFINED, d;

    if (u->unknown_example && i < u->list->n) {
        const UspPair *e = &u->list->e[i];
        if (name_half ? !e->nhole : !e->vhole)
            ex = name_half ? JS_NewStringLen(ctx, e->name, e->nlen) : JS_NewStringLen(ctx, e->value, e->vlen);
    }
    ops[0] = u->unknown;
    ops[1] = idx;
    d = concolic_new_derived(ctx, op, ops, 2, ex);
    DCHECK(!JS_IsUninitialized(d), "a URLSearchParams list parsed from an unknown derived no unknown pair");
    return d;
}

/* §6.2's value pairs to iterate over for a list derived from unknown input: the i-th tuple's name and value. */
static void usp_pair_of_unknown(JSContext *ctx, JSValueConst target, int i, JSValue *key, JSValue *value)
{
    UspObj *u = usp_rec(target);

    DCHECK(u != NULL && !JS_IsUndefined(u->unknown) && i >= 0,
           "a URLSearchParams pair over unknown input was asked of a list that is not derived from one");
    *key = usp_unknown_half(ctx, u, "URLSearchParams.pairName", i, 1);
    *value = usp_unknown_half(ctx, u, "URLSearchParams.pairValue", i, 0);
}

/* §6.2's value pairs to iterate over: the i-th tuple's halves, an unknown one as itself. */
static void usp_pair_at(JSContext *ctx, JSValueConst target, int i, JSValue *key, JSValue *value)
{
    UspObj *u = usp_rec(target);

    DCHECK(u != NULL && i < u->list->n, "a URLSearchParams pair was asked for past the end of the list");
    *key = usp_half(ctx, u->vals, i, 0);
    *value = usp_half(ctx, u->vals, i, 1);
}

static const IdlPairIterOps USP_PAIR_OPS = { usp_pair_count, usp_pair_at, "URLSearchParams", false, false,
                                             usp_pair_unknown, usp_pair_of_unknown };

/* ---- §6.2's constructor ------------------------------------------------------------------------------------
 *
 * A MACHINE, because its argument is a union whose two object arms are the page's code from end to end: the
 * sequence arm drives an ES iterator and the record arm is [[OwnPropertyKeys]] plus a descriptor and a [[Get]]
 * per key. Both cursors are Web IDL's shared ones. */
/* WHERE THIS MACHINE RESTS, AS §6.2 NUMBERS IT. The constructor is two steps and the second is "initialize
   this with init", whose three arms are the union's — and every one of the object arms' reads is the page's
   code, which is what the stages between them are. */
#define UC_STAGES(X) \
    X(UC_START, \
      "URL §6.2 new URLSearchParams(init) steps 1-2 (the leading `?` stripped from a string init, then " \
      "initialize's step 3 for that arm; Web IDL §3.7.1's `new` requirement precedes them)") \
    X(UC_UNKNOWN_ARM, \
      "Web IDL §3.2.25 over unknown input (which of the three arms the init's type facet admits, forked)") \
    X(UC_USEQ_LEN, "URL §6.2 initialize step 1 over an unknown sequence (§7.3.18 LengthOfArrayLike)") \
    X(UC_USEQ_PAIR, "URL §6.2 initialize step 1 over an unknown sequence (Get of the next innerSequence)") \
    X(UC_USEQ_PLEN, "URL §6.2 initialize step 1.1 over an unknown innerSequence (its size, which must be 2)") \
    X(UC_USEQ_ITEM, "URL §6.2 initialize step 1.2 over an unknown innerSequence (Get of its two items)") \
    X(UC_ITER_ASKED, \
      "Web IDL §3.2.25 (Get(init, @@iterator) — the read that picks the sequence arm of " \
      "`sequence<sequence<USVString>> or record<USVString, USVString> or USVString` over the record one)") \
    X(UC_SEQ_PAIR, "URL §6.2 initialize step 1 (the next innerSequence of the sequence arm)") \
    X(UC_SEQ_ITEM, "URL §6.2 initialize step 1.1 (that innerSequence's items — a size other than 2 is a " \
                   "TypeError, which is why the walk runs one step past the second)") \
    X(UC_KEY_PAIR, "URL §6.2 initialize step 2 (the next name → value of the record arm: Web IDL §3.2.23's " \
                   "[[OwnPropertyKeys]], descriptor and [[Get]] per key)") \
    X(UC_PAIR_NAME_STR, "Web IDL §3.2.12 (converting the pair's NAME to a USVString — the page's `toString`)") \
    X(UC_PAIR_VALUE_STR, "Web IDL §3.2.12 (converting the pair's VALUE to a USVString)") \
    X(UC_DONE, "URL §6.2 initialize steps 1.2/2/3.2 (the pair is appended to this's list, or the string arm " \
               "has been parsed)")
/* THE STAGE'S NAME IS THE WHOLE OF WHAT THE X-LIST'S FIRST SLOT MAY BE, and this list wrote `UC_START =
   IDL_STEP_FIRST` there. That spelling is what IDL_STEP_STAGE_BASE exists to replace — the offset stated once
   for the list rather than once per member — and it is also unusable by the third expansion: a generated
   `case UC_START = IDL_STEP_FIRST:` is not a case label and a generated `step_arm_UC_START = IDL_STEP_FIRST` is
   not a label, so the declaration could not have produced this function's dispatch at all. */
enum { IDL_STEP_STAGE_BASE(UC_STAGES) UC_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const UC_STEPS[] = { UC_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    uint8_t      after_pair;    /* which arm to return to once the pair is converted */
    IterCursor   outer, inner;
    RecordCursor rec;
    JSValue      item[2];
    int          nitem;
    JSValue      src;            /* the USVString arm's string, `?` included, parsed at UC_DONE */
    JSValue      pairs;          /* every converted pair so far, name then value, appended at UC_DONE */
    uint32_t     npairs;
    uint8_t      as_record;      /* the pairs came from the record arm (Web IDL §3.2.23's map) */
    JSValue      unknown;        /* an unknown string init, handed to the object (UspObj's `unknown`) */
    int          unknown_example;
    JSValue      upair;          /* the unknown sequence arm's current innerSequence */
    int64_t      ulen, ui, plen; /* that arm's length and position, and the innerSequence's size */
} JSUspCtorState;

/* EVERY REQUEST THIS MACHINE CAN HAVE IN FLIGHT, LISTED ONCE. STEP_GOTO takes that list at every transition and
   this machine has eleven of them; written out at each, they are eleven statements of one fact and the one not
   updated the day a cursor is added is the transition that quietly stops asserting. A MACRO and not a function
   for STEP_GOTO's own reason — DCHECK stamps the line it is WRITTEN at, so a helper would report this line at
   every abort and say nothing about which transition walked away.
   WHAT IS IN THE LIST IS A REQUEST CURSOR AND NOTHING ELSE: the two ES-iterator cursors' CALL phases, and the
   header's keyed-read, own-keys, own-descriptor and ToString phases — the record cursor's requests are all the
   header's. A cursor's own `phase` is NOT one: it is that cursor's POSITION in the iterator protocol and sits
   at IT_CALL_NEXT between pairs, so listing it would assert that the walk has not started. */
#define UC_GOTO(hdr, s, to) STEP_GOTO((hdr)->stage, (to), &(s)->outer.cphase, &(s)->inner.cphase,   \
                                      &(hdr)->get_phase, &(hdr)->keys_phase, &(hdr)->desc_phase,    \
                                      &(hdr)->str_phase, &(hdr)->len_phase, NULL)

static void js_usp_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSUspCtorState *s = st;
    iter_cursor_visit(ctx, &s->outer, v);
    iter_cursor_visit(ctx, &s->inner, v);
    record_cursor_visit(ctx, &s->rec, v);
    v->val(ctx, &s->item[0]);
    v->val(ctx, &s->item[1]);
    v->val(ctx, &s->unknown);
    v->val(ctx, &s->upair);
    v->val(ctx, &s->src);
    v->val(ctx, &s->pairs);
}

/* §6.2 step 5.2's key conversion is to USVString, which every String and no Symbol is. An unknown key is
   converted at UC_PAIR_NAME_STR, as an unknown value is. */
static int usp_record_key_ok(JSContext *ctx, JSValueConst key, void *user)
{
    (void)user;
    if (JS_IsSymbol(key)) {
        JS_ThrowTypeError(ctx, "a Symbol is not a valid URLSearchParams name");
        return -1;
    }
    return 0;
}

/* §6.2 initialize steps 1.2 and 2: append (name, value) to the list `vals` being built. `as_record` is Web IDL
   §3.2.23's "Set result[typedKey] to typedValue" (step 4.2.4), a MAP's set: two ES keys that convert to one
   USVString are one entry, which keeps the first's position and takes the last's value, so
   `{"\uD835x": "1", "xx": "2", "\uD83Dx": "3"}` is two pairs, both surrogate keys becoming "\uFFFDx". A
   sequence has no such rule: `[["a",1],["a",2]]` is two pairs. Both halves are borrowed. */
static void usp_init_append(JSContext *ctx, JSValue *vals, JSValueConst k, JSValueConst v, int as_record)
{
    int i, n = usp_vals_n(ctx, *vals);

    for (i = 0; as_record && i < n; i++) {
        JSValue a = usp_half(ctx, *vals, i, 0), c;
        bool same;

        /* The keys come from one [[OwnPropertyKeys]] List, which holds no duplicates (ECMAScript §6.1.7.3
           Invariants of the Essential Internal Methods), so two of them meet only through the scalar-value
           replacement, which an unknown key crosses without.
           Named residual. Not covered: an unknown key whose replacement would equal another key. Next diff
           builds: §3.2.12's replacement as a derivation over unknown input, compared here as a fork. How its
           absence shows: a record init over unknown keys holding a tuple more than a browser's list. */
        same = !concolic_is(a) && !concolic_is(k) && JS_IsStrictEqual(ctx, a, k);
        JS_FreeValue(ctx, a);
        if (!same) continue;
        c = usp_vals_new(ctx);
        for (int j = 0; j < n; j++) {
            JSValue x = usp_half(ctx, *vals, j, 0), y = usp_half(ctx, *vals, j, 1);
            usp_vals_push(ctx, c, x, j == i ? v : y);
            JS_FreeValue(ctx, x);
            JS_FreeValue(ctx, y);
        }
        JS_FreeValue(ctx, *vals);
        *vals = c;
        return;
    }
    usp_vals_push(ctx, *vals, k, v);
}

static int js_usp_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSUspCtorState *s = st;
    JSValueConst init = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue in = cb_result;
    int r;

    STEP_DISPATCH(UC_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(UC_START);
    {
        if (JS_IsUndefined(hdr->this_val)) {
            JS_FreeValue(ctx, in);
            JS_ThrowTypeError(ctx, "constructor URLSearchParams requires 'new'");
            return -1;
        }
        s->item[0] = s->item[1] = JS_UNDEFINED;
        /* No unknown init yet, stated before the first arm that can leave: an absent init goes to UC_DONE
           below, which hands `unknown` to the object. */
        s->unknown = JS_UNDEFINED;
        s->unknown_example = 0;
        s->upair = JS_UNDEFINED;
        s->src = JS_UNDEFINED;
        s->pairs = JS_UNDEFINED;
        s->npairs = 0;
        s->as_record = 0;
        iter_cursor_init(&s->outer);
        iter_cursor_init(&s->inner);
        record_cursor_init(&s->rec);
        JS_FreeValue(ctx, in);   /* the prologue's answer; every arm below asks for its own */
        /* §6.2: a non-object init is a USVString — the declaration has already made it one — and a LEADING `?`
           is stripped, which is what makes `new URLSearchParams(u.search)` round-trip. */
        /* §6.2: `optional init = ""`, so an ABSENT init is the empty string and not the string "undefined". */
        if (JS_IsUndefined(init)) {
            UC_GOTO(hdr, s, UC_DONE);
            return JS_STEP_YIELD;
        }
        /* §3.2.25 over unknown input: the arm is a type test the unknown answers only through its type facet,
           so UC_UNKNOWN_ARM forks the arms it admits. */
        if (concolic_is(init)) {
            UC_GOTO(hdr, s, UC_UNKNOWN_ARM);
            return JS_STEP_YIELD;
        }
        if (!JS_IsObject(init)) {
            /* §6.2's USVString arm. The declaration passes the init through unconverted (the union is this
               machine's to resolve), so the scalar-value replacement happens here. */
            JSValue str = JS_ToScalarValueString(ctx, JS_ToString(ctx, init));
            if (JS_IsException(str)) return -1;
            s->src = str;
            UC_GOTO(hdr, s, UC_DONE);
            return JS_STEP_YIELD;
        }
        UC_GOTO(hdr, s, UC_ITER_ASKED);
        return JS_STEP_YIELD;
    }

    STEP_ARM(UC_UNKNOWN_ARM);
    {
        static const char *const OPS[16] = {
            [IDL_UNION_ARM_SEQUENCE | IDL_UNION_ARM_RECORD] =
                "§3.2.25 (sequence<sequence<USVString>> or record<USVString, USVString> or USVString) "
                "arm: sequence, record",
            [IDL_UNION_ARM_STRING | IDL_UNION_ARM_SEQUENCE | IDL_UNION_ARM_RECORD] =
                "§3.2.25 (sequence<sequence<USVString>> or record<USVString, USVString> or USVString) "
                "arm: string, sequence, record",
        };
        unsigned arm = 0;

        r = idl_union_unknown_arm(ctx, hdr, init,
                                  IDL_UNION_ARM_STRING | IDL_UNION_ARM_SEQUENCE | IDL_UNION_ARM_RECORD, OPS, &in,
                                  &arm);
        if (r) return r;
        JS_FreeValue(ctx, in);
        if (arm == IDL_UNION_ARM_RECORD) {
            UC_GOTO(hdr, s, UC_KEY_PAIR);
            return JS_STEP_YIELD;
        }
        if (arm == IDL_UNION_ARM_SEQUENCE) {
            UC_GOTO(hdr, s, UC_USEQ_LEN);
            return JS_STEP_YIELD;
        }
        /* The USVString arm: the list is the parse of the example and every read derives from the unknown
           (usp_derive), with a leading `?` stripped as for a concrete string. */
        {
            JSValue ex;

            s->unknown = JS_DupValue(ctx, init);
            ex = concolic_example_string(ctx, init);
            s->unknown_example = !JS_IsUndefined(ex);
            if (s->unknown_example) {
                ex = JS_ToScalarValueString(ctx, ex);
                if (JS_IsException(ex)) return -1;
                s->src = ex;
            }
        }
        UC_GOTO(hdr, s, UC_DONE);
        return JS_STEP_YIELD;
    }

    /* THE SEQUENCE ARM OVER AN UNKNOWN: an unknown has no iterator to call, so §3.2.21.1's walk is the Array
       iterator's own (%ArrayIteratorPrototype%.next steps 9-14): §7.3.18 LengthOfArrayLike, then Get per index,
       as idl_union_seq_unknown walks its sequence arm. Each innerSequence is walked the same way. */
    STEP_ARM(UC_USEQ_LEN);
    r = step_length_run(ctx, hdr, init, in, &s->ulen, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return -1;
    s->ui = 0;
    UC_GOTO(hdr, s, UC_USEQ_PAIR);
    return JS_STEP_YIELD;

    STEP_ARM(UC_USEQ_PAIR);
    if (s->ui >= s->ulen) {
        JS_FreeValue(ctx, in);
        UC_GOTO(hdr, s, UC_DONE);
        return JS_STEP_YIELD;
    }
    {
        JSValue index = JS_NewInt64(ctx, s->ui);
        JSAtom key = JS_ValueToAtom(ctx, index);
        JSValue pair = JS_UNDEFINED;

        JS_FreeValue(ctx, index);
        CHECK(key != JS_ATOM_NULL, "OOM interning a sequence index");
        r = step_getprop_run(ctx, hdr, init, key, in, &pair, out_cb, out_argc);
        JS_FreeAtom(ctx, key);
        if (r > 0) return r;
        if (r < 0) return -1;
        JS_FreeValue(ctx, s->upair);
        s->upair = pair;
    }
    /* §3.2.21's conversion of the innerSequence: a non-Object is a TypeError. A member read of unknown input
       answers an unknown or a pinned primitive, never a concrete Object. */
    if (!concolic_is(s->upair)) {
        DCHECK(!JS_IsObject(s->upair), "a member read of unknown input answered an Object that is not unknown "
                                       "input — its @@iterator would be the page's code, which this arm cannot run");
        JS_ThrowTypeError(ctx, "a URLSearchParams init pair is not iterable");
        return -1;
    }
    UC_GOTO(hdr, s, UC_USEQ_PLEN);
    return JS_STEP_YIELD;

    STEP_ARM(UC_USEQ_PLEN);
    r = step_length_run(ctx, hdr, s->upair, in, &s->plen, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return -1;
    if (s->plen != 2) {
        JS_ThrowTypeError(ctx, "a URLSearchParams init pair does not contain exactly two items");
        return -1;
    }
    JS_FreeValue(ctx, s->item[0]); JS_FreeValue(ctx, s->item[1]);
    s->item[0] = s->item[1] = JS_UNDEFINED;
    s->nitem = 0;
    UC_GOTO(hdr, s, UC_USEQ_ITEM);
    return JS_STEP_YIELD;

    STEP_ARM(UC_USEQ_ITEM);
    {
        JSAtom key = JS_NewAtomUInt32(ctx, (uint32_t)s->nitem);
        JSValue item = JS_UNDEFINED;

        CHECK(key != JS_ATOM_NULL, "OOM interning a sequence index");
        r = step_getprop_run(ctx, hdr, s->upair, key, in, &item, out_cb, out_argc);
        JS_FreeAtom(ctx, key);
        if (r > 0) return r;
        if (r < 0) return -1;
        s->item[s->nitem++] = item;
    }
    if (s->nitem < 2) return JS_STEP_YIELD;   /* the stage does not move: the second item is the next turn's */
    s->ui++;
    s->after_pair = UC_USEQ_PAIR;
    UC_GOTO(hdr, s, UC_PAIR_NAME_STR);
    return JS_STEP_YIELD;

    STEP_ARM(UC_ITER_ASKED);
    {
        JSValue itf;
        r = step_getprop_run(ctx, hdr, init, JS_WellKnownSymbolAtom(JS_WKS_ITERATOR), in, &itf,
                             out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return -1;
        /* §6.2's init is a union with a sequence member, so Web IDL §3.2.25 step 11.2 chooses the arm and it is
           `? GetMethod(V, %Symbol.iterator%)` — a PRESENT non-callable is a TypeError there, never the record
           arm. `new URLSearchParams({[Symbol.iterator]: 1})` reached that arm and serialized an empty query. */
        r = idl_get_method(ctx, itf, "a URLSearchParams init's @@iterator");
        JS_FreeValue(ctx, itf);
        if (r < 0) return -1;
        UC_GOTO(hdr, s, r ? UC_SEQ_PAIR : UC_KEY_PAIR);
        return JS_STEP_YIELD;
    }

    /* THE SEQUENCE ARM: `sequence<sequence<USVString>>`, nested exactly as Web IDL nests it. §6.2 makes a
       pair that does not hold exactly two items a TypeError, which is why the inner cursor runs one step
       PAST the second item rather than stopping at it. */
    STEP_ARM(UC_SEQ_PAIR);
    r = iter_cursor_run(ctx, hdr, &s->outer, init, in, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return -1;
    if (s->outer.done) {
        UC_GOTO(hdr, s, UC_DONE);
        return JS_STEP_YIELD;
    }
    /* The previous pair's cursor released itself when it answered `done` (idl_iter.c's IT_GET_DONE) and
       UC_SEQ_ITEM is the only way back here, so this plants on a clear slot; the release that used to stand in
       front of it named a state this arm cannot be in. */
    iter_cursor_init(&s->inner);
    JS_FreeValue(ctx, s->item[0]); JS_FreeValue(ctx, s->item[1]);
    s->item[0] = s->item[1] = JS_UNDEFINED;
    s->nitem = 0;
    UC_GOTO(hdr, s, UC_SEQ_ITEM);
    return JS_STEP_YIELD;

    STEP_ARM(UC_SEQ_ITEM);
    r = iter_cursor_run(ctx, hdr, &s->inner, s->outer.value, in, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return -1;
    if (!s->inner.done) {
        if (s->nitem < 2) s->item[s->nitem] = JS_DupValue(ctx, s->inner.value);
        s->nitem++;
        /* THE STAGE DOES NOT MOVE — one item of the page's inner sequence per turn, and the scheduler is asked
           at every one of them because the sequence is as long as the page says. */
        return JS_STEP_YIELD;
    }
    if (s->nitem != 2) {
        JS_ThrowTypeError(ctx, "a URLSearchParams init pair does not contain exactly two items");
        return -1;
    }
    s->after_pair = UC_SEQ_PAIR;
    UC_GOTO(hdr, s, UC_PAIR_NAME_STR);
    return JS_STEP_YIELD;

    /* THE RECORD ARM: `record<USVString, USVString>`, the shared cursor. */
    STEP_ARM(UC_KEY_PAIR);
    r = record_cursor_run(ctx, hdr, &s->rec, init, in, usp_record_key_ok, NULL, out_cb, out_argc);
    if (r > 0) return r;
    if (r < 0) return -1;
    if (s->rec.done) {
        UC_GOTO(hdr, s, UC_DONE);
        return JS_STEP_YIELD;
    }
    JS_FreeValue(ctx, s->item[0]); s->item[0] = JS_DupValue(ctx, s->rec.key);
    JS_FreeValue(ctx, s->item[1]); s->item[1] = JS_DupValue(ctx, s->rec.value);
    s->after_pair = UC_KEY_PAIR;
    UC_GOTO(hdr, s, UC_PAIR_NAME_STR);
    return JS_STEP_YIELD;

    /* §3.2.12's USVString conversion for both halves. ToString may be the page's, so each is a request; the
       scalar-value replacement is what makes an unpaired surrogate U+FFFD, and it is the whole of what makes
       the type different from a DOMString. TWO ARMS, ONE BODY, which is what `case A: case B:` is — and it is
       what the `default:` here used to be, with a DCHECK behind it asserting that no OTHER stage had arrived.
       That assertion is the dispatch's now, and it is made of the declaration rather than of a list written a
       second time in this function. */
    STEP_ARM(UC_PAIR_NAME_STR);
    STEP_ARM(UC_PAIR_VALUE_STR);
    {
        int which = (hdr->stage == UC_PAIR_NAME_STR) ? 0 : 1;

        /* §7.1.19 ToString over unknown input is the identity, so an unknown half crosses as itself and the
           list holds it (UspObj's `vals`): the constructor's result is the platform object either way. */
        if (concolic_is(s->item[which])) {
            JS_FreeValue(ctx, in);
        } else {
            JSValue str = JS_UNDEFINED;

            r = step_tostring_run(ctx, hdr, s->item[which], in, &str, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return -1;
            str = JS_ToScalarValueString(ctx, str);
            if (JS_IsException(str)) return -1;
            JS_FreeValue(ctx, s->item[which]);
            s->item[which] = str;
        }
        if (which == 0) {
            UC_GOTO(hdr, s, UC_PAIR_VALUE_STR);
            return JS_STEP_YIELD;
        }
        if (JS_IsUndefined(s->pairs)) {
            s->pairs = JS_NewArray(ctx);
            CHECK(!JS_IsException(s->pairs), "urlsearchparams: OOM holding the init's pairs");
            s->as_record = s->after_pair == UC_KEY_PAIR;
        }
        DCHECK(s->as_record == (s->after_pair == UC_KEY_PAIR),
               "one URLSearchParams init converted pairs from both its record and its sequence arm");
        CHECK(JS_DefinePropertyValueUint32(ctx, s->pairs, 2 * s->npairs, JS_DupValue(ctx, s->item[0]),
                                           JS_PROP_C_W_E) >= 0 &&
              JS_DefinePropertyValueUint32(ctx, s->pairs, 2 * s->npairs + 1, JS_DupValue(ctx, s->item[1]),
                                           JS_PROP_C_W_E) >= 0, "urlsearchparams: OOM holding an init pair");
        s->npairs++;
        UC_GOTO(hdr, s, s->after_pair);
        return JS_STEP_YIELD;
    }

    /* The list is built here and nowhere earlier: until this stage the state may be forked, and a fork copies
       only what js_usp_ctor_visit names, so the parameter list is held as JS values until nothing can fork. */
    STEP_ARM(UC_DONE);
    JS_FreeValue(ctx, in);
    {
        JSValue vals, unknown;
        uint32_t i;

        DCHECK(!(JS_IsString(s->src) && s->npairs),
               "a URLSearchParams init took both its USVString arm and a pair-converting arm");
        if (JS_IsString(s->src)) {
            size_t n = 0;
            const char *q = JS_ToCStringLen(ctx, &n, s->src);

            if (!q) return -1;
            vals = usp_vals_parse(ctx, q[0] == '?' ? q + 1 : q, q[0] == '?' ? n - 1 : n);
            JS_FreeCString(ctx, q);
        } else {
            vals = usp_vals_new(ctx);
        }
        for (i = 0; i < s->npairs; i++) {
            JSValue k = usp_half(ctx, s->pairs, (int)i, 0), v = usp_half(ctx, s->pairs, (int)i, 1);

            usp_init_append(ctx, &vals, k, v, s->as_record);
            JS_FreeValue(ctx, k);
            JS_FreeValue(ctx, v);
        }
        unknown = s->unknown;
        s->unknown = JS_UNDEFINED;
        *presult = usp_new_with(ctx, JS_UNDEFINED, vals, unknown, s->unknown_example);
    }
    if (JS_IsException(*presult)) return -1;
    return 0;
}

static const IdlStepDecl js_usp_ctor_decl = {
    js_usp_ctor_step, sizeof(JSUspCtorState), js_usp_ctor_visit, NULL,
    "URL §6.2 new URLSearchParams(init)", UC_STEPS
};

/* ---- install --------------------------------------------------------------------------------------------- */

void usp_init(JSContext *ctx)
{
    JSClassDef def = { "URLSearchParams", .finalizer = usp_finalizer, .gc_mark = usp_gc_mark };
    JSRuntime *rt = JS_GetRuntime(ctx);
    static const IdlArgType TWO_STR[2] = { IDL_USVSTRING, IDL_USVSTRING };
    static const IdlArgType ONE_ANY[1] = { IDL_ANY };   /* the union the constructor's machine converts */

    DCHECK(g_usp_rt == NULL || g_usp_rt == rt,
           "URLSearchParams was installed into a second runtime — its class id and step ids belong to the "
           "first, and one WASM instance is one document");
    if (g_usp_rt == rt)
        return;
    g_usp_rt = rt;
    JS_NewClassID(rt, &g_usp_class);
    JS_NewClass(rt, g_usp_class, &def);
    /* THE AGENT STATE THIS LINE JUST CREATED, DECLARED BESIDE IT — `url_search_params` is this file's own row
       on core/platform.c's list, and it could declare nothing at all until that row had a release column. */
    agent_state_class("url_search_params", &g_usp_class,
                      "URL §6.2's URLSearchParams class — the brand Fetch §5.2's BodyInit arm reads through "
                      "usp_list_of and the per-realm prototype slot");
    g_usp_id[USP_APPEND]   = idl_method_id(ctx, TWO_STR, 2, js_usp_member, USP_APPEND);
    g_usp_id[USP_DELETE]   = idl_method_id(ctx, TWO_STR, 2, js_usp_member, USP_DELETE);
    idl_optional_from(1);   /* §6.2: `delete(name, optional value)` — undefined is NOT the value "undefined" */
    g_usp_id[USP_GET]      = idl_method_id_step(ctx, TWO_STR, 1, NULL, 0, &USP_LOOKUP_DECL, USP_GET);
    concolic_set_tuple_serializer(USP_COMPONENT, usp_serialize_value);
    g_usp_id[USP_GETALL]   = idl_method_id(ctx, TWO_STR, 1, js_usp_member, USP_GETALL);
    g_usp_id[USP_HAS]      = idl_method_id_step(ctx, TWO_STR, 2, NULL, 0, &USP_LOOKUP_DECL, USP_HAS);
    idl_optional_from(1);   /* §6.2: `has(name, optional value)` */
    g_usp_id[USP_SET]      = idl_method_id(ctx, TWO_STR, 2, js_usp_member, USP_SET);
    g_usp_id[USP_SORT]     = idl_method_id(ctx, TWO_STR, 0, js_usp_member, USP_SORT);
    g_usp_id[USP_TOSTRING] = idl_method_id(ctx, TWO_STR, 0, js_usp_member, USP_TOSTRING);

    g_usp_pair_handle = idl_pair_iter_declare(ctx, "url_search_params", &USP_PAIR_OPS);
    /* THE REST OF THIS ROW'S AGENT STATE, DECLARED BESIDE THE LINES THAT SET IT, so that the release below can
       be the undo rather than a hand-written list — the runtime latch, the constructor's machine, and the
       handle above whose class id core/idl_iter.c declares under this row from its own file. */
    agent_state_ptr("url_search_params", &g_usp_rt,
                    "the runtime URL §6.2's class and machines were declared in");
    agent_state_id("url_search_params", &g_usp_pair_handle,
                   "URL §6.2's `iterable<USVString, USVString>` handle");

    g_usp_ctor_stepid = idl_method_id_step(ctx, ONE_ANY, 1, NULL, 0, &js_usp_ctor_decl, 0);
    agent_state_id("url_search_params", &g_usp_ctor_stepid,
                   "URL §6.2's `constructor(optional URLSearchParamsInit init)` machine");
    idl_optional_from(0);   /* §6.2: `constructor(optional init = "")` */
    realm_declare_intrinsic(usp_install_realm);
}

/* §6.2's INTERFACE PROTOTYPE OBJECT, ITS Web IDL §3.7.1 INTERFACE OBJECT, AND §3.8's PROPERTY REFERENCE FOR
   IT — FOR ONE REALM.
   THE INTERFACE OBJECT IS HERE BECAUSE §3.8 IS GIVEN A REALM. `define the global property references` is "To
   define the global property references on target, given realm realm", step 1 being "Let interfaces be a
   list that contains every interface that is exposed in realm" — the population is a REALM's and the
   algorithm names no Document. §6.2 declares `[Exposed=*]`, so EVERY realm owes this name — and while it was
   placed from core/platform.c's per-document column, a worker realm, which reaches no
   platform_document_install, got neither the object nor the name. The prototype is in hand here, so the
   separate per-document entry's JS_GetClassProto re-read is gone. */
void usp_install_realm(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_usp_class != 0, "a realm asked for URLSearchParams.prototype before the class was declared");
    prev = JS_GetClassProto(ctx, g_usp_class);
    DCHECK(JS_IsNull(prev), "usp_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "URLSearchParams.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "URLSearchParams");
    idl_install_method(ctx, proto, "append", g_usp_id[USP_APPEND]);
    idl_install_method(ctx, proto, "delete", g_usp_id[USP_DELETE]);
    idl_install_method(ctx, proto, "get", g_usp_id[USP_GET]);
    idl_install_method(ctx, proto, "getAll", g_usp_id[USP_GETALL]);
    idl_install_method(ctx, proto, "has", g_usp_id[USP_HAS]);
    idl_install_method(ctx, proto, "set", g_usp_id[USP_SET]);
    idl_install_method(ctx, proto, "sort", g_usp_id[USP_SORT]);
    idl_install_method(ctx, proto, "toString", g_usp_id[USP_TOSTRING]);
    idl_install_accessor(ctx, proto, "size", js_usp_get_size, 0, -1);
    idl_pair_iter_install(ctx, proto, g_usp_pair_handle);

    {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue ctor;

        DCHECK(g_usp_ctor_stepid >= 0,
               "URLSearchParams was installed before usp_init declared its constructor");
        ctor = idl_step_constructor(ctx, "URLSearchParams", g_usp_ctor_stepid);
        CHECK(!JS_IsException(ctor), "the URLSearchParams interface object could not be allocated");
        JS_SetConstructor(ctx, ctor, proto);
        /* THE EXPOSURE IS THE DOOR'S ANSWER, NOT A CONDITION HERE: idl_define_global_property_reference asks
           §3.3.7 [Exposed] step 1 against this realm's §3.3.8 [Global] global names, keyed by the identifier
           it is already handed, so nothing at this site re-derives what the corpus states. */
        idl_define_global_property_reference(ctx, global, "URLSearchParams", ctor);
        JS_FreeValue(ctx, global);
    }
    JS_SetClassProto(ctx, g_usp_class, proto);   /* the realm owns it from here */
}

/* THE AGENT'S — core/platform.h's release column. IT TAKES NOTHING, and no line of the old body read the
   JSContext it used to take: §6.2's per-realm prototypes and its interface object are the REALMS' and went
   with their contexts, and what is left is a class id, a runtime handle and two pool indices — no value and
   no atom, so not even a JS_FreeValueRT to want the runtime for. */
void usp_free(void)
{
    if (!g_usp_rt)
        return;
    /* the prototypes are the REALMS' — released with their contexts, so this component owns no reference and
       there is nothing to free here. Free, assert, then undo.
       §6.2'S CLASS ID GOES BACK AT 0 — core/agent_state.h's ONE policy. usp_list_of already reads this slot
       defensively (`g_usp_class ? … : NULL`), which is the shape a zeroed id makes correct rather than one
       the undo breaks: a carried id would name a class in a runtime that is gone.
       THREE HAND-WRITTEN LINES STOOD HERE and the undo replaces them, for the reason core/fetch/headers.c's
       release already records. It is not a tidy-up — core/idl_iter.c declares §6.2's ITERATOR class under this
       row from its own file, and no line here can name that slot, so a hand-written release could not have
       given it back at all. */
    idl_pair_iter_release(g_usp_pair_handle);
    concolic_undeclare_sources(USP_COMPONENT);   /* the tuple rows usp_lookup_answer declared */
    concolic_set_tuple_serializer(NULL, NULL);
    agent_state_undo("url_search_params");
}
