/* PERMISSIONS §6.1 AND §6.2 — the Permissions interface and `query()`. See permissions.h. */

#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/dom/document.h"
#include "core/permissions/permissions.h"
#include "core/permissions/permission_status.h"
#include "core/permissions/permission_store.h"

static JSClassID g_permissions_class;
static int g_obj_slot = -1;
static int g_id_query = -1;
static JSAtom g_atom_name;
static JSRuntime *g_rt;

/* WEB IDL §3.7.5's BRAND CHECK, as core/frame/navigator.c's is and for its reason: a page tells a TypeError
   apart from `undefined`, and a feature detector that applies the method to a plain object reads the throw as
   "this is a real interface". */
static bool permissions_brand(JSContext *ctx, JSValueConst this_val)
{
    DCHECK(g_permissions_class != 0, "a Permissions member ran before permissions_init declared the class");
    if (JS_GetClassID(this_val) == g_permissions_class)
        return true;
    JS_ThrowTypeError(ctx, "Permissions.query was called on something that is not a Permissions object");
    return false;
}

/* ---- §6.2.1's query() ---------------------------------------------------------------------------------------
 *
 * "1. If this's relevant global object is a Window object, then: 1. If the current settings object's
 *     associated Document is not fully active, return a promise rejected with an InvalidStateError
 *     DOMException.
 *  2. Let rootDesc be the object permissionDesc refers to, converted to an IDL value of type
 *     PermissionDescriptor.  3. If the conversion throws an exception, return a promise rejected with that
 *     exception.  4. If rootDesc["name"] is not supported, return a promise rejected with a TypeError.
 *  5. Let typedDescriptor be the object permissionDesc refers to, converted to an IDL value of rootDesc's
 *     name's permission descriptor type.  6. If the conversion throws an exception, return a promise rejected
 *     with that exception.
 *  7. Let promise be a new promise.  8. Return promise and continue in parallel: 1. Let status be create a
 *     PermissionStatus with typedDescriptor.  2. Let query be status's [[query]] internal slot.  3. Run query's
 *     name's permission query algorithm, passing query and status.  4. Queue a global task on the permissions
 *     task source with this's relevant global object to resolve promise with status."
 *
 * THE CONVERSION IS THE BODY'S AND NOT THE ARGUMENT MACHINE'S, and the difference is where its throw lands.
 * The IDL declares `object permissionDesc` — so the DECLARED conversion is only "is it an object" — and steps
 * 2 and 5 then convert that object twice, once to `PermissionDescriptor` and once to the feature's own
 * descriptor type, INSIDE the algorithm, where a throw is a REJECTION. Declaring it as a dictionary argument
 * would convert it in the prologue, where a throw is a throw, and `query({get name(){throw 1}})` would go past
 * the `.catch` the page wrote. Every read is still a request, so a page's getter or Proxy trap suspends the
 * flow exactly as it does anywhere else.
 *
 * THE TWO CONVERSIONS ARE ONE PASS OVER THE OBJECT, which is what the spec's own steps describe: step 5's type
 * INHERITS from step 2's, so `name` is read once and the feature's aspect member — the one thing the derived
 * type adds — is read after it. A second literal read of `name` would be a second observable [[Get]] the
 * standard does not perform. */
#define PQ_STAGES(X)                                                                                           \
    X(PQ_NAME, "Permissions §6.2.1 step 2 (Get(permissionDesc, \"name\") — the required member of the "         \
               "PermissionDescriptor the object is converted to)")                                             \
    X(PQ_NAME_STR, "Permissions §6.2.1 step 2 (converting permissionDesc[\"name\"] to its DOMString)")          \
    X(PQ_SUBJECT, "Permissions §6.2.1 step 5 (converting permissionDesc to the permission descriptor type "     \
                  "rootDesc's name identifies — the REQUIRED member that type declares to identify which "     \
                  "instance of the feature the descriptor is about)")                                          \
    X(PQ_ASPECT, "Permissions §6.2.1 step 5 (converting permissionDesc to the permission descriptor type "      \
                 "rootDesc's name identifies — the one aspect member that type declares)")                     \
    X(PQ_ASPECT_STR, "Permissions §6.2.1 step 5 (Web IDL §3.2.18's ToString on an aspect member whose type is " \
                     "an enumeration)")                                                                        \
    X(PQ_RUN, "Permissions §6.2.1 steps 7-8 (a new promise; create a PermissionStatus with typedDescriptor, "   \
              "run the permission query algorithm, and queue a global task to resolve promise with status)")
