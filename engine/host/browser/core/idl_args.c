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

#include <stdbool.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "solver/concolic.h"
#include "core/idl_args.h"

/* Sized for the WHOLE platform surface, because every reflected content attribute is a declaration: HTML's
   per-tag interfaces contribute about 190 between them, each with its own setter. The pool is static because
   JS_RegisterStepDef BORROWS the definition, so it must outlive the runtime. */
#define IDL_MAX_MEMBERS 384
#define IDL_MAX_ARGS     8

typedef struct {
    IdlSetter  setter;      /* set instead of `body` for an attribute setter */
    bool       null_to_empty;
    IdlBody    body;
    IdlArgType types[IDL_MAX_DECLARED];
    int        nargs;      /* how many the IDL lists; a variadic tail repeats the last */
    int        magic;
    /* An IDL_DICT argument's members, and their names INTERNED at registration. The atom must be live at both
       the request and the answer — step_getprop_run is handed it twice, with a suspension in between — so it
       cannot be created per read. The names are static strings known when the member declares itself, so one
       intern per member serves every call. */
    const IdlDictMember *dict;
    JSAtom     dict_atoms[IDL_MAX_DICT];
    int        dict_n;
    /* A member whose algorithm is itself page code runs as a STEP once the conversions are done — see
       idl_method_id_step. Its state lives immediately after this machine's, which is why the def's size is
       per-member and not a constant. */
    const IdlStepDecl *step;
    bool       variadic;    /* the last declared type applies to every argument from there on */
    JSClassID  iface;       /* the brand an IDL_STRING_UNLESS_IFACE position tests against */
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
    int       dict_i;   /* THE OTHER RESUME POINT: the dictionary member being read */
    uint8_t   dict_phase;   /* 0 = read the member, 1 = convert what was read. Both can park, so a member needs
                               two resume points, not one — a resume in the CONVERSION must not re-read. */
    JSValue   dict_v;   /* the member's value between those two phases (owned) */
    int64_t   nums[IDL_MAX_ARGS];   /* the integer conversions, kept because the body wants numbers */
    JSValue   args[IDL_MAX_ARGS];
    /* A VARIADIC member's converted arguments, which cannot live in the fixed array above and must not be
       truncated to fit it: `ul.append(...items)` has as many as the page has items. It is an ARRAY rather than
       a heap block because that is what `visit` can carry — a deep fork byte-copies the state and re-takes what
       visit names, so a block pointer would be SHARED by two flows that both free it, and a pointer into the
       state itself would survive the copy still aimed at the original. One owned value, one v->val, no new
       ownership contract. Non-variadic members never touch it: their arguments are exactly the declared ones,
       so the fixed array is always big enough by IDL_MAX_DECLARED. */
    JSValue   conv;
    JSValue   vstage;   /* the variadic argument being converted, before it joins `conv` */
} JSIdlArgsState;

/* WHAT THIS MACHINE OWNS: the coerced arguments so far. A concolic branch inside one page `toString` forks the
   flow at that depth, and the two arms must not share one argument vector. */
static void js_idl_args_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSIdlArgsState *s = st;
    const IdlMember *m;
    int i;
    v->val(ctx, &s->result);
    v->val(ctx, &s->dict_v);
    v->val(ctx, &s->conv);
    v->val(ctx, &s->vstage);
    for (i = 0; i < IDL_MAX_ARGS; i++)
        v->val(ctx, &s->args[i]);
    DCHECK(s->hdr.arg >= 0 && s->hdr.arg < g_n, "an IDL member's visit ran with no pool entry behind it");
    m = &g_members[s->hdr.arg];
    if (m->step && m->step->visit) m->step->visit(ctx, (char *)st + sizeof(JSIdlArgsState), v);
}

static void idl_free_vec(JSContext *ctx, JSValue *vec, int n)
{
    int k;
    if (!vec) return;
    for (k = 0; k < n; k++) JS_FreeValue(ctx, vec[k]);
    js_free(ctx, vec);
}

