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
// An answer is records (replyStream*) of fixed byte ranges of its body, each due a latency after the one before,
// never the wire's reads: Fetch §4.7 "HTTP-network fetch" enqueues whatever bytes "have been transmitted" when
// the stream pulls, so reads are the host's network timing and would move a reply's delivery point with it.

/* The modelled network latency in engine work: 50 ms at browser/core/timing/event_loop.h's EVENT_LOOP_WORK_PER_MS
   (100000 units a ms), about 93 slices of ENGINE_QUANTUM_WORK and about 1.1 s of native dev thread time, so a
   read rarely waits on the wire. A calibration, not a bound: no flow runs less far under any value. */
var REPLY_LATENCY_WORK = 5000000;

/* The body bytes one record carries. A heuristic: Chrome 155's fetch body reader, reading ten large resources of
   excalidraw.com, tldraw.com and regex101.com through this project's container proxy, yielded 75 chunks from 10
   to 821890 bytes, of which 65536 was both the most frequent (18) and the median.
   A named residual: a streamed body whose server sends pieces smaller than a record with silence between (an
   event stream read through fetch()) reaches the page only once a record's worth or the end has arrived. The
   next diff cuts a `text/event-stream` body's records at the blank lines that dispatch its events (HTML §9.2.6
   "Interpreting an event stream"), which are the answer's own bytes. Its absence shows as a streamed fetch
   still owed at its due point while its wire keeps answering reads. */
var REPLY_RECORD_BYTES = 65536;

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

/* A send's answer as records of its decoded body: record k carries bytes [(k-1)·REPLY_RECORD_BYTES,
   k·REPLY_RECORD_BYTES), record 1 also the head, and the last record is the one the body ends in, so an answer of
   L bytes is max(1, ⌈L / REPLY_RECORD_BYTES⌉) records whatever reads the wire answered it in. Record k has landed
   once the body has run past it or the whole answer has arrived; it is the last when the answer arrived with no
   byte past it.
   A streamed entry (the caller's `stream` token, for a `fetch()` whose engine resolves at the head) writes every
   record at its due point (replyStreamPiece), the last saying how the body ended (solver/reply_body.h). Any
   other entry is handed its answer whole: a record due that has landed and is not the last moves the entry's
   `due` a latency on and writes nothing, and the last is the whole answer, written at its due point.
   The fields live on the caller's entry (`due`, `ordinal` and the caller's own beside them):
   `bytes` (the body bytes the wire has delivered), `record` (the 1-based record now owed), `settled` and `next`
   (the waiter of the next landing). */
function replyStreamOpen(entry) {
  DCHECK(entry.bytes === undefined, "a reply stream was opened twice on one entry");
  entry.bytes = 0;
  entry.record = 1;
  entry.settled = false;
  entry.next = null;
  return entry;
}

function _replyStreamWake(entry) {
  var w = entry.next;
  entry.next = null;
  if (w !== null) w.resolve();
}

/* The wire answered one more body read of `length` bytes for this send. */
function replyStreamRead(entry, length) {
  DCHECK(!entry.settled, "a body read was reported for a send whose answer was already produced: " + entry.bytes);
  DCHECK(Number.isSafeInteger(length) && length >= 0,
         "a body read was reported with the length " + length + " — safeFetch states each read's byte length");
  entry.bytes += length;
  _replyStreamWake(entry);
}

/* The send's whole answer is produced; its last record has landed. */
function replyStreamSettle(entry) {
  DCHECK(!entry.settled, "a send's answer was produced twice");
  entry.settled = true;
  _replyStreamWake(entry);
}

/* Whether record k of `entry` has landed: the body ran past its end, or the answer arrived. */
function _replyRecordLanded(entry, k) {
  return entry.settled || entry.bytes > k * REPLY_RECORD_BYTES;
}

/* Whether the record now owed has landed. */
function replyStreamLanded(entry) {
  return _replyRecordLanded(entry, entry.record);
}

/* Whether the record now owed is the answer's last; asked only of a landed record. */
function replyStreamLast(entry) {
  DCHECK(replyStreamLanded(entry), "a reply record was asked whether it is the last before it landed");
  return entry.settled && entry.bytes <= entry.record * REPLY_RECORD_BYTES;
}

/* Whether every record of `entry` due at `now` has landed: the one now owed and each later one whose due point
   (a latency apart) is at or before `now`, so a read that writes a streamed entry's records in turn never meets
   one the wire has not produced. */
function replyStreamDueLanded(entry, now) {
  DCHECK(Number.isSafeInteger(entry.due) && entry.due <= now,
         "a reply's due records were asked of an entry that is not due: " + entry.due + " at " + now);
  return _replyRecordLanded(entry, entry.record + Math.floor((now - entry.due) / REPLY_LATENCY_WORK));
}

/* The body bytes of the landed record now owed, copied out of `chunks` (the body's reads in order, byte views of
   any realm, as the caller kept them). */
function replyStreamPiece(entry, chunks) {
  DCHECK(replyStreamLanded(entry), "a reply record's bytes were asked for before it landed");
  var from = (entry.record - 1) * REPLY_RECORD_BYTES;
  var to = Math.min(entry.record * REPLY_RECORD_BYTES, entry.bytes);
  DCHECK(to >= from, "a reply's record " + entry.record + " ends at byte " + to + " before it starts at " + from +
         " — a body that ended before a record is landed through an earlier record, which was the last");
  var out = new Uint8Array(to - from), at = 0, i, c, lo, hi;
  for (i = 0; i < chunks.length && at < to; i++) {
    c = chunks[i];
    lo = Math.max(from, at);
    hi = Math.min(to, at + c.byteLength);
    if (lo < hi) out.set(new Uint8Array(c.buffer, c.byteOffset + (lo - at), hi - lo), lo - from);
    at += c.byteLength;
  }
  DCHECK(at >= to, "a landed reply record's bytes were asked for while its kept chunks hold " + at + " of " +
         entry.bytes + " bytes — every read the schedule counted is a chunk its caller kept");
  return out;
}

/* The landed record now owed is not the last: the next one is owed, due a latency later. */
function replyStreamAdvance(entry) {
  DCHECK(replyStreamLanded(entry) && !replyStreamLast(entry) && Number.isSafeInteger(entry.due),
         "a reply record was passed over that has not landed, is the answer's last, or has no due point");
  entry.record++;
  entry.due += REPLY_LATENCY_WORK;
}

/* A promise of the entry's next landing (a read or the answer). */
function replyStreamNext(entry) {
  if (entry.next === null) {
    var w = { promise: null, resolve: null };
    w.promise = new Promise(function (resolve) { w.resolve = resolve; });
    entry.next = w;
  }
  return entry.next.promise;
}
