/* The flow's pending register — see pending.h. */
#include "solver/pending.h"
#include "solver/cow.h"   /* the register is the SCHEDULER's bookkeeping: no delta may capture a write here */
/* THE FRONTIER'S SIDE OF THE SAME SET. Every mutation of a register runs in this file (pending.h: "one place,
   because every mutation is in this file"), which is exactly why the frontier-wide set of OUTSTANDING records
   is maintained from here and from nowhere else — a park site asked to remember a second call is the list
   beside a list that PENDING_FIELDS' default column exists to abolish. */
#include "solver/pending_index.h"
#include "check.h"
#include "core/loader/script_type.h"   /* PEND_SCRIPT_TYPE's "nothing yet": §4.12.1.1's null type */

#include <string.h>

/* THE SESSION'S CONTEXT. Every entry point here takes only the register, because the caller that matters has
   nothing else: the per-opcode preempt hook asks whether the running flow is blocked and holds a Flow. Named
   by the host exactly as cow_set_ctx and dom_cow_set_ctx are, and for the same reason. */
static JSContext *g_ctx;

/* THE FIELD NAMES, INTERNED ONCE — interning per read would allocate an atom on every register read there is. */
static JSAtom g_field[PEND_FIELD_COUNT];
static JSAtom g_len_atom;
/* AND THE ONE THING THE REGISTER SAYS ABOUT ITSELF: how many unanswered SYNCHRONOUS requests it holds. See
   pending.h at `pending_blocked` for why this one classification may be counted and the rest may not.
   IT LIVES ON THE REGISTER ARRAY AND NOT IN A C TABLE KEYED BY ITS POINTER, and that is not a shortcut past
   plumbing — it is the plumbing. The count and the entries it counts then have ONE lifetime and ONE fork:
   there is no create to pair with a destroy, no key to go stale when `pending_free` drops the array, and no
   way for the two to be separated by a path that handles one and not the other. It is scheduler bookkeeping
   and not platform data, so §State-isolation's obligation on it is only that no delta capture it — which is
   the same bracket every other write in this file already runs inside. */
static JSAtom g_sync_owed_atom;
static int    g_atoms_ready;

