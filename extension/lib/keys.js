// lib/keys.js — API key / token extraction. Scans URLs, headers, and response bodies (recursively through
// nested base64) for known key/token shapes and records them per service. Extracted from the offscreen-brain.js
// monolith (one problem per file); loaded before it, functions stay global and resolve _docForLearning/
// extractInterfaceName at call-time. The 'Smart Key Extraction' feature, just relocated.

// ─── Key Extraction ──────────────────────────────────────────────────────────

const KEY_PATTERNS = [
  { name: "Google API Key", re: /AIzaSy[\w-]{33}/g },
  { name: "Firebase Key", re: /AIza[0-9A-Za-z-_]{35}/g },
  { name: "Bearer Token", re: /bearer\s+([a-zA-Z0-9-._~+/]+=*)/gi },
  {
    name: "Generic API Key",
    re: /(?:api[-_]?key|access[-_]?token|auth[-_]?token)['"]?\s*[:=]\s*['"]?([a-zA-Z0-9\-_]{16,})['"]?/gi,
  },
  { name: "JWT", re: /ey[a-zA-Z0-9-_]+\.ey[a-zA-Z0-9-_]+\.[a-zA-Z0-9-_]+/g },
  { name: "Mapbox Token", re: /pk\.[a-zA-Z0-9.]+/g },
  { name: "GitHub Token", re: /ghp_[a-zA-Z0-9]{36}/g },
  { name: "Stripe Key", re: /[sk|pk]_(?:test|live)_[0-9a-zA-Z]{24}/g },
];

function extractKeysFromText(documentId, text, sourceUrl, sourceContext) {
  if (!text) return;
  const tab = _docForLearning(documentId);
  const url = sourceUrl ? new URL(sourceUrl) : null;
  const service = url ? extractInterfaceName(url) : "unknown";

  // Iterative BFS over nested base64 payloads. `visited` dedupes both the
  // input text and every decoded printable string, so a cycle (same bytes
  // reappearing at a deeper level) terminates without dropping new data.
  const visited = new Set();
  const queue = [{ text, context: sourceContext || "network" }];
  const B64_RE = /[a-zA-Z0-9+/]{20,2000}=*/g;

  while (queue.length > 0) {
    const { text: currentText, context } = queue.shift();
    if (visited.has(currentText)) continue;
    visited.add(currentText);

    // 1. Scan for direct key matches
    for (const pattern of KEY_PATTERNS) {
      pattern.re.lastIndex = 0;
      let m;
      while ((m = pattern.re.exec(currentText)) !== null) {
        const key = m[1] || m[0];
        if (key.length < 10) continue;

        if (!tab.apiKeys.has(key)) {
          tab.apiKeys.set(key, {
            name: pattern.name,
            origin: url ? url.origin : null,
            referer: url ? url.href : null,
            source: context,
            firstSeen: Date.now(),
            lastSeen: Date.now(),
            services: new Set(),
            hosts: new Set(),
            endpoints: new Set(),
            pageUrls: new Set(),
            requestCount: 0,
          });
        }

        const keyData = tab.apiKeys.get(key);
        keyData.lastSeen = Date.now();
        /* THE COUNT WAS INITIALISED AND NEVER INCREMENTED. Nothing in the extension raised it: this file set it
           to 0, merge.js maxed two zeroes, three DCHECKs asserted its type, and the popup's "N req" badge fell
           back to the size of the ENDPOINT set — so it displayed a real number about the wrong thing. This is
           the one place a key is observed in a request, so it is the one place that can count. */
        keyData.requestCount++;
        if (url) {
          keyData.services.add(service);
          keyData.hosts.add(url.hostname);
          keyData.endpoints.add(`${url.hostname}${url.pathname}`);
        }
        if (tab && tab.url) keyData.pageUrls.add(tab.url);
        if (!keyData.pageUrls) keyData.pageUrls = new Set();
      }
    }

    // 2. Scan for base64 blobs that might contain hidden keys.
    // Cap at 2000 chars to avoid decoding huge binary blobs (images, protobuf
    // payloads). Limit to first 50 matches per text to bound CPU time.
    B64_RE.lastIndex = 0;
    let b64m;
    let b64Count = 0;
    while ((b64m = B64_RE.exec(currentText)) !== null && b64Count < 50) {
      b64Count++;
      const candidate = b64m[0];
      try {
        if (tab.apiKeys.has(candidate)) continue;

        const padded =
          candidate.length % 4 === 0
            ? candidate
            : candidate + "=".repeat(4 - (candidate.length % 4));
        const decoded = atob(padded);

        // Filter out non-printable garbage to avoid regex hangs
        const printable = decoded.replace(/[^\x20-\x7E\t\n\r]/g, "");
        if (printable.length > 10 && !visited.has(printable)) {
          queue.push({ text: printable, context: context + " > b64" });
        }
      } catch (e) {
        // Not valid base64, ignore
      }
    }
  }
}

// Per-DOCUMENT analysis state, keyed by documentId — the stable per-document
// identity from MessageSender. tabId/frameId are FIELDS (UI filter + Chrome
// routing), NEVER the key. `origin` is the MessageSender principal (stamped at
// CONTENT_HTML via _originForDoc) — NEVER derived from `url`; it stays "" (fail
// closed) until the document reports. `url` is the document's OWN url (display +
// relative TARGET resolution only).
