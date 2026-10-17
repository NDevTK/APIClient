// speak-as-rule.js — the service worker's half of a zone send that speaks as a document: declarativeNetRequest
// session rules that set the request headers the zone computed (lib/speak-as.js), each scoped to one document
// by that document's secret token. A rule matches a method and a URL whose fragment begins `t=<token>.<k>.`,
// which only this worker's fetch of a send of that document carries (wire.js appends the send's own nonce);
// a server never sees a fragment (Fetch §4.6 "HTTP-network fetch" serializes the URL without it). Only the
// service worker can call declarativeNetRequest, and background.js serves these arms to the offscreen
// document alone (`sender.url === OFFSCREEN_HREF`).

/* The rule family: session rule ids this file owns. The zone mints ids inside it from its own counter. */
const SPEAK_AS_RULE_MIN = 1000000;
const SPEAK_AS_RULE_MAX = 1999999;
const _speakAsInFamily = (id) => id >= SPEAK_AS_RULE_MIN && id <= SPEAK_AS_RULE_MAX;

/* The request headers a rule may set or remove, lower-case: speakAsHeaders' and speakAsPreflightHeaders' (lib/
   speak-as.js). wire.js checks every no-tab request's final value of each against what the rule that matched
   it, or none, made of it. */
const SPEAK_AS_HEADER_NAMES = ["access-control-request-headers", "access-control-request-method", "cookie",
  "origin", "referer", "sec-ch-ua", "sec-ch-ua-mobile", "sec-ch-ua-platform", "sec-fetch-dest", "sec-fetch-mode",
  "sec-fetch-site", "sec-fetch-storage-access"];

/* A document's token and a rule's scope within it: 128 bits of hex, and `<token>.<k>`. */
const SPEAK_AS_TOKEN = /^[0-9a-f]{32}$/;
const SPEAK_AS_SCOPE = /^[0-9a-f]{32}\.[0-9]+$/;

/* declarativeNetRequest.RequestMethod: the eight methods it names, and "other" for every other verb. */
const _DNR_METHODS = ["connect", "delete", "get", "head", "options", "patch", "post", "put"];
function speakAsDnrMethod(method) {
  const m = method.toLowerCase();
  return _DNR_METHODS.indexOf(m) >= 0 ? m : "other";
}

/* The URL a rule of `scope` matches the beginning of: the send's fragment-less URL, then `#t=<scope>.`. */
function speakAsRulePrefix(url, scope) {
  return url + "#t=" + scope + ".";
}

/* The condition of a rule for `prefix`: this extension's initiator, no tab, the method, and a URL that begins
   with `prefix`, which pins the URL and the document's token. A left-anchored urlFilter, or an anchored,
   escaped regexFilter for a URL holding urlFilter's own metacharacters (`*`, `^`, `|`). */
function _speakAsCondition(method, prefix) {
  const c = { tabIds: [chrome.tabs.TAB_ID_NONE], initiatorDomains: [chrome.runtime.id],
              requestMethods: [speakAsDnrMethod(method)], resourceTypes: ["xmlhttprequest", "other"],
              isUrlFilterCaseSensitive: true };
  if (/[*^|]/.test(prefix)) c.regexFilter = "^" + prefix.replace(/[\\^$.*+?()[\]{}|]/g, "\\$&") + "[0-9a-f]{32}$";
  else c.urlFilter = "|" + prefix;
  return c;
}
/* Whether an installed rule's condition matches the request webRequest reported as `d`, over the conditions
   _speakAsCondition writes: the tab, the initiator's host (initiatorDomains matches a domain and its
   subdomains, and not a scheme), the resource type, the method, and the URL. */
function speakAsRuleMatches(rule, d) {
  const c = rule.condition;
  if (c.tabIds.indexOf(d.tabId) < 0 || c.resourceTypes.indexOf(d.type) < 0) return false;
  if (c.requestMethods.indexOf(speakAsDnrMethod(d.method)) < 0) return false;
  const host = typeof d.initiator === "string" && URL.canParse(d.initiator) ? new URL(d.initiator).hostname : null;
  if (host === null || !c.initiatorDomains.some((h) => host === h || host.endsWith("." + h))) return false;
  if (c.regexFilter !== undefined) return new RegExp(c.regexFilter).test(d.url);
  return d.url.startsWith(c.urlFilter.slice(1));
}

/* One rule's headers, checked: `o` is {url, scope, origin, stated, method, headers}. `headers` maps a
   request-header name to the value to set, or to null to remove it. The CHECKs are the boundary this
   capability stops at: the Origin set is the identity's own, and a Referer is of that origin or absent, so
   no send can name another site. The one exception is `stated` ({origin, referer}, each null or a value):
   the Origin and Referer a person typed into the Send panel, which only lib/safe-fetch.js's operator tier
   passes; every other send states null. Answers declarativeNetRequest's requestHeaders; a host with no
   declarativeNetRequest (engine/trusted.mjs) applies them to its own fetch. */
