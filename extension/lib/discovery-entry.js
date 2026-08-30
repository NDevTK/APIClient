/* lib/discovery-entry.js — THE SERVICE BUCKET'S NAMING RECORD: which URL-structure rule produced the name a
   discovery-doc entry is filed under, stated by the producer that named it and read as itself by every
   surface that renders it.

   WHY THIS FILE EXISTS. `grouping` was copied through five hops — lib/merge.js's mergeToGlobal, lib/
   persistence.js's serialize, lib/serialize.js's two projections, lib/discovery-probe.js's not_found record —
   and EVERY ONE of them wrote `v.grouping || null` while NO producer ever stated the field positively. Four of
   the five are a `||` on a fresh object literal, so the field gate saw exactly one of them and the finding
   count understated the contract by four: five consumers agreeing on a default is not a contract, it is five
   independent guesses that happen to spell the absence the same way. CLAUDE.md §Architecture: "a name that is
   READ somewhere and WRITTEN nowhere is a broken contract, and a default is what stops it being a crash".

   WHAT WAS ACTUALLY BROKEN UNDERNEATH THE DEFAULT, which is the half a `|| null` cannot show you. A learned
   bucket carries its rule (lib/learn.js states one on every virtual entry it mints), and then the PUBLISHED-
   document fetch and the req2proto probe each REPLACE that entry with a fresh literal naming neither — so
   fetching a discovery document for a service silently erased the record of why that service had the name it
   has, and the Send panel's "grouping rule:" row went blank for exactly the services the tool had worked
   hardest on. That is the same drop lib/discovery-probe.js's not_found branch already carries a paragraph
   about for `doc`, one field over, and the five `|| null`s are what made it invisible: a dropped rule and a
   never-recorded rule render identically when the reader supplies the absence itself.

   THE CONTRACT. Every producer of a discovery-doc entry STATES `grouping`, as one of exactly two things:

     a GROUPING RECORD {rule, matched, firstUrl} — a URL-structure rule named this bucket. `rule` is one of
     the declared rules below, `matched` is the exact fragment that rule matched, and `firstUrl` is the
     address of the request that first named the bucket. Service grouping is heuristic — no server-side fact
     says "this URL is service X" — so the rule and its matched fragment are what let a reviewer judge the
     classification instead of trusting it, which is the whole reason the field exists.

     `null` — NO URL-structure rule is recorded as having named this bucket. This is a POSITIVE STATEMENT and
     it is true of three real records: the popup's OpenAPI import, whose bucket name is the IMPORTED
     DOCUMENT's own (`info.x-service-key`, the host of `servers[0].url`, or `info.title` — no classifier ran);
     an outcome-only projection (`{status}`) that carries no method surface and no naming history; and the
     `pending` record minted the instant a service is first named, before anything has classified it.

   `undefined` IS NOT ONE OF THE TWO, and separating it from `null` is the entire point: `null` means a
   producer looked and had nothing to record, `undefined` means no producer spoke. The consumer reads the
   first and CRASHES on the second, which is what a `||` made impossible to tell apart. It is also why the
   absent value is `null` and not `undefined` — the entry crosses chrome.runtime.sendMessage to the popup
   (lib/serialize.js) and that serialization DROPS an `undefined` property, so an `undefined` absent value
   would arrive absent on one side of the boundary and present on the other.

   NAMED RESIDUAL — WHAT THIS FILE DOES NOT COVER YET. A discovery-doc entry carries a dozen more names
   (`status`, `url`, `apiKey`, `fetchedAt`, `doc`, `isVirtual`, `publishedJson`, `seedUrl`, `seedMethod`,
   `_triedKeys`, `pageUrls`, `frameOrigins`), and this file declares NONE of them: it is the `grouping`
   contract, not the entry's constructor. That is narrower than lib/endpoint-record.js is for an endpoint, and
   deliberately so — those names have four genuinely different record shapes (pending / not_found / fetched /
   virtual) and closing them is a separate reading of each producer. THE NEXT DIFF builds `makeDiscoveryEntry`
   here, with a shape per producer, and every `discoveryDocs.set` in the extension goes through it. ITS ABSENCE
   SHOWS as exactly the defect above one field over: `isVirtual` is read `!!v.isVirtual` at three consumers and
   the published-document fetch states it at none, so a fetched entry reads as "not virtual" through a `!!`
   that cannot tell that from a producer that went silent. */

