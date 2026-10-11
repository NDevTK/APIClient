/* dev.js — the extension's diagnostics page: readings of the engine host that a developer reads and a user
   of the popup does not. It asks the offscreen document through the same messages the popup uses (an
   extension document is the sender lib/popup-handlers.js admits) and renders only through lib/dom-builder.js. */

async function devLoad() {
  /* bridge.js declares `_level1` at load and lib/popup-handlers.js answers it verbatim: a record or the null
     that says no round has completed. Anything else is the command not reaching the offscreen document. */
  const order = await chrome.runtime.sendMessage({ type: "GET_HOST_ORDER" });
  DCHECK(order === null || (!!order && typeof order === "object" && !Array.isArray(order)),
         "GET_HOST_ORDER did not answer with the host's Level-1 reading or with the null that says it has " +
         "taken no round — the offscreen answers it straight off bridge.js's `_level1`");
  domReplaceChildren(document.getElementById("dev-host-order"), [devHostOrderSection(order)]);
  /* The offscreen answers GET_ENGINE_RUNS unconditionally off the array bridge.js declares at load, so
     anything but an array is the command not reaching that document. */
  const runs = await chrome.runtime.sendMessage({ type: "GET_ENGINE_RUNS" });
  DCHECK(Array.isArray(runs),
         "GET_ENGINE_RUNS did not answer with an array — the offscreen slices it straight off bridge.js's " +
         "_engineLog, so anything else is that command not reaching the offscreen document at all");
  domReplaceChildren(document.getElementById("dev-engine-runs"), [devEngineRunsSection(runs)]);
}

document.getElementById("dev-refresh").addEventListener("click", devLoad);
devLoad();
