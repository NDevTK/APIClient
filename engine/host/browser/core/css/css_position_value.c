/* CSS Values and Units 4 §8.3 "2D Positioning: the <position> type" — see css_position_value.h for why it is a
   component of its own. */
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_length.h"
#include "core/css/css_position_value.h"

#define POS_N(a) (sizeof(a) / sizeof((a)[0]))

/* A CSS keyword comparison over a span. CSS Syntax §4 makes an ident ASCII case-insensitive and a declaration
   lexbor's registry does not type arrives as the author's own bytes, so `LEFT` and `left` are one value.
   `kw` is the canonical lower-case spelling, which is what §8.3.2 serializes back out. */
static bool pos_word_is(const char *w, size_t n, const char *kw)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (kw[i] == '\0' || (char)tolower((unsigned char)w[i]) != kw[i]) return false;
    return kw[n] == '\0';
}

static const char *pos_keyword(const char *const *set, unsigned n, const char *w, size_t len)
{
    unsigned i;

    for (i = 0; i < n; i++)
        if (pos_word_is(w, len, set[i])) return set[i];
    return NULL;
}

/* §8.3's two keyword sets, split by AXIS. `center` is in BOTH, which is exactly what makes the `&&` arm
   decidable: every other keyword names its own axis, so a two-keyword value has at most one reading. */
static const char *const POS_H_KEYWORDS[] = { "left", "right", "center" };
static const char *const POS_V_KEYWORDS[] = { "top", "bottom", "center" };
/* The SIDE keywords — the subset that §8.3's fourth arm and css-backgrounds-3 §2.6's third arm allow an offset
   after. `center` is deliberately absent from both: `[ center | [ left | right ] <length-percentage>? ]` puts
   it in the arm with no offset at all, so `center 10px` is not a group and falls through to the positional
   arm, where the `10px` is the VERTICAL component. */
static const char *const POS_H_SIDES[] = { "left", "right" };
static const char *const POS_V_SIDES[] = { "top", "bottom" };

/* css-values-4 §5.6 "Mixing Percentages and Dimensions"'s `<length-percentage>` over a SPAN. The predicate
   core/css/css_length.h publishes takes a NUL-terminated value because every other caller has one; a component
   value here is a span into the declaration, and copying it is the whole of the adaptation. */
static bool pos_is_length_percentage(const char *w, size_t n)
{
    char *probe = malloc(n + 1);
    bool ok;

    CHECK(probe != NULL, "cssom: OOM testing a <position> component — a dropped one would make a valid "
                         "declaration invalid, which is a different position and not a missing one");
    memcpy(probe, w, n);
    probe[n] = '\0';
    ok = css_length_is_length_percentage(probe);
    free(probe);
    return ok;
}

/* ONE GROUP of css-backgrounds-3 §2.6's third arm — `[ center | [ left | right ] <length-percentage>? ]` and
   the same sentence one axis over. `want` is exactly how many components the caller is offering it, because
   the enumeration below drives the length rather than reading it: a group asked for 2 must take an offset and
   a group asked for 1 must not, which is what lets the four (length, length) combinations be tried in
   descending total order without any of them overlapping. */
static bool pos_group(const char *const *w, const size_t *wl, unsigned want, bool horizontal,
                      CssPositionValue *out)
{
    const char *kw;

    DCHECK(want == 1 || want == 2, "a <position> group was asked for a component count its grammar has no arm "
                                   "for — `center` is one component and `[left|right] <length-percentage>` is "
                                   "two, and there is no third length");
    if (want == 1) {
        kw = horizontal ? pos_keyword(POS_H_KEYWORDS, POS_N(POS_H_KEYWORDS), w[0], wl[0])
                        : pos_keyword(POS_V_KEYWORDS, POS_N(POS_V_KEYWORDS), w[0], wl[0]);
        if (!kw) return false;
        if (horizontal) out->h_kw = kw;
        else out->v_kw = kw;
        return true;
    }
    kw = horizontal ? pos_keyword(POS_H_SIDES, POS_N(POS_H_SIDES), w[0], wl[0])
                    : pos_keyword(POS_V_SIDES, POS_N(POS_V_SIDES), w[0], wl[0]);
    if (!kw || !pos_is_length_percentage(w[1], wl[1])) return false;
    if (horizontal) {
        out->h_kw = kw;
        out->h_off = w[1];
        out->h_off_len = wl[1];
    } else {
        out->v_kw = kw;
        out->v_off = w[1];
        out->v_off_len = wl[1];
    }
    return true;
}

/* §2.6's THIRD ARM, the `&&` one — either group may come first and each is one or two components long. The
   four (first, second) length pairs are tried in DESCENDING TOTAL, which is §8.3.1's "consumes as many
   components as possible" performed: a `left 10px top 20px` is four components and not a two-component
   `left 10px` with a stray pair after it. */
static unsigned pos_arm_pair(const char *const *w, const size_t *wl, unsigned n, bool three_value,
                             CssPositionValue *out)
{
    static const unsigned LENGTHS[4][2] = { { 2, 2 }, { 2, 1 }, { 1, 2 }, { 1, 1 } };
    unsigned i, k;

    for (i = 0; i < POS_N(LENGTHS); i++) {
        unsigned a = LENGTHS[i][0], b = LENGTHS[i][1], total = a + b;

        if (total > n) continue;
        /* §8.3's own Note is why this is a parameter and not a constant: the three-value form "creates parsing
           ambiguities when combined with other length or percentage components in a property value", so only
           §2.6 admits it and everything else stops one component short. */
        if (total == 3 && !three_value) continue;
        for (k = 0; k < 2; k++) {
            bool h_first = k == 0;
            CssPositionValue got;

            memset(&got, 0, sizeof got);
            if (!pos_group(w, wl, a, h_first, &got)) continue;
            if (!pos_group(w + a, wl + a, b, !h_first, &got)) continue;
            *out = got;
            return total;
        }
    }
    return 0;
}

