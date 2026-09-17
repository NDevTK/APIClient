/*
 * Forced-exec fork-local assert — MIRRORS engine/host/check.h (the quickjs submodule is a separate repo and
 * cannot include the host header, exactly as extension/check.js mirrors the same law on the JS side). Same
 * semantics: DFAIL = a DEV-ONLY should-never-happen (design invariant / not-yet-built capability) — emits
 * @WHY at the origin then aborts; CHECK/CHECK_FAIL = ALWAYS fatal (dev AND release) for a "must-not-proceed
 * even in production" invariant. DFAIL compiles out in release (APICLIENT_DEV=0); its condition must be
 * side-effect-free and never recoverable control flow.
 * Every name here is the host header's and the JS mirror's. Only the EMIT is this file's (a plain @WHY/@E line
 * rather than the host's JSON), because the submodule cannot include the host header.
 * It lives in its OWN header because every translation unit of the fork asserts — libregexp's parser as much as
 * the interpreter — and a per-file copy of the definition is a place for two of them to drift apart.
 *
 * AND A MIRROR THAT CARRIES FEWER NAMES THAN WHAT IT MIRRORS IS NOT A SMALLER MIRROR, IT IS A CAPABILITY THE
 * FORK DOES NOT HAVE. This header said "all four names" while the host header had grown formatted forms, and
 * the drift is invisible in exactly the way a mirror's is: nothing includes both, so no compiler can compare
 * them, and the absence shows up only as asserts inside the fork that cannot say WHICH value broke them. That
 * is not cosmetic here — the fork is where the interpreter, the trampoline and every step machine live, so a
 * should-never-happen with hundreds of call sites converging on it reports one line and no operand. Measured:
 * a step machine leaked a code into a stage that routed five others, and establishing WHICH code took a
 * reading of the machine rather than of the crash, because the crash had no way to hold an integer.
 * SO THE RULE IS: a name the host header has and this one lacks is a fork-side assert that cannot state its
 * evidence, and the fix is to add it HERE first. The always-fatal formatted pair (CHECKF / CHECK_FAILF) is
 * deliberately absent because nothing in the fork composes one yet; the day something does, it is added here
 * before it is used, and its absence would show as a CHECK in this submodule spelling out a runtime value it
 * cannot interpolate.
 */
#ifndef QUICKJS_CHECK_H
#define QUICKJS_CHECK_H

#include <stdio.h>
#include <stdlib.h>

/* WHAT A COMPOSED REASON GETS INTACT, and the TAIL reserved BEYOND it so a cut can say it was cut. Sized here
   rather than at the call sites for the reason the host header gives: a per-site `char msg[400]` is a cap
   nobody reviewed. THAT ARGUMENT WAS ONLY EVER HALF BUILT, and this comment is where it shows: it already said
   "a truncated reason is prose a reader cannot tell was cut" and then cut in exactly that way, because the
   composition below discarded `snprintf`'s return. A `@WHY` earns its place by naming WHAT TO BUILD, and this
   project's convention puts that at the END of the sentence — so the half a silent cut removes is precisely
   the half the crash exists for, and an abort whose remedy was eaten is indistinguishable from one whose
   author wrote none. Only the COMPOSED path ever had a buffer: a plain DFAIL/DCHECK message goes straight to
   `fprintf` and is not bounded here at all, which is why the fork carries plain reasons of 869 and 1480 bytes
   that have never been at risk.
   THE TAIL IS ADDED TO THE CAP RATHER THAN CARVED OUT OF IT, and that is a deliberate divergence from the host
   emitter's arithmetic rather than a mirror gone loose. Carving 64 out of 512 would start LABELLING a reason
   that fits today, so the diff written to stop losing tails would have begun reporting an intact message as
   truncated. The buffer is CAP + TAIL: a reason of up to CAP-1 bytes arrives byte-for-byte as it does now, and
   only one that genuinely outgrew the cap is touched.
   WHAT WOULD RETIRE THE NUMBER 512 is a measurement this cannot make: the composed buffer is a local in the
   macro's expansion, and one of the fork's three composed asserts expands inside JS_CallInternal, whose frame
   is the one thing the heap trampoline exists to keep flat. Raising the cap toward the host's is defensible on
   content alone — the plain path already emits three times this — and it is a stack question in the
   interpreter's hottest function, so it is priced by building rather than argued here. */
