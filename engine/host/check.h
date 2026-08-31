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
#include <stdarg.h>   /* va_list — the emitter composes the reason; see apiclient_assert_compose */
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
 * — 4096 on Linux, which is the size of the buffer — is not interleaved) and a regular file opened
 * O_APPEND. It also removes stdio from the abort path entirely, which is worth having on its own: this runs
 * where the heap may already be the reason we are aborting, and `write` allocates nothing.
 * TRUNCATION IS VISIBLE AND THE RECORD STILL PARSES. A reason longer than the buffer is a real possibility —
 * several in this tree cite a whole algorithm — so the tail is reserved, the record always closes, and an
 * overlong one says so in its own `reason` rather than arriving as silently shortened prose.
 *
 * AND "STILL PARSES" IS A PROPERTY OF THE APPENDER, NOT OF THE RESERVED TAIL — the tail guarantees the record
 * CLOSES and says nothing about where the cut LANDED. A byte-clamping appender splits whatever it is in the
 * middle of, and two of the things it is in the middle of are indivisible: a JSON escape (`\"` is two bytes
 * and a control-character escape is six) and a UTF-8 sequence (the `§` of every citation is two). A cut one
 * byte into `\"` leaves a lone `\` with the marker's space after it, and `\ ` is not a legal escape (RFC 8259 §7
 * "Strings" lists the nine), so `JSON.parse` throws at that byte — and bridge.js's @WHY reader then takes its
 * NON-JSON arm, which is the arm for the submodule's plain-line producer. The record announces itself as
 * machine-readable, is dropped into the other producer's shape, and `phase`/`reason` are lost: the SAME
 * failure the unescaped-interpolation bug had, arriving through the length instead of through the content,
 * and only in the longest reasons — which are the ones that name the most.
 * So AN APPEND IS ALL-OR-NOTHING. A unit that does not fit is not written at all, `*used` is driven to `cap`
 * so the reserved tail's marker fires, and the record is valid JSON and valid UTF-8 at every cut. */
/* Append to a fixed buffer, never past it and NEVER PARTIALLY. `*used` is the running length; a unit that
   does not fit whole is dropped whole and the buffer is marked full, because the callers below hand this
   indivisible units (an escape sequence, a UTF-8 character) and half of one is a corrupt record rather than
   a short one. Ordinary text arrives one character at a time, for which all-or-nothing is byte-clamping. */
static inline void apiclient_assert_put(char *b, size_t cap, size_t *used, const char *s, size_t n)
{
  size_t room = cap - *used;
  if (n > room) {
    /* THE DECLINED ROOM IS FILLED, NOT SKIPPED. Driving `*used` to `cap` is what makes the reserved tail's
       marker fire, and on its own it would leave the bytes this unit refused as UNINITIALISED STACK inside a
       record that is about to be written — which is how a run put a raw NUL in the middle of a JSON string
       and lost the whole record to `JSON.parse` again, one layer along from the split escape. A space is
       legal everywhere in a JSON string, is valid UTF-8, and reads as the sentence stopping. */
    memset(b + *used, ' ', room);
    *used = cap;
    return;
  }
  memcpy(b + *used, s, n);
  *used += n;
}

/* JSON (RFC 8259 §7 "Strings") requires escapes for `"`, `\` and every code point below U+0020, and permits
   any other Unicode character unescaped — so UTF-8 bytes (the `§` in every citation) pass straight through,
   but they pass through AS A CHARACTER: the lead byte and its continuations are handed to the appender in one
   call, so a cut lands between characters and the truncated reason is still decodable. A malformed sequence
   in the input (a lone continuation byte, a lead whose continuations are missing) is emitted as the single
   byte it is — this escaper reports what it was given and never repairs it. */
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
      else if (c < 0xC0) apiclient_assert_put(b, cap, used, (const char *) &c, 1);
      else {
        /* Unicode 16.0 §3.9 "Unicode Encoding Forms", Table 3-6: the lead byte states the length. The
           continuations are counted FORWARD so the scan stops at the terminator and never reads past it. */
        size_t want = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2, len = 1;
        while (len < want && ((unsigned char) s[len] & 0xC0) == 0x80) len++;
        if (len != want) len = 1;
        apiclient_assert_put(b, cap, used, s, len);
        s += len - 1;
      }
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

