/* DOMTokenList — DOM §7.1.
 *
 * WHY IT MATTERS TO A SOLVER: `el.classList.contains('is-admin')` and `classList.add('open')` are how a bundle
 * gates a whole branch of its UI, and a branch is where endpoints live. It was absent, so `el.classList` was
 * undefined and the page's own `.contains` threw — every route behind it unreachable.
 *
 * THERE IS NO STORAGE HERE, AND THAT IS THE DESIGN. §7.1 defines a token list as a VIEW over a content
 * attribute: its tokens ARE the attribute's value split on whitespace, and every mutation re-serialises and
 * writes the attribute back. So this file holds no tokens. That is not a simplification — it is what makes
 * `el.className = 'a'; el.classList.contains('a')` true, and it is what makes the list per-flow for free: the
 * attribute goes through the DOM-mutation chokepoint, so a token added in one arm of a fork is invisible to its
 * sibling, and a class change runs the custom-element attribute reaction, both without a line here.
 *
 * [SameObject] IS AN IDENTITY, so the list object is cached on the element's wrapper: `el.classList ===
 * el.classList` is what the IDL states, and a page that stashes the list and mutates it later must be mutating
 * the same one. The cache rides the WRAPPER, which is per-flow like the wrapper is.
 *
 * WHAT IS HONESTLY ABSENT: the INDEXED PROPERTY GETTER (`list[0]`) and `length`'s liveness as an array-like.
 * `item(i)` and `length` are real; a numeric index needs an exotic object with a [[GetOwnProperty]] that
 * consults the attribute, which is its own mechanism and not something to fake with a snapshot of properties
 * that would go stale the moment the attribute changed. */
#include <string.h>
#include <stdlib.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "solver/concolic.h"   /* an unknown NAME denotes its shape — see concolic_name_cstr */
#include "quickjs.h"
#include "solver/dom_cow.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/idl_indexed.h"
#include "core/dom/node.h"
#include "core/dom/dom_token_list.h"

static const IdlArgType IDL_1STR[1] = { IDL_DOMSTRING };
static const IdlArgType IDL_2STR[2] = { IDL_DOMSTRING, IDL_DOMSTRING };
/* `toggle(DOMString token, optional boolean force)` — the optional boolean is what makes the two-argument form
   mean something different from the one-argument form, so it is declared rather than sniffed. */
static const IdlArgType IDL_STR_BOOL[2] = { IDL_DOMSTRING, IDL_BOOLEAN };

/* PER REALM — §3.7 gives each its own, and here that decides ANSWERS: a C member runs in the realm that
   DEFINED it, so one shared prototype answers every document out of whichever realm built it first. Held in
   quickjs's per-context class-proto slot. */
static JSClassID g_tl_class;
/* Declared once per AGENT (the IDL pool is sealed after agent init); installed per realm. */
static int g_set_value_id = -1, g_item_id = -1, g_contains_id = -1, g_add_id = -1, g_remove_id = -1,
           g_toggle_id = -1, g_replace_id = -1, g_to_string_id = -1;
static JSValue g_key = JS_UNDEFINED;      /* the [SameObject] cache slot on an element's wrapper */
static JSValue g_owner_key = JS_UNDEFINED; /* the element a list is a view over */
static int     g_ready;

/* ---- the view ---------------------------------------------------------------------------------------------- */
/* Which content attribute this list reflects. One list kind so far (`class`), but the name is carried on the
   list rather than assumed, because §7.1's other reflections (`rel`, `sandbox`) are the same object over a
   different attribute and a hard-coded "class" would be a second implementation the day one lands. */
static lxb_dom_element_t *list_owner(JSContext *ctx, JSValueConst this_val, const char **attr)
{
    JSValue owner;
    JSAtom k;
    lxb_dom_node_t *n;

    if (attr) *attr = "class";
    if (!JS_IsObject(this_val)) return NULL;
    k = JS_ValueToAtom(ctx, g_owner_key);
    if (k == JS_ATOM_NULL) return NULL;
    if (JS_GetOwnSlot(ctx, &owner, this_val, k) <= 0) owner = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    n = node_of(owner);
    JS_FreeValue(ctx, owner);
    return n && n->type == LXB_DOM_NODE_TYPE_ELEMENT ? lxb_dom_interface_element(n) : NULL;
}

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

