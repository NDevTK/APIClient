/* HTML §4.12.5.1 "The 2D rendering context" — see canvas_rendering_context_2d.h for why the member, the
 * creation algorithm, the bitmap and update-the-rendering step 13 are ONE landing, and for why §4.12.5.1.16
 * "Pixel manipulation" is the road that lands with them while every drawing member stays absent. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/canvas/canvas_path.h"
#include "core/canvas/canvas_rendering_context_2d.h"
#include "core/canvas/image_data.h"
#include "core/html/html_canvas_element.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"

/* Declared once per AGENT. */
static JSClassID g_class;
static int g_id_attrs = -1, g_id_lost = -1, g_id_reset = -1,
           g_id_save = -1, g_id_restore = -1, g_id_set_alpha = -1,
           g_id_get_image_data = -1, g_id_put_image_data = -1;
static JSValue g_state_key = JS_UNDEFINED;
static JSAtom  g_atom_state = JS_ATOM_NULL;

/* §4.12.5.1's `enum CanvasColorType { "unorm8", "float16" };`, in the IDL's own list order — which is what
   Web IDL §3.2.18 Enumeration types' fork numbers its outcomes by, so the order is the declaration and not a
   ranking made here. The colour SPACE list is core/canvas/image_data.h's, read rather than copied: one
   enumeration, so a `getImageData` and an `ImageData` constructor cannot disagree about what "display-p3"
   means or about which index it is. */
const char *const CANVAS_COLOR_TYPES[] = { "unorm8", "float16", NULL };

/* §4.12.5.1's `dictionary CanvasRenderingContext2DSettings`, with every default the IDL writes. `alpha` is the
   one that is TRUE by default, which is why core/idl_args.h grew IDL_DEFAULT_TRUE with this member: an absent
   `alpha` converts to FALSE under ToBoolean, the opposite of the declared value, and every
   `getContext("2d")` with no options would have reported an opaque bitmap.
   THE ROWS ARE NOT IN THE ORDER §4.12.5.1 PRINTS THEM, and that is the point rather than a transcription
   slip: the IDL prints `alpha, desynchronized, colorSpace, colorType, willReadFrequently` and Web IDL
   §3.2.17 "Dictionary types" READS them in another — step 4's inner loop is "For each dictionary member
   member declared on dictionary, in lexicographical order", which puts `colorSpace` and `colorType` ahead of
   `desynchronized`. The order is observable: a page writes `getContext("2d", { get colorSpace() { throw x },
   get desynchronized() { … } })` and counts which getters ran. This dictionary inherits nothing — the
   harvested IDL declares it with no `:` — so every row is level 0 and §3.2.17 step 3's least-to-most-derived
   list is the single-element one, which is why lexicographic order is the WHOLE rule here and swapping rows
   is the right repair rather than a level correction. Do not re-sort these to match the printed IDL. */
const IdlDictMember CTX2D_SETTINGS[CTX2D_SETTINGS_N] = {
    { "alpha",              IDL_BOOLEAN, false, NULL, 0, NULL, IDL_DEFAULT_TRUE },
    { "colorSpace",         IDL_ENUM,    false, IMAGE_DATA_COLOR_SPACES, 0, NULL, IDL_DEFAULT_STRING, "srgb" },
    { "colorType",          IDL_ENUM,    false, CANVAS_COLOR_TYPES,      0, NULL, IDL_DEFAULT_STRING, "unorm8" },
    { "desynchronized",     IDL_BOOLEAN, false, NULL, 0, NULL, IDL_DEFAULT_FALSE },
    { "willReadFrequently", IDL_BOOLEAN, false, NULL, 0, NULL, IDL_DEFAULT_FALSE },
};

/* THE CONTEXT'S STATE IS JS PROPERTIES IN AN INTERNAL-SLOT RECORD, for core/html/html_canvas_element.h's
   reason one object over: a flow WRITES this — `reset()` in one arm and not its sibling — so it must fork per
   flow, park to the cold tier and resume, and an ordinary property write is what the COW delta already
   captures. The class below exists for the BRAND and holds no opaque: a C record would need its own
   `CowRecord` and its own capture site, which is a second mechanism for a component that needs none. */
static JSValue ctx2d_state(JSContext *ctx, JSValueConst self)
{
    JSValue st;

    DCHECK(g_atom_state != JS_ATOM_NULL, "a 2D context's state was reached before §4.12.5.1 was declared");
    if (JS_GetOwnSlot(ctx, &st, self, g_atom_state) > 0) return st;
    /* The class id already branded this value, and the ONLY mint for that class is the creation algorithm,
       which defines the slot before it returns — so a missing record is this codebase's own logic being wrong
       and never a page's doing. It is a `CHECK` rather than a `DFAIL` because the release arm would otherwise
       hand every member below an `undefined` to read fields off, which is a crash one algorithm later in the
       component that did nothing wrong. */
    CHECK_FAIL("a CanvasRenderingContext2D carries no state record — the creation algorithm is its only mint");
    return JS_EXCEPTION;
}

/* The receiver's state, or JS_EXCEPTION with a Web IDL §3.7.6 TypeError pending. A receiver is PAGE-SUPPLIED INPUT —
   `CanvasRenderingContext2D.prototype.getImageData.call(null, 0, 0, 1, 1)` is one line a page can write — so
   this is a throw and never a DCHECK, which would hand any page an abort switch for the whole engine. */
static JSValue ctx2d_state_of(JSContext *ctx, JSValueConst this_val, const char *member)
{
    if (JS_GetClassID(this_val) != g_class)
        return JS_ThrowTypeError(ctx, "%s called on something that is not a CanvasRenderingContext2D", member);
    return ctx2d_state(ctx, this_val);
}

static int32_t ctx2d_int(JSContext *ctx, JSValueConst st, const char *name)
{
    JSValue v = JS_GetPropertyStr(ctx, st, name);
    int32_t out = 0;

    DCHECK(JS_IsNumber(v), "a 2D context state field this component alone writes is not a Number");
    JS_ToInt32(ctx, &out, v);
    JS_FreeValue(ctx, v);
    return out;
}

static bool ctx2d_bool(JSContext *ctx, JSValueConst st, const char *name)
{
    JSValue v = JS_GetPropertyStr(ctx, st, name);
    bool out;

    DCHECK(JS_IsBool(v), "a 2D context state field this component alone writes is not a Boolean");
    out = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return out;
}

/* ---- §4.12.5.1.3 "The canvas state" — THE DRAWING STATE AND ITS STACK ------------------------------------- */

/* THE DRAWING STATE IS ITS OWN RECORD, AND THAT IS WHAT MAKES `save()` TOTAL RATHER THAN A LIST SOMEBODY
 * MAINTAINS. §4.12.5.1.3 is "The save() method steps are to push a copy of the current drawing state onto the
 * drawing state stack", so the copy is over a SET that grows from the one member below to the thirty that
 * section's own five bullets name. A field-by-field copy would put an obligation on `save()` at every one of
 * those landings — the add-a-field-to-a-clone shape §Offensive-programming makes a DCHECK for — and its
 * failure is SILENT in the direction that matters: a member left out of the copy is one `restore()` does not
 * restore, which is a WRONG VALUE a page reads back rather than an absent member it gets a TypeError for.
 *
 * SO THE PARTITION IS STRUCTURAL: a field IS drawing state exactly when it lives in THIS record, and is not
 * when it lives on the context's state record beside it. §4.12.5.1.3's two exclusions then hold BY
 * CONSTRUCTION rather than by anyone remembering them — "The rendering context's bitmaps are not part of the
 * drawing state, as they depend on whether and how the rendering context is bound to a canvas element", and
 * the current default path, which *reset the rendering context to its default state* empties in its own step
 * 2 and never mentions again in its steps 3 and 4. Both live on `st`, not here. So do §4.12.5.1.2's five
 * settings, which are that section's and appear in none of §4.12.5.1.3's bullets. */

