/* HTML §8.11.1 "The ImageData interface" — see image_data.h for why it is completable with no rendering
   context and why it is the platform name this corpus most costs. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/canvas/image_data.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/concolic.h"
#include "solver/cow.h"

/* §8.11.1's five observables. `data` is the ONE owned reference — the ImageDataArray itself, which *initialize
   an ImageData object* step 1.3 sets to the page's OWN object ("This step does not set this's data to a copy
   of data. It sets it to the actual ImageDataArray object passed as data") and step 2.1/2.2 sets to a freshly
   allocated one. The other four are plain scalars, which is what lets the COW record name exactly one slot. */
typedef struct {
    JSValue  data;
    uint32_t width;
    uint32_t height;
    uint8_t  pixel_format;   /* an index into IMAGE_DATA_PIXEL_FORMATS */
    uint8_t  color_space;    /* an index into IMAGE_DATA_COLOR_SPACES */
} ImageDataBox;

/* The one statement of what the record owns — read by the COW capture, by the finalizer and by the gc_mark, so
   a field added to one and not the others is caught by reading them together. */
static const uint16_t IMAGE_DATA_OFF[] = { (uint16_t)offsetof(ImageDataBox, data) };
static const CowRecord IMAGE_DATA_REC = { sizeof(ImageDataBox), IMAGE_DATA_OFF, 1 };

static JSClassID g_class;
static int g_id_ctor = -1;

/* §8.11.1's `enum ImageDataPixelFormat { "rgba-unorm8", "rgba-float16" };` and Canvas's `PredefinedColorSpace`,
   in the IDL's own list order — which is what Web IDL §3.2.18 Enumeration types' fork numbers its outcomes by,
   so the order is the declaration and is not a ranking made here. */
static const char *const IMAGE_DATA_PIXEL_FORMATS[] = { "rgba-unorm8", "rgba-float16", NULL };
static const char *const IMAGE_DATA_COLOR_SPACES[] = {
    "srgb", "srgb-linear", "display-p3", "display-p3-linear", NULL
};

/* §8.11.1's `dictionary ImageDataSettings`. `colorSpace` has NO default — the algorithm's steps 6/7/8 are a
   three-way chain on whether it EXISTS, and a default here would make the first arm always taken and the
   `defaultColorSpace` parameter unreachable. `pixelFormat` has one and the IDL writes it. */
#define IMAGE_DATA_SETTINGS_N 2
static const IdlDictMember IMAGE_DATA_SETTINGS[IMAGE_DATA_SETTINGS_N] = {
    { "colorSpace",  IDL_ENUM, false, IMAGE_DATA_COLOR_SPACES },
    { "pixelFormat", IDL_ENUM, false, IMAGE_DATA_PIXEL_FORMATS, 0, NULL, IDL_DEFAULT_STRING, "rgba-unorm8" },
};

/* §8.11.1's two constructor entries, as ONE declaration with Web IDL §3.6's distinguishing argument index at
   position 0 — see core/idl_args.h's IDL_ULONG_OR_IMAGE_DATA_ARRAY for why the split is a TYPE row and why
   position 2 reads the entry rather than testing a value.

       constructor(unsigned long sw, unsigned long sh, optional ImageDataSettings settings = {});
       constructor(ImageDataArray data, unsigned long sw, optional unsigned long sh,
                   optional ImageDataSettings settings = {});

   Position 1 is `unsigned long` in BOTH, which Web IDL §2.5.8 Overloading requires of every index below the
   distinguishing one; position 3 exists only on the longer entry, and steps 3-4 leave only that entry standing
   at the arity which reaches it, so it is a plain dictionary. */
static const IdlArgType IMAGE_DATA_CTOR[4] = {
    IDL_ULONG_OR_IMAGE_DATA_ARRAY, IDL_UNSIGNED_LONG, IDL_ULONG_OR_DICT_BY_ENTRY, IDL_DICT
};

static ImageDataBox *image_data_box(JSValueConst v)
{
    ImageDataBox *b = JS_GetOpaque(v, g_class);

    /* CLAUDE.md §A-COMPONENT'S-OWN-C-RECORD-TIME-TRAVELS: the capture belongs in the ACCESSOR, so a record a
       flow has reached is one it may write and there is no write site left to miss. The typed array's own
       ELEMENT writes are captured without this component's help — a page's `img.data[0] = 255` is an ordinary
       indexed store that cow_capture already sees. */
    if (b) cow_capture_host_record(v, b, &IMAGE_DATA_REC);
    return b;
}

