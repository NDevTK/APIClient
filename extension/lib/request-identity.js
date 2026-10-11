// lib/request-identity.js — who a request the trusted zone sends speaks as. An identity is a document: its
// origin and URL, and its cookie partition key. identityFor turns an identity and a request into the
// facts a send applies (Origin, Referer, credentials, partition key); cookieHeaderFor computes the Cookie
// a document of that identity would send (RFC 6265bis §5.2 and §5.8.3), and cookieNamesFor and
// identityProducibleBy state the same facts for the operator's preview. Loaded after lib/site.js and
// lib/referrer-policy.js; identityProducibleBy also calls lib/speak-as.js, which loads after it.

/* A frame the person picked, from browser-stamped facts only: webNavigation's record of the document and
   chrome.cookies' partition key for it, both looked up by the document's `documentId`. Resolves to null
   when the document is gone; a send then has no identity and is refused by its caller. */
async function frameIdentity(tabId, documentId) {
  DCHECK(typeof tabId === "number" && typeof documentId === "string" && documentId !== "",
         "frameIdentity needs the tab and the documentId the browser stamped on the observation");
  const frames = await swRpc("webNavigation.getAllFrames", { tabId });
  const f = (frames || []).find((x) => x.documentId === documentId);
  return f ? frameIdentityOf(f) : null;
}

/* frameIdentity for one entry of webNavigation.getAllFrames' answer: null for a document that is not
   http(s), which no send speaks as. */
async function frameIdentityOf(f) {
  const documentId = f.documentId;
  const u = new URL(f.url);
  if (u.protocol !== "http:" && u.protocol !== "https:") return null;
  const { partitionKey } = await swRpc("cookies.getPartitionKey", { documentId });
  DCHECK(!!partitionKey && typeof partitionKey.topLevelSite === "string" &&
         typeof partitionKey.hasCrossSiteAncestor === "boolean",
         "chrome.cookies.getPartitionKey answered no {topLevelSite, hasCrossSiteAncestor} for an http(s) frame");
  return { origin: u.origin, docUrl: f.url, partitionKey };
}

/* An engine instance's document: the cluster origin this zone assigned at engineCreate and the URL it
   loaded. Not covered: a document nested in a cross-site one, whose partition key has a cross-site
   ancestor; the next diff passes the ancestor origins engineCreate already holds. Its absence shows as
   such a document's cross-site sends carrying no partitioned cookies. */
function engineIdentity(origin, docUrl) {
  /* An opaque cluster ("null", a sandboxed document) speaks as "null" whatever its URL; a document whose URL
     has no origin of its own (about:blank, data:) speaks as the origin it inherited. */
  CHECK(typeof origin === "string" && URL.canParse(docUrl) &&
        (origin === "null" || [origin, "null"].indexOf(new URL(docUrl).origin) >= 0),
        "an engine identity's document URL " + JSON.stringify(docUrl) + " is not of its cluster origin " +
        JSON.stringify(origin) + " — the zone speaks as the origin it assigned, never as one a URL names");
  const site = origin === "null" ? siteOf(docUrl) : siteOf(origin + "/");
  return { origin, docUrl, partitionKey: { topLevelSite: site, hasCrossSiteAncestor: false } };
}

/* RFC 6265bis §5.2 "Same-site" and "cross-site" Requests: same-site when the request URL's origin is same
   site with the client's "site for cookies", which §5.2.1 makes the top-level origin when every ancestor
   is same site with it and an opaque origin otherwise. chrome.cookies.getPartitionKey states both halves. */
function cookieSiteStatus(identity, url) {
  if (identity.partitionKey.hasCrossSiteAncestor) return "cross-site";
  return siteOf(url) === identity.partitionKey.topLevelSite ? "same-site" : "cross-site";
}

/* Fetch §2.2.5 "Requests"' credentials modes. Named apart from lib/safe-fetch.js's `_CREDENTIALS_MODES`:
   this document's classic scripts share one global scope, and a second `const` of one name is a
   SyntaxError that stops the later file from loading at all. */
const IDENTITY_CREDENTIALS_MODES = ["omit", "same-origin", "include"];

/* The facts a send speaking as `identity` applies to `request` ({url, requestInit}): the Origin (always
   the identity's own), the Referer by Referrer Policy §8.3, the credentials mode, and the partition key
   the request's cookies come from. `documentPolicy` is the identity's policy container's referrer policy,
   which Fetch §4.1 "Main fetch" uses when the request states "". */