/* THE RECORD'S SIZE IS ONE NUMBER, SET BY POSIX AND NOT BY AN AUTHOR, and every buffer on the assert path is
   derived from it. PIPE_BUF is 4096 on Linux and a write at or below it is un-interleaved (POSIX.1-2001), so
   the record sits under that and reserves a tail the closing marker is known to fit in. Nothing else on this
   path gets to pick a size. */
enum {
  /* PIPE_BUF ITSELF, because that is the constraint and anything below it is an author's margin. POSIX.1-2001
     write(2) makes a request of PIPE_BUF BYTES OR LESS un-interleaved — 4096 on Linux — so the record is
     exactly that and the whole of it is available to the reason. The old 3072 spent a quarter of the budget
     on nothing and cut the tree's longest assert (core/dom/node_heap.c's, whose own comment sized a 4096-byte
     site buffer to hold a message this emitter was already truncating 500 bytes earlier). */
  APICLIENT_RECORD_CAP = 4096,
  APICLIENT_RECORD_TAIL = 64,
  /* What the record spends BEFORE the reason: the tag, the four JSON keys, the stringized condition and
     `file:line`. The keys are 51 bytes and the other two are bounded by the SOURCE rather than by a page's
     data — `#cond` is a C expression an author typed — so this is an ALLOWANCE, not a limit, and a condition
     that outruns it is caught by the record's own marker below rather than by a rule anybody has to keep. */
  APICLIENT_RECORD_HEAD = 256,
  /* AND THE COMPOSITION BUFFER IS WHAT IS LEFT, so a composed reason that was cut still ARRIVES cut-and-
     labelled instead of having its own label chopped off by the record. The two stages both report, and this
     subtraction is why each report can be read: escaping only expands (a `"` becomes two bytes), so the
     record's marker stays reachable as the backstop for a reason full of quotes. */
  APICLIENT_REASON_CAP = APICLIENT_RECORD_CAP - APICLIENT_RECORD_TAIL - APICLIENT_RECORD_HEAD
};

#if defined(__GNUC__) || defined(__clang__)
#define APICLIENT_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define APICLIENT_PRINTF(f, a)
#endif

/* THE MESSAGE IS COMPOSED HERE OR IT IS COMPOSED INTO A BUFFER NOBODY CHECKED. An assert whose reason carries
 * runtime data — which component, which source, how many flows — used to `snprintf` it into a `char msg[400]`
 * the author sized by eye, then hand the result to the emitter. That is a SECOND truncation stage in front of
 * the loud one, and it is silent in both directions: `snprintf` reports the length it would have written and
 * no site read it, so the tail simply stopped existing. `-Wformat-truncation` proved it at four sites where
 * the format's own LITERAL minimum already exceeded the buffer — the message was cut in every run that has
 * ever fired one, before a single argument was substituted — and every remaining site is the same defect
 * waiting for a longer `%s`. The part that dies is the END of the sentence, which is where a `@WHY` says WHAT
 * TO BUILD; CLAUDE.md's forcing function is exactly the half that was being dropped.
 * A LONGER PER-SITE BUFFER IS NOT THE FIX, because the next author picks the next number. The emitter owns
 * the sizing, so no call site has a buffer at all, and the ONE stage that can still run out of room ANSWERS
 * WITH ITS OWN LENGTH: `vsnprintf` returns what the message WOULD have been (C99 §7.19.6.12 "The vsnprintf
 * function"), so a cut reason says how much of itself is missing instead of ending mid-word.
 * The `format(printf)` attribute keeps every call site's arguments type-checked, which is the one thing the
 * per-site `snprintf` did give and must not be lost. */
APICLIENT_PRINTF(2, 3)
static inline void apiclient_assert_compose(char *r, const char *fmt, ...)
{
  const size_t body = (size_t) APICLIENT_REASON_CAP - (size_t) APICLIENT_RECORD_TAIL;
  size_t k;
  va_list ap;
  int n;

  va_start(ap, fmt);
  n = vsnprintf(r, body, fmt, ap);
  va_end(ap);

  if (n < 0) {
    /* The message's own formatting failed — an encoding error is the only way out of `vsnprintf` that is not
       a length. Report THAT rather than whatever half-written bytes are in the buffer. */
    static const char oops[] = "(this assert's message could not be formatted)";
    memcpy(r, oops, sizeof oops);
    return;
  }
  if ((size_t) n < body) return;

  /* Cut. Retreat to a UTF-8 character boundary first — Unicode 16.0 §3.9 "Unicode Encoding Forms" Table 3-6 —
     so the marker is appended to a decodable string; the citations this tree writes are full of `§`. */
  k = body - 1;
  {
    size_t j = k;
    while (j > 0 && ((unsigned char) r[j - 1] & 0xC0) == 0x80) j--;
    if (j > 0) {
      unsigned char lead = (unsigned char) r[j - 1];
      size_t want = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
      if (lead >= 0xC0 && k - (j - 1) < want) k = j - 1;
    }
  }
  snprintf(r + k, (size_t) APICLIENT_REASON_CAP - k,
           " [reason truncated: %u of %d bytes]", (unsigned) k, n);
}

