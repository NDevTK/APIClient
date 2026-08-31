/* THE Location OF A DOCUMENT IN ANOTHER WASM INSTANCE — see remote_location.h for why this is its own
 * component and not a branch inside core/frame/location.c.
 *
 * WHAT THIS OBJECT IS FOR, IN ONE SENTENCE A PAGE WRITES: `otherWindow.location.href = url` is how one document
 * navigates another across an origin boundary, and §7.2.1 puts `location` on CrossOriginProperties(Window)
 * for that single purpose. Until this component existed the member had nowhere to go — core/frame/window_proxy.c
 * answered `location` out of proxy_realm, whose first assert is that the navigable's active document is this
 * agent's, so a cross-origin read ABORTED at the boundary. That abort was the right crash (§Offensive
 * programming: a capability that should exist and does not), and this is the capability it named.
 *
 * IT CARRIES A DOCUMENT NAME AND NOTHING ELSE, WHICH IS THE WHOLE OF ITS STATE. §7.2.4 says "a Location object
 * has an associated url, which is this Location object's relevant Document's URL"; that Document is in another
 * instance, so there is no url to hold and every member that would read one is outside §7.2.1.3.1's two-entry
 * list and is a SecurityError. What is left is the identity of the Document the two surviving members act on,
 * and a document crosses instances BY NAME (solver/world.h — a `uint32_t doc` is this instance's handle into
 * its own table and means a different document in the peer's), so the record is that handle and is IMMUTABLE:
 * nothing in this file writes it after the mint. That is why there is no cow_capture_host_record here and no
 * gc_mark — the record holds no JSValue and no flow can change it, so there is nothing for a per-flow delta to
 * capture. A navigation of the peer's navigable does not change it either: it is the NAVIGABLE this object
 * belongs to, and core/frame/window_proxy.c is what tracks which document is active in one.
 *
 * FIVE OF §7.2.4's TEN INTERNAL METHODS ARE THIS FILE, AND THE OTHER FIVE HAVE NO HOOK TO BE.
 * §7.2.4.1 [[GetPrototypeOf]] returns null, §7.2.4.5 [[GetOwnProperty]] is §7.2.1.3.4 then §7.2.1.3.2,
 * §7.2.4.6 [[DefineOwnProperty]] and §7.2.4.9 [[Delete]] throw a SecurityError, and §7.2.4.10
 * [[OwnPropertyKeys]] is §7.2.1.3.7's six names. The five that are not here are the five quickjs's exotic
 * table has no slot for — §7.2.4.2's [[SetPrototypeOf]], §7.2.4.3's [[IsExtensible]] and §7.2.4.4's
 * [[PreventExtensions]], which core/frame/location.c already records as the same shape, plus §7.2.4.7 [[Get]]
 * and §7.2.4.8 [[Set]], whose cross-origin arms are §7.2.1.3.5 CrossOriginGet and §7.2.1.3.6 CrossOriginSet.
 *
 * WHAT THE MISSING [[Get]] COSTS, AND WHERE THIS FILE PAYS IT. §7.2.1.3.5's step 6 throws a SecurityError when
 * the descriptor has NO getter, and the one member that has none is `href` — §7.2.1.3.1 gives its entry
 * [[NeedsGetter]] FALSE. An ordinary [[Get]] over a getterless accessor answers `undefined` instead, and
 * `undefined` is exactly the answer §7.2.1.3.2's own last step exists to forbid: a page tells "you may not
 * look" from "there is no such member" with a try/catch, and that is how it feature-detects a cross-origin
 * object at all. So this file installs a REAL getter that throws the SecurityError, and the READ is then the
 * standard's answer. What diverges is one reflection detail — `Object.getOwnPropertyDescriptor(otherW.
 * location, "href").get`, which §7.2.1.3.4 leaves undefined and which reads here as a function — and, in the
 * same way, a WRITE to one of §7.2.1.3.2's four fallback names, where an ordinary [[Set]] over a non-writable
 * data property is a strict-mode TypeError and §7.2.1.3.6's last step is a SecurityError. Both are the same
 * one gap: a class-level [[Get]]/[[Set]] hook, which is a quickjs-side capability and not this component's.
 *
 * THE TWO SURVIVING MEMBERS BOTH NAVIGATE, AND NEITHER CAN YET. §7.2.4's href setter and `replace` end in
 * Location-object navigate, which for this object is a navigation of a navigable ANOTHER instance holds — see
 * each body for the record that has to cross and who may stamp its initiator origin. They CRASH there rather
 * than storing a string, because a setter that stored one would make a page believe it had navigated.
 *
 * WHY THE MEMBERS ARE ON A PER-REALM SURFACE AND THE DESCRIPTORS ARE ANSWERED AS THIS OBJECT'S OWN.
 * §7.2.1.3.4 builds "an anonymous built-in function, created in the CURRENT realm" per (current settings, O's
 * settings, P) and caches it in O's [[CrossOriginPropertyDescriptorMap]]; this engine's stand-in is one
 * function object per REALM, held on the class prototype, which is the same stand-in core/frame/window_proxy.c
 * makes for the Window half. What is different here — and better — is that [[GetOwnProperty]] answers those
 * names ITSELF instead of returning "no own property" and letting a prototype walk find them: §7.2.4.10's key
 * list may then name exactly the six §7.2.1.3.7 names, every one of which [[GetOwnProperty]] answers, and the
 * pairing is asserted rather than claimed. A key a list reports and [[GetOwnProperty]] refuses (or the
 * reverse) is one object with two member surfaces, which is the defect the WindowProxy's own cross-origin
 * [[OwnPropertyKeys]] still crashes rather than commit. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "check.h"
#include "quickjs.h"
#include "solver/world.h"   /* a document crosses BY NAME, and "this agent holds it" is that registry's answer */
#include "core/agent_state.h"
#include "core/frame/remote_location.h"
#include "core/idl_args.h"
#include "core/realm.h"

