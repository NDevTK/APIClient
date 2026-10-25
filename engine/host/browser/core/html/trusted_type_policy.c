/* Trusted Types §2.3.3 TrustedTypePolicy — see trusted_type_policy.h.
 *
 *   [Exposed=(Window,Worker)] interface TrustedTypePolicy {
 *     readonly attribute DOMString name;
 *     TrustedHTML createHTML(DOMString input, any... arguments);
 *     TrustedScript createScript(DOMString input, any... arguments);
 *     TrustedScriptURL createScriptURL(DOMString input, any... arguments);
 *   };
 *
 * The policy's name and options are a null-prototype record in an own slot, written once at creation by the
 * flow that creates the policy. */
#include <stdbool.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"
#include "core/html/trusted_type.h"
#include "core/html/trusted_type_policy.h"
#include "solver/concolic.h"

static JSClassID g_class;
static JSValue   g_slots_key = JS_UNDEFINED;
static JSAtom    g_slots_atom = JS_ATOM_NULL;
static int       g_id_create[3] = { -1, -1, -1 };

/* §3.3 step 1's table, indexed by TrustedTypeKind.
   NAMED RESIDUAL. Not covered: the editor's draft's fourth member `createParserOptions`, its options member and
   §2.2.4's TrustedHTMLParserOptions; §3.1 step 6 stores three callbacks. Next diff: §2.2.4's interface and §3.6
   Get Trusted Type compliant parser options over the sinks taking SetHTMLUnsafeOptions. How its absence shows:
   a policy's `createParserOptions` reads as undefined, and createPolicy never runs an options dictionary's
   `createParserOptions` getter. */
static const char *const TTP_FUNCTION_NAME[3] = { "createHTML", "createScript", "createScriptURL" };

bool trusted_type_policy_is(JSValueConst v)
{
    return JS_IsObject(v) && JS_GetClassID(v) == g_class;
}

static JSValue ttp_slots(JSContext *ctx, JSValueConst policy)
{
    JSValue rec = JS_UNDEFINED;
    int r;

    DCHECK(trusted_type_policy_is(policy), "a policy's options were read off something that is not a policy");
    r = JS_GetOwnSlot(ctx, &rec, policy, g_slots_atom);
    DCHECK(r == 1 && JS_IsObject(rec),
           "a TrustedTypePolicy has no name and options record, which trusted_type_policy_create sets on every "
           "instance");
    return rec;
}

JSValue trusted_type_policy_create(JSContext *ctx, JSValueConst name, JSValueConst options)
{
    JSValue proto, obj, rec;
    int k;

    DCHECK(JS_IsString(name), "a TrustedTypePolicy was created with a name that is not a String");
    proto = JS_GetClassProto(ctx, g_class);
    DCHECK(JS_IsObject(proto), "a TrustedTypePolicy was created in a realm whose install never ran");
    obj = JS_NewObjectProtoClass(ctx, proto, g_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "a TrustedTypePolicy could not be allocated");
    rec = idl_slots_new(ctx);
    CHECK(!JS_IsException(rec), "a TrustedTypePolicy's options record could not be allocated");
    /* Step 5: the policy's name. Step 6: its options, "createHTML" -> options["createHTML"] and the rest. */
    CHECK(JS_DefinePropertyValueStr(ctx, rec, "name", JS_DupValue(ctx, name), 0) >= 0,
          "a TrustedTypePolicy's name could not be stored");
    for (k = 0; k < 3; k++) {
        JSValue fn = idl_dict_get(ctx, options, TTP_FUNCTION_NAME[k]);

        DCHECK(JS_IsUndefined(fn) || JS_IsFunction(ctx, fn) || concolic_is(fn),
               "a TrustedTypePolicyOptions member reached §3.1 unconverted — the dictionary declares each a "
               "callback function type");
        CHECK(JS_DefinePropertyValueStr(ctx, rec, TTP_FUNCTION_NAME[k], fn, 0) >= 0,
              "a TrustedTypePolicy's option could not be stored");
    }
    CHECK(JS_DefinePropertyValue(ctx, obj, g_slots_atom, rec, 0) >= 0,
          "a TrustedTypePolicy's record could not be stored");
    return obj;
}

