/* CSS COLOUR CONVERSION — CSS Color 4 §11's algorithm with §19's own numbers. See css_color_convert.h.
 *
 * EVERY CONSTANT BELOW IS TRANSCRIBED FROM §19 "Sample code for Color Conversions", in the form the spec
 * prints it: the white points as the quotients of their four-figure CIE x,y chromaticities, the sRGB, P3, A98
 * and Rec.2020 matrices as the exact rational fractions, and the Bradford, ProPhoto and Oklab matrices as the
 * 16- and 17-figure decimals the spec carries (those have no rational form). They are written that way ON
 * PURPOSE — a decimal rounded down from a fraction is a transcription error nothing can catch, while
 * `506752 / 1228815` either matches the spec or does not. */
#include <math.h>
#include <stdbool.h>
#include <stddef.h>

#include "check.h"
#include "core/css/css_color_convert.h"

/* §19's `multiplyMatrices(M, v)`: row i of the result is the dot product of row i of M with v. (The spec's
   prose calls its matrices column-major; its own code indexes them M[row][column], and the arithmetic below is
   what reproduces its published results — D65 white, XYZ (0.9505, 1, 1.089), comes back as linear sRGB 1 1 1.) */
static void css_mat3_mul(const double m[3][3], const double v[3], double out[3])
{
    int i;

    for (i = 0; i < 3; i++)
        out[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
}

/* §19: "standard white points, defined by 4-figure CIE x,y chromaticities". Only D50 appears here — it is the
   white Lab is defined against, while every matrix below already has its own white baked in. */
static const double CSS_D50[3] = {
    0.3457 / 0.3585, 1.00000, (1.0 - 0.3457 - 0.3585) / 0.3585
};

/* §19's D50_to_D65 and D65_to_D50: "Bradford chromatic adaptation". Each matrix is the composition of three
   operations — XYZ into the retinal cone domain, a per-component scale from one reference white to the other,
   and back to XYZ — which is why each is a single matrix here and not three. The two are NOT each other's
   exact inverse in these printed decimals; each is transcribed from the spec rather than derived from the
   other, so a round trip through both moves a component by the two transcriptions' own round-off. */
static const double CSS_BRADFORD_D50_TO_D65[3][3] = {
    {  0.955473421488075,    -0.02309845494876471,   0.06325924320057072  },
    { -0.0283697093338637,    1.0099953980813041,    0.021041441191917323 },
    {  0.012314014864481998, -0.020507649298898964,  1.330365926242124    }
};

static const double CSS_BRADFORD_D65_TO_D50[3][3] = {
    {  1.0479297925449969,    0.022946870601609652, -0.05019226628920524  },
    {  0.02962780877005599,   0.9904344267538799,   -0.017073799063418826 },
    { -0.009243040646204504,  0.015055191490298152,  0.7518742814281371   }
};

/* ---- §10's TRANSFER FUNCTIONS ---------------------------------------------------------------------------- */

/* §19's gam_sRGB and lin_sRGB, over the EXTENDED range: "For negative values, linear portion extends on
   reflection of axis, then uses reflected pow below that". The reflection is what makes an out-of-gamut
   component a real number rather than a NaN from a fractional power of a negative base, and it is the whole
   reason this component does not have to clip. §19's lin_P3 and gam_P3 are these two functions — "same as
   sRGB" — which is why display-p3 names them below rather than declaring a second pair. */
static double css_gam_srgb(double v)
{
    double sign = v < 0.0 ? -1.0 : 1.0;
    double abs_v = fabs(v);

    if (abs_v > 0.0031308)
        return sign * (1.055 * pow(abs_v, 1.0 / 2.4) - 0.055);
    return 12.92 * v;
}

static double css_lin_srgb(double v)
{
    double sign = v < 0.0 ? -1.0 : 1.0;
    double abs_v = fabs(v);

    if (abs_v <= 0.04045)
        return v / 12.92;
    return sign * pow((abs_v + 0.055) / 1.055, 2.4);
}

/* §19's gam_a98rgb and lin_a98rgb: a pure power curve, "close to but not exactly 1/2.2" — §10.6 states the
   exponent as the fraction 256/563, and its inverse is 563/256. Negative values are accepted by reflection. */
static double css_gam_a98(double v)
{
    double sign = v < 0.0 ? -1.0 : 1.0;

    return sign * pow(fabs(v), 256.0 / 563.0);
}

static double css_lin_a98(double v)
{
    double sign = v < 0.0 ? -1.0 : 1.0;

    return sign * pow(fabs(v), 563.0 / 256.0);
}

/* §19's gam_ProPhoto and lin_ProPhoto: gamma 1.8 with a small linear portion near black, whose two ends meet at
   different thresholds because one is stated in gamma-encoded units (Et = 1/512) and the other in linear ones
   (Et2 = 16/512 = 16 × Et). */
static double css_gam_prophoto(double v)
{
    const double et = 1.0 / 512.0;
    double sign = v < 0.0 ? -1.0 : 1.0;
    double abs_v = fabs(v);

    if (abs_v >= et)
        return sign * pow(abs_v, 1.0 / 1.8);
    return 16.0 * v;
}

static double css_lin_prophoto(double v)
{
    const double et2 = 16.0 / 512.0;
    double sign = v < 0.0 ? -1.0 : 1.0;
    double abs_v = fabs(v);

    if (abs_v <= et2)
        return v / 16.0;
    return sign * pow(abs_v, 1.8);
}

/* §19's gam_2020 and lin_2020: "Reference electro-optical transfer function from Rec. ITU-R BT.1886 Annex 1
   with b (black lift) = 0 and a (user gain) = 1, defined over the extended range, not clamped" — which with
   those two parameters is a pure gamma 2.4 curve, reflected for negative values. */
static double css_gam_2020(double v)
{
    double sign = v < 0.0 ? -1.0 : 1.0;

    return sign * pow(fabs(v), 1.0 / 2.4);
}

static double css_lin_2020(double v)
{
    double sign = v < 0.0 ? -1.0 : 1.0;

    return sign * pow(fabs(v), 2.4);
}

/* ---- §19's RGB-to-XYZ MATRIX PAIRS, one per predefined RGB space -------------------------------------------
 *
 * Each pair is relative to its OWN space's white point: D65 for sRGB, display-p3, a98-rgb and rec2020, and D50
 * for prophoto-rgb, which is why the table below carries a `d50` flag rather than folding an adaptation into
 * one of these matrices. */

static const double CSS_LIN_SRGB_TO_XYZ[3][3] = {
    { 506752.0 / 1228815.0,  87881.0 / 245763.0,   12673.0 /   70218.0 },
    {  87098.0 /  409605.0, 175762.0 / 245763.0,   12673.0 /  175545.0 },
    {   7918.0 /  409605.0,  87881.0 / 737289.0, 1001167.0 / 1053270.0 }
};

static const double CSS_XYZ_TO_LIN_SRGB[3][3] = {
    {   12831.0 /   3959.0,    -329.0 /    214.0, -1974.0 /   3959.0 },
    { -851781.0 / 878810.0, 1648619.0 / 878810.0, 36519.0 / 878810.0 },
    {     705.0 /  12673.0,   -2585.0 /  12673.0,   705.0 /    667.0 }
};

static const double CSS_LIN_P3_TO_XYZ[3][3] = {
    { 608311.0 / 1250200.0, 189793.0 / 714400.0,  198249.0 / 1000160.0 },
    {  35783.0 /  156275.0, 247089.0 / 357200.0,  198249.0 / 2500400.0 },
    {      0.0 /       1.0,  32229.0 / 714400.0, 5220557.0 / 5000800.0 }
};

static const double CSS_XYZ_TO_LIN_P3[3][3] = {
    { 446124.0 / 178915.0, -333277.0 / 357830.0, -72051.0 / 178915.0 },
    { -14852.0 /  17905.0,    63121.0 / 35810.0,    423.0 /  17905.0 },
    {  11844.0 / 330415.0,  -50337.0 / 660830.0, 316169.0 / 330415.0 }
};

static const double CSS_LIN_A98_TO_XYZ[3][3] = {
    { 573536.0 /  994567.0,  263643.0 / 1420810.0,  187206.0 /  994567.0 },
    { 591459.0 / 1989134.0, 6239551.0 / 9945670.0,  374412.0 / 4972835.0 },
    {  53769.0 / 1989134.0,  351524.0 / 4972835.0, 4929758.0 / 4972835.0 }
};

static const double CSS_XYZ_TO_LIN_A98[3][3] = {
    { 1829569.0 /  896150.0, -506331.0 /  896150.0, -308931.0 /  896150.0 },
    { -851781.0 /  878810.0, 1648619.0 /  878810.0,   36519.0 /  878810.0 },
    {   16779.0 / 1248040.0, -147721.0 / 1248040.0, 1266979.0 / 1248040.0 }
};

/* §19: "matrix cannot be expressed in rational form, but is calculated to 64 bit accuracy". D50-relative. */
static const double CSS_LIN_PROPHOTO_TO_XYZ_D50[3][3] = {
    { 0.79776664490064230, 0.13518129740053308, 0.03134773412839220 },
    { 0.28807482881940130, 0.71183523424187300, 0.00008993693872564 },
    { 0.00000000000000000, 0.00000000000000000, 0.82510460251046020 }
};

static const double CSS_XYZ_D50_TO_LIN_PROPHOTO[3][3] = {
    {  1.34578688164715830, -0.25557208737979464, -0.05110186497554526 },
    { -0.54463070512490190,  1.50824774284514680,  0.02052744743642139 },
    {  0.00000000000000000,  0.00000000000000000,  1.21196754563894520 }
};

/* §19's lin_2020_to_XYZ notes of its own leading zero: "0 is actually calculated as 4.994106574466076e-17". */
static const double CSS_LIN_2020_TO_XYZ[3][3] = {
    { 63426534.0 / 99577255.0,  20160776.0 / 139408157.0,  47086771.0 / 278816314.0 },
    { 26158966.0 / 99577255.0, 472592308.0 / 697040785.0,   8267143.0 / 139408157.0 },
    {        0.0 /        1.0,  19567812.0 / 697040785.0, 295819943.0 / 278816314.0 }
};

static const double CSS_XYZ_TO_LIN_2020[3][3] = {
    {  30757411.0 / 17917100.0, -6372589.0 / 17917100.0, -4539589.0 / 17917100.0 },
    { -19765991.0 / 29648200.0, 47925759.0 / 29648200.0,   467509.0 / 29648200.0 },
    {    792561.0 / 44930125.0, -1921689.0 / 44930125.0, 42328811.0 / 44930125.0 }
};

/* ---- the nine spaces, as the three facts §11 asks of each ---------------------------------------------------
 *
 * ONE ROW PER SPACE, because §11's algorithm asks each space exactly three questions and a space that answers
 * two of them is a space this component would silently convert wrongly: what is its transfer function (NULL
 * where the space is linear-light, which is a real answer and not a missing one), what is its matrix to and
 * from CIE XYZ relative to its OWN white (NULL for the two XYZ spaces, which ARE that pivot), and is that white
 * D50 rather than D65. The pairs are asserted to be filled in pairs at every use, because a row with a forward
 * matrix and no inverse converts in one direction and answers rubbish in the other. */
typedef double (*CssTransferFn)(double);

typedef struct {
    const double (*to_xyz)[3];
    const double (*from_xyz)[3];
    CssTransferFn lin;
    CssTransferFn gam;
    bool d50;
} CssColorSpaceRow;

static const CssColorSpaceRow CSS_COLOR_SPACE_TABLE[CSS_COLOR_SPACE__COUNT] = {
    [CSS_COLOR_SPACE_SRGB]              = { CSS_LIN_SRGB_TO_XYZ, CSS_XYZ_TO_LIN_SRGB,
                                            css_lin_srgb, css_gam_srgb, false },
    [CSS_COLOR_SPACE_SRGB_LINEAR]       = { CSS_LIN_SRGB_TO_XYZ, CSS_XYZ_TO_LIN_SRGB,
                                            NULL, NULL, false },
    [CSS_COLOR_SPACE_DISPLAY_P3]        = { CSS_LIN_P3_TO_XYZ, CSS_XYZ_TO_LIN_P3,
                                            css_lin_srgb, css_gam_srgb, false },
    [CSS_COLOR_SPACE_DISPLAY_P3_LINEAR] = { CSS_LIN_P3_TO_XYZ, CSS_XYZ_TO_LIN_P3,
                                            NULL, NULL, false },
    [CSS_COLOR_SPACE_A98_RGB]           = { CSS_LIN_A98_TO_XYZ, CSS_XYZ_TO_LIN_A98,
                                            css_lin_a98, css_gam_a98, false },
    [CSS_COLOR_SPACE_PROPHOTO_RGB]      = { CSS_LIN_PROPHOTO_TO_XYZ_D50, CSS_XYZ_D50_TO_LIN_PROPHOTO,
                                            css_lin_prophoto, css_gam_prophoto, true },
    [CSS_COLOR_SPACE_REC2020]           = { CSS_LIN_2020_TO_XYZ, CSS_XYZ_TO_LIN_2020,
                                            css_lin_2020, css_gam_2020, false },
    [CSS_COLOR_SPACE_XYZ_D50]           = { NULL, NULL, NULL, NULL, true },
    [CSS_COLOR_SPACE_XYZ_D65]           = { NULL, NULL, NULL, NULL, false }
};

/* The row for a space, with the two invariants a half-written row would break asserted at the one place every
   conversion goes through. */
static const CssColorSpaceRow *css_color_space_row(CssColorSpace space)
{
    DCHECK(space >= 0 && space < CSS_COLOR_SPACE__COUNT,
           "a colour conversion was asked for a colour space that is not one of CSS Color 4 §10's — the space "
           "came from a keyword the grammar accepted, so add it to CssColorSpace and to this component's table");
    DCHECK((CSS_COLOR_SPACE_TABLE[space].to_xyz == NULL) == (CSS_COLOR_SPACE_TABLE[space].from_xyz == NULL),
           "a predefined colour space has a matrix in one direction and not the other — §19 prints both, and a "
           "row with one converts into the space or out of it but answers rubbish the other way");
    DCHECK((CSS_COLOR_SPACE_TABLE[space].lin == NULL) == (CSS_COLOR_SPACE_TABLE[space].gam == NULL),
           "a predefined colour space has a transfer function in one direction and not the other — a space is "
           "either gamma-encoded in both directions or linear-light in both");
    return &CSS_COLOR_SPACE_TABLE[space];
}

/* Every component this file answers with is asserted FINITE, and on the RESULT rather than the input, because
   that is where a non-finite number becomes visible: §16.5 notes that the components are "theoretically
   unbounded" and that "there may be an implementation-defined limit for values approaching infinity" — this
   component has no such limit, so a component that overflows to an infinity (and then, once a matrix subtracts
   two of them, to a NaN) names that limit as the thing to build rather than silently reaching a serializer
   that cannot round it. */
static void css_color_check_finite(const double v[3])
{
    DCHECK(isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]),
           "a colour conversion produced a non-finite component — CSS Color 4 §16.5 allows an "
           "implementation-defined limit for component values approaching infinity and this component does not "
           "impose one, so build that limit where the components are read");
}

