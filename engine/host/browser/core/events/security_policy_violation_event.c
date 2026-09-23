/* THE SecurityPolicyViolationEvent INTERFACE — Content Security Policy Level 3 §5.1 "Violation DOM Events".
 *
 *     enum SecurityPolicyViolationEventDisposition { "enforce", "report" };
 *     [Exposed=(Window,Worker)]
 *     interface SecurityPolicyViolationEvent : Event {
 *       constructor(DOMString type, optional SecurityPolicyViolationEventInit eventInitDict = {});
 *       readonly attribute USVString documentURI;
 *       readonly attribute USVString referrer;
 *       readonly attribute USVString blockedURI;
 *       readonly attribute DOMString effectiveDirective;
 *       readonly attribute DOMString violatedDirective;   // historical alias of effectiveDirective
 *       readonly attribute DOMString originalPolicy;
 *       readonly attribute USVString sourceFile;
 *       readonly attribute DOMString sample;
 *       readonly attribute SecurityPolicyViolationEventDisposition disposition;
 *       readonly attribute unsigned short statusCode;
 *       readonly attribute unsigned long lineNumber;
 *       readonly attribute unsigned long columnNumber;
 *     };
 *
 * WHY IT EXISTS AND WHAT WAS BLOCKED ON IT. Every refusal this engine makes on a page's behalf — a request
 * §4.1.2 blocks, an inline script or style §4.2.3 refuses, an `eval` §4.4.1 stops, a `<base href>` §6.3.1.1
 * rejects — ends in §5.5 "Report a violation", whose first observable is "fire an event named
 * securitypolicyviolation that uses the SecurityPolicyViolationEvent interface at target". There was no such
 * interface, so every one of those refusals happened in SILENCE: core/events/event_target.c has installed
 * `onsecuritypolicyviolation` for as long as it has had a handler table, and nothing in this tree ever wrote
 * it. A page that counts the event at its own document read zero across every block the engine made.
 *
 * TWELVE ATTRIBUTES AND A HANDLER READS THEM ALL. This is not an event whose type carries the information:
 * `blockedURI` is which address was refused, `effectiveDirective` is which rule refused it, `originalPolicy`
 * is the policy text, and `sample` is the first forty characters of the inline content. Firing a plain Event
 * here would not lose twelve properties quietly — it would make every one of them `undefined` and send a
 * handler's own `.startsWith` to a TypeError.
 *
 * `violatedDirective` IS NOT A SECOND FACT. §5.5 initializes it and `effectiveDirective` from the same value
 * and says so in its own note — "Both effectiveDirective and violatedDirective are the same value. This is
 * intentional to maintain backwards compatibility." — which is why the fire's field struct carries ONE field
 * and the interface declares TWO members. The CONSTRUCTOR is the other way round: §5.1's dictionary declares
 * both members separately, so a page that constructs one may set them to different strings and read both
 * back, and this file does not collapse what the IDL keeps apart.
 *
 * IT IS A REAL SUBCLASS: `SecurityPolicyViolationEvent.prototype.__proto__ === Event.prototype`, so
 * `e instanceof Event` holds and `initEvent` works on one. The base half is event_new_derived's.
 *
 * THE SLOTS ARE OWN PROPERTIES UNDER A PRIVATE SYMBOL, for the reason core/events/event.c gives: a slot
 * written as a property write is captured by the COW delta, so the event's state time-travels with the flow
 * that fired it, and the symbol is a brand a page cannot forge.
 *
 * THE INTERFACE OBJECT IS A PER-REALM INTRINSIC, declared into realm.h's one list beside the prototype —
 * §3.7 gives each realm its own interface OBJECT for the same reason it gives each its own prototype. */
#include <stdbool.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/events/event.h"
#include "core/events/security_policy_violation_event.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"

static JSValue   g_key = JS_UNDEFINED;   /* the private Symbol this interface's own slots hang off */
static JSClassID g_spv_class;   /* the class exists for its per-REALM prototype slot; nothing wears it */
static int       g_ready;
static int       g_ctor_stepid = -1;

static JSValue spv_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_spv_class);

    DCHECK(!JS_IsNull(proto),
           "SecurityPolicyViolationEvent.prototype was asked for in a realm that never ran its per-realm "
           "install");
    return proto;   /* OWNED */
}

