/* THE SCREEN INTERFACE — CSSOM VIEW §4.3, Blink core/frame, the output-device half of the browsing context.
 *
 * IT IS AN INTERFACE, AND IT WAS NOT ONE. This file built a PLAIN OBJECT with six own data properties and no
 * prototype of its own, which is four things ordinary page code observes and real bundles do all four:
 * `screen.width = 1` stuck (a browser ignores it in sloppy mode and throws in strict),
 * `Object.getOwnPropertyNames(screen)` listed every member where a browser lists none, `delete screen.width`
 * succeeded, and `Screen.prototype` and `window.Screen` did not exist at all — so `screen instanceof Screen`
 * threw and every device-detection shim that patches the prototype patched nothing. That is the same defect
 * Navigator had, in the same place, and it costs the same thing: coverage of the code behind those probes.
 *
 * EVERY MEMBER HERE IS THE ENVIRONMENT, so every member here is CONCOLIC with a real display's value as its
 * example. That is not a hedge, it is the whole point of the interface for this tool: `screen.width < 768` is
 * THE mobile gate, and a responsive bundle puts a different router, a different asset host and frequently a
 * different API base behind each side of it. Pinning the width to one number picks one arm and deletes the
 * other's endpoints — the same loss `navigator.userAgent` would have taken.
 *
 * There is no member of Screen that a spec fixes the way HTML fixes navigator.appName, so unlike Navigator
 * this file has no concrete half. `availWidth`/`availHeight` are separate sources from `width`/`height`: a
 * bundle that compares them is asking whether the OS reserves chrome (a taskbar), which is a different question
 * with its own two answers, and one shared source would tie the two branches together.
 *
 * THE VALUES ARE MINTED WITH THE REALM, in the realm's own record, for the reason §3.7 makes every prototype
 * per realm: a C member runs in the realm that DEFINED it (js_call_c_function takes `ctx` off the function
 * object), so one prototype shared between documents would answer every document's `screen.width` out of
 * whichever realm built it first — and a value minted lazily on the first READ is built inside whichever FLOW
 * got there first, making that flow's baseline everyone's.
 *
 * `orientation`, `isExtended` and `onchange` are honestly ABSENT — Screen Orientation's `orientation` is its
 * own interface with its own state machine, and Window Management's two are [SecureContext] members of a
 * partial interface that makes Screen an EventTarget. The IDL audit names all three until they exist. */
#include <stdbool.h>

#include "check.h"
#include "quickjs.h"
#include "solver/concolic.h"
#include "core/agent_state.h"
#include "core/frame/screen.h"
#include "core/idl_args.h"
#include "core/realm.h"

/* THE MODELLED DISPLAY'S BIT DEPTH, stated ONCE. It is `screen.colorDepth`'s example and it is also what MEDIA
   QUERIES §4.5's `color` feature reports — as bits per COLOR COMPONENT, which is this divided by the three
   components of an RGB display. Two readers of one fact, so the fact is a constant here rather than a second
   number written into the media-feature table (CLAUDE.md §per-realm: one fact answered from two places is the
   defect, whatever the places are). */
#define SCREEN_COLOR_DEPTH 24

/* THE MODELLED DISPLAY'S GEOMETRY, stated ONCE, for the same reason the depth is — see screen.h for the three
   standards that read it. A common desktop display, with 40 CSS pixels of the height reserved by the operating
   system: that is what makes `availHeight < height` a question with two sides rather than a tautology. */
#define SCREEN_WIDTH        1920.0
#define SCREEN_HEIGHT       1080.0
#define SCREEN_AVAIL_WIDTH  1920.0
#define SCREEN_AVAIL_HEIGHT 1040.0

/* THE MEMBER LIST, IN ONE PLACE, in the order §4.3's IDL declares them, because it is read THREE times — the
   magic a getter carries is an index into it, the per-realm record is filled at those indices, and the install
   walks it to define the accessors. Three hand-kept lists is a member that exists in two of them and not the
   third, which is a getter answering undefined with nothing to say so; one X-list makes that unspellable, and
   the record's completeness check below is the other half of the assertion. */
#define SCREEN_MEMBERS(X)                  \
    X(AVAIL_WIDTH,  "availWidth")          \
    X(AVAIL_HEIGHT, "availHeight")         \
    X(WIDTH,        "width")               \
    X(HEIGHT,       "height")              \
    X(COLOR_DEPTH,  "colorDepth")          \
    X(PIXEL_DEPTH,  "pixelDepth")

#define SCREEN_ENUM_ONE(id, str) SCR_##id,
#define SCREEN_NAME_ONE(id, str) str,

enum { SCREEN_MEMBERS(SCREEN_ENUM_ONE) SCR_N };
static const char *const SCR_NAME[] = { SCREEN_MEMBERS(SCREEN_NAME_ONE) };

