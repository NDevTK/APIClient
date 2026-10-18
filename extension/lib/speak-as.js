// lib/speak-as.js — the zone's half of a send that speaks as a document: the request headers that make the
// zone's own fetch the request a document of that identity would send, and the lifetime of the
// declarativeNetRequest rules (speak-as-rule.js, in the service worker) that set them on that document's sends
// alone.

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


/* The documents this zone's sends speak as, by the identity's `docKey`: {token, next, keys}. `token` is the
   document's secret, minted at its first send that speaks as it; `keys` maps a send's (method, URL, identity
   origin, stated pair, header set) to the rule that sets those headers, {docKey, key, ruleId, scope, regex,
   inFlight, installed, ended}, whose `scope` `<token>.<k>` names it in the URL's fragment. Two header sets for
   one method and URL are two scopes, so no send waits for another. `_speakAsRules` holds every entry by rule
   id in the order of last use; an entry is `installed` once its update succeeded. The family's only writer
   after `speakAs.clearFamily` is this document, and one update runs at a time (`_speakAsFlush`).
   `_speakAsFamily` is that clear (speakAsStart), then its answer: the family's limits. */
const _speakAsDocs = new Map();
const _speakAsRules = new Map();
let _speakAsNextId = SPEAK_AS_RULE_MIN;
let _speakAsFamily = null;
let _speakAsAdds = [];
let _speakAsRemoves = [];
let _speakAsFlushing = false;

/* Session rules outlive the document that installed them, and a previous document's rules are removed only
   here, so this document clears the family when it starts, whether or not it ever sends. Every send waits
   for the clear and fails with its failure. Called once, when swRpc exists. */
function speakAsStart() {
  CHECK(_speakAsFamily === null, "the speak-as family was cleared twice by one document");
  _speakAsFamily = swRpc("speakAs.clearFamily");
  _speakAsFamily.catch((e) => console.error("[speak-as] the family could not be cleared:", e && e.message || e));
}

function _speakAsMintId() {
  CHECK(_speakAsRules.size <= SPEAK_AS_RULE_MAX - SPEAK_AS_RULE_MIN, "every id of the speak-as rule family is installed");
  for (;;) {
    const id = _speakAsNextId;
    _speakAsNextId = id === SPEAK_AS_RULE_MAX ? SPEAK_AS_RULE_MIN : id + 1;
    if (!_speakAsRules.has(id)) return id;
  }
}

function _speakAsToken() {
  const b = new Uint8Array(16);
  crypto.getRandomValues(b);
  return Array.from(b, (x) => x.toString(16).padStart(2, "0")).join("");
}

/* Forgets entry `e`, and removes its rule when one is installed. */
function _speakAsRetire(e) {
  if (_speakAsRules.get(e.ruleId) !== e) return;
  _speakAsRules.delete(e.ruleId);
  const doc = _speakAsDocs.get(e.docKey);
  if (doc !== undefined && doc.keys.get(e.key) === e) doc.keys.delete(e.key);
  if (e.installed) _speakAsRemoves.push(e.ruleId);
}

function _speakAsKick() {
  if (!_speakAsFlushing && (_speakAsAdds.length || _speakAsRemoves.length)) _speakAsFlush();
}

/* Sends the pending adds and removes in one speakAs.update at a time; the adds that arrive while one is in
   flight go in the next. Rules no send is running under are retired, least recently used first, while the
   family is over declarativeNetRequest's session limits. A failed update leaves what is installed unknown,
   so the record is reconciled with the worker's answer. */