/* THE MEMBER LIST, IN ONE PLACE, in the order §5.1 declares it — the getter's magic is its index into this
   list and the install walks it, so a member cannot arrive with a hand-written getter and no row, and a row
   cannot arrive with no getter. Every attribute "must return the value it was initialized to", read out of
   the one slot record, so `magic` IS the member and there is one body rather than twelve copies of the same
   brand check. The SLOT NAME is the IDL identifier, which is what makes the constructor's dictionary read and
   the fire's own write two spellings of one name rather than two names that can drift. */
#define SPV_MEMBERS(X)                            \
    X(DOCUMENT_URI,        "documentURI")         \
    X(REFERRER,            "referrer")            \
    X(BLOCKED_URI,         "blockedURI")          \
    X(EFFECTIVE_DIRECTIVE, "effectiveDirective")  \
    X(VIOLATED_DIRECTIVE,  "violatedDirective")   \
    X(ORIGINAL_POLICY,     "originalPolicy")      \
    X(SOURCE_FILE,         "sourceFile")          \
    X(SAMPLE,              "sample")              \
    X(DISPOSITION,         "disposition")         \
    X(STATUS_CODE,         "statusCode")          \
    X(LINE_NUMBER,         "lineNumber")          \
    X(COLUMN_NUMBER,       "columnNumber")

#define SPV_ENUM_ONE(id, str) SPV_##id,
#define SPV_NAME_ONE(id, str) str,

enum { SPV_MEMBERS(SPV_ENUM_ONE) SPV_N };
static const char *const SPV_SLOT_NAME[] = { SPV_MEMBERS(SPV_NAME_ONE) };

static JSValue js_spv_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSAtom k;
    JSValue slots, v;

    DCHECK(g_ready,
           "a SecurityPolicyViolationEvent attribute was read before "
           "security_policy_violation_event_init ran");
    DCHECK(magic >= 0 && magic < SPV_N,
           "a SecurityPolicyViolationEvent accessor was installed with a magic this interface has no member "
           "for — SPV_MEMBERS is the one list the install walks and the getter indexes, so a magic outside it "
           "is a hand-written row rather than a member");
    /* WEB IDL §3.7.6 Attributes' BRAND CHECK, and it is a TypeError rather than a DCHECK: a receiver is
       PAGE-SUPPLIED INPUT, the corpus pulls these getters off the prototype and applies them deliberately,
       and an assert on one would hand any page an abort switch for the dev engine. */
    if (!JS_IsObject(this_val))
        return JS_ThrowTypeError(ctx,
                                 "a SecurityPolicyViolationEvent attribute was read on something that is "
                                 "not one");
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL) return JS_EXCEPTION;
    if (JS_GetOwnSlot(ctx, &slots, this_val, k) <= 0) slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx,
                                 "a SecurityPolicyViolationEvent attribute was read on something that is "
                                 "not one");
    }
    v = JS_GetPropertyStr(ctx, slots, SPV_SLOT_NAME[magic]);
    JS_FreeValue(ctx, slots);
    return v;
}

/* The twelve own slots, placed on an event whose Event half is already built. `vals` is indexed by the SAME
   magic the getter uses, which is what makes a missing write a missing READ at the same index rather than a
   member that silently answers undefined. CONSUMES every value of `vals`, including on the failure path —
   there is one owner and it is this function, so a caller that built twelve values hands them all over in one
   act rather than freeing eleven of them itself on each of two roads out. Returns -1 with the throw live. */
static int spv_init_slots(JSContext *ctx, JSValueConst ev, JSValue *vals)
{
    JSValue slots = idl_slots_new(ctx);
    JSAtom k = JS_ValueToAtom(ctx, g_key);
    int i;

    if (JS_IsException(slots) || k == JS_ATOM_NULL) {
        JS_FreeValue(ctx, slots);
        if (k != JS_ATOM_NULL) JS_FreeAtom(ctx, k);
        for (i = 0; i < SPV_N; i++) JS_FreeValue(ctx, vals[i]);
        return -1;
    }
    for (i = 0; i < SPV_N; i++)
        JS_SetPropertyStr(ctx, slots, SPV_SLOT_NAME[i], vals[i]);   /* the slot record takes each reference */
    JS_SetProperty(ctx, (JSValue)ev, k, slots);
    JS_FreeAtom(ctx, k);
    return 0;
}

/* Did any of the twelve fail to build? A string allocation can throw, and a half-filled array must not reach
   the slot record — every value is freed together by the one owner above. */
static bool spv_vals_ok(JSValue *vals)
{
    int i;

    for (i = 0; i < SPV_N; i++)
        if (JS_IsException(vals[i])) return false;
    return true;
}

