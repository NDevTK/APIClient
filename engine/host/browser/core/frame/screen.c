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
 * THE VALUES ARE MINTED WITH THE REALM AND CARRIED BY THE SCREEN, for the reason §3.7 makes every prototype
 * per realm: a C member runs in the realm that DEFINED it (js_call_c_function takes `ctx` off the function
 * object), so one prototype shared between documents would answer every document's `screen.width` out of
 * whichever realm built it first — and a value minted lazily on the first READ is built inside whichever FLOW
 * got there first, making that flow's baseline everyone's. THIS SENTENCE READ "in the realm's own record", and
 * a realm record is exactly what the per-realm prototype does NOT fix: a getter pulled off one realm's
 * prototype and applied to another realm's Screen still runs with the FIRST realm's `ctx`, so a per-realm slot
 * answers the wrong document for the one call shape the per-realm prototype cannot reach. The values ride the
 * INSTANCE instead — see the record below, and CSSOM VIEW §2.3, whose algorithm takes "this's relevant global
 * object's browsing context" and therefore names the receiver as the input.
 *
 * `orientation`, `isExtended` and `onchange` are honestly ABSENT — Screen Orientation's `orientation` is its
 * own interface with its own state machine, and Window Management's two are [SecureContext] members of a
 * partial interface that makes Screen an EventTarget. The IDL audit names all three until they exist. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "check.h"
#include "quickjs.h"
#include "solver/concolic.h"
#include "solver/cow.h"        /* the instance's record is a component's own C state — it time-travels */
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
   the one object per realm WEARS the class, so the check is a class-id comparison a page cannot forge.
   IT CARRIES THE MEMBER VALUES, and the sentence here used to say it carried no per-object data because "the
   values are the realm's" — see the record below for why that was the defect rather than a saving. */
static JSClassID g_screen_class;

/* THE RECORD — §4.3's six member values, carried by the Screen the realm minted rather than by the realm.
 *
 * WHY THE INSTANCE AND NOT THE REALM. A C member runs in the realm that DEFINED it (js_call_c_function does
 * `ctx = p->u.cfunc.realm`), so a getter reading a per-REALM slot answers out of whichever realm's prototype
 * the call went through — not out of the receiver. §4.3 says each of the six "must return the width/height of
 * the web exposed [available] screen area", and CSSOM VIEW §2.3 "Web-exposed screen information" defines that
 * area with an algorithm whose FIRST STEP is "let target be THIS's relevant global object's browsing context".
 * The input is the receiver's global, so the answer has to be read off the receiver, and a realm slot is
 * structurally unable to do it.
 *
 * THE ASSERT THAT STOOD HERE WAS A PAGE-HELD ABORT SWITCH. It compared the receiver against this realm's own
 * Screen and DCHECKed them equal — and a receiver is PAGE-SUPPLIED INPUT, which a DCHECK may never stand on
 * (CLAUDE.md §WHOSE-BYTES-STATE-THE-VALUE). `Object.getOwnPropertyDescriptor(Screen.prototype, "width").get
 * .call(otherFrame.screen)` is two lines of ordinary JavaScript and ended the process; a FORCING solver writes
 * receivers like that constantly, because calling a member with an unusual receiver is what forcing does. What
 * Web IDL §3.7.6 asks is the BRAND and nothing beside it, and `screen_brand` already answers that with a
 * TypeError — so the realm comparison was not a weaker check, it was a different question with no standing.
 *
 * AND ITS STATED REASON WAS WRONG, WHICH IS RECORDED RATHER THAN QUIETLY DROPPED because the next reader will
 * otherwise re-derive it. It said answering out of the member's own realm "hands back a concolic belonging to
 * a different document, so a branch pinned in one realm leaves the other's value unpinned". The two realms'
 * values share one identity: `SCR_HOLE`/`SCR_SRC` are string literals off the one X-list, so every realm mints
 * `{screen.width}`, and solver/concolic.h's `concolic_hole_key` is the shape with the braces removed — one key,
 * looked up the same from either document. A pin in one realm therefore reaches the other and the sentence
 * describes a loss that cannot happen. core/layout/used_value.c states the same fact from the other side and
 * REQUIRES it ("that row's source key must be screen.c's own `screen.width` rather than a per-document one"),
 * so this file and that one disagreed about one mechanism and that one was right. The OBLIGATION the assert
 * named survives its reason: §2.3's algorithm reads the receiver's global, and its own third arm — "the area
 * of the VIEWPORT in css pixels" — is a quantity that genuinely differs between a top-level document and a
 * child navigable, so two realms' answers are free to differ even though their identities do not.
 *
 * NO JSContext IN THE RECORD, unlike core/timing/performance.c's. That one holds a realm because HR-TIME §4's
 * time origin is keyed by context; the six members here are read with `JS_GetPropertyUint32` off an ordinary
 * array, which is a heap operation any context of this runtime performs identically — and every cross-realm
 * receiver that can reach a member is SAME-AGENT, so there is one heap. A field that would need a global to
 * hold it up is a field this record does not have. */
