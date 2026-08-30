/* CSSOM §6.4.1 — CSSRuleList. See css_rule_list.h for why it is a view over its holder's Array.
 *
 * THE BRAND IS THE OWN SLOT, and it is a slot of ITS OWN rather than the one the holder keeps the Array under.
 * An indexed-property object is what anything with an indexed getter is, so the class cannot tell one
 * collection from another and the slot has to; and the CSSStyleSheet that holds the same Array carries it under
 * the sheet's own key. One key for both would have made `CSSRuleList.prototype.length.call(sheet)` answer
 * instead of throw — the identical mistake core/css/style_sheet_list.c records for StyleSheetList. */
#include <stdbool.h>
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "core/css/css_rule.h"
#include "core/css/css_rule_list.h"
#include "core/idl_args.h"
#include "core/idl_indexed.h"
#include "core/realm.h"
#include "solver/concolic.h"

static JSClassID g_list_class;
static JSValue   g_rules_key = JS_UNDEFINED;
static JSAtom    g_atom_rules = JS_ATOM_NULL;
static int       g_id_item = -1;

static JSValue crl_rules(JSContext *ctx, JSValueConst v)
{
    JSValue rules;

    if (!JS_IsObject(v)) return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &rules, v, g_atom_rules) <= 0) return JS_UNDEFINED;   /* an own SLOT, never a lookup */
    return rules;
}

/* §6.4.1: "The length attribute must return the number of CSSRule objects represented by the collection." */
static uint32_t crl_length(JSContext *ctx, JSValueConst self)
{
    JSValue rules = crl_rules(ctx, self), len;
    uint32_t n = 0;

    if (!JS_IsArray(rules)) { JS_FreeValue(ctx, rules); return 0; }
    len = JS_GetPropertyStr(ctx, rules, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    JS_FreeValue(ctx, rules);
    return n;
}

/* THE INDEXED PROPERTY GETTER — JS_UNDEFINED past the end, which is what a lookup outside §6.4.1's supported
   property indices is. `item()` below turns that into the null its IDL declares. */
static JSValue crl_item(JSContext *ctx, JSValueConst self, uint32_t i)
{
    JSValue rules = crl_rules(ctx, self), r;

    if (!JS_IsArray(rules)) { JS_FreeValue(ctx, rules); return JS_UNDEFINED; }
    r = JS_GetPropertyUint32(ctx, rules, i);
    JS_FreeValue(ctx, rules);
    DCHECK(JS_IsUndefined(r) || css_rule_is(r),
           "a CSSRuleList held something that is not a CSSRule — its indexed getter and its `item` both declare "
           "`CSSRule?`, and §6.4's insert is the one place anything is ever put in one");
    return r;
}

static const IdlIndexedDecl CRL_INDEXED = { "CSSRuleList", crl_length, crl_item, NULL, 0 };

/* Web IDL §3.7.5's brand for the two PROTOTYPE members: the decl callbacks above are reached only through an
   index lookup on an object idl_indexed already resolved, so they answer the empty collection for a stranger,
   while a member read off `CSSRuleList.prototype` directly must THROW. */
static bool crl_is(JSContext *ctx, JSValueConst v)
{
    JSValue rules = crl_rules(ctx, v);
    bool ok = JS_IsArray(rules);

    JS_FreeValue(ctx, rules);
    return ok;
}

/* §6.4.1's `item(index)`: "must return the indexth CSSRule object in the collection. If there is no indexth
   object in the collection, then the method must return null." That null is the whole difference from the
   indexed getter, which is why both exist. */
static JSValue js_crl_item(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue r;
    uint32_t i = 0;

    (void)magic;
    if (!crl_is(ctx, this_val))
        return JS_ThrowTypeError(ctx, "CSSRuleList.prototype.item was reached on something that is not a "
                                      "CSSRuleList");
    DCHECK(argc >= 1, "§6.4.1's `item` reached its body with no argument — its IDL argument is required, so the "
                      "declaration's own argument-count check is what should have refused the call");
    if (concolic_is(argv[0])) {
        /* AN UNKNOWN INDEX. The empty collection is the one length at which that has an answer rather than a
           fork: §6.4.1 returns null for every index at or past the length. */
        DCHECK(crl_length(ctx, this_val) == 0,
               "§6.4.1's `item` was given an UNKNOWN index into a NON-EMPTY CSSRuleList — every rule in it is a "
               "distinct answer, so the read must FORK one flow per supported index (plus the null arm for an "
               "index past the end) instead of deciding it here");
        return JS_NULL;
    }
    JS_ToUint32(ctx, &i, argv[0]);
    r = crl_item(ctx, this_val, i);
    return JS_IsUndefined(r) ? JS_NULL : r;
}

static JSValue js_crl_length(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!crl_is(ctx, this_val))
        return JS_ThrowTypeError(ctx, "CSSRuleList.prototype.length was reached on something that is not a "
                                      "CSSRuleList");
    return JS_NewUint32(ctx, crl_length(ctx, this_val));
}

