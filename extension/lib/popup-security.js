/* Popup security panel — extracted from popup.js (classic script, shares the popup global scope + DOM).
   Renders the @S securityFindings (source -> sink -> poc) and drives live-verify: _handleVerify opens the
   sandboxed attacker popup, _pollVerify + the message listener report REAL EXPLOIT / NOT REPRODUCED. */
// ─── Security Panel ──────────────────────────────────────────────────────────

// §@S(d)'s ENVELOPE VOCABULARY, rendered. The TOKENS are the engine's — solve.h documents both sets, one
// `firesOn` per sink class and one `delivery` per source mechanism — and this file owns only the English for
// them. Keeping the sentences here and the tokens in C is the split that was missing: the old card carried a
// host-side `{hash}|{search}|{pm}|{reply}` taxonomy of its own, which is a second statement of the engine's
// attacker-source model and had already drifted into naming sources no component declares. A token with no
// sentence is a contract drift and DCHECKs at the card rather than rendering the bare token, because a bare
// token in a reproduction envelope is exactly the silence this record exists to end.
var _FIRES_ON = {
  "sink-evaluates": "auto-fires: the sink evaluates the payload where it stands — no interaction, no navigation",
  "parse-insert":   "auto-fires at insertion: the injected markup's onload/onerror handler runs when the page parses it — no click needed",
  "navigation":     "fires on navigation: the payload is a javascript: URL, so it runs when the navigation this sink starts is performed (for a form action, on submission)",
};
var _DELIVERY = {
  // A URL HAS EXACTLY TWO PLACES an attacker-controlled component lives, and `concolic_declare_source` refuses
  // an `address` source without one — so the third arm (`String(p)`) named a URL component no browser has, out
  // of a record that cannot exist. offscreen-brain.js's buildLiveDelivery asserts the identical pair at the
  // layer that PERFORMS the navigation; this is the same contract at the layer that DESCRIBES it, and the two
  // disagreeing is how a card and its verify came to say different things before.
  "address":           function (p) {
                         DCHECK(p === "#" || p === "?",
                           "an @S record declared an `address` delivery whose component is neither `#` nor `?` ("
                           + JSON.stringify(p) + ") — concolic_declare_source (solver/concolic.c) refuses an "
                           + "address source with no prefix, so this is either a component added in C with no "
                           + "placement here or a record that did not come from solve_json_array");
                         return "delivered in the victim's own URL (" + (p === "#" ? "fragment" : "query string")
                              + ") — one navigation is the whole PoC";
                       },
  "plant":             function ()  { return "TWO-STAGE (stored): the attacker plants the value on the victim's origin first, and the victim's later load is what fires it"; },
  "referring-address": function ()  { return "the payload rides the address the victim ARRIVES FROM — the attacker's URL, not the victim's"; },
  "user-file":         function ()  { return "the user must hand the document an attacker-supplied file — no navigation delivers it"; },
};

// The delivery clause for either entry shape. An ABSENT `delivery` is a statement (the source declared none);
// a PRESENT one this view has no sentence for is drift, and drift asserts — then still says WHICH token it
// could not render, because a release build strips the assert and a thrown TypeError here would empty the
// whole panel, which reads as "nothing is here".
function _deliverySentence(item) {
  if (!item.delivery)
    return "the engine declares no browser delivery for this source — nothing carries or transforms these bytes "
         + "on the way in, so there is no navigation that reproduces it";
  DCHECK(!!_DELIVERY[item.delivery],
    "an @S record reached the popup with a delivery mechanism this view has no sentence for ("
    + JSON.stringify(item.delivery) + ") — the token vocabulary is solve.h's, declared beside each attacker "
    + "source in C, so either a mechanism was added there without its sentence here or this record did not "
    + "come from solve_json_array (source=" + item.source + ")");
  if (!_DELIVERY[item.delivery])
    return "the engine declares the delivery `" + item.delivery + "`, which this view has no sentence for";
  return _DELIVERY[item.delivery](item.deliveryPrefix);
}

// §@S(a): A FIRING BREAKOUT IN THE MODEL IS NOT YET A WORKING EXPLOIT — it has to run under the page's ACTUAL
// policy, and the engine answers that with TWO INDEPENDENT facts on the record. `cspBlocks` carries the policy
// text that kills this vector; `trustedTypes` carries the sink GROUP the document requires a trusted type for,
// which makes the assignment THROW before the markup is ever parsed. Either one alone means the payload does
// not run on the real page.
//
// The badge asked only the first. So a sink under `require-trusted-types-for 'script'` badged a clean HIGH XSS
// one line above an envelope that said the assignment throws — the card contradicting itself, and what a
// reader takes away from a card is the badge. That is CLAUDE.md's recorded `cspBlocked` defect a second time:
// there, the popup read a name the engine never wrote; here, it reads one of the two names the engine DOES
// write and ignores the other, and the visible consequence is identical — a policy-dead vector reported as a
// clean XSS. So the verdict is computed ONCE, here, and every surface that states one reads this.
function _policyBlockers(item) {
  var out = [];
  if (item.cspBlocks)    out.push({ what: "CSP", detail: item.cspBlocks });
  if (item.trustedTypes) out.push({ what: "Trusted Types", detail: item.trustedTypes });
  return out;
}
function _blockerNames(blockers) {
  var n = [];
  for (var i = 0; i < blockers.length; i++) n.push(blockers[i].what);
  return n.join(" + ");
}

// THE ENCODE SET IS THREE STATES AND THIS READ TWO. `concolic_source_encodes` (solver/concolic.h) returns the
// bytes a source's own component percent-encodes, the EMPTY STRING for a declared source that encodes nothing,
// and NULL only for a source with no delivery declaration at all — and `emit_delivery` emits the empty string
// as an empty string, because `if (enc)` is true for it in C. On this side `if (item.sourceEncodes)` is FALSE
// for it, so a source that carries the attacker's bytes to the page UNTOUCHED — the strongest fact this field
// has, and the reason a raw-fragment JS-context breakout is real — read exactly like a source the engine never
// declared. Returns HTML (it emits <code>), so callers place it WITHOUT esc().
function _encodeSentence(item) {
  if (typeof item.sourceEncodes !== "string") return "";   // undeclared source — _deliverySentence states that
  if (item.sourceEncodes === "")
    return "the browser percent-encodes <strong>nothing</strong> in this source — the bytes reach the page "
         + "exactly as the attacker wrote them";
  return "the browser percent-encodes <code>" + esc(item.sourceEncodes) + "</code> in this source, so a "
       + "candidate needing those bytes arrives escaped";
}

