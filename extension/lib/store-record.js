/* lib/store-record.js — WHAT A RECORD IN THE CUMULATIVE STORE IS, FOR EVERY KIND OF RECORD IT HOLDS: one
   table naming which fields each kind states, at which STORE SHAPE it began stating them, what re-mints the
   record when this build cannot read its bytes, and how a stored value becomes the live one.

   WHY THIS FILE EXISTS. lib/persistence.js's restore door checked TWO of the store's seven maps. `endpoints`
   was checked against lib/endpoint-record.js and `discoveryDocs` against lib/discovery-entry.js; `apiKeys`,
   `probeResults`, `scopes`, `securityFindings` and `discoveryChanges` were copied out of the stored blob into
   the live maps with no question asked of them at all — `globalStore.probeResults.set(k, v)` and four lines
   exactly like it. Those five could not ABORT and they asserted NOTHING, which is the opposite failure from
   the one the stamp was built for and is the worse of the two to leave standing: a corrupt, half-written or
   stale record restored in silence is then read as a measurement, and CLAUDE.md §Offensive programming says
   which way that error runs — "too FEW asserts is the failure".

   IT IS NOT A SECOND MECHANISM. lib/persistence.js already states the store's own shape (`recordShape`) and
   splits what was one indistinguishable case into three: a STAMPED store's records ARE the shape it names, so
   the assert stands whole; a store from a LATER build is a CHECK; an UNSTAMPED store is the one population
   whose record shapes are not stated, so its records are ASKED rather than asserted, and one this build
   cannot read is SHED (its recipe outlives its bytes) or STRANDED (it is the only copy of itself) and
   COUNTED. This file is that same split carried to the five maps that had no door, plus the two that did —
   so the door has ONE loop over ONE table instead of seven hand-written blocks, and a map added to the
   serializer without a declaration here CRASHES at the door instead of restoring unchecked.

   THE SHAPE NUMBER IS PER KIND, AND THAT IS THE WHOLE OF WHY `statedFrom` EXISTS. A store stamped `1` was
   written by a door that checked two maps, so its OTHER FIVE hold records of whatever shape their producers
   had at the time — which is precisely the failure lib/persistence.js's `_STORE_SHAPE` comment names, "a
   migration that skipped a record would leave one of an older shape under this stamp, which is the one way a
   stamp is worse than no stamp". A single store-wide "is it stamped" test would therefore assert the five
   against a population nobody ever checked, and the first record that failed would take the whole cumulative
   frontier — the exact defect the stamp was built to end, re-created by trusting the stamp too widely. So
   each kind states the shape at which the store began describing IT, and the door asks
   `storeRecordShapeStates(kind, shape)`.

   HOW A KIND IS ASKED, AND WHY THE TWO OLDER KINDS ARE ASKED A NARROWER QUESTION THAN THE FIVE. The question
   is always "is a NO here this build's producer broken, or a record written before this build described this
   kind?", and the honest answer differs by what the store has ALREADY been asserting.
     `endpoints` and `discoveryDocs` have been asserted WHOLE at every save and every restore since their
     field lists existed, so the only axis on which a stored one can legitimately differ is the SET OF NAMES
     it carries — which is exactly what `endpointRecordMissingNames` and `discoveryEntryMissingNames` answer,
     and their files state why a wrong VALUE under a complete name set is the producer broken and still
     aborts. Those two ask what they have always asked; this file does not restate their question.
     The other five have never been asserted anywhere. A value their shape rejects therefore has a cause that
     is NOT this producer broken — a producer that was once permissive is the ordinary one — so for a store
     that does not state their shape the whole shape is the question, and a record that fails it is shed or
     stranded rather than fatal.
   Once a save restamps the store both populations are asserted whole, which is the terminating condition the
   unstamped path is built on.

   A DOOR RULE IS NEVER TIGHTER THAN THE PRODUCERS A READABLE STORE COULD HOLD. Where this file's shape is
   stricter than something a shipped producer once wrote, the fix belongs at that producer and the door states
   what is universally true — otherwise the assert is aimed at our own history, which has nowhere to go. */

/* ─── The predicates, named once. A field's rule is a PREDICATE and not prose, because the same rule is asked
   twice of every field — asserted at the door of a stamped store, and asked of an unstamped one — and two
   spellings of one rule are two rules free to disagree about which records are readable. ─────────────────── */
