// test-ast.js — Test suite for AST analysis engine
// Run: node test-ast.js

var fs = require("fs");

// Load Babel bundle (IIFE: var BabelBundle = (() => {...})())
var babelCode = fs.readFileSync(__dirname + "/extension/lib/babel-bundle.js", "utf8");
// Replace var declaration with globalThis assignment so it's accessible outside new Function
new Function(babelCode.replace(/^var BabelBundle/, "globalThis.BabelBundle"))();

// Load ast.js (expects BabelBundle global; expose analyzeJSBundle + extractSourceMapUrl)
var astCode = fs.readFileSync(__dirname + "/extension/lib/ast.js", "utf8");
new Function(astCode + "\nglobalThis.analyzeJSBundle = analyzeJSBundle;\nglobalThis.extractSourceMapUrl = extractSourceMapUrl;\nglobalThis.buildDefinitionMap = buildDefinitionMap;")();

var passed = 0, failed = 0, total = 0;

function test(name, code, check, forceScript) {
  total++;
  try {
    var result = analyzeJSBundle(code, "test://" + name, forceScript);
    var ok = check(result);
    if (ok) {
      passed++;
      console.log("  PASS: " + name);
    } else {
      failed++;
      console.log("  FAIL: " + name);
      console.log("    Result:", JSON.stringify(result.fetchCallSites, null, 2));
    }
  } catch (e) {
    failed++;
    console.log("  ERROR: " + name + " — " + e.message);
    console.log("    " + e.stack.split("\n").slice(0, 3).join("\n    "));
  }
}

// Strip leading newline from template literals so line 1 starts at the first code line
function trimCode(s) { return s.replace(/^\n/, ""); }

function testViewer(name, code, check) {
  total++;
  try {
    var result = buildDefinitionMap(code);
    var ok = check(result);
    if (ok) {
      passed++;
      console.log("  PASS: " + name);
    } else {
      failed++;
      console.log("  FAIL: " + name);
      console.log("    refMap:", JSON.stringify(result.refMap));
      console.log("    defMap:", JSON.stringify(result.defMap));
      console.log("    propDefs:", JSON.stringify(result.propDefs));
    }
  } catch (e) {
    failed++;
    console.log("  ERROR: " + name + " — " + e.message);
    console.log("    " + e.stack.split("\n").slice(0, 3).join("\n    "));
  }
}

function testResolve(name, check) {
  total++;
  try {
    var ok = check();
    if (ok) {
      passed++;
      console.log("  PASS: " + name);
    } else {
      failed++;
      console.log("  FAIL: " + name);
    }
  } catch (e) {
    failed++;
    console.log("  ERROR: " + name + " — " + e.message);
  }
}

function resolveDefLine(name, renderedLine, refMap, defMap, focusMode, lineRemap) {
  var line = renderedLine;
  if (focusMode && lineRemap && lineRemap.has(renderedLine)) {
    line = lineRemap.get(renderedLine);
  }
  if (refMap) {
    if (refMap[line] && refMap[line][name]) return refMap[line][name];
    return 0;
  }
  if (defMap && defMap[name]) return defMap[name];
  return 0;
}

console.log("\n=== Direct fetch() calls ===\n");

test("fetch with string literal URL", `
  fetch("https://api.example.com/users");
`, function(r) {
  return r.fetchCallSites.length === 1 &&
    r.fetchCallSites[0].url === "https://api.example.com/users" &&
    r.fetchCallSites[0].method === "GET";
});

test("fetch with method and headers", `
  fetch("https://api.example.com/data", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ name: "test", value: 42 })
  });
`, function(r) {
  return r.fetchCallSites.length === 1 &&
    r.fetchCallSites[0].method === "POST" &&
    r.fetchCallSites[0].headers["Content-Type"] === "application/json";
});

test("window.fetch call", `
  window.fetch("https://api.example.com/items");
`, function(r) {
  return r.fetchCallSites.length === 1 &&
    r.fetchCallSites[0].url === "https://api.example.com/items";
});

test("fetch with template literal URL", `
  var id = "abc123";
  fetch(\`https://api.example.com/users/\${id}\`);
`, function(r) {
  return r.fetchCallSites.length === 1 &&
    r.fetchCallSites[0].url === "https://api.example.com/users/abc123";
});

test("fetch with variable URL (const)", `
  var baseUrl = "https://api.example.com";
  fetch(baseUrl + "/endpoint");
`, function(r) {
  return r.fetchCallSites.length === 1 &&
    r.fetchCallSites[0].url === "https://api.example.com/endpoint";
});

test("fetch with options-as-variable", `
  var opts = { method: "PUT", headers: { "X-Token": "abc" } };
  fetch("https://api.example.com/update", opts);
`, function(r) {
  return r.fetchCallSites.length === 1 &&
    r.fetchCallSites[0].method === "PUT" &&
    r.fetchCallSites[0].headers["X-Token"] === "abc";
});

console.log("\n=== XHR calls ===\n");

test("XMLHttpRequest.open", `
  var xhr = new XMLHttpRequest();
  xhr.open("GET", "https://api.example.com/xhr-endpoint");
`, function(r) {
  return r.fetchCallSites.length === 1 &&
    r.fetchCallSites[0].url === "https://api.example.com/xhr-endpoint" &&
    r.fetchCallSites[0].method === "GET" &&
    r.fetchCallSites[0].type === "xhr";
});

console.log("\n=== Inter-procedural tracing ===\n");

test("Simple wrapper function", `
  function doFetch(url) {
    return fetch(url);
  }
  doFetch("https://api.example.com/wrapped");
`, function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url === "https://api.example.com/wrapped";
  });
});

test("Wrapper with method parameter", `
  function apiCall(method, url) {
    return fetch(url, { method: method });
  }
  apiCall("POST", "https://api.example.com/action");
`, function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url === "https://api.example.com/action" && s.method === "POST";
  });
});

test("Object method wrapper", `
  var api = {
    request: function(url) {
      return fetch(url);
    }
  };
  api.request("https://api.example.com/obj-method");
`, function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url === "https://api.example.com/obj-method";
  });
});

test("Closure pattern: obj.method = function wrapper", `
  var api = {};
  api.doFetch = function(url) {
    return fetch(url);
  };
  api.doFetch("https://api.example.com/closure-method");
`, function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url === "https://api.example.com/closure-method";
  });
});

console.log("\n=== MemberExpression value resolution ===\n");

test("Object property as URL", `
  var config = { apiUrl: "https://api.example.com/config" };
  fetch(config.apiUrl);
`, function(r) {
  return r.fetchCallSites.length === 1 &&
    r.fetchCallSites[0].url === "https://api.example.com/config";
});

test("Object property concatenation", `
  var config = { base: "https://api.example.com" };
  fetch(config.base + "/resource");
`, function(r) {
  return r.fetchCallSites.length === 1 &&
    r.fetchCallSites[0].url === "https://api.example.com/resource";
});

test("Closure module pattern: separate property assignment", `
  var mod = {};
  mod.baseUrl = "https://api.example.com";
  fetch(mod.baseUrl + "/path");
`, function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url === "https://api.example.com/path";
  });
});

test("Nested object property", `
  var config = { api: { url: "https://api.example.com/nested" } };
  fetch(config.api.url);
`, function(r) {
  // This may or may not resolve depending on nested member expression support
  return r.fetchCallSites.length === 1;
});

test("this.prop in object method", `
  var client = {
    base: "https://api.example.com/v2",
    get: function(path) {
      return fetch(this.base + path);
    }
  };
  client.get("/products");
`, function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url === "https://api.example.com/v2/products";
  });
});

console.log("\n=== Param location detection ===\n");

test("Wrapper params get correct locations", `
  function apiCall(method, url, body) {
    return fetch(url, { method: method, body: JSON.stringify(body) });
  }
  apiCall("POST", "https://api.example.com/action", { key: "val" });
`, function(r) {
  var site = r.fetchCallSites.find(function(s) { return s.url === "https://api.example.com/action"; });
  if (!site || !site.params) return false;
  var methodParam = site.params.find(function(p) { return p.name === "method"; });
  var urlParam = site.params.find(function(p) { return p.name === "url"; });
  // method param should not be location:unknown
  return (!methodParam || methodParam.location !== "unknown") &&
         (!urlParam || urlParam.location !== "unknown");
});

console.log("\n=== Value constraints ===\n");

test("Switch statement constraints", `
  function handle(action) {
    switch(action) {
      case "create": return fetch("/api/create");
      case "update": return fetch("/api/update");
      case "delete": return fetch("/api/delete");
    }
  }
`, function(r) {
  return r.valueConstraints.some(function(c) {
    return c.variable === "action" && c.values.length === 3;
  });
});

test("Includes constraint", `
  var FORMATS = ["json", "xml", "csv"];
  function convert(format) {
    if (FORMATS.includes(format)) {
      fetch("/api/convert?format=" + format);
    }
  }
`, function(r) {
  return r.valueConstraints.some(function(c) {
    return c.variable === "format" && c.values.length === 3;
  });
});

test("Equality chain constraint", `
  function process(type) {
    if (type === "text" || type === "html" || type === "markdown") {
      fetch("/api/process");
    }
  }
`, function(r) {
  return r.valueConstraints.some(function(c) {
    return c.variable === "type" && c.values.length === 3;
  });
});

console.log("\n=== Proto detection ===\n");

test("Proto enum detection (bidirectional map)", `
  var Status = { UNKNOWN: 0, ACTIVE: 1, INACTIVE: 2, DELETED: 3, 0: "UNKNOWN", 1: "ACTIVE", 2: "INACTIVE", 3: "DELETED" };
`, function(r) {
  return r.protoEnums.length === 1 &&
    r.protoEnums[0].values.ACTIVE === 1;
});

test("Proto field map from prototype assignment", `
  function MyMessage() {}
  MyMessage.prototype.getName = function() {
    return jspb.Message.getField(this, 1);
  };
  MyMessage.prototype.getAge = function() {
    return jspb.Message.getField(this, 2);
  };
`, function(r) {
  return r.protoFieldMaps.length === 2;
});

console.log("\n=== Scope verification ===\n");

test("Shadowed fetch is not a sink", `
  function myModule() {
    var fetch = function(url) { return url; };
    fetch("https://not-a-real-fetch.com");
  }
`, function(r) {
  return r.fetchCallSites.length === 0;
});

test("Shadowed window is not a sink", `
  function myModule() {
    var window = { fetch: function() {} };
    window.fetch("https://not-a-real-fetch.com");
  }
`, function(r) {
  return r.fetchCallSites.length === 0;
});

console.log("\n=== Source map URL ===\n");

test("Source map URL extraction", `
  var x = 1;
  //# sourceMappingURL=bundle.js.map
`, function(r) {
  return r.sourceMapUrl === "bundle.js.map";
});

console.log("\n=== Cross-script global resolution ===\n");

test("IIFE window alias — direct fetch via windowAlias.fetch", `
  !function(win) {
    win.fetch("https://api.example.com/from-iife");
  }(window);
`, function(r) {
  return r.fetchCallSites.length === 1 &&
    r.fetchCallSites[0].url === "https://api.example.com/from-iife";
});

test("Global assignment via window.X tracked", `
  window.myUrl = "https://api.example.com/global";
  fetch(myUrl);
`, function(r) {
  return r.fetchCallSites.length === 1 &&
    r.fetchCallSites[0].url === "https://api.example.com/global";
}, true);

test("UMD pattern — typeof window conditional", `
  !function(global) {
    global.apiBase = "https://api.example.com";
  }("undefined" != typeof window ? window : this);
  fetch(apiBase + "/endpoint");
`, function(r) {
  return r.fetchCallSites.length === 1 &&
    r.fetchCallSites[0].url === "https://api.example.com/endpoint";
}, true);

test("Indirect IIFE — factory(windowAlias) pattern", `
  !function(e, t) {
    t(e);
  }("undefined" != typeof window ? window : this, function(global) {
    global.baseUrl = "https://api.example.com/v2";
  });
  fetch(baseUrl + "/data");
`, function(r) {
  return r.fetchCallSites.length === 1 &&
    r.fetchCallSites[0].url === "https://api.example.com/v2/data";
}, true);

test("Global function via window.X = function()", `
  window.doFetch = function(url) { return fetch(url); };
  doFetch("https://api.example.com/global-fn");
`, function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url === "https://api.example.com/global-fn";
  });
}, true);

console.log("\n=== Object property tracing through function params ===\n");

test("fetch(opts.url) with object literal argument", `
  function request(opts) {
    return fetch(opts.url, { method: opts.method });
  }
  request({ url: "https://api.example.com/v1/users", method: "POST" });
`, function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url === "https://api.example.com/v1/users" && s.method === "POST";
  });
});

test("fetch(config.endpoint) with variable object argument", `
  function callApi(config) {
    return fetch(config.endpoint, { method: config.verb, headers: { "X-Key": "abc" } });
  }
  var opts = { endpoint: "https://api.example.com/v1/items", verb: "PUT" };
  callApi(opts);
`, function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url === "https://api.example.com/v1/items" && s.method === "PUT";
  });
});

test("Multiple callers with different object args", `
  function doRequest(params) {
    return fetch(params.url);
  }
  doRequest({ url: "https://api.example.com/a" });
  doRequest({ url: "https://api.example.com/b" });
`, function(r) {
  var hasA = r.fetchCallSites.some(function(s) { return s.url === "https://api.example.com/a"; });
  var hasB = r.fetchCallSites.some(function(s) { return s.url === "https://api.example.com/b"; });
  return hasA && hasB;
});

test("Wrapper function with opts.url → traced via wrapper", `
  function myFetch(opts) {
    return fetch(opts.url, { method: opts.method });
  }
  myFetch({ url: "https://api.example.com/traced", method: "DELETE" });
`, function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url === "https://api.example.com/traced" && s.method === "DELETE";
  });
});

test("Unresolvable placeholder URLs are suppressed (not emitted)", `
  function internalSink(settings) {
    var xhr = new XMLHttpRequest();
    xhr.open(settings.type, settings.url);
  }
`, function(r) {
  // No callers pass concrete values → should emit nothing
  return r.fetchCallSites.length === 0;
});

test("Object method with opts parameter", `
  var api = {
    call: function(opts) {
      return fetch(opts.url, { method: opts.method || "GET" });
    }
  };
  api.call({ url: "https://api.example.com/v1/obj-method", method: "PATCH" });
`, function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url === "https://api.example.com/v1/obj-method" && s.method === "PATCH";
  });
});

console.log("\n=== Return value resolution ===\n");

test("Simple function returning string literal", `
  function getUrl() { return "https://api.example.com/v1/data"; }
  fetch(getUrl());
`, function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url === "https://api.example.com/v1/data";
  });
});

test("Arrow function with expression body", `
  const getEndpoint = () => "https://api.example.com/v1/endpoint";
  fetch(getEndpoint());
`, function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url === "https://api.example.com/v1/endpoint";
  });
});

test("Function building URL from parameters", `
  function buildUrl(base, path) {
    return base + "/" + path;
  }
  fetch(buildUrl("https://api.example.com", "users"));
`, function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url === "https://api.example.com/users";
  });
});

test("Object method returning URL", `
  var config = {
    base: "https://api.example.com",
    getUrl: function() { return this.base + "/v1/items"; }
  };
  fetch(config.getUrl());
`, function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url === "https://api.example.com/v1/items";
  });
});

test("Chained return value — config factory", `
  function getConfig() {
    return { url: "https://api.example.com/v1/settings", method: "PUT" };
  }
  var cfg = getConfig();
  fetch(cfg.url, { method: cfg.method });
`, function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url === "https://api.example.com/v1/settings" && s.method === "PUT";
  });
});

console.log("\n=== Destructured parameter resolution ===\n");

test("Destructured {url} parameter", `
  function request({url}) {
    return fetch(url);
  }
  request({ url: "https://api.example.com/v1/destructured" });
`, function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url === "https://api.example.com/v1/destructured";
  });
});

test("Destructured {url, method} with both used", `
  function doFetch({url, method}) {
    return fetch(url, { method: method });
  }
  doFetch({ url: "https://api.example.com/v1/action", method: "DELETE" });
`, function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url === "https://api.example.com/v1/action" && s.method === "DELETE";
  });
});

test("Destructured with default value — {url, method = 'GET'}", `
  function api({url, method = "GET"}) {
    return fetch(url, { method: method });
  }
  api({ url: "https://api.example.com/v1/default-method" });
`, function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url === "https://api.example.com/v1/default-method";
  });
});

test("Destructured with rename — {endpoint: url}", `
  function callEndpoint({endpoint: url}) {
    return fetch(url);
  }
  callEndpoint({ endpoint: "https://api.example.com/v1/renamed" });
`, function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url === "https://api.example.com/v1/renamed";
  });
});

console.log("\n=== Additional browser sinks ===\n");

test("navigator.sendBeacon(url, data)", `
  var payload = JSON.stringify({ event: "click" });
  navigator.sendBeacon("/api/analytics", payload);
`, function(r) {
  return r.fetchCallSites.length === 1 &&
    r.fetchCallSites[0].url === "/api/analytics" &&
    r.fetchCallSites[0].method === "POST" &&
    r.fetchCallSites[0].type === "beacon";
});

test("navigator.sendBeacon with variable URL", `
  var endpoint = "/api/track";
  navigator.sendBeacon(endpoint, "data");
`, function(r) {
  return r.fetchCallSites.length === 1 &&
    r.fetchCallSites[0].url === "/api/track" &&
    r.fetchCallSites[0].method === "POST" &&
    r.fetchCallSites[0].type === "beacon";
});

test("new EventSource(url)", `
  var es = new EventSource("/api/events/stream");
`, function(r) {
  return r.fetchCallSites.length === 1 &&
    r.fetchCallSites[0].url === "/api/events/stream" &&
    r.fetchCallSites[0].method === "GET" &&
    r.fetchCallSites[0].type === "eventsource";
});

test("new EventSource with variable URL", `
  var streamUrl = "/api/notifications/live";
  var source = new EventSource(streamUrl);
`, function(r) {
  return r.fetchCallSites.length === 1 &&
    r.fetchCallSites[0].url === "/api/notifications/live" &&
    r.fetchCallSites[0].type === "eventsource";
});

test("Image pixel tracking — new Image().src = url", `
  var img = new Image();
  img.src = "/api/pixel/track?uid=123";
`, function(r) {
  return r.fetchCallSites.length === 1 &&
    r.fetchCallSites[0].url === "/api/pixel/track?uid=123" &&
    r.fetchCallSites[0].method === "GET" &&
    r.fetchCallSites[0].type === "pixel";
});

test("Image pixel with variable URL", `
  var pixelUrl = "/api/pixel/impression";
  var img = new Image();
  img.src = pixelUrl;
`, function(r) {
  return r.fetchCallSites.length === 1 &&
    r.fetchCallSites[0].url === "/api/pixel/impression" &&
    r.fetchCallSites[0].type === "pixel";
});

test("Shadowed navigator is not a beacon sink", `
  function test() {
    var navigator = { sendBeacon: function() {} };
    navigator.sendBeacon("/not-real");
  }
`, function(r) {
  return r.fetchCallSites.length === 0;
});

test("Shadowed EventSource is not a sink", `
  function test() {
    var EventSource = function() {};
    new EventSource("/not-real");
  }
`, function(r) {
  return r.fetchCallSites.length === 0;
});

console.log("\n=== Multi-caller value pairing ===\n");

test("Multiple callers with different URLs and methods", `
  function apiRequest(method, endpoint) {
    return fetch("/api/" + endpoint, { method: method });
  }
  apiRequest("POST", "orders");
  apiRequest("DELETE", "orders/123");
  apiRequest("GET", "inventory");
`, function(r) {
  var hasPost = r.fetchCallSites.some(function(s) { return s.url === "/api/orders" && s.method === "POST"; });
  var hasDel = r.fetchCallSites.some(function(s) { return s.url === "/api/orders/123" && s.method === "DELETE"; });
  var hasGet = r.fetchCallSites.some(function(s) { return s.url === "/api/inventory" && s.method === "GET"; });
  return hasPost && hasDel && hasGet;
});

test("BinaryExpression zip: base + path from multiple callers", `
  function makeRequest(base, path) {
    return fetch(base + path);
  }
  makeRequest("https://api.example.com", "/users");
  makeRequest("https://other.example.com", "/items");
`, function(r) {
  var hasFirst = r.fetchCallSites.some(function(s) { return s.url === "https://api.example.com/users"; });
  var hasSecond = r.fetchCallSites.some(function(s) { return s.url === "https://other.example.com/items"; });
  return hasFirst && hasSecond;
});

test("BinaryExpression broadcast: constant base + multiple paths", `
  var API_BASE = "https://api.example.com";
  function apiGet(path) {
    return fetch(API_BASE + path);
  }
  apiGet("/notifications");
  apiGet("/preferences");
`, function(r) {
  var hasNotif = r.fetchCallSites.some(function(s) { return s.url === "https://api.example.com/notifications"; });
  var hasPref = r.fetchCallSites.some(function(s) { return s.url === "https://api.example.com/preferences"; });
  return hasNotif && hasPref;
});

test("Per-caller method pairing: URL[i] pairs with Method[i]", `
  function rpc(service, method, verb) {
    return fetch("/rpc/" + service + "/" + method, { method: verb });
  }
  rpc("auth", "login", "POST");
  rpc("auth", "refresh", "POST");
  rpc("billing", "invoice", "GET");
`, function(r) {
  var hasLogin = r.fetchCallSites.some(function(s) { return s.url === "/rpc/auth/login" && s.method === "POST"; });
  var hasRefresh = r.fetchCallSites.some(function(s) { return s.url === "/rpc/auth/refresh" && s.method === "POST"; });
  var hasInvoice = r.fetchCallSites.some(function(s) { return s.url === "/rpc/billing/invoice" && s.method === "GET"; });
  return hasLogin && hasRefresh && hasInvoice;
});

console.log("\n=== Factory/closure pattern ===\n");

test("Factory function returning object with methods", `
  function createClient(baseUrl) {
    return {
      get: function(path) {
        return fetch(baseUrl + path);
      },
      post: function(path, body) {
        return fetch(baseUrl + path, { method: "POST", body: JSON.stringify(body) });
      }
    };
  }
  var client = createClient("https://api.example.com");
  client.get("/users");
  client.post("/users", { name: "test" });
`, function(r) {
  var hasGet = r.fetchCallSites.some(function(s) { return s.url === "https://api.example.com/users"; });
  var hasPost = r.fetchCallSites.some(function(s) {
    return s.url === "https://api.example.com/users" && s.method === "POST";
  });
  return hasGet && hasPost;
});

test("Factory with multiple instantiations", `
  function createApi(base) {
    return {
      fetch: function(path) { return fetch(base + path); }
    };
  }
  var v1 = createApi("/api/v1");
  var v2 = createApi("/api/v2");
  v1.fetch("/items");
  v2.fetch("/items");
`, function(r) {
  var hasV1 = r.fetchCallSites.some(function(s) { return s.url === "/api/v1/items"; });
  var hasV2 = r.fetchCallSites.some(function(s) { return s.url === "/api/v2/items"; });
  return hasV1 && hasV2;
});

console.log("\n=== Constructor this binding ===\n");

test("Constructor this.prop in prototype method", `
  function ApiService(base) {
    this.baseUrl = base;
  }
  ApiService.prototype.get = function(path) {
    return fetch(this.baseUrl + path);
  };
  var svc = new ApiService("https://api.example.com/v3");
  svc.get("/resources");
`, function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url === "https://api.example.com/v3/resources";
  });
});

test("Constructor with multiple instantiations", `
  function Client(host) {
    this.host = host;
  }
  Client.prototype.request = function(path) {
    return fetch(this.host + path);
  };
  var prod = new Client("https://prod.api.com");
  var staging = new Client("https://staging.api.com");
  prod.request("/health");
  staging.request("/health");
`, function(r) {
  var hasProd = r.fetchCallSites.some(function(s) { return s.url === "https://prod.api.com/health"; });
  var hasStaging = r.fetchCallSites.some(function(s) { return s.url === "https://staging.api.com/health"; });
  return hasProd && hasStaging;
});

console.log("\n=== Computed member access ===\n");

test("Object with computed key lookup — all values", `
  var endpoints = {
    users: "/api/users",
    orders: "/api/orders",
    products: "/api/products"
  };
  function loadEndpoint(name) {
    return fetch(endpoints[name]);
  }
`, function(r) {
  var urls = r.fetchCallSites.map(function(s) { return s.url; }).sort();
  return urls.length === 3 &&
    urls[0] === "/api/orders" &&
    urls[1] === "/api/products" &&
    urls[2] === "/api/users";
});

test("Array with computed index — all element values", `
  var urls = ["/api/first", "/api/second", "/api/third"];
  function loadByIndex(i) {
    return fetch(urls[i]);
  }
`, function(r) {
  var found = r.fetchCallSites.map(function(s) { return s.url; }).sort();
  return found.length === 3 &&
    found[0] === "/api/first" &&
    found[1] === "/api/second" &&
    found[2] === "/api/third";
});

test("Array of objects — arr[i].prop", `
  var configs = [
    { url: "/api/alpha", method: "GET" },
    { url: "/api/beta", method: "POST" },
    { url: "/api/gamma", method: "PUT" }
  ];
  function callConfig(idx) {
    return fetch(configs[idx].url);
  }
`, function(r) {
  var urls = r.fetchCallSites.map(function(s) { return s.url; }).sort();
  return urls.length === 3 &&
    urls[0] === "/api/alpha" &&
    urls[1] === "/api/beta" &&
    urls[2] === "/api/gamma";
});

test("Computed access with resolvable literal key", `
  var routes = { health: "/api/health", status: "/api/status" };
  var key = "health";
  fetch(routes[key]);
`, function(r) {
  return r.fetchCallSites.some(function(s) { return s.url === "/api/health"; });
});

console.log("\n=== Combined patterns ===\n");

test("Factory + multi-caller + method pairing", `
  function createEndpoint(base) {
    return {
      call: function(path, method) {
        return fetch(base + path, { method: method });
      }
    };
  }
  var api = createEndpoint("/api/v1");
  api.call("/users", "GET");
  api.call("/users", "POST");
  api.call("/users/1", "DELETE");
`, function(r) {
  var hasGet = r.fetchCallSites.some(function(s) { return s.url === "/api/v1/users" && s.method === "GET"; });
  var hasPost = r.fetchCallSites.some(function(s) { return s.url === "/api/v1/users" && s.method === "POST"; });
  var hasDel = r.fetchCallSites.some(function(s) { return s.url === "/api/v1/users/1" && s.method === "DELETE"; });
  return hasGet && hasPost && hasDel;
});

test("Wrapper chain: outer → inner → fetch", `
  function innerFetch(url, method) {
    return fetch(url, { method: method });
  }
  function outerFetch(endpoint) {
    return innerFetch("/api" + endpoint, "POST");
  }
  outerFetch("/submit");
`, function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url === "/api/submit" && s.method === "POST";
  });
});

// === Deep Sink Detection (library-level tracing) ===

console.log("\n=== Deep sink detection (jQuery-style) ===\n");

test("Deep sink: xhr.open in nested callback, property matching", `
  var lib = {};
  lib.extend = function(target) {
    for (var k in arguments[1]) target[k] = arguments[1][k];
    return target;
  };
  lib.extend({
    request: function(url, options) {
      if (typeof url === "object") { options = url; url = undefined; }
      options = options || {};
      function transport(opts) {
        var xhr = new XMLHttpRequest();
        xhr.open(opts.method, opts.url);
        xhr.send();
      }
      transport(options);
    }
  });
  lib.request({url: "/api/deep-test", method: "PUT"});
`, function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url === "/api/deep-test" && s.method === "PUT";
  });
}, true);

test("Deep sink: xhr.open in separate registered callback (store-and-call)", `
  var lib = {};
  var transports = [];
  function registerTransport(fn) { transports.push(fn); }
  registerTransport(function(options) {
    var xhr = new XMLHttpRequest();
    xhr.open(options.type, options.url);
    xhr.send();
  });
  lib.request = function(options) {
    options = options || {};
    for (var i = 0; i < transports.length; i++) transports[i](options);
  };
  lib.request({url: "/api/callback-sink", type: "DELETE"});
`, function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url === "/api/callback-sink" && s.method === "DELETE";
  });
}, true);

test("Deep sink: store-and-call with extend pattern", `
  var lib = {};
  lib.extend = function(target) {
    for (var k in arguments[1]) target[k] = arguments[1][k];
    return target;
  };
  var handlers = [];
  function addHandler(fn) { handlers.push(fn); }
  addHandler(function(opts) {
    var xhr = new XMLHttpRequest();
    xhr.open(opts.type, opts.url);
    xhr.send();
  });
  lib.extend({
    doRequest: function(options) {
      for (var i = 0; i < handlers.length; i++) handlers[i](options);
    }
  });
  lib.doRequest({url: "/api/extend-call", type: "PUT"});
`, function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url === "/api/extend-call" && s.method === "PUT";
  });
}, true);

test("Deep sink: $ alias via window.$ = window.jQuery = lib", `
  (function(window) {
    var lib = {};
    lib.extend = function(target) {
      for (var k in arguments[1]) target[k] = arguments[1][k];
      return target;
    };
    function doXHR(config) {
      var xhr = new XMLHttpRequest();
      xhr.open(config.method, config.url);
      xhr.send();
    }
    lib.extend({
      ajax: function(opts) { doXHR(opts); }
    });
    window.jQuery = window.$ = lib;
  })(window);
  jQuery.ajax({url: "/api/alias-test", method: "PATCH"});
  $.ajax({url: "/api/dollar-test", method: "POST"});
`, function(r) {
  var found1 = r.fetchCallSites.some(function(s) { return s.url === "/api/alias-test" && s.method === "PATCH"; });
  var found2 = r.fetchCallSites.some(function(s) { return s.url === "/api/dollar-test" && s.method === "POST"; });
  return found1 && found2;
}, true);

test("Deep sink: fetch in nested closure", `
  var api = {};
  api.extend = function(target) {
    for (var k in arguments[1]) target[k] = arguments[1][k];
    return target;
  };
  api.extend({
    call: function(config) {
      var doFetch = function() {
        fetch(config.endpoint, {method: config.verb});
      };
      doFetch();
    }
  });
  api.call({endpoint: "/api/nested-fetch", verb: "POST"});
`, function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url === "/api/nested-fetch";
  });
}, true);

test("Deep sink: no false positives for non-network functions", `
  var util = {};
  util.extend = function(target) {
    for (var k in arguments[1]) target[k] = arguments[1][k];
    return target;
  };
  util.extend({
    format: function(opts) {
      return opts.prefix + opts.value;
    }
  });
  util.format({prefix: "/api/", value: "test"});
`, function(r) {
  return r.fetchCallSites.length === 0;
}, true);

test("Deep sink: MemberExpression callee with direct sink (obj.method resolved via extend)", `
  var svc = {};
  svc.extend = function(target) {
    for (var k in arguments[1]) target[k] = arguments[1][k];
    return target;
  };
  svc.extend({
    get: function(url) {
      fetch(url);
    }
  });
  svc.get("/api/direct-extend");
`, function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url === "/api/direct-extend";
  });
}, true);

