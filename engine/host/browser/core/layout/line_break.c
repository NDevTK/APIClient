/* [UAX14] Unicode Line Breaking Algorithm §6 "Line Breaking Algorithm", rules LB1..LB31, over the Line_Break
   property core/layout/line_break_class.h carries. See line_break.h for why this is a component of its own and
   why it takes the whole sequence rather than one character at a time.

   THE RULES ARE ORDERED AND THE ORDER IS THE ALGORITHM. UAX14 §6 states them as a sequence and every one of
   them is written as if the ones above it have already been applied — "Otherwise, do not break before ';', ',',
   or '.'" is LB15d's own first word, and it is only true because LB15c ran first. So `lb_decide` below is one
   straight-line chain in the annex's order, each rule returning the moment it matches, and LB31 "Break
   everywhere else" is the fallthrough at the bottom rather than a default anybody chose. Reordering two of
   these is a silent wrong answer, which is why each carries its rule number AND the annex's own regular
   expression: the number alone cannot be checked against the text.

   THERE IS NO PAIR TABLE, and that is not a shortcut. UAX14 §7 was "Pair Table-Based Implementation" and in the
   revision this file is written against (tr14-55, Unicode 17.0.0) the whole section reads "7 Deleted. Formerly
   was: Pair Table-Based Implementation." A table indexed by (class, class) cannot express LB25's
   `PO × OP IS NU`, LB14's `OP SP* ×`, LB30a's regional-indicator parity or LB15b's lookahead past eot, so the
   rules are the implementation and a pair table would be a lossy cache of some of them. */
#include <stdbool.h>
#include <stdlib.h>

#include "check.h"
#include "core/layout/line_break.h"

/* TWO VALUES THAT ARE NOT CLASSES, kept out of the enum deliberately: a rule can compare a class against them
   only by naming them, and neither can be produced by the table.
     LB_IGNORED — LB9 "Do not break a combining character sequence" removed this character from the rules
       entirely ("in subsequent rules, any CM or ZWJ characters affected by this rule are ignored").
     LB_NONE — there is no character here at all: sot before the first, eot after the last. */
#define LB_IGNORED 0xFEu
#define LB_NONE 0xFFu

/* ---- the property lookups ----------------------------------------------------------------------------- */

LineBreakClass line_break_class_of(uint32_t cp)
{
    size_t lo = 0, hi = LINE_BREAK_CLASS_RUN_COUNT - 1;

    DCHECK(cp <= 0x10FFFF, "a value that is not a Unicode code point was handed to [UAX14]'s Line_Break "
                           "property. The generated table partitions 0..10FFFF and nothing above it, so this "
                           "is an encoder that produced a scalar out of range and not a character");
    /* The runs partition the code point space, so the search is for the FIRST run whose inclusive end is at or
       above `cp` — there is always one and the loop cannot fall out. */
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;

        if (line_break_class_run_end[mid] < cp) lo = mid + 1;
        else hi = mid;
    }
    DCHECK(line_break_class_run_end[lo] >= cp &&
               (lo == 0 || line_break_class_run_end[lo - 1] < cp),
           "the generated Line_Break run table is not sorted and disjoint, so a binary search over it landed on "
           "a run that does not contain the code point. engine/gen_line_break.mjs emits them by walking the "
           "code point space in order, so a table that fails this was edited by hand");
    DCHECK(line_break_class_run[lo] < LINE_BREAK_CLASS_COUNT,
           "the generated Line_Break run table holds a value that is not one of the classes its own enum "
           "declares. Every rule below reads this as a LineBreakClass and compares it against named constants, "
           "so an out-of-range value would fall through the whole chain to LB31 and read as a break nobody "
           "wrote");
    return (LineBreakClass)line_break_class_run[lo];
}

static bool lb_in_set(const LineBreakRange *set, size_t count, uint32_t cp)
{
    size_t lo = 0, hi = count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;

        if (cp < set[mid].lo) hi = mid;
        else if (cp > set[mid].hi) lo = mid + 1;
        else return true;
    }
    return false;
}

