// lib/http-cache.js — the HTTP cache of Fetch §4.6 "HTTP-network-or-cache fetch": one cache per partition
// (§2.8 "HTTP cache partitions", keyed by §2.7 "Network partition keys"), storing, selecting and invalidating
// responses by RFC 9111 (HTTP Caching) as a private cache, so `private` permits storage and `s-maxage` is
// not read. lib/safe-fetch.js asks it once per hop, before the firing decision, since a hit sends nothing.
//
// Freshness is HTTP's clock (RFC 9110 §5.6.7 "Date/Time Formats"), read by the caller and passed in as
// milliseconds; it is not the scheduler's work currency and ranks nothing.
//
// Named residual. Not covered: cross-session persistence; the cache lives in memory for one zone session.
// Next diff: IndexedDB beside the frontier. Its absence shows as a repeat fetch of a still-fresh reply at the
// first send after a restart.

/* Each partition's cache by its key's serialization, as {partition, entries}: `entries` maps a target URI
   (no fragment) to its stored responses, oldest first. */
var _httpCaches = new Map();

/* Fetch §2.8 "HTTP cache partitions" for a request whose partition key is `partitionKey`
   (lib/request-identity.js's {topLevelSite, hasCrossSiteAncestor}), or null for a request with no client,
   whose key §2.7 makes null; §4.6 then sets the cache mode to "no-store".
   Named residual. Not covered: an opaque top-level site, whose `topLevelSite` is null for every opaque
   origin, so one value would name many partitions. Next diff: key it on the zone's minted opaque-origin
   token. Its absence shows as a repeat fetch of a fresh reply from a sandboxed top-level document. */
function httpCacheFor(partitionKey) {
  if (partitionKey === null || partitionKey === undefined) return null;
  DCHECK(typeof partitionKey === "object" && typeof partitionKey.hasCrossSiteAncestor === "boolean" &&
         (partitionKey.topLevelSite === null || typeof partitionKey.topLevelSite === "string"),
         "a request's partition key is not identityFor's {topLevelSite, hasCrossSiteAncestor}");
  if (partitionKey.topLevelSite === null) return null;
  var name = JSON.stringify([partitionKey.topLevelSite, partitionKey.hasCrossSiteAncestor]);
  var cache = _httpCaches.get(name);
  if (cache === undefined) {
    cache = { partition: { topLevelSite: partitionKey.topLevelSite,
                           hasCrossSiteAncestor: partitionKey.hasCrossSiteAncestor },
              entries: new Map() };
    _httpCaches.set(name, cache);
  }
  return cache;
}

/* The request's header fields as RFC 9111 §4.1 compares them: lowercased names, multiple lines combined with
   ", " (RFC 9110 §5.2 "Field Lines and Combined Field Value"). `pairs` is the caller's header list and
   `overrides` the speak-as rule's set ({name: value, or null for a removed field}), which replaces a field of
   the same name, as the rule does on the wire. */
function httpCacheRequestFields(pairs, overrides) {
  var fields = new Map();
  (pairs || []).forEach(function (p) {
    var k = p[0].toLowerCase(), v = p[1].trim();
    fields.set(k, fields.has(k) ? fields.get(k) + ", " + v : v);
  });
  Object.keys(overrides).forEach(function (n) {
    if (overrides[n] === null) fields.delete(n.toLowerCase());
    else fields.set(n.toLowerCase(), String(overrides[n]).trim());
  });
  return fields;
}

/* Fetch §4.6's cache mode for a request stated `requested`: "If httpRequest's cache mode is "default" and
   httpRequest's header list contains `If-Modified-Since`, `If-None-Match`, `If-Unmodified-Since`,
   `If-Match`, or `If-Range`, then set httpRequest's cache mode to "no-store"."
   Named residual. Not covered: a cache mode the page states (Fetch §2.2.5 "Requests"); every page request is
   "default". Next diff: the engine's fetch record states the RequestInit's `cache` and safeFetch reads it.
   Its absence shows as a hit for a page request that stated `cache: "no-store"` or `"reload"`. */
