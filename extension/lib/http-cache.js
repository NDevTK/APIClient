// lib/http-cache.js — the HTTP cache of Fetch §4.6 "HTTP-network-or-cache fetch": one cache per partition
// (§2.8 "HTTP cache partitions", keyed by §2.7 "Network partition keys"), storing, selecting and invalidating
// responses by RFC 9111 (HTTP Caching) as a private cache, so `private` permits storage and `s-maxage` is
// not read. lib/safe-fetch.js asks it once per hop, before the firing decision, since a hit sends nothing.
//
// Freshness is HTTP's clock (RFC 9110 §5.6.7 "Date/Time Formats"), read by the caller and passed in as
// milliseconds; it is not the scheduler's work currency and ranks nothing.
//
// The cache outlives a session where its host keeps a store: the host states it once (httpCacheStated),
// restores each record before the first selection (httpCacheRestore), and writes back what changed
// (httpCacheTakeDirty) — bridge.js into IndexedDB beside the frontier; a process host keeps nothing.

/* Each partition's cache by its key's serialization, as {partition, entries}: `entries` maps a target URI
   (no fragment) to its stored responses, oldest first. */
var _httpCaches = new Map();

/* Fetch §2.7 "Network partition keys" for a request speaking as `identity` (lib/request-identity.js's
   identityFor answer), as {topLevelSite, hasCrossSiteAncestor, opaque}, or null for a request with no client,
   whose key §2.7 makes null; §4.6 then sets the cache mode to "no-store". An opaque top-level origin is its
   own site (HTML §7.1.1.1 "Sites"), and `topLevelSite` is null for every one of them, so such a partition is
   told apart by the document it is: `opaque` is that document's key, and null for a tuple site. */
function httpCachePartitionOf(identity) {
  if (identity === null || identity === undefined) return null;
  var key = identity.partitionKey;
  DCHECK(!!key && typeof key === "object" && typeof key.hasCrossSiteAncestor === "boolean" &&
         (key.topLevelSite === null || typeof key.topLevelSite === "string"),
         "a request's partition key is not identityFor's {topLevelSite, hasCrossSiteAncestor}");
  DCHECK(key.topLevelSite !== null || (!!identity.client && typeof identity.client.docKey === "string"),
         "a request with an opaque top-level site speaks as no document, so its partition has no name");
  return { topLevelSite: key.topLevelSite, hasCrossSiteAncestor: key.hasCrossSiteAncestor,
           opaque: key.topLevelSite === null ? identity.client.docKey : null };
}
/* Fetch §2.8 "HTTP cache partitions": the cache of `partition` (httpCachePartitionOf's), or null for none. */
function httpCacheFor(partition) {
  CHECK(_httpCacheStating !== undefined,
        "the HTTP cache was asked for a partition before its host stated whether it keeps a store — a host " +
        "that persists the cache would otherwise answer this session's first requests without it");
  return partition === null ? null : _httpCacheNamed(partition);
}
function _httpCacheNamed(partition) {
  var opaque = partition.opaque === undefined ? null : partition.opaque;
  var name = JSON.stringify([partition.topLevelSite, partition.hasCrossSiteAncestor, opaque]);
  var cache = _httpCaches.get(name);
  if (cache === undefined) {
    cache = { name: name, partition: { topLevelSite: partition.topLevelSite,
                                       hasCrossSiteAncestor: partition.hasCrossSiteAncestor, opaque: opaque },
              entries: new Map() };
    _httpCaches.set(name, cache);
  }
  return cache;
}

/* What the host keeps between sessions. `_httpCacheStating` is undefined until the host states it, then the
   restore's promise until it settles, then null. `_httpCacheDirty` holds the [partition name, target URI]
   pairs whose stored responses changed since the host last took them, and `_httpCacheOnDirty` is the host's
   notice that one did. */
var _httpCacheStating = undefined, _httpCacheDirty = new Map(), _httpCacheOnDirty = null;
/* The host's one statement: `restoring` is a promise that settles once it has restored every record, or null
   for a host that keeps no store; `onDirty` is called (with no arguments) when a record changes, or null. */