/* §7.1 "ordered set parser": the attribute's value split on ASCII whitespace, duplicates dropped. The caller
   gets the value it must free and a cursor it walks with token_next. */
static const char *list_value(lxb_dom_element_t *el, const char *attr, size_t *len)
{
    const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)attr, strlen(attr), len);
    if (!v) { *len = 0; return ""; }
    return (const char *)v;
}

/* Advance `*p` past whitespace and report the next token's bounds. false when there is none left. */
static bool token_next(const char **p, const char *end, const char **tok, size_t *tlen)
{
    const char *s = *p;
    while (s < end && is_ws(*s)) s++;
    if (s >= end) { *p = s; return false; }
    *tok = s;
    while (s < end && !is_ws(*s)) s++;
    *tlen = (size_t)(s - *tok);
    *p = s;
    return true;
}

static bool value_has(const char *v, size_t vlen, const char *tok, size_t tlen)
{
    const char *p = v, *end = v + vlen, *t;
    size_t l;
    while (token_next(&p, end, &t, &l))
        if (l == tlen && memcmp(t, tok, tlen) == 0) return true;
    return false;
}

/* §7.1's ORDERED SET PARSER DROPS DUPLICATES, and that is what this list IS — a token SET. `class="a a b"` is
   TWO tokens, so `length` is 2 and `item(1)` is `b`; walking the attribute's raw tokens made both of them
   answer 3 and `a`, which is `contains` and `length` disagreeing about the very same list.
   The set's members are the FIRST occurrences, so a token is in the set exactly when it does not appear in the
   value BEFORE itself — which the prefix ending at this token is, and which needs no allocation and no second
   pass. Every member that counts or indexes the list walks with this rather than with token_next; the two are
   deliberately different functions because the mutation walk wants the raw tokens (it dedups into its OUTPUT,
   which is what re-serialises the set). */
static bool set_next(const char *v, const char **p, const char *end, const char **tok, size_t *tlen)
{
    while (token_next(p, end, tok, tlen))
        if (!value_has(v, (size_t)(*tok - v), *tok, *tlen))
            return true;
    return false;
}

/* §7.1 "run the update steps": re-serialise the set and write it through the DOM chokepoint, so the write is
   per-flow and runs the attribute change steps exactly like a setAttribute the page wrote itself. */
static void list_write(lxb_dom_element_t *el, const char *attr, const char *val, size_t len)
{
    dom_cow_set_attribute(el, attr, val, len);
}

/* §7.1: a token must be non-empty and contain no ASCII whitespace. Both are the spec's own errors, not a
   silent no-op — a page that passes "a b" to add() is asking for something the list cannot represent. */
static int token_check(JSContext *ctx, const char *tok, size_t tlen)
{
    size_t i;
    if (tlen == 0) {
        JS_ThrowDOMException(ctx, "SyntaxError", "the token is empty");
        return -1;
    }
    for (i = 0; i < tlen; i++)
        if (is_ws(tok[i])) {
            JS_ThrowDOMException(ctx, "InvalidCharacterError", "the token contains whitespace");
            return -1;
        }
    return 0;
}

/* ---- the members ------------------------------------------------------------------------------------------- */
static JSValue js_tl_length(JSContext *ctx, JSValueConst this_val, int magic)
{
    const char *attr, *v, *p, *end, *t;
    size_t vlen = 0, tlen;
    lxb_dom_element_t *el = list_owner(ctx, this_val, &attr);
    uint32_t n = 0;

    (void)magic;
    if (!el) return JS_NewInt32(ctx, 0);
    v = list_value(el, attr, &vlen);
    p = v; end = v + vlen;
    while (set_next(v, &p, end, &t, &tlen)) n++;
    return JS_NewInt32(ctx, (int)n);
}

/* §7.1 `value` — the attribute itself, and its setter is the attribute's setter. It is the stringifier too. */
static JSValue js_tl_value(JSContext *ctx, JSValueConst this_val, int magic)
{
    const char *attr, *v;
    size_t vlen = 0;
    lxb_dom_element_t *el = list_owner(ctx, this_val, &attr);

    (void)magic;
    if (!el) return JS_NewString(ctx, "");
    v = list_value(el, attr, &vlen);
    return JS_NewStringLen(ctx, v, vlen);
}

static JSValue js_tl_to_string(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)argc; (void)argv; (void)magic;
    return js_tl_value(ctx, this_val, 0);
}

