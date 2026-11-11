/* THE OUTCOME FORK — see quickjs-step.h for what it is for. The two phases are "ask" and "answered", and
   which of them the SIBLING's snapshot carries is the whole of the design: it carries ASK, so the sibling
   re-enters here, asks again, and reads its arm out of its own decision vector. */
enum { FORK_PH_ASK = 0, FORK_PH_ANSWERED };

/* THE ASKED QUESTION'S IDENTITY — see JSStepHdr::fork_ask_key for why it is the operation string's BYTES and
   not its address. FNV-1a, which is what a content hash of a short ASCII key is everywhere else. A hash can
   only ever fail to CATCH a mismatch, never invent one, which is the property a DCHECK standing over every
   step machine in the engine has to have. */
static uint32_t step_fork_key(const char *op)
{
    uint32_t k = 2166136261u;

    if (op == NULL)
        return 0;
    while (*op) {
        k ^= (unsigned char)*op++;
        k *= 16777619u;
    }
    return k ? k : 1u;   /* 0 is reserved for "nothing asked", which a zeroed header already reads as */
}

/* AND A FORK'S ANSWER ARRIVES AT THE STAGE THAT ASKED — step_keyed_answered's assertion, made of the one
 * request kind that did not have it. See JSStepHdr::fork_stage for why the key above cannot stand in for it:
 * an elimination chain's key is composed from the PREDICATE and the member's own name and carries neither the
 * algorithm nor the operand, precisely so that two members reached over ONE unknown agree — which makes it
 * BYTE-IDENTICAL between two chains ONE algorithm draws over TWO operands at the same position.
 *
 * IT REFUSES A FORK THAT OUTLIVES ITS STAGE, AND IT IS step_keyed_answered's HALF OF THAT REFUSAL AND NOT
 * step_keyed_inflight's — this sentence named the wrong one of the pair, and the fork bookkeeping was built from
 * it, so the ISSUE side went unwritten while its own first line correctly named the ANSWER side. step_fork_inflight
 * is the other half now, and the two are a partition: this one cannot see an ask the ASK phase makes, because
 * that never reaches the branch below. Two asks
 * legitimately stand inside one stage in sequence — a chain asks per position, and permission_store.c's §5.1
 * asks twice off one phase byte — because only one of them is ever OUTSTANDING at a time. What is refused is
 * the re-entry that reaches a DIFFERENT sub-sequence's ask than the one that parked, and a sub-sequence that
 * can be in flight while another has already answered is one whose resume point is a STAGE. */
static void step_fork_answered_stage(const JSStepHdr *h)
{
#if APICLIENT_DEV
    char why[640];   /* the tail is where this names what to build; a truncated DFAIL loses exactly that */

    if (h->fork_stage == h->stage) return;
    snprintf(why, sizeof why,
             "%s consumed a fork's answer at stage %u (%s) that was ASKED at stage %u (%s) — the machine left "
             "the stage that parked on the ask, so this call site is taking another site's arm: a real "
             "completion, in range, recorded by the flow's decision vector under the OTHER question's key and "
             "read here as the answer to this one. The ask key cannot see it when the two sites spell one "
             "operation, which is what an elimination chain drawn twice over two operands does at every "
             "position the two chains share. Let the outstanding ask END before leaving its stage, and give a "
             "sub-sequence that can still be in flight while another has answered a STAGE of its own",
             h->def->algorithm, (unsigned)h->stage, step_stage_label(h, h->stage),
             (unsigned)h->fork_stage, step_stage_label(h, h->fork_stage));
    DFAIL(why);
#else
    (void)h;
#endif
}

/* AND A FORK MAY NOT BE ASKED AT A STAGE WHILE ONE ASKED AT ANOTHER IS STILL OUTSTANDING — the EAGER half of
 * the contract above, and the half the fork bookkeeping was built without.
 *
 * THE KEYED FAMILY HAS BOTH HALVES AND THE FORK COPIED ONE. step_keyed_inflight refuses the ISSUE and
 * step_keyed_answered refuses the ANSWER, and they are a PARTITION rather than a duplication: the issue side
 * sees a request that overlaps another WITHIN ONE `def->step()` call, where no answer is ever collected and the
 * answer side therefore never runs. step_fork_answered_stage is step_keyed_answered's assertion and says so in
 * its own first line; nothing was step_keyed_inflight's, so the fork's issue side was unwatched.
 *
 * WHAT IT CATCHES IS THE SIBLING WALKING AWAY FROM ITS OWN QUESTION, which is the one shape the answer side is
 * structurally unable to see. A clone is taken AT the ask carrying FORK_PH_ASK on purpose, so it re-enters at
 * the top of that stage with the key still set and re-asks — that is what replays its arm out of the flow's
 * decision vector. A sibling that instead leaves the stage and asks a DIFFERENT fork reaches this function in
 * the ASK phase, so step_fork_answered_stage is never called at all, and the ask below simply OVERWRITES the
 * key and the stage with the new question's. The sibling then explores an arm of a question it was not forked
 * for while the arm it WAS forked for is taken by nobody: a work item dropped with every assert on the path
 * satisfied, which is what §NO BOUNDS calls a cap.
 *
 * IT REFUSES AN ASK ACROSS A STAGE, NOT AN ASK. Two forks legitimately stand inside one stage in sequence — an
 * elimination chain asks per position, and permission_store.c's §5.1 asks twice off one phase byte — because
 * only one of them is ever OUTSTANDING at a time, which is the state a zero key already reads as. */
