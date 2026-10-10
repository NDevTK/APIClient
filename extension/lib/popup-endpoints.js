/* lib/popup-endpoints.js — the Endpoints panel: one row per method the forced execution learned (a method
   record with `_astInferred`, lib/learn.js), stating its verb, its address, the razor class its sightings
   stated, the address's provenance, and each parameter with the example values the run computed and the
   grade of each value. Every cell is a producer field read as itself; a fact no hop carries reads "unknown".
   Built only through lib/dom-builder.js.
   NAMED RESIDUAL. Not covered: the four proved domains on a parameter (`_excludedValues`, `_bounds`,
   `_predicates`, `_looselyEquals`) and the request body's bytes. The next diff renders them in the parameter
   cell, as lib/popup-form.js badges them. Their absence shows as a hole with no example value whose row
   says nothing about what the run proved it must satisfy. */

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
    "Parameters"].map((h) => domElement("th", { scope: "col" }, [h])))]);
  domReplaceChildren(body, [domElement("table", { class: "ep-table" }, [head, domElement("tbody", {}, rows)])]);
}

function endpointRow(svcName, doc, m) {
  const where = "lib/popup-endpoints.js row for " + JSON.stringify(m.id) + " of service " + JSON.stringify(svcName);
  const prov = methodProvenance(m, where);
  return domElement("tr", {}, [
    domElement("td", { class: "ep-method" }, [m.httpMethod]),
    domElement("td", { class: "ep-address" }, [endpointAddressCell(m, where)]),
    domElement("td", {}, [endpointRazorCell(m, where)]),
    domElement("td", {}, [domElement("span", { class: "ep-grade ep-grade-" + prov }, [prov])]),
    domElement("td", {}, [endpointParamsCell(doc, m, where)]),
  ]);
}

/* The address is the flat endpoint record's `url`, reached by the name lib/merge.js stamps on the method
   (`_endpointKey`). A method stored before the stamp, or one whose record the moat no longer holds, names no
   record in this reply, and its address is then not stated by anything the popup holds. */
function endpointAddressCell(m, where) {
  const key = m._endpointKey;
  if (typeof key !== "string" || !Object.prototype.hasOwnProperty.call(tabData.endpoints, key)) {
    return domElement("span", { class: "ep-unknown",
      title: "no endpoint record in this view is named by this method; its path template is " +
             JSON.stringify(m.path) }, [ENDPOINT_UNKNOWN]);
  }
  const ep = tabData.endpoints[key];
  checkEndpointRecord(ep, where);
  DCHECK(ep.method === m.httpMethod,
         "the learned method names endpoint record " + JSON.stringify(key) + ", whose verb is " +
         JSON.stringify(ep.method) + " and not " + JSON.stringify(m.httpMethod) + " (" + where + ")");
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

/* Path and query parameters from the method, then the fields of a request body lib/learn.js minted. A
   method a probed discovery document declared may carry no `parameters` (RestDescription makes it
   optional), which states that it has none. */
function endpointParamsCell(doc, m, where) {
  const items = [];
  if (m.parameters !== undefined) {
    for (const name of Object.keys(m.parameters)) items.push(endpointParamItem(name, m.parameters[name], where));
  }
  if (m.request && typeof m.request.$ref === "string" && doc.schemas &&
      Object.prototype.hasOwnProperty.call(doc.schemas, m.request.$ref) && doc.schemas[m.request.$ref]._astInferred) {
    const props = doc.schemas[m.request.$ref].properties;
    for (const name of Object.keys(props)) items.push(endpointParamItem(name, props[name], where, "body"));
  }
  if (items.length === 0) return domElement("span", { class: "ep-none" }, ["none learned"]);
  return domElement("ul", { class: "ep-params" }, items);
}

/* One parameter: its name and location, whether its value named a hole, and each example value with its
   grade. `_astValidValues` holds values some unforced path computed, which the record does not split
   between observed and derived, so they read "observed/derived"; `_astForcedValues` holds values every
   sighting of which stood on a forced arm. lib/learn.js writes each pool only when non-empty. */
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
      values.push(domElement("span", { class: "ep-value ep-value-" + (grade === "forced" ? "forced" : "offered"),
                                       title: grade }, [domElement("code", {}, [v]), " " + grade]));
    }
  }
  const head = [domElement("code", { class: "ep-param-name" }, [name]),
                " (" + (typeof loc === "string" && loc !== "" ? loc : "location not stated") + ")"];
  if (p._astValueClass !== undefined) {
    DCHECK(VALUE_CLASSES.indexOf(p._astValueClass) >= 0,
           "parameter " + JSON.stringify(name) + " carries the value class " + JSON.stringify(p._astValueClass) +
           ", none of " + VALUE_CLASSES.join("/") + " (" + where + ")");
    head.push(" " + p._astValueClass);
  }
  if (values.length === 0) values.push(domElement("span", { class: "ep-none" }, ["no example value learned"]));
  return domElement("li", {}, [domElement("div", {}, head), domElement("div", { class: "ep-values" }, values)]);
}
