/* ONE LINK OF A NAME-KEYED ELIMINATION CHAIN.
 *
 * WHAT AN ELIMINATION CHAIN IS, IN ONE PARAGRAPH, BECAUSE THE SHAPE IS WHAT MAKES THE LIFT LEGITIMATE. An
 * unknown reaches a body that must decide WHICH MEMBER OF A SET it names — a timer identifier, a collection
 * position, a table key — and the algorithm tells apart one world per member plus ONE remainder in which it
 * names none of them. That is an N-way completion, and solver/decide.c already walks N-1 binary decisions for
 * a machine declaring `n`; what it cannot do is NAME them, because solver_outcome composes each completion's
 * key out of `"%d"` of its POSITION. So the chain is spelled as a sequence of ordinary two-armed asks, one per
 * member, each keyed by that member's own name — the SAME elimination sequence, not a second mechanism, and
 * with `n == 2` at every ask so the return protocol's ceiling (solver/decide.h's SOLVER_FORKED_BIT) is never
 * approached by a page holding more members than that.
 *
 * IT WAS WRITTEN TWICE AND THIS IS THE PART THAT WAS THE SAME BOTH TIMES. core/timing/timer.c's §8.7
 * clearTimeout/clearInterval chain and core/idl_index_arg.c's Web IDL §3.2.4.6 `unsigned long index` chain
 * differ in their enumeration, their example comparison and their remainder — and their LINKS were the same
 * five lines twice: compose the key, assert it was not truncated, run the two-armed fork, assert the arm came
 * back as one of the two, read the answer. Two copies of five lines is not the cost; the cost is that the
 * NAMING RULE those five lines implement lived in two places, and CLAUDE.md is explicit that the copy which
 * drifts is the one nobody runs against reality.
 *
 * THE RULE, RESTATED WHERE IT CAN BE CHECKED. Each link's key names the member's own IDENTITY and never its
 * RANK. §AN-INDEX-NAMES-A-THING-ONLY-WHILE-THE-SET-IS-FIXED: a rank is a fact about the operand only where the
 * set is the machine's own and fixed at its definition — a registry of algorithm names is, a global the page
 * mutates is not — and over a mutable set a replayed rank names a different member at the second ask. Nothing
 * catches it: every arm is in range, every assert on the path is satisfied, and the flow acts on a member no
 * answer of its own ever named. core/timing/timer.c's banner works the exact case out for a map one entry
 * shorter at a second `clearTimeout(id)`.
 *
 * WHY THE LOOP IS NOT HERE. Three of a chain's four parts are the CALLER'S and must stay there, and each of
 * them is a place where the two existing callers already disagree. The ENUMERATION is a fact about the
 * caller's own set (ascending positions to a length handed in, versus the least identifier a live map still
 * holds at or above a cursor). The EXAMPLE COMPARISON is decided by the operand's TYPE, and §3.2.4.6's
 * ConvertToInt(V, 32, "unsigned") and §3.2.4.5's ConvertToInt(V, 32, "signed") do not agree about a negative
 * operand — the timer chain casts through `(uint32_t)(int32_t)` precisely so identifiers at or above 2**31
 * stay reachable, which the index chain must not do. And the REMAINDER's ANSWER is the one thing
 * core/idl_index_arg.c says at length must never be shared: `item` returns null, CSSStyleDeclaration's returns
 * the empty string, remove-a-CSS-rule throws an IndexSizeError, §8.7's removal of an absent key does nothing —
 * a component that decided that would be deciding every one of those algorithms from one place. Lifting the
 * loop would have taken all three; lifting the link takes what they share and nothing else.
 *
 * THE ASSERTS NAME THE CALLER AND NOT THIS FILE. Every should-never-happen below stamps the line it is written
 * at, so an invariant checked inside a shared link would report THIS file for every member that reaches it —
 * §AN-ASSERT-THAT-NAMES-A-REMEDY's defect exactly, whose cure is that the site travels with the operation. It
 * travels as `algorithm`, which core/idl_index_arg.h argues is better than a `__FILE__`/`__LINE__` pair here:
 * the member's spec identity is stable across an edition of this tree in a way a coordinate is not. This is
 * one function rather than a macro for the same reason — the address is a VALUE the caller states, so there
 * is nothing an expansion at each site would add. */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "solver/concolic.h"
