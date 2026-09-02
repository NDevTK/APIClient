/* A WEB IDL `unsigned long index` ARGUMENT, KNOWN AND UNKNOWN.
 *
 * ONE FAMILY, ONE MECHANISM. Eleven members of this engine take an `unsigned long index` that names a position
 * in a collection, and every one of them asks the same two questions of it: which position is it, and is it
 * past the end. They are the `item(index)` members of CSSRuleList, StyleSheetList, MediaList, DOMRectList,
 * DOMStringList, FileList, NodeList and HTMLCollection, NamedNodeMap, CSSStyleDeclaration and DOMTokenList,
 * plus CSSOM §6.4 CSS Rules' remove a CSS rule and insert a CSS rule, which the two `deleteRule`s and the two
 * `insertRule`s are declared over.
 *
 * WHAT DIFFERS BETWEEN THEM IS NOT THE QUESTION. It is what the algorithm says about the past-the-end world —
 * `item` returns null, CSSStyleDeclaration's returns the empty string, remove-a-CSS-rule throws an
 * IndexSizeError — and that is the CALLER's, stated at the caller, which is why this file answers with a flag
 * and never with a value or a throw of its own. It is also the one thing that must not be shared: a component
 * that decided the past-the-end answer would be deciding eleven different specs from one place.
 *
 * IT IS core/timing/timer.c's clearTimeout CHAIN AND DELIBERATELY NOT A SECOND MECHANISM. An N-way ask whose
 * completions were positions would file one key per COLLECTION LENGTH and press the return protocol's ceiling
 * (solver/decide.h's SOLVER_FORKED_BIT) for a document with many nodes, where a chain of two-armed asks forks
 * ONE sibling per link, is drawn lazily as the scheduler picks the machine up again, and keeps `n == 2` at
 * every ask.
 *
 * THE ORDER IS ASCENDING AND THE PARENT SITS ON THE EXAMPLE'S ARM AT EVERY LINK. The sequence is a function of
 * `npositions` alone, so it is the same sequence in a sibling's snapshot, after a park, and in a session that
 * resumes the flow from the cold tier; the parent answers each question with what its own example says (NO at
 * every position the example is not, YES at the one it is), so the example marks the real arm primary at every
 * link, which is §Learning-from-replies' rule. With NO example the parent walks the whole chain to the
 * past-the-end world and one sibling per position carries an element — neither arm marked forced, because
 * nothing was contradicted.
 *
 * OUTCOME 0 IS "NO" AT EVERY LINK, which is step_fork_run's one numbering rule read against this predicate,
 * and the rule holds for BOTH kinds of caller here for two different reasons that agree. For a member that
 * MUTATES — CSSOM §6.4's remove a CSS rule — a numbering that put the removal on the arm a run with no forking
 * policy takes would let an @S candidate re-fire tear out the very rule whose sink it is running to reach. For
 * a member that only READS — every `item(index)` — the same numbering is what keeps the no-policy walk from
 * ASSERTING a position: answering YES anywhere would hand the page an element on the strength of an index
 * nobody knows, which is §Solver's invented value, while walking to exhaustion states only what the type
 * already permits. The two callers do not need two numberings, which is part of why one chain serves both.
 *
 * THE HEADER CARRIES THE REST OF THE DESIGN; this file carries the two implementations and their asserts. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "solver/concolic.h"
#include "core/idl_args.h"
#include "core/idl_index_arg.h"

void idl_index_chain_visit(JSContext *ctx, void *state, JSStepVisit *v)
{
    /* NOTHING IS OWNED, so the visit names nothing — the state is a cursor and the buffer its key is spelled
       into, neither of them a JSValue. See the header for why it is declared at all. */
    (void)ctx; (void)state; (void)v;
}