static int js_idl_args_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSIdlArgsState *s = st;
    const IdlMember *m;
    JSValue *argv_vec = NULL;
    int r;

    DCHECK(s->hdr.arg >= 0 && s->hdr.arg < g_n, "an IDL member's step ran with no pool entry behind it");
    m = &g_members[s->hdr.arg];

    if (s->hdr.stage == 0) {
        /* A NON-VARIADIC member's arguments ARE its declared ones: a position the IDL does not list is not
           part of the member, so there is nothing past `nargs` to convert, to store, or to hand the body. A
           VARIADIC one takes every argument the page passed, however many that is. */
        s->n = m->variadic ? s->hdr.argc
             : (s->hdr.argc < m->nargs ? s->hdr.argc : m->nargs);
        DCHECK(m->variadic || s->n <= IDL_MAX_ARGS,
               "a member declared more arguments than this machine carries — IDL_MAX_DECLARED bounds what a "
               "member may declare, so this means the two have drifted apart");
        s->result = JS_UNDEFINED;
        s->dict_v = JS_UNDEFINED;
        s->conv = m->variadic ? JS_NewArray(ctx) : JS_UNDEFINED;
        s->vstage = JS_UNDEFINED;
        for (r = 0; r < IDL_MAX_ARGS; r++)
            s->args[r] = JS_UNDEFINED;
        s->i = 0;
        s->hdr.stage = 1;
    }

    while (s->i < s->n) {
        JSValueConst a = step_arg(&s->hdr, s->i);
        /* ONE STORE PER ARGUMENT, at the bottom of the loop. Every branch below writes the converted value
           into `slot` and falls through to `placed`, so the variadic append happens in exactly one place and
           cannot be forgotten by whichever branch is added next. */
        JSValue *slot = m->variadic ? &s->vstage : &s->args[s->i];
        /* A POSITION THE IDL DOES NOT LIST IS NOT CONVERTED. Repeating the last declared type instead was a
           catch-all with a real victim: addEventListener declares one DOMString, so the repeat converted its
           CALLBACK to a string and every listener registered was the string "function () {…}". A variadic
           member's tail is `any...` in every case here, which is exactly what not-listed already means. */
        IdlArgType t = (s->i < m->nargs) ? m->types[s->i]
                     : (m->variadic ? m->types[m->nargs - 1] : IDL_ANY);

        if (t == IDL_STRING_UNLESS_CALLABLE)
            t = JS_IsFunction(ctx, a) ? IDL_ANY : IDL_DOMSTRING;   /* the union's own rule */
        if (t == IDL_STRING_UNLESS_IFACE) {
            DCHECK(m->iface != 0, "a member declared an interface-or-string union with no interface to brand "
                                  "against — the class is half of what that type states");
            t = JS_GetOpaque(a, m->iface) ? IDL_ANY : IDL_DOMSTRING;
        }

        /* UNKNOWN EXTERNAL INPUT CROSSES AS ITSELF, whatever the declared type says.
           An IDL conversion is a BOUNDARY, not an ECMAScript operator: nothing observes its result except the
           component behind it, and every one of those bodies already asks explicitly for what it needs from a
           concolic (concolic_shape_c for the bytes a Text node carries, the attribute taint shadow for a value
           parked in the DOM). A DICTIONARY is excluded because it is not a value that crosses at all — it is a
           bag of member READS, and those happen on a concolic exactly as they do on anything else.
           Converting here would do the one thing that must never happen — hand ToString a
           concolic, which the C boundary asserts against because opacity has to SURVIVE a coercion or the value
           stops forking control flow and stops being solvable at a sink. This is the same answer JSON.stringify
           gives an opaque field: yield the opaque itself, never a de-tainting placeholder. */
        if (t != IDL_ANY && t != IDL_DICT && t != IDL_DICT_OR_BOOL_FIRST && concolic_is(a)) {
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            *slot = JS_DupValue(ctx, a);
            goto placed;
        }

        if (t == IDL_DICT || t == IDL_DICT_OR_BOOL_FIRST) {
            DCHECK(!m->variadic || s->i < m->nargs,
                   "a dictionary argument landed in a VARIADIC tail — the conversion cursor is per-member, so "
                   "a dictionary repeated by the tail would read the first one's names");
            /* `optional D options = {}`: undefined and null have no members to read, so every one defaults and
               no page code runs. An object's members are read IN ORDER and each is converted by ITS OWN type,
               parking on either half. A `required` member is checked here rather than in the body, because
               `required` is part of the TYPE the declaration states. */
            if (JS_IsUndefined(s->args[s->i]))
                s->args[s->i] = JS_NewObject(ctx);
            if (!JS_IsObject(a)) {
                JS_FreeValue(ctx, cb_result);
                cb_result = JS_UNDEFINED;
                /* §2.7 "flatten": a non-object IS the first member's boolean. There is nothing to READ, so
                   this runs none of the page's code either way. */
                if (t == IDL_DICT_OR_BOOL_FIRST && m->dict_n > 0) {
                    DCHECK(m->dict[0].type == IDL_BOOLEAN,
                           "a (dictionary or boolean) union declared a non-boolean first member — the union's "
                           "rule is that the bare value IS that member");
                    JS_SetPropertyStr(ctx, s->args[s->i], m->dict[0].name, JS_NewBool(ctx, JS_ToBool(ctx, a)));
                }
                for (r = 0; r < m->dict_n; r++)
                    if (m->dict[r].required)
                        return JS_ThrowTypeError(ctx, "required member %s is undefined", m->dict[r].name),
                               JS_STEP_ABRUPT;
                s->dict_i = m->dict_n;
            }
            while (s->dict_i < m->dict_n) {
                const IdlDictMember *dm = &m->dict[s->dict_i];
                IdlArgType mt = dm->type;

                if (s->dict_phase == 0) {
                    r = step_getprop_run(ctx, &s->hdr, a, m->dict_atoms[s->dict_i], cb_result, &s->dict_v,
                                         out_cb, out_argc);
                    cb_result = JS_UNDEFINED;
                    if (r > 0) return r;      /* parked ON THIS MEMBER's read; the resume comes back to it */
                    if (r < 0) return JS_STEP_ABRUPT;
                    s->dict_phase = 1;
                    if (dm->required && JS_IsUndefined(s->dict_v))
                        return JS_ThrowTypeError(ctx, "required member %s is undefined", dm->name),
                               JS_STEP_ABRUPT;
                }
                DCHECK(mt != IDL_DICT, "a dictionary member was declared as a dictionary — the conversion "
                                       "cursor is per-argument, so a nested one would read the outer's names");
                /* An ABSENT member is not converted: `undefined` on a dictionary means the member is not
                   there, and running ToString over it would write the four characters `undefined` where the
                   spec puts nothing. A boolean is the exception only because ToBoolean(undefined) is false,
                   which is the `= false` default every boolean member in this surface declares. */
                if (JS_IsUndefined(s->dict_v) && mt != IDL_BOOLEAN)
                    mt = IDL_ANY;
                /* The same boundary rule the arguments follow: unknown external input crosses as ITSELF, so a
                   concolic member keeps forking control flow instead of collapsing at a coercion. */
                if (mt != IDL_ANY && concolic_is(s->dict_v))
                    mt = IDL_ANY;
                if (mt == IDL_BOOLEAN) {
                    JSValue b = JS_NewBool(ctx, JS_ToBool(ctx, s->dict_v));
                    JS_FreeValue(ctx, s->dict_v);
                    s->dict_v = b;
                }
                else if (mt == IDL_LONG) {
                    r = step_toint64_run(ctx, &s->hdr, s->dict_v, cb_result, &s->nums[s->i], out_cb, out_argc);
                    cb_result = JS_UNDEFINED;
                    if (r > 0) return r;   /* parked ON THIS MEMBER's conversion; the read does not re-run */
                    if (r < 0) return JS_STEP_ABRUPT;
                    JS_FreeValue(ctx, s->dict_v);
                    s->dict_v = JS_NewInt64(ctx, s->nums[s->i]);
                }
                else if (mt == IDL_DOMSTRING || mt == IDL_DOMSTRING_NULLABLE) {
                    if (mt == IDL_DOMSTRING_NULLABLE && JS_IsNull(s->dict_v)) {
                        /* `DOMString?`: null is the IDL null, never the string "null". */
                    } else {
                        JSValue str = JS_UNDEFINED;
                        r = step_tostring_run(ctx, &s->hdr, s->dict_v, cb_result, &str, out_cb, out_argc);
                        cb_result = JS_UNDEFINED;
                        if (r > 0) return r;
                        if (r < 0) return JS_STEP_ABRUPT;
                        JS_FreeValue(ctx, s->dict_v);
                        s->dict_v = str;
                    }
                }
                JS_SetPropertyStr(ctx, s->args[s->i], dm->name, s->dict_v);
                s->dict_v = JS_UNDEFINED;
                s->dict_phase = 0;
                s->dict_i++;
            }
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            s->dict_i = 0;
            s->i++;
            continue;
        }


        /* `DOMString?`: null AND undefined become the IDL null before any ToString is reached. */
        if (t == IDL_DOMSTRING_NULLABLE) {
            if (JS_IsNull(a) || JS_IsUndefined(a)) {
                JS_FreeValue(ctx, cb_result);
                cb_result = JS_UNDEFINED;
                *slot = JS_NULL;
                goto placed;
            }
            t = IDL_DOMSTRING;
        }

        /* [LegacyNullToEmptyString]: null becomes "" rather than "null", and it is part of the TYPE — the
           declaration says so, so no body has to remember it. */
        if (t == IDL_DOMSTRING && m->null_to_empty && JS_IsNull(a)) {
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            *slot = JS_NewStringLen(ctx, "", 0);
            goto placed;
        }
        if (t == IDL_ANY) {
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            *slot = JS_DupValue(ctx, a);   /* no conversion: it crosses as itself */
            goto placed;
        }
        if (t == IDL_BOOLEAN) {
            /* ToBoolean runs nothing, but the ARGUMENT still crosses converted: `toggle(t, 1)` forces on, and a
               body that got the 1 would have to remember to coerce it. */
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            *slot = JS_IsUndefined(a) ? JS_UNDEFINED : JS_NewBool(ctx, JS_ToBool(ctx, a));
            goto placed;
        }
        if (t == IDL_LONG) {
            r = step_toint64_run(ctx, &s->hdr, a, cb_result, &s->nums[s->i], out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
            *slot = JS_NewInt64(ctx, s->nums[s->i]);
            goto placed;
        }
        DCHECK(t == IDL_DOMSTRING, "an IDL argument was declared with a type this machine does not convert");
        r = step_tostring_run(ctx, &s->hdr, a, cb_result, slot, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;          /* parked ON THIS ARGUMENT; the resume comes back to it */
        if (r < 0) return JS_STEP_ABRUPT;
    placed:
        if (m->variadic) {
            JS_SetPropertyUint32(ctx, s->conv, (uint32_t)s->i, s->vstage);
            s->vstage = JS_UNDEFINED;
        }
        s->i++;
    }

    /* THE BODY RUNS HERE, NOT IN fini. Every declared argument is a real string now, so it has no user code
       left to reach — the claim the declaration makes. It cannot run in fini because the shared teardown
       releases hdr.this_val BEFORE calling it, so a body that reads the receiver there reads a freed value:
       the listener registration silently found no receiver and registered nothing, with no throw to show for
       it. A machine's fini may yield what it already computed; it may not compute. */
    /* THE BODY TAKES A CONTIGUOUS VECTOR, so a variadic member's converted arguments are copied out of the
       array into one. It lives only across the body call — the body cannot park, which is the whole reason
       this vector needs no ownership contract of its own; the array keeps owning the values. */
    if (m->variadic) {
        int k;
        /* Every converted argument reached the array, which is the one thing the single `placed:` store exists
           to guarantee — an arm that returns without going through it leaves a hole the body reads as
           undefined, and that is exactly what a missed one did. */
        {
            JSValue lv = JS_GetPropertyStr(ctx, s->conv, "length");
            uint32_t have = 0;
            JS_ToUint32(ctx, &have, lv);
            JS_FreeValue(ctx, lv);
            DCHECK((int)have == s->n,
                   "a variadic member converted fewer arguments than it was given — an arm of the conversion "
                   "returned without storing through `placed:`");
        }
        argv_vec = s->n ? js_malloc(ctx, sizeof(JSValue) * (size_t)s->n) : NULL;
        if (s->n && !argv_vec) { JS_FreeValue(ctx, cb_result); return JS_STEP_ABRUPT; }
        for (k = 0; k < s->n; k++) argv_vec[k] = JS_GetPropertyUint32(ctx, s->conv, (uint32_t)k);
    }
    if (!m->step) JS_FreeValue(ctx, cb_result);
    if (m->step) {
        /* The member's own algorithm, as a machine. It is re-entered on every resume with `i == n`, so the
           conversion loop above is skipped and the resume lands back inside the body — which is what makes the
           body's stage the SECOND resume point of this machine, beside the argument cursor. */
        r = m->step->body(ctx, &s->hdr, (char *)s + sizeof(JSIdlArgsState), s->n,
                          (JSValueConst *)(argv_vec ? argv_vec : s->args),
                          cb_result, &s->result, out_cb, out_argc);
        idl_free_vec(ctx, argv_vec, s->n);
        if (r > 0) return r;
        if (r < 0) { s->result = JS_UNDEFINED; return JS_STEP_ABRUPT; }
        return JS_STEP_DONE;
    }
    s->result = m->setter
        ? m->setter(ctx, s->hdr.this_val, s->n > 0 ? s->args[0] : JS_UNDEFINED, m->magic)
        : m->body(ctx, s->hdr.this_val, s->n, (JSValueConst *)(argv_vec ? argv_vec : s->args), m->magic);
    idl_free_vec(ctx, argv_vec, s->n);
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
    const IdlMember *m;
    int i;

    DCHECK(s->hdr.arg >= 0 && s->hdr.arg < g_n, "an IDL member's teardown ran with no pool entry behind it");
    m = &g_members[s->hdr.arg];
    /* The step body's state goes FIRST: it may hold values this machine's arguments are the only other
       reference to, and a release that runs after they are freed reads what it no longer owns. */
    if (m->step && m->step->release) m->step->release(ctx, (char *)st + sizeof(JSIdlArgsState));

    if (take_result) s->result = JS_UNDEFINED;
    JS_FreeValue(ctx, s->result);
    s->result = JS_UNDEFINED;
    JS_FreeValue(ctx, s->dict_v);   /* a member read whose conversion never completed — the throw path owns it */
    JS_FreeValue(ctx, s->conv);
    JS_FreeValue(ctx, s->vstage);
    s->dict_v = s->conv = s->vstage = JS_UNDEFINED;
    for (i = 0; i < IDL_MAX_ARGS; i++) {
        JS_FreeValue(ctx, s->args[i]);
        s->args[i] = JS_UNDEFINED;
    }
    return r;
}

JSValue idl_dict_get(JSContext *ctx, JSValueConst dict, const char *name)
{
    if (!JS_IsObject(dict)) return JS_UNDEFINED;
    return JS_GetPropertyStr(ctx, dict, name);
}

bool idl_dict_bool(JSContext *ctx, JSValueConst dict, const char *name)
{
    JSValue v = idl_dict_get(ctx, dict, name);
    bool b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}

int idl_method_id(JSContext *ctx, const IdlArgType *types, int nargs, IdlBody body, int magic)
{
    return idl_method_id_dict(ctx, types, nargs, NULL, 0, body, magic);
}

int idl_method_id_ext(JSContext *ctx, const IdlArgType *types, int nargs, bool variadic, JSClassID iface,
                      IdlBody body, int magic)
{
    int id = idl_method_id_dict(ctx, types, nargs, NULL, 0, body, magic);
    g_members[g_n - 1].variadic = variadic;
    g_members[g_n - 1].iface = iface;
    return id;
}

int idl_method_id_dict(JSContext *ctx, const IdlArgType *types, int nargs,
                       const IdlDictMember *members, int nmembers, IdlBody body, int magic)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    int idx, k;

    DCHECK(g_rt == NULL || g_rt == rt,
           "an IDL member was installed into a second runtime — its step ids belong to the first, and one WASM "
           "instance is one document");
    CHECK(g_n < IDL_MAX_MEMBERS,
          "the IDL member pool is full — either raise IDL_MAX_MEMBERS for a genuinely larger surface, or a "
          "member is being DECLARED more than once (declare in the component's init, install from the cached "
          "id; a per-wrapper install mints a definition per object)");
    g_rt = rt;
    idx = g_n++;
    CHECK(nargs >= 1 && nargs <= IDL_MAX_DECLARED,
          "a member declared more argument types than IDL_MAX_DECLARED holds");
    g_members[idx].body          = body;
    g_members[idx].setter        = NULL;
    g_members[idx].null_to_empty = false;
    g_members[idx].nargs = nargs;
    g_members[idx].magic = magic;
    for (k = 0; k < nargs; k++)
        g_members[idx].types[k] = types[k];
    g_members[idx].dict = members;
    g_members[idx].dict_n = 0;
    if (members) {
        int ndict = 0;
        for (k = 0; k < nargs; k++)
            if (types[k] == IDL_DICT || types[k] == IDL_DICT_OR_BOOL_FIRST) ndict++;
        DCHECK(ndict == 1, "a member declared dictionary members but not exactly one dictionary argument — the "
                           "conversion cursor is per-member, so a second dictionary would read the first's "
                           "names");
        CHECK(nmembers <= IDL_MAX_DICT, "a dictionary declared more members than IDL_MAX_DICT holds");
        for (k = 0; k < nmembers; k++)
            g_members[idx].dict_atoms[k] = JS_NewAtom(ctx, members[k].name);
        g_members[idx].dict_n = nmembers;
    }
    g_members[idx].step = NULL;
    g_members[idx].variadic = false;
    g_members[idx].iface = 0;
    g_defs[idx].size  = sizeof(JSIdlArgsState);
    g_defs[idx].step  = js_idl_args_step;
    g_defs[idx].fini  = idl_args_result;
    g_defs[idx].arg   = idx;
    g_defs[idx].visit = js_idl_args_visit;
    return JS_RegisterStepDef(rt, &g_defs[idx]);
}

