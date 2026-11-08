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
   to 821890 bytes, of which 65536 was both the most frequent (18) and the median. */
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
   byte past it. A streamed `text/event-stream` body is cut at its events instead (replyStreamHead).
   A streamed entry (the caller's `stream` token, for a `fetch()` whose engine resolves at the head) writes every
   record at its due point (replyStreamPiece), the last saying how the body ended (solver/reply_body.h). Any
   other entry is handed its answer whole: a record due that has landed and is not the last moves the entry's
   `due` a latency on and writes nothing, and the last is the whole answer, written at its due point.
   The fields live on the caller's entry (`due`, `ordinal`, a streamed entry's `chunks` array and the caller's
   own beside them): `bytes` (the body bytes the wire has delivered), `record` (the 1-based record now owed),
   `settled`, `next` (the waiter of the next landing) and, once a streamed entry's head is stated, `events`. */
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

/* A streamed entry's head (safeFetch's `onHead`), stated before its body. A `text/event-stream` body (HTML
   §9.2.5 "Parsing an event stream" names the MIME type) is cut where a blank line dispatches an event (§9.2.6
   "Interpreting an event stream": lines end in CRLF, LF or CR, and an empty line dispatches), or
   REPLY_RECORD_BYTES after its record starts, whichever comes first. A record ending at such a point lands as
   soon as its bytes have, so an event reaches the page without waiting for the next; the body's end is then a
   record of its own, the bytes after the last cut, which may be none. */
function replyStreamHead(entry, headers) {
  DCHECK(entry.bytes === 0 && entry.events === undefined && Array.isArray(entry.chunks),
         "a streamed reply's head was stated after its body began, twice, or for an entry that keeps no chunks");
  var ct = headers["content-type"];
  entry.events = typeof ct === "string" && _mimeEssence(ct) === "text/event-stream"
    ? { cuts: [], ci: 0, ends: [], lineEmpty: true, prevCR: false, crEmpty: false, bom: 0 } : null;
}

/* The wire answered one more body read of `length` bytes for a send whose answer is handed over whole. */
function replyStreamRead(entry, length) {
  DCHECK(!entry.settled, "a body read was reported for a send whose answer was already produced: " + entry.bytes);
  DCHECK(entry.events === undefined, "a streamed send's body read was counted without its bytes");
  DCHECK(Number.isSafeInteger(length) && length >= 0,
         "a body read was reported with the length " + length + " — safeFetch states each read's byte length");
  entry.bytes += length;
  _replyStreamWake(entry);
}

/* One read of a streamed send's body (safeFetch's `onChunk`), a byte view the caller owns, kept on the entry. */
function replyStreamChunk(entry, chunk) {
  DCHECK(!entry.settled && entry.events !== undefined && ArrayBuffer.isView(chunk),
         "a streamed send's body read arrived after its answer, before its head, or not as bytes");
  entry.chunks.push(chunk);
  if (entry.events !== null) _replyEventScan(entry.events, chunk, entry.bytes);
  entry.bytes += chunk.byteLength;
  if (entry.events !== null) _replyEventEnds(entry);
  _replyStreamWake(entry);
}

/* The send's whole answer is produced; its last record has landed. A CR that ended an empty line as the body's
   last byte is a cut there. */
function replyStreamSettle(entry) {
  DCHECK(!entry.settled, "a send's answer was produced twice");
  entry.settled = true;
  var ev = entry.events;
  if (ev !== undefined && ev !== null) {
    if (ev.prevCR && ev.crEmpty) ev.cuts.push(entry.bytes);
    ev.prevCR = false;
    _replyEventEnds(entry);
  }
  _replyStreamWake(entry);
}

/* §9.2.6's line ends over the bytes of `chunk`, which starts at body offset `base`: each empty line's end is a
   cut. A CR's pair is decided by the byte after it, and a leading UTF-8 BOM is no content (the stream is UTF-8
   decoded first, which strips it). */
