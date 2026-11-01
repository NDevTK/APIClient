/* HTML §8.1.6.2 HostEnsureCanCompileStrings over CSP §4.4.1 EnsureCSPDoesNotBlockStringCompilation — see
 * ensure_can_compile_strings.h.
 *
 * NAMED RESIDUAL. Not covered: HTML §8.1.6.3 HostGetCodeForEval, Dynamic Code Brand Checks' other hook, so
 * PerformEval returns a TrustedScript argument unevaluated and the Function constructors read one through its
 * toString. Next diff builds: the hook in quickjs's js_eval_program_source, answering a TrustedScript's data,
 * which also makes CSP §4.4.1 step 2's isTrusted reachable from an eval. How its absence shows: under
 * `require-trusted-types-for 'script'` an `eval(policy.createScript(s))` answers the object instead of running
 * the program. */
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/dom/document.h"
#include "core/frame/csp_violation.h"
#include "core/frame/policy_container.h"
#include "core/html/ensure_can_compile_strings.h"
#include "core/html/trusted_type.h"
#include "core/html/trusted_types.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/concolic.h"

static int g_id = -1;

/* The hook's arguments, in quickjs.h's order. */
enum { ECS_PARAM_STRINGS = 0, ECS_BODY_STRING, ECS_CODE_STRING, ECS_TYPE, ECS_PARAM_ARGS, ECS_BODY_ARG, ECS_ARGC };

bool ensure_csp_allows_string_compilation(JSContext *ctx, JSValueConst source)
{
    JSValue text;
    const char *bytes = NULL;
    size_t n = 0;
    bool allowed;

    if (JS_IsString(source)) {
        text = JS_DupValue(ctx, source);
    } else {
        DCHECK(concolic_is(source), "CSP §4.4.1 was handed a sourceString that is neither a String nor unknown "
                                    "external input");
        text = concolic_example(ctx, source);
    }
    if (JS_IsString(text)) {
        bytes = JS_ToCStringLen(ctx, &n, text);
        CHECK(bytes != NULL, "CSP §4.4.1's sourceString could not be read for its sample");
    }
    /* Step 4.2's "does sink type require trusted types?" with realm, 'script' and false is one answer for every
       policy of the list, so it is asked once. */
    allowed = policy_allows_string_compilation(csp_reporter(ctx), document_policy(ctx),
                                               trusted_types_required(ctx, TRUSTED_TYPE_SCRIPT, false), bytes, n);
    if (bytes)
        JS_FreeCString(ctx, bytes);
    JS_FreeValue(ctx, text);
    return allowed;
}

#define ECS_STAGES(X) \
    X(ECS_TRUSTED,  "CSP §4.4.1 EnsureCSPDoesNotBlockStringCompilation steps 1-2 (sourceString; compilationSink, " \
                    "isTrusted, and the get trusted type compliant string algorithm with TrustedScript, realm, " \
                    "codeString, compilationSink and 'script', whose default policy's createScript is the page's " \
                    "code; its throw and a changed string are EvalErrors)") \
    X(ECS_POLICIES, "CSP §4.4.1 steps 3-6 (each policy's script-src or default-src source list, " \
                    "'trusted-types-eval' and 'unsafe-eval', the violation reports, and the EvalError for " \
                    "\"Blocked\")")
enum { IDL_STEP_STAGE_BASE(ECS_STAGES) ECS_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const ECS_STEPS[] = { ECS_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    uint8_t started;
    TrustedTypesCompliantRun tt;   /* step 2.4's get trusted type compliant string */
    JSValue source;                /* sourceString (owned) */
} EcsState;

static void ecs_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    EcsState *s = st;

    trusted_types_compliant_run_visit(ctx, &s->tt, v);
    v->val(ctx, &s->source);
}

/* Step 2.3: isTrusted — bodyArg and every one of parameterArgs implement TrustedScript. The spec's asserts that
   each string equals its object's data hold only while the page leaves TrustedScript's toString alone, since
   that is how this engine's CreateDynamicFunction read them (see the residual above), so they are not asserted. */
static bool ecs_is_trusted(JSContext *ctx, JSValueConst param_args, JSValueConst body_arg)
{
    int64_t n = 0, i;
    bool trusted = trusted_type_is(body_arg, TRUSTED_TYPE_SCRIPT);

    CHECK(JS_GetLength(ctx, param_args, &n) == 0, "HostEnsureCanCompileStrings's parameterArgs has no length");
    for (i = 0; trusted && i < n; i++) {
        JSValue arg = JS_GetPropertyInt64(ctx, param_args, i);

        trusted = trusted_type_is(arg, TRUSTED_TYPE_SCRIPT);
        JS_FreeValue(ctx, arg);
    }
    return trusted;
}

