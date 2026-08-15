/* CSS COLOUR CONVERSION — CSS Color 4 §12's algorithm with §19's own numbers. See css_color_convert.h.
 *
 * EVERY CONSTANT BELOW IS TRANSCRIBED FROM §19 "Sample code for Color Conversions", in the form the spec
 * prints it: the white points as the quotients of their four-figure CIE x,y chromaticities, the sRGB matrix as
 * the exact rational fractions, the Bradford and Oklab matrices as the 16-figure decimals the spec carries
 * (they have no rational form). They are written that way ON PURPOSE — a decimal rounded down from a fraction
 * is a transcription error nothing can catch, while `506752 / 1228815` either matches the spec or does not. */
#include <math.h>

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

/* §19: "standard white points, defined by 4-figure CIE x,y chromaticities". Only D50 appears here — Lab is
   defined relative to it, while the Oklab and sRGB matrices already have D65 baked into them. */
static const double CSS_D50[3] = {
    0.3457 / 0.3585, 1.00000, (1.0 - 0.3457 - 0.3585) / 0.3585
};

/* §19's D50_to_D65: "Bradford chromatic adaptation from D50 to D65". The matrix is the composition of three
   operations — XYZ into the retinal cone domain, a per-component scale from one reference white to the other,
   and back to XYZ — which is why it is a single matrix here and not three. */
static const double CSS_BRADFORD_D50_TO_D65[3][3] = {
    {  0.955473421488075,    -0.02309845494876471,   0.06325924320057072  },
    { -0.0283697093338637,    1.0099953980813041,    0.021041441191917323 },
    {  0.012314014864481998, -0.020507649298898964,  1.330365926242124    }
};

/* §19's XYZ_to_lin_sRGB. sRGB's own white is D65, so no adaptation is folded into this matrix. */
static const double CSS_XYZ_D65_TO_LIN_SRGB[3][3] = {
    {   12831.0 /   3959.0,    -329.0 /    214.0, -1974.0 /   3959.0 },
    { -851781.0 / 878810.0, 1648619.0 / 878810.0, 36519.0 / 878810.0 },
    {     705.0 /  12673.0,   -2585.0 /  12673.0,   705.0 /    667.0 }
};

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

/* §19's gam_sRGB, the sRGB transfer function over the EXTENDED range: "For negative values, linear portion
   extends on reflection of axis, then uses reflected pow below that". The reflection is what makes an
   out-of-gamut component a real number rather than a NaN from a fractional power of a negative base, and it is
   the whole reason this component does not have to clip. */
static double css_gam_srgb(double v)
{
    double sign = v < 0.0 ? -1.0 : 1.0;
    double abs_v = fabs(v);

    if (abs_v > 0.0031308)
        return sign * (1.055 * pow(abs_v, 1.0 / 2.4) - 0.055);
    return 12.92 * v;
}

/* The tail every one of the four entry points shares: XYZ relative to D65, through sRGB's own matrix and its
   transfer function. The DCHECK is on the RESULT rather than the input because that is where a non-finite
   number becomes visible: §16.5 notes that r, g and b are "theoretically unbounded" and that "there may be an
   implementation-defined limit for values approaching infinity" — this component has no such limit, so a
   component that overflows to an infinity (and then, once the sRGB matrix subtracts two of them, to a NaN)
   names that limit as the thing to build rather than silently reaching a serializer that cannot round it. */
static void css_xyz_d65_to_srgb(const double xyz[3], double srgb[3])
{
    double lin[3];
    int i;

    css_mat3_mul(CSS_XYZ_D65_TO_LIN_SRGB, xyz, lin);
    for (i = 0; i < 3; i++)
        srgb[i] = css_gam_srgb(lin[i]);
    DCHECK(isfinite(srgb[0]) && isfinite(srgb[1]) && isfinite(srgb[2]),
           "a colour conversion produced a non-finite sRGB component — CSS Color 4 §16.5 allows an "
           "implementation-defined limit for component values approaching infinity and this component does not "
           "impose one, so build that limit where the a/b/chroma components are read");
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
    css_xyz_d65_to_srgb(xyz_d65, srgb);
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
    /* Oklab is already relative to D65, which is sRGB's own white, so §12's chromatic-adaptation step is
       SKIPPED here rather than performed with an identity — the step exists only when the white points differ,
       and running a Bradford round trip anyway would move the colour by the transform's own round-off. */
    css_oklab_to_xyz_d65(l, a, b, xyz);
    css_xyz_d65_to_srgb(xyz, srgb);
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
