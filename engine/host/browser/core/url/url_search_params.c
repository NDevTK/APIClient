/* THE URLSearchParams INTERFACE — WHATWG URL §6, over §5.1's `application/x-www-form-urlencoded` list.
 *
 * It is a LIST OF PAIRS and not a map, for the reason the header list is: `?a=1&a=2` has two entries, `getAll`
 * reads them both back, and `get` answers with the first. A map keyed by name would answer `get` and lose
 * every repeat.
 *
 * IT WRITES BACK. §6.1 gives the object an associated URL, and every mutation runs the UPDATE STEPS: serialize
 * the list and set the URL's query to it, or to null when the list is empty. That is what makes
 * `u.searchParams.set("a", "1")` change `u.href`, which is the entire reason the accessor exists rather than
 * the page building a query string itself. The link is [SameObject] in both directions — one object per URL,
 * held by the URL, and holding the URL back so the update steps have something to write to. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/url/url.h"
#include "core/url/url_search_params.h"
#include "core/idl_args.h"
#include "core/idl_iter.h"

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

/* The class opaque. `owner` is the URL wrapper this belongs to, or JS_UNDEFINED — see the file comment. */
typedef struct { UspList list; JSValue owner; } UspObj;

static JSClassID g_usp_class;
static JSValue   g_usp_proto = JS_UNDEFINED;
static JSRuntime *g_usp_rt;
static int       g_usp_ctor_stepid = -1;
static int       g_usp_pair_handle = -1;

/* ---- the object ------------------------------------------------------------------------------------------ */

static void usp_finalizer(JSRuntime *rt, JSValue val)
{
    UspObj *u = JS_GetOpaque(val, g_usp_class);
    if (u) { usp_list_free(&u->list); JS_FreeValueRT(rt, u->owner); js_free_rt(rt, u); }
}

static void usp_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    UspObj *u = JS_GetOpaque(val, g_usp_class);
    /* The URL holds this object and this object holds the URL — a real cycle, which is what gc_mark is for. */
    if (u) JS_MarkValue(rt, u->owner, mark_func);
}

const UrlEncodedList *usp_list_of(JSValueConst v)
{
    UspObj *u = g_usp_class ? JS_GetOpaque(v, g_usp_class) : NULL;
    return u ? &u->list : NULL;
}

static UspObj *usp_of(JSContext *ctx, JSValueConst v)
{
    UspObj *u = JS_GetOpaque(v, g_usp_class);
    if (!u) JS_ThrowTypeError(ctx, "not a URLSearchParams");
    return u;
}

/* §6.1's UPDATE STEPS: serialize the list onto the associated URL's query, and set that query to NULL rather
   than to the empty string when the list is empty — `u.searchParams.delete("a")` on `?a=1` gives `u.href` with
   no `?` at all, which is exactly the null/empty distinction the record keeps. */
static void usp_update(JSContext *ctx, UspObj *u)
{
    UrlRecord *rec;
    size_t n = 0;
    char *q;

    if (JS_IsUndefined(u->owner)) return;
    rec = url_record_of(u->owner);
    DCHECK(rec != NULL, "a URLSearchParams' associated URL stopped being one");
    q = usp_serialize(&u->list, &n);
    free(rec->query);
    rec->query = n ? q : NULL;
    if (!n) free(q);
    (void)ctx;
}

JSValue usp_new(JSContext *ctx, JSValueConst owner, const char *query, size_t query_len)
{
    UspObj *u;
    JSValue obj;

    DCHECK(g_usp_class != 0, "a URLSearchParams was built before the class existed");
    obj = JS_NewObjectProtoClass(ctx, g_usp_proto, g_usp_class);
    if (JS_IsException(obj))
        return obj;
    u = js_mallocz(ctx, sizeof(*u));
    if (!u) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    u->owner = JS_DupValue(ctx, owner);
    if (query) usp_parse(&u->list, query, query_len);
    JS_SetOpaque(obj, u);
    return obj;
}

void usp_reset(JSContext *ctx, JSValueConst usp, const char *query, size_t query_len)
{
    UspObj *u = JS_GetOpaque(usp, g_usp_class);
    (void)ctx;
    DCHECK(u != NULL, "a URL's search setter re-initialised something that is not a URLSearchParams");
    usp_list_free(&u->list);
    if (query) usp_parse(&u->list, query, query_len);
}

/* ---- §6.2's members --------------------------------------------------------------------------------------- */

enum { USP_APPEND = 0, USP_DELETE, USP_GET, USP_GETALL, USP_HAS, USP_SET, USP_SORT, USP_TOSTRING };