function httpCacheStated(restoring, onDirty) {
  CHECK(_httpCacheStating === undefined, "the HTTP cache's host stated its store twice");
  CHECK((restoring === null || typeof restoring.then === "function") &&
        (onDirty === null || typeof onDirty === "function"),
        "the HTTP cache's host stated a store that is neither a restore promise nor null");
  _httpCacheOnDirty = onDirty;
  _httpCacheStating = restoring === null ? null : restoring.then(function () { _httpCacheStating = null; });
}
/* The restore a selection waits for, or null once there is none. */
function httpCacheRestoring() { return _httpCacheStating; }
/* An opaque partition is one document's, whose key names no document of a later session, so it is not kept. */
function _httpCacheMarkDirty(cache, target) {
  if (cache.partition.opaque !== null) return;
  _httpCacheDirty.set(JSON.stringify([cache.name, target]), { cache: cache, target: target });
  if (_httpCacheOnDirty !== null) _httpCacheOnDirty();
}
/* Every changed target URI's record, {key, partition, target, entries}, where `entries` is the whole stored
   list (empty once invalidated) in httpCacheRestore's shape; taking them clears the set. */
function httpCacheTakeDirty() {
  var out = [];
  _httpCacheDirty.forEach(function (d, key) {
    out.push({ key: key, partition: d.cache.partition, target: d.target,
               entries: (d.cache.entries.get(d.target) || []).map(function (e) {
                 return { method: e.method, fields: Array.from(e.fields), status: e.status,
                          statusText: e.statusText, headers: e.headers, body: e.body, requestTime: e.requestTime,
                          responseTime: e.responseTime, dateValue: e.dateValue };
               }) });
  });
  _httpCacheDirty.clear();
  return out;
}
/* Every stored response of every partition, gone, as the host's clear deletes all its data; what was dirty
   is dropped with it, since a write-back would re-store what the person deleted. */
