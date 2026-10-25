/* lib/popup-endpoints.js — the Endpoints panel: one row per method the forced execution learned (a method
   record with `_astInferred`, lib/learn.js), stating its verb, its address, the razor class its sightings
   stated, the address's provenance, what each sighting's flow assumed, each parameter with the example values
   the run computed, and the request body the run observed; a row's Send button opens the method in the Send panel. Every cell is a
   producer field read as itself; a fact no hop carries reads "unknown". Built only through
   lib/dom-builder.js. */

const ENDPOINT_UNKNOWN = "unknown";

// The panel. Reads `tabData` (GET_STATE, lib/serialize.js's serializeTabData) and nothing else.
function renderEndpointsPanel() {
  const body = document.getElementById("endpoints-body");
  // A null tabData is popup.js's own state for "GET_STATE has not answered, or Clear just emptied the view".
  if (tabData === null) {
    domReplaceChildren(body, [domElement("div", { class: "empty" }, ["No state loaded."])]);
    return;
  }
  DCHECK(tabData.discoveryDocs && typeof tabData.discoveryDocs === "object" &&
         tabData.endpoints && typeof tabData.endpoints === "object",
         "GET_STATE carried no discoveryDocs/endpoints map — serializeTabData writes both on every reply");
  const rows = [];
  for (const svcName of Object.keys(tabData.discoveryDocs).sort()) {
    const svc = tabData.discoveryDocs[svcName];
    if (!svc.doc) continue;   // an outcome-only record: the published fetch's status, no method surface
    for (const m of getDocMethods(svc.doc)) {
      if (methodProvenance(m, "lib/popup-endpoints.js row") === null) continue;   // no bundle sighting
      rows.push(endpointRow(svcName, svc.doc, m));
    }
  }
  if (rows.length === 0) {
    domReplaceChildren(body, [domElement("div", { class: "empty" }, [
      "No endpoint has been learned by forced execution in this view yet — not a verdict that the bundle " +
      "calls none."])]);
    return;
  }
  const head = domElement("thead", {}, [domElement("tr", {}, ["Method", "Address", "Razor", "Provenance",
    "Assumed", "Parameters", ""].map((h) => domElement("th", { scope: "col" }, [h])))]);
  domReplaceChildren(body, [domElement("table", { class: "ep-table" }, [head, domElement("tbody", {}, rows)])]);
}

function endpointRow(svcName, doc, m) {
  const where = "lib/popup-endpoints.js row for " + JSON.stringify(m.id) + " of service " + JSON.stringify(svcName);
  const prov = methodProvenance(m, where);
  const ep = endpointRecordOf(m, where);
  return domElement("tr", {}, [
    domElement("td", { class: "ep-method" }, [m.httpMethod]),
    domElement("td", { class: "ep-address" }, [endpointAddressCell(m, ep)]),
    domElement("td", {}, [endpointRazorCell(m, where)]),
    domElement("td", {}, [domElement("span", { class: "ep-grade ep-grade-" + prov }, [prov]),
                          ...(methodDriven(m, where) ? [" ", domElement("span", { class: "ep-grade ep-driven" },
                                                                        ["driven"])] : [])]),
    domElement("td", {}, [endpointWitnessCell(m, where)]),
    domElement("td", {}, [endpointParamsCell(doc, m, ep, where)]),
    domElement("td", {}, [endpointSendButton(svcName, m)]),
  ]);
}

/* Opens the method in the Send panel through the panel's own controls, so the request it shows is the one
   its form composes from the method record (one computed value prefilled, several offered, none left empty).
   Each control is driven by a change event, which runs the handler popup.js registered on it. */
function endpointSendButton(svcName, m) {
  const button = domElement("button", { type: "button", class: "btn-small",
    title: "open this method in the Send panel with the example values the run computed" }, ["Send…"]);
  button.addEventListener("click", () => {
    const svcSelect = document.getElementById("spec-service-select");
    svcSelect.value = svcName;
    DCHECK(svcSelect.value === svcName,
           "the Send panel's service list has no option " + JSON.stringify(svcName) + " — renderSendPanel lists " +
           "every discoveryDocs entry with a doc, which this row was built from");
    svcSelect.dispatchEvent(new Event("change"));
    const epSelect = document.getElementById("send-ep-select");
    const key = sendMethodOptionKey(svcName, m);
    epSelect.value = key;
    DCHECK(epSelect.value === key,
           "the Send panel's method list has no option " + JSON.stringify(key) + " — renderMethodDropdown lists " +
           "every method of the selected service, which this row was built from");
    epSelect.dispatchEvent(new Event("change"));
    document.querySelector(".tab[data-panel='send']").click();
  });
  return button;
}

/* The flat endpoint record the method names through lib/merge.js's `_endpointKey` stamp, or null. A method
   stored before the stamp, or one whose record the moat no longer holds, names no record in this reply, and
   its address and body are then not stated by anything the popup holds. */
