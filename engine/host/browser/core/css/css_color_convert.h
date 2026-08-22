/* CSS COLOUR CONVERSION — CSS Color Module Level 4 §11's CONVERTING COLORS, and the §10 PREDEFINED COLOR SPACES
 * it converts between: what each one's transfer function is, what its matrix to and from CIE XYZ is, and which
 * white point it is relative to.
 *
 * ITS OWN COMPONENT because it is a different problem from css_color.c's. That file's problem is a STRING:
 * lexbor's grammar, the `color()` production lexbor does not have, the shape of a parsed value, which
 * declarations are colours at all. This file's problem is ARITHMETIC over numbers somebody else already parsed
 * — matrices, white points and transfer functions whose exact values the specification prints. Nothing here
 * knows what a declaration is, and nothing in css_color.c needs to know what a cone response is. Each is then
 * one assertable contract, and this one is exercised with three doubles and no parser.
 *
 * XYZ RELATIVE TO D65 IS THE PIVOT. §11 converts src to dest through CIE XYZ, adapting the white point when the
 * two spaces' whites differ, so every space here declares itself by exactly two operations — to XYZ-D65 and
 * back — and any src-to-dest conversion is one of each. D65 is the pivot rather than D50 because it is the
 * white of six of the nine spaces (and of Oklab), so the Bradford transform runs only for the two D50 spaces,
 * which is what §11 asks for: adapt only "if dest-white is not the same as src-white".
 *
 * §11'S ALGORITHM, IN THE ORDER THE SPEC GIVES IT. Replace any missing component with zero (the CALLER does
 * that, because `none` is a parsed-value fact and only the caller holds it); convert a cylindrical polar
 * representation to its rectangular one; convert to CIE XYZ with the SOURCE white point; chromatically adapt to
 * the DESTINATION white point with a linear Bradford transform when the two differ; convert XYZ to the
 * destination's linear light; apply the destination's transfer function. Lab and LCH are D50 spaces, so they
 * adapt to D65; Oklab and OkLCh are already D65, so they do NOT — and Oklab is not Lab with different
 * constants, it is its own pair of LMS cone-response matrices with a cube root between them.
 *
 * OUT OF GAMUT IS PRESERVED, NOT MAPPED. §11 gamut maps in exactly one case: "if dest is a physical output
 * color space, such as a display, then col2 must be css gamut mapped so that it can be displayed". A used
 * `<color>` is a value, not a display, so §14.2's gamut mapping does not apply to this step at all. Every
 * component answered here is therefore EXTENDED — each RGB space's transfer function is defined over the whole
 * real line by reflecting the curve through the origin, and a component outside [0, 1] is a CORRECT answer that
 * the caller's own serialization then decides about (HTML §4.10.5.1.14 step 4.2 rounds "into the range 0 to
 * 255", which is where its clip belongs; §16.5's `color()` form keeps the out-of-gamut number, which is why
 * HTML's own example of a Display P3 colour well value is `color(display-p3 1.84 -0.19 0.72 / 0.6)`). Clipping
 * here would be a gamut map the spec did not ask for, performed at the one place that still knows the real
 * value.
 *
 * THE CALLER HAS ALREADY DONE THE SPEC'S PARSE-TIME CLAMPING. §9.3 and §9.4 clamp Lightness into its reference
 * range and clamp a negative chroma to zero "at parsed-value time", which is before this component sees them;
 * each Lab entry asserts that, because a lightness outside the range would silently answer a colour that no
 * input can name. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_COLOR_CONVERT_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_COLOR_CONVERT_H

/* CSS Color 4 §10's PREDEFINED COLOR SPACES — the nine distinct spaces the `color()` function's ten keywords
   name. `xyz` and `xyz-d65` are ONE space and not two: §10.9 says "The reference white for xyz-d50 is D50,
   while the reference white for xyz-d65 and xyz is D65", so the two keywords differ only in spelling, which is
   the grammar's business (css_color.c) and not this component's.
   The four spaces of the Lab family are deliberately NOT here — see css_color.h for why a lab() colour is
   converted at parse time rather than carried in its own space. */
typedef enum {
    CSS_COLOR_SPACE_SRGB = 0,
    CSS_COLOR_SPACE_SRGB_LINEAR,
    CSS_COLOR_SPACE_DISPLAY_P3,
    CSS_COLOR_SPACE_DISPLAY_P3_LINEAR,
    CSS_COLOR_SPACE_A98_RGB,
    CSS_COLOR_SPACE_PROPHOTO_RGB,
    CSS_COLOR_SPACE_REC2020,
    CSS_COLOR_SPACE_XYZ_D50,
    CSS_COLOR_SPACE_XYZ_D65,
    CSS_COLOR_SPACE__COUNT
} CssColorSpace;

/* The two halves of §11: a colour in `space` to CIE XYZ relative to D65, and back. `c` and `xyz` are three
   components each, and neither is clamped in either direction. A missing component must have been replaced by
   zero before the call — §11's own step 2 — because a missing component is a parsed-value fact this component
   cannot see. */
void css_color_space_to_xyz_d65(CssColorSpace space, const double c[3], double xyz[3]);
void css_color_space_from_xyz_d65(CssColorSpace space, const double xyz[3], double c[3]);

/* CIE Lab to extended sRGB. `l` is the CIE Lightness, already clamped to [0, 100] per §9.3; `a` and `b` are the
   signed, unbounded distances along the a and b axes. Writes the three gamma-encoded sRGB components, which
   are NOT restricted to [0, 1] — Lab's gamut is far larger than sRGB's. */
void css_lab_to_srgb(double l, double a, double b, double srgb[3]);

/* CIE LCH to extended sRGB: §19's LCH_to_Lab (the polar form, hue in DEGREES, 0deg along the positive a axis)
   followed by the Lab conversion above. `c` is the chroma, already clamped to a minimum of 0 per §9.3. The hue
   is NOT required to be normalised into [0, 360) — cosine and sine answer identically for any coterminal
   angle, which is why §19's own code does not reduce it either. */
void css_lch_to_srgb(double l, double c, double h_deg, double srgb[3]);

/* Oklab to extended sRGB. `l` is the Oklab Lightness, already clamped to [0, 1] per §9.4. */
void css_oklab_to_srgb(double l, double a, double b, double srgb[3]);

/* OkLCh to extended sRGB: §19's OKLCH_to_OKLab followed by the Oklab conversion above. `c` is already clamped
   to a minimum of 0 per §9.4, and the hue is in DEGREES. */
void css_oklch_to_srgb(double l, double c, double h_deg, double srgb[3]);

#endif