/* LB15a's and LB19's `[\p{Pi}&QU]` and `[\p{Pf}&QU]`, and LB19a's and LB30's `$EastAsian`, and LB30b's
   `[\p{Extended_Pictographic}&\p{Cn}]` — each generated already intersected with the class its rule asks
   about, so this file never carries a second Unicode property in general. */
static bool lb_is_qu_initial(uint32_t cp)
{
    return lb_in_set(line_break_qu_initial, LINE_BREAK_QU_INITIAL_COUNT, cp);
}

static bool lb_is_qu_final(uint32_t cp)
{
    return lb_in_set(line_break_qu_final, LINE_BREAK_QU_FINAL_COUNT, cp);
}

static bool lb_is_east_asian(uint32_t cp)
{
    return lb_in_set(line_break_east_asian, LINE_BREAK_EAST_ASIAN_COUNT, cp);
}

static bool lb_is_unassigned_pictographic(uint32_t cp)
{
    return lb_in_set(line_break_unassigned_pictographic, LINE_BREAK_UNASSIGNED_PICTOGRAPHIC_COUNT, cp);
}

/* ---- LB9 and LB10, which are a PASS and not a rule at a boundary --------------------------------------- */

/* ONE LEFT-TO-RIGHT PASS THAT ANSWERS EVERY BACKWARD QUESTION THE RULES ASK, and it is a pass rather than
   three loops because each of those questions has a RECURRENCE and a scan does not. Three rules are stated
   over an unbounded run to the left — LB8/LB14/LB15a/LB16/LB17's `SP*`, LB25's `NU ( SY | IS )*`, LB30a's
   `(RI RI)*` — and answering any of them by walking is QUADRATIC in the length of that run. That is not a
   theoretical cost here: this engine measures text a page chose, so `"1" + "/".repeat(100000)` inside a
   shrink-to-fit box is a document, and a scan would spend 10^10 comparisons on it with no bound anywhere to
   stop it. The recurrences are one line each and they are exact, so the walk is deleted rather than capped. */
typedef struct {
    /* The effective class of each character, or LB_IGNORED where LB9 removed it. */
    uint8_t *cls;
    /* `SP*` scanned leftwards: the index of the first character at or before this one whose class is not SP,
       or -1 when the whole prefix is spaces. */
    int32_t *nonspace;
    /* LB25's `NU ( SY | IS )*`: does the (possibly empty) run of SY and IS ending here sit on a NU? */
    uint8_t *numeric;
    /* LB30a: does the run of regional indicators ending here have ODD length? */
    uint8_t *odd_ri;
} LineBreakScan;

/* THE EFFECTIVE CLASS OF EVERY CHARACTER, with LB9's combining marks marked LB_IGNORED and LB10's applied.
     LB9 "Treat X (CM | ZWJ)* as if it were X. … where X is any line break class except BK, CR, LF, NL, SP, or
       ZW." So a CM or ZWJ takes its BASE's class and is then ignored by every later rule.
     LB10 "Treat any remaining CM or ZWJ as if it had the properties of U+0041 A LATIN CAPITAL LETTER A, that
       is, Line_Break=AL" — "this catches the case where a CM is the first character on the line or follows SP,
       BK, CR, LF, NL, or ZW."
   THE ORDER INSIDE A RUN OF MARKS IS THE SUBTLE PART and getting it wrong is invisible. In `SP CM CM` the
   FIRST mark is LB10's case (its base would be SP, which LB9 excludes) so it becomes a real AL character — and
   the SECOND mark's base is then that AL, which LB9 does NOT exclude, so it IS ignored. A pass that skipped
   left over the whole run of marks to find SP would call both of them AL and hand the rules a character that
   is not there. This carries the last non-ignored character, which is what makes the two marks differ.
   AN IGNORED MARK INHERITS ITS BASE'S THREE ANSWERS rather than being left undefined, which is LB9's own
   sentence ("treat it as if it has the line breaking class of the base character in all of the following
   rules") and costs nothing: no rule reads them at an ignored index, and a hole there would be a value whose
   wrongness only a future rule would discover. */
