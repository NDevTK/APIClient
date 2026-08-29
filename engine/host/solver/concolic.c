/* The concolic value type — see concolic.h. A host component built on upstream quickjs's PUBLIC class API, so
   the qjs fork carries no value-type delta. */
#include "solver/concolic.h"
#include "solver/absent.h"
#include "solver/flow.h"
#include "solver/reclaim.h"   /* the engine's own allocations ask for a flow back before they fail */
#include "check.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* The per-value state hung off the JSObject via JS_SetOpaque.
 *
 * `src`, `root` AND `ident` ARE THREE DIFFERENT FACTS, AND ONE FIELD HAS TWICE BEEN CAUGHT ANSWERING TWO.
 *   `src` is the INJECTION IDENTITY — the source an @S candidate substitutes at. A derivation is entitled to
 *   mint a new one and two of them do: a field read of an unknown OBJECT is an independently controlled datum
 *   (`{state}.admin`), and so is the result of calling an unknown.
 *   `root` is the DELIVERY PROVENANCE — the source whose component physically carried these bytes into the
 *   program. It is INHERITED unchanged through EVERY derivation, including the two above, because nothing a
 *   derivation does can change how the attacker got the bytes there. `location.hash.slice(1)` is no longer the
 *   fragment for INJECTION (the `#` is gone, and concolic_lead_hook says so) and is still the fragment for
 *   DELIVERY, which is what a reproduction envelope and a percent-encode set are questions about.
 *   `ident` is IDENTITY — WHICH VALUE THIS IS. It is COMPOSED at every derivation, because it is what the
 *   per-flow path constraint is keyed by, and a constraint key must name the value a branch tests.
 * With one field answering both, `x`, `x*2`, `x < 700` and `x < 300` were ONE fact: a flow that decided any of
 * them decided all of them, so feasible-refinement pruned arms the flow's constraint does not contradict —
 * the one direction §Solver-half forbids ("a contradicted branch is pruned (sound-only — uncertainty keeps
 * the arm)"), and the one that no gate can see, because a pruned arm emits nothing to be wrong.
 *
 * `ident` IS ABSENT (NULL) WHERE THIS ENGINE CANNOT SPELL THE VALUE EXACTLY — an operand that is a plain
 * object or a symbol, a property name that would not convert. That is a POSITIVE statement and not a hole: a
 * value with no identity is never decided from another value's record, so BOTH arms of every branch over it
 * stay. Absence costs forks; a wrong identity costs the arm. */
/* WITH `src` ANSWERING THE DELIVERY QUESTION TOO, THE REPORT LIED ABOUT A FINDING IT HAD JUST FIRED. A fixture
 * doing `eval("var t='" + location.hash.slice(1) + "';")` was loaded in real Chrome at
 * `http://127.0.0.1:8781/#';X9()//`; the sink fired, and the popup's reproduction envelope said "the engine
 * declares no browser delivery for this source — nothing carries or transforms these bytes on the way in, so
 * there is no navigation that reproduces it", about a payload delivered in the victim's URL fragment by the
 * single navigation that had just been performed. The chain is `location.hash` (src `location.hash`, declared)
 * -> concolic_exotic_get `.slice` (src `{location.hash}.slice`) -> concolic_call `()` (src
 * `{location.hash}.slice()`), and the registry is an exact strcmp, so it matched nothing. Nothing was broken
 * on the consumer side: reading absence as "the source declared none" is exactly what a consumer owes a
 * producer. The producer had thrown the fact away three derivations earlier. */
typedef struct {
    char *shape;        /* @H/@S display form */
    char *src;          /* INJECTION IDENTITY: the source an @S candidate substitutes at */
    char *root;         /* DELIVERY PROVENANCE: where the bytes ENTERED. Inherited unchanged; NULL iff !src */
    char *ident;        /* IDENTITY: this exact value, composed below. NULL = this engine cannot spell it */
    JSValue example;    /* concrete example, or JS_UNDEFINED */
    int cmp_op;         /* for an EQUALITY RESULT: OPCMP_EQ/NE — `src <op> cmp_tok` (else OPCMP_NONE) */
    char *cmp_tok;      /* the concrete side of the equality */
    char *cmp_subj;     /* …and the HOLE KEY of the unknown side — see concolic_cmp_subject. The three are ONE
                           observation: written together at pred_new, asserted together there, and read
                           together by decide.c, which pins on one arm and excludes on the other. */
    /* …AND THE ORDERING'S OWN THREE FIELDS, which are the SAME observation for the other half of §Solver-half's
       two-facts rule and are deliberately NOT the equality's. An equality determines a VALUE on one arm; an
       ordering determines a BOUND on both, so the two cannot share `cmp_op` without one of them answering a
       question it was not asked. `rel_op` is normalised SUBJECT-ON-THE-LEFT (`5 < x` arrives as `x > 5`), so
       decide.c reads a relation it can state without knowing which operand the page wrote first. */
    RelOp rel_op;       /* for an ORDERING RESULT: the relation, subject-left (else REL_NONE)  */
    char *rel_tok;      /* the concrete side SPELLED — the page's own §6.1.6.1.20 Number::toString */
    double rel_num;     /* …and its value, which is what a merge orders two bounds by */
    char *rel_subj;     /* …and the HOLE KEY of the unknown side. All four written and asserted together. */
} Concolic;

/* ── THE ONE ENCODING ───────────────────────────────────────────────────────────────────────────────────────
 * Every identity this file composes and every constraint key decide.c builds is a TAG plus a sequence of
 * FIELDS, written as `<byte length>:<bytes>` each. Two properties follow from that and both are load-bearing:
 *   - NO FIELD'S CONTENTS CAN SPELL ANOTHER FIELD'S BOUNDARY, so a member is never mistaken for a separator
 *     and two different sequences can never write the same string. The separator-plus-forbidden-character
 *     scheme concolic_source_wrap_joint uses cannot be applied here, because a member is often a PROPERTY NAME
 *     and a page may name a property anything at all — a DCHECK forbidding a byte in it would be an abort the
 *     page triggers.
 *   - THE LENGTH IS MEASURED AND WRITTEN BY THE SAME TWO LINES, so a buffer that could truncate does not
 *     exist. A truncated key is two different predicates under one name, which is the same defect as the
 *     collapsed relational key arriving by a second route: `decide_key` built into a `char key[256]` with an
 *     unchecked snprintf while concolic_exotic_get's shapes alone were 192 bytes.
 * A member that is ABSENT makes the whole composition absent — see the struct above. */
static size_t ident_field_len(const char *f)
{
    char pre[24];
    int w = snprintf(pre, sizeof pre, "%lu:", (unsigned long)strlen(f));
    DCHECK(w > 0 && (size_t)w < sizeof pre, "an identity field's length did not fit its own prefix");
    return (size_t)w + strlen(f);
}

static size_t ident_field_put(char *out, size_t at, const char *f)
{
    char pre[24];
    int w = snprintf(pre, sizeof pre, "%lu:", (unsigned long)strlen(f));
    size_t l = strlen(f);
    DCHECK(w > 0 && (size_t)w < sizeof pre, "an identity field's length did not fit its own prefix");
    memcpy(out + at, pre, (size_t)w);
    memcpy(out + at + (size_t)w, f, l);
    return at + (size_t)w + l;
}

char *concolic_ident_compose(const char *tag, const char *const *fields, int n)
{
    size_t len, at;
    char *out;
    int i;

    DCHECK(tag != NULL,
           "an identity was composed with no TAG — the tag is what keeps a field read, a call, an arithmetic "
           "result and a comparison over the same operands four different values");
    DCHECK(n == 0 || fields != NULL, "an identity was composed with a member count and no members");
    for (i = 0; i < n; i++)
        if (!fields[i]) return NULL;   /* an unspellable member makes the whole identity absent */
    len = ident_field_len(tag);
    for (i = 0; i < n; i++) len += ident_field_len(fields[i]);
    out = reclaim_malloc(len + 1);
    CHECK(out, "concolic: OOM composing an identity — a value whose identity could not be spelled would be "
               "decided by whatever constraint another value left under the key it fell back to");
    at = ident_field_put(out, 0, tag);
    for (i = 0; i < n; i++) at = ident_field_put(out, at, fields[i]);
    out[at] = '\0';
    DCHECK(at == len, "an identity was composed to a different length than it was measured for");
    return out;
}

/* THE DISPLAY SHAPE, SIZED FROM ITS PARTS. Every shape in this file was a fixed 192- or 224-byte buffer, and
   concolic_exotic_get's shape is ALSO the field path an @S candidate is injected at — so a chain long enough
   to truncate gave two different sources one provenance. Measured once, written once, freed by the caller. */
static char *shapef(const char *fmt, ...)
{
    va_list ap;
    int n, m;
    char *out;

    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    CHECK(n >= 0, "concolic: a display shape could not be measured");
    out = reclaim_malloc((size_t)n + 1);
    CHECK(out, "concolic: OOM building a display shape");
    va_start(ap, fmt);
    m = vsnprintf(out, (size_t)n + 1, fmt, ap);
    va_end(ap);
    DCHECK(m == n, "a display shape was written to a different length than it was measured for");
    (void)m;
    return out;
}

/* WHICH KIND OF CONCRETE OPERAND THIS IS, AND WHETHER IT IS ONE THIS ENGINE MAY SPELL AT ALL — asked HERE and
   nowhere else, because a comparison spells its other operand TWICE (once as half the predicate's identity,
   once as the value the taken arm PINS the unknown to) and two classifications of one operand can disagree.
   They did: this function has said since it was written that an OBJECT or a SYMBOL is unspellable, and
   concolic_cmp_hook four hundred lines down called JS_ToCString on the same operand unconditionally. §7.1.19
   ToString step 10 sends an Object to ToPrimitive, which runs the page's own valueOf/toString from a C
   activation with no flow base under it — the abort JS_ToPrimitiveFree names — and step 2 throws a TypeError
   for a Symbol, which that hook would have left PENDING on ctx while reporting success. Every library utility
   that walks a prototype chain or tests an identity against a sentinel object reaches it on its first driven
   call: `while (n !== Object.prototype)` took four real bundles' worth of engine instances down. */
static const char *operand_tag(JSValueConst v)
{
    if (JS_IsString(v))    return "s";
    if (JS_IsNumber(v))    return "n";
    if (JS_IsBool(v))      return "b";
    if (JS_IsNull(v))      return "z";
    if (JS_IsUndefined(v)) return "u";
    if (JS_IsBigInt(v))    return "g";
    return NULL;   /* an Object or a Symbol — unspellable, see above */
}

/* THE OPERAND SPELLED — its §7.1.19 ToString, owned by the caller, NULL where it has none. This is also the
   value an equality PINS to, and that is one fact rather than two: `""+x` is what a pinned source contributes
   to a URL the page builds, so the pin and the identity must be the same bytes or a flow's later reads compute
   a different string from the one the predicate matched. */
static char *literal_tok(JSContext *ctx, JSValueConst v)
{
    const char *s;
    char *r;

    if (!operand_tag(v)) return NULL;
    s = JS_ToCString(ctx, v);
    if (!s) return NULL;
    r = strdup(s);
    CHECK(r, "concolic: OOM spelling a concrete operand");
    JS_FreeCString(ctx, s);
    return r;
}

/* A CONCRETE OPERAND'S IDENTITY IS ITS VALUE, AND ITS TYPE IS PART OF THAT — `x === 5` and `x === "5"` are two
   predicates and their operands print the same. An OBJECT or a SYMBOL has no identity this engine can spell
   (an object's is its address, which does not survive the park a resumed flow replays through, and coercing it
   would run the page's own `toString` from C), so it answers absent and both arms of the branch stay. */
static char *literal_ident(JSContext *ctx, JSValueConst v)
{
    const char *tag = operand_tag(v);
    const char *f[2];
    char *tok, *r;

    if (!tag) return NULL;
    tok = literal_tok(ctx, v);
    if (!tok) return NULL;
    f[0] = tag; f[1] = tok;
    r = concolic_ident_compose("k", f, 2);
    free(tok);
    return r;
}

/* An operand's identity whichever kind it is, always OWNED by the caller (NULL = unspellable). */
static char *ident_of_operand(JSContext *ctx, JSValueConst v)
{
    if (concolic_is(v)) {
        const char *id = concolic_ident_c(v);
        char *r;
        if (!id) return NULL;
        r = strdup(id);
        CHECK(r, "concolic: OOM copying an operand's identity");
        return r;
    }
    return literal_ident(ctx, v);
}

/* Mint a value DERIVED from an unknown one: `ident` is CONSUMED, `example` is CONSUMED, and the candidate
   substitution applies exactly as it does to a source read (see concolic_new). */
static JSValue concolic_derived(JSContext *ctx, const char *shape, const char *src, const char *root,
                                char *ident, JSValue example);
/* …and the same without the substitution, for a value that is NOT a source read: a comparison RESULT is a
   boolean the operator computed, so handing the attacker's payload back in its place would answer a predicate
   with a string. */
static JSValue concolic_alloc(JSContext *ctx, const char *shape, const char *src, const char *root,
                              char *ident, JSValue example);

/* THE PER-FLOW PATH CONSTRAINT. One map carrying every fact this flow has learned about the unknown input it
   read — which is the whole of what a DART/SAGE-lineage constraint is here, because concrete execution grounds
   all shared and interprocedural state and only the INPUT variables are ever symbolic. Two facts, because two
   things narrow a domain, and they are keyed by two different names for the good reason that they are facts
   about two different things: a PIN is about a SOURCE (which is what a later read of that source looks itself
   up by), a decided TRUTH is about a PREDICATE (decide.c composes its key from the tested value's identity,
   the operator and both operands). One entry holds whichever of the two its key names:
     val   — an EQ gate PINNED the source to a concrete value on its true arm, so later reads compute the REAL
              @H value (`/api/admin`, never `/api/{state}.role`). CONCRETIZE-ON-PIN.
     truth — a PREDICATE over this source was already decided in this flow. `if (cfg.admin)` is not an equality
              and pins nothing, yet taking its true arm still says the value is truthy FOR THIS FLOW — and a
              bundle branches on the same flag over and over. Without this every one of those branches forked
              again, so N tests of one flag cost 2^N flows and the frontier is exponential in a quantity that
              carries no information: the sibling arms are CONTRADICTED, they explore nothing, and each still
              drags a COW delta. Deciding them is feasible-refinement, the thing that makes forced multi-path
              execution tractable, and it is sound-only — it prunes a branch on the SAME predicate the flow has
              already fixed, never one whose domain still permits both outcomes.
     pinned_root — SOME value ROOTED at this key was pinned by this flow, which is a fact about the SOURCE and
              not about the value. `event.origin`, `event.origin.toLowerCase()` and `String(event.origin)` are
              three identities with one root, and a demand made of any of them is one demand on the attacker's
              principal. It is a separate field and not `val` because it is a different claim: writing the
              lowercased token under `message.origin` would make a later read of `event.origin` ITSELF answer
              it, which is a fabricated value in exactly the direction §Solver-half forbids.
     excl  — the tokens an equality gate over this HOLE proved it is NOT, on the arm `val` does not cover.
              Forced multi-path runs both arms of every `x === "admin"`, so a pin and an exclusion are minted
              at the same rate; without this the second of the two flows knew nothing it could report about
              its own parameter. It is keyed by the HOLE KEY rather than by a source's `src` — the two differ
              for every derived value (see concolic_hole_key) and the emission has only the hole.
     bnd   — the INTERVAL an ordering gate over this HOLE narrowed it to, which is the exclusion's twin over an
              ORDERED domain and the second-most-frequent gate class in real minified bundles. It is a pointer
              rather than six inline fields because an entry either holds a domain over an ordered value or it
              does not, and paying six words on every pin and every branch outcome would grow the chain the
              cold tier measures for a fact most entries never carry.
              WITHIN ONE FLOW THE TWO SIDES CONJOIN. `if (x > 5 && x < 100)` is TWO observations of one
              parameter and a representation holding only one of them is a wrong report by the same rule that
              made `excl` necessary — so a lower and an upper bound are separate fields, each narrowed
              (max for the lower, min for the upper) as the flow observes more gates, and a tie between an
              inclusive and an exclusive bound at the same number keeps the EXCLUSIVE one, which is the
              tighter fact.
   All five are per-flow and travel together, which is why they are ONE entry rather than five maps that a
   fork, a suspend and a resume would each have to remember to carry. */
/* One side of an interval, and the SPELLING beside the number. The number is what a merge orders two bounds
   by; the text is the page's own §6.1.6.1.20 Number::toString of the literal it wrote, kept so nothing
   downstream re-spells a double and reports a bound the source file does not contain. */
typedef struct { double num; char *txt; signed char present; signed char inclusive; } BoundSide;
typedef struct { BoundSide lo, hi; } Bound;
typedef struct { char *key; char *val; char **excl; int nexcl; Bound *bnd;
                 signed char truth; signed char pinned_root; } Cons;

static void bound_side_free(BoundSide *s) { free(s->txt); s->txt = NULL; s->present = 0; }
/* THE ONE COPY OF ONE SIDE, because a bound is copied at the fork's copy-up and nowhere else, and a field
   added to BoundSide must be copied there or a sibling silently inherits half a fact. */
static void bound_side_copy(BoundSide *d, const BoundSide *s) {
    DCHECK(!s->present == !s->txt,
           "a bound side's presence and its spelling disagree — the two are written by one line, and a side "
           "that is present with no text is a bound the emission would read as a number it cannot print");
    *d = *s;
    d->txt = NULL;
    if (s->present) {
        d->txt = strdup(s->txt);
        CHECK(d->txt, "concolic: OOM copying an inherited bound's spelling");
    }
}

/* ONE FREE FOR ONE ENTRY, because there are now two places that dispose of an array of them (the live head and
   a frozen segment) and a field added to the struct must not be freed in one of them only. */
static void cons_entry_free(Cons *e) {
    int i;
    free(e->key);
    free(e->val);
    for (i = 0; i < e->nexcl; i++) free(e->excl[i]);
    free(e->excl);
    if (e->bnd) { bound_side_free(&e->bnd->lo); bound_side_free(&e->bnd->hi); free(e->bnd); }
}
/* …and one measurement, for the same reason: the freeze's byte census reads exactly the fields the free above
   disposes of, so a field that grows the entry is counted where it is counted for every other. */