/* Emit the machine-readable assertion line the JS bridge (linesToAnalysis) surfaces (@E / @WHY), then abort. */
#define APICLIENT_ASSERT_EMIT(tag, condstr, msg) do { \
    char apiclient_b_[APICLIENT_RECORD_CAP];   /* exactly PIPE_BUF; see the enum */ \
    size_t apiclient_n_ = 0; \
    const size_t apiclient_cap_ = sizeof apiclient_b_ - APICLIENT_RECORD_TAIL;   /* the reserved tail */ \
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
      apiclient_n_ += (size_t) snprintf(apiclient_b_ + apiclient_n_, APICLIENT_RECORD_TAIL, \
                                        " [record truncated at %u bytes]\"}\n", (unsigned) apiclient_cap_); \
    else \
      apiclient_n_ += (size_t) snprintf(apiclient_b_ + apiclient_n_, APICLIENT_RECORD_TAIL, "\"}\n"); \
    apiclient_assert_write(apiclient_b_, apiclient_n_); \
  } while (0)

/* The same record, with the reason COMPOSED rather than handed over ready-made. The composition happens
   inside the assert so the buffer is the emitter's, not the author's — see apiclient_assert_compose. */
#define APICLIENT_ASSERT_EMITF(tag, condstr, ...) do { \
    char apiclient_r_[APICLIENT_REASON_CAP]; \
    apiclient_assert_compose(apiclient_r_, __VA_ARGS__); \
    APICLIENT_ASSERT_EMIT(tag, condstr, apiclient_r_); \
  } while (0)

/* The release form of a compiled-out formatted assert. `sizeof` does not evaluate its operand, so no argument
   of a dropped message is computed, and the call inside it is still parsed — which is what keeps `-Wformat`
   checking the arguments and keeps a variable used ONLY in an assert message from reading as unused. */
APICLIENT_PRINTF(1, 2)
static inline int apiclient_assert_fmt_check(const char *fmt, ...) { (void) fmt; return 0; }
#define APICLIENT_FMT_UNUSED(...) ((void) sizeof apiclient_assert_fmt_check(__VA_ARGS__))

#define CHECK(cond, msg)  do { if (!(cond)) { APICLIENT_ASSERT_EMIT("@E", #cond, (msg)); abort(); } } while (0)
#define CHECK_FAIL(msg)   do { APICLIENT_ASSERT_EMIT("@E", "unreachable", (msg)); abort(); } while (0)

/* CHECKF / CHECK_FAILF — a CHECK whose reason carries runtime data. Always active, exactly like CHECK. */
#define CHECKF(cond, ...) do { if (!(cond)) { APICLIENT_ASSERT_EMITF("@E", #cond, __VA_ARGS__); abort(); } } while (0)
#define CHECK_FAILF(...)  do { APICLIENT_ASSERT_EMITF("@E", "unreachable", __VA_ARGS__); abort(); } while (0)

/* DCHECK — dev-only fatal; compiled out in release (side-effect-free condition required). */
#if APICLIENT_DEV
#define DCHECK(cond, msg) do { if (!(cond)) { APICLIENT_ASSERT_EMIT("@WHY", #cond, (msg)); abort(); } } while (0)
#define DFAIL(msg)        do { APICLIENT_ASSERT_EMIT("@WHY", "unreachable", (msg)); abort(); } while (0)
/* DCHECKF / DFAILF — the same, with the reason composed from runtime data. The format arguments are as
   side-effect-free as a DCHECK condition must be: they vanish with the assert in release. */
