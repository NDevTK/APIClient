// Measures what the `debugger` permission's DevTools Protocol gives an MV3 extension, in real Chrome, before
// anything is designed on it. Nothing in extension/ uses it; this probe loads an extension of its own.
//
//   NODE_PATH=<checkout>/node_modules xvfb-run -a -s "-screen 0 1280x1000x24" \
//     node testing/cdp_probe.cjs <out-dir>            # headed: the banner rows are measured
//   NODE_PATH=<checkout>/node_modules node testing/cdp_probe.cjs <out-dir> --headless
//
// Rows (out-dir/cdp_probe.json), each beside its controls:
//   warnings  chrome.management.getPermissionWarningsByManifest for manifests with and without `debugger`.
//   offscreen chrome.debugger.attach to the extension's own offscreen document by targetId, then Fetch at
//             request and response stage on that document's own fetch() to a local echo server.
//   tab       Network capture of a tab's document and subresources by the earliest point the extension
//             attached: before the navigation, at tabs.create, onBeforeNavigate, onCommitted, after load.
//   banner    root-window screenshots (headed only) while attached, across tabs, after detach, and with
//             DevTools opened on the attached tab by an XTest F12; CLICK_XY=x,y clicks the infobar's button.
"use strict";
const path = require("path");
const fs = require("fs");
const os = require("os");
const http = require("http");
const zlib = require("zlib");
const { spawnSync } = require("child_process");
const { launchChrome, chromeForTesting } = require("./harness.js");

const outDir = process.argv[2];
if (!outDir) throw new Error("usage: node testing/cdp_probe.cjs <out-dir> [--headless]");
fs.mkdirSync(outDir, { recursive: true });
const headless = process.argv.includes("--headless") || !process.env.DISPLAY;
const PA = 18741, PB = 18742;
const A = `http://localhost:${PA}`, B = `http://127.0.0.1:${PB}`;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// The server counts hits per path and query, records each request's raw header list, and keeps the exact
// bytes it sent for each path so a captured body can be compared byte for byte.
const hits = {}, seen = [], sent = {};
const UTF8 = "é—漢字🙂";
function serve(origin) {
  return (req, res) => {
    const key = origin + req.url;
    hits[key] = (hits[key] || 0) + 1;
    seen.push({ key, method: req.method, rawHeaders: req.rawHeaders });
    const u = new URL(req.url, origin);
    const send = (status, headers, body) => {
      const buf = Buffer.isBuffer(body) ? body : Buffer.from(body, "utf8");
      sent[key] = buf;
      res.writeHead(status, headers);
      res.end(buf);
    };
    const html = { "content-type": "text/html; charset=utf-8" };
    switch (u.pathname) {
      case "/echo":
        return send(200, { "content-type": "application/json", "set-cookie": ["e1=one; Path=/", "e2=two; Path=/"] },
                    JSON.stringify(req.rawHeaders));
      case "/r1":
        return send(302, { location: "/r2?" + u.searchParams, "set-cookie": ["hop1=a; Path=/", "hop1b=b; Path=/"] },
                    "moved-1");
      case "/r2":
        return send(302, { location: "/final?" + u.searchParams, "set-cookie": ["hop2=c; Path=/"] }, "moved-2");
      case "/final":
        return send(200, { "content-type": "text/plain; charset=utf-8", "set-cookie": ["fin=d; Path=/"] },
                    "final " + UTF8);
      case "/doc": {
        const c = u.searchParams.get("c");
        const frame = u.searchParams.get("frame") === "1" ? `<iframe src="${B}/frame?c=${c}"></iframe>` : "";
        const extra = u.pathname === "/doc" && u.searchParams.get("nostore") === "1"
          ? { "cache-control": "no-store" } : {};
        return send(200, Object.assign({}, html, extra),
          `<!doctype html><title>doc ${c}</title><p>${UTF8} ${c}</p>${frame}` +
          `<script src="/s.js?c=${c}"></script>` +
          `<script>fetch("/bin?c=${c}").then(r=>r.arrayBuffer());` +
          `fetch("/gz?c=${c}").then(r=>r.text());` +
          `var x=new XMLHttpRequest();x.open("GET","/x.json?c=${c}");x.send();</script>`);
      }
      case "/frame":
        return send(200, html, `<!doctype html><p>frame ${UTF8} ${u.searchParams.get("c")}</p>` +
                               `<script>fetch("/fx?c=${u.searchParams.get("c")}")</script>`);
      case "/fx":
        return send(200, { "content-type": "text/plain", "access-control-allow-origin": "*" }, "frame-fetch");
      case "/s.js":
        return send(200, { "content-type": "text/javascript" }, `window.S = "${UTF8}";`);
      case "/bin": {
        const b = Buffer.alloc(256); for (let i = 0; i < 256; i++) b[i] = i;
        return send(200, { "content-type": "application/octet-stream" }, b);
      }
      case "/gz": {
        const plain = Buffer.from("gzip " + UTF8 + " ".repeat(200), "utf8");
        res.writeHead(200, { "content-type": "text/plain; charset=utf-8", "content-encoding": "gzip" });
        sent[key] = plain;   // the decoded body is what a fetch() reader sees
        return res.end(zlib.gzipSync(plain));
      }
      case "/x.json":
        return send(200, { "content-type": "application/json" }, JSON.stringify({ x: UTF8 }));
      default:
        return send(404, { "content-type": "text/plain" }, "nf");
    }
  };
}

