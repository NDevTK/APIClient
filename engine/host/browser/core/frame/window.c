/* THE WINDOW INTERFACE — Blink core/frame, the browsing-context half of the global object.
 *
 * WHY THIS IS A COMPONENT AND NOT TWO LINES IN main.c. `window` and `self` were assigned there directly, which
 * covered the two names a bundle spells most often and nothing else. The rest of the browsing-context surface
 * — `parent`, `top`, `frames`, `opener`, `closed` — was simply missing, and a missing property on the global
 * is `undefined`, not a throw: 139 of 175 WPT dom/nodes documents died inside testharness.js's
 *
 *     var w = self; while (w != w.parent) { w = w.parent; ... }
 *
 * with "cannot read property 'parent' of undefined" on the SECOND iteration. A real browser ends that loop
 * immediately because a top-level window's `parent` IS the window.
 *
 * WHICH BROWSING CONTEXT IS THIS. HTML 7.2.2 answers window/self/frames with this Window's own WindowProxy,
 * and answers parent/top with the parent (or top) navigable's proxy, falling back to this one when there is
 * none. This engine runs ONE document per instance — SECURITY.md's one-WASM-instance-per-DOCUMENT — and holds
 * no channel to an embedder, so within the world this instance models the document IS its own top: parent and
 * top are the window. That is the concrete truth of the modelled context, not a shrug: making it concolic
 * would model an ignorance the engine does not have, and would fork the loop above without end. When the
 * cross-WASM chain that lets an embedded document reach its embedder exists, parent/top resolve through it and
 * this is where that lands.
 *

 * WHAT IT DOES NOT DO. This installed the PLATFORM GLOBALS as well — URL, URLSearchParams, FormData, Blob, the
 * four stream interfaces, TextEncoder/Decoder and their stream forms. None of them is a browsing-context
 * member and none belongs to this component; they were here only because main.c happened to be the one caller.
 * That cost a real gate: the WPT runner installs every one of those itself, so it could not call this without
 * double-installing, and it therefore had NO browsing-context members at all — `window` was not defined, and
 * every test in html/browsers/the-window-object failed on its first line. Two jobs in one function is what
 * made the second caller impossible; each install now sits with its own caller.
 *
 * `name` IS THE NAVIGABLE'S — §7.11's, the same attribute a WindowProxy answers — so it is not computed here
 * at all; window_proxy_name_value is. It is ATTACKER INPUT exactly when nobody stated it: the name survives
 * navigation, so whoever opened the document sets it, which is why CLAUDE.md lists it beside cookies and the
 * referrer — and it is a computed value when this engine's own `open(url, target)` named the navigable. Read
 * through a GETTER either way, for the same reason location.hash is: a candidate run substitutes a source at
 * MINT time, and a source minted once at install could never receive a breakout. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/frame/window.h"
#include "core/frame/window_proxy.h"
#include "core/dom/document.h"
#include "core/url/url.h"
#include "core/frame/bar_prop.h"
#include "core/html/html_iframe.h"
#include "core/frame/window_proxy.h"
#include "core/events/event_target.h"
#include "core/dom/collections.h"
#include "core/dom/document.h"
#include "core/dom/node.h"
#include "core/idl_args.h"
#include "solver/cow.h"

/* §7.2.5's `closed` IS THE NAVIGABLE'S, not the Window's — which is why it is not read from a byte here.
   `window.closed` and `iframe.contentWindow.closed` are the SAME fact about the SAME navigable, and a byte in
   this file made them two: closing through one left the other reporting open. The navigable's state lives on
   its WindowProxy (per-flow, captured into the running flow's delta like every other binding it holds), and
   §7.2.5.1 gives a navigable exactly one proxy — so reading it there is reading the one answer. */
static JSValue js_win_closed(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)this_val; (void)magic;
    return JS_NewBool(ctx, window_proxy_closed(ctx, document_window_proxy(ctx)));
}