/* Every member of §4.12.5.1.3's drawing state at the initial value its own section states, in ONE place —
   which is what *reset the rendering context to its default state* step 4, "Reset everything that drawing
   state consists of to their initial values", reaches, and what the creation algorithm reaches. Two spellings
   of one initial value is the shape that drifts. */
static JSValue ctx2d_drawing_new(JSContext *ctx)
{
    JSValue d = idl_slots_new(ctx);

    if (JS_IsException(d)) return d;
    /* §4.12.5.1.17 "Compositing": the global alpha "value ranges from 0.0 (fully transparent) to 1.0 (no
       additional transparency). It must initially have the value 1.0". */
    JS_SetPropertyStr(ctx, d, "globalAlpha", JS_NewFloat64(ctx, 1.0));
    return d;
}

/* The running drawing state. The creation algorithm defines it before it returns and *reset* only ever
   replaces it, so an absent record is this codebase's own logic being wrong — a CHECK for ctx2d_state's
   reason, since the release arm would otherwise hand every reader below an `undefined` to read fields off. */
static JSValue ctx2d_drawing(JSContext *ctx, JSValueConst st)
{
    JSValue d = JS_GetPropertyStr(ctx, st, "drawing");

    CHECK(JS_IsObject(d), "§4.12.5.1.3: a CanvasRenderingContext2D carries no drawing state");
    return d;
}

static uint32_t ctx2d_stack_len(JSContext *ctx, JSValueConst stack)
{
    JSValue lv = JS_GetPropertyStr(ctx, stack, "length");
    uint32_t n = 0;

    if (JS_ToUint32(ctx, &n, lv) < 0) n = 0;
    JS_FreeValue(ctx, lv);
    return n;
}

/* The context's stack of drawing states — "Objects that implement the CanvasState interface maintain a stack
   of drawing states". It belongs to the OBJECT and not to a state: no drawing state contains the stack, which
   is why this hangs off `st` and is absent from ctx2d_drawing_new above. */
static JSValue ctx2d_stack(JSContext *ctx, JSValueConst st)
{
    JSValue stack = JS_GetPropertyStr(ctx, st, "stack");

    CHECK(JS_IsArray(stack), "§4.12.5.1.3: a CanvasRenderingContext2D carries no drawing state stack");
    return stack;
}

/* §4.12.5.1.3's "a copy of the current drawing state", over WHATEVER that record holds — so a member added to
   ctx2d_drawing_new above is copied here with no edit to this function, which is the whole reason the state
   is a record rather than a set of fields on `st`.
   A SHALLOW COPY IS A DEEP ONE ONLY WHILE EVERY MEMBER IS A PRIMITIVE, AND THAT IS ASSERTED RATHER THAN
   ASSUMED. §4.12.5.1.3's bullets name a current dash list, a current transformation matrix and a current
   clipping region, and the day the first of those lands as an object this copy would ALIAS it: `save()`, a
   mutation, `restore()`, and the restored state carries the mutation that was supposed to be undone. The
   DCHECK fires at that landing and names what to build, which is the forcing function rather than a comment
   asking the next author to remember. Its two sides can disagree — it holds today and fails on the first
   object-valued member — so it is a check and not a restatement. */
static JSValue ctx2d_drawing_copy(JSContext *ctx, JSValueConst src)
{
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0, i;
    JSValue dst = idl_slots_new(ctx);

    if (JS_IsException(dst)) return dst;
    /* The record has a NULL prototype and only this component ever writes it, so there is no page code and no
       exotic behaviour for the enumeration to reach — a failure here is allocation and nothing else. */
    CHECK(JS_GetOwnPropertyNames(ctx, &tab, &n, src, JS_GPN_STRING_MASK) == 0,
          "§4.12.5.1.3: a 2D context's drawing state could not be enumerated to be copied");
    for (i = 0; i < n; i++) {
        JSValue v = JS_GetProperty(ctx, src, tab[i].atom);

        CHECK(!JS_IsException(v), "§4.12.5.1.3: a member of a 2D context's drawing state could not be read "
                                  "back — the record is engine-built data properties with a null prototype");
        DCHECK(!JS_IsObject(v),
               "§4.12.5.1.3's drawing state gained a member that is an OBJECT, and `save()` copies this record "
               "exactly one property deep — the copy would ALIAS it, so a mutation made after a save() would "
               "be visible through the restore() that exists to undo it. Build the per-member copy that member "
               "needs before putting one here: the current dash list is a sequence<unrestricted double> and "
               "the current transformation matrix is six numbers, so each is a copy of its elements");
        CHECK(JS_DefinePropertyValue(ctx, dst, tab[i].atom, v, JS_PROP_C_W_E) >= 0,
              "§4.12.5.1.3: a member of a 2D context's drawing state could not be written to its copy");
    }
    JS_FreePropertyEnum(ctx, tab, n);
    return dst;
}

/* §4.12.5.1.3's *reset the rendering context to its default state*, in its own four steps. It is a FUNCTION
   with two callers and not an inlined body, because the creation algorithm reaches it through *set bitmap
   dimensions* step 1 and `reset()` reaches it directly — two spellings of one algorithm is the shape that
   drifts. */
static void ctx2d_reset_to_default(JSContext *ctx, JSValueConst st)
{
    JSValue canvas = JS_GetPropertyStr(ctx, st, "canvas");
    JSValue path, stack, drawing;

    /* Step 1 — "Clear canvas's bitmap to transparent black." The `alpha` arm is §4.12.5.1.2's: a context whose
       alpha is false has a bitmap that "starts off as opaque black instead of transparent black", and its
       alpha component "must be fixed to 1.0 (fully opaque) for all pixels". */
    canvas_bitmap_clear(ctx, canvas, !ctx2d_bool(ctx, st, "alpha"));

    /* Step 2 — "Empty the list of subpaths in the context's current default path." §4.12.5.1.6's own
       `beginPath()` is defined as exactly this, so the day that member lands it calls this line rather than
       restating it. */
    path = JS_GetPropertyStr(ctx, st, "path");
    DCHECK(!JS_IsUndefined(path), "a 2D context carries no current default path");
    JS_FreeValue(ctx, path);
    path = canvas_path_new(ctx);
    /* An allocation failure here would leave the context with an EXCEPTION where its path belongs, which
       every later reader would dereference as an array — the drop-a-flow-on-OOM shape §Offensive-programming
       makes an always-fatal CHECK rather than a recoverable state. */
    CHECK(!JS_IsException(path), "§4.12.5.1.3: OOM emptying a 2D context's current default path");
    JS_SetPropertyStr(ctx, st, "path", path);

    /* Step 3 — "Clear the context's drawing state stack." */
    stack = ctx2d_stack(ctx, st);
    CHECK(JS_SetPropertyStr(ctx, stack, "length", JS_NewUint32(ctx, 0)) >= 0,
          "§4.12.5.1.3 step 3: a 2D context's drawing state stack could not be cleared");
    JS_FreeValue(ctx, stack);

    /* Step 4 — "Reset everything that drawing state consists of to their initial values." It is a fresh record
       from the ONE function that states those initial values, so this step is TOTAL over the drawing state by
       construction and stays total as members are added to it — never a list of assignments here that a later
       member could be left out of.
       THE BITMAP IS NOT AMONG THEM and that is §4.12.5.1.3's own sentence: "The rendering context's bitmaps
       are not part of the drawing state, as they depend on whether and how the rendering context is bound to a
       canvas element" — which is why step 1 above is a separate act and not part of steps 3 and 4. Nor is the
       current default path, which step 2 empties and these two never mention. */
    drawing = ctx2d_drawing_new(ctx);
    CHECK(!JS_IsException(drawing), "§4.12.5.1.3 step 4: OOM resetting a 2D context's drawing state");
    JS_SetPropertyStr(ctx, st, "drawing", drawing);

    JS_FreeValue(ctx, canvas);
}

