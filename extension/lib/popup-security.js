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
  // HTML §9.3.3 "Posting messages". No navigation and no plant: the attacker holds a SECOND DOCUMENT open
  // beside the victim — the victim in an iframe on the attacker's page, or opened as a popup — and posts to it
  // while it runs. The bytes are not transformed on the way in (§9.3.3 step 7's StructuredSerializeWithTransfer
  // and step 8.4's deserialize round-trip a string unchanged), which is why these reproduce where the same
  // candidate through a fragment does not.
  "cross-document-message":
                       function ()  { return "the attacker keeps the victim open in a document of their own (an iframe, or a popup) and postMessage()s the payload to it while it runs — no navigation, and nothing transforms the bytes on the way in"; },
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
  // AND THE DECLARATION IS A PRIOR, NOT AN OUTCOME — solve.h says so at `sourceDelivers` in those words, and
  // this sentence stated the prior AS the outcome ("so a candidate needing those bytes arrives escaped") on
  // every card that carried one. On a FIRED card that is a flat contradiction of the finding printed two lines
  // above it. Measured: an `innerHTML` sink fed `decodeURIComponent(location.hash.slice(1))` emits
  // poc `<svg onload=X9()>` with sourceEncodes `" \"<>`"` AND sourceDelivers the SAME set — the page's own
  // decode hands back every byte the browser encoded — and the envelope told the reader those bytes "arrive
  // escaped" beside a PoC built out of them. Confirmed on real Chrome through the declared delivery: the
  // fragment arrives as `#%3Csvg%20onload%3DX9()%3E`, the page decodes it, and the handler runs.
  // THE TWO ARE HELD AGAINST EACH OTHER, which is the whole reason the engine emits both. `sourceDelivers` is
  // the MEASURED subset that reached a sink; its ABSENCE is NOT a licence to report the prior as a result — so
  // with no measurement the sentence says what the browser does and stops there.
  //
  // …AND THAT ABSENCE IS THREE FACTS, NOT THE TWO THIS SAID. solve.h says so at `sourceDelivers` in exactly
  // those words — "ABSENT IS THREE FACTS AND IS READ WITH `deliveryProbed` BELOW OR NOT AT ALL, and this entry
  // used to say TWO and name the wrong pair" — and this file carried the same wrong pair on the consumer side:
  // the source declares no percent-encode set (handled above, where a non-string `sourceEncodes` returns
  // early), no delivery probe of this search has reached a sink YET, or one HAS and the page destroyed every
  // token it was built out of. The third was the one with no name here, and it is the LOUDEST reading this
  // instrument takes: `deliv_seen` is raised per TOKEN FOUND (solve.c), so a probe the page ATE clears no byte
  // and emits no `sourceDelivers` at all — arriving at this branch byte-identically with a search the
  // scheduler has never run. The card printed "unmeasured here" over a finished answer, which is the exact
  // inversion §@S forbids: a rung whose ABSENCE and whose ZERO read alike, and the two take opposite work.
  // `deliveryProbed` is what splits them, counted at the ARRIVAL before a single token is looked for.
  //
  // ITS OWN ABSENCE IS READ AS A FACT ABOUT THE RECORD AND NOT ABOUT THE SEARCH, WHICH IS A SEAM DECISION AND
  // IS DELIBERATE. solve.c emits `deliveryProbed` only where the search HOLDS a delivery probe
  // (cand_has_delivery_probe), so on a current engine its absence is positive: a single-context class states
  // its vectors at detection and runs none, and a derived search over server-injected page state has no byte
  // in question. But the SHIPPED wasm predates that producer — `deliveryProbed` occurs zero times in
  // extension/lib/qjs/qjs.wasm while `sourceDelivers` occurs once — so today every absent-`sourceDelivers`
  // record arrives here with no arrival count at all, and a consumer cannot tell "this search holds no probe"
  // from "this engine does not speak the field". Those are two facts under one absence, so this states
  // NEITHER as a fact about the search: it reports what the RECORD carries, which is true under both engines
  // and is the strongest claim this side is entitled to. It must not say "yet" here — that word fabricates a
  // pending measurement for a search that may hold nothing to measure with.
  var enc = "the browser percent-encodes <code>" + esc(item.sourceEncodes) + "</code> in this source";
  if (typeof item.sourceDelivers !== "string") {
    if (item.deliveryProbed > 0)
      return enc + ", and a delivery probe of this search REACHED a sink " + item.deliveryProbed + " time"
           + (item.deliveryProbed === 1 ? "" : "s") + " and <strong>not one</strong> of its tokens was in "
           + "what arrived — the page destroyed the instrument itself, so the source's own transform is the "
           + "whole of the failure and no re-derivation reaches past it. A finished answer, not a distance";
    if (item.deliveryProbed === 0)
      return enc + ", so a candidate needs those bytes to survive the way in — this search HOLDS a delivery "
           + "probe and it has not reached a sink yet, so whether they do is unmeasured and this is a "
           + "scheduling state, not an answer about the source";
    return enc + ", so a candidate needs those bytes to survive the way in — this record states no delivery "
         + "probe arrival at all, so whether they do is unmeasured here and nothing on it says a probe was "
         + "ever run";
  }
  if (item.sourceDelivers === "")
    return enc + " and a run MEASURED that <strong>none</strong> of them arrives — the source's own transform "
         + "is the whole of the failure, and no re-derivation reaches past it";
  if (item.sourceDelivers === item.sourceEncodes)
    return enc + ", and a run MEASURED that <strong>all</strong> of them arrive anyway — the page performs the "
         + "round trip that undoes it, so the encode set defeats nothing here";
  return enc + ", and a run MEASURED <code>" + esc(item.sourceDelivers) + "</code> of them arriving anyway — "
       + "the page undoes part of the transform, and an escape is constructed out of exactly that subset";
}

