/* HTML §4.10.5.1.19 "Image Button state (type=image)": the image an Image Button fetches. See
   image_input_type.h. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>

#include "check.h"
#include "quickjs.h"
#include "solver/dom_cow.h"     /* the taint of a `src` a script composed out of unknown input */
#include "solver/endpoint.h"
#include "solver/engine.h"
#include "core/idl_slots.h"
#include "core/url/url.h"
#include "core/dom/document.h"
#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/fetch/fetch.h"
#include "core/frame/policy_container.h"
#include "core/html/html_form.h"
#include "core/html/image_input_type.h"
#include "core/image/image_header.h"
#include "core/image/png_decode.h"

/* The private key the record hangs off. The record is an ordinary property of the element's wrapper, so every
   write below is captured by the running flow's COW delta. */
static JSValue g_key = JS_UNDEFINED;
static JSAtom g_atom = JS_ATOM_NULL;
static int g_ready;

static bool ii_in_state(const lxb_dom_node_t *n)
{
    return html_form_input_state(n) == INPUT_STATE_IMAGE;
}

/* The record, or JS_UNDEFINED when no trigger has reached the element. Owned. */
static JSValue ii_record_get(JSContext *ctx, JSValueConst wrap)
{
    JSValue st;

    DCHECK(g_ready, "an Image Button's record was read before image_input_declare ran");
    if (JS_GetOwnSlot(ctx, &st, wrap, g_atom) > 0) return st;
    return JS_UNDEFINED;
}

/* The record, created where a trigger first reaches the element. `inState` is whether the type attribute was
   in the Image Button state at the last trigger seen, and `srcSeen` the src value (JS_NULL for absent) the
   last time it was: the second trigger event compares against it. `gen` names the latest fetch. Owned. */
