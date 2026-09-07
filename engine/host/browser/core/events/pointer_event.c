/* THE PointerEvent INTERFACE — Pointer Events 4 §3.1 "PointerEvent interface".
 *
 * EVERY NUMBER IN THIS FILE IS ANCHORED TO ITS STANDARD, AND THAT IS NOT A STYLE. Pointer Events is not one of
 * the standards engine/specindex holds, so its citations are outside every check citegen makes — but only
 * where the anchor names it. A BARE `§3.1` here is resolved by the TERM standing beside it, and "event
 * constructing steps" is a DOM term, so the audit judged this file's Pointer Events quotations against DOM
 * §3.1 "Interface AbortController" and reported them as fabrications. That is exactly the shape CLAUDE.md
 * records for an unindexed standard whose numbers COLLIDE with an indexed one's: not shielded, ACCUSED, and
 * enriched for the sites whose author pasted verbatim. So the standard's name is written in front of every
 * number, including a repeat, and where that reads heavily it is the price of the citation being checkable.
 *
 * WHICH EDITION'S NUMBERS THESE ARE. The Editor's Draft at w3c.github.io/pointerevents/ carries NO SECTION
 * NUMBER ON ANY HEADING — every heading in the bytes a fetch returns is bare text, because that document is
 * numbered at render time — so there is nothing in it to cite. The numbers below are the published Level 4
 * Recommendation's, which is the convention core/events/mouse_event.c already states for `MouseEvent`, and
 * each carries its TITLE, which is what survives an edition the number does not.
 *
 * WHY THIS INTERFACE AND NOT ANOTHER. It was named by a residual in mouse_event.h, whose next-diff clause
 * named this file by path, and — independently — by the two instruments that ask which platform globals
 * this engine does not answer and which of those real bundles reach for. It is one of the few absent names any
 * frozen bundle in the corpus reaches for at all, and one of the two whose OBSERVABLES ARE WRITTEN BY ITS OWN
 * ALGORITHMS rather than by a subsystem this engine does not have — which is the test for whether an interface
 * can be landed survivably at all, since installing the object before the behaviour exists FLIPS a feature
 * guard true and abandons the fallback branch that was working. `"PointerEvent" in window` is a real feature
 * detection in a real bundle, and its false arm is silent: the pointer branch of a drag library is simply
 * never explored, with every endpoint and every sink behind it, and no crash anywhere says so.
 *
 * AND THE TRUE ARM IS SURVIVABLE, WHICH WAS CHECKED AGAINST THE BUNDLE RATHER THAN ASSUMED. Flipping a guard
 * true is only an improvement if the branch behind it can COMPLETE — otherwise the run abandons a fallback
 * that was working and dies one line later, which is worse than the absence. The reach site is a drag
 * library's `supportPointer: … && "PointerEvent" in window`, and everything its true arm does is register
 * `pointerdown`/`pointermove`/`pointerup` listeners and read `pointerType` off the event it is handed.
 * `setPointerCapture` — Pointer Events 4 §4 "Extensions to the Element interface", which this component does
 * not build and which belongs to core/dom/element.c — occurs ZERO times in the whole frozen corpus, and the
 * one `navigator.maxTouchPoints` in it (§6 "Extensions to the Navigator interface") sits in a touch-detection
 * helper that is not behind this guard and answers falsey either way. So the members this interface owes are
 * the members that branch needs.
 *
 * WHAT WRITES ITS OBSERVABLES. Pointer Events 4 §3.1's constructor and nothing else. `pointerId`, `width`,
 * `height`, `pressure`, `tangentialPressure`, `twist`, `pointerType`, `isPrimary` and `persistentDeviceId`
 * are the PointerEventInit members of the same name; `tiltX`/`tiltY` and `altitudeAngle`/`azimuthAngle` are
 * two spellings of ONE orientation, whose conversion Pointer Events 4 §3.1.5 states; and the two event lists
 * are that same section's own event constructing steps. There is no device in any of it: a constructed
 * pointer event's values are the ones it was constructed with, in a browser with a digitizer exactly as here.
 *
 * THE ORIENTATION CONVERSION RUNS AT THE CONSTRUCTOR, AND WHICH ARM IS CONFIRMATION RATHER THAN TEXT.
 * Pointer Events 4 §3.1.5 "Converting between tiltX / tiltY and altitudeAngle / azimuthAngle" is normative —
 * "User agents MUST use the following algorithm for converting these values" — and states its premise about
 * hardware: "Depending on the specific hardware and platform, user agents will likely only receive one set of
 * values for the transducer orientation relative to the screen plane". A CONSTRUCTED event is the one place a
 * PAGE supplies one set and not the other, and Pointer Events 4 §3.1's own attribute prose fixes what the
 * other set is when NEITHER is supplied ("For hardware and platforms that do not report tilt or angle, the
 * value MUST be 0" for tiltX and tiltY, pi/2 for `altitudeAngle`, 0 for `azimuthAngle`). What the standard
 * does not write down is which arm runs when BOTH sets are given, so that was MEASURED on real Chrome rather
 * than guessed — CLAUDE.md's rule is that Chrome is CONFIRMATION and the spec is the source of truth, and
 * here the algorithm is the spec's and only its trigger is confirmed. Nineteen constructions, one run each,
 * every reading reproduced by hand against §3.1.5's own code:
 *     {}                                     -> tilt 0,0    alt pi/2    az 0
 *     {tiltX:45}                             -> tilt 45,0   alt pi/4    az 0
 *     {tiltY:45}                             -> tilt 0,45   alt pi/4    az pi/2
 *     {tiltX:45,tiltY:45}                    -> tilt 45,45  alt 0.61548 az pi/4
 *     {altitudeAngle:0.5}                    -> tilt 61,0   alt 0.5     az 0
 *     {azimuthAngle:1.0}                     -> tilt 0,0    alt pi/2    az 1
 *     {altitudeAngle:0.5,azimuthAngle:1.0}   -> tilt 45,57  alt 0.5     az 1
 *     {tiltX:45,tiltY:45,altitudeAngle:0.5,azimuthAngle:1.0} -> tilt 45,45 alt 0.5 az 1
 *     {tiltX:undefined}                      -> tilt 0,0    alt pi/2    az 0
 * The rule those readings are: a set is SUPPLIED when either of its two members is present, and the conversion
 * runs exactly when ONE set is supplied — from tilt when only tilt is, from the angles when only the angles
 * are, and NOT AT ALL when both are (each attribute is then what the page wrote) or neither is (each is the
 * un-initialized value above). The absent member of a supplied set takes that un-initialized value FIRST,
 * which is what makes `{azimuthAngle:1.0}` answer tilt 0,0: `spherical2tilt(pi/2, 1)` divides by `tan(pi/2)`.
 * `{tiltX:undefined}` is the same as `{}` because Web IDL §3.2.17 "Dictionary types" step 4.1.4 asks "If
 * jsMemberValue is not undefined" — an explicitly-undefined member is ABSENT — which is the same question the
 * engine's own member loop asks and the reason a presence test here is a test for a missing key.
 *
 * ONE DIVERGENCE IS KNOWN AND IT IS OUTSIDE EVERY RANGE THE STANDARD STATES. Pointer Events 4 §3.1.5's two
 * functions are implemented literally, so an input outside the ranges Pointer Events 4 §3.1 declares for these
 * attributes flows through the arithmetic as written — `{altitudeAngle:5}` (the stated range is [0,pi/2]) and
 * `{tiltX:200}` (the stated range is [-90,90]) both answer differently here from real Chrome, which was
 * measured at tiltX 74 and altitudeAngle 1.22173 for those two. §3.1.5 states no clamp and §3.1 states no
 * behaviour for an out-of-range value, so there is nothing to implement and a value picked to match would be
 * an invention; what a page can reach is documented rather than guessed at.
 *
 * getCoalescedEvents() IS `[SecureContext]` AND getPredictedEvents() IS NOT, which is the IDL and is why the
 * two are installed differently. Web IDL §3.3.13 "[SecureContext]" REMOVES the member in a non-secure realm
 * rather than making it throw, so `e.getCoalescedEvents ? … : …` — which is how Pointer Events 4 §9.1
 * "Coalesced events" writes it in its own example — takes the fallback over plain http, in this engine as in
 * a real browser.
 *
 * THE TWO LISTS ARE JS ARRAYS AND NOT MALLOC'D C, per CLAUDE.md §PLATFORM-DATA-A-FLOW-QUEUES-IS-A-JS-VALUE: a
 * list of events hangs off this event's slot record, so it rides the per-flow COW delta and parks with the
 * flow like every other property write. A C list captured as a pointer would revert the POINTER on a context
 * switch and leave the nodes reachable from nothing.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/events/input_device_capabilities.h"
#include "core/events/mouse_event.h"
#include "core/events/pointer_event.h"
#include "core/events/ui_event.h"
#include "core/frame/window_proxy.h"
#include "core/idl_args.h"
#include "core/idl_slots.h"
#include "core/realm.h"

static JSValue   g_key = JS_UNDEFINED;   /* the private Symbol this interface's own slots hang off */
static JSClassID g_pe_class;    /* the class exists for its per-REALM prototype slot; nothing wears it */
static int       g_ready;
static int       g_ctor_stepid = -1;
static int       g_coalesced_id = -1;
static int       g_predicted_id = -1;