int idl_method_id_step(JSContext *ctx, const IdlArgType *types, int nargs,
                       const IdlDictMember *members, int nmembers,
                       const IdlStepDecl *decl, int magic)
{
    int id = idl_method_id_dict(ctx, types, nargs, members, nmembers, NULL, magic);
    /* the pool entry idl_method_id_dict just filled — a step member differs only in WHAT runs once the
       conversions are done, and in needing room after this machine's state for that thing to run in. */
    g_members[g_n - 1].step = decl;
    g_defs[g_n - 1].size = sizeof(JSIdlArgsState) + decl->state_size;
    return id;
}

int idl_setter_id(JSContext *ctx, IdlArgType type, bool null_to_empty, IdlSetter body, int magic)
{
    int id = idl_method_id(ctx, &type, 1, NULL, magic);
    /* the pool entry idl_method_id just filled — a setter differs only in which body it runs and in the
       null-to-empty rule its type carries. */
    g_members[g_n - 1].setter        = body;
    g_members[g_n - 1].null_to_empty = null_to_empty;
    return id;
}

void idl_install_accessor(JSContext *ctx, JSValueConst target, const char *name,
                          IdlGetter getter, int getter_magic, int setter_stepid)
{
    JSAtom a = JS_NewAtom(ctx, name);
    JSValue g = JS_UNDEFINED, st = JS_UNDEFINED;

    DCHECK(a != JS_ATOM_NULL, "an IDL accessor name could not be interned");
    if (getter)
        g = JS_NewCFunction2(ctx, (JSCFunction *)getter, name, 0, JS_CFUNC_getter_magic, getter_magic);
    if (setter_stepid >= 0)
        st = JS_NewCFunction2(ctx, NULL, name, 1, JS_CFUNC_step, setter_stepid);
    JS_DefinePropertyGetSet(ctx, (JSValue)target, a, g, st,
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, a);
}

void idl_install_method(JSContext *ctx, JSValueConst target, const char *name, int length, int stepid)
{
    DCHECK(stepid >= 0, "an IDL member was installed before it was declared");
    JS_SetPropertyStr(ctx, (JSValue)target, name,
                      JS_NewCFunction2(ctx, NULL, name, length, JS_CFUNC_step, stepid));
}

/* The pool interns one atom per dictionary member, for the runtime's life — release them with it. */
void idl_args_free(JSContext *ctx)
{
    int i, k;
    for (i = 0; i < g_n; i++)
        for (k = 0; k < g_members[i].dict_n; k++)
            JS_FreeAtom(ctx, g_members[i].dict_atoms[k]);
    memset(g_members, 0, sizeof(g_members));
    memset(g_defs, 0, sizeof(g_defs));
    g_n = 0;
    g_rt = NULL;
}
