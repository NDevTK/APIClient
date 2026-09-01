/* See css_unit_value.h. CSS Typed OM Level 1 §2 CSSStyleValue objects, §4.3.1 Common Numeric Operations, and
   the CSSNumericValue Superclass, §4.3.3 Value + Unit: CSSUnitValue objects, and §6.4 CSSUnitValue
   Serialization. §4.3.5 Numeric Factory Functions is the CSS NAMESPACE's, and lives with the rest of that
   object's members in core/css/css_namespace.c; what it needs from here is one entry. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"

#include "core/agent_state.h"
#include "core/css/css_length.h"
#include "core/css/css_math.h"
#include "core/css/css_unit_value.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/concolic.h"
#include "solver/cow.h"

/* §4.3.3's TWO INTERNAL SLOTS.
 *
 * `value` IS A JSValue — see css_unit_value.h for the whole argument. In one line: the slot is a position
 * unknown external input reaches (`CSS.px(el.dataset.n * 2)`), core/idl_args.h's numeric boundary passes such
 * input through AS ITSELF so opacity survives the coercion, and a `double` field would have to collapse it.
 *
 * `unit` IS AN OWNED C STRING AND IS WRITTEN ONCE. §4.3.3 declares it `readonly`, and neither §4.3.5's
 * factories nor the constructor rewrites it after the object is built, so there is no second writer for the
 * COW delta to have to see. It is NOT a JSValue for the same reason: a slot no member can assign to needs no
 * capture, and a counted string reference would be one more thing every teardown path has to discharge. */
typedef struct CssUnitValueData {
    JSValue value;   /* §4.3.3's `value` internal slot — a Number, or unknown external input. OWNED */
    char   *unit;    /* §4.3.3's `unit` internal slot — OWNED, NUL-terminated, never rewritten */
} CssUnitValueData;

/* THE OFFSET LIST IS THE SAME LIST THE FINALIZER FREES AND THE MARK WALKS, which is what makes a field added
   to one and not the others impossible to miss. `unit` is not on it because it is not a JSValue; the record is
   memcpy'd whole either way, and restoring a pointer that no member rewrites restores the same pointer. */
static const uint16_t UV_VALS[] = { (uint16_t)offsetof(CssUnitValueData, value) };
static const CowRecord UV_REC = { sizeof(CssUnitValueData), UV_VALS, 1 };

static JSClassID g_unit_class;
static int g_id_ctor     = -1;
static int g_id_to_string = -1;
static int g_id_value_set = -1;

/* WHICH OF §4.3.3's TWO ATTRIBUTES A GETTER MEANS — one body, magic'd, rather than two that can drift. */
enum { UV_ATTR_VALUE = 0, UV_ATTR_UNIT };

/* ---- §4.3.2 Numeric Value Typing's "create a type from a string unit", as far as §4.3.3 asks it ------------ */

/* §4.3.2's two LITERAL branches — "unit is "number"" and "unit is "percent"" — compared as the section spells
   them, by code points and not case-insensitively. That is deliberate and it is the spec's own shape: the
   other seven branches are stated over unit PRODUCTIONS ("unit is a <length> unit"), and CSS matches unit
   IDENTIFIERS ASCII case-insensitively, but neither "number" nor "percent" is a unit identifier at all — no
   CSS dimension token carries them, and §4.3.5 mints them from method names it fixes itself. */
static bool uv_literal_is(const char *unit, size_t unit_len, const char *lit)
{
    size_t n = strlen(lit);

    return unit_len == n && memcmp(unit, lit, n) == 0;
}