function _srStr(x) { return typeof x === "string" && x !== ""; }
function _srStrOrNull(x) { return x === null || _srStr(x); }
function _srNum(x) { return typeof x === "number" && Number.isFinite(x); }
function _srCount(x) { return Number.isInteger(x) && x >= 0; }
function _srObj(x) { return !!x && typeof x === "object" && !Array.isArray(x); }
function _srObjOrNull(x) { return x === null || _srObj(x); }
function _srArr(x) { return Array.isArray(x); }
function _srArrOrNull(x) { return x === null || Array.isArray(x); }
function _srArrOfStr(x) { return Array.isArray(x) && x.every((e) => _srStr(e)); }
function _srNonEmptyArrOfStr(x) { return _srArrOfStr(x) && x.length > 0; }

/* A RECORD SHAPE: the names it STATES (a producer that cannot state one has nothing to register) and the names
   whose ABSENT VALUE is itself a statement (`null` MEANS the producer looked and had nothing — never a hole a
   `||` fills, CLAUDE.md §Architecture). Both are `{name: predicate}` so the ONE list answers both questions:
   which names a record lacks, and whether the values under them are what this build describes. An EXTRA name
   is not an error here — a store written before a field was retired carries it, and the door reads records
   rather than minting them; the constructor is where an unknown name is a producer talking to nobody. */
function _srRecord(what, stated, absent) {
  return Object.freeze({ what: what, stated: Object.freeze(stated), absent: Object.freeze(absent || {}) });
}

/* A LIST SHAPE: the value is not a record but a LIST, so it has no names and "an earlier shape of this record"
   cannot be a question about names for it. `element` is a predicate for a list of leaves, or a record shape for
   a list of records — `scopes` is the first (a service's required OAuth scopes) and `discoveryChanges` the
   second (one entry per time a service's published surface changed). */
function _srList(what, elementWhat, element, nonEmpty) {
  return Object.freeze({ what: what, elementWhat: elementWhat, element: element, nonEmpty: !!nonEmpty });
}

/* ─── THE FIVE THAT HAD NO DOOR ──────────────────────────────────────────────────────────────────────────── */

/* AN API KEY'S ENTRY, in the form the STORE holds — which is lib/serialize.js's `serializeApiKeyEntry`
   projection, four ARRAYS where the live map carries four Sets. The live form is asserted by that function
   (it is the one place that turns Sets into the stored lists); this is the same record on the other side of
   IndexedDB, and stating it here is what lets the four `new Set(v.services || [])` defaults at the restore go:
   a `|| []` there reads a producer that stopped writing the field as "this key was never used against any
   service", which is a claim about the page made out of an absence.
     `referer` is the URL the key was matched IN, and `null` is lib/keys.js's positive statement that the
   scanned text had no address of its own (a response body, a decoded protobuf field) — a real state, not a gap.
     `name` is the KEY TYPE lib/keys.js's pattern named. lib/merge.js records that an entry stored before that
   field travelled carries none, which is why it is a STATED name and not an optional one: an absent name is
   how an earlier shape identifies itself, and the door's ask is what gives that record somewhere to go. */
const _SR_APIKEY = _srRecord(
  "an API-key entry (lib/keys.js mints it, lib/merge.js unions it into the moat, lib/serialize.js projects it)",
  {
    name: _srStr, source: _srStr, firstSeen: _srNum, lastSeen: _srNum, requestCount: _srCount,
    services: _srArrOfStr, hosts: _srArrOfStr, endpoints: _srArrOfStr, pageUrls: _srArrOfStr,
  },
  { referer: _srStrOrNull });

/* WHAT A FIELD PROBE LEARNED — lib/req2proto.js `probeApiEndpoint`, which has exactly ONE return and states
   all five of these on it. `metadata` and `scopes` are initialised to `null` there and assigned only when a
   rejection named them, so `null` MEANS "no probe answer carried a canonical service/method" and "no 403
   named a scope" — the two facts lib/popup-discovery.js guards on, read as themselves rather than defaulted. */