/* THE RULE DOMAIN. Asserted as a MEMBERSHIP rather than as "a string" for lib/endpoint-record.js's reason: a
   third spelling would render in the Send panel as a rule the reviewer has no way to evaluate, and the two
   below are the complete set of rules any producer in this extension applies.
     "origin"      — lib/grouping.js's classifyInterface: the bucket IS the real origin the browser saw.
                     Endpoint identity is never a name-regex or URL-pattern guess (minified names are
                     meaningless and a guess drifts silently); finer grouping comes from the engine.
     "ast-dynamic" — lib/learn.js's shape-origin arm: the call site's address is a SHAPE, so there is no
                     literal origin to classify and the bucket is named by the shape itself. */
const GROUPING_RULES = Object.freeze(["origin", "ast-dynamic"]);

/* THE ONE ORIGIN of a grouping record. `where` names the producer, because an assertion that cannot say WHICH
   producer went silent sends the reader to read all of them. */
function makeGroupingRecord(rule, matched, firstUrl, where) {
  DCHECK(GROUPING_RULES.indexOf(rule) >= 0,
         "a service bucket was named by the rule `" + rule + "`, which is not one this extension applies (" +
         where + ") — the Send panel prints the rule so a reviewer can judge whether the grouping is right " +
         "for the site, and a word outside the declared set is a rule nobody can evaluate");
  DCHECK(typeof matched === "string" && matched !== "",
         "a service bucket's grouping states no MATCHED fragment (" + where + ") — the rule alone says which " +
         "question was asked and the fragment says what answered it, and a rule with nothing beside it reads " +
         "in the panel as a classification with no evidence");
  DCHECK(typeof firstUrl === "string" && firstUrl !== "",
         "a service bucket's grouping states no FIRST address (" + where + ") — it is the request that named " +
         "this bucket, which is what makes the classification reproducible; both producers reach this line " +
         "past lib/learn.js's own check that the call site carries a non-empty url");
  return { rule: rule, matched: matched, firstUrl: firstUrl };
}

/* THE BOUNDARY CHECK. Called wherever an entry ARRIVES from somewhere that is not a producer in this session —
   across chrome.runtime.sendMessage into the popup, and out of the IndexedDB store a previous session wrote —
   and at every hop that COPIES the field, so a copier cannot be the place the statement goes missing. */
function checkDiscoveryGrouping(entry, where) {
  DCHECK(!!entry && typeof entry === "object" && !Array.isArray(entry),
         "a discovery-doc entry is not a record (" + where + ") — every value in a discoveryDocs map is one");
  DCHECK(Object.prototype.hasOwnProperty.call(entry, "grouping"),
         "a discovery-doc entry does not state `grouping` (" + where + ") — every producer states it, as a " +
         "{rule, matched, firstUrl} record or as `null` meaning \"no URL-structure rule is recorded as having " +
         "named this bucket\"; an ABSENT field is a producer that never spoke, and the five `|| null`s this " +
         "check replaced could not tell that from a producer that looked and had nothing");
  const g = entry.grouping;
  if (g === null) return;
  DCHECK(!!g && typeof g === "object" && !Array.isArray(g) &&
         GROUPING_RULES.indexOf(g.rule) >= 0 &&
         typeof g.matched === "string" && g.matched !== "" &&
         typeof g.firstUrl === "string" && g.firstUrl !== "",
         "a discovery-doc entry's `grouping` is neither a {rule, matched, firstUrl} record nor the stated " +
         "absence (" + where + ") — `null` is this record's one spelling of \"no rule is recorded\", and a " +
         "second spelling is a consumer having to guess which of them it is looking at");
}

/* CARRY THE STATEMENT ACROSS A REPLACEMENT. Three producers REPLACE an entry rather than mutate it (the
   published-document fetch, the req2proto probe, the not_found record), and what they must not do is drop the
   naming record the entry they are replacing carried — a fetch answering a question about a PUBLISHED DOCUMENT
   says nothing whatever about which rule named the bucket. No prior entry means no rule has been recorded for
   this bucket yet, which is the declared absence and not a hole. */
function carriedGrouping(prev, where) {
  if (!prev) return null;
  checkDiscoveryGrouping(prev, where);
  return prev.grouping;
}
