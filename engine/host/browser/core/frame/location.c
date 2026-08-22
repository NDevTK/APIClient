/* THE LOCATION INTERFACE — HTML §7.2.4 "The Location interface", Blink core/frame, and the place the two halves meet.
 *
 * Half of it is the PRINCIPAL and is CONCRETE: origin, protocol, host, hostname, port, pathname. A bundle
 * builds its request URLs out of these (`location.origin + "/api/x"`), so a concolic origin would turn every
 * endpoint into a shape and lose the very values this tool reports. CLAUDE.md says so directly: the principal
 * is concrete for URL building.
 *
 * The other half is ATTACKER INPUT and is CONCOLIC: `search` and `hash` are whatever the attacker puts in the
 * URL they get someone to click, so they are domain-carrying, they must never force a branch, and they are the
 * @S sources a breakout is solved for. They are NOT the same source: the browser percent-encodes them by
 * DIFFERENT sets (verified on Chrome — both encode `< > "` and space; the FRAGMENT additionally encodes
 * backtick and NOT `'`, the special-scheme QUERY encodes `'` and NOT backtick), so a candidate that breaks out
 * through one may be dead through the other and they carry separate identities.
 *
 * IT IS AN INTERFACE, AND IT WAS NOT ONE. This file built a PLAIN OBJECT with own data properties and no
 * prototype of its own: `Location.prototype` and `window.Location` did not exist, so `location instanceof
 * Location` threw, and prototype-based feature detection and patching — a favourite of analytics shims and of
 * anti-bot code, which is precisely the code this engine wants to run — found nothing to patch.
 *
 * AND ITS MEMBERS BELONG ON THE OBJECT, WHICH IS WHERE NAVIGATOR'S AND SCREEN'S DO NOT. §7.2.4 marks every
 * member `[LegacyUnforgeable]`, and Web IDL §3.4.10 says what that means: "the property will be
 * non-configurable and will exist as an own property on the object itself rather than on its prototype".
 * §3.7.6's "define the regular attributes" REMOVES the unforgeable ones from the interface prototype object, so
 * `Location.prototype` legitimately carries nothing but §3.7.3's `constructor` and its @@toStringTag — which is
 * exactly what a browser reports. The spec gives the reason in the same section, and it is a security reason:
 * the mitigations exist "for legacy code that consulted the Location interface, or stringified it, to determine
 * the document URL, and then used it in a security-sensitive way", so that `foo[location] = bar` and
 * `location + ""` cannot be misdirected. A configurable member on a shared prototype is exactly the
 * misdirection, so this is not a placement detail — putting Location's members where Navigator's go would
 * IMPLEMENT THE WRONG INTERFACE.
 *
 * §7.2.4's CREATE A LOCATION OBJECT adds two own properties that are not IDL members at all, for the same
 * reason: `valueOf` is pinned to %Object.prototype.valueOf% and %Symbol.toPrimitive% is pinned to undefined,
 * both non-writable and non-configurable, so ToPrimitive on a Location reaches the unforgeable stringifier and
 * nothing a page can install.
 *
 * WHAT §7.2.4 STILL WANTS AND THIS BUILD DOES NOT HAVE. Every member's SETTER, `assign`, `replace` and
 * `reload` all end in "Location-object navigate", and navigation from a Location is not built; `ancestorOrigins`
 * is a DOMStringList, an interface that does not exist here. They are honestly ABSENT and the IDL audit names
 * them, rather than being stubs that would let a page believe it had navigated. The §7.2.4.1-.10 EXOTIC
 * INTERNAL METHODS are a separate surface from the members: two of them ([[PreventExtensions]] returning false
 * and [[SetPrototypeOf]] being SetImmutablePrototype) have no hook in quickjs's exotic table at all, so they
 * are a quickjs-side capability rather than something this component can express.
 *
 * EVERY MEMBER READS THE DOCUMENT'S ADDRESS AT THE CALL. §7.2.4: "A Location object has an associated url,
 * which is this Location object's relevant Document's URL". The six concrete members were serialized ONCE at
 * install and stored as data properties, which was a per-realm fact captured at a moment — and the moment is
 * now wrong twice over, because a realm's intrinsics are built before its navigable has a document, and because
 * the address is what a navigation changes. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "solver/concolic.h"
#include "core/agent_state.h"
#include "core/frame/location.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/url/url.h"
#include "core/dom/document.h"   /* this realm's address — the one place a document's URL lives */

