/* lib/dev-engine-runs.js — the engine run records on the diagnostics page: bridge.js's `self._engineLog` as
   GET_ENGINE_RUNS answers it, one row per run. Each row states what the run cost (solver/result.c's cost
   counters), what it learned (endpoints, sinks), whether it reached code the page never called (the orphan
   census), what it parked for the next session, and the scheduler and subsystem censuses beside them.
   bridge.js builds two record shapes: a run with an @RESULT document carries every field, and a crashed run
   carries only `run`, `resumed`, `url` and `err`. A crash is rendered as the absence of counters, never as
   zeroes, and every field of the full shape is asserted rather than defaulted. A run has one row, which its
   partial snapshots rewrite, so a `partial` row's numbers are "so far". Built only through
   lib/dom-builder.js. */

const RUN_STATES = ["partial", "complete", "crashed"];

/* The section for `runs` (GET_ENGINE_RUNS's array, oldest first): a details element listing every run newest
   first, or a sentence for an empty log. */
function devEngineRunsSection(runs) {
  if (runs.length === 0) {
    return domElement("div", { class: "empty" }, ["No engine run has been logged in this session — the log is " +
                                                  "empty, which is not a run that learned nothing."]);
  }
  const label = (children) => domElement("span", { class: "deep-label" }, children);
  const FULL = [
    ["endpoints", "endpoints learned"], ["sinks", "@S sinks"], ["park", "flows parked for next session"],
    ["switches", "flow context-switches"], ["flows", "flows created"], ["candidates", "@S candidates run"],
    ["jobsQueued", "jobs queued"], ["jobsRun", "jobs run"],
    /* A queued job runs only with the execution context stack empty (HTML §8.1.4.4 step 3 of clean up after
       running script), so this says whether that boundary was reached: zero here beside thousands queued is
       a document none of whose programs finished. */
    ["unitsDone", "units of work completed"],
    ["worldSegmentsHeld", "cross-instance world segments held"],
    ["worldSegmentsMade", "…made (cumulative)"], ["worldSegmentsForked", "…forked"],
    /* What makes an empty @S surface readable: `securitySinks: []` has four readings that take opposite work
       (no attacker source read, none reached a sink, only the page's own strings arrived, or taint arrived
       behind an unforgeable check and the search waits on a sender at the pinned origin). */
    ["sourceReads", "attacker-source reads"], ["sinkReached", "sinks reached"],
    ["sinkTainted", "…with tainted input"], ["sinkNeedsSender", "…waiting on a sender (unforgeable check)"],
  ];
  const rows = runs.slice().reverse().map((m) => {
    DCHECK(typeof m.url === "string",
           "an engine run record reached the diagnostics page without a url — bridge.js writes it on BOTH " +
           "record shapes (the crash arm included), so a record missing it is that relay broken");
    /* `resumed` is a count or `null`, the stated absence of one: no instance reached the seeding of its
       frontier. "0 resumed" (seeded with no residue) must not be said when nobody looked. */
    DCHECK(m.resumed === null ||
           (typeof m.resumed === "number" && Number.isInteger(m.resumed) && m.resumed >= 0),
           "an engine run record reached the diagnostics page with a resume count that is neither a count nor " +
           "the stated 'not known' (`" + String(m.resumed) + "`) — bridge.js writes one or the other on BOTH " +
           "record shapes, so anything else is that relay broken and the user is shown a number of parked " +
           "flows nothing observed");
    const resumedTxt = m.resumed === null
      ? "resume state not known (this run never seeded a frontier)"
      : `${m.resumed} resumed`;
    /* What the cold tier answered, which `resumed` alone cannot say: `0 resumed` is true of a first visit, a
       redeployed bundle and a store that would not hand its residue over. bridge.js reads the frontier key and
       the keys its address holds in one transaction and states which of its words it met; `coldOther` (the
       entries parked under other bundle ids) and the bundle id are the evidence under the word, and the bundle
       id is what makes two runs of one address comparable. */
    DCHECK(m.coldLookup === null || (typeof m.coldLookup === "string" && m.coldLookup !== ""),
           "an engine run record reached the diagnostics page with a cold-tier lookup that is neither a word " +
           "nor the stated 'never read' (`" + String(m.coldLookup) + "`) — bridge.js writes one or the other " +
           "on BOTH record shapes, so anything else is that relay broken and a miss goes back to being a silence");
    DCHECK(m.coldOther === null ||
           (typeof m.coldOther === "number" && Number.isInteger(m.coldOther) && m.coldOther >= 0),
           "an engine run record reached the diagnostics page with a sibling-entry count that is neither a " +
           "count nor the stated absence of one (`" + String(m.coldOther) + "`) — it is the evidence under the " +
           "word beside it, and a `|| 0` here would render an arm that counted nothing as an address with nothing");
    const _bid = m.bundleId === null ? "" : ` (bundle ${m.bundleId})`;
    const _oth = m.coldOther === null ? "" : `${m.coldOther}`;
    // bridge.js's COLD_LOOKUP is the closed set; this switch is its one reading here, so a new word aborts.
    let coldTxt;
    switch (m.coldLookup) {
      case null:           coldTxt = "cold tier never read (this run did not reach its frontier lookup)"; break;
      case "not-asked":    coldTxt = "cold tier not asked (this document parks no residue)"; break;
      case "unreadable":   coldTxt = "cold tier UNREADABLE — this profile's storage refused the read, which is "
                                   + "a different fact from a page that has never been visited"; break;
      case "unvisited":    coldTxt = `cold tier holds nothing for this address${_bid} — first visit`; break;
      case "other-bundle": coldTxt = `cold tier MISS${_bid} — ${_oth} entr${m.coldOther === 1 ? "y" : "ies"} `
                                   + `parked at this address under OTHER bundle ids, so the bundle changed and `
                                   + `the key is correct to miss`; break;
      case "unread":       coldTxt = `cold tier DEFECT${_bid} — the store enumerates this run's own key for `
                                   + `this address and answered a read of it with nothing`; break;
      case "hit":          coldTxt = `cold tier HIT${_bid}`
                                   + (m.coldOther ? ` — ${_oth} further entr${m.coldOther === 1 ? "y" : "ies"} `
                                                  + `parked at this address under other bundle ids` : ""); break;
      default:
        DFAIL("an engine run record reached the diagnostics page with a cold-tier lookup this view does not " +
              "speak (`" + String(m.coldLookup) + "`) — bridge.js's COLD_LOOKUP is the closed set and this " +
              "switch is this realm's only reading of it, so an unrendered word is that set having grown " +
              "without its reader");
        coldTxt = "cold tier: " + String(m.coldLookup);
    }
    // Which of the three states this row is, asserted: a snapshot must never render as a finished run.
    DCHECK(RUN_STATES.indexOf(m.run) >= 0,
           "an engine run record reached the diagnostics page in a state this view does not speak (`" + m.run +
           "`) — bridge.js writes `run` on every record it logs, so a missing or unknown one is that seam " +
           "broken and a snapshot of a page still being analysed would be shown as a finished run of it");
    const where = String(m.url ? _shortUrl(m.url) : "(no source url)");
    if (m.run === "crashed") {
      /* The crash row says why (`err` carries the engine's root @WHY naming the missing capability) and states
         the resume state in all three forms: a count (the residue came back, then the run died), zero (the
         cold tier is not implicated) and not-known (the boot died before the frontier was seeded). */
      DCHECK(typeof m.err === "string" && m.err !== "",
             "a crashed engine run reached the diagnostics page with no `err` — bridge.js reads the " +
             "`engine-crash` line off the run's own output and asserts it there, so an absent one here is that " +
             "relay broken and the user is shown a crash with no cause");
      return domElement("div", { class: "deep-row" }, [label([
        `${where} — `, domElement("strong", {}, ["the engine crashed"]),
        `; this run reported no result document, so it has no counters at all (not zeroes)`
        + `; ${resumedTxt}; ${coldTxt}`
        + ` — ${m.err}`,
      ])]);
    }
    const live = m.run === "partial";
    const parts = FULL.map(([k, title]) => {
      // `endpoints`, `sinks` and `park` are lengths bridge.js takes off arrays; the rest are result.c counters.
      DCHECK(typeof m[k] === "number",
             "an engine run record reached the diagnostics page with no numeric " + k + " — every row of this " +
             "list is either a counter solver/result.c emits in its one cost snprintf or a length bridge.js " +
             "takes off an array that same document carries, and bridge.js asserts each of them before " +
             "writing this record, so a record that has an @RESULT document and is missing one is that seam " +
             "having changed underneath both");
      return title + " " + String(m[k]);
    });
    /* Whether this run drove code the page never called (solver/engine.h's orphan census), as one sentence
       because the pair is read together: `orphansAsked` separates "the bundle ships no uncalled code", "the
       walk ran and found none" and "no flow reached the end of its own work". `driven` counts drives SEEDED,
       not run; whether an uncalled body executed is the endpoint it recorded. On a resumed session a take can
       route to a waiting flow without raising `driven`, so the zero reading holds only on a fresh session.
       A seeded drive with no endpoint is displacement of a kind this pair cannot name. */
    DCHECK(typeof m.orphansDriven === "number" && typeof m.orphansAsked === "number",
           "an engine run record reached the diagnostics page without its orphan census — solver/result.c " +
           "emits `_orphansDriven`/`_orphansAsked` in the same cost snprintf as every counter above and " +
           "bridge.js asserts both before writing them onto this record, so a record that has an @RESULT " +
           "document and is missing one is that relay broken");

    /* Why `orphansAsked` can be zero, from solver/cold.h's partition of `outOfPrograms`: members never handed
       the thread, members suspended in a live frame (below the ladder's `if (!f->frame)`), and members
       standing where the next dispatch descends the ladder. These are gauges of one instant beside a lifetime
       count, so they are stated beside it and never differenced against it. The rows are asserted by name
       (bridge.js asserts only that `cold` is an object of finite rows). The arm each ladder member last
       returned through is printed by name and not classified as above or below the orphan seed: that needs
       flow_step's chain, which source order does not give. cold.c asserts the partition sums at its origin. */
    DCHECK(m.cold && typeof m.cold === "object" && !Array.isArray(m.cold),
           "an engine run record reached the diagnostics page with no `cold` census — solver/result.c composes " +
           "it into every document it builds and bridge.js relays it whole in the same object literal as the " +
           "orphan pair above, so a record carrying `orphansAsked` and no `cold` is that relay broken");
    for (const f of ["outOfPrograms", "outOfProgramsUnrun", "outOfProgramsFramed",
                     "outOfProgramsAtTheLadder"])
      DCHECK(typeof m.cold[f] === "number",
             "the engine's cold census carries no `" + f + "` — it is one of the four rows that say whether " +
             "any member was ever in a position to be asked the orphan question. TWO CAUSES IN THIS ORDER: " +
             "the loaded wasm predates this reader (read extension/lib/qjs/qjs.mjs.build.json's `head` and " +
             "ask whether the commit adding " + f + " is an ancestor of it — if it is not, the answer is to " +
             "BUILD and nothing here is wrong), or result_cold_json's composition changed under this seam");
    const ladderUnits = m.cold.outOfProgramsAtTheLadderUnits;
    const ladderArms = ladderUnits && typeof ladderUnits === "object"
      ? Object.keys(ladderUnits).filter((a) => ladderUnits[a] > 0)
              .sort((x, y) => ladderUnits[y] - ladderUnits[x])
      : [];
    const whereTheyStand =
      m.cold.outOfPrograms === 0
        ? `no member had run out of its own work when this was taken (\`outOfPrograms\` 0) — a GAUGE, so it `
          + `says where the frontier was at that instant and NOT that no member ever stood there`
        : m.cold.outOfProgramsAtTheLadder === 0
          ? `${m.cold.outOfPrograms} member(s) had run out of their own work, and NONE was in a `
            + `position to be asked: ${m.cold.outOfProgramsUnrun} had never been handed the `
            + `thread and ${m.cold.outOfProgramsFramed} were suspended inside a live frame, `
            + `which is below the ladder's own \`if (!f->frame)\`. That is a statement about the PICK and `
            + `about frames, not about the orphan take or the heap`
          : `${m.cold.outOfProgramsAtTheLadder} member(s) were standing where the next dispatch `
            + `descends the ladder (beside ${m.cold.outOfProgramsUnrun} never handed the thread `
            + `and ${m.cold.outOfProgramsFramed} in a live frame)`
            + (ladderArms.length === 0
                ? ``
                : `, by the arm each LAST returned through: `
                  + ladderArms.map((a) => String(ladderUnits[a]) + " " + String(a)).join(" · ")
                  + `. Read that arm's position in flow_step's chain against \`engine_orphan_seed\`: ABOVE `
                  + `the seed says these members ARE dispatched and that work the page arranged falls due `
                  + `ahead of the take every round; BELOW it says the member went PAST the seed`)
            + `. These are readings of ONE INSTANT and \`orphansAsked\` is a LIFETIME count, so they cannot `
            + `be differenced — but a population standing there beside an ask that never moved is the one `
            + `state in which the take is not the thing to open`;
    // Only a positive residue count makes a session resumed; `null` (never seeded) takes the fresh reading.
    const fromResidue = m.resumed !== null && m.resumed > 0;
    const orphan = m.orphansAsked === 0
      ? `uncalled code: the question was never reached — nothing walked the heap for a function the bundle `
        + `ships and never calls. WHY it was never reached is the row beside it, and the walk is the LAST arm `
        + `of flow_step's ladder, so every condition above it had to fall through first: ${whereTheyStand}. `
        + `That is a result about this RUN, not a fact about the page.`
      : m.orphansDriven === 0
        ? `uncalled code: the walk ran ${m.orphansAsked} time(s) and seeded no drive`
          + (fromResidue
              ? ` — but this session was seeded from a parked residue of ${m.resumed} flow(s), `
                + `where a take can route to a flow already waiting for that body without counting here. So `
                + `this does NOT establish that the bundle ships no uncalled code; the pair reads cleanly `
                + `only on a fresh session.`
              : `: the heap held no function the page had not already called. That is a fact about the PAGE, `
                + `and it is NOT evidence that anything was starved.`)
        : `uncalled code: ${m.orphansDriven} drive(s) SEEDED over `
          + `${m.orphansAsked} walk(s) — seeded, not run. The count is raised when the take `
          + `succeeds, before the drive reaches the frontier, so whether the uncalled body EXECUTED is `
          + `answered by the endpoints learned above and never by this number; a seeded drive with no `
          + `endpoint of its own is displacement, and this pair cannot say of which kind.`;
    /* The emitted surface's partitions, composed by bridge.js off the same @H array whose length is
       `endpoints` and written on every non-crashed record, so they are asserted (an engine older than a key
       makes one bucket read `(unstated)`, a fact about the build). The re-assert of each sum checks the record
       after it crossed a message boundary. Door, mint time and address class are three facts and none implies
       another; the buckets are rendered with the producer's own keys and no name list here, so a door added
       to solver/endpoint.h's ENDPOINT_DOORS reaches this reader unedited. A diagnostic, never a target. */
    DCHECK(m.endpointDoors && typeof m.endpointDoors === "object" && !Array.isArray(m.endpointDoors) &&
           m.endpointMintedAt && typeof m.endpointMintedAt === "object" &&
           !Array.isArray(m.endpointMintedAt) &&
           m.endpointAddressClass && typeof m.endpointAddressClass === "object" &&
           !Array.isArray(m.endpointAddressClass) &&
           m.endpointRazorClass && typeof m.endpointRazorClass === "object" &&
           !Array.isArray(m.endpointRazorClass) &&
           m.endpointWitnessClass && typeof m.endpointWitnessClass === "object" &&
           !Array.isArray(m.endpointWitnessClass),
           "an engine run record reached the diagnostics page without its endpoint partitions — bridge.js " +
           "builds every one of them off the same `fetchCallSites` array that the `endpoints` figure beside " +
           "them is the LENGTH of, and writes them onto every non-crashed record, so their absence is that " +
           "relay broken");
    for (const _p of [["endpointDoors", m.endpointDoors], ["endpointMintedAt", m.endpointMintedAt],
                      ["endpointAddressClass", m.endpointAddressClass],
                      ["endpointRazorClass", m.endpointRazorClass],
                      ["endpointWitnessClass", m.endpointWitnessClass]]) {
      let _n = 0;
      for (const k of Object.keys(_p[1])) _n += _p[1][k];
      DCHECK(_n === m.endpoints,
             "this run's `" + _p[0] + "` partition reached the diagnostics page summing to " + _n + " over an " +
             "emitted surface of " + m.endpoints + " row(s) — bridge.js builds the histogram and that figure " +
             "off ONE array at ONE moment and asserts they agree there, so a difference here is the record " +
             "having changed between that composition and this render");
    }
    const histCells = (h) => Object.keys(h).map((k) => String(k) + " " + String(h[k])).join(" · ");
    const surface = m.endpoints === 0
      ? `learned surface: no address at all, so there is nothing to partition — a statement about this RUN, `
        + `read WITH the uncalled-code sentence above and not against it`
      : `learned surface (${m.endpoints} address(es)) · composed by: `
        + `${histCells(m.endpointDoors)}`
        + ` · minted: ${histCells(m.endpointMintedAt)}`
        + ` · address: ${histCells(m.endpointAddressClass)} — WHICH MECHANISM, WHEN, and WHETHER THE RUN HAD `
        + `DETERMINED THE ADDRESS, and no one of the three implies another: a link element a router created `
        + `and one the markup declared arrive through the same door, a head whose first script runs before `
        + `the parser reaches the link below it mints that link post-program, and a literal chunk URL and one `
        + `built out of the fragment arrive through the same door as each other. "unknown" is the engine `
        + `stating it had NOT determined that address AND that its unknown is one somebody OUTSIDE the `
        + `engine supplied, which is the only one of these buckets that is a FLOOR under the hard bar below; `
        + `"unknown-unproven" is an address the run did not determine whose unknown the engine minted for its `
        + `own drive, or the served document itself spells, or no mint could speak for — undetermined and `
        + `REFUSED by that bar; "concrete" claims nothing at all about what a parse could reach. `
        + `A bucket named "(unstated)" is a row from an engine older than that key and is a fact about the `
        + `build, not about the page.`;
    /* The razor, read off the engine's own census `epReach` against `epEmitted` (a gauge taken at an instant,
       which is why its denominator is named and is not the array length above). solver/endpoint.c applies the
       door-to-reach map; this file holds no copy of it. `beyond` 0 is a refusal to claim, not a small number,
       and `either` is the undecidable population, so the floor and its bound are both rendered. No assert
       stands on these engine keys: an installed wasm older than a key is the ordinary case for a zone
       deployed on write, and its absence is stated (in two causes: no row, or class names this reader does
       not know) and never defaulted to a zero. */
    const _reach = m.cold.epReach;
    const _emitted = m.cold.epEmitted;
    const _reachStated = _reach !== null && typeof _reach === "object" && !Array.isArray(_reach)
                       && typeof _emitted === "number";
    const _beyond = _reachStated ? _reach.beyond : undefined;
    const _either = _reachStated ? _reach.either : undefined;
    const _razorStated = typeof _beyond === "number" && typeof _either === "number";
    const _bound = !_razorStated || _either === 0
      ? ``
      : ` A FLOOR, not a total: ${_either} further address(es) came through a mechanism that is `
        + `reached by a parser-inserted element and by a script-created one alike and does not record which, `
        + `and the producer declines to guess rather than under-credit one or over-credit the other — so the `
        + `true figure lies between the two and neither half is quotable alone.`;
    const razor = !_reachStated
      ? `razor — what this engine reached that a markup parse could not: NOT STATED BY THIS BUILD. The `
        + `installed engine composes no such census row, so nothing here has claimed or refused anything. `
        + `That is a fact about the BUILD and never about this page, and it is NOT a zero.`
      : !_razorStated
        ? `razor — what this engine reached that a markup parse could not: NOT READABLE FROM THIS BUILD. The `
          + `census carries the partition under class names this reader does not know, so no floor can be `
          + `read off it — again a fact about the BUILD, and again NOT a zero.`
        : _emitted === 0
          ? `razor — what this engine reached that a markup parse could not: this run emitted no address at `
            + `all, so there is no surface to partition and the razor has no population here. A statement `
            + `about this RUN, read WITH the surface row above and not against it.`
          : _beyond === 0
            ? `razor — what this engine reached that a markup parse could not: NONE of `
              + `${_emitted} emitted address(es). That is this run's REFUSAL TO CLAIM the `
              + `capability on this document and NOT a small number — every address on the surface above is `
              + `one a parse of the served bytes hands a reader for free.${_bound} A DIAGNOSTIC AND NEVER A `
              + `TARGET: an identity read WITHIN this run, not against another run's.`
            : `razor — what this engine reached that a markup parse could not: `
              + `${_beyond} of ${_emitted} emitted address(es) came through a `
              + `mechanism NO parse of the served document reaches.${_bound} A DIAGNOSTIC AND NEVER A `
              + `TARGET: an identity read WITHIN this run, not against another run's. The denominator is the `
              + `engine's own census taken at that instant, and is not the array length in the row above.`;
    /* The hard bar from the census's address classes: `unknown` rows (undetermined, resting on an unknown
       somebody outside the engine supplied) definitely clear it; `concrete` claims nothing about a parse. It
       is a floor that overlaps the razor and is never added to it. Same stated-absence rules and denominator
       as the razor, read off the same `_emitted` local so both sentences share one instant. */
    const _acls = m.cold.epAddressClass;
    const _aclsStated = _acls !== null && typeof _acls === "object" && !Array.isArray(_acls)
                      && typeof _emitted === "number";
    const _undet = _aclsStated ? _acls.unknown : undefined;
    const _barStated = typeof _undet === "number";
    const hardBar = !_aclsStated
      ? `hard bar — addresses NO parse of the served bytes can state: NOT STATED BY THIS BUILD. The installed `
        + `engine composes no such census row, so nothing here has claimed or refused anything. That is a `
        + `fact about the BUILD and never about this page, and it is NOT a zero.`
      : !_barStated
        ? `hard bar — addresses NO parse of the served bytes can state: NOT READABLE FROM THIS BUILD. The `
          + `census carries the partition under class names this reader does not know, so no floor can be `
          + `read off it — again a fact about the BUILD, and again NOT a zero.`
        : _emitted === 0
          ? `hard bar — addresses NO parse of the served bytes can state: this run emitted no address at all, `
            + `so there is no surface to partition and the bar has no population here. A statement about this `
            + `RUN, read WITH the surface row above and not against it.`
          : _undet === 0
            ? `hard bar — addresses NO parse of the served bytes can state: NONE of `
              + `${_emitted} emitted address(es) was one this run had not determined AND whose `
              + `unknown somebody outside the engine supplied. That is a `
              + `REFUSAL TO CLAIM the bar on this document and NOT a small number. It is a FLOOR and not a `
              + `verdict: a determined address may still be past every parse — one a flow PINNED and re-read, `
              + `or one a REPLY named — and this engine cannot say which, so the floor here and the razor `
              + `above OVERLAP and may not be added. A DIAGNOSTIC AND NEVER A TARGET, read WITHIN this run.`
            : `hard bar — addresses NO parse of the served bytes can state: at least `
              + `${_undet} of ${_emitted} emitted address(es) — the run reached `
              + `each of those holding a value it had NOT determined, resting on an unknown a document, a `
              + `server, an attacker or the environment supplied, so it stands in no served byte. (An `
              + `undetermined address whose unknown the ENGINE minted for its own drive is counted by none of `
              + `this: it is the engine's own "unknown-unproven" and the bar refuses it.) A FLOOR and not a `
              + `verdict: some of the remaining `
              + `${_emitted - _undet} may be past a parse too (a pinned source re-read, an `
              + `address a reply named) and nothing here guesses which, so this floor and the razor above `
              + `OVERLAP and may not be added. A DIAGNOSTIC AND NEVER A TARGET, read WITHIN this run.`;
    /* The same bar composed per row by solver/endpoint.c (`razorClass`: the `unknown` rows together with the
       rows whose door handed it bytes never in the served document) and partitioned by bridge.js, so it is one
       floor and not two halves. Its denominator is `endpoints`, the array length, not the census gauge, so it
       cross-checks the row above rather than adding to it. Readability asks whether a class name this reader
       knows appears, which covers both an engine older than the key and a class list that moved, without
       naming bridge.js's `(unstated)` constant from another realm. */
    const _razorCls = m.endpointRazorClass;
    const _only = _razorCls["runtime-only"] === undefined ? 0 : _razorCls["runtime-only"];
    const _razorReadable = _razorCls["runtime-only"] !== undefined || _razorCls["unproven"] !== undefined;
    const composedBar = m.endpoints === 0
      ? `hard bar, composed — this run emitted no address at all, so there is no surface to partition and the `
        + `bar has no population here. A statement about this RUN.`
      : !_razorReadable
        ? `hard bar, composed — NOT READABLE FROM THIS BUILD. No emitted address carries a razor class this `
          + `reader knows, which is either an installed engine older than the key or one whose class list `
          + `moved under this zone — a fact about the BUILD in both cases, and NOT a zero. The two `
          + `bracketing rows above are what this build can say.`
        : _only === 0
          ? `hard bar, composed — NONE of ${m.endpoints} emitted address(es) is one this run `
            + `PROVED exists only at run time. That is a REFUSAL TO CLAIM the bar on this document and NOT a `
            + `small number. Still a FLOOR: "unproven" claims nothing about what a parse could reach, and a `
            + `source this flow pinned and re-read really is past every parse — the row below is how large `
            + `that population can be.`
          : `hard bar, composed — ${_only} of ${m.endpoints} emitted address(es) `
            + `is one this run PROVED exists only at run time: it reached the address holding a value it had `
            + `not determined AND resting on an unknown a document, a server, an attacker or the environment `
            + `supplied, or it read the address out of bytes that were never in the served document. This `
            + `is the UNION of those two, composed per address by the engine, so it is ONE floor and not two `
            + `to bracket. Still a FLOOR: "unproven" claims nothing about what a parse could reach. A `
            + `DIAGNOSTIC AND NEVER A TARGET, read WITHIN this run; its denominator is the emitted array's `
            + `own length above and not the engine's census figure.`;
    /* The bound on the population that bar cannot size: `may-rest-on` rows were composed by a path that pinned
       a source's value on an arm its own example contradicted. A may, never added to the bar. Zero says the
       bar's `unproven` is the engine having proved nothing; nonzero says part of it is a population the bar
       cannot look at. */
    const _wCls = m.endpointWitnessClass;
    const _mayRest = _wCls["may-rest-on"] === undefined ? 0 : _wCls["may-rest-on"];
    const _unasked = _wCls["unasked"] === undefined ? 0 : _wCls["unasked"];
    const _wReadable = _wCls["may-rest-on"] !== undefined || _wCls["no-witness"] !== undefined ||
                       _wCls["unasked"] !== undefined;
    const witnessRow = m.endpoints === 0
      ? ``
      : !_wReadable
        ? `may rest on a witness — NOT READABLE FROM THIS BUILD. No emitted address carries a witness class `
          + `this reader knows, which is an installed engine older than the key or one whose class list moved `
          + `under this zone. A fact about the BUILD and NOT a zero.`
        : (_mayRest === 0
            ? `may rest on a witness — NONE of ${m.endpoints} emitted address(es) was composed `
              + `by a path that had pinned a source's value on an arm its own example contradicted. So this `
              + `run's "unproven" above is the bar having genuinely proved nothing, rather than a population `
              + `the bar could not look at.`
            : `may rest on a witness — ${_mayRest} of ${m.endpoints} emitted `
              + `address(es) was composed by a path that HAD pinned a source's value on an arm its own `
              + `example contradicted. That is a MAY and never a bar: it does not say the address read that `
              + `source. It is the bound on the one population "unproven" above cannot look at, because `
              + `a pinned read comes back as a bare value and the address then grades "concrete".`)
          + (_unasked === 0 ? ``
             : ` ${_unasked} address(es) could not be asked at all — no flow stood when the `
               + `record was minted, which is a door composing a request outside the scheduler and is a `
               + `finding about that door rather than about an address.`);
    /* Whether this tool's own egress policy is why the surface is the size it is: bridge.js raises
       `egressAsked` and `egressDeclined` in engineServiceFetch's one loop. The pair is rendered together,
       `asked` being the denominator, and the refusal reasons travel as the policy's own strings, since one may
       name a row of the person's own control and another a refusal nothing reopens. */
    DCHECK(typeof m.egressAsked === "number" && Number.isInteger(m.egressAsked) && m.egressAsked >= 0 &&
           m.egressDeclined && typeof m.egressDeclined === "object" && !Array.isArray(m.egressDeclined),
           "an engine run record reached the diagnostics page without its egress census — bridge.js raises " +
           "both halves in engineServiceFetch's one loop and asserts their containment before writing them " +
           "onto every non-crashed record, so a missing or mistyped half is that relay broken");
    let _decl = 0;
    for (const k of Object.keys(m.egressDeclined)) _decl += m.egressDeclined[k];
    const egress = m.egressAsked === 0
      ? `egress: this run asked this tool's chokepoint for nothing, so its own policy refused nothing and is `
        + `not what the surface figure above is measuring`
      : _decl === 0
        ? `egress: ${m.egressAsked} request(s) asked of this tool's chokepoint and NONE refused `
          + `— so the surface above is what the driving reached, and widening an origin would not add to it`
        : `egress: ${_decl} of ${m.egressAsked} request(s) REFUSED by this tool's `
          + `own policy · ${histCells(m.egressDeclined)} — which is a different fact from a page this engine `
          + `never derived the request for, and the two take opposite work: widen the origin, or improve the `
          + `driving.`;
    /* The frontier order (solver/result.c's `_wfq`, relayed whole): readings of one instant, rendered
       generically from the census's own rows so a row added there reaches this page unedited. A finalize
       composes its document after the frontier drained or parked, so `members: 0` is a sentence, never an
       order of zeroes. */
    DCHECK(m.wfq && typeof m.wfq === "object" && Number.isInteger(m.wfq.members),
           "an engine run record reached the diagnostics page with no WFQ census — solver/result.c composes it " +
           "into every document it builds and bridge.js asserts its shape before relaying it whole, so a " +
           "record that has an @RESULT document and no `wfq` is that relay broken");
    const order = m.wfq.members === 0
      ? `frontier order: no members standing when this was taken${live ? "" : " — the run drained or parked, so there was no order left to report"}`
      : `frontier order over ${m.wfq.members} flows: `
        + Object.keys(m.wfq).filter((k) => k !== "members")
                .map((k) => String(k) + " " + String(m.wfq[k])).join(" · ");
    /* What the order is denominated in (solver/quantum.h's `_quantum`), directly under it because it decides
       whether two runs' orders may be compared. On a wall-clock host the aging charge bills wall time to
       whichever flow the OS left running; on a CPU-clocked host every billed microsecond is one the flow held
       the thread for, but the slice is that same CPU, so the order moves on a loaded machine too. Hence the
       instruction is unconditional: compare two runs of one build before reading a difference between two. */
    DCHECK(m.quantum && typeof m.quantum === "object" && typeof m.quantum.isCpu === "boolean"
           && typeof m.quantum.measure === "string" && typeof m.quantum.sliceMs === "number",
           "an engine run record reached the diagnostics page with no `quantum` denomination — " +
           "solver/quantum.c composes it into every document result.c builds and bridge.js asserts its three " +
           "fields before relaying it whole, so a record that has an @RESULT document and no `quantum` is that " +
           "relay broken");
    const denom = label([`scheduler slice ${m.quantum.sliceMs} ms, billed in ${m.quantum.measure}`
      + (m.quantum.isCpu
          ? ` — real thread CPU, so every microsecond the aging charge bills is one the flow held the thread `
            + `for. That does not make the order reproducible: the slice is that same CPU, and a loaded `
            + `machine costs one opcode sequence more of it.`
          : ` — NOT CPU: the aging charge that produced the order above bills wall time to whichever flow the `
            + `OS left running, so a descheduling nothing in the page chose moves one flow's rank alone — a `
            + `second and larger source of variance on top of the one a CPU-clocked host has.`)
      + ` Either way, two runs of THIS SAME build over this same page take different orders: compare two runs `
      + `of one build before reading a difference between two builds.`]);
    /* The subsystem censuses, relayed whole by bridge.js and rendered generically: `_cold`, `_heap`, `_swap`
       (solver/result.c) and `_absent` (solver/absent.c: names a standard owns that the document read and this
       realm did not answer, which a feature-detecting page otherwise loses silently). A row is a number or a
       histogram (the ladder's per-arm tables), so an object renders as its buckets. decide.c's `_forkAt`
       follows; its `{}` is the positive statement that the document never forked and is said in words. */
    const CENSUS = [["cold", "frontier + cold tier"], ["heap", "heap"], ["swap", "COW switch"],
                    ["absent", "asked for and not answered"]];
    const censusRows = CENSUS.map(([k, title]) => {
      DCHECK(m[k] && typeof m[k] === "object",
             "an engine run record reached the diagnostics page with no `" + k + "` census — solver/result.c " +
             "composes it into every document it builds and bridge.js asserts its shape before relaying it " +
             "whole, so a record that has an @RESULT document and no `" + k + "` is that relay broken");
      const cell = (v) => (v !== null && typeof v === "object"
        ? Object.keys(v).map((a) => String(a) + " " + String(v[a])).join(", ")
        : String(v));
      return label([`${title}: `
                    + Object.keys(m[k]).map((f) => String(f) + " " + cell(m[k][f])).join(" · ")]);
    });
    DCHECK(m.forkAt && typeof m.forkAt === "object",
           "an engine run record reached the diagnostics page with no `forkAt` table — solver/decide.c " +
           "composes it into every document result.c builds, so its absence is that relay broken and not a " +
           "document that never forked, which is `{}` and is rendered as the sentence it is");
    censusRows.push(label([Object.keys(m.forkAt).length === 0
      ? `forked at: nothing — no predicate of this document ever split a flow`
      : `forked at: ` + Object.keys(m.forkAt).map((f) => String(f) + " ×" + String(m.forkAt[f])).join(" · ")]));
    // A snapshot's numbers are real observations of a page still being analysed, labelled as a running total.
    const head = live
      ? [domElement("strong", {}, ["still running"]), ` — snapshot, not a total; ${resumedTxt}; ${coldTxt} · so far: `]
      : `run complete · ${resumedTxt}; ${coldTxt} · `;
    /* Row order is load-bearing: sentences point at their neighbours by position ("the endpoints learned
       above", "the surface above", "this run's `unproven` above"), so the orphan sentence follows the cost
       row, the razor and bar rows follow the surface row, and the witness row follows the composed bar. */
    return domElement("div", { class: "deep-row" }, [
      label([`${where} — `, head, parts.join(" · ")]),
      label([orphan]),
      label([surface]),
      label([razor]),
      label([hardBar]),
      label([composedBar]),
      label([witnessRow]),
      label([egress]),
      label([order]), denom, censusRows,
    ]);
  });
  const live = runs.filter((m) => m.run === "partial").length;
  return domElement("details", { class: "deep-cross-tab" }, [
    domElement("summary", {}, [`Engine runs — cost, what was learned, what was parked `
                               + `(${runs.length}${live ? `, ${live} still running` : ""})`]),
    rows,
  ]);
}
