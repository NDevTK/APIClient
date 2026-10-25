/* Trusted Types §2.2.1 TrustedHTML, §2.2.2 TrustedScript, §2.2.3 TrustedScriptURL — see trusted_type.h.
 *
 *   [Exposed=(Window,Worker)] interface TrustedHTML { stringifier; DOMString toJSON(); };
 *
 * and the same for TrustedScript, and for TrustedScriptURL with a USVString toJSON. None declares a
 * constructor. The data is an own slot defined non-writable and non-configurable at creation; an object is
 * created by its own flow, so the slot is a flow-local write the heap delta does not need to capture. */
#include <stdbool.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/html/trusted_type.h"

#define TRUSTED_TYPE_KIND_N 3

static const char *const TT_NAME[TRUSTED_TYPE_KIND_N] = { "TrustedHTML", "TrustedScript", "TrustedScriptURL" };
static JSClassID g_class[TRUSTED_TYPE_KIND_N];
static JSValue   g_data_key = JS_UNDEFINED;
static JSAtom    g_data_atom = JS_ATOM_NULL;
static int       g_id_stringify[TRUSTED_TYPE_KIND_N] = { -1, -1, -1 };

static void tt_kind_check(TrustedTypeKind kind, const char *file, int line)
{
    DCHECKF((int)kind >= 0 && (int)kind < TRUSTED_TYPE_KIND_N,
            "%s:%d named a Trusted Type by a kind §2.2 does not define", file, line);
    (void)file; (void)line;
}
#define TT_KIND_CHECK(k) tt_kind_check((k), __FILE__, __LINE__)

const char *trusted_type_name(TrustedTypeKind kind)
{
    TT_KIND_CHECK(kind);
    return TT_NAME[kind];
}

JSClassID trusted_type_class(TrustedTypeKind kind)
{
    TT_KIND_CHECK(kind);
    DCHECK(g_class[kind] != 0, "a Trusted Type's class was asked for before trusted_type_init declared it");
    return g_class[kind];
}

bool trusted_type_is(JSValueConst v, TrustedTypeKind kind)
{
    TT_KIND_CHECK(kind);
    return JS_IsObject(v) && JS_GetClassID(v) == g_class[kind];
}

static bool tt_is_html(JSValueConst v)       { return trusted_type_is(v, TRUSTED_TYPE_HTML); }
static bool tt_is_script(JSValueConst v)     { return trusted_type_is(v, TRUSTED_TYPE_SCRIPT); }
static bool tt_is_script_url(JSValueConst v) { return trusted_type_is(v, TRUSTED_TYPE_SCRIPT_URL); }
static const IdlThisIs TT_IS[TRUSTED_TYPE_KIND_N] = { tt_is_html, tt_is_script, tt_is_script_url };

JSValue trusted_type_create(JSContext *ctx, TrustedTypeKind kind, JSValue data)
{
    JSValue proto, obj;

    TT_KIND_CHECK(kind);
    DCHECK(JS_IsString(data) || JS_IsObject(data),
           "a Trusted Type was created over data that is neither a String nor unknown external input — §3.2 "
           "stringifies the policy value before it creates the object");
    proto = JS_GetClassProto(ctx, g_class[kind]);
    DCHECK(JS_IsObject(proto), "a Trusted Type was created in a realm whose install never ran");
    obj = JS_NewObjectProtoClass(ctx, proto, g_class[kind]);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "a Trusted Type object could not be allocated");
    CHECK(JS_DefinePropertyValue(ctx, obj, g_data_atom, data, 0) >= 0,
          "a Trusted Type's associated data could not be stored");
    return obj;
}

JSValue trusted_type_data(JSContext *ctx, JSValueConst v, TrustedTypeKind kind)
{
    JSValue data = JS_UNDEFINED;
    int r;

    DCHECK(trusted_type_is(v, kind), "a Trusted Type's data was read off something that is not one");
    r = JS_GetOwnSlot(ctx, &data, v, g_data_atom);
    DCHECK(r == 1, "a Trusted Type has no associated data, which trusted_type_create sets on every instance");
    return data;
}

/* The stringification behavior steps and toJSON() method steps: "return the associated data value". The
   receiver is branded by idl_this_iface before this runs. */
static JSValue js_tt_stringify(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)argc; (void)argv;
    return trusted_type_data(ctx, this_val, (TrustedTypeKind)magic);
}

static void trusted_type_install_realm(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    int k;

    for (k = 0; k < TRUSTED_TYPE_KIND_N; k++) {
        JSValue prev = JS_GetClassProto(ctx, g_class[k]), proto;

        DCHECK(JS_IsNull(prev), "trusted_type_install_realm ran twice in one realm");
        JS_FreeValue(ctx, prev);
        proto = JS_NewObject(ctx);
        CHECK(!JS_IsException(proto), "a Trusted Type's interface prototype object could not be allocated");
        idl_interface_tag(ctx, proto, TT_NAME[k]);
        idl_install_method(ctx, proto, "toJSON", g_id_stringify[k]);
        idl_install_method(ctx, proto, "toString", g_id_stringify[k]);
        JS_SetClassProto(ctx, g_class[k], JS_DupValue(ctx, proto));
        idl_define_global_property_reference(ctx, global, TT_NAME[k], idl_interface_object(ctx, TT_NAME[k], proto));
        JS_FreeValue(ctx, proto);
    }
    JS_FreeValue(ctx, global);
}

void trusted_type_init(JSContext *ctx)
{
    int k;

    DCHECK(g_data_atom == JS_ATOM_NULL, "trusted_type_init ran twice — the classes and the data key are the agent's");
    for (k = 0; k < TRUSTED_TYPE_KIND_N; k++) {
        JSClassDef d = { TT_NAME[k] };

        JS_NewClassID(JS_GetRuntime(ctx), &g_class[k]);
        CHECK(JS_NewClass(JS_GetRuntime(ctx), g_class[k], &d) == 0, "a Trusted Type class could not be declared");
        agent_state_class("trusted_types", &g_class[k], "Trusted Types §2.2's class for one of its three types");
        g_id_stringify[k] = idl_method_id(ctx, NULL, 0, js_tt_stringify, k);
        idl_this_iface(TT_IS[k], TT_NAME[k]);
        agent_state_id("trusted_types", &g_id_stringify[k], "Trusted Types §2.2's stringifier and toJSON()");
    }
    g_data_key = JS_NewSymbol(ctx, "trustedTypeData", false);
    CHECK(!JS_IsException(g_data_key), "the Trusted Type data key could not be allocated");
    g_data_atom = JS_ValueToAtom(ctx, g_data_key);
    CHECK(g_data_atom != JS_ATOM_NULL, "the Trusted Type data key could not be interned");
    realm_declare_intrinsic(trusted_type_install_realm);
}

void trusted_type_free(JSRuntime *rt)
{
    int k;

    JS_FreeAtomRT(rt, g_data_atom);
    g_data_atom = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_data_key);
    g_data_key = JS_UNDEFINED;
    for (k = 0; k < TRUSTED_TYPE_KIND_N; k++) {
        g_class[k] = 0;
        g_id_stringify[k] = -1;
    }
}
