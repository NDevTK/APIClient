/* CSS Values and Units 4 §10 "Mathematical Expressions". See css_math.h for why this is a component, what
 * lexbor supplied and what is ported, why the type algebra is not arithmetic on numbers, and where §10.10.1's
 * simplification stops. */
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <lexbor/css/syntax/token.h>
#include <lexbor/css/syntax/tokenizer.h>

#include "check.h"
#include "core/css/css_dimension.h"
#include "core/css/css_length.h"
#include "core/css/css_math.h"

/* §10.7.1 "Numeric Constants: e, pi", to the digits that section itself prints: e is "approximately equal to
   2.7182818284590452354" and pi "approximately equal to 3.1415926535897932". Written out rather than taken
   from `M_E`/`M_PI`, which are POSIX rather than C and whose value is the platform's rather than the spec's. */
#define MTH_E  2.7182818284590452354
#define MTH_PI 3.1415926535897932

/* §10.8's own limit, in that section's own words: "UAs must support calculations of at least 32 <calc-value>
   terms and at least 32 levels of nesting (parentheses and/or functions) ... If a calculation contains more
   than the supported number of terms, arguments, or nesting it must be treated as if it were invalid." This is
   a GRAMMAR rule the spec states and not a cap on work — a calculation past it is INVALID CSS, which is what a
   real user agent answers too, and the number below is this user agent's supported maximum, chosen far above
   §10.8's floor of 32 and low enough that the recursive descent below cannot exhaust the C stack. */
#define MTH_NESTING_MAX 256

/* ---- CSS Typed OM 1 §4.3.2 "Numeric Value Typing" — the type algebra §10.9 links to by name -------------- */

static CssMathType mth_type_number(void)
{
    CssMathType t;

    memset(&t, 0, sizeof t);
    t.hint = CSS_MATH_HINT_NULL;
    return t;
}

static CssMathType mth_type_of(CssMathBase base)
{
    CssMathType t = mth_type_number();

    DCHECK((unsigned)base < (unsigned)CSS_MATH_BASE_COUNT,
           "a CSS numeric type was built over a base type outside CSS Typed OM 1 §4.3.2's seven — that section "
           "names them exhaustively (\"length\", \"angle\", \"time\", \"frequency\", \"resolution\", \"flex\", "
           "and \"percent\"), so an eighth is a unit family this file classified into a slot the enum does not "
           "have");
    t.exp[base] = 1;
    return t;
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

/* §4.3.2's "To add two types type1 and type2". */
static bool mth_type_add(const CssMathType *type1, const CssMathType *type2, CssMathType *out)
{
    CssMathType t1 = *type1, t2 = *type2;
    unsigned hint;

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

/* §4.3.2's "To multiply two types type1 and type2". */
static bool mth_type_mul(const CssMathType *type1, const CssMathType *type2, CssMathType *out)
{
    CssMathType t1 = *type1, t2 = *type2;
    unsigned i;

    if (t1.hint != CSS_MATH_HINT_NULL && t2.hint != CSS_MATH_HINT_NULL && t1.hint != t2.hint) return false;
    if (t1.hint != CSS_MATH_HINT_NULL && t2.hint == CSS_MATH_HINT_NULL) mth_apply_hint(&t2, t1.hint);
    else if (t2.hint != CSS_MATH_HINT_NULL && t1.hint == CSS_MATH_HINT_NULL) mth_apply_hint(&t1, t2.hint);
    *out = t1;
    for (i = 0; i < CSS_MATH_BASE_COUNT; i++) out->exp[i] = t1.exp[i] + t2.exp[i];
    out->hint = t1.hint;
    return true;
}

/* §4.3.2's "To invert a type type" — every exponent negated, the percent hint kept. */
static CssMathType mth_type_inv(const CssMathType *t)
{
    CssMathType out = *t;
    unsigned i;

    for (i = 0; i < CSS_MATH_BASE_COUNT; i++) out.exp[i] = -t->exp[i];
    return out;
}

/* §10.9's "To make a type base consistent with another type input", which is how every function whose RESULT
   type is fixed (`sin()` is a `<number>`, `atan()` an `<angle>`) still carries its argument's percent hint. */
static bool mth_make_consistent(CssMathType *base, const CssMathType *input)
{
    if (base->hint != CSS_MATH_HINT_NULL && input->hint != CSS_MATH_HINT_NULL && base->hint != input->hint)
        return false;
    if (base->hint == CSS_MATH_HINT_NULL) base->hint = input->hint;
    return true;
}

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
    }
    DFAIL("a math function's resolved type was matched against a production outside CssMathProduction — the "
          "enum and this switch are one list and have come apart");
    return false;
}

/* ---- §10.9.2 "Infinities, NaN, and Signed Zero" ---------------------------------------------------------- */

/* §10.9.2: "When comparing 0⁺ and 0⁻, 0⁻ is less than 0⁺. For example, min(0⁺, 0⁻) must produce 0⁻". C leaves
   `fmin(-0.0, 0.0)` unspecified, so the comparison is written rather than borrowed. NaN is NOT decided here —
   every caller tests for it first, because §10.5.1's note makes NaN infectious in every function while a
   comparison would simply answer false. */
static bool mth_less(double a, double b)
{
    DCHECK(a == a && b == b,
           "css-values-4 §10.9.2's zero-aware comparison was handed a NaN. §10.5.1's note makes NaN INFECTIOUS "
           "in every math function — \"forcing the function to return NaN if any argument calculation is "
           "NaN\" — so a NaN must be answered by the caller before any ordering question is asked; deciding it "
           "here would silently pick the other operand, which is JS's `Math.min` behavior and is exactly the "
           "divergence that note says CSS does not have");
    if (a == 0.0 && b == 0.0) return signbit(a) && !signbit(b);
    return a < b;
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
    /* §10.10.1 left a LIVE TREE NODE and this file holds no tree — a Product of two percentage-carrying
       operands, or a Min whose children cannot be compared because "percentages might resolve against a
       negative basis". */
    MTH_UNRESOLVED_TREE,
    /* css-grid-2 §7.2.4's `fr`, whose value exists only inside css-grid-2 §12's track sizing. */
    MTH_UNRESOLVED_FLEX
} MthUnresolved;

