/* The flow's pending register — see pending.h. */
#include "solver/pending.h"
#include "solver/cow.h"   /* the register is the SCHEDULER's bookkeeping: no delta may capture a write here */
#include "check.h"

#include <string.h>

/* THE SESSION'S CONTEXT. Every entry point here takes only the register, because the caller that matters has
   nothing else: the per-opcode preempt hook asks whether the running flow is blocked and holds a Flow. Named
   by the host exactly as cow_set_ctx and dom_cow_set_ctx are, and for the same reason. */
static JSContext *g_ctx;

/* THE FIELD NAMES, INTERNED ONCE — interning per read would allocate an atom on the blocked scan. */
static JSAtom g_field[PEND_FIELD_COUNT];
static JSAtom g_len_atom;
static int    g_atoms_ready;

/* WHAT THE FORK DOES WITH EACH FIELD, from the one list in pending.h. */
static const int PEND_COPY_MODE[PEND_FIELD_COUNT] = {
#define PEND_COPY(id, name, copy) copy,
    PENDING_FIELDS(PEND_COPY)
#undef PEND_COPY
};

JSContext *pending_ctx(void)
{
    DCHECK(g_ctx != NULL, "the pending register was reached before pending_set_ctx named the session's "
                          "context — every read and write here uses it");
    return g_ctx;
}
#define pend_ctx() pending_ctx()

static void pend_atoms(void)
{
    static const char *const NAMES[PEND_FIELD_COUNT] = {
#define PEND_NAME(id, name, copy) name,
        PENDING_FIELDS(PEND_NAME)
#undef PEND_NAME
    };
    int i;
    if (g_atoms_ready) return;
    for (i = 0; i < PEND_FIELD_COUNT; i++) g_field[i] = JS_NewAtom(pend_ctx(), NAMES[i]);
    g_len_atom = JS_NewAtom(pend_ctx(), "length");
    g_atoms_ready = 1;
}

/* NAMED BY THE ONE FLOW CONSTRUCTOR, and only the FIRST time. Every flow is born in flow_new, so no host can
   forget this the way a per-host line can be forgotten — which is the same failure the world registry's release
   already had in one of the three. It keeps the FIRST context because that is the agent's root realm: a child
   realm's context can be freed while the frontier that outlives it still holds registers to read. */
void pending_set_ctx(JSContext *ctx) { if (!g_ctx) g_ctx = ctx; }

void pending_free_ctx(JSContext *ctx)
{
    int i;
    if (g_atoms_ready) {
        for (i = 0; i < PEND_FIELD_COUNT; i++) JS_FreeAtom(ctx, g_field[i]);
        JS_FreeAtom(ctx, g_len_atom);
        g_atoms_ready = 0;
    }
    g_ctx = NULL;
}

/* ---- reads ------------------------------------------------------------------------------------------------
   A read is never captured, so none of these brackets anything. They go through JS_GetOwnSlot rather than
   JS_GetProperty for the reason the COW delta does: it is the SLOT — no prototype walk, no accessor, no Proxy
   trap — and these records are null-prototype precisely so a page cannot answer a question the scheduler asks. */

static JSValue pend_own(JSValueConst obj, JSAtom a)
{
    JSValue v;
    if (JS_GetOwnSlot(pend_ctx(), &v, obj, a) <= 0) return JS_UNDEFINED;
    return v;
}

static int pend_len(JSValueConst arr)
{
    JSValue v;
    int n = 0;
    if (!JS_IsObject(arr)) return 0;
    pend_atoms();
    if (JS_GetOwnSlot(pend_ctx(), &v, arr, g_len_atom) <= 0)
        DFAIL("a pending register has no own `length` — it is not the Array this file builds");
    DCHECK(JS_VALUE_GET_TAG(v) == JS_TAG_INT,
           "a pending register's length is not a small integer — the register is this file's own Array and "
           "nothing outside it appends");
    n = JS_VALUE_GET_INT(v);
    JS_FreeValue(pend_ctx(), v);
    return n;
}