/* THE BRAND CHECK, AND IT IS A `TypeError` RATHER THAN AN ASSERT — CLAUDE.md §WHOSE-BYTES-STATE-THE-VALUE. A
   receiver is whatever the page wrote, and a forcing solver reaches unusual ones constantly. */
static ImageDataBox *image_data_this(JSContext *ctx, JSValueConst this_val)
{
    ImageDataBox *b = image_data_box(this_val);

    if (!b) { JS_ThrowTypeError(ctx, "an ImageData attribute was reached on something that is not an "
                                     "ImageData"); return NULL; }
    return b;
}

static void image_data_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    ImageDataBox *b = JS_GetAnyOpaque(val, &id);

    (void)id;
    if (!b) return;
    JS_FreeValueRT(rt, b->data);
    free(b);
}

static void image_data_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID id = 0;
    ImageDataBox *b = JS_GetAnyOpaque(val, &id);

    (void)id;
    DCHECK(b != NULL, "an ImageData was marked with no record — image_data_alloc attaches it before the object "
                      "can reach a collection");
    JS_MarkValue(rt, b->data, mark_func);
}

/* Web IDL §3.8 "Platform objects implementing interfaces"'s *internally create a new object implementing the
   interface*, in its own order — the same three arms path_2d.c takes, and the LAST of them is why this reads
   the function's realm rather than `ctx`: `Reflect.construct(ImageData, […], otherRealmFunction)` must take the
   OTHER realm's ImageData.prototype. */
static JSValue image_data_alloc(JSContext *ctx, JSValueConst new_target)
{
    JSValue proto, obj;
    ImageDataBox *b;

    if (JS_IsUndefined(new_target)) {
        proto = JS_GetClassProto(ctx, g_class);
    } else {
        proto = JS_GetPropertyStr(ctx, new_target, "prototype");
        if (JS_IsException(proto)) return proto;
        if (!JS_IsObject(proto)) {
            JSContext *target_realm = JS_GetFunctionRealm(ctx, new_target);
            JS_FreeValue(ctx, proto);
            if (!target_realm) return JS_EXCEPTION;
            proto = JS_GetClassProto(target_realm, g_class);
        }
    }
    obj = JS_NewObjectProtoClass(ctx, proto, g_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) return obj;

    b = calloc(1, sizeof(*b));
    CHECK(b != NULL, "an ImageData's record could not be allocated");
    b->data = JS_UNDEFINED;
    JS_SetOpaque(obj, b);
    return obj;
}

/* Which of an enumeration's declared values a converted member holds, as its INDEX. The conversion already
   refused everything outside the list — Web IDL §3.2.18 Enumeration types' "If S is not one of E's enumeration
   values, then throw a TypeError" — so a value reaching here is one of them and the scan is total. */
static int image_data_enum_index(JSContext *ctx, JSValueConst dict, const char *name,
                                 const char *const *values, int dflt)
{
    JSValue v = idl_dict_get(ctx, dict, name);
    const char *s;
    int i;

    if (JS_IsUndefined(v)) { JS_FreeValue(ctx, v); return dflt; }
    s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s) return dflt;
    for (i = 0; values[i]; i++)
        if (!strcmp(s, values[i])) { JS_FreeCString(ctx, s); return i; }
    DFAIL("a declared IDL_ENUM dictionary member held a string outside its own declared values — Web IDL "
          "Web IDL §3.2.18 Enumeration types refuses one at the conversion, so this member reached a body "
          "without "
          "being converted");
    JS_FreeCString(ctx, s);
    return dflt;
}

/* §8.11.1's *initialize an ImageData object*, in the standard's own order and with its own parameter names.
   `source` is JS_UNDEFINED for the arm that was not given one; `default_color_space` is -1 for the same. */
/* `pixel_format` IS PASSED AND NOT RE-READ. Both §8.11.1 algorithms that reach here read
   `settings["pixelFormat"]` — the longer constructor at its own step 1, for the bytes per pixel — and a second
   read HERE would be a second derivation of one fact, free to disagree with the first the day either side
   gains a condition. The standard reading it twice is the standard describing one value, not two. */