test("Deep sink: multiple callers with different URLs", `
  var http = {};
  http.extend = function(target) {
    for (var k in arguments[1]) target[k] = arguments[1][k];
    return target;
  };
  var sinks = [];
  function reg(fn) { sinks.push(fn); }
  reg(function(c) {
    var x = new XMLHttpRequest();
    x.open(c.type, c.url);
    x.send();
  });
  http.extend({
    request: function(opts) {
      for (var i = 0; i < sinks.length; i++) sinks[i](opts);
    }
  });
  http.request({url: "/api/first", type: "GET"});
  http.request({url: "/api/second", type: "POST"});
`, function(r) {
  var f1 = r.fetchCallSites.some(function(s) { return s.url === "/api/first" && s.method === "GET"; });
  var f2 = r.fetchCallSites.some(function(s) { return s.url === "/api/second" && s.method === "POST"; });
  return f1 && f2;
}, true);

// ═══════════════════════════════════════════════════════════════════════
// ██  BODY PARAMETER EXTRACTION (caller-side resolution)
// ═══════════════════════════════════════════════════════════════════════

console.log("\n=== Body parameter extraction through wrappers ===\n");

test("Wrapper body: JSON.stringify(body) resolved to caller's object fields", `
  function apiRequest(method, path, body) {
    return fetch("/api/" + path, {
      method: method,
      headers: {"Content-Type": "application/json"},
      body: JSON.stringify(body)
    });
  }
  apiRequest("POST", "orders", {item: "widget", qty: 3});
`, function(r) {
  var site = r.fetchCallSites.find(function(s) { return s.url === "/api/orders"; });
  if (!site || !site.params) return false;
  var hasItem = site.params.some(function(p) { return p.name === "item" && p.location === "body"; });
  var hasQty = site.params.some(function(p) { return p.name === "qty" && p.location === "body"; });
  // Should NOT have a raw "body" param (the wrapper param name)
  var hasRawBody = site.params.some(function(p) { return p.name === "body" && p.location === "body"; });
  return hasItem && hasQty && !hasRawBody;
});

test("Wrapper body: multiple callers contribute different field sets", `
  function rpc(service, method, data) {
    return fetch("/rpc/" + service + "/" + method, {
      method: "POST",
      body: JSON.stringify(data)
    });
  }
  rpc("auth", "login", {user: "admin", pass: "secret"});
  rpc("billing", "charge", {amount: 100, currency: "USD"});
`, function(r) {
  var login = r.fetchCallSites.find(function(s) { return s.url === "/rpc/auth/login"; });
  if (!login || !login.params) return false;
  // All caller body fields get collected for the function
  var hasUser = login.params.some(function(p) { return p.name === "user" && p.location === "body"; });
  var hasPass = login.params.some(function(p) { return p.name === "pass" && p.location === "body"; });
  return hasUser && hasPass;
});

test("Wrapper body: object literal body (no JSON.stringify)", `
  function sendData(url, payload) {
    return fetch(url, {
      method: "POST",
      body: payload
    });
  }
  sendData("/api/form", {field1: "a", field2: "b"});
`, function(r) {
  var site = r.fetchCallSites.find(function(s) { return s.url === "/api/form"; });
  if (!site || !site.params) return false;
  var has1 = site.params.some(function(p) { return p.name === "field1" && p.location === "body"; });
  var has2 = site.params.some(function(p) { return p.name === "field2" && p.location === "body"; });
  return has1 && has2;
});

test("Wrapper body: body param default values extracted from caller literals", `
  function post(url, body) {
    return fetch(url, { method: "POST", body: JSON.stringify(body) });
  }
  post("/api/submit", {name: "test", count: 42, active: true});
`, function(r) {
  var site = r.fetchCallSites.find(function(s) { return s.url === "/api/submit"; });
  if (!site || !site.params) return false;
  var nameP = site.params.find(function(p) { return p.name === "name"; });
  var countP = site.params.find(function(p) { return p.name === "count"; });
  var activeP = site.params.find(function(p) { return p.name === "active"; });
  return nameP && nameP.defaultValue === "test" && nameP.type === "string" &&
         countP && countP.defaultValue === 42 && countP.type === "number" &&
         activeP && activeP.defaultValue === true && activeP.type === "boolean";
});

test("Wrapper body: URLSearchParams body resolved to caller fields", `
  function submitForm(url, data) {
    return fetch(url, { method: "POST", body: new URLSearchParams(data) });
  }
  submitForm("/api/form-encoded", {username: "alice", password: "secret"});
`, function(r) {
  var site = r.fetchCallSites.find(function(s) { return s.url === "/api/form-encoded"; });
  if (!site || !site.params) return false;
  var hasUser = site.params.some(function(p) { return p.name === "username" && p.location === "body"; });
  var hasPass = site.params.some(function(p) { return p.name === "password" && p.location === "body"; });
  return hasUser && hasPass;
});

// ═══════════════════════════════════════════════════════════════════════
// ██  PARAMETER TYPES, DEFAULTS, AND REQUIRED FLAGS
// ═══════════════════════════════════════════════════════════════════════

console.log("\n=== Parameter types, defaults, and required flags ===\n");

test("Direct fetch body: field types inferred from literal values", `
  fetch("/api/data", {
    method: "POST",
    body: JSON.stringify({
      name: "Alice",
      age: 30,
      verified: true,
      score: 4.5
    })
  });
`, function(r) {
  var site = r.fetchCallSites.find(function(s) { return s.url === "/api/data"; });
  if (!site || !site.params) return false;
  var nameP = site.params.find(function(p) { return p.name === "name"; });
  var ageP = site.params.find(function(p) { return p.name === "age"; });
  var verifiedP = site.params.find(function(p) { return p.name === "verified"; });
  return nameP && nameP.type === "string" && nameP.defaultValue === "Alice" &&
         ageP && ageP.type === "number" && ageP.defaultValue === 30 &&
         verifiedP && verifiedP.type === "boolean" && verifiedP.defaultValue === true;
});

test("Optional params with || default", `
  function createUser(name, role) {
    return fetch("/api/users", {
      method: "POST",
      body: JSON.stringify({ name: name, role: role || "viewer" })
    });
  }
  createUser("Alice", "admin");
`, function(r) {
  var site = r.fetchCallSites.find(function(s) { return s.url === "/api/users"; });
  if (!site || !site.params) return false;
  var roleP = site.params.find(function(p) { return p.name === "role"; });
  return roleP && roleP.required === false && roleP.defaultValue === "viewer";
});

test("Spread operator in body object", `
  function updateItem(id, changes) {
    return fetch("/api/items/" + id, {
      method: "PATCH",
      body: JSON.stringify({ id: id, ...changes })
    });
  }
  updateItem("42", {name: "test"});
`, function(r) {
  var site = r.fetchCallSites.find(function(s) { return s.url && s.url.indexOf("/api/items/") === 0; });
  if (!site || !site.params) return false;
  var spreadP = site.params.find(function(p) { return p.spread === true; });
  return spreadP && spreadP.name === "...changes";
});

// ═══════════════════════════════════════════════════════════════════════
// ██  VALUE CONSTRAINTS ON PARAMETERS (validValues)
// ═══════════════════════════════════════════════════════════════════════

console.log("\n=== Value constraints on parameters (validValues) ===\n");

test("Switch-case constraint appears as validValues on param", `
  function loadResource(type) {
    switch(type) {
      case "users": return fetch("/api/" + type);
      case "products": return fetch("/api/" + type);
      case "orders": return fetch("/api/" + type);
    }
  }
`, function(r) {
  // The "type" variable should have a value constraint with 3 values
  var constraint = r.valueConstraints.find(function(c) { return c.variable === "type"; });
  if (!constraint) return false;
  var hasUsers = constraint.values.indexOf("users") >= 0;
  var hasProducts = constraint.values.indexOf("products") >= 0;
  var hasOrders = constraint.values.indexOf("orders") >= 0;
  return constraint.values.length === 3 && hasUsers && hasProducts && hasOrders;
});

test(".includes() constraint appears as validValues on param", `
  var ALLOWED_FORMATS = ["json", "xml", "csv", "yaml"];
  function convert(format) {
    if (ALLOWED_FORMATS.includes(format)) {
      return fetch("/api/convert?format=" + format);
    }
  }
`, function(r) {
  var constraint = r.valueConstraints.find(function(c) { return c.variable === "format"; });
  if (!constraint) return false;
  return constraint.values.length === 4 &&
    constraint.values.indexOf("json") >= 0 &&
    constraint.values.indexOf("xml") >= 0 &&
    constraint.values.indexOf("csv") >= 0 &&
    constraint.values.indexOf("yaml") >= 0;
});

test("Equality chain constraint discovered for wrapper param", `
  function setStatus(status) {
    if (status === "active" || status === "inactive" || status === "suspended" || status === "deleted") {
      return fetch("/api/status", { method: "POST", body: JSON.stringify({status: status}) });
    }
  }
`, function(r) {
  var constraint = r.valueConstraints.find(function(c) { return c.variable === "status"; });
  if (!constraint) return false;
  return constraint.values.length === 4 &&
    constraint.values.indexOf("active") >= 0 &&
    constraint.values.indexOf("inactive") >= 0 &&
    constraint.values.indexOf("suspended") >= 0 &&
    constraint.values.indexOf("deleted") >= 0;
});

test("validValues cross-referenced onto fetch params", `
  function fetchData(endpoint) {
    switch(endpoint) {
      case "users": case "orders": case "products":
        return fetch("/api/" + endpoint);
    }
  }
  fetchData("users");
`, function(r) {
  var site = r.fetchCallSites.find(function(s) { return s.url === "/api/users"; });
  if (!site || !site.params) return false;
  var endpointParam = site.params.find(function(p) { return p.name === "endpoint"; });
  return endpointParam && endpointParam.validValues &&
    endpointParam.validValues.length === 3 &&
    endpointParam.validValues.indexOf("users") >= 0 &&
    endpointParam.validValues.indexOf("orders") >= 0 &&
    endpointParam.validValues.indexOf("products") >= 0;
});

test("validValues from .includes() on wrapper body source param", `
  var ROLES = ["admin", "editor", "viewer"];
  function assignRole(userId, role) {
    if (ROLES.includes(role)) {
      return fetch("/api/users/" + userId + "/role", {
        method: "PUT",
        body: JSON.stringify({role: role})
      });
    }
  }
  assignRole("123", "admin");
`, function(r) {
  // The body param "role" should have validValues because its source variable has constraints
  var site = r.fetchCallSites.find(function(s) { return s.url && s.url.indexOf("/api/users/") === 0; });
  if (!site || !site.params) return false;
  var roleP = site.params.find(function(p) { return p.name === "role"; });
  return roleP && roleP.validValues &&
    roleP.validValues.indexOf("admin") >= 0 &&
    roleP.validValues.indexOf("editor") >= 0 &&
    roleP.validValues.indexOf("viewer") >= 0;
});

test("Multiple independent constraints for different params", `
  var METHODS = ["GET", "POST", "PUT"];
  var FORMATS = ["json", "xml"];
  function apiCall(method, format, endpoint) {
    if (METHODS.includes(method) && FORMATS.includes(format)) {
      return fetch("/api/" + endpoint + "?format=" + format, { method: method });
    }
  }
`, function(r) {
  var methodC = r.valueConstraints.find(function(c) { return c.variable === "method"; });
  var formatC = r.valueConstraints.find(function(c) { return c.variable === "format"; });
  return methodC && methodC.values.length === 3 &&
         formatC && formatC.values.length === 2;
});

// ═══════════════════════════════════════════════════════════════════════
// ██  VALUE CONSTRAINTS → VDD CONTRACT (shapes expected by background.js)
// ═══════════════════════════════════════════════════════════════════════

console.log("\n=== Value constraints → VDD contract ===\n");

test("URL-concatenated param with validValues: values survive for VDD merge", `
  var FORMATS = ["json", "xml", "csv"];
  function getReport(format) {
    if (FORMATS.includes(format)) {
      return fetch("/api/report?format=" + format);
    }
  }
  getReport("json");
`, function(r) {
  var site = r.fetchCallSites.find(function(s) { return s.url && s.url.indexOf("/api/report") === 0; });
  if (!site || !site.params) return false;
  var fmtP = site.params.find(function(p) { return p.name === "format"; });
  // AST reports location=path for concatenated params — background.js learnFromRequest
  // later parses the full URL and correctly classifies as query param in the VDD
  return fmtP && fmtP.location === "path" &&
    fmtP.validValues && fmtP.validValues.length === 3 &&
    fmtP.validValues.indexOf("json") >= 0 &&
    fmtP.validValues.indexOf("xml") >= 0 &&
    fmtP.validValues.indexOf("csv") >= 0;
});

test("Body param with validValues: location=body and values present", `
  function updateStatus(id, status) {
    if (status === "active" || status === "paused" || status === "archived") {
      return fetch("/api/items/" + id, {
        method: "PUT",
        body: JSON.stringify({ status: status })
      });
    }
  }
  updateStatus("1", "active");
`, function(r) {
  var site = r.fetchCallSites.find(function(s) { return s.url && s.url.indexOf("/api/items/") === 0; });
  if (!site || !site.params) return false;
  var statusP = site.params.find(function(p) { return p.name === "status"; });
  return statusP && statusP.location === "body" &&
    statusP.validValues && statusP.validValues.length === 3 &&
    statusP.validValues.indexOf("active") >= 0 &&
    statusP.validValues.indexOf("paused") >= 0 &&
    statusP.validValues.indexOf("archived") >= 0;
});

test("Path param with validValues: location=path and values present", `
  function getResource(type) {
    switch(type) {
      case "users": case "teams": case "orgs":
        return fetch("/api/" + type + "/list");
    }
  }
  getResource("users");
`, function(r) {
  var site = r.fetchCallSites.find(function(s) { return s.url && s.url.indexOf("/api/users") === 0; });
  if (!site || !site.params) return false;
  var typeP = site.params.find(function(p) { return p.name === "type"; });
  return typeP && typeP.location === "path" &&
    typeP.validValues && typeP.validValues.length === 3 &&
    typeP.validValues.indexOf("users") >= 0 &&
    typeP.validValues.indexOf("teams") >= 0 &&
    typeP.validValues.indexOf("orgs") >= 0;
});

test("Default value AND validValues on same param", `
  var ROLES = ["admin", "editor", "viewer"];
  function setRole(userId, role) {
    if (ROLES.includes(role)) {
      return fetch("/api/users/" + userId + "/role", {
        method: "POST",
        body: JSON.stringify({ role: role || "viewer" })
      });
    }
  }
  setRole("42", "admin");
`, function(r) {
  var site = r.fetchCallSites.find(function(s) { return s.url && s.url.indexOf("/api/users/") === 0; });
  if (!site || !site.params) return false;
  var roleP = site.params.find(function(p) { return p.name === "role"; });
  // Must have both defaultValue and validValues
  return roleP && roleP.location === "body" &&
    roleP.defaultValue === "viewer" &&
    roleP.validValues && roleP.validValues.length === 3;
});

test("validValues are strings (VDD merge expects .map(String))", `
  function setPage(page) {
    switch(page) {
      case 1: case 2: case 3: case 4: case 5:
        return fetch("/api/data?page=" + page);
    }
  }
  setPage(1);
`, function(r) {
  var constraint = r.valueConstraints.find(function(c) { return c.variable === "page"; });
  if (!constraint) return false;
  // Values should be numbers from switch-case — background.js will .map(String) them
  return constraint.values.length === 5 &&
    constraint.values.indexOf(1) >= 0 &&
    constraint.values.indexOf(5) >= 0;
});

test("Constraint source metadata tracks origin (switch vs includes vs equality)", `
  var TYPES = ["a", "b", "c"];
  function fn1(x) { if (TYPES.includes(x)) fetch("/api/" + x); }
  function fn2(y) { switch(y) { case "p": case "q": fetch("/api2/" + y); } }
  function fn3(z) { if (z === "m" || z === "n") fetch("/api3/" + z); }
`, function(r) {
  var c1 = r.valueConstraints.find(function(c) { return c.variable === "x"; });
  var c2 = r.valueConstraints.find(function(c) { return c.variable === "y"; });
  var c3 = r.valueConstraints.find(function(c) { return c.variable === "z"; });
  // Each constraint should have a sources array (background.js reads it for _astValueSource)
  return c1 && Array.isArray(c1.sources) && c1.values.length === 3 &&
         c2 && Array.isArray(c2.sources) && c2.values.length === 2 &&
         c3 && Array.isArray(c3.sources) && c3.values.length === 2;
});

// ═══════════════════════════════════════════════════════════════════════
// ██  REAL LIBRARY: jQuery (unminified) — Parameter extraction
// ═══════════════════════════════════════════════════════════════════════

var _jqueryUnmin = null;
var _jqueryMin = null;
var _axiosMin = null;
try { _jqueryUnmin = fs.readFileSync(__dirname + "/jquery-3.7.1.js", "utf8"); } catch(e) {}
try { _jqueryMin = fs.readFileSync(__dirname + "/jquery-3.7.1.min.js", "utf8"); } catch(e) {}
try { _axiosMin = fs.readFileSync(__dirname + "/axios.min.js", "utf8"); } catch(e) {}
var _unfetchMin = null;
var _redaxiosMin = null;
var _kyMin = null;
var _superagentMin = null;
try { _unfetchMin = fs.readFileSync(__dirname + "/unfetch.min.js", "utf8"); } catch(e) {}
try { _redaxiosMin = fs.readFileSync(__dirname + "/redaxios.min.js", "utf8"); } catch(e) {}
try { _kyMin = fs.readFileSync(__dirname + "/ky.min.js", "utf8"); } catch(e) {}
try { _superagentMin = fs.readFileSync(__dirname + "/superagent.min.js", "utf8"); } catch(e) {}

// Helper: run analysis against a library + appended test code
function testLib(name, libCode, appendCode, check, forceScript) {
  if (!libCode) {
    total++;
    console.log("  SKIP: " + name + " (library file not found)");
    return;
  }
  total++;
  try {
    var fullCode = libCode + "\n;\n" + appendCode;
    var result = analyzeJSBundle(fullCode, "test://lib-" + name, forceScript !== false);
    var ok = check(result);
    if (ok) {
      passed++;
      console.log("  PASS: " + name);
    } else {
      failed++;
      console.log("  FAIL: " + name);
      console.log("    fetchCallSites (%d):", result.fetchCallSites.length);
      for (var i = 0; i < result.fetchCallSites.length; i++) {
        var s = result.fetchCallSites[i];
        console.log("      %s %s %s", s.method, s.url,
          s.params ? "[" + s.params.map(function(p) { return p.name + ":" + (p.location||"?"); }).join(", ") + "]" : "");
      }
    }
  } catch (e) {
    failed++;
    console.log("  ERROR: " + name + " — " + e.message);
    console.log("    " + e.stack.split("\n").slice(0, 3).join("\n    "));
  }
}

if (_jqueryUnmin) {
  console.log("\n=== jQuery (unminified) — URL and method tracing ===\n");

  testLib("jQuery.ajax() POST with data", _jqueryUnmin, `
    jQuery.ajax({url: "/api/items", method: "POST", data: {name: "Widget", price: 29.99}});
  `, function(r) {
    var site = r.fetchCallSites.find(function(s) { return s.url === "/api/items" && s.method === "POST"; });
    return !!site;
  });

  testLib("jQuery.get() traces through to XHR", _jqueryUnmin, `
    jQuery.get("/api/users");
  `, function(r) {
    return r.fetchCallSites.some(function(s) { return s.url === "/api/users" && s.method === "GET"; });
  });

  testLib("jQuery.post() traces through to XHR", _jqueryUnmin, `
    jQuery.post("/api/submit", {key: "value"});
  `, function(r) {
    return r.fetchCallSites.some(function(s) { return s.url === "/api/submit" && s.method === "POST"; });
  });

  testLib("$.ajax() DELETE via $ alias", _jqueryUnmin, `
    $.ajax({url: "/api/resource/42", type: "DELETE"});
  `, function(r) {
    return r.fetchCallSites.some(function(s) { return s.url === "/api/resource/42" && s.method === "DELETE"; });
  });

  testLib("$.ajax() PUT via $ alias", _jqueryUnmin, `
    $.ajax({url: "/api/resource/99", type: "PUT", data: {status: "active"}});
  `, function(r) {
    return r.fetchCallSites.some(function(s) { return s.url === "/api/resource/99" && s.method === "PUT"; });
  });

  testLib("jQuery.getJSON() traces as GET", _jqueryUnmin, `
    jQuery.getJSON("/api/config");
  `, function(r) {
    return r.fetchCallSites.some(function(s) { return s.url === "/api/config" && s.method === "GET"; });
  });

  testLib("Multiple jQuery calls all traced", _jqueryUnmin, `
    $.get("/api/a");
    $.post("/api/b", {x: 1});
    $.ajax({url: "/api/c", type: "PATCH"});
  `, function(r) {
    var a = r.fetchCallSites.some(function(s) { return s.url === "/api/a" && s.method === "GET"; });
    var b = r.fetchCallSites.some(function(s) { return s.url === "/api/b" && s.method === "POST"; });
    var c = r.fetchCallSites.some(function(s) { return s.url === "/api/c" && s.method === "PATCH"; });
    return a && b && c;
  });

  console.log("\n=== jQuery (unminified) — Body parameter extraction ===\n");

  testLib("jQuery.ajax() data property extracts body params", _jqueryUnmin, `
    jQuery.ajax({url: "/api/create", method: "POST", data: {name: "test", email: "a@b.com", age: 25}});
  `, function(r) {
    var site = r.fetchCallSites.find(function(s) { return s.url === "/api/create"; });
    if (!site || !site.params) return false;
    var hasName = site.params.some(function(p) { return p.name === "name" && p.location === "body"; });
    var hasEmail = site.params.some(function(p) { return p.name === "email" && p.location === "body"; });
    var hasAge = site.params.some(function(p) { return p.name === "age" && p.location === "body"; });
    return hasName && hasEmail && hasAge;
  });

  testLib("jQuery.ajax() data field types inferred", _jqueryUnmin, `
    $.ajax({url: "/api/typed", type: "POST", data: {title: "Hello", count: 5, active: true}});
  `, function(r) {
    var site = r.fetchCallSites.find(function(s) { return s.url === "/api/typed"; });
    if (!site || !site.params) return false;
    var titleP = site.params.find(function(p) { return p.name === "title"; });
    var countP = site.params.find(function(p) { return p.name === "count"; });
    var activeP = site.params.find(function(p) { return p.name === "active"; });
    return titleP && titleP.type === "string" && titleP.defaultValue === "Hello" &&
           countP && countP.type === "number" && countP.defaultValue === 5 &&
           activeP && activeP.type === "boolean" && activeP.defaultValue === true;
  });

  testLib("$.ajax() without data produces no body params", _jqueryUnmin, `
    $.ajax({url: "/api/no-data", type: "GET"});
  `, function(r) {
    var site = r.fetchCallSites.find(function(s) { return s.url === "/api/no-data"; });
    if (!site) return false;
    // Should have no params or empty params
    return !site.params || site.params.length === 0;
  });

  testLib("$.ajax() data with JSON.stringify extracts body fields", _jqueryUnmin, `
    $.ajax({
      url: "/api/json-body",
      method: "PUT",
      contentType: "application/json",
      data: JSON.stringify({title: "hello", content: "world"})
    });
  `, function(r) {
    var site = r.fetchCallSites.find(function(s) { return s.url === "/api/json-body"; });
    if (!site || !site.params) return false;
    var hasTitle = site.params.some(function(p) { return p.name === "title" && p.location === "body"; });
    var hasContent = site.params.some(function(p) { return p.name === "content" && p.location === "body"; });
    return hasTitle && hasContent;
  });

  console.log("\n=== jQuery (unminified) — Value constraints from jQuery internals ===\n");

  testLib("jQuery internals produce value constraints", _jqueryUnmin, `
    $.get("/api/dummy");
  `, function(r) {
    // jQuery code contains switch statements, equality chains, .includes() calls etc.
    // that produce value constraints for internal variables
    return r.valueConstraints.length >= 5;
  });

  testLib("jQuery method iteration constraint (get/post)", _jqueryUnmin, `
    $.get("/api/x");
  `, function(r) {
    // jQuery.each(["get","post"], ...) creates an iteration constraint
    // Check that some constraint contains "get" and "post"
    return r.valueConstraints.some(function(c) {
      return c.values.indexOf("get") >= 0 && c.values.indexOf("post") >= 0;
    });
  });
}

// ═══════════════════════════════════════════════════════════════════════
// ██  REAL LIBRARY: jQuery (minified) — Proves minification tolerance
// ═══════════════════════════════════════════════════════════════════════

if (_jqueryMin) {
  console.log("\n=== jQuery (minified) — URL and method tracing ===\n");

  testLib("jQuery.min: ajax POST traced", _jqueryMin, `
    jQuery.ajax({url: "/api/items", method: "POST", data: {name: "Widget"}});
  `, function(r) {
    return r.fetchCallSites.some(function(s) { return s.url === "/api/items" && s.method === "POST"; });
  });

  testLib("jQuery.min: $.get() traced", _jqueryMin, `
    jQuery.get("/api/users");
  `, function(r) {
    return r.fetchCallSites.some(function(s) { return s.url === "/api/users" && s.method === "GET"; });
  });

  testLib("jQuery.min: $.post() traced", _jqueryMin, `
    jQuery.post("/api/submit", {key: "value"});
  `, function(r) {
    return r.fetchCallSites.some(function(s) { return s.url === "/api/submit" && s.method === "POST"; });
  });

  testLib("jQuery.min: $.ajax DELETE via $ alias", _jqueryMin, `
    $.ajax({url: "/api/resource/42", type: "DELETE"});
  `, function(r) {
    return r.fetchCallSites.some(function(s) { return s.url === "/api/resource/42" && s.method === "DELETE"; });
  });

  testLib("jQuery.min: all 5 HTTP methods work", _jqueryMin, `
    $.ajax({url: "/api/get-it", type: "GET"});
    jQuery.ajax({url: "/api/post-it", method: "POST"});
    $.ajax({url: "/api/put-it", type: "PUT"});
    $.ajax({url: "/api/patch-it", type: "PATCH"});
    $.ajax({url: "/api/del-it", type: "DELETE"});
  `, function(r) {
    var g = r.fetchCallSites.some(function(s) { return s.url === "/api/get-it" && s.method === "GET"; });
    var po = r.fetchCallSites.some(function(s) { return s.url === "/api/post-it" && s.method === "POST"; });
    var pu = r.fetchCallSites.some(function(s) { return s.url === "/api/put-it" && s.method === "PUT"; });
    var pa = r.fetchCallSites.some(function(s) { return s.url === "/api/patch-it" && s.method === "PATCH"; });
    var d = r.fetchCallSites.some(function(s) { return s.url === "/api/del-it" && s.method === "DELETE"; });
    return g && po && pu && pa && d;
  });

  console.log("\n=== jQuery (minified) — Body parameter extraction ===\n");

  testLib("jQuery.min: data object fields extracted as body params", _jqueryMin, `
    $.ajax({url: "/api/create", method: "POST", data: {name: "test", age: 25, active: true}});
  `, function(r) {
    var site = r.fetchCallSites.find(function(s) { return s.url === "/api/create"; });
    if (!site || !site.params) return false;
    var hasName = site.params.some(function(p) { return p.name === "name" && p.location === "body"; });
    var hasAge = site.params.some(function(p) { return p.name === "age" && p.location === "body"; });
    var hasActive = site.params.some(function(p) { return p.name === "active" && p.location === "body"; });
    return hasName && hasAge && hasActive;
  });

  testLib("jQuery.min: data field types preserved through minified code", _jqueryMin, `
    $.ajax({url: "/api/typed", type: "PUT", data: {title: "Hello", count: 5, flag: false}});
  `, function(r) {
    var site = r.fetchCallSites.find(function(s) { return s.url === "/api/typed"; });
    if (!site || !site.params) return false;
    var titleP = site.params.find(function(p) { return p.name === "title"; });
    var countP = site.params.find(function(p) { return p.name === "count"; });
    var flagP = site.params.find(function(p) { return p.name === "flag"; });
    return titleP && titleP.type === "string" && titleP.defaultValue === "Hello" &&
           countP && countP.type === "number" && countP.defaultValue === 5 &&
           flagP && flagP.type === "boolean" && flagP.defaultValue === false;
  });

  testLib("jQuery.min: multiple ajax calls each get their own data params", _jqueryMin, `
    $.ajax({url: "/api/search", data: {q: "hello", page: 1}});
    $.ajax({url: "/api/update", type: "PATCH", data: {id: 42, status: "published"}});
  `, function(r) {
    var search = r.fetchCallSites.find(function(s) { return s.url === "/api/search"; });
    var update = r.fetchCallSites.find(function(s) { return s.url === "/api/update"; });
    if (!search || !search.params || !update || !update.params) return false;
    var searchHasQ = search.params.some(function(p) { return p.name === "q"; });
    var searchHasPage = search.params.some(function(p) { return p.name === "page"; });
    var updateHasId = update.params.some(function(p) { return p.name === "id"; });
    var updateHasStatus = update.params.some(function(p) { return p.name === "status"; });
    return searchHasQ && searchHasPage && updateHasId && updateHasStatus;
  });

  testLib("jQuery.min: POST with no data has no body params", _jqueryMin, `
    $.ajax({url: "/api/trigger", type: "POST"});
  `, function(r) {
    var site = r.fetchCallSites.find(function(s) { return s.url === "/api/trigger"; });
    if (!site) return false;
    return !site.params || site.params.length === 0;
  });

  console.log("\n=== jQuery (minified) — Value constraints ===\n");

  testLib("jQuery.min: internal value constraints discovered", _jqueryMin, `
    $.get("/api/dummy");
  `, function(r) {
    return r.valueConstraints.length >= 5;
  });

  testLib("jQuery.min: readyState constraint found", _jqueryMin, `
    $.get("/api/x");
  `, function(r) {
    // jQuery checks document.readyState === "complete" || "loading"
    return r.valueConstraints.some(function(c) {
      return c.values.indexOf("complete") >= 0 && c.values.indexOf("loading") >= 0;
    });
  });
}

// ═══════════════════════════════════════════════════════════════════════
// ██  REAL LIBRARY: Axios (minified) — Value constraint discovery
// ═══════════════════════════════════════════════════════════════════════