/* WHETHER ONE VALUE OF `fields` IS IN THE GRAMMAR THIS BUILD'S CARD ASSERTS — the same question
   `_srParkedSinkCurrent` asks of a parked @S entry, and the second and last place in this table where the
   answer was no. `fields: _srObj` said "is a non-array object" and nothing about the field records inside it,
   while lib/popup-discovery.js's `_discProbeFieldLabel` DCHECKs `f.name` on every one it labels and
   `_discFieldProbeHtml` renders `f.type` with no guard at all. So a field probe stored by a build before
   either name travelled passed this door and ABORTED the discovery card — abort for `name`, and a silent
   `undefined` in the rendered list for `type`, which is the quieter half of the same skew.
   TWO NAMES AND NOT SEVEN, BECAUSE THE CONSUMER DECIDES THE REQUIREMENT AND NOT THE PRODUCER. All five of
   lib/req2proto.js's `fields.push` sites state `name`, `path`, `type`, `number` and more, and requiring all of
   them would shed records this build reads perfectly well — a grammar stated for its own sake, which is the
   fourth-copy mistake this table already declined to make once. `number` is READ (`f.number !== null &&
   f.number !== undefined`) and deliberately not required: that test is the card reading a null as the positive
   statement it is, so absence and null must stay tellable apart rather than being demanded here.
   THESE TWO READS ARE NOT AUDITED, AND THAT IS MEASURED RATHER THAN ASSUMED — the assumption is the thing that
   failed here and it is worth more than the fix. `_srParkedSinkCurrent` states its names as member reads
   because engine/fieldgate.mjs walks reads, and the control there fires: a bogus name on `e` is reported as
   `READ with no writer` within one run. THAT CONTROL DOES NOT TRANSFER TO THIS PREDICATE, and it was run
   separately rather than argued: a bogus name on `f` here produces NO finding and leaves the gate at PASS,
   landing instead in the AMBIGUOUS ANCHOR band as "`f` shares `name`, also names `zzFieldProbe`". The reason
   is the anchor and not the construct — `e` reads fifteen names of solve.c's parked entry, so its receiver is
   decidable; `f` reads two, one of which (`name`) several producers also write, so the gate cannot decide
   whether this object is an emitted record and correctly refuses to judge the other name. This diff therefore
   added exactly one row to that band, 65 → 66, and that row is this function.
   WHAT THAT DOES AND DOES NOT COST. The door still refuses an older-grammar field record, so the abort at the
   discovery card is closed either way — the check is CORRECT and merely UNWATCHED. What is absent is the
   automatic notice if lib/req2proto.js later stops stating `name` or `type`: nothing would go red, and the
   symptom would be probe records quietly shedding and re-probing forever. Reading more of the element's names
   to strengthen the anchor is the obvious repair and is REFUSED here, because requiring a name the card does
   not depend on sheds records this build reads perfectly well — the requirement is the consumer's, not the
   anchor's convenience. The honest state is an unaudited row that says so. */
function _srProbeFieldValue(f) {
  return _srObj(f) && f.name !== undefined && f.type !== undefined;
}
function _srProbeFields(x) { return _srObj(x) && Object.values(x).every((f) => _srProbeFieldValue(f)); }

/* THE OTHER FOUR NAMES ON THIS RECORD ARE NOT THIS QUESTION, AND SAYING SO HERE IS WHAT STOPS THE NEXT READER
   RE-DERIVING IT. The test is whether a CONSUMER ASSERTS AN ELEMENT'S NAMES, never whether the element has any:
     `probeDetails` — its elements DO carry names and lib/send.js:334 reads them, but as a GUARD
                      (`pd.fieldCount > 0 && pd.contentType && …`), so a stale element is skipped rather than
                      asserted. Not exposed to this failure. It is exposed to a quieter one — a short
                      content-type list with nothing to say so — which is a DEFAULTED read and a different
                      category with a different remedy, recorded here and deliberately not fixed by a shape.
     `metadata`     — read only as `probe.metadata.service || "?"`, guarded and defaulted. No assert.
     `scopes`       — `probe.scopes.join(" ")`. Elements are strings; no name is read off one.
   A shape for any of these would be a copy of a grammar nothing checks against. */

const _SR_PROBE_FIELDS = _srRecord(
  "a field-probe answer (lib/req2proto.js `probeApiEndpoint`)",
  { url: _srStr, timestamp: _srNum, fieldCount: _srCount, fields: _srProbeFields, probeDetails: _srArr },
  { metadata: _srObjOrNull, scopes: _srArrOrNull });

/* WHAT AN ERROR-ENVELOPE PROBE NAMED — lib/req2proto.js `discoverServiceInfo`, also one return. `service`,
   `method` and `scopes` stay `null` when no content type's rejection named them, which is the whole outcome
   lib/popup-discovery.js prints in words ("the endpoint answered, and its error envelope named no service,
   method or scope"); `contentTypes` and `details` are built unconditionally and state what was tried. */
/* NEITHER LIST HERE CARRIES A GRAMMAR A CONSUMER ASSERTS, and both were checked rather than assumed.
   `contentTypes` is read as `svcInfo.contentTypes.join(", ")` — strings, no names. `details` is read by
   nothing: every `.details` in lib/req2proto.js is on a DECODED ERROR ENVELOPE a server stated
   (`decoded.error.details`), which is foreign input where §Offensive-programming makes an assert the forbidden
   answer, and not this stored list at all. So both stay element-blind on purpose. */
const _SR_PROBE_SVCINFO = _srRecord(
  "a service-info probe answer (lib/req2proto.js `discoverServiceInfo`)",
  { contentTypes: _srArr, details: _srArr },
  { service: _srStrOrNull, method: _srStrOrNull, scopes: _srArrOrNull });