static void step_fork_inflight(const JSStepHdr *h)
{
#if APICLIENT_DEV
    if (step_fork_pending(h)) {
        /* SIZED OVER THE FORMAT'S OWN MINIMUM PLUS THE THREE `%s`, never carved out of a round number: the
           compiler can see the literal and says so (-Wformat-truncation), and what a truncation drops is the
           TAIL, which is where this names what to do about it. */
        char why[1024];

        snprintf(why, sizeof why,
                 "%s asked a fork at stage %u (%s) while the one it asked at stage %u (%s) is still "
                 "outstanding — a fork's answer is replayed at the call site that asked it, so an ask from "
                 "another stage overwrites both halves of the outstanding question and the arm the flow's "
                 "decision vector recorded for it is taken by nobody. A SIBLING is the usual holder: it carries "
                 "FORK_PH_ASK on purpose so that it re-enters AT its ask and re-asks, and one that leaves that "
                 "stage first explores a question it was never forked for. Let the outstanding ask END before "
                 "leaving its stage, and give a sub-sequence that can still be in flight while another has "
                 "answered a STAGE of its own",
                 h->def->algorithm, (unsigned)h->stage, step_stage_label(h, h->stage),
                 (unsigned)h->fork_stage, step_stage_label(h, h->fork_stage));
        DCHECK(h->fork_stage == h->stage, why);
    }
#endif
}

/* THE FORK'S ONE BODY, under both of the questions that use it — see JSStepHdr.fork_kind. `kind` is the only
   thing that differs between an outcome fork and a ToBoolean, and it differs at exactly one line: which seam
   the DRIVER asks. Everything else — the two phases, the borrowed operands, the ask key, the range check on
   the answer — is one implementation, so the two questions cannot drift apart in their bookkeeping. */
static int step_fork_ask(JSContext *ctx, JSStepHdr *h, JSValueConst over, const char *op, int n, int real,
                         int kind, int *parm)
{
    (void)ctx;
    if (h->fork_phase == FORK_PH_ASK) {
        DCHECK(n >= 2, "a step machine declared a fork with fewer than two feasible completions — a single "
                       "completion is not a fork, it is the answer");
        DCHECK(real == JS_OUTCOME_REAL_UNSTATED || (real >= 0 && real < n),
               "a step machine declared a REAL completion that is not one of the completions it declared "
               "feasible — `real` is a completion index or JS_OUTCOME_REAL_UNSTATED, and a third value is a "
               "machine answering a different question here (the arm a non-forking run takes is the NUMBERING "
               "rule above, not this)");
        DCHECK(h->fork_op == NULL, "a fork was asked for while another one's operands were still on this "
                                   "machine's header");
        DCHECK(kind == JS_FORK_KIND_OUTCOME || kind == JS_FORK_KIND_TOBOOL,
               "a step machine's fork named neither of the two questions a fork can be — JS_FORK_KIND_NONE is "
               "the driver's reset value and names nothing, so an ask carrying it is one that never said which "
               "seam must answer it, and the driver would route a predicate to the wrong key space");
        /* THE STAGE AXIS OF THE SAME OVERLAP THE LINE ABOVE ASKS ABOUT ON THE OPERAND AXIS — `fork_op == NULL`
           sees a request whose operands never left, and this sees one whose SITE did. Asked before the writes
           below, because they are what destroys the evidence. */
        step_fork_inflight(h);
        /* A sibling re-asks the question it was forked at, under the key it carries: the walk's `real` is the
           one the first ask stated (JSStepHdr::fork_real_asked). */
        if (h->fork_ask_key != 0 && h->fork_ask_key == step_fork_key(op))
            real = h->fork_real_asked;
        else
            h->fork_real_asked = real;
        h->fork_over = over;   /* BORROWED for the length of the request; the driver reads and resets it */
        h->fork_op = op;
        h->fork_n = n;
        h->fork_real = real;
        h->fork_kind = (uint8_t)kind;
        /* WRITTEN ON EVERY ASK, including the sibling's first one: a clone carries the key of the ask it was
           forked at, and re-asking there overwrites it with the identical value. The STAGE is written with it
           and cleared with it — see JSStepHdr::fork_stage — so the two halves of one outstanding question can
           never be half-set, and a stale stage cannot outlive the key that names its question. */
        h->fork_ask_key = step_fork_key(op);
        h->fork_stage = h->stage;
        return JS_STEP_FORK;
    }
    DCHECK(h->fork_phase == FORK_PH_ANSWERED, "a step machine's fork resumed in no phase");
    /* THE SITE FIRST AND THE QUESTION SECOND, because the site is what the question cannot answer: two asks
       that spell one operation are indistinguishable to the key below, and the stage names them apart. */
    step_fork_answered_stage(h);
    DCHECK(h->fork_ask_key == step_fork_key(op),
           "a fork's answer was delivered to a DIFFERENT question than the one that asked for it. The "
           "machine resumed at a call site other than its outstanding ask, so the phase or stage it resumes on "
           "does not tell the two apart — the usual cause is an algorithm that DELEGATES to another one through "
           "the same phase byte while numbering its own phases in the same value space, which makes 'the inner "
           "chain is parked at its second question' read as 'the outer one is at its own'. Nothing else catches "
           "it: the arm is real and in range, and it was recorded under the ASKING operation's key, so consuming "
           "it here files one question's world as another's. Number a delegating algorithm's phases ABOVE the "
           "whole phase space of the one it delegates into");
    DCHECK(h->fork_arm >= 0 && h->fork_arm < n,
           "the fork answered with an arm this machine did not declare — a completion it never numbered, or a "
           "truth value that is neither true nor false");
    *parm = h->fork_arm;
    h->fork_phase = FORK_PH_ASK;   /* a machine may fork again — an iteration over unknown input does, per step */
    h->fork_arm = 0;
    h->fork_ask_key = 0;
    h->fork_stage = 0;             /* cleared WITH the key: neither half of an answered question survives it */
    return 0;
}