#include "core/idl_name_chain.h"

/* THE COMPOSED KEY'S ONE SPELLING, STATED ONCE BECAUSE THERE ARE TWO STORAGES FOR IT. Both entry points write
   this format and neither writes a format of its own: a constraint key is what a parked flow's recorded
   answers are filed under, so two spellings of one question would file it under two names and a flow reaching
   the same predicate through the other door would find nothing recorded. It is also the string both existing
   callers wrote by hand before the link was lifted, byte for byte. */
#define NAME_CHAIN_FMT "%s %s)"

/* EVERYTHING BOTH ENTRY POINTS ASK OF THEIR CALLER, BEFORE EITHER OF THEM COMPOSES ANYTHING. It is a function
   rather than a copy in each because the two differ ONLY in where the key's bytes live — every precondition
   below is about the QUESTION, which is the same question — and because a second copy of an assert is a second
   place for it to stop being made. `has_key` is the caller's own storage test, which is the one precondition
   whose subject differs between the two: an inline array is a field and cannot be absent, a supplied key is a
   pointer to a struct that can. */
static void chain_ask_checks(JSValueConst over, const char *predicate, const char *member, int real,
                             const char *algorithm, bool has_key, bool has_answer)
{
    DCHECK(algorithm != NULL && algorithm[0] != '\0',
           "a name-keyed elimination link was asked for a member that did not name itself — the name is the "
           "ADDRESS every assert in this file reports, so an unnamed one aborts naming no site and sends its "
           "reader to a shared component instead of to the member that reached it");
    DCHECKF(predicate != NULL && predicate[0] != '\0',
            "%s asked an elimination link with no predicate — the predicate is half the composed constraint "
            "key, and an empty one files this question under a key that is the member name alone, which every "
            "other asker over the same unknown would collide with",
            algorithm);
    /* ABSENT AND EMPTY ARE TWO DIFFERENT ANSWERS AND ONLY ONE OF THEM IS A DEFECT, WHICH IS WHY THE EMPTY-NAME
       HALF IS NOT ASKED HERE. A NULL is a caller with nothing to compose. An EMPTY STRING is a member whose
       name is the empty string, which for a page-supplied name is an ordinary member — `console.count("")`
       stores under it — and the composition tells it apart from every other member for the same reason it
       tells any two apart: no other name spells `"<predicate> )"`. The two uint32 askers cannot produce one
       (a decimal always spells at least one digit) and each asserts that at its own entry, where the reason
       is about that caller's own names rather than about names in general. */
    DCHECKF(member != NULL,
            "%s asked an elimination link about a member that has no name — the whole of what this component "
            "is for is that a completion carries its NAME and never its rank, so an absent name is the one "
            "input for which there is nothing to compose and nothing sound to fall back on",
            algorithm);
    DCHECKF(has_key && has_answer,
            "%s asked an elimination link with nowhere to spell its key or nowhere to put its answer — the "
            "key's storage must be on the machine's state because step_fork_run keeps a BORROWED pointer to "
            "it that the driver reads after this returns",
            algorithm);
    /* THE OPERAND IS UNKNOWN EXTERNAL INPUT, WHICH IS THE WHOLE PRECONDITION FOR ASKING AT ALL. A converted
       value denotes exactly one member and the caller reads it directly; forking over a number it already has
       would mint a sibling per member for a question the value answers. Both callers establish this before
       their loop — one by an assert at entry, one by branching on it — and asserting it per LINK is what makes
       a caller that later grows a second entry point unable to reach here the wrong way. */
    DCHECKF(concolic_is(over),
            "%s ran a name-keyed elimination link over an operand that is NOT unknown external input — a "
            "converted value names exactly one member and is read directly, so a known value here means the "
            "caller routed the wrong arm and is about to fork over something it already knows",
            algorithm);
    /* WHICH ARM THE EXAMPLE REACHES IS THE CALLER'S SECOND DECLARATION, and there are exactly three things it
       can say: this link's member (1), not this link's member (0), or nothing at all. A fourth value would be
       a completion index this two-armed ask does not have, and step_fork_run would read it as a claim about a
       world that does not exist. */
    DCHECKF(real == 0 || real == 1 || real == JS_OUTCOME_REAL_UNSTATED,
            "%s stated an example arm that is neither of this link's two nor the sentinel for having no "
            "example — a link is a two-armed question, and JS_OUTCOME_REAL_UNSTATED is what says the operand "
            "carries no example rather than a third completion",
            algorithm);
}