/* WHICH OF THE TWO A STORED PROBE ANSWER IS, DECIDED BY THE KEY THIS ZONE MINTED IT UNDER. The four spellings
   are this store's own vocabulary and lib/popup-discovery.js already dispatches its two renderers on exactly
   this split — the bare endpoint key and `auto:<service>::<url>` hold a field probe (lib/discovery-probe.js's
   `probeEndpoint` and `performProbeAndPatch`), `svc:<endpointKey>` and `svcinfo:POST <path>` hold a
   service-info answer (lib/popup-handlers.js's DISCOVER_SERVICE button, and lib/response-decode.js's automatic
   probe, WHICH IS DELETED — nothing in this tree mints `svcinfo:` any more, and the prefix stays here because
   this table is what a store written by an EARLIER BUILD is restored through: dropping it would dispatch those
   records to `_SR_PROBE_FIELDS` and abort the restore of every profile that holds one). It is not
   §RUN-DON'T-MATCH's
   forbidden matching: that rule is about reading meaning out of names a PAGE chose, and these four literals
   are written by four sites in this zone. Asking the RECORD which shape it is instead would be circular — the
   question the door asks is precisely whether the record still carries the names its shape declares. */
function _srProbeShape(key) {
  return (key.startsWith("svc:") || key.startsWith("svcinfo:")) ? _SR_PROBE_SVCINFO : _SR_PROBE_FIELDS;
}

/* THE REQUIRED OAUTH SCOPES A SERVICE'S OWN REJECTION NAMED, per service. A LIST, not a record: all three
   producers (lib/discovery-probe.js, lib/popup-handlers.js, lib/response-decode.js) store the split scope list
   ONLY when it has entries, so an EMPTY list is a producer that stopped guarding, and what that costs is that
   the panel reports a service as requiring no scope — a fact about the service, made out of an absence.
   (popup.js used to say that sentence beside its own copy of this rule; the copy is gone and the rule is
   asked once, here, so the reason it exists is stated here too rather than pointed at.)
     THE ELEMENTS ARE NON-EMPTY STRINGS (`_srStr`), AND THE BYTES WE OURSELVES ONCE SHIPPED ARE NOT WHAT THAT
   RULE IS AIMED AT — the two are reconciled by WHICH of this file's two questions a store gets, not by
   loosening the predicate. `sendProbe`'s 403 arm split `WWW-Authenticate`'s `scope=""` into `[""]` and two of
   the three producers stored it unfiltered, so a store written before that fix can hold a scope whose name is
   the empty string. Such a store does not state this kind's shape (`statedFrom: 2`), so it is ASKED and that
   record is shed or stranded — which is exactly the somewhere-to-go a refusal needs. A store that DOES state
   it was written by producers that all filter now (lib/req2proto.js's two arms state `null` when nothing
   survives; lib/discovery-probe.js, lib/popup-handlers.js and lib/response-decode.js each store only a list
   with entries), so asserting there is asserting against bytes only a broken producer in THIS build can make.
   An earlier form of this paragraph said the elements were "not necessarily non-empty ones", which described
   a predicate this file has never had; a comment that reads as licence to loosen the line beneath it is worse
   than none, because lib/serialize.js's projection now makes that line load-bearing on the LIVE session. */
const _SR_SCOPES = _srList("a service's required OAuth scopes", "a scope name", _srStr, true);

/* AN @S FINDING AS THE MOAT HOLDS IT — lib/merge.js's one producer, three names.
     `sourceUrl` is the script the sinks were observed in, and it is the KEY this map is filed under.
     `pageUrl` is the DOCUMENT that was running when they were, and `null` is the stated absence; it is what
   offscreen-brain.js resolves a live-verify's target from, so a hole there is a probe delivered nowhere.
     `securitySinks` is what solve.c emitted, whole. The element shape is lib/popup-security.js's, asserted
   where the panel reads it, and is not restated here: this door's subject is the RECORD the store holds. */