static long cons_entry_bytes(const Cons *e) {
    long n = (long)strlen(e->key) + 1 + (e->val ? (long)strlen(e->val) + 1 : 0);
    int i;
    n += (long)e->nexcl * (long)sizeof(char *);
    for (i = 0; i < e->nexcl; i++) n += (long)strlen(e->excl[i]) + 1;
    if (e->bnd) {
        n += (long)sizeof(Bound);
        if (e->bnd->lo.txt) n += (long)strlen(e->bnd->lo.txt) + 1;
        if (e->bnd->hi.txt) n += (long)strlen(e->bnd->hi.txt) + 1;
    }
    return n;
}

/* THE CONSTRAINT IS A MUTABLE HEAD OVER IMMUTABLE, REFCOUNTED, STRUCTURALLY-SHARED SEGMENTS — the third
   instance of the one primitive cow.c's `CowSeg` and dom_cow.c's `DomSeg` already are, and it is here for the
   same reason and in the same shape.
   IT USED TO BE A WHOLE-MAP DEEP COPY, PER FORK: one malloc for the array and two strdups per entry, taken by
   every sibling, so a flow that forks n times over a map it is also growing allocated O(n^2) bytes and held
   every one of them until the last sibling finished. THE SHAPE THAT REACHED IT is an unknown-length walk —
   `Array.from(state.items)` asks step_length_unknown one outcome fork per POSITION and each position adds one
   entry — and the run died in this file's own CHECK with the wasm heap gone, at the FIRST script.
   A frozen segment is never written again, so two flows that reach the same segment agree about everything
   from there down: a fork hands the sibling the parent's whole knowledge by taking ONE reference on the chain
   (O(1)), and what each flow LEARNS afterwards lives in its own head above it. That is the copy-on-write the
   deep copy was imitating, and it makes a fork cost what the arm has learned rather than what its whole history
   knows.
   A WRITE TO A KEY THE CHAIN ALREADY HOLDS COPIES THAT ONE ENTRY UP INTO THE HEAD (`cons_entry`), which is what
   keeps the shared segments immutable while a flow may still narrow a fact it inherited — never a write through
   into a segment a sibling is reading. */
/* `bytes` is what this segment costs, computed ONCE at the freeze and carried, because the strings it holds
   are strdup'd and re-walking them at every census would make a measurement O(chain). The cold tier reads it:
   a frozen segment is SHARED by every flow forked below it, so it is counted once for the whole frontier and
   never once per flow. */
typedef struct ConsSeg { Cons *e; int n; int *hash; int hash_cap; struct ConsSeg *base; int refcount; long bytes; } ConsSeg;
/* THE FROZEN CONSTRAINT CHAIN'S OWN CENSUS — the twin of cow.c's and dom_cow.c's, counted at the two points a
   segment's lifetime begins and ends so the pair cannot drift from the thing it counts. */
static long g_cons_seg_live, g_cons_seg_entries_live, g_cons_seg_bytes_live;
void concolic_chain_stats(long *segs, long *entries, long *bytes) {
    if (segs) *segs = g_cons_seg_live;
    if (entries) *entries = g_cons_seg_entries_live;
    if (bytes) *bytes = g_cons_seg_bytes_live;
}

static Cons    *g_pins = NULL;  static int g_pins_n = 0, g_pins_cap = 0;   /* the running flow's HEAD */
static int     *g_pins_hash = NULL; static int g_pins_hash_cap = 0;        /* …and its index (key -> idx+1) */
static ConsSeg *g_pins_base = NULL;                                        /* the frozen chain under it */

static uint32_t cons_hash(const char *k) {   /* FNV-1a over the constraint key */
    uint32_t h = 2166136261u;
    while (*k) { h ^= (unsigned char)*k++; h *= 16777619u; }
    return h;
}
/* THE ONE PROBE, over the head's index or a segment's — a segment's index IS the head's, handed over unchanged
   at the freeze, so there is one implementation and the two cannot drift. Returns the entry index or -1. */
static int cons_index_find(const Cons *e, const int *hash, int cap, const char *key) {
    uint32_t m, h;
    if (!hash) return -1;
    m = (uint32_t)cap - 1; h = cons_hash(key) & m;
    while (hash[h]) {
        if (!strcmp(e[hash[h] - 1].key, key)) return hash[h] - 1;
        h = (h + 1) & m;
    }
    return -1;
}
static void cons_hash_put(int idx) {   /* insert head entry idx; caller guarantees room */
    uint32_t m = (uint32_t)g_pins_hash_cap - 1, h = cons_hash(g_pins[idx].key) & m;
    while (g_pins_hash[h]) h = (h + 1) & m;
    g_pins_hash[h] = idx + 1;
}
static void cons_hash_rebuild(void) {   /* size to >= 2*n (power of two), re-insert every head entry */
    int i, nc = 16;
    int *nh;
    /* SIZED IN A LOCAL, PUBLISHED AFTER — the allocation can sell a flow (solver/reclaim.h), and a `cap`
       advertising the new size over the old table is an out-of-bounds read for anything that looks a pin up
       while the sale runs. */
    while (nc < g_pins_n * 2) nc *= 2;
    nh = reclaim_realloc(g_pins_hash, (size_t)nc * sizeof(int));
    CHECK(nh, "concolic: OOM path-constraint index");
    g_pins_hash = nh; g_pins_hash_cap = nc;
    memset(g_pins_hash, 0, (size_t)g_pins_hash_cap * sizeof(int));
    for (i = 0; i < g_pins_n; i++) cons_hash_put(i);
}
/* THE WHOLE CONSTRAINT THIS FLOW CAN SEE, nearest-first: the head, then each frozen segment from the newest
   down. Nearest-first is the copy-on-write rule read the other way round — a head entry SHADOWS the segment
   entry it was copied up from, so a fact this arm has narrowed is the one that answers. */
static const Cons *cons_lookup(const char *key) {
    ConsSeg *s;
    int i = cons_index_find(g_pins, g_pins_hash, g_pins_hash_cap, key);
    if (i >= 0) return &g_pins[i];
    for (s = g_pins_base; s; s = s->base) {
        i = cons_index_find(s->e, s->hash, s->hash_cap, key);
        if (i >= 0) return &s->e[i];
    }
    return NULL;
}
/* THE WRITABLE entry for `key`: the head's, copying the chain's fact up into the head the first time this flow
   narrows one it inherited. */
static Cons *cons_entry(const char *key) {
    const Cons *below;
    int i = cons_index_find(g_pins, g_pins_hash, g_pins_hash_cap, key);
    if (i >= 0) return &g_pins[i];
    below = cons_lookup(key);   /* the head misses, so this is the chain's entry or nothing */
    if (g_pins_n >= g_pins_cap) {
        int nc = g_pins_cap ? g_pins_cap * 2 : 8;
        Cons *np = reclaim_realloc(g_pins, (size_t)nc * sizeof(Cons));
        CHECK(np, "concolic: OOM path constraint");
        g_pins = np; g_pins_cap = nc;   /* published after the ask — see cons_hash_rebuild */
    }
    g_pins[g_pins_n].key = strdup(key); CHECK(g_pins[g_pins_n].key, "concolic: OOM constraint key");
    g_pins[g_pins_n].val = (below && below->val) ? strdup(below->val) : NULL;
    CHECK(!(below && below->val) || g_pins[g_pins_n].val, "concolic: OOM copying an inherited pin value");
    /* THE EXCLUSIONS COPY UP WITH THE REST OF THE ENTRY. They are a fact this flow proved, so a copy-up that
       dropped them would make the very same path report an unconstrained parameter the moment a context
       switch happened to fall between the gate and the request that carries the value. */
    g_pins[g_pins_n].excl = NULL;
    g_pins[g_pins_n].nexcl = 0;
    if (below && below->nexcl) {
        int k;
        g_pins[g_pins_n].excl = malloc((size_t)below->nexcl * sizeof(char *));
        CHECK(g_pins[g_pins_n].excl, "concolic: OOM copying an inherited exclusion set");
        for (k = 0; k < below->nexcl; k++) {
            g_pins[g_pins_n].excl[k] = strdup(below->excl[k]);
            CHECK(g_pins[g_pins_n].excl[k], "concolic: OOM copying an inherited excluded value");
        }
        g_pins[g_pins_n].nexcl = below->nexcl;
    }
    /* …AND SO DOES THE INTERVAL, for exactly the reason above. A flow that observed `x > 5`, was preempted,
       and resumed to build its request would otherwise report an unbounded parameter — the fact lost to a
       context switch rather than to anything the program did. */
    g_pins[g_pins_n].bnd = NULL;
    if (below && below->bnd) {
        Bound *nb = malloc(sizeof *nb);
        CHECK(nb, "concolic: OOM copying an inherited bound");
        bound_side_copy(&nb->lo, &below->bnd->lo);
        bound_side_copy(&nb->hi, &below->bnd->hi);
        g_pins[g_pins_n].bnd = nb;
    }
    g_pins[g_pins_n].truth = below ? below->truth : -1;
    /* INHERITED WITH THE REST OF THE ENTRY. A principal demand this flow made before its last freeze is still
       this flow's demand, and a copy-up that dropped it would make the very same path answer "unpinned" the
       moment a context switch happened to fall between the pin and the sink. */
    g_pins[g_pins_n].pinned_root = below ? below->pinned_root : 0;
    g_pins_n++;
    if (!g_pins_hash || g_pins_hash_cap < g_pins_n * 2) cons_hash_rebuild();
    else cons_hash_put(g_pins_n - 1);
    DCHECK(cons_index_find(g_pins, g_pins_hash, g_pins_hash_cap, key) == g_pins_n - 1,
           "a constraint entry is not findable through the index that was just given it — a later read of the "
           "same fact would fork a branch this flow has already decided");
    return &g_pins[g_pins_n - 1];
}
/* THE PIN, AND THE MARK BESIDE IT — see concolic.h. The two writes are one act and are made together here
   because nothing downstream can recover the second: by the time a value reaches a sink the identity that was
   pinned is long gone, and only the ROOT survives every derivation.
   THE ROOT IS WRITTEN EVEN WHEN IT EQUALS THE IDENTITY, which is the common case (`event.origin === X`). Two
   entries then hold one key and the second write is a no-op on the first, which is what `cons_entry` already
   does for every repeated narrowing — writing the mark only for a DERIVED pin would make the direct spelling
   the one case the principal rule missed, and it is the spelling every real bundle writes. */
void concolic_pin(const char *src, const char *root, const char *val) {
    Cons *c = cons_entry(src);
    free(c->val); c->val = strdup(val); CHECK(c->val, "concolic: OOM pin value");
    /* A CONCOLIC CARRIES A PROVENANCE AND A ROOT TOGETHER (concolic_alloc asserts it), so a pin taken off one
       and missing the other is a value minted somewhere that does not go through that mint — and what it costs
       is silent: the principal rule below would answer "unpinned" for a flow that demanded an origin, and the
       PoC it then emits is one no cross-document attacker can deliver. */
    DCHECK(root != NULL, "an equality pinned an attacker value that carries no delivery ROOT — the root is "
                         "what says WHOSE bytes were demanded, and §Attacker-sources' unforgeable-origin rule "
                         "is decided by exactly that");
    /* `c` IS DEAD FROM HERE. cons_entry may grow the head, and the growth is a realloc — so the second entry
       is taken only after the first has been written, and nothing below may reach back through `c`. */
    if (root) cons_entry(root)->pinned_root = 1;
}
/* THE HOLE KEY — see concolic.h. One speller, because it is read at two ends that would otherwise normalise
   the same hole differently and never meet. */
char *concolic_hole_key(const char *shape) {
    char *r;
    size_t i, n = 0;

    if (!shape || !strchr(shape, '{')) return NULL;
    r = malloc(strlen(shape) + 1);
    CHECK(r, "concolic: OOM naming the hole a domain is a fact about");
    for (i = 0; shape[i]; i++) if (shape[i] != '{' && shape[i] != '}') r[n++] = shape[i];
    r[n] = 0;
    if (!n) { free(r); return NULL; }   /* `{}` — the value this engine cannot name; it gets no domain either */
    return r;
}

/* THE NEGATIVE HALF OF THE EQUALITY OBSERVATION — see concolic.h. The token is the concrete side of a
   predicate the PAGE wrote, so nothing here is chosen: this records that the flow took the arm on which the
   value is not that token, which is as much an observation as the pin on the other arm is. */
/* THE CONSTRAINT KEY AN EXCLUSION SET LIVES UNDER. It is COMPOSED and not the raw hole, because the one map
   already holds two other kinds of key — a pin's raw `src` and a branch's composed `branch` identity — and a
   raw shape sharing a namespace with a raw source name is a collision waiting for the first page whose member
   path spells a declared source. Composition is this file's own encoding (length-prefixed fields under a tag),
   so an `excl` key can never equal a `branch` key or a bare `src`. Caller frees. */
static char *excl_key(const char *hole) {
    const char *f[1];
    f[0] = hole;
    return concolic_ident_compose("excl", f, 1);
}

void concolic_exclude(const char *hole, const char *tok) {
    Cons *c;
    char **a;
    char *key;
    int i;

    DCHECK(hole && *hole,
           "an equality's false arm was recorded against no HOLE — the subject is what the emission looks the "
           "domain up by, so a nameless one is a constraint stored where nothing can read it and a parameter "
           "that renders as unconstrained while this flow has proved otherwise");
    DCHECK(tok != NULL,
           "an equality's false arm named no TOKEN to exclude — the token is the concrete side the page's own "
           "predicate wrote, and without it there is no fact, only the knowledge that some fact existed");
    key = excl_key(hole);
    CHECK(key, "concolic: the exclusion key could not be composed — a hole is a real string, so the only way "
               "this fails is allocation, and a lost constraint reports an unconstrained parameter");
    c = cons_entry(key);
    free(key);
    for (i = 0; i < c->nexcl; i++) if (!strcmp(c->excl[i], tok)) return;   /* the same gate, tested again */
    a = realloc(c->excl, (size_t)(c->nexcl + 1) * sizeof(char *));
    CHECK(a, "concolic: OOM recording a value this flow proved its input is not");
    c->excl = a;
    c->excl[c->nexcl] = strdup(tok);
    CHECK(c->excl[c->nexcl], "concolic: OOM copying a value this flow proved its input is not");
    c->nexcl++;
}

const char *const *concolic_excluded(const char *hole, int *n) {
    const Cons *c = NULL;

    DCHECK(n != NULL, "the exclusion set was asked for with nowhere to put its SIZE — a borrowed array with no "
                      "count is an array the caller has to guess the end of");
    if (hole) {
        char *key = excl_key(hole);
        CHECK(key, "concolic: the exclusion key could not be composed at the read");
        c = cons_lookup(key);
        free(key);
    }
    if (!c || !c->nexcl) { *n = 0; return NULL; }
    *n = c->nexcl;
    return (const char *const *)c->excl;
}

/* THE CONSTRAINT KEY AN INTERVAL LIVES UNDER — composed under its own tag for the reason excl_key states, and
   a SEPARATE tag from that one because the two are different claims about one hole: `≠ "admin"` and `> 5` are
   both domain facts, and filing them together would make a consumer that reads one see the other's shape.
   Caller frees. */
static char *bnd_key(const char *hole) {
    const char *f[1];
    f[0] = hole;
    return concolic_ident_compose("bound", f, 1);
}

/* ONE SIDE NARROWED. Within one flow the observations CONJOIN — `if (x > 5 && x < 100)` is two facts about one
   parameter, and each side keeps the TIGHTER of what it held and what just arrived. On a tie between an
   inclusive and an exclusive bound at the same number the exclusive one wins, because `x > 5` is strictly more
   than `x >= 5` says and a flow that observed both stands on both. */
static void bound_side_narrow(BoundSide *s, int is_lower, double num, int inclusive, const char *txt) {
    if (s->present) {
        int tighter = is_lower ? (num > s->num) : (num < s->num);
        if (!tighter) {
            if (num != s->num) return;                     /* the looser fact adds nothing */
            if (inclusive || !s->inclusive) return;        /* same number: only exclusive-over-inclusive wins */
        }
        bound_side_free(s);
    }
    s->num = num;
    s->inclusive = (signed char)(inclusive ? 1 : 0);
    s->txt = strdup(txt);
    CHECK(s->txt, "concolic: OOM copying the spelling of a bound a flow observed");
    s->present = 1;
}

void concolic_bound(const char *hole, RelOp rel, double num, const char *txt) {
    Cons *c;
    char *key;

    DCHECK(hole && *hole,
           "an ordering gate was recorded against no HOLE — the subject is what the emission looks a domain "
           "up by, so a nameless one is a constraint stored where nothing can read it and a parameter that "
           "renders as unbounded while this flow has proved otherwise");
    DCHECK(rel != REL_NONE,
           "an ordering gate was recorded with no RELATION — `x < 5` and `x > 5` are the two answers this "
           "field distinguishes, so a bound with neither is a number filed under a claim nobody made");
    DCHECK(txt && *txt,
           "an ordering gate was recorded with no SPELLING for its bound — the text is the page's own literal "
           "and the only thing a report may print, because re-spelling the double here would state a number "
           "the source file does not contain");
    key = bnd_key(hole);
    CHECK(key, "concolic: the bound key could not be composed — a hole is a real string, so the only way this "
               "fails is allocation, and a lost constraint reports an unbounded parameter");
    c = cons_entry(key);
    free(key);
    if (!c->bnd) {
        c->bnd = calloc(1, sizeof *c->bnd);
        CHECK(c->bnd, "concolic: OOM recording the interval a flow narrowed its input to");
    }
    if (rel == REL_GT || rel == REL_GE)
        bound_side_narrow(&c->bnd->lo, 1, num, rel == REL_GE, txt);
    else
        bound_side_narrow(&c->bnd->hi, 0, num, rel == REL_LE, txt);
}