// The probe extension: the permissions the brief names, an offscreen document that fetches on request, and a
// service worker that records every chrome.debugger event and detach.
const MANIFEST = {
  manifest_version: 3, name: "CDP Probe", version: "1",
  permissions: ["debugger", "offscreen", "tabs", "webNavigation"],
  host_permissions: ["<all_urls>"],
  background: { service_worker: "sw.js" },
};
const SW = `
self.EV = []; self.DET = []; self.NAV = []; self.MODE = {}; self.PAUSES = []; self.PLAN = null;
chrome.debugger.onEvent.addListener((src, method, params) => {
  EV.push({ src, method, params, t: Date.now() });
  if (method === "Fetch.requestPaused" && PLAN) onPaused(src, params);
  if (method === "Target.attachedToTarget" && self.CHILD) onChild(src, params);
});
chrome.debugger.onDetach.addListener((src, reason) => DET.push({ src, reason, t: Date.now() }));
async function onChild(src, p) {
  const s = { tabId: src.tabId, sessionId: p.sessionId };
  const r = {};
  try { await chrome.debugger.sendCommand(s, "Network.enable", {}); r.net = "ok"; } catch (e) { r.net = String(e); }
  try { await chrome.debugger.sendCommand(s, "Runtime.runIfWaitingForDebugger", {}); r.run = "ok"; } catch (e) { r.run = String(e); }
  CHILD.push({ sessionId: p.sessionId, info: p.targetInfo, r });
}
async function onPaused(src, p) {
  const rec = { url: p.request.url, stage: p.responseStatusCode === undefined ? "request" : "response",
                status: p.responseStatusCode, headers: p.responseHeaders, reqHeaders: p.request.headers,
                requestId: p.requestId, networkId: p.networkId, redirectedRequestId: p.redirectedRequestId };
  try {
    if (rec.stage === "response") {
      try { const b = await chrome.debugger.sendCommand(src, "Fetch.getResponseBody", { requestId: p.requestId });
            rec.body = b; } catch (e) { rec.bodyError = String(e); }
      await chrome.debugger.sendCommand(src, "Fetch.continueRequest", { requestId: p.requestId });
    } else {
      const ov = PLAN(p.request.url);
      const args = { requestId: p.requestId };
      if (ov) {
        const h = Object.assign({}, p.request.headers, ov);
        args.headers = Object.entries(h).map(([name, value]) => ({ name, value }));
      }
      rec.sentHeaders = args.headers || null;
      await chrome.debugger.sendCommand(src, "Fetch.continueRequest", args);
    }
  } catch (e) { rec.error = String(e); }
  PAUSES.push(rec);
}
const navHook = (ev) => async (d) => {
  NAV.push({ ev, tabId: d.tabId, frameId: d.frameId, url: d.url, t: Date.now() });
  if (d.frameId !== 0 || MODE[d.tabId] !== ev) return;
  MODE[d.tabId] = "attaching";
  try { await chrome.debugger.attach({ tabId: d.tabId }, "1.3");
        await chrome.debugger.sendCommand({ tabId: d.tabId }, "Network.enable", {});
        MODE[d.tabId] = "attached@" + ev; } catch (e) { MODE[d.tabId] = "error@" + ev + ":" + e; }
};
chrome.webNavigation.onBeforeNavigate.addListener(navHook("onBeforeNavigate"));
chrome.webNavigation.onCommitted.addListener(navHook("onCommitted"));
chrome.webNavigation.onCompleted.addListener((d) => NAV.push({ ev: "onCompleted", tabId: d.tabId, frameId: d.frameId, url: d.url, t: Date.now() }));
self.offFetch = (m) => chrome.runtime.sendMessage(Object.assign({ to: "off" }, m));
chrome.offscreen.createDocument({ url: "off.html", reasons: ["WORKERS"], justification: "probe" });
`;
const OFF_HTML = `<!doctype html><script src="off.js"></script>`;
const OFF_JS = `
chrome.runtime.onMessage.addListener((m, s, reply) => {
  if (m.to !== "off") return;
  (async () => {
    try {
      const r = await fetch(m.url, { redirect: m.redirect || "follow", cache: "no-store", credentials: "omit" });
      reply({ status: r.status, type: r.type, url: r.url, redirected: r.redirected, headers: [...r.headers],
              body: r.type === "opaqueredirect" ? null : await r.text() });
    } catch (e) { reply({ error: String(e) }); }
  })();
  return true;
});`;

