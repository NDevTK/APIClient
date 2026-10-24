// lib/document-record.js — the trusted zone's record of each document it provisioned: how its navigation
// reached it, the sandboxing flags its navigable was created with, and its policy container's IP address space.
//
// A document's record holds the navigation's reach grade (the three provenance words), its drive mark (whether a drive
// of uncalled code made it), its witness mark (whether its address rested on a value a fork pinned), HTML §7.1.5's
// creation sandboxing flag set, and Local Network Access §3.5's IP address space. The zone writes one per document at
// provisioning, from the provisioning notice joined with the issuing document's record, and never again.
// lib/safe-fetch.js reads the marks as a floor under the engine's per-request words and classifies the private-network
// check against the space; the zone hands the flag set to the engine, so an engine whose words are wrong can narrow
// what fires and what a document may do, and never widen either.
//
// Records are frozen and minted only by docRecord; `_DOC_RECORDS` is how a reader tells a record from an
// object with the same keys. The marks' wire form is engine.h's arrival word, `<reach>,<drive>,<witness>`; the
// flag set's is the create notice's (§7.1.5's own flag names, comma-joined, or `none`), whose vocabulary
// lib/navigable-notice.js carries from core/frame/sandboxing.h.

var _DOC_REACH = ["observed", "derived", "forced"];
var _DOC_DRIVEN = ["driven", "undriven"];
var _DOC_PINNED = ["pinned", "unpinned"];
/* How two reach grades compose: a child of a document this tool chose to open is itself such a document, so
   the composition is the weaker grade. The order is an explicit map, not array positions, so a word added to
   the vocabulary cannot silently re-rank the others. */
var _DOC_REACH_RANK = { observed: 0, derived: 1, forced: 2 };
/* Local Network Access §2.1 "IP Address Space"'s three words, ranked from least to most public (its "less
   public than"); lib/safe-fetch.js compares a request's target against a record's space by the same map. The
   meet of several askers' spaces is the least public, since a request any asking document may make is made. */
var IP_ADDRESS_SPACE_RANK = Object.freeze({ loopback: 0, local: 1, public: 2 });
var _DOC_RECORDS = new WeakSet();
/* The three §7.1.5 flags this file's algorithms name, CHECKed against the vocabulary on load so a renamed flag
   fails here rather than reading as unset. */
var _SBX_ORIGIN = "sandboxed origin browsing context flag";
var _SBX_AUXILIARY = "sandboxed auxiliary navigation browsing context flag";
var _SBX_PROPAGATES = "sandbox propagates to auxiliary browsing contexts flag";
CHECK(typeof NAVIGABLE_NOTICE === "object" && NAVIGABLE_NOTICE.sandbox &&
      [_SBX_ORIGIN, _SBX_AUXILIARY, _SBX_PROPAGATES].every(function (n) {
        return NAVIGABLE_NOTICE.sandbox.flags.indexOf(n) >= 0;
      }),
      "lib/document-record.js loaded without lib/navigable-notice.js's §7.1.5 vocabulary, or a flag it names is " +
      "no longer in core/frame/sandboxing.h's list");

/* A serialized flag set as its members in sandboxing.h's order. The words cross from the untrusted engine on
   the create notice, so an unknown or repeated one is refused. */
function _sandboxOf(serialized) {
  var V = NAVIGABLE_NOTICE.sandbox;
  CHECK(typeof serialized === "string" && serialized !== "",
        "a document record was minted with no sandboxing flag set — `" + V.none + "` is the empty set");
  if (serialized === V.none) return [];
  var names = serialized.split(",");
  names.forEach(function (n, i) {
    CHECK(V.flags.indexOf(n) >= 0 && names.indexOf(n) === i,
          "a sandboxing flag set names " + JSON.stringify(n) + ", which is not one of HTML §7.1.5's flags or is " +
          "named twice");
  });
  return V.flags.filter(function (n) { return names.indexOf(n) >= 0; });
}

