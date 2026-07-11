/* HTMLImageElement / HTMLAudioElement / HTMLOptionElement — see media_element.h. Replaces a wrong shared ctor
 * that created an <img> for Image AND Audio AND Option and gave audio no behavior.
 *
 * `new Audio()` is a real HTMLMediaElement STATE MACHINE that PLAYS with no audio device (headless is not
 * valueless — the spec defines the behavior without a device): a fresh media element is paused; play() begins
 * playback (paused=false) and resolves Promise<undefined> per spec; pause() re-pauses. The state lives on the
 * element's JS object, so it is per-flow (the JS heap COW) — a flow that calls play() reads paused=false, and
 * that isolates across flows like any other write. A gate `if (audio.paused) …` therefore sees the REAL
 * modeled state, never an opaque shrug. */
#include "core/html/media_element.h"
#include "core/dom/dom_element.h"   /* el_wrap — the element JS wrapper */
#include "check.h"         /* DCHECK — the document exists before any constructor runs */
#include "solver/concolic.h"        /* js_concolic — currentTime/ended after play() are unknowable headless -> fork */
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

/* HTMLMediaElement.play(): begin playback with no device -> paused=false, resolve Promise<undefined> (spec).
   Playing with no clock/device advances currentTime by an UNKNOWABLE amount and playback MAY reach the end, so
   currentTime + ended become CONCOLIC — a gate `if (el.ended)` / `if (el.currentTime > 5)` then FORKS both
   worlds (reaching the on-ended / seek-gated shipped arm), never a bare-concrete 0/false that buries it. */
static JSValue media_play(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    JS_SetPropertyStr(ctx, this_val, "paused", JS_FALSE);
    JS_SetPropertyStr(ctx, this_val, "currentTime", js_concolic(ctx, "{currentTime}", JS_UNDEFINED));
    JS_SetPropertyStr(ctx, this_val, "ended", js_concolic(ctx, "{mediaEnded}", JS_UNDEFINED));
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
    (void)this_val; (void)argc; (void)argv;
    /* Codec support is genuinely unknowable headless, so canPlayType is CONCOLIC, not bare-concrete: a
       feature-detection branch `if (v.canPlayType(t) === 'probably')` must FORK both worlds (supported +
       unsupported, reaching the codec-gated shipped arm) while carrying "maybe" as the concrete example.
       A fixed "maybe" would silently take only the truthy arm and bury the fallback path. */
    return js_concolic(ctx, "{canPlayType}", JS_NewString(ctx, "maybe"));
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