/* WHAT THE FORK DOES WITH EACH FIELD, from the one list in pending.h. */
static const int PEND_COPY_MODE[PEND_FIELD_COUNT] = {
#define PEND_COPY(id, name, copy, dflt) copy,
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
#define PEND_NAME(id, name, copy, dflt) name,
        PENDING_FIELDS(PEND_NAME)
#undef PEND_NAME
    };
    int i;
    if (g_atoms_ready) return;
    for (i = 0; i < PEND_FIELD_COUNT; i++) g_field[i] = JS_NewAtom(pend_ctx(), NAMES[i]);
    g_len_atom = JS_NewAtom(pend_ctx(), "length");
    g_sync_owed_atom = JS_NewAtom(pend_ctx(), "syncOwed");
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
    /* AND THE FRONTIER-WIDE SET BUILT OUT OF THESE REGISTERS, released here for the atoms' reason exactly: it
       holds a counted reference to every outstanding record, so an index left standing across a runtime
       boundary is a handle into a freed heap that the next session's first reply would write through. Its own
       closing assert is that nothing is left in it — see pending_index_reset. */
    pending_index_reset(ctx);
    if (g_atoms_ready) {
        for (i = 0; i < PEND_FIELD_COUNT; i++) JS_FreeAtom(ctx, g_field[i]);
        JS_FreeAtom(ctx, g_len_atom);
        JS_FreeAtom(ctx, g_sync_owed_atom);
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

/* HOW MANY ENTRIES THIS REGISTER HOLDS — and `CHECK` for the reason its sibling `pend_sync_owed` gives one
   paragraph down, which applies here MORE strongly rather than less. Both asserts used to be dev-only, and the
   release continuation was not an abort and not a fallback: `JS_GetOwnSlot` writes JS_UNDEFINED into `*pval`
   before it looks (quickjs.c, its first statement), whose int payload is 0, so a register with no `length`
   answered EMPTY — and so did one whose length is not a small integer, `JS_VALUE_GET_INT` reading the union's
   int32 field whatever tag is on it. A plausible datum where no measurement exists, which is §Architecture's
   defaulted-field defect with the default supplied by the accessor instead of by an author.
   WHAT 0 DOES HERE IS NOT A WRONG READING, IT IS A LOST FLOW, and that is what makes proceeding worse than
   aborting in production. Every walk in this file is `for (i = 0; i < pend_len(reg); i++)`, so an empty answer
   makes every entry the register holds invisible: `pending_ready` reports nothing to deliver and the flow
   parked on that request is never resumed, while `pending_count_kind` reports no debt of any kind. Worse, the
   count is also where the next entry LANDS — `JS_SetPropertyUint32(reg, pend_len(reg), e)` — so a register
   reading 0 overwrites index 0 on every append, dropping one outstanding request per new one. §check.h names a
   dropped flow corrupting the frontier as the archetype of an always-fatal invariant, and this drops them
   silently and forever: the frontier is never reset, so the parked flows a release session loses this way are
   lost across every session that resumes from it.
   IT IS THIS FILE'S OWN INVARIANT AND NOTHING OUTSIDE CAN BREAK IT. The register is the plain Array allocated
   here, null-prototype so a page cannot answer for it, and `length` is present from its first instant — so a
   violation is a register something outside this file created, not input. */
static int pend_len(JSValueConst arr)
{
    JSValue v;
    int n = 0;
    if (!JS_IsObject(arr)) return 0;
    pend_atoms();
    CHECK(JS_GetOwnSlot(pend_ctx(), &v, arr, g_len_atom) > 0,
          "engine: a pending register has no own `length` — it is not the Array this file builds. Reading past "
          "this would answer EMPTY for a register that holds outstanding requests, so every flow parked on one "
          "of them is never resumed and the next append overwrites index 0");
    CHECK(JS_VALUE_GET_TAG(v) == JS_TAG_INT,
          "engine: a pending register's length is not a small integer — the register is this file's own Array "
          "and nothing outside it appends, so the int this would read out of another tag's payload is not a "
          "count of anything and decides both which entries are walked and where the next one lands");
    n = JS_VALUE_GET_INT(v);
    JS_FreeValue(pend_ctx(), v);
    return n;
}

/* HOW MANY UNANSWERED SYNCHRONOUS REQUESTS THIS REGISTER HOLDS — the ONE fact `pending_blocked` is a predicate
   over, read without touching an entry. 0 for a register that has never held anything, which is JS_UNDEFINED
   and answers with a tag test, exactly as `pend_len` does and for the same reason.
   `CHECK` AND NOT `DCHECK`, AND THIS DIFF IS WHAT MADE THAT SO. The value is load-bearing in RELEASE — the
   per-opcode preempt hook decides on it there — so a DCHECK would be compiled out in precisely the build where
   the read happens, and what it guards is not a crash but a PLAUSIBLE ANSWER: `JS_GetOwnSlot` writes
   JS_UNDEFINED into `*pval` before it looks, so a missing slot reads back as 0 through JS_VALUE_GET_INT and
   the register says "this flow is not blocked" for the rest of the session. That is §Architecture's
   defaulted-field defect exactly, with the default supplied by the accessor rather than by an author, and its
   consequence here is a flow spinning on an answer the host only gives between scheduler steps. Every register
   is built in this file with the slot present from its first instant, so the absence is this file's own
   invariant broken and must not be proceeded past in production either. */
static int pend_sync_owed(JSValueConst reg)
{
    JSValue v;
    int n;

    if (!JS_IsObject(reg)) return 0;
    pend_atoms();
    CHECK(JS_GetOwnSlot(pend_ctx(), &v, reg, g_sync_owed_atom) > 0,
          "engine: a pending register carries no count of the synchronous requests it is owed — every register "
          "is built in solver/pending.c with that slot present, so its absence is a register something outside "
          "this file created, and the preempt hook would read a BLOCKED flow as runnable and spin it on a "
          "question the host only answers between scheduler steps");
    CHECK(JS_VALUE_GET_TAG(v) == JS_TAG_INT,
          "engine: a pending register's count of the synchronous requests it is owed is not a small integer — "
          "this file is that slot's only writer and writes an int at every one of them");
    n = JS_VALUE_GET_INT(v);
    JS_FreeValue(pend_ctx(), v);
    return n;
}

/* …AND EVERY SITE THAT MOVES IT, IN ONE PLACE. Stated as an ABSOLUTE rather than a delta, so a caller cannot
   pass a new value computed from a read this file did not make. Bracketed like every other register write
   (pending.h: no delta may capture this register); the brackets nest, so a caller already inside one pays
   nothing for this. */
static void pend_sync_owed_set(JSValueConst reg, int n)
{
    int r;

    DCHECK(JS_IsObject(reg), "a synchronous-request count was written onto something that is not a register");
    DCHECK(n >= 0, "a pending register was told it is owed a NEGATIVE number of synchronous requests — the "
                   "count is one per unanswered entry of that kind, so this is a second decrement for one "
                   "entry, or a decrement for an entry this register never held");
    pend_atoms();
    cow_engine_write_begin();
    r = JS_DefinePropertyValue(pend_ctx(), reg, g_sync_owed_atom, JS_NewInt32(pend_ctx(), n), JS_PROP_C_W_E);
    cow_engine_write_end();
    DCHECK(r >= 0, "a pending register's synchronous-request count could not be defined — the register is the "
                   "plain Array this file allocated");
    (void)r;
}

int pending_count(JSValueConst reg) { return pend_len(reg); }

/* …AND HOW MANY OF ONE KIND — see pending.h. Written here beside the whole count so the two are read together:
   a caller that wants "what does this register owe" almost always wants it per kind, because the two debts are
   settled through different doors. */
int pending_count_kind(JSValueConst reg, int kind)
{
    int n = pend_len(reg), i, c = 0;

    for (i = 0; i < n; i++) {
        JSValue e = pending_entry(reg, i);
        if (pending_get_int(e, PEND_KIND) == kind) c++;
        JS_FreeValue(pend_ctx(), e);
    }
    return c;
}

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

/* IS THE HOST OWED THIS ENTRY — the ONE condition the three questions below are made of, and the same one
   engine_pending_fetches and engine_host_requests skip a record on. An entry carries a value or it does not;
   what DIFFERS between the callers is which kinds they are asking about, never what "owed" means. Written once
   here so the pager's credit and the list the host is actually shown cannot drift apart, which is the defect
   pending.h describes at pending_owed_replies. Cheap by construction — one own-slot read of a boolean — and
   the reason that matters has MOVED rather than gone: it used to be that `pending_blocked` asked it at every
   suspend point the interpreter offers, and that path no longer reads an entry at all. What still reads it is
   every walking predicate below (`pending_ready`, `pending_deliverable_count`, `pending_outstanding`,
   `pending_outstanding_kind`, `pending_owed_replies`), each of which pays this once per ENTRY over a register
   that only grows — so the cost of this line is multiplied by a number nothing bounds, which is the residual
   pending.h names at `pending_ready`. */
static int pend_owed(JSValueConst e)
{
    return !pending_get_int(e, PEND_HAVE_VALUE);
}

/* …AND THE HALF OF IT THE HOST CAN STILL ACT ON, WHICH IS A DIFFERENT QUESTION AND USED TO BE THE SAME ONE.
   The paragraph above is right that "owed" has one meaning; what it did not have was a second thing to ask,
   and a decline is that second thing. An entry the trusted zone has REFUSED is still unanswered — its flow is
   parked at the line that asked, so `pending_outstanding` above must go on saying so or the flow reads as
   FINISHED and its whole timeline is torn down — and the host is nonetheless owed NOTHING for it: no reply is
   coming, the joins do not list it, and a debt credited for it could only ever be spent excusing some LATER
   reply the host genuinely mispaired (pending.h says exactly that at pending_owed_replies).
   So the two diverge here and nowhere else: "is this flow still waiting" is `pend_owed`, and "can the host
   still be asked" is this. */
static int pend_host_owed(JSValueConst e)
{
    return pend_owed(e) && !pending_entry_declined(e);
}

int pending_entry_declined(JSValueConst e)
{
    JSValue d = pending_get(e, PEND_DECLINED);
    int hit;

    /* THE REASON OR NOTHING, AND NOTHING IS A POSITIVE STATEMENT. JS_NULL is "this zone has refused nothing
       about this request"; a STRING is the refusal, and it carries the sentence that explains it. A third
       shape would be a caller writing this field from outside engine_decline, so it crashes rather than
       being read as either answer. */
    DCHECK(JS_IsNull(d) || JS_IsString(d),
           "a pending record's refusal is neither the JS_NULL that means `nothing has been refused` nor the "
           "reason string a refusal is — engine_decline is the field's one writer and it writes the trusted "
           "zone's own words, so a third shape is a write from somewhere that does not hold a refusal");
    hit = JS_IsString(d);
    JS_FreeValue(pend_ctx(), d);
    return hit;
}

#if APICLIENT_DEV
/* THE SCAN THE COUNT REPLACED, KEPT AS THE ORACLE AND NEVER AS A PATH. Nothing returns its answer: it is only
   ever compared against the count, at the two sites that already walk the whole register anyway — the fork and
   the release — so there is no predicate anywhere choosing between two implementations of one question
   (§A-superseded-system-is-DELETED-in-the-same-diff), and the audit costs nothing that was not already paid.
   IT IS DELIBERATELY NOT CALLED FROM `pending_blocked`. engine.c makes the identical statement about the value
   yield's membership assert — "membership is an O(flows) scan and the read is per-opcode, so putting it there
   would make the dev build's hot path linear in the frontier" — and this is that rule about this register: an
   audit ON the hook's read would rebuild in dev exactly the walk this removes, so the dev and release builds
   would no longer be the same scheduler. */
static int pend_sync_owed_walk(JSValueConst reg)
{
    int n = pend_len(reg), i, c = 0;

    for (i = 0; i < n; i++) {
        JSValue e = pending_entry(reg, i);
        if (pending_get_int(e, PEND_KIND) == FLOW_PENDING_HOSTREQ && pend_owed(e)) c++;
        JS_FreeValue(pend_ctx(), e);
    }
    return c;
}

/* WHICH SLOT OF THIS REGISTER NAMES THIS RECORD, or -1 — the half of `pending_answer_sync`'s pairing that only
   the register can confirm, since a record carries no way back to the registers that name it (which is the
   whole reason that door takes one). O(entries) and affordable for exactly the reason the walk above is: it
   runs once per synchronous answer and never on the hook's path. */
static int pend_slot_of(JSValueConst reg, JSValueConst rec)
{
    int n = pend_len(reg), i;

    for (i = 0; i < n; i++) {
        JSValue e = pending_entry(reg, i);
        int hit = JS_VALUE_GET_PTR(e) == JS_VALUE_GET_PTR(rec);
        JS_FreeValue(pend_ctx(), e);
        if (hit) return i;
    }
    return -1;
}
#endif

int pending_blocked(JSValueConst reg) { return pend_sync_owed(reg) > 0; }

/* CAN flow_deliver_one_reply TAKE THIS ENTRY — the one condition the two questions below are made of, written
   once here for the reason `pend_owed` above is: what differs between the callers is whether they want to know
   THAT there is one or HOW MANY there are, never what "deliverable" means, and a second spelling of it would let
   the arm's guard and the arm's debt disagree about the same register.
   THE KIND IS HALF THE QUESTION — see pending.h. A HOSTREQ's answer belongs to the machine that is parked at the
   call site which asked for it; flow_deliver_one_reply never delivers one, so a register holding nothing else is
   not ready and its flow must go on waiting for its own resume rather than be sent to a delivery that would take
   the answer away from it. */
static int pend_deliverable(JSValueConst e)
{
    return pending_get_int(e, PEND_HAVE_VALUE) != 0 &&
           pending_get_int(e, PEND_KIND) != FLOW_PENDING_HOSTREQ;
}

int pending_ready(JSValueConst reg)
{
    int n = pend_len(reg), i;
    for (i = 0; i < n; i++) {
        JSValue e = pending_entry(reg, i);
        int hit = pend_deliverable(e);
        JS_FreeValue(pend_ctx(), e);
        if (hit) return 1;
    }
    return 0;
}

/* HOW MANY DELIVERIES THIS REGISTER STILL OWES ITS OWN FLOW — see pending.h for what it is for. Deliberately
   NOT short-circuited: `pending_ready` answers a guard and this answers a DEBT, and a debt that stopped at the
   first member would be the level whose collapse this counter exists to end. */
int pending_deliverable_count(JSValueConst reg)
{
    int n = pend_len(reg), i, c = 0;

    for (i = 0; i < n; i++) {
        JSValue e = pending_entry(reg, i);
        if (pend_deliverable(e)) c++;
        JS_FreeValue(pend_ctx(), e);
    }
    return c;
}

int pending_outstanding(JSValueConst reg)
{
    int n = pend_len(reg), i;
    for (i = 0; i < n; i++) {
        JSValue e = pending_entry(reg, i);
        int hit = pend_owed(e);
        JS_FreeValue(pend_ctx(), e);
        if (hit) return 1;
    }
    return 0;
}

int pending_outstanding_kind(JSValueConst reg, int kind)
{
    int n = pend_len(reg), i;
    for (i = 0; i < n; i++) {
        JSValue e = pending_entry(reg, i);
        int hit = pending_get_int(e, PEND_KIND) == kind && pend_owed(e);
        JS_FreeValue(pend_ctx(), e);
        if (hit) return 1;
    }
    return 0;
}

int pending_owed_replies(JSValueConst reg)
{
    int n = pend_len(reg), i, c = 0;

    for (i = 0; i < n; i++) {
        JSValue e = pending_entry(reg, i);
        int kind = (int)pending_get_int(e, PEND_KIND);
        /* `pend_host_owed` AND NOT `pend_owed`: A DECLINED PARK IS A REPLY THAT CANNOT ARRIVE. The zone has
           said it will not ask, so crediting a debt for it would hand the surplus to the next reply the host
           genuinely mispaired — which is the exact defect the paragraph below and pending.h both describe,
           reached through the one entry kind for which no reply exists at all. */
        if (kind != FLOW_PENDING_HOSTREQ && pend_host_owed(e)) {
            /* A DEBT IS A REPLY THAT CAN STILL ARRIVE, AND ONLY THE PAIR MAKES ONE ARRIVE. engine_pending_fetches
               lists `METHOD<TAB>DESTINATION<TAB>INITIATOR<TAB>PROVENANCE<TAB>URL` and engine_provide delivers
               against the pair, so an owed
               entry missing either is one the host was never shown and never will be — counting it credits a reply nobody is
               going to send, and the credit is then spent by a reply the host genuinely mispaired. Asserted at
               the origin of the DEBT rather than on the register's hot path: pending_blocked runs at every
               suspend point, this runs once per flow the pager sells.
               The OTHER half of the same contract — that an owed entry the host cannot be shown must not exist
               at all — is engine_host_owes's `tellable`; this is the same statement said about the pair the
               delivery is keyed on rather than about the address alone. */
#if APICLIENT_DEV
            JSValue uv = pending_get(e, PEND_URL);
            JSValue mv = pending_get(e, PEND_METHOD);
            int pair = JS_IsString(uv) && JS_IsString(mv);
            JS_FreeValue(pend_ctx(), mv);
            JS_FreeValue(pend_ctx(), uv);
            DCHECK(pair,
                   "a flow is owed a reply for an entry that names no (METHOD, URL) pair — the reply seam is "
                   "keyed on the pair, so no host can ever answer this entry, and crediting it at a sale hands "
                   "the reply door a credit that the next genuinely mispaired reply will spend");
#endif
            c++;
        }
        JS_FreeValue(pend_ctx(), e);
    }
    return c;
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

/* THE OWN-PROPERTY COUNT, DECLARED HERE AND DEFINED BESIDE THE CLONE THAT WAS ITS FIRST READER. It is the one
   thing this file asserts about a record's SHAPE, and it is asked at both ends of a record's life — where it is
   built and where it is copied — so it is declared before the first of them rather than moved to sit above it.
   Side-effect-free (it enumerates and frees the enumeration), which is what lets a DCHECK's condition call it. */
static int pend_own_count(JSValueConst e);

/* See pending.h. TWO FACTS, TWO OWNERS, ONE COMPOSITION — and the conjunction is the whole content: a
   parser-inserted park is a request a real load makes ONLY while the path that produced the row has stood on
   no contradicted arm, because a `document.write` on a forced arm produces a parser-inserted row of that
   flow's own sequence and of nothing else. Taking either fact alone reports the other one's answer. */
int pending_prov_compose(int kind, int path_forced)
{
    DCHECK(path_forced == 0 || path_forced == 1,
           "a park stated a forced-path mark that is neither set nor clear — it comes from flow_path_forced, "
           "which asserts the same thing at the other end, so a third value is a caller that computed this "
           "somewhere else");
    DCHECK(kind >= FLOW_PENDING_RESOLVE && kind <= FLOW_PENDING_MODULE,
           "a park stated a kind this register does not define — the provenance is composed from it, so an "
           "unknown kind would be answered by whichever arm of the test below happens to be the else");
    if (path_forced) return PROV_FORCED;
    return kind == FLOW_PENDING_DOCSCRIPT ? PROV_OBSERVED : PROV_DERIVED;
}

JSValue pending_push(JSValue *reg, int kind, int path_forced)
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
        /* AND ITS OWN COUNT OF WHAT BLOCKS ITS FLOW, PRESENT FROM THE ARRAY'S FIRST INSTANT. A register that
           acquired this slot lazily would answer the per-opcode hook from a MISSING property until something
           happened to write one, which is a hole a reader fills in rather than a producer stating "none yet"
           (§Architecture). Zero is the positive statement, and `pend_sync_owed`'s CHECK is what makes the
           absence a crash instead of an answer. */
        pend_sync_owed_set(*reg, 0);
    }
    /* EVERY FIELD, ALWAYS, AND FROM THE ONE LIST — the fork counts them, so a record short of one is a record
       the fork cannot check. This was a hand-written sequence of `pend_put` calls in this file, which is a
       second list beside PENDING_FIELDS, and it went out of step exactly as a second list does: `scriptEl` was
       added to the table, to the copy and to the census and never here, so every record ever pushed carried
       one field fewer than PEND_FIELD_COUNT for many commits. The check written for that is pend_entry_copy's
       own-property count, and it could not see it — a record is only COPIED at a fork, and the fork that
       reaches an answered synchronous request needs a peer's SECOND answer to be deliverable, which nothing
       could do until the answers began naming their timelines. A latent hole plus an unreachable check reads
       exactly like a healthy subsystem, which is why the default now lives in the table with the field and this
       loop cannot be short of one. */
#define PEND_DEFAULT(id, name, copy, dflt) pend_put(e, PEND_##id, dflt);
    PENDING_FIELDS(PEND_DEFAULT)
#undef PEND_DEFAULT
    /* AND THE RECORD ASSERTS ITS OWN COMPLETENESS WHERE IT IS BORN. pend_entry_copy already makes this claim
       at the clone, and its message names all three sites — "every field is an obligation at the push, at this
       copy and at the free, and this is the copy's half". This is the PUSH's half, and the two are not
       redundant: the clone's fires in whichever subsystem happens to fork first, arbitrarily far from the
       record's construction, while this one names the constructor. A record can still fail it — a path that
       builds an entry by hand, or a field written onto one from outside this file — and the expansion above is
       what makes the ordinary way of failing it impossible. */
    DCHECK(pend_own_count(e) == PEND_FIELD_COUNT,
           "a pending record was born carrying a number of fields PENDING_FIELDS does not name — every field is "
           "an obligation at the push, at the clone and at the census, and a record short of one hands the arm "
           "that reads it `undefined`, which is a real value belonging to the request");
    JS_SetPropertyUint32(pend_ctx(), *reg, (uint32_t)pend_len(*reg), JS_DupValue(pend_ctx(), e));
    /* …AND THE REGISTER LEARNS THAT ITS FLOW IS NOW BLOCKED, at the ONE site an entry of that kind is born.
       A record is pushed unanswered by construction (PENDING_FIELDS' default column gives `haveValue` JS_FALSE),
       so the push is unconditionally a debt and needs to read nothing back off the record it just built. */
    if (kind == FLOW_PENDING_HOSTREQ) pend_sync_owed_set(*reg, pend_sync_owed(*reg) + 1);
    cow_engine_write_end();
    /* AND THE FRONTIER'S SET LEARNS OF IT AT THE PUSH, WHICH IS BEFORE IT CAN BE SHARED. Tracking is what
       carries the count of registers naming this record, so it has to begin at the ONE moment exactly one
       does — a fork adds the rest. The synchronous request kind is refused: it is keyed by a request id and
       not by a pair (pending.h), so a pair-keyed set has no place to put one and would answer one seam's
       question through the other. */
    if (kind != FLOW_PENDING_HOSTREQ) pending_index_track(e);
    return e;
}