static JSValue ii_record(JSContext *ctx, JSValueConst wrap)
{
    JSValue st = ii_record_get(ctx, wrap);

    if (!JS_IsUndefined(st)) return st;
    st = idl_slots_new(ctx);
    CHECK(!JS_IsException(st), "§4.10.5.1.19: OOM building an Image Button's record");
    JS_SetPropertyStr(ctx, st, "inState", JS_FALSE);
    JS_SetPropertyStr(ctx, st, "srcSeen", JS_NULL);
    JS_SetPropertyStr(ctx, st, "image", JS_NewInt32(ctx, IMAGE_INPUT_NO_IMAGE));
    JS_SetPropertyStr(ctx, st, "gen", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, st, "naturalWidth", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, st, "naturalHeight", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, st, "bytes", JS_UNDEFINED);
    JS_DefinePropertyValue(ctx, (JSValue)wrap, g_atom, JS_DupValue(ctx, st),
                           JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    return st;
}

static int32_t ii_int(JSContext *ctx, JSValueConst st, const char *name)
{
    JSValue v = JS_GetPropertyStr(ctx, st, name);
    int32_t n = 0;

    DCHECK(JS_IsNumber(v), "an Image Button's record holds a non-number where this file writes only numbers");
    JS_ToInt32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

static double ii_num(JSContext *ctx, JSValueConst st, const char *name)
{
    JSValue v = JS_GetPropertyStr(ctx, st, name);
    double d = 0.0;

    DCHECK(JS_IsNumber(v), "an Image Button's record holds a non-number where this file writes only numbers");
    JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    return d;
}

/* Sets the image and drops the data of any earlier one. */
static void ii_set_image(JSContext *ctx, JSValueConst st, ImageInputImage image)
{
    JS_SetPropertyStr(ctx, st, "image", JS_NewInt32(ctx, image));
    JS_SetPropertyStr(ctx, st, "naturalWidth", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, st, "naturalHeight", JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, st, "bytes", JS_UNDEFINED);
}

/* The src attribute's value as a JS string, or JS_NULL when absent. Lexbor answers NULL for an empty value
   too, so presence is asked separately. Owned. */
static JSValue ii_src(JSContext *ctx, lxb_dom_element_t *el)
{
    size_t n = 0;
    const lxb_char_t *v;
    JSValue s;

    if (!lxb_dom_element_has_attribute(el, (const lxb_char_t *)"src", 3)) return JS_NULL;
    v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"src", 3, &n);
    s = JS_NewStringLen(ctx, v ? (const char *)v : "", v ? n : 0);
    CHECK(!JS_IsException(s), "§4.10.5.1.19: OOM reading an Image Button's src");
    return s;
}

/* HTML §2.4.2 "Parsing URLs"' encoding-parse a URL relative to `doc`, serialized; NULL is failure. Owned. */
static char *ii_url(const lxb_dom_document_t *doc, const char *src, size_t n)
{
    UrlRecord base, rec;
    const char *base_url = document_base_url_of(doc);
    bool have_base;
    char *abs = NULL;

    url_record_init(&base);
    have_base = base_url && url_parse(&base, base_url, strlen(base_url), NULL);
    url_record_init(&rec);
    if (url_parse(&rec, src, n, have_base ? &base : NULL)) abs = url_serialize(&rec, false);
    url_record_free(&rec);
    url_record_free(&base);
    return abs;
}

/* processResponseEndOfBody's two arms, for the fetch `gen` names: "If the download was successful and the
   image is available, queue an element task on the user interaction task source given the input element to
   fire an event named load"; otherwise the same with `error`. Every fetch's steps run, so a fetch a later
   trigger superseded still fires; only the latest one writes the element's image. */
static void ii_settle(JSContext *ctx, JSValueConst wrap, int32_t gen, const uint8_t *bytes, size_t blen)
{
    ImageHeader hdr = image_header_read(bytes, blen);
    JSValue st = ii_record_get(ctx, wrap);

    DCHECK(JS_IsObject(st), "an Image Button's fetch settled on an element with no record — the trigger that "
                            "issued it created one");
    if (ii_int(ctx, st, "gen") == gen) {
        ii_set_image(ctx, st, hdr.have_dims ? IMAGE_INPUT_AVAILABLE : IMAGE_INPUT_UNAVAILABLE);
        if (hdr.have_dims) {
            JS_SetPropertyStr(ctx, st, "naturalWidth", JS_NewFloat64(ctx, hdr.width));
            JS_SetPropertyStr(ctx, st, "naturalHeight", JS_NewFloat64(ctx, hdr.height));
            JS_SetPropertyStr(ctx, st, "bytes", JS_NewArrayBufferCopy(ctx, bytes, blen));
        }
    }
    JS_FreeValue(ctx, st);
    event_target_fire(ctx, wrap, event_new(ctx, hdr.have_dims ? "load" : "error", false, false), JS_UNDEFINED,
                      TASK_SOURCE_USER_INTERACTION);
}

/* processResponseEndOfBody. A network error is the JSON `null`, whose body is the empty byte sequence, so it
   takes the error arm with every reply no header reader measures. func_data = [wrapper, gen]. */
static JSValue ii_deliver(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic,
                          JSValueConst *func_data)
{
    JSValue body;
    const uint8_t *bytes;
    size_t blen = 0;
    int32_t gen = 0;

    (void)this_val; (void)magic;
    DCHECK(argc >= 1 && (JS_IsObject(argv[0]) || JS_IsNull(argv[0])),
           "an Image Button's reply arrived as something other than the host's reply record or null");
    JS_ToInt32(ctx, &gen, func_data[1]);
    body = fetch_reply_body(ctx, argv[0]);
    bytes = fetch_body_bytes(ctx, body, &blen);
    ii_settle(ctx, func_data[0], gen, bytes, blen);
    JS_FreeValue(ctx, body);
    return JS_UNDEFINED;
}

/* "Fetch request", run as a microtask the trigger queued. The request parks on the running flow's register,
   and a parsed document's own Image Buttons are triggered at the pre-boot baseline, which has no flow; every
   trigger takes this one route. The URL and the generation are fixed at the trigger, so only the moment the
   request leaves moves, and Fetch §4.1 "Main fetch" runs its network half in parallel anyway.
   The request: destination "image", credentials mode "include", and Fetch §2.2.5's defaults for the rest
   (mode "no-cors", referrer "client", an empty referrer policy main fetch fills from the policy container).
   Its initiator type "input" and use-URL-credentials flag have no reader in this engine.
   func_data = [wrapper, urlString, gen]. */
static JSValue ii_fetch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic,
                        JSValueConst *func_data)
{
    CspRequestMetadata meta = csp_request_metadata_unstated();
    const char *u;
    char *abs, *up;

    (void)this_val; (void)argc; (void)argv; (void)magic;
    /* Recorded before the policy checks: a request a policy refuses is still one the page makes. */
    endpoint_record(ctx, "GET", func_data[1], NULL, 0, NULL, engine_prov_of_running_path(),
                    engine_driven_of_running_path(), EPD_IMAGE_INPUT);
    u = JS_ToCString(ctx, func_data[1]);
    CHECK(u != NULL, "§4.10.5.1.19: OOM reading an Image Button's request URL");
    abs = strdup(u);
    JS_FreeCString(ctx, u);
    CHECK(abs != NULL, "§4.10.5.1.19: OOM copying an Image Button's request URL");
    /* Main fetch steps 4, 6 and 7 in the standard's order, step 7 judging the address step 6 left. */
    fetch_main_report_csp(ctx, abs, "image", meta);
    up = fetch_main_upgrade(ctx, abs, "image", NULL);
    if (up) { free(abs); abs = up; }
    if (fetch_main_blocked(ctx, abs, "image", meta, FETCH_MODE_NO_CORS)) {
        int32_t gen = 0;

        JS_ToInt32(ctx, &gen, func_data[2]);
        ii_settle(ctx, func_data[0], gen, NULL, 0);   /* a network error: "fails without a response" */
    } else {
        JSValueConst data[2] = { func_data[0], func_data[2] };
        JSValue deliver = JS_NewCFunctionData(ctx, ii_deliver, 1, 0, 2, data);
        FetchRequest req = {0};

        CHECK(!JS_IsException(deliver), "§4.10.5.1.19: OOM allocating an Image Button's completion steps");
        req.method = "GET";
        req.url = abs;
        req.destination = "image";
        req.credentials = FETCH_CREDENTIALS_INCLUDE;
        req.mode = FETCH_MODE_NO_CORS;
        req.referrer = "client";
        req.referrer_policy = "";
        req.address = FETCH_ADDRESS_CONCRETE;
        req.metadata = meta;
        engine_pending_resource_url(ctx, deliver, &req);
        JS_FreeValue(ctx, deliver);
    }
    free(abs);
    return JS_UNDEFINED;
}

