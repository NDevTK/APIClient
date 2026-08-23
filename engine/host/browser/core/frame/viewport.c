/* THE VIEWPORT — CSS 2.1 §9.1.1 / §10.1, and CSSOM VIEW §4's Window extensions over it. See viewport.h for why
   this is a modelled UA choice, why it is a component of its own, why it is answered per realm, and why §4's
   members are installed here rather than in window.c. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/document.h"
#include "core/frame/screen.h"
#include "core/frame/viewport.h"
#include "core/frame/window_proxy.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/concolic.h"

/* THE TOP-LEVEL TRAVERSABLE's viewport. A UA answering the question CSS hands it, exactly as rendering.c
   answers the refresh rate: 1280 x 720 is an ordinary desktop content area, and it is deliberately NOT
   screen.c's 1920 x 1080 — a window is not a screen, and a bundle that compares `innerWidth` against
   `screen.width` is asking a real question whose two sides must be able to differ. */
#define VIEWPORT_TOP_WIDTH   1280.0
#define VIEWPORT_TOP_HEIGHT   720.0

/* A CHILD NAVIGABLE's viewport is its container's CONTENT BOX, and for an `iframe` with no author size that
   box is CSS 2.1 §10.3.2's default for a replaced element with no intrinsic dimensions: 300 x 150. That is the
   spec's own number rather than a second UA choice, which is why it is stated here as a derivation and not as
   a preference.
   AN AUTHOR STYLE THAT RESIZES THE FRAME IS NOT MODELLED, and NAMING WHAT IS MISSING IS THE WHOLE POINT OF
   THIS PARAGRAPH: it used to say `iframe { width: 500px }` "is a cascade this engine has no CSS parser for, so
   the used width it produces does not exist to be read", and that stopped being true. There IS a cascade —
   core/css/css_style_declaration.c resolves an author declaration out of the style sheet OBJECTS §6.2's list
   holds — and there IS a used value over it: core/layout/used_value.h's `used_value_px(el, "width")`. A
   sentence that keeps advertising an absent component sends the next reader to build one that is already here,
   which is the failure mode CLAUDE.md §Disposition names for a stale DFAIL.
   WHAT IS ACTUALLY MISSING IS THE REACH, and it is a fact about this file rather than about CSS: the container
   ELEMENT is not obtainable from here. `viewport_width` is asked with a child realm's ctx, walks to its
   WindowProxy, and neither core/frame/window_proxy.h nor core/frame/navigable.h exports the navigable's
   container element — no member of either header names a `lxb_dom_element_t` at all. BUILD THAT: a navigable's
   container element beside its parent, and this entry becomes `used_value_px(container, "width")` with the
   300 x 150 below surviving only as §10.3.2's answer for a replaced element the cascade sized `auto`. */
#define VIEWPORT_CHILD_WIDTH  300.0
#define VIEWPORT_CHILD_HEIGHT 150.0

/* A modelled non-HiDPI display. See viewport.h. */
#define VIEWPORT_DPPX           1.0

/* THE CLIENT WINDOW — the browser window the top-level traversable is presented in, which is what CSSOM VIEW
   §4's `outerWidth`, `outerHeight`, `screenX` and `screenY` are all about. It is NOT the viewport: the viewport
   is the content area INSIDE it, and the difference is the UA's own chrome (a tab strip and a toolbar). A
   bundle that computes `outerHeight - innerHeight` is measuring exactly that, so the two must be able to
   differ; a client window equal to its viewport is the shrug this component exists not to make.
   THE SIZE IS DERIVED FROM THE VIEWPORT, in that direction, because the viewport is the load-bearing fact this
   file already states. There is no horizontal chrome on a desktop browser window, so only the height grows.
   IT IS THE SAME WINDOW FOR EVERY REALM OF THE PAGE. A child navigable has its own viewport and does NOT have
   its own client window — `iframe.contentWindow.screenX` is the position of the browser window, which is why
   these four take a realm only to be asked whether the document is being presented at all. */