static void lb_scan(const uint32_t *cps, size_t count, LineBreakScan *s)
{
    ptrdiff_t last = -1;
    size_t i;

    for (i = 0; i < count; i++) {
        LineBreakClass raw = line_break_class_of(cps[i]);
        uint8_t c;

        if (raw == LB_CLASS_CM || raw == LB_CLASS_ZWJ) {
            uint8_t base = last >= 0 ? s->cls[last] : LB_NONE;
            bool absorbs = base != LB_NONE && base != LB_CLASS_BK && base != LB_CLASS_CR &&
                           base != LB_CLASS_LF && base != LB_CLASS_NL && base != LB_CLASS_SP &&
                           base != LB_CLASS_ZW;

            if (absorbs) {
                s->cls[i] = LB_IGNORED;
                s->nonspace[i] = s->nonspace[last];
                s->numeric[i] = s->numeric[last];
                s->odd_ri[i] = s->odd_ri[last];
                continue;
            }
            c = LB_CLASS_AL;   /* LB10 */
        } else {
            c = (uint8_t)raw;
        }
        s->cls[i] = c;
        s->nonspace[i] = c == LB_CLASS_SP ? (last >= 0 ? s->nonspace[last] : -1) : (int32_t)i;
        s->numeric[i] = c == LB_CLASS_NU ? 1u
                        : ((c == LB_CLASS_SY || c == LB_CLASS_IS) && last >= 0 ? s->numeric[last] : 0u);
        s->odd_ri[i] = c != LB_CLASS_RI ? 0u
                       : (last >= 0 && s->cls[last] == LB_CLASS_RI ? (uint8_t)!s->odd_ri[last] : 1u);
        last = (ptrdiff_t)i;
    }
}

/* ---- the neighbours the rules are stated over ---------------------------------------------------------- */

/* The nearest character to the LEFT that LB9 did not ignore, or -1 for sot. `cls[0]` is never LB_IGNORED (at
   the first character LB9's X does not exist, so LB10 applies), which is what terminates the walk. IT IS THE
   ONE BACKWARD WALK THAT SURVIVES, and it is bounded by the length of a COMBINING CHARACTER SEQUENCE rather
   than by the length of the text — the walk above already collapsed the three unbounded ones. */
static ptrdiff_t lb_prev(const uint8_t *cls, ptrdiff_t i)
{
    ptrdiff_t j = i - 1;

    while (j >= 0 && cls[j] == LB_IGNORED) j--;
    return j;
}

/* The nearest to the RIGHT, or `count` for eot. */
static size_t lb_next(const uint8_t *cls, size_t count, size_t i)
{
    size_t j = i + 1;

    while (j < count && cls[j] == LB_IGNORED) j++;
    return j;
}

/* LB28a's `(AK | [◌] | AS)`, where "the class [◌] contains the single character U+25CC DOTTED CIRCLE". */
static bool lb_is_ak_like(const uint32_t *cps, const uint8_t *cls, ptrdiff_t i)
{
    if (i < 0) return false;
    return cls[i] == LB_CLASS_AK || cls[i] == LB_CLASS_AS || cps[i] == LINE_BREAK_DOTTED_CIRCLE;
}

/* ---- the ordered rules --------------------------------------------------------------------------------- */

/* THE ACTION AT THE BOUNDARY IMMEDIATELY BEFORE `cps[i]`, for 0 < i < count. LB2's sot and LB3's eot are the
   caller's two ends and are not in this chain. */
