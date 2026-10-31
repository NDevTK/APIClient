/* CSS Values and Units 4 §10 "Mathematical Expressions". See css_math.h for why this is a component, what
 * lexbor supplied and what is ported, why the type algebra is not arithmetic on numbers, and where §10.10.1's
 * simplification stops. */
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/css/syntax/token.h>
#include <lexbor/css/syntax/tokenizer.h>

#include "check.h"
#include "core/agent_state.h"
#include "core/css/css_dimension.h"
#include "core/css/css_length.h"
#include "core/css/css_math.h"
#include "core/realm.h"
#include <lexbor/core/base.h>   /* lexbor_realloc: the allocator the engine gives its refusal edge */

/* §10.7.1 "Numeric Constants: e, pi", to the digits that section itself prints: e is "approximately equal to
   2.7182818284590452354" and pi "approximately equal to 3.1415926535897932". Written out rather than taken
   from `M_E`/`M_PI`, which are POSIX rather than C and whose value is the platform's rather than the spec's. */
#define MTH_E  2.7182818284590452354
#define MTH_PI 3.1415926535897932


/* ---- CSS Typed OM 1 §4.3.2 "Numeric Value Typing" — the type algebra §10.9 links to by name -------------- */

CssMathType css_math_type_number(void)
{
    CssMathType t;

    memset(&t, 0, sizeof t);
    t.hint = CSS_MATH_HINT_NULL;
    return t;
}

CssMathType css_math_type_of(CssMathBase base)
{
    CssMathType t = css_math_type_number();

    DCHECK((unsigned)base < (unsigned)CSS_MATH_BASE_COUNT,
           "a CSS numeric type was built over a base type outside CSS Typed OM 1 §4.3.2's seven — that section "
           "names them exhaustively (\"length\", \"angle\", \"time\", \"frequency\", \"resolution\", \"flex\", "
           "and \"percent\"), so an eighth is a unit family this file classified into a slot the enum does not "
           "have");
    t.exp[base] = 1;
    return t;
}

/* §4.3.2's "Return failure", as a value of the record — see css_math.h for why it must be storable and why it
   is spelled in the hint rather than in a second field. */
CssMathType css_math_type_failure(void)
{
    CssMathType t;

    memset(&t, 0, sizeof t);
    t.hint = CSS_MATH_HINT_FAILURE;
    return t;
}

bool css_math_type_is_failure(const CssMathType *t)
{
    DCHECK(t != NULL, "a CSS numeric type was asked whether it is §4.3.2's failure through a NULL pointer");
    return t->hint == CSS_MATH_HINT_FAILURE;
}

/* §4.3.2's "apply the percent hint hint to a type WITHOUT a percent hint". */
static void mth_apply_hint(CssMathType *t, int hint)
{
    DCHECK(t->hint == CSS_MATH_HINT_NULL,
           "CSS Typed OM 1 §4.3.2's apply-the-percent-hint was run over a type that already carries one. The "
           "algorithm is stated over \"a type without a percent hint\" and every caller in §4.3.2 reaches it "
           "only through the branch that has just tested for a null one, so a second application is a caller "
           "that skipped that test — and it would silently re-fold an already-folded percent exponent");
    DCHECK(hint != CSS_MATH_HINT_NULL && (unsigned)hint < (unsigned)CSS_MATH_BASE_COUNT,
           "a null or out-of-range percent hint was APPLIED. §4.3.2 applies a hint only inside the branch where "
           "the other type's hint is non-null, so a null one here is that branch having been entered on a type "
           "whose hint is absent");
    t->hint = hint;
    if (hint != CSS_MATH_PERCENT) {
        t->exp[hint] += t->exp[CSS_MATH_PERCENT];
        t->exp[CSS_MATH_PERCENT] = 0;
    }
    DCHECK(hint == CSS_MATH_PERCENT || t->exp[CSS_MATH_PERCENT] == 0,
           "a percent hint other than \"percent\" was applied and the type still carries a non-zero \"percent\" "
           "exponent. §4.3.2's step is \"add type[\"percent\"] to type[hint], then SET TYPE[\"PERCENT\"] TO 0\" "
           "— the whole point of the hint is that the percentage has been re-expressed as the hinted base, so a "
           "surviving percent entry would let it be counted twice by the next addition");
}

/* Do the two types' non-zero entries agree — §4.3.2's "If all the entries of type1 with non-zero values are
   contained in type2 with the same value, and vice-versa". A dense map makes that one comparison. */
static bool mth_entries_equal(const CssMathType *a, const CssMathType *b)
{
    unsigned i;

    for (i = 0; i < CSS_MATH_BASE_COUNT; i++)
        if (a->exp[i] != b->exp[i]) return false;
    return true;
}

/* §4.3.2's "To add two types type1 and type2". PUBLIC — see css_math.h for which second standard asks. */
bool css_math_type_add(const CssMathType *type1, const CssMathType *type2, CssMathType *out)
{
    CssMathType t1 = *type1, t2 = *type2;
    unsigned hint;

    /* A FAILURE OPERAND ABSORBS — see css_math.h for why this arm exists at all and why it is not a DCHECK.
       It stands FIRST because everything below reads a hint, and a failure's is neither null nor a base type. */
    if (css_math_type_is_failure(&t1) || css_math_type_is_failure(&t2)) return false;
    if (t1.hint != CSS_MATH_HINT_NULL && t2.hint != CSS_MATH_HINT_NULL && t1.hint != t2.hint) return false;
    if (t1.hint != CSS_MATH_HINT_NULL && t2.hint == CSS_MATH_HINT_NULL) mth_apply_hint(&t2, t1.hint);
    else if (t2.hint != CSS_MATH_HINT_NULL && t1.hint == CSS_MATH_HINT_NULL) mth_apply_hint(&t1, t2.hint);
    if (mth_entries_equal(&t1, &t2)) {
        *out = t1;
        out->hint = t1.hint;
        return true;
    }
    /* §4.3.2's second arm: "If type1 and/or type2 contain "percent" with a non-zero value, and type1 and/or
       type2 contain a key other than "percent" with a non-zero value" — try every base type as the hint, which
       is what makes `calc(1px + 50%)` a `<length>` in a context that never named one. */
    if ((t1.exp[CSS_MATH_PERCENT] != 0 || t2.exp[CSS_MATH_PERCENT] != 0)) {
        bool other = false;
        unsigned i;

        for (i = 0; i < CSS_MATH_BASE_COUNT; i++)
            if (i != CSS_MATH_PERCENT && (t1.exp[i] != 0 || t2.exp[i] != 0)) other = true;
        if (!other) return false;
        for (hint = 0; hint < CSS_MATH_BASE_COUNT; hint++) {
            CssMathType p1 = t1, p2 = t2;

            if (hint == CSS_MATH_PERCENT) continue;
            /* "Provisionally apply the percent hint hint to BOTH type1 and type2" — provisionally, so the copy
               is per iteration and a failed hint reverts by being discarded rather than by being undone. */
            if (p1.hint == CSS_MATH_HINT_NULL) mth_apply_hint(&p1, (int)hint);
            if (p2.hint == CSS_MATH_HINT_NULL) mth_apply_hint(&p2, (int)hint);
            if (!mth_entries_equal(&p1, &p2)) continue;
            *out = p1;
            out->hint = (int)hint;
            return true;
        }
    }
    return false;
}

/* §4.3.2's "To multiply two types type1 and type2". PUBLIC — see css_math.h. */
bool css_math_type_mul(const CssMathType *type1, const CssMathType *type2, CssMathType *out)
{
    CssMathType t1 = *type1, t2 = *type2;
    unsigned i;

    /* A FAILURE OPERAND ABSORBS — the same arm as add's, for the same reason and in the same position. */
    if (css_math_type_is_failure(&t1) || css_math_type_is_failure(&t2)) return false;
    if (t1.hint != CSS_MATH_HINT_NULL && t2.hint != CSS_MATH_HINT_NULL && t1.hint != t2.hint) return false;
    if (t1.hint != CSS_MATH_HINT_NULL && t2.hint == CSS_MATH_HINT_NULL) mth_apply_hint(&t2, t1.hint);
    else if (t2.hint != CSS_MATH_HINT_NULL && t1.hint == CSS_MATH_HINT_NULL) mth_apply_hint(&t1, t2.hint);
    *out = t1;
    for (i = 0; i < CSS_MATH_BASE_COUNT; i++) out->exp[i] = t1.exp[i] + t2.exp[i];
    out->hint = t1.hint;
    return true;
}

/* §4.3.2's "To invert a type type" — every exponent negated, the percent hint kept. PUBLIC — see
   css_math.h. */
CssMathType css_math_type_invert(const CssMathType *t)
{
    CssMathType out = *t;
    unsigned i;

    /* A FAILURE COMES OUT A FAILURE WITH NO ARM HERE, AND THAT IS THE SPEC'S OWN SENTENCE RATHER THAN LUCK:
       §4.3.2 opens this algorithm with "Let result be a new type with an initially empty ordered map and a
       percent hint MATCHING THAT OF TYPE", and css_math.h spells failure IN the percent hint — so the one
       thing invert is stated to carry through unchanged is the one thing failure is written in. The negation
       below then runs over a failure's zeroed exponents and leaves them zero. Add and multiply need a real
       arm because they READ both hints and a failure's is neither null nor a base type; invert reads none. */
    for (i = 0; i < CSS_MATH_BASE_COUNT; i++) out.exp[i] = -t->exp[i];
    return out;
}

/* §10.9's "To make a type base consistent with another type input", which is how every function whose RESULT
   type is fixed (`sin()` is a `<number>`, `atan()` an `<angle>`) still carries its argument's percent hint. */
static bool mth_make_consistent(CssMathType *base, const CssMathType *input)
{
    DCHECK(!css_math_type_is_failure(base) && !css_math_type_is_failure(input),
           "§10.9's make-a-type-consistent was handed §4.3.2's FAILURE. That algorithm reads a percent hint off "
           "both operands and a failure has none. Every caller reaches this through `mth_make_consistent_total`, "
           "which answers a failure operand itself and never calls in with one, so one arriving here is a "
           "caller that bypassed that entry");
    if (base->hint != CSS_MATH_HINT_NULL && input->hint != CSS_MATH_HINT_NULL && base->hint != input->hint)
        return false;
    if (base->hint == CSS_MATH_HINT_NULL) base->hint = input->hint;
    return true;
}

/* §10.9'S PROPAGATION RULE, WHICH IS A STATEMENT ABOUT THE CALCULATION'S TYPE AND NEVER ABOUT THE PARSE:
   "At a + or - sub-expression, attempt to add the types of the left and right arguments. If this returns
   failure, THE ENTIRE CALCULATION'S TYPE IS FAILURE." A failure IS a type this component can hold — css_math.h
   spells it in the percent hint for exactly that reason — so §4.3.2's "return failure" becomes that TYPE
   here, in one place, and never a refusal by the walk. The three entries below are the only callers of the
   algebra inside this file, so the rule cannot be forgotten at a site: `mth_add` and `mth_multiply` return
   VOID, which leaves `<calc-sum>` and `<calc-product>` with no type-derived failure branch to write.
   THE DIFFERENCE IS THE WHOLE OF §10'S OWN SPLIT between §10.8 "Syntax" and §10.9 "Type Checking", and it is
   OBSERVABLE TWICE. `calc(1px + 1s)` matches §10.8's `<calc()> = calc( <calc-sum> )` and IS one math function,
   which §10.9 then makes INVALID ("For each of the above, if the type is failure, the math function is
   invalid") — so a walk that REFUSED it answered instead that those bytes are not a math function at all.
   `css_math_is_lone_function` exists to tell those two apart and nothing else, so the refusal made its only
   distinction unmakeable, and core/css/css_style_declaration.c reaches for that entry to keep a page's own
   typo out of a DFAIL: `text-indent: calc(2em + 1s)` is an author's mistake and was aborting the engine.
   AND IT MADE THE SYNTAX ANSWER DEPEND ON THE CALCULATION CONTEXT, which is the half that surfaced it.
   §10.9's `<percentage>` terminal types a percentage as the base the context resolves it against, so
   `calc(25% + 10deg)` adds angle+angle under an `<angle-percentage>` context and percent+angle under one that
   resolves percentages against nothing — and only the second returned failure. A caller asking "is this a
   math function" had to invent a context to ask in, and the answer it got was about the context it invented.
   With the propagation total the walk reaches EOF either way and `want` decides the TYPE alone, which is the
   contract css_math.h states for both entries. */
static CssMathType mth_types_added(const CssMathType *a, const CssMathType *b)
{
    CssMathType out;

    return css_math_type_add(a, b, &out) ? out : css_math_type_failure();
}

static CssMathType mth_types_multiplied(const CssMathType *a, const CssMathType *b)
{
    CssMathType out;

    return css_math_type_mul(a, b, &out) ? out : css_math_type_failure();
}

/* The same rule over §10.9's make-a-type-consistent, which may not be handed a failure (the DCHECK above), so
   the propagation stands IN FRONT of that algorithm rather than inside it — a fold whose accumulated or
   incoming type is already failure has nothing left to make consistent, and §10.9's own answer for a pair
   that cannot be made consistent is "Return failure" rather than a refusal. `base` is written on every path. */
static void mth_make_consistent_total(CssMathType *base, const CssMathType *input)
{
    if (css_math_type_is_failure(base) || css_math_type_is_failure(input) ||
        !mth_make_consistent(base, input))
        *base = css_math_type_failure();
}

/* RESIDUAL — THE PROPAGATION ABOVE IS §10.9'S ALGEBRA AND NOT EVERY TYPE RULE §10 STATES.
   THE LINE IS §10.8'S OWN AND IS NOT DRAWN HERE: after its grammar css-values-4 §10.8 "Syntax" says "Several of the math functions
   above have ADDITIONAL CONSTRAINTS on what their <calc-sum> arguments can contain. Check the definitions of
   the individual functions for details." So the algebra above is §10.9's and belongs to every calculation,
   and those constraints belong to one function each.
   NOT COVERED: a constraint a FUNCTION'S OWN SECTION puts on its arguments is still a refusal by the walk
   rather than a failure type — css-values-4 §10.4 "Trigonometric Functions: sin(), cos(), tan(), asin(),
   acos(), atan(), and atan2()" says sin/cos/tan "must resolve to either a <number> or an <angle>" and the arc
   functions "contain a single calculation which must resolve to a <number>"; §10.5's same sentence covers
   pow/log/sqrt/exp, and §10.3's
   `line-width` length requirement and its rule for when B may be omitted, and §10.9's "anything else: The
   calculation's type is failure" for a dimension whose unit names no base type. Each of those texts IS one
   math function by §10.8's grammar, so this component still reports some of them as not a math function at
   all — which is the same conflation the algebra had, surviving in the places §10.9 does not own.
   THE NEXT DIFF makes those refusals produce a failure TYPE exactly as the algebra now does, so that the walk
   reaches EOF for every text §10.8's grammar admits and §10.8's rung answers TRUE for all of it. What that
   then buys is the invariant this file cannot assert until it holds: that the SYNTAX rung's answer is
   independent of `want`, which today is true of every text the algebra decides and false of these.
   HOW ITS ABSENCE WOULD SHOW: core/css/css_style_declaration.c's multi-component DFAIL is reached for a value
   whose only defect is a per-function argument type, and its message is about a math function standing as ONE
   COMPONENT OF A MULTI-COMPONENT VALUE — so it fires naming a value that has no second component in it. */

/* Does the type have exactly one non-zero entry, and is it `base` with exponent 1 — §4.3.2's "A type matches
   <length> if its only non-zero entry is «["length" → 1]». Similarly for <angle>, <time>, <frequency>,
   <resolution>, and <flex>." */
static bool mth_only(const CssMathType *t, CssMathBase base)
{
    unsigned i;

    for (i = 0; i < CSS_MATH_BASE_COUNT; i++)
        if (t->exp[i] != (i == (unsigned)base ? 1 : 0)) return false;
    return true;
}

static bool mth_no_entries(const CssMathType *t)
{
    unsigned i;

    for (i = 0; i < CSS_MATH_BASE_COUNT; i++)
        if (t->exp[i] != 0) return false;
    return true;
}