#define VIEWPORT_CHROME_HEIGHT 74.0

/* IS THIS REALM's document IN the top-level traversable? Asked of the NAVIGABLE tree rather than remembered at
   install, because a realm is built before it is attached to anything and the answer is a fact about where the
   document sits. `window_proxy_top_navigable` answers the top-level traversable's proxy in every case
   INCLUDING the caller's own (window_proxy.h's note: `top` would answer the Window there, and a walk handed
   that reaches something that is not a proxy). */
static bool viewport_is_top(JSContext *ctx)
{
    JSValueConst self = document_window_proxy(ctx);
    JSValue top;
    bool is_top;

    DCHECK(window_proxy_is(self),
           "the viewport was asked for a realm whose document has no WindowProxy — every Document this agent "
           "holds is the active document of a navigable, and the navigable is what a viewport belongs to");
    top = window_proxy_top_navigable(ctx, self);
    DCHECK(window_proxy_is(top),
           "a navigable answered its top-level traversable with something that is not a WindowProxy");
    is_top = window_proxy_doc(top) == window_proxy_doc(self);
    JS_FreeValue(ctx, top);
    return is_top;
}

bool viewport_exists(JSContext *ctx)
{
    return document_fully_active(ctx);
}

double viewport_width(JSContext *ctx)
{
    return viewport_is_top(ctx) ? VIEWPORT_TOP_WIDTH : VIEWPORT_CHILD_WIDTH;
}

double viewport_height(JSContext *ctx)
{
    return viewport_is_top(ctx) ? VIEWPORT_TOP_HEIGHT : VIEWPORT_CHILD_HEIGHT;
}

double viewport_device_pixel_ratio(JSContext *ctx)
{
    (void)ctx;
    return VIEWPORT_DPPX;
}

/* CSS 2.1 §10.1: the initial containing block "has the dimensions of the viewport". */
CssPx viewport_icb_width(JSContext *ctx)
{
    return css_px_env(CSS_ENV_ICB_WIDTH, ctx, viewport_width(ctx));
}

CssPx viewport_icb_height(JSContext *ctx)
{
    return css_px_env(CSS_ENV_ICB_HEIGHT, ctx, viewport_height(ctx));
}

/* ONE ROW PER FACT, INDEXED BY THE FACT ITSELF — the one table over `CssEnvFact`, so a length mints its domain
   in one place whether it is a function of one fact or of three.
   THE SECOND COLUMN IS §4's "OR ZERO IF THERE IS NO VIEWPORT", asked per FACT because the facts differ on it
   and js_vp_get below already draws the same line at the same place: the INITIAL CONTAINING BLOCK has the
   dimensions of the viewport, so a document no navigable presents has none — while `devicePixelRatio`'s
   algorithm asks about the OUTPUT DEVICE, which exists whether or not this document is on it. One condition
   for both would either crash for a border width on a DOMParser element or wave through a length derived from
   a rectangle that does not exist. */
static const struct { const char *member; bool presented; } VIEWPORT_FACT[CSS_ENV_FACT_COUNT] = {
    [CSS_ENV_ICB_WIDTH]          = { "initialContainingBlock.width",  true  },
    [CSS_ENV_ICB_HEIGHT]         = { "initialContainingBlock.height", true  },
    [CSS_ENV_DEVICE_PIXEL_RATIO] = { "devicePixelRatio",              false },
};

/* THE SOURCE KEY, SPELLED ONCE — the document is part of it for media_query_list.c's reason (viewport.h), and
   both the member seam and the derived-length seam below compose it from here so a joint identity's members
   and a member read directly by a page cannot come out under two different names for one fact. */
#define VIEWPORT_SRC_MAX 128