int concolic_bound_read(const char *hole, ConcolicBound *out) {
    const Cons *c = NULL;

    DCHECK(out != NULL, "the interval was asked for with nowhere to put it — a bound returned only as a "
                        "yes/no is a constraint the caller cannot state");
    out->has_lo = out->has_hi = 0;
    out->lo_incl = out->hi_incl = 0;
    out->lo = out->hi = 0;
    out->lo_txt = out->hi_txt = NULL;
    if (hole) {
        char *key = bnd_key(hole);
        CHECK(key, "concolic: the bound key could not be composed at the read");
        c = cons_lookup(key);
        free(key);
    }
    if (!c || !c->bnd) return 0;
    if (c->bnd->lo.present) {
        out->has_lo = 1; out->lo = c->bnd->lo.num;
        out->lo_incl = c->bnd->lo.inclusive; out->lo_txt = c->bnd->lo.txt;
    }
    if (c->bnd->hi.present) {
        out->has_hi = 1; out->hi = c->bnd->hi.num;
        out->hi_incl = c->bnd->hi.inclusive; out->hi_txt = c->bnd->hi.txt;
    }
    DCHECK(out->has_lo || out->has_hi,
           "a hole carries a bound record with neither side present — concolic_bound allocates the record "
           "only as it writes a side, so an empty one is a narrowing that was allocated and then not made");
    return 1;
}

void concolic_constrain_branch(const char *key, int truth) {
    cons_entry(key)->truth = (signed char)(truth ? 1 : 0);
}
int concolic_branch_decided(const char *key) {
    const Cons *c = cons_lookup(key);
    return c ? c->truth : -1;
}

/* Drop a chain reference: refcount--, free the segment's entries at zero, continue into its base. A loop, not
   recursion — the chain's depth is the fork depth, and an unknown-length walk makes that as deep as the walk is
   long; C stack cannot be parked. The twin of cow_seg_unref / dom_seg_unref. */
static void cons_seg_unref(ConsSeg *s) {
    while (s && --s->refcount <= 0) {
        ConsSeg *base = s->base;
        int i;
        g_cons_seg_live--; g_cons_seg_entries_live -= s->n; g_cons_seg_bytes_live -= s->bytes;
        for (i = 0; i < s->n; i++) cons_entry_free(&s->e[i]);
        free(s->e); free(s->hash); free(s);
        s = base;
    }
}

void concolic_clear_pins(void) {
    int i;
    for (i = 0; i < g_pins_n; i++) cons_entry_free(&g_pins[i]);
    free(g_pins); g_pins = NULL; g_pins_n = g_pins_cap = 0;
    free(g_pins_hash); g_pins_hash = NULL; g_pins_hash_cap = 0;
    cons_seg_unref(g_pins_base); g_pins_base = NULL;
}

/* Per-flow constraint state is swappable so interleaved flows keep their OWN narrowing: suspend FREEZES the
   live head onto the chain and hands back a reference to it, resume installs a parked chain as the live one.
   A blob is one flow's constraint parked while another runs — and the blob a FORK takes is the sibling's whole
   starting knowledge, which is the same object either way and is why one function serves both. */
typedef struct { ConsSeg *seg; } PinBlob;
void *concolic_pins_suspend(void) {
    PinBlob *b = reclaim_malloc(sizeof *b);
    CHECK(b, "concolic: the path constraint could not be parked — the frontier never drops a work item, and a "
             "flow whose constraint is lost would re-fork every branch it has already decided");
    if (g_pins_n == 0) {
        /* NOTHING LEARNED SINCE THE LAST FREEZE, so there is nothing to freeze: the blob is one more reference
           on the chain the flow already stands on. Without this a park/resume pair with no writes between them
           would push an empty segment per switch and the chain's depth would count SWITCHES rather than forks. */
        b->seg = g_pins_base;
        if (b->seg) b->seg->refcount++;
        return b;
    }
    DCHECK(g_pins_hash != NULL, "a non-empty constraint head is being frozen with no index — every read of the "
                                "frozen segment would miss and the fact would be re-forked");
    {
        ConsSeg *s = reclaim_malloc(sizeof *s);
        CHECK(s, "concolic: OOM freezing the path constraint into a shared segment");
        s->e = g_pins; s->n = g_pins_n; s->hash = g_pins_hash; s->hash_cap = g_pins_hash_cap;
        s->base = g_pins_base; s->refcount = 2;   /* the running flow + this blob */
        {   /* what this segment costs, measured where it is frozen — see the struct */
            int k;
            s->bytes = (long)sizeof *s + (long)s->n * (long)sizeof(Cons)
                     + (long)s->hash_cap * (long)sizeof(int);
            for (k = 0; k < s->n; k++)
                s->bytes += cons_entry_bytes(&s->e[k]);
            g_cons_seg_live++; g_cons_seg_entries_live += s->n; g_cons_seg_bytes_live += s->bytes;
        }
        g_pins = NULL; g_pins_n = g_pins_cap = 0;
        g_pins_hash = NULL; g_pins_hash_cap = 0;
        g_pins_base = s;                          /* the running flow continues on top of what it just froze */
        b->seg = s;
    }
    return b;
}
void concolic_pins_resume(void *blob) {
    PinBlob *b = blob;
    DCHECK(b != NULL, "a flow was resumed with no parked path constraint — it would re-fork every branch it "
                      "had already decided, on a decision vector that replays the old answers");
    concolic_clear_pins();                 /* the live head and chain belong to the flow that just parked */
    g_pins_base = b->seg;
    if (g_pins_base) g_pins_base->refcount++;   /* the blob keeps its own reference until it is freed */
}
/* A CONSTRAINT THAT HAS LEARNED NOTHING YET, for a flow that RESUMES without ever having run in this session —
   the cold tier's. Such a flow is not fresh (it stands on a recorded decision chain, so the scheduler resumes it
   rather than entering it) and it holds no knowledge (its facts were narrowed from values this session's heap
   does not contain, and it re-derives every one of them as it replays the gates that produced them). The resume
   path asserts that a blob exists, and it is right to: a flow resumed with none would re-fork every branch it
   had already decided. This is the honest way to satisfy it — an empty chain, not a NULL the assert is taught
   to tolerate. */
void *concolic_pins_blob_empty(void) {
    PinBlob *b = reclaim_malloc(sizeof *b);
    CHECK(b, "concolic: a resumed flow's empty path constraint could not be allocated");
    b->seg = NULL;
    return b;
}

void concolic_pins_blob_free(void *blob) {
    PinBlob *b = blob; if (!b) return;
    cons_seg_unref(b->seg);
    free(b);
}
static const char *pin_of(const char *src) {
    const Cons *c = cons_lookup(src);
    return c ? c->val : NULL;
}

static JSClassID g_concolic_class = 0;   /* runtime-allocated; 0 until concolic_init */

static void concolic_finalizer(JSRuntime *rt, JSValueConst val) {
    Concolic *c = JS_GetOpaque(val, g_concolic_class);
    if (!c) return;
    free(c->shape);
    free(c->src);
    free(c->root);
    free(c->ident);
    free(c->cmp_tok);
    free(c->cmp_subj);
    free(c->rel_tok);
    free(c->rel_subj);
    JS_FreeValueRT(rt, c->example);
    free(c);
}

static void concolic_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func) {
    Concolic *c = JS_GetOpaque(val, g_concolic_class);
    if (c && !JS_IsUndefined(c->example)) JS_MarkValue(rt, c->example, mark_func);
}

/* @S CANDIDATE injection: during a verification re-run, the attacker source identified by g_cand_src returns
   the concrete breakout payload instead of a concolic, so the REAL code builds the exploit and it fires. */
static char *g_cand_src = NULL, *g_cand_payload = NULL;

/* THE DECLARED SOURCES and what their component does to an attacker's bytes. A source that declares nothing
   delivers as-is, which is right for injected server state (`window.__STATE`) — the attacker writes that JSON
   directly and no component transforms it.
   AND EVERY ROW CARRIES ITS CLAIMANT. The array is this component's storage and each row is another
   component's agent state (core/platform.h's fourth paragraph), so the row has to say WHOSE — that is what
   `concolic_undeclare_sources` is keyed by, what the duplicate assert reports the existing owner from, and
   what concolic_free names when a release did not finish. The pointer is the caller's static component name,
   held the way core/agent_state.h holds one and for the same reason: it outlives the agent it names. */
/* `principal` IS A SECOND COLUMN AND NOT A SECOND REGISTRY, because it is a fact about the SAME row: whether
   the attacker WRITES this value or merely OWNS it (concolic.h states the pair, and HTML §9.3.2.2 "User
   agents" states why one exists at all). Keeping it here is also what makes the give-back complete without a
   second give-back — the claimant releases its rows and the property goes with them. */
typedef struct { const char *component; char *src; char *encode; char prefix; SourceDeliverKind deliver;
                 int principal; }
        SourceDelivery;
static SourceDelivery *g_srcs;
static int g_srcs_n, g_srcs_cap;

/* THE JOINT DOMAIN'S SEPARATOR — see concolic.h for what reaches it, why the identity is the SET and not an
   expression over it, and why a narrowing of the joint narrows no member.
   THE SEPARATOR IS PART OF THE IDENTITY, so a member that contained it would let two different sets compose to
   one key — the truncation defect in a different costume, asserted at the mint rather than trusted.
   IT IS DECLARED HERE, ABOVE THE REGISTRY, BECAUSE THE REGISTRY IS ITS SECOND READER. It used to sit beside
   the mint three hundred lines below, which was right while the only thing that ever looked at a joint
   identity was the code that composed one. */
#define CONCOLIC_JOINT_SEP " & "

/* MEMBER `i` OF A ROOT, AS A SLICE OF IT — the walk that makes "a provenance can name a SET" a fact the
   registry can act on rather than a string it fails to match.
   A root is either one source's name or several joined by the separator above, and the mint asserts that no
   member contains that separator — which is exactly what makes this walk EXACT rather than a guess. `*p`/`*n`
   point into `root` and are never copied: the registry compares them against declared names and nothing here
   outlives the call. Answers 0 past the last member, so a caller LOOPS and never first asks whether the root
   it holds is joint — there is no second spelling for the single-source case, which answers at i == 0 and
   stops. */
static int root_member(const char *root, int i, const char **p, size_t *n)
{
    size_t seplen = strlen(CONCOLIC_JOINT_SEP);
    const char *at = root, *s;

    if (!root) return 0;
    for (; i > 0; i--) {
        if (!(s = strstr(at, CONCOLIC_JOINT_SEP))) return 0;
        at = s + seplen;
    }
    s = strstr(at, CONCOLIC_JOINT_SEP);
    *p = at;
    *n = s ? (size_t)(s - at) : strlen(at);
    return 1;
}

/* THE DECLARED ROW FOR ONE MEMBER, matched over the slice rather than a copy of it. The terminator test is
   half the comparison: `strncmp` alone would match `location.hash` against a declared `location.hashish`. */
static int src_row(const char *p, size_t n)
{
    int i;

    for (i = 0; i < g_srcs_n; i++)
        if (!strncmp(g_srcs[i].src, p, n) && g_srcs[i].src[n] == '\0') return i;
    return -1;
}

/* WHICH DECLARED SOURCE A ROOT NAMES, over a root that may name SEVERAL — the one walk both registry readers
 * below share, so the rule that decides an ambiguous root is stated once instead of twice.
 *
 * WHY THIS EXISTS AT ALL. Both readers were an exact strcmp over the whole root, so a JOINT root matched no row
 * and they answered 0 / NULL — and 0 / NULL is not "I could not tell", it is the POSITIVE statement "this
 * source declares no delivery", which the report renders as "no component carries these bytes to the victim".
 * That is the defaulted-field defect standing at the exact place a wrong answer becomes a wrong PoC, and it
 * would have arrived the moment anything composed a provenance out of two attacker sources.
 *
 * A JOINT WITH ONE DECLARING MEMBER HAS AN ANSWER, AND IT IS THAT MEMBER'S. A value that is a joint function of
 * `location.search` and the viewport width carries attacker bytes through the query and nothing at all through
 * the viewport: the delivery is the query's, the encode set is the query's, and there is no second constraint
 * to state. That case is the ordinary one, because most joints in this engine are over ENVIRONMENT facts that
 * declare nothing.
 *
 * TWO DECLARING MEMBERS IS THE ONE WITH NO SINGLE HONEST ANSWER, so it crashes. Each declares its own
 * percent-encode set and its own address component (`#` against `?` — the two sets deliberately differ, which
 * is the whole reason those are two sources), so answering with either states one source's constraint as the
 * whole constraint on bytes that came from both, and the envelope then tells a researcher to deliver through
 * the component the payload did not ride. §@S: a shape carrying one of its two facts is a WRONG report rather
 * than a partial one. Build the per-member emission — one candidate seeded per declaring member, each with its
 * own envelope, the one that FIRES emitted — rather than a rule for picking. */
static int root_declared_row(const char *root)
{
    const char *p;
    size_t n;
    int i, row, found = -1;

    for (i = 0; root_member(root, i, &p, &n); i++) {
        if ((row = src_row(p, n)) < 0) continue;
        DCHECK(found < 0,
               "a value's delivery ROOT names TWO declared attacker sources, and a report can state one "
               "percent-encode set and one address component. Answering with either would present one "
               "source's constraint as the whole constraint on bytes that arrived through both — the "
               "envelope would name the component the payload did not ride. Seed one candidate per declaring "
               "member, each carrying its own envelope, and emit the one that fires");
        found = row;
    }
    return found;
}

/* THE TOKEN A REPORT CARRIES for a mechanism, spelled once so the engine owns the whole source vocabulary —
   the declaration side is the enum, the emission side is this, and a delivery layer switches on what comes out
   of here. A mechanism added to the enum without a token would cross as nothing at all, so it says so. */
static const char *deliver_token(SourceDeliverKind k)
{
    switch (k) {
    case SRC_DELIVER_ADDRESS:           return "address";
    case SRC_DELIVER_PLANT:             return "plant";
    case SRC_DELIVER_REFERRING_ADDRESS: return "referring-address";
    case SRC_DELIVER_USER_FILE:         return "user-file";
    case SRC_DELIVER_CROSS_DOCUMENT_MESSAGE: return "cross-document-message";
    }
    DFAIL("a source declared a delivery mechanism with no report token — the token is what the reproduction "
          "envelope states and what the delivery layer switches on, so the mechanism would cross as nothing");
    return NULL;
}

void concolic_declare_source(const char *component, const char *src, const char *encode, char prefix,
                             SourceDeliverKind deliver)
{
    int i;
    CHECK(src != NULL, "a source was declared with no identity");
    DCHECK(component != NULL && *component,
           "an attacker source was declared by no component — the row is a CLAIM whose claimant gives it back "
           "at its own release, so a row with no owner is a row nobody can release and a registry whose "
           "emptiness asserts nothing about anybody");
    /* THE TWO HALVES OF ONE DECLARATION MUST AGREE. A value carried in the victim's own address has the
       component it is carried at, and a value carried by any other mechanism has none — so a `#` beside a
       `plant`, or an `address` with no component, is a declaration whose reproduction cannot be built from it
       and whose candidate delivery would prepend a character no browser puts there. */
    DCHECK((deliver == SRC_DELIVER_ADDRESS) == (prefix != 0),
           "a source's delivery mechanism and its address component disagree — only a value carried in the "
           "victim's own address has a component, and it always has one");
    for (i = 0; i < g_srcs_n; i++)
        if (!strcmp(g_srcs[i].src, src)) {
            /* WHO ALREADY OWNS IT IS THE HALF THAT SAYS WHAT WENT WRONG. The same component declaring twice is
               a declaration that ran twice or a claimant whose rows are dynamic and did not ask first; a
               DIFFERENT component is two claimants over one source, and then the give-back of either takes a
               row it does not own. The old message could not tell those apart. */
            DFAILF("a source declared its browser delivery twice — `%s` is already declared by %s, and one "
                   "component owns one source in BOTH directions. A second declaration by that same "
                   "component is a claimant with dynamic rows that did not ask concolic_source_declared_by "
                   "first; a declaration by any other is two claimants over one row, whichever of them "
                   "releases first taking a row it does not own", src, g_srcs[i].component);
            return;
        }
    if (g_srcs_n == g_srcs_cap) {
        int c = g_srcs_cap ? g_srcs_cap * 2 : 8;
        SourceDelivery *a = reclaim_realloc(g_srcs, (size_t)c * sizeof *a);
        CHECK(a != NULL, "concolic: OOM declaring a source's delivery");
        g_srcs = a; g_srcs_cap = c;
    }
    g_srcs[g_srcs_n].component = component;
    g_srcs[g_srcs_n].src = strdup(src);
    g_srcs[g_srcs_n].encode = strdup(encode ? encode : "");
    g_srcs[g_srcs_n].prefix = prefix;
    g_srcs[g_srcs_n].deliver = deliver;
    /* A SOURCE THE ATTACKER WRITES, WHICH IS WHAT NEARLY EVERY SOURCE IS. The exception says so with its own
       call (concolic_declare_source_principal) rather than a sixth argument every declaration would have to
       spell — and it is written here, at the mint, so the column can never be the uninitialized garbage a
       later reader would read as "principal". */
    g_srcs[g_srcs_n].principal = 0;
    CHECK(g_srcs[g_srcs_n].src && g_srcs[g_srcs_n].encode, "concolic: OOM declaring a source's delivery");
    g_srcs_n++;
}

/* THE CLAIM, GIVEN BACK BY THE COMPONENT THAT MADE IT — core/platform.h's fourth paragraph, for the one
 * registry in the solver half that a browser component writes into. A declared row is agent state whose
 * STORAGE is this file's array and whose CLAIM is the declaring component's, so the claimant removes its own
 * rows at its own release and concolic_free asserts, at this component's, that none are left.
 *
 * KEYED BY THE CLAIMANT AND NOT BY THE SOURCE, and the difference is not spelling. A release naming each
 * source reads well where the rows are FIXED and fails where they are not: core/file/file_system.c declares
 * `file:NAME` per file the device has held, so it would have to keep a parallel list of the names it declared
 * — a second copy of this array, with its own growth, its own OOM edge and its own dedup, which must agree
 * with this one and has nothing to make it. That is the hand-copied-list defect core/platform.h and
 * core/realm.h exist to abolish, in miniature, and the next claimant with dynamic rows writes it again. The
 * owner is on the row instead: one list, and the give-back reads it.
 *
 * IT IS NOT A BULK RESET, WHICH IS WHAT KEEPS THE HOLDER'S ASSERT FALSIFIABLE. A call that emptied the array
 * would be a release undoing a declaration it did not make — the shape core/platform.c's own table check
 * forbids one column up, and the shape three of core/dom/document.c's sub-components were in until b87eef36
 * moved them back to their declarer — and "every claimant gave its rows back" and "one claimant forgot and the
 * wipe covered it" would then be the same state. Removing exactly the caller's rows is what makes
 * `g_srcs_n == 0` a fact about the CLAIMANTS, and what lets the assert name the one that did not finish.
 *
 * ZERO ROWS IS A LEGITIMATE ANSWER and is deliberately not asserted against: a component whose rows are
 * dynamic declares as many as the agent gave it, and none is one of those numbers. What a claimant that
 * declared and did not release costs is the assert at concolic_free, which is where it is owed. */