/* §10.9.1's CALCULATION CONTEXT, derived from the production the caller wants — see css_math.h for why they
   are one parameter. The answer is the base type a `<percentage>` in this context resolves against, and it is
   ALWAYS one of §4.3.2's seven rather than an absence: a context that resolves percentages against nothing
   gives them "percent" itself, which is the spec's own "Otherwise" arm and is what then fails to match. */
static int mth_pct_base(CssMathProduction want)
{
    switch (want) {
    /* §10.9's `<percentage>` terminal rule: "If, in the context in which the math function containing this
       calculation is placed, <percentage>s are resolved relative to another type of value (such as in width,
       where <percentage> is resolved against a <length>), and that other type is not <number>, the type is
       determined as the other type, but with a percent hint set to that other type." */
    case CSS_MATH_PROD_LENGTH_PERCENTAGE: return CSS_MATH_LENGTH;
    case CSS_MATH_PROD_ANGLE_PERCENTAGE:  return CSS_MATH_ANGLE;
    /* "Otherwise, the type is «["percent" → 1]», with a percent hint of "percent"." A context that allows a
       bare `<percentage>` and one that allows no percentage at all reach the same terminal type; they differ
       only in whether that type then MATCHES, which is the rule below and not this one. */
    default: return CSS_MATH_PERCENT;
    }
}

/* §4.3.2's "A type is said to match a CSS production in some circumstances", over the productions this engine
   asks about. The percent-hint clauses are the half that cannot be dropped: `<length>` and
   `<length-percentage>` are DIFFERENT syntax components in CSS Properties and Values API 1 §5.1, and §4.3.2
   says so in its own terms — "If the context does not allow <percentage> values to be mixed with <length>/etc
   values (or doesn't allow <percentage> values at all, such as border-width), then for the type to be
   considered matching the percent hint must be null." */
static bool mth_type_matches(const CssMathType *t, CssMathProduction want)
{
    bool null_hint = t->hint == CSS_MATH_HINT_NULL;

    DCHECK(!css_math_type_is_failure(t),
           "§10.9's last rule was asked to MATCH §4.3.2's failure against a production. Its own preceding rule "
           "is \"For each of the above, if the type is failure, the math function is invalid\", so a failure "
           "never reaches the matching question — a caller that skipped that rule would read a failure's zeroed "
           "exponents and its answer would be \"this is not a <length>\", which is right for the wrong reason "
           "and would be wrong the day a production matched the empty map with a non-null hint");
    switch (want) {
    /* §10.9: "Additionally, math functions that resolve to <number> can be used in any place that only accepts
       <integer>", so the two ask one question and differ only in the rounding the value takes. */
    case CSS_MATH_PROD_NUMBER:
    case CSS_MATH_PROD_INTEGER:     return mth_no_entries(t) && null_hint;
    case CSS_MATH_PROD_PERCENTAGE:  return mth_only(t, CSS_MATH_PERCENT) &&
                                           (null_hint || t->hint == CSS_MATH_PERCENT);
    case CSS_MATH_PROD_LENGTH:      return mth_only(t, CSS_MATH_LENGTH) && null_hint;
    case CSS_MATH_PROD_ANGLE:       return mth_only(t, CSS_MATH_ANGLE) && null_hint;
    case CSS_MATH_PROD_TIME:        return mth_only(t, CSS_MATH_TIME) && null_hint;
    case CSS_MATH_PROD_FREQUENCY:   return mth_only(t, CSS_MATH_FREQUENCY) && null_hint;
    case CSS_MATH_PROD_RESOLUTION:  return mth_only(t, CSS_MATH_RESOLUTION) && null_hint;
    case CSS_MATH_PROD_FLEX:        return mth_only(t, CSS_MATH_FLEX) && null_hint;
    /* §4.3.2: "A type matches <length-percentage> if it matches <length> or matches <percentage>." The
       `<length>` half here is the one whose context DOES resolve percentages against a length, so its hint may
       be that length rather than having to be null. */
    case CSS_MATH_PROD_LENGTH_PERCENTAGE:
        return (mth_only(t, CSS_MATH_LENGTH) && (null_hint || t->hint == CSS_MATH_LENGTH)) ||
               (mth_only(t, CSS_MATH_PERCENT) && (null_hint || t->hint == CSS_MATH_PERCENT));
    /* The same rule over §5.6's other mixed production: "A type matches <length-percentage> if it matches
       <length> or matches <percentage>" is stated of one pair and is a rule about the PAIR, so the angle pair
       reads identically with its own base in place of the length. */
    case CSS_MATH_PROD_ANGLE_PERCENTAGE:
        return (mth_only(t, CSS_MATH_ANGLE) && (null_hint || t->hint == CSS_MATH_ANGLE)) ||
               (mth_only(t, CSS_MATH_PERCENT) && (null_hint || t->hint == CSS_MATH_PERCENT));
    }
    DFAIL("a math function's resolved type was matched against a production outside CssMathProduction — the "
          "enum and this switch are one list and have come apart");
    return false;
}

/* §10.9'S OWN DEFINITION OF AN *INVALID* MATH FUNCTION, which is its last rule read with no caller's
   production in hand: "A math function resolves to <number>, <length>, <angle>, <time>, <frequency>,
   <resolution>, <flex>, or <percentage> according to which of those productions its type matches. (These
   categories are mutually exclusive.) If it can't match any of these, THE MATH FUNCTION IS INVALID."
   THE LIST IS §10.9'S OWN AND IS NOT `CssMathProduction`. That enum carries three members this sentence does
   not name: `<integer>`, which §10.9 admits only as a place a `<number>` may be USED ("math functions that
   resolve to <number> can be used in any place that only accepts <integer>"), and the two §5.6 mixed types,
   which CSS Typed OM 1 §4.3.2 defines as the disjunction of two members already here ("A type matches
   <length-percentage> if it matches <length> or matches <percentage>"). Adding either kind would not widen
   the answer and would state a rule this sentence does not have.
   AND THE EIGHT ARE ONLY THE WHOLE ANSWER IN A CONTEXT THAT MIXES NO PERCENTAGES, WHICH IS WHY `ctx` IS A
   PARAMETER THIS FUNCTION READS ONLY TO ASSERT. §10.9's "its type matches" is CSS Typed OM 1 §4.3.2's
   matching, and that is stated CONTEXT-RELATIVE: the reading `mth_type_matches` carries for a bare `<angle>`
   is §4.3.2's LAST bullet, "If the context does not allow <percentage> values to be mixed with <length>/etc
   values (or doesn't allow <percentage> values at all, such as border-width), then for the type to be
   considered matching the percent hint must be null." A context that DOES mix takes the bullet above it
   instead — "it must either have a null percent hint, or the percent hint must match the other type" — so in
   an `<angle-percentage>` position «["angle" → 1]» with a hint of "angle" MATCHES `<angle>` and the math
   function is VALID, while this list answers that it matches none of the eight.
   THAT LIST WAS ONCE ASKED OF EVERY CALLER AND THE ARGUMENT IS REWRITTEN RATHER THAN DELETED, because it is
   the reading a maintainer re-derives: §10.9 ends with a FAILURE rule and then a sentence that reads like a
   validity rule followed by a resolution rule, so a gate that asked validity and then the caller's production
   looked like §10 read in order. It is ONE sentence read twice, and the second reading invented a context —
   every percentage-hinted type was refused, which is every math function mixing a percentage with a dimension
   in the position §5.6 defines for exactly that. `mth_top` now asks it once; the assert below is what stops
   the second reading being written again. */
static bool mth_type_is_valid(const CssMathType *t, CssMathProduction ctx)
{
    static const CssMathProduction OF[] = {
        CSS_MATH_PROD_NUMBER, CSS_MATH_PROD_PERCENTAGE, CSS_MATH_PROD_LENGTH, CSS_MATH_PROD_ANGLE,
        CSS_MATH_PROD_TIME, CSS_MATH_PROD_FREQUENCY, CSS_MATH_PROD_RESOLUTION, CSS_MATH_PROD_FLEX,
    };
    unsigned i;

    DCHECK(mth_pct_base(ctx) == CSS_MATH_PERCENT,
           "§10.9's validity rule was asked in a calculation context that RESOLVES PERCENTAGES AGAINST A "
           "DIMENSION. The eight productions below are matched through CSS Typed OM 1 §4.3.2's "
           "percent-hint-must-be-null bullet, which is that section's rule for a context mixing no "
           "percentages — so in a `<length-percentage>` or `<angle-percentage>` context this list refuses "
           "every type the hint exists to describe, `calc(100% - 20px)` included. A caller that names such a "
           "context HAS a production and asks `css_math_matches` for it; this entry is for the caller that "
           "has none");
    DCHECK(!css_math_type_is_failure(t),
           "§10.9's validity rule was handed §4.3.2's FAILURE. Its own preceding rule — \"For each of the "
           "above, if the type is failure, the math function is invalid\" — is a rule of ITS OWN and is "
           "answered by `mth_top` in front of both gates, so a failure arriving here is a caller that skipped "
           "it and `mth_type_matches` one frame down asserts the same thing");
    for (i = 0; i < sizeof OF / sizeof OF[0]; i++)
        if (mth_type_matches(t, OF[i])) return true;
    return false;
}

/* ---- what a calculation has simplified to (see css_math.h) ----------------------------------------------- */

/* WHY A VALUE HAS NO NUMBER, WHICH IS TWO DIFFERENT ABSENCES AND MUST NOT BE ONE FLAG. Both are carried to the
   top rather than crashed at once, because the calculation may still turn out to be INVALID CSS — `calc(50% *
   50%)` types to «["percent" → 2]» and `width: calc(1fr)` to «["flex" → 1]», and neither matches a production,
   so both are refused as an author's mistake and never reach a crash. What they must not share is the MESSAGE:
   a `1fr` aborting with a message about a Product of percentages is the stale-`DFAIL` failure, sending the next
   reader to build a calculation tree when what is missing is a grid track sizer. */
typedef enum {
    MTH_RESOLVED = 0,
    /* §10.10.1 cannot run this function's operation yet — a Min whose children cannot be compared because
       "percentages might resolve against a negative basis" — so mth_function_apply keeps it as an operator
       node of the calculation tree. Transient: no value leaves that function carrying it. */
    MTH_LIVE_NODE,
    /* css-grid-2 §7.2.4's `fr`, whose value exists only inside css-grid-2 §12's track sizing. */
    MTH_UNRESOLVED_FLEX
} MthUnresolved;

/* The FIRST reason wins, so the innermost absence is the one reported — a `min(1fr, 50% * 50%)` names the track
   sizer rather than the tree, because the `fr` is what the reader meets first when they open the value. */
static MthUnresolved mth_worse(MthUnresolved a, MthUnresolved b)
{
    return a != MTH_RESOLVED ? a : b;
}

/* §10.10's calc-operator nodes and the function node, kept where §10.10.1 stops. */
typedef enum { MTH_NODE_SUM, MTH_NODE_NEGATE, MTH_NODE_PRODUCT, MTH_NODE_INVERT, MTH_NODE_FUNCTION } MthNodeOp;

/* A simplified calculation: a Sum of at most two numeric terms, or (`node`) an operator node over such values.
   `num_term` records that the Sum has a non-percentage term even when its number is zero, which §10.13's
   serialization of a node's operands reads (`min(50%, 200px)` against `min(50% + 0px, 200px)`). */
typedef struct {
    CssMathType          type;
    CssPx                num;      /* the non-percentage term, in §5.4.1's canonical unit for `type` */
    double               pct;      /* the `<percentage>` term, as a number of percent */
    bool                 pct_term; /* §10.10.1: a zero-valued Sum term is not removable, so this is not `pct != 0` */
    bool                 num_term;
    MthUnresolved        blocked;
    struct CssMathTree  *node;     /* non-NULL: the value is this operator node; the four fields above unused */
} MthVal;

static MthVal mth_val(CssMathType type, double n)
{
    MthVal v;

    v.type = type;
    v.num = css_px(n);
    v.pct = 0.0;
    v.pct_term = false;
    v.num_term = true;
    v.blocked = MTH_RESOLVED;
    v.node = NULL;
    return v;
}

/* Record that this value has no number, keeping whichever reason was recorded first (see `mth_worse`). */
static void mth_block(MthVal *v, MthUnresolved why)
{
    v->blocked = mth_worse(v->blocked, why);
}

/* ---- the parse (§10.8's grammar), simplifying as it goes (§10.10.1's eager reduction) -------------------- */

typedef struct {
    lxb_css_syntax_tokenizer_t *tkz;
    const CssMathResolver      *res;      /* NULL is §10.9's TYPE-ONLY question — no leaf is resolved */
    int                         pct_base; /* §10.9.1's calculation context */
    unsigned                    depth;
    /* §10.8's WHITESPACE RULE, CARRIED RATHER THAN RE-DERIVED FROM THE CURSOR — true exactly when whitespace
       immediately precedes the current token. It is a field and not a return value because `<calc-sum>` asks
       the question about a position `<calc-product>` has already walked past: that production must skip
       whitespace to look for its own `*` or `/`, so by the time the sum sees the `+` the whitespace before it
       has been CONSUMED, and a sum that asked the tokenizer would find none and refuse `calc(100vw - 20px)`.
       That is CLAUDE.md's own named defect — an operation whose input is read back off the object it acts on
       is read at the wrong TIME — and the fix is the same one: the input travels with the operation. */
    bool                        ws_before;
    /* Every operator node this parse made, freed when it ends; a tree that survives it is interned first. */
    struct CssMathTree         *arena;
} Mth;

/* An operator node: a Sum or Product over `n` children, a Negate or Invert over one, or a math function over
   its arguments in order (`fn` names it, its rounding strategy and its `none` arms). Each child is a simplified
   Sum of numeric terms or another node. A parse's nodes are owned by its arena (`arena_next`); an interned
   tree's by its realm's table, children included. */
struct CssMathTree {
    MthNodeOp           op;
    CssMathType         type;
    MthVal             *kid;
    unsigned            n;
    char                name[16];
    size_t              nlen;
    int                 shape;        /* MthShape */
    int                 how;          /* MthRounding */
    bool                none[3];
    MthUnresolved       blocked;
    JSContext          *realm;        /* an interned tree's owning realm; NULL in a parse's arena */
    struct CssMathTree *arena_next;
};

struct MthFn;
static MthVal mth_node(Mth *m, MthNodeOp op, CssMathType type, const MthVal *kids, unsigned n,
                       const struct MthFn *fn);

static lxb_css_syntax_token_t *mth_peek(Mth *m) { return lxb_css_syntax_token(m->tkz); }

/* Consume one NON-WHITESPACE token, which clears the flag above because whitespace no longer precedes the
   cursor. That whitespace is never consumed here is the two-sided half of the invariant: `mth_skip_ws` is the
   ONLY place a whitespace token is taken, so a production that grew its own skip — which is exactly how
   `calc(100vw - 20px)` came to be refused — trips this rather than silently erasing the evidence §10.8's rule
   is about. */
static void mth_take(Mth *m)
{
    lxb_css_syntax_token_t *t = mth_peek(m);

    DCHECK(t == NULL || t->type != LXB_CSS_SYNTAX_TOKEN_WHITESPACE,
           "a WHITESPACE token was consumed through the math parser's non-whitespace take. css-values-4 §10.8 "
           "'Syntax' requires whitespace on both sides of `+` and `-`, and the only record that it was there "
           "is the flag `mth_skip_ws` sets — so a second place that eats whitespace destroys that record for "
           "whichever production asks next, which is a REFUSAL of valid CSS and not a crash. Route the skip "
           "through `mth_skip_ws`");
    lxb_css_syntax_token_consume(m->tkz);
    m->ws_before = false;
}

/* Skip whitespace, reporting whether any immediately precedes the cursor NOW — which includes whitespace an
   enclosing production already stepped over, for the reason the `ws_before` field carries. §10.8: "whitespace
   is required on both sides of the + and - operators. (The * and / operators can be used without white space
   around them.)" */
static bool mth_skip_ws(Mth *m)
{
    lxb_css_syntax_token_t *t = mth_peek(m);

    while (t != NULL && t->type == LXB_CSS_SYNTAX_TOKEN_WHITESPACE) {
        m->ws_before = true;
        lxb_css_syntax_token_consume(m->tkz);   /* not `mth_take`, which is the entry that CLEARS the flag */
        t = mth_peek(m);
    }
    return m->ws_before;
}