JSValue pointer_event_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_pe_class);

    DCHECK(!JS_IsNull(proto),
           "PointerEvent.prototype was asked for in a realm that never ran pointer_event_install_protos");
    return proto;   /* OWNED */
}

static JSValue pe_slots(JSContext *ctx, JSValueConst ev)
{
    JSAtom k;
    JSValue slots;

    DCHECK(g_ready, "a PointerEvent's slots were asked for before pointer_event_init ran");
    if (!JS_IsObject(ev))
        return JS_UNDEFINED;
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL)
        return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &slots, ev, k) <= 0)   /* an own SLOT, never a lookup — see ui_event.c */
        slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    return slots;
}

bool pointer_event_is_value(JSContext *ctx, JSValueConst v)
{
    JSValue slots = pe_slots(ctx, v);
    bool ok = JS_IsObject(slots);

    JS_FreeValue(ctx, slots);
    return ok;
}

/* ---- Pointer Events 4 §3.1.5's two conversions ------------------------------------------------------------ */

/* Pointer Events 4 §3.1: "The altitude (in radians) of the transducer (e.g. pen/stylus), in the range [0,π/2]" —
   and the value Pointer Events 4 §3.1 states for hardware that reports no tilt or angle, which is what an
   un-supplied set takes here. M_PI is the double closest to π, which is what `Math.PI` in Pointer Events 4 §3.1.5's
   own code is. */
#define PE_ALTITUDE_UNINITIALIZED (M_PI / 2.0)
#define PE_RAD_TO_DEG             (180.0 / M_PI)
#define PE_DEG_TO_RAD             (M_PI / 180.0)

/* ECMASCRIPT'S `Math.round`, WHICH Pointer Events 4 §3.1.5 NAMES AND C's `round()` IS NOT. "When the user agent
   calculates tiltX / tiltY from azimuthAngle / altitudeAngle it SHOULD round the final integer values using
   Math.round [ECMASCRIPT] rules" — and Math.round rounds a half UP (toward +∞) while C's `round` rounds a half AWAY
   FROM ZERO, so the two disagree at every negative half: -16.5 is -16 to one and -17 to the other. Both tilt values
   are the degrees of an `atan`, so they are within [-90,90] and `floor(x + 0.5)` is exact over that range. */
static double pe_math_round(double x)
{
    return floor(x + 0.5);
}

/* Pointer Events 4 §3.1.5's `tilt2spherical`, in C — the branch structure is the standard's own and is kept branch
   for branch rather than algebraically simplified, because what makes this checkable is that a reader can hold the
   two side by side. Degrees in, radians out. */
