/* THE LOCATION INTERFACE — HTML §7.2.4 "The Location interface", Blink core/frame, and the place the two halves meet.
 *
 * Half of it is the PRINCIPAL and is CONCRETE: origin, protocol, host, hostname, port. A bundle builds its
 * request URLs out of these (`location.origin + "/api/x"`), so a concolic origin would turn every endpoint
 * into a shape and lose the very values this tool reports. CLAUDE.md says so directly: the principal is
 * concrete for URL building. What makes that a THEOREM rather than a preference is §7.2.5's
 * can-have-its-URL-rewritten step 2 — a router may not change any of those five — which is also why `pathname`
 * is NOT among them: it is exactly what a router does change, so it answers concretely for an address a
 * response was fetched from and as a DERIVATION for one the bundle computed. See loc_of_address.
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
 * THE INTERFACE IS ITS SETTERS, AND THIS FILE HAD NONE OF THEM. What stood here said every member's setter,
 * `assign`, `replace` and `reload` "all end in Location-object navigate, and navigation from a Location is not
 * built", and that `ancestorOrigins` is "a DOMStringList, an interface that does not exist here". BOTH HAD
 * STOPPED BEING TRUE: core/frame/navigable.h has §7.4.2.2's navigate, reached by every hyperlink this engine
 * follows, and core/html/dom_string_list.h is HTML §2.6.5's DOMStringList with four consumers. It is the stale
 * claim this project's own §DFAIL rule is written about — accurate about the SPEC, wrong about THIS TREE — and
 * it was load-bearing rather than decorative, because the two DCHECKs it justified REFUSED THE INSTALL of
 * `href`'s setter and of `replace` until a capability was built that nothing needed. A mitigation that forbids
 * the member it protects is not a mitigation.
 *
 * WHY THOSE TWO ASSERTS ARE GONE RATHER THAN SATISFIED. They asked this file to build §7.2.4.5-.10's
 * cross-origin filtered Location before shipping the two members §7.2.1 puts on CrossOriginProperties(Location),
 * on the ground that "window_proxy.c's assert is the only thing standing between a cross-origin page and a
 * Location it can navigate". That IS the guard, and it is the RIGHT place for it: a filter is a property of the
 * object that CROSSES, and in this engine no Location crosses — window_proxy.c's WP_LOCATION answers out of
 * proxy_realm, whose first DCHECK is that the navigable's active document is this agent's, so a cross-origin
 * read aborts at the boundary rather than reaching a member here. A second copy of that guard, phrased as a
 * refusal to implement §7.2.4 at all, does not make the boundary safer; it makes the interface absent. The
 * cross-origin LIST stays declared beside the members below, because what it is for is the day a Location does
 * cross: it is then the statement of which two members survive the filter.
 *
 * WHAT §7.2.4 STILL WANTS AND THIS BUILD DOES NOT HAVE, named at the ONE place each is reached rather than as
 * a list here that a reader would have to re-check: `ancestorOrigins` crashes at the step that wants
 * §7.3.2.1's ancestor origins list. It is REACHED only after the steps in front of it have run, which is what
 * makes the crash name a subproblem instead of a surface. `reload`'s last step USED TO BE ON THAT LIST and is
 * not: HTML §7.4.3 "Reloading and traversing"'s reload is core/frame/navigable.h's, and the member is the
 * machine that drives it — which it has to be, because §7.4.3 step 1 fires a navigate event the page can
 * cancel.
 * §7.4.2.3.3 "Fragment navigations" USED TO BE ON THAT LIST and is not: every member here that navigates now
 * takes §7.4.2.2's same-document arm when the destination selects it, which is what makes `location.hash =
 * "#/route"` a route change instead of a second Document over the router that ran it. The §7.2.4.1-.10 EXOTIC INTERNAL METHODS are a separate surface from the members: two
 * of them ([[PreventExtensions]] returning false and [[SetPrototypeOf]] being SetImmutablePrototype) have no
 * hook in quickjs's exotic table at all, so they are a quickjs-side capability rather than something this
 * component can express.
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
#include "solver/solve.h"        /* §7.2.4's three whole-URL algorithms are the @S URL-context sink */
#include "solver/world.h"        /* this agent HOSTS this Document — §7.2.4's security check's whole premise */
#include "core/agent_state.h"
#include "core/frame/location.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/url/url.h"
#include "core/dom/document.h"   /* this realm's address — the one place a document's URL lives */
#include "core/encoding/encoding.h"      /* §7.2.4's search setter parses in the DOCUMENT's encoding */
#include "core/frame/navigable.h"        /* §7.4.2.2's navigate — what "Location-object navigate" performs —
                                            and §7.4.3's reload, which `reload()`'s last step IS */
#include "core/frame/remote_location.h"   /* §7.2.1.3.1's CrossOriginProperties(Location), spelled once */
#include "core/frame/session_history.h"  /* §7.4.2.2's same-document test, and §7.4.2.3.3's own machine */
#include "core/frame/window_proxy.h"     /* §7.5.10 step 8's null browsing context — the relevant Document */
#include "core/html/dom_string_list.h"   /* §2.6.5's DOMStringList — `ancestorOrigins` answers with one */

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

/* THE NINE URL MEMBERS, whose getters are all one read of §7.2.4's url and whose setters are all one write
   into a copy of it. §7.2.4's tenth attribute, `ancestorOrigins`, is deliberately NOT here: it reads a
   different fact (the Document's own list, not the url), so it has its own getter and no setter, and folding
   it in would give this table a column that is empty for nine rows out of ten.
   THE THIRD COLUMN IS §7.2.1's, and it is the READ-ONLY-ness that the fourth is: `origin` is the one member
   §7.2.4 declares `readonly`, and the day a tenth arrives the table is what says so rather than an `if` in the
   install loop. */
#define LOC_MEMBERS(X)                                     \
    X(HREF,     "href",     LOC_XO_SETTER, false)          \
    X(ORIGIN,   "origin",   LOC_XO_NONE,   true)           \
    X(PROTOCOL, "protocol", LOC_XO_NONE,   false)          \
    X(HOST,     "host",     LOC_XO_NONE,   false)          \
    X(HOSTNAME, "hostname", LOC_XO_NONE,   false)          \
    X(PORT,     "port",     LOC_XO_NONE,   false)          \
    X(PATHNAME, "pathname", LOC_XO_NONE,   false)          \
    X(SEARCH,   "search",   LOC_XO_NONE,   false)          \
    X(HASH,     "hash",     LOC_XO_NONE,   false)

#define LOC_ENUM_ONE(id, str, xo, ro) LOC_##id,
#define LOC_NAME_ONE(id, str, xo, ro) str,
#define LOC_XO_ONE(id, str, xo, ro)   xo,
#define LOC_RO_ONE(id, str, xo, ro)   ro,

enum { LOC_MEMBERS(LOC_ENUM_ONE) LOC_N };
static const char *const LOC_NAME[] = { LOC_MEMBERS(LOC_NAME_ONE) };
static const LocCrossOrigin LOC_XO[] = { LOC_MEMBERS(LOC_XO_ONE) };
static const bool LOC_READONLY[] = { LOC_MEMBERS(LOC_RO_ONE) };

/* §7.2.4's THREE OPERATIONS. `assign` and `replace` share a body and are told apart by this magic; `reload`
   takes no argument and has its own. They are not members of the table above because an operation is not an
   attribute — it has an argument list and no getter — and giving the table a row that means neither is how a
   list stops being readable. */
enum { LOC_ASSIGN, LOC_REPLACE };

/* §7.2.1.3.1's CrossOriginProperties(Location) IS core/frame/remote_location.h's LOCATION_CROSS_ORIGIN, and it
   is read from there rather than written out again here. THAT FILE IS THE OBJECT THE LIST DESCRIBES — a
   Location whose Document is in another instance, which §7.2.4.5 through §7.2.4.10's cross-origin arms filter
   down to exactly these two names — so this component is the list's OTHER reader: the assert below is that
   §7.2.4's IDL still declares both members, which is the one way the two sections can drift.
   THE LIST USED TO BE A SECOND COPY, and the paragraph that stood here said this Location "has no cross-origin
   surface at all" because "navigation from a Location is not built". Both halves are gone: the surface exists
   (remote_location.c) and the assert that matters is no longer "the entries are absent" but "the interface
   declares them", which is what a filter needs in order to have something to expose. */

/* THE CLASS IS THE BRAND. Web IDL §3.7.5's check is "does esValue implement the interface", and the one object
   per realm WEARS the class, so the check is a class-id comparison a page cannot forge. Its members are OWN
   properties of that object, so the only way to reach a getter with the wrong receiver is to pull it out with
   Object.getOwnPropertyDescriptor and apply it — which a browser answers with a TypeError, so this does. */
static JSClassID g_loc_class;
static int g_obj_slot = -1;   /* this realm's one Location */
/* §7.2.4 gives every Location an ASSOCIATED EMPTY DOMStringList, and it is a FIELD of the Location rather than
   a value the getter mints, which is the whole of what its first step promises: a Location whose relevant
   Document is null answers the SAME object on every read. Per realm because a Location is. The standard notes
   why it can be shared this way — it "cannot carry state across navigations because it is only returned when
   there is no relevant Document". */