/* WHETHER ONE ELEMENT OF `securitySinks` IS IN THE GRAMMAR THIS BUILD'S CARD ASSERTS, and it is a question
   about the ENGINE's serialized shape rather than about this file's. The names below are solve.h's parked
   entry, and lib/popup-security.js DCHECKs every one of them unconditionally when it renders the card.
   WHY THE ELEMENT AND NOT JUST THE LIST. `securitySinks: _srArr` said "is an Array" and nothing whatever about
   what is IN it, while the door's own contract one file along reads "a stamped store's records ARE the shape
   it names". Both were true and they were about DIFFERENT OBJECTS: the stamp guaranteed the OUTER record and
   the card asserts the ELEMENT, and the gap between the two objects had no guard at all. So a store stamped
   `2` — the current, correct stamp — legitimately held parked entries of any older engine's grammar, the door
   asserted the outer record and passed them through, and the card aborted on the first one it rendered. The
   §ONE frontier is cross-session, persisted and NEVER reset, so a record written by an older engine is the
   ORDINARY case here and not the exotic one.
   THE OBLIGATION COULD NOT HAVE BEEN NOTICED, WHICH IS THE PART WORTH KEEPING. lib/persistence.js's stamp says
   to bump "WHENEVER ANY RECORD IN THE STORE GAINS OR LOSES A STATED NAME" — and these names were not STATED
   anywhere, so the two commits that added `resumedWithdrawn` and `runwayPerMille` owed no bump under the letter
   of that contract and correctly made none. A version stamp cannot cover a grammar owned by a component with
   its own release cadence: the engine's C moves on a build and this constant moves on an edit, and nothing
   binds them. Stating the names HERE is what puts the grammar inside the contract, and asking the BYTES rather
   than trusting the number is what keeps it honest when someone forgets.
   READ, NEVER LISTED, AND THAT IS THE WHOLE REASON THIS IS A FUNCTION AND NOT AN ARRAY OF STRINGS. A name in a
   `_srRecord` key or in a `_ENDPOINT_STATED`-style list is a STRING, and engine/fieldgate.mjs does not audit
   strings — measured, with the control run both ways: a bogus name added as a declaration key produced NO
   finding and left the gate at PASS/279 judged, while a bogus name READ off a record was reported within one
   run as `READ with no writer` and turned the gate red. So a list here would be a fourth unwatched copy of a
   grammar that has already drifted once; these are member READS on the stored element, which puts them in the
   population that gate walks — the engine dropping one of them lights up as a read with no writer, and the
   engine ADDING one lights up as a write with no reader, which is exactly how this defect was found.
   THREE NAMES ARE DELIBERATELY NOT IN THE LIST BELOW YET, AND THIS IS THE DEFERRAL RATHER THAN AN OVERSIGHT.
   solve.c emits `runwayArms`, `runwayWalked` and `runwayOf` — the arm count of the search's frozen re-injection
   path and the two halves of the best replay position — and they are what finally split `runwayPerMille`'s zero
   into the three states it has been saying at once. Requiring them here is what the paragraph above prescribes
   and it is owed. It is not taken YET because this file deploys on WRITE and the engine's C is live only after
   a build (CLAUDE.md §A-CROSS-BOUNDARY-DIFF), and requiring a name the shipped artifact does not emit does not
   merely fail to help: this predicate SHEDS what it judges not current, so it would shed every parked @S record
   in the store until the next install.
   THE OBSERVATION THAT RETIRES IT IS NAMED RATHER THAN THE REASON, so whoever finds this runs it instead of
   re-deriving the argument — the same check the card's own runway note already uses, by CONTENT and never by
   timestamp, with a negative control so a zero means absent rather than mis-addressed:
     grep -ac runwayArms extension/lib/qjs/qjs.wasm   # and runwayWalked, runwayOf
     grep -ac runwayPerMille extension/lib/qjs/qjs.wasm   # positive control: this one is PRESENT today
     grep -ac NOT_A_REAL_FIELD_CONTROL extension/lib/qjs/qjs.wasm   # negative control: must be 0
   Measured at this commit: `survivedOf` and `runwayPerMille` PRESENT, all three of the new names ABSENT, the
   invented control absent. When the artifact carries them, the three names join the list below AND the card
   reads them, in ONE diff — the currency gate is what makes that read safe, since a record reaching the card
   has by then been judged to carry them.
   THE FIELD GATE REPORTS THEM AS WRITES WITH NO READER IN THE MEANTIME AND THAT IS CORRECT. It is not a band to
   be exempted into: a deferred consumer is a consumer that does not exist, the count is honest, and the entry
   this paragraph is stops it reading as an oversight. It goes to zero on the diff above and not before.
   A FIRED ENTRY IS NOT THIS QUESTION. solve.h emits two entry shapes and only the parked one carries these
   names, so an entry that does not say `search: "parked"` is passed rather than judged — demanding them of a
   fire-verified PoC would shed a record that is perfectly current. */
function _srParkedSinkCurrent(e) {
  if (!_srObj(e)) return false;
  if (e.search !== "parked") return true;
  return e.tried !== undefined && e.resumed !== undefined && e.resumedWithdrawn !== undefined
      && e.reached !== undefined && e.turns !== undefined && e.substituted !== undefined
      && e.sinkStrings !== undefined && e.runwayPerMille !== undefined && e.survived !== undefined
      && e.survivedOf !== undefined && e.escaped !== undefined && e.probes !== undefined
      && e.payloads !== undefined && e.survivedBy !== undefined && e.withdrawn !== undefined;
}
function _srSecuritySinks(x) { return Array.isArray(x) && x.every((e) => _srParkedSinkCurrent(e)); }