// WHICH QUESTION A PARKED SEARCH IS STUCK ON, which is the whole content of the parked card and which one
// number could never say. `solve.h` emits four fields for exactly this and the panel rendered none of them,
// so two states needing OPPOSITE work printed byte-identically:
//   * `turns:0` — the WFQ has never once given this search the thread. Nothing has RUN, so "N breakouts run"
//     was itself false: `tried` is raised where a candidate is SEEDED (solve_seed_candidates), `turns` where
//     one is switched in (solve_flow_begin). This is a scheduling question.
//   * `turns:N, substituted:0` — the candidates have held the thread and NOT ONE of them reached its own
//     SOURCE READ, so their bytes never entered the page's program at all. A question about the PATH in front
//     of the source: a gate is turning these flows away. Nothing here is about the payload, the filter or the
//     sink, and no field could say it before — every other number on the record is observed AT a sink.
//   * `substituted:D, sinkStrings:0` — their bytes ARE in the page's program and no code-execution sink has
//     run at all while they were live. A distance-through-the-document question, and the opposite action.
//   * `sinkStrings:X, survived:0` — X sinks EXECUTED and not one byte of the candidate was in any string they
//     were handed. A question about the PAYLOAD's own transform or routing, and the opposite instruction to
//     the line above it.
// THOSE THREE WERE ONE SENTENCE, and it was the middle one said for all of them: "the flows run and do not
// get this far through the document". It is a confident wrong instruction for the first (the flows did not
// get as far as the READ, which is a different distance and a different fix) and for the third (the flows DID
// get that far; the bytes did not). §@S's tell exactly — a rung whose ABSENCE and whose ZERO read alike —
// which is what `survived:0` was, because a best-so-far ratchet is silent about how many times it looked.
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
  // …AND WHICH SEGMENT THAT WAS, WHICH IS THE HALF A SIZE CANNOT SAY AND THE HALF A MUTATION ACTS ON.
  // `survivedAt` is the offset into the CANDIDATE at which the longest surviving run begins (solve.h), so the
  // two readings take OPPOSITE work: a run beginning at byte 0 is a payload whose TAIL broke — the escape
  // opened and its terminator never arrived — and one beginning later is a payload with no longer contiguous
  // run ahead of it, so what broke is the HEAD and the escape never opened at all. Same gap, same sentence,
  // until this line. `survivedTo` says where that segment landed in the string the sink was handed, which is
  // what distinguishes a run that arrived at the front of a large template from one buried inside it.
  // ABSENT IS NOT ZERO AND IS NOT DEFAULTED HERE: solve.c omits the pair exactly when no run was recorded, the
  // biconditional is asserted where the record arrives, and the `survived === 0` branches below never reach
  // this string. Nothing is claimed about bytes OUTSIDE the run — a shorter run of them may have survived
  // elsewhere — so the sentence states what the longest run says and no more.
  var seg = item.survived === 0 ? ""
    : item.survivedAt === 0
      ? (item.survived === item.survivedOf
         ? ' (all of it, contiguous, landing at byte ' + item.survivedTo + ' of what the sink was handed)'
         : ' — its leading ' + item.survived + ', landing at byte ' + item.survivedTo + ' of what the sink was '
           + 'handed, so what broke is the TAIL')
      : ' — the segment beginning at byte ' + item.survivedAt + ' of the candidate, landing at byte '
        + item.survivedTo + ' of what the sink was handed, with no longer contiguous run ahead of it, so what '
        + 'broke is the HEAD';
  var got  = item.survived + ' of ' + item.survivedOf + ' bytes of the furthest candidate' + seg;
  // HOW MANY OF THIS SEARCH'S CANDIDATES HAVE NEVER BEEN SEEN AT A SINK — counted, never indexed. solve.h
  // says the inert context probe is entry 0 of a DERIVED class and that it is told apart by carrying no
  // marker; deciding WHICH entry is the probe from its POSITION would be this view restating a producer fact
  // it cannot check, which is the drift these cards exist to end. A count needs no such claim and answers the
  // question anyway: with `survived` saturated at a full-length run, "and N of its M candidates have never
  // been seen at a sink" is what says the run belongs to one candidate and the others never travelled.
  // …AND A ZERO THERE IS TWO OPPOSITE FACTS UNTIL `withdrawn` SPLITS THEM, which is the same producer-states-it
  // rule one paragraph up. A candidate the search WITHDREW was never seeded at all — the delivery table it was
  // constructed under narrowed afterwards and its bytes are now known not to arrive — so its `survivedBy` entry
  // is 0 for a reason that has nothing to do with distance, while the branch below tells the reader a
  // never-seen candidate "has not re-traversed the document yet". That is the wrong instruction: nothing is
  // going to re-traverse anything, and what the page needs is a decode or another source. So the two are
  // counted apart and only the SENT ones are called unseen.
  // The column's presence and its length are asserted once, in the parked block that calls this — restating
  // them here would be the same contract in two places, free to disagree.
  var withdrawn = 0, unseen = 0;
  item.survivedBy.forEach(function (n, i) {
    if (item.withdrawn[i]) withdrawn++;
    else if (n === 0) unseen++;
  });
  var alsoUnseen = (unseen
    ? ', and ' + unseen + ' of its ' + item.survivedBy.length + ' candidate'
      + (item.survivedBy.length === 1 ? "" : "s") + ' ' + (unseen === 1 ? 'has' : 'have')
      + ' never been seen at a sink at all'
    : "")
    + (withdrawn
    ? ', and ' + withdrawn + ' more ' + (withdrawn === 1 ? 'was' : 'were') + ' WITHDRAWN before running — the '
      + 'search measured that the bytes ' + (withdrawn === 1 ? 'it needs' : 'they need') + ' do not survive '
      + 'delivery through this source, so ' + (withdrawn === 1 ? 'it was' : 'they were') + ' never sent'
    : "");

  // …AND A CANDIDATE THIS BUILD REFUSED AT THE DOOR IS NOT A CANDIDATE THE SCHEDULER HAS NOT REACHED, WHICH IS
  // THE STATE `turns:0,tried:0` COULD NOT SAY. A parked record carries bytes an EARLIER session derived, and
  // solve_resume_candidate seeds this search's delivery table from the root's carrier declaration the moment
  // the root arrives — so a record written before that narrowing names a payload this build positively
  // contradicts, and it is withdrawn at the door. solve.h is explicit that NEITHER `tried` NOR `resumed` moves
  // for it, because both count candidate RUNS and a withdrawn record has none. Without this field a search
  // whose every parked candidate was refused reports `tried:0,resumed:0,turns:0` byte-identically to a search
  // nothing was ever parked for, and the branch below states the SECOND for both — "seeded and queued and
  // nothing has run yet, so this is a scheduling state and not a search that failed" — which is a confident
  // wrong instruction for the first: nothing is queued, nothing is coming, and what happened is that this
  // build's carrier rules have moved past a stored recipe. The two take opposite work, so they are told apart
  // before the sentence that is wrong about one of them.
  // COUNTED AND NEVER SUBTRACTED FROM ANYTHING. It is disjoint from `tried` and from `resumed` by the producer's
  // own statement, so there is no arithmetic here to perform and none is attempted; the recipe outlives the
  // bytes (solve.h), so the path itself comes back as an ordinary exploration flow that re-opens the search
  // against the narrowed table, and this sentence says so rather than reporting a loss.
  var refused = item.resumedWithdrawn === 0 ? ""
    : ', and ' + item.resumedWithdrawn + ' parked candidate'
      + (item.resumedWithdrawn === 1 ? ' was' : 's were') + ' WITHDRAWN AT THE DOOR when '
      + (item.resumedWithdrawn === 1 ? 'it' : 'they') + ' came back from a previous session — this build\'s '
      + 'delivery table has narrowed past the recipe that wrote ' + (item.resumedWithdrawn === 1 ? 'it' : 'them')
      + ', so ' + (item.resumedWithdrawn === 1 ? 'its bytes are' : 'their bytes are') + ' known not to arrive '
      + 'through this source and ' + (item.resumedWithdrawn === 1 ? 'it' : 'they') + ' never ran. The path is '
      + 'not lost: it comes back as an ordinary flow that re-opens this search against the narrowed table';
  // FOLDED INTO `held` SO IT REACHES EVERY BRANCH BELOW AND NOT ONLY THE ONE THAT IS WRONG ABOUT IT. A
  // withdrawal is a fact about the SEARCH, not about the state the card happens to land in, and every sentence
  // under this point accounts for what ran out of `tried` and `payloads` — neither of which a withdrawn record
  // is in. Reporting it only where `turns === 0` would leave a search that ran ten turns and refused four
  // stored candidates describing its distance out of an account with a hole in it.
  held += refused;
  if (item.turns === 0)
    return (item.resumedWithdrawn > 0
            ? 'nothing has run for this search' + refused + (item.tried > 0
               ? ', and its ' + item.tried + ' other candidate' + (item.tried === 1 ? ' is' : 's are')
                 + ' seeded and queued'
               : ' — and it holds no other candidate at all, so this is NOT a scheduling state and no turn '
                 + 'will change it')
            : 'the scheduler has not yet given this search a turn — its ' + item.tried + ' candidate'
              + (item.tried === 1 ? " is" : "s are") + ' seeded and queued and NOTHING has run yet, so this is '
              + 'a scheduling state and not a search that failed');
  // THE RUNWAY STATE, CHECKED BEFORE EVERY SINK ONE BECAUSE ALL OF THEM ARE WRONG ABOUT IT — INCLUDING THE
  // PROBE BRANCH BELOW. `substituted` is raised where the page's own source read hands back the candidate's
  // bytes (solve.h), so its site is the lowest one in the page's own program. A zero here is
  // not a thinner reading of the distance question further down, it is a DIFFERENT question — about the path
  // in front of the SOURCE — and every sentence below names the wrong distance for it. It also subsumes the
  // probe branch's own `witnessed === 0` arm rather than competing with it: a context is READ off a string a
  // real run handed the sink, so a search that has never substituted cannot have witnessed one, and the arm
  // that would otherwise fire says "the probes have not got this far through the document" when what happened
  // is that they never got as far as the read.
  // THE RUNWAY SPLITS THAT SENTENCE IN TWO, AND THE TWO TAKE OPPOSITE WORK. `runwayPerMille` is the report's
  // copy of the ladder rung BELOW the delivery (flow.h's `cand_replay`, sampled by solve.c's observe_runway at
  // every switch-in and once more at the flow's end, ratcheted over the search), and it is the only number on
  // this record whose observation site is IN FRONT of the source read — every other one is at it or past it,
  // which is §@S(i)'s requirement that a rung be observed strictly before the thing it is a distance to.
  //   0     — the candidates were given the thread and consumed NONE of their own recorded path. The replay is
  //           being turned back at its FIRST ARM, so nothing here is a distance at all and the work is to find
  //           what refuses that arm.
  //   1000  — SATURATED, which is not "nearly at the read". solve.c's own declaration is explicit that what
  //           this is a fraction of is DECISION ARMS and not statements, so a candidate whose recorded path is
  //           short saturates here while still far from its source read in program order: the bottom rung is
  //           spent and the remaining runway is UNMEASURED BY IT. A card that said "almost there" would be
  //           restating the comparator's arithmetic as a claim about the page's program.
  //   middle — the replay is consuming its path and stopping partway, which is the distance question this
  //           branch has always been read as, and the only one of the three it was ever right about.
  // IT UNDER-READS AND NEVER OVER-READS, which is solve.c's own named residual (a candidate that walks part of
  // its runway and is then parked contributes only what its last switch-in saw), so a low number is read as a
  // floor and never as a measured stop. That asymmetry is why the 0 arm speaks about the FIRST ARM — the one
  // reading no under-sampling can manufacture — rather than about a distance.
  // ITS ABSENCE WAS THE DEFECT THIS REPLACES, AND THE DEFERRAL THAT STOOD HERE WAS TRUE WHEN IT WAS WRITTEN.
  // It said a reader here would speak a protocol the shipped wasm does not implement (§A-CROSS-BOUNDARY-DIFF),
  // and that was exactly right on the day: the trusted zone's JS is interpreted from the tree and deploys on
  // WRITE while the C is live only after a build. What retired it is not an argument, it is the build — the
  // clause is a claim about an ARTIFACT and it went stale the moment one was produced. Checked the way the
  // `deliveryProbed` note below checks the same thing, by CONTENT and never by timestamp: `runwayPerMille`
  // occurs in extension/lib/qjs/qjs.wasm, beside `survivedOf` which this card already reads and unlike a name
  // no producer writes, so both halves are live and the deferral has nothing left to defer.
  if (item.substituted === 0)
    return held + ' and NOT ONE of them reached its own SOURCE READ — the substitution is performed where the '
         + 'page reads the attacker source, and it has never once happened for this search, so these bytes '
         + 'have never been in the page\'s program at all. That is a question about the PATH in front of the '
         + 'source, and the runway says WHICH question: '
         + (item.runwayPerMille === 0
            ? 'not one of these candidates consumed a single arm of its own recorded path, so the replay is '
              + 'being turned back at its FIRST ARM — this is not a distance through the document at all, and '
              + 'the work is to find what refuses that arm'
            : item.runwayPerMille >= 1000
              ? 'they consumed the WHOLE of their recorded path and the source read is still ahead of them. '
                + 'That rung is SATURATED and is directing nothing further — and it is a fraction of decision '
                + 'ARMS, not of the page\'s program, so this says the ladder has run out of runway to measure '
                + 'and NOT that the read is close'
              : 'they consumed ' + (item.runwayPerMille / 10) + '% of their own recorded path and stopped, so '
                + 'something in front of the source is turning these flows away partway through the replay')
         + '. Nothing here is about the payload, the page\'s own filter or the sink';
  // THE STATE THE OTHER FOUR CANNOT SAY, AND IT IS CHECKED BEFORE THEM BECAUSE THEY ARE WRONG ABOUT IT. When
  // every candidate this search has run is an inert probe, no breakout has been CONSTRUCTED — so `reached:0`
  // is not a distance question and not a filter question, and the two branches below state both of those as
  // if they were the only possibilities. Measured on an `innerHTML` sink fed the RAW fragment: `turns:2,
  // reached:0, survived:14, survivedOf:14`, and the card said the bytes "were cut down by the page's own
  // FILTER" or had "not re-traversed the document yet". Neither happened. The fragment percent-encode set
  // (URL §1.3 "Percent-encoded bytes") holds SPACE, `"`, `<`, `>` and backtick, the delivery probe MEASURED
  // that none of them arrives, and the derivation therefore built nothing to send — which is the search
  // working, not failing, and the one reading that tells the user this page needs a DECODE in it before any
  // markup breakout is possible at all. `probes` is the producer's own count (solve.h); it is never derived
  // here from position, because the marker vocabulary that tells a probe apart is the engine's.
  //
  // …AND THAT BRANCH WAS ITSELF TWO OPPOSITE STATES UNDER ONE SENTENCE, WHICH IS THE DEFECT IT WAS ADDED TO
  // END, COMMITTED ONE LEVEL DOWN. `payloads == probes` says a derivation built nothing; it does NOT say the
  // derivation ever RAN. A derived class reads its breakout off the string a REAL run handed the sink, so
  // with no such string there is nothing to read — solve.c's derive_from_witness asserts exactly that
  // (`nwit > 0`) and is the only route to queue_derived, so `witnessed:0` means NO DERIVATION HAS HAPPENED
  // and `witnessed:N` means N contexts were read and none of them could be left. Those take opposite work:
  // the first is the same distance-through-the-document question `turns` and `survived` are asked for, the
  // second is the joint solve's CORRECT and final answer for a percent-encoded source. The sentence below
  // stated the SECOND for both, so a search whose probe had simply not got there yet was told the page
  // "needs a decode, or another source" — a confident wrong instruction, and the exact tell §@S names: a rung
  // whose ABSENCE and whose ZERO read alike.
  // `witnessed` IS THE ENGINE'S OWN OBSERVATION AND IT HAD NO READER. solve.c counts it (`nwit`, deduped by
  // the witness text so two writes of one template are one context), emits it on every parked entry of a
  // deriving class, and nothing in the shipped path read it — §@S: an observation with a computed writer and
  // no reader is not a mechanism. It is read here, never re-derived: a view cannot tell from `payloads`
  // whether a probe arrived, because arrival is measured on a string the popup never sees.
  // ABSENT IS NOT ZERO AND IS NOT A CASE HERE. solve.c emits `witnessed` only for a class that DERIVES, and
  // only such a class has probes at all (its `nprobe` counts the leading probe entries; a single-context
  // class states its vectors at detection and has `probes:0`), so this branch — which requires `probes > 0` —
  // is reached only where the field is present. The parked block asserts that biconditional rather than
  // letting a `=== 0` here silently answer for a missing field.
  // …AND THE CROSS-SESSION RUN IS ANSWERED BEFORE THE PROBES-ONLY BRANCH, because that branch's whole sentence
  // is false of it. "Every one of them was an inert PROBE, so there is nothing that could arrive" is a claim
  // about the runs and reads them off the LIST, and a candidate rebuilt out of the cold tier carries its
  // payload on the resumed FLOW and is in no row — so the branch below would tell the reader nothing could
  // arrive about a search whose furthest run is a real breakout that did. It is also the strongest thing a
  // parked @S entry can say: this search is older than this session, its bytes were derived and parked
  // somewhere else, and they came back and were re-run against today's page.
  if (item.resumed > 0 && item.payloads.length === item.probes)
    return held + ', and ' + item.resumed + ' of its ' + item.tried + ' run'
         + (item.tried === 1 ? "" : "s") + ' came back from a PREVIOUS SESSION — those candidates were derived '
         + 'and parked before this run, and their bytes ride the resumed flow rather than this session\'s '
         + 'list, which is why nothing below is an attack of this session\'s. '
         + (item.reached > 0
            ? 'They reached this sink ' + item.reached + ' time' + (item.reached === 1 ? "" : "s")
              + ' and did not fire, so this is a question about the BYTES: a breakout an earlier session built '
              + 'arrives at today\'s page and does not break out of it'
            : 'They have not reached this sink yet, so this is a distance question through today\'s document '
              + 'and not one about the payload');
  if (item.payloads.length && item.payloads.length === item.probes) {
    if (item.witnessed === 0)
      return held + ' and every one of them was an inert PROBE whose bytes have NOT yet been seen at this '
           + 'sink — a breakout for this class is READ off the string a real run hands the sink, so with no '
           + 'context read there has been nothing to derive one from and the derivation has never run. That '
           + 'is a distance question, the same one `turns` and `survived` are asked for: the probes have not '
           + 'got this far through the document yet, and nothing here says anything about whether this '
           + 'source can carry an escape';
    // …AND THE PRESCRIPTION AT THE END OF IT RESTED ON THE WRONG INSTRUMENT'S ARRIVAL. This branch is reached
    // on `witnessed > 0`, and solve.c says in as many words that the two counts are one question asked of two
    // instruments — "`witnessed` says the CONTEXT probe reached the sink; this says the DELIVERY probe did".
    // The sentence claimed the probes had measured "which of the source's declared bytes survive delivery"
    // and then instructed the reader to reach for a decode, off a delivery measurement that the CONTEXT
    // probe's arrival says nothing whatever about. With `deliveryProbed:0` that instruction is confident,
    // specific and unsupported — the same failure one rung down as the one this branch was added to end. So
    // the delivery clause is stated only where the delivery probe actually arrived, and the seam arm keeps
    // the context half (which `witnessed` does establish) while declining the delivery half the record on a
    // pre-`deliveryProbed` engine cannot answer.
    var builtNothing = held + ' and every one of them was an inert PROBE — ' + item.witnessed + ' sink context'
         + (item.witnessed === 1 ? ' was' : 's were') + ' READ and the derivation built no escape from '
         + (item.witnessed === 1 ? 'it' : 'any of them') + ', so this search has not constructed a breakout '
         + 'at all and there is nothing that could arrive. ';
    if (item.deliveryProbed > 0)
      return builtNothing + 'The probes measure the context AND, on ' + item.deliveryProbed + ' separate '
           + 'arrival' + (item.deliveryProbed === 1 ? "" : "s") + ', which of the source\'s declared bytes '
           + 'survive delivery — and a derivation that builds nothing from both of those together is the '
           + 'search reporting that the bytes an escape needs cannot reach this sink through this source. '
           + 'That is neither a scheduling nor a filter question: it is answered by a decode, or by another '
           + 'source';
    if (item.deliveryProbed === 0)
      return builtNothing + 'What has been measured is the CONTEXT only: this search\'s delivery probe has '
           + 'not reached a sink even once, so which of the source\'s declared bytes survive the way in is '
           + 'still unmeasured. The context read alone gave the derivation nothing to build from — that is a '
           + 'statement about this sink\'s context, and NOT yet the finding that this source cannot carry an '
           + 'escape';
    return builtNothing + 'The context probes measure where in the parse the bytes land, and this record '
         + 'states no delivery-probe arrival beside them — so whether the source\'s declared bytes survive '
         + 'the way in is not answered here, and a decode is not yet established as what this needs';
  }
  if (item.reached === 0 && item.sinkStrings === 0)
    return held + ', their bytes entered the page\'s own program ' + item.substituted + ' time'
         + (item.substituted === 1 ? "" : "s") + ', and NO code-execution sink has run at all while they were '
         + 'live — so the flows get past the source read and do not get as far as a sink. A distance question, '
         + 'and not one about the payload';
  if (item.reached === 0 && item.survived === 0)
    return held + ', their bytes entered the page\'s own program ' + item.substituted + ' time'
         + (item.substituted === 1 ? "" : "s") + ', and ' + item.sinkStrings + ' string'
         + (item.sinkStrings === 1 ? " was" : "s were") + ' handed to a code-execution sink with NOT ONE BYTE '
         + 'of the candidate in ' + (item.sinkStrings === 1 ? "it" : "any of them") + '. The sinks RAN and the '
         + 'flows got there; what did not is the payload — the page transformed it past recognition or never '
         + 'routes this value to a sink at all. A question about the PAYLOAD, and the opposite of a distance '
         + 'one';
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
     SO THE KEY IS THE RECORD, AND IT IS NO LONGER A LIST OF NAMES. Every incident above is the same one — a
     field the card reads and the key does not — and the list was corrected five times, each time by adding
     the one name that had just gone wrong. That is a SECOND COPY of "what a card is computed from", kept by
     hand beside the cards themselves, and CLAUDE.md §Architecture names the failure exactly: a restated rule
     drifts, and the copy that drifts is the one nobody runs against reality. It had drifted again by the time
     this was written — `survived`, `survivedOf`, `survivedAt`, `survivedTo`, `escaped` and `survivedBy` are
     all read by _parkedProgress and NONE of them was in the key, so a search whose ratchet improved (a better
     run of the payload observed at a sink, moving no count) went on rendering the previous sentence, which is
     the sixth instance of the recorded defect standing in the fix for the fifth.
     A record is what the engine emitted, so stringifying it cannot omit a field the card reads: there is
     nothing left to keep in step. It is still not a change detector over rendered HTML — it is the INPUT the
     cards are a pure function of, which is the one thing that can be compared without restating them.
     THE COST IS A CACHE HIT AND NOT A CORRECTNESS PROPERTY. Fields that move on every engine report (`turns`,
     and now `substituted`/`sinkStrings`) were already in the key, so a search making progress already
     re-rendered; what changes is that a search making progress in a field nobody listed does too. A panel
     that re-renders more often is a cost; a panel showing a sentence the record has already refuted is the
     confident wrong instruction these cards exist to end. */
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
      // THE WHOLE RECORD, and JSON rather than a delimiter-joined string for the reason the field list used
      // to give about `poc` and `payloads`: those are attacker payloads and may hold any byte this file would
      // use as a separator, so a hand-rolled key is one the PAGE picks collisions in.
      // ABSENCE SURVIVES THE KEY, which the list could not manage without a per-field `=== undefined ? null`
      // and got wrong wherever it forgot one. `fires` absent (the eval class queues nothing) and `fires:0`
      // (arrived, nothing executable) are two statements the parked card renders differently, and so are
      // `witnessed` absent (a single-context class) and `witnessed:0`; JSON.stringify keeps an absent key
      // absent and a 0 as 0 with nothing to remember.
      // THE SOURCE URL IS NOT PART OF THE RECORD, so it is carried beside it: the same sink on two pages is
      // two cards, and the engine's record does not name the page it was observed on.
      fpParts.push([findings[i].sourceUrl, s]);
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
    // deliveryPrefix?} for a fired sink and {sink, source, search:"parked", tried, resumed, resumedWithdrawn,
    // reached, turns, substituted, sinkStrings, runwayPerMille, survived,
    // survivedOf, survivedAt?, survivedTo?, escaped, fires?, witnessed?, deliveryProbed?, probes, payloads,
    // survivedBy, withdrawn, sourceEncodes?, sourceDelivers?, delivery?,
    // deliveryPrefix?}
    // for a parked one — the unbracketed names are solve.h's own grammar and are emitted unconditionally, which
    // is what makes 0 their load-bearing reading and absence the relay broken rather than a statement. This card used to read `shape`,
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
      // …AND `resumed` BESIDE IT, BECAUSE IT IS THE OTHER TERM OF `tried` AND WITHOUT IT THE ARITHMETIC solve.h
      // STATES CANNOT BE PERFORMED. `tried` is the entries not marked `withdrawn` PLUS the candidates the
      // engine rebuilt out of the cold tier, whose bytes ride the resumed FLOW and are in no row of `payloads`
      // at all. A reader given only the first term reads `tried:6` beside an empty list identically as a
      // cross-session search whose every run came back from a park document and as a producer that dropped a
      // field — and a `|| 0` here would pick the second, which is the confident wrong direction: it would
      // license every probes-only and every survivedBy implication below over a search whose furthest run has
      // no column at all. Emitted UNCONDITIONALLY and 0 is the load-bearing value (every run is a row here),
      // so there is no absent form to read positively.
      DCHECK(typeof pit.resumed === "number" && pit.resumed >= 0 && pit.resumed <= pit.tried,
        "a parked @S record reached the popup without its cold-resumed run count, or with more resumed runs "
        + "than runs — solve_json_array emits `resumed` on every parked entry and it is a subset of `tried` by "
        + "construction (solve_resume_candidate raises both together), so this card is about to read the "
        + "columns below as a complete account of what this search has run when it is not (sink=" + pit.sink
        + " tried=" + JSON.stringify(pit.tried) + " resumed=" + JSON.stringify(pit.resumed) + ")");
      // …AND `resumedWithdrawn` BESIDE BOTH, WHICH IS THE THIRD TERM AND THE ONE NEITHER OF THEM CAN EXPRESS.
      // solve_resume_candidate refuses a stored record whose payload this build's carrier rules contradict, and
      // solve.h states that NEITHER `tried` NOR `resumed` moves for it — both count candidate RUNS and a
      // withdrawn record has none. So a `|| 0` here is the confident wrong direction exactly as it is one
      // assert up: it would print "seeded and queued and NOTHING has run yet, so this is a scheduling state and
      // not a search that failed" over a search whose every stored candidate this build positively refused,
      // which is the opposite instruction — nothing is queued and no turn is coming.
      // ASSERTED FOR TYPE AND SIGN AND FOR NO RELATION, DELIBERATELY. `resumed <= tried` is asserted above
      // because the producer raises those two together; this one is DISJOINT from both by the producer's own
      // statement, so there is no relation here to check and inventing one — `resumedWithdrawn <= tried` is the
      // one that reads naturally — would abort on a correct record the moment a session's only parked
      // candidates were the refused ones, which is precisely the state the field exists to report. Emitted
      // UNCONDITIONALLY on the parked shape (solve.h's grammar carries it unbracketed), so 0 is the
      // load-bearing value and absence is that serializer or the relay broken rather than a statement.
      DCHECK(typeof pit.resumedWithdrawn === "number" && pit.resumedWithdrawn >= 0,
        "a parked @S record reached the popup without its withdrawn-at-the-door count — solve_json_array emits "
        + "`resumedWithdrawn` on every parked entry, and without it a search whose every stored candidate this "
        + "build refused reports tried:0,resumed:0,turns:0 byte-identically to a search nothing was ever "
        + "parked for, so this card is about to call a narrowed delivery table a scheduling state (sink="
        + pit.sink + " source=" + pit.source + " tried=" + JSON.stringify(pit.tried)
        + " resumedWithdrawn=" + JSON.stringify(pit.resumedWithdrawn) + ")");
      DCHECK(typeof pit.reached === "number" && typeof pit.turns === "number" && Array.isArray(pit.payloads),
        "a parked @S record reached the popup without reached/turns/payloads — solve_json_array emits all "
        + "three on every parked entry, so absence is that serializer or the relay to this panel broken, and "
        + "the card is about to state WHICH question this search is stuck on out of numbers it does not have "
        + "(sink=" + pit.sink + " source=" + pit.source + " reached=" + JSON.stringify(pit.reached)
        + " turns=" + JSON.stringify(pit.turns) + ")");
      // THE TWO OBSERVATION COUNTS ARE ASSERTED BESIDE THEM, AND THE IMPLICATIONS BETWEEN THEM ARE THE POINT.
      // `substituted` and `sinkStrings` are what split `turns:N,reached:0,survived:0` into the three opposite
      // instructions it was giving at once (solve.h), so a DEFAULT here is the worst possible failure of this
      // card: `substituted || 0` would print "a gate in front of the source read is turning these flows away"
      // — a confident, specific, wrong instruction — about a search whose bytes reached a sink four hundred
      // times. Presence is asserted because solve_json_array writes both UNCONDITIONALLY on the parked shape:
      // 0 is the load-bearing reading of each, so neither has an absent form to read positively.
      // THE ORDER IS THE ASSERTION AND NOT ONLY THE TYPES. The three counts are nested observations of one
      // run — bytes enter the program, then a sink is handed a string, then a run of those bytes is found in
      // it — so `sinkStrings > 0` implies `substituted > 0` and `survived > 0` implies `sinkStrings > 0`.
      // solve.c asserts the first at both of its sink entries, at the origin; asserted again here because a
      // relay that drops or reorders a field produces exactly a violated implication, and this card states
      // WHICH question the search is stuck on out of the three numbers together.
      DCHECK(typeof pit.substituted === "number" && typeof pit.sinkStrings === "number"
             && pit.substituted >= 0 && pit.sinkStrings >= 0,
        "a parked @S record reached the popup without substituted/sinkStrings — solve_json_array emits both "
        + "unconditionally on every parked entry and 0 is the load-bearing reading of each, so absence is "
        + "that serializer or the relay broken, and the card is about to name a gate in front of the source "
        + "read out of a number it does not have (sink=" + pit.sink + " source=" + pit.source
        + " substituted=" + JSON.stringify(pit.substituted) + " sinkStrings=" + JSON.stringify(pit.sinkStrings)
        + ")");
      // …AND THE RUNG BENEATH THEM, WHICH IS THE ONE FIELD ON THIS RECORD OBSERVED IN FRONT OF THE SOURCE READ.
      // `runwayPerMille` splits `substituted:0` — itself a positive statement that these runs ended before
      // their own source read — into a replay turned back at its FIRST ARM and a replay that consumed its whole
      // recorded path, which take opposite work. A default here would pick the first and print "the replay is
      // being turned back at its first arm" over a candidate that walked all of it.
      // THE RANGE IS THE PRODUCER'S OWN AND IS NOT RESTATED FROM ANYTHING. solve.c's observe_runway asserts
      // `cand_replay` in [0,1] at its own site and publishes `(int)(cand_replay * 1000.0 + 0.5)`, so [0,1000]
      // is the domain that computation can produce and nothing narrower is this file's to claim. No relation to
      // `turns` is asserted: observe_runway is called from solve_flow_end as well as from the switch-in, so a
      // candidate flow that ended without ever being switched in can legitimately carry a reading over
      // `turns:0`, and an assert tying the two would fire on it.
      DCHECK(typeof pit.runwayPerMille === "number" && pit.runwayPerMille >= 0
             && pit.runwayPerMille <= 1000,
        "a parked @S record reached the popup without its runway reading, or with one outside the fraction "
        + "solve.c can publish — observe_runway asserts cand_replay in [0,1] and emits thousandths of it, so a "
        + "value outside [0,1000] is the ladder's bottom rung carrying something other than a fraction, and "
        + "this card is about to name WHICH question a search is stuck on out of a number that is not one "
        + "(sink=" + pit.sink + " source=" + pit.source + " substituted=" + JSON.stringify(pit.substituted)
        + " runwayPerMille=" + JSON.stringify(pit.runwayPerMille) + ")");
      DCHECK((pit.sinkStrings === 0 || pit.substituted > 0) && (pit.reached === 0 || pit.substituted > 0),
        "a parked @S record reports a sink observation or an arrival over a search that has never recorded a "
        + "substitution — bytes enter the program before any sink can be handed a string carrying them, and "
        + "solve.c asserts the same at both of its sink entries, so this pair was measured on two different "
        + "searches (sink=" + pit.sink + " substituted=" + pit.substituted + " sinkStrings=" + pit.sinkStrings
        + " reached=" + pit.reached + ")");
      // THE DELIVERY PROBE'S ARRIVAL COUNT IS ASSERTED ON PRESENCE AND NEVER FOR IT, WHICH IS THE OPPOSITE OF
      // THE PAIR ABOVE AND FOR A REASON THAT IS ABOUT THE PRODUCER RATHER THAN ABOUT CAUTION. `substituted`
      // and `sinkStrings` are written UNCONDITIONALLY, so 0 is their load-bearing reading and absence is the
      // relay broken. `deliveryProbed` is written only where the search HOLDS a delivery probe
      // (cand_has_delivery_probe, solve.c) — a single-context class states its vectors at detection and runs
      // none — so its absence is a POSITIVE statement and demanding it would abort on a correct record. That
      // is §A-FIELD-A-CONSUMER-DEFAULTS' other half: a field the producer may legitimately omit gets a
      // presence TEST, and only its TYPE is asserted.
      // IT ALSO CANNOT FIRE AGAINST THE SHIPPED ENGINE, AND THAT IS CHECKED RATHER THAN HOPED. The trusted
      // zone's JS is interpreted from the tree and deploys on WRITE while the engine's C is live only after a
      // build, so an assert added here runs first against a wasm that predates its producer. Measured by
      // CONTENT, not by timestamp: `deliveryProbed` occurs ZERO times in extension/lib/qjs/qjs.wasm and
      // `sourceDelivers` occurs once, so today this field is always absent and every clause below is guarded
      // behind its presence. The assert is therefore inert on the current engine and live on the next one.
      if (typeof pit.deliveryProbed !== "undefined") {
        DCHECK(typeof pit.deliveryProbed === "number" && pit.deliveryProbed >= 0
               && pit.deliveryProbed === Math.floor(pit.deliveryProbed),
          "a parked @S record carries a `deliveryProbed` that is not a non-negative whole count — it is the "
          + "number of times this search's delivery probe REACHED a sink (solve.c increments it at the "
          + "arrival, before a single token is looked for), so a non-count here is the serializer or the "
          + "relay broken and the card is about to tell the reader a page ate an instrument that never ran "
          + "(sink=" + pit.sink + " source=" + pit.source + " deliveryProbed="
          + JSON.stringify(pit.deliveryProbed) + ")");
        // AND THE MONOTONE LINK BETWEEN THE TWO, WHICH IS THE ONLY THING THAT MAKES THE ABSENCE READABLE.
        // `sourceDelivers` is gated on `deliv_seen`, raised per TOKEN FOUND, and a token can only be found in
        // a string whose ARRIVAL was already counted — so a measured delivery set standing over
        // `deliveryProbed:0` is a byte learned with no run behind it. solve.c asserts exactly this at the
        // producer (`!deliv_seen || deliv_runs > 0`); it is asserted again here because a relay that drops or
        // reorders a field produces precisely a violated implication, and this card now reads the two
        // together to decide which of the three facts an absent `sourceDelivers` is.
        DCHECK(typeof pit.sourceDelivers !== "string" || pit.deliveryProbed > 0,
          "a parked @S record reports a MEASURED delivery set over a delivery probe that has never reached a "
          + "sink — `sourceDelivers` is the subset of the declared bytes a RUN saw arrive and its gate is "
          + "raised per token found, so this pair was measured on two different searches or the relay "
          + "reordered them (sink=" + pit.sink + " source=" + pit.source + " sourceDelivers="
          + JSON.stringify(pit.sourceDelivers) + " deliveryProbed=" + pit.deliveryProbed + ")");
      }
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
      // …AND THE LAST LEG OF THE NESTING, ASKED HERE BECAUSE `survived` IS ONLY NOW KNOWN TO BE A NUMBER. A
      // surviving run is found IN a string a code-execution sink was handed, so a run recorded over
      // `sinkStrings:0` is the ratchet and its observation counter having been measured on different searches
      // — and it is the one direction that would make the card read the page's own text as the candidate's.
      DCHECK(pit.survived === 0 || pit.sinkStrings > 0,
        "a parked @S record reports a surviving run for a search no code-execution sink has ever been "
        + "observed to run under — the run is measured IN the string a sink was handed and the counter is "
        + "raised at the same entry, above the improvement test, so a run without one means the ratchet and "
        + "its observation count came apart (sink=" + pit.sink + " survived=" + pit.survived
        + " sinkStrings=" + pit.sinkStrings + ")");
      // …AND WHERE THAT RUN IS, WHICH IS THE HALF THE PAIR ABOVE CANNOT STATE. solve.c emits `survivedAt` and
      // `survivedTo` exactly when a run has been recorded and omits BOTH when it has not, because 0 is a real
      // and common offset — so their presence is the biconditional asserted here rather than a `!== undefined`
      // read per field, which would let a relay that dropped one of them render a segment out of the other.
      // `survivedAt` is the offset into the CANDIDATE, so it plus the run is bounded by `survivedOf` for the
      // same reason `survived <= survivedOf` is: the run is a substring of its own candidate by construction,
      // and a card that states WHICH segment died out of numbers that do not fit inside the candidate states
      // it about bytes no candidate ever carried.
      DCHECK((pit.survived > 0) === (typeof pit.survivedAt === "number")
             && (typeof pit.survivedAt === "number") === (typeof pit.survivedTo === "number")
             && (pit.survived === 0 || (pit.survivedAt >= 0 && pit.survivedTo >= 0
                                        && pit.survivedAt + pit.survived <= pit.survivedOf)),
        "a parked @S record carries a surviving run without the offsets that say WHERE it is, carries offsets "
        + "for a run it never recorded, or names a segment that does not fit inside the candidate it measured "
        + "— solve.c writes the pair in the same ratchet branch as the run and omits both when there is none, "
        + "so any of those means the card is about to state which segment the page ate out of a position "
        + "measured against some other string (sink=" + pit.sink + " survived=" + JSON.stringify(pit.survived)
        + " survivedOf=" + JSON.stringify(pit.survivedOf) + " survivedAt=" + JSON.stringify(pit.survivedAt)
        + " survivedTo=" + JSON.stringify(pit.survivedTo) + ")");
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
      // AND `withdrawn` IS THE THIRD COLUMN OF THAT SAME TABLE, asserted for the reason the second one is: it
      // is read by index beside the other two, and a zero in `survivedBy` means opposite things depending on
      // it. A candidate marked here was never SENT — the search's measured delivery table contradicted its
      // bytes after it was constructed — so reporting it as one that ran and was never seen tells the reader
      // to wait for a traversal that is never going to happen. Presence is asserted rather than defaulted:
      // solve_json_array writes the column unconditionally on every parked entry, so an absent one is the
      // relay dropping a field and not an engine that had nothing to say.
      DCHECK(Array.isArray(pit.withdrawn) && pit.withdrawn.length === pit.payloads.length,
        "a parked @S record's per-candidate withdrawal column does not line up with its payload list — "
        + "solve_json_array writes payloads, survivedBy and withdrawn in one order and the card reads all "
        + "three by index, so a mismatch reports a candidate the search declined to send as one that ran and "
        + "travelled nowhere (sink=" + pit.sink + " payloads=" + pit.payloads.length + " withdrawn="
        + JSON.stringify(pit.withdrawn) + ")");
      // `probes` IS ASSERTED AGAINST `payloads` FOR THE SAME REASON `survivedBy` IS: the two are one fact, and
      // the card's FIRST sentence is now computed from the pair. solve_json_array writes it unconditionally
      // and 0 is a real value (a single-context class has no probe), so a `|| 0` here would turn "the relay
      // dropped the field" into "every entry is an attack" — which is precisely the confident wrong
      // instruction this card exists to end, in the direction that manufactures a search that never happened.
      // A count above the list length is the producer's own cursor and the list having come apart.
      DCHECK(typeof pit.probes === "number" && pit.probes >= 0 && pit.probes <= pit.payloads.length,
        "a parked @S record reached the popup without its probe count, or with more probes than payloads — "
        + "solve_json_array emits `probes` on every parked entry and the probes are the LEADING entries of the "
        + "list, so the card is about to state whether this search ever constructed an escape out of a number "
        + "it does not have (sink=" + pit.sink + " probes=" + JSON.stringify(pit.probes)
        + " payloads=" + pit.payloads.length + ")");
      // AND THE IMPLICATION THAT TIES IT TO THE ARRIVAL RUNG. A probe carries no marker by construction, so a
      // search holding nothing but probes cannot have had a breakout ARRIVE — `reached` is raised only where
      // a marker-carrying candidate is seen at the sink. The pair disagreeing means the probe cursor and the
      // arrival counter were measured on different searches.
      // …AND THE IMPLICATION IS ABOUT THE RUNS, NOT ABOUT THE LIST, WHICH IS THE QUANTIFIER IT WAS MISSING. It
      // holds exactly while every run this search has had is a ROW in `payloads`, and §@S has one run that is
      // legitimately not: a candidate rebuilt out of the cold tier carries its payload on the resumed FLOW, so
      // its marker-carrying bytes really can arrive at a sink whose whole visible list is probes. The engine's
      // own arrival assert made the same unquantified claim and ABORTED the process on it — the first time a
      // parked candidate ever came back and reached its sink, which is the one run the whole cross-session
      // machinery exists to produce. `resumed` is the producer stating the excuse; inferring it here from
      // `tried` against `payloads` and `withdrawn` would be this view re-deriving a producer fact it cannot
      // check, which is the drift these cards exist to end.
      DCHECK(!(pit.payloads.length && pit.payloads.length === pit.probes)
             || pit.reached === 0 || pit.resumed > 0,
        "a parked @S record reports a breakout ARRIVING at a sink while every candidate it has run is an inert "
        + "probe and none of its runs came back from a previous session — a probe carries no marker by "
        + "construction and a resumed candidate's payload is the only marker-carrying bytes with no row in "
        + "`payloads`, so with neither of those nothing this search ran could have raised `reached` (sink="
        + pit.sink + " reached=" + pit.reached + " probes=" + pit.probes
        + " payloads=" + pit.payloads.length + " resumed=" + pit.resumed + ")");
      // `witnessed` IS ASSERTED AS A BICONDITIONAL WITH `probes`, BECAUSE ITS ABSENCE AND ITS ZERO ARE THE TWO
      // FACTS THE CARD NOW SPLITS ON AND A `=== 0` CANNOT TELL THEM APART. solve_json_array emits the field
      // only for a class whose `derive` column is set, and solve_init asserts the exclusive-or that makes that
      // the same classes as the ones with probes (a class either states written-down vectors or derives, never
      // both), while add_pending always pushes the context probe for a deriving class — so `probes > 0` and
      // `witnessed` present are one fact stated twice, and a record where they disagree is a report about two
      // different sink classes. WITHOUT THIS the `item.witnessed === 0` branch would be FALSE for a dropped
      // field and the card would fall through to "answered by a decode, or by another source" about a search
      // whose probe has not arrived — the confident wrong instruction, reached through a missing field
      // instead of through a `||`.
      DCHECK((pit.probes > 0) === (typeof pit.witnessed === "number") && !(pit.witnessed < 0),
        "a parked @S record's probe count and its context-witness count disagree about whether this sink class "
        + "DERIVES its breakouts — solve.c emits `witnessed` for exactly the classes that have probes, so one "
        + "without the other is the relay dropping a field or two classes' records crossed, and the card is "
        + "about to state whether a derivation ever RAN out of a number it does not have (sink=" + pit.sink
        + " probes=" + pit.probes + " witnessed=" + JSON.stringify(pit.witnessed) + ")");
      // AND THE IMPLICATION THAT MAKES THE SPLIT SOUND. queue_derived is reached only from derive_from_witness,
      // which asserts it holds a witness — so a search with no witness cannot have constructed an escape, and
      // `witnessed:0` beside a payload list LONGER than its probes means an escape was built from a context
      // nothing recorded reading. One direction only: a witness that yielded no escape is the search's correct
      // final answer and is exactly what the branch below reports.
      DCHECK(pit.witnessed === undefined || pit.witnessed > 0 || pit.payloads.length === pit.probes,
        "a parked @S record holds a constructed escape for a search that has read no sink context — a derived "
        + "breakout is read off a witness (solve.c: derive_from_witness is the only route to queue_derived and "
        + "asserts `nwit > 0`), so these bytes were built by something that is not this search (sink="
        + pit.sink + " witnessed=" + pit.witnessed + " probes=" + pit.probes + " payloads="
        + pit.payloads.length + ")");
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
    else if (d.type === "POC_RAN" && ent.resultEl) _reportDelivery(d, ent);
    return;
  }
});