/* THE MEMBER LIST, IN ONE PLACE, in the order §7.2.4's IDL declares them, and with the cross-origin entry
   §7.2.1 "Security infrastructure for Window, WindowProxy, and Location objects" gives each — read three
   times (the getter's magic is an index into it, the install walks it, and the cross-origin assertion below
   reads the third column). */
typedef enum {
    /* Not on §7.2.1's CrossOriginProperties(Location) at all: a cross-origin read is a SecurityError. */
    LOC_XO_NONE = 0,
    /* On the list with [[NeedsSetter]] and no [[NeedsGetter]] — a cross-origin document may WRITE this member
       and may not read it. `href` is the only attribute entry, and it is why §7.2.1 lists Location at all:
       `otherW.location.href = url` is how one document navigates another. */
    LOC_XO_SETTER,
} LocCrossOrigin;

#define LOC_MEMBERS(X)                          \
    X(HREF,     "href",     LOC_XO_SETTER)      \
    X(ORIGIN,   "origin",   LOC_XO_NONE)        \
    X(PROTOCOL, "protocol", LOC_XO_NONE)        \
    X(HOST,     "host",     LOC_XO_NONE)        \
    X(HOSTNAME, "hostname", LOC_XO_NONE)        \
    X(PORT,     "port",     LOC_XO_NONE)        \
    X(PATHNAME, "pathname", LOC_XO_NONE)        \
    X(SEARCH,   "search",   LOC_XO_NONE)        \
    X(HASH,     "hash",     LOC_XO_NONE)

#define LOC_ENUM_ONE(id, str, xo) LOC_##id,
#define LOC_NAME_ONE(id, str, xo) str,
#define LOC_XO_ONE(id, str, xo)   xo,

enum { LOC_MEMBERS(LOC_ENUM_ONE) LOC_N };
static const char *const LOC_NAME[] = { LOC_MEMBERS(LOC_NAME_ONE) };
static const LocCrossOrigin LOC_XO[] = { LOC_MEMBERS(LOC_XO_ONE) };

/* §7.2.1's CrossOriginProperties(Location), verbatim: « { [[Property]]: "href", [[NeedsSetter]]: true },
   { [[Property]]: "replace" } ». Those two are what a document of ANOTHER origin may reach; §7.2.4.5 through
   §7.2.4.10 make everything else a SecurityError.
   BOTH OF THEM NAVIGATE, AND NAVIGATION FROM A LOCATION IS NOT BUILT, so this object has no cross-origin
   surface at all — which is exactly why window_proxy.c's `location` stops at an assert rather than handing a
   Location across an origin boundary. Declared BESIDE THE MEMBERS rather than asked at the read, for the reason
   window_proxy.c gives for the Window half: the failure mode of a per-read check is silence, and a member added
   without one leaks a cross-origin document with nothing to say so.
   ASSERTED PER REALM against the object itself — the operation entry must be absent, and the attribute entry
   must have no setter. The day either capability lands, the assert fires AT the install and names what the
   object then needs. */
static const char *const LOC_CROSS_ORIGIN_OPERATIONS[] = { "replace" };

/* THE CLASS IS THE BRAND. Web IDL §3.7.5's check is "does esValue implement the interface", and the one object
   per realm WEARS the class, so the check is a class-id comparison a page cannot forge. Its members are OWN
   properties of that object, so the only way to reach a getter with the wrong receiver is to pull it out with
   Object.getOwnPropertyDescriptor and apply it — which a browser answers with a TypeError, so this does. */
static JSClassID g_loc_class;
static int g_obj_slot = -1;   /* this realm's one Location */

static bool loc_brand(JSContext *ctx, JSValueConst this_val)
{
    DCHECK(g_loc_class != 0, "a Location member ran before location_init declared the class — the member is an "
                             "own property of an object the per-realm install builds, so there is no route here "
                             "that has not run the declaration first");
    if (JS_GetClassID(this_val) == g_loc_class) return true;
    JS_ThrowTypeError(ctx, "a Location member was reached on something that is not a Location");
    return false;
}

/* THE HALF OF "THIS's ..." THIS ENGINE CAN ANSWER, asserted rather than assumed — the same shape, and the same
   reason, as navigator.c's. A C member runs in the realm that DEFINED it, so a getter pulled off THIS realm's
   Location and applied to ANOTHER realm's would read the wrong document's address: same-origin documents are
   one heap, so both objects are reachable from one flow and the mix-up is a real reachable state rather than a
   hypothetical. */