const _SR_SECURITY_FINDING = _srRecord(
  "an @S security finding (lib/merge.js's security merge)",
  { sourceUrl: _srStr, securitySinks: _srSecuritySinks },
  { pageUrl: _srStrOrNull });

/* ONE TIME A SERVICE'S PUBLISHED SURFACE CHANGED BETWEEN TWO FETCHES — lib/discovery-probe.js's diff, pushed
   onto a per-service list. `_diffDiscoveryDocs` answers `changes.length > 0 ? changes : null` and the caller
   pushes only on a truthy answer, so an EMPTY change list is a record of a change that did not happen. */
const _SR_DISCOVERY_CHANGE = _srRecord(
  "an API-drift record (lib/discovery-probe.js's published-document diff)",
  { timestamp: _srNum, fetchUrl: _srStr, changes: _srArr }, {});
const _SR_DRIFT_HISTORY = _srList("a service's API-drift history", "an API-drift record",
                                  _SR_DISCOVERY_CHANGE, true);

/* ─── THE TABLE. Its ORDER is the restore's order, and the restore fails whole, so a kind's position is the
   blast radius of a refusal in it — which is the fact that made a single unreadable endpoint take the other
   five maps entire before the stamp existed. ───────────────────────────────────────────────────────────── */
const STORE_RECORD_KINDS = Object.freeze({
  apiKeys: Object.freeze({
    statedFrom: 2, shape: () => _SR_APIKEY,
    /* THE DOCUMENT THAT SCANS THIS KEY OUT OF THE TRAFFIC AGAIN. lib/keys.js adds the page's own address to
       `pageUrls` on every sighting, so re-visiting one re-mints the entry — §OOM/paging's re-derivable third
       category, exactly as `endpointRecordRecipe` is `pageUrl` one map along. An entry with an empty list was
       matched in text no document address was known for, and is then the only copy of itself. */
    recipe: (v) => (_srArrOfStr(v.pageUrls) && v.pageUrls.length ? v.pageUrls[0] : null),
    adopt: (v) => ({
      ...v,
      services: new Set(v.services), hosts: new Set(v.hosts),
      endpoints: new Set(v.endpoints), pageUrls: new Set(v.pageUrls),
    }),
  }),
  endpoints: Object.freeze({
    statedFrom: 1, delegate: "endpoint",
    recipe: (v) => endpointRecordRecipe(v),
    adopt: (v) => v,
  }),
  discoveryDocs: Object.freeze({
    statedFrom: 1, delegate: "discoveryEntry",
    recipe: (v) => discoveryEntryRecipe(v),
    /* THE TWO SETS KEEP THEIR `|| []` AND THAT IS NOT A DEFAULT THIS FILE MISSED. lib/discovery-entry.js
       states which names an entry is asked for, and these are not among them: lib/learn.js's virtual entry
       carries neither field and lib/response-decode.js adds them the first time a request names the service,
       so their absence is that entry's own positive statement and lib/serialize.js says so at the tab loop. */
    adopt: (v) => ({ ...v, pageUrls: new Set(v.pageUrls || []), frameOrigins: new Set(v.frameOrigins || []) }),
  }),
  probeResults: Object.freeze({
    /* `4` AND NOT `2`, for the reason `securityFindings` is `3`: a store stamped `2` or `3` was written by a
       door that asked only whether `fields` was an object, so it states nothing about the field records inside
       it and must be ASKED. The recipe below is what makes that safe — a field probe names the address that
       re-mints it, so a stale one is shed and re-probed rather than aborting the discovery card. */
    statedFrom: 4, shape: (key) => _srProbeShape(key),
    /* THE ADDRESS THE PROBE WAS SENT TO, which re-probing re-mints. A SERVICE-INFO answer carries no address
       — its key names a service or a path, and neither is something `safeFetch` can be pointed at — so it is
       the only copy of itself and is reported as an overage rather than traded away. */
    recipe: (v, key) => (_srProbeShape(key) === _SR_PROBE_FIELDS && _srStr(v.url) ? v.url : null),
    adopt: (v) => v,
  }),
  scopes: Object.freeze({
    statedFrom: 2, shape: () => _SR_SCOPES,
    /* NO RECIPE, AND THE REASON IS THE RECORD RATHER THAN A GAP IN IT. A scope list is what a 403 named for a
       SERVICE; the record holds strings and no address, and the probe that learned them was issued FROM a
       document this record does not name (§Attacker-sources: an active fetch is made from the document that
       learned the endpoint). So there is nothing to point a re-derivation at, and one this build cannot read
       is STRANDED — counted and named, never dropped in silence. */
    recipe: () => null,
    adopt: (v) => v,
  }),
  securityFindings: Object.freeze({
    /* `3` AND NOT `2`, BECAUSE THE SHAPE THIS KIND STATES CHANGED WHEN `_srParkedSinkCurrent` BEGAN DESCRIBING
       THE ELEMENT. A store stamped `2` was written by a door that asked only whether `securitySinks` was an
       Array, so it makes no claim about the parked entries inside it and must be ASKED rather than asserted —
       which is precisely what raising this number does, and it is the difference between those records being
       shed and re-derived and the card aborting on them. */
    statedFrom: 3, shape: () => _SR_SECURITY_FINDING,
    /* THE DOCUMENT WHOSE FORCED EXECUTION EMITTED THESE SINKS — the same recipe an endpoint has, for the same
       reason: re-visiting it runs the same bundle and solve.c emits again, and §Time-travel-resume makes the
       re-derived answer the CURRENT one rather than a reconstruction of the old. */
    recipe: (v) => (_srStr(v.pageUrl) ? v.pageUrl : null),
    adopt: (v) => v,
  }),
  /* IT IS REHYDRATED WHERE THE DELETED `scriptCache` IS NOT, AND THE DIFFERENCE IS THAT IT HAS A READER AND
     DECIDES NO WORK. The replay cache was a document-identity seen-set whose hit SKIPPED the engine — a §NO
     BOUNDS cap; this is the record of every time a service's published surface changed, which lib/popup-
     discovery.js's drift panel reads, so restoring it restores an observation rather than a skip. */
  discoveryChanges: Object.freeze({
    statedFrom: 2, shape: () => _SR_DRIFT_HISTORY,
    /* NO RECIPE, AND `fetchUrl` IS NOT ONE — this is the entry where naming the obvious address would be the
       wrong answer rather than a missing one. The record is a DIFF BETWEEN TWO FETCHES that already happened;
       fetching that address again answers what the service publishes NOW, which is one side of a comparison
       whose other side existed only in the store. So an API-drift history is the only copy of itself, and
       §OOM/paging's sentence about the unrecoverable copy is about exactly this: "an address naming bytes
       rather than a server". Shedding it against a recipe it does not have would trade the whole history of
       a service's surface for a fetch that cannot reproduce one line of it. */
    recipe: () => null,
    adopt: (v) => v,
  }),
});