/* Whether a request is made for an Image Button of `doc`. §4.10.5.1.19 states no document condition; Chrome
   153 makes none for one in a `DOMParser` document or a template's contents, and those are the documents that
   are not this realm's fully active one (the condition §4.8.4.3.5 "Updating the image data" states for img). */
static bool ii_document_fetches(JSContext *ctx, const lxb_dom_document_t *doc)
{
    return doc != NULL && document_associated(ctx) == doc && document_fully_active(ctx);
}

/* The steps §4.10.5.1.19 runs at each trigger event, "unless … the src attribute's value is the empty string":
   encoding-parse the URL ("If url is failure, then return"), then fetch. An src that names no URL names no
   image, so the element's image is cleared first; a fetch makes it FETCHING. */
static void ii_run(JSContext *ctx, JSValueConst wrap, lxb_dom_element_t *el, const lxb_dom_document_t *doc,
                   JSValueConst st)
{
    int32_t gen = ii_int(ctx, st, "gen") + 1;
    JSValue taint;
    const lxb_char_t *v;
    size_t n = 0;
    char *url;

    JS_SetPropertyStr(ctx, st, "gen", JS_NewInt32(ctx, gen));
    ii_set_image(ctx, st, IMAGE_INPUT_NO_IMAGE);
    if (!ii_document_fetches(ctx, doc)) return;
    /* An src composed out of unknown input: neither its emptiness nor its URL is decidable, so its shape is
       the endpoint and nothing is fetched. Only a script writes a taint, so a flow is running here. */
    taint = dom_cow_attr_taint(el, "src");
    if (!JS_IsUndefined(taint)) {
        endpoint_record(ctx, "GET", taint, NULL, 0, NULL, engine_prov_of_running_path(),
                        engine_driven_of_running_path(), EPD_IMAGE_INPUT);
        return;
    }
    v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"src", 3, &n);
    if (v == NULL || n == 0) return;
    url = ii_url(doc, (const char *)v, n);
    if (url == NULL) return;
    ii_set_image(ctx, st, IMAGE_INPUT_FETCHING);
    {
        JSValueConst data[3];
        JSValue uv = JS_NewString(ctx, url), gv = JS_NewInt32(ctx, gen), fn;

        CHECK(!JS_IsException(uv), "§4.10.5.1.19: OOM keeping an Image Button's request URL");
        data[0] = wrap;
        data[1] = uv;
        data[2] = gv;
        fn = JS_NewCFunctionData(ctx, ii_fetch, 0, 0, 3, data);
        CHECK(!JS_IsException(fn), "§4.10.5.1.19: OOM allocating an Image Button's fetch");
        JS_EnqueueCallJob(ctx, fn, 0, NULL);
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, uv);
    }
    free(url);
}

