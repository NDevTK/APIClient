// speak-as-rule.js — the service worker's half of a zone send that speaks as a document: one
// declarativeNetRequest session rule per in-flight send, which sets the request headers the zone computed
// (lib/speak-as.js) on exactly that request. Only the service worker can call declarativeNetRequest, and
// background.js serves these arms to the offscreen document alone (`sender.url === OFFSCREEN_HREF`).

/* The rule family: session rule ids this file owns. The zone mints ids inside it from its own counter. */
const SPEAK_AS_RULE_MIN = 1000000;
const SPEAK_AS_RULE_MAX = 1999999;
const _speakAsInFamily = (id) => id >= SPEAK_AS_RULE_MIN && id <= SPEAK_AS_RULE_MAX;

/* declarativeNetRequest.RequestMethod: the eight methods it names, and "other" for every other verb. */
const _DNR_METHODS = ["connect", "delete", "get", "head", "options", "patch", "post", "put"];
function speakAsDnrMethod(method) {
  const m = method.toLowerCase();
  return _DNR_METHODS.indexOf(m) >= 0 ? m : "other";
}

/* The condition that matches this one request and no other: the extension's own initiator, no tab, the
   method, and the URL exactly. A URL holding urlFilter's own metacharacters (`*`, `^`, `|`) is matched by
   an anchored, escaped regexFilter instead. */
function _speakAsCondition(method, url) {
  const c = { tabIds: [chrome.tabs.TAB_ID_NONE], initiatorDomains: [chrome.runtime.id],
              requestMethods: [speakAsDnrMethod(method)], resourceTypes: ["xmlhttprequest", "other"],
              isUrlFilterCaseSensitive: true };
  if (/[*^|]/.test(url)) c.regexFilter = "^" + url.replace(/[\\^$.*+?()[\]{}|]/g, "\\$&") + "$";
  else c.urlFilter = "|" + url + "|";
  return c;
}

/* Installs the rules for one send: `o` is {url, origin, rules: [{ruleId, method, headers}]}, one rule per
   method the send puts on the wire (its own, and OPTIONS when it is preflighted). `headers` maps a request-
   header name to the value to set, or to null to remove it. The CHECKs are the boundary this capability
   stops at: the Origin set is the identity's own, and a Referer is of that origin or absent, so no send can
   name another site. Returns the family's rule ids now installed for this URL, whatever their method. */
async function speakAsInstall(o) {
  CHECK(typeof o.url === "string" && /^https?:/.test(o.url) && !o.url.includes("#"),
        "a speak-as rule names " + JSON.stringify(o.url) + ", which is not a fragment-less http(s) URL");
  CHECK(typeof o.origin === "string" && (o.origin === "null" || new URL(o.origin).origin === o.origin),
        "a speak-as rule names the identity origin " + JSON.stringify(o.origin) + ", which is not a serialized origin");
  const addRules = o.rules.map((r) => {
    CHECK(_speakAsInFamily(r.ruleId), "a speak-as rule id " + r.ruleId + " is outside the family");
    const origin = r.headers.Origin, referer = r.headers.Referer;
    CHECK(origin === undefined || origin === null || origin === o.origin || origin === "null",
          "a speak-as rule sets Origin " + JSON.stringify(origin) + " for the identity " + o.origin);
    CHECK(referer === undefined || referer === null || new URL(referer).origin === o.origin,
          "a speak-as rule sets a Referer " + JSON.stringify(referer) + " not of the identity " + o.origin);
    const requestHeaders = Object.keys(r.headers).map((h) => r.headers[h] === null
      ? { header: h, operation: "remove" } : { header: h, operation: "set", value: r.headers[h] });
    return { id: r.ruleId, priority: 1, condition: _speakAsCondition(r.method, o.url),
             action: { type: "modifyHeaders", requestHeaders } };
  });
  await chrome.declarativeNetRequest.updateSessionRules({ addRules });
  const want = _speakAsCondition("get", o.url);
  return (await chrome.declarativeNetRequest.getSessionRules())
    .filter((r) => _speakAsInFamily(r.id) && r.condition.urlFilter === want.urlFilter &&
                   r.condition.regexFilter === want.regexFilter)
    .map((r) => r.id);
}

async function speakAsRelease(ruleIds) {
  CHECK(ruleIds.every(_speakAsInFamily), "a speak-as rule id in " + JSON.stringify(ruleIds) + " is outside the family");
  await chrome.declarativeNetRequest.updateSessionRules({ removeRuleIds: ruleIds });
}

/* Session rules outlive the document that installed them, so the offscreen document removes every rule of
   the family when it starts, before its first send. Not at service-worker start: the worker is evicted when
   idle while a send in the offscreen document is still in flight, and clearing then would strip that
   send's rule mid-request. */
async function speakAsClearFamily() {
  const ids = (await chrome.declarativeNetRequest.getSessionRules()).map((r) => r.id).filter(_speakAsInFamily);
  if (ids.length) await chrome.declarativeNetRequest.updateSessionRules({ removeRuleIds: ids });
  return ids.length;
}
