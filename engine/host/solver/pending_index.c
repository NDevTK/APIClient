/* The frontier's outstanding requests — see pending_index.h. */
#include "solver/pending_index.h"
#include "solver/pending.h"   /* the session's context; this file allocates no realm state of its own */
#include "check.h"

#include <stdlib.h>
#include <string.h>

/* A RECORD THIS INDEX IS TRACKING. `namers` is how many REGISTERS name it, which is the one fact neither the
   record nor the register carries: a fork shares the record, so the record cannot say how many arms hold it and
   any one register only knows about itself. It is counted here because here is where the answer is needed —
   the last drop is what removes the request from the set the host is shown.
   `node` is NULL until the record carries both halves of its identity. A record is pushed with neither and the
   park sets them one at a time, so there is a window in which a tracked record has no pair; it is not a state
   to be defaulted past, it is the record being built. */
typedef struct PendIndexMember {
    JSValue                 rec;    /* OWNED — so a namer count that ever goes wrong is a LEAK the runtime's
                                       own gc_obj_list walk reports, and never a read through a freed record */
    int                     namers;
    PendIndexNode          *node;
    struct PendIndexMember *hnext;  /* the by-record chain */
} PendIndexMember;

/* A REQUEST — the (method, url) pair the reply seam is keyed on (engine.h), with the records still parked on it.
   IT OUTLIVES ITS MEMBERS. `answered` is what tells a host that answered one request twice from a host whose
   pairing is off, and both of those are silent in every other way. The count of distinct pairs a session issues
   is the endpoint surface, which this engine already keeps whole. */
struct PendIndexNode {
    char             *method;
    char             *url;
    uint32_t          hash;
    PendIndexMember **mem;
    int               n_mem, cap_mem;
    long              answered;
    PendIndexNode    *hnext;   /* the by-pair chain */
    PendIndexNode    *anext;   /* every node, in creation order — the join's walk */
};

/* TWO LOOKUPS OVER ONE SET, because the two doors ask different questions of it: the reply door has a PAIR and
   wants the records, and every membership mutation has a RECORD and wants its member. Open chaining, doubled
   when the load reaches one, which is the shape quickjs's own tables have. */
static PendIndexMember **g_by_rec;
static uint32_t          g_rec_buckets;
static uint32_t          g_rec_n;
static PendIndexNode   **g_by_pair;
static uint32_t          g_pair_buckets;
static uint32_t          g_pair_n;
static PendIndexNode    *g_nodes;       /* the walk's head */
static PendIndexNode    *g_nodes_tail;  /* appended in creation order, so the join's line order is stable */

/* THE JOIN'S ORDER IS AN OBSERVABLE OF THE HOST, so the walk is in creation order rather than in whatever
   order a hash happens to bucket. A zone's firing decision reading a list whose order moved between two runs
   of one document is the same defect as a value read two ways on two runs. */