/* ---- §4.12.5's 2D context creation algorithm ---------------------------------------------------------------- */

/* THE SETTINGS ARE CONVERTED BY A DECLARATION AND NOT BY THIS BODY. Step 1 is "Let settings be the result of
   converting options to the dictionary type CanvasRenderingContext2DSettings. (This can throw an exception.)",
   and the conversion runs the page's getters — so it belongs at the argument boundary, which is where
   `idl_method_id_dict` puts it. `getContext` declares `options` as `any` because §4.12.5's own step 1 there is
   "If options is not an object, then set options to null", which happens BEFORE this algorithm sees it; this
   is the one place that turns it into the dictionary. */
static int ctx2d_enum_index(JSContext *ctx, JSValueConst settings, const char *name,
                            const char *const *values, int dflt)
{
    JSValue v = idl_dict_get(ctx, settings, name);
    const char *s;
    int i;

    if (JS_IsUndefined(v)) { JS_FreeValue(ctx, v); return dflt; }
    s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (s == NULL) return dflt;
    for (i = 0; values[i] != NULL; i++)
        if (strcmp(values[i], s) == 0) { JS_FreeCString(ctx, s); return i; }
    /* The conversion already refused every value outside the enumeration — Web IDL §3.2.18's own TypeError —
       so a string arriving here that is not in the list is this codebase's own logic being wrong. */
    DFAIL("a declared enumeration member crossed carrying a value its own list does not contain");
    JS_FreeCString(ctx, s);
    return dflt;
}

static bool ctx2d_dict_bool(JSContext *ctx, JSValueConst settings, const char *name, bool dflt)
{
    JSValue v = idl_dict_get(ctx, settings, name);
    bool out;

    if (JS_IsUndefined(v)) { JS_FreeValue(ctx, v); return dflt; }
    DCHECK(JS_IsBool(v), "a declared boolean dictionary member crossed as something else");
    out = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return out;
}

/* The algorithm's steps 2 to 7. Step 1 ran at the argument boundary — see above — and `settings` is its
   result. */
static JSValue ctx2d_create_from_settings(JSContext *ctx, JSValueConst target, JSValueConst settings)
{
    JSValue proto, self, st, path, drawing, stack;
    uint32_t w, h;

    DCHECK(g_class != 0, "a 2D context was minted before §4.12.5.1 was declared");

    /* Step 2 — "Let context be a new CanvasRenderingContext2D object." §A-PER-REALM-FACT: the prototype is
       read from the CALLING realm's class-proto slot, so a child navigable's `getContext("2d")` gets that
       document's CanvasRenderingContext2D.prototype and not the one whose realm happened to declare first. */
    proto = JS_GetClassProto(ctx, g_class);
    self = JS_NewObjectProtoClass(ctx, proto, g_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(self)) return self;

    st = idl_slots_new(ctx);
    if (JS_IsException(st)) { JS_FreeValue(ctx, self); return JS_EXCEPTION; }

    /* Step 3 — "Initialize context's canvas attribute to point to target." */
    JS_SetPropertyStr(ctx, st, "canvas", JS_DupValue(ctx, target));

    /* Step 6's *initialize a CanvasSettings output bitmap*, hoisted above step 5 for one reason the standard's
       own order does not forbid: step 5's *set bitmap dimensions* runs *reset the rendering context to its
       default state*, whose "Clear canvas's bitmap" READS `alpha` to decide between transparent and opaque
       black. Running step 6 after step 5 would clear the bitmap against an `alpha` that is not yet set, and
       §4.12.5.1.2's opaque-black arm would never be taken on a freshly created context — the one observable
       the ordering touches. The five assignments are §4.12.5.1.2's own five, in its own order. */
    JS_SetPropertyStr(ctx, st, "alpha",
                      JS_NewBool(ctx, ctx2d_dict_bool(ctx, settings, "alpha", true)));
    JS_SetPropertyStr(ctx, st, "desynchronized",
                      JS_NewBool(ctx, ctx2d_dict_bool(ctx, settings, "desynchronized", false)));
    JS_SetPropertyStr(ctx, st, "colorSpace",
                      JS_NewInt32(ctx, ctx2d_enum_index(ctx, settings, "colorSpace",
                                                        IMAGE_DATA_COLOR_SPACES, 0)));
    JS_SetPropertyStr(ctx, st, "colorType",
                      JS_NewInt32(ctx, ctx2d_enum_index(ctx, settings, "colorType", CANVAS_COLOR_TYPES, 0)));
    JS_SetPropertyStr(ctx, st, "willReadFrequently",
                      JS_NewBool(ctx, ctx2d_dict_bool(ctx, settings, "willReadFrequently", false)));

    /* §4.12.5.1's "context lost", which the CONTEXT LOST STEPS set and nothing else does — see
       canvas_rendering_context_2d_step_13 for why those steps are unreachable in this agent and why that makes
       `isContextLost()` a computed false rather than a stub. */
    JS_SetPropertyStr(ctx, st, "contextLost", JS_FALSE);

    /* §4.12.5.1.6's "When an object implementing the CanvasPath interface is created, its path must be
       initialized to zero subpaths" — `CanvasRenderingContext2D includes CanvasPath`, so the current default
       path exists from creation whether or not any member yet builds one, and *reset*'s step 2 empties a real
       list rather than a missing one. */
    path = canvas_path_new(ctx);
    if (JS_IsException(path)) { JS_FreeValue(ctx, st); JS_FreeValue(ctx, self); return JS_EXCEPTION; }
    JS_SetPropertyStr(ctx, st, "path", path);

    /* §4.12.5.1.3's drawing state and the stack of them, BEFORE step 5 below — *set bitmap dimensions* step 1
       is *reset the rendering context to its default state*, whose steps 3 and 4 clear that stack and rebuild
       that record, so both have to exist for the reset to be operating on the object rather than on nothing.
       This is the same ordering argument step 6 above is hoisted for. */
    drawing = ctx2d_drawing_new(ctx);
    if (JS_IsException(drawing)) { JS_FreeValue(ctx, st); JS_FreeValue(ctx, self); return JS_EXCEPTION; }
    JS_SetPropertyStr(ctx, st, "drawing", drawing);
    stack = JS_NewArray(ctx);
    if (JS_IsException(stack)) { JS_FreeValue(ctx, st); JS_FreeValue(ctx, self); return JS_EXCEPTION; }
    JS_SetPropertyStr(ctx, st, "stack", stack);

    JS_DefinePropertyValue(ctx, self, g_atom_state, st, JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);


    /* Step 4 — "Set context's output bitmap to the same bitmap as target's bitmap (so that they are shared)."
       The sharing is that the context reaches the bitmap THROUGH its `canvas`, so there is exactly one name
       for it; core/html/html_canvas_element.h says why a copied pointer would be a second name free to go
       stale under *set bitmap dimensions*. There is no assignment here because there is no second slot to
       assign to, which is the step being SATISFIED BY CONSTRUCTION rather than skipped.

       Step 5 — "Set bitmap dimensions to the numeric values of target's width and height content attributes."
       Its own step 1 is *reset the rendering context to its default state* and runs BEFORE the resize, which
       is the standard's order and is observable the day a context is re-created on a canvas that already had
       one: the reset clears the OLD bitmap, the resize then allocates the new one already transparent black. */
    ctx2d_reset_to_default(ctx, st);                                              /* set bitmap dimensions 1 */
    {
        /* The dimensions are the ATTRIBUTES' numeric values and not the element's current bitmap size, which
           is the step's own word — a canvas whose `width` attribute changed while it was in mode none gets the
           attribute's value here and not the stale one. core/html/html_canvas_element.c owns that read because
           it owns §4.12.5's two defaults (300 and 150) and the reflection row that must agree with them. */
        if (canvas_bitmap_set_dimensions_from_attributes(ctx, target) < 0) {      /* steps 2 to 5 */
            JS_FreeValue(ctx, self);
            return JS_EXCEPTION;
        }
        /* The bitmap the resize allocated is transparent black. A context whose `alpha` is false wants OPAQUE
           black — §4.12.5.1.2: "the bitmap of such a context starts off as opaque black instead of transparent
           black" — and the clear inside step 1 above ran against the OLD (empty) bitmap, so the new one is
           cleared here. This is the one place the two orders visibly differ, which is why it is a second call
           and not a reordering of the first. */
        if (!ctx2d_bool(ctx, st, "alpha")) canvas_bitmap_clear(ctx, target, true);
    }

    /* Step 7 — "Return context." Step 6 ran above; see its comment for why. */
    return self;
}

