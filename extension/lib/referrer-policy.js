// lib/referrer-policy.js — the `Referer` a request sent by the trusted zone carries when it speaks as a
// document: Referrer Policy (W3C Editor's Draft) §8.3 "Determine request's Referrer" and §8.4 "Strip url for
// use as a referrer", over Secure Contexts §3.2 "Is url potentially trustworthy?". The inputs are the zone's
// own: the identity's document, and the request facts intercept.js recorded or the operator chose.

/* Referrer Policy §3 "Referrer Policies": the empty string means the policy container's, which Fetch §4.1
   "Main fetch" substitutes before calling §8.3, so it is not in this list. */
const REFERRER_POLICIES = ["no-referrer", "no-referrer-when-downgrade", "same-origin", "origin", "strict-origin",
                           "origin-when-cross-origin", "strict-origin-when-cross-origin", "unsafe-url"];

/* Secure Contexts §3.1 "Is origin potentially trustworthy?" and §3.2 for a URL. `chrome-extension:` is the one
   scheme this user agent considers authenticated a priori (§7.1 Packaged Applications). */
function _urlPotentiallyTrustworthy(u) {
  if (u.href === "about:blank" || u.href === "about:srcdoc" || u.protocol === "data:") return true;
  if (u.origin === "null") return false;
  if (u.protocol === "https:" || u.protocol === "wss:" || u.protocol === "file:" ||
      u.protocol === "chrome-extension:") return true;
  const h = u.hostname;
  if (/^127\.\d+\.\d+\.\d+$/.test(h) || h === "[::1]") return true;
  return h === "localhost" || h === "localhost." || h.endsWith(".localhost") || h.endsWith(".localhost.");
}

/* §8.4: a local scheme (Fetch §2 "about", "blob", "data") has no referrer; otherwise the URL without its
   credentials and fragment, and with the origin-only flag also without its path and query. */
function _stripForReferrer(u, originOnly) {
  if (u.protocol === "about:" || u.protocol === "blob:" || u.protocol === "data:") return null;
  const s = new URL(u.href);
  s.username = "";
  s.password = "";
  s.hash = "";
  if (originOnly) { s.pathname = "/"; s.search = ""; }
  return s;
}

/* Fetch §5.4 "Request class", init["referrer"]: a URL that is `about:client` or not same origin with the
   identity's origin becomes "client". The zone applies it for the identity it chose, which need not be the
   document the referrer was observed in. Returns "client" or the absolute URL. */
function referrerForIdentity(referrer, identityOrigin) {
  DCHECK(typeof referrer === "string" && referrer !== "" && URL.canParse(referrer),
         "referrerForIdentity was given " + JSON.stringify(referrer) + " — intercept.js records an absolute " +
         "URL or about:client, and \"\" (no-referrer) is handled before a referrer is computed");
  const parsed = new URL(referrer);
  if (parsed.href === "about:client" || parsed.origin === "null" || parsed.origin !== identityOrigin)
    return "client";
  return parsed.href;
}

/* §8.3 for a request whose client is a Window: `referrer` is "client" or a URL string, `policy` one of
   REFERRER_POLICIES, `clientUrl` the identity's document URL, `currentUrl` the request's URL. Returns the
   `Referer` value, or null for no referrer.
   Not covered: an iframe srcdoc document, whose referrer source is its container's document URL; the zone
   holds no container chain. Building it means the identity carrying its nearest non-srcdoc ancestor's URL;
   its absence shows as no Referer on a send as a srcdoc frame, where the frame's own fetch sends its
   embedder's URL. */
function determineRequestReferrer(referrer, policy, clientUrl, currentUrl) {
  DCHECK(REFERRER_POLICIES.indexOf(policy) >= 0,
         "determineRequestReferrer was given the policy " + JSON.stringify(policy) + " — Fetch §4.1 replaces " +
         "\"\" with the policy container's before this algorithm, and nothing else is a policy");
  DCHECK(URL.canParse(clientUrl) && URL.canParse(currentUrl),
         "determineRequestReferrer needs the identity's document URL and the request URL, both absolute");
  const client = new URL(clientUrl);
  let source;
  if (referrer === "client") {
    if (client.origin === "null") return null;
    source = client;
  } else {
    DCHECK(URL.canParse(referrer), "determineRequestReferrer's referrer is neither \"client\" nor a URL");
    source = new URL(referrer);
  }
  let referrerURL = _stripForReferrer(source, false);
  const referrerOrigin = _stripForReferrer(source, true);
  if (referrerURL === null) return null;
  if (referrerURL.href.length > 4096) referrerURL = referrerOrigin;
  const current = new URL(currentUrl);
  const sameOrigin = referrerURL.origin === current.origin && referrerURL.origin !== "null";
  const downgrade = _urlPotentiallyTrustworthy(referrerURL) && !_urlPotentiallyTrustworthy(current);
  switch (policy) {
    case "no-referrer": return null;
    case "origin": return referrerOrigin.href;
    case "unsafe-url": return referrerURL.href;
    case "strict-origin": return downgrade ? null : referrerOrigin.href;
    case "strict-origin-when-cross-origin": return sameOrigin ? referrerURL.href : downgrade ? null : referrerOrigin.href;
    case "same-origin": return sameOrigin ? referrerURL.href : null;
    case "origin-when-cross-origin": return sameOrigin ? referrerURL.href : referrerOrigin.href;
    case "no-referrer-when-downgrade": return downgrade ? null : referrerURL.href;
    default: DFAIL("determineRequestReferrer reached a policy REFERRER_POLICIES does not list");
  }
}
