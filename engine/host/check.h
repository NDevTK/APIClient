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
 * asserts "we must NOT proceed even in production" (safety/security). When unsure, it is a DCHECK.
 *
 * A POINTER INVARIANT IS NOT ASSERTED BY THE HARDWARE ON THE TARGET THIS ENGINE SHIPS ON, so it is asserted
 * HERE OR NOWHERE. This engine is built for two targets and they do not agree about what a bad pointer does.
 * A native binary meets an unmapped page and the process dies at the instruction that dereferenced it, which
 * is why a whole class of should-never-happen has never needed a macro: the fault WAS the assert. A
 * WebAssembly module has no such page. A memory access there has exactly ONE trap condition — WebAssembly
 * Core §4.6.8 Memory Instructions, the `t.load` rule's step 10: the effective address plus the access width
 * running PAST THE CURRENT MEMORY SIZE. There is no guard region and no reserved block beneath it, so
 * address 0 is ordinary readable and writable linear memory and a load through a null pointer RETURNS A
 * VALUE.
 * WHICH VALUE IS NOT A FACT TO LEAN ON EITHER, AND THAT IS THE POINT RATHER THAN A CAVEAT. It is whatever the
 * link's memory layout leaves down there, so it changes with a flag nobody was thinking about the pointer
 * when they set — and a layout that puts the machine stack lowest makes address 0 a slot a trampolining
 * engine never grows down far enough to reach, so a null WRITE lands somewhere permanently unread. Under NAN
 * boxing the read is worse than merely un-trapped: a JSValue decoded out of low memory carries a tag with no
 * reference count, so it is invisible to the refcount machinery that is the one thing that might otherwise
 * have noticed. What the native build reports as a fault at the origin, the shipped build reports as a
 * plausible datum — the failure this file exists to make impossible, arriving through the pointer.
 * THE RULE: assert the pointer EXPLICITLY, where it is ESTABLISHED — the site that decided NULL was allowed
 * is the only one that knows why — and write the condition over the quantity the DEREFERENCE depends on, not
 * over a neighbouring quantity that happens to imply it today, because the implication is precisely what the
 * next route through that site changes. A link-time checking mode exists on the wasm side and is a build's
 * choice, so no assert may assume one is on; and "it did not crash under wasm" is never evidence that a
 * pointer was good. */
#ifndef ENGINE_HOST_CHECK_H
#define ENGINE_HOST_CHECK_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* write(2) — the record is ONE syscall; see the emitter */

#ifndef APICLIENT_DEV
#define APICLIENT_DEV 1   /* development is the default; a release build compiles with -DAPICLIENT_DEV=0 */
#endif

/* THE ASSERTION LINE IS JSON, SO IT IS ESCAPED HERE AND NOT ASKED FOR BY A COMMENT. This emitter used to
 * interpolate `condstr` and `msg` with a bare `%s` under an instruction to "keep msg a plain literal" — a
 * comment standing where a mechanism belongs, and one that the two fields BOTH break in their ordinary use.
 * `condstr` is `#cond`, so a `DCHECK(strcmp(d, "none") != 0, …)` stringizes its own operand's quotes into the
 * field; and a `reason` earns its place by CITING the clause it implements, which means quoting the spec's
 * words. The result was a record announcing itself as machine-readable that `JSON.parse` rejects at the first
 * quotation mark — and every consumer had a default waiting: the shipped bridge caught the throw and pushed
 * the raw line under a generic context, and the corpus report read the fixed `cond` instead. So the ONE field
 * that names what to build was unreadable in exactly the asserts that named it best, and nothing anywhere
 * reported a parse failure. Escaping at the ONE emitter is the root fix; every author's message is then
 * ordinary prose and no caller has a rule to remember.
 * JSON (RFC 8259 §7 "Strings") requires escapes for `"`, `\` and every code point below U+0020, and permits
 * any other Unicode character unescaped — so UTF-8 bytes (the `§` in every citation) pass straight through.
 *
 * ONE WRITE, BECAUSE A LOCK CANNOT REACH THE OTHER WRITER. The record is many appends once the escaping is a
 * sequence, and an interleaved record is the failure this file exists to make impossible: a line that
 * decomposes into a location from one assert and a reason from another is a plausible datum assembled out of
 * two true ones. This was `flockfile` (POSIX.1-2001, the mechanism stdio defines for exactly this), reasoned
 * about THREADS — run-test262 drives the corpus on several of them — and it is correct for threads and
 * STRUCTURALLY UNABLE to help here, because the other writer is not a thread. wpt_runner.c forks a CHILD
 * PROCESS for a peer instance, parent and child share this file descriptor, and `flockfile` locks a FILE in
 * one address space: it has no visibility of the other process at all. It was not that the lock missed this
 * emitter or that some path bypassed it — the lock covered it and could not work.
 * So the record is COMPOSED into a stack buffer and emitted with ONE `write(2)`. That is atomic against every
 * other writer for the two things stderr is here: a pipe (POSIX.1-2001 guarantees a write of at most PIPE_BUF
 * — 4096 on Linux, which is why the buffer is sized under it — is not interleaved) and a regular file opened
 * O_APPEND. It also removes stdio from the abort path entirely, which is worth having on its own: this runs
 * where the heap may already be the reason we are aborting, and `write` allocates nothing.
 * TRUNCATION IS VISIBLE AND THE RECORD STILL PARSES. A reason longer than the buffer is a real possibility —
 * several in this tree cite a whole algorithm — so the tail is reserved, the record always closes, and an
 * overlong one says so in its own `reason` rather than arriving as silently shortened prose. */
