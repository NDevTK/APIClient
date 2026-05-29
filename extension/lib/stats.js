// lib/stats.js — Parameter statistics engine
// Tracks per-parameter observation counts, value distributions, format hints,
// numeric ranges, and cross-parameter correlations.

const STATS_MAX_UNIQUE_VALUES = 50;
const STATS_MIN_OBS_FOR_REQUIRED = 3;
const STATS_MIN_OBS_FOR_ENUM = 5;
const STATS_MAX_ENUM_VALUES = 20;
const STATS_DEFAULT_THRESHOLD = 0.8;

function createParamStats() {
  return {
    observedCount: 0,
    values: {},
    numericRange: null,
    formatHints: { "date-time": 0, uri: 0, email: 0, uuid: 0, integer: 0 },
  };
}

function updateParamStats(stats, value) {
  stats.observedCount++;

  // Track value frequencies (capped)
  const strVal = String(value);
  if (Object.keys(stats.values).length < STATS_MAX_UNIQUE_VALUES || stats.values[strVal] != null) {
    stats.values[strVal] = (stats.values[strVal] || 0) + 1;
  }

  // Numeric range
  const num = Number(value);
  if (!isNaN(num) && isFinite(num)) {
    if (!stats.numericRange) {
      stats.numericRange = { min: num, max: num };
    } else {
      if (num < stats.numericRange.min) stats.numericRange.min = num;
      if (num > stats.numericRange.max) stats.numericRange.max = num;
    }
  }

  // Format detection — real parsing, not regex
  detectFormat(stats, strVal);
}

function detectFormat(stats, value) {
  // date-time: parseable date with structural indicators. new Date(invalid)
  // returns Invalid Date (NaN getTime) instead of throwing, so the try/catch
  // is just defensive against unexpected platform behavior — the isNaN check
  // is the real validity test. No try needed.
  if (value.length >= 8 && (value.includes("-") || value.includes("T"))) {
    const d = new Date(value);
    if (!isNaN(d.getTime()) && d.getFullYear() > 1900 && d.getFullYear() < 2200) {
      stats.formatHints["date-time"]++;
    }
  }

  // uri: canParse guard — root-cause fix for the URL-parse-as-validity test.
  if (value.length >= 8 && (value.startsWith("http://") || value.startsWith("https://")) && URL.canParse(value)) {
    const u = new URL(value);
    if (u.protocol === "http:" || u.protocol === "https:") {
      stats.formatHints.uri++;
    }
  }

  // email: exactly one @ with text on both sides and a dot after @
  if (value.includes("@")) {
    const parts = value.split("@");
    if (parts.length === 2 && parts[0].length > 0 && parts[1].includes(".") && parts[1].length > 2) {
      stats.formatHints.email++;
    }
  }

  // uuid: 36 chars, correct dash positions, valid hex
  if (value.length === 36) {
    const segments = value.split("-");
    if (segments.length === 5 &&
        segments[0].length === 8 && segments[1].length === 4 &&
        segments[2].length === 4 && segments[3].length === 4 &&
        segments[4].length === 12) {
      let allHex = true;
      for (let i = 0; i < value.length; i++) {
        const c = value.charCodeAt(i);
        if (value[i] === "-") continue;
        if (!((c >= 48 && c <= 57) || (c >= 65 && c <= 70) || (c >= 97 && c <= 102))) {
          allHex = false;
          break;
        }
      }
      if (allHex) stats.formatHints.uuid++;
    }
  }

  // integer: strictly digits with optional leading sign
  if (value.length > 0) {
    const n = Number(value);
    if (Number.isInteger(n) && String(n) === value) {
      stats.formatHints.integer++;
    }
  }
}

function analyzeRequired(stats, requestCount) {
  if (requestCount < STATS_MIN_OBS_FOR_REQUIRED) {
    return { required: false, confidence: stats.observedCount / Math.max(requestCount, 1) };
  }
  const confidence = stats.observedCount / requestCount;
  return { required: confidence >= 1.0, confidence };
}