void css_color_space_to_xyz_d65(CssColorSpace space, const double c[3], double xyz[3])
{
    const CssColorSpaceRow *row = css_color_space_row(space);
    double lin[3], own_white[3];
    int i;

    for (i = 0; i < 3; i++)
        lin[i] = row->lin ? row->lin(c[i]) : c[i];

    if (row->to_xyz != NULL)
        css_mat3_mul(row->to_xyz, lin, own_white);
    else
        for (i = 0; i < 3; i++) own_white[i] = lin[i];

    if (row->d50)
        css_mat3_mul(CSS_BRADFORD_D50_TO_D65, own_white, xyz);
    else
        for (i = 0; i < 3; i++) xyz[i] = own_white[i];

    css_color_check_finite(xyz);
}

void css_color_space_from_xyz_d65(CssColorSpace space, const double xyz[3], double c[3])
{
    const CssColorSpaceRow *row = css_color_space_row(space);
    double own_white[3], lin[3];
    int i;

    if (row->d50)
        css_mat3_mul(CSS_BRADFORD_D65_TO_D50, xyz, own_white);
    else
        for (i = 0; i < 3; i++) own_white[i] = xyz[i];

    if (row->from_xyz != NULL)
        css_mat3_mul(row->from_xyz, own_white, lin);
    else
        for (i = 0; i < 3; i++) lin[i] = own_white[i];

    for (i = 0; i < 3; i++)
        c[i] = row->gam ? row->gam(lin[i]) : lin[i];

    css_color_check_finite(c);
}

