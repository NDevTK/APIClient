/* See css_keyword_value.h. CSS Typed OM Level 1 §4.2 "CSSKeywordValue objects" and §6.2 "CSSKeywordValue
   Serialization". */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "check.h"
#include "quickjs.h"

#include "core/agent_state.h"
#include "core/css/css_keyword_value.h"
#include "core/idl_args.h"
#include "solver/concolic.h"
#include "solver/cow.h"

/* §4.2's ONE INTERNAL SLOT.
 *
 * It is a JSValue and not a `char *` — see css_keyword_value.h for the whole argument. In one line: §4.2
 * declares `attribute USVString value` WRITABLE, the position is one unknown external input reaches, and
 * core/idl_args.h's string boundary passes such input across AS ITSELF so that opacity survives the
 * conversion. */
typedef struct CssKeywordValueData {
    JSValue value;   /* §4.2's `value` internal slot — a JS string, or unknown external input. OWNED */
} CssKeywordValueData;

/* THE OFFSET LIST IS THE SAME LIST THE FINALIZER FREES AND THE MARK WALKS, which is what makes a field added
   to one and not the others impossible to miss — core/css/css_unit_value.c's rule for its own record. */
static const uint16_t KW_VALS[] = { (uint16_t)offsetof(CssKeywordValueData, value) };
static const CowRecord KW_REC = { sizeof(CssKeywordValueData), KW_VALS, 1 };

static JSClassID g_kw_class;
static int g_id_ctor      = -1;
static int g_id_value_set = -1;

/* ---- the record, and the COW capture every member reaches it through -------------------------------------- */

/* THE CAPTURE IS HERE AND NOT AT THE ONE WRITE, which is solver/cow.h's rule: a record a flow has REACHED is
   one it may write, the delta dedups to one entry, and there is then no write site left to miss. §4.2 declares
   `value` writable, so this interface has a real second writer today — `kw.value = "grid"`. */
static CssKeywordValueData *kw_of(JSValueConst v)
{
    CssKeywordValueData *k = JS_GetOpaque(v, g_kw_class);

    if (k) cow_capture_host_record(v, k, &KW_REC);
    return k;
}

bool css_keyword_value_is(JSValueConst v)
{
    return g_kw_class != 0 && JS_GetClassID(v) == g_kw_class;
}

/* WEB IDL §3.7.6 "Attributes"' BRAND CHECK. `CSSKeywordValue.prototype.value` read off `{}` is a TypeError,
   and a page tells that apart from `undefined`. It is a TypeError and NOT a DCHECK because the receiver is
   PAGE-SUPPLIED input: asserting on it would hand any page an abort switch for this agent. */
static CssKeywordValueData *kw_here(JSContext *ctx, JSValueConst v)
{
    CssKeywordValueData *k = kw_of(v);

    if (!k) {
        JS_ThrowTypeError(ctx, "a CSSKeywordValue member was reached on something that is not a "
                               "CSSKeywordValue");
        return NULL;
    }
    return k;
}

/* THE COLLECTOR'S TWO ENTRIES READ NO STATIC THIS COMPONENT'S RELEASE RESETS — core/agent_state.h's rule. Both
   run AFTER core/platform.c's release column, so a keyword value a page still holds would be finalized with
   `g_kw_class` already back at 0 and `JS_GetOpaque(val, 0)` would answer NULL: the record and its counted
   value reference would leak, and an unmarked child keeps the internal reference gc_decref exists to subtract.
   JS_GetAnyOpaque, because the collector dispatched here THROUGH the class. */
static void kw_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    CssKeywordValueData *k = JS_GetAnyOpaque(val, &id);

    (void)id;
    /* NOT `if (!k) return;`. css_keyword_value_new is the one mint and it sets the record with nothing in
       between that allocates in the JS heap. */
    DCHECK(k != NULL, "a CSSKeywordValue was finalized with no record — this component's one mint sets it with "
                      "nothing in between that could collect");
    JS_FreeValueRT(rt, k->value);
    free(k);
}

static void kw_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID id = 0;
    CssKeywordValueData *k = JS_GetAnyOpaque(val, &id);

    (void)id;
    DCHECK(k != NULL, "a CSSKeywordValue was marked with no record — its `value` slot is a counted reference "
                      "and an unmarked child is read by gc_scan as rooted from outside the heap");
    JS_MarkValue(rt, k->value, mark_func);
}

JSValue css_keyword_value_new(JSContext *ctx, JSValue value)
{
    JSValue proto, obj;
    CssKeywordValueData *k;

    DCHECK(g_kw_class != 0, "a CSSKeywordValue was built before css_keyword_value_init declared the interface");
    DCHECK(JS_IsString(value) || concolic_is(value),
           "§4.2's `value` internal slot was set to something that is neither a string nor unknown external "
           "input. Web IDL §3.2.12's USVString conversion produces the first and passes the second through as "
           "itself, and those are the only two things that reach a mint — so anything else is a caller that "
           "skipped the declaration and is about to make `.value` answer a thing no page can concatenate");
    proto = JS_GetClassProto(ctx, g_kw_class);
    DCHECK(!JS_IsNull(proto), "a CSSKeywordValue was built in a realm with no CSSKeywordValue.prototype");
    obj = JS_NewObjectProtoClass(ctx, proto, g_kw_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "css-typed-om: a CSSKeywordValue could not be allocated");
    k = calloc(1, sizeof *k);
    CHECK(k != NULL, "css-typed-om: a CSSKeywordValue's record allocation failed");
    k->value = value;
    JS_SetOpaque(obj, k);
    return obj;
}