/* The FIRST reason wins, so the innermost absence is the one reported — a `min(1fr, 50% * 50%)` names the track
   sizer rather than the tree, because the `fr` is what the reader meets first when they open the value. */
static MthUnresolved mth_worse(MthUnresolved a, MthUnresolved b)
{
    return a != MTH_RESOLVED ? a : b;
}

typedef struct {
    CssMathType   type;
    CssPx         num;      /* the non-percentage term, in §5.4.1's canonical unit for `type` */
    double        pct;      /* the `<percentage>` term, as a number of percent */
    bool          pct_term; /* §10.10.1: a zero-valued Sum term is not removable, so this is not `pct != 0` */
    MthUnresolved blocked;
} MthVal;

static MthVal mth_val(CssMathType type, double n)
{
    MthVal v;

    v.type = type;
    v.num = css_px(n);
    v.pct = 0.0;
    v.pct_term = false;
    v.blocked = MTH_RESOLVED;
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
} Mth;

static bool mth_sum(Mth *m, MthVal *out);

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

/* §10.8's `<calc-value>` — "<number> | <dimension> | <percentage> | <calc-keyword> | ( <calc-sum> )" — plus
   §10.10's "If leaf is a math function, replace leaf with the internal representation of that math function". */
static bool mth_function(Mth *m, MthVal *out);

static bool mth_value(Mth *m, MthVal *out)
{
    lxb_css_syntax_token_t *t;

    mth_skip_ws(m);
    t = mth_peek(m);
    if (t == NULL) return false;
    switch (t->type) {
    case LXB_CSS_SYNTAX_TOKEN_NUMBER:
        /* §10.9: "<number> / <integer>: the type is «[ ]» (empty map)". Its own note is why a bare `0` is not a
           length here: "Because <number-token>s are always interpreted as <number>s or <integer>s, "unitless
           zero" <length>s aren't supported in math functions. That is, width: calc(0 + 5px); is invalid". */
        *out = mth_val(mth_type_number(), lxb_css_syntax_token_number(t)->num);
        mth_take(m);
        return true;
    case LXB_CSS_SYNTAX_TOKEN_PERCENTAGE: {
        /* §10.9's `<percentage>` terminal rule, under §10.9.1's calculation context. The number is NOT resolved
           — §10.11: "Where percentages are not resolved at computed-value time, they are not resolved in math
           functions" — so it survives as the Sum's percentage term and the hint records what it will become. */
        CssMathType ty = mth_type_number();

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
        mth_take(m);
        return true;
    }
    case LXB_CSS_SYNTAX_TOKEN_DIMENSION: {
        const lxb_css_syntax_token_string_t *u = lxb_css_syntax_token_dimension_string(t);
        double n = lxb_css_syntax_token_dimension(t)->num.num;
        CssMathBase base;

        /* The unit span points into the tokenizer's own buffer and is read BEFORE the token is consumed —
           lexbor keeps a token's cooked string only until the next one is requested. */
        if (!css_math_unit_base((const char *)u->data, u->length, &base)) return false;
        *out = mth_val(mth_type_of(base), 0.0);
        /* §10.9's TYPE is answered for every family; only the VALUE splits. css-grid-2 §7.2.4's `fr` has no
           number outside css-grid-2 §12's track sizing, so it is marked unresolved and carried — the type
           still says `<flex>`, so `width: calc(1fr)` is refused by §10.9's last rule as invalid CSS and the
           crash below is reached only by a caller that genuinely asked for a `<flex>` value. */
        if (m->res != NULL) {
            if (base == CSS_MATH_FLEX) out->blocked = MTH_UNRESOLVED_FLEX;
            else out->num = mth_canonical(m, base, n, (const char *)u->data, u->length);
        }
        mth_take(m);
        return true;
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
        else return false;
        *out = mth_val(mth_type_number(), v);
        mth_take(m);
        return true;
    }
    case LXB_CSS_SYNTAX_TOKEN_L_PARENTHESIS:
        /* §10.1: "Parentheses and nesting additional calc() functions are equivalent" — which is why one
           production serves both and neither needs its own type rule. */
        if (m->depth >= MTH_NESTING_MAX) return false;
        m->depth++;
        mth_take(m);
        if (!mth_sum(m, out)) { m->depth--; return false; }
        mth_skip_ws(m);
        t = mth_peek(m);
        if (t == NULL || t->type != LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS) { m->depth--; return false; }
        mth_take(m);
        m->depth--;
        return true;
    case LXB_CSS_SYNTAX_TOKEN_FUNCTION:
        return mth_function(m, out);
    default:
        return false;
    }
}

/* ---- §10.10.1's operator reductions ---------------------------------------------------------------------- */

/* §10.10.1's Sum: "For each set of root's children that are numeric values with identical units, remove those
   children and replace them with a single numeric value containing the sum of the removed nodes." Once the
   TYPES have added, both operands are the same type and both terms are addable in that type's canonical unit —
   which is why the type check is the whole of the validity question and the arithmetic below has none. */
static bool mth_add(const MthVal *a, const MthVal *b, MthVal *out)
{
    CssMathType ty;

    if (!mth_type_add(&a->type, &b->type, &ty)) return false;
    out->type = ty;
    out->num = css_px_add(a->num, b->num);
    out->pct = a->pct + b->pct;
    out->pct_term = a->pct_term || b->pct_term;
    out->blocked = mth_worse(a->blocked, b->blocked);
    return true;
}