/* §4.12.5's 2D context creation algorithm. Step 1 — "Let settings be the result of converting options to the
   dictionary type CanvasRenderingContext2DSettings. (This can throw an exception.)" — has ALREADY RUN when
   this is reached: it is declared at `getContext`'s own `options` position, which is the argument boundary, so
   the page's getters run where a conversion can park rather than from inside a C activation of this file. The
   `settings` this receives is that dictionary. See core/html/html_canvas_element.c's declaration for the one
   case in which hoisting the conversion to that position is observable. */
JSValue canvas_rendering_context_2d_create(JSContext *ctx, JSValueConst target, JSValueConst settings)
{
    DCHECK(canvas_element_is(target), "the 2D context creation algorithm was given a target that is not a "
                                      "canvas element");
    return ctx2d_create_from_settings(ctx, target, settings);
}

/* ---- §4.12.5.1's members ------------------------------------------------------------------------------------ */

enum { M_CANVAS = 0, M_ATTRS, M_LOST, M_GLOBAL_ALPHA };

static JSValue js_ctx2d_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue st, d, out;

    switch (magic) {
    case M_CANVAS:
        st = ctx2d_state_of(ctx, this_val, "canvas");
        if (JS_IsException(st)) return JS_EXCEPTION;
        out = JS_GetPropertyStr(ctx, st, "canvas");   /* the back-reference step 3 initialized */
        JS_FreeValue(ctx, st);
        return out;
    case M_GLOBAL_ALPHA:
        /* §4.12.5.1.17: "The globalAlpha getter steps are to return this's global alpha." */
        st = ctx2d_state_of(ctx, this_val, "globalAlpha");
        if (JS_IsException(st)) return JS_EXCEPTION;
        d = ctx2d_drawing(ctx, st);
        out = JS_GetPropertyStr(ctx, d, "globalAlpha");
        JS_FreeValue(ctx, d);
        JS_FreeValue(ctx, st);
        return out;
    default:
        /* The magic is a value THIS file enumerates and every install below passes one of the two above, so
           this arm is unreachable by construction — a guard, and not a member left to build. */
        DFAIL("a 2D context accessor dispatched on a magic its own member list does not hold");
        return JS_UNDEFINED;
    }
}

/* §4.12.5.1.17: "The globalAlpha setter steps are: If the given value is either infinite, NaN, or not in the
 * range 0.0 to 1.0, then return. Otherwise, set this's global alpha to the given value."
 *
 * THE DECLARED TYPE IS `unrestricted double` AND THAT IS WHY THIS BODY NAMES NaN AND THE INFINITIES AT ALL.
 * Web IDL §3.2.8 "unrestricted double" lets them through as VALUES of the type, so the refusal is this
 * member's own step rather than the conversion's; declaring `double` instead would make `ctx.globalAlpha =
 * NaN` a TypeError where the standard, and every browser, ignores it silently. And IGNORING is not clamping —
 * the attribute keeps whatever it already held, which is the value a page reads back on the next line. */
static JSValue js_ctx2d_set_global_alpha(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    JSValue st = ctx2d_state_of(ctx, this_val, "globalAlpha");
    JSValue d;
    double a;

    (void)magic;
    if (JS_IsException(st)) return JS_EXCEPTION;
    /* The declared type already ran ToNumber at the argument boundary, so this reads the CONVERTED value and
       is never itself a conversion that could run a page's valueOf from inside this body. */
    if (JS_ToFloat64(ctx, &a, val) < 0) { JS_FreeValue(ctx, st); return JS_EXCEPTION; }
    /* ONE PREDICATE FOR ALL THREE REFUSALS, and it is the comparison rather than a chain of tests: every
       comparison with NaN is false, both infinities fall outside the interval, and so does every finite value
       the step excludes. Spelling them separately would be three chances to disagree about one range. */
    if (a >= 0.0 && a <= 1.0) {
        d = ctx2d_drawing(ctx, st);
        JS_SetPropertyStr(ctx, d, "globalAlpha", JS_NewFloat64(ctx, a));
        JS_FreeValue(ctx, d);
    }
    JS_FreeValue(ctx, st);
    return JS_UNDEFINED;
}

/* §4.12.5.1.3: "The save() method steps are to push a copy of the current drawing state onto the drawing
   state stack." */
static JSValue js_ctx2d_save(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue st = ctx2d_state_of(ctx, this_val, "save");
    JSValue d, copy, stack;

    (void)argc; (void)argv; (void)magic;
    if (JS_IsException(st)) return JS_EXCEPTION;
    d = ctx2d_drawing(ctx, st);
    copy = ctx2d_drawing_copy(ctx, d);
    JS_FreeValue(ctx, d);
    if (JS_IsException(copy)) { JS_FreeValue(ctx, st); return JS_EXCEPTION; }
    stack = ctx2d_stack(ctx, st);
    JS_SetPropertyUint32(ctx, stack, ctx2d_stack_len(ctx, stack), copy);
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, st);
    return JS_UNDEFINED;
}

/* §4.12.5.1.3: "The restore() method steps are to pop the top entry in the drawing state stack, and reset the
 * drawing state it describes. If there is no saved state, then the method must do nothing."
 *
 * THE EMPTY-STACK ARM RETURNS AND DOES NOT THROW, which is that sentence's own second half rather than a
 * softened error: a page whose `restore()` calls outnumber its `save()` calls is running the behaviour every
 * browser gives it, so an abort here would be this engine inventing a failure the standard rules out. */
static JSValue js_ctx2d_restore(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue st = ctx2d_state_of(ctx, this_val, "restore");
    JSValue stack, top;
    uint32_t n;

    (void)argc; (void)argv; (void)magic;
    if (JS_IsException(st)) return JS_EXCEPTION;
    stack = ctx2d_stack(ctx, st);
    n = ctx2d_stack_len(ctx, stack);
    if (n > 0) {
        top = JS_GetPropertyUint32(ctx, stack, n - 1);
        CHECK(JS_IsObject(top), "§4.12.5.1.3: a 2D context's drawing state stack holds an entry that is not a "
                                "drawing state — `save()` is the only thing that pushes one");
        /* The popped entry BECOMES the drawing state. It is a copy `save()` made and the stack is its only
           other name, so there is no second copy owed here and nothing left aliasing it after the truncation
           below — which is why this is an assignment and not another ctx2d_drawing_copy. */
        JS_SetPropertyStr(ctx, st, "drawing", top);
        CHECK(JS_SetPropertyStr(ctx, stack, "length", JS_NewUint32(ctx, n - 1)) >= 0,
              "§4.12.5.1.3: a 2D context's drawing state stack could not be popped");
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, st);
    return JS_UNDEFINED;
}

