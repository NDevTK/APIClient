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
#include "solver/concolic.h"
#include "solver/cow.h"

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
   `unknown` is the unknown string the constructor parsed, or JS_UNDEFINED: when set, `list` is the parse of its
   example (empty where it has none) and every read answers a derivation over it (usp_derive).
   THE LIST IS PER-FLOW STATE AND IS NEVER CHANGED IN PLACE. `list` points into `list_box`, a payload whose
   storage the box owns (usp_list_box); a write builds a changed copy in a new box (usp_list_edit), so the
   capture restores a sibling's `list` and `list_box` together and no arm sees another's write. */
typedef struct { JSValue list_box; UspList *list; JSValue owner; JSValue unknown; int unknown_example; } UspObj;
#define USP_OFF(f) (uint16_t)offsetof(UspObj, f)
static const uint16_t USP_VALS[] = { USP_OFF(list_box), USP_OFF(owner), USP_OFF(unknown) };
static const CowRecord USP_REC = { sizeof(UspObj), USP_VALS, (int)(sizeof USP_VALS / sizeof *USP_VALS) };

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

/* Replace the record's list with `l` (malloc'd, taken), through the record's write. */
static void usp_list_replace(JSContext *ctx, UspObj *u, UspList *l)
{
    cow_record_set(ctx, u, &USP_REC, &u->list_box, usp_list_box(ctx, l));
    u->list = l;
}

/* §6.2's list as this flow may change it: a copy, held by the record from here. */
static UspList *usp_list_edit(JSContext *ctx, UspObj *u)
{
    UspList *l = calloc(1, sizeof *l);
    int i;

    CHECK(l != NULL, "urlsearchparams: OOM copying a list for a write");
    for (i = 0; i < u->list->n; i++) {
        const UspPair *e = &u->list->e[i];
        usp_list_append(l, e->name, e->nlen, e->nhole, e->value, e->vlen, e->vhole);
    }
    usp_list_replace(ctx, u, l);
    return l;
}

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

/* A URLSearchParams over `l` (malloc'd, taken) and `unknown` (taken), initialised before the record is
   reachable. */
static JSValue usp_new_with(JSContext *ctx, JSValueConst owner, UspList *l, JSValue unknown, int unknown_example)
{
    UspObj *u;
    JSValue obj;

    DCHECK(g_usp_class != 0, "a URLSearchParams was built before the class existed");
    {
        JSValue proto = JS_GetClassProto(ctx, g_usp_class);
        DCHECK(!JS_IsNull(proto), "a URLSearchParams was minted in a realm that never ran its install");
        obj = JS_NewObjectProtoClass(ctx, proto, g_usp_class);
        JS_FreeValue(ctx, proto);
    }
    u = JS_IsException(obj) ? NULL : js_mallocz(ctx, sizeof(*u));
    if (!u) { JS_FreeValue(ctx, obj); JS_FreeValue(ctx, unknown); usp_list_box_free(NULL, NULL, l); return JS_EXCEPTION; }
    u->owner = JS_DupValue(ctx, owner);
    u->unknown = unknown;
    u->unknown_example = unknown_example;
    u->list = l;
    u->list_box = usp_list_box(ctx, l);
    JS_SetOpaque(obj, u);
    return obj;
}

JSValue usp_new(JSContext *ctx, JSValueConst owner, const char *query, size_t query_len)
{
    UspList *l = calloc(1, sizeof *l);

    CHECK(l != NULL, "urlsearchparams: OOM building a list");
    if (query) usp_parse(l, query, query_len);
    return usp_new_with(ctx, owner, l, JS_UNDEFINED, 0);
}

void usp_reset(JSContext *ctx, JSValueConst usp, const char *query, size_t query_len)
{
    UspObj *u = usp_rec(usp);
    UspList *l = calloc(1, sizeof *l);

    DCHECK(u != NULL, "a URL's search setter re-initialised something that is not a URLSearchParams");
    CHECK(l != NULL, "urlsearchparams: OOM building a list");
    if (query) usp_parse(l, query, query_len);
    usp_list_replace(ctx, u, l);
}

/* ---- §6.2's members --------------------------------------------------------------------------------------- */

enum { USP_APPEND = 0, USP_DELETE, USP_GET, USP_GETALL, USP_HAS, USP_SET, USP_SORT, USP_TOSTRING,
       USP_MEMBER_N };