/* WHAT THE FRONTIER'S SET MUST BE TOLD ABOUT A FIELD WRITE, IN ONE PLACE — three fields decide membership and
   the rest decide nothing, so this is a switch and not a call at each of the twenty writes upstairs.
   A RECORD IS BORN WITHOUT ITS IDENTITY. pending_push creates every field at its "nothing yet" and the park
   then states the address and the method one write at a time, so a tracked record has NO PAIR until the second
   of the two arrives; that is the record being built and not a state to default past. It is keyed on the write
   that completes it, and it leaves for good on the write that answers it. */
static void pend_index_sync(JSValueConst e, int field)
{
    JSValue uv, mv;

    if (field != PEND_URL && field != PEND_METHOD && field != PEND_HAVE_VALUE &&
        field != PEND_DECLINED) return;
    if (!pending_index_tracked(e)) return;   /* answered, refused, synchronous, or dropped by every register */
    /* A REFUSAL TAKES THE RECORD OUT OF THE SET THE HOST IS LOOKED UP IN, and this arm is asked BEFORE the
       `haveValue` one below because the two pay different counters and only one of them is right for a request
       no reply is coming for.
       WHAT ITS ABSENCE COST, SAID HERE BECAUSE THIS SWITCH IS WHERE IT WAS ABSENT. `declined` was not among the
       fields this function reacts to, so a record the trusted zone had REFUSED stayed a keyed member of its
       pair — and the pair is what `engine_provide` delivers against. The next reply for that address therefore
       filled a record already carrying a refusal, and the flow woke holding both: told the server answered AND
       that nobody asked it, which are opposite directions. It surfaced two seams away, at flow_decline_fork's
       "a request carries BOTH a reply and a refusal", which can say the two raced and cannot say which write
       should never have happened. This is the write that should never have happened, refused at its origin.
       IT LEAVES THE PAIR INDEX AND IT STAYS OUTSTANDING, AND THOSE ARE TWO SETS. This one answers "can the
       host still be asked"; `pend_owed` above answers "is this flow still waiting", and the refused flow IS
       still waiting — parked at the line that asked, owed the failure arm §@S requires. Collapsing them would
       read the flow as FINISHED and tear its whole timeline down, which is why `pend_host_owed` exists and why
       the assert below stands on the field this arm does not touch. */
    if (field == PEND_DECLINED) {
        DCHECK(pending_entry_declined(e),
               "the frontier's outstanding set was told about a `declined` write that is not a REFUSAL — the "
               "field's one writer is engine_decline and it writes the zone's own words, so a write leaving "
               "the record un-refused is a path trying to UN-decline one, and this arm would take the record "
               "out of the set the host is looked up in while every register goes on listing it as refused");
        DCHECK(pending_index_keyed(e),
               "a request carrying no (METHOD, ADDRESS) pair was refused — the host is shown a request only "
               "once its identity is complete, so a refusal of one that was never askable answers a question "
               "this frontier never put, and engine_decline reaches records only through the pair they are "
               "keyed under");
        DCHECK(pend_owed(e),
               "a record was refused while it already carries a reply — `haveValue` says the server answered "
               "and `declined` says nobody asked it, so the flow parked on it would run its failure path over "
               "a body it already holds. engine_decline asserts the same pair at the write it makes; this is "
               "the index's half, at the instant the record would leave the set the reply seam is keyed on");
        pending_index_declined(e);
        return;
    }
    if (pending_get_int(e, PEND_HAVE_VALUE)) { pending_index_answered(e); return; }
    /* A REQUEST'S IDENTITY IS WRITTEN ONCE, AT THE PARK, and the set is keyed on it — so a second write of
       either half would leave this index answering for an address that is no longer on the record, and the
       flow parked on the new one would wait for the rest of the session. Asserted here rather than trusted,
       because the write that would break it is an ordinary-looking `pending_set` in some other file. */
    DCHECK(!pending_index_keyed(e),
           "a request's METHOD or ADDRESS was rewritten after the frontier's outstanding set was keyed on it — "
           "the reply seam is keyed on the pair, so the set would go on offering the old identity to the host "
           "and the reply for the new one would match nothing");
    uv = pending_get(e, PEND_URL);
    mv = pending_get(e, PEND_METHOD);
    if (JS_IsString(uv) && JS_IsString(mv)) {
        const char *u = JS_ToCString(pend_ctx(), uv);
        const char *m = JS_ToCString(pend_ctx(), mv);
        CHECK(u != NULL && m != NULL, "engine: OOM naming an outstanding request");
        pending_index_key(e, m, u);
        JS_FreeCString(pend_ctx(), m);
        JS_FreeCString(pend_ctx(), u);
    }
    JS_FreeValue(pend_ctx(), mv);
    JS_FreeValue(pend_ctx(), uv);
}

