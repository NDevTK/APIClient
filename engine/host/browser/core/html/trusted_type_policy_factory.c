/* Trusted Types §2.3.2 TrustedTypePolicyFactory and §4.1.1 Extensions to the WindowOrWorkerGlobalScope
 * interface — see trusted_type_policy_factory.h.
 *
 *   [Exposed=(Window,Worker)] interface TrustedTypePolicyFactory {
 *     TrustedTypePolicy createPolicy(DOMString policyName, optional TrustedTypePolicyOptions policyOptions = {});
 *     boolean isHTML(any value); boolean isScript(any value); boolean isScriptURL(any value);
 *     readonly attribute TrustedHTML emptyHTML; readonly attribute TrustedScript emptyScript;
 *     DOMString? getAttributeType(DOMString tagName, DOMString attribute, optional DOMString? elementNs = "",
 *                                 optional DOMString? attrNs = "");
 *     DOMString? getPropertyType(DOMString tagName, DOMString property, optional DOMString? elementNs = "");
 *     readonly attribute TrustedTypePolicy? defaultPolicy;
 *   };
 *   partial interface mixin WindowOrWorkerGlobalScope { readonly attribute TrustedTypePolicyFactory trustedTypes; };
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"
#include "core/html/trusted_type.h"
#include "core/html/trusted_type_policy.h"
#include "core/html/trusted_type_policy_factory.h"
#include "core/html/trusted_types.h"
#include "solver/concolic.h"

#define TT_NS_HTML "http://www.w3.org/1999/xhtml"

static JSClassID g_class;
static JSClassID g_obj_slot = JS_INVALID_CLASS_ID;   /* this realm's factory */
static JSValue   g_slots_key = JS_UNDEFINED;
static JSAtom    g_slots_atom = JS_ATOM_NULL;
static int       g_id_create_policy = -1, g_id_get_attribute_type = -1, g_id_get_property_type = -1;
static int       g_id_is[3] = { -1, -1, -1 };

static bool ttpf_is(JSValueConst v)
{
    return JS_IsObject(v) && JS_GetClassID(v) == g_class;
}

/* The factory is built with the realm, before any flow forks, so createPolicy's writes to its record are heap
   writes over a shared baseline and each flow holds its own policies. emptyHTML and emptyScript are built
   once with it and answer the same object on every read.
   The record: "defaultPolicy", "createdPolicyNames" (a null-prototype object keyed by name, the
   ordered set's membership), "emptyHTML" and "emptyScript". OWNED. */
static JSValue ttpf_slots(JSContext *ctx, JSValueConst factory)
{
    JSValue rec = JS_UNDEFINED;
    int r;

    DCHECK(ttpf_is(factory), "a factory's record was read off something that is not a TrustedTypePolicyFactory");
    r = JS_GetOwnSlot(ctx, &rec, factory, g_slots_atom);
    DCHECK(r == 1 && JS_IsObject(rec), "a TrustedTypePolicyFactory has no record, which its realm install sets");
    return rec;
}

static JSValue ttpf_field(JSContext *ctx, JSValueConst factory, const char *field)
{
    JSValue rec = ttpf_slots(ctx, factory), v = JS_GetPropertyStr(ctx, rec, field);

    JS_FreeValue(ctx, rec);
    DCHECK(!JS_IsUndefined(v), "a TrustedTypePolicyFactory record lacks a field its realm install writes");
    return v;
}

JSValue trusted_type_policy_factory_default_policy(JSContext *ctx)
{
    JSValue factory = realm_value_get(ctx, g_obj_slot), policy;

    policy = ttpf_field(ctx, factory, "defaultPolicy");
    JS_FreeValue(ctx, factory);
    DCHECK(JS_IsNull(policy) || trusted_type_policy_is(policy),
           "a factory's default policy is neither null nor a TrustedTypePolicy");
    return policy;
}

/* ---- createPolicy: §3.1 Create a Trusted Type Policy -------------------------------------------------------- */

/* `dictionary TrustedTypePolicyOptions`, in Web IDL §3.2.17's sorted read order. Its fourth member,
   createParserOptions, is trusted_type_policy.c's named residual. */
