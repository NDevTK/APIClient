/* HTML §4.12.5 "The canvas element" — see html_canvas_element.h for why the bitmap's pixels are a JS typed
 * array and why it is materialized at context creation rather than at parse. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/canvas/canvas_rendering_context_2d.h"
#include "core/dom/element.h"
#include "core/html/html_canvas_element.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>

/* Declared once per AGENT. */
static int    g_id_get_context = -1;
static JSValue g_state_key = JS_UNDEFINED;
static JSAtom  g_atom_state = JS_ATOM_NULL;

/* §4.12.5's own two defaults: "a natural width equal to the numeric value of the element's width attribute"
   with 300 and 150 when the attribute is absent or not a valid non-negative integer. They are the same pair
   core/html/html_element.c's R_CANVAS reflection row declares, which is the fact rather than a copy of it —
   the reflection ANSWERS the member and this READS the attribute, and a canvas whose two disagreed would have
   `c.width` and its bitmap describing different rectangles. */
#define CANVAS_DEFAULT_WIDTH  300
#define CANVAS_DEFAULT_HEIGHT 150

/* §4.12.5's "the canvas element" asked of a VALUE, of the NODE, exactly as core/html/media_element.c asks its
   own — that is what the spec says a canvas element is, and a wrapper class would be a second answer. */
bool canvas_element_is(JSValueConst v)
{
    lxb_dom_element_t *el = element_of_value(v);

    if (el == NULL) return false;
    return lxb_html_tree_node_is(lxb_dom_interface_node(el), LXB_TAG_CANVAS);
}

/* The numeric value of one of the two content attributes — §4.12.5's "those values being interpreted in CSS
   pixels, and being updated as the attributes are set, changed, or removed", which is a READ of the attribute
   at every ask rather than a cached number that a `setAttribute` could leave behind. */
static uint32_t canvas_attr_dimension(JSValueConst canvas, const char *name, long long dflt)
{
    lxb_dom_element_t *el = element_of_value(canvas);
    size_t n = 0;
    const lxb_char_t *v;
    long long got;

    DCHECK(el != NULL, "§4.12.5's width/height were read off something that is not an element");
    v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)name, strlen(name), &n);
    got = element_reflect_ulong_value((const char *)v, n, dflt, true, 0, 0, false);
    DCHECK(got >= 0, "§4.12.5's unsigned long reflection answered a negative dimension");
    return (uint32_t)got;
}

/* THE ELEMENT'S §4.12.5 STATE, created where a flow first REACHES the element — core/html/media_element.c's
   rule for a record a flow may write, and here as there this is the record's ONLY creation site, so there is
   no write site left to miss. Every field is an ordinary property of a prototype-less slots object, so every
   write is captured by the running flow's COW delta and parks and resumes with it. */
