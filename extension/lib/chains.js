// lib/chains.js — Response-to-request value chaining engine
// Tracks when response values from one API call appear as parameters in another.

const CHAIN_MIN_VALUE_LENGTH = 4;
const CHAIN_MAX_VALUE_LENGTH = 500;
const CHAIN_IGNORE_VALUES = new Set([
  "true", "false", "null", "undefined", "0", "1", "-1",
  "GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS",
  "application/json", "text/plain", "text/html",
  "utf-8", "UTF-8", "en", "en-US", "en-GB",
]);

function createValueIndex() {
  return { strings: new Map(), numbers: new Map() };
}

/**
 * Index values from a parsed response body.
 * @param {object} index - The value index { strings: Map, numbers: Map }
 * @param {*} body - Parsed response body (object, array, or primitive)
 * @param {string} methodId - Source method ID
 * @param {string} prefix - Field path prefix for recursion
 */
function indexResponseValues(index, body, methodId, prefix) {
  // Iterative worklist over (value, path). Replaces self-recursion so
  // adversarially-deep response payloads can't blow the JS stack. Also
  // drops the original 50-element array cap — every reachable string /
  // number gets indexed, matching the resolver-completeness rule.
  var queue = [{ value: body, path: prefix === undefined ? "" : prefix }];
  while (queue.length > 0) {
    var entry = queue.shift();
    var v = entry.value, p = entry.path;
    if (Array.isArray(v)) {
      for (var i = 0; i < v.length; i++) {
        queue.push({ value: v[i], path: p + "[" + i + "]" });
      }
      continue;
    }
    if (typeof v === "object" && v !== null) {
      var keys = Object.keys(v);
      for (var k = 0; k < keys.length; k++) {
        var key = keys[k];
        queue.push({ value: v[key], path: p ? p + "." + key : key });
      }
      continue;
    }
    if (typeof v === "string") {
      if (v.length >= CHAIN_MIN_VALUE_LENGTH &&
          v.length <= CHAIN_MAX_VALUE_LENGTH &&
          !CHAIN_IGNORE_VALUES.has(v)) {
        var entries = index.strings.get(v);
        if (!entries) { entries = []; index.strings.set(v, entries); }
        var isDupe = false;
        for (var d = 0; d < entries.length; d++) {
          if (entries[d].methodId === methodId && entries[d].fieldPath === p) {
            isDupe = true;
            break;
          }
        }
        if (!isDupe) entries.push({ methodId: methodId, fieldPath: p, timestamp: Date.now() });
      }
      continue;
    }
    if (typeof v === "number" && isFinite(v) && v !== 0 && v !== 1 && v !== -1) {
      var numEntries = index.numbers.get(v);
      if (!numEntries) { numEntries = []; index.numbers.set(v, numEntries); }
      var isNumDupe = false;
      for (var nd = 0; nd < numEntries.length; nd++) {
        if (numEntries[nd].methodId === methodId && numEntries[nd].fieldPath === p) {
          isNumDupe = true;
          break;
        }
      }
      if (!isNumDupe) numEntries.push({ methodId: methodId, fieldPath: p, timestamp: Date.now() });
    }
  }
}

/**
 * Check request parameter values against the value index to find chains.
 * @param {object} index - The value index
 * @param {object} params - Request query parameters { name: value }
 * @param {object} bodyValues - Flat map of body field paths to values
 * @param {string} targetMethodId - The method being called
 * @returns {Array} chain link objects
 */
function findChainLinks(index, params, bodyValues, targetMethodId) {
  var links = [];

  // Check query/path params
  for (var pNames = Object.keys(params), p = 0; p < pNames.length; p++) {
    var name = pNames[p];
    var found = _lookupValue(index, params[name]);
    if (found) {
      for (var f = 0; f < found.length; f++) {
        if (found[f].methodId !== targetMethodId) {
          links.push({
            paramName: name,
            paramLocation: "query",
            sourceMethodId: found[f].methodId,
            sourceFieldPath: found[f].fieldPath,
            lastSeen: Date.now(),
          });
        }
      }
    }
  }

  // Check body field values
  for (var bKeys = Object.keys(bodyValues), b = 0; b < bKeys.length; b++) {
    var bKey = bKeys[b];
    var bFound = _lookupValue(index, bodyValues[bKey]);
    if (bFound) {
      for (var bf = 0; bf < bFound.length; bf++) {
        if (bFound[bf].methodId !== targetMethodId) {
          links.push({
            paramName: bKey,
            paramLocation: "body",
            sourceMethodId: bFound[bf].methodId,
            sourceFieldPath: bFound[bf].fieldPath,
            lastSeen: Date.now(),
          });
        }
      }
    }
  }

  return links;
}

function _lookupValue(index, value) {
  if (typeof value === "string" && value.length >= CHAIN_MIN_VALUE_LENGTH && value.length <= CHAIN_MAX_VALUE_LENGTH) {
    return index.strings.get(value) || null;
  }
  if (typeof value === "number" && isFinite(value) && value !== 0 && value !== 1) {
    return index.numbers.get(value) || null;
  }
  return null;
}

/**
 * Flatten a JSON object into { "path.to.field": value } map.
 */
function flattenObjectValues(obj, prefix, result) {
  // Iterative worklist (value, path). Replaces self-recursion and drops
  // the 20-element array cap so every reachable leaf gets included in
  // the flattened view.
  if (!result) result = {};
  var queue = [{ value: obj, path: prefix || "" }];
  while (queue.length > 0) {
    var entry = queue.shift();
    var v = entry.value, p = entry.path;
    if (typeof v !== "object" || v === null) {
      if (p) result[p] = v;
      continue;
    }
    if (Array.isArray(v)) {
      for (var i = 0; i < v.length; i++) {
        queue.push({ value: v[i], path: p + "[" + i + "]" });
      }
      continue;
    }
    var keys = Object.keys(v);
    for (var k = 0; k < keys.length; k++) {
      var key = keys[k];
      queue.push({ value: v[key], path: p ? p + "." + key : key });
    }
  }
  return result;
}

/**
 * Merge new chain links into existing method chain data.
 */
function mergeChainLinks(existing, newLinks) {
  if (!existing) existing = { incoming: [], outgoing: [] };

  for (var i = 0; i < newLinks.length; i++) {
    var link = newLinks[i];
    var match = null;
    for (var j = 0; j < existing.incoming.length; j++) {
      var c = existing.incoming[j];
      if (c.paramName === link.paramName &&
          c.sourceMethodId === link.sourceMethodId &&
          c.sourceFieldPath === link.sourceFieldPath) {
        match = c;
        break;
      }
    }
    if (match) {
      match.observedCount = (match.observedCount || 1) + 1;
      match.lastSeen = link.lastSeen;
    } else {
      existing.incoming.push({
        paramName: link.paramName,
        paramLocation: link.paramLocation,
        sourceMethodId: link.sourceMethodId,
        sourceFieldPath: link.sourceFieldPath,
        observedCount: 1,
        lastSeen: link.lastSeen,
      });
    }
  }

  return existing;
}