static const IdlDictMember POLICY_OPTIONS[] = {
    { "createHTML",      IDL_CALLBACK },
    { "createScript",    IDL_CALLBACK },
    { "createScriptURL", IDL_CALLBACK },
};

static JSValue js_ttpf_create_policy(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                     int magic)
{
    JSValueConst name = argv[0], options = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue rec, names, policy, existing;
    JSAtom atom;
    const char *cname;
    size_t len = 0;
    bool created, blocked, is_default;

    (void)magic;
    if (concolic_is(name)) {
        DFAIL("createPolicy was given a policy name that is unknown external input — §4.2.5 compares it against "
              "the trusted-types allowlist: fork the two arms, a listed name and an unlisted one");
        return JS_ThrowTypeError(ctx, "a Trusted Type policy name that is not known cannot be checked");
    }
    DCHECK(ttpf_is(this_val) && JS_IsString(name),
           "createPolicy ran past idl_this_iface's brand or its DOMString conversion");
    rec = ttpf_slots(ctx, this_val);
    names = JS_GetPropertyStr(ctx, rec, "createdPolicyNames");
    atom = JS_ValueToAtom(ctx, name);
    CHECK(atom != JS_ATOM_NULL, "a policy name could not be interned");
    existing = JS_UNDEFINED;
    created = JS_GetOwnSlot(ctx, &existing, names, atom) == 1;
    JS_FreeValue(ctx, existing);
    cname = JS_ToCStringLen(ctx, &len, name);
    CHECK(cname != NULL, "a policy name could not be read as bytes");
    blocked = trusted_types_policy_creation_blocked(ctx, cname, len, created);  /* step 1 */
    is_default = len == 7 && memcmp(cname, "default", 7) == 0;
    policy = JS_UNDEFINED;
    if (blocked) {                                                              /* step 2 */
        JS_ThrowTypeError(ctx, "Policy \"%s\" disallowed.", cname);
        policy = JS_EXCEPTION;
    } else if (is_default) {                                                    /* step 3 */
        JSValue dflt = JS_GetPropertyStr(ctx, rec, "defaultPolicy");

        if (!JS_IsNull(dflt)) {
            JS_ThrowTypeError(ctx, "Policy with name \"default\" already exists.");
            policy = JS_EXCEPTION;
        }
        JS_FreeValue(ctx, dflt);
    }
    JS_FreeCString(ctx, cname);
    if (!JS_IsException(policy)) {
        policy = trusted_type_policy_create(ctx, name, options);               /* steps 4-6 */
        if (is_default)                                                         /* step 7 */
            CHECK(JS_DefinePropertyValueStr(ctx, rec, "defaultPolicy", JS_DupValue(ctx, policy),
                                            JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE) >= 0,
                  "a factory's default policy could not be stored");
        CHECK(JS_DefinePropertyValue(ctx, names, atom, JS_TRUE, JS_PROP_C_W_E) >= 0,   /* step 8 */
              "a factory's created policy names could not be extended");
    }
    JS_FreeAtom(ctx, atom);
    JS_FreeValue(ctx, names);
    JS_FreeValue(ctx, rec);
    return policy;                                                              /* step 9 */
}

/* ---- the is* members, the two empty values and defaultPolicy ------------------------------------------------- */

/* "Returns true if value is an instance of [the type] and has an associated data value set". */
static JSValue js_ttpf_is(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)ctx; (void)this_val; (void)argc;
    return JS_NewBool(ctx, trusted_type_is(argv[0], (TrustedTypeKind)magic));
}

enum { GET_EMPTY_HTML = 0, GET_EMPTY_SCRIPT, GET_DEFAULT_POLICY };
static const char *const TTPF_GETTER_FIELD[] = { "emptyHTML", "emptyScript", "defaultPolicy" };

static JSValue ttpf_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    return ttpf_field(ctx, this_val, TTPF_GETTER_FIELD[magic]);
}

/* ---- getAttributeType / getPropertyType ---------------------------------------------------------------------- */

/* A converted DOMString argument's bytes, ASCII-lowercased when asked. NULL for a null `DOMString?`. Unknown
   input has no bytes to look up, so it is asserted. Free with free(). */
