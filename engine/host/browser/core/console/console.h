/* The Console Standard's `console` namespace — Blink core/inspector's ConsoleMessage surface, as the standard
 * writes it rather than as a shape.
 *
 * IT IS NOT A LOGGER, IT IS FOUR PIECES OF OBSERVABLE STATE AND TWO ALGORITHMS THAT RUN THE PAGE'S CODE.
 * §1.2's COUNT MAP, §1.3's GROUP STACK and §1.4's TIMER TABLE are per namespace object, so per realm, and each
 * is mutated by members a page calls in a loop — which makes them per-FLOW state that has to time-travel like
 * any other. §2.2's Formatter calls %String%, %parseInt% and %parseFloat% on the page's values, and every one
 * of those is one `toString`/`valueOf` away from being the page's own code, so the members that reach it are
 * step machines and suspend exactly where an `await` does.
 *
 * WHY IT IS A COMPONENT AND NOT A BAG OF NOOPS. `js_noop` as an interface member is banned here, and this is
 * the interface it would have been easiest to write that way: Printer is implementation-defined, so a console
 * whose twenty operations all returned undefined would pass every test a page can write except the ones that
 * matter — `console.log("%s", {toString(){ sideEffect() }})` runs that side effect in every browser, and
 * `console.count("x")` twice prints 1 then 2 because the standard keeps a map.
 *
 * WHAT ITS ABSENCE COST, measured: `console` is on browser/platform_names.h, so solver/absent.c correctly left
 * an unresolved `console` alone and the page's own ReferenceError named this component — the forcing function
 * working exactly as designed. A real third-party bundle mirrored from vuejs.org reaches
 * `console.warn('[BannerStudio] Missing affiliate param')` on its early-return path, so the ReferenceError
 * aborted the whole IIFE and every endpoint behind it went with the flow. */
#ifndef ENGINE_HOST_BROWSER_CORE_CONSOLE_CONSOLE_H
#define ENGINE_HOST_BROWSER_CORE_CONSOLE_CONSOLE_H

#include "quickjs.h"

/* THE AGENT'S HALF: the twenty operations' pool entries and the realm-value slot their state lives in.
   Declares the per-realm intrinsic, so every realm — the agent's first and every child navigable's — gets its
   own namespace object with its own maps. */
void console_init(JSContext *ctx);

/* THE AGENT'S HALF, UNDONE. The namespace object and the three maps are the REALMS' and go with their
   contexts; what the agent holds is nineteen pool ids and one slot number. */
void console_free(void);

#endif