/* THE WRITE ITSELF, WITH NO QUESTION ASKED ABOUT WHICH FIELD IT IS — the two doors below both end here, so a
   field is defined and the frontier's set is told about it in ONE place whichever door was used. */
static void pend_set_field(JSValueConst e, int field, JSValue v)
{
    DCHECK(field >= 0 && field < PEND_FIELD_COUNT, "a pending field was written by an id the record does not have");
    pend_atoms();
    cow_engine_write_begin();
    pend_put(e, field, v);
    cow_engine_write_end();
    /* OUTSIDE THE BRACKET, because the set is not the flow's state and no delta may capture it (pending_index.h
       states the same rule this file's own header states for the register, one step stronger). */
    pend_index_sync(e, field);
}

void pending_set(JSValueConst e, int field, JSValue v)
{
    /* THE ONE WRITE THIS DOOR REFUSES, AND THE REFUSAL IS WHAT KEEPS THE COUNT HONEST. Settling a SYNCHRONOUS
       request changes what `pending_blocked` answers, and that answer is held by the REGISTER — which this
       door does not have and cannot find, because a record carries no way back to the registers naming it.
       So the transition has its own door (pending_answer_sync) that is HANDED the register, and this one
       crashes rather than doing half of it: a synchronous answer written here would settle the entry while
       the count went on saying the flow is blocked, and the pick would skip that flow for the rest of the
       session with nothing anywhere to say so. */
    DCHECK(field != PEND_HAVE_VALUE || pending_get_int(e, PEND_KIND) != FLOW_PENDING_HOSTREQ,
           "a SYNCHRONOUS host request was answered through the generic field door — the register holds the "
           "count that decides whether its flow is blocked and this door has only the record, so the entry "
           "would be settled while the flow went on reading as blocked and was skipped at every pick. Write it "
           "through pending_answer_sync, which is handed the register");
    pend_set_field(e, field, v);
}

