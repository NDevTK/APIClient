/* CSS TYPED OM 1 §4.3.4 Complex Numeric Values: CSSMathValue objects, and §6.5 CSSMathValue Serialization —
 * the OPERATION NODES of a numeric value, over core/css/css_unit_value.h's leaves.
 *
 * §4.3.4's own first sentence is the whole shape: "Numeric values that are more complicated than a single
 * value+unit are represented by a tree of CSSMathValue subclasses, eventually terminating in CSSUnitValue
 * objects at the leaf nodes." So this component owns a TREE and core/css/css_unit_value.c owns a RECORD, which
 * is why they are two files: every algorithm here is a walk with a stack under it, and not one of them is
 * about the state of a single object.
 *
 * WHY IT EXISTS AT ALL, WHICH IS THE PART THREE COMPONENTS WERE WAITING ON. §4.3.1's six arithmetic operations
 * — add, sub, mul, div, min, max — each RETURN an object of this section on the arm where their operands do
 * not collapse to one unit. That is not a rare arm: `CSS.px(1).add(CSS.em(2))` takes it, and so does every
 * mixed-unit expression a page writes. A member answering only the collapsing arm would be WRONG for input a
 * browser answers rather than narrower, so all six stayed absent until there was something for them to return.
 *
 * WHAT IS HONESTLY ABSENT, AND EACH IS ABSENT FOR A REASON WRITTEN DOWN AT ITS SITE.
 *   §4.3.4's CSSMathClamp — SEE THE RESIDUAL BELOW. It is the one interface in this section that two of the
 *   normative algorithms it must take part in have no arm for at all, in the published draft.
 *   §4.3.1's `toSum` and its create-a-sum-value used to stand here, and they do not any more: the six arms of
 *   create-a-sum-value that walk THIS section's tree are core/css/css_sum_value.c, which is where the
 *   (value, unit map) tuple list, the product of two unit maps and create-a-type-from-a-unit-map live. This
 *   component is what that walk reads — `css_math_value_op`, `css_math_value_items` — and owns none of it.
 *
 * THE TYPE IS COMPUTED ONCE, AT THE MINT, AND THAT IS NOT A CACHE — IT IS WHAT KEEPS THE C STACK FLAT.
 * §4.3.4 states a math value's type recursively ("the result of adding the types of each of the items in its
 * values internal slot"), and a C function that walked the tree to answer `type()` would be a self-contained
 * recursion whose depth the PAGE picks — `for (…) v = new CSSMathNegate(v)` — which engine/check_recursion.mjs
 * fails by name and which CLAUDE.md's flat-C-stack rule forbids outright. The type is IMMUTABLE: the `values`,
 * `value`, `lower` and `upper` slots are `readonly`, and a leaf's contribution is its `unit` slot, which
 * §4.3.3 also declares `readonly`. (A leaf's `value` IS writable and the type does not read it.) So the answer
 * is stored, each mint reads only its CHILDREN's stored answers, and no walk is ever needed.
 *
 * THE TYPE STEP BELONGS TO THE CALLER AND THE MINT IS THE MINT — they were one entry and are two, because the
 * standard has THREE callers and only two of them refuse. §4.3.4's four list constructors state "Let type be
 * the result of adding the types of all the items of args. If type is failure, throw a TypeError", and so do
 * §4.3.1's add, mul, min and max in their own words. Its CSSMathNegate and CSSMathInvert constructors state NO
 * type step at all. And §4.3.1's `toSum` ends at "Return a new CSSMathSum object whose values internal slot is
 * set to result" — the SPEC-INTERNAL mint, with no type step — so `CSS.px(1).toSum("px", "s")` is a CSSMathSum
 * of `1px` and `0s` whose type is FAILURE. THAT READING IS CONTESTED BY A CONFORMANCE FILE AND THE ARGUMENT
 * FOR IT IS WRITTEN OUT AT `nv_to_sum_result` IN core/css/css_numeric_value.c — read it before changing this,
 * because it also states what one line of this component would become unreachable if the reading flipped.
 * A mint that refused for the caller
 * therefore could not serve toSum, and a TypeError there would be a refusal §4.3.1 does not state. So the mint
 * always mints and STORES whatever type the table gives it, failure included (core/css/css_math.h's
 * `css_math_type_failure`), and the two callers that have a step 3 run it themselves, before they mint, in the
 * order their own algorithms state it.
 *
 * THE `values` SLOT IS ITS CSSNumericArray, MINTED AT THE CONSTRUCTOR AND NEVER AT A READ. Two facts force it
 * and they point the same way. A getter that minted one per read would answer a DIFFERENT object each time,
 * which `a.values === a.values` sees; and minting inside a read is a WRITE to shared baseline state performed
 * by a mere read, which leaves a COW delta entry for a flow that only looked. The list inside it is a JS Array
 * for CLAUDE.md's own reason — a malloc'd list captured as pointers reverts the POINTERS on a context switch
 * and leaves the nodes reachable from nothing, a leak the runtime's own GC walk cannot see.
 *
 * §6.5's SERIALIZATION OF AN UNKNOWN IS §6.4's RULE ONE LEVEL UP, AND THE LEVEL IS WHAT MAKES IT DIFFERENT.
 * core/css/css_unit_value.c derives an unknown STRING from ONE unknown `value` slot through solver/concolic.h's
 * builtin seam, carrying the REAL §6.4 run on that slot's example. A math value has N leaves and any subset of
 * them may be unknown, so a derivation naming ONE of them would give two trees differing only in a leaf it did
 * not name a single identity — the defect core/html/html_progress.c wrote down as a residual. This walk
 * therefore runs the REAL §6.5 over every leaf's own example and derives the result through
 * `concolic_new_derived`, naming every unknown leaf IN SERIALIZATION ORDER. Order, not a set: §6.5's output is
 * a string in which the leaves appear in that order, so `calc({a} + {b})` and `calc({b} + {a})` are two
 * different strings and must not be one key.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_MATH_VALUE_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_MATH_VALUE_H

#include <stdbool.h>

#include "quickjs.h"

#include "core/css/css_math.h"

/* §4.3.4's `CSSMathOperator` enumeration, which is also this component's dispatch: "sum", "product", "negate",
   "invert", "min", "max", "clamp". The `operator` attribute's steps are stated as one table from the INTERFACE
   to one of these strings, so the kind and the operator are one fact and there is no second table.
   CLAMP IS NOT HERE — see the residual on `css_math_value_new` for the two normative algorithms that have no
   arm for it, and note that its ABSENCE from this enumeration is what makes a `clamp` arm impossible to write
   accidentally rather than a thing a reader has to remember. */
