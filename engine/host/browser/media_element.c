/* HTMLImageElement / HTMLAudioElement / HTMLOptionElement — see media_element.h. Replaces a wrong shared ctor
 * that created an <img> for Image AND Audio AND Option and gave audio no behavior.
 *
 * `new Audio()` is a real HTMLMediaElement STATE MACHINE that PLAYS with no audio device (headless is not
 * valueless — the spec defines the behavior without a device): a fresh media element is paused; play() begins
 * playback (paused=false) and resolves Promise<undefined> per spec; pause() re-pauses. The state lives on the
 * element's JS object, so it is per-flow (the JS heap COW) — a flow that calls play() reads paused=false, and
 * that isolates across flows like any other write. A gate `if (audio.paused) …` therefore sees the REAL
 * modeled state, never an opaque shrug. */
#include "media_element.h"
#include "dom_element.h"   /* el_wrap — the element JS wrapper */
#include "check.h"         /* DCHECK — the document exists before any constructor runs */
#include <string.h>
#include <lexbor/html/html.h>

extern lxb_html_document_t *g_dom;                          /* the live parsed document (main.c) */
extern JSValue js_resolved(JSContext *ctx, JSValue val);    /* scheduler-side: wrap in a resolved promise */

static JSValue make_el(JSContext *ctx, const char *tag, size_t tl) {
    DCHECK(g_dom, "media-el: g_dom NULL at construction — the document is created before boot, so this is impossible");
    lxb_dom_element_t *el = lxb_dom_document_create_element(lxb_dom_interface_document(g_dom), (const lxb_char_t *)tag, tl, NULL);
    DCHECK(el, "media-el: create_element failed — Lexbor creates any valid tag name");
    return el_wrap(ctx, el);
}

/* HTMLMediaElement.play(): begin playback with no device -> paused=false, resolve Promise<undefined> (spec). */
static JSValue media_play(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    JS_SetPropertyStr(ctx, this_val, "paused", JS_FALSE);
    return js_resolved(ctx, JS_UNDEFINED);
}
/* HTMLMediaElement.pause(): -> paused=true. */
static JSValue media_pause(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    JS_SetPropertyStr(ctx, this_val, "paused", JS_TRUE);
    return JS_UNDEFINED;
}
static JSValue media_noop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv; return JS_UNDEFINED;   /* load()/addTextTrack — no-op, no device */
}
static JSValue media_can_play(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv; return JS_NewString(ctx, "maybe");   /* canPlayType: a defined, plausible engine answer */
}

JSValue js_image_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    (void)nt; (void)argc; (void)argv;
    return make_el(ctx, "img", 3);
}
JSValue js_option_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    (void)nt; (void)argc; (void)argv;
    return make_el(ctx, "option", 6);
}
JSValue js_audio_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv) {
    (void)nt;
    JSValue el = make_el(ctx, "audio", 5);
    lxb_dom_element_t *lel = JS_GetOpaque(el, g_el_class_id);
    if (lel && argc >= 1) {   /* new Audio(src): reflect the src attribute (a media asset, not an endpoint) */
        const char *s = JS_ToCString(ctx, argv[0]);
        if (s && s[0]) lxb_dom_element_set_attribute(lel, (const lxb_char_t *)"src", 3, (const lxb_char_t *)s, strlen(s));
        if (s) JS_FreeCString(ctx, s);
    }
    /* HTMLMediaElement state machine (modeled, no device) — on the JS object, so per-flow via the heap COW. */
    JS_SetPropertyStr(ctx, el, "paused", JS_TRUE);            /* a fresh media element is paused until play() */
    JS_SetPropertyStr(ctx, el, "ended", JS_FALSE);
    JS_SetPropertyStr(ctx, el, "currentTime", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, el, "play", JS_NewCFunction(ctx, media_play, "play", 0));
    JS_SetPropertyStr(ctx, el, "pause", JS_NewCFunction(ctx, media_pause, "pause", 0));
    JS_SetPropertyStr(ctx, el, "load", JS_NewCFunction(ctx, media_noop, "load", 0));
    JS_SetPropertyStr(ctx, el, "canPlayType", JS_NewCFunction(ctx, media_can_play, "canPlayType", 1));
    return el;
}