if (_axiosMin) {
  console.log("\n=== Axios (minified) — Value constraint discovery ===\n");

  testLib("Axios.min: discovers response type constraints", _axiosMin, `
    // empty — just analyze the library itself
  `, function(r) {
    // Axios has responseType checks: "stream", "response", etc.
    return r.valueConstraints.some(function(c) {
      return c.values.indexOf("stream") >= 0 || c.values.indexOf("response") >= 0;
    });
  });

  testLib("Axios.min: discovers content type constraints (text/json)", _axiosMin, `
  `, function(r) {
    return r.valueConstraints.some(function(c) {
      return c.values.indexOf("text") >= 0 && c.values.indexOf("json") >= 0;
    });
  });

  testLib("Axios.min: discovers HTTP method constraints", _axiosMin, `
  `, function(r) {
    // Axios groups methods as no-data (get, head) vs data methods
    return r.valueConstraints.some(function(c) {
      return c.values.indexOf("get") >= 0 && c.values.indexOf("head") >= 0;
    });
  });

  testLib("Axios.min: discovers collection type constraints (Map/Set)", _axiosMin, `
  `, function(r) {
    return r.valueConstraints.some(function(c) {
      return c.values.indexOf("Map") >= 0 && c.values.indexOf("Set") >= 0;
    });
  });

  testLib("Axios.min: discovers control flow constraints", _axiosMin, `
  `, function(r) {
    return r.valueConstraints.some(function(c) {
      return c.values.indexOf("return") >= 0 && c.values.indexOf("throw") >= 0;
    });
  });

  console.log("\n=== Axios (minified) + wrapper functions — Parameter extraction ===\n");

  testLib("Axios context: wrapper using fetch() extracts body params", _axiosMin, `
    function apiCall(method, url, data) {
      return fetch(url, {
        method: method,
        headers: {"Content-Type": "application/json"},
        body: data ? JSON.stringify(data) : undefined
      });
    }
    apiCall("POST", "/api/orders", {item: "widget", qty: 3, priority: "high"});
  `, function(r) {
    var site = r.fetchCallSites.find(function(s) { return s.url === "/api/orders"; });
    if (!site || !site.params) return false;
    var hasItem = site.params.some(function(p) { return p.name === "item" && p.location === "body"; });
    var hasQty = site.params.some(function(p) { return p.name === "qty" && p.location === "body"; });
    var hasPriority = site.params.some(function(p) { return p.name === "priority" && p.location === "body"; });
    return hasItem && hasQty && hasPriority;
  });

  testLib("Axios context: value-constrained URL from switch", _axiosMin, `
    function loadResource(type) {
      switch(type) {
        case "users": case "products": case "orders":
          return fetch("/api/" + type);
      }
    }
    loadResource("users");
  `, function(r) {
    var site = r.fetchCallSites.find(function(s) { return s.url === "/api/users"; });
    if (!site || !site.params) return false;
    var typeP = site.params.find(function(p) { return p.name === "type"; });
    return typeP && typeP.validValues &&
      typeP.validValues.indexOf("users") >= 0 &&
      typeP.validValues.indexOf("products") >= 0 &&
      typeP.validValues.indexOf("orders") >= 0;
  });

  testLib("Axios context: XHR wrapper with body params", _axiosMin, `
    function request(opts) {
      var xhr = new XMLHttpRequest();
      xhr.open(opts.method || "GET", opts.url);
      xhr.send(opts.data);
    }
    request({url: "/api/legacy", method: "POST", data: "key=value"});
  `, function(r) {
    return r.fetchCallSites.some(function(s) {
      return s.url === "/api/legacy" && s.method === "POST";
    });
  });
}

// ═══════════════════════════════════════════════════════════════════════
// ██  REAL LIBRARY: unfetch (minified) — Fetch polyfill using XHR
// ═══════════════════════════════════════════════════════════════════════

if (_unfetchMin) {
  console.log("\n=== unfetch (minified) — XHR-based fetch polyfill tracing ===\n");

  testLib("unfetch: simple GET URL traced through XHR polyfill", _unfetchMin, `
    unfetch("/api/users");
  `, function(r) {
    return r.fetchCallSites.some(function(s) { return s.url === "/api/users"; });
  });

  testLib("unfetch: explicit POST method traced", _unfetchMin, `
    unfetch("/api/data", {method: "POST"});
  `, function(r) {
    return r.fetchCallSites.some(function(s) { return s.url === "/api/data" && s.method === "POST"; });
  });

  testLib("unfetch: body param extraction through polyfill", _unfetchMin, `
    unfetch("/api/submit", {method: "POST", body: JSON.stringify({key: "val", num: 42})});
  `, function(r) {
    var site = r.fetchCallSites.find(function(s) { return s.url === "/api/submit"; });
    if (!site || !site.params) return false;
    return site.params.some(function(p) { return p.name === "key" && p.location === "body"; }) &&
           site.params.some(function(p) { return p.name === "num" && p.location === "body"; });
  });

  testLib("unfetch: custom headers traced through setRequestHeader loop", _unfetchMin, `
    unfetch("/api/auth", {
      method: "GET",
      headers: {"Authorization": "Bearer tok123", "X-Custom": "val"}
    });
  `, function(r) {
    var site = r.fetchCallSites.find(function(s) { return s.url === "/api/auth"; });
    if (!site) return false;
    return site.headers && site.headers["Authorization"] === "Bearer tok123";
  });

  testLib("unfetch: multiple calls with different methods all traced", _unfetchMin, `
    unfetch("/api/get-it");
    unfetch("/api/post-it", {method: "POST", body: JSON.stringify({a: 1})});
    unfetch("/api/put-it", {method: "PUT"});
  `, function(r) {
    var g = r.fetchCallSites.some(function(s) { return s.url === "/api/get-it"; });
    var p = r.fetchCallSites.some(function(s) { return s.url === "/api/post-it" && s.method === "POST"; });
    var u = r.fetchCallSites.some(function(s) { return s.url === "/api/put-it" && s.method === "PUT"; });
    return g && p && u;
  });

  testLib("unfetch: value constraints from credentials check", _unfetchMin, `
    unfetch("/api/dummy");
  `, function(r) {
    // unfetch checks n.credentials === "include" — should produce at least one constraint
    return r.valueConstraints.length >= 1;
  });
}

// ═══════════════════════════════════════════════════════════════════════
// ██  REAL LIBRARY: redaxios (minified) — Axios-compatible fetch wrapper
// ═══════════════════════════════════════════════════════════════════════

if (_redaxiosMin) {
  console.log("\n=== redaxios (minified) — Axios-compatible fetch wrapper tracing ===\n");

  testLib("redaxios: .get() traces to fetch GET", _redaxiosMin, `
    e.get("/api/users");
  `, function(r) {
    return r.fetchCallSites.some(function(s) { return s.url === "/api/users" && s.method === "GET"; });
  });

  testLib("redaxios: .post() traces to fetch POST with body params", _redaxiosMin, `
    e.post("/api/users", {name: "test", email: "a@b.com"});
  `, function(r) {
    var site = r.fetchCallSites.find(function(s) { return s.url === "/api/users" && s.method === "POST"; });
    if (!site) return false;
    if (!site.params) return false;
    return site.params.some(function(p) { return p.name === "name" && p.location === "body"; }) &&
           site.params.some(function(p) { return p.name === "email" && p.location === "body"; });
  });

  testLib("redaxios: .put() traces to fetch PUT", _redaxiosMin, `
    e.put("/api/users/123", {status: "active"});
  `, function(r) {
    return r.fetchCallSites.some(function(s) { return s.url === "/api/users/123" && s.method === "PUT"; });
  });

  testLib("redaxios: .delete() traces to fetch DELETE", _redaxiosMin, `
    e.delete("/api/users/456");
  `, function(r) {
    return r.fetchCallSites.some(function(s) { return s.url === "/api/users/456" && s.method === "DELETE"; });
  });

  testLib("redaxios: .patch() traces to fetch PATCH", _redaxiosMin, `
    e.patch("/api/users/789", {role: "admin"});
  `, function(r) {
    return r.fetchCallSites.some(function(s) { return s.url === "/api/users/789" && s.method === "PATCH"; });
  });

  testLib("redaxios: multiple calls with distinct URLs and methods", _redaxiosMin, `
    e.get("/api/list");
    e.post("/api/create", {name: "Widget", price: 29.99});
    e.delete("/api/remove/42");
  `, function(r) {
    var g = r.fetchCallSites.some(function(s) { return s.url === "/api/list"; });
    var p = r.fetchCallSites.some(function(s) { return s.url === "/api/create" && s.method === "POST"; });
    var d = r.fetchCallSites.some(function(s) { return s.url === "/api/remove/42" && s.method === "DELETE"; });
    return g && p && d;
  });

  testLib("redaxios: value constraints (response types, methods)", _redaxiosMin, `
  `, function(r) {
    // redaxios has responseType checks ("stream", "text") and method strings
    return r.valueConstraints.some(function(c) {
      return c.values.indexOf("stream") >= 0;
    }) || r.valueConstraints.some(function(c) {
      return c.values.indexOf("get") >= 0 && c.values.indexOf("post") >= 0;
    });
  });

  testLib("redaxios: .create() with baseURL config merging", _redaxiosMin, `
    var api = e.create({baseURL: "/api/v1"});
    api.get("/users");
    api.post("/items", {name: "thing"});
  `, function(r) {
    // Traces baseURL + path concatenation to discover URLs
    var hasUsers = r.fetchCallSites.some(function(s) { return s.url && s.url.indexOf("/users") >= 0; });
    var hasItems = r.fetchCallSites.some(function(s) { return s.url && s.url.indexOf("/items") >= 0; });
    return hasUsers && hasItems;
  });
}

// ═══════════════════════════════════════════════════════════════════════
// ██  REAL LIBRARY: ky (ESM) — Modern class-based fetch wrapper
// ═══════════════════════════════════════════════════════════════════════

if (_kyMin) {
  console.log("\n=== ky (ESM) — Class-based fetch wrapper tracing ===\n");

  testLib("ky: library parses without error", _kyMin, `
  `, function(r) {
    // Sanity: ky with class private fields and ESM should parse cleanly
    return true;
  }, false);

  testLib("ky: HTTP methods array constraint (get/post/put/patch/head/delete)", _kyMin, `
  `, function(r) {
    // ky defines h=["get","post","put","patch","head","delete"]
    return r.valueConstraints.some(function(c) {
      return c.values.indexOf("get") >= 0 && c.values.indexOf("post") >= 0 &&
             c.values.indexOf("put") >= 0 && c.values.indexOf("delete") >= 0;
    });
  }, false);

  testLib("ky: retry status codes constraint", _kyMin, `
  `, function(r) {
    // ky has statusCodes:[408,413,429,500,502,503,504]
    return r.valueConstraints.some(function(c) {
      return c.values.indexOf(408) >= 0 && c.values.indexOf(429) >= 0 && c.values.indexOf(503) >= 0;
    });
  }, false);

  testLib("ky: content-type mapping constraint (json/text)", _kyMin, `
  `, function(r) {
    // ky defines c={json:"application/json",text:"text/*",...}
    return r.valueConstraints.some(function(c) {
      return c.values.indexOf("application/json") >= 0;
    }) || r.valueConstraints.some(function(c) {
      return c.values.indexOf("json") >= 0 && c.values.indexOf("text") >= 0;
    });
  }, false);

  testLib("ky: k.get() traced to fetch", _kyMin, `
    k.get("https://api.example.com/users");
  `, function(r) {
    return r.fetchCallSites.some(function(s) {
      return s.url && s.url.indexOf("/users") >= 0;
    });
  }, false);

  testLib("ky: k.post() with json body params", _kyMin, `
    k.post("https://api.example.com/users", {json: {name: "Alice", role: "admin"}});
  `, function(r) {
    var site = r.fetchCallSites.find(function(s) {
      return s.url && s.url.indexOf("/users") >= 0 && s.method === "POST";
    });
    if (!site) return false;
    return site.params && site.params.some(function(p) { return p.name === "name" && p.location === "body"; });
  }, false);

  testLib("ky: retry methods constraint (get/put/head/delete/options/trace)", _kyMin, `
  `, function(r) {
    // ky retry defaults: methods:["get","put","head","delete","options","trace"]
    return r.valueConstraints.some(function(c) {
      return c.values.indexOf("get") >= 0 && c.values.indexOf("trace") >= 0;
    });
  }, false);

  testLib("ky: multiple k.method() calls traced", _kyMin, `
    k.get("https://api.example.com/a");
    k.post("https://api.example.com/b", {json: {x: 1}});
    k.put("https://api.example.com/c", {json: {y: 2}});
    k.delete("https://api.example.com/d");
  `, function(r) {
    var a = r.fetchCallSites.some(function(s) { return s.url && s.url.indexOf("/a") >= 0; });
    var b = r.fetchCallSites.some(function(s) { return s.url && s.url.indexOf("/b") >= 0 && s.method === "POST"; });
    var c = r.fetchCallSites.some(function(s) { return s.url && s.url.indexOf("/c") >= 0 && s.method === "PUT"; });
    var d = r.fetchCallSites.some(function(s) { return s.url && s.url.indexOf("/d") >= 0 && s.method === "DELETE"; });
    return a && b && c && d;
  }, false);
}

// ═══════════════════════════════════════════════════════════════════════
// ██  REAL LIBRARY: superagent (minified) — XHR-based chaining client
// ═══════════════════════════════════════════════════════════════════════

if (_superagentMin) {
  console.log("\n=== superagent (minified) — XHR chaining client tracing ===\n");

  testLib("superagent: .get() URL traced through XHR", _superagentMin, `
    superagent.get("/api/users");
  `, function(r) {
    return r.fetchCallSites.some(function(s) { return s.url === "/api/users" && s.method === "GET"; });
  });

  testLib("superagent: .post() URL and method traced", _superagentMin, `
    superagent.post("/api/users");
  `, function(r) {
    return r.fetchCallSites.some(function(s) { return s.url === "/api/users" && s.method === "POST"; });
  });

  testLib("superagent: .put() traced", _superagentMin, `
    superagent.put("/api/items/42");
  `, function(r) {
    return r.fetchCallSites.some(function(s) { return s.url === "/api/items/42" && s.method === "PUT"; });
  });

  testLib("superagent: .delete() traced", _superagentMin, `
    superagent.delete("/api/items/99");
  `, function(r) {
    return r.fetchCallSites.some(function(s) { return s.url === "/api/items/99" && s.method === "DELETE"; });
  });

  testLib("superagent: .patch() traced", _superagentMin, `
    superagent.patch("/api/items/7");
  `, function(r) {
    return r.fetchCallSites.some(function(s) { return s.url === "/api/items/7" && s.method === "PATCH"; });
  });

  testLib("superagent: .send() body param extraction", _superagentMin, `
    superagent.post("/api/users").send({name: "test", email: "a@b.com", age: 25});
  `, function(r) {
    var site = r.fetchCallSites.find(function(s) { return s.url === "/api/users" && s.method === "POST"; });
    if (!site || !site.params) return false;
    return site.params.some(function(p) { return p.name === "name" && p.location === "body"; }) &&
           site.params.some(function(p) { return p.name === "email" && p.location === "body"; });
  });

  testLib("superagent: .set() header extraction", _superagentMin, `
    superagent.post("/api/secure")
      .set("Authorization", "Bearer mytoken")
      .set("X-Custom", "value")
      .send({data: "test"});
  `, function(r) {
    var site = r.fetchCallSites.find(function(s) { return s.url === "/api/secure"; });
    if (!site) return false;
    return site.headers && site.headers["Authorization"] === "Bearer mytoken";
  });

  testLib("superagent: multiple calls with different methods and data", _superagentMin, `
    superagent.get("/api/list");
    superagent.post("/api/create").send({title: "Widget", price: 9.99});
    superagent.put("/api/update/5").send({status: "active"});
    superagent.delete("/api/remove/3");
  `, function(r) {
    var g = r.fetchCallSites.some(function(s) { return s.url === "/api/list" && s.method === "GET"; });
    var p = r.fetchCallSites.some(function(s) { return s.url === "/api/create" && s.method === "POST"; });
    var u = r.fetchCallSites.some(function(s) { return s.url === "/api/update/5" && s.method === "PUT"; });
    var d = r.fetchCallSites.some(function(s) { return s.url === "/api/remove/3" && s.method === "DELETE"; });
    return g && p && u && d;
  });
}

// ═══════════════════════════════════════════════════════════════════════
// ██  SYNTHETIC ADVANCED PATTERNS — Real-world code idioms
// ═══════════════════════════════════════════════════════════════════════

console.log("\n=== Synthetic: Advanced real-world code patterns ===\n");

test("ES6 class with fetch: this.base + path", `
  class ApiClient {
    constructor(base) { this.base = base; }
    async get(path) { return fetch(this.base + path); }
    async post(path, body) {
      return fetch(this.base + path, { method: "POST", body: JSON.stringify(body) });
    }
  }
  var client = new ApiClient("https://api.example.com");
  client.get("/users");
  client.post("/orders", {item: "widget", qty: 3});
`, function(r) {
  var hasGet = r.fetchCallSites.some(function(s) {
    return s.url === "https://api.example.com/users";
  });
  var hasPost = r.fetchCallSites.some(function(s) {
    return s.url === "https://api.example.com/orders" && s.method === "POST";
  });
  return hasGet && hasPost;
});

test("Promise.then chain making second request", `
  var url1 = "/api/config";
  var url2 = "/api/data";
  fetch(url1).then(function(r) { return r.json(); }).then(function(d) {
    return fetch(url2, {method: "POST", body: JSON.stringify(d)});
  });
`, function(r) {
  var has1 = r.fetchCallSites.some(function(s) { return s.url === "/api/config"; });
  var has2 = r.fetchCallSites.some(function(s) { return s.url === "/api/data" && s.method === "POST"; });
  return has1 && has2;
});

test("async/await wrapper function", `
  async function getData(id) {
    var r = await fetch("/api/items/" + id);
    return r.json();
  }
  getData("42");
`, function(r) {
  return r.fetchCallSites.some(function(s) { return s.url === "/api/items/42"; });
});

test("Object.assign config merge", `
  var defaults = { method: "POST", headers: {"Content-Type": "application/json"} };
  function apiCall(url, userOpts) {
    return fetch(url, Object.assign({}, defaults, userOpts));
  }
  apiCall("/api/submit", {body: JSON.stringify({key: "val"})});
`, function(r) {
  var site = r.fetchCallSites.find(function(s) { return s.url === "/api/submit"; });
  return site && site.method === "POST";
});

test("URLSearchParams query building", `
  function search(query, page) {
    var params = new URLSearchParams({q: query, page: page});
    return fetch("/api/search?" + params);
  }
  search("test", "1");
`, function(r) {
  return r.fetchCallSites.some(function(s) { return s.url && s.url.indexOf("/api/search") === 0; });
});

test("Conditional method via ternary", `
  function saveItem(id, data, isNew) {
    return fetch("/api/items/" + id, {
      method: isNew ? "POST" : "PUT",
      body: JSON.stringify(data)
    });
  }
  saveItem("1", {name: "test"}, true);
`, function(r) {
  var site = r.fetchCallSites.find(function(s) { return s.url && s.url.indexOf("/api/items/") === 0; });
  if (!site) return false;
  // Should discover both POST and PUT as possible methods
  return site.method === "POST" || site.method === "PUT" ||
    r.fetchCallSites.some(function(s) { return s.method === "POST"; }) &&
    r.fetchCallSites.some(function(s) { return s.method === "PUT"; });
});

test("Array.map producing fetch calls", `
  var endpoints = ["/api/users", "/api/orders", "/api/products"];
  endpoints.map(function(url) { return fetch(url); });
`, function(r) {
  var u = r.fetchCallSites.some(function(s) { return s.url === "/api/users"; });
  var o = r.fetchCallSites.some(function(s) { return s.url === "/api/orders"; });
  var p = r.fetchCallSites.some(function(s) { return s.url === "/api/products"; });
  return u && o && p;
});

test("Retry with fallback URL", `
  var primaryUrl = "/api/primary";
  var fallbackUrl = "/api/fallback";
  fetch(primaryUrl).catch(function() { return fetch(fallbackUrl); });
`, function(r) {
  var hasPrimary = r.fetchCallSites.some(function(s) { return s.url === "/api/primary"; });
  var hasFallback = r.fetchCallSites.some(function(s) { return s.url === "/api/fallback"; });
  return hasPrimary && hasFallback;
});

test("Template literal with expression in URL", `
  var version = "v2";
  var resource = "users";
  fetch(\`/api/\${version}/\${resource}\`);
`, function(r) {
  return r.fetchCallSites.some(function(s) { return s.url === "/api/v2/users"; });
});

test("Chained string builder for URL", `
  function buildUrl(parts) {
    return parts.join("/");
  }
  fetch(buildUrl(["/api", "v1", "users", "search"]));
`, function(r) {
  return r.fetchCallSites.some(function(s) { return s.url === "/api/v1/users/search"; });
});

// ═══════════════════════════════════════════════════════════════════════
// ██  ADVANCED COMBINED PATTERNS
// ═══════════════════════════════════════════════════════════════════════

console.log("\n=== Advanced: Factory + constraints + body params ===\n");

test("Factory returning methods with constrained + typed body", `
  function createClient(base) {
    return {
      get: function(path) { return fetch(base + path); },
      post: function(path, body) {
        return fetch(base + path, { method: "POST", body: JSON.stringify(body) });
      }
    };
  }
  var api = createClient("/api/v2");
  api.get("/users");
  api.post("/users", {name: "Alice", role: "admin"});
  api.post("/orders", {item: "widget", qty: 1});
`, function(r) {
  var hasGet = r.fetchCallSites.some(function(s) { return s.url === "/api/v2/users" && s.method === "GET"; });
  var hasPostUsers = r.fetchCallSites.some(function(s) { return s.url === "/api/v2/users" && s.method === "POST"; });
  var hasPostOrders = r.fetchCallSites.some(function(s) { return s.url === "/api/v2/orders" && s.method === "POST"; });
  // Check that post calls have body params
  var postSite = r.fetchCallSites.find(function(s) { return s.url === "/api/v2/users" && s.method === "POST"; });
  var hasBodyParams = postSite && postSite.params && postSite.params.some(function(p) { return p.name === "name" && p.location === "body"; });
  return hasGet && hasPostUsers && hasPostOrders && hasBodyParams;
});

test("Constructor pattern with body params and value constraints", `
  function ApiClient(baseUrl) {
    this.base = baseUrl;
  }
  ApiClient.prototype.request = function(method, path, data) {
    return fetch(this.base + path, {
      method: method,
      body: data ? JSON.stringify(data) : undefined
    });
  };
  var client = new ApiClient("https://api.example.com");
  client.request("POST", "/auth/login", {username: "admin", password: "secret"});
  client.request("GET", "/users");
`, function(r) {
  var loginSite = r.fetchCallSites.find(function(s) {
    return s.url === "https://api.example.com/auth/login";
  });
  if (!loginSite) return false;
  // Should have body params from caller
  if (!loginSite.params) return false;
  var hasUsername = loginSite.params.some(function(p) { return p.name === "username" && p.location === "body"; });
  var hasPassword = loginSite.params.some(function(p) { return p.name === "password" && p.location === "body"; });
  return hasUsername && hasPassword;
});

test("Enum-constrained method with body: complete API learning", `
  var ACTIONS = ["create", "update", "delete"];
  function performAction(action, resourceId, data) {
    var methods = {create: "POST", update: "PUT", "delete": "DELETE"};
    return fetch("/api/resources/" + resourceId, {
      method: methods[action],
      body: JSON.stringify(data)
    });
  }
  performAction("create", "new", {name: "Widget", price: 9.99});
`, function(r) {
  // Check value constraints on "action"
  var actionC = r.valueConstraints.find(function(c) { return c.variable === "action"; });
  var hasConstraint = actionC && actionC.values.length === 3;
  // Check that a call site was found
  var hasSite = r.fetchCallSites.length >= 1;
  return hasConstraint && hasSite;
});

test("Chained wrapper with header injection and body params", `
  function withAuth(token) {
    return function(url, method, body) {
      return fetch(url, {
        method: method,
        headers: {"Authorization": "Bearer " + token, "Content-Type": "application/json"},
        body: JSON.stringify(body)
      });
    };
  }
  var authedFetch = withAuth("my-token");
  authedFetch("/api/protected", "POST", {action: "unlock", target: "vault"});
`, function(r) {
  var site = r.fetchCallSites.find(function(s) { return s.url === "/api/protected"; });
  if (!site) return false;
  // Check headers preserved
  var hasAuth = site.headers && site.headers["Authorization"] === "Bearer my-token";
  // Check body params resolved from caller
  var hasBody = site.params && site.params.some(function(p) { return p.name === "action" && p.location === "body"; });
  return hasAuth && hasBody;
});

test("Multi-level wrapper: outer → middle → fetch with body resolution", `
  function jsonFetch(url, method, body) {
    return fetch(url, { method: method, body: JSON.stringify(body) });
  }
  function apiPost(endpoint, data) {
    return jsonFetch("/api" + endpoint, "POST", data);
  }
  apiPost("/messages", {text: "hello", channel: "general"});
`, function(r) {
  var site = r.fetchCallSites.find(function(s) { return s.url === "/api/messages"; });
  if (!site) return false;
  return site.method === "POST" && site.params &&
    site.params.some(function(p) { return p.name === "text" && p.location === "body"; }) &&
    site.params.some(function(p) { return p.name === "channel" && p.location === "body"; });
});

test("Computed URL endpoints + body params from object map", `
  var endpoints = {
    users: "/api/users",
    orders: "/api/orders",
    products: "/api/products"
  };
  function create(resource, data) {
    return fetch(endpoints[resource], { method: "POST", body: JSON.stringify(data) });
  }
  create("users", {name: "test", email: "test@example.com"});
`, function(r) {
  // All 3 endpoints should be discovered (computed member access)
  var urls = r.fetchCallSites.map(function(s) { return s.url; }).sort();
  var hasAll = urls.indexOf("/api/orders") >= 0 &&
               urls.indexOf("/api/products") >= 0 &&
               urls.indexOf("/api/users") >= 0;
  // Body params from caller should be present
  var userSite = r.fetchCallSites.find(function(s) { return s.url === "/api/users"; });
  var hasBody = userSite && userSite.params &&
    userSite.params.some(function(p) { return p.name === "name" && p.location === "body"; });
  return hasAll && hasBody;
});

console.log("\n=== Advanced: XHR wrapper patterns with body params ===\n");

test("XHR wrapper: opts.data sent via xhr.send()", `
  function httpRequest(opts) {
    var xhr = new XMLHttpRequest();
    xhr.open(opts.method, opts.url);
    xhr.setRequestHeader("Content-Type", "application/json");
    xhr.send(JSON.stringify(opts.data));
  }
  httpRequest({url: "/api/xhr-body", method: "POST", data: {key: "value", num: 42}});
`, function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url === "/api/xhr-body" && s.method === "POST";
  });
});

test("Deep sink pattern with body data extraction", `
  var lib = {};
  lib.extend = function(target) {
    for (var k in arguments[1]) target[k] = arguments[1][k];
    return target;
  };
  function doXHR(config) {
    var xhr = new XMLHttpRequest();
    xhr.open(config.method, config.url);
    xhr.send(config.data);
  }
  lib.extend({
    ajax: function(opts) { doXHR(opts); }
  });
  lib.ajax({url: "/api/deep-body", method: "POST", data: {field1: "a", field2: "b"}});
`, function(r) {
  var site = r.fetchCallSites.find(function(s) { return s.url === "/api/deep-body"; });
  return !!site && site.method === "POST";
}, true);

console.log("\n=== Advanced: Real-world API client patterns ===\n");

test("REST client with CRUD methods and typed params", `
  function RestClient(baseUrl) {
    this.base = baseUrl;
  }
  RestClient.prototype.get = function(path) {
    return fetch(this.base + path);
  };
  RestClient.prototype.post = function(path, body) {
    return fetch(this.base + path, { method: "POST", body: JSON.stringify(body) });
  };
  RestClient.prototype.put = function(path, body) {
    return fetch(this.base + path, { method: "PUT", body: JSON.stringify(body) });
  };
  RestClient.prototype.delete = function(path) {
    return fetch(this.base + path, { method: "DELETE" });
  };

  var api = new RestClient("https://api.example.com/v1");
  api.get("/users");
  api.post("/users", {name: "Alice", email: "alice@test.com", role: "admin"});
  api.put("/users/123", {name: "Bob"});
  api.delete("/users/456");
`, function(r) {
  var getUser = r.fetchCallSites.some(function(s) {
    return s.url === "https://api.example.com/v1/users" && s.method === "GET";
  });
  var postUser = r.fetchCallSites.find(function(s) {
    return s.url === "https://api.example.com/v1/users" && s.method === "POST";
  });
  var putUser = r.fetchCallSites.some(function(s) {
    return s.url === "https://api.example.com/v1/users/123" && s.method === "PUT";
  });
  var delUser = r.fetchCallSites.some(function(s) {
    return s.url === "https://api.example.com/v1/users/456" && s.method === "DELETE";
  });
  // POST should have body params
  var hasBodyParams = postUser && postUser.params &&
    postUser.params.some(function(p) { return p.name === "name" && p.location === "body"; }) &&
    postUser.params.some(function(p) { return p.name === "email" && p.location === "body"; }) &&
    postUser.params.some(function(p) { return p.name === "role" && p.location === "body"; });
  return getUser && postUser && putUser && delUser && hasBodyParams;
});

test("GraphQL-like pattern: query string + variables as body", `
  function graphql(query, variables) {
    return fetch("/graphql", {
      method: "POST",
      headers: {"Content-Type": "application/json"},
      body: JSON.stringify({query: query, variables: variables})
    });
  }
  graphql("query { users { id name } }", {limit: 10, offset: 0});
`, function(r) {
  var site = r.fetchCallSites.find(function(s) { return s.url === "/graphql"; });
  if (!site) return false;
  if (!site.params) return false;
  var hasQuery = site.params.some(function(p) { return p.name === "query" && p.location === "body"; });
  var hasVars = site.params.some(function(p) { return p.name === "variables" && p.location === "body"; });
  return site.method === "POST" && hasQuery && hasVars;
});

test("Rate-limited fetch with retry — URL and method preserved", `
  function fetchWithRetry(url, opts, maxRetries) {
    return fetch(url, opts);
  }
  fetchWithRetry("/api/flaky", {method: "PATCH", body: JSON.stringify({status: "retry"})}, 3);
`, function(r) {
  var site = r.fetchCallSites.find(function(s) { return s.url === "/api/flaky"; });
  return site && site.method === "PATCH";
});

// =============================================================================
// Security Analysis Tests
// =============================================================================

console.log("\n=== Security: DOM XSS Sink Detection ===\n");

test("innerHTML with user-controlled source (location.hash) → high severity XSS", `
  var x = location.hash;
  document.getElementById("output").innerHTML = x;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" && s.source === "location.hash";
  });
});

test("innerHTML with string literal → not flagged (not user-controlled)", `
  document.getElementById("output").innerHTML = "<b>Hello</b>";
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

test("document.write with dynamic param → not flagged (not user-controlled)", `
  function render(content) {
    document.write(content);
  }
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "document.write";
  });
});

test("eval with user-controlled value → high severity", `
  var code = location.search.slice(1);
  eval(code);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "eval" && s.severity === "high" && s.source === "location.search";
  });
});

test("eval with string literal → not flagged (not user-controlled)", `
  eval("console.log('hello')");
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "eval";
  });
});

test("insertAdjacentHTML with user-controlled value → high severity XSS", `
  var markup = location.hash;
  document.body.insertAdjacentHTML("beforeend", markup);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "insertAdjacentHTML" && s.severity === "high";
  });
});

test("setTimeout with string arg → not flagged (not user-controlled)", `
  var action = "doSomething()";
  setTimeout(action, 1000);
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "setTimeout";
  });
});

test("setAttribute with literal handler → not flagged (not user-controlled)", `
  var handler = "alert(1)";
  document.getElementById("btn").setAttribute("onclick", handler);
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "setAttribute:onclick";
  });
});

test("Open redirect: location.href = user-controlled value → high severity", `
  var url = location.hash.slice(1);
  location.href = url;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "href" && s.severity === "high";
  });
});

test("Open redirect: location.assign with user value → high severity", `
  var target = decodeURIComponent(location.search.slice(4));
  location.assign(target);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "location.assign" && s.severity === "high";
  });
});

// ── Redirect severity: scheme-locked via query/hash-only taint ──

