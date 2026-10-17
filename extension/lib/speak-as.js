// lib/speak-as.js — the zone's half of a send that speaks as a document: the request headers that make the
// zone's own fetch the request a document of that identity would send, and the lifetime of the one
// declarativeNetRequest rule (speak-as-rule.js, in the service worker) that sets them on that request alone.

/* Fetch §3.2 "`Origin` header", append a request `Origin` header, for a request whose origin is the
   identity's. Response tainting is Fetch §4.1 "Main fetch"'s: "basic" same-origin, "cors" for a cross-origin
   cors-mode request, "opaque" for no-cors. Returns the value, or null where Fetch appends none. */
function speakAsOrigin(origin, method, mode, url, policy) {
  const sameOrigin = new URL(url).origin === origin;
  if (!sameOrigin && mode === "cors") return origin;
  if (method === "GET" || method === "HEAD") return null;
  if (mode !== "cors") {
    if (policy === "no-referrer") return "null";
    if ((policy === "no-referrer-when-downgrade" || policy === "strict-origin" ||
         policy === "strict-origin-when-cross-origin") &&
        origin.startsWith("https:") && !url.startsWith("https:")) return "null";
    if (policy === "same-origin" && !sameOrigin) return "null";
  }
  return origin;
}

/* Fetch Metadata §2.3 "The Sec-Fetch-Site HTTP Request Header"'s set-site over a request's url list, against
   its origin: same-origin while every URL is, same-site while every URL is at least that, else cross-site. */
function speakAsFetchSite(origin, urlList) {
  if (origin === "null") return "cross-site";   // an opaque origin is same site with no URL's origin
  let value = "same-origin";
  for (const url of urlList) {
    if (new URL(url).origin === origin) continue;
    if (!sameSite(origin + "/", url)) return "cross-site";
    value = "same-site";
  }
  return value;
}

/* Fetch §2.2.5 "Requests"' redirect-taint of a request with origin `origin` and URL list `urlList`:
   "same-origin", "same-site" or "cross-site". Serializing the request origin answers "null" for any taint but
   "same-origin", which is the Origin a redirected request carries and the one its replies are CORS-checked
   against. */
function requestRedirectTaint(origin, urlList) {
  let lastURL = null, taint = "same-origin";
  for (const url of urlList) {
    if (lastURL === null) { lastURL = url; continue; }
    const last = new URL(lastURL).origin;
    if (!sameSite(url, lastURL) && !(origin !== "null" && sameSite(origin + "/", lastURL))) return "cross-site";
    if (new URL(url).origin !== last && origin !== last) taint = "same-site";
    lastURL = url;
  }
  return taint;
}

/* The request headers a send speaking as `facts` (identityFor's answer) sets, as {header name: value, or null
   to remove}: Origin, Referer, the Cookie cookieHeaderFor chose, Fetch Metadata §2.1-§2.3's Sec-Fetch-Dest,
   -Mode and -Site, and Storage Access Headers §4.1's Sec-Fetch-Storage-Access. Fetch Metadata sends these
   only to a potentially trustworthy URL. `mode` is the request's logical mode, which the zone's own fetch
   need not use. Names are spelled as Chrome spells them, because a rule that adds a header writes the name
   it is given.
   Not covered: the storage access status is taken as "active" for every credentialed cross-site send, which
   is what Chrome answers while third-party cookies are allowed. The next diff reads the person's setting.
   Its absence shows as `active` where Chrome sends `none` or `inactive`. */
function speakAsHeaders(facts, method, mode, destination, urlList, cookie) {
  const url = urlList[urlList.length - 1];
  const serialized = requestRedirectTaint(facts.origin, urlList) === "same-origin" ? facts.origin : "null";
  const h = { "Origin": speakAsOrigin(serialized, method, mode, url, facts.referrerPolicy),
              "Referer": facts.referrer };
  if (cookie.cookie !== null) h["Cookie"] = cookie.cookie;
  const trustworthy = urlPotentiallyTrustworthy(new URL(url));
  h["Sec-Fetch-Site"] = trustworthy ? speakAsFetchSite(facts.origin, urlList) : null;
  h["Sec-Fetch-Mode"] = trustworthy ? mode : null;
  h["Sec-Fetch-Dest"] = trustworthy ? (destination === "" ? "empty" : destination) : null;
  h["Sec-Fetch-Storage-Access"] = trustworthy && facts.credentials === "include" && !facts.sameSite ? "active" : null;
  return h;
}