/* §7.2.5.2 `close()`. A REAL STATE CHANGE, never a no-effect: the navigable is closed and `closed` reports it
   from that point on, which is exactly what a page tests before touching a popup again. */
static JSValue js_win_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)this_val; (void)argc; (void)argv; (void)magic;
    window_proxy_set_closed(ctx, document_window_proxy(ctx));
    return JS_UNDEFINED;
}

/* §7.2.5's `focus()` and `blur()` move SYSTEM focus between windows, and §7.2.5 defines no scriptable result
   for either — nothing observable to a script changes. This is the documented no-effect the IDL audit permits,
   not a stub standing in for a value: there is no value to compute. `stop()` is the same shape here, because
   this engine has no in-flight navigation to abort. */
static JSValue js_win_noeffect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv; (void)magic;
    return JS_UNDEFINED;
}

/* §7.2.5's `length`: the number of DOCUMENT-TREE CHILD NAVIGABLES. A GETTER over a real walk, never a stored
   number — a page appends a frame, removes it and reads this, and a count something forgot to adjust is wrong
   in exactly that case. It was absent entirely, which is `undefined` rather than 0 and is not the same answer
   for any page that tests it. */
static JSValue js_win_length(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)this_val; (void)magic;
    return JS_NewInt32(ctx, iframe_child_navigable_count(ctx));
}

/* §7.2.5's BROWSING-CONTEXT LINKS, each answered by this realm's navigable — see window_proxy.h.
 *
 * The mapping between a window's two spellings — another navigable is its PROXY, this one is the global — is
 * window_proxy.c's `win_or_proxy`, applied by every member that can answer with a navigable. */
static JSValue js_win_parent(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)this_val; (void)magic;
    return window_proxy_parent(ctx, document_window_proxy(ctx));
}

static JSValue js_win_top(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)this_val; (void)magic;
    return window_proxy_top_of(ctx, document_window_proxy(ctx));
}

static JSValue js_win_opener(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)this_val; (void)magic;
    return window_proxy_opener(ctx, document_window_proxy(ctx));
}

/* §7.2.5's `opener` SETTER, and it has TWO branches that do different things — which is why it is written out
   here rather than declared [Replaceable].
     null  -> DISOWN: the navigable's opener link is severed, and NO own property is defined. A page writes this
              to cut a popup loose from the document that opened it, and defining an own `null` instead would
              answer null to the page while leaving the link intact for everything that reads the navigable.
     other -> Web IDL's CreateDataPropertyOrThrow(this, "opener", V) — the same operation [Replaceable] performs,
              reached through the same one implementation. */
static JSValue js_win_set_opener(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    (void)magic;
    if (JS_IsNull(val)) {
        window_proxy_disown_opener(ctx, document_window_proxy(ctx));
        return JS_UNDEFINED;
    }
    return idl_replace_with_value(ctx, this_val, "opener", val) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

/* §7.11's `name` — THE NAVIGABLE'S, which is the same attribute `w.name` reads through the WindowProxy and is
   answered from the same record. It was a second source here: a source-only concolic with no example, so
   `open(url, "chan42")` gave "chan42" to the opener and an example-free unknown to the popup's own script,
   which is the popup unable to learn the name it was created with. §7.11 says "return the current name of
   this's navigable"; window_proxy_name_value is where that is computed, including whether it is known at all. */
static JSValue js_win_get_name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return window_proxy_name_value(ctx, document_window_proxy(ctx));
}

/* §7.11's `name` is SETTABLE, and it was not — the accessor had no setter at all, so `window.name = "x"` was a
   silent no-op and a page that names itself to be reached by `open(url, "x")` could not. It renames the
   NAVIGABLE, which is the same write `w.name = "x"` performs from outside. */
static JSValue js_win_set_name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc;
    return window_proxy_name_assign(ctx, document_window_proxy(ctx), argv[0]);
}