void concolic_undeclare_sources(const char *component)
{
    int i = 0;

    DCHECK(component != NULL && *component,
           "the source registry was asked to give back the claims of no component — the claim is keyed by its "
           "claimant, and a release with no name would either take every row or none");
    while (i < g_srcs_n) {
        if (strcmp(g_srcs[i].component, component) != 0) { i++; continue; }
        free(g_srcs[i].src);
        free(g_srcs[i].encode);
        /* THE ORDER OF THE REST IS PRESERVED rather than closed with a swap. Every read of this array is a
           lookup by identity, so order decides no answer — but it decides the order a WALK would report, and a
           registry that reshuffles itself when an unrelated component releases is a registry whose contents
           depend on teardown. Declaration order is a fact about the browser; nothing here may make it a fact
           about the sequence of frees. */
        memmove(&g_srcs[i], &g_srcs[i + 1], (size_t)(g_srcs_n - i - 1) * sizeof *g_srcs);
        g_srcs_n--;
    }
}

int concolic_source_declared_by(const char *component, const char *src)
{
    int i;

    DCHECK(component != NULL && *component && src != NULL,
           "the source registry was asked whether a component owns a row, with no component or no source — "
           "the answer decides whether a claimant declares a second time, so a half-formed question would "
           "either drop a source or trip the duplicate assert");
    for (i = 0; i < g_srcs_n; i++)
        if (!strcmp(g_srcs[i].src, src)) {
            /* THE DUPLICATE ASSERT SEEN FROM THE OTHER END. A claimant asking about a source ANOTHER component
               declared is about to skip a declaration it owes, or — worse — to give back a row it does not
               own, and the delivery a report then builds is for a source that component does not answer for. */
            DCHECK(strcmp(g_srcs[i].component, component) == 0,
                   "a component asked whether it owns an attacker source that a DIFFERENT component declared "
                   "— one source is owned by one component in both directions, so this is either a source "
                   "identity two components spell the same way or a claimant reaching into another's rows");
            return 1;
        }
    return 0;
}

/* THE SECOND HALF OF A ROW — see concolic.h for what a principal IS and what it decides.
 * IT IS A SEPARATE CALL AND NOT A SIXTH ARGUMENT, for the reason core/platform.h gives about every other
 * optional property of a claim: a parameter every declaration must spell is a parameter five components would
 * spell `0` for, and a `0` written five times to describe a property four of them have never heard of is the
 * shape a reader has to decode rather than read. The call names the exception.
 * IT REFUSES A ROW THIS COMPONENT DOES NOT OWN, both directions, because the row is a CLAIM: marking somebody
 * else's would put a property on a source this component does not answer for, and marking one that does not
 * exist would be a claim whose claimant the give-back can never find. */
void concolic_declare_source_principal(const char *component, const char *src)
{
    int i;

    DCHECK(component != NULL && *component && src != NULL,
           "a source was declared a PRINCIPAL by no component, or with no identity — the property rides the "
           "row, and the row is a claim whose claimant releases it");
    for (i = 0; i < g_srcs_n; i++)
        if (!strcmp(g_srcs[i].src, src)) {
            DCHECK(strcmp(g_srcs[i].component, component) == 0,
                   "a component declared an attacker source a PRINCIPAL that a DIFFERENT component owns — one "
                   "source is owned by one component in both directions, and a property written onto another "
                   "claimant's row outlives every release this component makes");
            g_srcs[i].principal = 1;
            return;
        }
    DFAIL("a source was declared a PRINCIPAL before its delivery was declared. The property is a COLUMN of the "
          "delivery row, so there is nothing here to write it onto — and a registry that grew a row for it "
          "would hold a source whose browser delivery nothing states, which the reproduction envelope reads as "
          "\"no component carries these bytes to the victim\". Call concolic_declare_source first");
}

/* HAS THIS FLOW DEMANDED A PARTICULAR VALUE OF AN ATTACKER'S PRINCIPAL? — see concolic.h.
 * A WALK OF THE REGISTRY RATHER THAN A LOOKUP BY KEY, because the question is asked of the DECLARATIONS: the
 * flow does not know which sources are principals and must not be taught a name for one, and this array is the
 * one place that does know. It is O(declared principals) — the number of sources whose value the browser
 * stamps rather than the attacker writes — and each row costs one indexed probe of the constraint chain. */
int concolic_principal_pinned(void)
{
    int i;

    for (i = 0; i < g_srcs_n; i++) {
        const Cons *c;
        if (!g_srcs[i].principal) continue;
        c = cons_lookup(g_srcs[i].src);
        if (c && c->pinned_root) return 1;
    }
    return 0;
}

int concolic_source_delivery(const char *root, const char **kind, char *prefix)
{
    int row = root_declared_row(root);

    DCHECK(kind != NULL && prefix != NULL, "a source's declared delivery was asked for with nowhere to put it");
    if (row < 0) return 0;
    *kind = deliver_token(g_srcs[row].deliver);
    *prefix = g_srcs[row].prefix;
    return 1;
}

const char *concolic_source_encodes(const char *root)
{
    int row = root_declared_row(root);

    return row < 0 ? NULL : g_srcs[row].encode;
}

/* JSConcolicHooks.lead — the DOMAIN fact the delivery declaration already holds, read by a builtin that has to
   decide whether one of its completions is feasible at all. The prefix is what the component states its
   component's value carries (`#` for a fragment, `?` for a query), so this is not a guess about the value: it
   is the same declaration the candidate delivery is built from, asked the other way round.
   A DERIVED value (`location.hash.slice(1)`) has its own source identity and no declaration, so it answers 0
   and nothing is refined — which is right, since slicing is exactly what removes the prefix. */
static int concolic_lead_hook(JSValueConst v)
{
    const char *src = concolic_src_c(v);
    int i;
    if (!src) return 0;
    for (i = 0; i < g_srcs_n; i++)
        if (!strcmp(g_srcs[i].src, src)) return (unsigned char)g_srcs[i].prefix;
    return 0;
}

/* THE CANDIDATE AS THE PAGE READS IT. The solver's payload is what the ATTACKER puts in the URL; this is what
   the browser hands the page, which is the only thing re-execution can honestly decide a breakout against.
 *
 * THE TWO HALVES OF THE DECLARATION ARE ASKED WITH TWO DIFFERENT KEYS, because they are facts about two
 * different things and only one of them survives a derivation.
 *   The ENCODE SET is a fact about the ROOT: the browser percent-encoded the bytes on the way INTO the address,
 *   and `location.hash.slice(1)` does not decode them, so a candidate substituted at the slice's RESULT must
 *   still be the encoded form. Asked with `src` — an exact strcmp that a derived identity never matches — the
 *   whole set went missing for exactly the derivations real code is written in, and the payload was handed over
 *   RAW. That is not a reporting gap: an HTML-context breakout containing `<` fired in the model at
 *   `innerHTML = location.hash.slice(1)` and cannot fire in Chrome, which encodes `<` in a fragment. It is the
 *   precise false-PoC generator this declaration was introduced to end, arriving through the derivation door.
 *   The PREFIX is a fact about the INJECTION POINT: `#` is a character the fragment's own value carries, and
 *   slicing is exactly what removes it (concolic_lead_hook states the same thing from the other side). So it
 *   comes off `src`'s own row, which a derived identity correctly does not have.
 * WHEN `src` HAS A ROW AT ALL, IT IS THE ROOT'S ROW — a value carrying a declared source's own name as its
 * identity is one that inherited it unchanged, so it inherited the root unchanged too. Asserted rather than
 * assumed: the two facts are threaded from an operand each, and taking them from DIFFERENT operands would
 * encode for one source and prefix for another with nothing to say so. */
static JSValue concolic_deliver(JSContext *ctx, const char *src, const char *root, const char *payload)
{
    const SourceDelivery *at = NULL;   /* the INJECTION POINT's row, if it has one: the component's prefix */
    const char *encode;                /* …and the ROOT's percent-encode set, which every derivation inherits */
    const unsigned char *p;
    char *out;
    size_t o = 0, n;
    int i;

    if (!payload) payload = "";
    for (i = 0; i < g_srcs_n; i++)
        if (src && !strcmp(g_srcs[i].src, src)) { at = &g_srcs[i]; break; }
    encode = concolic_source_encodes(root);
    DCHECK(!at || (root && !strcmp(root, src)),
           "a candidate is being delivered at an injection point that names a DECLARED source while its "
           "delivery root names a different one — the two are threaded from an operand each, so this value "
           "took them from two, and the payload would be percent-encoded for one source and prefixed for "
           "the other");
    if (!at && !encode) return JS_NewString(ctx, payload);   /* nothing carries these bytes: delivered as itself */
    if (!encode) encode = "";

    n = strlen(payload);
    out = reclaim_malloc(n * 3 + 2);
    CHECK(out != NULL, "concolic: OOM delivering a candidate");
    if (at && at->prefix) out[o++] = at->prefix;
    for (p = (const unsigned char *)payload; *p; p++) {
        /* C0 controls and DEL are percent-encoded by every URL component, so they are not in any declared set. */
        if (*p < 0x20 || *p == 0x7F || strchr(encode, (char)*p)) {
            static const char HEX[] = "0123456789ABCDEF";
            out[o++] = '%'; out[o++] = HEX[*p >> 4]; out[o++] = HEX[*p & 15];
        } else {
            out[o++] = (char)*p;
        }
    }
    out[o] = 0;
    {
        JSValue r = JS_NewStringLen(ctx, out, o);
        free(out);
        return r;
    }
}
/* THE INSTALLED SUBSTITUTION BELONGS TO THE RUNNING FLOW — asserted here because this is where it is installed
   and the scheduler is the only caller. An installed candidate that is not the running flow's means an
   exploring flow is about to read an attacker payload where its concolic source belongs: it silently stops
   forking and stops being a detectable sink, which produces a smaller learned surface and no error at all.
   Two-sided on purpose — a candidate flow must have its own payload installed, and any other flow must have
   none — so neither direction of the mismatch can return. */
void concolic_set_candidate(const char *src, const char *payload) {
    free(g_cand_src); free(g_cand_payload);
    g_cand_src = src ? strdup(src) : NULL;
    g_cand_payload = payload ? strdup(payload) : NULL;
    {
        const Flow *f = flow_running();
        DCHECK(!!src == !!(f && f->cand_src),
               "the installed @S substitution does not match the running flow — an exploring flow would read "
               "the previous candidate's payload in place of its concolic source");
    }
}

/* THE MEMBER'S OWN EXAMPLE — the third of §Solver's triple, computed by RUNNING THE REAL READ on the parent's
   example rather than by deriving anything from a recorded expression.
 *
 * A concolic whose example is a RECORD is what the real codec hands back: 25.5.1 over an unknown text runs the
 * engine's own `JSON.parse` on the source's example and the completion carries that parsed value. Until this
 * existed, every field read off it minted example-free, so a loaded config's `region` — a value the run had
 * concretely in hand — reached an @H parameter as a hole with no value under it, which is §@H's "a shape
 * states two facts" with one of them thrown away. Absence stays absence: a member the record does NOT hold
 * yields no example, which is the honest statement that the payload this visitor was served does not carry
 * that field, and is exactly what keeps the gate over it forking instead of inventing a value for it.
 *
 * NOT [[Get]] — JS_GetOwnSlotDesc, which runs NONE of the page's code. Three reasons and each is load-bearing:
 * a prototype walk would answer `hasOwnProperty` and `toString` for members no server ever wrote; a getter is
 * the page's own function and calling it from this C activation is the drive-to-completion with no flow base
 * the whole flow machinery exists to avoid; and the question being asked is whether the RECORD holds the
 * member, which is [[GetOwnProperty]]'s question and not [[Get]]'s.
 *
 * AN ACCESSOR ON AN EXAMPLE IS ANSWERED WITH NO EXAMPLE, and that is a positive statement rather than a
 * default: its value is whatever the page's getter would compute, this read may not run it, so there is no
 * concrete value here to report — the same answer as a member the record does not hold, for a different and
 * equally honest reason. `JSON.parse` builds only data properties, so the only route to one is a 25.5.1 step 9
 * reviver that defined one, which is the page's own choice. */
static JSValue example_member(JSContext *ctx, JSValueConst parent_example, JSAtom atom)
{
    JSPropertyDescriptor pd;
    int has;

    /* A primitive example has no members to read: `{hash}.length` off a concrete fragment is a DERIVATION and
       not a slot, and answering it from this object-shaped read would be this file inventing the operation. */
    if (!JS_IsObject(parent_example))
        return JS_UNDEFINED;
    has = JS_GetOwnSlotDesc(ctx, &pd, parent_example, atom);
    DCHECK(has >= 0,
           "reading a member off a concolic's own example threw — JS_GetOwnSlotDesc runs none of the page's "
           "code and aborts on a Proxy rather than trapping, so the only completion it has is a value, and an "
           "exception raised here would be left on a context with no flow base to hand it to");
    if (has <= 0)
        return JS_UNDEFINED;
    if (pd.flags & JS_PROP_GETSET) {
        JS_FreeValue(ctx, pd.getter);
        JS_FreeValue(ctx, pd.setter);
        return JS_UNDEFINED;
    }
    return pd.value;
}

/* Exotic [[Get]]: reading ANY field of a concolic value yields a DERIVED concolic — unknown injected/attacker
   state is unknown per-field, carrying the FIELD-PATH identity ("{state}.admin"), which doubles as the source
   identity for @S injection. This is what lets a gated `if (state.admin)` fork AND lets an @S candidate inject
   at a precise source. The derived value carries the parent's example READ THROUGH, so a field the record
   holds arrives with the bytes the server sent and a field it does not hold arrives with none — opaque for
   control flow either way, which is what §solver's trust boundary requires of a loaded config: "a loaded
   `features.admin:false` must NOT concretize the gate, or the admin endpoint is lost". */
static JSValue concolic_exotic_get(JSContext *ctx, JSValueConst obj, JSAtom atom, JSValueConst receiver) {
    (void)receiver;
    Concolic *c = JS_GetOpaque(obj, g_concolic_class);
    const char *field, *pv, *f[2];
    char *shape, *ident;
    JSValue r;

    if (!c) return JS_UNDEFINED;
    field = JS_AtomToCString(ctx, atom);
    shape = shapef("%s.%s", c->shape ? c->shape : "{}", field ? field : "?");
    f[0] = c->ident; f[1] = field;                           /* a name that would not convert -> no identity */
    ident = concolic_ident_compose(".", f, 2);
    if (field) JS_FreeCString(ctx, field);
    if (g_cand_src && !strcmp(shape, g_cand_src)) {          /* candidate run: this source -> the concrete breakout */
        r = concolic_deliver(ctx, shape, c->root, g_cand_payload);
        free(shape); free(ident);
        return r;
    }
    pv = pin_of(shape);                                      /* an EQ gate pinned this source -> the real value */
    if (pv) {
        r = JS_NewString(ctx, pv);
        free(shape); free(ident);
        return r;
    }
    /* src = the field path (a precise @S injection point), root = the parent's, unchanged. A field of an
       unknown object is a datum the attacker controls SEPARATELY — which is why this mints a new injection
       identity at all — but it is not a datum that arrives by a different route: whatever carried the object's
       bytes in carried this field's, and a report has to say so. */
    r = concolic_alloc(ctx, shape, shape, c->root, ident, example_member(ctx, c->example, atom));
    free(shape);
    return r;
}

/* MINT A COMPARISON RESULT — the boolean `if (x === 'admin')` and `if (x < 700)` branch on.
 *
 * ITS IDENTITY IS THE OPERATOR AND BOTH OPERANDS, which is the whole of the predicate, so a comparison result
 * is an ordinary derived value as far as decide.c is concerned and that file needs no second key rule for it.
 * `ia`/`ib` are the operands' identities in the order the composition wants them and are CONSUMED; `eq_kind`
 * and `tok` are the PIN, which only an equality against a concrete side has (an ordering narrows a domain and
 * determines no value — §Solver-half: a range-gated parameter stays a domain-annotated shape). */
static JSValue pred_new(JSContext *ctx, const char *op, const char *src, const char *root, char *ia, char *ib,
                        int eq_kind, const char *tok, const char *subj)
{
    const char *f[3];
    char *ident;
    JSValue r;
    Concolic *c;

    DCHECK(op != NULL,
           "a comparison result was minted with no OPERATOR. The operator is half the predicate: without it "
           "`x < 700` and `x > 700` compose to one identity, the flow's record of either DECIDES the other, "
           "and the arm it deletes is one nothing contradicts");
    /* THE PIN IS THREE FIELDS. The operator says an arm determines something, the token says WHAT, and the
       subject says WHICH HOLE the report must state it under — decide.c pins on one arm and EXCLUDES on the
       other, and it reads all three at the same line.
       THE SUBJECT IS THE ONE OF THE THREE THAT MAY BE HONESTLY ABSENT, and its absence is a POSITIVE
       statement rather than a hole: a value whose shape is the unnameable `{}` has no hole the @H surface
       will print (endpoint.c's path scan mints no param for one either), so there is no name to file a domain
       under. The pin is unaffected, because a pin is keyed by `src` and every concolic has one. */
    DCHECK(eq_kind == OPCMP_NONE ? (tok == NULL && subj == NULL) : (tok != NULL),
           "a comparison result's operator and pin token disagree about whether its arms determine anything "
           "— the two are written together at this mint and read together by decide.c, which pins on one arm "
           "and records the exclusion on the other");
    f[0] = op; f[1] = ia; f[2] = ib;
    ident = concolic_ident_compose("?", f, 3);
    free(ia); free(ib);
    /* NOT a source read, so no candidate substitution — see concolic_alloc's declaration. */
    r = concolic_alloc(ctx, "{cmp}", src, root, ident, JS_UNDEFINED);
    c = JS_GetOpaque(r, g_concolic_class);
    DCHECK(c != NULL, "a comparison result was minted as something that is not a concolic value");
    c->cmp_op = eq_kind;
    c->cmp_tok = tok ? strdup(tok) : NULL;
    CHECK(!tok || c->cmp_tok, "concolic: OOM recording the value an equality pins to");
    c->cmp_subj = subj ? strdup(subj) : NULL;
    CHECK(!subj || c->cmp_subj, "concolic: OOM recording the hole an equality is a fact about");
    return r;
}

