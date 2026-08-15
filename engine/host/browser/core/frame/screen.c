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

/* THE MODELLED DISPLAY'S BIT DEPTH, stated ONCE. It is `screen.colorDepth`'s example and it is also what MEDIA
   QUERIES §4.5's `color` feature reports — as bits per COLOR COMPONENT, which is this divided by the three
   components of an RGB display. Two readers of one fact, so the fact is a constant here rather than a second
   number written into the media-feature table (CLAUDE.md §per-realm: one fact answered from two places is the
   defect, whatever the places are). */
#define SCREEN_COLOR_DEPTH 24

/* THE MODELLED DISPLAY'S GEOMETRY, stated ONCE, for the same reason the depth is — see screen.h for the three
   standards that read it. A common desktop display, with 40 CSS pixels of the height reserved by the operating
   system: that is what makes `availHeight < height` a question with two sides rather than a tautology. */
#define SCREEN_WIDTH        1920.0
#define SCREEN_HEIGHT       1080.0
#define SCREEN_AVAIL_WIDTH  1920.0
#define SCREEN_AVAIL_HEIGHT 1040.0

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

int screen_color_depth(void)
{
    return SCREEN_COLOR_DEPTH;
}

double screen_width(void)        { return SCREEN_WIDTH; }
double screen_height(void)       { return SCREEN_HEIGHT; }
double screen_avail_width(void)  { return SCREEN_AVAIL_WIDTH; }
double screen_avail_height(void) { return SCREEN_AVAIL_HEIGHT; }

void screen_install(JSContext *ctx, JSValueConst global)
{
    JSValue scr;

    DCHECK(JS_IsObject(global), "screen_install was given something that is not the global object");
    /* §2.3's AVAILABLE area is a SUB-AREA of the screen area — a modelled display whose available half is the
       larger of the two is not a display any UA could report, and every consumer that positions something
       inside the available area (viewport.c's client window) would then place it off the screen. */
    DCHECK(SCREEN_AVAIL_WIDTH <= SCREEN_WIDTH && SCREEN_AVAIL_HEIGHT <= SCREEN_HEIGHT,
           "the modelled Web-exposed AVAILABLE screen area is larger than the Web-exposed screen area it is "
           "part of");

    scr = JS_NewObject(ctx);
    CHECK(!JS_IsException(scr), "the Screen allocation failed");

    /* The modelled display, out of the one statement of it above. The examples decide what the code COMPUTES;
       the fork is what stops them deciding which code is reached. */
    screen_env(ctx, scr, "width",  JS_NewInt32(ctx, (int)SCREEN_WIDTH));
    screen_env(ctx, scr, "height", JS_NewInt32(ctx, (int)SCREEN_HEIGHT));
    /* Separate sources from width/height on purpose: `screen.availHeight < screen.height` is the "is there a
       taskbar" question, and sharing one source would make that branch answer the size branch. */
    screen_env(ctx, scr, "availWidth",  JS_NewInt32(ctx, (int)SCREEN_AVAIL_WIDTH));
    screen_env(ctx, scr, "availHeight", JS_NewInt32(ctx, (int)SCREEN_AVAIL_HEIGHT));
    screen_env(ctx, scr, "colorDepth", JS_NewInt32(ctx, SCREEN_COLOR_DEPTH));
    screen_env(ctx, scr, "pixelDepth", JS_NewInt32(ctx, SCREEN_COLOR_DEPTH));

    JS_SetPropertyStr(ctx, (JSValue)global, "screen", scr);
}