/* THE AGENT'S POOL ENTRIES, one per §6.2 operation — the OBJECTS they are installed as are each realm's. */
static int g_usp_id[USP_MEMBER_N];

/* WHAT A HALF THAT IS A HOLE CANNOT DO YET, SAID ONCE. §6.2's list is a list of tuples of USVStrings and this
   one holds their BYTES, so an unknown enters it as its display shape (see UrlEncodedPair). Bytes are enough
   for the two things §6.2 does with a stored half that this engine can already answer — §5.2 serializing it
   onto the associated URL, and §5.2 serializing it as a request body — because the serializer copies a hole
   verbatim and the emission reads it by its braces.
   THEY ARE NOT ENOUGH FOR THE OTHER TWO, and each is a WRONG ANSWER rather than a missing one, which is why
   this crashes instead of proceeding:
     - an EQUALITY. `USP_NAME_IS`/`USP_VALUE_IS` memcmp the halves, and a shape against real bytes compares
       FALSE while having proven nothing — so `get(unknown)` answers null and `delete(name, unknown)` removes
       nothing, both of them decisions this flow never made. §Solver-half's forced multi-path execution runs
       BOTH arms of an undecided equality; a memcmp runs neither.
     - a READ BACK AS A VALUE. §6.2's `get`, `getAll` and the pair iterator hand the page a half, and a page
       branching on what it gets back must branch on the UNKNOWN. Answering with the shape as a real String
       de-taints it: the value that came out of `navigator.language` is indistinguishable from a literal, and
       every gate downstream of it is decided concretely.
   BUILD: §6.2's list carrying the VALUE and not its bytes — a concolic rides a JSValue, and the FormData
   entry list (core/html/form_data.c) already holds one per entry and projects to bytes at its serializers.
   §5.1's byte list stays bytes: it is produced by a parser over bytes and consumed by a serializer over
   bytes, and it is the INTERFACE that has USVStrings. */
#define USP_HOLE_WHY(what)                                                                          \
    "a URLSearchParams " what " — §6.2's list holds USVStrings and this list holds their BYTES, so " \
    "an unknown is in it as its display shape and this operation would decide or de-taint it. Build " \
    "the value-carrying §6.2 list (form_data.c's entry list is the worked example); §5.1's byte list " \
    "stays bytes"

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
    DCHECK(!JS_IsUninitialized(d), "a URLSearchParams list parsed from an unknown derived no unknown list");
    cow_record_set(ctx, u, &USP_REC, &u->unknown, d);
}