function analyzeEnum(stats) {
  if (stats.observedCount < STATS_MIN_OBS_FOR_ENUM) {
    return { isEnum: false, values: [] };
  }
  const uniqueValues = Object.keys(stats.values);
  if (uniqueValues.length >= 2 && uniqueValues.length <= STATS_MAX_ENUM_VALUES) {
    // All observed values fit in a small set — likely an enum
    return { isEnum: true, values: uniqueValues };
  }
  return { isEnum: false, values: [] };
}

function analyzeDefault(stats) {
  if (stats.observedCount < STATS_MIN_OBS_FOR_REQUIRED) {
    return { hasDefault: false, value: null, confidence: 0 };
  }
  let maxCount = 0;
  let maxValue = null;
  for (const [val, count] of Object.entries(stats.values)) {
    if (count > maxCount) {
      maxCount = count;
      maxValue = val;
    }
  }
  const confidence = maxCount / stats.observedCount;
  if (confidence >= STATS_DEFAULT_THRESHOLD) {
    return { hasDefault: true, value: maxValue, confidence };
  }
  return { hasDefault: false, value: null, confidence };
}

function analyzeFormat(stats) {
  if (stats.observedCount < STATS_MIN_OBS_FOR_REQUIRED) return null;

  // Find the dominant format hint (must be >80% of observations)
  const threshold = stats.observedCount * STATS_DEFAULT_THRESHOLD;
  for (const [format, count] of Object.entries(stats.formatHints)) {
    if (count >= threshold) return format;
  }
  return null;
}

function analyzeRange(stats) {
  if (!stats.numericRange) return null;
  if (stats.numericRange.min === stats.numericRange.max) return null;
  return stats.numericRange;
}

/**
 * Detect cross-parameter correlations.
 * @param {object} methodStats - The method's _stats object
 * @returns {Array} correlation entries
 */
function detectCorrelations(methodStats) {
  if (methodStats.requestCount < STATS_MIN_OBS_FOR_ENUM) return [];

  const paramNames = Object.keys(methodStats.params);
  if (paramNames.length < 2) return [];

  const correlations = [];

  // For each pair of params, check if one's presence predicts the other
  for (let i = 0; i < paramNames.length; i++) {
    for (let j = i + 1; j < paramNames.length; j++) {
      const a = methodStats.params[paramNames[i]];
      const b = methodStats.params[paramNames[j]];

      // If both appear in nearly the same proportion, they're correlated
      if (a.observedCount >= 3 && b.observedCount >= 3) {
        const ratio = Math.min(a.observedCount, b.observedCount) / Math.max(a.observedCount, b.observedCount);
        if (ratio >= 0.9) {
          correlations.push({
            paramA: paramNames[i],
            paramB: paramNames[j],
            confidence: ratio,
          });
        }
      }
    }
  }

  return correlations.slice(0, 20);
}

// Convert a string observation back to the field's declared type when the
// type is numeric or boolean. Observations are always stored as strings
// (for dedup), but when we present an "example value" to the user or to
// the request builder, we want a properly-typed value so
// encodeFormToJson/encodeFormToJspb don't double-quote numbers, etc.
function _coerceToFieldType(value, type) {
  if (value == null) return value;
  if (!type) return value;
  if (type === "boolean" || type === "bool") {
    if (value === true || value === false) return value;
    if (value === "true") return true;
    if (value === "false") return false;
    return value;
  }
  if (type === "number" || type === "integer" ||
      type === "int32" || type === "int64" || type === "uint32" || type === "uint64" ||
      type === "sint32" || type === "sint64" || type === "double" || type === "float" ||
      type === "fixed32" || type === "fixed64" || type === "sfixed32" || type === "sfixed64") {
    if (typeof value === "number") return value;
    const n = Number(value);
    return Number.isNaN(n) ? value : n;
  }
  return value;
}

