/* lib/dev-host-order.js — the Level-1 order across documents on the diagnostics page: bridge.js's `_level1`,
   the reading its scheduler takes at the end of every round of which live instance, waiting tab, declared
   route and parked frontier it ranked ahead of which. No engine result can carry it, because it is composed
   in the trusted zone out of one weight per instance and per non-resident work item.
   Every key is rendered generically in the raw row, so a key bridge.js adds reaches the page with no edit
   here; the states the census marks by a key's presence (a round that did not ask the non-resident order,
   one that asked and ranked nothing, a real ordering) are said in words. Weights are over served members; a
   never-served one weighs +Infinity and is counted, never folded into a spread. Built only through
   lib/dom-builder.js. */

/* The section for `order`: undefined is "not asked yet", null is the host's statement that no round has
   completed, and a record is a reading whose every value is a finite number. */
function devHostOrderSection(order) {
  const summary = (text) => domElement("summary", {}, [text]);
  const row = (text) => domElement("div", { class: "deep-row" }, [domElement("span", { class: "deep-label" }, [text])]);
  if (order === undefined) return domElement("div", { class: "empty" }, ["The host order has not been asked yet."]);
  if (order === null) {
    return domElement("details", { class: "deep-cross-tab", open: "" }, [
      summary("Level-1 order — across documents"),
      row("the host has completed no scheduler round in this session, so there is no order to report — this " +
          "is the absence of a reading, not an order of zeroes")]);
  }
  DCHECK(Number.isInteger(order.round) && order.round > 0 && Number.isInteger(order.hot),
         "the host's Level-1 reading carries no round number or no hot count — bridge.js writes both on every " +
         "record it composes, in the `finally` of the round itself");
  for (const k of Object.keys(order)) {
    DCHECK(typeof order[k] === "number" && Number.isFinite(order[k]),
           "the host's Level-1 reading carries a non-finite `" + k + "` — every row is a count, a 0/1 state " +
           "or a weight over served members, and the two infinities travel as the `drained` and " +
           "`unserved` counts");
  }
  const n = (k) => String(order[k]);
  const rows = [devResidentOrder(order, n), devNonResidentOrder(order, n)];
  if ("cands" in order) {
    rows.push(`left the order: ${n("exclLive")} parked residue(s) whose address a live or waiting document ` +
      `holds (that weight is carried by the instance instead), ${n("exclHeld")} whose key an instance ` +
      `already holds, ${n("exclStranded")} whose re-derivation stopped answering, ${n("exclCrashed")} whose ` +
      `resume aborted this engine, ${n("exclResting")} resting on refusals under this engine and egress ` +
      `policy, ${n("exclSub")} waiting ` +
      `document(s) that are sub-frames their embedder names, and ${n("exclSeedLive")} + ` +
      `${n("exclSeedParked")} declared route(s) whose address a live document already holds or a parked ` +
      `frontier already carries`);
  }
  rows.push(Object.keys(order).map((k) => k + " " + n(k)).join(" · "));
  return domElement("details", { class: "deep-cross-tab", open: "" }, [
    summary("Level-1 order — across documents, as of round " + n("round") +
            (order.atFloor ? " (at the RAM floor)" : "")),
    ...rows.map(row)]);
}

function devResidentOrder(order, n) {
  if (order.hot === 0) return "resident order: no instance was rankable this round";
  const unserved = order.unserved ? `${n("unserved")} never served (ranked first); ` : "";
  const drained = order.drained ? `${n("drained")} drained and therefore ranked last` : "";
  if (!("wTop" in order)) {
    return `resident order over ${n("hot")} hot instance(s): no served, runnable one — ${unserved}${drained}`;
  }
  return `resident order over ${n("hot")} hot: served top ${n("wTop")}, bottom ${n("wMin")}` +
         (unserved || drained ? `; ${unserved}${drained}` : "");
}

/* The non-resident half is read at the top of the round and the pool counts at its end, so a waiting count
   here above the raw row's is the round having seated one. */
function devNonResidentOrder(order, n) {
  if (!("cands" in order)) {
    return "non-resident order: NOT ASKED this round — the round spent its one advance on a navigation, or a " +
           "reservation in flight held admission while the working set was under the floor. This is the " +
           "order not being taken, which is a different fact from its being taken and finding nothing";
  }
  if (order.cands === 0) {
    return "non-resident order: asked, and it ranked nothing — no document is waiting for an instance and no " +
           "parked residue was admissible";
  }
  let s = `non-resident order over ${n("cands")} work item(s) when it was asked — ${n("candDocs")} waiting ` +
          `document(s)` + ("candDocWMax" in order ? ` (best served ${n("candDocWMax")})` : "") +
          `, ${n("candSeeds")} declared route(s)` + ("candSeedWMax" in order ? ` (best served ${n("candSeedWMax")})` : "") +
          `, ${n("candCold")} parked frontier(s)` + ("candColdWMax" in order ? ` (best served ${n("candColdWMax")})` : "") +
          `; ${n("candUnserved")} of ${n("cands")} never served`;
  if ("candWMax" in order) {
    s += `. Over the served: index ${n("candWMin")}..${n("candWMax")}, novel value per pull ` +
         `${n("candRateMin")}..${n("candRateMax")}, most service ${n("candSvcMax")} pulls`;
  }
  return s;
}