static void pe_tilt_to_spherical(double tilt_x_deg, double tilt_y_deg, double *altitude, double *azimuth)
{
    double tilt_x_rad = tilt_x_deg * PE_DEG_TO_RAD;
    double tilt_y_rad = tilt_y_deg * PE_DEG_TO_RAD;
    double az = 0.0, alt;

    if (tilt_x_deg == 0.0) {
        if (tilt_y_deg > 0.0)       az = M_PI / 2.0;
        else if (tilt_y_deg < 0.0)  az = 3.0 * M_PI / 2.0;
    } else if (tilt_y_deg == 0.0) {
        if (tilt_x_deg < 0.0)       az = M_PI;
    } else if (fabs(tilt_x_deg) == 90.0 || fabs(tilt_y_deg) == 90.0) {
        az = 0.0;   /* "not enough information to calculate azimuth" */
    } else {
        az = atan2(tan(tilt_y_rad), tan(tilt_x_rad));
        if (az < 0.0)
            az += 2.0 * M_PI;
    }

    if (fabs(tilt_x_deg) == 90.0 || fabs(tilt_y_deg) == 90.0)
        alt = 0.0;
    else if (tilt_x_deg == 0.0)
        alt = M_PI / 2.0 - fabs(tilt_y_rad);
    else if (tilt_y_deg == 0.0)
        alt = M_PI / 2.0 - fabs(tilt_x_rad);
    else
        alt = atan(1.0 / sqrt(pow(tan(tilt_x_rad), 2.0) + pow(tan(tilt_y_rad), 2.0)));

    *altitude = alt;
    *azimuth = az;
}

/* Pointer Events 4 §3.1.5's `spherical2tilt`, in C. Radians in, DEGREES out, already rounded by the rule
   Pointer Events 4 §3.1.5 names. */
static void pe_spherical_to_tilt(double altitude, double azimuth, double *tilt_x_deg, double *tilt_y_deg)
{
    double tilt_x_rad = 0.0, tilt_y_rad = 0.0;

    if (altitude == 0.0) {
        /* "the pen is in the X-Y plane" — the four axis cases and the four quadrants, exactly as
           Pointer Events 4 §3.1.5 enumerates them. An azimuth that is none of these leaves both at zero, which is
           that algorithm's own answer and not a default chosen here. */
        if (azimuth == 0.0 || azimuth == 2.0 * M_PI)  tilt_x_rad = M_PI / 2.0;
        if (azimuth == M_PI / 2.0)                    tilt_y_rad = M_PI / 2.0;
        if (azimuth == M_PI)                          tilt_x_rad = -M_PI / 2.0;
        if (azimuth == 3.0 * M_PI / 2.0)              tilt_y_rad = -M_PI / 2.0;
        if (azimuth > 0.0 && azimuth < M_PI / 2.0) {
            tilt_x_rad = M_PI / 2.0;  tilt_y_rad = M_PI / 2.0;
        }
        if (azimuth > M_PI / 2.0 && azimuth < M_PI) {
            tilt_x_rad = -M_PI / 2.0; tilt_y_rad = M_PI / 2.0;
        }
        if (azimuth > M_PI && azimuth < 3.0 * M_PI / 2.0) {
            tilt_x_rad = -M_PI / 2.0; tilt_y_rad = -M_PI / 2.0;
        }
        if (azimuth > 3.0 * M_PI / 2.0 && azimuth < 2.0 * M_PI) {
            tilt_x_rad = M_PI / 2.0;  tilt_y_rad = -M_PI / 2.0;
        }
    } else {
        double tan_alt = tan(altitude);

        tilt_x_rad = atan(cos(azimuth) / tan_alt);
        tilt_y_rad = atan(sin(azimuth) / tan_alt);
    }
    *tilt_x_deg = pe_math_round(tilt_x_rad * PE_RAD_TO_DEG);
    *tilt_y_deg = pe_math_round(tilt_y_rad * PE_RAD_TO_DEG);
}

/* ---- the slot record ------------------------------------------------------------------------------------- */

enum { PE_POINTER_ID = 0, PE_WIDTH, PE_HEIGHT, PE_PRESSURE, PE_TANGENTIAL_PRESSURE, PE_TILT_X, PE_TILT_Y,
       PE_TWIST, PE_ALTITUDE_ANGLE, PE_AZIMUTH_ANGLE, PE_POINTER_TYPE, PE_IS_PRIMARY,
       PE_PERSISTENT_DEVICE_ID };
static const char *const PE_SLOT[] = {
    "pointerId", "width", "height", "pressure", "tangentialPressure", "tiltX", "tiltY",
    "twist", "altitudeAngle", "azimuthAngle", "pointerType", "isPrimary",
    "persistentDeviceId",
};
/* The two event lists, which are not attributes: Pointer Events 4 §3.1 gives them no IDL attribute at all and
   reaches them only through the two methods, so they are slots with no row in the table above. */
static const char *const PE_COALESCED = "coalescedEvents";
static const char *const PE_PREDICTED = "predictedEvents";

/* IS THIS MEMBER OF THE CONVERTED DICTIONARY PRESENT — Web IDL §3.2.17 step 4.1.4's question, asked of the
   record the declaration already built. A member whose IDL writes NO default (`long tiltX;`) and that the page
   did not supply leaves no key at all, and every member that IS present is a Number by then, so `undefined`
   means absent and can mean nothing else. Only the four orientation members need this: every other member of
   this dictionary declares a default, so its absence and its default are the same value. */
static bool pe_dict_has(JSContext *ctx, JSValueConst init, const char *name)
{
    JSValue v = idl_dict_get(ctx, init, name);
    bool present = !JS_IsUndefined(v);

    JS_FreeValue(ctx, v);
    return present;
}