// WHICH QUESTION A PARKED SEARCH IS STUCK ON, which is the whole content of the parked card and which one
// number could never say. `solve.h` emits four fields for exactly this and the panel rendered none of them,
// so two states needing OPPOSITE work printed byte-identically:
//   * `turns:0` — the WFQ has never once given this search the thread. Nothing has RUN, so "N breakouts run"
//     was itself false: `tried` is raised where a candidate is SEEDED (solve_seed_candidates), `turns` where
//     one is switched in (solve_flow_begin). This is a scheduling question.
//   * `turns:N, reached:0, survived:0` — the candidates have held the thread and NONE of their bytes has been
//     seen at any sink at all. A distance-through-the-document question, and the opposite action from above.
//   * `turns:N, reached:0, survived:S>0` — their bytes DID get through the page's own transforms, S of L of
//     them, and no whole breakout arrived. The page's FILTER is what is eating the candidate: a question about
//     the BYTES, arriving long before the sink, and until `survived` existed it printed as the line above it.
//   * `reached:M, escaped:0` — they arrived and none reached an EXECUTABLE position: the bytes are still
//     inside the literal, comment or text they were written into. Also a question about the BYTES.
//   * `reached:M, escaped:E>0` — a program EXISTS and has not been run yet. That belongs to the flow's
//     sequence, and this card becomes a fired PoC when it runs.
// THE THIRD STATE USED TO BE READ OFF `fires` AND THAT WAS THE WRONG FIELD. `fires` counts every auto-firing
// handler in the parse INCLUDING the page's own template markup, so an innerHTML template that already carries
// an `<img onerror=…>` raised it for a candidate that escaped nothing — and this card stated "NONE reached an
// executable position" as a fact about the payload on evidence that was partly about the markup around it.
// `escaped` is the observation itself (solve.h: the eval sink asks ECMAScript §12's own scan whether the marker
// BEGINS an input element, the markup sink reads it out of an auto-firing handler, the URL sink asks whether
// the address survived as a `javascript:` one), so that is what the sentence is now computed from.
// `fires` ABSENT is a statement and not a missing field: solve.c emits it only for a class whose `queues_fire`
// is set, and the eval class evaluates its own argument (ECMAScript §19.2.1 eval ( source ), and §20.2.1.1.1
// CreateDynamicFunction ( ctor, newTarget, kind, paramArgs, bodyArg ) for the Function form), so there is no
// queue to count. A `0` written there would read as "nothing executable" when it means "nothing to queue" —
// which is why this reads the absence positively rather than defaulting it.
function _parkedProgress(item) {
  var held = 'its candidates have held the thread ' + item.turns + ' time' + (item.turns === 1 ? "" : "s");
  var got  = item.survived + ' of ' + item.survivedOf + ' bytes of the furthest candidate';
  // HOW MANY OF THIS SEARCH'S CANDIDATES HAVE NEVER BEEN SEEN AT A SINK — counted, never indexed. solve.h
  // says the inert context probe is entry 0 of a DERIVED class and that it is told apart by carrying no
  // marker; deciding WHICH entry is the probe from its POSITION would be this view restating a producer fact
  // it cannot check, which is the drift these cards exist to end. A count needs no such claim and answers the
  // question anyway: with `survived` saturated at a full-length run, "and N of its M candidates have never
  // been seen at a sink" is what says the run belongs to one candidate and the others never travelled.
  var unseen = item.survivedBy.filter(function (n) { return n === 0; }).length;
  var alsoUnseen = unseen
    ? ', and ' + unseen + ' of its ' + item.survivedBy.length + ' candidate'
      + (item.survivedBy.length === 1 ? "" : "s") + ' ' + (unseen === 1 ? 'has' : 'have')
      + ' never been seen at a sink at all'
    : "";

  if (item.turns === 0)
    return 'the scheduler has not yet given this search a turn — its ' + item.tried + ' candidate'
         + (item.tried === 1 ? " is" : "s are") + ' seeded and queued and NOTHING has run yet, so this is a '
         + 'scheduling state and not a search that failed';
  if (item.reached === 0 && item.survived === 0)
    return held + ' and NONE of their bytes has been seen at any sink — the flows run and do not get this far '
         + 'through the document, so this is still a distance question and not one about the payload';
  if (item.reached === 0)
    return held + ' and ' + got + ' survived the page\'s own transforms to a sink' + alsoUnseen
         + ', but no whole breakout has ARRIVED at this sink. A candidate that has never been seen at a sink '
         + 'has not re-traversed the document yet; one whose bytes arrived short of their own length was cut '
         + 'down by the page\'s own FILTER. Those are different questions and this line separates them';
  if (item.escaped === 0)
    return item.reached + ' breakout' + (item.reached === 1 ? "" : "s") + ' arrived at the sink and NONE '
         + 'reached an executable position — the bytes are still inside the literal, comment or text they were '
         + 'written into, so the parse never runs them (' + got + ' survived). A question about the payload';
  if (item.fires === undefined)
    return item.escaped + ' arrival' + (item.escaped === 1 ? "" : "s") + ' reached an executable position and '
         + 'the marker did not call — this sink EVALUATES its own argument, so the escape is out of its §12 '
         + 'state and the Script the page built around it either does not parse or threw before the call';
  return item.fires + ' executable program' + (item.fires === 1 ? "" : "s") + ' queued from ' + item.escaped
       + ' escaped arrival' + (item.escaped === 1 ? "" : "s") + ' and not yet run — the breakout EXISTS; this '
       + 'card becomes a fired PoC when the flow that holds it is scheduled';
}

// WHAT THE SEARCH ACTUALLY RAN, which is the field of the four that is not a count — and the state a parked
// search is most often in (arrived, did not fire) is a question about the BYTES that no quantity answers.
// Returns HTML (it emits <code>), so callers place it WITHOUT esc().
//
// AN EMPTY LIST IS A POSITIVE STATEMENT AND NOT A HOLE. solve.c raises `tried` for a COLD-RESUMED candidate
// (solve_resume_candidate) whose payload rides the resumed flow rather than this session's record, and its own
// DCHECK permits `npl == 0 || tried > 0` for exactly that reason. So an empty list beside a non-zero `tried`
// says the candidates came back from the cold tier, never that the search constructed nothing.
//
// WHICH ENTRY IS THE INERT CONTEXT PROBE IS NOT RE-DERIVED HERE. solve.h says the probe is told apart by
// carrying no marker, and the marker vocabulary is the engine's; guessing it from the ORDER ("entry 0") would
// be this view restating a producer fact it cannot check, which is the drift the whole card exists to end.
function _payloadList(item) {
  if (!item.payloads.length) return "";
  var out = '<div class="card-poc"><span class="poc-lbl" title="the candidates this search has run, as the '
          + 'search built them — never as the browser delivers them; the source\'s own transform is stated '
          + 'separately, beside this">candidates run</span>';
  for (var i = 0; i < item.payloads.length; i++)
    out += ' <code class="poc-payload">' + esc(item.payloads[i]) + '</code>';
  return out + '</div>';
}

// Stable key for a FIRED finding card within a tab. The engine emits one record per (sink, source); key by
// that + the script the sink lives in.
//
// NOTHING HERE IS DEFAULTED. This runs only from the fired loop, below the DCHECK that sink/source/poc are all
// present, so `|| "?"` / `|| ""` beside them was the release behaviour of a contract already declared — and it
// was not inert: two drifted records both missing a poc collapse onto ONE key, so the second card's verify
// result lands in the first card's row and reports Chrome's answer about the wrong payload. `sourceUrl` is the
// one genuinely optional half — the engine analyses inline script that has no address of its own and bridge.js
// carries that as "" — so the empty case is NAMED rather than filled.
function _findingKey(entry) {
  const it = entry.item;
  return (entry.sourceUrl || "(inline)") + "|" + it.sink + "|" + it.source + "|" + it.poc;
}