#define APICLIENT_QJS_REASON_CAP  512
#define APICLIENT_QJS_REASON_TAIL 64

#if defined(__GNUC__) || defined(__clang__)
#define APICLIENT_QJS_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define APICLIENT_QJS_PRINTF(f, a)
#endif

/* THE RELEASE FORM OF A COMPILED-OUT FORMATTED ASSERT, and it is not decoration. `sizeof` does not evaluate its
   operand, so no argument of a dropped message is computed — but the call is still PARSED, which is what keeps
   -Wformat checking the arguments in the build that never runs them, and keeps a variable used ONLY in an
   assert message from reading as unused. Dropping the arguments entirely instead would mean release is the
   build where a wrong format specifier is legal, which is the one place nobody would look for it. */
APICLIENT_QJS_PRINTF(1, 2)
static inline int apiclient_qjs_fmt_check(const char *fmt, ...) { (void) fmt; return 0; }
#define APICLIENT_QJS_FMT_UNUSED(...) ((void) sizeof apiclient_qjs_fmt_check(__VA_ARGS__))

/* MARK A COMPOSED REASON THAT DID NOT FIT, from `snprintf`'s own return — the length the message WOULD have
   been (C99 §7.19.6.5 "The snprintf function"). The record then states how much of itself is missing instead
   of ending mid-word, which is the ONE thing that tells a reader an author wrote no remedy apart from a
   remedy that was eaten.
   IT TAKES THE LENGTH AND NOT THE FORMAT, AND THAT IS THE WHOLE DESIGN. Composing inside a helper — the host
   emitter's shape — would have retired the only instrument that has ever caught this: `-Wformat-truncation`
   fires only where the destination size and the format literal are BOTH visible at the call being written, so
   a `vsnprintf` behind a function is invisible to it however the arguments are attributed. Measured on both
   compilers this tree builds with, at the build's own flag list: composition at the call site warns, the same
   message composed inside a helper does not. Leaving the `snprintf` in the macro keeps that check AND adds
   this one, and the two do not overlap — the compiler decides the case where the FORMAT ALONE overruns, before
   an argument is substituted, and this decides the case where a runtime `%s` does, which no compiler can see.
   THE MARKER'S WORDING IS THE HOST EMITTER'S, to the byte, so one grep over a log finds a cut from either
   producer; the two write different record SHAPES and this is the one sentence they should not diverge on. */
static inline void apiclient_qjs_reason_mark(char *r, int n)
{
  size_t k, j;

  if (n < 0) {
    /* The message's own formatting failed — an encoding error is the only way out of `snprintf` that is not a
       length. Report THAT rather than whatever half-written bytes are in the buffer. */
    snprintf(r, APICLIENT_QJS_REASON_CAP, "%s", "(this assert's message could not be formatted)");
    return;
  }
  if ((size_t) n < (size_t) APICLIENT_QJS_REASON_CAP) return;   /* arrived whole */

  /* Cut. Retreat to a UTF-8 character boundary first — Unicode 16.0 §3.9 "Unicode Encoding Forms", Table 3-6
     — so the marker is appended to a decodable string. This is not hypothetical for this fork: the longest
     composed reason it carries holds both `§` (two bytes) and `—` (three), and a cut one byte into either
     leaves a lead byte with no continuation for the reader to decode. */
  k = (size_t) APICLIENT_QJS_REASON_CAP - 1;
  j = k;
  while (j > 0 && ((unsigned char) r[j - 1] & 0xC0) == 0x80) j--;
  if (j > 0) {
    unsigned char lead = (unsigned char) r[j - 1];
    size_t want = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
    if (lead >= 0xC0 && k - (j - 1) < want) k = j - 1;
  }
  snprintf(r + k, (size_t) APICLIENT_QJS_REASON_TAIL,
           " [reason truncated: %u of %d bytes]", (unsigned) k, n);
}