/* ---- the Lab family ------------------------------------------------------------------------------------- */

/* §19's OKLab_to_XYZ, "recalculated for consistent reference white ... recalculated for 64bit precision". The
   pair is applied OKLabtoLMS first, then a cube of each component, then LMStoXYZ — the cube is the inverse of
   the cube root Oklab's forward direction takes, and it is the step that makes this a different algorithm from
   Lab rather than the same one with other constants. */
static const double CSS_OKLAB_TO_LMS[3][3] = {
    { 1.0000000000000000,  0.3963377773761749,  0.2158037573099136 },
    { 1.0000000000000000, -0.1055613458156586, -0.0638541728258133 },
    { 1.0000000000000000, -0.0894841775298119, -1.2914855480194092 }
};

static const double CSS_LMS_TO_XYZ_D65[3][3] = {
    {  1.2268798758459243, -0.5578149944602171,  0.2813910456659647 },
    { -0.0405757452148008,  1.1122868032803170, -0.0717110580655164 },
    { -0.0763729366746601, -0.4214933324022432,  1.5869240198367816 }
};

/* §19's Lab_to_XYZ, "Convert Lab to D50-adapted XYZ". The two constants are the CIE standard's own rational
   fractions — ε is 6³/29³ and κ is 29³/3³ — and the y branch tests the LIGHTNESS against κ·ε rather than
   testing the cubed f value, exactly as the spec's code does; the two are equivalent in exact arithmetic and
   the spec's form is the one that is stable at the join. */