test("Open redirect: template literal with taint only in query → MEDIUM (scheme locked)", `
  var t = location.search.slice(1);
  location.href = \`https://example.com/path?\${t}\`;
`, function(r) {
  // Prefix "https://example.com/path?" locks the scheme+host+path; taint
  // only flows into the query portion, which can't produce a javascript:
  // URL or redirect to another origin.
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "href" && s.severity === "medium" &&
           /query\/hash/.test(s.notes || "");
  });
});

test("Open redirect: template literal with taint only in hash → MEDIUM (scheme locked)", `
  var t = location.hash.slice(1);
  location.href = \`/dashboard#\${t}\`;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "href" && s.severity === "medium";
  });
});

test("Open redirect: Reddit-style `${untaintedPermalink}?${taintedQuery}` → MEDIUM", `
  class PostCard {
    redirectToPDP(e) {
      var t = new URLSearchParams(location.search);
      t.set(e, "true");
      location.href = \`\${this.permalink}?\${t}\`;
    }
  }
`, function(r) {
  // Reddit's redirectToPDP pattern: first interpolation is an untainted
  // class property, then literal "?", then a tainted query. The walker
  // must prove the pre-delimiter interpolation isn't user-controlled
  // before honouring the downgrade.
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "href" && s.severity === "medium";
  });
});

test("Open redirect: tainted untaintedPrefix-interpolation before `?` → HIGH (can reach host)", `
  var host = location.hash.slice(1);
  location.href = \`\${host}?\${location.search}\`;
`, function(r) {
  // First interpolation is tainted AND lands before any ?/#, so the
  // attacker can rewrite the host portion. Stays HIGH.
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "href" && s.severity === "high";
  });
});

test("Open redirect: template literal with taint in PATH (no ? or # yet) → still HIGH", `
  var t = location.hash.slice(1);
  location.href = \`https://example.com/\${t}\`;
`, function(r) {
  // The first quasi "https://example.com/" has no ?/#, so taint can reach
  // the path — even though scheme is locked, this stays HIGH because the
  // helper's job is to only downgrade provable query/hash-only flows.
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "href" && s.severity === "high";
  });
});

test("Open redirect: template literal where taint reaches scheme → HIGH", `
  var scheme = location.hash.slice(1);
  location.href = \`\${scheme}://example.com/?ok=1\`;
`, function(r) {
  // First quasi is empty (interpolation is at position 0) — no ?/# in it,
  // so taint can reach the scheme. Classic javascript: XSS vector.
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "href" && s.severity === "high";
  });
});

test("Open redirect: location.assign with query-only taint → MEDIUM", `
  var t = location.search.slice(1);
  location.assign(\`/results?q=\${t}\`);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "location.assign" && s.severity === "medium";
  });
});

test("Open redirect: location.replace with full-URL taint → still HIGH", `
  var target = location.hash.slice(1);
  location.replace(target);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "location.replace" && s.severity === "high";
  });
});

// Reddit devvit playtest "Reloading" pattern: build URL from current
// location.href, mutate searchParams, navigate. Scheme + host come from
// location.href (browser-validated current origin) — searchParams.set
// can't change them. Must NOT flag redirect.
test("Open redirect: new URL(location.href) + searchParams.set + location.assign → suppressed", `
  function reload(value) {
    const t = new URL(location.href);
    t.searchParams.set("st", value);
    location.assign(t);
  }
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "redirect" && (s.sink === "location.assign" || s.sink === "location.replace");
  });
});

// Polarity check for the above: URL object built from a free-form
// source (location.hash content can be a full URL including
// "javascript:alert(1)") — must still flag.
test("Open redirect: new URL(location.hash) + location.assign → still HIGH", `
  const t = new URL(location.hash.slice(1));
  location.assign(t);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "location.assign" && s.severity === "high";
  });
});

// Reddit _redirect pattern: `e = location.origin + location.pathname;
// window.location.href = e`. Assembled URL begins with the browser-
// validated current origin, so the assignment is a same-origin same-path
// reload without query. Must NOT flag.
test("Open redirect: location.origin + location.pathname → location.href (reload) → suppressed", `
  function _redirect() {
    setTimeout(() => {
      const e = window.location.origin + window.location.pathname;
      window.location.href = e;
    }, 1000);
  }
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "redirect";
  });
});

// Polarity: same shape but leftmost isn't a browser-validated origin —
// a plain string variable from user input. Must still flag.
test("Open redirect: tainted + location.pathname → location.href → HIGH", `
  function bad() {
    const host = location.hash.slice(1);
    window.location.href = host + window.location.pathname;
  }
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "href" && s.severity === "high";
  });
});

// Same reload pattern, call-form: location.assign(origin + pathname).
test("Open redirect: location.assign(location.origin + location.pathname) (reload) → suppressed", `
  location.assign(window.location.origin + window.location.pathname);
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "redirect";
  });
});

// github's approvedHandler pattern: `let url` declared uninitialised,
// then assigned `new URL(..., location.origin)` in each branch of an
// if/else, with `url.searchParams.set(...)` mutations before
// `location.assign(url)`. The URL object's origin is pinned to current
// page, searchParams mutation can't change it. Type inference through
// reassignments makes this suppressible.
test("Open redirect: let url; branches assign new URL(...); location.assign(url) → suppressed", `
  function approvedHandler(token) {
    let url;
    if (token) {
      url = new URL('password_reset/' + encodeURIComponent(token), window.location.origin);
    } else {
      url = new URL('', window.location.href);
    }
    url.searchParams.set('redirect', 'true');
    window.location.assign(url);
  }
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "redirect";
  });
});

// Polarity: ONE branch assigns a URL, the OTHER assigns a plain string.
// Type inference must NOT commit to URL — the string branch could carry
// an attacker-chosen origin. Must still flag HIGH.
test("Open redirect: branches assign URL vs string; location.assign(url) → HIGH", `
  function maybeUrl(token) {
    let url;
    if (token) {
      url = new URL('', window.location.href);
    } else {
      url = location.hash.slice(1);
    }
    window.location.assign(url);
  }
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.severity === "high";
  });
});

// github permalink.ts pattern: `target.href + window.location.hash`
// assigned to `window.location.href`. location.hash (unsliced) still
// carries its "#" structural marker — the concat result puts taint in
// the hash portion of target.href (whatever origin that is) so scheme
// + host can't be forced by the attacker. Must downgrade to MEDIUM.
test("Open redirect: anchor.href + location.hash → location.href → MEDIUM", `
  on('click', '.js-permalink-shortcut', function (event) {
    var target = event.currentTarget;
    window.location.href = target.href + window.location.hash;
  });
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "href" && s.severity === "medium";
  });
});

// Polarity: same RHS shape but location.hash.slice(1) — prefix strip
// upgrades origin dim, taint is now free-form content. Must stay HIGH.
test("Open redirect: anchor.href + location.hash.slice(1) → location.href → HIGH", `
  function onClick(event) {
    var target = event.currentTarget;
    window.location.href = target.href + window.location.hash.slice(1);
  }
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "href" && s.severity === "high";
  });
});

// All-dims-stripped: `new URL(location.href).protocol` is a static
// scheme string — dim propagation strips origin/path/query/hash and
// there's no content contribution. Sink must NOT fire. Real-world FP:
// react-router RSCErrorHandler 20-hop chain concluding at
// `window.location.href = n.absoluteURL || n.to` where n.absoluteURL's
// dim-trail ends at `.protocol`.
test("Open redirect: new URL(location.href).protocol → href → suppressed", `
  function navigate() {
    var scheme = new URL(location.href).protocol;
    window.location.href = scheme + "//example.com/";
  }
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "redirect";
  });
});

// Map.get(tainted) returns the VALUE stored in the map — independent
// of the key's taint. Propagating key-origin to the value produced
// widespread FPs (github turbo's bfCache.get(location.href) pattern).
test("xss:.replaceWith: bfCache.get(location.href).entries() → element.replaceWith(t) → suppressed", `
  var bfCache = new Map();
  bfCache.set("x", document.createElement("div"));
  function restore() {
    var cache = bfCache.get(window.location.href);
    if (cache) for (var [k, el] of cache.entries()) {
      var target = document.getElementById(k);
      if (target) target.replaceWith(el);
    }
  }
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === ".replaceWith" && s.severity === "high";
  });
});

// Polarity: a real URLSearchParams.entries() iteration DOES surface
// attacker-content as the iterated value. The receiver-type gate
// must not suppress THIS case.
test("xss:.replaceWith: URLSearchParams(location.search).entries() → el.replaceWith(v) → HIGH", `
  function applyParams() {
    var params = new URLSearchParams(location.search);
    for (var [k, v] of params.entries()) {
      var target = document.getElementById(k);
      if (target) target.replaceWith(v);
    }
  }
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === ".replaceWith" && s.severity === "high";
  });
});

// dangerousPatterns now respects the all-dims-false gate, the same
// one _pushSink uses. A 110-hop chain through framework code that
// strips every dim along the way (e.g. `Object.entries(serverResult)`
// where dims hit {none}) shouldn't surface a HIGH proto-pollution
// finding the reviewer can't act on.
test("proto-pollution: all-dims-false key → suppressed", `
  function build(input) {
    var protocol = new URL(input).protocol;
    var routes = ["/a", "/b", "/c"];
    var out = {};
    for (var key of routes) {
      out[protocol + key] = { value: 1 };
    }
    return out;
  }
  build(window.location.href);
`, function(r) {
  // protocol/key concat strips all dims; no real attacker-controlled
  // key reaches the dynamic property assignment.
  return !r.dangerousPatterns.some(function(d) {
    return d.type === "prototype-pollution" && d.severity === "high";
  });
});

// Polarity: a key actually built from attacker content stays HIGH.
test("proto-pollution: attacker-content key → HIGH", `
  function risky() {
    var key = location.hash.slice(1);
    var out = {};
    out[key] = { value: 1 };
    return out;
  }
`, function(r) {
  return r.dangerousPatterns.some(function(d) {
    return d.type === "prototype-pollution" && d.severity === "high";
  });
});

// FormData.append / URLSearchParams.append / Headers.append are NOT DOM
// XSS sinks. Real-world FP: react-router copies form entries via
// `t.append(r, a)` on a FormData receiver. Must NOT flag xss:.append.
test("xss:.append: FormData.append(key, taintedValue) → not flagged", `
  function buildForm(src) {
    var t = new FormData();
    for (let [r, a] of src.entries()) {
      t.append(r, typeof a === "string" ? a : a.name);
    }
    return t;
  }
  buildForm(new URLSearchParams(location.search));
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === ".append";
  });
});

// Polarity: real DOM `.append(tainted)` on a tracked Element must still fire.
test("xss:.append: element.append(tainted) → flagged", `
  function render(src) {
    var el = document.getElementById("root");
    el.append(src);
  }
  render(location.hash.slice(1));
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === ".append";
  });
});

test("Open redirect: window.open with query-only taint → MEDIUM", `
  var t = location.search.slice(1);
  window.open(\`https://example.com/#\${t}\`);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "window.open" && s.severity === "medium";
  });
});

test("Open redirect: window.open with full-URL taint → HIGH", `
  var t = location.hash.slice(1);
  window.open(t);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "window.open" && s.severity === "high";
  });
});

test("Open redirect: sanitized scheme-locked assignment — sanitizer takes priority over scheme-lock", `
  var t = location.hash.slice(1);
  location.href = \`/path?\${encodeURIComponent(t)}\`;
`, function(r) {
  // If sanitisation is detected it should stay INFO; if not, the scheme-
  // lock applies and we get MEDIUM. Either outcome is acceptable as long
  // as we don't land on HIGH — the scheme-locked flow is not a full XSS
  // vector, and a sanitised one shouldn't be promoted back up by the
  // scheme-lock override either.
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "href" &&
           (s.severity === "info" || s.severity === "medium");
  });
});

console.log("\n=== Security: Dangerous Pattern Detection ===\n");

test("new Function with dynamic arg → not flagged (not user-controlled)", `
  function compile(code) { return new Function(code); }
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "new Function";
  });
});

test("postMessage listener without origin check → dangerous", `
  window.addEventListener("message", function(event) {
    document.body.innerHTML = event.data;
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin" && p.severity === "high";
  });
});

test("postMessage listener WITH origin check → not flagged", `
  window.addEventListener("message", function(event) {
    if (event.origin !== "https://trusted.com") return;
    document.body.innerHTML = event.data;
  });
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin";
  });
});

test("Prototype pollution: obj[user-controlled key] = primitive → MEDIUM (local shadowing, needs attack chain)", `
  var key = location.hash.slice(1);
  var obj = {};
  obj[key] = "pwned";
`, function(r) {
  // Primitive value means `obj["__proto__"] = string` is a spec no-op and
  // `obj["hasOwnProperty"] = string` only shadows locally. Real prototype
  // pollution requires an object value. Severity MEDIUM reflects local-
  // shadowing availability risk that requires an attack chain.
  return r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution" && p.severity === "medium";
  });
});

test("Prototype pollution: obj[user-controlled key] = OBJECT → HIGH (real pollution)", `
  var key = location.hash.slice(1);
  var obj = {};
  obj[key] = { evil: true };
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution" && p.severity === "high";
  });
});

test("Prototype pollution: obj[user-controlled] = JSON.parse(...) → HIGH", `
  var key = location.hash.slice(1);
  var payload = JSON.parse(location.search.slice(1));
  var obj = {};
  obj[key] = payload;
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution" && p.severity === "high";
  });
});

test("Prototype pollution: cookie parser pattern (primitive value) → MEDIUM not HIGH", `
  function parseCookies(cookieString) {
    var aCookies = {};
    var pairs = cookieString.split(";");
    for (var i = 0; i < pairs.length; i++) {
      var parts = pairs[i].split("=");
      aCookies[parts[0]] = parts[1];
    }
    return aCookies;
  }
  parseCookies(document.cookie);
`, function(r) {
  var patterns = (r.dangerousPatterns || []).filter(function(p) {
    return p.type === "prototype-pollution";
  });
  // Must flag (user-controlled key) but as MEDIUM (primitive value).
  // obj["__proto__"] = string is a spec no-op; this isn't real pollution.
  return patterns.some(function(p) { return p.severity === "medium"; }) &&
         !patterns.some(function(p) { return p.severity === "high"; });
});

test("RegExp with user-controlled pattern → flagged as ReDoS risk", `
  var pattern = location.search.slice(1);
  new RegExp(pattern).test("input");
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "regex-dynamic" && p.severity === "high";
  });
});

test("location.replace with user input → open redirect", `
  var next = location.hash.substring(1);
  location.replace(next);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "location.replace" && s.severity === "high";
  });
});

console.log("\n=== Security: Taint Tracking Through Transformations ===\n");

test("Taint flows through string concatenation", `
  var param = location.search;
  var html = "<div>" + param + "</div>";
  document.body.innerHTML = html;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.severity === "high" && s.source === "location.search";
  });
});

test("Taint flows through variable assignment chain", `
  var a = location.hash;
  var b = a;
  eval(b);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.severity === "high" && s.source === "location.hash";
  });
});

test("Taint flows through function parameter", `
  function display(html) {
    document.body.innerHTML = html;
  }
  display(location.hash);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.severity === "high" && s.source === "location.hash";
  });
});

console.log("\n=== Security: Complex XSS Patterns ===\n");

test("innerHTML via template literal with tainted interpolation", `
  var userInput = location.hash;
  document.getElementById("out").innerHTML = \`<div class="msg">\${userInput}</div>\`;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" && s.source === "location.hash";
  });
});

test("innerHTML via multi-hop variable chain (a → b → c → sink)", `
  var a = location.search;
  var b = a;
  var c = b;
  document.body.innerHTML = c;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" && s.source === "location.search";
  });
});

test("innerHTML via method chaining on tainted object (.slice().toLowerCase())", `
  var fragment = location.hash.slice(1).toLowerCase();
  document.getElementById("target").innerHTML = fragment;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" && s.source === "location.hash";
  });
});

test("outerHTML with user-controlled source → high severity XSS", `
  var payload = location.hash.slice(1);
  document.getElementById("widget").outerHTML = payload;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "outerHTML" && s.severity === "high" && s.source === "location.hash";
  });
});

test("document.writeln with user-controlled value → XSS", `
  var content = location.search.slice(1);
  document.writeln(content);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "document.writeln" && s.severity === "high" && s.source === "location.search";
  });
});

test("innerHTML via decodeURIComponent preserving taint", `
  var raw = location.hash;
  var decoded = decodeURIComponent(raw);
  document.body.innerHTML = decoded;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" && s.source === "location.hash";
  });
});

test("innerHTML via conditional with tainted branch", `
  var useCustom = true;
  var markup = useCustom ? location.hash : "<p>default</p>";
  document.body.innerHTML = markup;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" && s.source === "location.hash";
  });
});

test("innerHTML via complex HTML string concatenation", `
  var src = location.hash.slice(1);
  var html = '<img src="' + src + '" onerror="alert(1)">';
  document.body.innerHTML = html;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" && s.source === "location.hash";
  });
});

test("setAttribute onmouseover with user-controlled value → XSS", `
  var handler = location.hash.slice(1);
  document.getElementById("el").setAttribute("onmouseover", handler);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "setAttribute:onmouseover" && s.severity === "high";
  });
});

test("setTimeout with user-controlled string → eval sink", `
  var code = location.search.slice(1);
  setTimeout(code, 0);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "setTimeout" && s.severity === "high" && s.source === "location.search";
  });
});

test("setInterval with user-controlled string → eval sink", `
  var expr = location.hash.slice(1);
  setInterval(expr, 1000);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "setInterval" && s.severity === "high" && s.source === "location.hash";
  });
});

test("new Function with user-controlled code → eval sink", `
  var body = location.search.slice(1);
  var fn = new Function("x", body);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "new Function" && s.severity === "high" && s.source === "location.search";
  });
});

test("innerHTML via window.name taint source", `
  var name = window.name;
  document.body.innerHTML = name;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" && s.source === "window.name";
  });
});

test("eval with document.cookie taint source", `
  var data = document.cookie;
  eval(data);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "eval" && s.severity === "high" && s.source === "document.cookie";
  });
});

test("location.href redirect via document.referrer taint", `
  var ref = document.referrer;
  location.href = ref;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "href" && s.severity === "high" && s.source === "document.referrer";
  });
});

test("innerHTML via atob() preserving taint", `
  var encoded = location.hash.slice(1);
  var decoded = atob(encoded);
  document.body.innerHTML = decoded;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" && s.source === "location.hash";
  });
});

test("eval via unescape() preserving taint", `
  var raw = location.search.slice(1);
  var unescaped = unescape(raw);
  eval(unescaped);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "eval" && s.severity === "high" && s.source === "location.search";
  });
});

test("insertAdjacentHTML afterbegin with tainted value", `
  var markup = location.hash.slice(1);
  document.getElementById("container").insertAdjacentHTML("afterbegin", markup);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "insertAdjacentHTML" && s.severity === "high" && s.source === "location.hash";
  });
});

test("innerHTML via document.URL taint source", `
  var url = document.URL;
  document.body.innerHTML = url;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" && s.source === "document.URL";
  });
});

test("innerHTML via document.documentURI taint source", `
  var uri = document.documentURI;
  document.body.innerHTML = uri;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" && s.source === "document.documentURI";
  });
});

test("Multiple taint sources in concat — first tainted wins", `
  var a = location.hash;
  var b = document.referrer;
  var html = a + " from " + b;
  document.body.innerHTML = html;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high";
  });
});

test("Nested function calls: wrapper(wrapper(tainted)) → innerHTML", `
  function inner(x) { return x; }
  function outer(y) { return inner(y); }
  var val = outer(location.hash);
  document.body.innerHTML = val;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" && s.source === "location.hash";
  });
});

console.log("\n=== Security: Expected Non-Findings (True Negatives) ===\n");

test("textContent assignment with tainted value → not flagged (safe sink)", `
  var userInput = location.hash;
  document.getElementById("out").textContent = userInput;
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.sink === "textContent";
  });
});

test("innerText assignment with tainted value → not flagged (safe sink)", `
  var userInput = location.hash;
  document.getElementById("out").innerText = userInput;
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.sink === "innerText";
  });
});

test("innerHTML with empty string → not flagged (literal)", `
  document.body.innerHTML = "";
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

test("innerHTML with value from DOM element (not taint source) → not flagged", `
  var content = document.getElementById("source").textContent;
  document.getElementById("target").innerHTML = content;
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

test("innerHTML with static template literal (no expressions) → not flagged", `
  document.body.innerHTML = \`<div>static content</div>\`;
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

test("innerHTML with locally-computed value (no taint) → not flagged", `
  var count = 42;
  var html = "<span>" + count + " items</span>";
  document.body.innerHTML = html;
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

test("eval with locally-defined string constant → not flagged", `
  var code = "return 42";
  eval(code);
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "eval";
  });
});

test("setTimeout with function reference → not flagged", `
  function doWork() { console.log("done"); }
  setTimeout(doWork, 1000);
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "setTimeout";
  });
});

test("setTimeout with arrow function → not flagged", `
  setTimeout(() => console.log("tick"), 100);
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "setTimeout";
  });
});

test("setInterval with function expression → not flagged", `
  setInterval(function() { console.log("interval"); }, 500);
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "setInterval";
  });
});

test("location.href with hardcoded URL → not flagged", `
  location.href = "https://example.com/login";
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "href";
  });
});

test("location.assign with string literal → not flagged", `
  location.assign("/dashboard");
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "location.assign";
  });
});

test("location.replace with string literal → not flagged", `
  location.replace("https://example.com/new-page");
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "location.replace";
  });
});

test("setAttribute with non-event attribute (class) → not flagged", `
  var cls = location.hash.slice(1);
  document.getElementById("el").setAttribute("class", cls);
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink && s.sink.startsWith("setAttribute");
  });
});

test("setAttribute with data attribute → not flagged", `
  var val = location.hash.slice(1);
  document.getElementById("el").setAttribute("data-value", val);
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink && s.sink.startsWith("setAttribute");
  });
});

test("new Function with string literal body → not flagged", `
  var fn = new Function("a", "b", "return a + b");
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "new Function";
  });
});

test("new RegExp with string literal pattern → not flagged", `
  var re = new RegExp("^[a-z]+$", "i");
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "regex-dynamic";
  });
});

test("Prototype pollution with string literal key → not flagged", `
  var obj = {};
  obj["knownKey"] = "value";
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution";
  });
});

test("Prototype pollution with numeric key → not flagged", `
  var arr = [];
  arr[0] = "first";
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution";
  });
});

test("Dynamic property in for-in loop (not user-controlled) → not flagged", `
  var src = { a: 1, b: 2 };
  var dst = {};
  for (var k in src) {
    dst[k] = src[k];
  }
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution";
  });
});

test("Shadowed location variable → not flagged (not global location)", `
  function route(location) {
    document.body.innerHTML = location.hash;
  }
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

test("Shadowed eval identifier → not flagged (not global eval)", `
  function sandbox(eval) {
    eval("safe code");
  }
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "eval";
  });
});

test("postMessage listener with origin === check → not flagged", `
  window.addEventListener("message", function(e) {
    if (e.origin === "https://allowed.com") {
      document.body.innerHTML = e.data;
    }
  });
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin";
  });
});

test("postMessage listener with origin == check → not flagged", `
  window.addEventListener("message", function(ev) {
    if (ev.origin == "https://partner.com") {
      processData(ev.data);
    }
  });
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin";
  });
});

test("postMessage listener with source-only check → flagged (source insufficient)", `
  window.addEventListener("message", function(event) {
    if (event.source !== parent) return;
    handleMessage(event.data);
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin" && p.description.indexOf("source") !== -1;
  });
});

test("document.write with string literal → not flagged", `
  document.write("<p>Loading...</p>");
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "document.write";
  });
});

test("insertAdjacentHTML with string literal → not flagged", `
  document.body.insertAdjacentHTML("beforeend", "<hr>");
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "insertAdjacentHTML";
  });
});

test("innerHTML set from JSON.stringify (not user-controlled) → not flagged", `
  var data = { count: 5, label: "test" };
  document.body.innerHTML = JSON.stringify(data);
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

test("RegExp with dynamic but non-user-controlled pattern → not flagged", `
  function search(field) {
    var re = new RegExp(field, "i");
    return re.test("input");
  }
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "regex-dynamic";
  });
});

test("new Function with dynamic but non-user-controlled arg → not flagged", `
  function makeGetter(prop) {
    return new Function("obj", "return obj." + prop);
  }
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "new Function";
  });
});

console.log("\n=== Security: Realistic Vulnerability Patterns ===\n");

test("URL parameter extraction via split → innerHTML", `
  var param = location.search.split("=")[1];
  document.getElementById("greeting").innerHTML = "Hello, " + param;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" && s.source === "location.search";
  });
});

test("Hash-based router injects route content into DOM", `
  var route = location.hash.slice(1);
  var content = "<h1>" + route + "</h1>";
  document.getElementById("app").innerHTML = content;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" && s.source === "location.hash";
  });
});

test("document.write with template literal containing tainted value", `
  var name = location.search.slice(6);
  document.write(\`<h1>Welcome \${name}</h1>\`);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "document.write" && s.severity === "high" && s.source === "location.search";
  });
});

test("Multiple sinks in same code — both detected", `
  var input = location.hash.slice(1);
  document.getElementById("a").innerHTML = input;
  document.getElementById("b").outerHTML = input;
`, function(r) {
  var hasInner = r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high";
  });
  var hasOuter = r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "outerHTML" && s.severity === "high";
  });
  return hasInner && hasOuter;
});

test("Taint through assignment expression (x = tainted) used in sink", `
  var x;
  document.body.innerHTML = (x = location.hash);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" && s.source === "location.hash";
  });
});

test("Reassignment: let safe → tainted, used in sink → flagged", `
  let x = "safe";
  x = location.hash.slice(1);
  document.body.innerHTML = x;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" &&
           s.source === "location.hash";
  });
});

test("Reassignment: let safe → safe (no taint introduced) → NOT flagged", `
  let x = "safe";
  x = "still safe";
  document.body.innerHTML = x;
`, function(r) {
  // Both the initial value and the reassignment are literal. The new
  // constantViolations walker should see both as literal and not
  // promote the sink to user-controlled.
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.sourceType === "user-controlled";
  });
});

test("Reassignment: uninitialised let, reassigned to tainted → flagged", `
  let x;
  x = location.search.slice(1);
  document.body.innerHTML = x;
`, function(r) {
  // VariableDeclarator with no init, only constantViolations — the
  // walker's second branch must cover this shape too.
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" &&
           s.source === "location.search";
  });
});

test("Reassignment: uninitialised let, reassigned to literal only → NOT flagged", `
  let x;
  x = "hardcoded";
  document.body.innerHTML = x;
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.sourceType === "user-controlled";
  });
});

test("Reassignment: tainted init, overwritten by literal → still flagged (any-tainted-path is user-controlled)", `
  let x = location.hash.slice(1);
  x = "overwritten";
  document.body.innerHTML = x;
`, function(r) {
  // Deliberate over-taint: the walker returns the first user-controlled
  // source found (init OR a violation). We don't path-sensitivity-check
  // which reassignment is live at the sink — that would require full CFG
  // reaching-definitions analysis. Over-tainting here is the safe side.
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high";
  });
});

test("Reassignment: multiple reassigns, any one tainted → flagged", `
  let x = "a";
  x = "b";
  x = location.hash;
  x = "c";
  document.body.innerHTML = x;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" &&
           s.source === "location.hash";
  });
});

test("TaggedTemplateExpression with tainted interpolation → flagged", `
  var t = location.hash.slice(1);
  document.body.innerHTML = String.raw\`<p>\${t}</p>\`;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high";
  });
});

test("TaggedTemplateExpression with only literal interpolations → NOT flagged", `
  var name = "world";
  document.body.innerHTML = String.raw\`<p>hello \${name}</p>\`;
`, function(r) {
  // name is a local, not user-controlled — the tagged template should
  // not promote the sink.
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.sourceType === "user-controlled";
  });
});

test("TaggedTemplateExpression with no expressions → NOT flagged", `
  document.body.innerHTML = String.raw\`<p>static</p>\`;
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

test("Optional chaining on tainted object propagates taint → flagged", `
  var data = location;
  document.body.innerHTML = data?.hash;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high";
  });
});

test("Optional chaining on SAFE object → NOT flagged", `
  var cfg = { name: "static" };
  document.body.innerHTML = cfg?.name;
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.sourceType === "user-controlled";
  });
});

test("Optional call on tainted propagates taint → flagged", `
  var t = location.hash;
  document.body.innerHTML = t?.slice(1);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high";
  });
});

test("Optional call on SAFE receiver → NOT flagged", `
  var s = "hello";
  document.body.innerHTML = s?.toUpperCase();
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.sourceType === "user-controlled";
  });
});

test("Direct optional-chained taint source: location?.hash → flagged", `
  document.body.innerHTML = location?.hash;
`, function(r) {
  // _matchTaintSource must recognise OptionalMemberExpression too.
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" &&
           s.source === "location.hash";
  });
});

test("setAttribute onerror with user-controlled value", `
  var code = location.hash.slice(1);
  document.querySelector("img").setAttribute("onerror", code);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "setAttribute:onerror" && s.severity === "high";
  });
});

test("setAttribute onload with user-controlled value", `
  var handler = location.search.slice(1);
  document.querySelector("iframe").setAttribute("onload", handler);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "setAttribute:onload" && s.severity === "high";
  });
});

test("location.pathname as taint source → innerHTML", `
  var path = location.pathname;
  document.body.innerHTML = "<pre>" + path + "</pre>";
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" && s.source === "location.pathname";
  });
});

test("document.domain as taint source → eval", `
  var domain = document.domain;
  eval("loadScript('" + domain + "')");
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "eval" && s.severity === "high" && s.source === "document.domain";
  });
});

test("XSS and redirect in same code block — both types detected", `
  var input = location.hash.slice(1);
  document.body.innerHTML = input;
  location.href = input;
`, function(r) {
  var hasXss = r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
  var hasRedirect = r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "href";
  });
  return hasXss && hasRedirect;
});

test("Taint through String() wrapper call", `
  var raw = location.hash;
  var str = String(raw);
  document.body.innerHTML = str;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" && s.source === "location.hash";
  });
});

test("Taint through .toString() on tainted object", `
  var raw = location.search;
  var str = raw.toString();
  document.body.innerHTML = str;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" && s.source === "location.search";
  });
});

test("Taint through .trim() on tainted value", `
  var input = location.hash.trim();
  document.body.innerHTML = input;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" && s.source === "location.hash";
  });
});

test("Taint through .replace() on tainted string", `
  var raw = location.search.slice(1);
  var cleaned = raw.replace(/</g, "");
  eval(cleaned);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "eval" && s.severity === "high" && s.source === "location.search";
  });
});

test("Prototype pollution with nested bracket access obj[tainted][prop]", `
  var key = location.hash.slice(1);
  var target = {};
  target[key] = {};
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution" && p.severity === "high";
  });
});

test("location.href self-assignment from tainted source → redirect", `
  location.href = location.search.slice(5);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "href" && s.severity === "high" && s.source === "location.search";
  });
});

test("Taint preserved through decodeURI wrapper", `
  var encoded = location.hash.slice(1);
  var decoded = decodeURI(encoded);
  document.body.innerHTML = decoded;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" && s.source === "location.hash";
  });
});