/* §10.10.1's Negate over an already-simplified Sum: "If root's child is a numeric value, return an equivalent
   numeric value, but with the value negated (0 - value)", applied to each surviving term. */
static MthVal mth_negate(const MthVal *a)
{
    MthVal out = *a;

    out.num = css_px_sub(css_px(0.0), a->num);
    out.pct = 0.0 - a->pct;
    return out;
}

/* §10.10.1's Product. The two arms that reduce are that section's own: its last Product rule ("If root
   contains only numeric values and/or Invert nodes containing numeric values, and multiplying the types ...")
   and the rule above it ("If root contains only two children, one of which is a number ... and the other of
   which is a Sum whose children are all numeric values, multiply all of the Sum's children by the number").
   Anything else leaves a live Product node, which is `blocked`. */
static bool mth_multiply(const MthVal *a, const MthVal *b, MthVal *out)
{
    CssMathType ty;

    if (!mth_type_mul(&a->type, &b->type, &ty)) return false;
    out->type = ty;
    out->blocked = mth_worse(a->blocked, b->blocked);
    if (!a->pct_term && !b->pct_term) {
        out->num = css_px_mul(a->num, b->num);
        out->pct = 0.0;
        out->pct_term = false;
        return true;
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
            return true;
        }
    }
    out->num = css_px(0.0);
    out->pct = 0.0;
    out->pct_term = a->pct_term || b->pct_term;
    mth_block(out, MTH_UNRESOLVED_TREE);
    return true;
}

/* §10.8's `/`, as §10.9 states it: "let left type be the result of finding the types of its left argument, and
   right type be the result of finding the types of its right argument AND THEN INVERTING IT. The
   sub-expression's type is the result of multiplying the left type and right type." §10.10.1's Invert reduces
   only "if root's child is a number (not a percentage or dimension)", so a divisor carrying a percentage term
   leaves a live Invert node under the Product — which the multiply above then reports as blocked. */
static bool mth_divide(const MthVal *a, const MthVal *b, MthVal *out)
{
    MthVal inv;

    /* THE RECIPROCAL IS TAKEN RAW AND NOT THROUGH `css_px_div`, which asserts a non-zero divisor. That assert
       is right for its own callers — Intersection Observer §3.2.10 step 12 states its non-zero branch itself —
       and wrong here, because §10.9.2 DEFINES division by zero: "Dividing a value by zero produces either +∞
       or −∞, according to the standard sign rules", which is IEEE-754's answer and is what `calc(1 / 0)` must
       give rather than a crash. The environment facts still ride the operand, so the union is unaffected. */
    inv = *b;
    inv.type = mth_type_inv(&b->type);
    if (b->pct_term) {
        inv.num = css_px(0.0);
        inv.pct = 0.0;
        mth_block(&inv, MTH_UNRESOLVED_TREE);
    } else {
        inv.num = b->num;
        inv.num.px = 1.0 / b->num.px;
        inv.pct = 0.0;
        inv.pct_term = false;
    }
    return mth_multiply(a, &inv, out);
}

/* §10.8's `<calc-product> = <calc-value> [ [ '*' | / ] <calc-value> ]*`. */
static bool mth_product(Mth *m, MthVal *out)
{
    MthVal lhs;

    if (!mth_value(m, &lhs)) return false;
    for (;;) {
        lxb_css_syntax_token_t *t;
        MthVal rhs, res;
        bool div;

        mth_skip_ws(m);
        t = mth_peek(m);
        if (mth_is_delim(t, '*')) div = false;
        else if (mth_is_delim(t, '/')) div = true;
        else break;
        mth_take(m);
        if (!mth_value(m, &rhs)) return false;
        if (!(div ? mth_divide(&lhs, &rhs, &res) : mth_multiply(&lhs, &rhs, &res))) return false;
        lhs = res;
    }
    *out = lhs;
    return true;
}

/* §10.8's `<calc-sum> = <calc-product> [ [ '+' | '-' ] <calc-product> ]*`, with that section's whitespace rule:
   "whitespace is required on both sides of the + and - operators". */
static bool mth_sum(Mth *m, MthVal *out)
{
    MthVal lhs;

    if (!mth_product(m, &lhs)) return false;
    for (;;) {
        lxb_css_syntax_token_t *t;
        MthVal rhs, neg, res;
        bool ws_before, minus;

        ws_before = mth_skip_ws(m);
        t = mth_peek(m);
        if (mth_is_delim(t, '+')) minus = false;
        else if (mth_is_delim(t, '-')) minus = true;
        else break;
        /* §10.8's whitespace rule, over the state BEFORE the operator is consumed and the state after — the
           `mth_take` between them is what clears the flag, so the two reads are of two different positions and
           both are the positions the rule names. */
        mth_take(m);
        if (!ws_before || !mth_skip_ws(m)) return false;
        if (!mth_product(m, &rhs)) return false;
        if (minus) { neg = mth_negate(&rhs); rhs = neg; }
        if (!mth_add(&lhs, &rhs, &res)) return false;
        lhs = res;
    }
    *out = lhs;
    return true;
}

/* ---- §10.2 to §10.6's functions ------------------------------------------------------------------------- */

/* §10.5.1's note: "NaN is 'infectious' in every function, forcing the function to return NaN if any argument
   calculation is NaN." Asked before every ordering and every libm call, because C's own library does not have
   that property (`Math.hypot(Infinity, NaN)` is the example the note itself gives). */
static bool mth_is_nan(const MthVal *v) { return v->num.px != v->num.px; }

