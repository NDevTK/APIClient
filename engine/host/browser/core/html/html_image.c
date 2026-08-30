/* HTML §4.8.3 "The img element" and §4.8.4.3 "Processing model" — see html_image.h for what each half of this
 * component exists for and for what is honestly absent. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "solver/endpoint.h"      /* every request host-edge funnels one endpoint into the @H surface */
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/url/url.h"
#include "core/dom/node.h"
#include "core/dom/element.h"
#include "core/dom/element_view.h"   /* §4.8.3's "being rendered", which is §6's "has an associated box" */
#include "core/dom/document.h"
#include "core/layout/replaced_element.h" /* HTML §15.4.2's rules, which decide whether §10 applies at all */
#include "core/layout/used_value.h"  /* §4.8.3's "rendered width and height" — CSS 2.1 §10's used values */
#include "core/dom/shadow_root.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/fetch/fetch.h"
#include "core/fetch/port_blocking.h"
#include "core/frame/policy_container.h"
#include "core/html/html_image.h"
#include "core/html/image_source_set.h"   /* §4.8.4.3.7-.12: what source this element selects, and from what */

/* §4.8.4.3 "Processing model": "An image request's state is one of the following" — and the two composite
   answers the spec gives names to are derived from it rather than stored, because a stored copy of a derived
   fact is a second thing to keep in step. */
/* THE FOUR STATES ARE `HtmlImageState` IN html_image.h — one enumeration, because HTML §15.4.2 "Images"
   decides an `img`'s whole rendering classification on them (core/layout/replaced_element.h) and a second
   private copy here would be two lists free to disagree about which integer means broken. */

/* "When an image request's state is either partially available or completely available, the image request is
   said to be available." */
static bool img_state_available(int state)
{
    return state == HTML_IMAGE_PARTIALLY_AVAILABLE || state == HTML_IMAGE_COMPLETELY_AVAILABLE;
}

/* The private Symbol §4.8.4.3's per-element state hangs off, and the atom for the own-slot read — the store
   core/html/media_element.c keeps §4.8.11's state in, for the same two reasons: it is per ELEMENT and it must
   be per FLOW, and an ordinary property on the element's own wrapper is both. */
static JSValue g_state_key = JS_UNDEFINED;
static JSAtom  g_atom_state = JS_ATOM_NULL;
/* Declared once per AGENT; installed into every realm. */
static int g_id_factory = -1;   /* §4.8.3's `Image(width, height)` argument declaration */
static int g_task_stepid = -1;  /* the DOM manipulation task that fires `load` or `error` at the element */
/* §4.8.3's `width` and `height` SETTERS, indexed by the same magic their getters carry — 0 is the horizontal
   one — so the pair cannot be installed against each other's declaration. */
static int g_id_set_dimension[2] = { -1, -1 };
static int g_ready;

/* §4.8.3's members this build does not have, asserted absent per realm rather than left as a silence the
   auditor and the next reader each have to re-derive. */
static const char *const IMG_ABSENT[] = { "decode", "fetchPriority", "controls" };

/* ---- the element, and its image requests ------------------------------------------------------------------ */

/* WHICH NODES ARE `img` ELEMENTS — the INTERNED TAG ID and the namespace, which is what
   core/html/media_element.c's media_is_node and core/html/html_script.c's script_is ask, for both of their
   reasons: an SVG `<img>` is not §4.8.3's element and wears none of its members, and the question is asked of
   every node of a parsed document. */
static bool img_is_node(const lxb_dom_node_t *n)
{
    return n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT && lxb_html_tree_node_is(n, LXB_TAG_IMG);
}

/* The same question asked of a VALUE — the brand every member of this file performs, asked of the NODE because
   that is what the spec says an img element is. */
static bool img_is(JSValueConst v)
{
    return img_is_node(lxb_dom_interface_node(element_of_value(v)));
}

/* THE STATE RECORD, created where a flow first REACHES the element — core/html/media_element.c's rule for a
   record a flow may write, and here it is also the record's only creation site, so there is no write site left
   to miss. Every field is an ordinary property, so every write is captured by the running flow's COW delta and
   two arms that each set a `src` hold their own image requests.
   THE INITIAL VALUES ARE §4.8.4.3's OWN: "The current request is initially set to a new image request. The
   pending request is initially set to null"; "An image request's current URL is initially the empty string";
   "An image request's state is initially unavailable"; "Each img element has a last selected source, which
   must initially be null."
   `gen` IS NOT THE SPEC'S AND IS NOT AN INVENTION EITHER: §4.8.4.3.5 says "If another instance of this
   algorithm for this img element was started after this instance (even if it aborted and is no longer
   running), then return", which is a question one instance asks about another, and a counter on the element is
   what makes "started after" answerable. It is what stops `img.src = a; img.src = b` from issuing two
   requests, and what tells a reply belonging to an aborted image request from the live one. */
static JSValue img_state(JSContext *ctx, JSValueConst el)
{
    JSValue st;

    DCHECK(g_ready, "an img element's state was reached before §4.8.3 was declared");
    if (JS_GetOwnSlot(ctx, &st, el, g_atom_state) > 0) return st;

    st = idl_slots_new(ctx);
    CHECK(!JS_IsException(st), "§4.8.4.3: OOM building an img element's image requests");
    JS_SetPropertyStr(ctx, st, "state", JS_NewInt32(ctx, HTML_IMAGE_UNAVAILABLE));
    JS_SetPropertyStr(ctx, st, "currentURL", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, st, "lastSelected", JS_NULL);
    /* "The pending request is initially set to null" — JS_NULL is that statement and not a hole: an image
       request that exists is named by its current URL, so a null here is the absence the spec defines and
       `complete`'s steps read directly. */
    JS_SetPropertyStr(ctx, st, "pendingURL", JS_NULL);
    JS_SetPropertyStr(ctx, st, "gen", JS_NewInt32(ctx, 0));
    JS_DefinePropertyValue(ctx, (JSValue)el, g_atom_state, JS_DupValue(ctx, st),
                           JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    return st;
}

static int32_t st_int(JSContext *ctx, JSValueConst st, const char *name)
{
    JSValue v = JS_GetPropertyStr(ctx, st, name);
    int32_t n = 0;

    DCHECK(JS_IsNumber(v), "§4.8.4.3's image request answered a numeric field with something that is not a "
                           "number — every field is written by this file and by nothing else");
    JS_ToInt32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

static void st_set_int(JSContext *ctx, JSValueConst st, const char *name, int32_t v)
{
    JS_SetPropertyStr(ctx, (JSValue)st, name, JS_NewInt32(ctx, v));
}

static void st_set_str(JSContext *ctx, JSValueConst st, const char *name, const char *v)
{
    JSValue s = JS_NewString(ctx, v);

    CHECK(!JS_IsException(s), "§4.8.4.3: OOM writing an image request's URL");
    JS_SetPropertyStr(ctx, (JSValue)st, name, s);
}

/* See html_image.h: the current request's state and whether it has a URL, WITHOUT minting the record. The
   absence of the record IS §4.8.4.3's initial pair, so the two answers below are the standard's own words
   rather than a default this reader chose — which is the distinction CLAUDE.md draws between a positive
   statement and a hole a `||` filled. */

/* An attribute's value, or NULL — AND NULL MEANS EITHER ABSENT OR EMPTY, which is Lexbor's answer and not this
   function's choice: `lxb_dom_element_get_attribute` returns NULL for an attribute whose VALUE is absent, so
   `<img src="">` and `<img>` are indistinguishable through it. That merge is exactly right for the one
   question §4.8.4.3.5 asks with a value — "it has a src attribute specified whose value is not the empty
   string", where both answers are "no candidate" — and exactly wrong for the two that ask about PRESENCE
   ("the element has a src attribute", and `complete`'s "the src attribute's value is the empty string"),
   which is why those ask `lxb_dom_element_has_attribute` instead. Reading presence off this is the mistake
   core/html/html_script.c records making, where it ran `<script src="">`'s child text as a program. */
static const char *img_attr(lxb_dom_element_t *el, const char *name, size_t *out_n)
{
    size_t n = 0;
    const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)name, strlen(name), &n);

    if (out_n) *out_n = v ? n : 0;
    return (const char *)v;
}

/* §4.8.3's `complete` GETTER STEPS, over a record that may be ABSENT — the ONE implementation, reached from
   the member below and from the exported reader above it. It is factored out because HTML §15.4.2 "Images"'
   second rule asks the same question in different words: "the user agent has reason to believe the image will
   become available and be rendered in due course" is true of exactly an img whose request has NOT SETTLED, and
   that is `complete` being false. Deriving it a second time in core/layout/replaced_element.c from some proxy
   for it — whether the current URL is non-empty, say — gets a DIFFERENT answer at a different moment: §4.8.4.3.5
   writes the current URL after a microtask, so a layout that ran in the same task as `img.src = u` would see an
   image with nothing outstanding and classify a LOADING image as a broken one.
   `st` is JS_UNDEFINED when the element carries no image request record, which §4.8.4.3's initial values make
   a complete answer rather than a hole: state `unavailable`, current URL the empty string, pending request
   null. */
