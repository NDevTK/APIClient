/* See pair_kerning.h. Field layouts are OpenType's 'GPOS' — Glyph Positioning Table and the common table
   formats chapter (ScriptList, FeatureList, LookupList, Coverage, ClassDef). */
#include <string.h>

#include "check.h"
#include "core/fonts/pair_kerning.h"

#define PK_TAG(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define PK_IGNORE_MARKS 0x0008u   /* LookupFlag bit 3 */
#define PK_RIGHT_TO_LEFT 0x0001u  /* LookupFlag bit 0: cursive attachment only, so no pair reads it */
#define PK_X_ADVANCE 0x0004u      /* ValueFormat bit 2 */

static bool pk_fits(size_t len, size_t off, size_t n)
{
    return n <= len && off <= len - n;
}

static uint16_t pk_u16(const unsigned char *b, size_t off)
{
    return (uint16_t)((b[off] << 8) | b[off + 1]);
}

static uint32_t pk_u32(const unsigned char *b, size_t off)
{
    return ((uint32_t)pk_u16(b, off) << 16) | pk_u16(b, off + 2);
}

#define PK_REJECT(k, why) do { (k)->reject = (why); return false; } while (0)

/* A Coverage table at `off` of `b[0..len)` holds within it: format 1's sorted glyph array or format 2's ranges. */
static bool pk_coverage_ok(const unsigned char *b, size_t len, size_t off)
{
    uint16_t fmt, n;

    if (!pk_fits(len, off, 4)) return false;
    fmt = pk_u16(b, off);
    n = pk_u16(b, off + 2);
    if (fmt == 1) return pk_fits(len, off + 4, (size_t)2 * n);
    if (fmt == 2) return pk_fits(len, off + 4, (size_t)6 * n);
    return false;
}

/* The glyph's Coverage index, or -1 when it is not covered. Both formats are sorted by glyph, so a search. */
static int32_t pk_coverage(const unsigned char *b, size_t off, uint16_t glyph)
{
    uint16_t fmt = pk_u16(b, off), n = pk_u16(b, off + 2);
    int32_t lo = 0, hi = (int32_t)n - 1;

    while (lo <= hi) {
        int32_t mid = (lo + hi) / 2;
        if (fmt == 1) {
            uint16_t g = pk_u16(b, off + 4 + (size_t)2 * mid);
            if (g == glyph) return mid;
            if (g < glyph) lo = mid + 1; else hi = mid - 1;
        } else {
            size_t r = off + 4 + (size_t)6 * mid;
            if (glyph < pk_u16(b, r)) hi = mid - 1;
            else if (glyph > pk_u16(b, r + 2)) lo = mid + 1;
            else return (int32_t)pk_u16(b, r + 4) + (glyph - pk_u16(b, r));
        }
    }
    return -1;
}

static bool pk_class_def_ok(const unsigned char *b, size_t len, size_t off)
{
    uint16_t fmt;

    if (!pk_fits(len, off, 6)) return false;
    fmt = pk_u16(b, off);
    if (fmt == 1) return pk_fits(len, off + 6, (size_t)2 * pk_u16(b, off + 4));
    if (fmt == 2) return pk_fits(len, off + 4, (size_t)6 * pk_u16(b, off + 2));
    return false;
}

/* The glyph's class; a glyph the table does not list is class 0. */
static uint16_t pk_class(const unsigned char *b, size_t off, uint16_t glyph)
{
    uint16_t fmt = pk_u16(b, off);

    if (fmt == 1) {
        uint16_t start = pk_u16(b, off + 2), n = pk_u16(b, off + 4);
        return glyph >= start && glyph - start < n ? pk_u16(b, off + 6 + (size_t)2 * (glyph - start)) : 0;
    } else {
        int32_t lo = 0, hi = (int32_t)pk_u16(b, off + 2) - 1;
        while (lo <= hi) {
            int32_t mid = (lo + hi) / 2;
            size_t r = off + 4 + (size_t)6 * mid;
            if (glyph < pk_u16(b, r)) hi = mid - 1;
            else if (glyph > pk_u16(b, r + 2)) lo = mid + 1;
            else return pk_u16(b, r + 4);
        }
        return 0;
    }
}

