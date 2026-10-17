// lib/body-store.js — a fetched body stored once, by content digest, and named by every frontier entry that
// holds it.
//
// Two documents of one application are very often one body: a single-page app serves the same shell at every
// route, so two tabs of it park two frontier entries over identical bytes. The frontier store (bridge.js's
// `frontier`) therefore names a document by its digest and this file keeps the bytes once in `bodies`, each
// record counting the entries that name it. An entry that gives its document up (a shed, a rewrite under other
// bytes, a delete) drops its reference, and the body goes when nothing names it, never earlier: a body is the
// only copy of an unrecomputable document exactly when some entry that cannot re-derive it still names it, and
// that entry's reference holds it. The functions take object stores of a transaction the caller owns, so an
// entry and the references it implies commit together or not at all.

/* SHA-256 of a document half as 64 lowercase hex digits. A stored document is bytes or characters (a cold entry
   an earlier build parked as a string); the type is folded first, so equal text in the two forms is two bodies,
   as it is two things to hand an engine. */
async function bodyDigest(value) {
  DCHECK(value instanceof Uint8Array || typeof value === "string",
         "a body was digested that is neither bytes nor characters — frontierDoc asserts one of the two");
  const tag = new Uint8Array([value instanceof Uint8Array ? 0x62 : 0x73]);
  const data = value instanceof Uint8Array ? value : new TextEncoder().encode(value);
  const all = new Uint8Array(1 + data.length);
  all.set(tag, 0);
  all.set(data, 1);
  const d = new Uint8Array(await crypto.subtle.digest("SHA-256", all));
  let hex = "";
  for (const b of d) hex += b.toString(16).padStart(2, "0");
  return hex;
}

/* Bytes a body occupies, characters counted at two (the store holds UTF-16). */
function bodyBytes(value) {
  return value instanceof Uint8Array ? value.length : value.length * 2;
}

/* One more entry names `digest`; `value` is its bytes, stored if this is the first name. Request callbacks
   only, so the caller's transaction stays open across the read and the write. */
function bodyRef(bodies, digest, value) {
  const g = bodies.get(digest);
  g.onsuccess = () => {
    const rec = g.result;
    if (rec === undefined) { bodies.put({ value, refs: 1, bytes: bodyBytes(value) }, digest); return; }
    DCHECK(Number.isInteger(rec.refs) && rec.refs >= 1, "a stored body carries no reference count");
    bodies.put({ value: rec.value, refs: rec.refs + 1, bytes: rec.bytes }, digest);
  };
}

/* One entry no longer names `digest`; the body goes with its last name. A body nothing names is this store's
   own accounting broken, since every reference was taken by an entry write in one transaction with it. */
function bodyUnref(bodies, digest) {
  const g = bodies.get(digest);
  g.onsuccess = () => {
    const rec = g.result;
    DCHECK(rec !== undefined && Number.isInteger(rec.refs) && rec.refs >= 1,
           "an entry dropped its reference to a body the store does not hold — every reference is taken in the " +
           "transaction that writes the entry naming it");
    if (rec === undefined) return;
    if (rec.refs === 1) bodies.delete(digest);
    else bodies.put({ value: rec.value, refs: rec.refs - 1, bytes: rec.bytes }, digest);
  };
}