static void viewport_src_key(JSContext *ctx, const char *member, char *out, size_t n)
{
    JSValueConst self = document_window_proxy(ctx);

    DCHECK(window_proxy_is(self),
           "a viewport-derived member was read in a realm whose document has no WindowProxy — a viewport "
           "belongs to a navigable, and every Document this agent holds is one's active document");
    DCHECK(strlen(member) + 24 < n, "a CSSOM VIEW member name longer than any in the IDL");
    snprintf(out, n, "{viewport#%u}%s", (unsigned)window_proxy_doc(self), member);
}

/* See viewport.h: the one place a length's fact SET becomes a domain a page can fork on. Every fact in the set
   contributes one member of solver/concolic.h's JOINT identity, in the table's own order — which the solver
   canonicalizes, so the identity is the set's and not this loop's. */
JSValue viewport_env_derived(CssPx len, JSValue computed)
{
    const char *shapes[CSS_ENV_FACT_COUNT];
    const char *srcs[CSS_ENV_FACT_COUNT];
    char key[CSS_ENV_FACT_COUNT][VIEWPORT_SRC_MAX];
    int f, n = 0;

    if (len.env == CSS_ENV_NONE) {
        DCHECK(len.realm == NULL,
               "a length that derives from NO environment fact carried a realm anyway — the two are written "
               "together by css_px_env and by nothing else, so one without the other is a length assembled "
               "field-by-field past that entry");
        return computed;
    }
    DCHECK(len.realm != NULL,
           "a length that is a function of an environment fact reached the boundary with no realm to answer it "
           "per — a child navigable's ICB is 300 CSS pixels wide and the top-level traversable's is 1280, so "
           "the fact alone does not say which question this is");
    DCHECK((len.env & ~CSS_ENV_ALL) == CSS_ENV_NONE,
           "a length carries a CssEnvFact this seam has no member name for. Every fact is one core/frame/"
           "viewport.h has decided is PICKED — css_length.h says so, and the test for one is this component's "
           "— so a new fact is a new row HERE, and a fact without one would cross to the page as a bare number "
           "with its domain dropped");
    for (f = 0; f < CSS_ENV_FACT_COUNT; f++) {
        if (!(len.env & CSS_ENV_BIT(f))) continue;
        DCHECK(VIEWPORT_FACT[f].member != NULL,
               "a fact this seam declares has no member name in its row — the table is indexed BY the fact, so "
               "a hole in it is a fact whose row was never written and whose domain would be spelled empty");
        DCHECK(!VIEWPORT_FACT[f].presented || viewport_exists(len.realm),
               "a length derived from the INITIAL CONTAINING BLOCK reached the page out of a realm whose "
               "document is not being presented. §10.1's ICB has the dimensions of the viewport, and "
               "viewport.h makes a document that is not fully active have none — so this length was derived "
               "from a rectangle that does not exist rather than from one whose size is a UA choice");
        viewport_src_key(len.realm, VIEWPORT_FACT[f].member, key[n], sizeof key[n]);
        shapes[n] = VIEWPORT_FACT[f].member;
        srcs[n] = key[n];
        n++;
    }
    DCHECK(n >= 1, "a non-empty fact set named no facts — the set and the table have come apart");
    return concolic_source_wrap_joint(len.realm, shapes, srcs, n, computed);
}

/* See viewport.h: the one seam, and the one speller of the key. */
JSValue viewport_env_value(JSContext *ctx, const char *member, JSValue computed)
{
    char src[VIEWPORT_SRC_MAX];

    viewport_src_key(ctx, member, src, sizeof src);
    return concolic_source_wrap(ctx, member, src, computed);
}