function docRecord(reach, driven, pinned, sandbox, addressSpace) {
  CHECK(_DOC_REACH.indexOf(reach) >= 0 && typeof _DOC_REACH_RANK[reach] === "number" &&
        _DOC_DRIVEN.indexOf(driven) >= 0 && _DOC_PINNED.indexOf(pinned) >= 0,
        "a document record was minted from " + JSON.stringify([reach, driven, pinned]) + " — a ranked reach " +
        "grade, `driven`/`undriven` and `pinned`/`unpinned` are the only words, because each row the record " +
        "floors reads one");
  /* A person names an observed document by no drive and no pin, so `observed` with a mark is two statements
     about one navigation that cannot both be true. */
  CHECK(reach !== "observed" || (driven === "undriven" && pinned === "unpinned"),
        "a document record states `observed` with a drive or witness mark — an observed document is one a " +
        "person named, which no drive navigated to and no pin addressed");
  CHECK(typeof IP_ADDRESS_SPACE_RANK[addressSpace] === "number",
        "a document record was minted with the IP address space " + JSON.stringify(addressSpace) + " — Local " +
        "Network Access §3.5 gives every policy container one of `public`, `local` and `loopback`, initially public");
  var r = Object.freeze({ reach: reach, driven: driven, pinned: pinned,
                          sandbox: Object.freeze(_sandboxOf(sandbox)), addressSpace: addressSpace });
  _DOC_RECORDS.add(r);
  return r;
}

function docRecordIs(r) {
  return _DOC_RECORDS.has(r);
}

function _sandboxSerialized(flags) {
  return flags.length ? flags.join(",") : NAVIGABLE_NOTICE.sandbox.none;
}

/* The flag set, serialized for the engine (the `document` and `join` records). */
function docRecordSandbox(r) {
  CHECK(_DOC_RECORDS.has(r), "a flag set was asked of something docRecord did not mint");
  return _sandboxSerialized(r.sandbox);
}

/* Whether the record's navigable has HTML §7.1.5's sandboxed origin browsing context flag, which §7.3.2.1's
   determine the origin answers with a new opaque origin. */
function docRecordOpaque(r) {
  CHECK(_DOC_RECORDS.has(r), "an origin question was asked of something docRecord did not mint");
  return r.sandbox.indexOf(_SBX_ORIGIN) >= 0;
}

/* The policy container's IP address space (Local Network Access §3.5), which a request this document makes is
   classified against. */
function docRecordAddressSpace(r) {
  CHECK(_DOC_RECORDS.has(r), "an address space was asked of something docRecord did not mint");
  return r.addressSpace;
}

/* The record of the document a navigation of `r`'s navigable created with a policy container whose IP address
   space is `addressSpace` (Local Network Access §3.5: a response's own, or a clone of its creator's), with
   every mark and flag of `r`. A join's space is its creator's, the navigable's initial document's. */
function docRecordWithAddressSpace(r, addressSpace) {
  CHECK(_DOC_RECORDS.has(r), "an address space was given to something docRecord did not mint");
  return _docRecordDerived(r.reach, r.driven, r.pinned, r.sandbox, addressSpace, [r]);
}

/* Whether `r` carries every mark and flag `of` carries and a reach no stronger. */
function _docRecordCovers(r, of) {
  return _DOC_REACH_RANK[r.reach] >= _DOC_REACH_RANK[of.reach] &&
         (of.driven !== "driven" || r.driven === "driven") && (of.pinned !== "pinned" || r.pinned === "pinned") &&
         of.sandbox.every(function (n) { return r.sandbox.indexOf(n) >= 0; });
}

/* Build a record from parts and assert permanence against the records it came from: nothing a document does
   relaxes a flag or mark its navigation carries, so the result covers each of `from`. */
function _docRecordDerived(reach, driven, pinned, flags, addressSpace, from) {
  var j = docRecord(reach, driven, pinned, _sandboxSerialized(flags), addressSpace);
  from.forEach(function (f) {
    CHECK(_docRecordCovers(j, f),
          "a derived document record cleared a flag or mark, or strengthened the reach, of " + JSON.stringify(f) +
          " — a record can only narrow");
  });
  return j;
}