static void loc_assert_this_realm(JSContext *ctx, JSValueConst this_val)
{
    JSValue own = realm_value_get(ctx, g_obj_slot);
    bool same = JS_VALUE_GET_PTR(own) == JS_VALUE_GET_PTR(this_val);

    JS_FreeValue(ctx, own);
    DCHECK(same, "a Location member was reached on ONE realm's Location through ANOTHER realm's getter — the "
                 "getter reads document_base_url of ITS OWN realm, so the answer would be a different "
                 "document's address wearing this document's Location. BUILD the Location that carries its own "
                 "realm: give the instance its realm as its class opaque (with the finalizer, gc_mark and "
                 "cow_capture_host_record contract that entails) so the member reads it off THIS");
}

/* THIS LOCATION'S URL — §7.2.4's "A Location object has an associated url, which is this Location object's
   relevant Document's URL". Read AT THE CALL, which is what the sentence says: a realm's intrinsics are built
   before its navigable has a document, so there is nothing to capture at the install, and a navigation is
   exactly what makes a captured copy wrong. `rec` is filled and the caller frees it. */
static void loc_url(JSContext *ctx, UrlRecord *rec)
{
    /* THE DOCUMENT OWNS WHAT ITS ADDRESS IS, and it asserts that it has one — a realm whose Document was never
       installed aborts THERE, at the origin of the fact, rather than in a second copy of the same check here. */
    const char *addr = document_url(ctx);

    /* THE ADDRESS GOES THROUGH THE REAL PARSER, which initialises the record itself. A hand-rolled splitter
       stood here once — strstr("://"), strrchr(':'), fixed 256-byte buffers — and it was wrong wherever a URL
       is interesting: it read `http://0x7f.1/` as the domain `0x7f.1` rather than the address 127.0.0.1, and
       its port split guessed at IPv6 brackets. */
    CHECK(url_parse(rec, addr, strlen(addr), NULL),
          "this realm's document address is not a URL — the host captured something this engine cannot make a "
          "principal out of");
}

/* A member's answer out of a malloc'd serialization, taking ownership of it — every concrete member of Location
   is one of these, so the free belongs here rather than at seven call sites. */
static JSValue loc_string(JSContext *ctx, char *owned)
{
    JSValue v;

    CHECK(owned != NULL, "a Location member's serialization could not be allocated");
    v = JS_NewString(ctx, owned);
    free(owned);
    CHECK(!JS_IsException(v), "a Location member's string could not be allocated");
    return v;
}

static char *loc_concat(const char *a, const char *b)
{
    size_t na, nb;
    char *r;

    CHECK(a != NULL && b != NULL,
          "a Location member joined a serialization that is not there — every url_serialize_* answers with an "
          "allocated string, the empty one included, so a NULL here is an allocation that was never checked");
    na = strlen(a); nb = strlen(b);
    r = malloc(na + nb + 1);
    CHECK(r, "location: OOM building an address");
    memcpy(r, a, na);
    memcpy(r + na, b, nb);
    r[na + nb] = 0;
    return r;
}

/* THE COMPONENT PART OF THIS REALM'S ADDRESS, prefixed as §7.2.4 serializes it ("?x=1", "#frag") and empty when
   the address has none — the `search` and `hash` getter steps, which differ from each other only in which
   component and which prefix. */
static JSValue loc_component(JSContext *ctx, const UrlRecord *rec, bool want_query)
{
    const char *part = want_query ? rec->query : rec->fragment;

    if (!part) return JS_NewStringLen(ctx, "", 0);
    return loc_string(ctx, loc_concat(want_query ? "?" : "#", part));
}

/* EVERY DECLARED MEMBER'S GETTER, once, with §3.7.5's brand check and the realm assertion in front of it. Its
   magic is its index; there is nothing per member to write, which is what stops a member from arriving with a
   hand-written getter that forgets either. */