/* THE FORK ITSELF, ONCE THE KEY IS SPELLED — the half that does not care which storage spelled it. `op` is
   BORROWED and must live in the machine's state, which is what the two entry points' key types are; this
   function never sees the storage and so cannot be the place either one's ownership goes wrong. */
static int chain_ask_fork(JSContext *ctx, JSStepHdr *hdr, JSValueConst over, const char *op, int real,
                          const char *algorithm, bool *pyes)
{
    int arm = 0, rc;

    rc = step_fork_run(ctx, hdr, over, op, 2, real, &arm);
    if (rc)
        return rc;   /* JS_STEP_FORK: parked at the ask, and the sibling's snapshot was taken there */
    DCHECKF(arm == 0 || arm == 1,
            "%s asked a two-armed outcome fork and was answered with an arm that is neither of them",
            algorithm);
    /* OUTCOME 0 IS "NO", which is step_fork_run's one numbering rule read against this predicate. Both callers
       need that and each states its own reason at its own site; this component only holds the mapping. */
    *pyes = (arm == 1);
    return 0;
}

int idl_name_chain_ask(JSContext *ctx, JSStepHdr *hdr, IdlNameChainKey *key, JSValueConst over,
                       const char *predicate, const char *member, int real, const char *algorithm,
                       bool *pyes)
{
    int wrote;

    chain_ask_checks(over, predicate, member, real, algorithm, key != NULL, pyes != NULL);
    /* THIS ENTRY'S CALLERS SPELL THEIR OWN NAMES AND A DECIMAL IS NEVER EMPTY, which is the fact the shared
       checks above deliberately do not assume — a supplied name may legitimately be the empty string. An empty
       one HERE is a caller that wrote nothing into its own name buffer, which composes a key naming no member
       at all and would let every such call share one question. */
    DCHECKF(member[0] != '\0',
            "%s spelled an EMPTY member name into an inline elimination key — this entry point's callers "
            "derive a name from the operand's own type (a uint32 always spells at least one digit), so an "
            "empty one is a name buffer nothing wrote and every call that reached here that way would file "
            "one question under one key",
            algorithm);
    /* THE COMPOSITION, AND IT IS FROZEN. See the header: a caller's spelling is what its recorded answers are
       filed under, in this session and out of the cold tier in the next, so this format is what both existing
       callers already wrote by hand and is byte-identical to it. */
    wrote = snprintf(key->op, sizeof key->op, NAME_CHAIN_FMT, predicate, member);
    /* THE RUN-TIME HALF OF THE TRUNCATION CHECK. IDL_NAME_CHAIN_SPELLS settles it at COMPILE time wherever the
       predicate is a literal and the name's width is arithmetic, which is both callers today; this catches the
       case that check cannot see, which is a caller that was WRONG about its own widest name. The member's
       name is the LAST thing in the string, so a buffer one byte short names a DIFFERENT member's question and
       the flow answers it with this one's record — the fabricated timeline the naming rule exists to prevent,
       reached by a different road. snprintf reports what it WOULD have written, which is the only way to see
       it. */
    DCHECKF(wrote > 0 && (size_t)wrote < sizeof key->op,
            "%s could not spell the member its question is about — the name is the tail of the constraint key, "
            "so a truncated one merges two members' questions into one and lets one link's record decide "
            "another's; declare this asker's widest name with IDL_NAME_CHAIN_SPELLS, and if it has none "
            "because the page supplies it, move this asker to IdlNameChainSuppliedKey, whose buffer is grown "
            "to the composition instead of being sized against a literal",
            algorithm);
    return chain_ask_fork(ctx, hdr, over, key->op, real, algorithm, pyes);
}