function _marksJoin(a, b) {
  return { reach: _DOC_REACH_RANK[a.reach] >= _DOC_REACH_RANK[b.reach] ? a.reach : b.reach,
           driven: a.driven === "driven" || b.driven === "driven" ? "driven" : "undriven",
           pinned: a.pinned === "pinned" || b.pinned === "pinned" ? "pinned" : "unpinned" };
}

/* The stronger-wins join: the weaker reach, a mark either record carries, and the union of the flag sets.
   It is HTML §7.1.5's "determine the creation sandboxing flags" for a nested navigable, "the union of the
   flags that are present in" "the flags set on embedder's iframe sandboxing flag set" and "the flags set on
   embedder's node document's active sandboxing flag set": `a` is the creating document's record and `b` the
   notice's own. It is also how a top-level provision keeps its navigable's flags and its issuer's marks.
   The address space is `a`'s, the clone of the creator's container; a document created from a network
   response is provisioned with its response's own (docRecordWithAddressSpace, docRecordOfArrival). */
function docRecordJoin(a, b) {
  CHECK(_DOC_RECORDS.has(a) && _DOC_RECORDS.has(b),
        "two document records were joined and one was not minted by docRecord");
  var m = _marksJoin(a, b);
  return _docRecordDerived(m.reach, m.driven, m.pinned,
                           NAVIGABLE_NOTICE.sandbox.flags.filter(function (n) {
                             return a.sandbox.indexOf(n) >= 0 || b.sandbox.indexOf(n) >= 0;
                           }), a.addressSpace, [a, b]);
}

/* An auxiliary navigable (a popup) the creating document `creator` opened, whose notice states `own`. HTML
   §7.3.1.7 "the rules for choosing a navigable", given the creator's active sandboxing flag set:
     - "If sandboxingFlagSet has the sandboxed auxiliary navigation browsing context flag set", no navigable
       is created; this answers null and the caller refuses the notice.
     - "If sandboxingFlagSet's sandbox propagates to auxiliary browsing contexts flag is set, then all the flags
       that are set in sandboxingFlagSet must be set in chosen's active browsing context's popup sandboxing
       flag set." Otherwise the popup starts from its own set alone, which is how `allow-popups-to-escape-
       sandbox` works: only when every sandbox above the creator left the propagates flag clear.
   The one branch that is not the spec's: the risk marks join whatever the flags do. They describe this tool's
   act, which no attribute the page writes can opt out of, so escaping the sandbox cannot launder a forced,
   driven or pinned creator's popup into an unmarked one. */
function docRecordAuxiliary(creator, own) {
  CHECK(_DOC_RECORDS.has(creator) && _DOC_RECORDS.has(own),
        "an auxiliary navigable's record was asked of something docRecord did not mint");
  if (creator.sandbox.indexOf(_SBX_AUXILIARY) >= 0) return null;
  var inherits = creator.sandbox.indexOf(_SBX_PROPAGATES) >= 0;
  var m = _marksJoin(creator, own);
  var j = _docRecordDerived(m.reach, m.driven, m.pinned,
                            NAVIGABLE_NOTICE.sandbox.flags.filter(function (n) {
                              return own.sandbox.indexOf(n) >= 0 || (inherits && creator.sandbox.indexOf(n) >= 0);
                            }), creator.addressSpace, [own]);
  CHECK(_DOC_REACH_RANK[j.reach] >= _DOC_REACH_RANK[creator.reach] &&
        (creator.driven !== "driven" || j.driven === "driven") &&
        (creator.pinned !== "pinned" || j.pinned === "pinned"),
        "an auxiliary navigable's record dropped a risk mark its creator carries — they propagate to every " +
        "navigable the creator opens");
  return j;
}