typedef struct {
    JSValue vals;   /* §4.3's six members, indexed by the enum above. OWNED. */
} Screen;

/* THE ONE STATEMENT OF WHAT THE RECORD OWNS — the same list the finalizer frees and the gc_mark walks, which
   is why all three are written here together: a field added to one and not the others is the defect this
   arrangement exists to make unspellable. */
static const uint16_t SCREEN_VAL_OFF[] = { (uint16_t)offsetof(Screen, vals) };
static const CowRecord SCREEN_REC = { sizeof(Screen), SCREEN_VAL_OFF, 1 };

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

/* THE ACCESSOR EVERY MEMBER REACHES THE RECORD THROUGH, and the capture is IN it for solver/cow.h's reason: a
   record a flow has REACHED is one it may write, the delta dedups to one entry per (flow, object), and there is
   then no write site left to miss. The cost is bounded by §4's own shape rather than argued — there is exactly
   ONE Screen per realm, so this can add at most one delta entry per realm per flow.
   NOT the brand: a brand check is a QUESTION, asked of values that are not Screens at all, and a question must
   not capture. `screen_brand` is asked first and separately, one line above every caller. */
static Screen *screen_rec(JSValueConst v)
{
    Screen *s = g_screen_class ? JS_GetOpaque(v, g_screen_class) : NULL;

    if (s) cow_capture_host_record(v, s, &SCREEN_REC);
    return s;
}

/* JS_GetAnyOpaque and not JS_GetOpaque in BOTH of these, deliberately — core/agent_state.h states the rule and
   this file is one of the cases it was written for: the collector dispatched here THROUGH the class, and
   screen_free sets `g_screen_class` back to 0 before the runtime is torn down, so reading the static would
   make a finalizer running after that column answer NULL for a record that is there and leak it silently. */
static void screen_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    Screen *s = JS_GetAnyOpaque(val, &id);

    (void)id;
    DCHECK(s != NULL, "a Screen was finalized with no record — §4's object has exactly one mint and it attaches "
                      "the record with nothing in between that could collect");
    JS_FreeValueRT(rt, s->vals);
    free(s);
}

static void screen_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSClassID id = 0;
    Screen *s = JS_GetAnyOpaque(val, &id);

    (void)id;
    DCHECK(s != NULL, "a Screen was marked with no record — its member array is a counted reference and an "
                      "unmarked child keeps the internal count gc_decref subtracts, so gc_scan reads it as "
                      "rooted from OUTSIDE the heap and it is never collected at all");
    JS_MarkValue(rt, s->vals, mark_func);
}

/* EVERY DECLARED MEMBER'S GETTER, once. Its magic is its index; there is nothing per member to write, which is
   what stops a member from arriving with a hand-written getter that forgets the brand check.
   THE ANSWER IS THE RECEIVER'S — see the record above. CSSOM VIEW §2.3's algorithm takes "this's relevant
   global object's browsing context", so reading the values off THIS is what makes this member §4.3 rather than
   a member that answers for whichever realm the page reached the getter through. */
static JSValue js_screen_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    Screen *s;
    JSValue v;

    if (!screen_brand(ctx, this_val)) return JS_EXCEPTION;
    DCHECK(magic >= 0 && magic < SCR_N, "a Screen getter was installed with a magic that is not a member index "
                                        "— the magic IS the index into the one member X-list");
    s = screen_rec(this_val);
    DCHECK(s != NULL, "a Screen reached a member with no record — the brand is the class and the mint attaches "
                      "the record before the object leaves it, so a branded object without one came from a "
                      "second mint that does not exist");
    v = JS_GetPropertyUint32(ctx, s->vals, (uint32_t)magic);
    DCHECK(!JS_IsUndefined(v), "a Screen member's record holds nothing at its index — the member list and the "
                               "record builder are one X-list, so an empty index means a member was declared "
                               "and never given the value its IDL says it answers with");
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