/* HTML §7.2.1.3.1 CrossOriginProperties ( O ), the Location arm, verbatim: « { [[Property]]: "href",
   [[NeedsGetter]]: false, [[NeedsSetter]]: true }, { [[Property]]: "replace" } ». Declared in this component's
   header rather than beside §7.2.4's member table, because this is the object the list DESCRIBES; see there
   for who else reads it. The ORDER is the standard's and it is what §7.2.1.3.7's key list reports. */
/* §7.2.1.3.1's Location arm VERBATIM, records and all — see remote_location.h for why the flags are on the
   one list and not a second one beside it. `replace` is written `{ [[Property]]: "replace" }` in the standard,
   which is an entry with NEITHER flag: an operation, and the shape §7.2.1.1's "type is method and e has
   neither [[NeedsGetter]] nor [[NeedsSetter]]" lets through. */
const CrossOriginProperty LOCATION_CROSS_ORIGIN[LOCATION_XO_N] = {
    { "href",    /* [[NeedsGetter]] */ false, /* [[NeedsSetter]] */ true  },
    { "replace", /* [[NeedsGetter]] */ false, /* [[NeedsSetter]] */ false },
};

/* §7.2.1.3.2 CrossOriginPropertyFallback ( P )'s FOUR NAMES — `then` (so a cross-origin Location is not
   mistaken for a thenable and awaited) and the three well-known symbols an engine touches while doing
   something else. They are answered as real property descriptors of undefined rather than by falling off a
   chain, because §7.2.1.3.7 lists them among the own keys. */
#define RL_FALLBACK_N 4

/* THIS COMPONENT'S NAME, spelled once — core/platform.c's row and every slot it declares to
   core/agent_state.h. Neither spelling is checked by the compiler, and a mismatch is not a weaker check but a
   check that is never RUN, sitting behind a message about a different repair entirely. */
#define RL_COMPONENT "remote_location"

static JSClassID g_rl_class;
static JSRuntime *g_rl_rt;
static JSAtom g_member_atom[LOCATION_XO_N];
static JSAtom g_fallback_atom[RL_FALLBACK_N];
static int g_rl_href_setter = -1, g_rl_replace = -1;

/* THE RECORD — see the header: one document name, written at the mint and never again. */
typedef struct { uint32_t doc; } RemoteLocData;

/* THE ONE Location PER PEER DOCUMENT. The row BORROWS, exactly as core/frame/window_proxy.c's remote-navigable
   table does and for the same reason: an owning row would be an immortal root holding an object for every
   peer document a forced-execution frontier ever reached, while a borrowed one exists only while something
   else holds the object and the finalizer takes it out at the moment the collector takes the object. So the
   table is exactly the set of live cross-origin Locations and nothing is kept alive by being named. */
typedef struct { uint32_t doc; JSValueConst obj; } RemoteLocRow;
static RemoteLocRow *g_rows;
static int g_rows_n, g_rows_cap;

/* ---- the record, and §3.7.6's receiver ------------------------------------------------------------------- */

/* THE RECORD AS A CLASS HOOK SEES IT. JS_GetAnyOpaque AND NOT JS_GetOpaque(obj, g_rl_class): core/agent_state.h
   states the rule and the reason — the collector and this component's release can run in either order, and a
   lookup against an id a release has already zeroed answers NULL for every live object of the class. The
   collector dispatched through the class, so the id is a fact it already has. Side-effect-free, which is what
   lets the hooks below assert on it. */
static void *rl_opaque(JSValueConst obj)
{
    JSClassID cid = 0;

    return JS_GetAnyOpaque(obj, &cid);
}