var _REPLY_BOM = [0xEF, 0xBB, 0xBF];
function _replyEventScan(ev, chunk, base) {
  var bytes = new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength);
  for (var i = 0; i < bytes.length; i++) {
    var o = base + i, b = bytes[i];
    if (ev.prevCR) {
      ev.prevCR = false;
      if (ev.crEmpty) ev.cuts.push(b === 0x0A ? o + 1 : o);
      if (b === 0x0A) continue;
    }
    if (ev.bom === o && o < 3) {
      if (b === _REPLY_BOM[o]) { ev.bom++; continue; }
      if (o > 0) ev.lineEmpty = false;
      ev.bom = -1;
    }
    if (b === 0x0D) { ev.prevCR = true; ev.crEmpty = ev.lineEmpty; ev.lineEmpty = true; }
    else if (b === 0x0A) { if (ev.lineEmpty) ev.cuts.push(o + 1); ev.lineEmpty = true; }
    else ev.lineEmpty = false;
  }
}

/* Extends an event stream's record ends as far as the bytes decide them: the first cut within
   REPLY_RECORD_BYTES of the record's start, else that cap once the body has reached it (no later byte can cut
   before it then). */
function _replyEventEnds(entry) {
  var ev = entry.events;
  for (;;) {
    var s = ev.ends.length ? ev.ends[ev.ends.length - 1] : 0;
    while (ev.ci < ev.cuts.length && ev.cuts[ev.ci] <= s) ev.ci++;
    if (ev.ci < ev.cuts.length && ev.cuts[ev.ci] - s <= REPLY_RECORD_BYTES) { ev.ends.push(ev.cuts[ev.ci]); continue; }
    if (entry.bytes >= s + REPLY_RECORD_BYTES) { ev.ends.push(s + REPLY_RECORD_BYTES); continue; }
    return;
  }
}

function _replyEvents(entry) {
  return entry.events !== undefined && entry.events !== null;
}

/* Whether record k of `entry` has landed. */
function _replyRecordLanded(entry, k) {
  if (_replyEvents(entry)) return entry.settled || k <= entry.events.ends.length;
  return entry.settled || entry.bytes > k * REPLY_RECORD_BYTES;
}

/* Whether the record now owed has landed. */
function replyStreamLanded(entry) {
  return _replyRecordLanded(entry, entry.record);
}

/* Whether the record now owed is the answer's last; asked only of a landed record. */
function replyStreamLast(entry) {
  DCHECK(replyStreamLanded(entry), "a reply record was asked whether it is the last before it landed");
  if (_replyEvents(entry)) return entry.settled && entry.record > entry.events.ends.length;
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

/* The body bytes of a streamed entry's landed record now owed, copied out of its kept chunks (byte views of any
   realm). */
function replyStreamPiece(entry) {
  DCHECK(replyStreamLanded(entry) && Array.isArray(entry.chunks),
         "a reply record's bytes were asked for before it landed, or of an entry that keeps no chunks");
  var k = entry.record, from, to;
  if (_replyEvents(entry)) {
    var ends = entry.events.ends;
    from = k > 1 ? ends[k - 2] : 0;
    to = k <= ends.length ? ends[k - 1] : entry.bytes;
  } else {
    from = (k - 1) * REPLY_RECORD_BYTES;
    to = Math.min(k * REPLY_RECORD_BYTES, entry.bytes);
  }
  DCHECK(to >= from, "a reply's record " + k + " ends at byte " + to + " before it starts at " + from +
         " — a body that ended before a record is landed through an earlier record, which was the last");
  var out = new Uint8Array(to - from), at = 0, i, c, lo, hi;
  for (i = 0; i < entry.chunks.length && at < to; i++) {
    c = entry.chunks[i];
    lo = Math.max(from, at);
    hi = Math.min(to, at + c.byteLength);
    if (lo < hi) out.set(new Uint8Array(c.buffer, c.byteOffset + (lo - at), hi - lo), lo - from);
    at += c.byteLength;
  }
  DCHECK(at >= to, "a landed reply record's bytes were asked for while its kept chunks hold " + at + " of " +
         entry.bytes + " bytes — every read the schedule counted is a chunk it kept");
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