/* §7.2.5.1's EXOTIC OWN-PROPERTY BEHAVIOUR — `window[0]`, and why it is not a property anyone sets.
 *
 * The global is a LEGACY PLATFORM OBJECT. Its SUPPORTED PROPERTY INDICES are its document-tree child
 * navigables, and `window[i]` is the i-th one's WindowProxy. Materialising those as own data properties would
 * be right until the first frame was appended, removed or moved — the set changes on every tree mutation, and
 * a count or a slot that a mutation forgot to adjust is wrong in exactly the case the spec files test. Exotic
 * behaviour is what §7.2.5.1 describes and what this is: every answer is computed from the tree at the moment
 * it is asked.
 *
 * THE THREE OPERATIONS ARE NOT SYMMETRIC, and each asymmetry is a spec sentence:
 *   [[GetOwnProperty]] answers only for a SUPPORTED index — an unsupported one is an ordinary miss, so
 *     `window[7] = "x"` on a frameless page really does create an ordinary property.
 *   [[DefineOwnProperty]] returns FALSE for EVERY array index, supported or not. `Object.defineProperty(window,
 *     0, …)` fails on a page with no frames at all, which is what distinguishes it from the above.
 *   [[Delete]] returns false only for a SUPPORTED index; anything else is the ordinary "nothing to delete".
 *
 * IT IS REACHED ONLY AFTER THE ORDINARY LOOKUP MISSES (quickjs consults a class's exotic get_own_property when
 * find_own_property found nothing), so a global variable read pays for this only when it was going to fail
 * anyway — and the index test is the engine's own, not a re-parse of the atom's text. */
static JSClassID g_window_class;

/* "IS A Window OBJECT" — the brand, off the class the global carries. DOM §2.9 step 6.9.5 asks it of every
   parent the event path walk reaches, because a Window is the one path entry that is NOT a node and so is the
   one the shadow-including ancestor test cannot answer for. Asking it as "is it not a node" would be an
   inference about who else can appear in a path rather than a fact about this object, and the class is what
   makes it a fact: HTML's global IS the Window, and window_install gives the global exactly this class. */
bool window_is(JSValueConst v)
{
    return g_window_class != 0 && JS_GetClassID(v) == g_window_class;
}

/* The child navigable this index names, or false. Owned on true. */
static bool win_supported_index(JSContext *ctx, JSAtom prop, JSValue *out)
{
    uint32_t idx;
    JSValue nav;

    if (!JS_AtomIsIndex(ctx, &idx, prop) || idx > (uint32_t)INT32_MAX) return false;
    nav = iframe_child_navigable(ctx, (int)idx);
    if (JS_IsUndefined(nav)) return false;
    *out = nav;
    return true;
}

static int win_get_own(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom prop)
{
    JSValue v;
    (void)obj;
    if (!win_supported_index(ctx, prop, &v)) return 0;
    if (!desc) { JS_FreeValue(ctx, v); return 1; }
    /* §7.2.5.1: { [[Value]]: the child's WindowProxy, [[Writable]]: false, [[Enumerable]]: true,
       [[Configurable]]: true }. */
    desc->flags = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE;
    desc->getter = JS_UNDEFINED;
    desc->setter = JS_UNDEFINED;
    desc->value = v;
    return 1;
}

static int win_define_own(JSContext *ctx, JSValueConst obj, JSAtom prop, JSValueConst val,
                          JSValueConst getter, JSValueConst setter, int flags)
{
    uint32_t idx;

    if (JS_AtomIsIndex(ctx, &idx, prop)) {
        if (flags & JS_PROP_THROW) {
            JS_ThrowTypeError(ctx, "cannot define an indexed property on the window object");
            return -1;
        }
        return 0;
    }
    /* EVERYTHING ELSE IS ORDINARY, and the exotic hook REPLACES the ordinary path rather than preceding it —
       so the ordinary path is re-entered here explicitly, with the exotic step suppressed. Forgetting this
       would not break `window[0]`; it would break every `var` and every property a page defines on the
       global. */
    return JS_DefineProperty(ctx, obj, prop, val, getter, setter, flags | JS_PROP_NO_EXOTIC);
}