static int image_data_initialize(JSContext *ctx, ImageDataBox *b, uint32_t pixels_per_row, uint32_t rows,
                                 JSValueConst settings, int pixel_format, JSValueConst source,
                                 int default_color_space)
{
    int color_space;
    JSValue cs;

    if (!JS_IsUndefined(source)) {                                              /* step 1 */
        int kind = JS_GetTypedArrayType(source);

        /* Steps 1.1 and 1.2 — the pixel format and the array's own type must agree, and a mismatch is an
           "InvalidStateError" rather than the TypeError a brand check throws: Web IDL §3.6 already established this is
           ONE of ImageDataArray's two members, and the refusal here is about which. */
        if (pixel_format == 0 && kind != JS_TYPED_ARRAY_UINT8C)
            return JS_ThrowTypeError(ctx, "InvalidStateError: a \"rgba-unorm8\" ImageData needs a "
                                          "Uint8ClampedArray"), -1;
        if (pixel_format == 1 && kind != JS_TYPED_ARRAY_FLOAT16)
            return JS_ThrowTypeError(ctx, "InvalidStateError: a \"rgba-float16\" ImageData needs a "
                                          "Float16Array"), -1;
        JS_FreeValue(ctx, b->data);
        b->data = JS_DupValue(ctx, source);                                    /* step 1.3 */
    } else {                                                                    /* step 2 */
        JSValue len = JS_NewInt64(ctx, (int64_t)rows * (int64_t)pixels_per_row);
        JSValue arr = JS_NewTypedArray(ctx, 1, (JSValueConst *)&len,
                                       pixel_format == 1 ? JS_TYPED_ARRAY_FLOAT16 : JS_TYPED_ARRAY_UINT8C);

        /* §A-PER-REALM-FACT: `JS_NewTypedArray` reaches `ctx->class_proto[…]`, which is a member of the
           JSContext — so a child navigable's `new ImageData(…)` gets THAT realm's Uint8ClampedArray, and the
           `ctx` passed here must be the member's own and never a cached one. */
        JS_FreeValue(ctx, len);
        /* Step 2.3 — "If the storage ArrayBuffer could not be allocated, then rethrow the RangeError thrown by
           JavaScript, and return." The engine's own allocator raised it; it is the page's to see. */
        if (JS_IsException(arr)) return -1;
        JS_FreeValue(ctx, b->data);
        b->data = arr;
    }
    b->width  = pixels_per_row;                                                 /* step 3 */
    b->height = rows;                                                           /* step 4 */
    b->pixel_format = (uint8_t)pixel_format;                                    /* step 5 */

    /* Steps 6, 7 and 8 — a three-way chain on whether `settings["colorSpace"]` EXISTS, which is why that
       member declares no default: a default would make step 6 always taken and steps 7 and 8 unreachable. */
    cs = idl_dict_get(ctx, settings, "colorSpace");
    if (!JS_IsUndefined(cs)) {
        JS_FreeValue(ctx, cs);
        color_space = image_data_enum_index(ctx, settings, "colorSpace", IMAGE_DATA_COLOR_SPACES, 0);  /* 6 */
    } else {
        JS_FreeValue(ctx, cs);
        color_space = default_color_space >= 0 ? default_color_space : 0;                          /* 7, 8 */
    }
    b->color_space = (uint8_t)color_space;
    return 0;
}

/* THE ONE DECLARED ARGUMENT THAT MAY BE UNKNOWN EXTERNAL INPUT AND IS READ AS A NUMBER. Web IDL §3.2.4's
   conversion produces a Number and core/idl_args.c's boundary passes a concolic through as itself, so exactly
   two values reach here and the DCHECK over that pair is a statement about this codebase's own logic.
 *
 * NAMED RESIDUAL — A DIMENSION THAT IS UNKNOWN EXTERNAL INPUT. WHAT IS NOT COVERED: `sw`/`sh` are recorded as
 * the concrete EXAMPLE the run computed rather than as themselves, so the allocated bitmap has the example's
 * size and a page that branches on `img.width` afterwards takes the arm the example names rather than forking.
 * WHAT THE NEXT DIFF BUILDS: §8.11.1's own step 1 ("If one or both of sw and sh are zero, then throw an
 * IndexSizeError") is a PREDICATE over the unknown and belongs at the branch seam like any other, and the
 * allocation behind it needs a length that is a concolic — which is the same Web IDL §3.2.26-over-unknown capability
 * the argument machine's own ImageDataArray arm names, reached from the other side. HOW ITS ABSENCE WOULD SHOW:
 * a page that sizes a canvas from injected state and then tests `img.width` against a constant is decided
 * rather than forked, observable as that document's fork census naming the injected source at the sizing read
 * and at no branch behind it. */
