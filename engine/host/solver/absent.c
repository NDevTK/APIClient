/* WHICH ABSENT GLOBAL IS INPUT, AND WHICH IS A COMPONENT THIS ENGINE OWES.
 *
 * Both wear the same shape — a name that resolves nowhere — and answering them the same way loses either the
 * whole logged-in surface or every forcing function.
 *
 * SERVER-INJECTED APP STATE is unknown INPUT. `window.__FLAGS`, `__USER`, `__NEXT_DATA__` are written into the
 * document by the server for a logged-in visitor and simply absent for this one, so what they hold is not
 * `undefined` — it is UNKNOWN. Answering `undefined` makes `__FLAGS.admin` throw on the first field access and
 * buries every endpoint behind it, which is precisely the surface this tool exists to reach: the bundle ships
 * the auth and admin code to a logged-out visitor and it never runs. Symbolic instead, so the gate FORKS and
 * the logged-in arm is explored.
 *
 * A WEB API THIS ENGINE HAS NOT BUILT is honestly absent. Its ReferenceError is the forcing function that names
 * the component to write, and handing back a symbol instead would let a flow run past a missing capability and
 * report a surface it never reached.
 *
 * THE DISTINCTION IS WEB IDL'S TO MAKE, NOT A LIST'S. A global name belongs to the platform exactly when the
 * IDL exposes it on Window, and browser/platform_names.h is that set, generated from @webref/idl by
 * engine/idlgen.mjs. It replaced a 22-name list typed into main.c — and the difference is not cosmetic: every
 * interface off that list (Node, Element, Event, DOMException, HTMLElement, and ~1300 more) was mistaken for
 * app state, so a branch on one FORKED instead of throwing. A page touching eight of them multiplied the
 * frontier by 256; a WPT document exhausted 2.8 GB in forty seconds doing it. A hand-maintained allowlist
 * cannot be right about a surface of this size, and the moment it is wrong the error is silent. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "solver/absent.h"
#include "solver/concolic.h"
#include "browser/platform_names.h"

#define PLATFORM_NAMES_N ((int)(sizeof(PLATFORM_NAMES) / sizeof(PLATFORM_NAMES[0])))

/* The generated table is SORTED (idlgen sorts it), so membership is a binary search — a linear scan of 1300
   names would run on every unresolved global read, of which a forced-exec run does a great many. The sort is
   the generator's invariant and is asserted here, at the one place that depends on it. */
int absent_is_platform_name(const char *name)
{
    int lo = 0, hi = PLATFORM_NAMES_N - 1;

    DCHECK(name != NULL, "absent_is_platform_name: no name");
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int c = strcmp(name, PLATFORM_NAMES[mid]);
        if (c == 0)
            return 1;
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return 0;
}

JSValue absent_global_hook(JSContext *ctx, JSAtom name)
{
    const char *s = JS_AtomToCString(ctx, name);
    JSValue r = JS_UNINITIALIZED;
    char shape[128], src[128];

    if (!s)
        return JS_UNINITIALIZED;
    /* A name the platform owns is a component this engine owes; leave the read alone so its throw names it. */
    if (absent_is_platform_name(s))
        goto done;
    /* Anything else the page reads and nothing defines is the server's to have injected. Example-free: nothing
       here knows what a logged-in visitor's flags WOULD hold, and inventing one fabricates an observation. */
    snprintf(shape, sizeof(shape), "{%s}", s);
    snprintf(src,   sizeof(src),   "%s", s);
    r = concolic_new(ctx, shape, src, JS_UNDEFINED);
done:
    JS_FreeCString(ctx, s);
    return r;
}
