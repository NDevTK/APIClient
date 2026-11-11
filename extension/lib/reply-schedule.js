// lib/reply-schedule.js — how a send's answer is handed to the engine instance that issued it, and the
// cluster timeline a record one instance causes in another is delivered on.
//
// A network answer lands at its due point on the path of the flow that asked for it (solver/engine.h's
// engine_reply_clock), which the engine computes, so the zone hands an answer over as soon as the wire has produced
// it and never schedules it: a non-streamed answer whole once it has settled, a streamed one record by record as
// each lands (replyStream*). bridge.js (the extension) and engine/trusted.mjs (the native host's zone) both hand
// answers over through this file, so the two hosts deliver by one rule.
// A record one instance's read causes in a peer (engine/trusted.mjs's `crossSend`) is due on the cluster's
// timeline: each instance's clock is its origin, the point on that timeline at which it was provisioned, plus the
// engine work its reads state (`poll` and `stalled`) plus what its stalls skipped, and the record is due
// PEER_RECORD_LATENCY_WORK after the causing read.
// A streamed answer's records are fixed byte ranges of its body, never the wire's reads: Fetch §4.7 "HTTP-network
// fetch" enqueues whatever bytes "have been transmitted" when the stream pulls, so reads are the host's network
// timing and would move a reply's content per record with it.

/* The latency of a record one instance causes in a peer, on the cluster's timeline. A placeholder heuristic with no
   evidence, set to the engine's ENGINE_REPLY_LATENCY_WORK (50 ms at core/timing/event_loop.h's
   EVENT_LOOP_WORK_PER_MS); the evidence that would set it is the delivery time of cross-origin messages measured in
   Chrome. */
var PEER_RECORD_LATENCY_WORK = 5000000;

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

/* The due point of a record caused at the clock's current read. */
function replyClockDue(clock) {
  return replyClockNow(clock) + PEER_RECORD_LATENCY_WORK;
}

/* The next issue ordinal, the tie-break between equal due points. */
function replyClockOrdinal(clock) {
  return ++clock.ordinal;
}

/* A send's answer. A streamed entry (the caller's `stream` token, for a `fetch()` whose engine resolves at the
   head) is records of its decoded body: record k carries bytes [(k-1)·REPLY_RECORD_BYTES, k·REPLY_RECORD_BYTES),
   record 1 also the head, and the last record is the one the body ends in. Record k has landed once the body has
   run past it or the whole answer has arrived; it is the last when the answer arrived with no byte past it. A
   `text/event-stream` body is cut at its events instead (replyStreamHead). Each landed record is written in turn
   (replyStreamPiece), the last saying how the body ended (solver/reply_body.h). Any other entry is one record, its
   whole answer, landed once it has settled.
   The fields live on the caller's entry (`ordinal`, a streamed entry's `chunks` array and the caller's own beside
   them): `bytes` (the body bytes a streamed entry has kept), `record` (the 1-based record now owed), `settled`,
   `next` (the waiter of the next landing) and, once a streamed entry's head is stated, `events`. */
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

/* A streamed entry's landed record now owed is written and is not the last: the next one is owed. */
function replyStreamAdvance(entry) {
  DCHECK(Array.isArray(entry.chunks) && replyStreamLanded(entry) && !replyStreamLast(entry),
         "a reply record was advanced past that is not a streamed entry's, has not landed, or is the answer's last");
  entry.record++;
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