static int win_delete(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    JSValue v;
    (void)obj;
    /* Reached only when the ordinary own-property scan found nothing, so "not an index" is "nothing to
       delete", which is true. */
    if (!win_supported_index(ctx, prop, &v)) return 1;
    JS_FreeValue(ctx, v);
    return 0;
}

static int win_own_names(JSContext *ctx, JSPropertyEnum **ptab, uint32_t *plen, JSValueConst obj)
{
    int n = iframe_child_navigable_count(ctx), i;
    JSPropertyEnum *tab;

    (void)obj;
    /* §7.2.5.1 [[OwnPropertyKeys]] lists the supported indices FIRST, in order. quickjs merges what this
       returns with the object's ordinary keys, so only the indices belong here. */
    tab = n ? js_malloc(ctx, sizeof(*tab) * (size_t)n) : NULL;
    if (n && !tab) return -1;
    for (i = 0; i < n; i++) {
        tab[i].is_enumerable = true;
        tab[i].atom = JS_NewAtomUInt32(ctx, (uint32_t)i);
    }
    *ptab = tab;
    *plen = (uint32_t)n;
    return 0;
}

static const JSClassExoticMethods WINDOW_EXOTIC = {
    .get_own_property = win_get_own,
    .get_own_property_names = win_own_names,
    .delete_property = win_delete,
    .define_own_property = win_define_own,
    /* The lookup is a walk of the document tree and a read of each iframe's navigable slot — no page code, by
       construction, which is what lets the engine's own accessor walks run it from C. */
    .get_own_property_no_user_code = true,
};

static const JSClassDef WINDOW_CLASS = { "Window", NULL, NULL, NULL, &WINDOW_EXOTIC };

/* §7.2.5's INTERFACE PROTOTYPE OBJECT, and the chain it sits in: window -> Window.prototype ->
   EventTarget.prototype -> Object.prototype. The global had Object.prototype directly, so `Window` did not
   exist as a name, `window instanceof EventTarget` was false, and every member of Window was an OWN property
   of the global — which is where Web IDL puts only the [LegacyUnforgeable] ones (`window`, `self`, `location`,
   `top`, `document`). Everything else is declared on the prototype, and a page reads the difference: an
   `assert_own_property` on the wrong object, a descriptor test, a `delete window.closed`.
   §7.2.5's WindowProperties object sits between Window.prototype and EventTarget.prototype in the real chain
   and is what NAMED access (`window.myIframeName`) is declared on. It is not built here, so it is not claimed
   here either: this chain is two links of the three, and the third arrives with named access. */

/* HTML §7.3.3 — NAMED ACCESS ON THE WINDOW OBJECT, and the object it is declared on.
 *
 * `window.myFrameName` and `window.someElementId` are not properties of the global and not properties of
 * Window.prototype: Web IDL puts an interface's NAMED PROPERTIES on a separate object one link further up the
 * chain — window -> Window.prototype -> WindowProperties -> EventTarget.prototype — precisely so that a page's
 * own `window.foo = 1` SHADOWS the named property rather than colliding with it. The corpus walks that chain
 * link by link, so the object is not a formality: without it the chain is short by one and every level below
 * compares against the wrong prototype.
 *
 * THE ORDER IN §7.3.3 IS THE WHOLE ALGORITHM, and each branch is a different kind of answer:
 *   a document-tree child NAVIGABLE with that name wins outright, and answers with its WindowProxy;
 *   a single named ELEMENT that is itself a container with a navigable answers with THAT navigable's proxy —
 *     `window.myIframeName` is the frame's window, not the <iframe> element;
 *   a single named element answers with the element;
 *   more than one answers with a live HTMLCollection, because a page reads `.length` off it.
 * A "named element" is two rules, not one: any HTML element whose `id` matches, plus embed/form/img/object/
 * iframe whose `name` attribute matches — see collections.c, which owns the filter.
 *
 * [LegacyUnenumerableNamedProperties] is what Window carries, so the descriptor is
 * { writable: true, enumerable: FALSE, configurable: true }. */