// Return ONE example value for a field, with provenance, so the UI and
// request-builder can always present a usable value. Priority is
// observed-facts → AST-facts → declared-schema:
//   1. "observed-default" — stats distribution was dominant enough for
//      analyzeDefault() to fire (>=80% of observations).
//   2. "observed-top"     — most-frequent observed value, even at low
//      dominance. Beats declared enum because real traffic that got
//      through the server is a stronger signal than spec-declared
//      order (enum[0] is usually alphabetical or author-convention,
//      not "the value most likely to succeed").
//   3. "ast-constraint"   — _astValidValues from AST (switch/case,
//      .includes, equality chains). Facts about the client's code,
//      but may list values the server never actually receives.
//   4. "enum"             — first declared enum value (spec-level data).
//
// Removed (legacy): "format-synth", "range-min", "type-default" — these
// synthesised placeholder values when no real source existed, violating
// the project rule "no placeholders / opaque fallbacks". A field with
// no traceable value returns null so callers surface the gap honestly
// instead of acting on fabricated data.
function pickExampleValue(field, stats) {
  const type = field && field.type ? field.type : null;

  // 1. observed-default — analyzeDefault already accepted this
  if (field && field._defaultValue != null) {
    return {
      value: _coerceToFieldType(field._defaultValue, type),
      source: "observed-default",
      confidence: field._defaultConfidence || null,
    };
  }
  // 2. observed-top — most frequent observed value. Wins over declared
  //    enum because a value that actually shipped through the server
  //    is a stronger signal than spec-declared order.
  if (stats && stats.values) {
    const keys = Object.keys(stats.values);
    if (keys.length) {
      let top = keys[0];
      let topCount = stats.values[top];
      for (let i = 1; i < keys.length; i++) {
        if (stats.values[keys[i]] > topCount) { top = keys[i]; topCount = stats.values[keys[i]]; }
      }
      return {
        value: _coerceToFieldType(top, type),
        source: "observed-top",
        confidence: stats.observedCount ? topCount / stats.observedCount : null,
      };
    }
  }
  // 3. ast-constraint — switch cases, includes() arguments, literal chains
  if (field && Array.isArray(field._astValidValues) && field._astValidValues.length) {
    return { value: field._astValidValues[0], source: "ast-constraint" };
  }
  // 4. enum — declared enum OR detected enum
  if (field && Array.isArray(field.enum) && field.enum.length) {
    return { value: _coerceToFieldType(field.enum[0], type), source: "enum" };
  }
  // No real value could be derived from observed traffic, AST analysis,
  // or declared schema. Return null — callers must handle absence.
  return null;
}

function mergeParamStats(a, b) {
  if (!a) return b;
  if (!b) return a;

  const merged = createParamStats();
  merged.observedCount = a.observedCount + b.observedCount;

  // Merge values
  for (const [val, count] of Object.entries(a.values)) {
    merged.values[val] = (merged.values[val] || 0) + count;
  }
  for (const [val, count] of Object.entries(b.values)) {
    merged.values[val] = (merged.values[val] || 0) + count;
  }
  // Trim if over cap
  const entries = Object.entries(merged.values);
  if (entries.length > STATS_MAX_UNIQUE_VALUES) {
    entries.sort((x, y) => y[1] - x[1]);
    merged.values = {};
    for (let i = 0; i < STATS_MAX_UNIQUE_VALUES; i++) {
      merged.values[entries[i][0]] = entries[i][1];
    }
  }

  // Merge numeric range
  if (a.numericRange && b.numericRange) {
    merged.numericRange = {
      min: Math.min(a.numericRange.min, b.numericRange.min),
      max: Math.max(a.numericRange.max, b.numericRange.max),
    };
  } else {
    merged.numericRange = a.numericRange || b.numericRange;
  }

  // Merge format hints
  for (const key of Object.keys(merged.formatHints)) {
    merged.formatHints[key] = (a.formatHints[key] || 0) + (b.formatHints[key] || 0);
  }

  return merged;
}