static bool mth_is_delim(lxb_css_syntax_token_t *t, char c)
{
    return t != NULL && t->type == LXB_CSS_SYNTAX_TOKEN_DELIM &&
           lxb_css_syntax_token_delim_char(t) == (lxb_codepoint_t)(unsigned char)c;
}

/* An ASCII case-insensitive compare of a token's own text against a lower-case literal — CSS Syntax 3 §4 makes
   an ident sequence's identity case-insensitive and §10.7.2 says so again for these keywords in particular
   ("As usual for CSS keywords, these are ASCII case-insensitive. Thus, calc(InFiNiTy) is perfectly valid"). */
static bool mth_name_is(const char *s, size_t len, const char *lower)
{
    size_t i;

    for (i = 0; i < len; i++) {
        char c = s[i];

        if (lower[i] == '\0') return false;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != lower[i]) return false;
    }
    return lower[len] == '\0';
}

/* §10.8's twenty-one functional notations, in that section's own order. */
static const char *const MTH_FUNCTIONS[] = {
    "calc", "min", "max", "clamp", "round", "mod", "rem", "sin", "cos", "tan", "asin", "acos", "atan",
    "atan2", "pow", "sqrt", "hypot", "log", "exp", "abs", "sign",
};

bool css_math_is_function(const char *name, size_t len)
{
    unsigned i;

    DCHECK(name != NULL || len == 0,
           "§10.8's function set was asked about through a NULL span with a non-zero length — a FUNCTION token "
           "names its own text inside the buffer it was tokenized from, so an absent pointer is a caller that "
           "lost the buffer");
    for (i = 0; i < sizeof(MTH_FUNCTIONS) / sizeof(MTH_FUNCTIONS[0]); i++)
        if (mth_name_is(name, len, MTH_FUNCTIONS[i])) return true;
    return false;
}

/* §10.9's TERMINAL VALUE rules for a DIMENSION: which base type the unit names. The unit tables are the
   components that own them — core/css/css_length.h for §6's `<length>` and core/css/css_dimension.h for §7's
   three other families — because a second copy here is the copy that disagrees about `dvmin` the day one of
   them is edited. FALSE is §10.9's "anything else: The calculation's type is failure".
   PUBLIC BECAUSE A SECOND SPECIFICATION ASKS THE SAME QUESTION OF THE SAME TABLES — see css_math.h. */
bool css_math_unit_base(const char *unit, size_t len, CssMathBase *base)
{
    if (css_length_is_length_unit(unit, len))  { *base = CSS_MATH_LENGTH;     return true; }
    if (css_angle_unit(unit, len))             { *base = CSS_MATH_ANGLE;      return true; }
    if (css_time_unit(unit, len))              { *base = CSS_MATH_TIME;       return true; }
    if (css_frequency_unit(unit, len))         { *base = CSS_MATH_FREQUENCY;  return true; }
    if (css_resolution_unit(unit, len))        { *base = CSS_MATH_RESOLUTION; return true; }
    /* css-grid-2 §7.2.4 "Flexible Lengths: the fr unit" — "A flexible length or <flex> is a dimension with the
       fr unit". It is ONE identifier and it lives here rather than in a unit component of its own because
       §10.9's type table is the only thing in this engine that has ever needed to know `<flex>` exists; the
       moment a grid track sizer does, the table moves out and this line goes with it. */
    if (mth_name_is(unit, len, "fr"))          { *base = CSS_MATH_FLEX;       return true; }
    return false;
}

/* §10.10.1's "express the resulting numeric value in the appropriate canonical unit", for the four families
   whose conversion is an exact ratio (§7.1 deg, §7.2 s, §7.3 hz, §7.4 dppx). §6's `<length>` is the caller's
   (see css_math.h) and `<flex>` has no conversion in this engine at all. */
static CssPx mth_canonical(const Mth *m, CssMathBase base, double n, const char *unit, size_t len)
{
    double out = 0.0;

    DCHECK(m->res != NULL,
           "a math function's leaf was converted to its canonical unit in §10.9's TYPE-ONLY mode. `calc(5px + "
           "1em)` is a `<length>` whether or not anything knows what an `em` is here, which is the whole reason "
           "that mode exists, so a conversion reached from it is a type question that started resolving");
    switch (base) {
    case CSS_MATH_LENGTH:     return m->res->length_px(m->res->ctx, n, unit, len);
    case CSS_MATH_ANGLE:      if (css_angle_deg(unit, len, n, &out))     return css_px(out); break;
    case CSS_MATH_TIME:       if (css_time_s(unit, len, n, &out))        return css_px(out); break;
    case CSS_MATH_FREQUENCY:  if (css_frequency_hz(unit, len, n, &out))  return css_px(out); break;
    case CSS_MATH_RESOLUTION: if (css_resolution_dppx(unit, len, n, &out)) return css_px(out); break;
    case CSS_MATH_FLEX:
        DFAIL("a `<flex>` leaf reached the canonical-unit conversion. It has no conversion at all — css-grid-2 "
              "§7.2.4's `fr` is \"a fraction of the leftover space in the grid container\" and gets a number "
              "only from css-grid-2 §12's track sizing — so `<calc-value>`'s own dimension arm marks it "
              "unresolved and never calls this, which is what lets `width: calc(1fr)` be REFUSED as invalid "
              "CSS rather than aborted on. One arriving here is that arm having stopped deciding");
        break;
    case CSS_MATH_PERCENT:
    case CSS_MATH_BASE_COUNT:
        DFAIL("a `<percentage>` reached the DIMENSION canonical-unit conversion. CSS Syntax 3 §4 tokenizes a "
              "percentage as its own token type and §10.9 gives it its own terminal rule, so it never carries "
              "a unit identifier and can never be converted by a unit table");
        break;
    }
    DFAIL("a dimension whose unit this file classified into one of css-values-4 §7's families has no ratio to "
          "that family's canonical unit. The membership test and the conversion are two readings of ONE list "
          "in core/css/css_dimension.h, so a unit in the first and not the second is that list having grown in "
          "one place");
    return css_px(0.0);
}

/* §10.8's supported nesting (CSS_MATH_NESTING_SUPPORTED), applied where either of §10.1's equivalent nestings,
   a parenthesis or a math function, enters a level. False is "treated as if it were invalid": a grammar rule
   the spec states, not a cap on work. */
static bool mth_nest_enter(Mth *m)
{
    if (m->depth >= CSS_MATH_NESTING_SUPPORTED) return false;
    m->depth++;
    return true;
}

/* §10.8's `<calc-value>` — "<number> | <dimension> | <percentage> | <calc-keyword> | ( <calc-sum> )" — at the
   cursor, for its leaves. A `(` or a math function opens a nesting level instead, which mth_function's walk
   owns: §10.1's "Parentheses and nesting additional calc() functions are equivalent" is why one production
   serves both, and §10.10's "If leaf is a math function, replace leaf with the internal representation of that
   math function" is the function's own frame there. */
typedef enum { MTH_LEAF_VALUE, MTH_LEAF_REFUSED, MTH_LEAF_PAREN, MTH_LEAF_FUNCTION } MthLeaf;

static MthLeaf mth_leaf(Mth *m, MthVal *out)
{
    lxb_css_syntax_token_t *t;

    mth_skip_ws(m);
    t = mth_peek(m);
    if (t == NULL) return MTH_LEAF_REFUSED;
    switch (t->type) {
    case LXB_CSS_SYNTAX_TOKEN_NUMBER:
        /* §10.9: "<number> / <integer>: the type is «[ ]» (empty map)". Its own note is why a bare `0` is not a
           length here: "Because <number-token>s are always interpreted as <number>s or <integer>s, "unitless
           zero" <length>s aren't supported in math functions. That is, width: calc(0 + 5px); is invalid". */
        *out = mth_val(css_math_type_number(), lxb_css_syntax_token_number(t)->num);
        mth_take(m);
        return MTH_LEAF_VALUE;
    case LXB_CSS_SYNTAX_TOKEN_PERCENTAGE: {
        /* §10.9's `<percentage>` terminal rule, under §10.9.1's calculation context. The number is NOT resolved
           — §10.11: "Where percentages are not resolved at computed-value time, they are not resolved in math
           functions" — so it survives as the Sum's percentage term and the hint records what it will become. */
        CssMathType ty = css_math_type_number();

        if (m->pct_base == CSS_MATH_PERCENT) {
            ty.exp[CSS_MATH_PERCENT] = 1;
            ty.hint = CSS_MATH_PERCENT;
        } else {
            ty.exp[m->pct_base] = 1;
            ty.hint = m->pct_base;
        }
        *out = mth_val(ty, 0.0);
        out->pct = lxb_css_syntax_token_number(t)->num;
        out->pct_term = true;
        out->num_term = false;
        mth_take(m);
        return MTH_LEAF_VALUE;
    }
    case LXB_CSS_SYNTAX_TOKEN_DIMENSION: {
        const lxb_css_syntax_token_string_t *u = lxb_css_syntax_token_dimension_string(t);
        double n = lxb_css_syntax_token_dimension(t)->num.num;
        CssMathBase base;

        /* The unit span points into the tokenizer's own buffer and is read BEFORE the token is consumed —
           lexbor keeps a token's cooked string only until the next one is requested. */
        if (!css_math_unit_base((const char *)u->data, u->length, &base)) return MTH_LEAF_REFUSED;
        *out = mth_val(css_math_type_of(base), 0.0);
        /* §10.9's TYPE is answered for every family; only the VALUE splits. css-grid-2 §7.2.4's `fr` has no
           number outside css-grid-2 §12's track sizing, so it is marked unresolved and carried — the type
           still says `<flex>`, so `width: calc(1fr)` is refused by §10.9's last rule as invalid CSS and the
           crash below is reached only by a caller that genuinely asked for a `<flex>` value. */
        if (m->res != NULL) {
            if (base == CSS_MATH_FLEX) out->blocked = MTH_UNRESOLVED_FLEX;
            else out->num = mth_canonical(m, base, n, (const char *)u->data, u->length);
        }
        mth_take(m);
        return MTH_LEAF_VALUE;
    }
    case LXB_CSS_SYNTAX_TOKEN_IDENT: {
        /* §10.7's numeric keywords. §10.7.1: "Both of these keywords are <number>s, and resolve at parse time";
           §10.7.2 says the same of the three degenerate ones. §10.8's `<calc-keyword>` is exactly these five. */
        const lxb_css_syntax_token_string_t *s = lxb_css_syntax_token_ident(t);
        const char *k = (const char *)s->data;
        size_t n = s->length;
        double v;

        if (mth_name_is(k, n, "e"))              v = MTH_E;
        else if (mth_name_is(k, n, "pi"))        v = MTH_PI;
        else if (mth_name_is(k, n, "infinity"))  v = INFINITY;
        else if (mth_name_is(k, n, "-infinity")) v = -INFINITY;
        else if (mth_name_is(k, n, "nan"))       v = NAN;
        else return MTH_LEAF_REFUSED;
        *out = mth_val(css_math_type_number(), v);
        mth_take(m);
        return MTH_LEAF_VALUE;
    }
    case LXB_CSS_SYNTAX_TOKEN_L_PARENTHESIS:
        return MTH_LEAF_PAREN;
    case LXB_CSS_SYNTAX_TOKEN_FUNCTION:
        return MTH_LEAF_FUNCTION;
    default:
        return MTH_LEAF_REFUSED;
    }
}

/* ---- §10.10.1's operator reductions ---------------------------------------------------------------------- */

/* §10.10.1's Sum: "For each set of root's children that are numeric values with identical units, remove those
   children and replace them with a single numeric value containing the sum of the removed nodes." Once the
   TYPES have added, both operands are the same type and both terms are addable in that type's canonical unit —
   which is why the type check is the whole of the validity question and the arithmetic below has none. */
static void mth_add(Mth *m, const MthVal *a, const MthVal *b, MthVal *out)
{
    if (a->node != NULL || b->node != NULL) {
        MthVal kids[2] = { *a, *b };

        *out = mth_node(m, MTH_NODE_SUM, mth_types_added(&a->type, &b->type), kids, 2, NULL);
        return;
    }
    out->type = mth_types_added(&a->type, &b->type);
    out->num = css_px_add(a->num, b->num);
    out->pct = a->pct + b->pct;
    out->pct_term = a->pct_term || b->pct_term;
    out->num_term = a->num_term || b->num_term;
    out->blocked = mth_worse(a->blocked, b->blocked);
    out->node = NULL;
}

/* §10.10.1's Negate over an already-simplified Sum: "If root's child is a numeric value, return an equivalent
   numeric value, but with the value negated (0 - value)", applied to each surviving term. */
static MthVal mth_negate(Mth *m, const MthVal *a)
{
    MthVal out = *a;

    if (a->node != NULL && a->node->op == MTH_NODE_SUM) {
        /* §10.10.1's Negate over a Sum: "Return a Sum node with negated grandchildren as its children". */
        const struct CssMathTree *sum = a->node;
        MthVal *neg = lexbor_malloc(sum->n * sizeof *neg);
        unsigned i;

        CHECK(neg != NULL, "css_math: OOM negating a calculation node's children");
        for (i = 0; i < sum->n; i++) neg[i] = mth_negate(m, &sum->kid[i]);
        out = mth_node(m, MTH_NODE_SUM, a->type, neg, sum->n, NULL);
        free(neg);
        return out;
    }
    if (a->node != NULL) return mth_node(m, MTH_NODE_NEGATE, a->type, a, 1, NULL);
    out.num = css_px_sub(css_px(0.0), a->num);
    out.pct = 0.0 - a->pct;
    return out;
}

/* §10.10.1's Product. The two arms that reduce are that section's own: its last Product rule ("If root
   contains only numeric values and/or Invert nodes containing numeric values, and multiplying the types ...")
   and the rule above it ("If root contains only two children, one of which is a number ... and the other of
   which is a Sum whose children are all numeric values, multiply all of the Sum's children by the number").
   Anything else leaves a live Product node, which is `blocked`. */
static void mth_multiply(Mth *m, const MthVal *a, const MthVal *b, MthVal *out)
{
    CssMathType ty = mth_types_multiplied(&a->type, &b->type);
    MthVal kids[2] = { *a, *b };

    if (a->node != NULL || b->node != NULL) {
        *out = mth_node(m, MTH_NODE_PRODUCT, ty, kids, 2, NULL);
        return;
    }
    out->type = ty;
    out->blocked = mth_worse(a->blocked, b->blocked);
    out->node = NULL;
    out->num_term = true;
    if (!a->pct_term && !b->pct_term) {
        out->num = css_px_mul(a->num, b->num);
        out->pct = 0.0;
        out->pct_term = false;
        return;
    }
    /* The percentage-carrying operand keeps its shape only when the other is a pure `<number>` with no surviving
       percentage of its own — which is exactly §10.10.1's number-times-Sum rule. Both operands' environment
       facts reach the result, because a `calc(2 * 50%)` whose 2 came from `100vw / 50px` is a function of the
       viewport as much as any term that still spells a unit. */
    {
        const MthVal *sum = a->pct_term ? a : b;
        const MthVal *k   = a->pct_term ? b : a;

        if (!k->pct_term && mth_no_entries(&k->type)) {
            out->num = css_px_mul(sum->num, k->num);
            out->pct = sum->pct * k->num.px;
            out->pct_term = true;
            out->num_term = sum->num_term;
            return;
        }
    }
    /* Anything else is a live Product node, which the used-value simplification runs once its percentages
       have a basis. */
    *out = mth_node(m, MTH_NODE_PRODUCT, ty, kids, 2, NULL);
}

/* A function's result value: css_length.h's atom for the function `name` (`nlen` bytes, the notation as written)
   over `args`, with `qual` naming what else picks the function (a strategy, an argument's unit) or NULL. It
   carries every operand's facts, the union a sum carries, for the reason css_length.h gives for `css_px_max`. */
static CssPx mth_atom(const char *name, size_t nlen, const char *qual, const CssPx *args, int nargs, double px)
{
    char op[64];
    int w = snprintf(op, sizeof op, "%.*s%s%s", (int)nlen, name, qual ? ":" : "", qual ? qual : "");

    DCHECK(w > 0 && (size_t)w < sizeof op, "a math function's atom name outgrew its buffer");
    return css_px_atom(op, args, nargs, px);
}

