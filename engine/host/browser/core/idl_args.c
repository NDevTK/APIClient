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
#include "core/frame/secure_context.h"   /* §3.3.7 [Exposed]'s conditions: HTML §8.1.3.5 Secure contexts' answer
                                            for this realm */
#include "core/frame/window_proxy.h"     /* §3.7.6's `jsValue`: the receiver resolution and its Window brand */
#include "core/streams/readable_stream.h"
#include "core/idl_iter.h"
#include "core/file/blob.h"
#include "core/html/custom_elements.h"
#include "core/html/form_data.h"
#include "core/url/url_search_params.h"
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

/* §3.2'S INTEGER CONVERSION, ONCE, over the number ToNumber produced.
 *
 * The types differ only in WIDTH, in SIGN, and in whether a [Clamp] replaces the modulo — so this is one
 * function and the declared type is its arguments. Writing it per type is how a surface ends up with `long`
 * saturating and `unsigned short` wrapping, which is what it did.
 *
 * WITHOUT [Clamp]: a non-finite value (NaN, ±∞) and a zero are +0; otherwise sign(x)·floor(|x|) taken modulo
 * 2^width, then folded into range if the type is signed. `fmod` is exact for doubles, and above 2^53 a double is
 * already a multiple of a power of two, so the modulo is exact at 64 bits too.
 * WITH [Clamp] (§3.2.4.9): NaN is +0, the value is clamped to the type's range, and then rounded to the NEAREST
 * integer choosing the EVEN one at a half — which is `nearbyint` under the default rounding mode, and is why
 * this is not a truncation. */
static int64_t idl_int_convert(double x, int width, bool is_signed, bool clamp)
{
    double span = ldexp(1.0, width), half = ldexp(1.0, width - 1);

    if (clamp) {
        DCHECK(width == 64 && is_signed, "[Clamp] was declared on an integer type this conversion has no "
                                         "range for — state the type's bounds here, they are the spec's");
        if (isnan(x)) return 0;
        if (x <= -half) return INT64_MIN;
        if (x >= half)  return INT64_MAX;
        return (int64_t)nearbyint(x);
    }
    if (!isfinite(x) || x == 0) return 0;
    x = (x < 0 ? -1.0 : 1.0) * floor(fabs(x));
    x = fmod(x, span);
    if (x < 0) x += span;
    if (is_signed && x >= half) x -= span;
    /* A 64-BIT UNSIGNED RESULT AT OR ABOVE 2**63 IS NOT AN int64_t, and converting a double past that range to
       a signed integer is undefined behaviour rather than a wrap — it has to go through the unsigned type,
       whose conversion IS the modulo this function just performed. */
    if (!is_signed && width == 64 && x >= half) return (int64_t)(uint64_t)x;
    return (int64_t)x;
}

/* WHAT AN INTEGER TYPE IS, read off the declaration rather than remembered per call site. */
/* Web IDL 3.2.16's BRAND TEST — "does this value IMPLEMENT the declared interface", which is a question about
   its CLASS and nothing else. Every site below asked `JS_GetOpaque(v, iface)` instead, which is the class test
   AND a second, unstated condition: that the object carries a NON-NULL OPAQUE PAYLOAD. Those two coincide only
   for a component that keeps its state in one, and a component is free not to — core/html/custom_elements.c
   keeps a registry's 4.13.4 fields in an own slot under a private symbol DELIBERATELY, so that a write to them
   is an ordinary property write the per-flow COW delta captures and a definition committed in one arm of a fork
   is invisible to its sibling. It calls JS_SetOpaque nowhere, so every CustomElementRegistry in existence
   failed a brand test it is by definition the subject of.
   JS_NewObjectProtoClass stamps the class id at the mint, nothing in the language can forge one, and
   JS_GetClassID answers JS_INVALID_CLASS_ID for a non-object — which is the whole of what a brand needs. The
   supplementary narrowing a class cannot express stays where it was, in idl_iface_narrow. */
static bool idl_is_iface(JSValueConst v, JSClassID iface)
{
    DCHECK(iface != 0, "an interface brand test was made with no class to brand against");
    return JS_GetClassID(v) == iface;
}

static bool idl_is_integer(IdlArgType t)
{
    return t == IDL_LONG || t == IDL_UNSIGNED_LONG || t == IDL_UNSIGNED_SHORT ||
           t == IDL_LONG_LONG || t == IDL_UNSIGNED_LONG_LONG || t == IDL_LONG_LONG_CLAMP ||
           t == IDL_UNSIGNED_LONG_ENFORCE;
}


int64_t idl_integer_of(IdlArgType t, double x)
{
    switch (t) {
    case IDL_LONG:            return idl_int_convert(x, 32, true,  false);
    case IDL_UNSIGNED_LONG:   return idl_int_convert(x, 32, false, false);
    case IDL_UNSIGNED_SHORT:  return idl_int_convert(x, 16, false, false);
    case IDL_LONG_LONG:       return idl_int_convert(x, 64, true,  false);
    case IDL_UNSIGNED_LONG_LONG: return idl_int_convert(x, 64, false, false);
    default:
        DCHECK(t == IDL_LONG_LONG_CLAMP, "a non-integer type reached the integer conversion");
        return idl_int_convert(x, 64, true, true);
    }
}

/* Every type whose conversion IS ToNumber, integer or not. They share one request and differ only in what is
   done with the double it answers, which is what makes them one branch rather than two. */
static bool idl_is_numeric(IdlArgType t)
{
    return idl_is_integer(t) || t == IDL_UNRESTRICTED_DOUBLE || t == IDL_DOUBLE;
}

/* §3.2.4.8's `unsigned long long` AS THE MAGNITUDE IT IS. Public because a conversion that happens OUTSIDE this
   machine needs the same answer: File System §2.5's write algorithm reads `position` and `size` off a page
   dictionary with its own request and then converts what it got, and a second copy of this arithmetic is a
   second chance to hand an algorithm a negative size the spec has no step for. */
double idl_unsigned_long_long_of(double x)
{
    int64_t wrapped = idl_integer_of(IDL_UNSIGNED_LONG_LONG, x);

    return wrapped < 0 ? (double)(uint64_t)wrapped : (double)wrapped;
}

/* The value a numeric type places, given the double ToNumber produced. */
static JSValue idl_num_of(JSContext *ctx, IdlArgType t, double x)
{
    if (t == IDL_UNRESTRICTED_DOUBLE) return JS_NewFloat64(ctx, x);
    /* §3.2.7: "if V is NaN, +Infinity or -Infinity, then throw a TypeError". The two call sites below place
       what this returns and check it for the exception, so a restricted double is refused by the TYPE. */
    if (t == IDL_DOUBLE) {
        if (!isfinite(x))
            return JS_ThrowTypeError(ctx, "the provided double value is non-finite");
        return JS_NewFloat64(ctx, x);
    }
    /* §3.3.6 [EnforceRange]'s ARM of §3.2.4.9 Abstract operations' ConvertToInt, which is four of that
       algorithm's own steps and not a bound this file chose: "If x is NaN, +∞, or −∞, then throw a TypeError";
       "Set x to IntegerPart(x)" (itself "floor(abs(n))", negated when n < 0); "If x < lowerBound or
       x > upperBound, then throw a TypeError"; "Return x". The bounds are `unsigned long`'s, which is the
       only type in this build carrying the attribute — a second one states its own here rather than sharing a
       width parameter, because the whole point of the attribute is that the range is part of the TYPE. */
    if (t == IDL_UNSIGNED_LONG_ENFORCE) {
        if (!isfinite(x))
            return JS_ThrowTypeError(ctx, "the provided value is non-finite and its argument enforces a range");
        x = (x < 0 ? -1.0 : 1.0) * floor(fabs(x));
        if (x < 0 || x > 4294967295.0)
            return JS_ThrowTypeError(ctx, "the provided value is outside the range of an unsigned long");
        return JS_NewInt64(ctx, (int64_t)x);
    }
    /* AN `unsigned long long` DOES NOT FIT IN AN int64_t, and the half of its range that does not is exactly
       the half a page reaches by writing a negative: §3.2.4.8's conversion of -1 is 2**64-1, which as an int64_t
       is the bit pattern -1 again, so handing it back through JS_NewInt64 would undo the whole conversion. The
       value the member receives is a JS NUMBER either way, and a double holds the magnitude (to the same 53
       bits of precision `Number(2n**64n-1n)` has), so the unsigned type places one. */
    if (t == IDL_UNSIGNED_LONG_LONG) return JS_NewFloat64(ctx, idl_unsigned_long_long_of(x));
    return JS_NewInt64(ctx, idl_integer_of(t, x));
}

/* THE NUMBER A CONVERTED NUMERIC ARGUMENT DENOTES — see the contract in idl_args.h. */
int idl_number_of(JSContext *ctx, IdlArgType t, JSValueConst v, double *out)
{
    JSValue ex, conv;
    double x = 0;
    int r;

    DCHECK(idl_is_numeric(t),
           "idl_number_of was asked what a NON-NUMERIC IDL type denotes — the only conversion it runs is "
           "§3.2's ToNumber-and-width, so a type without one has no number for it to answer with");
    if (!concolic_is(v)) {
        DCHECK(JS_IsNumber(v),
               "a numeric IDL argument reached its body neither a Number nor unknown external input. An "
               "`undefined` here is §3.6 steps 15.4.1 and 16.1 NOT DECLARED: an optional argument whose IDL "
               "writes `= …` "
               "holds that value, and a position that never called idl_arg_default arrives ABSENT instead — "
               "declare the default beside idl_optional_from rather than reading the absence in the body");
        r = JS_ToFloat64(ctx, out, v);
        DCHECK(r >= 0, "ToNumber of an already-converted numeric argument threw — the declaration's conversion "
                       "produced this value, so nothing here can still run the page's code");
        return 1;
    }
    ex = concolic_example(ctx, v);
    if (JS_IsUndefined(ex)) {
        JS_FreeValue(ctx, ex);
        return 0;
    }
    DCHECK(!JS_IsObject(ex) && !JS_IsSymbol(ex),
           "an unknown numeric argument carries an OBJECT or a Symbol as its example, and §3.2's conversion "
           "begins with ToNumber — which on an object runs the page's own valueOf and therefore has to PARK, "
           "and on a Symbol throws. Building that means routing this conversion through the request machine "
           "in this file instead of answering it from C here");
    r = JS_ToFloat64(ctx, &x, ex);
    DCHECK(r >= 0, "ToNumber of an unknown's own example threw, and an example is a concrete primitive the "
                   "solver learned — so this is the example being a value ToNumber refuses, not the page's");
    JS_FreeValue(ctx, ex);
    conv = idl_num_of(ctx, t, x);
    DCHECK(!JS_IsException(conv),
           "§3.2's conversion of an unknown's EXAMPLE threw: `double` refuses a non-finite value (§3.2.7) and "
           "[EnforceRange] refuses one outside its range. That is one example landing on the throw arm while "
           "the unknown's DOMAIN still permits the success arm, so deciding the completion from it would "
           "decide it for every value the source can take. BOTH arms must run — ask solver_outcome over the "
           "value with two completions here, exactly as JSON.parse forks its SyntaxError arm");
    r = JS_ToFloat64(ctx, out, conv);
    DCHECK(r >= 0, "the converted example is not a number — idl_num_of places a Number for every numeric type");
    JS_FreeValue(ctx, conv);
    return 1;
}

/* §3.2.18's ENUMERATION check, over the string ToString produced. Returns -1 with a TypeError live. */
static int idl_enum_check(JSContext *ctx, JSValueConst v, const char *const *values, const char *member)
{
    const char *s = JS_ToCString(ctx, v);
    int i;

    DCHECK(values != NULL, "an IDL_ENUM member was declared with no value list — the list IS the type");
    if (!s) return -1;
    for (i = 0; values[i]; i++)
        if (!strcmp(s, values[i])) { JS_FreeCString(ctx, s); return 0; }
    JS_ThrowTypeError(ctx, "'%s' is not a valid value for the enumeration member %s", s, member);
    JS_FreeCString(ctx, s);
    return -1;
}

/* THE POOL IS CHUNKED, AND HAS NO CEILING. It was one fixed array sized "for the whole platform surface", which
   is a number nobody can know: every reflected content attribute is a declaration and HTML's per-tag interfaces
   contribute about 190 between them, so the surface grows with every component built — and the ceiling was
   reached by the six members of Headers, a component whose entire job is one header list. A ceiling on how much
   of the platform this engine may implement is a cap on the work, and it fails at INSTALL time, which is the
   worst place to find out.
   It could not simply be realloc'd: JS_RegisterStepDef BORROWS the definition, so a definition that MOVES leaves
   every registered id pointing at freed memory. So the pool is a list of BLOCKS that are allocated on demand and
   never moved — an address handed out stays valid for the life of the runtime, which is the property the borrow
   needs, and there is nothing left to run out of. */
#define IDL_POOL_CHUNK 128

/* §3.6 steps 15.4.1 and 16.1's DEFAULT VALUE AT ONE POSITION — see idl_args.h's idl_arg_default for why
   the rule is two clauses and not one. Stated in the same two fields IdlDictMember already uses for
   a dictionary member's: the KIND, and the string the IDL wrote for the one kind that carries one. It is a
   struct rather than two parallel arrays so that a member's positions cannot be described half in one and half
   in the other. */
typedef struct {
    IdlDictDefault kind;
    const char    *str;
} IdlArgDefault;

/* §3.2.26 Buffer source types STEP 1's `T` AT ONE POSITION, and the two §3.3 extended attributes steps 3 and 4
   turn on — see idl_typed_array. A struct rather than three parallel arrays for the reason IdlArgDefault is
   one: a position cannot be described half in one array and half in another. `kind` is a JSTypedArrayEnum, or
   -1 for a position that declared none — which is every position of every member that declares no
   IDL_TYPED_ARRAY, and is what idl_args_seal reads to catch a position that declared the type and not the T. */
typedef struct {
    int16_t kind;
    bool    allow_shared;
    bool    allow_resizable;
} IdlTypedArrayDecl;

/* §3.2.15 Interface types' `I` AT ONE POSITION — see idl_arg_iface. A struct rather than two parallel arrays
   for the reason IdlArgDefault is one: a position cannot be described half in one array and half in another,
   and here the halves are the TEST and the name the TypeError it throws is about. `is == NULL` is a position
   that stated none, which is every position of every member that never calls idl_arg_iface. */
typedef struct {
    bool      (*is)(JSContext *ctx, JSValueConst v);
    const char *iface;
} IdlArgIface;

typedef struct {
    IdlSetter  setter;      /* set instead of `body` for an attribute setter */
    bool       null_to_empty;
    IdlBody    body;
    /* THE DECLARED TYPES, ALLOCATED, one per position the IDL lists. It was `IdlArgType[IDL_MAX_DECLARED]`
       with a CHECK — the ceiling-as-detector this pool has now replaced three times (the member array, the
       dictionary atoms, and this) — and the members it kept out were real: `initMouseEvent` declares fifteen
       arguments and `initKeyboardEvent` ten, so both were ABSENT from the platform and both named this array
       as the reason. The list is COPIED at declaration (a caller may pass a stack array), never moved, and
       freed with the pool. */
    IdlArgType *types;
    int        nargs;      /* how many the IDL lists; a variadic tail repeats the last */
    /* THE FIRST OPTIONAL ARGUMENT's index. §3.6 resolves an `undefined` passed for an optional argument with
       no default as the argument being ABSENT — `new URL("aaa:b", undefined)` is a one-argument call, not a
       call with the base "undefined". Declared per member rather than assumed, because the same undefined at a
       REQUIRED position is the string "undefined" and collapsing the two is wrong in one direction or the
       other. Its "there are none" value is `nargs` — one past the member's own last position, so a member that
       does not declare it converts every position it lists. It was IDL_MAX_DECLARED + 1, a sentinel that could
       only exist while there was a ceiling to derive it from, and that named a position no member had. */
    int        first_optional;
    /* §3.6's LENGTH-DIFFERING OVERLOAD SPLIT, as the two numbers steps 3-4 and step 15.3 need and the
       declaration's single `first_optional` cannot carry. `split_at` is the LAST POSITION THE SHORTER ENTRY
       DECLARES, or -1 for the members that declare none — which is nearly all of them. It arrives one of two
       ways and means the same thing either way: READ OFF THE TYPE LIST where the two entries also differ in
       TYPE there (idl_type_is_length_split), or STATED by idl_overload_length_split_at where they share it and
       the type list therefore cannot say. Only the first of those makes step 15.2's type a second question —
       see the conversion loop's `step4_only_longer`.
       `split_longer_optional` is the LONGER entry's OWN first optional position, declared through
       idl_overload_split_optional_from and asserted present at the seal, because the entry that survives
       steps 3-4 is the one whose optionality step 15.3 reads and the shorter entry's is the wrong list. */
    int        split_at;
    int        split_longer_optional;
    /* §3.6 STEPS 15.4.1 AND 16.1's DEFAULT VALUES, one entry per position the IDL lists — see
       idl_arg_default. NULL for a
       member declaring none, which is nearly all of them; allocated by the first declaration that names one,
       and freed with the pool exactly as `types` is. A position whose entry is IDL_DEFAULT_NONE has no default,
       which is what §3.6's absent rule is for. */
    IdlArgDefault *arg_dflts;
    /* §3.2.26's `T` AND ITS §3.3 ATTRIBUTES, one entry per position the IDL lists — see idl_typed_array. NULL
       for a member that declares no IDL_TYPED_ARRAY position, which is nearly all of them; allocated by the
       first declaration that names one and freed with the pool exactly as `arg_dflts` is. */
    IdlTypedArrayDecl *arg_views;
    /* §3.2.15's `I` AT EACH POSITION THAT OVERRODE THE MEMBER-WIDE BRAND — see idl_arg_iface. One entry per
       position the IDL lists; NULL for a member that stated none, which is nearly all of them, allocated by
       the first idl_arg_iface and freed with the pool exactly as `arg_views` is. A position whose entry has no
       predicate falls back to `iface`/`iface_narrow` below, which is the shape and the reason
       IdlDictMember::iface's zero has. */
    IdlArgIface *arg_ifaces;
    int        magic;
    /* An IDL_DICT argument's members, and their names INTERNED at registration. The atom must be live at both
       the request and the answer — step_getprop_run is handed it twice, with a suspension in between — so it
       cannot be created per read. The names are static strings known when the member declares itself, so one
       intern per member serves every call. */
    /* THE ATOM ARRAY IS ALLOCATED, not inline. It was `JSAtom[IDL_MAX_DICT]` with a CHECK, which is the
       ceiling-as-detector this pool already replaced once: RequestInit declares eleven members and the
       platform's largest declares more, so a fixed six was a number that a real interface walks past. The
       array is malloc'd per member and freed with the pool. */
    const IdlDictMember *dict;
    JSAtom    *dict_atoms;
    int        dict_n;
    /* HOW MANY NESTED-CONVERSION FRAMES this member's declared types need — the depth of its type TREE, worked
       out once at declaration (idl_members_depth) and the reason the state's size is per-member. It is not a
       bound on the page's data: a `sequence<(DOMString or D)>` ends where D's own members stop naming another
       one, so a page nesting its arrays deeper converts no deeper. Zero for every member that declares none. */
    uint8_t    conv_depth;
    /* A member whose algorithm is itself page code runs as a STEP once the conversions are done — see
       idl_method_id_step. Its state lives immediately after this machine's, which is why the def's size is
       per-member and not a constant. */
    const IdlStepDecl *step;
    /* THE STAGE LABELS THIS MEMBER'S DEFINITION POINTS AT, owned here because they are two lists joined: the
       two stages this machine owns, and the ones the member's own algorithm declares. Built once per member at
       declaration and freed with the pool — the definition BORROWS it, exactly as JS_RegisterStepDef borrows
       the definition. NULL for a member that declares no steps, which is what "not converted" is. */
    const char **steps;
    /* HOW MANY OF THEM ARE THE MEMBER'S OWN — which is where the `[CEReactions]` epilogue's stage sits, since
       it is appended after the member's last. Kept beside the list rather than re-counted at each use: a
       NULL-terminated array counted in two places is two statements of one number. */
    int        nsteps;
    bool       variadic;    /* the last declared type applies to every argument from there on */
    /* WEB IDL §3.7.7's PROMISE RETURN TYPE — see idl_returns_promise. It is a fact about the member's
       DECLARATION and not about its body, which is why it lives here and not on IdlStepDecl. */
    bool       returns_promise;
    JSClassID  iface;       /* the brand an IDL_STRING_UNLESS_IFACE position tests against */
    /* THE NARROWING half of that brand — see idl_iface_narrow. NULL for a member whose interface a class id
       already names exactly, which is most of them. */
    bool     (*iface_narrow)(JSValueConst v);
    /* §3.2.18's VALUE LIST AT EACH POSITION WHOSE TYPE ADMITS AN ENUMERATION — see idl_arg_enum. One entry per
       position the IDL lists; NULL for a member that declares no such position, which is nearly all of them,
       allocated by the first idl_arg_enum and freed with the pool exactly as `arg_ifaces` is. It was ONE list
       per member, which could not say what Web Cryptography §14.3.9 The importKey method's IDL says — a
       KeyFormat at position 0 and a sequence<KeyUsage> at position 4 — so the second was checked in the
       member's body, which is the per-body copy of a rule this machine exists to have one of. */
    const char *const **arg_enums;
    const char *name;       /* what to call this member in a diagnostic; set when it is installed */
    /* WEB IDL §3.5 Security's THIRD INPUT for this member — "method", "getter" or "setter" — plus the one
       value §3.5 has no name for, because the member is not one of the three: a CONSTRUCTOR. It is stated at
       the MINT and not at the call, which is the whole point of it: `idl_step_function` mints an operation,
       `idl_mint_accessor` mints an attribute's getter or its setter, and `idl_step_constructor` mints
       something §3.7.7 Operations' create an operation function never runs at all. Asking the question at the
       call site instead would be a per-member `if` over a receiver, which is exactly what HTML §7.2.1's own
       failure mode is — see window_proxy.c's §7.2.1.1 Integration with IDL. */
    uint8_t    sec_kind;
    /* WEB IDL §3.7 Interfaces' implementation-check an object, its `interface` INPUT — "If object does not
       implement interface, then throw a TypeError". Stated at the DECLARATION beside the argument types,
       because it is the same kind of fact they are: what this member accepts. The predicate is the component's
       own (`idb_object_store_is`, `element_of_value`), never a class comparison — see idl_this_iface for why a
       class cannot answer "implements" for an interface anything inherits from.
       NULL is the member that has not stated one; see idl_implementation_check for what that means and what
       remains to be converted. */
    bool     (*this_is)(JSValueConst v);
    /* The interface's IDL identifier, for the TypeError §3.7 step 3 throws. Set with the predicate and never
       without it — a name with no test to go with it is a message about a check nobody makes. */
    const char *this_iface;
} IdlMember;

/* The four values of that field. IDL_SEC_NONE is FIRST so a zeroed pool entry is "not one of §3.5's three",
   which is the only value that is safe to have by accident: it asks no security check at all, and a member
   that reached its step without going through a mint is already a DCHECK at the top of the step. */
enum { IDL_SEC_NONE = 0, IDL_SEC_METHOD, IDL_SEC_GETTER, IDL_SEC_SETTER };

/* THE TWO STAGES EVERY DECLARED MEMBER PASSES THROUGH BEFORE ITS OWN ALGORITHM STARTS, and they are stages
   rather than bookkeeping because a flow RESTS at both: the second one is where a page's `toString` or a
   Proxy's get trap runs, so a member parked there is parked inside the page's code with its own algorithm not
   yet begun. Naming them here rather than in each member is the same rule as everything else in this file —
   one declaration, no per-member line to forget — and it is why IDL_STEP_FIRST is 2. */
static const char *const IDL_PROLOGUE_STEPS[] = {
    "Web IDL §3.6 (the operation's argument count: fewer than the required ones is a TypeError)",
    "Web IDL §3.6 (converting each passed argument to its declared IDL type)",
};
/* The two are one number seen from two sides — how many stages this machine owns, and where a member's own
   stages begin — so they agree at COMPILE time rather than by a comment asking them to. A member's steps would
   otherwise be joined on at the wrong base and every label would name the step next door. */
typedef char idl_prologue_matches_step_first[
    (sizeof IDL_PROLOGUE_STEPS / sizeof *IDL_PROLOGUE_STEPS) == IDL_STEP_FIRST ? 1 : -1];

/* AND THE STAGES EVERY DECLARED MEMBER PASSES THROUGH AFTER ITS OWN ALGORITHM ENDS. HTML §4.13.6 replaces
   the steps of every `[CEReactions]` operation and setter with: push an element queue, run the member's own
   steps, pop the queue, INVOKE the reactions in it, then return. Step 4 is the page's code — a custom element
   constructor or a lifecycle callback — so it is a rest point, and it is appended to every member's stage list
   for the same reason the prologue's two are prepended: this machine is the one point every declared member
   converges on, and a per-member line would be a per-member line to forget. A member whose IDL carries no
   [CEReactions] never enqueues anything during its own steps, so its queue is empty and this stage is reached
   and left in one step. */
/* THREE OF THEM, because §4.13.6 step 1.3.1 SWITCHES on the reaction's type and the three arms rest at three
   different spec steps: a callback reaction parks inside a lifecycle callback, an UPGRADE reaction parks inside
   §4.13.5 step 8.3's Construct of the page's class, and a reaction that threw parks inside HTML §8.1.4.6's
   `error` event. One label for all three would name a resume point that means three things — which is exactly
   what a stage may not do, since a cold-tier resume reports it. The order IS custom_elements.h's CE_ARM_*, and
   custom_elements.c static-asserts the same pairing for the backup queue's copy. */
static const char *const IDL_EPILOGUE_STEPS[] = {
    "HTML §4.13.6 custom element reactions steps 3-4, invoke step 1.3.1 callback reaction (invoke the "
    "reaction's callback function with \"report\"), one reaction per step",
    "HTML §4.13.6 custom element reactions steps 3-4, invoke step 1.3.1 upgrade reaction — HTML §4.13.5 step "
    "8.3 (constructing the definition's constructor with no arguments)",
    "HTML §4.13.6 custom element reactions steps 3-4, invoke step 1.3.1 (reporting the exception a reaction "
    "threw), which is HTML §8.1.4.6 report an exception",
};
#define IDL_EPILOGUE_NSTEPS ((int)(sizeof IDL_EPILOGUE_STEPS / sizeof *IDL_EPILOGUE_STEPS))

/* THE STAGES A MEMBER HAS WHEN IT DECLARES NO ALGORITHM OF ITS OWN — which is not "no stages". Every declared
 * member passes through the prologue and the epilogue whether or not anything runs between them, and a flow
 * RESTS in both (an argument's `toString`, a lifecycle callback), so those rest points need naming for exactly
 * the reason every other rest point does: a parked flow reports where it is, and a cold-tier resume resolves
 * that label back to a stage.
 *
 * This exists because the runtime now REFUSES a definition that declares neither (js_step_def_check, at
 * JS_RegisterStepDef), and the refusal caught something worth having caught. Both halves of the declaration
 * were being written AFTER the registration: a plain member never got one at all, and a step member got its
 * algorithm and its joined step list assigned to the pool entry once idl_method_id_step had returned from
 * idl_method_id_dict — so at the moment the runtime was handed the definition, EVERY member's was empty. The
 * fix belongs here rather than in the check, because a member whose only stages are the shared ones still has
 * stages, and answering the runtime with "none" was never true of any member.
 *
 * Built once, joined from the same two lists by the same loops rather than restated as a third literal — the
 * drift this file avoids everywhere else. This is the answer for a member that HAS no algorithm of its own, and
 * for no other member: a step member is built with its own joined list from the start and never wears this one
 * even briefly. It used to — the two were assigned here and replaced after the registration — which meant the
 * runtime checked this list and never saw the one the member would actually rest against. */
static const char *const *idl_plain_steps(void)
{
    static const char *joined[IDL_STEP_FIRST + IDL_EPILOGUE_NSTEPS + 1];
    int k;

    if (joined[0] != NULL) return joined;
    for (k = 0; k < IDL_STEP_FIRST; k++) joined[k] = IDL_PROLOGUE_STEPS[k];
    for (k = 0; k < IDL_EPILOGUE_NSTEPS; k++) joined[IDL_STEP_FIRST + k] = IDL_EPILOGUE_STEPS[k];
    joined[IDL_STEP_FIRST + IDL_EPILOGUE_NSTEPS] = NULL;
    return joined;
}
/* What that machine IS, for a member contributing no steps between the two shared ends. Named as the two
   standards that own those ends, because that is the whole of what runs: a plain member's own body executes
   inside one stage without resting, which is what makes it plain rather than a step machine. */
#define IDL_PLAIN_ALGORITHM \
    "Web IDL §3.6 (an operation's argument handling), inside HTML §4.13.6's custom element reactions steps"

/* The DOM layer's tree-steps edge — see idl_args.h. NULL until the DOM registers it, which is what the
   platform-less test builds and the pre-DOM boot look like. */
static const IdlTreeSteps *g_tree;

void idl_set_tree_steps(const IdlTreeSteps *ops)
{
    DCHECK(g_tree == NULL || g_tree == ops, "two components registered the tree-steps edge");
    g_tree = ops;
}

/* One block of the pool. The member and its definition live together because they are allocated together and
   indexed identically — two parallel block lists would be two chances to grow one and not the other. */
typedef struct { IdlMember m[IDL_POOL_CHUNK]; JSTrampStepDef d[IDL_POOL_CHUNK]; } IdlChunk;
static IdlChunk **g_chunks;
static int        g_nchunks;

/* Ensure the pool holds index `i`, allocating whole blocks. Called only from the declare path; every reader
   below asks for an index that path has already made. */
static void idl_pool_reserve(int i)
{
    while (i / IDL_POOL_CHUNK >= g_nchunks) {
        IdlChunk **c = realloc(g_chunks, (size_t)(g_nchunks + 1) * sizeof *c);
        CHECK(c, "idl: OOM growing the member pool — a member that cannot be declared is an API the page cannot "
                 "call");
        g_chunks = c;
        g_chunks[g_nchunks] = calloc(1, sizeof **g_chunks);
        CHECK(g_chunks[g_nchunks], "idl: OOM allocating a member-pool block");
        g_nchunks++;
    }
}
static IdlMember *idl_member(int i)
{
    DCHECK(i >= 0 && i / IDL_POOL_CHUNK < g_nchunks, "an IDL member was read at an index the pool never made");
    return &g_chunks[i / IDL_POOL_CHUNK]->m[i % IDL_POOL_CHUNK];
}
static JSTrampStepDef *idl_def(int i)
{
    DCHECK(i >= 0 && i / IDL_POOL_CHUNK < g_nchunks, "an IDL definition was read at an index the pool never made");
    return &g_chunks[i / IDL_POOL_CHUNK]->d[i % IDL_POOL_CHUNK];
}
/* STEP ID -> POOL INDEX. A member's DECLARE returns what JS_RegisterStepDef gave it, which is the RUNTIME's id
   for the definition and not this pool's index for the member — the pool index travels separately, inside the
   def as `arg`, which is why the step reads s->hdr.arg rather than its own id. Indexing g_members by the step
   id therefore lands on some other member, or off the end. It did: naming members by step id reported the
   wrong ones and then tripped the range DCHECK, which is the only reason the confusion surfaced at all.
   The two meet at exactly one place — the single JS_RegisterStepDef call below — so that is where the mapping
   is recorded, and nothing else has to know the two numbers are different. */
static int  *g_step2mem;
static int   g_step2mem_cap;
static void idl_map_step(int stepid, int idx) {
    CHECK(stepid >= 0, "JS_RegisterStepDef returned no id for an IDL member");
    if (stepid >= g_step2mem_cap) {
        int c = g_step2mem_cap ? g_step2mem_cap * 2 : 64, i;
        while (stepid >= c) c *= 2;
        g_step2mem = realloc(g_step2mem, (size_t)c * sizeof *g_step2mem);
        CHECK(g_step2mem != NULL, "idl: OOM mapping a step id to its member");
        for (i = g_step2mem_cap; i < c; i++) g_step2mem[i] = -1;
        g_step2mem_cap = c;
    }
    g_step2mem[stepid] = idx;
}
static int idl_member_of_step(int stepid) {
    return (stepid >= 0 && stepid < g_step2mem_cap) ? g_step2mem[stepid] : -1;
}
static int            g_n;
static JSRuntime     *g_rt;
static bool           g_sealed;   /* the first document's install is done — see idl_declared_before_seal */
static int            g_sealed_at; /* how many members existed then: a member minted after this is a new one */

/* Defined beside the split helpers below, and called from the seal because the seal is the ONE moment the whole
   platform's declarations exist and none of them can change again — see idl_overload_split_optional_from. */
static void idl_seal_check_splits(void);
/* The same one-moment check for idl_typed_array's other half — defined beside the conversion's own helpers. */
static void idl_seal_check_typed_arrays(void);
/* And for idl_iface_brand's / idl_arg_iface's — every §3.2.15 position of every member has an interface named,
   asked once where the whole platform is in hand rather than on whichever call first reaches the position. */
static void idl_seal_check_ifaces(void);
/* And for idl_arg_enum's — every §3.2.18 position of every member has a value list to be a member of. */
static void idl_seal_check_enums(void);
/* And for idl_this_iface's — §3.7's implementation-check takes ONE `interface`, so one interface identifier is
   answered by ONE predicate however many members name it. Asked here for the same reason as the two above: a
   declaration cannot see the other declarations of its own interface. */
static void idl_seal_check_receivers(void);
static void idl_seal_check_dictionaries(void);

void idl_args_seal(void)
{
    idl_seal_check_splits();
    idl_seal_check_typed_arrays();
    idl_seal_check_ifaces();
    idl_seal_check_enums();
    idl_seal_check_receivers();
    idl_seal_check_dictionaries();
    g_sealed = true;
    g_sealed_at = g_n;
}

/* WAS THIS MEMBER DECLARED BEFORE THE PLATFORM WAS SEALED? Asked at INSTALL, because the install is where the
   member's NAME is — and a name is the whole difference between an assert you can act on and one that only
   tells you that something, somewhere, minted twice. The rule it enforces is unchanged: a component DECLARES
   in its init and INSTALLS from the cached id, so a per-wrapper mint and a per-REALM mint are the same bug and
   both land here. */
bool idl_declared_before_seal(int stepid)
{
    /* A STEP ID AND A MEMBER INDEX ARE TWO NAMESPACES — the step id names the minted function, the member
       index names the pool entry, and idl_member_of_step is the map between them. Comparing the wrong one
       reports a member declared long before the seal as if it were fresh. */
    int idx = idl_member_of_step(stepid);
    return !g_sealed || (idx >= 0 && idx < g_sealed_at);
}

/* §3.2.17 READS A DICTIONARY'S MEMBERS IN LEXICOGRAPHIC ORDER, not in the order the IDL writes them — a page
   can see which of two getters ran first, and BlobPropertyBag declares `type` before `endings` while the spec
   reads `endings` first. The machine reads in DECLARED order, so the declaration must BE that order, and this
   is what makes that a crash rather than something each component remembers. A dictionary that INHERITS another
   reads the inherited members first and each level sorted, which a single sorted list cannot express — so the
   member states its LEVEL and the check is over both.
   ONE STATEMENT of the rule, because there are now two kinds of declaration to check it on: a member's own
   dictionary argument and a NESTED dictionary a sequence's element type names. */
static void idl_dict_order_check(const IdlDictMember *members, int n, int k)
{
    DCHECK(k >= 0 && k < n, "a dictionary's order check was asked about a member outside the table — `n` is "
                            "the caller's own loop bound and `k` its own index, so the two disagreeing means "
                            "one of them is not this table's");
    (void)n;
    if (k == 0) return;
    if (members[k].level > members[k - 1].level) return;
    if (members[k].level == members[k - 1].level && strcmp(members[k - 1].name, members[k].name) < 0) return;
#if APICLIENT_DEV
    {
        /* NAME THE PAIR. This used to abort with the RULE and nothing else, and the pool holds every dictionary
           in the engine — so a reader standing at the abort had a search rather than a diagnosis, and the two
           members that have to swap are the entire content of the answer. Building it costs a stack buffer on
           a path that is about to abort.
           AND NAME THE TABLE, WHICH THE PAIR ALONE DOES ONLY WHILE THOSE TWO NAMES ARE UNIQUE IN THE TREE.
           `multiEntry`/`unique` were, so IDBIndexParameters was one grep; `mode`, `type` and `signal` are not,
           and for a table made of those the pair is a search again. There is no NAME to print: a member's own
           dictionary argument is an ANONYMOUS array, and giving every one of them a name would be a change at
           every declaration in the engine. Its MEMBER LIST identifies it exactly and costs only this buffer.
           THE PAIR STAYS FIRST so that a message this truncates loses the list and never the diagnosis. */
        char list[320];
        size_t used = 0;
        int i;

        list[0] = '\0';
        for (i = 0; i < n; i++) {
            int w = snprintf(list + used, sizeof list - used, "%s%s", i ? ", " : "", members[i].name);

            if (w < 0 || (size_t)w >= sizeof list - used) break;
            used += (size_t)w;
        }
        /* AND IT SAYS WHERE IT STOPPED. The loop leaves on the member that would not fit, and a list that
           simply ends is read as a COMPLETE table — a shorter table than the one declared, which is a
           different defect from the one being reported. */
        if (i < n) {
            const char more[] = ", …";
            size_t at = used + sizeof more <= sizeof list ? used : sizeof list - sizeof more;

            memcpy(list + at, more, sizeof more);
        }
        DFAILF("a dictionary's members were declared out of Web IDL 3.2.17's read order: `%s` (level %d) is "
               "declared after `%s` (level %d). Inherited levels come first, and each level's own members "
               "sort lexicographically among themselves. The table declares [%s]",
               members[k].name, members[k].level, members[k - 1].name, members[k - 1].level, list);
    }
#endif
}

/* THE INTERNED MEMBER NAMES OF A NESTED DICTIONARY. The atom must be live at both halves of a keyed read —
   step_getprop_run is handed it twice, with a suspension in between — so it cannot be made per read; and a
   nested dictionary is SHARED by every member that names it (four of SanitizerConfig's nine name the same two),
   so it is interned once per DECLARATION rather than once per member. The declaration is the key, because that
   is the thing the conversion has in hand at the point it needs the atom. */
typedef struct { const IdlDictDecl *d; JSAtom *atoms; } IdlDictIntern;
static IdlDictIntern *g_dicts;
static int            g_ndicts;

static const JSAtom *idl_dict_atoms(const IdlDictDecl *d)
{
    int i;

    for (i = 0; i < g_ndicts; i++)
        if (g_dicts[i].d == d) return g_dicts[i].atoms;
    DFAIL("a nested dictionary's member names were asked for before any declaration interned them — the intern "
          "walks the whole declared type tree, so this is a dictionary the conversion reached by a route the "
          "declaration never saw");
    return NULL;
}

/* Intern one declaration's names and every dictionary reachable from it. The entry is recorded BEFORE the walk,
   so a declaration that names itself terminates instead of recursing — and the walk is over STATIC declarations
   at init, never over the page's data.
   IT IS PUBLIC AS idl_dict_declare BECAUSE AN ALGORITHM'S DICTIONARY HAS NO ARGUMENT POSITION TO BE DECLARED
   AT. §3.2.17 reached from inside an algorithm needs exactly what a nested one needs — atoms live across the
   suspension between a read's two halves, and §3.2.17's read-order check over the member list — so it goes
   through this rather than reaching for JS_NewAtom, which would be a second answer to both. */
static void idl_dict_intern(JSContext *ctx, const IdlDictDecl *d)
{
    IdlDictIntern *g;
    JSAtom *atoms;
    int i, k;

    for (i = 0; i < g_ndicts; i++)
        if (g_dicts[i].d == d) return;
    DCHECK(d->name != NULL && d->members != NULL && d->n > 0,
           "a nested dictionary was declared with no identifier or no members — the identifier is what a "
           "conversion diagnostic names, and a dictionary with no members converts nothing");
    atoms = malloc(sizeof(JSAtom) * (size_t)d->n);
    CHECK(atoms != NULL, "idl: OOM interning a nested dictionary's member names");
    g = realloc(g_dicts, (size_t)(g_ndicts + 1) * sizeof *g);
    CHECK(g != NULL, "idl: OOM recording a nested dictionary's declaration");
    g_dicts = g;
    g_dicts[g_ndicts].d = d;
    g_dicts[g_ndicts].atoms = atoms;
    g_ndicts++;
    for (k = 0; k < d->n; k++) {
        idl_dict_order_check(d->members, d->n, k);
        atoms[k] = JS_NewAtom(ctx, d->members[k].name);
        if (d->members[k].dict) idl_dict_intern(ctx, d->members[k].dict);
    }
}

const JSAtom *idl_dict_declare(JSContext *ctx, const IdlDictDecl *d)
{
    idl_dict_intern(ctx, d);
    return idl_dict_atoms(d);
}

/* HOW DEEP THE DECLARED TYPE TREE GOES — how many sequence cursors can be in flight at once for this member
   list. Each `sequence<(DOMString or D)>` is one frame plus whatever D's own members need. */
static int idl_members_depth(const IdlDictMember *ms, int n)
{
    int max = 0, k;

    for (k = 0; k < n; k++) {
        int d;

        if (ms[k].type != IDL_SEQUENCE_STRING_OR_DICT) continue;
        DCHECK(ms[k].dict != NULL,
               "a `sequence<(DOMString or D)>` member named no dictionary for the union's second arm — the "
               "dictionary is half of what that type states");
        d = 1 + idl_members_depth(ms[k].dict->members, ms[k].dict->n);
        if (d > max) max = d;
    }
    return max;
}

/* A DEFAULT VALUE, AS AN IDL VALUE. It is already converted — a default is written in the IDL, not computed
   from the page — so it is placed and never coerced.
   ONE FUNCTION, WHEREVER IT WAS DECLARED: §3.2.17 step 4.1.5 (a dictionary member) and §3.6 steps 15.4.1
   and 16.1 (an optional positional argument) place the SAME kinds of value, so a second copy would be
   a second list of which forms the platform writes. */
static JSValue idl_default_of(JSContext *ctx, IdlDictDefault kind, const char *str)
{
    if (kind == IDL_DEFAULT_NULL) return JS_NULL;
    if (kind == IDL_DEFAULT_ZERO) return JS_NewInt32(ctx, 0);
    if (kind == IDL_DEFAULT_FALSE) return JS_NewBool(ctx, 0);
    DCHECK(kind == IDL_DEFAULT_STRING && str != NULL,
           "a declaration named a default this machine has no value for — the forms are the ones the "
           "platform's IDL writes, and a new one is an arm here rather than a string that means something else");
    return JS_NewString(ctx, str);
}

static JSValue idl_default_value(JSContext *ctx, const IdlDictMember *dm)
{
    return idl_default_of(ctx, dm->dflt, dm->dflt_str);
}

/* ---- §3.2.21 OVER A UNION ELEMENT TYPE: THE NESTED CONVERSION, AS A STACK OF CURSORS -----------------------
 *
 * IdlConvFrame is idl_args.h's, beside the walk that drives it: an embedded conversion supplies its own frames,
 * so the frame's shape is part of what an embedder has to be able to size. The frames live in whichever state
 * block hosts the walk, because a deep fork BYTE-COPIES the state and re-takes only what `visit` names: a
 * malloc'd list would be one allocation two flows both free. */
enum { IDL_CONV_PULL = 0, IDL_CONV_STRING, IDL_CONV_MEMBERS };

typedef struct {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    int       i;        /* THE RESUME POINT: the argument being coerced */
    int       n;        /* how many of them there are */
    JSValue   result;   /* the body's answer (owned) */
    /* THE OTHER RESUME POINT: §3.2.17's conversion, which is idl_args.h's own walk and NOT a cursor of this
       machine's. The argument path is ONE OF ITS TWO ENTRIES and holds no dictionary logic of its own — an
       algorithm converting a dictionary it is holding drives the identical loop through the identical struct,
       so there is no argument-only copy for the two to disagree across. Its sequence cursor and union-arm byte
       are also what an ARGUMENT-position sequence uses, which is sound for the reason it was always sound and
       is now structural: Web IDL converts arguments strictly left to right, so an argument's sequence and a
       dictionary member's are never in flight at once. */
    IdlDictWalk dw;
    /* A VARIADIC member's converted arguments, which cannot live in the per-position vector the declaration
       sizes: `ul.append(...items)` has as many as the page has items, and no declaration knows that number. It
       is an ARRAY rather than a heap block because that is what `visit` can carry — a deep fork byte-copies the
       state and re-takes what visit names, so a block pointer would be SHARED by two flows that both free it,
       and a pointer into the state itself would survive the copy still aimed at the original. One owned value,
       one v->val, no new ownership contract. Non-variadic members never touch it: their arguments are exactly
       the positions their declaration lists, which is exactly what the vector holds. */
    JSValue   conv;
    JSValue   vstage;   /* the variadic argument being converted, before it joins `conv` */
    /* §3.6 STEP 12'S ARM FOR AN OVERLOAD SPLIT THAT ASKS THE SAME QUESTION — a separate byte from the one
       above, and the reason is exactly the sentence above it is NOT. `uni_phase` is shared between an argument
       position and a dictionary member's because arguments convert strictly left to right and the two are
       never in flight at once. AN OVERLOAD ARM THAT RESOLVED TO A DICTIONARY BREAKS THAT: the argument's arm
       is still in flight while that dictionary's members are being read, and the member loop RESETS
       `uni_phase` to IDL_UNI_ASK as each member finishes — so a park inside member two would resume, re-enter
       the conversion loop, find the arm "unasked" and perform `GetMethod(V, %Symbol.iterator%)` on the page's
       value a SECOND time. A Proxy `get` trap counts that read and an accessor may answer it differently, and
       the second answer could choose the other overload for a call already half-converted. */
    uint8_t    ovl_phase;
    /* §4.2.3's tree steps this member's body caused, taken from the DOM layer so the drain is per-machine: the
       drain YIELDS, and a shared list would be appended to by whichever flow ran during the suspension. */
    void     *tree;
    uint8_t   tree_after_body;   /* 1 = the body is finished; the member ends when the drain does */
    /* §4.13.6's `[CEReactions]` wrapper — the element queue this member pushed at step 1, and the drain of it
       at step 4. It is the MACHINE's state because the drain runs the page's code and therefore parks inside
       this machine; the algorithm is custom_elements.c's. Its queue is created by the FIRST enqueue and is
       therefore flow-private, which is why a member that enqueues nothing costs nothing and why the queue's
       own appends never reach the COW delta. */
    CustomElementQueue ce;
    uint8_t   ce_after_body;     /* 1 = the member's own steps are over; only the invoke remains */
    /* §4.13.6 STEP 2's CAUGHT EXCEPTION, held as a VALUE until step 5 rethrows it. The wrapper runs the
       member's own steps "catching exceptions", invokes the reactions, and only THEN rethrows — so a member
       that enqueues a reaction and throws still runs the reaction, which is what the spec says and what a page
       that catches the throw and then reads the callback's effect observes. Held as an owned value rather than
       left pending in the context, because the invoke PARKS: an exception live in the context across a
       suspension would be seen by whatever flow ran next. */
    uint8_t   ce_threw;
    JSValue   ce_exc;
    /* §4.13.4'S ACTIVE CUSTOM ELEMENT CONSTRUCTOR MAP ENTRY THIS INVOCATION ENTERED AND HAS NOT LEFT — the `C`
       of DOM §4.9 create an element step 5.1.1, held so steps 5.1.5-5.1.6 can run at the one exit common to
       every way out of step 5.1, INCLUDING a flow discarded while parked on the page's constructor. Owned;
       JS_UNDEFINED when this invocation entered nothing, which is every member but that one.
       IT IS THE MACHINE'S AND NOT THE MEMBER'S, and that is the whole point of the field rather than a
       placement detail. The map is a MAP OF CONSTRUCTORS TO REGISTRIES, so giving an entry back DROPS A
       REFERENCE to the constructor — and a member's `release` runs inside the fingerprint bracket below, whose
       comparison is over the whole heap's count of every value the declaration names. A give-back that moves
       any of those counts is indistinguishable there from a `release` that discharged the declaration itself,
       so the bracket refuses it. The agent's OTHER bracket of this same pair was never in that position:
       §4.13.5 "Upgrades" step 10's regardless-list is given back by custom_elements_queue_unlock, which runs
       BELOW the bracket. This puts DOM §4.9's in the same place, so the agent's map has ONE give-back point
       and a member's `release` is left holding only what its own contract names — a lexbor handle, a foreign
       allocation, a flag to lower, none of which is a reference. */
    JSValue   ace_ctor;
} JSIdlArgsState;

/* THE STATE BLOCK'S TAIL, WHOSE SIZE IS THE MEMBER'S AND NOT THIS MACHINE'S. Three things live after the fixed
   state, each sized by the DECLARATION rather than by a maximum: the CONVERTED ARGUMENTS (one slot per position
   the IDL lists), the nested conversion's FRAMES (one per declared sequence level), and the member's own STEP
   STATE. They live in the block rather than in a side allocation because a deep fork BYTE-COPIES the state and
   re-takes only what `visit` names: a malloc'd argument vector would be one allocation two flows both free, and
   a pointer into the state itself would survive the copy still aimed at the original.
   ONE STATEMENT OF THE LAYOUT — each offset is expressed in terms of the one before it, so a fourth section is
   added in one place and the size the definition declares cannot drift from where the accessors read.
   EACH SECTION STARTS AT ITS OWN ALIGNMENT, computed rather than assumed, and the rounding is worded in
   ALIGNMENTS and never in SIZES: sizeof(JSValue) is 16 on a 64-bit host while _Alignof(JSValue) is 8, so an
   offset worded in terms of a size states something that is not the invariant (and an assert worded that way
   can refuse to compile on a host where the two differ). A JSValue's alignment covers every other thing in this
   tail — its union holds a double and a pointer — so it is the one number here; the member's own step state is
   a struct this file has never seen, and it can need no more than that, because the whole block comes from ONE
   js_mallocz and nothing in it can be aligned better than the block itself. */
#define IDL_ALIGN_UP(n, a) (((size_t)(n) + (size_t)(a) - 1) & ~((size_t)(a) - 1))

static size_t idl_off_args(void)
{
    return IDL_ALIGN_UP(sizeof(JSIdlArgsState), _Alignof(JSValue));
}
static size_t idl_off_frames(int nargs)
{
    return IDL_ALIGN_UP(idl_off_args() + (size_t)nargs * sizeof(JSValue), _Alignof(IdlConvFrame));
}
static size_t idl_off_body(int nargs, int conv_depth)
{
    return IDL_ALIGN_UP(idl_off_frames(nargs) + (size_t)conv_depth * sizeof(IdlConvFrame),
                        _Alignof(JSValue));
}
/* IDL_ALIGN_UP's mask needs each alignment to be a POWER OF TWO, which every alignment on every C
   implementation is — asserted rather than assumed because the mask does not fail loudly if it is not, it
   quietly rounds to the wrong address. */
typedef char idl_tail_alignments_are_powers_of_two[
    ((_Alignof(JSValue)      & (_Alignof(JSValue) - 1))      == 0 &&
     (_Alignof(IdlConvFrame) & (_Alignof(IdlConvFrame) - 1)) == 0 &&
     _Alignof(IdlConvFrame) <= _Alignof(JSValue)) ? 1 : -1];

/* THE CONVERTED ARGUMENTS. The vector holds exactly the positions the member declares, so the body's argv IS
   this pointer for every member that is not variadic. */
static JSValue *idl_args_vec(void *st)
{
    return (JSValue *)((char *)st + idl_off_args());
}

/* ONE ARGUMENT'S SLOT, with the declaration's own bound asserted at every reach — an index past `nargs` is a
   member converting a position its IDL never listed, which is the state the vector's size makes impossible and
   this is what says so at the origin. */
static JSValue *idl_arg_slot(const IdlMember *m, void *st, int i)
{
    DCHECK(i >= 0 && i < m->nargs,
           "an IDL member reached a converted-argument slot past the positions its declaration lists — the "
           "vector is sized from `nargs`, so this index has nothing behind it");
    return &idl_args_vec(st)[i];
}

static IdlConvFrame *idl_frames(const IdlMember *m, void *st)
{
    return (IdlConvFrame *)((char *)st + idl_off_frames(m->nargs));
}

static void *idl_body_state(const IdlMember *m, void *st)
{
    return (char *)st + idl_off_body(m->nargs, m->conv_depth);
}

/* THE SAME OFFSET FOR A READ-ONLY CALLER — the fork asks a member whether it may be cloned and hands it a state
   it must not write. Written beside its twin, and derived from the same idl_off_body, so the two cannot name
   different places; a caller that needs to WRITE the body uses the one above. */
static const void *idl_body_state_const(const IdlMember *m, const void *st)
{
    return (const char *)st + idl_off_body(m->nargs, m->conv_depth);
}

/* HOW MANY POSITIONS THE IDL LISTS AS ORDINARY ARGUMENTS. A VARIADIC member's last declared type is the type of
   "every argument from here on" and not a position of its own, which is why the count differs — and why §3.6's
   absent-optional rule stops there: each value a page passes to a `T...` tail is CONVERTED, so
   `el.append('a', undefined)` appends the text "undefined" rather than skipping an argument. */
/* IS THIS DECLARED TYPE A DICTIONARY — the plain one and the three unions whose OMITTED arm is one.
 *
 * IT IS ASKED BECAUSE AN OMITTED DICTIONARY ARGUMENT IS NOT AN ABSENT ONE. §3.6's rule that an optional
 * argument given `undefined` is absent is about a value type, where the body has to tell "no base" from the
 * base `undefined` — but Web IDL writes `optional D options = {}`, and §3.2.17 converting `undefined` to D
 * yields a dictionary carrying every member's DEFAULT. So the body of such a member sees a dictionary whether
 * or not the page passed one, and there is no state in which it sees `undefined`.
 *
 * THIS FILE ALREADY PERFORMED THAT CONVERSION AND THE LOOP NEVER REACHED IT. `s->n` was min(argc, nargs), so a
 * call that omitted the trailing dictionary never visited its position at all, and every such body had to
 * hand-roll `argc > N ? argv[N] : JS_UNDEFINED` — which is the consumer-side default §Offensive-programming
 * names, one call site at a time. It read as correct wherever the IDL default happened to equal what the
 * hand-rolled `undefined` produced (`idl_dict_bool` answers false, and `= false` is the common default), and
 * it was WRONG wherever it did not: HTML §7.2.2's `WindowPostMessageOptions` gives `targetOrigin` the default
 * "/", so `window.postMessage(msg)` read an undefined target origin where the spec reads "/". */
static bool idl_type_is_dictionary(IdlArgType t)
{
    return t == IDL_DICT || t == IDL_DICT_OR_BOOL_FIRST || t == IDL_BOOL_OR_DICT ||
           t == IDL_STRING_OR_DICT ||
           t == IDL_USVSTRING_OR_DICT || t == IDL_UNRESTRICTED_DOUBLE_OR_DICT ||
           t == IDL_SEQUENCE_OBJECT_OR_DICT;
}

/* §3.6's LENGTH-DIFFERING SPLIT — a declared type whose two overload entries END at different positions, so
   steps 3-4 can remove one of them from the ARGUMENT COUNT alone, before any value is looked at.
   IDL_SEQUENCE_OBJECT_OR_DICT is deliberately NOT one: its two entries are the same length, nothing is removed
   early, and its arm is step 12's test of the value. The distinction is what decides both halves below — which
   position gets its type rewritten, and whose optionality list step 15.3 then reads. */
static bool idl_type_is_length_split(IdlArgType t)
{
    return t == IDL_USVSTRING_OR_DICT || t == IDL_UNRESTRICTED_DOUBLE_OR_DICT;
}

/* THE TYPE THE LONGER ENTRY DECLARES AT THAT POSITION — what step 4 leaves standing once the shorter entry is
   gone. It is a total function over the rows above and crashes for anything else, so a split row added without
   a longer-entry type names itself here rather than falling through to the shorter entry's dictionary. */
static IdlArgType idl_split_longer_type(IdlArgType t)
{
    switch (t) {
    case IDL_USVSTRING_OR_DICT:           return IDL_USVSTRING;
    case IDL_UNRESTRICTED_DOUBLE_OR_DICT: return IDL_UNRESTRICTED_DOUBLE;
    default: break;
    }
    DFAIL("§3.6 steps 3-4 removed the shorter overload entry at a position whose declared type names no LONGER "
          "entry type — a length-differing split states BOTH of its arms, so a row idl_type_is_length_split "
          "accepts and this does not is half a declaration");
    return t;
}

/* §3.6 STEPS 3-4's OUTCOME for a member declaring a length-differing split: did the LONGER entry survive.
   argcount is min(maxarg, args) and the shorter entry's type list ENDS at the split position, so a call
   passing anything BEYOND that position has removed it outright. Asked of `argc` rather than of the position
   being converted, because it is a fact about the CALL and every position of the call reads the same answer —
   which is exactly what the version of this that lived inline at one position could not express. */
static bool idl_split_longer_survived(const IdlMember *m, int argc)
{
    return m->split_at >= 0 && argc > m->split_at + 1;
}

/* §3.6 STEP 15.3's OPTIONALITY, READ OFF THE ENTRY THAT SURVIVED — "let optionality be the value at index i in
   the list of optionality values of the REMAINING entry". For every member with no length-differing split
   there is one entry and the declaration's own number IS that list; for a member that declares one, the
   surviving entry decides — however that split was stated, since the removal at steps 3-4 is by argument count
   in both forms. idl_args_seal asserts that such a member declared the longer entry's number, so there is
   nothing to fall back to here. */
static int idl_first_optional(const IdlMember *m, int argc)
{
    if (!idl_split_longer_survived(m, argc)) return m->first_optional;
    DCHECK(m->split_longer_optional >= 0,
           "a §3.6 length-differing overload split reached a conversion without its LONGER entry's optional "
           "index — idl_args_seal asserts every such member declares one, so this member was declared after "
           "the platform was sealed");
    return m->split_longer_optional;
}

/* THE TWO-SIDED HALF OF THAT DECLARATION, run once when the platform is sealed. A member that declares a
   length-differing split — by its type list, or by idl_overload_length_split_at where the two entries share
   the type there — and that never said where the LONGER entry's optional arguments begin is a member
   §3.6 step 15.3 cannot be run for — so it is caught HERE, with every declaration in hand and before any page
   can call one, rather than at whichever arity happens to reach the missing number first. The reverse is
   asserted too: a number declared by a member with no split is a declaration describing an entry that does not
   exist, which is how such a call lands on the wrong member (see IDL_LAST_DECL_ONLY). */
static void idl_seal_check_splits(void)
{
    int i, k;

    for (i = 0; i < g_n; i++) {
        const IdlMember *m = idl_member(i);

        DCHECK(m->split_at < 0 || m->split_longer_optional >= 0,
               "a member declared a §3.6 overload split whose two entries differ in LENGTH and never said where "
               "the LONGER entry's own optional arguments begin — step 15.3 reads optionality off the entry "
               "that survived steps 3-4, so without it every position past the split is measured against the "
               "SHORTER entry's declaration. State it with idl_overload_split_optional_from");
        DCHECK(m->split_at >= 0 || m->split_longer_optional < 0,
               "a member with no length-differing §3.6 overload split declared where its LONGER entry's "
               "optional arguments begin — there is no such entry, so the declaration describes a member that "
               "is not this one");
        /* THE TWO FACTS §3.7.7 Operations' LENGTH DERIVATION RESTS ON — see idl_member_length_of. That number
           is min(first_optional, declared positions) read at argument count 0, and for a split member it must
           be the minimum over BOTH entries' tuples in Web IDL §2.5.8 Overloading's effective overload set, not
           just the shorter one's. It is, because of exactly these two:
             - `first_optional` belongs to the SHORTER entry, whose type list ends at `split_at`, so its "there
               are none" value is split_at + 1 and it can never name a position past its own end. Declared
               larger — which is what a split member that simply forgot idl_optional_from would leave, since
               the default is `nargs` — the derivation would read the LONGER entry's arity and hand a page a
               length its own shorter overload contradicts.
             - a split member is NOT variadic, so `declared positions` is `nargs`, which is at least split_at+1.
               With split_longer_optional > split_at asserted above, the longer entry's own shortest tuple is
               then at least the shorter entry's, and the minimum over S is the one this derivation reads. */
        DCHECK(m->split_at < 0 || m->first_optional <= m->split_at + 1,
               "a §3.6 length-differing overload split's SHORTER entry declared its first optional argument "
               "past its own type list, which ends at the split — so §3.7.7 Operations' length would be "
               "measured against the LONGER entry's arity. State the shorter entry's own optional index with "
               "idl_optional_from");
        DCHECK(m->split_at < 0 || !m->variadic,
               "a member declared BOTH a §3.6 length-differing overload split and a variadic tail — §2.5.8 "
               "Overloading expands a variadic entry and steps 3-4 remove a shorter one, and this pool models "
               "only one of the two at a time, so its §3.7.7 length and its arity check would both be read off "
               "an entry set that was never computed");
        for (k = 0; k < m->nargs; k++)
            DCHECK(!idl_type_is_length_split(m->types[k]) || m->split_at == k,
                   "a member's length-differing §3.6 split was not recorded at the position its type list "
                   "declares it — the position is READ from the types at declaration, so the two disagreeing "
                   "means a second split was declared and one of them decides every arity");
    }
}

/* THE TWELVE NAMES §3.2.26's typed-array algorithm ranges over — "Int8Array, Int16Array, Int32Array, Uint8Array,
   Uint16Array, Uint32Array, Uint8ClampedArray, BigInt64Array, BigUint64Array, Float16Array, Float32Array, or
   Float64Array" — in JSTypedArrayEnum's OWN order, which is not the order the standard lists them in. The
   enumerator is what a declaration passes, so the table is indexed by it and the two cannot drift; a name here
   is T's NAME in step 2's sense, so it is also the word a refusal has to say. */
static const char *const IDL_TYPED_ARRAY_NAMES[] = {
    "Uint8ClampedArray", "Int8Array", "Uint8Array", "Int16Array", "Uint16Array", "Int32Array",
    "Uint32Array", "BigInt64Array", "BigUint64Array", "Float16Array", "Float32Array", "Float64Array",
};
/* The enum's own last member sizes it, so a typed array added upstream fails to COMPILE here rather than
   reading past the table at whichever conversion first names it. */
typedef char idl_typed_array_names_cover_the_enum[
    (sizeof IDL_TYPED_ARRAY_NAMES / sizeof *IDL_TYPED_ARRAY_NAMES) == JS_TYPED_ARRAY_FLOAT64 + 1 ? 1 : -1];

static const char *idl_typed_array_name(int kind)
{
    DCHECK(kind >= 0 && kind <= JS_TYPED_ARRAY_FLOAT64,
           "a typed array kind outside JSTypedArrayEnum reached §3.2.26 Buffer source types' conversion — the "
           "declaration is what carries it and it asserts the same range");
    return IDL_TYPED_ARRAY_NAMES[kind];
}

/* THE TWO-SIDED HALF OF idl_typed_array's DECLARATION, run once when the platform is sealed — the same shape
   as the split check above and for the same reason. §3.2.26 step 1 is "let T be the IDL type V is being
   converted to", so a position declared IDL_TYPED_ARRAY that never said WHICH is a conversion with no brand
   test at all, and it is caught HERE — with every declaration in hand and before any page can call one —
   rather than at whichever call happens to reach that position first. The reverse is asserted too: a T
   declared at a position whose type is something else describes a member that is not this one, which is
   exactly how a setter stated from an INSTALL lands on whichever component declared last (IDL_LAST_DECL_ONLY). */
/* A DICTIONARY POSITION AND ITS MEMBER LIST ARE ONE DECLARATION, asserted over the whole platform rather than
   at the position that happens to be converted first. §3.2.17's conversion is a walk of the member list, so a
   position declared IDL_DICT with no list is a conversion with nothing to read: before the walk existed it
   quietly produced an EMPTY object, which is the silent shape — a body would then read every member as absent
   and the IDL's own defaults would never be placed. The walk refuses it at its start instead, and this is what
   makes that refusal unreachable rather than a crash waiting for a page to call the member.
   THE CONVERSE IS THE OTHER HALF: a member list declared at a member with no dictionary position is a
   declaration describing a member that is not this one, exactly as the typed-array pair above states. */
static void idl_seal_check_dictionaries(void)
{
    int i, k;

    for (i = 0; i < g_n; i++) {
        const IdlMember *m = idl_member(i);
        bool has_position = false;

        for (k = 0; k < m->nargs; k++)
            if (idl_type_is_dictionary(m->types[k])) has_position = true;
        DCHECK(!has_position || m->dict_n > 0,
               "a member declared a dictionary argument position and no dictionary members — §3.2.17's "
               "conversion IS the walk of that list, so this position has nothing to convert and would hand "
               "the body an object on which every member the IDL declares is absent");
        DCHECK(has_position || m->dict_n == 0,
               "a member declared dictionary members at no dictionary position — nothing reads the list, so "
               "the declaration describes a member that is not this one");
    }
}

static void idl_seal_check_typed_arrays(void)
{
    int i, k;

    for (i = 0; i < g_n; i++) {
        const IdlMember *m = idl_member(i);

        for (k = 0; k < m->nargs; k++) {
            bool stated = m->arg_views != NULL && m->arg_views[k].kind >= 0;

            DCHECK(m->types[k] != IDL_TYPED_ARRAY || stated,
                   "a member declared an IDL_TYPED_ARRAY position and never said WHICH typed array it is — "
                   "§3.2.26 Buffer source types step 2 tests [[TypedArrayName]] against T's own name, so "
                   "without T the conversion has no brand test to run. State it with idl_typed_array");
            DCHECK(m->types[k] == IDL_TYPED_ARRAY || !stated,
                   "a member stated §3.2.26 step 1's T at a position whose declared type is not "
                   "IDL_TYPED_ARRAY — no other conversion reads it, so the declaration describes a position "
                   "that never asks for it");
        }
    }
}

/* ---- Web IDL §3.2.15 Interface types' `I`, RESOLVED AT ONE DECLARED POSITION ------------------------------
 *
 * Three readers below, all in this file and all inside one loop of one function, so the DCHECKs these carry
 * name a line a reader can act on by reading three call sites — which is the test
 * §AN-ASSERT-THAT-NAMES-A-REMEDY sets, and a FOURTH caller is the point at which the position has to travel
 * with the check rather than being derived here. `ti` is the DECLARED position the argument takes its
 * statements from, derived once by the conversion (a variadic tail repeats the last declared position), never
 * the argument's own index.
 */

/* HAS THIS POSITION AN INTERFACE TO TEST AGAINST — the question every arm below asserts before it tests, and
   the one the seal asks of the whole platform at once. Either form is an answer; neither is a defect. */
static bool idl_arg_iface_stated(const IdlMember *m, int ti)
{
    DCHECK(ti >= 0 && ti < m->nargs,
           "§3.2.15's brand was asked about a position outside the member's own declared type list");
    return (m->arg_ifaces != NULL && m->arg_ifaces[ti].is != NULL) || m->iface != 0;
}

/* §3.2.15's "If V implements I". The POSITION's own predicate is the whole test where one was stated, and the
   declaration-wide class-plus-narrowing pair otherwise — the same override IdlDictMember::iface states for a
   dictionary's members, asked here for an argument list's positions. */
static bool idl_arg_implements(JSContext *ctx, const IdlMember *m, int ti, JSValueConst v)
{
    DCHECK(idl_arg_iface_stated(m, ti), "§3.2.15's brand test ran at a position that declared no interface");
    if (m->arg_ifaces != NULL && m->arg_ifaces[ti].is != NULL)
        return m->arg_ifaces[ti].is(ctx, v);
    return idl_is_iface(v, m->iface) && (m->iface_narrow == NULL || m->iface_narrow(v));
}

/* WHAT THE TypeError IS ABOUT. A per-position declaration names its interface in the IDL's own identifier;
   idl_iface_brand names a CLASS and has no identifier to give, and that absence is the positive statement
   "this declaration says which class, not which interface" rather than a field nobody wrote — so the phrase
   the message falls back to is the one that was there before any position could name itself. */
static const char *idl_arg_iface_subject(const IdlMember *m, int ti)
{
    DCHECK(idl_arg_iface_stated(m, ti), "the subject of §3.2.15's TypeError was asked for at a position that "
                                        "declared no interface");
    return (m->arg_ifaces != NULL && m->arg_ifaces[ti].is != NULL) ? m->arg_ifaces[ti].iface
                                                                   : "the declared interface";
}

/* EVERY BRANDING POSITION HAS A BRAND, over the whole platform at once — idl_seal_check_typed_arrays' sweep
   asked of the other half of a declared type. It is the SEAL rather than the conversion because a member whose
   brand was never stated is broken from the moment it is declared, and the conversion only finds out on the
   first call that reaches that position: a member nothing on this page happens to call would carry the defect
   to whichever page does. The set of types that ask is idl_type_brands_interface's and no copy of it. */
static void idl_seal_check_ifaces(void)
{
    int i, k;

    for (i = 0; i < g_n; i++) {
        const IdlMember *m = idl_member(i);

        for (k = 0; k < m->nargs; k++) {
            bool stated = m->arg_ifaces != NULL && m->arg_ifaces[k].is != NULL;

            DCHECKF(!idl_type_brands_interface(m->types[k]) ||
                    stated || m->iface != 0,
                    "member `%s` declares a Web IDL §3.2.15 interface type at position %d and names no "
                    "interface for it — §3.2.15 is \"If V implements I, then return … Throw a TypeError\", and "
                    "with no I there is nothing to test, so every value would reach the body. State it with "
                    "idl_iface_brand (the class, for an interface one class names exactly) or with "
                    "idl_arg_iface (the predicate and the identifier, for a position whose interface a class "
                    "cannot express or whose member declares more than one)",
                    m->name ? m->name : "(not installed)", k);
            DCHECKF(idl_type_brands_interface(m->types[k]) || !stated,
                    "member `%s` stated §3.2.15's interface at position %d, whose declared type asks for no "
                    "brand — no conversion reads it, so the declaration describes a position that never tests "
                    "against it",
                    m->name ? m->name : "(not installed)", k);
        }
    }
}

/* ---- Web IDL §3.2.18 Enumeration types' `E`, RESOLVED AT ONE DECLARED POSITION ----------------------------
 *
 * The same shape as §3.2.15's `I` directly above, one axis over, and `ti` means the same thing: the DECLARED
 * position the argument takes its statements from, derived once by the conversion (a variadic tail repeats the
 * last declared position), never the argument's own index.
 */
static const char *const *idl_arg_enum_values(const IdlMember *m, int ti)
{
    DCHECK(ti >= 0 && ti < m->nargs,
           "§3.2.18's value list was asked about a position outside the member's own declared type list");
    DCHECK(m->arg_enums != NULL && m->arg_enums[ti] != NULL,
           "§3.2.18 step 2 is a membership test and this position declared nothing to be a member OF — "
           "idl_args_seal asserts every position whose type admits an enumeration states its values, so this "
           "member was declared after the platform was sealed");
    return m->arg_enums[ti];
}

/* EVERY POSITION WHOSE TYPE ADMITS AN ENUMERATION HAS ONE, over the whole platform at once —
   idl_seal_check_ifaces' sweep asked of the other half of a declared type, and it is the SEAL rather than the
   conversion for that function's own reason: a member whose value list was never stated is broken from the
   moment it is declared, and the conversion only finds out on the first call that reaches the position, so a
   member nothing on this page happens to call would carry the defect to whichever page does. The set of types
   that ask is idl_type_admits_enumeration's and no copy of it. */
static void idl_seal_check_enums(void)
{
    int i, k;

    for (i = 0; i < g_n; i++) {
        const IdlMember *m = idl_member(i);

        for (k = 0; k < m->nargs; k++) {
            bool stated = m->arg_enums != NULL && m->arg_enums[k] != NULL;

            DCHECKF(!idl_type_admits_enumeration(m->types[k]) || stated,
                    "member `%s` declares a Web IDL §3.2.18 enumeration at position %d and names no values for "
                    "it — §3.2.18 step 2 is \"If S is not one of E's enumeration values, then throw a "
                    "TypeError\", and with no E there is nothing to be one of, so every string would reach the "
                    "body. State them with idl_arg_enum",
                    m->name ? m->name : "(not installed)", k);
            DCHECKF(idl_type_admits_enumeration(m->types[k]) || !stated,
                    "member `%s` stated §3.2.18's values at position %d, whose declared type asks for no "
                    "enumeration — no conversion reads them, so the declaration describes a position that "
                    "never tests against them",
                    m->name ? m->name : "(not installed)", k);
        }
    }
}

/* ---- Web IDL §3.7 Interfaces' implementation-check an object, its `interface` INPUT, ACROSS EVERY MEMBER ---
 *
 * ONE INTERFACE HAS ONE IMPLEMENTATION TEST. §3.7's implementation-check takes a single `interface`, and
 * idl_this_iface pairs a PREDICATE with an IDENTIFIER by hand at every member that states one — so two members
 * of one interface can name two different tests, and the disagreement then decides which receivers each of
 * them accepts with nothing to say so. A member branded against a NEIGHBOURING component's predicate is the
 * same defect and reads as a copy-paste that compiled: it is the only kind of receiver bug that makes a member
 * refuse a receiver a browser accepts, which is a page-visible TypeError on a correct call rather than a
 * missing refusal. It is asked at the SEAL for idl_seal_check_ifaces' own reason — a declaration cannot see
 * the other declarations of its own interface, and the conversion would only find out on the first call that
 * reaches a member, so an interface nothing on this page happens to call would carry the disagreement to
 * whichever page does.
 *
 * THE OTHER DIRECTION IS NOT ASSERTED AND MUST NOT BE. One predicate legitimately answers for two identifiers
 * wherever this engine has a single class for an interface and for something that inherits it — a member of
 * `StyleSheet` reached on the only stylesheet objects that exist is answered by the very test a member of
 * `CSSStyleSheet` uses — so requiring the pairing to be injective would refuse a correct declaration.
 *
 * IT IS DEV-ONLY BODY AND NOT ONLY DEV-ONLY ASSERTS, WHICH IS A DELIBERATE DIFFERENCE FROM THE TWO CHECKS
 * ABOVE. The comparison below DEREFERENCES `this_iface`, and what guarantees it is non-NULL beside a non-NULL
 * `this_is` is a DCHECK in idl_this_iface — which is compiled out in exactly the build a live `strcmp` would
 * still run in. Leaving the loop live would trade a dev abort for a release segfault, so the read lives where
 * the statement that makes it safe lives.
 *
 * NAMED RESIDUAL — COVERAGE IS THE HALF THIS CANNOT ASK, AND IT IS THE HALF THAT MATTERS MOST WHILE THE
 * PLATFORM IS BEING CONVERTED. WHAT IS NOT COVERED: whether a member of an interface whose OTHER members
 * declare a receiver declares one itself. The pool holds no member→interface link for a member that declared
 * nothing, so the next member added to a converted component is silently unchecked — and it is WORSE than an
 * unconverted member, because converting a component DELETES the body's own brand test, so the member that
 * forgets has no test at all rather than a late one. WHAT THE NEXT DIFF BUILDS: the receiver predicate
 * recorded against the INTERFACE PROTOTYPE OBJECT where the component already names it — idl_interface_tag,
 * which §3.7.3 makes every interface prototype call — and read at idl_install_method, the one point every
 * installed operation converges on, so an unbranded member installed on a converted prototype aborts naming
 * itself. HOW ITS ABSENCE WOULD SHOW: a member added to a component that has converted takes EVERY receiver,
 * and `Iface.prototype.<new>.call({})` reaches the body where its converted neighbours throw.
 * AND THAT DIFF OWES A MEASUREMENT RATHER THAN AN ASSUMPTION, because the prototype is not always there and a
 * check that skips a target in silence is the zero this file spends its length warning about: an install
 * target that carries no §3.7.3 tag — a [Global] object, a namespace object, the instance an unforgeable
 * operation is installed on — has no interface for that read to find, and a handful of interface prototypes
 * are tagged AFTER their first install rather than before it, which is an ordering to fix at those components
 * rather than a case to skip.
 */
static void idl_seal_check_receivers(void)
{
#if APICLIENT_DEV
    int i, k;

    for (i = 0; i < g_n; i++) {
        const IdlMember *m = idl_member(i);

        if (m->this_is == NULL) continue;
        DCHECK(m->this_iface != NULL,
               "a member carries §3.7's implementation test with no interface identifier beside it — "
               "idl_this_iface sets the two together, so a member holding one of them was written to by "
               "something that is not that declaration");
        for (k = 0; k < i; k++) {
            const IdlMember *o = idl_member(k);

            if (o->this_is == NULL || strcmp(o->this_iface, m->this_iface) != 0) continue;
            DCHECKF(o->this_is == m->this_is,
                    "members `%s` and `%s` both declare interface %s as their receiver and name two different "
                    "implementation tests for it — Web IDL §3.7 Interfaces' implementation-check an object "
                    "takes ONE `interface`, so one of these two members refuses receivers the other accepts "
                    "and a page calling both on the same object is told the object implements the interface "
                    "and does not. State the interface's own predicate at both",
                    o->name ? o->name : "(not installed)", m->name ? m->name : "(not installed)",
                    m->this_iface);
        }
    }
#endif
}

static int idl_declared_positions(const IdlMember *m)
{
    DCHECK(!m->variadic || m->nargs >= 1,
           "a variadic member declared no types at all — the tail's type is what `T...` means, so there is "
           "always at least one");
    return m->variadic ? m->nargs - 1 : m->nargs;
}

/* WEB IDL §3.7.7 Operations' `length`, DERIVED FROM THE DECLARATION AND NEVER PASSED IN.
 *
 * §3.7.7's create-an-operation-function ends verbatim: "Compute the effective overload set for regular
 * operations … with identifier id on target and with argument count 0, and let S be the result. Let length be
 * the length of the shortest argument list in the entries in S. Let F be CreateBuiltinFunction(steps, length,
 * id, « », realm)." §3.7.1 Interface object and §3.7.2 Legacy factory functions say the SAME sentence for a
 * constructor and for a legacy factory function, over their own effective overload sets, so one derivation
 * serves all three shapes this pool mints.
 *
 * IT IS THE NUMBER §3.6 STEP 5's ARITY CHECK ALREADY COMPUTES, and that is not a coincidence — both are "how
 * many arguments does the entry that survived require", asked at argument count 0. Web IDL §2.5.8 Overloading's
 * compute-the-effective-overload-set is what makes them the same: with N = 0, step 5.4's `max` is the member's
 * own maxarg, so step 5.7's variadic expansion (i runs n to max − 1) appends NOTHING and only step 5.9's
 * trailing loop can shorten a tuple. That loop breaks when "arguments[i] is not optional (i.e., it is not
 * marked as optional and is not a final, variadic argument)", so it walks back over exactly the positions this
 * declaration calls optional PLUS a variadic tail — which is `idl_first_optional` and `idl_declared_positions`
 * respectively, and the shortest tuple in S is therefore the smaller of the two.
 *
 * THE SPLIT MEMBERS ARE THE CASE WORTH CHECKING, AND THEY COME OUT RIGHT BY THE ALGORITHM RATHER THAN BY LUCK.
 * `idl_split_longer_survived` is `argc > split_at + 1`, which is FALSE at argc 0 — so the SHORTER entry's
 * optionality list is the one read here, and §3.7.7's "shortest argument list in the entries in S" wants
 * exactly that: S holds both entries' tuples, the shorter entry's type list ends at `split_at`, and
 * idl_seal_check_splits asserts `first_optional <= split_at + 1` and that a split member is not variadic — so
 * the shorter entry's own shortest tuple is at most `split_at + 1`, the longer entry's is at least that (its
 * own first optional is declared PAST the split), and the minimum over S is the shorter entry's. `postMessage`
 * is 1 and `scroll` is 0, which is what a browser answers.
 *
 * A PER-CALL-SITE `length` PARAMETER IS WHAT THIS REPLACES. Seven mixin installs reaching ONE declaration
 * disagreed with each other about it — five said 1 and two said 0 for the identical arity — and `new Event()`'s
 * interface object carried the DECLARED ARITY 2 where §3.7.1 computes 1. Neither could be caught from the
 * outside: a `length` is a number a page reads and a feature detection branches on, and nothing compared it
 * with the declaration standing one file away. */
static int idl_member_length_of(const IdlMember *m)
{
    int declared  = idl_declared_positions(m);
    int first_opt = idl_first_optional(m, 0);

    DCHECK(first_opt >= 0 && declared >= 0,
           "a member's declaration reached §3.7.7 Operations' length with a negative position count — both "
           "halves are indices into its own type list, and idl_method_id_ext sets them at declaration");
    return first_opt < declared ? first_opt : declared;
}

/* WHAT ONE NESTED-CONVERSION FRAME OWNS, declared ONCE. The state's own visit walks every declared frame
   through this, and the reset below discharges the same declaration with the freeing consumer — so a value
   added to a frame is added in one place and both consumers see it. */
static void idl_conv_frame_visit(JSContext *ctx, IdlConvFrame *f, JSStepVisit *v)
{
    iter_cursor_visit(ctx, &f->cur, v);
    v->val(ctx, &f->src);
    v->val(ctx, &f->list);
    v->val(ctx, &f->esrc);
    v->val(ctx, &f->eout);
    v->val(ctx, &f->mv);
}

/* Release everything one frame holds and leave it empty. Safe on a frame that was never used: a zeroed state's
   JSValue is the non-refcounted integer 0, so freeing it does nothing. */
static void idl_conv_frame_clear(JSContext *ctx, IdlConvFrame *f)
{
    idl_conv_frame_visit(ctx, f, JS_StepFreeVisitor());
    memset(f, 0, sizeof *f);
    f->src = f->list = f->esrc = f->eout = f->mv = JS_UNDEFINED;
}

/* Begin one sequence level over `src`. Returns -1 with a throw live if its list could not be allocated.
   THE FRAMES ARRIVE AS A PARAMETER and are never stored on the walk — see idl_args.h: a byte-copying deep fork
   would carry a stored pointer into the ORIGINAL state block, so the host re-derives them from its own layout on
   every entry and there is nothing here to go stale. */
static int idl_conv_push(JSContext *ctx, IdlDictWalk *w, IdlConvFrame *frames, int frames_cap,
                         JSValueConst src, const IdlDictDecl *d)
{
    IdlConvFrame *f;

    DCHECK(d != NULL, "a `sequence<(DOMString or D)>` was converted with no dictionary named for its union's "
                      "second arm");
    CHECK(w->conv_sp < frames_cap,
          "a nested IDL conversion went deeper than the DECLARED type tree — the depth is computed from the "
          "declaration and the host sized its frames for it, so this means the two have drifted apart");
    f = &frames[w->conv_sp++];
    idl_conv_frame_clear(ctx, f);
    iter_cursor_init(&f->cur);
    f->d = d;
    f->atoms = idl_dict_atoms(d);
    f->src = JS_DupValue(ctx, src);
    f->list = JS_NewArray(ctx);
    if (JS_IsException(f->list)) { f->list = JS_UNDEFINED; return -1; }
    return 0;
}

static void idl_conv_pop(JSContext *ctx, IdlDictWalk *w, IdlConvFrame *frames)
{
    DCHECK(w->conv_sp > 0, "a nested IDL conversion popped a frame it never pushed");
    idl_conv_frame_clear(ctx, &frames[--w->conv_sp]);
}

/* DRIVE the stack one re-entry's worth. Returns >0 (the caller returns it — the machine is parked inside the
   page's iterator or one of its getters), 0 with *pout holding the converted sequence (owned), or -1 with a
   throw live. Every arm that parks does so with the cursor standing exactly where it was. */
static int idl_conv_run(JSContext *ctx, JSStepHdr *hdr, IdlDictWalk *w, IdlConvFrame *frames, int frames_cap,
                        JSValue in, JSValue *pout, JSValue **out_cb, int *out_argc)
{
    int r;

    DCHECK(w->conv_sp > 0, "the nested conversion was driven with no frame under it");
    for (;;) {
        IdlConvFrame *f = &frames[w->conv_sp - 1];

        if (f->phase == IDL_CONV_PULL) {
            r = iter_cursor_run(ctx, hdr, &f->cur, f->src, in, out_cb, out_argc);
            in = JS_UNDEFINED;
            if (r > 0) return r;          /* parked ON THIS ELEMENT, at THIS depth */
            if (r < 0) return -1;
            if (f->cur.done) {
                JSValue done = f->list;

                f->list = JS_UNDEFINED;
                idl_conv_pop(ctx, w, frames);
                if (w->conv_sp == 0) { *pout = done; return 0; }
                {   /* the frame below is the MEMBER that named this sequence, waiting for its value */
                    IdlConvFrame *p = &frames[w->conv_sp - 1];

                    DCHECK(p->phase == IDL_CONV_MEMBERS && p->mphase == 1,
                           "a nested sequence finished under a frame that was not converting a member — a "
                           "level is pushed from exactly one place, and this is not where it returns to");
                    JS_FreeValue(ctx, p->mv);
                    p->mv = done;
                    p->mphase = 2;
                }
                continue;
            }
            /* UNKNOWN EXTERNAL INPUT CROSSES AS ITSELF, before the union is asked anything: a concolic IS an
               object, so resolving the arm first would read a dictionary's members off it and de-taint what
               the solver has to keep forking on. Same boundary rule every declared type follows. */
            if (concolic_is(f->cur.value)) {
                JS_SetPropertyUint32(ctx, f->list, f->n++, JS_DupValue(ctx, f->cur.value));
                continue;
            }
            /* §3.2.25 over `(DOMString or D)`: null and undefined take the dictionary arm (step 4) and so does
               any Object (step 10); everything else reaches step 12's string arm. */
            if (JS_IsObject(f->cur.value) || JS_IsNull(f->cur.value) || JS_IsUndefined(f->cur.value)) {
                f->esrc = JS_DupValue(ctx, f->cur.value);
                f->eout = JS_NewObject(ctx);
                if (JS_IsException(f->eout)) { f->eout = JS_UNDEFINED; return -1; }
                f->mi = 0;
                f->mphase = 0;
                f->phase = IDL_CONV_MEMBERS;
                continue;
            }
            f->phase = IDL_CONV_STRING;
            continue;
        }

        if (f->phase == IDL_CONV_STRING) {
            JSValue str = JS_UNDEFINED;

            r = step_tostring_run(ctx, hdr, f->cur.value, in, &str, out_cb, out_argc);
            in = JS_UNDEFINED;
            if (r > 0) return r;
            if (r < 0) return -1;
            JS_SetPropertyUint32(ctx, f->list, f->n++, str);
            f->phase = IDL_CONV_PULL;
            continue;
        }

        DCHECK(f->phase == IDL_CONV_MEMBERS, "a nested conversion resumed at a phase it never parks in");
        {
            bool pushed = false;

            while (f->mi < f->d->n) {
                const IdlDictMember *dm = &f->d->members[f->mi];

                if (f->mphase == 0) {
                    /* §3.2.17 step 4.1.2: an undefined or null dictionary has no object to read from — every
                       member is absent and none of the page's code runs. */
                    if (JS_IsObject(f->esrc)) {
                        r = step_getprop_run(ctx, hdr, f->esrc, f->atoms[f->mi], in, &f->mv,
                                             out_cb, out_argc);
                        in = JS_UNDEFINED;
                        if (r > 0) return r;   /* parked ON THIS MEMBER's read; the resume comes back to it */
                        if (r < 0) return -1;
                    }
                    f->mphase = 1;
                    if (dm->required && JS_IsUndefined(f->mv)) {
                        JS_ThrowTypeError(ctx, "required member %s of dictionary %s is undefined",
                                          dm->name, f->d->name);
                        return -1;
                    }
                }
                if (f->mphase == 1) {
                    if (JS_IsUndefined(f->mv)) {
                        /* step 4.1.5's default, or a member that simply does not exist — which this engine
                           spells as the undefined it already holds. */
                        if (dm->dflt != IDL_DEFAULT_NONE) {
                            f->mv = idl_default_value(ctx, dm);
                            if (JS_IsException(f->mv)) { f->mv = JS_UNDEFINED; return -1; }
                        }
                    } else if (concolic_is(f->mv)) {
                        /* crosses as itself, exactly as an argument of unknown external input does */
                    } else if (dm->type == IDL_SEQUENCE_STRING_OR_DICT) {
                        if (!JS_IsObject(f->mv)) {
                            JS_ThrowTypeError(ctx, "member %s of dictionary %s is not a sequence",
                                              dm->name, f->d->name);
                            return -1;
                        }
                        if (idl_conv_push(ctx, w, frames, frames_cap, f->mv, dm->dict) < 0) return -1;
                        pushed = true;
                        break;
                    } else if (dm->type == IDL_DOMSTRING || dm->type == IDL_DOMSTRING_NULLABLE) {
                        /* `DOMString?`: null is the IDL null and never the four characters "null". */
                        if (!(dm->type == IDL_DOMSTRING_NULLABLE && JS_IsNull(f->mv))) {
                            JSValue str = JS_UNDEFINED;

                            r = step_tostring_run(ctx, hdr, f->mv, in, &str, out_cb, out_argc);
                            in = JS_UNDEFINED;
                            if (r > 0) return r;
                            if (r < 0) return -1;
                            JS_FreeValue(ctx, f->mv);
                            f->mv = str;
                        }
                    } else {
                        DFAIL("a dictionary reached through a sequence declared a member type this conversion "
                              "does not perform — it converts DOMString, DOMString? and another "
                              "`sequence<(DOMString or D)>`, which is what HTML §8.6.3's dictionaries are made "
                              "of. Build the missing conversion here; placing the value unconverted would hand "
                              "the component the page's own object");
                        JS_ThrowTypeError(ctx, "dictionary member %s has a type this engine cannot convert",
                                          dm->name);
                        return -1;
                    }
                    f->mphase = 2;
                }
                DCHECK(f->mphase == 2, "a nested dictionary member resumed at a phase it never parks in");
                JS_SetPropertyStr(ctx, f->eout, dm->name, f->mv);
                f->mv = JS_UNDEFINED;
                f->mi++;
                f->mphase = 0;
            }
            if (pushed) continue;
            JS_SetPropertyUint32(ctx, f->list, f->n++, f->eout);
            f->eout = JS_UNDEFINED;
            JS_FreeValue(ctx, f->esrc);
            f->esrc = JS_UNDEFINED;
            f->phase = IDL_CONV_PULL;
        }
    }
}

/* ---- `? GetMethod(V, %Symbol.iterator%)`: THE ARM, AS A REQUEST -------------------------------------------
 *
 * TWO ALGORITHMS ASK THIS ONE QUESTION, and they are stated here once because a second copy is a second notion
 * of GetMethod that can answer differently:
 *   - §3.2.25 Union types over `(DOMString or sequence<DOMString>)` and `(double or sequence<double>)`. The
 *     member types are a flat type and a sequence and NOTHING else, so steps 4 through 10 name no arm the
 *     union has and the whole decision is step 11.2 against the flat clause below it.
 *   - §3.6 Overload resolution algorithm step 12's SEQUENCE clause — "Otherwise: if V is an Object and there
 *     is an entry in S that has … a sequence type … and after performing the following steps, Let method be
 *     ? GetMethod(V, %Symbol.iterator%). method is not undefined, then remove from S all other entries."
 * Either way it is a [[Get]] of the PAGE'S value, so this is a rest point, and which arm it answered has to
 * survive the rest.
 *
 * WHY THE METHOD IS HANDED TO THE CURSOR RATHER THAN RE-READ. §3.2.21.1's "creating a sequence from an
 * iterable" takes the iterable AND the method the caller already obtained — which is why §3.6 step 14 says
 * "creating a sequence of type T from V and method" rather than converting V afresh. A cursor that read
 * @@iterator itself would perform the property access twice for one conversion, which a Proxy `get` trap
 * counts and an accessor can answer differently the second time. iter_cursor_init_from_method is that second
 * entry, and it is why the method is never held on this state across a park: it is consumed on the entry that
 * completes the read. */
enum { IDL_UNI_ASK = 0, IDL_UNI_STRING, IDL_UNI_SEQUENCE };

/* Resolve the arm at `v`: `flat_arm` (there is no @@iterator) or `seq_arm` (there is, with this machine's
   cursor already planted on the method the page gave). §3.2.25 step 2's null arm is the CALLER's — it runs
   none of the page's code and the callers place the IDL null in different slots — and so is §3.6 step 12's
   undefined/null/non-object clause chain, for the same reason.
   `phase` IS THE CALLER'S BYTE and not a fixed field, because which conversions can be in flight at once
   differs: a union arm and a dictionary member's union arm never overlap, while an OVERLOAD arm that resolved
   to a dictionary is still live while that dictionary's members convert. See JSIdlArgsState::ovl_phase.
   `*pin` is the request answer on the way in and is CLEARED when it was consumed, because a resume that lands
   here on its way back INTO the sequence still has to hand that answer to the cursor. Returns >0 (the caller
   returns it), 0 with *pout set, or -1 with a throw live. */
static int idl_union_seq_arm(JSContext *ctx, JSStepHdr *hdr, IdlDictWalk *w, uint8_t *phase, JSValueConst v,
                             JSValue *pin, IdlArgType *pout, IdlArgType seq_arm, IdlArgType flat_arm,
                             JSValue **out_cb, int *out_argc)
{
    /* §3.2.25 step 11 asks its whole question of an OBJECT; every other value reaches the flat clause without
       a read. (§3.6's caller has already refused a non-object, so this line is the union's alone.) */
    if (*phase == IDL_UNI_ASK && !JS_IsObject(v))
        *phase = IDL_UNI_STRING;
    if (*phase == IDL_UNI_ASK) {
        JSValue method = JS_UNDEFINED;
        int r = step_getprop_run(ctx, hdr, v, JS_WellKnownSymbolAtom(JS_WKS_ITERATOR), *pin, &method,
                                 out_cb, out_argc);

        *pin = JS_UNDEFINED;
        if (r > 0) return r;      /* parked INSIDE the page's @@iterator getter or its Proxy trap */
        if (r < 0) return -1;
        /* ECMAScript's GetMethod, which is the operation §3.2.25 step 11.2 and §3.6 step 12's sequence clause
           both name and not a plain [[Get]]. Its three steps after the read are stated ONCE, in idl_iter.c:
           undefined and null are "there is no method" and fall through to the flat arm, and anything else that
           is not callable is a TypeError rather than a quiet second arm. The copy that stood here was the same
           three lines, and the two other unions in the tree each spelled the question a third way. */
        r = idl_get_method(ctx, method, "the value's @@iterator");
        if (r <= 0) {
            JS_FreeValue(ctx, method);
            if (r < 0) return -1;
            *phase = IDL_UNI_STRING;
        } else {
            /* THE STATE IS COMPLETE BEFORE THE METHOD IS HANDED OVER: the list is placed on the machine
               first, so a failure here tears down through the same `visit` that would have named it, and the
               method is released by the one branch that still owns it. */
            DCHECK(w->seq_phase == 0,
                   "the union's sequence arm was chosen while a sequence was already in flight on this "
                   "machine — one argument is converted at a time, so the cursor has exactly one user");
            w->seq_list = JS_NewArray(ctx);
            if (JS_IsException(w->seq_list)) {
                w->seq_list = JS_UNDEFINED;
                JS_FreeValue(ctx, method);
                return -1;
            }
            iter_cursor_init_from_method(ctx, &w->seq, method);   /* CONSUMES `method` */
            w->seq_phase = 1;
            *phase = IDL_UNI_SEQUENCE;
        }
    }
    DCHECK(*phase == IDL_UNI_STRING || *phase == IDL_UNI_SEQUENCE,
           "a position whose arm is decided by @@iterator resumed at an arm neither §3.2.25 step 11.2 nor "
           "§3.6 step 12's sequence clause parks in");
    /* WHICH TWO TYPES THE TWO OUTCOMES ARE is the only thing each caller states for itself; the
       `? GetMethod(V, %Symbol.iterator%)` read above is the ALGORITHM's and is stated once. `IDL_UNI_STRING`
       names the FLAT arm rather than a string one — the phase is "there was no @@iterator", which is the same
       fact for every caller of this shape, and §3.6's flat arm is a DICTIONARY rather than a string at all. */
    *pout = (*phase == IDL_UNI_SEQUENCE) ? seq_arm : flat_arm;
    return 0;
}

/* §3.2.11 ByteString's RANGE, defined below beside the public idl_is_bytestring it is the throwing form of.
   Forward-declared rather than moved, because it is a CONVERSION detail of a type and belongs with its type's
   other half, and the walk below is the only caller that stands before it. */
static int idl_bytestring_check(JSContext *ctx, JSValueConst str);

/* ---- WEB IDL §3.2.17 Dictionary types, AS THE ONE WALK BOTH ENTRIES DRIVE ---------------------------------
 *
 * See idl_args.h for what the two entries are and why there is one machine. What is below is the whole of the
 * conversion: the argument path calls it for an IDL_DICT position, and an algorithm calls it for a dictionary
 * it is holding. There is no argument-only copy left, which is the point — the seam between two copies is where
 * the bugs in this shape come from, and there is no seam. */

/* THE NAME A DIAGNOSTIC USES. An algorithm's dictionary has an IDL identifier; an argument's is an anonymous
   member list (the declaration passes the bare array), so it answers with what the page can see instead. It is
   never a hole a `%s` would print as "(null)". */
static const char *idl_dict_where(const IdlDictWalk *w)
{
    return w->name ? w->name : "the dictionary argument";
}

int idl_dict_walk_start(JSContext *ctx, IdlDictWalk *w, JSValueConst src,
                        const IdlDictMember *members, int n, const JSAtom *atoms, const char *name,
                        JSClassID iface, bool (*narrow)(JSValueConst v),
                        IdlConvFrame *frames, int frames_cap)
{
    DCHECK(!w->started, "a §3.2.17 dictionary conversion was started while one was already in flight on this "
                        "walk — a host converts one dictionary at a time and TAKES it before starting the "
                        "next, so this is a walk whose previous conversion was never collected");
    DCHECK(members != NULL && atoms != NULL && n > 0,
           "a §3.2.17 dictionary conversion was started with no member list or no interned names — the atoms "
           "must outlive the suspension between a member read's two halves, which is why they are interned at "
           "declaration (idl_dict_declare) and never made per read");
    /* §3.2.17 (ES-to-IDL list) STEP 1 IS THE CALLER'S, and it is asserted rather than performed because the two
       entries throw it in two different places: the argument path at the position it is converting, an
       algorithm wherever its own branch decided this value is a dictionary at all. Undefined and null are LEGAL
       here — step 4.1.2 gives every member `undefined` and the loop defaults them. */
    DCHECK(JS_IsObject(src) || JS_IsUndefined(src) || JS_IsNull(src),
           "a §3.2.17 dictionary conversion was started over a value that is neither an Object, undefined nor "
           "null — that is step 1's TypeError and it belongs at the caller, which is the only place that knows "
           "what to say about where the value came from");
    /* THE FRAMES ARE THE HOST'S AND THIS IS WHERE THE TWO NUMBERS MEET. `idl_members_depth` is what the
       argument machine sizes its tail from; an algorithm has to size its own, and a dictionary that declares a
       `sequence<(DOMString or D)>` member and was given no frames would otherwise fail at the DEPTH rather
       than at the start, naming a drift instead of a host that never provided them. */
    DCHECK(frames_cap >= idl_members_depth(members, n),
           "a §3.2.17 dictionary conversion was started with fewer nested-conversion frames than its declared "
           "type tree needs — the depth is idl_members_depth over the member list, and the host block that "
           "embeds this walk is what has to be sized for it");
    DCHECK(frames != NULL || frames_cap == 0, "a nested-conversion depth was declared with no frames behind it");
    (void)frames; (void)frames_cap;   /* the frames are the RUN's, and both are read only by the asserts above */
    /* THE STATE IS COMPLETE BEFORE THE FIRST OPERATION THAT CAN THROW. `out` is the only allocation here, and
       everything the walk owns is placed before it — so a failure to mint it tears down through the same
       `visit`/`clear` that would have named the rest, and leaves nothing half-adopted. */
    w->members = members;
    w->atoms   = atoms;
    w->name    = name;
    w->n       = n;
    w->iface   = iface;
    w->narrow  = narrow;
    w->mi      = 0;
    w->mphase  = 0;
    w->mv      = JS_UNDEFINED;
    w->seq_list = JS_UNDEFINED;
    w->seq_n   = 0;
    w->seq_phase = 0;
    w->uni_phase = IDL_UNI_ASK;
    w->conv_sp = 0;
    iter_cursor_init(&w->seq);
    w->src = JS_DupValue(ctx, src);
    w->out = JS_NewObject(ctx);                                          /* step 2's idlDict */
    if (JS_IsException(w->out)) {
        w->out = JS_UNDEFINED;
        JS_FreeValue(ctx, w->src);
        w->src = JS_UNDEFINED;
        return -1;
    }
    w->started = 1;
    return 0;
}

int idl_dict_walk_run(JSContext *ctx, JSStepHdr *hdr, IdlDictWalk *w, IdlConvFrame *frames, int frames_cap,
                      JSValue in, JSValue **out_cb, int *out_argc)
{
    int r;

    DCHECK(w->started, "a §3.2.17 dictionary conversion was driven with nothing in flight — the walk is started "
                       "once and driven until it answers 0, and a resume that lost the start would read member "
                       "zero of a dictionary with no source and no object to place it on");
    while (w->mi < w->n) {
        const IdlDictMember *dm = &w->members[w->mi];
        IdlArgType mt = dm->type;

        if (w->mphase == 0) {
            /* §3.2.17 (ES-to-IDL list) step 4.1.2: "If jsDict is either undefined or null, let
               jsMemberValue be undefined" — so a non-object source READS NOTHING and every member
               reaches step 4.1.5's default or step 4.1.6's refusal through this same loop. That is why
               there is no second "default them all" pass: there was one, in the argument path, and a
               conversion with both is two answers to one section. */
            if (JS_IsObject(w->src)) {                                  /* step 4.1.3.1's `? Get` */
                r = step_getprop_run(ctx, hdr, w->src, w->atoms[w->mi], in, &w->mv,
                                     out_cb, out_argc);
                in = JS_UNDEFINED;
                if (r > 0) return r;  /* parked ON THIS MEMBER's read; the resume comes back to it */
                if (r < 0) return -1;
            }
            w->mphase = 1;
            /* `in` is released for the reason step 4.1.5's arm below states: a non-object source consumed
               nothing on the way here, and this is the other edge that can be reached holding it. */
            if (dm->required && JS_IsUndefined(w->mv)) {                 /* step 4.1.6 */
                JS_FreeValue(ctx, in);
                JS_ThrowTypeError(ctx, "required member %s of %s is undefined",
                                  dm->name, idl_dict_where(w));
                return -1;
            }
        }
        DCHECK(mt != IDL_DICT, "a dictionary member was declared as a dictionary — the conversion "
                               "cursor is per-argument, so a nested one would read the outer's names");
        /* AN OVERLOAD SPLIT IS NOT A TYPE A MEMBER CAN HAVE. §3.6 resolves between two ENTRIES of an
           effective overload set at an ARGUMENT INDEX; a dictionary member has no entries and no
           index, so a member declared this way names a decision there is nothing to decide. Asserted
           here because the member loop below would otherwise fall past every arm and place the page's
           value unconverted, which is the silent shape rather than the loud one. */
        DCHECK(mt != IDL_SEQUENCE_OBJECT_OR_DICT,
               "a dictionary member was declared as a §3.6 overload split — that type resolves between "
               "two overload ENTRIES at an argument position, and a member is inside one entry already; "
               "the member's own IDL names either the sequence or the dictionary, never both");
        /* §3.2.21 OVER A DICTIONARY MEMBER IS THIS FUNCTION'S OWN COPY OF THE ARGUMENT-POSITION WALK, and it
           has no §3.2.18 element arm — so a member declared this way would run step 3.3's element conversion
           as a plain ToString and place every string the page wrote, which is the same silent shape the two
           asserts above name. Twenty dictionary members of the platform's IDL are `sequence<E>`
           (GPUDeviceDescriptor's requiredFeatures and XRHitTestOptionsInit's entityTypes among them), so this
           is a row to BUILD beside the IDL_SEQUENCE_DOMSTRING arm below — the element conversion is
           step_tostring_run then idl_enum_check against `dm->values`, exactly as the argument path does — and
           not a shape to keep out. */
        DCHECK(mt != IDL_SEQUENCE_ENUM,
               "a dictionary member was declared `sequence<E>` and the dictionary walk has no §3.2.18 element "
               "conversion — build it beside this function's IDL_SEQUENCE_DOMSTRING arm, over dm->values");
        /* §3.2.17 (ES-to-IDL list) step 4.1.5's DEFAULT comes first, because it is the difference between a
           member that does not exist and one that exists holding what the IDL wrote. It is already an IDL
           value, so nothing converts it.
           `in` IS RELEASED ON THE FAILURE EDGE because this is the ONE arm a live request answer can reach: a
           non-object source reads nothing (step 4.1.2), so nothing consumed `in` on the way here, and every
           other arm below is unreachable in that state — an absent member is rewritten to IDL_ANY before any of
           them is asked. Every arm that CAN be reached with `in` live either consumes it or frees it here. */
        if (JS_IsUndefined(w->mv) && dm->dflt != IDL_DEFAULT_NONE) {
            w->mv = idl_default_value(ctx, dm);
            if (JS_IsException(w->mv)) {
                w->mv = JS_UNDEFINED;
                JS_FreeValue(ctx, in);
                return -1;
            }
            mt = IDL_ANY;
        }
        /* An ABSENT member is not converted: `undefined` on a dictionary means the member is not
           there, and running ToString over it would write the four characters `undefined` where the
           spec puts nothing. IDL_BOOLEAN is the exception because ToBoolean(undefined) is false, which
           IS the `= false` default a member declared that way carries — and IDL_BOOLEAN_NO_DEFAULT is
           the member that declares no default, which is exactly why it is NOT exempt here: it stays
           undefined so the body can tell absence from false (see idl_args.h). */
        if (JS_IsUndefined(w->mv) && mt != IDL_BOOLEAN)
            mt = IDL_ANY;
        /* The same boundary rule the arguments follow: unknown external input crosses as ITSELF, so a
           concolic member keeps forking control flow instead of collapsing at a coercion. */
        if (mt != IDL_ANY && concolic_is(w->mv))
            mt = IDL_ANY;
        /* §3.2.25 over `(DOMString or sequence<DOMString>)` ON A DICTIONARY MEMBER — the same union the
           argument path resolves, resolved here so the arm's @@iterator read parks on the MEMBER it is
           on. It rewrites `mt` and the arms below convert what it chose; step 2's null is placed
           directly, because the IDL null runs none of the page's code and is not a string arm. */
        if (mt == IDL_DOMSTRING_OR_SEQUENCE || mt == IDL_DOMSTRING_OR_SEQUENCE_NULLABLE) {
            if (mt == IDL_DOMSTRING_OR_SEQUENCE_NULLABLE &&
                (JS_IsNull(w->mv) || JS_IsUndefined(w->mv))) {
                JS_FreeValue(ctx, w->mv);
                w->mv = JS_NULL;
                mt = IDL_ANY;
            } else {
                r = idl_union_seq_arm(ctx, hdr, w, &w->uni_phase, w->mv, &in, &mt,
                                      IDL_SEQUENCE_DOMSTRING, IDL_DOMSTRING, out_cb, out_argc);
                if (r > 0) return r;   /* parked ON THIS MEMBER's arm; the resume finds it chosen */
                if (r < 0) return -1;
            }
        }
        /* §3.2.25 over `(double or sequence<double>)` ON A DICTIONARY MEMBER — Intersection Observer
           §2.4's `threshold`. The same step 11.2 read, resolved here so the @@iterator access parks on
           the MEMBER it is on rather than after every later member of the same dictionary was read. */
        if (mt == IDL_DOUBLE_OR_SEQUENCE) {
            r = idl_union_seq_arm(ctx, hdr, w, &w->uni_phase, w->mv, &in, &mt,
                                  IDL_SEQUENCE_DOUBLE, IDL_DOUBLE, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return -1;
        }
        if (mt == IDL_SEQUENCE_STRING_OR_DICT) {
            /* §3.2.21 whose element type is §3.2.25's `(DOMString or D)` — the conversion that NESTS,
               driven from here as a stack of cursors so a resume comes back to the element it was on
               at the depth it was at. A value that is not an Object is a TypeError before anything is
               read, exactly as it is for every other sequence: the check is on the TYPE and not on
               iterability. */
            JSValue seq = JS_UNDEFINED;

            if (w->conv_sp == 0) {
                if (!JS_IsObject(w->mv)) {
                    JS_FreeValue(ctx, in);
                    JS_ThrowTypeError(ctx, "member `%s` of %s is not a sequence", dm->name,
                                  idl_dict_where(w));
                    return -1;
                }
                if (idl_conv_push(ctx, w, frames, frames_cap, w->mv, dm->dict) < 0) {
                    JS_FreeValue(ctx, in);
                    return -1;
                }
            }
            r = idl_conv_run(ctx, hdr, w, frames, frames_cap, in, &seq, out_cb, out_argc);
            in = JS_UNDEFINED;
            if (r > 0) return r;   /* parked INSIDE the page's iterator, at whatever depth */
            if (r < 0) return -1;
            JS_FreeValue(ctx, w->mv);
            w->mv = seq;
        }
        else if (mt == IDL_SEQUENCE_DOMSTRING || mt == IDL_SEQUENCE_INTERFACE ||
                 mt == IDL_SEQUENCE_OBJECT || mt == IDL_SEQUENCE_DOUBLE) {
            /* §3.2.21 over a dictionary member. A value that is not an Object is a TypeError before
               anything is read, exactly as it is in argument position — the check is on the TYPE and
               not on iterability, so `{attributeFilter: "id"}` throws even though a string iterates.
               The cursor is the machine's own (`seq`), which the argument-position conversion also
               uses: the two cannot be in flight at once because arguments are converted strictly left
               to right and one argument is being converted here. */
            DCHECK(w->seq_phase == 0 || JS_IsObject(w->seq_list),
                   "a dictionary member's sequence resumed with no list under it");
            if (!JS_IsObject(w->mv)) {
                JS_FreeValue(ctx, in);
                JS_ThrowTypeError(ctx, "member `%s` of %s is not a sequence", dm->name,
                                  idl_dict_where(w));
                return -1;
            }
            if (w->seq_phase == 0) {
                w->seq_list = JS_NewArray(ctx);
                if (JS_IsException(w->seq_list)) { JS_FreeValue(ctx, in); return -1; }
                iter_cursor_init(&w->seq);
                w->seq_phase = 1;
            }
            for (;;) {
                JSValue str = JS_UNDEFINED;

                if (w->seq_phase == 1) {
                    r = iter_cursor_run(ctx, hdr, &w->seq, w->mv, in, out_cb, out_argc);
                    in = JS_UNDEFINED;
                    if (r > 0) return r;   /* parked ON THIS ELEMENT; the resume comes back to it */
                    if (r < 0) return -1;
                    if (w->seq.done) break;
                    /* §3.2.13's `object` over a dictionary member — HTML §9.4.4's `transfer`. An
                       Object crosses as itself and anything else is a TypeError, which runs none of
                       the page's code, so it is decided here rather than being a rest point. */
                    if (mt == IDL_SEQUENCE_OBJECT) {
                        if (!JS_IsObject(w->seq.value)) {
                            JS_ThrowTypeError(ctx, "an element of member `%s` of %s is "
                                              "not an object", dm->name, idl_dict_where(w));
                            return -1;
                        }
                        JS_SetPropertyUint32(ctx, w->seq_list, w->seq_n++,
                                             JS_DupValue(ctx, w->seq.value));
                        continue;
                    }
                    /* §3.2.15's ELEMENT CONVERSION: a platform object implementing the interface
                       crosses as itself and anything else is a TypeError. It runs none of the page's
                       code, so it is decided here rather than being a rest point of its own. */
                    if (mt == IDL_SEQUENCE_INTERFACE) {
                        DCHECK(w->iface != 0,
                               "a dictionary declared a sequence of an interface type with no class to "
                               "brand against — idl_iface_brand is the other half of that type");
                        if (!idl_is_iface(w->seq.value, w->iface) ||
                            (w->narrow && !w->narrow(w->seq.value))) {
                            JS_ThrowTypeError(ctx, "an element of member `%s` of %s does "
                                              "not implement the declared interface", dm->name,
                                              idl_dict_where(w));
                            return -1;
                        }
                        JS_SetPropertyUint32(ctx, w->seq_list, w->seq_n++,
                                             JS_DupValue(ctx, w->seq.value));
                        continue;
                    }
                    w->seq_phase = 2;
                }
                DCHECK(w->seq_phase == 2, "a dictionary member's sequence resumed at a phase it never "
                                          "parks in");
                /* §3.2.7 `double`'s element conversion — ToNumber is the page's `valueOf`, so it is a
                   request and its own rest point, and the RESTRICTED type refuses a non-finite result
                   here rather than leaving it for the algorithm that reads the list. */
                if (mt == IDL_SEQUENCE_DOUBLE) {
                    double d = 0.0;

                    r = step_todouble_run(ctx, hdr, w->seq.value, in, &d, out_cb, out_argc);
                    in = JS_UNDEFINED;
                    if (r > 0) return r;
                    if (r < 0) return -1;
                    if (!isfinite(d)) {
                        JS_ThrowTypeError(ctx, "an element of member `%s` of %s is not "
                                          "a finite double", dm->name, idl_dict_where(w));
                        return -1;
                    }
                    JS_SetPropertyUint32(ctx, w->seq_list, w->seq_n++, JS_NewFloat64(ctx, d));
                    w->seq_phase = 1;
                    continue;
                }
                r = step_tostring_run(ctx, hdr, w->seq.value, in, &str, out_cb, out_argc);
                in = JS_UNDEFINED;
                if (r > 0) return r;
                if (r < 0) return -1;
                JS_SetPropertyUint32(ctx, w->seq_list, w->seq_n++, str);
                w->seq_phase = 1;
            }
            JS_FreeValue(ctx, w->mv);
            w->mv = w->seq_list;
            w->seq_list = JS_UNDEFINED;
            w->seq_n = 0;
            w->seq_phase = 0;
        }
        else if (mt == IDL_BOOLEAN || mt == IDL_BOOLEAN_NO_DEFAULT) {
            JSValue b = JS_NewBool(ctx, JS_ToBool(ctx, w->mv));
            JS_FreeValue(ctx, w->mv);
            w->mv = b;
        }
        else if (mt == IDL_CALLBACK) {
            if (!JS_IsFunction(ctx, w->mv)) {
                JS_ThrowTypeError(ctx, "member `%s` of %s is not callable", dm->name,
                                  idl_dict_where(w));
                return -1;
            }
        }
        else if (mt == IDL_INTERFACE) {
            /* §3.2.15 on a dictionary member, which is where StaticRangeInit's four live: the brand
               test is the TYPE's, so the member does not cross as itself and the body performs no
               check of its own.
               WHICH CLASS IS THE MEMBER'S OWN WHEN IT STATES ONE. A declaration states one class
               (idl_iface_brand) and that is all a dictionary needs while every interface-typed member
               of it is the same interface; NavigateEventInit's four are four DIFFERENT interfaces, so
               the member carries its own and the declaration's is not consulted. Neither stated is the
               assert below and not a read past a missing class. */
            JSClassID want = dm->iface ? dm->iface : w->iface;

            DCHECK(want != 0, "a dictionary declared an interface-typed member with no class to brand "
                              "against — IdlDictMember::iface states it per member, and "
                              "idl_iface_brand states it once for a dictionary whose interface-typed "
                              "members are all one interface");
            if (!idl_is_iface(w->mv, want)) {
                JS_ThrowTypeError(ctx, "member `%s` of %s does not implement the "
                                  "declared interface", dm->name, idl_dict_where(w));
                return -1;
            }
        }
        else if (mt == IDL_INTERFACE_NULLABLE) {
            /* §3.2.15 Interface types UNDER §3.2.20 Nullable types' RULE, on a dictionary member. null and undefined are the
               IDL null and nothing is branded; what survives takes the brand AND the declaration's
               narrowing, which is what lets a member state a union of two interface types the way
               Intersection Observer §2.4's `(Element or Document)? root` does: both arms are Nodes to
               this engine's class system, so the class is the brand and the narrowing is the union.
               (An undefined member never reaches here — the absent-member rewrite above turns it into
               IDL_ANY before any conversion runs — so the null this branch sees is the page's own.)
               IT HAD NO BRANCH AT ALL and therefore crossed unconverted, which is the silent kind of
               gap: `{root: 5}` would have reached a body that then asked node_of for a node. */
            JSClassID want = dm->iface ? dm->iface : w->iface;

            DCHECK(want != 0, "a dictionary declared a nullable interface-typed member with no class "
                              "to brand against — IdlDictMember::iface states it per member, and "
                              "idl_iface_brand states it once per declaration");
            if (JS_IsNull(w->mv)) {
                /* the IDL null; nothing to brand */
            } else if (!idl_is_iface(w->mv, want) ||
                       (w->narrow && !w->narrow(w->mv))) {
                JS_ThrowTypeError(ctx, "member `%s` of %s does not implement the "
                                  "declared interface", dm->name, idl_dict_where(w));
                return -1;
            }
        }
        else if (idl_is_numeric(mt)) {
            /* WHAT ToNumber PRODUCED, before the type's arithmetic — the NUMBER, because that is what
               §3.2 works from and a saturating int64 has already lost the modulo and the half-to-even
               rounding. It is a LOCAL and not state: step_todouble_run writes it only on the entry that
               COMPLETES the coercion, so a park leaves nothing behind to carry across. It used to be an
               array indexed by the argument cursor, which a variadic member with a numeric tail would
               have run off the end of. */
            double num;

            r = step_todouble_run(ctx, hdr, w->mv, in, &num, out_cb, out_argc);
            in = JS_UNDEFINED;
            if (r > 0) return r;   /* parked ON THIS MEMBER's conversion; the read does not re-run */
            if (r < 0) return -1;
            JS_FreeValue(ctx, w->mv);
            w->mv = idl_num_of(ctx, mt, num);
            if (JS_IsException(w->mv)) {   /* §3.2.7's restricted double refused the value */
                w->mv = JS_UNDEFINED;
                return -1;
            }
        }
        else if (mt == IDL_DOMSTRING || mt == IDL_DOMSTRING_NULLABLE || mt == IDL_BYTESTRING ||
                 mt == IDL_USVSTRING || mt == IDL_USVSTRING_NULLABLE || mt == IDL_ENUM ||
                 mt == IDL_ENUM_NULLABLE) {
            if ((mt == IDL_DOMSTRING_NULLABLE || mt == IDL_USVSTRING_NULLABLE ||
                 mt == IDL_ENUM_NULLABLE) && JS_IsNull(w->mv)) {
                /* `DOMString?` / `NavigationType?`: null is the IDL null, never the string "null". */
            } else {
                JSValue str = JS_UNDEFINED;
                r = step_tostring_run(ctx, hdr, w->mv, in, &str, out_cb, out_argc);
                in = JS_UNDEFINED;
                if (r > 0) return r;
                if (r < 0) return -1;
                JS_FreeValue(ctx, w->mv);
                w->mv = str;
                if (mt == IDL_BYTESTRING && idl_bytestring_check(ctx, w->mv) < 0)
                    return -1;
                if (mt == IDL_USVSTRING || mt == IDL_USVSTRING_NULLABLE) {
                    w->mv = JS_ToScalarValueString(ctx, w->mv);
                    if (JS_IsException(w->mv)) return -1;
                }
                if ((mt == IDL_ENUM || mt == IDL_ENUM_NULLABLE) &&
                    idl_enum_check(ctx, w->mv, dm->values, dm->name) < 0)
                    return -1;
            }
        }
        JS_SetPropertyStr(ctx, w->out, dm->name, w->mv);
        w->mv = JS_UNDEFINED;
        w->mphase = 0;
        /* THE UNION'S ARM IS PER MEMBER, and this is the one place a member finishes — the next
           member's own §3.2.25 must ask its own value rather than inherit the answer this one got. */
        w->uni_phase = IDL_UNI_ASK;
        w->mi++;
    }
    JS_FreeValue(ctx, in);
    /* STEP 5: the whole dictionary is read. The walk keeps `out` until the host TAKES it, so a host that parks
       between the last member and its own next stage still owns exactly one reference to it. */
    return 0;
}

JSValue idl_dict_walk_take(JSContext *ctx, IdlDictWalk *w)
{
    JSValue out;

    DCHECK(w->started, "a §3.2.17 dictionary was taken from a walk that never started one");
    DCHECK(w->mi == w->n && w->mphase == 0,
           "a §3.2.17 dictionary was taken HALF-READ. Its unread members are absent on the object, which is "
           "indistinguishable from members the page did not write — so the algorithm that receives it would "
           "read the walk's progress as the page's data. Take it only on idl_dict_walk_run's 0 edge");
    DCHECK(w->conv_sp == 0, "a §3.2.17 dictionary was taken with a nested sequence conversion still in flight");
    out = w->out;
    w->out = JS_UNDEFINED;
    JS_FreeValue(ctx, w->src);
    w->src = JS_UNDEFINED;
    w->started = 0;
    w->mi = 0;
    return out;
}

void idl_dict_walk_visit(JSContext *ctx, IdlDictWalk *w, IdlConvFrame *frames, int frames_cap, JSStepVisit *v)
{
    int i;

    v->val(ctx, &w->src);
    v->val(ctx, &w->out);
    v->val(ctx, &w->mv);
    v->val(ctx, &w->seq_list);
    iter_cursor_visit(ctx, &w->seq, v);
    DCHECK(frames != NULL || frames_cap == 0,
           "a walk's frames were visited through a null block with a non-zero capacity — the two are one "
           "statement of the host's layout and a `visit` that took only the count would drop what a live frame "
           "still holds");
    for (i = 0; i < frames_cap; i++)
        idl_conv_frame_visit(ctx, &frames[i], v);
}

/* EVERYTHING THE WALK SAYS ABOUT WHAT IS IN FLIGHT, and nothing it OWNS — so it is safe both beside a free
   (idl_dict_walk_clear, which frees first) and beside a discharge that has not happened yet (the argument
   machine's own result, whose owned values the driver releases through `visit` after it returns). Stated once
   because a state with `started` cleared and a cursor left standing is a walk that would resume into the middle
   of a dictionary nobody is converting. */
static void idl_dict_walk_reset_cursors(IdlDictWalk *w)
{
    iter_cursor_init(&w->seq);
    w->seq_n = 0;
    w->seq_phase = 0;
    w->uni_phase = IDL_UNI_ASK;
    w->conv_sp = 0;
    w->mi = 0;
    w->mphase = 0;
    w->started = 0;
}

void idl_dict_walk_clear(JSContext *ctx, IdlDictWalk *w, IdlConvFrame *frames, int frames_cap)
{
    idl_dict_walk_visit(ctx, w, frames, frames_cap, JS_StepFreeVisitor());
    w->src = w->out = w->mv = w->seq_list = JS_UNDEFINED;
    idl_dict_walk_reset_cursors(w);
}

/* WHAT THIS MACHINE OWNS: the coerced arguments so far. A concolic branch inside one page `toString` forks the
   flow at that depth, and the two arms must not share one argument vector. */
static void js_idl_args_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSIdlArgsState *s = st;
    const IdlMember *m;
    int i;

    DCHECK(s->hdr.arg >= 0 && s->hdr.arg < g_n, "an IDL member's visit ran with no pool entry behind it");
    m = idl_member(s->hdr.arg);
    v->val(ctx, &s->result);
    v->val(ctx, &s->conv);
    v->val(ctx, &s->vstage);
    v->val(ctx, &s->ce_exc);
    v->val(ctx, &s->ace_ctor);
    /* §3.2.17's WALK — its source, the dictionary it is building, the member in flight, the sequence cursor and
       EVERY declared frame, named in ONE place that both entries share. The frames are handed in rather than
       stored on the walk, so this is where the argument machine's tail layout meets it. */
    idl_dict_walk_visit(ctx, &s->dw, idl_frames(m, st), m->conv_depth, v);
    /* EVERY declared position — the vector's size is the member's `nargs`, so this loop and the tail the
       declaration allocated are the same number read from the same place. */
    for (i = 0; i < m->nargs; i++)
        v->val(ctx, &idl_args_vec(st)[i]);
    /* A FORK CAN HAPPEN MID-DRAIN, and this is the buffer's clone contract. It used to assert the opposite —
       "a fork is a concolic branch, which is bytecode; the drain runs none" — and that was true only while
       every per-node effect was an enqueue. HTML §6.6.7's insertion steps are not: their step 5 asks §6.6.6's
       allow focus steps, whose second clause is §6.4.1's transient activation, which is unknown external state
       and therefore a FORK inside the walk. So the walk's buffer is visited like any other owned storage —
       each arm gets its own cursor and its own per-node phase, and neither frees the other's. */
    if (s->tree) {
        DCHECK(g_tree != NULL, "an IDL member holds a tree-steps buffer with no DOM layer registered — the "
                               "buffer can only have come from that layer's own take");
        g_tree->visit(ctx, &s->tree, v);
    }
    custom_elements_queue_visit(ctx, &s->ce, v);
    /* UNCONDITIONAL, because a step member's declaration is now REQUIRED (idl_method_id_step asserts it). The
       guard used to be `m->step->visit != NULL`, and a false arm that visits nothing is how a body's owned
       values would be invisible to the fork AND to the teardown at once — which is the same state the pool
       refuses at the declaration rather than tolerating here. */
    if (m->step) m->step->visit(ctx, idl_body_state(m, st), v);
}

/* MAY THIS MEMBER'S MACHINE BE FORKED RIGHT NOW — asked by the fork, forwarded to the member's own answer.
   It is installed on EVERY member's definition rather than only on the two that declare one, for the reason
   every other edge here is: a per-member install is a line to forget on the member that first needs it, and
   the forwarding already answers NULL for a member that declares nothing.
   The machine this pool builds owns nothing a fork cannot copy — its argument vector, its cursors and its
   queues are all declared — so the only possible answer is the member body's. */
static const char *js_idl_args_unforkable(const void *st)
{
    const JSIdlArgsState *s = st;
    const IdlMember *m;

    DCHECK(s->hdr.arg >= 0 && s->hdr.arg < g_n,
           "an IDL member was asked whether it may fork with no pool entry behind it");
    m = idl_member(s->hdr.arg);
    if (!m->step || !m->step->unforkable) return NULL;
    return m->step->unforkable(idl_body_state_const(m, st));
}

static void idl_free_vec(JSContext *ctx, JSValue *vec, int n)
{
    int k;
    if (!vec) return;
    for (k = 0; k < n; k++) JS_FreeValue(ctx, vec[k]);
    js_free(ctx, vec);
}

/* A MEMBER THAT MUTATED THE TREE AND THEN THREW. Every mutating member in this engine validates BEFORE it
   touches the tree — insertBefore's hierarchy check, replaceChild's NotFoundError, insertAdjacentHTML's
   position — so this cannot happen today, and it is asserted rather than assumed because the two ways out of it
   are both wrong. Dropping the records diverges from the spec, which ran the steps for whatever was already
   inserted. Draining them here would have to hold the pending exception live across every yield of the walk.
   The right answer is the third one: a member that needs to mutate and then throw splits into stages so the
   drain happens between them, and this names that requirement at the moment it is first needed. */
#define IDL_TREE_THREW \
    "a member threw after mutating the tree — its insertion steps have nowhere to run: the spec ran them for " \
    "whatever was already inserted, and draining them here would hold the exception live across the walk. " \
    "Split the member so the mutation and the throw are different stages"

/* Take whatever the mutation chokepoints recorded while the body ran. Called at EVERY boundary a body returns
   through, so a record cannot outlive the member that caused it. */
static void idl_tree_take(JSContext *ctx, JSIdlArgsState *s)
{
    if (!g_tree || s->tree) return;
    s->tree = g_tree->take(ctx);
}

/* THE DRAIN, one node per entry. It FORWARDS whatever the walk answers with — JS_STEP_YIELD when it has more
   work, and JS_STEP_FORK the same way, because a per-node effect can ask a question whose answer is unknown
   external state (HTML §6.6.7's insertion steps run §6.6.6's allow focus steps, whose second clause is §6.4.1's
   transient activation). Both codes mean the same thing to this machine: return it, and the re-entry above
   comes straight back here. 0 is the walk finished, and the buffer is released on that edge and nowhere else.
   AND A REQUEST THAT PARKS ON THE PAGE'S CODE IS FORWARDED TOO, which is a correction and not a widening: the
   reason that stood here for refusing one — "that code would run between two nodes' insertion steps, which is
   not the order §4.2.3 states" — is TRUE of DOM §4.2.3 "Mutation algorithms"'s insertion steps and FALSE of
   its POST-CONNECTION steps, whose whole purpose is the JavaScript the insertion steps may not run (insert
   step 10's own note: "the post-connection steps can modify the tree's structure, making live traversal
   unsafe"). Stated as one blanket refusal it was the standing instruction that HTML §4.12.1.1 "Processing
   model" step 36's "immediately execute the script element el" could not be performed on this walk at all,
   which is why it never was. WHICH PHASE MAY MAKE A REQUEST IS THE DOM LAYER'S INVARIANT and is asserted at
   the walk that knows the phase; this file must not know what a Node is, so it forwards and asserts only that
   the code is one the step contract defines. */
static int idl_tree_drain(JSContext *ctx, JSIdlArgsState *s, JSValue in, JSValue **out_cb, int *out_argc)
{
    int r;

    if (!s->tree) { JS_FreeValue(ctx, in); return 0; }
    r = g_tree->step(ctx, s->tree, &s->hdr, in, out_cb, out_argc);
    if (r) {
        /* THREE CODES AND NOT THE WHOLE CONTRACT, because a set is a claim about what the walk is licensed to
           do and admitting a code nothing produces is a licence granted in advance. JS_STEP_CALL is the one
           addition and it is exactly what §4.2.3 insert step 12 requires; a throw is NOT among them, because
           HTML §8.1.4.4 "Calling scripts"'s run a classic script REPORTS its exception rather than propagating
           it, so a post-connection step that runs the page's code owns that completion and never hands it
           back through here to abort the member that caused the mutation. */
        DCHECK(r == JS_STEP_YIELD || r == JS_STEP_FORK || r == JS_STEP_CALL,
               "§4.2.3's tree-steps walk answered with a step code this drain cannot forward — it may rest "
               "(JS_STEP_YIELD), fork (JS_STEP_FORK) or park on the page's own code (JS_STEP_CALL, which "
               "§4.2.3's POST-CONNECTION steps license and its insertion steps do not), and any other code is "
               "one this walk has no licence for rather than one this file declines to carry");
        return r;
    }
    g_tree->release(ctx, s->tree);
    s->tree = NULL;
    return 0;
}

/* HTML §4.13.6 STEPS 3-4 — the `[CEReactions]` EPILOGUE, and EVERY member ends through it. The queue this
   member pushed at step 1 is popped and its reactions run before the member's value is returned, which is the
   whole of the ordering the spec's wrapper exists to give: `document.body.appendChild(el)` runs el's
   connectedCallback before the next statement, not at the next microtask.
   The stage moves to the epilogue's own label, appended to this member's list at declaration — so a flow
   parked on a lifecycle callback reports §4.13.6 step 4 and not whichever of the member's own steps it
   happened to finish at. */
/* THE SAME POP WITH NO INVOKE, for the throw path and for a flow dropped mid-member. §4.13.6 steps 4-5 run the
   reactions and THEN rethrow, which would mean holding the pending exception live across every park of the
   invoke — the same requirement IDL_TREE_THREW names and refuses for the tree steps, and the same answer: a
   member that must both enqueue and throw splits its stages so the invoke happens between them. Every member
   in this engine validates before it mutates, so the popped queue is empty, and custom_elements asserts it. */
static int idl_ce_finish(JSContext *ctx, JSIdlArgsState *s, JSValue in, JSValue **out_cb, int *out_argc)
{
    const IdlMember *m = idl_member(s->hdr.arg);
    int r;

    if (!s->ce_after_body) s->ce_after_body = 1;
    r = custom_elements_reactions_invoke(ctx, &s->ce, in, out_cb, out_argc);
    /* THE STAGE IS THE ARM THE DRAIN IS IN, set on every pass — a park inside a lifecycle callback, inside
       §4.13.5's Construct and inside the `error` event are three different resume points and the stage says
       which. Set unconditionally rather than once on entry, because the arm changes between reactions. */
    if (m->steps) s->hdr.stage = IDL_STEP_FIRST + m->nsteps + custom_elements_queue_arm(&s->ce);
    if (r) return r;
    if (s->ce_threw) {          /* §4.13.6 step 5: rethrow, now that step 4's reactions have run */
        s->ce_threw = 0;
        JS_Throw(ctx, s->ce_exc);
        s->ce_exc = JS_UNDEFINED;
        return JS_STEP_ABRUPT;
    }
    return JS_STEP_DONE;
}

/* THE SLOWEST SINGLE STEP of any IDL member this scheduler-step ran, because a step machine's whole contract
   is that ONE step is short. The engine's seam assertion can say a flow went five seconds without offering a
   suspend point; it cannot say what the flow was inside, and a call point is now offered before every call, so
   a gap that survives is by elimination INSIDE one native call that never returned. Every declared Web API
   member passes through this one function, so this is where such a call names itself — and if the answer comes
   back small, the culprit is not an IDL member and that is information too.
   Dev-only: two clock reads per member step is not a cost a release build should carry to answer a question
   only a development assertion asks. */
#if APICLIENT_DEV
static int64_t     g_slow_ms;
static const char *g_slow_name;
/* The MAX alone cannot tell one five-second call apart from a hundred thousand short ones, and those are
   different bugs with different fixes. The count and the total say which. */
static int64_t     g_step_total;
static long        g_step_count;
static int64_t idl_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
void idl_slowest_reset(void) { g_slow_ms = 0; g_slow_name = NULL; g_step_total = 0; g_step_count = 0; }
int64_t idl_slowest_step(const char **name) {
    if (name) *name = g_slow_name ? g_slow_name : "(none)";
    return g_slow_ms;
}
int64_t idl_step_total(long *count) { if (count) *count = g_step_count; return g_step_total; }
#else
void idl_slowest_reset(void) { }
int64_t idl_slowest_step(const char **name) { if (name) *name = "(release)"; return 0; }
int64_t idl_step_total(long *count) { if (count) *count = 0; return 0; }
#endif

static int js_idl_args_step_inner(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc);

static int js_idl_args_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSIdlArgsState *s = st;
    int rr;
#if APICLIENT_DEV
    int64_t t0 = idl_now_ms();
#endif
    /* HTML §4.13.6 STEP 1, AND STEP 3 ON THE WAY OUT. The element queue this member enqueues its reactions onto
       is CURRENT for exactly as long as the member's own steps are running, and this C call IS that: a member
       parks by RETURNING, so nothing else in the program can run between these two lines. That is why there is
       no stack array — §4.13.6's reactions stack models the nesting of `[CEReactions]` invocations, and with
       the trampoline that nesting is one frame deep by construction. A DOM mutation reached from page code the
       member called is inside ANOTHER declared member, which pushes its own queue here and invokes it at its
       own boundary, which is what the spec's stack says too. A mutation reached from no member at all (the
       parser) finds no current queue and takes the backup arm — also what the spec says. */
    custom_elements_reactions_push(&s->ce);
    rr = js_idl_args_step_inner(ctx, st, cb_result, out_cb, out_argc);
    custom_elements_reactions_pop();
    /* WEB IDL §3.7.7's CREATE AN OPERATION FUNCTION, ITS LAST TWO STEPS — and they cover EVERYTHING above,
     * which is the whole reason they are asked here and not inside any member.
     *
     *   "Let steps be the following series of steps … : Try running the following steps: … If jsValue does not
     *    implement the interface target, throw a TypeError … Let <operation, values> be the result of passing S
     *    and args to the overload resolution algorithm … set R to the result of running the method steps …
     *    And then, if an exception E was thrown: If op has a return type that is a promise type, then return
     *    ! Call(%Promise.reject%, %Promise%, «E»). Otherwise, end these steps and allow the exception to
     *    propagate."
     *
     * THE `Try` OPENS BEFORE THE BRAND CHECK AND CLOSES AFTER THE METHOD STEPS, so a promise-returning
     * operation NEVER throws synchronously: not for a wrong receiver, not for too few arguments, not for an
     * argument whose conversion failed, and not for its own algorithm. `crypto.subtle.digest('SHA-256', {})`
     * rejects; it does not throw, and a bundle that wrote `.catch` around it and nothing else is relying on
     * exactly that.
     *
     * IT REPLACES A PER-MEMBER WORKAROUND AND THAT IS WHY IT IS WORTH A MECHANISM. Without it the only way to
     * reject rather than throw was for the member to declare types that CANNOT fail — `IDL_ANY` plus
     * `idl_optional_from(0)` — and then re-derive the argument's type in its own body, which is the
     * hand-written brand test this file's type list exists to have exactly one of. Every promise-returning
     * member added since would have had to remember the trick, and the ones that did not would throw where the
     * platform rejects.
     *
     * THE PROMISE IS A NEW ONE, NOT THE MEMBER'S OWN CAPABILITY. §3.7.7 says %Promise.reject%, so a member
     * whose algorithm already created a capability and then threw hands back a DIFFERENT, rejected promise and
     * abandons its own — which is what the spec describes and is unobservable, since a promise the member
     * never returned has no reactions on it. */
    if (rr == JS_STEP_ABRUPT && s->hdr.arg >= 0 && s->hdr.arg < g_n && idl_member(s->hdr.arg)->returns_promise) {
        JSValue exc = JS_GetException(ctx);
        JSValue funcs[2];
        JSValue promise;

        DCHECK(!JS_IsUninitialized(exc),
               "a member with a promise return type completed abruptly with no live exception — §3.7.7's `And "
               "then, if an exception E was thrown` has no E to reject with, so the abrupt came from somewhere "
               "that did not throw");
        promise = JS_NewPromiseCapability(ctx, funcs);
        CHECK(!JS_IsException(promise),
              "§3.7.7's Promise.reject could not be allocated — a promise-returning operation that answers "
              "with neither a promise nor a throw is a call a page can only hang on");
        if (JS_CallAsFlow(ctx, funcs[1], exc) < 0)
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, funcs[0]);
        JS_FreeValue(ctx, funcs[1]);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, s->result);
        s->result = promise;
        rr = JS_STEP_DONE;
    }
#if APICLIENT_DEV
    {
        int64_t d = idl_now_ms() - t0;
        g_step_total += d;
        g_step_count++;
        if (d > g_slow_ms) {
            g_slow_ms = d;
            g_slow_name = (s->hdr.arg >= 0 && s->hdr.arg < g_n && idl_member(s->hdr.arg)->name)
                        ? idl_member(s->hdr.arg)->name : "(a member installed by neither install path)";
        }
    }
#endif
    return rr;
}

/* WEB IDL §3.2.11's `ByteString` RANGE, over the UTF-8 the engine hands out. A ByteString's code points are
   0x00..0xFF and a value outside that is a TypeError — which is the whole of what makes the type different
   from a DOMString, and what makes `new Response("", {statusText: "\u0100"})` throw. It is here rather than
   in whichever component noticed it first because it is a TYPE's rule: Headers' fill needs the same answer for
   a record key it converts outside this machine. */
bool idl_is_bytestring(const char *utf8, size_t len)
{
    const unsigned char *p = (const unsigned char *)utf8, *end = p + len;
    while (p < end) {
        unsigned c = *p++;
        if (c < 0x80) continue;                       /* 0x00..0x7f: one byte, in range */
        if ((c & 0xe0) == 0xc0) {                     /* two bytes: U+0080..U+07FF */
            unsigned cp;
            if (p >= end) return false;
            cp = ((c & 0x1f) << 6) | (*p & 0x3f);
            p++;
            if (cp > 0xff) return false;              /* U+0100 and up is not a byte */
            continue;
        }
        return false;                                 /* three or more bytes: far past 0xff */
    }
    return true;
}

/* The conversion itself: the string is already made, so this is the range test plus the throw. */
static int idl_bytestring_check(JSContext *ctx, JSValueConst str)
{
    size_t len = 0;
    const char *u = JS_ToCStringLen(ctx, &len, str);
    int ok;
    if (!u) return -1;
    ok = idl_is_bytestring(u, len);
    JS_FreeCString(ctx, u);
    if (!ok) {
        JS_ThrowTypeError(ctx, "a ByteString argument has a code point above U+00FF");
        return -1;
    }
    return 0;
}

/* WEB IDL §3.2.26 Buffer source types' TWO REFUSALS, WHICH EVERY ONE OF ITS FOUR CONVERSIONS STATES AND WHICH
 * THIS ENGINE'S TWO BUFFER-SOURCE POSITIONS BOTH OWE. Each of §3.2.26's algorithms — for ArrayBuffer, for
 * SharedArrayBuffer, for DataView and for the twelve typed arrays — is a brand test followed by these two and
 * then a return, and they are asked of the buffer UNDER the value: of V for a buffer, of
 * V.[[ViewedArrayBuffer]] for a view. The submodule's pair answers both arms, which is why this reads as one
 * question rather than as a walk repeated per arm.
 *   - SHARED: "If the conversion is not to an IDL type associated with the [AllowShared] extended attribute,
 *     and IsSharedArrayBuffer(V.[[ViewedArrayBuffer]]) is true, then throw a TypeError."
 *   - RESIZABLE: "If the conversion is not to an IDL type associated with the [AllowResizable] extended
 *     attribute, and IsFixedLengthArrayBuffer(V.[[ViewedArrayBuffer]]) is false, then throw a TypeError."
 * EACH CONDITION IS THE POSITION'S TO STATE, WHICH IS WHAT §3.2.26 WRITES — "if the conversion is not to an
 * IDL type associated with the […] extended attribute" — so the two flags come from the DECLARATION and never
 * from this function's opinion. §4.1 ArrayBufferView carries no extended attribute and §4.2 BufferSource
 * says in its own note that [AllowShared] "cannot be used with BufferSource as ArrayBuffer does not support
 * it" (§4.3 AllowSharedBufferSource is the typedef for that), so both of those rows pass false and false and
 * both refusals are unconditional at them. A SPECIFIC typed array position states its own pair: Encoding
 * §7.4 Interface TextEncoder declares `[AllowShared] Uint8Array destination`, which switches the SHARED
 * refusal off at that one position and leaves the resizable one standing. Hard-coding either answer here was
 * right only while no member in the platform wrote an attribute, and a rule that is true by the accident of
 * what has been built is the one that goes wrong silently the day something is.
 * THE REFUSAL IS NOT PEDANTRY, IT IS THE MEMORY-SAFETY BOUNDARY THIS CONVERSION EXISTS TO DRAW. A
 * length-tracking view over a resizable buffer reports a byte length that is recomputed at every read, so a
 * component that took its window and then let page code run — a `toString`, a getter, a promise resolution —
 * holds a window that no longer describes the allocation. §3.2.26 answers that by keeping such a view out of
 * every position that has not asked for one, which is why the check belongs HERE and not at each fill site:
 * a fill site can only assert that the window it was handed is still inside the buffer, and an assert that
 * fires is a defect that already reached the algorithm.
 * Returns -1 with a TypeError live, or 0. The value has already passed its position's brand test, which is the
 * order §3.2.26 states and is what lets the predicates below require a buffer source. */
static int idl_buffer_source_refuse(JSContext *ctx, JSValueConst v, const char *type_name,
                                    bool allow_shared, bool allow_resizable)
{
    if (!allow_shared && JS_IsSharedBufferSource(v)) {
        JS_ThrowTypeError(ctx,
                          "§3.2.26 Buffer source types refuses a SharedArrayBuffer to a `%s`: the position "
                          "carries no [AllowShared] extended attribute, and §4.2 BufferSource cannot carry "
                          "one at all — a member that wants a shared buffer declares AllowSharedBufferSource",
                          type_name);
        return -1;
    }
    if (!allow_resizable && !JS_IsFixedLengthBufferSource(v)) {
        JS_ThrowTypeError(ctx,
                          "§3.2.26 Buffer source types refuses a resizable buffer to a `%s`: the position "
                          "carries no §3.3.1 [AllowResizable] extended attribute, so IsFixedLengthArrayBuffer "
                          "of the buffer under it must be true",
                          type_name);
        return -1;
    }
    return 0;
}

/* WEB IDL §3.7 Interfaces' `implementation-check an object`, WHICH IS THE STANDARD'S OWN NAME FOR THIS AND THE
 * WHOLE REASON THE TWO HALVES BELOW ARE ONE FUNCTION. §3.7 defines it as four steps —
 *
 *   "To implementation-check an object jsValue against the interface interface, with the identifier name and
 *    the type type: 1. Let object to ? ToObject(jsValue). 2. If object is a platform object, then perform a
 *    security check, passing: the platform object object, the identifier name, the type type. 3. If object does
 *    not implement interface, then throw a TypeError. 4. Return object."
 *
 * — and adds, in its own note, "This algo is not yet consistently used everywhere." That note is about the
 * SPEC's own sections: §3.7.7 Operations' create an operation function and §3.7.6 Attributes' creating an
 * attribute getter / creating an attribute setter each still spell the same three steps out inline. They are
 * the same three steps, in the same order, so this engine asks them once.
 * (THE GETTER'S NAME IS "creating an attribute getter" AND THIS LINE SAID "create an attribute getter", which
 * is a phrase that occurs NOWHERE in Web IDL — §3.7.6's define the regular attributes says "Let getter be the
 * result of creating an attribute getter given attr, definition, and realm", and the algorithm itself opens
 * "The attribute getter is created as follows". The SETTER has both spellings — the same call site says
 * "creating an attribute setter" and the note beside it says "the algorithm to create an attribute setter" —
 * so only the getter's was wrong. Corrected here, at the site the claim was made, after fetching the spec:
 * a wrong algorithm NAME is the §Browser-half quotation hazard rather than the citation one, because citegen
 * resolves the SECTION and can say nothing about the words beside it.)
 *
 * THE ORDER IS THE POINT AND IT IS OBSERVABLE. In §3.7.7's try-list the security check is step 2.1.2.2 and the
 * brand TypeError is 2.1.2.3, and only afterwards does 2.1.4 compute the effective overload set that 2.1.5 hands
 * to §3.6 Overload resolution algorithm — so BOTH refusals precede §3.6's arity TypeError and every argument
 * conversion. A receiver brand written in a member's BODY runs after all of that, which is what
 * `Iface.prototype.member.call({}, {toString(){ … }})` reads: the page's `toString` runs, and only then does the
 * TypeError arrive. A browser runs none of it. §3.7.6's two algorithms put the same pair at the same place —
 * the getter's try-list at 2.1.2.2 / 2.1.2.3 and the setter at step 4.5.2 / 4.5.4, whose 4.5.3 computes
 * `validThis` — and the setter's `V` is only READ at step 4.2, never converted, until step 4.6.
 *
 * (§3.7.7 step 2 and §3.7.6's attribute-getter step 1 each hold TWO sibling lists — the try-list, and the "And
 * then, if an exception E was thrown" list that both restart at .1 — so the sub-numbers above are the try-list's;
 * the setter's step 4 holds exactly one list, so 4.5.2 is unambiguous.)
 *
 * STEP 1's ToObject IS PERFORMED BY NOT ALLOCATING, and that is a claim about what it can be observed to do
 * rather than a shortcut. ToObject runs no user code — it has no [Symbol.toPrimitive] arm and no getter — so
 * for a non-object receiver its only effect here is to hand steps 2 and 3 a fresh wrapper, which is not a
 * platform object (step 2 returns) and implements no interface (step 3 throws). Testing the primitive directly
 * reaches the identical TypeError with no wrapper minted. It cannot throw either, because §3.7.7 step 2.1.2.1
 * and §3.7.6's 1.1.2.1 / 4.5.1 have already replaced a null or undefined `this` before ToObject could see one —
 * see the second named residual below for what this engine does and does not do about that replacement.
 *
 * Returns 0 when the member may proceed, or -1 with §3.5's "SecurityError" or §3.7's TypeError live. */
static int idl_implementation_check(JSContext *ctx, const IdlMember *m, JSValueConst js_value)
{
    /* §3.7 STEP 2, whose `type` is §3.5 Security's third input — "method", "getter" or "setter" — stated at the
     * member's MINT and not asked at the call: `idl_step_function` mints an operation, `idl_mint_accessor` an
     * attribute's getter or setter, and `idl_step_constructor` something §3.7.7's create an operation function
     * never runs at all, which is why §3.5 has no fourth value.
     *
     * IT IS ASKED HERE BECAUSE HERE IS WHERE EVERY DECLARED MEMBER CONVERGES, and that is the whole mechanism.
     * HTML §7.2.1's own failure mode is SILENCE: a check written per member is a check the next member added
     * does not have, and nothing says so. There is exactly one opening block every declared member passes
     * through, so the member that forgets cannot exist, and the only thing a member states is WHAT IT IS. */
    if (m->sec_kind != IDL_SEC_NONE) {
        static const WindowProxySecurityType SEC[] = {
            WP_SEC_METHOD,   /* IDL_SEC_METHOD */
            WP_SEC_GETTER,   /* IDL_SEC_GETTER */
            WP_SEC_SETTER,   /* IDL_SEC_SETTER */
        };
        DCHECK(m->sec_kind - 1 < (int)(sizeof SEC / sizeof SEC[0]),
               "a pool entry carries a Web IDL §3.5 Security type this machine has no row for — the four "
               "values are declared beside IdlMember and every one of them is set at a mint");
        if (window_proxy_security_check(ctx, js_value, m->name, SEC[m->sec_kind - 1]) < 0) return -1;
    }
    /* §3.7 STEP 3 — "If object does not implement interface, then throw a TypeError."
     *
     * THE MEMBER STATES THE INTERFACE, NEVER THE REMEDY, exactly as it states its argument types: what it
     * accepts as `this` is a fact about the DECLARATION, and the predicate that answers it belongs to whichever
     * component owns the interface (see idl_this_iface).
     *
     * NAMED RESIDUAL — THE MEMBERS THAT HAVE NOT STATED ONE YET. WHAT IS NOT COVERED: a member whose
     * declaration makes no idl_this_iface call is not brand-checked here; its own body still performs the
     * equivalent test, which is where every such test was before this machine existed and is correct except in
     * its ORDER — it runs after §3.6's conversions rather than before them. WHAT THE NEXT DIFF BUILDS: the next
     * component's declarations call idl_this_iface and its bodies' own receiver tests are DELETED in that same
     * diff, exactly as core/indexeddb/idb_object_store.c's sixteen declarations gained the call and the eleven
     * `os_brand` call sites its pool members reached were deleted.
     * HOW ITS ABSENCE SHOWS: for an unconverted member,
     * `Iface.prototype.member.call({}, {toString(){ window.ran = true; return "x"; }})` leaves `window.ran`
     * true and throws afterwards, where a browser throws with `window.ran` still undefined; and its arity
     * refusal answers "N arguments required" where §3.7.7's try-list reaches 2.1.2.3 before 2.1.4 and a browser
     * says the receiver is wrong. */
    if (m->this_is) {
        /* SECOND NAMED RESIDUAL — §3.7.7's step 2.1.2.1 (and §3.7.6's 1.1.2.1 and 4.5.1), "Let jsValue be the
           this value, if it is not null or undefined, or realm's global object otherwise". WHAT IS NOT COVERED:
           this engine does not substitute, so a null or undefined receiver is tested as itself. That is the
           SAME ANSWER for every interface that is not the realm's global — `Iface.prototype.m.call(undefined)`
           throws §3.7 step 3's TypeError either way, and step 2 returns either way because neither `undefined`
           nor this realm's own Window is refused — which is why the code is narrower rather than wrong. WHAT
           THE NEXT DIFF BUILDS: the substitution written onto the state's receiver, before this is called, so
           the substituted object is also §3.7.7 step 2.1.2.4's `idlObject` — the receiver the member's own BODY
           is handed — rather than a value only this predicate sees. HOW ITS ABSENCE SHOWS: the first [Global]
           interface member declared with idl_this_iface makes `const f = window.<member>; f()` throw
           "does not implement interface Window" where every browser runs it. */
        if (!m->this_is(js_value)) {
            JS_ThrowTypeError(ctx,
                              "'%s' called on an object that does not implement interface %s",
                              m->name ? m->name : "(unnamed member)", m->this_iface);
            return -1;
        }
    }
    return 0;
}

static int js_idl_args_step_inner(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSIdlArgsState *s = st;
    const IdlMember *m;
    JSValue *argv_vec = NULL;
    int r;

    DCHECK(s->hdr.arg >= 0 && s->hdr.arg < g_n, "an IDL member's step ran with no pool entry behind it");
    m = idl_member(s->hdr.arg);
    /* EVERY MEMBER IS NAMED, and this is what makes that true rather than hoped. A name is set by the one mint
       (idl_step_function / idl_step_constructor), so an unnamed member is one that was minted by a hand-written
       JS_NewCFunction2 instead — which is invisible until some diagnostic needs to say what the engine was
       inside and can only answer "(none)". Crashing here names the mint site to convert; a diagnostic that
       shrugs does not. */
    DCHECK(m->name != NULL, "an IDL member reached its step with no name — it was minted by a hand-written "
                            "JS_NewCFunction2 instead of idl_step_function/idl_step_constructor");

    /* A RESUME INSIDE THE EPILOGUE goes straight back to it: the member's own steps are finished and its
       arguments are converted, so neither the conversion loop nor the body may run again. */
    if (s->ce_after_body) return idl_ce_finish(ctx, s, cb_result, out_cb, out_argc);

    /* AN ABRUPT DELIVERY BELONGS TO WHICHEVER ALGORITHM DECLARED THAT IT CATCHES. The definition always
       declares catches_abrupt, because §4.13.6's epilogue below catches for every member — so a request that
       threw during an ARGUMENT CONVERSION, or inside a body that did NOT declare it, arrives here as
       JS_EXCEPTION with the throw still live, and this hands it straight back. This is routing and not a
       fallback: it picks which one implementation answers, and the shape it does not route (a conversion, which
       Web IDL propagates) has no other answer to give. Re-entering the conversion loop with it would run the
       page's getter a second time, which is the failure that turns a throwing `toString` into an infinite
       re-ask. */
    /* …AND THE §4.2.3 WALK IS ONE OF THE ALGORITHMS THAT DECLARED IT, WHICH IS WHY `s->tree` IS ASKED FIRST.
       HTML §8.1.4.4 "Calling scripts"'s run a classic script step 8's third bullet REPORTS an abrupt completion
       rather than rethrowing it — §4.12.1.1 "Processing model"'s "execute the script element" invokes it with
       `rethrow errors` false — and that same algorithm's step 6 then restores this document's currentScript. So
       an injected script's throw is the WALK's own value: re-raised here it would tear down the member that
       performed the insertion, `body.appendChild(s)` would throw whatever the injected program threw (a
       completion no browser produces), and §3.1.7's slot would keep the element for the rest of the session
       because the bracket's other half never ran. The drain below owns `in` on both arms, which is the same
       sentence this file already makes about the normal completion. */
    if (JS_IsException(cb_result) && !s->tree && !(m->step && m->step->catches_abrupt)) return JS_STEP_ABRUPT;

    /* THE DRAIN COMES FIRST ON EVERY RE-ENTRY, before the conversion loop or the body, because the steps the
       previous step recorded must finish before anything else this member does.
       AND THE COMPLETION GOES TO IT RATHER THAN INTO THE BIN. It was freed here, which was correct only while
       the walk could ask nothing: a re-entry then carried an answer to a question this member had asked and
       the walk had not, so there was nothing to deliver. Now that a post-connection step may park on the
       page's own code (§4.2.3 insert step 12), the value arriving on a re-entry the WALK caused is that
       program's completion, and freeing it would deliver JS_UNDEFINED to a machine standing on a request. The
       drain owns `in` from here, on both arms. */
    if (s->tree) {
        r = idl_tree_drain(ctx, s, cb_result, out_cb, out_argc);
        if (r) return r;
        if (s->tree_after_body) return idl_ce_finish(ctx, s, JS_UNDEFINED, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
    }

    if (s->hdr.stage == 0) {
        /* A RECORD NOBODY OWNS. Every tree mutation happens inside a declared member's body and is drained
           before that member returns, so anything still waiting here was written by something that is not a
           declared member — a raw JS_CFUNC_DEF that mutates the tree. Its insertion steps would never run: an
           inserted <script> would not execute and a custom element would not upgrade, with nothing to show
           for it.
           THIS COMMENT SAID THAT WAS "the one shape this machine cannot reach", AND THERE WAS A SECOND ONE.
           An HTML §7.5.3 "Loading XML documents" load builds its tree through core/dom/node.h's `insert`, so
           every construct it placed was recorded — and a document LOAD is not a member, so the remedy this
           message states named no site at all for it: there was nothing to declare. It fired on every `.xhtml`
           document, script or no script, where the byte-identical `.html` one passed. The entries then sat in
           the chokepoint's scratch until the page called its first declared member, whose `take` below would
           have run a whole document's insertion and post-connection steps inside an unrelated call — which is
           what a release build, with this DCHECK compiled out, actually did.
           THE LOAD IS RULED OUT AT THE RECORD AND NOT HERE, by core/dom/element.c's tree_steps_can_run: the
           walk runs a node's steps in the NODE'S document's realm, and this engine installs that realm AFTER
           the parse, so a load's node has none and is not recorded. That is a fact about the node, which is
           what lets it survive a load resting between constructs while sibling flows run. So the remedy below
           is once again the only shape that can reach here, and a member is once again the only answer. */
        DCHECK(!g_tree || !g_tree->recorded(),
               "a DOM mutation recorded tree steps outside any declared member — declare that member so it "
               "converges on this machine, which is the only thing that drains them");
        /* WEB IDL §3.7 Interfaces' IMPLEMENTATION-CHECK, WHICH IS THE ONE ALGORITHM §3.7.7 Operations' and
         * §3.7.6 Attributes' opening steps both spell out — the security check and the receiver's brand, in
         * that order. It is asked HERE because here is where every declared member converges: see
         * idl_implementation_check for the algorithm, the order, and the two residuals.
         *
         * AND IT IS BEFORE §3.6 Overload resolution algorithm's ARITY TypeError AND BEFORE EVERY CONVERSION,
         * WHICH IS OBSERVABLE TWICE OVER. §3.7.7's try-list performs both refusals at its step 2.1.2 and only
         * reaches "compute the effective overload set" at 2.1.4 — so `setTimeout.call(crossOriginFrame)` with
         * NO arguments is a "SecurityError" and not "1 argument required, but only 0 present", and
         * `store.put.call({}, v, {valueOf(){…}})` throws before the page's `valueOf` runs rather than after. A
         * page reads both differences.
         *
         * A PROMISE-RETURNING MEMBER REJECTS RATHER THAN THROWS, and that needs no line here: the `Try` this
         * abrupt returns into is closed by js_idl_args_step above, whose last two steps turn any abrupt from a
         * promise-typed operation into %Promise.reject%. That is what a browser does with
         * `fetch.call(crossOriginFrame, "/")`, and it is why this check must be INSIDE the try rather than at
         * whatever minted the function.
         *
         * THE RESIDUAL IS THE PLAIN-C GETTER ON AN INTERFACE PROTOTYPE, AND IT IS NAMED HERE BECAUSE THIS IS
         * THE SITE IT WOULD REACH. idl_install_accessor takes an `IdlGetter` — a raw JS_CFUNC_getter_magic with
         * NO pool entry — so an attribute whose read runs none of the page's code never arrives at this
         * machine, and neither of §3.7's two steps is performed for it: not §3.5's "getter" security check and
         * not step 3's brand.
         * WHAT IS NOT COVERED IS NOW THE PROTOTYPE HALF ONLY. A plain getter installed as an own property of
         * the realm's [Global] object HAS both steps: idl_mint_plain_getter routes it through §3.7.6's own
         * opening steps, because there the TARGET settles the interface (Window is this engine's only [Global]
         * interface) with nothing for the member to state. On a PROTOTYPE the interface is the member's to
         * declare and this machine is where it would be asked, which is what leaves the shape standing.
         * AND EVERY INTERFACE CONVERTED TO idl_this_iface LEAVES EXACTLY ITS ATTRIBUTE GETTERS BEHIND, which is
         * why this names the SHAPE and not a list of them: converting a component deletes its operations' and
         * setters' own brand tests and cannot touch its getters', so each conversion adds members to this
         * residual rather than retiring it. `grep -l idl_this_iface` names the components that have been
         * converted; the brand helper still standing in each of them is what is left.
         * WHAT THE NEXT DIFF BUILDS: a pool entry for a plain getter — `idl_mint_accessor` takes a STEP id and
         * asks the pool for it, so there is no entry to route an `IdlGetter` through and one has to be minted
         * for it, at idl_mint_plain_getter, which is the one place a plain getter is created. THAT CLAUSE USED
         * TO NAME idl_define_accessor AND IT WAS WRONG ABOUT THIS TREE: there were TWO such mints, and the
         * other one — idl_install_replaceable's readonly form — is what installs CSSOM VIEW §4's thirteen
         * Window members and HTML §7.2.2.4's `parent` and `length`, so a diff obeying the clause as written
         * would have left fifteen global attributes unrouted and believed itself finished. The two mints have
         * since been made one; the clause names that one.
         * HOW ITS ABSENCE SHOWS: `Object.getOwnPropertyDescriptor(IDBObjectStore.prototype, "name").get
         * .call({})` throws from the body's own test rather than from this one, and an unconverted prototype
         * attribute answers `desc.get.call(crossOriginWindowProxy)` out of the reading realm instead of
         * throwing "SecurityError". */
        if (idl_implementation_check(ctx, m, s->hdr.this_val) < 0) {
            JS_FreeValue(ctx, cb_result);
            return JS_STEP_ABRUPT;
        }
        /* A NON-VARIADIC member's arguments ARE its declared ones: a position the IDL does not list is not
           part of the member, so there is nothing past `nargs` to convert, to store, or to hand the body. A
           VARIADIC one takes every argument the page passed, however many that is. */
        s->n = m->variadic ? s->hdr.argc
             : (s->hdr.argc < m->nargs ? s->hdr.argc : m->nargs);
        DCHECK(m->variadic || s->n <= m->nargs,
               "a non-variadic member is converting more positions than its declaration lists — the argument "
               "vector in this state's tail is sized from that same `nargs`, so there is nothing behind them");
        {
            /* §3.6 STEP 5: a call with fewer arguments than the member has REQUIRED ones is a TypeError, and
               it is thrown before any conversion runs. `new File()` built a File out of nothing; `new File([])`
               built one with the name "undefined". The count is the same `first_optional` the declaration
               already states, capped at what the IDL lists — a member that never declares an optional position
               requires every argument it declared, which is what the IDL means by writing them. */
            /* A VARIADIC ARGUMENT IS OPTIONAL — that is part of what the tail MEANS, so `el.remove()` and
               `el.append()` require nothing even though the member declares the tail's type. Stated here
               rather than per member: a variadic member that had to remember to say so is a member whose
               forgetting turns into a TypeError the spec does not have. */
            /* AND IT IS THE SURVIVING ENTRY'S COUNT, not the declaration's. §3.6 removes entries at steps 3-4
               and only then, at step 5, counts what the remaining one requires — so a call that reached past a
               length-differing split is measured against the LONGER entry, whose required positions run past
               the split even where the declaration made position 0 optional for the shorter one. */
            int declared = idl_declared_positions(m);
            int first_opt = idl_first_optional(m, s->hdr.argc);
            int required = first_opt < declared ? first_opt : declared;
            if (s->hdr.argc < required) {
                JS_FreeValue(ctx, cb_result);
                JS_ThrowTypeError(ctx, "%d argument%s required, but only %d present",
                                  required, required == 1 ? "" : "s", s->hdr.argc);
                return JS_STEP_ABRUPT;
            }
        }
        /* A DECLARED DICTIONARY POSITION IS CONVERTED EVEN WHEN THE PAGE STOPPED SHORT OF IT, because its
           value is the IDL's `= {}` and not the page's — see idl_type_is_dictionary. Every position between
           is optional (a required one was already a TypeError above) and takes §3.6's absent path, so it
           reaches the body as the `undefined` it would have anyway; what changes is only that the dictionary
           behind them exists. A VARIADIC member may not declare one at all, which the conversion asserts. */
        /* …AND SO IS A POSITION WITH A DECLARED DEFAULT, for exactly the same reason and by §3.6's own step
           16, whose loop runs "While i is less than the number of arguments callable is declared to take"
           and whose 16.1 is "If callable's argument at index i is declared with a default value, then append
           to values that default value". That loop STARTS where the passed arguments ran out, so it is the
           clause about exactly this case — a member the page stopped short of still receives the IDL's value
           at every defaulted position behind it.
           THE SENTENCE QUOTED HERE BEFORE — "if the argument is optional and it has a default value, set the
           value to that default" — APPEARS NOWHERE IN WEB IDL, and it was attributed to a step 14.2 that is
           really "Let T be the type at index i in the type list of the remaining entry in S". A fabricated
           quotation is the one citation error that tells the reader NOT to open the spec, which is why it
           outlived every number beside it.
           IT WAS THE DICTIONARY CLAUSE ALONE, AND THE ONE EXISTING USER WAS SAVED BY ACCIDENT. IndexedDB
           §4.4's `transaction(storeNames, optional IDBTransactionMode mode = "readonly", optional
           IDBTransactionOptions options = {})` declares a default at position 1, and `db.transaction(["s"])`
           reached the body with argv[1] undefined — except that position 2 is a dictionary, so the clause
           above extended the count past 1 and the default was placed anyway. The body's own DCHECK
           (idl_number_of's, which says an `undefined` at a numeric position means no default was DECLARED)
           states the rule this now keeps: a
           member whose defaulted position has no dictionary behind it — Console §1.4.1's
           `time(optional DOMString label = "default")` — would have reached its body with nothing there. */
        if (!m->variadic) {
            for (r = s->n; r < m->nargs; r++)
                if (idl_type_is_dictionary(m->types[r]))
                    s->n = r + 1;
        }
        /* THE DEFAULT CLAUSE APPLIES TO A VARIADIC MEMBER TOO, and that is not symmetry for its own sake — a
           variadic member's DECLARED positions are the ones BEFORE the tail, and §3.6 step 16.1 reads a
           default for each of them exactly as it does for a member with no tail — it is 16.2, the "missing"
           arm, that asks "if callable's argument at index i is not variadic" and so stops at the tail.
           Console §1.4.2's
           `timeLog(optional DOMString label = "default", any... data)` is one: `console.timeLog()` passes
           nothing, so the count above is 0, and without this the body would be handed an EMPTY argument vector
           for a member whose IDL guarantees a label at position 0. The dictionary clause stays where it is
           because a variadic member may not declare one at all — the conversion asserts that a few lines on. */
        for (r = s->n; r < idl_declared_positions(m); r++)
            if (m->arg_dflts && m->arg_dflts[r].kind != IDL_DEFAULT_NONE)
                s->n = r + 1;
        s->result = JS_UNDEFINED;
        s->conv = m->variadic ? JS_NewArray(ctx) : JS_UNDEFINED;
        s->vstage = JS_UNDEFINED;
        for (r = 0; r < m->nargs; r++)
            idl_args_vec(s)[r] = JS_UNDEFINED;
        s->i = 0;
        s->tree = NULL;
        s->tree_after_body = 0;
        /* §4.13.6 STEP 1, and it is here rather than after the conversions because the conversions are the
           page's code: a `toString` that appends an element must have its reaction run at THIS member's
           boundary, not at whatever outer one happens to be on the stack. */
        custom_elements_queue_init(&s->ce);
        s->ce_after_body = 0;
        s->ce_threw = 0;
        s->ce_exc = JS_UNDEFINED;
        s->ace_ctor = JS_UNDEFINED;
        s->hdr.stage = 1;
    }

    while (s->i < s->n) {
        JSValueConst a = step_arg(&s->hdr, s->i);
        /* ONE STORE PER ARGUMENT, at the bottom of the loop. Every branch below writes the converted value
           into `slot` and falls through to `placed`, so the variadic append happens in exactly one place and
           cannot be forgotten by whichever branch is added next. */
        JSValue *slot = m->variadic ? &s->vstage : idl_arg_slot(m, s, s->i);
        /* A POSITION THE IDL DOES NOT LIST IS NOT CONVERTED. Repeating the last declared type instead was a
           catch-all with a real victim: addEventListener declares one DOMString, so the repeat converted its
           CALLBACK to a string and every listener registered was the string "function () {…}". A variadic
           member's tail is `any...` in every case here, which is exactly what not-listed already means. */
        /* THE DECLARED POSITION THIS ARGUMENT TAKES ITS TYPE FROM, and then the type — derived ONCE, because
           everything the declaration states PER POSITION is indexed by it and a second derivation is a second
           answer. A variadic tail REPEATS the member's last declared type, so what the declaration says about
           that position (its type, and for an IDL_TYPED_ARRAY the §3.2.26 step 1 `T` stated beside it) it says
           for every argument past the tail as well. -1 is a position past a NON-variadic member's declared
           arity: not part of the member at all, so nothing is converted and nothing is stated about it. */
        int ti = (s->i < m->nargs) ? s->i : (m->variadic ? m->nargs - 1 : -1);
        IdlArgType t = ti >= 0 ? m->types[ti] : IDL_ANY;
        /* §3.6 STEPS 3-4, WHICH RUN BEFORE ANY VALUE IS LOOKED AT: argcount is min(maxarg, args) and every
           entry of the effective overload set whose type list is not that long is REMOVED. The shape of that
           this platform declares is a position where one of two overloads ENDS (idl_type_is_length_split) — so
           a call reaching past that position has already deleted the shorter entry, and what is left is the
           longer one's own type at that index and the longer one's own OPTIONALITY at every index.
           BOTH HALVES ARE THE SAME FACT ABOUT THE CALL, which is why they are computed together and from
           `argc` rather than from `s->i`: the removal happens once, before the loop starts, and every position
           the loop then converts belongs to the entry that survived it. Written as a per-position test of one
           type it could only ever rewrite the split position itself, and the positions AFTER the split went on
           reading the shorter entry's optional index — see idl_overload_split_optional_from. */
        bool longer_survived = idl_split_longer_survived(m, s->hdr.argc);
        /* THE TYPE HALF IS ASKED ONLY WHERE THE TWO ENTRIES' TYPE LISTS DIFFER AT THE SPLIT, which is what the
           union type at that position IS. The two facts were one test while the only splits in the platform
           carried both — see idl_overload_length_split_at — and reading them as one would send a split
           declared by ARITY into idl_split_longer_type with a plain type it has no other arm for. The
           OPTIONALITY half below is unconditional, because step 15.3 reads it off the surviving entry however
           that entry was chosen. */
        bool step4_only_longer = longer_survived && s->i == m->split_at &&
                                 idl_type_is_length_split(m->types[m->split_at]);
        int  first_opt = idl_first_optional(m, s->hdr.argc);

        if (step4_only_longer) t = idl_split_longer_type(t);

        /* §3.6: an optional argument given `undefined` is ABSENT, so nothing is converted and the body sees
           undefined — which is what lets it tell "no base" from the base "undefined". A VARIADIC TAIL is not
           one of those positions (see idl_declared_positions): every value passed to a `T...` is converted.
           OPTIONALITY IS THE SURVIVING ENTRY'S, NOT THE DECLARATION'S. §3.6 step 15.3 reads it "at index i in
           the list of optionality values of the REMAINING entry", and the entry that made this position optional is
           exactly the one step 4 just removed — so `postMessage(m, undefined, [p])` converts the string
           "undefined" rather than treating the target origin as absent. */
        /* …AND IT IS NOT ABOUT A DICTIONARY. `store.createObjectStore('s', undefined)` and
           `store.createObjectStore('s')` are the same call, and both are `options = {}` with every member at
           its declared default — so a dictionary position falls through to the conversion below, which is
           what §3.2.17 says converting `undefined` to a dictionary produces. */
        if (!step4_only_longer && !idl_type_is_dictionary(t) &&
            s->i < idl_declared_positions(m) && s->i >= first_opt && JS_IsUndefined(a)) {
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            /* §3.6 STEP 15.4, BOTH ARMS — the guard reached here is that one, "If optionality is 'optional'
               and V is undefined". Its 15.4.1 gives an argument whose IDL writes `= …` THAT value, which is
               already an IDL value and is therefore placed rather than coerced; its 15.4.2 appends "the
               special value 'missing'" for a position with no declared default, which is the undefined the
               body reads as "not given". Step 16.1 is the same placement for a position the page never
               reached — see idl_args.h's idl_arg_default. */
            *slot = (m->arg_dflts != NULL && m->arg_dflts[s->i].kind != IDL_DEFAULT_NONE)
                  ? idl_default_of(ctx, m->arg_dflts[s->i].kind, m->arg_dflts[s->i].str)
                  : JS_UNDEFINED;
            if (JS_IsException(*slot)) { *slot = JS_UNDEFINED; return JS_STEP_ABRUPT; }
            goto placed;
        }

        /* §3.2.20's NULLABLE RULE over the two interface-valued types: null AND undefined are the IDL null, and
           what survives takes the un-nullable type's own conversion — which is why this collapses the TYPE
           here rather than adding a null arm to each of those conversions below. It is resolved BEFORE the
           unknown-external-input pass-through, so a concolic still reaches §3.2.15's brand test and is refused
           there: a value that is not a platform object is a TypeError however it was declared. */
        if (t == IDL_INTERFACE_NULLABLE || t == IDL_SEQUENCE_INTERFACE_NULLABLE) {
            if (JS_IsNull(a) || JS_IsUndefined(a)) {
                JS_FreeValue(ctx, cb_result);
                cb_result = JS_UNDEFINED;
                *slot = JS_NULL;
                goto placed;
            }
            t = (t == IDL_INTERFACE_NULLABLE) ? IDL_INTERFACE : IDL_SEQUENCE_INTERFACE;
        }

        /* UNKNOWN EXTERNAL INPUT CROSSES AS ITSELF, whatever the declared type says.
           An IDL conversion is a BOUNDARY, not an ECMAScript operator: nothing observes its result except the
           component behind it, and every one of those bodies already asks explicitly for what it needs from a
           concolic (concolic_shape_c for the bytes a Text node carries, the attribute taint shadow for a value
           parked in the DOM). Converting here would do the one thing that must never happen — hand ToString a
           concolic, which the C boundary asserts against because opacity has to SURVIVE a coercion or the value
           stops forking control flow and stops being solvable at a sink. This is the same answer JSON.stringify
           gives an opaque field: yield the opaque itself, never a de-tainting placeholder.
           WHICH TYPES THOSE ARE IS NOT SPELLED OUT HERE ANY MORE, and that is the whole of this hunk: the list
           was written twice, as this condition's `t != …` chain and as the assert below, and the two disagreed
           about three types for as long as both existed. idl_args.h states it ONCE and both read it — a
           dictionary and an interface brand ask the value nothing this pass-through has to answer for (see
           IDL_CONCOLIC_UNASKED), and a union whose ARM is a test of the value is neither crossed nor picked
           but FORKED at its own resolution site (IDL_CONCOLIC_FORKS). */
        if (idl_concolic_rule(t) == IDL_CONCOLIC_CROSSES && concolic_is(a)) {
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            *slot = JS_DupValue(ctx, a);
            goto placed;
        }

        /* §3.2.25 Union types OVER THE THREE UNIONS WHOSE ARM IS A RUNTIME TYPE TEST ON THE VALUE ITSELF, and
           they are HERE — below the pass-through — for the reason every other union in this function is, which
           they were the only three not to obey. The rule this whole block states is that unknown external input
           reaches no coercion and answers no question about what it IS; §3.2.25 decides a union arm by asking
           exactly such questions ("If V is an Object …", "If IsCallable(V) is true, then: If types includes a
           callback function type, then return the result of converting V to that callback function type"), and
           asking one of a concolic is a CONTROL-FLOW decision over unknown input, which §@S forbids.
           IT ANSWERED, AND IT ANSWERED THE WRONG WAY. A concolic is an object carrying a [[Call]] —
           solver/concolic.c installs one so `document.cookie.indexOf("role=admin")` yields another unknown
           instead of throwing "not a function" and taking the rest of the program with it — so IsCallable over
           EVERY unknown external input is true, and `(DOMString or Function)` took the callback arm for all of
           them. HTML §8.7 Timers makes the OTHER arm a code-execution sink (its task substeps: "If handler is a
           Function, then invoke handler … Otherwise: … Assert: handler is a string … Let script be the result
           of creating a classic script given handler … Run the classic script script"), so
           `setTimeout(location.hash, 1)` was routed to the callback arm and announced to nobody: measured on
           this artifact, an eval sink over the same source fired a verified PoC and the string timer over it
           produced no @S entry at all. Nothing crashed, because taking a real arm of a real union is not a
           broken invariant — it is the collapse this pass-through exists to prevent, arriving three types
           early.
           WHAT A CONCOLIC DOES NOW is cross as itself, exactly as it does at every other declared type, and the
           BODY decides — which is the only place that can, because which arm of a union unknown input belongs
           to is a fact about the member's algorithm and not about the value.
           §3.2.26 is Buffer source types; the citation on the `(object or DOMString)` clause below said so and
           was wrong. The clause it is about is this one. */
        /* BEFORE THE ARMS AND NOT AFTER THEM, because each `if` below OVERWRITES `t` with the arm it chose:
           asked afterwards this would read IDL_ANY or IDL_DOMSTRING and pass for exactly the value it exists to
           catch. It is not a tautology about the current order — it is that order's invariant, stated where a
           later move of these blocks back above the pass-through, or a `goto` that reaches them around it,
           makes it fire instead of silently answering a union arm for an unknown.
           IT ASKS THE RULE AND NOT `!concolic_is(a)`, AND THE DIFFERENCE IS EVERY MEMBER THAT TAKES AN `any`.
           Written as "no concolic reaches this line at all" it was a claim the pass-through above never made:
           that condition deliberately leaves three types uncrossed, so the assert fired on the FIRST unknown
           handed to an `any` position (Indexed Database §4.5 The IDBObjectStore interface declares
           `IDBRequest put(any value, optional any key)`, so one `store.put(location.hash, 1)` reaches it) —
           and it named a union arm that
           value had not reached and could not reach, since every arm below is a type whose rule is CROSSES.
           A false @WHY is the stale-DFAIL failure mode with an abort behind it: authoritative, reproducible,
           and about a program that was working. IDL_CONCOLIC_FORKS is likewise not caught here, because its
           arm is asked further down at the site that acts on it, where the value it forks over still exists. */
        DCHECK(!concolic_is(a) || idl_concolic_rule(t) != IDL_CONCOLIC_CROSSES,
               "unknown external input reached a §3.2.25 union arm that is decided by a runtime type test — the "
               "pass-through above is what stops that, so a concolic here means the arm is about to be chosen "
               "from what the SOLVER's value class is rather than from what the page's value is, which is how "
               "`(DOMString or Function)` sent every unknown down the callback arm and left HTML §8.7's string "
               "handler with no @S detection at all");
        if (t == IDL_STRING_UNLESS_CALLABLE)
            t = JS_IsFunction(ctx, a) ? IDL_ANY : IDL_DOMSTRING;   /* the union's own rule */
        if (t == IDL_STRING_UNLESS_IFACE) {
            DCHECK(idl_arg_iface_stated(m, ti),
                   "a member declared an interface-or-string union with no interface to brand against — the "
                   "interface is half of what that type states, and it is stated by idl_iface_brand for one a "
                   "class names exactly or by idl_arg_iface for one it does not");
            /* THE ARM IS §3.2.15's OWN TEST, so it is the SAME resolution the throwing arms below make and not
               a bare class comparison: a declaration that narrowed its interface past a class (`(HTMLElement
               or DOMString)`) would otherwise take the object arm for every Node and the narrowing would be a
               statement two arms of one file disagreed about. */
            t = idl_arg_implements(ctx, m, ti, a) ? IDL_ANY : IDL_DOMSTRING;
        }
        /* §3.2.25 over `(object or DOMString)`, read in the union algorithm's own ORDER — which is the whole
           of the difference between this and IDL_STRING_OR_DICT. The union names no nullable and no dictionary
           type, so `null` never reaches an object arm: it falls past every Object clause to the string one and
           becomes the four characters "null". Reading it as "an object is the object, everything else is a
           string" agrees on every ordinary case and is the same sentence; it is written as one clause because
           that is what §3.2.25 leaves once the arms are named. */
        if (t == IDL_STRING_UNLESS_OBJECT)
            t = JS_IsObject(a) ? IDL_ANY : IDL_DOMSTRING;

        /* §3.2.25 over `(DOMString or D)`, and it is resolved AFTER the pass-through above on purpose: a
           concolic IS an object, so asking the union first would send unknown external input down the
           dictionary arm and read members off it. What reaches here is a real value, and then the union's own
           order decides — null and undefined take the dictionary arm (step 4), any Object takes it (step 10),
           and everything else falls through to step 12's string arm. */
        if (t == IDL_STRING_OR_DICT) {
            DCHECK(m->dict_n > 0, "a `(DOMString or D)` argument was declared with no dictionary members — the "
                                  "dictionary is half of what that type states");
            t = (JS_IsObject(a) || JS_IsNull(a) || JS_IsUndefined(a)) ? IDL_DICT : IDL_DOMSTRING;
        }
        /* §3.6 STEP 12 over the two entries step 4 left, and it reaches here only at the arity where BOTH
           survive — the longer one was already taken above. The clause order is the same one the union arm
           above follows and for the same reason: null and undefined name the entry declaring a dictionary at
           this index, so does ANY Object, and every remaining value reaches the string clause. It is likewise
           resolved AFTER the concolic pass-through, since unknown external input IS an object and asking the
           overload first would read `targetOrigin` off an attacker's value. */
        if (t == IDL_USVSTRING_OR_DICT) {
            DCHECK(m->dict_n > 0, "a member declared an overload splitting at a dictionary argument with no "
                                  "dictionary members — the dictionary is half of what that type states");
            t = (JS_IsObject(a) || JS_IsNull(a) || JS_IsUndefined(a)) ? IDL_DICT : IDL_USVSTRING;
        }
        /* §3.6 AT THE ARITY WHERE ONLY THE DICTIONARY ENTRY IS LEFT, and there is no step 12 to run: the row
           above needs one because BOTH its entries are still standing at arity 2, while this one's three
           entries have lengths 0, 1 and 2 and steps 3-4 therefore leave exactly one at every arity (see
           IDL_UNRESTRICTED_DOUBLE_OR_DICT). Reaching here means the longer entry was removed, so the surviving
           entry declares a dictionary at this position and §3.2.17 converts whatever the page passed —
           including `el.scrollTo(0)`, whose 0 is not undefined, null or an Object and is therefore a
           TypeError from the type rather than an x coordinate. */
        if (t == IDL_UNRESTRICTED_DOUBLE_OR_DICT) {
            DCHECK(m->dict_n > 0, "a member declared a number-or-dictionary overload split with no dictionary "
                                  "members — the dictionary is half of what that type states");
            DCHECK(!step4_only_longer,
                   "§3.6 steps 3-4 chose the LONGER entry and the position still carries the split type — the "
                   "rewrite at the top of this loop is what performs that choice, so the two have come apart");
            t = IDL_DICT;
        }

        /* §3.2.25 over `(DOMString or sequence<DOMString>)`, whose arm is decided by a READ of the page's value
           and is therefore the only union here that can park. Step 2 first — the nullable spelling makes null
           and undefined the IDL null before anything is looked at — and then the arm, which for everything but
           an Object with a callable @@iterator is step 16's string. Resolved AFTER the concolic pass-through
           above for the same reason the two unions before it are: unknown external input IS an object, and
           asking it for @@iterator would read a property off an attacker's value. */
        if (t == IDL_DOMSTRING_OR_SEQUENCE || t == IDL_DOMSTRING_OR_SEQUENCE_NULLABLE) {
            if (t == IDL_DOMSTRING_OR_SEQUENCE_NULLABLE && (JS_IsNull(a) || JS_IsUndefined(a))) {
                JS_FreeValue(ctx, cb_result);
                cb_result = JS_UNDEFINED;
                *slot = JS_NULL;
                goto placed;
            }
            r = idl_union_seq_arm(ctx, &s->hdr, &s->dw, &s->dw.uni_phase, a, &cb_result, &t,
                                  IDL_SEQUENCE_DOMSTRING, IDL_DOMSTRING, out_cb, out_argc);
            if (r > 0) return r;   /* parked ON THE ARM's read; the resume finds the arm already chosen */
            if (r < 0) return JS_STEP_ABRUPT;
        }

        /* §3.2.25 over `(double or sequence<double>)` — the same step 11.2 read, the same park, two other arms.
           Resolved AFTER the concolic pass-through above for the reason every union here is: unknown external
           input IS an object, and asking it for @@iterator would read a property off an attacker's value. */
        if (t == IDL_DOUBLE_OR_SEQUENCE) {
            r = idl_union_seq_arm(ctx, &s->hdr, &s->dw, &s->dw.uni_phase, a, &cb_result, &t,
                                  IDL_SEQUENCE_DOUBLE, IDL_DOUBLE, out_cb, out_argc);
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
        }

        /* §3.6 STEP 12 OVER TWO ENTRIES STEP 4 REMOVED NEITHER OF — a `sequence<object>` against an
           `optional StructuredSerializeOptions options = {}` at the same index, which is what
           HTML §9.4.4 Message ports' `MessagePort.postMessage` declares. The row above can decide its split
           on the ARGUMENT COUNT because one of its entries is longer; here both type lists are the same
           length, so nothing is removed before the value is looked at and every outcome is one of step 12's
           clauses. Resolved AFTER the concolic pass-through above for the reason every arm here is: unknown
           external input IS an object, and asking it for @@iterator would read a property off an attacker's
           value. */
        if (t == IDL_SEQUENCE_OBJECT_OR_DICT) {
            DCHECK(m->dict_n > 0, "a member declared a `sequence<object>`-or-dictionary overload split with no "
                                  "dictionary members — the dictionary is half of what that type states");
            if (JS_IsNull(a) || JS_IsUndefined(a)) {
                /* Step 12's FIRST clause ("V is undefined, and there is an entry whose list of optionality
                   values has 'optional' at index i") and its SECOND ("V is null or undefined, and there is an
                   entry that has … a dictionary type") both name the dictionary entry here, so the two land
                   together. §3.2.17 then converts undefined/null to that dictionary's own defaults, which is
                   why this is a type collapse and not a placed IDL null. */
                t = IDL_DICT;
            } else if (!JS_IsObject(a)) {
                /* Step 12's FINAL "Otherwise: throw a TypeError". Neither entry has a string, numeric,
                   boolean, bigint or `any` type at this position, so no clause between the dictionary one and
                   the end of the chain names an entry and there is nothing left to select. This is the one
                   place this type is observably NOT the row above, whose string arm swallows the same values:
                   `port.postMessage(m, "x")` throws, and an implementation that reused the string union here
                   would build a transfer list out of a value the standard refuses. */
                JS_FreeValue(ctx, cb_result);
                JS_ThrowTypeError(ctx, "argument %d is neither a sequence nor a dictionary", s->i + 1);
                return JS_STEP_ABRUPT;
            } else {
                /* Step 12's SEQUENCE clause against its callback-interface/dictionary/record/object clause,
                   and the test between them is `? GetMethod(V, %Symbol.iterator%)` — the page's code, so this
                   position PARKS here exactly as a `(T or sequence<T>)` union does. The phase byte is this
                   member's own (ovl_phase) rather than the union's, because the dictionary arm's member reads
                   run while this answer is still live and they reset the union's. */
                r = idl_union_seq_arm(ctx, &s->hdr, &s->dw, &s->ovl_phase, a, &cb_result, &t,
                                      IDL_SEQUENCE_OBJECT, IDL_DICT, out_cb, out_argc);
                if (r > 0) return r;   /* parked ON THE OVERLOAD's read; the resume finds it already chosen */
                if (r < 0) return JS_STEP_ABRUPT;
            }
        }

        if (t == IDL_DICT || t == IDL_DICT_OR_BOOL_FIRST || t == IDL_BOOL_OR_DICT) {
            /* WHICH ARM OF `(dictionary or boolean)` THIS FLOW IS ON — §3.2.25 step 11 against steps 12/18.
               For a dictionary type there is no union and no arm: undefined, null and an Object all reach
               §3.2.17, and anything else is that section's step 1 TypeError below. */
            bool bool_arm = false;
            /* A DICTIONARY ARGUMENT IS BUILT BY THE WALK and placed in the vector slot when the walk is TAKEN,
               rather than through `slot` and `placed:` — its members are written onto the object across many
               re-entries, so there is nothing to append at the end. That is why a VARIADIC member may not
               declare one at all: its body reads the converted arguments out of `conv`, which this store never
               reaches, and the member would receive an undefined where its dictionary belongs. The old form of
               this assert allowed one at a declared position and only refused it in the tail, which is the half
               of the rule that leaves a hole. */
            DCHECK(!m->variadic,
                   "a VARIADIC member declared a dictionary argument — a dictionary is built by the §3.2.17 "
                   "walk into the argument vector and a variadic member's body reads `conv`, so the two never "
                   "meet");
            /* §3.2.25's ARM FOR `(AddEventListenerOptions or boolean)`, ASKED HERE — at the site that acts on
               it — because this union's two arms ARE the dictionary and the boolean, and there is no third type
               to collapse `t` to in the arm block above.
               For a value the page determined it is the union's own clause order: step 11 "If V is an Object"
               takes the dictionary arm, and everything else falls to step 12 "If V is a Boolean" / step 18
               "If types includes boolean" — which is what DOM §2.7 "Interface EventTarget"'s flatten options
               states as "If options is a boolean, then return options".
               OVER UNKNOWN EXTERNAL INPUT IT IS A FORK AND NOT A TEST, and it was a test. A concolic is an
               object carrying a [[Call]] — solver/concolic.c installs one so a method on an unknown yields
               another unknown instead of throwing "not a function" and taking the rest of the program with it
               — so JS_IsObject is TRUE over EVERY unknown external input, for a reason that is a fact about
               this engine's value class and not about the page's value. Asked here it sent every unknown down
               the dictionary arm and DELETED the other world, which is the collapse `(DOMString or Function)`
               had one type over, arriving in the one union the arm block above never saw.
               THE ARMS DIFFER IN WHAT THE ALGORITHM OBSERVES, which is why neither may be picked: DOM §2.7's
               flatten more options leaves `once` false and `passive` and `signal` NULL on the boolean arm and
               READS all three off the value on the dictionary arm, and a null `passive` is the whole of what
               makes a wheel listener on a Window passive by default. So BOTH arms run.
               OUTCOME 0 IS THE DICTIONARY ARM, per step_fork_run's one rule on the numbering — outcome 0 is
               what a run with no forking policy takes, and an @S candidate re-fire must not be diverted on its
               way to a sink. It is also the arm §3.2.25 gives the object an unknown is represented BY, so a
               no-policy run answers exactly as it did and the boolean world is the one the fork ADDS.
               IT IS ASKED ONLY ON THE ENTRY THAT STARTS THE WALK, and the reason is NOT that a second ask
               would fork twice — step_fork_run is idempotent, and its own contract says a park and a
               cross-session resume both "land back on the ask, which re-derives the same arm from the flow's
               decision vector". It is that the ask RELEASES this machine's outstanding request answer first
               (it takes no `in` to hand it to, and the sibling's snapshot is taken at its return, so nothing of
               the caller's may be live across it). Asked on a RESUME, that release destroys the very answer the
               parked member read is waiting for, and the member would come back holding `undefined` where the
               page's value belongs. `started` is what says the arm is already behind us. */
            if (t != IDL_DICT && !s->dw.started) {
                bool_arm = !JS_IsObject(a);
                if (concolic_is(a)) {
                    int arm = 0, rc;

                    DCHECK(idl_concolic_rule(t) == IDL_CONCOLIC_FORKS,
                           "this conversion forked §3.2.25's arm for a type idl_args.h does not declare as one "
                           "it forks — the SITE and the rule are the two halves of one statement, and a type "
                           "that loses its FORKS rule while this ask stands would fork an arm the pass-through "
                           "above had already crossed the value at");
                    /* `cb_result` is this machine's outstanding answer and step_fork_run takes no `in` to hand
                       it to, so it is released HERE, exactly as the boolean arm below releases it — the
                       sibling's snapshot is taken at this return and nothing of the caller's may be live
                       across it. */
                    JS_FreeValue(ctx, cb_result);
                    cb_result = JS_UNDEFINED;
                    rc = step_fork_run(ctx, &s->hdr, a, "§3.2.25 (dictionary or boolean) arm", 2,
                                       JS_OUTCOME_REAL_UNSTATED, &arm);
                    if (rc) return rc;
                    bool_arm = (arm != 0);
                }
            }
            if (bool_arm) {
                /* §2.7 "flatten": a non-object IS the first member's boolean. NO §3.2.17 CONVERSION RUNS ON
                   THIS ARM AT ALL — §3.2.25 converts V to exactly one of the two types, and on this arm it is
                   the boolean — so the walk is never started and there is no half-built dictionary to discard.
                   The old form minted the object before the arm was known and then abandoned it here, AND ran
                   §3.2.17 step 4.1.5's defaults over a dictionary this arm does not produce. That was a
                   §3.2.17 step reached from outside §3.2.17, and it is gone rather than moved into the walk:
                   what the algorithm reads on this arm is DOM §2.7 "Interface EventTarget"'s flatten more
                   options — "If options is a boolean, then return «[ \"capture\" → options, \"once\" → false,
                   \"passive\" → null, \"signal\" → null ]»" — whose values are the ALGORITHM'S and not the
                   IDL's, and which an absent member already answers (idl_dict_bool reads absent as false).
                   No member in the platform observed the difference: the one IDL_DICT_OR_BOOL_FIRST member
                   declares no default on any of its four, and the IDL_BOOL_OR_DICT arm skipped the loop
                   outright. It would have observed it the day one did.
                   AND THE VALUE THE UNION SAYS *IS* THAT MEMBER CROSSES AS ITSELF WHEN IT IS UNKNOWN. DOM §2.7
                   "Interface EventTarget"'s flatten options is "If options is a boolean, then return options"
                   — the member IS V, so ToBoolean here would be this boundary coercing unknown external input
                   after the fork above decided only that V is on the boolean arm, which is not the same fact
                   as V being TRUE. A concolic object is truthy, so the coercion pinned `capture` for every
                   unknown while destroying the taint that says it is one. It is placed instead, which is what
                   every other type does with unknown input, and the taint reaches the member's readers. */
                JSValue *slot_b = idl_arg_slot(m, s, s->i);
                JSValue flat = concolic_is(a) ? JS_DupValue(ctx, a) : JS_NewBool(ctx, JS_ToBool(ctx, a));

                JS_FreeValue(ctx, cb_result);
                cb_result = JS_UNDEFINED;
                JS_FreeValue(ctx, *slot_b);
                if (t == IDL_BOOL_OR_DICT) {
                    /* §3.2.25's OTHER DESTINATION FOR THE SAME ARM — the boolean ITSELF, where the member's own
                       algorithm reads it (CSSOM VIEW §6's `scrollIntoView` step 6) rather than a dictionary
                       member the IDL never declared. */
                    *slot_b = flat;
                } else {
                    DCHECK(m->dict_n > 0 && m->dict[0].type == IDL_BOOLEAN,
                           "a (dictionary or boolean) union declared a non-boolean first member — the union's "
                           "rule is that the bare value IS that member");
                    *slot_b = JS_NewObject(ctx);
                    if (JS_IsException(*slot_b)) { *slot_b = JS_UNDEFINED; JS_FreeValue(ctx, flat);
                                                   return JS_STEP_ABRUPT; }
                    JS_SetPropertyStr(ctx, *slot_b, m->dict[0].name, flat);
                }
                s->ovl_phase = IDL_UNI_ASK;
                s->i++;
                continue;
            }
            /* §3.2.17 (ES-to-IDL list) STEP 1: a value that is NOT undefined, null or an Object is a TypeError
               before any member is read — `new Blob([], 123)` throws, and reading `123.type` instead answered
               undefined and built a Blob. It is performed HERE, at the position, because this is where what to
               say about the value is known; the walk asserts it rather than repeating it. The union forms never
               reach this line — a non-object took the boolean arm above. */
            if (!s->dw.started && !JS_IsObject(a) && !JS_IsUndefined(a) && !JS_IsNull(a)) {
                JS_FreeValue(ctx, cb_result);
                JS_ThrowTypeError(ctx, "the dictionary argument is neither an object, null nor undefined");
                return JS_STEP_ABRUPT;
            }
            /* AND THEN IT IS THE ONE WALK, DRIVEN EXACTLY AS AN ALGORITHM DRIVES IT. `optional D options = {}`
               with undefined or null passed is not a special case here: §3.2.17 step 4.1.2 makes every member's
               value undefined and the walk's own loop defaults them, which is why the second "default them all"
               pass that used to stand here is gone rather than duplicated into the walk. */
            if (!s->dw.started &&
                idl_dict_walk_start(ctx, &s->dw, a, m->dict, m->dict_n, m->dict_atoms, /*name*/ NULL,
                                    m->iface, m->iface_narrow, idl_frames(m, s), m->conv_depth) < 0) {
                JS_FreeValue(ctx, cb_result);
                return JS_STEP_ABRUPT;
            }
            r = idl_dict_walk_run(ctx, &s->hdr, &s->dw, idl_frames(m, s), m->conv_depth, cb_result,
                                  out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r > 0) return r;    /* parked ON A MEMBER's read or its own coercion; the resume comes back */
            if (r < 0) return JS_STEP_ABRUPT;
            JS_FreeValue(ctx, *idl_arg_slot(m, s, s->i));
            *idl_arg_slot(m, s, s->i) = idl_dict_walk_take(ctx, &s->dw);
            /* THE OVERLOAD ARM IS PER POSITION and this branch does not pass through `placed:`, so the reset
               is stated here too — a dictionary reached AS the flat arm of §3.6 step 12 leaves the arm chosen,
               and the next declared position must ask its own value rather than inherit this answer. */
            s->ovl_phase = IDL_UNI_ASK;
            s->i++;
            continue;
        }

        /* §3.2.21's `sequence<T>`: the ES ITERATOR PROTOCOL, and a value that is not an Object is a TypeError
           BEFORE anything is read — `new Blob("fail")` throws even though a string is iterable, because the
           check is on the TYPE and not on iterability, and `new Blob(null)` throws for the same reason.
           IT IS CONVERTED HERE AND NOT IN THE BODY, which is the whole point of it being a declared type: Web
           IDL converts arguments LEFT TO RIGHT, so a sequence that throws mid-iteration must run before the
           dictionary after it is read at all. Driven from the body it ran after every other argument, and
           `new Blob(throwingIterable, {get type(){…}})` called the type getter the spec never reaches. */
        /* IDL_SEQUENCE_DOMSTRING IS ONE OF THEM AT AN ARGUMENT POSITION, and it reaches this loop by two
           roads: declared outright, and as the arm §3.2.25 resolved a `(DOMString or sequence<DOMString>)` to
           — in which case the cursor was already planted on the method the union read and `seq_phase` is
           already 1, so the setup below is skipped and nothing reads @@iterator twice. IDL_SEQUENCE_OBJECT has
           a THIRD road for the same reason and with the same cursor already planted: §3.6 step 12's sequence
           clause, whose step 14 says to create the sequence "from V and method" — the method that clause
           already obtained, never a second read. */
        if (t == IDL_SEQUENCE_BLOBPART || t == IDL_SEQUENCE_INTERFACE || t == IDL_SEQUENCE_OBJECT ||
            t == IDL_SEQUENCE_DOMSTRING || t == IDL_SEQUENCE_DOUBLE || t == IDL_SEQUENCE_ENUM) {
            if (!JS_IsObject(a)) {
                JS_FreeValue(ctx, cb_result);
                JS_ThrowTypeError(ctx, "the sequence argument is not an object");
                return JS_STEP_ABRUPT;
            }
            if (s->dw.seq_phase == 0) {
                s->dw.seq_list = JS_NewArray(ctx);
                if (JS_IsException(s->dw.seq_list)) return JS_STEP_ABRUPT;
                iter_cursor_init(&s->dw.seq);
                s->dw.seq_phase = 1;
            }
            for (;;) {
                if (s->dw.seq_phase == 1) {
                    r = iter_cursor_run(ctx, &s->hdr, &s->dw.seq, a, cb_result, out_cb, out_argc);
                    cb_result = JS_UNDEFINED;
                    if (r > 0) return r;   /* parked ON THIS ELEMENT; the resume comes back to it */
                    if (r < 0) return JS_STEP_ABRUPT;
                    if (s->dw.seq.done) break;
                    /* §3.2.13's `object`: an Object crosses as ITSELF and anything else is a TypeError. Like
                       the brand test below it runs none of the page's code, so the cursor's next pull follows
                       it in the same step. */
                    if (t == IDL_SEQUENCE_OBJECT) {
                        if (!JS_IsObject(s->dw.seq.value)) {
                            JS_ThrowTypeError(ctx, "an element of argument %d is not an object", s->i + 1);
                            return JS_STEP_ABRUPT;
                        }
                        JS_SetPropertyUint32(ctx, s->dw.seq_list, s->dw.seq_n++, JS_DupValue(ctx, s->dw.seq.value));
                        continue;
                    }
                    /* §3.2.15's ELEMENT CONVERSION — the brand test, which runs none of the page's code, so
                       the cursor's next pull follows it in the same step. */
                    if (t == IDL_SEQUENCE_INTERFACE) {
                        DCHECK(idl_arg_iface_stated(m, ti),
                               "an interface-sequence argument was declared with no interface to brand against "
                               "— idl_iface_brand (a class) or idl_arg_iface (a predicate and the identifier) "
                               "is the other half of that type");
                        if (!idl_arg_implements(ctx, m, ti, s->dw.seq.value)) {
                            JS_ThrowTypeError(ctx, "an element of argument %d does not implement %s",
                                              s->i + 1, idl_arg_iface_subject(m, ti));
                            return JS_STEP_ABRUPT;
                        }
                        JS_SetPropertyUint32(ctx, s->dw.seq_list, s->dw.seq_n++, JS_DupValue(ctx, s->dw.seq.value));
                        continue;
                    }
                    /* `BlobPart` is `(BufferSource or Blob or USVString)`, and its rule is a BRAND test: a
                       BufferSource and a Blob cross as themselves, everything else takes the USVString arm,
                       whose ToString is the page's code. Stated once, here, like BodyInit's.
                       IT IS THE BLOBPART TYPE'S AND NOT THE LOOP'S: a `sequence<DOMString>` holding a Blob has
                       one element type and it is a string, so that element ToStrings to "[object Blob]" rather
                       than crossing as an interface the declaration never named. */
                    if (t == IDL_SEQUENCE_BLOBPART && blob_is(s->dw.seq.value)) {
                        JS_SetPropertyUint32(ctx, s->dw.seq_list, s->dw.seq_n++, JS_DupValue(ctx, s->dw.seq.value));
                        continue;
                    }
                    /* THE BUFFERSOURCE ARM IS SPLIT OUT FROM THE BLOB ARM BECAUSE §3.2.25 DOES NOT HAND BACK
                       THE OBJECT HERE — its buffer clauses say "return the result of CONVERTING V to
                       ArrayBuffer" and "…to that type", so §3.2.26 Buffer source types' own conversion runs
                       on this element, refusals included. A Blob is not a buffer source and must not be asked
                       either question. */
                    if (t == IDL_SEQUENCE_BLOBPART &&
                        (JS_IsArrayBuffer(s->dw.seq.value) || JS_GetTypedArrayType(s->dw.seq.value) >= 0 ||
                         JS_IsDataView(s->dw.seq.value))) {
                        if (idl_buffer_source_refuse(ctx, s->dw.seq.value, "BlobPart",
                                                     /* §4.2 BufferSource carries neither §3.3 attribute */
                                                     false, false))
                            return JS_STEP_ABRUPT;
                        JS_SetPropertyUint32(ctx, s->dw.seq_list, s->dw.seq_n++, JS_DupValue(ctx, s->dw.seq.value));
                        continue;
                    }
                    s->dw.seq_phase = 2;
                }
                /* §3.2.7 `double` AS AN ELEMENT CONVERSION — ToNumber, which is the page's `valueOf`, so it
                   is a request and its own rest point exactly as the ToString arm below is. The RESTRICTED
                   type's refusal of a non-finite value belongs here and not to whichever algorithm reads the
                   list: `sequence<double>` is what the IDL declares, and `[Infinity]` is a TypeError before
                   any step of the caller runs. */
                if (t == IDL_SEQUENCE_DOUBLE) {
                    double d = 0.0;

                    DCHECK(s->dw.seq_phase == 2, "the sequence conversion resumed at a phase it never parks in");
                    r = step_todouble_run(ctx, &s->hdr, s->dw.seq.value, cb_result, &d, out_cb, out_argc);
                    cb_result = JS_UNDEFINED;
                    if (r > 0) return r;
                    if (r < 0) return JS_STEP_ABRUPT;
                    if (!isfinite(d)) {
                        JS_ThrowTypeError(ctx, "an element of argument %d is not a finite double", s->i + 1);
                        return JS_STEP_ABRUPT;
                    }
                    JS_SetPropertyUint32(ctx, s->dw.seq_list, s->dw.seq_n++, JS_NewFloat64(ctx, d));
                    s->dw.seq_phase = 1;
                    continue;
                }
                {
                    JSValue str = JS_UNDEFINED;
                    DCHECK(s->dw.seq_phase == 2, "the sequence conversion resumed at a phase it never parks in");
                    r = step_tostring_run(ctx, &s->hdr, s->dw.seq.value, cb_result, &str, out_cb, out_argc);
                    cb_result = JS_UNDEFINED;
                    if (r > 0) return r;
                    if (r < 0) return JS_STEP_ABRUPT;
                    /* THE SCALAR VALUE CONVERSION BELONGS TO THE ELEMENT TYPE. `BlobPart`'s string arm is a
                       USVString, so §3.2.12 replaces its lone surrogates; a `sequence<DOMString>`'s element is
                       a DOMString and an enumeration's is §3.2.18's own ToString, and both keep them.
                       IT IS ASKED POSITIVELY AND IT USED TO BE ASKED AS `t != IDL_SEQUENCE_DOMSTRING`, which
                       is a NEGATIVE test over a set that grows: every sequence row added afterwards silently
                       inherited a §3.2.12 conversion its element type never asked for, and the row that would
                       have inherited it is the one directly below. A test naming the type that DOES want it
                       cannot go wrong that way. */
                    if (t == IDL_SEQUENCE_BLOBPART) {
                        str = JS_ToScalarValueString(ctx, str);   /* §3.2.12: lone surrogates become U+FFFD */
                        if (JS_IsException(str)) return JS_STEP_ABRUPT;
                    }
                    /* §3.2.18's ELEMENT CONVERSION, STEP 2 — "If S is not one of E's enumeration values, then
                       throw a TypeError", over the string step 1's ToString just produced. It runs none of the
                       page's code, so it is decided here rather than being a rest point of its own; and it is
                       decided BEFORE the cursor's next pull, which is §3.2.21.1's own order (the element
                       conversion is step 3.3 of the repeat loop, after step 3.1's IteratorStepValue and before
                       the next iteration's) and is observable to a page whose iterator has side effects. */
                    if (t == IDL_SEQUENCE_ENUM &&
                        idl_enum_check(ctx, str, idl_arg_enum_values(m, ti), "argument") < 0) {
                        /* The string is this block's, not yet handed to the list — the abrupt return below
                           discharges everything the STATE owns and knows nothing about a local. */
                        JS_FreeValue(ctx, str);
                        return JS_STEP_ABRUPT;
                    }
                    JS_SetPropertyUint32(ctx, s->dw.seq_list, s->dw.seq_n++, str);
                    s->dw.seq_phase = 1;
                }
            }
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            *slot = s->dw.seq_list;
            s->dw.seq_list = JS_UNDEFINED;
            s->dw.seq_n = 0;
            s->dw.seq_phase = 0;
            goto placed;
        }

        /* §3.2.15's INTERFACE type: the brand test, once, so no body performs it — and a value that is not one
           is a TypeError BEFORE the algorithm's step 1, which is what `walker.currentNode = null` asserts. */
        if (t == IDL_INTERFACE) {
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            DCHECK(idl_arg_iface_stated(m, ti),
                   "an interface-typed argument was declared with no interface to brand against — "
                   "idl_iface_brand (a class) or idl_arg_iface (a predicate and the identifier) is the other "
                   "half of that type");
            if (!idl_arg_implements(ctx, m, ti, a)) {
                JS_ThrowTypeError(ctx, "argument %d does not implement %s", s->i + 1,
                                  idl_arg_iface_subject(m, ti));
                return JS_STEP_ABRUPT;
            }
            *slot = JS_DupValue(ctx, a);
            goto placed;
        }

        /* §3.2.16's NULLABLE CALLBACK INTERFACE: null and undefined are the IDL null, ANY object crosses as
           itself (its operation is read off it by name, so it need not be callable), and a primitive is a
           TypeError. */
        if (t == IDL_CALLBACK_INTERFACE_NULLABLE) {
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            if (JS_IsNull(a) || JS_IsUndefined(a)) {
                *slot = JS_NULL;
                goto placed;
            }
            if (!JS_IsObject(a)) {
                JS_ThrowTypeError(ctx, "argument %d is not an object and cannot be a callback interface",
                                  s->i + 1);
                return JS_STEP_ABRUPT;
            }
            *slot = JS_DupValue(ctx, a);
            goto placed;
        }

        /* §3.2.26 Buffer source types, over §4.2's `BufferSource`: the brand test, once, so no body performs
           it. (The number was §3.2.25 here, which is `Union types` — see the row's own comment in the header.) */
        if (t == IDL_BUFFERSOURCE) {
            if (!JS_IsArrayBuffer(a) && JS_GetTypedArrayType(a) < 0 && !JS_IsDataView(a)) {
                JS_FreeValue(ctx, cb_result);
                JS_ThrowTypeError(ctx, "the argument is not a BufferSource");
                return JS_STEP_ABRUPT;
            }
            if (idl_buffer_source_refuse(ctx, a, "BufferSource", false, false)) {
                JS_FreeValue(ctx, cb_result);
                return JS_STEP_ABRUPT;
            }
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            *slot = JS_DupValue(ctx, a);
            goto placed;
        }

        /* §3.2.26 Buffer source types, over §4.1's `ArrayBufferView`: the SAME brand test WITHOUT the
           ArrayBuffer arm, which is the whole of the difference between the two typedefs. §4.1 lists the
           thirteen view types — the nine integer typed arrays, the three float ones and DataView — and every
           one of them crosses here as itself; a member that accepts only some of them takes that refusal in
           its own algorithm, where the standard puts it (Web Cryptography §10.1.1 step 1's TypeMismatchError),
           and NOT here, because the two throws are different exceptions a page tells apart. */
        if (t == IDL_ARRAYBUFFERVIEW) {
            if (JS_GetTypedArrayType(a) < 0 && !JS_IsDataView(a)) {
                JS_FreeValue(ctx, cb_result);
                JS_ThrowTypeError(ctx, "the argument is not an ArrayBufferView");
                return JS_STEP_ABRUPT;
            }
            if (idl_buffer_source_refuse(ctx, a, "ArrayBufferView", false, false)) {
                JS_FreeValue(ctx, cb_result);
                return JS_STEP_ABRUPT;
            }
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            *slot = JS_DupValue(ctx, a);
            goto placed;
        }

        /* §3.2.26 Buffer source types over ONE of its twelve typed arrays — the arm the two above cannot
           express, because its step 2 tests [[TypedArrayName]] "with a value equal to T's name" and T is what
           the POSITION declares. Every step is here in the standard's own order: step 2's brand, then steps 3
           and 4 through the same refusal the two rows above use, each conditioned on the §3.3 attribute this
           position states rather than on this file's opinion of what the platform contains. */
        if (t == IDL_TYPED_ARRAY) {
            const IdlTypedArrayDecl *d;

            DCHECK(m->arg_views != NULL && ti >= 0 && m->arg_views[ti].kind >= 0,
                   "an IDL_TYPED_ARRAY position reached its conversion with no typed array declared — §3.2.26 "
                   "step 1 is \"let T be the IDL type V is being converted to\", and idl_args_seal asserts "
                   "every such position states its T with idl_typed_array, so this member was declared after "
                   "the platform was sealed");
            d = &m->arg_views[ti];
            /* STEP 2, AND IT IS THE WHOLE BRAND TEST. A DataView has no [[TypedArrayName]] at all and every
               other typed array's is a different name, so one comparison answers for all thirteen view types
               §4.1 lists and for the two buffers §4.2 adds — `enc.encodeInto("x", new Float64Array(1))` is a
               TypeError from the TYPE, where "is it a typed array" admitted it and let the algorithm write
               bytes into a view whose elements are eight of them. */
            if (JS_GetTypedArrayType(a) != d->kind) {
                JS_FreeValue(ctx, cb_result);
                JS_ThrowTypeError(ctx, "the argument is not a %s", idl_typed_array_name(d->kind));
                return JS_STEP_ABRUPT;
            }
            if (idl_buffer_source_refuse(ctx, a, idl_typed_array_name(d->kind),
                                         d->allow_shared, d->allow_resizable)) {
                JS_FreeValue(ctx, cb_result);
                return JS_STEP_ABRUPT;
            }
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            *slot = JS_DupValue(ctx, a);
            goto placed;
        }

        /* `BodyInit?`: null and undefined are the IDL null; a BufferSource crosses as itself; anything else
           is the union's USVString arm. The brand test is the union's own rule, stated once. */
        if (t == IDL_BODYINIT_NULLABLE) {
            if (JS_IsNull(a) || JS_IsUndefined(a)) {
                JS_FreeValue(ctx, cb_result);
                cb_result = JS_UNDEFINED;
                *slot = JS_NULL;
                goto placed;
            }
            /* THE FOUR INTERFACE ARMS cross as themselves; only what is none of them takes the USVString arm.
               `new Response(blob)` stringified to the thirteen bytes of "[object Blob]" while three of those
               interfaces existed, because the test was written when none of them did. */
            /* THE BUFFERSOURCE ARM IS ASKED FIRST AND SEPARATELY, for the reason BlobPart's is: §3.2.25's
               buffer clauses return the result of CONVERTING V, so §3.2.26 Buffer source types' refusals run
               on a body that is one. The other four arms are INTERFACE types, which §3.2.25 does hand back as
               references, so they cross untouched. A question some entries of a conversion ask and others do
               not is one missing capability wearing several names, and this is the third entry of this one. */
            if (JS_IsArrayBuffer(a) || JS_GetTypedArrayType(a) >= 0 || JS_IsDataView(a)) {
                if (idl_buffer_source_refuse(ctx, a, "BodyInit", false, false)) {
                    JS_FreeValue(ctx, cb_result);
                    return JS_STEP_ABRUPT;
                }
                t = IDL_ANY;
            } else {
                t = (blob_is(a) || form_data_is(a) || usp_list_of(a) || readable_stream_is(a))
                  ? IDL_ANY : IDL_DOMSTRING;
            }
        }

        /* `(File or USVString or FormData)?`: the same shape as BodyInit's, over the arms HTML §4.13.7.3
           names. A plain Blob is not one of them, so it takes the USVString arm and stringifies. */
        if (t == IDL_FORMVALUE_NULLABLE) {
            if (JS_IsNull(a) || JS_IsUndefined(a)) {
                JS_FreeValue(ctx, cb_result);
                cb_result = JS_UNDEFINED;
                *slot = JS_NULL;
                goto placed;
            }
            t = (blob_file_name_of(a) != NULL || form_data_is(a)) ? IDL_ANY : IDL_USVSTRING;
        }

        /* `DOMString?` and `USVString?`: null AND undefined become the IDL null before any ToString is
           reached; what survives takes the un-nullable type's own conversion. */
        if (t == IDL_DOMSTRING_NULLABLE || t == IDL_USVSTRING_NULLABLE) {
            if (JS_IsNull(a) || JS_IsUndefined(a)) {
                JS_FreeValue(ctx, cb_result);
                cb_result = JS_UNDEFINED;
                *slot = JS_NULL;
                goto placed;
            }
            t = (t == IDL_DOMSTRING_NULLABLE) ? IDL_DOMSTRING : IDL_USVSTRING;
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
            int res = 0;

            /* Web IDL §3.2.3 boolean: "Let x be the result of computing ToBoolean(V)" then "Return the IDL
               boolean value that is the one that represents the same truth value as the JavaScript Boolean
               value x". ToBoolean runs none of the page's code, but the ARGUMENT still crosses CONVERTED:
               `toggle(t, 1)` forces on, and a body that got the 1 would have to remember to coerce it.
               AND OVER UNKNOWN EXTERNAL INPUT §3.2.3 IS A FORK, NOT A COERCION — this position is the ONE
               place that fact can be acted on, which is why it is here and not in the forty-odd bodies that
               read a boolean argument. ECMAScript §7.1.2 ToBoolean ( arg ) ends "Return true", and a concolic
               wears an ordinary Object (solver/concolic.c installs one so a method on an unknown yields
               another unknown instead of throwing), so the coercion answered `true` for every unknown there
               has ever been — `el.hidden = flags.beta`, `node.cloneNode(cfg.deep)`, `xhr.open(m, u, cfg.sync)`
               each ran one world and deleted the other with nothing to say so. That is the SAME defect
               §3.2.25's arm had three hundred lines up, one type below it: a conversion answering from the
               REPRESENTATION rather than from the page's value, which is not a coarse answer but a DECIDED
               one.
               CROSSING IS NOT THE CURE HERE, AND THAT IS WHAT SEPARATES THIS TYPE FROM EVERY STRING AND
               NUMBER ABOVE. A crossed DOMString reaches a body that asks it for its bytes and carries the
               taint to a sink; a boolean's only consumer is CONTROL FLOW, so a crossed one has nowhere to go
               but a `JS_ToBool` in some body — the collapse moved, not removed. See idl_args.h's
               IDL_CONCOLIC_FORKS.
               IT IS THE BRANCH SEAM AND NOT THE OUTCOME SEAM, WHICH IS WHY THIS IS step_tobool_run AND NOT THE
               step_fork_run THE UNION ARM ABOVE USES. quickjs-step.h states the rule at both: a ToBoolean is
               the SAME PREDICATE `if (p)` asks about, keyed by the value's own branch identity, so
               `if (cfg.on)` and `el.classList.toggle(t, cfg.on)` are ONE gate and one constraint entry. Asked
               at the outcome seam this would file a second, independent entry over one predicate — a flow that
               had already pinned `cfg.on` would fork again and stand on two arms contradicting each other —
               and it would record NONE of what a branch records, so an @H parameter the page had gated would
               print with no domain at all. The union arm above is genuinely the other question: which of the
               conversion's OWN completions this position reaches, which no `if` in the page ever asks.
               NOTHING IS NUMBERED HERE, and that is a consequence of the seam rather than an omission. A truth
               value has exactly two completions and the arm a real session takes is computed by the branch
               seam out of the value's own example, so this site declares neither an ordering nor a `real`;
               step_tobool_run hands back 0 or 1 and they mean false and true.
               A VALUE THE PAGE DETERMINED TAKES §7.1.2 ITSELF, inside that same call, with no request and no
               fork — so there is no `concolic_is` predicate at this call site, because there is no second path
               for one to select. */
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            /* THE SITE AND THE RULE ARE TWO HALVES OF ONE STATEMENT, and this is the half that can still be
               checked — asked only on the unknown path it could never fire, because a boolean whose rule said
               CROSSES would be swallowed by the pass-through hundreds of lines up and this branch would simply
               go unvisited. Asked here it fires on the FIRST boolean conversion of any kind, which is what
               makes a header edit that puts IDL_BOOLEAN back under `default:` a crash rather than a silent
               return to pinning every unknown flag to `true`. */
            DCHECK(idl_concolic_rule(IDL_BOOLEAN) == IDL_CONCOLIC_FORKS,
                   "§3.2.3's conversion asks §7.1.2 ToBoolean at the branch seam for unknown input while "
                   "idl_args.h no longer declares IDL_BOOLEAN as a type that forks — the pass-through above "
                   "would then cross an unknown boolean as itself, this ask would never be reached, and every "
                   "`JS_ToBool` in every body that reads a boolean argument would answer `true` again with "
                   "nothing to say so");
            /* §3.6 step 15.4.2's "missing" is placed rather than coerced, and it is placed BEFORE the coercion
               because it is not a value: a required position handed `undefined` is what §3.2.3 converts (to
               false, which is what every body's own `JS_ToBool` of the placed undefined already answers), and
               an ABSENT one is what a body tells apart with its `argc` test. */
            if (JS_IsUndefined(a)) { *slot = JS_UNDEFINED; goto placed; }
            r = step_tobool_run(ctx, &s->hdr, a, "Web IDL §3.2.3 boolean", &res);
            if (r) return r;
            DCHECK(res == 0 || res == 1,
                   "§3.2.3's conversion came back with something that is neither of the two truth values a "
                   "boolean has — the two worlds a flag names are all there are, and a third is a world "
                   "nothing can be in");
            *slot = JS_NewBool(ctx, res);
            goto placed;
        }
        if (t == IDL_OBJECT_NULLABLE) {
            /* §3.2.16 `object`: "If Type(V) is not Object, then throw a TypeError." §3.2.20's nullable wrapper
               puts null and undefined ahead of that as the IDL value null. Nothing is READ, so no page code
               runs — which is why the refusal is the TYPE's and not a body's. */
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            if (JS_IsUndefined(a) || JS_IsNull(a)) { *slot = JS_NULL; goto placed; }
            if (!JS_IsObject(a)) {
                JS_ThrowTypeError(ctx, "argument %d is not an object", s->i + 1);
                return JS_STEP_ABRUPT;
            }
            *slot = JS_DupValue(ctx, a);
            goto placed;
        }
        if (idl_is_numeric(t)) {
            double num;   /* see the dictionary member's arm: a local, because a park writes nothing */

            r = step_todouble_run(ctx, &s->hdr, a, cb_result, &num, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
            *slot = idl_num_of(ctx, t, num);
            if (JS_IsException(*slot)) {   /* §3.2.7's restricted double refused the value */
                *slot = JS_UNDEFINED;
                return JS_STEP_ABRUPT;
            }
            goto placed;
        }
        if (t == IDL_CALLBACK) {
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            if (!JS_IsUndefined(a) && !JS_IsFunction(ctx, a)) {
                JS_ThrowTypeError(ctx, "argument %d is not callable", s->i + 1);
                return JS_STEP_ABRUPT;
            }
            *slot = JS_DupValue(ctx, a);
            goto placed;
        }
        DCHECK(t == IDL_DOMSTRING || t == IDL_BYTESTRING || t == IDL_USVSTRING || t == IDL_ENUM,
               "an IDL argument was declared with a type this machine does not convert");
        r = step_tostring_run(ctx, &s->hdr, a, cb_result, slot, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;          /* parked ON THIS ARGUMENT; the resume comes back to it */
        if (r < 0) return JS_STEP_ABRUPT;
        if (t == IDL_BYTESTRING && idl_bytestring_check(ctx, *slot) < 0) return JS_STEP_ABRUPT;
        /* §3.2.18's ENUMERATION, AT A POSITIONAL ARGUMENT — the same check the dictionary path makes, over the
           string ToString produced, against the list the declaration named. It is here rather than in a body
           because it is part of the TYPE: `history.scrollRestoration = "bogus"` is a TypeError from the
           conversion, before the setter's algorithm runs at all, and a body performing it would be one body's
           private copy of a rule every enumeration member has. */
        if (t == IDL_ENUM && idl_enum_check(ctx, *slot, idl_arg_enum_values(m, ti), "argument") < 0)
            return JS_STEP_ABRUPT;
        if (t == IDL_USVSTRING) {
            *slot = JS_ToScalarValueString(ctx, *slot);
            if (JS_IsException(*slot)) return JS_STEP_ABRUPT;
        }
    placed:
        if (m->variadic) {
            JS_SetPropertyUint32(ctx, s->conv, (uint32_t)s->i, s->vstage);
            s->vstage = JS_UNDEFINED;
        }
        /* THE UNION'S ARM IS PER POSITION — the same reason the store above is per position. Two
           `(DOMString or sequence<DOMString>)` arguments in one member each ask their own value. §3.6 step
           12's overload arm is per position for the identical reason, and keeps its own byte because a
           dictionary it resolved to would otherwise clear the union's out from under it. */
        s->dw.uni_phase = IDL_UNI_ASK;
        s->ovl_phase = IDL_UNI_ASK;
        s->i++;
    }

    /* THE PROLOGUE IS OVER, so the stage stops being this machine's and becomes the MEMBER'S. It is advanced
       here — at the one place the conversion loop is known to have finished — rather than by the body, because
       a body that had to remember to leave stage 1 behind is a body whose forgetting makes its first algorithm
       step indistinguishable from a suspension inside the page's `toString`. Every member passes through this
       line, including the ones whose bodies are plain C: the stage names where the machine is, and after this
       line the machine is inside the member. */
    if (s->hdr.stage == 1) s->hdr.stage = IDL_STEP_FIRST;

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
        r = m->step->body(ctx, &s->hdr, idl_body_state(m, s), s->n,
                          (JSValueConst *)(argv_vec ? argv_vec : idl_args_vec(s)),
                          cb_result, &s->result, out_cb, out_argc);
        idl_free_vec(ctx, argv_vec, s->n);
        if (r < 0) {
            s->result = JS_UNDEFINED;
            DCHECK(!g_tree || !g_tree->recorded(), IDL_TREE_THREW);
            s->ce_threw = 1;                              /* §4.13.6 step 2's catch */
            s->ce_exc = JS_GetException(ctx);
            return idl_ce_finish(ctx, s, JS_UNDEFINED, out_cb, out_argc);
        }
        /* A REQUEST carries operands in out_cb that only the driver's immediate read can honour, so the drain
           cannot come first there. No step body both mutates the tree and asks the page for something — and if
           one ever does, its steps would run AFTER that page code, which is the ordering §4.2.3 forbids. */
        if (r > 0 && r != JS_STEP_YIELD) {
            DCHECK(!g_tree || !g_tree->recorded(),
                   "a step body mutated the tree and then parked on the page's code — the insertion steps would "
                   "run after that code, which is not the order §4.2.3 states; split the mutation and the "
                   "request into two stages");
            return r;
        }
        idl_tree_take(ctx, s);
        s->tree_after_body = (r == 0);
        /* JS_UNDEFINED IS THE WHOLE OF WHAT A FIRST ENTRY HAS TO SAY: the walk has just been handed its buffer
           and has made no request, so there is no completion owed to it. The re-entry above is the only site
           that ever carries one. */
        if (s->tree) { int d = idl_tree_drain(ctx, s, JS_UNDEFINED, out_cb, out_argc); if (d) return d; }
        if (r) return r;
        return idl_ce_finish(ctx, s, JS_UNDEFINED, out_cb, out_argc);
    }
    s->result = m->setter
        ? m->setter(ctx, s->hdr.this_val, s->n > 0 ? *idl_arg_slot(m, s, 0) : JS_UNDEFINED, m->magic)
        : m->body(ctx, s->hdr.this_val, s->n,
                  (JSValueConst *)(argv_vec ? argv_vec : idl_args_vec(s)), m->magic);
    idl_free_vec(ctx, argv_vec, s->n);
    if (JS_IsException(s->result)) {
        s->result = JS_UNDEFINED;
        DCHECK(!g_tree || !g_tree->recorded(), IDL_TREE_THREW);
        s->ce_threw = 1;                                  /* §4.13.6 step 2's catch */
        s->ce_exc = JS_GetException(ctx);
        return idl_ce_finish(ctx, s, JS_UNDEFINED, out_cb, out_argc);
    }
    idl_tree_take(ctx, s);
    s->tree_after_body = 1;
    /* JS_UNDEFINED for the reason the step-body entry above states: a walk that has just been handed its
       buffer is owed no completion. */
    if (s->tree) { int d = idl_tree_drain(ctx, s, JS_UNDEFINED, out_cb, out_argc); if (d) return d; }
    return idl_ce_finish(ctx, s, JS_UNDEFINED, out_cb, out_argc);
}

/* THE COMPLETION, AND THE TWO THINGS NO DECLARATION CAN CARRY — it neither restates the list nor discharges it.
 *
 * Every JSValue this machine owns — its result, its converted argument vector, its dictionary and sequence
 * cursors, its element-reaction queue, its nested conversion frames, and the member body's own state — is named
 * by js_idl_args_visit, because a deep fork has to take a second reference to each. So this function used to be
 * that same list written out a second time, in another order, forty lines long: exactly the pair this engine
 * forbids, where adding a field to a state creates an obligation in TWO places and nothing catches the one that
 * is missed. It had already been missed — querySelectorAll's collected-matches array was named by qs_visit and
 * freed by nothing, so every abandoned selector walk leaked its element wrappers, and the repair was to hand-add
 * the free to the second list, which left the next omission exactly as invisible.
 *
 * There is one list now. What remains here is only what a declaration cannot express: the POD cursors the
 * zeroed state reads as "nothing in flight", and the member's own `release`, whose contract is asserted below
 * rather than described. */
static JSValue idl_args_result(JSContext *ctx, void *st, bool take_result)
{
    JSIdlArgsState *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    const IdlMember *m;

    DCHECK(s->hdr.arg >= 0 && s->hdr.arg < g_n, "an IDL member's teardown ran with no pool entry behind it");
    m = idl_member(s->hdr.arg);
    /* HANDED OVER, so the declaration must not release it. Cleared BEFORE the discharge rather than after,
       because the discharge is the only thing that reads the slot. */
    if (take_result) s->result = JS_UNDEFINED;

    /* THE MEMBER'S OWN RELEASE GOES FIRST, AND IT TOUCHES NO SLOT THE DECLARATION NAMES. It runs first because
       the work it does is real algorithm work that READS what this machine owns — §4.13.4 step 14's "regardless
       of whether the above steps threw" lowers a flag off `s->registry` — and it may free only what the
       declaration does NOT name: a lexbor handle, a foreign C allocation, a flag its algorithm took. That
       split is measured, not trusted: freeing a declared value is silent both ways it can be written
       (free-and-null leaves the discharge a no-op; free-without-null makes the discharge the second free), so
       the declaration is folded into a number on each side of the call and the two must agree.
       WHAT THE FOLD MEASURES IS SLOT IDENTITY — the tag of every declared slot, and its payload where that is a
       pointer — AND IT IS DELIBERATELY NOT A REFERENCE COUNT. This bracket USED to fold the heap's count of
       every declared value and it could not mean what it said: a count records HOW MANY holders an object has
       and never WHICH, so a `release` that gives back SOMEBODY ELSE'S reference to an object a declared slot
       also names (§4.13.4's active custom element constructor map is keyed BY a constructor, and `C` is a
       declared value) moved it by exactly what a `release` discharging the declaration moved it by. Every
       completed `document.createElement` of a defined name aborted on that, and no measurement separates the
       two. So the rule this bracket states is the one it can actually decide: a `release` leaves every slot the
       declaration names naming the same thing. Free-and-null, replace and hand-over move the number;
       free-WITHOUT-null does not, and reaches the discharge as the second free where the allocator answers —
       the same trade quickjs.c's fold has always taken for a declared `buf`.
       IT THEREFORE DOES NOT FORBID a `release` from moving reference counts elsewhere in the agent's object
       graph. The active-ctor give-back is still paid below this bracket rather than in a `release`, and the
       reason is now only the second one it always also had: it is one half of a PAIR whose other half
       (custom_elements_queue_unlock, §4.13.5 "Upgrades" step 10's regardless-list) is paid there, and the two
       must unwind in nesting order — a member's `release` runs before that unlock, so an upgrade reached from
       inside this member's own Construct would leave the OUTER bracket first.
       AND IT NAMES THE MEMBER, because this is the ONE point every declared member's teardown converges on: an
       assert stamped with this line and a remedy phrased as "release only what the declaration does not name"
       is an instruction with no object, and finding which of the platform's members it meant is a search of
       every declaration in the tree. The member's own algorithm string is the address. */
    if (m->step && m->step->release) {
#if APICLIENT_DEV
        uint64_t owned_before = JS_StepVisitOwnedFingerprint(ctx, m->step->visit, idl_body_state(m, st));
        uint64_t owned_after;
#endif
        m->step->release(ctx, idl_body_state(m, st));
#if APICLIENT_DEV
        owned_after = JS_StepVisitOwnedFingerprint(ctx, m->step->visit, idl_body_state(m, st));
        DCHECKF(owned_after == owned_before,
                "the `release` of %s (%s) left a slot its own `visit` names naming something else. The visit IS "
                "the one list of what the state owns and the teardown discharges it; a second list beside it "
                "leaks whatever the next field misses, and double-frees whatever this one did not null. Release "
                "only what the declaration does not name: a lexbor handle, a foreign C allocation, a flag to "
                "lower. This fold reads slot IDENTITY and never a reference count, so it is NOT reporting that "
                "the agent's object graph moved — one of the declaration's own slots was freed, nulled, "
                "replaced or handed over, and that slot is the thing to stop touching",
                m->name ? m->name : "an unnamed member",
                m->step->algorithm ? m->step->algorithm : "no algorithm declared");
#endif
    }

    /* §8.1.4.6 step 5's FLAG, if this member's reaction drain was abandoned holding it. Not a reference, so no
       declaration names it and the discharge the driver runs after this cannot give it back; leaving it set
       would put the global in error reporting mode forever and silently swallow every later report. */
    custom_elements_queue_unlock(ctx, &s->ce);
    /* …AND THE SAME FLAG, IF THE §4.2.3 WALK WAS ABANDONED HOLDING IT. The walk parks on the page's code at
       insert step 12 (HTML §4.12.1.1 "Processing model" step 36's nested program, and the report its throw
       owes), so it is the second algorithm on this state that can be dropped mid-report. The buffer's own
       `release` runs on the walk's normal 0 edge and cannot answer for this one. */
    if (s->tree) {
        DCHECK(g_tree != NULL, "an IDL member holds a tree-steps buffer with no DOM layer registered");
        g_tree->unlock(ctx, s->tree);
    }
    /* …AND DOM §4.9 create an element STEPS 5.1.5-5.1.6, if this invocation entered §4.13.4's active custom
       element constructor map and step 5.1 did not get back out — which for a flow discarded while parked on
       the page's constructor is every time, and which no resume ever comes back to run. It is BELOW the
       fingerprint bracket and not inside the member's `release` because giving the entry back drops a
       reference to `C`, and that bracket cannot tell that from a `release` discharging the declaration.
       IT IS ALSO WHERE THE AGENT'S OTHER BRACKET OF THIS PAIR IS GIVEN BACK — custom_elements_queue_unlock
       above runs §4.13.5 "Upgrades" step 10's regardless-list steps 1-2 — and the ORDER of the two is the
       nesting: an upgrade reached from inside this member's own Construct entered LAST, so it leaves FIRST, and
       the leave's own top-of-stack identity assert is what catches a pair that ran out of order.
       The map is agent-wide, so an entry left on it answers every later `new C()` in the agent, in flows that
       never named this registry — and nothing reports it, because the entry is a live value on a live object. */
    /* THE QUESTION IS "IS THERE A CONSTRUCTOR HERE", NOT "IS THE SLOT UNDEFINED", because a step state arrives
       ZEROED and a zeroed JSValue is a NUMBER rather than JS_UNDEFINED — so a machine torn down before its
       stage 0 ever ran would answer "not undefined" and hand §4.13.4's leave an integer. Every key of that map
       is a constructor, which its own enter asserts, so this is the same fact stated in the form the zeroed
       state also answers correctly. */
    if (JS_IsObject(s->ace_ctor)) {
        custom_elements_active_ctor_leave(ctx, s->ace_ctor);
        JS_FreeValue(ctx, s->ace_ctor);
        s->ace_ctor = JS_UNDEFINED;
    }

    /* AND THE ONE LIST IS DISCHARGED BY THE DRIVER, after this returns — tramp_step_state_free_1 reads
       js_idl_args_visit exactly as it reads every other machine's, so this function neither restates it nor
       calls it. It covers the whole state: the converted argument vector, every declared conversion frame, the
       sequence cursor and its collected elements, the drain buffer of an abandoned §4.2.3 walk, the
       element-reaction queue of a flow dropped mid-member, and — through its last line — the member body's own
       owned values. An abandoned queue's reactions die with the flow, which is what an abandoned flow means.
       The cursors below are what no declaration can carry: a zeroed state means "nothing in flight", and these
       are what say so. `ce_threw` is the epilogue's caught-completion flag, and the WALK's are its whole
       in-flight statement — `started` above all, because a §3.2.17 conversion abandoned mid-member (a throw
       from a `toString`, a flow dropped) leaves it set, and the walk's own start refuses to begin over one that
       is. It is cleared here and not freed here: the values it names are the driver's to discharge, through the
       one `visit` that named them. */
    idl_dict_walk_reset_cursors(&s->dw);
    s->ce_threw = 0;
    return r;
}

JSValue idl_dict_get(JSContext *ctx, JSValueConst dict, const char *name)
{
    if (!JS_IsObject(dict)) return JS_UNDEFINED;
    return JS_GetPropertyStr(ctx, dict, name);
}

/* …AND A BOOLEAN ONE IS A COERCION, WHICH IS WHY IT ASSERTS WHAT THE READ ABOVE DOES NOT.
   Web IDL §3.2.17 Dictionary types step 4.1.4 converts a `boolean` member with ToBoolean, and this engine
   deliberately does NOT run that conversion over unknown external input: idl_args.h's IDL_CONCOLIC_CROSSES
   crosses the value as ITSELF so opacity survives the boundary and reaches the member's own algorithm. This
   function is where that care is spent, because a concolic wears an Object — solver/concolic.c gives it one so
   a method on an unknown yields another unknown — and EVERY Object is truthy. ToBoolean here therefore does
   not read the member: it PINS it to `true` for every unknown there has ever been and deletes the world in
   which it is false, one level below the union arm that has the same shape.
   A MEMBER THAT NEEDS A C bool OUT OF AN UNKNOWN ASKS FOR AN ARM, and an ask is a request, so the member is a
   step machine (idl_method_id_step) whose stage calls step_fork_run — decide.h states why a plain C body
   cannot: it is already inside its activation and has no state for the other arm to be snapshotted at. This
   assert is what names that conversion at the site that needs it, rather than letting a member go on answering
   one world confidently. */
bool idl_dict_bool(JSContext *ctx, JSValueConst dict, const char *name)
{
    JSValue v = idl_dict_get(ctx, dict, name);
    bool b;

    DCHECK(!concolic_is(v),
           "a boolean dictionary member was read out of UNKNOWN EXTERNAL INPUT through the plain C reader — "
           "ToBoolean pins it to `true` for every unknown and deletes the false world. Declare this member a "
           "step machine (idl_method_id_step) and ask step_fork_run for the flag at its own stage");
    b = JS_ToBool(ctx, v);
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

    /* ONE STATEMENT OF THE TAIL. It was written here as a field store and again nowhere else, which was fine
       while this was the only declaration form that could carry one; a step machine could not be variadic at
       all until idl_variadic existed, and two ways to say the same thing is how the two come to disagree. */
    if (variadic) idl_variadic();
    idl_member(g_n - 1)->iface = iface;
    return id;
}

/* THE ONE BUILDER EVERY DECLARED MEMBER GOES THROUGH, plain and step alike — because a definition must be
   COMPLETE at the single moment it is handed to the runtime, and the two entries below differ only in whether
   there is a step declaration to finish it with. `body` and `decl` are the two halves of "what runs once the
   conversions are done", and exactly one of them is given. */
static int idl_method_id_all(JSContext *ctx, const IdlArgType *types, int nargs,
                             const IdlDictMember *members, int nmembers, IdlBody body,
                             const IdlStepDecl *decl, int magic)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    int idx, k;

    DCHECK(g_rt == NULL || g_rt == rt,
           "an IDL member was installed into a second runtime — its step ids belong to the first, and one WASM "
           "instance is one document");
    /* THE CEILING WAS ALSO A DETECTOR, and this is what it was really detecting. A member declared more than
       once — a per-wrapper install minting a definition per object — showed up as the pool filling, which is a
       number standing in for the actual invariant: a component DECLARES at init and installs from the cached
       id, so once the document's install is done no declaration can be correct. That is asserted directly now,
       which catches the same bug at the first repeat instead of the 384th and cannot be reached by a platform
       that simply has more members in it. */
    g_rt = rt;
    idx = g_n++;
    idl_pool_reserve(idx);   /* the pool grows to fit the platform; there is nothing here to run out of */
    DCHECK(nargs >= 0, "a member declared a negative number of argument positions");   /* 0 = a getter */
    DCHECK(nargs == 0 || types != NULL,
           "a member declared argument positions and named no types for them — the type list IS the declaration");
    idl_member(idx)->body          = body;
    idl_member(idx)->setter        = NULL;
    idl_member(idx)->null_to_empty = false;
    idl_member(idx)->nargs = nargs;
    /* NO OPTIONAL ARGUMENTS, stated in the member's OWN terms: one past its last position. There is nothing
       here to overflow and nothing to keep in step with a ceiling — see idl_optional_from. */
    idl_member(idx)->first_optional = nargs;
    /* §3.6's LENGTH-DIFFERING SPLIT, READ OFF THE TYPE LIST WHEREVER THE TYPE LIST CAN SAY — a union type at
       the split position states the position AND the second entry's type, so asking for the position again
       would be one fact stated twice and free to disagree. Where the two entries share their type there, the
       list cannot say and idl_overload_length_split_at does; the loop below leaves `split_at` at -1 for such a
       member and that declaration sets it, asserting first that this loop found nothing. -1 is "this member
       declares none", which is nearly all of them, and the LONGER entry's optional index stays unset until
       idl_overload_split_optional_from states it — asserted at the seal, where every declaration exists and
       none can change again. */
    idl_member(idx)->split_at = -1;
    idl_member(idx)->split_longer_optional = -1;
    for (k = 0; k < nargs; k++)
        if (idl_type_is_length_split(types[k])) {
            DCHECK(idl_member(idx)->split_at < 0,
                   "a member declared TWO §3.6 length-differing overload splits — steps 3-4 remove entries by "
                   "one argument count, so a second split is a second answer to the same question and the "
                   "conversion would resolve every position against whichever was found first");
            idl_member(idx)->split_at = k;
        }
    /* NO DECLARED DEFAULTS, which is what §3.6's absent rule is the answer for. The array is allocated by the
       first idl_arg_default this member makes, so a member with none costs nothing. */
    idl_member(idx)->arg_dflts = NULL;
    /* NO DECLARED TYPED ARRAYS, on the same terms: the array is allocated by the first idl_typed_array this
       member states, so a member with no IDL_TYPED_ARRAY position costs nothing and idl_args_seal reads the
       NULL as "none stated" — which is a defect only where the member's own types name one. */
    idl_member(idx)->arg_views = NULL;
    /* NO PER-POSITION INTERFACE STATED, on the same terms: the array is allocated by the first idl_arg_iface
       this member makes, and a NULL means every branding position of this member takes idl_iface_brand's one
       class. That is a STATEMENT and not a hole — the same one IdlDictMember::iface's zero makes — and
       idl_args_seal is what turns "neither was stated" into a crash. */
    idl_member(idx)->arg_ifaces = NULL;
    /* HOW MANY THE CALLER MUST PASS. §3.6 throws a TypeError before ANY conversion when a call has fewer
       arguments than the member has required ones — `new File()` throws rather than building a File with no
       bits. It is the same number `first_optional` already states, capped at what the IDL declares, so a member
       that never calls idl_optional_from requires every argument it listed. */
    idl_member(idx)->magic = magic;
    /* THE DECLARED TYPES, COPIED — the caller's array is usually a `static const` and sometimes a local (a
       setter declares one type on the stack), so the pool owns its own copy for the life of the runtime. The
       allocation failing is a member that cannot be declared, which is an API the page cannot call: a CHECK,
       fatal in release too, never a truncation to whatever would fit. */
    idl_member(idx)->types = NULL;
    if (nargs > 0) {
        idl_member(idx)->types = malloc(sizeof(IdlArgType) * (size_t)nargs);
        CHECK(idl_member(idx)->types != NULL,
              "idl: OOM copying a member's declared argument types — a member that cannot be declared is an API "
              "the page cannot call");
        memcpy(idl_member(idx)->types, types, sizeof(IdlArgType) * (size_t)nargs);
    }
    idl_member(idx)->dict = members;
    idl_member(idx)->dict_n = 0;
    idl_member(idx)->dict_atoms = NULL;
    idl_member(idx)->conv_depth = 0;
    if (members) {
        int ndict = 0;
        for (k = 0; k < nargs; k++)
            if (idl_type_is_dictionary(types[k])) ndict++;
        DCHECK(ndict == 1, "a member declared dictionary members but not exactly one dictionary argument — the "
                           "conversion cursor is per-member, so a second dictionary would read the first's "
                           "names");
        idl_member(idx)->dict_atoms = malloc(sizeof(JSAtom) * (size_t)nmembers);
        CHECK(idl_member(idx)->dict_atoms, "idl: OOM interning a dictionary's member names");
        for (k = 0; k < nmembers; k++) {
            idl_dict_order_check(members, nmembers, k);
            idl_member(idx)->dict_atoms[k] = JS_NewAtom(ctx, members[k].name);
            /* A NESTED DICTIONARY is interned HERE, at the declaration, and so is every dictionary reachable
               from it: the conversion needs its member atoms live across a suspension, and the declaration is
               the last point at which the whole type tree is in hand. */
            if (members[k].dict) idl_dict_intern(ctx, members[k].dict);
        }
        idl_member(idx)->dict_n = nmembers;
        idl_member(idx)->conv_depth = (uint8_t)idl_members_depth(members, nmembers);
    }
    idl_member(idx)->step = decl;
    idl_member(idx)->steps = NULL;
    idl_member(idx)->nsteps = 0;
    idl_member(idx)->variadic = false;
    idl_member(idx)->iface = 0;
    /* NO RECEIVER INTERFACE STATED — see idl_implementation_check. Cleared here and never anywhere else, so a
       pool slot reused across a runtime cannot inherit the previous member's brand. */
    idl_member(idx)->this_is = NULL;
    idl_member(idx)->this_iface = NULL;
    /* NO PER-POSITION VALUE LIST STATED, on the same terms as the two arrays above: allocated by the first
       idl_arg_enum this member makes, so a member with no enumeration position costs nothing, and
       idl_args_seal is what turns "the type asks for one and none was stated" into a crash. */
    idl_member(idx)->arg_enums = NULL;
    /* THE STATE CARRIES THE MEMBER'S OWN ARGUMENT VECTOR AND NESTED-CONVERSION FRAMES, which is why its size is
       per-member and not a constant: a getter pays for neither, a fifteen-argument legacy initializer gets
       fifteen slots, and a member declaring a sequence-of-union type gets exactly the depth its declared type
       tree has. One statement of the layout (idl_off_body) computes both the size here and every offset the
       accessors read, so the definition and the reads cannot drift. */
    idl_def(idx)->size  = idl_off_body(nargs, idl_member(idx)->conv_depth);
    idl_def(idx)->step  = js_idl_args_step;
    idl_def(idx)->fini  = idl_args_result;
    idl_def(idx)->arg   = idx;
    idl_def(idx)->visit = js_idl_args_visit;
    idl_def(idx)->unforkable = js_idl_args_unforkable;
    /* EVERY DECLARED MEMBER CATCHES, AND IT CATCHES IN EXACTLY ONE PLACE. §4.13.6 step 1.3.1 says an upgrade
       reaction's throw is CAUGHT AND REPORTED and a callback reaction is invoked "with \"report\"" — so the
       epilogue every member ends through has an abrupt completion that is its own VALUE, and without this
       declaration one throwing custom element constructor tore the member down and dropped every reaction
       queued behind it. It is declared here for all members rather than per member because the epilogue is
       here for all members: a per-member opt-in would be a line to forget on the member that first needs it.
       Nowhere ELSE does this machine catch — an argument coercion's throw and the member body's own request
       propagate exactly as before, which js_idl_args_step_inner re-raises at its top. */
    idl_def(idx)->catches_abrupt = 1;
    /* THE WHOLE DECLARATION, FINISHED BEFORE THE REGISTRATION — and "finished" has to mean the list the machine
       will actually rest against, not a placeholder that reads like one.
       This said "DECLARED BEFORE THE REGISTRATION, WHICH IS THE WHOLE POINT" while admitting in its own next
       sentence that "a step member's real list overwrites these below" — which is to say the runtime was handed
       IDL_PLAIN_ALGORITHM and the plain step list, checked THOSE, and then idl_method_id_step replaced both
       after the check had passed. Every step member's joined list therefore entered the runtime unexamined:
       js_step_def_check saw a list that no flow would ever rest against, and the one it would rest against was
       seen by nothing. That is worse than an unchecked join, because the check that ran reads as if it had
       covered it.
       So the declaration is completed HERE, in the one function every member is built by, and the state where a
       registered definition's declaration is later rewritten no longer exists to get wrong. */
    if (decl) {
        /* THE OWNERSHIP DECLARATION IS MANDATORY, asserted where the member enters the pool. It is the ONE
           list: the fork clones through it, the teardown releases through it, and the teardown's assert folds
           it to check that `release` left it alone. A member without one has a state whose values are invisible
           to all three at once, which is not "it owns nothing" — a member that owns nothing writes a visit that
           visits nothing, and that is one line. */
        DCHECK(decl->visit != NULL,
               "an IDL member declared a step body with no `visit` — the visit is the one list of what its "
               "state owns, so without it a deep fork hands two flows one state and the teardown frees none "
               "of it");
        DCHECK(decl->algorithm != NULL && decl->steps != NULL,
               "an IDL member declared a step body with no algorithm or no steps — a stage the driver asserts "
               "against needs both halves, and half a declaration names a resume point nothing can report");
        idl_def(idx)->size += decl->state_size;   /* after this machine's state AND its conversion frames */
        /* THE MEMBER'S STAGES, JOINED TO THIS MACHINE'S TWO ENDS — the one place a member's declaration and its
           definition are both in hand, so no member restates the prologue and none can index its own steps from
           the wrong base. A label colliding across the join is caught by js_step_labels_check on the line
           below, which is the first point the whole list exists. */
        {
            int n = 0, j;
            const char **all;

            while (decl->steps[n]) n++;
            all = malloc(sizeof(*all) * (size_t)(IDL_STEP_FIRST + n + IDL_EPILOGUE_NSTEPS + 1));
            CHECK(all != NULL, "idl: OOM joining a member's declared steps to the prologue's — a member whose "
                               "stages cannot be named is a resume point nothing can check");
            for (j = 0; j < IDL_STEP_FIRST; j++) all[j] = IDL_PROLOGUE_STEPS[j];
            for (j = 0; j < n; j++) all[IDL_STEP_FIRST + j] = decl->steps[j];
            /* §4.13.6 steps 3-4, which every member ends through — one stage per arm of its invoke's switch. */
            for (j = 0; j < IDL_EPILOGUE_NSTEPS; j++) all[IDL_STEP_FIRST + n + j] = IDL_EPILOGUE_STEPS[j];
            all[IDL_STEP_FIRST + n + IDL_EPILOGUE_NSTEPS] = NULL;
            idl_member(idx)->steps  = all;
            idl_member(idx)->nsteps = n;
            idl_def(idx)->algorithm = decl->algorithm;
            idl_def(idx)->steps     = all;
        }
    } else {
        idl_def(idx)->algorithm = IDL_PLAIN_ALGORITHM;
        idl_def(idx)->steps     = idl_plain_steps();
    }
    {
        int sid = JS_RegisterStepDef(rt, idl_def(idx));
        idl_map_step(sid, idx);   /* the one place the runtime's id and this pool's index are both in hand */
        return sid;
    }
}

int idl_method_id_dict(JSContext *ctx, const IdlArgType *types, int nargs,
                       const IdlDictMember *members, int nmembers, IdlBody body, int magic)
{
    return idl_method_id_all(ctx, types, nargs, members, nmembers, body, NULL, magic);
}

int idl_method_id_step(JSContext *ctx, const IdlArgType *types, int nargs,
                       const IdlDictMember *members, int nmembers,
                       const IdlStepDecl *decl, int magic)
{
    /* A STEP MEMBER DIFFERS FROM A PLAIN ONE ONLY IN WHAT RUNS once the conversions are done, and in needing
       room after this machine's state for that thing to run in — so it is the same builder, handed the
       declaration instead of a body. Nothing is written to the pool entry AFTER it is built: the joined step
       list and the algorithm it belongs to are part of the definition the runtime is handed, which is the only
       way js_step_def_check can be asked about the list a flow will actually rest against. */
    DCHECK(decl != NULL,
           "an IDL member was declared as a step machine with no declaration — the state size, the ownership "
           "list and the stages all come off it, so there is nothing to build a machine out of");
    return idl_method_id_all(ctx, types, nargs, members, nmembers, NULL, decl, magic);
}

/* A SETTER THAT NAMES THE LAST DECLARATION MAY ONLY BE CALLED FROM A DECLARATION. All three of these
   (idl_optional_from, idl_iface_brand, idl_iface_narrow) describe "the member the LAST declaration made",
   because the id a declaration returns is the RUNTIME's step id and not this pool's index — so reaching the
   entry through the id was reading past the pool. That works only where the declaration is the thing that just
   happened. Called from an INSTALL it names whichever component declared last, and writes one member's rule
   onto a stranger: `postMessage`'s per-realm install did exactly that, once per realm, and nothing said so.
   The seal is where that becomes checkable — after it, no declaration can be correct, so neither can anything
   that describes one. Before it, the surrounding order is the caller's to keep, which is why each of these
   also asserts what it can about the member it lands on. */
#define IDL_LAST_DECL_ONLY                                                                            \
    "a member's declaration was described from outside a declaration — these setters name the LAST "  \
    "member declared, so after the platform is sealed they name whichever component declared last, "  \
    "not the member being installed. State it where the member is declared"

/* See idl_args.h. It names the member the LAST declaration made. */
void idl_optional_from(int first_optional)
{
    DCHECK(g_n > 0, "an optional-argument index was declared before any member was");
    DCHECK(!g_sealed, IDL_LAST_DECL_ONLY);
    /* IT IS AN INDEX INTO THIS MEMBER'S OWN LIST, so `nargs` — one past the last position — is the largest it
       can be, and that value is what "no optional arguments" already means. A member naming a position it never
       declared is a declaration that disagrees with its own IDL, and it used to be expressible because the
       "none" value was derived from a CEILING rather than from the member. */
    DCHECK(first_optional >= 0 && first_optional <= idl_member(g_n - 1)->nargs,
           "a member's first OPTIONAL argument is not one of the positions it declares");
    idl_member(g_n - 1)->first_optional = first_optional;
}

/* See idl_args.h. Same "names the last declaration" rule as idl_optional_from — and it is an index into the
   SAME type list, because a length-differing split is two entries over ONE declaration: the shorter one ends
   at the split and the longer one runs to `nargs`. */
void idl_overload_length_split_at(int shorter_last_position)
{
    IdlMember *m;

    DCHECK(g_n > 0, "an overload split position was declared before any member was");
    DCHECK(!g_sealed, IDL_LAST_DECL_ONLY);
    m = idl_member(g_n - 1);
    DCHECK(m->split_at < 0,
           "a member declared TWO §3.6 length-differing overload splits — its own type list already names one, "
           "and steps 3-4 remove entries by ONE argument count, so a second is a second answer to the same "
           "question and every arity would be resolved by whichever was found first");
    /* THE SHORTER ENTRY DECLARES AT LEAST ONE POSITION AND ENDS BEFORE THE LONGER ONE DOES, which is what
       makes the two entries differ in LENGTH at all: a split at the member's last position would leave the
       longer entry with nothing of its own. */
    DCHECK(shorter_last_position >= 0 && shorter_last_position < m->nargs - 1,
           "a §3.6 length-differing overload split was declared at a position that is not the SHORTER entry's "
           "last one — it names the final index that entry declares, so it must be inside the member's list "
           "and must leave at least one position for the longer entry");
    m->split_at = shorter_last_position;
}

/* See idl_args.h. Same "names the last declaration" rule as idl_optional_from — and it is an index into the
   SAME type list, because a length-differing split is two entries over ONE declaration: the shorter one ends
   at the split and the longer one runs to `nargs`. */
void idl_overload_split_optional_from(int longer_first_optional)
{
    IdlMember *m;

    DCHECK(g_n > 0, "a longer-overload-entry optional index was declared before any member was");
    DCHECK(!g_sealed, IDL_LAST_DECL_ONLY);
    m = idl_member(g_n - 1);
    DCHECK(m->split_at >= 0,
           "a member that declares no §3.6 length-differing overload split said where its LONGER entry's "
           "optional arguments begin — a split is either named by the member's own type list or stated with "
           "idl_overload_length_split_at, and a member with neither has no second entry for this to describe");
    /* THE LONGER ENTRY STARTS PAST THE SPLIT, so its first optional position cannot be at or before it: the
       shorter entry is the one that ENDS there, and a longer entry whose optional arguments began inside the
       shared prefix would be the shorter entry with extra positions rather than a second overload. */
    DCHECK(longer_first_optional > m->split_at && longer_first_optional <= m->nargs,
           "a longer overload entry's first OPTIONAL argument is not one of the positions past the split that "
           "the member declares");
    m->split_longer_optional = longer_first_optional;
}

/* See idl_args.h. Same "names the last declaration" rule as idl_optional_from, and it must be stated AFTER
   that one: §3.6 step 15.4 reads a default only for an OPTIONAL argument — its guard is "If optionality is
   'optional' and V is undefined" — so a default on a position the member
   still calls required is a declaration disagreeing with itself. */
void idl_arg_default(int index, IdlDictDefault dflt, const char *dflt_str)
{
    IdlMember *m;

    DCHECK(g_n > 0, "an argument default was declared before any member was");
    DCHECK(!g_sealed, IDL_LAST_DECL_ONLY);
    m = idl_member(g_n - 1);
    DCHECK(index >= 0 && index < m->nargs,
           "an argument default named a position the member's IDL does not list — the index is into that "
           "member's own type list, which is what its declaration passed");
    DCHECK(index >= m->first_optional,
           "a REQUIRED argument was given a default value — §3.6 step 15.4 reads one only for an optional "
           "argument, so idl_optional_from is stated before the defaults the optional positions carry");
    DCHECK(dflt != IDL_DEFAULT_NONE,
           "an argument default declared IDL_DEFAULT_NONE, which is the ABSENCE of a default — that is what a "
           "position with no declaration already is, and saying it here means the IDL was misread");
    if (m->arg_dflts == NULL) {
        m->arg_dflts = calloc((size_t)m->nargs, sizeof *m->arg_dflts);
        CHECK(m->arg_dflts != NULL,
              "idl: OOM recording a member's declared argument defaults — a member that cannot be declared is "
              "an API the page cannot call");
    }
    m->arg_dflts[index].kind = dflt;
    m->arg_dflts[index].str  = dflt_str;
}

/* §3.2.26 Buffer source types STEP 1's `T` AT ONE POSITION, plus the two §3.3 extended attributes its steps 3
   and 4 turn on — see idl_args.h. Same "names the last declaration" rule as idl_optional_from, and it is
   PER POSITION rather than per member because a member's IDL may write several: Web Audio API §1.13.3
   Methods declares `getFrequencyResponse(Float32Array frequencyHz, Float32Array magResponse, Float32Array
   phaseResponse)` on the BiquadFilterNode interface, which is three on one line. */
void idl_typed_array(int index, JSTypedArrayEnum kind, bool allow_shared, bool allow_resizable)
{
    IdlMember *m;

    DCHECK(g_n > 0, "a typed array was declared before any member was");
    DCHECK(!g_sealed, IDL_LAST_DECL_ONLY);
    m = idl_member(g_n - 1);
    DCHECK(index >= 0 && index < m->nargs,
           "a typed array named a position the member's IDL does not list — the index is into that member's "
           "own type list, which is what its declaration passed");
    /* THE POSITION'S TYPE IS WHAT ASKS FOR A T, so stating one anywhere else is a declaration about a
       different position — and, if the member was declared elsewhere, about a different member entirely. The
       seal asserts the same pair from the other side, over every declaration at once. */
    DCHECK(m->types[index] == IDL_TYPED_ARRAY,
           "§3.2.26 step 1's T was stated at a position whose declared type is not IDL_TYPED_ARRAY — no other "
           "conversion reads it, so this describes a position that never asks for one");
    DCHECK(kind >= 0 && kind <= JS_TYPED_ARRAY_FLOAT64,
           "a typed array kind outside JSTypedArrayEnum was declared — §3.2.26's algorithm ranges over exactly "
           "the twelve the enumeration names");
    if (m->arg_views == NULL) {
        int k;
        m->arg_views = calloc((size_t)m->nargs, sizeof *m->arg_views);
        CHECK(m->arg_views != NULL,
              "idl: OOM recording a member's declared typed arrays — a member that cannot be declared is an "
              "API the page cannot call");
        /* Zero is a VALID enumerator (JS_TYPED_ARRAY_UINT8C), so "none stated" has to be a value outside the
           enumeration and the calloc cannot be the statement. Both readers — the conversion and the seal —
           test `kind >= 0`, so the two ask the same question. */
        for (k = 0; k < m->nargs; k++) m->arg_views[k].kind = -1;
    }
    /* Twice is two answers to one question, and only one of them decides every call. §3.3.2's own example
       writes BOTH attributes on one position, so `allow_shared && allow_resizable` is not the thing to
       refuse — restating the position is. */
    DCHECK(m->arg_views[index].kind < 0, "a position stated §3.2.26 step 1's T twice");
    m->arg_views[index].kind = (int16_t)kind;
    m->arg_views[index].allow_shared = allow_shared;
    m->arg_views[index].allow_resizable = allow_resizable;
}

/* See idl_args.h. Same "names the last declaration" rule as idl_optional_from, and it must be stated BEFORE
   idl_optional_from and idl_arg_default: both of those measure positions against `idl_declared_positions`,
   which a variadic tail shortens by one, so a member that said it afterwards would have had its optional
   index and its defaults checked against a list one longer than the one it declares. */
void idl_returns_promise(void)
{
    IdlMember *m;

    DCHECK(g_n > 0, "a promise return type was declared before any member was");
    DCHECK(!g_sealed, IDL_LAST_DECL_ONLY);
    m = idl_member(g_n - 1);
    DCHECK(!m->returns_promise, "a member declared its promise return type twice");
    m->returns_promise = true;
}

void idl_variadic(void)
{
    IdlMember *m;

    DCHECK(g_n > 0, "a variadic tail was declared before any member was");
    DCHECK(!g_sealed, IDL_LAST_DECL_ONLY);
    m = idl_member(g_n - 1);
    DCHECK(m->nargs >= 1,
           "a variadic member declared no argument types — the LAST one is the tail's type, which is what `T...` "
           "states, so there is always at least one");
    DCHECK(!m->variadic, "a member declared its variadic tail twice");
    m->variadic = true;
}

/* See idl_args.h. Same "names the last declaration" rule as idl_optional_from. */
void idl_iface_brand(JSClassID iface)
{
    DCHECK(g_n > 0, "an interface brand was declared before any member was");
    DCHECK(!g_sealed, IDL_LAST_DECL_ONLY);
    DCHECK(iface != 0, "an interface brand named no class — the class is half of what the type states");
    idl_member(g_n - 1)->iface = iface;
    idl_member(g_n - 1)->iface_narrow = NULL;   /* a fresh brand narrows to nothing until the member says so */
}

/* §3.2.18's `E` AT ONE POSITION — see idl_args.h for why there is no declaration-wide form left beside it.
   Same "names the last declaration" rule as idl_optional_from, and it is PER POSITION for the reason
   idl_arg_iface is: a member's IDL may write several, and Web Cryptography §14.3.9 The importKey method's two
   are two DIFFERENT enumerations. */
void idl_arg_enum(int index, const char *const *values)
{
    IdlMember *m;

    DCHECK(g_n > 0, "an enumeration's value list was declared before any member was");
    DCHECK(!g_sealed, IDL_LAST_DECL_ONLY);
    m = idl_member(g_n - 1);
    DCHECK(index >= 0 && index < m->nargs,
           "an enumeration's value list named a position the member's IDL does not list — the index is into "
           "that member's own type list, which is what its declaration passed");
    /* THE POSITION'S TYPE IS WHAT ASKS FOR AN ENUMERATION, so stating one anywhere else is a declaration about
       a different position — and, if the member was declared elsewhere, about a different member entirely. The
       seal asserts the same pair from the other side, over every declaration at once. */
    DCHECK(idl_type_admits_enumeration(m->types[index]),
           "§3.2.18's values were stated at a position whose declared type is not an enumeration — no "
           "conversion reads them, so the declaration describes a position that never tests against them");
    DCHECK(values != NULL && values[0] != NULL,
           "an enumeration declared an empty value list — every §3.2.18 enumeration has at least one value, and "
           "a member whose type admits nothing is a member no assignment can satisfy");
    if (m->arg_enums == NULL) {
        m->arg_enums = calloc((size_t)m->nargs, sizeof *m->arg_enums);
        CHECK(m->arg_enums != NULL,
              "idl: OOM recording a member's per-position enumeration values — a member that cannot be "
              "declared is an API the page cannot call");
    }
    /* Twice is two answers to one question, and only one of them decides every call — the same refusal
       idl_arg_iface and idl_typed_array make, and for the same reason: it is restating the POSITION that is
       wrong, not what was said about it. */
    DCHECK(m->arg_enums[index] == NULL, "a position stated §3.2.18's value list twice");
    m->arg_enums[index] = values;
}

void idl_iface_narrow(bool (*is)(JSValueConst v))
{
    DCHECK(g_n > 0, "an interface narrowing was declared before any member was");
    DCHECK(!g_sealed, IDL_LAST_DECL_ONLY);
    DCHECK(idl_member(g_n - 1)->iface != 0,
           "an interface narrowing was declared for a member with no class brand — it narrows a brand, so the "
           "brand has to be there to narrow");
    DCHECK(is != NULL, "an interface narrowing named no predicate");
    idl_member(g_n - 1)->iface_narrow = is;
}

/* §3.2.15's `I` AT ONE POSITION — see idl_args.h for the two shapes of member that need it and for why its
   predicate takes a realm where idl_iface_narrow's does not. Same "names the last declaration" rule as
   idl_optional_from, and it is PER POSITION rather than per member for the reason idl_typed_array is: a
   member's IDL may write several, and initMouseEvent's two are two DIFFERENT interfaces. */
void idl_arg_iface(int index, bool (*is)(JSContext *ctx, JSValueConst v), const char *iface)
{
    IdlMember *m;

    DCHECK(g_n > 0, "a position's interface was declared before any member was");
    DCHECK(!g_sealed, IDL_LAST_DECL_ONLY);
    m = idl_member(g_n - 1);
    DCHECK(index >= 0 && index < m->nargs,
           "a position's interface named a position the member's IDL does not list — the index is into that "
           "member's own type list, which is what its declaration passed");
    /* THE POSITION'S TYPE IS WHAT ASKS FOR AN INTERFACE, so stating one anywhere else is a declaration about a
       different position — and, if the member was declared elsewhere, about a different member entirely. The
       seal asserts the same pair from the other side, over every declaration at once. */
    DCHECK(idl_type_brands_interface(m->types[index]),
           "§3.2.15's interface was stated at a position whose declared type asks for no brand — no conversion "
           "reads it, so the declaration describes a position that never tests against it");
    DCHECK(is != NULL,
           "a position's interface named no predicate — §3.2.15 step 1 is \"If V implements I\", and the "
           "component that owns the interface is the only thing that can answer it");
    DCHECK(iface != NULL && *iface,
           "a position's interface named a predicate and no identifier — the identifier is the SUBJECT of the "
           "TypeError §3.2.15 throws, so a page told only that `the declared interface` was not implemented "
           "learns nothing it did not already know");
    if (m->arg_ifaces == NULL) {
        m->arg_ifaces = calloc((size_t)m->nargs, sizeof *m->arg_ifaces);
        CHECK(m->arg_ifaces != NULL,
              "idl: OOM recording a member's per-position interfaces — a member that cannot be declared is an "
              "API the page cannot call");
    }
    /* Twice is two answers to one question, and only one of them decides every call — the same refusal
       idl_typed_array makes, and for the same reason: it is restating the POSITION that is wrong, not the
       combination of what was said. */
    DCHECK(m->arg_ifaces[index].is == NULL, "a position stated §3.2.15's interface twice");
    m->arg_ifaces[index].is = is;
    m->arg_ifaces[index].iface = iface;
}

/* WEB IDL §3.7 Interfaces' implementation-check an object, its `interface` INPUT — see idl_args.h for why this
   is a different question from idl_iface_brand's and why it is a predicate rather than a class. */
void idl_this_iface(bool (*is)(JSValueConst v), const char *iface)
{
    DCHECK(g_n > 0, "a receiver interface was declared before any member was");
    DCHECK(!g_sealed, IDL_LAST_DECL_ONLY);
    DCHECK(is != NULL,
           "a receiver interface named no predicate — Web IDL §3.7 Interfaces' implementation-check step 3 is "
           "\"if object does not implement interface\", and the component that owns the interface is the only "
           "thing that can answer it");
    DCHECK(iface != NULL && *iface,
           "a receiver interface named no identifier — the TypeError §3.7 Interfaces' implementation-check "
           "step 3 throws says which interface the receiver failed to implement, and a member that cannot say "
           "sends its reader to grep for the predicate instead");
    /* ONE STATEMENT PER MEMBER. A member is DECLARED once (idl_method_id_all) however many realms mint it, so a
       second declaration here is two answers to §3.7's one `interface` input — the shape idl_mint_step already
       refuses for §3.5's `type`, asked at the other end of the same algorithm. */
    DCHECK(idl_member(g_n - 1)->this_is == NULL,
           "one member declared its receiver interface twice — Web IDL §3.7 Interfaces' implementation-check "
           "takes ONE `interface`, so a second is a second answer and the first one silently wins");
    idl_member(g_n - 1)->this_is = is;
    idl_member(g_n - 1)->this_iface = iface;
}

/* WEB IDL §3.3.10 [PutForwards]'s FORWARDING HALF, WHICH IS A BINDING RULE AND NOT A COMPONENT'S ALGORITHM.
 *
 * §3.3.10 [PutForwards] says what the extended attribute MEANS — "the assignment is 'forwarded' to the
 * attribute (specified by the extended attribute argument) on the object that is currently referenced by the
 * attribute being assigned to" — and §3.7.6 Attributes says what a binding DOES about it, as five steps of
 * "create an attribute setter":
 *     4.5.8.1  Let Q be ? Get(jsValue, id).
 *     4.5.8.2  If Q is not an Object, then throw a TypeError.
 *     4.5.8.3  Let forwardId be the identifier argument of the [PutForwards] extended attribute.
 *     4.5.8.4  Perform ? Set(Q, forwardId, V, false).
 *     4.5.8.5  Return undefined.
 * Not one word of that is about Location, or about a declaration block, or about a MediaList. The two operands
 * — `id` and `forwardId` — are the ONLY thing that differs between the attributes that carry it, which is why
 * this is ONE machine parameterised by a pair and not one body per carrier. It had been the other shape: two
 * components had hand-written the same five steps over their own two names, both of them citing a section
 * number that is [LegacyNamespace] in the edition they were written against, both of them writing 4.5.8.4's
 * flag as `true` where the spec writes `false`, and a third and fourth carrier had no forwarding at all.
 *
 * AND IT IS A STEP MACHINE BECAUSE 4.5.8.4 RUNS THE PAGE'S CODE — twice over, and one of the two is this
 * engine's own. The Get at 4.5.8.1 reaches an accessor or a Proxy trap; the Set at 4.5.8.4 reaches the
 * forwarded-to attribute's SETTER, and HTML §7.2.4 The Location interface's `href` setter is a machine that
 * NAVIGATES — it suspends inside its own assignment while a `navigate` event listener runs. A plain C body
 * performing either with JS_GetPropertyStr/JS_SetPropertyStr is a C activation hosting the page's loops, which
 * is the drive-to-completion this engine aborts on, and against a step-machine setter it is the abort itself.
 *
 * WHAT IT DOES NOT PERFORM IS §3.7.6 steps 4.5.2-4.5.4 — the security check, and the `validThis` TypeError for
 * a receiver that does not implement the attribute's interface. `idl_attribute_this` above is that pair, and it
 * is Window-only by construction (window_proxy.c owns the brand, and the two accessors this file mints itself
 * are all installed on the realm's global); a [PutForwards] carrier is declared on Window, on Document, on
 * Element and on four CSSRule interfaces, so the check needs the RECEIVER'S BRAND to be a fact the pool holds.
 * THE NEXT DIFF IS THAT: a receiver-brand on the declaration, the way idl_iface_brand already carries an
 * ARGUMENT's, consumed here and by idl_attribute_this alike. ITS ABSENCE SHOWS as the WRONG TypeError for a
 * receiver that implements neither interface (4.5.8.2's "not an Object" instead of 4.5.4's, since the Get of a
 * member the receiver does not have answers undefined), and as NO error at all for the one receiver shape that
 * has the property without implementing the target —
 * `Object.getOwnPropertyDescriptor(Document.prototype, "location").set.call(window, u)`, which the spec
 * refuses at 4.5.4 and this forwards.
 *
 * THE STAGES ARE §3.7.6's OWN, one spec step each, and the two requests are why: a machine that could not park
 * at 4.5.8.1 and again at 4.5.8.4 would be driving both to completion. */
#define IDL_PF_STAGES(X)                                                                                      \
    X(IDL_PF_ENTER, "Web IDL §3.7.6 Attributes, create an attribute setter steps 4.1-4.2, 4.5.1 and 4.5.8 (V " \
                    "is the first argument passed; jsValue is the this value, or the realm's global object "   \
                    "when that is null or undefined; the attribute's [PutForwards] branch is taken)")         \
    X(IDL_PF_GET,   "Web IDL §3.7.6 Attributes, create an attribute setter step 4.5.8.1 (let Q be "            \
                    "? Get(jsValue, id)), whose step 4.5.8.2 refusal of a non-Object Q is on its answer")      \
    X(IDL_PF_SET,   "Web IDL §3.7.6 Attributes, create an attribute setter step 4.5.8.4 (perform "             \
                    "? Set(Q, forwardId, V, false))")
enum { IDL_STEP_STAGE_BASE(IDL_PF_STAGES) IDL_PF_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const IDL_PF_STEPS[] = { IDL_PF_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* §3.7.6's `id` and §3.3.10's identifier argument, INTERNED AT THE DECLARATION. A keyed request is handed its
   atom twice — once to park on it and once at the call site that collects the answer — with a suspension in
   between, so it cannot be created per assignment; and the two names are static strings known when the
   carrier declares itself. The pair is what a magic names, exactly as a member table's index is elsewhere in
   this engine, so ONE machine serves every carrier without a table of "who is special". */
typedef struct {
    JSAtom attr;      /* §3.7.6's `id`: the attribute the assignment was made to */
    JSAtom forward;   /* §3.3.10's identifier argument: the attribute on Q that receives it */
} IdlPutForwards;
static IdlPutForwards *g_pf;
static int             g_pf_n;

static const IdlPutForwards *idl_put_forwards_of(int magic)
{
    DCHECK(magic >= 0 && magic < g_pf_n,
           "a [PutForwards] setter ran with a magic that names no declared pair — the magic IS the index into "
           "the pair list this file declares, so an out-of-range one means the setter id outlived the pool the "
           "pair was declared into (an agent teardown) or was minted by something other than "
           "idl_setter_id_put_forwards");
    return &g_pf[magic];
}

/* WHAT THE MACHINE HOLDS ACROSS ITS SUSPENSIONS: §3.7.6's `jsValue` and its `Q`. The PAIR is the declaration's
   and V is the invocation's `argv[0]`, so neither of those is this state's to own. `jsValue` is here rather
   than re-derived at the Get because window_proxy_this_object ANSWERS OWNED and step 4.5.1 is computed once —
   re-running it per entry would take a second reference to this realm's global on every resume. */
typedef struct {
    JSValue js_value;   /* §3.7.6 step 4.5.1's `jsValue` */
    JSValue q;          /* §3.7.6 step 4.5.8.1's `Q` */
} IdlPutForwardsState;

static void idl_put_forwards_visit(JSContext *ctx, void *state, JSStepVisit *v)
{
    IdlPutForwardsState *s = state;

    v->val(ctx, &s->js_value);
    v->val(ctx, &s->q);
}

static int idl_put_forwards_body(JSContext *ctx, JSStepHdr *hdr, void *state, int argc, JSValueConst *argv,
                                 JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    IdlPutForwardsState *s = state;
    const IdlPutForwards *pf = idl_put_forwards_of(idl_step_magic(hdr));
    int r;

    STEP_DISPATCH(IDL_PF_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(IDL_PF_ENTER);
    /* Nothing has been asked for yet, so this entry's request answer belongs to nobody. */
    JS_FreeValue(ctx, cb_result);
    /* EVERY OWNED FIELD IN PLACE BEFORE THE FIRST THING THAT CAN THROW — the failure path tears this state
       down through the declaration, which frees exactly what `visit` names and nothing else. */
    s->js_value = JS_UNDEFINED;
    s->q = JS_UNDEFINED;
    /* §3.7.6 steps 4.1-4.2: "Let V be undefined. If any arguments were passed, then set V to the value of the
       first argument passed." A zero-argument call is spec-legal (`desc.set.call(obj)`) and needs no branch
       here: `argc` is the DECLARATION's position count and not the caller's, so the pool has already placed
       the undefined at argv[0] for one. */
    DCHECK(argc >= 1 && argv != NULL,
           "a [PutForwards] setter's body ran with no value — a step setter is delivered as a ONE-ARGUMENT "
           "call whose single declared position the pool always places, so the assigned value is argv[0]");
    /* STEP 4.5.1: "Let jsValue be the this value, if it is not null or undefined, or realm's global object
       otherwise." It is the SAME sentence idl_attribute_this resolves for the two accessors this file mints,
       and it is universal — every regular attribute's setter carries it, not only a [Global] interface's — so
       `Object.getOwnPropertyDescriptor(window, "location").set.call(null, u)` forwards through this realm's
       global rather than throwing on a read of undefined. What this does NOT then do is 4.5.2-4.5.4; see the
       machine's own paragraph for the brand the pool would have to hold and what its absence looks like. */
    s->js_value = window_proxy_this_object(ctx, hdr->this_val);
    STEP_GOTO(hdr->stage, IDL_PF_GET, &hdr->get_phase, NULL);
    return JS_STEP_YIELD;

    STEP_ARM(IDL_PF_GET);
    /* STEP 4.5.8.1. A REAL PROPERTY GET BY NAME, which is what makes one machine serve every carrier and what
       makes the cross-origin case work: `otherWindow.location = u` reads §7.2.1 Security infrastructure for
       Window, WindowProxy, and Location objects' cross-origin `location` off the WindowProxy, and whatever
       answers it is what the write below lands on. */
    r = step_getprop_run(ctx, hdr, s->js_value, pf->attr, cb_result, &s->q, out_cb, out_argc);
    if (r) return r < 0 ? JS_STEP_ABRUPT : r;
    /* STEP 4.5.8.2, on the answer: "If Q is not an Object, then throw a TypeError." It is NOT unreachable and
       must not be a DCHECK — an attribute this forwards through is [LegacyUnforgeable] on Window and on
       Document and is NOT on Element or on a CSSRule, so a page that redefines `style` to answer a primitive
       reaches this line. */
    if (!JS_IsObject(s->q)) {
        JS_ThrowTypeError(ctx, "the attribute this assignment forwards to did not answer with an object");
        return JS_STEP_ABRUPT;
    }
    STEP_GOTO(hdr->stage, IDL_PF_SET, &hdr->get_phase, NULL);
    return JS_STEP_YIELD;

    STEP_ARM(IDL_PF_SET);
    /* STEPS 4.5.8.3-4.5.8.4: `forwardId` is the pair's second half, and the write is 7.3.4 Set with Throw
       FALSE — a target that REFUSES the write is not an error this forwarding invents. This is where HTML
       §7.2.4 The Location interface's `href` setter runs, so this member suspends here for the whole of a
       navigation: its `navigate` event, its listeners, and §7.4.6.2's `popstate`. */
    r = step_setprop_bare_run(ctx, hdr, s->q, pf->forward, argv[0], cb_result, out_cb, out_argc);
    if (r) return r < 0 ? JS_STEP_ABRUPT : r;
    /* STEP 4.5.8.5. Set on the entry that FINISHES, never once in the head: `presult` is an out-parameter of
       EACH entry, so a member that wrote it before it parked would leave the resumed entry's answer as
       whatever the driver's slot held. */
    *presult = JS_UNDEFINED;
    return JS_STEP_DONE;
}

/* THERE IS NO `release`. Everything this state holds is the two JSValues the visit above names, so the teardown
   frees them through that list — and a second list here is what idl_args.h's ownership contract refuses. */
static const IdlStepDecl IDL_PUT_FORWARDS_DECL = {
    idl_put_forwards_body, sizeof(IdlPutForwardsState), idl_put_forwards_visit, NULL,
    "Web IDL §3.7.6 Attributes, create an attribute setter step 4.5.8 (the §3.3.10 [PutForwards] forwarding)",
    IDL_PF_STEPS
};

int idl_setter_id_put_forwards(JSContext *ctx, const char *attr_id, const char *forward_id)
{
    IdlPutForwards *grown;

    DCHECK(attr_id != NULL && *attr_id && forward_id != NULL && *forward_id,
           "a [PutForwards] setter was declared with an empty identifier — §3.3.10 takes an identifier and "
           "§3.7.6 step 4.5.8.1 reads the attribute back BY THAT NAME, so a missing one is a get of the empty "
           "string on the receiver rather than of the member");
    DCHECK(strcmp(attr_id, forward_id) != 0,
           "a [PutForwards] setter forwards an attribute to ITSELF — §3.3.10 forbids a cycle in a chain of "
           "forwarded assignments outright, and the one-link cycle is the only one a single declaration can "
           "state; this one would re-enter its own setter until the heap ran out");
    grown = realloc(g_pf, sizeof(*g_pf) * (size_t)(g_pf_n + 1));
    CHECK(grown != NULL, "OOM growing the [PutForwards] pair list");
    g_pf = grown;
    g_pf[g_pf_n].attr    = JS_NewAtom(ctx, attr_id);
    g_pf[g_pf_n].forward = JS_NewAtom(ctx, forward_id);
    CHECK(g_pf[g_pf_n].attr != JS_ATOM_NULL && g_pf[g_pf_n].forward != JS_ATOM_NULL,
          "a [PutForwards] attribute's own name would not intern");
    g_pf_n++;
    /* IDL_ANY, and that is §3.7.6's ORDER rather than a convenience: step 4.5.8 returns BEFORE step 4.6's
       "converting V to an IDL value of attribute's type" ever runs, so a forwarded assignment hands the
       forwarded-to setter the page's own value and THAT member's declared type is what converts it. Converting
       here would coerce twice, and for `location` the first coercion would be to the type of an attribute
       (`Location`) that no assignment is ever converted to at all. */
    return idl_setter_id_step(ctx, IDL_ANY, /*null_to_empty*/ false, &IDL_PUT_FORWARDS_DECL, g_pf_n - 1);
}

int idl_setter_id_step(JSContext *ctx, IdlArgType type, bool null_to_empty, const IdlStepDecl *decl, int magic)
{
    int id = idl_method_id_step(ctx, &type, 1, NULL, 0, decl, magic);
    /* the pool entry idl_method_id_step just filled. A step setter is delivered as a ONE-ARGUMENT call, so its
       body reads argv[0]; what it needs from the setter form is the type's null rule. */
    idl_member(g_n - 1)->null_to_empty = null_to_empty;
    return id;
}

int idl_step_magic(const JSStepHdr *hdr)
{
    DCHECK(hdr->arg >= 0 && hdr->arg < g_n, "a step body asked for its magic with no pool entry behind it");
    return idl_member(hdr->arg)->magic;
}

void idl_active_ctor_owed(JSContext *ctx, JSStepHdr *hdr, JSValueConst ctor)
{
    /* The header is this state's FIRST field — the same identity every read of `hdr->arg` in this file already
       relies on — so a body that holds its header holds its machine. */
    JSIdlArgsState *s = (JSIdlArgsState *)hdr;

    DCHECK(hdr->arg >= 0 && hdr->arg < g_n,
           "a step body declared an active custom element constructor map entry with no pool entry behind it");
    /* NOT `JS_IsUndefined` — see the teardown's own read for why a zeroed state cannot be asked that way. */
    DCHECK(!JS_IsObject(s->ace_ctor),
           "one IDL invocation entered §4.13.4's active custom element constructor map TWICE without leaving. "
           "The pair nests one deep per invocation — DOM §4.9 create an element step 5.1's bracket is the only "
           "one a declared member enters directly, and it brackets a single Construct — so a second entry means "
           "the first is about to be given back for the wrong constructor, and HTML §3.2.3 \"HTML element "
           "constructors\" step 3 would then answer some other algorithm's registry");
    s->ace_ctor = JS_DupValue(ctx, ctor);
}

int idl_setter_id(JSContext *ctx, IdlArgType type, bool null_to_empty, IdlSetter body, int magic)
{
    int id = idl_method_id(ctx, &type, 1, NULL, magic);
    /* the pool entry idl_method_id just filled — a setter differs only in which body it runs and in the
       null-to-empty rule its type carries. */
    idl_member(g_n - 1)->setter        = body;
    idl_member(g_n - 1)->null_to_empty = null_to_empty;
    return id;
}

int idl_getter_id_step(JSContext *ctx, const IdlStepDecl *decl, int magic)
{
    return idl_method_id_step(ctx, NULL, 0, NULL, 0, decl, magic);
}

#if APICLIENT_DEV
#include "idl_inheritance.h"   /* GENERATED from @webref/idl — §3.7.3's proto step, per interface */

static int idl_inherits_cmp(const void *iface, const void *row)
{
    return strcmp((const char *)iface, ((const IdlInherits *)row)->iface);
}

/* §3.7.3 Interface prototype object's PROTO STEP, ASSERTED — the one fact about this engine that the Web IDL
 * gap audit STANDS ON and that nothing, until this, established.
 *
 * §3.7.3 says it in one sentence: "Otherwise, if interface is declared to inherit from another interface, then
 * set proto to the interface prototype object IN REALM of that inherited interface", and then
 * "Set interfaceProtoObj to OrdinaryObjectCreate(proto)". Three other arms cover the rest — a [Global]
 * interface that supports named properties takes §3.7.4 Named properties object's object, DOMException takes
 * %Error.prototype%, everything else %Object.prototype%.
 *
 * WHY IT IS ASSERTED HERE RATHER THAN CHECKED BY THE AUDIT. engine/idlgen.mjs credits a BASE's installed
 * members to every interface that inherits it — `addEventListener` counts for HTMLSpanElement because the IDL
 * says HTMLSpanElement inherits EventTarget — so a prototype built over the WRONG parent reads COMPLETE for
 * every member of the parent the IDL names while a page reaches none of them. That is ~64 members on one row
 * and ~2900 across the HTML family: the largest false COMPLETE the auditor could mint, and it would mint it
 * silently, because the IDL side and the installed side are each read correctly and only the LINK between them
 * was believed. The link is a RUNTIME fact, PER REALM (a member answers out of the realm that defined it, so a
 * chain that is right in the agent's first realm and wrong in a child navigable's is two different answers),
 * so a static approximation of it would be a second plausible answer beside the first rather than a check.
 *
 * IT COSTS NOTHING TO REACH, because §3.7.3's own last line already put the answer on the object: "The class
 * string of an interface prototype object is the interface's qualified name". So the parent IDENTIFIES ITSELF,
 * with the tag this very function writes, and the check is the tag read back one link up. The two arms that
 * name an INTRINSIC carry no class string at all, so those compare object identity against the realm's own
 * %Object.prototype% / %Error.prototype% instead — which is stricter than "carries no tag" and is what stops a
 * prototype parented to some untagged record from passing.
 *
 * ORDER IS PART OF THE INVARIANT AND IS WHY THIS FIRES RATHER THAN WARNS. core/realm.h already states that the
 * per-realm intrinsics run in DECLARATION order because that is the dependency order — "Event declares before
 * MessageEvent, so a realm's Event.prototype exists before the prototype that chains to it". A component that
 * declares itself too early builds its prototype over a parent that does not exist yet, and until now the
 * result was a silently orphaned chain; now it is an abort naming both interfaces.
 *
 * IT READS THE LINK AS §3.7.3 ESTABLISHES IT — AT CONSTRUCTION — AND THAT IS NOT A DETAIL OF WHERE THE CHECK
 * SITS. §3.7.3 resolves proto and then OrdinaryObjectCreate(proto)s the object, so an interface prototype
 * object is never observable outside its chain. Fourteen components instead built the object bare, tagged it,
 * and re-parented it with a JS_SetPrototype a few lines later; every one of those chains was CORRECT by the
 * time the realm finished, and every one of them would have aborted here, because the question was being asked
 * in the window the spec does not leave open. The answer was to close the window rather than to move the
 * question — a check that waits until the realm is finished cannot name the component that built the object,
 * and a component that re-parents can be read by anything that runs in between. The name of that fix is
 * event_target_derived_proto: the parent is resolved FIRST and the object is created over it.
 *
 * EVERY MESSAGE NAMES THE INTERFACE, because this function is reachable from every interface prototype object
 * in the engine and a DCHECK stamps the file and line it is WRITTEN at — so without the identifier the abort
 * would report this one line for all of them and its remedy would name an action with no object. */
static void idl_assert_inherits(JSContext *ctx, JSValueConst proto, const char *iface)
{
    const IdlInherits *row;
    JSValue parent, tag = JS_UNDEFINED, want;
    int got;

    row = bsearch(iface, IDL_INHERITS, sizeof IDL_INHERITS / sizeof IDL_INHERITS[0],
                  sizeof IDL_INHERITS[0], idl_inherits_cmp);
    DCHECKF(row != NULL,
            "the interface prototype object tagged \"%s\" carries a class string @webref/idl declares no "
            "interface for — §3.7.3's proto step has no arm for it, so nothing can say what this object's "
            "[[Prototype]] must be and every member the audit credits it by inheritance is credited on unread "
            "ground. Either the identifier is misspelt, or idl_inheritance.h is stale: regenerate it with "
            "`node engine/idlgen.mjs --regen` and commit", iface);

    parent = JS_GetPrototype(ctx, proto);
    DCHECKF(JS_IsObject(parent),
            "the interface prototype object of \"%s\" was built with a NULL [[Prototype]] — §3.7.3's proto step "
            "asserts \"proto is an Object\" on every one of its four arms, and an object at the end of no chain "
            "reaches neither its base's members nor Object.prototype's", iface);

    if (row->arm == IDL_PROTO_INHERITS) {
        const char *have;

        got = JS_GetOwnSlot(ctx, &tag, parent, JS_WellKnownSymbolAtom(JS_WKS_TO_STRING_TAG));
        CHECK(got >= 0, "reading a §3.7.3 class string off an interface prototype object's [[Prototype]] threw "
                        "— the tag is a plain data property this same function defines, so there is no getter "
                        "to run and nothing to throw with");
        DCHECKF(got == 1,
                "the [[Prototype]] of \"%s\"'s interface prototype object carries NO §3.7.3 class string, so it "
                "is not the interface prototype object of \"%s\", which is the only object §3.7.3's proto step "
                "names. Either this component built it over the wrong thing, or it built it BARE and re-parents "
                "it further down — §3.7.3 resolves proto first and OrdinaryObjectCreate()s over it, so the "
                "object must never exist unchained", iface, row->proto);
        have = JS_ToCString(ctx, tag);
        CHECK(have != NULL, "a §3.7.3 class string could not be read as a C string");
        DCHECKF(strcmp(have, row->proto) == 0,
                "the [[Prototype]] of \"%s\"'s interface prototype object is \"%s\"'s, and the IDL says it must "
                "be \"%s\"'s — §3.7.3's proto step is \"the interface prototype object in realm of that "
                "inherited interface\". engine/idlgen.mjs credits \"%s\" with every member installed on \"%s\", "
                "so all of them now read COMPLETE and no page can reach one of them",
                iface, have, row->proto, iface, row->proto);
        JS_FreeCString(ctx, have);
        JS_FreeValue(ctx, tag);
        JS_FreeValue(ctx, parent);
        return;
    }

    /* §3.7.3's two INTRINSIC arms. Identity, not absence-of-tag: an untagged record would pass the weaker
       test, and a prototype parented to one reaches Object.prototype's members through nothing. */
    want = JS_GetClassProto(ctx, row->arm == IDL_PROTO_ERROR ? JS_CLASS_ERROR : JS_CLASS_OBJECT);
    DCHECKF(JS_IsSameValue(ctx, parent, want),
            "\"%s\" inherits from no interface, so §3.7.3's proto step is this realm's %%Object.prototype%% "
            "(%%Error.prototype%% for DOMException) and its interface prototype object's [[Prototype]] is some "
            "other object — either the component built it over the wrong thing, or it was built in a realm "
            "other than the one installing it, which is the shared-prototype defect core/realm.h exists to "
            "prevent", iface);
    JS_FreeValue(ctx, want);
    JS_FreeValue(ctx, parent);
}
#endif

/* THE WRITE ITSELF, shared by the two statements below. §3.7.3 gives the property { writable: false,
   enumerable: false, configurable: true }, which is JS_PROP_CONFIGURABLE and nothing else.
   IT IS A PRIVATE HELPER AND NOT ONE OF THEM CALLING THE OTHER, because engine/idl_installed.mjs reads the two
   public calls as the SEED that says which definition an object's members belong to: a delegation would put a
   seed call inside a function whose `iface` is a parameter rather than a literal, which is an install target
   the auditor honestly cannot decide — its own category, reported, and caused entirely by how this file was
   spelled rather than by anything in the engine. */
static void idl_tag_write(JSContext *ctx, JSValueConst obj, const char *iface)
{
    JS_DefinePropertyValue(ctx, (JSValue)obj, JS_DupAtom(ctx, JS_WellKnownSymbolAtom(JS_WKS_TO_STRING_TAG)),
                           JS_NewString(ctx, iface), JS_PROP_CONFIGURABLE);
}

/* §3.7.3: EVERY INTERFACE PROTOTYPE OBJECT CARRIES @@toStringTag, whose value is the interface's IDENTIFIER and
   whose attributes are { writable: false, enumerable: false, configurable: true }. It is what makes
   `Object.prototype.toString.call(new Blob())` answer "[object Blob]" — the brand check a page performs without
   `instanceof`, and the one wpt's own assert_class_string makes about every interface it touches.
   NOT ONE INTERFACE IN THIS ENGINE HAD IT. Every one of them answered "[object Object]", which is Web IDL's rule
   missed twenty-two times over — the shape a per-component rule always ends up in, and why this is one call the
   interface makes rather than a line each of them remembers.
   IT IS ALSO WHERE §3.7.3's PROTO STEP IS ASSERTED, and the two belong at one call for the same reason: the tag
   is what makes the [[Prototype]] link CHECKABLE, so the object that writes the tag is the object that can read
   the one above it. The assertion runs BEFORE the write, so a chain built wrong aborts naming the interface it
   was about to become rather than after it already is one. */
void idl_interface_tag(JSContext *ctx, JSValueConst proto, const char *iface)
{
    DCHECK(JS_IsObject(proto), "an interface's @@toStringTag was installed on something that is not an object");
    DCHECK(iface != NULL && *iface, "an interface prototype object was tagged with no identifier — §3.7.3 makes "
                                    "the class string the interface's qualified name, so there is nothing to "
                                    "write and nothing below can name what this object's [[Prototype]] must be");
#if APICLIENT_DEV
    idl_assert_inherits(ctx, proto, iface);
#endif
    idl_tag_write(ctx, proto, iface);
}

/* §3.7.3's CLASS STRING ON AN OBJECT THAT IS NOT AN INTERFACE PROTOTYPE OBJECT — the write above without the
   proto-step assertion above it, because there is exactly one object in this engine that must carry an
   interface's class string while §3.7.3 says nothing about its [[Prototype]].
   THE OBJECT IS §7.2.3 The WindowProxy exotic object's PROTOTYPE, and §7.2.3 is explicit in one sentence:
   "There is no WindowProxy interface object." A WindowProxy answers out of the [[Window]] it wraps, so the
   class string a page reads off it is WINDOW's identifier — which is also what §3.7.3's tag would say — and
   the two objects are still not the same object: core/frame/window.c builds the REAL §3.7.3 Window interface
   prototype object over §3.7.4's named properties object, which is what IDL_INHERITS states and what the
   assertion above checks. Sending this object through the same call would ask §3.7.3's proto-step question
   about an object §3.7.3 does not define, and get a confident wrong answer.
   IT IS DELIBERATELY ITS OWN FUNCTION rather than a flag, for the reason idl_namespace_tag and
   idl_async_iterator_tag are: which KIND of object is being tagged is a fact the C states, and
   engine/idl_installed.mjs reads the statement rather than guessing it from a name — both forms seed the same
   attribution table, so the fifty members installed on this object are still credited to Window.
   RESIDUAL, NAMED: this says only that §3.7.3's proto step does not govern this object; it does not yet say
   what DOES. §7.2.3.1 [[GetPrototypeOf]] returns OrdinaryGetPrototypeOf(W) for a same-origin WindowProxy, so
   `Object.getPrototypeOf(otherWindow)` should reach the same Window.prototype window.c builds, and this engine
   gives WindowProxy instances a SECOND object in that position. The next diff makes the two one object rather
   than two, and its absence shows as a cross-origin-capable page finding
   `Object.getPrototypeOf(frames[0]) !== Window.prototype` while both answer "[object Window]". */
void idl_class_string(JSContext *ctx, JSValueConst obj, const char *iface)
{
    DCHECK(JS_IsObject(obj), "a §3.7.3 class string was installed on something that is not an object");
    DCHECK(iface != NULL && *iface, "a §3.7.3 class string was written with no identifier — the class string IS "
                                    "the interface's qualified name, so there is nothing to write");
    idl_tag_write(ctx, obj, iface);
}

/* See idl_args.h. §3.13.1's class string on a NAMESPACE object — the same §3.2 descriptor as an interface
   prototype's tag, said through its own function because the object it is said about is a different kind and
   the auditor that reads these statements must be able to tell them apart. */
void idl_namespace_tag(JSContext *ctx, JSValueConst ns, const char *identifier)
{
    DCHECK(JS_IsObject(ns), "a §3.13.1 class string was installed on something that is not an object");
    DCHECK(identifier != NULL && *identifier,
           "a namespace object was tagged with no identifier — §3.13.1 makes the class string the NAMESPACE's "
           "identifier, so there is nothing to write");
    JS_DefinePropertyValue(ctx, (JSValue)ns, JS_DupAtom(ctx, JS_WellKnownSymbolAtom(JS_WKS_TO_STRING_TAG)),
                           JS_NewString(ctx, identifier), JS_PROP_CONFIGURABLE);
}

/* §3.7.10.2's ASYNCHRONOUS ITERATOR PROTOTYPE OBJECT is NOT an interface prototype object, and what it carries
   is nobody's IDL member: §3.7.10.2 defines `next`, and `return` when the interface has an asynchronous
   iterator return algorithm, and Web IDL defines nothing else on one. Its class string is "the result of
   concatenating the identifier of the interface and the string ' AsyncIterator'", and the composition lives
   here — one place, so it cannot be spelled two ways, and so that an object whose properties are nobody's
   members is DISTINGUISHABLE from an object the Web IDL gap audit merely failed to attribute. */
void idl_async_iterator_tag(JSContext *ctx, JSValueConst aproto, const char *iface)
{
    char name[128];

    DCHECK(JS_IsObject(aproto),
           "a §3.7.10.2 class string was installed on something that is not an object");
    DCHECK(iface != NULL && *iface,
           "a §3.7.10.2 asynchronous iterator prototype object was named for no interface — the class string "
           "IS the interface's identifier plus a suffix, so there is nothing to concatenate");
    snprintf(name, sizeof name, "%s AsyncIterator", iface);
    JS_DefinePropertyValue(ctx, (JSValue)aproto, JS_WellKnownSymbolAtom(JS_WKS_TO_STRING_TAG),
                           JS_NewString(ctx, name), JS_PROP_CONFIGURABLE);
}

/* §3.2.27's CREATE FROZEN ARRAY — see idl_args.h for why the preventExtensions half alone is not it. */
int idl_freeze_array(JSContext *ctx, JSValueConst arr)
{
    JSValue len_v;
    uint32_t n = 0, i;
    JSAtom k;
    int r;

    DCHECK(JS_IsArray(arr), "idl_freeze_array was handed something that is not an Array — a FrozenArray IS an "
                            "Array at §3.2.27's integrity level, and freezing anything else here would answer "
                            "a different type with the right name on it");
    len_v = JS_GetPropertyStr(ctx, arr, "length");
    if (JS_IsException(len_v)) return -1;
    r = JS_ToUint32(ctx, &n, len_v);
    JS_FreeValue(ctx, len_v);
    if (r < 0) return -1;
    /* Every own INDEX loses writable and configurable... */
    for (i = 0; i < n; i++) {
        char buf[16];
        snprintf(buf, sizeof buf, "%u", i);
        k = JS_NewAtom(ctx, buf);
        if (k == JS_ATOM_NULL) return -1;
        r = JS_DefineProperty(ctx, arr, k, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED,
                              JS_PROP_HAS_WRITABLE | JS_PROP_HAS_CONFIGURABLE);
        JS_FreeAtom(ctx, k);
        if (r < 0) return -1;
    }
    /* ...and so does `length`, which is the own property an array always has and the one a freeze that only
       prevented extensions left writable — `arr.length = 0` still emptied a "frozen" array. */
    k = JS_NewAtom(ctx, "length");
    if (k == JS_ATOM_NULL) return -1;
    r = JS_DefineProperty(ctx, arr, k, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_PROP_HAS_WRITABLE);
    JS_FreeAtom(ctx, k);
    if (r < 0) return -1;
    return JS_PreventExtensions(ctx, arr) < 0 ? -1 : 0;
}

/* The ONE mint and its §3.7.6 Attributes form, defined beside idl_step_function below because that is where
   every other minting form lives; declared here because the accessor installs reach them first. */
static JSValue idl_mint_accessor(JSContext *ctx, const char *name, int stepid, int expect);
/* And the ONE mint for an attribute whose getter is a plain C function — defined beside §3.7.6's receiver
   machinery below, because routing a global attribute's read through it is the whole of what it decides. */
static JSValue idl_mint_plain_getter(JSContext *ctx, JSValueConst target, const char *name,
                                     IdlGetter getter, int getter_magic);

/* IS THIS INSTALL PUTTING AN OWN PROPERTY ON THE REALM'S [Global] OBJECT? Web IDL §3.7.6's opening prose —
   "Regular attributes are exposed on the interface prototype object, unless the attribute is unforgeable or if
   the interface was declared with the [Global] extended attribute, in which case they are exposed on every
   object that implements the interface" — is what makes this question answer "which interface is `target`":
   Window is the only [Global] interface this engine has, so an own property of the global is a Window member.
   IT COMPARES THE REALM'S GLOBAL AND NEVER READS A `globalThis` PROPERTY. The binding is a writable data
   property a page may reassign, so a receiver or a target taken from it is not necessarily the Window;
   JS_GetGlobalObject answers the realm's own object, which is the value §3.7.6 means by "realm's global
   object". Side-effect-free: the reference it takes is given straight back. */
static bool idl_target_is_realm_global(JSContext *ctx, JSValueConst target)
{
    JSValue g = JS_GetGlobalObject(ctx);
    bool same = JS_VALUE_GET_PTR(g) == JS_VALUE_GET_PTR(target);

    JS_FreeValue(ctx, g);
    return same;
}

/* WEB IDL §3.7.6 Attributes' NAME FOR AN ACCESSOR'S FUNCTION OBJECT — the step every mint in this file used to
 * skip, and the reason it went unseen for as long as it did.
 *
 * §3.7.6 states it twice, once per algorithm, and the two sentences are the whole rule. The attribute getter
 * is created with "Let name be the string \"get \" prepended to attribute's identifier." followed by "Let F be
 * CreateBuiltinFunction(steps, 0, name, « », realm)"; the attribute setter ends "Let name be the string
 * \"set \" prepended to id." and "Let F be CreateBuiltinFunction(steps, 1, name, « », realm)". So an
 * attribute's PROPERTY is keyed by the identifier and the FUNCTIONS behind it are not: a browser answers
 * `Object.getOwnPropertyDescriptor(HTMLElement.prototype, "style").get.name` with "get style", never "style".
 *
 * WHY EVERY MINT GOT IT WRONG THE SAME WAY. The identifier is already in the installer's hand and it is
 * already the right string for four other things — the property key, the pool entry, §3.7.6's TypeError, and
 * [Replaceable]'s CreateDataPropertyOrThrow — so passing it on to the mint reads as obviously correct at every
 * site. Nothing in this engine could disagree: the length beside it was derived and asserted, and the name was
 * the one field of the descriptor no local instrument asks about. It took WPT's idlharness.js, which reads
 * `.name` off every member and diffs it against the published IDL, and the assertion it makes is this section
 * quoted back: `assert_equals(desc.get.name, "get " + member.name)`.
 *
 * THE PREFIX GOES ON THE FUNCTION OBJECT AND NOWHERE ELSE, WHICH IS WHAT MAKES THIS ONE COMPOSER RATHER THAN A
 * REWRITE OF THE ARGUMENT. The pool entry keeps the bare identifier because HTML §7.2.1.1 Integration with IDL
 * matches THAT against CrossOriginProperties's [[NeedsGetter]] and [[NeedsSetter]] rows — prefix it and every
 * cross-origin member silently stops matching, which is a security answer changed by a naming fix. The
 * [Replaceable] setter's own data keeps it for the same kind of reason: its steps end in
 * CreateDataPropertyOrThrow(jsValue, id, V), so `window.self = 1` must define `self` and not `set self`.
 * Composing HERE, into a buffer that reaches only the mint, is what keeps those four readers on the identifier
 * while the one reader §3.7.6 is about gets the composed string.
 *
 * `buf` is the CALLER'S, because JS_NewCFunction2 and JS_NewCFunctionData2 both intern the name into an atom
 * (js_new_c_function_data / JS_NewCFunction3 do `JS_NewAtom(ctx, name)`) and retain no pointer — so a stack
 * buffer that outlives the call is all this needs, and the composed string must NEVER be what a caller stores.
 *
 * IT IS NOT `static`, AND THAT IS NOT AN INVITATION. Everything above is performed FOR the installs in this
 * file; what reaches it from outside are the few members defined at a raw JS_DefinePropertyGetSet because no
 * installer form takes a plain C setter yet — see idl_args.h, which states that residual and what retires it.
 * Exporting a composer cannot be misused the way exporting a PREDICATE can: there is no call site at which
 * using this is the wrong thing to do, because using it IS the rule. */
const char *idl_accessor_name(char *buf, size_t cap, const char *id, IdlAccessorKind kind)
{
    int n;

    DCHECK(id != NULL && *id, "a Web IDL §3.7.6 Attributes accessor was named from an empty IDL identifier — "
                              "the composed name is the identifier with a prefix, so there is nothing to "
                              "prepend to");
    /* THE IDENTIFIER ARRIVES BARE, AND THAT IS AN INVARIANT WITH TWO EDGES. Compose twice and the member
       reports "get get style"; hand a prefixed identifier to the pool and HTML §7.2.1.1's cross-origin match
       is asked about a name no CrossOriginProperties row carries. The engine already contains four accessors
       spelled with the prefix BY HAND at raw JS_DefinePropertyGetSet sites, so the day one of those is routed
       through an installer this is the difference between a named abort and a wrong name nobody reads.
       The offender is NAMED because this composer is reached from every accessor install in the engine, and a
       crash reporting only this line would send its reader to read all of them. */
    DCHECKF(strncmp(id, "get ", 4) != 0 && strncmp(id, "set ", 4) != 0,
            "the IDL identifier '%s' already carries a Web IDL §3.7.6 Attributes accessor prefix — §3.7.6's "
            "prepend is performed HERE and only here, so an install hands this the bare identifier and the "
            "pool, the property key and the TypeError all keep it bare", id);
    n = snprintf(buf, cap, "%s%s", kind == IDL_ACCESSOR_SET ? "set " : "get ", id);
    /* A TRUNCATED NAME IS THIS DEFECT ARRIVING FROM THE OTHER SIDE — a member reporting a name no reading of
       the IDL produces — so the ceiling CRASHES rather than clamping. It is this engine's longest declared
       member plus a prefix, not a limit Web IDL §2 Lexical analysis imposes, so a longer identifier is a
       number to raise here and never a name to shorten. */
    DCHECKF(n > 0 && (size_t)n < cap,
            "the Web IDL §3.7.6 Attributes name for '%s' needs %d bytes and IDL_ACCESSOR_NAME_MAX is %d — "
            "raise it; a truncated accessor name is exactly the wrong name this composer exists to end",
            id, n + 1, (int)cap);
    return buf;
}

void idl_install_accessor_step(JSContext *ctx, JSValueConst target, const char *name,
                               int getter_stepid, int setter_stepid)
{
    JSAtom a = JS_NewAtom(ctx, name);
    JSValue g = JS_UNDEFINED, st = JS_UNDEFINED;

    DCHECK(a != JS_ATOM_NULL, "an IDL accessor name could not be interned");
    DCHECK(getter_stepid >= 0, "idl_install_accessor_step with no getter — a write-only attribute installs "
                               "through idl_install_accessor, which is the form that takes no getter at all");
    /* Through the ONE mint, like every other member — an accessor's getter and setter are pool members too, and
       minting them by hand here is what left an attribute reporting itself as "(none)" in a diagnostic. */
    g = idl_mint_accessor(ctx, name, getter_stepid, 0);
    if (setter_stepid >= 0)
        st = idl_mint_accessor(ctx, name, setter_stepid, 1);
    JS_DefinePropertyGetSet(ctx, (JSValue)target, a, g, st,
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, a);
}

/* WEB IDL §3.3.7 [Exposed]'s "is exposed in realm", for the conditions this engine models — see idl_args.h for
   why the question is asked HERE and the answer is stated by the component as data.
   THE NUMBER USED TO READ §3.9, WHICH IS "LEGACY PLATFORM OBJECTS" — a section that exists, is about
   [[GetOwnProperty]] on a legacy caller and has no exposure step in it at all. That is the wrong-citation
   failure this project calls worse than none: it reads as authoritative and sends the reader to a section that
   does not say what the code claims. It also could not be caught, and the reason is exactly the blind spot
   engine/citegen.mjs is required to state beside itself — the section RESOLVED, because §3.9 is a real
   section, and the citation carried no TITLE for the checker to disagree with. So it carries one now, which is
   what makes the next such mistake visible instead of silent.
   The algorithm is defined under that heading and its steps are numbered there: step 1 is the exposure set,
   step 2 is "If realm's settings object is not a secure context, and construct is conditionally exposed on
   [SecureContext], then return false", step 3 is [CrossOriginIsolated], step 4 returns true.
   IT IS `static` DELIBERATELY. A public predicate is an invitation to write `if (idl_exposed(ctx, ...))` at a
   call site, which is the per-member conditional the parameter exists to remove; the only way to reach this is
   to hand an installer the attribute the IDL states. When a [SecureContext] member of a shape that has no
   exposed-form installer yet arrives (an operation — HTML's registerProtocolHandler is the one waiting), that
   installer gets the same parameter and the same one line, rather than this becoming reachable from outside. */
static bool idl_exposed(JSContext *ctx, IdlExposure exposure)
{
    switch (exposure) {
    case IDL_EXPOSED:        return true;
    case IDL_SECURE_CONTEXT: return secure_context_is(ctx);   /* §3.3.7 [Exposed] step 2 */
    }
    DFAIL("a member was installed with an exposure condition Web IDL §3.3.7 [Exposed] has no step for — every "
          "value of IdlExposure is one of that algorithm's numbered conditions, so a new one is a new step to "
          "write here");
    return true;
}

void idl_install_accessor_exposed(JSContext *ctx, JSValueConst target, const char *name,
                                  IdlGetter getter, int getter_magic, int setter_stepid, IdlExposure exposure)
{
    /* §3.3.13: the member is simply NOT THERE. Nothing is minted, nothing is defined, and `name in target` is
       false — which is what a page's feature detection reads and what makes the non-secure arm of that branch
       a different program rather than the same one with a different value in it. */
    if (!idl_exposed(ctx, exposure)) return;
    idl_install_accessor(ctx, target, name, getter, getter_magic, setter_stepid);
}

/* §3.7.6's "define the attributes" step 1.5, which is the WHOLE of what an ordinary attribute and an
   unforgeable one differ by: "Let configurable be false if attr is unforgeable and true otherwise." The
   descriptor is otherwise identical — the same getter, the same setter, [[Enumerable]] true — so this is one
   define with one flag decided by the member's IDL, not two installs. */
static void idl_define_accessor(JSContext *ctx, JSValueConst target, const char *name,
                                IdlGetter getter, int getter_magic, int setter_stepid, int flags,
                                bool no_user_code)
{
    DCHECK(setter_stepid < 0 || idl_declared_before_seal(setter_stepid), name);
    JSAtom a = JS_NewAtom(ctx, name);
    JSValue g = JS_UNDEFINED, st = JS_UNDEFINED;

    DCHECK(a != JS_ATOM_NULL, "an IDL accessor name could not be interned");
    DCHECK(getter != NULL || !no_user_code,
           "a no-user-code declaration was made for an attribute that has no getter — the claim is about what a "
           "GETTER'S BODY reaches, and a write-only member has no body to make it about");
    if (getter) {
        g = idl_mint_plain_getter(ctx, target, name, getter, getter_magic);
        /* THE DECLARATION RIDES THE MINTED OBJECT, which is what makes it PER MEMBER. A shared C body reached
           through several magics (js_rule_get is one) is several function objects, so one arm of it saying this
           says nothing about the others — the granularity a per-function-pointer exemption would not have. */
        if (no_user_code) {
            /* AND A GLOBAL ATTRIBUTE'S GETTER IS NOT THAT OBJECT ANY MORE. On the realm's global the mint
               above returns §3.7.6's opening steps wrapped around the member's body, so the claim would be
               about window_proxy_security_check and window_proxy_this_object as well — which nothing has
               established. No member combines the two today; the day one does, establish it for those two
               halves and declare it on the wrapper, rather than on a body the read no longer reaches first. */
            DCHECK(!idl_target_is_realm_global(ctx, target),
                   "a no-user-code declaration was made for an attribute installed on the realm's GLOBAL — its "
                   "read now runs Web IDL §3.7.6's opening steps first, so the claim is about those and not "
                   "only about the member's body");
            JS_DeclareCFunctionNoUserCode(g);
        }
    }
    /* The GETTER here is a plain C function with no pool entry (this is the form for an attribute whose read
       runs none of the page's code), but the SETTER is a step member exactly like any other, so it is minted
       the same way and named the same way. It was the fourth hand-written mint. */
    if (setter_stepid >= 0)
        st = idl_mint_accessor(ctx, name, setter_stepid, 1);
    JS_DefinePropertyGetSet(ctx, (JSValue)target, a, g, st, flags);
    JS_FreeAtom(ctx, a);
}

void idl_install_accessor(JSContext *ctx, JSValueConst target, const char *name,
                          IdlGetter getter, int getter_magic, int setter_stepid)
{
    idl_define_accessor(ctx, target, name, getter, getter_magic, setter_stepid,
                        JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE, /*no_user_code*/false);
}

/* THE SAME INSTALL, WITH THE GETTER'S BODY DECLARING WHAT IT REACHES — see idl_args.h for what the claim buys
   and what it costs. It is a separate entry point rather than a parameter on the one above for a reason that is
   not convenience: a parameter would have to be answered at every one of the hundreds of existing installs, and
   the answer that gets typed under those conditions is the one that makes the build go quiet. A member that
   says nothing is undeclared, which is the safe reading and the crashing one. */
void idl_install_accessor_no_user_code(JSContext *ctx, JSValueConst target, const char *name,
                                       IdlGetter getter, int getter_magic, int setter_stepid)
{
    idl_define_accessor(ctx, target, name, getter, getter_magic, setter_stepid,
                        JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE, /*no_user_code*/true);
}

/* §3.4.10's [LegacyUnforgeable] — see idl_args.h. The property is defined at the SAME moment and with the same
   getter as any other attribute; what differs is the two things §3.7.6 and §3.4.10 state, and both are here:
   it is defined on the object the caller passes (its INSTANCE, not its prototype) and it is NOT configurable. */
void idl_install_accessor_unforgeable(JSContext *ctx, JSValueConst target, const char *name,
                                      IdlGetter getter, int getter_magic, int setter_stepid)
{
    DCHECK(getter != NULL,
           "an unforgeable attribute was installed with no getter — [LegacyUnforgeable] exists so that a read "
           "of this member cannot be redirected, and a member with nothing to read is not that member");
    idl_define_accessor(ctx, target, name, getter, getter_magic, setter_stepid, JS_PROP_ENUMERABLE,
                        /*no_user_code*/false);
}

/* §3.7.6's [Replaceable] SETTER, and its one implementation. The spec's steps are "Perform
   ? CreateDataPropertyOrThrow(esValue, id, V)" — a DEFINE on the RECEIVER, not a store into whatever the getter
   reads, so the accessor is gone from that object afterwards and every other realm's is untouched. THROWING is
   part of it: a page that has already made the property non-configurable gets a TypeError from the define
   rather than a silent no-op, which window-opener-unconfigurable.window.js reads as its last assertion.
   The NAME rides on the function as its data. A magic index into a table would have been a second list to keep
   in step with the first; the name is the only thing this needs and it is already a string at the call site. */
int idl_replace_with_value(JSContext *ctx, JSValueConst obj, const char *name, JSValueConst v)
{
    JSAtom a = JS_NewAtom(ctx, name);
    int r;

    DCHECK(a != JS_ATOM_NULL, "a replaceable attribute's own name would not intern");
    r = JS_DefinePropertyValue(ctx, (JSValue)obj, a, JS_DupValue(ctx, v), JS_PROP_C_W_E | JS_PROP_THROW);
    JS_FreeAtom(ctx, a);
    return r;
}

/* WEB IDL §3.7.6 Attributes' `jsValue`, FOR EVERY ACCESSOR THIS FILE INSTALLS ON A [Global] OBJECT — resolved,
 * security-checked and brand-checked, in the standard's own order.
 *
 * §3.7.6 gives create an attribute getter and create an attribute setter the SAME three opening steps ahead of
 * the member's own getter/setter steps. Getter, step 1's try-list (step 1 holds TWO lists — the try-list and
 * the "if an exception E was thrown" list — so the list is named rather than left to a bare sub-number):
 *   1.1.2.1 "Let jsValue be the this value, if it is not null or undefined, or realm's global object
 *            otherwise."
 *   1.1.2.2 "If jsValue is a platform object, then perform a security check, passing jsValue, attribute's
 *            identifier, and \"getter\"."
 *   1.1.2.3 "If jsValue does not implement target, then:" … 1.1.2.3.2 "Otherwise, throw a TypeError."
 * The setter's are 4.5.1, 4.5.2 (with "setter") and 4.5.3/4.5.4 ("Let validThis be true if jsValue implements
 * target, or false otherwise" / "If validThis is false and attribute was not specified with the
 * [LegacyLenientThis] extended attribute, then throw a TypeError"). §3.5's third input is the ONE thing that
 * differs between the two, so it is what the caller states — the same fact the pool states at a step member's
 * mint as `sec_kind`, asked here at the one call because a plain-C accessor has no pool entry to carry it.
 *
 * THEY ARE THE ACCESSOR MACHINERY'S WORK, NOT THE MEMBER'S — which is why they belong here, and why every
 * member minted here had them missing TOGETHER: the [Replaceable] setter handed its raw `this` straight to the
 * define, and the held-value getter opened with `(void)this_val`. A MISSING receiver and a FOREIGN one reached
 * the same answer, which is the two facts a default confuses, written out in the loudest possible form.
 * The setter's own §7.2.2.4 spelling of this — `opener`'s, in window.c — already resolved its receiver by
 * hand through these same two window_proxy.c halves; the shared mechanism underneath it did not.
 *
 * AND 1.1.2.2 IS NOT A REFINEMENT OF 1.1.2.3 — IT IS THE STEP THAT RUNS FIRST AND REFUSES A DIFFERENT SET.
 * A cross-origin WindowProxy IMPLEMENTS Window, so the brand alone lets every one of this engine's global
 * attributes answer a foreign document's read: `Object.getOwnPropertyDescriptor(window, "crypto").get
 * .call(otherWindow)` reached the reading realm's Crypto with nothing between. §7.2.1.3.1 CrossOriginProperties
 * is what says `crypto` is not on that list at all, and window_proxy_security_check is where the list
 * lives. A page tells the two refusals apart by name — "SecurityError" against TypeError — which is exactly
 * what cross-origin-objects.html asserts.
 *
 * THE INTERFACE IS Window FOR EVERY MEMBER THAT REACHES HERE, and that is ASSERTED AT THE INSTALL rather than
 * assumed at the read: every call site that routes here targets the realm's global, so `target` is Window and
 * window_proxy.c owns the test. A brand parameter every existing caller would pass identically is a field
 * nobody would notice going wrong; IDL_CHECK_GLOBAL_TARGET below is what names the brand this file has to be
 * TOLD the day a component declares one of these attributes on some other interface.
 *
 * Returns the resolved jsValue OWNED, or JS_EXCEPTION with §3.7.6's TypeError or §3.5's SecurityError pending. */
static JSValue idl_attribute_this(JSContext *ctx, JSValueConst this_val, const char *name,
                                  WindowProxySecurityType type)
{
    JSValue js = window_proxy_this_object(ctx, this_val);   /* 1.1.2.1 / 4.5.1, written once */

    /* 1.1.2.2 / 4.5.2. window_proxy_security_check's own step 1 is "If platformObject is not a Window or
       Location object, then return", so handing it a plain object is the standard's own no-op and not a
       widening of what gets checked. */
    if (window_proxy_security_check(ctx, js, name, type) < 0) {
        JS_FreeValue(ctx, js);
        return JS_EXCEPTION;
    }
    if (window_proxy_implements_window(js)) return js;   /* 1.1.2.3 / 4.5.3 */
    JS_FreeValue(ctx, js);
    return JS_ThrowTypeError(ctx, "'%s' called on an object that does not implement interface Window", name);
}

/* ---- WEB IDL §3.7.6's OPENING STEPS FOR A PLAIN-C ATTRIBUTE GETTER ---------------------------------------
 *
 * A plain-C getter is an `IdlGetter` — a raw JS_CFUNC_getter_magic with no pool entry — so it reaches neither
 * js_idl_args_step's implementation-check nor idl_attribute_this above, and NEITHER of §3.7.6's opening steps
 * was performed for any of them. On a PROTOTYPE that is the residual js_idl_args_step names by shape; on the
 * [Global] object it is decidable HERE, because the target settles the interface: Window is the only [Global]
 * interface this engine has (idl_args.h's §3.3.7 note states the same thing from the other side), so an
 * accessor installed as an own property of the realm's global is a Window attribute and `target` is Window.
 * That is a fact about the INSTALL, which is why the routing is made there and not asked at the read.
 *
 * THE MEMBER TRAVELS WITH THE OPERATION. One shared body serves every global attribute, so a DCHECK or a
 * TypeError written in it would name this file and none of the ~20 components that install one — the shape
 * §AN-ASSERT-THAT-NAMES-A-REMEDY is about. The member's own getter, its magic and its IDENTIFIER are captured
 * at the install and reached through the wrapper's magic, so every message below names the member a page
 * actually touched.
 *
 * IT IS A DECLARATION TABLE AND NOT PER-REALM STATE. The three fields are compile-time facts about the member
 * — a C function pointer, its magic, and the identifier its declaration owns — none of which can differ
 * between realms, so there is nothing here for a realm to answer differently. What IS per realm is the
 * FUNCTION OBJECT: idl_mint_plain_getter runs inside each realm's own intrinsic install, so js_call_c_function
 * sets `ctx` to the realm that minted it and §3.7.6's "realm's global object" is that realm's. A module static
 * holding a JSValue would be the defect this table is arranged to avoid.
 *
 * The pointer is NOT stored in a JSCFunctionType and NOT boxed into a JSValue: both are the strict-aliasing
 * shape §C-stack names, which passes at -O0 and segfaults a directory at -O1. It rides an INDEX, the way every
 * other magic in this file does. */
typedef struct {
    IdlGetter   getter;   /* §3.7.6 step 1.1.3's "getter steps of attribute", run with jsValue as this */
    int         magic;    /* the member's own magic, which the wrapper's has displaced */
    const char *name;     /* the identifier 1.1.2.2 passes to the security check and 1.1.2.3.2's TypeError names */
} IdlGlobalAttr;
static IdlGlobalAttr *g_gattr;
static int            g_gattr_n, g_gattr_cap;

/* THE SAME MEMBER IS INSTALLED ONCE PER REALM, so the entry is found rather than appended after the first —
   which is what keeps the table the size of the platform's global attribute list rather than of the session's
   realm count.
   THE IDENTIFIER IS PART OF THE KEY AND NOT A FIELD CHECKED AGAINST IT, because TWO MEMBERS LEGITIMATELY SHARE
   ONE BODY AND ONE MAGIC. HTML §7.2.2 The Window object's IDL declares `readonly attribute Navigator
   navigator;` and, on the next line, `[Replaceable] readonly attribute Navigator clientInformation; // legacy
   alias of .navigator` — two members answering out of one C body at magic 0, and they differ in their
   extended attribute as well as their name, which is why core/frame/navigator.c installs them through two
   different forms on consecutive lines. Keying on (getter, magic) alone
   would have made them one entry, and then §3.7.6 step 1.1.2.2's security check would be passed the wrong
   identifier and 1.1.2.3.2's TypeError would name the wrong member. They are DISTINCT members of the
   interface, so they are distinct entries. */
static int idl_global_attr_declare(IdlGetter getter, int magic, const char *name)
{
    int i;

    DCHECK(getter != NULL, "a §3.7.6 global attribute was declared with no getter steps to run");
    DCHECK(name != NULL && *name,
           "a §3.7.6 global attribute was declared with no identifier — its step 1.1.2.2 passes the identifier "
           "to §7.2.1.1 Integration with IDL, which matches it against CrossOriginProperties by name, so a "
           "member reaching there unnamed would be refused every cross-origin read the list permits");
    for (i = 0; i < g_gattr_n; i++)
        if (g_gattr[i].getter == getter && g_gattr[i].magic == magic && strcmp(g_gattr[i].name, name) == 0)
            return i;
    if (g_gattr_n == g_gattr_cap) {
        int cap = g_gattr_cap ? g_gattr_cap * 2 : 32;
        IdlGlobalAttr *t = realloc(g_gattr, (size_t)cap * sizeof *t);
        CHECK(t != NULL, "idl: OOM recording a §3.7.6 global attribute — a member that cannot be installed is "
                         "an API the page cannot read");
        g_gattr = t;
        g_gattr_cap = cap;
    }
    /* THE MAGIC IS 16 BITS AND THIS INDEX BECOMES ONE. JS_NewCFunction2 stores it in `u.cfunc.magic`, so an
       index past that is a member silently answering as some other member. It is asserted at the DECLARATION,
       where the count is, rather than at the read, where the damage would already be a wrong answer. */
    DCHECKF(g_gattr_n < 32767,
            "the §3.7.6 global attribute table passed what a C function's magic can carry at '%s' — the index "
            "is stored in u.cfunc.magic, which is 16 bits, so the next entry would answer as an earlier member",
            name);
    g_gattr[g_gattr_n] = (IdlGlobalAttr){ getter, magic, name };
    return g_gattr_n++;
}

/* THE WRAPPER — §3.7.6 step 1.1.2's three steps, then step 1.1.3's "running the getter steps of attribute with
   idlObject as this". idlObject is the IDL value 1.1.2.5 sets from jsValue, which for this engine is jsValue
   itself, so the member's body receives the RESOLVED receiver and never the raw one. That is the whole of the
   behaviour change a member sees: a body that used to be handed `undefined` for a bare `window.crypto` read is
   now handed this realm's global, and a body that used to be handed a foreign document's proxy is not reached
   at all.

   NAMED RESIDUAL — A MEMBER WHOSE GETTER STEPS IGNORE idlObject. WHAT IS NOT COVERED: this machine performs
   §3.7.6's opening steps and hands the member the receiver they resolved; what the member's own step 1.1.3
   does with it is the MEMBER'S declaration, and several global attributes answer out of the realm the getter
   was installed in rather than out of idlObject's navigable (the shape is `(void)this_val;` followed by
   `realm_value_get(ctx, slot)`). For every receiver that IS this realm's Window or its WindowProxy those are
   the same answer, which is every read a page makes by writing the member's name; they part company only for a
   receiver applied by hand across navigables, and then the reading realm's object is returned for a question
   about a window that is not it. THIS MACHINE CANNOT SEE THE DIFFERENCE: the members that read the navigable
   (§7.2.2's `closed`, `name`, `opener`, `frameElement`) answer a foreign receiver CORRECTLY and must not be
   refused, so a receiver-is-own-realm test here would abort on the members that already work. WHAT THE NEXT
   DIFF BUILDS: those getter steps read idlObject's navigable — the way §7.2.2.4's `closed` already does
   through window_proxy_this_navigable — so that the answer is a fact about the receiver and the realm the
   accessor was minted in stops being consulted at all; the DCHECK idl_held_value_get carries is the same
   statement for the form whose value is fixed at install time. HOW ITS ABSENCE SHOWS:
   `Object.getOwnPropertyDescriptor(window, "location").get.call(frames[0])` returns THIS document's Location
   where HTML §7.2.1.3.1 CrossOriginProperties lists `location` with [[NeedsGetter]] and a browser returns the
   child's; the same call for "history" and "crypto" returns this realm's object instead of the child's. */
static JSValue idl_global_attribute_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    const IdlGlobalAttr *a;
    JSValue js, r;

    DCHECK(magic >= 0 && magic < g_gattr_n,
           "a §3.7.6 global attribute getter ran at an index this table never made — the index rides the "
           "function object's magic and is written only by idl_global_attr_declare");
    a = &g_gattr[magic];
    js = idl_attribute_this(ctx, this_val, a->name, WP_SEC_GETTER);
    if (JS_IsException(js)) return JS_EXCEPTION;
    r = a->getter(ctx, js, a->magic);   /* step 1.1.3, with idlObject as this */
    JS_FreeValue(ctx, js);
    return r;
}

/* THE ONE PLACE A PLAIN-C ATTRIBUTE GETTER IS MINTED. It was two — idl_define_accessor and
   idl_install_replaceable each built their own JS_NewCFunction2 — and a residual in js_idl_args_step named the
   first of them as "the one place every plain getter is created", which was true of one of the two mints and
   therefore false of this tree: [Replaceable]'s readonly form (`parent`, `length`, `navigation`,
   `visualViewport`, `clientInformation`, and CSSOM VIEW §4's thirteen Window members) is a plain getter on the
   global that the other mint would have left unrouted. One mint is what makes the routing below unforgettable
   rather than remembered twice.
   §3.7.6's composed name and its length of 0 are the SECTION's constants and are unchanged here. */
static JSValue idl_mint_plain_getter(JSContext *ctx, JSValueConst target, const char *name,
                                     IdlGetter getter, int getter_magic)
{
    char nb[IDL_ACCESSOR_NAME_MAX];
    bool on_global = idl_target_is_realm_global(ctx, target);
    JSValue f;

    DCHECK(getter != NULL, "a plain-C attribute getter was minted with no getter steps");
    /* §3.7.6's create an attribute getter: the property is keyed by the identifier and the FUNCTION carries
       the composed name. The two differ, and only this mint knows it. */
    if (on_global)
        f = JS_NewCFunction2(ctx, (JSCFunction *)idl_global_attribute_get,
                             idl_accessor_name(nb, sizeof nb, name, IDL_ACCESSOR_GET), 0,
                             JS_CFUNC_getter_magic, idl_global_attr_declare(getter, getter_magic, name));
    else
        f = JS_NewCFunction2(ctx, (JSCFunction *)getter,
                             idl_accessor_name(nb, sizeof nb, name, IDL_ACCESSOR_GET), 0,
                             JS_CFUNC_getter_magic, getter_magic);
    CHECK(!JS_IsException(f), "an IDL attribute's getter could not be allocated");
    return f;
}

static JSValue idl_replaceable_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                   int magic, JSValue *data)
{
    const char *name;
    JSValue js;
    int r;

    (void)magic; (void)argc;
    /* §3.7.6 steps 1-2: "Let V be undefined. If any arguments were passed, then set V to the value of the
       first argument passed." A ZERO-ARGUMENT CALL IS SPEC-LEGAL — `desc.set.call(null)` writes it — and
       reading argv[0] is still right, because js_call_c_function_data pads arg_buf to the mint's `length` of
       1 with JS_UNDEFINED while passing the caller's own argc through. An assert stood here demanding argc>=1
       on the strength of "the interpreter passes undefined for a bare `x.y = `", which is true of that one
       spelling and of no other: it aborted the engine on a call the spec defines. */
    name = JS_ToCString(ctx, data[0]);   /* the function's own data: a string this file put there */
    if (!name) return JS_EXCEPTION;
    js = idl_attribute_this(ctx, this_val, name, WP_SEC_SETTER);   /* §3.7.6 steps 4.5.1-4.5.4 */
    if (JS_IsException(js)) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
    r = idl_replace_with_value(ctx, js, name, argv[0]);
    JS_FreeValue(ctx, js);
    JS_FreeCString(ctx, name);
    return r < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

/* THE GETTER FOR AN ATTRIBUTE WHOSE VALUE THE ENGINE ALREADY HOLDS — the realm's own Document, its global,
   its one custom element registry. It reads no state and runs none of the page's code; the value rides on the
   function as its data, which is what makes it per-REALM rather than a module static answering every realm out
   of whichever built it first. Shared by §3.7.6's [Replaceable] form and the readonly one below, because it is
   the same getter and there is no second thing for it to be.
   THE RECEIVER IS RESOLVED HERE EVEN THOUGH THE ANSWER DOES NOT DEPEND ON IT, and that is not a computed value
   with no reader: §3.7.6's brand check IS its reader. A held value answers a question about `jsValue`, so a
   receiver that does not implement Window must throw rather than be handed this realm's Document — the two are
   different facts and `(void)this_val` made them one.
   data[0] is the VALUE and data[1] is the member's NAME, which the throw needs to name the member. */
static JSValue idl_held_value_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                  int magic, JSValue *data)
{
    const char *name;
    JSValue js;

    (void)argc; (void)argv; (void)magic;
    name = JS_ToCString(ctx, data[1]);
    if (!name) return JS_EXCEPTION;
    js = idl_attribute_this(ctx, this_val, name, WP_SEC_GETTER);   /* §3.7.6 step 1's try-list, 1.1.2.1-1.1.2.3 */
    JS_FreeCString(ctx, name);
    if (JS_IsException(js)) return JS_EXCEPTION;
    DCHECK(window_proxy_receiver_is_own_realm(ctx, js),
           "a held-value Window attribute was read with a receiver belonging to ANOTHER navigable — the value "
           "on this getter is THIS realm's, so answering it is this realm's answer to a question about a "
           "window that is not it. §3.7.6 runs the getter steps with idlObject as this; give this member "
           "getter STEPS that read the receiver's realm rather than a value fixed at install time");
    JS_FreeValue(ctx, js);
    return JS_DupValue(ctx, data[0]);
}

/* THE STATEMENT THAT MAKES idl_attribute_this's Window BRAND THE RIGHT ONE, asserted where the interface is
 * DECIDED rather than where the receiver arrives. §3.7.6's TypeError is about `target`, and this file mints
 * the accessor without being told which interface that is — so what it relies on is that every member reaching
 * it belongs to the one [Global] interface this engine has. `target` being the realm's global object IS that
 * fact (idl_args.h's §3.3.7 note states the same thing from the other side: there is no WorkerGlobalScope here,
 * so one global kind means one [Global] interface), and the day a component declares a [Replaceable] or a
 * held-value attribute on an ordinary interface this fires and names the brand that must then become data the
 * component states — the way IdlExposure and IdlAttrForge already are.
 *
 * IT IS NOT window_proxy_implements_window(target), and the reason is an ORDERING that would have made the
 * stricter-looking assert simply wrong: realm_install_intrinsics runs BEFORE the per-document install column,
 * so viewport's thirteen members are installed while the global is still a plain object and only i_window's
 * JS_SetGlobalClass gives it the Window class afterwards. The global's IDENTITY is settled at both moments;
 * its class is not.
 *
 * IT NAMES THE MEMBER, because the caller's file:line is the one coordinate a shared install helper destroys.
 * Every component that installs one of these attributes arrives at this same line, so an assert reporting only
 * "a replaceable attribute" would name the helper and none of the components that could have been wrong. */
#if APICLIENT_DEV
static void idl_check_global_target(JSContext *ctx, JSValueConst target, const char *name, const char *form)
{
    if (idl_target_is_realm_global(ctx, target)) return;
    DFAILF("the %s attribute `%s` was installed on something that is not the realm's global object — its "
           "accessor applies Web IDL §3.7.6's TypeError against the Window brand, which is right only "
           "because Window is the only [Global] interface in this engine. Give the install the interface's "
           "own brand as data, the way IdlExposure and IdlAttrForge are stated", form, name);
}
#define IDL_CHECK_GLOBAL_TARGET(c, t, n, f) idl_check_global_target((c), (t), (n), (f))
#else
#define IDL_CHECK_GLOBAL_TARGET(c, t, n, f) ((void)0)
#endif

/* Both forms end here: an accessor with §3.7.6's shared setter, at an IDL attribute's flags. */
static void idl_define_replaceable(JSContext *ctx, JSValueConst target, const char *name, JSValue getter)
{
    JSAtom a = JS_NewAtom(ctx, name);
    JSValue nm, setter;
    char nb[IDL_ACCESSOR_NAME_MAX];

    IDL_CHECK_GLOBAL_TARGET(ctx, target, name, "[Replaceable]");
    DCHECK(a != JS_ATOM_NULL, "a replaceable attribute name could not be interned");
    /* THE DATA IS THE IDENTIFIER AND THE NAME IS §3.7.6'S — the one place in this file where the distinction
       is load-bearing for something other than a reported name. `nm` reaches idl_replaceable_set, whose steps
       end in CreateDataPropertyOrThrow(jsValue, id, V), so composing it would make `window.self = 1` define a
       property called "set self" and leave the accessor in place. */
    nm = JS_NewString(ctx, name);
    CHECK(!JS_IsException(nm), "a replaceable attribute's name could not be allocated");
    setter = JS_NewCFunctionData2(ctx, idl_replaceable_set,
                                  idl_accessor_name(nb, sizeof nb, name, IDL_ACCESSOR_SET), 1, 0, 1,
                                  (JSValueConst *)&nm);
    CHECK(!JS_IsException(setter), "a replaceable attribute's setter could not be allocated");
    JS_FreeValue(ctx, nm);
    JS_DefinePropertyGetSet(ctx, (JSValue)target, a, getter, setter,
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, a);
}

/* THE READONLY [Replaceable] FORM'S GETTER IS A PLAIN-C GETTER LIKE ANY OTHER, so it is minted by the one mint.
   It used to have a JS_NewCFunction2 of its own, which is how CSSOM VIEW §4's thirteen Window members and
   HTML §7.2.2.4's `parent` and `length` came to be global attributes reading an unresolved receiver while the
   SETTER installed one line later resolved one. */
void idl_install_replaceable(JSContext *ctx, JSValueConst target, const char *name,
                             IdlGetter getter, int getter_magic)
{
    DCHECK(getter != NULL, "a replaceable attribute with no getter — it is READONLY, so the read is all it has");
    idl_define_replaceable(ctx, target, name, idl_mint_plain_getter(ctx, target, name, getter, getter_magic));
}

/* THE HELD-VALUE GETTER'S DATA IS TWO VALUES, and the second is the member's own NAME. §3.7.6's TypeError
   names the member a page mis-invoked, and a getter that carries only its value has nothing to name it with —
   the same argument the [Replaceable] setter above already makes for carrying its name as data. */
static JSValue idl_held_value_getter(JSContext *ctx, const char *name, JSValue value)
{
    JSValue d[2], g;
    char nb[IDL_ACCESSOR_NAME_MAX];

    d[0] = value;
    /* d[1] IS THE IDENTIFIER, not §3.7.6's composed name: it is what the getter's TypeError names the member
       with, and "get document does not implement interface Window" names a member no IDL declares. */
    d[1] = JS_NewString(ctx, name);
    CHECK(!JS_IsException(d[1]), "a held-value attribute's name could not be allocated");
    g = JS_NewCFunctionData2(ctx, idl_held_value_get,
                             idl_accessor_name(nb, sizeof nb, name, IDL_ACCESSOR_GET), 0, 0, 2,
                             (JSValueConst *)d);
    CHECK(!JS_IsException(g), "a held-value attribute's getter could not be allocated");
    JS_FreeValue(ctx, d[0]);   /* the getter holds its own reference to both */
    JS_FreeValue(ctx, d[1]);
    return g;
}

void idl_install_replaceable_value(JSContext *ctx, JSValueConst target, const char *name, JSValue value)
{
    idl_define_replaceable(ctx, target, name, idl_held_value_getter(ctx, name, value));
}

/* §3.7.6's READONLY ATTRIBUTE OVER A VALUE THE REALM ALREADY HOLDS — the primitive four members needed and
   none of them had, which is why four of them were plain data properties instead.
   §3.7.6 defines EVERY attribute as `PropertyDescriptor{[[Getter]], [[Setter]], [[Enumerable]]: true,
   [[Configurable]]: configurable}` and computes that last field as "false if attr is unforgeable and true
   otherwise". Nothing in it is conditional on whether the VALUE can change — and "the value is fixed, so an
   accessor would compute the same answer forever" is precisely the argument three independent authors reached
   here, each writing a data property that answers getOwnPropertyDescriptor wrongly and, where it came from
   JS_SetPropertyStr, is writable enough for a page to replace the member outright.
   THE [Replaceable] FORM ABOVE IS THE WRONG NEIGHBOUR TO REACH FOR: it installs §3.7.6's replaceable setter,
   and `window`, `document` and `customElements` are not [Replaceable] — HTML §7.2.2's IDL marks the first two
   [LegacyUnforgeable] and the third plain readonly — so a page assigning to one must not replace it. This form
   has NO setter, which is what readonly means. */
void idl_install_value_attribute(JSContext *ctx, JSValueConst target, const char *name, JSValue value,
                                 IdlAttrForge forge)
{
    JSValue g = idl_held_value_getter(ctx, name, value);
    JSAtom a;

    IDL_CHECK_GLOBAL_TARGET(ctx, target, name, "held-value readonly");
    a = JS_NewAtom(ctx, name);
    DCHECK(a != JS_ATOM_NULL, "an IDL attribute name could not be interned");
    JS_DefinePropertyGetSet(ctx, (JSValue)target, a, g, JS_UNDEFINED,
                            JS_PROP_ENUMERABLE | (forge == IDL_ATTR_UNFORGEABLE ? 0 : JS_PROP_CONFIGURABLE));
    JS_FreeAtom(ctx, a);
}

/* THE ONE PLACE A STEP MEMBER IS MINTED. A member is DECLARED (which builds its pool entry) before it is
   given a name, and stepid ties the two together — so whoever mints the function is the only one who can tell
   the pool what to call itself. There were THREE ways to mint one: this file's install helper, and a bare
   JS_NewCFunction2(..., JS_CFUNC_step, stepid) written out at six other sites. Only the first named anything,
   which is how a five-second member step reported itself as "(none)" and stayed anonymous through two rebuilds.
   Six hand-written copies of one call is the same shape as one builtin answering differently depending on which
   spelling reached it: the fix is not to name them one by one, it is for there to be one mint. */
/* WEB IDL §3.7.1: AN INTERFACE WITH NO CONSTRUCTOR IS STILL A FUNCTION OBJECT, and calling or constructing it
   throws a TypeError — `new MessagePort()` is a TypeError, not a way to make a port. Six interfaces built their
   interface object with a NULL body, which is not "no constructor": it is a null function pointer, and
   `new MessagePort()` called through it and SEGFAULTED the process. The spec's TypeError was arriving as a
   crash. One call builds an interface object now, so a seventh interface cannot invent a seventh way to have no
   constructor. */
static JSValue idl_illegal_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

JSValue idl_interface_object(JSContext *ctx, const char *name, JSValueConst proto)
{
    JSValue ctor = JS_NewCFunction2(ctx, idl_illegal_ctor, name, 0, JS_CFUNC_constructor, 0);

    DCHECK(JS_IsObject(proto), "an interface object was built with no prototype behind it — §3.7.1 gives every "
                               "one a `prototype`, and a page brand-checks through it");
    CHECK(!JS_IsException(ctor), "an interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);   /* .prototype and .constructor, both directions, one call */
    return ctor;
}

/* §3.7 Interfaces' "corresponding property … on the realm's global object", asked through §3.3.7 [Exposed]'s
   one gate — see idl_args.h for why the exposure is the install's argument and never a caller's `if`.
   THE MINT IS INSIDE THE GATE. Building the interface object and then declining to install it would leave a
   §3.7.1 function object with this realm's prototype on it, reachable from nothing and freed immediately — a
   thing that exists in a realm the standard says it does not exist in, and the kind of half-state a later
   reader takes for a cache. Not exposed means not built. */
void idl_install_interface_object_exposed(JSContext *ctx, JSValueConst target, const char *name,
                                          JSValueConst proto, IdlExposure exposure)
{
    DCHECK(name != NULL && *name, "an interface object was installed with no identifier — §3.7 names the "
                                  "global's property after the interface, and there is nothing else to key it "
                                  "by");
    if (!idl_exposed(ctx, exposure)) return;   /* §3.3.13: "there will be no \"X\" property on Window" */
    JS_SetPropertyStr(ctx, (JSValue)target, name, idl_interface_object(ctx, name, proto));
}

/* WEB IDL §3.11.1 "Legacy callback interface object". "For every callback interface that is exposed in a given
   realm and on which constants are defined, a corresponding property exists on the realm's global object …
   its value is an object called the legacy callback interface object", and that object is created by "Let
   steps be the following steps: Throw a TypeError. Let F be CreateBuiltinFunction(steps, 0, id, « », realm).
   Define the constants of interface on F". It is a FUNCTION, not an ordinary object — the section's own note
   says `typeof` answers "function" — and it is NOT a constructor: §3.7.1's interface object is the one built
   with [[Construct]], and `new NodeFilter()` is a TypeError either way.
   The whole of §3.11.1 is these three lines, so there is ONE of them for the same reason there is one
   idl_interface_object: a callback interface that hand-rolled its own would be free to build an ordinary
   object again, which is what DOM's NodeFilter did — `typeof NodeFilter` answered "object" in this engine and
   "function" in every browser. */
static JSValue idl_callback_iface_call(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal invocation");
}

JSValue idl_callback_interface_object(JSContext *ctx, const char *name)
{
    JSValue f;

    DCHECK(name != NULL && *name, "a legacy callback interface object was built with no identifier — §3.11.1 "
                                  "names the function after the interface, and the global property after it");
    f = JS_NewCFunction2(ctx, idl_callback_iface_call, name, 0, JS_CFUNC_generic, 0);
    CHECK(!JS_IsException(f), "a legacy callback interface object could not be allocated");
    return f;
}

/* THE ONE MINT, and the one place §3.7.7 Operations' `length` is stated — see idl_member_length_of. A caller
   names the member and the pool answers how long it is; there is no argument for a caller to get wrong. */
static JSValue idl_mint_step(JSContext *ctx, const char *name, int stepid, JSCFunctionEnum cproto,
                             uint8_t sec_kind)
{
    int idx = idl_member_of_step(stepid);
    /* NAMING THE OFFENDER IS THE POINT. "some member was never declared" sends whoever hits it grepping every
       install site; the name is right here in the argument, so the assert says it. */
    if (idx < 0)
        DFAILF("step function '%s' was minted for a member this pool never declared — a step machine that is "
               "not an args-machine member installs through idl_install_step_method", name ? name : "?");
    DCHECK(name != NULL && *name, "a step function was minted with no name — the pool has nothing to call it");
    idl_member(idx)->name = name;
    /* THE KIND IS SET ON EVERY MINT AND MUST NOT DIFFER BETWEEN THEM. A member is minted once per REALM (each
       realm installs its own function object over one pool entry), so this runs many times for one member and
       writes the same value — and a stepid reaching here as an operation in one realm and as an attribute's
       setter in another would be one entry answering §7.2.1.1 Integration with IDL's `type` two ways. */
    DCHECK(idl_member(idx)->sec_kind == IDL_SEC_NONE || idl_member(idx)->sec_kind == sec_kind,
           "one pool entry was minted under two different Web IDL §3.5 Security types — the type is what "
           "HTML §7.2.1.1 Integration with IDL matches against CrossOriginProperties's [[NeedsGetter]] and "
           "[[NeedsSetter]], so a member that is an operation here and an attribute's accessor there decides "
           "a cross-origin access two ways");
    idl_member(idx)->sec_kind = sec_kind;
    /* WEB IDL §3.7.6 Attributes' NAME STEP, ASKED FROM THE KIND THE MINT ALREADY STATES rather than from a
       second list of which members are accessors. `sec_kind` is set one line above and is the same fact
       §3.5 Security asks about, so the day a member is minted as an accessor it is named as one — there is no
       parallel table to keep in step, which is what the §3.5 kind being declared at the mint buys twice.
       §3.7.7 Operations and §3.7.1 Interface object both mint with the bare `id`, so a method and a
       constructor fall through unprefixed, which is the whole of their rule. And the POOL entry above keeps
       `name` BARE — see idl_accessor_name for why HTML §7.2.1.1's cross-origin match depends on that. */
    {
        char nb[IDL_ACCESSOR_NAME_MAX];
        const char *fn = name;

        if (sec_kind == IDL_SEC_GETTER || sec_kind == IDL_SEC_SETTER)
            fn = idl_accessor_name(nb, sizeof nb, name,
                                   sec_kind == IDL_SEC_SETTER ? IDL_ACCESSOR_SET : IDL_ACCESSOR_GET);
        return JS_NewCFunction2(ctx, NULL, fn, idl_member_length_of(idl_member(idx)), cproto, stepid);
    }
}

/* AN ATTRIBUTE'S ACCESSOR, whose length is WEB IDL §3.7.6 Attributes' OWN number and not §3.7.7 Operations':
   create-an-attribute-getter ends "Let F be CreateBuiltinFunction(steps, 0, name, « », realm)" and
   create-an-attribute-setter ends with the same line carrying 1. Those are constants of the SECTION — every
   attribute getter is 0 and every setter is 1, whatever it is an attribute of — so this states them, and the
   derivation is asserted to agree rather than being trusted to: a getter is declared with no argument position
   and a setter with exactly one, so the two numbers coincide today, and the day a declaration stops agreeing
   is the day one of the two is wrong. Which one is then visible instead of silent.
   THE `name` IN THAT QUOTED LINE IS NOT THE IDENTIFIER, and reading it as one is how every mint in this file
   came to report an attribute's accessor under the member's own name. It is the value §3.7.6 binds one step
   EARLIER — "Let name be the string \"get \" prepended to attribute's identifier", and the setter's "Let name
   be the string \"set \" prepended to id" — so the length and the name are BOTH constants of this section and
   only the length was being read off it. idl_accessor_name performs the prepend, at the mint below. */
static JSValue idl_mint_accessor(JSContext *ctx, const char *name, int stepid, int expect)
{
    int idx = idl_member_of_step(stepid);

    DCHECKF(idx < 0 || idl_member_length_of(idl_member(idx)) == expect,
            "the Web IDL §3.7.6 Attributes %s for '%s' derives length %d from its declaration where §3.7.6 "
            "mints it with %d — an attribute getter declares no argument position and a setter declares "
            "exactly one, so a declaration that says otherwise is describing an operation and not an attribute",
            expect ? "setter" : "getter", name ? name : "?",
            idl_member_length_of(idl_member(idx)), expect);
    return idl_mint_step(ctx, name, stepid, JS_CFUNC_step, expect ? IDL_SEC_SETTER : IDL_SEC_GETTER);
}

/* MINT WITHOUT INSTALLING — for an internal door a C caller holds and calls rather than a property a page
   reads (core/html/focus.c's two, core/frame/remote_object.c's four proxy traps, core/timing/timer.c's §8.7
   Timers re-arm door). It carries the same derived §3.7.7 Operations length as an installed member, because it
   is the same member: a page that reaches one of these objects reads the same `length` off it. */
JSValue idl_step_function(JSContext *ctx, const char *name, int stepid)
{
    return idl_mint_step(ctx, name, stepid, JS_CFUNC_step, IDL_SEC_METHOD);
}

/* The same mint for a member reached with `new` — Web IDL §3.7.1 Interface object's `length`, which is the
   same sentence §3.7.7 states over the effective overload set for CONSTRUCTORS. JS_CFUNC_step_ctor differs
   only in how the receiver slot carries new.target; the pool entry and its name are the same thing. */
JSValue idl_step_constructor(JSContext *ctx, const char *name, int stepid)
{
    DCHECK(idl_member_of_step(stepid) >= 0, "a step constructor was minted for a member this pool never "
                                            "declared");
    DCHECK(name != NULL && *name, "a step constructor was minted with no name");
    /* WEB IDL §3.5 Security HAS NO TYPE FOR A CONSTRUCTOR, and that is not an omission to paper over: §3.7.7
       Operations' create an operation function is what performs a security check, and §3.7.1 Interface
       object's construct steps are a different algorithm with no such step. The receiver slot of a
       JS_CFUNC_step_ctor call carries NEW.TARGET rather than a `this` value, so asking §7.2.1.1 Integration
       with IDL about it would be asking about the wrong object entirely. */
    /* AND §3.7's BRAND HAS NO PLACE HERE FOR THE SAME REASON, WHICH IS WHY IT IS ASSERTED AND NOT ASSUMED. Both
       halves of implementation-check read the receiver slot, and for a constructor that slot is new.target — so
       a declaration that stated a receiver interface would brand-check the CONSTRUCTOR the page reached the
       member through, and `new Iface()` would throw for a receiver §3.7.1 Interface object's construct steps
       never look at. The pool cannot see which mint a declaration will get, so the mint is where the pair is
       checked. */
    DCHECK(idl_member_of_step(stepid) < 0 ||
           idl_member(idl_member_of_step(stepid))->this_is == NULL,
           "a step CONSTRUCTOR declared a receiver interface with idl_this_iface — Web IDL §3.7.1 Interface "
           "object's construct steps run no implementation-check, and the receiver slot of a "
           "JS_CFUNC_step_ctor call carries new.target rather than a `this` value, so the brand would be "
           "asked of the wrong object");
    return idl_mint_step(ctx, name, stepid, JS_CFUNC_step_ctor, IDL_SEC_NONE);
}

void idl_install_method_exposed(JSContext *ctx, JSValueConst target, const char *name, int stepid,
                                IdlExposure exposure)
{
    /* §3.3.13: the member is simply NOT THERE — the same rule an attribute's install already states, asked at
       the same one place. A method needed it the moment an interface whose whole partial is [SecureContext]
       arrived (File System §3's `getDirectory`), and `'getDirectory' in navigator.storage` is exactly the
       feature detection the removal exists to answer honestly. */
    if (!idl_exposed(ctx, exposure)) return;
    idl_install_method(ctx, target, name, stepid);
}

void idl_install_method(JSContext *ctx, JSValueConst target, const char *name, int stepid)
{
    DCHECK(idl_declared_before_seal(stepid), name);
    DCHECK(stepid >= 0, "an IDL member was installed before it was declared");
    JS_SetPropertyStr(ctx, (JSValue)target, name,
                      idl_mint_step(ctx, name, stepid, JS_CFUNC_step, IDL_SEC_METHOD));
}

/* §3.7.7 "Operations"'s UNFORGEABLE HALF, which is the operation twin of idl_install_accessor_unforgeable and
   was the missing one. §3.7.7's define-the-operations reads "let modifiable be false if op is unforgeable and
   true otherwise" and then defines {[[Value]]: method, [[Writable]]: modifiable, [[Enumerable]]: true,
   [[Configurable]]: modifiable}, and §3.7.7's own opening sentence says WHERE: "Regular operations are exposed
   on the interface prototype object, UNLESS the operation is unforgeable … in which case they are exposed on
   every object that implements the interface." So it is the same two differences the attribute half already
   states — the INSTANCE rather than the prototype, and non-configurable — plus the non-writable that only an
   operation has, since a data property is what an operation is.
   THE ATTRIBUTE HALF EXISTING ALONE IS WHY THIS IS NOT A CONVENIENCE. HTML §7.2.4 marks `assign`, `replace`
   and `reload` [LegacyUnforgeable] for the same security reason it marks the attributes, and the only
   installer available was idl_install_method — a JS_SetPropertyStr, so configurable AND writable, which is
   precisely the forgery `[LegacyUnforgeable]` on a Location exists to prevent. A component reaching for the
   wrong installer would not fail; it would ship a Location whose `replace` a page can overwrite. */
void idl_install_method_unforgeable(JSContext *ctx, JSValueConst target, const char *name, int stepid)
{
    DCHECK(idl_declared_before_seal(stepid), name);
    DCHECK(stepid >= 0, "an unforgeable IDL operation was installed before it was declared");
    JS_DefinePropertyValueStr(ctx, (JSValue)target, name,
                              idl_mint_step(ctx, name, stepid, JS_CFUNC_step, IDL_SEC_METHOD),
                              JS_PROP_ENUMERABLE);
}

/* A DOM METHOD WHOSE ALGORITHM IS A STEP MACHINE BUT WHOSE ARGUMENTS ARE NOT THIS MACHINE'S. `click` and
   `dispatchEvent` register their own JSTrampStepDef and have no entry in this pool, so there is nothing here to
   name and nothing to convert — they are a genuinely different thing, not a member that skipped a step, and
   collapsing them into idl_install_method is what made a five-second member report itself as "(none)".
   Two installers because there are two kinds; each asserts it was handed its own kind, so neither can be used
   for the other by mistake. The IDL-shaped future for these is to declare their arguments through the args
   machine like every other member — at which point they move to idl_install_method and this loses a caller.
   RESIDUAL — THIS INSTALLER STILL TAKES `length` AND CANNOT DERIVE IT.
   NOT COVERED: every install reached from core/dom/observable.c and observable_ops.c, the five core/streams
   files, core/events/event_target.c, core/html/html_form.c and element_internals.c, core/idl_async_iter.c and
   core/idl_iter.c — each registers its algorithm with a raw JS_RegisterStepDef and has NO pool entry, so there
   is no declared arity for Web IDL §3.7.7 Operations' length to be computed from and the number at the call
   site is the ONLY statement of it. (A count is deliberately not written here: it is what grep answers, and it
   is wrong the first time one of those components converts.) That is why this is a residual and not a DFAIL: the
   code is correct for what it does, and the assert below is what keeps it from being reached by a member that
   COULD derive — a declared member cannot arrive here, and a raw one cannot reach idl_install_method.
   WHAT THE NEXT DIFF BUILDS: those members' arguments declared through this machine (idl_method_id_step and
   its siblings), one component at a time, at which point each moves to idl_install_method, loses its number,
   and this installer loses a caller — reaching zero, at which point it deletes.
   HOW ITS ABSENCE WOULD SHOW: a `length` a page reads off one of those members disagreeing with its
   IDL — `subscription.addTeardown.length`, `writer.write.length` — with nothing in this engine able to say so,
   which is exactly how seven mixin installs came to disagree with each other. */
void idl_install_step_method(JSContext *ctx, JSValueConst target, const char *name, int length, int stepid)
{
    DCHECK(stepid >= 0, "a step method was installed before its definition was registered");
    DCHECK(idl_member_of_step(stepid) < 0,
           "a DECLARED IDL member was installed through idl_install_step_method — it has a pool entry, so it "
           "installs through idl_install_method, which is what names it");
    JS_SetPropertyStr(ctx, (JSValue)target, name,
                      JS_NewCFunction2(ctx, NULL, name, length, JS_CFUNC_step, stepid));
}

/* WEB IDL SAYS THE MEMBER EXISTS; THE SPEC'S PROSE SAYS IT EXISTS ONLY UNDER A CONDITION THIS USER AGENT DOES
   NOT MEET. Both halves are true, and neither the IDL corpus nor a reader of this prototype can tell that state
   apart from a member nobody has written yet. §8.10.1.1's `taintEnabled()` and `oscpu` are the case: the
   published `html.idl` carries their partial FLAT, and the condition — "if the navigator compatibility mode is
   Gecko" — is a sentence above it. So the ENGINE states it, here, where the members would otherwise go, and
   this call is what makes the statement CODE rather than a comment: it asserts, per realm, that the prototype
   really does lack every name it claims to exclude. Re-add one and this fires at the origin.
   The IDL gap auditor reads the SAME call, so a name declared here leaves its ABSENT list — and the auditor
   checks the other direction, that each name is still a member of the interface in the current corpus, so an
   exclusion for a member the spec has dropped is a loud error rather than a line nobody revisits. That pair is
   the whole difference between this and an exclusion list: a list is unfalsifiable, and this is checked from
   both sides by things that are not this file. */
void idl_members_excluded(JSContext *ctx, JSValueConst proto, const char *iface,
                          const char *const *names, int n, const char *why)
{
    DCHECK(iface != NULL && why != NULL,
           "a conditional-member exclusion was declared with no interface or no reason — the reason IS the "
           "declaration, because it is the spec sentence the IDL could not carry");
    DCHECK(n > 0, "a conditional-member exclusion declared no members");
    DCHECK(JS_IsObject(proto), "a conditional-member exclusion was declared against no prototype");
#if APICLIENT_DEV
    {
        int i;
        for (i = 0; i < n; i++) {
            JSAtom a = JS_NewAtom(ctx, names[i]);
            int has;
            CHECK(a != JS_ATOM_NULL, "an excluded member's name could not be interned");
            has = JS_HasProperty(ctx, proto, a);
            JS_FreeAtom(ctx, a);
            /* The interface INSTALLS a member it declares this user agent cannot have. */
            DCHECK(has == 0, names[i]);
        }
    }
#else
    (void)ctx; (void)proto; (void)iface; (void)names; (void)n; (void)why;
#endif
}

/* THE COLUMN A ROW-FILTERED INSTALL LOOP COVERS — see idl_args.h for why this exists and why it is the RESULT
   that is declared rather than the filter. What is asserted here is exactly what the auditor credits: every
   name the column holds is an OWN property of the object the loop installed onto.
   OWN and not inherited, because the audit files a member under the interface ITS TARGET is: an inherited
   property would answer this question with a member the base interface installed, which is the false COMPLETE
   in a second costume. And the lookup runs NO page code — the object a realm's own install loop wrote on is one
   the engine built, and JS_GetOwnPropertyNoUserCode aborts rather than running a trap or an undeclared exotic
   hook in a C activation with no flow base under it.
   The name is what the DCHECK reports, because the name is the whole finding: it is the member the gap audit
   would have counted installed on the strength of this declaration and the page would not have found. */
void idl_install_covers_column(JSContext *ctx, JSValueConst target, const char *const *column,
                               int n, size_t stride, const char *why)
{
    DCHECK(why != NULL,
           "an install-coverage declaration carries no reason — the reason is the sentence saying why this "
           "loop's row filter cannot remove a NAME, and without it the declaration is an assertion of itself");
    DCHECK(column != NULL, "an install-coverage declaration named no column");
    DCHECK(n > 0, "an install-coverage declaration named an empty column — a loop that installs nothing is not "
                  "a subset the audit needs told, it is a site with no members to credit");
    DCHECK(stride >= sizeof(const char *),
           "an install-coverage declaration strides by less than the pointer it strides over, so it reads one "
           "row's name out of two rows' bytes");
    DCHECK(JS_IsObject(target), "an install-coverage declaration was made against something that is not an "
                                "object, so there is nothing the loop can have installed on");
#if APICLIENT_DEV
    {
        int i;
        for (i = 0; i < n; i++) {
            const char *name;
            JSAtom a;
            int has;
            /* the column is a FIELD of a wider row, so it is read by stride out of the row's bytes — through
               memcpy, because a pointer read out of a byte cursor is the strict-aliasing violation that
               passes at -O0 and segfaults a directory at -O1 */
            memcpy(&name, (const unsigned char *)column + (size_t)i * stride, sizeof name);
            CHECK(name != NULL, "an install-coverage declaration's column holds a NULL where a member name is");
            a = JS_NewAtom(ctx, name);
            CHECK(a != JS_ATOM_NULL, "a covered member's name could not be interned");
            has = JS_GetOwnPropertyNoUserCode(ctx, NULL, target, a);
            JS_FreeAtom(ctx, a);
            /* The loop's row filter removed a NAME, or this column is not the one it walked: the audit credits
               this member to the target's interface and the page cannot find it. */
            DCHECK(has == 1, name);
        }
    }
#else
    (void)ctx; (void)target; (void)column; (void)n; (void)stride; (void)why;
#endif
}

/* THE POOL IS RELEASED IN TWO HALVES, BECAUSE THE TWO THINGS IT HOLDS HAVE DIFFERENT LIFETIMES and one call
 * could only ever get one of them right. It was one call, it ran BEFORE the frontier and before the runtime in
 * two of the three hosts, and both halves were wrong there at once:
 *
 *   the ATOMS are the RUNTIME's — JS_FreeAtom needs a live runtime, and JS_FreeRuntime's own atom walk reports
 *   one still held — so they go back while the runtime lives;
 *
 *   the BLOCKS are the RUNTIME's CREDITOR. Each holds the JSTrampStepDef that JS_RegisterStepDef BORROWED
 *   ("must outlive the runtime — static data, as the engine's own are"): every live machine names it through
 *   `hdr.def`, tramp_step_state_free_1 reads `def->fini` and `def->visit` through it to tear the machine down,
 *   and JS_FreeRuntime's [stepleak] census reads `def->steps` and `def->algorithm` to name each machine nobody
 *   finished. Freeing the blocks first is therefore a use-after-free of the runtime's own table, and it was
 *   reached the moment the smoke fixture ended with a real parked residue: a flow suspended inside an IDL
 *   member was released by flow_registry_free with the pool already gone, `idl_args_result` ran through the
 *   freed definition, and the ONLY thing that said so was the range assert at its first line finding g_n == 0.
 *   The three hosts disagreed about the order — the WPT runner had solver_agent_free FIRST and was correct by
 *   accident of position — which is exactly how a hand-copied teardown hides a contract violation.
 *
 * The ORDER between them is not prose either: the atom half asserts that no machine is live, so a host that
 * releases the pool with a flow still parked inside a member aborts AT THE RELEASE naming that, and the block
 * half asserts the atom half has run. */
void idl_args_free(JSContext *ctx)
{
    int i, k;

    /* NO MACHINE MAY BE LIVE WHEN THIS POOL STARTS GOING BACK. Every live step state — an IDL member's or any
       other — names a definition this pool may own and reads it at its teardown, so "the frontier is gone"
       (flow_registry_free asserts the same count for the same reason) is the precondition for touching the
       pool at all, and it is the ONE ordering statement a host has to get right. It is asked of the runtime the
       POOL's definitions were registered with rather than of the caller's realm: a host that declared no member
       has no definition anyone can be resting against, and it may not have a realm either. */
    DCHECK(g_rt == NULL || JS_StepMachineCount(g_rt) == 0,
           "the IDL member pool was released with STEP MACHINES still live — a flow parked inside an IDL member "
           "reads this pool at its teardown (its definition through hdr.def, its member through hdr.arg), so the "
           "pool must go back AFTER the frontier that holds those flows: release solver_agent_free first");
    for (i = 0; i < g_n; i++) {
        for (k = 0; k < idl_member(i)->dict_n; k++)
            JS_FreeAtom(ctx, idl_member(i)->dict_atoms[k]);
        free(idl_member(i)->dict_atoms);
        idl_member(i)->dict_atoms = NULL;
        idl_member(i)->dict_n = 0;
    }
    /* And the NESTED dictionaries' names, interned once per declaration rather than once per member — the same
       runtime-lifetime atoms, released with the runtime. */
    for (i = 0; i < g_ndicts; i++) {
        for (k = 0; k < g_dicts[i].d->n; k++)
            JS_FreeAtom(ctx, g_dicts[i].atoms[k]);
        free(g_dicts[i].atoms);
    }
    free(g_dicts);
    g_dicts = NULL;
    g_ndicts = 0;
    /* And §3.3.10 [PutForwards]'s two names per carrier, interned at the declaration for the same reason the
       dictionary members' are — a keyed request holds its atom across a suspension — and therefore given back
       here, with a runtime still to give them back to. */
    for (i = 0; i < g_pf_n; i++) {
        JS_FreeAtom(ctx, g_pf[i].attr);
        JS_FreeAtom(ctx, g_pf[i].forward);
    }
    free(g_pf);
    g_pf = NULL;
    g_pf_n = 0;
    /* AND THE RUNTIME IS DONE WITH THIS POOL, which is the statement the block half rests on. */
    g_rt = NULL;
}

/* THE BLOCKS, AFTER JS_FreeRuntime — see above, and idl_async_iter_free, which is released beside this one for
   exactly the same reason. The member records and their definitions go together because they are allocated
   together and indexed identically. */
void idl_args_pool_free(void)
{
    int i;

    DCHECK(g_rt == NULL,
           "the IDL member pool's BLOCKS were released before its atoms were — the atom release is what states "
           "the runtime is done with this pool, and each block holds a JSTrampStepDef the runtime borrowed and "
           "requires to outlive it");
    for (i = 0; i < g_n; i++) {
        DCHECK(idl_member(i)->dict_atoms == NULL,
               "an IDL member still holds interned dictionary names at the pool's block release — the atoms are "
               "the runtime's half and there is no runtime left to give them back to");
        /* The declared type list, copied at the declaration and owned for the runtime's life — the third thing
           this pool allocates per member, and it is freed here beside the other two. */
        free(idl_member(i)->types);
        idl_member(i)->types = NULL;
        /* §3.6 steps 15.4.1 and 16.1's declared defaults — the strings themselves are the declaring
           component's statics;
           the array naming them is this pool's, allocated by the first default a member declared. */
        free(idl_member(i)->arg_dflts);
        idl_member(i)->arg_dflts = NULL;
        /* §3.2.26 step 1's declared T's — the array is this pool's, allocated by the first position a member
           stated one at, and it holds nothing but the enumerator and two flags. */
        free(idl_member(i)->arg_views);
        idl_member(i)->arg_views = NULL;
        /* §3.2.15's per-position `I` — the predicates and the identifiers are the declaring component's, and
           the array naming them is this pool's, allocated by the first position a member stated one at. */
        free(idl_member(i)->arg_ifaces);
        idl_member(i)->arg_ifaces = NULL;
        /* §3.2.18's per-position value lists — the lists themselves are the declaring component's statics, and
           the array naming them is this pool's, allocated by the first position a member stated one at. */
        free(idl_member(i)->arg_enums);
        idl_member(i)->arg_enums = NULL;
        /* The joined stage labels — the strings themselves are statics belonging to the member and to this
           file; the array that names them is this pool's, and it is part of the BORROWED definition. */
        free((void *)idl_member(i)->steps);
        idl_member(i)->steps = NULL;
    }
    for (i = 0; i < g_nchunks; i++)
        free(g_chunks[i]);
    free(g_chunks);
    g_chunks = NULL;
    g_nchunks = 0;
    g_n = 0;
    /* THE STEP-ID MAP, which no release had ever named. It is malloc'd and holds no JSValue, so it was
       invisible to both of JS_FreeRuntime's censuses and visible only to LeakSanitizer — and a next agent in
       this process would have read the previous one's member indices out of it. */
    free(g_step2mem);
    g_step2mem = NULL;
    g_step2mem_cap = 0;
    /* AND §3.7.6's GLOBAL ATTRIBUTE TABLE, for the same reason: it is malloc'd, holds no JSValue and no atom,
       and its entries name a previous agent's members. It goes back HERE rather than with the atoms above,
       because the function objects that carry indices into it are the realms', released with the runtime. */
    free(g_gattr);
    g_gattr = NULL;
    g_gattr_n = g_gattr_cap = 0;
    g_sealed = false;
    g_sealed_at = 0;
}