/* WHAT THE SANDBOX SAYS ITS DELIVERY PRODUCED — the gate in front of _pollVerify, and the reason the verdict
   below it is allowed to name the engine at all.
   THIS LINE USED TO POLL ON EVERY RUN, AND POLLING IS WHAT MAKES THE CLAIM. _pollVerify's last arm reports
   "NOT REPRODUCED … the engine's model diverges from Chrome here (an engine-fidelity bug to investigate)" —
   the strongest instruction this panel gives, and one that is only true of a payload a document actually
   received. Three states reached it as one: a delivery that ran and did not fire (the real divergence), a
   delivery that THREW before navigating anything, and a `window.open` that created NO NAVIGABLE — HTML
   §7.2.2.1 "Opening and closing windows" step 14, "If targetNavigable is null, then return null", which is
   what a popup blocker is. The second and third are HARNESS results with nothing in them about the engine,
   and both printed as an engine bug. That is §@S's tell exactly: a rung whose ABSENCE and whose ZERO read
   alike, here on the one surface CLAUDE.md defines as ENGINE AGREEMENT.
   IT FAILS CLOSED, AND DELIBERATELY DOES NOT DCHECK THE TOKEN. `outcome` crosses from the frame that EVALS
   the payload, so a payload can post its own POC_RAN — asserting the vocabulary here would hand an analysed
   page an abort of the popup, which is the hazard offscreen-brain.js's `_recordProbeHit` names for the same
   channel. So the strongest reading is reachable ONLY from an explicit positive statement and every other
   value — absent, unknown, or forged — renders as a non-delivery with the token shown. A hostile payload can
   therefore weaken its own report and can never manufacture a fire: the fire verdict is decided in the
   trusted zone against browser-stated facts, never here. */
