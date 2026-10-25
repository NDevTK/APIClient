/* lib/popup-inputs.js — the Inputs panel: the learned API surface regrouped by the input each request rests
   on. Under every input it lists the endpoints a sighting reached under a condition over that input (a pin's
   subject, a branch's predicate), and the request parts whose value is spelled from it. An input is named as
   the engine spells a source read, the text between a shape's braces (lib/callsite-url.js's astHoleNames):
   `location.search`, `window.name`, `document.referrer`, `message.data:<id>`. It reads the producer fields
   the Endpoints panel reads and says "unknown" where none is carried. Built only through lib/dom-builder.js.
   A sighting's acts (the act list its flow performed) are steps too: a message act is one unit under
   `postMessage`, with its route, the origin and the data their domains compose, and every other act sits under
   `user act`.
   Named residual. Not covered: an input's address delivery and the value a verified recipe composes for it;
   the witness still spells its pins and branches. Next diff builds: inputRecipeOf over the composed, verified
   recipes that replace the spelled entries. How its absence shows: a source's conditions listed as the page
   spelled them, with no composed value and no verification beside a message act's data. */

// The panel. Reads `tabData` (GET_STATE, lib/serialize.js's serializeTabData) and nothing else.
function renderInputsPanel() {
  const body = document.getElementById("inputs-body");
  if (tabData === null) {
    domReplaceChildren(body, [domElement("div", { class: "empty" }, ["No state loaded."])]);
    return;
  }
  const view = inputView(tabData);
  const out = [];
  for (const name of [...view.inputs.keys()].sort()) out.push(inputSection(name, view.inputs.get(name)));
  if (view.unknown.length) out.push(inputUnknownSection(view.unknown));
  if (out.length === 0) {
    domReplaceChildren(body, [domElement("div", { class: "empty" }, [
      "No learned request in this view rests on an input — not a verdict that none does."])]);
    return;
  }
  domReplaceChildren(body, out);
}

/* THE ONE READER OF A SIGHTING'S CONDITIONS: a witness pool member (lib/endpoint-record.js's methodWitnesses)
   as {provenance, asked, verification, steps}, each step {text, inputs}. `asked` false is a sighting no flow
   stood at. A pin's `source` is its subject's hole key, the shape with its braces removed, so a pin on a source
   itself spells the source bare (`location.hash`) and a pin on a derivation keeps its operands' braces. The
   record carries spelled pins and branches today; a change to composed recipes retargets this function and
   nothing else. */
function inputRecipeOf(member, where) {
  if (member.assumed === null) return { provenance: member.provenance, asked: false, verification: null, steps: null };
  const steps = (member.acts === null ? [] : member.acts.map((a) => inputActStep(a, where))).concat(
    member.assumed.map((x) => {
      const holes = astHoleNames("source" in x ? x.source : x.decision);
      return { text: endpointAssumptionText(x, where),
               inputs: holes.length === 0 && "source" in x ? [x.source] : holes };
    }));
  return { provenance: member.provenance, asked: true, verification: member.verification, steps: steps };
}

/* One act of a sighting's act list as a step: a message posted to the window (its route, and the data its
   domain composes, as one unit) or a victim act on its target. */
function inputActStep(a, where) {
  DCHECK(!!a && typeof a === "object" && typeof a.kind === "string" && typeof a.target === "string",
         "a stored witness act is not {kind, target, …} (" + where + "): " + JSON.stringify(a));
  if (a.kind === "message") {
    return { text: "a message posted to the window by " + a.route + " route " + messageOriginClause(a) +
                   ("data" in a ? " with data " + JSON.stringify(a.data) : ", its data not composed"),
             inputs: ["postMessage"] };
  }
  return { text: a.kind + " " + a.target + (a.type !== a.kind ? " (" + a.type + ")" : ""), inputs: ["user act"] };
}

/* The regrouping. `inputs` maps an input's name to {conditions, parts}; `unknown` holds, once per reason, the
   endpoints some sighting of which states no conditions. An endpoint every sighting of which was asked and
   assumed nothing, and none of whose parts is unknown, appears in neither. */
function inputView(td) {
  DCHECK(td.discoveryDocs && typeof td.discoveryDocs === "object" && td.endpoints && typeof td.endpoints === "object",
         "GET_STATE carried no discoveryDocs/endpoints map — serializeTabData writes both on every reply");
  const inputs = new Map(), unknown = new Map(), partsOwed = [];
  const at = (name) => {
    if (!inputs.has(name)) inputs.set(name, { conditions: [], parts: [] });
    return inputs.get(name);
  };
  const unknownFor = (row, why) => unknown.set(JSON.stringify([row.svcName, row.m.id, why]), { row: row, why: why });
  for (const svcName of Object.keys(td.discoveryDocs).sort()) {
    const svc = td.discoveryDocs[svcName];
    if (!svc.doc) continue;
    for (const m of getDocMethods(svc.doc)) {
      if (methodProvenance(m, "lib/popup-inputs.js") === null) continue;   // no bundle sighting
      const where = "lib/popup-inputs.js for " + JSON.stringify(m.id) + " of service " + JSON.stringify(svcName);
      const row = { svcName: svcName, m: m };
      const pool = methodWitnesses(m, where);
      if (pool === undefined) {
        unknownFor(row, "the engine build that reported this method's sightings stated no witness");
      } else {
        for (const member of pool) {
          const recipe = inputRecipeOf(member, where);
          if (!recipe.asked) { unknownFor(row, "a " + recipe.provenance + " sighting no flow stood at"); continue; }
          if (recipe.steps.length === 0 && recipe.provenance === "forced") {
            unknownFor(row, "a forced sighting whose forcing act filed no entry (assumption not spellable)");
            continue;
          }
          for (const step of recipe.steps) {
            if (step.inputs.length === 0) unknownFor(row, "a condition whose spelling names no input: " + step.text);
            for (const name of new Set(step.inputs)) at(name).conditions.push({ row: row, recipe: recipe, step: step });
          }
        }
      }
      for (const part of inputPartsOf(m, where)) partsOwed.push({ row: row, part: part });
    }
  }
  /* A part spelled with braces names its inputs directly. A path parameter is named by its hole's key, the
     shape with its braces removed, so it is filed under the longest input name this view already holds that
     the key begins at a member or call boundary; a key no input begins is filed under itself. */
  for (const { row, part } of partsOwed) {
    if (part.inputs.length === 0 && !part.path) {
      unknownFor(row, "the value of " + part.label + " is unknown and its spelling names no input");
      continue;
    }
    for (const name of new Set(part.inputs.length ? part.inputs : [inputOfHoleKey(part.key, inputs)])) {
      at(name).parts.push({ row: row, part: part });
    }
  }
  return { inputs: inputs, unknown: [...unknown.values()] };
}