/* A MEMBER'S SOURCE IDENTITY AND ITS DISPLAY SHAPE, spelled from the SAME X-list token the name is — the
   fourth and fifth reads of that one list, and they are here rather than composed at each use for the reason
   the list itself exists. Two consumers need them and they must agree exactly: the record builder below, whose
   concolic IS the member, and a component whose own value is a JOINT function of a Screen member and must name
   this member as one of the joint's members (core/frame/viewport.c's `screenX`/`screenY`). A `snprintf` at
   each of those is two spellings of one fact, and the one that would go wrong is silent — a joint naming
   `{screen.availWidth}` where the mint named something else composes a key no report can look up, which is
   the defaulted-field defect standing between an observed constraint and an emitted domain.
   THE SHAPE IS THE SOURCE IN BRACES, which is what concolic_new asserts of a source and what makes
   `concolic_hole_key` able to answer for it at all. */
#define SCREEN_SRC_ONE(id, str)  "screen." str,
#define SCREEN_HOLE_ONE(id, str) "{screen." str "}",
static const char *const SCR_SRC[]  = { SCREEN_MEMBERS(SCREEN_SRC_ONE) };
static const char *const SCR_HOLE[] = { SCREEN_MEMBERS(SCREEN_HOLE_ONE) };

/* THE CLASS IS THE BRAND. Web IDL §3.7.6 Attributes' check on every getter is "If jsValue does not implement
   target" — this interface declares attributes and nothing else. THE NUMBER READ §3.7.5, WHICH IS Constants,
   and the phrase quoted beside it named `esValue`, the identifier an OLDER edition used. And
   the one object per realm WEARS the class, so the check is a class-id comparison a page cannot forge. It
   carries no per-object data — the values are the realm's — so it needs no finalizer and no gc_mark. */
static JSClassID g_screen_class;
static int g_vals_slot = -1;   /* this realm's member VALUES, indexed by the enum above */
static int g_obj_slot  = -1;   /* this realm's one Screen */

int screen_color_depth(void)
{
    return SCREEN_COLOR_DEPTH;
}

double screen_width(void)        { return SCREEN_WIDTH; }
double screen_height(void)       { return SCREEN_HEIGHT; }
double screen_avail_width(void)  { return SCREEN_AVAIL_WIDTH; }
double screen_avail_height(void) { return SCREEN_AVAIL_HEIGHT; }

/* THE MEMBER THE TWO NUMBERS ABOVE ARE THE EXAMPLE OF, NAMED — see screen.h. The pair comes out of the same
   X-list row the getter's own concolic is minted from, so the joint a caller composes names the very hole this
   file mints and there is no second spelling to drift. */
void screen_avail_source(bool vertical, const char **shape, const char **src)
{
    int idx = vertical ? SCR_AVAIL_HEIGHT : SCR_AVAIL_WIDTH;

    DCHECK(shape != NULL && src != NULL,
           "CSSOM VIEW §2.3's available screen area was asked for its source identity with nowhere to put half "
           "of it — a caller composing a joint needs the SHAPE and the SOURCE together, and one without the "
           "other is a joint member whose display form and whose key would come from two different reads");
    *shape = SCR_HOLE[idx];
    *src   = SCR_SRC[idx];
}

/* WEB IDL §3.7.6 Attributes' BRAND CHECK. `Screen.prototype.width` read off a plain object is a TypeError,
   and a page
   tells that apart from `undefined` — a feature detector that probes the descriptor and applies the getter
   reads the throw as "this is a real interface". It is a real throw and not an assert for exactly that reason. */
static bool screen_brand(JSContext *ctx, JSValueConst this_val)
{
    DCHECK(g_screen_class != 0, "a Screen member ran before screen_init declared the class — the member is only "
                                "reachable through a prototype the per-realm install builds, so there is no "
                                "route here that has not run the declaration first");
    if (JS_GetClassID(this_val) == g_screen_class) return true;
    JS_ThrowTypeError(ctx, "a Screen member was reached on something that is not a Screen");
    return false;
}

/* THE HALF OF "THIS's ..." THIS ENGINE CAN ANSWER, asserted rather than assumed — the same shape, and the same
   reason, as navigator.c's. An ordinary `screen.width` arrives with the ctx of the document whose prototype it
   went through, which is the right Screen. What does NOT arrive right is one realm's getter applied to
   ANOTHER realm's Screen: the values would come out of the getter's realm, so the two objects' members would
   answer with the same numbers wearing different identities and a flow pinning one would leave the other
   unpinned. */