static JSValue js_tl_set_value(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    const char *attr, *s;
    size_t slen = 0;
    lxb_dom_element_t *el = list_owner(ctx, this_val, &attr);

    (void)magic;
    if (!el) return JS_UNDEFINED;
    /* the declaration passes UNKNOWN input through as itself; an unknown token denotes its SHAPE, so
       `classList.add(x)` adds one stable class per source instead of ending the document at the coercion. */
    s = concolic_name_cstr(ctx, val);
    slen = s ? strlen(s) : 0;
    if (!s) return JS_EXCEPTION;
    list_write(el, attr, s, slen);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

/* §7.1 item(index) — null past the end, which is what makes it different from an indexed getter. */
static JSValue js_tl_item(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    const char *attr, *v, *p, *end, *t;
    size_t vlen = 0, tlen;
    lxb_dom_element_t *el = list_owner(ctx, this_val, &attr);
    int64_t want = 0;
    uint32_t n = 0;

    (void)magic;
    if (!el || argc < 1) return JS_NULL;
    JS_ToInt64(ctx, &want, argv[0]);   /* a real number by now: the declaration converted it */
    if (want < 0) return JS_NULL;
    v = list_value(el, attr, &vlen);
    p = v; end = v + vlen;
    while (set_next(v, &p, end, &t, &tlen)) {
        if (n == (uint32_t)want) return JS_NewStringLen(ctx, t, tlen);
        n++;
    }
    return JS_NULL;
}

static JSValue js_tl_contains(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    const char *attr, *v, *tok;
    size_t vlen = 0, tlen = 0;
    lxb_dom_element_t *el = list_owner(ctx, this_val, &attr);
    bool has;

    (void)magic;
    if (!el || argc < 1) return JS_FALSE;
    tok = JS_ToCStringLen(ctx, &tlen, argv[0]);
    if (!tok) return JS_EXCEPTION;
    v = list_value(el, attr, &vlen);
    has = value_has(v, vlen, tok, tlen);
    JS_FreeCString(ctx, tok);
    return JS_NewBool(ctx, has);
}

/* THE ONE MUTATION, under four names. add/remove/toggle/replace differ only in which tokens they mean to be
   present afterwards, so they build the new value the same way: walk the existing tokens, decide each, then
   append what is newly wanted. Four bodies would be four chances to disagree about whitespace and duplicates.
   magic 0 = add, 1 = remove, 2 = toggle, 3 = replace. */
static JSValue js_tl_mutate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    const char *attr, *v, *p, *end, *t;
    size_t vlen = 0, tlen;
    lxb_dom_element_t *el = list_owner(ctx, this_val, &attr);
    char *out = NULL;
    size_t out_len = 0, out_cap = 0;
    JSValue result = JS_UNDEFINED;
    const char *a[2] = { NULL, NULL };
    size_t alen[2] = { 0, 0 };
    int i, na = (magic == 3) ? 2 : 1;
    bool force_given = false, force = false, present, want, replaced = false;

    if (!el || argc < 1) return magic == 2 ? JS_FALSE : (magic == 3 ? JS_FALSE : JS_UNDEFINED);
    for (i = 0; i < na; i++) {
        if (argc <= i) break;
        a[i] = JS_ToCStringLen(ctx, &alen[i], argv[i]);
        if (!a[i] || token_check(ctx, a[i], alen[i]) < 0) { result = JS_EXCEPTION; goto out; }
    }
    if (magic == 2 && argc > 1 && !JS_IsUndefined(argv[1])) {
        force_given = true;
        force = JS_ToBool(ctx, argv[1]);   /* a real boolean by now: the declaration converted it */
    }
    v = list_value(el, attr, &vlen);
    present = value_has(v, vlen, a[0], alen[0]);
    /* §7.1 toggle's answer is whether the token is present AFTERWARDS, and with `force` it is force itself. */
    want = (magic == 0) ? true
         : (magic == 1) ? false
         : (magic == 2) ? (force_given ? force : !present)
         : present;                       /* replace: only if the old token was there */

    out_cap = vlen + alen[0] + alen[1] + 4;
    out = malloc(out_cap);
    CHECK(out != NULL, "DOMTokenList: OOM building the updated token set");
    p = v; end = v + vlen;
    while (token_next(&p, end, &t, &tlen)) {
        const char *keep = t;
        size_t keep_len = tlen;
        if (tlen == alen[0] && memcmp(t, a[0], tlen) == 0) {
            if (magic == 3) {
                if (!want) continue;      /* replace with the old token absent changes nothing */
                keep = a[1]; keep_len = alen[1]; replaced = true;
            } else if (!want) {
                continue;                 /* remove, or toggle to absent */
            } else if (magic != 3) {
                /* already present and wanted: keep it once, and the append below must not add a second */
            }
        }
        /* §7.1's set semantics: a token already emitted is not emitted twice. */
        if (value_has(out, out_len, keep, keep_len)) continue;
        if (out_len) out[out_len++] = ' ';
        memcpy(out + out_len, keep, keep_len);
        out_len += keep_len;
    }
    if (want && magic != 3 && !value_has(out, out_len, a[0], alen[0])) {
        if (out_len) out[out_len++] = ' ';
        memcpy(out + out_len, a[0], alen[0]);
        out_len += alen[0];
    }
    list_write(el, attr, out, out_len);
    if (magic == 2)      result = JS_NewBool(ctx, want);
    else if (magic == 3) result = JS_NewBool(ctx, replaced);
out:
    free(out);
    for (i = 0; i < na; i++)
        if (a[i]) JS_FreeCString(ctx, a[i]);
    return result;
}