static JSValue js_usp_member(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    UspObj *u = usp_of(ctx, this_val);
    const char *name = NULL, *value = NULL;
    size_t nn = 0, vn = 0;
    JSValue r = JS_UNDEFINED;
    int i, w;

    if (!u) return JS_EXCEPTION;
    if (magic == USP_SORT) {
        /* §6.2 sort(): by the NAMES' code units, and STABLE — the relative order of equal names is preserved,
           which is why this is an insertion sort and not qsort. */
        for (i = 1; i < u->list.n; i++) {
            UspPair tmp = u->list.e[i];
            int j;
            for (j = i - 1; j >= 0 && usp_name_cmp(&u->list.e[j], &tmp) > 0; j--)
                u->list.e[j + 1] = u->list.e[j];
            u->list.e[j + 1] = tmp;
        }
        usp_update(ctx, u);
        return JS_UNDEFINED;
    }
    if (magic == USP_TOSTRING) {
        char *s = usp_serialize(&u->list, NULL);
        r = JS_NewString(ctx, s);
        free(s);
        return r;
    }

    name = JS_ToCStringLen(ctx, &nn, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (!name) return JS_EXCEPTION;
#define USP_NAME_IS(i)  (u->list.e[i].nlen == nn && !memcmp(u->list.e[i].name, name, nn))
#define USP_VALUE_IS(i) (!value || (u->list.e[i].vlen == vn && !memcmp(u->list.e[i].value, value, vn)))
    /* §6.2: `delete` and `has` take an OPTIONAL value, and its presence changes what they match — `delete(name)`
       removes every pair with that name, `delete(name, value)` only the pairs that also have that value. The
       optional-argument rule is what tells the two apart, so undefined must reach here as undefined. */
    if (magic == USP_APPEND || magic == USP_SET ||
        ((magic == USP_DELETE || magic == USP_HAS) && argc > 1 && !JS_IsUndefined(argv[1]))) {
        value = JS_ToCStringLen(ctx, &vn, argv[1]);
        if (!value) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
    }

    switch (magic) {
    case USP_APPEND:
        usp_list_append(&u->list, name, nn, value, vn);
        usp_update(ctx, u);
        break;
    case USP_DELETE:
        for (i = 0, w = 0; i < u->list.n; i++) {
            int match = USP_NAME_IS(i) && USP_VALUE_IS(i);
            if (match) { free(u->list.e[i].name); free(u->list.e[i].value); continue; }
            u->list.e[w++] = u->list.e[i];
        }
        u->list.n = w;
        usp_update(ctx, u);
        break;
    case USP_GET:
        r = JS_NULL;   /* §6.2: absent is null, not "" */
        for (i = 0; i < u->list.n; i++)
            if (USP_NAME_IS(i)) { r = JS_NewStringLen(ctx, u->list.e[i].value, u->list.e[i].vlen); break; }
        break;
    case USP_GETALL: {
        uint32_t k = 0;
        r = JS_NewArray(ctx);
        if (JS_IsException(r)) break;
        for (i = 0; i < u->list.n; i++)
            if (USP_NAME_IS(i))
                JS_SetPropertyUint32(ctx, r, k++,
                                     JS_NewStringLen(ctx, u->list.e[i].value, u->list.e[i].vlen));
        break;
    }
    case USP_HAS:
        r = JS_FALSE;
        for (i = 0; i < u->list.n; i++)
            if (USP_NAME_IS(i) && USP_VALUE_IS(i)) { r = JS_TRUE; break; }
        break;
    default: {
        /* §6.2 set(): the FIRST pair with this name keeps its position and takes the new value, and every
           other pair with that name is removed. Appending after a delete would move it to the end. */
        int found = -1;
        DCHECK(magic == USP_SET, "a URLSearchParams member was declared with a magic this component does not answer");
        for (i = 0, w = 0; i < u->list.n; i++) {
            if (USP_NAME_IS(i)) {
                if (found < 0) {
                    found = w;
                    free(u->list.e[i].value);
                    u->list.e[i].value = usp_strdup(value, vn);
                    u->list.e[i].vlen = vn;
                    u->list.e[w++] = u->list.e[i];
                } else {
                    free(u->list.e[i].name);
                    free(u->list.e[i].value);
                }
                continue;
            }
            u->list.e[w++] = u->list.e[i];
        }
        u->list.n = w;
        if (found < 0) usp_list_append(&u->list, name, nn, value, vn);
        usp_update(ctx, u);
        break;
    }
    }
    JS_FreeCString(ctx, name);
    if (value) JS_FreeCString(ctx, value);
    return r;
#undef USP_NAME_IS
#undef USP_VALUE_IS
}

static JSValue js_usp_get_size(JSContext *ctx, JSValueConst this_val, int magic)
{
    UspObj *u = usp_of(ctx, this_val);
    (void)magic;
    if (!u) return JS_EXCEPTION;
    return JS_NewInt32(ctx, u->list.n);
}

/* §3.7.10's two operations, over the pair list as it stands right now. */
static int usp_pair_count(JSContext *ctx, JSValueConst target)
{
    UspObj *u = JS_GetOpaque(target, g_usp_class);
    (void)ctx;
    return u ? u->list.n : -1;
}

static void usp_pair_at(JSContext *ctx, JSValueConst target, int i, JSValue *key, JSValue *value)
{
    UspObj *u = JS_GetOpaque(target, g_usp_class);
    DCHECK(u != NULL && i < u->list.n, "a URLSearchParams pair was asked for past the end of the list");
    *key = JS_NewStringLen(ctx, u->list.e[i].name, u->list.e[i].nlen);
    *value = JS_NewStringLen(ctx, u->list.e[i].value, u->list.e[i].vlen);
}

static const IdlPairIterOps USP_PAIR_OPS = { usp_pair_count, usp_pair_at, "URLSearchParams" };

/* ---- §6.2's constructor ------------------------------------------------------------------------------------
 *
 * A MACHINE, because its argument is a union whose two object arms are the page's code from end to end: the
 * sequence arm drives an ES iterator and the record arm is [[OwnPropertyKeys]] plus a descriptor and a [[Get]]
 * per key. Both cursors are Web IDL's shared ones. */
enum { UC_START = 0, UC_ITER_ASKED, UC_SEQ_PAIR, UC_SEQ_ITEM, UC_KEY_PAIR,
       /* the pair's two ToString conversions, which are the PAGE'S code when either half is an object with a
          `toString` — so they are requests, not a JS_ToCString from C. Both arms converge on them. */
       UC_PAIR_NAME_STR, UC_PAIR_VALUE_STR, UC_DONE };

typedef struct {
    uint8_t      stage;
    uint8_t      after_pair;    /* which arm to return to once the pair is converted */
    IterCursor   outer, inner;
    RecordCursor rec;
    JSValue      item[2];
    int          nitem;
    UspList      list;
} JSUspCtorState;

static void js_usp_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSUspCtorState *s = st;
    iter_cursor_visit(ctx, &s->outer, v);
    iter_cursor_visit(ctx, &s->inner, v);
    record_cursor_visit(ctx, &s->rec, v);
    v->val(ctx, &s->item[0]);
    v->val(ctx, &s->item[1]);
}