/* CSSOM VIEW §4: "the x-coordinate, RELATIVE TO THE INITIAL CONTAINING BLOCK ORIGIN, of the left of the
 * viewport". So the question is where the viewport sits over its own scrolling area, and this engine can
 * COMPUTE that rather than guess it.
 *
 * §2 defines a VIEWPORT's scrolling area as the initial containing block extended by the margin edges of all of
 * the viewport's DESCENDANTS' BOXES. This engine gives GEOMETRY to exactly one box — the ICB — so no descendant
 * has a margin edge to extend it by, the scrolling area is exactly the ICB, the ICB is exactly the viewport, and
 * a scrolling box whose scrolling area is its own size has one valid scroll position, its origin. The left of
 * the viewport therefore IS the ICB origin, for every flow at every moment, and 0 is a derived value rather than
 * an absent one.
 *
 * WHAT STOOD HERE SAID "THIS ENGINE GENERATES NO BOXES", which is a claim about box EXISTENCE and was the wrong
 * half: a user agent generates a box for the root element of a document it is presenting, this engine presents
 * every document it holds, and core/html/focus.c's §6.6.2 row 1 had already committed to the opposite answer
 * (a connected element that is not `hidden` IS being rendered). One fact with two answers — so the box model is
 * now stated in ONE place, core/dom/element_view.h, which owns the predicate both files ask. Existence is
 * decidable and geometry is not, and it is only geometry this derivation ever needed.
 *
 * THE MOMENT THERE IS A LAYOUT this stops being a derivation: the scrolling area becomes the union above, the
 * position becomes real state, and this becomes the read of what §3's perform-a-scroll wrote. The two-sided
 * assertion for that already exists and is in the right place — update-the-rendering step 9 (CSSOM VIEW §13.2's
 * SCROLL STEPS) is asserted against `scrollTo`, the member whose arrival means a scrolling box can be moved at
 * all, so the step that would have to drain doc's pending scroll events names itself first. §6's
 * `scrollTop`/`scrollLeft` setter reaches `viewport_scroll` below and moves nothing: its clamp collapses to the
 * position the viewport already has, which is asserted there. */
double viewport_scroll_x(JSContext *ctx)
{
    (void)ctx;
    return 0.0;
}

double viewport_scroll_y(JSContext *ctx)
{
    (void)ctx;
    return 0.0;
}

/* THE ATTRIBUTE, as opposed to the derivation above — see viewport.h for why the two are separate and why the
   member's "or zero if there is no viewport" is written once here rather than at each of its three callers. */
double viewport_window_scroll(JSContext *ctx, bool vertical)
{
    if (!viewport_exists(ctx)) return 0.0;
    return vertical ? viewport_scroll_y(ctx) : viewport_scroll_x(ctx);
}

/* §2's SCROLLING AREA OF A VIEWPORT — "the initial containing block extended by the margin edges of all of the
   viewport's DESCENDANTS' BOXES". The ICB is the viewport, and no box in this model has the geometry to extend
   it by (core/dom/element_view.h), so it is the viewport's own size.
   IT IS A FUNCTION AND NOT A CONSTANT because it is the fact TWO derivations read — the scroll position above
   and §4's clamp below, with CSSOM VIEW §6's `scrollWidth`/`scrollHeight` a third reader through viewport.h. The
   day a layout gives a descendant a margin edge, this grows, the clamp stops collapsing, and step 11's assert
   fires naming the perform-a-scroll steps that must then be written. Written as a constant in each place, that
   day would arrive silently in two of the three. */
double viewport_scrolling_area_width(JSContext *ctx)  { return viewport_width(ctx); }
double viewport_scrolling_area_height(JSContext *ctx) { return viewport_height(ctx); }

/* CSSOM VIEW §4's scroll() STEPS, as the internal algorithm §2 requires a caller to invoke — see viewport.h.
   The steps are written in the spec's own order and the whole of the work is the CLAMP: it is what turns an
   arbitrary requested position into one the viewport can actually have, and it is why a write that this engine
   cannot honour is a no-op DECIDED BY THE SPEC rather than a write dropped on the floor. */