int idl_index_chain_run(JSContext *ctx, JSStepHdr *hdr, IdlIndexChain *c, JSValueConst index_v,
                        uint32_t npositions, const char *algorithm, uint32_t *pindex, bool *ppast_end)
{
    double num = 0;
    int have;

    DCHECK(algorithm != NULL && algorithm[0] != '\0',
           "an index elimination chain was run for a member that did not name itself — the name is the "
           "operation half of the constraint key AND the address every assert in this file reports, so an "
           "unnamed one files its questions under a key no member owns and aborts naming no site");
    DCHECKF(pindex != NULL && ppast_end != NULL,
            "%s ran the index elimination chain with nowhere to put its answer — the chain has TWO answers "
            "and both are out-parameters, so a missing one is a world this member would never be told about",
            algorithm);
    DCHECKF(concolic_is(index_v),
            "%s ran the index elimination chain over an index that is NOT unknown external input — a "
            "converted `unsigned long` is idl_index_arg_known's to read and reaches its position directly, so "
            "a known value here means the caller routed the wrong arm and would fork over a number it has",
            algorithm);
    /* THE EXAMPLE IS READ ONCE PER ENTRY AND ABOVE THE CHAIN, so every question this activation asks is
       decided against ONE reading of it — taking it again inside the loop would be two reads of one example
       with nothing forcing them to agree. `have` is 0 for an unknown carrying no example, which is a POSITIVE
       fact the chain reads as JS_OUTCOME_REAL_UNSTATED and never as a position to fall back on. */
    have = idl_number_of(ctx, IDL_UNSIGNED_LONG, index_v, &num);

    for (;;) {
        uint32_t k = c->next;
        int arm = 0, real, rc, wrote;

        /* EVERY POSITION ELIMINATED, WHICH IS THE ALGORITHM'S OWN PAST-THE-END WORLD. §3.2.4.6 unsigned long
           admits no value below 0, so "none of 0 ... npositions-1" and "at or past npositions" are the same
           statement about this value. It is also the WHOLE answer for an EMPTY collection, which is why an
           empty one asks no question: one feasible completion is not a fork, it is the answer. */
        if (k >= npositions) {
            *ppast_end = true;
            return 0;
        }
        /* THE MACHINE'S SECOND DECLARATION — which completion this operation reaches on the operand's own
           EXAMPLE, computed by RUNNING the comparison rather than by a rule predicting it. §3.2.4.6's
           ConvertToInt(V, 32, "unsigned") has already been run on the example above, through the one copy of
           that arithmetic, so its result is an integer in [0, 2**32-1] and the cast below is exact. */
        real = have ? ((uint32_t)num == k) : JS_OUTCOME_REAL_UNSTATED;
        wrote = snprintf(c->op, sizeof c->op, "%s (index is %u)", algorithm, (unsigned)k);
        /* A TRUNCATED OPERATION STRING MERGES TWO PREDICATES. The position is the LAST thing in this string,
           so a buffer one byte short would file two positions under one key and let one link's record decide
           another's — and because the prefix is the CALLER's, the length that overflows is a fact about the
           member rather than about this file. snprintf reports what it WOULD have written, which is the only
           way to see it. */
        DCHECKF(wrote > 0 && (size_t)wrote < sizeof c->op,
                "%s could not spell the position its question is about — the operation string is half the "
                "constraint key and the position is its tail, so a truncated one names a DIFFERENT position's "
                "question and the flow answers it with this one's record; the member's own name is what "
                "overflowed the buffer, so shorten the name or widen IdlIndexChain::op",
                algorithm);
        rc = step_fork_run(ctx, hdr, index_v, c->op, 2, real, &arm);
        if (rc)
            return rc;
        DCHECKF(arm == 0 || arm == 1,
                "%s asked a two-armed outcome fork and was answered with an arm that is neither of them",
                algorithm);
        /* NAMED RESIDUAL — the DECISION is recorded and the value's DOMAIN is not, which is narrower than
           §Solver's concretize-on-pin and is not wrong: an unnarrowed value keeps arms, and keeping an arm is
           the sound direction. WHAT IS NOT COVERED: each link is an equality over the index — the YES arm
           proves it IS this position and the NO arm proves it is NOT — and neither fact reaches the value;
           only the decision vector holds them, and a vector answers the question it recorded and no other.
           WHAT THE NEXT DIFF BUILDS: the pin and the exclusion solver/decide.c already takes at a bytecode
           equality, taken over the OPERAND's own identity rather than off a comparison RESULT this seam does
           not have — the same piece core/timing/timer.c's own chain names, and one piece for both.
           HOW ITS ABSENCE SHOWS: a flow that has answered YES at some position and then reaches a SECOND
           member of this family mints a sibling for every OTHER position that member's collection holds —
           worlds its own path has already contradicted — and an @H parameter carrying this value renders with
           no domain beside it where the run observed one. */
        if (arm == 1) {
            /* THIS WORLD'S ANSWER: the index IS this position. `npositions` was read by the caller at the top
               of this same entry and nothing has run since — step_fork_run runs none of the page's code and
               the driver only clones and re-enters — so the caller's own read of its collection cannot
               disagree with the one this chain was drawn against. */
            *pindex = k;
            *ppast_end = false;
            return 0;
        }
        /* ELIMINATED: the index is not this position, so the next question is about the next one. The cursor
           is advanced AFTER the answer and never before the ask, which is what makes the two arms agree: the
           sibling's snapshot was taken with the cursor still at `k`, so it re-asks about the same position and
           answers the other way. */
        c->next = k + 1;
    }
}

uint32_t idl_index_arg_known(JSContext *ctx, JSValueConst index_v, const char *algorithm)
{
    double d = 0;
    int have;

    DCHECK(algorithm != NULL && algorithm[0] != '\0',
           "a converted `unsigned long index` was read for a member that did not name itself — the name is "
           "the address the asserts below report, and without it a wrong conversion aborts naming no site");
    DCHECKF(!concolic_is(index_v),
            "%s read its `index` as a converted number when it is UNKNOWN external input — this is the read "
            "that owes C a real number and cannot have one, and the fork it needs instead is "
            "idl_index_chain_run",
            algorithm);
    have = idl_number_of(ctx, IDL_UNSIGNED_LONG, index_v, &d);
    DCHECKF(have,
            "%s got no number for an `index` this arm has already established is NOT unknown external input "
            "— idl_number_of answers 0 only for an unknown carrying no example, so a 0 here is a value that "
            "is neither a Number nor a concolic reaching a body whose declaration converts its index",
            algorithm);
    /* §3.2.4.6 `unsigned long`'s own postcondition: §3.2.4.9 Abstract operations' ConvertToInt takes the
       integer part modulo 2**32, so the result is always an integer in [0, 2**32-1] — NaN and both infinities
       became +0 in the conversion. */
    DCHECKF(d >= 0 && d <= 4294967295.0 && d == (double)(uint32_t)d,
            "%s was given something that is not an `unsigned long` — §3.2.4.6's conversion result is the "
            "integer part taken modulo 2**32, so a value outside that range, or one with a fraction, means "
            "this argument was never converted by anything",
            algorithm);
    return (uint32_t)d;
}
