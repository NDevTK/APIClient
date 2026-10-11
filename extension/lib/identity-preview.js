// lib/identity-preview.js — the record lib/send.js's operatorIdentityPreview answers the Send panel with: the
// identity an operator send speaks as, its Origin, Referer and cookie names; and operatorJarDomain's list of
// the frames whose jars and origins the panel offers. Loaded by ast-worker.html, which makes both, and
// popup.html, which renders them, so both edges assert the one shape.

/* Fetch §2.2.5 "Requests"' credentials modes, which the record's `credentials` is one of. */
const IDENTITY_PREVIEW_CREDENTIALS = ["omit", "same-origin", "include"];

/* The preview is {error} or the full record; a cookie entry carries a name and the rule that excluded it,
   never a value. `producibleBy` is null exactly when a stated value is refused. `who` names the edge. */
function checkIdentityPreview(p, who) {
  const str = (v) => typeof v === "string";
  const strOrNull = (v) => v === null || typeof v === "string";
  const key = (k) => !!k && str(k.topLevelSite) && typeof k.hasCrossSiteAncestor === "boolean";
  const pb = p ? p.producibleBy : null;
  DCHECK(!!p && (str(p.error) || (
           !!p.frame && str(p.frame.origin) && str(p.frame.docUrl) && key(p.frame.partitionKey) &&
           !!p.jar && key(p.jar.partitionKey) && ["frame", "operator"].indexOf(p.jar.source) >= 0 &&
           !!p.jar.holder && str(p.jar.holder.origin) && str(p.jar.holder.docUrl) &&
           str(p.method) && str(p.mode) &&
           strOrNull(p.origin) && strOrNull(p.referer) && str(p.policy) &&
           ["default", "request"].indexOf(p.policySource) >= 0 &&
           ["client", "none", "request"].indexOf(p.referrerSource) >= 0 &&
           !!p.sent && strOrNull(p.sent.origin) && strOrNull(p.sent.referer) && strOrNull(p.statedRefusal) &&
           IDENTITY_PREVIEW_CREDENTIALS.indexOf(p.credentials) >= 0 &&
           ["none", "listed"].indexOf(p.cookieArm) >= 0 &&
           Array.isArray(p.cookieNames) && p.cookieNames.every(str) && Array.isArray(p.excluded) &&
           p.excluded.every((e) => !!e && str(e.name) && str(e.rule) && Object.keys(e).length === 2) &&
           (pb === null) === (p.statedRefusal !== null) &&
           (pb === null || (typeof pb.producible === "boolean" && typeof pb.jarHeld === "boolean" &&
                            str(pb.by) && Array.isArray(pb.reasons) &&
                            pb.reasons.every(str) && pb.producible === (pb.reasons.length === 0))))),
         who + ": the identity preview is neither {error} nor the record operatorIdentityPreview " +
         "builds: " + JSON.stringify(p) + " — the Send panel would render facts no send computed");
}

/* operatorJarDomain's answer: one entry per open http(s) frame, its partition key the browser's. */
function checkJarDomain(list, who) {
  DCHECK(Array.isArray(list) && list.every((f) => !!f && Number.isInteger(f.tabId) &&
           typeof f.documentId === "string" && f.documentId !== "" && typeof f.origin === "string" &&
           typeof f.url === "string" && !!f.partitionKey && typeof f.partitionKey.topLevelSite === "string" &&
           typeof f.partitionKey.hasCrossSiteAncestor === "boolean"),
         who + ": the jar domain is not operatorJarDomain's list of {tabId, documentId, origin, url, " +
         "partitionKey}: " + JSON.stringify(list) + " — the panel would offer a jar no frame holds");
}