static JSValue js_usp_member(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    UspObj *u = usp_of(ctx, this_val);
    const char *name = NULL, *value = NULL;
    size_t nn = 0, vn = 0;
    int nhole = 0, vhole = 0;
    JSValue r = JS_UNDEFINED;
    int i, w;

    if (!u) return JS_EXCEPTION;
    /* §6.2's four mutations write a copy of the list (see UspObj). */
    if (magic == USP_APPEND || magic == USP_DELETE || magic == USP_SET || magic == USP_SORT)
        usp_list_edit(ctx, u);
    if (magic == USP_SORT) {
        /* §6.2 sort(): by the NAMES' code units, and STABLE — the relative order of equal names is preserved,
           which is why this is an insertion sort and not qsort. */
        for (i = 0; i < u->list->n; i++)
            DCHECK(!u->list->e[i].nhole,
                   USP_HOLE_WHY("sort() ordered a name that is a hole by its display shape's code units"));
        for (i = 1; i < u->list->n; i++) {
            UspPair tmp = u->list->e[i];
            int j;
            for (j = i - 1; j >= 0 && usp_name_cmp(&u->list->e[j], &tmp) > 0; j--)
                u->list->e[j + 1] = u->list->e[j];
            u->list->e[j + 1] = tmp;
        }
        usp_update(ctx, u);
        if (!JS_IsUndefined(u->unknown)) usp_rederive(ctx, u, "URLSearchParams.sort", 0, NULL);
        return JS_UNDEFINED;
    }
    if (magic == USP_TOSTRING) {
        char *s = usp_serialize(u->list, NULL);
        r = JS_NewString(ctx, s);
        free(s);
        return JS_IsUndefined(u->unknown) ? r : usp_derive(ctx, u, "URLSearchParams.toString", r, 0, NULL);
    }

    /* §6.2's members take USVStrings, and an UNKNOWN one has no bytes to be. The Web IDL declaration passes
       unknown external input through as itself on purpose, so a JS_ToCString here is the one thing this
       boundary must not do (quickjs.c's js_force_tostring says so at the crash): a `const char *` cannot carry
       a concolic, and §7.1.19 ToString ( arg ) over an unknown is the identity, so there is nothing to convert.
       NEITHER HALF IS AN OPERATOR — none of the four members reaching this line returns the converted string.
       `append` and `set` STORE it, and §6.2 URLSearchParams class's update steps then serialize it onto the
       associated URL, which is
       what `vscode.dev`'s `e.searchParams.set("vscode-lang", navigator.language.toLowerCase())` does one line
       before it assigns `e.href` to `window.location.href`. `delete` and `has` MATCH against it. So each half
       is a NAME in js_force_tostring's sense — an unknown denotes its own display SHAPE, a real string, stable
       per source — and the pair records WHICH halves are holes so §5.2's serializer does not encode them out
       of existence.
       THE LENGTH IS strlen FOR A HOLE AND JS_ToCStringLen's FOR DATA, and the difference is not cosmetic: a
       real USVString may contain U+0000 (`?a=b%00c` is one pair whose value is three characters), while a
       display shape has none — which url_encoded_list_append asserts, because §5.2 copies a hole half verbatim
       and measures it with strlen. Both spellings return an OWNED string, so there is one free path below. */
    {
        JSValueConst a0 = argc > 0 ? argv[0] : JS_UNDEFINED;
        nhole = concolic_is(a0);
        name = nhole ? concolic_name_cstr(ctx, a0) : JS_ToCStringLen(ctx, &nn, a0);
        if (!name) return JS_EXCEPTION;
        if (nhole) nn = strlen(name);
    }
#define USP_NAME_IS(i)  (u->list->e[i].nlen == nn && !memcmp(u->list->e[i].name, name, nn))
#define USP_VALUE_IS(i) (!value || (u->list->e[i].vlen == vn && !memcmp(u->list->e[i].value, value, vn)))
    /* §6.2: `delete` and `has` take an OPTIONAL value, and its presence changes what they match — `delete(name)`
       removes every pair with that name, `delete(name, value)` only the pairs that also have that value. The
       optional-argument rule is what tells the two apart, so undefined must reach here as undefined. */
    if (magic == USP_APPEND || magic == USP_SET ||
        ((magic == USP_DELETE || magic == USP_HAS) && argc > 1 && !JS_IsUndefined(argv[1]))) {
        vhole = concolic_is(argv[1]);
        value = vhole ? concolic_name_cstr(ctx, argv[1]) : JS_ToCStringLen(ctx, &vn, argv[1]);
        if (!value) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
        if (vhole) vn = strlen(value);
    }
    /* THE EQUALITY BARRIER. Everything but `append` memcmps the NAME against the stored halves, and
       `delete`/`has` memcmp the VALUE too — see USP_HOLE_WHY. `set` is in the first group: it compares names
       to find the tuple to overwrite, and only its VALUE is free to be a hole. */
    DCHECK(!nhole || magic == USP_APPEND,
           USP_HOLE_WHY("member matched an unknown NAME against the stored names by memcmp"));
    DCHECK(!vhole || magic == USP_APPEND || magic == USP_SET,
           USP_HOLE_WHY("delete()/has() matched an unknown VALUE against the stored values by memcmp"));

    switch (magic) {
    case USP_APPEND:
        usp_list_append(u->list, name, nn, nhole, value, vn, vhole);
        usp_update(ctx, u);
        break;
    case USP_DELETE:
        for (i = 0, w = 0; i < u->list->n; i++) {
            int match = USP_NAME_IS(i) && USP_VALUE_IS(i);
            if (match) { free(u->list->e[i].name); free(u->list->e[i].value); continue; }
            u->list->e[w++] = u->list->e[i];
        }
        u->list->n = w;
        usp_update(ctx, u);
        break;
    case USP_GET:
        r = JS_NULL;   /* §6.2: absent is null, not "" */
        for (i = 0; i < u->list->n; i++)
            if (USP_NAME_IS(i)) {
                /* THE READ-BACK BARRIER — see USP_HOLE_WHY. §6.2 get() hands the page the tuple's VALUE, and a
                   page branching on what it gets back must branch on the UNKNOWN this list stored the shape
                   of; a real String here de-taints it and every gate below decides concretely. */
                DCHECK(!u->list->e[i].vhole,
                       USP_HOLE_WHY("get() answered with a value that is a hole, as a real String"));
                r = JS_NewStringLen(ctx, u->list->e[i].value, u->list->e[i].vlen);
                break;
            }
        break;
    case USP_GETALL: {
        uint32_t k = 0;
        r = JS_NewArray(ctx);
        if (JS_IsException(r)) break;
        for (i = 0; i < u->list->n; i++)
            if (USP_NAME_IS(i)) {
                DCHECK(!u->list->e[i].vhole,
                       USP_HOLE_WHY("getAll() answered with a value that is a hole, as a real String"));
                JS_SetPropertyUint32(ctx, r, k++,
                                     JS_NewStringLen(ctx, u->list->e[i].value, u->list->e[i].vlen));
            }
        break;
    }
    case USP_HAS:
        r = JS_FALSE;
        for (i = 0; i < u->list->n; i++)
            if (USP_NAME_IS(i) && USP_VALUE_IS(i)) { r = JS_TRUE; break; }
        break;
    default: {
        /* §6.2 set(): the FIRST pair with this name keeps its position and takes the new value, and every
           other pair with that name is removed. Appending after a delete would move it to the end. */
        int found = -1;
        DCHECK(magic == USP_SET, "a URLSearchParams member was declared with a magic this component does not answer");
        for (i = 0, w = 0; i < u->list->n; i++) {
            if (USP_NAME_IS(i)) {
                if (found < 0) {
                    found = w;
                    free(u->list->e[i].value);
                    u->list->e[i].value = usp_strdup(value, vn);
                    u->list->e[i].vlen = vn;
                    u->list->e[i].vhole = vhole ? 1u : 0u;   /* the hole bit rides the value it describes */
                    u->list->e[w++] = u->list->e[i];
                } else {
                    free(u->list->e[i].name);
                    free(u->list->e[i].value);
                }
                continue;
            }
            u->list->e[w++] = u->list->e[i];
        }
        u->list->n = w;
        if (found < 0) usp_list_append(u->list, name, nn, nhole, value, vn, vhole);
        usp_update(ctx, u);
        break;
    }
    }
    JS_FreeCString(ctx, name);
    if (value) JS_FreeCString(ctx, value);
    if (!JS_IsUndefined(u->unknown) && !JS_IsException(r)) {
        if (magic == USP_GET || magic == USP_GETALL || magic == USP_HAS)
            r = usp_derive(ctx, u, magic == USP_GET ? "URLSearchParams.get" : magic == USP_GETALL
                                       ? "URLSearchParams.getAll" : "URLSearchParams.has", r, argc, argv);
        else
            usp_rederive(ctx, u, magic == USP_APPEND ? "URLSearchParams.append" : magic == USP_DELETE
                                     ? "URLSearchParams.delete" : "URLSearchParams.set", argc, argv);
    }
    return r;
#undef USP_NAME_IS
#undef USP_VALUE_IS
}

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