static bool img_is_complete(JSContext *ctx, lxb_dom_element_t *el, JSValueConst st)
{
    bool has_src = lxb_dom_element_has_attribute(el, (const lxb_char_t *)"src", 3);
    bool has_srcset = lxb_dom_element_has_attribute(el, (const lxb_char_t *)"srcset", 6);
    size_t sn = 0;
    /* "the src attribute's value is the empty string" — PRESENT with no value, which is one question and not
       the two `img_attr`'s NULL merges (see its note). */
    bool src_empty = has_src && img_attr(el, "src", &sn) == NULL;
    int32_t state = HTML_IMAGE_UNAVAILABLE;
    bool pending_null = true;

    if (!JS_IsUndefined(st)) {
        JSValue p = JS_GetPropertyStr(ctx, st, "pendingURL");

        state = st_int(ctx, st, "state");
        pending_null = JS_IsNull(p);
        JS_FreeValue(ctx, p);
    }
    if (!has_src && !has_srcset) return true;
    if (!has_srcset && src_empty) return true;
    if (state == HTML_IMAGE_COMPLETELY_AVAILABLE && pending_null) return true;
    if (state == HTML_IMAGE_BROKEN && pending_null) return true;
    return false;
}

/* See html_image.h: the current request's state and §4.8.3's `complete`, WITHOUT minting the record. */
HtmlImageState html_image_current_request_state(JSContext *ctx, lxb_dom_element_t *el, bool *complete)
{
    JSValue wrapper, st = JS_UNDEFINED;
    int32_t state = HTML_IMAGE_UNAVAILABLE;

    DCHECK(g_ready, "an img element's image request state was read before §4.8.3 was declared");
    DCHECK(el != NULL && complete != NULL,
           "§4.8.4.3's image request state was asked for with no element, or with nowhere to report whether "
           "the request has SETTLED — both facts are required because HTML §15.4.2's second rule turns on the "
           "second one and a caller holding only the state cannot tell an outstanding fetch from an `<img>` "
           "with no `src` at all");
    DCHECK(img_is_node(lxb_dom_interface_node(el)),
           "§4.8.4.3's image request state was asked about an element that is not an HTML `img` — an image "
           "request belongs to §4.8.3's element, and an SVG `img` wears none of its members");
    wrapper = element_wrap(ctx, el);
    DCHECK(!JS_IsNull(wrapper),
           "an `img` element has no wrapper to hold its image requests — §4.8.4.3's state is an own property "
           "of the element's own JS object, which is what makes it per-flow, so an element without one has "
           "nowhere for the record to live");
    if (JS_GetOwnSlot(ctx, &st, wrapper, g_atom_state) > 0)
        state = st_int(ctx, st, "state");
    else
        st = JS_UNDEFINED;                                 /* "an image request's state is initially unavailable" */
    *complete = img_is_complete(ctx, el, st);
    JS_FreeValue(ctx, st);
    JS_FreeValue(ctx, wrapper);
    DCHECK(state >= HTML_IMAGE_UNAVAILABLE && state <= HTML_IMAGE_BROKEN,
           "§4.8.4.3's image request holds a state that is not one of the four the standard lists — every "
           "write to it is one of this file's four, so a fifth value is a record assembled past them");
    return (HtmlImageState)state;
}

/* §4.8.4.3 "Processing model": "An img element is said to use srcset or picture if it has a srcset attribute
   specified or if it has a parent that is a picture element." */
static bool img_uses_srcset_or_picture(lxb_dom_element_t *el)
{
    lxb_dom_node_t *p = lxb_dom_interface_node(el)->parent;

    if (lxb_dom_element_has_attribute(el, (const lxb_char_t *)"srcset", 6)) return true;
    return p != NULL && p->type == LXB_DOM_NODE_TYPE_ELEMENT && lxb_html_tree_node_is(p, LXB_TAG_PICTURE);
}

/* HTML §7.3.3 "Fully active documents", asked of THE ELEMENT'S NODE DOCUMENT — which is not the question
   document_fully_active answers. §7.3.3: "A Document d is said to be FULLY ACTIVE when d is the ACTIVE
   DOCUMENT of a navigable navigable, and either navigable is a top-level traversable or navigable's container
   document is fully active." A realm holds several documents — a `DOMParser` parse, a `createHTMLDocument`, an
   XHR's responseXML — and only its ACTIVE one is any navigable's, so the element's document is fully active
   exactly when it IS this realm's active document and that document is. Asking the realm alone answers TRUE
   for a `DOMParser` document's elements, and §4.8.4.3.5 would then fetch every address in markup a page merely
   PARSED — requests a browser does not make, and which a `<template>`'s contents do not make either. */
static bool img_document_fully_active(JSContext *ctx, lxb_dom_element_t *el)
{
    lxb_dom_node_t *root = document_root_node(ctx);
    lxb_dom_node_t *n = lxb_dom_interface_node(el);

    DCHECK(n != NULL, "§7.3.3's fully active was asked about no element");
    if (!root || root->owner_document != n->owner_document) return false;
    return document_fully_active(ctx);
}

/* HTML §2.4.2 "Parsing URLs"'s ENCODING-PARSING AND SERIALIZING A URL, relative to the element's node
   document — the operation §4.8.4.3.5 performs on the selected source, twice. NULL is the standard's failure,
   which each of its two call sites answers differently and neither of them papers over. */
static char *img_url_absolute(JSContext *ctx, const char *src, size_t src_len)
{
    UrlRecord base, rec;
    const char *base_url = document_base_url(ctx);
    bool have_base;
    char *abs_url = NULL;

    url_record_init(&base);
    have_base = base_url && url_parse(&base, base_url, strlen(base_url), NULL);
    url_record_init(&rec);
    if (url_parse(&rec, src, src_len, have_base ? &base : NULL))
        abs_url = url_serialize(&rec, /*exclude_fragment*/ false);
    url_record_free(&rec);
    url_record_free(&base);
    return abs_url;
}

/* ---- the queued element task: §4.8.4.3.5's `load` and `error` -------------------------------------------- */

/* IT IS A TASK, WHICH IS WHAT §4.8.4.3.5 SAYS IT IS — every one of its event fires reads "queue an element
 * task on the DOM manipulation task source given the img element … to fire an event named load/error at the
 * img element". A synchronous fire inside the setter or inside the reply's delivery is two things wrong at
 * once: it is the wrong position in HTML §8.1.7's event loop (a page's `img.src = u` would see its own
 * `onerror` run before the next statement), and it is unparkable — the listener list is the PAGE's, so the
 * dispatch runs the page's code and must have a flow base under it. */
#define IMG_TASK_STAGES(X) \
    X(IMGT_FIRE, "HTML §4.8.4.3.5 Updating the image data — the queued element task's fire of `load` or `error`")
enum { IMG_TASK_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const img_task_steps[] = { IMG_TASK_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr   hdr;       /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t     fphase;    /* the dispatch's own phase, held across a suspension */
    uint8_t     started;
    JSValue     ev;        /* the Event being dispatched (owned) */
    EventFireCb cb;        /* §2.9's dispatch request buffer, whose width travels with its type */
} ImgTask;

static void img_task_visit(JSContext *ctx, void *stp, JSStepVisit *v)
{
    ImgTask *s = stp;
    int k;

    /* NOTHING IS OWNED UNTIL THE FIRST STEP RUNS. The state's block is ZEROED (quickjs-step.h), and a zeroed
       JSValue is not JS_UNDEFINED — so a visit that reached a machine the driver has allocated and not yet
       entered would hand the collector a value nobody wrote. `started` is that fact, and it is the same flag
       the step's own entry arm turns on before anything that can fail. */
    if (!s->started) return;
    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, k) v->val(ctx, &s->cb[k]);
}