/* Fetch §4.8 "CORS-preflight fetch": the OPTIONS request carries the request's origin, referrer and referrer
   policy, mode "cors", the request's URL list and no credentials, plus `Access-Control-Request-Method` and,
   when the request has CORS-unsafe header names, `Access-Control-Request-Headers`. Both are forbidden
   request-headers, so the rule sets them rather than the zone's fetch. `unsafeNames` is that list, already
   sorted and lowercased. */
function speakAsPreflightHeaders(facts, method, destination, urlList, unsafeNames) {
  const h = speakAsHeaders(Object.assign({}, facts, { credentials: "omit" }), "OPTIONS", "cors", destination,
                           urlList, { cookie: null });
  h["Origin"] = requestRedirectTaint(facts.origin, urlList) === "same-origin" ? facts.origin : "null";
  h["Access-Control-Request-Method"] = method;
  h["Access-Control-Request-Headers"] = unsafeNames.length ? unsafeNames.join(",") : null;
  // Chrome sends no User-Agent client hints on its own preflight; the zone's fetch would.
  h["Sec-CH-UA"] = null;
  h["Sec-CH-UA-Mobile"] = null;
  h["Sec-CH-UA-Platform"] = null;
  return h;
}

/* The rules this document has installed, by slot: a slot is a declarativeNetRequest method and a fragment-less
   URL (`get https://a.example/x`), which is all a rule condition tells apart, so a slot holds one rule and
   every send of that slot shares it or replaces it. An entry is {url, ruleId, key, regex, inFlight}: `key` is
   the identity origin, the stated pair, the method and the header set, so two sends with one key set the same
   headers; `inFlight` counts the sends running under it. The Map's order is the order of last use. The
   family's only writer after `speakAs.clearFamily` is this document, and one batch of settles runs at a time
   (`_speakAsFlush`), so the Map is what is installed. lib/safe-fetch.js sends one request per URL at a time
   (`_sendSerialized`), so a slot's rule is never replaced under a send in flight, and a batch holds each URL
   once. */
const _speakAsSlots = new Map();
const _speakAsIds = new Set();
let _speakAsNextId = SPEAK_AS_RULE_MIN;
let _speakAsFamily = null;
let _speakAsPending = [];
let _speakAsFlushing = false;

function _speakAsMintId() {
  CHECK(_speakAsIds.size <= SPEAK_AS_RULE_MAX - SPEAK_AS_RULE_MIN,
        "every id of the speak-as rule family is installed");
  for (;;) {
    const id = _speakAsNextId;
    _speakAsNextId = id === SPEAK_AS_RULE_MAX ? SPEAK_AS_RULE_MIN : id + 1;
    if (!_speakAsIds.has(id)) return id;
  }
}

function _speakAsSlot(method, url) {
  return speakAsDnrMethod(method) + " " + url;
}

/* The least recently used rule no send is running under, of a regexFilter rule only when `regexOnly`, as
   [slot, entry]; undefined when every one is in flight. */
function _speakAsLeastRecent(regexOnly) {
  for (const [slot, e] of _speakAsSlots)
    if (e.inFlight === 0 && (!regexOnly || e.regex)) return [slot, e];
  return undefined;
}

/* Makes the slots of `url` that `wanted` names ({method, headers, key}, headers null for a send that speaks as
   no document) hold exactly those rules. Settles that arrive while a batch's update is in flight join the
   next batch, so a burst of sends costs one updateSessionRules call per batch. Answers the entries the send
   runs under, each counted in flight. */
function _speakAsSettle(origin, url, wanted, stated) {
  return new Promise((resolve, reject) => {
    _speakAsPending.push({ origin, url, wanted, stated, resolve, reject });
    if (!_speakAsFlushing) _speakAsFlush();
  });
}

async function _speakAsFlush() {
  _speakAsFlushing = true;
  while (_speakAsPending.length) {
    const batch = _speakAsPending;
    _speakAsPending = [];
    let runs;
    try {
      runs = await _speakAsSettleBatch(batch);
    } catch (e) {
      // Every settle of the batch shared the update that failed.
      for (const b of batch) b.reject(e);
      continue;
    }
    batch.forEach((b, i) => b.resolve(runs[i]));
  }
  _speakAsFlushing = false;
}

/* One batch, in at most one speakAs.update: a slot whose key is unchanged keeps its rule, and only a new or
   changed key installs one. Rules nothing is running under are retired, least recently used first, while
   the family is over declarativeNetRequest's session limits. */