static void css_lab_to_xyz_d50(double l, double a, double b, double xyz[3])
{
    const double eps = 216.0 / 24389.0;
    const double kappa = 24389.0 / 27.0;
    double f[3], xyz_rel[3];
    int i;

    f[1] = (l + 16.0) / 116.0;
    f[0] = a / 500.0 + f[1];
    f[2] = f[1] - b / 200.0;

    xyz_rel[0] = f[0] * f[0] * f[0] > eps ? f[0] * f[0] * f[0] : (116.0 * f[0] - 16.0) / kappa;
    xyz_rel[1] = l > kappa * eps ? f[1] * f[1] * f[1] : l / kappa;
    xyz_rel[2] = f[2] * f[2] * f[2] > eps ? f[2] * f[2] * f[2] : (116.0 * f[2] - 16.0) / kappa;

    /* "Compute XYZ by scaling xyz by reference white." */
    for (i = 0; i < 3; i++)
        xyz[i] = xyz_rel[i] * CSS_D50[i];
}

/* §19's OKLab_to_XYZ, "Given OKLab, convert to XYZ relative to D65". */
static void css_oklab_to_xyz_d65(double l, double a, double b, double xyz[3])
{
    double lab[3], lms[3], lms_lin[3];
    int i;

    lab[0] = l;
    lab[1] = a;
    lab[2] = b;
    css_mat3_mul(CSS_OKLAB_TO_LMS, lab, lms);
    for (i = 0; i < 3; i++)
        lms_lin[i] = lms[i] * lms[i] * lms[i];
    css_mat3_mul(CSS_LMS_TO_XYZ_D65, lms_lin, xyz);
}