bool css_unit_value_type_is_valid(const char *unit, size_t unit_len)
{
    CssMathBase base;

    DCHECK(unit != NULL || unit_len == 0,
           "§4.3.2's create-a-type was asked about a NULL span with a non-zero length — every caller holds the "
           "bytes it is asking about, so an absent pointer is a caller that lost the buffer");
    /* "unit is "number" → Return «[ ]»" and "unit is "percent" → Return «[ "percent" → 1 ]»". Both are types,
       so both are VALID; only §4.3.2's last branch ("anything else → Return failure") is not. */
    if (uv_literal_is(unit, unit_len, "number") || uv_literal_is(unit, unit_len, "percent")) return true;
    /* §4.3.2's six PRODUCTION branches — <length>, <angle>, <time>, <frequency>, <resolution>, <flex> — which
       are css-values-4 §10.9 Type Checking's terminal rule for a dimension over the same unit tables. See
       core/css/css_math.h for why the two questions share the table and not the entry. */
    return css_math_unit_base(unit, unit_len, &base);
}

/* ---- the record, and the COW capture every member reaches it through -------------------------------------- */

/* THE CAPTURE IS HERE AND NOT AT THE ONE WRITE, which is solver/cow.h's rule: a record a flow has REACHED
   is one it may write, the delta dedups to one entry, and there is then no write site left to miss. §4.3.3
   declares `value` writable, so unlike a wholly-readonly interface this one has a real second writer today. */
static CssUnitValueData *uv_of(JSValueConst v)
{
    CssUnitValueData *u = JS_GetOpaque(v, g_unit_class);

    if (u) cow_capture_host_record(v, u, &UV_REC);
    return u;
}

bool css_unit_value_is(JSValueConst v)
{
    return g_unit_class != 0 && JS_GetClassID(v) == g_unit_class;
}

/* WEB IDL §3.7.5's BRAND CHECK. `CSSUnitValue.prototype.value` read off `{}` is a TypeError, and a page tells
   that apart from `undefined`. */
static CssUnitValueData *uv_here(JSContext *ctx, JSValueConst v)
{
    CssUnitValueData *u = uv_of(v);

    if (!u) {
        JS_ThrowTypeError(ctx, "a CSSUnitValue member was reached on something that is not a CSSUnitValue");
        return NULL;
    }
    return u;
}

/* THE COLLECTOR'S TWO ENTRIES READ NO STATIC THIS COMPONENT'S RELEASE RESETS — core/agent_state.h's rule. Both
   run AFTER core/platform.c's release column, so a unit value a page still holds would be finalized with
   `g_unit_class` already back at 0 and `JS_GetOpaque(val, 0)` would answer NULL: the record, its counted value
   reference and its unit string would all leak, and an unmarked child keeps the internal reference gc_decref
   exists to subtract. JS_GetAnyOpaque, because the collector dispatched here THROUGH the class. */
static void uv_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    CssUnitValueData *u = JS_GetAnyOpaque(val, &id);

    (void)id;
    /* NOT `if (!u) return;`. §4.3.3's create-a-CSSUnitValue-from-a-pair is the one mint and it sets the record
       with nothing in between that allocates in the JS heap. */
    DCHECK(u != NULL, "a CSSUnitValue was finalized with no record — §4.3.3's one mint sets it with nothing in "
                      "between that could collect");
    JS_FreeValueRT(rt, u->value);
    free(u->unit);
    free(u);
}

static void uv_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID id = 0;
    CssUnitValueData *u = JS_GetAnyOpaque(val, &id);

    (void)id;
    DCHECK(u != NULL, "a CSSUnitValue was marked with no record — its `value` slot is a counted reference and "
                      "an unmarked child is read by gc_scan as rooted from outside the heap");
    JS_MarkValue(rt, u->value, mark_func);
}