/* §10.8's `/`, as §10.9 states it: "let left type be the result of finding the types of its left argument, and
   right type be the result of finding the types of its right argument AND THEN INVERTING IT. The
   sub-expression's type is the result of multiplying the left type and right type." §10.10.1's Invert reduces
   only "if root's child is a number (not a percentage or dimension)", so a divisor carrying a percentage term
   leaves a live Invert node under the Product — which the multiply above then reports as blocked. */
static void mth_divide(Mth *m, const MthVal *a, const MthVal *b, MthVal *out)
{
    MthVal inv;

    /* THE RECIPROCAL IS TAKEN RAW AND NOT THROUGH `css_px_div`, which asserts a non-zero divisor. That assert
       is right for its own callers — Intersection Observer §3.2.10 step 12 states its non-zero branch itself —
       and wrong here, because §10.9.2 DEFINES division by zero: "Dividing a value by zero produces either +∞
       or −∞, according to the standard sign rules", which is IEEE-754's answer and is what `calc(1 / 0)` must
       give rather than a crash. The environment facts still ride the operand, so the union is unaffected. */
    inv = *b;
    inv.type = css_math_type_invert(&b->type);
    if (b->pct_term || b->node != NULL) {
        inv = mth_node(m, MTH_NODE_INVERT, inv.type, b, 1, NULL);
    } else {
        inv.num = mth_atom("inv", 3, NULL, &b->num, 1, 1.0 / b->num.px);
        inv.pct = 0.0;
        inv.pct_term = false;
    }
    mth_multiply(m, a, &inv, out);
}

/* ---- §10.2 to §10.6's functions ------------------------------------------------------------------------- */

/* §10.5.1's note: "NaN is 'infectious' in every function, forcing the function to return NaN if any argument
   calculation is NaN." Asked before every ordering and every libm call, because C's own library does not have
   that property (`Math.hypot(Infinity, NaN)` is the example the note itself gives). */
static bool mth_is_nan(const MthVal *v) { return v->num.px != v->num.px; }

/* §10.3's `<rounding-strategy> = nearest | up | down | to-zero | line-width`, and §10.3's own default: "If
   <rounding-strategy> is omitted, it defaults to nearest." */
typedef enum { MTH_ROUND_NEAREST = 0, MTH_ROUND_UP, MTH_ROUND_DOWN, MTH_ROUND_TO_ZERO, MTH_ROUND_LINE_WIDTH }
MthRounding;

static bool mth_rounding_of(const char *s, size_t len, MthRounding *out)
{
    if (mth_name_is(s, len, "nearest"))    { *out = MTH_ROUND_NEAREST;    return true; }
    if (mth_name_is(s, len, "up"))         { *out = MTH_ROUND_UP;         return true; }
    if (mth_name_is(s, len, "down"))       { *out = MTH_ROUND_DOWN;       return true; }
    if (mth_name_is(s, len, "to-zero"))    { *out = MTH_ROUND_TO_ZERO;    return true; }
    if (mth_name_is(s, len, "line-width")) { *out = MTH_ROUND_LINE_WIDTH; return true; }
    return false;
}

/* §10.3's round(), with §10.3.1 "Argument Ranges"'s degenerate cases first because that section says they come
   first ("If A is infinite but B is finite, the result is the same infinity"). */
static double mth_round_value(MthRounding how, double a, double b)
{
    double lower, upper, c1, c2;

    if (a != a || b != b) return NAN;
    if (b == 0.0) return NAN;                              /* §10.3.1: "if B is 0, the result is NaN" */
    if (isinf(a) && isinf(b)) return NAN;                  /* "If A and B are both infinite, the result is NaN" */
    if (isinf(a)) return a;                                /* "the result is the same infinity" */
    if (isinf(b)) {
        /* §10.3.1's table for a finite A and an infinite B, which depends on the strategy and the sign of A. */
        switch (how) {
        case MTH_ROUND_UP:
            if (a > 0.0) return INFINITY;
            return (a == 0.0 && !signbit(a)) ? 0.0 : -0.0;
        case MTH_ROUND_DOWN:
            if (a < 0.0) return -INFINITY;
            return (a == 0.0 && signbit(a)) ? -0.0 : 0.0;
        default:
            /* nearest and to-zero: "If A is positive or 0⁺, return 0⁺. Otherwise, return 0⁻." `line-width` is
               not in §10.3.1's table; it is `nearest` with a zero excluded, and no non-zero multiple of an
               infinite B exists, so it lands on the same answer. */
            return (a > 0.0 || (a == 0.0 && !signbit(a))) ? 0.0 : -0.0;
        }
    }
    /* §10.3: "If A is exactly equal to an integer multiple of B, round() resolves to A exactly (preserving
       whether A is 0⁻ or 0⁺, if relevant)." */
    if (fmod(a, b) == 0.0) return a;
    /* "there are two integer multiples of B that are potentially 'closest' to A, lower B which is closer to −∞
       and upper B which is closer to +∞" — derived from both roundings of the quotient so a NEGATIVE B cannot
       swap them. */
    c1 = floor(a / b) * b;
    c2 = ceil(a / b) * b;
    lower = c1 < c2 ? c1 : c2;
    upper = c1 < c2 ? c2 : c1;
    /* "If lower B would be zero, it is specifically equal to 0⁺; if upper B would be zero, it is specifically
       equal to 0⁻." Written through `fabs` rather than as an assignment of the literal, because `x = 0.0` after
       `x == 0.0` reads to a compiler as a no-op while the two zeroes differ in exactly the bit this line is
       about — and §10.9.2 makes that bit observable (`calc(1 / (-5 * 0))` is −∞ and `calc(1 / (5 * 0))` is +∞). */
    if (lower == 0.0) lower = fabs(lower);
    if (upper == 0.0) upper = -fabs(upper);
    switch (how) {
    case MTH_ROUND_UP:      return upper;
    case MTH_ROUND_DOWN:    return lower;
    case MTH_ROUND_TO_ZERO: return fabs(lower) <= fabs(upper) ? lower : upper;
    case MTH_ROUND_LINE_WIDTH:
        /* "if one of lower B or upper B is zero, the non-zero one is chosen" — the snap itself is applied by
           the caller, which is the half that needs a device pixel and therefore a realm. */
        if (lower == 0.0) return upper;
        if (upper == 0.0) return lower;
        break;
    case MTH_ROUND_NEAREST: break;
    }
    /* nearest: "Choose whichever of lower B and upper B that has the smallest absolute difference from A. If
       both have an equal difference (A is exactly between the two values), choose upper B." */
    return (a - lower) < (upper - a) ? lower : upper;
}

/* §10.3's mod(): "the value of the function is equal to the value of A shifted by the integer multiple of B
   that brings the value between zero and B", which guarantees the result "will either be zero or share the
   sign of B, not A". */
static double mth_mod_value(double a, double b)
{
    double r;

    if (a != a || b != b) return NAN;
    if (b == 0.0 || isinf(a)) return NAN;             /* §10.3.1's two shared NaN cases */
    /* §10.3.1: "In mod(A, B) only, if B is infinite and A has opposite sign to B (including an oppositely-
       signed zero), the result is NaN." Every other infinite-B case "just returns A immediately". */
    if (isinf(b)) return (signbit(a) != signbit(b)) ? NAN : a;
    r = fmod(a, b);
    if (r != 0.0 && signbit(r) != signbit(b)) r += b;
    return r;
}

/* §10.3's rem(), which "chooses the integer multiple of B that puts the value between zero and -B, avoiding
   changing the sign of the value" — which is C's own `fmod`. */
static double mth_rem_value(double a, double b)
{
    if (a != a || b != b) return NAN;
    if (b == 0.0 || isinf(a)) return NAN;
    if (isinf(b)) return a;
    return fmod(a, b);
}

/* §10.4's sin/cos/tan, "interpreting the result of their argument as radians". An `<angle>` operand has been
   canonicalized to DEGREES (§7.1: "All <angle> units are compatible, and deg is their canonical unit"), so the
   conversion is here and the argument's own type says whether it is needed. */
static double mth_to_radians(const MthVal *a, bool is_angle)
{
    return is_angle ? a->num.px * MTH_PI / 180.0 : a->num.px;
}

/* §10.5's pow(), with §10.5.1's tables. C's `pow` IS IEEE-754 `pow` and agrees with every row of them; what it
   does not have is §10.5.1's note that NaN is infectious, which is where `pow(NaN, 0)`, `pow(1, ∞)` and
   `pow(-1, ∞)` diverge — JS answers 1 for all three and CSS answers NaN. */
static double mth_pow_value(double a, double b)
{
    if (a != a || b != b) return NAN;
    if ((a == 1.0 || a == -1.0) && isinf(b)) return NAN;
    return pow(a, b);
}

/* §10.5's log(A, B?), with §10.5.1's cases. C has no base-B logarithm and a quotient of logarithms answers an
   infinity where §10.5.1 wants NaN, so the guards are the spec's own words. */
static double mth_log_value(double a, double b, bool have_b)
{
    if (a != a || (have_b && b != b)) return NAN;
    if (have_b && (b < 0.0 || b == 1.0)) return NAN;   /* "if B is 1 or negative, the result is NaN" */
    if (a < 0.0) return NAN;                           /* "If A is negative, the result is NaN" */
    if (a == 0.0) return -INFINITY;                    /* "If A is 0⁺ or 0⁻, the result is −∞" */
    if (a == 1.0) return 0.0;                          /* "If A is 1, the result is 0⁺" */
    if (isinf(a)) return INFINITY;                     /* "If A is +∞, the result is +∞" */
    return have_b ? log(a) / log(b) : log(a);
}

/* ---- the twenty-one functions' argument shapes and §10.9's type table ------------------------------------ */

/* The argument list §10.8 gives each function, as mth_function's walk reads it. */
typedef enum {
    MTH_SHAPE_ONE,          /* calc() and the functions of one calculation */
    MTH_SHAPE_TWO,          /* mod(), rem() and atan2() */
    MTH_SHAPE_ONE_OR_TWO,   /* pow() and log(); pow()'s second calculation is required, which its arm checks */
    MTH_SHAPE_LIST,         /* min(), max() and hypot(): `<calc-sum>#` */
    MTH_SHAPE_CLAMP,        /* clamp( [ <calc-sum> | none ], <calc-sum>, [ <calc-sum> | none ] ) */
    MTH_SHAPE_ROUND         /* round( <rounding-strategy>?, <calc-sum>, <calc-sum>? ) */
} MthShape;

/* One math function being read: its name, its shape, and one calculation per argument position read so far
   (`arg`, OWNED). A clamp() `none` holds a placeholder and is marked in `none`. */
typedef struct MthFn {
    char        name[16];
    size_t      nlen;
    MthShape    shape;
    MthVal     *arg;
    unsigned    n, cap;
    bool        none[3];
    MthRounding how;
} MthFn;

/* ---- §10.10's calculation tree, where §10.10.1 stops ------------------------------------------------------ */

static void mth_kids_push(MthVal **v, unsigned *n, unsigned *cap, MthVal x)
{
    if (*n == *cap) {
        unsigned c = *cap ? 2 * *cap : 4;
        MthVal *grown = lexbor_realloc(*v, c * sizeof *grown);

        CHECK(grown != NULL, "css_math: OOM keeping a calculation node's children");
        *v = grown;
        *cap = c;
    }
    (*v)[(*n)++] = x;
}

/* §10.10.1's operator node `op` over `kids`, in the arena: a Sum's and a Product's children that are the same
   operation are replaced by their children ("For each of root's children that are Sum nodes, replace them with
   their children", and the Product rule's twin), a Sum's numeric children are combined into one ("For each set
   of root's children that are numeric values with identical units ... a single numeric value containing the
   sum"), a Negate of a Negate is its grandchild, and a node left with one child is that child. */
static MthVal mth_node(Mth *m, MthNodeOp op, CssMathType type, const MthVal *kids, unsigned n, const MthFn *fn)
{
    struct CssMathTree *t;
    MthVal *v = NULL, out;
    unsigned cnt = 0, cap = 0, i, j;
    int linear = -1;

    /* §10.9's type-only mode reads only the type, which every node's maker has already computed. */
    if (m->res == NULL) {
        out = mth_val(type, 0.0);
        return out;
    }
    if (op == MTH_NODE_NEGATE && kids[0].node != NULL && kids[0].node->op == MTH_NODE_NEGATE)
        return kids[0].node->kid[0];
    for (i = 0; i < n; i++) {
        const struct CssMathTree *k = kids[i].node;

        if ((op == MTH_NODE_SUM || op == MTH_NODE_PRODUCT) && k != NULL && k->op == op) {
            for (j = 0; j < k->n; j++) mth_kids_push(&v, &cnt, &cap, k->kid[j]);
            continue;
        }
        mth_kids_push(&v, &cnt, &cap, kids[i]);
    }
    if (op == MTH_NODE_SUM) {
        unsigned w = 0;

        for (i = 0; i < cnt; i++) {
            if (v[i].node == NULL && linear >= 0) {
                MthVal sum;

                mth_add(m, &v[linear], &v[i], &sum);
                v[linear] = sum;
                continue;
            }
            if (v[i].node == NULL) linear = (int)w;
            v[w++] = v[i];
        }
        cnt = w;
    }
    if (cnt == 1 && (op == MTH_NODE_SUM || op == MTH_NODE_PRODUCT)) {
        out = v[0];
        free(v);
        return out;
    }
    t = lexbor_calloc(1, sizeof *t);
    CHECK(t != NULL, "css_math: OOM making a calculation node");
    t->op = op;
    t->type = type;
    t->kid = v;
    t->n = cnt;
    t->blocked = MTH_RESOLVED;
    for (i = 0; i < cnt; i++) t->blocked = mth_worse(t->blocked, v[i].blocked);
    if (fn != NULL) {
        memcpy(t->name, fn->name, fn->nlen);
        t->nlen = fn->nlen;
        t->shape = (int)fn->shape;
        t->how = (int)fn->how;
        memcpy(t->none, fn->none, sizeof t->none);
    }
    t->arena_next = m->arena;
    m->arena = t;
    out = mth_val(type, 0.0);
    out.blocked = t->blocked;
    out.node = t;
    return out;
}

static void mth_tree_free(struct CssMathTree *t)
{
    unsigned i;

    for (i = 0; i < t->n; i++)
        if (t->kid[i].node != NULL && t->realm != NULL) mth_tree_free(t->kid[i].node);
    free(t->kid);
    free(t);
}

/* `name`'s shape, false for a name that is none of §10.8's twenty-one. */
static bool mth_shape_of(const char *name, size_t nlen, MthShape *out)
{
    static const struct { const char *name; MthShape shape; } SHAPES[] = {
        { "calc", MTH_SHAPE_ONE },        { "min", MTH_SHAPE_LIST },    { "max", MTH_SHAPE_LIST },
        { "clamp", MTH_SHAPE_CLAMP },     { "round", MTH_SHAPE_ROUND }, { "mod", MTH_SHAPE_TWO },
        { "rem", MTH_SHAPE_TWO },         { "sin", MTH_SHAPE_ONE },     { "cos", MTH_SHAPE_ONE },
        { "tan", MTH_SHAPE_ONE },         { "asin", MTH_SHAPE_ONE },    { "acos", MTH_SHAPE_ONE },
        { "atan", MTH_SHAPE_ONE },        { "atan2", MTH_SHAPE_TWO },   { "pow", MTH_SHAPE_ONE_OR_TWO },
        { "sqrt", MTH_SHAPE_ONE },        { "hypot", MTH_SHAPE_LIST },  { "log", MTH_SHAPE_ONE_OR_TWO },
        { "exp", MTH_SHAPE_ONE },         { "abs", MTH_SHAPE_ONE },     { "sign", MTH_SHAPE_ONE },
    };
    unsigned i;

    for (i = 0; i < sizeof SHAPES / sizeof SHAPES[0]; i++)
        if (mth_name_is(name, nlen, SHAPES[i].name)) {
            *out = SHAPES[i].shape;
            return true;
        }
    DCHECK(!css_math_is_function(name, nlen),
           "one of css-values-4 §10.8's twenty-one functional notations has no argument shape. "
           "`css_math_is_function`'s table and this one are one list read twice, so a name in the first and not "
           "the second is a function that types as valid and evaluates as nothing");
    return false;
}