JSValue trusted_type_policy_function(JSContext *ctx, JSValueConst policy, TrustedTypeKind kind)
{
    JSValue rec, fn;

    DCHECK((int)kind >= 0 && (int)kind < 3, "a policy's function was asked for a kind §3.3's table lacks");
    rec = ttp_slots(ctx, policy);
    fn = JS_GetPropertyStr(ctx, rec, TTP_FUNCTION_NAME[kind]);
    JS_FreeValue(ctx, rec);
    return fn;
}

/* `readonly attribute DOMString name`. */
static JSValue ttp_get_name(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue rec = ttp_slots(ctx, this_val), name;

    (void)magic;
    name = JS_GetPropertyStr(ctx, rec, "name");
    JS_FreeValue(ctx, rec);
    return name;
}

/* ---- createHTML / createScript / createScriptURL: §3.2 over §3.3 ---------------------------------------- */

#define TTP_STAGES(X) \
    X(TTP_FUNCTION, "Trusted Types §3.3 Get Trusted Type policy value steps 1-6 (the function name, " \
                    "options[functionName], the missing-function TypeError, and args « value » with arguments " \
                    "appended) — O(1) engine actions over the record the policy carries") \
    X(TTP_INVOKE,   "Trusted Types §3.3 step 7 (invoking function with args and \"rethrow\" — Web IDL §3.12 " \
                    "Invoking callback functions' Call)") \
    X(TTP_CREATE,   "Trusted Types §3.2 Create a Trusted Type steps 4-6 (Web IDL §3.12's conversion of the " \
                    "result to the callback's DOMString? or USVString?, the empty string for null, and the new " \
                    "instance)")
enum { IDL_STEP_STAGE_BASE(TTP_STAGES) TTP_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const TTP_STEPS[] = { TTP_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    uint8_t phase;    /* step_call_run's cursor */
    JSValue call;     /* the policy's function bound over « value » + arguments (owned) */
    JSValue value;    /* policyValue, the callback's result before conversion (owned) */
    JSValue data;     /* dataString (owned) */
    JSValue cb[2];    /* step_call_run's buffer: [this, call] — the arguments travel in the bound call */
} TtpState;

static void ttp_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    TtpState *s = st;
    int i;

    v->val(ctx, &s->call);
    v->val(ctx, &s->value);
    v->val(ctx, &s->data);
    STEP_CB_FOREACH(s->cb, i) v->val(ctx, &s->cb[i]);
}

