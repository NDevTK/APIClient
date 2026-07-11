/* CredentialsContainer (navigator.credentials) — the Credential Management API as a real Blink-style module, and
 * the moat's HEADLINE auth gate. get()/store()/create() resolve to a CONCOLIC Credential with a TRUTHY example so
 * `credentials.get().then(c => c && loginWith(c))` reaches the LOGGED-IN arm (the auth/session/admin code a
 * logged-out visitor never runs) while the concolic still forks the null (no-credential) arm; c.id / c.token are
 * concolic (unknown user/attacker input). Never fires a real request. Split out of navigator.c so the auth-moat
 * concern owns its file + DCHECKs + a subagent behind one contract (credentials_make); unbuilt members DFAIL. */
#ifndef ENGINE_HOST_BROWSER_MODULES_CREDENTIALS_H
#define ENGINE_HOST_BROWSER_MODULES_CREDENTIALS_H
#include "quickjs.h"

/* The navigator.credentials object (a CredentialsContainer instance). */
JSValue credentials_make(JSContext *ctx);

#endif