static int img_task_step(JSContext *ctx, void *stp, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    ImgTask *s = stp;
    JSValueConst element = step_arg(&s->hdr, 0);
    JSValueConst name = step_arg(&s->hdr, 1);
    int r;

    STEP_DISPATCH(IMG_TASK_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(IMGT_FIRE);
        if (!s->started) {
            const char *type;
            int k;

            /* EVERY OWNED FIELD IS PLACED BEFORE THE FIRST THING THAT CAN FAIL, and the state's block is
               ZEROED rather than filled with JS_UNDEFINEDs (quickjs-step.h), so a read of `ev` between the
               entry and the first assignment is a read of a value that is not one. `JS_ToCString` allocates,
               which is a moment the collector can walk this machine through `img_task_visit`. */
            s->started = 1;
            s->fphase = 0;
            s->ev = JS_UNDEFINED;
            STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
            type = JS_ToCString(ctx, name);
            CHECK(type != NULL, "§4.8.4.3.5: OOM reading a queued image task's event name");
            /* Neither of §4.8.4.3.5's two events bubbles and neither is cancelable — the standard names each
               fire as "fire an event named e at the img element" and gives no initialiser. */
            s->ev = event_new(ctx, type, /*bubbles*/ false, /*cancelable*/ false);
            JS_FreeCString(ctx, type);
            if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; return JS_STEP_ABRUPT; }
        }
        r = event_target_fire_run(ctx, &s->fphase, STEP_CB(s->cb), element, s->ev, JS_UNDEFINED, cb_result,
                                  NULL, out_cb, out_argc);
        if (r > 0) return r;   /* parked INSIDE the dispatch: a listener is the page's code */
        JS_FreeValue(ctx, s->ev);
        s->ev = JS_UNDEFINED;
        if (r < 0) return JS_STEP_ABRUPT;
        return JS_STEP_DONE;
}

static const JSTrampStepDef img_task_def = {
    sizeof(ImgTask), img_task_step, NULL, 0,
    .visit = img_task_visit,
    .algorithm = "HTML §4.8.4.3.5 Updating the image data — a queued element task on the DOM manipulation "
                 "task source",
    .steps = img_task_steps
};

/* "Queue an element task on the DOM manipulation task source given the img element and the following steps:
   … fire an event named `name` at the img element." */
static void img_queue_fire(JSContext *ctx, JSValueConst el, const char *name)
{
    JSValueConst argv[2];
    JSValue fn, nm;

    DCHECK(g_task_stepid >= 0, "an img element task was queued before §4.8.3 registered its machine");
    /* THE CALLEE IS MINTED IN THE ENQUEUING REALM: a C function runs in the realm that DEFINED it (§3.7), and
       this one fires an event at an element of THIS document. */
    fn = JS_NewCFunction2(ctx, NULL, "imageElementTask", 2, JS_CFUNC_step, g_task_stepid);
    CHECK(!JS_IsException(fn), "§4.8.4.3.5: the queued image task's callee could not be allocated");
    nm = JS_NewString(ctx, name);
    CHECK(!JS_IsException(nm), "§4.8.4.3.5: OOM allocating a queued image task's event name");
    argv[0] = el;
    argv[1] = nm;
    JS_EnqueueCallTask(ctx, fn, 2, argv);   /* §4.8.4.3.5: the DOM manipulation task source */
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, nm);
}

/* ---- §4.8.4.3.5's processResponse ------------------------------------------------------------------------- */

/* THE REPLY, AND WHY THERE IS ONLY ONE ARM OF THE SWITCH.
 *
 * §4.8.4.3.5 ends "Fetch request. Return from this algorithm, and run the remaining steps as part of the
 * fetch's processResponse for the response response", and those remaining steps are a three-way jump: a
 * `multipart/x-mixed-replace` resource, a resource whose "type and data corresponds to a supported image
 * format", and "Otherwise". THIS USER AGENT SUPPORTS NO IMAGE FORMAT — it has no decoder — so every reply,
 * including a byte-perfect PNG, lands in the third arm, whose steps are stated in full: "the user agent must
 * set image request's state to broken, abort the image request for the current request and the pending
 * request, upgrade the pending request to the current request if image request is the pending request, and
 * then … queue an element task on the DOM manipulation task source given the img element to fire an event
 * named error at the img element."
 * That is not a stub standing in for the other two arms and it is not a shrug: it is the arm the standard
 * itself names for a resource this agent cannot decode, and it is the same answer a real browser gives for a
 * corrupt image. What is missing is the DECODER, and the two arms it would unlock are unreachable until one
 * exists — which is why they are asserted rather than written blind.
 *
 * IT RUNS NONE OF THE PAGE'S CODE, which is why it is a data closure and not a step machine: it writes this
 * element's own image request and ENQUEUES the task above. The page's `onerror` runs when the scheduler
 * reaches that task, under a flow, exactly as §8.1.7 says.
 * func_data = [the element's wrapper, urlString, the generation this request was issued at]. */
static JSValue img_deliver(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                           int magic, JSValueConst *func_data)
{
    JSValueConst el = func_data[0];
    JSValueConst url = func_data[1];
    JSValue st;
    int32_t issued = 0, live_gen;
    const char *u;

    (void)this_val; (void)magic;
    DCHECK(argc >= 1, "an image reply was delivered with no response — the host calls this with the reply "
                      "record or with null for a network error, and never with nothing");
    /* THE ENGINE↔HOST CONTRACT, ASSERTED AT THE EDGE IT CROSSES. Every arm below reaches the same broken
       state, so nothing here READS the record — which is exactly why its shape has to be asserted rather than
       discovered three frames later by whoever does read it (solver/reply_decode.c learns from this same reply
       at engine_provide). A network error is the JSON `null`, which is a positive answer and not a hole. */
    DCHECK(argc < 1 || JS_IsObject(argv[0]) || JS_IsNull(argv[0]),
           "an image reply arrived as something other than the host's reply record — every host builds one "
           "with fetch_reply_new or parses the trusted zone's JSON into one, and a bare string here is a host "
           "still delivering only bytes");
    JS_ToInt32(ctx, &issued, func_data[2]);
    st = img_state(ctx, el);
    live_gen = st_int(ctx, st, "gen");
    /* "To abort the image request … means to … abort any instance of the fetching algorithm for image request,
       discarding any PENDING TASKS generated by that algorithm." A later instance of §4.8.4.3.5 aborted this
       image request, and the reply the trusted zone had already been asked for cannot be un-asked — so the
       discarding happens here, where the reply arrives, and no event is fired for a request the element is no
       longer making. Without this a `img.src = a; img.src = b` pair fires two `error` events. */
    if (live_gen != issued) { JS_FreeValue(ctx, st); return JS_UNDEFINED; }

    /* "Set image request's state to broken", and "upgrade the pending request to the current request IF image
       request is the pending request" — which it never is here: the issue site asserts that this agent's
       current request is never available, so every request it makes IS the current one. The reply is still
       matched to the address it was made for, because a stale one is what the generation above discards. */
    u = JS_ToCString(ctx, url);
    CHECK(u != NULL, "§4.8.4.3.5: OOM reading the address an image reply answered");
    DCHECK(strcmp(u, "") != 0,
           "an image reply arrived for the empty address — the delivery closes over the urlString the fetch "
           "was issued with, and the empty string is what §4.8.4.3.5's NULL-SOURCE arm writes instead of "
           "fetching, so a reply carrying it is a closure built somewhere other than the issue site");
    JS_FreeCString(ctx, u);
    st_set_int(ctx, st, "state", HTML_IMAGE_BROKEN);
    /* "If maybe omit events is not set OR previousURL is not equal to urlString, then … fire an event named
       error." The maybe omit events flag is never set in this build — §4.8.4.3.2 sets it only for an element
       that ALLOWS AUTO-SIZES, which needs `loading=lazy` and `sizes=auto` and so is behind the same absent
       lazy-loading machinery — so the disjunction's first operand is true at every arrival here and the fire
       is unconditional. */
    img_queue_fire(ctx, el, "error");
    JS_FreeValue(ctx, st);
    return JS_UNDEFINED;
}

/* ---- §4.8.4.3.5 "Updating the image data" ----------------------------------------------------------------- */

/* THE REST OF THE ALGORITHM, WHICH THE STANDARD PUTS IN A MICROTASK — "Queue a microtask to perform the rest
 * of this algorithm, allowing the task that invoked this algorithm to continue", followed immediately by "If
 * another instance of this algorithm for this img element was started after this instance (even if it aborted
 * and is no longer running), then return", with the standard's own note: "Only the last instance takes effect,
 * to avoid multiple requests when, for example, the src, srcset, and crossorigin attributes are all set in
 * succession."
 * That pair is the whole reason the split exists, and dropping it is not a simplification: a bundle that writes
 * `img.crossOrigin = "anonymous"; img.src = u` would issue TWO requests for one image, and every one of them
 * would fire its own event at the element.
 * It runs none of the page's code — it resolves a URL, records an endpoint and hands the request to the host —
 * so it is a data closure. func_data = [the element's wrapper, the generation this instance was started at]. */