/* §7.1 `getter DOMString? item(unsigned long index)` — the INDEXED PROPERTY GETTER, which is what `list[0]`
   is. Its backing is the same attribute every other member reads, so nothing here caches and nothing goes
   stale: the index is resolved against the class attribute at the moment it is asked. */
static uint32_t tl_length(JSContext *ctx, JSValueConst self)
{
    JSValue n = js_tl_length(ctx, self, 0);
    uint32_t v = 0;
    JS_ToUint32(ctx, &v, n);
    JS_FreeValue(ctx, n);
    return v;
}

static JSValue tl_item(JSContext *ctx, JSValueConst self, uint32_t i)
{
    JSValue idx = JS_NewUint32(ctx, i), r;
    JSValueConst argv[1];
    argv[0] = idx;
    r = js_tl_item(ctx, self, 1, argv, 0);
    JS_FreeValue(ctx, idx);
    return JS_IsNull(r) ? (JS_FreeValue(ctx, r), JS_UNDEFINED) : r;   /* past the end is not a property */
}

static const IdlIndexedDecl TOKEN_LIST_INDEXED = { "DOMTokenList", tl_length, tl_item, NULL };

/* ---- the interface ----------------------------------------------------------------------------------------- */
/* §4.9 `[SameObject] readonly attribute DOMTokenList classList` — the SAME object every time, cached on the
   element's wrapper (so per-flow, like the wrapper). A fresh list per read would make `a.classList ===
   a.classList` false, and a page that keeps the list and mutates it later would be mutating a corpse. */
static JSValue js_el_class_list(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSAtom k, ok;
    JSValue list;

    (void)magic;
    DCHECK(g_ready, "classList was read before dom_token_list_init built its prototype");
    if (!JS_IsObject(this_val)) return JS_UNDEFINED;
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL) return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &list, this_val, k) <= 0) list = JS_UNDEFINED;
    if (!JS_IsObject(list)) {
        JS_FreeValue(ctx, list);
        JSValue tl_proto = dom_token_list_proto(ctx);
        list = idl_indexed_new(ctx, tl_proto, &TOKEN_LIST_INDEXED);
        JS_FreeValue(ctx, tl_proto);
        CHECK(!JS_IsException(list), "DOMTokenList: OOM allocating a classList");
        ok = JS_ValueToAtom(ctx, g_owner_key);
        CHECK(ok != JS_ATOM_NULL, "the DOMTokenList owner key could not be reached");
        JS_DefinePropertyValue(ctx, list, ok, JS_DupValue(ctx, this_val), 0);
        JS_FreeAtom(ctx, ok);
        JS_DefinePropertyValue(ctx, (JSValue)this_val, k, JS_DupValue(ctx, list), 0);
    }
    JS_FreeAtom(ctx, k);
    return list;
}