/* §7.2.1.1 Integration with IDL's step 1, THE LOCATION HALF — see remote_location.h for why only the
   cross-origin arm needs a brand at all. It is asked of EVERY receiver of EVERY declared member, so it reads
   two words and allocates nothing.
   THE DCHECK IS core/frame/window_proxy.c's window_proxy_is VERBATIM AND FOR ITS REASON: a `g_rl_class != 0 &&`
   here would make "this component is not declared" and "that object is not a cross-origin Location" ONE value,
   and after remote_location_free gives the id back every live object of this class would report itself as
   something else — silently, at the one site that decides whether a cross-origin access is refused. This
   component is a row on core/platform.c's declare column, so every agent has run it before any member can be
   called. */
bool remote_location_is(JSValueConst v)
{
    DCHECK(g_rl_class != 0,
           "§7.2.1.1 Integration with IDL's step 1 asked whether an object is a cross-origin Location before "
           "remote_location_init declared the class or after remote_location_free gave it back — with no class "
           "there is no answer, and returning `not a Location` would say the access is PERMITTED");
    return JS_GetOpaque(v, g_rl_class) != NULL;
}

/* Web IDL §3.7.6 "Attributes"' and §3.7.7 "Operations"' brand check for this object, answered as the record or
   NULL with a TypeError pending. The two members below are reached ONLY through the descriptors §7.2.4.5
   answers with, so `this` is this object in every ordinary spelling — but a page may pull a descriptor's
   `.set` out and apply it, which is the same reach §7.2.3.5 makes ordinary for the Window half, and a member
   of a cross-origin Location applied to something else must throw rather than act on a document it names. */
static RemoteLocData *rl_record(JSContext *ctx, JSValueConst this_val)
{
    RemoteLocData *d;

    DCHECK(g_rl_class != 0,
           "a cross-origin Location member ran before remote_location_init declared the class — the member is "
           "installed by the per-realm surface this component registers, so no route here has skipped it");
    d = JS_GetOpaque(this_val, g_rl_class);
    if (!d) {
        JS_ThrowTypeError(ctx, "a Location member was reached on something that is not a Location");
        return NULL;
    }
    DCHECK(!world_doc_hosted(d->doc),
           "a cross-origin Location names a Document THIS AGENT HOLDS — the object exists only for the arm "
           "§7.2.1.3.3 IsPlatformObjectSameOrigin answers false for, and a hosted Document is answered by its "
           "own realm's Location (core/frame/location.h), so this object is filtering a document it should "
           "never have been minted for");
    return d;
}

/* ---- §7.2.1.3.1's two members --------------------------------------------------------------------------- */

/* §7.2.4's `href` GETTER ACROSS AN ORIGIN — §7.2.1.3.1 gives the entry [[NeedsGetter]] FALSE, so §7.2.1.3.5
   CrossOriginGet reaches its step 6 ("if getter is undefined, then throw a SecurityError") and the read
   throws. This function IS that step; see the file header for why it is a getter at all rather than an absent
   half of the descriptor, and for the one reflection detail that costs. */
static JSValue rl_href_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!rl_record(ctx, this_val)) return JS_EXCEPTION;
    return JS_ThrowDOMException(ctx, "SecurityError",
                                "the origins do not permit reading the href of that Location");
}

/* THE NAVIGATION BOTH SURVIVING MEMBERS END IN, and the one thing this component does not have. Stated once
   and CRASHED at both call sites rather than described in a comment, because §Fix the ROOT is explicit that a
   comment is not a valid follow-up.
   WHAT IS ALREADY DECIDED HERE AND MUST TRAVEL WITH THE RECORD: §7.2.4's href setter step 2 and `replace`
   step 3 both parse the given value "relative to the ENTRY settings object's API base URL", which is the
   INITIATING document's — this one — and not the target's. §scheduler's rule about an operation that becomes
   a work item is exactly this case: the URL is resolved on the line that made the assignment, and what
   crosses is the ABSOLUTE address, never the string the page wrote. */
static JSValue rl_navigate_unbuilt(JSContext *ctx, const RemoteLocData *d, const char *who)
{
    (void)ctx;
    /* THE MEMBER AND THE TARGET DOCUMENT ARE IN THE CRASH, because the two members reach one algorithm and a
       reader standing at a `@WHY` that named neither would not know which spelling a page used nor which
       peer it was aimed at. */
    DFAILF("HTML §7.2.4's Location-object navigate reached through %s for document %s, whose Document is in "
           "ANOTHER WASM INSTANCE — the one operation §7.2.1 puts a Location on CrossOriginProperties for. "
           "Every edge but the record exists: a document crosses BY NAME (solver/world.h's world_doc_name), a "
           "flow SUSPENDS on a cross-instance operation and resumes byte-identically (core/frame/"
           "window_proxy.c's step machine is the worked example), and core/frame/remote_op.c is where a peer "
           "PERFORMS one by running a program in the named document's own realm. What has to be built is a "
           "`location.navigate` record carrying (target document name, this flow's world and its ancestry, the "
           "URL this document already RESOLVED, and §7.4.2.2's history handling), with its INITIATOR ORIGIN "
           "stamped by the trusted zone and never by this engine (SECURITY.md — a forgeable initiator defeats "
           "every origin check a page makes), plus the OPS row that runs §7.4.2.2's navigate for the peer's "
           "own navigable", who, world_doc_name(d->doc));
    return JS_UNDEFINED;
}