/* A `double` MEMBER READ WITH THE ATTRIBUTE'S OWN UN-INITIALIZED VALUE UNDER IT, and it is a SECOND SPEC FACT
   rather than a second copy of the dictionary's default. Two of Pointer Events 4 §3.1's attributes have a
   non-zero un-initialized value — `width` and `height`, whose prose states "the user agent MUST return a
   default value of 1" — and every path that builds one of these events is entitled to that value, INCLUDING
   the ones that never ran Web IDL §3.2.17's member loop: DOM §2.5 "Constructing events"' create an event
   passes an absent dictionary, and HTML §8.1.8.3 "Event firing" hands this component a record it built itself.
   The dictionary's `= 1` and the attribute's un-initialized 1 are the same number stated by two different
   requirements of one standard, so they cannot disagree without §3.1 disagreeing with itself.
   IT IS NEEDED HERE AND NOT IN mouse_event.c because every un-initialized value on THAT interface is zero,
   which is what a plain read of an absent member already answers — the difference is §3.1's, not a
   convention's. */
static double pe_dict_f64_or(JSContext *ctx, JSValueConst init, const char *name, double uninitialized)
{
    return pe_dict_has(ctx, init, name) ? ui_event_dict_f64(ctx, init, name) : uninitialized;
}

/* `sequence<PointerEvent>` OFF THE CONVERTED DICTIONARY, CLONED — Pointer Events 4 §3.1's "The event constructing
   steps for PointerEvent clones PointerEventInit's coalescedEvents to coalesced events list and clones
   PointerEventInit's predictedEvents to predicted events list". The declaration's Web IDL §3.2.21 walk already
   built an Array of branded PointerEvents; this copies it so the list this event holds is its own. AN ABSENT MEMBER
   IS AN EMPTY LIST, WHICH IS THE IDL's OWN `= []` and not a default invented here. It is placed by this body rather
   than by the declaration because IdlDictDefault has no empty-sequence row — the values it can place are a null, a
   string, a zero, a one and a false — so the member is declared IDL_DEFAULT_NONE, which is what the IDL writes for
   every OTHER defaultless member of this dictionary too and is therefore not a statement that this member has no
   default. The two are observationally one thing: Pointer Events 4 §3.1's default IS the empty sequence. */
static JSValue pe_event_list_of(JSContext *ctx, JSValueConst init, const char *name)
{
    JSValue src = idl_dict_get(ctx, init, name), out = JS_NewArray(ctx);
    uint32_t i, n = 0;
    JSValue len;

    if (JS_IsException(out)) {
        JS_FreeValue(ctx, src);
        return out;
    }
    if (!JS_IsObject(src)) {
        JS_FreeValue(ctx, src);
        return out;
    }
    len = JS_GetPropertyStr(ctx, src, "length");
    if (JS_ToUint32(ctx, &n, len) < 0)
        n = 0;
    JS_FreeValue(ctx, len);
    for (i = 0; i < n; i++)
        JS_SetPropertyUint32(ctx, out, i, JS_GetPropertyUint32(ctx, src, i));
    JS_FreeValue(ctx, src);
    return out;
}

/* THE ORIENTATION PAIR, Pointer Events 4 §3.1.5's conversion at the one place a page can supply one set and not the
   other. `init` is the CONVERTED dictionary, so every value read here is already a Number of its declared type and
   nothing below runs the page's code. */
static void pe_orientation(JSContext *ctx, JSValueConst init, double *tilt_x, double *tilt_y,
                           double *altitude, double *azimuth)
{
    bool has_tilt = pe_dict_has(ctx, init, "tiltX") || pe_dict_has(ctx, init, "tiltY");
    bool has_angle = pe_dict_has(ctx, init, "altitudeAngle") || pe_dict_has(ctx, init, "azimuthAngle");

    /* Pointer Events 4 §3.1's un-initialized values first, so a set that is supplied with only one of its two
       members has the other one to convert FROM. */
    *tilt_x = (double)ui_event_dict_i32(ctx, init, "tiltX");
    *tilt_y = (double)ui_event_dict_i32(ctx, init, "tiltY");
    *altitude = pe_dict_f64_or(ctx, init, "altitudeAngle", PE_ALTITUDE_UNINITIALIZED);
    *azimuth = ui_event_dict_f64(ctx, init, "azimuthAngle");
    if (has_tilt && !has_angle)
        pe_tilt_to_spherical(*tilt_x, *tilt_y, altitude, azimuth);
    else if (has_angle && !has_tilt)
        pe_spherical_to_tilt(*altitude, *azimuth, tilt_x, tilt_y);
}

/* The thirteen own slots plus the two event lists, placed on an event whose Event, UIEvent and MouseEvent
   halves are already built. Returns -1 with the throw live. */
