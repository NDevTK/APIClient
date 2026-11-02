// lib/reply-schedule.js — the work point at which a send's answer reaches the engine instance that issued it.
//
// A reply lands at a function of the instance's own engine work, never of how fast the wire answered, so two
// runs of one document deliver every reply at the same point of every path. Each read of an instance states
// its work (solver/engine.h's engine_service_work): `poll` and `stalled` on the native ABI host, GetServiceWork
// on a renderer. A send first issued at a read is due at that read's clock plus REPLY_LATENCY_WORK; a read
// delivers exactly the answers due at or before its clock, by due point and then issue ordinal, waiting for any
// the wire has not produced. A stalled instance runs nothing while it waits, so its clock first moves to its
// earliest due point. bridge.js (the extension) and engine/trusted.mjs (the native host's zone) both keep their
// instances' clocks through this file, so the two hosts deliver by one rule. An instance's clock starts at its
// origin, the point on its cluster's timeline at which it was provisioned, so the clocks of one cluster's
// instances are comparable (engine/trusted.mjs delivers cross-instance records on them).

/* The modelled network latency in engine work: 50 ms at browser/core/timing/event_loop.h's EVENT_LOOP_WORK_PER_MS
   (100000 units a ms), about 93 slices of ENGINE_QUANTUM_WORK and about 1.1 s of native dev thread time, so a
   read rarely waits on the wire. A calibration, not a bound: no flow runs less far under any value. */
var REPLY_LATENCY_WORK = 5000000;

/* One instance's clock: its origin on the cluster's timeline, the work its last read stated (null before the
   first read), the work skipped at stalls, and the issue ordinal last handed out. */
function replyClock(origin) {
  DCHECK(Number.isSafeInteger(origin) && origin >= 0, "a reply clock was made with the origin " + origin);
  return { origin: origin, work: null, skip: 0, ordinal: 0 };
}

/* A read's stated work, taken onto the clock; the refusal is a sentence when the engine's statement is not a
   non-negative safe integer at or above the last one, since due points are on that clock and a decrease would
   deliver a reply before the work it waits for. The engine is untrusted, so its caller refuses the instance. */
function replyClockRead(clock, work) {
  if (!Number.isSafeInteger(work) || work < 0) return "stated the engine work " + String(work) + ", which is not a count";
  if (clock.work !== null && work < clock.work)
    return "stated the engine work " + work + " after " + clock.work + " — an instance's work never decreases";
  clock.work = work;
  return null;
}

/* The instance's network clock: its origin plus its last read's work plus the work skipped at stalls. */
function replyClockNow(clock) {
  DCHECK(clock.work !== null, "a reply clock was asked for its time before any read stated the instance's work");
  return clock.origin + clock.work + clock.skip;
}

/* The earliest time the instance's next read can state: its clock, or its origin before its first read. */
function replyClockFloor(clock) {
  return clock.work === null ? clock.origin : replyClockNow(clock);
}

/* The due point of a send first issued at the clock's current read. */
function replyClockDue(clock) {
  return replyClockNow(clock) + REPLY_LATENCY_WORK;
}

/* The next issue ordinal, the tie-break between equal due points. */
function replyClockOrdinal(clock) {
  return ++clock.ordinal;
}

/* The entries (each `{due, ordinal}`, due null while unstamped) due at the clock, in delivery order. */
function replyDueEntries(clock, entries) {
  var now = replyClockNow(clock);
  return entries.filter(function (x) { return x.due !== null && x.due <= now; })
                .sort(function (a, b) { return a.due - b.due || a.ordinal - b.ordinal; });
}

/* A stall: the clock moves to the earliest of `dues` (the stamped due points of everything owed) when that is
   later than now, and answers how much work it skipped. */
function replyClockStall(clock, dues) {
  var now = replyClockNow(clock), next = Infinity;
  for (var i = 0; i < dues.length; i++) {
    DCHECK(Number.isSafeInteger(dues[i]), "a stalled instance owes an answer with no stamped due point: " + dues[i]);
    if (dues[i] < next) next = dues[i];
  }
  if (next === Infinity || next <= now) return 0;
  clock.skip += next - now;
  return next - now;
}
