/* PERFORMING A CROSS-AGENT OPERATION — see remote_op.h for why this is one file and not one per host. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/frame/remote_object.h"
#include "core/frame/remote_op.h"
/* §7.2.1.3.1 CrossOriginProperties ( O )'s list, asked rather than copied — a `windowproxy.get` names a member
   and this agent must not perform one the standard does not list. */
#include "core/frame/window_proxy.h"
#include "core/realm.h"

/* WHICH OPERATION A RECORD IS, and everything that is a fact about it, on one row. The verb, how many fields
   it carries and the program that performs it were three lists in the host this was hoisted out of, and the
   field count came back as the wrong one once already. */
enum { OP_WPGET, OP_GET, OP_SET, OP_DELETE, OP_APPLY, OP_N };

/* `need` is the MINIMUM: `object.apply` carries one field per argument and there is no ceiling on how many may
   cross — a bound on the argument count is a bound on which calls this engine can make. */
static const struct { const char *verb; int need; const char *program; } OPS[OP_N] = {
    /* §7.2.1's member of THIS document's Window, read as the IDL getter it IS — §7.2.1.3.4
       CrossOriginGetOwnPropertyHelper ( O, P ) calls "an anonymous built-in function, created in the current
       realm, that performs the same steps as the getter of the IDL attribute P on object O", and this is that
       call. It was `globalThis[__apiclientKey]`, an ordinary [[Get]], which is a DIFFERENT operation: Web IDL
       §3.3.11 [Replaceable] makes `window.length = 5` create an own data property that "will shadow the
       accessor property", so a peer page could decide what this agent answered a cross-origin reader — and
       answer it with a plausible NUMBER, which no type test on the far side can tell from the real count.
       NEITHER OPERAND IS THE PAGE'S. The getter is window_proxy.c's per-realm capture, taken before any of
       this document's script ran; the receiver is JS_GetGlobalObject, not the `globalThis` binding, which is
       an ordinary writable data property of the global that a page may reassign like any other. */
    { "windowproxy.get", 4, "__apiclientOp(__apiclientGetter, __apiclientThis, __apiclientArgs)" },
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
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — core/agent_state.h. Two slot NUMBERS into a
       realm-value pool that is itself the agent's, and this row's release column was empty because its file
       already had a `_free` and the name was TAKEN: `remote_op_free` frees ONE parsed record, per operation.
       A name collision is not a release, and the pairing in core/platform.c could not see the difference —
       the row declared nothing and released nothing, which is the arm that passes in silence. */
    agent_state_id("remote_op", &g_set_slot,
                   "the realm-value slot holding %Reflect.set%, the intrinsic a peer performs a cross-agent "
                   "[[Set]] through");
    agent_state_id("remote_op", &g_apply_slot,
                   "the realm-value slot holding %Reflect.apply%, the intrinsic a peer performs a cross-agent "
                   "[[Call]] through");
    realm_declare_intrinsic(remote_op_install);
}

/* THE AGENT HALF, UNDONE — and it is named `_agent_free` because `remote_op_free` below is a DIFFERENT
   operation on a DIFFERENT lifetime: it frees one parsed record, once per operation performed. The two halves
   of `document` are named the same way and for the same reason. What this owes is the two slot NUMBERS: the
   VALUES they hold are each realm's own and go with their contexts, and the POOL is released by
   realm_intrinsics_free — so a carried number is an index into a pool the next agent has not built yet, read
   by remote_op_program at the first cross-agent [[Set]] or [[Call]] that agent is asked to perform. */