static void usp_pair_at(JSContext *ctx, JSValueConst target, int i, JSValue *key, JSValue *value)
{
    UspObj *u = usp_rec(target);
    DCHECK(u != NULL && i < u->list->n, "a URLSearchParams pair was asked for past the end of the list");
    /* THE READ-BACK BARRIER, over §6.2's "value pairs to iterate over" — see USP_HOLE_WHY. `for (const [k, v]
       of params)` hands the page both halves, and a half this list holds the display shape of would arrive as
       a real String with its provenance gone. */
    DCHECK(!u->list->e[i].nhole && !u->list->e[i].vhole,
           USP_HOLE_WHY("pair iterator answered with a half that is a hole, as a real String"));
    *key = JS_NewStringLen(ctx, u->list->e[i].name, u->list->e[i].nlen);
    *value = JS_NewStringLen(ctx, u->list->e[i].value, u->list->e[i].vlen);
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
    UspList      list;
    JSValue      unknown;        /* an unknown string init, handed to the object (UspObj's `unknown`) */
    int          unknown_example;
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
                                      &(hdr)->str_phase, NULL)

static void js_usp_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSUspCtorState *s = st;
    iter_cursor_visit(ctx, &s->outer, v);
    iter_cursor_visit(ctx, &s->inner, v);
    record_cursor_visit(ctx, &s->rec, v);
    v->val(ctx, &s->item[0]);
    v->val(ctx, &s->item[1]);
    v->val(ctx, &s->unknown);
}

