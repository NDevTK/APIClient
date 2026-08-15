/* HTMLMediaElement, MediaError and TimeRanges — HTML §4.8.11. See media_element.c for the model. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_MEDIA_ELEMENT_H
#define ENGINE_HOST_BROWSER_CORE_HTML_MEDIA_ELEMENT_H

#include <stdbool.h>

#include "quickjs.h"

/* Declared ONCE PER AGENT, from html_element_init — the interfaces, the reflections §4.8.11 puts on
   HTMLMediaElement rather than on the two element interfaces that inherit them, and the step machines. */
void media_element_declare(JSContext *ctx);
void media_element_free(JSContext *ctx);

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

#endif