static JSValue js_loc_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    UrlRecord rec;
    JSValue v;

    if (!loc_brand(ctx, this_val)) return JS_EXCEPTION;
    loc_assert_this_realm(ctx, this_val);
    DCHECK(magic >= 0 && magic < LOC_N, "a Location getter was installed with a magic that is not a member "
                                        "index — the magic IS the index into the one member X-list");
    loc_url(ctx, &rec);
    switch ((int)magic) {
    /* §7.2.4's href getter steps are "return this's url, serialized", and this answers the address WITHOUT its
       query and fragment: those two are the attacker's and are read through their own getters, which is what
       keeps their source identities separate. Splicing them in here is what href becomes once assignment to a
       Location is modelled, and the two concolic halves then propagate through the interpreter's own `+` with
       no special case in this file. */
    case LOC_HREF: {
        char *origin = url_serialize_origin(&rec), *path = url_serialize_path(&rec);
        v = loc_string(ctx, loc_concat(origin, path));
        free(origin);
        free(path);
        break;
    }
    case LOC_ORIGIN:   v = loc_string(ctx, url_serialize_origin(&rec)); break;
    case LOC_PROTOCOL: v = loc_string(ctx, loc_concat(rec.scheme, ":")); break;
    case LOC_HOST:     v = loc_string(ctx, url_serialize_host_port(&rec)); break;
    case LOC_HOSTNAME: v = loc_string(ctx, url_serialize_host(&rec.host)); break;
    case LOC_PORT:     v = loc_string(ctx, url_serialize_port(&rec)); break;
    case LOC_PATHNAME: v = loc_string(ctx, url_serialize_path(&rec)); break;
    /* THE TWO ATTACKER SOURCES, AND THEY CARRY THE EXAMPLE THE ADDRESS ACTUALLY HAS. §solver's rule is that a
       value is a domain plus an example when the code pins, computes or LEARNS one, and this one is learned
       from the address the host supplied — the document was LOADED from it, so its query and fragment are
       observed facts rather than invented ones. The value stays CONCOLIC: the domain is unconstrained and a
       branch on it still forks, which is the whole point of the source. It simply also knows what it concretely
       is right now.
       MINTED PER READ, never once at the install: a candidate run substitutes a source with a breakout at MINT
       time, so a source minted once could never receive one and its sink would be detected and never
       fire-verified. */
    case LOC_SEARCH:
        v = concolic_source_wrap(ctx, LOCATION_SEARCH_SHAPE, LOCATION_SEARCH_SRC, loc_component(ctx, &rec, true));
        break;
    case LOC_HASH:
        v = concolic_source_wrap(ctx, LOCATION_HASH_SHAPE, LOCATION_HASH_SRC, loc_component(ctx, &rec, false));
        break;
    default:
        DFAIL("a Location member was read with a magic no member of this file declares — the magic IS the "
              "member, so an unknown one means a name was installed without a case to answer it");
        v = JS_UNDEFINED;
        break;
    }
    url_record_free(&rec);
    return v;
}

/* §7.2.4's STRINGIFIER: `[LegacyUnforgeable] stringifier attribute USVString href`. Web IDL §3.7.8's
   toStringSteps are the brand check followed by "the stringification behavior of the interface", which for a
   stringifier ATTRIBUTE is that attribute's getter steps — so this is one call into the member above and not a
   second reader of the address.
   IT IS NOT A CORNER. A stringifier is what makes a Location usable ANYWHERE a USVString is expected:
   `new URL(path, location)` is how a page builds an absolute URL from a relative one, and WPT's own
   /common/dispatcher/dispatcher.js opens with exactly it. Without one, ToString found Object.prototype.toString
   and handed the URL parser the eight characters `[object Object]`, so the constructor threw "the base URL is
   not a valid URL" and four webmessaging files ended at their first import. */
static JSValue js_loc_to_string(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    return js_loc_get(ctx, this_val, LOC_HREF);
}

JSValue location_object(JSContext *ctx)
{
    /* OWNED — realm_value_get asserts that THIS realm ran the install that built it. */
    return realm_value_get(ctx, g_obj_slot);
}

/* §7.2.4: "The Window object's location getter steps are to return this's Location object." The same answer the
   two C readers get, out of the same function, because two spellings of one fact is how they come apart. */
static JSValue js_win_location(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)this_val; (void)magic;
    return location_object(ctx);
}

/* ---- §7.2.4's create a Location object -------------------------------------------------------------------- */