static char *ttpf_arg_bytes(JSContext *ctx, JSValueConst v, bool lower)
{
    const char *p;
    char *out;
    size_t len = 0, i;

    if (JS_IsNull(v)) return NULL;
    if (concolic_is(v)) {
        DFAIL("a Trusted Types type lookup was given unknown external input — fork over the table's rows, one "
              "arm per row and one for no row");
        return NULL;
    }
    p = JS_ToCStringLen(ctx, &len, v);
    CHECK(p != NULL, "a converted DOMString argument could not be read as bytes");
    out = malloc(len + 1);
    CHECK(out != NULL, "a Trusted Types lookup argument could not be copied");
    for (i = 0; i < len; i++)
        out[i] = (lower && p[i] >= 'A' && p[i] <= 'Z') ? (char)(p[i] - 'A' + 'a') : p[i];
    out[len] = 0;
    JS_FreeCString(ctx, p);
    return out;
}

/* "If elementNs is null or an empty string, set elementNs to HTML namespace." */
static const char *ttpf_element_ns(const char *ns)
{
    return (!ns || !*ns) ? TT_NS_HTML : ns;
}

static JSValue ttpf_type_name(JSContext *ctx, bool found, TrustedTypeKind kind)
{
    return found ? JS_NewString(ctx, trusted_type_name(kind)) : JS_NULL;
}

/* §2.3.2 getAttributeType's steps: lowercase both names, default the namespaces, and return the fourth member
   of §3.10's attribute data, or null. The (namespace, local name) pair is the element interface the table's
   first column names. */
static JSValue js_ttpf_get_attribute_type(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                          int magic)
{
    char *tag = ttpf_arg_bytes(ctx, argv[0], true), *attr = ttpf_arg_bytes(ctx, argv[1], true);
    char *ens = ttpf_arg_bytes(ctx, argv[2], false), *ans = ttpf_arg_bytes(ctx, argv[3], false);
    TrustedTypeKind kind = TRUSTED_TYPE_HTML;
    char sink[96];
    bool found = false;

    (void)this_val; (void)argc; (void)magic;
    if (tag && attr)
        found = trusted_types_attribute_data(ttpf_element_ns(ens), tag, (ans && *ans) ? ans : NULL, attr, &kind,
                                             sink, sizeof sink);
    free(tag); free(attr); free(ens); free(ans);
    return ttpf_type_name(ctx, found, kind);
}

/* §2.3.2 getPropertyType's steps over its table. `property` is compared as given: the steps lowercase only
   the tag name. */
static JSValue js_ttpf_get_property_type(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                         int magic)
{
    static const struct { const char *local, *property; TrustedTypeKind kind; } ROWS[] = {
        { "iframe", "srcdoc",      TRUSTED_TYPE_HTML },
        { "script", "innerText",   TRUSTED_TYPE_SCRIPT },
        { "script", "src",         TRUSTED_TYPE_SCRIPT_URL },
        { "script", "text",        TRUSTED_TYPE_SCRIPT },
        { "script", "textContent", TRUSTED_TYPE_SCRIPT },
        { NULL,     "innerHTML",   TRUSTED_TYPE_HTML },    /* "*" */
        { NULL,     "outerHTML",   TRUSTED_TYPE_HTML },    /* "*" */
    };
    char *tag = ttpf_arg_bytes(ctx, argv[0], true), *prop = ttpf_arg_bytes(ctx, argv[1], false);
    char *ens = ttpf_arg_bytes(ctx, argv[2], false);
    TrustedTypeKind kind = TRUSTED_TYPE_HTML;
    bool found = false, html = strcmp(ttpf_element_ns(ens), TT_NS_HTML) == 0;
    size_t i;

    (void)this_val; (void)argc; (void)magic;
    for (i = 0; tag && prop && i < COUNTOF(ROWS) && !found; i++) {
        /* HTMLIFrameElement and HTMLScriptElement are exactly those local names in the HTML namespace. */
        if (ROWS[i].local && (!html || strcmp(tag, ROWS[i].local) != 0)) continue;
        if (strcmp(prop, ROWS[i].property) != 0) continue;
        kind = ROWS[i].kind;
        found = true;
    }
    free(tag); free(prop); free(ens);
    return ttpf_type_name(ctx, found, kind);
}