/* THE PARAMETER LIST ALONE — a malloc'd array of malloc'd name/value pairs, which is not a reference and which
   no declaration names. The two cursors and the pair being read are named by js_usp_ctor_visit, and the
   teardown discharges that one list. */
static void js_usp_ctor_release(JSContext *ctx, void *st)
{
    (void)ctx;
    usp_list_free(&((JSUspCtorState *)st)->list);
}

/* §6.2 step 5.2's key conversion is to USVString, which every String and no Symbol is. */
static int usp_record_key_ok(JSContext *ctx, JSValueConst key, void *user)
{
    (void)user;
    if (JS_IsSymbol(key)) {
        JS_ThrowTypeError(ctx, "a Symbol is not a valid URLSearchParams name");
        return -1;
    }
    return 0;
}

/* Both halves are USVStrings by the time this runs, so what is left is the bytes.
   `as_record` is Web IDL §3.2.23's MAP semantics: a record's keys are converted BEFORE they are stored, so two ES keys
   that convert to the same USVString are ONE entry — it keeps the first's position and takes the last's value.
   `{"\uD835x": "1", "xx": "2", "\uD83Dx": "3"}` is two pairs and not three, because both surrogate keys become
   "\uFFFDx". A sequence has no such rule: `[["a",1],["a",2]]` is two pairs. */
/* THE SAME QUESTION THIS COMPONENT'S MEMBERS ASK, ASKED AT ITS OTHER ENTRY. §6.2's initialize builds the list
   from the sequence and record arms, and its Web IDL USVString conversion passes UNKNOWN external input
   through as itself exactly as a member's does — so `new URLSearchParams({lang: navigator.language})` reaches
   this line with a concolic. An entry that skipped the question would report the same absent capability as an
   unrelated ToString failure one boundary away, which is why it is asked at every entry that builds a pair.
   See js_usp_member's conversions for why each half is a NAME, and USP_HOLE_WHY for what a hole cannot do. */