static void screen_assert_this_realm(JSContext *ctx, JSValueConst this_val)
{
    JSValue own = realm_value_get(ctx, g_obj_slot);
    bool same = JS_VALUE_GET_PTR(own) == JS_VALUE_GET_PTR(this_val);

    JS_FreeValue(ctx, own);
    DCHECK(same, "a Screen member was reached through ONE realm's Screen.prototype on ANOTHER realm's Screen — "
                 "answering out of the member's own realm hands back a concolic belonging to a different "
                 "document, so a branch pinned in one realm leaves the other's value unpinned. BUILD the "
                 "Screen that carries its own realm's record: give the instance the record as its class opaque "
                 "(with the finalizer, gc_mark and cow_capture_host_record contract that entails) so the member "
                 "reads it off THIS, and delete the two realm slots above");
}

/* EVERY DECLARED MEMBER'S GETTER, once. Its magic is its index; there is nothing per member to write, which is
   what stops a member from arriving with a hand-written getter that forgets the brand check. */
static JSValue js_screen_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue rec, v;

    if (!screen_brand(ctx, this_val)) return JS_EXCEPTION;
    screen_assert_this_realm(ctx, this_val);
    DCHECK(magic >= 0 && magic < SCR_N, "a Screen getter was installed with a magic that is not a member index "
                                        "— the magic IS the index into the one member X-list");
    rec = realm_value_get(ctx, g_vals_slot);
    v = JS_GetPropertyUint32(ctx, rec, (uint32_t)magic);
    JS_FreeValue(ctx, rec);
    DCHECK(!JS_IsUndefined(v), "a Screen member's realm record holds nothing at its index — the member list and "
                               "the record builder are one X-list, so an empty index means a member was "
                               "declared and never given the value its IDL says it answers with");
    return v;
}

/* ---- the per-realm record --------------------------------------------------------------------------------- */

/* An ENVIRONMENT member: opaque for control flow, carrying what a real display reports. One helper, so a member
   added later cannot quietly arrive as bare-concrete — a Screen member IS its own source, so both halves are
   spelled from ONE token, and the source is the DISPLAY rather than the document, so it is not keyed per realm
   the way viewport.c's members are (a UA presents every document of a page on one screen; it presents each in
   its own viewport).
   THE SHAPE IS THAT TOKEN IN BRACES AND THE SOURCE IDENTITY IS IT BARE — concolic_new asserts it, and this
   file used to pass one string as both. `screen.width < 768` is the responsive gate every bundle writes, and
   without a brace in the shape concolic_hole_key answered NULL, so the ordering hook filed no bound and the
   arm's endpoint reported a parameter nothing had narrowed. */
static void screen_env(JSContext *ctx, JSValueConst rec, int idx, JSValue example)
{
    JSValue v;

    DCHECK(idx >= 0 && idx < SCR_N, "a Screen environment value was minted for a non-member index");
    CHECK(!JS_IsException(example), "a Screen member's example could not be allocated");
    v = concolic_new(ctx, SCR_HOLE[idx], SCR_SRC[idx], example);
    CHECK(!JS_IsException(v), "minting a Screen environment value failed");
    JS_SetPropertyUint32(ctx, rec, (uint32_t)idx, v);
}

/* THIS REALM'S MEMBER VALUES, built with the realm. Returns an OWNED array; the caller hands it to the realm
   slot. */
static JSValue screen_build_values(JSContext *ctx)
{
    JSValue rec = JS_NewArray(ctx);
    int i;

    CHECK(!JS_IsException(rec), "the Screen member record could not be allocated");
    /* §2.3's AVAILABLE area is a SUB-AREA of the screen area — a modelled display whose available half is the
       larger of the two is not a display any UA could report, and every consumer that positions something
       inside the available area (viewport.c's client window) would then place it off the screen. Asserted here
       because this is where the four numbers are read together. */
    DCHECK(SCREEN_AVAIL_WIDTH <= SCREEN_WIDTH && SCREEN_AVAIL_HEIGHT <= SCREEN_HEIGHT,
           "the modelled Web-exposed AVAILABLE screen area is larger than the Web-exposed screen area it is "
           "part of");

    /* The modelled display, out of the one statement of it above. The examples decide what the code COMPUTES;
       the fork is what stops them deciding which code is reached.
       `width`/`height` are `long` and `colorDepth`/`pixelDepth` are `unsigned long`, which is the only thing
       §4.3's two groups of IDL differ on here. */
    screen_env(ctx, rec, SCR_WIDTH,  JS_NewInt32(ctx, (int)SCREEN_WIDTH));
    screen_env(ctx, rec, SCR_HEIGHT, JS_NewInt32(ctx, (int)SCREEN_HEIGHT));
    /* Separate sources from width/height on purpose: `screen.availHeight < screen.height` is the "is there a
       taskbar" question, and sharing one source would make that branch answer the size branch. */
    screen_env(ctx, rec, SCR_AVAIL_WIDTH,  JS_NewInt32(ctx, (int)SCREEN_AVAIL_WIDTH));
    screen_env(ctx, rec, SCR_AVAIL_HEIGHT, JS_NewInt32(ctx, (int)SCREEN_AVAIL_HEIGHT));
    screen_env(ctx, rec, SCR_COLOR_DEPTH, JS_NewInt32(ctx, SCREEN_COLOR_DEPTH));
    screen_env(ctx, rec, SCR_PIXEL_DEPTH, JS_NewInt32(ctx, SCREEN_COLOR_DEPTH));

    /* THE OTHER HALF OF THE X-LIST'S ASSERTION: every declared member got a value. A member added to the list
       and not to the builder is a getter that answers undefined, and this is where that is caught rather than
       in whichever bundle happens to read it. */
    for (i = 0; i < SCR_N; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, rec, (uint32_t)i);
        bool got = !JS_IsUndefined(v);
        JS_FreeValue(ctx, v);
        DCHECK(got, "a Screen member was declared in the X-list and given no value by the record builder");
    }
    return rec;
}