/* ---- the per-realm factory and `trustedTypes` ---------------------------------------------------------------- */

/* §4.1.1: "return this's relevant global object's trusted type policy factory". */
static JSValue ttpf_get_trusted_types(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)this_val; (void)magic;
    return realm_value_get(ctx, g_obj_slot);
}

static void trusted_type_policy_factory_install_realm(JSContext *ctx)
{
    static const char *const IS_NAME[3] = { "isHTML", "isScript", "isScriptURL" };
    JSValue prev = JS_GetClassProto(ctx, g_class), proto, global, factory, rec, names, target;
    int k;

    DCHECK(JS_IsNull(prev), "trusted_type_policy_factory_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "TrustedTypePolicyFactory.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "TrustedTypePolicyFactory");
    idl_install_method(ctx, proto, "createPolicy", g_id_create_policy);
    for (k = 0; k < 3; k++)
        idl_install_method(ctx, proto, IS_NAME[k], g_id_is[k]);
    idl_install_accessor_this(ctx, proto, "emptyHTML", ttpf_get, GET_EMPTY_HTML, -1, ttpf_is,
                              "TrustedTypePolicyFactory");
    idl_install_accessor_this(ctx, proto, "emptyScript", ttpf_get, GET_EMPTY_SCRIPT, -1, ttpf_is,
                              "TrustedTypePolicyFactory");
    idl_install_method(ctx, proto, "getAttributeType", g_id_get_attribute_type);
    idl_install_method(ctx, proto, "getPropertyType", g_id_get_property_type);
    idl_install_accessor_this(ctx, proto, "defaultPolicy", ttpf_get, GET_DEFAULT_POLICY, -1, ttpf_is,
                              "TrustedTypePolicyFactory");
    JS_SetClassProto(ctx, g_class, JS_DupValue(ctx, proto));
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "TrustedTypePolicyFactory",
                                         idl_interface_object(ctx, "TrustedTypePolicyFactory", proto));

    /* NAMED RESIDUAL. Not covered: the Worker half of `trustedTypes`' [Exposed=(Window,Worker)] — a worker
       global gets no factory and no attribute, because its CSP list, which createPolicy and every sink read,
       is not built. Next diff: the WorkerGlobalScope's policy container from its script's response, as HTML's
       run a worker steps set it, then this install in worker realms. How its absence shows: a worker that
       feature-detects `self.trustedTypes` takes its fallback. */
    if (realm_global_is_worker(ctx)) {
        JS_FreeValue(ctx, proto);
        JS_FreeValue(ctx, global);
        return;
    }
    /* HTML's "Window and Worker objects have a trusted type policy factory" (§4.1): one per global, built
       with the realm so every flow forks from the same object. Default policy null, created names « ». */
    factory = JS_NewObjectProtoClass(ctx, proto, g_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(factory), "this realm's TrustedTypePolicyFactory could not be allocated");
    rec = idl_slots_new(ctx);
    names = idl_slots_new(ctx);
    CHECK(!JS_IsException(rec) && !JS_IsException(names), "a TrustedTypePolicyFactory record could not be allocated");
    CHECK(JS_DefinePropertyValueStr(ctx, rec, "defaultPolicy", JS_NULL, JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE) >= 0
          && JS_DefinePropertyValueStr(ctx, rec, "createdPolicyNames", names, 0) >= 0
          && JS_DefinePropertyValueStr(ctx, rec, "emptyHTML",
                                       trusted_type_create(ctx, TRUSTED_TYPE_HTML, JS_NewString(ctx, "")), 0) >= 0
          && JS_DefinePropertyValueStr(ctx, rec, "emptyScript",
                                       trusted_type_create(ctx, TRUSTED_TYPE_SCRIPT, JS_NewString(ctx, "")), 0) >= 0
          && JS_DefinePropertyValue(ctx, factory, g_slots_atom, rec, 0) >= 0,
          "a TrustedTypePolicyFactory's record could not be stored");
    realm_value_set(ctx, g_obj_slot, factory);

    target = idl_global_member_target(ctx, global, "trustedTypes");
    idl_install_accessor(ctx, target, "trustedTypes", ttpf_get_trusted_types, 0, -1);
    JS_FreeValue(ctx, target);
    JS_FreeValue(ctx, global);
}