JSValue css_unit_value_new(JSContext *ctx, JSValue value, const char *unit)
{
    JSValue proto, obj;
    CssUnitValueData *u;
    size_t n;

    DCHECK(g_unit_class != 0, "a CSSUnitValue was built before css_unit_value_init declared the interface");
    DCHECK(unit != NULL && *unit != '\0',
           "§4.3.3's create-a-CSSUnitValue-from-a-pair was handed no unit. Every mint names one — the "
           "constructor from its converted argument and §4.3.5's factories from the method's own name — and "
           "the empty string is not one of §4.3.2's branches, so an absent one is a caller that lost it");
    DCHECK(JS_IsNumber(value) || concolic_is(value),
           "§4.3.3's `value` internal slot was set to something that is neither a Number nor unknown external "
           "input. Web IDL §3.2's `double` conversion produces the first and passes the second through as "
           "itself, and those are the only two things that reach a mint — so anything else is a caller that "
           "skipped the declaration and is about to make `.value` answer a thing no page can compute with");
    proto = JS_GetClassProto(ctx, g_unit_class);
    DCHECK(!JS_IsNull(proto), "a CSSUnitValue was built in a realm with no CSSUnitValue.prototype");
    obj = JS_NewObjectProtoClass(ctx, proto, g_unit_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "css-typed-om: a CSSUnitValue could not be allocated");
    u = calloc(1, sizeof *u);
    CHECK(u != NULL, "css-typed-om: a CSSUnitValue's record allocation failed");
    n = strlen(unit);
    u->unit = malloc(n + 1);
    CHECK(u->unit != NULL, "css-typed-om: a CSSUnitValue's unit could not be copied");
    memcpy(u->unit, unit, n + 1);
    u->value = value;
    JS_SetOpaque(obj, u);
    return obj;
}

/* ---- §6.4 CSSUnitValue Serialization ---------------------------------------------------------------------- */

/* §6.4 step 3, which is a list headed "If unit is:" with three arms: for `"number"`, "Do nothing."; for
   `"percent"`, "Append "%" to s."; and for `anything else`, "Append unit to s." This function answers them as
   the SUFFIX, because what the arms differ in is the text appended after the digits and nothing else. */
static const char *uv_suffix(const char *unit)
{
    if (strcmp(unit, "number") == 0) return "";
    if (strcmp(unit, "percent") == 0) return "%";
    return unit;
}

JSValue css_unit_value_serialize(JSContext *ctx, JSValueConst v)
{
    CssUnitValueData *u = uv_of(v);
    JSValue example, derived;
    double n = 0.0;
    char *s;

    DCHECK(u != NULL, "§6's serialize-a-CSSStyleValue reached §6.4's arm on something that is not a "
                      "CSSUnitValue — the caller asks css_unit_value_is before it asks this");
    /* §6.4 steps 1-3 over a value this engine knows. `idl_number_of` is what a body reads a converted numeric
       slot through: for a Number it is the number, and for unknown external input it is §3.2's conversion RUN
       ON THAT VALUE'S OWN EXAMPLE — the concrete the page actually computed — with 0 meaning there is no
       example yet rather than meaning the value is zero. */
    if (idl_number_of(ctx, IDL_DOUBLE, u->value, &n)) {
        s = css_length_serialize_number(n, uv_suffix(u->unit));
        DCHECK(s != NULL, "CSSOM §6.7.2's serializer answered nothing at all — it returns an owned string for "
                          "every finite number including zero, so a null is an allocation that was not checked");
        example = JS_NewStringLen(ctx, s, strlen(s));
        free(s);
    }
    else {
        /* NO EXAMPLE YET. §6.4 has a real number for nothing, so there is nothing concrete to derive; the
           result is the unknown WITHOUT one, which is a positive statement (this string is whatever the page's
           own input makes it) and never an invented "0". */
        example = JS_UNDEFINED;
    }
    if (!concolic_is(u->value)) {
        DCHECK(!JS_IsUndefined(example),
               "§6.4 produced no serialization for a `value` slot this engine knows the number of — a Number "
               "always has an example by definition, so this is idl_number_of's two answers having come apart");
        return example;
    }
    /* THE VALUE IS UNKNOWN EXTERNAL INPUT, SO THE SERIALIZATION IS TOO. A concrete string here would DE-TAINT
       the one thing a `CSS.px(attackerNumber)` carries into whatever consumes the stringification, which is
       exactly the placeholder solver/concolic.h's builtin seam exists instead of. The derived value keeps the
       operand's source and root and carries the REAL §6.4 run on its example as its own — never a rule
       predicting what §6.4 would have produced. */
    derived = concolic_builtin_hook(ctx, u->value, "CSS Typed OM §6.4 serialize a CSSUnitValue", example);
    DCHECK(!JS_IsUninitialized(derived),
           "solver/concolic.h's builtin seam refused an operand this body had already established is unknown "
           "external input — the two tests read the same value one line apart");
    return derived;
}