static int g_empty_asl_slot = -1;

/* DECLARED ONCE PER AGENT, INSTALLED PER REALM — the IDL pool is sealed after agent init, so a helper that
   mints inline works for the first realm and aborts on the second (core/html/hyperlink.c states the same
   rule for the same reason). `origin` and `ancestorOrigins` are readonly and hold -1 here, which is what
   idl_install_accessor_unforgeable's `setter_stepid` means by "read-only". */
static int g_loc_set[LOC_N];
static int g_loc_assign = -1, g_loc_replace = -1, g_loc_reload = -1;
/* §7.2.2 The Window object's `location`, whose [PutForwards=href] SETTER is Web IDL's and not this
   component's — idl_args.c declares §3.7.6's five forwarding steps once for every carrier in the platform. */
static int g_win_location_set = -1;

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

/* §7.2.4's RELEVANT DOCUMENT, AS A YES/NO — "a Location object has an associated relevant Document, which is
   its relevant global object's browsing context's active document, if this Location object's relevant global
   object's browsing context is non-null, and NULL OTHERWISE".
 *
 * IT IS THE FIRST STEP OF EVERY SETTER AND EVERY METHOD, and the order is observable: `href`, `protocol`,
 * `host`, `hostname`, `port`, `pathname`, `search`, `hash`, `assign`, `replace`, `reload` and `ancestorOrigins`
 * each open with "if this's relevant Document is null, then return", BEFORE the security check that would
 * throw and before the parse that would throw. A Location whose browsing context is gone is therefore INERT,
 * not broken — a whole WPT file (no-browsing-context.window.js, 46 subtests) is nothing but that distinction.
 *
 * WHAT ANSWERS IT IS THE FACT AND NOT ONE OF ITS WRITERS. "Its browsing context is null" is written by
 * §7.5.10 "Destroying documents" step 8 ("set document's browsing context to null") AND by §7.1.3.2's
 * opener-policy browsing context group swap, and core/frame/window_proxy.h names both in as many words. Asking
 * `window_proxy_destroyed` would report the first and answer NO for the second, so a swapped-out window would
 * fall through step 1 and NAVIGATE where the spec returns — which is why the reader for the combined fact was
 * added to that component rather than half of it read from here. A realm with no navigable at all (nothing
 * this file can build a Location for today, but the same sentence covers it) has a null browsing context by
 * the same clause. */
static bool loc_document_is_null(JSContext *ctx)
{
    JSValueConst proxy = document_window_proxy(ctx);

    return !window_proxy_is(proxy) || window_proxy_browsing_context_null(ctx, proxy);
}

/* §7.2.4's SECURITY CHECK, which every member but `href`'s setter and `replace` performs: "if this's relevant
   Document's origin is not same origin-domain with the entry settings object's origin, then throw a
   SecurityError". (The two exceptions are the standard's own, stated twice in notes: "the href setter
   intentionally has no security check" and "the replace() method intentionally has no security check".)
 *
 * ITS ANSWER IS A CONSTANT HERE, AND WHAT MAKES IT CONSTANT IS ASSERTED RATHER THAN ASSUMED. An instance is an
 * ORIGIN-KEYED AGENT CLUSTER, so a Document THIS AGENT HOLDS is same origin with every realm the entry
 * settings object could be — that is the same premise window_proxy.c's proxy_realm opens with, and
 * `document.domain` is a no-op in this engine, so same origin-domain and same origin are one question. The
 * premise is what this checks: `world_doc_hosted` over this Location's own Document. It is a condition that
 * CAN be false — solver/world.h's hosting is a decision §7.4 makes per document, and a Location reached for a
 * navigable a PEER instance holds would fail it — so this is a real check and not a restatement of the null
 * test the callers already ran.
 * IT IS STATED ONCE RATHER THAN SKIPPED TWELVE TIMES, because a step silently absent from twelve bodies is
 * indistinguishable from a step nobody read; the day a Location does cross an instance, this is the line that
 * has to grow the real cross-instance comparison, and the assert fires there first. */
static void loc_assert_same_origin_domain(JSContext *ctx)
{
    DCHECK(world_doc_hosted(document_doc(ctx)),
           "§7.2.4's security check ran against a Document a PEER INSTANCE holds — an instance is an "
           "origin-keyed agent cluster, so a Document this agent hosts is same origin-domain with every realm "
           "the entry settings object could be, and that premise is the ONLY reason this check can be a "
           "constant. A Location whose Document lives elsewhere needs the real comparison: §7.2.1's filtered "
           "cross-origin object, and a SecurityError for the members outside CrossOriginProperties(Location)");
}

/* THIS LOCATION'S URL — §7.2.4's "A Location object has an associated url, which is this Location object's
   relevant Document's URL, IF THIS LOCATION OBJECT'S RELEVANT DOCUMENT IS NON-NULL, AND about:blank
   OTHERWISE". Read AT THE CALL, which is what the sentence says: a realm's intrinsics are built before its
   navigable has a document, so there is nothing to capture at the install, and a navigation is exactly what
   makes a captured copy wrong. `rec` is filled and the caller frees it.
   THE about:blank ARM IS NOT A FALLBACK. It is the second half of the definition, and it is what makes the
   GETTERS on a browsing-context-less Location answer the way a browser does: `href` is "about:blank",
   `pathname` is "blank" (about:blank has an OPAQUE path, so the path serializer answers the opaque string
   without a leading slash), `host`, `port`, `search` and `hash` are empty, and `origin` is the string "null"
   because §4.7 gives an `about:` URL an opaque origin. Reading the destroyed document's last address instead
   would agree with a browser on five of those and disagree on `origin` — the one that matters, because it is
   the answer a page makes a trust decision out of. */