/* §4.12.5.1.2's "The getContextAttributes() method steps are to return «[ "alpha" → this's alpha,
   "desynchronized" → this's desynchronized, "colorSpace" → this's color space, "colorType" → this's color
   type, "willReadFrequently" → this's will read frequently ]»" — the five, in the section's own order.
   IT IS A FRESH OBJECT AT EVERY CALL, because a dictionary is returned BY VALUE: two calls must not answer the
   same object, or a page that writes to the first would see its write in the second. */
static JSValue js_ctx2d_get_context_attributes(JSContext *ctx, JSValueConst this_val, int argc,
                                               JSValueConst *argv, int magic)
{
    JSValue st = ctx2d_state_of(ctx, this_val, "getContextAttributes");
    JSValue out;

    (void)argc; (void)argv; (void)magic;
    if (JS_IsException(st)) return JS_EXCEPTION;
    out = JS_NewObject(ctx);
    if (JS_IsException(out)) { JS_FreeValue(ctx, st); return out; }
    JS_SetPropertyStr(ctx, out, "alpha", JS_NewBool(ctx, ctx2d_bool(ctx, st, "alpha")));
    JS_SetPropertyStr(ctx, out, "desynchronized", JS_NewBool(ctx, ctx2d_bool(ctx, st, "desynchronized")));
    JS_SetPropertyStr(ctx, out, "colorSpace",
                      JS_NewString(ctx, IMAGE_DATA_COLOR_SPACES[ctx2d_int(ctx, st, "colorSpace")]));
    JS_SetPropertyStr(ctx, out, "colorType",
                      JS_NewString(ctx, CANVAS_COLOR_TYPES[ctx2d_int(ctx, st, "colorType")]));
    JS_SetPropertyStr(ctx, out, "willReadFrequently",
                      JS_NewBool(ctx, ctx2d_bool(ctx, st, "willReadFrequently")));
    JS_FreeValue(ctx, st);
    return out;
}

/* §4.12.5.1's `boolean isContextLost()` — "return whether context is lost".
 * ITS FALSE IS COMPUTED AND NOT A STUB, and the difference is checkable. The only algorithm that sets a
 * context's "context lost" is the CONTEXT LOST STEPS, which HTML §8.1.7.3's update-the-rendering step 13 runs
 * "if the user agent detects that the backing storage associated with a CanvasRenderingContext2D ... has been
 * lost". This agent has no such detection to make: an allocation failure here is an always-fatal `CHECK`, so
 * there is no arm on which the engine continues with a bitmap it has lost. The flag is therefore false for
 * every context this build can produce, and it is READ from the record rather than returned as a literal so
 * that the day a detection exists this member already answers it. */
static JSValue js_ctx2d_is_context_lost(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                        int magic)
{
    JSValue st = ctx2d_state_of(ctx, this_val, "isContextLost");
    bool lost;

    (void)argc; (void)argv; (void)magic;
    if (JS_IsException(st)) return JS_EXCEPTION;
    lost = ctx2d_bool(ctx, st, "contextLost");
    JS_FreeValue(ctx, st);
    return JS_NewBool(ctx, lost);
}

/* §4.12.5.1.3: "The reset() method steps are to reset the rendering context to its default state." */
static JSValue js_ctx2d_reset(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue st = ctx2d_state_of(ctx, this_val, "reset");

    (void)argc; (void)argv; (void)magic;
    if (JS_IsException(st)) return JS_EXCEPTION;
    ctx2d_reset_to_default(ctx, st);
    JS_FreeValue(ctx, st);
    return JS_UNDEFINED;
}

/* ---- §4.12.5.1.16 "Pixel manipulation" ----------------------------------------------------------------------- */

/* "The getImageData(sx, sy, sw, sh, settings) method steps are:" — the section's own six.
 * NOTHING HERE READS A DRAWING STATE, AND THAT IS THE SECTION'S OWN LAST PARAGRAPH RATHER THAN THIS BUILD'S
 * NARROWNESS: "The current path, transformation matrix, shadow attributes, global alpha, the clipping region,
 * and current compositing and blending operator must not affect the methods described in this section." */
static JSValue js_ctx2d_get_image_data(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                       int magic)
{
    JSValue st = ctx2d_state_of(ctx, this_val, "getImageData");
    JSValue canvas, out;
    CanvasBitmap bm;
    ImageDataPixels dst;
    int64_t sx, sy, sw, sh;
    int cs;
    int64_t row;

    (void)magic; (void)argc;
    if (JS_IsException(st)) return JS_EXCEPTION;
    /* THE ARGUMENT VECTOR IS THE DECLARED ARITY AND `argc` IS NOT, so nothing here asserts a count: the
       machine EXTENDS `argc` over defaulted and dictionary positions and leaves it at what the page reached
       otherwise, which is why §3.6's own question is `idl_arg_given` and never a comparison. Every position
       below is required, so each holds a converted value. */
    JS_ToInt64(ctx, &sx, argv[0]);
    JS_ToInt64(ctx, &sy, argv[1]);
    JS_ToInt64(ctx, &sw, argv[2]);
    JS_ToInt64(ctx, &sh, argv[3]);

    /* Step 1 — "If either the sw or sh arguments are zero, then throw an "IndexSizeError" DOMException." */
    if (sw == 0 || sh == 0) {
        JS_FreeValue(ctx, st);
        return JS_ThrowDOMException(ctx, "IndexSizeError",
                                    "getImageData was asked for a rectangle with a zero side");
    }

    /* Step 2 — "If the CanvasRenderingContext2D's origin-clean flag is set to false, then throw a
       "SecurityError" DOMException." It is TRUE for every bitmap this build can produce, because the only
       algorithms that clear it draw an image and none of them exists — see core/html/html_canvas_element.h.
       The test is written rather than elided so the day an image source lands the refusal is already here. */
    canvas = JS_GetPropertyStr(ctx, st, "canvas");
    if (canvas_bitmap_get(ctx, canvas, &bm) && !bm.origin_clean) {
        JS_FreeValue(ctx, canvas); JS_FreeValue(ctx, st);
        return JS_ThrowDOMException(ctx, "SecurityError",
                                    "getImageData on a canvas whose bitmap is not origin-clean");
    }

    /* Steps 3 and 4 — a new ImageData initialized given sw, sh, settings and `defaultColorSpace` set to this's
       color space. §3.2.4.5's `[EnforceRange] long` already refused anything non-finite or out of range, so
       the magnitudes below are the algorithm's own "absolute magnitude" and cannot overflow a uint32_t. */
    cs = ctx2d_enum_index(ctx, argv[4], "colorSpace", IMAGE_DATA_COLOR_SPACES, ctx2d_int(ctx, st, "colorSpace"));
    out = image_data_new(ctx, (uint32_t)(sw < 0 ? -sw : sw), (uint32_t)(sh < 0 ? -sh : sh), cs);
    if (JS_IsException(out)) { JS_FreeValue(ctx, canvas); JS_FreeValue(ctx, st); return out; }

    /* Steps 5 and 6 — the source rectangle, and "Set the pixel values of imageData to be the pixels of this's
       output bitmap in the area specified by the source rectangle", with step 6's second half: "Set the pixels
       values of imageData for areas of the source rectangle that are outside of the output bitmap to
       transparent black." The fresh ImageData is ALREADY transparent black, so the outside is written by not
       writing it — which is the step performed rather than skipped, and is why the loop below tests each row
       and column against the bitmap instead of clamping the rectangle (a clamp would move the DESTINATION
       offset too, and the parts outside would land at the wrong place).
       A NEGATIVE WIDTH OR HEIGHT IS THE RECTANGLE'S OWN, not an error: step 5 names the four corners
       (sx, sy), (sx+sw, sy), (sx+sw, sy+sh), (sx, sy+sh), which for a negative side is the same rectangle
       extending the other way, so the origin moves and the magnitude is the extent. */
    if (sw < 0) { sx += sw; sw = -sw; }
    if (sh < 0) { sy += sh; sh = -sh; }
    if (!image_data_pixels(ctx, out, &dst)) {
        /* The ImageData was minted by this file one statement ago, in this realm, with a `rgba-unorm8` format
           and nothing between here and there that could detach it — so a refusal is this codebase's own logic
           being wrong and not a page's value. */
        DFAIL("§4.12.5.1.16: a freshly minted ImageData answered no pixels");
        JS_FreeValue(ctx, canvas); JS_FreeValue(ctx, st);
        return out;
    }
    if (bm.rgba != NULL) {
        for (row = 0; row < sh; row++) {
            int64_t by = sy + row, col;
            if (by < 0 || by >= (int64_t)bm.height) continue;
            for (col = 0; col < sw; col++) {
                int64_t bx = sx + col;
                if (bx < 0 || bx >= (int64_t)bm.width) continue;
                memcpy(dst.rgba + ((size_t)row * (size_t)sw + (size_t)col) * 4u,
                       bm.rgba + ((size_t)by * (size_t)bm.width + (size_t)bx) * 4u, 4u);
            }
        }
    }
    JS_FreeValue(ctx, canvas);
    JS_FreeValue(ctx, st);
    return out;                                                                   /* step 7 */
}