/* Records `v` as argument position `f->n`. */
static void mth_fn_push(MthFn *f, MthVal v)
{
    if (f->n == f->cap) {
        MthVal *grown;

        f->cap = f->cap ? 2 * f->cap : 4;
        grown = lexbor_realloc(f->arg, f->cap * sizeof *grown);
        CHECK(grown != NULL, "css_math: OOM keeping a math function's operands");
        f->arg = grown;
    }
    f->arg[f->n++] = v;
}

/* After the argument just recorded, whose list ended with `)` when `last`: whether another follows (`*more`).
   False refuses the list as §10.8's grammar for `f`'s shape does. */
static bool mth_fn_next(const MthFn *f, bool last, bool *more)
{
    switch (f->shape) {
    case MTH_SHAPE_ONE:
        *more = false;
        return last;
    case MTH_SHAPE_TWO:
        *more = f->n < 2;
        return last == (f->n == 2);
    case MTH_SHAPE_ONE_OR_TWO:
    case MTH_SHAPE_ROUND:
        *more = f->n < 2 && !last;
        return f->n < 2 || last;
    case MTH_SHAPE_LIST:
        *more = !last;
        return true;
    case MTH_SHAPE_CLAMP:
        *more = f->n < 3;
        return last == (f->n == 3);
    }
    DFAIL("a math function's argument shape is none of the six mth_shape_of assigns");
    return false;
}

/* mth_variadic's value over its `n` operands. */
static CssPx mth_variadic_result(int kind, const CssPx *args, int n, bool saw_nan, bool saw_inf, double fold,
                                 CssPx best)
{
    /* §10.5.1: "In hypot(A, …), if any of the inputs are infinite, the result is +∞" — after the NaN rule, which
       §10.5.1's note makes strictly stronger ("Math.hypot(Infinity, NaN) will return Infinity" is named there as
       the JS behavior CSS does not share). */
    if (kind == 2) return mth_atom("hypot", 5, NULL, args, n, saw_nan ? NAN : (saw_inf ? INFINITY : sqrt(fold)));
    if (saw_nan) return mth_atom(kind == 0 ? "min" : "max", 3, NULL, args, n, NAN);
    return best;
}

/* §10.2's min()/max() and §10.5's hypot(), which share one shape: "one or more comma-separated calculations",
   a type that is "the result of adding the types of its comma-separated calculations", and a fold over the
   values. `kind` is 0 for min, 1 for max, 2 for hypot. */
static void mth_variadic(Mth *m, int kind, const MthVal *argv, unsigned argc, MthVal *out)
{
    MthVal acc = mth_val(css_math_type_number(), 0.0);
    unsigned n = 0;
    bool saw_nan = false, saw_inf = false, any_pct = false;
    double fold = 0.0;
    CssPx best = css_px(0.0), *args = NULL;

    while (n < argc) {
        const MthVal a = argv[n];

        if (n == 0) {
            acc = a;
        } else {
            /* §10.9: the type is "the result of adding the types of its comma-separated calculations". Only the
               TYPES add here — the VALUE is a fold and not a sum, which is why this is not `mth_add`. */
            acc.type = mth_types_added(&acc.type, &a.type);
        }
        if (a.pct_term) any_pct = true;
        acc.blocked = mth_worse(acc.blocked, a.blocked);
        if (m->res != NULL) {
            args = lexbor_realloc(args, (size_t)(n + 1) * sizeof *args);
            CHECK(args, "css_math: OOM keeping a math function's operands");
            args[n] = a.num;
            if (mth_is_nan(&a)) saw_nan = true;
            else if (isinf(a.num.px)) saw_inf = true;
            if (kind == 2) fold += a.num.px * a.num.px;
            /* §10.5.1's note makes NaN INFECTIOUS in every function, so once one has been seen the result is
               decided and NO ORDERING IS ASKED. That is not an optimisation: the ordering asserts it is never
               handed a NaN, because a comparison that quietly answered false would be JS's `Math.min` behavior,
               and `min(1px, calc(NaN * 1px))` is a page's own value rather than a broken invariant. */
            else if (saw_nan) { /* the result below is superseded by §10.5.1's NaN */ }
            /* The length's own extreme, which keeps the winner's form where the facts' ranges decide it. */
            else if (n == 0) best = a.num;
            else best = kind == 0 ? css_px_min(best, a.num) : css_px_max(best, a.num);
        }
        n++;
    }
    DCHECK(n > 0, "a variadic math function was folded with no arguments read — §10.8 gives min(), max() and "
                  "hypot() a `<calc-sum>#` argument, whose `#` multiplier has no zero-length arm, and the walk "
                  "cannot close one without having read one");
    *out = acc;
    if (m->res == NULL) return;
    /* §10.10.1: a percentage "will usually block simplification of the node, since it needs to be resolved
       against another value using information not currently available ... This includes operations such as
       "min", since percentages might resolve against a negative basis, and thus end up with an opposite
       comparative relationship than the raw percentage value would seem to indicate." min()/max() with ONE
       argument are the exception the same section states — "If root has only one child, return the child" — so
       `min(50%)` is `50%` and needs no comparison at all. hypot() has no such arm: with one argument it "gives
       the ABSOLUTE VALUE of its input" (§10.5), which is a sign question a percentage cannot answer either. */
    if (any_pct) {
        free(args);
        if (n > 1 || kind == 2) mth_block(out, MTH_LIVE_NODE);
        return;
    }
    out->pct = 0.0;
    out->pct_term = false;
    out->num = mth_variadic_result(kind, args, (int)n, saw_nan, saw_inf, fold, best);
    free(args);
}

/* §10.2's clamp(MIN, VAL, MAX), whose min/max arms may each be the keyword `none`: §10.8 gives it
   `clamp( [ <calc-sum> | none ], <calc-sum>, [ <calc-sum> | none ] )`. */
static void mth_clamp(Mth *m, const MthVal *arg, const bool *none, MthVal *out)
{
    bool present[3];
    unsigned i;
    CssMathType ty = css_math_type_number();
    bool have_ty = false;

    for (i = 0; i < 3; i++) present[i] = !none[i];
    /* §10.9: clamp()'s type is "the result of adding the types of its comma-separated calculations". `none` is
       a keyword and not a calculation, so it contributes none. */
    for (i = 0; i < 3; i++) {
        if (!present[i]) continue;
        if (!have_ty) { ty = arg[i].type; have_ty = true; continue; }
        ty = mth_types_added(&ty, &arg[i].type);
    }
    DCHECK(have_ty, "clamp()'s central calculation is absent — §10.8's grammar makes only the FIRST and THIRD "
                    "arms admit `none`, and mth_fn_before_arg refuses the keyword in the second position, so a "
                    "clamp with no calculation at all could not have parsed");
    *out = arg[1];
    out->type = ty;
    out->blocked = mth_worse(arg[1].blocked, mth_worse(present[0] ? arg[0].blocked : MTH_RESOLVED,
                                                       present[2] ? arg[2].blocked : MTH_RESOLVED));
    if (m->res == NULL) return;
    /* §10.2: "clamp(MIN, VAL, MAX) ... represents exactly the same value as max(MIN, min(VAL, MAX))", so the
       minimum wins over the maximum when the two are in the wrong order — which §10.2 states as a rule and not
       as a consequence ("clamp(100px, ..., 50px) will resolve to 100px, exceeding its stated 'max' value"). */
    if (arg[1].pct_term || (present[0] && arg[0].pct_term) || (present[2] && arg[2].pct_term)) {
        if (present[0] || present[2]) mth_block(out, MTH_LIVE_NODE);
        return;
    }
    {
        bool nan = mth_is_nan(&arg[1]);
        CssPx ops[3] = { arg[1].num }, v = arg[1].num;
        int nops = 1;

        /* §10.2's max(MIN, min(VAL, MAX)), over the lengths' own extremes and after §10.5.1's NaN. */
        if (present[2]) {
            ops[nops++] = arg[2].num;
            nan = nan || mth_is_nan(&arg[2]);
            if (!nan) v = css_px_min(v, arg[2].num);
        }
        if (present[0]) {
            ops[nops++] = arg[0].num;
            nan = nan || mth_is_nan(&arg[0]);
            if (!nan) v = css_px_max(arg[0].num, v);
        }
        out->num = nan ? mth_atom("clamp", 5, NULL, ops, nops, NAN) : v;
        out->pct = 0.0;
        out->pct_term = false;
    }
}

/* §10.3's round(<rounding-strategy>?, A, B?). */
static bool mth_round(Mth *m, MthRounding how, const MthVal *argv, unsigned argc, MthVal *out)
{
    MthVal a = argv[0];
    MthVal b = argc == 2 ? argv[1] : mth_val(css_math_type_number(), 0.0);
    bool have_b = argc == 2;
    CssMathType ty;

    /* §10.3: "If <rounding-strategy> is line-width, the type of A must match <length>." */
    if (how == MTH_ROUND_LINE_WIDTH && !mth_only(&a.type, CSS_MATH_LENGTH)) return false;
    /* §10.3: "If the type of A matches <number>, then B may be omitted, and defaults to 1. If
       <rounding-strategy> is line-width, B may also be omitted ... In all other cases, omitting B is invalid." */
    if (!have_b && how != MTH_ROUND_LINE_WIDTH && !mth_no_entries(&a.type)) return false;
    ty = a.type;
    if (have_b) ty = mth_types_added(&a.type, &b.type);
    *out = a;
    out->type = ty;
    out->blocked = mth_worse(a.blocked, have_b ? b.blocked : MTH_RESOLVED);
    if (m->res == NULL) return true;
    if (a.pct_term || (have_b && b.pct_term)) {
        mth_block(out, MTH_LIVE_NODE);
        return true;
    }
    /* §10.3's `line-width` strategy ends in css-values-4 §6's SNAP A LENGTH AS A LINE WIDTH — with B omitted
       that is the whole of it ("If B is omitted, A is snapped as a line width"), and with B present it is
       applied to the rounded result ("the final result is snapped as a line width"). The snap is defined over
       the DEVICE PIXEL, which core/frame/viewport.h makes a per-realm environment fact, so it is the one
       production here that needs a realm at all. */
    if (how == MTH_ROUND_LINE_WIDTH && m->res->realm == NULL)
        DFAIL("`round(line-width, ...)` was evaluated through a resolver carrying no realm. css-values-4 §10.3 "
              "'Stepped Value Functions: round(), mod(), and rem()' ends that strategy in css-values-4 §6's "
              "snap a length as a line width, which is defined over the DEVICE PIXEL — and "
              "core/frame/viewport.h makes `devicePixelRatio` a PICKED per-realm environment fact, so there is "
              "no device pixel without a realm and a snapped width that lost it would read as a number the "
              "author's own declaration determined. Give the CssMathResolver the realm whose document this "
              "declaration belongs to, which every caller inside a cascade already holds");
    if (how == MTH_ROUND_LINE_WIDTH && !have_b) {
        out->num = css_length_snap_line_width(m->res->realm, a.num);
    } else {
        CssPx ab[2] = { a.num, b.num };
        char qual[16];

        snprintf(qual, sizeof qual, "%d", (int)how);
        out->num = mth_atom("round", 5, qual, ab, have_b ? 2 : 1,
                            mth_round_value(how, a.num.px, have_b ? b.num.px : 1.0));
        if (how == MTH_ROUND_LINE_WIDTH) out->num = css_length_snap_line_width(m->res->realm, out->num);
    }
    out->pct = 0.0;
    out->pct_term = false;
    return true;
}

/* §10.10: "The internal representation is an operator node with the same name as the function, whose children
   are the result of parsing a calculation from each of the function's arguments." Dispatch on the name, with
   §10.9's own type table as the return type of each. */