/* ---- §2 CSSStyleValue's STRINGIFIER ------------------------------------------------------------------------ */

/* Web IDL §3.7.8 Stringifiers puts a `stringifier;` on the interface prototype object as `toString`, and §2
   states its behavior by reference: "The stringification behavior of CSSStyleValue objects is defined in § 6
   CSSStyleValue Serialization."
 *
 * NAMED RESIDUAL — §6 DISPATCHES OVER EIGHT SUBCLASSES AND THIS ENGINE HAS BUILT ONE.
 * WHAT IS NOT COVERED: §6.1 CSSUnparsedValue, §6.2 CSSKeywordValue, §6.3 CSSNumericValue (the abstract arm),
 * §6.5 CSSMathValue and §6.6 CSSTransformValue Serialization have no arm here, because no component mints an
 * object of any of those interfaces — so the brand test below, which asks whether `this` is a CSSUnitValue, is
 * today exactly the question "is `this` a CSSStyleValue at all".
 * WHAT THE NEXT DIFF BUILDS: the subclass it adds arrives with its own §6 arm and this body grows the branch,
 * so the two facts stop coinciding at the same moment one of them stops being true.
 * HOW ITS ABSENCE WOULD SHOW: it cannot show as a wrong answer while it holds — there is no object for the
 * missing arms to be reached on — which is why it is stated rather than asserted. It would show the day a
 * subclass is minted and its `toString` throws the TypeError below instead of serializing. */
static JSValue js_css_style_value_to_string(JSContext *ctx, JSValueConst this_val, int argc,
                                            JSValueConst *argv, int magic)
{
    (void)argc; (void)argv; (void)magic;
    if (!css_unit_value_is(this_val))
        return JS_ThrowTypeError(ctx, "CSSStyleValue.prototype.toString was reached on something that is not "
                                      "a CSSStyleValue");
    return css_unit_value_serialize(ctx, this_val);
}

/* ---- §4.3.3's constructor and its two attributes ----------------------------------------------------------- */

/* "The CSSUnitValue(value, unit) constructor must, when called, perform the following steps: If creating a type
   from unit returns failure, throw a TypeError and abort this algorithm. Return a new CSSUnitValue with its
   value internal slot set to value and its unit set to unit."
   THE UNIT'S BYTES COME THROUGH concolic_name_cstr, which is what every member that needs the TEXT of a
   possibly-unknown string argument reads it through: Web IDL's boundary passes unknown external input across
   as itself, so JS_ToCString on this position would owe C a string it does not have. An unknown denotes its
   SHAPE — a real string, stable per source — which is not one of §4.3.2's branches, so `new CSSUnitValue(1,
   location.hash)` throws the TypeError the standard gives for any unit that is not a unit. */
static JSValue js_css_unit_value_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                      int magic)
{
    const char *unit;
    JSValue r;

    (void)this_val; (void)magic;
    DCHECK(argc == 2, "§4.3.3's constructor reached its body with an argument count its IDL does not declare — "
                      "`value` and `unit` are both required and there are no other positions, so the "
                      "conversion machine owed this body exactly two arguments");
    unit = concolic_name_cstr(ctx, argv[1]);
    CHECK(unit != NULL, "css-typed-om: OOM encoding the `unit` argument of §4.3.3's constructor");
    if (!css_unit_value_type_is_valid(unit, strlen(unit))) {
        r = JS_ThrowTypeError(ctx, "'%s' is not a CSS unit that CSSUnitValue can carry", unit);
        JS_FreeCString(ctx, unit);
        return r;
    }
    r = css_unit_value_new(ctx, JS_DupValue(ctx, argv[0]), unit);
    JS_FreeCString(ctx, unit);
    return r;
}

/* "attribute double value" and "readonly attribute USVString unit" — their getter steps are to return the
   matching internal slot, which is the whole of what §4.3.3 states for either. */