static JSValue canvas_state(JSContext *ctx, JSValueConst canvas)
{
    JSValue st;

    DCHECK(g_atom_state != JS_ATOM_NULL, "a canvas's state was reached before §4.12.5 was declared");
    if (JS_GetOwnSlot(ctx, &st, canvas, g_atom_state) > 0) return st;

    st = idl_slots_new(ctx);
    CHECK(!JS_IsException(st), "§4.12.5: OOM building a canvas element's state");
    /* §4.12.5: "When its canvas context mode is none, a canvas element has no rendering context". */
    JS_SetPropertyStr(ctx, st, "mode", JS_NewInt32(ctx, CANVAS_MODE_NONE));
    JS_SetPropertyStr(ctx, st, "context", JS_NULL);
    /* The bitmap. `data` is JS_NULL until a context materializes it — see the header for why mode none needs
       no bytes. The two dimensions are the bitmap's OWN and are not re-read from the attributes: `set bitmap
       dimensions` writes the attributes back to match, so they agree, and storing them is what lets a resize
       be an event rather than a comparison made at every read. */
    JS_SetPropertyStr(ctx, st, "data", JS_NULL);
    JS_SetPropertyStr(ctx, st, "width", JS_NewUint32(ctx, 0));
    JS_SetPropertyStr(ctx, st, "height", JS_NewUint32(ctx, 0));
    /* §4.12.5.1.2's "Initially, when one of these bitmaps is created, its origin-clean flag must be set to
       true." Nothing in this build can clear it — see the header — and it is stored rather than derived so
       that the day an image source lands there is somewhere to clear. */
    JS_SetPropertyStr(ctx, st, "originClean", JS_TRUE);
    JS_DefinePropertyValue(ctx, (JSValue)canvas, g_atom_state, JS_DupValue(ctx, st),
                           JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    return st;
}

static int32_t canvas_st_int(JSContext *ctx, JSValueConst st, const char *name)
{
    JSValue v = JS_GetPropertyStr(ctx, st, name);
    int32_t out = 0;

    DCHECK(JS_IsNumber(v), "a canvas state field this component alone writes is not a Number");
    JS_ToInt32(ctx, &out, v);
    JS_FreeValue(ctx, v);
    return out;
}

CanvasContextMode canvas_context_mode(JSContext *ctx, JSValueConst canvas)
{
    JSValue st = canvas_state(ctx, canvas);
    CanvasContextMode m = (CanvasContextMode)canvas_st_int(ctx, st, "mode");

    JS_FreeValue(ctx, st);
    return m;
}

bool canvas_bitmap_get(JSContext *ctx, JSValueConst canvas, CanvasBitmap *out)
{
    JSValue st, data, clean, buf;
    size_t off = 0, len = 0, bpe = 0, want;
    uint8_t *base;
    bool ok = false;

    DCHECK(out != NULL, "a canvas bitmap was asked for with nowhere to put it");
    out->rgba = NULL; out->width = 0; out->height = 0; out->origin_clean = true;
    if (!canvas_element_is(canvas)) return false;

    st = canvas_state(ctx, canvas);
    data = JS_GetPropertyStr(ctx, st, "data");
    if (JS_IsNull(data)) { JS_FreeValue(ctx, data); JS_FreeValue(ctx, st); return false; }

    out->width  = (uint32_t)canvas_st_int(ctx, st, "width");
    out->height = (uint32_t)canvas_st_int(ctx, st, "height");
    clean = JS_GetPropertyStr(ctx, st, "originClean");
    out->origin_clean = JS_ToBool(ctx, clean);
    JS_FreeValue(ctx, clean);

    want = (size_t)out->width * (size_t)out->height * 4u;
    if (want == 0) { JS_FreeValue(ctx, data); JS_FreeValue(ctx, st); return true; }

    /* THE ARRAY IS THIS COMPONENT'S OWN AND IS NEVER HANDED TO THE PAGE, which is what makes the two
       assertions below statements about this codebase's logic rather than about a page's value: no script can
       reach the bitmap's `Uint8ClampedArray` to detach it or replace it, because `getImageData` COPIES out of
       it into a fresh ImageData and `putImageData` copies in. */
    buf = JS_GetTypedArrayBuffer(ctx, data, &off, &len, &bpe);
    CHECK(!JS_IsException(buf), "§4.12.5: a canvas bitmap's own array is detached — nothing may reach it");
    base = JS_GetArrayBuffer(ctx, &want, buf);
    CHECK(base != NULL, "§4.12.5: a canvas bitmap's own buffer answered no bytes");
    JS_FreeValue(ctx, buf);
    DCHECK(len >= (size_t)out->width * (size_t)out->height * 4u,
           "§4.12.5: a canvas bitmap's array is shorter than its own dimensions");
    out->rgba = base + off;
    ok = true;

    JS_FreeValue(ctx, data);
    JS_FreeValue(ctx, st);
    return ok;
}

/* HTML §4.12.5.1 "The 2D rendering context"'s "set bitmap dimensions to width and height", STEPS 2 TO 5 —
   step 1 is *reset the rendering context to its default state*, which belongs to the context and calls this.
   Splitting the algorithm at that line is the standard's own seam rather than a convenience: step 1 is defined
   over a RENDERING CONTEXT and steps 2-5 over the ELEMENT, and this file has no context to reset.
   THE NUMBER WAS §4.12.5 AT FOUR SITES AND §4.12.5 REFERENCES THIS ALGORITHM RATHER THAN DEFINING IT, which
   is the mis-aimed citation no quotation check can see: "The canvas element" is a real section with a real
   title whose own *2D context creation algorithm* step 5 SAYS `Set bitmap dimensions to the numeric values of
   target's width and height content attributes`, and the steps themselves are one subsection down. Verified by
   fetching both headings rather than by reading either number. */
int canvas_bitmap_set_dimensions(JSContext *ctx, JSValueConst canvas, uint32_t width, uint32_t height)
{
    JSValue st, arr, len;
    /* THE PRODUCT IS FORMED IN A WIDTH THAT CANNOT OVERFLOW AND ITS FOURTH FACTOR IS REFUSED BEFORE IT IS
       APPLIED — which is core/canvas/image_data.c's spelling of the identical question, ROUTED TO rather than
       answered a second way. `int64_t bytes = (int64_t)width * (int64_t)height * 4;` stood here and was
       SIGNED OVERFLOW for inputs a page reaches: core/dom/element.c's §2.6.1 reflection caps an `unsigned
       long` content attribute at 2147483647, and four times 2147483647 squared is about 1.84e19 against an
       INT64_MAX of 9.22e18 — so `<canvas width="2147483647" height="2147483647">` and `getContext("2d")` was
       undefined behaviour rather than a wrong number, which this project has already measured a compiler
       turning into a two-byte self-jump.
       TWO uint32 DIMENSIONS MULTIPLY EXACTLY IN 64 BITS AND FOUR TIMES THAT NEED NOT, so the multiply that
       would express an over-large request is the one that cannot be performed. §4.12.5.1's own step 2 is
       "Resize the output bitmap to the new width and height" and states NO failure arm, so this invents none:
       it routes into the SAME `-1` the array allocation below already returns, which the creation algorithm's
       caller already turns into the page's exception. Past this line and below the engine's own array-length
       ceiling the same RangeError comes back from the length coercion, so there is ONE refusal across the
       whole range — the sentence image_data.c writes about its own copy of this arithmetic. */
    uint64_t pixels = (uint64_t)width * (uint64_t)height;
    int64_t bytes;

    DCHECK(canvas_element_is(canvas),
           "§4.12.5.1's set bitmap dimensions was given something that is not a canvas");
    if (pixels > (uint64_t)INT64_MAX / 4u)
        return JS_ThrowRangeError(ctx, "a canvas of %u by %u pixels needs more storage than an ArrayBuffer "
                                       "can name", width, height), -1;
    bytes = (int64_t)(pixels * 4u);
    st = canvas_state(ctx, canvas);

    /* Step 2 — "Resize the output bitmap to the new width and height." A fresh array rather than a resize of
       the old one, because §4.12.5's own step 1 has already cleared the bitmap and every byte of the new one
       must be transparent black: growing in place would carry the old pixels into the region they used to
       occupy, which is the one thing a resize may not do. A zero-area bitmap holds JS_NULL and not an empty
       array — a state rather than an absence, exactly as core/graphics/raster_surface.h's NULL `px` is. */
    if (bytes == 0) {
        JS_SetPropertyStr(ctx, st, "data", JS_NULL);
    } else {
        len = JS_NewInt64(ctx, bytes);
        arr = JS_NewTypedArray(ctx, 1, (JSValueConst *)&len, JS_TYPED_ARRAY_UINT8C);
        JS_FreeValue(ctx, len);
        /* §A-PER-REALM-FACT: JS_NewTypedArray reaches `ctx`'s Uint8ClampedArray intrinsic, so a child
           navigable's canvas must reach this with ITS realm's context and never a cached one. */
        if (JS_IsException(arr)) { JS_FreeValue(ctx, st); return -1; }
        JS_SetPropertyStr(ctx, st, "data", arr);
    }
    JS_SetPropertyStr(ctx, st, "width", JS_NewUint32(ctx, width));
    JS_SetPropertyStr(ctx, st, "height", JS_NewUint32(ctx, height));

    /* Steps 3 to 5 — "If the numeric value of canvas's width content attribute differs from width, then set
       canvas's width content attribute to the shortest possible string representing width as a valid
       non-negative integer", and the same for height. THE COMPARISON IS AGAINST THE NUMERIC VALUE and not
       against the bytes, which is the step's own word: `<canvas width="0300">` is numerically 300, so a
       context creation on it writes nothing back and the attribute keeps the page's spelling. */
    if (canvas_attr_dimension(canvas, "width", CANVAS_DEFAULT_WIDTH) != width) {
        char buf[16];
        snprintf(buf, sizeof buf, "%u", width);
        element_attr_set(ctx, canvas, "width", buf);
    }
    if (canvas_attr_dimension(canvas, "height", CANVAS_DEFAULT_HEIGHT) != height) {
        char buf[16];
        snprintf(buf, sizeof buf, "%u", height);
        element_attr_set(ctx, canvas, "height", buf);
    }
    JS_FreeValue(ctx, st);
    return 0;
}


/* §4.12.5.1's *set bitmap dimensions* with the creation algorithm's own step 5 argument — "the numeric values of
   target's width and height content attributes". It is HERE and not at the context because §4.12.5's two
   defaults (300 and 150) are this file's, and a second reader of those attributes would be a second answer to
   the element's own dimensions — which core/html/html_element.c's R_CANVAS reflection row already gives once. */
int canvas_bitmap_set_dimensions_from_attributes(JSContext *ctx, JSValueConst canvas)
{
    return canvas_bitmap_set_dimensions(ctx, canvas,
                                        canvas_attr_dimension(canvas, "width", CANVAS_DEFAULT_WIDTH),
                                        canvas_attr_dimension(canvas, "height", CANVAS_DEFAULT_HEIGHT));
}

void canvas_bitmap_clear(JSContext *ctx, JSValueConst canvas, bool opaque)
{
    CanvasBitmap bm;

    if (!canvas_bitmap_get(ctx, canvas, &bm) || bm.rgba == NULL) return;
    if (!opaque) {
        /* *Clear canvas's bitmap to transparent black* — every component zero, which is what
           css-color-4's transparent black is and what §4.12.5.1.22 "Drawing model" step 1 starts from. */
        memset(bm.rgba, 0, (size_t)bm.width * (size_t)bm.height * 4u);
    } else {
        /* §4.12.5.1.2: "the bitmap of such a context starts off as opaque black instead of transparent
           black" — a context whose `alpha` is false. The colour is black and the alpha is fixed to 1.0, which
           the same paragraph states as "every fourth byte from getImageData() is always 255". */
        size_t i, n = (size_t)bm.width * (size_t)bm.height;
        memset(bm.rgba, 0, n * 4u);
        for (i = 0; i < n; i++) bm.rgba[i * 4u + 3u] = 255;
    }
}

/* ---- §4.12.5's getContext ---------------------------------------------------------------------------------- */

/* THE TABLE, AS THE TABLE. §4.12.5's `getContext` is a matrix whose columns are the canvas context mode and
   whose rows are the context id, and the cells are not alike: the `"2d"` row's mode-none cell RUNS an
   algorithm, its mode-2d cell returns the SAME OBJECT as last time, four cells return null and one throws.
   Writing it as the matrix is what keeps a cell from being derived from a neighbour it does not resemble.
   THE `"2d"` ROW CARRIES NO SUPPORTS-CONDITION AND THAT IS THE WHOLE REASON THIS MEMBER EXISTS TODAY. The
   `"webgl"`/`"webgl2"` row is written "if the user agent supports the WebGL feature in its current
   configuration" and the `"webgpu"` row the same way, so an agent without them falls to the table's
   unsupported-value row and returns null — a conformant answer. The `"2d"` row has no such clause, so
   `return null because this user agent cannot draw` is an arm the standard does not define and a canvas
   handed one would be this engine inventing a behaviour. (That run is this tree's OWN prose and is backticked
   rather than quoted for exactly that reason — a quotation channel cannot tell a fabricated spec sentence
   from a spelling being SHOWN, and backticks put it outside the channel by construction.) */
static JSValue js_canvas_get_context(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                     int magic)
{
    const char *id;
    JSValue st, existing, context;
    CanvasContextMode mode;
    bool is_2d;

    (void)magic;
    /* Web IDL §3.7.6's TypeError for a receiver that is not a canvas element — never a DCHECK, which would
       hand any page an abort switch for the whole engine through
       `HTMLCanvasElement.prototype.getContext.call(null, "2d")`. */
    if (!canvas_element_is(this_val))
        return JS_ThrowTypeError(ctx, "getContext called on something that is not an HTMLCanvasElement");

    id = JS_ToCString(ctx, argv[0]);
    if (id == NULL) return JS_EXCEPTION;
    is_2d = strcmp(id, "2d") == 0;
    JS_FreeCString(ctx, id);

    mode = canvas_context_mode(ctx, this_val);

    /* THE `placeholder` COLUMN THROWS FOR EVERY ROW, and it is written as the first test because that is the
       one cell every row shares. It is unreachable in this build — the only algorithm that enters that mode is
       `transferControlToOffscreen`, which this engine does not have — and it is spelled rather than elided so
       that the day that member lands the column is already answered. */
    if (mode == CANVAS_MODE_PLACEHOLDER)
        return JS_ThrowDOMException(ctx, "InvalidStateError",
                                    "getContext on a canvas whose bitmap has been transferred to an "
                                    "OffscreenCanvas");

    if (!is_2d) {
        /* EVERY OTHER ROW IS NULL IN THIS BUILD, AND EACH FOR ITS OWN REASON RATHER THAN BY A SHARED DEFAULT.
           `"webgl"`, `"webgl2"` and `"webgpu"` carry the table's own supports-condition, which this agent does
           not satisfy, so they fall to the unsupported-value row. `"bitmaprenderer"` carries NO condition, and
           a null for it is this engine answering the mode-2d column's cell only when that column is where the
           canvas already is; in mode none it is an arm the standard does not define, exactly as the `"2d"` row
           has none — so it is a NAMED RESIDUAL and not a settled answer.
           NAMED RESIDUAL — `"bitmaprenderer"` in context mode NONE. WHAT IS NOT COVERED: §4.12.5.2.2's
           ImageBitmapRenderingContext creation algorithm is not run, so this returns null where a conformant
           agent returns a context. WHAT THE NEXT DIFF BUILDS: §4.12.5.2.2's interface and its `transferFromImageBitmap`,
           which needs an `ImageBitmap` and therefore a decoded pixel source — core/image/image_header.c's own
           header says why it is not a decoder — so it is blocked behind the same component `drawImage` is.
           HOW ITS ABSENCE WOULD SHOW: a document that composites decoded bitmaps without a 2D context reads a
           null where it expects an object, and its next member access ends the flow. */
        return JS_NULL;
    }

    /* The `"2d"` row's mode-2d cell — "Return the same object as was returned the last time the method was
       invoked with this same first argument." The identity is observable (`c.getContext('2d') ===
       c.getContext('2d')`), so a second context minted here would be a wrong ANSWER and not a waste. */
    if (mode == CANVAS_MODE_2D) {
        st = canvas_state(ctx, this_val);
        existing = JS_GetPropertyStr(ctx, st, "context");
        JS_FreeValue(ctx, st);
        DCHECK(!JS_IsNull(existing), "a canvas in context mode 2d holds no rendering context");
        return existing;
    }
    if (mode != CANVAS_MODE_NONE) return JS_NULL;   /* the `"2d"` row's webgl/webgpu/bitmaprenderer cells */

    /* The `"2d"` row's mode-none cell, in its own three steps: "Let context be the result of running the 2D
       context creation algorithm given this and options. Set this's context mode to 2d. Return context." */
    context = canvas_rendering_context_2d_create(ctx, this_val, argv[1]);
    if (JS_IsException(context)) return context;

    st = canvas_state(ctx, this_val);
    JS_SetPropertyStr(ctx, st, "mode", JS_NewInt32(ctx, CANVAS_MODE_2D));
    JS_SetPropertyStr(ctx, st, "context", JS_DupValue(ctx, context));
    JS_FreeValue(ctx, st);
    return context;
}

/* ---- the declaration and the install ----------------------------------------------------------------------- */

void html_canvas_element_declare(JSContext *ctx)
{
    /* `RenderingContext? getContext(DOMString contextId, optional any options = null)`.
       POSITION 1 IS DECLARED AS THE DICTIONARY AND NOT AS `any`, WHICH HOISTS THE 2D CONTEXT CREATION
       ALGORITHM'S STEP 1 TO THIS BOUNDARY, and the reason is not tidiness. That step is "Let settings be the
       result of converting options to the dictionary type CanvasRenderingContext2DSettings", and a conversion
       READS THE PAGE'S GETTERS — `{ get alpha() { … } }` is one line a page can write. Run from inside the
       member's C body it would be a C activation hosting the page's code, which is the drive-to-completion
       this engine aborts on; run at the argument boundary it parks and resumes like every other declared
       conversion. The dictionary itself is core/canvas/canvas_rendering_context_2d.h's, read rather than
       copied.

       NAMED RESIDUAL — `options` THAT IS NEITHER AN OBJECT, NULL NOR UNDEFINED. WHAT IS NOT COVERED: §4.12.5's
       getContext step 1 is "If options is not an object, then set options to null", so a browser answers
       `canvas.getContext("2d", 5)` with a context carrying default settings; the declaration above converts
       position 1 before this body runs, and Web IDL §3.2.17 Dictionary types throws a TypeError for a value
       that is not an object and not null or undefined. The same hoist converts `options` for a contextId whose
       row never reads it, so `getContext("webgl", { colorSpace: "bogus" })` throws where a browser returns
       null. WHAT THE NEXT DIFF BUILDS: the conversion moved back behind the table, which needs an
       algorithm-facing entry to §3.2.17 that a member body can drive — `idl_dict_walk_start`/
       `idl_dict_walk_take` are that walk, and driving one from a member means this member becomes a STEP
       MACHINE, because the walk can park on a page getter. That is the whole of the work and it is why the
       hoist stands today rather than a per-body conversion, which would run those getters from a C activation.
       HOW ITS ABSENCE WOULD SHOW: a `getContext` whose second argument is a number or a string raises a
       TypeError at the call instead of returning a context, and a flow that would have gone on drawing ends
       there. */
    static const IdlArgType GET_CONTEXT[2] = { IDL_DOMSTRING, IDL_DICT };

    DCHECK(g_id_get_context < 0, "html_canvas_element_declare ran twice — §4.12.5 is declared once per AGENT");
    g_id_get_context = idl_method_id_dict(ctx, GET_CONTEXT, 2, CTX2D_SETTINGS, CTX2D_SETTINGS_N,
                                          js_canvas_get_context, 0);
    idl_optional_from(1);

    g_state_key = JS_NewSymbol(ctx, "canvasElementState", false);
    CHECK(!JS_IsException(g_state_key), "§4.12.5: the canvas element state slot key allocation failed");
    g_atom_state = JS_ValueToAtom(ctx, g_state_key);
    CHECK(g_atom_state != JS_ATOM_NULL, "§4.12.5: the canvas element state slot key could not be interned");

    /* `element`, THE ROW THAT RELEASES THIS, and not this file — html_canvas_element_free is reached from
       html_element_free and thence from element_free, which is the `element` row's release column, and this
       component has no row of its own. core/html/custom_elements.c declares under the same name from the same
       release, one line over in element_free. RETIREMENT: as at the declaration in
       core/canvas/canvas_rendering_context_2d.c. */
    agent_state_id("element", &g_id_get_context, "§4.12.5's getContext declaration");
}

void html_canvas_install(JSContext *ctx, JSValueConst canvas_proto)
{
    DCHECK(g_id_get_context >= 0, "§4.12.5's getContext was installed before it was declared");
    DCHECK(JS_IsObject(canvas_proto), "§4.12.5's members were installed with no HTMLCanvasElement.prototype");
    idl_install_method(ctx, canvas_proto, "getContext", g_id_get_context);
}

void html_canvas_element_free(JSRuntime *rt)
{
    /* The slot key is the AGENT's, so it is released with the agent — a Symbol nobody frees is a live GC
       object the runtime's own walk counts as a leak. */
    if (g_atom_state != JS_ATOM_NULL) JS_FreeAtomRT(rt, g_atom_state);
    g_atom_state = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_state_key);
    g_state_key = JS_UNDEFINED;
    g_id_get_context = -1;
    /* AND THE CASCADE REACHED THIS FILE — element_free reaches html_element_free reaches this, and its
       last line may put the entry above back only because this says so. The two slots above it are
       this file's own and are not declared to core/agent_state.h at all. */
    agent_state_reached("element");
}