var _HTTP_CACHE_CONDITIONAL = ["if-modified-since", "if-none-match", "if-unmodified-since", "if-match", "if-range"];
function httpCacheMode(requested, fields) {
  DCHECK(requested === "default" || requested === "no-store",
         "a request reached the cache with the mode " + JSON.stringify(requested) + " — this file implements " +
         "\"default\" and \"no-store\", and no caller states another");
  if (requested === "default" && _HTTP_CACHE_CONDITIONAL.some(function (n) { return fields.has(n); }))
    return "no-store";
  return requested;
}
/* The header fields Fetch §4.6 appends for the mode: "If httpRequest's cache mode is "no-store" or "reload",
   then" `Pragma: no-cache` and `Cache-Control: no-cache`, each only where the list holds none. */
function httpCacheModeHeaders(mode, fields) {
  if (mode !== "no-store") return [];
  var out = [];
  if (!fields.has("pragma")) out.push(["Pragma", "no-cache"]);
  if (!fields.has("cache-control")) out.push(["Cache-Control", "no-cache"]);
  return out;
}

/* RFC 9111 §5.2 "Cache-Control": `#cache-directive`, each `token [ "=" ( token / quoted-string ) ]`, names
   compared case-insensitively. Answers a Map of name to argument (null for none), keeping the first
   occurrence (RFC 9111 §4.2.1 "Calculating Freshness Lifetime": "either the first occurrence should be
   used or the response should be considered stale"). A quoted-string's quoted-pairs are unescaped (RFC 9110 §5.6.4 "Quoted Strings"). */
function _httpCacheDirectives(value) {
  var out = new Map();
  if (value === undefined || value === null) return out;
  var s = String(value), i = 0;
  while (i < s.length) {
    while (i < s.length && (s[i] === "," || s[i] === " " || s[i] === "\t")) i++;
    var start = i;
    while (i < s.length && s[i] !== "=" && s[i] !== ",") i++;
    var name = s.slice(start, i).trim().toLowerCase(), arg = null;
    if (s[i] === "=") {
      i++;
      if (s[i] === "\"") {
        arg = "";
        for (i++; i < s.length && s[i] !== "\""; i++) arg += s[i] === "\\" && i + 1 < s.length ? s[++i] : s[i];
        i++;
        while (i < s.length && s[i] !== ",") i++;
      } else {
        var a = i;
        while (i < s.length && s[i] !== ",") i++;
        arg = s.slice(a, i).trim();
      }
    }
    if (name !== "" && !out.has(name)) out.set(name, arg);
  }
  return out;
}
/* RFC 9111 §1.2.2 "Delta Seconds": `1*DIGIT`, or NaN for anything else; a value past 2^31 is 2^31. */
function _httpCacheDeltaSeconds(arg) {
  if (arg === null || !/^[0-9]+$/.test(arg)) return NaN;
  return Math.min(Number(arg), 2147483648);
}
/* RFC 9110 §5.6.7 "Date/Time Formats": IMF-fixdate, the obsolete RFC 850 form and asctime, all UTC, as
   milliseconds, or NaN. RFC 9111 §4.2 matches them case-insensitively and treats a zone other than GMT as
   invalid; an RFC 850 year more than 50 years ahead is the most recent past year with those digits. */