/* §7.2.4's `href` SETTER — the member §7.2.1's whole Location entry exists for. */
static JSValue rl_href_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    const RemoteLocData *d = rl_record(ctx, this_val);

    (void)val; (void)magic;
    if (!d) return JS_EXCEPTION;
    return rl_navigate_unbuilt(ctx, d, "the href setter");
}

/* §7.2.4's `replace(url)` — the other entry, and the standard states twice that it "intentionally has no
   security check", which is why it survives the filter at all. */
static JSValue rl_replace(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    const RemoteLocData *d = rl_record(ctx, this_val);

    (void)argc; (void)argv; (void)magic;
    if (!d) return JS_EXCEPTION;
    return rl_navigate_unbuilt(ctx, d, "replace()");
}

/* ---- §7.2.4's cross-origin internal methods --------------------------------------------------------------- */

/* §7.2.1.3.4's descriptor FOR ONE OF THE TWO NAMES, read off this realm's surface object. `desc` filled and
   OWNED when non-NULL; 1 when the surface answers, and it must, which is what the assert says.
   THE FLAGS ARE §7.2.1.3.4's AND NOT THE SURFACE'S: [[Enumerable]] FALSE and [[Configurable]] TRUE for both
   shapes, and non-writable for the operation's value. The standard gives its own reasons in the same section
   — configurable "to preserve the invariants of the essential internal methods" since a navigation can change
   what the property means, and non-enumerable "for compatibility with existing web content" despite
   mismatching the same-origin behaviour — so neither may be inherited from however the member was installed. */
static int rl_member_desc(JSContext *ctx, JSPropertyDescriptor *desc, JSAtom prop, int member)
{
    JSValue surface = JS_GetClassProto(ctx, g_rl_class);
    JSPropertyDescriptor d;
    int has;

    DCHECK(JS_IsObject(surface),
           "a cross-origin Location was read in a realm that never ran this component's per-realm install — "
           "§7.2.1.3.4's functions are created in the CURRENT realm, so a realm without a surface has none of "
           "them to answer with");
    has = JS_GetOwnSlotDesc(ctx, &d, surface, prop);
    JS_FreeValue(ctx, surface);
    DCHECK(has > 0,
           "§7.2.1.3.1's CrossOriginProperties(Location) names a member this component's per-realm surface "
           "does not install — the list and the install are the two halves of one filter, so a name on one "
           "and not the other is a property this object REPORTS and cannot answer");
    if (has < 0) return -1;
    if (!has) return 0;
    /* THE TWO SHAPES ARE THE STANDARD'S TWO BRANCHES, asserted rather than assumed: an entry with
       [[NeedsGetter]]/[[NeedsSetter]] is an ACCESSOR and one with neither is the property itself, so a `href`
       that arrived as a data property (or a `replace` that arrived as an accessor) would be answered under
       the wrong branch of §7.2.1.3.4 with nothing to say so. */
    /* AND THE BRANCH IS READ OFF THE ENTRY'S OWN FLAGS, not off its index. `member == LOCATION_XO_HREF` was
       the same claim written as a constant, which is true only while the list has exactly these two rows in
       exactly this order — and the list is §7.2.1.3.1's, not this file's. The flags say it directly: an entry
       with either [[NeedsGetter]] or [[NeedsSetter]] IS an accessor, one with neither is the property itself,
       which is also the split §7.2.1.1 Integration with IDL decides `type` against. */
    DCHECK((LOCATION_CROSS_ORIGIN[member].needs_get || LOCATION_CROSS_ORIGIN[member].needs_set) ==
           ((d.flags & JS_PROP_GETSET) != 0),
           "a member of §7.2.1.3.1's CrossOriginProperties(Location) is installed with the wrong SHAPE — "
           "`href` is the entry with [[NeedsSetter]] and must be an accessor, `replace` has neither getter nor "
           "setter and must be the operation's own property");
    if (!desc) {
        JS_FreeValue(ctx, d.value);
        JS_FreeValue(ctx, d.getter);
        JS_FreeValue(ctx, d.setter);
        return 1;
    }
    *desc = d;
    desc->flags = (d.flags & JS_PROP_GETSET) ? (JS_PROP_GETSET | JS_PROP_CONFIGURABLE) : JS_PROP_CONFIGURABLE;
    return 1;
}

/* HTML §7.2.4.5 [[GetOwnProperty]] ( P ), CROSS-ORIGIN ARM — "let property be CrossOriginGetOwnPropertyHelper
 * (this, P); if property is not undefined, then return property; return ? CrossOriginPropertyFallback(P)".
 * There is no same-origin arm here: this object exists only for the branch §7.2.1.3.3 answers false for.
 *
 * AN ARRAY INDEX IS NOT A CASE. §7.2.1.3.1's note — "indexed properties do not need to be safelisted in this
 * algorithm, as they are handled directly by the WindowProxy object" — is about the Window; a Location has no
 * indexed access at all, so an index falls through to §7.2.1.3.2's throw exactly as any other name does. */
