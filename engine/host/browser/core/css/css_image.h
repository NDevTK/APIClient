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
 * WHAT IS NOT COVERED, BY NAME. This is css-images-3's `<image>`, which is the level css-backgrounds-3
 * normatively references. css-images-4 §2 "2D Image Values: the <image> type" widens it to
 * `<url> | <image()> | <image-set()> | <cross-fade()> | <element()> | <gradient>` and its §3.3 "Conic
 * Gradients: the conic-gradient() notation" adds two more gradient notations; each of those function names
 * reaches a DFAIL naming its own section rather than being refused, because refusing would DROP a declaration
 * that is valid CSS and the drop is silent — the page's `background` would read as undeclared, with the
 * property's initial value to show for it. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_IMAGE_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_IMAGE_H
#include <stdbool.h>
#include <stddef.h>

/* Does the ONE component value `text`/`len` match css-images-3 §2's `<image>`? The span is a single component
   value — CSS Syntax §4 makes a function token one however many commas its arguments carry — neither
   NUL-terminated nor lowercased.
   FALSE for `none`, which is NOT an `<image>`: css-backgrounds-3 §2.3 "Image Sources: the background-image
   property" states its own `<bg-image> = <image> | none`, so the keyword belongs to the property's grammar and
   a caller that folded it in here would admit `none` in every context that names `<image>` alone. */
bool css_image_is_image(const char *text, size_t len);

#endif