static int ecs_body(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv, JSValue cb_result,
                    JSValue *presult, JSValue **out_cb, int *out_argc)
{
    EcsState *s = st;
    JSValue out;
    int r;

    *presult = JS_UNDEFINED;
    if (hdr->stage == ECS_TRUSTED) {
        int type;
        const char *sink;

        DCHECK(argc == ECS_ARGC && JS_IsString(argv[ECS_CODE_STRING]) && JS_IsArray(argv[ECS_PARAM_ARGS])
                   && JS_VALUE_GET_TAG(argv[ECS_TYPE]) == JS_TAG_INT,
               "HostEnsureCanCompileStrings was called with operands other than quickjs.h's six");
        type = JS_VALUE_GET_INT(argv[ECS_TYPE]);
        DCHECK(type >= JS_COMPILATION_DIRECT_EVAL && type <= JS_COMPILATION_FUNCTION,
               "HostEnsureCanCompileStrings was called with a compilationType quickjs.h does not define");
        if (!s->started) {
            s->started = 1;
            trusted_types_compliant_run_init(&s->tt);
            s->source = JS_UNDEFINED;
            /* Steps 1 and 2.3: sourceString is codeString unless step 2.4 replaces it. */
            if (ecs_is_trusted(ctx, argv[ECS_PARAM_ARGS], argv[ECS_BODY_ARG])) {
                JS_FreeValue(ctx, cb_result);
                s->source = JS_DupValue(ctx, argv[ECS_CODE_STRING]);
                STEP_GOTO(hdr->stage, ECS_POLICIES, &s->tt.phase, NULL);
                goto policies;
            }
        }
        /* Step 2.1's compilationSink is "Function" for a Function constructor and "eval" otherwise. */
        sink = type == JS_COMPILATION_FUNCTION ? "Function" : "eval";
        r = trusted_types_compliant_string_run(ctx, hdr, &s->tt, TRUSTED_TYPE_SCRIPT, argv[ECS_CODE_STRING], sink,
                                               cb_result, &out, out_cb, out_argc);
        if (r > 0)
            return r;
        if (r < 0) {
            /* Step 2.4.2: "If the algorithm throws an error, throw an EvalError." */
            JS_FreeValue(ctx, JS_GetException(ctx));
            JS_ThrowEvalError(ctx, "Evaluating a string as JavaScript violates this document's Trusted Type "
                                   "assignment requirements.");
            return JS_STEP_ABRUPT;
        }
        if (concolic_is(out)) {
            /* The release arm keeps the codeString, which is the answer for the policy that returned it intact. */
            DFAIL("CSP §4.4.1 step 2.4.3 compared a default policy's createScript answer that is unknown external "
                  "input with codeString: build the comparison as a fork over whether the two are equal");
            JS_FreeValue(ctx, out);
            out = JS_DupValue(ctx, argv[ECS_CODE_STRING]);
        }
        /* Step 2.4.3: "If sourceString is not equal to codeString, throw an EvalError." */
        if (!JS_IsStrictEqual(ctx, out, argv[ECS_CODE_STRING])) {
            JS_FreeValue(ctx, out);
            JS_ThrowEvalError(ctx, "Evaluating a string as JavaScript violates this document's Trusted Type "
                                   "assignment requirements.");
            return JS_STEP_ABRUPT;
        }
        s->source = out;
        STEP_GOTO(hdr->stage, ECS_POLICIES, &s->tt.phase, NULL);
    } else {
        JS_FreeValue(ctx, cb_result);
    }
 policies:
    DCHECK(hdr->stage == ECS_POLICIES, "HostEnsureCanCompileStrings resumed at a stage CSP §4.4.1 does not have");
    /* Step 6: "If result is "Blocked", throw an EvalError exception." */
    if (!ensure_csp_allows_string_compilation(ctx, s->source)) {
        JS_ThrowEvalError(ctx, "Refused to evaluate a string as JavaScript because 'unsafe-eval' is not an "
                               "allowed source of script in this document's Content Security Policy.");
        return JS_STEP_ABRUPT;
    }
    return JS_STEP_DONE;
}

static const IdlStepDecl ECS_DECL = {
    ecs_body, sizeof(EcsState), ecs_visit, NULL,
    "HTML §8.1.6.2 HostEnsureCanCompileStrings (CSP §4.4.1 EnsureCSPDoesNotBlockStringCompilation)", ECS_STEPS,
    /* Step 2.4.2 turns the default policy's throw into an EvalError, so the body takes it. */
    .catches_abrupt = 1,
};

static void ensure_can_compile_strings_install_realm(JSContext *ctx)
{
    JSValue fn = idl_step_function(ctx, "HostEnsureCanCompileStrings", g_id);

    CHECK(!JS_IsException(fn), "a realm's HostEnsureCanCompileStrings could not be allocated");
    JS_SetHostEnsureCanCompileStrings(ctx, fn);
    JS_FreeValue(ctx, fn);
}

void ensure_can_compile_strings_init(JSContext *ctx)
{
    static const IdlArgType ARGS[ECS_ARGC] = { IDL_ANY, IDL_ANY, IDL_ANY, IDL_ANY, IDL_ANY, IDL_ANY };

    DCHECK(g_id < 0, "ensure_can_compile_strings_init ran twice — the machine is the agent's");
    g_id = idl_method_id_step(ctx, ARGS, ECS_ARGC, NULL, 0, &ECS_DECL, 0);
    agent_state_id("compile_strings", &g_id, "HTML §8.1.6.2 HostEnsureCanCompileStrings's machine");
    realm_declare_intrinsic(ensure_can_compile_strings_install_realm);
}

void ensure_can_compile_strings_free(JSRuntime *rt)
{
    (void)rt;
    g_id = -1;
}