var _HTTP_MONTHS = ["jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"];
function _httpDate(value, nowMs) {
  if (typeof value !== "string") return NaN;
  var v = value.trim().toLowerCase(), m, day, mon, year, hms;
  if ((m = /^(mon|tue|wed|thu|fri|sat|sun), (\d\d) ([a-z]{3}) (\d{4}) (\d\d):(\d\d):(\d\d) gmt$/.exec(v))) {
    day = +m[2]; mon = m[3]; year = +m[4]; hms = [+m[5], +m[6], +m[7]];
  } else if ((m = /^(monday|tuesday|wednesday|thursday|friday|saturday|sunday), (\d\d)-([a-z]{3})-(\d\d) (\d\d):(\d\d):(\d\d) gmt$/.exec(v))) {
    day = +m[2]; mon = m[3]; hms = [+m[5], +m[6], +m[7]];
    var nowYear = new Date(nowMs).getUTCFullYear();
    year = Math.floor(nowYear / 100) * 100 + +m[4];
    if (year > nowYear + 50) year -= 100;
  } else if ((m = /^(mon|tue|wed|thu|fri|sat|sun) ([a-z]{3}) (\d\d| \d) (\d\d):(\d\d):(\d\d) (\d{4})$/.exec(v))) {
    mon = m[2]; day = +m[3].trim(); hms = [+m[4], +m[5], +m[6]]; year = +m[7];
  } else return NaN;
  var month = _HTTP_MONTHS.indexOf(mon);
  if (month < 0 || day < 1 || day > 31 || hms[0] > 23 || hms[1] > 59 || hms[2] > 60) return NaN;
  return Date.UTC(year, month, day, hms[0], hms[1], hms[2]);
}

/* RFC 9110 §15.1 "Overview of Status Codes"' heuristically cacheable codes. */
var _HTTP_HEURISTICALLY_CACHEABLE = [200, 203, 204, 206, 300, 301, 308, 404, 405, 410, 414, 501];
/* The final statuses whose caching requirements this cache implements (RFC 9111 §3's "understood"): every one
   but 206, whose Range combining (§3.4) is not built, and 304, which only validation (§4.3) consumes. */
function _httpCacheUnderstood(status) { return status >= 200 && status !== 206 && status !== 304; }

/* RFC 9111 §3 "Storing Responses in Caches", for a private cache: a GET, a final status this cache
   understands, no `no-store` (unless `must-understand` with an understood status, §5.2.2.3), and one of
   `public`, `private`, `Expires`, `max-age`, or a heuristically cacheable status. A request's own
   `no-store` (§5.2.1.5) also forbids it. `headers` is the response's lowercased field map. */
function httpCacheStorable(method, status, headers, fields) {
  var cc = _httpCacheDirectives(headers["cache-control"]);
  if (method !== "GET" || !_httpCacheUnderstood(status)) return false;
  if (cc.has("no-store") && !cc.has("must-understand")) return false;
  if (_httpCacheDirectives(fields.get("cache-control")).has("no-store")) return false;
  return cc.has("public") || cc.has("private") || headers.expires !== undefined || cc.has("max-age") ||
         _HTTP_HEURISTICALLY_CACHEABLE.indexOf(status) >= 0;
}

/* RFC 9111 §4.2.1 "Calculating Freshness Lifetime" in seconds: `max-age`, else `Expires` minus `Date`
   (or the time received), else §4.2.2 "Calculating Heuristic Freshness" — a tenth of the time since
   `Last-Modified`, for a heuristically cacheable status or a `public` response, and zero otherwise. An
   invalid `max-age` is stale (§4.2.1), and an invalid `Expires` is in the past (§5.3 "Expires"). */
function _httpCacheLifetime(entry) {
  var cc = _httpCacheDirectives(entry.headers["cache-control"]);
  if (cc.has("max-age")) {
    var age = _httpCacheDeltaSeconds(cc.get("max-age"));
    return isNaN(age) ? 0 : age;
  }
  if (entry.headers.expires !== undefined) {
    var expires = _httpDate(entry.headers.expires, entry.responseTime);
    return isNaN(expires) ? 0 : Math.max(0, (expires - entry.dateValue) / 1000);
  }
  if (_HTTP_HEURISTICALLY_CACHEABLE.indexOf(entry.status) < 0 && !cc.has("public")) return 0;
  var lastModified = _httpDate(entry.headers["last-modified"], entry.responseTime);
  return isNaN(lastModified) ? 0 : Math.max(0, (entry.dateValue - lastModified) / 1000 / 10);
}
/* RFC 9111 §4.2.3 "Calculating Age": current_age = max(apparent_age, age_value + response_delay) +
   resident_time, in seconds, where `Age` is its first member if a non-negative integer (§5.1 "Age"). */