static uint32_t image_data_dim(JSContext *ctx, JSValueConst v)
{
    JSValue ex;
    uint32_t out = 0;

    if (JS_IsNumber(v)) {
        int ok = JS_ToUint32(ctx, &out, v);
        DCHECK(ok == 0, "a Number at a declared `unsigned long` position did not convert to a uint32");
        return out;
    }
    DCHECK(concolic_is(v),
           "an ImageData dimension was neither a Number nor unknown external input — Web IDL §3.2.4's "
           "conversion produces the first and the IDL boundary passes the second through as itself, and there "
           "is no third thing that reaches a converted position");
    ex = concolic_example(ctx, v);
    if (JS_IsNumber(ex)) {
        int ok = JS_ToUint32(ctx, &out, ex);
        DCHECK(ok == 0, "a concolic's Number example did not convert to a uint32");
        JS_FreeValue(ctx, ex);
        return out;
    }
    JS_FreeValue(ctx, ex);
    /* NOT A GUESS. An unknown with no example has no dimension, and zero is the value §8.11.1's own step 1
       refuses — which is the algorithm's answer for a size it has no number for, and not a plausible datum. */
    return 0;
}

/* §8.11.1's TWO constructor algorithms. Which one runs is Web IDL §3.6's answer and not this body's: the
   declared types at positions 0 and 2 carry the split, so by here the surviving entry is already legible from
   the SHAPE of what was placed — a typed array at position 0 is the longer entry and a Number is the shorter
   one, because Web IDL §3.6 step 12's typed-array clause is exactly what chose between them. */