/* HOW MANY REQUESTS THIS DOCUMENT HAS EVER PUT TO THE REPLY DOOR, AND HOW MANY REPLIES SETTLED ONE — the reply
 * door's half of the pair engine.c states for the SYNCHRONOUS door, sitting at the two ends of the membership
 * this file owns and for exactly the reason that pair exists: "Starvation is a RATE", and until this line the
 * reply door had none. The census held `pend` (every register's LENGTH), `owed` (flows marked host-owed) and
 * `blocked` (flows parked on a SYNCHRONOUS request) — three LEVELS, and engine.c's own argument is that no
 * single reading of a level can tell a host that is paying promptly from one that has never paid at all.
 *
 * WHAT IT COST TO NOT HAVE IT, WHICH IS WHY IT IS HERE AND NOT A ROW SOMEBODY MIGHT LIKE. `hostAsked` and
 * `hostAnswered` are minted at engine.c's `mint_req`, whose callers push FLOW_PENDING_HOSTREQ — the four
 * cross-instance reads (a navigable's, a remote object's, a WindowProxy's, an iframe's) AND
 * `XMLHttpRequest`, which places its request through the same rendezvous because §3.5.6's synchronous arm
 * must BLOCK the flow and the asynchronous arm places the identical request from a task. So a document
 * that makes no cross-document read AND NO XHR reads `0/0` for ever; a document that XHRs does not.
 * THIS SENTENCE SAID "only two callers" AND "NOTHING ELSE", AND BOTH WERE WRONG — the second by exactly
 * the caller a reader of a census most needs to know about. It is repaired here rather than deleted
 * because a reader who re-derives the narrow reading will re-introduce it. Its sibling in solver/result.c
 * carried the identical claim and was repaired first; this one survived that repair for a while, which is
 * the recurrence CLAUDE.md names — a fix filed where it was found, while the wrong spelling stays at every
 * site that reached for the same idiom. The grep that ends it is the CLAIM, not the symbol. That pair was rendered under a caption asking whether "a waiting frontier
 * is waiting because of the RANKING or because nobody paid it", which is the GENERAL question, and a reader
 * took the general answer from it: `hostAsked: 0` was reported as "nothing is ever asked of the host" for a
 * fixture whose registers held hundreds of thousands of records. Nothing was wrong with the number. What was
 * wrong is that the door §Learning-from-replies calls "the POINT" had no number at all, and an absent one is
 * filled by whichever plausible neighbour is printed next to it — CLAUDE.md's defaulted-field defect performed
 * on a report rather than on a record.
 *
 * THE TWO SITES ARE THE TWO ENDS OF ONE MEMBERSHIP, which is what makes the pair balanced rather than two
 * counts that happen to be printed together. A record ENTERS the set the host is shown when its (method, url)
 * pair completes (`pending_index_key`, which DCHECKs it is keyed at most once) and LEAVES answered when a
 * reply writes its value (`pending_index_answered`, which untracks it so it can never re-enter). Each record
 * therefore contributes at most one to each, and answered implies keyed — so `answered <= asked` is a real
 * invariant and result.c asserts it, exactly as engine.c asserts the synchronous door's.
 * THERE IS A THIRD END AND IT PAYS NOTHING, WHICH IS WHY IT DOES NOT DISTURB THAT INVARIANT. A record may also
 * leave REFUSED (`pending_index_declined`), and a refusal is not a reply: it credits neither of the two terms
 * above, so it can only ever take a record OUT of the population that could later be answered. The pair
 * therefore still describes one population and the inequality holds a fortiori — what a session full of
 * refusals makes wide is the GAP between the two, and that gap is what a refused surface should look like
 * rather than a host failing to pay.
 * THIS PARAGRAPH USED TO END `Nothing here can tell those two apart; the record's own declined reason is what
 * can`, AND THAT SENTENCE IS THE REASON THE THREE COUNTS BELOW EXIST — it is rewritten rather than deleted
 * because a reader who re-derives the argument above will re-derive the missing entry with it. Every word of
 * it was TRUE and it is CLAUDE.md §A-CONTRACT-THAT-NAMES-A-HAZARD-AND-OFFERS-NO-EXIT exactly: the one thing a
 * caller could do with this file was take the gap, the sentence said the gap has two readings, and it named
 * as the discriminator a per-record field NO CENSUS CAN REACH — so the forbidden reading was the only reading
 * on offer, for the life of the interface. A warning a caller cannot act on is §Offensive-programming's
 * category (2), an unbuilt capability wearing prose, and the missing ENTRY is the fix.
 * MEASURED, WHICH IS WHY IT IS A DEFECT RECORD AND NOT A TIDY-UP: four fresh-browser drives of one real SPA at
 * artifact f5650152 read `replyAsked 9 / replyAnswered 7` in every one, and the gap was TWO REFUSALS of that
 * page's own boot `fetch()` and of the `.catch` arm's error report — nothing was outstanding and nothing was
 * stuck. It was relayed onward as two replies the host had failed to pay, with an address named that had in
 * fact come back 200, and a coordinator dispatched on it. A second page in the same browser read 31/31 with no
 * refusal at all, which is the same pair saying the opposite thing for the same reason.
 *
 * SO THE GAP IS PARTITIONED AND THE PARTS SUM, which is what makes the new rows checkable rather than a second
 * opinion (CLAUDE.md §a-bare-count-over-a-population-you-have-not-partitioned: raise three rows where you were
 * raising one, and ASSERT that they sum to the total). Every record this index has ever KEYED is now, at any
 * instant, in exactly one of four states, and each of the four is credited at the ONE line that puts it there:
 *   ANSWERED     `pending_index_answered` — a reply reached the register.
 *   DECLINED     `pending_index_declined` — the trusted zone refused to make the request.
 *   DROPPED      `pending_index_unref`'s last naming — the flow that held it DEPARTED owing this reply
 *                (finished, or SOLD to the cold tier, which `pending_free`'s own comment states is a designed
 *                state and not a loss), so no reply can ever settle it.
 *   OUTSTANDING  still keyed — `g_keyed_n`, the one GAUGE here, and the only one of the four that means the
 *                host still owes something.
 * THE PROOF THAT THOSE ARE THE ONLY FOUR IS BY INSPECTION AND IS ONE LINE LONG: `pend_untrack` has exactly
 * three callers and they are the first three above, `pend_unkey` is the only line a record leaves a pair on,
 * and `pending_index_reset` DCHECKs the set is EMPTY before it runs — so the gauge is 0 across a reset and the
 * three lifetime totals may be left standing there exactly as the first two already are.
 * THE KINDS ARE STATED BECAUSE THEY DIFFER (CLAUDE.md §A-GAUGE-AND-A-LIFETIME-COUNTER): `asked`, `answered`,
 * `declined` and `dropped` are LIFETIME and may be differenced; `keyed` is a GAUGE, may FALL, and a reader who
 * differences it is reading a level as a rate. The identity is therefore an assertion about ONE INSTANT and is
 * asserted where all five are in one hand.
 * WHAT THE FOUR DO NOT SAY is which KIND of request each record was — a fetch, an injected `<script src>`, a
 * document script slot or a dynamic `import()`. That is a different partition of the same total and
 * test_forced.c holds its own named residual for it; this one is by END and the two do not substitute.
 *
 * THE UNIT IS THE RECORD AND NOT THE ISSUED REQUEST, and the difference is the sharing this file is built on:
 * N arms share ONE record (pending.h's PEND_SHARE), the join dedups over the pair, so one issued request may
 * settle several records and does so with one `engine_provide`. That is the same unit `engine_provide` returns
 * and the same unit the census's `pend` is a length of, which is what lets the three be read together.
 *
 * WHAT IT IS READ AGAINST, because a payment is not yet a thing learned. `replyAnswered` is the host's value
 * reaching the REGISTER; the flow still has to take it (flow_deliver_one_reply), which the census already
 * counts as a step-unit arm. `replyAnswered` climbing with that arm at zero is a document being paid and
 * consuming nothing — the exact shape CLAUDE.md records as having happened once already, where every flow a
 * page's fetch parked "stayed parked forever" and the learning "had never once happened".
 * AND THAT IS A PREDICATE ON THE ARM BEING ZERO AND NEVER A RATIO AGAINST IT — this pair is per RECORD and the
 * arm is per NAMING, one record being named by every register that forked while it was in flight. The census's
 * `pendReady` is the arm's own unit; pending_index.h carries the derivation.
 *
 * NOT RESET AT A SESSION BOUNDARY, with engine.c's five and for their reason: a rate whose numerator and
 * denominator are cleared at different moments is not a rate. `pending_index_reset` frees the table below and
 * leaves these two alone. They are read by NOTHING but the census (§NO BOUNDS): no pick, no weight, no exit. */