/* Steps 2-4: "Let valueOf be location's relevant realm.[[Intrinsics]].[[%Object.prototype.valueOf%]]", then
   `valueOf` and %Symbol.toPrimitive% become own properties that are non-writable, non-enumerable and
   NON-CONFIGURABLE. The pair is a mitigation and not bookkeeping: with @@toPrimitive pinned to undefined and
   valueOf pinned to the intrinsic that answers with the object itself, ToPrimitive on a Location always falls
   through to the unforgeable stringifier, so `location + ""` is the document's URL and a page cannot make it
   anything else.
   THE INTRINSIC IS READ FROM THIS REALM, through two plain data properties (`Object` on the global and
   `prototype` on it) — no accessor is reachable on that path, which is what makes reading it from C legal. */
static void loc_pin_to_primitive(JSContext *ctx, JSValueConst loc)
{
    JSValue global = JS_GetGlobalObject(ctx), ctor, proto, value_of, sym_ctor, to_prim;
    JSAtom a;

    ctor = JS_GetPropertyStr(ctx, global, "Object");
    CHECK(JS_IsObject(ctor), "§7.2.4 step 2 wants this realm's %Object.prototype.valueOf% and the realm has no "
                             "`Object` — the Location is built with the realm's intrinsics, so this is a realm "
                             "whose intrinsics were never installed");
    proto = JS_GetPropertyStr(ctx, ctor, "prototype");
    CHECK(JS_IsObject(proto), "this realm's `Object` has no `prototype`");
    value_of = JS_GetPropertyStr(ctx, proto, "valueOf");
    CHECK(JS_IsFunction(ctx, value_of), "this realm's %Object.prototype.valueOf% is not a function");
    JS_FreeValue(ctx, proto);
    JS_FreeValue(ctx, ctor);
    /* Flags 0: not writable, not enumerable, not configurable — §7.2.4 steps 3 and 4 verbatim. */
    JS_DefinePropertyValueStr(ctx, loc, "valueOf", value_of, 0);

    sym_ctor = JS_GetPropertyStr(ctx, global, "Symbol");
    CHECK(JS_IsObject(sym_ctor), "this realm has no `Symbol`, so %Symbol.toPrimitive% cannot be named");
    to_prim = JS_GetPropertyStr(ctx, sym_ctor, "toPrimitive");
    CHECK(JS_IsSymbol(to_prim), "%Symbol.toPrimitive% is not a symbol in this realm");
    JS_FreeValue(ctx, sym_ctor);
    a = JS_ValueToAtom(ctx, to_prim);
    JS_FreeValue(ctx, to_prim);
    CHECK(a != JS_ATOM_NULL, "%Symbol.toPrimitive% could not be interned");
    JS_DefinePropertyValue(ctx, loc, a, JS_UNDEFINED, 0);
    JS_FreeAtom(ctx, a);
    JS_FreeValue(ctx, global);
}

/* §7.2.1's list, asserted against the object this realm just built — see LOC_CROSS_ORIGIN_OPERATIONS. */
static void loc_assert_no_cross_origin_capability(JSContext *ctx, JSValueConst loc)
{
    size_t i;

    for (i = 0; i < sizeof LOC_CROSS_ORIGIN_OPERATIONS / sizeof LOC_CROSS_ORIGIN_OPERATIONS[0]; i++) {
        JSAtom a = JS_NewAtom(ctx, LOC_CROSS_ORIGIN_OPERATIONS[i]);
        int has;

        DCHECK(a != JS_ATOM_NULL, "a §7.2.1 cross-origin property name could not be interned");
        has = JS_HasProperty(ctx, loc, a);
        JS_FreeAtom(ctx, a);
        CHECK(has >= 0, "[[HasProperty]] over an engine-built Location runs none of the page's code and has "
                        "nothing to throw with");
        /* An entry with neither [[NeedsGetter]] nor [[NeedsSetter]] is an OPERATION: §7.2.1.3.4 exposes the
           whole property, so its mere PRESENCE is a surface a cross-origin document can reach. */
        DCHECK(!has,
               "HTML §7.2.1 exposes this Location operation to a document of ANOTHER ORIGIN, and this engine "
               "has no cross-origin Location to filter through — window_proxy.c's `location` asserts rather "
               "than handing one across, so shipping the operation makes that assert the only thing standing "
               "between a cross-origin page and an unfiltered Location. BUILD §7.2.4.5-.10's filtered object "
               "first: a [[GetOwnProperty]], [[Get]], [[Set]], [[Delete]] and [[OwnPropertyKeys]] that answer "
               "only CrossOriginProperties(Location) and throw a SecurityError for the rest");
    }
}

/* ---- the declaration and the per-realm install ------------------------------------------------------------ */