/* The weaker-wins meet of `records`: the most observed reach, a mark only if every record carries it, and the
   intersection of the flag sets. One reply fills every park a pending line folds, and the engine folds the
   line's own marks toward the most permissive member (solver/engine.c's engine_pending_fetches), so the line
   is judged under the meet of its askers' records: the request is made if any asking document may make it.
   Every record covers the meet. */
function docRecordMeet(records) {
  CHECK(Array.isArray(records) && records.length > 0 && records.every(function (r) { return _DOC_RECORDS.has(r); }),
        "a meet was asked of no records, or of something docRecord did not mint");
  var m = records.reduce(function (a, b) {
    return { reach: _DOC_REACH_RANK[a.reach] <= _DOC_REACH_RANK[b.reach] ? a.reach : b.reach,
             driven: a.driven === "driven" && b.driven === "driven" ? "driven" : "undriven",
             pinned: a.pinned === "pinned" && b.pinned === "pinned" ? "pinned" : "unpinned",
             sandbox: a.sandbox.filter(function (n) { return b.sandbox.indexOf(n) >= 0; }),
             addressSpace: IP_ADDRESS_SPACE_RANK[a.addressSpace] <= IP_ADDRESS_SPACE_RANK[b.addressSpace]
               ? a.addressSpace : b.addressSpace };
  });
  var j = docRecord(m.reach, m.driven, m.pinned, _sandboxSerialized(m.sandbox), m.addressSpace);
  records.forEach(function (r) {
    CHECK(_docRecordCovers(r, j), "a meet is not covered by one of its records: " + JSON.stringify(r));
  });
  return j;
}

/* The name of the document `name` was minted from, or null for a name with no creator component: the engine
   names every document it creates or navigates to "<creator>.<ordinal>-<history>" (solver/world.c's
   world_mint_doc: a per-timeline ordinal and sixteen hex digits naming the creating history), from the creating
   document for a new navigable and from the navigable's current document for a navigation. */
function docRecordNameParent(name) {
  CHECK(typeof name === "string" && name !== "", "a document name's parent was asked of no name");
  var m = /^(.+)\.[0-9]+-[0-9a-f]{16}$/.exec(name);
  return m ? m[1] : null;
}

/* The document a cross-instance record's world field names: the head of solver/world.c's vector, whose fields
   are `<doc>:<session>:<serial>` joined by commas, the head first. A world is minted by a flow of exactly one
   document, so the head names the document that sent the record (solver/engine.c's routed delivery reads it
   the same way). The name ends at the head's last two colons, since a root may be named anything. */
function docRecordWorldDocument(world) {
  var head, serial, session;
  CHECK(typeof world === "string" && world !== "", "a world's document was asked of no world vector");
  head = world.split(",")[0];
  serial = head.lastIndexOf(":");
  session = serial > 0 ? head.lastIndexOf(":", serial - 1) : -1;
  CHECK(session > 0 && /^[0-9]+$/.test(head.slice(session + 1, serial)) && /^[0-9]+$/.test(head.slice(serial + 1)),
        "the world vector " + JSON.stringify(world) + " does not begin `<doc>:<session>:<serial>` — " +
        "world_serialize writes every field so");
  return head.slice(0, session);
}

/* The record a request is judged against, from the asking document's name as the engine states it on the
   pending line. A document this zone provisioned answers with its own record. One the engine created inside
   its own heap (a same-origin child or popup, or the next document of a navigable) was never provisioned;
   the engine names it from its creator (docRecordNameParent), so the nearest provisioned ancestor
   of its name is the document whose heap made it, and that record is the floor this zone holds it to.
   `lookup(name)` answers a record for a document of the asking instance, `false` for a document provisioned
   into another instance, and `undefined` for a name this zone never provisioned. Answers `{doc, refusal}`:
   the record and null, or null and why the engine named a document that is not one of its own: `elsewhere`
   when the walk meets another instance's document, `unprovisioned` when it runs out of names. A name is
   engine-stated, so an engine can name any document of its own instance; the floor is a record of that
   instance, never one it does not hold. */