/* A function's result value, carrying the environment facts of every operand — the union, exactly as a sum
   does, and for the same reason core/css/css_length.h gives for `css_px_max`: the operand that lost at this
   viewport is the one that wins at another, so a `min(50vw, 400px)` is a function of the viewport whichever
   arm the modelled number takes. */
static CssPx mth_join(CssPx a, CssPx b, double n)
{
    CssPx out = css_px_mul(a, b);   /* the union of the two fact sets, with the realm assertion */

    out.px = n;
    return out;
}

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

/* Consume one comma-separated argument of a math function. `*last` reports that the closing `)` followed
   instead of a comma; a caller that wanted more arguments then refuses. */
static bool mth_argument(Mth *m, MthVal *out, bool *last)
{
    lxb_css_syntax_token_t *t;

    if (!mth_sum(m, out)) return false;
    mth_skip_ws(m);
    t = mth_peek(m);
    if (t == NULL) return false;
    if (t->type == LXB_CSS_SYNTAX_TOKEN_COMMA)          { mth_take(m); *last = false; return true; }
    if (t->type == LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS)  { mth_take(m); *last = true;  return true; }
    return false;
}

/* §10.2's min()/max() and §10.5's hypot(), which share one shape: "one or more comma-separated calculations",
   a type that is "the result of adding the types of its comma-separated calculations", and a fold over the
   values. `kind` is 0 for min, 1 for max, 2 for hypot. */
static bool mth_variadic(Mth *m, int kind, MthVal *out)
{
    MthVal acc = mth_val(mth_type_number(), 0.0);
    unsigned n = 0;
    bool last = false, saw_nan = false, saw_inf = false, any_pct = false;
    double fold = 0.0;
    CssPx facts = css_px(0.0);

    for (;;) {
        MthVal a;

        if (!mth_argument(m, &a, &last)) return false;
        if (n == 0) {
            acc = a;
        } else {
            /* §10.9: the type is "the result of adding the types of its comma-separated calculations". Only the
               TYPES add here — the VALUE is a fold and not a sum, which is why this is not `mth_add`. */
            CssMathType ty;

            if (!mth_type_add(&acc.type, &a.type, &ty)) return false;
            acc.type = ty;
        }
        if (a.pct_term) any_pct = true;
        acc.blocked = mth_worse(acc.blocked, a.blocked);
        if (m->res != NULL) {
            facts = mth_join(facts, a.num, 0.0);
            if (mth_is_nan(&a)) saw_nan = true;
            else if (isinf(a.num.px)) saw_inf = true;
            if (kind == 2) fold += a.num.px * a.num.px;
            else if (n == 0) fold = a.num.px;
            /* §10.5.1's note makes NaN INFECTIOUS in every function, so once one has been seen the result is
               decided and NO ORDERING IS ASKED. That is not an optimisation: `mth_less` asserts it is never
               handed a NaN, because a comparison that quietly answered false would be JS's `Math.min` behavior,
               and `min(1px, calc(NaN * 1px))` is a page's own value rather than a broken invariant. */
            else if (saw_nan) { /* the fold below is superseded by §10.5.1's NaN */ }
            else if (kind == 0) fold = mth_less(a.num.px, fold) ? a.num.px : fold;
            else fold = mth_less(fold, a.num.px) ? a.num.px : fold;
        }
        n++;
        if (last) break;
    }
    DCHECK(n > 0, "a variadic math function returned with no arguments read — §10.8 gives min(), max() and "
                  "hypot() a `<calc-sum>#` argument, whose `#` multiplier has no zero-length arm, and the loop "
                  "above cannot leave without having read one");
    *out = acc;
    if (m->res == NULL) return true;
    /* §10.10.1: a percentage "will usually block simplification of the node, since it needs to be resolved
       against another value using information not currently available ... This includes operations such as
       "min", since percentages might resolve against a negative basis, and thus end up with an opposite
       comparative relationship than the raw percentage value would seem to indicate." min()/max() with ONE
       argument are the exception the same section states — "If root has only one child, return the child" — so
       `min(50%)` is `50%` and needs no comparison at all. hypot() has no such arm: with one argument it "gives
       the ABSOLUTE VALUE of its input" (§10.5), which is a sign question a percentage cannot answer either. */
    if (any_pct) {
        if (n > 1 || kind == 2) mth_block(out, MTH_UNRESOLVED_TREE);
        return true;
    }
    out->pct = 0.0;
    out->pct_term = false;
    if (kind == 2) {
        /* §10.5.1: "In hypot(A, …), if any of the inputs are infinite, the result is +∞" — after the NaN rule,
           which §10.5.1's note makes strictly stronger ("Math.hypot(Infinity, NaN) will return Infinity" is
           named there as the JS behavior CSS does not share). */
        fold = saw_nan ? NAN : (saw_inf ? INFINITY : sqrt(fold));
    } else if (saw_nan) {
        fold = NAN;
    }
    out->num = facts;
    out->num.px = fold;
    return true;
}

/* §10.2's clamp(MIN, VAL, MAX), whose min/max arms may each be the keyword `none`: §10.8 gives it
   `clamp( [ <calc-sum> | none ], <calc-sum>, [ <calc-sum> | none ] )`. */