static int usp_take_pair(JSContext *ctx, UspList *out, JSValueConst k, JSValueConst v, int as_record)
{
    size_t nn = 0, vn = 0;
    int nhole = concolic_is(k), vhole = concolic_is(v);
    const char *kn = nhole ? concolic_name_cstr(ctx, k) : JS_ToCStringLen(ctx, &nn, k);
    const char *vv;
    int i;

    if (!kn) return -1;
    if (nhole) nn = strlen(kn);
    vv = vhole ? concolic_name_cstr(ctx, v) : JS_ToCStringLen(ctx, &vn, v);
    if (!vv) { JS_FreeCString(ctx, kn); return -1; }
    if (vhole) vn = strlen(vv);
    if (as_record) {
        /* §6.2 initialize step 2 over Web IDL §3.2.23's MAP: the key dedup is an EQUALITY, so an unknown key
           takes USP_HOLE_WHY's first arm — a shape against real bytes compares false having proven nothing. */
        DCHECK(!nhole, USP_HOLE_WHY("record init deduplicated an unknown KEY against the stored names by "
                                    "memcmp"));
        for (i = 0; i < out->n; i++) {
            if (out->e[i].nlen != nn || memcmp(out->e[i].name, kn, nn)) continue;
            free(out->e[i].value);
            out->e[i].value = usp_strdup(vv, vn);
            out->e[i].vlen = vn;
            out->e[i].vhole = vhole ? 1u : 0u;
            JS_FreeCString(ctx, kn);
            JS_FreeCString(ctx, vv);
            return 0;
        }
    }
    usp_list_append(out, kn, nn, nhole, vv, vn, vhole);
    JS_FreeCString(ctx, kn);
    JS_FreeCString(ctx, vv);
    return 0;
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
        s->unknown = JS_UNDEFINED;
        /* §3.2.25 OVER UNKNOWN INPUT: the union's arm is a runtime type test on the value, which an unknown
           answers only through its stated type set. A String in every world takes the USVString arm; the list
           is the parse of its example and every read derives from the unknown (usp_derive). */
        if (concolic_is(init)) {
            JSValue ex;
            size_t n = 0;
            const char *q;

            if (!concolic_type_is_string(init))
                DFAIL("URL §6.2's init union was handed unknown input whose type set is not String, so which of "
                      "its three arms runs is a runtime type test over unknown input — build that fork; the "
                      "release arm takes the USVString arm");
            s->unknown = JS_DupValue(ctx, init);
            ex = concolic_example_string(ctx, init);
            s->unknown_example = !JS_IsUndefined(ex);
            if (s->unknown_example) {
                ex = JS_ToScalarValueString(ctx, ex);
                if (JS_IsException(ex)) return -1;
                q = JS_ToCStringLen(ctx, &n, ex);
                JS_FreeValue(ctx, ex);
                if (!q) return -1;
                usp_parse(&s->list, q[0] == '?' ? q + 1 : q, q[0] == '?' ? n - 1 : n);
                JS_FreeCString(ctx, q);
            }
            UC_GOTO(hdr, s, UC_DONE);
            return JS_STEP_YIELD;
        }
        if (!JS_IsObject(init)) {
            /* §6.2's USVString arm. The declaration passes the init through unconverted (the union is this
               machine's to resolve), so the scalar-value replacement happens here. */
            JSValue str = JS_ToScalarValueString(ctx, JS_ToString(ctx, init));
            size_t n = 0;
            const char *q;
            if (JS_IsException(str)) return -1;
            q = JS_ToCStringLen(ctx, &n, str);
            JS_FreeValue(ctx, str);
            if (!q) return -1;
            usp_parse(&s->list, q[0] == '?' ? q + 1 : q, q[0] == '?' ? n - 1 : n);
            JS_FreeCString(ctx, q);
            UC_GOTO(hdr, s, UC_DONE);
            return JS_STEP_YIELD;
        }
        UC_GOTO(hdr, s, UC_ITER_ASKED);
        return JS_STEP_YIELD;
    }

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
    JS_FreeValue(ctx, s->item[0]); s->item[0] = JS_DupValue(ctx, s->rec.name);
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
        JSValue str = JS_UNDEFINED;

        r = step_tostring_run(ctx, hdr, s->item[which], in, &str, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return -1;
        str = JS_ToScalarValueString(ctx, str);
        if (JS_IsException(str)) return -1;
        JS_FreeValue(ctx, s->item[which]);
        s->item[which] = str;
        if (which == 0) {
            UC_GOTO(hdr, s, UC_PAIR_VALUE_STR);
            return JS_STEP_YIELD;
        }
        if (usp_take_pair(ctx, &s->list, s->item[0], s->item[1],
                          s->after_pair == UC_KEY_PAIR) < 0) return -1;
        UC_GOTO(hdr, s, s->after_pair);
        return JS_STEP_YIELD;
    }

    STEP_ARM(UC_DONE);
    JS_FreeValue(ctx, in);
    {
        UspList *l = malloc(sizeof *l);
        JSValue unknown = s->unknown;

        CHECK(l != NULL, "urlsearchparams: OOM holding a constructed list");
        *l = s->list;
        memset(&s->list, 0, sizeof s->list);   /* the object owns it now */
        s->unknown = JS_UNDEFINED;
        *presult = usp_new_with(ctx, JS_UNDEFINED, l, unknown, s->unknown_example);
    }
    if (JS_IsException(*presult)) return -1;
    return 0;
}

static const IdlStepDecl js_usp_ctor_decl = {
    js_usp_ctor_step, sizeof(JSUspCtorState), js_usp_ctor_visit, js_usp_ctor_release,
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
    g_usp_id[USP_GET]      = idl_method_id(ctx, TWO_STR, 1, js_usp_member, USP_GET);
    g_usp_id[USP_GETALL]   = idl_method_id(ctx, TWO_STR, 1, js_usp_member, USP_GETALL);
    g_usp_id[USP_HAS]      = idl_method_id(ctx, TWO_STR, 2, js_usp_member, USP_HAS);
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
    agent_state_undo("url_search_params");
}