JSValue security_policy_violation_event_new_to_fire(JSContext *ctx,
                                                    const SecurityPolicyViolationEventFields *f)
{
    JSValue vals[SPV_N], tv, ev;
    int i;

    DCHECK(g_ready,
           "a SecurityPolicyViolationEvent was minted before security_policy_violation_event_init declared "
           "the interface");
    DCHECK(f != NULL, "CSP §5.5's fire was reached with no attribute list — every row of its initializer is "
                      "a value the violation already holds, so there is no shape of this call with nothing "
                      "to say");
    /* NOT `source_file`, which §5.5 states as nullable and §5.1 types USVString — see the header. Every other
       field is a real string in §2.4's own terms, so a NULL here is a caller that never placed it. */
    DCHECK(f->document_uri && f->referrer && f->blocked_uri && f->effective_directive && f->original_policy &&
           f->sample && f->disposition,
           "CSP §5.5's fire was reached with an unplaced attribute — §2.4 gives a violation's sample the "
           "empty string as its initial value and every other field here a string or a URL, so a NULL is a "
           "zero-filled struct rather than a violation state. Only source file may be absent");
    DCHECK(!strcmp(f->disposition, "enforce") || !strcmp(f->disposition, "report"),
           "CSP §5.5's fire was reached with a disposition outside §5.1's enumeration — "
           "SecurityPolicyViolationEventDisposition has exactly the two values and §2.2 gives a policy no "
           "third one, so a string outside them is a miscast field rather than a disposition");
    vals[SPV_DOCUMENT_URI]       = JS_NewString(ctx, f->document_uri);
    vals[SPV_REFERRER]           = JS_NewString(ctx, f->referrer);
    vals[SPV_BLOCKED_URI]        = JS_NewString(ctx, f->blocked_uri);
    vals[SPV_EFFECTIVE_DIRECTIVE] = JS_NewString(ctx, f->effective_directive);
    /* §5.5's note: the two are one value on a FIRED event, which is why the field struct has one field. */
    vals[SPV_VIOLATED_DIRECTIVE] = JS_NewString(ctx, f->effective_directive);
    vals[SPV_ORIGINAL_POLICY]    = JS_NewString(ctx, f->original_policy);
    vals[SPV_SOURCE_FILE]        = JS_NewString(ctx, f->source_file ? f->source_file : "");
    vals[SPV_SAMPLE]             = JS_NewString(ctx, f->sample);
    vals[SPV_DISPOSITION]        = JS_NewString(ctx, f->disposition);
    vals[SPV_STATUS_CODE]        = JS_NewUint32(ctx, f->status_code);
    vals[SPV_LINE_NUMBER]        = JS_NewUint32(ctx, f->line_number);
    vals[SPV_COLUMN_NUMBER]      = JS_NewUint32(ctx, f->column_number);
    tv = JS_NewString(ctx, "securitypolicyviolation");
    if (JS_IsException(tv) || !spv_vals_ok(vals)) {
        for (i = 0; i < SPV_N; i++) JS_FreeValue(ctx, vals[i]);
        JS_FreeValue(ctx, tv);
        return JS_EXCEPTION;
    }
    /* §5.5 writes `bubbles: true` and `composed: true` into the same initializer list as the twelve
       attributes, and writes no `cancelable`, so DOM's fire-an-event sets exactly those two flags. */
    ev = event_new_derived(ctx, spv_proto(ctx), tv, /*bubbles*/ true, /*cancelable*/ false,
                           /*composed*/ true, /*trusted*/ true);
    JS_FreeValue(ctx, tv);
    if (JS_IsException(ev)) {
        for (i = 0; i < SPV_N; i++) JS_FreeValue(ctx, vals[i]);
        return ev;
    }
    if (spv_init_slots(ctx, ev, vals) < 0) {   /* CONSUMES vals on both roads */
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    return ev;
}

/* ---- the constructor --------------------------------------------------------------------------------------
 *
 * `constructor(DOMString type, optional SecurityPolicyViolationEventInit eventInitDict = {})`.
 * SecurityPolicyViolationEventInit INHERITS EventInit, and Web IDL §3.2.17 converts a dictionary's members
 * with the INHERITED ones first and each level lexicographically among itself — which is the order this list
 * is in, and the order a page pins by throwing from one member's getter. The twelve derived members are in
 * LEXICOGRAPHIC order and NOT in the order §5.1's dictionary writes them: the IDL declares
 * `violatedDirective` before `effectiveDirective`, and the conversion reads `effectiveDirective` first. A
 * list written in IDL source order would run a page's two getters the wrong way round, which is observable
 * with one `throw` and nothing else.
 *
 * EVERY MEMBER CARRIES THE IDL's OWN DEFAULT, so `new SecurityPolicyViolationEvent("x").blockedURI` is the
 * empty string and `.statusCode` is 0 without this body deciding anything — §3.2.17 step 4.1.5 PLACES each
 * one, and a body that filled an absence itself would be inventing the value. */
IDL_ENUM_VALUES(SPV_DISPOSITION_VALUES, "enforce", "report");

static const IdlArgType SPV_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static const IdlDictMember SPV_INIT[] = {
    { "bubbles", IDL_BOOLEAN }, { "cancelable", IDL_BOOLEAN }, { "composed", IDL_BOOLEAN },
    { "blockedURI",         IDL_USVSTRING,      false, NULL, 1, NULL, IDL_DEFAULT_STRING, "" },
    { "columnNumber",       IDL_UNSIGNED_LONG,  false, NULL, 1, NULL, IDL_DEFAULT_ZERO },
    { "disposition",        IDL_ENUM,           false, SPV_DISPOSITION_VALUES, 1, NULL,
                                                                     IDL_DEFAULT_STRING, "enforce" },
    { "documentURI",        IDL_USVSTRING,      false, NULL, 1, NULL, IDL_DEFAULT_STRING, "" },
    { "effectiveDirective", IDL_DOMSTRING,      false, NULL, 1, NULL, IDL_DEFAULT_STRING, "" },
    { "lineNumber",         IDL_UNSIGNED_LONG,  false, NULL, 1, NULL, IDL_DEFAULT_ZERO },
    { "originalPolicy",     IDL_DOMSTRING,      false, NULL, 1, NULL, IDL_DEFAULT_STRING, "" },
    { "referrer",           IDL_USVSTRING,      false, NULL, 1, NULL, IDL_DEFAULT_STRING, "" },
    { "sample",             IDL_DOMSTRING,      false, NULL, 1, NULL, IDL_DEFAULT_STRING, "" },
    { "sourceFile",         IDL_USVSTRING,      false, NULL, 1, NULL, IDL_DEFAULT_STRING, "" },
    { "statusCode",         IDL_UNSIGNED_SHORT, false, NULL, 1, NULL, IDL_DEFAULT_ZERO },
    { "violatedDirective",  IDL_DOMSTRING,      false, NULL, 1, NULL, IDL_DEFAULT_STRING, "" },
};

static JSValue js_spv_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue vals[SPV_N], ev;
    int i;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor SecurityPolicyViolationEvent requires 'new'");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "SecurityPolicyViolationEvent constructor requires a type");
    /* THE DICTIONARY'S VALUES, ALREADY CONVERTED — read by the SLOT's own name, which is the IDL identifier
       and therefore the dictionary member's, so the two lists cannot drift apart. */
    for (i = 0; i < SPV_N; i++)
        vals[i] = idl_dict_get(ctx, init, SPV_SLOT_NAME[i]);
    /* DOM §2.5 "Constructing events" with THIS interface's prototype — an event the PAGE constructs is
       untrusted, and takes the three flags from the dictionary rather than from §5.5's initializer list. */
    ev = event_new_derived(ctx, spv_proto(ctx), argv[0],
                           idl_dict_bool(ctx, init, "bubbles"),
                           idl_dict_bool(ctx, init, "cancelable"),
                           idl_dict_bool(ctx, init, "composed"), /*trusted*/ false);
    if (JS_IsException(ev)) {
        for (i = 0; i < SPV_N; i++) JS_FreeValue(ctx, vals[i]);
        return ev;
    }
    if (spv_init_slots(ctx, ev, vals) < 0) {   /* CONSUMES vals on both roads */
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    return ev;
}

/* ---- install ----------------------------------------------------------------------------------------------- */

#define SPV_GETTER_ONE(id, str) JS_CGETSET_MAGIC_DEF(str, js_spv_get, NULL, SPV_##id),
static const JSCFunctionListEntry js_spv_proto[] = { SPV_MEMBERS(SPV_GETTER_ONE) };

void security_policy_violation_event_init(JSContext *ctx)
{
    JSClassDef d = { "SecurityPolicyViolationEvent" };

    DCHECK(!g_ready,
           "security_policy_violation_event_init ran twice — the interface is declared once per AGENT");
    /* The getter table and the slot-name table are ONE list walked twice, and this is what says so: the two
       are generated from SPV_MEMBERS, so a member added to the list arrives with both or neither. */
    DCHECK((int)(sizeof(js_spv_proto) / sizeof(js_spv_proto[0])) == SPV_N,
           "the SecurityPolicyViolationEvent accessor table and its member enumeration disagree about how "
           "many attributes §5.1 declares");
    g_key = JS_NewSymbol(ctx, "securityPolicyViolationEventSlots", false);
    CHECK(!JS_IsException(g_key), "the SecurityPolicyViolationEvent slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_spv_class);
    JS_NewClass(JS_GetRuntime(ctx), g_spv_class, &d);
    g_ctor_stepid = idl_method_id_dict(ctx, SPV_CTOR_ARGS, 2, SPV_INIT,
                                       (int)(sizeof(SPV_INIT) / sizeof(SPV_INIT[0])), js_spv_ctor, 0);
    idl_optional_from(1);              /* `optional SecurityPolicyViolationEventInit eventInitDict = {}` */
    g_ready = 1;
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — and it names the `event` row, not this file, for
       the reason core/agent_state.h gives and core/events/hash_change_event.c repeats: every Event subclass
       is declared by event_init and released by event_free, so the row whose RELEASE gives these slots back
       is core/platform.c's `event`. */
    agent_state_flag("event", &g_ready,
                     "the SecurityPolicyViolationEvent interface of CSP §5.1 \"Violation DOM Events\" — its "
                     "declaration latch");
    agent_state_class("event", &g_spv_class,
                      "the SecurityPolicyViolationEvent interface of CSP §5.1 \"Violation DOM Events\" — its "
                      "class, held for its per-realm prototype slot");
    agent_state_value("event", &g_key,
                      "the private Symbol the SecurityPolicyViolationEvent interface of CSP §5.1 "
                      "\"Violation DOM Events\" hangs its slot record off");
    agent_state_id("event", &g_ctor_stepid,
                   "the SecurityPolicyViolationEvent interface of CSP §5.1 \"Violation DOM Events\" — its "
                   "`constructor(DOMString type, optional SecurityPolicyViolationEventInit "
                   "eventInitDict = {})`");
    realm_declare_intrinsic(security_policy_violation_event_install_protos);
}

void security_policy_violation_event_install_protos(JSContext *ctx)
{
    JSValue proto, prev, base, ctor, global;

    DCHECK(g_ready,
           "a realm asked for SecurityPolicyViolationEvent before security_policy_violation_event_init "
           "declared it");
    prev = JS_GetClassProto(ctx, g_spv_class);
    DCHECK(JS_IsNull(prev),
           "security_policy_violation_event_install_protos ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "SecurityPolicyViolationEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "SecurityPolicyViolationEvent");
    JS_SetPropertyFunctionList(ctx, proto, js_spv_proto,
                               (int)(sizeof(js_spv_proto) / sizeof(js_spv_proto[0])));
    JS_SetClassProto(ctx, g_spv_class, JS_DupValue(ctx, proto));

    /* §3.7.1's interface object, on THIS realm's global — one `SecurityPolicyViolationEvent` per realm, whose
       `prototype` is the prototype this same install just built. §5.1 declares it `[Exposed=(Window,Worker)]`
       and every realm this engine builds is one of the two. */
    ctor = idl_step_constructor(ctx, "SecurityPolicyViolationEvent", g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the SecurityPolicyViolationEvent interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "SecurityPolicyViolationEvent", ctor);
    JS_FreeValue(ctx, global);
}

/* THE RUNTIME, NOT A REALM — core/platform.h's release column, reached through event_free. What this gives
   back is the AGENT's: a private Symbol, a class id and this interface's member declarations; every prototype
   it built is in some realm's class-proto slot and goes with that realm. */
void security_policy_violation_event_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;` — core/events/event.c's event_init calls this component's init on the ONE
       declaration pass and its event_free calls this release unconditionally, so the test could never be true
       and what it could do was hide a release that left the latch set. */
    DCHECK(g_ready,
           "the SecurityPolicyViolationEvent interface of CSP §5.1 \"Violation DOM Events\" was released in "
           "an agent that never declared it — event_init declares every Event subclass on the one "
           "unconditional pass");
    JS_FreeValueRT(rt, g_key);   /* the prototypes are the REALMS' — each is released with its context */
    g_key = JS_UNDEFINED;
    g_ready = 0;
    /* core/agent_state.h's one policy: a class id is given back like every other slot, because the id doubles
       as the init latch and a carried one names a class in a runtime that is gone. Nothing WEARS this class —
       it exists for its per-realm prototype slot — so there is no finalizer and no gc_mark here. */
    g_spv_class = 0;
    g_ctor_stepid = -1;
}
