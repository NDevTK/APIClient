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

/* Fetch Metadata §2.3 "The Sec-Fetch-Site HTTP Request Header" over a one-URL url list: same-origin, else
   same-site, else cross-site, against the identity's origin. */
function speakAsFetchSite(origin, url) {
  if (origin === "null") return "cross-site";   // an opaque origin is same site with no URL's origin
  if (new URL(url).origin === origin) return "same-origin";
  return sameSite(origin + "/", url) ? "same-site" : "cross-site";
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
function speakAsHeaders(facts, method, mode, destination, url, cookie) {
  const h = { "Origin": speakAsOrigin(facts.origin, method, mode, url, facts.referrerPolicy),
              "Referer": facts.referrer };
  if (cookie.cookie !== null) h["Cookie"] = cookie.cookie;
  const trustworthy = urlPotentiallyTrustworthy(new URL(url));
  h["Sec-Fetch-Site"] = trustworthy ? speakAsFetchSite(facts.origin, url) : null;
  h["Sec-Fetch-Mode"] = trustworthy ? mode : null;
  h["Sec-Fetch-Dest"] = trustworthy ? (destination === "" ? "empty" : destination) : null;
  h["Sec-Fetch-Storage-Access"] = trustworthy && facts.credentials === "include" && !facts.sameSite ? "active" : null;
  return h;
}

/* Fetch §4.8 "CORS-preflight fetch": the OPTIONS request carries the request's origin, referrer and referrer
   policy, mode "cors" and no credentials, plus `Access-Control-Request-Method` and, when the request has
   CORS-unsafe header names, `Access-Control-Request-Headers`. Both are forbidden request-headers, so the
   rule sets them rather than the zone's fetch. `unsafeNames` is that list, already sorted and lowercased. */
function speakAsPreflightHeaders(facts, method, destination, url, unsafeNames) {
  const h = speakAsHeaders(Object.assign({}, facts, { credentials: "omit" }), "OPTIONS", "cors", destination, url,
                           { cookie: null });
  h["Origin"] = facts.origin;
  h["Access-Control-Request-Method"] = method;
  h["Access-Control-Request-Headers"] = unsafeNames.length ? unsafeNames.join(",") : null;
  // Chrome sends no User-Agent client hints on its own preflight; the zone's fetch would.
  h["Sec-CH-UA"] = null;
  h["Sec-CH-UA-Mobile"] = null;
  h["Sec-CH-UA-Platform"] = null;
  return h;
}

/* One send at a time per URL: a send's rules (its own method, and OPTIONS when it is preflighted) are
   installed and released as one unit, and the next send of the URL waits, so no other identity's rule can
   land between a preflight and its request. Ids come from this counter inside the worker's family. */
const _speakAsTails = new Map();
let _speakAsNextId = SPEAK_AS_RULE_MIN;
let _speakAsCleared = null;

function _speakAsMintId() {
  const id = _speakAsNextId;
  _speakAsNextId = _speakAsNextId === SPEAK_AS_RULE_MAX ? SPEAK_AS_RULE_MIN : _speakAsNextId + 1;
  return id;
}

/* Runs `send()` (the zone's fetches of `url`) under one rule per entry of `rules` ({method, headers}) for the
   identity `origin`, and removes them when the send settles. Rules a previous offscreen document left are
   removed before this document's first send. */
async function withSpeakAsRules(origin, url, rules, send) {
  if (_speakAsCleared === null) _speakAsCleared = swRpc("speakAs.clearFamily");
  await _speakAsCleared;
  const u = new URL(url);
  u.hash = "";
  const key = u.href;
  const prev = _speakAsTails.has(key) ? _speakAsTails.get(key) : Promise.resolve();
  let done;
  const mine = new Promise((r) => { done = r; });
  const tail = prev.then(() => mine);
  _speakAsTails.set(key, tail);
  await prev;
  const minted = rules.map((r) => ({ ruleId: _speakAsMintId(), method: r.method, headers: r.headers }));
  const ids = minted.map((r) => r.ruleId);
  try {
    const installed = await swRpc("speakAs.install", { url: key, origin, rules: minted });
    CHECK(installed.length === ids.length && ids.every((id) => installed.indexOf(id) >= 0),
          "the speak-as rules installed for " + key + " are " + JSON.stringify(installed) + ", not exactly " +
          JSON.stringify(ids) + " — another rule would set another identity's headers on this send");
    return await send();
  } finally {
    await swRpc("speakAs.release", ids);
    done();
    if (_speakAsTails.get(key) === tail) _speakAsTails.delete(key);
  }
}