typedef enum {
    CSS_MATH_OP_SUM = 0,
    CSS_MATH_OP_PRODUCT,
    CSS_MATH_OP_NEGATE,
    CSS_MATH_OP_INVERT,
    CSS_MATH_OP_MIN,
    CSS_MATH_OP_MAX,
    CSS_MATH_OP_N
} CssMathOp;

/* Does this operator hold a LIST (§4.3.4's `values` internal slot, a CSSNumericArray) or a SINGLE operand (its
   `value` internal slot, a CSSNumericValue)? Sum, product, min and max declare `CSSNumberish... args` and the
   array; negate and invert declare one `CSSNumberish arg` and the single value. ONE predicate, because the
   arity of the constructor, the name of the attribute and the shape of the slot are the same fact. */
bool css_math_op_is_list(CssMathOp op);

/* §4.3.4's spec-internal mint — "Return a new CSSMathSum whose values internal slot is set to args", and the
 * same sentence for the other five.
 *
 * `items` is an array of `n` CSSNumericValues, BORROWED (each is dup'd into the object). For a list operator
 * `n` is §4.3.4's `args` and must be at least 1 ("If args is empty, throw a SyntaxError" is the CALLER's step,
 * because a SyntaxError is a JS exception and this entry is also reached from §4.3.1's arithmetic, whose lists
 * are never empty). For a single-operand operator `n` is exactly 1.
 *
 * IT ALWAYS MINTS, AND IT NEVER REFUSES. The type is computed here because it has to be STORED (see the
 * header), and a FAILURE type is stored like any other. The refusal is the CALLER's own step and is run BEFORE
 * the call through `css_math_value_type_fold` below — see the header for the three callers and for why exactly
 * one of them, §4.3.1's `toSum`, must not refuse at all.
 *
 * NAMED RESIDUAL — §4.3.4's CSSMathClamp IS NOT MINTED, AND THE REASON IS IN THE PUBLISHED DRAFT.
 * WHAT IS NOT COVERED: `CSSMathClamp`, its `lower`/`value`/`upper` slots and the `"clamp"` operator.
 * WHY IT IS NOT A NARROWING OF THIS ENTRY BUT AN ABSENT INTERFACE: §4.3.4 defines the constructor and the
 * operator string for it, and then TWO normative algorithms it must take part in have no arm for it at all.
 * §4.3.1's equal numeric values lists "CSSMathSums, CSSMathProducts, CSSMathMins, or CSSMathMaxs" and then
 * asserts "value1 and value2 are both CSSMathNegates or CSSMathInverts" — a clamp reaches the assert. §6.5
 * CSSMathValue Serialization branches on min/max, sum, negate, product and invert and simply ends, so a clamp
 * falls off the list with no value returned. Minting one would therefore mean either an abort at `toString` on
 * an object a page legitimately built, or a serialization this engine invented and no standard states.
 * WHAT THE NEXT DIFF BUILDS: the clamp arm, once the draft states those two arms — the interface, its three
 * slots, its type (§4.3.4 does state that one: "the result of adding the types of the lower, value, and upper
 * internal slots") and the two algorithm arms together.
 * HOW ITS ABSENCE WOULD SHOW: `new CSSMathClamp(...)` is the TypeError a browser without the interface gives,
 * which is the forcing function; it cannot show as a wrong answer, because nothing here mints one — and that
 * half is STRUCTURAL rather than a promise: `CssMathOp` has no clamp member, so a clamp cannot be represented
 * at all, and every switch over that enum names each member with NO `default:`, so adding one reddens them
 * instead of being swallowed.
 * THE OBSERVATION THAT RETIRES THIS, so the next reader RUNS it rather than re-deriving the argument above:
 * fetch the document engine/specindex/csstypedom1.json's own `base` field names and grep the two sections
 * for `clamp`. While §6.5 CSSMathValue Serialization and §4.3.1's equal numeric values each answer ZERO, the
 * residual stands; the day either answers, the arm it states is what this builds. Re-derived at the base
 * that corpus row records, against the draft it stamps `6 August 2026`: §6.5 0, equal-numeric-values 0,
 * while §4.3.4 names CSSMathClamp five times — so the draft still defines the constructor and neither
 * algorithm it must take part in. */