/* §4.12.5.1.16's *put pixels from an ImageData onto a bitmap*, which BOTH `putImageData` overloads are defined
   as calling — the three-argument one "given imageData, this's output bitmap, dx, dy, 0, 0, imageData's width,
   and imageData's height". ONE body for both, because the standard defines one algorithm and the shorter
   overload is the longer one with four arguments named; two bodies would be two answers to one question. */
static JSValue js_ctx2d_put_image_data(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                       int magic)
{
    JSValue st = ctx2d_state_of(ctx, this_val, "putImageData");
    JSValue canvas;
    CanvasBitmap bm;
    ImageDataPixels src;
    int64_t dx, dy, dirty_x, dirty_y, dirty_w, dirty_h, y;

    (void)magic;
    if (JS_IsException(st)) return JS_EXCEPTION;
    /* Step 1 and 2 — the [[ViewedArrayBuffer]], and "If IsDetachedBuffer(buffer) is true, then throw an
       "InvalidStateError" DOMException". `image_data_pixels` reports both failures separately because they are
       two different exceptions: a receiver that is not an ImageData is the DECLARATION's TypeError and never
       reaches here, so a false with `is_image_data` set is the detached arm. */
    if (!image_data_pixels(ctx, argv[0], &src)) {
        JS_FreeValue(ctx, st);
        if (!src.is_image_data)
            return JS_ThrowTypeError(ctx, "putImageData's first argument is not an ImageData");
        if (src.detached)
            return JS_ThrowDOMException(ctx, "InvalidStateError",
                                        "putImageData was given an ImageData whose data is detached");
        /* A "rgba-float16" ImageData — §8.11.1 admits one and this build's bitmap is eight-bit, so there is no
           conversion to perform and no step of this algorithm that describes one. */
        DFAIL("§4.12.5.1.16: putImageData was given a float16 ImageData and this bitmap is unorm8 — the "
              "next diff is the colorType road, which gives the bitmap a float16 representation to convert to");
        return JS_EXCEPTION;
    }

    JS_ToInt64(ctx, &dx, argv[1]);
    JS_ToInt64(ctx, &dy, argv[2]);
    /* The three-argument overload's own arguments: "0, 0, imageData's width, and imageData's height". The
       declaration makes positions 3 to 6 optional and `idl_arg_given` is what says which overload was called —
       a test on the VALUE would be the argument machine's job done in a body. */
    if (idl_arg_given(argc, argv, 3)) {
        JS_ToInt64(ctx, &dirty_x, argv[3]);
        JS_ToInt64(ctx, &dirty_y, argv[4]);
        JS_ToInt64(ctx, &dirty_w, argv[5]);
        JS_ToInt64(ctx, &dirty_h, argv[6]);
    } else {
        dirty_x = 0; dirty_y = 0;
        dirty_w = (int64_t)src.width; dirty_h = (int64_t)src.height;
    }

    /* Steps 3 to 8 — the dirty rectangle's own normalization, in the algorithm's own order. Each is written
       out because the ORDER is observable: step 5 clamps `dirtyX` to zero only AFTER step 3 may have moved it
       there, so a negative width and a negative x compose rather than cancelling. */
    if (dirty_w < 0) { dirty_x += dirty_w; dirty_w = -dirty_w; }                  /* step 3 */
    if (dirty_h < 0) { dirty_y += dirty_h; dirty_h = -dirty_h; }                  /* step 4 */
    if (dirty_x < 0) { dirty_w += dirty_x; dirty_x = 0; }                         /* step 5 */
    if (dirty_y < 0) { dirty_h += dirty_y; dirty_y = 0; }                         /* step 6 */
    if (dirty_x + dirty_w > (int64_t)src.width)  dirty_w = (int64_t)src.width  - dirty_x;   /* step 7 */
    if (dirty_y + dirty_h > (int64_t)src.height) dirty_h = (int64_t)src.height - dirty_y;   /* step 8 */
    /* Step 9 — "If, after those changes, either dirtyWidth or dirtyHeight are negative or zero, then return
       without affecting any bitmaps." */
    if (dirty_w <= 0 || dirty_h <= 0) { JS_FreeValue(ctx, st); return JS_UNDEFINED; }

    canvas = JS_GetPropertyStr(ctx, st, "canvas");
    if (canvas_bitmap_get(ctx, canvas, &bm) && bm.rgba != NULL) {
        bool opaque = !ctx2d_bool(ctx, st, "alpha");

        /* Step 10 — "For all integer values of x and y where dirtyX ≤ x < dirtyX + dirtyWidth and dirtyY ≤ y <
           dirtyY + dirtyHeight, set the pixel with coordinate (dx + x, dy + y) in bitmap to the color of the
           pixel at coordinate (x, y) in the imageData data structure's bitmap". The colour-space conversion
           the step names is the identity here: the bitmap and the ImageData are both this context's colour
           space until a member can give them different ones, which is the `colorSpace` settings road. */
        for (y = dirty_y; y < dirty_y + dirty_h; y++) {
            int64_t by = dy + y, x;
            if (by < 0 || by >= (int64_t)bm.height) continue;
            for (x = dirty_x; x < dirty_x + dirty_w; x++) {
                int64_t bx = dx + x;
                uint8_t *d;
                if (bx < 0 || bx >= (int64_t)bm.width) continue;
                d = bm.rgba + ((size_t)by * (size_t)bm.width + (size_t)bx) * 4u;
                memcpy(d, src.rgba + ((size_t)y * (size_t)src.width + (size_t)x) * 4u, 4u);
                /* §4.12.5.1.2: a context whose alpha is false has its alpha component "fixed to 1.0 (fully
                   opaque) for all pixels, and attempts to change the alpha component of any pixel must be
                   silently ignored" — which the same paragraph spells for this member in as many words: "the
                   putImageData() method effectively ignores every fourth byte in its input". */
                if (opaque) d[3] = 255;
            }
        }
    }
    JS_FreeValue(ctx, canvas);
    JS_FreeValue(ctx, st);
    return JS_UNDEFINED;
}

