/* Clipboard (navigator.clipboard) — the Async Clipboard API as a real Blink-style module. readText()/read()
 * return ATTACKER-CONTROLLED content ({clipboard}, paste-jacking: the attacker controls what the victim copies),
 * so `clipboard.readText().then(t => el.innerHTML = t)` is a clipboard-XSS the engine detects and replay-verifies
 * (delivery precondition: a user paste). writeText()/write() are Promise<undefined> (not a scriptable sink).
 * Split out of navigator.c so this attacker-source concern owns its own file, DCHECKs, and a subagent behind one
 * contract (clipboard_make); unbuilt Clipboard members DFAIL via the shared idl_dfail_wrap audit trap. */
#ifndef ENGINE_HOST_BROWSER_MODULES_CLIPBOARD_H
#define ENGINE_HOST_BROWSER_MODULES_CLIPBOARD_H
#include "quickjs.h"

/* The navigator.clipboard object (a Clipboard interface instance). */
JSValue clipboard_make(JSContext *ctx);

#endif
