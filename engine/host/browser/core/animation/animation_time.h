/* Web Animations Level 1 §4.2 "Time values": the arithmetic and the order every timing algorithm in
 * core/animation/ runs over a time value.
 *
 * A time value is JS_NULL when unresolved (§6.1 "Time values in the programming interface" represents an
 * unresolved one as null), and otherwise a number or unknown external input: a timeline's time derives from the
 * event loop's clock, which an unknown timeout moves, and a page may assign an unknown to `startTime`. The
 * arithmetic runs the engine's own operator on such a value so the result keeps its provenance and its example.
 * The order is read, never asked: these algorithms run in plain C members that cannot fork, so a comparison over
 * an unknown this flow has not decided crashes naming the step machine to build. */
#ifndef ENGINE_HOST_BROWSER_CORE_ANIMATION_ANIMATION_TIME_H
#define ENGINE_HOST_BROWSER_CORE_ANIMATION_ANIMATION_TIME_H

#include <stdbool.h>

#include "quickjs.h"

/* Is `t` a resolved time value? Asserts it is null, a number or an unknown. */
bool animation_time_resolved(JSValueConst t);

/* `a − b`, `a + b`, `a × b` and `a / b` over two resolved time values (or a time value and a rate). OWNED;
   operands borrowed. */
JSValue animation_time_sub(JSContext *ctx, JSValueConst a, JSValueConst b);
JSValue animation_time_add(JSContext *ctx, JSValueConst a, JSValueConst b);
JSValue animation_time_mul(JSContext *ctx, JSValueConst a, JSValueConst b);
JSValue animation_time_div(JSContext *ctx, JSValueConst a, JSValueConst b);
/* The additive inverse, for §4.5.16's "-effective playback rate". OWNED. */
JSValue animation_time_neg(JSContext *ctx, JSValueConst a);

/* `a < b` and `a == b` over two resolved values, as this flow decides them. `site` is the caller's
   __FILE__ ":" line, which the crash over an undecided unknown names. */
bool animation_time_lt_at(JSContext *ctx, JSValueConst a, JSValueConst b, const char *site);
bool animation_time_eq_at(JSContext *ctx, JSValueConst a, JSValueConst b, const char *site);
#define ANIMATION_TIME_SITE2(x) #x
#define ANIMATION_TIME_SITE(x)  ANIMATION_TIME_SITE2(x)
#define animation_time_lt(ctx, a, b) animation_time_lt_at((ctx), (a), (b), __FILE__ ":" ANIMATION_TIME_SITE(__LINE__))
#define animation_time_eq(ctx, a, b) animation_time_eq_at((ctx), (a), (b), __FILE__ ":" ANIMATION_TIME_SITE(__LINE__))
#define animation_time_le(ctx, a, b) (!animation_time_lt((ctx), (b), (a)))

/* The max and min §4.6.5.3 and §4.7.2 evaluate. OWNED. */
JSValue animation_time_max(JSContext *ctx, JSValueConst a, JSValueConst b);
JSValue animation_time_min(JSContext *ctx, JSValueConst a, JSValueConst b);

#endif