function docRecordOfAsker(name, lookup) {
  CHECK(typeof name === "string" && name !== "" && typeof lookup === "function",
        "an asker's record was asked with no name or no lookup");
  for (var n = name; ; ) {
    var r = lookup(n);
    if (r === false) return { doc: null, refusal: "elsewhere" };
    if (r !== undefined) {
      CHECK(_DOC_RECORDS.has(r), "an asker lookup answered something docRecord did not mint for " + n);
      return { doc: r, refusal: null };
    }
    n = docRecordNameParent(n);
    if (n === null) return { doc: null, refusal: "unprovisioned" };
  }
}

/* The refusal of a request or notice whose document is not one of the instance's: a closed token
   `blocked-asker:<role>-<reason>` (the zone's decline vocabulary, so a census keyed on it stays over a closed
   set) and a sentence naming the document. `role` says which document failed: the `asker` of a request, the
   `creator` of a create, the `issuer` of a provisioning notice, or the `navigable`'s current document that a
   navigation's new name is minted from. `reason` is docRecordOfAsker's, or `not-minted` for a `child` or
   `document` name that is not minted from the document it must be, or `readdressed` for a `child` whose name is
   already provisioned at another address. A name is shared by every timeline whose history created the
   document, so a second create under it at another address is two documents claiming one identity. */
var _ASKER_REFUSALS = {
  "asker-elsewhere": 1, "asker-unprovisioned": 1, "creator-elsewhere": 1, "creator-unprovisioned": 1,
  "issuer-elsewhere": 1, "issuer-unprovisioned": 1, "navigable-elsewhere": 1, "navigable-unprovisioned": 1,
  "child-not-minted": 1, "document-not-minted": 1, "child-readdressed": 1,
  "opener-elsewhere": 1, "opener-unprovisioned": 1, "opener-foreign": 1, "child-sandboxed": 1
};
function docRecordAskerRefusal(role, reason, name) {
  var key = role + "-" + reason;
  CHECK(_ASKER_REFUSALS[key] === 1 && typeof name === "string" && name !== "",
        "an asker refusal was composed as " + JSON.stringify([role, reason, name]) + ", which is not one of " +
        "the closed refusals or names no document");
  var why = reason === "elsewhere" ? "a document of another instance"
          : reason === "unprovisioned" ? "no document this zone provisioned, nor one minted from such a document"
          : reason === "readdressed" ? "already provisioned at another address"
          : reason === "foreign" ? "not the navigable's creator, the only opener this zone's record allows"
          : reason === "sandboxed" ? "an auxiliary navigable its creator's record forbids (HTML §7.1.5's sandboxed " +
                                     "auxiliary navigation browsing context flag)"
          : "not minted from the document it must be";
  return { token: "blocked-asker:" + key, text: "blocked-asker:" + key + " (the " + role + " " + name + " is " +
           why + ")" };
}

/* The document and origin a navigable identity names (core/frame/remote_object.c's nav_encode: `W`, then each
   field base64 and terminated by '.', the document's world name first and its origin second), or null for
   remote_object.h's `N`. The token is the engine's claim; this reads it so the zone can compare it. */