static LineBreakAction lb_decide(const uint32_t *cps, const LineBreakScan *s, size_t count, size_t i)
{
    const uint8_t *cls = s->cls;
    ptrdiff_t left = lb_prev(cls, (ptrdiff_t)i), k, j;
    uint8_t before, after;
    size_t n;

    DCHECK(left >= 0, "[UAX14]'s rules were asked about a boundary with no character to its left. LB2 \"Never "
                      "break at the start of text\" is the answer at position 0 and this chain is not reached "
                      "for it, so a missing left character here is a resolve pass that marked the FIRST "
                      "character LB_IGNORED — which LB10 makes impossible");
    before = cls[left];
    after = cls[i];

    /* LB4 `BK !` — always break after hard line breaks. */
    if (before == LB_CLASS_BK) return LINE_BREAK_MANDATORY;
    /* LB5 `CR × LF`, `CR !`, `LF !`, `NL !`. */
    if (before == LB_CLASS_CR && after == LB_CLASS_LF) return LINE_BREAK_PROHIBITED;
    if (before == LB_CLASS_CR || before == LB_CLASS_LF || before == LB_CLASS_NL) return LINE_BREAK_MANDATORY;
    /* LB6 `× ( BK | CR | LF | NL )` — do not break BEFORE a hard line break. */
    if (after == LB_CLASS_BK || after == LB_CLASS_CR || after == LB_CLASS_LF || after == LB_CLASS_NL)
        return LINE_BREAK_PROHIBITED;
    /* LB7 `× SP`, `× ZW`. */
    if (after == LB_CLASS_SP || after == LB_CLASS_ZW) return LINE_BREAK_PROHIBITED;
    /* LB8 `ZW SP* ÷` — break after a zero width space even if spaces intervene. `k` is that `SP*` scanned
       leftwards, and it is read again by LB14, LB15a, LB16 and LB17, which name the same run. */
    k = s->nonspace[left];
    if (k >= 0 && cls[k] == LB_CLASS_ZW) return LINE_BREAK_OPPORTUNITY;
    /* LB8a `ZWJ ×` — stated over the character LITERALLY before this one, because LB8a is applied before LB9
       absorbs that character into its base and a rule cannot see a resolution that has not happened yet. */
    if (line_break_class_of(cps[i - 1]) == LB_CLASS_ZWJ) return LINE_BREAK_PROHIBITED;
    /* LB9 — this character is inside a combining character sequence, so there is no boundary here. Every rule
       above fell through rather than being skipped: an ignored mark's base is by construction none of BK, CR,
       LF, NL, SP or ZW, which is exactly what LB4, LB5 and LB8 test on the left. */
    if (after == LB_IGNORED) {
        DCHECK(before != LB_CLASS_BK && before != LB_CLASS_CR && before != LB_CLASS_LF &&
                   before != LB_CLASS_NL && before != LB_CLASS_SP && before != LB_CLASS_ZW,
               "LB9 ignored a combining mark whose base is one of the six classes LB9 excludes from being a "
               "base. The resolve pass and this rule disagree about which characters LB9 absorbs, and the "
               "rules above this line were skipped on the strength of the agreement");
        return LINE_BREAK_PROHIBITED;
    }
    /* LB10 is not here: the resolve pass has already given a surviving CM or ZWJ the class AL. */
    /* LB11 `× WJ`, `WJ ×`. */
    if (after == LB_CLASS_WJ || before == LB_CLASS_WJ) return LINE_BREAK_PROHIBITED;
    /* LB12 `GL ×`. */
    if (before == LB_CLASS_GL) return LINE_BREAK_PROHIBITED;
    /* LB12a `[^SP BA HY HH] × GL`. */
    if (after == LB_CLASS_GL && before != LB_CLASS_SP && before != LB_CLASS_BA && before != LB_CLASS_HY &&
        before != LB_CLASS_HH)
        return LINE_BREAK_PROHIBITED;
    /* LB13 `× CL`, `× CP`, `× EX`, `× SY`. */
    if (after == LB_CLASS_CL || after == LB_CLASS_CP || after == LB_CLASS_EX || after == LB_CLASS_SY)
        return LINE_BREAK_PROHIBITED;
    /* LB14 `OP SP* ×` — do not break after an opening bracket, even after spaces. */
    if (k >= 0 && cls[k] == LB_CLASS_OP) return LINE_BREAK_PROHIBITED;
    /* LB15a `(sot | BK | CR | LF | NL | OP | QU | GL | SP | ZW) [\p{Pi}&QU] SP* ×`. */
    if (k >= 0 && lb_is_qu_initial(cps[k])) {
        j = lb_prev(cls, k);
        if (j < 0) return LINE_BREAK_PROHIBITED;   /* sot */
        if (cls[j] == LB_CLASS_BK || cls[j] == LB_CLASS_CR || cls[j] == LB_CLASS_LF ||
            cls[j] == LB_CLASS_NL || cls[j] == LB_CLASS_OP || cls[j] == LB_CLASS_QU ||
            cls[j] == LB_CLASS_GL || cls[j] == LB_CLASS_SP || cls[j] == LB_CLASS_ZW)
            return LINE_BREAK_PROHIBITED;
    }
    /* LB15b `× [\p{Pf}&QU] ( SP | GL | WJ | CL | QU | CP | EX | IS | SY | BK | CR | LF | NL | ZW | eot)`. */
    if (lb_is_qu_final(cps[i])) {
        n = lb_next(cls, count, i);
        if (n == count) return LINE_BREAK_PROHIBITED;   /* eot */
        if (cls[n] == LB_CLASS_SP || cls[n] == LB_CLASS_GL || cls[n] == LB_CLASS_WJ ||
            cls[n] == LB_CLASS_CL || cls[n] == LB_CLASS_QU || cls[n] == LB_CLASS_CP ||
            cls[n] == LB_CLASS_EX || cls[n] == LB_CLASS_IS || cls[n] == LB_CLASS_SY ||
            cls[n] == LB_CLASS_BK || cls[n] == LB_CLASS_CR || cls[n] == LB_CLASS_LF ||
            cls[n] == LB_CLASS_NL || cls[n] == LB_CLASS_ZW)
            return LINE_BREAK_PROHIBITED;
    }
    /* LB15c `SP ÷ IS NU` — "break before a decimal mark that follows a space, for instance, in 'subtract .5'".
       IT EXISTS ONLY TO OVERRIDE LB15d, which is why deleting it is not a smaller rule set but a wrong one:
       without it the `.` of `subtract .5` is `× IS` and the whole string is one unbreakable segment. */
    if (before == LB_CLASS_SP && after == LB_CLASS_IS) {
        n = lb_next(cls, count, i);
        if (n < count && cls[n] == LB_CLASS_NU) return LINE_BREAK_OPPORTUNITY;
    }
    /* LB15d `× IS` — "otherwise, do not break before ';', ',', or '.', even after spaces". */
    if (after == LB_CLASS_IS) return LINE_BREAK_PROHIBITED;
    /* LB16 `(CL | CP) SP* × NS`. */
    if (after == LB_CLASS_NS && k >= 0 && (cls[k] == LB_CLASS_CL || cls[k] == LB_CLASS_CP))
        return LINE_BREAK_PROHIBITED;
    /* LB17 `B2 SP* × B2`. */
    if (after == LB_CLASS_B2 && k >= 0 && cls[k] == LB_CLASS_B2) return LINE_BREAK_PROHIBITED;
    /* LB18 `SP ÷` — break after spaces. THE OPPORTUNITY IS AFTER THE SPACE AND NOT BEFORE IT, which is what
       LB7's `× SP` states on the other side, and it is why css-text-3 §4.1.2's trimming bills a collapsible
       space to the line that PRECEDES the break. */
    if (before == LB_CLASS_SP) return LINE_BREAK_OPPORTUNITY;
    /* LB19 `× [ QU - \p{Pi} ]`, `[ QU - \p{Pf} ] ×`. */
    if (after == LB_CLASS_QU && !lb_is_qu_initial(cps[i])) return LINE_BREAK_PROHIBITED;
    if (before == LB_CLASS_QU && !lb_is_qu_final(cps[left])) return LINE_BREAK_PROHIBITED;
    /* LB19a `[^$EastAsian] × QU`, `× QU ( [^$EastAsian] | eot )`, `QU × [^$EastAsian]`,
       `( sot | [^$EastAsian] ) QU ×`. */
    if (after == LB_CLASS_QU) {
        if (!lb_is_east_asian(cps[left])) return LINE_BREAK_PROHIBITED;
        n = lb_next(cls, count, i);
        if (n == count || !lb_is_east_asian(cps[n])) return LINE_BREAK_PROHIBITED;
    }
    if (before == LB_CLASS_QU) {
        if (!lb_is_east_asian(cps[i])) return LINE_BREAK_PROHIBITED;
        j = lb_prev(cls, left);
        if (j < 0 || !lb_is_east_asian(cps[j])) return LINE_BREAK_PROHIBITED;
    }
    /* LB20 `÷ CB`, `CB ÷` — the DEFAULT for an unresolved contingent break, which is what an engine with no
       out-of-band resolution has. UAX14: "conditional breaks should be resolved external to the line breaking
       rules. However, the default action is to treat unresolved CB as breaking before and after." */
    if (after == LB_CLASS_CB || before == LB_CLASS_CB) return LINE_BREAK_OPPORTUNITY;
    /* LB20a `( sot | BK | CR | LF | NL | SP | ZW | CB | GL ) ( HY | HH ) × ( AL | HL )`. */
    if ((after == LB_CLASS_AL || after == LB_CLASS_HL) &&
        (before == LB_CLASS_HY || before == LB_CLASS_HH)) {
        j = lb_prev(cls, left);
        if (j < 0) return LINE_BREAK_PROHIBITED;   /* sot */
        if (cls[j] == LB_CLASS_BK || cls[j] == LB_CLASS_CR || cls[j] == LB_CLASS_LF ||
            cls[j] == LB_CLASS_NL || cls[j] == LB_CLASS_SP || cls[j] == LB_CLASS_ZW ||
            cls[j] == LB_CLASS_CB || cls[j] == LB_CLASS_GL)
            return LINE_BREAK_PROHIBITED;
    }
    /* LB21 `× BA`, `× HH`, `× HY`, `× NS`, `BB ×`. */
    if (after == LB_CLASS_BA || after == LB_CLASS_HH || after == LB_CLASS_HY || after == LB_CLASS_NS ||
        before == LB_CLASS_BB)
        return LINE_BREAK_PROHIBITED;
    /* LB21a `HL (HY | HH) × [^HL]`. */
    if ((before == LB_CLASS_HY || before == LB_CLASS_HH) && after != LB_CLASS_HL) {
        j = lb_prev(cls, left);
        if (j >= 0 && cls[j] == LB_CLASS_HL) return LINE_BREAK_PROHIBITED;
    }
    /* LB21b `SY × HL`. */
    if (before == LB_CLASS_SY && after == LB_CLASS_HL) return LINE_BREAK_PROHIBITED;
    /* LB22 `× IN`. */
    if (after == LB_CLASS_IN) return LINE_BREAK_PROHIBITED;
    /* LB23 `(AL | HL) × NU`, `NU × (AL | HL)`. */
    if ((before == LB_CLASS_AL || before == LB_CLASS_HL) && after == LB_CLASS_NU)
        return LINE_BREAK_PROHIBITED;
    if (before == LB_CLASS_NU && (after == LB_CLASS_AL || after == LB_CLASS_HL))
        return LINE_BREAK_PROHIBITED;
    /* LB23a `PR × (ID | EB | EM)`, `(ID | EB | EM) × PO`. */
    if (before == LB_CLASS_PR && (after == LB_CLASS_ID || after == LB_CLASS_EB || after == LB_CLASS_EM))
        return LINE_BREAK_PROHIBITED;
    if ((before == LB_CLASS_ID || before == LB_CLASS_EB || before == LB_CLASS_EM) && after == LB_CLASS_PO)
        return LINE_BREAK_PROHIBITED;
    /* LB24 `(PR | PO) × (AL | HL)`, `(AL | HL) × (PR | PO)`. */
    if ((before == LB_CLASS_PR || before == LB_CLASS_PO) && (after == LB_CLASS_AL || after == LB_CLASS_HL))
        return LINE_BREAK_PROHIBITED;
    if ((before == LB_CLASS_AL || before == LB_CLASS_HL) && (after == LB_CLASS_PR || after == LB_CLASS_PO))
        return LINE_BREAK_PROHIBITED;
    /* LB25 "Do not break numbers", fifteen productions in the annex's own order. The four that begin
       `NU ( SY | IS )* CL` or `CP` come first because they are longer matches of the same left context as the
       two that follow, and the six `(PO | PR) × [OP [IS]] NU` productions are the ones that read forward past
       the boundary — which is the reason this component is handed the whole sequence. */
    if ((before == LB_CLASS_CL || before == LB_CLASS_CP) &&
        (after == LB_CLASS_PO || after == LB_CLASS_PR)) {
        j = lb_prev(cls, left);
        if (j >= 0 && s->numeric[j]) return LINE_BREAK_PROHIBITED;
    }
    if ((after == LB_CLASS_PO || after == LB_CLASS_PR) && s->numeric[left]) return LINE_BREAK_PROHIBITED;
    if (before == LB_CLASS_PO || before == LB_CLASS_PR) {
        if (after == LB_CLASS_NU) return LINE_BREAK_PROHIBITED;
        if (after == LB_CLASS_OP) {
            n = lb_next(cls, count, i);
            if (n < count && cls[n] == LB_CLASS_NU) return LINE_BREAK_PROHIBITED;
            if (n < count && cls[n] == LB_CLASS_IS) {
                size_t n2 = lb_next(cls, count, n);

                if (n2 < count && cls[n2] == LB_CLASS_NU) return LINE_BREAK_PROHIBITED;
            }
        }
    }
    /* `HY × NU`, `IS × NU`, `NU ( SY | IS )* × NU`. */
    if (after == LB_CLASS_NU && (before == LB_CLASS_HY || before == LB_CLASS_IS || s->numeric[left]))
        return LINE_BREAK_PROHIBITED;
    /* LB26 `JL × (JL | JV | H2 | H3)`, `(JV | H2) × (JV | JT)`, `(JT | H3) × JT`. */
    if (before == LB_CLASS_JL && (after == LB_CLASS_JL || after == LB_CLASS_JV || after == LB_CLASS_H2 ||
                                  after == LB_CLASS_H3))
        return LINE_BREAK_PROHIBITED;
    if ((before == LB_CLASS_JV || before == LB_CLASS_H2) && (after == LB_CLASS_JV || after == LB_CLASS_JT))
        return LINE_BREAK_PROHIBITED;
    if ((before == LB_CLASS_JT || before == LB_CLASS_H3) && after == LB_CLASS_JT)
        return LINE_BREAK_PROHIBITED;
    /* LB27 `(JL | JV | JT | H2 | H3) × PO`, `PR × (JL | JV | JT | H2 | H3)`. */
    if ((before == LB_CLASS_JL || before == LB_CLASS_JV || before == LB_CLASS_JT ||
         before == LB_CLASS_H2 || before == LB_CLASS_H3) && after == LB_CLASS_PO)
        return LINE_BREAK_PROHIBITED;
    if (before == LB_CLASS_PR && (after == LB_CLASS_JL || after == LB_CLASS_JV || after == LB_CLASS_JT ||
                                  after == LB_CLASS_H2 || after == LB_CLASS_H3))
        return LINE_BREAK_PROHIBITED;
    /* LB28 `(AL | HL) × (AL | HL)` — "do not break between alphabetics ('at')". This is the rule that makes a
       word a word, and it is the LAST of the joining rules rather than the first. */
    if ((before == LB_CLASS_AL || before == LB_CLASS_HL) && (after == LB_CLASS_AL || after == LB_CLASS_HL))
        return LINE_BREAK_PROHIBITED;
    /* LB28a, the orthographic syllables of Brahmic scripts:
       `AP × (AK | [◌] | AS)`, `(AK | [◌] | AS) × (VF | VI)`, `(AK | [◌] | AS) VI × (AK | [◌])`,
       `(AK | [◌] | AS) × (AK | [◌] | AS) VF`. */
    if (before == LB_CLASS_AP && lb_is_ak_like(cps, cls, (ptrdiff_t)i)) return LINE_BREAK_PROHIBITED;
    if (lb_is_ak_like(cps, cls, left) && (after == LB_CLASS_VF || after == LB_CLASS_VI))
        return LINE_BREAK_PROHIBITED;
    if (before == LB_CLASS_VI && lb_is_ak_like(cps, cls, lb_prev(cls, left)) &&
        (after == LB_CLASS_AK || cps[i] == LINE_BREAK_DOTTED_CIRCLE))
        return LINE_BREAK_PROHIBITED;
    if (lb_is_ak_like(cps, cls, left) && lb_is_ak_like(cps, cls, (ptrdiff_t)i)) {
        n = lb_next(cls, count, i);
        if (n < count && cls[n] == LB_CLASS_VF) return LINE_BREAK_PROHIBITED;
    }
    /* LB29 `IS × (AL | HL)` — "do not break between numeric punctuation and alphabetics ('e.g.')". */
    if (before == LB_CLASS_IS && (after == LB_CLASS_AL || after == LB_CLASS_HL))
        return LINE_BREAK_PROHIBITED;
    /* LB30 `(AL | HL | NU) × [OP-$EastAsian]`, `[CP-$EastAsian] × (AL | HL | NU)` — "person(s)". */
    if ((before == LB_CLASS_AL || before == LB_CLASS_HL || before == LB_CLASS_NU) &&
        after == LB_CLASS_OP && !lb_is_east_asian(cps[i]))
        return LINE_BREAK_PROHIBITED;
    if (before == LB_CLASS_CP && !lb_is_east_asian(cps[left]) &&
        (after == LB_CLASS_AL || after == LB_CLASS_HL || after == LB_CLASS_NU))
        return LINE_BREAK_PROHIBITED;
    /* LB30a `sot (RI RI)* RI × RI`, `[^RI] (RI RI)* RI × RI`. */
    if (before == LB_CLASS_RI && after == LB_CLASS_RI && s->odd_ri[left]) return LINE_BREAK_PROHIBITED;
    /* LB30b `EB × EM`, `[\p{Extended_Pictographic}&\p{Cn}] × EM`. The second production is stated over the
       ORIGINAL properties of a code point LB1 has already turned into AL, which is why the generated set is
       the intersection rather than either property on its own. */
    if (after == LB_CLASS_EM && (before == LB_CLASS_EB || lb_is_unassigned_pictographic(cps[left])))
        return LINE_BREAK_PROHIBITED;
    /* LB31 `ALL ÷`, `÷ ALL` — break everywhere else. */
    return LINE_BREAK_OPPORTUNITY;
}

