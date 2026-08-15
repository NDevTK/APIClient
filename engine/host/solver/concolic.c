/* The concolic value type — see concolic.h. A host component built on upstream quickjs's PUBLIC class API, so
   the qjs fork carries no value-type delta. */
#include "solver/concolic.h"
#include "solver/absent.h"
#include "solver/flow.h"
#include "check.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* The per-value state hung off the JSObject via JS_SetOpaque. */
typedef struct {
    char *shape;        /* @H/@S display form */
    char *src;          /* source identity (constraint correlation key) */
    JSValue example;    /* concrete example, or JS_UNDEFINED */
    int cmp_op;         /* for a COMPARISON RESULT: OPCMP_EQ/NE — `src <op> cmp_tok` (else OPCMP_NONE) */
    char *cmp_tok;      /* the concrete side of the comparison */
} Concolic;

/* THE PER-FLOW PATH CONSTRAINT. One map, keyed by SOURCE IDENTITY, carrying every fact this flow has learned
   about the unknown input it read — which is the whole of what a DART/SAGE-lineage constraint is here, because
   concrete execution grounds all shared and interprocedural state and only the INPUT variables are ever
   symbolic. Two facts, because two things narrow a domain:
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
    int i;
    g_pins_hash_cap = 16; while (g_pins_hash_cap < g_pins_n * 2) g_pins_hash_cap *= 2;
    g_pins_hash = realloc(g_pins_hash, (size_t)g_pins_hash_cap * sizeof(int));
    CHECK(g_pins_hash, "concolic: OOM path-constraint index");
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
        g_pins_cap = g_pins_cap ? g_pins_cap * 2 : 8;
        g_pins = realloc(g_pins, (size_t)g_pins_cap * sizeof(Cons));
        CHECK(g_pins, "concolic: OOM path constraint");
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
    PinBlob *b = malloc(sizeof *b);
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
        ConsSeg *s = malloc(sizeof *s);
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
    PinBlob *b = malloc(sizeof *b);
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

/* THE DECLARED SOURCES and what their component does to an attacker's bytes. Small and fixed: a source that
   declares nothing delivers as-is, which is right for injected server state (`window.__STATE`) — the attacker
   writes that JSON directly and no component transforms it. */
typedef struct { char *src; char *encode; char prefix; SourceDeliverKind deliver; } SourceDelivery;
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