static int pe_init_slots(JSContext *ctx, JSValueConst ev, JSValueConst init)
{
    JSValue slots, coalesced, predicted;
    JSAtom k;
    double tilt_x, tilt_y, altitude, azimuth;

    DCHECK(g_ready, "a PointerEvent was minted before pointer_event_init declared the interface — the slot key "
                    "it hangs its state off is made there");
    slots = idl_slots_new(ctx);
    k = JS_ValueToAtom(ctx, g_key);
    if (JS_IsException(slots) || k == JS_ATOM_NULL) {
        JS_FreeValue(ctx, slots);
        if (k != JS_ATOM_NULL) JS_FreeAtom(ctx, k);
        return -1;
    }
    coalesced = pe_event_list_of(ctx, init, PE_COALESCED);
    predicted = pe_event_list_of(ctx, init, PE_PREDICTED);
    if (JS_IsException(coalesced) || JS_IsException(predicted)) {
        JS_FreeValue(ctx, coalesced);
        JS_FreeValue(ctx, predicted);
        JS_FreeValue(ctx, slots);
        JS_FreeAtom(ctx, k);
        return -1;
    }
    pe_orientation(ctx, init, &tilt_x, &tilt_y, &altitude, &azimuth);
    JS_SetPropertyStr(ctx, slots, PE_SLOT[PE_POINTER_ID],
                      JS_NewInt32(ctx, ui_event_dict_i32(ctx, init, "pointerId")));
    /* `double width = 1` / `double height = 1` — Pointer Events 4 §3.1's own "the user agent MUST return a
       default value of 1" for an input with no contact geometry. The 1 is written here as well as in the
       dictionary because it is the ATTRIBUTE's un-initialized value and reaches events the member loop never
       converted — see pe_dict_f64_or. */
    JS_SetPropertyStr(ctx, slots, PE_SLOT[PE_WIDTH],
                      JS_NewFloat64(ctx, pe_dict_f64_or(ctx, init, "width", 1.0)));
    JS_SetPropertyStr(ctx, slots, PE_SLOT[PE_HEIGHT],
                      JS_NewFloat64(ctx, pe_dict_f64_or(ctx, init, "height", 1.0)));
    /* THE TWO `float` MEMBERS, READ BACK AS THE DOUBLES THEY ALREADY ARE. Web IDL §3.2.5 "float"'s rounding
       into single precision is the DECLARATION's — IDL_FLOAT — so what is on the record is already the
       single-precision value and re-rounding here would be a second answer to one conversion. */
    JS_SetPropertyStr(ctx, slots, PE_SLOT[PE_PRESSURE],
                      JS_NewFloat64(ctx, ui_event_dict_f64(ctx, init, "pressure")));
    JS_SetPropertyStr(ctx, slots, PE_SLOT[PE_TANGENTIAL_PRESSURE],
                      JS_NewFloat64(ctx, ui_event_dict_f64(ctx, init, "tangentialPressure")));
    JS_SetPropertyStr(ctx, slots, PE_SLOT[PE_TILT_X], JS_NewInt32(ctx, (int32_t)tilt_x));
    JS_SetPropertyStr(ctx, slots, PE_SLOT[PE_TILT_Y], JS_NewInt32(ctx, (int32_t)tilt_y));
    JS_SetPropertyStr(ctx, slots, PE_SLOT[PE_TWIST],
                      JS_NewInt32(ctx, ui_event_dict_i32(ctx, init, "twist")));
    JS_SetPropertyStr(ctx, slots, PE_SLOT[PE_ALTITUDE_ANGLE], JS_NewFloat64(ctx, altitude));
    JS_SetPropertyStr(ctx, slots, PE_SLOT[PE_AZIMUTH_ANGLE], JS_NewFloat64(ctx, azimuth));
    /* `DOMString pointerType = ""` — Pointer Events 4 §3.1 states the empty string as the value for a device
       type the user agent cannot detect ("If the device type cannot be detected by the user agent, then the
       value MUST be an empty string"), which is both the dictionary's default and the attribute's
       un-initialized value. It is written here for pe_dict_f64_or's reason, one type over: a record HTML
       §8.1.8.3 built by hand never ran the member loop, and `undefined` in a DOMString attribute is a value no
       reading of §3.1 admits. */
    JS_SetPropertyStr(ctx, slots, PE_SLOT[PE_POINTER_TYPE],
                      pe_dict_has(ctx, init, "pointerType") ? idl_dict_get(ctx, init, "pointerType")
                                                            : JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, slots, PE_SLOT[PE_IS_PRIMARY],
                      JS_NewBool(ctx, idl_dict_bool(ctx, init, "isPrimary")));
    JS_SetPropertyStr(ctx, slots, PE_SLOT[PE_PERSISTENT_DEVICE_ID],
                      JS_NewInt32(ctx, ui_event_dict_i32(ctx, init, "persistentDeviceId")));
    JS_SetPropertyStr(ctx, slots, PE_COALESCED, coalesced);
    JS_SetPropertyStr(ctx, slots, PE_PREDICTED, predicted);
    JS_SetProperty(ctx, (JSValue)ev, k, slots);
    JS_FreeAtom(ctx, k);
    return 0;
}

static JSValue pointer_event_new_derived(JSContext *ctx, JSValue proto, JSValueConst type, JSValueConst init,
                                         bool trusted)
{
    JSValue ev = mouse_event_new_derived(ctx, proto, type, init, trusted);

    if (JS_IsException(ev))
        return ev;
    if (pe_init_slots(ctx, ev, init) < 0) {
        JS_FreeValue(ctx, ev);
        return JS_EXCEPTION;
    }
    return ev;
}