JSValue css_rule_list_new(JSContext *ctx, JSValue rules)
{
    JSValue proto, obj;

    DCHECK(g_list_class != 0, "a CSSRuleList was built before css_rule_list_init declared the interface");
    DCHECK(JS_IsArray(rules),
           "a CSSRuleList was built over something that is not an Array — the rules are held as one so that "
           "they fork per flow and park with the flow that holds them");
    proto = JS_GetClassProto(ctx, g_list_class);
    DCHECK(!JS_IsNull(proto), "a CSSRuleList was built in a realm that never ran its prototype install");
    obj = idl_indexed_new(ctx, proto, &CRL_INDEXED);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "a CSSRuleList could not be allocated");
    JS_DefinePropertyValue(ctx, obj, g_atom_rules, rules, 0);   /* CONSUMES rules */
    return obj;
}

void css_rule_list_init(JSContext *ctx)
{
    JSClassDef d = { "CSSRuleList" };
    static const IdlArgType ONE_ULONG[1] = { IDL_UNSIGNED_LONG };

    if (g_list_class) return;   /* one AGENT, one class and one pool entry */
    JS_NewClassID(JS_GetRuntime(ctx), &g_list_class);
    JS_NewClass(JS_GetRuntime(ctx), g_list_class, &d);
    g_rules_key = JS_NewSymbol(ctx, "cssRuleListRules", false);
    CHECK(!JS_IsException(g_rules_key), "the CSSRuleList slot key allocation failed");
    g_atom_rules = JS_ValueToAtom(ctx, g_rules_key);
    CHECK(g_atom_rules != JS_ATOM_NULL, "the CSSRuleList slot key could not be interned");
    g_id_item = idl_method_id(ctx, ONE_ULONG, 1, js_crl_item, 0);
    realm_declare_intrinsic(css_rule_list_install_proto);
}

void css_rule_list_install_proto(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_list_class != 0, "a realm asked for CSSRuleList.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_list_class);
    DCHECK(JS_IsNull(prev), "css_rule_list_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "CSSRuleList.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "CSSRuleList");
    idl_install_accessor_no_user_code(ctx, proto, "length", js_crl_length, 0, -1);
    idl_install_method(ctx, proto, "item", 1, g_id_item);
    /* Web IDL §3.7.10: an indexed property getter plus an integer `length` gets %Array.prototype.values% as
       @@iterator, which is what makes `[...sheet.cssRules]` work. §6.4.1 declares no `iterable<>`, so it gets
       that and NOT `entries`/`keys`/`forEach` — two different clauses. */
    idl_indexed_install_iterable(ctx, proto);
    JS_SetClassProto(ctx, g_list_class, proto);
}

void css_rule_list_install(JSContext *ctx, JSValueConst global)
{
    JSValue proto = JS_GetClassProto(ctx, g_list_class);

    DCHECK(!JS_IsNull(proto), "CSSRuleList was installed in a realm that never ran its prototype install");
    /* §6.4.1 declares no constructor, so the interface object's call and construct both throw. */
    JS_SetPropertyStr(ctx, (JSValue)global, "CSSRuleList", idl_interface_object(ctx, "CSSRuleList", proto));
    JS_FreeValue(ctx, proto);
}

void css_rule_list_free(JSRuntime *rt)
{
    if (!g_list_class) return;   /* the prototype is the REALM's — released with its context */
    JS_FreeAtomRT(rt, g_atom_rules);
    g_atom_rules = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_rules_key);
    g_rules_key = JS_UNDEFINED;
    g_id_item = -1;
}