static JSValue img_update_rest(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                               int magic, JSValueConst *func_data)
{
    JSValueConst elv = func_data[0];
    lxb_dom_element_t *el = element_of_value(elv);
    JSValue st;
    int32_t started = 0;
    const char *selected;
    size_t selected_n = 0;
    char *abs = NULL;
    UrlRecord rec;
    ImageSourceSet ss;
    int i;

    (void)this_val; (void)argc; (void)argv; (void)magic;
    DCHECK(el != NULL, "§4.8.4.3.5's microtask continuation was queued for something that is not an element");
    JS_ToInt32(ctx, &started, func_data[1]);
    st = img_state(ctx, elv);
    /* "If another instance of this algorithm for this img element was started after this instance … return." */
    if (st_int(ctx, st, "gen") != started) { JS_FreeValue(ctx, st); return JS_UNDEFINED; }

    /* "Let selected source and selected pixel density be the URL and pixel density that results from SELECTING
       AN IMAGE SOURCE" — §4.8.4.3.7, whose first step updates the source set (§4.8.4.3.9) and whose grammar and
       choice are core/html/image_source_set.h's whole problem. IT IS ASKED FOR EVERY `img` AND NOT ONLY FOR ONE
       THAT USES SRCSET OR PICTURE, which is the standard's own shape and is why this file no longer reads the
       `src` attribute for itself: for an element with a bare `src`, §4.8.4.3.9 sets default source to that
       attribute's value, §4.8.4.3.8 step 4 appends it (there is no 1x and no width descriptor to stop it) and
       §4.8.4.3.12 gives it a 1x — the same single candidate the hand-written branch produced, out of the
       algorithm that is defined to produce it rather than out of a second one that agreed with it.
       THE PIXEL DENSITY IS NOT CARRIED, and that is a consequence rather than an omission: every consumer of
       it in §4.8.4.3.5 reads it into the image request's CURRENT PIXEL DENSITY, which is only ever read back
       by the density-corrected natural dimensions — a decoded image's, which this agent has none of. It comes
       back with the decoder, beside the dimensions it corrects. */
    image_source_set_select(ctx, el, &ss);

    /* EVERY IMAGE SOURCE IN THE SET IS AN ADDRESS THE BUNDLE SHIPPED, and only ONE of them is fetched. The
       others are what a browser at another device pixel ratio or another viewport requests from the same
       markup — which is CLAUDE.md §What-the-tool-produces exactly ("what the bundle CAN do but didn't"), so
       they reach the @H surface here even though no request is owed for them. Recorded BEFORE the policy checks
       below for the reason core/fetch/fetch.c records one before its own: a request a policy refuses is still a
       request the bundle can make. The selected one is recorded again at its own site and the surface dedupes
       on method+url, which is what makes stating it twice harmless rather than a second endpoint. */
    for (i = 0; i < ss.n; i++) {
        char *cand;
        JSValue uv;

        DCHECK(ss.items[i].url != NULL && ss.items[i].url[0] != '\0',
               "§4.8.4.3's source set holds an image source with no URL — §4.8.4.3.10 asserts a non-empty url "
               "for every candidate it appends and §4.8.4.3.8 appends a default source only when it is not the "
               "empty string, so an empty one here is one of those two disagreeing with its own assert");
        cand = img_url_absolute(ctx, ss.items[i].url, strlen(ss.items[i].url));
        if (!cand) continue;   /* HTML §2.4.2's failure: an address that is not one names no endpoint */
        uv = JS_NewString(ctx, cand);
        CHECK(!JS_IsException(uv), "§4.8.4.3.7: OOM naming an image candidate for the endpoint surface");
        endpoint_record(ctx, "GET", uv, NULL, 0, NULL);
        JS_FreeValue(ctx, uv);
        free(cand);
    }

    /* AN ATTRIBUTE THE SELECTION DEPENDS ON WAS COMPOSED OUT OF UNKNOWN EXTERNAL INPUT, so which source this
       element selects is not a question any host can be asked. It is still a request the page makes: the SHAPE
       reaches the @H surface rather than disappearing, which is the same answer core/html/html_script.c gives
       an unknown `<script src>`. The image request is left at its current state — nothing was fetched, so
       nothing may claim to have been, and no event is fired for a decision that was never taken. */
    if (ss.undecided) {
        if (!JS_IsUndefined(ss.undecided_url))
            endpoint_record(ctx, "GET", ss.undecided_url, NULL, 0, NULL);
        image_source_set_release(ctx, &ss);
        JS_FreeValue(ctx, st);
        return JS_UNDEFINED;
    }

    selected = ss.selected >= 0 ? ss.items[ss.selected].url : NULL;
    selected_n = selected ? strlen(selected) : 0;

    /* "If selected source is null: Set the current request's state to broken … Queue an element task … Change
       the current request's current URL to the empty string. If all of the following are true: the element has
       a src attribute or it uses srcset or picture; and maybe omit events is not set … then fire an event
       named error at the img element. Return."
       An `<img>` with no `src` at all therefore goes broken SILENTLY, which is what a browser does — the
       error is for an element that named a source and got nowhere. */
    if (!selected) {
        bool named_a_source = lxb_dom_element_has_attribute(el, (const lxb_char_t *)"src", 3) ||
                              img_uses_srcset_or_picture(el);
        st_set_int(ctx, st, "state", HTML_IMAGE_BROKEN);
        st_set_str(ctx, st, "currentURL", "");
        if (named_a_source) img_queue_fire(ctx, elv, "error");
        image_source_set_release(ctx, &ss);
        JS_FreeValue(ctx, st);
        return JS_UNDEFINED;
    }

    /* "Let urlString be the result of encoding-parsing-and-serializing a URL given selected source, relative to
       the element's node document. If urlString is failure: … Set the current request's state to broken … fire
       an event named error at the img element. Return." */
    abs = img_url_absolute(ctx, selected, selected_n);
    if (!abs) {
        st_set_int(ctx, st, "state", HTML_IMAGE_BROKEN);
        /* "Change the current request's current URL to SELECTED SOURCE" — the string that would not parse,
           not the empty string, which is the other failure arm's answer. */
        {
            JSValue s = JS_NewStringLen(ctx, selected, selected_n);
            CHECK(!JS_IsException(s), "§4.8.4.3.5: OOM keeping an unparseable image source");
            JS_SetPropertyStr(ctx, st, "currentURL", s);
        }
        img_queue_fire(ctx, elv, "error");
        image_source_set_release(ctx, &ss);
        JS_FreeValue(ctx, st);
        return JS_UNDEFINED;
    }
    /* THE SELECTED SOURCE HAS BEEN COPIED INTO `abs` AND IS NOT READ AGAIN, so the set is released here rather
       than at each return below it — `selected` points INTO the set and every read of that pointer is above
       this line, which is what the two assignments after the release keep true. */
    image_source_set_release(ctx, &ss);
    selected = NULL;
    selected_n = 0;

    /* "If the pending request is not null and urlString is the same as the pending request's current URL, then
       return." The pending request is null throughout this build; the assert two steps down is where that is
       stated rather than assumed.
       "If urlString is the same as the current request's current URL and the current request's state is
       PARTIALLY AVAILABLE: … Return." Reaching that needs a decoded image's dimensions, so it rides with the
       decoder; the state is asserted below rather than tested here, where a test would read as a live branch.

       "Set image request to a new image request whose current URL is urlString. If the current request's state
       is unavailable or broken, then set the current request to image request. Otherwise, set the pending
       request to image request." */
    /* Read in a DEV block rather than inside the DCHECK's condition, because the read takes a reference and a
       condition that is unevaluated in release may not own one — the form core/solver/engine.c's own contract
       asserts use for exactly this. */
#if APICLIENT_DEV
    DCHECK(!img_state_available(st_int(ctx, st, "state")),
           "§4.8.4.3.5 reached an img element whose CURRENT REQUEST IS AVAILABLE — that state means decoded "
           "image dimensions, which this agent has no decoder to produce, so every request here is the current "
           "one and the pending request is always null. Build §4.8.4.3.5's pending-request half — the second "
           "request, the `upgrade the pending request to the current request` step and `complete`'s reads of "
           "it — beside the decoder that makes the state reachable");
#endif
    st_set_str(ctx, st, "currentURL", abs);

    /* THE ADDRESS AS THE @H SURFACE MUST SEE IT, recorded BEFORE the policy checks below for the reason
       core/fetch/fetch.c records one before its own: the endpoint is what the page's code COMPOSED, and a
       request a policy refuses is still a request the bundle can make. An image URL is not filtered out here
       on the grounds that images are static assets — CLAUDE.md §Attacker sources makes that a decision about
       what a RESOURCE turned out to be ("magic-byte + content-type, not URL suffix"), which is answered on the
       reply by solver/reply_decode.c and cannot be answered at the request. */
    {
        JSValue uv = JS_NewString(ctx, abs);
        CHECK(!JS_IsException(uv), "§4.8.4.3.5: OOM naming an image request for the endpoint surface");
        endpoint_record(ctx, "GET", uv, NULL, 0, NULL);
        JS_FreeValue(ctx, uv);
    }

    /* §4.8.4.3.5's LAZY BRANCH — "If the will lazy load element steps given the img return true: Set the img's
       lazy load resumption steps to the rest of this algorithm starting with the step labeled fetch the image.
       START INTERSECTION-OBSERVING A LAZY LOADING ELEMENT for the img element. Return." — reaches the fetch
       below immediately in this user agent, and that is this agent's VIEWPORT, not a step skipped.
       §2.5.7 "Lazy loading attributes"'s will lazy load element steps do return true for `<img loading=lazy>`
       (scripting is enabled for every document this engine runs, so the attribute's state is the whole
       question), and its lazy load intersection observer is what runs the resumption steps: an element is
       observed against the viewport plus a root margin, and one that is already intersecting resumes at the
       first observation. THIS AGENT PRESENTS THE WHOLE DOCUMENT AT ONCE — it has no scrolling, so there is no
       part of a document outside the viewport for an element to be below — which makes every connected lazy
       element intersecting at that first observation and the resumption immediate. That is the same kind of
       answer §Headless-is-not-valueless gives `matchMedia`'s default viewport and `getComputedStyle`'s
       UA-default layering: a modelled device rather than an absent one, and the deferral's only observable —
       WHEN the request is made — is a question about a scroll position this agent does not have.
       WHAT WOULD CHANGE THIS is a real layout with a scrolled viewport, at which point the branch is written
       here with the observer that decides it, and its argument is a rectangle rather than an attribute. */

    /* "Let request be the result of creating a potential-CORS request given urlString, `image`, and the current
       state of the element's crossorigin content attribute" … "Fetch request."
       WHAT THIS ENGINE MAY DECIDE ABOUT THAT REQUEST AND WHAT IT MAY NOT. Fetch §4.1 "Main fetch" step 7 —
       "If should request be blocked due to a bad port, should fetching request be blocked as mixed content, or
       should request be blocked by Content Security Policy returns blocked, then set response to a network
       error" — runs HERE, in the engine, because a policy is the DOCUMENT's. §4.8.4.3.5 creates the request
       with the `image` DESTINATION, and CSP §6.8.1 "Get the effective directive for request" is the switch on
       that destination which answers `img-src`, so naming the destination is the whole of what this call site
       has to get right for the right directive to govern the check. The CORS mode is
       NOT decided here: SECURITY.md puts SOP/CORS/credentials behind the trusted zone's `safeFetch`, which is
       the one chokepoint that may see a cross-origin body, so the mode crosses as the request it is and the
       zone answers it. */
    url_record_init(&rec);
    if (url_parse(&rec, abs, strlen(abs), NULL) &&
        (fetch_block_bad_port(&rec) == FETCH_PORT_BLOCKED ||
         policy_should_block_request(document_policy(ctx), &rec, /*destination*/ "image",
                                     /*redirect count*/ 0) == CSP_REQUEST_BLOCKED)) {
        /* A blocked request is a NETWORK ERROR, which is "no data could be obtained" — §4.8.4.3's own gloss on
           the broken state — so it takes the same arm the delivery does, without ever owing the host a reply. */
        url_record_free(&rec);
        st_set_int(ctx, st, "state", HTML_IMAGE_BROKEN);
        img_queue_fire(ctx, elv, "error");
        free(abs);
        JS_FreeValue(ctx, st);
        return JS_UNDEFINED;
    }
    url_record_free(&rec);

    {
        JSValueConst data[3];
        JSValue deliver, uv, gv;
        FetchRequest req;

        uv = JS_NewString(ctx, abs);
        CHECK(!JS_IsException(uv), "§4.8.4.3.5: OOM keeping an image request's address for its reply");
        gv = JS_NewInt32(ctx, started);
        data[0] = elv;
        data[1] = uv;
        data[2] = gv;
        deliver = JS_NewCFunctionData(ctx, img_deliver, 1, 0, 3, data);
        CHECK(!JS_IsException(deliver), "§4.8.4.3.5: OOM allocating an image reply's processResponse steps");
        /* §4.8.4.3.5 creates the request and never sets a method, so it is Fetch §2.2.5 "Requests"'s `GET`.
           STATED, because the reply seam is keyed on the (method, url) pair and a park that does not say is a
           park the host's join cannot list.
           §4.8.4.3.5's "If the element USES SRCSET OR PICTURE, set request's INITIATOR to `imageset`" writes a
           field core/fetch/fetch.h's request record does not carry, and it is not carried because it has no
           reader in this engine: Fetch's initiator is read by service workers and by §4.8.4.3.13 "Reacting to
           environment changes", and neither exists here. A field a producer writes and nothing reads is the
           mirror of the defect CLAUDE.md counts seven of, so it arrives with its first consumer. */
        /* …AND THE DESTINATION IS THAT SENTENCE COMING TRUE FOR THE FIELD BESIDE IT. §4.8.4.3.5's step is "Let
           request be the result of creating a potential-CORS request given urlString, `image`, and the current
           state of the element's crossorigin content attribute", and §2.5.1 "Terminology"'s create a
           potential-CORS request returns "a new request whose URL is url, destination is destination" — so
           that second operand IS Fetch §2.2.5 "Requests"' destination. Its reader is the trusted zone's CORB
           class, which asks only whether the destination is SCRIPT-LIKE; `image` is not, so this reply is data,
           and that is an answer this park states rather than a question it leaves to a default. */
        req.method = "GET";
        req.url = abs;
        req.destination = "image";
        req.headers = NULL;
        req.body = NULL;
        req.body_len = 0;
        fetch_owe(ctx, deliver, &req);
        JS_FreeValue(ctx, deliver);
        JS_FreeValue(ctx, uv);
        JS_FreeValue(ctx, gv);
    }
    free(abs);
    JS_FreeValue(ctx, st);
    return JS_UNDEFINED;
}

