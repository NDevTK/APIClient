/* PERFORMING A CROSS-AGENT OPERATION — see remote_op.h for why this is one file and not one per host. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/frame/remote_object.h"
#include "core/frame/remote_op.h"
#include "core/realm.h"

/* WHICH OPERATION A RECORD IS, and everything that is a fact about it, on one row. The verb, how many fields
   it carries and the program that performs it were three lists in the host this was hoisted out of, and the
   field count came back as the wrong one once already. */
enum { OP_WPGET, OP_GET, OP_SET, OP_DELETE, OP_APPLY, OP_N };

/* `need` is the MINIMUM: `object.apply` carries one field per argument and there is no ceiling on how many may
   cross — a bound on the argument count is a bound on which calls this engine can make. */
static const struct { const char *verb; int need; const char *program; } OPS[OP_N] = {
    /* §7.2.1's member of THIS document's Window, read as the IDL getter it is. `globalThis[k]` and not
       `self[k]`: the page may shadow `self`, and the global object is what the standard reads the member of. */
    { "windowproxy.get", 4, "globalThis[__apiclientKey]" },
    /* ECMA-262 10.1.8 — pure syntax, so nothing the page owns is between the operation and its answer. */
    { "object.get",      5, "__apiclientLent[__apiclientKey]" },
    /* 10.1.9 through %Reflect.set%: an assignment expression completes with the VALUE, and 10.5.9 step 8 asks
       the trap for the BOOLEAN, which is a different fact and the only one that says whether the write took. */
    { "object.set",      6, "__apiclientOp(__apiclientLent, __apiclientKey, __apiclientVal)" },
    /* 10.1.10, SLOPPY MODE ON PURPOSE: `delete` yields the boolean here and THROWS for a false in strict mode,
       and the boolean is exactly what 10.5.10 step 8 asks the trap for. */
    { "object.delete",   5, "delete __apiclientLent[__apiclientKey]" },
    /* 10.2.1 through %Reflect.apply%, because a call needs its argument list SPREAD and `f.apply` is a property
       of the function that the page may replace. */
    { "object.apply",    5, "__apiclientOp(__apiclientLent, __apiclientThis, __apiclientArgs)" },
};

struct RemoteOp { int op; char *text; char **f; int nf; };

/* ---- THE TWO INTRINSICS, PER REALM ----------------------------------------------------------------------- */

static int g_set_slot = -1, g_apply_slot = -1;

/* CAPTURED BEFORE THIS REALM'S SCRIPTS RUN, which is what makes them the intrinsics rather than whatever the
   page has left on `Reflect` by the time a peer asks. Both reads are ordinary data properties of objects the
   runtime built moments ago, so nothing here runs page code — there is none yet to run. */
static void remote_op_install(JSContext *ctx)
{
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue reflect = JS_GetPropertyStr(ctx, g, "Reflect");
    JSValue set, apply;

    DCHECK(JS_IsObject(reflect),
           "a realm was built without %Reflect% — a peer performs a cross-agent [[Set]] and [[Call]] through "
           "its two intrinsics, and a realm without them can answer neither");
    set = JS_GetPropertyStr(ctx, reflect, "set");
    apply = JS_GetPropertyStr(ctx, reflect, "apply");
    CHECK(JS_IsFunction(ctx, set) && JS_IsFunction(ctx, apply),
          "%Reflect.set% / %Reflect.apply% are not functions in a realm this agent just built — a peer would "
          "perform a cross-agent write or call through something that is not the internal method");
    realm_value_set(ctx, g_set_slot, set);
    realm_value_set(ctx, g_apply_slot, apply);
    JS_FreeValue(ctx, reflect);
    JS_FreeValue(ctx, g);
}

void remote_op_init(JSContext *ctx)
{
    DCHECK(ctx != NULL, "the cross-agent operation performer was declared into no realm");
    g_set_slot = realm_value_declare(ctx, "%Reflect.set%, the intrinsic a peer performs a cross-agent [[Set]] "
                                          "through");
    g_apply_slot = realm_value_declare(ctx, "%Reflect.apply%, the intrinsic a peer performs a cross-agent "
                                            "[[Call]] through");
    realm_declare_intrinsic(remote_op_install);
}

/* ---- THE RECORD ------------------------------------------------------------------------------------------ */

/* SPLIT ONCE AND WITHOUT A CEILING. This was a fixed array of eight, which is a cap on how many ARGUMENTS may
   cross, and the ninth field would have been read as part of the eighth. `line` is modified in place. */