/* ---- the declaration and the per-realm install ------------------------------------------------------------ */

/* ONE PROTOTYPE, ONE INTERFACE OBJECT AND ONE SCREEN PER REALM, built WITH the realm — see the file comment for
   why that is answers and not identities. */
static void screen_install_realm(JSContext *ctx)
{
    JSValue proto, prev, global, scr;
    int i;

    prev = JS_GetClassProto(ctx, g_screen_class);
    DCHECK(JS_IsNull(prev), "screen_install_realm ran twice in one realm — everything already holding the first "
                            "Screen.prototype would answer out of a discarded object");
    JS_FreeValue(ctx, prev);

    realm_value_set(ctx, g_vals_slot, screen_build_values(ctx));

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "Screen.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "Screen");
    for (i = 0; i < SCR_N; i++)
        idl_install_accessor(ctx, proto, SCR_NAME[i], js_screen_get, i, -1);
    JS_SetClassProto(ctx, g_screen_class, JS_DupValue(ctx, proto));

    /* §3.7.1's INTERFACE OBJECT, on THIS realm's global. Screen declares no constructor, so `new Screen()` is a
       TypeError — and its PRESENCE is what `screen instanceof Screen` and every prototype-patching shim needs,
       which is exactly what this interface had none of. */
    global = JS_GetGlobalObject(ctx);
    idl_define_global_property_reference(ctx, global, "Screen", idl_interface_object(ctx, "Screen", proto));

    scr = JS_NewObjectProtoClass(ctx, proto, g_screen_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(scr), "the Window's associated Screen could not be allocated");
    /* CSSOM VIEW §4's Window extension: `[SameObject, Replaceable] readonly attribute Screen screen`. It was a
       plain data property, which is neither half of that — SameObject means every read is the one object this
       realm built, and Replaceable means an assignment REPLACES the accessor with the assigned value rather
       than being ignored, which is a distinction the corpus reads the descriptor on both sides of. */
    idl_install_replaceable_value(ctx, global, "screen", JS_DupValue(ctx, scr));
    realm_value_set(ctx, g_obj_slot, scr);
    JS_FreeValue(ctx, global);
}

void screen_init(JSContext *ctx)
{
    JSClassDef d = { "Screen" };

    DCHECK(g_vals_slot < 0, "screen_init ran twice — the class and the slots are declared once per AGENT");
    /* THE CLASS IS BOTH THE PER-REALM PROTOTYPE SLOT AND THE BRAND: the one object per realm WEARS it, so
       §3.7.6 Attributes' check is a class-id comparison and a page cannot forge one. */
    JS_NewClassID(JS_GetRuntime(ctx), &g_screen_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_screen_class, &d) == 0,
          "Screen: the per-realm prototype slot could not be declared");
    g_vals_slot = realm_value_declare(ctx, "CSSOM VIEW §4.3 the Screen's member values");
    g_obj_slot  = realm_value_declare(ctx, "CSSOM VIEW §4 the Window's associated Screen");
    agent_state_id("screen", &g_vals_slot, "§4.3's member-values realm slot, and the declaration latch");
    agent_state_id("screen", &g_obj_slot, "§4's associated-Screen realm slot");
    realm_declare_intrinsic(screen_install_realm);
}

void screen_free(void)
{
    /* The prototypes, the interface objects, the Screens and their records are the REALMS' — each is released
       with its context. What the agent holds is the two slots, and a slot id is a class id in a runtime that is
       going away with it. */
    g_vals_slot = -1;
    g_obj_slot = -1;
}