int step_fork_run(JSContext *ctx, JSStepHdr *h, JSValueConst over, const char *op, int n, int real, int *parm)
{
    return step_fork_ask(ctx, h, over, op, n, real, JS_FORK_KIND_OUTCOME, parm);
}

/* IS ONE OF THIS MACHINE'S FORKS OUTSTANDING? — the question `fork_phase` was being asked and cannot answer.
   A machine re-entered at a stage that both ISSUES A REQUEST and FORKS must know whether `cb_result` is its
   request's answer or the driver's filler, and there are THREE ways in rather than two: the request answering
   (nothing outstanding); the PARENT's fork delivery (FORK_PH_ANSWERED, re-entered with JS_UNDEFINED); and the
   SIBLING that fork minted, which carries FORK_PH_ASK ON PURPOSE — the clone is taken AT the ask so it re-asks
   and replays its arm out of its own decision vector — and is re-entered with its `park_in`, JS_UNDEFINED.
   SO `fork_phase != FORK_PH_ANSWERED` IS A TWO-WAY TEST OVER THREE ROUTES, and it groups the SIBLING with the
   request. Every prototype-chain walk in this file spelled it that way, and on every sibling arm each took the
   driver's UNDEFINED as its next LINK: the enumeration walk handed it to [[OwnPropertyKeys]] and aborted, two
   walks answered `false` for a chain nothing had walked, and the legacy accessor lookup dereferenced it as an
   object. One loud and three silent, from one predicate. The machines that got it right (JSON.parse's outcome
   fork, the array-callback coercion, the promise resolving function) all keep their operand on their own state
   and RELEASE `cb_result`, which is the shape that has no question to get wrong.
   THE ASK KEY IS THE FACT AND IT ALREADY EXISTED — written on every ask, cleared only when the answer is
   consumed, so it is set across BOTH entries that carry no link and clear on the one that does. Named here,
   beside the bookkeeping that owns it, because a fact answered from one place is what stops four sites
   disagreeing about it a fifth time. */
bool step_fork_pending(const JSStepHdr *h)
{
    return h->fork_ask_key != 0;
}

/* ADOPT A [[GetPrototypeOf]] ANSWER AS THIS WALK'S NEXT LINK. Taking `answer` BY POINTER and clearing it is
   the whole content: `cb_result` is an owned reference the step must release exactly once, so a bare
   `field = cb_result` leaves a SECOND NAME for the reference the field now owns, and any later free on that
   path drops the reference the field is standing on. The field is then handed to the solver seam a few lines
   down, where the use-after-free surfaces as the concolic brand check failing on a value that WAS concolic —
   an abort naming decide.c for a defect in this file. Four walks here adopt a link and three cleared by hand;
   consuming the source leaves a fifth nothing to forget. */
static void step_adopt_link(JSContext *ctx, JSValue *field, JSValue *answer)
{
    JS_FreeValue(ctx, *field);
    *field = *answer;
    *answer = JS_UNDEFINED;
}