void viewport_scroll(JSContext *ctx, double x, double y)
{
    double vw, vh, area_w, area_h;

    /* step 3: "If there is no viewport, return a resolved Promise and abort the remaining steps." */
    if (!viewport_exists(ctx)) return;
    /* steps 4-5: the viewport EXCLUDING the scroll bar, of which this user agent renders none. */
    vw = viewport_width(ctx);
    vh = viewport_height(ctx);
    area_w = viewport_scrolling_area_width(ctx);
    area_h = viewport_scrolling_area_height(ctx);
    /* STEPS 6-9 CLAMP, and this engine does not have to decide the OVERFLOW DIRECTIONS they are stated per:
       a rightward direction gives max(0, min(x, area - viewport)) and a leftward one min(0, max(x, viewport -
       area)), and with an area equal to the viewport both are the origin. The assert is what says the branch
       need not be decided; the day the area is bigger, the two arms differ and the root element's computed
       `writing-mode` and `direction` have to be read to tell them apart. */
    DCHECK(area_w == vw && area_h == vh,
           "CSSOM VIEW §4 scroll() steps 6-9 clamp per the viewport's OVERFLOW DIRECTIONS, and this engine "
           "answers them without deciding which they are only because the scrolling area equals the viewport. "
           "It no longer does, so read the root element's computed `writing-mode` and `direction` and write "
           "both arms");
    x = fmax(0.0, fmin(x, area_w - vw));
    y = fmax(0.0, fmin(y, area_h - vh));
    /* Step 11: "If position is the same as the viewport's current scroll position, and the viewport does not
       have an ongoing smooth scroll, return a resolved Promise and abort these steps." Every request lands
       here, which is the whole observable behaviour of a scroll in this engine — and the assert is the
       two-sided half: the moment a clamped request is somewhere else, steps 12-13 must be written. */
    DCHECK(x == viewport_scroll_x(ctx) && y == viewport_scroll_y(ctx),
           "CSSOM VIEW §4 scroll() step 11 no longer aborts: the clamped position differs from the viewport's "
           "current one, so steps 12-13 must PERFORM A SCROLL (§3.1) — make the viewport's scroll position "
           "per-flow state in the COW delta, queue the scroll event on the Document's pending scroll event "
           "targets, and write update-the-rendering step 9's SCROLL STEPS, which rendering.c asserts against "
           "the arrival of a way to move a scrolling box");
}

/* The client window's size, in CSS pixels, and its position relative to §2.3's Web-exposed screen area origin.
   The position is DERIVED and not a second UA choice: an ordinary desktop window sits in the middle of the
   space the operating system leaves for it, and screen.c already states what that space is. That derivation is
   why `screenX`/`screenY` are the two §4 members that stay concrete — see viewport.h.
   BOTH ASSERTS BELOW ARE OVER THE MODELLED EXAMPLE, which is the only value that reaches them: the size is a
   `double` this file computes and screen.c's is a `double` it computes, so neither can be a concolic and a C
   `if` here cannot pick an arm. What they assert is that the MODEL is coherent — a window a UA could actually
   open — at the one place an incoherent one would produce a negative coordinate. */
static double viewport_client_width(void)  { return VIEWPORT_TOP_WIDTH; }
static double viewport_client_height(void) { return VIEWPORT_TOP_HEIGHT + VIEWPORT_CHROME_HEIGHT; }

static double viewport_client_screen_x(void)
{
    DCHECK(viewport_client_width() <= screen_avail_width(),
           "the modelled client window is wider than the Web-exposed available screen area it is positioned "
           "inside — a window a UA could not open, and `screenX` would come out negative");
    return (double)(int)((screen_avail_width() - viewport_client_width()) / 2.0);
}

static double viewport_client_screen_y(void)
{
    DCHECK(viewport_client_height() <= screen_avail_height(),
           "the modelled client window is taller than the Web-exposed available screen area it is positioned "
           "inside — a window a UA could not open, and `screenY` would come out negative");
    return (double)(int)((screen_avail_height() - viewport_client_height()) / 2.0);
}

/* ---- CSSOM VIEW §4's Window extensions ------------------------------------------------------------------- */