void pending_set_int(JSValueConst e, int field, int64_t v)
{
    pending_set(e, field, JS_NewInt64(pend_ctx(), v));
}

void pending_answer_sync(JSValueConst reg, JSValueConst e)
{
    int owed;

    DCHECK(pending_get_int(e, PEND_KIND) == FLOW_PENDING_HOSTREQ,
           "a record that is not a synchronous host request was answered through the synchronous door — this "
           "door takes one off the register's count of unanswered synchronous requests, so a record of any "
           "other kind would spend a debt nothing put there and leave a genuinely blocked flow reading as "
           "runnable, which is the busy spin engine_host_request asserts against at the ask");
    DCHECK(!pending_get_int(e, PEND_HAVE_VALUE),
           "a synchronous request that already carries an answer was answered again — the first answer took "
           "this register's count down, so a second takes it down for an entry that is no longer on it");
#if APICLIENT_DEV
    DCHECK(pend_slot_of(reg, e) >= 0,
           "a synchronous answer was written through a register that does not hold the record — the register "
           "is the half of this transition the record cannot state, so it is CARRIED here and checked here. A "
           "wrong one settles the entry while the flow actually parked on it keeps its count, and that flow is "
           "then skipped at every pick for the rest of the session");
#endif
    owed = pend_sync_owed(reg);
    DCHECK(owed > 0,
           "a register was told one of its synchronous requests has been answered while it counts none "
           "outstanding — the count is raised at the push of every entry of that kind and lowered only here "
           "and at the removal of an unanswered one, so this is one transition that reached the register twice");
    pend_set_field(e, PEND_HAVE_VALUE, JS_TRUE);
    pend_sync_owed_set(reg, owed - 1);
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

/* ---- the answers beyond the first ---------------------------------------------------------------------------
   See PEND_EXTRA in pending.h. They are TUPLES in the same shape the header list uses, deliberately:
   pend_list_fork below copies a tuple list by its ARITY, so the field a fork must give each arm its own
   container of needs no second copier — and a second copier is exactly where a container that must diverge
   quietly stays shared. */

void pending_extra_add(JSValueConst e, int completion, JSValue value, const char *world)
{
    JSValue list, pair;

    /* THE TIMELINE IS NOT OPTIONAL ON A SECOND ANSWER. A second answer EXISTS only because the peer has more
       than one timeline, so an unnamed one is the answer of a document whose state is its flows with the flow
       left out — and the arm forked over it could then never say which of the peer's timelines it is in. */
    DCHECK(world != NULL && *world,
           "a peer's further answer was recorded without the timeline that computed it — a second answer is a "
           "second timeline by definition, so one that names none cannot be told from the first answer arriving "
           "twice, and the arm forked over it would belong to no timeline of the answering document");
    pend_atoms();
    list = pend_own(e, g_field[PEND_EXTRA]);
    cow_engine_write_begin();
    if (!JS_IsObject(list)) {
        JS_FreeValue(pend_ctx(), list);
        list = JS_NewArray(pend_ctx());
        CHECK(!JS_IsException(list),
              "engine: OOM recording a peer timeline's answer — a dropped one is a whole timeline of the "
              "answering document that the asking flow will never fork an arm for, and nothing says so");
        pend_put(e, PEND_EXTRA, JS_DupValue(pend_ctx(), list));
    }
    pair = JS_NewArray(pend_ctx());
    CHECK(!JS_IsException(pair), "engine: OOM recording a peer timeline's answer");
    /* THE TYPE AND THE VALUE ARE ONE WRITE, the same rule PEND_COMPLETION keeps beside PEND_VALUE: a peer
       answers by RUNNING A PROGRAM and a program that threw completed just as truly as one that returned. */
    JS_SetPropertyUint32(pend_ctx(), pair, 0, JS_NewInt32(pend_ctx(), completion));
    JS_SetPropertyUint32(pend_ctx(), pair, 1, value);
    /* …AND SO IS THE TIMELINE, for the same reason and in the same bracket. */
    JS_SetPropertyUint32(pend_ctx(), pair, 2, JS_NewString(pend_ctx(), world));
    JS_SetPropertyUint32(pend_ctx(), list, (uint32_t)pend_len(list), pair);
    cow_engine_write_end();
    JS_FreeValue(pend_ctx(), list);
}

/* HAS THIS PEER TIMELINE ALREADY ANSWERED — the first answer's own world and then every triple. Read-only and
   allocation-free apart from the string compare, because engine_host_answer asks it on the delivery path of
   every cross-instance answer there is. */
int pending_answer_world_seen(JSValueConst e, const char *world)
{
    JSValue w, list;
    int seen = 0, i, n;

    DCHECK(world != NULL && *world, "a request was asked whether an UNNAMED timeline had answered it — the "
                                    "question is only meaningful about a named one");
    pend_atoms();
    w = pend_own(e, g_field[PEND_ANSWER_WORLD]);
    if (JS_IsString(w)) {
        const char *s = JS_ToCString(pend_ctx(), w);
        CHECK(s != NULL, "engine: OOM reading the timeline that answered a cross-instance request");
        seen = !strcmp(s, world);
        JS_FreeCString(pend_ctx(), s);
    }
    JS_FreeValue(pend_ctx(), w);
    if (seen) return 1;
    list = pend_own(e, g_field[PEND_EXTRA]);
    n = JS_IsObject(list) ? pend_len(list) : 0;
    for (i = 0; i < n && !seen; i++) {
        JSValue tuple = JS_GetPropertyUint32(pend_ctx(), list, (uint32_t)i);
        JSValue tw = JS_GetPropertyUint32(pend_ctx(), tuple, 2);
        const char *s;
        DCHECK(JS_IsString(tw), "a recorded peer answer carries no timeline — the completion, the value and the "
                                "world are one write, so a triple missing the third was written by something "
                                "that does not share this record's shape");
        s = JS_ToCString(pend_ctx(), tw);
        CHECK(s != NULL, "engine: OOM reading the timeline that answered a cross-instance request");
        seen = !strcmp(s, world);
        JS_FreeCString(pend_ctx(), s);
        JS_FreeValue(pend_ctx(), tw);
        JS_FreeValue(pend_ctx(), tuple);
    }
    JS_FreeValue(pend_ctx(), list);
    return seen;
}

int pending_extra_count(JSValueConst e)
{
    JSValue list;
    int n;

    pend_atoms();
    list = pend_own(e, g_field[PEND_EXTRA]);
    n = JS_IsObject(list) ? pend_len(list) : 0;
    JS_FreeValue(pend_ctx(), list);
    return n;
}

int pending_extra_pop(JSValueConst e, JSValue *pvalue, JSValue *pworld)
{
    JSValue list, pair, c;
    int n;
    int32_t completion = 0;

    DCHECK(pvalue != NULL, "a peer timeline's answer was taken with nowhere to put its value — the arm forked "
                           "over it would resume the same frame with the FIRST answer, which is the timeline "
                           "its parent is already exploring");
    DCHECK(pworld != NULL, "a peer timeline's answer was taken with nowhere to put the TIMELINE that computed "
                           "it — the arm forked over it would then be an arm of no timeline, and a later "
                           "operation from it could not be addressed to the peer flow whose answer it holds");
    pend_atoms();
    list = pend_own(e, g_field[PEND_EXTRA]);
    DCHECK(JS_IsObject(list), "an answer beyond the first was taken from a request that has none recorded");
    n = pend_len(list);
    DCHECK(n > 0, "an answer beyond the first was taken from an empty list — the list is dropped the moment it "
                  "empties, so a zero-length one is a container this file did not build");
    /* THE LAST, because these are ALTERNATIVES and not a queue: two answers to one question are two peer
       timelines, and neither is before the other. Taking from the end is what keeps the drop O(1). */
    pair = JS_GetPropertyUint32(pend_ctx(), list, (uint32_t)(n - 1));
    c = JS_GetPropertyUint32(pend_ctx(), pair, 0);
    DCHECK(JS_IsNumber(c), "a recorded peer answer carries no completion type — the type and the value are one "
                           "write, so a pair without one would deliver a peer's THROW to the arm as a value");
    JS_ToInt32(pend_ctx(), &completion, c);
    JS_FreeValue(pend_ctx(), c);
    *pvalue = JS_GetPropertyUint32(pend_ctx(), pair, 1);
    *pworld = JS_GetPropertyUint32(pend_ctx(), pair, 2);
    DCHECK(JS_IsString(*pworld),
           "a recorded peer answer carries no timeline — the completion, the value and the world are one write "
           "(pending_extra_add), so a triple missing the third is a record written outside this file");
    JS_FreeValue(pend_ctx(), pair);
    cow_engine_write_begin();
    JS_SetProperty(pend_ctx(), list, g_len_atom, JS_NewInt32(pend_ctx(), n - 1));
    /* AND AN EMPTY LIST IS NOT A LIST — the same statement pending_remove makes about an empty register, for
       the same reason: `extra` is read on every step of every flow that ever parked on anything, so its answer
       for the overwhelmingly common case has to be a tag test rather than a shape lookup for `length`. */
    if (n == 1) pend_put(e, PEND_EXTRA, JS_NULL);
    cow_engine_write_end();
    JS_FreeValue(pend_ctx(), list);
    return (int)completion;
}

void pending_remove(JSValue *reg, int i)
{
    int n = pend_len(*reg);
    JSValue gone;
    DCHECK(i >= 0 && i < n, "a pending entry was removed by an index its register does not hold");
    pend_atoms();
    /* ONE FEWER REGISTER NAMES THIS RECORD, said BEFORE the slot is overwritten — after the swap-remove below
       the entry at `i` is a different record and this register's naming of the removed one is unrecoverable.
       A no-op for the three states pending_index.h names (answered, synchronous, already dropped). */
    gone = pending_entry(*reg, i);
    /* …AND SO IS THE COUNT OF WHAT BLOCKS THIS FLOW, read off the record BEFORE the swap for the same reason
       the naming is given back before it: after the swap the slot holds a different record and what left is
       unrecoverable. An UNANSWERED one only — an answered synchronous rendezvous was already taken off the
       count by pending_answer_sync, and engine_host_take removes exactly those. The other path here is
       engine_host_terminate, which withdraws a rendezvous the host will never answer: that IS an unanswered
       one leaving, and it is the reason this cannot be folded into the answer door. */
    if (pending_get_int(gone, PEND_KIND) == FLOW_PENDING_HOSTREQ && pend_owed(gone))
        pend_sync_owed_set(*reg, pend_sync_owed(*reg) - 1);
    pending_index_unref(gone);
    JS_FreeValue(pend_ctx(), gone);
    cow_engine_write_begin();
    if (i != n - 1)
        JS_SetPropertyUint32(pend_ctx(), *reg, (uint32_t)i,
                             JS_GetPropertyUint32(pend_ctx(), *reg, (uint32_t)(n - 1)));
    JS_SetProperty(pend_ctx(), *reg, g_len_atom, JS_NewInt32(pend_ctx(), n - 1));
    cow_engine_write_end();
    /* A REGISTER WITH NOTHING IN IT IS NOT A REGISTER, and that is a HOT-PATH statement rather than tidiness.
       flow_blocked is asked at every suspend point the interpreter offers, and its answer for a flow that owes
       nothing has to be a TAG TEST — the C list answered it with `npend == 0`. Left as an empty Array it would
       be a property lookup instead, and four dom/ranges tests crossed the gate's 60s CPU budget on exactly that
       difference. (The lookup it names used to be `length`; `pending_blocked` reads the register's own count of
       the synchronous requests it is owed now, which is the same one property read on the same object, so the
       measurement stands and only the slot's name has changed.) */
    if (n == 1) pending_free(pend_ctx(), reg);
}

void pending_free(JSContext *ctx, JSValue *reg)
{
    /* THE NAMINGS GO BACK BEFORE THE REGISTER DOES, AND THAT IS THE HALF THE INDEX CANNOT DERIVE. A flow SOLD
       to the cold tier is released while it is still owed replies (the pager credits that debt and the host
       still sends them), so without this line the request it was parked on would stay in the set the host is
       shown for the rest of the session, and the zone would fetch an address no flow is parked on — a real
       outbound request nobody is waiting for. A record several arms share survives here on their namings,
       which is why this is a count and not a delete. */
    int n = pend_len(*reg), i;
#if APICLIENT_DEV
    /* THE COUNT AUDITED AGAINST THE ENTRIES, AT A SITE THAT ALREADY WALKS THEM. This and the fork below are
       the two places the whole register is read anyway, so the second representation `pending_blocked` reads
       is checked against the thing it represents for free — every release of a register, and every fork. A
       drift caught here names the register whose count went wrong; a drift never caught is a flow silently in
       or out of the pick, which is §scheduler's razor. */
    DCHECK(pend_sync_owed(*reg) == pend_sync_owed_walk(*reg),
           "a register's count of the synchronous requests it is owed disagrees with its own entries at the "
           "release — the count is what the per-opcode preempt hook decides blockedness on, so a flow has been "
           "spinning on an answer that had arrived, or skipped at every pick for an answer it already had");
#endif
    for (i = 0; i < n; i++) {
        JSValue e = pending_entry(*reg, i);
        pending_index_unref(e);
        JS_FreeValue(ctx, e);
    }
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
    /* BY THE TUPLE'S OWN ARITY, and this used to copy exactly two elements. Two lists go through here and they
       are not the same width — a header is [name, value] and a peer's further answer is [completion, value,
       world] — so a hardcoded pair SILENTLY TRUNCATED the wider one at a fork: the arm inherited an answer with
       no timeline on it, which is the one thing that makes a second answer indistinguishable from a duplicate.
       Reading the length is what makes the shape the LIST's business rather than this copier's, which is the
       same reason pending.h says a field added there is inherited by default. */
    for (i = 0; i < n; i++) {
        JSValue tuple = JS_GetPropertyUint32(pend_ctx(), src, (uint32_t)i);
        JSValue cp = JS_NewArray(pend_ctx());
        int w = pend_len(tuple), j;
        CHECK(!JS_IsException(cp), "engine: OOM inheriting a pending request's tuple at a fork");
        DCHECK(w > 0, "a pending record's list holds an EMPTY tuple — every list this file builds writes its "
                      "elements in the bracket that appends the tuple, so a zero-width one is a container "
                      "something outside this file has written to");
        for (j = 0; j < w; j++)
            JS_SetPropertyUint32(pend_ctx(), cp, (uint32_t)j, JS_GetPropertyUint32(pend_ctx(), tuple, (uint32_t)j));
        JS_SetPropertyUint32(pend_ctx(), out, (uint32_t)i, cp);
        JS_FreeValue(pend_ctx(), tuple);
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
#if APICLIENT_DEV
    /* THE PARENT'S COUNT AUDITED AGAINST ITS ENTRIES — the fork's half of the pair pending_free holds, and the
       reason the audit is at these two sites and at no third: both already walk the whole register, so the
       check adds nothing to any path, and between them they cover every register that is ever released and
       every one that is ever inherited. */
    DCHECK(pend_sync_owed(reg) == pend_sync_owed_walk(reg),
           "a register's count of the synchronous requests it is owed disagrees with its own entries at a fork "
           "— the sibling inherits that count, so a wrong one is about to be copied into a second flow and "
           "both will be blocked or runnable on a number neither of their registers supports");
#endif
    out = JS_NewArray(pend_ctx());
    CHECK(!JS_IsException(out), "engine: OOM inheriting the pending replies at a fork");
    cow_engine_write_begin();
    /* THE COUNT IS INHERITED WHOLE, and O(1) rather than recounted, because the sibling's register names the
       SAME records in the same states at this instant — the arm has not run. What follows in
       engine_sibling_assemble does not move it either: it unshares each unanswered synchronous record and mints
       a fresh rendezvous id, and a field-identical copy of an unanswered request is still an unanswered
       request. The audit above is what makes copying a number safe rather than a hope. */
    pend_sync_owed_set(out, pend_sync_owed(reg));
    for (i = 0; i < n; i++) {
        JSValue e = pending_entry(reg, i);
        /* A SECOND REGISTER NAMES THIS RECORD FROM HERE ON, and the frontier's set counts registers rather than
           records for exactly this: ONE record is one member of the outstanding set however many arms hold it
           (pending_index.h), so the fork adds no request to the host's list and takes none away — it adds a
           naming, and the record leaves the set only when the last of them is given back. */
        pending_index_ref(e);
        JS_SetPropertyUint32(pend_ctx(), out, (uint32_t)i, e);
    }
    cow_engine_write_end();
    return out;
}

JSValue pending_unshare(JSValueConst reg, int i)
{
    JSValue src = pending_entry(reg, i);
    JSValue dst;
    /* THE ONE RECORD A FORK DOES NOT SHARE IS THE SYNCHRONOUS REQUEST'S, and this file may now say so rather
       than leave it to three call sites to keep true. The frontier's outstanding set is keyed on (method, url)
       and HOSTREQ is keyed on a request id, so a HOSTREQ record is never in that set — which is what makes this
       copy free of it. A record of any OTHER kind reaching here would be a second, untracked twin of a request
       the set still names through the original: the host would answer the original, and the arm holding the
       copy would wait for the rest of the session with nothing anywhere to say so. */
    /* …AND THE SECOND IS A RECORD THE ZONE HAS REFUSED, which is the SAME rule and not an exception to it. What
       makes an unshare unsound is a reply that can still arrive for the ORIGINAL: the copy is outside the
       frontier's outstanding set, so the host answers the original and the arm holding the copy waits for the
       rest of the session with nothing to say so. A HOSTREQ is admitted because it was never in that set; a
       DECLINED record is admitted because it is NO LONGER IN IT — the refusal itself took it out, at
       pend_index_sync's decline arm above — so "no reply is coming for it" is a property of the index that
       holds by construction rather than a promise about what the zone intends to do next. The wait it leaves
       the arm in is the PARK §@S requires, with the refusal's own reason on the record as the something that
       says so; the failure arm forked beside it takes its own private copy, which was never in the set
       either. */
    DCHECK(pending_get_int(src, PEND_KIND) == FLOW_PENDING_HOSTREQ || pending_entry_declined(src),
           "a pending record that is neither a synchronous host request nor a DECLINED one was unshared — a "
           "fetch record copied here would leave the copy outside the frontier's outstanding set while the "
           "original still answers for the address, so the host would answer the original and the arm holding "
           "the copy would wait for the rest of the session with nothing anywhere to say so");
    dst = pend_entry_copy(src);
    /* AND THE REGISTER'S COUNT OF WHAT BLOCKS ITS FLOW IS UNTOUCHED, WHICH IS A STATEMENT AND NOT AN OMISSION.
       pend_entry_copy goes through PENDING_FIELDS, so the copy carries the source's `kind` and `haveValue`
       exactly: the slot held an unanswered synchronous request before this line and holds one after it. What
       changes is WHICH record the slot names, and the count is over slots. */
    /* THIS REGISTER STOPS NAMING THE ORIGINAL, so the naming goes back exactly as it does at a remove — the
       count is over registers and this slot has just changed which record it holds. A no-op for BOTH kinds the
       assert above admits — one was never tracked, the other stopped being tracked at its refusal — and the
       bookkeeping is written anyway because the count's correctness is a property of every naming change and
       not of the kinds that happen to reach one. */
    pending_index_unref(src);
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
                /* NO BODY IS A POSITIVE STATEMENT AND IS READ AS ONE — never as a hole a NULL return fills.
                 * A record is BORN with `PEND_BODY` = JS_NULL (pending_push above, from PENDING_FIELDS' own
                 * default column) and only a request that
                 * actually carries bytes ever replaces it (engine.c's pending_set_bytes), so on a frontier of
                 * fetch replies almost every record's body field is JS_NULL by design.
                 * `JS_GetArrayBuffer` THROWS on anything that is not an ArrayBuffer — "ArrayBuffer object
                 * expected", from the class check every ArrayBuffer accessor shares — and answers NULL. Asking
                 * it unconditionally therefore raised a TypeError per bodiless record, and `if (ptr)` read the
                 * NULL as "add nothing" and walked on: the count was right and the COMPLETION was left standing
                 * in the runtime. That is §Architecture's default-conceals-a-hole exactly, and the concealment
                 * is the `if`, not the throw.
                 * WHERE IT LANDED IS WHY IT MATTERED. `rt->current_exception` is per-RUNTIME while a completion
                 * is per-EVALUATION (ECMA-262 §6.2.4 "The Completion Record Specification Type"; §5.2.4.3
                 * "Shorthands for Unwrapping Completion Records" — "?" propagates an abrupt completion TO THE
                 * CALLER), and this walk is a CENSUS: it runs in the host's own time between two scheduler
                 * slices (cold_census, the @COLD line), where no flow is running and nothing owns a throw. The
                 * next flow the scheduler resumed found it and read it as its own.
                 * A CENSUS MEASURES; IT MAY NOT THROW. `pend_str_bytes` three lines above already states that
                 * shape — it asks `JS_IsString` and answers 0 — and this is the same sentence for the same
                 * reason. The third case, a body field that is neither absent nor a buffer, is a PRODUCER bug
                 * and says so rather than being measured as zero. */
                DCHECK(JS_IsNull(v) || JS_IsArrayBuffer(v),
                       "a pending record's body field is neither absent nor a byte sequence — it is born "
                       "JS_NULL and only pending_set_bytes ever writes one, so a third shape here is a "
                       "producer writing something that is not a request body into the slot a park serializes");
                if (JS_IsArrayBuffer(v) && JS_GetArrayBuffer(pend_ctx(), &bl, v)) total += (long)bl;
            /* THE TWO PAIR LISTS, counted by one arm of this walk because they are one shape — the request's
               headers, and the answers beyond the first that a peer's other timelines gave. A field a census
               does not walk is a field a pager is surprised by. */
            } else if ((f == PEND_HEADERS || f == PEND_EXTRA) && JS_IsObject(v)) {
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