static void js_usp_ctor_release(JSContext *ctx, void *st)
{
    JSUspCtorState *s = st;
    iter_cursor_release(ctx, &s->outer);
    iter_cursor_release(ctx, &s->inner);
    record_cursor_release(ctx, &s->rec);
    JS_FreeValue(ctx, s->item[0]);
    JS_FreeValue(ctx, s->item[1]);
    s->item[0] = s->item[1] = JS_UNDEFINED;
    usp_list_free(&s->list);
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
   `as_record` is §3.2.21's MAP semantics: a record's keys are converted BEFORE they are stored, so two ES keys
   that convert to the same USVString are ONE entry — it keeps the first's position and takes the last's value.
   `{"\uD835x": "1", "xx": "2", "\uD83Dx": "3"}` is two pairs and not three, because both surrogate keys become
   "\uFFFDx". A sequence has no such rule: `[["a",1],["a",2]]` is two pairs. */
static int usp_take_pair(JSContext *ctx, UspList *out, JSValueConst k, JSValueConst v, int as_record)
{
    size_t nn = 0, vn = 0;
    const char *kn = JS_ToCStringLen(ctx, &nn, k);
    const char *vv;
    int i;

    if (!kn) return -1;
    vv = JS_ToCStringLen(ctx, &vn, v);
    if (!vv) { JS_FreeCString(ctx, kn); return -1; }
    if (as_record) {
        for (i = 0; i < out->n; i++) {
            if (out->e[i].nlen != nn || memcmp(out->e[i].name, kn, nn)) continue;
            free(out->e[i].value);
            out->e[i].value = usp_strdup(vv, vn);
            out->e[i].vlen = vn;
            JS_FreeCString(ctx, kn);
            JS_FreeCString(ctx, vv);
            return 0;
        }
    }
    usp_list_append(out, kn, nn, vv, vn);
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

    if (s->stage == UC_START) {
        if (JS_IsUndefined(hdr->this_val)) {
            JS_FreeValue(ctx, in);
            JS_ThrowTypeError(ctx, "constructor URLSearchParams requires 'new'");
            return -1;
        }
        s->item[0] = s->item[1] = JS_UNDEFINED;
        iter_cursor_init(&s->outer);
        iter_cursor_init(&s->inner);
        record_cursor_init(&s->rec);
        /* §6.2: a non-object init is a USVString — the declaration has already made it one — and a LEADING `?`
           is stripped, which is what makes `new URLSearchParams(u.search)` round-trip. */
        /* §6.2: `optional init = ""`, so an ABSENT init is the empty string and not the string "undefined". */
        if (JS_IsUndefined(init)) {
            JS_FreeValue(ctx, in);
            in = JS_UNDEFINED;
            s->stage = UC_DONE;
        } else if (!JS_IsObject(init)) {
            /* §6.2's USVString arm. The declaration passes the init through unconverted (the union is this
               machine's to resolve), so the scalar-value replacement happens here. */
            JSValue str = JS_ToScalarValueString(ctx, JS_ToString(ctx, init));
            size_t n = 0;
            const char *q;
            JS_FreeValue(ctx, in);
            if (JS_IsException(str)) return -1;
            q = JS_ToCStringLen(ctx, &n, str);
            JS_FreeValue(ctx, str);
            if (!q) return -1;
            usp_parse(&s->list, q[0] == '?' ? q + 1 : q, q[0] == '?' ? n - 1 : n);
            JS_FreeCString(ctx, q);
            s->stage = UC_DONE;
        } else {
            s->stage = UC_ITER_ASKED;
        }
    }

    if (s->stage == UC_ITER_ASKED) {
        JSValue itf;
        r = step_getprop_run(ctx, hdr, init, JS_WellKnownSymbolAtom(JS_WKS_ITERATOR), in, &itf,
                             out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return -1;
        in = JS_UNDEFINED;
        s->stage = JS_IsFunction(ctx, itf) ? UC_SEQ_PAIR : UC_KEY_PAIR;
        JS_FreeValue(ctx, itf);
    }

    /* ONE LOOP over the stages, because the two arms CONVERGE: both hand their pair to the same USVString
       conversion and both come back to their own next step afterwards. Written as separate loops it needed a
       goto per arm to re-enter the other's loop, which is the shape that hides a missed stage. */
    while (s->stage != UC_DONE) {
        switch (s->stage) {
        /* THE SEQUENCE ARM: `sequence<sequence<USVString>>`, nested exactly as Web IDL nests it. §6.2 makes a
           pair that does not hold exactly two items a TypeError, which is why the inner cursor runs one step
           PAST the second item rather than stopping at it. */
        case UC_SEQ_PAIR:
            r = iter_cursor_run(ctx, hdr, &s->outer, init, in, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return -1;
            in = JS_UNDEFINED;
            if (s->outer.done) { s->stage = UC_DONE; break; }
            iter_cursor_release(ctx, &s->inner);
            iter_cursor_init(&s->inner);
            JS_FreeValue(ctx, s->item[0]); JS_FreeValue(ctx, s->item[1]);
            s->item[0] = s->item[1] = JS_UNDEFINED;
            s->nitem = 0;
            s->stage = UC_SEQ_ITEM;
            break;

        case UC_SEQ_ITEM:
            r = iter_cursor_run(ctx, hdr, &s->inner, s->outer.value, in, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return -1;
            in = JS_UNDEFINED;
            if (!s->inner.done) {
                if (s->nitem < 2) s->item[s->nitem] = JS_DupValue(ctx, s->inner.value);
                s->nitem++;
                break;
            }
            if (s->nitem != 2) {
                JS_ThrowTypeError(ctx, "a URLSearchParams init pair does not contain exactly two items");
                return -1;
            }
            s->after_pair = UC_SEQ_PAIR;
            s->stage = UC_PAIR_NAME_STR;
            break;

        /* THE RECORD ARM: `record<USVString, USVString>`, the shared cursor. */
        case UC_KEY_PAIR:
            r = record_cursor_run(ctx, hdr, &s->rec, init, in, usp_record_key_ok, NULL, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return -1;
            in = JS_UNDEFINED;
            if (s->rec.done) { s->stage = UC_DONE; break; }
            JS_FreeValue(ctx, s->item[0]); s->item[0] = JS_DupValue(ctx, s->rec.name);
            JS_FreeValue(ctx, s->item[1]); s->item[1] = JS_DupValue(ctx, s->rec.value);
            s->after_pair = UC_KEY_PAIR;
            s->stage = UC_PAIR_NAME_STR;
            break;

        /* §3.2.11's USVString conversion for both halves. ToString may be the page's, so each is a request;
           the scalar-value replacement is what makes an unpaired surrogate U+FFFD, and it is the whole of what
           makes the type different from a DOMString. */
        default: {
            int which = (s->stage == UC_PAIR_NAME_STR) ? 0 : 1;
            JSValue str = JS_UNDEFINED;
            DCHECK(s->stage == UC_PAIR_NAME_STR || s->stage == UC_PAIR_VALUE_STR,
                   "the URLSearchParams constructor was re-entered at a stage it never parks in");
            r = step_tostring_run(ctx, hdr, s->item[which], in, &str, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return -1;
            in = JS_UNDEFINED;
            str = JS_ToScalarValueString(ctx, str);
            if (JS_IsException(str)) return -1;
            JS_FreeValue(ctx, s->item[which]);
            s->item[which] = str;
            if (which == 0) { s->stage = UC_PAIR_VALUE_STR; break; }
            if (usp_take_pair(ctx, &s->list, s->item[0], s->item[1],
                              s->after_pair == UC_KEY_PAIR) < 0) return -1;
            s->stage = s->after_pair;
            break;
        }
        }
    }

    JS_FreeValue(ctx, in);
    *presult = usp_new(ctx, JS_UNDEFINED, NULL, 0);
    if (JS_IsException(*presult)) return -1;
    {
        UspObj *u = JS_GetOpaque(*presult, g_usp_class);
        u->list = s->list;
        memset(&s->list, 0, sizeof s->list);   /* the object owns it now */
    }
    return 0;
}

static const IdlStepDecl js_usp_ctor_decl = {
    js_usp_ctor_step, sizeof(JSUspCtorState), js_usp_ctor_visit, js_usp_ctor_release
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
    g_usp_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_usp_proto), "URLSearchParams.prototype could not be allocated");
    idl_interface_tag(ctx, g_usp_proto, "URLSearchParams");

    idl_install_method(ctx, g_usp_proto, "append", 2,
                       idl_method_id(ctx, TWO_STR, 2, js_usp_member, USP_APPEND));
    idl_install_method(ctx, g_usp_proto, "delete", 1,
                       idl_method_id(ctx, TWO_STR, 2, js_usp_member, USP_DELETE));
    idl_optional_from(1);   /* §6.2: `delete(name, optional value)` — undefined is NOT the value "undefined" */
    idl_install_method(ctx, g_usp_proto, "get", 1,
                       idl_method_id(ctx, TWO_STR, 1, js_usp_member, USP_GET));
    idl_install_method(ctx, g_usp_proto, "getAll", 1,
                       idl_method_id(ctx, TWO_STR, 1, js_usp_member, USP_GETALL));
    idl_install_method(ctx, g_usp_proto, "has", 1,
                       idl_method_id(ctx, TWO_STR, 2, js_usp_member, USP_HAS));
    idl_optional_from(1);   /* §6.2: `has(name, optional value)` */
    idl_install_method(ctx, g_usp_proto, "set", 2,
                       idl_method_id(ctx, TWO_STR, 2, js_usp_member, USP_SET));
    idl_install_method(ctx, g_usp_proto, "sort", 0,
                       idl_method_id(ctx, TWO_STR, 0, js_usp_member, USP_SORT));
    idl_install_method(ctx, g_usp_proto, "toString", 0,
                       idl_method_id(ctx, TWO_STR, 0, js_usp_member, USP_TOSTRING));
    idl_install_accessor(ctx, g_usp_proto, "size", js_usp_get_size, 0, -1);

    g_usp_pair_handle = idl_pair_iter_declare(ctx, &USP_PAIR_OPS);
    idl_pair_iter_install(ctx, g_usp_proto, g_usp_pair_handle);

    g_usp_ctor_stepid = idl_method_id_step(ctx, ONE_ANY, 1, NULL, 0, &js_usp_ctor_decl, 0);
}

void usp_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;
    DCHECK(g_usp_ctor_stepid >= 0, "URLSearchParams was installed before usp_init declared its constructor");
    ctor = idl_step_constructor(ctx, "URLSearchParams", 0, g_usp_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the URLSearchParams interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, g_usp_proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "URLSearchParams", ctor);
}

void usp_free(JSContext *ctx)
{
    if (!g_usp_rt)
        return;
    JS_FreeValue(ctx, g_usp_proto);
    g_usp_proto = JS_UNDEFINED;
    idl_pair_iter_free(ctx, g_usp_pair_handle);
    g_usp_rt = NULL;
    g_usp_ctor_stepid = -1;
}