static JSValue js_css_unit_value_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    CssUnitValueData *u = uv_here(ctx, this_val);

    if (!u) return JS_EXCEPTION;
    DCHECK(magic == UV_ATTR_VALUE || magic == UV_ATTR_UNIT,
           "a CSSUnitValue getter was installed with a magic naming neither of §4.3.3's two attributes");
    if (magic == UV_ATTR_VALUE) return JS_DupValue(ctx, u->value);
    return JS_NewStringLen(ctx, u->unit, strlen(u->unit));
}

/* §4.3.3 gives `value` no setter steps of its own, so Web IDL §3.7.6 Attributes' default applies: the
   converted value is stored in the slot the getter reads. The conversion is the DECLARATION's — IDL_DOUBLE is
   Web IDL's RESTRICTED `double`, so `u.value = NaN` is a TypeError before this body runs, and unknown external
   input crosses as itself exactly as it does through the constructor's first position. */
static JSValue js_css_unit_value_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    CssUnitValueData *u = uv_here(ctx, this_val);

    (void)magic;
    if (!u) return JS_EXCEPTION;
    DCHECK(JS_IsNumber(val) || concolic_is(val),
           "§4.3.3's `value` setter reached its body with something that is neither a Number nor unknown "
           "external input — IDL_DOUBLE produces the first and passes the second through, and there is no "
           "third thing for a declared numeric position to be");
    JS_FreeValue(ctx, u->value);
    u->value = JS_DupValue(ctx, val);
    return JS_UNDEFINED;
}

/* ---- the per-realm install --------------------------------------------------------------------------------- */

