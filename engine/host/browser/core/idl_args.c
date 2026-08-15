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
 * WITH [Clamp] (§3.2.4.2): NaN is +0, the value is clamped to the type's range, and then rounded to the NEAREST
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
           t == IDL_LONG_LONG || t == IDL_UNSIGNED_LONG_LONG || t == IDL_LONG_LONG_CLAMP;
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
    return idl_is_integer(t) || t == IDL_UNRESTRICTED_DOUBLE;
}

/* §3.2.9's `unsigned long long` AS THE MAGNITUDE IT IS. Public because a conversion that happens OUTSIDE this
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
    /* AN `unsigned long long` DOES NOT FIT IN AN int64_t, and the half of its range that does not is exactly
       the half a page reaches by writing a negative: §3.2.9's conversion of -1 is 2**64-1, which as an int64_t
       is the bit pattern -1 again, so handing it back through JS_NewInt64 would undo the whole conversion. The
       value the member receives is a JS NUMBER either way, and a double holds the magnitude (to the same 53
       bits of precision `Number(2n**64n-1n)` has), so the unsigned type places one. */
    if (t == IDL_UNSIGNED_LONG_LONG) return JS_NewFloat64(ctx, idl_unsigned_long_long_of(x));
    return JS_NewInt64(ctx, idl_integer_of(t, x));
}

/* §3.2.19's ENUMERATION check, over the string ToString produced. Returns -1 with a TypeError live. */
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
    /* THE FIRST OPTIONAL ARGUMENT's index. §3.6.2 resolves an `undefined` passed for an optional argument with
       no default as the argument being ABSENT — `new URL("aaa:b", undefined)` is a one-argument call, not a
       call with the base "undefined". Declared per member rather than assumed, because the same undefined at a
       REQUIRED position is the string "undefined" and collapsing the two is wrong in one direction or the
       other. Its "there are none" value is `nargs` — one past the member's own last position, so a member that
       does not declare it converts every position it lists. It was IDL_MAX_DECLARED + 1, a sentinel that could
       only exist while there was a ceiling to derive it from, and that named a position no member had. */
    int        first_optional;
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
    JSClassID  iface;       /* the brand an IDL_STRING_UNLESS_IFACE position tests against */
    /* THE NARROWING half of that brand — see idl_iface_narrow. NULL for a member whose interface a class id
       already names exactly, which is most of them. */
    bool     (*iface_narrow)(JSValueConst v);
    /* §3.2.19's VALUE LIST for this member's IDL_ENUM position — the list IS the type, so a declaration
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
    "Web IDL §3.6.2 (the operation's argument count: fewer than the required ones is a TypeError)",
    "Web IDL §3.6.2 (converting each passed argument to its declared IDL type)",
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
    "Web IDL §3.6.2 (an operation's argument handling), inside HTML §4.13.6's custom element reactions steps"

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

void idl_args_seal(void) { g_sealed = true; g_sealed_at = g_n; }

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
static void idl_dict_order_check(const IdlDictMember *members, int k)
{
    if (k == 0) return;
    if (members[k].level > members[k - 1].level) return;
    if (members[k].level == members[k - 1].level && strcmp(members[k - 1].name, members[k].name) < 0) return;
#if APICLIENT_DEV
    {
        /* NAME THE PAIR. This used to abort with the RULE and nothing else, and the pool holds every dictionary
           in the engine — so a reader standing at the abort had a search rather than a diagnosis, and the two
           members that have to swap are the entire content of the answer. Building it costs a stack buffer on
           a path that is about to abort. */
        char why[320];
        snprintf(why, sizeof why,
                 "a dictionary's members were declared out of Web IDL 3.2.17's read order: `%s` (level %d) is "
                 "declared after `%s` (level %d). Inherited levels come first, and each level's own members "
                 "sort lexicographically among themselves",
                 members[k].name, members[k].level, members[k - 1].name, members[k - 1].level);
        DFAIL(why);
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
        idl_dict_order_check(d->members, k);
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

/* §3.2.17 step 4.1.5's DEFAULT VALUE, as an IDL value. It is already converted — a default is written in the
   IDL, not computed from the page — so it is placed and never coerced. */