/* ---- §6.2 CSSKeywordValue Serialization ------------------------------------------------------------------- */

/* THE WHOLE SECTION IS ONE STEP — "Return this's value internal slot" — so this is a dup and nothing else.
   That it needs no derivation over an unknown slot is argued in css_keyword_value.h: §6.2 performs no
   operation, so there is no result to derive and the serialization of an unknown keyword IS that unknown. */
JSValue css_keyword_value_serialize(JSContext *ctx, JSValueConst v)
{
    CssKeywordValueData *k = kw_of(v);

    DCHECK(k != NULL, "§6's serialize-a-CSSStyleValue reached §6.2's arm on something that is not a "
                      "CSSKeywordValue — the caller asks css_keyword_value_is before it asks this");
    return JS_DupValue(ctx, k->value);
}

/* ---- §4.2's constructor and its one attribute -------------------------------------------------------------- */

/* §4.2's FIRST STEP, SHARED BY THE CONSTRUCTOR AND THE SETTER — "If value is an empty string, throw a
 * TypeError." Both members state it in those same words, so it is one predicate rather than two that can
 * drift.
 *
 * THE BYTES COME THROUGH concolic_name_cstr, which is what every member that needs the TEXT of a possibly-
 * unknown string argument reads it through: Web IDL's boundary passes unknown external input across as itself,
 * so JS_ToCString on this position would owe C a string it does not have. For a real string those bytes ARE
 * the string, so the check is exact. For unknown external input the answer is the SHAPE — a real string,
 * stable per source — and the arm that shape takes is the NON-THROWING one, which is a decision and not an
 * accident:
 *   CLAUDE.md §C-stack states the rule for a builtin's own branch on an opaque operand — it must NOT fork
 *   that branch, it short-circuits. So one arm has to be chosen here, and throwing would be choosing to
 *   DELETE a world nothing contradicted: a page's `new CSSKeywordValue(location.hash.slice(1))` denotes a
 *   real keyword in every run where the fragment is not empty, and refusing it takes the object, its
 *   provenance and every branch downstream of it with it. Carrying it keeps the unknown as the slot's own
 *   value, which is what makes §6.2 answer an unknown string rather than an invented one. */
static bool kw_value_is_empty(JSContext *ctx, JSValueConst v)
{
    const char *s = concolic_name_cstr(ctx, v);
    bool empty;

    CHECK(s != NULL, "css-typed-om: OOM encoding the `value` of a CSS Typed OM 1 §4.2 member");
    empty = (*s == '\0');
    JS_FreeCString(ctx, s);
    return empty;
}

/* "The CSSKeywordValue(value) constructor must, when called, perform the following steps: If value is an empty
   string, throw a TypeError. Otherwise, return a new CSSKeywordValue with its value internal slot set to
   value." Both steps, in order, and there is no third. */
static JSValue js_css_keyword_value_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                         int magic)
{
    (void)this_val; (void)magic;
    DCHECK(argc == 1, "§4.2's constructor reached its body with an argument count its IDL does not declare — "
                      "`value` is the one required position and there are no others, so the conversion machine "
                      "owed this body exactly one argument");
    if (kw_value_is_empty(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "CSSKeywordValue's value cannot be the empty string");
    return css_keyword_value_new(ctx, JS_DupValue(ctx, argv[0]));
}

/* §4.2 gives `value` no getter steps of its own, so Web IDL §3.7.6 "Attributes"' default applies: return the
   internal slot the setter writes. */
static JSValue js_css_keyword_value_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    CssKeywordValueData *k = kw_here(ctx, this_val);

    (void)magic;
    if (!k) return JS_EXCEPTION;
    return JS_DupValue(ctx, k->value);
}

/* "The value attribute of a CSSKeywordValue this must, on setting a value value, perform the following steps:
   If value is an empty string, throw a TypeError. Otherwise, set this's value internal slot, to value."
   §4.2 STATES SETTER STEPS OF ITS OWN, so this is NOT Web IDL §3.7.6's default store: the empty-string
   refusal is the attribute's own algorithm and runs on every assignment, which is why `kw.value = ""`
   throws where an ordinary USVString attribute would simply hold the empty string. The CONVERSION is the
   DECLARATION's and has already run — IDL_USVSTRING is ToString and then Web IDL §3.2.12's scalar value
   replacement — and unknown external input crosses as itself exactly as it does through the constructor's
   one position. */
