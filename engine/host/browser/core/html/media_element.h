/* HTMLMediaElement, MediaError and TimeRanges — HTML §4.8.11. See media_element.c for the model. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_MEDIA_ELEMENT_H
#define ENGINE_HOST_BROWSER_CORE_HTML_MEDIA_ELEMENT_H

#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "quickjs.h"
#include "core/mime/mime_type.h"

/* WHICH TYPES THIS BUILD'S MODELLED MEDIA DEVICE RENDERS — the UA CAPABILITY behind §4.8.11.3's canPlayType,
 * exported because a SECOND standard asks the same question of the same device and a second list would be a
 * second answer. MIME Sniffing §7 steps 5 and 7 gate their pattern matching on "an image / audio or video MIME
 * type SUPPORTED BY THE USER AGENT", and core/mime deliberately does not own that conjunct: §4.6's GROUPS say
 * what a byte stream IS, and what a build can decode is this component's fact.
 * It is not canPlayType's answer narrowed to a bool — canPlayType additionally distinguishes "probably" from
 * "maybe" by the `codecs` parameter, which is a question about how much the device knows and not about whether
 * it renders the container at all. `m` must be a record §4.4 produced. */
bool media_device_renders(const MimeType *m);

/* Declared ONCE PER AGENT, from html_element_init — the interfaces, the reflections §4.8.11 puts on
   HTMLMediaElement rather than on the two element interfaces that inherit them, and the step machines. */
void media_element_declare(JSContext *ctx);
void media_element_free(JSRuntime *rt);

/* PER REALM. Builds HTMLMediaElement.prototype over `html_proto` (§4.8.11's `interface HTMLMediaElement :
   HTMLElement`), MediaError.prototype and TimeRanges.prototype. It must run BEFORE the per-tag prototypes,
   because HTMLAudioElement's and HTMLVideoElement's are built ON this one — which is what the IDL says and
   what makes `audio.play` a property of HTMLMediaElement.prototype rather than of two unrelated objects. */
void media_element_install_proto(JSContext *ctx, JSValueConst html_proto);
/* HTMLMediaElement.prototype for THIS realm, OWNED — html_element.c's table asks for it as the parent of the
   two interfaces that inherit it. */
JSValue media_element_proto(JSContext *ctx);
/* The interface objects — `HTMLMediaElement`, `MediaError`, `TimeRanges` — on this realm's global. */
void media_element_install(JSContext *ctx, JSValueConst global);

/* §4.8.11.2's SECOND sentence — "if a src attribute of a media element is set or changed, the user agent must
   invoke the media element's media element load algorithm" — as one of §4.9's attribute change steps, which is
   the chokepoint every spelling of that write reaches. `val` is the NEW value, NULL for a removal, which the
   parenthetical after that sentence makes the whole question this asks of it. */
void media_element_attr_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local,
                                const char *val);
/* §4.8.11.2's FIRST sentence — "if a media element is created with a src attribute, the user agent must
   immediately invoke the media element's resource selection algorithm" — for the elements a PARSE created with
   their attributes, which lexbor builds with no per-token hook to run it at. Called at the two seams this
   engine already treats as a parse boundary: the document's, and every fragment's. Seeds one job per media
   element that carries a `src` OR has a `source` element child — the second being §4.8.12's source element
   insertion steps for the same parse, since a parser-inserted `<source>` reaches no mutation chokepoint.
   Costs a tag test per node and allocates for nothing else. */
void media_element_parsed(JSContext *ctx, lxb_dom_node_t *root);
/* §4.8.12's SOURCE HTML ELEMENT INSERTION STEPS — "let parent be insertedNode's parent; if parent is a media
   element that has no src attribute and whose networkState has the value NETWORK_EMPTY, then invoke that media
   element's resource selection algorithm". Called from core/dom/element.c's §4.2.3 insertion-steps drain,
   beside the other HTML element insertion steps and in the inserted node's own document realm; it asks whether
   `el` is a `source` itself, so the drain states no brand it would have to keep in step with this file. */
void media_element_source_inserted(JSContext *ctx, lxb_dom_element_t *el);

#endif
