/* The image of an `input` in HTML §4.10.5.1.19 "Image Button state (type=image)": the fetch that state's
   steps make, its `load` and `error` element tasks, and the "available" fact HTML §15.4.2 "Images" reads.
   Chromium's ImageInputType. The image is a record on the element's own wrapper, so it is per flow. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_IMAGE_INPUT_TYPE_H
#define ENGINE_HOST_BROWSER_CORE_HTML_IMAGE_INPUT_TYPE_H

#include <stdbool.h>
#include <stdint.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"

/* What §4.10.5.1.19 knows about the image of one element. `FETCHING` is §15.4.2's second rule's "reason to
   believe that the image will become available": a fetch whose processResponseEndOfBody has not run. */
typedef enum {
    IMAGE_INPUT_NO_IMAGE = 0,   /* no fetch for the current src: never triggered, empty, or not a URL */
    IMAGE_INPUT_FETCHING,
    IMAGE_INPUT_AVAILABLE,      /* "the image is said to be available" */
    IMAGE_INPUT_UNAVAILABLE     /* fetched, and not a valid image of a supported type */
} ImageInputImage;

/* The image of Image Button `el`, read without creating the record (a layout read writes nothing into the
   running flow's delta). For IMAGE_INPUT_AVAILABLE, `*w` and `*h` are the header's natural dimensions in
   CSS pixels; otherwise they are untouched. `ctx` is the element's node document's realm. */
ImageInputImage image_input_image(JSContext *ctx, lxb_dom_element_t *el, double *w, double *h);

/* The available image's pixels, non-premultiplied RGBA8, in a buffer the caller frees; NULL when there are
   none to composite (not available, or bytes no decoder here reads). */
uint8_t *image_input_decoded_rgba(JSContext *ctx, lxb_dom_element_t *el, uint32_t *w, uint32_t *h);

/* §4.10.5.1.19's three trigger events that a content attribute write is: `type` entering the Image Button
   state, and `src` set or changed while in it. One of DOM §4.9's attribute change steps; it asks for itself
   whether `el` is an `input`. */
void image_input_attr_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local);

/* The same trigger for elements a parse created ("possibly when the element is first created"), which reach
   no attribute change step. `target` is the node the parsed tree is for: the Document for a document load,
   the fragment's target for a fragment parse; its node document decides whether anything is fetched. */
void image_input_parsed(JSContext *ctx, lxb_dom_node_t *root, lxb_dom_node_t *target);

void image_input_declare(JSContext *ctx);
void image_input_free(JSRuntime *rt);

#endif