function _httpCacheCurrentAge(entry, nowMs) {
  var ageValue = _httpCacheDeltaSeconds(String(entry.headers.age === undefined ? "" : entry.headers.age)
    .split(",")[0].trim());
  var apparent = Math.max(0, (entry.responseTime - entry.dateValue) / 1000);
  var corrected = (isNaN(ageValue) ? 0 : ageValue) + (entry.responseTime - entry.requestTime) / 1000;
  return Math.max(apparent, corrected) + (nowMs - entry.responseTime) / 1000;
}
/* RFC 9111 §4.1 "Calculating Cache Keys with the Vary Header Field": every field `Vary` nominates matches the
   request that stored the entry, absent matching only absent; a `*` member never matches. */
function _httpCacheVaryMatches(entry, fields) {
  var vary = entry.headers.vary;
  if (vary === undefined) return true;
  return vary.split(",").map(function (n) { return n.trim().toLowerCase(); })
    .filter(function (n) { return n !== ""; })
    .every(function (n) { return n !== "*" && entry.fields.get(n) === fields.get(n); });
}

/* RFC 9111 §4 "Constructing Responses from Caches" for a GET to `url` with the header fields `fields` at
   `nowMs`: the most recent stored response (by `Date`) whose Vary fields match, that has no `no-cache`, and
   that is fresh (§4.2: freshness_lifetime > current_age), within the request's own `max-age` and
   `min-fresh` (§5.2.1). Answers {entry, age} or null.
   Named residual. Not covered: §4.3 "Validation"; a stale or `no-cache` entry, and a request's `no-cache`,
   are a miss and a full refetch. Next diff: send `If-None-Match` and `If-Modified-Since` from the entry and
   freshen it on a 304 (§4.3.4). Its absence shows as a 200 refetched in full for a stored response that
   carried a validator. */
function httpCacheSelect(cache, url, method, fields, nowMs) {
  DCHECK(method === "GET", "a " + method + " asked the cache for a stored response — RFC 9111 §4 makes a cache " +
         "write through an unsafe method, and this cache stores GETs only");
  var req = _httpCacheDirectives(fields.get("cache-control"));
  if (req.has("no-cache")) return null;
  var list = cache.entries.get(_httpCacheTarget(url)) || [], best = null;
  list.forEach(function (entry) {
    if (!_httpCacheVaryMatches(entry, fields)) return;
    if (_httpCacheDirectives(entry.headers["cache-control"]).has("no-cache")) return;
    var age = _httpCacheCurrentAge(entry, nowMs), lifetime = _httpCacheLifetime(entry);
    if (!(lifetime > age)) return;
    var maxAge = _httpCacheDeltaSeconds(req.has("max-age") ? req.get("max-age") : null);
    if (!isNaN(maxAge) && age > maxAge) return;
    var minFresh = _httpCacheDeltaSeconds(req.has("min-fresh") ? req.get("min-fresh") : null);
    if (!isNaN(minFresh) && lifetime < age + minFresh) return;
    if (best === null || entry.dateValue >= best.entry.dateValue) best = { entry: entry, age: age };
  });
  return best;
}
/* Whether the cache holds any stored response for `url`'s target URI, so a caller composes the Vary fields
   only where a selection can find something. */
function httpCacheHolds(cache, url) { return cache.entries.has(_httpCacheTarget(url)); }
/* The Response a hit answers: the stored status, status message, header list and body, with `Age` replaced
   by the current age (RFC 9111 §4: "a cache MUST generate an Age header field"). The body is a fresh copy, so
   a reader of one hit cannot change another's. */
var _HTTP_NULL_BODY_STATUSES = [101, 103, 204, 205, 304];
function httpCacheResponse(hit) {
  var e = hit.entry, pairs = [];
  Object.keys(e.headers).forEach(function (k) { if (k !== "age") pairs.push([k, e.headers[k]]); });
  pairs.push(["age", String(Math.floor(hit.age))]);
  var body = _HTTP_NULL_BODY_STATUSES.indexOf(e.status) >= 0 ? null : e.body.slice();
  return new Response(body, { status: e.status, statusText: e.statusText, headers: pairs });
}