JSValue pointer_event_new_synthetic(JSContext *ctx, const char *type, JSValueConst view)
{
    JSValue init, t, ev;

    DCHECK(g_ready, "HTML §8.1.8.3 \"Event firing\"'s fire a synthetic pointer event ran before "
                    "pointer_event_init declared the interface — step 1 creates an event using PointerEvent "
                    "and its slot key is made there");
    DCHECK(type != NULL && *type,
           "HTML §8.1.8.3 \"Event firing\"'s fire a synthetic pointer event was given no event name — step 2 "
           "initializes the type attribute to the name the caller fires, and there is no unnamed one");
    /* STEP 7's `view`, asserted rather than trusted: it is "target's node document's Window object, if any",
       so the two admissible values are a Window and null. Anything else is a caller that read the wrong
       object, and UIEvent's `Window?` conversion would report it as the page's TypeError instead. */
    DCHECK(JS_IsNull(view) || window_proxy_is_window(ctx, view),
           "HTML §8.1.8.3 \"Event firing\"'s fire a synthetic pointer event was given a `view` that is neither "
           "a Window nor null — step 7 initializes it to the TARGET's node document's Window object, and null "
           "is the spec's own answer when that document has none");
    /* THE CONVERTED DICTIONARY, exactly as focus_event.c builds one for HTML §6.6.4's fire a focus event: a
       null-prototyped record carrying the members that EXIST, each already an engine value of its IDL type. Nothing
       of the page's is on it, so building it runs none of the page's code — and it is the same record `new
       PointerEvent(type, init)` reaches pointer_event_new_derived with, so a synthetic click and a constructed one
       are ONE construction path. Steps 3, 4 and 7 are three of these members. HTML §8.1.8.3's step 6 — "according
       to the current state of the key input device, if any (false for any keys that are not available)" — and its
       step 8's getModifierState are the un-initialized key modifier state ui_event.c writes for an absent
       dictionary member: false for every key, which is what a headless agent's key input device makes them and not
       a value invented here. `pointerId` IS THE FOURTH AND IT IS NOT A STEP OF HTML §8.1.8.3, WHICH SETS NOTHING ON
       THIS INTERFACE. It is Pointer Events 4 §3.1's own sentence about the value: "The pointerId value of -1 MUST
       be reserved and used to indicate events that were generated by something other than a pointing device." A
       synthetic pointer event fired by `element.click()` is exactly such an event — there is no pointing device
       anywhere in HTML §6.5's activation behaviour — so -1 is what Pointer Events 4 §3.1 requires and 0 (the
       dictionary's default, and the right answer for a page's own `new PointerEvent`) would be a claim that a
       pointer produced it. Real Chrome answers -1 here, which is confirmation of the reading and not its source. */
    init = idl_slots_new(ctx);
    if (JS_IsException(init))
        return init;
    JS_SetPropertyStr(ctx, init, "bubbles", JS_TRUE);              /* step 3 */
    JS_SetPropertyStr(ctx, init, "cancelable", JS_TRUE);           /* step 3 */
    JS_SetPropertyStr(ctx, init, "composed", JS_TRUE);             /* step 4: "Set event's composed flag" */
    JS_SetPropertyStr(ctx, init, "view", JS_DupValue(ctx, view));  /* step 7 */
    JS_SetPropertyStr(ctx, init, "pointerId", JS_NewInt32(ctx, -1));
    t = JS_NewString(ctx, type);                                   /* step 2 */
    if (JS_IsException(t)) {
        JS_FreeValue(ctx, init);
        return t;
    }
    /* STEP 5: "If the not trusted flag is set, initialize event's isTrusted attribute to false." The one
       caller sets it, so the flag is not an argument — see the header for why widening it would be a
       parameter no step supplies. */
    ev = pointer_event_new_derived(ctx, pointer_event_proto(ctx), t, init, /*trusted*/ false);
    JS_FreeValue(ctx, t);
    JS_FreeValue(ctx, init);
    return ev;
}

/* ---- the attributes -------------------------------------------------------------------------------------- */

static JSValue js_pe_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue slots = pe_slots(ctx, this_val), v;

    DCHECK(magic >= 0 && magic < (int)(sizeof(PE_SLOT) / sizeof(PE_SLOT[0])),
           "a PointerEvent attribute was declared with a magic the slot table does not name");
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "a PointerEvent attribute was read on something that is not one");
    }
    v = JS_GetPropertyStr(ctx, slots, PE_SLOT[magic]);
    JS_FreeValue(ctx, slots);
    return v;
}

/* Pointer Events 4 §3.1's `getCoalescedEvents()` and `getPredictedEvents()` — "A method that returns the list
   of coalesced events" and "A method that returns the list of predicted events". A Web IDL `sequence<T>` RETURN
   is a NEW Array per call (Web IDL §3.2.21 "Sequences — sequence< T >": a sequence is not a reference to the
   list it came from), so the stored list is copied out rather than handed over — two calls answer two Arrays
   holding the same events, which is what every browser does and what a caller that mutates the result depends
   on. `magic` NAMES THE SLOT, so the two members are one implementation over two lists and cannot drift. */
static JSValue js_pe_get_events(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue slots = pe_slots(ctx, this_val), src, out;
    uint32_t i, n = 0;
    JSValue len;

    (void)argc; (void)argv;
    DCHECK(magic == 0 || magic == 1, "a PointerEvent event-list member was declared with a magic that names "
                                     "neither the coalesced list nor the predicted one");
    if (!JS_IsObject(slots)) {
        JS_FreeValue(ctx, slots);
        return JS_ThrowTypeError(ctx, "a PointerEvent method was called on something that is not one");
    }
    src = JS_GetPropertyStr(ctx, slots, magic == 0 ? PE_COALESCED : PE_PREDICTED);
    JS_FreeValue(ctx, slots);
    DCHECK(JS_IsObject(src), "a PointerEvent's event-list slot holds no list — Pointer Events 4 §3.1's event "
                             "constructing steps place both lists on every PointerEvent, an absent member's "
                             "being the empty one");
    out = JS_NewArray(ctx);
    if (JS_IsException(out)) {
        JS_FreeValue(ctx, src);
        return out;
    }
    len = JS_GetPropertyStr(ctx, src, "length");
    if (JS_ToUint32(ctx, &n, len) < 0)
        n = 0;
    JS_FreeValue(ctx, len);
    for (i = 0; i < n; i++)
        JS_SetPropertyUint32(ctx, out, i, JS_GetPropertyUint32(ctx, src, i));
    JS_FreeValue(ctx, src);
    return out;
}

/* ---- the constructor -------------------------------------------------------------------------------------
 *
 * `constructor(DOMString type, optional PointerEventInit eventInitDict = {})`. PointerEventInit inherits
 * MouseEventInit inherits EventModifierInit inherits UIEventInit inherits EventInit, and Web IDL §3.2.17
 * "Dictionary types" reads the INHERITED members first and each dictionary's own lexicographically among
 * THEMSELVES — which is the order this list is in, and the order a page pins by throwing from one member's
 * getter. The three inherited levels are SPLICED from ui_event.h and mouse_event.h rather than written again,
 * so this dictionary and MouseEventInit cannot state them differently; this dictionary's own fifteen sit at
 * level 4. */