static int rl_get_own(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom prop)
{
    int i;

    DCHECK(rl_opaque(obj) != NULL,
           "a cross-origin Location class hook ran on an object carrying no document record");
    /* A NAME TABLE THAT WAS NEVER FILLED COMPARES EVERY PROPERTY AGAINST JS_ATOM_NULL and matches none, which
       refuses §7.2.1.3.1's whole list rather than answering it — a filter that throws for everything passes
       any test that only checks the throw. */
    DCHECK(g_member_atom[0] != JS_ATOM_NULL && g_fallback_atom[0] != JS_ATOM_NULL,
           "a cross-origin Location was read before this agent interned §7.2.1.3.1 CrossOriginProperties ( O )'s "
           "and §7.2.1.3.2 CrossOriginPropertyFallback ( P )'s names");

    for (i = 0; i < LOCATION_XO_N; i++)                       /* §7.2.1.3.4 */
        if (prop == g_member_atom[i]) return rl_member_desc(ctx, desc, prop, i);

    for (i = 0; i < RL_FALLBACK_N; i++)                       /* §7.2.1.3.2 step 1 */
        if (prop == g_fallback_atom[i]) {
            if (!desc) return 1;
            desc->flags = JS_PROP_CONFIGURABLE;   /* undefined, non-writable, non-enumerable, CONFIGURABLE */
            desc->value = JS_UNDEFINED;
            desc->getter = JS_UNDEFINED;
            desc->setter = JS_UNDEFINED;
            return 1;
        }

    /* §7.2.1.3.2 step 2. It is a THROW and not `undefined`, and the difference is the whole point of the
       object: `undefined` says "that document has no such member", the SecurityError says "you may not
       look", and a try/catch around the read is how a page tells a cross-origin object apart at all. */
    JS_ThrowDOMException(ctx, "SecurityError",
                         "the origins do not permit reading this member of that Location");
    return -1;
}

/* HTML §7.2.4.10 [[OwnPropertyKeys]] ( ) — §7.2.1.3.7 CrossOriginOwnPropertyKeys: the two names of
 * CrossOriginProperties(Location) followed by "then", %Symbol.toStringTag%, %Symbol.hasInstance% and
 * %Symbol.isConcatSpreadable%, in that order.
 *
 * THE LIST AND §7.2.4.5's ANSWERS ARE ONE FACT, and this is where they are tied together. quickjs merges what
 * this returns with the object's ordinary own keys, of which this object has none — it is built by this file
 * and nothing ever defines a property on it (§7.2.4.6 throws) — so the six below are the whole list. */
static int rl_own_keys(JSContext *ctx, JSPropertyEnum **ptab, uint32_t *plen, JSValueConst obj)
{
    const uint32_t n = LOCATION_XO_N + RL_FALLBACK_N;
    JSPropertyEnum *tab;
    uint32_t i;

    DCHECK(rl_opaque(obj) != NULL,
           "a cross-origin Location class hook ran on an object carrying no document record");
    tab = js_malloc(ctx, sizeof(*tab) * (size_t)n);
    if (!tab) return -1;
    for (i = 0; i < n; i++) {
        JSAtom a = i < LOCATION_XO_N ? g_member_atom[i] : g_fallback_atom[i - LOCATION_XO_N];

        /* EVERY KEY THIS LISTS IS ONE §7.2.4.5 ANSWERS. A key reported by `Object.getOwnPropertyNames` whose
           `Object.getOwnPropertyDescriptor` is undefined (or throws) is one object with two member surfaces,
           which is precisely the state the two tables above exist to make impossible. Side-effect-free: the
           `desc`-less form of the hook reads the surface and frees what it read. */
        DCHECK(rl_get_own(ctx, NULL, obj, a) == 1,
               "§7.2.1.3.7's key list names a property §7.2.4.5 does not answer for — the two are one filter, "
               "so a key here that [[GetOwnProperty]] refuses is a name this object reports and cannot "
               "describe");
        tab[i].is_enumerable = false;   /* §7.2.1.3.4 and §7.2.1.3.2 both give [[Enumerable]] false */
        tab[i].atom = JS_DupAtom(ctx, a);
    }
    *ptab = tab;
    *plen = n;
    return 0;
}

/* HTML §7.2.4.6 [[DefineOwnProperty]] ( P, Desc ), cross-origin arm: "throw a SecurityError DOMException".
   Without a hook the define SUCCEEDED, quietly, and the object then carried an own property of the page's
   making that §7.2.4.5's filter knows nothing about. */