/* The bytes of a ValueRecord in `format`: two per set bit. */
static size_t pk_value_size(uint16_t format)
{
    size_t n = 0;

    for (; format; format >>= 1) n += format & 1u;
    return 2 * n;
}

/* One PairPos subtable at `off`, proved: its formats, its Coverage, and every PairSet or class record. */
static bool pk_pair_pos_ok(PairKerning *k, size_t off)
{
    const unsigned char *b = k->gpos;
    size_t len = k->gpos_len, rec;
    uint16_t fmt, vf1, vf2, i, n;

    if (!pk_fits(len, off, 8)) PK_REJECT(k, "a 'GPOS' PairPos subtable runs past the table");
    fmt = pk_u16(b, off);
    vf1 = pk_u16(b, off + 4);
    vf2 = pk_u16(b, off + 6);
    if (vf1 != PK_X_ADVANCE || vf2 != 0)
        PK_REJECT(k, "a 'GPOS' kern PairPos subtable's ValueFormats are not an XAdvance for the first glyph and "
                     "nothing for the second; placement, device tables and second-glyph adjustment are not built");
    if (!pk_coverage_ok(b, len, off + pk_u16(b, off + 2)))
        PK_REJECT(k, "a 'GPOS' PairPos subtable's Coverage table is malformed or runs past the table");
    rec = 2 + pk_value_size(vf1) + pk_value_size(vf2);
    if (fmt == 1) {
        if (!pk_fits(len, off + 10, (size_t)2 * pk_u16(b, off + 8)))
            PK_REJECT(k, "a 'GPOS' PairPosFormat1 subtable's PairSet offsets run past the table");
        for (i = 0, n = pk_u16(b, off + 8); i < n; i++) {
            size_t ps = off + pk_u16(b, off + 10 + (size_t)2 * i);
            if (!pk_fits(len, ps, 2) || !pk_fits(len, ps + 2, rec * pk_u16(b, ps)))
                PK_REJECT(k, "a 'GPOS' PairSet runs past the table");
        }
        return true;
    }
    if (fmt == 2) {
        if (!pk_fits(len, off, 16) || !pk_class_def_ok(b, len, off + pk_u16(b, off + 8)) ||
            !pk_class_def_ok(b, len, off + pk_u16(b, off + 10)) ||
            !pk_fits(len, off + 16, (rec - 2) * pk_u16(b, off + 12) * pk_u16(b, off + 14)))
            PK_REJECT(k, "a 'GPOS' PairPosFormat2 subtable's class definitions or class records run past the "
                         "table");
        return true;
    }
    PK_REJECT(k, "a 'GPOS' PairPos subtable's format is neither 1 nor 2");
}