void trusted_type_policy_factory_init(JSContext *ctx)
{
    static const IdlArgType CREATE_POLICY_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
    static const IdlArgType IS_ARGS[1] = { IDL_ANY };
    static const IdlArgType ATTR_ARGS[4] = { IDL_DOMSTRING, IDL_DOMSTRING, IDL_DOMSTRING_NULLABLE,
                                             IDL_DOMSTRING_NULLABLE };
    static const IdlArgType PROP_ARGS[3] = { IDL_DOMSTRING, IDL_DOMSTRING, IDL_DOMSTRING_NULLABLE };
    JSClassDef d = { "TrustedTypePolicyFactory" };
    int k;

    DCHECK(g_class == 0, "trusted_type_policy_factory_init ran twice — the class and the members are the agent's");
    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_class, &d) == 0,
          "the TrustedTypePolicyFactory class could not be declared");
    agent_state_class("trusted_types", &g_class, "Trusted Types §2.3.2's TrustedTypePolicyFactory class");
    g_obj_slot = realm_value_declare(ctx, "Trusted Types §4.1 this global's trusted type policy factory");
    agent_state_realm_slot("trusted_types", &g_obj_slot, "Trusted Types §4.1's per-realm factory slot");

    g_id_create_policy = idl_method_id_dict(ctx, CREATE_POLICY_ARGS, 2, POLICY_OPTIONS,
                                            (int)COUNTOF(POLICY_OPTIONS), js_ttpf_create_policy, 0);
    idl_optional_from(1);
    idl_this_iface(ttpf_is, "TrustedTypePolicyFactory");
    agent_state_id("trusted_types", &g_id_create_policy, "Trusted Types §2.3.2's createPolicy");
    for (k = 0; k < 3; k++) {
        g_id_is[k] = idl_method_id(ctx, IS_ARGS, 1, js_ttpf_is, k);
        idl_this_iface(ttpf_is, "TrustedTypePolicyFactory");
        agent_state_id("trusted_types", &g_id_is[k], "Trusted Types §2.3.2's is* member");
    }
    g_id_get_attribute_type = idl_method_id(ctx, ATTR_ARGS, 4, js_ttpf_get_attribute_type, 0);
    idl_optional_from(2);
    idl_arg_default(2, IDL_DEFAULT_STRING, "");
    idl_arg_default(3, IDL_DEFAULT_STRING, "");
    idl_this_iface(ttpf_is, "TrustedTypePolicyFactory");
    agent_state_id("trusted_types", &g_id_get_attribute_type, "Trusted Types §2.3.2's getAttributeType");
    g_id_get_property_type = idl_method_id(ctx, PROP_ARGS, 3, js_ttpf_get_property_type, 0);
    idl_optional_from(2);
    idl_arg_default(2, IDL_DEFAULT_STRING, "");
    idl_this_iface(ttpf_is, "TrustedTypePolicyFactory");
    agent_state_id("trusted_types", &g_id_get_property_type, "Trusted Types §2.3.2's getPropertyType");

    g_slots_key = JS_NewSymbol(ctx, "trustedTypePolicyFactory", false);
    CHECK(!JS_IsException(g_slots_key), "the TrustedTypePolicyFactory record key could not be allocated");
    g_slots_atom = JS_ValueToAtom(ctx, g_slots_key);
    CHECK(g_slots_atom != JS_ATOM_NULL, "the TrustedTypePolicyFactory record key could not be interned");
    realm_declare_intrinsic(trusted_type_policy_factory_install_realm);
}

void trusted_type_policy_factory_free(JSRuntime *rt)
{
    int k;

    JS_FreeAtomRT(rt, g_slots_atom);
    g_slots_atom = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_slots_key);
    g_slots_key = JS_UNDEFINED;
    g_class = 0;
    g_obj_slot = JS_INVALID_CLASS_ID;
    g_id_create_policy = g_id_get_attribute_type = g_id_get_property_type = -1;
    for (k = 0; k < 3; k++) g_id_is[k] = -1;
}