async function _speakAsFlush() {
  _speakAsFlushing = true;
  while (_speakAsAdds.length || _speakAsRemoves.length) {
    const adds = _speakAsAdds;
    _speakAsAdds = [];
    const regexCount = () => [..._speakAsRules.values()].filter((e) => e.regex).length;
    for (const e of [..._speakAsRules.values()]) {
      const overRules = _speakAsRules.size > _speakAsFamily.rules, overRegex = regexCount() > _speakAsFamily.regex;
      if (!overRules && !overRegex) break;
      if (e.inFlight === 0 && (overRules || e.regex)) _speakAsRetire(e);
    }
    CHECK(_speakAsRules.size <= _speakAsFamily.rules && regexCount() <= _speakAsFamily.regex,
          "the speak-as rules exceed declarativeNetRequest's session limits (" + _speakAsFamily.rules + " rules, " +
          _speakAsFamily.regex + " regexFilter rules) with every one under a send in flight");
    const remove = _speakAsRemoves;
    _speakAsRemoves = [];
    let installed, failure = null;
    try {
      installed = await swRpc("speakAs.update", { add: adds.map((a) => a.rule), remove });
    } catch (err) {
      failure = err;
      installed = await swRpc("speakAs.installed");
    }
    const there = new Set(installed);
    for (const a of adds) a.entry.installed = there.has(a.entry.ruleId);
    if (failure !== null) {
      for (const e of [..._speakAsRules.values()]) if (e.installed && !there.has(e.ruleId)) e.installed = false;
      for (const id of installed)
        if ((!_speakAsRules.has(id) || !_speakAsRules.get(id).installed) && !_speakAsRemoves.includes(id))
          _speakAsRemoves.push(id);
      for (const a of adds) a.reject(failure);
      continue;
    }
    /* The worker's answer is this zone's installed record plus the rules retired while the update was in
       flight (a send released, a document ended), whose removal is queued for the next update. */
    const mine = [..._speakAsRules.values()].filter((e) => e.installed).map((e) => e.ruleId);
    CHECK(mine.every((id) => there.has(id)) &&
          installed.every((id) => mine.includes(id) || _speakAsRemoves.includes(id)),
          "the speak-as rules installed are " + JSON.stringify(installed) + ", not this document's record " +
          JSON.stringify(mine.sort((a, b) => a - b)) + " with the removals queued since the update, " +
          JSON.stringify(_speakAsRemoves) + " — a rule would set headers no send of this zone computed");
    for (const a of adds) a.resolve();
  }
  _speakAsFlushing = false;
}

/* The scope a send of `method` to the fragment-less `url` speaking as `speak` ({docKey, origin, stated,
   headers}) runs under, as {scope, ruleId, release}: the rule of its document and key, installed if it is
   not, and counted in flight until `release()`. */
async function speakAsAcquire(url, method, speak) {
  CHECK(typeof speak.docKey === "string" && speak.docKey !== "",
        "a send speaks as an identity with no document key — a rule is scoped to one document's sends");
  CHECK(_speakAsFamily !== null, "a send spoke as a document before this document cleared the speak-as family");
  _speakAsFamily = await _speakAsFamily;
  let doc = _speakAsDocs.get(speak.docKey);
  if (doc === undefined) {
    doc = { token: _speakAsToken(), next: 0, keys: new Map() };
    _speakAsDocs.set(speak.docKey, doc);
  }
  const key = JSON.stringify([method, url, speak.origin, speak.stated, speak.headers]);
  let e = doc.keys.get(key), installing = null;
  if (e === undefined) {
    e = { docKey: speak.docKey, key, ruleId: _speakAsMintId(), scope: doc.token + "." + doc.next++,
          regex: /[*^|]/.test(url), inFlight: 0, installed: false, ended: false };
    doc.keys.set(key, e);
    const entry = e;
    installing = new Promise((resolve, reject) => {
      _speakAsAdds.push({ entry, resolve, reject, rule: { ruleId: entry.ruleId, url, scope: entry.scope,
        origin: speak.origin, stated: speak.stated, method, headers: speak.headers } });
    });
  }
  _speakAsRules.delete(e.ruleId);
  _speakAsRules.set(e.ruleId, e);
  e.inFlight++;
  if (installing === null && !e.installed) installing = e.installing;
  if (installing !== null) {
    e.installing = installing;
    _speakAsKick();
    try {
      await installing;
    } catch (err) {
      e.inFlight--;
      _speakAsRetire(e);
      _speakAsKick();
      throw err;
    }
  }
  let released = false;
  return { scope: e.scope, ruleId: e.ruleId, release: () => {
    DCHECK(!released, "a speak-as scope was released twice");
    released = true;
    e.inFlight--;
    if (e.inFlight === 0 && e.ended) {
      _speakAsRetire(e);
      _speakAsKick();
    }
  } };
}

/* The document `docKey` ended (HTML §7.5.10 "Destroying documents", or its instance ended): the token of
   every scope it is part of (a frame's origin over another frame's jar is `<frame>+<jar>`) is forgotten, and
   their rules are removed, each once no send runs under it. */
function speakAsDocumentEnded(docKey) {
  for (const [k, doc] of [..._speakAsDocs]) {
    if (k.split("+").indexOf(docKey) < 0) continue;
    _speakAsDocs.delete(k);
    for (const e of [...doc.keys.values()]) {
      if (e.inFlight === 0) _speakAsRetire(e); else e.ended = true;
    }
  }
  _speakAsKick();
}
