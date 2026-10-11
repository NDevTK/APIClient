// lib/request-identity.js — who a request the trusted zone sends speaks as. An identity is a document: its
// origin and URL, and its cookie partition key. identityFor turns an identity and a request into the
// facts a send applies (Origin, Referer, credentials, partition key); cookieHeaderFor computes the Cookie
// a document of that identity would send (RFC 6265bis §5.2 and §5.8.3). Loaded after lib/site.js and
// lib/referrer-policy.js, which it calls.

/* A frame the person picked, from browser-stamped facts only: webNavigation's record of the document and
   chrome.cookies' partition key for it, both looked up by the document's `documentId`. Resolves to null
   when the document is gone; a send then has no identity and is refused by its caller. */
async function frameIdentity(tabId, documentId) {
  DCHECK(typeof tabId === "number" && typeof documentId === "string" && documentId !== "",
         "frameIdentity needs the tab and the documentId the browser stamped on the observation");
  const frames = await swRpc("webNavigation.getAllFrames", { tabId });
  const f = (frames || []).find((x) => x.documentId === documentId);
  if (!f) return null;
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
  CHECK(typeof origin === "string" && URL.canParse(docUrl) && new URL(docUrl).origin === origin,
        "an engine identity's document URL " + JSON.stringify(docUrl) + " is not of its cluster origin " +
        JSON.stringify(origin) + " — the zone speaks as the origin it assigned, never as one a URL names");
  return { origin, docUrl, partitionKey: { topLevelSite: siteOf(docUrl), hasCrossSiteAncestor: false } };
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
  if (ri.referrer === "") referrer = null;
  else {
    const r = ri.referrer === null ? "client" : referrerForIdentity(ri.referrer, identity.origin);
    referrer = determineRequestReferrer(r, policy, identity.docUrl, request.url);
  }
  const status = cookieSiteStatus(identity, request.url);
  return { origin: identity.origin, referrer, referrerPolicy: policy, credentials, sameSite: status === "same-site",
           partitionKey: { topLevelSite: identity.partitionKey.topLevelSite, hasCrossSiteAncestor: status === "cross-site" } };
}

/* The cookies a document of this identity sends with `url`, as {wire, cookie}: `wire` is the credentials
   mode the zone's own fetch uses, and `cookie` a Cookie value the send sets, or null.
   - omit, and same-origin to another origin: nothing.
   - same-site: the browser's own jar, which is the page's: `wire` "include" and no Cookie set.
   - cross-site: RFC 6265bis §5.8.3 excludes every cookie whose same-site-flag is not "None" (the Lax
     exception is for navigations), so the unpartitioned SameSite=None cookies plus this partition's;
     `wire` "omit", so the extension's own jar adds nothing and a reply's Set-Cookie is not stored.
   Not covered: the creation-time order between the two lists (chrome.cookies states no creation time, so
   each list keeps its own order and the merge is by path length only), a same-site send's partitioned
   cookies, and the person's third-party-cookie setting. Their absence shows as a Cookie header whose
   order, or whose partitioned or unpartitioned members, differ from the page's own request. */
async function cookieHeaderFor(identityFacts, url) {
  const { credentials, sameSite, partitionKey, origin } = identityFacts;
  if (credentials === "omit" || (credentials === "same-origin" && new URL(url).origin !== origin))
    return { wire: "omit", cookie: null };
  if (sameSite) return { wire: "include", cookie: null };
  const unpartitioned = (await swRpc("cookies.getAll", { url })).filter((c) => c.sameSite === "no_restriction");
  const partitioned = await swRpc("cookies.getAll", { url, partitionKey });
  const list = unpartitioned.concat(partitioned).map((c, i) => ({ c, i }))
    .sort((a, b) => (b.c.path.length - a.c.path.length) || (a.i - b.i)).map((x) => x.c);
  if (list.length === 0) return { wire: "omit", cookie: null };
  return { wire: "omit", cookie: list.map((c) => (c.name !== "" ? c.name + "=" : "") + c.value).join("; ") };
}
