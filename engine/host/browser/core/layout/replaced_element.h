/* Whether an element is replaced and what its natural dimensions are: HTML §15.4 "Replaced elements" and
 * css-images-3 §4.1 "Object-Sizing Terminology", the two facts CSS 2.1 §10.3.2 "Inline, replaced elements"
 * needs from outside CSS. CSS 2.1 §3.1 leaves which elements are replaced to the user agent; HTML §15.4 names
 * the elements that can be, and §15.4.1 "Embedded content" and §15.4.2 "Images" decide which one is now, from
 * state that changes as replies land (an `img` request's state and `alt`). It is one component because
 * core/css/css_property_applies.c and core/layout/used_value.c both ask. css-display's Appendix B list is a
 * different, wider question (it holds `select`, `textarea`, and every `img`) and must not stand in for this.
 *
 * Each natural dimension "may or may not exist", and §10.3.2's arms branch on which exist, so each carries a
 * presence flag: a natural width of 0 (§15.4.2's `img` that represents nothing) is a real answer. "A
 * degenerate natural aspect ratio (at least one part being zero or infinity)" is treated as no ratio.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_LAYOUT_REPLACED_ELEMENT_H
#define ENGINE_HOST_BROWSER_CORE_LAYOUT_REPLACED_ELEMENT_H

/* The dispatch is total: every element §15.4 lists leaves with a derived answer, in release as in dev. An
   `iframe` has "no natural dimensions at all" (so §10.3.2 gives 300 x 150, which core/frame/viewport.c uses);
   an `img` is read from core/html/html_image.h's request state; an `embed` represents nothing (every arm of
   §4.8.6's type steps needs a plugin); a `video` lacks a natural width and height; a `canvas` uses its
   attributes with defaults 300 and 150; an `object` represents its fallback content; an `audio` exposes no
   user interface. Their narrower-than-spec parts are named residuals at each arm. Two arms are fatal in both
   builds: an `input` in the Image Button state, and an `img` in a document no navigable presents. */

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "core/css/css_length.h"

/* HTML §15.4's classification of one element at this moment, with css-images-3 §4.1's three natural
   dimensions. When `replaced` is false the other fields are zeroed and meaningless (§10.3.3 owns that box).
   Values are `CssPx` so they carry css_length.h's environment facts into the used-value arithmetic; a decoded
   image's own dimensions arrive with `CSS_ENV_NONE`, a single-point domain. */
typedef struct {
    bool   replaced;
    bool   has_width;    /* css-images-3 §4.1's natural width is present */
    bool   has_height;   /* its natural height is present */
    bool   has_ratio;    /* its natural aspect ratio is present and is not degenerate */
    CssPx  width;
    CssPx  height;
    double ratio;        /* the ratio between the width and height (§4.1), so width = height * ratio */
} ReplacedElement;

ReplacedElement replaced_element_of(lxb_dom_element_t *el);

#endif
