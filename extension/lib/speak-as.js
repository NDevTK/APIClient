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

/* A send's rules (its own method, and OPTIONS when it is preflighted) are installed and released as one unit;
   lib/safe-fetch.js sends one request per URL at a time (`_sendSerialized`), so no other send's rule can land
   between a preflight and its request. Ids come from this counter inside the worker's family. */
let _speakAsNextId = SPEAK_AS_RULE_MIN;
let _speakAsCleared = null;

function _speakAsMintId() {
  const id = _speakAsNextId;
  _speakAsNextId = _speakAsNextId === SPEAK_AS_RULE_MAX ? SPEAK_AS_RULE_MIN : _speakAsNextId + 1;
  return id;
}

/* Runs `send()` (the zone's fetches of `url`) under one rule per entry of `rules` ({method, headers}) for the
   identity `origin`, and removes them when the send settles. `stated` is the operator tier's person-typed
   {origin, referer}, or null; speak-as-rule.js admits an Origin or Referer outside the identity only as it.
   Rules a previous offscreen document left are removed before this document's first send. */
async function withSpeakAsRules(origin, url, rules, stated, send) {
  if (_speakAsCleared === null) _speakAsCleared = swRpc("speakAs.clearFamily");
  await _speakAsCleared;
  const u = new URL(url);
  u.hash = "";
  const key = u.href;
  const minted = rules.map((r) => ({ ruleId: _speakAsMintId(), method: r.method, headers: r.headers }));
  const ids = minted.map((r) => r.ruleId);
  try {
    const installed = await swRpc("speakAs.install", { url: key, origin, stated, rules: minted });
    CHECK(installed.length === ids.length && ids.every((id) => installed.indexOf(id) >= 0),
          "the speak-as rules installed for " + key + " are " + JSON.stringify(installed) + ", not exactly " +
          JSON.stringify(ids) + " — another rule would set another identity's headers on this send");
    return await send();
  } finally {
    await swRpc("speakAs.release", ids);
  }
}