function writeExtension() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "cdpprobe-ext-"));
  fs.writeFileSync(path.join(dir, "manifest.json"), JSON.stringify(MANIFEST, null, 1));
  fs.writeFileSync(path.join(dir, "sw.js"), SW);
  fs.writeFileSync(path.join(dir, "off.html"), OFF_HTML);
  fs.writeFileSync(path.join(dir, "off.js"), OFF_JS);
  return dir;
}

function shot(name) {
  if (headless) return null;
  const file = path.join(outDir, name + ".png");
  const r = spawnSync("import", ["-display", process.env.DISPLAY, "-window", "root", file]);
  return r.status === 0 ? file : "import failed: " + r.stderr;
}
// XTest through ctypes: no X headers or xdotool are installed, libXtst.so.6 is.
function xtest(op) {
  const py = `
import ctypes, sys
x = ctypes.CDLL("libX11.so.6"); t = ctypes.CDLL("libXtst.so.6")
x.XOpenDisplay.restype = ctypes.c_void_p
d = ctypes.c_void_p(x.XOpenDisplay(None))
op = sys.argv[1]
if op.startswith("key:"):
    kc = x.XKeysymToKeycode(d, ctypes.c_ulong(int(op[4:], 16)))
    t.XTestFakeKeyEvent(d, kc, 1, 0); t.XTestFakeKeyEvent(d, kc, 0, 0)
else:
    xx, yy = map(int, op[6:].split(","))
    t.XTestFakeMotionEvent(d, -1, xx, yy, 0); x.XFlush(d)
    if op.startswith("click:"):
        t.XTestFakeButtonEvent(d, 1, 1, 0); t.XTestFakeButtonEvent(d, 1, 0, 0)
x.XFlush(d)
`;
  const r = spawnSync("python3", ["-I", "-c", py, op]);
  return r.status === 0 ? "ok" : String(r.stderr);
}

const bytesOf = (b) => b && (b.base64Encoded ? Buffer.from(b.body, "base64") : Buffer.from(b.body, "utf8"));
const headersAt = (key) => seen.filter((s) => s.key === key).map((s) => s.rawHeaders);