#if defined(APICLIENT_DEV) && APICLIENT_DEV == 0
#define DFAIL(msg)         ((void)0)
#define DCHECK(cond, msg)  ((void)0)
#define DCHECKF(cond, ...) do { (void)sizeof(cond); APICLIENT_QJS_FMT_UNUSED(__VA_ARGS__); } while (0)
#else
#define DFAIL(msg)         do { fprintf(stderr, "@WHY %s (%s:%d)\n", (msg), __FILE__, __LINE__); abort(); } while (0)
#define DCHECK(cond, msg)  do { if (!(cond)) DFAIL(msg); } while (0)
/* Composed into ONE buffer and emitted as ONE line, never as three fprintf calls: the harness reads
   `@WHY <reason> (<file>:<line>)` as a record, and a reason split across writes is a record another thread's
   output can land inside of. The format arguments are as side-effect-free as a DCHECK condition must be —
   they vanish with the assert in release. */
#define DCHECKF(cond, ...) do { if (!(cond)) { \
        char apiclient_qjs_r_[APICLIENT_QJS_REASON_CAP + APICLIENT_QJS_REASON_TAIL]; \
        apiclient_qjs_reason_mark(apiclient_qjs_r_, \
            snprintf(apiclient_qjs_r_, APICLIENT_QJS_REASON_CAP, __VA_ARGS__)); \
        DFAIL(apiclient_qjs_r_); \
    } } while (0)
/* RESIDUAL — THE LABELLING ABOVE COVERS THE COMPOSED PATH THAT GOES THROUGH THIS MACRO, AND MOST COMPOSED
   ASSERTS IN THE FORK DO NOT. NOT COVERED: an assert that composes into a per-site `char why[N]` with its own
   bare `snprintf` and hands the result to plain DFAIL. Those never reach this macro, so they cut silently and
   split UTF-8 exactly as this one did, and their N is the "cap nobody reviewed" the comment above rejects —
   sized by eye, per site, differing by more than an order of magnitude between neighbours. Derive today's set
   rather than trusting a number here:
     grep -nE '(^|[^_A-Za-z])(DFAIL|CHECK_FAIL)[[:space:]]*\([[:space:]]*[A-Za-z_]' *.c | grep -vE '\([[:space:]]*"'
   WHAT THE NEXT DIFF BUILDS, in this order and not combined: (1) settle the cap, which is a STACK question and
   not a content one — the buffer is a local in this macro's expansion and one composed assert expands inside
   JS_CallInternal, so raising it toward the host's is priced by building rather than argued; (2) add DFAILF —
   the host header has it, this one does not, and that is this file's own rule about a missing mirror name
   arriving as ~28 sites hand-rolling it; (3) convert those sites onto it and DELETE their per-site buffers, so
   no call site owns a size at all, which is the whole argument for composing here.
   HOW ITS ABSENCE SHOWS: gcc's -Wformat-truncation reports `may be truncated` at per-site assert buffers in
   this file and clang does not (clang implements only the always-truncated case), so the shipped emcc build is
   blind to precisely the sites this residual is about; and a `@WHY` from one of them ends mid-sentence with no
   marker, which is indistinguishable from an author who wrote no remedy. */
#endif
#define CHECK_FAIL(msg)    do { fprintf(stderr, "@E %s (%s:%d)\n", (msg), __FILE__, __LINE__); abort(); } while (0)
#define CHECK(cond, msg)   do { if (!(cond)) CHECK_FAIL(msg); } while (0)

#endif /* QUICKJS_CHECK_H */
