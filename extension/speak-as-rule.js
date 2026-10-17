// speak-as-rule.js — the service worker's half of a zone send that speaks as a document: declarativeNetRequest
// session rules, one per (method, URL) the zone has sent to as a document, each setting the request headers
// the zone computed (lib/speak-as.js) for that method and URL. Only the service worker can call
// declarativeNetRequest, and background.js serves these arms to the offscreen document alone
// (`sender.url === OFFSCREEN_HREF`).

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

/* Whether the rule for `url` is a regexFilter rule, which declarativeNetRequest counts against a limit of its
   own. */
function speakAsUsesRegex(url) {
  return /[*^|]/.test(url);
}

/* The condition that matches this extension's requests of one method to one URL: the extension's own
   initiator, no tab, the method, and the URL exactly. A URL holding urlFilter's own metacharacters (`*`, `^`,
   `|`) is matched by an anchored, escaped regexFilter instead. */
function _speakAsCondition(method, url) {
  const c = { tabIds: [chrome.tabs.TAB_ID_NONE], initiatorDomains: [chrome.runtime.id],
              requestMethods: [speakAsDnrMethod(method)], resourceTypes: ["xmlhttprequest", "other"],
              isUrlFilterCaseSensitive: true };
  if (speakAsUsesRegex(url)) c.regexFilter = "^" + url.replace(/[\\^$.*+?()[\]{}|]/g, "\\$&") + "$";
  else c.urlFilter = "|" + url + "|";
  return c;
}

/* The rules for one send, checked: `o` is {url, origin, stated, rules: [{ruleId, method, headers}]}, one rule
   per method the send puts on the wire (its own, and OPTIONS when it is preflighted). `headers` maps a
   request-header name to the value to set, or to null to remove it. The CHECKs are the boundary this
   capability stops at: the Origin set is the identity's own, and a Referer is of that origin or absent, so
   no send can name another site. The one exception is `stated` ({origin, referer}, each null or a value):
   the Origin and Referer a person typed into the Send panel, which only lib/safe-fetch.js's operator tier
   passes; every other send states null. Answers [{ruleId, method, requestHeaders}] in declarativeNetRequest's
   header vocabulary; a host with no declarativeNetRequest (engine/trusted.mjs) applies it to its own fetch. */
function speakAsCheckedRules(o) {
  CHECK(typeof o.url === "string" && /^https?:/.test(o.url) && !o.url.includes("#"),
        "a speak-as rule names " + JSON.stringify(o.url) + ", which is not a fragment-less http(s) URL");
  CHECK(typeof o.origin === "string" && (o.origin === "null" || new URL(o.origin).origin === o.origin),
        "a speak-as rule names the identity origin " + JSON.stringify(o.origin) + ", which is not a serialized origin");
  CHECK(o.stated === null || (!!o.stated && typeof o.stated === "object" &&
        ["origin", "referer"].every((k) => o.stated[k] === null || typeof o.stated[k] === "string")),
        "a speak-as rule states neither null nor the Send panel's {origin, referer} as `stated`");
  const statedOrigin = o.stated === null ? null : o.stated.origin;
  const statedReferer = o.stated === null ? null : o.stated.referer;
  return o.rules.map((r) => {
    CHECK(_speakAsInFamily(r.ruleId), "a speak-as rule id " + r.ruleId + " is outside the family");
    const origin = r.headers.Origin, referer = r.headers.Referer;
    CHECK(origin === undefined || origin === null || origin === o.origin || origin === "null" ||
          (statedOrigin !== null && origin === statedOrigin),
          "a speak-as rule sets Origin " + JSON.stringify(origin) + " for the identity " + o.origin +
          ", and it is not the Origin the person stated in the Send panel (`stated.origin`)");
    CHECK(referer === undefined || referer === null || new URL(referer).origin === o.origin ||
          (statedReferer !== null && referer === statedReferer),
          "a speak-as rule sets a Referer " + JSON.stringify(referer) + " not of the identity " + o.origin +
          ", and it is not the Referer the person stated in the Send panel (`stated.referer`)");
    const requestHeaders = Object.keys(r.headers).map((h) => r.headers[h] === null
      ? { header: h, operation: "remove" } : { header: h, operation: "set", value: r.headers[h] });
    return { ruleId: r.ruleId, method: r.method, requestHeaders };
  });
}

/* One update of the family, in one updateSessionRules call: removes `o.removeRuleIds` (rules the zone
   retires) and installs `speakAsCheckedRules(g)` for each `g` of `o.groups` (speakAsCheckedRules's argument,
   one URL each). Answers, for each URL of `o.urls`, the family's rule ids installed for it afterwards,
   whatever their method, as {url: [id]}. */
async function speakAsUpdate(o) {
  CHECK(Array.isArray(o.removeRuleIds) && o.removeRuleIds.every(_speakAsInFamily),
        "a speak-as update removes " + JSON.stringify(o.removeRuleIds) + ", which is not a list of the family's ids");
  CHECK(Array.isArray(o.groups) && Array.isArray(o.urls) && o.groups.every((g) => o.urls.indexOf(g.url) >= 0),
        "a speak-as update installs rules for a URL it does not answer for");
  const addRules = o.groups.flatMap((g) => speakAsCheckedRules(g).map((r) => ({ id: r.ruleId, priority: 1,
    condition: _speakAsCondition(r.method, g.url),
    action: { type: "modifyHeaders", requestHeaders: r.requestHeaders } })));
  CHECK(addRules.length || o.removeRuleIds.length, "a speak-as update installs and removes nothing");
  await chrome.declarativeNetRequest.updateSessionRules({ removeRuleIds: o.removeRuleIds, addRules });
  const family = (await chrome.declarativeNetRequest.getSessionRules()).filter((r) => _speakAsInFamily(r.id));
  const out = {};
  for (const url of o.urls) {
    const want = _speakAsCondition("get", url);
    out[url] = family.filter((r) => r.condition.urlFilter === want.urlFilter &&
                                    r.condition.regexFilter === want.regexFilter).map((r) => r.id);
  }
  return out;
}

/* Session rules outlive the document that installed them, so the offscreen document removes every rule of
   the family when it starts, before its first send. Not at service-worker start: the worker is evicted when
   idle while a send in the offscreen document is still in flight, and clearing then would strip that
   send's rule mid-request. Answers {cleared, rules, regex}: how many it removed, and how many rules and
   regexFilter rules the family may hold, which are declarativeNetRequest's session limits (a modifyHeaders
   rule is an unsafe one) less the rules outside the family. */
async function speakAsClearFamily() {
  const dnr = chrome.declarativeNetRequest;
  CHECK([dnr.MAX_NUMBER_OF_SESSION_RULES, dnr.MAX_NUMBER_OF_UNSAFE_SESSION_RULES, dnr.MAX_NUMBER_OF_REGEX_RULES]
          .every((n) => Number.isInteger(n) && n > 0),
        "declarativeNetRequest states no session, unsafe-session or regex rule limit");
  const all = await dnr.getSessionRules();
  const ids = all.map((r) => r.id).filter(_speakAsInFamily);
  if (ids.length) await dnr.updateSessionRules({ removeRuleIds: ids });
  const others = all.filter((r) => !_speakAsInFamily(r.id));
  return { cleared: ids.length,
           rules: Math.min(dnr.MAX_NUMBER_OF_SESSION_RULES, dnr.MAX_NUMBER_OF_UNSAFE_SESSION_RULES) - others.length,
           regex: dnr.MAX_NUMBER_OF_REGEX_RULES - others.filter((r) => r.condition.regexFilter !== undefined).length };
}