/* THE MEMBERS, AS ONE LIST, and the aliases are IN it rather than being a second install: §4 states
   `pageXOffset` as "the value returned by the scrollX attribute" and `screenLeft` as `screenX`'s, so the two
   names share one magic and there is no second body that could ever answer differently. Every one of them is
   [Replaceable] readonly — assigning replaces the accessor with an ordinary data property, which is what
   idl_install_replaceable performs. */
typedef enum {
    VP_INNER_W, VP_INNER_H, VP_OUTER_W, VP_OUTER_H,
    VP_SCROLL_X, VP_SCROLL_Y, VP_SCREEN_X, VP_SCREEN_Y, VP_DPPX
} ViewportMember;

#define VIEWPORT_WINDOW_MEMBERS(X)   \
    X("innerWidth",       VP_INNER_W)  \
    X("innerHeight",      VP_INNER_H)  \
    X("outerWidth",       VP_OUTER_W)  \
    X("outerHeight",      VP_OUTER_H)  \
    X("scrollX",          VP_SCROLL_X) \
    X("pageXOffset",      VP_SCROLL_X) \
    X("scrollY",          VP_SCROLL_Y) \
    X("pageYOffset",      VP_SCROLL_Y) \
    X("screenX",          VP_SCREEN_X) \
    X("screenLeft",       VP_SCREEN_X) \
    X("screenY",          VP_SCREEN_Y) \
    X("screenTop",        VP_SCREEN_Y) \
    X("devicePixelRatio", VP_DPPX)

/* The names are string LITERALS so engine/idlgen.mjs's install audit can see them — a name built by
   concatenation reads as absent to the auditor, which is the audit lying by omission. */
static const char *const VP_NAME[] = {
#define X(n, m) n,
    VIEWPORT_WINDOW_MEMBERS(X)
#undef X
};
static const int VP_MAGIC[] = {
#define X(n, m) (int)(m),
    VIEWPORT_WINDOW_MEMBERS(X)
#undef X
};
#define VP_NAMES ((int)(sizeof(VP_NAME) / sizeof(VP_NAME[0])))

/* A member whose IDL type is `long`. Every length this component models is a whole number of CSS pixels, so a
   fraction here is a derivation that produced something the type cannot carry rather than a value to round.
   THE ASSERT HOLDS FOR THE WHOLE DOMAIN and not merely for the example: the members it guards are declared
   `long`, so no viewport this UA could have picked makes one of them fractional. */
static JSValue vp_long(JSContext *ctx, double v)
{
    DCHECK(v == (double)(int32_t)v,
           "a CSSOM VIEW §4 Window member declared `long` computed a value that is not an integer");
    return JS_NewInt32(ctx, (int32_t)v);
}

/* A `long` member that reports the environment: the modelled answer as the EXAMPLE of a concolic keyed on this
   document's answer for this member. */
static JSValue vp_env_long(JSContext *ctx, const char *member, double v)
{
    return viewport_env_value(ctx, member, vp_long(ctx, v));
}