void css_lab_to_srgb(double l, double a, double b, double srgb[3])
{
    double xyz_d50[3], xyz_d65[3];

    DCHECK(l >= 0.0 && l <= 100.0,
           "a Lab lightness outside [0, 100] reached the conversion — CSS Color 4 §9.3 clamps it at "
           "parsed-value time, so its caller skipped the clamp");
    DCHECK(isfinite(a) && isfinite(b),
           "a non-finite Lab a or b axis reached the conversion — the <color> grammar's <number> production "
           "produces a finite double, so this came from arithmetic the caller performed on it");
    css_lab_to_xyz_d50(l, a, b, xyz_d50);
    css_mat3_mul(CSS_BRADFORD_D50_TO_D65, xyz_d50, xyz_d65);
    css_color_space_from_xyz_d65(CSS_COLOR_SPACE_SRGB, xyz_d65, srgb);
}

void css_lch_to_srgb(double l, double c, double h_deg, double srgb[3])
{
    double rad;

    DCHECK(c >= 0.0, "a negative LCH chroma reached the conversion — CSS Color 4 §9.3 clamps it to 0 at "
                     "parsed-value time, so its caller skipped the clamp");
    DCHECK(isfinite(c) && isfinite(h_deg),
           "a non-finite LCH chroma or hue reached the conversion — <hue> resolves to a finite number of "
           "degrees and <number> to a finite double");
    /* §19's LCH_to_Lab: "Convert from polar form". */
    rad = h_deg * M_PI / 180.0;
    css_lab_to_srgb(l, c * cos(rad), c * sin(rad), srgb);
}