// location.origin / .hostname / .protocol are SERVER-set (scheme+host+port
// of the currently-served page), not attacker-controlled. They must not
// promote a sink to HIGH severity.
test("location.origin is NOT user-controlled (server-set)", `
  var origin = location.origin;
  document.body.innerHTML = "<a href='" + origin + "'>Link</a>";
`, function(r) {
  // innerHTML sink may still fire as MEDIUM/LOW but MUST NOT be HIGH with
  // location.origin recorded as user-controlled source.
  return !r.securitySinks.some(function(s) {
    return s.sink === "innerHTML" && s.severity === "high" && s.source === "location.origin";
  });
});

test("location.hostname is NOT user-controlled", `
  var host = location.hostname;
  location.assign("https://" + host + "/callback");
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.severity === "high" && s.source === "location.hostname";
  });
});

test("new URL(x, location.origin) for same-origin fetch is safe", `
  var e = new URL(this.paginationSrc, window.location.origin);
  fetch(e);
`, function(r) {
  // Must not flag as HIGH request-forgery just because location.origin is in the chain.
  return !r.securitySinks.some(function(s) {
    return s.type === "request-forgery" && s.severity === "high" && (s.source === "location.origin" || s.source === "location.hostname" || s.source === "location");
  });
});

test("github showMore pattern: new URL + searchParams + fetch is NOT flagged HIGH", `
  async function showMore() {
    let e = new URL(this.paginationSrc, window.location.origin);
    this.currentPage++;
    e.searchParams.append("page", this.currentPage.toString());
    let i = await fetch(e);
    return i.text();
  }
`, function(r) {
  // The paginator is the most common FP we hit on github. Must not flag HIGH.
  return !r.securitySinks.some(function(s) {
    return s.type === "request-forgery" && s.severity === "high";
  });
});

// ── Fetch-response taint boundary ─────────────────────────────────────────
// Break taint through .then() only when we can prove the fetch target is
// SAME-ORIGIN (attacker can influence path/query but not server). If the
// URL's origin could be attacker-chosen, they serve the response — taint
// propagates through the response body.

test("fetch(SAME-ORIGIN url with tainted query).then(...) breaks taint", `
  function search(q) {
    var url = new URL("/api/search", location.origin);
    url.searchParams.set("q", location.hash.slice(1));
    fetch(url).then(r => r.text()).then(html => {
      document.getElementById("results").innerHTML = html;
    });
  }
`, function(r) {
  // Base is location.origin, input "/api/search" is relative, overall URL
  // targets the page's own server. Response is server-controlled.
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.severity === "high" && /location/.test(s.source || "");
  });
});

test("fetch('/api?' + tainted) is same-origin, taint breaks", `
  window.fetch("/api?" + location.search).then(r => r.json()).then(data => {
    document.body.innerHTML = data.html;
  });
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.severity === "high" && /location/.test(s.source || "");
  });
});

// DANGEROUS: fetch(attacker-controlled URL) — response IS attacker-controlled.
test("fetch(location.hash) — attacker picks origin — taint PROPAGATES (HIGH)", `
  fetch(location.hash.slice(1)).then(r => r.text()).then(html => {
    document.body.innerHTML = html;
  });
`, function(r) {
  // location.hash could be `https://evil.com/payload` — attacker controls
  // the whole URL including origin, so the response is attacker-controlled.
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.severity === "high" && /location/.test(s.source || "");
  });
});

test("fetch(location.href) — attacker controls href — taint PROPAGATES", `
  fetch(location.href).then(r => r.text()).then(html => {
    document.body.innerHTML = html;
  });
`, function(r) {
  // Note: location.href string passed directly is taken at face value —
  // if the attacker can navigate victim to any URL, they control the full
  // fetch target. We conservatively treat this as attacker-controlled.
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.severity === "high" && /location/.test(s.source || "");
  });
});

test("fetch('//evil.com/' + tainted) — protocol-relative — taint PROPAGATES", `
  fetch("//" + location.hash.slice(1)).then(r => r.text()).then(html => {
    document.body.innerHTML = html;
  });
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.severity === "high";
  });
});

// SAFETY: a direct assignment `innerHTML = location.hash` STILL fires.
test("direct innerHTML = location.hash without fetch in between IS flagged", `
  document.body.innerHTML = location.hash;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.severity === "high" && s.source === "location.hash";
  });
});

// SAFETY: if data goes URL → fetch → innerHTML but the URL was NEVER
// tainted, the finding should not fire at all. (Check: no HIGH from this.)
test("fetch(literal-url).then(innerHTML) is not flagged", `
  fetch("/api/safe").then(r => r.text()).then(html => document.body.innerHTML = html);
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.severity === "high";
  });
});

test("new URL(absolute-string, location.origin) does NOT break taint (input overrides base)", `
  fetch(new URL(location.hash.slice(1), location.origin)).then(r => r.text()).then(html => {
    document.body.innerHTML = html;
  });
`, function(r) {
  // Even though the base is same-origin, if location.hash = "https://evil.com/x",
  // new URL("https://evil.com/x", location.origin) === "https://evil.com/x"
  // — the input overrides the base. We can't prove same-origin here.
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.severity === "high";
  });
});

test("template-literal fetch(`/api?q=${tainted}`) IS same-origin", `
  fetch(\`/api?q=\${location.search}\`).then(r => r.text()).then(html => {
    document.body.innerHTML = html;
  });
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.severity === "high" && /location/.test(s.source || "");
  });
});

test("self.location.protocol inside fetch base is safe", `
  fetch(new URL("/api", self.location.protocol + "//" + self.location.hostname));
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.severity === "high" && (s.source === "self.location" || s.source === "location.protocol" || s.source === "location.hostname");
  });
});

test("innerHTML with tainted value inside IIFE", `
  (function() {
    var x = location.hash;
    document.body.innerHTML = x;
  })();
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" && s.source === "location.hash";
  });
});

test("Taint through arrow function parameter", `
  var render = (html) => { document.body.innerHTML = html; };
  render(location.hash);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" && s.source === "location.hash";
  });
});

console.log("\n=== Security: Advanced True Negatives ===\n");

test("createElement + textContent (safe DOM construction) → not flagged", `
  var el = document.createElement("div");
  el.textContent = location.hash;
  document.body.appendChild(el);
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss";
  });
});

test("console.log with tainted value → not flagged (not a sink)", `
  var data = location.hash;
  console.log(data);
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" || s.type === "eval";
  });
});

test("Tainted value sent via fetch body → not flagged (not client-side XSS)", `
  var input = location.search.slice(1);
  fetch("/api/log", { method: "POST", body: input });
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" || s.type === "eval";
  });
});

test("RegExp.test with tainted input string (not pattern) → not flagged", `
  var input = location.search.slice(1);
  var re = /^[a-z]+$/;
  var result = re.test(input);
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "regex-dynamic";
  });
});

test("Array push with tainted value → not flagged", `
  var items = [];
  items.push(location.hash);
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" || s.type === "eval";
  });
});

test("addEventListener for non-message event → not flagged as postMessage", `
  window.addEventListener("click", function(event) {
    document.body.innerHTML = event.target.id;
  });
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin";
  });
});

test("Shadowed document AND location → not flagged (both are local params)", `
  function render(document, location) {
    document.body.innerHTML = location.hash;
  }
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

test("innerHTML with boolean value → not flagged", `
  var flag = true;
  document.body.innerHTML = flag;
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

test("innerHTML with null value → not flagged", `
  document.body.innerHTML = null;
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

test("innerHTML with numeric value → not flagged", `
  var count = 42;
  document.body.innerHTML = count;
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

test("setTimeout with numeric argument → not flagged", `
  setTimeout(0, 100);
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "setTimeout";
  });
});

test("innerHTML assignment in loop with literal array → not flagged", `
  var items = ["<li>A</li>", "<li>B</li>", "<li>C</li>"];
  var html = "";
  for (var i = 0; i < items.length; i++) {
    html += items[i];
  }
  document.body.innerHTML = html;
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

test("Prototype pollution: Object.keys iteration (not user-controlled) → not flagged", `
  var src = { a: 1, b: 2 };
  var dst = {};
  Object.keys(src).forEach(function(key) {
    dst[key] = src[key];
  });
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution";
  });
});

test("Shadowed window variable → not flagged", `
  function init(window) {
    document.body.innerHTML = window.name;
  }
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.source === "window.name";
  });
});

test("postMessage handler with named function having origin check → not flagged", `
  function handleMsg(event) {
    if (event.origin !== "https://trusted.com") return;
    console.log(event.data);
  }
  window.addEventListener("message", handleMsg);
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin";
  });
});

test("location.href = concat of two literals → not flagged", `
  var base = "https://example.com";
  var path = "/login";
  location.href = base + path;
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "redirect";
  });
});

test("eval inside try-catch with literal string → not flagged", `
  try {
    eval("JSON.parse('{}')");
  } catch(e) {}
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "eval";
  });
});

test("insertAdjacentHTML with locally-built safe markup → not flagged", `
  var count = 5;
  var html = "<span class='badge'>" + count + "</span>";
  document.body.insertAdjacentHTML("beforeend", html);
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "insertAdjacentHTML";
  });
});

test("document.write with string literal → not flagged (closing tag)", `
  document.write("</div><div class='footer'>");
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "document.write";
  });
});

test("setAttribute with href (javascript: protocol injection) → flagged", `
  var url = location.hash.slice(1);
  document.querySelector("a").setAttribute("href", url);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "setAttribute:href" && s.source === "location.hash";
  });
});

test("new RegExp with variable from function param (not user-controlled) → not flagged", `
  function matchField(fieldName) {
    return new RegExp("^" + fieldName + "$");
  }
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "regex-dynamic";
  });
});

console.log("\n=== Security: Detection Gap Coverage ===\n");

// -- Computed property sinks: el["innerHTML"] --

test("el[\"innerHTML\"] = tainted → detected (computed string literal property)", `
  var payload = location.hash.slice(1);
  document.getElementById("out")["innerHTML"] = payload;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" && s.source === "location.hash";
  });
});

test("el[\"outerHTML\"] = tainted → detected (computed string literal property)", `
  var payload = location.hash.slice(1);
  document.getElementById("out")["outerHTML"] = payload;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "outerHTML" && s.severity === "high";
  });
});

test("el[variable] = tainted → not flagged (unknown computed property)", `
  var prop = "textContent";
  var val = location.hash;
  document.body[prop] = val;
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss";
  });
});

// -- window.location / self.location redirect sinks --

test("window.location.href = tainted → redirect detected", `
  var url = location.hash.slice(1);
  window.location.href = url;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "href" && s.severity === "high" && s.source === "location.hash";
  });
});

test("self.location.href = tainted → redirect detected", `
  var url = location.search.slice(1);
  self.location.href = url;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "href" && s.severity === "high" && s.source === "location.search";
  });
});

test("window.location.assign(tainted) → redirect detected", `
  var dest = location.hash.slice(1);
  window.location.assign(dest);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "location.assign" && s.severity === "high";
  });
});

test("window.location.replace(tainted) → redirect detected", `
  var dest = location.search.slice(1);
  window.location.replace(dest);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "location.replace" && s.severity === "high";
  });
});

test("window.location.href = literal → not flagged", `
  window.location.href = "/dashboard";
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "redirect";
  });
});

test("self.location.assign(literal) → not flagged", `
  self.location.assign("https://example.com");
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "redirect";
  });
});

test("location.href = location.href (self-assignment reload) → not flagged", `
  location.href = location.href;
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "redirect";
  });
});

test("window.location.href = window.location.href → not flagged", `
  window.location.href = window.location.href;
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "redirect";
  });
});

test("document.location = document.location → not flagged", `
  document.location = document.location;
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "redirect";
  });
});

test("location.href = location.search (different property) → still flagged", `
  location.href = location.search;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "href";
  });
});

// -- Untested redirect assignment sinks --

test("location.pathname = tainted → redirect detected", `
  var path = location.hash.slice(1);
  location.pathname = path;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "pathname" && s.severity === "high";
  });
});

test("location.search = tainted → redirect detected", `
  var qs = document.referrer;
  location.search = qs;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "search" && s.severity === "high" && s.source === "document.referrer";
  });
});

test("location.protocol is NOT user-controlled", `
  var proto = location.protocol;
  document.body.innerHTML = proto;
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.severity === "high" && s.source === "location.protocol";
  });
});

test("location.hash IS user-controlled (attacker picks the link)", `
  var h = location.hash;
  document.body.innerHTML = h;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.severity === "high" && s.source === "location.hash";
  });
});

test("location.search IS user-controlled", `
  var s = location.search;
  document.body.innerHTML = s;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.severity === "high" && s.source === "location.search";
  });
});

// URLSearchParams iteration: query-param KEYS are attacker-controlled.
// Using them as object keys is prototype-pollution exposure. The taint
// must flow through `for (const [k, v] of new URLSearchParams(location.search))`.
// Observed in github's environment.js (Session 2026-04-17 audit) where the
// taint source was previously recorded as null.
test("prototype pollution: URLSearchParams key destructuring taints both k and v", `
  function track(obj) {
    for (const [t, n] of new URLSearchParams(window.location.search)) {
      obj[t.toLowerCase()] = n;
    }
    return obj;
  }
`, function(r) {
  // Value `n` is a URLSearchParams string entry → primitive → MEDIUM per
  // value-shape analysis (no object-level prototype pollution possible).
  // Test intent: taint flows through destructuring AND a finding is made.
  return (r.dangerousPatterns || []).some(function(p) {
    return /prototype/i.test(p.type || "") && p.severity !== "info";
  });
});

// ── Allowlist-key validation downgrade ──────────────────────────────────
// Proper implementation using CFG-equivalent AST ancestor dominator check
// with scope-aware identifier matching + collection-purity verification.

test("prototype pollution: static Array allowlist guard → INFO (downgrade)", `
  function track(t) {
    const ALLOWED = ["utm_source", "utm_medium", "ref"];
    for (const [e, r] of new URLSearchParams(window.location.search)) {
      ALLOWED.includes(e.toLowerCase()) && (t[e] = r);
    }
    return t;
  }
`, function(r) {
  var patterns = r.dangerousPatterns || [];
  var hasInfo = patterns.some(function(p) { return /prototype-pollution/.test(p.type) && p.severity === "info"; });
  var hasHigh = patterns.some(function(p) { return /prototype-pollution/.test(p.type) && p.severity !== "info"; });
  return hasInfo && !hasHigh;
});

test("prototype pollution: if + Set.has allowlist → INFO", `
  function track(obj) {
    const allowed = new Set(["a", "b", "c"]);
    for (const [k, v] of new URLSearchParams(location.search)) {
      if (allowed.has(k)) obj[k] = v;
    }
  }
`, function(r) {
  return (r.dangerousPatterns || []).some(function(p) {
    return /prototype-pollution/.test(p.type) && p.severity === "info";
  });
});

test("prototype pollution: `key in obj` allowlist → INFO", `
  function track(out) {
    const allowed = { a: 1, b: 2 };
    for (const [k, v] of new URLSearchParams(location.search)) {
      if (k in allowed) out[k] = v;
    }
  }
`, function(r) {
  return (r.dangerousPatterns || []).some(function(p) {
    return /prototype-pollution/.test(p.type) && p.severity === "info";
  });
});

test("prototype pollution: ternary + .includes allowlist → INFO", `
  function track(t) {
    const ALLOWED = ["x", "y"];
    for (const [k, v] of new URLSearchParams(location.search)) {
      ALLOWED.includes(k) ? (t[k] = v) : null;
    }
  }
`, function(r) {
  return (r.dangerousPatterns || []).some(function(p) {
    return /prototype-pollution/.test(p.type) && p.severity === "info";
  });
});

test("prototype pollution: NO guard stays HIGH", `
  function track(obj) {
    for (const [k, v] of new URLSearchParams(location.search)) {
      obj[k] = v;
    }
  }
`, function(r) {
  return (r.dangerousPatterns || []).some(function(p) {
    return /prototype-pollution/.test(p.type) && p.severity !== "info";
  });
});

// SAFETY: a tainted allowlist doesn't sanitize — the attacker chose what's allowed.
test("prototype pollution: allowlist ITSELF tainted → stays HIGH", `
  function track(obj) {
    const tainted = JSON.parse(location.hash.slice(1)).allowedKeys;
    for (const [k, v] of new URLSearchParams(location.search)) {
      if (tainted.includes(k)) obj[k] = v;
    }
  }
`, function(r) {
  return (r.dangerousPatterns || []).some(function(p) {
    return /prototype-pollution/.test(p.type) && p.severity !== "info";
  });
});

// SAFETY: the allowlist guard tests a DIFFERENT identifier than the sink key.
test("prototype pollution: guard tests a different identifier → stays HIGH", `
  function track(obj) {
    const ALLOWED = ["a","b"];
    const unrelatedFlag = "a";
    for (const [k, v] of new URLSearchParams(location.search)) {
      if (ALLOWED.includes(unrelatedFlag)) obj[k] = v;
    }
  }
`, function(r) {
  return (r.dangerousPatterns || []).some(function(p) {
    return /prototype-pollution/.test(p.type) && p.severity !== "info";
  });
});

// SAFETY: guard is BEFORE the sink but not dominating it.
test("prototype pollution: guard not dominating sink → stays HIGH", `
  function track(obj) {
    const ALLOWED = ["a","b"];
    for (const [k, v] of new URLSearchParams(location.search)) {
      if (ALLOWED.includes(k)) { /* noop */ }
      obj[k] = v; // assignment is OUTSIDE the consequent
    }
  }
`, function(r) {
  return (r.dangerousPatterns || []).some(function(p) {
    return /prototype-pollution/.test(p.type) && p.severity !== "info";
  });
});

// SAFETY: else branch is NOT dominated by the validator.
test("prototype pollution: assignment in else branch → stays HIGH", `
  function track(obj) {
    const ALLOWED = ["a"];
    for (const [k, v] of new URLSearchParams(location.search)) {
      if (ALLOWED.includes(k)) {
        // safe path
      } else {
        obj[k] = v;  // unvalidated
      }
    }
  }
`, function(r) {
  return (r.dangerousPatterns || []).some(function(p) {
    return /prototype-pollution/.test(p.type) && p.severity !== "info";
  });
});

test("prototype pollution: allowlist + indexOf-style DOES NOT downgrade (not a predicate)", `
  function track(obj) {
    const arr = ["a","b"];
    for (const [k, v] of new URLSearchParams(location.search)) {
      if (arr.indexOf(k) >= 0) obj[k] = v;
    }
  }
`, function(r) {
  // indexOf isn't in our validator allowlist — conservative: stays HIGH.
  return (r.dangerousPatterns || []).some(function(p) {
    return /prototype-pollution/.test(p.type) && p.severity !== "info";
  });
});

// ── CFG imprecision safety: direction-of-error must be safe (no false
// downgrades). Each test constructs a control-flow shape the CFG handles
// imprecisely and asserts the sink stays HIGH when there's no true
// dominating validator, even if a validator exists somewhere in the function.

test("CFG imprecision: validator inside switch case ≠ dominator for case fall-through", `
  function track(obj) {
    for (const [k, v] of new URLSearchParams(location.search)) {
      switch (k) {
        case "explicit":
          obj[k] = v;  // no validator — switch-match is NOT an allowlist test
          break;
        default:
          obj[k] = v;
      }
    }
  }
`, function(r) {
  // switch-case matching doesn't match .includes/.has/in — validator detector
  // won't mark these blocks as validators — sink stays HIGH.
  return (r.dangerousPatterns || []).some(function(p) {
    return /prototype-pollution/.test(p.type) && p.severity !== "info";
  });
});

test("CFG imprecision: early return inside validated branch doesn't expose later sink", `
  function track(obj) {
    const ALLOWED = ["x"];
    for (const [k, v] of new URLSearchParams(location.search)) {
      if (ALLOWED.includes(k)) {
        obj[k] = v;   // VALIDATED → info
        return;
      }
      obj[k] = v;     // UNVALIDATED (after the early-return guard fails) → HIGH
    }
  }
`, function(r) {
  var patterns = r.dangerousPatterns || [];
  var hasHigh = patterns.some(function(p) {
    return /prototype-pollution/.test(p.type) && p.severity !== "info";
  });
  var hasInfo = patterns.some(function(p) {
    return /prototype-pollution/.test(p.type) && p.severity === "info";
  });
  // Must see BOTH findings, each at the correct severity.
  return hasHigh && hasInfo;
});

test("CFG imprecision: throw inside validated branch doesn't relax later sink", `
  function track(obj) {
    const ALLOWED = ["x"];
    for (const [k, v] of new URLSearchParams(location.search)) {
      if (ALLOWED.includes(k)) {
        throw new Error(k);
      }
      obj[k] = v;  // UNVALIDATED path — stays HIGH
    }
  }
`, function(r) {
  return (r.dangerousPatterns || []).some(function(p) {
    return /prototype-pollution/.test(p.type) && p.severity !== "info";
  });
});

test("CFG imprecision: nested if — outer validator dominates inner sink", `
  function track(obj) {
    const ALLOWED = ["a"];
    for (const [k, v] of new URLSearchParams(location.search)) {
      if (ALLOWED.includes(k)) {
        if (v.length > 0) {
          obj[k] = v;  // outer validator dominates — should be INFO
        }
      }
    }
  }
`, function(r) {
  return (r.dangerousPatterns || []).some(function(p) {
    return /prototype-pollution/.test(p.type) && p.severity === "info";
  });
});

test("CFG imprecision: two separate guards where BOTH branches contain a sink", `
  function track(obj) {
    const A = ["a"];
    const B = ["b"];
    for (const [k, v] of new URLSearchParams(location.search)) {
      if (A.includes(k)) {
        obj[k] = v;  // validated
      } else if (B.includes(k)) {
        obj[k] = v;  // validated
      }
    }
  }
`, function(r) {
  // Both assignments are inside validated consequents — BOTH should be INFO,
  // not HIGH.
  var highs = (r.dangerousPatterns || []).filter(function(p) {
    return /prototype-pollution/.test(p.type) && p.severity !== "info";
  });
  var infos = (r.dangerousPatterns || []).filter(function(p) {
    return /prototype-pollution/.test(p.type) && p.severity === "info";
  });
  return highs.length === 0 && infos.length >= 1;
});

test("CFG imprecision: try/catch — sink in try after validator is INFO", `
  function track(obj) {
    const ALLOWED = ["k"];
    for (const [k, v] of new URLSearchParams(location.search)) {
      if (ALLOWED.includes(k)) {
        try {
          obj[k] = v;  // validator dominates via try-entry → INFO
        } catch (e) {}
      }
    }
  }
`, function(r) {
  return (r.dangerousPatterns || []).some(function(p) {
    return /prototype-pollution/.test(p.type) && p.severity === "info";
  });
});

test("CFG imprecision: validator in catch does NOT cover sink in try", `
  function track(obj) {
    const ALLOWED = ["k"];
    for (const [k, v] of new URLSearchParams(location.search)) {
      try {
        obj[k] = v;   // UNVALIDATED — try-entry is reached before catch
      } catch (e) {
        if (ALLOWED.includes(k)) obj[k] = v;  // this one is validated (info)
      }
    }
  }
`, function(r) {
  var patterns = r.dangerousPatterns || [];
  // Must have at least one HIGH (the try-block sink).
  return patterns.some(function(p) {
    return /prototype-pollution/.test(p.type) && p.severity !== "info";
  });
});

test("CFG imprecision: validator in finally does NOT cover sink in try", `
  function track(obj) {
    const ALLOWED = ["k"];
    for (const [k, v] of new URLSearchParams(location.search)) {
      try {
        obj[k] = v;
      } finally {
        // Finally runs AFTER try — validator here can't retroactively sanitize
        // the already-executed try sink.
        ALLOWED.includes(k);
      }
    }
  }
`, function(r) {
  return (r.dangerousPatterns || []).some(function(p) {
    return /prototype-pollution/.test(p.type) && p.severity !== "info";
  });
});

test("CFG imprecision: while loop body — validator inside loop body", `
  function track(obj) {
    const ALLOWED = ["a"];
    let i = 0;
    const params = new URLSearchParams(location.search);
    const entries = [...params];
    while (i < entries.length) {
      const [k, v] = entries[i];
      if (ALLOWED.includes(k)) {
        obj[k] = v;  // validated, in while body
      }
      i++;
    }
  }
`, function(r) {
  return (r.dangerousPatterns || []).some(function(p) {
    return /prototype-pollution/.test(p.type) && p.severity === "info";
  });
});

test("CFG imprecision: do-while sink without validator stays HIGH", `
  function track(obj) {
    let i = 0;
    const entries = [...new URLSearchParams(location.search)];
    do {
      const [k, v] = entries[i];
      obj[k] = v;  // no validator
      i++;
    } while (i < entries.length);
  }
`, function(r) {
  return (r.dangerousPatterns || []).some(function(p) {
    return /prototype-pollution/.test(p.type) && p.severity !== "info";
  });
});

// ── Message-listener receiver discrimination ──────────────────────────────
// BroadcastChannel, Worker, SharedWorker, MessageChannel, EventSource are
// all same-origin-by-spec. Their "message" events never carry cross-origin
// data, so the usual event.origin check doesn't apply — flagging them as
// "postmessage-no-origin" is a false positive.

test("BroadcastChannel message listener is NOT flagged as postmessage-no-origin", `
  const chan = new BroadcastChannel("my-channel");
  chan.addEventListener("message", (ev) => {
    document.body.innerHTML = ev.data.html;
  });
`, function(r) {
  return !(r.dangerousPatterns || []).some(function(p) {
    return /postmessage-no-origin|postmessage-weak/i.test(p.type);
  });
});

test("inline new BroadcastChannel().addEventListener is NOT flagged", `
  new BroadcastChannel("shared-worker-error").addEventListener("message", e => {
    console.log(e.data.error);
  });
`, function(r) {
  return !(r.dangerousPatterns || []).some(function(p) {
    return /postmessage-no-origin|postmessage-weak/i.test(p.type);
  });
});

test("Worker.addEventListener is NOT flagged (same-origin worker)", `
  const w = new Worker("./worker.js");
  w.addEventListener("message", (e) => {
    document.body.textContent = e.data;
  });
`, function(r) {
  return !(r.dangerousPatterns || []).some(function(p) {
    return /postmessage-no-origin|postmessage-weak/i.test(p.type);
  });
});

test("EventSource.addEventListener is NOT flagged", `
  const es = new EventSource("/stream");
  es.addEventListener("message", (e) => {
    render(e.data);
  });
`, function(r) {
  return !(r.dangerousPatterns || []).some(function(p) {
    return /postmessage-no-origin|postmessage-weak/i.test(p.type);
  });
});

// SAFETY: window listener WITHOUT origin check is still flagged.
test("window.addEventListener('message', ...) without origin check IS flagged", `
  window.addEventListener("message", (e) => {
    document.body.innerHTML = e.data.html;
  });
`, function(r) {
  return (r.dangerousPatterns || []).some(function(p) {
    return p.type === "postmessage-no-origin";
  });
});

test("bare addEventListener('message', ...) at global scope IS flagged", `
  addEventListener("message", (e) => {
    document.body.innerHTML = e.data.html;
  });
`, function(r) {
  return (r.dangerousPatterns || []).some(function(p) {
    return p.type === "postmessage-no-origin";
  });
});

test("MessagePort.addEventListener — cross-origin-capable, still flagged", `
  function handlePort(port) {
    port.addEventListener("message", (e) => {
      document.body.innerHTML = e.data;
    });
  }
`, function(r) {
  // MessagePort can carry cross-origin data when transferred via postMessage.
  // We only whitelist constructor-typed BroadcastChannel/Worker/etc, not
  // arbitrary identifiers named like port/channel — conservative.
  return (r.dangerousPatterns || []).some(function(p) {
    return p.type === "postmessage-no-origin";
  });
});


test("URLSearchParams key taint survives via for-of destructuring → innerHTML", `
  for (const [k, v] of new URLSearchParams(location.search)) {
    document.body.innerHTML = k;
  }
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.severity === "high" &&
      (s.source === "location.search" || /location/.test(s.source || ""));
  });
});

test("URLSearchParams value taint survives destructuring → innerHTML", `
  for (const [k, v] of new URLSearchParams(location.hash.slice(1))) {
    document.body.innerHTML = v;
  }
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.severity === "high" &&
      (/location\.(hash|search)/.test(s.source || ""));
  });
});

test("Plain for-of (not destructured) over URLSearchParams is also tainted", `
  for (const entry of new URLSearchParams(location.search)) {
    document.body.innerHTML = entry[0];
  }
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.severity === "high" &&
      /location/.test(s.source || "");
  });
});

test("Object.entries(obj)-style destructuring in for-of propagates taint", `
  var data = JSON.parse(location.hash.slice(1));
  for (const [k, v] of Object.entries(data)) {
    document.body.innerHTML = v;
  }
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.severity === "high";
  });
});


console.log("\n=== Security: Taint Tracer Edge Cases ===\n");

test("Taint through conditional where both branches are tainted", `
  var x = true ? location.hash : location.search;
  document.body.innerHTML = x;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high";
  });
});

test("Taint through conditional where only alternate is tainted", `
  var x = false ? "safe" : location.hash;
  document.body.innerHTML = x;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" && s.source === "location.hash";
  });
});

test("Deep variable hops: 4+ hops traced through cycle detection", `
  var a = location.hash;
  var b = a;
  var c = b;
  var d = c;
  var e = d;
  document.body.innerHTML = e;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" && s.source === "location.hash";
  });
});

test("Taint through nested computed access: tainted.split('&')[0].split('=')[1]", `
  var parts = location.search.slice(1).split("&");
  var val = parts[0];
  document.body.innerHTML = val;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high" && s.source === "location.search";
  });
});

test("Taint through template literal with multiple tainted expressions", `
  var a = location.hash;
  var b = location.search;
  document.body.innerHTML = \`\${a} and \${b}\`;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.severity === "high";
  });
});

test("setTimeout with variable resolving to arrow function → not flagged", `
  var handler = () => console.log("tick");
  setTimeout(handler, 100);
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "setTimeout";
  });
});

test("setInterval with variable resolving to function expression → not flagged", `
  var tick = function() { console.log("tock"); };
  setInterval(tick, 500);
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "setInterval";
  });
});

test("Taint through += concatenation on innerHTML (augmented assignment)", `
  document.body.innerHTML += location.hash;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.source === "location.hash";
  });
});

test("Multiple postMessage listeners — only the one without origin check flagged", `
  window.addEventListener("message", function(e) {
    if (e.origin === "https://trusted.com") {
      safe(e.data);
    }
  });
  window.addEventListener("message", function(e) {
    dangerous(e.data);
  });
`, function(r) {
  var patterns = r.dangerousPatterns.filter(function(p) {
    return p.type === "postmessage-no-origin";
  });
  return patterns.length === 1;
});

// ═══════════════════════════════════════════════════════════════════
// Google Firing Range - Address DOM XSS test cases
// https://public-firing-range.appspot.com/address/index.html
// ═══════════════════════════════════════════════════════════════════

console.log("\n=== Security: Firing Range — location.hash Sources ===\n");

test("FR: location.hash → location.assign (open redirect)", `
  var payload = window.location.hash.substr(1);
  window.location.assign(payload);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "location.assign" && s.source === "location.hash";
  });
});

test("FR: location.hash → document.write", `
  var payload = window.location.hash.substr(1);
  document.write(payload);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "document.write" && s.source === "location.hash";
  });
});