int pending_count(JSValueConst reg) { return pend_len(reg); }

JSValue pending_entry(JSValueConst reg, int i)
{
    JSValue e;
    DCHECK(i >= 0 && i < pend_len(reg), "a pending entry was asked for past the end of its register");
    e = JS_GetPropertyUint32(pend_ctx(), reg, (uint32_t)i);
    DCHECK(JS_IsObject(e), "a pending register held something that is not an entry record");
    return e;
}

JSValue pending_get(JSValueConst e, int field)
{
    DCHECK(field >= 0 && field < PEND_FIELD_COUNT, "a pending field was read by an id the record does not have");
    pend_atoms();
    return pend_own(e, g_field[field]);
}

int64_t pending_get_int(JSValueConst e, int field)
{
    JSValue v = pending_get(e, field);
    int64_t n = 0;
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) n = JS_VALUE_GET_INT(v);
    else if (JS_VALUE_GET_TAG(v) == JS_TAG_BOOL) n = JS_VALUE_GET_BOOL(v);
    else JS_ToInt64(pend_ctx(), &n, v);
    JS_FreeValue(pend_ctx(), v);
    return n;
}

int pending_blocked(JSValueConst reg)
{
    int n = pend_len(reg), i;
    for (i = 0; i < n; i++) {
        JSValue e = pending_entry(reg, i);
        int hit = pending_get_int(e, PEND_KIND) == FLOW_PENDING_HOSTREQ &&
                  !pending_get_int(e, PEND_HAVE_VALUE);
        JS_FreeValue(pend_ctx(), e);
        if (hit) return 1;
    }
    return 0;
}

int pending_ready(JSValueConst reg)
{
    int n = pend_len(reg), i;
    for (i = 0; i < n; i++) {
        JSValue e = pending_entry(reg, i);
        int hit = pending_get_int(e, PEND_HAVE_VALUE) != 0;
        JS_FreeValue(pend_ctx(), e);
        if (hit) return 1;
    }
    return 0;
}

/* ---- writes -----------------------------------------------------------------------------------------------
   EVERY ONE OF THEM IS BRACKETED, and that is the invariant this file exists to hold — see pending.h. */

static void pend_put(JSValueConst obj, int field, JSValue v)
{
    int r = JS_DefinePropertyValue(pend_ctx(), obj, g_field[field], v, JS_PROP_C_W_E);
    DCHECK(r >= 0, "a pending record's field could not be defined — the record is a plain null-prototype "
                   "object this file allocated one line earlier");
    (void)r;
}

JSValue pending_push(JSValue *reg, int kind)
{
    JSValue e;

    DCHECK(reg != NULL, "a pending entry was pushed onto no register");
    pend_atoms();
    e = JS_NewObjectProto(pend_ctx(), JS_NULL);
    CHECK(!JS_IsException(e), "engine: OOM allocating a pending record — a dropped request parks its flow "
                              "forever on a reply nothing will ever be asked for");
    cow_engine_write_begin();
    if (!JS_IsObject(*reg)) {
        JS_FreeValue(pend_ctx(), *reg);
        *reg = JS_NewArray(pend_ctx());
        CHECK(!JS_IsException(*reg), "engine: OOM allocating a flow's pending register");
    }
    /* EVERY FIELD, ALWAYS — the fork counts them, so a record short of one is a record the fork cannot check.
       The defaults are each field's "nothing yet": no address, no answer, no rendezvous. */
    pend_put(e, PEND_RESOLVE, JS_UNDEFINED);
    pend_put(e, PEND_VALUE, JS_UNDEFINED);
    pend_put(e, PEND_COMPLETION, JS_UNDEFINED);   /* no answer yet, so no completion type */
    pend_put(e, PEND_URL, JS_NULL);
    pend_put(e, PEND_HAVE_VALUE, JS_FALSE);
    pend_put(e, PEND_KIND, JS_NewInt32(pend_ctx(), kind));
    pend_put(e, PEND_SCRIPT_I, JS_NewInt32(pend_ctx(), -1));
    pend_put(e, PEND_REQ, JS_NewInt64(pend_ctx(), 0));
    pend_put(e, PEND_OP, JS_NULL);
    pend_put(e, PEND_METHOD, JS_NULL);
    pend_put(e, PEND_HEADERS, JS_NULL);
    pend_put(e, PEND_BODY, JS_NULL);
    JS_SetPropertyUint32(pend_ctx(), *reg, (uint32_t)pend_len(*reg), JS_DupValue(pend_ctx(), e));
    cow_engine_write_end();
    return e;
}