function speakAsCheckedHeaders(o) {
  CHECK(typeof o.url === "string" && /^https?:/.test(o.url) && !o.url.includes("#"),
        "a speak-as rule names " + JSON.stringify(o.url) + ", which is not a fragment-less http(s) URL — the " +
        "one fragment a send carries is its document's token");
  CHECK(typeof o.origin === "string" && (o.origin === "null" || new URL(o.origin).origin === o.origin),
        "a speak-as rule names the identity origin " + JSON.stringify(o.origin) + ", which is not a serialized origin");
  CHECK(o.stated === null || (!!o.stated && typeof o.stated === "object" &&
        ["origin", "referer"].every((k) => o.stated[k] === null || typeof o.stated[k] === "string")),
        "a speak-as rule states neither null nor the Send panel's {origin, referer} as `stated`");
  const statedOrigin = o.stated === null ? null : o.stated.origin;
  const statedReferer = o.stated === null ? null : o.stated.referer;
  const origin = o.headers.Origin, referer = o.headers.Referer;
  CHECK(origin === undefined || origin === null || origin === o.origin || origin === "null" ||
        (statedOrigin !== null && origin === statedOrigin),
        "a speak-as rule sets Origin " + JSON.stringify(origin) + " for the identity " + o.origin +
        ", and it is not the Origin the person stated in the Send panel (`stated.origin`)");
  CHECK(referer === undefined || referer === null || new URL(referer).origin === o.origin ||
        (statedReferer !== null && referer === statedReferer),
        "a speak-as rule sets a Referer " + JSON.stringify(referer) + " not of the identity " + o.origin +
        ", and it is not the Referer the person stated in the Send panel (`stated.referer`)");
  return Object.keys(o.headers).map((h) => {
    CHECK(SPEAK_AS_HEADER_NAMES.indexOf(h.toLowerCase()) >= 0,
          "a speak-as rule names the header " + h + ", which is not one of SPEAK_AS_HEADER_NAMES");
    return o.headers[h] === null ? { header: h, operation: "remove" } : { header: h, operation: "set", value: o.headers[h] };
  });
}

/* The family's installed rules as getSessionRules answers them, kept by every update this worker makes, so
   wire.js's header CHECK reads them synchronously. Session rules outlive a worker, so a new worker reads
   them first (speakAsInstalledReady, which wire.js starts at the worker's start). */
let speakAsInstalled = [];
let _speakAsInstalledP = null;
function speakAsInstalledReady() {
  if (_speakAsInstalledP === null) _speakAsInstalledP = speakAsInstalledIds();
  return _speakAsInstalledP;
}

async function speakAsInstalledIds() {
  speakAsInstalled = (await chrome.declarativeNetRequest.getSessionRules()).filter((r) => _speakAsInFamily(r.id));
  return speakAsInstalled.map((r) => r.id).sort((a, b) => a - b);
}

/* One update of the family, in one updateSessionRules call: removes `o.remove` and installs one rule for each
   `{ruleId, url, scope, origin, stated, method, headers}` of `o.add`. Answers the family's ids afterwards,
   sorted. */
async function speakAsUpdate(o) {
  await speakAsInstalledReady();
  CHECK(Array.isArray(o.remove) && o.remove.every(_speakAsInFamily) && Array.isArray(o.add) &&
        (o.add.length > 0 || o.remove.length > 0),
        "a speak-as update is not a non-empty list of rules to add and of the family's ids to remove");
  const addRules = o.add.map((a) => {
    CHECK(_speakAsInFamily(a.ruleId), "a speak-as rule id " + a.ruleId + " is outside the family");
    CHECK(typeof a.scope === "string" && SPEAK_AS_SCOPE.test(a.scope),
          "a speak-as rule's scope " + JSON.stringify(a.scope) + " is not a document token and an index");
    return { id: a.ruleId, priority: 1, condition: _speakAsCondition(a.method, speakAsRulePrefix(a.url, a.scope)),
             action: { type: "modifyHeaders", requestHeaders: speakAsCheckedHeaders(a) } };
  });
  await chrome.declarativeNetRequest.updateSessionRules({ removeRuleIds: o.remove, addRules });
  return speakAsInstalledIds();
}

/* Session rules outlive the document that installed them, so the offscreen document removes every rule of
   the family when it starts, before its first send. Answers {cleared, rules, regex}: how many it removed,
   and how many rules and regexFilter rules the family may hold, which are declarativeNetRequest's session
   limits (a modifyHeaders rule is an unsafe one) less the rules outside the family. */
async function speakAsClearFamily() {
  await speakAsInstalledReady();
  const dnr = chrome.declarativeNetRequest;
  CHECK([dnr.MAX_NUMBER_OF_SESSION_RULES, dnr.MAX_NUMBER_OF_UNSAFE_SESSION_RULES, dnr.MAX_NUMBER_OF_REGEX_RULES]
          .every((n) => Number.isInteger(n) && n > 0),
        "declarativeNetRequest states no session, unsafe-session or regex rule limit");
  const all = await dnr.getSessionRules();
  const ids = all.map((r) => r.id).filter(_speakAsInFamily);
  if (ids.length) await dnr.updateSessionRules({ removeRuleIds: ids });
  await speakAsInstalledIds();
  const others = all.filter((r) => !_speakAsInFamily(r.id));
  return { cleared: ids.length,
           rules: Math.min(dnr.MAX_NUMBER_OF_SESSION_RULES, dnr.MAX_NUMBER_OF_UNSAFE_SESSION_RULES) - others.length,
           regex: dnr.MAX_NUMBER_OF_REGEX_RULES - others.filter((r) => r.condition.regexFilter !== undefined).length };
}