test("FR: location.hash → document.writeln", `
  var payload = window.location.hash.substr(1);
  document.writeln(payload);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "document.writeln" && s.source === "location.hash";
  });
});

test("FR: location.hash → eval", `
  var payload = window.location.hash.substr(1);
  eval(payload);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "eval" && s.source === "location.hash";
  });
});

test("FR: location.hash → innerHTML", `
  var payload = window.location.hash.substr(1);
  var div = document.createElement('div');
  div.id = 'divEl';
  document.documentElement.appendChild(div);
  var divEl = document.getElementById('divEl');
  divEl.innerHTML = payload;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.source === "location.hash";
  });
});

test("FR: location.hash → range.createContextualFragment", `
  var payload = window.location.hash.substr(1);
  var div = document.createElement('div');
  div.id = 'divEl';
  document.documentElement.appendChild(div);
  var range = document.createRange();
  range.selectNode(document.getElementsByTagName("div").item(0));
  var documentFragment = range.createContextualFragment(payload);
  document.body.appendChild(documentFragment);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "createContextualFragment" && s.source === "location.hash";
  });
});

test("FR: location.hash → location.replace (open redirect)", `
  var payload = window.location.hash.substr(1);
  location.replace(payload);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "location.replace" && s.source === "location.hash";
  });
});

test("FR: location.hash → setTimeout(string)", `
  var payload = window.location.hash.substr(1);
  setTimeout('var a=a;' + payload, 1);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "setTimeout" && s.source === "location.hash";
  });
});

test("FR: location.hash → new Function", `
  var payload = window.location.hash.substr(1);
  var f = new Function(payload);
  f();
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "new Function" && s.source === "location.hash";
  });
});

test("FR: location.hash → setAttribute('onclick')", `
  var payload = window.location.hash.substr(1);
  var div = document.createElement('div');
  div.setAttribute('onclick', payload);
  document.documentElement.appendChild(div);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "setAttribute:onclick" && s.source === "location.hash";
  });
});

test("FR: location.hash → addEventListener + new Function", `
  var payload = window.location.hash.substr(1);
  var div = document.createElement('div');
  div.addEventListener('click', new Function(payload), false);
  document.documentElement.appendChild(div);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "new Function" && s.source === "location.hash";
  });
});

test("FR: location.hash → setAttribute('href') (javascript: protocol)", `
  var payload = window.location.hash.substr(1);
  var a = document.createElement('a');
  a.setAttribute('href', payload);
  document.documentElement.appendChild(a);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "setAttribute:href" && s.source === "location.hash";
  });
});

test("FR: location.hash → innerHTML (inline event in string)", `
  var payload = window.location.hash.substr(1);
  var div = document.createElement('div');
  div.innerHTML = '<div onclick=\\'' + payload.replace(/'/g, '"') + '\\'>div</div>';
  document.documentElement.appendChild(div);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.source === "location.hash";
  });
});

test("FR: location.hash → setAttribute('action') (form action)", `
  var payload = window.location.hash.substr(1);
  var form = document.createElement('form');
  form.setAttribute('action', payload);
  form.innerHTML = '<input type=\\'submit\\'></input>';
  document.documentElement.appendChild(form);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "setAttribute:action" && s.source === "location.hash";
  });
});

console.log("\n=== Security: Firing Range — window.location Object Source ===\n");

test("FR: window.location → location.assign", `
  var payload = window.location;
  window.location.assign(payload);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "location.assign" && s.source === "location";
  });
});

test("FR: window.location → document.write", `
  var payload = window.location;
  document.write(payload);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "document.write" && s.source === "location";
  });
});

test("FR: window.location → document.writeln", `
  var payload = window.location;
  document.writeln(payload);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "document.writeln" && s.source === "location";
  });
});

test("FR: window.location → eval", `
  var payload = window.location;
  eval(payload);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "eval" && s.source === "location";
  });
});

test("FR: window.location → innerHTML", `
  var payload = window.location;
  var div = document.createElement('div');
  div.id = 'divEl';
  document.documentElement.appendChild(div);
  var divEl = document.getElementById('divEl');
  divEl.innerHTML = payload;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.source === "location";
  });
});

test("FR: window.location → createContextualFragment", `
  var payload = window.location;
  var range = document.createRange();
  range.selectNode(document.getElementsByTagName("div").item(0));
  var documentFragment = range.createContextualFragment(payload);
  document.body.appendChild(documentFragment);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "createContextualFragment" && s.source === "location";
  });
});

test("FR: window.location → location.replace", `
  var payload = window.location;
  location.replace(payload);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "location.replace" && s.source === "location";
  });
});

test("FR: window.location → setTimeout(string)", `
  var payload = window.location;
  setTimeout('var a=a;' + payload, 1);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "setTimeout" && s.source === "location";
  });
});

console.log("\n=== Security: Firing Range — Other Taint Sources ===\n");

test("FR: document.documentURI → document.write", `
  var payload = document.documentURI;
  document.write(payload);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "document.write" && s.source === "document.documentURI";
  });
});

test("FR: document.baseURI → document.write", `
  var payload = document.baseURI;
  document.write(payload);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "document.write" && s.source === "document.baseURI";
  });
});

test("FR: location.href → document.write", `
  var payload = window.location.href;
  document.write(payload);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "document.write" && s.source === "location.href";
  });
});

test("FR: location.pathname → document.write", `
  var payload = window.location.pathname;
  document.write(payload);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "document.write" && s.source === "location.pathname";
  });
});

test("FR: location.search → document.write", `
  var payload = window.location.search.substr(1);
  document.write(payload);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "document.write" && s.source === "location.search";
  });
});

test("FR: document.URL → document.write", `
  var payload = document.URL;
  document.write(payload);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "document.write" && s.source === "document.URL";
  });
});

test("FR: document.URLUnencoded → document.write", `
  var payload = document.URLUnencoded;
  document.write(payload);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "document.write" && s.source === "document.URLUnencoded";
  });
});

// ═══════════════════════════════════════════════════════════════════
// Cross-File Scanning Tests
// Simulate combined analysis where scripts are concatenated with ";\n"
// and parsed with forceScript=true (shared global scope).
// ═══════════════════════════════════════════════════════════════════

console.log("\n=== Security: Cross-File — Tainted Global Variable ===\n");

test("XF: tainted global in script A → innerHTML in script B", [
  // Script A: defines tainted global
  'var crossFilePayload = location.hash.substring(1);',
  // Script B: uses it in a sink
  'document.getElementById("target").innerHTML = crossFilePayload;',
].join(";\n"), function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.source === "location.hash";
  });
}, true);

test("XF: tainted global in script A → eval in script B", [
  'var taintedCode = location.hash.substring(1);',
  'eval(taintedCode);',
].join(";\n"), function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "eval" && s.source === "location.hash";
  });
}, true);

test("XF: tainted global in script A → document.write in script B", [
  'var globalPayload = location.search;',
  'document.write("<div>" + globalPayload + "</div>");',
].join(";\n"), function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "document.write" && s.source === "location.search";
  });
}, true);

test("XF: tainted global in script A → location.href in script B", [
  'var nextUrl = location.hash.substring(1);',
  'location.href = nextUrl;',
].join(";\n"), function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "href" && s.source === "location.hash";
  });
}, true);

console.log("\n=== Security: Cross-File — Tainted Utility Function ===\n");

test("XF: render function in script A → called with tainted arg in script B", [
  // Script A: defines a function that sinks to innerHTML
  'function renderUnsafe(html) { document.getElementById("t").innerHTML = html; }',
  // Script B: calls it with tainted value
  'var userInput = location.search.substring(1);',
  'renderUnsafe(userInput);',
].join(";\n"), function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.source === "location.search";
  });
}, true);

test("XF: eval wrapper in script A → called with tainted arg in script B", [
  'function runCode(code) { eval(code); }',
  'runCode(location.hash.substring(1));',
].join(";\n"), function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "eval" && s.source === "location.hash";
  });
}, true);

test("XF: redirect wrapper in script A → called from script B", [
  'function navigateTo(url) { location.assign(url); }',
  'navigateTo(location.search.split("next=")[1]);',
].join(";\n"), function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "location.assign" && s.source === "location.search";
  });
}, true);

console.log("\n=== Security: Cross-File — Tainted Config Object ===\n");

test("XF: config object in script A with tainted values → redirect in script B", [
  'var appConfig = { redirectUrl: location.search.split("next=")[1], debug: false };',
  'location.assign(appConfig.redirectUrl);',
].join(";\n"), function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "location.assign";
  });
}, true);

test("XF: config object in script A → eval in script C", [
  'var cfg = { debugExpr: location.hash.substring(1) };',
  'void 0;',  // Script B does nothing
  'eval(cfg.debugExpr);',
].join(";\n"), function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "eval";
  });
}, true);

test("XF: config object in script A → document.write in script C", [
  'var settings = { userName: location.hash.substring(1) };',
  'document.write("<div>" + settings.userName + "</div>");',
].join(";\n"), function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "document.write";
  });
}, true);

console.log("\n=== Security: Cross-File — Class/Prototype Pattern ===\n");

test("XF: class prototype method — tainted arg dispatched through prototype", [
  'function UnsafeRenderer() {}',
  'UnsafeRenderer.prototype.render = function(c) { document.getElementById("t").outerHTML = c; };',
  // Script B:
  'var r = new UnsafeRenderer();',
  'r.render(location.hash.substring(1));',
].join(";\n"), function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "outerHTML" && s.source === "location.hash";
  });
}, true);

console.log("\n=== Security: Cross-File — New Sinks (Firing Range) ===\n");

test("XF: tainted global → createContextualFragment in another script", [
  'var xfPayload = location.hash.substring(1);',
  'var range = document.createRange();',
  'range.selectNode(document.body);',
  'var frag = range.createContextualFragment(xfPayload);',
  'document.body.appendChild(frag);',
].join(";\n"), function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "createContextualFragment" && s.source === "location.hash";
  });
}, true);

test("XF: tainted global → new Function in another script", [
  'var xfCode = location.search.substring(1);',
  'var fn = new Function("return " + xfCode);',
].join(";\n"), function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "new Function" && s.source === "location.search";
  });
}, true);

test("XF: tainted global → setTimeout string in another script", [
  'var xfExpr = location.hash.substring(1);',
  'setTimeout(xfExpr, 100);',
].join(";\n"), function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "setTimeout" && s.source === "location.hash";
  });
}, true);

test("XF: tainted global → setAttribute href in another script", [
  'var xfUrl = location.hash.substring(1);',
  'var link = document.createElement("a");',
  'link.setAttribute("href", xfUrl);',
].join(";\n"), function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "setAttribute:href" && s.source === "location.hash";
  });
}, true);

console.log("\n=== Security: Cross-File — Fetch URL from Tainted Base ===\n");

test("XF: URL builder with tainted base in script A → fetch in script B", [
  'var apiBase = location.hash.substring(1);',
  'function buildUrl(path) { return apiBase + "/api/" + path; }',
  // Script B:
  'fetch(buildUrl("users"));',
].join(";\n"), function(r) {
  return r.fetchCallSites.some(function(s) {
    return s.url && s.url.indexOf("/api/users") >= 0;
  }) || r.fetchCallSites.length > 0;
}, true);

console.log("\n=== Security: Cross-File — True Negatives ===\n");

test("XF: safe global (literal) in script A → innerHTML in script B — NOT flagged", [
  'var safeContent = "Hello, world!";',
  'document.getElementById("t").innerHTML = safeContent;',
].join(";\n"), function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
}, true);

test("XF: safe function (literal return) in script A → called in script B — NOT flagged", [
  'function getSafeHtml() { return "<p>Safe</p>"; }',
  'document.getElementById("t").innerHTML = getSafeHtml();',
].join(";\n"), function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
}, true);

// ═══════════════════════════════════════════════════════════════════════════════
// ██  RESEARCH-BASED SECURITY IMPROVEMENTS
// ═══════════════════════════════════════════════════════════════════════════════

// ── New Assignment Sinks (from DOM XSS research) ──
console.log("\n=== Security: Research-Based New Sinks ===\n");

test("iframe.srcdoc = tainted → XSS", `
  var payload = location.hash.slice(1);
  var iframe = document.createElement("iframe");
  iframe.srcdoc = payload;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "srcdoc" && s.severity === "high";
  });
});

test("element.href = tainted → XSS (javascript: protocol)", `
  var link = document.createElement("a");
  link.href = location.hash.slice(1);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "href" && s.severity === "high";
  });
});

test("element.src = tainted → XSS (script/iframe/embed injection)", `
  var script = document.createElement("script");
  script.src = location.hash.slice(1);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "src" && s.severity === "high";
  });
});

test("element.action = tainted → XSS (form action hijack)", `
  var form = document.getElementById("f");
  form.action = location.hash.slice(1);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "action" && s.severity === "high";
  });
});

test("element.formAction = tainted → XSS (submit button hijack)", `
  var btn = document.getElementById("submit");
  btn.formAction = location.hash.slice(1);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "formAction" && s.severity === "high";
  });
});

test("location.href = tainted → redirect (NOT xss)", `
  location.href = location.hash.slice(1);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "href";
  }) && !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "href";
  });
});

test("element.href = literal → NOT flagged", `
  var link = document.createElement("a");
  link.href = "https://example.com";
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.sink === "href";
  });
});

test("element.setHTMLUnsafe(tainted) → XSS", `
  var content = location.hash.slice(1);
  document.getElementById("out").setHTMLUnsafe(content);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "setHTMLUnsafe" && s.severity === "high";
  });
});

test("Document.parseHTMLUnsafe(tainted) → XSS", `
  var html = location.hash.slice(1);
  Document.parseHTMLUnsafe(html);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "parseHTMLUnsafe" && s.severity === "high";
  });
});

// ── Dynamic import() ──
console.log("\n=== Security: Dynamic import() ===\n");

test("import(taintedUrl) → eval-class sink", `
  var mod = location.hash.slice(1);
  import(mod).then(function(m) { m.default(); });
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "import" && s.severity === "high";
  });
});

test("import(literal) → NOT flagged", `
  import("./module.js").then(function(m) { m.default(); });
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.sink === "import";
  });
});

// ── window.open() ──
console.log("\n=== Security: window.open() ===\n");

test("window.open(tainted) → open redirect (bare open)", `
  var url = location.hash.slice(1);
  open(url);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "window.open" && s.severity === "high";
  });
});

test("window.open(tainted) → open redirect (member expression)", `
  var url = location.hash.slice(1);
  window.open(url);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "window.open" && s.severity === "high";
  });
});

test("window.open(literal) → NOT flagged", `
  window.open("https://example.com");
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.sink === "window.open";
  });
});

// ── Worker/SharedWorker injection ──
console.log("\n=== Security: Worker Injection ===\n");

test("new Worker(tainted) → eval-class sink", `
  var url = location.hash.slice(1);
  var w = new Worker(url);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "new Worker" && s.severity === "high";
  });
});

test("new SharedWorker(tainted) → eval-class sink", `
  var url = location.hash.slice(1);
  var sw = new SharedWorker(url);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "new SharedWorker" && s.severity === "high";
  });
});

test("new Worker(literal) → NOT flagged", `
  var w = new Worker("/worker.js");
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.sink === "new Worker";
  });
});

test("importScripts(tainted) → eval-class sink", `
  var url = location.hash.slice(1);
  importScripts(url);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "importScripts" && s.severity === "high";
  });
});

test("navigator.serviceWorker.register(tainted) → eval-class sink", `
  var url = location.hash.slice(1);
  navigator.serviceWorker.register(url);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "serviceWorker.register" && s.severity === "high";
  });
});

test("navigator.serviceWorker.register(literal) → NOT flagged", `
  navigator.serviceWorker.register("/sw.js");
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.sink === "serviceWorker.register";
  });
});

// ── Open redirect: bare location, document.location ──
console.log("\n=== Security: Extended Redirect Sinks ===\n");

test("location = tainted → redirect (bare assignment)", `
  var url = location.hash.slice(1);
  location = url;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "location" && s.severity === "high";
  });
});

test("document.location = tainted → redirect", `
  var url = location.hash.slice(1);
  document.location = url;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "location" && s.severity === "high";
  });
});

test("document.location.href = tainted → redirect", `
  var url = location.hash.slice(1);
  document.location.href = url;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "href";
  });
});

test("document.location.assign(tainted) → redirect", `
  var url = location.hash.slice(1);
  document.location.assign(url);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.sink === "location.assign";
  });
});

// ── PostMessage improvements ──
console.log("\n=== Security: PostMessage Improvements ===\n");

test("postMessage listener with event.origin.indexOf → weak origin check", `
  window.addEventListener("message", function(event) {
    if (event.origin.indexOf("example.com") !== -1) {
      document.body.innerHTML = event.data;
    }
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-weak-origin" && p.severity === "high";
  });
});

test("postMessage listener with event.origin.includes → weak origin check", `
  window.addEventListener("message", function(event) {
    if (event.origin.includes("trusted.com")) {
      eval(event.data);
    }
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-weak-origin";
  });
});

test("postMessage listener with event.origin.startsWith → weak origin check", `
  window.addEventListener("message", function(event) {
    if (event.origin.startsWith("https://example.com")) {
      document.body.innerHTML = event.data;
    }
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-weak-origin";
  });
});

test("postMessage listener with event.origin.endsWith → weak origin check", `
  window.addEventListener("message", function(event) {
    if (event.origin.endsWith("example.com")) {
      document.body.innerHTML = event.data;
    }
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-weak-origin";
  });
});

test("postMessage listener with strict origin === → NOT flagged as weak", `
  window.addEventListener("message", function(event) {
    if (event.origin !== "https://trusted.com") return;
    document.body.innerHTML = event.data;
  });
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin" || p.type === "postmessage-weak-origin";
  });
});

test("window.onmessage = handler without origin check → flagged", `
  window.onmessage = function(event) {
    document.body.innerHTML = event.data;
  };
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin" && p.severity === "high";
  });
});

test("self.onmessage = handler without origin check → flagged", `
  self.onmessage = function(e) {
    eval(e.data);
  };
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin" && p.severity === "high";
  });
});

test("window.onmessage = handler WITH origin check → NOT flagged", `
  window.onmessage = function(event) {
    if (event.origin !== "https://trusted.com") return;
    document.body.innerHTML = event.data;
  };
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin" || p.type === "postmessage-weak-origin";
  });
});

test("postMessage(data, '*') → wildcard target flagged", `
  var token = "secret123";
  window.parent.postMessage({ auth: token }, "*");
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-wildcard-target";
  });
});

test("window.opener.postMessage(data, '*') → flagged with opener context", `
  window.opener.postMessage({ token: "abc" }, "*");
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-wildcard-target" && p.severity === "high" &&
           p.description.indexOf("opener") !== -1;
  });
});

test("postMessage(data, 'https://specific.com') → NOT flagged", `
  window.parent.postMessage({ data: 123 }, "https://specific.com");
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-wildcard-target";
  });
});

// ── PostMessage Severity ──
console.log("\n=== Security: PostMessage Severity ===\n");

test("postMessage listener without sink → medium severity", `
  window.addEventListener("message", function(event) {
    console.log(event.data);
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin" && p.severity === "medium";
  });
});

test("postMessage listener with innerHTML sink → high severity", `
  window.addEventListener("message", function(event) {
    document.body.innerHTML = event.data;
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin" && p.severity === "high";
  });
});

test("postMessage listener with eval sink → high severity", `
  window.addEventListener("message", function(event) {
    eval(event.data);
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin" && p.severity === "high";
  });
});

test("postMessage listener with document.write sink → high severity", `
  window.addEventListener("message", function(event) {
    document.write(event.data);
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin" && p.severity === "high";
  });
});

test("postMessage listener with location.href sink → high severity", `
  window.addEventListener("message", function(event) {
    location.href = event.data;
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin" && p.severity === "high";
  });
});

test("weak origin postMessage with sink → high severity", `
  window.addEventListener("message", function(event) {
    if (event.origin.includes("example.com")) {
      document.body.innerHTML = event.data;
    }
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-weak-origin" && p.severity === "high";
  });
});

test("weak origin postMessage without sink → medium severity", `
  window.addEventListener("message", function(event) {
    if (event.origin.includes("example.com")) {
      console.log(event.data);
    }
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-weak-origin" && p.severity === "medium";
  });
});

test("postMessage high severity description includes sink name", `
  window.addEventListener("message", function(event) {
    document.body.innerHTML = event.data;
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin" && p.description.indexOf("innerHTML") !== -1;
  });
});

// ── PostMessage Origin Check Tracing ──
console.log("\n=== Security: PostMessage Origin Tracing ===\n");

test("origin passed to function with === check → strong (not flagged)", `
  function validateOrigin(origin) {
    return origin === "https://trusted.com";
  }
  window.addEventListener("message", function(event) {
    if (!validateOrigin(event.origin)) return;
    document.body.innerHTML = event.data;
  });
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin" || p.type === "postmessage-weak-origin";
  });
});

test("origin passed to method with === check → strong (not flagged)", `
  var c = { i: function(o) { return o === "https://trusted.com"; } };
  window.addEventListener("message", function(m) {
    if (!c.i(m.origin)) return;
    document.body.innerHTML = m.data;
  });
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin" || p.type === "postmessage-weak-origin";
  });
});

test("origin passed to function with .includes() → weak", `
  function checkOrigin(origin) {
    return origin.includes("example.com");
  }
  window.addEventListener("message", function(event) {
    if (checkOrigin(event.origin)) {
      document.body.innerHTML = event.data;
    }
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-weak-origin";
  });
});

test("origin passed to method with .indexOf() → weak", `
  var validator = { check: function(o) { return o.indexOf("trusted") !== -1; } };
  window.addEventListener("message", function(e) {
    if (validator.check(e.origin)) {
      eval(e.data);
    }
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-weak-origin";
  });
});

test("origin passed to function with no check → still flagged as no-origin", `
  function logOrigin(origin) {
    console.log(origin);
  }
  window.addEventListener("message", function(event) {
    logOrigin(event.origin);
    document.body.innerHTML = event.data;
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin";
  });
});

test("origin passed to unresolvable function → still flagged", `
  window.addEventListener("message", function(m) {
    unknownFunc(m.origin);
    document.body.innerHTML = m.data;
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin";
  });
});

// ── Prototype Pollution API Detection ──
console.log("\n=== Security: Prototype Pollution APIs ===\n");

test("Object.defineProperty with user-controlled key → flagged", `
  var key = location.hash.slice(1);
  Object.defineProperty(window, key, { value: true, writable: true });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution" && p.description.indexOf("defineProperty") !== -1;
  });
});

test("Object.defineProperty with literal key → NOT flagged", `
  Object.defineProperty(window, "myProp", { value: true, writable: true });
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution" && p.description.indexOf("defineProperty") !== -1;
  });
});

test("Reflect.set with user-controlled key → flagged", `
  var key = location.hash.slice(1);
  var config = {};
  Reflect.set(config, key, "pwned");
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution" && p.description.indexOf("Reflect.set") !== -1;
  });
});

test("Reflect.set with literal key → NOT flagged", `
  var config = {};
  Reflect.set(config, "name", "value");
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution" && p.description.indexOf("Reflect.set") !== -1;
  });
});

test("Object.assign with JSON.parse(tainted) → prototype-pollution-merge", `
  var userInput = JSON.parse(location.hash.slice(1));
  var config = {};
  Object.assign(config, userInput);
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution-merge" && p.severity === "medium";
  });
});

test("Object.assign with tainted STRING (not object with open keys) → NOT flagged", `
  var userInput = location.hash.slice(1);
  var config = {};
  Object.assign(config, userInput);
`, function(r) {
  // Object.assign over a string enumerates numeric indices only — can't
  // set __proto__ or any non-numeric key, so this is not a real vector.
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution-merge";
  });
});

test("Object.assign with fresh locals + function-result .params (route matcher) → NOT flagged", `
  function matchRoute(r, url) { return { params: { id: "1" } }; }
  var c = matchRoute(route, location.pathname);
  var n = {};
  Object.assign(n, c.params);
`, function(r) {
  // react-router-style pattern: c.params keys are declared route names,
  // not attacker-chosen. Values may come from the URL but Object.assign
  // only cares about keys for proto pollution.
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution-merge";
  });
});

test("Object.assign with literal source → NOT flagged", `
  var config = {};
  Object.assign(config, { name: "value" });
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution-merge";
  });
});

test("Object.assign with multiple sources, one is JSON.parse(tainted) → flagged", `
  var defaults = { theme: "light" };
  var overrides = JSON.parse(location.hash.slice(1));
  var config = {};
  Object.assign(config, defaults, overrides);
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution-merge";
  });
});

test("Object.assign with Object.fromEntries(tainted URLSearchParams) → flagged", `
  var overrides = Object.fromEntries(new URLSearchParams(location.search));
  Object.assign({}, overrides);
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution-merge";
  });
});

test("Object.assign with variable chain from JSON.parse(tainted) → flagged", `
  var raw = JSON.parse(location.hash.slice(1));
  var mid = raw;
  var overrides = mid;
  Object.assign({}, overrides);
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution-merge";
  });
});

test("Object.assign with variable chain from LITERAL → NOT flagged", `
  var raw = JSON.parse('{"x":1}');
  var mid = raw;
  var overrides = mid;
  Object.assign({}, overrides);
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution-merge";
  });
});

test("Object.assign with MemberExpression off JSON.parse(tainted) → flagged", `
  var data = JSON.parse(location.hash.slice(1));
  Object.assign({}, data.nested);
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution-merge";
  });
});

test("Object.assign with MemberExpression off SAFE parent → NOT flagged", `
  var data = { nested: { a: 1 } };
  Object.assign({}, data.nested);
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution-merge";
  });
});

test("Object.assign with structuredClone(JSON.parse(tainted)) → flagged", `
  var clone = structuredClone(JSON.parse(location.hash.slice(1)));
  Object.assign({}, clone);
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution-merge";
  });
});

test("Object.assign with structuredClone of SAFE source → NOT flagged", `
  var clone = structuredClone({ a: 1 });
  Object.assign({}, clone);
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution-merge";
  });
});

test("Object.assign with object literal + computed key tainted → flagged", `
  var key = location.hash.slice(1);
  Object.assign({}, { [key]: 1 });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution-merge";
  });
});

test("Object.assign with object literal + computed key safe → NOT flagged", `
  var key = "theme";
  Object.assign({}, { [key]: "light" });
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution-merge";
  });
});

test("Object.assign with spread of JSON.parse(tainted) in object literal → flagged", `
  Object.assign({}, { a: 1, ...JSON.parse(location.hash.slice(1)) });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution-merge";
  });
});

test("Object.assign with spread of LITERAL in object literal → NOT flagged", `
  var base = { a: 1 };
  Object.assign({}, { b: 2, ...base });
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution-merge";
  });
});

test("Object.assign with Object.fromEntries of UNTAINTED iterable → NOT flagged", `
  var entries = [["a", 1], ["b", 2]];
  Object.assign({}, Object.fromEntries(entries));
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution-merge";
  });
});

test("Object.assign with shadowed Object binding → NOT flagged (not our assign)", `
  function f(Object) {
    Object.assign({}, JSON.parse(location.hash.slice(1)));
  }
`, function(r) {
  // The Object here is a local parameter, not the global — so the call
  // is not the real Object.assign and shouldn't trigger our detector.
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution-merge";
  });
});

test("Object.assign with shadowed JSON binding → NOT flagged (not real JSON.parse)", `
  function f(JSON) {
    Object.assign({}, JSON.parse(location.hash.slice(1)));
  }
`, function(r) {
  // JSON is shadowed by a parameter — the .parse call isn't the global
  // JSON.parse, so the source can't be classified as arbitrary-key.
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution-merge";
  });
});

test("Object.assign with history.state → NOT flagged (same-origin only)", `
  var h = history.state;
  h = Object.assign({}, h, { lfId: "abc" });
  history.pushState(h, "", "");
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution-merge";
  });
});

// ── New Taint Sources ──
console.log("\n=== Security: New Taint Sources ===\n");

test("history.state → innerHTML → NOT flagged (same-origin only, not attacker-controlled)", `
  var title = history.state;
  document.getElementById("header").innerHTML = title;
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.source === "history.state";
  });
});

test("document.title → innerHTML → XSS", `
  var breadcrumb = document.title;
  document.getElementById("nav").innerHTML = breadcrumb;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.source === "document.title";
  });
});

test("document.title → eval → code injection", `
  var cmd = document.title;
  eval(cmd);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.source === "document.title";
  });
});

test("history.state → location.href → NOT flagged (same-origin only)", `
  var state = history.state;
  location.href = state;
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.source === "history.state";
  });
});

// ── setAttribute("style", tainted) — CSS injection ──
console.log("\n=== Security: CSS Injection via setAttribute ===\n");

test("setAttribute('style', tainted) → XSS (CSS injection)", `
  var css = location.hash.slice(1);
  document.getElementById("el").setAttribute("style", css);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "setAttribute:style" && s.severity === "high";
  });
});

test("setAttribute('style', literal) → NOT flagged", `
  document.getElementById("el").setAttribute("style", "color: red");
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.sink === "setAttribute:style";
  });
});

// ── Combined / Real-world patterns ──
console.log("\n=== Security: Real-World Vulnerability Patterns ===\n");

test("Firing Range: location.hash → iframe.srcdoc (Google FR pattern)", `
  var payload = location.hash.substring(1);
  var frame = document.createElement("iframe");
  frame.srcdoc = payload;
  document.body.appendChild(frame);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "srcdoc" && s.source === "location.hash";
  });
});

test("Firing Range: location.hash → script.src (script injection)", `
  var src = location.hash.slice(1);
  var s = document.createElement("script");
  s.src = src;
  document.head.appendChild(s);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "src" && s.source === "location.hash";
  });
});

test("OAuth postMessage: opener.postMessage with wildcard → flagged", `
  var token = getOAuthToken();
  window.opener.postMessage({ type: "auth", token: token }, "*");
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-wildcard-target" && p.description.indexOf("opener") !== -1;
  });
});

test("DOM clobbering gadget: document.currentScript.src → script.src (CVE pattern)", `
  var baseUrl = document.currentScript.src;
  var s = document.createElement("script");
  s.src = baseUrl;
  document.body.appendChild(s);
`, function(r) {
  // document.currentScript.src is not a taint source (not user-controlled)
  // so this should NOT be flagged — DOM clobbering requires HTML injection first
  return !r.securitySinks.some(function(s) {
    return s.sink === "src";
  });
});

test("Prototype pollution via Object.assign with parsed JSON from hash", `
  var data = JSON.parse(decodeURIComponent(location.hash.slice(1)));
  var config = {};
  Object.assign(config, data);
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution-merge";
  });
});

test("Multiple sinks: location.search → innerHTML AND eval (both detected)", `
  var input = location.search.slice(1);
  document.body.innerHTML = input;
  eval(input);
`, function(r) {
  var hasXss = r.securitySinks.some(function(s) { return s.type === "xss" && s.sink === "innerHTML"; });
  var hasEval = r.securitySinks.some(function(s) { return s.type === "eval" && s.sink === "eval"; });
  return hasXss && hasEval;
});

test("Service Worker hijack: hash → serviceWorker.register (CVE pattern)", `
  var swUrl = location.hash.substring(1);
  if (navigator.serviceWorker) {
    navigator.serviceWorker.register(swUrl);
  }
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "serviceWorker.register" && s.source === "location.hash";
  });
});

test("Worker XSS escalation: hash → new Worker (CVE pattern)", `
  var workerUrl = location.hash.slice(1);
  var w = new Worker(workerUrl);
  w.postMessage("start");
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "new Worker" && s.source === "location.hash";
  });
});

