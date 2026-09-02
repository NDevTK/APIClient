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

int idl_name_chain_ask(JSContext *ctx, JSStepHdr *hdr, IdlNameChainKey *key, JSValueConst over,
                       const char *predicate, const char *member, int real, const char *algorithm,
                       bool *pyes)
{
    int arm = 0, rc, wrote;

    DCHECK(algorithm != NULL && algorithm[0] != '\0',
           "a name-keyed elimination link was asked for a member that did not name itself — the name is the "
           "ADDRESS every assert in this file reports, so an unnamed one aborts naming no site and sends its "
           "reader to a shared component instead of to the member that reached it");
    DCHECKF(predicate != NULL && predicate[0] != '\0',
            "%s asked an elimination link with no predicate — the predicate is half the composed constraint "
            "key, and an empty one files this question under a key that is the member name alone, which every "
            "other asker over the same unknown would collide with",
            algorithm);
    DCHECKF(member != NULL && member[0] != '\0',
            "%s asked an elimination link about a member that has no name — the whole of what this component "
            "is for is that a completion carries its NAME and never its rank, so an absent name is the one "
            "input for which there is nothing to compose and nothing sound to fall back on",
            algorithm);
    DCHECKF(key != NULL && pyes != NULL,
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
    /* THE COMPOSITION, AND IT IS FROZEN. See the header: a caller's spelling is what its recorded answers are
       filed under, in this session and out of the cold tier in the next, so this format is what both existing
       callers already wrote by hand and is byte-identical to it. */
    wrote = snprintf(key->op, sizeof key->op, "%s %s)", predicate, member);
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
            "because the page supplies it, this key's inline storage is the wrong one for it",
            algorithm);
    rc = step_fork_run(ctx, hdr, over, key->op, 2, real, &arm);
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