function _reportDelivery(d, ent) {
  var el = ent.resultEl;
  if (d.outcome === "delivered") {
    el.className = "verify-result";
    el.textContent = "payload delivered — waiting for the sink to fire in Chrome…";
    _pollVerify(el, ent.marker, ent.blockers);
    return;
  }
  el.className = "verify-result verify-miss";
  if (d.outcome === "threw") {
    // VALIDATED, NOT DEFAULTED, and the difference is which side of the seam this is. Everything on a POC_RAN
    // arrives from the frame that EVALS the payload, so these are checked the way offscreen-brain.js's
    // `_recordProbeHit` checks a relayed hit — by TYPE, with the miss named — rather than asserted the way a
    // producer's own record is. A `||` here would read to the next person as a defaulted contract field.
    el.textContent = "NOT DELIVERED — the delivery threw in the attacker sandbox before any navigation "
      + "happened (" + (typeof d.error === "string" && d.error ? d.error : "the sandbox stated no message")
      + "), so no document was ever handed the payload. This is a "
      + "result about the HARNESS: it is NOT an engine-fidelity divergence, and NOT a statement that the sink "
      + "is safe. The finding stands — the engine fire-verified this breakout.";
  } else if (d.outcome === "no-navigable") {
    el.textContent = "NOT DELIVERED — window.open created no navigable, so no document was ever handed the "
      + "payload. HTML §7.2.2.1 “Opening and closing windows” step 14 returns null exactly when the user agent "
      + "creates none, which is what a popup blocker is: allow popups for this extension and run it again. "
      + "This is a result about the HARNESS — NOT an engine-fidelity divergence, and NOT a statement that the "
      + "sink is safe. The finding stands.";
  } else {
    el.textContent = "NOT DELIVERED — the attacker sandbox did not state what its delivery produced (outcome="
      + JSON.stringify(d.outcome) + "). poc-sandbox.html answers delivered / no-navigable / threw for every "
      + "run, so this is that contract broken — or a payload that posted its own POC_RAN. Either way nothing "
      + "here is evidence about the sink, and nothing here says anything about the engine's model.";
  }
}
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
    // THE DIVERGENCE CLAIM RESTS ON A PREMISE THIS FUNCTION DOES NOT ESTABLISH, so it is named: _reportDelivery
    // reaches here ONLY for `outcome === "delivered"`, i.e. the window open steps answered a navigable rather
    // than HTML §7.2.2.1 step 14's null. Without that gate this arm printed "an engine-fidelity bug to
    // investigate" for a popup Chrome never opened — a confident wrong instruction about a navigation that did
    // not happen. Anyone tempted to poll unconditionally again is removing the premise, not a guard.
    : "NOT REPRODUCED — the payload WAS delivered to a real document (window.open answered a navigable) and apiclientsink never fired, and the engine reported neither a blocking CSP nor a Trusted-Types requirement for this vector, so the engine’s model diverges from Chrome here (an engine-fidelity bug to investigate). Not a statement that the sink is safe.";
}
