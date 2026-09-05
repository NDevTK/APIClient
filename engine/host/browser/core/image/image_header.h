/* THE NATURAL DIMENSIONS OF AN ENCODED IMAGE, READ FROM ITS OWN HEADER — css-images-3 §4.1 "Object-Sizing
 * Terminology"'s natural width and height, obtained from the bytes a fetch returned and from nothing else.
 *
 * WHY THIS EXISTS, AND WHY IT IS NOT A DECODER. HTML §4.8.4.3.5 "Updating the image data" ends in a three-way
 * jump over the response, and every arm of the middle one — "If the resource type and data corresponds to a
 * supported image format" — is conditioned on one capability and only one: "the user agent is able to
 * determine image request's image's width and height". NOT on decoding a pixel. An agent that can answer that
 * question reaches `partially available`, `completely available` and the `load` event; an agent that cannot
 * takes the third arm for every reply and fires `error` at every image ever fetched, which is what this build
 * did. The dimensions of a PNG, a GIF and a JPEG are STATED IN THEIR HEADERS as integer fields at defined
 * offsets, so determining them is a header read. That is what CLAUDE.md §Headless-is-not-valueless asks for —
 * the missing piece is a physical IO device and the spec still defines the behaviour without one — and it is
 * the same answer the mock filesystem gives: model the state, do not shrug to opaque.
 *
 * WHAT IS DELIBERATELY NOT HERE. No pixel decode, no colour management, no interlace pass reconstruction —
 * none of those is an operand of anything §4.8.4.3.5, §4.8.3's `naturalWidth` or HTML §15.4.2 "Images" reads.
 * A component that decoded pixels would be answering a question no caller in this engine asks.
 *
 * THE SIGNATURE TABLE IS NOT DUPLICATED HERE. Which byte pattern is which image type is MIME Sniffing §6.1
 * "Matching an image type pattern", and core/mime/mime_sniff.h already holds that table cell for cell. A
 * second copy would be two tables free to drift into disagreeing about what a PNG is — the same defect
 * core/html/html_image.h names for the image request STATE, where it rejects a private copy of the four
 * values in the layout as two enumerations free to drift apart. So this component asks §6.1 what the bytes
 * ARE and owns only the part §6.1 does not answer: where each format states its dimensions.
 *
 * THE BYTES ARE A STRANGER'S AND ARE NEVER ASSERTED. CLAUDE.md's rule for a value this codebase did not
 * compute is that it is INPUT and not an invariant: a malformed header is REFUSED — `supported` or `have_dims`
 * false, which §4.8.4.3.5 has a named arm for — and never a `DCHECK`, because asserting on a response body
 * hands any server on the internet an abort switch for this engine. Every assert below is about what a CALLER
 * of this file passed in, which is a value this codebase computed.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_IMAGE_IMAGE_HEADER_H
#define ENGINE_HOST_BROWSER_CORE_IMAGE_IMAGE_HEADER_H

#include <stdbool.h>
#include <stddef.h>

/* WHAT A HEADER READ ANSWERS — ONE FACT AND A VALUE, and the single flag is deliberate.
   AN EARLIER SHAPE OF THIS STRUCT CARRIED A SECOND FLAG, `supported`, to tell HTML §4.8.4.3.5's two failing
   arms apart: its outer "Otherwise" ("The image data is not in a supported file format") from the inner "the
   user agent is able to determine that image request's image is CORRUPTED IN SOME FATAL WAY such that the
   image dimensions cannot be obtained". Both arms end in `broken` and an `error` event, so NOTHING READ IT —
   and a field a producer writes and no consumer reads is the mirror of the defect CLAUDE.md counts seven of,
   whatever future arm it is being kept for. It is gone rather than kept: the day an arm needs the
   distinction, that arm and this field arrive together, and until then this struct states what it can answer.
   THE DIMENSIONS ARE `double` BECAUSE css-images-3 §4.1's NATURAL DIMENSIONS ARE, and because the one consumer
   that does arithmetic on them (core/layout/replaced_element.h's `CssPx`) is a floating type. Every format
   below states them as integers, so no precision is lost crossing into this. */
typedef struct {
    bool   have_dims;   /* css-images-3 §4.1's natural width and height were read out of the header */
    double width;
    double height;
} ImageHeader;

/* READ THEM. `bytes`/`n` are a response body as core/fetch/fetch.h hands it over — a byte sequence, never a
   string, because a decode would have destroyed the very fields this reads. A zero-length body is a complete
   and ordinary answer (`supported` false), not a hole, so no caller needs a null test for it.
   IT IS A PURE FUNCTION OF THE BYTES: no realm, no element, no flow, nothing captured into a COW delta. That
   is what makes it callable from a reply delivery that must not run any of the page's code. */
ImageHeader image_header_read(const unsigned char *bytes, size_t n);

#endif