/* ONE PROTOTYPE, ONE INTERFACE OBJECT AND ONE LOCATION PER REALM, built WITH the realm — §7.2.4: "Each Window
   object is associated with a unique instance of a Location object, allocated when the Window object is
   created." */
static void location_install_realm(JSContext *ctx)
{
    JSValue proto, prev, global, loc;
    int i;

    prev = JS_GetClassProto(ctx, g_loc_class);
    DCHECK(JS_IsNull(prev), "location_install_realm ran twice in one realm — everything already holding the "
                            "first Location would answer out of a discarded object");
    JS_FreeValue(ctx, prev);

    /* §3.7.6: the interface prototype object of an interface whose every member is unforgeable carries NO
       members. `constructor` comes from the interface object below and @@toStringTag is §3.7.3's, and a
       browser's Location.prototype has exactly those two. */
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "Location.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "Location");
    JS_SetClassProto(ctx, g_loc_class, JS_DupValue(ctx, proto));

    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "Location", idl_interface_object(ctx, "Location", proto));

    loc = JS_NewObjectProtoClass(ctx, proto, g_loc_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(loc), "the Window's associated Location could not be allocated");

    /* §3.4.10's placement, one member at a time: an OWN, NON-CONFIGURABLE accessor on the Location itself. */
    for (i = 0; i < LOC_N; i++) {
        /* §7.2.4 declares a SETTER for every member but `origin`, and every one of them ends in
           "Location-object navigate" — which is not built, so this build installs none. */
        int setter = -1;

        /* §7.2.1's OTHER HALF, at the one site that can change it: the cross-origin list's single attribute
           entry exposes href's SETTER and not its getter, so a setter here is the surface a document of
           another origin reaches. */
        DCHECK(setter < 0 || LOC_XO[i] != LOC_XO_SETTER,
               "HTML §7.2.1 exposes this Location member's SETTER to a document of ANOTHER ORIGIN, and this "
               "engine has no cross-origin Location to filter through — window_proxy.c's `location` asserts "
               "rather than handing one across, so shipping the setter makes that assert the only thing "
               "standing between a cross-origin page and a Location it can navigate. BUILD §7.2.4.5-.10's "
               "filtered object first: a [[GetOwnProperty]], [[Get]], [[Set]], [[Delete]] and "
               "[[OwnPropertyKeys]] that answer only CrossOriginProperties(Location) and throw a SecurityError "
               "for the rest");
        idl_install_accessor_unforgeable(ctx, loc, LOC_NAME[i], js_loc_get, i, setter);
    }
    /* §3.7.8: the stringifier is unforgeable, so its property is on the object with
       {[[Writable]]: false, [[Enumerable]]: true, [[Configurable]]: false}. */
    JS_DefinePropertyValueStr(ctx, loc, "toString",
                              JS_NewCFunction(ctx, js_loc_to_string, "toString", 0), JS_PROP_ENUMERABLE);
    loc_pin_to_primitive(ctx, loc);
    loc_assert_no_cross_origin_capability(ctx, loc);

    /* §7.2.2's `[PutForwards=href, LegacyUnforgeable] readonly attribute Location location`. It was a plain
       writable data property, so `window.location = {href: "..."}` REPLACED the Location object outright —
       which is the forgery the unforgeable annotations on both sides of this exist to prevent. The forwarding
       half of [PutForwards=href] is ABSENT rather than stubbed, because it navigates: a setter that stored a
       string would make a page believe it had navigated. */
    idl_install_accessor_unforgeable(ctx, global, "location", js_win_location, 0, -1);
    realm_value_set(ctx, g_obj_slot, loc);
    JS_FreeValue(ctx, global);
}

/* THIS COMPONENT'S NAME, spelled once and used for both things a name is used for here: the slots it declares
   to core/agent_state.h and the two rows it claims in solver/concolic.c's source registry. The registry's
   give-back is keyed by it, so a declaration and a release naming two different owners is the one way that can
   go wrong — and a literal typed out four times is how it would. */
#define LOC_COMPONENT "location"