static bool mth_clamp(Mth *m, MthVal *out)
{
    MthVal arg[3];
    bool present[3] = { true, true, true };
    bool last = false;
    unsigned i;
    CssMathType ty = mth_type_number();
    bool have_ty = false;

    for (i = 0; i < 3; i++) arg[i] = mth_val(mth_type_number(), 0.0);

    for (i = 0; i < 3; i++) {
        lxb_css_syntax_token_t *t;

        mth_skip_ws(m);
        t = mth_peek(m);
        if (t == NULL) return false;
        if ((i == 0 || i == 2) && t->type == LXB_CSS_SYNTAX_TOKEN_IDENT &&
            mth_name_is((const char *)lxb_css_syntax_token_ident(t)->data,
                        lxb_css_syntax_token_ident(t)->length, "none")) {
            mth_take(m);
            present[i] = false;
            mth_skip_ws(m);
            t = mth_peek(m);
            if (t == NULL) return false;
            if (i < 2) {
                if (t->type != LXB_CSS_SYNTAX_TOKEN_COMMA) return false;
                mth_take(m);
            } else {
                if (t->type != LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS) return false;
                mth_take(m);
            }
            continue;
        }
        if (!mth_argument(m, &arg[i], &last)) return false;
        if (last != (i == 2)) return false;
    }
    /* §10.9: clamp()'s type is "the result of adding the types of its comma-separated calculations". `none` is
       a keyword and not a calculation, so it contributes none. */
    for (i = 0; i < 3; i++) {
        CssMathType sum;

        if (!present[i]) continue;
        if (!have_ty) { ty = arg[i].type; have_ty = true; continue; }
        if (!mth_type_add(&ty, &arg[i].type, &sum)) return false;
        ty = sum;
    }
    DCHECK(have_ty, "clamp()'s central calculation is absent — §10.8's grammar makes only the FIRST and THIRD "
                    "arms admit `none`, and the loop above refuses the keyword in the second position, so a "
                    "clamp with no calculation at all could not have parsed");
    *out = arg[1];
    out->type = ty;
    out->blocked = mth_worse(arg[1].blocked, mth_worse(present[0] ? arg[0].blocked : MTH_RESOLVED,
                                                       present[2] ? arg[2].blocked : MTH_RESOLVED));
    if (m->res == NULL) return true;
    /* §10.2: "clamp(MIN, VAL, MAX) ... represents exactly the same value as max(MIN, min(VAL, MAX))", so the
       minimum wins over the maximum when the two are in the wrong order — which §10.2 states as a rule and not
       as a consequence ("clamp(100px, ..., 50px) will resolve to 100px, exceeding its stated 'max' value"). */
    if (arg[1].pct_term || (present[0] && arg[0].pct_term) || (present[2] && arg[2].pct_term)) {
        if (present[0] || present[2]) mth_block(out, MTH_UNRESOLVED_TREE);
        return true;
    }
    {
        double v = arg[1].num.px;
        CssPx facts = arg[1].num;

        if (present[2]) {
            facts = mth_join(facts, arg[2].num, 0.0);
            if (mth_is_nan(&arg[2]) || v != v) v = NAN;
            else if (mth_less(arg[2].num.px, v)) v = arg[2].num.px;
        }
        if (present[0]) {
            facts = mth_join(facts, arg[0].num, 0.0);
            if (mth_is_nan(&arg[0]) || v != v) v = NAN;
            else if (mth_less(v, arg[0].num.px)) v = arg[0].num.px;
        }
        out->num = facts;
        out->num.px = v;
        out->pct = 0.0;
        out->pct_term = false;
    }
    return true;
}

/* §10.3's round(<rounding-strategy>?, A, B?). */
static bool mth_round(Mth *m, MthVal *out)
{
    MthRounding how = MTH_ROUND_NEAREST;
    MthVal a = mth_val(mth_type_number(), 0.0);
    MthVal b = mth_val(mth_type_number(), 0.0);
    bool have_b = false, last = false;
    lxb_css_syntax_token_t *t;
    CssMathType ty;

    mth_skip_ws(m);
    t = mth_peek(m);
    if (t == NULL) return false;
    if (t->type == LXB_CSS_SYNTAX_TOKEN_IDENT &&
        mth_rounding_of((const char *)lxb_css_syntax_token_ident(t)->data,
                        lxb_css_syntax_token_ident(t)->length, &how)) {
        mth_take(m);
        mth_skip_ws(m);
        t = mth_peek(m);
        if (t == NULL || t->type != LXB_CSS_SYNTAX_TOKEN_COMMA) return false;
        mth_take(m);
    }
    if (!mth_argument(m, &a, &last)) return false;
    if (!last) {
        if (!mth_argument(m, &b, &last)) return false;
        if (!last) return false;
        have_b = true;
    }
    /* §10.3: "If <rounding-strategy> is line-width, the type of A must match <length>." */
    if (how == MTH_ROUND_LINE_WIDTH && !mth_only(&a.type, CSS_MATH_LENGTH)) return false;
    /* §10.3: "If the type of A matches <number>, then B may be omitted, and defaults to 1. If
       <rounding-strategy> is line-width, B may also be omitted ... In all other cases, omitting B is invalid." */
    if (!have_b && how != MTH_ROUND_LINE_WIDTH && !mth_no_entries(&a.type)) return false;
    ty = a.type;
    if (have_b) {
        CssMathType sum;

        if (!mth_type_add(&a.type, &b.type, &sum)) return false;
        ty = sum;
    }
    *out = a;
    out->type = ty;
    out->blocked = mth_worse(a.blocked, have_b ? b.blocked : MTH_RESOLVED);
    if (m->res == NULL) return true;
    if (a.pct_term || (have_b && b.pct_term)) {
        mth_block(out, MTH_UNRESOLVED_TREE);
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
        out->num = have_b ? mth_join(a.num, b.num, 0.0) : a.num;
        out->num.px = mth_round_value(how, a.num.px, have_b ? b.num.px : 1.0);
        if (how == MTH_ROUND_LINE_WIDTH) out->num = css_length_snap_line_width(m->res->realm, out->num);
    }
    out->pct = 0.0;
    out->pct_term = false;
    return true;
}

/* Read exactly `want` comma-separated arguments and the closing `)`. */
static bool mth_fixed_args(Mth *m, unsigned want, MthVal *arg)
{
    unsigned i;

    for (i = 0; i < want; i++) {
        bool last = false;

        if (!mth_argument(m, &arg[i], &last)) return false;
        if (last != (i + 1 == want)) return false;
    }
    return true;
}

