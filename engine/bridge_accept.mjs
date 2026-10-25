/* The trusted zone's acceptance of an engine's result document, asked in a process: extension/bridge.js's own
 * resultDocumentRefusal, loaded verbatim with the zone's scripts (engine/zone.mjs), so a document a native run
 * composes is judged by the function that judges the shipped engine's.
 *
 *   node engine/bridge_accept.mjs <run-log>   prints the refusal of the log's last @RESULT, or `accepted`;
 *                                            exits 1 on a refusal and 2 when the log carries no document. */

import { readFileSync } from "node:fs";
import { createContext } from "node:vm";
import { fileURLToPath } from "node:url";
import { loadZone, processSiteRpc } from "./zone.mjs";

let zone = null;

/* The zone, loaded once per process. `_globalStoreReady` never settles, so bridge.js's start-up kick never drives
   a pool here. */
function bridgeZone() {
  if (zone) return zone;
  const sandbox = { console, fetch, URL, TextDecoder, TextEncoder, setTimeout, clearTimeout, MessageChannel,
                    AbortController, AbortSignal, atob, swRpc: processSiteRpc,
                    _globalStoreReady: new Promise(() => {}) };
  sandbox.self = sandbox;
  sandbox.globalThis = sandbox;
  sandbox.module = { exports: {} };
  createContext(sandbox);
  loadZone(sandbox, ["bridge.js"]);
  if (typeof sandbox.resultDocumentRefusal !== "function")
    throw new Error("extension/bridge.js declares no resultDocumentRefusal, the acceptance this module asks");
  return (zone = sandbox);
}

/* The zone's refusal of `doc` (a parsed @RESULT document), or null when it accepts it. `complete` is the outcome
   the zone checks a finished run's document under. */
export function bridgeRefusal(doc, complete) {
  return bridgeZone().resultDocumentRefusal(doc, complete);
}

/* The last @RESULT document a run log carries, parsed, or null when it carries none. */
export function lastResultDocument(log) {
  let last = null;
  for (const m of log.matchAll(/^@RESULT (\{.*\})$/gm)) last = m[1];
  return last === null ? null : JSON.parse(last);
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  if (process.argv.length !== 3) throw new Error("usage: node engine/bridge_accept.mjs <run-log>");
  const doc = lastResultDocument(readFileSync(process.argv[2], "utf8"));
  if (doc === null) { console.error("no @RESULT document in the log"); process.exit(2); }
  const why = bridgeRefusal(doc, false);
  console.log(why === null ? "accepted" : "refused: " + why);
  process.exit(why === null ? 0 : 1);
}
