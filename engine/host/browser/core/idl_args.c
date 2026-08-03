/* THE WEB IDL ARGUMENT COERCION, AS ONE MACHINE.
 *
 * Nearly every DOM member this engine implements has the same shape: some of its arguments are DOMStrings, and
 * everything after the conversion touches only the component's own Lexbor tree, which the page cannot reach.
 * The conversion is the part that CAN run the page's code — `el.getAttribute({toString(){ for(;;){} }})` is a
 * page loop — and it was a JS_ToCString from C in element.c, document.c, timer.c and node.c alike.
 *
 * ONE MACHINE RATHER THAN ONE PER MEMBER, because that is what the members actually have in common, and because
 * a per-member machine is a per-member chance to get the resumption wrong. It is the same declaration the
 * engine already makes for its own coerce-then-compute builtins: the member DECLARES which arguments the spec
 * coerces, this performs those coercions on the trampoline, and the body is called with the strings in place —
 * where it has no user code left to reach, which is exactly what the declaration asserts.
 *
 * The cursor is the ARGUMENT INDEX, so a resume comes back to the argument it was on and not to the start:
 * `setAttribute({toString(){…}}, {toString(){…}})` coerces two, and a suspension in the first must not re-run
 * it. That is the whole reason this is a machine and not a loop.
 *
 * A member's def is registered once and lives in a static pool, because JS_RegisterStepDef BORROWS the
 * definition and it must outlive the runtime. `arg` carries the pool index, which is how one step function
 * serves every member — the same thing the engine's own contiguous STEPDEF blocks do. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"

#define IDL_MAX_MEMBERS 32
#define IDL_MAX_ARGS     8

typedef struct {
    IdlBody  body;
    uint32_t strmask;
    int      magic;
} IdlMember;

static IdlMember      g_members[IDL_MAX_MEMBERS];
static JSTrampStepDef g_defs[IDL_MAX_MEMBERS];   /* not const: `arg` is filled at registration with the index */
static int            g_n;
static JSRuntime     *g_rt;

typedef struct {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    int       i;        /* THE RESUME POINT: the argument being coerced */
    int       n;        /* how many of them there are */
    JSValue   result;   /* the body's answer (owned) */
    JSValue   args[IDL_MAX_ARGS];
} JSIdlArgsState;

/* WHAT THIS MACHINE OWNS: the coerced arguments so far. A concolic branch inside one page `toString` forks the
   flow at that depth, and the two arms must not share one argument vector. */
static void js_idl_args_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSIdlArgsState *s = st;
    int i;
    v->val(ctx, &s->result);
    for (i = 0; i < IDL_MAX_ARGS; i++)
        v->val(ctx, &s->args[i]);
}

static int js_idl_args_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSIdlArgsState *s = st;
    const IdlMember *m;
    int r;

    DCHECK(s->hdr.arg >= 0 && s->hdr.arg < g_n, "an IDL member's step ran with no pool entry behind it");
    m = &g_members[s->hdr.arg];

    if (s->hdr.stage == 0) {
        s->n = s->hdr.argc < IDL_MAX_ARGS ? s->hdr.argc : IDL_MAX_ARGS;
        DCHECK(s->hdr.argc <= IDL_MAX_ARGS,
               "a DOM member was called with more arguments than this machine carries — raise IDL_MAX_ARGS "
               "rather than silently dropping the tail");
        s->result = JS_UNDEFINED;
        for (r = 0; r < IDL_MAX_ARGS; r++)
            s->args[r] = JS_UNDEFINED;
        s->i = 0;
        s->hdr.stage = 1;
    }

    while (s->i < s->n) {
        JSValueConst a = step_arg(&s->hdr, s->i);
        if (!(m->strmask & (1u << s->i))) {
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            s->args[s->i] = JS_DupValue(ctx, a);   /* not a DOMString: it crosses unconverted */
            s->i++;
            continue;
        }
        r = step_tostring_run(ctx, &s->hdr, a, cb_result, &s->args[s->i], out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;          /* parked ON THIS ARGUMENT; the resume comes back to it */
        if (r < 0) return JS_STEP_ABRUPT;
        s->i++;
    }
    JS_FreeValue(ctx, cb_result);

    /* THE BODY RUNS HERE, NOT IN fini. Every declared argument is a real string now, so it has no user code
       left to reach — the claim the declaration makes. It cannot run in fini because the shared teardown
       releases hdr.this_val BEFORE calling it, so a body that reads the receiver there reads a freed value:
       the listener registration silently found no receiver and registered nothing, with no throw to show for
       it. A machine's fini may yield what it already computed; it may not compute. */
    s->result = m->body(ctx, s->hdr.this_val, s->n, (JSValueConst *)s->args, m->magic);
    if (JS_IsException(s->result)) {
        s->result = JS_UNDEFINED;
        return JS_STEP_ABRUPT;
    }
    return JS_STEP_DONE;
}

static JSValue idl_args_result(JSContext *ctx, void *st, bool take_result)
{
    JSIdlArgsState *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    int i;

    if (take_result) s->result = JS_UNDEFINED;
    JS_FreeValue(ctx, s->result);
    s->result = JS_UNDEFINED;
    for (i = 0; i < IDL_MAX_ARGS; i++) {
        JS_FreeValue(ctx, s->args[i]);
        s->args[i] = JS_UNDEFINED;
    }
    return r;
}

int idl_string_method_id(JSContext *ctx, uint32_t strmask, IdlBody body, int magic)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    int idx;

    DCHECK(g_rt == NULL || g_rt == rt,
           "an IDL member was installed into a second runtime — its step ids belong to the first, and one WASM "
           "instance is one document");
    CHECK(g_n < IDL_MAX_MEMBERS,
          "the IDL member pool is full — a member is being DECLARED more than once (Element installs its "
          "members on every wrapper; declare in the component's init and install from the cached id)");
    g_rt = rt;
    idx = g_n++;
    g_members[idx].body    = body;
    g_members[idx].strmask = strmask;
    g_members[idx].magic   = magic;
    g_defs[idx].size  = sizeof(JSIdlArgsState);
    g_defs[idx].step  = js_idl_args_step;
    g_defs[idx].fini  = idl_args_result;
    g_defs[idx].arg   = idx;
    g_defs[idx].visit = js_idl_args_visit;
    return JS_RegisterStepDef(rt, &g_defs[idx]);
}

void idl_install_method(JSContext *ctx, JSValueConst target, const char *name, int length, int stepid)
{
    DCHECK(stepid >= 0, "an IDL member was installed before it was declared");
    JS_SetPropertyStr(ctx, (JSValue)target, name,
                      JS_NewCFunction2(ctx, NULL, name, length, JS_CFUNC_step, stepid));
}