function endpointRecordOf(m, where) {
  const key = m._endpointKey;
  if (typeof key !== "string" || !Object.prototype.hasOwnProperty.call(tabData.endpoints, key)) return null;
  const ep = tabData.endpoints[key];
  checkEndpointRecord(ep, where);
  DCHECK(ep.method === m.httpMethod,
         "the learned method names endpoint record " + JSON.stringify(key) + ", whose verb is " +
         JSON.stringify(ep.method) + " and not " + JSON.stringify(m.httpMethod) + " (" + where + ")");
  return ep;
}

function endpointAddressCell(m, ep) {
  if (ep === null) {
    return domElement("span", { class: "ep-unknown",
      title: "no endpoint record in this view is named by this method; its path template is " +
             JSON.stringify(m.path) }, [ENDPOINT_UNKNOWN]);
  }
  return domElement("code", {}, [ep.url]);
}

function endpointRazorCell(m, where) {
  const set = methodRazorClasses(m, where);
  if (set === undefined) {
    return domElement("span", { class: "ep-unknown",
      title: "the engine build that reported this method's sightings stated no razor class" }, [ENDPOINT_UNKNOWN]);
  }
  return domElement("span", {}, set.map((c) => domElement("span", { class: "ep-razor ep-razor-" + c }, [c])));
}

/* What each sighting's flow assumed (lib/endpoint-record.js's `methodWitnesses`), one line per sighting and
   never merged. A sighting with assumptions reads "found by assuming" and its entries, after its verification:
   "engine-verified" where an engine run firing the value reached the request (never a live run), "unverified" otherwise, with why in the title
   (never "infeasible": an unverified witness is a parked search); a forced sighting with
   none is solver/endpoint.h's named residual (a forced act that files no decision) and says so rather than
   reading as nothing assumed; an unforced sighting with none assumed nothing and adds no line; a sighting no
   flow stood at was not asked. */
function endpointWitnessCell(m, where) {
  const pool = methodWitnesses(m, where);
  if (pool === undefined) {
    return domElement("span", { class: "ep-unknown",
      title: "the engine build that reported this method's sightings stated no witness" }, [ENDPOINT_UNKNOWN]);
  }
  const items = [];
  for (const { provenance, assumed, verification } of pool) {
    const grade = domElement("span", { class: "ep-grade ep-grade-" + provenance }, [provenance]);
    if (assumed === null) {
      items.push(domElement("li", { class: "ep-none" }, [grade, " no flow stood at this sighting; not asked"]));
    } else if (assumed.length === 0) {
      if (provenance !== "forced") continue;
      items.push(domElement("li", { class: "ep-unknown",
        title: "the path was forced by an act that files no decision (solver/endpoint.h's witness residual)" },
        [grade, " assumption not spellable"]));
    } else {
      const verified = verification === "verified";
      const parts = [grade, " ", domElement("span", {
        class: "ep-verify ep-verify-" + (verified ? "verified" : "unverified"),
        title: verified ? "an engine run with this value delivered reached the request assuming nothing else; no "
            + "live run reproduced it"
          : verification === "candidate" ? "a verification run was minted and has not reached the request"
          : "firing cannot check this witness yet (solver/endpoint.c's witness_payload residual)" },
        [verified ? "engine-verified" : "unverified"]), " found by assuming "];
      assumed.forEach((x, i) => {
        if (i) parts.push("; ");
        parts.push(domElement("code", { class: "ep-assumption" }, [endpointAssumptionText(x, where)]));
      });
      items.push(domElement("li", {}, parts));
    }
  }
  if (items.length === 0) return domElement("span", { class: "ep-none" }, ["nothing assumed"]);
  return domElement("ul", { class: "ep-witness" }, items);
}

/* One entry, in the three shapes extension/bridge.js asserts at the seam. A pin's String value is quoted so it
   cannot read as the undefined, null, number or boolean of the same spelling. */
function endpointAssumptionText(x, where) {
  if ("source" in x) {
    DCHECK(typeof x.source === "string" && typeof x.value === "string" && typeof x.kind === "string",
           "a stored witness pin is not {source, value, kind} (" + where + "): " + JSON.stringify(x));
    return x.source + " = " + (x.kind === "string" ? JSON.stringify(x.value) : x.value);
  }
  if ("taken" in x) {
    DCHECK(typeof x.decision === "string" && typeof x.taken === "boolean",
           "a stored witness branch is not {decision, taken} (" + where + "): " + JSON.stringify(x));
    return x.decision + " → " + x.taken;
  }
  DCHECK(typeof x.decision === "string" && Number.isInteger(x.completion),
         "a stored witness entry is none of the three shapes (" + where + "): " + JSON.stringify(x));
  return x.decision + " → completion " + x.completion;
}

/* Path and query parameters from the method, then the fields of its request schema, then the body the flat
   record carries. The schema is read whoever created it: lib/learn.js merges the engine's body fields into a
   schema live traffic created first without marking that schema `_astInferred`, so that flag says who
   minted the schema, not whether the engine computed its fields. RestDescription makes `parameters`,
   `request` and a schema's `properties` optional, and an absent one states there are none. */