/* THE ORDERING'S OBSERVATION, STAMPED ON A PREDICATE pred_new HAS JUST MINTED. It is a second call rather than
   four more parameters because only ONE of pred_new's callers has an ordering to state, and a signature every
   caller has to spell `REL_NONE, NULL, 0, NULL` into is a signature that invites a caller to spell it wrong.
   The three facts are written HERE, together, so the assert that they are one observation has a single site. */
static void pred_set_bound(JSValueConst pred, RelOp rel, const char *tok, double num, const char *subj) {
    Concolic *c = JS_GetOpaque(pred, g_concolic_class);

    DCHECK(c != NULL, "an ordering's bound was stamped on something that is not a comparison result");
    DCHECK(rel != REL_NONE && tok != NULL && subj != NULL,
           "an ordering's relation, token and subject were not all present at the one line that writes them — "
           "decide.c reads all three at once to state a bound, so a result carrying some of them is a "
           "constraint that can be observed and never reported");
    DCHECK(c->rel_op == REL_NONE, "a comparison result was given a second bound — a predicate is one relation "
                                  "over one pair, so a second write is two facts wearing one identity");
    c->rel_op = rel;
    c->rel_num = num;
    c->rel_tok = strdup(tok);
    CHECK(c->rel_tok, "concolic: OOM recording the literal an ordering bounds a value by");
    c->rel_subj = strdup(subj);
    CHECK(c->rel_subj, "concolic: OOM recording the hole an ordering is a fact about");
}

/* A COMPARISON-RESULT bool carrying `src <op> tok`, for a component whose IDL member IS a comparison over its
   own source (HTML §6.2's `document.hidden` is `visibilityState === "hidden"`). It composes the identity the
   page's own `x === tok` composes for the same source and token, which is what makes the two ONE constraint
   entry — the property page_visibility.h states and now the encoding rather than a coincidence of spelling. */
JSValue concolic_new_cmp(JSContext *ctx, const char *src, int op, const char *tok) {
    const char *sf[1], *kf[2];

    DCHECK(op == OPCMP_EQ || op == OPCMP_NE,
           "a comparison result was declared with an operator that is neither an equality nor an inequality — "
           "an ordering is minted by the relational hook, which composes the engine's own operator id");
    sf[0] = src;
    kf[0] = "s"; kf[1] = tok;   /* the token is a string literal by construction here */
    /* THE COMPONENT'S OWN MEMBER IS A SOURCE READ, so it is its own root exactly as concolic_new's is. */
    /* THE COMPONENT'S SOURCE IS ITS OWN HOLE. A declared source's shape is its provenance in braces
       (concolic_source_wrap asserts exactly that), so stripping them gives `src` back — stated here rather
       than by calling the stripper on a shape this function was never handed. */
    return pred_new(ctx, op == OPCMP_NE ? "!=" : "==", src, src,
                    concolic_ident_compose("s", sf, 1), concolic_ident_compose("k", kf, 2), op, tok, src);
}
/* …AND THE TWIN FOR A RELATION OVER TWO LIVE VALUES, for a browser component whose own algorithm compares two
 * operands either of which may be unknown (HTML §8.7's timer task source orders one expiry against another).
 *
 * IT EXISTS SO THERE IS STILL ONE SPELLER OF THE KEY. The component could not reach concolic_rel_hook: that
 * entry takes the INTERPRETER'S OWN OPCODE for the operator, which is an identity quickjs never exports, and a
 * host that invented a number for it would be spelling a second opcode namespace beside the engine's. Nor could
 * it compose the key itself — decide.c keys a constraint by the value's identity precisely so that the format
 * lives in one place. So the component states the SPEC RELATION IT IS PERFORMING as the operator's name and
 * this file composes the identity, exactly as it composes `rel%d` for the interpreter; the two namespaces
 * cannot collide because concolic_ident_compose writes every field length-prefixed.
 *
 * IT PINS NOTHING, and that is the relation's own property rather than a shortcut. §Solver-half: an ordering
 * narrows a domain and determines no value, and an equality whose other side is also unknown has no concrete
 * value to pin to — so the pin stays where a pin can be honest, in the equality hook over a source and a
 * literal token. Both operands are BORROWED. */
JSValue concolic_new_rel(JSContext *ctx, const char *op, JSValueConst a, JSValueConst b) {
    JSValueConst opq;

    DCHECK(op != NULL,
           "a component asked for a relation over two values without naming the RELATION — the operator is "
           "half the predicate, so two different comparisons of one pair would compose to one identity and "
           "the flow's record of either would decide the other");
    DCHECK(concolic_is(a) || concolic_is(b),
           "a component asked the solver to relate two values NEITHER of which is unknown — a relation over "
           "two concrete operands is decided by running it, and minting a predicate for it would put a fork "
           "in the frontier over a question the engine can already answer");
    opq = concolic_is(a) ? a : b;
    return pred_new(ctx, op, concolic_src_c(opq), concolic_root_c(opq),
                    ident_of_operand(ctx, a), ident_of_operand(ctx, b), OPCMP_NONE, NULL, NULL);
}

int concolic_cmp(JSValueConst v, const char **psrc, const char **ptok) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    if (!c || c->cmp_op == OPCMP_NONE) return OPCMP_NONE;
    if (psrc) *psrc = c->src; if (ptok) *ptok = c->cmp_tok;
    return c->cmp_op;
}

RelOp concolic_rel(JSValueConst v, const char **ptok, double *pnum, const char **psubj) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    if (!c || c->rel_op == REL_NONE) return REL_NONE;
    DCHECK(c->rel_tok != NULL && c->rel_subj != NULL,
           "an ordering result carries a relation with no token or no subject — the three are one observation, "
           "written together at the mint, so a result holding only some of them is a bound whose own producer "
           "does not know what it is about");
    if (ptok)  *ptok  = c->rel_tok;
    if (pnum)  *pnum  = c->rel_num;
    if (psubj) *psubj = c->rel_subj;
    return c->rel_op;
}

const char *concolic_cmp_subject(JSValueConst v) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    if (!c || c->cmp_op == OPCMP_NONE) return NULL;
    return c->cmp_subj;   /* NULL = the operand's shape names no hole this surface prints — see pred_new */
}

/* JSConcolicCmpHook for == / === : a concolic operand -> a concolic bool whose IDENTITY is the operator and
   both operands, and which additionally carries the PIN when one side is concrete. Matches the slow-eq stack
   effect (both operands freed, result in sp[-2]). is_neq flips EQ->NE.
   LOOSE AND STRICT EQUALITY ARRIVE HERE AS ONE OPERATOR, because the hook is handed only `is_neq` — so
   `x == 'a'` and `x === 'a'` still compose to one identity and either decides the other. That is the same
   defect this file just removed from the ordering hook, in the one place the host cannot see the distinction:
   it needs quickjs to say which of the two called, the way JS_CARITH_* already spares the solver the opcode
   numbers. */
int concolic_cmp_hook(JSContext *ctx, JSValue *sp, int is_neq) {
    JSValue a = sp[-2], b = sp[-1];
    int ca = concolic_is(a), cb = concolic_is(b);
    JSValueConst opq, other;
    const char *src, *root;
    char *tok = NULL, *iu, *io, *subj;
    JSValue res;

    if (!ca && !cb) return 0;
    opq = ca ? a : b; other = ca ? b : a;
    src = concolic_src_c(opq);
    root = concolic_root_c(opq);   /* THE SAME OPERAND, or the assert at concolic_alloc has two facts about two values */
    /* THE PIN TOKEN IS THE OTHER OPERAND SPELLED, AND ONLY AN OPERAND THIS ENGINE CAN SPELL HAS ONE — which is
       the same question literal_ident answers, asked through the same classification so the two cannot
       disagree. It read the operand with a bare JS_ToCString, which for an OBJECT is §7.1.19 ToString step 10
       -> ToPrimitive from C with no flow base, and for a Symbol is step 2's TypeError left pending on ctx.
       ABSENCE IS THE RIGHT ANSWER AND NOT A GAP. §7.2.14 IsStrictlyEqual step 1 returns false whenever
       SameType(x, y) is false, so no string this solver could substitute for the unknown is ever === a given
       object: there is nothing to pin to. §7.2.13 IsLooselyEqual step 12 converts the OBJECT with ToPrimitive,
       i.e. runs the page's own code, so naming the object here would be inventing that call's result — which
       §@H forbids in the same words it forbids inventing `6` for `x > 5`. The predicate still forks: the
       comparison result below carries the operator and both operands' identities, and only the PIN is absent. */
    if (!concolic_is(other)) tok = literal_tok(ctx, other);
    /* EQUALITY IS SYMMETRIC, so `x === 'a'` and `'a' === x` compose to ONE identity: the unknown operand is
       written first, and where BOTH are unknown the two identities are ordered between themselves. Without
       that the same predicate written the other way round would be a second fact and fork a second time —
       which costs work but is sound; the ordering is what makes the sound answer also the cheap one. */
    iu = ident_of_operand(ctx, opq);
    io = ident_of_operand(ctx, other);
    /* THE TWO SPELLINGS OF ONE OPERAND AGREE, asserted where they meet. A token with no identity beside it is a
       flow that PINS its source to a value no predicate key mentions — the pin outlives the branch and every
       later read of that source computes it — while an identity with no token is the arm that was silently
       dropped when the classifications diverged. Only a CONCRETE other operand is spelled at all; a concolic
       one carries its own identity and pins nothing. */
    DCHECK(concolic_is(other) ? tok == NULL : (tok != NULL) == (io != NULL),
           "an equality's other operand was spelled as a pin token and as a predicate identity and the two "
           "disagreed about whether it can be spelled at all — one of them is reading a value the other says "
           "this engine cannot name");
    if (ca && cb && iu && io && strcmp(iu, io) > 0) { char *t = iu; iu = io; io = t; }
    /* THE HOLE THE REPORT WILL PRINT FOR THIS OPERAND — taken from the SHAPE and not from `src`, which for
       every derived value is the braced shape itself and would be looked up under a name the emission never
       spells. Minted only where there is a token, because the subject and the token are one observation. */
    subj = tok ? concolic_hole_key(concolic_shape_c(opq)) : NULL;
    res = pred_new(ctx, is_neq ? "!=" : "==", src, root, iu, io,
                   tok ? (is_neq ? OPCMP_NE : OPCMP_EQ) : OPCMP_NONE, tok, subj);
    free(subj);
    free(tok);
    JS_FreeValue(ctx, a); JS_FreeValue(ctx, b);
    sp[-2] = res;
    return 1;
}
/* CALLING an unknown yields an unknown. `document.cookie.indexOf("role=admin")` reads a field off a concolic —
   which concolic_exotic_get already answers with another concolic — and then CALLS it. A concolic that is not
   callable throws "not a function" there, and the throw takes the whole program with it: every statement after
   the first method call on an unknown string is unreachable, which silently truncated exploration at exactly
   the checks that gate a session. The result is concolic because that is what it IS: nothing here knows what
   indexOf would return over a cookie jar this engine was never given, so the comparison after it FORKS instead
   of collapsing. The ARGUMENTS still ran — they are the page's own expressions and their effects have already
   happened by the time this is reached. */
static JSValue concolic_call(JSContext *ctx, JSValueConst func_obj, JSValueConst this_val,
                             int argc, JSValueConst *argv, int flags)
{
    Concolic *c = JS_GetOpaque(func_obj, g_concolic_class);
    char *shape, *ident, **owned;
    const char **f;
    JSValue r;
    int i;

    (void)this_val; (void)flags;
    DCHECK(c != NULL, "the concolic call handler ran on something that is not a concolic value");
    shape = shapef("%s()", c->shape ? c->shape : "{}");
    /* THE ARGUMENTS ARE PART OF THE CALL'S IDENTITY. `x.startsWith("/api")` and `x.startsWith("javascript:")`
       are two gates over one source, and a call identity that dropped its arguments made the second DECIDED by
       the first — the same collapse the relational operator's missing operand made, reached by another route.
       An argument this engine cannot spell (a plain object) makes the whole identity absent, so both arms of
       every branch over the result stay. */
    owned = reclaim_malloc((size_t)(argc + 1) * sizeof *owned);
    f = reclaim_malloc((size_t)(argc + 2) * sizeof *f);
    CHECK(owned && f, "concolic: OOM identifying a call over an unknown value");
    f[0] = c->ident;
    for (i = 0; i < argc; i++) { owned[i] = ident_of_operand(ctx, argv[i]); f[i + 1] = owned[i]; }
    ident = concolic_ident_compose("()", f, argc + 1);
    for (i = 0; i < argc; i++) free(owned[i]);
    free(owned); free(f);
    /* The RESULT of calling an unknown is a new injection identity — nothing here knows what the call returned,
       so a candidate substitutes at the call — and it is NOT new bytes: whatever carried the callee's bytes in
       carried these. This is the derivation the reported defect walked through (`{location.hash}.slice()`). */
    r = concolic_derived(ctx, shape, shape, c->root, ident, JS_UNDEFINED);
    free(shape);
    return r;
}

/* ORDERING over an unknown is unknown. < <= > >= coerce with ToPrimitive, which a concolic cannot satisfy —
   it is an object whose coercion answers with another concolic — so the operator threw TypeError and took the
   whole program with it: `document.cookie.indexOf("role=admin") >= 0` explored NEITHER arm, losing both the
   session path and the anonymous one. The result is a concolic BOOL, so the branch forks exactly as an equality
   gate does; it carries no {op,tok} constraint because an ordering does not PIN a value the way `=== 'admin'`
   does — it narrows a domain, and the arm that is taken says which way.
   IT CARRIES ITS OPERATOR AND BOTH OPERANDS ALL THE SAME, and dropping them was a soundness bug and not a
   simplification. The equality hook above composed `{src, op, tok}`; this one composed nothing but the
   operand's SOURCE, so every ordering over one source was one predicate: in one flow `parseInt(gCS(a).width)
   < 700` and `parseInt(gCS(b).width) < 300` produced the same key and the second was DECIDED by the first,
   pruning an arm the flow's constraint does not contradict. `x < 700` and `x > 700` were one fact too.
   ORDERING IS NOT SYMMETRIC, so the operands keep the order the program wrote them: `x < 700` and `700 < x`
   are two predicates and neither may answer the other.
   AND IT NOW STATES ITS BOUND, which is the other half of §Solver-half's two-facts rule and the half the
   identity above cannot carry. An identity only tells predicates APART; a report has to SAY what one requires.
   `rel169` is a fact this run observed and could not print, so a parameter gated by `> 5` rendered with the
   same bytes as one nothing had ever tested — the exact asymmetry that made an equality's false arm a wrong
   report rather than a partial one, arriving on the second-most-frequent gate class in a real bundle.
   IT INVENTS NO VALUE. What is recorded is the relation the page wrote, the literal the page wrote, and the
   arm this run took; §@H's "never invent `6` for `x > 5`" is about the VALUE, and no value is determined here
   — the emission stays a domain-annotated shape. */
int concolic_rel_hook(JSContext *ctx, JSValue *sp, int op) {
    JSValue a = sp[-2], b = sp[-1];
    int ca = concolic_is(a), cb = concolic_is(b);
    char opid[24];
    RelOp rel;
    int w;

    if (!ca && !cb) return 0;
    /* THE OPERATOR ARRIVES AS THE ENGINE'S OWN OPCODE FOR IT (see the hook's contract in quickjs.h), which is
       an identity and not a name. The IDENTITY keeps the raw opcode — the four operators need only stay TOLD
       APART for the constraint key, and re-spelling them there would put a second encoding beside this file's
       one. The NAME is asked of solver/rel_op.h, which reads it off the engine's own opcode table.
       IT IS ASKED HERE, ON EVERY ORDERING, and not down beside the one branch that uses it: rel_op_of_opcode
       is what catches the host's rebuild of the opcode table drifting from quickjs.c's, and an assert placed
       under the numeric-operand test would only fire on a bundle that happens to compare against a literal —
       so a drifted build could run a whole document naming no relation and reporting nothing wrong. */
    rel = rel_op_of_opcode(op);
    w = snprintf(opid, sizeof opid, "rel%d", op);
    DCHECK(w > 0 && (size_t)w < sizeof opid, "a relational operator's id did not fit its own buffer");
    (void)w;
    {
        JSValueConst opq = ca ? a : b, other = ca ? b : a;
        JSValue res = pred_new(ctx, opid, concolic_src_c(opq), concolic_root_c(opq),
                               ident_of_operand(ctx, a), ident_of_operand(ctx, b), OPCMP_NONE, NULL, NULL);
        /* A BOUND EXISTS ONLY WHERE THE RELATION'S OWN MEANING IS DETERMINED, and §7.2.12 IsLessThan is what
           decides that. Step 3 takes the STRING comparison when px and py are BOTH Strings and the numeric
           one otherwise, so a concrete side that is a Number puts the comparison on the numeric path whatever
           the unknown turns out to be — and a concrete side that is a String leaves the page performing
           either a lexicographic or a numeric comparison depending on a value this engine does not have.
           Stating a bound there would be naming which comparison the page ran, which is the same invention as
           naming a value, so it is left absent.
           NON-FINITE IS THE SAME ANSWER FOR A DIFFERENT REASON: `x < Infinity` narrows nothing, `x < NaN` is
           false for every x (§7.2.12 answers undefined and §13.10.1 turns that into false), and neither
           Infinity nor NaN is a JSON number a report could carry. */
        /* Where neither holds, the predicate still forks and only the BOUND is absent. */
        if (!concolic_is(other) && JS_IsNumber(other)) {
            double num = 0;
            int gotn = JS_ToFloat64(ctx, &num, other);
            DCHECK(gotn == 0, "a Number operand did not convert to a double — §7.1.4 ToNumber over a value "
                              "JS_IsNumber has already answered for cannot fail, so this is the tag test and "
                              "the conversion disagreeing about what the operand is");
            if (gotn == 0 && isfinite(num)) {
                char *subj = concolic_hole_key(concolic_shape_c(opq));
                char *txt = subj ? literal_tok(ctx, other) : NULL;
                /* NORMALISED SUBJECT-LEFT EXACTLY ONCE. `700 < x` is `x > 700`; recording the relation as
                   written would make every consumer downstream re-derive which operand the page put first,
                   and the first one to forget reports the bound inverted. */
                if (txt) pred_set_bound(res, ca ? rel : rel_op_mirror(rel), txt, num, subj);
                free(txt);
                free(subj);
            }
        }
        JS_FreeValue(ctx, a); JS_FreeValue(ctx, b);
        sp[-2] = res;
    }
    return 1;
}