/* ---- HTML §8.1.7.3's update-the-rendering step 13 ------------------------------------------------------------ */

/* "For each doc of docs, IF THE USER AGENT DETECTS THAT THE BACKING STORAGE associated with a
 * CanvasRenderingContext2D or an OffscreenCanvasRenderingContext2D, context, has been lost, then it must run
 * the context lost steps for each such context".
 *
 * THE STEP IS PERFORMED AND ITS CONDITION IS FALSE, which is a different thing from the step being unwritten
 * and is why the `realm_awaits` that stood in core/rendering/rendering.c is RETIRED rather than relaxed. The
 * condition is a DETECTION this user agent makes, and this one makes none: every allocation on the bitmap road
 * is an always-fatal `CHECK`, so there is no arm anywhere on which this engine continues running with a canvas
 * whose backing storage it has lost. A context's "context lost" is therefore false for the life of every
 * context this build can produce, and `isContextLost()` reads that rather than returning a literal.
 * WHAT WOULD MAKE IT TRUE is a backing store this agent does not own — a GPU-resident bitmap, or an
 * OffscreenCanvas transferred to a worker whose agent went away — and the day either exists, this function is
 * where the detection goes and the context lost steps (set the flag, reset to default state, fire
 * `contextlost` CANCELABLE, and fire `contextrestored` on a successful restore) are written under it. It is a
 * function with a caller rather than a comment in rendering.c for the reason §Offensive-programming gives: a
 * step whose condition nobody can find is a step the next reader has to re-derive. */
void canvas_rendering_context_2d_step_13(JSContext *ctx)
{
    (void)ctx;
}

/* ---- the declaration and the per-realm install ---------------------------------------------------------------- */

void canvas_rendering_context_2d_init(JSContext *ctx)
{
    JSClassDef def = { "CanvasRenderingContext2D", NULL, NULL };
    /* `getImageData([EnforceRange] long sx, [EnforceRange] long sy, [EnforceRange] long sw,
       [EnforceRange] long sh, optional ImageDataSettings settings = {})`. */
    static const IdlArgType GET_IMAGE_DATA[5] = {
        IDL_LONG_ENFORCE, IDL_LONG_ENFORCE, IDL_LONG_ENFORCE, IDL_LONG_ENFORCE, IDL_DICT
    };
    /* BOTH `putImageData` ENTRIES AS ONE DECLARATION. Web IDL §3.6 distinguishes them by ARITY alone — every
       position they share has the same type — so there is no distinguishing argument index and the longer
       entry with four optional positions IS the effective overload set. */
    static const IdlArgType PUT_IMAGE_DATA[7] = {
        IDL_ANY, IDL_LONG_ENFORCE, IDL_LONG_ENFORCE,
        IDL_LONG_ENFORCE, IDL_LONG_ENFORCE, IDL_LONG_ENFORCE, IDL_LONG_ENFORCE
    };
    DCHECK(g_class == 0, "canvas_rendering_context_2d_init ran twice — §4.12.5.1 is declared once per AGENT");
    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    JS_NewClass(JS_GetRuntime(ctx), g_class, &def);

    g_id_attrs = idl_method_id(ctx, NULL, 0, js_ctx2d_get_context_attributes, 0);
    g_id_lost  = idl_method_id(ctx, NULL, 0, js_ctx2d_is_context_lost, 0);
    g_id_reset = idl_method_id(ctx, NULL, 0, js_ctx2d_reset, 0);
    g_id_save    = idl_method_id(ctx, NULL, 0, js_ctx2d_save, 0);
    g_id_restore = idl_method_id(ctx, NULL, 0, js_ctx2d_restore, 0);
    /* `attribute unrestricted double globalAlpha` — the type is §3.2.8's and not §3.2.7's, so NaN and the
       infinities reach the body, which is what lets it perform §4.12.5.1.17's own ignore. */
    g_id_set_alpha = idl_setter_id(ctx, IDL_UNRESTRICTED_DOUBLE, false, js_ctx2d_set_global_alpha, 0);

    g_id_get_image_data = idl_method_id_dict(ctx, GET_IMAGE_DATA, 5, IMAGE_DATA_SETTINGS,
                                             IMAGE_DATA_SETTINGS_N, js_ctx2d_get_image_data, 0);
    idl_optional_from(4);
    g_id_put_image_data = idl_method_id(ctx, PUT_IMAGE_DATA, 7, js_ctx2d_put_image_data, 0);
    idl_optional_from(3);

    g_state_key = JS_NewSymbol(ctx, "canvasRenderingContext2DState", false);
    CHECK(!JS_IsException(g_state_key), "§4.12.5.1: the 2D context state slot key allocation failed");
    g_atom_state = JS_ValueToAtom(ctx, g_state_key);
    CHECK(g_atom_state != JS_ATOM_NULL, "§4.12.5.1: the 2D context state slot key could not be interned");

    /* THE ROW'S NAME AND NOT THIS FILE'S. core/platform.c's row is `canvas_ctx2d` and its release column is
       what reaches canvas_rendering_context_2d_free; platform_check_agent_state pairs declaration to row by
       strcmp, so a name spelled after the FILE names a row that does not exist — the registry walk aborts,
       and the row that really owns these slots is left reporting "declared no agent state" in the exact words
       a component that declared nothing would use. RETIREMENT: this note goes when a declaration takes its
       component name FROM the row rather than from a string written here, which is what would make the two
       spellings unable to disagree. */
    agent_state_class("canvas_ctx2d", &g_class,
                      "§4.12.5.1's CanvasRenderingContext2D class, and the declaration latch");
    realm_declare_intrinsic(canvas_rendering_context_2d_install_realm);
}