static void css_unit_value_install_realm(JSContext *ctx)
{
    JSValue sv_proto, nv_proto, uv_proto, ctor, global, prev;

    DCHECK(g_unit_class != 0, "a realm asked for CSSUnitValue before the interface was declared");
    prev = JS_GetClassProto(ctx, g_unit_class);
    DCHECK(JS_IsNull(prev), "css_unit_value_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    global = JS_GetGlobalObject(ctx);

    /* §2's CSSStyleValue. §3.7.3 Interface prototype object's proto step's last arm — it inherits from no
       interface — so its interface prototype object is built over this realm's %Object.prototype%, which
       JS_NewObject already gives. */
    sv_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(sv_proto), "CSSStyleValue.prototype could not be allocated");
    idl_interface_tag(ctx, sv_proto, "CSSStyleValue");
    idl_install_method(ctx, sv_proto, "toString", g_id_to_string);
    /* §3.7.1 Interface object: an interface with NO constructor still has an interface object, and calling
       it is a TypeError — which is also what tells a feature-detecting bundle the interface EXISTS. */
    ctor = idl_interface_object(ctx, "CSSStyleValue", sv_proto);
    CHECK(!JS_IsException(ctor), "the CSSStyleValue interface object could not be allocated");
    JS_SetPropertyStr(ctx, global, "CSSStyleValue", ctor);

    /* §4.3.1's CSSNumericValue. §3.7.3 Interface prototype object: "if interface is declared to inherit from
       another interface, then set proto to the interface prototype object IN REALM of that inherited
       interface" — the object one line up, which core/idl_args.c asserts by reading the §3.7.3 Interface
       prototype object class string back off it. It carries none of its own eleven members yet, and that is
       honest absence rather than a stub: a page reaching `add` gets the TypeError a browser without it gives,
       and engine/idlgen.mjs reports all eleven as the gaps they are. */
    nv_proto = JS_NewObjectProto(ctx, sv_proto);
    CHECK(!JS_IsException(nv_proto), "CSSNumericValue.prototype could not be allocated");
    idl_interface_tag(ctx, nv_proto, "CSSNumericValue");
    ctor = idl_interface_object(ctx, "CSSNumericValue", nv_proto);
    CHECK(!JS_IsException(ctor), "the CSSNumericValue interface object could not be allocated");
    JS_SetPropertyStr(ctx, global, "CSSNumericValue", ctor);
    JS_FreeValue(ctx, sv_proto);

    /* §4.3.3's CSSUnitValue, over §4.3.1's object for the same §3.7.3 Interface prototype object reason. */
    uv_proto = JS_NewObjectProto(ctx, nv_proto);
    CHECK(!JS_IsException(uv_proto), "CSSUnitValue.prototype could not be allocated");
    idl_interface_tag(ctx, uv_proto, "CSSUnitValue");
    JS_FreeValue(ctx, nv_proto);
    idl_install_accessor_no_user_code(ctx, uv_proto, "value", js_css_unit_value_get, UV_ATTR_VALUE,
                                      g_id_value_set);
    idl_install_accessor_no_user_code(ctx, uv_proto, "unit", js_css_unit_value_get, UV_ATTR_UNIT, -1);
    JS_SetClassProto(ctx, g_unit_class, JS_DupValue(ctx, uv_proto));

    /* §3.7.1 Interface object's INTERFACE OBJECT for the one interface here that DECLARES a constructor. */
    ctor = idl_step_constructor(ctx, "CSSUnitValue", g_id_ctor);
    CHECK(!JS_IsException(ctor), "the CSSUnitValue interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, uv_proto);
    JS_FreeValue(ctx, uv_proto);
    JS_SetPropertyStr(ctx, global, "CSSUnitValue", ctor);
    JS_FreeValue(ctx, global);
}

void css_unit_value_init(JSContext *ctx)
{
    JSClassDef d = { "CSSUnitValue", uv_finalizer, uv_gc_mark };
    /* `constructor(double value, USVString unit)` — two required positions, so §3.7.1 Interface object's
       length is 2. */
    static const IdlArgType CTOR_ARGS[2] = { IDL_DOUBLE, IDL_USVSTRING };

    DCHECK(g_unit_class == 0,
           "css_unit_value_init ran twice — the interface is declared once per AGENT, and a second class id "
           "would leave every unit value already built branded with the first");
    JS_NewClassID(JS_GetRuntime(ctx), &g_unit_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_unit_class, &d) == 0,
          "CSSUnitValue: the class could not be declared");

    g_id_ctor = idl_method_id(ctx, CTOR_ARGS, 2, js_css_unit_value_ctor, 0);
    /* Web IDL §3.7.8 Stringifiers' stringifier takes no arguments. */
    g_id_to_string = idl_method_id(ctx, NULL, 0, js_css_style_value_to_string, 0);
    g_id_value_set = idl_setter_id(ctx, IDL_DOUBLE, false, js_css_unit_value_set, 0);

    DCHECK(g_id_ctor >= 0 && g_id_to_string >= 0 && g_id_value_set >= 0,
           "one of this component's three declarations did not enter the argument pool");
    agent_state_class("css_unit_value", &g_unit_class,
                      "CSS Typed OM 1 §4.3.3's CSSUnitValue class, and this component's declaration latch");
    agent_state_id("css_unit_value", &g_id_ctor, "CSS Typed OM 1 §4.3.3's CSSUnitValue constructor declaration");
    agent_state_id("css_unit_value", &g_id_to_string,
                   "CSS Typed OM 1 §2's CSSStyleValue stringifier declaration");
    agent_state_id("css_unit_value", &g_id_value_set,
                   "CSS Typed OM 1 §4.3.3's `value` attribute setter declaration");
    realm_declare_intrinsic(css_unit_value_install_realm);
}

/* THE INVERSE OF THE DECLARATION ABOVE. The three prototypes and the interface objects are the REALMS' and go
   with their contexts; what is the AGENT's is the class this runtime registered and the three pool entries
   beside it. The class goes back to 0 because a class is registered in a RUNTIME — core/agent_state.h's one
   policy — and it is also this component's latch, so a carried id would make the next agent's
   `css_unit_value_init` abort on a declaration that had in fact not been made. */
void css_unit_value_free(void)
{
    DCHECK(g_unit_class != 0, "CSS Typed OM 1 §4.3.3's CSSUnitValue was released in an agent that never "
                              "declared it");
    g_id_ctor = g_id_to_string = g_id_value_set = -1;
    g_unit_class = 0;
}