static JSValue js_image_data_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv,
                                  int magic)
{
    JSValue out;
    ImageDataBox *b;
    int kind;

    (void)magic;
    /* EVERY POSITION THIS MEMBER DECLARES IS CONVERTED, WHATEVER THE PAGE PASSED, and that is the argument
       machine's own rule rather than a guarantee this body arranges: Web IDL §3.6 step 16.1 places a declared default
       at every position behind the ones the page reached, and a trailing DICTIONARY position is converted even
       when omitted because Web IDL §3.2.17 Dictionary types over `undefined` is an all-defaults dictionary and
       not an
       absent argument. Both of this member's dictionary-capable positions are behind its required two, so the
       vector is always four long and the settings each entry reads is simply its own index — `argc > N`
       guards here would be dead code that reads as a live choice. WHICH of the four positions hold a MISSING
       argument is a separate question and is asked of idl_arg_given, never of this count. */
    DCHECK(argc == 4, "§8.11.1's constructor was handed an argument vector that is not its four declared "
                      "positions — core/idl_args.c converts every declared position and extends the count "
                      "over the trailing dictionary, so a shorter vector is a member declared with a different "
                      "type list than this body reads");
    out = image_data_alloc(ctx, new_target);
    if (JS_IsException(out)) return out;
    b = image_data_box(out);
    DCHECK(b != NULL, "a freshly allocated ImageData carried no record");

    /* WHAT CAN STAND AT POSITION 0 ONCE WEB IDL §3.6 HAS RUN, which is what makes reading it here a DISPATCH rather
       than a second resolution. The declared split converted this position to the surviving entry's own type:
       the longer entry crosses the ImageDataArray as itself, so the value is a Uint8ClampedArray or a
       Float16Array; the shorter entry's `unsigned long` leaves a Number, or unknown external input, which the
       IDL boundary passes through as itself and whose longer arm crashed at the conversion by name rather than
       reaching here. A typed array of any OTHER kind cannot appear — step 12's clause matches the name against
       `ImageDataArray`'s two flattened members and everything else falls to the numeric fallback, so
       `new ImageData(new Int8Array(16), 2)` is the SHORTER entry and a `sw` of NaN-to-zero. */
    kind = JS_GetTypedArrayType(argv[0]);
    DCHECK(kind == JS_TYPED_ARRAY_UINT8C || kind == JS_TYPED_ARRAY_FLOAT16 ||
           JS_IsNumber(argv[0]) || concolic_is(argv[0]),
           "§8.11.1's constructor was handed a position 0 that is neither one of ImageDataArray's two members "
           "nor a converted `unsigned long` nor unknown external input — Web IDL §3.6 step 12 chose the entry "
           "and the conversion placed that entry's own type, so a third kind of value here is a declared split "
           "that did not run");
    if (kind == JS_TYPED_ARRAY_UINT8C || kind == JS_TYPED_ARRAY_FLOAT16) {
        /* `new ImageData(data, sw, sh, settings)` — eight steps, in the standard's order. */
        JSValueConst settings = argv[3];
        int pixel_format = image_data_enum_index(ctx, settings, "pixelFormat", IMAGE_DATA_PIXEL_FORMATS, 0);
        size_t bytes_per_pixel = pixel_format == 1 ? 8 : 4;                     /* step 1 */
        size_t byte_len = 0, length;
        uint32_t sw = image_data_dim(ctx, argv[1]), height;

        /* Step 2 — "Let length be the buffer source byte length of data". It is the VIEW's byte length
           and not the backing buffer's: a Uint8ClampedArray built over an offset into a larger buffer is
           exactly what the corpus's own entry-B call site passes, so reading the buffer would measure the
           page's whole frame store instead of the window it handed us. */
        {
            JSValue buf = JS_GetTypedArrayBuffer(ctx, argv[0], NULL, &byte_len, NULL);
            if (JS_IsException(buf)) { JS_FreeValue(ctx, out); return JS_EXCEPTION; }
            JS_FreeValue(ctx, buf);
        }
        if (byte_len == 0 || byte_len % bytes_per_pixel != 0) {                 /* step 3 */
            JS_FreeValue(ctx, out);
            return JS_ThrowTypeError(ctx, "InvalidStateError: an ImageData's data length must be a nonzero "
                                          "multiple of the bytes per pixel");
        }
        length = byte_len / bytes_per_pixel;                                    /* step 4 */
        /* Step 5 — "If length is not an integral multiple of sw". The standard's own note says the length is
           guaranteed nonzero by step 3, so an `sw` of zero throws HERE rather than dividing. */
        if (sw == 0 || length % sw != 0) {
            JS_FreeValue(ctx, out);
            return JS_ThrowTypeError(ctx, "IndexSizeError: an ImageData's data length is not a multiple of "
                                          "its width");
        }
        height = (uint32_t)(length / sw);                                       /* step 6 */
        /* Step 7 — "If sh was given and its value is not equal to height". GIVEN is Web IDL §3.6's own word, and the
           argument machine records it: idl_arg_given reads the bit step 15.4.2 set, never the argument count,
           so `new ImageData(u8, 2, undefined)` is an ABSENT sh and not a zero one. */
        if (idl_arg_given(argc, argv, 2) && image_data_dim(ctx, argv[2]) != height) {
            JS_FreeValue(ctx, out);
            return JS_ThrowTypeError(ctx, "IndexSizeError: an ImageData's given height does not match its "
                                          "data length");
        }
        if (image_data_initialize(ctx, b, sw, height, settings, pixel_format, argv[0], -1) < 0) {  /* 8 */
            JS_FreeValue(ctx, out);
            return JS_EXCEPTION;
        }
        return out;
    }

    /* `new ImageData(sw, sh, settings)` — three steps. */
    {
        uint32_t sw = image_data_dim(ctx, argv[0]), sh = image_data_dim(ctx, argv[1]);
        JSValueConst settings = argv[2];

        if (sw == 0 || sh == 0) {                                               /* step 1 */
            JS_FreeValue(ctx, out);
            return JS_ThrowTypeError(ctx, "IndexSizeError: an ImageData's width and height must both be "
                                          "nonzero");
        }
        /* Steps 2 and 3 — *initialize* with no `source`, whose step 2 allocates the array and whose
           allocation is already zero-filled, which IS "initialize the image data of this to transparent
           black" for both pixel formats. */
        if (image_data_initialize(ctx, b, sw, sh, settings,
                                  image_data_enum_index(ctx, settings, "pixelFormat",
                                                        IMAGE_DATA_PIXEL_FORMATS, 0),
                                  JS_UNDEFINED, -1) < 0) {
            JS_FreeValue(ctx, out);
            return JS_EXCEPTION;
        }
        return out;
    }
}

/* §8.11.1's five readonly attributes. The magic IS the member. */
typedef enum { IMG_WIDTH = 0, IMG_HEIGHT, IMG_DATA, IMG_PIXEL_FORMAT, IMG_COLOR_SPACE } ImageDataAttr;

static JSValue js_image_data_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    ImageDataBox *b = image_data_this(ctx, this_val);

    if (!b) return JS_EXCEPTION;
    switch (magic) {
    case IMG_WIDTH:        return JS_NewUint32(ctx, b->width);
    case IMG_HEIGHT:       return JS_NewUint32(ctx, b->height);
    case IMG_DATA:         return JS_DupValue(ctx, b->data);
    case IMG_PIXEL_FORMAT: return JS_NewString(ctx, IMAGE_DATA_PIXEL_FORMATS[b->pixel_format]);
    case IMG_COLOR_SPACE:  return JS_NewString(ctx, IMAGE_DATA_COLOR_SPACES[b->color_space]);
    default: break;
    }
    DFAIL("an ImageData attribute ran with a magic outside its own list");
    return JS_UNDEFINED;
}

