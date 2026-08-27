/* WHICH SPEC RELATION THE INTERPRETER IS PERFORMING — one problem, one file.
 *
 * ECMAScript §13.10.1 Runtime Semantics: Evaluation defines four RelationalExpression productions over
 * §7.2.12 IsLessThan ( x, y, leftFirst ), and quickjs emits one opcode per production. The solver's relational
 * hook is handed that OPCODE and nothing else (see quickjs.h's `.rel`: "`op` is the OP_lt/lte/gt/gte opcode"),
 * which is an IDENTITY and not a NAME — enough to tell four predicates apart, not enough to STATE one.
 *
 * §Solver-half needs it stated. A shape carries two facts, and the second — the DOMAIN this flow's own
 * predicates narrowed the value to — is unspellable while the relation is a number: `rel42` is a fact the run
 * observed and cannot report, which renders a range-gated parameter with the same bytes as one nothing tested.
 *
 * THE MAPPING IS READ OFF THE ENGINE'S OWN OPCODE TABLE, never written down here. quickjs-opcode.h is the one
 * definition of what OP_lt is, and this file includes it with the same X-macro the interpreter's own enum is
 * built from, so a reordered table renumbers both at once. A hand-copied number would be the module-static
 * defect §Browser-half describes — one fact answered from two places, silently disagreeing after an upstream
 * merge, and disagreeing in the direction that reports `x < 700` as `x > 700`. rel_op_of_opcode closes even
 * that: an opcode outside the four this hook is contracted to receive CRASHES rather than picking a relation.
 */
#ifndef ENGINE_HOST_SOLVER_REL_OP_H
#define ENGINE_HOST_SOLVER_REL_OP_H

/* The four relations, SUBJECT-ON-THE-LEFT. `REL_NONE` is a positive statement: this comparison result carries
   no bound at all (both operands unknown, or the concrete one is not a finite Number — see concolic.c). */
typedef enum { REL_NONE = 0, REL_LT, REL_LE, REL_GT, REL_GE } RelOp;

/* The interpreter's relational opcode -> the relation, as WRITTEN (`a < b` -> REL_LT, subject not yet
   normalised). Aborts on anything else: quickjs.h contracts `.rel` to the four, so a fifth opcode arriving here
   is the reconstruction below having drifted from the table it was built from, and guessing a relation for it
   would put a WRONG bound in a report rather than an absent one. */
RelOp rel_op_of_opcode(int op);

/* THE SAME RELATION READ FROM THE OTHER OPERAND'S SIDE — `5 < x` is `x > 5`. Ordering is not symmetric, so the
   solver normalises the SUBJECT (the unknown operand) to the left exactly once, here, rather than at each of
   the two places that would otherwise have to remember which side it was on. */
RelOp rel_op_mirror(RelOp r);

/* THE RELATION THE FALSE ARM STANDS ON, per §13.10.1's own definitions: `<` and `>=` are one production pair
   over IsLessThan(lval, rval, true), `>` and `<=` the other over IsLessThan(rval, lval, false).
   IT IS NOT EXACTLY THE COMPLEMENT AND THE DIFFERENCE IS NAMED RATHER THAN HIDDEN. §13.10.1 returns false for
   `<` when IsLessThan is undefined AND returns false for `>=` in the same case — §7.2.12's undefined means
   "the operands could not be coerced to comparable values of the same numeric type" — so the false arm of
   `x < 5` is `x >= 5` OR x incomparable, and this answers REL_GE for it. That is a NARROWING of the observed
   domain, which is the safe direction for a report whose job is to say what a reviewer must SEND: every value
   the narrowed domain admits does take the arm the flow took. The residue (a parameter whose numeric coercion
   is NaN — `?page=abc` reaching the else of `if (page < 5)`) is a value no bound could state anyway. */
RelOp rel_op_negate(RelOp r);

/* The relation as a reviewer reads it: "<", "<=", ">", ">=". NULL for REL_NONE — there is no spelling of
   "no relation", and a caller that wants one is about to print a constraint it does not have. */
const char *rel_op_spelling(RelOp r);

#endif
