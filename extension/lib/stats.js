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

/* `seenThisRequest` IS A SET OF STATS OBJECTS AND IT IS WHAT MAKES `observedCount` A COUNT OF REQUESTS.
   It counted OCCURRENCES, and `analyzeRequired` divides it by `requestCount` — two different events under
   one quotient, which is CLAUDE.md's own tell verbatim: a subset that exceeds the population it claims to
   be drawn from. `URLSearchParams.forEach` VISITS A REPEATED NAME ONCE PER OCCURRENCE, so one request to
   `?sdk=a&sdk=b&sdk=c` raised this by three against a requestCount of one.
   IT IS NOT A DISPLAY DEFECT. The popup rendered `seen 300%`, which is merely absurd; `analyzeRequired`
   returns `required: confidence >= 1.0`, so the same skew marks an OPTIONAL parameter REQUIRED from a
   SINGLE request — and that badge is written into the OpenAPI export, where it is a claim about the API
   rather than about this tool. Measured on a real corpus row at requestCount 1 / observedCount 5.
   THE SET IS KEYED ON THE STATS OBJECT, not on the name, because a query `sdk` and a body `sdk` are
   different parameters with different stats and must each count once; keying on the name would have made
   one of them invisible. Passing the set rather than a boolean keeps the rule in ONE place — the three
   call sites cannot each get it subtly wrong, which is how the query one and the path one would have
   diverged (a template naming `{id}` twice repeats exactly as a query name does).
   VALUE FREQUENCY AND NUMERIC RANGE STILL SEE EVERY OCCURRENCE, deliberately: `?a=1&a=2` genuinely
   observed two values and the enum and range facts are about VALUES, not about requests. Only the
   per-request count moved. */
