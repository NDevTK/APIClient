// lib/owned-navigables.js — THE OWNERSHIP RECORD FOR EVERY REAL TAB THIS EXTENSION OPENS, AND THE ONE
// CONTROL POINT THAT DECIDES WHETHER IT MAY BE OPENED AT ALL.
//
// Two obligations from CLAUDE.md §"A REAL NAVIGABLE IS A LEGITIMATE INSTRUMENT" and §"A REAL ARTIFACT
// OUTLIVES THE ENGINE", and they are one component because they are one decision:
//
//   1. EVERY TAB SENDS COOKIES. There is no credential-free context — no partition, no incognito, no
//      interception. A session-less tab is a third thing that models nothing: not the person's browser and
//      not a clean client, served a bundle the person will never be served, producing findings that do not
//      reproduce for them. So the fidelity IS the safety property, and it puts the WHOLE of the safety in
//      the CHOICE OF ADDRESS, decided BEFORE the navigation, with nothing behind it. A top-level navigation
//      is a GET, which RFC 9110 §9.2.1 Safe Methods' safe set contains, so the METHOD half is answered and
//      the entire remaining question is PROVENANCE. `openOwnedNavigable` is where that question is asked,
//      and an address whose provenance is not established CRASHES there rather than proceeding — there is
//      no second line to catch it.
//
//   2. A REAL ARTIFACT OUTLIVES THE ENGINE. A modelled navigable dies with the WASM instance holding it, so
//      an engine crash reclaims it; a tab dies with nothing. An engine crash, an extension reload or a
//      browser restart leaves real tabs owned by NOTHING — and with every tab carrying cookies, an orphaned
//      tab is the person's own logged-in session left open by something that is no longer running. So
//      ownership is DURABLE, lives in the trusted zone (the engine holds no policy by construction, and a
//      record that lives in the engine dies with the thing it was supposed to outlive), and is RECONCILED at
//      startup so a restart closes what no flow claims.
//
// A TAB IS A VISIT, NOT AN EXECUTION SURFACE. Forced execution happens in the WASM sandbox; a tab exists to
// obtain a document and to host §LIVE-VERIFY. Nothing here injects, drives, or scripts one — the only browser
// verbs this file reaches for are create, enumerate and close, and it does not hold even those (see below).
//
// THIS FILE IS LOADED IN TWO REALMS AND THAT IS DELIBERATE — the offscreen document (<script> in
// ast-worker.html) and the service worker (importScripts in background.js). They write DISJOINT stores of one
// database and the schema is described ONCE, for the same reason ast-worker.html gives for loading mojom.js
// into both ends of the renderer boundary: a store exists only if both ends agree on it, and ONE description
// is what makes that true. Which realm may write which store is stated at each store and asserted at each
// entry — a second writer is how a record and its epoch start disagreeing.
//
// IT HOLDS NO BROWSER EDGE. The two realms cannot reach the same chrome.* surface ("The runtime API is the
// only extensions API supported by offscreen documents" — chrome.offscreen §Concepts and usage), so the
// entries that need one TAKE it as an argument. That is also what makes this component isolation-testable
// against one fixture: hand it a `listTabs` returning a literal array and a `closeTab` recording its calls,
// and every invariant below either holds or crashes at its origin.
//
// WHAT IS NOT HERE: the engine→trusted-zone request that asks for an address, and the person-facing view that
// lists what was opened and stops it. `listOwnedNavigables` is the read that view is built on, and the record
// carries what it needs (the address, who claimed it, how the address was arrived at, when, and whether its
// handle is still meaningful) because a record that cannot answer those is the wrong record, not a smaller one.

