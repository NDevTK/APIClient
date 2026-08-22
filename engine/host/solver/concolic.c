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

/* A CONCRETE OPERAND'S IDENTITY IS ITS VALUE, AND ITS TYPE IS PART OF THAT — `x === 5` and `x === "5"` are two
   predicates and their operands print the same. An OBJECT or a SYMBOL has no identity this engine can spell
   (an object's is its address, which does not survive the park a resumed flow replays through, and coercing it
   would run the page's own `toString` from C), so it answers absent and both arms of the branch stay. */
static char *literal_ident(JSContext *ctx, JSValueConst v)
{
    const char *tag, *s;
    const char *f[2];
    char *r;

    if (JS_IsString(v))         tag = "s";
    else if (JS_IsNumber(v))    tag = "n";
    else if (JS_IsBool(v))      tag = "b";
    else if (JS_IsNull(v))      tag = "z";
    else if (JS_IsUndefined(v)) tag = "u";
    else if (JS_IsBigInt(v))    tag = "g";
    else return NULL;
    s = JS_ToCString(ctx, v);
    if (!s) return NULL;
    f[0] = tag; f[1] = s;
    r = concolic_ident_compose("k", f, 2);
    JS_FreeCString(ctx, s);
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
   Both are per-flow and travel together, which is why they are ONE entry rather than two maps that a fork,
   a suspend and a resume would each have to remember to carry. */
typedef struct { char *key; char *val; signed char truth; } Cons;

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
    g_pins[g_pins_n].truth = below ? below->truth : -1;
    g_pins_n++;
    if (!g_pins_hash || g_pins_hash_cap < g_pins_n * 2) cons_hash_rebuild();
    else cons_hash_put(g_pins_n - 1);
    DCHECK(cons_index_find(g_pins, g_pins_hash, g_pins_hash_cap, key) == g_pins_n - 1,
           "a constraint entry is not findable through the index that was just given it — a later read of the "
           "same fact would fork a branch this flow has already decided");
    return &g_pins[g_pins_n - 1];
}
void concolic_pin(const char *src, const char *val) {
    Cons *c = cons_entry(src);
    free(c->val); c->val = strdup(val); CHECK(c->val, "concolic: OOM pin value");
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
        for (i = 0; i < s->n; i++) { free(s->e[i].key); free(s->e[i].val); }
        free(s->e); free(s->hash); free(s);
        s = base;
    }
}