function renderSecurityPanel() {
  const container = document.getElementById("security-findings");
  const empty = document.getElementById("security-empty");

  // `tabData` ABSENT and `securityFindings` ABSENT ARE TWO DIFFERENT STATEMENTS, and `tabData?.x || []` said
  // the same thing for both. A null tabData is "GET_STATE has not answered yet, or Clear just emptied the
  // view" — a real state this panel renders as its empty text. A tabData that HAS no securityFindings is
  // serializeTabData broken: `mergedSecurityFindings` returns an array on every path, so there is no run in
  // which the field is missing, and defaulting it to [] renders "nothing has been reported" for a document
  // whose findings were dropped in transit — the "safe" verdict §@S forbids, arrived at by a `||`.
  if (!tabData) { container.innerHTML = ""; empty.style.display = "block"; return; }
  DCHECK(Array.isArray(tabData.securityFindings),
         "GET_STATE answered without a securityFindings array — serializeTabData builds one on every path "
         + "(lib/serialize.js -> mergedSecurityFindings), so its absence is that serializer broken and this "
         + "panel is about to report a page with dropped findings as a page with none");
  const findings = tabData.securityFindings;
  // A securitySink is one of TWO records (see the split below): a FIRED PoC — a concrete candidate driven
  // through the real code + branches + filters that BROKE OUT at the sink, self-verifying by replay — or a
  // PARKED search on a sink attacker input demonstrably reaches. Neither carries a verdict to compute: a
  // proven exploit is HIGH unless the page's own policy kills the vector (see _policyBlockers), and the
  // absence of either record is NOT "safe".
  /* THE FINGERPRINT IS OVER WHAT THE CARDS CLAIM, NOT OVER HOW MANY THERE ARE. It was
     `findings.length + ":" + secCount`, and every event this panel exists to show leaves BOTH numbers
     unchanged:
       - a PARKED search that finally SOLVES replaces one entry with another (`search`:"parked" becomes a
         `poc`), so the array is the same length and the panel kept rendering PARKED over a fire-verified
         working exploit until some unrelated sink appeared;
       - a `cspBlocks` or `trustedTypes` that arrives on a re-analysis (the policy is read from fetched headers
         and a `<meta>`, so it can land after the sink) flips a HIGH badge to POLICY-BLOCKED and moved neither
         count, so the panel went on badging a policy-dead vector HIGH — the same visible consequence as the
         recorded `cspBlocked` defect, reached through the cache in front of the fix instead of through the
         read;
       - `tried` alone is NOT the parked card's whole content, and while this key said it was, the four fields
         that carry the card's actual verdict could each move on their own with the fingerprint unchanged:
         `turns` rising from 0 is the moment "the WFQ has never scheduled this" becomes "it runs and does not
         get there", `reached` rising is "arrived", `fires` rising is "an executable program now exists", and a
         new entry in `payloads` is the one thing a reader can act on. A streamed partial that moves only those
         — which is exactly what a search making progress without seeding a new candidate looks like — landed
         on the early return below and never re-rendered. Same defect as the `cspBlocks` one above: the cache
         in front of the fix.
     So the key is built from exactly the fields a card's claim is computed from. It is not a change detector
     over rendered HTML: each name here is one the card reads. */
  const fpParts = [];
  for (let i = 0; i < findings.length; i++) {
    // lib/merge.js pushes `securitySinks: secSinks` straight from the analysis it has already DCHECKed is an
    // array, so an entry without one is that merge broken — not a source that reported no sinks (merge.js
    // does not create an entry at all in that case).
    DCHECK(Array.isArray(findings[i].securitySinks),
           "a securityFindings entry reached the popup with no securitySinks array — lib/merge.js only "
           + "creates an entry when it HAS sinks, so an entry without the array is that merge broken "
           + "(sourceUrl=" + findings[i].sourceUrl + ")");
    const sinks = findings[i].securitySinks;
    for (let j = 0; j < sinks.length; j++) {
      const s = sinks[j];
      // JSON, not a delimiter-joined string: a `poc` is an attacker payload and may hold any byte this file
      // would use as a separator, so a hand-rolled key is one the PAGE picks collisions in — and `payloads`
      // goes in WHOLE for the same reason, since it is a list of attacker payloads and any join over it is
      // that same collision one level down. `fires` is carried as itself, not as a boolean: absent (the eval
      // class, which queues nothing) and 0 (arrived, nothing executable) are the two statements the parked
      // card renders differently, and `!!s.fires` collapses them onto each other.
      fpParts.push([findings[i].sourceUrl, s.sink, s.source, s.search, s.tried, s.poc,
                    !!s.cspBlocks, !!s.trustedTypes,
                    s.reached, s.turns, s.fires === undefined ? null : s.fires, s.payloads]);
    }
  }
  const fp = JSON.stringify(fpParts);
  if (fp === _lastSecFp) return;
  _lastSecFp = fp;

  // The container is NOT cleared here. A contract DCHECK below throws where the engine's record does not
  // carry what a card claims, and clearing first would leave the panel EMPTY on that throw — an empty Vulns
  // panel reads as "nothing is here", which is the "safe" verdict solve.h forbids, arrived at by a crash
  // instead of by a claim. The clear happens where a replacement is ready (the early return, and the final
  // assignment), so a failed render leaves the LAST honest cards standing beside a loud console @WHY.

  // TWO STATES, NEVER ONE LIST. The engine reports every sink an attacker source REACHES, and a sink whose
  // breakout has not been solved carries "search":"parked" instead of a poc. Rendering both under one fired
  // heading would badge a parked search HIGH with an empty payload — a claim the engine never made. They are
  // separated here for the same reason the engine emits them apart: a fired PoC is proof, a parked search is
  // an open lead, and neither is a statement that the sink is safe.
  var allItems = [], parked = [];
  for (var fi = 0; fi < findings.length; fi++) {
    var f = findings[fi];
    var srcLabel = f.sourceUrl ? _shortUrl(f.sourceUrl) : "(unknown)";
    for (var si = 0; si < f.securitySinks.length; si++) {
      var it = f.securitySinks[si];
      // THE TWO SHAPES ARE ASSERTED ON THE LINE THAT TELLS THEM APART, because this is where the panel decides
      // which CLAIM it makes about a sink: a fired record gets a HIGH badge and a payload, a parked one gets
      // neither. `it && it.search === "parked"` guarded a null the DCHECK further down says cannot exist, and
      // the guard was not harmless — ANYTHING that is not exactly the string "parked" (a null element, a third
      // state, a record from some other producer) routed into the FIRED list, where a missing `poc` renders as
      // a HIGH XSS badge over an empty breakout input. solve_json_array emits EXACTLY ONE of `poc` and
      // `search`:"parked" per entry (solve.h states both shapes), so that is what is checked, once, here.
      DCHECK(it && typeof it === "object" && !Array.isArray(it),
             "a securitySinks element is not an object — solve_json_array emits one JSON object per detected "
             + "sink, so anything else is that serializer or the relay to this panel broken (sourceUrl="
             + f.sourceUrl + ")");
      DCHECK((typeof it.poc === "string") !== (it.search === "parked"),
             "an @S record is neither a fired PoC nor a parked search, or claims to be both — solve_json_array "
             + "emits exactly one of `poc` and `search`:\"parked\", and this is the line that decides whether "
             + "this sink is badged a working exploit or reported as an open search (sink=" + it.sink
             + " search=" + JSON.stringify(it.search) + ")");
      var e = { item: it, sourceUrl: f.sourceUrl, srcLabel: srcLabel, pageUrl: f.pageUrl };
      (it.search === "parked" ? parked : allItems).push(e);
    }
  }

  if (!allItems.length && !parked.length) { container.innerHTML = ""; empty.style.display = "block"; return; }
  empty.style.display = "none";

  // THE HEADING IS A CLAIM TOO. "Working XSS PoCs" was written over a list that includes every breakout the
  // page's own CSP or Trusted-Types policy kills on real Chrome — §@S(a)'s "never a bare XSS", made by the
  // section title rather than by a card. What the engine proved about every entry in this list is exactly one
  // thing: the candidate BROKE OUT AND FIRED in the model. Whether it runs on the real page is the per-card
  // policy verdict, so the heading says the first and counts the second.
  var blockedCount = 0;
  for (var bi = 0; bi < allItems.length; bi++) if (_policyBlockers(allItems[bi].item).length) blockedCount++;
  var html = allItems.length
    ? '<div class="section-header">Breakouts the engine FIRED <span class="badge badge-status">' + allItems.length + '</span>'
      + (blockedCount
          ? ' <span class="badge badge-medium" title="the sink is REAL and the breakout fired in the model, but the page\'s own policy kills the vector on real Chrome — each needs a policy-permitted vector">'
            + blockedCount + ' policy-blocked</span>'
          : "")
      + '</div>'
    : "";

  for (var i = 0; i < allItems.length; i++) {
    var entry = allItems[i];
    var item = entry.item;

    var srcLink = entry.sourceUrl && /^https?:\/\//i.test(entry.sourceUrl)
      ? '<a href="' + esc(entry.sourceUrl) + '" target="_blank" title="' + esc(entry.sourceUrl) + '">' + esc(entry.srcLabel) + '</a>'
      : esc(entry.srcLabel);
    if (entry.pageUrl && entry.pageUrl !== entry.sourceUrl)
      srcLink += ' <span class="page-context" title="' + esc(entry.pageUrl) + '">in ' + esc(_shortUrl(entry.pageUrl)) + '</span>';

    // THE CARD READS THE RECORD THE ENGINE ACTUALLY EMITS. `solve_json_array` (engine/host/solver/solve.c)
    // writes {sink, source, poc, firesOn, cspBlocks?, trustedTypes?, sourceEncodes?, delivery?,
    // deliveryPrefix?} for a fired sink and {sink, source, search:"parked", tried, reached, turns, survived,
    // survivedOf, escaped, fires?, payloads, survivedBy, sourceEncodes?, delivery?, deliveryPrefix?} for a
    // parked one. This card used to read `shape`,
    // `evidence`, `cspBlocked`, `cspReason` and `csp`: five names from a contract that no longer exists, so
    // every card silently dropped its source line AND its CSP verdict, and the live-verify button (gated on
    // `shape`) could not appear for any finding the engine has ever emitted. A bridge edge asserts its
    // contract, per CLAUDE.md §Architecture, so drift like that crashes where it is born instead of quietly
    // rendering less.
    //
    // THIS ENUMERATION USED TO END "— and nothing else", AND THAT WAS FALSE OF THE PARKED SHAPE FOR AS LONG AS
    // IT STOOD: `reached`, `turns`, `fires` and `payloads` crossed the whole relay verbatim and were read by
    // nothing in the tree, and this sentence is why nobody looked — it reads as authoritative and is checkable
    // by one grep, which is the stale-DFAIL failure mode sitting in the popup. A prose statement of a
    // producer's contract is a claim about ANOTHER file, so it is re-derived from that file when it is
    // touched, never repeated from memory.
    DCHECK(item.sink && item.source && item.poc,
      "an @S record reached the popup without sink/source/poc — solve_json_array emits all three for a fired "
      + "sink, so a card is about to claim a working PoC it cannot show (sink=" + item.sink + " source="
      + item.source + ")");
    // AND WHAT THE SOLVE COST, asserted rather than defaulted for the reason every other field here is: a
    // fired record always has a search behind it (solve.c CHECKs the twin at the moment the PoC is stored), so
    // an absent `searched` is this relay broken and not a cheap solve. It is the only progress number that
    // survives success — the parked shape's four all disappear the instant a search fires.
    DCHECK(typeof item.searched === "number" && item.searched > 0,
      "a fire-verified @S record reached the popup without its search cost — solve_json_array writes "
      + "`searched` on every fired entry and a PoC exists only because at least one candidate ran, so a "
      + "missing or zero count is the relay rather than a solve that cost nothing (sink=" + item.sink
      + " searched=" + JSON.stringify(item.searched) + ")");
    DCHECK(_FIRES_ON[item.firesOn],
      "an @S record reached the popup with a firesOn this view has no sentence for (" + JSON.stringify(item.firesOn)
      + ") — the token vocabulary is solve.h's, one per sink class, and a PoC whose firing semantics cannot be "
      + "stated is a payload the reader cannot reproduce (sink=" + item.sink + ")");

    // NO `|| "?"` BESIDE AN ASSERTED FIELD. The DCHECK above states that sink/source/poc are all present on a
    // fired record; a placeholder beside it is the release-build behaviour of a contract that has already
    // been declared, and it is what lets a drifted record render a card that looks complete.
    var srcHtml = '<div class="card-value" title="the attacker-controlled source whose bytes reach this sink">'
      + 'source: <code>' + esc(item.source) + '</code></div>';
    var pocHtml = '<div class="card-poc"><span class="poc-lbl">breakout input</span> <code class="poc-payload">'
      + esc(item.poc) + '</code></div>';

    // §@S(d): EVERY PoC CARRIES ITS REPRODUCTION ENVELOPE — what makes it fire, whether it is stored, and the
    // CSP/Trusted-Types state it needs — because a payload without those is not reproducible by the person
    // reading it. Every clause below is now a POSITIVE engine statement, and so is every ABSENCE: no
    // `cspBlocks` means policy_allows said yes, no `trustedTypes` means no TT requirement reaches this sink,
    // and no `delivery` means the source declared none (server-injected page state the attacker writes
    // directly) rather than one the engine forgot. The one thing this card must never do is fill a silence
    // with a plausible default — that is how it used to badge every finding HIGH.
    var blockers = _policyBlockers(item);
    var policyClause = blockers.length
      ? '<strong>the page\'s own policy blocks this vector</strong> — '
        + blockers.map(function (b) {
            return b.what === "CSP"
              ? 'CSP <code>' + esc(b.detail) + '</code>'
              : 'Trusted Types: the document requires a trusted type for the <code>' + esc(b.detail)
                + '</code> sink group, so the assignment throws before the payload is ever parsed unless a '
                + 'policy stringifies it';
          }).join(' &middot; and ')
      : 'the page CSP permits this vector, and no Trusted-Types requirement reaches this sink';
    var encClause = _encodeSentence(item);   // already HTML — see _encodeSentence
    var envHtml = '<div class="card-dims">reproduction envelope: '
      + '<strong>' + esc(_FIRES_ON[item.firesOn]
          || ('the engine reports this vector as `' + item.firesOn + '`, which this view has no sentence for'))
        + '</strong>'
      + ' · ' + policyClause
      + ' · ' + esc(_deliverySentence(item))
      + (encClause ? ' · ' + encClause : "")
      // THE COST, because a reader deciding how much to trust a finding wants to know whether the tool found
      // it or stumbled on it. One candidate run is the first written-down vector firing; a larger number is a
      // context probe, a derivation and that many re-runs of the page — which is the work no other tool does.
      + ' · ' + esc(item.searched === 1
          ? 'solved on the first candidate run'
          : 'solved after ' + item.searched + ' candidate runs of the real page')
      + '</div>';

    // ENGINE AGREEMENT verify: run the engine's EXACT poc against the REAL page in a sandboxed attacker
    // window; the sink firing apiclientsink is ground truth. WHETHER the delivery can be PERFORMED is
    // decided by the layer that performs it (startExploitProbe -> buildLiveDelivery), never re-decided
    // here. The old second predicate asked `/\{(hash|search|pm)\}/` of a record whose source reads
    // `location.hash`, so it was false for every finding and the card printed a reason ("needs a
    // client-deliverable source") that was not the real one. One decision, in the layer that owns it; its
    // own `pocWhy` is what the card reports when the mechanism cannot be performed.
    // The probe carries the engine's DECLARATION (`delivery` + `deliveryPrefix`) and no host-side source
    // taxonomy: `srcpath` and `gatefields` used to be sent here and the engine emits neither — they were
    // inputs to the deleted `{pm}` field-path builder, so they were `undefined` on every probe ever sent.
    // THE PROBE CARRIES ABSENCE AS ABSENCE. `item.delivery || ""` turned "the engine declared no delivery for
    // this source" into an empty string, which buildLiveDelivery then re-reads as the same thing through its
    // own `!delivery` — a round trip that works only because both ends happen to agree that "" is falsy. An
    // engine field the record legitimately omits is omitted here too, so the receiver's DCHECKs see the
    // record's real shape rather than this view's normalisation of it.
    var key = _findingKey(entry);
    var verifyHtml = "";
    if (entry.pageUrl) {
      var probeObj = { poc: item.poc, source: item.source, sinkName: item.sink,
                       sourceUrl: entry.sourceUrl, pageUrl: entry.pageUrl, findingId: key };
      if (item.delivery)       probeObj.delivery = item.delivery;
      if (item.deliveryPrefix) probeObj.deliveryPrefix = item.deliveryPrefix;
      if (item.cspBlocks)      probeObj.cspBlocks = item.cspBlocks;
      if (item.trustedTypes)   probeObj.trustedTypes = item.trustedTypes;
      var probe = JSON.stringify(probeObj);
      verifyHtml = '<div class="verify-row">'
        + '<button class="verify-btn" data-probe=\'' + esc(probe) + '\' data-key="' + esc(key) + '">Verify in real Chrome</button>'
        + '<span class="verify-hint">loads the real page with the engine’s EXACT payload in a sandboxed attacker window, under the page’s real CSP/Trusted-Types. A relayed <code>apiclientsink</code> call from the delivered document (origin/tab/frame browser-matched) means the payload’s code RAN there — what a fired sink produces, and what the model predicts. It is not proof the SINK produced it: the hook lives in the page’s own world.</span>'
        + '<div class="verify-result" data-key="' + esc(key) + '"></div></div>';
    } else {
      verifyHtml = '<div class="verify-na">no page url recorded for this finding — live verify delivers the payload to the page the sink was observed on</div>';
    }

    // POLICY-RELATIVE severity, over BOTH policy facts the engine emits — see _policyBlockers for why reading
    // only `cspBlocks` here was the same live defect as reading a name the engine never wrote.
    var sevBadge = blockers.length
      ? '<span class="badge badge-medium" title="' + esc('broke out and fired in the model, but the page\'s own policy ('
          + _blockerNames(blockers) + ') kills THIS vector on real Chrome — the sink is REAL and needs a '
          + 'policy-permitted vector') + '">POLICY-BLOCKED · ' + esc(_blockerNames(blockers)) + '</span>'
      : '<span class="badge badge-high">HIGH</span>';
    html += '<div class="card" data-finding-key="' + esc(key) + '">'
      + '<div class="card-label"><span class="badge badge-xss">XSS PoC</span> ' + sevBadge + ' ' + esc(item.sink) + '</div>'
      + srcHtml + pocHtml + envHtml
      + '<div class="card-meta">' + srcLink + '</div>'
      + verifyHtml
      + '</div>';
  }

  // PARKED SEARCHES — reached, searched this far, not broken out of YET. Shown because the alternative is
  // silence, and silence reads as "nothing is here" for a sink attacker input demonstrably reaches. The card
  // states what constrained the search — the bytes the source's own component percent-encodes — which is the
  // fact that tells a reader what would change the answer (an app that decodes its fragment breaks out with a
  // candidate that is parked here). It is deliberately not a severity: there is no verdict to render.
  if (parked.length) {
    html += '<div class="section-header">Reached, search parked <span class="badge badge-status">' + parked.length + '</span></div>';
    for (var pi = 0; pi < parked.length; pi++) {
      var pe = parked[pi], pit = pe.item;
      // THE FOUR PROGRESS FIELDS ARE ASSERTED BESIDE `tried`, BECAUSE THE CARD'S SENTENCE IS COMPUTED FROM ALL
      // FIVE. solve_json_array writes `reached`, `turns` and `payloads` UNCONDITIONALLY on the parked shape, so
      // an absent one is that serializer or the relay broken — and a default there would print a confident
      // "the scheduler has never given this search a turn" about a search that has run 900 of them, which is
      // the opposite instruction to the reader. `fires` is the one that is legitimately absent and is read as
      // the positive statement it is (see _parkedProgress); asserted only for its TYPE, so a name that arrives
      // as something other than a count still crashes here rather than rendering as one.
      DCHECK(pit.sink && pit.source && typeof pit.tried === "number",
        "a parked @S record reached the popup without sink/source/tried — solve_json_array emits all three, "
        + "and without `tried` the card would say '0 breakouts run' about a search that has run (sink="
        + pit.sink + " source=" + pit.source + ")");
      DCHECK(typeof pit.reached === "number" && typeof pit.turns === "number" && Array.isArray(pit.payloads),
        "a parked @S record reached the popup without reached/turns/payloads — solve_json_array emits all "
        + "three on every parked entry, so absence is that serializer or the relay to this panel broken, and "
        + "the card is about to state WHICH question this search is stuck on out of numbers it does not have "
        + "(sink=" + pit.sink + " source=" + pit.source + " reached=" + JSON.stringify(pit.reached)
        + " turns=" + JSON.stringify(pit.turns) + ")");
      // THE TWO MIDDLE RUNGS ARE ASSERTED LIKE THE OTHERS AND FOR THE SAME REASON. solve_json_array writes
      // `survived`, `survivedOf` and `escaped` unconditionally on the parked shape, and 0 is a real value each
      // of them must be able to say — so an `|| 0` here would turn "this relay dropped the field" into "the
      // page's filter ate everything", which is the confident-wrong-instruction failure this card exists to
      // end. `survivedOf:0` beside `survived:0` is one statement said once (no observation recorded), never a
      // length the engine failed to write, which is why the pair is asserted together rather than separately.
      DCHECK(typeof pit.survived === "number" && typeof pit.survivedOf === "number"
             && typeof pit.escaped === "number" && pit.survived <= pit.survivedOf,
        "a parked @S record reached the popup without survived/survivedOf/escaped, or with a surviving run "
        + "longer than the candidate it survived out of — solve_json_array emits all three on every parked "
        + "entry and the run is a substring of the candidate by construction, so the card is about to state "
        + "WHICH question this search is stuck on out of numbers that are not measurements (sink=" + pit.sink
        + " survived=" + JSON.stringify(pit.survived) + " survivedOf=" + JSON.stringify(pit.survivedOf)
        + " escaped=" + JSON.stringify(pit.escaped) + ")");
      // AND THE IMPLICATIONS BETWEEN THE FOUR RUNGS, which are the only thing that can say the four numbers
      // are about the same search rather than four counters that happen to travel together. An escape is
      // observed on a string a breakout ARRIVED in (solve.c asserts the same at its origin); and for a class
      // that queues a fire, every escape is recorded in the same block that queues one, so an escape with no
      // queued program is those two blocks having come apart.
      DCHECK(pit.escaped === 0 || pit.reached > 0,
        "a parked @S record claims a breakout reached an executable position at a sink its bytes never "
        + "arrived at — arrival and escape are read off the SAME string, so this pair was measured on two "
        + "(sink=" + pit.sink + " reached=" + pit.reached + " escaped=" + pit.escaped + ")");
      DCHECK(pit.escaped === 0 || pit.fires === undefined || pit.fires > 0,
        "a parked @S record claims an escape for a sink class that queues its fire, while nothing was ever "
        + "queued — for those classes the escape is observed in the same block that queues the program, so "
        + "one without the other means solve.c's fire oracle and its escape rung came apart (sink=" + pit.sink
        + " escaped=" + pit.escaped + " fires=" + JSON.stringify(pit.fires) + ")");
      // `survivedBy` IS ASSERTED AGAINST `payloads`, BECAUSE THE TWO ARE ONE FACT READ BY INDEX. A card that
      // lines up a run with the wrong payload states that the page ate a candidate it never ran, which is a
      // confident wrong instruction rather than a missing one — so the LENGTHS are the assertion, not the
      // presence. Each entry is bounded by the byte length of the payload it belongs to for the same reason
      // `survived <= survivedOf` is asserted above: a run is a substring of its own candidate by construction.
      DCHECK(Array.isArray(pit.survivedBy) && pit.survivedBy.length === pit.payloads.length,
        "a parked @S record's per-candidate survival does not line up with its payload list — solve_json_array "
        + "writes the two in one order and the card reads them by index, so a mismatch would attribute a "
        + "surviving run to a candidate that did not produce it (sink=" + pit.sink + " payloads="
        + pit.payloads.length + " survivedBy=" + JSON.stringify(pit.survivedBy) + ")");
      DCHECK(pit.survivedBy.every(function (n, i) {
               return typeof n === "number" && n >= 0 && n <= pit.payloads[i].length;
             }),
        "a parked @S record reports a candidate surviving more bytes than that candidate has — the run is a "
        + "substring of its own payload, so this column was measured against a different string (sink="
        + pit.sink + " survivedBy=" + JSON.stringify(pit.survivedBy) + ")");
      DCHECK(pit.fires === undefined || typeof pit.fires === "number",
        "a parked @S record carries a `fires` that is not a count — the field is emitted only for a sink class "
        + "whose breakout becomes a QUEUED program, and its absence is the statement that this class evaluates "
        + "its own argument; anything else is a third meaning this card has no sentence for (sink=" + pit.sink
        + " fires=" + JSON.stringify(pit.fires) + ")");
      var pEnc = _encodeSentence(pit);   // already HTML — see _encodeSentence
      var pSrc = pe.sourceUrl && /^https?:\/\//i.test(pe.sourceUrl)
        ? '<a href="' + esc(pe.sourceUrl) + '" target="_blank" title="' + esc(pe.sourceUrl) + '">' + esc(pe.srcLabel) + '</a>'
        : esc(pe.srcLabel);
      html += '<div class="card">'
        + '<div class="card-label"><span class="badge badge-status" title="an attacker source reaches this sink; no breakout has fired yet — this is NOT a finding that the sink is safe">PARKED</span> '
        + esc(pit.sink) + ' &larr; <code>' + esc(pit.source) + '</code></div>'
        // No `|| 0` beside `tried`: the DCHECK above states it is a number, and 0 is a real value this card
        // must be able to say (a sink reached but not yet searched) rather than one a default manufactures.
        //
        // "N breakouts run, none fired" STOOD HERE AND WAS TWO WRONG CLAIMS IN ONE LINE. `tried` is raised at
        // SEED time, so "run" was false of every search the WFQ had not yet scheduled; and "none fired" is the
        // only thing every parked entry has in common, so the sentence said nothing that the PARKED badge one
        // line above had not already said. What the reader needs is WHICH of the four states this is, because
        // they take opposite work — that is _parkedProgress, computed from the fields the engine has been
        // emitting all along.
        + '<div class="card-dims">' + esc(String(pit.tried)) + ' candidate' + (pit.tried === 1 ? "" : "s")
        + ' seeded &middot; ' + esc(_parkedProgress(pit))
        + (pEnc ? ' — ' + pEnc : "")
        + '</div>'
        // THE BYTES, because the state this search is most often in is a question about them — see
        // _payloadList for why an empty list is a statement rather than a gap.
        + _payloadList(pit)
        // The parked entry carries the SOURCE DECLARATION, not an envelope: there is no PoC yet, so there is
        // no firing vector or policy verdict to state about one. What it can say is how the attacker would
        // have to reach the victim if a candidate ever fires — which is the same declared fact the fired card
        // renders, from the same field.
        + '<div class="card-dims">' + esc(_deliverySentence(pit)) + '</div>'
        + '<div class="card-meta">' + pSrc + '</div>'
        + '</div>';
    }
  }

  container.innerHTML = html;
  container.querySelectorAll(".verify-btn").forEach(function (b) { b.addEventListener("click", function () { _handleVerify(b); }); });
}