void concolic_declare_source(const char *src, const char *encode, char prefix, SourceDeliverKind deliver)
{
    int i;
    CHECK(src != NULL, "a source was declared with no identity");
    /* THE TWO HALVES OF ONE DECLARATION MUST AGREE. A value carried in the victim's own address has the
       component it is carried at, and a value carried by any other mechanism has none — so a `#` beside a
       `plant`, or an `address` with no component, is a declaration whose reproduction cannot be built from it
       and whose candidate delivery would prepend a character no browser puts there. */
    DCHECK((deliver == SRC_DELIVER_ADDRESS) == (prefix != 0),
           "a source's delivery mechanism and its address component disagree — only a value carried in the "
           "victim's own address has a component, and it always has one");
    for (i = 0; i < g_srcs_n; i++)
        if (!strcmp(g_srcs[i].src, src)) {
            DCHECK(0, "a source declared its browser delivery twice — one component owns one source");
            return;
        }
    if (g_srcs_n == g_srcs_cap) {
        int c = g_srcs_cap ? g_srcs_cap * 2 : 8;
        SourceDelivery *a = realloc(g_srcs, (size_t)c * sizeof *a);
        CHECK(a != NULL, "concolic: OOM declaring a source's delivery");
        g_srcs = a; g_srcs_cap = c;
    }
    g_srcs[g_srcs_n].src = strdup(src);
    g_srcs[g_srcs_n].encode = strdup(encode ? encode : "");
    g_srcs[g_srcs_n].prefix = prefix;
    g_srcs[g_srcs_n].deliver = deliver;
    CHECK(g_srcs[g_srcs_n].src && g_srcs[g_srcs_n].encode, "concolic: OOM declaring a source's delivery");
    g_srcs_n++;
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
   the browser hands the page, which is the only thing re-execution can honestly decide a breakout against. */
static JSValue concolic_deliver(JSContext *ctx, const char *src, const char *payload)
{
    const SourceDelivery *d = NULL;
    const unsigned char *p;
    char *out;
    size_t o = 0, n;
    int i;

    if (!payload) payload = "";
    for (i = 0; i < g_srcs_n; i++)
        if (src && !strcmp(g_srcs[i].src, src)) { d = &g_srcs[i]; break; }
    if (!d) return JS_NewString(ctx, payload);   /* an undeclared source is delivered as itself */

    n = strlen(payload);
    out = malloc(n * 3 + 2);
    CHECK(out != NULL, "concolic: OOM delivering a candidate");
    if (d->prefix) out[o++] = d->prefix;
    for (p = (const unsigned char *)payload; *p; p++) {
        /* C0 controls and DEL are percent-encoded by every URL component, so they are not in any declared set. */
        if (*p < 0x20 || *p == 0x7F || strchr(d->encode, (char)*p)) {
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
    if (!c) return JS_UNDEFINED;
    const char *field = JS_AtomToCString(ctx, atom);
    char shape[192];
    snprintf(shape, sizeof shape, "%s.%s", c->shape ? c->shape : "{}", field ? field : "?");
    if (field) JS_FreeCString(ctx, field);
    if (g_cand_src && !strcmp(shape, g_cand_src))            /* candidate run: this source -> the concrete breakout */
        return concolic_deliver(ctx, shape, g_cand_payload);
    const char *pv = pin_of(shape);                          /* an EQ gate pinned this source -> the real value */
    if (pv) return JS_NewString(ctx, pv);
    return concolic_new(ctx, shape, shape, JS_UNDEFINED);    /* src = the field path (precise @S identity) */
}

/* Mint a COMPARISON-RESULT concolic bool carrying `src <op> tok`, so `if (x === 'admin')` forks (instead of a
   concrete false) and the taken arm can pin/negate. */
JSValue concolic_new_cmp(JSContext *ctx, const char *src, int op, const char *tok) {
    JSValue r = concolic_new(ctx, "{cmp}", src, JS_UNDEFINED);
    Concolic *c = JS_GetOpaque(r, g_concolic_class);
    if (c) { c->cmp_op = op; c->cmp_tok = tok ? strdup(tok) : NULL; }
    return r;
}
int concolic_cmp(JSValueConst v, const char **psrc, const char **ptok) {
    Concolic *c = g_concolic_class ? JS_GetOpaque(v, g_concolic_class) : NULL;
    if (!c || c->cmp_op == OPCMP_NONE) return OPCMP_NONE;
    if (psrc) *psrc = c->src; if (ptok) *ptok = c->cmp_tok;
    return c->cmp_op;
}

/* JSConcolicCmpHook for == / === : a concolic operand -> a concolic bool carrying {src, op, tok}. Matches the
   slow-eq stack effect (both operands freed, result in sp[-2]). is_neq flips EQ->NE. */
int concolic_cmp_hook(JSContext *ctx, JSValue *sp, int is_neq) {
    JSValue a = sp[-2], b = sp[-1];
    int ca = concolic_is(a), cb = concolic_is(b);
    if (!ca && !cb) return 0;
    JSValueConst opq = ca ? a : b, other = ca ? b : a;
    const char *src = concolic_src_c(opq);
    char *tok = NULL;
    if (!concolic_is(other)) { const char *s = JS_ToCString(ctx, other); if (s) { tok = strdup(s); JS_FreeCString(ctx, s); } }
    JSValue res = concolic_new_cmp(ctx, src, is_neq ? OPCMP_NE : OPCMP_EQ, tok);
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
    char shape[224];
    (void)this_val; (void)argc; (void)argv; (void)flags;
    DCHECK(c != NULL, "the concolic call handler ran on something that is not a concolic value");
    snprintf(shape, sizeof shape, "%s()", c->shape ? c->shape : "{}");
    return concolic_new(ctx, shape, shape, JS_UNDEFINED);
}

/* ORDERING over an unknown is unknown. < <= > >= coerce with ToPrimitive, which a concolic cannot satisfy —
   it is an object whose coercion answers with another concolic — so the operator threw TypeError and took the
   whole program with it: `document.cookie.indexOf("role=admin") >= 0` explored NEITHER arm, losing both the
   session path and the anonymous one. The result is a concolic BOOL, so the branch forks exactly as an equality
   gate does; it carries no {op,tok} constraint because an ordering does not PIN a value the way `=== 'admin'`
   does — it narrows a domain, and the arm that is taken says which way. */
int concolic_rel_hook(JSContext *ctx, JSValue *sp, int op) {
    JSValue a = sp[-2], b = sp[-1];
    int ca = concolic_is(a), cb = concolic_is(b);
    if (!ca && !cb) return 0;
    (void)op;
    {
        const char *src = concolic_src_c(ca ? a : b);
        JSValue res = concolic_new(ctx, "{cmp}", src ? src : "{cmp}", JS_UNDEFINED);
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
    char shape[192];
    const char *src;
    JSValue example = JS_UNDEFINED, res;

    if (!ca && !cb) return 0;
    name = carith_name(op, &unary);
    src = ca ? concolic_src_c(a) : concolic_src_c(b);

    if (unary) {
        snprintf(shape, sizeof shape, "%s%s", name, concolic_shape_c(a) ? concolic_shape_c(a) : "{}");
    } else {
        char *sa = ca ? strdup(concolic_shape_c(a) ? concolic_shape_c(a) : "{}") : cstr_dup(ctx, a);
        char *sb = cb ? strdup(concolic_shape_c(b) ? concolic_shape_c(b) : "{}") : cstr_dup(ctx, b);
        CHECK(sa && sb, "concolic arithmetic: OOM shape");
        snprintf(shape, sizeof shape, "%s%s%s", sa, name, sb);
        free(sa); free(sb);
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

    res = concolic_new(ctx, shape, src ? src : shape, example);
    JS_FreeValue(ctx, sp[-nops]);
    if (nops == 2) JS_FreeValue(ctx, sp[-1]);
    sp[-nops] = res;
    return 1;
}

/* 7.1.17 ToString over unknown input: unknown, source kept, example computed by actually stringifying the
   example when there is one. */
JSValue concolic_tostr_hook(JSContext *ctx, JSValueConst v) {
    const char *src, *sh;
    char shape[192];
    JSValue ex, example = JS_UNDEFINED;

    if (!concolic_is(v)) return JS_UNINITIALIZED;
    src = concolic_src_c(v);
    sh = concolic_shape_c(v);
    snprintf(shape, sizeof shape, "String(%s)", sh ? sh : "{}");
    ex = concolic_example(ctx, v);
    if (!JS_IsUndefined(ex)) {
        const char *p = JS_ToCString(ctx, ex);
        if (p) { example = JS_NewString(ctx, p); JS_FreeCString(ctx, p); }
    }
    JS_FreeValue(ctx, ex);
    return concolic_new(ctx, shape, src ? src : shape, example);
}

/* A BUILTIN OVER AN UNKNOWN OPERAND — see the hook's contract in quickjs.h. The shape records WHICH operation
   produced it, so an @H shape reads as the expression the page actually wrote and an @S search knows which
   source to solve for. Example-free: this engine does not yet run the operation on the operand's example (a
   regex match over a known query string HAS a concrete answer, and producing it is the next step here), and
   inventing one would be a fabricated observation. */
JSValue concolic_builtin_hook(JSContext *ctx, JSValueConst v, const char *op, JSValue example) {
    const char *src, *sh;
    char shape[224];

    if (!concolic_is(v)) { JS_FreeValue(ctx, example); return JS_UNINITIALIZED; }
    src = concolic_src_c(v);
    sh = concolic_shape_c(v);
    snprintf(shape, sizeof shape, "%s.%s()", sh ? sh : "{}", op ? op : "builtin");
    /* `example` is what the operator got by RUNNING THE REAL OPERATION on this operand's own example. It is
       never computed here and never predicted: the codec really encoded, the parser really parsed. */
    return concolic_new(ctx, shape, src ? src : shape, example);
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
    const char *src;
    char shape[192];

    if (!concolic_is(key)) return JS_UNINITIALIZED;
    src = concolic_src_c(key);
    snprintf(shape, sizeof shape, "{}[%s]", concolic_shape_c(key) ? concolic_shape_c(key) : "{}");
    (void)obj;
    return concolic_new(ctx, shape, src ? src : shape, JS_UNDEFINED);
}

JSValue concolic_typeof_hook(JSContext *ctx, JSValueConst v) {
    const char *src;
    char shape[192];
    if (!concolic_is(v)) return JS_UNINITIALIZED;
    src = concolic_src_c(v);
    snprintf(shape, sizeof shape, "typeof %s", src ? src : "{}");
    return concolic_new(ctx, shape, shape, JS_UNDEFINED);
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

void concolic_free(JSContext *ctx) { (void)ctx; /* class lives with the runtime; per-value state freed by the finalizer */ }

JSValue concolic_new(JSContext *ctx, const char *shape, const char *src, JSValue example) {
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
        return concolic_deliver(ctx, src, g_cand_payload);
    }
    DCHECK(g_concolic_class != 0, "concolic_new before concolic_init — the class is unregistered");
    JSValue obj = JS_NewObjectClass(ctx, g_concolic_class);
    if (JS_IsException(obj)) { JS_FreeValue(ctx, example); return obj; }
    Concolic *c = calloc(1, sizeof *c);
    CHECK(c, "concolic_new: OOM allocating value state — a dropped concolic corrupts the flow's domain");
    c->shape = strdup(shape ? shape : "{}");
    c->src = src ? strdup(src) : NULL;
    c->example = example;   /* consume */
    JS_SetOpaque(obj, c);
    return obj;
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
    char *shape = malloc(ln); CHECK(shape, "concolic +: OOM shape concat");
    snprintf(shape, ln, "%s%s", sha, shb);
    const char *src = ca ? concolic_src_c(a) : concolic_src_c(b);

    JSValue exa = ca ? concolic_example(ctx, a) : JS_DupValue(ctx, a);
    JSValue exb = cb ? concolic_example(ctx, b) : JS_DupValue(ctx, b);
    JSValue example = JS_UNDEFINED;
    if (!JS_IsUndefined(exa) && !JS_IsUndefined(exb)) {
        const char *pa = JS_ToCString(ctx, exa), *pb = JS_ToCString(ctx, exb);
        if (pa && pb) { size_t l = strlen(pa) + strlen(pb) + 1; char *e = malloc(l); if (e) { snprintf(e, l, "%s%s", pa, pb); example = JS_NewString(ctx, e); free(e); } }
        if (pa) JS_FreeCString(ctx, pa); if (pb) JS_FreeCString(ctx, pb);
    }
    JS_FreeValue(ctx, exa); JS_FreeValue(ctx, exb);

    JSValue result = concolic_new(ctx, shape, src, example);   /* consumes example */
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
JSValue concolic_source_wrap(JSContext *ctx, const char *shape, const char *src, JSValue computed)
{
    if (!g_source_overlay)
        return computed;
    return concolic_new(ctx, shape, src, computed);
}