static int rl_define_own(JSContext *ctx, JSValueConst obj, JSAtom prop, JSValueConst val,
                         JSValueConst getter, JSValueConst setter, int flags)
{
    (void)obj; (void)prop; (void)val; (void)getter; (void)setter; (void)flags;
    JS_ThrowDOMException(ctx, "SecurityError",
                         "the origins do not permit defining a member of that Location");
    return -1;
}

/* HTML §7.2.4.9 [[Delete]] ( P ), cross-origin arm: the same throw. */
static int rl_delete(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    (void)obj; (void)prop;
    JS_ThrowDOMException(ctx, "SecurityError",
                         "the origins do not permit deleting a member of that Location");
    return -1;
}

/* HTML §7.2.4.1 [[GetPrototypeOf]] ( ): "if IsPlatformObjectSameOrigin(this) is true, then return !
   OrdinaryGetPrototypeOf(this). Return null." Always the second sentence here.
   THE SHAPE LINK STILL POINTS AT THE SURFACE, and that is not a contradiction: quickjs's ordinary lookups walk
   the stored link rather than asking this hook (JS_GetPrototype's own comment states it from that side), and
   no lookup ever gets there — §7.2.4.5 above answers every one of §7.2.1.3.7's six names itself and THROWS for
   every other, so there is no name whose resolution continues past this object. */
static JSValue rl_get_prototype(JSContext *ctx, JSValueConst obj)
{
    (void)ctx; (void)obj;
    return JS_NULL;
}

/* No page code: the surface read is an own-slot read of an ordinary object this file built, and the throw
   builds a DOMException out of the engine's own intrinsics. That is what lets the engine's own accessor walks
   run these hooks from C with no flow base under them. */
static const JSClassExoticMethods RL_EXOTIC = {
    .get_own_property = rl_get_own,
    .get_own_property_names = rl_own_keys,
    .delete_property = rl_delete,
    .define_own_property = rl_define_own,
    .get_prototype = rl_get_prototype,
    .get_own_property_no_user_code = true,
};

/* ---- the object, its table, and its lifetime -------------------------------------------------------------- */

static void rl_finalizer(JSRuntime *rt, JSValue val)
{
    void *d = rl_opaque(val);   /* see rl_opaque: never JS_GetOpaque against an id a release may have zeroed */
    int i;

    (void)rt;
    /* THE ROW GOES FIRST, and unconditionally: it names this object by POINTER, so a row left behind would
       hand the next ask for that document a freed object. */
    for (i = 0; i < g_rows_n; i++)
        if (JS_VALUE_GET_PTR(g_rows[i].obj) == JS_VALUE_GET_PTR(val)) {
            g_rows[i] = g_rows[--g_rows_n];
            break;
        }
    free(d);
}

static void rl_record_row(JSValueConst obj, uint32_t doc)
{
    int i;

    DCHECK(!world_doc_hosted(doc),
           "a cross-origin Location row was recorded for a Document whose realm this agent HOLDS — that "
           "document's own realm answers `location` (core/frame/location.h), so a row for one is a second "
           "answer to a question that already has one");
    for (i = 0; i < g_rows_n; i++)
        DCHECK(g_rows[i].doc != doc,
               "a SECOND cross-origin Location row was recorded for one Document — the table is a map from a "
               "peer's document to the one object that answers for it, so two rows make "
               "`otherW.location === otherW.location` decide by which row the scan reached first");
    if (g_rows_n == g_rows_cap) {
        int cap = g_rows_cap ? g_rows_cap * 2 : 8;
        RemoteLocRow *g = realloc(g_rows, (size_t)cap * sizeof *g);

        CHECK(g != NULL, "remote location: OOM recording a cross-origin Location — an unrecorded object is "
                         "minted again on the next ask, and `otherW.location === otherW.location` is then "
                         "false about one document");
        g_rows = g;
        g_rows_cap = cap;
    }
    g_rows[g_rows_n].doc = doc;
    g_rows[g_rows_n].obj = obj;   /* BORROWED — rl_finalizer takes the row out */
    g_rows_n++;
}

JSValue remote_location_of_document(JSContext *ctx, uint32_t doc)
{
    RemoteLocData *d;
    JSValue obj;
    int i;

    DCHECK(g_rl_rt != NULL, "a cross-origin Location was asked for before remote_location_init ran");
    DCHECK(doc != 0, "the cross-origin Location of document zero was asked for — zero is the world registry's "
                     "NONE, so the caller is holding a handle it never resolved rather than a document");
    DCHECK(!world_doc_hosted(doc),
           "the cross-origin Location of a Document THIS AGENT HOLDS was asked for — §7.2.1.3.3 answers TRUE "
           "for it, so §7.2.4 wants that realm's own Location (core/frame/location.h's location_object) and a "
           "filtered object here would refuse a read the origins permit");
    for (i = 0; i < g_rows_n; i++)
        if (g_rows[i].doc == doc) return JS_DupValue(ctx, g_rows[i].obj);

    obj = JS_NewObjectClass(ctx, g_rl_class);
    if (JS_IsException(obj)) return obj;
    d = calloc(1, sizeof *d);
    CHECK(d != NULL, "remote location: OOM building a cross-origin Location");
    d->doc = doc;
    JS_SetOpaque(obj, d);
    rl_record_row(obj, doc);
    return obj;
}