void pending_set(JSValueConst e, int field, JSValue v)
{
    DCHECK(field >= 0 && field < PEND_FIELD_COUNT, "a pending field was written by an id the record does not have");
    pend_atoms();
    cow_engine_write_begin();
    pend_put(e, field, v);
    cow_engine_write_end();
}

void pending_set_int(JSValueConst e, int field, int64_t v)
{
    pending_set(e, field, JS_NewInt64(pend_ctx(), v));
}

void pending_set_bytes(JSValueConst e, int field, const void *p, size_t n)
{
    JSValue b = JS_NewArrayBufferCopy(pend_ctx(), (const uint8_t *)p, n);
    CHECK(!JS_IsException(b), "engine: OOM recording a pending request's body");
    pending_set(e, field, b);
}

JSValue pending_list_new(void)
{
    JSValue a = JS_NewArray(pend_ctx());
    CHECK(!JS_IsException(a), "engine: OOM allocating a pending request's header list");
    return a;
}

void pending_list_add_pair(JSValueConst list, const char *name, const char *value)
{
    JSValue pair = JS_NewArray(pend_ctx());
    CHECK(!JS_IsException(pair), "engine: OOM allocating a pending request's header");
    cow_engine_write_begin();
    JS_SetPropertyUint32(pend_ctx(), pair, 0, JS_NewString(pend_ctx(), name));
    JS_SetPropertyUint32(pend_ctx(), pair, 1, JS_NewString(pend_ctx(), value));
    JS_SetPropertyUint32(pend_ctx(), list, (uint32_t)pend_len(list), pair);
    cow_engine_write_end();
}

void pending_remove(JSValue *reg, int i)
{
    int n = pend_len(*reg);
    DCHECK(i >= 0 && i < n, "a pending entry was removed by an index its register does not hold");
    pend_atoms();
    cow_engine_write_begin();
    if (i != n - 1)
        JS_SetPropertyUint32(pend_ctx(), *reg, (uint32_t)i,
                             JS_GetPropertyUint32(pend_ctx(), *reg, (uint32_t)(n - 1)));
    JS_SetProperty(pend_ctx(), *reg, g_len_atom, JS_NewInt32(pend_ctx(), n - 1));
    cow_engine_write_end();
    /* A REGISTER WITH NOTHING IN IT IS NOT A REGISTER, and that is a HOT-PATH statement rather than tidiness.
       flow_blocked is asked at every suspend point the interpreter offers, and its answer for a flow that owes
       nothing has to be a TAG TEST — the C list answered it with `npend == 0`. Left as an empty Array it would
       be a shape lookup for `length` instead, and four dom/ranges tests crossed the gate's 60s CPU budget on
       exactly that difference. */
    if (n == 1) pending_free(pend_ctx(), reg);
}

void pending_free(JSContext *ctx, JSValue *reg)
{
    JS_FreeValue(ctx, *reg);
    *reg = JS_UNDEFINED;
}

/* ---- the fork's copy --------------------------------------------------------------------------------------- */