void idl_name_chain_supplied_visit(JSContext *ctx, IdlNameChainSuppliedKey *key, JSStepVisit *v)
{
    DCHECK(key != NULL && v != NULL, "a machine named a supplied elimination key with no key or no visitor — "
                                     "the visit is how a fork COPIES the buffer, so a machine that skips it "
                                     "hands its sibling a pointer into storage the parent's discharge frees");
    /* THE POINTER AND ITS ALLOCATION ARE ONE FACT AND THE VISIT IS WHERE THEY ARE READ TOGETHER, which is the
       whole reason this is a function here rather than a `v->buf` line at each machine: the copy's size and
       the free's subject come from the same two fields, read once. */
    DCHECK((key->op != NULL) == (key->cap != 0),
           "a supplied elimination key holds a buffer with no allocation size, or a size with no buffer — the "
           "two are written together by idl_name_chain_ask_supplied and the zeroed state a machine arrives in "
           "is NULL and 0, so a half-set pair means something wrote one of them directly");
    v->buf(ctx, (void **)&key->op, key->cap);
}

int idl_name_chain_ask_supplied(JSContext *ctx, JSStepHdr *hdr, IdlNameChainSuppliedKey *key, JSValueConst over,
                                const char *predicate, const char *member, int real, const char *algorithm,
                                bool *pyes)
{
    int need, wrote;

    chain_ask_checks(over, predicate, member, real, algorithm, key != NULL, pyes != NULL);
    /* MEASURE, THEN GROW, THEN WRITE — which is why there is no truncation to detect here and no cap to raise.
       snprintf's return is what it WOULD have written, and C11 §7.21.6.5 The snprintf function's Returns
       paragraph gives that answer for a null buffer and a zero size, so the composition is measured with the
       very format that will write it rather than with an arithmetic guess about it. The inline key's compile-time IDL_NAME_CHAIN_SPELLS has nothing to check
       for a caller whose member names the PAGE supplies, and §NO BOUNDS forbids the obvious substitute — a cap
       on how long a label may be is a bound, and one that silently merges two members' questions at that. */
    need = snprintf(NULL, 0, NAME_CHAIN_FMT, predicate, member);
    CHECKF(need > 0, "%s could not measure the constraint key its elimination link is about", algorithm);
    if ((size_t)need + 1 > key->cap) {
        /* GROW ONLY, NEVER SHRINK, so the `visit`'s copy size and the discharge's free stay one fact that
           cannot lag the buffer. A failed allocation is fatal in both builds for §Offensive-programming's
           reason: a dropped flow corrupts the frontier, and there is no arm here that could carry on without
           a key to file its question under. */
        char *grown = js_realloc(ctx, key->op, (size_t)need + 1);

        CHECKF(grown != NULL, "%s could not allocate the constraint key its elimination link is about",
               algorithm);
        key->op = grown;
        key->cap = (size_t)need + 1;
    }
    wrote = snprintf(key->op, key->cap, NAME_CHAIN_FMT, predicate, member);
    /* THE TWO CALLS ARE THE SAME FORMAT OVER THE SAME ARGUMENTS, so a disagreement is not a truncation — the
       buffer was grown to `need` — but a caller mutating `predicate` or `member` between them, which nothing
       on this path can do and an asker with a live JSString behind its name could. */
    DCHECKF(wrote == need,
            "%s spelled a different constraint key than it measured — the measure and the write are one format "
            "over one pair of arguments, so a difference means the member's bytes changed between them",
            algorithm);
    return chain_ask_fork(ctx, hdr, over, key->op, real, algorithm, pyes);
}