static const IdlArgType PE_CTOR_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
static const IdlDictMember PE_INIT[] = {
    UI_EVENT_INIT_MEMBERS,
    EVENT_MODIFIER_INIT_MEMBERS,
    MOUSE_EVENT_INIT_MEMBERS,
    /* FOUR MEMBERS OF THIS LIST DECLARE NO DEFAULT AND THAT IS THE IDL — `altitudeAngle` and `azimuthAngle`
       here, `tiltX` and `tiltY` further down, split apart because Web IDL §3.2.17 orders a dictionary's own
       members LEXICOGRAPHICALLY and not by what they are about. Each is written `double altitudeAngle;` /
       `long tiltX;` with no `= …` where every other member of this dictionary has one, and that is what makes
       Pointer Events 4 §3.1.5's conversion expressible at all: a member with a default has no absent state, so
       a `tiltX = 0` would make "the page supplied no tilt" and "the page supplied a tilt of zero" the same
       dictionary and the conversion would have nothing to key on. */
    { "altitudeAngle", IDL_DOUBLE, false, NULL, 4 },
    { "azimuthAngle", IDL_DOUBLE, false, NULL, 4 },
    /* `sequence<PointerEvent> coalescedEvents = []`. Web IDL §3.2.15's `I` is this member's own PREDICATE and not a
       class, for a reason one step past the three idl_args.h enumerates: every Event subclass's class id is
       worn by NOTHING (it exists for the per-realm prototype slot, and every event is minted through
       JS_NewObjectProto), so a class comparison cannot tell a PointerEvent from a plain Event and no narrowing
       of one class can either. The declaration-wide brand is already spoken for by UIEventInit's
       `sourceCapabilities`, which is the other half of why: one class per declaration cannot serve two
       interfaces, and this is the spelling that does not need one. */
    { "coalescedEvents", IDL_SEQUENCE_INTERFACE, false, NULL, 4,
      .iface_is = pointer_event_is_value, .iface_name = "PointerEvent" },
    { "height", IDL_DOUBLE, false, NULL, 4, NULL, IDL_DEFAULT_ONE },
    { "isPrimary", IDL_BOOLEAN, false, NULL, 4, NULL, IDL_DEFAULT_FALSE },
    { "persistentDeviceId", IDL_LONG, false, NULL, 4, NULL, IDL_DEFAULT_ZERO },
    { "pointerId", IDL_LONG, false, NULL, 4, NULL, IDL_DEFAULT_ZERO },
    { "pointerType", IDL_DOMSTRING, false, NULL, 4, NULL, IDL_DEFAULT_STRING, "" },
    { "predictedEvents", IDL_SEQUENCE_INTERFACE, false, NULL, 4,
      .iface_is = pointer_event_is_value, .iface_name = "PointerEvent" },
    /* `float pressure` and `float tangentialPressure` — Web IDL §3.2.5 "float" and not Web IDL §3.2.7
       "double", which is a difference a page reads back: 0.1 answers 0.10000000149011612, and 1e40 is a
       TypeError where a `double` member would have taken it. */
    { "pressure", IDL_FLOAT, false, NULL, 4, NULL, IDL_DEFAULT_ZERO },
    { "tangentialPressure", IDL_FLOAT, false, NULL, 4, NULL, IDL_DEFAULT_ZERO },
    { "tiltX", IDL_LONG, false, NULL, 4 },
    { "tiltY", IDL_LONG, false, NULL, 4 },
    { "twist", IDL_LONG, false, NULL, 4, NULL, IDL_DEFAULT_ZERO },
    { "width", IDL_DOUBLE, false, NULL, 4, NULL, IDL_DEFAULT_ONE },
};

static JSValue js_pe_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor PointerEvent requires 'new'");
    DCHECK(argc >= 1, "the PointerEvent constructor body ran with no type argument — Web IDL §3.6 step 5 is the "
                      "declaration's and throws before any body is entered");
    /* An event the PAGE constructs is untrusted. */
    return pointer_event_new_derived(ctx, pointer_event_proto(ctx), argv[0],
                                     argc > 1 ? argv[1] : JS_UNDEFINED, /*trusted*/ false);
}

/* ---- install ---------------------------------------------------------------------------------------------- */

static const JSCFunctionListEntry js_pe_proto[] = {
    JS_CGETSET_MAGIC_DEF("pointerId", js_pe_get, NULL, PE_POINTER_ID),
    JS_CGETSET_MAGIC_DEF("width", js_pe_get, NULL, PE_WIDTH),
    JS_CGETSET_MAGIC_DEF("height", js_pe_get, NULL, PE_HEIGHT),
    JS_CGETSET_MAGIC_DEF("pressure", js_pe_get, NULL, PE_PRESSURE),
    JS_CGETSET_MAGIC_DEF("tangentialPressure", js_pe_get, NULL, PE_TANGENTIAL_PRESSURE),
    JS_CGETSET_MAGIC_DEF("tiltX", js_pe_get, NULL, PE_TILT_X),
    JS_CGETSET_MAGIC_DEF("tiltY", js_pe_get, NULL, PE_TILT_Y),
    JS_CGETSET_MAGIC_DEF("twist", js_pe_get, NULL, PE_TWIST),
    JS_CGETSET_MAGIC_DEF("altitudeAngle", js_pe_get, NULL, PE_ALTITUDE_ANGLE),
    JS_CGETSET_MAGIC_DEF("azimuthAngle", js_pe_get, NULL, PE_AZIMUTH_ANGLE),
    JS_CGETSET_MAGIC_DEF("pointerType", js_pe_get, NULL, PE_POINTER_TYPE),
    JS_CGETSET_MAGIC_DEF("isPrimary", js_pe_get, NULL, PE_IS_PRIMARY),
    JS_CGETSET_MAGIC_DEF("persistentDeviceId", js_pe_get, NULL, PE_PERSISTENT_DEVICE_ID),
};

