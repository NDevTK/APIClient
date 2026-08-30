/* THE MEDIA ELEMENT — HTML §4.8.11, as a real state machine with a MOCK MEDIA DEVICE under it.
 *
 * WHAT WAS HERE BEFORE, AND WHY IT WAS THE BIGGEST GAP LEFT. `<audio>` and `<video>` had SIX reflected content
 * attributes on two prototypes that inherited from HTMLElement directly, and nothing else: no `play`, no
 * `paused`, no `readyState`, no `currentTime`, no `duration`, no `error`, no `canPlayType`, and no
 * HTMLMediaElement interface at all. Every one of those is a page-visible value the spec COMPUTES, so a bundle
 * asking `if (v.canPlayType('video/mp4;codecs="avc1.42E01E"'))` or `v.play().catch(fallback)` threw a
 * TypeError on the first line and everything behind it — the fallback player, the telemetry endpoint, the
 * "your browser can't play this" route — was never reached. §Headless names this file's job exactly: the only
 * missing piece is a physical IO device, and the spec still defines the whole state machine without one.
 *
 * THE DEVICE IS MODELLED AS AN UNBOUNDED STREAM, AND EVERY NUMBER IT ANSWERS IS THE SPEC'S OWN. This is the
 * one decision the rest follows from, and it invents nothing (§Attacker-sources: COMPUTE OR SHAPE, NEVER
 * INVENT). No bytes are fetched, so the length of the resource is not known — and §4.8.11.6 already says what
 * `duration` is for a resource "not known to be bounded": positive Infinity. From that one spec-derived fact
 * everything else follows without a single invented constant:
 *   - `seekable` and `buffered` are ONE range, [0, duration) — §4.8.11.9's "if the user agent can seek to
 *     anywhere in the media resource … one range whose start is the earliest possible position and whose end
 *     is the same plus the duration";
 *   - `played` is EMPTY until the position advances, because §4.8.11.8 defines it as the ranges reached
 *     "through the usual monotonic increase of the current playback position during normal playback" and no
 *     media timeline clock has advanced;
 *   - `ended` is false forever, because §4.8.11.8's ended playback requires the position to reach the end of
 *     the resource and an unbounded stream has none. A page waiting for `ended` on a live stream waits in a
 *     real browser too.
 * A shorter modelled duration would be a number nobody computed, which is what §H forbids — and it would make
 * `ended` a lie the moment a page seeked past it.
 *
 * WHAT IS UNKNOWABLE IS THE ONE THING THAT FORKS: WHETHER THE RESOURCE IS USABLE. Whether the bytes at
 * `currentSrc` decode is a fact about a server this engine has not read, so §4.8.11.5's media data processing
 * steps are an OUTCOME FORK (solver/decide.h's seam, the same one JSON.parse over unknown text uses). Outcome
 * 0 — the arm a run with no forking policy takes, which is step_fork_run's rule — is the resource being
 * USABLE, because that is the ordinary completion and the one an @S candidate re-fire must follow to a sink.
 * Outcome 1 is the load having failed, and WHICH of §4.8.11.5's three failure labels that reaches belongs to
 * the MODE: object and attribute mode run the dedicated media source failure steps (a
 * MEDIA_ERR_SRC_NOT_SUPPORTED MediaError, `error` at the media element, the play promises rejected), while
 * children mode fires `error` at the `source` ELEMENT and goes looking for the next candidate — it never sets
 * `error` at all. Both arms are real code in real pages and they run DIFFERENT endpoints: one
 * flow reaches `oncanplay`/`onloadedmetadata`, its sibling reaches `onerror` and whatever fallback it fetches.
 * That fork is the whole value of this file to the solver, and it is why the load path had to be a state
 * machine rather than a getter that shrugs.
 *
 * AND `duration` IS CONCOLIC WITH THAT MODELLED ANSWER AS ITS EXAMPLE — matchMedia's rule for `.matches`
 * (core/css/media_query_list.c), for matchMedia's reason. The modelled device answers +Infinity so a
 * conformance host reads a real number, and an exploring host reads a concolic carrying it, so
 * `if (v.duration > 60)` still forks into the world where the resource is long. Collapsing it to bare
 * concrete would delete that world; shrugging to opaque would delete the value.
 *
 * THE STATE IS OWN SLOTS ON THE ELEMENT'S WRAPPER, WHICH IS WHAT MAKES IT TIME-TRAVEL. §State-isolation's rule
 * is that platform data a flow queues is a JS VALUE and never malloc'd C: a record hung off a private Symbol
 * is written with ordinary property writes, which the per-flow heap COW delta already captures — so two forked
 * arms that both call `play()` each hold their own `paused`, their own pending play promises and their own
 * `error`, and a flow parked at an `await` between two of them resumes with exactly what it wrote. The list of
 * pending play promises is a JS Array for the same reason: a malloc'd list captured by pointer reverts the
 * POINTER on a context switch and leaves the nodes reachable from nothing.
 *
 * WHAT IS HONESTLY ABSENT. The TRACK surface — `audioTracks`, `videoTracks`, `textTracks` and `addTextTrack` —
 * is four members over five interfaces (AudioTrackList, VideoTrackList, TextTrackList, TextTrack,
 * TextTrackCue) that this file does not build, so it does not install them: a page reading `v.textTracks`
 * gets `undefined` and its own TypeError, which is this engine's forcing function, and engine/idlgen.mjs's
 * audit reports the four by name against this component. The two steps of §4.8.11.5 that would DRAIN those
 * lists — "forget the media element's media-resource-specific tracks" and the media data processing steps'
 * addtrack — assert their producer's absence through realm_awaits, so the day someone lands TextTrackList the
 * DCHECK fires AT the step that must then be written rather than the step silently doing nothing forever. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/css/media_query.h"
#include "core/dom/document.h"
#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"
#include "core/events/event.h"
#include "core/frame/sandboxing.h"
#include "core/events/event_target.h"
#include "core/html/media_element.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/mime/mime_type.h"
#include "core/realm.h"
#include "solver/concolic.h"
#include "solver/decide.h"

/* §4.8.11.4's network states and §4.8.11.7's ready states, as the IDL's own constants. */
enum { NETWORK_EMPTY = 0, NETWORK_IDLE = 1, NETWORK_LOADING = 2, NETWORK_NO_SOURCE = 3 };
enum { HAVE_NOTHING = 0, HAVE_METADATA = 1, HAVE_CURRENT_DATA = 2, HAVE_FUTURE_DATA = 3, HAVE_ENOUGH_DATA = 4 };
/* §4.8.11.1's error codes. */
enum { MEDIA_ERR_ABORTED = 1, MEDIA_ERR_NETWORK = 2, MEDIA_ERR_DECODE = 3, MEDIA_ERR_SRC_NOT_SUPPORTED = 4 };

/* §4.8.11.8's muted STATE, which is three-valued ("either true, false, or `default`") and not a boolean. */
enum { MUTED_DEFAULT = 0, MUTED_TRUE = 1, MUTED_FALSE = 2 };

/* PER REALM — §3.7, and here it decides ANSWERS: a C member runs in the realm that DEFINED it, so a prototype
   held in a module static would answer every document's question with the defining realm's. These are
   quickjs's own per-context class-proto slots. HTMLMediaElement has no instances of its own class (an instance
   is an `<audio>` or `<video>` element wrapper, whose class is html_element.c's); the class id is how this
   realm's prototype is held, exactly as HTMLElement.prototype is held by html_element.c's. */
static JSClassID g_media_class, g_error_class, g_ranges_class;
/* The private Symbol §4.8.11's per-element state hangs off, and the atom for the own-slot read. */
static JSValue g_state_key = JS_UNDEFINED;
static JSAtom  g_atom_state = JS_ATOM_NULL;
/* Declared once per AGENT; installed into every realm. */
static int g_refl_base = -1;
static int g_id_load = -1, g_id_can_play = -1, g_id_play = -1, g_id_pause = -1, g_id_fast_seek = -1,
           g_id_start_date = -1, g_id_range_start = -1, g_id_range_end = -1;
static int g_set_src_object = -1, g_set_current_time = -1, g_set_volume = -1, g_set_muted = -1,
           g_set_rate = -1, g_set_default_rate = -1, g_set_pitch = -1, g_set_loading = -1;
static int g_task_stepid = -1, g_select_stepid = -1;
static int g_ready;

/* ---- the element, and its state record --------------------------------------------------------------------- */

/* WHICH NODES ARE MEDIA ELEMENTS — §4.8.11's first sentence, over the INTERNED TAG ID and the namespace, which
   is what core/html/html_script.c's script_is asks and for both of its reasons.
   IT IS THE CORRECT FORM, not merely the cheap one. The name-only test it replaces answered TRUE for an SVG
   `<video>`, which is not a media element and whose wrapper wears none of §4.8.11's members — and
   `HTMLMediaElement.prototype.play.call(svgVideo)` is a call any page can write, so the brand is what stands
   between that call and a state record on an element the interface does not describe.
   AND IT IS THE CHEAP ONE, which is what §4.8.11.2's walk below needs: three integer compares, no allocation
   and no wrapper, asked of every node of a parsed document. */
static bool media_is_node(const lxb_dom_node_t *n)
{
    return n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT &&
           (lxb_html_tree_node_is(n, LXB_TAG_AUDIO) || lxb_html_tree_node_is(n, LXB_TAG_VIDEO));
}

/* WHICH NODES ARE `source` ELEMENTS — §4.8.11.5 step 10's "source element child" and §4.8.12's insertion
   steps, over the interned tag id and the namespace for media_is_node's two reasons: an SVG `<source>` is not
   one, and the question is asked of every child of every media element and of every inserted node. */
static bool media_source_is(const lxb_dom_node_t *n)
{
    return n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT && lxb_html_tree_node_is(n, LXB_TAG_SOURCE);
}

/* §4.8.11.5 step 10's "the first source element child in tree order", or NULL — ONE walker, so the mode
   selection and the parse boundary's §4.8.12 sweep cannot disagree about what a candidate is. */
static lxb_dom_node_t *media_first_source(const lxb_dom_node_t *el)
{
    lxb_dom_node_t *c;

    DCHECK(el != NULL, "§4.8.11.5 step 10 was asked for the source children of nothing");
    for (c = el->first_child; c; c = c->next)
        if (media_source_is(c)) return c;
    return NULL;
}

/* The same question asked of a VALUE — the brand every member of this file performs. It is asked of the NODE
   rather than of a wrapper class, because that is what the spec says a media element is. */
static bool media_is(JSContext *ctx, JSValueConst v)
{
    (void)ctx;
    return media_is_node(lxb_dom_interface_node(element_of_value(v)));
}

/* THE STATE RECORD, created where a flow first REACHES the element — core/css/media_query_list.c's rule for a
   record a flow may write, and here it is also the record's only creation site, so there is no write site left
   to miss. Every field is an ordinary property, so every write is captured by the running flow's COW delta.
   The initial values are §4.8.11's own: paused true, muted state "default", volume 1.0, both playback rates
   1.0, preservesPitch true, the three positions zero, the timeline offset NaN, `can autoplay` true. */