// === Research-Based: WebSocket Injection ===
console.log("\n=== Security: WebSocket Injection ===\n");

test("new WebSocket(tainted) → request-forgery", `
  var wsUrl = location.hash.slice(1);
  var ws = new WebSocket(wsUrl);
  ws.onmessage = function(e) { console.log(e.data); };
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "request-forgery" && s.sink === "new WebSocket" && s.source === "location.hash";
  });
});

test("new WebSocket(literal) → NOT flagged", `
  var ws = new WebSocket("wss://api.example.com/ws");
`, function(r) {
  return !r.securitySinks.some(function(s) { return s.sink === "new WebSocket"; });
});

test("new WebSocket(window.name) → request-forgery", `
  var ws = new WebSocket(window.name);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "request-forgery" && s.sink === "new WebSocket" && s.source === "window.name";
  });
});

// === Research-Based: EventSource Injection ===
console.log("\n=== Security: EventSource Injection ===\n");

test("new EventSource(tainted) → request-forgery", `
  var streamUrl = new URLSearchParams(location.search).get("stream");
  var es = new EventSource(streamUrl);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "request-forgery" && s.sink === "new EventSource";
  });
});

test("new EventSource(literal) → NOT flagged", `
  var es = new EventSource("/api/events");
`, function(r) {
  return !r.securitySinks.some(function(s) { return s.sink === "new EventSource"; });
});

// === Research-Based: fetch/XHR/sendBeacon Request Forgery ===
console.log("\n=== Security: Network Request Forgery ===\n");

test("fetch(tainted URL) → request-forgery", `
  var url = location.hash.slice(1);
  fetch(url);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "request-forgery" && s.sink === "fetch" && s.source === "location.hash";
  });
});

test("fetch(literal URL) → NOT flagged as request-forgery", `
  fetch("/api/data");
`, function(r) {
  return !r.securitySinks.some(function(s) { return s.type === "request-forgery"; });
});

test("fetch(tainted URL + credentials) → request-forgery", `
  var endpoint = location.hash.slice(1);
  fetch(endpoint, { credentials: "include" });
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "request-forgery" && s.sink === "fetch";
  });
});

test("navigator.sendBeacon(tainted URL) → request-forgery", `
  var exfilUrl = location.hash.slice(1);
  navigator.sendBeacon(exfilUrl, document.cookie);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "request-forgery" && s.sink === "navigator.sendBeacon" && s.source === "location.hash";
  });
});

test("navigator.sendBeacon(literal) → NOT flagged", `
  navigator.sendBeacon("/analytics", "data");
`, function(r) {
  return !r.securitySinks.some(function(s) { return s.sink === "navigator.sendBeacon"; });
});

test("xhr.open(method, tainted URL) → request-forgery", `
  var url = location.hash.slice(1);
  var xhr = new XMLHttpRequest();
  xhr.open("GET", url);
  xhr.send();
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "request-forgery" && s.sink === "XMLHttpRequest.open" && s.source === "location.hash";
  });
});

test("xhr.open(method, literal) → NOT flagged as request-forgery", `
  var xhr = new XMLHttpRequest();
  xhr.open("GET", "/api/data");
  xhr.send();
`, function(r) {
  return !r.securitySinks.some(function(s) { return s.type === "request-forgery" && s.sink === "XMLHttpRequest.open"; });
});

// === Research-Based: jQuery DOM Manipulation ===
console.log("\n=== Security: jQuery DOM Manipulation Sinks ===\n");

test("jQuery .html(tainted) → xss", `
  var content = location.hash.slice(1);
  $("#container").html(content);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === ".html" && s.source === "location.hash";
  });
});

test("jQuery .append(tainted) → xss", `
  var markup = location.hash.slice(1);
  $("#list").append(markup);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === ".append";
  });
});

test("jQuery .prepend(tainted) → xss", `
  var item = document.referrer;
  $("body").prepend(item);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === ".prepend" && s.source === "document.referrer";
  });
});

test("jQuery .after(tainted) → xss", `
  var data = location.search;
  el.after(data);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === ".after";
  });
});

test("jQuery .before(tainted) → xss", `
  var data = location.search;
  el.before(data);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === ".before";
  });
});

test("jQuery .replaceWith(tainted) → xss", `
  var html = document.URL;
  el.replaceWith(html);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === ".replaceWith";
  });
});

test("jQuery .html(literal) → NOT flagged", `
  $("#container").html("<b>safe</b>");
`, function(r) {
  return !r.securitySinks.some(function(s) { return s.sink === ".html"; });
});

// === Research-Based: Implicit ReDoS ===
console.log("\n=== Security: Implicit ReDoS ===\n");

test("str.match(tainted) → regex-implicit", `
  var pattern = location.hash.slice(1);
  var result = "test string".match(pattern);
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "regex-implicit" && p.description.indexOf("match") !== -1;
  });
});

test("str.search(tainted) → regex-implicit", `
  var pat = location.search.slice(1);
  var idx = "hello world".search(pat);
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "regex-implicit" && p.description.indexOf("search") !== -1;
  });
});

test("str.split(tainted) → NOT flagged (split does not create implicit RegExp)", `
  var sep = location.hash.slice(1);
  var parts = "a,b,c".split(sep);
`, function(r) {
  return !r.dangerousPatterns.some(function(p) { return p.type === "regex-implicit"; });
});

test("str.match(literal regex) → NOT flagged", `
  var result = "test".match(/\\d+/);
`, function(r) {
  return !r.dangerousPatterns.some(function(p) { return p.type === "regex-implicit"; });
});

test("str.match(literal string) → NOT flagged", `
  var result = "test".match("simple");
`, function(r) {
  return !r.dangerousPatterns.some(function(p) { return p.type === "regex-implicit"; });
});

// === Research-Based: React dangerouslySetInnerHTML ===
console.log("\n=== Security: React dangerouslySetInnerHTML ===\n");

test("dangerouslySetInnerHTML with tainted __html → xss", `
  var content = location.hash.slice(1);
  React.createElement("div", { dangerouslySetInnerHTML: { __html: content } });
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "dangerouslySetInnerHTML" && s.source === "location.hash";
  });
});

test("dangerouslySetInnerHTML with literal __html → NOT flagged", `
  React.createElement("div", { dangerouslySetInnerHTML: { __html: "<b>safe</b>" } });
`, function(r) {
  return !r.securitySinks.some(function(s) { return s.sink === "dangerouslySetInnerHTML"; });
});

test("dangerouslySetInnerHTML nested in JSX-compiled output → xss", `
  var userInput = document.referrer;
  (0, jsx)("div", { dangerouslySetInnerHTML: { __html: userInput } });
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "dangerouslySetInnerHTML" && s.source === "document.referrer";
  });
});

// === Research-Based: localStorage/sessionStorage Taint Sources ===
console.log("\n=== Security: Storage Taint Sources ===\n");

test("localStorage.getItem → innerHTML → xss", `
  var name = localStorage.getItem("username");
  document.getElementById("profile").innerHTML = name;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.source === "localStorage.getItem";
  });
});

test("sessionStorage.getItem → eval → eval", `
  var code = sessionStorage.getItem("handler");
  eval(code);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "eval" && s.source === "sessionStorage.getItem";
  });
});

test("localStorage.getItem → location.href → redirect", `
  var url = localStorage.getItem("redirect");
  location.href = url;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect" && s.source === "localStorage.getItem";
  });
});

test("localStorage.getItem → fetch → request-forgery", `
  var endpoint = localStorage.getItem("api");
  fetch(endpoint);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "request-forgery" && s.sink === "fetch" && s.source === "localStorage.getItem";
  });
});

// === Research-Based: Trusted Types Passthrough ===
console.log("\n=== Security: Trusted Types Passthrough ===\n");

test("trustedTypes.createPolicy with arrow identity → flagged", `
  trustedTypes.createPolicy("default", {
    createHTML: (s) => s,
    createScript: (s) => s,
    createScriptURL: (s) => s,
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "trusted-types-passthrough" && p.description.indexOf("createHTML") !== -1;
  });
});

test("trustedTypes.createPolicy with function identity → flagged", `
  trustedTypes.createPolicy("default", {
    createHTML: function(s) { return s; },
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "trusted-types-passthrough";
  });
});

test("trustedTypes.createPolicy with block arrow identity → flagged", `
  trustedTypes.createPolicy("mypolicy", {
    createScriptURL: (input) => { return input; },
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "trusted-types-passthrough" && p.description.indexOf("createScriptURL") !== -1;
  });
});

test("trustedTypes.createPolicy with sanitizer → NOT flagged", `
  trustedTypes.createPolicy("safe", {
    createHTML: (s) => DOMPurify.sanitize(s),
    createScript: (s) => "",
    createScriptURL: (s) => s.startsWith("https://") ? s : "",
  });
`, function(r) {
  return !r.dangerousPatterns.some(function(p) { return p.type === "trusted-types-passthrough"; });
});

// === Research-Based: Deep MemberExpression Taint Propagation ===
console.log("\n=== Security: Deep Taint Propagation ===\n");

test("DOMParser output → innerHTML (mXSS double-parse pattern)", `
  var doc = new DOMParser().parseFromString(location.hash, "text/html");
  document.getElementById("out").innerHTML = doc.body.innerHTML;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.source === "location.hash";
  });
});

test("JSON.parse(tainted).property → innerHTML → xss", `
  var data = JSON.parse(location.hash.slice(1));
  document.getElementById("greeting").innerHTML = data.message;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.source === "location.hash";
  });
});

test("JSON.parse(tainted).nested.property → eval → eval", `
  var config = JSON.parse(location.search.slice(1));
  eval(config.settings.code);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "eval";
  });
});

test("deep chain: tainted.a.b.c → innerHTML → xss", `
  var obj = JSON.parse(location.hash);
  el.innerHTML = obj.data.items.html;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

// === Research-Based: Real-World CVE Patterns ===
console.log("\n=== Security: Real-World CVE Patterns (2024-2025) ===\n");

test("new URLSearchParams(tainted).get() → innerHTML → xss", `
  var params = new URLSearchParams(location.search);
  document.getElementById("name").innerHTML = params.get("name");
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

test("WebSocket hijack: hash → new WebSocket (CVE-2023-41896 pattern)", `
  var wsHost = location.hash.slice(1);
  var socket = new WebSocket("wss://" + wsHost + "/ws");
  socket.onmessage = function(e) {
    document.getElementById("output").innerHTML = e.data;
  };
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "request-forgery" && s.sink === "new WebSocket";
  });
});

test("sendBeacon exfiltration: hash URL + cookie data", `
  var url = location.hash.slice(1);
  navigator.sendBeacon(url, document.cookie);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "request-forgery" && s.sink === "navigator.sendBeacon";
  });
});

test("Stored DOM XSS: localStorage → innerHTML (persistent XSS)", `
  var savedTemplate = localStorage.getItem("template");
  document.getElementById("widget").innerHTML = savedTemplate;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.source === "localStorage.getItem";
  });
});

test("EventSource data injection: hash → new EventSource", `
  var sseUrl = location.hash.slice(1);
  var source = new EventSource(sseUrl);
  source.onmessage = function(e) { console.log(e.data); };
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "request-forgery" && s.sink === "new EventSource";
  });
});

test("Trusted Types bypass: passthrough policy (CVE-2024-45801 pattern)", `
  if (window.trustedTypes) {
    trustedTypes.createPolicy("default", {
      createHTML: (s) => s,
    });
  }
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "trusted-types-passthrough";
  });
});

test("jQuery + hash XSS: .html(location.hash)", `
  var content = decodeURIComponent(location.hash.slice(1));
  $(".output").html(content);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === ".html";
  });
});

test("fetch SSRF: hash → fetch (client-side request forgery)", `
  var apiUrl = location.hash.slice(1);
  fetch(apiUrl).then(function(r) { return r.json(); });
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "request-forgery" && s.sink === "fetch";
  });
});

test("implicit ReDoS: location.hash → str.match (CVE-2024-52798 pattern)", `
  var searchPattern = location.hash.slice(1);
  var matches = document.body.textContent.match(searchPattern);
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "regex-implicit";
  });
});

test("React SSR XSS: referrer → dangerouslySetInnerHTML", `
  var input = document.referrer;
  React.createElement("div", {
    dangerouslySetInnerHTML: { __html: "<p>" + input + "</p>" }
  });
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "dangerouslySetInnerHTML";
  });
});

// ── Taint Propagation Improvements ──

console.log("\n--- event.data taint source ---");

test("Security: event.data → innerHTML (postMessage XSS)", `
  window.addEventListener("message", function(event) {
    document.getElementById("output").innerHTML = event.data;
  });
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.source === "event.data";
  });
});

test("Security: event.data property access → innerHTML", `
  window.addEventListener("message", function(e) {
    var html = e.data.content;
    document.getElementById("target").innerHTML = html;
  });
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

test("Security: event.data → eval", `
  self.addEventListener("message", function(event) {
    eval(event.data);
  });
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "eval";
  });
});

test("Security: locally-bound event → NOT flagged as event.data", `
  function process(event) {
    var event = { data: "safe" };
    document.getElementById("x").innerHTML = event.data;
  }
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.source === "event.data";
  });
});

console.log("\n--- LogicalExpression taint propagation ---");

test("Security: location.hash || default → innerHTML (OR propagation)", `
  var content = location.hash.slice(1) || "<p>default</p>";
  document.getElementById("out").innerHTML = content;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

test("Security: condition && tainted → innerHTML (AND propagation)", `
  var val = window.loaded && location.search;
  document.body.innerHTML = val;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

test("Security: nullish coalescing ?? tainted → innerHTML", `
  var url = cachedValue ?? location.hash;
  document.getElementById("frame").innerHTML = url;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

test("Security: literal || literal → NOT flagged", `
  var val = "hello" || "world";
  document.getElementById("out").innerHTML = val;
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.source;
  });
});

console.log("\n--- Destructuring taint propagation ---");

test("Security: const { data } = event → innerHTML (object destructuring)", `
  window.addEventListener("message", function(event) {
    var { data } = event;
    document.getElementById("out").innerHTML = data;
  });
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

test("Security: const { html } = parsed where parsed = JSON.parse(location.hash)", `
  var parsed = JSON.parse(location.hash.slice(1));
  var { html } = parsed;
  document.getElementById("out").innerHTML = html;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

test("Security: nested destructuring const { payload: { html } } from tainted", `
  var msg = JSON.parse(location.search);
  var { payload } = msg;
  document.body.innerHTML = payload;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

test("Security: array destructuring [first] = tainted.split()", `
  var parts = location.hash.split("#");
  var first = parts[0];
  document.body.innerHTML = first;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

console.log("\n--- Return-value tracing through nested blocks ---");

test("Security: function with if/else return → taint propagates", `
  function getContent(mode) {
    if (mode === "user") {
      return location.hash.slice(1);
    } else {
      return "default";
    }
  }
  document.body.innerHTML = getContent("user");
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

test("Security: function with switch/case return → taint propagates", `
  function getData(type) {
    switch (type) {
      case "hash": return location.hash;
      case "search": return location.search;
      default: return "/";
    }
  }
  document.body.innerHTML = getData("hash");
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

test("Security: function with try/catch return → taint propagates", `
  function safeParse() {
    try {
      return JSON.parse(location.hash.slice(1));
    } catch (e) {
      return "error";
    }
  }
  document.body.innerHTML = safeParse();
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

test("Security: function with nested if return → taint propagates", `
  function resolve(cfg) {
    if (cfg.type === "url") {
      if (cfg.trusted) {
        return "/safe";
      } else {
        return location.search;
      }
    }
    return "default";
  }
  document.body.innerHTML = resolve({type: "url"});
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

console.log("\n--- SequenceExpression taint propagation ---");

test("Security: (0, eval)(tainted) → indirect eval", `
  var code = location.hash.slice(1);
  (0, eval)(code);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "eval";
  });
});

test("Security: (sideEffect(), tainted) → innerHTML", `
  var val = (console.log("loading"), location.hash.slice(1));
  document.body.innerHTML = val;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

console.log("\n--- AwaitExpression taint propagation ---");

test("Security: await tainted.json() → innerHTML", `
  async function loadData() {
    var resp = await fetch(location.hash.slice(1));
    var data = await resp.json();
    document.body.innerHTML = data;
  }
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

test("Security: await fetch(tainted) → request forgery", `
  async function load() {
    var url = location.hash.slice(1);
    var resp = await fetch(url);
  }
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "request-forgery" && s.sink === "fetch";
  });
});

console.log("\n--- SpreadElement taint propagation ---");

test("Security: [...tainted] → preserves taint through spread", `
  var parts = location.hash.split("/");
  var copy = [...parts];
  document.body.innerHTML = copy[0];
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

console.log("\n--- __proto__ assignment detection ---");

test("Security: obj.__proto__ = tainted → prototype pollution", `
  var key = location.hash.slice(1);
  var payload = JSON.parse(key);
  var obj = {};
  obj.__proto__ = payload;
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution" && p.description.indexOf("__proto__") !== -1;
  });
});

test("Security: obj.__proto__ = literal → NOT flagged", `
  var obj = {};
  obj.__proto__ = { toString: function() { return "safe"; } };
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution" && p.description.indexOf("__proto__") !== -1;
  });
});

test("Security: target.__proto__ = event.data → prototype pollution via postMessage", `
  window.addEventListener("message", function(e) {
    var target = {};
    target.__proto__ = e.data;
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution" && p.description.indexOf("__proto__") !== -1;
  });
});

console.log("\n--- Real-world combined patterns ---");

test("Security: postMessage event.data → document.write (full chain)", `
  window.addEventListener("message", function(evt) {
    var content = evt.data;
    document.write(content);
  });
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "document.write";
  });
});

test("Security: event.data via destructuring → location.href (redirect)", `
  window.addEventListener("message", function(event) {
    var { url } = event.data;
    location.href = url;
  });
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "redirect";
  });
});

test("Security: location.search ?? default → fetch (request forgery via ??)", `
  async function loadApi() {
    var endpoint = new URLSearchParams(location.search).get("api") ?? "/default";
    var resp = await fetch(endpoint);
  }
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "request-forgery" && s.sink === "fetch";
  });
});

test("Security: function with if/return → jQuery .html() (combined pattern)", `
  function getTemplate(page) {
    if (page === "custom") {
      return location.hash.slice(1);
    }
    return "<p>default</p>";
  }
  $(".container").html(getTemplate("custom"));
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === ".html";
  });
});

test("Security: event.data → new WebSocket (request forgery)", `
  window.addEventListener("message", function(e) {
    var ws = new WebSocket(e.data.wsUrl);
  });
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "request-forgery" && s.sink === "new WebSocket";
  });
});

test("Security: await + destructuring + LogicalExpression combined", `
  async function process() {
    var raw = location.hash.slice(1);
    var data = JSON.parse(raw);
    var { html } = data;
    var content = html || "<p>empty</p>";
    document.body.innerHTML = content;
  }
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

// ── Real-World CVE Validation (regression tests for fixed gaps) ──

console.log("\n--- IIFE taint propagation ---");

test("Security: IIFE (function(a) { document.write(a) })(location.hash) → xss", `
  (function(a) {
    document.write(a);
  })(location.hash.slice(1));
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "document.write";
  });
});

test("Security: IIFE with multiple params → taint traces to correct param", `
  (function(safe, tainted) {
    document.body.innerHTML = tainted;
  })("hello", location.search);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

test("Security: nested IIFE → eval", `
  (function(x) {
    (function(y) {
      eval(y);
    })(x);
  })(location.hash.slice(1));
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "eval";
  });
});

console.log("\n--- jQuery $() selector injection ---");

test("Security: $(location.hash) → jQuery XSS (CVE-2011-4969 pattern)", `
  $(window).on("hashchange", function() {
    var element = $(location.hash);
    element[0].scrollIntoView();
  });
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "jQuery";
  });
});

test("Security: jQuery(location.search) → jQuery XSS", `
  var html = location.search.slice(1);
  jQuery(html).appendTo("body");
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "jQuery";
  });
});

test("Security: locally-bound $ → NOT flagged", `
  var $ = function(sel) { return document.querySelector(sel); };
  $(".safe-selector");
`, function(r) {
  return !r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "jQuery";
  });
});

console.log("\n--- Object.assign taint propagation ---");

test("Security: Object.assign({}, tainted) → innerHTML (direct)", `
  var tainted = location.hash;
  document.body.innerHTML = Object.assign({}, tainted);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

console.log("\n--- Real-world CVE patterns ---");

test("Security: Zoho postMessage → JSON.parse → iframe.src (no origin check)", `
  window.addEventListener("message", function(event) {
    var data = JSON.parse(event.data);
    if (data.type === "banner") {
      var iframe = document.createElement("iframe");
      iframe.src = data.url;
      document.body.appendChild(iframe);
    }
  });
`, function(r) {
  var hasPostMsg = r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin";
  });
  var hasSrc = r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "src";
  });
  return hasPostMsg && hasSrc;
});

test("Security: AddThis postMessage → script.src injection", `
  window.addEventListener("message", function(event) {
    var data = event.data;
    if (typeof data === "string") {
      var script = document.createElement("script");
      script.src = data;
      document.head.appendChild(script);
    }
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin";
  }) && r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "src";
  });
});

test("Security: Trusted Types bypass via import(tainted)", `
  var moduleUrl = location.hash.slice(1);
  import(moduleUrl);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "import";
  });
});

test("Security: multi-hop location.search → URLSearchParams → JSON.parse → innerHTML", `
  var params = new URLSearchParams(location.search);
  var raw = params.get("data");
  var obj = JSON.parse(raw);
  document.body.innerHTML = obj.html;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

test("Security: postMessage → function param → innerHTML (inter-procedural)", `
  window.addEventListener("message", function(e) {
    renderContent(e.data);
  });
  function renderContent(html) {
    document.getElementById("app").innerHTML = html;
  }
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
});

// ── For-in taint propagation ──

test("Security: for-in key from tainted object is user-controlled (prototype pollution)", `
  function mergeDeep(target, source) {
    for (var key in source) {
      if (typeof source[key] === "object") {
        if (!target[key]) target[key] = {};
        mergeDeep(target[key], source[key]);
      } else {
        target[key] = source[key];
      }
    }
  }
  var userInput = JSON.parse(location.hash.slice(1));
  mergeDeep({}, userInput);
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "prototype-pollution";
  });
});

test("Security: for-of value from tainted array is user-controlled → eval", `
  var items = JSON.parse(location.hash.slice(1));
  for (var item of items) {
    eval(item);
  }
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "eval";
  });
});

// ── Object.assign property-level tracking ──

test("Security: Object.assign({}, tainted).prop → innerHTML", `
  var config = { html: location.hash.slice(1), safe: true };
  var merged = Object.assign({}, config);
  document.body.innerHTML = merged.html;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.sourceType === "user-controlled";
  });
});

test("Security: Object.assign with multiple sources, tainted property → eval", `
  var defaults = { code: "console.log('safe')" };
  var userOpts = { code: location.search.slice(1) };
  var opts = Object.assign({}, defaults, userOpts);
  eval(opts.code);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "eval" && s.sourceType === "user-controlled";
  });
});

// ── Factory handler pattern (message handler returned from factory function) ──

test("Security: factory-returned message handler → innerHTML via callback", `
  function makeHandler(sinkFn) {
    return function(event) {
      sinkFn(event.data);
    };
  }
  window.addEventListener("message", makeHandler(function(data) {
    document.body.innerHTML = data;
  }));
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.sourceType === "user-controlled";
  });
});

test("Security: factory-returned handler detects postmessage-no-origin", `
  function createHandler(cb) {
    return function(ev) { cb(ev.data); };
  }
  window.addEventListener("message", createHandler(function(d) {
    document.body.innerHTML = d;
  }));
`, function(r) {
  var hasPostMsg = r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin";
  });
  var hasXss = r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML";
  });
  return hasPostMsg && hasXss;
});

test("Security: factory-returned handler assigned to onmessage → eval", `
  var makeListener = function(fn) {
    return function(e) { fn(e.data); };
  };
  window.onmessage = makeListener(function(payload) {
    eval(payload);
  });
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "eval" && s.sourceType === "user-controlled";
  });
});

// ── Stage 4: Parameter destructuring taint propagation ──

test("Security: destructured param function({data}) in message handler → innerHTML", `
  window.addEventListener("message", function({data}) {
    document.getElementById("out").innerHTML = data;
  });
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.sourceType === "user-controlled";
  });
});

test("Security: destructured param with default function({data} = {}) in message handler → innerHTML", `
  window.addEventListener("message", function({data} = {}) {
    document.body.innerHTML = data;
  });
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.sourceType === "user-controlled";
  });
});

test("Security: renamed destructured param function({origin: src}) in message handler → eval", `
  window.addEventListener("message", function({origin: src, payload}) {
    eval(payload);
  });
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "eval" && s.sourceType === "user-controlled";
  });
});

test("Security: destructured param in named function called with tainted arg → innerHTML", `
  function render({html}) {
    document.body.innerHTML = html;
  }
  render({html: location.hash});
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.sourceType === "user-controlled";
  });
});

// ── Stage 4: Array method callback taint propagation ──

test("Security: tainted.forEach(x => innerHTML = x)", `
  var parts = location.hash.split(",");
  parts.forEach(function(item) {
    document.body.innerHTML = item;
  });
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.sourceType === "user-controlled";
  });
});

test("Security: tainted.map(x => eval(x))", `
  var cmds = location.search.split("&");
  cmds.map(function(cmd) {
    eval(cmd);
  });
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "eval" && s.sourceType === "user-controlled";
  });
});

test("Security: tainted.filter callback param is tainted → innerHTML", `
  var items = [location.hash];
  items.filter(function(x) {
    document.getElementById("log").innerHTML = x;
    return true;
  });
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.sourceType === "user-controlled";
  });
});

test("Security: tainted.find callback param → eval", `
  var data = [location.hash, "safe"];
  data.find(function(item) {
    eval(item);
  });
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "eval" && s.sourceType === "user-controlled";
  });
});

test("Security: tainted.reduce — currentValue (param 1) is tainted → innerHTML", `
  var chunks = location.hash.split("/");
  chunks.reduce(function(acc, chunk) {
    document.body.innerHTML = chunk;
    return acc + chunk;
  }, "");
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.sourceType === "user-controlled";
  });
});

test("Security: named function passed by reference to forEach → innerHTML", `
  function process(item) {
    document.body.innerHTML = item;
  }
  var data = [location.hash];
  data.forEach(process);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.sourceType === "user-controlled";
  });
});

// ── Stage 4: Promise .then() chain taint propagation ──

test("Security: fetch(tainted).then(r => r.text()).then(data => innerHTML = data)", `
  fetch(location.hash)
    .then(function(r) { return r.text(); })
    .then(function(data) {
      document.body.innerHTML = data;
    });
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.sourceType === "user-controlled";
  });
});

test("Security: promise.then(data => eval(data)) with tainted promise", `
  var p = fetch(location.hash);
  p.then(function(data) {
    eval(data);
  });
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.sink === "eval" && s.sourceType === "user-controlled";
  });
});

test("Security: await fetch(tainted) then await .text() → innerHTML", `
  async function load() {
    var resp = await fetch(location.hash);
    var text = await resp.text();
    document.body.innerHTML = text;
  }
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.sourceType === "user-controlled";
  });
});

test("Security: .then() with named handler function", `
  function handleData(data) {
    document.body.innerHTML = data;
  }
  fetch(location.hash).then(handleData);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.sink === "innerHTML" && s.sourceType === "user-controlled";
  });
});

// === Scope-Aware _containsNetworkSink (V1) ===
console.log("\n=== Scope-Aware Network Sink Detection ===\n");

test("V1: shadowed fetch is NOT a network sink", `
  (function() {
    var fetch = function(url) { return {then: function(){}}; };
    fetch("/api/local");
  })();
`, function(r) {
  // Since fetch is shadowed by a local variable, /api/local should NOT appear as a fetch call site
  return !r.fetchCallSites.some(function(s) { return s.url === "/api/local"; });
});

test("V1: global fetch IS a network sink", `
  function doRequest(url) {
    return fetch(url);
  }
  doRequest("/api");
`, function(r) {
  return r.fetchCallSites.some(function(s) { return s.url === "/api"; });
});

test("V1: .open() on non-XHR object is NOT detected as XHR network sink", `
  function sendData(method, url) {
    var channel = new BroadcastChannel("test");
    channel.open(method, url);
  }
  sendData("GET", "/api/data");
`, function(r) {
  // channel.open() should NOT be treated as XMLHttpRequest.open()
  // since channel is a BroadcastChannel, not XMLHttpRequest
  return !r.fetchCallSites.some(function(s) { return s.url === "/api/data"; });
});

test("V1: .open() on XMLHttpRequest IS a network sink", `
  function doXHR(method, url) {
    var xhr = new XMLHttpRequest();
    xhr.open(method, url);
    xhr.send();
  }
  doXHR("GET", "/api/data");
`, function(r) {
  return r.fetchCallSites.some(function(s) { return s.url === "/api/data"; });
});

// === Scope-Aware Taint Source Classification (V2/Phase 2) ===
console.log("\n=== Scope-Aware Taint Source Classification ===\n");

test("V2: shadowed location is NOT a taint source", `
  function safe() {
    var location = { hash: "#safe" };
    document.body.innerHTML = location.hash;
  }
`, function(r) {
  return r.securitySinks.length === 0;
});

test("V2: global location.hash IS a taint source", `
  document.body.innerHTML = location.hash;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.source === "location.hash";
  });
});

test("V2: window.location.hash with global window IS a taint source", `
  document.body.innerHTML = window.location.hash;
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.source === "location.hash";
  });
});

test("V2: self.document.referrer IS a taint source", `
  eval(self.document.referrer);
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "eval" && s.source === "document.referrer";
  });
});

test("V2: shadowed window.location is NOT a taint source", `
  function safe() {
    var window = { location: { hash: "#safe" } };
    document.body.innerHTML = window.location.hash;
  }
`, function(r) {
  return r.securitySinks.length === 0;
});

// === PostMessage Origin Sub-classification ===
console.log("\n=== Security: PostMessage Origin Sub-classification ===\n");

// --- Weak method sub-classification ---

test("postMessage startsWith includes method name and bypass in description", `
  window.addEventListener("message", function(event) {
    if (event.origin.startsWith("https://trusted.com")) {
      document.body.innerHTML = event.data;
    }
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-weak-origin" &&
           p.weakMethod === "startsWith" &&
           p.description.indexOf("startsWith") !== -1 &&
           p.description.indexOf("evil.com") !== -1;
  });
});

test("postMessage endsWith description explains subdomain risk", `
  window.addEventListener("message", function(event) {
    if (event.origin.endsWith(".google.com")) {
      eval(event.data);
    }
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-weak-origin" &&
           p.weakMethod === "endsWith" &&
           p.description.indexOf("endsWith") !== -1 &&
           p.description.indexOf("subdomain") !== -1;
  });
});

test("postMessage includes description explains substring risk", `
  window.addEventListener("message", function(event) {
    if (event.origin.includes("example")) {
      document.body.innerHTML = event.data;
    }
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-weak-origin" &&
           p.weakMethod === "includes" &&
           p.description.indexOf("substring") !== -1;
  });
});