void pointer_event_init(JSContext *ctx)
{
    JSClassDef d = { "PointerEvent" };

    DCHECK(!g_ready, "pointer_event_init ran twice — the interface is declared once per AGENT");
    g_key = JS_NewSymbol(ctx, "pointerEventSlots", false);
    CHECK(!JS_IsException(g_key), "the PointerEvent slot key allocation failed");
    JS_NewClassID(JS_GetRuntime(ctx), &g_pe_class);
    JS_NewClass(JS_GetRuntime(ctx), g_pe_class, &d);
    /* DECLARED HERE, at agent init, and not from the per-realm install: a fresh id minted per realm is a member
       being minted per realm, which idl_declared_before_seal exists to catch. */
    g_coalesced_id = idl_method_id(ctx, NULL, 0, js_pe_get_events, 0);
    g_predicted_id = idl_method_id(ctx, NULL, 0, js_pe_get_events, 1);
    g_ctor_stepid = idl_method_id_dict(ctx, PE_CTOR_ARGS, 2, PE_INIT,
                                       (int)(sizeof(PE_INIT) / sizeof(PE_INIT[0])), js_pe_ctor, 0);
    idl_optional_from(1);   /* `constructor(DOMString type, optional PointerEventInit eventInitDict = {})` */
    /* THE DECLARATION-WIDE CLASS, the brand of exactly ONE of this dictionary's five interface-typed members:
       UIEventInit's `sourceCapabilities`. The other four — UIEventInit's `view`, MouseEventInit's `relatedTarget`,
       and this dictionary's own two `sequence<PointerEvent>` members — state Web IDL §3.2.15's `I` as their OWN
       predicate, which idl_member_implements takes in preference to the class, so this line never decides
       for them. */
    idl_iface_brand(input_device_capabilities_class());
    g_ready = 1;
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — AND IT NAMES THE `event` ROW, NOT THIS FILE.
       core/agent_state.h: a sub-component names the row whose RELEASE gives its slots back, which for every
       Event subclass is core/platform.c's `event` row — event_init calls this init and event_free calls this
       release. */
    agent_state_flag("event", &g_ready,
                     "Pointer Events 4 §3.1 PointerEvent interface's declaration latch");
    agent_state_class("event", &g_pe_class,
                      "Pointer Events 4 §3.1 PointerEvent interface's class, held for its per-realm prototype "
                      "slot");
    agent_state_value("event", &g_key,
                      "the private Symbol Pointer Events 4 §3.1 PointerEvent interface's slot record hangs off");
    agent_state_id("event", &g_ctor_stepid,
                   "Pointer Events 4 §3.1 PointerEvent interface's `constructor(DOMString type, optional "
                   "PointerEventInit eventInitDict = {})`");
    agent_state_id("event", &g_coalesced_id,
                   "Pointer Events 4 §3.1 PointerEvent interface's `[SecureContext] sequence<PointerEvent> "
                   "getCoalescedEvents()`");
    agent_state_id("event", &g_predicted_id,
                   "Pointer Events 4 §3.1 PointerEvent interface's `sequence<PointerEvent> "
                   "getPredictedEvents()`");
    realm_declare_intrinsic(pointer_event_install_protos);
}

void pointer_event_install_protos(JSContext *ctx)
{
    JSValue proto, prev, base, ctor, global;

    DCHECK(g_ready, "a realm asked for PointerEvent before pointer_event_init declared it");
    prev = JS_GetClassProto(ctx, g_pe_class);
    DCHECK(JS_IsNull(prev), "pointer_event_install_protos ran twice in one realm — Web IDL §3.7 gives a realm ONE "
                            "PointerEvent.prototype, and a second leaves every event already chained to the "
                            "first answering out of a discarded object");
    JS_FreeValue(ctx, prev);
    /* `interface PointerEvent : MouseEvent` — THIS realm's MouseEvent.prototype, which the intrinsic declared
       before this one has already built. */
    base = mouse_event_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "PointerEvent.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "PointerEvent");
    JS_SetPropertyFunctionList(ctx, proto, js_pe_proto, (int)(sizeof(js_pe_proto) / sizeof(js_pe_proto[0])));
    /* `[SecureContext] sequence<PointerEvent> getCoalescedEvents()` — Web IDL §3.3.13 REMOVES the member in a
       non-secure realm rather than making it throw, which is exactly the branch Pointer Events 4 §9.1's own example
       is written to take (`if (e.getCoalescedEvents) { … } else { paint(e); }`). */
    idl_install_method_exposed(ctx, proto, "getCoalescedEvents", g_coalesced_id, IDL_SECURE_CONTEXT);
    /* `sequence<PointerEvent> getPredictedEvents()` carries no exposure condition at all, so it is there over
       plain http exactly as it is in a real browser. */
    idl_install_method(ctx, proto, "getPredictedEvents", g_predicted_id);
    JS_SetClassProto(ctx, g_pe_class, JS_DupValue(ctx, proto));

    /* Web IDL §3.7.1's interface object on THIS realm's global — see ui_event.c. */
    ctor = idl_step_constructor(ctx, "PointerEvent", g_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the PointerEvent interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "PointerEvent", ctor);
    JS_FreeValue(ctx, global);
}

/* THE RUNTIME, NOT A REALM — core/platform.h's release column, reached through event_free. What this gives
   back is the AGENT's: a private Symbol, a class id and this interface's member declarations; every prototype
   it built is in some realm's class-proto slot and goes with that realm. */
void pointer_event_free(JSRuntime *rt)
{
    /* NOT `if (!g_ready) return;`. core/events/event.c's event_init calls this component's init on the ONE
       declaration pass and its event_free — which has already asserted its own latch — calls this release
       unconditionally, so the test could never be true and what it could do was hide a release that left the
       latch set. */
    DCHECK(g_ready, "Pointer Events 4 §3.1 PointerEvent interface was released in an agent that never declared "
                    "it — event_init declares every Event subclass on the one unconditional pass");
    JS_FreeValueRT(rt, g_key);   /* the prototypes are the REALMS' — each is released with its context */
    g_key = JS_UNDEFINED;
    g_ready = 0;
    /* core/agent_state.h's one policy: a class id is given back like every other slot, because the id doubles
       as the init latch and a carried one names a class in a runtime that is gone. Nothing WEARS this class —
       it exists for its per-realm prototype slot, and every event in this engine is minted by
       core/events/event.c's event_make_proto through JS_NewObjectProto — so there is no finalizer and no
       gc_mark here to owe the JS_GetAnyOpaque the zeroing costs a component whose objects do wear one. */
    g_pe_class = 0;
    g_ctor_stepid = g_coalesced_id = g_predicted_id = -1;
}