JSValue css_math_value_new(JSContext *ctx, CssMathOp op, JSValueConst *items, int n);

/* §4.3.4's "The type of a CSSMathValue depends on its class", over an operator and the operand list a mint is
   about to be given — which is ALSO, word for word, the type step of §4.3.4's four list constructors ("Let type
   be the result of adding the types of all the items of args") and of §4.3.1's add, mul, min and max ("Let type
   be the result of adding the types of every item in values"). ONE entry, because those are one sentence stated
   in four places, and because the mint has to compute the same thing to store it.
   THE ANSWER MAY BE core/css/css_math.h's FAILURE, which is what a caller with a step 3 tests for and turns
   into its own TypeError. `items` is BORROWED and `n` obeys the same shape rule as the mint's. */
CssMathType css_math_value_type_fold(JSContext *ctx, CssMathOp op, JSValueConst *items, int n);

/* Web IDL §3.8 Platform objects implementing interfaces' "value implements an interface interface" for the
   whole family: is this object one of §4.3.4's math values in this agent? The TypeError a member owes a
   receiver that fails it is §3.7.6 Attributes'. THE NUMBER READ §3.7.5, WHICH IS Constants. */
bool css_math_value_is(JSValueConst v);

/* WHICH ONE — §4.3.4's `operator` table read as this component's dispatch. `v` must be a math value; the
   caller has already asked css_math_value_is. */
CssMathOp css_math_value_op(JSValueConst v);

/* THE STORED TYPE — §4.3.4's "The type of a CSSMathValue depends on its class", answered from the mint rather
   than from a walk (see the header). `v` must be a math value. */
CssMathType css_math_value_type(JSValueConst v);

/* §4.3.4's `values` / `value` internal slot, for the algorithms §4.3.1 states over it — its `add` step 2
   ("If this is a CSSMathSum object, prepend the items in this's values internal slot to values"), its negate
   and invert ("If this is a CSSMathNegate object, return this's value internal slot"), and equal numeric
   values' third and fifth steps.
   THE ANSWER IS THE ITEM LIST AS A JS Array AND NEVER THE CSSNumericArray WRAPPER, because every one of those
   callers wants the items and none of them wants the platform object; the wrapper is what the ATTRIBUTE
   answers and is minted once at the constructor. OWNED: the caller frees. `v` must be a math value. */
JSValue css_math_value_items(JSContext *ctx, JSValueConst v);

/* CSS Typed OM 1 §6.5 CSSMathValue Serialization over one math value, with `nested` and `paren-less` at the
   defaults §6.5 states ("defaulting to false if unspecified") — which is what §6.3's dispatch passes and the
   only entry any caller outside this file has ever wanted.
   OWNED: a JS string, or the unknown a §6.5 run over unknown leaf `value` slots derives (see the header).
   `v` must be a math value; the caller has already asked css_math_value_is. */
JSValue css_math_value_serialize(JSContext *ctx, JSValueConst v);

/* The per-realm install of §4.3.4's seven interface prototype objects and interface objects, plus
   CSSNumericArray's. `numeric_proto` is THIS realm's CSSNumericValue.prototype, which Web IDL §3.7.3 Interface
   prototype object makes the proto of every one of them — it is passed in rather than looked up for the reason
   core/css/css_unit_value.c states about the chain being one object graph: one place creates it, and a second
   component reaching for it through the global would be reading a name a page could have replaced by the time
   a later realm is built. */
void css_math_value_install_realm(JSContext *ctx, JSValueConst numeric_proto);

void css_math_value_init(JSContext *ctx);
/* THE INVERSE. `rt` is needed because this component holds an ATOM and a Symbol beside its classes — the
   prototypes and interface objects are the REALMS' and go with their contexts. */
void css_math_value_free(JSRuntime *rt);

#endif