function inputOfHoleKey(key, inputs) {
  let best = null;
  for (const name of inputs.keys()) {
    if (name === "" || !key.startsWith(name)) continue;
    const next = key.charAt(name.length);
    if (next !== "" && next !== "." && next !== "(" && next !== "[") continue;
    if (best === null || name.length > best.length) best = name;
  }
  return best === null ? key : best;
}

/* The parts of one method whose value the page's code did not fix: each parameter whose value class is
   `unknown` or whose example spells a hole (a path parameter is one of the address's hole segments, named by
   the hole's key), and the body shape. Each is {label, inputs, key}: `inputs` the brace-delimited names its
   spelling holds, `key` its own name. */
function inputPartsOf(m, where) {
  const parts = [];
  if (m.parameters !== undefined) {
    for (const name of Object.keys(m.parameters)) {
      const p = m.parameters[name];
      const spelled = [];
      for (const field of ["_astValidValues", "_astForcedValues"]) {
        if (p[field] !== undefined) for (const v of p[field]) spelled.push(...astHoleNames(v));
      }
      if (spelled.length === 0 && p._astValueClass !== "unknown") continue;
      const loc = typeof p.location === "string" && p.location !== "" ? p.location : "location not stated";
      const path = p.location === "path";
      parts.push({ label: name + " (" + loc + ")", inputs: path ? [] : spelled, key: name, path: path });
    }
  }
  const ep = endpointRecordOf(m, where);
  if (ep !== null && ep.bodyShape !== null) {
    parts.push({ label: "body " + ep.bodyShape.shape, inputs: astHoleNames(ep.bodyShape.shape),
                 key: ep.bodyShape.shape, path: false });
  }
  return parts;
}

function inputEndpointLabel(row) {
  return [domElement("span", { class: "ep-method" }, [row.m.httpMethod]), " ",
          domElement("code", {}, [row.svcName + "/" + row.m.path])];
}

/* One input: each endpoint once, with the distinct conditions over this input its sightings were reached
   under (each with its grades and whether any was verified) and the parts whose value this input spells. */
function inputSection(name, entry) {
  const byEndpoint = new Map();
  const of = (row) => {
    const k = JSON.stringify([row.svcName, row.m.id]);
    if (!byEndpoint.has(k)) byEndpoint.set(k, { row: row, conds: new Map(), parts: new Set() });
    return byEndpoint.get(k);
  };
  for (const { row, recipe, step } of entry.conditions) {
    const conds = of(row).conds;
    if (!conds.has(step.text)) conds.set(step.text, { grades: new Set(), verified: false });
    const c = conds.get(step.text);
    c.grades.add(recipe.provenance);
    c.verified = c.verified || recipe.verification === "verified";
  }
  for (const { row, part } of entry.parts) of(row).parts.add(part.label);
  const items = [...byEndpoint.values()].map(({ row, conds, parts }) => {
    const lines = [];
    for (const [text, c] of conds) {
      lines.push(domElement("li", {}, [
        ...[...c.grades].sort().map((g) => domElement("span", { class: "ep-grade ep-grade-" + g }, [g])), " ",
        domElement("span", { class: "ep-verify ep-verify-" + (c.verified ? "verified" : "unverified") },
                   [c.verified ? "engine-verified" : "unverified"]),
        " reached assuming ", domElement("code", { class: "ep-assumption" }, [text])]));
    }
    for (const label of parts) {
      lines.push(domElement("li", {}, ["value of ", domElement("code", { class: "ep-param-name" }, [label])]));
    }
    return domElement("li", {}, [domElement("div", {}, inputEndpointLabel(row)), domElement("ul", {}, lines)]);
  });
  return domElement("section", { class: "input-group" }, [
    domElement("h3", {}, [domElement("code", {}, [name === "" ? "{} (unnameable)" : name])]),
    domElement("ul", { class: "ep-witness" }, items)]);
}

function inputUnknownSection(unknown) {
  return domElement("section", { class: "input-group" }, [
    domElement("h3", {}, ["Input not stated"]),
    domElement("ul", { class: "ep-witness" }, unknown.map(({ row, why }) =>
      domElement("li", { class: "ep-unknown" }, [...inputEndpointLabel(row), " " + ENDPOINT_UNKNOWN + ": " + why])))]);
}