void dom_token_list_init(JSContext *ctx)
{
    JSClassDef d = { "DOMTokenList" };

    DCHECK(!g_ready, "dom_token_list_init ran twice — the interface is declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "classListSlot", false);
    g_owner_key = JS_NewSymbol(ctx, "tokenListOwner", false);
    CHECK(!JS_IsException(g_key) && !JS_IsException(g_owner_key),
          "the DOMTokenList slot keys could not be allocated");
    JS_NewClassID(JS_GetRuntime(ctx), &g_tl_class);
    JS_NewClass(JS_GetRuntime(ctx), g_tl_class, &d);
    g_set_value_id = idl_setter_id(ctx, IDL_DOMSTRING, false, js_tl_set_value, 0);
    g_item_id = idl_method_id(ctx, (const IdlArgType[]){ IDL_LONG }, 1, js_tl_item, 0);
    g_contains_id = idl_method_id(ctx, IDL_1STR, 1, js_tl_contains, 0);
    /* `add` and `remove` are variadic (`DOMString... tokens`) in the IDL; one token is what this declares and
       converts, and a second is not silently ignored — it is not yet accepted, which the audit reports. */
    g_add_id = idl_method_id(ctx, IDL_1STR, 1, js_tl_mutate, 0);
    g_remove_id = idl_method_id(ctx, IDL_1STR, 1, js_tl_mutate, 1);
    g_toggle_id = idl_method_id(ctx, IDL_STR_BOOL, 2, js_tl_mutate, 2);
    idl_optional_from(1);   /* §7.1: `toggle(token, optional force)` */
    g_replace_id = idl_method_id(ctx, IDL_2STR, 2, js_tl_mutate, 3);
    /* §7.1's stringifier IS `value` — the same attribute under the operation's name, so it reads the same
       thing rather than formatting one. Its own body because a getter and a method have different shapes, and
       casting one to the other reads `magic` out of `argc`. */
    g_to_string_id = idl_method_id(ctx, IDL_1STR, 1, js_tl_to_string, 0);
    idl_optional_from(0);   /* §7.1's stringifier takes NO arguments — the declared one is `value`'s */
    g_ready = 1;
    realm_declare_intrinsic(dom_token_list_install_proto);
}

/* §7.1's INTERFACE PROTOTYPE OBJECT, FOR ONE REALM. */
void dom_token_list_install_proto(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_ready, "a realm asked for DOMTokenList.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_tl_class);
    DCHECK(JS_IsNull(prev), "dom_token_list_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "DOMTokenList.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "DOMTokenList");
    idl_install_accessor(ctx, proto, "length", js_tl_length, 0, -1);
    idl_install_accessor(ctx, proto, "value", js_tl_value, 0, g_set_value_id);
    idl_install_method(ctx, proto, "item", 1, g_item_id);
    idl_install_method(ctx, proto, "contains", 1, g_contains_id);
    idl_install_method(ctx, proto, "add", 1, g_add_id);
    idl_install_method(ctx, proto, "remove", 1, g_remove_id);
    idl_install_method(ctx, proto, "toggle", 1, g_toggle_id);
    idl_install_method(ctx, proto, "replace", 2, g_replace_id);
    idl_install_method(ctx, proto, "toString", 0, g_to_string_id);
    /* §3.7.10: an interface with an indexed getter is iterable through %Array.prototype.values%, which is why
       `for (const c of el.classList)` is ordinary code — and had nothing. */
    idl_indexed_install_iterable(ctx, proto);
    JS_SetClassProto(ctx, g_tl_class, proto);
}

JSValue dom_token_list_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_tl_class);
    DCHECK(!JS_IsNull(proto),
           "DOMTokenList.prototype was asked for in a realm that never ran its install");
    return proto;   /* OWNED */
}

void dom_token_list_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;

    DCHECK(g_ready, "DOMTokenList was installed before its prototype was built");
    {
        JSValue proto = dom_token_list_proto(ctx);
        ctor = idl_interface_object(ctx, "DOMTokenList", proto);
        JS_FreeValue(ctx, proto);
    }
    JS_SetPropertyStr(ctx, (JSValue)global, "DOMTokenList", ctor);
}

void dom_token_list_install_element(JSContext *ctx, JSValueConst element_proto)
{
    DCHECK(g_ready, "classList was installed before dom_token_list_init built its prototype");
    idl_install_accessor(ctx, element_proto, "classList", js_el_class_list, 0, -1);
}

void dom_token_list_free(JSContext *ctx)
{
    if (!g_ready) return;
    JS_FreeValue(ctx, g_key);
    JS_FreeValue(ctx, g_owner_key);
    g_key = g_owner_key = JS_UNDEFINED;   /* the prototypes are the REALMS' — released with their contexts */
    g_ready = 0;
}