static JSValue js_css_keyword_value_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    CssKeywordValueData *k = kw_here(ctx, this_val);

    (void)magic;
    if (!k) return JS_EXCEPTION;
    DCHECK(JS_IsString(val) || concolic_is(val),
           "§4.2's `value` setter reached its body with something that is neither a string nor unknown "
           "external input — IDL_USVSTRING produces the first and passes the second through, and there is no "
           "third thing for a declared USVString position to be");
    if (kw_value_is_empty(ctx, val))
        return JS_ThrowTypeError(ctx, "CSSKeywordValue's value cannot be the empty string");
    JS_FreeValue(ctx, k->value);
    k->value = JS_DupValue(ctx, val);
    return JS_UNDEFINED;
}

/* ---- the per-realm install --------------------------------------------------------------------------------- */

void css_keyword_value_install_realm(JSContext *ctx, JSValueConst style_value_proto)
{
    JSValue kw_proto, ctor, global, prev;

    DCHECK(g_kw_class != 0, "a realm asked for CSSKeywordValue before the interface was declared");
    prev = JS_GetClassProto(ctx, g_kw_class);
    DCHECK(JS_IsNull(prev), "css_keyword_value_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    DCHECK(!JS_IsNull(style_value_proto) && !JS_IsUndefined(style_value_proto),
           "Web IDL §3.7.3's inherited-interface arm was handed no CSSStyleValue.prototype — the caller owns "
           "that "
           "object and passes it precisely because §4.2 declares `CSSKeywordValue : CSSStyleValue`");

    /* Web IDL §3.7.3 "Interface prototype object":
       "if interface is declared to inherit from another interface, then set proto to the interface prototype
       object in realm of that inherited interface" — the object the
       caller built, which core/idl_args.c asserts by reading the Web IDL §3.7.3 class string back off it. */
    kw_proto = JS_NewObjectProto(ctx, style_value_proto);
    CHECK(!JS_IsException(kw_proto), "CSSKeywordValue.prototype could not be allocated");
    idl_interface_tag(ctx, kw_proto, "CSSKeywordValue");
    idl_install_accessor_no_user_code(ctx, kw_proto, "value", js_css_keyword_value_get, 0, g_id_value_set);
    JS_SetClassProto(ctx, g_kw_class, JS_DupValue(ctx, kw_proto));

    /* Web IDL §3.7.1 "Interface object" for an interface that DECLARES a constructor. */
    global = JS_GetGlobalObject(ctx);
    ctor = idl_step_constructor(ctx, "CSSKeywordValue", g_id_ctor);
    CHECK(!JS_IsException(ctor), "the CSSKeywordValue interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, kw_proto);
    JS_FreeValue(ctx, kw_proto);
    idl_define_global_property_reference(ctx, global, "CSSKeywordValue", ctor);
    JS_FreeValue(ctx, global);
}

void css_keyword_value_init(JSContext *ctx)
{
    JSClassDef d = { "CSSKeywordValue", kw_finalizer, kw_gc_mark };
    /* `constructor(USVString value)` — one required position, so Web IDL §3.7.1 "Interface object"'s length
       is 1. */
    static const IdlArgType CTOR_ARGS[1] = { IDL_USVSTRING };

    DCHECK(g_kw_class == 0,
           "css_keyword_value_init ran twice — the interface is declared once per AGENT, and a second class id "
           "would leave every keyword value already built branded with the first");
    JS_NewClassID(JS_GetRuntime(ctx), &g_kw_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_kw_class, &d) == 0,
          "CSSKeywordValue: the class could not be declared");

    g_id_ctor = idl_method_id(ctx, CTOR_ARGS, 1, js_css_keyword_value_ctor, 0);
    g_id_value_set = idl_setter_id(ctx, IDL_USVSTRING, false, js_css_keyword_value_set, 0);

    DCHECK(g_id_ctor >= 0 && g_id_value_set >= 0,
           "one of this component's two declarations did not enter the argument pool");
    agent_state_class("css_keyword_value", &g_kw_class,
                      "CSS Typed OM 1 §4.2's CSSKeywordValue class, and this component's declaration latch");
    agent_state_id("css_keyword_value", &g_id_ctor,
                   "CSS Typed OM 1 §4.2's CSSKeywordValue constructor declaration");
    agent_state_id("css_keyword_value", &g_id_value_set,
                   "CSS Typed OM 1 §4.2's `value` attribute setter declaration");
    /* NO realm_declare_intrinsic HERE. Web IDL §3.7.3 makes this interface one object graph with core/css/
       css_unit_value.c's three, so that component's realm install calls css_keyword_value_install_realm with
       the CSSStyleValue.prototype it has just built — the same arrangement core/css/css_math_value.c has. */
}

/* THE INVERSE OF THE DECLARATION ABOVE. The prototype and the interface object are the REALM's and go with its
   context; what is the AGENT's is the class this runtime registered and the two pool entries beside it. The
   class goes back to 0 because a class is registered in a RUNTIME — core/agent_state.h's one policy — and it
   is also this component's latch, so a carried id would make the next agent's css_keyword_value_init abort on
   a declaration that had in fact not been made. */
void css_keyword_value_free(void)
{
    DCHECK(g_kw_class != 0, "CSS Typed OM 1 §4.2's CSSKeywordValue was released in an agent that never "
                            "declared it");
    agent_state_undo("css_keyword_value");
}