void html_image_update(JSContext *ctx, lxb_dom_element_t *el)
{
    JSValue wrap, st, fn, gv;
    JSValueConst data[2];
    int32_t gen;
    const char *selected;
    size_t selected_n = 0;

    if (!img_is_node(lxb_dom_interface_node(el))) return;
    DCHECK(g_ready, "an img element's data was updated before §4.8.3 was declared");

    /* STEP 1: "If the element's node document is not fully active: Continue running this algorithm IN PARALLEL.
       Wait until the element's node document is fully active. … Queue a microtask to continue this algorithm."
       The wait ends when a Document that is not fully active becomes fully active, and the documents that reach
       this line and are not — a `DOMParser` document, an `<iframe>`'s after it was removed — are documents no
       step of this engine ever makes fully active, so the wait is the whole of what happens to them. That is
       also the answer a browser gives: `new DOMParser().parseFromString(…)`'s images do not load. */
    if (!img_document_fully_active(ctx, el)) return;

    /* STEP 2: "If the user agent cannot support images, or its support for images has been disabled, then abort
       the image request … set the current request's state to unavailable … and return."
       NOT TAKEN, AND THAT IS THE DESIGN DECISION THIS FILE IS ABOUT. This agent supports images: it fetches
       them, keeps their image requests, and fires their events. What it has no DECODER for is the pixels, and
       the standard already has an arm for a resource whose data is in no supported format — the one the reply
       lands in. Taking this exit instead would issue no request and fire no event, which deletes both an
       endpoint the page named and the auto-firing sink an `onerror` is. */

    wrap = node_wrap(ctx, lxb_dom_interface_node(el));
    st = img_state(ctx, wrap);
    /* "Let previousURL be the current request's current URL." It is read only by the maybe omit events
       comparisons, and that flag is never set in this build (see img_deliver), so nothing is kept: a variable
       whose every reader is behind a condition that cannot hold is a value nobody has. */

    /* STEPS 3-5: "Let selected source be null and selected pixel density be undefined. If the element does not
       use srcset or picture and it has a src attribute specified whose value is not the empty string, then set
       selected source to the value of the element's src attribute and set selected pixel density to 1.0. Set
       the element's LAST SELECTED SOURCE to selected source." */
    selected = img_uses_srcset_or_picture(el) ? NULL : img_attr(el, "src", &selected_n);
    DCHECK(selected == NULL || selected_n > 0,
           "Lexbor answered a `src` attribute with a zero-length value — see the same assert in the "
           "continuation; emptiness has exactly one spelling in this file and it is the NULL");
    if (selected) {
        JSValue s = JS_NewStringLen(ctx, selected, selected_n);
        CHECK(!JS_IsException(s), "§4.8.4.3.5: OOM keeping an img element's last selected source");
        JS_SetPropertyStr(ctx, st, "lastSelected", s);
    } else {
        JS_SetPropertyStr(ctx, st, "lastSelected", JS_NULL);
    }

    /* STEP 6: "If selected source is not null: Let urlString be … Let key be a tuple … If THE LIST OF AVAILABLE
       IMAGES contains an entry for key: … fire an event named load … Abort the update the image data
       algorithm."
       THE LIST IS EMPTY IN THIS BUILD AND THAT IS DERIVED, NOT MISSING. §4.8.4.3.5 adds to it in exactly one
       place — "Add the image to the list of available images using the key key" in the supported-file-format
       arm of the reply — and no reply of this agent reaches that arm, so a lookup can never hit and the whole
       of step 6 is a URL parse whose result is discarded. Nothing is stubbed for it, and the day a decoder
       lands, §4.8.4.3.3 "The list of available images" is built with the arm that fills it. */

    /* "QUEUE A MICROTASK to perform the rest of this algorithm, allowing the task that invoked this algorithm
       to continue." The generation is bumped HERE, at the start of the instance, because what the continuation
       asks is whether another instance was started AFTER it. */
    gen = st_int(ctx, st, "gen") + 1;
    st_set_int(ctx, st, "gen", gen);
    gv = JS_NewInt32(ctx, gen);
    data[0] = wrap;
    data[1] = gv;
    fn = JS_NewCFunctionData(ctx, img_update_rest, 0, 0, 2, data);
    CHECK(!JS_IsException(fn), "§4.8.4.3.5: OOM allocating the microtask the rest of the algorithm runs in");
    JS_EnqueueCallJob(ctx, fn, 0, NULL);   /* a MICROTASK, which is what the standard queues */
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, gv);
    JS_FreeValue(ctx, st);
    JS_FreeValue(ctx, wrap);
}