/* The target URI a stored response answers (RFC 9111 §4: "the presented target URI ... and that of the
   stored response match"); Fetch never sends a fragment. */
function _httpCacheTarget(url) {
  var u = new URL(url);
  u.hash = "";
  return u.href;
}
/* RFC 9110 §9.2.1 "Safe Methods". */
var _HTTP_SAFE_METHODS = ["GET", "HEAD", "OPTIONS", "TRACE"];

/* Fetch §4.6: "If httpRequest's method is unsafe and forwardResponse's status is in the range 200 to 399,
   inclusive, invalidate appropriate stored responses in httpCache" — RFC 9111 §4.4 "Invalidating Stored
   Responses"' target URI, in this partition. The step reads only the method and the status, so a request in
   the "no-store" mode invalidates too. */
function httpCacheInvalidate(cache, method, url, status) {
  if (_HTTP_SAFE_METHODS.indexOf(method) < 0 && status >= 200 && status <= 399)
    cache.entries.delete(_httpCacheTarget(url));
}
/* What Fetch §4.6 does with a response the network answered, for one hop: the invalidation step, then, unless
   the mode is "no-store" (§2.2.5 "Requests": "Fetch behaves as if there is no HTTP cache at all"), "Store
   httpRequest and forwardResponse in httpCache" when RFC 9111 §3 permits.
   `resp` is the wire's Response, still unread; a storable one is cloned and stored once its whole body has
   arrived, and a body that fails or is aborted first is incomplete and is not stored (§3.3 "Storing
   Incomplete Responses"). Set-Cookie is not stored: a reply's cookies are stored once, at the send that
   received it, and a hit is not a send. `fields` is the request's (httpCacheRequestFields) as it was sent. */
function httpCacheForwarded(cache, mode, partitionKey, url, method, fields, resp, requestTime, responseTime) {
  httpCacheInvalidate(cache, method, url, resp.status);
  if (mode === "no-store") return;
  var headers = {};
  resp.headers.forEach(function (v, k) {
    k = String(k).toLowerCase();
    if (k !== "set-cookie" && k !== "set-cookie2") headers[k] = v;
  });
  if (!httpCacheStorable(method, resp.status, headers, fields)) return;
  var date = _httpDate(headers.date, responseTime);
  var entry = { partition: { topLevelSite: partitionKey.topLevelSite,
                             hasCrossSiteAncestor: partitionKey.hasCrossSiteAncestor },
                method: method, fields: fields, status: resp.status, statusText: resp.statusText,
                headers: headers, body: null, requestTime: requestTime, responseTime: responseTime,
                dateValue: isNaN(date) ? responseTime : date };
  resp.clone().arrayBuffer().then(function (buf) {
    entry.body = new Uint8Array(buf);
    var target = _httpCacheTarget(url);
    /* A newer response for the same Vary fields supersedes the older (§4: "the most recent one"). */
    var kept = (cache.entries.get(target) || []).filter(function (old) { return !_httpCacheVaryMatches(old, fields); });
    kept.push(entry);
    cache.entries.set(target, kept);
  }, function (e) { RETHROW_FATAL(e); });
}

if (typeof self !== "undefined") {
  self.httpCacheFor = httpCacheFor;
  self.httpCacheRequestFields = httpCacheRequestFields;
  self.httpCacheMode = httpCacheMode;
  self.httpCacheModeHeaders = httpCacheModeHeaders;
  self.httpCacheStorable = httpCacheStorable;
  self.httpCacheHolds = httpCacheHolds;
  self.httpCacheSelect = httpCacheSelect;
  self.httpCacheResponse = httpCacheResponse;
  self.httpCacheInvalidate = httpCacheInvalidate;
  self.httpCacheForwarded = httpCacheForwarded;
}