void concolic_clear_pins(void) {
    int i;
    for (i = 0; i < g_pins_n; i++) { free(g_pins[i].key); free(g_pins[i].val); }
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
                s->bytes += (long)strlen(s->e[k].key) + 1 + (s->e[k].val ? (long)strlen(s->e[k].val) + 1 : 0);
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
typedef struct { const char *component; char *src; char *encode; char prefix; SourceDeliverKind deliver; }
        SourceDelivery;
static SourceDelivery *g_srcs;
static int g_srcs_n, g_srcs_cap;

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
#if APICLIENT_DEV
            static char msg[400];
            snprintf(msg, sizeof msg,
                     "a source declared its browser delivery twice — `%s` is already declared by %s, and one "
                     "component owns one source in BOTH directions. A second declaration by that same "
                     "component is a claimant with dynamic rows that did not ask concolic_source_declared_by "
                     "first; a declaration by any other is two claimants over one row, whichever of them "
                     "releases first taking a row it does not own", src, g_srcs[i].component);
            DFAIL(msg);
#endif
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

int concolic_source_delivery(const char *src, const char **kind, char *prefix)
{
    int i;

    DCHECK(kind != NULL && prefix != NULL, "a source's declared delivery was asked for with nowhere to put it");
    for (i = 0; i < g_srcs_n; i++)
        if (src && !strcmp(g_srcs[i].src, src)) {
            *kind = deliver_token(g_srcs[i].deliver);
            *prefix = g_srcs[i].prefix;
            return 1;
        }
    return 0;
}

const char *concolic_source_encodes(const char *src)
{
    int i;
    for (i = 0; i < g_srcs_n; i++)
        if (src && !strcmp(g_srcs[i].src, src)) return g_srcs[i].encode;
    return NULL;
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

/* Exotic [[Get]]: reading ANY field of a concolic value yields a DERIVED concolic — unknown injected/attacker
   state is unknown per-field, carrying the FIELD-PATH identity ("{state}.admin"), which doubles as the source
   identity for @S injection. This is what lets a gated `if (state.admin)` fork AND lets an @S candidate inject
   at a precise source. */
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
    r = concolic_alloc(ctx, shape, shape, c->root, ident, JS_UNDEFINED);
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
                        int eq_kind, const char *tok)
{
    const char *f[3];
    char *ident;
    JSValue r;
    Concolic *c;

    DCHECK(op != NULL,
           "a comparison result was minted with no OPERATOR. The operator is half the predicate: without it "
           "`x < 700` and `x > 700` compose to one identity, the flow's record of either DECIDES the other, "
           "and the arm it deletes is one nothing contradicts");
    DCHECK(eq_kind == OPCMP_NONE || tok != NULL,
           "a comparison result declares that its taken arm PINS but names no value to pin to — the two are "
           "written together at the mint and read together by concolic_cmp");
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
    return r;
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
    return pred_new(ctx, op == OPCMP_NE ? "!=" : "==", src, src,
                    concolic_ident_compose("s", sf, 1), concolic_ident_compose("k", kf, 2), op, tok);
}
int concolic_cmp(JSValueConst v, const char **psrc, const char **ptok) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    if (!c || c->cmp_op == OPCMP_NONE) return OPCMP_NONE;
    if (psrc) *psrc = c->src; if (ptok) *ptok = c->cmp_tok;
    return c->cmp_op;
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
    char *tok = NULL, *iu, *io;
    JSValue res;

    if (!ca && !cb) return 0;
    opq = ca ? a : b; other = ca ? b : a;
    src = concolic_src_c(opq);
    root = concolic_root_c(opq);   /* THE SAME OPERAND, or the assert at concolic_alloc has two facts about two values */
    if (!concolic_is(other)) { const char *s = JS_ToCString(ctx, other); if (s) { tok = strdup(s); JS_FreeCString(ctx, s); } }
    /* EQUALITY IS SYMMETRIC, so `x === 'a'` and `'a' === x` compose to ONE identity: the unknown operand is
       written first, and where BOTH are unknown the two identities are ordered between themselves. Without
       that the same predicate written the other way round would be a second fact and fork a second time —
       which costs work but is sound; the ordering is what makes the sound answer also the cheap one. */
    iu = ident_of_operand(ctx, opq);
    io = ident_of_operand(ctx, other);
    if (ca && cb && iu && io && strcmp(iu, io) > 0) { char *t = iu; iu = io; io = t; }
    res = pred_new(ctx, is_neq ? "!=" : "==", src, root, iu, io,
                   tok ? (is_neq ? OPCMP_NE : OPCMP_EQ) : OPCMP_NONE, tok);
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
   are two predicates and neither may answer the other. */
int concolic_rel_hook(JSContext *ctx, JSValue *sp, int op) {
    JSValue a = sp[-2], b = sp[-1];
    int ca = concolic_is(a), cb = concolic_is(b);
    char opid[24];
    int w;

    if (!ca && !cb) return 0;
    /* THE OPERATOR ARRIVES AS THE ENGINE'S OWN OPCODE FOR IT (see the hook's contract in quickjs.h), which is
       an identity and not a name. Nothing here interprets it — an ordering never pins, so the four operators
       need only stay TOLD APART — and it is composed as a field like any other. */
    w = snprintf(opid, sizeof opid, "rel%d", op);
    DCHECK(w > 0 && (size_t)w < sizeof opid, "a relational operator's id did not fit its own buffer");
    (void)w;
    {
        JSValueConst opq = ca ? a : b;
        JSValue res = pred_new(ctx, opid, concolic_src_c(opq), concolic_root_c(opq),
                               ident_of_operand(ctx, a), ident_of_operand(ctx, b), OPCMP_NONE, NULL);
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

/* 7.1.4 ToNumber AND 7.1.17 ToString OVER UNKNOWN INPUT — answered where the operator computes, never at the
 * conversion boundary, which owes C a real primitive.
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
    default: DFAIL("concolic arithmetic with an unknown operator id"); return "?";
    }
}

static int carith_apply(int op, double a, double b, double *out)
{
    switch (op) {
    case JS_CARITH_NEG:  *out = -a; return 1;
    case JS_CARITH_PLUS: *out = a; return 1;
    case JS_CARITH_NOT:  *out = (double)(~(int32_t)a); return 1;
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

int concolic_arith_hook(JSContext *ctx, JSValue *sp, int op, int nops) {
    JSValue a = sp[-nops], b = nops == 2 ? sp[-1] : JS_UNDEFINED;
    int ca = concolic_is(a), cb = nops == 2 && concolic_is(b);
    const char *name;
    int unary = 0;
    char *shape, *ident;
    const char *src, *root;
    JSValue example = JS_UNDEFINED, res;

    if (!ca && !cb) return 0;
    name = carith_name(op, &unary);
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
        if (!JS_IsUndefined(exa) && (nops == 1 || !JS_IsUndefined(exb))) {
            double da = 0, db = 0, out = 0;
            if (JS_ToFloat64(ctx, &da, exa) == 0 &&
                (nops == 1 || JS_ToFloat64(ctx, &db, exb) == 0) &&
                carith_apply(op, da, db, &out))
                example = JS_NewFloat64(ctx, out);
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

/* 7.1.17 ToString over unknown input: unknown, source kept, example computed by actually stringifying the
   example when there is one. */
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

void concolic_init(JSContext *ctx) {
    JSRuntime *rt = JS_GetRuntime(ctx);
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
    if (g_srcs_n != 0) {
        static char msg[400];

        snprintf(msg, sizeof msg,
                 "%s did not give back the attacker SOURCE `%s` before the solver's agent state was released. "
                 "Each row is a claim in this component's array whose claimant releases it at its own "
                 "concolic_undeclare_sources, on core/platform.h's release column, which platform_agent_free "
                 "runs before this call. A row left behind is not only a leaked pair of strings: it describes "
                 "a browser delivery for a document that is gone, and it is what the next agent's declaration "
                 "of the same source collides with", g_srcs[0].component, g_srcs[0].src);
        DFAIL(msg);
    }
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

/* JSConcolicAddHook: `a + b` where a or b is concolic -> a DERIVED concolic. shape = display(a)++display(b)
   (a concrete operand contributes its string, a concolic its shape); example = the concrete concat when BOTH
   sides have a concrete value/example, else absent (unknown input stays unknown). Matches js_add_slow's stack
   effect: both operands freed, result in sp[-2]. */
int concolic_add_hook(JSContext *ctx, JSValue *sp) {
    JSValue a = sp[-2], b = sp[-1];
    int ca = concolic_is(a), cb = concolic_is(b);
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
        const char *pa = JS_ToCString(ctx, exa), *pb = JS_ToCString(ctx, exb);
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
        if (pa) JS_FreeCString(ctx, pa); if (pb) JS_FreeCString(ctx, pb);
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
   concolic COMES FROM is a different decision. absent_global_hook mints one out of a global that was never
   set, which is §solver's "server-injected absent state is unknown input" — a deliberate exploration choice,
   and the opposite of what a conformance run wants, where an unset global is a ReferenceError and the spec
   says so.
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

void concolic_install_source_overlay(void)
{
    DCHECK(g_hooks.add != NULL, "the source overlay was installed over a hook set with no value semantics — a "
                                "source that cannot be added or coerced is a value the page's first expression "
                                "throws on");
    g_source_overlay = 1;
    g_hooks.absent = absent_global_hook;
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
#if APICLIENT_DEV
    if (src && concolic_source_encodes(src)) {
        char *hole = shapef("{%s}", src);
        static char msg[400];

        snprintf(msg, sizeof msg,
                 "the attacker source `%s` was minted with the display shape `%s`, and the one its own "
                 "declaration spells is `%s`. A declared source's shape is its provenance in braces — that is "
                 "what makes it a hole an @H param and an @S envelope can both name — so this is a second "
                 "spelling of one fact, and the consumer that reads the other one reports a mechanism as "
                 "broken forever. Spell both halves from the component's own token (core/frame/location.h)",
                 src, shape ? shape : "(none)", hole);
        DCHECK(shape && !strcmp(shape, hole), msg);
        free(hole);
    }
#endif
    return concolic_new(ctx, shape, src, computed);
}

/* THE JOINT DOMAIN — see concolic.h for what reaches it, why the identity is the SET and not an expression over
   it, and why a narrowing of the joint narrows no member.
   THE SEPARATOR IS PART OF THE IDENTITY, so a member that contained it would let two different sets compose to
   one key — the truncation defect in a different costume, and asserted here rather than trusted. */
#define CONCOLIC_JOINT_SEP " & "

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