static char **op_split(char *line, int *pn)
{
    char **f = NULL, *q = line;
    int n = 0, cap = 0;

    for (;;) {
        char *t = strchr(q, '\t');
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            f = realloc(f, (size_t)cap * sizeof *f);
            CHECK(f != NULL, "remote op: OOM splitting a cross-agent record");
        }
        f[n++] = q;
        if (!t) break;
        *t = 0;
        q = t + 1;
    }
    *pn = n;
    return f;
}

static int op_of(const char *verb)
{
    int i;
    for (i = 0; i < OP_N; i++)
        if (!strcmp(verb, OPS[i].verb)) return i;
    DFAIL("a cross-instance record named an operation this agent does not perform — an unanswered record parks "
          "the asking flow forever, so the operation has to be built rather than ignored");
    return OP_WPGET;
}

RemoteOp *remote_op_parse(const char *record)
{
    RemoteOp *o;

    DCHECK(record != NULL && *record, "a cross-agent operation arrived with no text to perform");
    o = calloc(1, sizeof *o);
    CHECK(o != NULL, "remote op: OOM receiving a cross-agent operation");
    o->text = strdup(record);
    CHECK(o->text != NULL, "remote op: OOM copying a cross-agent operation");
    o->f = op_split(o->text, &o->nf);
    o->op = op_of(o->f[0]);
    /* A `CHECK` AND NOT A DCHECK. A short record is the writer and this reader disagreeing about the grammar,
       which is a dev-only defect — but the release build would then READ A FIELD THAT IS NOT THERE, so this is
       one of the invariants that must hold in production as well. Answering from the fields that did arrive
       answers a different question, and reading past them answers with whatever is next in memory. */
    CHECK(o->nf >= OPS[o->op].need,
          "a cross-agent operation arrived with fewer fields than the operation carries — the instance that "
          "wrote it and this one disagree about the record's grammar");
    return o;
}

const char *remote_op_worlds(const RemoteOp *op)
{
    DCHECK(op != NULL, "the world of a cross-agent operation that was never parsed");
    return op->f[2];
}

const char *remote_op_doc(const RemoteOp *op)
{
    DCHECK(op != NULL, "the target document of a cross-agent operation that was never parsed");
    return op->f[1];
}

const char *remote_op_program(JSContext *ctx, const RemoteOp *op)
{
    JSValue g;

    DCHECK(op != NULL, "the program of a cross-agent operation that was never parsed");
    g = JS_GetGlobalObject(ctx);
    if (op->op == OP_WPGET) {
        /* THE MEMBER NAME IS NOT AN ENCODED VALUE. §7.2.1's list is a fixed twelve of this engine's own
           spelling (window_proxy.c's PROXY_MEMBER), so it crosses as itself. */
        JS_SetPropertyStr(ctx, g, "__apiclientKey", JS_NewString(ctx, op->f[3]));
    } else {
        JSValueConst held = remote_object_by_id((uint32_t)strtoul(op->f[3], NULL, 10));

        DCHECK(!JS_IsUndefined(held),
               "a peer named an object this agent never lent — the name it used was minted somewhere else, or "
               "the export table was lost between the lend and the operation");
        JS_SetPropertyStr(ctx, g, "__apiclientLent", JS_DupValue(ctx, held));
        if (op->op == OP_APPLY) {
            JSValue args = JS_NewArray(ctx);
            int i;
            /* THE ARGUMENT LIST IS FLAT ON THE WIRE — one field per argument, in the one grammar — so there is
               no second grammar for a list and no ceiling on how many may cross. */
            for (i = 5; i < op->nf; i++)
                JS_SetPropertyUint32(ctx, args, (uint32_t)(i - 5), remote_object_decode(ctx, op->f[i]));
            JS_SetPropertyStr(ctx, g, "__apiclientThis", remote_object_decode(ctx, op->f[4]));
            JS_SetPropertyStr(ctx, g, "__apiclientArgs", args);
            JS_SetPropertyStr(ctx, g, "__apiclientOp", realm_value_get(ctx, g_apply_slot));
        } else {
            JS_SetPropertyStr(ctx, g, "__apiclientKey", remote_object_decode(ctx, op->f[4]));
            if (op->op == OP_SET) {
                JS_SetPropertyStr(ctx, g, "__apiclientVal", remote_object_decode(ctx, op->f[5]));
                JS_SetPropertyStr(ctx, g, "__apiclientOp", realm_value_get(ctx, g_set_slot));
            }
        }
    }
    JS_FreeValue(ctx, g);
    return OPS[op->op].program;
}

void remote_op_free(RemoteOp *op)
{
    if (!op) return;
    free(op->f);
    free(op->text);
    free(op);
}