unsigned css_position_match(const char *const *w, const size_t *wl, unsigned n, bool three_value,
                            CssPositionValue *out)
{
    CssPositionValue got;
    const char *kw;
    unsigned took;

    DCHECK(w != NULL && wl != NULL && out != NULL,
           "a <position> match was asked with no components or nowhere to write the normalized pair");
    if (n == 0) return 0;
    memset(&got, 0, sizeof got);
    /* THE `&&` ARM IS TRIED FIRST because it is the LONGEST, which is the whole of §8.3.1's greediness. It
       also subsumes §8.3's second arm (`[left|center|right] && [top|center|bottom]`, both groups one component
       long), so that arm needs no branch of its own. */
    took = pos_arm_pair(w, wl, n, three_value, &got);
    if (took > 0) {
        *out = got;
        return took;
    }
    /* §2.6's SECOND ARM (§8.3's third): `[ left | center | right | <length-percentage> ]
       [ top | center | bottom | <length-percentage> ]` — POSITIONAL, so a `top 50%` does not match it and a
       `center 50%` does. */
    if (n >= 2) {
        const char *h = pos_keyword(POS_H_KEYWORDS, POS_N(POS_H_KEYWORDS), w[0], wl[0]);
        const char *v = pos_keyword(POS_V_KEYWORDS, POS_N(POS_V_KEYWORDS), w[1], wl[1]);
        bool h_off = !h && pos_is_length_percentage(w[0], wl[0]);
        bool v_off = !v && pos_is_length_percentage(w[1], wl[1]);

        if ((h || h_off) && (v || v_off)) {
            got.h_kw = h;
            if (h_off) { got.h_off = w[0]; got.h_off_len = wl[0]; }
            got.v_kw = v;
            if (v_off) { got.v_off = w[1]; got.v_off_len = wl[1]; }
            *out = got;
            return 2;
        }
    }
    /* §8.3's FIRST ARM: `[ left | center | right | top | bottom | <length-percentage> ]`, with §8.3.2's
       "the implied center keyword is added" supplying the other axis. A bare `<length-percentage>` is the
       HORIZONTAL one, which is what §8.3's own ordering of the arm's keywords says and what makes
       `background-position: 50%` mean `50% center`. */
    kw = pos_keyword(POS_H_SIDES, POS_N(POS_H_SIDES), w[0], wl[0]);
    if (kw) {
        got.h_kw = kw;
        got.v_kw = "center";
        *out = got;
        return 1;
    }
    kw = pos_keyword(POS_V_SIDES, POS_N(POS_V_SIDES), w[0], wl[0]);
    if (kw) {
        got.h_kw = "center";
        got.v_kw = kw;
        *out = got;
        return 1;
    }
    if (pos_word_is(w[0], wl[0], "center")) {
        got.h_kw = "center";
        got.v_kw = "center";
        *out = got;
        return 1;
    }
    if (pos_is_length_percentage(w[0], wl[0])) {
        got.h_off = w[0];
        got.h_off_len = wl[0];
        got.v_kw = "center";
        *out = got;
        return 1;
    }
    return 0;
}

char *css_position_serialize(const CssPositionValue *p)
{
    /* Four components at most, each one component value plus its separating space, plus the terminator. The
       two offsets are the only unbounded pieces and they are spans of the declaration this run came from. */
    size_t cap, len = 0;
    char *out;
    unsigned i;
    const char *piece[4];
    size_t piece_len[4];
    unsigned n = 0;

    DCHECK(p != NULL, "a <position> serialization was asked with no position");
    DCHECK((p->h_kw != NULL || p->h_off != NULL) && (p->v_kw != NULL || p->v_off != NULL),
           "a <position> reached §8.3.2's serialization with an EMPTY axis. Every arm of §8.3 names both — the "
           "one-component arm through §8.3.2's own implied `center` — so an empty axis is a match that wrote a "
           "partial pair, and the string would be a one-value position, which §8.3.2's Note says is never "
           "serialized because it re-creates the parsing ambiguity the type exists to avoid");
    /* §8.3.2's ORDER, which is the whole reason the match normalized rather than copying: "components are
       serialized horizontal first, then vertical", and within an axis the keyword precedes its offset because
       that is the order §8.3's fourth arm writes them in. */
    if (p->h_kw) { piece[n] = p->h_kw; piece_len[n] = strlen(p->h_kw); n++; }
    if (p->h_off) { piece[n] = p->h_off; piece_len[n] = p->h_off_len; n++; }
    if (p->v_kw) { piece[n] = p->v_kw; piece_len[n] = strlen(p->v_kw); n++; }
    if (p->v_off) { piece[n] = p->v_off; piece_len[n] = p->v_off_len; n++; }
    cap = 1;
    for (i = 0; i < n; i++) cap += piece_len[i] + 1;
    out = malloc(cap);
    CHECK(out != NULL, "cssom: OOM serializing a <position> — a dropped one reads as the property's initial "
                       "value, which is a real coordinate and a different one");
    for (i = 0; i < n; i++) {
        if (i > 0) out[len++] = ' ';
        memcpy(out + len, piece[i], piece_len[i]);
        len += piece_len[i];
    }
    out[len] = '\0';
    return out;
}