static JSValue media_state(JSContext *ctx, JSValueConst el)
{
    JSValue st;

    DCHECK(g_ready, "a media element's state was reached before §4.8.11 was declared");
    if (JS_GetOwnSlot(ctx, &st, el, g_atom_state) > 0) return st;

    st = idl_slots_new(ctx);
    CHECK(!JS_IsException(st), "§4.8.11: OOM building a media element's state");
    JS_SetPropertyStr(ctx, st, "networkState", JS_NewInt32(ctx, NETWORK_EMPTY));
    JS_SetPropertyStr(ctx, st, "readyState", JS_NewInt32(ctx, HAVE_NOTHING));
    JS_SetPropertyStr(ctx, st, "currentSrc", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, st, "error", JS_NULL);
    JS_SetPropertyStr(ctx, st, "srcObject", JS_NULL);
    JS_SetPropertyStr(ctx, st, "paused", JS_TRUE);
    JS_SetPropertyStr(ctx, st, "seeking", JS_FALSE);
    JS_SetPropertyStr(ctx, st, "showPoster", JS_TRUE);
    JS_SetPropertyStr(ctx, st, "canAutoplay", JS_TRUE);
    JS_SetPropertyStr(ctx, st, "current", JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, st, "official", JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, st, "defaultStart", JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, st, "duration", JS_NewFloat64(ctx, NAN));
    JS_SetPropertyStr(ctx, st, "timelineOffset", JS_NewFloat64(ctx, NAN));
    JS_SetPropertyStr(ctx, st, "rate", JS_NewFloat64(ctx, 1.0));
    JS_SetPropertyStr(ctx, st, "defaultRate", JS_NewFloat64(ctx, 1.0));
    JS_SetPropertyStr(ctx, st, "preservesPitch", JS_TRUE);
    JS_SetPropertyStr(ctx, st, "volume", JS_NewFloat64(ctx, 1.0));
    JS_SetPropertyStr(ctx, st, "muted", JS_NewInt32(ctx, MUTED_DEFAULT));
    JS_SetPropertyStr(ctx, st, "loadedData", JS_FALSE);   /* §4.8.11.7's "first time … since load()" latch */
    /* §4.8.11.5 step 14's CHILDREN MODE parked this element at its WAITING step, whose only wake is a `source`
       inserted into it — so the flag is read by §4.8.12's source element insertion steps and by nothing else.
       It is a POSITIVE statement and not a hole: false is "this element is not waiting at a pointer", which is
       a different fact from NETWORK_NO_SOURCE, since step 1 and the dedicated media source failure steps both
       leave that same network state behind without any wait. */
    JS_SetPropertyStr(ctx, st, "sourceWait", JS_FALSE);
    {
        JSValue promises = JS_NewArray(ctx);
        CHECK(!JS_IsException(promises), "§4.8.11.8: OOM building the list of pending play promises");
        JS_SetPropertyStr(ctx, st, "playPromises", promises);
    }
    JS_DefinePropertyValue(ctx, (JSValue)el, g_atom_state, JS_DupValue(ctx, st),
                           JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    return st;
}

/* The receiver's state, or NULL with a TypeError pending — one brand check, spelled once. */
static JSValue media_state_of(JSContext *ctx, JSValueConst this_val, const char *member)
{
    if (!media_is(ctx, this_val))
        return JS_ThrowTypeError(ctx, "%s called on something that is not a media element", member);
    return media_state(ctx, this_val);
}

static int32_t st_int(JSContext *ctx, JSValueConst st, const char *name)
{
    JSValue v = JS_GetPropertyStr(ctx, st, name);
    int32_t n = 0;

    DCHECK(JS_IsNumber(v), "§4.8.11's state answered a numeric field with something that is not a number — "
                           "every field is written by this file and by nothing else");
    JS_ToInt32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

static double st_num(JSContext *ctx, JSValueConst st, const char *name)
{
    JSValue v = JS_GetPropertyStr(ctx, st, name);
    double d = 0;

    DCHECK(JS_IsNumber(v), "§4.8.11's state answered a position with something that is not a number");
    JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    return d;
}

static bool st_bool(JSContext *ctx, JSValueConst st, const char *name)
{
    JSValue v = JS_GetPropertyStr(ctx, st, name);
    bool b;

    DCHECK(JS_IsBool(v), "§4.8.11's state answered a flag with something that is not a boolean");
    b = JS_ToBool(ctx, v) != 0;
    JS_FreeValue(ctx, v);
    return b;
}

static void st_set_int(JSContext *ctx, JSValueConst st, const char *name, int32_t v)
{
    JS_SetPropertyStr(ctx, (JSValue)st, name, JS_NewInt32(ctx, v));
}

static void st_set_num(JSContext *ctx, JSValueConst st, const char *name, double v)
{
    JS_SetPropertyStr(ctx, (JSValue)st, name, JS_NewFloat64(ctx, v));
}

static void st_set_bool(JSContext *ctx, JSValueConst st, const char *name, bool v)
{
    JS_SetPropertyStr(ctx, (JSValue)st, name, JS_NewBool(ctx, v));
}

/* ---- the MOCK MEDIA DEVICE ---------------------------------------------------------------------------------
 *
 * Two questions a real device answers from hardware and this one answers from the spec: which types it renders
 * (§4.8.11.3's canPlayType), and what the timeline of a resource it has selected looks like (§4.8.11.6). */

/* §4.8.11.3: "a type that the user agent knows it cannot render is one that describes a resource that the user
   agent definitely does not support, for example because it doesn't recognize the container type". These are
   the container types the modelled device recognises — a UA capability, stated as data the way the modelled
   viewport is, and NOT a MIME group (Sniffing §4.6's audio-or-video group answers a different question: what a
   byte stream IS, not what this device can play). */
static const char *const DEVICE_CONTAINERS[] = {
    "audio/mpeg", "audio/mp4", "audio/aac", "audio/ogg", "audio/wav", "audio/webm", "audio/flac",
    "video/mp4", "video/webm", "video/ogg", "video/mpeg", "video/quicktime",
    "application/vnd.apple.mpegurl",
};

/* §4.8.11.3's three answers, computed. "" when the type is one the device knows it cannot render or is
   `application/octet-stream` with no parameters; "probably" when the container is recognised AND a codecs
   parameter names what is in it (the spec: "a user agent should never return probably for a type that allows
   the codecs parameter if that parameter is not present"); "maybe" otherwise. */
static bool device_recognises(const char *essence)
{
    int i;

    for (i = 0; i < (int)(sizeof(DEVICE_CONTAINERS) / sizeof(DEVICE_CONTAINERS[0])); i++)
        if (!strcmp(essence, DEVICE_CONTAINERS[i])) return true;
    return false;
}

static const char *device_can_play(const char *type)
{
    MimeType m;
    char *essence;
    const char *answer = "";

    mime_type_init(&m);
    if (!mime_type_parse(&m, type, strlen(type))) { mime_type_free(&m); return ""; }
    essence = mime_type_essence(&m);
    CHECK(essence != NULL, "§4.8.11.3: OOM taking a MIME type's essence");
    /* The one special case the standard names: `application/octet-stream` with NO parameters is never a type
       the user agent knows it cannot render, and canPlayType must answer the empty string for it anyway. */
    if (!strcmp(essence, "application/octet-stream")) {
        free(essence);
        mime_type_free(&m);
        return "";
    }
    if (device_recognises(essence))
        answer = mime_type_parameter(&m, "codecs") ? "probably" : "maybe";
    free(essence);
    mime_type_free(&m);
    return answer;
}

/* THE SAME UA CAPABILITY, ASKED BY A SECOND STANDARD — see media_element.h. */
bool media_device_renders(const MimeType *m)
{
    char *essence;
    bool yes;

    DCHECK(m != NULL && m->type != NULL && m->subtype != NULL,
           "the modelled media device was asked whether it renders something that is not a MIME type — the "
           "question is about a resource's TYPE and a half-built record is what §4.4 leaves behind on failure");
    essence = mime_type_essence(m);
    CHECK(essence != NULL, "§4.8.11.3: OOM taking a MIME type's essence");
    yes = device_recognises(essence);
    free(essence);
    return yes;
}

/* §4.8.11.6's ESTABLISH THE MEDIA TIMELINE for the modelled device: the resource is not known to be bounded,
   so its duration is positive Infinity and its earliest possible position is zero. */
#define DEVICE_DURATION  INFINITY

/* THE SOURCE IDENTITY of "the bytes at this address" — what the outcome fork is keyed by and what `duration`
   is tagged with. The ADDRESS is part of it because two `<video>` elements pointing at two URLs are two
   different questions, and one key would let a fork taken for one decide the other. */
static void media_source_id(JSContext *ctx, JSValueConst st, char *shape, size_t nshape, char *src,
                            size_t nsrc)
{
    JSValue cur = JS_GetPropertyStr(ctx, st, "currentSrc");
    const char *url = JS_ToCString(ctx, cur);

    snprintf(shape, nshape, "{media:%s}", url ? url : "");
    snprintf(src, nsrc, "{media}%s", url ? url : "");
    if (url) JS_FreeCString(ctx, url);
    JS_FreeValue(ctx, cur);
}

/* THE MEDIA RESOURCE, AS A VALUE THE OUTCOME SEAM CAN BE ASKED ABOUT. In an exploring host this is a concolic
   with no example — the bytes are external input nobody has read, which is the most general concolic value —
   and in a conformance host it is JS_UNDEFINED, which is not concolic, so the fork is never asked and the
   modelled device's own answer (the resource is usable) stands. */
static JSValue media_resource_value(JSContext *ctx, JSValueConst st)
{
    char shape[256], src[256];

    media_source_id(ctx, st, shape, sizeof shape, src, sizeof src);
    return concolic_source_wrap(ctx, shape, src, JS_UNDEFINED);
}

/* ---- §4.8.11.14's TimeRanges --------------------------------------------------------------------------------
 *
 * A TimeRanges object is "ranges, a list of zero or more time ranges", each a start and an end. The list is an
 * own slot holding a JS Array of [start, end] pairs, for the reason the element's state is one: it is built
 * inside a flow (every getter that returns one mints a NEW object, which the spec enshrines) and every write
 * to it is a property write the delta captures. */

static JSValue ranges_new(JSContext *ctx, const double *pairs, int n)
{
    JSValue proto = JS_GetClassProto(ctx, g_ranges_class), obj, list;
    int i;

    DCHECK(!JS_IsNull(proto), "a TimeRanges was minted in a realm that never ran §4.8.11's install");
    obj = JS_NewObjectProtoClass(ctx, proto, g_ranges_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) return obj;
    list = JS_NewArray(ctx);
    CHECK(!JS_IsException(list), "§4.8.11.14: OOM building a TimeRanges' ranges");
    for (i = 0; i < n; i++) {
        JSValue pair = JS_NewArray(ctx);
        CHECK(!JS_IsException(pair), "§4.8.11.14: OOM building a time range");
        JS_SetPropertyUint32(ctx, pair, 0, JS_NewFloat64(ctx, pairs[2 * i]));
        JS_SetPropertyUint32(ctx, pair, 1, JS_NewFloat64(ctx, pairs[2 * i + 1]));
        JS_SetPropertyUint32(ctx, list, (uint32_t)i, pair);
    }
    JS_DefinePropertyValue(ctx, obj, g_atom_state, list, JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    return obj;
}

/* The ranges list, or NULL with a TypeError pending. */
static JSValue ranges_list(JSContext *ctx, JSValueConst this_val, const char *member)
{
    JSValue list;

    if (JS_GetOwnSlot(ctx, &list, this_val, g_atom_state) <= 0)
        return JS_ThrowTypeError(ctx, "%s called on something that is not a TimeRanges", member);
    /* One private Symbol carries the internal state of all three of this file's object kinds, so the brand is
       WHAT IS UNDER IT: only a TimeRanges keeps a ranges list there, and `TimeRanges.prototype.start.call(x)`
       on a MediaError must be the TypeError §3.7.5 gives it rather than an IndexSizeError. */
    if (!JS_IsArray(list)) {
        JS_FreeValue(ctx, list);
        return JS_ThrowTypeError(ctx, "%s called on something that is not a TimeRanges", member);
    }
    return list;
}

static uint32_t arr_len(JSContext *ctx, JSValueConst arr)
{
    JSValue len = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    return n;
}

static JSValue js_ranges_length(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue list = ranges_list(ctx, this_val, "length");
    uint32_t n;

    (void)magic;
    if (JS_IsException(list)) return list;
    n = arr_len(ctx, list);
    JS_FreeValue(ctx, list);
    return JS_NewUint32(ctx, n);
}

/* §4.8.11.14's start(index) and end(index): "if index is greater than or equal to this's ranges's size, then
   throw an IndexSizeError", otherwise the range's start or end. `magic` is which of the two ends. */
static JSValue js_ranges_at(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue list = ranges_list(ctx, this_val, magic ? "end" : "start"), pair, out;
    uint32_t i = 0;

    DCHECK(argc >= 1, "TimeRanges.start/end reached its body with no index — §3.6 step 5's TypeError for a "
                      "call short of a member's required arguments is the declaration's");
    if (JS_IsException(list)) return list;
    JS_ToUint32(ctx, &i, argv[0]);
    if (i >= arr_len(ctx, list)) {
        JS_FreeValue(ctx, list);
        return JS_ThrowDOMException(ctx, "IndexSizeError", "the index is not in the TimeRanges' ranges");
    }
    pair = JS_GetPropertyUint32(ctx, list, i);
    out = JS_GetPropertyUint32(ctx, pair, (uint32_t)(magic ? 1 : 0));
    JS_FreeValue(ctx, pair);
    JS_FreeValue(ctx, list);
    return out;
}

/* §4.8.11.9's seekable and §4.8.11.5's buffered, for the modelled device: ONE range over the whole timeline
   once the metadata is known, and nothing before that. `played` is empty until the position advances. */
static JSValue media_ranges_for(JSContext *ctx, JSValueConst st, bool played)
{
    double pair[2];

    if (played || st_int(ctx, st, "readyState") < HAVE_METADATA)
        return ranges_new(ctx, NULL, 0);
    pair[0] = 0;
    pair[1] = st_num(ctx, st, "duration");
    return ranges_new(ctx, pair, 1);
}

/* ---- §4.8.11.1's MediaError ---------------------------------------------------------------------------------
 *
 * "To create a MediaError, given an error code … return a new MediaError object whose code is the given error
 * code and whose message is a string containing any details the user agent is able to supply … or the empty
 * string if the user agent is unable to supply such details." This user agent has no decoder to report from,
 * so the message is the empty string, which is what that sentence asks for. */
static JSValue media_error_new(JSContext *ctx, int code)
{
    JSValue proto = JS_GetClassProto(ctx, g_error_class), obj, slots;

    DCHECK(!JS_IsNull(proto), "a MediaError was minted in a realm that never ran §4.8.11's install");
    obj = JS_NewObjectProtoClass(ctx, proto, g_error_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) return obj;
    slots = idl_slots_new(ctx);
    CHECK(!JS_IsException(slots), "§4.8.11.1: OOM building a MediaError's slots");
    JS_SetPropertyStr(ctx, slots, "code", JS_NewInt32(ctx, code));
    JS_SetPropertyStr(ctx, slots, "message", JS_NewString(ctx, ""));
    JS_DefinePropertyValue(ctx, obj, g_atom_state, slots, JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    return obj;
}

static JSValue js_media_error_field(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue slots, v;

    if (JS_GetOwnSlot(ctx, &slots, this_val, g_atom_state) <= 0)
        return JS_ThrowTypeError(ctx, "a MediaError member was read on something that is not a MediaError");
    v = JS_GetPropertyStr(ctx, slots, magic ? "message" : "code");
    JS_FreeValue(ctx, slots);
    return v;
}

/* ---- §4.8.11's MEDIA ELEMENT EVENT TASK SOURCE ---------------------------------------------------------------
 *
 * "To queue a media element task with a media element element and a series of steps steps, queue an element
 * task on the media element's media element event task source given element and steps." The task is a JOB,
 * which in this engine is a call-root FLOW: preemptible, forkable and parkable — it has to be, because it
 * FIRES events and every listener body is the page's code.
 *
 * ONE MACHINE, because the tasks §4.8.11 queues differ in exactly two ways: which events they fire, in order,
 * and what they do to the list of pending play promises when the fires are done. Both are ARGUMENTS. A machine
 * per event name would be the same algorithm written out once per string. */
enum { MTA_FIRE = 0, MTA_RESOLVE, MTA_REJECT_ABORT, MTA_FAILURE };

typedef struct {
    JSStepHdr hdr;
    /* HAVE I STARTED — a machine can never answer that from its stage, because the first stage IS the entry
       stage (quickjs-step.h), and this machine's owned fields must be JS_UNDEFINED before the first thing that
       can fail: a zeroed block is not a block of JS_UNDEFINEDs. */
    uint8_t   started;
    uint32_t  i;        /* which event of the list is next */
    uint8_t   fphase;   /* the fire request's own phase */
    uint8_t   cphase;   /* the settle call's own phase */
    JSValue   ev;       /* the event being fired, held across the suspension (owned) */
    JSValue   reason;   /* the DOMException a rejecting task rejects with (owned) */
    EventFireCb cb;     /* the fire request buffer */
    JSValue   ccb[3];   /* the settle call's buffer: [this, func, value] */
} MediaTask;

#define MEDIA_TASK_STAGES(X) \
    X(MT_FIRE,   "HTML §4.8.11 the queued media element task (fire an event named e at the media element)") \
    X(MT_SETTLE, "HTML §4.8.11.8 resolve pending play promises / reject pending play promises (settle the " \
                 "promises this task took, one per entry)")
enum { MEDIA_TASK_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const MEDIA_TASK_STEPS[] = { MEDIA_TASK_STAGES(JS_STEP_STAGE_LABEL) NULL };

static void media_task_visit(JSContext *ctx, void *stp, JSStepVisit *v)
{
    MediaTask *s = stp;
    int k;

    v->val(ctx, &s->ev);
    v->val(ctx, &s->reason);
    STEP_CB_FOREACH(s->cb, k) v->val(ctx, &s->cb[k]);
    STEP_CB_FOREACH(s->ccb, k) v->val(ctx, &s->ccb[k]);
}

static int media_task_step(JSContext *ctx, void *stp, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    MediaTask *s = stp;
    JSValueConst element = step_arg(&s->hdr, 0);
    JSValueConst events = step_arg(&s->hdr, 1);
    JSValueConst promises = step_arg(&s->hdr, 2);
    int action = 0, r;

    {
        int32_t a = 0;
        JS_ToInt32(ctx, &a, step_arg(&s->hdr, 3));
        action = a;
    }

    STEP_DISPATCH(MEDIA_TASK_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(MT_FIRE);
        if (!s->started) {
            int k;

            s->started = 1;
            s->i = 0;
            s->fphase = s->cphase = 0;
            s->ev = s->reason = JS_UNDEFINED;
            STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
            STEP_CB_FOREACH(s->ccb, k) s->ccb[k] = JS_UNDEFINED;
            /* §4.8.11.5's DEDICATED MEDIA SOURCE FAILURE STEPS 1-4, WHICH RUN IN THIS TASK RATHER THAN AT THE
               ENQUEUE. The standard queues a media element task "to run the dedicated media source failure
               steps", so between the algorithm's failure and this task `error` is still null and networkState
               is still NETWORK_LOADING — which is what a page reading `v.error` from a microtask observes.
               Running them at the enqueue reported the finished state for that whole window. Steps 5 and 6 are
               this machine's own two arms: the `error` fire and the "NotSupportedError" rejection. */
            if (action == MTA_FAILURE) {
                JSValue fst = media_state(ctx, element);
                JSValue err = media_error_new(ctx, MEDIA_ERR_SRC_NOT_SUPPORTED);

                CHECK(!JS_IsException(err), "§4.8.11.1: OOM creating a MediaError");
                JS_SetPropertyStr(ctx, fst, "error", err);              /* step 1 */
                realm_awaits(ctx, "HTMLMediaElement.prototype.textTracks",
                             "HTML §4.8.11.5's dedicated media source failure steps step 2 must FORGET the "
                             "media element's media-resource-specific tracks — remove them from the list of "
                             "text tracks and empty the AudioTrackList and VideoTrackList — write that step "
                             "here");
                st_set_int(ctx, fst, "networkState", NETWORK_NO_SOURCE); /* step 3 */
                st_set_bool(ctx, fst, "showPoster", true);               /* step 4 */
                JS_FreeValue(ctx, fst);
            }
        }
        for (;;) {
            if (JS_IsUndefined(s->ev)) {
                JSValue name;
                const char *type;

                if (s->i >= arr_len(ctx, events)) break;
                name = JS_GetPropertyUint32(ctx, events, s->i);
                type = JS_ToCString(ctx, name);
                JS_FreeValue(ctx, name);
                CHECK(type != NULL, "§4.8.11: OOM reading a queued task's event name");
                /* None of §4.8.11's own events bubbles and none is cancelable — the standard names each fire
                   as "fire an event named e at the element" and gives no initialiser. */
                s->ev = event_new(ctx, type, /*bubbles*/ false, /*cancelable*/ false);
                JS_FreeCString(ctx, type);
                if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; return JS_STEP_ABRUPT; }
            }
            r = event_target_fire_run(ctx, &s->fphase, STEP_CB(s->cb), element, s->ev, JS_UNDEFINED, cb_result,
                                      NULL, out_cb, out_argc);
            if (r > 0) return r;
            cb_result = JS_UNDEFINED;
            JS_FreeValue(ctx, s->ev);
            s->ev = JS_UNDEFINED;
            if (r < 0) return JS_STEP_ABRUPT;
            s->i++;
            /* One event per turn: a listener list is of the PAGE'S size, so the scheduler is asked between
               two fires rather than after all of them. */
            if (s->i < arr_len(ctx, events)) return JS_STEP_YIELD;
        }
        if (action == MTA_FIRE) return JS_STEP_DONE;
        s->i = 0;
        STEP_GOTO(s->hdr.stage, MT_SETTLE, &s->fphase, &s->cphase, NULL);
        /* The driver re-enters at the stage this arm just set: an arm ENDS in a return, and one that runs on
           into the next has claimed a rest point the driver never saw. */
        return JS_STEP_YIELD;

    STEP_ARM(MT_SETTLE);
        DCHECK(JS_IsArray(promises), "a settling media element task was queued with no promise list — the "
                                     "take-pending-play-promises step hands one over even when it is empty");
        if (action != MTA_RESOLVE && JS_IsUndefined(s->reason)) {
            /* §4.8.11.8's two rejections name their exception: an "AbortError" for a load or a pause that
               interrupted the play, a "NotSupportedError" for the dedicated media source failure steps. */
            JS_ThrowDOMException(ctx, action == MTA_REJECT_ABORT ? "AbortError" : "NotSupportedError",
                                 action == MTA_REJECT_ABORT
                                     ? "the media element's play() was interrupted"
                                     : "the media resource is not suitable");
            s->reason = JS_GetException(ctx);
        }
        while (s->i < arr_len(ctx, promises)) {
            JSValue pair = JS_GetPropertyUint32(ctx, promises, s->i);
            JSValue fn = JS_GetPropertyUint32(ctx, pair, action == MTA_RESOLVE ? 0 : 1);
            JSValueConst arg = action == MTA_RESOLVE ? JS_UNDEFINED : (JSValueConst)s->reason;
            JSValue settled = JS_UNDEFINED;

            JS_FreeValue(ctx, pair);
            r = step_call_run(ctx, &s->cphase, STEP_CB(s->ccb), fn, JS_UNDEFINED, 1, &arg, cb_result, &settled,
                              out_cb, out_argc);
            JS_FreeValue(ctx, fn);
            if (r > 0) return r;
            cb_result = JS_UNDEFINED;
            JS_FreeValue(ctx, settled);
            if (r < 0) return JS_STEP_ABRUPT;
            s->i++;
        }
        return JS_STEP_DONE;
}

static const JSTrampStepDef media_task_def = {
    sizeof(MediaTask), media_task_step, NULL, 0,
    .visit = media_task_visit,
    .algorithm = "HTML §4.8.11 the media element event task source",
    .steps = MEDIA_TASK_STEPS
};

/* QUEUE ONE. `events` and `promises` are CONSUMED.
 *
 * IT IS A TASK, WHICH IS WHAT §4.8.11 SAYS IT IS: "to QUEUE A MEDIA ELEMENT TASK with a media element element
 * and a series of steps steps, QUEUE AN ELEMENT TASK on the media element's media element event task source
 * given element and steps". Every one of the twenty-odd sites in §4.8.11.5 through §4.8.11.16 that fires
 * `loadstart`, `progress`, `suspend`, `canplay`, `playing`, `pause`, `error` — and every one that settles a
 * pending play promise — reaches this function through that sentence, so the whole of this element's
 * observable event stream sat on the MICROTASK queue. That is a different position in HTML §8.1.7's event
 * loop and not a smaller one: a microtask runs inside the enqueuing flow's own checkpoint, so a `pause()` in a
 * script fired `pause` at the element BEFORE a `setTimeout(f,0)` that had already expired and before a message
 * already delivered — and, worse for the pair of them, before the `play()` promise settlement that §4.8.11.8
 * queues as a task of its own. §8.1.7's two queues exist so that cannot happen, and core/frame/navigable.c
 * states the same rule at §7.4.2.2's load. */
static void media_queue_task(JSContext *ctx, JSValueConst el, JSValue events, JSValue promises, int action)
{
    JSValueConst argv[4];
    JSValue fn, act;

    DCHECK(g_task_stepid >= 0, "a media element task was queued before §4.8.11 registered its machine");
    /* THE CALLEE IS MINTED IN THE ENQUEUING REALM: a C function runs in the realm that DEFINED it, and this
       one fires events at an element of THIS document. */
    fn = JS_NewCFunction2(ctx, NULL, "mediaElementTask", 4, JS_CFUNC_step, g_task_stepid);
    CHECK(!JS_IsException(fn), "§4.8.11: the media element task's callee could not be allocated");
    act = JS_NewInt32(ctx, action);
    argv[0] = el;
    argv[1] = events;
    argv[2] = promises;
    argv[3] = act;
    JS_EnqueueCallTask(ctx, fn, 4, argv);   /* §4.8.11: the media element event task source */
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, events);
    JS_FreeValue(ctx, promises);
    JS_FreeValue(ctx, act);
}

static JSValue media_event_list(JSContext *ctx, const char *const *names, int n)
{
    JSValue arr = JS_NewArray(ctx);
    int i;

    CHECK(!JS_IsException(arr), "§4.8.11: OOM building a queued task's event list");
    for (i = 0; i < n; i++)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewString(ctx, names[i]));
    return arr;
}

/* "Queue a media element task given the media element to fire an event named e at the media element" — the
   shape §4.8.11 states twenty times. */
static void media_queue_fire(JSContext *ctx, JSValueConst el, const char *name)
{
    media_queue_task(ctx, el, media_event_list(ctx, &name, 1), JS_UNDEFINED, MTA_FIRE);
}

/* §4.8.11.8's TAKE PENDING PLAY PROMISES: "copy the media element's list of pending play promises to promises,
   clear the list, and return promises". The copy is the ARRAY the state holds; clearing it means giving the
   state a fresh one, which is one property write the delta captures. */
static JSValue media_take_play_promises(JSContext *ctx, JSValueConst st)
{
    JSValue taken = JS_GetPropertyStr(ctx, st, "playPromises");
    JSValue fresh = JS_NewArray(ctx);

    CHECK(!JS_IsException(fresh), "§4.8.11.8: OOM clearing the list of pending play promises");
    JS_SetPropertyStr(ctx, (JSValue)st, "playPromises", fresh);
    return taken;
}

/* §4.8.11.8's NOTIFY ABOUT PLAYING: take the pending play promises, then queue a task that fires `playing` and
   resolves them. */
static void media_notify_playing(JSContext *ctx, JSValueConst el, JSValueConst st)
{
    static const char *const PLAYING[] = { "playing" };

    media_queue_task(ctx, el, media_event_list(ctx, PLAYING, 1), media_take_play_promises(ctx, st),
                     MTA_RESOLVE);
}

/* ---- §4.8.11.7's READY STATE TRANSITIONS ---------------------------------------------------------------------
 *
 * "When the ready state of a media element whose networkState is not NETWORK_EMPTY changes, the user agent
 * must follow the steps given below" — the event cascade a page's `onloadedmetadata`/`oncanplay` handlers hang
 * off, and the reason this is one function rather than a line at each caller. */
static bool media_eligible_for_autoplay(JSContext *ctx, JSValueConst el, JSValueConst st)
{
    char *autoplay;
    bool eligible;

    /* §4.8.11.7's list: the can autoplay flag, `paused`, an `autoplay` content attribute, and "the element's
       node document's ACTIVE SANDBOXING FLAG SET does not have the SANDBOXED AUTOMATIC FEATURES BROWSING
       CONTEXT FLAG set" — the same flag §6.6.7's autofocus reads, and a fact this build now carries
       (core/frame/sandboxing.h). The "autoplay" permission-policy feature is the one conjunct still absent,
       and it can only ever make an element LESS eligible than these four do.
       IT IS THE ELEMENT'S NODE DOCUMENT, not the running realm: the step names the element's document, and an
       element adopted into another document is governed by the document it is IN. A node document that is the
       active document of no navigable answers NULL, and §7.1.5's answer for such a Document is a POSITIVE one
       rather than a gap — "when the Document is created, its active sandboxing flag set must be empty", and
       only the navigation algorithm ever populates it, so a Document no navigation reached has an empty set. */
    lxb_dom_node_t *node = node_of(el);
    JSContext *dctx = node && node->owner_document
                    ? document_active_realm_of(lxb_dom_interface_node(node->owner_document)) : NULL;

    if (!st_bool(ctx, st, "canAutoplay") || !st_bool(ctx, st, "paused")) return false;
    if (dctx && (document_active_sandbox_flags(dctx) & SANDBOX_AUTOMATIC_FEATURES)) return false;
    autoplay = element_attr_get(ctx, el, "autoplay");
    eligible = autoplay != NULL;
    free(autoplay);
    return eligible;
}

static void media_set_ready_state(JSContext *ctx, JSValueConst el, JSValueConst st, int to)
{
    int from = st_int(ctx, st, "readyState");

    DCHECK(st_int(ctx, st, "networkState") != NETWORK_EMPTY,
           "§4.8.11.7's ready state transition ran on an element whose networkState is NETWORK_EMPTY — such an "
           "element is always in HAVE_NOTHING and has no transition to report");
    if (to == from) return;
    st_set_int(ctx, st, "readyState", to);

    if (from == HAVE_NOTHING && to == HAVE_METADATA) {
        media_queue_fire(ctx, el, "loadedmetadata");
        return;
    }
    if (from == HAVE_METADATA && to >= HAVE_CURRENT_DATA) {
        /* "If this is the first time this occurs for this media element since the load() algorithm was last
           invoked" — the latch the load algorithm resets. */
        if (!st_bool(ctx, st, "loadedData")) {
            st_set_bool(ctx, st, "loadedData", true);
            media_queue_fire(ctx, el, "loadeddata");
        }
    }
    if (from >= HAVE_FUTURE_DATA && to <= HAVE_CURRENT_DATA) {
        /* The element was potentially playing and no longer can be: §4.8.11.7's timeupdate-then-waiting. */
        if (!st_bool(ctx, st, "paused")) {
            media_queue_fire(ctx, el, "timeupdate");
            media_queue_fire(ctx, el, "waiting");
        }
        return;
    }
    if (from <= HAVE_CURRENT_DATA && to >= HAVE_FUTURE_DATA) {
        media_queue_fire(ctx, el, "canplay");
        if (!st_bool(ctx, st, "paused")) media_notify_playing(ctx, el, st);
    }
    if (to == HAVE_ENOUGH_DATA) {
        media_queue_fire(ctx, el, "canplaythrough");
        if (media_eligible_for_autoplay(ctx, el, st)) {
            /* §4.8.11.7's autoplay substeps, which this user agent runs: it has no user preference to honour
               and no viewport intersection to observe, and the alternative — never autoplaying — would delete
               every endpoint a page reaches from its `play` handler. */
            st_set_bool(ctx, st, "paused", false);
            st_set_bool(ctx, st, "showPoster", false);
            media_queue_fire(ctx, el, "play");
            media_notify_playing(ctx, el, st);
        }
    }
}

/* ---- §4.8.11.5's RESOURCE SELECTION ALGORITHM -----------------------------------------------------------------
 *
 * "This algorithm is always invoked as part of a task, but one of the first steps in the algorithm is to
 * return and continue running the remaining steps in parallel" — the step that returns is step 4, so steps 1-3
 * run in the INVOKING task (media_invoke_selection below) and this machine is what step 4 awaits. It is a step
 * machine because it holds the OUTCOME FORK that decides whether the resource is usable, and because the
 * standard's own "end the synchronous section" is a REST POINT at six separate steps of it.
 *
 * THE MODE AND THE OUTCOME ARE TWO ANSWERS AND THE PRODUCER USED TO GIVE ONE. media_select_resource returned a
 * `char *`, and NULL stood for BOTH "step 11's otherwise — there is no candidate at all" AND "attribute mode
 * whose src attribute is the empty string". The caller read every NULL as step 11 and went silently to
 * NETWORK_EMPTY, so `<video src="">` fired NOTHING — while §4.8.11.5's attribute mode step 1 says "if the src
 * attribute's value is the empty string, then end the synchronous section, and jump down to the failed with
 * attribute step below", and that step is BELOW step 12's NETWORK_LOADING and step 13's queued `loadstart`. A
 * page whose framework renders `<video src="">` on its first pass and listens for `error` heard neither event,
 * and the element it was watching claimed never to have started.
 *
 * THE SAME CONFLATION ATE THE CHILDREN MODE, TWICE. Step 10 chooses that mode from the PRESENCE of a `source`
 * element child, so `<video><source></video>` is in children mode and owes a `loadstart`, an `error` AT THE
 * SOURCE ELEMENT, and NETWORK_NO_SOURCE — and the old scan, which returned the first `source` child carrying a
 * non-empty `src` and NULL otherwise, produced step 11's silent NETWORK_EMPTY for all of it. It also skipped
 * over an unusable candidate rather than FAILING at it, so the `error` the standard fires at each rejected
 * `source` (the event the fallback idiom in §4.8.12's own example listens for) never fired at all. Object mode
 * was the third state the one string carried: a malloc'd "" meaning "currentSrc is the empty string, go and
 * fetch the provider object".
 *
 * So the producer answers the MODE, the candidate rides with it, and the caller cannot conflate two outcomes
 * because they are no longer one value. The three failure LABELS the standard writes are three stages, and
 * which one a failed load reaches is the MODE's to say — failed with media provider and failed with attribute
 * run the dedicated media source failure steps at the media element, and failed with elements does something
 * else entirely (it fires at the candidate and never touches `error` or the play promises). */
enum { MEDIA_MODE_NONE = 0, MEDIA_MODE_OBJECT, MEDIA_MODE_ATTRIBUTE, MEDIA_MODE_CHILDREN };

typedef struct {
    JSStepHdr hdr;
    JSValue   over;   /* the media resource the fork is asked about (owned across the ask) */
    /* §4.8.11.5's `candidate`, which is also the node BEFORE its `pointer`: the find next candidate step walks
       the sibling chain forward from it. A raw lexbor node, exactly as core/dom/element.c's tree-steps buffer
       holds two, because the tree a parked flow names is that flow's own through the DOM COW delta — there is
       no reference to take and the fork's byte copy is the whole contract. */
    lxb_dom_node_t *candidate;
    uint8_t   mode;
} MediaSelect;

/* THE STAGES ARE THE STANDARD'S OWN SYNCHRONOUS SECTIONS, AND THAT IS WHAT DECIDES WHERE THEY MAY DIVIDE.
   A stage boundary is a rest point, and a rest point here is not merely an engine convenience: this machine's
   siblings on the frontier include ITS OWN queued media element tasks, so a rest between two ⌛ steps runs the
   page's `loadstart` listener inside a span the standard declares atomic — and `video.removeAttribute('src')`
   from that listener would land between step 9's mode selection and step 14's read of the same attribute. So
   the span from step 5 to each mode's "end the synchronous section" is ONE stage, not because the page happens
   to be quiet across it (which JSTrampStepDef::steps rightly calls a cap wearing a justification) but because
   the standard forbids the page from running there at all. Every boundary below is an "end the synchronous
   section", an "await a stable state", or the fetch. */
#define MEDIA_SELECT_STAGES(X) \
    X(MS_SELECT,     "HTML §4.8.11.5 steps 5-13 and step 14's per-mode synchronous steps, to the first \"end " \
                     "the synchronous section\" (the mode, the candidate, step 11's otherwise, " \
                     "NETWORK_LOADING, the queued loadstart, and currentSrc)") \
    X(MS_FETCH,      "HTML §4.8.11.5 step 14's resource fetch algorithm and media data processing steps " \
                     "(whether the resource is usable)") \
    X(MS_FAILED_SRC, "HTML §4.8.11.5 step 14's failed with media provider / failed with attribute step") \
    X(MS_FAILED_EL,  "HTML §4.8.11.5 step 14's failed with elements step") \
    X(MS_RESELECT,   "HTML §4.8.11.5 step 14's children mode second synchronous section (forget the " \
                     "media-resource-specific tracks, find next candidate, the search loop, and the waiting " \
                     "step)")
enum { MEDIA_SELECT_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const MEDIA_SELECT_STEPS[] = { MEDIA_SELECT_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* THE TITLE BESIDE THE NUMBER, and the phrase that was standing in for it kept as what it is. §4.8.11.5 is
   titled "Loading the media resource"; "the media data processing steps list" is a phrase INSIDE it, and a
   citation of the form "§N <phrase>" reads as a title claim, which is the one form §Browser half says must
   survive an edition the number does not. This string is also the decision-site label every `forkedAt`
   histogram prints, so it is read far more often than the code around it. */
#define MEDIA_FETCH_OP "HTML §4.8.11.5 Loading the media resource — the media data processing steps list "\
                       "(is the resource usable)"

static void media_select_visit(JSContext *ctx, void *stp, JSStepVisit *v)
{
    MediaSelect *s = stp;

    v->val(ctx, &s->over);
}

/* "Take pending play promises and queue a media element task given the media element to run the DEDICATED
   MEDIA SOURCE FAILURE STEPS with the result" — the whole of what the two `src`-side failure labels do. The
   steps themselves run inside the task (media_task_step's MTA_FAILURE arm), which is where the standard puts
   them and which is what makes `v.error` null for every turn until it executes. */
static void media_failure(JSContext *ctx, JSValueConst el, JSValueConst st)
{
    static const char *const ERROR_EV[] = { "error" };

    media_queue_task(ctx, el, media_event_list(ctx, ERROR_EV, 1), media_take_play_promises(ctx, st),
                     MTA_FAILURE);
}

/* §4.8.11.5's STEPS 6-11 — the MODE, and the children mode's first candidate with it. Two answers, because
   they are two questions: the mode decides which of step 14's three branches runs AND which failure label a
   failed load reaches, and neither is recoverable from a string. Step 11's "otherwise" is its own value
   (MEDIA_MODE_NONE) rather than an absent one, which is the whole of the defect this replaces. */
static int media_select_mode(JSContext *ctx, JSValueConst el, JSValueConst st, lxb_dom_node_t **candidate)
{
    JSValue obj = JS_GetPropertyStr(ctx, st, "srcObject");
    bool has_object = !JS_IsNull(obj) && !JS_IsUndefined(obj);
    char *src;

    JS_FreeValue(ctx, obj);
    *candidate = NULL;                                                          /* step 7 */
    if (has_object) return MEDIA_MODE_OBJECT;                                   /* step 8 */
    /* Step 9: "otherwise, if the media element has a src attribute, then set mode to attribute" — PRESENCE,
       never the value. `<video src="">` HAS one, and the empty string is answered five steps later, after the
       loadstart, by the failed with attribute step. */
    src = element_attr_get(ctx, el, "src");
    if (src) { free(src); return MEDIA_MODE_ATTRIBUTE; }
    /* Step 10: "otherwise, if the media element has a source element child, then set mode to children and set
       candidate to the first source element child in tree order" — again PRESENCE. Whether that child names a
       resource is step 14's question and it is answered by FAILING at the candidate, not by looking past it. */
    *candidate = media_first_source(lxb_dom_interface_node(element_of_value(el)));
    if (*candidate) return MEDIA_MODE_CHILDREN;
    return MEDIA_MODE_NONE;                                                     /* step 11 */
}

/* §4.8.11.5 step 14's CHILDREN MODE steps 2-7, "Process candidate" — the span the find next candidate step
   JUMPS BACK INTO, and therefore a function rather than an arm: the two synchronous sections that reach it are
   two stages, and a shared span written once per stage is two spans one edit apart from disagreeing. It reads
   and writes the machine's own state and returns the STAGE TO REST AT — MS_FETCH when the candidate names a
   resource (s->over is the question the fork will be asked), MS_FAILED_EL when it does not. */
static int media_process_candidate(JSContext *ctx, MediaSelect *s, JSValueConst st)
{
    JSValue cand;
    char *src, *attr;
    bool usable;

    DCHECK(s->mode == MEDIA_MODE_CHILDREN, "§4.8.11.5's process candidate step ran in another mode");
    DCHECK(media_source_is(s->candidate),
           "§4.8.11.5's process candidate step was entered with something that is not a `source` element — "
           "step 10 and the find next candidate step are the only two writers of the candidate");
    cand = element_wrap(ctx, lxb_dom_interface_element(s->candidate));
    /* Step 2, "Process candidate": "if candidate does not have a src attribute, or if its src attribute's
       value is the empty string, then end the synchronous section, and jump down to the failed with elements
       step below". */
    src = element_attr_get(ctx, cand, "src");
    usable = src != NULL && src[0] != 0;
    /* Step 3: "If candidate has a media attribute whose value does not MATCH THE ENVIRONMENT, then end the
       synchronous section, and jump down to the failed with elements step below."
       "Matches the environment" is HTML's own definition, HTML §2.3.10 "Media queries": a string matches it "if
       it is the empty string, a string consisting of only ASCII whitespace, or is a media query list that
       matches the user's environment according to the definitions given in Media Queries". All three arms are
       core/css/media_query.h's `media_query_parse` — its lexer skips whitespace, so an empty and a
       whitespace-only attribute both parse to a list of NO queries, which MQ4 §3.1 makes a list that matches
       everything. There is no arm here for those cases because the grammar already has them.
       THIS IS THE SAME QUESTION `<source media>` UNDER `<picture>` ASKS, AND IT IS ANSWERED THE SAME WAY.
       core/html/image_source_set.c runs §4.8.4.3's update-the-source-set step 5.6 — the identical attribute on
       the identical element — through `media_query_parse` + `media_query_matches_now`, and so does the cascade
       for an `@media` rule. One environment, one predicate, one answer: a second spelling here could resolve
       the same query differently from the one the page's own `matchMedia` and stylesheet resolved, which is one
       fact answered from two places.
       THE `_now` READ IS THE ENGINE-SIDE POLICY AND NOT A COLLAPSE OF THE CONCOLIC. core/css/media_query.h
       states the split: `matches` stays concolic where a PAGE reads it, so a page that branches on the viewport
       still explores both worlds, while C — which cannot fork — takes the arm THIS FLOW has already committed
       to (solver/decide.h) and falls back to the modelled environment where it has committed to neither. A
       DFAIL here used to demand a fork at this one site instead; that would make `<video><source media>` the
       only environment reader in the engine with its own forking policy, disagreeing with the cascade,
       update-the-rendering and `<picture>` about the same predicate — and it named a file (media_query_list.c,
       whose problem is the CSSOM object) as exporting no evaluator, when the evaluator is the LANGUAGE's and
       has been exported by media_query.h next door all along. */
    if (usable) {
        attr = element_attr_get(ctx, cand, "media");
        if (attr) {
            MediaQuerySet *set = media_query_parse(attr);

            DCHECK(set != NULL,
                   "media_query_parse answered NULL for a `source` element's `media` attribute. MQ4 §3.1's "
                   "forward-compatible rule replaces a query that does not match the grammar with `not all`, "
                   "so a LIST parse has no failure to report and media_query.h states it is never null for "
                   "non-null input — a null here means that entry started reporting parse errors and every "
                   "caller of it, this one included, is reading a pointer the grammar says cannot exist");
            usable = media_query_matches_now(ctx, set);
            media_query_free(set);
            free(attr);
        }
    }
    /* Steps 4-5's urlRecord: the resolution is the reflection's, which is the raw attribute here.
       Step 6: "if candidate has a type attribute whose value, when parsed as a MIME type …, represents a type
       that the user agent knows it cannot render" — §4.8.11.3's own question, which the modelled device
       answers, and the empty string is what "knows it cannot render" means there. */
    if (usable) {
        attr = element_attr_get(ctx, cand, "type");
        if (attr && device_can_play(attr)[0] == 0) usable = false;
        free(attr);
    }
    JS_FreeValue(ctx, cand);
    if (!usable) {
        free(src);
        return MS_FAILED_EL;
    }
    /* Step 7: "set the currentSrc attribute to the result of applying the URL serializer to urlRecord". */
    JS_SetPropertyStr(ctx, (JSValue)st, "currentSrc", JS_NewString(ctx, src));
    free(src);
    s->over = media_resource_value(ctx, st);
    return MS_FETCH;
}

static int media_select_step(JSContext *ctx, void *stp, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    MediaSelect *s = stp;
    JSValueConst el = step_arg(&s->hdr, 0);
    JSValue st = media_state(ctx, el);
    int arm = 0, rc;

    JS_FreeValue(ctx, cb_result);
    (void)out_cb; (void)out_argc;

    STEP_DISPATCH(MEDIA_SELECT_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(MS_SELECT); {
        int next;

        s->over = JS_UNDEFINED;
        s->candidate = NULL;
        /* Step 5: "if the media element's blocked-on-parser flag is false, then populate the list of pending
           text tracks." */
        realm_awaits(ctx, "HTMLMediaElement.prototype.textTracks",
                     "HTML §4.8.11.5 step 5 must POPULATE THE LIST OF PENDING TEXT TRACKS when the media "
                     "element's blocked-on-parser flag is false — write that step here");
        s->mode = (uint8_t)media_select_mode(ctx, el, st, &s->candidate);
        if (s->mode == MEDIA_MODE_NONE) {
            /* Step 11: NETWORK_EMPTY, and the algorithm ENDS with no `loadstart`. That is the ONLY outcome
               that ends here, and telling it apart from a mode whose candidate cannot be USED is the whole of
               what this stage's producer had to start answering. */
            st_set_int(ctx, st, "networkState", NETWORK_EMPTY);
            JS_FreeValue(ctx, st);
            return JS_STEP_DONE;
        }
        /* Steps 12-13, which EVERY mode reaches — that is what the standard's numbering says, and what the old
           producer's single NULL denied to `<video src="">` and to a `<video>` whose sources are unusable. */
        st_set_int(ctx, st, "networkState", NETWORK_LOADING);
        media_queue_fire(ctx, el, "loadstart");
        if (s->mode == MEDIA_MODE_CHILDREN) {
            next = media_process_candidate(ctx, s, st);
        } else if (s->mode == MEDIA_MODE_OBJECT) {
            /* Object mode step 1: "set the currentSrc attribute to the empty string" — an assigned media
               provider object has no URL, and the empty string is what the page reads back. */
            JS_SetPropertyStr(ctx, st, "currentSrc", JS_NewString(ctx, ""));
            s->over = media_resource_value(ctx, st);
            next = MS_FETCH;
        } else {
            char *src = element_attr_get(ctx, el, "src");

            /* THE ATTRIBUTE IS READ HERE AND NOT AT STEP 9, and nothing rests in between, which is what makes
               this assert an invariant rather than a race: step 9 picked attribute mode from the attribute's
               PRESENCE, and both reads are inside the one synchronous section. */
            DCHECK(src != NULL,
                   "§4.8.11.5 is in attribute mode on an element with no `src` attribute — step 9 chooses "
                   "that mode from the attribute's presence, and no step between the two ends the "
                   "synchronous section, so nothing can have removed it");
            if (!src[0]) {
                /* Attribute mode step 1: "if the src attribute's value is the empty string, then end the
                   synchronous section, and jump down to the failed with attribute step below" — BELOW, which
                   is after the `loadstart` this stage has already queued. */
                next = MS_FAILED_SRC;
            } else {
                /* Attribute mode steps 2-3: urlRecord, and currentSrc as its serialization. The resolution is
                   the reflection's, which is the raw attribute in this engine. */
                JS_SetPropertyStr(ctx, st, "currentSrc", JS_NewString(ctx, src));
                s->over = media_resource_value(ctx, st);
                next = MS_FETCH;
            }
            free(src);
        }
        STEP_GOTO(s->hdr.stage, next, NULL);
        JS_FreeValue(ctx, st);
        return JS_STEP_YIELD;   /* "End the synchronous section" — an arm ends in a return */
    }

    STEP_ARM(MS_FETCH);
        /* THE ONE UNKNOWABLE: whether the bytes at this address are a resource this user agent can render.
           Outcome 0 is the ordinary completion — the resource IS usable — because that is the arm a run with
           no forking policy takes and the one an @S candidate re-fire must not be diverted from. */
        if (concolic_is(s->over)) {
            rc = step_fork_run(ctx, &s->hdr, s->over, MEDIA_FETCH_OP, 2, JS_OUTCOME_REAL_UNSTATED, &arm);
            if (rc) { JS_FreeValue(ctx, st); return rc; }
        }
        JS_FreeValue(ctx, s->over);
        s->over = JS_UNDEFINED;
        if (arm != 0) {
            /* "If that algorithm returns without aborting this one, then the load failed" — and WHICH failure
               step that is belongs to the MODE, which is the second reason the mode had to survive step 11. */
            STEP_GOTO(s->hdr.stage, s->mode == MEDIA_MODE_CHILDREN ? MS_FAILED_EL : MS_FAILED_SRC, NULL);
            JS_FreeValue(ctx, st);
            return JS_STEP_YIELD;
        }
        /* The media data processing steps' "once enough of the media data has been fetched to determine the
           duration of the media resource": establish the timeline, set both positions to the earliest possible
           position, update the duration, and walk the ready state up to HAVE_ENOUGH_DATA — which is what a
           device with the whole resource available reports. Each transition queues its own events. */
        realm_awaits(ctx, "HTMLMediaElement.prototype.audioTracks",
                     "HTML §4.8.11.5's media data processing steps must create an AudioTrack and a VideoTrack "
                     "for the tracks the resource is found to have, update the element's AudioTrackList and "
                     "VideoTrackList, and fire `addtrack` at each — write those steps here");
        st_set_num(ctx, st, "current", 0);
        st_set_num(ctx, st, "official", 0);
        st_set_num(ctx, st, "duration", DEVICE_DURATION);
        st_set_int(ctx, st, "networkState", NETWORK_IDLE);
        media_queue_fire(ctx, el, "durationchange");
        media_set_ready_state(ctx, el, st, HAVE_METADATA);
        media_set_ready_state(ctx, el, st, HAVE_ENOUGH_DATA);
        media_queue_fire(ctx, el, "suspend");
        JS_FreeValue(ctx, st);
        return JS_STEP_DONE;

    STEP_ARM(MS_FAILED_SRC);
        DCHECK(s->mode == MEDIA_MODE_OBJECT || s->mode == MEDIA_MODE_ATTRIBUTE,
               "§4.8.11.5's failed with attribute step was reached in children mode — children mode fails at "
               "its own label, which fires at the `source` element and never touches `error`");
        /* "Failed with media provider" / "Failed with attribute": "Take pending play promises and queue a
           media element task given the media element to run the dedicated media source failure steps with the
           result." Then "wait for the task queued by the previous step to have executed" and "return. The
           element won't attempt to load another resource until this algorithm is triggered again" — this
           machine has nothing after the wait, so the wait and the return are the same instruction. */
        media_failure(ctx, el, st);
        JS_FreeValue(ctx, st);
        return JS_STEP_DONE;

    STEP_ARM(MS_FAILED_EL); {
        JSValue cand;

        DCHECK(media_source_is(s->candidate),
               "§4.8.11.5's failed with elements step has no `source` element to fire at");
        /* "Failed with elements: Queue a media element task given the media element to fire an event named
           error AT CANDIDATE." At the `source` element, and the DEDICATED MEDIA SOURCE FAILURE STEPS DO NOT
           RUN: children mode never sets the media element's `error`, never rejects its play promises, and
           never ends the algorithm here — it goes looking for the next candidate. Conflating this label with
           the other two would report a `<video>` with one unusable `<source>` as MEDIA_ERR_SRC_NOT_SUPPORTED,
           and a page's `video.onerror` fallback would fire where a real browser fires `source.onerror`. */
        cand = element_wrap(ctx, lxb_dom_interface_element(s->candidate));
        media_queue_fire(ctx, cand, "error");
        JS_FreeValue(ctx, cand);
        STEP_GOTO(s->hdr.stage, MS_RESELECT, NULL);
        JS_FreeValue(ctx, st);
        return JS_STEP_YIELD;   /* "Await a stable state." */
    }

    STEP_ARM(MS_RESELECT); {
        lxb_dom_node_t *n;

        realm_awaits(ctx, "HTMLMediaElement.prototype.textTracks",
                     "HTML §4.8.11.5 step 14's children mode must FORGET the media element's "
                     "media-resource-specific tracks before it looks for the next candidate — write that "
                     "step here");
        /* "⌛ Find next candidate: let candidate be null. ⌛ Search loop: if the node after pointer is the end
           of the list, then jump to the waiting step below. ⌛ If the node after pointer is a source element,
           let candidate be that element. ⌛ Advance pointer… ⌛ If candidate is null, jump back to the search
           loop step. Otherwise, jump back to the process candidate step." Pointer is the position immediately
           after the node this run last processed, so the search loop IS the sibling chain from it, and the
           jump back into process candidate is a jump back into the FIRST synchronous section — which is why
           that span is a function both stages call and not an arm one of them owns. */
        DCHECK(s->candidate->parent == lxb_dom_interface_node(element_of_value(el)),
               "§4.8.11.5's pointer is no longer between two children of the media element. The standard's "
               "pointer update rules — \"if the node before pointer is removed, let pointer be the point "
               "between the node after pointer and the node before the node after pointer\" — are not built, "
               "and the `error` the failed with elements step queued at the candidate has a listener that can "
               "remove exactly that node. Build the pointer as a position the DOM mutation chokepoint "
               "maintains, the way core/dom/element.c's tree-steps cursor is maintained");
        for (n = s->candidate->next; n; n = n->next)
            if (media_source_is(n)) break;
        if (n) {
            int next;

            s->candidate = n;
            next = media_process_candidate(ctx, s, st);
            STEP_GOTO(s->hdr.stage, next, NULL);
            JS_FreeValue(ctx, st);
            return JS_STEP_YIELD;
        }
        /* "⌛ Waiting: set the element's networkState attribute to the NETWORK_NO_SOURCE value. ⌛ Set the
           element's show poster flag to true." The algorithm then waits "until the node after pointer is a
           node other than the end of the list", and the standard says out loud that this "might wait
           forever": the ONLY thing that can end the wait is a `source` inserted into this element, which is
           where §4.8.12's source element insertion steps stand. So the wait is RECORDED where its waker can
           read it and this flow ENDS rather than spinning — a JS_STEP_YIELD loop over a condition no code in
           this flow can change is a monopolizer, not a park. media_element_source_inserted below crashes by
           name on the insertion that would have to resume it. */
        st_set_int(ctx, st, "networkState", NETWORK_NO_SOURCE);
        st_set_bool(ctx, st, "showPoster", true);
        st_set_bool(ctx, st, "sourceWait", true);
        JS_FreeValue(ctx, st);
        return JS_STEP_DONE;
    }
}

static const JSTrampStepDef media_select_def = {
    sizeof(MediaSelect), media_select_step, NULL, 0,
    .visit = media_select_visit,
    .algorithm = "HTML §4.8.11.5 the resource selection algorithm",
    .steps = MEDIA_SELECT_STEPS
};

/* "Invoke the media element's resource selection algorithm."
 *
 * STEPS 1-3 RUN HERE, IN THE INVOKING TASK, and the job is what step 4's "await a stable state" awaits. That
 * is what the algorithm's own first paragraph says — "one of the first steps in the algorithm is to return and
 * continue running the remaining steps in parallel", and the step that returns is step 4, not step 1.
 *
 * AND THE PLACEMENT IS LOAD-BEARING, NOT A TIDY-UP. §4.8.12's source element insertion steps invoke this only
 * "if parent is a media element that has no src attribute and whose networkState has the value NETWORK_EMPTY",
 * so `<video><source src=a><source src=b></video>` must invoke it from the FIRST child's insertion and not
 * from the second. With step 1 inside the enqueued job the network state was still NETWORK_EMPTY when the
 * second child arrived, and one element would have run two resource selection algorithms over one tree —
 * §scheduler's double-load, arriving for the same reason it always does: a step that became a work item was
 * read at the wrong time.
 *
 * AND THE JOB IS A MICROTASK, WHICH IS THE ONE THING ABOUT THIS FUNCTION THAT MUST NOT BE "CORRECTED". Step 4
 * is "AWAIT A STABLE STATE, allowing the task that invoked this algorithm to continue", and HTML §8.1.7.3
 * defines that in one sentence: "when an algorithm running in parallel is to AWAIT A STABLE STATE, the user
 * agent must QUEUE A MICROTASK that runs the following steps". It is not §4.8.11's media element task source —
 * that source is media_queue_task above, and it IS a task. The two are one algorithm apart and they are
 * different queues, so making this one a task would run the synchronous section behind every task already
 * standing (an expired timer, a delivered message) instead of at the next checkpoint, and `loadstart` would
 * reach the page after them rather than before. */
static void media_invoke_selection(JSContext *ctx, JSValueConst el)
{
    JSValue fn, st = media_state(ctx, el);

    DCHECK(g_select_stepid >= 0, "§4.8.11.5 was invoked before its machine was registered");
    /* §4.8.11.5'S "ALWAYS INVOKED AS PART OF A TASK" IS NOW TRUE HERE BY CONSTRUCTION, AND THE ASSERTION THAT
       SAID OTHERWISE IS GONE WITH THE TWO THINGS IT NAMED. It stood here because step 4's await-a-stable-state
       is, per §8.1.7.3, a microtask the USER AGENT queues from an algorithm running in parallel — and
       §4.8.11.2's "if a media element is created with a src attribute, the user agent must IMMEDIATELY invoke
       the resource selection algorithm" brought every `<video src>` in a page's own markup through
       media_element_parsed's walk, inside qjs_init, before the frontier exists. quickjs's platform enqueue
       routed an ownerless callback to the baseline list only when it was a TASK, so the microtask fell onto the
       global job list this engine never drains and qjs_begin aborted on it.
       Both halves this comment asked for have landed: the routing condition no longer asks `is_task`, and the
       adoption loop no longer asserts the list holds tasks only — it hands the hook the entry's own `is_task`,
       so a microtask is adopted onto the first flow as the microtask it was queued as. This line is deleted
       rather than relaxed: the invariant it protected is now established by the mechanism instead of by a
       caller remembering, which is the only reason an assertion may go. */
    st_set_int(ctx, st, "networkState", NETWORK_NO_SOURCE);   /* step 1 */
    st_set_bool(ctx, st, "showPoster", true);                 /* step 2 */
    /* A new run of the algorithm is not the run that parked at step 14's children mode waiting step, so the
       pointer that run was waiting on is not this one's to be woken at. */
    st_set_bool(ctx, st, "sourceWait", false);
    JS_FreeValue(ctx, st);
    fn = JS_NewCFunction2(ctx, NULL, "mediaResourceSelection", 1, JS_CFUNC_step, g_select_stepid);
    CHECK(!JS_IsException(fn), "§4.8.11.5: the resource selection task's callee could not be allocated");
    JS_EnqueueCallJob(ctx, fn, 1, &el);
    JS_FreeValue(ctx, fn);
}

/* ---- §4.8.11.5's MEDIA ELEMENT LOAD ALGORITHM ------------------------------------------------------------- */
static void media_load_algorithm(JSContext *ctx, JSValueConst el, JSValueConst st)
{
    int network = st_int(ctx, st, "networkState");

    /* Step 5: an abort event when the element was already fetching. */
    if (network == NETWORK_LOADING || network == NETWORK_IDLE)
        media_queue_fire(ctx, el, "abort");
    if (network != NETWORK_EMPTY) {
        media_queue_fire(ctx, el, "emptied");
        realm_awaits(ctx, "HTMLMediaElement.prototype.textTracks",
                     "HTML §4.8.11.5 step 6.4 must FORGET the media element's media-resource-specific tracks — "
                     "remove them from the list of text tracks and empty the AudioTrackList and "
                     "VideoTrackList — write that step here");
        if (st_int(ctx, st, "readyState") != HAVE_NOTHING)
            st_set_int(ctx, st, "readyState", HAVE_NOTHING);
        if (!st_bool(ctx, st, "paused")) {
            static const char *const NONE[] = { "" };

            st_set_bool(ctx, st, "paused", true);
            /* "Take pending play promises and reject pending play promises with the result and an AbortError"
               — a task with no fires at all, which is what an empty event list is. */
            media_queue_task(ctx, el, media_event_list(ctx, NONE, 0), media_take_play_promises(ctx, st),
                             MTA_REJECT_ABORT);
        }
        if (st_bool(ctx, st, "seeking")) st_set_bool(ctx, st, "seeking", false);
        if (st_num(ctx, st, "current") != 0 || st_num(ctx, st, "official") != 0) {
            st_set_num(ctx, st, "current", 0);
            st_set_num(ctx, st, "official", 0);
            media_queue_fire(ctx, el, "timeupdate");
        }
        st_set_num(ctx, st, "timelineOffset", NAN);
        /* "Update the duration attribute to NaN. The user agent will not fire a durationchange event for this
           particular change of the duration." */
        st_set_num(ctx, st, "duration", NAN);
    }
    st_set_num(ctx, st, "rate", st_num(ctx, st, "defaultRate"));
    JS_SetPropertyStr(ctx, (JSValue)st, "error", JS_NULL);
    st_set_bool(ctx, st, "canAutoplay", true);
    st_set_bool(ctx, st, "loadedData", false);
    media_invoke_selection(ctx, el);
}

/* ---- §4.8.11.2's TWO TRIGGERS, which are two ALGORITHMS at two MOMENTS ---------------------------------------
 *
 * "If a media element is created with a src attribute, the user agent must immediately invoke the media
 * element's resource selection algorithm. If a src attribute of a media element is set or changed, the user
 * agent must invoke the media element's media element load algorithm. (Removing the src attribute does not do
 * this, even if there are source elements present.)"
 *
 * Those are two sentences and they are not interchangeable: the first runs the RESOURCE SELECTION algorithm at
 * CREATION and the second runs the MEDIA ELEMENT LOAD algorithm on a CHANGE — the load algorithm aborts a
 * running selection, fires `abort` and `emptied`, rejects the pending play promises and only then invokes
 * selection, all of which is exactly wrong for an element that was created a moment ago and has none of it.
 * Every SCRIPTED entry to this file was already built and none of these two were, so a media element that
 * script never touched never loaded anything: `<video src=x>` with an `onerror` listener sat at NETWORK_EMPTY
 * forever, and the outcome fork §4.8.11.5 holds — the whole reason this component exists to the solver — was
 * reachable only from a bundle that assigned `.src` itself.
 *
 * INSERTION IS NOT THE MOMENT, AND THAT IS THE STANDARD'S OWN DISTINCTION RATHER THAN A READING OF IT. DOM
 * §4.2.3 defines INSERTION STEPS as a named hook ("Specifications may define insertion steps for all or some
 * nodes"), HTML uses it for exactly this family one element over — "The source HTML element insertion steps,
 * given insertedNode, are: … if parent is a media element that has no src attribute and whose networkState has
 * the value NETWORK_EMPTY, then invoke that media element's resource selection algorithm" — and for the media
 * element itself it says CREATED instead. The difference is observable in both directions: a `<video src=x>`
 * built by `createElement` + `setAttribute` and never inserted still loads, and a `<video>` created with no
 * `src` that is later moved into a document does not. Hooking insertion would get both backwards.
 *
 * WHICH LEAVES ONE QUESTION FOR THIS ENGINE: WHERE AN ELEMENT IS CREATED WITH ITS ATTRIBUTES. Two places, and
 * only two. A SCRIPT-created element is created bare and gains `src` through §4.9's attribute change steps,
 * which is the chokepoint below. A PARSER-created one is created with its attributes inside lexbor — HTML
 * §13.2.6.1's "create an element for a token" ends by appending "each attribute in the given token to element"
 * — and lexbor's tree builder has no per-token hook, so this engine takes the parse BOUNDARY, which is the
 * seam dom_attr_normalize_parsed, html_script_parsed and declarative_shadow_parsed already stand at and
 * is unobservable for their reason: no page code runs between the start tag that created the element and the
 * end of the parse that produced it. */

/* §4.9's ATTRIBUTE CHANGE STEPS for `src`, registered on element.c's element_attr_changed beside
   custom_elements_attribute_changed and slot_attribute_changed — the chokepoint every attribute write reaches,
   which is why the invocation is HERE and not in the reflection's setter: `v.src = url`, `v.setAttribute('src',
   url)` and `v.attributes.src.value = url` are one write of one attribute, and a setter-side call answers for
   the first spelling only.
   `val` IS THE OPERATION'S INPUT AND NOTHING ELSE READS IT: the parenthetical is a rule about the CHANGE, so
   presence of a new value is what decides whether an algorithm runs at all, while the address that algorithm
   loads is read at its own step 6 off the element, which is where §4.8.11.5 puts that read. */
void media_element_attr_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local,
                                const char *val)
{
    JSValue wrap, st;

    if (!media_is_node(lxb_dom_interface_node(el))) return;
    if (ns != NULL || !local || strcmp(local, "src")) return;   /* the `src` CONTENT attribute, null namespace */
    if (!val) return;                    /* "(Removing the src attribute does not do this …)" */
    DCHECK(g_ready, "a media element's `src` changed before §4.8.11 was declared — the load algorithm it must "
                    "invoke lives on a state record this agent has no key for");
    wrap = node_wrap(ctx, lxb_dom_interface_node(el));
    st = media_state(ctx, wrap);
    media_load_algorithm(ctx, wrap, st);
    JS_FreeValue(ctx, st);
    JS_FreeValue(ctx, wrap);
}

/* §4.8.11.2's "CREATED WITH A SRC ATTRIBUTE", for the elements a PARSE created — the document's, from
 * document_install, and every fragment's, from element.c's markup machine.
 *
 * WHAT IT COSTS PER ELEMENT, WHICH IS THE REASON IT IS A WALK AND NOT A CREATION HOOK. This engine mints an
 * element wrapper LAZILY, and that is load-bearing rather than incidental: a hook that handed every created
 * element to C would put a wrapper in node.c's identity map for every node of every page, which is a per-node
 * heap cost on documents whose DOM already outweighs their heap by an order of magnitude and therefore a
 * paging cost on the cold tier. So the question is asked of the NODE: media_is_node is three integer compares
 * against interned ids, and the `src` test is lexbor's own attribute lookup on the element — no allocation, no
 * wrapper, and nothing retained. The walk is shadow-including, which adds one node_wrap_peek (a hash probe
 * that never mints) per element to ask whether it hosts a shadow root; a `<video src>` written inside a
 * `<template shadowrootmode>` is created by the parser exactly like any other, and this runs after
 * declarative_shadow_parsed has moved it into the shadow tree where the plain child walk cannot see it.
 * ONLY a media element that actually carries `src` allocates: one wrapper, one state record, one job.
 *
 * AND WHAT IT ENQUEUES IS A JOB, NOT A DRIVE. media_invoke_selection is "invoke the media element's resource
 * selection algorithm" as the task §4.8.11.5's own first paragraph says it is ("This algorithm is always
 * invoked as part of a task"), so a parsed document with fifty `<video src>` elements adds fifty work items to
 * the one frontier and runs none of them here — each is a flow that forks at the media data processing steps
 * on its own turn, and the walk that seeded them returns in O(nodes) with no page code having run.
 *
 * PRESENCE, NOT VALUE, is the condition: `<video src="">` IS created with a src attribute, and §4.8.11.5's
 * attribute mode is where the empty string is answered — with a `loadstart` and an `error`, which is exactly
 * what a producer that could not tell an empty `src` from no candidate at all used to deny it.
 *
 * AND IT CARRIES §4.8.12's SOURCE ELEMENT INSERTION STEPS FOR THE SAME PARSE, because those are what start a
 * `<video>` that has no `src` and only `<source>` children — the shape every page that ships more than one
 * codec is written in. A parser-inserted `<source>` reaches no mutation chokepoint, so the insertion steps
 * that would have invoked the algorithm from the FIRST such child never ran and that whole family of elements
 * sat at NETWORK_EMPTY exactly as `<video src>` did before 09849674. Asking it here rather than per `<source>`
 * is the same statement in one place: media_invoke_selection's step 1 leaves the element at NETWORK_NO_SOURCE,
 * which is precisely the condition under which those steps decline to invoke it a second time. */
void media_element_parsed(JSContext *ctx, lxb_dom_node_t *root)
{
    lxb_dom_node_t *n;

    DCHECK(root != NULL, "§4.8.11.2's created-with-a-src walk was given no tree to walk");
    DCHECK(g_ready, "a parsed tree reached §4.8.11.2 before media_element_declare ran");
    for (n = root; n; n = shadow_root_next_in_shadow_including(ctx, n, root)) {
        JSValue wrap;

        if (!media_is_node(n)) continue;
        if (!lxb_dom_element_has_attribute(lxb_dom_interface_element(n), (const lxb_char_t *)"src", 3) &&
            !media_first_source(n))
            continue;
        wrap = node_wrap(ctx, n);
        media_invoke_selection(ctx, wrap);
        JS_FreeValue(ctx, wrap);
    }
}

/* ---- §4.8.12's SOURCE ELEMENT INSERTION STEPS ----------------------------------------------------------------
 *
 * "The source HTML element insertion steps, given insertedNode, are: 1. Let parent be insertedNode's parent.
 * 2. If parent is a media element that has no src attribute and whose networkState has the value
 * NETWORK_EMPTY, then invoke that media element's resource selection algorithm."
 *
 * IT IS NOT A node_add_tree_hook, AND THAT IS A STATEMENT ABOUT WHAT THAT LIST IS. Its members are the DOM's
 * own step families — §5.5's live-range pre-remove, §6.1's NodeIterator pre-remove, §4.2.2's slot steps,
 * §4.3's mutation records — plus the RECORDER that feeds §4.2.3's drain. Every HTML ELEMENT INSERTION STEPS
 * entry already lives in that drain instead (html_script_prepare, iframe_create_navigable,
 * custom_elements_element_connected), for three things a hook cannot give: the drain runs in the INSERTED
 * NODE'S document realm rather than in whichever realm performed the write, it runs per node at a rest point
 * that can yield and fork, and it runs at §4.2.3 insert step 7 — BEFORE step 8's mutation record, which a
 * sixth hook registered after mutation_observer_tree_steps would run after. (The list's bound is real and not
 * a truncation: node_add_tree_hook CHECKs it in dev and in release, so a seventh registrant would abort at
 * registration rather than be dropped. Nothing here needs the sixth slot.) */
void media_element_source_inserted(JSContext *ctx, lxb_dom_element_t *el)
{
    lxb_dom_node_t *parent;
    JSValue wrap, st;
    char *src;
    bool has_src;

    if (!media_source_is(lxb_dom_interface_node(el))) return;
    parent = lxb_dom_interface_node(el)->parent;                       /* step 1 */
    if (!media_is_node(parent)) return;
    DCHECK(g_ready, "a `source` was inserted into a media element before §4.8.11 was declared");
    wrap = node_wrap(ctx, parent);
    st = media_state(ctx, wrap);
    src = element_attr_get(ctx, wrap, "src");
    has_src = src != NULL;
    free(src);
    /* Step 2. */
    if (!has_src && st_int(ctx, st, "networkState") == NETWORK_EMPTY) {
        media_invoke_selection(ctx, wrap);
    } else if (st_bool(ctx, st, "sourceWait")) {
        /* THE OTHER CONSUMER OF THIS EVENT, which the standard states inside §4.8.11.5 rather than here. */
        DFAIL("a `source` was inserted into a media element parked at HTML §4.8.11.5 step 14's children mode "
              "WAITING step, and that insertion is the wake the step is waiting for: \"wait until the node "
              "after pointer is a node other than the end of the list\", then set the delaying-the-load-event "
              "flag back to true, set the networkState back to NETWORK_LOADING, and jump back to the find "
              "next candidate step. This engine ENDS that flow at the wait rather than parking it, so the "
              "pointer it stopped at was never recorded and there is nothing here to resume. Build it: record "
              "the pointer beside `sourceWait` where MS_RESELECT sets that flag, and re-enter the machine at "
              "MS_RESELECT from here");
    }
    JS_FreeValue(ctx, st);
    JS_FreeValue(ctx, wrap);
}

/* ---- the members ------------------------------------------------------------------------------------------- */

/* MAGICS for the getters that share one body — a getter is one C function over the state record's fields. */
enum {
    MG_NETWORK_STATE = 0, MG_READY_STATE, MG_CURRENT_SRC, MG_ERROR, MG_SRC_OBJECT, MG_PAUSED, MG_SEEKING,
    MG_DEFAULT_RATE, MG_RATE, MG_PRESERVES_PITCH, MG_VOLUME, MG_MUTED, MG_ENDED,
};

static JSValue js_media_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue st = media_state_of(ctx, this_val, "a media element member"), out;

    if (JS_IsException(st)) return st;
    switch (magic) {
    case MG_NETWORK_STATE: out = JS_NewInt32(ctx, st_int(ctx, st, "networkState")); break;
    case MG_READY_STATE:   out = JS_NewInt32(ctx, st_int(ctx, st, "readyState")); break;
    case MG_CURRENT_SRC:   out = JS_GetPropertyStr(ctx, st, "currentSrc"); break;
    case MG_ERROR:         out = JS_GetPropertyStr(ctx, st, "error"); break;
    case MG_SRC_OBJECT:    out = JS_GetPropertyStr(ctx, st, "srcObject"); break;
    case MG_PAUSED:        out = JS_NewBool(ctx, st_bool(ctx, st, "paused")); break;
    case MG_SEEKING:       out = JS_NewBool(ctx, st_bool(ctx, st, "seeking")); break;
    case MG_DEFAULT_RATE:  out = JS_NewFloat64(ctx, st_num(ctx, st, "defaultRate")); break;
    case MG_RATE:          out = JS_NewFloat64(ctx, st_num(ctx, st, "rate")); break;
    case MG_PRESERVES_PITCH: out = JS_NewBool(ctx, st_bool(ctx, st, "preservesPitch")); break;
    case MG_VOLUME:        out = JS_NewFloat64(ctx, st_num(ctx, st, "volume")); break;
    /* §4.8.11.8: "a media element is muted if … its muted state is true, or its muted state is `default` and
       it has a muted content attribute" — three states, one boolean answer. */
    case MG_MUTED: {
        int m = st_int(ctx, st, "muted");
        bool muted;

        if (m == MUTED_DEFAULT) {
            char *attr = element_attr_get(ctx, this_val, "muted");
            muted = attr != NULL;
            free(attr);
        } else {
            muted = m == MUTED_TRUE;
        }
        out = JS_NewBool(ctx, muted);
        break;
    }
    /* §4.8.11.8's ENDED PLAYBACK. The modelled resource is unbounded, so the current playback position can
       never be the end of it — which is the same answer a real browser gives for a live stream. */
    case MG_ENDED:
        out = JS_NewBool(ctx, st_int(ctx, st, "readyState") >= HAVE_METADATA &&
                              st_num(ctx, st, "current") >= st_num(ctx, st, "duration"));
        break;
    default:
        DFAIL("a media element getter was declared with a magic §4.8.11 does not have");
        out = JS_UNDEFINED;
    }
    JS_FreeValue(ctx, st);
    return out;
}

/* §4.8.11.6's `duration`: the modelled device's own answer, carried as the EXAMPLE of a concolic keyed on the
   resource — so `if (v.duration > 60)` forks into the world where the resource is long instead of collapsing
   onto one modelled number. concolic_source_wrap hands back the plain double where no source overlay is
   installed, which is what keeps this component testable against the standard. */
static JSValue js_media_duration(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue st = media_state_of(ctx, this_val, "duration"), out;
    char shape[256], src[256];

    (void)magic;
    if (JS_IsException(st)) return st;
    out = JS_NewFloat64(ctx, st_num(ctx, st, "duration"));
    if (st_int(ctx, st, "readyState") >= HAVE_METADATA) {
        media_source_id(ctx, st, shape, sizeof shape, src, sizeof src);
        out = concolic_source_wrap(ctx, shape, src, out);
    }
    JS_FreeValue(ctx, st);
    return out;
}

/* §4.8.11.6's `currentTime` getter: "the media element's default playback start position, unless that is zero,
   in which case the element's official playback position". */
static JSValue js_media_current_time(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue st = media_state_of(ctx, this_val, "currentTime");
    double start;

    (void)magic;
    if (JS_IsException(st)) return st;
    start = st_num(ctx, st, "defaultStart");
    {
        double out = start != 0 ? start : st_num(ctx, st, "official");
        JS_FreeValue(ctx, st);
        return JS_NewFloat64(ctx, out);
    }
}

/* §4.8.11.9's SEEK, which the currentTime setter and fastSeek() both perform. The seekable ranges are one
   range over the whole timeline once the metadata is known, so a position at or after zero is always in one;
   with no metadata there are no ranges at all and step 8 sets `seeking` back to false and returns. */
static void media_seek(JSContext *ctx, JSValueConst el, JSValueConst st, double to)
{
    st_set_bool(ctx, st, "showPoster", false);
    if (st_int(ctx, st, "readyState") == HAVE_NOTHING) return;
    st_set_bool(ctx, st, "seeking", true);
    if (to > st_num(ctx, st, "duration")) to = st_num(ctx, st, "duration");
    if (to < 0) to = 0;
    media_queue_fire(ctx, el, "seeking");
    st_set_num(ctx, st, "current", to);
    st_set_num(ctx, st, "official", to);
    /* The media data for the new position is available the moment the position moves — the modelled device
       holds the whole resource — so the synchronous section runs with nothing to wait for. */
    st_set_bool(ctx, st, "seeking", false);
    media_queue_fire(ctx, el, "timeupdate");
    media_queue_fire(ctx, el, "seeked");
}

static JSValue js_media_set_current_time(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    JSValue st = media_state_of(ctx, this_val, "currentTime");
    double to = 0;

    (void)magic;
    if (JS_IsException(st)) return st;
    DCHECK(JS_IsNumber(val), "the currentTime setter's value reached the body unconverted — §4.8.11.6 declares "
                             "it a double and the args machine converts it before the body runs");
    JS_ToFloat64(ctx, &to, val);
    if (st_int(ctx, st, "readyState") == HAVE_NOTHING)
        st_set_num(ctx, st, "defaultStart", to);
    else
        media_seek(ctx, this_val, st, to);
    JS_FreeValue(ctx, st);
    return JS_UNDEFINED;
}

/* §4.8.11.9's `fastSeek(time)`: "seek to the time given by time, with the approximate-for-speed flag set". The
   flag lets a user agent snap to a nearby key frame; the modelled device has no frames to snap to, so the
   adjusted position is the requested one — which the spec's own constraint (the adjustment must stay on the
   same side of the current position) permits exactly. */
static JSValue js_media_fast_seek(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                  int magic)
{
    JSValue st = media_state_of(ctx, this_val, "fastSeek");
    double to = 0;

    (void)magic;
    DCHECK(argc >= 1, "fastSeek reached its body with no time — §3.6 step 5's TypeError is the declaration's");
    if (JS_IsException(st)) return st;
    JS_ToFloat64(ctx, &to, argv[0]);
    media_seek(ctx, this_val, st, to);
    JS_FreeValue(ctx, st);
    return JS_UNDEFINED;
}

/* §4.8.11.6's `getStartDate()`: "return a new Date object representing the current timeline offset". The
   modelled resource declares no start date, so the offset is NaN and the Date is an invalid one — which is
   exactly what a real browser answers for a file with no explicit time and date. */
static JSValue js_media_start_date(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                   int magic)
{
    JSValue st = media_state_of(ctx, this_val, "getStartDate"), out;

    (void)argc; (void)argv; (void)magic;
    if (JS_IsException(st)) return st;
    out = JS_NewDate(ctx, st_num(ctx, st, "timelineOffset"));
    JS_FreeValue(ctx, st);
    return out;
}

/* The three TimeRanges getters — each mints a NEW object, which §4.8.11.8 enshrines ("returning a new object
   each time is a bad pattern … only enshrined here as it would be costly to change it"). */
enum { MR_BUFFERED = 0, MR_SEEKABLE, MR_PLAYED };

static JSValue js_media_ranges(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue st = media_state_of(ctx, this_val, "a TimeRanges attribute"), out;

    if (JS_IsException(st)) return st;
    out = media_ranges_for(ctx, st, magic == MR_PLAYED);
    JS_FreeValue(ctx, st);
    return out;
}

/* §4.8.11.2's `srcObject` setter: "set this's assigned media provider object to the given value. Invoke this's
   media element load algorithm." The IDL type is `(MediaStream or MediaSource or Blob)?`, and of those three
   only Blob exists in this build — so a page can only ever hand over a Blob or null, and the value is stored
   as it arrives rather than brand-checked against two interfaces that are honestly absent. */
static JSValue js_media_set_src_object(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    JSValue st = media_state_of(ctx, this_val, "srcObject");

    (void)magic;
    if (JS_IsException(st)) return st;
    JS_SetPropertyStr(ctx, st, "srcObject",
                      JS_IsUndefined(val) ? JS_NULL : JS_DupValue(ctx, val));
    media_load_algorithm(ctx, this_val, st);
    JS_FreeValue(ctx, st);
    return JS_UNDEFINED;
}

/* §4.8.11.8's `volume` setter: "if the given value is not in the range 0.0 to 1.0 inclusive, then throw an
   IndexSizeError", then set the playback volume — which fires `volumechange` when the value CHANGES and does
   nothing at all when it does not. */
static JSValue js_media_set_volume(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    JSValue st = media_state_of(ctx, this_val, "volume");
    double v = 0;

    (void)magic;
    if (JS_IsException(st)) return st;
    JS_ToFloat64(ctx, &v, val);
    if (!(v >= 0.0 && v <= 1.0)) {
        JS_FreeValue(ctx, st);
        return JS_ThrowDOMException(ctx, "IndexSizeError", "the volume is not in the range 0.0 to 1.0");
    }
    if (v != st_num(ctx, st, "volume")) {
        st_set_num(ctx, st, "volume", v);
        media_queue_fire(ctx, this_val, "volumechange");
    }
    JS_FreeValue(ctx, st);
    return JS_UNDEFINED;
}

/* §4.8.11.8's `muted` setter: "set the muted state of this to the given value", which is the three-valued
   state going to true or false and a `volumechange` when it changed. */
static JSValue js_media_set_muted(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    JSValue st = media_state_of(ctx, this_val, "muted");
    int to = JS_ToBool(ctx, val) ? MUTED_TRUE : MUTED_FALSE;

    (void)magic;
    if (JS_IsException(st)) return st;
    if (to != st_int(ctx, st, "muted")) {
        st_set_int(ctx, st, "muted", to);
        media_queue_fire(ctx, this_val, "volumechange");
    }
    JS_FreeValue(ctx, st);
    return JS_UNDEFINED;
}

/* §4.8.11.8's two playback rates. Both fire `ratechange` when they change value; `playbackRate` additionally
   throws a NotSupportedError for a rate this user agent does not support, and this one supports every finite
   rate because it renders no audio to resample. */
enum { MS_DEFAULT_RATE = 0, MS_RATE };

static JSValue js_media_set_rate(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    JSValue st = media_state_of(ctx, this_val, magic == MS_RATE ? "playbackRate" : "defaultPlaybackRate");
    const char *field = magic == MS_RATE ? "rate" : "defaultRate";
    double v = 0;

    if (JS_IsException(st)) return st;
    JS_ToFloat64(ctx, &v, val);
    if (v != st_num(ctx, st, field)) {
        st_set_num(ctx, st, field, v);
        media_queue_fire(ctx, this_val, "ratechange");
    }
    JS_FreeValue(ctx, st);
    return JS_UNDEFINED;
}

/* §4.8.11.8's `preservesPitch`: "the setter steps are to correspondingly switch the pitch-preserving algorithm
   on or off". There is no audio to pitch-shift, so what the flag switches is the value the getter reads back,
   which is the whole of what a page can observe. */
static JSValue js_media_set_pitch(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    JSValue st = media_state_of(ctx, this_val, "preservesPitch");

    (void)magic;
    if (JS_IsException(st)) return st;
    st_set_bool(ctx, st, "preservesPitch", JS_ToBool(ctx, val) != 0);
    JS_FreeValue(ctx, st);
    return JS_UNDEFINED;
}

/* An ASCII CASE-INSENSITIVE match — HTML's own comparison for a keyword of an enumerated attribute, which is
   what `<video loading="LAZY">` needs and what the C library's locale-dependent strcasecmp is not. */
static bool media_ascii_eq(const char *a, const char *b)
{
    size_t i;

    for (i = 0; a[i] && b[i]; i++) {
        char x = a[i], y = b[i];

        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return a[i] == b[i];
}

/* §4.8.11.5's `loading` — an enumerated attribute limited to only known values, whose invalid and missing
   value defaults are both the Eager state. The getter therefore returns the canonical keyword of the state the
   content value corresponds to, which is what "limited to only known values" means (§2.6.1's reflect rules)
   and is why this is not a plain string reflection. */
static JSValue js_media_get_loading(JSContext *ctx, JSValueConst this_val, int magic)
{
    char *v;
    bool lazy;

    (void)magic;
    if (!media_is(ctx, this_val))
        return JS_ThrowTypeError(ctx, "loading read on something that is not a media element");
    v = element_attr_get(ctx, this_val, "loading");
    lazy = v && media_ascii_eq(v, "lazy");
    free(v);
    return JS_NewString(ctx, lazy ? "lazy" : "eager");
}

static JSValue js_media_set_loading(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    const char *s;

    (void)magic;
    if (!media_is(ctx, this_val))
        return JS_ThrowTypeError(ctx, "loading set on something that is not a media element");
    s = JS_ToCString(ctx, val);
    if (!s) return JS_EXCEPTION;
    element_attr_set(ctx, this_val, "loading", s);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

/* §4.8.11.3's `canPlayType(type)`. */
static JSValue js_media_can_play_type(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                      int magic)
{
    const char *type;
    JSValue out;

    (void)magic;
    DCHECK(argc >= 1, "canPlayType reached its body with no type — §3.6 step 5's TypeError is the "
                      "declaration's");
    if (!media_is(ctx, this_val))
        return JS_ThrowTypeError(ctx, "canPlayType called on something that is not a media element");
    /* An UNKNOWN type is still a type: concolic_name_cstr gives the SHAPE, which is a real, stable string, and
       the answer for a container nobody recognises is the empty string — the honest answer for a type nobody
       has pinned, and the same rule matchMedia applies to an unknown query. */
    type = concolic_name_cstr(ctx, argv[0]);
    if (!type) return JS_EXCEPTION;
    out = JS_NewString(ctx, device_can_play(type));
    JS_FreeCString(ctx, type);
    return out;
}

/* §4.8.11.5's `load()`. */
static JSValue js_media_load(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue st = media_state_of(ctx, this_val, "load");

    (void)argc; (void)argv; (void)magic;
    if (JS_IsException(st)) return st;
    media_load_algorithm(ctx, this_val, st);
    JS_FreeValue(ctx, st);
    return JS_UNDEFINED;
}

/* §4.8.11.8's `play()` and its INTERNAL PLAY STEPS. The promise is created here and appended to the list of
   pending play promises; every settlement of it happens in a queued task, which is what the spec says and what
   keeps a resolve out of this C body. */
static JSValue js_media_play(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue st = media_state_of(ctx, this_val, "play"), promise, error, pair, list;
    JSValue funcs[2];
    int ready;

    (void)argc; (void)argv; (void)magic;
    if (JS_IsException(st)) return st;

    /* Step 2: "if the media element's error attribute is not null and its code is
       MEDIA_ERR_SRC_NOT_SUPPORTED, then return a promise rejected with a NotSupportedError". */
    error = JS_GetPropertyStr(ctx, st, "error");
    if (!JS_IsNull(error) && !JS_IsUndefined(error)) {
        JSValue code = JS_UNDEFINED, slots;
        int32_t c = 0;

        if (JS_GetOwnSlot(ctx, &slots, error, g_atom_state) > 0) {
            code = JS_GetPropertyStr(ctx, slots, "code");
            JS_FreeValue(ctx, slots);
            JS_ToInt32(ctx, &c, code);
            JS_FreeValue(ctx, code);
        }
        if (c == MEDIA_ERR_SRC_NOT_SUPPORTED) {
            JSValue rejected;

            JS_FreeValue(ctx, error);
            JS_FreeValue(ctx, st);
            JS_ThrowDOMException(ctx, "NotSupportedError", "the media resource is not suitable");
            rejected = JS_GetException(ctx);
            promise = JS_NewPromiseCapability(ctx, funcs);
            if (JS_IsException(promise)) { JS_FreeValue(ctx, rejected); return promise; }
            /* A promise REJECTED WITH a value. The resolving function is called AS A CALL-ROOT FLOW, never as
               a C activation: JS_Call from here has no flow base under it, which is what every other host
               delivery in this engine avoids for the same reason. */
            if (JS_CallAsFlow(ctx, funcs[1], rejected) < 0) JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, funcs[0]);
            JS_FreeValue(ctx, funcs[1]);
            JS_FreeValue(ctx, rejected);
            return promise;
        }
    }
    JS_FreeValue(ctx, error);

    /* Step 5: "let promise be a new promise and append promise to the list of pending play promises". */
    promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) { JS_FreeValue(ctx, st); return promise; }
    pair = JS_NewArray(ctx);
    CHECK(!JS_IsException(pair), "§4.8.11.8: OOM appending a pending play promise");
    JS_SetPropertyUint32(ctx, pair, 0, funcs[0]);
    JS_SetPropertyUint32(ctx, pair, 1, funcs[1]);
    list = JS_GetPropertyStr(ctx, st, "playPromises");
    JS_SetPropertyUint32(ctx, list, arr_len(ctx, list), pair);
    JS_FreeValue(ctx, list);

    /* The INTERNAL PLAY STEPS. */
    if (st_int(ctx, st, "networkState") == NETWORK_EMPTY)
        media_invoke_selection(ctx, this_val);
    ready = st_int(ctx, st, "readyState");
    if (st_bool(ctx, st, "paused")) {
        st_set_bool(ctx, st, "paused", false);
        st_set_bool(ctx, st, "showPoster", false);
        media_queue_fire(ctx, this_val, "play");
        if (ready <= HAVE_CURRENT_DATA)
            media_queue_fire(ctx, this_val, "waiting");
        else
            media_notify_playing(ctx, this_val, st);
    } else if (ready >= HAVE_FUTURE_DATA) {
        /* "The media element is already playing": take the pending play promises and queue a task to resolve
           them, with no event at all. */
        static const char *const NONE[] = { "" };

        media_queue_task(ctx, this_val, media_event_list(ctx, NONE, 0), media_take_play_promises(ctx, st),
                         MTA_RESOLVE);
    }
    st_set_bool(ctx, st, "canAutoplay", false);
    JS_FreeValue(ctx, st);
    return promise;
}

/* §4.8.11.8's `pause()` and its INTERNAL PAUSE STEPS. */
static JSValue js_media_pause(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue st = media_state_of(ctx, this_val, "pause");

    (void)argc; (void)argv; (void)magic;
    if (JS_IsException(st)) return st;
    if (st_int(ctx, st, "networkState") == NETWORK_EMPTY)
        media_invoke_selection(ctx, this_val);
    st_set_bool(ctx, st, "canAutoplay", false);
    if (!st_bool(ctx, st, "paused")) {
        static const char *const PAUSE_EV[] = { "timeupdate", "pause" };

        st_set_bool(ctx, st, "paused", true);
        media_queue_task(ctx, this_val, media_event_list(ctx, PAUSE_EV, 2), media_take_play_promises(ctx, st),
                         MTA_REJECT_ABORT);
        st_set_num(ctx, st, "official", st_num(ctx, st, "current"));
    }
    JS_FreeValue(ctx, st);
    return JS_UNDEFINED;
}

/* ---- declare / install --------------------------------------------------------------------------------------- */

/* §4.8.11's REFLECTIONS, which the IDL puts on HTMLMediaElement and not on the two interfaces that inherit
   them. They were on HTMLAudioElement.prototype and HTMLVideoElement.prototype — two copies of one member,
   which is what `Object.getOwnPropertyDescriptor(HTMLVideoElement.prototype, 'src')` returning a descriptor
   makes visible in this engine and undefined in a browser.
   `src` IS here now. It was excluded on the grounds that its setter carries a step no plain reflection can —
   "if a src attribute of a media element is set or changed, invoke the media element load algorithm" — and
   that is not the SETTER's step: it is §4.9's attribute change steps, which media_element_attr_changed already
   owns so that `v.setAttribute('src', u)` loads too. What actually kept it out is that the IDL says
   `[CEReactions, ReflectURL] attribute USVString src` and the registry had no URL kind, so the hand-written
   getter answered the RAW attribute — `<video src="/a.mp4">` read back `/a.mp4` where every browser answers
   the absolute URL. Both bodies are deleted; the kind carries it. */
static const ElReflect R_MEDIA[] = {
    { "src",         "src",         REFLECT_URL },
    { "crossOrigin", "crossorigin", REFLECT_STRING },
    { "preload",     "preload",     REFLECT_STRING },
    { "autoplay",    "autoplay",    REFLECT_BOOL },
    { "loop",        "loop",        REFLECT_BOOL },
    { "controls",    "controls",    REFLECT_BOOL },
    { "defaultMuted", "muted",      REFLECT_BOOL },
};

/* §4.8.11's constants. Web IDL puts a `const` on BOTH the interface object and the prototype. */
static const JSCFunctionListEntry MEDIA_CONSTS[] = {
    JS_PROP_INT32_DEF("NETWORK_EMPTY", NETWORK_EMPTY, 0),
    JS_PROP_INT32_DEF("NETWORK_IDLE", NETWORK_IDLE, 0),
    JS_PROP_INT32_DEF("NETWORK_LOADING", NETWORK_LOADING, 0),
    JS_PROP_INT32_DEF("NETWORK_NO_SOURCE", NETWORK_NO_SOURCE, 0),
    JS_PROP_INT32_DEF("HAVE_NOTHING", HAVE_NOTHING, 0),
    JS_PROP_INT32_DEF("HAVE_METADATA", HAVE_METADATA, 0),
    JS_PROP_INT32_DEF("HAVE_CURRENT_DATA", HAVE_CURRENT_DATA, 0),
    JS_PROP_INT32_DEF("HAVE_FUTURE_DATA", HAVE_FUTURE_DATA, 0),
    JS_PROP_INT32_DEF("HAVE_ENOUGH_DATA", HAVE_ENOUGH_DATA, 0),
};

static const JSCFunctionListEntry ERROR_CONSTS[] = {
    JS_PROP_INT32_DEF("MEDIA_ERR_ABORTED", MEDIA_ERR_ABORTED, 0),
    JS_PROP_INT32_DEF("MEDIA_ERR_NETWORK", MEDIA_ERR_NETWORK, 0),
    JS_PROP_INT32_DEF("MEDIA_ERR_DECODE", MEDIA_ERR_DECODE, 0),
    JS_PROP_INT32_DEF("MEDIA_ERR_SRC_NOT_SUPPORTED", MEDIA_ERR_SRC_NOT_SUPPORTED, 0),
};

/* The four members of §4.8.11.10 and §4.8.11.11 this build does not have, asserted absent per realm rather
   than left as a silence the auditor and the next reader each have to re-derive. */
static const char *const MEDIA_ABSENT[] = { "audioTracks", "videoTracks", "textTracks", "addTextTrack" };

void media_element_declare(JSContext *ctx)
{
    static const IdlArgType STR1[1] = { IDL_DOMSTRING };
    static const IdlArgType DBL1[1] = { IDL_DOUBLE };
    static const IdlArgType IDX1[1] = { IDL_UNSIGNED_LONG };
    JSRuntime *rt = JS_GetRuntime(ctx);

    DCHECK(!g_ready, "media_element_declare ran twice — §4.8.11's interfaces are declared once per AGENT");
    {
        JSClassDef m = { "HTMLMediaElement" }, e = { "MediaError" }, r = { "TimeRanges" };
        JS_NewClassID(rt, &g_media_class);
        JS_NewClass(rt, g_media_class, &m);
        JS_NewClassID(rt, &g_error_class);
        JS_NewClass(rt, g_error_class, &e);
        JS_NewClassID(rt, &g_ranges_class);
        JS_NewClass(rt, g_ranges_class, &r);
    }
    g_state_key = JS_NewSymbol(ctx, "mediaElementState", false);
    CHECK(!JS_IsException(g_state_key), "§4.8.11: the media element state slot key allocation failed");
    g_atom_state = JS_ValueToAtom(ctx, g_state_key);
    CHECK(g_atom_state != JS_ATOM_NULL, "§4.8.11: the media element state slot key could not be interned");

    g_refl_base = element_declare_reflections(ctx, R_MEDIA, (int)(sizeof(R_MEDIA) / sizeof(R_MEDIA[0])));
    g_id_load = idl_method_id(ctx, NULL, 0, js_media_load, 0);
    g_id_play = idl_method_id(ctx, NULL, 0, js_media_play, 0);
    g_id_pause = idl_method_id(ctx, NULL, 0, js_media_pause, 0);
    g_id_start_date = idl_method_id(ctx, NULL, 0, js_media_start_date, 0);
    g_id_can_play = idl_method_id(ctx, STR1, 1, js_media_can_play_type, 0);
    g_id_fast_seek = idl_method_id(ctx, DBL1, 1, js_media_fast_seek, 0);
    g_id_range_start = idl_method_id(ctx, IDX1, 1, js_ranges_at, 0);
    g_id_range_end = idl_method_id(ctx, IDX1, 1, js_ranges_at, 1);

    g_set_src_object = idl_setter_id(ctx, IDL_ANY, false, js_media_set_src_object, 0);
    g_set_current_time = idl_setter_id(ctx, IDL_DOUBLE, false, js_media_set_current_time, 0);
    g_set_volume = idl_setter_id(ctx, IDL_DOUBLE, false, js_media_set_volume, 0);
    g_set_muted = idl_setter_id(ctx, IDL_BOOLEAN, false, js_media_set_muted, 0);
    g_set_rate = idl_setter_id(ctx, IDL_DOUBLE, false, js_media_set_rate, MS_RATE);
    g_set_default_rate = idl_setter_id(ctx, IDL_DOUBLE, false, js_media_set_rate, MS_DEFAULT_RATE);
    g_set_pitch = idl_setter_id(ctx, IDL_BOOLEAN, false, js_media_set_pitch, 0);
    g_set_loading = idl_setter_id(ctx, IDL_DOMSTRING, false, js_media_set_loading, 0);

    g_task_stepid = JS_RegisterStepDef(rt, &media_task_def);
    g_select_stepid = JS_RegisterStepDef(rt, &media_select_def);
    g_ready = 1;
}

JSValue media_element_proto(JSContext *ctx)
{
    JSValue p = JS_GetClassProto(ctx, g_media_class);

    DCHECK(!JS_IsNull(p), "HTMLMediaElement.prototype was asked for in a realm that never ran §4.8.11's "
                          "install — the two interfaces that inherit it are built ON this object");
    return p;
}

void media_element_install_proto(JSContext *ctx, JSValueConst html_proto)
{
    JSValue proto, prev;

    DCHECK(g_ready, "a realm asked for §4.8.11's prototypes before the interfaces were declared");
    prev = JS_GetClassProto(ctx, g_media_class);
    DCHECK(JS_IsNull(prev), "media_element_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);

    /* MediaError.prototype and TimeRanges.prototype first — neither inherits from anything but Object, and
       both are minted by algorithms on the media element's own prototype. */
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "MediaError.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "MediaError");
    JS_SetPropertyFunctionList(ctx, proto, ERROR_CONSTS,
                               (int)(sizeof(ERROR_CONSTS) / sizeof(ERROR_CONSTS[0])));
    idl_install_accessor(ctx, proto, "code", js_media_error_field, 0, -1);
    idl_install_accessor(ctx, proto, "message", js_media_error_field, 1, -1);
    JS_SetClassProto(ctx, g_error_class, proto);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "TimeRanges.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "TimeRanges");
    idl_install_accessor(ctx, proto, "length", js_ranges_length, 0, -1);
    idl_install_method(ctx, proto, "start", 1, g_id_range_start);
    idl_install_method(ctx, proto, "end", 1, g_id_range_end);
    JS_SetClassProto(ctx, g_ranges_class, proto);

    /* §4.8.11's `interface HTMLMediaElement : HTMLElement`. */
    proto = JS_NewObjectProto(ctx, html_proto);
    CHECK(!JS_IsException(proto), "HTMLMediaElement.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "HTMLMediaElement");
    JS_SetPropertyFunctionList(ctx, proto, MEDIA_CONSTS,
                               (int)(sizeof(MEDIA_CONSTS) / sizeof(MEDIA_CONSTS[0])));
    element_install_reflections(ctx, proto, g_refl_base, (int)(sizeof(R_MEDIA) / sizeof(R_MEDIA[0])));

    idl_install_accessor(ctx, proto, "error", js_media_get, MG_ERROR, -1);
    idl_install_accessor(ctx, proto, "srcObject", js_media_get, MG_SRC_OBJECT, g_set_src_object);
    idl_install_accessor(ctx, proto, "currentSrc", js_media_get, MG_CURRENT_SRC, -1);
    idl_install_accessor(ctx, proto, "networkState", js_media_get, MG_NETWORK_STATE, -1);
    idl_install_accessor(ctx, proto, "buffered", js_media_ranges, MR_BUFFERED, -1);
    idl_install_accessor(ctx, proto, "readyState", js_media_get, MG_READY_STATE, -1);
    idl_install_accessor(ctx, proto, "seeking", js_media_get, MG_SEEKING, -1);
    idl_install_accessor(ctx, proto, "currentTime", js_media_current_time, 0, g_set_current_time);
    idl_install_accessor(ctx, proto, "duration", js_media_duration, 0, -1);
    idl_install_accessor(ctx, proto, "paused", js_media_get, MG_PAUSED, -1);
    idl_install_accessor(ctx, proto, "defaultPlaybackRate", js_media_get, MG_DEFAULT_RATE, g_set_default_rate);
    idl_install_accessor(ctx, proto, "playbackRate", js_media_get, MG_RATE, g_set_rate);
    idl_install_accessor(ctx, proto, "preservesPitch", js_media_get, MG_PRESERVES_PITCH, g_set_pitch);
    idl_install_accessor(ctx, proto, "played", js_media_ranges, MR_PLAYED, -1);
    idl_install_accessor(ctx, proto, "seekable", js_media_ranges, MR_SEEKABLE, -1);
    idl_install_accessor(ctx, proto, "ended", js_media_get, MG_ENDED, -1);
    idl_install_accessor(ctx, proto, "volume", js_media_get, MG_VOLUME, g_set_volume);
    idl_install_accessor(ctx, proto, "muted", js_media_get, MG_MUTED, g_set_muted);
    idl_install_accessor(ctx, proto, "loading", js_media_get_loading, 0, g_set_loading);
    idl_install_method(ctx, proto, "load", 0, g_id_load);
    idl_install_method(ctx, proto, "canPlayType", 1, g_id_can_play);
    idl_install_method(ctx, proto, "fastSeek", 1, g_id_fast_seek);
    idl_install_method(ctx, proto, "getStartDate", 0, g_id_start_date);
    idl_install_method(ctx, proto, "play", 0, g_id_play);
    idl_install_method(ctx, proto, "pause", 0, g_id_pause);
    idl_members_excluded(ctx, proto, "HTMLMediaElement", MEDIA_ABSENT,
                         (int)(sizeof(MEDIA_ABSENT) / sizeof(MEDIA_ABSENT[0])),
                         "§4.8.11.10's AudioTrackList and VideoTrackList and §4.8.11.11's TextTrackList are "
                         "not built, so the four members that answer with one are ABSENT rather than shape-"
                         "only — a page's own TypeError is the forcing function, and §4.8.11.5's two steps "
                         "that drain those lists assert the producer through realm_awaits");
    JS_SetClassProto(ctx, g_media_class, proto);
}

void media_element_install(JSContext *ctx, JSValueConst global)
{
    JSValue proto, ctor;

    DCHECK(g_ready, "§4.8.11's interface objects were installed before the interfaces were declared");
    /* HTMLMediaElement's interface object goes up through the DOM's installer, which is what every other
       element interface object in this build uses — so `HTMLVideoElement` and `HTMLMediaElement` sit on the
       same [[Prototype]] rather than one of them being reachable a different way. The CONSTANTS are placed on
       it before it goes, because Web IDL puts a `const` on the interface object as well as the prototype. */
    proto = media_element_proto(ctx);
    ctor = idl_interface_object(ctx, "HTMLMediaElement", proto);
    CHECK(!JS_IsException(ctor), "the HTMLMediaElement interface object could not be allocated");
    JS_SetPropertyFunctionList(ctx, ctor, MEDIA_CONSTS,
                               (int)(sizeof(MEDIA_CONSTS) / sizeof(MEDIA_CONSTS[0])));
    node_install_interface_ctor(ctx, global, "HTMLMediaElement", proto, ctor);
    JS_FreeValue(ctx, proto);

    proto = JS_GetClassProto(ctx, g_error_class);
    DCHECK(!JS_IsNull(proto), "MediaError was installed in a realm with no prototype for it");
    ctor = idl_interface_object(ctx, "MediaError", proto);
    CHECK(!JS_IsException(ctor), "the MediaError interface object could not be allocated");
    JS_SetPropertyFunctionList(ctx, ctor, ERROR_CONSTS,
                               (int)(sizeof(ERROR_CONSTS) / sizeof(ERROR_CONSTS[0])));
    JS_FreeValue(ctx, proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "MediaError", ctor);

    proto = JS_GetClassProto(ctx, g_ranges_class);
    DCHECK(!JS_IsNull(proto), "TimeRanges was installed in a realm with no prototype for it");
    ctor = idl_interface_object(ctx, "TimeRanges", proto);
    CHECK(!JS_IsException(ctor), "the TimeRanges interface object could not be allocated");
    JS_FreeValue(ctx, proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "TimeRanges", ctor);
}

void media_element_free(JSRuntime *rt)
{
    if (!g_ready) return;
    g_ready = 0;
    /* The prototypes are the REALMS' — each goes with its context. */
    JS_FreeAtomRT(rt, g_atom_state);
    g_atom_state = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_state_key);
    g_state_key = JS_UNDEFINED;
    g_refl_base = g_id_load = g_id_can_play = g_id_play = g_id_pause = g_id_fast_seek = -1;
    g_id_start_date = g_id_range_start = g_id_range_end = -1;
    g_set_src_object = g_set_current_time = g_set_volume = g_set_muted = -1;
    g_set_rate = g_set_default_rate = g_set_pitch = g_set_loading = -1;
}