static long g_asked_total;
static long g_answered_total;
/* THE THIRD AND FOURTH ENDS, AND THE GAUGE THAT CLOSES THE IDENTITY — see the banner above for why the three
   of them are one diff and for the inspection that says the four states are the only four. */
static long g_declined_total;
static long g_dropped_total;
/* KEYED RIGHT NOW. A GAUGE: it rises at the one line that keys a record and falls at the one line that unkeys
   one, so it is the count of requests the host may still be shown — never differenced, and 0 at a reset. */
static long g_keyed_n;

static uint32_t pend_hash_bytes(const char *s, uint32_t h)
{
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

static uint32_t pend_hash_pair(const char *method, const char *url)
{
    uint32_t h = pend_hash_bytes(method, 2166136261u);
    h = pend_hash_bytes("\t", h);       /* the separator is part of the key: `GET` + `\tx` is not `GET\t` + `x` */
    return pend_hash_bytes(url, h);
}

static uint32_t pend_hash_ptr(JSValueConst rec)
{
    uintptr_t p = (uintptr_t)JS_VALUE_GET_PTR(rec);
    uint64_t x = (uint64_t)p;
    /* A POINTER'S LOW BITS ARE ITS ALIGNMENT AND ITS HIGH BITS ARE THE ALLOCATOR'S — neither distinguishes two
       records on their own, so the two halves are mixed before the mask takes the low ones. */
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    return (uint32_t)x;
}

static void pend_rec_grow(void)
{
    uint32_t nb = g_rec_buckets ? g_rec_buckets * 2 : 64, i;
    PendIndexMember **tab = calloc(nb, sizeof *tab);
    CHECK(tab != NULL, "engine: OOM growing the frontier's outstanding-request index");
    for (i = 0; i < g_rec_buckets; i++) {
        PendIndexMember *m = g_by_rec[i];
        while (m) {
            PendIndexMember *next = m->hnext;
            uint32_t b = pend_hash_ptr(m->rec) & (nb - 1);
            m->hnext = tab[b];
            tab[b] = m;
            m = next;
        }
    }
    free(g_by_rec);
    g_by_rec = tab;
    g_rec_buckets = nb;
}

static void pend_pair_grow(void)
{
    uint32_t nb = g_pair_buckets ? g_pair_buckets * 2 : 64, i;
    PendIndexNode **tab = calloc(nb, sizeof *tab);
    CHECK(tab != NULL, "engine: OOM growing the frontier's outstanding-request index");
    for (i = 0; i < g_pair_buckets; i++) {
        PendIndexNode *n = g_by_pair[i];
        while (n) {
            PendIndexNode *next = n->hnext;
            uint32_t b = n->hash & (nb - 1);
            n->hnext = tab[b];
            tab[b] = n;
            n = next;
        }
    }
    free(g_by_pair);
    g_by_pair = tab;
    g_pair_buckets = nb;
}

static PendIndexMember *pend_member(JSValueConst rec)
{
    PendIndexMember *m;
    if (!g_rec_buckets) return NULL;
    for (m = g_by_rec[pend_hash_ptr(rec) & (g_rec_buckets - 1)]; m; m = m->hnext)
        if (JS_VALUE_GET_PTR(m->rec) == JS_VALUE_GET_PTR(rec)) return m;
    return NULL;
}

/* TAKE THE RECORD OUT OF ITS PAIR — the members array is a SET, so the hole is filled by the last one, exactly
   as pending_remove's swap-remove is and for the same reason. */
static void pend_unkey(PendIndexMember *m)
{
    PendIndexNode *n = m->node;
    int i;
    DCHECK(n != NULL, "a record was unkeyed from a pair it was never keyed under");
    for (i = 0; i < n->n_mem; i++)
        if (n->mem[i] == m) {
            n->mem[i] = n->mem[n->n_mem - 1];
            n->n_mem--;
            m->node = NULL;
            /* THE GAUGE FALLS HERE AND NOWHERE ELSE, which is what makes it the count of records the host may
               still be shown rather than a fourth thing that has to be kept in step by hand: this is the one
               line a record's membership of a pair ever ends on, and every end above it — answered, declined,
               the last naming given back — reaches it through pend_untrack. */
            DCHECK(g_keyed_n > 0,
                   "a record was unkeyed while the index holds no keyed record — the gauge rises at "
                   "pending_index_key and falls only here, so this is a second unkey of one membership and "
                   "the partition `asked == answered + declined + dropped + keyed` is about to go negative");
            g_keyed_n--;
            return;
        }
    DFAIL("a record names a pair whose member list does not hold it — the two halves of one membership have "
          "come apart, and the record would then be listed to the host by a walk that can never remove it");
}

static void pend_untrack(PendIndexMember *m)
{
    uint32_t b = pend_hash_ptr(m->rec) & (g_rec_buckets - 1);
    PendIndexMember **pp = &g_by_rec[b];
    if (m->node) pend_unkey(m);
    while (*pp && *pp != m) pp = &(*pp)->hnext;
    DCHECK(*pp == m, "a tracked record is not in the bucket its own pointer hashes to — the record moved, "
                     "which a JS object does not do, or the table was rebuilt without it");
    *pp = m->hnext;
    g_rec_n--;
    JS_FreeValue(pending_ctx(), m->rec);
    free(m);
}

void pending_index_track(JSValueConst rec)
{
    PendIndexMember *m;
    uint32_t b;

    DCHECK(JS_IsObject(rec), "the outstanding-request index was asked to track something that is not a record");
    DCHECK(pend_member(rec) == NULL,
           "a pending record was tracked twice — tracking begins at the ONE push that creates a record, so a "
           "second one is a record built outside pending_push or a pointer the runtime has already reused for "
           "an object this index never let go of");
    if (g_rec_n >= g_rec_buckets) pend_rec_grow();
    m = calloc(1, sizeof *m);
    CHECK(m != NULL, "engine: OOM tracking a pending record — an untracked outstanding request is one the host "
                     "is never shown, and the flow parked on it waits for the rest of the session");
    m->rec = JS_DupValue(pending_ctx(), rec);
    m->namers = 1;   /* the register it was just pushed onto; a fork adds the rest */
    b = pend_hash_ptr(rec) & (g_rec_buckets - 1);
    m->hnext = g_by_rec[b];
    g_by_rec[b] = m;
    g_rec_n++;
}

int pending_index_tracked(JSValueConst rec) { return pend_member(rec) != NULL; }

int pending_index_keyed(JSValueConst rec)
{
    PendIndexMember *m = pend_member(rec);
    return m != NULL && m->node != NULL;
}

void pending_index_ref(JSValueConst rec)
{
    PendIndexMember *m = pend_member(rec);
    if (!m) return;   /* answered, synchronous, or already dropped by every register — see pending_index.h */
    DCHECK(m->namers > 0, "a record with no register naming it was inherited by a fork — it should have left "
                          "this index at the drop that took its last namer");
    m->namers++;
}

void pending_index_unref(JSValueConst rec)
{
    PendIndexMember *m = pend_member(rec);
    if (!m) return;
    DCHECK(m->namers > 0, "a register gave back a naming of a record it does not hold — the count is one per "
                          "register slot, so this is a second free of one slot or a drop of a record that "
                          "was never on this register");
    if (--m->namers == 0) {
        /* THE FOURTH END: THE LAST FLOW NAMING THIS RECORD IS GONE AND NO REPLY CAN EVER SETTLE IT. A record
           reaches this line when the register holding it was released — a flow that FINISHED, or one the pager
           SOLD to the cold tier, which `pending_free`'s own comment names as the designed case ("the pager
           credits that debt and the host still sends them"). It is neither a payment nor a refusal, and until
           this row existed it was the third thing hiding inside `asked - answered`: a gap that means the host
           still owes something and a gap that means the ASKER LEFT are opposite findings, and only one of them
           is about the reply door at all. Guarded on `m->node` because an UNKEYED record never entered
           `asked`. */
        if (m->node) g_dropped_total++;
        pend_untrack(m);
    }
}

void pending_index_key(JSValueConst rec, const char *method, const char *url)
{
    PendIndexMember *m = pend_member(rec);
    PendIndexNode *n;
    uint32_t h, b;

    DCHECK(method != NULL && url != NULL,
           "a request was keyed into the outstanding set with half a pair — the reply seam is keyed on "
           "(method, url) and a half key would collect another request's body");
    DCHECK(m != NULL, "an untracked record was keyed — tracking begins at the push and a record that reaches "
                      "this line without it is one the last register already dropped");
    DCHECK(m->node == NULL, "a record was keyed twice — a request's identity is written once, at the park, so "
                            "a second key is either a rewritten address or a member the first key lost");
    h = pend_hash_pair(method, url);
    if (!g_pair_buckets) pend_pair_grow();
    for (n = g_by_pair[h & (g_pair_buckets - 1)]; n; n = n->hnext)
        if (n->hash == h && !strcmp(n->method, method) && !strcmp(n->url, url)) break;
    if (!n) {
        if (g_pair_n >= g_pair_buckets) pend_pair_grow();
        n = calloc(1, sizeof *n);
        CHECK(n != NULL, "engine: OOM recording an outstanding request");
        n->method = strdup(method);
        n->url = strdup(url);
        CHECK(n->method && n->url, "engine: OOM recording an outstanding request's identity");
        n->hash = h;
        b = h & (g_pair_buckets - 1);
        n->hnext = g_by_pair[b];
        g_by_pair[b] = n;
        g_pair_n++;
        if (g_nodes_tail) g_nodes_tail->anext = n; else g_nodes = n;
        g_nodes_tail = n;
    }
    if (n->n_mem == n->cap_mem) {
        int nc = n->cap_mem ? n->cap_mem * 2 : 4;
        PendIndexMember **mm = realloc(n->mem, (size_t)nc * sizeof *mm);
        CHECK(mm != NULL, "engine: OOM parking a record on an outstanding request");
        n->mem = mm;
        n->cap_mem = nc;
    }
    n->mem[n->n_mem++] = m;
    m->node = n;
    /* THE ASK, COUNTED WHERE THE RECORD BECOMES ASKABLE and not where it was pushed. A record is born with
       neither half of its identity and the park writes them one at a time, so a push is not yet a request the
       host can be shown; this line is the moment it is one, and it is guarded by the once-only DCHECK above. */
    g_asked_total++;
    /* …AND IT IS OUTSTANDING FROM THIS INSTANT, which is the gauge's other end. The two rise together because
       they are the same event read as a rate and as a level, and the once-only DCHECK above is what stops the
       level being raised twice for one record. */
    g_keyed_n++;
}

void pending_index_answered(JSValueConst rec)
{
    PendIndexMember *m = pend_member(rec);

    DCHECK(m != NULL, "a reply landed on a record this index is not tracking — a fetch record is tracked from "
                      "its push until it is answered, so this is a second answer to one request or a write "
                      "onto a record every register has already dropped");
    /* THE PAYMENT, AND ONLY FOR A RECORD THAT WAS ON THE SET — the same condition the per-node count above is
       under, and it is what makes `answered <= asked` an invariant rather than a hope. An unkeyed record was
       never a request the host could be shown, so a value arriving on one is not a reply being paid; it is
       counted by neither term and the two stay describing one population. */
    if (m->node) { m->node->answered++; g_answered_total++; }
    /* AND IT STOPS BEING TRACKED, NOT MERELY UNKEYED. An answered record can never re-enter the outstanding
       set — nothing ever clears `haveValue` — so keeping a member for it would grow this index with the very
       thing it exists to stop walking. The namer count goes with it, which is why the two calls above are
       documented as no-ops on an untracked record rather than as errors. */
    pend_untrack(m);
}

void pending_index_declined(JSValueConst rec)
{
    PendIndexMember *m = pend_member(rec);

    DCHECK(m != NULL, "a refusal landed on a record this index is not tracking — a fetch record is tracked from "
                      "its push until it is answered or refused, so this is a second refusal of one request or "
                      "a write onto a record every register has already dropped");
    /* AND NEITHER TOTAL MOVES, WHICH IS THE WHOLE OF WHAT MAKES THIS A SEPARATE ENTRY POINT — see the
       derivation at pending_index.h. The record entered `asked` when its pair completed and it leaves WITHOUT
       ever having been answered, which is what a refused request IS; crediting either term here would put a
       payment nobody made into a rate, and the surplus is spent by the next reply the host genuinely
       mispaired. The per-pair `answered` is left alone for the same reason one level down: it separates a
       reply sent TWICE from a reply for a request nobody made, and a refusal is neither.
       SO THE PAIR'S NODE MAY NOW BE EMPTY WITH `answered` AT ZERO, and that is a fourth reading it did not
       have — no member parked, none answered, because the zone refused them. It is not ambiguous with the
       others: `engine_provide` tells "answered twice" from "answered for nobody" by whether ANY record
       matched, and a refused pair matches nothing, so it takes the arm the host's own pairing assert owns.
       WHAT DOES MOVE IS THE THIRD END'S OWN COUNT, and the two statements are not in tension: crediting
       `answered` here would put a payment nobody made into a rate, and crediting NOTHING left that rate's own
       GAP unattributable, which is the defect the banner records. A refusal is its own end and gets its own
       row, so `replyAnswered/replyAsked` stays exactly the rate it was and `asked - answered` stops being a
       number with two readings. Guarded on `m->node` for `answered`'s reason exactly: an UNKEYED record was
       never a request the host could be shown, so it entered neither `asked` nor this. */
    if (m->node) g_declined_total++;
    pend_untrack(m);
}

PendIndexNode *pending_index_first(void) { return g_nodes; }
PendIndexNode *pending_index_next(PendIndexNode *n)
{
    DCHECK(n != NULL, "the outstanding-request walk was advanced past its end");
    return n->anext;
}
const char *pending_index_node_method(const PendIndexNode *n)
{
    DCHECK(n != NULL, "an outstanding request's method was read off no node");
    return n->method;
}
const char *pending_index_node_url(const PendIndexNode *n)
{
    DCHECK(n != NULL, "an outstanding request's address was read off no node");
    return n->url;
}
int pending_index_node_count(const PendIndexNode *n)
{
    DCHECK(n != NULL, "an outstanding request's member count was read off no node");
    return n->n_mem;
}
JSValue pending_index_node_member(const PendIndexNode *n, int i)
{
    DCHECK(n != NULL && i >= 0 && i < n->n_mem,
           "an outstanding request's member was asked for past the end of its list");
    return JS_DupValue(pending_ctx(), n->mem[i]->rec);
}
long pending_index_node_answered(const PendIndexNode *n)
{
    DCHECK(n != NULL, "a request's answered count was read off no node");
    return n->answered;
}

PendIndexNode *pending_index_find(const char *method, const char *url)
{
    uint32_t h;
    PendIndexNode *n;
    DCHECK(method != NULL && url != NULL, "the outstanding set was searched with half a request identity");
    if (!g_pair_buckets) return NULL;
    h = pend_hash_pair(method, url);
    for (n = g_by_pair[h & (g_pair_buckets - 1)]; n; n = n->hnext)
        if (n->hash == h && !strcmp(n->method, method) && !strcmp(n->url, url)) return n;
    return NULL;
}

long pending_index_asked_total(void)    { return g_asked_total; }
long pending_index_answered_total(void) { return g_answered_total; }
/* SEPARATE ENTRIES AND NOT OUT-PARAMETERS ON ONE CALL, for CLAUDE.md §A-CONTRACT-THAT-NAMES-A-HAZARD's own
   reason: two answers of different KIND taken from one call are free to be read into a variable named for the
   other, and three of these are LIFETIME counts while the last is a LEVEL. Separate entries make that mistake
   a different CALL instead. */
long pending_index_declined_total(void) { return g_declined_total; }
long pending_index_dropped_total(void)  { return g_dropped_total; }
long pending_index_keyed_now(void)      { return g_keyed_n; }

void pending_index_reset(JSContext *ctx)
{
    PendIndexNode *n;
    uint32_t i;

    /* THE TWO TOTALS ARE NOT ON THIS FUNCTION'S LIST, and their absence is the statement. Everything below is
       the SET — a derived lookup rebuilt by the same pushes that rebuild the registers — and the two counts
       above are a RATE over the whole life of this instance, which is what makes them able to say a host has
       never paid at all. Clearing them here would clear the numerator and the denominator at a moment that
       means nothing to either, and would reset them to a state a resumed frontier is not in. */

    /* EVERY REGISTER IS GONE BY HERE, SO EVERY NAMING IS TOO — flow_registry_free releases each flow's register
       before it reaches this line, and each release gives back one naming per entry. A member still standing is
       therefore a register that was freed without giving its namings back, which is the ONE way the count can
       be wrong in the direction that matters: it would have kept a request in the host's list for ever. */
    DCHECK(g_rec_n == 0,
           "the frontier's outstanding-request index still names records after every register was released — a "
           "naming was taken at a fork or a push and never given back, so this index was holding requests no "
           "flow was parked on and the host was being shown them every slice");
    for (i = 0; i < g_rec_buckets; i++)
        while (g_by_rec[i]) {
            PendIndexMember *m = g_by_rec[i];
            g_by_rec[i] = m->hnext;
            if (m->node) pend_unkey(m);
            JS_FreeValue(ctx, m->rec);
            free(m);
        }
    free(g_by_rec); g_by_rec = NULL; g_rec_buckets = 0; g_rec_n = 0;
    n = g_nodes;
    while (n) {
        PendIndexNode *next = n->anext;
        free(n->method);
        free(n->url);
        free(n->mem);
        free(n);
        n = next;
    }
    free(g_by_pair); g_by_pair = NULL; g_pair_buckets = 0; g_pair_n = 0;
    g_nodes = NULL; g_nodes_tail = NULL;
}