static int ttp_body(JSContext *ctx, JSStepHdr *hdr, void *state, int argc, JSValueConst *argv,
                    JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    TtpState *s = state;
    TrustedTypeKind kind = (TrustedTypeKind)idl_step_magic(hdr);
    JSValue out;
    int r, i;

    if (hdr->stage == TTP_FUNCTION) {
        JSValue fn;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->call = s->value = s->data = JS_UNDEFINED;
        STEP_CB_FOREACH(s->cb, i) s->cb[i] = JS_UNDEFINED;
        DCHECK(trusted_type_policy_is(hdr->this_val) && argc >= 1,
               "a create* member ran past idl_this_iface's brand or the arity check of its required input");
        fn = trusted_type_policy_function(ctx, hdr->this_val, kind);              /* steps 1-2 */
        if (JS_IsUndefined(fn)) {                                                 /* step 3.1 */
            JSValue name = ttp_get_name(ctx, hdr->this_val, 0);
            const char *n = JS_ToCString(ctx, name);

            CHECK(n != NULL, "a TrustedTypePolicy's name could not be read back");
            JS_ThrowTypeError(ctx, "Policy %s's TrustedTypePolicyOptions did not specify a '%s' member.", n,
                              TTP_FUNCTION_NAME[kind]);
            JS_FreeCString(ctx, n);
            JS_FreeValue(ctx, name);
            return JS_STEP_ABRUPT;
        }
        s->call = JS_NewBoundCall(ctx, fn, JS_UNDEFINED, argc, argv);           /* steps 5-6 */
        JS_FreeValue(ctx, fn);
        CHECK(!JS_IsException(s->call), "a policy callback's argument list could not be allocated");
        STEP_GOTO(hdr->stage, TTP_INVOKE, &s->phase, NULL);
    }
    if (hdr->stage == TTP_INVOKE) {
        r = step_call_run(ctx, &s->phase, STEP_CB(s->cb), s->call, JS_UNDEFINED, 0, NULL, cb_result, &out,
                          out_cb, out_argc);
        if (r > 0) return r;
        cb_result = JS_UNDEFINED;
        if (JS_IsException(out)) return JS_STEP_ABRUPT;
        s->value = out;
        STEP_GOTO(hdr->stage, TTP_CREATE, &s->phase, NULL);
    }
    DCHECK(hdr->stage == TTP_CREATE, "a create* member resumed at a stage §3.2 and §3.3 do not have");
    if (JS_IsNull(s->value) || JS_IsUndefined(s->value)) {
        /* Web IDL §3.2's nullable conversion gives null for both, and §3.2 step 5 makes that the empty string. */
        JS_FreeValue(ctx, cb_result);
        s->data = JS_NewString(ctx, "");
        CHECK(!JS_IsException(s->data), "the empty string could not be allocated");
    } else if (concolic_is(s->value)) {
        /* Unknown external input crosses a DOMString conversion as itself, as at every IDL argument. */
        JS_FreeValue(ctx, cb_result);
        s->data = JS_DupValue(ctx, s->value);
    } else {
        r = step_tostring_run(ctx, hdr, s->value, cb_result, &s->data, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return JS_STEP_ABRUPT;
        if (kind == TRUSTED_TYPE_SCRIPT_URL) {
            /* CreateScriptURLCallback returns `USVString?`: Web IDL §3.2.12's scalar value conversion. */
            s->data = JS_ToScalarValueString(ctx, s->data);
            if (JS_IsException(s->data)) { s->data = JS_UNDEFINED; return JS_STEP_ABRUPT; }
        }
    }
    *presult = trusted_type_create(ctx, kind, s->data);                        /* step 6 */
    s->data = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static const IdlStepDecl TTP_DECL = {
    ttp_body, sizeof(TtpState), ttp_visit, NULL,
    "Trusted Types §2.3.3 TrustedTypePolicy's createHTML/createScript/createScriptURL (§3.2 Create a Trusted "
    "Type)", TTP_STEPS
};

static void trusted_type_policy_install_realm(JSContext *ctx)
{
    JSValue prev = JS_GetClassProto(ctx, g_class), proto, global;
    int k;

    DCHECK(JS_IsNull(prev), "trusted_type_policy_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "TrustedTypePolicy.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "TrustedTypePolicy");
    idl_install_accessor_this(ctx, proto, "name", ttp_get_name, 0, -1, trusted_type_policy_is,
                              "TrustedTypePolicy");
    for (k = 0; k < 3; k++)
        idl_install_method(ctx, proto, TTP_FUNCTION_NAME[k], g_id_create[k]);
    JS_SetClassProto(ctx, g_class, JS_DupValue(ctx, proto));
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "TrustedTypePolicy",
                                         idl_interface_object(ctx, "TrustedTypePolicy", proto));
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, proto);
}

void trusted_type_policy_init(JSContext *ctx)
{
    /* `DOMString input, any... arguments`: the tail repeats the last declared type, so it is declared `any`. */
    static const IdlArgType CREATE_ARGS[2] = { IDL_DOMSTRING, IDL_ANY };
    JSClassDef d = { "TrustedTypePolicy" };
    int k;

    DCHECK(g_class == 0, "trusted_type_policy_init ran twice — the class and the members are the agent's");
    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_class, &d) == 0, "the TrustedTypePolicy class could not be declared");
    agent_state_class("trusted_types", &g_class, "Trusted Types §2.3.3's TrustedTypePolicy class");
    for (k = 0; k < 3; k++) {
        g_id_create[k] = idl_method_id_step(ctx, CREATE_ARGS, 2, NULL, 0, &TTP_DECL, k);
        idl_optional_from(1);
        idl_variadic();
        idl_this_iface(trusted_type_policy_is, "TrustedTypePolicy");
        agent_state_id("trusted_types", &g_id_create[k], "Trusted Types §2.3.3's create* member");
    }
    g_slots_key = JS_NewSymbol(ctx, "trustedTypePolicy", false);
    CHECK(!JS_IsException(g_slots_key), "the TrustedTypePolicy record key could not be allocated");
    g_slots_atom = JS_ValueToAtom(ctx, g_slots_key);
    CHECK(g_slots_atom != JS_ATOM_NULL, "the TrustedTypePolicy record key could not be interned");
    realm_declare_intrinsic(trusted_type_policy_install_realm);
}

void trusted_type_policy_free(JSRuntime *rt)
{
    int k;

    JS_FreeAtomRT(rt, g_slots_atom);
    g_slots_atom = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_slots_key);
    g_slots_key = JS_UNDEFINED;
    g_class = 0;
    for (k = 0; k < 3; k++) g_id_create[k] = -1;
}
