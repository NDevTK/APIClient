/* DOM §4.5 createEvent — THE LEGACY EVENT FACTORY, AND WHY IT IS ITS OWN FILE.
 *
 * `document.createEvent('Event')` predates event constructors and is still how a great deal of shipped code
 * makes an event: a bundle that synthesises a `click`, a router that fires a custom `navigate`, every library
 * old enough to support IE. Without it those paths throw at the factory and the code after them — which is
 * where the fetches are — never runs. The member is Document's, but nothing about the algorithm is: it is a
 * TABLE of legacy interface names and a construction, so it lives with the interfaces it constructs and
 * Document merely calls it.
 *
 * THE TABLE IS THE SPEC'S, AND ITS EMPTY ROWS ARE THE ALGORITHM RATHER THAN A GAP. §4.5 step 3 throws
 * "NotSupportedError" for a name outside the table and step 4 throws the SAME exception for a name in it whose
 * interface is "not exposed" — so an interface this engine has not built is not a hole here, it is the
 * branch the spec writes for exactly that case. A row with no maker IS that branch.
 *
 * AND THE PAIRING IS ASSERTED, so it cannot rot. A row with no maker is only correct while the interface is
 * genuinely absent from the global, so that is what the DCHECK checks: build `MouseEvent` without wiring it
 * here and the first `createEvent('MouseEvent')` CRASHES naming the row to fill, rather than telling the page
 * that an interface it can see does not exist.
 *
 * THE MATCH IS ASCII-CASE-INSENSITIVE AND ONLY ASCII. `UİEvent` (LATIN CAPITAL LETTER I WITH DOT ABOVE)
 * and `UıEvent` (DOTLESS I) are not `UIEvent`, and a Unicode-aware lowercase would say they were. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/events/before_unload_event.h"
#include "core/events/create_event.h"
#include "core/events/event.h"
#include "core/events/message_event.h"

typedef JSValue (*EventMaker)(JSContext *ctx);

/* §4.5's table, in its own order: the alias in ASCII lowercase (which is how the match is made), the interface
   it names, and this engine's maker for that interface — NULL where the interface is not exposed. */
typedef struct {
    const char *alias;
    const char *iface;
    EventMaker  make;
} CreateEventRow;

/* The Event rows' maker. §4.5 steps 6-8 overwrite type/isTrusted and unset the initialized flag afterwards, so
   what a maker owes is only "a default instance of this interface". */
static JSValue make_event(JSContext *ctx)
{
    return event_new_untrusted(ctx, "", /*bubbles*/ false, /*cancelable*/ false);
}

static JSValue make_message_event(JSContext *ctx)
{
    return message_event_new(ctx, "", JS_NULL, "", JS_NULL, JS_UNDEFINED);
}

/* HTML §7.2.7.7 declares NO constructor for BeforeUnloadEvent, so this row is the ONLY way a page makes one —
   which is what "There are no BeforeUnloadEvent-specific initialization methods" leaves behind, and why the
   first row of §4.5's table names an interface whose IDL a page cannot otherwise reach.
   §4.5 steps 6-8 then run over it, and this interface's own state is untouched by them: `returnValue` stays the
   empty string §7.2.7.7 says creation gives it. */
static JSValue make_before_unload_event(JSContext *ctx)
{
    return before_unload_event_new(ctx);
}

/* THERE IS NO PageTransitionEvent ROW, and that is §4.5's table rather than a gap here: the table names
   BeforeUnloadEvent, CompositionEvent, CustomEvent, DeviceMotionEvent, DeviceOrientationEvent, DragEvent,
   Event, FocusEvent, HashChangeEvent, KeyboardEvent, MessageEvent, MouseEvent, StorageEvent, TextEvent,
   TouchEvent and UIEvent — and not that one. `document.createEvent('PageTransitionEvent')` is step 3's
   NotSupportedError in every browser, and adding a row would make this engine answer where the spec throws. */
