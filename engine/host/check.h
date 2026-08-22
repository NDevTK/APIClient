/* check.h — the CHECK / DCHECK invariant system.  See CLAUDE.md's offensive-programming law.
 *
 * TWO distinct assertions, split by whether the invariant must ALSO hold in a RELEASE build:
 *
 *  DCHECK(cond, msg) — DEV-ONLY. A SHOULD-NEVER-HAPPEN: a design invariant, a not-yet-built capability, a
 *    COW/host gap, a NULL that can't be NULL, an out-of-range index, an unbalanced tramp/COW stack, a delta
 *    that fails to restore its baseline. In a DEV build it ABORTS LOUD at the ORIGIN (the failing expression +
 *    file:line + msg, as a machine-readable @WHY the JS bridge surfaces) — the offensive forcing function that
 *    makes a bad state caught where it is BORN a fix, not a bug you build on. In a RELEASE build it is COMPILED
 *    OUT (the release exemption: the state is genuinely unsupportable outside development, so the USER is not
 *    crashed on it — this is NOT a fallback, it is the absence of one). Because it vanishes in release, a
 *    DCHECK condition MUST be side-effect-free. A DCHECK is NEVER recoverable control flow: if a state must be
 *    HANDLED at runtime, that is an `if`, not a DCHECK.
 *
 *  DFAIL(msg) — the UNCONDITIONAL DCHECK: control reached a point it should-never reach (a @WHY gap, an
 *    unimplemented case). Same dev-abort / release-continue semantics as DCHECK; use where there is no single
 *    boolean to test (the caller falls through to a best-effort path in release).
 *
 *  CHECK(cond, msg) — ALWAYS active (dev AND release). A UNIVERSAL invariant that must hold in EVERY build:
 *    memory-allocation success (a dropped flow corrupts the frontier), a physical floor (RAM/disk — a real
 *    resource wall), a security / authorization boundary, data integrity. Aborting in release is CORRECT —
 *    proceeding past a violated CHECK is worse than crashing. Emitted as @E.
 *
 * Rule of thumb: DCHECK asserts "the engine's OWN logic is correct" (dev-only, the moat's development); CHECK
 * asserts "we must NOT proceed even in production" (safety/security). When unsure, it is a DCHECK. */
#ifndef ENGINE_HOST_CHECK_H
#define ENGINE_HOST_CHECK_H
#include <stdio.h>
#include <stdlib.h>

#ifndef APICLIENT_DEV
#define APICLIENT_DEV 1   /* development is the default; a release build compiles with -DAPICLIENT_DEV=0 */
#endif

/* Emit the machine-readable assertion line the JS bridge (linesToAnalysis) surfaces (@E / @WHY), then abort.
   Keep `msg` a plain literal/simple string — it is embedded in JSON unescaped. */
#define APICLIENT_ASSERT_EMIT(tag, condstr, msg) do { \
    fflush(stdout); \
    fprintf(stderr, tag " {\"phase\":\"assert\",\"cond\":\"%s\",\"at\":\"%s:%d\",\"reason\":\"%s\"}\n", \
            (condstr), __FILE__, __LINE__, (msg)); \
    fflush(stderr); \
  } while (0)

/* CHECK — always fatal (dev + release). */
#define CHECK(cond, msg)  do { if (!(cond)) { APICLIENT_ASSERT_EMIT("@E", #cond, (msg)); abort(); } } while (0)
#define CHECK_FAIL(msg)   do { APICLIENT_ASSERT_EMIT("@E", "unreachable", (msg)); abort(); } while (0)

/* DCHECK — dev-only fatal; compiled out in release (side-effect-free condition required). */
#if APICLIENT_DEV
#define DCHECK(cond, msg) do { if (!(cond)) { APICLIENT_ASSERT_EMIT("@WHY", #cond, (msg)); abort(); } } while (0)
#define DFAIL(msg)        do { APICLIENT_ASSERT_EMIT("@WHY", "unreachable", (msg)); abort(); } while (0)
#else
#define DCHECK(cond, msg) ((void)sizeof(cond))   /* type-checked, never evaluated */
#define DFAIL(msg)        ((void)0)
#endif

#endif
