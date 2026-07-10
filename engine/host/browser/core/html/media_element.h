/* HTMLImageElement / HTMLAudioElement / HTMLOptionElement constructors — Blink core/html/*. Each builds its
 * CORRECT element (the old shared ctor made an <img> for all three), and `new Audio()` is a real
 * HTMLMediaElement state machine that plays with no audio device. See media_element.c. */
#ifndef ENGINE_HOST_BROWSER_MEDIA_ELEMENT_H
#define ENGINE_HOST_BROWSER_MEDIA_ELEMENT_H
#include "quickjs.h"
JSValue js_image_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv);    /* new Image() -> <img> */
JSValue js_audio_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv);    /* new Audio(src) -> <audio> + media state */
JSValue js_option_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv);   /* new Option() -> <option> */
#endif
