/* See rel_op.h. The opcode identities are reconstructed from quickjs's OWN table, in a translation unit whose
   only job is to hold them — the enum this include builds names every opcode in the engine, and a file that
   also did something else would carry 250 macro-defined identifiers into whatever it does next. */
#include "solver/rel_op.h"
#include "check.h"

/* THE ENGINE'S OPCODE ENUM, REBUILT FROM THE ENGINE'S OWN DEFINITION. quickjs.c builds `OPCodeEnum` from this
   same header with these same three macros (`FMT` discarded, `DEF` and `def` each emitting `OP_<id>`), and the
   DEF section carries no conditional compilation, so the two enums are the same sequence by construction. This
   is the established-tooling rule §Architecture states: quickjs already answers "what is OP_lt", and a second
   answer written by hand here would be the one that goes wrong quietly. */
typedef enum {
#define FMT(f)
#define DEF(id, size, n_pop, n_push, f) HOST_OP_ ## id,
#define def(id, size, n_pop, n_push, f)
#include "quickjs-opcode.h"
#undef def
#undef DEF
#undef FMT
    HOST_OP_COUNT,
} HostOPCode;

RelOp rel_op_of_opcode(int op) {
    switch (op) {
    case HOST_OP_lt:  return REL_LT;
    case HOST_OP_lte: return REL_LE;
    case HOST_OP_gt:  return REL_GT;
    case HOST_OP_gte: return REL_GE;
    default: break;
    }
    /* NOT A DEFAULT AND NOT A REL_NONE. quickjs.h contracts the `.rel` hook to exactly the four
       RelationalExpression productions §13.10.1 defines over §7.2.12 IsLessThan, so a fifth opcode here means
       the enum above no longer numbers the same table quickjs.c numbers — and the first symptom of that drift
       is not a missing bound, it is `x < 700` recorded as `x > 700`, which a report states with confidence.
       DCHECK rather than CHECK because it asserts the engine's own logic, and the assert is why the
       reconstruction is safe to rely on: it fires on the FIRST relational comparison a drifted build performs,
       which is inside the first script of the first document. */
    DFAIL("the relational hook was handed an opcode that is none of OP_lt/lte/gt/gte — quickjs.h contracts it "
          "to those four, so this is the host's rebuild of the opcode table having drifted from quickjs.c's, "
          "and the next thing it would do is name the wrong relation in a report");
    return REL_NONE;
}

RelOp rel_op_mirror(RelOp r) {
    switch (r) {
    case REL_LT: return REL_GT;
    case REL_LE: return REL_GE;
    case REL_GT: return REL_LT;
    case REL_GE: return REL_LE;
    case REL_NONE: break;
    }
    DFAIL("a relation with no spelling was asked which relation it is from the other side — REL_NONE is the "
          "absence of a bound and has no mirror, so a caller reaching here is normalising a fact it does "
          "not have");
    return REL_NONE;
}

RelOp rel_op_negate(RelOp r) {
    switch (r) {
    case REL_LT: return REL_GE;   /* §13.10.1: `<` and `>=` are one pair over IsLessThan(lval, rval, true) */
    case REL_GE: return REL_LT;
    case REL_GT: return REL_LE;   /* …and `>` and `<=` the other, over IsLessThan(rval, lval, false)        */
    case REL_LE: return REL_GT;
    case REL_NONE: break;
    }
    DFAIL("the false arm of a comparison that carries no relation was asked what it proves — an arm of a "
          "predicate with no bound proves nothing about a domain, and the caller must not be here");
    return REL_NONE;
}

const char *rel_op_spelling(RelOp r) {
    switch (r) {
    case REL_LT: return "<";
    case REL_LE: return "<=";
    case REL_GT: return ">";
    case REL_GE: return ">=";
    case REL_NONE: break;
    }
    return NULL;   /* a POSITIVE answer: there is no relation here to print */
}