static JSValue js_vp_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    /* §4 conditions six of these on there being a viewport or a client window, and the two absences are the
       same one in this engine: both are what a NAVIGABLE presents, and a document that is not fully active is
       presented by nothing. `devicePixelRatio` is NOT one of them — its algorithm asks about the OUTPUT DEVICE,
       which exists whether or not this document is on it. */
    bool presented = viewport_exists(ctx);

    (void)this_val;
    /* WHICH OF THESE ARE SOURCES AND WHICH ARE DERIVED is viewport.h's paragraph, decided per member and shown
       here at the member. "OR ZERO IF THERE IS NO VIEWPORT" IS NEVER A SOURCE: a document that is not being
       presented has no viewport, the SPEC states the answer, and there is nothing for a UA to have chosen — so
       the absent case is concrete even where the present one forks. */
    switch ((ViewportMember)magic) {
    /* "The innerWidth attribute must return the viewport width INCLUDING the size of a rendered scroll bar (if
       any), or zero if there is no viewport." Including the scrollbar is why no scrollbar question arises: the
       answer is the viewport, whether or not one is rendered. */
    case VP_INNER_W:  return presented ? vp_env_long(ctx, "innerWidth", viewport_width(ctx)) : vp_long(ctx, 0.0);
    case VP_INNER_H:  return presented ? vp_env_long(ctx, "innerHeight", viewport_height(ctx))
                                       : vp_long(ctx, 0.0);
    /* "The outerWidth attribute must return the width of the client window. If there is no client window this
       attribute must return zero." A SEPARATE source from the viewport's own size, which is what makes
       `outerHeight - innerHeight` — how much chrome this UA wears — a question with two answers. */
    case VP_OUTER_W:  return presented ? vp_env_long(ctx, "outerWidth", viewport_client_width())
                                       : vp_long(ctx, 0.0);
    case VP_OUTER_H:  return presented ? vp_env_long(ctx, "outerHeight", viewport_client_height())
                                       : vp_long(ctx, 0.0);
    /* "The scrollX attribute must return the x-coordinate, relative to the initial containing block origin, of
       the left of the viewport, or zero if there is no viewport." An `unrestricted double`, not a `long`.
       CONCRETE: the derivation below leaves one valid scroll position, and the layout that would give it a
       range would also give it a WRITER — see viewport.h. The member is `viewport_window_scroll`, which is this
       sentence WHOLE — the derivation and the no-viewport zero — because §10's `pageX` and §6's scroll members
       invoke this attribute by name and must get the same two halves. */
    case VP_SCROLL_X: return JS_NewFloat64(ctx, viewport_window_scroll(ctx, /*vertical*/ false));
    case VP_SCROLL_Y: return JS_NewFloat64(ctx, viewport_window_scroll(ctx, /*vertical*/ true));
    /* "The screenX and screenLeft attributes must return the x-coordinate, relative to the origin of the
       Web-exposed screen area, of the left of the client window as number of CSS pixels, or zero if there is
       no such thing." CONCRETE: a position derived from screen.c's available area and the client window's
       size, both of which a page can already explore at their own members. */
    case VP_SCREEN_X: return vp_long(ctx, presented ? viewport_client_screen_x() : 0.0);
    case VP_SCREEN_Y: return vp_long(ctx, presented ? viewport_client_screen_y() : 0.0);
    /* §4's DETERMINE THE DEVICE PIXEL RATIO: with no output device return 1; otherwise the CSS pixel size
       divided by the device pixel size. This engine models an output device (screen.c), so it is the ratio —
       and it is a source, because `devicePixelRatio > 1` is the retina gate. */
    case VP_DPPX:     return viewport_env_value(ctx, "devicePixelRatio",
                                                JS_NewFloat64(ctx, viewport_device_pixel_ratio(ctx)));
    }
    DFAIL("a CSSOM VIEW §4 Window member was read with a magic no member of this file declares — the magic IS "
          "the member, so an unknown one means a name was installed without a case to answer it");
    return JS_UNDEFINED;
}

/* ---- CSSOM VIEW §13.1's resize steps, this component's half ----------------------------------------------- */

/* The record's three fields ARE the spec sentence: what the viewport measured the last time the steps ran, and
   whether there HAS been a last time. See viewport.h for why the second one cannot be replaced by seeding the
   first at install.
   WHAT IT LATCHES IS THE MODELLED GEOMETRY, never what a flow decided about it. §13.1 asks whether the VIEWPORT
   changed, and a flow that took the true arm of `innerWidth < 768` did not resize anything — it recorded what
   it believes, and there is no solve-back that hands the model a different width. So the comparison is over the
   `double` this component computes, and the `JS_IsNumber` asserts below are two-sided: they fire if a concolic
   ever reaches the latch, which is what would say the seam had moved to the wrong side of the boundary. */
#define VP_RESIZE_RAN "hasBeenRun"
#define VP_RESIZE_W   "width"
#define VP_RESIZE_H   "height"

static int g_resize_slot = -1;