/* NAMES A STORED BLOB MAY CARRY THAT NO KIND DESCRIBES — the retired ones, listed so the door can tell them
   from a map somebody added to the serializer and forgot to declare. That distinction is the whole point of
   listing them: without it the door's "every key is a declared kind" assert would abort on every store
   written before a field was retired, and with a blanket ignore it would restore an undeclared map unchecked,
   which is the state this file exists to end.
     `scriptCache` was a document-identity seen-set whose hit skipped the engine — a §NO BOUNDS cap, deleted
   with its TTL and its 500-entry LRU trim; a store written before that carries the key and nothing reads it.
     `savedAt` was written on every save and read by nothing. */
const STORE_RETIRED_KEYS = Object.freeze(["scriptCache", "savedAt"]);

/* DOES A STORE AT `shape` STATE WHAT THIS KIND'S RECORDS LOOK LIKE? `null` is a store written before the stamp
   existed and states nothing; a number states every kind whose `statedFrom` it has reached. The door asserts
   where this answers true and ASKS where it answers false — the three-way split lib/persistence.js built,
   asked per kind because a stamp is a claim about the maps the door checked when it was written. */
function storeRecordShapeStates(kind, shape) {
  const d = STORE_RECORD_KINDS[kind];
  DCHECK(!!d, "lib/persistence.js asked about the store kind `" + kind + "`, which STORE_RECORD_KINDS does " +
              "not declare — the door iterates this table, so a name it does not hold is a map added to the " +
              "serializer without a record shape, and it would restore with nothing asked of it");
  return shape !== null && shape >= d.statedFrom;
}

/* WHY THIS BUILD CANNOT READ THIS RECORD, as a short reason, or `null` when it can. This is the ASK — the
   question put to a store that does not state this kind's shape — and it answers in words because the answer
   is REPORTED: a stranded record's whole worth to the next reader is knowing what was wrong with it. */
function storeRecordUnreadable(kind, key, v) {
  const d = STORE_RECORD_KINDS[kind];
  DCHECK(!!d, "the store shape question was asked of the kind `" + kind + "`, which is not declared");
  if (d.delegate === "endpoint") {
    const miss = endpointRecordMissingNames(v, "lib/store-record.js asking whether an endpoint out of an " +
                                               "unstamped store is of an earlier shape, key " + JSON.stringify(key));
    return miss.length ? "states no " + miss.join(", ") : null;
  }
  if (d.delegate === "discoveryEntry") {
    const miss = discoveryEntryMissingNames(v);
    return miss.length ? "states no " + miss.join(", ") : null;
  }
  return _srShapeMismatch(d.shape(key), v);
}