static void loc_url(JSContext *ctx, UrlRecord *rec)
{
    /* THE DOCUMENT OWNS WHAT ITS ADDRESS IS, and it asserts that it has one — a realm whose Document was never
       installed aborts THERE, at the origin of the fact, rather than in a second copy of the same check here. */
    const char *addr = loc_document_is_null(ctx) ? "about:blank" : document_url(ctx);

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

/* A MEMBER'S ANSWER, DERIVED FROM THE ADDRESS IT WAS COMPUTED OUT OF — the solver half of §7.2.4, and the
 * reason `location.pathname` on a route the bundle BUILT still forks.
 *
 * WHAT REACHES IT. A client-side router calls §7.2.5's `pushState` with `"/routes/" + cfg.region + "/admin"`
 * over a config the run fetched, and §7.4.4 "Non-fragment synchronous \"navigations\"" step 8 makes that this
 * Document's address. core/dom/document.h's document_url_value keeps the address as the VALUE it was computed
 * out of, so it carries a concrete EXAMPLE and an unconstrained DOMAIN — and a member of it is then a
 * derivation of that value and not a fresh string. `location.pathname.startsWith("/admin")` runs BOTH arms,
 * which is exactly the gated code this engine exists to reach; writing the bare example in would DECIDE that
 * branch and lose the arm with nothing to say so.
 *
 * THE EXAMPLE IS THE REAL PARSE. `owned` is what url_parse and URL §4.5 "URL serializing"'s own serializers
 * computed from the address's example — this engine never predicts an example, it runs the operation — and
 * `op` names WHICH member's serialization it is, so `pathname` and `href` over one address are two
 * derivations with two identities rather than one predicate deciding the other.
 *
 * ONLY THE MEMBERS A §7.4.4 UPDATE CAN MOVE COME THROUGH HERE. §7.2.5's can-have-its-URL-rewritten step 2
 * requires the target's scheme, username, password, host and port to EQUAL the document's, so `origin`,
 * `protocol`, `host`, `hostname` and `port` are invariant under every address change a router can make — the
 * PRINCIPAL, which CLAUDE.md keeps concrete because a bundle builds its request URLs out of it and a concolic
 * origin would turn every endpoint into a shape. `search` and `hash` do not come through here either: they are
 * already this component's declared attacker SOURCES, minted per read with their own percent-encode sets, and
 * a second derivation over them would be a second identity for one datum.
 *
 * CONSUMES `owned` (through loc_string). §7.2.4's about:blank arm is a CONSTANT OF THE STANDARD — "and
 * about:blank otherwise" — so it is nothing's derivation and returns concrete. */
static JSValue loc_of_address(JSContext *ctx, char *owned, const char *op)
{
    JSValue computed = loc_string(ctx, owned);
    JSValue addr, derived;

    if (loc_document_is_null(ctx)) return computed;
    addr = document_url_value(ctx);
    if (!concolic_is(addr)) { JS_FreeValue(ctx, addr); return computed; }
    derived = concolic_builtin_hook(ctx, addr, op, computed);   /* consumes `computed` */
    JS_FreeValue(ctx, addr);
    DCHECK(concolic_is(derived),
           "a Location member over a COMPUTED address answered with something that is not a derivation — the "
           "address is unknown external input, so every serialization of it is, and a plain string here is the "
           "collapse to bare-concrete that deletes the arm a later branch on this member would have forked");
    return derived;
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
    /* HTML §7.2.4 "The Location interface"'s href getter step 2: "Return this's url, SERIALIZED" — URL §4.5
       "URL serializing", whose step 6 appends U+003F (?) and the query when the query is non-null and whose
       step 7 appends U+0023 (#) and the fragment when the fragment is non-null and the exclude-fragment
       boolean is false. That is url_serialize, which this component already owns and which §7.4.2.2's
       navigate below calls for exactly the same string.
       IT USED TO BE ORIGIN+PATH, on the stated grounds that the query and the fragment "are the attacker's
       and are read through their own getters, which is what keeps their source identities separate". Separate
       identities are a real requirement and this was the wrong way to meet it: the getter is defined by §7.2.4
       to serialize the URL, so answering with a different string is a fidelity bug in EVERY host — a
       conformance run included, where no source is minted at all and the only thing this decided was that
       `location.href` on `http://x/p?q#f` came back `http://x/p`. It is also not the corner it looks:
       §7.2.4's stringifier IS this getter, so `String(location)`, `location + ""` and `new URL(rel, location)`
       all read it, and URL §4.4's basic parser keeps the BASE's query for a fragment-only relative — so
       `new URL("#b", location)` resolved against an address this file had already deleted the query from.
       WHAT IT DOES NOT YET DO IS CARRY THE TWO SOURCES, and that is a different, larger subproblem than this
       one rather than a piece of it: a string whose bytes come from two sources with two DIFFERENT
       percent-encode sets and two different delivery prefixes (see the declarations in location_init) cannot
       be named by the single identity concolic_add_hook's concatenation keeps, and every byte consumer of a
       Location — url.c's base argument first — has to answer over unknown input before the composed value can
       exist at all. Nothing here pretends otherwise: this answers what the SPEC says the member is, which is
       what §CLAUDE assigns the browser half, and it is the step the solver half's version is built ON. */
    case LOC_HREF:
        v = loc_of_address(ctx, url_serialize(&rec, /*exclude_fragment*/ false), "href");
        break;
    case LOC_ORIGIN:   v = loc_string(ctx, url_serialize_origin(&rec)); break;
    case LOC_PROTOCOL: v = loc_string(ctx, loc_concat(rec.scheme, ":")); break;
    case LOC_HOST:     v = loc_string(ctx, url_serialize_host_port(&rec)); break;
    case LOC_HOSTNAME: v = loc_string(ctx, url_serialize_host(&rec.host)); break;
    case LOC_PORT:     v = loc_string(ctx, url_serialize_port(&rec)); break;
    /* §7.2.4's pathname getter steps: "Return the result of URL PATH SERIALIZING this Location object's url"
       (URL §4.5 "URL serializing"). It is the one member of the address a router MOVES, so it is the one this
       whole primitive exists for — see loc_of_address. */
    case LOC_PATHNAME: v = loc_of_address(ctx, url_serialize_path(&rec), "pathname"); break;
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

/* ---- §7.2.4's setters, its three operations, and the one navigate all twelve end in ----------------------- */

/* §7.2.4's "LOCATION-OBJECT NAVIGATE a Location object location to a URL url, optionally given a
 * NavigationHistoryBehavior historyHandling (default "auto")", whose four steps are:
 *   1. Let navigable be location's relevant global object's navigable.
 *   2. Let sourceDocument be the incumbent global object's associated Document.
 *   3. If location's relevant Document is not yet completely loaded, and the incumbent global object does not
 *      have transient activation, then set historyHandling to "replace".
 *   4. Navigate navigable to url using sourceDocument, with exceptionsEnabled set to true and historyHandling
 *      set to historyHandling.
 *
 * ONE FUNCTION FOR ALL TWELVE ALGORITHMS, which is the whole reason the setters above it are small: each of
 * them computes a URL and hands it here, so what §7.4 owes this component is owed ONCE and crashes ONCE,
 * naming itself, instead of twelve bodies each deciding separately what to do about it.
 *
 * §7.4.2.2 "BEGINNING NAVIGATION" IS WHAT PERFORMS IT, AND IT HAS TWO ARMS. Its steps 9-10 resolve
 * historyHandling, and its step 11 then asks four conjuncts — "documentResource is null; response is null; url
 * equals navigable's active session history entry's URL WITH EXCLUDE FRAGMENTS SET TO TRUE; and url's fragment
 * is non-null" — and when they hold it runs §7.4.2.3.3 "Fragment navigations"'s NAVIGATE TO A FRAGMENT and
 * RETURNS. That is a SAME-DOCUMENT operation: it appends a session history entry, fires `popstate` and queues
 * `hashchange`, and the Document, its realm and every flow suspended in it survive. The other arm FETCHES and
 * builds a new Document. Taking the second where the spec takes the first is not a weaker answer, it is a
 * different one — `location.hash = "#/route"` would tear down the router that ran it.
 *
 * THE TEST IS NOT ASKED HERE. core/frame/session_history.h's session_history_is_fragment_navigation is the one
 * implementation, because §7.4.2.2 compares against the ACTIVE SESSION HISTORY ENTRY's URL — that component's
 * field, not this one's address — and because a question some entries ask and others do not is one capability
 * wearing two names. This file's own version of it compared against the DOCUMENT's URL and said the two agree;
 * they do agree for a navigable showing one document, and they are different fields with different writers, so
 * the sentence was a claim rather than a check. navigable_navigate now asserts the negative, so the cross-
 * document loader is UNREACHABLE for a destination this arm owns instead of quietly fetching it.
 *
 * `history_handling` IS §7.4.2.2's ARGUMENT AND IT IS RESOLVED, not passed through. The Location algorithms
 * hand over a NavigationHistoryBehavior — "auto" for eleven of them, "replace" for `replace()` — and §7.4.2.2
 * steps 9-10 turn that into the "push"/"replace" §7.4.2.3.3 fires its navigate event with. The cross-document
 * arm still reads nothing from it: this engine rebuilds the session history record outright for a new Document
 * (core/frame/session_history.c's session_history_install_document starts a fresh list at step 0 and asserts
 * one Document per realm), so "push" and "replace" leave byte-identical state there. */

/* §7.4.2.2 STEPS 9 AND 10, over §7.2.4's NavigationHistoryBehavior. Returns a STATIC string, because the answer
 * outlives a park: §7.4.2.3.3 carries it across its navigate event and hands it on as the NavigationType at
 * three later steps.
 *
 * STEP 9's SECOND CONJUNCT IS TRUE BY THIS FILE'S OWN PRECONDITION and is asserted rather than computed:
 * "initiatorOriginSnapshot is same origin with navigable's active document's origin", where the initiator is
 * the document whose script ran — which loc_assert_this_realm has already established is this one.
 * STEP 10's "THE NAVIGATION MUST BE A REPLACE given url and document" is two disjuncts and both are real here:
 * "url's scheme is `javascript`" (a `javascript:` URL never gets a history entry of its own) or "document's is
 * initial about:blank is true" (the Document a navigable is created with is replaced rather than pushed past). */
static const char *loc_resolve_history_handling(JSContext *ctx, const UrlRecord *target, const char *requested)
{
    const char *resolved = requested;

    DCHECK(requested != NULL && (!strcmp(requested, "auto") || !strcmp(requested, "replace")),
           "§7.2.4's Location-object navigate was given a NavigationHistoryBehavior none of its callers "
           "passes — eleven of the twelve pass \"auto\" and `replace()` passes \"replace\"");
    if (strcmp(requested, "auto") == 0) {
        /* STEP 9's FIRST CONJUNCT is URL §4.6 equivalence with exclude fragments FALSE — the WHOLE address,
           fragment included, which is what makes `location.href = location.href` a replace and
           `location.hash = "#b"` from `#a` a push. */
        char *whole = url_serialize(target, /*exclude_fragment*/ false);

        CHECK(whole != NULL, "location: the destination could not be serialized for §7.4.2.2 step 9");
        resolved = strcmp(whole, document_url(ctx)) == 0 ? "replace" : "push";
        free(whole);
    }
    /* STEP 10. */
    if (target->scheme && strcmp(target->scheme, "javascript") == 0) resolved = "replace";
    if (session_history_is_initial_about_blank(ctx)) resolved = "replace";
    return resolved;
}

/* §7.2.4 step 4 / §7.4.2.2, PERFORMED. Answers TRUE when the destination took §7.4.2.2's SAME-DOCUMENT arm and
 * `w` now holds a begun §7.4.2.3.3 — the caller's next stage drives it, because that algorithm runs the page's
 * `navigate`, `currententrychange`, `dispose` and `popstate` listeners and therefore suspends. FALSE means the
 * cross-document arm enqueued the load and the member is finished.
 *
 * §7.4.2.2's FIRST TWO CONJUNCTS ARE STRUCTURAL AT THIS CALL AND SAY SO: `documentResource` is a POST resource
 * that only §4.10.22's form submission produces, and `response` is what §7.4.2.3.1's cross-document case
 * carries — a Location has neither, which is why the predicate takes neither. */
static bool loc_navigate_begin(JSContext *ctx, SessionHistoryFragmentNav *w, const UrlRecord *target,
                               const char *requested_handling)
{
    const char *history_handling;
    char *addr;
    JSValue r;

    DCHECK(!loc_document_is_null(ctx),
           "§7.2.4's Location-object navigate ran for a Location whose relevant Document is NULL — every one "
           "of the twelve algorithms that reach it returns at its own step 1 in that case, so arriving here "
           "means a body skipped it and is about to navigate a navigable that has no browsing context");

    history_handling = loc_resolve_history_handling(ctx, target, requested_handling);
    addr = url_serialize(target, /*exclude_fragment*/ false);
    CHECK(addr != NULL, "location: the destination could not be serialized");
    if (session_history_is_fragment_navigation(ctx, addr)) {
        session_history_fragment_nav_begin(ctx, w, addr, history_handling);
        free(addr);
        return true;
    }
    /* §7.4.2.2's other arm. navigable_navigate answers JS_UNDEFINED only for an address that does not parse,
       and this one is a SERIALIZATION of a record the parser produced, so a failure here is this file's bug. */
    r = navigable_navigate(ctx, document_window_proxy(ctx), addr);
    free(addr);
    CHECK(!JS_IsUndefined(r),
          "§7.4.2.2's navigate refused a URL this component serialized out of a parsed record — a serialized "
          "URL re-parses, so this is a round-trip the URL component broke rather than anything a page did");
    JS_FreeValue(ctx, r);
    return false;
}

/* WHICH OF §7.2.4's TWELVE ALGORITHMS IS A CODE-EXECUTION SINK, AND IT IS A PARTITION THE STANDARD DRAWS
 * RATHER THAN A NARROWING THIS FILE CHOSE. The @S URL class has ONE context and therefore one written-down
 * vector — solver/solve.h: "navigating executes the `javascript:` scheme and nothing else does" — so the
 * question "is this member a sink" is exactly "can the value the page assigned become the destination's
 * SCHEME", and §7.2.4 answers it per member in its own steps:
 *   `href`, `assign` and `replace` ENCODING-PARSE THE WHOLE GIVEN VALUE ("let url be the result of
 *      encoding-parsing a URL given the given value, relative to the entry settings object"), so the scheme is
 *      the attacker's along with everything else. These three are the sink.
 *   The seven component setters parse into A COPY OF THIS'S URL with a STATE OVERRIDE, and URL §4.4 "URL
 *      parsing" enters the machine at that state ("let state be state override if given, or scheme start state
 *      otherwise") — so host, hostname, port, pathname, search and hash never reach a scheme state at all and
 *      the destination keeps THIS document's scheme.
 *   `protocol` is the one that does reach one, and §7.2.4 stops it by name one step later: "if copyURL's
 *      scheme is not an HTTP(S) scheme, then TERMINATE THESE STEPS". `location.protocol = "javascript"` is a
 *      silent no-op, not a navigation.
 * So opening a search from a component setter would queue `javascript:X9()` against an algorithm whose own
 * steps make it unarrivable, and report it for ever as a search merely unfinished — which §@S calls a rung
 * whose absence and whose zero read alike. What a component setter IS is an open redirect, and this engine
 * has no such class: a redirect executes nothing, so there is no marker for it to fire and nothing for the
 * one oracle §@S allows to prove. It is not reported as safe either — it is not reported at all, which is the
 * same silence §Attacker-sources gives an unforgeable principal and for the same reason.
 *
 * THE ANNOUNCEMENT IS UNCONDITIONAL ON THAT ARM, taint or no taint, because the CANDIDATE run's value is a
 * plain String by construction: solver/solve.c's URL detector is both the exploration-time recorder and the
 * verification-time fire oracle, and a `location.href = { toString(){ return "javascript:…" } }` is a real
 * vector its own comment names. It is placed after §7.2.4 step 1's null relevant Document and step 2's
 * security check, because a Location those return from navigates nothing and is therefore no sink.
 *
 * ANSWERS TRUE WHEN §7.2.4's ALGORITHM ENDS HERE — an unknown destination has no address to navigate to, and
 * §Solver-half's rule is that the browser RUNS the real operation on the concrete: there is no concrete. It is
 * the same answer core/dom/element.c gives a concolic reaching the markup sink ("nothing concrete to parse;
 * the sink is what this write means"), and it is not a swallowed navigation — the navigation happens, in the
 * candidate flow, against the real §7.4.2.2 "Beginning navigation" with the attacker's real bytes, which is
 * §Re-execution discharging the constraint rather than a model of it. Navigating the exploring flow on a
 * guess would be worse than not navigating: it tears down the Document that flow is exploring for an address
 * the run never computed.
 *
 * IT TAKES THE ANSWER AND NOT THE MAGIC, and that is not a style choice — the two bodies that call it count in
 * TWO DIFFERENT NAMESPACES. `LOC_HREF` is the member table's first index and `LOC_ASSIGN` is the operations
 * enum's first constant, so both are 0 and `LOC_ORIGIN` and `LOC_REPLACE` are both 1; a shared
 * `magic == LOC_HREF || magic == LOC_ASSIGN` would read as a three-way test while being a one-way test that
 * happens to agree, and the day §7.2.4 grows a tenth attribute the agreement moves without a word. Each caller
 * knows which of ITS OWN members hands over a whole URL and says so at the call. */
static bool loc_announce_url_sink(JSContext *ctx, JSValueConst val, bool value_is_whole_url)
{
    if (!value_is_whole_url) return false;
    solve_url_sink(ctx, val);
    return concolic_is(val) != 0;
}

/* THE ASSIGNED VALUE, AS BYTES — which by this line is the SEVEN COMPONENT SETTERS' value and nothing else,
   because the three whole-URL algorithms have already ended above on a concolic.
   §7.2.4's component setters write into a URL RECORD, which is bytes, so unknown external input still has no
   modelled answer here for the same reason core/html/hyperlink.c's §4.6.3 setters do not: asking a concolic
   for a C string is the ToString this engine has no concolic semantics for, and it would take the source
   identity and the domain off the triple on the way into the address bar. Returns NULL with an exception
   pending. */
static const char *loc_value_bytes(JSContext *ctx, JSValueConst val, size_t *len)
{
    DCHECK(!concolic_is(val),
           "unknown external input was assigned to one of §7.2.4's COMPONENT setters — `location.hash = x`, "
           "`location.pathname = x` and their five siblings parse the value into a copy of this document's "
           "URL record as bytes, which takes the source identity and the domain off the triple, and the "
           "address this navigable then carries is unknown input with nothing left saying so. Build the "
           "URL-record write over a concolic component (core/url/url.h's url_parse_override is where the bytes "
           "land) so the component the page wrote stays unknown through §7.4.2.2's navigate and a later read "
           "of `location.search` gives back what was put there. It is NOT the missing @S URL sink — that one "
           "is announced above, from the three algorithms whose value can carry a scheme, and no component "
           "setter can produce the `javascript:` one this engine's URL class breaks out of");
    return JS_ToCStringLen(ctx, len, val);
}

/* HTML §2.4.2 "Parsing URLs"'s ENCODING-PARSE A URL, "relative to the entry settings object" — the three
 * algorithms in §7.2.4 that take a whole URL rather than one component: the `href` setter, `assign` and
 * `replace`.
 *
 * THE BASE IS THE API BASE URL AND NOT THIS LOCATION'S ADDRESS, and the two are different objects that happen
 * to be the same string until a page ships `<base href>`. §2.4.2 step: "let baseURL be environment's base URL,
 * if environment is a Document object; otherwise environment's API base URL" (§8.1.3.2 "Environment settings
 * objects" is where that field is declared), and HTML §7.2.2.6 "Script settings for Window objects" gives a
 * Window's the answer "return the current base URL of window's associated Document" — which is
 * core/dom/document.h's document_base_url, stated there as §2.4.3's DOCUMENT BASE URL and explicitly NOT the
 * address that document_url answers. Using the address would resolve `location.href = "x"` against the
 * navigable's current URL and ignore the `<base>` the page set, which is the same confusion that header
 * records having already cost this component once.
 *
 * WHOSE settings object it is remains an approximation, and it is the engine's rather than this file's: §7.2.4
 * says the ENTRY settings object (the script that ran), and a C member here runs in the realm that DEFINED it,
 * so what is reachable is the RELEVANT one. They differ only when one document sets another's location with a
 * relative URL, and every realm this instance holds is same-origin by construction. core/frame/history.c's
 * §7.2.5 push/replace state makes the same substitution at its own encoding-parse.
 *
 * §2.4.2's ENCODING half is UTF-8 for these three: the algorithm sets encoding from the environment's
 * Document's character encoding, and the only component that observably carries a non-UTF-8 one into a URL is
 * the query — which is the `search` setter's own step and asserts there. Returns false for §4.4's FAILURE,
 * which each caller turns into a SyntaxError. */
static bool loc_encoding_parse(JSContext *ctx, const char *v, size_t vlen, UrlRecord *out)
{
    UrlRecord base;
    const char *api_base = document_base_url(ctx);
    bool ok;

    url_record_init(&base);
    CHECK(url_parse(&base, api_base, strlen(api_base), NULL),
          "§7.2.2.6's API base URL of this realm is not a URL — §2.4.3 resolves a document base URL out of the "
          "document's address and a `<base href>`, both of which parsed, so an unparseable result is that "
          "resolution's bug and not this caller's");
    ok = url_parse(out, v, vlen, &base);
    url_record_free(&base);
    return ok;
}

/* EVERY SETTER §7.2.4 DECLARES, once. Its magic is the member index, exactly as the getter's is, so a member
   cannot arrive with a hand-written setter that forgets step 1 or the security check.
 *
 * THE SHARED PREAMBLE IS THE FIRST TWO STEPS AND ITS ORDER IS OBSERVABLE. Every setter here opens with "if
 * this's relevant Document is null, then return" and then the security check — so a browsing-context-less
 * Location swallows the write BEFORE anything can throw, which is what makes `loc.port = "notaport"` on a
 * removed frame a no-op rather than a SyntaxError. `href` is the one exception and the standard states it as
 * one: "The href setter intentionally has no security check."
 *
 * WHAT DIFFERS PER MEMBER IS ONLY WHAT copyURL BECOMES, which is why they are one switch and not nine
 * functions: seven of them are §4.4's basic URL parser entered at the state the member's own step names, and
 * the difference between §7.2.4's setters and §6.1's URL setters — which url.c already implements over the
 * same parser — is exactly three things, all of them tested by name in this directory:
 *   `protocol` THROWS on the parser's failure where §6.1 ignores it, and then TERMINATES rather than
 *      navigating when the result is not an HTTP(S) scheme;
 *   `search` parses in the relevant Document's CHARACTER ENCODING where §6.1's is always UTF-8;
 *   `hash` does NOT special-case the empty string (§6.1's sets the fragment to null; this one does not, and
 *      the standard's note says why: "to remain compatible with deployed scripts"), and it BAILS OUT when the
 *      fragment it computed equals the one already there — "necessary for compatibility with deployed content,
 *      which redundantly sets location.hash on scroll".
 * So url_member_set is deliberately NOT reused: it is the OTHER interface's algorithm, and the three
 * differences are the whole of what a Location is.
 *
 * AND IT IS A MACHINE, BECAUSE ITS LAST STEP RUNS THE PAGE'S CODE. §7.2.4 step 4's navigate reaches
 * §7.4.2.3.3, which fires a `navigate` event a router may cancel and then `currententrychange`, `dispose` and
 * `popstate` — so `location.hash = "#/route"` SUSPENDS inside its own assignment, siblings run, and it resumes
 * with the address already moved. A plain C body could not: reaching for a dispatch from one is a C activation
 * hosting the page's loops, which is the drive-to-completion this engine aborts on. The head of the algorithm —
 * everything that computes copyURL — runs none of the page's code and is one stage; §7.4.2.3.3 is the other,
 * and its own rest points are its work record's. */
#define LOC_NAV_STAGES(X)                                                                                     \
    X(LOC_NAV_COMPUTE,  "HTML §7.2.4's attribute setters and its `assign`/`replace` (the relevant-Document "   \
                        "and same origin-domain checks, and the one write into a copy of this Location's "     \
                        "url that the member is), ending at §7.2.4's Location-object navigate step 4")         \
    X(LOC_NAV_FRAGMENT, "HTML §7.4.2.2 beginning navigation step 11 (navigate to a fragment given navigable, " \
                        "url, historyHandling, userInvolvement, sourceElement, navigationAPIState and "        \
                        "navigationId — §7.4.2.3.3, whose own steps its work record names)")
enum { IDL_STEP_STAGE_BASE(LOC_NAV_STAGES) LOC_NAV_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const LOC_NAV_STEPS[] = { LOC_NAV_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* WHAT THE MEMBER HOLDS ACROSS THE SUSPENSION, and it is nothing of its own. Every value the head computes is
   consumed by the head — the URL record is freed where it was made, and the destination goes onto §7.4.2.3.3's
   own record as the string that parks. So the state IS that record, and the ownership is declared once, by the
   visit below, which forwards to the one list that names it. */
static void loc_nav_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    session_history_fragment_nav_visit(ctx, st, v);
}

/* THERE IS NO `release`. Everything this state holds is a JSValue the visit above names, so the teardown frees
   them through that one list — and idl_args.c asserts across a `release` that it freed nothing the declaration
   owns, which a second list here would break. */

static int js_loc_set(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                      JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    SessionHistoryFragmentNav *w = st;
    JSValueConst this_val = hdr->this_val, val;
    UrlRecord copy;
    const char *v;
    size_t vlen = 0;
    /* `navigate` is §7.2.4's OWN flag — the four setters whose steps say "terminate these steps" without
       throwing clear it — and `fragment` is which ARM of §7.4.2.2 the destination took. They are two different
       questions and were briefly one variable, which reads as "did we navigate" answering "is a suspension in
       flight". */
    bool navigate = true, fragment = false;
    int magic = idl_step_magic(hdr), r;

    STEP_DISPATCH(LOC_NAV_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(LOC_NAV_COMPUTE);
    /* Nothing has been asked for yet, so this entry's request answer belongs to nobody; the fragment stage
       below collects its own. */
    JS_FreeValue(ctx, cb_result);
    *presult = JS_UNDEFINED;
    session_history_fragment_nav_start(w);
    DCHECK(argc >= 1, "§7.2.4's setter body ran with no value — a step setter is delivered as a ONE-argument "
                      "call, so the assigned value is argv[0] and the declaration placed it there");
    val = argv[0];
    if (!loc_brand(ctx, this_val)) return JS_STEP_ABRUPT;
    loc_assert_this_realm(ctx, this_val);
    DCHECK(magic >= 0 && magic < LOC_N, "a Location setter was installed with a magic that is not a member "
                                        "index — the magic IS the index into the one member X-list");
    DCHECK(magic != LOC_ORIGIN, "§7.2.4 declares `origin` READONLY, so reaching a setter for it means one was "
                                "declared — a mistake in the install below and not anything a page can cause");

    /* STEP 1 of every one of them: "If this's relevant Document is null, then return." */
    if (loc_document_is_null(ctx)) return JS_STEP_DONE;
    /* STEP 2 for every member but `href`, whose note says it intentionally has none. */
    if (magic != LOC_HREF) loc_assert_same_origin_domain(ctx);

    /* §7.2.4's href setter step 2 DESTINATION, announced to the @S URL-context detector — see the partition
       above for why `href` is the only one of the nine members this body serves that reaches it. */
    if (loc_announce_url_sink(ctx, val, magic == LOC_HREF)) return JS_STEP_DONE;
    v = loc_value_bytes(ctx, val, &vlen);
    if (!v) return JS_STEP_ABRUPT;

    /* §7.2.4's href setter has no copyURL at all: it ENCODING-PARSES the whole value against the entry
       settings object and navigates to the result, so a failure is a SyntaxError rather than a no-op. */
    if (magic == LOC_HREF) {
        UrlRecord target;

        url_record_init(&target);
        if (!loc_encoding_parse(ctx, v, vlen, &target)) {
            url_record_free(&target);
            JS_FreeCString(ctx, v);
            JS_ThrowDOMException(ctx, "SyntaxError", "the value assigned to location.href is not a URL");
            return JS_STEP_ABRUPT;
        }
        JS_FreeCString(ctx, v);
        fragment = loc_navigate_begin(ctx, w, &target, "auto");
        url_record_free(&target);
        if (!fragment) return JS_STEP_DONE;
        STEP_GOTO(hdr->stage, LOC_NAV_FRAGMENT, NULL);
        return JS_STEP_YIELD;
    }

    /* "Let copyURL be a copy of this's url." Every remaining setter starts here, and the copy is what makes a
       refused write leave the address alone even when the parser wrote into the record before failing. */
    {
        UrlRecord here;

        loc_url(ctx, &here);
        CHECK(url_record_copy(&copy, &here), "location: this document's address could not be copied");
        url_record_free(&here);
    }

    switch (magic) {
    case LOC_PROTOCOL: {
        /* "Let possibleFailure be the result of BASIC URL PARSING the given value, FOLLOWED BY `:`, with
           copyURL as url and SCHEME START STATE as state override." The trailing colon is why `https:` and
           even `https::::` mean the same as `https` — §4.4's scheme state ignores what follows the first one,
           and the standard says so in a note right there. */
        char *with_colon = malloc(vlen + 2);

        CHECK(with_colon, "location: OOM building the protocol setter's input");
        memcpy(with_colon, v, vlen);
        with_colon[vlen] = ':';
        with_colon[vlen + 1] = 0;
        if (!url_parse_override(&copy, with_colon, vlen + 1, URL_OVERRIDE_SCHEME_START)) {
            /* "If possibleFailure is failure, then throw a SyntaxError DOMException." §4.4's scheme states
               return failure ONLY under a state override and the standard names the reader: "This indication
               of failure is used exclusively by the Location object's protocol setter." This is it. */
            free(with_colon);
            url_record_free(&copy);
            JS_FreeCString(ctx, v);
            JS_ThrowDOMException(ctx, "SyntaxError",
                                 "the value assigned to location.protocol is not a scheme");
            return JS_STEP_ABRUPT;
        }
        free(with_colon);
        /* "If copyURL's scheme is not an HTTP(S) scheme, then terminate these steps." A well-formed scheme the
           parser accepted but that this step refuses is a SILENT no-op, which is the whole difference between
           `location.protocol = "data"` and `location.protocol = "†"`. */
        if (!url_scheme_is_http_s(copy.scheme)) navigate = false;
        break;
    }
    case LOC_HOST:
        /* "If copyURL has an opaque path, then return." §4.1: a URL has EITHER path segments OR one opaque
           string, so the field IS the predicate. */
        if (copy.opaque_path) navigate = false;
        else url_parse_override(&copy, v, vlen, URL_OVERRIDE_HOST);
        break;
    case LOC_HOSTNAME:
        if (copy.opaque_path) navigate = false;
        else url_parse_override(&copy, v, vlen, URL_OVERRIDE_HOSTNAME);
        break;
    case LOC_PORT:
        /* "If copyURL cannot have a username/password/port, then return." Then the empty string sets the port
           to NULL rather than parsing — §4.1's null port, which is not the port 0. */
        if (url_cannot_have_username_password_port(&copy)) navigate = false;
        else if (vlen == 0) copy.port = -1;
        else url_parse_override(&copy, v, vlen, URL_OVERRIDE_PORT);
        break;
    case LOC_PATHNAME:
        /* "If copyURL has an opaque path, then return. Set copyURL's path to the empty list. Basic URL parse
           the given value, with copyURL as url and PATH START STATE as state override." The emptying is a
           separate step and it is why `location.pathname = "x"` replaces the path rather than appending. */
        if (copy.opaque_path) {
            navigate = false;
        } else {
            while (copy.npath) free(copy.path[--copy.npath]);
            url_parse_override(&copy, v, vlen, URL_OVERRIDE_PATH_START);
        }
        break;
    case LOC_SEARCH:
        /* "If the given value is the empty string, set copyURL's query to NULL" — null, not empty, which is
           the difference between `http://x/` and `http://x/?`. */
        if (vlen == 0) {
            free(copy.query);
            copy.query = NULL;
        } else {
            /* "Let input be the given value with a single leading `?` removed, if any. Set copyURL's query to
               the empty string. Basic URL parse input, with null, THE RELEVANT DOCUMENT'S DOCUMENT'S CHARACTER
               ENCODING, copyURL as url, and query state as state override." */
            const char *in = v[0] == '?' ? v + 1 : v;
            size_t n = v[0] == '?' ? vlen - 1 : vlen;

            DCHECK(document_encoding(ctx) == encoding_utf8(),
                   "§7.2.4's search setter must parse its query in the RELEVANT DOCUMENT'S CHARACTER ENCODING "
                   "and this document's is not UTF-8 — §4.4's `encoding` parameter is the one argument of the "
                   "basic URL parser core/url/url.h does not carry, so what would be written is a UTF-8 "
                   "percent-encoding of bytes the document says are something else (`?x=ß` is `%DF` under "
                   "windows-1252 and `%C3%9F` under UTF-8). Give url_parse_override the encoding argument and "
                   "hand it document_encoding");
            free(copy.query);
            copy.query = strdup("");
            CHECK(copy.query != NULL, "location: OOM emptying the query before the search setter's parse");
            url_parse_override(&copy, in, n, URL_OVERRIDE_QUERY);
        }
        break;
    default: {
        /* THE HASH SETTER, AND ITS TWO DEPARTURES FROM §6.1's ARE BOTH COMPATIBILITY RULES THE STANDARD
           EXPLAINS. It does NOT special-case the empty string ("unlike the equivalent API for the a and area
           elements, the hash setter does not special case the empty string, to remain compatible with deployed
           scripts"), and it BAILS OUT when the fragment it computed is the one already there ("this bailout is
           necessary for compatibility with deployed content, which redundantly sets location.hash on scroll").
           THE BAILOUT IS COMPARED AGAINST thisURLFragment — "copyURL's fragment if it is non-null; OTHERWISE
           THE EMPTY STRING" — so on a URL with no fragment at all, `location.hash = ""` computes the empty
           fragment, finds it equal, and does not navigate. Folding null into "" anywhere else would be wrong;
           here the spec does it itself, in this one step, and only for the comparison. */
        const char *in = v[0] == '#' ? v + 1 : v;
        size_t n = v[0] == '#' ? vlen - 1 : vlen;
        char *before;

        DCHECK(magic == LOC_HASH, "a Location setter was declared for a member this file does not answer — the "
                                  "magic IS the member, so an unknown one means a name was installed without a "
                                  "case to write it");
        before = strdup(copy.fragment ? copy.fragment : "");
        CHECK(before != NULL, "location: OOM recording the fragment the hash setter compares against");
        free(copy.fragment);
        copy.fragment = strdup("");
        CHECK(copy.fragment != NULL, "location: OOM emptying the fragment before the hash setter's parse");
        url_parse_override(&copy, in, n, URL_OVERRIDE_FRAGMENT);
        if (copy.fragment && strcmp(copy.fragment, before) == 0) navigate = false;
        free(before);
        break;
    }
    }

    JS_FreeCString(ctx, v);
    if (navigate) fragment = loc_navigate_begin(ctx, w, &copy, "auto");
    url_record_free(&copy);
    if (!fragment) return JS_STEP_DONE;
    /* THE PHASE LIST STEP_GOTO WOULD ASSERT OVER IS EMPTY BY CONSTRUCTION, not by inspection: the work record
       was started at the top of THIS entry and nothing has issued a request since, so there is no sub-sequence
       for the next stage's request site to collect the answer of. Reaching into the record's own phases from
       here would be this file reading fields session_history.h says nobody outside it reads. */
    STEP_GOTO(hdr->stage, LOC_NAV_FRAGMENT, NULL);
    return JS_STEP_YIELD;

    STEP_ARM(LOC_NAV_FRAGMENT);
    /* §7.4.2.3.3, DRIVEN. It owns every rest point past this line — its navigate event, then §7.4.6.2's
       navigation-API update and its `popstate` — and re-enters itself at whichever of them its record holds,
       so a resume here runs none of the member's own steps again. A navigation a `navigate` listener CANCELED
       answers 0 exactly as a completed one does, and the member returns undefined either way: §7.2.4's setters
       have no return value and §7.4.2.3.3 step 5's return is not an error. */
    r = session_history_fragment_nav_run(ctx, w, cb_result, out_cb, out_argc);
    if (r != 0) return r;
    /* SET ON THE ENTRY THAT FINISHES, not once in the head: `presult` is an out-parameter of EACH entry, so a
       member that wrote it before it parked would leave the resumed entry's answer as whatever the driver's
       slot held. */
    *presult = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static const IdlStepDecl LOC_SET_DECL = {
    js_loc_set, sizeof(SessionHistoryFragmentNav), loc_nav_visit, NULL,
    "HTML §7.2.4's Location attribute setters, over its Location-object navigate", LOC_NAV_STEPS
};

/* §7.2.4's `assign(url)` and `replace(url)`, which are ONE body because they differ in exactly two things the
 * standard states one line apart: `replace` has no security check ("the replace() method intentionally has no
 * security check") and it navigates with historyHandling "replace". Everything else — step 1's null relevant
 * Document, the encoding-parse against the entry settings object, and the SyntaxError a failed parse throws —
 * is written identically in both.
 *
 * IT IS THE SAME MACHINE AS THE SETTERS AND SHARES THEIR STAGE LIST, because it is the same algorithm with a
 * different head: `location.assign("#/route")` and `location.hash = "#/route"` reach §7.4.2.2 with the same
 * destination and must take the same arm. Two stage lists over one algorithm is the drift one declaration
 * exists to remove — and a version of this member that stayed a plain C body would have had to CRASH on a
 * fragment destination the setter beside it handles, which is one capability answering differently depending
 * on how the page spelled the call. */
static int js_loc_assign_replace(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                 JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    SessionHistoryFragmentNav *w = st;
    JSValueConst this_val = hdr->this_val;
    UrlRecord target;
    const char *v;
    size_t vlen = 0;
    int magic = idl_step_magic(hdr), r;
    bool fragment;

    STEP_DISPATCH(LOC_NAV_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(LOC_NAV_COMPUTE);
    JS_FreeValue(ctx, cb_result);
    *presult = JS_UNDEFINED;
    session_history_fragment_nav_start(w);
    if (!loc_brand(ctx, this_val)) return JS_STEP_ABRUPT;
    loc_assert_this_realm(ctx, this_val);
    DCHECK(argc >= 1, "§7.2.4's assign/replace ran with no argument — `USVString url` is required, so §3.6's "
                      "arity check answered that before this body was entered");
    DCHECK(magic == LOC_ASSIGN || magic == LOC_REPLACE,
           "§7.2.4's assign/replace body ran with a magic neither of its two members declares");

    if (loc_document_is_null(ctx)) return JS_STEP_DONE;             /* step 1, both */
    if (magic == LOC_ASSIGN) loc_assert_same_origin_domain(ctx);    /* step 2, assign only */

    /* §7.2.4's `USVString url` ARGUMENT IS THE WHOLE DESTINATION for both of these — the assert above has just
       established that this body's magic is one of its own two — so both are the @S URL sink unconditionally.
       See the partition above the byte conversion. */
    if (loc_announce_url_sink(ctx, argv[0], true)) return JS_STEP_DONE;
    v = loc_value_bytes(ctx, argv[0], &vlen);
    if (!v) return JS_STEP_ABRUPT;
    url_record_init(&target);
    if (!loc_encoding_parse(ctx, v, vlen, &target)) {
        url_record_free(&target);
        JS_FreeCString(ctx, v);
        JS_ThrowDOMException(ctx, "SyntaxError", "the URL passed to location.%s could not be parsed",
                             magic == LOC_ASSIGN ? "assign" : "replace");
        return JS_STEP_ABRUPT;
    }
    JS_FreeCString(ctx, v);
    fragment = loc_navigate_begin(ctx, w, &target, magic == LOC_ASSIGN ? "auto" : "replace");
    url_record_free(&target);
    if (!fragment) return JS_STEP_DONE;
    STEP_GOTO(hdr->stage, LOC_NAV_FRAGMENT, NULL);
    return JS_STEP_YIELD;

    STEP_ARM(LOC_NAV_FRAGMENT);
    r = session_history_fragment_nav_run(ctx, w, cb_result, out_cb, out_argc);
    if (r != 0) return r;
    /* SET ON THE ENTRY THAT FINISHES, not once in the head: `presult` is an out-parameter of EACH entry, so a
       member that wrote it before it parked would leave the resumed entry's answer as whatever the driver's
       slot held. */
    *presult = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static const IdlStepDecl LOC_ASSIGN_DECL = {
    js_loc_assign_replace, sizeof(SessionHistoryFragmentNav), loc_nav_visit, NULL,
    "HTML §7.2.4's `assign` and `replace`, over its Location-object navigate", LOC_NAV_STEPS
};

/* §7.2.4's `reload()`: "let document be this's relevant Document; if document is null, then return; if
 * document's origin is not same origin-domain with the entry settings object's origin, then throw a
 * SecurityError; RELOAD document's node navigable."
 *
 * ITS LAST STEP IS HTML §7.4.3 "Reloading and traversing"'s RELOAD, WHICH RUNS THE PAGE'S CODE, so this member
 * is a machine like the setters and assign/replace above it. §7.4.3 step 1 fires a push/replace/reload navigate
 * event with navigationType "reload", and a router's `navigate` listener may `preventDefault()` it — so a
 * plain C body could neither wait for the answer nor honour it, and reaching for the dispatch from one would be
 * a C activation hosting the page's loops.
 *
 * ALL FOUR OF §7.4.3's OPTIONAL ARGUMENTS ARE AT THEIR DEFAULTS HERE, which is what "reload document's node
 * navigable" with no further words means: navigationAPIState null (so step 1.3 leaves the destination state at
 * the active entry's), userInvolvement "none" — NOT "browser UI", which is why step 1 is taken at all rather
 * than skipped — and apiMethodTracker null. §7.2.6.7's `navigation.reload()` is the caller that passes others.
 *
 * THE STATE IS §7.4.3's OWN RECORD and this member holds nothing beside it: steps 1-3 compute no value that
 * outlives them, and the destination §7.4.3 parks on is taken off the active session history entry by
 * navigable_reload_begin rather than by anything here. */
#define LOC_RELOAD_STAGES(X)                                                                                   \
    X(LOC_RELOAD_CHECKS,    "HTML §7.2.4's `reload()` steps 1-3 (this's relevant Document, and the same "       \
                            "origin-domain security check), ending at step 4's reload of the Document's node "  \
                            "navigable")                                                                       \
    X(LOC_RELOAD_NAVIGABLE, "HTML §7.4.3's reload a navigable (its navigate event and everything past it, "     \
                            "whose own rest points its work record names)")
enum { IDL_STEP_STAGE_BASE(LOC_RELOAD_STAGES) LOC_RELOAD_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const LOC_RELOAD_STEPS[] = { LOC_RELOAD_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* THE OWNERSHIP IS §7.4.3's, DECLARED ONCE, BY FORWARDING — the same shape and the same reason as
   loc_nav_visit above: a second list here would be a second place a field added to that record has to be
   named, and the one that is not updated leaks it. */
static void loc_reload_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    navigable_reload_visit(ctx, st, v);
}

static int js_loc_reload(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                         JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    NavigableReloadWork *w = st;
    JSValueConst this_val = hdr->this_val;
    int r;

    (void)argc; (void)argv;

    STEP_DISPATCH(LOC_RELOAD_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(LOC_RELOAD_CHECKS);
    /* Nothing has been asked for yet, so this entry's request answer belongs to nobody; the stage below
       collects its own. */
    JS_FreeValue(ctx, cb_result);
    *presult = JS_UNDEFINED;
    navigable_reload_start(w);
    if (!loc_brand(ctx, this_val)) return JS_STEP_ABRUPT;
    loc_assert_this_realm(ctx, this_val);

    /* STEP 2 — and it is why `loc.reload()` on a browsing-context-less Location is the NO-OP the standard says
       it is rather than a throw: the return comes before the security check. */
    if (loc_document_is_null(ctx)) return JS_STEP_DONE;
    loc_assert_same_origin_domain(ctx);                   /* step 3 */
    /* STEP 4, CREATED HERE AND DRIVEN AT THE NEXT STAGE — the same split every member above takes, and for the
       same reason: between this line and the navigate event's answer the page's own listeners run. */
    navigable_reload_begin(ctx, w);
    /* THE PHASE LIST STEP_GOTO WOULD ASSERT OVER IS EMPTY BY CONSTRUCTION: the work record was started at the
       top of THIS entry and nothing has issued a request since, so there is no sub-sequence for the next
       stage's request site to collect the answer of. */
    STEP_GOTO(hdr->stage, LOC_RELOAD_NAVIGABLE, NULL);
    return JS_STEP_YIELD;

    STEP_ARM(LOC_RELOAD_NAVIGABLE);
    /* §7.4.3, DRIVEN. It owns every rest point past this line and re-enters itself at whichever of them its
       record holds, so a resume here runs none of this member's own steps again. A reload a `navigate`
       listener CANCELLED answers 0 exactly as a completed one does, and the member returns undefined either
       way: §7.2.4's `reload()` has no return value and §7.4.3 step 1.5's return is not an error. */
    r = navigable_reload_run(ctx, w, cb_result, out_cb, out_argc);
    if (r != 0) return r;
    /* SET ON THE ENTRY THAT FINISHES, not once in the head: `presult` is an out-parameter of EACH entry, so a
       member that wrote it before it parked would leave the resumed entry's answer as whatever the driver's
       slot held. */
    *presult = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static const IdlStepDecl LOC_RELOAD_DECL = {
    js_loc_reload, sizeof(NavigableReloadWork), loc_reload_visit, NULL,
    "HTML §7.2.4's `reload`, over §7.4.3's reload a navigable", LOC_RELOAD_STEPS
};

/* §7.2.4's `ancestorOrigins`: "if this's relevant Document is null, then return this's EMPTY DOMStringList; if
 * this's relevant Document's origin is not same origin-domain with the entry settings object's origin, then
 * throw a SecurityError; assert: this's relevant Document's ancestor origins list is not null; otherwise
 * return this's relevant Document's ancestor origins list."
 * THE FIRST ARM IS A REAL ANSWER AND NOT A DEGRADED ONE — the standard gives a Location "an associated empty
 * DOMStringList" precisely so this member has something to return, and notes that it "cannot carry state
 * across navigations because it is only returned when there is no relevant Document". */
static JSValue js_loc_ancestor_origins(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)magic;
    if (!loc_brand(ctx, this_val)) return JS_EXCEPTION;
    loc_assert_this_realm(ctx, this_val);

    /* "IF THIS'S RELEVANT DOCUMENT IS NULL, THEN RETURN THIS'S ASSOCIATED EMPTY DOMStringList." ASSOCIATED is
       the load-bearing word and this arm was ignoring it: it MINTED a DOMStringList per read, so two reads of a
       removed frame's `ancestorOrigins` answered two different objects. `location-ancestor-origins-new-object`
       asserts that exact pair in sequence — a NEW object once the frame is removed (this list, not the
       Document's) and the SAME object on every read after it. */
    if (loc_document_is_null(ctx))
        return realm_value_get(ctx, g_empty_asl_slot);
    loc_assert_same_origin_domain(ctx);
    /* "Assert: this's relevant Document's ancestor origins list is not null. Return this's relevant Document's
       ancestor origins list." §7.3.2.1 built BOTH lists at the creation (core/dom/document.c) — the internal
       one of origins and this serialization of it — which is what makes the answer a snapshot of the tree this
       Document was created in rather than a walk of the tree as it stands now. */
    return document_ancestor_origins(ctx);
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

/* §7.2.1.3.1's list, asserted against the object this realm just built — see the paragraph where the list is
 * cited above.
 *
 * WHAT IT ASSERTS TURNED OVER, and the old direction was the wrong one. It used to require the two entries on
 * CrossOriginProperties(Location) to be ABSENT, so that no cross-origin document could reach an unfiltered
 * Location — and the effect was that §7.2.4's `href` setter and `replace` could not be implemented at all,
 * which is a mitigation that deletes the interface it protects. The filter is a property of the object that
 * CROSSES an origin boundary, and that object is core/frame/remote_location.c's: a Location for a Document a
 * PEER holds, which exposes these two names and answers every other read with §7.2.1.3.2's SecurityError.
 *
 * SO THE LIST IS ASSERTED AS A LIST OF THINGS THAT EXIST, which is what makes it live rather than decorative:
 * the filtered object needs both members to be there to expose. An entry that is MISSING is the failure — a
 * §7.2.1.3.1 list naming a member this interface does not have is a list that has drifted from §7.2.4's IDL,
 * which is the one way these two sections can disagree. BOTH ENTRIES ARE CHECKED, not just the operation:
 * `href` is a member of this object exactly as `replace` is, and the half that was left out is the half the
 * filter's one WRITE goes through. */
static void loc_assert_cross_origin_surface(JSContext *ctx, JSValueConst loc)
{
    int i;

    for (i = 0; i < LOCATION_XO_N; i++) {
        JSAtom a = JS_NewAtom(ctx, LOCATION_CROSS_ORIGIN[i]);
        int has;

        DCHECK(a != JS_ATOM_NULL, "a §7.2.1.3.1 cross-origin property name could not be interned");
        has = JS_HasProperty(ctx, loc, a);
        JS_FreeAtom(ctx, a);
        CHECK(has >= 0, "[[HasProperty]] over an engine-built Location runs none of the page's code and has "
                        "nothing to throw with");
        DCHECK(has,
               "HTML §7.2.1.3.1's CrossOriginProperties(Location) names a member this Location does not have "
               "— the two sections have drifted, because §7.2.4's IDL is what declares the member and §7.2.1 "
               "only says which of them survive a cross-origin filter. Add the member to §7.2.4's install "
               "below, or take the entry off core/frame/remote_location.h's list if §7.2.1 no longer names it");
    }
    /* THE ATTRIBUTE HALF'S SECOND FACT, which the presence check above cannot see: §7.2.1.3.1 gives `href`
       [[NeedsSetter]] true, and the member table's third column carries that. What is checked here is that the
       column and the install agree — a cross-origin entry marked [[NeedsSetter]] whose member ships no setter
       would be a filtered Location with nothing behind its one write. */
    for (i = 0; i < LOC_N; i++)
        DCHECK(LOC_XO[i] != LOC_XO_SETTER || !LOC_READONLY[i],
               "a Location member is on §7.2.1's cross-origin list with [[NeedsSetter]] and is declared "
               "READONLY by the member table — the two columns describe the same member and cannot both be "
               "right, and the filtered object §7.2.1 describes would expose a setter that does not exist");
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

    /* §3.4.10's placement, one member at a time: an OWN, NON-CONFIGURABLE accessor on the Location itself.
       §7.2.4 declares a SETTER for every member but `origin`, and the table is what says which — the loop has
       no `if` of its own to get out of step with the IDL. */
    for (i = 0; i < LOC_N; i++)
        idl_install_accessor_unforgeable(ctx, loc, LOC_NAME[i], js_loc_get, i, g_loc_set[i]);
    /* §7.2.4's tenth attribute, readonly, with its own getter — see the member table for why it is not in it. */
    idl_install_accessor_unforgeable(ctx, loc, "ancestorOrigins", js_loc_ancestor_origins, 0, -1);
    /* §3.7.7's UNFORGEABLE OPERATIONS: on the INSTANCE, non-writable and non-configurable, which is what
       [LegacyUnforgeable] means for an operation exactly as it does for an attribute. */
    idl_install_method_unforgeable(ctx, loc, "assign", 1, g_loc_assign);
    idl_install_method_unforgeable(ctx, loc, "replace", 1, g_loc_replace);
    idl_install_method_unforgeable(ctx, loc, "reload", 0, g_loc_reload);
    /* §3.7.8: the stringifier is unforgeable, so its property is on the object with
       {[[Writable]]: false, [[Enumerable]]: true, [[Configurable]]: false}. */
    JS_DefinePropertyValueStr(ctx, loc, "toString",
                              JS_NewCFunction(ctx, js_loc_to_string, "toString", 0), JS_PROP_ENUMERABLE);
    loc_pin_to_primitive(ctx, loc);
    loc_assert_cross_origin_surface(ctx, loc);

    /* §7.2.2 The Window object's `[PutForwards=href, LegacyUnforgeable] readonly attribute Location location`,
       with BOTH halves of the declaration. It was a plain writable data property, so
       `window.location = {href: "..."}` REPLACED the Location object outright — which is the forgery the
       unforgeable annotations on both sides of this exist to prevent — and then the forwarding half was absent,
       so `window.location = url` reached nothing at all: a readonly accessor with no setter DROPS the
       assignment in sloppy mode and throws in strict, where a browser NAVIGATES. The setter is Web IDL's
       §3.7.6 forwarding and not a body this file writes: it ends in `Set(Q, "href", V, false)`, which is the
       `href` setter three screens up — the same machine, entered through its own member, so the navigation
       suspends and resumes exactly as `location.href = url` does. */
    idl_install_accessor_unforgeable(ctx, global, "location", js_win_location, 0, g_win_location_set);
    realm_value_set(ctx, g_obj_slot, loc);
    {
        /* §7.2.4's associated empty DOMStringList, built WITH the Location and never again. */
        JSValue empty = JS_NewArray(ctx);

        CHECK(!JS_IsException(empty), "location: §7.2.4's empty DOMStringList could not be allocated");
        realm_value_set(ctx, g_empty_asl_slot, dom_string_list_new(ctx, empty));
    }
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
    static const IdlArgType URL_ARG[] = { IDL_USVSTRING };
    int i;

    DCHECK(g_obj_slot < 0, "location_init ran twice — the class, the slot and the two sources are declared once "
                           "per AGENT");
    /* §7.2.4's EIGHT SETTERS AND THREE OPERATIONS, declared here because the IDL pool is sealed after agent
       init: minting one inside location_install_realm would work for the first realm and abort on the second,
       which is exactly the per-realm-fact-from-a-shared-place shape this file already carries a paragraph
       about. The member table's readonly column decides which get one, so the IDL and this loop cannot drift.
       `USVString url` for both assign and replace; `reload()` takes nothing, which is a NULL type list. */
    for (i = 0; i < LOC_N; i++)
        g_loc_set[i] = LOC_READONLY[i] ? -1
                                       : idl_setter_id_step(ctx, IDL_USVSTRING, false, &LOC_SET_DECL, i);
    g_loc_assign  = idl_method_id_step(ctx, URL_ARG, 1, NULL, 0, &LOC_ASSIGN_DECL, LOC_ASSIGN);
    g_loc_replace = idl_method_id_step(ctx, URL_ARG, 1, NULL, 0, &LOC_ASSIGN_DECL, LOC_REPLACE);
    g_loc_reload  = idl_method_id_step(ctx, NULL, 0, NULL, 0, &LOC_RELOAD_DECL, 0);
    /* §7.2.2 The Window object's `location` is [PutForwards=href], so its setter is declared HERE — beside the
       members the pool must issue before it is sealed — and its ALGORITHM is Web IDL §3.7.6's, shared with
       every other carrier. The pair is (`location`, `href`) because that is what the IDL line says, and the
       `href` it names is resolved at assignment time off the receiver rather than baked in here, which is what
       makes `otherWindow.location = u` reach the OTHER window's Location. */
    g_win_location_set = idl_setter_id_put_forwards(ctx, "location", "href");
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
    g_empty_asl_slot = realm_value_declare(ctx, "HTML §7.2.4 the Location's associated empty DOMStringList");
    realm_declare_intrinsic(location_install_realm);
    agent_state_class(LOC_COMPONENT, &g_loc_class, "§7.2.4's per-realm prototype slot and brand");
    agent_state_id(LOC_COMPONENT, &g_obj_slot, "the per-realm slot §7.2.4's one Location lives in");
    agent_state_id(LOC_COMPONENT, &g_empty_asl_slot,
                   "the per-realm slot §7.2.4's associated empty DOMStringList lives in");
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
    int i;

    DCHECK(g_obj_slot >= 0, "§7.2.4's Location was released in an agent that never declared it");
    /* The prototypes, the interface objects and the Locations are the REALMS' — each is released with its
       context. What the agent holds is the brand, the slots and the member ids the IDL pool issued — every one
       this file declares, §7.2.2's forwarding setter included. */
    g_obj_slot = -1;
    g_empty_asl_slot = -1;
    g_loc_class = 0;
    for (i = 0; i < LOC_N; i++) g_loc_set[i] = -1;
    g_loc_assign = g_loc_replace = g_loc_reload = -1;
    g_win_location_set = -1;
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