/* HOW MANY OWN PROPERTIES A RECORD CARRIES — the clone's own check. A record with more fields than the copy
   table names is a field the sibling would not inherit; one with fewer is a record some path built by hand
   instead of through pending_push. It allocates and frees, which a DCHECK condition may do because the
   condition is compiled out ENTIRELY in release — this never runs there. */
static int pend_own_count(JSValueConst e)
{
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0;
    if (JS_GetOwnPropertyNames(pend_ctx(), &tab, &n, e, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) return -1;
    JS_FreePropertyEnum(pend_ctx(), tab, n);
    return (int)n;
}

/* A [name, value] LIST, copied so each arm owns its own. The leaves are SHARED: a JS string is immutable, so a
   reference is the whole copy — which is the difference between this fork and the strdup'd one it replaces. */
static JSValue pend_list_fork(JSValueConst src)
{
    JSValue out;
    int n, i;
    if (!JS_IsObject(src)) return JS_DupValue(pend_ctx(), src);
    n = pend_len(src);
    out = JS_NewArray(pend_ctx());
    CHECK(!JS_IsException(out), "engine: OOM inheriting a pending request's header list at a fork");
    cow_engine_write_begin();
    for (i = 0; i < n; i++) {
        JSValue pair = JS_GetPropertyUint32(pend_ctx(), src, (uint32_t)i);
        JSValue cp = JS_NewArray(pend_ctx());
        CHECK(!JS_IsException(cp), "engine: OOM inheriting a pending request's header at a fork");
        JS_SetPropertyUint32(pend_ctx(), cp, 0, JS_GetPropertyUint32(pend_ctx(), pair, 0));
        JS_SetPropertyUint32(pend_ctx(), cp, 1, JS_GetPropertyUint32(pend_ctx(), pair, 1));
        JS_SetPropertyUint32(pend_ctx(), out, (uint32_t)i, cp);
        JS_FreeValue(pend_ctx(), pair);
    }
    cow_engine_write_end();
    return out;
}

/* ONE RECORD, COPIED. Used where an arm must hold a field the other arm may not see — which is exactly one
   field and exactly one kind, the unanswered synchronous request's rendezvous id. */
static JSValue pend_entry_copy(JSValueConst src)
{
    JSValue dst;
    int f;

    /* The field names, HERE and not left to a caller's DCHECK to have interned them — a condition compiled out
       in release is not an initialisation. */
    pend_atoms();
    dst = JS_NewObjectProto(pend_ctx(), JS_NULL);

    CHECK(!JS_IsException(dst), "engine: OOM copying a pending record");
    DCHECK(pend_own_count(src) == PEND_FIELD_COUNT,
           "a pending record carries a field PENDING_FIELDS does not name — the copy would leave it behind, "
           "and the arm that reads it would see `undefined`, which is a real value belonging to the request; "
           "every field is an obligation at the push, at this copy and at the free, and this is the copy's half");
    cow_engine_write_begin();
    for (f = 0; f < PEND_FIELD_COUNT; f++) {
        JSValue v = pend_own(src, g_field[f]);
        if (PEND_COPY_MODE[f] == PEND_STRUCT) {
            JSValue c = pend_list_fork(v);
            JS_FreeValue(pend_ctx(), v);
            v = c;
        }
        pend_put(dst, f, v);
    }
    cow_engine_write_end();
    return dst;
}

/* THE SIBLING'S REGISTER SHARES THE PARENT'S RECORDS, and copies only the ARRAY that names them.
 *
 * A record is IMMUTABLE from the moment it is pushed, with one exception: the ANSWER (`value`/`haveValue`),
 * which every arm waiting on that request genuinely observes — engine_provide already filled EVERY copy that
 * named the URL, and the resolve capability was already shared between the arms (its already_resolved latch
 * and the promise's settlement are per-flow state the COW delta captures, which is what lets both arms settle
 * one capability). So two arms holding one record see exactly what two arms holding two identical copies saw,
 * and the fork stops paying for the duplicate: on the minimal fixture the boot flow's eight parked fetches
 * were being re-materialised into 112484 records across 14062 forks.
 * The register itself is still per-flow and must be: each arm removes an entry when IT delivers, and the host
 * walks every flow's register from outside any flow's delta. */
JSValue pending_fork(JSValueConst reg)
{
    JSValue out;
    int n, i;

    if (!JS_IsObject(reg)) return JS_UNDEFINED;   /* the flow never parked on anything: nothing to inherit */
    pend_atoms();
    n = pend_len(reg);
    out = JS_NewArray(pend_ctx());
    CHECK(!JS_IsException(out), "engine: OOM inheriting the pending replies at a fork");
    cow_engine_write_begin();
    for (i = 0; i < n; i++)
        JS_SetPropertyUint32(pend_ctx(), out, (uint32_t)i, pending_entry(reg, i));
    cow_engine_write_end();
    return out;
}

JSValue pending_unshare(JSValueConst reg, int i)
{
    JSValue src = pending_entry(reg, i);
    JSValue dst = pend_entry_copy(src);
    JS_FreeValue(pend_ctx(), src);
    cow_engine_write_begin();
    JS_SetPropertyUint32(pend_ctx(), reg, (uint32_t)i, JS_DupValue(pend_ctx(), dst));
    cow_engine_write_end();
    return dst;
}

/* ---- the census row ----------------------------------------------------------------------------------------
   WHAT THIS REGISTER COSTS A PAGER, in quickjs's bytes rather than the host allocator's — which is the point of
   the conversion. A string is counted by the LENGTH the register names, because the same string is very often
   one the page already holds and one every sibling of this flow shares.
   IT IS DELIBERATELY THE PER-FLOW FIGURE, records included, and that now OVERSTATES what the frontier holds:
   since pending_fork shares records, N arms naming one record are counted N times here. That is what a pager
   which cannot yet NAME the sharing would actually write, so it is the right number for this row until the
   cold tier grows a shared-record row the way cow.c's frozen chain already has one. The real live figure is
   the allocator's (`cLiveKiB` on the @HEAP line) and it is where the sharing shows up. */

static long pend_str_bytes(JSValueConst v)
{
    size_t len = 0;
    const char *s;
    if (!JS_IsString(v)) return 0;
    s = JS_ToCStringLen(pend_ctx(), &len, v);
    if (s) JS_FreeCString(pend_ctx(), s);
    return (long)len + 1;
}

long pending_bytes(JSValueConst reg)
{
    long total = 0;
    int n = pend_len(reg), i;
    for (i = 0; i < n; i++) {
        JSValue e = pending_entry(reg, i);
        int f;
        total += (long)PEND_FIELD_COUNT * (long)sizeof(JSValue);   /* the record's own slots */
        for (f = 0; f < PEND_FIELD_COUNT; f++) {
            JSValue v = pending_get(e, f);
            total += pend_str_bytes(v);
            if (f == PEND_BODY) {
                size_t bl = 0;
                if (JS_GetArrayBuffer(pend_ctx(), &bl, v)) total += (long)bl;
            } else if (f == PEND_HEADERS && JS_IsObject(v)) {
                int hn = pend_len(v), h;
                for (h = 0; h < hn; h++) {
                    JSValue pair = JS_GetPropertyUint32(pend_ctx(), v, (uint32_t)h);
                    JSValue a = JS_GetPropertyUint32(pend_ctx(), pair, 0);
                    JSValue b = JS_GetPropertyUint32(pend_ctx(), pair, 1);
                    total += pend_str_bytes(a) + pend_str_bytes(b) + 2 * (long)sizeof(JSValue);
                    JS_FreeValue(pend_ctx(), a); JS_FreeValue(pend_ctx(), b); JS_FreeValue(pend_ctx(), pair);
                }
            }
            JS_FreeValue(pend_ctx(), v);
        }
        JS_FreeValue(pend_ctx(), e);
    }
    return total;
}