static bool mth_function_value(Mth *m, const MthFn *fn, MthVal *out)
{
    const char *name = fn->name;
    size_t nlen = fn->nlen;
    const MthVal *arg = fn->arg;
    bool ok = false;

    if (mth_name_is(name, nlen, "calc")) {
        /* §10.9: calc()'s type is "the type of its contained calculation". */
        *out = arg[0];
        ok = true;
    } else if (mth_name_is(name, nlen, "min")) {
        mth_variadic(m, 0, arg, fn->n, out);
        ok = true;
    } else if (mth_name_is(name, nlen, "max")) {
        mth_variadic(m, 1, arg, fn->n, out);
        ok = true;
    } else if (mth_name_is(name, nlen, "hypot")) {
        mth_variadic(m, 2, arg, fn->n, out);
        ok = true;
    } else if (mth_name_is(name, nlen, "clamp")) {
        mth_clamp(m, arg, fn->none, out);
        ok = true;
    } else if (mth_name_is(name, nlen, "round")) {
        ok = mth_round(m, fn->how, arg, fn->n, out);
    } else if (mth_name_is(name, nlen, "mod") || mth_name_is(name, nlen, "rem")) {
        /* §10.3: "The argument calculations can resolve to any <number>, <dimension>, or <percentage>, but must
           have the same type ... the result will have the same type as the arguments." */
        CssMathType ty;

        ty = mth_types_added(&arg[0].type, &arg[1].type);
        *out = arg[0];
        out->type = ty;
        out->blocked = mth_worse(arg[0].blocked, arg[1].blocked);
        if (m->res != NULL) {
            if (arg[0].pct_term || arg[1].pct_term) { mth_block(out, MTH_LIVE_NODE); ok = true; goto done; }
            CssPx ab[2] = { arg[0].num, arg[1].num };

            out->num = mth_atom(name, nlen, NULL, ab, 2, mth_name_is(name, nlen, "mod")
                                                          ? mth_mod_value(arg[0].num.px, arg[1].num.px)
                                                          : mth_rem_value(arg[0].num.px, arg[1].num.px));
            out->pct = 0.0;
            out->pct_term = false;
        }
        ok = true;
    } else if (mth_name_is(name, nlen, "sin") || mth_name_is(name, nlen, "cos") ||
               mth_name_is(name, nlen, "tan")) {
        /* §10.4: sin/cos/tan "must resolve to either a <number> or an <angle> ... They all represent a
           <number>, with the return type made consistent with the input calculation's type." */
        bool is_angle;
        CssMathType ty = css_math_type_number();

        is_angle = mth_only(&arg[0].type, CSS_MATH_ANGLE);
        if (!is_angle && !mth_no_entries(&arg[0].type)) goto done;
        mth_make_consistent_total(&ty, &arg[0].type);
        *out = arg[0];
        out->type = ty;
        if (m->res != NULL) {
            double r;

            if (arg[0].pct_term) { mth_block(out, MTH_LIVE_NODE); ok = true; goto done; }
            r = mth_to_radians(&arg[0], is_angle);
            /* §10.4.1: "In sin(A), cos(A), or tan(A), if A is infinite, the result is NaN." C's `sin` already
               answers NaN there, and its NaN propagation is the same; the guard is written so the rule is
               visible where the reader looks for it rather than left to a library's conformance. */
            if (r != r || isinf(r)) r = NAN;
            else if (mth_name_is(name, nlen, "sin")) r = sin(r);
            else if (mth_name_is(name, nlen, "cos")) r = cos(r);
            else r = tan(r);
            out->num = mth_atom(name, nlen, is_angle ? "angle" : "number", &arg[0].num, 1, r);
            out->pct = 0.0;
            out->pct_term = false;
        }
        ok = true;
    } else if (mth_name_is(name, nlen, "asin") || mth_name_is(name, nlen, "acos") ||
               mth_name_is(name, nlen, "atan")) {
        /* §10.4: the arc functions "contain a single calculation which must resolve to a <number>, and compute
           their corresponding function, interpreting their result as a number of radians, representing an
           <angle>". §10.9 gives all three the type «["angle" → 1]». */
        CssMathType ty = css_math_type_of(CSS_MATH_ANGLE);

        if (!mth_no_entries(&arg[0].type)) goto done;
        mth_make_consistent_total(&ty, &arg[0].type);
        *out = arg[0];
        out->type = ty;
        if (m->res != NULL) {
            double v = arg[0].num.px, r;

            if (arg[0].pct_term) { mth_block(out, MTH_LIVE_NODE); ok = true; goto done; }
            if (v != v) r = NAN;
            else if (mth_name_is(name, nlen, "asin")) r = asin(v);   /* §10.4.1: |A| > 1 is NaN, which C gives */
            else if (mth_name_is(name, nlen, "acos")) r = acos(v);
            else r = atan(v);                                        /* §10.4.1: ±∞ is ±90deg, which C gives */
            /* §7.1's canonical unit for an `<angle>` is `deg` */
            out->num = mth_atom(name, nlen, NULL, &arg[0].num, 1, r * 180.0 / MTH_PI);
            out->pct = 0.0;
            out->pct_term = false;
        }
        ok = true;
    } else if (mth_name_is(name, nlen, "atan2")) {
        /* §10.4: "A and B can resolve to any <number>, <dimension>, or <percentage>, but must have a consistent
           type" and the result is an `<angle>` "with the return type made consistent with the input
           calculation's type". §10.4.1's whole table of unusual argument combinations is C's own `atan2`. */
        CssMathType sum, ty = css_math_type_of(CSS_MATH_ANGLE);

        sum = mth_types_added(&arg[0].type, &arg[1].type);
        mth_make_consistent_total(&ty, &sum);
        *out = arg[0];
        out->type = ty;
        out->blocked = mth_worse(arg[0].blocked, arg[1].blocked);
        if (m->res != NULL) {
            if (arg[0].pct_term || arg[1].pct_term) { mth_block(out, MTH_LIVE_NODE); ok = true; goto done; }
            CssPx ab[2] = { arg[0].num, arg[1].num };

            out->num = mth_atom(name, nlen, NULL, ab, 2, (mth_is_nan(&arg[0]) || mth_is_nan(&arg[1]))
                                         ? NAN : atan2(arg[0].num.px, arg[1].num.px) * 180.0 / MTH_PI);
            out->pct = 0.0;
            out->pct_term = false;
        }
        ok = true;
    } else if (mth_name_is(name, nlen, "pow") || mth_name_is(name, nlen, "log")) {
        /* §10.5: pow()'s two calculations and log()'s one-or-two "must resolve to <number>s", and the result is
           a `<number>` "with the return type made consistent with the input calculation's type". */
        CssMathType ty = css_math_type_number();
        bool is_pow = mth_name_is(name, nlen, "pow");
        bool have_b = fn->n == 2;

        if (is_pow && !have_b) goto done;              /* §10.8: `<pow()> = pow( <calc-sum>, <calc-sum> )` */
        if (!mth_no_entries(&arg[0].type)) goto done;
        if (have_b && !mth_no_entries(&arg[1].type)) goto done;
        mth_make_consistent_total(&ty, &arg[0].type);
        if (have_b) mth_make_consistent_total(&ty, &arg[1].type);
        *out = arg[0];
        out->type = ty;
        out->blocked = mth_worse(arg[0].blocked, have_b ? arg[1].blocked : MTH_RESOLVED);
        if (m->res != NULL) {
            if (arg[0].pct_term || (have_b && arg[1].pct_term)) {
                mth_block(out, MTH_LIVE_NODE);
                ok = true;
                goto done;
            }
            CssPx ab[2] = { arg[0].num, arg[1].num };

            out->num = mth_atom(name, nlen, NULL, ab, have_b ? 2 : 1,
                                is_pow ? mth_pow_value(arg[0].num.px, arg[1].num.px)
                                       : mth_log_value(arg[0].num.px, have_b ? arg[1].num.px : 0.0, have_b));
            out->pct = 0.0;
            out->pct_term = false;
        }
        ok = true;
    } else if (mth_name_is(name, nlen, "sqrt") || mth_name_is(name, nlen, "exp")) {
        /* §10.5: both "contain a single calculation which must resolve to a <number>". §10.5.1's cases for both
           — `sqrt(+∞)` is `+∞`, `sqrt(0⁻)` is `0⁻`, `sqrt(A < 0)` is NaN, `exp(+∞)` is `+∞`, `exp(−∞)` is `0⁺`
           — are IEEE-754's own and are C's `sqrt` and `exp` unchanged. */
        CssMathType ty = css_math_type_number();

        if (!mth_no_entries(&arg[0].type)) goto done;
        mth_make_consistent_total(&ty, &arg[0].type);
        *out = arg[0];
        out->type = ty;
        if (m->res != NULL) {
            if (arg[0].pct_term) { mth_block(out, MTH_LIVE_NODE); ok = true; goto done; }
            out->num = mth_atom(name, nlen, NULL, &arg[0].num, 1,
                                mth_name_is(name, nlen, "sqrt") ? sqrt(arg[0].num.px) : exp(arg[0].num.px));
            out->pct = 0.0;
            out->pct_term = false;
        }
        ok = true;
    } else if (mth_name_is(name, nlen, "abs")) {
        /* §10.6: abs(A) "returns the absolute value of A, as the same type as the input", which §10.9 states
           again as "The type of its contained calculation". */
        *out = arg[0];
        if (m->res != NULL) {
            /* §10.6's own note: "an expression like 10% might be positive or negative once it's resolved,
               depending on what value it's resolved against", so a surviving percentage blocks the sign. */
            if (arg[0].pct_term) mth_block(out, MTH_LIVE_NODE);
            else out->num = mth_atom(name, nlen, NULL, &arg[0].num, 1, fabs(arg[0].num.px));
        }
        ok = true;
    } else if (mth_name_is(name, nlen, "sign")) {
        /* §10.6: sign(A) "returns -1 if A's numeric value is negative, +1 if ... positive, 0⁺ if ... 0⁺, and
           0⁻ if ... 0⁻. The return type is a <number>, made consistent with the input calculation's type." */
        CssMathType ty = css_math_type_number();

        mth_make_consistent_total(&ty, &arg[0].type);
        *out = arg[0];
        out->type = ty;
        if (m->res != NULL) {
            double v = arg[0].num.px;

            if (arg[0].pct_term) { mth_block(out, MTH_LIVE_NODE); ok = true; goto done; }
            out->num = mth_atom(name, nlen, NULL, &arg[0].num, 1,
                                v != v ? NAN : v < 0.0 ? -1.0 : v > 0.0 ? 1.0 : signbit(v) ? -0.0 : 0.0);
            out->pct = 0.0;
            out->pct_term = false;
        }
        ok = true;
    } else {
        DFAIL("a math function whose argument shape mth_shape_of answered reached the dispatch above and no "
              "arm claimed it; the two read one list of §10.8's twenty-one names");
    }

done:
    return ok;
}

/* A math function over its arguments, or its operator node where §10.10.1 cannot run it yet: an argument that is
   itself a node ("If root is an operator node that's not one of the calc-operator nodes, and all of its
   calculation children are numeric values with enough information to compute the operation root represents,
   return the result"; a node child is not one), or an operation a percentage argument blocks. The node's type
   is §10.9's, from the same dispatch run over the types alone. calc() is its argument and never a node. */
static bool mth_function_apply(Mth *m, const MthFn *fn, MthVal *out)
{
    bool live = false, ok;
    unsigned i;

    for (i = 0; i < fn->n; i++)
        if (fn->arg[i].node != NULL) live = true;
    if (m->res != NULL && live && !mth_name_is(fn->name, fn->nlen, "calc")) {
        Mth typed = *m;

        typed.res = NULL;
        ok = mth_function_value(&typed, fn, out);
        if (ok) *out = mth_node(m, MTH_NODE_FUNCTION, out->type, fn->arg, fn->n, fn);
        return ok;
    }
    ok = mth_function_value(m, fn, out);
    if (!ok || m->res == NULL) return ok;
    if (out->blocked == MTH_LIVE_NODE) {
        CssMathType ty = out->type;

        *out = mth_node(m, MTH_NODE_FUNCTION, ty, fn->arg, fn->n, fn);
    } else if (out->node == NULL && !out->pct_term) {
        out->num_term = true;
    }
    return ok;
}

/* ---- the walk over §10.8's nesting ---------------------------------------------------------------------- */

/* One nesting level the walk is in: a math function being read (`is_fn`), or a `<calc-sum>` — the inside of a
   `( … )` (`paren`) or an argument of the function frame below it — with the sum so far and the
   `<calc-product>` being read into it. */
typedef struct {
    bool   is_fn;
    bool   paren, have_sum, minus, have_prod, div;
    MthVal sum, prod;
    MthFn  fn;
} MthFrame;

typedef struct {
    MthFrame *v;
    size_t    n, cap;
} MthWalk;

static MthFrame *mth_walk_push(MthWalk *w)
{
    MthFrame *f;

    if (w->n == w->cap) {
        MthFrame *grown;

        w->cap = w->cap ? 2 * w->cap : 8;
        grown = lexbor_realloc(w->v, w->cap * sizeof *grown);
        CHECK(grown != NULL, "css_math: OOM growing a math function's nesting");
        w->v = grown;
    }
    f = &w->v[w->n++];
    memset(f, 0, sizeof *f);
    return f;
}

static void mth_walk_free(MthWalk *w)
{
    size_t i;

    for (i = 0; i < w->n; i++)
        if (w->v[i].is_fn) free(w->v[i].fn.arg);
    free(w->v);
}

/* Opens the math function whose FUNCTION token is at the cursor into `f`. False refuses it: a name that is
   none of §10.8's, or a nesting level past §10.8's supported depth. */
static bool mth_fn_open(Mth *m, MthFn *f)
{
    lxb_css_syntax_token_t *t = mth_peek(m);
    const lxb_css_syntax_token_string_t *s;

    DCHECK(t != NULL && t->type == LXB_CSS_SYNTAX_TOKEN_FUNCTION,
           "a math function was opened with the cursor off a FUNCTION token — §10.8's grammar reaches a function "
           "only through `<calc-value>`'s leaf, which has just answered one");
    s = lxb_css_syntax_token_function(t);
    if (s->length >= sizeof f->name) return false;   /* longer than any of §10.8's twenty-one names */
    /* The name is copied before the token is consumed: lexbor keeps a token's cooked string only until the
       next token is requested. */
    f->nlen = s->length;
    memcpy(f->name, s->data, f->nlen);
    if (!mth_nest_enter(m)) return false;
    mth_take(m);
    f->how = MTH_ROUND_NEAREST;
    return mth_shape_of(f->name, f->nlen, &f->shape);
}

typedef enum { MTH_ARG_READ, MTH_ARG_FILLED, MTH_ARG_DONE, MTH_ARG_REFUSED } MthArgStep;

/* What precedes argument position `f->n`. clamp()'s `none` is the whole argument and its separator (FILLED,
   or DONE when it was the last); round()'s `<rounding-strategy>` precedes its first. READ: a `<calc-sum>` is
   next. */
static MthArgStep mth_fn_before_arg(Mth *m, MthFn *f)
{
    lxb_css_syntax_token_t *t;
    unsigned i = f->n;

    if (f->shape != MTH_SHAPE_CLAMP && !(f->shape == MTH_SHAPE_ROUND && i == 0)) return MTH_ARG_READ;
    mth_skip_ws(m);
    t = mth_peek(m);
    if (t == NULL) return MTH_ARG_REFUSED;
    if (f->shape == MTH_SHAPE_ROUND) {
        /* §10.3's default: "If <rounding-strategy> is omitted, it defaults to nearest." */
        if (t->type == LXB_CSS_SYNTAX_TOKEN_IDENT &&
            mth_rounding_of((const char *)lxb_css_syntax_token_ident(t)->data,
                            lxb_css_syntax_token_ident(t)->length, &f->how)) {
            mth_take(m);
            mth_skip_ws(m);
            t = mth_peek(m);
            if (t == NULL || t->type != LXB_CSS_SYNTAX_TOKEN_COMMA) return MTH_ARG_REFUSED;
            mth_take(m);
        }
        return MTH_ARG_READ;
    }
    if ((i == 0 || i == 2) && t->type == LXB_CSS_SYNTAX_TOKEN_IDENT &&
        mth_name_is((const char *)lxb_css_syntax_token_ident(t)->data, lxb_css_syntax_token_ident(t)->length,
                    "none")) {
        mth_take(m);
        f->none[i] = true;
        mth_fn_push(f, mth_val(css_math_type_number(), 0.0));
        mth_skip_ws(m);
        t = mth_peek(m);
        if (t == NULL) return MTH_ARG_REFUSED;
        if (t->type != (i < 2 ? LXB_CSS_SYNTAX_TOKEN_COMMA : LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS))
            return MTH_ARG_REFUSED;
        mth_take(m);
        return i < 2 ? MTH_ARG_FILLED : MTH_ARG_DONE;
    }
    return MTH_ARG_READ;
}

/* The math function at the cursor, simplified as §10.10.1 reduces it. Its nesting — §10.1's parentheses and
   nested functions, which §10.8 bounds only by the depth a UA supports — is walked on a heap stack of frames,
   so it costs no C stack. Every refusal refuses the whole function: the grammar has no alternative to back up
   to. */
static bool mth_function(Mth *m, MthVal *out)
{
    MthWalk w = { NULL, 0, 0 };
    MthFrame *f;
    MthVal v = mth_val(css_math_type_number(), 0.0), rhs, res;
    lxb_css_syntax_token_t *t;
    bool last, more, ws_before, ok;
    enum { MTH_OPEN, MTH_ARG, MTH_VALUE, MTH_GOT_VALUE, MTH_GOT_PAREN, MTH_GOT_ARG, MTH_CLOSE } step = MTH_OPEN;

    for (;;) {
        switch (step) {
        case MTH_OPEN:
            f = mth_walk_push(&w);
            f->is_fn = true;
            if (!mth_fn_open(m, &f->fn)) goto refused;
            step = MTH_ARG;
            break;
        case MTH_ARG:
            switch (mth_fn_before_arg(m, &w.v[w.n - 1].fn)) {
            case MTH_ARG_READ:   mth_walk_push(&w); step = MTH_VALUE; break;
            case MTH_ARG_FILLED: break;
            case MTH_ARG_DONE:   step = MTH_CLOSE; break;
            case MTH_ARG_REFUSED: goto refused;
            }
            break;
        case MTH_VALUE:
            switch (mth_leaf(m, &v)) {
            case MTH_LEAF_VALUE:    step = MTH_GOT_VALUE; break;
            case MTH_LEAF_FUNCTION: step = MTH_OPEN; break;
            case MTH_LEAF_REFUSED:  goto refused;
            case MTH_LEAF_PAREN:
                if (!mth_nest_enter(m)) goto refused;
                mth_take(m);
                mth_walk_push(&w)->paren = true;
                break;
            }
            break;
        case MTH_GOT_VALUE:
            f = &w.v[w.n - 1];
            DCHECK(!f->is_fn, "a `<calc-value>` was handed to a function frame rather than to the sum it is in");
            /* §10.8's `<calc-product> = <calc-value> [ [ '*' | / ] <calc-value> ]*`. */
            if (!f->have_prod) {
                f->prod = v;
                f->have_prod = true;
            } else {
                if (f->div) mth_divide(m, &f->prod, &v, &res);
                else mth_multiply(m, &f->prod, &v, &res);
                f->prod = res;
            }
            mth_skip_ws(m);
            t = mth_peek(m);
            if (mth_is_delim(t, '*') || mth_is_delim(t, '/')) {
                f->div = mth_is_delim(t, '/');
                mth_take(m);
                step = MTH_VALUE;
                break;
            }
            /* §10.8's `<calc-sum> = <calc-product> [ [ '+' | '-' ] <calc-product> ]*`. */
            if (!f->have_sum) {
                f->sum = f->prod;
                f->have_sum = true;
            } else {
                rhs = f->minus ? mth_negate(m, &f->prod) : f->prod;
                mth_add(m, &f->sum, &rhs, &res);
                f->sum = res;
            }
            f->have_prod = false;
            ws_before = mth_skip_ws(m);
            t = mth_peek(m);
            if (mth_is_delim(t, '+') || mth_is_delim(t, '-')) {
                f->minus = mth_is_delim(t, '-');
                /* §10.8's whitespace rule — "whitespace is required on both sides of the + and - operators" —
                   over the state before the operator is consumed and the state after: the `mth_take` between
                   them is what clears the flag, so the two reads are of the two positions the rule names. */
                mth_take(m);
                if (!ws_before || !mth_skip_ws(m)) goto refused;
                step = MTH_VALUE;
                break;
            }
            v = f->sum;
            step = f->paren ? MTH_GOT_PAREN : MTH_GOT_ARG;
            w.n--;
            break;
        case MTH_GOT_PAREN:
            mth_skip_ws(m);
            t = mth_peek(m);
            if (t == NULL || t->type != LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS) goto refused;
            mth_take(m);
            m->depth--;
            step = MTH_GOT_VALUE;
            break;
        case MTH_GOT_ARG:
            /* One comma-separated argument ends at a comma, or at the closing `)` (`last`). */
            mth_skip_ws(m);
            t = mth_peek(m);
            if (t == NULL) goto refused;
            if (t->type == LXB_CSS_SYNTAX_TOKEN_COMMA) last = false;
            else if (t->type == LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS) last = true;
            else goto refused;
            mth_take(m);
            f = &w.v[w.n - 1];
            DCHECK(f->is_fn, "a math function's argument ended into a frame that is not its function");
            mth_fn_push(&f->fn, v);
            if (!mth_fn_next(&f->fn, last, &more)) goto refused;
            step = more ? MTH_ARG : MTH_CLOSE;
            break;
        case MTH_CLOSE:
            f = &w.v[w.n - 1];
            ok = mth_function_apply(m, &f->fn, &v);
            free(f->fn.arg);
            w.n--;
            m->depth--;
            if (!ok) goto refused;
            if (w.n == 0) {
                free(w.v);
                *out = v;
                return true;
            }
            step = MTH_GOT_VALUE;
            break;
        }
    }

refused:
    mth_walk_free(&w);
    return false;
}