/* THIS REALM'S MEMBER VALUES, built with the realm. Returns an OWNED array; the caller hands it to the
   Screen's own record, which is what frees it (screen_finalizer). */
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
    Screen *s;
    int i;

    prev = JS_GetClassProto(ctx, g_screen_class);
    DCHECK(JS_IsNull(prev), "screen_install_realm ran twice in one realm — everything already holding the first "
                            "Screen.prototype would answer out of a discarded object");
    JS_FreeValue(ctx, prev);

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
    /* THE RECORD, ATTACHED BEFORE THE OBJECT LEAVES THIS FUNCTION — which is what `screen_rec`'s "a branded
       object without one came from a second mint that does not exist" rests on, and there is no second mint.
       The values are built HERE rather than on first read for the reason the file comment gives: a value minted
       lazily is built inside whichever FLOW got there first, making that flow's baseline everyone's. */
    s = calloc(1, sizeof *s);
    CHECK(s != NULL, "this realm's Screen record could not be allocated");
    s->vals = screen_build_values(ctx);
    JS_SetOpaque(scr, s);
    /* CSSOM VIEW §4's Window extension: `[SameObject, Replaceable] readonly attribute Screen screen`. It was a
       plain data property, which is neither half of that — SameObject means every read is the one object this
       realm built, and Replaceable means an assignment REPLACES the accessor with the assigned value rather
       than being ignored, which is a distinction the corpus reads the descriptor on both sides of.
       THE PROPERTY IS THE ONLY OWNER, and [SameObject] is satisfied BY it: the value is held by the member
       rather than by a realm slot beside it, so there is no second reference that could answer a read. The slot
       that stood here existed only to feed the realm comparison this diff deleted, and keeping it would have
       been a second owner of one object with no reader — which is the write-with-no-reader half of
       CLAUDE.md §A-FIELD-A-CONSUMER-DEFAULTS. */
    idl_install_replaceable_value(ctx, global, "screen", scr);   /* CONSUMED on every path, refusal included */
    JS_FreeValue(ctx, global);
}

void screen_init(JSContext *ctx)
{
    JSClassDef d = { "Screen", .finalizer = screen_finalizer, .gc_mark = screen_gc_mark };

    DCHECK(g_screen_class == 0, "screen_init ran twice — the class is declared once per AGENT");
    /* THE CLASS IS BOTH THE PER-REALM PROTOTYPE SLOT AND THE BRAND: the one object per realm WEARS it, so
       §3.7.6 Attributes' check is a class-id comparison and a page cannot forge one. It now also carries the
       member values, so it has a finalizer and a gc_mark — see the record above. */
    JS_NewClassID(JS_GetRuntime(ctx), &g_screen_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_screen_class, &d) == 0,
          "Screen: the per-realm prototype slot could not be declared");
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED UNDER ITS OWN ROW — `screen` is a row on
       core/platform.c's list with `r_screen` in its release column, and the two realm-slot declarations that
       stood here were all it had. Deleting those slots without this line would have left the row's release
       column and this component's declarations BOTH empty, which core/frame/bar_prop.c records as the state in
       which a pairing reads two silences as one another's confirmation — so the class id below is declared
       here because this diff is what made it the only thing left to declare. */
    agent_state_class("screen", &g_screen_class,
                      "CSSOM VIEW §4.3 The Screen Interface: Screen's per-realm prototype slot and brand");
    realm_declare_intrinsic(screen_install_realm);
}

void screen_free(void)
{
    /* The prototypes, the interface objects, the Screens and their records are the REALMS' — each is released
       with its context, and a Screen's record goes with it through screen_finalizer. What the AGENT holds is
       the class id, and this line is the one that gives it back.
       A CLASS ID IS A REGISTRATION IN A RUNTIME, so a carried one names a class in a runtime that is gone:
       JS_NewClassID hands a NON-ZERO slot back unchanged rather than allocating, so a second agent's
       screen_init would call JS_NewClass on the FIRST agent's number — a number the new runtime's own
       allocator never issued and will issue to whichever component asks next. core/frame/bar_prop.c states the
       same reasoning at its own release and is where this was read from. screen_finalizer reads its record
       through JS_GetAnyOpaque precisely because this line runs before the collection that reaches it. */
    g_screen_class = 0;
}