void image_data_init(JSContext *ctx)
{
    JSClassDef def = { "ImageData", image_data_finalizer, image_data_gc_mark };

    DCHECK(g_class == 0, "image_data_init ran twice — §8.11.1's class is declared once per AGENT");
    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    JS_NewClass(JS_GetRuntime(ctx), g_class, &def);

    g_id_ctor = idl_method_id_dict(ctx, IMAGE_DATA_CTOR, 4, IMAGE_DATA_SETTINGS,
                                   IMAGE_DATA_SETTINGS_N,
                                   js_image_data_ctor, 0);
    /* BOTH ENTRIES FIRST TURN OPTIONAL AT INDEX 2, which is why these two numbers are the same and why the
       assert behind idl_overload_split_optional_from had to stop demanding they differ — see its own
       paragraph, and Web IDL §2.5.8 Overloading's bound on the shared prefix, which ends at the DISTINGUISHING index
       and not at the split. */
    idl_optional_from(2);
    idl_overload_length_split_at(2);
    idl_overload_split_optional_from(2);

    agent_state_class("image_data", &g_class, "§8.11.1's ImageData class, and the declaration latch");
    agent_state_id("image_data", &g_id_ctor, "§8.11.1's constructor declaration");
    realm_declare_intrinsic(image_data_install_realm);
}

/* Web IDL §3.7.3's interface prototype object for §8.11.1, its §3.7.1 interface object, and §3.8's property
 * reference for the one name — for ONE realm.
 *
 * NAMED RESIDUAL — §8.11.1's SERIALIZATION STEPS. WHAT IS NOT COVERED: `ImageData` is declared `[Serializable]`
 * and this component registers none of §8.11.1's serialization or deserialization steps, so a `structuredClone`
 * of one, or a `postMessage` carrying one, reaches HTML §2.7's StructuredSerializeInternal with an object it
 * has no steps for. WHAT THE NEXT DIFF BUILDS: the five `[[Data]]`/`[[Width]]`/`[[Height]]`/`[[ColorSpace]]`/
 * `[[PixelFormat]]` slots §8.11.1 names, with `[[Data]]` taken through the sub-serialization of the `data`
 * attribute so the typed array travels by the machinery that already carries one, and the deserialization
 * steps that read them back. HOW ITS ABSENCE WOULD SHOW: a document that hands a worker its pixels — which is
 * the ordinary shape of every image pipeline in this corpus — reaches the clone and its flow ends there,
 * having already produced the bitmap it was going to send. */
void image_data_install_realm(JSContext *ctx)
{
    JSValue proto, prev, ctor, global;

    DCHECK(g_class != 0, "a realm asked for ImageData.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_class);
    DCHECK(JS_IsNull(prev), "image_data_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "ImageData.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "ImageData");
    idl_install_accessor(ctx, proto, "width",       js_image_data_get, IMG_WIDTH,        -1);
    idl_install_accessor(ctx, proto, "height",      js_image_data_get, IMG_HEIGHT,       -1);
    idl_install_accessor(ctx, proto, "data",        js_image_data_get, IMG_DATA,         -1);
    idl_install_accessor(ctx, proto, "pixelFormat", js_image_data_get, IMG_PIXEL_FORMAT, -1);
    idl_install_accessor(ctx, proto, "colorSpace",  js_image_data_get, IMG_COLOR_SPACE,  -1);

    global = JS_GetGlobalObject(ctx);
    DCHECK(g_id_ctor >= 0, "ImageData's interface object was built before image_data_init declared its "
                           "constructor");
    ctor = idl_step_constructor(ctx, "ImageData", g_id_ctor);
    CHECK(!JS_IsException(ctor), "the ImageData interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    idl_define_global_property_reference(ctx, global, "ImageData", ctor);
    JS_FreeValue(ctx, global);

    JS_SetClassProto(ctx, g_class, proto);      /* the realm owns it from here */
}

void image_data_free(void)
{
    /* The prototype is the REALMS' and the pool entry is the agent's. THE CLASS ID COMES BACK TOO, because
       image_data_init consults it to decide whether it has anything to do — see core/agent_state.h. */
    g_class = 0;
    g_id_ctor = -1;
}