void line_break_actions(const uint32_t *cps, size_t count, LineBreakAction *out)
{
    LineBreakScan s;
    unsigned char *block;
    size_t i;

    DCHECK(out != NULL, "[UAX14]'s line break actions were asked for with nowhere to write them");
    DCHECK(cps != NULL || count == 0, "a non-empty character sequence was described with a NULL pointer");
    /* LB2 `sot ×` — never break at the start of text. It is applied BEFORE LB3, so an EMPTY sequence's one
       position is this and not eot's mandatory break. */
    out[0] = LINE_BREAK_PROHIBITED;
    if (count == 0) return;
    DCHECK(count <= 0x7FFFFFFFu, "an inline formatting context of more than two billion characters was handed "
                                 "to [UAX14]. The scan below indexes it with an int32_t, which is a real "
                                 "ceiling and is asserted rather than wrapped");
    /* ONE ALLOCATION, carved rather than four: the int32 array is first so that its alignment is the block's
       and nothing has to be padded. */
    block = malloc(count * (sizeof(int32_t) + 3));
    CHECK(block != NULL, "out of memory scanning [UAX14]'s Line_Break classes for a text run. The allocation is "
                         "seven bytes per character of one inline formatting context, so a failure here is the "
                         "physical floor and not a layout that asked for something unreasonable");
    s.nonspace = (int32_t *)(void *)block;
    s.cls = block + count * sizeof(int32_t);
    s.numeric = s.cls + count;
    s.odd_ri = s.numeric + count;
    lb_scan(cps, count, &s);
    for (i = 1; i < count; i++) out[i] = lb_decide(cps, &s, count, i);
    /* LB3 `! eot` — always break at the end of text. */
    out[count] = LINE_BREAK_MANDATORY;
    free(block);
}
