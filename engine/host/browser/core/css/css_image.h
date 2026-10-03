/* CSS Images 3 §2 "Image Values: the <image> type" — `<image> = <url> | <gradient>`.
 *
 * WHY IT IS A COMPONENT AND NOT A LINE IN A SHORTHAND. `<image>` is the RESIDUE of every partition it appears
 * in: css-backgrounds-3 §2.10's `<bg-layer>` is a `||` whose other terms are keyword sets, lengths and a
 * `<color>`, so a component value that is none of those is an image OR the declaration is invalid, and there
 * is no third answer. That makes the production a VALIDITY test rather than a classification — and the cost of
 * not having one is not a missing feature, it is a WRONG one: a `background: bogus` whose unmatched component
 * fell through to the image slot would be a declaration CSS Syntax drops, kept, with a value no grammar
 * admits, exactly as `display: bogus` once reached a computed value through lexbor's `__UNDEF`.
 *
 * WHAT ASKS FOR IT BESIDES `background`. `list-style-image`, `border-image-source`, `cursor` and
 * `object-position`'s neighbours all name `<image>`, and css-images-3 §2's own sentence names the first three;
 * a copy per shorthand is what disagrees about `radial-gradient(circle at left, red, blue)` the day one of
 * them is edited.
 *
 * THE ANSWER IS A BOOLEAN AND THE VALUE IS THE AUTHOR'S OWN BYTES, deliberately. §2's last sentence makes the
 * computed value "the specified value with any <url>s, <color>s, and <length>s computed", so the SPECIFIED
 * value — which is the layer this whole cascade is stated over — is what was written, and CSSOM §6.7.2
 * "Serializing CSS Values" serializes it back. A canonicalizing answer here would be this component inventing
 * a spelling for a value whose own module has not stated one.
 *
 * WHAT IS NOT COVERED, BY NAME. The production is css-images-3 §2's `<image> = <url> | <gradient>`, which is
 * the level css-backgrounds-3 normatively references. css-images-4 §2 "2D Image Values: the <image> type"
 * widens it to `<url> | <image()> | <image-set()> | <cross-fade()> | <element()> | <gradient>`, and THOSE FOUR
 * EXTRA ARMS ARE WHAT IS MISSING: each of those function names reaches a DFAIL naming its own section rather
 * than being refused, because refusing would DROP a declaration that is valid CSS and the drop is silent — the
 * page's `background` would read as undeclared, with the property's initial value to show for it.
 *
 * `<gradient>` ITSELF IS READ AT LEVEL 4, and the split is deliberate rather than an oversight. §3 "Gradients"
 * there has SIX notations rather than four; §3.1 and §3.2.1 "Adding <color-interpolation-method>" hang that
 * production off the linear and radial families; and §3.5.1 "Color Stop Lists" gives every colour stop a
 * second optional position. Every one of those is a value each of the major engines accepts today, so a
 * component that refused them in order to be consistent about a level number would be dropping valid
 * declarations — which is the same silent failure the paragraph above describes, chosen on purpose. The four
 * arms are absent because they are NOT BUILT; the gradients are level 4's because they ARE.
 *
 * THE TWO ARMS ARE EXPORTED AS A KIND, AND THE BOOLEAN IS NOW A PREDICATE OVER IT rather than a second scan.
 * Nothing outside this file could ask WHICH arm a component value took, and one caller needs to:
 * css-lists-3 §3.3 "Image Markers: the list-style-image property"' `Computed value:` line is "the keyword
 * noneor the computed <image>" — the absent space is the DRAFT'S OWN, §3.3's table closing an `<a>` element
 * directly against the next word, so it must not be "corrected" here or the quotation stops matching the only
 * text that can judge it. That second phrase is not ONE answer in this engine. css-images-3 §2's own
 * sentence above computes `<url>`s, `<color>`s and `<length>`s; a `<url>` this engine cannot resolve HAS a
 * defined computed value and it is the SPECIFIED one, by css-values-4 §4.5.1 "Relative URLs"' last sentence
 * — "The computed value of a URL that the UA cannot resolve to an absolute URL is the specified value" — over
 * the DECISION core/css/css_font_src.h records, which holds a url as the page spelled it and assigns
 * resolution to the fetching component. So the `<url>` arm is as-specified HERE and the `<gradient>` arm is
 * not: a `linear-gradient(red, blue)` has a `<color>` to compute, and reporting the author's bytes for it
 * would be a specified value under the word computed. The split is therefore a GRAMMAR question this file
 * already decides internally and no other file can ask, which is why the entry lands before any row does.
 *
 * IT ANSWERS A KIND AND NOT BYTES, which is the paragraph above applied and not a second decision: the
 * specified value is the author's own bytes and every caller is already holding them, so an entry returning a
 * string would be this component inventing the spelling that paragraph refuses to invent — and the `<color>`s
 * and `<length>`s a computed `<gradient>` needs are core/css/css_color.h's and core/css/css_length.h's to
 * compute, each over a value this file has only validated.
 *
 * ONE WALK, TWO QUESTIONS, AND NO SECOND BIT TO DRIFT. `css_image_is_image` is DEFINED as an inequality over
 * `css_image_kind` rather than re-deciding the same component value, so the two cannot disagree by
 * construction and no assert is owed for an agreement nothing can break. The boolean keeps its five call
 * sites, which is the point of two predicates over one fact rather than one entry answering both. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_IMAGE_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_IMAGE_H
#include <stdbool.h>
#include <stddef.h>

/* WHICH ARM of css-images-3 §2's `<image> = <url> | <gradient>` the ONE component value `text`/`len` took. */
typedef enum {
    /* NOT an `<image>` — a REFUSAL, and never css-lists-3 §3.3 "Image Markers: the list-style-image
       property"' `none`, which belongs to a PROPERTY's own grammar and not to this production:
       css-backgrounds-3 §2.3 "Image Sources: the background-image property" states `<bg-image> = <image> |
       none` for itself, exactly as the note on `css_image_is_image` below records. ZERO so that an unset
       variable refuses rather than being admitted.
       IN A RELEASE BUILD css-images-4 §2's FOUR EXTRA ARMS REACH THIS, and that is the arm beneath the banner's
       `DFAIL` rather than a classification made here: the crash is compiled out there, so `image-set(...)`
       leaves this walk with the refusal and the banner's silent drop is what happens. A consumer that maps
       this member to a property's `none` keyword would additionally report that drop AS a declared `none`,
       which is a second property reached by the same gap — BUILD the arm the `DFAIL` names. */
    CSS_IMAGE_NOT_AN_IMAGE = 0,
    CSS_IMAGE_URL,        /* the `<url>` arm, css-values-4 §4.5 "Resource Locators: the <url> type" */
    CSS_IMAGE_GRADIENT    /* the `<gradient>` arm, read at css-images-4 §3 "Gradients" — see the banner */
} CssImageKind;

/* The span is a single component value — CSS Syntax §4 makes a function token one however many commas its
   arguments carry — neither NUL-terminated nor lowercased. A `<gradient>` notation whose own arguments do not
   match its production is CSS_IMAGE_NOT_AN_IMAGE and not CSS_IMAGE_GRADIENT: `linear-gradient(, red, blue)`
   names a gradient and is not one, and there is no third answer for it to have. */
CssImageKind css_image_kind(const char *text, size_t len);

/* Does the ONE component value `text`/`len` match css-images-3 §2's `<image>`? THIS IS A PREDICATE OVER
   `css_image_kind` AND NOT A WALK OF ITS OWN — see the banner's one-walk paragraph — so a caller that needs
   only the validity test asks here and one that needs the arm asks above, over the one fact.
   FALSE for `none`, which is NOT an `<image>`: css-backgrounds-3 §2.3 "Image Sources: the background-image
   property" states its own `<bg-image> = <image> | none`, so the keyword belongs to the property's grammar and
   a caller that folded it in here would admit `none` in every context that names `<image>` alone. */
bool css_image_is_image(const char *text, size_t len);

#endif