function endpointParamsCell(doc, m, ep, where) {
  const items = [];
  if (m.parameters !== undefined) {
    for (const name of Object.keys(m.parameters)) items.push(endpointParamItem(name, m.parameters[name], where));
  }
  // lib/learn.js writes `request: null` for a method with no body; a probed document omits the key.
  if (m.request !== null && m.request !== undefined) {
    // A probed document is a server's input and may name a schema it does not hold; that is said, not asserted.
    const schemas = doc.schemas === undefined ? {} : doc.schemas;
    if (!Object.prototype.hasOwnProperty.call(schemas, m.request.$ref)) {
      items.push(domElement("li", { class: "ep-unknown" }, ["request body schema " +
        JSON.stringify(m.request.$ref) + " is not in this document"]));
    } else if (schemas[m.request.$ref].properties !== undefined) {
      const props = schemas[m.request.$ref].properties;
      for (const name of Object.keys(props)) items.push(endpointParamItem(name, props[name], where, "body"));
    }
  }
  if (ep !== null) {
    const bodyItem = endpointBodyItem(ep);
    if (bodyItem !== null) items.push(bodyItem);
  }
  if (items.length === 0) return domElement("span", { class: "ep-none" }, ["none learned"]);
  return domElement("ul", { class: "ep-params" }, items);
}

/* One parameter: its name, location and value class, and each example value with its grade. `_astValidValues` holds values some unforced path computed, which the record does
   not split between observed and derived, so they read "observed/derived"; `_astForcedValues` holds values
   every sighting of which stood on a forced arm. lib/learn.js writes each pool only when non-empty. */
function endpointParamItem(name, p, where, bodyLocation) {
  const loc = bodyLocation === undefined ? p.location : bodyLocation;
  // lib/learn.js states a location on every parameter it mints; a probed document's is that document's input.
  DCHECK(!p._astInferred || (typeof loc === "string" && loc !== ""),
         "a learned parameter " + JSON.stringify(name) + " states no location (" + where + ")");
  const pools = [["_astValidValues", "observed/derived"], ["_astForcedValues", "forced"]];
  const values = [];
  for (const [field, grade] of pools) {
    if (p[field] === undefined) continue;
    DCHECK(Array.isArray(p[field]) && p[field].length > 0 && p[field].every((v) => typeof v === "string"),
           "parameter " + JSON.stringify(name) + "'s " + field + " is not a non-empty list of strings (" +
           where + ") — lib/learn.js writes the pool only when it holds a value, and every value as a string");
    for (const v of p[field]) {
      /* An offered value only this tool's drives computed is marked (lib/learn.js's `_astDrivenValues`). */
      const driven = grade !== "forced" && Array.isArray(p._astDrivenValues) && p._astDrivenValues.indexOf(v) >= 0;
      const label = driven ? grade + ", driven" : grade;
      values.push(domElement("span", { class: "ep-value ep-value-" + (grade === "forced" ? "forced" : "offered"),
                                       title: label }, [domElement("code", {}, [v]), " " + label]));
    }
  }
  const head = [domElement("code", { class: "ep-param-name" }, [name]),
                " (" + (typeof loc === "string" && loc !== "" ? loc : "location not stated") + ")"];
  if (p._astValueClass !== undefined) {
    DCHECK(VALUE_CLASSES.indexOf(p._astValueClass) >= 0,
           "parameter " + JSON.stringify(name) + " carries the value class " + JSON.stringify(p._astValueClass) +
           ", none of " + VALUE_CLASSES.join("/") + " (" + where + ")");
    head.push(", value class " + p._astValueClass);
  }
  if (values.length === 0) values.push(domElement("span", { class: "ep-none" }, ["no example value learned"]));
  return domElement("li", {}, [domElement("div", {}, head), domElement("div", { class: "ep-values" }, values)]);
}

/* The request body the flat record carries: bytes the request sends (`bodySent`), the display spelling of a
   body built from unknown input (`bodyShape`), or what a byte-composed payload currently looks like
   (`bodyExample`). checkEndpointRecord asserts their shapes and that at most one is set. */
function endpointBodyItem(ep) {
  let label, text, mime;
  if (ep.bodySent !== null) { label = "body sent"; text = endpointBodyText(ep.bodySent.base64); mime = ep.bodySent.mime; }
  else if (ep.bodyShape !== null) { label = "body shape"; text = ep.bodyShape.shape; mime = ep.bodyShape.mime; }
  else if (ep.bodyExample !== null) { label = "body example"; text = endpointBodyText(ep.bodyExample.base64); mime = ep.bodyExample.mime; }
  else return null;
  return domElement("li", {}, [
    domElement("div", {}, [domElement("code", { class: "ep-param-name" }, [label]),
                           " (" + (mime === null ? "no type stated" : mime) + ")"]),
    domElement("div", { class: "ep-values" }, [domElement("code", { class: "ep-value" }, [text])]),
  ]);
}

// Base64 bytes as UTF-8 text where TextDecoder's fatal mode accepts them, else a byte count and the base64.
function endpointBodyText(base64) {
  const bin = atob(base64);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  try {
    return new TextDecoder("utf-8", { fatal: true }).decode(bytes);
  } catch (e) {
    RETHROW_FATAL(e);
    return bytes.length + " bytes, not UTF-8, base64 " + base64;
  }
}