void remote_op_agent_free(void)
{
    DCHECK(g_set_slot >= 0 && g_apply_slot >= 0,
           "the cross-agent operation performer was released in an agent that never declared it");
    g_set_slot = g_apply_slot = -1;
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
    /* AND A `windowproxy.get` NAMES A MEMBER HTML §7.2.1.3.1 CrossOriginProperties ( O ) LISTS — asserted HERE,
       where the record is BORN in this agent, rather than where its operand is used.
       IT IS THE SAME KIND OF INVARIANT AS THE FIELD COUNT ABOVE and belongs beside it: both are facts about
       whether this record is well-formed at all, and neither is answerable anywhere else. The ASKING half of
       this seam cannot emit an unlisted name — window_proxy.c builds the record out of `PROXY_MEMBER[magic]`,
       an index into a fixed table — but that is a fact about one writer, and what arrives here is TEXT from
       another instance, relayed by a zone whose contract is that it does not read what it routes and could not
       judge this if it did (remote_op.h: "only an engine knows what a name means"). SECURITY.md makes every
       WASM instance untrusted, so this field is attacker-reachable, and an unlisted one becomes
       `globalThis[k]` — a CROSS-ORIGIN READ of this document's own global (`document`, `cookie`, anything it
       carries) computed by this agent and relayed to the asker as an ordinary completion. Nothing downstream
       can tell that answer from a listed member's: it is a real value of this document's, which is the
       defaulted-field failure with an origin boundary under it.
       AT THE PARSE AND NOT AT THE PROGRAM, because a record is parsed by TWO callers and only one of them goes
       on to build a program — the arrival path parses the record, asserts what it names, and frees it, so a
       check at the use site would let a malformed record through the entry that receives it. Both callers
       reach this line.
       A CHECK AND NOT A DCHECK, for the reason the field count above is one and one step stronger: this is
       §Offensive-programming's security boundary, so the release build must not perform the read either. There
       is deliberately nothing to fall back to — a record naming an unlisted member was not written by this
       engine's asking half, and answering it at all is the defect. */
    CHECK(o->op != OP_WPGET || window_proxy_cross_origin_property(o->f[3]) != NULL,
          "a cross-agent record asked this agent to read a Window member HTML §7.2.1.3.1 CrossOriginProperties "
          "( O ) does not list among the cross-origin accessible window property names — performing it would "
          "compute a cross-origin read of this document's own global and relay it to the asking instance as an "
          "ordinary answer. AND ONE EMITTER IN THIS ENGINE REACHES IT: §7.3.1.3 Child navigables' content "
          "document, whose cross-instance arm in core/html/html_iframe.c names the member `document` after "
          "filtering for same origin-domain. That read is legitimate and this refusal is still right, because "
          "the RECORD carries no claim about who is asking and a receiver cannot tell that emitter from an "
          "untrusted instance naming the same member. What it needs is a verb of its own, and an answer: a "
          "Document is an OBJECT, which crosses only once core/frame/remote_object.c can name one");
    /* AND THAT THE ENTRY IS §7.2.1.3.4's ACCESSOR BRANCH, ASKED IN THE SAME BREATH AND OFF THE SAME LOOKUP.
       §7.2.1.3.1 writes each entry with its [[NeedsGetter]] and [[NeedsSetter]], and the lookup above returns
       the RECORD rather than a boolean precisely so the second question needs no second table. An entry with
       neither flag — `{ [[Property]]: "close" }`, and `focus`, `blur`, `postMessage` beside it — is
       §7.2.1.3.4's OPERATION branch, whose answer is an anonymous function and not a getter's result, and this
       engine's `windowproxy.get` performs the accessor branch only.
       A DCHECK AND NOT A CHECK, which is the one place this differs from the field count and the listed-member
       CHECK above: both of those are memory or a security boundary, and this is neither. The name IS on
       §7.2.1.3.1's list, so answering it leaks nothing the standard withholds — the release build simply runs
       %Reflect.apply% over an absent getter and hands back a TypeError, which is a wrong answer rather than an
       unsafe one. BUILD THE OPERATION BRANCH and this goes: it needs a function to be nameable across the
       seam, which is core/frame/remote_object.c's remote-object handle. */
    DCHECK(o->op != OP_WPGET || window_proxy_cross_origin_property(o->f[3])->needs_get,
           "a cross-agent record asked this agent for a Window member whose HTML §7.2.1.3.1 "
           "CrossOriginProperties ( O ) record carries neither [[NeedsGetter]] nor [[NeedsSetter]] — that is "
           "§7.2.1.3.4 CrossOriginGetOwnPropertyHelper's OPERATION branch, whose answer is \"an anonymous "
           "built-in function, created in the current realm, that performs the same steps as the IDL operation "
           "P on object O\", and this "
           "seam performs the getter branch only. The two members window_proxy.c's proxy_get_step can emit are "
           "`length` and `closed`, both accessor entries, so a record naming an operation was written by "
           "something other than that step machine");
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
        /* THE MEMBER NAME IS NOT AN ENCODED VALUE. HTML §7.2.1.3.1 CrossOriginProperties ( O ) names THIRTEEN
           cross-origin accessible window property names, and they are this engine's own spelling
           (window_proxy.c's PROXY_MEMBER), so the member crosses as itself rather than through
           remote_object.c's value grammar. It never reaches the PROGRAM now — it selects a captured getter
           here, in C, and the program has no name in it at all. */
        /* THAT IT IS ONE OF THE THIRTEEN, AND THAT ITS ENTRY CARRIES [[NeedsGetter]], ARE ALREADY TRUE HERE —
           remote_op_parse asserts both where the record is born, which is the only place BOTH callers of the
           parse reach. Neither is re-asked at this line: a second CHECK of one invariant is a second copy of
           it, and the copy that drifts is the one further from the record. */
        JSValue args = JS_NewArray(ctx);

        CHECK(!JS_IsException(args),
              "remote op: the empty argument list for §7.2.1.3.4's getter could not be allocated");
        JS_SetPropertyStr(ctx, g, "__apiclientGetter", window_proxy_cross_origin_getter(ctx, op->f[3]));
        /* §7.2.1.3.4 runs the getter's steps ON O, and O is the peer document's Window — this realm's global
           object, taken from the runtime rather than read out of the realm under whatever name. */
        JS_SetPropertyStr(ctx, g, "__apiclientThis", JS_GetGlobalObject(ctx));
        JS_SetPropertyStr(ctx, g, "__apiclientArgs", args);
        JS_SetPropertyStr(ctx, g, "__apiclientOp", realm_value_get(ctx, g_apply_slot));
    } else {
        /* THE OBJECT IS NAMED BY (GENERATION, ID) — see remote_object.h. An id alone is an index into
           whichever of this document's sessions happens to be running when the record lands, and it is IN
           RANGE in every one of them, so a name from a session that parked would resolve to an unrelated
           object. A CHECK and not a DCHECK for the reason the field count above is one: the release build
           would read past the field rather than answer a different question. */
        const char *idf = op->f[3], *sep = strchr(idf, ':');
        JSValueConst held;

        CHECK(sep != NULL,
              "a cross-agent operation named an object by an id with no GENERATION — an export id is an index "
              "into ONE session's table, so the instance that wrote this record and this one disagree about "
              "what names an object");
        held = remote_object_by_id((uint32_t)strtoul(idf, NULL, 10), (uint32_t)strtoul(sep + 1, NULL, 10));

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