void image_input_attr_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el);
    bool is_type;
    JSValue wrap, st, src;

    if (ns && *ns) return;
    DCHECK(local != NULL, "§4.10.5.1.19 was asked about an attribute change with no attribute name");
    is_type = strcmp(local, "type") == 0;
    if (!is_type && strcmp(local, "src") != 0) return;
    if (html_form_input_state(n) == INPUT_STATE_NONE) return;
    wrap = node_wrap(ctx, n);
    if (!ii_in_state(n)) {
        /* Leaving the state: the second trigger event asks about the src "since the last time the type
           attribute was in the Image Button state", so the record keeps the value it saw. */
        st = ii_record_get(ctx, wrap);
        if (is_type && JS_IsObject(st)) JS_SetPropertyStr(ctx, st, "inState", JS_FALSE);
        JS_FreeValue(ctx, st);
        JS_FreeValue(ctx, wrap);
        return;
    }
    st = ii_record_get(ctx, wrap);
    src = ii_src(ctx, el);
    if (is_type) {
        /* "the input element's type attribute is first set to the Image Button state …, and the src attribute
           is present", and "changed back to the Image Button state, and the src attribute is present, and its
           value has changed since the last time the type attribute was in the Image Button state". A type
           write that keeps the state is neither. */
        bool first = !JS_IsObject(st), run;

        if (!first && JS_ToBool(ctx, JS_GetPropertyStr(ctx, st, "inState"))) {
            JS_FreeValue(ctx, src);
            JS_FreeValue(ctx, st);
            JS_FreeValue(ctx, wrap);
            return;
        }
        if (first) st = ii_record(ctx, wrap);
        run = !JS_IsNull(src);
        if (!first && run) {
            JSValue seen = JS_GetPropertyStr(ctx, st, "srcSeen");

            run = !JS_IsStrictEqual(ctx, seen, src);
            JS_FreeValue(ctx, seen);
        }
        JS_SetPropertyStr(ctx, st, "inState", JS_TRUE);
        JS_SetPropertyStr(ctx, st, "srcSeen", JS_DupValue(ctx, src));
        if (run) ii_run(ctx, wrap, el, n->owner_document, st);
    } else {
        /* "the input element's type attribute is in the Image Button state, and the src attribute is set or
           changed". A removal is neither. An element in the state with no record is one whose entry no
           trigger saw, a clone's. */
        if (!JS_IsObject(st)) {
            st = ii_record(ctx, wrap);
            JS_SetPropertyStr(ctx, st, "inState", JS_TRUE);
        }
        JS_SetPropertyStr(ctx, st, "srcSeen", JS_DupValue(ctx, src));
        if (!JS_IsNull(src)) ii_run(ctx, wrap, el, n->owner_document, st);
    }
    JS_FreeValue(ctx, src);
    JS_FreeValue(ctx, st);
    JS_FreeValue(ctx, wrap);
}

void image_input_parsed(JSContext *ctx, lxb_dom_node_t *root, lxb_dom_node_t *target)
{
    const lxb_dom_document_t *doc;
    lxb_dom_node_t *n;

    DCHECK(root != NULL && target != NULL, "§4.10.5.1.19's parsed-tree walk was given no tree or no target");
    DCHECK(g_ready, "a parsed tree reached §4.10.5.1.19 before image_input_declare ran");
    doc = target->type == LXB_DOM_NODE_TYPE_DOCUMENT ? lxb_dom_interface_document(target) : target->owner_document;
    for (n = root; n; n = shadow_root_next_in_shadow_including(ctx, n, root)) {
        lxb_dom_element_t *el;
        JSValue wrap, st, src;

        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT || !ii_in_state(n)) continue;
        el = lxb_dom_interface_element(n);
        wrap = node_wrap(ctx, n);
        st = ii_record(ctx, wrap);
        src = ii_src(ctx, el);
        JS_SetPropertyStr(ctx, st, "inState", JS_TRUE);
        JS_SetPropertyStr(ctx, st, "srcSeen", JS_DupValue(ctx, src));
        if (!JS_IsNull(src)) ii_run(ctx, wrap, el, doc, st);
        JS_FreeValue(ctx, src);
        JS_FreeValue(ctx, st);
        JS_FreeValue(ctx, wrap);
    }
}