/* HOW MUCH OF §10 IS PART OF THE ANSWER, which is §10's own division into sections and not a convenience:
   §10.8 "Syntax" gives the grammar, and §10.9 "Type Checking" gives first a FAILURE rule ("For each of the
   above, if the type is failure, the math function is invalid") and then ONE sentence that is its validity
   and its resolution at once ("A math function resolves to <number>, <length>, <angle>, <time>, <frequency>,
   <resolution>, <flex>, or <percentage> according to which of those productions its type matches ... If it
   can't match any of these, the math function is invalid").
   THE LAST TWO RUNGS ARE TWO READINGS OF THAT ONE SENTENCE AND NOT TWO SENTENCES, which this enum was landed
   without: the VALID rung reads it with no production in hand, over §10.9's own eight, and the PRODUCTION rung
   reads it against `want`. Asking BOTH of a caller that named one asked CSS Typed OM 1 §4.3.2's
   CONTEXT-RELATIVE matching a second time in a context the walk is not in — see `mth_type_is_valid`, which
   now asserts the context it may be read in.
   THE RUNGS NEST WITHIN ONE CONTEXT and only there: each admits strictly fewer texts than the one above it,
   and the three public entries below are one rung each. ACROSS contexts they cannot, and §10.9.1 is why — a
   percentage's type is a fact about where the value sits, so `calc(25% + 10deg)` is VALID in an
   `<angle-percentage>` position and INVALID where percentages resolve against nothing. That is not a gap in
   the nesting; it is what the middle rung means, and it is why `css_math_is_valid_function` names its own
   context rather than taking one from a caller that by definition has no production to name.
   IT IS ONE WALK AND NOT THREE, which is the point: a second scan written to answer "is this one math
   function" would have to re-derive §10.8's nesting, its comma arities and its `<calc-value>` productions,
   and the day the two disagreed a caller would be told a value is not a math function by one and refused by
   the other. */
typedef enum {
    MTH_GATE_SYNTAX = 0,   /* §10.8 alone */
    MTH_GATE_VALID,        /* ... and §10.9's validity */
    MTH_GATE_PRODUCTION    /* ... and §10.9's last rule against `want` */
} MthGate;

/* ---- the per-realm table of interned calculation trees --------------------------------------------------
   A computed value is copied freely (core/css/css_length.h's CssLength carries no destructor), so the tree it
   holds is immutable and interned: one copy per distinct tree per realm, keyed by its whole content, owned by
   the realm and freed with it. A tree's leaves name the realm's environment facts, which is why the realm and
   no wider scope owns it. */

typedef struct {
    char               **key;    /* open-addressed, a power of two long, at most half full; NULL is empty */
    struct CssMathTree **tree;   /* beside `key`; both OWNED */
    size_t               cap, n;
} MthTreeTable;

static JSClassID g_mth_tree_table_class;
static JSClassID g_mth_tree_table_slot;

static void mth_tree_table_finalizer(JSRuntime *rt, JSValue v)
{
    MthTreeTable *t = JS_GetOpaque(v, g_mth_tree_table_class);
    size_t i;

    (void)rt;
    if (t == NULL) return;
    for (i = 0; i < t->cap; i++) {
        if (t->key[i] == NULL) continue;
        free(t->key[i]);
        mth_tree_free(t->tree[i]);
    }
    free(t->key);
    free(t->tree);
    free(t);
}

static void mth_install_realm(JSContext *ctx)
{
    JSValue obj = JS_NewObjectClass(ctx, (int)g_mth_tree_table_class);
    MthTreeTable *t;

    CHECK(!JS_IsException(obj), "css_math: OOM creating a realm's calculation tree table");
    t = lexbor_calloc(1, sizeof *t);
    CHECK(t != NULL, "css_math: OOM creating a realm's calculation tree table");
    JS_SetOpaque(obj, t);
    realm_value_set(ctx, g_mth_tree_table_slot, obj);
}

void css_math_init(JSContext *ctx)
{
    JSClassDef def = { "CssMathTreeTable", .finalizer = mth_tree_table_finalizer };
    JSRuntime *rt = JS_GetRuntime(ctx);

    DCHECK(g_mth_tree_table_slot == JS_INVALID_CLASS_ID, "css_math_init ran twice — the tree table's class and "
                                                         "slot are the agent's");
    JS_NewClassID(rt, &g_mth_tree_table_class);
    CHECK(JS_NewClass(rt, g_mth_tree_table_class, &def) == 0, "css_math: the tree table's class could not be "
                                                               "declared");
    g_mth_tree_table_slot = realm_value_declare(ctx, "css_math.h's per-realm table of interned calculation trees");
    agent_state_class("css_math", &g_mth_tree_table_class, "the per-realm calculation tree table's class");
    agent_state_realm_slot("css_math", &g_mth_tree_table_slot, "the per-realm calculation tree table's slot");
    realm_declare_intrinsic(mth_install_realm);
}

void css_math_free(void)
{
    g_mth_tree_table_slot = JS_INVALID_CLASS_ID;
    g_mth_tree_table_class = 0;
}

typedef struct {
    char  *p;
    size_t n, cap;
} MthBuf;

static void mth_buf_put(MthBuf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        char *grown;

        while (b->n + n + 1 > cap) cap *= 2;
        grown = lexbor_realloc(b->p, cap);
        CHECK(grown != NULL, "css_math: OOM composing a calculation tree's text");
        b->p = grown;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}

static void mth_buf_puts(MthBuf *b, const char *s) { mth_buf_put(b, s, strlen(s)); }

