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
#include "core/frame/secure_context.h"   /* §3.9's exposure conditions: HTML §8.1.3.5's answer for this realm */
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
    /* §3.2.4.10's [EnforceRange], which is four of that section's own steps and not a bound this file chose:
       "if x is NaN, +∞ or −∞, then throw a TypeError"; "let x be sign(x)·floor(abs(x))"; "if x < lowerBound or
       x > upperBound, then throw a TypeError". The bounds are `unsigned long`'s, which is the only type in this
       build carrying the attribute — a second one states its own here rather than sharing a width parameter,
       because the whole point of the attribute is that the range is part of the TYPE. */
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
               "`undefined` here is §3.6 step 14.2 NOT DECLARED: an optional argument whose IDL writes `= …` "
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

/* §3.6 step 14.2's DEFAULT VALUE AT ONE POSITION, stated in the same two fields IdlDictMember already uses for
   a dictionary member's: the KIND, and the string the IDL wrote for the one kind that carries one. It is a
   struct rather than two parallel arrays so that a member's positions cannot be described half in one and half
   in the other. */
typedef struct {
    IdlDictDefault kind;
    const char    *str;
} IdlArgDefault;

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
       declaration's single `first_optional` cannot carry. `split_at` is the position such a split sits at
       (idl_type_is_length_split), or -1 for the members that declare none — which is all but two of them.
       `split_longer_optional` is the LONGER entry's OWN first optional position, declared through
       idl_overload_split_optional_from and asserted present at the seal, because the entry that survives
       steps 3-4 is the one whose optionality step 15.3 reads and the shorter entry's is the wrong list. */
    int        split_at;
    int        split_longer_optional;
    /* §3.6 STEP 14.2's DEFAULT VALUES, one entry per position the IDL lists — see idl_arg_default. NULL for a
       member declaring none, which is nearly all of them; allocated by the first declaration that names one,
       and freed with the pool exactly as `types` is. A position whose entry is IDL_DEFAULT_NONE has no default,
       which is what §3.6's absent rule is for. */
    IdlArgDefault *arg_dflts;
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
    /* §3.2.18's VALUE LIST for this member's IDL_ENUM position — the list IS the type, so a declaration
       carrying one is a declaration stating what it takes. It lived only on IdlDictMember, which is why a
       positional enumeration stood at a DCHECK naming this field as the thing to build; the enumeration
       attribute that needed it is HTML §7.2.5's `scrollRestoration`, whose setter a router calls. NULL for
       every member with no such position, which is nearly all of them. */
    const char *const *enum_values;
    const char *name;       /* what to call this member in a diagnostic; set when it is installed */
} IdlMember;

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

void idl_args_seal(void) { idl_seal_check_splits(); g_sealed = true; g_sealed_at = g_n; }

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
   at init, never over the page's data. */
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
   ONE FUNCTION, WHEREVER IT WAS DECLARED: §3.2.17 step 4.1.5 (a dictionary member) and §3.6 step 14.2 (an
   optional positional argument) place the SAME kinds of value, so a second copy for the argument side would be
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
 * ONE LEVEL of a `sequence<(DOMString or D)>`: the sequence's own iterator, and the D-dictionary the element it
 * is standing on is being converted as. A frame is pushed when a member of that dictionary is itself a sequence
 * of the same shape, so the machine parks at the element it is on AT WHATEVER DEPTH — which a C loop over any
 * of it could not, and which C recursion could not either, since a park has to be a RETURN.
 * The frames live in the state block itself (idl_frames), because a deep fork BYTE-COPIES the state and re-takes
 * only what `visit` names: a malloc'd list would be one allocation two flows both free. */
typedef struct {
    IterCursor  cur;        /* the sequence's iterator, over `src` */
    JSValue     src;        /* the value being iterated (owned) */
    JSValue     list;       /* the elements converted so far (owned) */
    JSValue     esrc;       /* the element whose dictionary is being read (owned) */
    JSValue     eout;       /* the object that element's converted members are placed on (owned) */
    JSValue     mv;         /* one member's value between its read and its conversion (owned) */
    const IdlDictDecl *d;   /* the element type's dictionary arm */
    const JSAtom *atoms;    /* its member names, interned when the member was declared */
    uint32_t    n;          /* how many elements `list` holds */
    int         mi;         /* the member cursor */
    uint8_t     phase;      /* IDL_CONV_* */
    uint8_t     mphase;     /* 0 = read the member, 1 = convert what was read, 2 = place it */
} IdlConvFrame;

enum { IDL_CONV_PULL = 0, IDL_CONV_STRING, IDL_CONV_MEMBERS };