ImageInputImage image_input_image(JSContext *ctx, lxb_dom_element_t *el, double *w, double *h)
{
    JSValue wrap, st;
    ImageInputImage image = IMAGE_INPUT_NO_IMAGE;

    DCHECK(el != NULL && w != NULL && h != NULL, "an Image Button's image was asked with no element or no "
                                                 "place for its natural dimensions");
    DCHECK(ii_in_state(lxb_dom_interface_node(el)),
           "an Image Button's image was asked of an element whose type is not in the Image Button state");
    wrap = element_wrap(ctx, el);
    DCHECK(!JS_IsNull(wrap), "an Image Button has no wrapper to hold its record");
    st = ii_record_get(ctx, wrap);
    if (JS_IsObject(st)) {
        image = (ImageInputImage)ii_int(ctx, st, "image");
        DCHECK(image >= IMAGE_INPUT_NO_IMAGE && image <= IMAGE_INPUT_UNAVAILABLE,
               "an Image Button's record holds an image state outside ImageInputImage");
        if (image == IMAGE_INPUT_AVAILABLE) {
            *w = ii_num(ctx, st, "naturalWidth");
            *h = ii_num(ctx, st, "naturalHeight");
        }
    }
    JS_FreeValue(ctx, st);
    JS_FreeValue(ctx, wrap);
    return image;
}

/* The bytes are a server's, so a refusal by the decoder is an answer (NULL), never an assert. */
uint8_t *image_input_decoded_rgba(JSContext *ctx, lxb_dom_element_t *el, uint32_t *w, uint32_t *h)
{
    JSValue wrap, st, buf = JS_UNDEFINED;
    const uint8_t *bytes;
    uint8_t *out = NULL;
    size_t blen = 0, n = 0;
    PngDecode d;
    PngHeader hdr;

    *w = *h = 0;
    wrap = element_wrap(ctx, el);
    st = ii_record_get(ctx, wrap);
    if (!JS_IsObject(st) || ii_int(ctx, st, "image") != IMAGE_INPUT_AVAILABLE) goto done;
    buf = JS_GetPropertyStr(ctx, st, "bytes");
    bytes = JS_GetArrayBuffer(ctx, &blen, buf);
    if (bytes == NULL || blen == 0) goto done;
    png_decode_init(&d, bytes, blen, 0);
    if (png_decode_run(&d) == PNG_DONE && png_decode_header(&d, &hdr) && png_decode_rgba(&d, &n) != NULL) {
        DCHECKF((size_t)hdr.width * (size_t)hdr.height * 4 == n,
                "the PNG decoder answered PNG_DONE for a %ux%u image and handed over %zu bytes", hdr.width,
                hdr.height, n);
        out = png_decode_take_rgba(&d, &n);
        *w = hdr.width;
        *h = hdr.height;
    }
    png_decode_free(&d);
done:
    JS_FreeValue(ctx, buf);
    JS_FreeValue(ctx, st);
    JS_FreeValue(ctx, wrap);
    return out;
}

void image_input_declare(JSContext *ctx)
{
    DCHECK(!g_ready, "image_input_declare ran twice; the record key is declared once per agent");
    g_key = JS_NewSymbol(ctx, "imageInput", false);
    CHECK(!JS_IsException(g_key), "§4.10.5.1.19: the record key allocation failed");
    g_atom = JS_ValueToAtom(ctx, g_key);
    CHECK(g_atom != JS_ATOM_NULL, "§4.10.5.1.19: the record key could not be interned");
    g_ready = 1;
}

void image_input_free(JSRuntime *rt)
{
    if (!g_ready) return;
    g_ready = 0;
    JS_FreeAtomRT(rt, g_atom);
    g_atom = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_key);
    g_key = JS_UNDEFINED;
}