/* §10.10: "The internal representation is an operator node with the same name as the function, whose children
   are the result of parsing a calculation from each of the function's arguments." Dispatch on the name, with
   §10.9's own type table as the return type of each. */
static bool mth_function(Mth *m, MthVal *out)
{
    lxb_css_syntax_token_t *t = mth_peek(m);
    const lxb_css_syntax_token_string_t *s;
    char name[16];
    size_t nlen;
    MthVal arg[2];
    bool ok = false;

    arg[0] = mth_val(mth_type_number(), 0.0);
    arg[1] = mth_val(mth_type_number(), 0.0);
    DCHECK(t != NULL && t->type == LXB_CSS_SYNTAX_TOKEN_FUNCTION,
           "the math-function production was entered with the cursor off a FUNCTION token — §10.8's grammar "
           "reaches a function only through `<calc-value>`'s own switch, which has just tested for one");
    s = lxb_css_syntax_token_function(t);
    if (s->length >= sizeof name) return false;   /* longer than any of §10.8's twenty-one names */
    /* The name is copied BEFORE the token is consumed: lexbor keeps a token's cooked string only until the
       next token is requested, and every arm below requests one. */
    nlen = s->length;
    memcpy(name, s->data, nlen);
    if (m->depth >= MTH_NESTING_MAX) return false;
    m->depth++;
    mth_take(m);

    if (mth_name_is(name, nlen, "calc")) {
        /* §10.9: calc()'s type is "the type of its contained calculation". */
        bool last = false;

        ok = mth_argument(m, out, &last) && last;
    } else if (mth_name_is(name, nlen, "min")) {
        ok = mth_variadic(m, 0, out);
    } else if (mth_name_is(name, nlen, "max")) {
        ok = mth_variadic(m, 1, out);
    } else if (mth_name_is(name, nlen, "hypot")) {
        ok = mth_variadic(m, 2, out);
    } else if (mth_name_is(name, nlen, "clamp")) {
        ok = mth_clamp(m, out);
    } else if (mth_name_is(name, nlen, "round")) {
        ok = mth_round(m, out);
    } else if (mth_name_is(name, nlen, "mod") || mth_name_is(name, nlen, "rem")) {
        /* §10.3: "The argument calculations can resolve to any <number>, <dimension>, or <percentage>, but must
           have the same type ... the result will have the same type as the arguments." */
        CssMathType ty;

        if (!mth_fixed_args(m, 2, arg)) goto done;
        if (!mth_type_add(&arg[0].type, &arg[1].type, &ty)) goto done;
        *out = arg[0];
        out->type = ty;
        out->blocked = mth_worse(arg[0].blocked, arg[1].blocked);
        if (m->res != NULL) {
            if (arg[0].pct_term || arg[1].pct_term) { mth_block(out, MTH_UNRESOLVED_TREE); ok = true; goto done; }
            out->num = mth_join(arg[0].num, arg[1].num, 0.0);
            out->num.px = mth_name_is(name, nlen, "mod") ? mth_mod_value(arg[0].num.px, arg[1].num.px)
                                                         : mth_rem_value(arg[0].num.px, arg[1].num.px);
            out->pct = 0.0;
            out->pct_term = false;
        }
        ok = true;
    } else if (mth_name_is(name, nlen, "sin") || mth_name_is(name, nlen, "cos") ||
               mth_name_is(name, nlen, "tan")) {
        /* §10.4: sin/cos/tan "must resolve to either a <number> or an <angle> ... They all represent a
           <number>, with the return type made consistent with the input calculation's type." */
        bool is_angle;
        CssMathType ty = mth_type_number();

        if (!mth_fixed_args(m, 1, arg)) goto done;
        is_angle = mth_only(&arg[0].type, CSS_MATH_ANGLE);
        if (!is_angle && !mth_no_entries(&arg[0].type)) goto done;
        if (!mth_make_consistent(&ty, &arg[0].type)) goto done;
        *out = arg[0];
        out->type = ty;
        if (m->res != NULL) {
            double r;

            if (arg[0].pct_term) { mth_block(out, MTH_UNRESOLVED_TREE); ok = true; goto done; }
            r = mth_to_radians(&arg[0], is_angle);
            /* §10.4.1: "In sin(A), cos(A), or tan(A), if A is infinite, the result is NaN." C's `sin` already
               answers NaN there, and its NaN propagation is the same; the guard is written so the rule is
               visible where the reader looks for it rather than left to a library's conformance. */
            if (r != r || isinf(r)) out->num.px = NAN;
            else if (mth_name_is(name, nlen, "sin")) out->num.px = sin(r);
            else if (mth_name_is(name, nlen, "cos")) out->num.px = cos(r);
            else out->num.px = tan(r);
            out->pct = 0.0;
            out->pct_term = false;
        }
        ok = true;
    } else if (mth_name_is(name, nlen, "asin") || mth_name_is(name, nlen, "acos") ||
               mth_name_is(name, nlen, "atan")) {
        /* §10.4: the arc functions "contain a single calculation which must resolve to a <number>, and compute
           their corresponding function, interpreting their result as a number of radians, representing an
           <angle>". §10.9 gives all three the type «["angle" → 1]». */
        CssMathType ty = mth_type_of(CSS_MATH_ANGLE);

        if (!mth_fixed_args(m, 1, arg)) goto done;
        if (!mth_no_entries(&arg[0].type)) goto done;
        if (!mth_make_consistent(&ty, &arg[0].type)) goto done;
        *out = arg[0];
        out->type = ty;
        if (m->res != NULL) {
            double v = arg[0].num.px, r;

            if (arg[0].pct_term) { mth_block(out, MTH_UNRESOLVED_TREE); ok = true; goto done; }
            if (v != v) r = NAN;
            else if (mth_name_is(name, nlen, "asin")) r = asin(v);   /* §10.4.1: |A| > 1 is NaN, which C gives */
            else if (mth_name_is(name, nlen, "acos")) r = acos(v);
            else r = atan(v);                                        /* §10.4.1: ±∞ is ±90deg, which C gives */
            out->num.px = r * 180.0 / MTH_PI;   /* §7.1's canonical unit for an `<angle>` is `deg` */
            out->pct = 0.0;
            out->pct_term = false;
        }
        ok = true;
    } else if (mth_name_is(name, nlen, "atan2")) {
        /* §10.4: "A and B can resolve to any <number>, <dimension>, or <percentage>, but must have a consistent
           type" and the result is an `<angle>` "with the return type made consistent with the input
           calculation's type". §10.4.1's whole table of unusual argument combinations is C's own `atan2`. */
        CssMathType sum, ty = mth_type_of(CSS_MATH_ANGLE);

        if (!mth_fixed_args(m, 2, arg)) goto done;
        if (!mth_type_add(&arg[0].type, &arg[1].type, &sum)) goto done;
        if (!mth_make_consistent(&ty, &sum)) goto done;
        *out = arg[0];
        out->type = ty;
        out->blocked = mth_worse(arg[0].blocked, arg[1].blocked);
        if (m->res != NULL) {
            if (arg[0].pct_term || arg[1].pct_term) { mth_block(out, MTH_UNRESOLVED_TREE); ok = true; goto done; }
            out->num = mth_join(arg[0].num, arg[1].num, 0.0);
            out->num.px = (mth_is_nan(&arg[0]) || mth_is_nan(&arg[1]))
                            ? NAN : atan2(arg[0].num.px, arg[1].num.px) * 180.0 / MTH_PI;
            out->pct = 0.0;
            out->pct_term = false;
        }
        ok = true;
    } else if (mth_name_is(name, nlen, "pow") || mth_name_is(name, nlen, "log")) {
        /* §10.5: pow()'s two calculations and log()'s one-or-two "must resolve to <number>s", and the result is
           a `<number>` "with the return type made consistent with the input calculation's type". */
        CssMathType ty = mth_type_number();
        bool is_pow = mth_name_is(name, nlen, "pow");
        bool last = false, have_b = false;

        if (!mth_argument(m, &arg[0], &last)) goto done;
        if (!last) {
            if (!mth_argument(m, &arg[1], &last) || !last) goto done;
            have_b = true;
        }
        if (is_pow && !have_b) goto done;              /* §10.8: `<pow()> = pow( <calc-sum>, <calc-sum> )` */
        if (!mth_no_entries(&arg[0].type)) goto done;
        if (have_b && !mth_no_entries(&arg[1].type)) goto done;
        if (!mth_make_consistent(&ty, &arg[0].type)) goto done;
        if (have_b && !mth_make_consistent(&ty, &arg[1].type)) goto done;
        *out = arg[0];
        out->type = ty;
        out->blocked = mth_worse(arg[0].blocked, have_b ? arg[1].blocked : MTH_RESOLVED);
        if (m->res != NULL) {
            if (arg[0].pct_term || (have_b && arg[1].pct_term)) {
                mth_block(out, MTH_UNRESOLVED_TREE);
                ok = true;
                goto done;
            }
            out->num = have_b ? mth_join(arg[0].num, arg[1].num, 0.0) : arg[0].num;
            out->num.px = is_pow ? mth_pow_value(arg[0].num.px, arg[1].num.px)
                                 : mth_log_value(arg[0].num.px, have_b ? arg[1].num.px : 0.0, have_b);
            out->pct = 0.0;
            out->pct_term = false;
        }
        ok = true;
    } else if (mth_name_is(name, nlen, "sqrt") || mth_name_is(name, nlen, "exp")) {
        /* §10.5: both "contain a single calculation which must resolve to a <number>". §10.5.1's cases for both
           — `sqrt(+∞)` is `+∞`, `sqrt(0⁻)` is `0⁻`, `sqrt(A < 0)` is NaN, `exp(+∞)` is `+∞`, `exp(−∞)` is `0⁺`
           — are IEEE-754's own and are C's `sqrt` and `exp` unchanged. */
        CssMathType ty = mth_type_number();

        if (!mth_fixed_args(m, 1, arg)) goto done;
        if (!mth_no_entries(&arg[0].type)) goto done;
        if (!mth_make_consistent(&ty, &arg[0].type)) goto done;
        *out = arg[0];
        out->type = ty;
        if (m->res != NULL) {
            if (arg[0].pct_term) { mth_block(out, MTH_UNRESOLVED_TREE); ok = true; goto done; }
            out->num.px = mth_name_is(name, nlen, "sqrt") ? sqrt(arg[0].num.px) : exp(arg[0].num.px);
            out->pct = 0.0;
            out->pct_term = false;
        }
        ok = true;
    } else if (mth_name_is(name, nlen, "abs")) {
        /* §10.6: abs(A) "returns the absolute value of A, as the same type as the input", which §10.9 states
           again as "The type of its contained calculation". */
        if (!mth_fixed_args(m, 1, arg)) goto done;
        *out = arg[0];
        if (m->res != NULL) {
            /* §10.6's own note: "an expression like 10% might be positive or negative once it's resolved,
               depending on what value it's resolved against", so a surviving percentage blocks the sign. */
            if (arg[0].pct_term) mth_block(out, MTH_UNRESOLVED_TREE);
            else out->num.px = fabs(arg[0].num.px);
        }
        ok = true;
    } else if (mth_name_is(name, nlen, "sign")) {
        /* §10.6: sign(A) "returns -1 if A's numeric value is negative, +1 if ... positive, 0⁺ if ... 0⁺, and
           0⁻ if ... 0⁻. The return type is a <number>, made consistent with the input calculation's type." */
        CssMathType ty = mth_type_number();

        if (!mth_fixed_args(m, 1, arg)) goto done;
        if (!mth_make_consistent(&ty, &arg[0].type)) goto done;
        *out = arg[0];
        out->type = ty;
        if (m->res != NULL) {
            double v = arg[0].num.px;

            if (arg[0].pct_term) { mth_block(out, MTH_UNRESOLVED_TREE); ok = true; goto done; }
            if (v != v) out->num.px = NAN;
            else if (v < 0.0) out->num.px = -1.0;
            else if (v > 0.0) out->num.px = 1.0;
            else out->num.px = signbit(v) ? -0.0 : 0.0;
            out->pct = 0.0;
            out->pct_term = false;
        }
        ok = true;
    } else {
        DCHECK(!css_math_is_function(name, nlen),
               "one of css-values-4 §10.8's twenty-one functional notations reached the dispatch above and no "
               "arm claimed it. `css_math_is_function`'s table and this dispatch are ONE list read twice, so a "
               "name in the first and not the second is a function that types as valid and evaluates as "
               "nothing — the shape §Architecture calls a plausible datum");
    }

done:
    m->depth--;
    return ok;
}