async function main() {
  const out = { chrome: chromeForTesting().version, headless, startedAt: new Date().toISOString() };
  const sA = http.createServer(serve(A)).listen(PA, "127.0.0.1");
  const sB = http.createServer(serve(B)).listen(PB, "127.0.0.1");
  const ext = writeExtension();
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), "cdpprobe-prof-"));
  const root = typeof process.getuid === "function" && process.getuid() === 0;
  const browser = await launchChrome({
    headless, pipe: true, enableExtensions: [ext], userDataDir: profile, defaultViewport: null,
    waitForInitialPage: false,
    // The banner is the subject, so the two defaults that change infobars are left out.
    ignoreDefaultArgs: ["--disable-infobars", "--enable-automation"],
    // Puppeteer's own session attaches to the probe's service worker alone, so every other session in this run
    // is the extension's.
    targetFilter: (t) => t.type() === "service_worker" || t.type() === "browser",
    args: ["--no-first-run", "--no-default-browser-check", "--window-position=0,0", "--window-size=1280,960",
           ...(root ? ["--no-sandbox"] : [])],
  });
  out.browserPid = browser.process() && browser.process().pid;
  console.log("browser pid", out.browserPid);
  try {
    let swT = null;
    for (let i = 0; i < 100 && !swT; i++) {
      swT = browser.targets().find((t) => t.type() === "service_worker" && t.url().endsWith("/sw.js"));
      if (!swT) await sleep(100);
    }
    if (!swT) throw new Error("probe service worker never started");
    const sw = await swT.worker();
    out.extId = new URL(swT.url()).host;
    const ev = (fn, ...a) => sw.evaluate(fn, ...a);
    await sleep(1000);

    // ---- warnings ----
    const prod = JSON.parse(fs.readFileSync(path.join(__dirname, "..", "extension", "manifest.json"), "utf8"));
    const man = (perms, hosts) => JSON.stringify({ manifest_version: 3, name: "w", version: "1",
                                                   permissions: perms, host_permissions: hosts });
    out.warnings = await ev(async (ms) => {
      const r = {};
      for (const [k, m] of Object.entries(ms)) {
        try { r[k] = await chrome.management.getPermissionWarningsByManifest(m); } catch (e) { r[k] = String(e); }
      }
      return r;
    }, {
      product: man(prod.permissions, prod.host_permissions),
      productPlusDebugger: man([...prod.permissions, "debugger"], prod.host_permissions),
      debuggerOnly: man(["debugger"], []),
      none: man([], []),
      inventedPermission: man(["notAPermission123"], []),
    });

    // ---- offscreen ----
    const o = out.offscreen = {};
    o.targets = await ev(() => chrome.debugger.getTargets());
    const offT = o.targets.find((t) => t.url.endsWith("/off.html"));
    o.offTarget = offT || null;
    if (shot("b0-baseline")) o.shotBaseline = path.join(outDir, "b0-baseline.png");
    // The detached control first: the browser's own headers on the offscreen document's fetch.
    o.detached = await ev((u) => offFetch({ url: u }), A + "/echo?case=detached");
    o.detachedServer = headersAt(A + "/echo?case=detached");
    o.attachInvented = await ev(() => chrome.debugger.attach({ targetId: "INVENTED0000" }, "1.3")
                                       .then(() => "attached", (e) => String(e)));
    o.attach = await ev((id) => chrome.debugger.attach({ targetId: id }, "1.3").then(() => "attached", (e) => String(e)),
                        offT && offT.id);
    o.shotAttachedOffscreenOnly = shot("b1-offscreen-attached");
    const offSrc = { targetId: offT && offT.id };
    o.networkEnable = await ev((s) => chrome.debugger.sendCommand(s, "Network.enable", {}).then(() => "ok", (e) => String(e)),
                               offSrc);
    o.fetchEnable = await ev((s, a) => chrome.debugger.sendCommand(s, "Fetch.enable", { patterns: [
      { urlPattern: a + "/echo?case=over*", requestStage: "Request" },
      { urlPattern: a + "/echo?case=noov*", requestStage: "Request" },
      { urlPattern: a + "/echo?case=*", requestStage: "Response" },
      { urlPattern: a + "/r*", requestStage: "Request" }, { urlPattern: a + "/r*", requestStage: "Response" },
      { urlPattern: a + "/final*", requestStage: "Request" }, { urlPattern: a + "/final*", requestStage: "Response" },
      { urlPattern: a + "/invented-path-only*", requestStage: "Request" },
    ] }).then(() => "ok", (e) => String(e)), offSrc, A);
    await ev(() => { self.PLAN = (url) => url.includes("case=over")
      ? { Origin: "https://chosen-origin.example", Referer: "https://chosen-origin.example/page?r=1",
          Cookie: "chosen=1; other=2", "Sec-Fetch-Site": "cross-site" } : null; });
    o.override = await ev((u) => offFetch({ url: u }), A + "/echo?case=over#t=fragment-token");
    o.overrideServer = headersAt(A + "/echo?case=over");
    o.noOverride = await ev((u) => offFetch({ url: u }), A + "/echo?case=noov");
    o.noOverrideServer = headersAt(A + "/echo?case=noov");
    // Invented negative control: no pattern names this path, so no pause may be recorded for it.
    o.unmatched = await ev((u) => offFetch({ url: u }), A + "/echo-unmatched?case=x");
    o.manual = await ev((u) => offFetch({ url: u, redirect: "manual" }), A + "/r1?m=manual");
    o.follow = await ev((u) => offFetch({ url: u }), A + "/r1?m=follow");
    // Network errors: a port Chrome refuses, and a port nothing listens on.
    o.unsafePort = await ev((u) => offFetch({ url: u }), "http://127.0.0.1:6000/x?e=unsafe");
    o.refused = await ev((u) => offFetch({ url: u }), "http://127.0.0.1:18749/x?e=refused");
    await sleep(500);
    o.network = await ev((t) => EV.filter((e) => e.src.targetId === t && e.method.startsWith("Network.") &&
        /ExtraInfo|loadingFailed|requestWillBeSent$|responseReceived$/.test(e.method))
      .map((e) => ({ method: e.method, requestId: e.params.requestId,
                     url: e.params.request ? e.params.request.url : (e.params.response ? e.params.response.url : undefined),
                     urlFragment: e.params.request ? e.params.request.urlFragment : undefined,
                     redirect: e.params.redirectResponse ? e.params.redirectResponse.status : undefined,
                     status: e.params.statusCode, errorText: e.params.errorText, blocked: e.params.blockedCookies,
                     headers: e.params.headers, headersText: e.params.headersText,
                     associatedCookies: e.params.associatedCookies && e.params.associatedCookies.length })), offT && offT.id);
    o.pauses = await ev(() => PAUSES);
    o.hitsRedirect = { manual: [hits[A + "/r1?m=manual"], hits[A + "/r2?m=manual"]],
                       follow: [hits[A + "/r1?m=follow"], hits[A + "/r2?m=follow"], hits[A + "/final?m=follow"]] };
    o.sentBodies = { r1: String(sent[A + "/r1?m=follow"]), final: String(sent[A + "/final?m=follow"]) };
    o.detach = await ev((s) => chrome.debugger.detach(s).then(() => "ok", (e) => String(e)), offSrc);
    o.afterDetach = await ev((u) => offFetch({ url: u }), A + "/echo?case=after");
    o.afterDetachServer = headersAt(A + "/echo?case=after");
    await ev(() => { self.PLAN = null; });

    // ---- tab ----
    const tab = out.tab = {};
    const settle = async (tabId, docUrl) => {
      for (let i = 0; i < 100; i++) {
        const n = await ev((t, u) => NAV.some((x) => x.ev === "onCompleted" && x.tabId === t && x.frameId === 0 &&
                                                   x.url === u), tabId, docUrl);
        if (n) break;
        await sleep(100);
      }
      await sleep(1500);
    };
    const capture = async (tabId, label) => {
      const r = await ev(async (t) => {
        const resp = EV.filter((e) => e.src.tabId === t && e.method === "Network.responseReceived");
        const fin = new Set(EV.filter((e) => e.src.tabId === t && e.method === "Network.loadingFinished")
                               .map((e) => (e.src.sessionId || "") + "|" + e.params.requestId));
        const rows = [];
        for (const e of resp) {
          const s = e.src.sessionId ? { tabId: t, sessionId: e.src.sessionId } : { tabId: t };
          const row = { url: e.params.response.url, type: e.params.type, requestId: e.params.requestId,
                        session: e.src.sessionId || null, status: e.params.response.status,
                        finished: fin.has((e.src.sessionId || "") + "|" + e.params.requestId) };
          try { row.body = await chrome.debugger.sendCommand(s, "Network.getResponseBody", { requestId: e.params.requestId }); }
          catch (err) { row.bodyError = String(err); }
          rows.push(row);
        }
        return rows;
      }, tabId);
      for (const row of r) {
        const key = row.url;
        const want = sent[key];
        row.byteEqual = row.body && want ? bytesOf(row.body).equals(want) : null;
        row.base64Encoded = row.body ? row.body.base64Encoded : null;
        row.serverHits = hits[key];
        delete row.body;
      }
      tab[label].captured = r;
    };
    const hitRow = (c, frame) => {
      const ks = [`${A}/doc?c=${c}${frame ? "&frame=1" : ""}`, `${A}/s.js?c=${c}`, `${A}/bin?c=${c}`,
                  `${A}/gz?c=${c}`, `${A}/x.json?c=${c}`];
      if (frame) ks.push(`${B}/frame?c=${c}`, `${B}/fx?c=${c}`);
      return Object.fromEntries(ks.map((k) => [k, hits[k] || 0]));
    };

    // pre: attach to an about:blank tab before navigating it, with auto-attach for out-of-process frames.
    {
      const c = "pre", url = `${A}/doc?c=${c}&frame=1`;
      tab[c] = {};
      tab[c].setup = await ev(async (u) => {
        self.CHILD = [];
        const t = await chrome.tabs.create({ url: "about:blank" });
        await new Promise((r) => setTimeout(r, 500));
        const s = { tabId: t.id };
        const r = { tabId: t.id };
        r.attach = await chrome.debugger.attach(s, "1.3").then(() => "ok", (e) => String(e));
        r.net = await chrome.debugger.sendCommand(s, "Network.enable", {}).then(() => "ok", (e) => String(e));
        r.autoAttach = await chrome.debugger.sendCommand(s, "Target.setAutoAttach",
          { autoAttach: true, waitForDebuggerOnStart: true, flatten: true }).then(() => "ok", (e) => String(e));
        await chrome.tabs.update(t.id, { url: u });
        return r;
      }, url);
      await settle(tab[c].setup.tabId, url);
      tab[c].shotAttachedTab = shot("b2-tab-attached");
      await capture(tab[c].setup.tabId, c);
      tab[c].children = await ev(() => CHILD);
      // The out-of-process frame's document: its Network events arrive on the parent's session; ask the child
      // session for the body too, and the child's Page for its content.
      tab[c].frameDoc = await ev(async (t) => {
        const doc = EV.find((e) => e.src.tabId === t && e.method === "Network.responseReceived" && e.params.type === "Document" &&
                                   e.params.response.url.includes("/frame?"));
        const r = {};
        for (const k of CHILD) {
          const s = { tabId: t, sessionId: k.sessionId };
          r.childBody = doc ? await chrome.debugger.sendCommand(s, "Network.getResponseBody", { requestId: doc.params.requestId })
            .then((b) => b, (e) => String(e)) : "no event";
          await chrome.debugger.sendCommand(s, "Page.enable", {}).catch(() => {});
          const tree = await chrome.debugger.sendCommand(s, "Page.getFrameTree", {}).then((x) => x, (e) => String(e));
          r.childFrame = tree.frameTree ? tree.frameTree.frame.url : tree;
          r.childPage = tree.frameTree ? await chrome.debugger.sendCommand(s, "Page.getResourceContent",
            { frameId: tree.frameTree.frame.id, url: tree.frameTree.frame.url }).then((b) => b, (e) => String(e)) : null;
        }
        return r;
      }, tab[c].setup.tabId);
      const fk = `${B}/frame?c=${c}`;
      for (const k of ["childBody", "childPage"]) {
        const v = tab[c].frameDoc[k];
        if (v && typeof v === "object") tab[c].frameDoc[k] = { byteEqual: bytesOf({ body: v.content !== undefined ? v.content : v.body,
                                                                                 base64Encoded: v.base64Encoded }).equals(sent[fk]) };
      }
      tab[c].hits = hitRow(c, true);
    }
    // nostore: the same, for a no-store document, without auto-attach.
    {
      const c = "nostore", url = `${A}/doc?c=${c}&nostore=1`;
      tab[c] = {};
      tab[c].setup = await ev(async (u) => {
        const t = await chrome.tabs.create({ url: "about:blank" });
        await new Promise((r) => setTimeout(r, 500));
        const r = { tabId: t.id };
        r.attach = await chrome.debugger.attach({ tabId: t.id }, "1.3").then(() => "ok", (e) => String(e));
        r.net = await chrome.debugger.sendCommand({ tabId: t.id }, "Network.enable", {}).then(() => "ok", (e) => String(e));
        await chrome.tabs.update(t.id, { url: u });
        return r;
      }, url);
      await settle(tab[c].setup.tabId, url);
      await capture(tab[c].setup.tabId, c);
      tab[c].hits = { [url]: hits[url] };
      // A cross-site navigation of the same attached tab: does the session follow it?
      const url2 = `${B}/doc?c=crosssite`;
      await ev((t, u) => chrome.tabs.update(t, { url: u }), tab[c].setup.tabId, url2);
      await settle(tab[c].setup.tabId, url2);
      tab.crosssite = {};
      await capture(tab[c].setup.tabId, "crosssite");
      tab.crosssite.captured = tab.crosssite.captured.filter((r) => r.url.startsWith(B));
      tab.crosssite.detachEvents = await ev((t) => DET.filter((d) => d.src.tabId === t), tab[c].setup.tabId);
    }
    // created / onBeforeNavigate / onCommitted: attach at that point of a tab created with its URL.
    for (const [c, how] of [["created", "created"], ["bnav", "onBeforeNavigate"], ["commit", "onCommitted"]]) {
      const url = `${A}/doc?c=${c}`;
      tab[c] = {};
      tab[c].setup = await ev(async (u, h) => {
        if (h === "created") {
          const t = await chrome.tabs.create({ url: u });
          const r = { tabId: t.id, tabUrlAtCreate: t.url, pendingUrl: t.pendingUrl };
          r.attach = await chrome.debugger.attach({ tabId: t.id }, "1.3").then(() => "ok", (e) => String(e));
          r.net = await chrome.debugger.sendCommand({ tabId: t.id }, "Network.enable", {}).then(() => "ok", (e) => String(e));
          return r;
        }
        const t = await chrome.tabs.create({ url: "about:blank" });
        await new Promise((r) => setTimeout(r, 500));
        MODE[t.id] = h;
        await chrome.tabs.update(t.id, { url: u });
        return { tabId: t.id };
      }, url, how);
      await settle(tab[c].setup.tabId, url);
      tab[c].mode = await ev((t) => MODE[t] || null, tab[c].setup.tabId);
      await capture(tab[c].setup.tabId, c);
      tab[c].hits = hitRow(c, false);
    }
    // after: attach after the load completed; ask Network by the frame's loaderId, and Page for its content.
    for (const [c, url] of [["after", `${A}/doc?c=after`], ["afternostore", `${A}/doc?c=afternostore&nostore=1`],
                            ["afterframe", `${A}/doc?c=afterframe&frame=1`]]) {
      tab[c] = {};
      const tabId = await ev(async (u) => (await chrome.tabs.create({ url: u })).id, url);
      await settle(tabId, url);
      tab[c].result = await ev(async (t, u, a) => {
        const s = { tabId: t };
        const r = {};
        r.attach = await chrome.debugger.attach(s, "1.3").then(() => "ok", (e) => String(e));
        await chrome.debugger.sendCommand(s, "Network.enable", {});
        const tree = await chrome.debugger.sendCommand(s, "Page.getFrameTree", {});
        const f = tree.frameTree.frame;
        r.frame = { id: f.id, loaderId: f.loaderId, url: f.url };
        r.networkByLoaderId = await chrome.debugger.sendCommand(s, "Network.getResponseBody", { requestId: f.loaderId })
          .then((b) => b, (e) => String(e));
        r.networkInvented = await chrome.debugger.sendCommand(s, "Network.getResponseBody", { requestId: "INVENTED.1" })
          .then((b) => b, (e) => String(e));
        await chrome.debugger.sendCommand(s, "Page.enable", {});
        r.pageDoc = await chrome.debugger.sendCommand(s, "Page.getResourceContent", { frameId: f.id, url: u })
          .then((b) => b, (e) => String(e));
        const rt = await chrome.debugger.sendCommand(s, "Page.getResourceTree", {});
        r.resources = rt.frameTree.resources.map((x) => x.url);
        r.pageSub = {};
        for (const x of rt.frameTree.resources)
          r.pageSub[x.url] = await chrome.debugger.sendCommand(s, "Page.getResourceContent", { frameId: f.id, url: x.url })
            .then((b) => b, (e) => String(e));
        r.pageInvented = await chrome.debugger.sendCommand(s, "Page.getResourceContent", { frameId: f.id, url: a + "/never-loaded" })
          .then((b) => b, (e) => String(e));
        return r;
      }, tabId, url, A);
      const R = tab[c].result;
      const cmp = (b, k) => (typeof b === "object" && b && sent[k]) ? bytesOf({ body: b.content !== undefined ? b.content : b.body,
                                                                              base64Encoded: b.base64Encoded }).equals(sent[k]) : null;
      R.pageDocByteEqual = typeof R.pageDoc === "string" ? R.pageDoc : cmp(R.pageDoc, url);
      R.pageSubByteEqual = Object.fromEntries(Object.entries(R.pageSub).map(([k, v]) => [k, typeof v === "string" ? v : cmp(v, k)]));
      delete R.pageDoc; R.pageSub = undefined;
      tab[c].hits = Object.assign(hitRow(c, c === "afterframe"), { [url]: hits[url] });
      tab[c].tabId = tabId;
      if (c === "afterframe") {
        // The out-of-process frame, attached by its own targetId after the load.
        const fk = `${B}/frame?c=${c}`;
        const fr = await ev(async (u) => {
          const t = (await chrome.debugger.getTargets()).find((x) => x.url === u);
          if (!t) return { error: "no target for " + u };
          const s = { targetId: t.id };
          const r = { type: t.type };
          r.attach = await chrome.debugger.attach(s, "1.3").then(() => "ok", (e) => String(e));
          await chrome.debugger.sendCommand(s, "Page.enable", {}).catch(() => {});
          const tree = await chrome.debugger.sendCommand(s, "Page.getFrameTree", {}).then((x) => x, (e) => String(e));
          r.page = tree.frameTree ? await chrome.debugger.sendCommand(s, "Page.getResourceContent",
            { frameId: tree.frameTree.frame.id, url: u }).then((b) => b, (e) => String(e)) : tree;
          await chrome.debugger.detach(s).catch(() => {});
          return r;
        }, fk);
        if (fr.page && typeof fr.page === "object") fr.page = { byteEqual: bytesOf({ body: fr.page.content, base64Encoded: fr.page.base64Encoded }).equals(sent[fk]) };
        tab[c].frame = fr;
        tab[c].hits[fk] = hits[fk];
      }
    }

    // ---- banner (headed only) ----
    if (!headless) {
      const b = out.banner = {};
      const sh = async (name, ms) => { await sleep(ms); return shot(name); };
      const attachTab = (t) => ev((x) => chrome.debugger.attach({ tabId: x }, "1.3").then(() => "ok", (e) => String(e)), t);
      const detachTab = (t) => ev((x) => chrome.debugger.detach({ tabId: x }).then(() => "ok", (e) => String(e)), t);
      const activate = (t) => ev((x) => chrome.tabs.update(x, { active: true }).then(() => "ok"), t);
      const create = (u) => ev(async (x) => (await chrome.tabs.create({ url: x, active: true })).id, u);
      const cmd = (t) => ev((x) => chrome.debugger.sendCommand({ tabId: x }, "Runtime.evaluate", { expression: "1+1", returnByValue: true })
                                      .then((r) => r.result.value, (e) => String(e)), t);
      // A clean slate: every tab session of the rows above is detached, and the 5 s auto-close has run.
      b.detachRows = await ev(async () => {
        const ts = (await chrome.debugger.getTargets()).filter((t) => t.attached && t.tabId !== undefined);
        return Promise.all(ts.map((t) => chrome.debugger.detach({ tabId: t.tabId }).then(() => "ok", (e) => String(e))));
      });
      const t0 = await create(`${A}/echo?case=t0`);
      b.c0 = await sh("c0-none-attached", 7000);
      const t1 = await create(`${A}/echo?case=t1`);
      b.attachT1 = await attachTab(t1);
      b.c1 = await sh("c1-t1-attached-0.5s", 500);
      b.c2 = await sh("c2-t1-attached-3s", 2500);
      const t2 = await create(`${A}/echo?case=t2`);
      b.c3 = await sh("c3-t2-created-after-attach", 1500);
      await activate(t0);
      b.c4 = await sh("c4-t0-created-before-attach", 1500);
      await activate(t1);
      b.c5 = await sh("c5-t1-again", 1500);
      b.detachT1 = await detachTab(t1);
      b.c6 = await sh("c6-detached-1s", 1000);
      b.c7 = await sh("c7-detached-7s", 6000);
      // The extension's own offscreen document alone.
      b.attachOff = await ev((id) => chrome.debugger.attach({ targetId: id }, "1.3").then(() => "ok", (e) => String(e)), out.offscreen.offTarget.id);
      b.c8 = await sh("c8-offscreen-only-t1", 2000);
      await activate(t2);
      b.c9 = await sh("c9-offscreen-only-t2", 1500);
      b.detachOff = await ev((id) => chrome.debugger.detach({ targetId: id }).then(() => "ok", (e) => String(e)), out.offscreen.offTarget.id);
      await sleep(7000);
      // DevTools opened first (F12 on t2), then the extension attaches.
      b.f12t2 = [xtest("move:640,600"), xtest("key:ffc9")];
      await sleep(3000);
      b.attachT2WithDevtoolsOpen = await attachTab(t2);
      b.cmdT2 = await cmd(t2);
      b.c10 = await sh("c10-t2-devtools-then-attach", 1500);
      // The extension attaches first (t1), then DevTools opens on it.
      await activate(t1);
      b.attachT1b = await attachTab(t1);
      await sleep(1000);
      b.f12t1 = [xtest("move:640,600"), xtest("key:ffc9")];
      await sleep(3000);
      b.cmdT1AfterDevtools = await cmd(t1);
      b.detachEventsBeforeCancel = await ev(() => DET.slice());
      b.c11 = await sh("c11-t1-attach-then-devtools", 500);
      // The infobar's Cancel button, at the place the c11 shot shows it (CLICK_XY overrides).
      b.clickAt = process.env.CLICK_XY || "795,115";
      b.click = xtest("click:" + b.clickAt);
      await sleep(1500);
      b.c12 = shot("c12-after-cancel");
      b.detachEventsAfterCancel = await ev(() => DET.slice());
      b.cmdT1AfterCancel = await cmd(t1);
      b.cmdT2AfterCancel = await cmd(t2);
      b.attachedAfterCancel = await ev(() => chrome.debugger.getTargets().then((ts) => ts.filter((t) => t.attached).map((t) => t.url)));
    }
    out.detachEvents = await ev(() => DET);
    out.nav = await ev(() => NAV.filter((n) => n.frameId === 0));
  } finally {
    out.finishedAt = new Date().toISOString();
    fs.writeFileSync(path.join(outDir, "cdp_probe.json"), JSON.stringify(out, null, 1));
    await browser.close();
    sA.close(); sB.close();
    fs.rmSync(profile, { recursive: true, force: true });
    fs.rmSync(ext, { recursive: true, force: true });
  }
  console.log("wrote", path.join(outDir, "cdp_probe.json"));
}
main().catch((e) => { console.error(e); process.exit(1); });
