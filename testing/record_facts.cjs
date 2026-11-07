// testing/record_facts.cjs — lib/endpoint-record.js's record facts and the Endpoints panel's classes, in node.
//
// It loads extension/check.js and extension/lib/endpoint-record.js into one context, folds @H rows shaped as
// solver/endpoint.c emits them, and checks:
//   classes          security-relevant records and plain ones stand apart, each in its given order;
//   filter, sort     one fact's value keeps exactly the rows stating it, in their order; one fact lists each class
//                    by its vocabulary, ties in their order;
//   folds            a method's rows fold by the engine's rules (set unions, `common` where some row's holds,
//                    deliveries kept distinct, the minimal assumption sets);
//   invented negative  a row folded with itself states that row's facts.
// Exit status 0 is every expectation met.
//
//   node testing/record_facts.cjs
"use strict";
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const EXT = path.join(__dirname, "..", "extension");
const ctx = vm.createContext({ console });
vm.runInContext("var self = globalThis;", ctx);
for (const f of ["check.js", "lib/endpoint-record.js"])
  vm.runInContext(fs.readFileSync(path.join(EXT, f), "utf8"), ctx, { filename: f });
const R = (name) => vm.runInContext(name, ctx);

const via = (delivery, prefix, routes) => {
  const s = { source: "s", delivery, deliveryPrefix: prefix };
  if (routes !== undefined) s.messageRoutes = routes;
  return { victimAct: false, sources: [s] };
};
const row = (o) => Object.assign({ attackerRooted: false, credentials: [null], destinations: ["unknown"],
                                   attackerNeeds: false, assumes: [[]] }, o);
const marked = (common, deliveries, o) =>
  row(Object.assign({ attackerRooted: { address: "{location.search}[q]", victimAct: false },
                      attackerNeeds: { common, deliveries } }, o));
const ITEMS = [
  { id: "plain-a", rows: [row({ destinations: ["same-origin"] })] },
  { id: "third", rows: [marked(true, [via("address", "?")], { credentials: ["omit"], destinations: ["cross-origin"] })] },
  { id: "plain-b", rows: [row({ assumes: [[{ kind: "absent-arm", name: "window.__rrUser" }]] })] },
  { id: "name", rows: [marked(false, [via("navigable-name", null)], { credentials: ["same-origin"],
                                                                      destinations: ["same-origin"] })] },
  { id: "both", rows: [
      marked(false, [via("cross-document-message", null, ["frame", "opener"])], { credentials: ["include"] }),
      marked(true, [via("address", "?")], { credentials: ["omit"] })] },
];

let failed = 0;
const expect = (ok, what) => { if (!ok) failed++; console.log((ok ? "ok   " : "FAIL ") + what); };
const fold = (rows) => rows.reduce((f, r) => R("recordFactsJoin")(f, R("recordFacts")(r, "record_facts.cjs")),
                                   undefined);
for (const it of ITEMS) it.f = fold(it.rows);
const ids = (c) => JSON.stringify({ marked: c.marked.map((it) => it.id), plain: c.plain.map((it) => it.id) });
const classes = (field, value, sortBy) => ids(R("recordClasses")(ITEMS, (it) => it.f,
                                                                   field === null ? null : [field, value],
                                                                   sortBy === undefined ? null : sortBy));

expect(classes(null, null) === JSON.stringify({ marked: ["third", "name", "both"], plain: ["plain-a", "plain-b"] }),
       "classes: each in its given order; read " + classes(null, null));
expect(classes("delivery", "address") === JSON.stringify({ marked: ["third", "both"], plain: [] }),
       "filter: the address delivery keeps the two rows stating it; read " + classes("delivery", "address"));
expect(classes(null, null, "credentials") === JSON.stringify({ marked: ["third", "both", "name"],
                                                               plain: ["plain-a", "plain-b"] }),
       "sort: credentials lists each class by its vocabulary, ties in order; read " + classes(null, null, "credentials"));
expect(classes("assumes", "state") === JSON.stringify({ marked: [], plain: ["plain-b"] }),
       "filter: assumed state keeps the row behind an absent global; read " + classes("assumes", "state"));
const both = ITEMS.find((it) => it.id === "both").f;
expect(both.needs.common === true && both.needs.deliveries.length === 2,
       "a message and `?q=` fold to common with both deliveries stated");
expect(JSON.stringify(both.credentials) === JSON.stringify(["omit", "include"]),
       "the credentials modes fold to their union in vocabulary order; read " + JSON.stringify(both.credentials));
const asm = fold([row({ assumes: [[{ kind: "absent-arm", name: "a" }, { kind: "absent-read", name: "b" }]] }),
                  row({ assumes: [[{ kind: "absent-arm", name: "a" }]] })]).assumes;
expect(JSON.stringify(asm) === JSON.stringify([[{ kind: "absent-arm", name: "a" }]]),
       "the assumption sets keep the minimal one; read " + JSON.stringify(asm));
expect(R("recordAssumesText")(ITEMS.find((it) => it.id === "plain-b").f) ===
       "reached assuming window.__rrUser present", "an absent arm reads as the state taken as present");
const one = ITEMS.find((it) => it.id === "name");
expect(JSON.stringify(fold([one.rows[0], one.rows[0]])) === JSON.stringify(one.f),
       "invented negative: a row folded with itself states that row's facts");
process.exit(failed ? 1 : 0);
