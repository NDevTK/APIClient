// check.js — the JS-side mirror of engine/host/check.h, so the OFFSCREEN BRAIN (the trusted analysis zone)
// fails HARD on invariant violations exactly like the C engine does, and each shrinking JS component becomes
// isolation-testable (you can exercise it and TRUST that a violated assumption crashes at the origin instead
// of silently corrupting globalStore). Loaded BEFORE offscreen-brain.js in ast-worker.html.
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
})(self);