/* ---- the three public questions -------------------------------------------------------------------------- */

/* Parse the whole of `text` as ONE math function, in §10.9's type-only mode when `res` is NULL.
   `type_gated` says whether §10.9's LAST RULE is part of the answer. It is for both questions that ask what a
   math function IS; it is not for the one that asks only whether the text is one at all, and the difference is
   exactly the SYNTAX/TYPE split §10 draws between §10.8 "Syntax" and §10.9 "Type Checking". The flag exists so
   that split is made in ONE walk: a second scan written to answer "is this one math function" would have to
   re-derive §10.8's nesting, its comma arities and its `<calc-value>` productions, and the day the two
   disagreed the caller would be told a value is not a math function by one and refused by the other. */
static bool mth_top(const char *text, size_t len, const CssMathResolver *res, CssMathProduction want,
                    bool type_gated, CssMathValue *out)
{
    Mth m = { NULL, res, CSS_MATH_HINT_NULL, 0, false };
    lxb_css_syntax_token_t *t;
    MthVal v = mth_val(mth_type_number(), 0.0);
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
    /* §10.9's last rule: "A math function resolves to <number>, <length>, ... according to which of those
       productions its type matches ... If it can't match any of these, the math function is invalid." */
    if (type_gated && !mth_type_matches(&v.type, want)) goto done;
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
    else if (v.blocked == MTH_UNRESOLVED_TREE)
        DFAIL("a VALID math function simplified to a residue this engine cannot represent. css-values-4 "
              "§10.10.1 'Simplification' reduces a calculation tree eagerly and leaves a LIVE NODE where it "
              "cannot — a Product of two operands that both still carry a `<percentage>` (`calc(50% * 1em / "
              "1px)`), or a Min/Max/clamp whose arguments cannot be compared because \"percentages might "
              "resolve against a negative basis, and thus end up with an opposite comparative relationship "
              "than the raw percentage value would seem to indicate\". core/css/css_math.h states that this "
              "component simplifies as it parses and therefore holds no tree, which is what makes those "
              "residues unrepresentable rather than merely unevaluated. BUILD §10.10's CALCULATION TREE as a "
              "real node graph here — Sum, Product, Negate, Invert plus one node per math function, owned and "
              "freed by this file — and make §10.10.1 a walk over it, so an unreduced node survives to "
              "§10.11's computed-value simplification with the information it was waiting for");
    out->type = v.type;
    out->num = v.num;
    out->pct = v.pct;
    out->pct_term = v.pct_term;
    /* §10.9.2's TOP-LEVEL CENSORING, the context-free half: "NaN does not escape a top-level calculation; it's
       censored into a zero value" and "Signed zeroes do not escape a top-level calculation; they're censored
       into the 'unsigned' zero." An INFINITY is left alone — §10.9.2 clamps it "to the minimum or maximum value
       allowed in the context, as defined in §10.12 Range Checking", and the context is the caller's property. */
    if (out->num.px != out->num.px) out->num.px = 0.0;
    if (out->num.px == 0.0) out->num.px = fabs(out->num.px);
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
    if (want == CSS_MATH_PROD_INTEGER) out->num.px = mth_round_value(MTH_ROUND_NEAREST, out->num.px, 1.0);

done:
    lxb_css_syntax_tokenizer_destroy(m.tkz);
    return ok;
}

bool css_math_matches(const char *text, size_t len, CssMathProduction want)
{
    return mth_top(text, len, NULL, want, true, NULL);
}

bool css_math_is_lone_function(const char *text, size_t len)
{
    /* `want` is unread on this path — `type_gated` is what decides whether it is consulted at all — and it is
       passed as `CSS_MATH_PROD_NUMBER` rather than left indeterminate because §10.9's own rounding step reads
       it, and a value chosen to be harmless is a value a later edit could make load-bearing without noticing.
       `res` is NULL, so that step is not reached either. */
    return mth_top(text, len, NULL, CSS_MATH_PROD_NUMBER, false, NULL);
}

bool css_math_eval(const char *text, size_t len, const CssMathResolver *res, CssMathProduction want,
                   CssMathValue *out)
{
    DCHECK(res != NULL && out != NULL,
           "a math function was evaluated with no resolver or nowhere to write the result — the TYPE question "
           "needs neither and is `css_math_matches`, so an absent one here is a caller that meant to ask that "
           "one instead");
    return mth_top(text, len, res, want, true, out);
}
