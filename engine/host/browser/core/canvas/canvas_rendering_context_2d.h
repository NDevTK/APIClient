/* HTML §4.12.5.1 "The 2D rendering context" — the interface `getContext("2d")` returns, its output bitmap and
 * §4.12.5.1.16 "Pixel manipulation".
 *
 * WHY THIS LANDS AS ONE DIFF AND WHY IT STOPS WHERE IT DOES. §4.12.5's `getContext` table gives the `"2d"` row
 * NO supports-condition — the `"webgl"`, `"webgl2"` and `"webgpu"` rows carry one ("if the user agent supports
 * the WebGL feature in its current configuration") and fall to the unsupported-value row without it, and the
 * `"2d"` row has none — so a conformant agent has no null arm to take and MUST run the 2D context creation
 * algorithm. That algorithm's step 2 mints a `CanvasRenderingContext2D`, its step 4 shares the canvas's bitmap
 * and its step 5 *sets bitmap dimensions*, whose own step 1 is *reset the rendering context to its default
 * state* — which begins "Clear canvas's bitmap to transparent black". A context with no bitmap is therefore
 * not a smaller version of this; it is §NO-STUBS' own tell, a list of MEMBERS where the standard states STEPS.
 * The member, the algorithm, the bitmap and HTML §8.1.7.3 "Processing model"'s update-the-rendering step 13
 * are one landing for a reason that is checkable rather than argued: core/rendering/rendering.c asserted step
 * 13's absence through the Web IDL §3.8 property reference for THIS NAME, so the interface object and the step
 * land together or that probe is a liar — the rule core/fullscreen/fullscreen.h states for step 12.
 *
 * WHAT IS DELIBERATELY NOT HERE IS THE DRAWING STATE, AND §4.12.5.1.16 IS THE STANDARD SAYING SO. Its own last
 * paragraph is "The current path, transformation matrix, shadow attributes, global alpha, the clipping region,
 * and current compositing and blending operator must not affect the methods described in this section." So the
 * pixel-manipulation road is separable from the drawing road BY THE SPECIFICATION and not by a preference
 * here: `getImageData` and `putImageData` read and write the bitmap directly, and nothing they do is a
 * function of any drawing-state member. That is what makes a context carrying them COMPLETE rather than
 * narrow — every observable this interface installs is written by an algorithm inside this file.
 *
 * AND THE ORDER IS FORCED BY WHAT A SILENT WRONG PIXEL WOULD COST. §A-FIELD-A-CONSUMER-DEFAULTS applies to a
 * bitmap exactly as it does to a field: a context that ACCEPTS a drawing call and reads back transparent black
 * manufactures a plausible datum, and the corpus is full of read-back capability probes that would believe it.
 * Every drawing member is therefore ABSENT rather than present-and-inert — a page's `ctx.fillRect(…)` raises
 * Web IDL's TypeError for a missing member, which is an honest report and is §NO-STUBS' forcing function. The
 * one shape that is forbidden is the pair, and it is the pair this file does not contain: `fillStyle` present
 * beside an absent `fillRect` is harmless, and `fillRect` present beside an absent `fillStyle` would paint
 * BLACK where the page asked for a colour, because an assignment to a member that does not exist creates an
 * ordinary property and throws nothing. That pairing is why the drawing road's first diff is not `fillRect`
 * alone. */
#ifndef ENGINE_HOST_BROWSER_CORE_CANVAS_CANVAS_RENDERING_CONTEXT_2D_H
#define ENGINE_HOST_BROWSER_CORE_CANVAS_CANVAS_RENDERING_CONTEXT_2D_H

#include <stdbool.h>

#include "quickjs.h"
#include "core/idl_args.h"   /* IdlDictMember — §4.12.5.1.2's settings dictionary is declared once, below */

/* Declared once per AGENT: the class and every member id. REGISTERS the per-realm install. */
void canvas_rendering_context_2d_init(JSContext *ctx);
void canvas_rendering_context_2d_install_realm(JSContext *ctx);
void canvas_rendering_context_2d_free(JSRuntime *rt);

/* §4.12.5.1.2's `dictionary CanvasRenderingContext2DSettings` and §4.12.5.1's `enum CanvasColorType`, declared
   HERE and not in the body that reads them, because the member which CONVERTS one is `HTMLCanvasElement`'s
   `getContext` — see canvas_rendering_context_2d.c for why step 1's conversion belongs at that argument
   boundary. One table, so the converter and the reader cannot disagree about a default or about the order Web
   IDL §3.2.18 Enumeration types numbers a fork's outcomes by. Both outlive every declaration, which is what
   `idl_method_id_dict` requires of the members it keeps a pointer to. */
#define CTX2D_SETTINGS_N 5
extern const IdlDictMember CTX2D_SETTINGS[CTX2D_SETTINGS_N];
extern const char *const CANVAS_COLOR_TYPES[];

/* §4.12.5's 2D CONTEXT CREATION ALGORITHM, steps 2 to 7 — "which is passed a target (a canvas element) and
   options". `settings` is step 1's ALREADY-CONVERTED dictionary, which `getContext`'s own declaration built.
   Returns the new context or an exception. The CALLER sets the canvas's context mode, which is §4.12.5's own
   split: the algorithm returns a context and `getContext`'s table cell is what moves the mode. */
JSValue canvas_rendering_context_2d_create(JSContext *ctx, JSValueConst target, JSValueConst settings);

/* HTML §8.1.7.3's update-the-rendering STEP 13, for core/rendering/rendering.c — see its definition for why
   this agent's answer is that the condition is FALSE and why that is a performed step rather than an elided
   one. */
void canvas_rendering_context_2d_step_13(JSContext *ctx);

#endif /* ENGINE_HOST_BROWSER_CORE_CANVAS_CANVAS_RENDERING_CONTEXT_2D_H */