/* ---- §4.8.4.3.2 "Reacting to DOM mutations" --------------------------------------------------------------- */

/* WHICH `img` ELEMENTS A `source` ELEMENT'S MUTATION IS RELEVANT TO, which §4.8.4.3.9's own closing note
   answers rather than §4.8.4.3.2: "Each img element independently considers its PREVIOUS SIBLING source
   elements plus the img element itself for selecting an image source … [ignoring] source elements that are
   FOLLOWING SIBLINGS of the relevant img element." So a mutated `source` is relevant to exactly the `img`
   elements that come AFTER it under the same `picture` parent — one `<picture>` may hold several, and each of
   them selects out of a different prefix of the child list.
   A `source` OUTSIDE a `picture` is §4.8.12's media-element source and is nobody's image candidate; it reaches
   this function and leaves it, which is why the parent test is here rather than at the call sites. */
static void img_source_mutated(JSContext *ctx, lxb_dom_element_t *src)
{
    lxb_dom_node_t *parent = lxb_dom_interface_node(src)->parent;
    lxb_dom_node_t *n;

    if (!parent || parent->type != LXB_DOM_NODE_TYPE_ELEMENT ||
        !lxb_html_tree_node_is(parent, LXB_TAG_PICTURE))
        return;
    for (n = lxb_dom_interface_node(src)->next; n; n = n->next)
        if (img_is_node(n)) html_image_update(ctx, lxb_dom_interface_element(n));
}

void html_image_attr_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local)
{
    const lxb_dom_node_t *n = lxb_dom_interface_node(el);

    /* A content attribute is the HTML namespace's; a `foo:src` is a different attribute entirely. */
    if (ns && *ns) return;
    DCHECK(local != NULL, "§4.8.4.3.2 was asked about an attribute change with no attribute name");
    /* "The element's parent is a picture element and A SOURCE ELEMENT THAT IS A PREVIOUS SIBLING has its
       srcset, sizes, media, type, width or height attributes set, changed, or removed." SIX names, and they are
       not the img's six: `media` and `type` are §4.8.4.3.9's own two conditions on a `source`, and `width` and
       `height` are the ones its step 9 reads for the dimension attribute source. */
    if (n && n->type == LXB_DOM_NODE_TYPE_ELEMENT && lxb_html_tree_node_is(n, LXB_TAG_SOURCE)) {
        if (strcmp(local, "srcset") && strcmp(local, "sizes") && strcmp(local, "media") &&
            strcmp(local, "type") && strcmp(local, "width") && strcmp(local, "height"))
            return;
        img_source_mutated(ctx, el);
        return;
    }
    if (!img_is_node(n)) return;
    /* "The element's src, srcset, width, or sizes attributes are set, changed, or removed", plus "The element's
       crossorigin attribute's state is changed" and "The element's referrerpolicy attribute's state is
       changed". `width` is in the list because it feeds the auto-sizes machinery §4.8.4.3.9 reads, and it
       counts as a relevant mutation whether or not this build acts on the rest of that sentence. */
    if (strcmp(local, "src") && strcmp(local, "srcset") && strcmp(local, "width") &&
        strcmp(local, "sizes") && strcmp(local, "crossorigin") && strcmp(local, "referrerpolicy"))
        return;
    html_image_update(ctx, el);
}

void html_image_inserted(JSContext *ctx, lxb_dom_element_t *el)
{
    const lxb_dom_node_t *n = lxb_dom_interface_node(el);

    /* "The img or source HTML element insertion steps … count the mutation as a relevant mutation." BOTH
       halves: an inserted `source` changes which candidates every `img` after it in the same `picture` selects
       from, and §4.8.4.3.9's walk is what reads it. */
    if (n && n->type == LXB_DOM_NODE_TYPE_ELEMENT && lxb_html_tree_node_is(n, LXB_TAG_SOURCE)) {
        img_source_mutated(ctx, el);
        return;
    }
    if (!img_is_node(n)) return;
    html_image_update(ctx, el);
}

void html_image_parsed(JSContext *ctx, lxb_dom_node_t *root)
{
    lxb_dom_node_t *n;

    DCHECK(root != NULL, "§4.8.4.3.2's parsed-tree walk was given no tree to walk");
    DCHECK(g_ready, "a parsed tree reached §4.8.4.3.2 before html_image_declare ran");
    /* SHADOW-INCLUDING, for the reason core/html/media_element.c's walk is: a `<template shadowrootmode>` has
       by now become a shadow root whose children are in the tree and are not reachable by the ordinary walk. */
    for (n = root; n; n = shadow_root_next_in_shadow_including(ctx, n, root)) {
        if (!img_is_node(n)) continue;
        /* PRESENCE, NOT VALUE — `<img src="">` IS an element that named a source, and §4.8.4.3.5's null-source
           arm is where the empty string is answered, with the `error` event a browser fires for it. An element
           with no `src` and no `srcset` AND NO `picture` PARENT has nothing to update and is skipped so a parse
           of a document full of `<img alt>` placeholders queues nothing.
           THE PARENT IS PART OF THE TEST because §4.8.4.3 makes it part of the question: an element "uses
           srcset or picture … if it has a srcset attribute specified OR IF IT HAS A PARENT THAT IS A PICTURE
           ELEMENT", so `<picture><source srcset=a><img alt=x></picture>` selects a real source out of a
           `source` sibling while carrying neither attribute itself. Testing only the two attributes skipped
           exactly the markup this component was built for. */
        if (!lxb_dom_element_has_attribute(lxb_dom_interface_element(n), (const lxb_char_t *)"src", 3) &&
            !img_uses_srcset_or_picture(lxb_dom_interface_element(n)))
            continue;
        html_image_update(ctx, lxb_dom_interface_element(n));
    }
}

/* ---- §4.8.3's members ------------------------------------------------------------------------------------- */

static JSValue img_state_of(JSContext *ctx, JSValueConst this_val, const char *member)
{
    if (!img_is(this_val))
        return JS_ThrowTypeError(ctx, "%s called on something that is not an img element", member);
    return img_state(ctx, this_val);
}

/* "The naturalWidth and naturalHeight getter steps are: If the image is not available, then return 0. Return
   the respective component of the image's density-corrected natural width and height, in CSS pixels." */
static JSValue js_img_natural(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue st = img_state_of(ctx, this_val, magic == 0 ? "naturalWidth" : "naturalHeight");
    int state;

    if (JS_IsException(st)) return st;
    state = st_int(ctx, st, "state");
    JS_FreeValue(ctx, st);
    if (!img_state_available(state)) return JS_NewInt32(ctx, 0);   /* step 1 */
    DFAIL("§4.8.3's naturalWidth/naturalHeight reached its second step — the DENSITY-CORRECTED NATURAL WIDTH "
          "AND HEIGHT are a decoded image's, and this agent has no image decoder, so no image request of it "
          "can be available. Build the decoder and the natural dimensions it produces together; a number "
          "answered here without one would be a measurement of nothing");
    return JS_NewInt32(ctx, 0);
}