function docRecordNavigableIdentity(token) {
  CHECK(typeof token === "string" && token !== "", "a navigable identity was read from no token");
  if (token === "N") return null;
  var parts = token.charAt(0) === "W" ? token.slice(1).split(".") : [];
  CHECK(parts.length >= 3 && parts[parts.length - 1] === "",
        "the navigable identity " + JSON.stringify(token) + " is not nav_encode's `W` and '.'-terminated fields");
  var field = function (b) {
    var bin = atob(b), bytes = new Uint8Array(bin.length);
    for (var i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
    return new TextDecoder().decode(bytes);
  };
  return { doc: field(parts[0]), origin: field(parts[1]) };
}

/* The opener this zone's record allows a navigation minting `minted` to carry: the record of the nearest
   document on the walk up from the navigable's current document (minted's name parent), which is the
   navigable's creation (`navigable.open`) or the instance root it was provisioned as. `lookup(name)` answers
   that document's record ({ opener: {doc, origin} or null, refused }) for the asking instance, `false` for
   another instance's, `undefined` for none. Answers { opener, refusal }, the refusal a docRecordAskerRefusal:
   `opener-elsewhere` or `opener-unprovisioned` for the walk, `child-sandboxed` for a navigable whose creation
   this zone refused. */
function docRecordOpenerOf(minted, lookup) {
  CHECK(typeof lookup === "function", "an opener record was asked with no lookup");
  var start = docRecordNameParent(minted);
  for (var n = start; n !== null; n = docRecordNameParent(n)) {
    var r = lookup(n);
    if (r === false) return { opener: null, refusal: docRecordAskerRefusal("opener", "elsewhere", n) };
    if (r !== undefined) {
      CHECK(r.opener === null || (typeof r.opener.doc === "string" && typeof r.opener.origin === "string"),
            "an opener record for " + n + " is neither null nor a document and its origin");
      if (r.refused === true) return { opener: null, refusal: docRecordAskerRefusal("child", "sandboxed", n) };
      return { opener: r.opener, refusal: null };
    }
  }
  return { opener: null, refusal: docRecordAskerRefusal("opener", "unprovisioned", start === null ? minted : start) };
}

/* Whether the engine's opener claim `token` may stand against the `allowed` opener of this zone's record: `N`
   always may (an engine may state less), and an identity only when it names that document at that origin.
   Answers null or a docRecordAskerRefusal. What an engine states is bounded by its own instance, so the
   peer's opener is never one the record does not name.
   Named residual: the record names the opener's document at the creation, so an opener whose navigable has
   navigated since is not covered. The next diff records the opener as a navigable and accepts a document of
   the same instance minted from the recorded one. Its absence shows as `opener-foreign` on a navigation whose
   opener page had navigated after opening it. */
function docRecordOpenerClaim(token, allowed) {
  var claim = docRecordNavigableIdentity(token);
  if (claim === null) return null;
  if (allowed !== null && claim.doc === allowed.doc && claim.origin === allowed.origin) return null;
  return docRecordAskerRefusal("opener", "foreign", claim.doc);
}

/* engine.h's arrival word, the notice's flag set and the address space, parsed; the arrival word serialized. */
function docRecordOfArrival(arrival, sandbox, addressSpace) {
  var w = typeof arrival === "string" ? arrival.split(",") : [];
  CHECK(w.length === 3, "a document record was asked of the arrival " + JSON.stringify(arrival) +
                        ", which is not engine.h's three comma-joined words");
  return docRecord(w[0], w[1], w[2], sandbox, addressSpace);
}
function docRecordArrival(r) {
  CHECK(_DOC_RECORDS.has(r), "an arrival was asked of something docRecord did not mint");
  return r.reach + "," + r.driven + "," + r.pinned;
}

if (typeof self !== "undefined") {
  self.docRecord = docRecord;
  self.docRecordIs = docRecordIs;
  self.docRecordJoin = docRecordJoin;
  self.docRecordAuxiliary = docRecordAuxiliary;
  self.docRecordOfArrival = docRecordOfArrival;
  self.docRecordArrival = docRecordArrival;
  self.docRecordSandbox = docRecordSandbox;
  self.docRecordOpaque = docRecordOpaque;
  self.docRecordAddressSpace = docRecordAddressSpace;
  self.docRecordWithAddressSpace = docRecordWithAddressSpace;
  self.docRecordMeet = docRecordMeet;
  self.docRecordOfAsker = docRecordOfAsker;
  self.docRecordNameParent = docRecordNameParent;
  self.docRecordWorldDocument = docRecordWorldDocument;
  self.docRecordAskerRefusal = docRecordAskerRefusal;
  self.docRecordNavigableIdentity = docRecordNavigableIdentity;
  self.docRecordOpenerOf = docRecordOpenerOf;
  self.docRecordOpenerClaim = docRecordOpenerClaim;
}
