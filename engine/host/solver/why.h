/* Runtime-reasoned @WHY — the engine reached a SHOULD-NEVER-HAPPEN with a RUNTIME phase+reason string (unlike
 * check.h's DFAIL, whose message is a compile-time literal). Same disposition as DFAIL: DEV emits @WHY and aborts
 * at the origin (a @WHY is a forcing function, never log-and-continue; the design goal is ZERO @WHY); RELEASE
 * surfaces it but does not crash the USER (the exemption). A tiny component so every subsystem can raise it
 * without main.c owning the definition. */
#ifndef ENGINE_HOST_SOLVER_WHY_H
#define ENGINE_HOST_SOLVER_WHY_H
#include "quickjs.h"
void why_add(JSContext *ctx, const char *phase, const char *reason);
#endif