/* ---- the declaration and the per-realm surface ------------------------------------------------------------ */

static void remote_location_install_realm(JSContext *ctx)
{
    JSValue proto, prev;

    prev = JS_GetClassProto(ctx, g_rl_class);
    DCHECK(JS_IsNull(prev), "remote_location_install_realm ran twice in one realm — every cross-origin Location "
                            "already answering out of the first surface would keep functions of a realm this "
                            "one has replaced");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "the cross-origin Location surface could not be allocated");
    /* WHICH INTERFACE THESE TWO MEMBERS BELONG TO, for the Web IDL gap audit — they are §7.2.4's own `href`
       and `replace` and not members of a surface of this component's invention. The tag itself is never
       reachable THROUGH this object: §7.2.1.3.2 answers %Symbol.toStringTag% with undefined before any walk,
       so `Object.prototype.toString.call(otherW.location)` is "[object Object]", which is what a browser
       reports for a cross-origin Location. */
    idl_interface_tag(ctx, proto, "Location");
    idl_install_accessor(ctx, proto, LOCATION_CROSS_ORIGIN[LOCATION_XO_HREF].name, rl_href_get, LOCATION_XO_HREF,
                         g_rl_href_setter);
    idl_install_method(ctx, proto, LOCATION_CROSS_ORIGIN[LOCATION_XO_REPLACE].name, g_rl_replace);
    JS_SetClassProto(ctx, g_rl_class, proto);
}

void remote_location_init(JSContext *ctx)
{
    static const IdlArgType URL_ARG[] = { IDL_USVSTRING };
    JSClassDef d = { "Location", .finalizer = rl_finalizer, .exotic = &RL_EXOTIC };
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue global, sym_ctor;
    int i;

    /* ONE INSTANCE IS ONE AGENT, so this runs ONCE and a second call is a defect rather than a no-op. The
       `if (g_rl_rt == rt) return;` that used to stand under the assert made a re-declaration silent, which is
       the shape core/agent_state.h's latch argument is about: a component that reports itself built is exactly
       what a stale handle produces. */
    DCHECK(g_rl_rt == NULL,
           "remote_location_init ran twice — one instance is one agent, and a second declaration would re-mint "
           "the class id every live cross-origin Location is already branded with");
    g_rl_rt = rt;
    JS_NewClassID(rt, &g_rl_class);
    CHECK(JS_NewClass(rt, g_rl_class, &d) == 0,
          "the cross-origin Location's class could not be declared");

    for (i = 0; i < LOCATION_XO_N; i++) {
        g_member_atom[i] = JS_NewAtom(ctx, LOCATION_CROSS_ORIGIN[i].name);
        CHECK(g_member_atom[i] != JS_ATOM_NULL,
              "remote location: §7.2.1.3.1's CrossOriginProperties(Location) names could not be interned — "
              "without them every read is decided by comparing against nothing");
    }
    g_fallback_atom[0] = JS_NewAtom(ctx, "then");
    CHECK(g_fallback_atom[0] != JS_ATOM_NULL, "remote location: §7.2.1.3.2's `then` could not be interned");
    {
        /* THE THREE WELL-KNOWN SYMBOLS, captured from THIS realm's %Symbol% before any page script runs — the
           same rule and the same reason as core/frame/window_proxy.c's capture: `Symbol` is a global a page may
           replace, and a well-known symbol resolved after that names whatever the page put there. Atoms are the
           RUNTIME's, and an agent is one runtime, so one capture serves every realm. */
        static const char *const SYM[RL_FALLBACK_N - 1] = { "toStringTag", "hasInstance",
                                                            "isConcatSpreadable" };
        global = JS_GetGlobalObject(ctx);
        sym_ctor = JS_GetPropertyStr(ctx, global, "Symbol");
        JS_FreeValue(ctx, global);
        CHECK(JS_IsObject(sym_ctor),
              "remote location: this realm has no %Symbol% — §7.2.1.3.2's fallback is three well-known symbols "
              "and a realm without the intrinsic can recognise none of them");
        for (i = 0; i < RL_FALLBACK_N - 1; i++) {
            /* A DATA PROPERTY of %Symbol% (ECMA-262 6.1.5 makes every well-known symbol one, non-writable and
               non-configurable), so this read runs none of the page's code — which it must not, since it runs
               from C with no flow base under it. */
            JSValue s = JS_GetPropertyStr(ctx, sym_ctor, SYM[i]);

            CHECK(JS_IsSymbol(s), "remote location: %Symbol% carries no well-known symbol under one of the "
                                  "three names §7.2.1.3.2's fallback is defined over");
            g_fallback_atom[1 + i] = JS_ValueToAtom(ctx, s);
            JS_FreeValue(ctx, s);
            CHECK(g_fallback_atom[1 + i] != JS_ATOM_NULL,
                  "remote location: a well-known symbol could not be interned");
        }
        JS_FreeValue(ctx, sym_ctor);
    }

    /* DECLARED ONCE PER AGENT, INSTALLED PER REALM — the IDL pool is sealed after agent init, so a member
       minted inside the per-realm install works for the first realm and aborts on the second. */
    g_rl_href_setter = idl_setter_id(ctx, IDL_USVSTRING, false, rl_href_set, LOCATION_XO_HREF);
    g_rl_replace = idl_method_id(ctx, URL_ARG, 1, rl_replace, LOCATION_XO_REPLACE);

    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — core/agent_state.h. The CLASS is the slot the
       release used to keep: a class is registered in a RUNTIME, so a carried id names a class that is gone and
       every JS_GetOpaque against it in the next agent answers about whichever class that runtime hands the
       number to. The two pool entries were already given back and are declared for the same reason — a
       declaration is a registration in the same runtime, and an index into a pool the next agent has not built
       is what remote_location_install_realm would install §7.2.4's two members out of. */
    agent_state_ptr(RL_COMPONENT, &g_rl_rt, "the runtime §7.2.4's cross-origin class and members were "
                                            "declared in");
    agent_state_class(RL_COMPONENT, &g_rl_class,
                      "§7.2.4 The Location interface's cross-origin per-realm prototype slot and brand");
    agent_state_id(RL_COMPONENT, &g_rl_href_setter, "§7.2.4's cross-origin `href` setter declaration");
    agent_state_id(RL_COMPONENT, &g_rl_replace, "§7.2.4's cross-origin `replace` declaration");
    for (i = 0; i < LOCATION_XO_N; i++)
        agent_state_atom(RL_COMPONENT, &g_member_atom[i],
                         "one of §7.2.1.3.1 CrossOriginProperties ( O )'s Location names, interned");
    for (i = 0; i < RL_FALLBACK_N; i++)
        agent_state_atom(RL_COMPONENT, &g_fallback_atom[i],
                         "one of §7.2.1.3.2 CrossOriginPropertyFallback ( P )'s names, interned");
    agent_state_ptr(RL_COMPONENT, &g_rows, "the one cross-origin Location per PEER document of this agent");
    realm_declare_intrinsic(remote_location_install_realm);
}