/* The child navigable whose browsing-context name is `name`, or JS_UNDEFINED. Owned on success. */
static JSValue win_named_navigable(JSContext *ctx, const char *name)
{
    int n = iframe_child_navigable_count(ctx), i;

    for (i = 0; i < n; i++) {
        JSValue nav = iframe_child_navigable(ctx, i);
        if (JS_IsUndefined(nav)) continue;
        if (!strcmp(window_proxy_name(nav), name)) return nav;
        JS_FreeValue(ctx, nav);
    }
    return JS_UNDEFINED;
}

/* §7.3.3's value for `name`, or JS_UNDEFINED when the name is not supported. Owned on success. */
static JSValue win_named_value(JSContext *ctx, const char *name)
{
    JSValue coll, doc, first, second, out = JS_UNDEFINED;

    if (!*name) return JS_UNDEFINED;   /* the empty name is no name at all, and no attribute carries it */
    out = win_named_navigable(ctx, name);
    if (!JS_IsUndefined(out)) return out;
    if (!document_root_node(ctx)) return JS_UNDEFINED;

    doc = node_wrap(ctx, document_root_node(ctx));
    coll = collections_named(ctx, doc, name);
    JS_FreeValue(ctx, doc);
    if (JS_IsException(coll)) return JS_UNDEFINED;
    /* HOW MANY, asked by INDEX rather than by `length`. An HTMLCollection's `length` is an IDL accessor, and
       reaching a getter from C is the one thing this engine refuses — there is no flow base under a C
       activation. The indexed read is the collection's exotic own-property behaviour, which runs no page code
       and is declared so; and the two indices are all this branch needs, because §7.3.3 only distinguishes
       none, exactly one, and more than one. */
    first  = JS_GetPropertyUint32(ctx, coll, 0);
    second = JS_GetPropertyUint32(ctx, coll, 1);
    if (JS_IsUndefined(first)) {
        JS_FreeValue(ctx, first); JS_FreeValue(ctx, second); JS_FreeValue(ctx, coll);
        return JS_UNDEFINED;
    }
    if (!JS_IsUndefined(second)) {
        JS_FreeValue(ctx, first); JS_FreeValue(ctx, second);
        return coll;   /* the LIVE collection is the answer, not a snapshot of it */
    }
    JS_FreeValue(ctx, second);
    out = first;
    JS_FreeValue(ctx, coll);
    /* §7.3.3: a single named element that HAS a content navigable answers with the navigable's WindowProxy —
       `window.myIframeName` is the frame's window, and a page that then reads `.document` off it would get an
       element otherwise. */
    /* ASKED OF THE COMPONENT THAT OWNS THE SLOT, never read off `contentWindow`: that attribute is an IDL
       accessor, and this walk runs from C with no flow base under it — which is why the exotic declares
       `get_own_property_no_user_code`. Reading it aborted window-properties, window-named-properties and
       nested-context, each on the first named frame it reached. */
    {
        JSValue nav = iframe_navigable(ctx, out);
        if (JS_IsObject(nav)) { JS_FreeValue(ctx, out); return nav; }
        JS_FreeValue(ctx, nav);
    }
    return out;
}

