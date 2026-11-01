/* HTML §8.1.6.2 HostEnsureCanCompileStrings(realm, parameterStrings, bodyString, codeString, compilationType,
 * parameterArgs, bodyArg): "Perform ? EnsureCSPDoesNotBlockStringCompilation(realm, parameterStrings,
 * bodyString, codeString, compilationType, parameterArgs, bodyArg)", which is CSP §4.4.1.
 *
 * Every realm's engine hook (quickjs.h's JS_SetHostEnsureCanCompileStrings) is this component's machine, so
 * PerformEval, CreateDynamicFunction and the timers' string handlers reach one CSP §4.4.1. Step 2 runs Trusted
 * Types' get trusted type compliant string, whose default policy's createScript is the page's code, which is why
 * the hook is a step machine the engine calls on the trampoline. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_ENSURE_CAN_COMPILE_STRINGS_H
#define ENGINE_HOST_BROWSER_CORE_HTML_ENSURE_CAN_COMPILE_STRINGS_H

#include <stdbool.h>

#include "quickjs.h"

/* Declares the machine (an agent-wide step id) and the per-realm install of the hook. */
void ensure_can_compile_strings_init(JSContext *ctx);
void ensure_can_compile_strings_free(JSRuntime *rt);

/* CSP §4.4.1 steps 3-6 for this realm over sourceString `source` (a String, or unknown external input, whose
   example is the sample where it has one): each policy's verdict, its violation report, and whether the result
   is "Allowed". Step 1-2's Trusted Types half is the caller's; for compilationType "timer" step 2 does nothing,
   so HTML §8.7 Timers' step 1.5 is this call alone. */
bool ensure_csp_allows_string_compilation(JSContext *ctx, JSValueConst source);

#endif