enum { IDL_STEP_STAGE_BASE(PQ_STAGES) PQ_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const PQ_STEPS[] = { PQ_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSValue promise;
    JSValue funcs[2];   /* the capability's [resolve, reject] */
    JSValue name_v;     /* permissionDesc["name"] as read, then as its DOMString */
    /* THE SUBJECT MEMBER'S VALUE (owned) — the `required FileSystemHandle handle` of the one registered
       descriptor type that names one. It is held from step 5's read to step 8's descriptor because the
       descriptor BORROWS it, and the two are separated by the aspect member's own read (and by that read's
       possible suspension), so a borrowed reference to a value nothing on this state held would be a use after
       free the moment the page's getter dropped its last other reference. */
    JSValue subject_v;
    JSValue aspect_v;   /* the aspect member as read, then as its DOMString where the member is an enumeration */
    JSAtom  aspect;     /* the feature's aspect member, interned for the length of step 5's read */
    JSAtom  subject;    /* and its subject member, interned for the length of the same step */
    int     feature;
    uint8_t aspect_on;
    uint8_t started;
} PermQueryState;

static void pq_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    PermQueryState *s = st;

    /* THE ASPECT ATOM IS OWNED AND WAS NAMED BY NOTHING. Step 5's read of the feature's aspect member parks on
       the page's getter, so a concolic fork lands inside it — and the byte copy the fork makes would hand two
       arms one interned name with one reference, which both of them then free. It is visited BEFORE the
       started-guard because it is interned by step 5 whether or not the promise half of the algorithm has
       begun. */
    v->atom(ctx, &s->aspect);
    v->atom(ctx, &s->subject);
    if (!s->started)
        return;
    v->val(ctx, &s->promise);
    v->val(ctx, &s->funcs[0]);
    v->val(ctx, &s->funcs[1]);
    v->val(ctx, &s->name_v);
    v->val(ctx, &s->subject_v);
    v->val(ctx, &s->aspect_v);
}

/* WHICH STAGE STEP 5's CONVERSION STARTS AT, from the registry row alone. The derived permission descriptor
   type adds a SUBJECT member, an ASPECT member, both or neither, and Web IDL §3.2.17's lexicographic order over the
   derived type's own members puts `handle` before `mode` — so the one type that declares both is read in that
   order and every other type skips straight past what it does not declare. */
static int pq_after_name(PermQueryState *s)
{
    if (permission_feature_subject(s->feature)) return PQ_SUBJECT;
    if (permission_feature_aspect(s->feature))  return PQ_ASPECT;
    return PQ_RUN;
}

static void pq_intern_aspect(JSContext *ctx, PermQueryState *s)
{
    DCHECK(s->aspect == JS_ATOM_NULL, "§6.2.1 interned a feature's aspect member name twice — a re-entry at the "
                                      "same stage would leak the first, and the read is made once");
    s->aspect = JS_NewAtom(ctx, permission_feature_aspect(s->feature));
    CHECK(s->aspect != JS_ATOM_NULL, "a feature's aspect member name could not be interned");
}

/* EVERY FAILURE STEP OF §6.2.1, as the one operation all of them are: settle the promise this algorithm has
   already created with the exception that is live, and hand the promise back. The exception is a VALUE here
   rather than a completion — which is exactly what "return a promise rejected with that exception" means, and
   what makes `query(bad).catch(f)` run f. */