static const CreateEventRow CREATE_EVENT[] = {
    { "beforeunloadevent",      "BeforeUnloadEvent",      make_before_unload_event },
    { "compositionevent",       "CompositionEvent",       NULL },
    { "customevent",            "CustomEvent",            NULL },
    { "devicemotionevent",      "DeviceMotionEvent",      NULL },
    { "deviceorientationevent", "DeviceOrientationEvent", NULL },
    { "dragevent",              "DragEvent",              NULL },
    { "event",                  "Event",                  make_event },
    { "events",                 "Event",                  make_event },
    { "focusevent",             "FocusEvent",             NULL },
    { "hashchangeevent",        "HashChangeEvent",        NULL },
    { "htmlevents",             "Event",                  make_event },
    { "keyboardevent",          "KeyboardEvent",          NULL },
    { "messageevent",           "MessageEvent",           make_message_event },
    { "mouseevent",             "MouseEvent",             NULL },
    { "mouseevents",            "MouseEvent",             NULL },
    { "storageevent",           "StorageEvent",           NULL },
    { "svgevents",              "Event",                  make_event },
    { "textevent",              "TextEvent",              NULL },
    { "touchevent",             "TouchEvent",             NULL },
    { "uievent",                "UIEvent",                NULL },
    { "uievents",               "UIEvent",                NULL },
};

/* Is `s` the row's alias, compared the way §4.5 compares — ASCII case-insensitively, byte for byte. A byte
   >= 0x80 is part of a non-ASCII code point and matches nothing in the table, which is the whole point. */
static bool alias_is(const char *s, const char *alias)
{
    size_t i;

    for (i = 0; alias[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if (c != (unsigned char)alias[i]) return false;
    }
    return s[i] == 0;
}

/* Is the interface EXPOSED — §4.5 step 4's question, asked of the realm rather than of a list.
   AN OWN SLOT, NEVER A PROPERTY LOOKUP. An interface object is an own property of the global, and a LOOKUP for
   a name that is not there walks the prototype chain into the solver's absent-state seam, which mints a
   concolic for it and forks — 42 forks per document, one per row this engine has not built, for a question
   whose answer is a fact about the engine. It is also why a platform name that is absent THROWS on a lookup:
   the read has to be the internal one. */
static bool iface_exposed(JSContext *ctx, JSValueConst global, const char *iface)
{
    JSAtom a = JS_NewAtom(ctx, iface);
    JSValue v;
    int has;

    CHECK(a != JS_ATOM_NULL, "createEvent: an interface name could not be interned");
    has = JS_GetOwnSlot(ctx, &v, global, a);
    JS_FreeAtom(ctx, a);
    if (has > 0) JS_FreeValue(ctx, v);
    return has > 0;
}

JSValue create_event(JSContext *ctx, JSValueConst global, const char *iface)
{
    unsigned k;

    DCHECK(iface != NULL, "createEvent reached its algorithm with no interface name — the IDL declaration is "
                          "what converts the argument, and a body that coerces one runs the page's toString "
                          "from C");
    for (k = 0; k < sizeof(CREATE_EVENT) / sizeof(CREATE_EVENT[0]); k++) {
        const CreateEventRow *row = &CREATE_EVENT[k];
        JSValue ev;

        if (!alias_is(iface, row->alias)) continue;
        if (!row->make) {
            /* step 4: the interface is not exposed. ASSERTED, because the row is only right while that is
               true — an interface built without a maker here would be exposed AND refused. */
            DCHECK(!iface_exposed(ctx, global, row->iface),
                   "a createEvent row has no maker for an interface this realm exposes — fill the row in "
                   "core/events/create_event.c, or the page can see an interface the factory denies");
            return JS_ThrowDOMException(ctx, "NotSupportedError",
                                        "createEvent: this document does not expose that event interface");
        }
        DCHECK(iface_exposed(ctx, global, row->iface),
               "a createEvent row makes an interface this realm does not expose — the maker and the global "
               "are the same interface and one of them was not installed");
        ev = row->make(ctx);
        if (JS_IsException(ev)) return ev;
        /* steps 6-8: type is the empty string, isTrusted is false, and the INITIALIZED FLAG IS UNSET — which
           is what makes dispatching one before initEvent an InvalidStateError, and is the only observable
           difference between this object and one the constructor produced. */
        event_uninitialize(ctx, ev);
        return ev;                                   /* step 9 */
    }
    /* step 3: no row matched. */
    return JS_ThrowDOMException(ctx, "NotSupportedError",
                                "createEvent: that is not a legacy event interface name");
}
