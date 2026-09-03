// check.js — the JS-side mirror of engine/host/check.h, so the OFFSCREEN BRAIN (the trusted analysis zone)
// fails HARD on invariant violations exactly like the C engine does, and each shrinking JS component becomes
// isolation-testable (you can exercise it and TRUST that a violated assumption crashes at the origin instead
// of silently corrupting globalStore).
//
// LOADED FIRST IN EVERY REALM THAT ASSERTS, because a realm without this file does not assert LESS — it
// asserts NOTHING, and the seams it sits on are read with `||` instead. FIRST means FIRST OF THE SCRIPT TAGS,
// not merely present: this line said "popup.html loads it" while that document loaded it NINETEENTH of twenty,
// after eighteen libraries, under an argument about what those libraries happen to do at load — a claim about
// code nobody re-checks, standing in a header whose subject is that an assumption must crash at its origin. A
// realm named here is named with its POSITION so the next divergence is one grep rather than a reading of
// eighteen files. ast-worker.html loads it first of its script tags, popup.html loads it first of its script
// tags (ahead of lib/field-def.js), the renderer frame is handed its bytes in the boot record it
// checks itself against, and the CONTENT SCRIPT's isolated world gets it from manifest content_scripts, where
// it is listed AHEAD of content.js —
// content.js is a party to two seams (intercept.js's per-transport RESPONSE_BODY detail, and the trusted
// zone's PAGE_FETCH / *_SEND_MSG relays) and had no way to state either contract before it was injected
// there. A content script is UNTRUSTED (SECURITY.md) and that is not in tension with this: the assertions it
// makes are about what the TRUSTED zone promised to send it, and an abort there costs one page's relay.
//
// The CHECK vs DCHECK law is IDENTICAL to check.h — a real distinction, not synonyms:
//
//   DCHECK(cond, msg) / DFAIL(msg) — a DEV-ONLY should-never-happen: a design invariant, a not-yet-built
//     capability, or an engine↔JS contract the engine must guarantee. Throws LOUD (@WHY) in dev at the
//     origin so the bug surfaces where it is BORN; NO-OP in release (APICLIENT_DEV=false), so the condition
//     MUST be side-effect-free and NEVER recoverable control flow. Asserts "our OWN logic is correct".
//
//   CHECK(cond, msg) / CHECK_FAIL(msg) — ALWAYS fatal (dev AND release, @E) for a UNIVERSAL invariant that
//     must hold in production too: a security/authorization boundary (SECURITY.md), data integrity, an
//     IndexedDB quota/OOM floor. Asserts "we must not PROCEED even in production".
//
// Rule: when unsure it is a DCHECK. Offensive programming: too FEW asserts is the failure — never a
// `?? default` / `if(bad)return` / `?.`-past that continues over a broken invariant, never a try/catch that
// swallows one. The goal is ZERO @WHY, reached by fixing the ROOT, never by muting the check.
(function (g) {
  // DEV by default (mirrors the C build defaulting to -DAPICLIENT_DEV=1); a release packaging step sets
  // self.APICLIENT_DEV = false to strip DCHECKs, exactly like the engine's `release` build arg.
  if (typeof g.APICLIENT_DEV === "undefined") g.APICLIENT_DEV = true;

  // A DCHECK/CHECK failure is a HARD failure — throw so it aborts the current operation LOUD (never returns).
  function fail(tag, msg) {
    const e = new Error(tag + " " + (msg || "(no message)"));
    e.apiclientFatal = true;              // so a top-level handler can distinguish an invariant abort
    // Surface on the console too (the harness captures offscreen console), then throw.
    try { (g.console && g.console.error) && g.console.error(tag, msg); } catch (_) {}
    throw e;
  }

  g.DCHECK = function (cond, msg) { if (g.APICLIENT_DEV && !cond) fail("@WHY DCHECK failed:", msg); };
  g.DFAIL  = function (msg)       { if (g.APICLIENT_DEV) fail("@WHY DFAIL:", msg); };
  g.CHECK  = function (cond, msg) { if (!cond) fail("@E CHECK failed:", msg); };
  g.CHECK_FAIL = function (msg)   { fail("@E CHECK_FAIL:", msg); };

  // RETHROW_FATAL(e) — THE ONE PRIMITIVE C DOES NOT NEED, and the reason this mirror was only half a mirror.
  // check.h aborts the process, so a C `if (err) goto fail` CANNOT swallow a DCHECK; on this side an assertion
  // is a THROW, so every legitimate `catch` in the zone is also a place an invariant abort silently becomes a
  // plausible answer. That is not hypothetical: the DCHECK on safeFetch's URL list sat inside the reply
  // builder's `catch (_) { return null; }`, and null is the engine's NETWORK ERROR — a broken host contract
  // would have been delivered to the engine as a page whose request simply failed.
  //
  // A catch that HAS a real job (a network error IS a Fetch §5.6 outcome, not a bug) opens with this line: an
  // invariant failure travels ON through it, everything else is handled as the catch intends. It is not a
  // second assertion mechanism — it is what keeps the ONE mechanism from being locally disabled.
  g.RETHROW_FATAL = function (e) { if (e && e.apiclientFatal) throw e; };
})(self);