static int pq_reject(JSContext *ctx, PermQueryState *s, JSValue *presult)
{
    JSValue exc = JS_GetException(ctx);

    DCHECK(s->started, "§6.2.1 rejected before it created its promise — steps 1 through 6 all settle the "
                       "promise step 7 creates, so the capability is built before the first thing that can "
                       "fail rather than at the step the spec numbers it");
    if (JS_CallAsFlow(ctx, s->funcs[1], exc) < 0)
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, exc);
    *presult = s->promise;
    s->promise = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static int pq_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                   JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    PermQueryState *s = st;
    JSValueConst desc = argc > 0 ? argv[0] : JS_UNDEFINED;
    PermissionDescriptor d;
    int r;

    /* THE ONE-TIME PROLOGUE IS GUARDED BY `started` AND NOT BY THE STAGE, and the difference is a leak and a
       lost promise. A request that SUSPENDS re-enters this function at the SAME stage, so everything a stage
       does before its request runs again — and here that would mint a second promise capability over the first,
       hand the page a promise nothing ever settles, and answer the brand check twice. A stage says where the
       machine RESTS; it does not say what has already happened.
       EVERY ANSWER SLOT IS SPELLED HERE, on the one entry every path passes through — a zeroed step state's
       JSValue is the INTEGER 0 and not JS_UNDEFINED, so an "is it set yet" test on a slot always reads as
       already-set. */
    if (!s->started) {
        s->promise = s->funcs[0] = s->funcs[1] = s->name_v = JS_UNDEFINED;
        s->subject_v = s->aspect_v = JS_UNDEFINED;
        s->aspect = s->subject = JS_ATOM_NULL;
        s->feature = -1;
        s->aspect_on = 0;
        /* STEP 7's PROMISE, CREATED FIRST. See pq_reject: every failure step of this algorithm settles it. */
        s->promise = JS_NewPromiseCapability(ctx, s->funcs);
        CHECK(!JS_IsException(s->promise),
              "§6.2.1's promise capability could not be allocated — a query that answers with neither a "
              "promise nor a throw is a call a page can only hang on");
        s->started = 1;
        /* WEB IDL §3.7.7's BRAND CHECK, which every operation performs before its algorithm: "If esValue does
           not implement the interface, throw a TypeError." The same section makes that a REJECTION here rather
           than a throw, because the operation's return type is a promise — which is why it is asked AFTER the
           capability exists rather than at the top of the member. */
        if (!permissions_brand(ctx, hdr->this_val)) {
            JS_FreeValue(ctx, cb_result);
            return pq_reject(ctx, s, presult);
        }
        /* STEP 1: "If the current settings object's associated Document is not fully active, return a promise
           rejected with an InvalidStateError DOMException." Every global this engine has is a Window, so
           step 1's outer condition is always true. */
        if (!document_fully_active(ctx)) {
            JS_FreeValue(ctx, cb_result);
            JS_ThrowDOMException(ctx, "InvalidStateError", "%s",
                                 "the document is not fully active, so its permissions cannot be queried");
            return pq_reject(ctx, s, presult);
        }
        /* WEB IDL'S `object` — the declared type of the one argument, and the only conversion the prologue
           would have performed. A non-object is a TypeError, and §3.7.7 makes that a REJECTION because the
           operation returns a promise. */
        if (!JS_IsObject(desc)) {
            JS_FreeValue(ctx, cb_result);
            JS_ThrowTypeError(ctx, "the permission descriptor passed to Permissions.query is not an object");
            return pq_reject(ctx, s, presult);
        }
    }
    /* AN ABRUPT REQUEST RESULT ARRIVES AT THE HELPER THAT PARKED, AS ITS OWN -1 — and it is taken THERE, at
       each `if (r < 0) return pq_reject(...)` below, rather than by one test at the top of this function.
       A blanket `if (JS_IsException(cb_result)) return pq_reject(...)` stood here and is deleted. It was a
       workaround for a driver-side rewind (`step_hdr_request_abandon`) that no longer exists: quickjs-step.h's
       request contract now says an abrupt keyed or coercion completion arrives as the helper's own -1, having
       ENDED the request first — the cursor rewound and the key released by the same lines that end a normal
       completion. Consuming the delivery before the helper runs leaves the request recorded as IN FLIGHT, so
       `step_keyed_abrupt`'s reset never runs and the two asserts that exist to name a stage collecting another
       stage's answer — `step_getprop_done`'s key check and `step_keyed_answered`'s stage check — are disarmed
       on exactly the paths where a page's getter throws. It also made every `r < 0` branch below unreachable:
       five sites that read as the contract while a sixth quietly decided instead of them, which is the seam
       §C-stack is about rather than a style point. */

    if (hdr->stage == PQ_NAME) {
        /* STEP 2: `required DOMString name`. The read is one accessor or Proxy trap away from the page's own
           code, which is why it is a request and not a JS_GetPropertyStr. */
        r = step_getprop_run(ctx, hdr, desc, g_atom_name, cb_result, &s->name_v, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return pq_reject(ctx, s, presult);
        cb_result = JS_UNDEFINED;
        STEP_GOTO(hdr->stage, PQ_NAME_STR, &hdr->get_phase, &hdr->str_phase, NULL);
    }
    if (hdr->stage == PQ_NAME_STR) {
        /* WEB IDL §3.2.17: a REQUIRED dictionary member the object does not have is a TypeError, and
           `undefined` counts as not having it — which is why this is tested before the ToString rather than
           after, where it would become the four characters "undefined" and then an unsupported name. */
        if (JS_IsUndefined(s->name_v)) {
            JS_FreeValue(ctx, cb_result);
            JS_ThrowTypeError(ctx, "the permission descriptor has no `name`, which PermissionDescriptor "
                                   "declares as a required member");
            return pq_reject(ctx, s, presult);
        }
        if (!JS_IsString(s->name_v)) {
            JSValue str;

            r = step_tostring_run(ctx, hdr, s->name_v, cb_result, &str, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return pq_reject(ctx, s, presult);
            JS_FreeValue(ctx, s->name_v);
            s->name_v = str;
        } else {
            JS_FreeValue(ctx, cb_result);
        }
        cb_result = JS_UNDEFINED;
        /* STEP 4: "If rootDesc["name"] is not supported, return a promise rejected with a TypeError." The
           spec's own note explains why this is a DOMString and not an enum: "browsers vary greatly in the
           powerful features they support", so the SET is this user agent's and an unsupported name is the
           answer a feature-detecting bundle reads. */
        {
            const char *nm = JS_ToCString(ctx, s->name_v);

            s->feature = permission_feature_of(nm);
            if (s->feature < 0) {
                JS_ThrowTypeError(ctx, "'%s' is not a powerful feature this user agent supports",
                                  nm ? nm : "");
                if (nm) JS_FreeCString(ctx, nm);
                return pq_reject(ctx, s, presult);
            }
            if (nm) JS_FreeCString(ctx, nm);
        }
        /* STEP 5's CONVERSION, AND ITS ORDER IS WEB IDL'S. §3.2.17 reads a dictionary's INHERITED members
           first — `name`, above — and then the derived type's OWN members LEXICOGRAPHICALLY among themselves,
           so `FileSystemPermissionDescriptor`'s two are read `handle` then `mode`. The derived type adds at
           most a SUBJECT and at most an ASPECT, and a feature whose type is the default `PermissionDescriptor`
           adds neither. */
        /* THE DESTINATION IS COMPUTED INTO A LOCAL FIRST, because STEP_GOTO evaluates it twice (once to ask
           whether the stage MOVES and once to assign). Both of these are pure, so the value would be the same
           — but a destination whose expression is written inline is one edit away from being neither. */
        {
            int to = pq_after_name(s);

            STEP_GOTO(hdr->stage, to, &hdr->get_phase, &hdr->str_phase, NULL);
        }
        if (hdr->stage == PQ_SUBJECT) {
            s->subject = JS_NewAtom(ctx, permission_feature_subject(s->feature));
            CHECK(s->subject != JS_ATOM_NULL, "a feature's subject member name could not be interned");
        } else if (hdr->stage == PQ_ASPECT) {
            pq_intern_aspect(ctx, s);
        }
    }
    if (hdr->stage == PQ_SUBJECT) {
        r = step_getprop_run(ctx, hdr, desc, s->subject, cb_result, &s->subject_v, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return pq_reject(ctx, s, presult);
        cb_result = JS_UNDEFINED;
        /* WEB IDL §3.2.17 again: a `required` member the object does not have is a TypeError, and `undefined`
           IS not having it. */
        if (JS_IsUndefined(s->subject_v)) {
            JS_ThrowTypeError(ctx, "a permission descriptor for '%s' has no `%s`, which its permission "
                                   "descriptor type declares as a required member",
                              permission_feature_name(s->feature), permission_feature_subject(s->feature));
            return pq_reject(ctx, s, presult);
        }
        /* Web IDL §3.2.15's INTERFACE conversion: "a platform object implementing the interface crosses as itself and
           anything else is a TypeError". The brand belongs to the component that owns the interface, which is
           why the registry holds a test rather than a class id. */
        if (!permission_subject_is(s->feature, s->subject_v)) {
            JS_ThrowTypeError(ctx, "the `%s` of a permission descriptor for '%s' is not of the interface type "
                                   "its permission descriptor type declares",
                              permission_feature_subject(s->feature), permission_feature_name(s->feature));
            return pq_reject(ctx, s, presult);
        }
        {
            int to = permission_feature_aspect(s->feature) ? PQ_ASPECT : PQ_RUN;

            STEP_GOTO(hdr->stage, to, &hdr->get_phase, &hdr->str_phase, NULL);
        }
        if (hdr->stage == PQ_ASPECT) pq_intern_aspect(ctx, s);
    }
    if (hdr->stage == PQ_ASPECT) {
        r = step_getprop_run(ctx, hdr, desc, s->aspect, cb_result, &s->aspect_v, out_cb, out_argc);
        if (r > 0) return r;
        if (r < 0) return pq_reject(ctx, s, presult);
        cb_result = JS_UNDEFINED;
        /* AN ABSENT MEMBER TAKES THE IDL'S DEFAULT, whichever type it is: `= false` for a boolean, and the
           enumeration's first listed value for `FileSystemPermissionMode mode = "read"` — both of which are
           aspect bit 0, which is what makes the two types one bit. */
        if (JS_IsUndefined(s->aspect_v)) {
            s->aspect_on = 0;
            STEP_GOTO(hdr->stage, PQ_RUN, &hdr->get_phase, &hdr->str_phase, NULL);
        } else if (!permission_feature_aspect_values(s->feature)) {
            /* `boolean X = false`: ToBoolean, which runs none of the page's code, so there is nothing after
               this read to rest on. */
            s->aspect_on = (uint8_t)JS_ToBool(ctx, s->aspect_v);
            STEP_GOTO(hdr->stage, PQ_RUN, &hdr->get_phase, &hdr->str_phase, NULL);
        } else {
            /* Web IDL §3.2.18's ToString may be the page's, so it rests */
            STEP_GOTO(hdr->stage, PQ_ASPECT_STR, &hdr->get_phase, &hdr->str_phase, NULL);
        }
    }
    if (hdr->stage == PQ_ASPECT_STR) {
        const char *const *values = permission_feature_aspect_values(s->feature);
        const char *v;

        DCHECK(values != NULL, "§6.2.1 reached the enumeration conversion for an aspect member whose registry "
                               "row declares no enumeration");
        if (!JS_IsString(s->aspect_v)) {
            JSValue str;

            r = step_tostring_run(ctx, hdr, s->aspect_v, cb_result, &str, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return pq_reject(ctx, s, presult);
            JS_FreeValue(ctx, s->aspect_v);
            s->aspect_v = str;
        } else {
            JS_FreeValue(ctx, cb_result);
        }
        cb_result = JS_UNDEFINED;
        /* Web IDL §3.2.18: "if S is not one of the enumeration's values, throw a TypeError". Never the default — an
           unrecognised value is an error, and silently taking `read` for it would answer a question the page
           did not ask. */
        v = JS_ToCString(ctx, s->aspect_v);
        if (!v || (strcmp(v, values[0]) && strcmp(v, values[1]))) {
            JS_ThrowTypeError(ctx, "'%s' is not a value of the enumeration `%s` names",
                              v ? v : "", permission_feature_aspect(s->feature));
            if (v) JS_FreeCString(ctx, v);
            return pq_reject(ctx, s, presult);
        }
        s->aspect_on = (uint8_t)(strcmp(v, values[1]) == 0);
        JS_FreeCString(ctx, v);
        STEP_GOTO(hdr->stage, PQ_RUN, &hdr->get_phase, &hdr->str_phase, NULL);
    }
    DCHECK(hdr->stage == PQ_RUN, "§6.2.1's query resumed into a stage it does not have");
    JS_FreeValue(ctx, cb_result);
    DCHECK(s->feature >= 0, "§6.2.1 reached step 8 with no feature — step 4 rejects an unsupported name and "
                            "returns, so a descriptor here always names a registry row");
    d.feature = s->feature;
    d.aspect = s->aspect_on != 0;
    d.subject = s->subject_v;
    /* STEP 8.1-8.3: create a PermissionStatus with typedDescriptor and run the permission query algorithm over
       its [[query]] slot — both of which permission_status_new performs, because §6.3.4's update steps perform
       the identical pair and two copies of one algorithm is one of them being wrong later. */
    {
        JSValue status = permission_status_new(ctx, &d);
        JSValueConst args[1];

        /* STEP 8.4: "Queue a global task on the permissions task source with this's relevant global object to
           resolve promise with status." A JOB, so the settle runs as a call-root flow like every other one. */
        args[0] = status;
        JS_EnqueueCallJob(ctx, s->funcs[0], 1, args);
        JS_FreeValue(ctx, status);
    }
    *presult = s->promise;
    s->promise = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static const IdlStepDecl PQ_DECL = {
    pq_step, sizeof(PermQueryState), pq_visit, NULL,
    "Permissions §6.2.1 query(permissionDesc)", PQ_STEPS,
    /* catches_abrupt: §6.2.1's every failure step REJECTS. A read that throws after suspending would otherwise
       tear this machine down and propagate the throw synchronously, past the `.catch` the page wrote. */
    1
};

/* ---- §6.2's interface, per realm --------------------------------------------------------------------------- */

JSValue permissions_object(JSContext *ctx)
{
    return realm_value_get(ctx, g_obj_slot);   /* OWNED — realm_value_get asserts the realm ran its install */
}

static void permissions_install_realm(JSContext *ctx)
{
    JSValue proto, prev, global, obj;

    prev = JS_GetClassProto(ctx, g_permissions_class);
    DCHECK(JS_IsNull(prev), "permissions_install_realm ran twice in one realm — everything already holding the "
                            "first Permissions.prototype would answer out of a discarded object");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "Permissions.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "Permissions");
    DCHECK(g_id_query >= 0, "§6.2's query was installed on a realm's prototype before permissions_init "
                            "declared it");
    idl_install_method(ctx, proto, "query", 1, g_id_query);
    JS_SetClassProto(ctx, g_permissions_class, JS_DupValue(ctx, proto));

    /* §3.7.1's INTERFACE OBJECT on THIS realm's global. §6.2 declares no constructor. */
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "Permissions", idl_interface_object(ctx, "Permissions", proto));
    JS_FreeValue(ctx, global);

    /* §6.1's `[SameObject]` object, minted WITH the realm — so `navigator.permissions ===
       navigator.permissions` holds by construction and no flow can make its own first read into every
       sibling's baseline. */
    obj = JS_NewObjectProtoClass(ctx, proto, g_permissions_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "the realm's Permissions object could not be allocated");
    realm_value_set(ctx, g_obj_slot, obj);
}

void permissions_init(JSContext *ctx)
{
    JSClassDef def = { "Permissions" };
    JSRuntime *rt = JS_GetRuntime(ctx);
    static const IdlArgType PQ_ARGS[] = { IDL_ANY };

    DCHECK(g_obj_slot < 0, "permissions_init ran twice — the class, the slot and the member's pool id are the "
                           "AGENT's");
    g_rt = rt;
    /* §3's MODEL AND §6.3's INTERFACE ARE THIS COMPONENT'S DEPENDENCIES, declared here rather than by each
       host: a host that installed Permissions and not PermissionStatus would answer a query with an object
       that has no prototype, which is the hand-copied list core/realm.h exists to abolish. */
    permission_store_init(ctx);
    permission_status_init(ctx);
    JS_NewClassID(rt, &g_permissions_class);
    CHECK(JS_NewClass(rt, g_permissions_class, &def) == 0,
          "Permissions: the per-realm prototype slot could not be declared");
    g_obj_slot = realm_value_declare(ctx, "Permissions §6.1 the Navigator's Permissions object");
    g_atom_name = JS_NewAtom(ctx, "name");
    CHECK(g_atom_name != JS_ATOM_NULL, "PermissionDescriptor's `name` could not be interned");
    g_id_query = idl_method_id_step(ctx, PQ_ARGS, 1, NULL, 0, &PQ_DECL, 0);
    /* §6.1's `Promise<PermissionStatus> query(object permissionDesc)`. The return type is a DECLARATION now,
       so Web IDL §3.7.7 turns the prologue's arity TypeError into a rejection in the one place that rule
       belongs. This used to be `idl_optional_from(0)` — the one argument declared OPTIONAL so the count check
       could not throw at all — which produced the same answer for `query()` by making the member's IDL say
       something it does not say, and which each new promise-returning member had to remember. */
    idl_returns_promise();
    realm_declare_intrinsic(permissions_install_realm);
}

void permissions_free(void)
{
    if (g_obj_slot < 0)
        return;
    DCHECK(g_rt != NULL, "Permissions was declared without recording the runtime its atom belongs to");
    JS_FreeAtomRT(g_rt, g_atom_name);
    g_atom_name = JS_ATOM_NULL;
    g_obj_slot = -1;
    g_id_query = -1;
    permission_status_free();
    permission_store_free();
    g_rt = NULL;
}