/* THE ONE SHAPE TEST, shared by the ask above and the assert below so the two cannot disagree about which
   records are readable — the reason a field's rule is a predicate in the table and not prose at two sites. */
function _srShapeMismatch(shape, v) {
  if (shape.element) {
    if (!Array.isArray(v)) return "is not a LIST, where the store states " + shape.what;
    if (shape.nonEmpty && v.length === 0) {
      return "is an EMPTY list, and " + shape.what + " is written by no producer in that state — every one " +
             "of them stores the list ONLY when it has entries, so an empty one is a producer that stopped " +
             "guarding and reads downstream as a positive statement that there is nothing to have";
    }
    for (let i = 0; i < v.length; i++) {
      const bad = typeof shape.element === "function"
        ? (shape.element(v[i]) ? null : "is not " + shape.elementWhat)
        : _srShapeMismatch(shape.element, v[i]);
      if (bad) return "entry " + i + " of " + shape.what + " " + bad;
    }
    return null;
  }
  if (!_srObj(v)) return "is not " + shape.what;
  for (const k of Object.keys(shape.stated)) {
    if (!Object.prototype.hasOwnProperty.call(v, k)) return "states no `" + k + "`";
    if (!shape.stated[k](v[k])) return "`" + k + "` is not the value " + shape.what + " states there";
  }
  for (const k of Object.keys(shape.absent)) {
    if (!Object.prototype.hasOwnProperty.call(v, k)) return "states no `" + k + "`";
    if (!shape.absent[k](v[k])) {
      return "`" + k + "` is neither what " + shape.what + " states there nor the `null` that MEANS the " +
             "producer looked and had nothing — and a second spelling of that absence is a consumer having " +
             "to guess which of the two it is looking at";
    }
  }
  return null;
}

/* THE BOUNDARY CHECK, called wherever a record ARRIVES from somewhere that is not a producer in this session
   (out of IndexedDB) or is about to LEAVE for one (into IndexedDB). `where` names the site, because an
   assertion inside a shared helper reports the helper for every caller and a remedy with no object is a crash
   nobody can act on — CLAUDE.md §AN-ASSERT-THAT-NAMES-A-REMEDY. */
function checkStoreRecord(kind, key, v, where) {
  const d = STORE_RECORD_KINDS[kind];
  DCHECK(!!d, "a store record was checked as the kind `" + kind + "`, which is not declared (" + where + ")");
  if (d.delegate === "endpoint") { checkEndpointRecord(v, where); return; }
  if (d.delegate === "discoveryEntry") { checkDiscoveryGrouping(v, where); return; }
  const bad = _srShapeMismatch(d.shape(key), v);
  DCHECK(bad === null,
         "a record in the cumulative store's `" + kind + "` map " + bad + " (" + where + ", key " +
         JSON.stringify(key) + ") — the store STATES that its records are this shape (lib/persistence.js's " +
         "`recordShape`), so one that is not is this build's own producer having gone silent rather than a " +
         "record an earlier shape wrote, and every surface reading it would supply the missing fact itself");
}

/* THE ADDRESS THAT MINTS THIS RECORD AGAIN, or `null` for one that is the only copy of itself — §OOM/paging's
   third category, stated per kind above with the reason it is or is not re-derivable. The caller decides what
   to do with the answer; this says which of the two it is. */
function storeRecordRecipe(kind, key, v) {
  const d = STORE_RECORD_KINDS[kind];
  DCHECK(!!d, "a store record's recipe was asked for the kind `" + kind + "`, which is not declared");
  const r = d.recipe(v, key);
  DCHECK(r === null || _srStr(r),
         "a store record's recipe is neither an address nor the stated absence (kind=" + kind + ") — `null` " +
         "is the one spelling of \"nothing can recompute this record\", and it is what separates a shed " +
         "record from a REPORTED overage");
  return r;
}

/* THE STORED VALUE AS THE LIVE MAP HOLDS IT. It is here rather than at the door because it belongs to the
   record: `apiKeys` is four Sets in RAM and four arrays on disk, and the conversion is part of what this
   kind IS. The four `new Set(v.services || [])` defaults the restore used to carry are gone with it — the
   door has asserted the arrays by the time this runs, so an absent one no longer becomes an empty Set that
   reads as "this key was never used against any service". */
function storeRecordAdopt(kind, v) {
  const d = STORE_RECORD_KINDS[kind];
  DCHECK(!!d, "a store record was adopted as the kind `" + kind + "`, which is not declared");
  return d.adopt(v);
}