void location_init(JSContext *ctx)
{
    JSClassDef d = { "Location" };

    DCHECK(g_obj_slot < 0, "location_init ran twice — the class, the slot and the two sources are declared once "
                           "per AGENT");
    /* THE TWO ATTACKER SOURCES THIS COMPONENT OWNS. A source's browser delivery is a fact about the COMPONENT,
       not about a document, so it belongs beside the class registration and not in the per-realm install. A
       second same-origin document re-declaring it is what caught this.
       THE TWO SETS, verified on Chrome and stated in CLAUDE.md: both components encode space, `"`, `<` and `>`;
       the FRAGMENT additionally encodes the backtick and NOT the apostrophe, and the special-scheme QUERY
       encodes the apostrophe and `#` and NOT the backtick. That difference is the whole reason these are two
       sources rather than one — the same candidate is a live JS-context breakout through the fragment and a
       dead one through the query, and vice versa for a template-literal context. */
    /* BOTH ARE CARRIED IN THE VICTIM'S OWN ADDRESS, which is what makes them the two single-navigation sources
       — the attacker writes one URL and the victim's load of it is the whole PoC. The component each rides is
       the `prefix` already declared, so the reproduction needs nothing this line does not already say. */
    concolic_declare_source(LOC_COMPONENT, LOCATION_HASH_SRC, " \"<>`", '#', SRC_DELIVER_ADDRESS);
    concolic_declare_source(LOC_COMPONENT, LOCATION_SEARCH_SRC, " \"#<>'", '?', SRC_DELIVER_ADDRESS);

    JS_NewClassID(JS_GetRuntime(ctx), &g_loc_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_loc_class, &d) == 0,
          "Location: the per-realm prototype slot could not be declared");
    g_obj_slot = realm_value_declare(ctx, "HTML §7.2.4 the Window's associated Location");
    realm_declare_intrinsic(location_install_realm);
    agent_state_class(LOC_COMPONENT, &g_loc_class, "§7.2.4's per-realm prototype slot and brand");
    agent_state_id(LOC_COMPONENT, &g_obj_slot, "the per-realm slot §7.2.4's one Location lives in");
}

/* THE AGENT'S HALF, UNDONE — core/platform.h's third column, which this component was NOT on: `location_free`
 * was a line in main.c's teardown, and again in wpt_runner.c's, and again in test_forced.c's. That is the
 * hand-copied list that file exists to abolish, and it was already one edit from the drift that cost the WPT
 * runner §3.2's permission store and both delivery callees — a component released from a list one host writes
 * out by hand is a component some other host leaks.
 *
 * IT IS ALSO WHAT MAKES THE ORDER OF THE TWO SOURCE CLAIMS A CHECKED FACT rather than a coincidence of three
 * teardowns. The claims below are given back into solver/concolic.c's registry, and concolic_free asserts that
 * registry is empty; the only thing that placed this release before that assert was that all three hosts
 * happened to write the two lines in that order. On the column it is reverse declaration order that places it,
 * and platform_agent_free runs before solver_agent_free by construction.
 *
 * AND THE CLASS WAS NEVER GIVEN BACK AT ALL, which is core/agent_state.h's dom_rect defect exactly: the release
 * cleared the realm slot and left `g_loc_class` holding an id issued by a runtime that is going away. The next
 * agent in the process would keep it — JS_NewClassID returns a non-zero id unchanged — and register §7.2.4's
 * brand at a number the new runtime's own allocator is free to hand to some other component. Both slots are
 * named to core/agent_state.h now, so platform_agent_free asserts each is back where a fresh process would
 * have found it. */
void location_free(void)
{
    DCHECK(g_obj_slot >= 0, "§7.2.4's Location was released in an agent that never declared it");
    /* The prototypes, the interface objects and the Locations are the REALMS' — each is released with its
       context. What the agent holds is the brand and the slot. */
    g_obj_slot = -1;
    g_loc_class = 0;
    /* THE SOURCE CLAIMS, GIVEN BACK BY THE COMPONENT THAT OWNS THE SOURCES, and LAST because they are declared
       FIRST — a release is the inverse of its declaration. Their STORAGE is solver/concolic.c's registry and
       the CLAIM is this component's (core/platform.h's fourth paragraph), so they come back at the release of
       the component whose `location_init` states what a browser does to an attacker's bytes on their way into
       a fragment and a query. A row left behind is not an idle string: it answers a later agent's delivery
       question for a document that no longer exists, and it is what that agent's own declaration of
       `location.hash` collides with.
       ONE CALL FOR BOTH ROWS, and it names neither: the registry stores the claimant on the row, so what this
       component gave back cannot drift from what it declared. A release reciting the two names could — and the
       third claimant, whose rows are one per file on the device, could not recite them at all. */
    concolic_undeclare_sources(LOC_COMPONENT);
}