static int win_named_get_own(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom prop)
{
    const char *name;
    JSValue v;

    (void)obj;
    /* A SYMBOL IS NOT A NAMED PROPERTY. Web IDL's named-property algorithm applies to STRING property names
       only, and stringifying a symbol here would answer for `Symbol.toStringTag` with whatever element happened
       to carry the id "Symbol(Symbol.toStringTag)" — and, far more often, would run a whole tree walk for every
       symbol lookup that reaches this object. */
    {
        JSValue pv = JS_AtomToValue(ctx, prop);
        bool sym = JS_IsSymbol(pv);
        JS_FreeValue(ctx, pv);
        if (sym) return 0;
    }
    name = JS_AtomToCString(ctx, prop);
    if (!name) return 0;
    v = win_named_value(ctx, name);
    JS_FreeCString(ctx, name);
    if (JS_IsUndefined(v)) return 0;
    if (!desc) { JS_FreeValue(ctx, v); return 1; }
    desc->flags = JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE;   /* [LegacyUnenumerableNamedProperties] */
    desc->getter = JS_UNDEFINED;
    desc->setter = JS_UNDEFINED;
    desc->value = v;
    return 1;
}

static const JSClassExoticMethods WINDOW_PROPS_EXOTIC = {
    .get_own_property = win_named_get_own,
    /* The lookup is a walk of the document tree and a read of content attributes — no page code, by
       construction, which is what lets the engine's own accessor walks run it from C. */
    .get_own_property_no_user_code = true,
};
static const JSClassDef WINDOW_PROPS_CLASS = { "WindowProperties", NULL, NULL, NULL, &WINDOW_PROPS_EXOTIC };
static JSClassID g_window_props_class;


/* THE CLASSES ARE THE AGENT'S, THE PROTOTYPES ARE THE REALM'S — and that line is Web IDL's, not a convenience.
   A class id is a registration in the JSRuntime and there is one runtime per agent; a PROTOTYPE is an object,
   and §3.7 gives every realm its own, which is why `frames[0].Window.prototype !== Window.prototype` in a
   browser. So this registers, and window_install builds. */
static int g_id_close, g_id_focus, g_id_blur, g_id_stop;   /* declared once per agent — see window_init */
static int g_id_opener_set;   /* §7.2.5's `opener` setter, declared with them for the same reason */

void window_init(JSContext *ctx)
{
    DCHECK(g_window_class == 0, "window_init ran twice — a class is registered once per agent, and a second "
                                "registration would give one interface two ids that compare unequal");
    JS_NewClassID(JS_GetRuntime(ctx), &g_window_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_window_class, &WINDOW_CLASS) == 0,
          "the Window class could not be registered");
    JS_NewClassID(JS_GetRuntime(ctx), &g_window_props_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_window_props_class, &WINDOW_PROPS_CLASS) == 0,
          "the WindowProperties class could not be registered");
    /* THE MEMBERS ARE DECLARED HERE AND INSTALLED PER REALM. A declaration builds a pool entry and a member has
       ONE, so declaring inside the install would mint a second entry for the second realm's prototype — which
       is the same shape as a per-wrapper mint and is what the pool's seal asserts against. */
    bar_prop_init(ctx);   /* §7.2.5.3's BarProp class, one per agent */
    g_id_opener_set = idl_setter_id(ctx, IDL_ANY, false, js_win_set_opener, 0);
    g_id_close = idl_method_id(ctx, NULL, 0, js_win_close, 0);
    g_id_focus = idl_method_id(ctx, NULL, 0, js_win_noeffect, 0);
    g_id_blur  = idl_method_id(ctx, NULL, 0, js_win_noeffect, 1);
    g_id_stop  = idl_method_id(ctx, NULL, 0, js_win_noeffect, 2);
}