function updateParamStats(stats, value, seenThisRequest) {
  if (!seenThisRequest || !seenThisRequest.has(stats)) {
    stats.observedCount++;
    if (seenThisRequest) seenThisRequest.add(stats);
  }

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

/* ECMAScript §21.4.1.32 "Date Time String Format" — CONFORMANCE, asked BEFORE the value is parsed.
   `new Date(v)` is real parsing and that is exactly why it cannot be the test on its own: §21.4.3.2
   "Date.parse ( string )" says, verbatim, "The function first attempts to parse the String according to
   the format described in Date Time String Format (21.4.1.32), including expanded years. If the String
   does not conform to that format the function may fall back to any implementation-specific heuristics
   or implementation-specific date formats." V8 takes that licence generously, so a comment reading
   "real parsing, not regex" was true and was not the point: the parser was real and deliberately lenient,
   and what it returned for a value that is not a date was a DATE.
   MEASURED on node v22: "ConfigCat-React/a-4.8.0" parses as 2000-04-08, "sdk-2.15.3" as 2003-02-15,
   "my-app-1.2" as 2001-01-02 — ordinary SDK version strings, which is what a `?sdk=` query parameter
   holds. The hint reaches `field.format` (lib/learn.js analyzeFormat call site) and `openapi-export.js`
   writes it out, so it stops being a claim about this tool and becomes `format: date-time` on somebody
   else's API. CLAUDE.md §@H: a value known only to satisfy a guess is INVENTED, never emitted as observed.
   THE SPLIT IS STRUCTURE HERE, BOUNDS AT THE PARSER, and both are needed. The grammar decides SHAPE — it
   is a production from the standard, not a guess about what a date looks like — and the real parse still
   decides VALIDITY, because the grammar admits combinations the standard calls out of bounds
   (DD is "01 to 31", so 2024-02-31 conforms and is not a day; HH is "00 to 24", so T24:30 conforms and is
   not a time). Running the real parser on a string already known to conform is RUN, DON'T MATCH with the
   fallback door shut.
   THE OLD YEAR WINDOW (1900 < y < 2200) IS GONE RATHER THAN KEPT. It was standing in for "is this really
   a date" against the lenient parser's output; with the grammar gate ahead of it the garbage it was
   filtering never arrives, and the only strings it still rejected were CONFORMING dates outside an
   arbitrary window. A guess that has stopped catching anything but true positives is not a safety net. */
function _allDigits(s, from, len) {
  if (from < 0 || from + len > s.length) return false;
  for (let i = from; i < from + len; i++) {
    const c = s.charCodeAt(i);
    if (c < 48 || c > 57) return false;
  }
  return true;
}
function _num2(s, from, len) { return Number(s.slice(from, from + len)); }

function _isDateTimeStringFormat(value) {
  // Shortest conforming form this accepts is YYYY-MM-DD (10). Not a heuristic — arithmetic on the
  // grammar below, which exists only to skip the scan for values that cannot possibly reach its end.
  if (value.length < 10) return false;

  let i;
  // YYYY: "four decimal digits from 0000 to 9999, or as an expanded year of '+' or '-' followed by six
  // decimal digits" (§21.4.1.32, §21.4.1.32.1 "Expanded Years").
  if (value[0] === "+" || value[0] === "-") {
    if (!_allDigits(value, 1, 6)) return false;
    i = 7;
  } else {
    if (!_allDigits(value, 0, 4)) return false;
    i = 4;
  }

  /* §21.4.1.32 also lists the date-only forms YYYY and YYYY-MM, and this DELIBERATELY requires the full
     YYYY-MM-DD. Two reasons, both stated so the narrowing is not read as an oversight. The hint is named
     `date-time` and is exported as an OpenAPI `format`, where date-time is an RFC 3339 instant and a bare
     year is not one. And accepting bare YYYY would type every four-digit numeric parameter — a year, a
     port, a page size — as date-time, because `analyzeFormat` returns the FIRST hint over its threshold
     and `createParamStats` lists "date-time" ahead of "integer", so date-time wins that tie outright.
     The predicate it replaces already required length >= 8, so neither form reached it before either:
     this is the same population, decided by a rule instead of by a length. */
  if (value[i] !== "-" || !_allDigits(value, i + 1, 2)) return false;
  const month = _num2(value, i + 1, 2);
  if (month < 1 || month > 12) return false;            // "01 (January) to 12 (December)"
  i += 3;
  if (value[i] !== "-" || !_allDigits(value, i + 1, 2)) return false;
  const day = _num2(value, i + 1, 2);
  if (day < 1 || day > 31) return false;                // "01 to 31"
  i += 3;
  if (i === value.length) return true;                  // date-only YYYY-MM-DD

  // "T" appears literally, to indicate the beginning of the time element.
  if (value[i] !== "T") return false;
  i += 1;
  if (!_allDigits(value, i, 2)) return false;
  if (_num2(value, i, 2) > 24) return false;            // HH "from 00 to 24"
  i += 2;
  if (value[i] !== ":" || !_allDigits(value, i + 1, 2)) return false;
  if (_num2(value, i + 1, 2) > 59) return false;        // mm "from 00 to 59"
  i += 3;
  if (value[i] === ":") {                               // optional :ss
    if (!_allDigits(value, i + 1, 2)) return false;
    if (_num2(value, i + 1, 2) > 59) return false;      // ss "from 00 to 59"
    i += 3;
    if (value[i] === ".") {                             // optional .sss
      if (!_allDigits(value, i + 1, 3)) return false;
      i += 4;
    }
  }
  if (i === value.length) return true;                  // no offset — a local-time form, still conforming

  // Z ::: "Z", or "+"/"-" followed by HH:mm. §21.4.1.33 "Time Zone Offset String Format" bounds that
  // hour at 23 (`Hour ::: 0 DecimalDigit | 1 DecimalDigit | 20 | 21 | 22 | 23`) and the minute at 59.
  if (value[i] === "Z") return i + 1 === value.length;
  if (value[i] !== "+" && value[i] !== "-") return false;
  if (!_allDigits(value, i + 1, 2) || _num2(value, i + 1, 2) > 23) return false;
  i += 3;
  if (value[i] !== ":" || !_allDigits(value, i + 1, 2)) return false;
  if (_num2(value, i + 1, 2) > 59) return false;
  return i + 3 === value.length;
}

function detectFormat(stats, value) {
  // date-time: conforms to §21.4.1.32's grammar AND the real parser accepts it. new Date(invalid)
  // returns Invalid Date (NaN getTime) instead of throwing, so isNaN is the validity test; no try needed.
  if (_isDateTimeStringFormat(value) && !isNaN(new Date(value).getTime())) {
    stats.formatHints["date-time"]++;
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
  /* ASSERTED WHERE BOTH ARE IN ONE HAND, which is the one check a reader of this quotient can make without
     re-deriving the whole mechanism. `observedCount` counts REQUESTS CONTAINING this parameter and
     `requestCount` counts REQUESTS, so the first can never exceed the second — and when it did, this
     function returned a confidence above 1 and marked the parameter REQUIRED. A stored record from before
     the counting fix can still carry the skew, and firing on it is correct: the datum is wrong, the badge
     derived from it is wrong, and the OpenAPI export writes it out as a claim about somebody's API. */
  DCHECK(stats.observedCount <= requestCount,
         'a parameter was observed in ' + stats.observedCount + ' request(s) out of ' + requestCount +
         ' — a subset cannot exceed the population it is drawn from. observedCount counts REQUESTS ' +
         'CONTAINING this parameter, so either it was raised more than once for one request (the ' +
         'occurrence-vs-request defect updateParamStats now prevents) or this record predates that fix ' +
         'and its `required` badge is derived from a ratio above 1');
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
  /* THIS TIER ASKS NO QUESTION ABOUT PROVENANCE AND THAT IS THE POINT, NOT AN OMISSION. It is the site the
     per-value grade was built for: this line picks the value the Send panel PREFILLS, so a value the engine
     reached only by forcing a gate arriving here is an example a server will reject, offered under a method
     the tool says the app's own code computes. It cannot arrive. lib/learn.js keeps such a value in
     `_astForcedValues` and lib/endpoint-record.js's `provenanceOffersExample` states why the line is a FIELD
     NAME rather than a grade each reader must remember to consult — this reader is the reason: nothing here
     could have asserted that it remembered, so the pool it reads is what makes the wrong answer impossible.
     If a later change gives this tier a grade to consult, the split has failed and the grade is the symptom. */
  /* AND A MEMBER THIS RECORD CANNOT STATE AS AN EXAMPLE IS NOT A CANDIDATE, which is `fdCanStateExample`'s
     whole subject: a `null` member returned here would be written onto the field as `_exampleValue: null`
     BESIDE the source below, and that pair says "nothing was computed" and "here is where it came from" at
     once. The tier does not fire, so the priority order continues to the declared schema — which is what an
     unstatable AST observation leaves: no AST example, and the next SOURCE down still gets its turn. */
  if (field && Array.isArray(field._astValidValues) && field._astValidValues.length &&
      fdCanStateExample(field._astValidValues[0])) {
    return { value: field._astValidValues[0], source: "ast-constraint" };
  }
  /* 4. enum — declared enum OR detected enum. THE COERCION IS NOT WHAT MAKES THIS SAFE, AND EXPECTING IT TO
     BE IS HOW THE NULL GOT THROUGH. `_coerceToFieldType` opens with `if (value == null) return value` — it
     declines to type an absence and hands it straight back — so a `null` member of a declared enum arrives at
     the caller UNCHANGED, as the record's own spelling of "nothing was computed", with `source: "enum"`
     beside it. The member is therefore asked about HERE, where the candidate is, and not downstream of a
     conversion that was never going to object to it. */
  if (field && Array.isArray(field.enum) && field.enum.length &&
      fdCanStateExample(field.enum[0])) {
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