/* §4.8.3's "TO DETERMINE THE DIMENSIONS of an img element image", all three steps, and the ONE algorithm both
 * `width` and `height` are stated over: "The width getter steps are to return the width of this's dimensions"
 * and the same sentence for `height`.
 *   1. "If image is BEING RENDERED, then return its RENDERED WIDTH AND HEIGHT, in CSS pixels."
 *   2. "If image is AVAILABLE and has density-corrected natural width and height, then return its
 *      density-corrected natural width and height, in CSS pixels."
 *   3. "Return a width of 0 and a height of 0."
 * BEING RENDERED IS "has any associated CSS layout boxes", which core/dom/element_view.h states is the ONE
 * predicate CSSOM VIEW §6's "associated box" and HTML's "being rendered" are two names for — so step 1 is
 * `element_view_has_box` and not a second test.
 * THE RENDERED WIDTH IS THE CONTENT EXTENT, which is why this asks `used_value_content_px` and not
 * `used_value_px`. The two differ under `box-sizing: border-box`, where css-sizing §5 makes the exposed used
 * value the BORDER box's; what an image is RENDERED at is the box its content is drawn into, which css-images-3
 * §4.5 "Sizing Objects: the object-fit property" calls the concrete object size and defines under the initial
 * `fill` as "the element's used width and height". `* { box-sizing: border-box }` is on most of the modern web,
 * so the difference is not a corner.
 * THE ANSWER CROSSES THROUGH viewport.h's ONE SEAM. A used width whose containing-block chain bottoms out in
 * the initial containing block carries that fact (core/layout/used_value.h), and `img.width < 768` is the same
 * responsive question `innerWidth < 768` is — so the number is minted as a concolic exactly as CSSOM VIEW §6's
 * extents are, through the shared conversion that also owns the rounding to an integral IDL value. */
static JSValue js_img_dimension(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el;
    JSValue st;
    bool vertical = magic != 0;
    int state;

    if (!img_is(this_val))
        return JS_ThrowTypeError(ctx, "%s called on something that is not an img element",
                                 vertical ? "height" : "width");
    el = element_of_value(this_val);
    if (element_view_has_box(lxb_dom_interface_node(el))) {                     /* step 1 */
        /* BEING RENDERED IS NOT THE SAME AS BEING REPLACED, and this is the one member where the difference
           reaches a page as a number. HTML §15.4.2 "Images"' third rule makes an `img` that represents text —
           a broken image with an `alt` — a NON-REPLACED PHRASING ELEMENT, which still generates a box and is
           still being rendered; its rendered width is then the width of its INLINE BOXES over the alt text,
           which CSS 2.1 §10.3.1 "Inline, non-replaced elements" states outright that `width` does not
           determine. Routing it into §10's used width would answer with an algorithm that does not apply. */
        if (!replaced_element_of(el).replaced)
            DFAIL("§4.8.3's determine the dimensions was asked for the RENDERED width of an `img` that HTML "
                  "§15.4.2 \"Images\"' third rule makes a NON-REPLACED phrasing element — an image that "
                  "represents the text of its `alt` attribute because its request broke. It is being rendered, "
                  "so step 1 applies and there is a real answer; that answer is the extent of the LINE BOXES "
                  "the alt text lays out into, which CSS 2.1 §9.4.2 \"Inline formatting contexts\" defines. THE "
                  "FONT IS NO LONGER THE MISSING OPERAND — core/layout/text_run.h measures a run's advance and "
                  "its break opportunities, and §10.3.5's shrink-to-fit is computed over them — what is missing "
                  "is the EXTENT of the line boxes themselves, which is a greedy fill against each line's "
                  "available width and is core/layout/line_box.c's own named crash. BUILD that fill; every "
                  "text-sized replaced element in core/layout/replaced_element.c is waiting on the same one");
        return element_view_length_long(ctx, used_value_content_px(el, vertical));
    }
    st = img_state(ctx, this_val);
    state = st_int(ctx, st, "state");
    JS_FreeValue(ctx, st);
    if (img_state_available(state))                                             /* step 2 */
        DFAIL("§4.8.3's determine the dimensions reached its SECOND step — an image request became AVAILABLE, "
              "so this element has a DENSITY-CORRECTED NATURAL WIDTH AND HEIGHT to report and there is no "
              "decoder to have produced one. It is the same absent component `naturalWidth` crashes for one "
              "member up, and the same one HTML §15.4.2's first rule crashes for in "
              "core/layout/replaced_element.c; all three are read from the decoded image and must arrive "
              "together");
    /* Step 3: "Return a width of 0 and a height of 0." Not a fallback and not a shrug — it is the algorithm's
       own final step, and it is the answer for every `img` that is not being rendered: one in a document no
       navigable presents, one whose `display` computes to `none`, one that has never been inserted. */
    return JS_NewInt32(ctx, 0);
}

/* THE OTHER HALF OF `[CEReactions, ReflectSetter] attribute unsigned long width`, which is what makes these two
 * members asymmetric: the GETTER is §4.8.3's determine the dimensions above and the SETTER is an ordinary
 * reflection, so neither can be a row in core/html/html_element.c's reflection table and neither can be a plain
 * readonly accessor. Installing only the getter would be worse than the absence it replaces — `img.width = 50`
 * is how a page sizes an image it just constructed, and a readonly accessor drops it silently in sloppy mode.
 *
 * HTML §2.6.1 "Reflecting content attributes in IDL attributes", the `unsigned long` setter steps, which have
 * no arm this member takes but the last two: it is not limited to only positive numbers, has no default value
 * and is not clamped to a range, so `minimum` is 0, `newValue` starts at 0, and "if the given value is in the
 * range minimum to 2147483647, INCLUSIVE, then set newValue to it" is the whole of the decision. That upper
 * bound is not the `unsigned long` type's — Web IDL's conversion has already run and produced a value modulo
 * 2^32 — so `img.width = -1` arrives here as 4294967295, falls OUTSIDE the range, and writes `0`. Dropping the
 * range test would write "4294967295" instead, which is a different attribute value and a different rendered
 * box. */
static JSValue js_img_dimension_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    char buf[16];
    uint32_t v = 0;
    int wrote;

    if (!img_is(this_val))
        return JS_ThrowTypeError(ctx, "HTMLImageElement.%s was set on something that is not an img element",
                                 magic == 0 ? "width" : "height");
    DCHECK(JS_IsNumber(val),
           "§4.8.3's `width`/`height` setter was handed something that is not a number — it is declared "
           "IDL_UNSIGNED_LONG, so Web IDL's `unsigned long` conversion produced one before this body ran");
    JS_ToUint32(ctx, &v, val);
    /* Step: "If the given value is in the range minimum to 2147483647, inclusive, then set newValue to it" —
       otherwise newValue stays at `minimum`, which is 0 for this member. */
    if (v > 2147483647u) v = 0;
    wrote = snprintf(buf, sizeof buf, "%u", (unsigned)v);
    /* "the shortest possible string representing the number as a VALID NON-NEGATIVE INTEGER" — decimal digits
       with no sign, no leading zero and no exponent, which `%u` is. The widest value that reaches here is
       2147483647, ten characters, so a truncation is a value that did not come through the conversion and the
       attribute written would be a DIFFERENT NUMBER rather than an absent one. */
    DCHECK(wrote > 0 && (size_t)wrote < sizeof buf,
           "§2.6.1's `unsigned long` setter could not serialise its value — the range test above bounds it at "
           "2147483647, so a value too wide for the buffer is one that did not come through it");
    element_attr_set(ctx, this_val, magic == 0 ? "width" : "height", buf);
    return JS_UNDEFINED;
}

/* "The complete getter steps are: If any of the following are true: both the src attribute and the srcset
   attribute are omitted; the srcset attribute is omitted and the src attribute's value is the empty string;
   the img element's current request's state is completely available and its pending request is null; or the
   img element's current request's state is broken and its pending request is null, then return true. Return
   false." */
static JSValue js_img_complete(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue st = img_state_of(ctx, this_val, "complete");
    bool complete;

    (void)magic;
    if (JS_IsException(st)) return st;
    /* The four conditions are `img_is_complete` above and are NOT written twice: HTML §15.4.2's second rule
       asks the same question of the same element through core/layout/replaced_element.c, and two derivations
       of one getter's steps are two answers free to disagree about a loading image. */
    complete = img_is_complete(ctx, element_of_value(this_val), st);
    JS_FreeValue(ctx, st);
    return complete ? JS_TRUE : JS_FALSE;
}

/* "The currentSrc IDL attribute must return the img element's current request's current URL." */
static JSValue js_img_current_src(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue st = img_state_of(ctx, this_val, "currentSrc"), out;

    (void)magic;
    if (JS_IsException(st)) return st;
    out = JS_GetPropertyStr(ctx, st, "currentURL");
    DCHECK(JS_IsString(out), "§4.8.3's currentSrc read an image request with no current URL — the record is "
                             "created with the empty string §4.8.4.3 gives it and written by this file only");
    JS_FreeValue(ctx, st);
    return out;
}

/* ---- §4.8.3's LEGACY FACTORY FUNCTION --------------------------------------------------------------------- */