/* `typeof` an unknown is UNKNOWN. A concolic is a real object of a host class, and that class is callable so
   `document.cookie.indexOf(...)` works — which made typeof report "function" and a bundle testing
   `typeof x === "function"` take an arm decided by the solver's representation rather than the value. The
   answer is a concolic STRING carrying the same source, so the comparison after it forks like every other gate
   over an unknown. */
static char *cstr_dup(JSContext *ctx, JSValueConst v);   /* defined with the + hook below */

/* §7.1.4 ToNumber ( arg ) AND §7.1.19 ToString ( arg ) OVER UNKNOWN INPUT — answered where the operator
 * computes, never at the conversion boundary, which owes C a real primitive.
 *
 * The RESULT keeps the SOURCE, so `-location.hash` still forks a later branch and still solves at a later sink;
 * a value that collapsed to NaN or "[object Object]" here would end the exploration at the first coercion, which
 * is exactly what §Solver means by opacity surviving coercion.
 *
 * The EXAMPLE propagates by RUNNING THE REAL OP on the operands' examples — the engine actually negates, actually
 * multiplies — which is what makes this the concolic TRIPLE rather than a taint label with a derived note. When
 * an operand has no example there is nothing concrete to run and the result is example-free, which is the honest
 * answer: @H never invents. */
static const char *carith_name(int op, int *unary)
{
    *unary = (op <= JS_CARITH_DEC);
    switch (op) {
    case JS_CARITH_NEG:  return "-";
    case JS_CARITH_PLUS: return "+";
    case JS_CARITH_NOT:  return "~";
    case JS_CARITH_INC:  return "++";
    case JS_CARITH_DEC:  return "--";
    case JS_CARITH_SUB:  return "-";
    case JS_CARITH_MUL:  return "*";
    case JS_CARITH_DIV:  return "/";
    case JS_CARITH_MOD:  return "%";
    case JS_CARITH_POW:  return "**";
    case JS_CARITH_AND:  return "&";
    case JS_CARITH_OR:   return "|";
    case JS_CARITH_XOR:  return "^";
    case JS_CARITH_SHL:  return "<<";
    case JS_CARITH_SAR:  return ">>";
    case JS_CARITH_SHR:  return ">>>";
    default: DFAIL("concolic arithmetic with an unknown operator id"); return "?";
    }
}

/* WHICH OF THE TWO REAL OPERATIONS THIS OPERATOR IS. §6.1.6.1's numeric methods split cleanly in two: most
   compute over the Number itself, and seven begin by narrowing their operands to 32 bits — §6.1.6.1.2
   Number::bitwiseNOT ( number ), §6.1.6.1.9 Number::leftShift ( x, y ), §6.1.6.1.10 Number::signedRightShift
   ( x, y ), §6.1.6.1.11 Number::unsignedRightShift ( x, y ) and §6.1.6.1.16 NumberBitwiseOp ( op, x, y ). A
   double is the wrong carrier for the second group: §7.1.8 ToInt32 ( arg ) is "Let int be ? ToIntegerOrInfinity
   (arg). Return 𝔽(ToFixedSizeInteger(int, signed, 32))", which is defined for every Number including the
   infinities, while a C cast of a double outside the int32 range is undefined behaviour. So the engine's OWN
   §7.1.8/§7.1.9 run on the example and the result is an exact integer JSValue. */
static int carith_is_32bit(int op)
{
    return op == JS_CARITH_NOT || (op >= JS_CARITH_AND && op <= JS_CARITH_SHR);
}

static int carith_apply(int op, double a, double b, double *out)
{
    DCHECK(!carith_is_32bit(op),
           "a 32-bit operator reached the double arithmetic — §6.1.6.1.2/.9/.10/.11/.16 begin with ToInt32 or "
           "ToUint32, so casting their operands from a double is both wrong at the edges and undefined "
           "behaviour outside the int32 range; carith_apply32 is the one that runs them");
    switch (op) {
    case JS_CARITH_NEG:  *out = -a; return 1;
    case JS_CARITH_PLUS: *out = a; return 1;
    case JS_CARITH_INC:  *out = a + 1; return 1;
    case JS_CARITH_DEC:  *out = a - 1; return 1;
    case JS_CARITH_SUB:  *out = a - b; return 1;
    case JS_CARITH_MUL:  *out = a * b; return 1;
    case JS_CARITH_DIV:  *out = a / b; return 1;
    case JS_CARITH_MOD:  *out = fmod(a, b); return 1;
    case JS_CARITH_POW:  *out = pow(a, b); return 1;
    default: return 0;
    }
}

/* THE SEVEN 32-BIT OPERATIONS, run on the operands' EXAMPLES by the engine's own conversions. `exa`/`exb` are
   borrowed; `*out` is a new owned JSValue on success. Returns 0 when a conversion threw — an example that
   cannot convert produces NO example, which is the honest answer rather than a fabricated number. */
static int carith_apply32(JSContext *ctx, int op, JSValueConst exa, JSValueConst exb, JSValue *out)
{
    int32_t ia = 0, ib = 0;
    uint32_t ua = 0, ub = 0, sc;

    DCHECK(carith_is_32bit(op), "carith_apply32 reached with an operator §6.1.6.1 computes over the Number");
    DCHECK(op != JS_CARITH_NOT || JS_IsUndefined(exb),
           "§6.1.6.1.2 Number::bitwiseNOT takes ONE argument — a second operand here means the interpreter "
           "pushed a binary window for a unary operator");

    if (op == JS_CARITH_SHR) {
        /* §6.1.6.1.11: "Let leftNumber be ! ToUint32(x)." — the ONE operator whose left side is unsigned, which
           is why >>> carries its own id instead of sharing >>'s. */
        if (JS_ToUint32(ctx, &ua, exa) || JS_ToUint32(ctx, &ub, exb)) return 0;
        sc = ub % 32;   /* §6.1.6.1.11: "Let shiftCount be ℝ(rightNumber) modulo 32." */
        *out = JS_NewUint32(ctx, ua >> sc);
        return 1;
    }
    if (JS_ToInt32(ctx, &ia, exa)) return 0;
    if (op == JS_CARITH_NOT) {
        /* §6.1.6.1.2: "Let oldValue be ! ToInt32(number). Return the bitwise complement of oldValue." */
        *out = JS_NewInt32(ctx, ~ia);
        return 1;
    }
    if (op == JS_CARITH_SHL || op == JS_CARITH_SAR) {
        /* §6.1.6.1.9/.10: ToInt32 on the left, ToUint32 on the RIGHT, shiftCount modulo 32. */
        if (JS_ToUint32(ctx, &ub, exb)) return 0;
        sc = ub % 32;   /* §6.1.6.1.9/.10: "Let shiftCount be ℝ(rightNumber) modulo 32." */
        /* The left shift is performed on the unsigned bit string §6.1.6.1.9 names, so a negative left operand
           and an overflow are both defined here rather than undefined in C. */
        *out = JS_NewInt32(ctx, op == JS_CARITH_SHL ? (int32_t)((uint32_t)ia << sc) : (ia >> sc));
        return 1;
    }
    /* §6.1.6.1.16 NumberBitwiseOp: ToInt32 on both, then the bit string operation. */
    if (JS_ToInt32(ctx, &ib, exb)) return 0;
    *out = JS_NewInt32(ctx, op == JS_CARITH_AND ? (ia & ib)
                          : op == JS_CARITH_OR  ? (ia | ib)
                                                : (ia ^ ib));
    return 1;
}

int concolic_arith_hook(JSContext *ctx, JSValue *sp, int op, int nops) {
    JSValue a = sp[-nops], b = nops == 2 ? sp[-1] : JS_UNDEFINED;
    int ca = concolic_is(a), cb = nops == 2 && concolic_is(b);
    const char *name;
    int unary = 0;
    char *shape, *ident;
    const char *src, *root;
    JSValue example = JS_UNDEFINED, res;

    if (!ca && !cb) return 0;
    DCHECK(nops == 1 || nops == 2,
           "a concolic arithmetic hook was handed an arity no ECMAScript operator has — the operand window is "
           "sp[-nops..sp[-1]] and the result is written to sp[-nops], so a third arity has no stack shape");
    name = carith_name(op, &unary);
    DCHECK(unary == (nops == 1),
           "the operator's declared arity disagrees with the operand count the interpreter pushed — the two "
           "are one fact (JS_CARITH_*'s unary ids come first) and the identity composition reads the arity "
           "from carith_name while the stack effect reads it from nops");
    src = ca ? concolic_src_c(a) : concolic_src_c(b);
    root = ca ? concolic_root_c(a) : concolic_root_c(b);

    /* THE OPERATOR AND ITS OPERANDS ARE THE IDENTITY, and the ARITY is part of it too: `carith_name` spells
       negation and subtraction both `-`, and a one-member composition can never write the same bytes as a
       two-member one. Without this `x` and `x*2` were one fact and `if (x) … if (x*2)` decided each other. */
    if (unary) {
        const char *f[1];
        char *ia = ident_of_operand(ctx, a);
        shape = shapef("%s%s", name, concolic_shape_c(a) ? concolic_shape_c(a) : "{}");
        f[0] = ia;
        ident = concolic_ident_compose(name, f, 1);
        free(ia);
    } else {
        const char *f[2];
        char *ia = ident_of_operand(ctx, a), *ib = ident_of_operand(ctx, b);
        char *sa = ca ? strdup(concolic_shape_c(a) ? concolic_shape_c(a) : "{}") : cstr_dup(ctx, a);
        char *sb = cb ? strdup(concolic_shape_c(b) ? concolic_shape_c(b) : "{}") : cstr_dup(ctx, b);
        CHECK(sa && sb, "concolic arithmetic: OOM shape");
        shape = shapef("%s%s%s", sa, name, sb);
        free(sa); free(sb);
        f[0] = ia; f[1] = ib;
        ident = concolic_ident_compose(name, f, 2);
        free(ia); free(ib);
    }

    /* RUN THE REAL OP on the examples when there are any — the concrete half of the triple. */
    {
        JSValue exa = ca ? concolic_example(ctx, a) : JS_DupValue(ctx, a);
        JSValue exb = nops == 2 ? (cb ? concolic_example(ctx, b) : JS_DupValue(ctx, b)) : JS_UNDEFINED;

        /* AN EXAMPLE IS A PRIMITIVE. Every conversion below is §7.1.4 ToNumber, whose object case is
           "Let primitiveValue be ? ToPrimitive(arg, number)" — running the page's valueOf from this C frame,
           which has no flow base under it. A source or a derivation that attached an OBJECT example put a value
           here that only crashes, so the assert names the producer rather than the coercion. */
        DCHECK(!JS_IsObject(exa) && !JS_IsObject(exb),
               "a concolic carries an OBJECT as its concrete example — an example is the value the engine "
               "COMPUTED, so it is a primitive; ToNumber over an object one reaches ToPrimitive from C");

        if (!JS_IsUndefined(exa) && (nops == 1 || !JS_IsUndefined(exb))) {
            if (carith_is_32bit(op)) {
                JSValue r32 = JS_UNDEFINED;
                if (carith_apply32(ctx, op, exa, exb, &r32))
                    example = r32;
            } else {
                double da = 0, db = 0, out = 0;
                int oka = JS_ToFloat64(ctx, &da, exa) == 0;
                int okb = oka && (nops == 1 || JS_ToFloat64(ctx, &db, exb) == 0);
                DCHECK(oka && okb,
                       "a concolic's example could not be converted by §7.1.4 ToNumber — its steps 2 and 3 "
                       "throw only for a Symbol or a BigInt, and no source in this engine mints either as an "
                       "example, so the producer attached a value it never computed");
                if (oka && okb && carith_apply(op, da, db, &out))
                    example = JS_NewFloat64(ctx, out);
            }
            /* A CONVERSION THAT THREW LEFT ITS EXCEPTION STANDING IN THE CONTEXT, and the operator is about to
               return 1 — a success — so the page's very next statement would have thrown a TypeError it never
               wrote. The example is dropped (@H never invents) and the throw is dropped with it, because the
               throw belongs to a coercion the PROGRAM did not perform. */
            if (JS_IsUndefined(example) && JS_HasException(ctx))
                JS_FreeValue(ctx, JS_GetException(ctx));
        }
        JS_FreeValue(ctx, exa); JS_FreeValue(ctx, exb);
    }

    res = concolic_derived(ctx, shape, src ? src : shape, root ? root : shape, ident, example);
    free(shape);
    JS_FreeValue(ctx, sp[-nops]);
    if (nops == 2) JS_FreeValue(ctx, sp[-1]);
    sp[-nops] = res;
    return 1;
}

/* §7.1.19 ToString ( arg ) over unknown input: unknown, source kept, example computed by actually stringifying
   the example when there is one.
   IT IS THE ANSWER FOR 22.1.1.1 String ( value ) AND FOR NOTHING ELSE, which is what the shape says: `String(x)`
   is the algorithm whose RESULT is the coercion. A builtin that merely CONSUMES the string derives its own
   result instead, named by its own operation — concolic_builtin_hook, reached from the engine's one shared
   ToString sub-sequence (JS_STEP_UNKNOWN). Two derivations, not two spellings of one. */
JSValue concolic_tostr_hook(JSContext *ctx, JSValueConst v) {
    const char *src, *root, *sh, *f[1];
    char *shape, *ident;
    JSValue ex, example = JS_UNDEFINED, r;

    if (!concolic_is(v)) return JS_UNINITIALIZED;
    src = concolic_src_c(v);
    root = concolic_root_c(v);
    sh = concolic_shape_c(v);
    shape = shapef("String(%s)", sh ? sh : "{}");
    f[0] = concolic_ident_c(v);
    ident = concolic_ident_compose("String", f, 1);
    ex = concolic_example(ctx, v);
    if (!JS_IsUndefined(ex)) {
        const char *p = JS_ToCString(ctx, ex);
        if (p) { example = JS_NewString(ctx, p); JS_FreeCString(ctx, p); }
    }
    JS_FreeValue(ctx, ex);
    r = concolic_derived(ctx, shape, src ? src : shape, root ? root : shape, ident, example);
    free(shape);
    return r;
}

/* A BUILTIN OVER AN UNKNOWN OPERAND — see the hook's contract in quickjs.h. The shape records WHICH operation
   produced it, so an @H shape reads as the expression the page actually wrote and an @S search knows which
   source to solve for. Example-free: this engine does not yet run the operation on the operand's example (a
   regex match over a known query string HAS a concrete answer, and producing it is the next step here), and
   inventing one would be a fabricated observation. */
JSValue concolic_builtin_hook(JSContext *ctx, JSValueConst v, const char *op, JSValue example) {
    const char *src, *root, *sh, *f[2];
    char *shape, *ident;
    JSValue r;

    if (!concolic_is(v)) { JS_FreeValue(ctx, example); return JS_UNINITIALIZED; }
    DCHECK(op != NULL,
           "a builtin derived an unknown result without naming the OPERATION it was performing — the operation "
           "is what tells two derivations from one operand apart, and a value that dropped it would be decided "
           "by whichever of them this flow reached first");
    src = concolic_src_c(v);
    root = concolic_root_c(v);
    sh = concolic_shape_c(v);
    shape = shapef("%s.%s()", sh ? sh : "{}", op);
    f[0] = concolic_ident_c(v); f[1] = op;
    ident = concolic_ident_compose("b", f, 2);
    /* `example` is what the operator got by RUNNING THE REAL OPERATION on this operand's own example. It is
       never computed here and never predicted: the codec really encoded, the parser really parsed. */
    r = concolic_derived(ctx, shape, src ? src : shape, root ? root : shape, ident, example);
    free(shape);
    return r;
}

/* THE BYTES A DOM MEMBER NEEDS FROM AN ARGUMENT THAT MAY BE UNKNOWN — a selector, an attribute name, a class
   token, an element id. Every one of those call sites did JS_ToCString under a comment saying "a real string by
   now: the declaration converted it", and that comment is FALSE for a concolic: the IDL boundary passes unknown
   input through as itself on purpose, so the coercion crashed and `document.querySelector(location.hash)` ended
   the document. An unknown NAME denotes its SHAPE, which is the same rule the key_name hook already states for
   `obj[x]` and the same one setAttribute already applies to an unknown VALUE: a real string, stable per source,
   so two lookups through one source agree and two sources never collide. OWNED either way, so the call site
   keeps one free path and does not grow a branch. */
const char *concolic_name_cstr(JSContext *ctx, JSValueConst v) {
    if (concolic_is(v)) {
        const char *sh = concolic_shape_c(v);
        JSValue s = JS_NewString(ctx, sh ? sh : "{}");
        const char *r = JS_ToCString(ctx, s);
        JS_FreeValue(ctx, s);
        return r;
    }
    return JS_ToCString(ctx, v);
}

/* THE NAME an unknown key denotes: its own SHAPE, as a real string. Stable per source, so every key-taking
   operation agrees with every other — see the contract at JS_ToPropertyKeyInternal. */