// ── ENGINE-AGREEMENT live verify ─────────────────────────────────────────────
// The offscreen builds pocJs = the engine's EXACT poc (X9 -> apiclientsink) delivered to the REAL page via
// its source. We embed the sandboxed attacker page (poc-sandbox.html) and postMessage it the pocJs; the
// user clicks Run inside it (real user gesture for window.open). When the real sink fires, intercept.js's
// apiclientsink relays a hit keyed by the session marker.
//
// WHAT A HIT PROVES, STATED ONCE HERE BECAUSE EVERY SENTENCE BELOW IS BOUNDED BY IT. The marker rides INSIDE
// the payload — it must, since it is what a fired sink relays — and the payload is delivered TO the page, so
// the page holds it. Two consequences, and they are different sizes:
//   * CROSS-DOCUMENT: closed. The offscreen attributes each hit against the delivery it actually built —
//     browser-stated MessageSender.origin / tabId / frameId / documentId vs the address this zone navigated to
//     (offscreen-brain.js `_recordProbeHit`). A hit from any other document is REFUSED as evidence for the
//     sink and reported as what it is.
//   * SAME-DOCUMENT: IRREDUCIBLE, and no evidence the hook could carry changes that. `window.apiclientsink` is
//     installed in the page's own main world, and everything the payload does, a page script can do — there is
//     no secret to put in a payload that is handed to the page, and no "only the sink path could have done
//     this" fact, because a real `eval(location.hash)` sink fires with the page's own script on the stack,
//     which is the same shape a fabricating script has. So the hook reports the caller's frame as CONTEXT for
//     a human (offscreen `pageClaimed`), the verdict never derives strength from it, and the verdict says
//     "the payload's code RAN in the delivered document", never "the sink produced it".
// That is the strongest claim the evidence supports, and it is still the claim worth having: the payload only
// executes if the page's real CSP and Trusted-Types let it, which is exactly what the model predicted.
var _verifySandboxes = new Map();   // pocId -> {pocJs, marker, resultEl}
var _verifyIdSeq = 0;
window.addEventListener("message", function (e) {
  var d = e.data;
  if (!d || typeof d !== "object" || (d.type !== "POC_READY" && d.type !== "POC_RAN")) return;
  for (var ent of _verifySandboxes.values()) {
    var ifr = document.querySelector('iframe[data-verify-id="' + ent.pocId + '"]');
    if (!ifr || ifr.contentWindow !== e.source) continue;
    // NO try/catch AROUND THE HANDOFF. This posts a plain object to a same-origin extension frame THIS view
    // created and whose contentWindow it has just identified, so the only way it throws is a bug in that
    // identification — and swallowing it left the sandbox waiting for a POC_SETUP that never came, which
    // reads to the user as a live-verify that simply never answers.
    if (d.type === "POC_READY") { e.source.postMessage({ type: "POC_SETUP", pocJs: ent.pocJs, marker: ent.marker }, "*"); }
    else if (d.type === "POC_RAN" && ent.resultEl) {
      ent.resultEl.textContent = d.error ? "PoC threw in the sandbox: " + d.error : "payload delivered — waiting for the sink to fire in Chrome…";
      _pollVerify(ent.resultEl, ent.marker, ent.blockers);
    }
    return;
  }
});
async function _handleVerify(btn) {
  // THE PROBE IS THIS VIEW'S OWN JSON, so a parse failure here is this file disagreeing with itself — never a
  // page state. `probe = {}` on the catch built a probe with no poc and no pageUrl and sent it anyway, and
  // startExploitProbe's "need the engine's poc" throw came back as a build error about the finding rather
  // than about the attribute that failed to round-trip.
  DCHECK(typeof btn.dataset.probe === "string" && btn.dataset.probe.length > 0,
         "a verify button carries no data-probe — renderSecurityPanel writes one on every button it creates, "
         + "so an empty one is that attribute failing to survive the innerHTML round trip");
  var probe = JSON.parse(btn.dataset.probe);
  var card = btn.closest(".card");
  var resultEl = card ? card.querySelector(".verify-result") : null;
  if (!resultEl) return;
  btn.disabled = true; var prev = btn.textContent; btn.textContent = "Building…"; resultEl.textContent = "Building the delivery from the engine’s poc…";
  try {
    var start = await new Promise(function (res) { chrome.runtime.sendMessage(Object.assign({ type: "EXPLOIT_PROBE_START", waitMs: 6000 }, probe), function (r) { res(r); }); });
    // NO pocJs IS AN ANSWER, AND `pocWhy` IS THAT ANSWER. The delivery layer states which engine-declared
    // mechanism it cannot perform (a planted cookie, an attacker-served referrer, a user-supplied file); the
    // finding itself stands — the engine fire-verified the breakout. Reporting only "no pocJs" read as a
    // broken build, which is a different claim from "this vector is not deliverable from a sandbox".
    if (!start || start.error || !start.pocJs) {
      resultEl.textContent = "not deliverable from this sandbox: "
        + ((start && (start.error || start.pocWhy)) || "the offscreen returned no PoC and no reason — an engine↔host contract gap");
      btn.disabled = false; btn.textContent = prev; return;
    }
    // THE MARKER IS THE WHOLE CORRELATION, so it is asserted and never allowed to arrive as `undefined`. It
    // rides INSIDE the payload as apiclientsink('<id>') and is the only thing that ties a real Chrome hit back
    // to this session, so an absent one keys _verifySandboxes under `undefined` and then polls a session id the
    // offscreen has never held — every live verify would report NOT REPRODUCED whatever Chrome actually did,
    // which is the exact consequence the deleted `snap.executed` read used to have. `pocJs` above was already
    // checked; the id it is useless without was not.
    DCHECK(typeof start.sessionId === "string" && start.sessionId.length > 0,
           "EXPLOIT_PROBE_START answered with a pocJs but no sessionId — startExploitProbe mints a "
           + "crypto.randomUUID marker on every session and popup-handlers answers it as `sessionId`, so its "
           + "absence is that reply broken and this verify could never be correlated to a real Chrome hit");
    var pocId = "v" + (_verifyIdSeq++);
    // The probe carries the engine's policy facts (present only when the engine stated them), so the ONE
    // _policyBlockers reading serves the card and the verify verdict alike. Two spellings of "did the page's
    // policy kill this" is how the badge and the envelope came to disagree in the first place.
    _verifySandboxes.set(start.sessionId, { pocId: pocId, pocJs: start.pocJs, marker: start.sessionId,
                                            resultEl: resultEl, blockers: _policyBlockers(probe) });
    var ifr = document.createElement("iframe");
    ifr.setAttribute("data-verify-id", pocId);
    ifr.src = "poc-sandbox.html";
    ifr.style.cssText = "width:100%;height:120px;border:1px solid var(--border,#444);border-radius:6px;margin-top:6px;";
    resultEl.textContent = "click Run PoC inside the sandbox below (your click is the user gesture window.open needs):";
    resultEl.parentNode.appendChild(ifr);
    btn.textContent = prev; btn.disabled = false;
  } catch (err) {
    RETHROW_FATAL(err);   // an invariant abort is never reported as a verify that went wrong
    resultEl.textContent = "verify error: " + (err && err.message || err); btn.disabled = false; btn.textContent = prev;
  }
}
// The page's own account of the caller — CONTEXT for a human, never a component of the verdict. intercept.js
// reads it inside the untrusted main world, so it is prefixed as a claim and is the only place `pageClaimed`
// is rendered. `unavailable` is a real state (that document refused to describe its own caller) and prints as
// one rather than folding into "nothing to show" — the same reason the offscreen records it instead of
// defaulting it away.
function _hitFrame(pc) {
  DCHECK(!!pc && typeof pc === "object",
         "a probe hit reached the panel with no pageClaimed half — _recordProbeHit writes both halves on every "
         + "hit it pushes, and the split is what keeps the untrusted renderer's claims from being rendered "
         + "beside the browser's facts with nothing marking which is which");
  if (pc.unavailable) return " The page’s world refused to describe the caller (Error/currentScript poisoned) — itself a signal.";
  var bits = [];
  if (pc.eventType) bits.push("called inside a ‘" + pc.eventType + "’ handler"
                              + (pc.eventTarget ? " on <" + pc.eventTarget.toLowerCase() + ">" : ""));
  bits.push(pc.currentScript ? "document.currentScript = " + pc.currentScript : "no script element was executing");
  if (!pc.addressMatch) bits.push("that document was no longer at the delivered address");
  return " Page-CLAIMED caller context (read in the page’s own world — context, not evidence): " + bits.join("; ") + ".";
}
function _refusedReasons(refused) {
  var seen = [];
  refused.forEach(function (h) { if (h.mismatch && seen.indexOf(h.mismatch) < 0) seen.push(h.mismatch); });
  return seen.join("; ");
}
async function _pollVerify(resultEl, marker, blockers) {
  var refused = [];
  for (var i = 0; i < 20; i++) {
    await new Promise(function (r) { setTimeout(r, 400); });
    var snap = await new Promise(function (res) { chrome.runtime.sendMessage({ type: "EXPLOIT_PROBE_STATUS", sessionId: marker }, function (r) { res(r); }); });
    // A session that has expired out of the offscreen's LRU answers {error}; that is a real state and the
    // loop keeps polling the remaining attempts rather than claiming anything about the sink.
    if (!snap || snap.error) continue;
    // `hits` IS THE ONLY EVIDENCE, and `executed` NEVER EXISTED. This line read a second vocabulary —
    // `snap.executed`, an object of per-payload booleans — as a fallback source of "did it fire". Nothing in
    // this extension has ever WRITTEN it: startExploitProbe builds the session with {marker, status, pageUrl,
    // findingId, sourceUrl, sinkName, waitMs, hits, createdAt, finishedAt, error} and PROBE_HIT only ever
    // pushes onto `hits`, so the reply's own producer wrote `executed: ses.executed || null` — a default over
    // a field with no writer, which is what kept a dead vocabulary looking live. It is the `canVerify`/`shape`
    // defect exactly: a reader whose writer does not exist, hidden by a default. Deleted on both sides, and
    // `hits` is asserted rather than defaulted so the day EXPLOIT_PROBE_STATUS stops carrying it this crashes
    // instead of reporting NOT REPRODUCED for every finding forever.
    DCHECK(Array.isArray(snap.hits),
           "EXPLOIT_PROBE_STATUS answered without a hits array — the probe session is created with hits:[] "
           + "and PROBE_HIT only appends to it, so its absence is that reply broken and every live verify "
           + "would report NOT REPRODUCED no matter what real Chrome did");
    /* A HIT IS EVIDENCE ONLY IF IT CAME FROM THE DELIVERED DOCUMENT, so the array is PARTITIONED before it is
       read as an outcome. `snap.hits.length` alone was the whole test, which is why any document in any tab
       that knew the marker could print the strongest verdict this panel has. Attribution is decided in the
       trusted zone against browser-stated facts (offscreen `_recordProbeHit`); this view only reports it. */
    var fired = null;
    refused = [];
    for (var h of snap.hits) {
      DCHECK(typeof h.attributed === "boolean",
             "a probe hit reached the panel with no attribution verdict — _recordProbeHit decides `attributed` "
             + "on every hit it pushes, so an absent one is this view counting a call it has not established "
             + "came from the document the payload was delivered to");
      if (h.attributed) { if (!fired) fired = h; } else refused.push(h);
    }
    if (fired) {
      resultEl.className = "verify-result verify-hit";
      // TWO CLAIMS, KEPT APART. (1) The MODEL: a fire the engine predicted would be blocked is a DIVERGENCE,
      // not an agreement — saying it agreed would hide a wrong policy read behind the best-looking result the
      // panel can print. (2) The EVIDENCE: what a hit establishes is that the payload's CODE RAN in the
      // delivered document, which is what a fired sink produces and is not the same statement as "the sink
      // produced it" — see the section comment above for why no evidence the hook can carry closes that gap.
      resultEl.textContent =
        "FIRED IN THE DELIVERED DOCUMENT — apiclientsink was called from the document the payload was "
        + "navigated to (browser-stated origin, tab and frame all match the delivery), so the payload’s code "
        + "RAN in real Chrome under the page’s own CSP and Trusted-Types. "
        + (blockers.length
            ? "But the engine predicted this vector was dead (" + _blockerNames(blockers) + "), so its policy "
              + "read DIVERGES from Chrome here: the model that called it blocked is wrong (an engine-fidelity "
              + "bug to investigate). "
            : "That is what a fired sink produces, and the engine’s model agrees with Chrome. ")
        + "NOT proof the SINK produced the call: apiclientsink is installed in the page’s own world, so any "
        + "script in that document can call it. Cross-document fabrication is refused; the same-document case "
        + "is irreducible by this mechanism."
        + _hitFrame(fired.pageClaimed)
        + (refused.length ? " Also refused: " + refused.length + " call(s) with this marker from elsewhere ("
                            + _refusedReasons(refused) + ")." : "");
      return;
    }
  }
  if (refused.length) {
    // §@S: absence of a PoC is never a "safe" verdict, and a marker surfacing where it was never delivered is
    // itself a fact about the page — so this is REFUSED (not evidence for this sink), never dropped and never
    // counted as a fire.
    resultEl.className = "verify-result verify-miss";
    resultEl.textContent =
      "REFUSED — apiclientsink fired with this session’s marker " + refused.length + " time(s), and every one "
      + "came from a document the payload was never delivered to (" + _refusedReasons(refused) + "). The marker "
      + "rides inside the payload, so any document that can read the delivered address holds it; a call from "
      + "elsewhere is another document’s claim about this sink, not evidence for it. It IS a fact about the "
      + "page — the marker reached a document that was never handed one. The sink stays REAL and this vector "
      + "stays unverified; NOT a statement that the sink is safe."
      + (blockers.length
          ? " No call came from the delivered document, which is consistent with the engine’s prediction that "
            + "the page’s own policy kills this vector (" + _blockerNames(blockers) + ")."
          : "");
    return;
  }
  resultEl.className = "verify-result verify-miss";
  // POLICY-RELATIVE no-fire: when the engine already flagged the page's own policy — a CSP, a Trusted-Types
  // requirement, or both — as killing THIS vector, a non-fire is the EXPECTED, confirmed outcome (real sink,
  // dead vector), not an engine-fidelity divergence to chase. The last arm does not OFFER "CSP/Trusted-Types"
  // as a possibility: the engine answered both questions on the record, so an empty blocker list means both
  // are a No and the remaining explanation is a divergence. A no-fire is NEVER "safe" either — the sink stays
  // REAL and the search stays open.
  resultEl.textContent = blockers.length
    ? "BLOCKED AS PREDICTED (" + _blockerNames(blockers) + ") — apiclientsink never fired, and the engine said "
      + "this vector is dead on the real page: "
      + blockers.map(function (b) {
          return b.what === "CSP"
            ? "the page CSP blocks it (" + b.detail + ")"
            : "the document requires a trusted type for the '" + b.detail + "' sink group, so the assignment "
              + "throws before the payload is parsed";
        }).join("; and ")
      + ". The sink is REAL; it needs a policy-permitted vector. A policy-relative result, NOT an "
      + "engine-fidelity bug — and NOT a statement that the sink is safe."
    : "NOT REPRODUCED — apiclientsink never fired, and the engine reported neither a blocking CSP nor a Trusted-Types requirement for this vector, so the engine’s model diverges from Chrome here (an engine-fidelity bug to investigate). Not a statement that the sink is safe.";
}
