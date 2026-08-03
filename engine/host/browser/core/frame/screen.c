/* THE SCREEN INTERFACE — Blink core/frame, the output-device half of the browsing context.
 *
 * EVERY MEMBER HERE IS THE ENVIRONMENT, so every member here is CONCOLIC with a real display's value as its
 * example. That is not a hedge, it is the whole point of the interface for this tool: `screen.width < 768` is
 * THE mobile gate, and a responsive bundle puts a different router, a different asset host and frequently a
 * different API base behind each side of it. Pinning the width to one number picks one arm and deletes the
 * other's endpoints — the same loss `navigator.userAgent` would have taken.
 *
 * There is no member of Screen that a spec fixes the way HTML fixes navigator.appName, so unlike Navigator
 * this file has no concrete half. `availWidth`/`availHeight` are separate sources from `width`/`height`: a
 * bundle that compares them is asking whether the OS reserves chrome (a taskbar), which is a different question
 * with its own two answers, and one shared source would tie the two branches together.
 *
 * `orientation`, `isExtended` and `onchange` are honestly ABSENT — the Screen Orientation API is its own
 * interface with its own state machine, and the IDL audit names them until it exists. */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "solver/concolic.h"
#include "core/frame/screen.h"

/* An environment member: opaque for control flow, carrying what a real display reports. One helper, so a member
   added later cannot quietly arrive as bare-concrete. */
static void screen_env(JSContext *ctx, JSValueConst scr, const char *name, JSValue example)
{
    char path[64];
    JSValue v;

    DCHECK(strlen(name) + 8 < sizeof(path), "a Screen member name longer than any in the IDL");
    snprintf(path, sizeof(path), "screen.%s", name);
    v = concolic_new(ctx, path, path, example);
    CHECK(!JS_IsException(v), "minting a Screen environment value failed");
    JS_SetPropertyStr(ctx, (JSValue)scr, name, v);
}

void screen_install(JSContext *ctx, JSValueConst global)
{
    JSValue scr;

    DCHECK(JS_IsObject(global), "screen_install was given something that is not the global object");

    scr = JS_NewObject(ctx);
    CHECK(!JS_IsException(scr), "the Screen allocation failed");

    /* A common desktop display. The examples decide what the code COMPUTES; the fork is what stops them
       deciding which code is reached. */
    screen_env(ctx, scr, "width",  JS_NewInt32(ctx, 1920));
    screen_env(ctx, scr, "height", JS_NewInt32(ctx, 1080));
    /* Separate sources from width/height on purpose: `screen.availHeight < screen.height` is the "is there a
       taskbar" question, and sharing one source would make that branch answer the size branch. */
    screen_env(ctx, scr, "availWidth",  JS_NewInt32(ctx, 1920));
    screen_env(ctx, scr, "availHeight", JS_NewInt32(ctx, 1040));
    screen_env(ctx, scr, "colorDepth", JS_NewInt32(ctx, 24));
    screen_env(ctx, scr, "pixelDepth", JS_NewInt32(ctx, 24));

    JS_SetPropertyStr(ctx, (JSValue)global, "screen", scr);
}