// ─── The store ───────────────────────────────────────────────────────────────
// A SEPARATE DATABASE FROM `uasr_store`, AND THE SEPARATION IS LOAD-BEARING. `uasr_store` holds LEARNED data
// and lib/persistence.js's `clearGlobalStore` empties it whole — that is the Clear button's contract. An
// ownership record is not learned data: emptying it would not close a single tab, it would only make the
// extension forget which of the person's logged-in sessions it left open, which is the wreckage this file
// exists to prevent written down as a feature. Clear must be able to wipe learning without touching ownership.
const OWNED_DB_NAME = "uasr_owned";
const OWNED_DB_VERSION = 1;
// Written ONLY by the trusted offscreen brain. Key = navId.
const OWNED_STORE_RECORDS = "records";
// Written ONLY by the service worker. One key, `bootEpoch`. See markBrowserSessionStart.
const OWNED_STORE_SESSION = "session";
const OWNED_KEY_BOOT_EPOCH = "bootEpoch";

function _ownedOpen() {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(OWNED_DB_NAME, OWNED_DB_VERSION);
    req.onupgradeneeded = (e) => {
      const db = e.target.result;
      if (!db.objectStoreNames.contains(OWNED_STORE_RECORDS)) db.createObjectStore(OWNED_STORE_RECORDS);
      if (!db.objectStoreNames.contains(OWNED_STORE_SESSION)) db.createObjectStore(OWNED_STORE_SESSION);
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

// Run `body(store)` in one transaction and resolve once the transaction COMMITS, with `body`'s return value —
// or, when it returns `{__req}`, with that request's result. Resolving on a request's own `onsuccess` instead
// would report a write as durable while the transaction could still abort, and durability is the entire claim
// this record makes: a record that is not on disk before the tab exists is a tab nothing can be told about.
function _ownedTx(storeName, mode, body) {
  return _ownedOpen().then(
    (db) =>
      new Promise((resolve, reject) => {
        const tx = db.transaction(storeName, mode);
        let out;
        try {
          out = body(tx.objectStore(storeName));
        } catch (e) {
          db.close();
          reject(e);
          return;
        }
        tx.oncomplete = () => { db.close(); resolve(out && out.__req ? out.__req.result : out); };
        tx.onerror = () => { db.close(); reject(tx.error); };
        tx.onabort = () => { db.close(); reject(tx.error || new Error("owned-navigables transaction aborted")); };
      }),
  );
}

// ─── The browser-session epoch — the ONLY thing that makes a stored tabId meaningful ─────────────────────
//
// `chrome.tabs.Tab.id` is documented as "Tab IDs are unique within a browser session" (chrome.tabs §Tab), and
// that sentence is the whole problem: a stored tabId is an EXACT handle for as long as the browser session it
// was minted in lasts, and a NUMBER NAMING SOMEBODY ELSE'S TAB afterwards. Chrome hands out fresh ids at each
// launch and restores tabs under new ones, so a record that survives a restart holds a handle that is not
// merely stale but plausibly VALID — a plausible datum, whose consequence here is closing a tab of the
// person's that this extension never opened.
//
// So a record carries the epoch its tabId was minted in, and the epoch is a BROWSER FACT rather than our
// guess: `chrome.runtime.onStartup` is documented as "Fired when a profile that has this extension installed
// first starts up" (chrome.runtime §onStartup), which is exactly the boundary at which tab ids are reassigned.
//
// THE SERVICE WORKER IS THE SOLE WRITER, AND IT WRITES BEFORE IT CREATES THE OFFSCREEN DOCUMENT. That
// ordering is what makes this robust against the failure it would otherwise have: if the boundary travelled
// as a MESSAGE, a message lost to an eviction or to a create race would leave the brain reconciling this
// session's tabs against the previous session's epoch — closing the person's tabs. A durable write that
// happens before the reader can exist has no such window. The brain never writes this key: two writers of one
// epoch is two answers to "which session is this", and the whole record hangs off there being one.
//
// A minted-here fallback is deliberately ABSENT. An epoch this file invented would be indistinguishable from
// one the browser's own event produced, and every record keyed to it would claim a handle validity nothing
// established. Absent reads as UNKNOWN, and unknown orphans every record and refuses to mint a new one — the
// fail-closed direction, whose cost is that a tab we could have closed is instead reported to the person.
async function markBrowserSessionStart() {
  const epoch = crypto.randomUUID();
  await _ownedTx(OWNED_STORE_SESSION, "readwrite", (s) => { s.put(epoch, OWNED_KEY_BOOT_EPOCH); return epoch; });
  return epoch;
}

// Write an epoch only if the store holds none, in ONE transaction so the get and the put cannot be separated
// by another realm's write. Called from `chrome.runtime.onInstalled`, which is NOT a session boundary — an
// extension update leaves every tab id valid — so bumping the epoch there would orphan records this session
// can still act on. What onInstalled is for is the case where no epoch exists at all: a first install, whose
// `onStartup` does not fire until the next browser launch.
async function ensureBrowserSessionEpoch() {
  return _ownedTx(OWNED_STORE_SESSION, "readwrite", (s) => {
    const req = s.get(OWNED_KEY_BOOT_EPOCH);
    req.onsuccess = () => { if (req.result === undefined) s.put(crypto.randomUUID(), OWNED_KEY_BOOT_EPOCH); };
  });
}

// The current browser session's epoch, or null when no realm has written one yet. NULL IS A POSITIVE
// STATEMENT — "this realm cannot say which browser session it is in" — and every consumer below treats it as
// one rather than filling it in.
async function readBrowserSessionEpoch() {
  const v = await _ownedTx(OWNED_STORE_SESSION, "readonly", (s) => ({ __req: s.get(OWNED_KEY_BOOT_EPOCH) }));
  if (v === undefined) return null;
  DCHECK(typeof v === "string" && v.length > 0,
         "the boot epoch is written only by markBrowserSessionStart/ensureBrowserSessionEpoch (a " +
         "crypto.randomUUID), so a stored value that is not a non-empty string means a second writer reached " +
         "this store — and the record's whole claim that a tabId is a valid handle rests on there being " +
         "exactly one answer to which session it is");
  return v;
}

// ─── Address provenance — the whole of the safety, asked before the navigation ────────────────────────────
//
// CLAUDE.md §provenance's three grades, applied to an ADDRESS rather than to a request's values, which is
// what a top-level navigation is:
//
//   OBSERVED  the page made this navigation, or would have made it on the path real values take.
//   DERIVED   the app's own code computed the address from real inputs — a route only its router knows, an
//             admin view no link reaches. A person could have reached it. Navigating it IS the capability
//             this tool exists for: "what the bundle CAN do but didn't".
//   FORCED    the address exists only because a gate was forced, an equality pinned an arm the run did not
//             take, or a range was satisfied at all. Its reply is carried as FORCED and never merged into
//             the observed pool, because a CREDENTIALED reply to a navigation no client would make is the
//             plausible fabrication §provenance is entirely about.
const NAV_PROVENANCE_OBSERVED = "observed";
const NAV_PROVENANCE_DERIVED = "derived";
const NAV_PROVENANCE_FORCED = "forced";
const NAV_PROVENANCE_KINDS = Object.freeze([
  NAV_PROVENANCE_OBSERVED,
  NAV_PROVENANCE_DERIVED,
  NAV_PROVENANCE_FORCED,
]);

// THE CONTROL POINT. Every tab carries the person's cookies, so this predicate is the only thing standing
// between the engine's search and an action taken as the person; there is no partition, no interception and
// no downstream check to catch what it lets through.
//
// It is a CHECK and not a DCHECK — fatal in dev AND release — because check.h's law puts a security boundary
// on that side, and because the release exemption ("we cannot fix or add features in release") argues the
// wrong way here: a release build that cannot establish provenance must not navigate either.
//
// FORCED IS REFUSED OUTRIGHT TODAY, AND THE REFUSAL IS A BUILD ORDER RATHER THAN A POLICY. CLAUDE.md makes a
// forced address "the deliberate per-origin widening" — configurable, per-origin, default conservative, never
// inferred from a site looking like a test — and no such policy component exists in this zone: there is
// nothing to ask, so there is no origin for which the widening has been chosen, so the answer is no. The diff
// that BUILDS the per-origin widening replaces this arm with a call into it; until then a forced navigation
// crashes naming what to build rather than silently taking the widest reading of an absent setting.
function checkNavigationProvenance(url, provenance) {
  CHECK(typeof url === "string" && url.length > 0,
        "a navigation was decided with no address — the address IS the decision, so there is nothing here to " +
        "authorize and nothing downstream that would catch it");
  CHECK(NAV_PROVENANCE_KINDS.indexOf(provenance) !== -1,
        "navigation to '" + url + "' states no established provenance (got '" + provenance + "'). Every tab " +
        "sends the person's cookies, so the choice of address is the whole of the safety and it is decided " +
        "here or nowhere: an address is OBSERVED (the page made this navigation), DERIVED (the app's own " +
        "code computed it from real inputs) or FORCED (it exists only because a gate was forced), and a " +
        "fourth answer means the caller does not know what it is asking for");
  CHECK(provenance !== NAV_PROVENANCE_FORCED,
        "navigation to '" + url + "' is FORCED, which CLAUDE.md makes the deliberate per-origin widening — " +
        "and no per-origin navigation policy exists in the trusted zone to have chosen it for this origin. " +
        "BUILD that policy (configurable, per-origin, default conservative, never inferred from a site " +
        "looking like a test) and replace this arm with a call into it; a forced address navigated on an " +
        "absent setting is this tool acting as the person on a request their app never produced");
}

// ─── The record ──────────────────────────────────────────────────────────────
//
// EVERY FIELD IS REQUIRED AND EVERY FIELD IS ASSERTED, because CLAUDE.md's §"A FIELD A CONSUMER DEFAULTS" has
// seven worked instances in this tree and not one of them was a crash. A consumer here never fills a hole: a
// record that cannot state its address, its owner, how that address was arrived at, or the session its handle
// belongs to is not a partial record — it is a claim about one of the person's live logged-in tabs that
// nobody can act on.
//
//   navId       string  this record's own durable identity, minted here. Survives every epoch; it is what the
//                       person's view and any later close address, since tabId does not survive a restart.
//   url         string  the address the TRUSTED ZONE decided. It is the only field a person can recognise,
//                       and it is deliberately not re-read from the tab afterwards: what we owe them is what
//                       we chose to open, not where the page has since navigated itself.
//   owner       string  the cluster key of the flow that claimed it. Reconciliation exists to close what NO
//                       FLOW CLAIMS, which is unanswerable for a record that names no claimant.
//   provenance  string  one of NAV_PROVENANCE_KINDS — carried because a reply learned through a FORCED
//                       navigation must never be merged into the observed pool, and because the person's
//                       view has to be able to say which of their sessions was spent on what.
//   epoch       string  the browser session `tabId` was minted in.
//   openedAt    number  epoch ms.
//   state       "pending" | "open" | "orphaned"
//   tabId       number  present IFF state === "open". Absent while pending (no tab yet, or its id not yet
//                       known) and DROPPED when orphaned — a meaningless handle left on a record is an
//                       invitation to act on it, which is the one mistake this whole file is about.
//   orphanedAt  number  present IFF state === "orphaned".
const OWNED_STATE_PENDING = "pending";
const OWNED_STATE_OPEN = "open";
const OWNED_STATE_ORPHANED = "orphaned";

function checkOwnedNavigable(r) {
  DCHECK(!!r && typeof r === "object", "an owned-navigable record is an object");
  DCHECK(typeof r.navId === "string" && r.navId.length > 0, "owned-navigable record has no navId — it is the only identity that survives a browser restart, so a record without one cannot be closed, listed or dismissed");
  DCHECK(typeof r.url === "string" && r.url.length > 0, "owned-navigable record has no url — the person cannot recognise, and therefore cannot decline, an artifact this extension will not name");
  DCHECK(typeof r.owner === "string" && r.owner.length > 0, "owned-navigable record has no owner — reconciliation closes what NO FLOW CLAIMS, which is unanswerable for a record that names no claimant");
  DCHECK(NAV_PROVENANCE_KINDS.indexOf(r.provenance) !== -1, "owned-navigable record's provenance is not one of observed/derived/forced — the grade decides whether what this tab learned may be merged into the observed pool, and an ungraded reply is the plausible fabrication");
  DCHECK(typeof r.epoch === "string" && r.epoch.length > 0, "owned-navigable record has no epoch — its tabId is then a number with no stated session, i.e. a handle that may name one of the person's own tabs");
  DCHECK(typeof r.openedAt === "number" && r.openedAt > 0, "owned-navigable record has no openedAt");
  DCHECK(r.state === OWNED_STATE_PENDING || r.state === OWNED_STATE_OPEN || r.state === OWNED_STATE_ORPHANED,
         "owned-navigable record's state is none of pending/open/orphaned");
  // The two presence rules, each asserted as a BICONDITIONAL so a record cannot carry half of a state. A
  // tabId on anything but an open record is a handle nothing established; an open record without one is a tab
  // this extension opened and cannot close.
  DCHECK((r.state === OWNED_STATE_OPEN) === (typeof r.tabId === "number" && Number.isInteger(r.tabId) && r.tabId >= 0),
         "tabId is present exactly when state is open — absent while pending (no tab yet) and dropped when " +
         "orphaned (the id names another session's tab), so any other combination is a handle whose validity " +
         "nothing states");
  DCHECK((r.state === OWNED_STATE_ORPHANED) === (typeof r.orphanedAt === "number"),
         "orphanedAt is present exactly when state is orphaned — an orphaned record with no time cannot be " +
         "reported to the person in any order, and an open record carrying one is two states at once");
}

// Records claimed by THIS realm's lifetime. Reconciliation's whole premise is that at brain boot NO FLOW
// CLAIMS ANYTHING — every engine instance died with the previous offscreen document — so it must run before
// this realm issues its first claim. That is an invariant, so it is asserted rather than arranged.
let _ownedClaimsIssued = 0;

async function _putOwnedNavigable(r) {
  checkOwnedNavigable(r);
  await _ownedTx(OWNED_STORE_RECORDS, "readwrite", (s) => { s.put(r, r.navId); });
  return r;
}

// ─── Opening one ─────────────────────────────────────────────────────────────
//
// THE DECISION AND THE ACTION ARE ONE ENTRY, WHICH IS WHY THE ASSERT CANNOT BE STEPPED AROUND. A separate
// `authorize()` beside a separate `create()` is two mechanisms that must agree, and the window between them
// is exactly the harm — the same shape as the credential-stripping rule this design refused. There is no way
// to reach the browser's tab-create verb from this component without passing `checkNavigationProvenance`
// first, because this entry is the only thing that calls the edge.
//
// THE RECORD IS DURABLE BEFORE THE TAB EXISTS, and that ordering is the whole reason the record is worth
// having. A tab created before its record is a tab nothing can be told about if this realm dies in between —
// permanently, since nothing in Chrome distinguishes it from the person's own tabs afterwards. A record
// created before its tab is at worst an entry the person is told about that turned out never to open, which
// is a cost paid in one line of a list.
//
// `edges.createTab(url)` returns the new tab's id. It is passed in rather than reached for because neither
// realm this file loads in can call `chrome.tabs.*` directly (the offscreen document supports `chrome.runtime`
// and nothing else), and because a component whose browser verbs are arguments is one you can exercise with
// a fixture.
async function openOwnedNavigable(decision, edges) {
  DCHECK(!!decision && typeof decision === "object", "openOwnedNavigable takes the trusted zone's decision");
  DCHECK(!!edges && typeof edges.createTab === "function" && typeof edges.closeTab === "function",
         "openOwnedNavigable is handed its browser edges — a missing one is a tab that gets created and then " +
         "cannot be cleaned up, which is the orphan this component exists to make impossible");

  // The control point, before anything observable happens.
  checkNavigationProvenance(decision.url, decision.provenance);
  DCHECK(typeof decision.owner === "string" && decision.owner.length > 0,
         "a navigation was decided with no owning flow — reconciliation closes what no flow claims, so an " +
         "unowned tab is one that is either never closed or closed while something still needs it");

  const epoch = await readBrowserSessionEpoch();
  // Same reasoning one step on: a record whose handle belongs to no stated session is a handle we could later
  // act on wrongly, and the act is closing one of the person's tabs. Refuse to mint one.
  CHECK(epoch !== null,
        "no browser-session epoch is stored, so a tabId recorded now could not later be told apart from a " +
        "number naming one of the person's own tabs — the service worker writes the epoch at " +
        "chrome.runtime.onStartup before it creates this document, so its absence means this realm came up " +
        "by a path that did not");

  const rec = await _putOwnedNavigable({
    navId: crypto.randomUUID(),
    url: String(decision.url),
    owner: String(decision.owner),
    provenance: decision.provenance,
    epoch: epoch,
    openedAt: Date.now(),
    state: OWNED_STATE_PENDING,
  });
  _ownedClaimsIssued++;

  let tabId;
  try {
    tabId = await edges.createTab(rec.url);
  } catch (e) {
    // No tab exists, so the record names nothing. Drop it rather than leave the person a line about an
    // artifact that was never created.
    RETHROW_FATAL(e);
    await forgetOwnedNavigable(rec.navId);
    throw e;
  }
  DCHECK(typeof tabId === "number" && Number.isInteger(tabId) && tabId >= 0,
         "the createTab edge answered with something that is not a tab id — the record would then hold a " +
         "handle that closes nothing, and the tab it just opened would be unreachable");

  try {
    return await _putOwnedNavigable(Object.assign({}, rec, { state: OWNED_STATE_OPEN, tabId: tabId }));
  } catch (e) {
    // The tab exists and we cannot record it. Closing it is the only outcome that leaves nothing behind.
    await edges.closeTab(tabId);
    await forgetOwnedNavigable(rec.navId);
    throw e;
  }
}

// Every record, newest first. The read the person's see-and-stop view is built on; also what reconciliation
// walks. Each record is asserted on the way out, so a store corrupted by a second writer crashes here rather
// than being rendered as a list of plausible artifacts.
async function listOwnedNavigables() {
  const all = await _ownedTx(OWNED_STORE_RECORDS, "readonly", (s) => ({ __req: s.getAll() }));
  const out = Array.isArray(all) ? all : [];
  for (const r of out) checkOwnedNavigable(r);
  out.sort((a, b) => b.openedAt - a.openedAt);
  return out;
}

async function forgetOwnedNavigable(navId) {
  DCHECK(typeof navId === "string" && navId.length > 0, "forgetOwnedNavigable takes a navId");
  await _ownedTx(OWNED_STORE_RECORDS, "readwrite", (s) => { s.delete(navId); });
}

// The person closed a tab this extension opened. Called from the SW-forwarded `__evt TAB_REMOVED`, which is
// the only way this zone learns a tab went away.
//
// THE EPOCH IS PART OF THE MATCH, NOT AN EXTRA CHECK. A tabId alone would also match an orphaned record from
// a previous session whose number Chrome has since handed to a different tab, and the record it deleted would
// be the one artifact the person still needed telling about. Records outside the current epoch hold no tabId
// at all (see checkOwnedNavigable), so this cannot reach them even by accident — the epoch test is what makes
// that structural rather than incidental.
async function forgetOwnedNavigableByTabId(tabId) {
  DCHECK(typeof tabId === "number", "forgetOwnedNavigableByTabId takes a tab id");
  const epoch = await readBrowserSessionEpoch();
  if (epoch === null) return [];
  const gone = [];
  for (const r of await listOwnedNavigables()) {
    if (r.state !== OWNED_STATE_OPEN || r.epoch !== epoch || r.tabId !== tabId) continue;
    await forgetOwnedNavigable(r.navId);
    gone.push(r);
  }
  return gone;
}

// ─── Startup reconciliation ──────────────────────────────────────────────────
//
// CLAUDE.md: "a durable record of every navigable this extension opened, reconciled against what is actually
// open at startup, so a restart CLOSES what no flow claims" — and with every tab carrying the person's
// cookies this is not housekeeping, it is the other half of the address decision.
//
// WHAT NO FLOW CLAIMS IS, AT THIS MOMENT, EVERYTHING. Flows live in engine instances hosted by the offscreen
// document; when this document is (re)created every one of them is gone, so no record has a claimant left.
// That is the premise the whole function rests on, so it is asserted rather than assumed: reconciliation runs
// before this realm issues its first claim.
//
// FOUR OUTCOMES, AND THE LAST TWO ARE THE HONEST ONES:
//   • epoch is this session's, tab open  → CLOSE it and drop the record. The handle is exact.
//   • epoch is this session's, tab gone  → drop the record. Closed by the person, or by a TAB_REMOVED this
//                                          realm never saw.
//   • epoch is NOT this session's        → ORPHAN it, act on NOTHING, keep it for the person.
//   • state is PENDING                   → ORPHAN it. A tab may or may not have been created before this
//                                          realm died, and no id was ever recorded, so there is nothing to
//                                          act on either way. Reporting it is the only honest outcome.
//
// THE ORPHAN CASE IS A REAL LIMIT, NOT AN UNBUILT PIECE, and it is the refutation this component owes its
// reader. Chrome exposes no per-tab handle that survives a restart: `Tab.id` is "unique within a browser
// session" and `Tab.sessionId` is documented as "the session ID used to uniquely identify a tab obtained from
// the sessions API", i.e. not a property of a tab we hold. Matching on the URL instead is unsound in BOTH
// directions — a tab we opened has very likely navigated since (it is a real page running a real bundle), and
// a tab the person opened at that same address is theirs — so an orphan is RETAINED and reported rather than
// guessed at. Retention is not accretion for its own sake: the address of an artifact that may still be open
// is exactly the unrecoverable copy §disk-share refuses to shed, and the alternative to telling the person is
// not telling them.
async function reconcileOwnedNavigables(edges) {
  DCHECK(!!edges && typeof edges.listTabs === "function" && typeof edges.closeTab === "function",
         "reconcileOwnedNavigables is handed its two browser edges because neither realm this file loads in " +
         "can reach the same chrome.* surface — a missing edge is a reconciliation that silently does nothing");
  DCHECK(_ownedClaimsIssued === 0,
         "reconciliation ran after this realm had already opened a navigable, so 'close what no flow claims' " +
         "is false of at least one record it is about to close — it belongs in the boot chain, before the " +
         "first claim, not on a later trigger");

  const records = await listOwnedNavigables();
  const summary = { closed: [], resolved: [], orphaned: [], epoch: null };
  if (records.length === 0) return summary;

  const epoch = await readBrowserSessionEpoch();
  summary.epoch = epoch;

  // The browser's own answer to "what is actually open", asked once. A tab id absent from it is a tab that
  // does not exist; that is the only thing this list is used for.
  const live = new Set();
  if (epoch !== null) {
    const tabs = await edges.listTabs();
    DCHECK(Array.isArray(tabs),
           "the listTabs edge must answer with an array of tabs — a non-array is a broken browser edge, and " +
           "reading it as 'nothing is open' would drop every record while every tab stayed open");
    for (const t of tabs) if (t && typeof t.id === "number") live.add(t.id);
  }

  for (const r of records) {
    if (r.state === OWNED_STATE_ORPHANED) continue;          // already unactionable; kept for the person
    if (r.state === OWNED_STATE_PENDING || epoch === null || r.epoch !== epoch) {
      const orphan = Object.assign({}, r, { state: OWNED_STATE_ORPHANED, orphanedAt: Date.now() });
      delete orphan.tabId;                                   // the id names another session's tab now
      summary.orphaned.push(await _putOwnedNavigable(orphan));
      continue;
    }
    if (!live.has(r.tabId)) {
      await forgetOwnedNavigable(r.navId);
      summary.resolved.push(r);
      continue;
    }
    // Same browser session, tab still open, and no flow claims it (asserted above). Close it: it is one of
    // the person's logged-in sessions held open by an engine that no longer exists.
    await edges.closeTab(r.tabId);
    await forgetOwnedNavigable(r.navId);
    summary.closed.push(r);
  }
  return summary;
}