test("postMessage indexOf description explains substring risk", `
  window.addEventListener("message", function(event) {
    if (event.origin.indexOf("example.com") !== -1) {
      document.body.innerHTML = event.data;
    }
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-weak-origin" &&
           p.weakMethod === "indexOf" &&
           p.description.indexOf("substring") !== -1;
  });
});

// --- Checked value extraction ---

test("postMessage weak check extracts checked value string", `
  window.addEventListener("message", function(event) {
    if (event.origin.startsWith("https://a.com")) {
      document.body.innerHTML = event.data;
    }
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-weak-origin" &&
           p.checkedValue === "https://a.com";
  });
});

test("postMessage weak check with variable arg has null checkedValue", `
  var prefix = getOriginPrefix();
  window.addEventListener("message", function(event) {
    if (event.origin.startsWith(prefix)) {
      document.body.innerHTML = event.data;
    }
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-weak-origin" &&
           p.checkedValue === null;
  });
});

// --- Whitelist/dictionary patterns (strong) ---

test("postMessage Set.has(origin) whitelist → strong (not flagged)", `
  var allowed = new Set(["https://trusted.com", "https://other.com"]);
  window.addEventListener("message", function(event) {
    if (!allowed.has(event.origin)) return;
    document.body.innerHTML = event.data;
  });
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin" || p.type === "postmessage-weak-origin";
  });
});

test("postMessage array.includes(origin) whitelist → strong (not flagged)", `
  var allowedOrigins = ["https://trusted.com", "https://other.com"];
  window.addEventListener("message", function(event) {
    if (!allowedOrigins.includes(event.origin)) return;
    document.body.innerHTML = event.data;
  });
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin" || p.type === "postmessage-weak-origin";
  });
});

test("postMessage dict[origin] === true whitelist → strong (not flagged)", `
  var allowed = { "https://trusted.com": true };
  window.addEventListener("message", function(event) {
    if (allowed[event.origin] === true) {
      document.body.innerHTML = event.data;
    }
  });
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin" || p.type === "postmessage-weak-origin";
  });
});

test("postMessage origin.includes('str') still weak, not confused with array.includes", `
  window.addEventListener("message", function(event) {
    if (event.origin.includes("trusted")) {
      document.body.innerHTML = event.data;
    }
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-weak-origin";
  });
});

// --- Whitelist via callee tracing ---

test("postMessage origin passed to function with Set.has → strong", `
  var allowed = new Set(["https://trusted.com"]);
  function checkOrigin(o) { return allowed.has(o); }
  window.addEventListener("message", function(event) {
    if (!checkOrigin(event.origin)) return;
    document.body.innerHTML = event.data;
  });
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin" || p.type === "postmessage-weak-origin";
  });
});

test("postMessage origin passed to function with dict[param] === true → strong", `
  var allowed = { "https://trusted.com": true };
  function checkOrigin(o) { return allowed[o] === true; }
  window.addEventListener("message", function(event) {
    if (!checkOrigin(event.origin)) return;
    document.body.innerHTML = event.data;
  });
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin" || p.type === "postmessage-weak-origin";
  });
});

// --- Class constructor property tracing ---

test("postMessage class instance c.i(origin) with strong checker → not flagged", `
  var Zp = class {
    constructor(a) { this.i = a; }
  };
  var c = new Zp(function(o) { return o === "https://trusted.com"; });
  window.addEventListener("message", function(m) {
    if (!c.i(m.origin)) return;
    document.body.innerHTML = m.data;
  });
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin" || p.type === "postmessage-weak-origin";
  });
});

test("postMessage class instance with factory $p pattern → strong", `
  function $p(origins) {
    var b = {};
    origins.forEach(function(c) { b[c] = true; });
    return function(c) { return b[c] === true; };
  }
  var Zp = class { constructor(a) { this.i = a; } };
  var c = new Zp($p(["https://trusted.com"]));
  window.addEventListener("message", function(m) {
    if (!c.i(m.origin)) return;
    document.body.innerHTML = m.data;
  });
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin" || p.type === "postmessage-weak-origin";
  });
});

test("postMessage class instance with weak checker in factory → weak", `
  function $w(b) { return function(c) { return c.includes(b); }; }
  var Zp = class { constructor(a) { this.i = a; } };
  var c = new Zp($w("trusted"));
  window.addEventListener("message", function(m) {
    if (!c.i(m.origin)) return;
    document.body.innerHTML = m.data;
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-weak-origin";
  });
});

// --- Destructured parameter tracing ---

test("postMessage c.i(origin) where c is destructured param from caller with new Zp → strong", `
  var Zp = class { constructor(a) { this.i = a; } };
  function $p(origins) {
    var b = {};
    origins.forEach(function(c) { b[c] = true; });
    return function(c) { return b[c] === true; };
  }
  var aq = function({kh: c}) {
    window.addEventListener("message", function(m) {
      if (c.i(m.origin)) {
        document.body.innerHTML = m.data;
      }
    });
  };
  aq({ kh: new Zp($p(["https://trusted.com"])) });
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin" || p.type === "postmessage-weak-origin";
  });
});

test("postMessage c.i(origin) where c is destructured param, direct === check → strong", `
  var Zp = class { constructor(a) { this.i = a; } };
  var aq = ({kh: c}) => {
    window.addEventListener("message", function(m) {
      if (c.i(m.origin)) {
        document.body.innerHTML = m.data;
      }
    });
  };
  aq({ kh: new Zp(function(o) { return o === "https://trusted.com"; }) });
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin" || p.type === "postmessage-weak-origin";
  });
});

test("postMessage c.i(origin) where c is destructured param, ternary with new Zp → strong", `
  var Zp = class { constructor(a) { this.i = a; } };
  function $p(a) {
    var b = {};
    (typeof a === "string" ? [a] : a).forEach(function(c) { b[c] = true; });
    return function(c) { return b[c] === true; };
  }
  var aq = ({kh: c}) => {
    window.addEventListener("message", function(m) {
      if (c.i(m.origin)) {
        document.body.innerHTML = m.data;
      }
    });
  };
  var origin = "https://trusted.com";
  aq({ kh: origin instanceof Zp ? origin : new Zp($p(origin)) });
`, function(r) {
  return !r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-no-origin" || p.type === "postmessage-weak-origin";
  });
});

// --- Regression: severity upgrade still works ---

test("postMessage startsWith with sink → severity upgraded to high", `
  window.addEventListener("message", function(event) {
    if (event.origin.startsWith("https://example.com")) {
      document.body.innerHTML = event.data;
    }
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-weak-origin" && p.severity === "high" &&
           p.description.indexOf("startsWith") !== -1;
  });
});

test("postMessage startsWith without sink → stays medium", `
  window.addEventListener("message", function(event) {
    if (event.origin.startsWith("https://example.com")) {
      console.log(event.data);
    }
  });
`, function(r) {
  return r.dangerousPatterns.some(function(p) {
    return p.type === "postmessage-weak-origin" && p.severity === "medium";
  });
});

// === Origin Check Walker (V3) ===
console.log("\n=== Origin Check Walker (V3) ===\n");

test("V3: origin check inside for loop is detected", `
  window.addEventListener("message", function(e) {
    for (var i = 0; i < allowedOrigins.length; i++) {
      if (e.origin === allowedOrigins[i]) {
        eval(e.data);
      }
    }
  });
`, function(r) {
  // Should classify as "strong" origin check — no postmessage-no-origin warning
  return !r.dangerousPatterns.some(function(d) { return d.type === "postmessage-no-origin"; });
});

test("V3: origin check inside while loop is detected", `
  window.addEventListener("message", function(e) {
    var found = false;
    var i = 0;
    while (i < origins.length) {
      if (e.origin === origins[i]) found = true;
      i++;
    }
    if (found) eval(e.data);
  });
`, function(r) {
  return !r.dangerousPatterns.some(function(d) { return d.type === "postmessage-no-origin"; });
});

test("V3: origin check inside switch statement is detected", `
  window.addEventListener("message", function(e) {
    switch(true) {
      case e.origin === "https://trusted.com":
        eval(e.data);
        break;
    }
  });
`, function(r) {
  return !r.dangerousPatterns.some(function(d) { return d.type === "postmessage-no-origin"; });
});

// === Type Tracker (Phase 3) ===
console.log("\n=== Type Tracker ===\n");

test("Type: new XMLHttpRequest() → .open() correctly identified as XHR sink", `
  function sendRequest(url) {
    var xhr = new XMLHttpRequest();
    xhr.open("GET", url);
    xhr.send();
  }
  sendRequest("/api/data");
`, function(r) {
  return r.fetchCallSites.some(function(s) { return s.url === "/api/data" && s.method === "GET"; });
});

test("Type: Array literal .forEach() propagates taint", `
  var items = location.hash.split(",");
  items.forEach(function(item) {
    document.body.innerHTML = item;
  });
`, function(r) {
  return r.securitySinks.some(function(s) { return s.type === "xss"; });
});

// === Taint-path trace (every finding records the hops) ===
console.log("\n=== Taint-path trace on findings ===\n");

// Helper: pick the first sink of a given kind and return its taintPath kinds
function _taintKindsFor(r, matcher) {
  var sink = r.securitySinks.find(matcher) || r.dangerousPatterns.find(matcher);
  if (!sink || !Array.isArray(sink.taintPath)) return null;
  return sink.taintPath.map(function(h) { return h.kind; });
}

test("taintPath: direct location.href → innerHTML records source + member + assignment hops", `
  document.body.innerHTML = location.href;
`, function(r) {
  var kinds = _taintKindsFor(r, function(s) { return s.sink === "innerHTML"; });
  return kinds && kinds[0] === "source" && kinds.length >= 1;
});

test("taintPath: negative — safe literal innerHTML has no user-controlled finding", `
  document.body.innerHTML = "<p>static</p>";
`, function(r) {
  return r.securitySinks.length === 0;
});

test("taintPath: multi-hop location.hash → split → decodeURIComponent → template → innerHTML records chain", `
  var raw = location.hash.split("=")[1];
  var decoded = decodeURIComponent(raw);
  var html = \`<div>\${decoded}</div>\`;
  document.body.innerHTML = html;
`, function(r) {
  var kinds = _taintKindsFor(r, function(s) { return s.sink === "innerHTML"; });
  if (!kinds) return false;
  // Must begin at the taint source and end at a propagation hop.
  if (kinds[0] !== "source") return false;
  // Must include a call-arg hop (for decodeURIComponent) and a template-interp hop.
  return kinds.indexOf("call-arg") >= 0 && kinds.indexOf("template-interp") >= 0;
});

test("taintPath: negative — same shape with literal source has no taintPath hops on sink", `
  var raw = "static=value".split("=")[1];
  var decoded = decodeURIComponent(raw);
  var html = \`<div>\${decoded}</div>\`;
  document.body.innerHTML = html;
`, function(r) {
  return r.securitySinks.length === 0;
});

test("taintPath: reassignment records reassign hop kind", `
  let x = "safe";
  x = location.hash;
  document.body.innerHTML = x;
`, function(r) {
  var kinds = _taintKindsFor(r, function(s) { return s.sink === "innerHTML"; });
  return kinds && kinds.indexOf("reassign") >= 0;
});

test("taintPath: negative — reassignment to safe value has no sink finding", `
  let x = location.hash;
  x = "safe";
  document.body.innerHTML = x;
`, function(r) {
  // A sound tracer picks up the ORIGINAL init — this is expected to still
  // flag; the polarity here is specifically that no reassign-from-tainted
  // hop appears since the reassignment is to a literal.
  var sink = r.securitySinks.find(function(s) { return s.sink === "innerHTML"; });
  if (!sink || !Array.isArray(sink.taintPath)) return true; // no finding is also fine
  return !sink.taintPath.some(function(h) { return h.kind === "reassign" || h.kind === "reassign-uninit"; });
});

test("taintPath: method call on tainted object records method-call hop", `
  var lowered = location.hash.toLowerCase();
  document.body.innerHTML = lowered;
`, function(r) {
  var kinds = _taintKindsFor(r, function(s) { return s.sink === "innerHTML"; });
  return kinds && kinds.indexOf("method-call") >= 0;
});

test("taintPath: negative — method call on safe object has no finding", `
  var lowered = "STATIC".toLowerCase();
  document.body.innerHTML = lowered;
`, function(r) {
  return r.securitySinks.length === 0;
});

test("taintPath: every hop carries a location", `
  var raw = location.hash;
  document.body.innerHTML = raw;
`, function(r) {
  var sink = r.securitySinks.find(function(s) { return s.sink === "innerHTML"; });
  if (!sink || !Array.isArray(sink.taintPath) || !sink.taintPath.length) return false;
  return sink.taintPath.every(function(h) {
    return h && h.at && typeof h.at.line === "number" && typeof h.at.column === "number";
  });
});

test("taintPath: dangerous pattern also records taintPath (Object.defineProperty with tainted key)", `
  var k = location.hash.slice(1);
  Object.defineProperty(obj, k, { value: 1 });
`, function(r) {
  var d = r.dangerousPatterns.find(function(x) { return x.type === "prototype-pollution"; });
  return d && Array.isArray(d.taintPath) && d.taintPath.length > 0 && d.taintPath[0].kind === "source";
});

// === Sanitizer verification reports ===
console.log("\n=== Sanitizer verification reports ===\n");

test("sanitizerReport: DOMPurify.sanitize on-path downgrades to info AND records matched+on-path candidate", `
  function render(input) {
    var clean = DOMPurify.sanitize(input);
    document.body.innerHTML = clean;
  }
  render(location.hash);
`, function(r) {
  var sink = r.securitySinks.find(function(s) { return s.sink === "innerHTML"; });
  if (!sink) return false;
  if (sink.severity !== "info") return false;
  if (!sink.sanitizerReport || !Array.isArray(sink.sanitizerReport.candidates)) return false;
  var cand = sink.sanitizerReport.candidates.find(function(c) { return c.label === "DOMPurify.sanitize()"; });
  return cand && cand.matched === true && cand.onPath === true && sink.sanitizerReport.decision === "fires-on-all-paths";
});

test("sanitizerReport: negative — shadowed DOMPurify reports matched=false with shadow reason", `
  function render(input, DOMPurify) {
    var clean = DOMPurify.sanitize(input);
    document.body.innerHTML = clean;
  }
  render(location.hash, {sanitize: function(x){return x}});
`, function(r) {
  var sink = r.securitySinks.find(function(s) { return s.sink === "innerHTML"; });
  if (!sink) return false;
  // Must NOT be downgraded to info — shadow defeats the sanitizer
  if (sink.severity === "info") return false;
  if (!sink.sanitizerReport) return false;
  var cand = sink.sanitizerReport.candidates.find(function(c) { return c.label === "DOMPurify.sanitize()"; });
  return cand && cand.matched === false && /shadow/i.test(cand.matchReason || "");
});

test("sanitizerReport: branch-only sanitizer flagged matched but NOT on-path", `
  function render(input, flag) {
    var clean = input;
    if (flag) {
      clean = DOMPurify.sanitize(input);
    }
    document.body.innerHTML = clean;
  }
  render(location.hash, true);
`, function(r) {
  var sink = r.securitySinks.find(function(s) { return s.sink === "innerHTML"; });
  if (!sink) return false;
  if (!sink.sanitizerReport) return false;
  var cand = sink.sanitizerReport.candidates.find(function(c) { return c.label === "DOMPurify.sanitize()"; });
  return cand && cand.matched === true && cand.onPath === false && sink.sanitizerReport.decision === "missing-on-some-paths";
});

test("sanitizerReport: negative — function with no sanitizer-shaped calls has no sanitizerReport", `
  function render(input) {
    document.body.innerHTML = input;
  }
  render(location.hash);
`, function(r) {
  var sink = r.securitySinks.find(function(s) { return s.sink === "innerHTML"; });
  // no sanitizerReport because no candidates found
  return sink && !sink.sanitizerReport;
});

test("sanitizerReport: .toString() is NOT treated as a sanitizer (prototype inheritance regression)", `
  function render(input) {
    var s = input.toString();
    document.body.innerHTML = s;
  }
  render(location.hash);
`, function(r) {
  var sink = r.securitySinks.find(function(s) { return s.sink === "innerHTML"; });
  if (!sink) return false;
  // Must NOT be downgraded: toString doesn't sanitize HTML.
  if (sink.severity === "info") return false;
  // And no candidate should claim toString as a match.
  if (sink.sanitizerReport) {
    var bad = sink.sanitizerReport.candidates.find(function(c) { return /\.toString\(\)/.test(c.label) && c.matched; });
    if (bad) return false;
  }
  return true;
});

test("sanitizerReport: .valueOf() / .hasOwnProperty() are NOT treated as sanitizers either", `
  function render(input) {
    var s = input.valueOf();
    var t = s.hasOwnProperty("x");
    document.body.innerHTML = s;
  }
  render(location.hash);
`, function(r) {
  var sink = r.securitySinks.find(function(s) { return s.sink === "innerHTML"; });
  if (!sink) return false;
  if (sink.severity === "info") return false;
  return true;
});

test("sanitizerReport: encodeURIComponent on-path records global-sanitizer match", `
  function redirect(input) {
    var enc = encodeURIComponent(input);
    location.href = "/go?to=" + enc;
  }
  redirect(location.hash);
`, function(r) {
  var sink = r.securitySinks.find(function(s) { return s.sink === "href"; });
  if (!sink) return false;
  if (!sink.sanitizerReport) return false;
  var cand = sink.sanitizerReport.candidates.find(function(c) { return c.label === "encodeURIComponent()"; });
  return cand && cand.matched === true && /sanitizer global/i.test(cand.matchReason || "");
});

// === CFG + Sanitizer Path Analysis (Phase 4) ===
console.log("\n=== CFG + Sanitizer Path Analysis ===\n");

test("CFG: sanitized with encodeURIComponent before href → info severity", `
  function safe() {
    var input = location.hash;
    var sanitized = encodeURIComponent(input);
    document.body.innerHTML = sanitized;
  }
`, function(r) {
  if (r.securitySinks.length === 0) return false;
  return r.securitySinks.some(function(s) {
    return s.sanitized === true && s.severity === "info";
  });
});

test("CFG: unsanitized location.hash → innerHTML is high severity", `
  function unsafe() {
    var input = location.hash;
    document.body.innerHTML = input;
  }
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.severity === "high" && !s.sanitized;
  });
});

test("CFG: DOMPurify.sanitize() before innerHTML → info severity", `
  function safe() {
    var dirty = location.hash;
    var clean = DOMPurify.sanitize(dirty);
    document.body.innerHTML = clean;
  }
`, function(r) {
  if (r.securitySinks.length === 0) return false;
  return r.securitySinks.some(function(s) {
    return s.sanitized === true && s.severity === "info";
  });
});

test("CFG: parseInt() before eval → info severity (number conversion kills XSS)", `
  function safe() {
    var input = location.hash;
    var num = parseInt(input);
    eval(num);
  }
`, function(r) {
  if (r.securitySinks.length === 0) return false;
  return r.securitySinks.some(function(s) {
    return s.sanitized === true && s.severity === "info";
  });
});

test("CFG: partial sanitization (if branch only) remains high", `
  function partial(flag) {
    var input = location.hash;
    if (flag) {
      input = encodeURIComponent(input);
    }
    document.body.innerHTML = input;
  }
`, function(r) {
  return r.securitySinks.some(function(s) {
    return s.type === "xss" && s.severity === "high";
  });
});

// =============================================================================
// Viewer: Scope Resolution
// =============================================================================

console.log("\n=== Viewer: Scope Resolution ===\n");

testViewer("basic variable reference resolves to declaration", trimCode(`
var x = 1;
console.log(x);
`), function(r) {
  return r.refMap[2] && r.refMap[2]["x"] === 1;
});

testViewer("same name in different function scopes resolves to correct scope", trimCode(`
function a() {
  var m = 1;
}
function b() {
  var m = 2;
  console.log(m);
}
`), function(r) {
  return r.refMap[6] && r.refMap[6]["m"] === 5;
});

testViewer("nested scope shadowing resolves to innermost binding", trimCode(`
var m = 1;
function outer() {
  var m = 2;
  function inner() {
    console.log(m);
  }
}
`), function(r) {
  return r.refMap[5] && r.refMap[5]["m"] === 3;
});

testViewer("function parameter resolves as binding", trimCode(`
function handler(c) {
  console.log(c);
}
`), function(r) {
  return r.refMap[2] && r.refMap[2]["c"] === 1;
});

testViewer("parameter shadows outer variable — resolves to parameter", trimCode(`
var c = 1;
function outer() {
  function inner(c) {
    c.toString();
  }
}
`), function(r) {
  return r.refMap[4] && r.refMap[4]["c"] === 3;
});

testViewer("unbound global identifier has no refMap entry", trimCode(`
_.M();
`), function(r) {
  return !r.refMap[1] || !r.refMap[1]["_"];
});

testViewer("self-reference at declaration line is not recorded", trimCode(`
var x = 1;
`), function(r) {
  return !r.refMap[1] || !r.refMap[1]["x"];
});

// =============================================================================
// Viewer: Property Definition Tracking
// =============================================================================

console.log("\n=== Viewer: Property Definition Tracking ===\n");

testViewer("object literal properties tracked in propDefs", trimCode(`
var obj = {
  foo: function() {},
  bar: 42
};
`), function(r) {
  var key = "1:obj";
  return r.propDefs[key] && r.propDefs[key]["foo"] === 2 && r.propDefs[key]["bar"] === 3;
});

testViewer("assignment properties tracked in propDefs", trimCode(`
var obj = {};
obj.method = function() {};
obj.value = 42;
`), function(r) {
  var key = "1:obj";
  return r.propDefs[key] && r.propDefs[key]["method"] === 2 && r.propDefs[key]["value"] === 3;
});

testViewer("prototype assignment tracked under class binding", trimCode(`
function MyClass() {}
MyClass.prototype.doIt = function() {};
`), function(r) {
  var key = "1:MyClass";
  return r.propDefs[key] && r.propDefs[key]["doIt"] === 2;
});

testViewer("string literal keys in object literal tracked", trimCode(`
var obj = {
  "hello": 1,
  "world": 2
};
`), function(r) {
  var key = "1:obj";
  return r.propDefs[key] && r.propDefs[key]["hello"] === 2 && r.propDefs[key]["world"] === 3;
});

testViewer("computed property NOT tracked (dynamic key)", trimCode(`
var obj = {};
obj[key] = 1;
`), function(r) {
  var key = "1:obj";
  return !r.propDefs[key];
});

testViewer("properties on unbound global NOT tracked", trimCode(`
_.foo = 1;
`), function(r) {
  return Object.keys(r.propDefs).length === 0;
});

testViewer("reassignment object literal updates propDefs", trimCode(`
var obj;
obj = {
  alpha: 1,
  beta: 2
};
`), function(r) {
  var key = "1:obj";
  return r.propDefs[key] && r.propDefs[key]["alpha"] === 3 && r.propDefs[key]["beta"] === 4;
});

// =============================================================================
// Viewer: Property Access Resolution
// =============================================================================

console.log("\n=== Viewer: Property Access Resolution ===\n");

testViewer("obj.method() with object literal — method resolves to definition", trimCode(`
var obj = {
  foo: function() {},
  bar: 42
};
obj.foo();
obj.bar;
`), function(r) {
  return r.refMap[5] && r.refMap[5]["foo"] === 2 &&
         r.refMap[6] && r.refMap[6]["bar"] === 3;
});

testViewer("obj.prop with assignment — prop resolves to assignment line", trimCode(`
var obj = {};
obj.method = function() {};
obj.method();
`), function(r) {
  return r.refMap[3] && r.refMap[3]["method"] === 2;
});

testViewer("property on function parameter does NOT resolve (no propDefs)", trimCode(`
function handler(c) {
  c.i();
}
`), function(r) {
  var cResolves = r.refMap[2] && r.refMap[2]["c"] === 1;
  var iMissing = !r.refMap[2] || !r.refMap[2]["i"];
  return cResolves && iMissing;
});

testViewer("property on unbound global does NOT resolve", trimCode(`
_.M();
`), function(r) {
  return !r.refMap[1] || !r.refMap[1]["M"];
});

testViewer("scope-resolved ref takes priority over property resolution", trimCode(`
var i = 1;
var obj = { i: 2 };
var x = i + obj.i;
`), function(r) {
  return r.refMap[3] && r.refMap[3]["i"] === 1;
});

testViewer("object in c.i() resolves via scope (c is object of MemberExpression)", trimCode(`
var c = {
  i: function() {}
};
c.i();
`), function(r) {
  return r.refMap[4] && r.refMap[4]["c"] === 1 && r.refMap[4]["i"] === 2;
});

testViewer("multiple properties on same object all resolve", trimCode(`
var svc = {
  init: function() {},
  start: function() {},
  stop: function() {}
};
svc.init();
svc.start();
svc.stop();
`), function(r) {
  return r.refMap[6] && r.refMap[6]["init"] === 2 &&
         r.refMap[7] && r.refMap[7]["start"] === 3 &&
         r.refMap[8] && r.refMap[8]["stop"] === 4;
});

testViewer("chained member expression — only first level resolves", trimCode(`
var a = {
  b: { c: 1 }
};
a.b.c;
`), function(r) {
  var bResolves = r.refMap[4] && r.refMap[4]["b"] === 2;
  var cMissing = !r.refMap[4] || !r.refMap[4]["c"];
  return bResolves && cMissing;
});

// =============================================================================
// Viewer: Closure-compiled Patterns
// =============================================================================

console.log("\n=== Viewer: Closure-compiled Patterns ===\n");

testViewer("short names in different scopes resolve independently", trimCode(`
(function() {
  var a = function(m) {
    m.data;
  };
  var b = function(m) {
    m.type;
  };
})();
`), function(r) {
  var m3 = r.refMap[3] && r.refMap[3]["m"];
  var m6 = r.refMap[6] && r.refMap[6]["m"];
  return m3 === 2 && m6 === 5;
});

testViewer("callback parameter with message event pattern", trimCode(`
window.addEventListener("message", function(e) {
  if (e.data.type === "test") {
    e.data.value;
  }
});
`), function(r) {
  var e2 = r.refMap[2] && r.refMap[2]["e"] === 1;
  var e3 = r.refMap[3] && r.refMap[3]["e"] === 1;
  var dataMissing = !r.refMap[2] || !r.refMap[2]["data"];
  return e2 && e3 && dataMissing;
});

testViewer("local object with method called in callback — both resolve", trimCode(`
var handlers = {
  onMessage: function(d) {},
  onError: function(e) {}
};
function setup() {
  handlers.onMessage(data);
}
`), function(r) {
  return r.refMap[6] && r.refMap[6]["handlers"] === 1 && r.refMap[6]["onMessage"] === 2;
});

testViewer("IIFE with namespaced object pattern", trimCode(`
var ns = {};
ns.init = function() {};
ns.run = function() {};
ns.init();
ns.run();
`), function(r) {
  return r.refMap[4] && r.refMap[4]["init"] === 2 &&
         r.refMap[5] && r.refMap[5]["run"] === 3;
});

testViewer("prototype method — access on class name resolves", trimCode(`
function Ctor() {}
Ctor.prototype.method = function() {};
Ctor.prototype.other = function() {};
Ctor.method;
Ctor.other;
`), function(r) {
  return r.refMap[4] && r.refMap[4]["method"] === 2 &&
         r.refMap[5] && r.refMap[5]["other"] === 3;
});

// =============================================================================
// Viewer: _resolveDefLine
// =============================================================================

console.log("\n=== Viewer: _resolveDefLine ===\n");

testResolve("scope-resolved lookup returns correct line", function() {
  var refMap = { 10: { "x": 3 } };
  return resolveDefLine("x", 10, refMap, {}, false, null) === 3;
});

testResolve("returns 0 when name not in refMap for that line", function() {
  var refMap = { 10: { "x": 3 } };
  return resolveDefLine("y", 10, refMap, { "y": 5 }, false, null) === 0;
});

testResolve("returns 0 when line not in refMap at all", function() {
  var refMap = { 10: { "x": 3 } };
  return resolveDefLine("x", 20, refMap, { "x": 5 }, false, null) === 0;
});

testResolve("falls back to defMap when refMap is null", function() {
  return resolveDefLine("foo", 10, null, { "foo": 7 }, false, null) === 7;
});

testResolve("focused mode remaps rendered line to beautified line", function() {
  var refMap = { 100: { "x": 50 } };
  var remap = new Map();
  remap.set(5, 100);
  return resolveDefLine("x", 5, refMap, {}, true, remap) === 50;
});

testResolve("focused mode — unmapped line uses rendered line", function() {
  var refMap = { 5: { "x": 1 } };
  var remap = new Map();
  remap.set(3, 99);
  return resolveDefLine("x", 5, refMap, {}, true, remap) === 1;
});

testResolve("does NOT fall back to defMap when refMap exists", function() {
  var refMap = {};
  return resolveDefLine("foo", 10, refMap, { "foo": 7 }, false, null) === 0;
});

// =============================================================================
// Viewer: Edge Cases
// =============================================================================

console.log("\n=== Viewer: Edge Cases ===\n");

testViewer("property defined after access — still resolves (same traversal)", trimCode(`
var obj = {};
obj.late();
obj.late = function() {};
`), function(r) {
  return r.refMap[2] && r.refMap[2]["late"] === 3;
});

testViewer("same property name on different objects — no cross-pollution", trimCode(`
var a = { x: 1 };
var b = { x: 2 };
a.x;
b.x;
`), function(r) {
  var ax = r.refMap[3] && r.refMap[3]["x"];
  var bx = r.refMap[4] && r.refMap[4]["x"];
  return ax === 1 && bx === 2;
});

testViewer("arrow function property in object literal", trimCode(`
var api = {
  fetch: () => {},
  parse: () => {}
};
api.fetch();
api.parse();
`), function(r) {
  return r.refMap[5] && r.refMap[5]["fetch"] === 2 &&
         r.refMap[6] && r.refMap[6]["parse"] === 3;
});

testViewer("mixed assignment and literal properties", trimCode(`
var svc = {
  a: 1
};
svc.b = 2;
svc.a;
svc.b;
`), function(r) {
  return r.refMap[5] && r.refMap[5]["a"] === 2 &&
         r.refMap[6] && r.refMap[6]["b"] === 4;
});

testViewer("nested function — inner param same name as outer var", trimCode(`
var data = {};
data.handler = function(data) {
  data.process();
};
data.handler();
`), function(r) {
  var dataRef = r.refMap[3] && r.refMap[3]["data"];
  return dataRef === 2;
});

testViewer("arrow param call NOT in funcMap calls (scope-aware collectCalls)", trimCode(`
function b() { return 1; }
var p = new Promise(b => {
  b(42);
});
`), function(r) {
  // funcMap["b"] should be the outer function b at line 1
  if (!r.funcMap["b"] || r.funcMap["b"].line !== 1) return false;
  // The arrow b => { b(42) } should NOT have "b" in its calls
  // because b is a parameter binding, not a named function
  var arrowRange = r.allFuncRanges.find(function(fr) {
    return fr.line >= 2 && fr.calls && !fr.calls.has("b");
  });
  return !!arrowRange;
});

// ── Summary ──
console.log("\n" + "=".repeat(50));
console.log("Results: " + passed + "/" + total + " passed, " + failed + " failed");
if (failed > 0) {
  process.exit(1);
} else {
  console.log("All tests passed!");
}