/* Web IDL §3.7.3's interface prototype object for §4.12.5.1, its §3.7.1 interface object, and §3.8's property
 * reference for the one name — for ONE realm.
 *
 * THE INTERFACE OBJECT DOES NOT CONSTRUCT. `CanvasRenderingContext2D` declares no `constructor` in its IDL, so
 * Web IDL §3.7.1 Interface object gives `new CanvasRenderingContext2D()` a TypeError, and the only thing that
 * mints one is the 2D context creation algorithm — which is why `canvas_rendering_context_2d_create` is not a
 * constructor declaration.
 *
 * THE `save()`/`restore()` RESIDUAL THAT STOOD HERE IS RETIRED BY THE STACK ABOVE, AND ITS COUNT WAS WRONG IN
 * A WAY WORTH KEEPING. It said §4.12.5.1.3 `defines a drawing state as a list of eleven things` — BACKTICKED
 * rather than quoted because it is THIS TREE'S retired prose and not the standard's, and a quoted run beside a
 * section number is read by the citation auditor as a claim about that section, which is a fabrication finding
 * manufactured out of a correct record. That section's list is FIVE top-level items naming THIRTY: the
 * current transformation matrix; the current
 * clipping region; a third item carrying eight ("The current letter spacing, word spacing, fill style, stroke
 * style, filter, global alpha, compositing and blending operator, and shadow color"); a fourth carrying
 * nineteen attributes by name; and the current dash list. Eleven is what you get by counting the first, the
 * second, the third's eight and the fifth and SKIPPING the fourth item entirely — a count that disagrees with
 * its own list, which is the one error in a residual that needs no tree and no fetch to catch, because both
 * halves are in the sentence. Its CONCLUSION was right and only its arithmetic was wrong, which is the shape
 * that survives review longest: a reader checks the verdict, finds it holds, and inherits the method. So the
 * durable form of that clause is the LIST and not a number, and the population question it was really about is
 * a command rather than a claim — the member-list audit's ABSENT column for this interface, read against
 * §4.12.5.1.3's five items, is what says how much of a drawing state exists on any given day.
 *
 * WHAT WAS TRUE AND IS WHY THE STACK COULD NOT LAND ALONE: a `save()` over an EMPTY drawing state is correct at
 * every value of the program and observable at none, which is `js_noop` as an interface member and is banned by
 * name. It lands here because `globalAlpha` lands with it, so the copy has something to copy and
 * `save(); ctx.globalAlpha = 0.25; restore(); ctx.globalAlpha` answers 1 — an observable the stack alone could
 * not have produced.
 *
 * NAMED RESIDUAL — `createImageData`. WHAT IS NOT COVERED: neither entry of §4.12.5.1.16's
 * `createImageData(sw, sh, settings)` / `createImageData(imageData)` is placed. WHAT THE NEXT DIFF BUILDS: an
 * `IdlArgType` row for a position that is `long` on one entry and `ImageData` on the other — Web IDL §3.6's
 * distinguishing argument index is 0 and the two arms are §3.2.15's interface test and §3.2.4.5's conversion,
 * which is a row in core/idl_args.h beside `IDL_ULONG_OR_IMAGE_DATA_ARRAY` rather than a test in a body. The
 * shorter entry alone may NOT be declared first: `createImageData(someImageData)` would then convert its
 * argument to a `long`, reach zero, and throw "IndexSizeError" where the standard returns a new ImageData —
 * a wrong ANSWER rather than an honest absence, which is why this residual is one member and not two. HOW ITS
 * ABSENCE WOULD SHOW: a document that mints a scratch buffer from the context rather than from
 * `new ImageData(w, h)` ends its flow at the call.
 *
 * NAMED RESIDUAL — `globalCompositeOperation`, WHICH IS `globalAlpha`'s OWN MIXIN AND IS THE MEMBER A READER
 * WILL ASK ABOUT FIRST. WHAT IS NOT COVERED: `CanvasCompositing` declares two attributes and only one is
 * placed, so a page that assigns the other creates an ordinary property on the context and reads its own value
 * back, where a browser would have refused an unknown one. WHAT THE NEXT DIFF BUILDS: the value lists its
 * setter tests against — §4.12.5.1.17's step is "If the given value is not identical to any of the values that
 * the `<blend-mode>` or the `<composite-mode>` properties are defined to take, then return" — which are
 * COMPOSITING AND BLENDING's enumerations and not HTML's, so this member is blocked on a table from another
 * standard rather than on anything in this file, and that is the whole reason it is not in the diff that
 * landed `globalAlpha` beside it. Its initial value is this section's own "source-over". HOW ITS ABSENCE WOULD
 * SHOW: read the member back after assigning a value no compositing mode names — this build answers that
 * value, a browser answers the one it held.
 *
 * NAMED RESIDUAL — EVERY DRAWING MEMBER. WHAT IS NOT COVERED: `CanvasTransform`, `CanvasFillStrokeStyles`,
 * `CanvasRect`, `CanvasDrawPath`, `CanvasPath`'s ten builders, `CanvasText`, `CanvasDrawImage`, the rest of
 * `CanvasCompositing` and the rest of the seventeen mixins are not placed. WHAT THE NEXT DIFF BUILDS: the FILL
 * road as one landing — the drawing state's transformation matrix with `CanvasTransform`'s setters, `fillStyle`
 * with a CSS colour, the ten path builders applying the CTM as they copy (core/graphics/raster_path.h's own
 * "FOR THE CANVAS LANE" note: the copy and the transform are one walk and an ARC is transformed by its
 * PARAMETERS), `clearRect`/`fillRect` and `fill` over core/graphics/rasterizer.h's `raster_fill`. `stroke` and
 * `strokeRect` are NOT in that diff and are not merely deferred: §4.12.5.1.13's *trace a path* — the dash-list
 * state machine and its `Convert` step's caps and joins — has no component in core/graphics at all, so they are
 * blocked on a stroker the way `drawImage` is blocked on a decoder. HOW ITS ABSENCE WOULD SHOW: a document
 * reaches its first drawing call and its flow ends there, having already obtained a context and sized a bitmap. */
void canvas_rendering_context_2d_install_realm(JSContext *ctx)
{
    JSValue proto, prev, global;

    DCHECK(g_class != 0, "a realm asked for CanvasRenderingContext2D.prototype before it was declared");
    prev = JS_GetClassProto(ctx, g_class);
    DCHECK(JS_IsNull(prev), "canvas_rendering_context_2d_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "CanvasRenderingContext2D.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "CanvasRenderingContext2D");

    /* In the IDL's own order: the interface's one direct member, then the mixins it includes. Web IDL §3.7.3
       gives a mixin no prototype of its own, so flattening them onto the includer is the standard's shape. */
    idl_install_accessor(ctx, proto, "canvas", js_ctx2d_get, M_CANVAS, -1);         /* the interface itself */
    idl_install_method(ctx, proto, "getContextAttributes", g_id_attrs);             /* CanvasSettings */
    idl_install_method(ctx, proto, "save", g_id_save);                              /* CanvasState */
    idl_install_method(ctx, proto, "restore", g_id_restore);                        /* CanvasState */
    idl_install_method(ctx, proto, "reset", g_id_reset);                            /* CanvasState */
    idl_install_method(ctx, proto, "isContextLost", g_id_lost);                     /* CanvasState */
    idl_install_accessor(ctx, proto, "globalAlpha", js_ctx2d_get, M_GLOBAL_ALPHA,
                         g_id_set_alpha);                                           /* CanvasCompositing */
    idl_install_method(ctx, proto, "getImageData", g_id_get_image_data);            /* CanvasImageData */
    idl_install_method(ctx, proto, "putImageData", g_id_put_image_data);            /* CanvasImageData */

    /* §3.7.1's interface object for an interface that declares NO constructor — `CanvasRenderingContext2D`
       has none in its IDL, so `new CanvasRenderingContext2D()` is a TypeError and the only thing that mints
       one is the 2D context creation algorithm. Its PRESENCE is what tells a feature-detecting bundle the API
       exists, which is the whole of what §3.8's property reference is for. */
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "CanvasRenderingContext2D",
                                         idl_interface_object(ctx, "CanvasRenderingContext2D", proto));
    JS_FreeValue(ctx, global);

    JS_SetClassProto(ctx, g_class, proto);      /* the realm owns it from here */
}

void canvas_rendering_context_2d_free(JSRuntime *rt)
{
    /* The slot key is the AGENT's, so it is released with the agent — a Symbol nobody frees is a live GC
       object the runtime's own walk counts as a failure. */
    if (g_atom_state != JS_ATOM_NULL) JS_FreeAtomRT(rt, g_atom_state);
    g_atom_state = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_state_key);
    g_state_key = JS_UNDEFINED;
    g_class = 0;
    g_id_attrs = g_id_lost = g_id_reset = -1;
    g_id_save = g_id_restore = g_id_set_alpha = -1;
    g_id_get_image_data = g_id_put_image_data = -1;
}