function identityFor(identity, request, documentPolicy) {
  const ri = request.requestInit;
  const credentials = ri.credentials === null ? "same-origin" : ri.credentials;
  CHECK(IDENTITY_CREDENTIALS_MODES.indexOf(credentials) >= 0, "a request names the credentials mode " +
        JSON.stringify(credentials) + ", which Fetch §2.2.5 does not define");
  const policy = ri.referrerPolicy === null || ri.referrerPolicy === "" ? documentPolicy : ri.referrerPolicy;
  let referrer;
  // §8.3: a client whose origin is opaque has no referrer, and §5.4 makes every stated referrer "client" there.
  if (ri.referrer === "" || identity.origin === "null") referrer = null;
  else {
    const r = ri.referrer === null ? "client" : referrerForIdentity(ri.referrer, identity.origin);
    referrer = determineRequestReferrer(r, policy, identity.docUrl, request.url);
  }
  const status = cookieSiteStatus(identity, request.url);
  return { origin: identity.origin, referrer, referrerPolicy: policy, credentials, sameSite: status === "same-site",
           partitionKey: { topLevelSite: identity.partitionKey.topLevelSite, hasCrossSiteAncestor: status === "cross-site" } };
}

/* The identity an engine's page request speaks as on the wire, for both hosts (bridge.js, engine/trusted.mjs):
   the cluster origin the zone assigned the instance and the document URL it loaded, never a word the engine
   states. The referrer and its policy are the engine's, off the request record: `referrerPolicy` after Fetch
   §4.1 "Main fetch" gave an empty one the policy container's, and `referrer` either "no-referrer" or the URL
   the engine resolved "client" to. They are input from the untrusted half, so identityFor still makes a
   referrer that is not the identity's origin "client" (Fetch §5.4). An empty policy left after §4.1 is
   Referrer Policy §3's default. */
function pageRequestIdentity(origin, docUrl, url, credentials, mode, referrerPolicy, referrer) {
  CHECK(typeof referrerPolicy === "string" && (referrer === "no-referrer" || URL.canParse(referrer)),
        "a page request reached its identity with no referrer policy, or a referrer that is neither " +
        "\"no-referrer\" nor a URL — both hosts CHECK the engine's record before this");
  return identityFor(engineIdentity(origin, docUrl),
                     { url, requestInit: { credentials, mode, referrer: referrer === "no-referrer" ? "" : referrer,
                                           referrerPolicy } },
                     "strict-origin-when-cross-origin");
}

/* Whether a document of this identity sends cookies with `url`, as {arm, rule}: "none" for omit, or
   same-origin to another origin (Fetch §2.2.5 "Requests"), with `rule` saying which; otherwise "listed".
   The zone's own fetch omits credentials on every arm and a "listed" send sets the Cookie
   `_identityCookies` computes, so the extension's own jar, whose partition is the extension's and holds what
   the zone's earlier fetches stored, adds nothing, and a reply's Set-Cookie is not stored. */
function cookieArmFor(identityFacts, url) {
  const { credentials, origin } = identityFacts;
  if (credentials === "omit") return { arm: "none", rule: "the credentials mode is omit (Fetch §2.2.5 Requests)" };
  if (credentials === "same-origin" && new URL(url).origin !== origin)
    return { arm: "none", rule: "the credentials mode is same-origin and " + new URL(url).origin + " is not " +
                                origin + " (Fetch §2.2.5 Requests)" };
  return { arm: "listed", rule: null };
}

/* The cookies of the partition `partitionKey` for `url`. chrome.cookies states a partitioned cookie's key on
   the cookie, so each answer is asserted to be of the partition asked for. */
async function _partitionedCookies(url, partitionKey) {
  const list = await swRpc("cookies.getAll", { url, partitionKey });
  for (const c of list)
    DCHECK(!!c.partitionKey && c.partitionKey.topLevelSite === partitionKey.topLevelSite &&
           c.partitionKey.hasCrossSiteAncestor === partitionKey.hasCrossSiteAncestor,
           "chrome.cookies.getAll for the partition " + JSON.stringify(partitionKey) + " answered the cookie " +
           JSON.stringify(c.name) + " of the partition " + JSON.stringify(c.partitionKey) + " — a send that " +
           "joins this list to the unpartitioned one would carry an unpartitioned cookie twice");
  return list;
}

/* RFC 6265bis §5.8.3 "Retrieval Algorithm" for a request that is not a navigation: on a cross-site one
   (`sameSite` false) a cookie whose same-site-flag is not "None" is excluded, as the Lax exception is for
   navigations. So the unpartitioned cookies that survive plus the request's partition's, merged by path
   length, as {sent, excluded: [{name, rule}]}.
   Not covered: the creation-time order between the two lists (chrome.cookies states no creation time, so each
   list keeps its own order), and the person's third-party-cookie setting. Their absence shows as a Cookie
   header whose order or members differ from the page's own. */