static void mth_buf_printf(MthBuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void mth_buf_printf(MthBuf *b, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    int w;

    va_start(ap, fmt);
    w = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    CHECK(w >= 0 && (size_t)w < sizeof tmp, "css_math: a calculation tree's key field outgrew its buffer");
    mth_buf_put(b, tmp, (size_t)w);
}

/* A value's identity over every field a resolution or a serialization reads, a node's children included. The
   environment facts are named by their interned form and realm, which identify them within the realm. */
static void mth_key(MthBuf *b, const MthVal *v)
{
    unsigned i;

    mth_buf_puts(b, "{");
    for (i = 0; i < CSS_MATH_BASE_COUNT; i++) mth_buf_printf(b, "%d,", v->type.exp[i]);
    mth_buf_printf(b, "h%d;", v->type.hint);
    if (v->node == NULL) {
        mth_buf_printf(b, "%a,%u,%p,%p,%a,%d%d}", v->num.px, (unsigned)v->num.env, (void *)v->num.realm,
                       (const void *)v->num.form, v->pct, v->pct_term, v->num_term);
        return;
    }
    mth_buf_printf(b, "o%d,%.*s,%d,%d%d%d,%u:", (int)v->node->op, (int)v->node->nlen, v->node->name,
                   v->node->how, v->node->none[0], v->node->none[1], v->node->none[2], v->node->n);
    for (i = 0; i < v->node->n; i++) mth_key(b, &v->node->kid[i]);
    mth_buf_puts(b, "}");
}

static uint64_t mth_key_hash(const char *k)
{
    uint64_t h = 1469598103934665603ull;

    for (; *k; k++) h = (h ^ (unsigned char)*k) * 1099511628211ull;
    return h;
}

/* A deep copy of the arena node `src` into storage `realm` owns. */
static struct CssMathTree *mth_tree_copy(const struct CssMathTree *src, JSContext *realm)
{
    struct CssMathTree *t = lexbor_malloc(sizeof *t);
    unsigned i;

    CHECK(t != NULL, "css_math: OOM interning a calculation tree");
    *t = *src;
    t->realm = realm;
    t->arena_next = NULL;
    t->kid = lexbor_malloc((src->n ? src->n : 1) * sizeof *t->kid);
    CHECK(t->kid != NULL, "css_math: OOM interning a calculation tree");
    for (i = 0; i < src->n; i++) {
        t->kid[i] = src->kid[i];
        if (src->kid[i].node != NULL) t->kid[i].node = mth_tree_copy(src->kid[i].node, realm);
    }
    return t;
}

/* `realm`'s one copy of the tree whose root is `v`, a node of a parse's arena. */
static const CssMathTree *mth_tree_intern(JSContext *realm, const MthVal *v)
{
    MthBuf b = { NULL, 0, 0 };
    JSValue obj;
    MthTreeTable *t;
    size_t i;

    DCHECK(v->node != NULL, "a value that is no calculation node was interned as a tree");
    mth_key(&b, v);
    obj = realm_value_get(realm, g_mth_tree_table_slot);
    t = JS_GetOpaque(obj, g_mth_tree_table_class);
    JS_FreeValue(realm, obj);
    CHECK(t != NULL, "css_math: a realm's calculation tree table is missing — its install runs at every realm's "
                     "creation");
    if (2 * (t->n + 1) > t->cap) {
        size_t cap = t->cap ? 2 * t->cap : 32, j;
        char **keys = lexbor_calloc(cap, sizeof *keys);
        struct CssMathTree **trees = lexbor_calloc(cap, sizeof *trees);

        CHECK(keys != NULL && trees != NULL, "css_math: OOM growing a realm's calculation tree table");
        for (j = 0; j < t->cap; j++) {
            if (t->key[j] == NULL) continue;
            for (i = mth_key_hash(t->key[j]) & (cap - 1); keys[i]; i = (i + 1) & (cap - 1)) {}
            keys[i] = t->key[j];
            trees[i] = t->tree[j];
        }
        free(t->key);
        free(t->tree);
        t->key = keys;
        t->tree = trees;
        t->cap = cap;
    }
    for (i = mth_key_hash(b.p) & (t->cap - 1); t->key[i]; i = (i + 1) & (t->cap - 1))
        if (strcmp(t->key[i], b.p) == 0) {
            free(b.p);
            return t->tree[i];
        }
    t->key[i] = b.p;
    t->tree[i] = mth_tree_copy(v->node, realm);
    t->n++;
    return t->tree[i];
}

/* ---- §10.11's used-value simplification ------------------------------------------------------------------ */

static CssPx mth_tree_no_unit(void *ctx, double n, const char *unit, size_t unit_len)
{
    (void)ctx; (void)n; (void)unit; (void)unit_len;
    DFAIL("a calculation tree's leaf was converted to its canonical unit at used-value time. Every dimension leaf "
          "was canonicalized when the tree was made, so only percentages are left to resolve");
    return css_px(0.0);
}

/* `v` with every percentage resolved against `basis` and every operator node run: a Sum of one numeric term. */
static MthVal mth_tree_value(Mth *m, const MthVal *v, CssPx basis)
{
    const struct CssMathTree *t = v->node;
    MthVal *r, out, acc, one;
    MthFn f;
    unsigned i;
    bool ok;

    if (t == NULL) {
        out = *v;
        if (out.pct_term) out.num = css_px_add(css_px_scale(basis, out.pct / 100.0), out.num);
        out.pct = 0.0;
        out.pct_term = false;
        out.num_term = true;
        return out;
    }
    r = lexbor_malloc((t->n ? t->n : 1) * sizeof *r);
    CHECK(r != NULL, "css_math: OOM resolving a calculation tree");
    for (i = 0; i < t->n; i++) r[i] = mth_tree_value(m, &t->kid[i], basis);
    switch (t->op) {
    case MTH_NODE_SUM:
    case MTH_NODE_PRODUCT:
        acc = r[0];
        for (i = 1; i < t->n; i++) {
            if (t->op == MTH_NODE_SUM) mth_add(m, &acc, &r[i], &out);
            else mth_multiply(m, &acc, &r[i], &out);
            acc = out;
        }
        out = acc;
        break;
    case MTH_NODE_NEGATE:
        out = mth_negate(m, &r[0]);
        break;
    case MTH_NODE_INVERT:
        one = mth_val(css_math_type_number(), 1.0);
        mth_divide(m, &one, &r[0], &out);
        break;
    case MTH_NODE_FUNCTION:
        memset(&f, 0, sizeof f);
        memcpy(f.name, t->name, t->nlen);
        f.nlen = t->nlen;
        f.shape = (MthShape)t->shape;
        f.how = (MthRounding)t->how;
        memcpy(f.none, t->none, sizeof f.none);
        f.arg = r;
        f.n = f.cap = t->n;
        ok = mth_function_value(m, &f, &out);
        DCHECK(ok, "a calculation node's function refused operands of the types it was built over");
        break;
    default:
        DFAIL("a calculation node carries an operation §10.10 does not define");
        out = r[0];
        break;
    }
    free(r);
    DCHECK(out.node == NULL && !out.pct_term && out.blocked == MTH_RESOLVED,
           "a calculation tree did not simplify to a single numeric value at used-value time, though §10.11 says "
           "\"with used value time information, a math function always simplifies down to a single numeric "
           "value\" and every percentage leaf has just been resolved");
    return out;
}

CssPx css_math_tree_resolve(const CssMathTree *tree, CssPx basis)
{
    CssMathResolver res = { mth_tree_no_unit, NULL, NULL };
    Mth m;
    MthVal root;
    CssPx px;

    DCHECK(tree != NULL && tree->realm != NULL, "a calculation tree was resolved that no realm interned");
    memset(&m, 0, sizeof m);
    res.realm = tree->realm;
    m.res = &res;
    root = mth_val(tree->type, 0.0);
    root.node = (struct CssMathTree *)tree;
    px = mth_tree_value(&m, &root, basis).num;
    DCHECK(m.arena == NULL, "the used-value simplification made a calculation node");
    /* §10.9.2's top-level censoring, as mth_top applies it to a function reduced at parse time. */
    if (px.px != px.px) px = mth_atom("nan-censor", 10, NULL, &px, 1, 0.0);
    if (px.px == 0.0) px = css_px_add(px, css_px(0.0));
    return px;
}

CssEnvSet css_math_tree_env(const CssMathTree *tree)
{
    CssEnvSet env = CSS_ENV_NONE;
    unsigned i;

    for (i = 0; i < tree->n; i++)
        env |= tree->kid[i].node != NULL ? css_math_tree_env(tree->kid[i].node) : tree->kid[i].num.env;
    return env;
}

/* ---- §10.13's serialization of a computed calculation tree ------------------------------------------------ */

static const char *mth_canonical_unit(const CssMathType *t)
{
    if (mth_only(t, CSS_MATH_LENGTH)) return "px";
    if (mth_only(t, CSS_MATH_ANGLE)) return "deg";
    if (mth_only(t, CSS_MATH_TIME)) return "s";
    if (mth_only(t, CSS_MATH_FREQUENCY)) return "hz";
    if (mth_only(t, CSS_MATH_RESOLUTION)) return "dppx";
    if (mth_no_entries(t)) return "";
    DFAIL("a calculation tree's numeric leaf has a type no canonical unit serializes — §10.9's last rule admits "
          "only the eight productions, so a leaf of another type is a tree built over an invalid function");
    return "";
}

static void mth_ser_number(MthBuf *b, double v, const char *unit)
{
    char *t = css_length_serialize_number(v, unit);

    mth_buf_puts(b, t);
    free(t);
}

static void mth_ser(MthBuf *b, const MthVal *v);

/* A numeric child of a Sum, as its terms in "sort a calculation's children nodes"' order — a number or a
   percentage before a dimension — each after its sign: " + ", or " - " and the negation for a negative one. */
static void mth_ser_terms(MthBuf *b, const MthVal *v, bool *first)
{
    const char *unit = mth_canonical_unit(&v->type);
    bool pct_first = v->pct_term, num_after = v->num_term || !v->pct_term;
    double terms[2];
    int kinds[2], k = 0, i;

    if (pct_first) { terms[k] = v->pct; kinds[k++] = 1; }
    if (num_after) { terms[k] = v->num.px; kinds[k++] = 0; }
    for (i = 0; i < k; i++) {
        double x = terms[i];

        if (!*first) {
            mth_buf_puts(b, x < 0.0 || (x == 0.0 && signbit(x)) ? " - " : " + ");
            x = fabs(x);
        }
        mth_ser_number(b, x, kinds[i] ? "%" : unit);
        *first = false;
    }
}

static void mth_ser_stripped(MthBuf *b, const MthVal *v)
{
    MthBuf in = { NULL, 0, 0 };

    mth_ser(&in, v);
    if (in.n >= 2 && in.p[0] == '(' && in.p[in.n - 1] == ')') mth_buf_put(b, in.p + 1, in.n - 2);
    else mth_buf_puts(b, in.p);
    free(in.p);
}

/* §10.13's "serialize a calculation tree". A numeric child is a Sum of up to two terms, written as one. */
static void mth_ser(MthBuf *b, const MthVal *v)
{
    const struct CssMathTree *t = v->node;
    bool first = true;
    unsigned i, pass;

    if (t == NULL) {
        bool two = v->pct_term && v->num_term;

        if (two) mth_buf_puts(b, "(");
        mth_ser_terms(b, v, &first);
        if (two) mth_buf_puts(b, ")");
        return;
    }
    switch (t->op) {
    case MTH_NODE_NEGATE:
        mth_buf_puts(b, "(-1 * ");
        mth_ser(b, &t->kid[0]);
        mth_buf_puts(b, ")");
        return;
    case MTH_NODE_INVERT:
        mth_buf_puts(b, "(1 / ");
        mth_ser(b, &t->kid[0]);
        mth_buf_puts(b, ")");
        return;
    case MTH_NODE_SUM:
        /* "Sort root's children": the one numeric child's terms first, then the nodes in order. */
        mth_buf_puts(b, "(");
        for (pass = 0; pass < 2; pass++)
            for (i = 0; i < t->n; i++) {
                const MthVal *c = &t->kid[i];

                if ((c->node == NULL) != (pass == 0)) continue;
                if (c->node == NULL) { mth_ser_terms(b, c, &first); continue; }
                if (!first) mth_buf_puts(b, c->node->op == MTH_NODE_NEGATE ? " - " : " + ");
                mth_ser(b, c->node->op == MTH_NODE_NEGATE && !first ? &c->node->kid[0] : c);
                first = false;
            }
        mth_buf_puts(b, ")");
        return;
    case MTH_NODE_PRODUCT:
        mth_buf_puts(b, "(");
        for (pass = 0; pass < 2; pass++)
            for (i = 0; i < t->n; i++) {
                const MthVal *c = &t->kid[i];

                if ((c->node == NULL) != (pass == 0)) continue;
                if (!first) mth_buf_puts(b, c->node != NULL && c->node->op == MTH_NODE_INVERT ? " / " : " * ");
                mth_ser(b, c->node != NULL && c->node->op == MTH_NODE_INVERT && !first ? &c->node->kid[0] : c);
                first = false;
            }
        mth_buf_puts(b, ")");
        return;
    case MTH_NODE_FUNCTION:
        for (i = 0; i < t->nlen; i++) {
            char ch = (char)tolower((unsigned char)t->name[i]);

            mth_buf_put(b, &ch, 1);
        }
        mth_buf_puts(b, "(");
        if (t->how != MTH_ROUND_NEAREST && mth_name_is(t->name, t->nlen, "round")) {
            static const char *const HOW[] = { "nearest", "up", "down", "to-zero", "line-width" };

            mth_buf_puts(b, HOW[t->how]);
            mth_buf_puts(b, ", ");
        }
        for (i = 0; i < t->n; i++) {
            if (i > 0) mth_buf_puts(b, ", ");
            if (i < 3 && t->none[i]) mth_buf_puts(b, "none");
            else mth_ser_stripped(b, &t->kid[i]);
        }
        mth_buf_puts(b, ")");
        return;
    }
    DFAIL("a calculation node carries an operation §10.13 does not serialize");
}

char *css_math_tree_serialize(const CssMathTree *tree)
{
    MthBuf b = { NULL, 0, 0 };
    MthVal root;

    DCHECK(tree != NULL, "a calculation tree was serialized that does not exist");
    root = mth_val(tree->type, 0.0);
    root.node = (struct CssMathTree *)tree;
    /* §10.13's serialize a math function: a function root is its name; a calc-operator root is `calc(`. */
    if (tree->op == MTH_NODE_FUNCTION) {
        mth_ser(&b, &root);
        return b.p;
    }
    mth_buf_puts(&b, "calc(");
    mth_ser_stripped(&b, &root);
    mth_buf_puts(&b, ")");
    return b.p;
}

/* ---- the three public questions -------------------------------------------------------------------------- */

/* Parse the whole of `text` as ONE math function, in §10.9's type-only mode when `res` is NULL. `gate` says
   which of §10's three rungs the answer is; see MthGate.
   `want` IS READ UNDER EVERY GATE, including MTH_GATE_SYNTAX, because it names §10.9.1's calculation context
   and the `<percentage>` terminal types a percentage as the base that context resolves it against. What the
   gate decides is whether that TYPE can then refuse — the walk itself is total over §4.3.2's failure (see
   `mth_types_added`), so no type it can compute stops it reaching EOF, and the syntax rung's answer is
   therefore the same under every `want`. It was not always: while the algebra refused, `calc(25% + 10deg)`
   was ONE math function under `<angle-percentage>` and not a math function at all under anything else, and
   the caller that asked had to invent the context it was answered in. */
static bool mth_top(const char *text, size_t len, const CssMathResolver *res, CssMathProduction want,
                    MthGate gate, CssMathValue *out)
{
    Mth m = { NULL, res, CSS_MATH_HINT_NULL, 0, false };
    lxb_css_syntax_token_t *t;
    MthVal v = mth_val(css_math_type_number(), 0.0);
    bool ok = false;

    DCHECK(text != NULL || len == 0,
           "a math function was read from a NULL span with a non-zero length — the caller holds the value's own "
           "bytes by the time it can know a FUNCTION token starts them, so an absent pointer is a caller that "
           "lost the buffer");
    DCHECK(res == NULL || res->length_px != NULL,
           "a math function was evaluated through a resolver with no `<length>` conversion. css_math.h makes "
           "that callback the ONE question this component asks its caller, so a resolver without it is one "
           "assembled field-by-field rather than by the caller that owns the unit table — and the first `2em` "
           "would call through a NULL");
    m.pct_base = mth_pct_base(want);
    m.tkz = lxb_css_syntax_tokenizer_create();
    CHECK(m.tkz != NULL, "css math: the math function tokenizer allocation failed");
    if (lxb_css_syntax_tokenizer_init(m.tkz) != LXB_STATUS_OK) {
        lxb_css_syntax_tokenizer_destroy(m.tkz);
        return false;
    }
    lxb_css_syntax_tokenizer_buffer_set(m.tkz, (const lxb_char_t *)text, len);
    mth_skip_ws(&m);
    t = mth_peek(&m);
    if (t == NULL || t->type != LXB_CSS_SYNTAX_TOKEN_FUNCTION) goto done;
    if (!css_math_is_function((const char *)lxb_css_syntax_token_function(t)->data,
                              lxb_css_syntax_token_function(t)->length)) goto done;
    if (!mth_function(&m, &v)) goto done;
    DCHECK(m.depth == 0,
           "the math-function parse returned with an unbalanced nesting depth — every production that "
           "increments it decrements it on both its success and its failure path, so a non-zero count here is "
           "one that returns without doing so and would refuse a later sibling for depth it is not using");
    /* A math function is ONE component value. Anything after it means the caller handed more than one, which no
       grammar here can answer for; it is refused rather than asserted because the entry takes arbitrary text. */
    mth_skip_ws(&m);
    t = mth_peek(&m);
    if (t == NULL || t->type != LXB_CSS_SYNTAX_TOKEN__EOF) goto done;
    /* §10.9's FAILURE RULE, which is a rule of its OWN and stands in front of BOTH gates below: "For each of
       the above, if the type is failure, the math function is invalid." An INVALID math function is still ONE
       math function, so §10.8's grammar has already been satisfied by the time control is here; and this step
       has to precede every match, because `mth_type_matches` asserts it is never handed a failure — an assert
       that is LIVE rather than decorative only because the algebra above now PRODUCES a failure type where it
       used to refuse the walk. */
    if (gate != MTH_GATE_SYNTAX && css_math_type_is_failure(&v.type)) goto done;
    /* §10.9's LAST RULE, ASKED ONCE AND IN THE CONTEXT THE CALLER NAMED: "A math function resolves to
       <number>, <length>, ... according to which of those productions its type matches ... If it can't match
       any of these, the math function is invalid." The two lines below are two READINGS of that sentence and
       never two sentences — against `want` where a production is named, over §10.9's own eight where none is
       — so exactly one of them runs. Running the eight-way one FIRST, under every gate, asked CSS Typed OM 1
       §4.3.2's matching in a context the walk was not in: it carries that section's percent-hint-must-be-null
       bullet, so it refused every type a `<length-percentage>` or `<angle-percentage>` context produces, and
       `css_math_matches` answered false for `calc(100% - 20px)` and `calc(25% + 10deg)` in the one position
       §5.6 defines them for. `mth_type_is_valid` now asserts the context it may be read in. */
    if (gate == MTH_GATE_VALID && !mth_type_is_valid(&v.type, want)) goto done;
    if (gate == MTH_GATE_PRODUCTION && !mth_type_matches(&v.type, want)) goto done;
    ok = true;
    if (res == NULL || out == NULL) goto done;
    if (v.blocked == MTH_UNRESOLVED_FLEX)
        DFAIL("a VALID math function resolves to a `<flex>` and this engine has no grid track sizer to give one "
              "a number. css-grid-2 §7.2.4 'Flexible Lengths: the fr unit' makes `1fr` \"a fraction of the "
              "leftover space in the grid container\", so its value exists only inside css-grid-2 §12 'Grid "
              "Sizing' once the free space of a track list is known — it is not a length and must not be built "
              "out of one. TYPING it is right and is done: §10.9's base types include \"flex\" and "
              "`calc(1fr + 1fr)` IS a `<flex>`, which is why every caller that asks for a `<length>` refuses "
              "one before reaching here. BUILD css-grid-2 §12's track sizing algorithm, and resolve an `fr` "
              "from the free space it computes");
    DCHECK(v.blocked == MTH_RESOLVED, "a math function's top-level value carries a block its function node "
                                      "was meant to absorb");
    out->tree = NULL;
    if (v.node != NULL) {
        /* §10.11: the calculation tree is the computed value until used-value time supplies the basis. */
        DCHECK(res->realm != NULL, "a math function that keeps its calculation tree was evaluated through a "
                                   "resolver with no realm to own the tree");
        out->type = v.type;
        out->num = css_px(0.0);
        out->pct = 0.0;
        out->pct_term = false;
        out->tree = mth_tree_intern(res->realm, &v);
        goto done;
    }
    out->type = v.type;
    out->num = v.num;
    out->pct = v.pct;
    out->pct_term = v.pct_term;
    /* §10.9.2's TOP-LEVEL CENSORING, the context-free half: "NaN does not escape a top-level calculation; it's
       censored into a zero value" and "Signed zeroes do not escape a top-level calculation; they're censored
       into the 'unsigned' zero." An INFINITY is left alone — §10.9.2 clamps it "to the minimum or maximum value
       allowed in the context, as defined in §10.12 Range Checking", and the context is the caller's property. */
    if (out->num.px != out->num.px) out->num = mth_atom("nan-censor", 10, NULL, &out->num, 1, 0.0);
    /* Adding 0⁺ is the zero's censoring and keeps its form: under round-to-nearest, 0⁻ + 0⁺ is 0⁺. */
    if (out->num.px == 0.0) out->num = css_px_add(out->num, css_px(0.0));
    if (out->pct != out->pct) out->pct = 0.0;
    if (out->pct == 0.0) out->pct = fabs(out->pct);
    DCHECK(out->num.px == out->num.px && out->pct == out->pct,
           "a NaN escaped a top-level math function. css-values-4 §10.9.2 'Infinities, NaN, and Signed Zero' "
           "makes that impossible by definition — \"NaN does not escape a top-level calculation; it's censored "
           "into a zero value\" — so one here is the censoring above having been skipped, and every caller "
           "would go on to compare it and take the wrong arm silently");
    DCHECK(!signbit(out->num.px) || out->num.px != 0.0,
           "a NEGATIVE ZERO escaped a top-level math function. §10.9.2: \"Signed zeroes do not escape a "
           "top-level calculation; they're censored into the 'unsigned' zero\", and 0⁻ is observable — "
           "`calc(1 / calc(-5 * 0))` is −∞ while `calc(1 / (0))` is +∞ — so one that leaks decides a later "
           "division's sign on a fact the spec says was erased");
    /* §10.9: "math functions that resolve to <number> can be used in any place that only accepts <integer>; the
       value is rounded to the nearest integer as it resolves." */
    if (gate == MTH_GATE_PRODUCTION && want == CSS_MATH_PROD_INTEGER)
        out->num = mth_atom("integer", 7, NULL, &out->num, 1,
                            mth_round_value(MTH_ROUND_NEAREST, out->num.px, 1.0));

done:
    while (m.arena != NULL) {
        struct CssMathTree *next = m.arena->arena_next;

        free(m.arena->kid);
        free(m.arena);
        m.arena = next;
    }
    lxb_css_syntax_tokenizer_destroy(m.tkz);
    return ok;
}

bool css_math_matches(const char *text, size_t len, CssMathProduction want)
{
    return mth_top(text, len, NULL, want, MTH_GATE_PRODUCTION, NULL);
}

bool css_math_is_valid_function(const char *text, size_t len)
{
    /* `want` NAMES §10.9.1's CALCULATION CONTEXT, which decides the TYPE the validity question is then asked
       of AND which of §4.3.2's matching bullets that question may be read through — the two are one fact, so
       `mth_type_is_valid` asserts this context rather than taking any. `CSS_MATH_PROD_NUMBER` is the context
       whose percentages resolve against nothing, which is §10.9's own "Otherwise" arm and the only honest
       answer for a caller that holds no property grammar: a bare `calc(50%)` is a valid `<percentage>` under
       it, and `calc(25% + 10deg)` is INVALID under it and valid in a position that states an
       `<angle-percentage>`. A caller that knows the position asks `css_math_matches` for it instead, and gets
       §10.9's last rule read against that position rather than against this one. */
    return mth_top(text, len, NULL, CSS_MATH_PROD_NUMBER, MTH_GATE_VALID, NULL);
}

bool css_math_is_lone_function(const char *text, size_t len)
{
    /* `want` IS READ on this path and the ANSWER does not depend on it, which are two different facts and
       only the second is this entry's contract. §10.9's `<percentage>` terminal types a percentage as the base
       §10.9.1's calculation context resolves it against, and `want` is where that context comes from — so a
       walk whose TYPE STEP could refuse would answer a different question for every production a caller named,
       and did: `calc(25% + 10deg)` typed angle+angle under `<angle-percentage>` and percent+angle here, and
       only the second failed. The type algebra is TOTAL now (see `mth_types_added`), so no type this walk can
       compute stops it reaching EOF, and `CSS_MATH_PROD_NUMBER` names the context whose percentages resolve
       against nothing — the one a caller that has not yet decided what it is asking for would mean.
       `res` is NULL, so §10.9's rounding step — the other reader of `want` — is not reached either. */
    return mth_top(text, len, NULL, CSS_MATH_PROD_NUMBER, MTH_GATE_SYNTAX, NULL);
}

bool css_math_eval(const char *text, size_t len, const CssMathResolver *res, CssMathProduction want,
                   CssMathValue *out)
{
    DCHECK(res != NULL && out != NULL,
           "a math function was evaluated with no resolver or nowhere to write the result — the TYPE question "
           "needs neither and is `css_math_matches`, so an absent one here is a caller that meant to ask that "
           "one instead");
    return mth_top(text, len, res, want, MTH_GATE_PRODUCTION, out);
}