JSValue concolic_key_name_hook(JSContext *ctx, JSValueConst key) {
    const char *sh;
    if (!concolic_is(key)) return JS_UNINITIALIZED;
    sh = concolic_shape_c(key);
    return JS_NewString(ctx, sh ? sh : "{}");
}

/* `obj[x]` WITH AN UNKNOWN KEY. Not a coercion of the operand: nothing about x says WHICH slot was meant, so
   the read's result is unknown and the honest answer is a concolic that keeps the key's source — a gate on the
   value still forks, and a sink still solves for the key that would reach it. Example-free on purpose: which
   property the attacker names is exactly what is not known, and @H never invents one. */
JSValue concolic_key_read_hook(JSContext *ctx, JSValueConst obj, JSValueConst key) {
    const char *src, *root, *f[2];
    char *shape, *ident, *io, *ik;
    JSValue r;

    if (!concolic_is(key)) return JS_UNINITIALIZED;
    src = concolic_src_c(key);
    root = concolic_root_c(key);
    shape = shapef("{}[%s]", concolic_shape_c(key) ? concolic_shape_c(key) : "{}");
    /* WHICH OBJECT WAS READ IS HALF THE IDENTITY: `a[x]` and `b[x]` are two reads and one must not decide the
       other. An ordinary object has no identity that survives the park a resumed flow replays through, so the
       composition is ABSENT there and every branch over the result keeps both arms. */
    io = ident_of_operand(ctx, obj);
    ik = ident_of_operand(ctx, key);
    f[0] = io; f[1] = ik;
    ident = concolic_ident_compose("[]", f, 2);
    free(io); free(ik);
    r = concolic_derived(ctx, shape, src ? src : shape, root ? root : shape, ident, JS_UNDEFINED);
    free(shape);
    return r;
}

JSValue concolic_typeof_hook(JSContext *ctx, JSValueConst v) {
    const char *src, *f[1];
    char *shape, *ident;
    JSValue r;

    if (!concolic_is(v)) return JS_UNINITIALIZED;
    src = concolic_src_c(v);
    shape = shapef("typeof %s", src ? src : "{}");
    f[0] = concolic_ident_c(v);
    ident = concolic_ident_compose("typeof", f, 1);
    r = concolic_derived(ctx, shape, shape, concolic_root_c(v), ident, JS_UNDEFINED);
    free(shape);
    return r;
}

/* `x in concolic` / property existence: a concolic collection "has" any key (so a membership gate still runs). */
static int concolic_exotic_has(JSContext *ctx, JSValueConst obj, JSAtom atom) {
    (void)ctx; (void)atom;
    return JS_GetOpaque(obj, g_concolic_class) != NULL;
}
static JSClassExoticMethods g_concolic_exotic = {
    .get_property = concolic_exotic_get,
    .has_property = concolic_exotic_has,
};

/* HOW MANY VALUES THE SOURCE OVERLAY HAS MINTED — see concolic_source_wrap for what the number is for. Reset
   with the agent, because it is a fact about ONE document's run and the result document that reports it is per
   document; a counter that survived would make the second page in an instance report the first one's reading.
   DECLARED HERE, ABOVE ITS FIRST USE, and not beside the overlay flag it belongs with three hundred lines
   down: `concolic_init` zeroes it, so a declaration after that line does not compile at all. The accessor
   stays beside the overlay, where the reader looking for "who mints these" will be. */
static long g_source_reads;

void concolic_init(JSContext *ctx) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    g_source_reads = 0;   /* per document: see the counter's declaration above */
    if (g_concolic_class == 0) {
        JS_NewClassID(rt, &g_concolic_class);
        DCHECK(g_concolic_class != 0, "concolic: class id allocation returned 0 — runtime class table exhausted");
    }
    if (!JS_IsRegisteredClass(rt, g_concolic_class)) {
        JSClassDef def = { "Concolic", .finalizer = concolic_finalizer, .gc_mark = concolic_gc_mark,
                           .call = concolic_call, .exotic = &g_concolic_exotic };
        int r = JS_NewClass(rt, g_concolic_class, &def);
        CHECK(r == 0, "concolic: JS_NewClass failed — cannot register the solver's value type");
    }
}

/* THE HOLDER'S END OF THE SOURCE REGISTRY — solver/engine.h's release column, run after the whole platform.
 *
 * THE ROWS ARE NOT FREED HERE. Each is a CLAIM whose claimant gives it back at its own release, and every one
 * of those releases is a row on core/platform.h's third column — which platform_agent_free runs BEFORE the
 * solver's. So an empty registry here is a checked statement about the CLAIMANTS, and a non-empty one names
 * the one that did not finish rather than leaving a strdup'd row that answers a later agent's delivery
 * question for a document that no longer exists.
 *
 * IT NAMES THE COMPONENT OUT OF THE DATA, which is the whole reason the row carries its claimant. The obvious
 * message here lists the claimants and how many rows each owes, and that list is the stale-`DFAIL` failure
 * mode CLAUDE.md describes: it stays true about the CONTRACT and goes wrong about THIS TREE the first time a
 * component is added, renamed or moved, while reading as authoritative to whoever the abort sends looking. The
 * row knows who owns it, so the assert reads it and this file names nobody.
 *
 * WHAT THIS OWNS IS THE ARRAY UNDER THEM, and it is freed rather than merely emptied. They are two different
 * facts: `g_srcs_n` is what every lookup reads, and `g_srcs`/`g_srcs_cap` is a plain `malloc`'d buffer that no
 * detector this tree has can see — not gc_obj_list (not a GC object), not the atom walk (not an atom), not
 * JS_DUMP_LEAKS's malloc_count (it counts js_malloc_rt). A release that reset the count and kept the buffer is
 * exactly the fetch_free shape core/agent_state.h records, one layer down: the next agent's first declaration
 * grows a capacity a dead runtime's reclaim allocator sized. The COUNT goes with the buffer for a harder
 * reason than symmetry — a release build compiles the assert above out, and a count left standing over a freed
 * array is a use-after-free at the next lookup rather than a leak. */
void concolic_free(void)
{
#if APICLIENT_DEV
    if (g_srcs_n != 0)
        DFAILF("%s did not give back the attacker SOURCE `%s` before the solver's agent state was released. "
               "Each row is a claim in this component's array whose claimant releases it at its own "
               "concolic_undeclare_sources, on core/platform.h's release column, which platform_agent_free "
               "runs before this call. A row left behind is not only a leaked pair of strings: it describes "
               "a browser delivery for a document that is gone, and it is what the next agent's declaration "
               "of the same source collides with", g_srcs[0].component, g_srcs[0].src);
#endif
    free(g_srcs);
    g_srcs = NULL;
    g_srcs_n = g_srcs_cap = 0;
    /* THE INSTALLED @S SUBSTITUTION IS THIS COMPONENT'S OWN COPY, not the flow's. concolic_set_candidate strdups
       both halves out of the running flow at every switch, so the last flow to run leaves a pair behind that
       the frontier's own release cannot reach — flow_registry_free frees the FLOW's strings, which were never
       these. Freed rather than asserted null, because nothing orders the last switch against this call. */
    free(g_cand_src);
    free(g_cand_payload);
    g_cand_src = g_cand_payload = NULL;
    /* AND THE PUBLISHED-NAMESPACE PATHS, for the same reason the source rows above are given back here: a path
       names a record of a document that is gone, and the addresses it is keyed by are about to be reused. */
    absent_free();
    /* §Solver's VALUE CLASS IS DELIBERATELY NOT GIVEN BACK HERE, and that is asserted rather than commented —
       the same statement core/dom/document.c makes about §gc's realm-mark hook, for the same reason. Every live
       Concolic OUTLIVES this call: the objects are freed by JS_FreeRuntime, whose finalizer reaches each
       record through this id, and the collection a host runs before it marks each example through it. An id
       cleared here would make both of those read NULL — every record's shape, provenance, identity and example
       retained with nothing naming them, and an example the collector stopped marking freed under a value that
       still points at it. The id belongs to the runtime for as long as the runtime holds objects branded with
       it, so this is where that is checked and not where it is undone. */
    DCHECK(g_concolic_class != 0,
           "the Concolic value class was given back before the runtime that issued it — it must NOT be, and "
           "this is where that is checked: every value of this class is finalized by JS_FreeRuntime, which "
           "reaches its record through this id, and the collection before it marks each example through it");
}

static JSValue concolic_alloc(JSContext *ctx, const char *shape, const char *src, const char *root,
                              char *ident, JSValue example)
{
    JSValue obj;
    Concolic *c;

    DCHECK(g_concolic_class != 0, "concolic_new before concolic_init — the class is unregistered");
    /* THE TWO ARE PRESENT TOGETHER OR ABSENT TOGETHER, asserted at the ONE mint every value goes through. A
       value that has a provenance HAS a root: either it is a source read, whose root is itself, or it was
       derived from something that had one. A value with a `src` and no `root` is a derivation that dropped
       the fact on the way — which is not a crash anywhere, it is a report that names a source and then states
       that no navigation delivers it. And the reverse would be a root with nothing to inject at. */
    DCHECK(!!src == !!root,
           "a concolic value carries a provenance without a delivery ROOT, or a root with no provenance — the "
           "two are one fact about where the bytes came from and every derivation inherits the second while "
           "some of them re-mint the first, so a mismatch is a derivation that forgot to thread it");
    obj = JS_NewObjectClass(ctx, g_concolic_class);
    CHECK(!JS_IsException(obj), "concolic: the value object could not be allocated — a dropped concolic "
                                "collapses a branch to a concrete arm and deletes everything behind the other");
    c = reclaim_calloc(1, sizeof *c);
    CHECK(c, "concolic_new: OOM allocating value state — a dropped concolic corrupts the flow's domain");
    c->shape = strdup(shape ? shape : "{}");
    CHECK(c->shape, "concolic: OOM copying a display shape");
    c->src = src ? strdup(src) : NULL;
    CHECK(!src || c->src, "concolic: OOM copying a source's provenance");
    c->root = root ? strdup(root) : NULL;
    CHECK(!root || c->root, "concolic: OOM copying a value's delivery root");
    c->ident = ident;       /* consume — NULL means this engine cannot spell the value; see the struct */
    c->example = example;   /* consume */
    c->cmp_op = OPCMP_NONE;
    c->rel_op = REL_NONE;   /* reclaim_calloc already zeroes it; stated beside cmp_op so the two stay one act */
    JS_SetOpaque(obj, c);
    return obj;
}

static JSValue concolic_derived(JSContext *ctx, const char *shape, const char *src, const char *root,
                                char *ident, JSValue example)
{
    /* A CANDIDATE RUN substitutes one source with a breakout. The check lived only in the field-read path, so a
       source installed as a plain property value — location.hash, document.cookie — was minted once at install
       and never passed through it: its candidate could not be delivered and the sink never fired. Minting is
       the one place every source goes through, whichever way it is reached. */
    if (g_cand_src && src && !strcmp(src, g_cand_src)) {
        /* THE EXAMPLE IS CONSUMED ON THIS PATH TOO. `example` is owned by this call whichever value comes back,
           and the candidate's payload REPLACES it — a source under substitution reads the attacker's bytes, not
           what the address concretely held. Returning without the free leaked it, and it stopped being a
           dormant leak the moment sources started carrying one: location.search/hash hand over the address's
           real query and fragment, so every candidate re-fire of a URL source leaked a string. */
        JS_FreeValue(ctx, example);
        free(ident);
        return concolic_deliver(ctx, src, root, g_cand_payload);
    }
    return concolic_alloc(ctx, shape, src, root, ident, example);
}

/* A SOURCE READ — the root of every identity. Its identity IS its provenance, because nothing derived it: this
   is where an unknown enters the program. A source with no provenance has no identity either, which is the
   honest answer and the one that keeps both arms of every branch over it. */
JSValue concolic_new(JSContext *ctx, const char *shape, const char *src, JSValue example) {
    const char *f[1];

    /* A SOURCE'S SHAPE NAMES A HOLE, AND THE BRACE IS WHAT MAKES IT ONE — asserted at the ONE mint every
       source goes through, because it was true of most components and silently false for six.
       concolic_hole_key is the only route there is from a shape to a domain, and its first line is
       `if (!strchr(shape, '{')) return NULL`. So an unbraced source has no hole key, and BOTH ends of the
       domain machinery then go quiet without failing: concolic_cmp_subject mints no subject, the ordering
       hook's `txt = subj ? literal_tok(...) : NULL` means pred_set_bound is never called at all, and
       endpoint.c looks a domain up under a name nothing was ever filed under. The constraint is OBSERVED and
       DROPPED — `if (screen.width < 768)` records nothing, `navigator.userAgent === x` records nothing — and
       the parameter is emitted carrying provenance and no domain, which §@S calls a WRONG report rather than
       a partial one, because its silence about the gate reads as "anything goes".
       ENDPOINT.c CANNOT USE A LAXER RULE, which is why the brace belongs here rather than there: kv_pairs
       hands concolic_hole_key an arbitrary query-value SUBSTRING, so `lang=en-US` would mint a hole named
       `en-US` and a concrete value would borrow a domain. The brace is what tells a hole from a literal once
       the concolic that carried it is gone, so it has to be in the shape from the start.
       AND IT MAKES ABSENCE MEAN ONE THING. pred_new documents a NULL subject as the honest answer for the
       unnameable `{}` — one fact. It was also the answer for every navigator, screen and viewport member,
       which is a different fact wearing the same NULL. With this, `{}` is the only one left. */
    DCHECK(shape != NULL && strchr(shape, '{') != NULL,
           "a SOURCE was minted with a display shape that names no hole — a shape carries its provenance in "
           "braces, because concolic_hole_key reads a hole by them and it is the only path from a shape to a "
           "domain. Without one this value's every gate is observed and discarded, and its parameter reports "
           "provenance with no constraint. Spell the shape as the src in braces (core/frame/location.h is the "
           "pattern); a shape that is not simply `{src}` is fine as long as it names its hole");
    f[0] = src;
    /* A SOURCE READ IS ITS OWN ROOT — stated here, once, rather than as a second argument every one of the
       seventeen components that owns a source would have to spell the same way twice. */
    return concolic_derived(ctx, shape, src, src, concolic_ident_compose("s", f, 1), example);
}

int concolic_is(JSValueConst v) {
    return g_concolic_class != 0 && JS_GetOpaque(v, g_concolic_class) != NULL;
}

const char *concolic_shape_c(JSValueConst v) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    return c ? c->shape : NULL;
}

const char *concolic_src_c(JSValueConst v) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    return c ? c->src : NULL;
}

const char *concolic_root_c(JSValueConst v) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    return c ? c->root : NULL;
}

const char *concolic_ident_c(JSValueConst v) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    return c ? c->ident : NULL;
}

JSValue concolic_example(JSContext *ctx, JSValueConst v) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    if (!c || JS_IsUndefined(c->example)) return JS_UNDEFINED;
    return JS_DupValue(ctx, c->example);
}

void concolic_set_example(JSContext *ctx, JSValueConst v, JSValue example) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    if (!c) { JS_FreeValue(ctx, example); return; }
    JS_FreeValue(ctx, c->example);
    c->example = example;   /* consume */
}

static char *cstr_dup(JSContext *ctx, JSValueConst v) {   /* concrete operand -> its string form (heap copy) */
    const char *s = JS_ToCString(ctx, v);
    char *r = strdup(s ? s : "");
    if (s) JS_FreeCString(ctx, s);
    return r;
}

/* §13.15.3 step 1.c's CONCLUSION, run on the two EXAMPLES: "Let leftString be ? ToString(leftPrimitive). Let
   rightString be ? ToString(rightPrimitive). Return the string-concatenation of leftString and rightString."
   `exa`/`exb` are borrowed; the result is a new owned JSValue, or JS_UNDEFINED for "no example". */