static JSValue idl_default_value(JSContext *ctx, const IdlDictMember *dm)
{
    if (dm->dflt == IDL_DEFAULT_NULL) return JS_NULL;
    DCHECK(dm->dflt == IDL_DEFAULT_STRING && dm->dflt_str != NULL,
           "a dictionary member declared a default this machine has no value for — the forms are the ones the "
           "platform's IDL writes, and a new one is an arm here rather than a string that means something else");
    return JS_NewString(ctx, dm->dflt_str);
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
    /* §3.2.20's `sequence<T>` CONVERSION: the ES iterator protocol, whose every step is the page's code — so the
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
   "every argument from here on" and not a position of its own, which is why the count differs — and why §3.6.2's
   absent-optional rule stops there: each value a page passes to a `T...` tail is CONVERTED, so
   `el.append('a', undefined)` appends the text "undefined" rather than skipping an argument. */
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

/* WEB IDL §3.2.10's `ByteString` RANGE, over the UTF-8 the engine hands out. A ByteString's code points are
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
            /* §3.6.2 STEP 1: a call with fewer arguments than the member has REQUIRED ones is a TypeError, and
               it is thrown before any conversion runs. `new File()` built a File out of nothing; `new File([])`
               built one with the name "undefined". The count is the same `first_optional` the declaration
               already states, capped at what the IDL lists — a member that never declares an optional position
               requires every argument it declared, which is what the IDL means by writing them. */
            /* A VARIADIC ARGUMENT IS OPTIONAL — that is part of what the tail MEANS, so `el.remove()` and
               `el.append()` require nothing even though the member declares the tail's type. Stated here
               rather than per member: a variadic member that had to remember to say so is a member whose
               forgetting turns into a TypeError the spec does not have. */
            int declared = idl_declared_positions(m);
            int required = m->first_optional < declared ? m->first_optional : declared;
            if (s->hdr.argc < required) {
                JS_FreeValue(ctx, cb_result);
                JS_ThrowTypeError(ctx, "%d argument%s required, but only %d present",
                                  required, required == 1 ? "" : "s", s->hdr.argc);
                return JS_STEP_ABRUPT;
            }
        }
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

        /* §3.6.2: an optional argument given `undefined` is ABSENT, so nothing is converted and the body sees
           undefined — which is what lets it tell "no base" from the base "undefined". A VARIADIC TAIL is not
           one of those positions (see idl_declared_positions): every value passed to a `T...` is converted. */
        if (s->i < idl_declared_positions(m) && s->i >= m->first_optional && JS_IsUndefined(a)) {
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            *slot = JS_UNDEFINED;
            goto placed;
        }

        if (t == IDL_STRING_UNLESS_CALLABLE)
            t = JS_IsFunction(ctx, a) ? IDL_ANY : IDL_DOMSTRING;   /* the union's own rule */
        if (t == IDL_STRING_UNLESS_IFACE) {
            DCHECK(m->iface != 0, "a member declared an interface-or-string union with no interface to brand "
                                  "against — the class is half of what that type states");
            t = idl_is_iface(a, m->iface) ? IDL_ANY : IDL_DOMSTRING;
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
        /* AN INTERFACE BRAND IS NOT A COERCION, so the pass-through below does not cover it: unknown external
           input is not a platform object, and letting it cross as itself hands the body something node_of
           answers NULL for. §3.2.16's answer to "this is not a Node" is a TypeError either way, and a
           TypeError de-taints nothing. */
        if (t != IDL_ANY && t != IDL_DICT && t != IDL_DICT_OR_BOOL_FIRST && t != IDL_INTERFACE &&
            concolic_is(a)) {
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            *slot = JS_DupValue(ctx, a);
            goto placed;
        }

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

        if (t == IDL_DICT || t == IDL_DICT_OR_BOOL_FIRST) {
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
            /* §3.2.18 step 2: a value that is NOT undefined, null or an Object is a TypeError before any member
               is read — `new Blob([], 123)` throws, and reading `123.type` instead answered undefined and built
               a Blob. The union form is exempt because its whole rule is that a non-object IS a member. */
            if (t == IDL_DICT && !JS_IsObject(a) && !JS_IsUndefined(a) && !JS_IsNull(a)) {
                JS_FreeValue(ctx, cb_result);
                JS_ThrowTypeError(ctx, "the dictionary argument is neither an object, null nor undefined");
                return JS_STEP_ABRUPT;
            }
            if (!JS_IsObject(a)) {
                JS_FreeValue(ctx, cb_result);
                cb_result = JS_UNDEFINED;
                /* §2.7 "flatten": a non-object IS the first member's boolean. There is nothing to READ, so
                   this runs none of the page's code either way. */
                if (t == IDL_DICT_OR_BOOL_FIRST && m->dict_n > 0) {
                    DCHECK(m->dict[0].type == IDL_BOOLEAN,
                           "a (dictionary or boolean) union declared a non-boolean first member — the union's "
                           "rule is that the bare value IS that member");
                    JS_SetPropertyStr(ctx, *idl_arg_slot(m, s, s->i), m->dict[0].name,
                                      JS_NewBool(ctx, JS_ToBool(ctx, a)));
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
                else if (mt == IDL_SEQUENCE_DOMSTRING || mt == IDL_SEQUENCE_INTERFACE) {
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
                    /* §3.2.16 on a dictionary member, which is where StaticRangeInit's four live: the brand
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
                s->dict_i++;
            }
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            s->dict_i = 0;
            s->i++;
            continue;
        }

        /* §3.2.20's `sequence<T>`: the ES ITERATOR PROTOCOL, and a value that is not an Object is a TypeError
           BEFORE anything is read — `new Blob("fail")` throws even though a string is iterable, because the
           check is on the TYPE and not on iterability, and `new Blob(null)` throws for the same reason.
           IT IS CONVERTED HERE AND NOT IN THE BODY, which is the whole point of it being a declared type: Web
           IDL converts arguments LEFT TO RIGHT, so a sequence that throws mid-iteration must run before the
           dictionary after it is read at all. Driven from the body it ran after every other argument, and
           `new Blob(throwingIterable, {get type(){…}})` called the type getter the spec never reaches. */
        if (t == IDL_SEQUENCE_BLOBPART || t == IDL_SEQUENCE_INTERFACE) {
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
                       whose ToString is the page's code. Stated once, here, like BodyInit's. */
                    if (blob_is(s->seq.value) || JS_IsArrayBuffer(s->seq.value) ||
                        JS_GetTypedArrayType(s->seq.value) >= 0 || JS_IsDataView(s->seq.value)) {
                        JS_SetPropertyUint32(ctx, s->seq_list, s->seq_n++, JS_DupValue(ctx, s->seq.value));
                        continue;
                    }
                    s->seq_phase = 2;
                }
                {
                    JSValue str = JS_UNDEFINED;
                    DCHECK(s->seq_phase == 2, "the sequence conversion resumed at a phase it never parks in");
                    r = step_tostring_run(ctx, &s->hdr, s->seq.value, cb_result, &str, out_cb, out_argc);
                    cb_result = JS_UNDEFINED;
                    if (r > 0) return r;
                    if (r < 0) return JS_STEP_ABRUPT;
                    str = JS_ToScalarValueString(ctx, str);   /* §3.2.11: lone surrogates become U+FFFD */
                    if (JS_IsException(str)) return JS_STEP_ABRUPT;
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

        /* §3.2.16's INTERFACE type: the brand test, once, so no body performs it — and a value that is not one
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

        /* §3.2.22's NULLABLE CALLBACK INTERFACE: null and undefined are the IDL null, ANY object crosses as
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
        if (idl_is_numeric(t)) {
            double num;   /* see the dictionary member's arm: a local, because a park writes nothing */

            r = step_todouble_run(ctx, &s->hdr, a, cb_result, &num, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
            *slot = idl_num_of(ctx, t, num);
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
               "type (§3.2.19), so idl_enum_values must name it beside the declaration");
        DCHECK(t == IDL_DOMSTRING || t == IDL_BYTESTRING || t == IDL_USVSTRING || t == IDL_ENUM,
               "an IDL argument was declared with a type this machine does not convert");
        r = step_tostring_run(ctx, &s->hdr, a, cb_result, slot, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;          /* parked ON THIS ARGUMENT; the resume comes back to it */
        if (r < 0) return JS_STEP_ABRUPT;
        if (t == IDL_BYTESTRING && idl_bytestring_check(ctx, *slot) < 0) return JS_STEP_ABRUPT;
        /* §3.2.19's ENUMERATION, AT A POSITIONAL ARGUMENT — the same check the dictionary path makes, over the
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

    DCHECK(!variadic || nargs >= 1,
           "a variadic member declared no argument types — the LAST one is the tail's type, which is what `T...` "
           "states, so there is always at least one");
    idl_member(g_n - 1)->variadic = variadic;
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
    /* HOW MANY THE CALLER MUST PASS. §3.6.2 throws a TypeError before ANY conversion when a call has fewer
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
            if (types[k] == IDL_DICT || types[k] == IDL_DICT_OR_BOOL_FIRST ||
                types[k] == IDL_STRING_OR_DICT) ndict++;
        DCHECK(ndict == 1, "a member declared dictionary members but not exactly one dictionary argument — the "
                           "conversion cursor is per-member, so a second dictionary would read the first's "
                           "names");
        idl_member(idx)->dict_atoms = malloc(sizeof(JSAtom) * (size_t)nmembers);
        CHECK(idl_member(idx)->dict_atoms, "idl: OOM interning a dictionary's member names");
        for (k = 0; k < nmembers; k++) {
            idl_dict_order_check(members, k);
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

/* See idl_args.h. Same "names the last declaration" rule as idl_optional_from. */
void idl_iface_brand(JSClassID iface)
{
    DCHECK(g_n > 0, "an interface brand was declared before any member was");
    DCHECK(!g_sealed, IDL_LAST_DECL_ONLY);
    DCHECK(iface != 0, "an interface brand named no class — the class is half of what the type states");
    idl_member(g_n - 1)->iface = iface;
    idl_member(g_n - 1)->iface_narrow = NULL;   /* a fresh brand narrows to nothing until the member says so */
}

/* §3.2.19's ENUMERATION VALUES for the member's IDL_ENUM position — see idl_args.h. Named after the
   declaration, on the member the LAST one made, exactly as idl_iface_brand and idl_optional_from are and for
   the same reason: the id a declaration returns is the RUNTIME's step id, not this pool's index. */
void idl_enum_values(const char *const *values)
{
    DCHECK(g_n > 0, "an enumeration's value list was declared before any member was");
    DCHECK(!g_sealed, IDL_LAST_DECL_ONLY);
    DCHECK(values != NULL && values[0] != NULL,
           "an enumeration declared an empty value list — every §3.2.19 enumeration has at least one value, and "
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

/* §3.2.24's CREATE FROZEN ARRAY — see idl_args.h for why the preventExtensions half alone is not it. */
int idl_freeze_array(JSContext *ctx, JSValueConst arr)
{
    JSValue len_v;
    uint32_t n = 0, i;
    JSAtom k;
    int r;

    DCHECK(JS_IsArray(arr), "idl_freeze_array was handed something that is not an Array — a FrozenArray IS an "
                            "Array at §3.2.24's integrity level, and freezing anything else here would answer "
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

static JSValue idl_replaceable_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                   int magic, JSValue *data)
{
    const char *name;
    int r;

    (void)magic;
    DCHECK(argc >= 1, "a setter was called with no value — the interpreter passes undefined for a bare `x.y = `");
    name = JS_ToCString(ctx, data[0]);   /* the function's own data: a string this file put there */
    if (!name) return JS_EXCEPTION;
    r = idl_replace_with_value(ctx, this_val, name, argv[0]);
    JS_FreeCString(ctx, name);
    return r < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

/* The getter for the FIXED-VALUE form: the value is the function's data, so the realm owns it for as long as
   the getter does and there is no slot on the target to keep in step with it. */
static JSValue idl_replaceable_get_value(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                         int magic, JSValue *data)
{
    (void)this_val; (void)argc; (void)argv; (void)magic;
    return JS_DupValue(ctx, data[0]);
}

/* Both forms end here: an accessor with §3.7.6's shared setter, at an IDL attribute's flags. */
static void idl_define_replaceable(JSContext *ctx, JSValueConst target, const char *name, JSValue getter)
{
    JSAtom a = JS_NewAtom(ctx, name);
    JSValue nm, setter;

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

void idl_install_replaceable_value(JSContext *ctx, JSValueConst target, const char *name, JSValue value)
{
    JSValue g = JS_NewCFunctionData2(ctx, idl_replaceable_get_value, name, 0, 0, 1, (JSValueConst *)&value);

    CHECK(!JS_IsException(g), "a replaceable attribute's getter could not be allocated");
    JS_FreeValue(ctx, value);   /* the getter holds its own reference */
    idl_define_replaceable(ctx, target, name, g);
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

JSValue idl_step_function(JSContext *ctx, const char *name, int length, int stepid)
{
    int idx = idl_member_of_step(stepid);
    /* NAMING THE OFFENDER IS THE POINT. "some member was never declared" sends whoever hits it grepping every
       install site; the name is right here in the argument, so the assert says it. */
    if (idx < 0) {
        /* Sized past the format's own minimum, which the compiler computes and warns about: this message ends
           by naming the install function to use instead, and a truncated DFAIL loses exactly that tail. */
        char why[288];
        snprintf(why, sizeof why,
                 "step function '%s' was minted for a member this pool never declared — a step machine that is "
                 "not an args-machine member installs through idl_install_step_method", name ? name : "?");
        DFAIL(why);
    }
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

/* The pool interns one atom per dictionary member, for the runtime's life — release them with it. */
void idl_args_free(JSContext *ctx)
{
    int i, k;
    for (i = 0; i < g_n; i++) {
        for (k = 0; k < idl_member(i)->dict_n; k++)
            JS_FreeAtom(ctx, idl_member(i)->dict_atoms[k]);
        free(idl_member(i)->dict_atoms);
        idl_member(i)->dict_atoms = NULL;
        /* The declared type list, copied at the declaration and owned for the runtime's life — the third thing
           this pool allocates per member, and it is freed here beside the other two. */
        free(idl_member(i)->types);
        idl_member(i)->types = NULL;
        /* The joined stage labels — the strings themselves are statics belonging to the member and to this
           file; the array that names them is this pool's. */
        free((void *)idl_member(i)->steps);
        idl_member(i)->steps = NULL;
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
    for (i = 0; i < g_nchunks; i++)
        free(g_chunks[i]);
    free(g_chunks);
    g_chunks = NULL;
    g_nchunks = 0;
    g_n = 0;
    g_rt = NULL;
    g_sealed = false;
}
