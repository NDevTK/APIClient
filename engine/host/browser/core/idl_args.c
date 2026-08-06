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
#include "core/idl_iter.h"
#include "core/file/blob.h"
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
    return (int64_t)x;
}

/* WHAT AN INTEGER TYPE IS, read off the declaration rather than remembered per call site. */
static bool idl_is_integer(IdlArgType t)
{
    return t == IDL_LONG || t == IDL_UNSIGNED_LONG || t == IDL_UNSIGNED_SHORT ||
           t == IDL_LONG_LONG || t == IDL_LONG_LONG_CLAMP;
}

static int64_t idl_int_of(IdlArgType t, double x)
{
    switch (t) {
    case IDL_LONG:            return idl_int_convert(x, 32, true,  false);
    case IDL_UNSIGNED_LONG:   return idl_int_convert(x, 32, false, false);
    case IDL_UNSIGNED_SHORT:  return idl_int_convert(x, 16, false, false);
    case IDL_LONG_LONG:       return idl_int_convert(x, 64, true,  false);
    default:
        DCHECK(t == IDL_LONG_LONG_CLAMP, "a non-integer type reached the integer conversion");
        return idl_int_convert(x, 64, true, true);
    }
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
#define IDL_MAX_ARGS     8

typedef struct {
    IdlSetter  setter;      /* set instead of `body` for an attribute setter */
    bool       null_to_empty;
    IdlBody    body;
    IdlArgType types[IDL_MAX_DECLARED];
    int        nargs;      /* how many the IDL lists; a variadic tail repeats the last */
    /* THE FIRST OPTIONAL ARGUMENT's index. §3.6.2 resolves an `undefined` passed for an optional argument with
       no default as the argument being ABSENT — `new URL("aaa:b", undefined)` is a one-argument call, not a
       call with the base "undefined". Declared per member rather than assumed, because the same undefined at a
       REQUIRED position is the string "undefined" and collapsing the two is wrong in one direction or the
       other. Defaults past the end, so a member that does not declare it converts every position as before. */
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
    /* A member whose algorithm is itself page code runs as a STEP once the conversions are done — see
       idl_method_id_step. Its state lives immediately after this machine's, which is why the def's size is
       per-member and not a constant. */
    const IdlStepDecl *step;
    bool       variadic;    /* the last declared type applies to every argument from there on */
    JSClassID  iface;       /* the brand an IDL_STRING_UNLESS_IFACE position tests against */
    const char *name;       /* what to call this member in a diagnostic; set when it is installed */
} IdlMember;

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
static bool           g_sealed;   /* the document's install is done, so no further declaration can be correct */

void idl_args_seal(void) { g_sealed = true; }

typedef struct {
    JSStepHdr hdr;      /* FIRST — the driver writes the def and the operand bounds through it */
    int       i;        /* THE RESUME POINT: the argument being coerced */
    int       n;        /* how many of them there are */
    JSValue   result;   /* the body's answer (owned) */
    int       dict_i;   /* THE OTHER RESUME POINT: the dictionary member being read */
    uint8_t   dict_phase;   /* 0 = read the member, 1 = convert what was read. Both can park, so a member needs
                               two resume points, not one — a resume in the CONVERSION must not re-read. */
    JSValue   dict_v;   /* the member's value between those two phases (owned) */
    /* WHAT ToNumber PRODUCED, before the type's arithmetic — kept as the NUMBER because that is what §3.2 says
       to work from, and a saturating int64 has already lost the modulo and the half-to-even rounding. */
    double    nums[IDL_MAX_ARGS];
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
    /* §4.2.3's tree steps this member's body caused, taken from the DOM layer so the drain is per-machine: the
       drain YIELDS, and a shared list would be appended to by whichever flow ran during the suspension. */
    void     *tree;
    uint8_t   tree_after_body;   /* 1 = the body is finished; the member ends when the drain does */
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
    iter_cursor_visit(ctx, &s->seq, v);
    v->val(ctx, &s->seq_list);
    for (i = 0; i < IDL_MAX_ARGS; i++)
        v->val(ctx, &s->args[i]);
    DCHECK(s->hdr.arg >= 0 && s->hdr.arg < g_n, "an IDL member's visit ran with no pool entry behind it");
    /* A FORK CANNOT HAPPEN MID-DRAIN, and that is why this buffer needs no clone contract. A fork is a concolic
       branch, which is bytecode; the drain runs none — every per-node effect is an enqueue. If this ever fires,
       the drain grew a call into the page and the buffer needs a real ownership declaration. */
    DCHECK(s->tree == NULL, "an IDL member was forked while draining its tree steps");
    m = idl_member(s->hdr.arg);
    if (m->step && m->step->visit) m->step->visit(ctx, (char *)st + sizeof(JSIdlArgsState), v);
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

/* THE DRAIN, one node per entry. Returns JS_STEP_YIELD while it has work, or 0 when there is none left. */
static int idl_tree_drain(JSContext *ctx, JSIdlArgsState *s)
{
    if (!s->tree) return 0;
    if (g_tree->step(ctx, s->tree)) return JS_STEP_YIELD;
    g_tree->release(ctx, s->tree);
    s->tree = NULL;
    return 0;
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
#if APICLIENT_DEV
    int64_t t0 = idl_now_ms();
    int rr = js_idl_args_step_inner(ctx, st, cb_result, out_cb, out_argc);
    int64_t d = idl_now_ms() - t0;
    g_step_total += d;
    g_step_count++;
    if (d > g_slow_ms) {
        JSIdlArgsState *ss = st;
        g_slow_ms = d;
        g_slow_name = (ss->hdr.arg >= 0 && ss->hdr.arg < g_n && idl_member(ss->hdr.arg)->name)
                    ? idl_member(ss->hdr.arg)->name : "(a member installed by neither install path)";
    }
    return rr;
#else
    return js_idl_args_step_inner(ctx, st, cb_result, out_cb, out_argc);
#endif
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

    /* THE DRAIN COMES FIRST ON EVERY RE-ENTRY, before the conversion loop or the body, because the steps the
       previous step recorded must finish before anything else this member does. */
    if (s->tree) {
        JS_FreeValue(ctx, cb_result);
        r = idl_tree_drain(ctx, s);
        if (r) return r;
        if (s->tree_after_body) return JS_STEP_DONE;
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
        s->tree = NULL;
        s->tree_after_body = 0;
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

        /* §3.6.2: an optional argument given `undefined` is ABSENT, so nothing is converted and the body sees
           undefined — which is what lets it tell "no base" from the base "undefined". */
        if (s->i >= m->first_optional && JS_IsUndefined(a)) {
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
                else if (idl_is_integer(mt)) {
                    r = step_todouble_run(ctx, &s->hdr, s->dict_v, cb_result, &s->nums[s->i], out_cb, out_argc);
                    cb_result = JS_UNDEFINED;
                    if (r > 0) return r;   /* parked ON THIS MEMBER's conversion; the read does not re-run */
                    if (r < 0) return JS_STEP_ABRUPT;
                    JS_FreeValue(ctx, s->dict_v);
                    s->dict_v = JS_NewInt64(ctx, idl_int_of(mt, s->nums[s->i]));
                }
                else if (mt == IDL_DOMSTRING || mt == IDL_DOMSTRING_NULLABLE || mt == IDL_BYTESTRING ||
                         mt == IDL_USVSTRING || mt == IDL_ENUM) {
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
                        if (mt == IDL_BYTESTRING && idl_bytestring_check(ctx, s->dict_v) < 0)
                            return JS_STEP_ABRUPT;
                        if (mt == IDL_USVSTRING) {
                            s->dict_v = JS_ToScalarValueString(ctx, s->dict_v);
                            if (JS_IsException(s->dict_v)) return JS_STEP_ABRUPT;
                        }
                        if (mt == IDL_ENUM &&
                            idl_enum_check(ctx, s->dict_v, dm->values, dm->name) < 0)
                            return JS_STEP_ABRUPT;
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

        /* §3.2.20's `sequence<T>`: the ES ITERATOR PROTOCOL, and a value that is not an Object is a TypeError
           BEFORE anything is read — `new Blob("fail")` throws even though a string is iterable, because the
           check is on the TYPE and not on iterability, and `new Blob(null)` throws for the same reason.
           IT IS CONVERTED HERE AND NOT IN THE BODY, which is the whole point of it being a declared type: Web
           IDL converts arguments LEFT TO RIGHT, so a sequence that throws mid-iteration must run before the
           dictionary after it is read at all. Driven from the body it ran after every other argument, and
           `new Blob(throwingIterable, {get type(){…}})` called the type getter the spec never reaches. */
        if (t == IDL_SEQUENCE_BLOBPART) {
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
                 blob_is(a) || form_data_list_of(a) || usp_list_of(a))
              ? IDL_ANY : IDL_DOMSTRING;
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
        if (idl_is_integer(t)) {
            r = step_todouble_run(ctx, &s->hdr, a, cb_result, &s->nums[s->i], out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r > 0) return r;
            if (r < 0) return JS_STEP_ABRUPT;
            *slot = JS_NewInt64(ctx, idl_int_of(t, s->nums[s->i]));
            goto placed;
        }
        DCHECK(t != IDL_ENUM,
               "an ENUMERATION was declared as a positional argument — the value list lives on a dictionary "
               "member, so a positional one has nothing to check against; give the declaration somewhere to "
               "carry the list");
        DCHECK(t == IDL_DOMSTRING || t == IDL_BYTESTRING || t == IDL_USVSTRING,
               "an IDL argument was declared with a type this machine does not convert");
        r = step_tostring_run(ctx, &s->hdr, a, cb_result, slot, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;          /* parked ON THIS ARGUMENT; the resume comes back to it */
        if (r < 0) return JS_STEP_ABRUPT;
        if (t == IDL_BYTESTRING && idl_bytestring_check(ctx, *slot) < 0) return JS_STEP_ABRUPT;
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
        if (r < 0) {
            s->result = JS_UNDEFINED;
            DCHECK(!g_tree || !g_tree->recorded(), IDL_TREE_THREW);
            return JS_STEP_ABRUPT;
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
        return r ? r : JS_STEP_DONE;
    }
    s->result = m->setter
        ? m->setter(ctx, s->hdr.this_val, s->n > 0 ? s->args[0] : JS_UNDEFINED, m->magic)
        : m->body(ctx, s->hdr.this_val, s->n, (JSValueConst *)(argv_vec ? argv_vec : s->args), m->magic);
    idl_free_vec(ctx, argv_vec, s->n);
    if (JS_IsException(s->result)) {
        s->result = JS_UNDEFINED;
        DCHECK(!g_tree || !g_tree->recorded(), IDL_TREE_THREW);
        return JS_STEP_ABRUPT;
    }
    idl_tree_take(ctx, s);
    s->tree_after_body = 1;
    if (s->tree) { int d = idl_tree_drain(ctx, s); if (d) return d; }
    return JS_STEP_DONE;
}

static JSValue idl_args_result(JSContext *ctx, void *st, bool take_result)
{
    JSIdlArgsState *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    const IdlMember *m;
    int i;

    DCHECK(s->hdr.arg >= 0 && s->hdr.arg < g_n, "an IDL member's teardown ran with no pool entry behind it");
    m = idl_member(s->hdr.arg);
    /* The step body's state goes FIRST: it may hold values this machine's arguments are the only other
       reference to, and a release that runs after they are freed reads what it no longer owns. */
    if (m->step && m->step->release) m->step->release(ctx, (char *)st + sizeof(JSIdlArgsState));
    /* An abandoned drain — the member threw, or the flow was dropped mid-walk. The remaining nodes' steps do
       not run, which is what an abrupt completion means, but the buffer is still this machine's to free. */
    if (s->tree && g_tree) { g_tree->release(ctx, s->tree); s->tree = NULL; }

    if (take_result) s->result = JS_UNDEFINED;
    JS_FreeValue(ctx, s->result);
    s->result = JS_UNDEFINED;
    JS_FreeValue(ctx, s->dict_v);   /* a member read whose conversion never completed — the throw path owns it */
    JS_FreeValue(ctx, s->conv);
    JS_FreeValue(ctx, s->vstage);
    s->dict_v = s->conv = s->vstage = JS_UNDEFINED;
    /* A sequence argument whose walk never finished — the iterator the cursor still holds, and the elements
       converted so far. Both are this machine's, on the throw path exactly as on the normal one. */
    iter_cursor_release(ctx, &s->seq);
    JS_FreeValue(ctx, s->seq_list);
    s->seq_list = JS_UNDEFINED;
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
    idl_member(g_n - 1)->variadic = variadic;
    idl_member(g_n - 1)->iface = iface;
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
    /* THE CEILING WAS ALSO A DETECTOR, and this is what it was really detecting. A member declared more than
       once — a per-wrapper install minting a definition per object — showed up as the pool filling, which is a
       number standing in for the actual invariant: a component DECLARES at init and installs from the cached
       id, so once the document's install is done no declaration can be correct. That is asserted directly now,
       which catches the same bug at the first repeat instead of the 384th and cannot be reached by a platform
       that simply has more members in it. */
    DCHECK(!g_sealed,
           "an IDL member was declared after the document was installed — a component declares in its init and "
           "installs from the cached id; a declaration reached from a wrapper or a flow mints a definition per "
           "object");
    g_rt = rt;
    idx = g_n++;
    idl_pool_reserve(idx);   /* the pool grows to fit the platform; there is nothing here to run out of */
    CHECK(nargs >= 0 && nargs <= IDL_MAX_DECLARED,
          "a member declared more argument types than IDL_MAX_DECLARED holds");   /* 0 = a getter, which takes none */
    idl_member(idx)->body          = body;
    idl_member(idx)->setter        = NULL;
    idl_member(idx)->null_to_empty = false;
    idl_member(idx)->nargs = nargs;
    idl_member(idx)->first_optional = IDL_MAX_DECLARED + 1;   /* none, until the member says otherwise */
    idl_member(idx)->magic = magic;
    for (k = 0; k < nargs; k++)
        idl_member(idx)->types[k] = types[k];
    idl_member(idx)->dict = members;
    idl_member(idx)->dict_n = 0;
    idl_member(idx)->dict_atoms = NULL;
    if (members) {
        int ndict = 0;
        for (k = 0; k < nargs; k++)
            if (types[k] == IDL_DICT || types[k] == IDL_DICT_OR_BOOL_FIRST) ndict++;
        DCHECK(ndict == 1, "a member declared dictionary members but not exactly one dictionary argument — the "
                           "conversion cursor is per-member, so a second dictionary would read the first's "
                           "names");
        idl_member(idx)->dict_atoms = malloc(sizeof(JSAtom) * (size_t)nmembers);
        CHECK(idl_member(idx)->dict_atoms, "idl: OOM interning a dictionary's member names");
        for (k = 0; k < nmembers; k++) {
            /* §3.2.18 READS A DICTIONARY'S MEMBERS IN LEXICOGRAPHIC ORDER, not in the order the IDL writes
               them — a page can see which of two getters ran first, and BlobPropertyBag declares `type` before
               `endings` while the spec reads `endings` first. The machine reads in DECLARED order, so the
               declaration must BE that order, and this is what makes that a crash rather than something each
               component remembers. A dictionary that INHERITS another reads the inherited members first and
               each level sorted, which no flat list can express — the first one to need it splits this list
               into levels rather than relaxing the check. */
            DCHECK(k == 0 || strcmp(members[k - 1].name, members[k].name) < 0,
                   "a dictionary's members were declared out of lexicographic order — §3.2.18 reads them "
                   "sorted, and the machine reads them as declared");
            idl_member(idx)->dict_atoms[k] = JS_NewAtom(ctx, members[k].name);
        }
        idl_member(idx)->dict_n = nmembers;
    }
    idl_member(idx)->step = NULL;
    idl_member(idx)->variadic = false;
    idl_member(idx)->iface = 0;
    idl_def(idx)->size  = sizeof(JSIdlArgsState);
    idl_def(idx)->step  = js_idl_args_step;
    idl_def(idx)->fini  = idl_args_result;
    idl_def(idx)->arg   = idx;
    idl_def(idx)->visit = js_idl_args_visit;
    {
        int sid = JS_RegisterStepDef(rt, idl_def(idx));
        idl_map_step(sid, idx);   /* the one place the runtime's id and this pool's index are both in hand */
        return sid;
    }
}

int idl_method_id_step(JSContext *ctx, const IdlArgType *types, int nargs,
                       const IdlDictMember *members, int nmembers,
                       const IdlStepDecl *decl, int magic)
{
    int id = idl_method_id_dict(ctx, types, nargs, members, nmembers, NULL, magic);
    /* the pool entry idl_method_id_dict just filled — a step member differs only in WHAT runs once the
       conversions are done, and in needing room after this machine's state for that thing to run in. */
    idl_member(g_n - 1)->step = decl;
    idl_def(g_n - 1)->size = sizeof(JSIdlArgsState) + decl->state_size;
    return id;
}

/* See idl_args.h. It names the member the LAST declaration made, the way idl_method_id_ext sets `variadic` and
   `iface` — the id a declaration returns is the RUNTIME's step id and not this pool's index, so reaching the
   entry through the id was reading past the pool. */
void idl_optional_from(int first_optional)
{
    DCHECK(g_n > 0, "an optional-argument index was declared before any member was");
    idl_member(g_n - 1)->first_optional = first_optional;
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

void idl_install_accessor(JSContext *ctx, JSValueConst target, const char *name,
                          IdlGetter getter, int getter_magic, int setter_stepid)
{
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
    JS_DefinePropertyGetSet(ctx, (JSValue)target, a, g, st,
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, a);
}

/* THE ONE PLACE A STEP MEMBER IS MINTED. A member is DECLARED (which builds its pool entry) before it is
   given a name, and stepid ties the two together — so whoever mints the function is the only one who can tell
   the pool what to call itself. There were THREE ways to mint one: this file's install helper, and a bare
   JS_NewCFunction2(..., JS_CFUNC_step, stepid) written out at six other sites. Only the first named anything,
   which is how a five-second member step reported itself as "(none)" and stayed anonymous through two rebuilds.
   Six hand-written copies of one call is the same shape as one builtin answering differently depending on which
   spelling reached it: the fix is not to name them one by one, it is for there to be one mint. */
JSValue idl_step_function(JSContext *ctx, const char *name, int length, int stepid)
{
    int idx = idl_member_of_step(stepid);
    /* NAMING THE OFFENDER IS THE POINT. "some member was never declared" sends whoever hits it grepping every
       install site; the name is right here in the argument, so the assert says it. */
    if (idx < 0) {
        char why[160];
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

void idl_install_method(JSContext *ctx, JSValueConst target, const char *name, int length, int stepid)
{
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

/* The pool interns one atom per dictionary member, for the runtime's life — release them with it. */
void idl_args_free(JSContext *ctx)
{
    int i, k;
    for (i = 0; i < g_n; i++) {
        for (k = 0; k < idl_member(i)->dict_n; k++)
            JS_FreeAtom(ctx, idl_member(i)->dict_atoms[k]);
        free(idl_member(i)->dict_atoms);
        idl_member(i)->dict_atoms = NULL;
    }
    for (i = 0; i < g_nchunks; i++)
        free(g_chunks[i]);
    free(g_chunks);
    g_chunks = NULL;
    g_nchunks = 0;
    g_n = 0;
    g_rt = NULL;
    g_sealed = false;
}