typedef struct {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    int       i;        /* THE RESUME POINT: the argument being coerced */
    int       n;        /* how many of them there are */
    JSValue   result;   /* the body's answer (owned) */
    int       dict_i;   /* THE OTHER RESUME POINT: the dictionary member being read */
    uint8_t   dict_phase;   /* 0 = read the member, 1 = convert what was read. Both can park, so a member needs
                               two resume points, not one — a resume in the CONVERSION must not re-read. */
    JSValue   dict_v;   /* the member's value between those two phases (owned) */
    /* A VARIADIC member's converted arguments, which cannot live in the per-position vector the declaration
       sizes: `ul.append(...items)` has as many as the page has items, and no declaration knows that number. It
       is an ARRAY rather than a heap block because that is what `visit` can carry — a deep fork byte-copies the
       state and re-takes what visit names, so a block pointer would be SHARED by two flows that both free it,
       and a pointer into the state itself would survive the copy still aimed at the original. One owned value,
       one v->val, no new ownership contract. Non-variadic members never touch it: their arguments are exactly
       the positions their declaration lists, which is exactly what the vector holds. */
    JSValue   conv;
    JSValue   vstage;   /* the variadic argument being converted, before it joins `conv` */
    /* §3.2.21's `sequence<T>` CONVERSION: the ES iterator protocol, whose every step is the page's code — so the
       cursor and the list it fills are the machine's own state, and the resume comes back to the element it was
       on. The list is a JS Array for the reason `conv` is one: a deep fork byte-copies the state and re-takes
       only what `visit` names, so an owned heap block would be freed twice. */
    IterCursor seq;
    JSValue    seq_list;
    uint32_t   seq_n;
    /* 0 = NOT STARTED, 1 = pull the next element, 2 = convert the one just pulled. "Not started" is a stage of
       its own rather than a null list, because a zeroed state's JSValue is the INTEGER 0 and not JS_UNDEFINED —
       JS_TAG_INT is 0 — so "have I built the list yet" read off the value is always "yes". */
    uint8_t    seq_phase;
    /* §3.2.25's ARM FOR A `(DOMString or sequence<DOMString>)` POSITION, which is a RESUME POINT because the
       decision is `? GetMethod(V, %Symbol.iterator%)` — the page's code. Without it a park inside the page's
       @@iterator getter would come back, re-enter the conversion loop, and ask for the property a second time;
       with it the resume finds the arm already chosen and continues into the string coercion or the cursor.
       It is shared between an argument position and a dictionary member's for exactly the reason `seq` is:
       arguments are converted strictly left to right, so the two are never in flight at once. */
    uint8_t    uni_phase;
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
    /* THE NESTED CONVERSION'S STACK POINTER — how many IdlConvFrame frames (which live immediately after this
       state, one per declared sequence level) are live. Zero means no nested conversion is in flight, which is
       what tells a resumed member's sequence branch to continue rather than start over. */
    uint8_t    conv_sp;
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
    return t == IDL_DICT || t == IDL_DICT_OR_BOOL_FIRST || t == IDL_STRING_OR_DICT ||
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
   there is one entry and the declaration's own number IS that list; for the two that have one, the surviving
   entry decides. idl_args_seal asserts that such a member declared the longer entry's number, so there is
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

/* THE TWO-SIDED HALF OF THAT DECLARATION, run once when the platform is sealed. A member whose types name a
   length-differing split and that never said where the LONGER entry's optional arguments begin is a member
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
        for (k = 0; k < m->nargs; k++)
            DCHECK(!idl_type_is_length_split(m->types[k]) || m->split_at == k,
                   "a member's length-differing §3.6 split was not recorded at the position its type list "
                   "declares it — the position is READ from the types at declaration, so the two disagreeing "
                   "means a second split was declared and one of them decides every arity");
    }
}

static int idl_declared_positions(const IdlMember *m)
{
    DCHECK(!m->variadic || m->nargs >= 1,
           "a variadic member declared no types at all — the tail's type is what `T...` means, so there is "
           "always at least one");
    return m->variadic ? m->nargs - 1 : m->nargs;
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

/* Begin one sequence level over `src`. Returns -1 with a throw live if its list could not be allocated. */
static int idl_conv_push(JSContext *ctx, JSIdlArgsState *s, const IdlMember *m, JSValueConst src,
                         const IdlDictDecl *d)
{
    IdlConvFrame *f;

    DCHECK(d != NULL, "a `sequence<(DOMString or D)>` was converted with no dictionary named for its union's "
                      "second arm");
    CHECK(s->conv_sp < m->conv_depth,
          "a nested IDL conversion went deeper than the DECLARED type tree — the depth is computed from the "
          "declaration and the state is sized for it, so this means the two have drifted apart");
    f = &idl_frames(m, s)[s->conv_sp++];
    idl_conv_frame_clear(ctx, f);
    iter_cursor_init(&f->cur);
    f->d = d;
    f->atoms = idl_dict_atoms(d);
    f->src = JS_DupValue(ctx, src);
    f->list = JS_NewArray(ctx);
    if (JS_IsException(f->list)) { f->list = JS_UNDEFINED; return -1; }
    return 0;
}

static void idl_conv_pop(JSContext *ctx, JSIdlArgsState *s, const IdlMember *m)
{
    DCHECK(s->conv_sp > 0, "a nested IDL conversion popped a frame it never pushed");
    idl_conv_frame_clear(ctx, &idl_frames(m, s)[--s->conv_sp]);
}

/* DRIVE the stack one re-entry's worth. Returns >0 (the caller returns it — the machine is parked inside the
   page's iterator or one of its getters), 0 with *pout holding the converted sequence (owned), or -1 with a
   throw live. Every arm that parks does so with the cursor standing exactly where it was. */
static int idl_conv_run(JSContext *ctx, JSIdlArgsState *s, const IdlMember *m, JSValue in, JSValue *pout,
                        JSValue **out_cb, int *out_argc)
{
    int r;

    DCHECK(s->conv_sp > 0, "the nested conversion was driven with no frame under it");
    for (;;) {
        IdlConvFrame *f = &idl_frames(m, s)[s->conv_sp - 1];

        if (f->phase == IDL_CONV_PULL) {
            r = iter_cursor_run(ctx, &s->hdr, &f->cur, f->src, in, out_cb, out_argc);
            in = JS_UNDEFINED;
            if (r > 0) return r;          /* parked ON THIS ELEMENT, at THIS depth */
            if (r < 0) return -1;
            if (f->cur.done) {
                JSValue done = f->list;

                f->list = JS_UNDEFINED;
                idl_conv_pop(ctx, s, m);
                if (s->conv_sp == 0) { *pout = done; return 0; }
                {   /* the frame below is the MEMBER that named this sequence, waiting for its value */
                    IdlConvFrame *p = &idl_frames(m, s)[s->conv_sp - 1];

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

            r = step_tostring_run(ctx, &s->hdr, f->cur.value, in, &str, out_cb, out_argc);
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
                        r = step_getprop_run(ctx, &s->hdr, f->esrc, f->atoms[f->mi], in, &f->mv,
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
                        if (idl_conv_push(ctx, s, m, f->mv, dm->dict) < 0) return -1;
                        pushed = true;
                        break;
                    } else if (dm->type == IDL_DOMSTRING || dm->type == IDL_DOMSTRING_NULLABLE) {
                        /* `DOMString?`: null is the IDL null and never the four characters "null". */
                        if (!(dm->type == IDL_DOMSTRING_NULLABLE && JS_IsNull(f->mv))) {
                            JSValue str = JS_UNDEFINED;

                            r = step_tostring_run(ctx, &s->hdr, f->mv, in, &str, out_cb, out_argc);
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
static int idl_union_seq_arm(JSContext *ctx, JSIdlArgsState *s, uint8_t *phase, JSValueConst v, JSValue *pin,
                             IdlArgType *pout, IdlArgType seq_arm, IdlArgType flat_arm,
                             JSValue **out_cb, int *out_argc)
{
    /* §3.2.25 step 11 asks its whole question of an OBJECT; every other value reaches the flat clause without
       a read. (§3.6's caller has already refused a non-object, so this line is the union's alone.) */
    if (*phase == IDL_UNI_ASK && !JS_IsObject(v))
        *phase = IDL_UNI_STRING;
    if (*phase == IDL_UNI_ASK) {
        JSValue method = JS_UNDEFINED;
        int r = step_getprop_run(ctx, &s->hdr, v, JS_WellKnownSymbolAtom(JS_WKS_ITERATOR), *pin, &method,
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
            DCHECK(s->seq_phase == 0,
                   "the union's sequence arm was chosen while a sequence was already in flight on this "
                   "machine — one argument is converted at a time, so the cursor has exactly one user");
            s->seq_list = JS_NewArray(ctx);
            if (JS_IsException(s->seq_list)) {
                s->seq_list = JS_UNDEFINED;
                JS_FreeValue(ctx, method);
                return -1;
            }
            iter_cursor_init_from_method(ctx, &s->seq, method);   /* CONSUMES `method` */
            s->seq_phase = 1;
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
    v->val(ctx, &s->dict_v);
    v->val(ctx, &s->conv);
    v->val(ctx, &s->vstage);
    iter_cursor_visit(ctx, &s->seq, v);
    v->val(ctx, &s->seq_list);
    v->val(ctx, &s->ce_exc);
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
    /* EVERY declared frame, not only the live ones. A popped frame holds JS_UNDEFINED and a never-used one the
       zeroed state's non-refcounted integer, so visiting all of them takes no reference it should not — where a
       loop bounded by `conv_sp` would silently drop whatever a frame still held if the cursor and the frames
       ever disagreed. */
    for (i = 0; i < m->conv_depth; i++)
        idl_conv_frame_visit(ctx, &idl_frames(m, st)[i], v);
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
   comes straight back here. 0 is the walk finished, and the buffer is released on that edge and nowhere else. */
static int idl_tree_drain(JSContext *ctx, JSIdlArgsState *s)
{
    int r;

    if (!s->tree) return 0;
    r = g_tree->step(ctx, s->tree, &s->hdr);
    if (r) {
        DCHECK(r == JS_STEP_YIELD || r == JS_STEP_FORK,
               "§4.2.3's tree-steps walk answered with a step code this drain cannot forward — it may rest "
               "(JS_STEP_YIELD) or fork (JS_STEP_FORK), and a request that parks on the PAGE's code would run "
               "that code between two nodes' insertion steps, which is not the order §4.2.3 states");
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
    if (JS_IsException(cb_result) && !(m->step && m->step->catches_abrupt)) return JS_STEP_ABRUPT;

    /* THE DRAIN COMES FIRST ON EVERY RE-ENTRY, before the conversion loop or the body, because the steps the
       previous step recorded must finish before anything else this member does. */
    if (s->tree) {
        JS_FreeValue(ctx, cb_result);
        r = idl_tree_drain(ctx, s);
        if (r) return r;
        if (s->tree_after_body) return idl_ce_finish(ctx, s, JS_UNDEFINED, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
    }

    if (s->hdr.stage == 0) {
        /* A RECORD NOBODY OWNS. Every tree mutation happens inside a declared member's body and is drained
           before that member returns, so anything still waiting here was written by something that is not a
           declared member — a raw JS_CFUNC_DEF that mutates the tree, which is the one shape this machine
           cannot reach. Its insertion steps would never run: an inserted <script> would not execute and a
           custom element would not upgrade, with nothing to show for it. */
        DCHECK(!g_tree || !g_tree->recorded(),
               "a DOM mutation recorded tree steps outside any declared member — declare that member so it "
               "converges on this machine, which is the only thing that drains them");
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
           14.2: "if the argument is optional and it has a default value, set the value to that default" runs
           for EVERY declared argument, not only for the ones the page reached — so a member the page stopped
           short of still receives the IDL's value at every defaulted position behind it.
           IT WAS THE DICTIONARY CLAUSE ALONE, AND THE ONE EXISTING USER WAS SAVED BY ACCIDENT. IndexedDB
           §4.4's `transaction(storeNames, optional IDBTransactionMode mode = "readonly", optional
           IDBTransactionOptions options = {})` declares a default at position 1, and `db.transaction(["s"])`
           reached the body with argv[1] undefined — except that position 2 is a dictionary, so the clause
           above extended the count past 1 and the default was placed anyway. The body's own DCHECK
           ("§3.6 step 14.2 places one whether or not the page passed one") states the rule this now keeps: a
           member whose defaulted position has no dictionary behind it — Console §1.4.1's
           `time(optional DOMString label = "default")` — would have reached its body with nothing there. */
        if (!m->variadic) {
            for (r = s->n; r < m->nargs; r++)
                if (idl_type_is_dictionary(m->types[r]))
                    s->n = r + 1;
        }
        /* THE DEFAULT CLAUSE APPLIES TO A VARIADIC MEMBER TOO, and that is not symmetry for its own sake — a
           variadic member's DECLARED positions are the ones BEFORE the tail, and §3.6 step 14.2 reads a default
           for each of them exactly as it does for a member with no tail. Console §1.4.2's
           `timeLog(optional DOMString label = "default", any... data)` is one: `console.timeLog()` passes
           nothing, so the count above is 0, and without this the body would be handed an EMPTY argument vector
           for a member whose IDL guarantees a label at position 0. The dictionary clause stays where it is
           because a variadic member may not declare one at all — the conversion asserts that a few lines on. */
        for (r = s->n; r < idl_declared_positions(m); r++)
            if (m->arg_dflts && m->arg_dflts[r].kind != IDL_DEFAULT_NONE)
                s->n = r + 1;
        s->result = JS_UNDEFINED;
        s->dict_v = JS_UNDEFINED;
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
        IdlArgType t = (s->i < m->nargs) ? m->types[s->i]
                     : (m->variadic ? m->types[m->nargs - 1] : IDL_ANY);
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
        bool step4_only_longer = longer_survived && s->i == m->split_at;
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
            /* §3.6 STEP 14.2: an optional argument whose IDL writes `= …` is not absent — it holds THAT value,
               which is already an IDL value and is therefore placed rather than coerced. A position with no
               declared default is absent, which is the undefined the body reads as "not given". */
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
            DCHECK(m->iface != 0, "a member declared an interface-or-string union with no interface to brand "
                                  "against — the class is half of what that type states");
            t = idl_is_iface(a, m->iface) ? IDL_ANY : IDL_DOMSTRING;
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
            r = idl_union_seq_arm(ctx, s, &s->uni_phase, a, &cb_result, &t,
                                  IDL_SEQUENCE_DOMSTRING, IDL_DOMSTRING, out_cb, out_argc);
            if (r > 0) return r;   /* parked ON THE ARM's read; the resume finds the arm already chosen */
            if (r < 0) return JS_STEP_ABRUPT;
        }

        /* §3.2.25 over `(double or sequence<double>)` — the same step 11.2 read, the same park, two other arms.
           Resolved AFTER the concolic pass-through above for the reason every union here is: unknown external
           input IS an object, and asking it for @@iterator would read a property off an attacker's value. */
        if (t == IDL_DOUBLE_OR_SEQUENCE) {
            r = idl_union_seq_arm(ctx, s, &s->uni_phase, a, &cb_result, &t,
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
                r = idl_union_seq_arm(ctx, s, &s->ovl_phase, a, &cb_result, &t,
                                      IDL_SEQUENCE_OBJECT, IDL_DICT, out_cb, out_argc);
                if (r > 0) return r;   /* parked ON THE OVERLOAD's read; the resume finds it already chosen */
                if (r < 0) return JS_STEP_ABRUPT;
            }
        }

        if (t == IDL_DICT || t == IDL_DICT_OR_BOOL_FIRST) {
            /* WHICH ARM OF `(dictionary or boolean)` THIS FLOW IS ON — §3.2.25 step 11 against steps 12/18.
               For a dictionary type there is no union and no arm, and the answer is the type's own §3.2.17
               step 1: an Object is read, anything else has already thrown or defaulted. */
            bool object_arm;
            /* A DICTIONARY ARGUMENT IS BUILT IN PLACE, in the vector slot, rather than through `slot` and
               `placed:` — its members are written onto the object across many re-entries, so there is nothing to
               append at the end. That is why a VARIADIC member may not declare one at all: its body reads the
               converted arguments out of `conv`, which this store never reaches, and the member would receive an
               undefined where its dictionary belongs. The old form of this assert allowed one at a declared
               position and only refused it in the tail, which is the half of the rule that leaves a hole. */
            DCHECK(!m->variadic,
                   "a VARIADIC member declared a dictionary argument — a dictionary is built in the argument "
                   "vector and a variadic member's body reads `conv`, so the two never meet");
            /* `optional D options = {}`: undefined and null have no members to read, so every one defaults and
               no page code runs. An object's members are read IN ORDER and each is converted by ITS OWN type,
               parking on either half. A `required` member is checked here rather than in the body, because
               `required` is part of the TYPE the declaration states. */
            if (JS_IsUndefined(*idl_arg_slot(m, s, s->i)))
                *idl_arg_slot(m, s, s->i) = JS_NewObject(ctx);
            /* §3.2.17 step 1: a value that is NOT undefined, null or an Object is a TypeError before any member
               is read — `new Blob([], 123)` throws, and reading `123.type` instead answered undefined and built
               a Blob. The union form is exempt because its whole rule is that a non-object IS a member. */
            if (t == IDL_DICT && !JS_IsObject(a) && !JS_IsUndefined(a) && !JS_IsNull(a)) {
                JS_FreeValue(ctx, cb_result);
                JS_ThrowTypeError(ctx, "the dictionary argument is neither an object, null nor undefined");
                return JS_STEP_ABRUPT;
            }
            /* §3.2.25's ARM FOR `(AddEventListenerOptions or boolean)`, ASKED HERE — at the site that acts on
               it — because this union's two arms ARE the two halves of the dictionary this block builds, and
               there is no third type to collapse `t` to in the arm block above.
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
               no-policy run answers exactly as it did and the boolean world is the one the fork ADDS. */
            object_arm = JS_IsObject(a);
            if (t == IDL_DICT_OR_BOOL_FIRST && concolic_is(a)) {
                int arm = 0, rc;

                DCHECK(idl_concolic_rule(t) == IDL_CONCOLIC_FORKS,
                       "this conversion forked §3.2.25's arm for a type idl_args.h does not declare as one it "
                       "forks — the SITE and the rule are the two halves of one statement, and a type that "
                       "loses its FORKS rule while this ask stands would fork an arm the pass-through above "
                       "had already crossed the value at");
                /* `cb_result` is this machine's outstanding answer and step_fork_run takes no `in` to hand it
                   to, so it is released HERE, exactly as the boolean arm below releases it — the sibling's
                   snapshot is taken at this return and nothing of the caller's may be live across it. */
                JS_FreeValue(ctx, cb_result);
                cb_result = JS_UNDEFINED;
                rc = step_fork_run(ctx, &s->hdr, a, "§3.2.25 (dictionary or boolean) arm", 2, &arm);
                if (rc) return rc;
                object_arm = (arm == 0);
            }
            if (!object_arm) {
                JS_FreeValue(ctx, cb_result);
                cb_result = JS_UNDEFINED;
                /* §2.7 "flatten": a non-object IS the first member's boolean. There is nothing to READ, so
                   this runs none of the page's code either way.
                   AND THE VALUE THE UNION SAYS *IS* THAT MEMBER CROSSES AS ITSELF WHEN IT IS UNKNOWN. DOM §2.7
                   "Interface EventTarget"'s flatten options is "If options is a boolean, then return options"
                   — the member IS V, so ToBoolean here would be this boundary coercing unknown external input
                   after the fork above decided only that V is on the boolean arm, which is not the same fact
                   as V being TRUE. A concolic object is truthy, so the coercion pinned `capture` for every
                   unknown while destroying the taint that says it is one. It is placed instead, which is what
                   every other type does with unknown input, and the taint reaches the member's readers. */
                if (t == IDL_DICT_OR_BOOL_FIRST && m->dict_n > 0) {
                    DCHECK(m->dict[0].type == IDL_BOOLEAN,
                           "a (dictionary or boolean) union declared a non-boolean first member — the union's "
                           "rule is that the bare value IS that member");
                    JS_SetPropertyStr(ctx, *idl_arg_slot(m, s, s->i), m->dict[0].name,
                                      concolic_is(a) ? JS_DupValue(ctx, a)
                                                     : JS_NewBool(ctx, JS_ToBool(ctx, a)));
                }
                for (r = 0; r < m->dict_n; r++) {
                    if (m->dict[r].required)
                        return JS_ThrowTypeError(ctx, "required member %s is undefined", m->dict[r].name),
                               JS_STEP_ABRUPT;
                    /* §3.2.17 step 4.1.5 applies with nothing to read, too: a member whose IDL writes a
                       DEFAULT exists on the converted dictionary even when there was no object to look it up
                       on, which is what makes `new Sanitizer(null)`'s members the ones the IDL states. */
                    if (m->dict[r].dflt != IDL_DEFAULT_NONE) {
                        JSValue dv = idl_default_value(ctx, &m->dict[r]);

                        if (JS_IsException(dv)) return JS_STEP_ABRUPT;
                        JS_SetPropertyStr(ctx, *idl_arg_slot(m, s, s->i), m->dict[r].name, dv);
                    }
                }
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
                /* AN OVERLOAD SPLIT IS NOT A TYPE A MEMBER CAN HAVE. §3.6 resolves between two ENTRIES of an
                   effective overload set at an ARGUMENT INDEX; a dictionary member has no entries and no
                   index, so a member declared this way names a decision there is nothing to decide. Asserted
                   here because the member loop below would otherwise fall past every arm and place the page's
                   value unconverted, which is the silent shape rather than the loud one. */
                DCHECK(mt != IDL_SEQUENCE_OBJECT_OR_DICT,
                       "a dictionary member was declared as a §3.6 overload split — that type resolves between "
                       "two overload ENTRIES at an argument position, and a member is inside one entry already; "
                       "the member's own IDL names either the sequence or the dictionary, never both");
                /* §3.2.17 step 4.1.5's DEFAULT comes first, because it is the difference between a member
                   that does not exist and one that exists holding what the IDL wrote. It is already an IDL
                   value, so nothing converts it. */
                if (JS_IsUndefined(s->dict_v) && dm->dflt != IDL_DEFAULT_NONE) {
                    s->dict_v = idl_default_value(ctx, dm);
                    if (JS_IsException(s->dict_v)) { s->dict_v = JS_UNDEFINED; return JS_STEP_ABRUPT; }
                    mt = IDL_ANY;
                }
                /* An ABSENT member is not converted: `undefined` on a dictionary means the member is not
                   there, and running ToString over it would write the four characters `undefined` where the
                   spec puts nothing. IDL_BOOLEAN is the exception because ToBoolean(undefined) is false, which
                   IS the `= false` default a member declared that way carries — and IDL_BOOLEAN_NO_DEFAULT is
                   the member that declares no default, which is exactly why it is NOT exempt here: it stays
                   undefined so the body can tell absence from false (see idl_args.h). */
                if (JS_IsUndefined(s->dict_v) && mt != IDL_BOOLEAN)
                    mt = IDL_ANY;
                /* The same boundary rule the arguments follow: unknown external input crosses as ITSELF, so a
                   concolic member keeps forking control flow instead of collapsing at a coercion. */
                if (mt != IDL_ANY && concolic_is(s->dict_v))
                    mt = IDL_ANY;
                /* §3.2.25 over `(DOMString or sequence<DOMString>)` ON A DICTIONARY MEMBER — the same union the
                   argument path resolves, resolved here so the arm's @@iterator read parks on the MEMBER it is
                   on. It rewrites `mt` and the arms below convert what it chose; step 2's null is placed
                   directly, because the IDL null runs none of the page's code and is not a string arm. */
                if (mt == IDL_DOMSTRING_OR_SEQUENCE || mt == IDL_DOMSTRING_OR_SEQUENCE_NULLABLE) {
                    if (mt == IDL_DOMSTRING_OR_SEQUENCE_NULLABLE &&
                        (JS_IsNull(s->dict_v) || JS_IsUndefined(s->dict_v))) {
                        JS_FreeValue(ctx, s->dict_v);
                        s->dict_v = JS_NULL;
                        mt = IDL_ANY;
                    } else {
                        r = idl_union_seq_arm(ctx, s, &s->uni_phase, s->dict_v, &cb_result, &mt,
                                              IDL_SEQUENCE_DOMSTRING, IDL_DOMSTRING, out_cb, out_argc);
                        if (r > 0) return r;   /* parked ON THIS MEMBER's arm; the resume finds it chosen */
                        if (r < 0) return JS_STEP_ABRUPT;
                    }
                }
                /* §3.2.25 over `(double or sequence<double>)` ON A DICTIONARY MEMBER — Intersection Observer
                   §2.4's `threshold`. The same step 11.2 read, resolved here so the @@iterator access parks on
                   the MEMBER it is on rather than after every later member of the same dictionary was read. */
                if (mt == IDL_DOUBLE_OR_SEQUENCE) {
                    r = idl_union_seq_arm(ctx, s, &s->uni_phase, s->dict_v, &cb_result, &mt,
                                          IDL_SEQUENCE_DOUBLE, IDL_DOUBLE, out_cb, out_argc);
                    if (r > 0) return r;
                    if (r < 0) return JS_STEP_ABRUPT;
                }
                if (mt == IDL_SEQUENCE_STRING_OR_DICT) {
                    /* §3.2.21 whose element type is §3.2.25's `(DOMString or D)` — the conversion that NESTS,
                       driven from here as a stack of cursors so a resume comes back to the element it was on
                       at the depth it was at. A value that is not an Object is a TypeError before anything is
                       read, exactly as it is for every other sequence: the check is on the TYPE and not on
                       iterability. */
                    JSValue seq = JS_UNDEFINED;

                    if (s->conv_sp == 0) {
                        if (!JS_IsObject(s->dict_v)) {
                            JS_FreeValue(ctx, cb_result);
                            JS_ThrowTypeError(ctx, "dictionary member `%s` is not a sequence", dm->name);
                            return JS_STEP_ABRUPT;
                        }
                        if (idl_conv_push(ctx, s, m, s->dict_v, dm->dict) < 0) {
                            JS_FreeValue(ctx, cb_result);
                            return JS_STEP_ABRUPT;
                        }
                    }
                    r = idl_conv_run(ctx, s, m, cb_result, &seq, out_cb, out_argc);
                    cb_result = JS_UNDEFINED;
                    if (r > 0) return r;   /* parked INSIDE the page's iterator, at whatever depth */
                    if (r < 0) return JS_STEP_ABRUPT;
                    JS_FreeValue(ctx, s->dict_v);
                    s->dict_v = seq;
                }
                else if (mt == IDL_SEQUENCE_DOMSTRING || mt == IDL_SEQUENCE_INTERFACE ||
                         mt == IDL_SEQUENCE_OBJECT || mt == IDL_SEQUENCE_DOUBLE) {
                    /* §3.2.21 over a dictionary member. A value that is not an Object is a TypeError before
                       anything is read, exactly as it is in argument position — the check is on the TYPE and
                       not on iterability, so `{attributeFilter: "id"}` throws even though a string iterates.
                       The cursor is the machine's own (`seq`), which the argument-position conversion also
                       uses: the two cannot be in flight at once because arguments are converted strictly left
                       to right and one argument is being converted here. */
                    DCHECK(s->seq_phase == 0 || JS_IsObject(s->seq_list),
                           "a dictionary member's sequence resumed with no list under it");
                    if (!JS_IsObject(s->dict_v)) {
                        JS_FreeValue(ctx, cb_result);
                        JS_ThrowTypeError(ctx, "dictionary member `%s` is not a sequence", dm->name);
                        return JS_STEP_ABRUPT;
                    }
                    if (s->seq_phase == 0) {
                        s->seq_list = JS_NewArray(ctx);
                        if (JS_IsException(s->seq_list)) return JS_STEP_ABRUPT;
                        iter_cursor_init(&s->seq);
                        s->seq_phase = 1;
                    }
                    for (;;) {
                        JSValue str = JS_UNDEFINED;

                        if (s->seq_phase == 1) {
                            r = iter_cursor_run(ctx, &s->hdr, &s->seq, s->dict_v, cb_result, out_cb, out_argc);
                            cb_result = JS_UNDEFINED;
                            if (r > 0) return r;   /* parked ON THIS ELEMENT; the resume comes back to it */
                            if (r < 0) return JS_STEP_ABRUPT;
                            if (s->seq.done) break;
                            /* §3.2.13's `object` over a dictionary member — HTML §9.4.4's `transfer`. An
                               Object crosses as itself and anything else is a TypeError, which runs none of
                               the page's code, so it is decided here rather than being a rest point. */
                            if (mt == IDL_SEQUENCE_OBJECT) {
                                if (!JS_IsObject(s->seq.value)) {
                                    JS_ThrowTypeError(ctx, "an element of dictionary member `%s` is not an "
                                                      "object", dm->name);
                                    return JS_STEP_ABRUPT;
                                }
                                JS_SetPropertyUint32(ctx, s->seq_list, s->seq_n++,
                                                     JS_DupValue(ctx, s->seq.value));
                                continue;
                            }
                            /* §3.2.15's ELEMENT CONVERSION: a platform object implementing the interface
                               crosses as itself and anything else is a TypeError. It runs none of the page's
                               code, so it is decided here rather than being a rest point of its own. */
                            if (mt == IDL_SEQUENCE_INTERFACE) {
                                DCHECK(m->iface != 0,
                                       "a dictionary declared a sequence of an interface type with no class to "
                                       "brand against — idl_iface_brand is the other half of that type");
                                if (!idl_is_iface(s->seq.value, m->iface) ||
                                    (m->iface_narrow && !m->iface_narrow(s->seq.value))) {
                                    JS_ThrowTypeError(ctx, "an element of dictionary member `%s` does not "
                                                      "implement the declared interface", dm->name);
                                    return JS_STEP_ABRUPT;
                                }
                                JS_SetPropertyUint32(ctx, s->seq_list, s->seq_n++,
                                                     JS_DupValue(ctx, s->seq.value));
                                continue;
                            }
                            s->seq_phase = 2;
                        }
                        DCHECK(s->seq_phase == 2, "a dictionary member's sequence resumed at a phase it never "
                                                  "parks in");
                        /* §3.2.7 `double`'s element conversion — ToNumber is the page's `valueOf`, so it is a
                           request and its own rest point, and the RESTRICTED type refuses a non-finite result
                           here rather than leaving it for the algorithm that reads the list. */
                        if (mt == IDL_SEQUENCE_DOUBLE) {
                            double d = 0.0;

                            r = step_todouble_run(ctx, &s->hdr, s->seq.value, cb_result, &d, out_cb, out_argc);
                            cb_result = JS_UNDEFINED;
                            if (r > 0) return r;
                            if (r < 0) return JS_STEP_ABRUPT;
                            if (!isfinite(d)) {
                                JS_ThrowTypeError(ctx, "an element of dictionary member `%s` is not a finite "
                                                  "double", dm->name);
                                return JS_STEP_ABRUPT;
                            }
                            JS_SetPropertyUint32(ctx, s->seq_list, s->seq_n++, JS_NewFloat64(ctx, d));
                            s->seq_phase = 1;
                            continue;
                        }
                        r = step_tostring_run(ctx, &s->hdr, s->seq.value, cb_result, &str, out_cb, out_argc);
                        cb_result = JS_UNDEFINED;
                        if (r > 0) return r;
                        if (r < 0) return JS_STEP_ABRUPT;
                        JS_SetPropertyUint32(ctx, s->seq_list, s->seq_n++, str);
                        s->seq_phase = 1;
                    }
                    JS_FreeValue(ctx, s->dict_v);
                    s->dict_v = s->seq_list;
                    s->seq_list = JS_UNDEFINED;
                    s->seq_n = 0;
                    s->seq_phase = 0;
                }
                else if (mt == IDL_BOOLEAN || mt == IDL_BOOLEAN_NO_DEFAULT) {
                    JSValue b = JS_NewBool(ctx, JS_ToBool(ctx, s->dict_v));
                    JS_FreeValue(ctx, s->dict_v);
                    s->dict_v = b;
                }
                else if (mt == IDL_CALLBACK) {
                    if (!JS_IsFunction(ctx, s->dict_v)) {
                        JS_ThrowTypeError(ctx, "dictionary member `%s` is not callable", dm->name);
                        return JS_STEP_ABRUPT;
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
                    JSClassID want = dm->iface ? dm->iface : m->iface;

                    DCHECK(want != 0, "a dictionary declared an interface-typed member with no class to brand "
                                      "against — IdlDictMember::iface states it per member, and "
                                      "idl_iface_brand states it once for a dictionary whose interface-typed "
                                      "members are all one interface");
                    if (!idl_is_iface(s->dict_v, want)) {
                        JS_ThrowTypeError(ctx, "dictionary member `%s` does not implement the declared "
                                          "interface", dm->name);
                        return JS_STEP_ABRUPT;
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
                    JSClassID want = dm->iface ? dm->iface : m->iface;

                    DCHECK(want != 0, "a dictionary declared a nullable interface-typed member with no class "
                                      "to brand against — IdlDictMember::iface states it per member, and "
                                      "idl_iface_brand states it once per declaration");
                    if (JS_IsNull(s->dict_v)) {
                        /* the IDL null; nothing to brand */
                    } else if (!idl_is_iface(s->dict_v, want) ||
                               (m->iface_narrow && !m->iface_narrow(s->dict_v))) {
                        JS_ThrowTypeError(ctx, "dictionary member `%s` does not implement the declared "
                                          "interface", dm->name);
                        return JS_STEP_ABRUPT;
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

                    r = step_todouble_run(ctx, &s->hdr, s->dict_v, cb_result, &num, out_cb, out_argc);
                    cb_result = JS_UNDEFINED;
                    if (r > 0) return r;   /* parked ON THIS MEMBER's conversion; the read does not re-run */
                    if (r < 0) return JS_STEP_ABRUPT;
                    JS_FreeValue(ctx, s->dict_v);
                    s->dict_v = idl_num_of(ctx, mt, num);
                    if (JS_IsException(s->dict_v)) {   /* §3.2.7's restricted double refused the value */
                        s->dict_v = JS_UNDEFINED;
                        return JS_STEP_ABRUPT;
                    }
                }
                else if (mt == IDL_DOMSTRING || mt == IDL_DOMSTRING_NULLABLE || mt == IDL_BYTESTRING ||
                         mt == IDL_USVSTRING || mt == IDL_USVSTRING_NULLABLE || mt == IDL_ENUM ||
                         mt == IDL_ENUM_NULLABLE) {
                    if ((mt == IDL_DOMSTRING_NULLABLE || mt == IDL_USVSTRING_NULLABLE ||
                         mt == IDL_ENUM_NULLABLE) && JS_IsNull(s->dict_v)) {
                        /* `DOMString?` / `NavigationType?`: null is the IDL null, never the string "null". */
                    } else {
                        JSValue str = JS_UNDEFINED;
                        r = step_tostring_run(ctx, &s->hdr, s->dict_v, cb_result, &str, out_cb, out_argc);
                        cb_result = JS_UNDEFINED;
                        if (r > 0) return r;
                        if (r < 0) return JS_STEP_ABRUPT;
                        JS_FreeValue(ctx, s->dict_v);
                        s->dict_v = str;
                        if (mt == IDL_BYTESTRING && idl_bytestring_check(ctx, s->dict_v) < 0)
                            return JS_STEP_ABRUPT;
                        if (mt == IDL_USVSTRING || mt == IDL_USVSTRING_NULLABLE) {
                            s->dict_v = JS_ToScalarValueString(ctx, s->dict_v);
                            if (JS_IsException(s->dict_v)) return JS_STEP_ABRUPT;
                        }
                        if ((mt == IDL_ENUM || mt == IDL_ENUM_NULLABLE) &&
                            idl_enum_check(ctx, s->dict_v, dm->values, dm->name) < 0)
                            return JS_STEP_ABRUPT;
                    }
                }
                JS_SetPropertyStr(ctx, *idl_arg_slot(m, s, s->i), dm->name, s->dict_v);
                s->dict_v = JS_UNDEFINED;
                s->dict_phase = 0;
                /* THE UNION'S ARM IS PER MEMBER, and this is the one place a member finishes — the next
                   member's own §3.2.25 must ask its own value rather than inherit the answer this one got. */
                s->uni_phase = IDL_UNI_ASK;
                s->dict_i++;
            }
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            s->dict_i = 0;
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
            t == IDL_SEQUENCE_DOMSTRING || t == IDL_SEQUENCE_DOUBLE) {
            if (!JS_IsObject(a)) {
                JS_FreeValue(ctx, cb_result);
                JS_ThrowTypeError(ctx, "the sequence argument is not an object");
                return JS_STEP_ABRUPT;
            }
            if (s->seq_phase == 0) {
                s->seq_list = JS_NewArray(ctx);
                if (JS_IsException(s->seq_list)) return JS_STEP_ABRUPT;
                iter_cursor_init(&s->seq);
                s->seq_phase = 1;
            }
            for (;;) {
                if (s->seq_phase == 1) {
                    r = iter_cursor_run(ctx, &s->hdr, &s->seq, a, cb_result, out_cb, out_argc);
                    cb_result = JS_UNDEFINED;
                    if (r > 0) return r;   /* parked ON THIS ELEMENT; the resume comes back to it */
                    if (r < 0) return JS_STEP_ABRUPT;
                    if (s->seq.done) break;
                    /* §3.2.13's `object`: an Object crosses as ITSELF and anything else is a TypeError. Like
                       the brand test below it runs none of the page's code, so the cursor's next pull follows
                       it in the same step. */
                    if (t == IDL_SEQUENCE_OBJECT) {
                        if (!JS_IsObject(s->seq.value)) {
                            JS_ThrowTypeError(ctx, "an element of argument %d is not an object", s->i + 1);
                            return JS_STEP_ABRUPT;
                        }
                        JS_SetPropertyUint32(ctx, s->seq_list, s->seq_n++, JS_DupValue(ctx, s->seq.value));
                        continue;
                    }
                    /* §3.2.15's ELEMENT CONVERSION — the brand test, which runs none of the page's code, so
                       the cursor's next pull follows it in the same step. */
                    if (t == IDL_SEQUENCE_INTERFACE) {
                        DCHECK(m->iface != 0,
                               "an interface-sequence argument was declared with no class to brand against — "
                               "idl_iface_brand is the other half of that type");
                        if (!idl_is_iface(s->seq.value, m->iface) ||
                            (m->iface_narrow && !m->iface_narrow(s->seq.value))) {
                            JS_ThrowTypeError(ctx, "an element of argument %d does not implement the declared "
                                              "interface", s->i + 1);
                            return JS_STEP_ABRUPT;
                        }
                        JS_SetPropertyUint32(ctx, s->seq_list, s->seq_n++, JS_DupValue(ctx, s->seq.value));
                        continue;
                    }
                    /* `BlobPart` is `(BufferSource or Blob or USVString)`, and its rule is a BRAND test: a
                       BufferSource and a Blob cross as themselves, everything else takes the USVString arm,
                       whose ToString is the page's code. Stated once, here, like BodyInit's.
                       IT IS THE BLOBPART TYPE'S AND NOT THE LOOP'S: a `sequence<DOMString>` holding a Blob has
                       one element type and it is a string, so that element ToStrings to "[object Blob]" rather
                       than crossing as an interface the declaration never named. */
                    if (t == IDL_SEQUENCE_BLOBPART &&
                        (blob_is(s->seq.value) || JS_IsArrayBuffer(s->seq.value) ||
                         JS_GetTypedArrayType(s->seq.value) >= 0 || JS_IsDataView(s->seq.value))) {
                        JS_SetPropertyUint32(ctx, s->seq_list, s->seq_n++, JS_DupValue(ctx, s->seq.value));
                        continue;
                    }
                    s->seq_phase = 2;
                }
                /* §3.2.7 `double` AS AN ELEMENT CONVERSION — ToNumber, which is the page's `valueOf`, so it
                   is a request and its own rest point exactly as the ToString arm below is. The RESTRICTED
                   type's refusal of a non-finite value belongs here and not to whichever algorithm reads the
                   list: `sequence<double>` is what the IDL declares, and `[Infinity]` is a TypeError before
                   any step of the caller runs. */
                if (t == IDL_SEQUENCE_DOUBLE) {
                    double d = 0.0;

                    DCHECK(s->seq_phase == 2, "the sequence conversion resumed at a phase it never parks in");
                    r = step_todouble_run(ctx, &s->hdr, s->seq.value, cb_result, &d, out_cb, out_argc);
                    cb_result = JS_UNDEFINED;
                    if (r > 0) return r;
                    if (r < 0) return JS_STEP_ABRUPT;
                    if (!isfinite(d)) {
                        JS_ThrowTypeError(ctx, "an element of argument %d is not a finite double", s->i + 1);
                        return JS_STEP_ABRUPT;
                    }
                    JS_SetPropertyUint32(ctx, s->seq_list, s->seq_n++, JS_NewFloat64(ctx, d));
                    s->seq_phase = 1;
                    continue;
                }
                {
                    JSValue str = JS_UNDEFINED;
                    DCHECK(s->seq_phase == 2, "the sequence conversion resumed at a phase it never parks in");
                    r = step_tostring_run(ctx, &s->hdr, s->seq.value, cb_result, &str, out_cb, out_argc);
                    cb_result = JS_UNDEFINED;
                    if (r > 0) return r;
                    if (r < 0) return JS_STEP_ABRUPT;
                    /* THE SCALAR VALUE CONVERSION BELONGS TO THE ELEMENT TYPE. `BlobPart`'s string arm is a
                       USVString, so §3.2.12 replaces its lone surrogates; a `sequence<DOMString>`'s element is
                       a DOMString and keeps them, which is the whole difference between the two. */
                    if (t != IDL_SEQUENCE_DOMSTRING) {
                        str = JS_ToScalarValueString(ctx, str);   /* §3.2.12: lone surrogates become U+FFFD */
                        if (JS_IsException(str)) return JS_STEP_ABRUPT;
                    }
                    JS_SetPropertyUint32(ctx, s->seq_list, s->seq_n++, str);
                    s->seq_phase = 1;
                }
            }
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            *slot = s->seq_list;
            s->seq_list = JS_UNDEFINED;
            s->seq_n = 0;
            s->seq_phase = 0;
            goto placed;
        }

        /* §3.2.15's INTERFACE type: the brand test, once, so no body performs it — and a value that is not one
           is a TypeError BEFORE the algorithm's step 1, which is what `walker.currentNode = null` asserts. */
        if (t == IDL_INTERFACE) {
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            DCHECK(m->iface != 0, "an interface-typed argument was declared with no class to brand against — "
                                  "idl_iface_brand is the other half of that type");
            if (!idl_is_iface(a, m->iface) || (m->iface_narrow && !m->iface_narrow(a))) {
                JS_ThrowTypeError(ctx, "argument %d does not implement the declared interface", s->i + 1);
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

        /* §3.2.25's `BufferSource`: the brand test, once, so no body performs it. */
        if (t == IDL_BUFFERSOURCE) {
            if (!JS_IsArrayBuffer(a) && JS_GetTypedArrayType(a) < 0 && !JS_IsDataView(a)) {
                JS_FreeValue(ctx, cb_result);
                JS_ThrowTypeError(ctx, "the argument is not a BufferSource");
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
            t = (JS_IsArrayBuffer(a) || JS_GetTypedArrayType(a) >= 0 || JS_IsDataView(a) ||
                 blob_is(a) || form_data_is(a) || usp_list_of(a) || readable_stream_is(a))
              ? IDL_ANY : IDL_DOMSTRING;
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
            /* ToBoolean runs nothing, but the ARGUMENT still crosses converted: `toggle(t, 1)` forces on, and a
               body that got the 1 would have to remember to coerce it. */
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            *slot = JS_IsUndefined(a) ? JS_UNDEFINED : JS_NewBool(ctx, JS_ToBool(ctx, a));
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
        DCHECK(t != IDL_ENUM || m->enum_values != NULL,
               "an ENUMERATION was declared at a positional argument with no value list — the list IS the "
               "type (§3.2.18), so idl_enum_values must name it beside the declaration");
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
        if (t == IDL_ENUM && idl_enum_check(ctx, *slot, m->enum_values, "argument") < 0)
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
        s->uni_phase = IDL_UNI_ASK;
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
        if (s->tree) { int d = idl_tree_drain(ctx, s); if (d) return d; }
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
    if (s->tree) { int d = idl_tree_drain(ctx, s); if (d) return d; }
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

    /* THE MEMBER'S OWN RELEASE GOES FIRST, AND IT OWNS NO REFERENCE. It runs first because the work it does is
       real algorithm work that READS what this machine owns — §4.13.4 step 14's "regardless of whether the
       above steps threw" lowers a flag off `s->registry` — and it may free only what the declaration does NOT
       name: a lexbor handle, a foreign C allocation, a flag its algorithm took. That split is measured, not
       trusted: freeing a declared value is silent both ways it can be written (free-and-null leaves the
       discharge a no-op; free-without-null makes the discharge the second free), so the declaration is folded
       into a number on each side of the call and the two must agree. */
    if (m->step && m->step->release) {
#if APICLIENT_DEV
        uint64_t owned_before = JS_StepVisitOwnedFingerprint(ctx, m->step->visit, idl_body_state(m, st));
        uint64_t owned_after;
#endif
        m->step->release(ctx, idl_body_state(m, st));
#if APICLIENT_DEV
        owned_after = JS_StepVisitOwnedFingerprint(ctx, m->step->visit, idl_body_state(m, st));
        DCHECK(owned_after == owned_before,
               "a member's `release` freed a value its own `visit` already names — the visit IS the one list of "
               "what the state owns and the teardown discharges it; a second list beside it leaks whatever the "
               "next field misses, and double-frees whatever this one did not null. Release only what the "
               "declaration does not name (a lexbor handle, a foreign allocation, a flag to lower)");
#endif
    }

    /* §8.1.4.6 step 5's FLAG, if this member's reaction drain was abandoned holding it. Not a reference, so no
       declaration names it and the discharge the driver runs after this cannot give it back; leaving it set
       would put the global in error reporting mode forever and silently swallow every later report. */
    custom_elements_queue_unlock(ctx, &s->ce);

    /* AND THE ONE LIST IS DISCHARGED BY THE DRIVER, after this returns — tramp_step_state_free_1 reads
       js_idl_args_visit exactly as it reads every other machine's, so this function neither restates it nor
       calls it. It covers the whole state: the converted argument vector, every declared conversion frame, the
       sequence cursor and its collected elements, the drain buffer of an abandoned §4.2.3 walk, the
       element-reaction queue of a flow dropped mid-member, and — through its last line — the member body's own
       owned values. An abandoned queue's reactions die with the flow, which is what an abandoned flow means.
       The cursors below are what no declaration can carry: a zeroed state means "nothing in flight", and these
       are what say so. `ce_threw` is the epilogue's caught-completion flag, `conv_sp` the nested-conversion
       depth. */
    s->conv_sp = 0;
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
    /* §3.6's LENGTH-DIFFERING SPLIT, READ OFF THE TYPE LIST rather than named a second time by the caller: the
       position IS in the types the member just declared, so asking for it again would be one fact stated
       twice and free to disagree. -1 is "this member declares none", which is nearly all of them, and the
       LONGER entry's optional index stays unset until idl_overload_split_optional_from states it — asserted
       at the seal, where every declaration exists and none can change again. */
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
    idl_member(idx)->enum_values = NULL;
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
void idl_overload_split_optional_from(int longer_first_optional)
{
    IdlMember *m;

    DCHECK(g_n > 0, "a longer-overload-entry optional index was declared before any member was");
    DCHECK(!g_sealed, IDL_LAST_DECL_ONLY);
    m = idl_member(g_n - 1);
    DCHECK(m->split_at >= 0,
           "a member that declares no §3.6 length-differing overload split said where its LONGER entry's "
           "optional arguments begin — the split is read off the type list, so a member with none has no "
           "second entry for this to describe");
    /* THE LONGER ENTRY STARTS PAST THE SPLIT, so its first optional position cannot be at or before it: the
       shorter entry is the one that ENDS there, and a longer entry whose optional arguments began inside the
       shared prefix would be the shorter entry with extra positions rather than a second overload. */
    DCHECK(longer_first_optional > m->split_at && longer_first_optional <= m->nargs,
           "a longer overload entry's first OPTIONAL argument is not one of the positions past the split that "
           "the member declares");
    m->split_longer_optional = longer_first_optional;
}

/* See idl_args.h. Same "names the last declaration" rule as idl_optional_from, and it must be stated AFTER
   that one: §3.6 step 14.2 reads a default only for an OPTIONAL argument, so a default on a position the member
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
           "a REQUIRED argument was given a default value — §3.6 step 14.2 reads one only for an optional "
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

/* §3.2.18's ENUMERATION VALUES for the member's IDL_ENUM position — see idl_args.h. Named after the
   declaration, on the member the LAST one made, exactly as idl_iface_brand and idl_optional_from are and for
   the same reason: the id a declaration returns is the RUNTIME's step id, not this pool's index. */
void idl_enum_values(const char *const *values)
{
    DCHECK(g_n > 0, "an enumeration's value list was declared before any member was");
    DCHECK(!g_sealed, IDL_LAST_DECL_ONLY);
    DCHECK(values != NULL && values[0] != NULL,
           "an enumeration declared an empty value list — every §3.2.18 enumeration has at least one value, and "
           "a member whose type admits nothing is a member no assignment can satisfy");
    idl_member(g_n - 1)->enum_values = values;
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

/* §3.7.3: EVERY INTERFACE PROTOTYPE OBJECT CARRIES @@toStringTag, whose value is the interface's IDENTIFIER and
   whose attributes are { writable: false, enumerable: false, configurable: true }. It is what makes
   `Object.prototype.toString.call(new Blob())` answer "[object Blob]" — the brand check a page performs without
   `instanceof`, and the one wpt's own assert_class_string makes about every interface it touches.
   NOT ONE INTERFACE IN THIS ENGINE HAD IT. Every one of them answered "[object Object]", which is Web IDL's rule
   missed twenty-two times over — the shape a per-component rule always ends up in, and why this is one call the
   interface makes rather than a line each of them remembers. */
void idl_interface_tag(JSContext *ctx, JSValueConst proto, const char *iface)
{
    DCHECK(JS_IsObject(proto), "an interface's @@toStringTag was installed on something that is not an object");
    JS_DefinePropertyValue(ctx, (JSValue)proto, JS_DupAtom(ctx, JS_WellKnownSymbolAtom(JS_WKS_TO_STRING_TAG)),
                           JS_NewString(ctx, iface), JS_PROP_CONFIGURABLE);
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
    g = idl_step_function(ctx, name, 0, getter_stepid);
    if (setter_stepid >= 0)
        st = idl_step_function(ctx, name, 1, setter_stepid);
    JS_DefinePropertyGetSet(ctx, (JSValue)target, a, g, st,
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, a);
}

/* WEB IDL §3.9's "is exposed in realm", for the conditions this engine models — see idl_args.h for why the
   question is asked HERE and the answer is stated by the component as data.
   IT IS `static` DELIBERATELY. A public predicate is an invitation to write `if (idl_exposed(ctx, ...))` at a
   call site, which is the per-member conditional the parameter exists to remove; the only way to reach this is
   to hand an installer the attribute the IDL states. When a [SecureContext] member of a shape that has no
   exposed-form installer yet arrives (an operation — HTML's registerProtocolHandler is the one waiting), that
   installer gets the same parameter and the same one line, rather than this becoming reachable from outside. */
static bool idl_exposed(JSContext *ctx, IdlExposure exposure)
{
    switch (exposure) {
    case IDL_EXPOSED:        return true;
    case IDL_SECURE_CONTEXT: return secure_context_is(ctx);   /* §3.9 step 2 */
    }
    DFAIL("a member was installed with an exposure condition Web IDL §3.9 has no step for — every value of "
          "IdlExposure is one of §3.9's numbered conditions, so a new one is a new step to write here");
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
                                IdlGetter getter, int getter_magic, int setter_stepid, int flags)
{
    DCHECK(setter_stepid < 0 || idl_declared_before_seal(setter_stepid), name);
    JSAtom a = JS_NewAtom(ctx, name);
    JSValue g = JS_UNDEFINED, st = JS_UNDEFINED;

    DCHECK(a != JS_ATOM_NULL, "an IDL accessor name could not be interned");
    if (getter)
        g = JS_NewCFunction2(ctx, (JSCFunction *)getter, name, 0, JS_CFUNC_getter_magic, getter_magic);
    /* The GETTER here is a plain C function with no pool entry (this is the form for an attribute whose read
       runs none of the page's code), but the SETTER is a step member exactly like any other, so it is minted
       the same way and named the same way. It was the fourth hand-written mint. */
    if (setter_stepid >= 0)
        st = idl_step_function(ctx, name, 1, setter_stepid);
    JS_DefinePropertyGetSet(ctx, (JSValue)target, a, g, st, flags);
    JS_FreeAtom(ctx, a);
}

void idl_install_accessor(JSContext *ctx, JSValueConst target, const char *name,
                          IdlGetter getter, int getter_magic, int setter_stepid)
{
    idl_define_accessor(ctx, target, name, getter, getter_magic, setter_stepid,
                        JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
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
    idl_define_accessor(ctx, target, name, getter, getter_magic, setter_stepid, JS_PROP_ENUMERABLE);
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

/* WEB IDL §3.7.6's `jsValue`, FOR THE TWO ACCESSORS THIS FILE MINTS ITSELF — resolved and brand-checked.
 *
 * §3.7.6 puts BOTH of these steps in "create an attribute getter" and "create an attribute setter", ahead of
 * the member's own getter/setter steps: "Let jsValue be the this value, if it is not null or undefined, or
 * realm's global object otherwise", and then "If jsValue does not implement target ... throw a TypeError".
 * They are the ACCESSOR MACHINERY'S work, not the member's — which is why they belong here, and why every
 * member minted here had them missing TOGETHER: the [Replaceable] setter handed its raw `this` straight to the
 * define, and the held-value getter opened with `(void)this_val`. A MISSING receiver and a FOREIGN one reached
 * the same answer, which is the two facts a default confuses, written out in the loudest possible form.
 * The setter's own §7.2.2.4 spelling of this — `opener`'s, in window.c — already resolved its receiver by
 * hand through these same two window_proxy.c halves; the shared mechanism underneath it did not.
 *
 * THE INTERFACE IS Window FOR EVERY MEMBER THAT REACHES HERE, and that is ASSERTED AT THE INSTALL rather than
 * assumed at the read: every idl_install_replaceable / _replaceable_value / _value_attribute call site targets
 * the realm's global, so `target` is Window and window_proxy.c owns the test. A brand parameter every existing
 * caller would pass identically is a field nobody would notice going wrong; IDL_CHECK_GLOBAL_TARGET below is
 * what names the brand this file has to be TOLD the day a component declares one of these attributes on some
 * other interface.
 *
 * Returns the resolved jsValue OWNED, or JS_EXCEPTION with §3.7.6's TypeError pending. */
static JSValue idl_attribute_this(JSContext *ctx, JSValueConst this_val, const char *name)
{
    JSValue js = window_proxy_this_object(ctx, this_val);   /* §3.7.6's first clause, written once */

    if (window_proxy_implements_window(js)) return js;
    JS_FreeValue(ctx, js);
    return JS_ThrowTypeError(ctx, "'%s' called on an object that does not implement interface Window", name);
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
    js = idl_attribute_this(ctx, this_val, name);
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
    js = idl_attribute_this(ctx, this_val, name);
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
 * fact (idl_args.h's §3.9 note states the same thing from the other side: there is no WorkerGlobalScope here,
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
    JSValue g = JS_GetGlobalObject(ctx);
    bool same = JS_VALUE_GET_PTR(g) == JS_VALUE_GET_PTR(target);

    JS_FreeValue(ctx, g);   /* balanced, so the check observes nothing */
    if (same) return;
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

    IDL_CHECK_GLOBAL_TARGET(ctx, target, name, "[Replaceable]");
    DCHECK(a != JS_ATOM_NULL, "a replaceable attribute name could not be interned");
    nm = JS_NewString(ctx, name);
    CHECK(!JS_IsException(nm), "a replaceable attribute's name could not be allocated");
    setter = JS_NewCFunctionData2(ctx, idl_replaceable_set, name, 1, 0, 1, (JSValueConst *)&nm);
    CHECK(!JS_IsException(setter), "a replaceable attribute's setter could not be allocated");
    JS_FreeValue(ctx, nm);
    JS_DefinePropertyGetSet(ctx, (JSValue)target, a, getter, setter,
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, a);
}

void idl_install_replaceable(JSContext *ctx, JSValueConst target, const char *name,
                             IdlGetter getter, int getter_magic)
{
    JSValue g;

    DCHECK(getter != NULL, "a replaceable attribute with no getter — it is READONLY, so the read is all it has");
    g = JS_NewCFunction2(ctx, (JSCFunction *)getter, name, 0, JS_CFUNC_getter_magic, getter_magic);
    CHECK(!JS_IsException(g), "a replaceable attribute's getter could not be allocated");
    idl_define_replaceable(ctx, target, name, g);
}

/* THE HELD-VALUE GETTER'S DATA IS TWO VALUES, and the second is the member's own NAME. §3.7.6's TypeError
   names the member a page mis-invoked, and a getter that carries only its value has nothing to name it with —
   the same argument the [Replaceable] setter above already makes for carrying its name as data. */
static JSValue idl_held_value_getter(JSContext *ctx, const char *name, JSValue value)
{
    JSValue d[2], g;

    d[0] = value;
    d[1] = JS_NewString(ctx, name);
    CHECK(!JS_IsException(d[1]), "a held-value attribute's name could not be allocated");
    g = JS_NewCFunctionData2(ctx, idl_held_value_get, name, 0, 0, 2, (JSValueConst *)d);
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

JSValue idl_step_function(JSContext *ctx, const char *name, int length, int stepid)
{
    int idx = idl_member_of_step(stepid);
    /* NAMING THE OFFENDER IS THE POINT. "some member was never declared" sends whoever hits it grepping every
       install site; the name is right here in the argument, so the assert says it. */
    if (idx < 0)
        DFAILF("step function '%s' was minted for a member this pool never declared — a step machine that is "
               "not an args-machine member installs through idl_install_step_method", name ? name : "?");
    DCHECK(name != NULL && *name, "a step function was minted with no name — the pool has nothing to call it");
    idl_member(idx)->name = name;
    return JS_NewCFunction2(ctx, NULL, name, length, JS_CFUNC_step, stepid);
}

/* The same mint for a member reached with `new`. JS_CFUNC_step_ctor differs only in how the receiver slot
   carries new.target; the pool entry and its name are the same thing. */
JSValue idl_step_constructor(JSContext *ctx, const char *name, int length, int stepid)
{
    int idx = idl_member_of_step(stepid);
    DCHECK(idx >= 0, "a step constructor was minted for a member this pool never declared");
    DCHECK(name != NULL && *name, "a step constructor was minted with no name");
    idl_member(idx)->name = name;
    return JS_NewCFunction2(ctx, NULL, name, length, JS_CFUNC_step_ctor, stepid);
}

void idl_install_method_exposed(JSContext *ctx, JSValueConst target, const char *name, int length, int stepid,
                                IdlExposure exposure)
{
    /* §3.3.13: the member is simply NOT THERE — the same rule an attribute's install already states, asked at
       the same one place. A method needed it the moment an interface whose whole partial is [SecureContext]
       arrived (File System §3's `getDirectory`), and `'getDirectory' in navigator.storage` is exactly the
       feature detection the removal exists to answer honestly. */
    if (!idl_exposed(ctx, exposure)) return;
    idl_install_method(ctx, target, name, length, stepid);
}

void idl_install_method(JSContext *ctx, JSValueConst target, const char *name, int length, int stepid)
{
    DCHECK(idl_declared_before_seal(stepid), name);
    DCHECK(stepid >= 0, "an IDL member was installed before it was declared");
    JS_SetPropertyStr(ctx, (JSValue)target, name, idl_step_function(ctx, name, length, stepid));
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
void idl_install_method_unforgeable(JSContext *ctx, JSValueConst target, const char *name, int length,
                                    int stepid)
{
    DCHECK(idl_declared_before_seal(stepid), name);
    DCHECK(stepid >= 0, "an unforgeable IDL operation was installed before it was declared");
    JS_DefinePropertyValueStr(ctx, (JSValue)target, name, idl_step_function(ctx, name, length, stepid),
                              JS_PROP_ENUMERABLE);
}

/* A DOM METHOD WHOSE ALGORITHM IS A STEP MACHINE BUT WHOSE ARGUMENTS ARE NOT THIS MACHINE'S. `click` and
   `dispatchEvent` register their own JSTrampStepDef and have no entry in this pool, so there is nothing here to
   name and nothing to convert — they are a genuinely different thing, not a member that skipped a step, and
   collapsing them into idl_install_method is what made a five-second member report itself as "(none)".
   Two installers because there are two kinds; each asserts it was handed its own kind, so neither can be used
   for the other by mistake. The IDL-shaped future for these is to declare their arguments through the args
   machine like every other member — at which point they move to idl_install_method and this loses a caller. */
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
        /* §3.6 step 14.2's declared defaults — the strings themselves are the declaring component's statics;
           the array naming them is this pool's, allocated by the first default a member declared. */
        free(idl_member(i)->arg_dflts);
        idl_member(i)->arg_dflts = NULL;
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
    g_sealed = false;
    g_sealed_at = 0;
}