function httpCacheClear() {
  _httpCaches.clear();
  _httpCacheDirty.clear();
}
/* One record a host kept, back into its partition, before any selection reads it. */
function httpCacheRestore(rec) {
  CHECK(_httpCacheStating !== undefined && _httpCacheStating !== null,
        "a cache record was restored outside its host's restore — after it, a selection may already have " +
        "answered without it");
  DCHECK(typeof rec.target === "string" && Array.isArray(rec.entries) && rec.entries.length > 0 &&
         rec.entries.every(function (e) { return ArrayBuffer.isView(e.body) && Array.isArray(e.fields); }),
         "a kept cache record is not httpCacheTakeDirty's {partition, target, entries} with each body's bytes");
  var cache = _httpCacheNamed(rec.partition);
  cache.entries.set(rec.target, rec.entries.map(function (e) {
    return { partition: cache.partition, method: e.method, fields: new Map(e.fields), status: e.status,
             statusText: e.statusText, headers: e.headers, body: e.body, requestTime: e.requestTime,
             responseTime: e.responseTime, dateValue: e.dateValue };
  }));
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

/* The stored responses RFC 9111 §4 could choose for a request with the header fields `fields`: the target
   URI's, whose Vary fields match (§4.1). */
function _httpCacheCandidates(cache, url, fields) {
  return (cache.entries.get(_httpCacheTarget(url)) || []).filter(function (e) {
    return _httpCacheVaryMatches(e, fields);
  });
}
/* RFC 9111 §4 "Constructing Responses from Caches" for a GET to `url` with the header fields `fields` at
   `nowMs`: the most recent candidate (by `Date`, §4). It answers the request, `fresh`, when it has no
   `no-cache`, is fresh (§4.2: freshness_lifetime > current_age) and is within the request's own `no-cache`,
   `max-age` and `min-fresh` (§5.2.1). Otherwise it is one Fetch §4.6 must validate ("If storedResponse is a
   stale response, then set the revalidatingFlag"), which needs a validator (§4.3.1), and one with none is no
   answer. Answers {entry, age, fresh} or null. */
function httpCacheSelect(cache, url, method, fields, nowMs) {
  DCHECK(method === "GET", "a " + method + " asked the cache for a stored response — RFC 9111 §4 makes a cache " +
         "write through an unsafe method, and this cache stores GETs only");
  var best = null;
  _httpCacheCandidates(cache, url, fields).forEach(function (e) {
    if (best === null || e.dateValue >= best.dateValue) best = e;
  });
  if (best === null) return null;
  var req = _httpCacheDirectives(fields.get("cache-control"));
  var age = _httpCacheCurrentAge(best, nowMs), lifetime = _httpCacheLifetime(best);
  var maxAge = _httpCacheDeltaSeconds(req.has("max-age") ? req.get("max-age") : null);
  var minFresh = _httpCacheDeltaSeconds(req.has("min-fresh") ? req.get("min-fresh") : null);
  var fresh = !req.has("no-cache") && !_httpCacheDirectives(best.headers["cache-control"]).has("no-cache") &&
              lifetime > age && !(age > maxAge) && !(lifetime < age + minFresh);
  if (!fresh && best.headers.etag === undefined && best.headers["last-modified"] === undefined) return null;
  return { entry: best, age: age, fresh: fresh };
}
/* RFC 9111 §4.3.1 "Sending a Validation Request" for one stored response: it "MUST send the relevant entity
   tags" (If-None-Match) and "SHOULD send the Last-Modified value (using If-Modified-Since)". Fetch §4.6
   appends them to its own copy of the request, after the CORS-preflight decision. */
function httpCacheValidators(entry) {
  var out = [];
  if (entry.headers.etag !== undefined) out.push(["If-None-Match", entry.headers.etag]);
  if (entry.headers["last-modified"] !== undefined) out.push(["If-Modified-Since", entry.headers["last-modified"]]);
  return out;
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
  var target = _httpCacheTarget(url);
  if (_HTTP_SAFE_METHODS.indexOf(method) < 0 && status >= 200 && status <= 399 && cache.entries.delete(target))
    _httpCacheMarkDirty(cache, target);
}
/* A response's header fields as this cache stores them: lowercased, with no Set-Cookie, since a reply's
   cookies are stored once, at the send that received it, and a hit is not a send. */
function _httpCacheStoredHeaders(resp) {
  var headers = {};
  resp.headers.forEach(function (v, k) {
    k = String(k).toLowerCase();
    if (k !== "set-cookie" && k !== "set-cookie2") headers[k] = v;
  });
  return headers;
}
/* RFC 9110 §8.8.3.2 "Comparison": strong comparison needs both tags strong and equal; weak comparison
   ignores the `W/` prefix. */
function _httpEtagWeak(tag) { return tag.indexOf("W/") === 0; }
function _httpEtagOpaque(tag) { return _httpEtagWeak(tag) ? tag.slice(2) : tag; }
/* RFC 9111 §4.3.4 "Freshening Stored Responses upon Validation" over the candidates `set`, for the 304's
   header fields `h`: those sharing one of its strong validators, else the most recent sharing a weak one
   (a weak entity tag or `Last-Modified`), else a lone candidate when neither carries a validator. Each one
   identified takes the 304's fields (§3.2 "Updating Stored Header Fields", except Content-Length) and is
   validated now. Answers how many were identified. */
function _httpCacheFreshen(set, h, requestTime, responseTime) {
  var tag = h.etag === undefined ? null : h.etag.trim(), chosen;
  if (tag !== null && !_httpEtagWeak(tag)) {
    chosen = set.filter(function (e) { return e.headers.etag !== undefined && !_httpEtagWeak(e.headers.etag) &&
                                              e.headers.etag.trim() === tag; });
  } else if (tag !== null || h["last-modified"] !== undefined) {
    var weak = set.filter(function (e) {
      return (tag !== null && e.headers.etag !== undefined &&
              _httpEtagOpaque(e.headers.etag.trim()) === _httpEtagOpaque(tag)) ||
             (h["last-modified"] !== undefined && e.headers["last-modified"] === h["last-modified"]);
    }).sort(function (a, b) { return b.dateValue - a.dateValue; });
    chosen = weak.slice(0, 1);
  } else {
    chosen = set.length === 1 && set[0].headers.etag === undefined && set[0].headers["last-modified"] === undefined
      ? set : [];
  }
  chosen.forEach(function (e) {
    Object.keys(h).forEach(function (k) { if (k !== "content-length") e.headers[k] = h[k]; });
    var date = _httpDate(e.headers.date, responseTime);
    e.requestTime = requestTime;
    e.responseTime = responseTime;
    e.dateValue = isNaN(date) ? responseTime : date;
  });
  return chosen.length;
}
/* What Fetch §4.6 does with a response the network answered, for one hop: the invalidation step; then, for a
   request sent to validate `validating` (httpCacheSelect's answer, or null), "If the revalidatingFlag is set
   and forwardResponse's status is 304", freshen the stored responses and answer the stored one (§4.3.3
   "Handling a Validation Response"); otherwise, unless the mode is "no-store" (§2.2.5 "Requests": "Fetch
   behaves as if there is no HTTP cache at all"), "Store httpRequest and forwardResponse in httpCache" when
   RFC 9111 §3 permits. Answers the Response the request reads in place of `resp`, or null for `resp` itself.
   `resp` is the wire's Response, still unread; a storable one is cloned and stored once its whole body has
   arrived, and a body that fails or is aborted first is incomplete and is not stored (§3.3 "Storing
   Incomplete Responses"). `fields` is the request's (httpCacheRequestFields) as it was sent. */
function httpCacheForwarded(cache, mode, url, method, fields, resp, requestTime, responseTime, validating) {
  httpCacheInvalidate(cache, method, url, resp.status);
  if (validating !== null && resp.status === 304) {
    if (_httpCacheFreshen(_httpCacheCandidates(cache, url, fields), _httpCacheStoredHeaders(resp), requestTime,
                          responseTime) > 0)
      _httpCacheMarkDirty(cache, _httpCacheTarget(url));
    return httpCacheResponse({ entry: validating.entry,
                               age: _httpCacheCurrentAge(validating.entry, responseTime) });
  }
  if (mode === "no-store") return null;
  var headers = _httpCacheStoredHeaders(resp);
  if (!httpCacheStorable(method, resp.status, headers, fields)) return null;
  var date = _httpDate(headers.date, responseTime);
  var entry = { partition: cache.partition, method: method, fields: fields, status: resp.status, statusText: resp.statusText,
                headers: headers, body: null, requestTime: requestTime, responseTime: responseTime,
                dateValue: isNaN(date) ? responseTime : date };
  resp.clone().arrayBuffer().then(function (buf) {
    entry.body = new Uint8Array(buf);
    var target = _httpCacheTarget(url);
    /* A newer response for the same Vary fields supersedes the older (§4: "the most recent one"). */
    var kept = (cache.entries.get(target) || []).filter(function (old) { return !_httpCacheVaryMatches(old, fields); });
    kept.push(entry);
    cache.entries.set(target, kept);
    _httpCacheMarkDirty(cache, target);
  }, function (e) { RETHROW_FATAL(e); });
  return null;
}

if (typeof self !== "undefined") {
  self.httpCacheFor = httpCacheFor;
  self.httpCacheStated = httpCacheStated;
  self.httpCacheRestoring = httpCacheRestoring;
  self.httpCacheTakeDirty = httpCacheTakeDirty;
  self.httpCacheRestore = httpCacheRestore;
  self.httpCacheClear = httpCacheClear;
  self.httpCacheRequestFields = httpCacheRequestFields;
  self.httpCacheMode = httpCacheMode;
  self.httpCacheModeHeaders = httpCacheModeHeaders;
  self.httpCacheStorable = httpCacheStorable;
  self.httpCacheHolds = httpCacheHolds;
  self.httpCacheSelect = httpCacheSelect;
  self.httpCacheValidators = httpCacheValidators;
  self.httpCacheResponse = httpCacheResponse;
  self.httpCacheInvalidate = httpCacheInvalidate;
  self.httpCacheForwarded = httpCacheForwarded;
}