/* Append to a fixed buffer, never past it. `*used` is the running length; every appender is bounds-checked
   against `cap` so the caller can reserve a tail it knows will fit. */
static inline void apiclient_assert_put(char *b, size_t cap, size_t *used, const char *s, size_t n)
{
  size_t room = cap - *used;
  if (n > room) n = room;
  memcpy(b + *used, s, n);
  *used += n;
}

/* JSON (RFC 8259 §7 "Strings") requires escapes for `"`, `\` and every code point below U+0020, and permits
   any other Unicode character unescaped — so UTF-8 bytes (the `§` in every citation) pass straight through. */
static inline void apiclient_assert_emit_json(char *b, size_t cap, size_t *used, const char *s)
{
  for (; *s != '\0'; s++) {
    unsigned char c = (unsigned char) *s;
    char esc[8];
    switch (c) {
    case '"':  apiclient_assert_put(b, cap, used, "\\\"", 2); break;
    case '\\': apiclient_assert_put(b, cap, used, "\\\\", 2); break;
    case '\n': apiclient_assert_put(b, cap, used, "\\n",  2); break;
    case '\r': apiclient_assert_put(b, cap, used, "\\r",  2); break;
    case '\t': apiclient_assert_put(b, cap, used, "\\t",  2); break;
    default:
      if (c < 0x20) { int k = snprintf(esc, sizeof esc, "\\u%04x", (unsigned) c);
                      apiclient_assert_put(b, cap, used, esc, (size_t) k); }
      else          apiclient_assert_put(b, cap, used, (const char *) &c, 1);
    }
  }
}

/* ONE record, ONE write. Returns nothing and cannot fail in a way the caller could act on — it is the last
   thing that happens before abort(). Partial writes are looped, because a signal can cut one short and half a
   record is the interleaving this exists to prevent. */
static inline void apiclient_assert_write(const char *b, size_t n)
{
  size_t off = 0;
  while (off < n) {
    ssize_t w = write(2, b + off, n - off);
    if (w <= 0) break;   /* the fd is gone or refuses; there is nowhere left to report that */
    off += (size_t) w;
  }
}

/* Emit the machine-readable assertion line the JS bridge (linesToAnalysis) surfaces (@E / @WHY), then abort. */
#define APICLIENT_ASSERT_EMIT(tag, condstr, msg) do { \
    /* Under PIPE_BUF (4096 on Linux) so a write to a pipe is atomic against the forked peer instance. */ \
    char apiclient_b_[3072]; \
    size_t apiclient_n_ = 0; \
    const size_t apiclient_cap_ = sizeof apiclient_b_ - 64;   /* the reserved tail */ \
    char apiclient_at_[32]; \
    fflush(stdout); \
    apiclient_assert_put(apiclient_b_, apiclient_cap_, &apiclient_n_, tag " {\"phase\":\"assert\",\"cond\":\"", \
                         sizeof(tag " {\"phase\":\"assert\",\"cond\":\"") - 1); \
    apiclient_assert_emit_json(apiclient_b_, apiclient_cap_, &apiclient_n_, (condstr)); \
    apiclient_assert_put(apiclient_b_, apiclient_cap_, &apiclient_n_, "\",\"at\":\"", 8); \
    apiclient_assert_emit_json(apiclient_b_, apiclient_cap_, &apiclient_n_, __FILE__); \
    apiclient_assert_put(apiclient_b_, apiclient_cap_, &apiclient_n_, apiclient_at_, \
                         (size_t) snprintf(apiclient_at_, sizeof apiclient_at_, ":%d", __LINE__)); \
    apiclient_assert_put(apiclient_b_, apiclient_cap_, &apiclient_n_, "\",\"reason\":\"", 12); \
    apiclient_assert_emit_json(apiclient_b_, apiclient_cap_, &apiclient_n_, (msg)); \
    /* THE TAIL IS RESERVED, so this always fits and the record always parses. A reason that ran out of room \
       says so IN the reason rather than ending mid-sentence as prose nobody can tell was cut. */ \
    if (apiclient_n_ == apiclient_cap_) \
      apiclient_n_ += (size_t) snprintf(apiclient_b_ + apiclient_n_, 64, \
                                        " [record truncated at %u bytes]\"}\n", (unsigned) apiclient_cap_); \
    else \
      apiclient_n_ += (size_t) snprintf(apiclient_b_ + apiclient_n_, 64, "\"}\n"); \
    apiclient_assert_write(apiclient_b_, apiclient_n_); \
  } while (0)

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