/* "A legacy factory function is provided for creating HTMLImageElement objects (in addition to the factory
 * methods from DOM such as createElement()): Image(width, height). When invoked, the legacy factory function
 * must perform the following steps:
 *   1. Let document be the CURRENT GLOBAL OBJECT's associated Document.
 *   2. Let img be the result of CREATING AN ELEMENT given document, "img", and the HTML namespace.
 *   3. If width is given, then SET AN ATTRIBUTE VALUE for img using "width" and width.
 *   4. If height is given, then set an attribute value for img using "height" and height.
 *   5. Return img."
 *
 * IT IS NOT `document.createElement("img")`, and the difference is observable twice. The document is the
 * CURRENT global object's — the realm whose `Image` was called, which for a C function is the realm that
 * DEFINED it (§3.7), which is why this function object is minted per realm rather than held in a static. And
 * the width and height are set only WHEN GIVEN: `new Image()` produces an element with no `width` attribute at
 * all, where a defaulted 0 would put `width="0"` in the serialized markup.
 *
 * DOM §4.9 "Interface Element"'s CREATE AN ELEMENT rather than §4.5 "Interface Document"'s createElement is
 * also what makes step 2 run no page code — there is no custom element definition for `img` to look up, and
 * the internal creation is the half that has none. */
static JSValue js_image_factory(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue img;

    (void)magic;
    /* Web IDL §3.7.2 "Legacy factory functions" step 1 of the function's steps: "If NewTarget is undefined,
       then throw a TypeError." JS_CFUNC_step_ctor delivers NEW_TARGET in the receiver slot and undefined for a
       plain call, which is how `Image()` is told apart from `new Image()`. */
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "Failed to construct 'Image': please use the 'new' operator");
    img = document_create_element_internal(ctx, "img", 3);            /* steps 1-2 */
    if (JS_IsException(img)) return img;
    DCHECK(img_is_node(lxb_dom_interface_node(element_of_value(img))),
           "§4.8.3's legacy factory function created something that is not an `img` element — DOM §4.9's "
           "create an element was given the local name `img` in the HTML namespace, so a node that is not one "
           "means the element-interface resolution and this brand disagree about what an img is");
    /* Steps 3-4. DOM §4.9 "Interface Element"'s SET AN ATTRIBUTE VALUE goes through the mutation chokepoint,
       which is what makes the write per-flow AND what runs the same section's attribute change steps —
       including this file's own, which is why an `Image(w,h)` whose `width` counts as a relevant mutation is
       one update rather than a special case.
       THE VALUE CROSSES AS THE VALUE, not as a C string this function formats: `new Image(cfg.thumbSize)`
       passes an argument the page computed, and where that is unknown external input the attribute must keep
       its provenance rather than be stamped with whatever number a coercion invented. element_attr_set_value
       is the accessor that carries the whole triple into §@S's (element, name) shadow. */
    if (argc >= 1) element_attr_set_value(ctx, img, "width", argv[0]);
    if (argc >= 2) element_attr_set_value(ctx, img, "height", argv[1]);
    return img;                                                        /* step 5 */
}

/* ---- declaration and installation -------------------------------------------------------------------------- */

void html_image_declare(JSContext *ctx)
{
    static const IdlArgType FACTORY_ARGS[2] = { IDL_UNSIGNED_LONG, IDL_UNSIGNED_LONG };
    JSRuntime *rt = JS_GetRuntime(ctx);

    DCHECK(!g_ready, "html_image_declare ran twice — §4.8.3's state key and machines are declared once per "
                     "AGENT, and a second Symbol would leave every element that already carries an image "
                     "request under the first key answering `unavailable` under the second");
    g_state_key = JS_NewSymbol(ctx, "imageRequests", false);
    CHECK(!JS_IsException(g_state_key), "§4.8.4.3: the image request slot key allocation failed");
    g_atom_state = JS_ValueToAtom(ctx, g_state_key);
    CHECK(g_atom_state != JS_ATOM_NULL, "§4.8.4.3: the image request slot key could not be interned");

    /* §4.8.3's two `[ReflectSetter] attribute unsigned long` setters. The Web IDL `unsigned long` conversion
       — ToNumber, then modulo 2^32 — runs the page's own `valueOf` before either body is entered, which is
       why they are declared rather than written as plain C setters. */
    g_id_set_dimension[0] = idl_setter_id(ctx, IDL_UNSIGNED_LONG, false, js_img_dimension_set, 0);
    g_id_set_dimension[1] = idl_setter_id(ctx, IDL_UNSIGNED_LONG, false, js_img_dimension_set, 1);
    g_id_factory = idl_method_id(ctx, FACTORY_ARGS, 2, js_image_factory, 0);
    /* `Image(optional unsigned long width, optional unsigned long height)` — BOTH optional, which is what
       makes the function object's `length` 0 (Web IDL §3.7.2: "the length of the shortest argument list"). */
    idl_optional_from(0);
    g_task_stepid = JS_RegisterStepDef(rt, &img_task_def);
    g_ready = 1;
}

void html_image_install(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_ready, "a realm asked for §4.8.3's members before the interface was declared");
    /* The ten `[Reflect]`ed members — `src`, `srcset`, `sizes`, `alt`, `useMap`, `crossOrigin`,
       `referrerPolicy`, `loading`, `decoding`, `isMap` — are core/html/html_element.c's R_IMG and are already
       on this prototype. What goes on here is every §4.8.3 member that COMPUTES rather than mirrors. */
    idl_install_accessor(ctx, proto, "naturalWidth", js_img_natural, 0, -1);
    idl_install_accessor(ctx, proto, "naturalHeight", js_img_natural, 1, -1);
    /* `[CEReactions, ReflectSetter] attribute unsigned long width` and its `height` twin. `ReflectSetter` and
       not `Reflect` is what keeps them out of core/html/html_element.c's reflection table: only the SETTER is a
       reflection, and the getter is §4.8.3's determine the dimensions, which is a layout question. */
    idl_install_accessor(ctx, proto, "width", js_img_dimension, 0, g_id_set_dimension[0]);
    idl_install_accessor(ctx, proto, "height", js_img_dimension, 1, g_id_set_dimension[1]);
    idl_install_accessor(ctx, proto, "complete", js_img_complete, 0, -1);
    idl_install_accessor(ctx, proto, "currentSrc", js_img_current_src, 0, -1);
    idl_members_excluded(ctx, proto, "HTMLImageElement", IMG_ABSENT,
                         (int)(sizeof(IMG_ABSENT) / sizeof(IMG_ABSENT[0])),
                         "§4.8.3's `decode()` resolves when the image has been decoded and rejects with an "
                         "`EncodingError` when it cannot be, so it needs the DECODER this agent does not have "
                         "— the same component `naturalWidth` and determine-the-dimensions' second branch "
                         "crash for, and they must arrive together. `fetchPriority` and `controls` are "
                         "§2.5.9's and §4.8.3's own attributes with no consumer in this build");
}

void html_image_install_global(JSContext *ctx, JSValueConst global, JSValueConst proto)
{
    JSValue factory;

    DCHECK(g_ready, "§4.8.3's legacy factory function was installed before the interface was declared");
    DCHECK(JS_IsObject(proto),
           "§4.8.3's legacy factory function was installed with no HTMLImageElement.prototype — Web IDL "
           "§3.7.2 gives F a non-configurable `prototype` naming the interface prototype object of THIS "
           "realm, and a factory whose `prototype` is not that object makes `new Image() instanceof "
           "HTMLImageElement` and `Image.prototype === HTMLImageElement.prototype` disagree");
    /* Web IDL §3.7.2: "Let F be CreateBuiltinFunction(steps, length, id, « », realm)", with length 0 and id
       `Image`. It is a step-declared constructor so the two `unsigned long` arguments are converted by the
       declaration before the body runs, and so a call without `new` arrives with an undefined receiver. */
    factory = idl_step_constructor(ctx, "Image", 0, g_id_factory);
    CHECK(!JS_IsException(factory), "§4.8.3: the `Image` legacy factory function could not be allocated");
    /* "Perform ! DefinePropertyOrThrow(F, "prototype", PropertyDescriptor{[[Value]]: proto, [[Writable]]:
       false, [[Enumerable]]: false, [[Configurable]]: false})." NOT JS_SetConstructor, which would also write
       `HTMLImageElement.prototype.constructor = Image` — a legacy factory function is not the interface's
       constructor, and `Image.prototype.constructor` must stay `HTMLImageElement`. Flags 0 is exactly
       {[[Writable]]: false, [[Enumerable]]: false, [[Configurable]]: false}. */
    JS_DefinePropertyValueStr(ctx, factory, "prototype", JS_DupValue(ctx, proto), 0);
    JS_SetPropertyStr(ctx, (JSValue)global, "Image", factory);
}

void html_image_free(JSRuntime *rt)
{
    if (!g_ready) return;
    g_ready = 0;
    JS_FreeAtomRT(rt, g_atom_state);
    g_atom_state = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_state_key);
    g_state_key = JS_UNDEFINED;
    g_id_factory = -1;
    g_id_set_dimension[0] = -1;
    g_id_set_dimension[1] = -1;
    g_task_stepid = -1;
}