void css_oklab_to_srgb(double l, double a, double b, double srgb[3])
{
    double xyz[3];

    DCHECK(l >= 0.0 && l <= 1.0,
           "an Oklab lightness outside [0, 1] reached the conversion — CSS Color 4 §9.4 clamps it at "
           "parsed-value time, so its caller skipped the clamp");
    DCHECK(isfinite(a) && isfinite(b),
           "a non-finite Oklab a or b axis reached the conversion — the <color> grammar's <number> production "
           "produces a finite double, so this came from arithmetic the caller performed on it");
    /* Oklab is already relative to D65, which is sRGB's own white, so §11's chromatic-adaptation step is
       SKIPPED here rather than performed with an identity — the step exists only when the white points differ,
       and running a Bradford round trip anyway would move the colour by the transform's own round-off. */
    css_oklab_to_xyz_d65(l, a, b, xyz);
    css_color_space_from_xyz_d65(CSS_COLOR_SPACE_SRGB, xyz, srgb);
}

void css_oklch_to_srgb(double l, double c, double h_deg, double srgb[3])
{
    double rad;

    DCHECK(c >= 0.0, "a negative OkLCh chroma reached the conversion — CSS Color 4 §9.4 clamps it to 0 at "
                     "parsed-value time, so its caller skipped the clamp");
    DCHECK(isfinite(c) && isfinite(h_deg),
           "a non-finite OkLCh chroma or hue reached the conversion — <hue> resolves to a finite number of "
           "degrees and <number> to a finite double");
    /* §19's OKLCH_to_OKLab, the same polar form as LCH's with OkLCh's own component ranges. */
    rad = h_deg * M_PI / 180.0;
    css_oklab_to_srgb(l, c * cos(rad), c * sin(rad), srgb);
}