async function _speakAsSettleBatch(batch) {
  if (_speakAsFamily === null) _speakAsFamily = await swRpc("speakAs.clearFamily");
  const family = _speakAsFamily;
  const urls = batch.map((b) => b.url);
  DCHECK(new Set(urls).size === urls.length, "one batch of speak-as settles names a URL twice — " +
         "lib/safe-fetch.js's _sendSerialized runs one send per URL");
  const removeRuleIds = [], groups = [], runs = [];
  const retire = (slot, e) => {
    _speakAsSlots.delete(slot);
    _speakAsIds.delete(e.ruleId);
    removeRuleIds.push(e.ruleId);
  };
  for (const b of batch) {
    const add = [], running = [];
    for (const w of b.wanted) {
      const slot = _speakAsSlot(w.method, b.url), had = _speakAsSlots.get(slot);
      if (had !== undefined && w.headers !== null && had.key === w.key) {
        _speakAsSlots.delete(slot);
        _speakAsSlots.set(slot, had);
        running.push(had);
        continue;
      }
      if (had !== undefined) {
        DCHECK(had.inFlight === 0, "the speak-as rule of " + slot + " is replaced under a send in flight — " +
               "lib/safe-fetch.js's _sendSerialized runs one send per URL");
        retire(slot, had);
      }
      if (w.headers === null) continue;
      const e = { url: b.url, ruleId: _speakAsMintId(), key: w.key, regex: speakAsUsesRegex(b.url), inFlight: 0 };
      _speakAsSlots.set(slot, e);
      _speakAsIds.add(e.ruleId);
      add.push({ ruleId: e.ruleId, method: w.method, headers: w.headers });
      running.push(e);
    }
    for (const e of running) e.inFlight++;
    runs.push(running);
    if (add.length) groups.push({ url: b.url, origin: b.origin, stated: b.stated, rules: add });
  }
  for (let regexCount = [..._speakAsSlots.values()].filter((e) => e.regex).length; ; ) {
    const over = _speakAsSlots.size > family.rules ? _speakAsLeastRecent(false)
      : regexCount > family.regex ? _speakAsLeastRecent(true) : null;
    if (over === null) break;
    CHECK(over !== undefined, "the speak-as rules exceed declarativeNetRequest's session limits (" + family.rules +
          " rules, " + family.regex + " regexFilter rules) with every one under a send in flight");
    if (over[1].regex) regexCount--;
    retire(over[0], over[1]);
  }
  if (groups.length === 0 && removeRuleIds.length === 0) return runs;
  let installed;
  try {
    installed = await swRpc("speakAs.update", { groups, removeRuleIds, urls });
  } catch (e) {
    // What a failed update left installed is unknown, so the next batch clears the family and starts over.
    _speakAsSlots.clear();
    _speakAsIds.clear();
    _speakAsFamily = null;
    throw e;
  }
  for (const url of urls) {
    const mine = [..._speakAsSlots.values()].filter((e) => e.url === url).map((e) => e.ruleId);
    const there = installed[url];
    CHECK(Array.isArray(there) && there.length === mine.length && mine.every((id) => there.indexOf(id) >= 0),
          "the speak-as rules installed for " + url + " are " + JSON.stringify(there) + ", not exactly " +
          JSON.stringify(mine) + " — another rule would set another identity's headers on this send");
  }
  return runs;
}

function _speakAsUrl(url) {
  const u = new URL(url);
  u.hash = "";
  return u.href;
}

/* Runs `send()` (the zone's fetches of `url`) with one rule per entry of `rules` ({method, headers}) for the
   identity `origin` installed, which stay installed for the next send of the same key. `stated` is the
   operator tier's person-typed {origin, referer}, or null; speak-as-rule.js admits an Origin or Referer
   outside the identity only as it. Rules a previous offscreen document left are removed before this
   document's first send. */
async function withSpeakAsRules(origin, url, rules, stated, send) {
  const wanted = rules.map((r) => ({ method: r.method, headers: r.headers,
                                     key: JSON.stringify([origin, stated, r.method, r.headers]) }));
  const running = await _speakAsSettle(origin, _speakAsUrl(url), wanted, stated);
  try {
    return await send();
  } finally {
    for (const e of running) e.inFlight--;
  }
}

/* Runs `send()`, a request of `method` to `url` that speaks as no document, with no speak-as rule installed
   for its slot, so it carries none of a previous send's headers. */
async function withoutSpeakAsRules(url, method, send) {
  await _speakAsSettle(null, _speakAsUrl(url), [{ method, headers: null, key: null }], null);
  return send();
}