async function _identityCookies(url, partitionKey, sameSite) {
  const all = await swRpc("cookies.getAll", { url });
  const kept = (c) => sameSite || c.sameSite === "no_restriction";
  const excluded = all.filter((c) => !kept(c)).map((c) => ({ name: c.name,
    rule: "SameSite " + c.sameSite + " on a cross-site request that is not a navigation (RFC 6265bis §5.8.3)" }));
  const sent = all.filter(kept).concat(await _partitionedCookies(url, partitionKey))
    .map((c, i) => ({ c, i })).sort((a, b) => (b.c.path.length - a.c.path.length) || (a.i - b.i)).map((x) => x.c);
  return { sent, excluded };
}

/* The cookies a document of this identity sends with `url`, as {cookie}: the Cookie value the send sets, or
   null. */
async function cookieHeaderFor(identityFacts, url) {
  if (cookieArmFor(identityFacts, url).arm === "none") return { cookie: null };
  const { sent } = await _identityCookies(url, identityFacts.partitionKey, identityFacts.sameSite);
  if (sent.length === 0) return { cookie: null };
  return { cookie: sent.map((c) => (c.name !== "" ? c.name + "=" : "") + c.value).join("; ") };
}

/* The cookie names, never values, a send as this identity carries, as {arm, names, excluded: [{name, rule}]},
   from what cookieHeaderFor reads. */
async function cookieNamesFor(identityFacts, url) {
  const arm = cookieArmFor(identityFacts, url);
  const pk = identityFacts.partitionKey;
  if (arm.arm === "listed") {
    const { sent, excluded } = await _identityCookies(url, pk, identityFacts.sameSite);
    return { arm: arm.arm, names: sent.map((c) => c.name), excluded };
  }
  const all = (await swRpc("cookies.getAll", { url })).concat(await _partitionedCookies(url, pk));
  return { arm: arm.arm, names: [], excluded: all.map((c) => ({ name: c.name, rule: arm.rule })) };
}

/* Whether a real frame could send this request: one speaking as `speaking` (the Origin the request carries as
   its own, stated or the frame's) from the frame `identity`, whose partition key is the jar (the frame's own,
   `frameKey`, or one the person stated), with `facts` (identityFor's answer) and the `sent` {origin, referer}
   headers (an origin of null is none sent). Answers {producible, by, reasons, jarHeld}: `reasons` names each
   header no document of `speaking` under that top-level site sends, and `jarHeld` whether the picked frame
   holds the jar itself.
   - Jar: RFC 6265bis §5.2.1 "Document-based requests" makes a document's site for cookies opaque when it or
     an ancestor is not same site with the top-level origin; the key's hasCrossSiteAncestor states that.
   - Origin and Sec-Fetch-Site: speakAsOrigin's and speakAsFetchSite's answers for a document of `speaking`.
   - Referer: Fetch §5.4 "Request class" makes a referrer of another origin "client". */
function identityProducibleBy(identity, frameKey, facts, speaking, sent, method, mode, url) {
  const pk = identity.partitionKey;
  const jarHeld = pk.topLevelSite === frameKey.topLevelSite && pk.hasCrossSiteAncestor === frameKey.hasCrossSiteAncestor;
  const reasons = [];
  const speakingSite = speaking === "null" ? null : siteOf(speaking + "/");
  if (!pk.hasCrossSiteAncestor && speakingSite !== null && speakingSite !== pk.topLevelSite)
    reasons.push("the jar is " + pk.topLevelSite + " with no cross-site ancestor, which no document of " +
                 speaking + " holds: that document is itself cross-site with the top-level site");
  const own = speakAsOrigin(speaking, method, mode, url, facts.referrerPolicy);
  if (sent.origin !== own)
    reasons.push("a document of " + speaking + " sends " + (own === null ? "no Origin" : "Origin " + own) +
                 " on this request (Fetch §3.2 Origin header)");
  const frameSite = speakAsFetchSite(identity.origin, url), speakingFetchSite = speakAsFetchSite(speaking, url);
  if (urlPotentiallyTrustworthy(new URL(url)) && frameSite !== speakingFetchSite)
    reasons.push("Sec-Fetch-Site stays the frame's " + frameSite + ", and a document of " + speaking + " sends " +
                 speakingFetchSite);
  if (sent.referer !== null && (speaking === "null" || new URL(sent.referer).origin !== speaking))
    reasons.push("a document of " + speaking + " sends no Referer of another origin (Fetch §5.4 Request class)");
  const where = !pk.hasCrossSiteAncestor ? "" :
    speakingSite === pk.topLevelSite ? " nested inside a cross-site frame" : " embedded";
  return { producible: reasons.length === 0, reasons, jarHeld,
           by: "a document of " + speaking + where + " under the top-level site " + pk.topLevelSite };
}