/* THE AGENT'S HALF, UNDONE — core/platform.h's third column, and it takes the RUNTIME because that is what an
   agent is. It took a JSContext until this diff, and that signature is the whole of what kept this component
   off the column and left it a hand-written line in three hosts. JS_FreeAtomRT reaches the same atoms
   JS_FreeAtom did — JS_FreeAtom IS JS_FreeAtomRT(ctx->rt, a). */
void remote_location_free(JSRuntime *rt)
{
    int i;

    /* NOT `if (!g_rl_rt) return;`. This runs from a release column that runs only where platform_agent_init
       ran, and this component's declaration is unconditional on that list — so a null runtime here is a host
       that tore down a browser it never built, and the silent return made that indistinguishable from a
       release that worked (core/agent_state.h). */
    DCHECK(g_rl_rt != NULL,
           "§7.2.4's cross-origin surface was released in an agent that never declared it — "
           "remote_location_init is a row on core/platform.c's declare column, so reaching here without it is "
           "a teardown of a browser that was never brought up");
    DCHECK(g_rl_rt == rt,
           "§7.2.4's cross-origin surface was released against a RUNTIME other than the one it was declared "
           "in — its class and its two pool entries are registrations in that runtime, and giving them back "
           "against another frees atoms of a heap this component never interned into");
    for (i = 0; i < LOCATION_XO_N; i++) { JS_FreeAtomRT(rt, g_member_atom[i]); g_member_atom[i] = JS_ATOM_NULL; }
    for (i = 0; i < RL_FALLBACK_N; i++) { JS_FreeAtomRT(rt, g_fallback_atom[i]); g_fallback_atom[i] = JS_ATOM_NULL; }
    /* THE ROWS BORROW, so this frees the TABLE and nothing in it. Emptying it here is what keeps a finalizer
       running later in the teardown from scanning freed storage: the loop then reads n == 0 and touches
       nothing. */
    free(g_rows);
    g_rows = NULL;
    g_rows_n = g_rows_cap = 0;
    g_rl_href_setter = g_rl_replace = -1;
    /* AND THE CLASS ID, for the reason core/agent_state.h states: a class is registered in a runtime, so a
       carried id names a class that is gone and brands every object the next agent's component mints with a
       number that runtime never gave out. rl_finalizer already reads its record through JS_GetAnyOpaque (see
       rl_opaque), which is what makes this line safe rather than what it costs. */
    g_rl_class = 0;
    /* the surfaces are the REALMS' — released with their contexts */
    g_rl_rt = NULL;
}