bool viewport_resize_changed(JSContext *ctx)
{
    JSValue rec = realm_value_get(ctx, g_resize_slot);
    JSValue ran = JS_GetPropertyStr(ctx, rec, VP_RESIZE_RAN);
    double w = viewport_width(ctx), h = viewport_height(ctx);
    bool had_run, changed;

    DCHECK(JS_IsBool(ran),
           "the §13.1 resize-steps record's `has been run` field is not a boolean — nothing but these steps "
           "ever writes this record, and they write exactly the three fields the algorithm names");
    had_run = JS_ToBool(ctx, ran);
    JS_FreeValue(ctx, ran);

    changed = false;
    if (had_run) {
        JSValue lw = JS_GetPropertyStr(ctx, rec, VP_RESIZE_W);
        JSValue lh = JS_GetPropertyStr(ctx, rec, VP_RESIZE_H);
        double dw = 0.0, dh = 0.0;

        DCHECK(JS_IsNumber(lw) && JS_IsNumber(lh),
               "the §13.1 resize-steps record holds a dimension that is not a number — a run that latched one "
               "latched both, so this is a write from outside the algorithm");
        JS_ToFloat64(ctx, &dw, lw);
        JS_ToFloat64(ctx, &dh, lh);
        JS_FreeValue(ctx, lw);
        JS_FreeValue(ctx, lh);
        changed = dw != w || dh != h;
    }
    /* WRITTEN ONLY WHEN IT MOVES, because each write is captured into the running flow's COW delta and a frame
       that re-latches an unchanged pair would grow every flow's delta once per rendering opportunity for
       nothing. Re-writing the same numbers is not observable, so not writing them is the same algorithm. */
    if (changed || !had_run) {
        JS_SetPropertyStr(ctx, rec, VP_RESIZE_W, JS_NewFloat64(ctx, w));
        JS_SetPropertyStr(ctx, rec, VP_RESIZE_H, JS_NewFloat64(ctx, h));
        if (!had_run)
            JS_SetPropertyStr(ctx, rec, VP_RESIZE_RAN, JS_TRUE);
    }
    JS_FreeValue(ctx, rec);
    return changed;
}

/* ---- the declaration and the per-realm install ------------------------------------------------------------ */

static void viewport_install(JSContext *ctx)
{
    JSValue global, rec;
    int i;

    /* §13.1's record, built WITH the realm — running twice in one realm is asserted by realm_value_set, which
       is where the first record is standing. */
    rec = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(rec), "viewport: OOM building a realm's CSSOM VIEW §13.1 resize-steps record");
    JS_SetPropertyStr(ctx, rec, VP_RESIZE_RAN, JS_FALSE);
    JS_SetPropertyStr(ctx, rec, VP_RESIZE_W, JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, rec, VP_RESIZE_H, JS_NewFloat64(ctx, 0.0));
    realm_value_set(ctx, g_resize_slot, rec);

    /* Web IDL §3.7.3: Window is [Global], so its members are own properties of the GLOBAL OBJECT rather than of
       Window.prototype — see window.c, which states the same rule for the browsing-context half. */
    global = JS_GetGlobalObject(ctx);
    for (i = 0; i < VP_NAMES; i++)
        idl_install_replaceable(ctx, global, VP_NAME[i], js_vp_get, VP_MAGIC[i]);
    JS_FreeValue(ctx, global);
}

void viewport_init(JSContext *ctx)
{
    DCHECK(g_resize_slot < 0, "viewport_init ran twice — the §13.1 record's slot is declared once per AGENT");
    g_resize_slot = realm_value_declare(ctx, "CSSOM VIEW §13.1 the viewport as the resize steps last saw it");
    realm_declare_intrinsic(viewport_install);
}

void viewport_free(void)
{
    /* The records are the REALMS' — each is released with its context. What the agent holds is the slot, and a
       slot id is a class id in a runtime that is going away with it. */
    g_resize_slot = -1;
}