void window_install(JSContext *ctx, JSValueConst global, const char *url)
{
    JSValue g = (JSValue)global, gp, props, etp;

    DCHECK(JS_IsObject(global), "window_install was given something that is not the global object");
    DCHECK(g_window_class != 0, "window_install ran before window_init registered the Window class");

    /* §7.2.5.1's exotic behaviour comes from the object's CLASS, and the global was created by the context
       before any host class existed — so it is given one here. It owns no per-object data (no finalizer, no
       gc_mark), which is what makes handing it to an already-built object sound. */
    CHECK(JS_SetGlobalClass(ctx, g_window_class) == 0,
          "the global object would not take the Window class — §7.2.5.1's indexed access has nowhere to live");

    /* THE PROTOTYPE CHAIN, before any member is installed on any of its objects. PER REALM, and held by
       quickjs's own per-context class-prototype slot rather than by a static in this file: a second same-origin
       document is a second realm in this agent, and a static would have given both the first realm's Window
       objects — so `frames[0].Window` would have been this document's. */
    etp = event_target_proto(ctx);   /* THIS realm's — §3.6's [Global] rule is read off the member's own ctx */
    props = JS_NewObjectProtoClass(ctx, etp, g_window_props_class);
    JS_FreeValue(ctx, etp);
    CHECK(!JS_IsException(props), "the WindowProperties object could not be allocated");
    idl_interface_tag(ctx, props, "WindowProperties");
    gp = JS_NewObjectProto(ctx, props);
    CHECK(!JS_IsException(gp), "Window.prototype could not be allocated");
    idl_interface_tag(ctx, gp, "Window");
    JS_SetClassProto(ctx, g_window_props_class, props);   /* the realm owns them from here */
    JS_SetClassProto(ctx, g_window_class, JS_DupValue(ctx, gp));
    JS_SetPrototype(ctx, g, gp);
    /* ECMAScript gives THE GLOBAL OBJECT an own @@toStringTag of "global", and it shadows the interface tag
       that §3.7.3 puts on Window.prototype — so `Object.prototype.toString.call(window)` answered
       "[object global]" where every browser answers "[object Window]". That own property is the plain-host
       global's, not a Window's: HTML's global IS the Window, and the tag it carries is the interface's. */
    JS_DeleteProperty(ctx, g, JS_WellKnownSymbolAtom(JS_WKS_TO_STRING_TAG), 0);
    event_target_install_interface(ctx, g);   /* §2.7's interface object, now that its prototype is in a chain */
    JS_SetPropertyStr(ctx, g, "Window", idl_interface_object(ctx, "Window", gp));

    /* 7.2.2: window, self and frames all return THIS Window's proxy, and the global object IS that proxy here —
       so `window.X`, `self.X` and a bare `X` are one read spelled three ways. */
    /* EVERY MEMBER BELOW IS AN OWN PROPERTY OF THE GLOBAL, because Window is declared [Global] — Web IDL
       §3.7.3: an interface with [Global] defines its members on the GLOBAL OBJECT, not on the interface
       prototype object, which is left with nothing on it but its @@toStringTag and `constructor`. That is not
       a placement detail: `Object.getOwnPropertyDescriptor(window, "opener")` is `undefined` when the member
       is one link up the chain, and window-properties.https.html reads exactly that for every attribute and
       every method Window has.
       The comment that stood here said the opposite — that [LegacyUnforgeable] is what puts a member on the
       global and that `frames`, `parent` and `opener` therefore "are declared on the prototype like every
       other member". [LegacyUnforgeable] decides the ATTRIBUTES (non-configurable, so a page cannot shadow or
       delete), never the LOCATION; on a [Global] interface there is no other location. */
    JS_DefinePropertyValueStr(ctx, g, "window", JS_DupValue(ctx, global), JS_PROP_ENUMERABLE);
    /* `self` is [Replaceable], not [LegacyUnforgeable] — `window` is the unforgeable one. A page may
       overwrite `self` and the IDL says so; a fixed own value said it could not. */
    idl_install_replaceable_value(ctx, g, "self", JS_DupValue(ctx, global));
    /* §7.2.5 marks `top` [LegacyUnforgeable] — an OWN property — but its VALUE is the navigable's, and `top`
       is a WALK of the parent chain, so a grandchild answers with the top-level traversable rather than with
       itself. An own ACCESSOR, not an own value frozen at install time. */
    JS_DefinePropertyGetSet(ctx, g, JS_NewAtom(ctx, "top"),
                            JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_win_top, "get top", 0,
                                                 JS_CFUNC_getter_magic, 0),
                            JS_UNDEFINED, JS_PROP_ENUMERABLE);
    idl_install_replaceable_value(ctx, g, "frames", JS_DupValue(ctx, global));   /* [Replaceable] */
    /* §7.2.5's `parent` and `opener` ARE THE NAVIGABLE'S, so they are read from this realm's own WindowProxy
       rather than answered here. They were two FIXED values behind two comments explaining why an embedder
       could not exist — "no embedder is reachable from this instance" and "the document was navigated to, not
       opened by a script in another navigable" — and both were true exactly while one instance was one
       document. A §7.4 child realm in this agent HAS a creator, and a popup whose `opener` is null cannot post
       back to the page that opened it, which is the whole of what a popup is for. */
    idl_install_replaceable(ctx, g, "parent", js_win_parent, 0);   /* [Replaceable] readonly */
    /* `opener` is NOT [Replaceable]: the IDL declares `attribute any opener`, and §7.2.5 gives it setter steps
       of its own whose null branch DISOWNS rather than assigns. Its non-null branch is [Replaceable]'s define,
       reached through the same implementation. */
    idl_install_accessor(ctx, g, "opener", js_win_opener, 0, g_id_opener_set);
    /* §7.2.5.3's six user-interface bars. */
    bar_prop_install(ctx, g);

    /* §7.2.5 `frameElement` — the element this navigable is nested THROUGH. A top-level navigable is nested
       through nothing, so it is null: the real answer for what this is, not a placeholder for one. */
    JS_SetPropertyStr(ctx, g, "frameElement", JS_NULL);

    /* `closed` is a GETTER over the NAVIGABLE's per-flow state, because close() changes it. */
    idl_install_accessor(ctx, g, "closed", js_win_closed, 0, -1);
    idl_install_replaceable(ctx, g, "length", js_win_length, 0);   /* [Replaceable] readonly */
    idl_install_method(ctx, g, "close", 0, g_id_close);
    idl_install_method(ctx, g, "focus", 0, g_id_focus);
    idl_install_method(ctx, g, "blur",  0, g_id_blur);
    idl_install_method(ctx, g, "stop",  0, g_id_stop);

    /* The Window's origin, serialized — the principal, concrete for the same reason Location's is: a bundle
       compares it and builds URLs out of it, and a shape there loses every endpoint behind the comparison.
       IT IS §4.7's SERIALIZATION, not a substring of the address. The scan that stood here took everything
       before the first `/?#` after `://`, which is not an origin: it kept a default port that §4.7 drops, kept
       userinfo that an origin never has, and had no answer at all for a scheme with an OPAQUE origin — a
       `data:` document's `origin` is the string "null", and this gave it nothing. */
    if (url && *url) {
        UrlRecord rec;
        if (url_parse(&rec, url, strlen(url), NULL)) {
            char *origin = url_serialize_origin(&rec);
            idl_install_replaceable_value(ctx, g, "origin", JS_NewString(ctx, origin));
            free(origin);
        }
        url_record_free(&rec);
    }

    JS_DefinePropertyGetSet(ctx, g, JS_NewAtom(ctx, "name"),
                            JS_NewCFunction(ctx, js_win_get_name, "get name", 0),
                            JS_NewCFunction(ctx, js_win_set_name, "set name", 1),
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeValue(ctx, gp);   /* the realm's class-proto slot and the chain hold it now */
}

void window_free(JSContext *ctx)
{
    (void)ctx;
    g_window_class = 0;
    g_window_props_class = 0;
    bar_prop_free(ctx);
}