bool pair_kerning_read(PairKerning *k, const unsigned char *gpos, size_t gpos_len, const unsigned char *gdef,
                       size_t gdef_len)
{
    size_t sl, fl, ll, script = 0, langsys;
    uint16_t i, j, nf, idx[PAIR_KERNING_LOOKUP_MAX * 4];
    int nidx = 0;

    memset(k, 0, sizeof *k);
    if (gpos == NULL) return true;   /* no 'GPOS': the face kerns nothing */
    k->gpos = gpos;
    k->gpos_len = gpos_len;
    if (!pk_fits(gpos_len, 0, 10) || pk_u16(gpos, 0) != 1)
        PK_REJECT(k, "the 'GPOS' header is short or its majorVersion is not 1");
    sl = pk_u16(gpos, 4);
    fl = pk_u16(gpos, 6);
    ll = pk_u16(gpos, 8);
    if (!pk_fits(gpos_len, sl, 2) || !pk_fits(gpos_len, sl + 2, (size_t)6 * pk_u16(gpos, sl)) ||
        !pk_fits(gpos_len, fl, 2) || !pk_fits(gpos_len, fl + 2, (size_t)6 * pk_u16(gpos, fl)) ||
        !pk_fits(gpos_len, ll, 2) || !pk_fits(gpos_len, ll + 2, (size_t)2 * pk_u16(gpos, ll)))
        PK_REJECT(k, "a 'GPOS' ScriptList, FeatureList or LookupList runs past the table");
    for (i = 0; i < pk_u16(gpos, sl); i++) {
        uint32_t tag = pk_u32(gpos, sl + 2 + (size_t)6 * i);
        if (tag == PK_TAG('l', 'a', 't', 'n') || (tag == PK_TAG('D', 'F', 'L', 'T') && script == 0))
            script = sl + pk_u16(gpos, sl + 6 + (size_t)6 * i);
    }
    if (script == 0) return true;   /* neither script: nothing a run here is kerned with */
    if (!pk_fits(gpos_len, script, 4)) PK_REJECT(k, "a 'GPOS' Script table runs past the table");
    if (pk_u16(gpos, script) == 0) return true;   /* no default language system */
    langsys = script + pk_u16(gpos, script);
    if (!pk_fits(gpos_len, langsys, 6) || !pk_fits(gpos_len, langsys + 6, (size_t)2 * pk_u16(gpos, langsys + 4)))
        PK_REJECT(k, "a 'GPOS' LangSys table runs past the table");
    nf = pk_u16(gpos, langsys + 4);
    for (i = 0; i <= nf; i++) {
        uint16_t f = i < nf ? pk_u16(gpos, langsys + 6 + (size_t)2 * i) : pk_u16(gpos, langsys + 2);
        size_t ft;

        if (i == nf && f == 0xFFFFu) break;   /* requiredFeatureIndex: none */
        if (f >= pk_u16(gpos, fl)) PK_REJECT(k, "a 'GPOS' LangSys names a feature past the FeatureList");
        if (pk_u32(gpos, fl + 2 + (size_t)6 * f) != PK_TAG('k', 'e', 'r', 'n')) continue;
        ft = fl + pk_u16(gpos, fl + 6 + (size_t)6 * f);
        if (!pk_fits(gpos_len, ft, 4) || !pk_fits(gpos_len, ft + 4, (size_t)2 * pk_u16(gpos, ft + 2)))
            PK_REJECT(k, "a 'GPOS' Feature table runs past the table");
        for (j = 0; j < pk_u16(gpos, ft + 2); j++) {
            if (nidx == PAIR_KERNING_LOOKUP_MAX * 4)
                PK_REJECT(k, "a face's 'kern' features name more lookups than PAIR_KERNING_LOOKUP_MAX allows for; "
                             "widen it");
            idx[nidx++] = pk_u16(gpos, ft + 4 + (size_t)2 * j);
        }
    }
    /* LookupList order, each lookup once: HarfBuzz applies a feature set's lookups by index. */
    for (i = 0; i < (uint16_t)nidx; i++)
        for (j = (uint16_t)(i + 1); j < (uint16_t)nidx; j++)
            if (idx[j] < idx[i]) { uint16_t t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
    for (i = 0; i < (uint16_t)nidx; i++) {
        size_t lt;
        uint16_t type, flag, ns, s;

        if (i > 0 && idx[i] == idx[i - 1]) continue;
        if (idx[i] >= pk_u16(gpos, ll)) PK_REJECT(k, "a 'GPOS' Feature names a lookup past the LookupList");
        if (k->n == PAIR_KERNING_LOOKUP_MAX)
            PK_REJECT(k, "a face's 'kern' features name more lookups than PAIR_KERNING_LOOKUP_MAX; widen it");
        lt = ll + pk_u16(gpos, ll + 2 + (size_t)2 * idx[i]);
        if (!pk_fits(gpos_len, lt, 6) || !pk_fits(gpos_len, lt + 6, (size_t)2 * pk_u16(gpos, lt + 4)))
            PK_REJECT(k, "a 'GPOS' Lookup table runs past the table");
        type = pk_u16(gpos, lt);
        flag = pk_u16(gpos, lt + 2);
        ns = pk_u16(gpos, lt + 4);
        if (type != 2)
            PK_REJECT(k, "a 'kern' lookup is not LookupType 2 (Pair Adjustment); an Extension (type 9) or another "
                         "positioning type is not built");
        if (flag & ~(PK_IGNORE_MARKS | PK_RIGHT_TO_LEFT))
            PK_REJECT(k, "a 'kern' lookup's LookupFlag asks for more than IgnoreMarks: base or ligature skipping, a "
                         "mark attachment class or a mark filtering set is not built");
        for (s = 0; s < ns; s++)
            if (!pk_pair_pos_ok(k, lt + pk_u16(gpos, lt + 6 + (size_t)2 * s))) return false;
        k->lookup[k->n] = lt;
        k->ignore_marks[k->n] = (flag & PK_IGNORE_MARKS) != 0;
        k->n++;
    }
    if (gdef != NULL) {
        k->gdef = gdef;
        k->gdef_len = gdef_len;
        if (!pk_fits(gdef_len, 0, 6) || pk_u16(gdef, 0) != 1)
            PK_REJECT(k, "the 'GDEF' header is short or its majorVersion is not 1");
        k->glyph_class_def = pk_u16(gdef, 4);
        if (k->glyph_class_def != 0 && !pk_class_def_ok(gdef, gdef_len, k->glyph_class_def))
            PK_REJECT(k, "the 'GDEF' glyphClassDef table runs past the table");
    }
    return true;
}

/* Whether lookup `l` skips `glyph`: IgnoreMarks over a glyph GDEF classes as a mark. */
static bool pk_skips(const PairKerning *k, int l, uint16_t glyph)
{
    return k->ignore_marks[l] && k->glyph_class_def != 0 && pk_class(k->gdef, k->glyph_class_def, glyph) == 3;
}

/* The pair's XAdvance in lookup `l`: the first subtable that applies to it, as HarfBuzz takes it. Format 1
   applies when it lists the second glyph for the covered first one; format 2 whenever the first is covered. */
static int32_t pk_pair(const PairKerning *k, int l, uint16_t left, uint16_t right)
{
    const unsigned char *b = k->gpos;
    size_t lt = k->lookup[l];
    uint16_t s, ns = pk_u16(b, lt + 4);

    for (s = 0; s < ns; s++) {
        size_t st = lt + pk_u16(b, lt + 6 + (size_t)2 * s);
        int32_t cov = pk_coverage(b, st + pk_u16(b, st + 2), left);

        if (cov < 0) continue;
        if (pk_u16(b, st) == 1) {
            size_t ps = st + pk_u16(b, st + 10 + (size_t)2 * cov);
            int32_t lo = 0, hi = (int32_t)pk_u16(b, ps) - 1;
            while (lo <= hi) {
                int32_t mid = (lo + hi) / 2;
                uint16_t g = pk_u16(b, ps + 2 + (size_t)4 * mid);
                if (g == right) return (int16_t)pk_u16(b, ps + 4 + (size_t)4 * mid);
                if (g < right) lo = mid + 1; else hi = mid - 1;
            }
            continue;
        } else {
            uint16_t c1 = pk_class(b, st + pk_u16(b, st + 8), left), c2 = pk_class(b, st + pk_u16(b, st + 10), right);
            uint16_t n1 = pk_u16(b, st + 12), n2 = pk_u16(b, st + 14);
            if (c1 >= n1 || c2 >= n2) return 0;
            return (int16_t)pk_u16(b, st + 16 + (size_t)2 * ((size_t)c1 * n2 + c2));
        }
    }
    return 0;
}

int32_t pair_kerning_x_advance(const PairKerning *k, uint16_t left, PairKerningGlyphAfter after, void *arg)
{
    int32_t sum = 0;
    int l;

    DCHECK(k->reject == NULL, "a face whose 'GPOS' kerning pair_kerning_read refused was asked for a pair");
    for (l = 0; l < k->n; l++) {
        size_t at = 0;
        int32_t g;

        if (pk_skips(k, l, left)) continue;
        while ((g = after(arg, at)) >= 0 && pk_skips(k, l, (uint16_t)g)) at++;
        if (g >= 0) sum += pk_pair(k, l, left, (uint16_t)g);
    }
    return sum;
}