static JSValue example_string_concat(JSContext *ctx, JSValueConst exa, JSValueConst exb) {
    const char *pa, *pb;
    JSValue example = JS_UNDEFINED;

    pa = JS_ToCString(ctx, exa);
    pb = pa ? JS_ToCString(ctx, exb) : NULL;
    /* THE `if (e)` THAT USED TO STAND HERE WAS THE CONCEALMENT, not the safety. §Solver-half: the example
       propagates because the engine RUNS the real op, so a `+` that quietly produced no example turns a
       computed value into a shape — an @H row that says `{a}{b}` where the code determined `/api/us-east-1`
       — and nothing anywhere says a concatenation was dropped. With the refusal edge underneath it a NULL
       is the physical floor and nothing else, which is exactly what a CHECK is for. */
    if (pa && pb) {
        size_t l = strlen(pa) + strlen(pb) + 1;
        char *e = reclaim_malloc(l);
        CHECK(e, "concolic +: the concatenated example could not be allocated — the sum would carry a "
                 "shape where the code computed a value");
        snprintf(e, l, "%s%s", pa, pb);
        example = JS_NewString(ctx, e);
        free(e);
    }
    if (pa) JS_FreeCString(ctx, pa);
    if (pb) JS_FreeCString(ctx, pb);
    /* A CONVERSION THAT REFUSED LEFT ITS THROW STANDING and the operator is about to report SUCCESS, so the
       page's next statement would throw a TypeError it never wrote. §7.1.19 ToString refuses only for a Symbol
       (its step 2) and for an object whose ToPrimitive threw, and the caller's assert says neither operand is
       one — so in dev this drains nothing and in release it drains what a compiled-out assert stopped saying. */
    if (JS_IsUndefined(example) && JS_HasException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
    return example;
}

/* §13.15.3 steps 2-7 over the two EXAMPLES, once step 1.c has said neither is a String: step 2's "NOTE: At
   this point, it must be a numeric operation", then "Let leftNumber be ? ToNumeric(leftValue)" and the same
   for the right (§7.1.3 ToNumeric, which is §7.1.4 ToNumber for everything that is not a BigInt), then step
   7's table entry for `+`, which is §6.1.6.1.7 Number::add ( x, y ). Number::add's steps ARE IEEE 754 binary64
   addition — the NaN and infinity rows, the -0𝔽 + -0𝔽 row, and finally "Return 𝔽(ℝ(x) + ℝ(y))" — so the engine
   RUNS the real operation here rather than re-implementing its cases. Borrowed operands; owned result, or
   JS_UNDEFINED for "no example". */
static JSValue example_number_add(JSContext *ctx, JSValueConst exa, JSValueConst exb) {
    double da = 0, db = 0;
    int oka = JS_ToFloat64(ctx, &da, exa) == 0;
    int okb = oka && JS_ToFloat64(ctx, &db, exb) == 0;

    /* §7.1.4 ToNumber step 2 — "If arg is either a Symbol or a BigInt, throw a TypeError exception" — is the
       ONLY refusal a primitive can reach here, and no source or derivation in this engine mints either as an
       example. So an arrival is a PRODUCER having attached a value it never computed. It is also where the
       BigInt arm gets built when one does exist: §13.15.3 step 5's "If SameType(leftNumber, rightNumber) is
       false, throw a TypeError exception" and step 6's §6.1.6.2.7 BigInt::add ( x, y ). Deliberately NOT built
       on speculation — a BigInt example would be a fabricated observation with nothing producing it. */
    DCHECK(oka && okb,
           "a concolic's example refused §13.15.3 step 3/4's §7.1.3 ToNumeric — its only primitive refusals "
           "are §7.1.4 ToNumber step 2's Symbol and BigInt, and no producer in this engine mints either as an "
           "example; build §13.15.3 step 5's SameType test and step 6's §6.1.6.2.7 BigInt::add here when one "
           "does");
    if (!oka || !okb) {
        /* The example is dropped (@H never invents) and the throw is dropped with it: it belongs to a
           coercion the PROGRAM did not perform — the program's `+` is over the unknown, not over this
           engine's guess at its concrete value, and a concolic operand's own type is not the example's. */
        DCHECK(JS_HasException(ctx),
               "§7.1.3 ToNumeric reported a refusal and left no exception standing — the drain below would "
               "then take the NEXT operator's throw instead of this one's");
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_UNDEFINED;
    }
    return JS_NewFloat64(ctx, da + db);
}

/* A CONCATENATION WHERE EITHER OPERAND IS CONCOLIC -> a DERIVED concolic. `op` names which spec algorithm is
   concatenating (JSConcolicAddOp in quickjs.h); matches js_add_slow's stack effect — both operands freed,
   result in sp[-2].
   THE SHAPE IS display(a)++display(b) FOR BOTH ARMS, and that is deliberate rather than an oversight of the
   numeric one. The shape is a function of the OPERAND SHAPES ALONE, because .key_name spells an unknown
   property key with it and relies on it being stable: `obj[x+1] = v` must write the slot `obj[x+1]` later
   reads. Composing it from the arm instead would make one expression name two different slots depending on
   whether an example happened to be known in that flow, which is a miss with nothing to report it.
   THE EXAMPLE IS §13.15.3's OWN TEST, ASKED OF THE EXAMPLES. An example is a concrete value this engine
   COMPUTED, so running the real operator on the two of them is running it on concrete operands, and step
   1.c's test is over their types. Steps 1.a and 1.b are NOT performed here: js_add_slow calls this before any
   coercion, and §7.1.1 over a real object is the page's own code, so the assert inside names the trampoline
   that owes them. This hook reported `concolic(5) + 3` as `"53"` for as long as it had only
   one arm, and §@H emits a computed value as an OBSERVED fact, so that string was published as a measurement
   of an endpoint the code never addressed. The concolic RESULT is unchanged either way: provenance, shape and
   identity are the same and a later branch still forks, which is what keeps both arms of the gate. */
int concolic_add_hook(JSContext *ctx, JSValue *sp, JSConcolicAddOp op) {
    JSValue a = sp[-2], b = sp[-1];
    int ca = concolic_is(a), cb = concolic_is(b);
    DCHECK(op == JS_CONCOLIC_ADD_PLUS || op == JS_CONCOLIC_ADD_CONCAT,
           "a concatenation reached the concolic derivation without naming which spec algorithm it is — "
           "13.15.3 and 22.1.3.5 disagree about the numeric arm, so an unnamed caller has no answer here");
    if (!ca && !cb) return 0;

    char *sha = ca ? strdup(concolic_shape_c(a) ? concolic_shape_c(a) : "{}") : cstr_dup(ctx, a);
    char *shb = cb ? strdup(concolic_shape_c(b) ? concolic_shape_c(b) : "{}") : cstr_dup(ctx, b);
    CHECK(sha && shb, "concolic +: OOM shape");
    size_t ln = strlen(sha) + strlen(shb) + 1;
    char *shape = reclaim_malloc(ln); CHECK(shape, "concolic +: OOM shape concat");
    snprintf(shape, ln, "%s%s", sha, shb);
    const char *src = ca ? concolic_src_c(a) : concolic_src_c(b);
    const char *root = ca ? concolic_root_c(a) : concolic_root_c(b);

    JSValue exa = ca ? concolic_example(ctx, a) : JS_DupValue(ctx, a);
    JSValue exb = cb ? concolic_example(ctx, b) : JS_DupValue(ctx, b);
    JSValue example = JS_UNDEFINED;
    if (!JS_IsUndefined(exa) && !JS_IsUndefined(exb)) {
        /* STEP 1.c TESTS PRIMITIVES, and both of the ways a non-primitive can arrive here are named because
           they are different bugs with different fixes. This one operand is either a CONCOLIC's example or a
           CONCRETE operand handed straight over by the operator, and each has its own producer. */
        DCHECK(!JS_IsObject(exa) && !JS_IsObject(exb) && !JS_IsSymbol(exa) && !JS_IsSymbol(exb),
               "§13.15.3 step 1.c tests PRIMITIVES — its steps 1.a and 1.b §7.1.1 ToPrimitive both operands "
               "first — and one operand here is not one. From the CONCOLIC side that is a producer having "
               "attached a value this engine never computed: an example rides the value the interpreter "
               "actually produced, so it is an operable primitive. From the CONCRETE side it is the operator "
               "handing over its RAW operand: §7.1.1 there runs the PAGE's valueOf/toString, so it belongs on "
               "the ToPrimitive trampoline (js_toprim_operand / do_toprim_tramp) BEFORE the arm is chosen — "
               "choosing from an unconverted object takes the string arm for `x + {valueOf(){return 5}}`, "
               "which §13.15.3 makes an addition");
        /* §13.15.3 step 1.c: "If leftPrimitive is a String or rightPrimitive is a String". 22.1.3.5 asks no
           such question — its pieces are already ToString'd — so it states the string arm and skips the test. */
        if (op == JS_CONCOLIC_ADD_CONCAT || JS_IsString(exa) || JS_IsString(exb))
            example = example_string_concat(ctx, exa, exb);
        else
            example = example_number_add(ctx, exa, exb);
    }
    JS_FreeValue(ctx, exa); JS_FreeValue(ctx, exb);

    char *ident;
    { const char *f[2]; char *ia = ident_of_operand(ctx, a), *ib = ident_of_operand(ctx, b);
      f[0] = ia; f[1] = ib; ident = concolic_ident_compose("+", f, 2); free(ia); free(ib); }

    JSValue result = concolic_derived(ctx, shape, src, root, ident, example);   /* consumes example and ident */
    free(sha); free(shb); free(shape);
    JS_FreeValue(ctx, a); JS_FreeValue(ctx, b);
    sp[-2] = result;
    return 1;
}

/* TWO SETS, BECAUSE THEY ANSWER TWO DIFFERENT QUESTIONS. What a concolic value DOES once it exists — how it
   adds, compares, coerces, reports its type — is the value class's own semantics, and it must be installed
   wherever a concolic can be reached at all: without it every operator falls through to the ordinary-object
   path and `"x" + document.cookie` throws "toPrimitive" from an expression the page never wrote. Where a
   concolic COMES FROM is a different decision. absent_read_hook mints one out of a global that was never set,
   and out of a field the document's own record does not hold, which is §solver's "server-injected absent state
   is unknown input" — a deliberate exploration choice, and the opposite of what a conformance run wants, where
   an unset global is a ReferenceError and a missing field is `undefined` and the spec says so.
   They were one set, so a host that wanted the first was forced to take the second and every host that
   declined took neither. The conformance runner declined, and paid for it by growing a SECOND Location
   component out of the address to avoid the concolic it could not coerce — which is how a §7.10.5 stringifier
   fix landed in a file that runner does not use. */
static JSConcolicHooks g_hooks = {
    .add = concolic_add_hook, .cmp = concolic_cmp_hook, .is = concolic_is,
    .rel = concolic_rel_hook, .type_of = concolic_typeof_hook,
    .arith = concolic_arith_hook, .to_str = concolic_tostr_hook,
    .key_read = concolic_key_read_hook,
    .key_name = concolic_key_name_hook,
    .builtin = concolic_builtin_hook,
    .example = concolic_example,
    .lead = concolic_lead_hook };

/* Concolic VALUE propagation stays installed across scheduling AND verification, because taint must flow
   during a candidate re-fire too; the EXPLORATION hooks (branch/fork/preempt) are the scheduler's. */
void concolic_install_hooks(void)
{
    JS_SetConcolicHooks(&g_hooks);
}

/* IS THIS HOST EXPLORING? One statement of it, because every consequence of the answer is the same decision:
   an unset global becomes unknown server-injected input rather than a ReferenceError, and a browser value the
   ATTACKER controls becomes a source rather than the plain string the address computed. A host that is not
   exploring gets the browser's own answers, which is what the spec defines and what a conformance run checks.
   It is not a "mode": the value semantics above are installed unconditionally, so a concolic that reaches this
   host still adds, compares and coerces. This decides only whether one is MINTED. */
static int g_source_overlay;
long concolic_source_reads(void) { return g_source_reads; }
/* The same answer, for a component that mints a source of its OWN — see the header. Not folded into
   concolic_source_wrap, because that seam ALSO files the value in the attacker-delivery registry and counts it
   as attacker input acquired: a §4.12.1 data block is neither, and counting one there would report a page that
   read no attacker source as one that did, which is the direction that manufactures a measurement. */
int concolic_is_exploring(void) { return g_source_overlay; }

void concolic_install_source_overlay(void)
{
    DCHECK(g_hooks.add != NULL, "the source overlay was installed over a hook set with no value semantics — a "
                                "source that cannot be added or coerced is a value the page's first expression "
                                "throws on");
    g_source_overlay = 1;
    g_hooks.absent = absent_read_hook;
    /* THE TWO ENDS OF ONE CHANNEL, INSTALLED TOGETHER. `.publish` is what makes the engine mark the records a
       document injects at all, and `.absent` is the only thing that reads those marks; a host that installed
       one without the other would either pay for marks nobody consults or ask about a graph nobody built. */
    g_hooks.publish = absent_publish_hook;
    JS_SetConcolicHooks(&g_hooks);
}

/* THE ONE SEAM between a value the BROWSER computed and the SOLVER's view of it. §CLAUDE splits them exactly
   here: the browser half computes what the spec says the member is — `location.search` IS the address's query,
   and every document has an address — and the solver half decides that an attacker controls it, so it is
   ALSO a symbolic source that forks control flow. Written as one call rather than each component minting a
   concolic itself, because the components that mint one directly are the ones a non-exploring host cannot
   use at all, which is what grew a second Location out of the address.
   `computed` is CONSUMED, and it becomes the source's EXAMPLE: the value is opaque for control flow and still
   knows what it concretely is, which is §solver's whole triple rather than a choice between the two. */
/* AND A DECLARED SOURCE'S TWO HALVES MUST AGREE, ASSERTED AT THE MINT — the invariant this seam exists to hold
   and the one nothing checked. A source is a PROVENANCE (`location.hash`, what an @S record names and what
   concolic_declare_source registers) and a DISPLAY SHAPE (`{location.hash}`, what the @H surface prints as a
   param's value), and every component that declares a browser delivery spells the second as the first in
   braces: location's two, document.cookie, document.referrer, and file_system's `{file:NAME}`. That was five
   independent string literals agreeing by hand.
   THE COST OF NOT ASSERTING IT IS A CONSUMER THAT CANNOT SPELL THE SOURCE, and it has been paid twice. The
   offscreen grew a `{hash}|{search}|{pm}|{reply}` taxonomy against a shape this engine has never emitted (see
   this file's header), and test_forced.c's `loc-hash-param` row asked whether `location.hash` reaches an @H
   param as `{hash}` — a spelling no producer writes — so it read 0 while the shape, the provenance, the `+`
   propagation and the emission were all intact. A row that can only be 0 is worse than a crash: it names a
   mechanism as broken and sends the next reader into it.
   IT IS SCOPED TO DECLARED SOURCES ON PURPOSE. An UNdeclared one legitimately carries a shape that is not its
   name — `{hidden|visible}` is a DOMAIN and `navigator.userAgent` is a member path — and §Solver's rule is that
   a shape states what the value can be. What a DECLARED source additionally owes is a hole the report and the
   PoC can both name, which is exactly `{` src `}`. */
JSValue concolic_source_wrap(JSContext *ctx, const char *shape, const char *src, JSValue computed)
{
    if (!g_source_overlay)
        return computed;
    /* THE ONE POINT AT WHICH THIS DOCUMENT'S RUN ACQUIRES ATTACKER-CONTROLLED INPUT, COUNTED THERE. Every
       component that owns an attacker source mints through this call, so this is the whole of "did the page
       read one", and it is the first of the two facts an empty @S surface collapses. An @S surface with no
       entries has at least four readings and they take opposite actions: the page never read an attacker
       source; it read one and nothing tainted ever reached a code-execution sink; something tainted reached one
       and was suppressed because the check on it was unforgeable; or no sink ran at all. The last three are
       counted at the arrival (solver/solve.h); this is the first, and without it "we looked and there is
       nothing" is spelled exactly like "we never looked".
       AFTER THE OVERLAY GATE, so it counts values MINTED and not calls made. A conformance host installs no
       overlay and this function is then the identity — counting above the gate would report a browser run as
       one that acquired attacker input, which is the direction that manufactures a measurement. */
    g_source_reads++;
#if APICLIENT_DEV
    if (src && concolic_source_encodes(src)) {
        char *hole = shapef("{%s}", src);

        DCHECKF(shape && !strcmp(shape, hole),
                "the attacker source `%s` was minted with the display shape `%s`, and the one its own "
                "declaration spells is `%s`. A declared source's shape is its provenance in braces — that is "
                "what makes it a hole an @H param and an @S envelope can both name — so this is a second "
                "spelling of one fact, and the consumer that reads the other one reports a mechanism as "
                "broken forever. Spell both halves from the component's own token (core/frame/location.h)",
                src, shape ? shape : "(none)", hole);
        free(hole);
    }
#endif
    return concolic_new(ctx, shape, src, computed);
}

/* The permutation that sorts `srcs`, so the composed key is a property of the set. Insertion sort: `n` is a
   component's fact count and never a page's data, and the sort must be over the same order the shapes are then
   joined in or the display and the key would name their members in two different orders. */
static void concolic_joint_order(const char *const *srcs, int n, int *order)
{
    int i, j;

    for (i = 0; i < n; i++) order[i] = i;
    for (i = 1; i < n; i++) {
        int cur = order[i];
        for (j = i; j > 0 && strcmp(srcs[order[j - 1]], srcs[cur]) > 0; j--) order[j] = order[j - 1];
        order[j] = cur;
    }
}

static char *concolic_joint_join(const char *const *parts, const int *order, int n)
{
    size_t seplen = strlen(CONCOLIC_JOINT_SEP), len = 1, at = 0;
    char *out;
    int i;

    for (i = 0; i < n; i++) len += strlen(parts[order[i]]) + (i ? seplen : 0);
    /* SIZED FROM THE MEMBERS RATHER THAN INTO A FIXED BUFFER: a truncated identity is two different domains
       under one key, so a later branch over one would be decided by a branch over the other. */
    out = reclaim_malloc(len);
    CHECK(out, "concolic: OOM composing a JOINT source identity — a value whose domain could not be spelled "
               "would cross to the page as a bare number with every arm behind it deleted");
    for (i = 0; i < n; i++) {
        if (i) { memcpy(out + at, CONCOLIC_JOINT_SEP, seplen); at += seplen; }
        memcpy(out + at, parts[order[i]], strlen(parts[order[i]]));
        at += strlen(parts[order[i]]);
    }
    out[at] = '\0';
    DCHECK(at + 1 == len, "a joint identity was composed to a different length than it was measured for");
    return out;
}

JSValue concolic_source_wrap_joint(JSContext *ctx, const char *const *shapes, const char *const *srcs,
                                   int n, JSValue computed)
{
    int *order;
    char *shape, *src;
    JSValue out;
    int i;

    DCHECK(n >= 1 && shapes != NULL && srcs != NULL,
           "a joint domain was minted over NO members — a value derived from nothing is one the cascade and "
           "the layout determined, and its caller must hand back the computed value rather than ask for a "
           "domain over an empty set");
    if (!g_source_overlay)
        return computed;
    for (i = 0; i < n; i++) {
        DCHECK(shapes[i] != NULL && srcs[i] != NULL,
               "a member of a joint domain arrived with no display shape or no source identity — every member "
               "is one row of its component's own seam, so a NULL is a row that was never filled in");
        DCHECK(strstr(srcs[i], CONCOLIC_JOINT_SEP) == NULL && strstr(shapes[i], CONCOLIC_JOINT_SEP) == NULL,
               "a member's own identity contains the separator a joint identity is composed with, so two "
               "DIFFERENT sets of facts would compose to the same key — and a branch over one would then be "
               "decided by a branch the flow took over the other. Give the composition a separator this "
               "component's identities cannot contain");
    }
    order = reclaim_malloc((size_t)n * sizeof *order);
    CHECK(order, "concolic: OOM ordering a joint domain's members");
    concolic_joint_order(srcs, n, order);
    for (i = 1; i < n; i++)
        DCHECK(strcmp(srcs[order[i - 1]], srcs[order[i]]) < 0,
               "the SAME source appears twice in one joint domain. A set holds each member once, so this key "
               "would count how many times the arithmetic touched a fact rather than name which facts the "
               "value is a function of — and the same dependence assembled by a different route would carry a "
               "different identity and fork a predicate this flow has already decided");
    shape = concolic_joint_join(shapes, order, n);
    src = concolic_joint_join(srcs, order, n);
    out = concolic_source_wrap(ctx, shape, src, computed);
    free(order); free(shape); free(src);
    return out;
}