#define DCHECKF(cond, ...) do { if (!(cond)) { APICLIENT_ASSERT_EMITF("@WHY", #cond, __VA_ARGS__); abort(); } } while (0)
#define DFAILF(...)        do { APICLIENT_ASSERT_EMITF("@WHY", "unreachable", __VA_ARGS__); abort(); } while (0)
#else
#define DCHECK(cond, msg) ((void)sizeof(cond))   /* type-checked, never evaluated */
#define DFAIL(msg)        ((void)0)
#define DCHECKF(cond, ...) do { (void)sizeof(cond); APICLIENT_FMT_UNUSED(__VA_ARGS__); } while (0)
#define DFAILF(...)        APICLIENT_FMT_UNUSED(__VA_ARGS__)
#endif

/* COUNTOF(a) — THE EXTENT OF AN ARRAY, AND A COMPILE ERROR ON A POINTER.
 *
 * `sizeof(a) / sizeof((a)[0])` is the remedy this project prescribes for an unbounded scan, and written bare it
 * is a TRAP RATHER THAN A BOUND: handed a POINTER it does not fail, it ANSWERS — 1, on any target whose element
 * is as wide as an address, which is every table of `const char *`. So the bound added to stop a scan running
 * off the end silently becomes a bound of one element, and a loop that was too long becomes a loop that is too
 * short. Both are wrong and only one of them looks wrong, which is why the naked idiom must not be the thing a
 * reader reaches for. The guard turns the pointer case into a DIAGNOSTIC: an array decays to a pointer only in
 * a VALUE context and `__typeof__` is not one, so `__builtin_types_compatible_p` still tells `T[N]` from `T *`,
 * and the pointer arm asks for `char[-1]` — the translation unit does not compile.
 *
 * IT IS A COMPILE-TIME CHECK AND THEREFORE NOT A DCHECK: it costs nothing at runtime, it reads identically in
 * dev and release, and there is no build in which it is compiled out. That distinction matters more here than
 * it usually would, for the reason the pointer-invariant note at the head of this file gives — the state it
 * refuses is one this engine's shipping target does not trap on. A read past the end of a static table in
 * WebAssembly linear memory returns whatever the link left after it, so a scan that overruns does not fault; it
 * keeps reading. And an overrun the compiler is entitled to assume cannot happen licenses the compiler to
 * conclude the loop containing it cannot exit, which is a HANG rather than a crash and presents as a
 * performance problem rather than as a defect. */
#if defined(__GNUC__) || defined(__clang__)
#define APICLIENT_MUST_BE_ARRAY(a) \
  (0 * (int) sizeof(char[1 - 2 * __builtin_types_compatible_p(__typeof__(a), __typeof__(&(a)[0]))]))
#else
#define APICLIENT_MUST_BE_ARRAY(a) 0
#endif
#define COUNTOF(a) (sizeof(a) / sizeof((a)[0]) + (size_t) APICLIENT_MUST_BE_ARRAY(a))

/* DCHECK_SENTINEL(a) — A SENTINEL-TERMINATED TABLE ENDS IN THE TERMINATOR ITS READERS SCAN FOR.
 *
 * A table read by `for (i = 0; a[i]; i++)` states its own length in exactly ONE place — its last element — and
 * nothing about writing the declaration makes that element mandatory. What makes the omission expensive is not
 * that the entry is missing but WHERE it is found: the scan discovers it by reading past the end, which is
 * undefined, so the diagnosis surfaces as a hang or as garbage somewhere downstream instead of as a fault at
 * the declaration that is actually wrong.
 * ASSERTED HERE the table is checked where its EXTENT IS STILL VISIBLE, which is why this is a macro and not a
 * function: a function receives the pointer, and the extent is precisely what a pointer has already lost. It is
 * also why the assert must expand AT the declaring site — a shared checker would stamp its own file and line
 * for every table in the tree, naming a remedy with no object. Expanded here it names the table to fix.
 * IT DOES NOT REPLACE SUPPLYING THE TERMINATOR FROM A DEFINITION MACRO, which removes the possibility instead
 * of reporting it; this is for the tables a macro does not declare. */
#define DCHECK_SENTINEL(a) \
  DCHECK((a)[COUNTOF(a) - 1] == NULL, \
         "a sentinel-terminated table does not end in its terminator — every reader scans for one, so the scan " \
         "runs past the end of the array and reads whatever the link happened to place after it")

#endif
