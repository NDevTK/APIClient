/* render-process-host.js — THE RENDERER REGISTRY. Which agent clusters have a renderer, what routing id each
 * was given, and the refusal of a SECOND one for a cluster that already has one: SECURITY.md's
 * one-instance-per-`(browsing-context group, origin)` rule, held by the component whose job it is to hold it.
 *
 * THE NAME IS CHROMIUM'S FOR EXACTLY THIS OBJECT, and it is not the same object as its neighbour.
 * `RenderProcessHost` is the browser-side object that owns a renderer PROCESS and mints its ID;
 * `RenderFrameHost` — extension/renderer-host.js, which says so in its own first line — owns a FRAME inside
 * one. The split here is theirs: this file DECIDES that a renderer exists, that file MATERIALIZES it and is
 * the only thing that speaks to it. The near-identical names carry the distinction rather than blurring it,
 * which is why neither is a coinage (CLAUDE.md §Architecture: never coin a system when an established one
 * exists).
 *
 * ═══ WHY THIS IS JAVASCRIPT, AND WHY THAT IS THE SAME ARGUMENT THAT PUTS THE ENGINE IN C ═══
 *
 * This component arbitrates between renderers of DIFFERENT ORIGINS. It decides which exist, and its table is
 * what stops two documents that are not one similar-origin window agent from being handed one heap and one
 * principal. A memory-corruption bug HERE is a cross-origin boundary failure — the privileged component is
 * exactly the one that must be memory-safe, and this is the option Chromium does not have and we do.
 *
 * The renderer is the opposite case and is why C belongs THERE: it runs attacker-influenced page code, it
 * needs the per-flow COW delta and the forking that JavaScript structurally cannot do, and it is already
 * confined by an opaque origin, Site Isolation and the WASM sandbox — so a memory bug in it corrupts that
 * renderer's own analysis and nothing else. CLAUDE.md §Architecture states the same test from the other
 * side: the engine owns what a FLOW needs mid-execution, whose answer must fork and park with the flow. A
 * `Map` from a string to an integer forks with nothing and parks with nothing. It is decided ONCE, between
 * flows, in the trusted zone, where a mistake is a wrong answer instead of a corrupted heap.
 *
 * THIS TABLE WAS C FOR ONE SESSION AND THE COST IS ON THE RECORD. It was 364 lines of hand-grown table,
 * hand-grown JSON serialization, two insertion sorts and a `realloc` loop, reached through a
 * `_malloc`/`HEAPU8.set`/`_free` marshalling of the cluster key — all of it to answer `map.has(k)`. It is a
 * Map again.
 *
 * ═══ WHAT DID NOT COME BACK AS IT WAS ═══
 *
 * The duplicate-cluster refusal was a `DCHECK` — dev-only — and its release path FELL THROUGH to overwriting
 * the map entry. A second renderer would silently have replaced the first's registration, and the first's
 * termination would then have freed the SECOND's cluster: SECURITY.md's rule failing open in production, with
 * nothing anywhere to say so. It is a `CHECK` here, fatal in dev AND release, because what a violation means
 * is two heaps behind one principal and that does not become acceptable because the build is a release one —
 * which is check.js's own stated test for CHECK-class ("a security/authorization boundary (SECURITY.md)").
 * The same reasoning promotes every other refusal in this file: an id this table never minted, a renderer
 * reported dead twice, an exhausted id space, and the arithmetic that ties the slots to the counters.
 *
 * ═══ AND THE KEY IS A PAIR, WHICH THIS TABLE NOW SAYS ═══
 *
 * The authority took the cluster key as an OPAQUE non-empty string and asserted a CHARACTER SET over it: no
 * comma, no `|`, no control character but the NUL that joins the halves, on the stated grounds that "a cluster
 * key is a browsing-context group and a URL-serialized origin, neither of which can hold" those. Both halves of
 * that were wrong, in the two directions that matter.
 *
 * It asserted the WRONG THING. What makes `group + NUL + origin` a safe name for a `(browsing-context group,
 * origin)` agent cluster is that the mapping is INJECTIVE — and a comma cannot break that while a SECOND NUL
 * can: `("a\0b", "")` and `("a", "b")` are two different agent clusters and one string, so a table keyed on
 * that string hands them ONE instance, which is two principals behind one heap and is exactly the split
 * SECURITY.md's rule exists to forbid. The character set never checked injectivity; it checked legibility.
 *
 * And it was FACTUALLY FALSE ABOUT THIS TREE, so it was a live landmine. `bridge.js` rehydrates a cold recipe
 * with `groupId: "cold:" + c.key` and that frontier key is `origin + "|" + bundleId` — a `|` in the group half,
 * on every cross-session resume there has ever been. In a dev build the char assert aborts the offscreen at the
 * admission; in release it compiles out and the ambiguity it was there to keep out of a DIAGNOSTIC arrives
 * silently. The group half is NOT a URL origin — it is a browser-stated tab id or that frontier key — and a
 * rule that constrains what a producer may put in it is this table legislating for a producer it does not own.
 *
 * So the shape is asserted where it is load-bearing and the legibility problem is solved where it was: the key
 * is PARSED into its two halves (CHECK: exactly one separator, a non-empty group), and `getRegistry` reports
 * the halves as a RECORD instead of re-joining them into a string that needed a character set to stay readable.
 * An EMPTY ORIGIN half is not refused, because it is a positive statement rather than a hole: a rehydrated cold
 * recipe has no live browser document and therefore no browser-stated principal, and bridge.js gives it a group
 * that is unique per recipe — a cluster of one, which collides with nothing.
 *
 * ═══ WHAT THE REGISTRY IS NOT, AND WHY THERE IS NO PIPE ═══
 *
 * It was a dedicated Worker reached over a Mojo pipe (`content.mojom.RendererHost`), ordering the offscreen's
 * `content.mojom.Zygote` back to materialize each frame. Both interfaces are deleted with that Worker, and
 * the reason is the one Mojo itself gives: a pipe is a PROCESS boundary. The offscreen writes the Worker's
 * program, so the Worker was never a trust boundary — and with the C gone what the pipe separated was a Map
 * from its only caller, at the cost of three thread hops per renderer, an admission decision that could
 * suspend between its check and its write, and a MessagePort detached and re-adopted so that "the endpoint
 * travelled through the deciding process" would be a true sentence. A boundary that isolates nothing is the
 * confinement-by-convention this project deletes everywhere else.
 *
 * AND THE DECISION IS NOW STRUCTURALLY ATOMIC RATHER THAN DEFENDED BY A COMMENT. `registerRenderer` is a
 * synchronous function in a run-to-completion realm: there is no await between the refusal and the write, so
 * a second request for one cluster arriving while a fork is outstanding CANNOT find an empty table. The C
 * version made the same claim on the grounds that "a ccall cannot suspend", and the JavaScript it replaced
 * needed a paragraph asking the next editor not to await there. Neither is needed now.
 *
 * WHAT IT STILL COSTS TO BE HONEST ABOUT: the routing ids are minted in the same realm as the frames they
 * name, so `rendererPoolProbe`'s cross-check no longer proves that another PROGRAM decided. What it proves is
 * stated at that probe, and what makes the inversion hold is that `registerRenderer` has exactly one caller
 * and that caller is the only path to a frame — a discipline in one file, not a boundary.
 */
(function (g) {
  "use strict";

  DCHECK(typeof g.CHECK === "function",
         "extension/check.js is not loaded in this realm — SECURITY.md's one-instance-per-agent-cluster " +
         "refusal in this file is CHECK-class, so a realm without the mechanism would hold the authority with " +
         "nothing to enforce it");

  /* ONE TABLE AND NO SECOND INDEX. A termination names a ROUTING ID while an admission names a CLUSTER KEY,
     which is a standing invitation to keep two maps — and two maps for one authority is two answers with
     nothing to say which is right the first time they disagree. The pool this serves holds a handful of
     renderers, so the id lookup is a scan over the authority itself.
     THE SLOT'S STATE IS PART OF THE CONTRACT. A slot is RESERVED from the moment this component decides the
     cluster gets an instance until the frame it caused has booted, and LAUNCHED afterwards. Both are
     registered — a reserved cluster is taken, which is the entire reason the id is minted before the frame is
     built — but they are not the same fact, and keeping them apart is what lets `rendererLaunchFailed` assert
     that it is freeing a slot whose renderer never booted and `rendererTerminated` assert that it is freeing
     one whose renderer did. */
  var _renderers = new Map();   /* clusterKey -> { routingId, launched } */
  var _nextRoutingId = 1;
  var _launched = 0, _terminated = 0, _failed = 0;

  /* `clusterKeyOf` joins the browsing-context group and the origin with a NUL. THE JOIN IS ONLY A NAME FOR THE
     PAIR IF IT IS INJECTIVE, so this is where the pair is taken back apart and the injectivity is asserted:
     exactly one separator, and a group half that is not empty. Two NULs would let two agent clusters spell one
     key and share one instance; an empty group half would put every document whose group went missing into one
     cluster with every other. Both are CHECK-class for the same reason the duplicate refusal is — what a
     violation means is two principals behind one heap.
     THE HALVES ARE NOT INSPECTED BEYOND THAT, and the previous character-set assert is deleted rather than
     narrowed: the group half is a browser-stated tab id OR `bridge.js`'s `cold:<origin>|<bundle>` frontier key,
     so a `|` in it is the cross-session frontier working, not a caller that built a key out of something else.
     A NUL is invisible in every console that prints it, which is why `getRegistry` reports the two halves as
     separate FIELDS — there is no re-joined string left for a character set to have to keep legible. */
  var CLUSTER_KEY_SEP = String.fromCharCode(0);
  function clusterPair(clusterKey) {
    var i;
    CHECK(typeof clusterKey === "string" && clusterKey !== "",
          "the renderer registry was handed an agent cluster key that is not a string — a renderer IS a " +
          "cluster's instance and the key is the whole of how this table knows which, so anything else here " +
          "is a caller naming a cluster this authority cannot tell apart from another");
    i = clusterKey.indexOf(CLUSTER_KEY_SEP);
    CHECK(i > 0 && clusterKey.indexOf(CLUSTER_KEY_SEP, i + 1) < 0,
          "an agent cluster key is not a (browsing-context group, origin) PAIR — it is the two halves joined " +
          "by one NUL, and a key with none has no group, a key whose separator is first has an EMPTY group, " +
          "and a key with two lets `(a\\0b, \"\")` and `(a, b)` — two different agent clusters — spell one " +
          "name, which is this table handing them one instance, one heap and one principal");
    return { group: clusterKey.slice(0, i), origin: clusterKey.slice(i + 1) };
  }
  /* THE PAIR, RENDERED FOR A HUMAN READING AN `@E` LINE. It is never a key and never compared — the authority
     is the string the caller handed over, and this is the only thing in the file that reshapes it. */
  function pairText(p) { return p.group + " | " + p.origin; }

  /* ── THE TABLE'S OWN INVARIANTS, ASSERTED AFTER EVERY MUTATION AND BEFORE EVERY READ. They are CHECK and
     not DCHECK for this file's stated reason: what a violation means is two heaps behind one principal, or an
     agent cluster refused an instance forever, and neither becomes acceptable in a release build.
     THE ARITHMETIC IS WHAT MAKES A SNAPSHOT UNABLE TO DISAGREE WITH ITSELF. Every id this table ever minted
     is in exactly one of three states — still registered, terminated, or failed to launch — so the three
     counts must account for the whole id space it has issued; and the slots that say they launched must be
     exactly the launches it has recorded minus the terminations. A counter that stopped being incremented, or
     a slot freed without its counter, is caught here and not in a probe that happens to compare two of the
     four.
     ONE CHECK THE C VERSION NEEDED IS STRUCTURAL HERE AND IS THEREFORE ABSENT: two slots holding ONE agent
     cluster was a pairwise `memcmp` over an array, and a `Map` keyed on the cluster cannot represent it. */
  function invariants() {
    var liveLaunched = 0, ids = new Set();
    CHECK(_nextRoutingId > 0,
          "the renderer registry's routing id counter is not positive — an id is the only name a renderer " +
          "has, and a non-positive one is a counter that wrapped and is about to re-issue a live renderer's " +
          "name");
    _renderers.forEach(function (s, key) {
      CHECK(typeof s.routingId === "number" && s.routingId > 0 && s.routingId < _nextRoutingId,
            "the renderer registry holds a renderer whose routing id this table never minted — the counter is " +
            "the only source of an id, so a registered one outside its issued range is a renderer some other " +
            "component decided existed");
      /* THE KEY'S SHAPE IS PART OF THE TABLE'S STATE, so it is re-checked here with the arithmetic rather than
         only at the door. A slot whose key stopped being a pair is a cluster nothing can free by name. */
      clusterPair(key);
      CHECK(!ids.has(s.routingId),
            "two renderers in the registry carry ONE routing id — an id is the only name a renderer has, so a " +
            "collision means a termination frees whichever of the two the scan reaches first and leaves the " +
            "other registered forever");
      ids.add(s.routingId);
      if (s.launched) liveLaunched++;
    });
    CHECK(_renderers.size + _terminated + _failed === _nextRoutingId - 1,
          "the renderer registry does not account for every routing id it has minted — each one is " +
          "registered, terminated or failed to launch, so a shortfall is a slot freed without its counter and " +
          "a surplus is a counter moved without its slot");
    CHECK(liveLaunched === _launched - _terminated,
          "the renderer registry holds a different number of launched renderers than it has launched and not " +
          "yet buried — the slots and the counters are one fact recorded twice, and the probe that compares " +
          "this document's frames against this table would report whichever of the two it happened to read");
  }

  /* THE LOOKUP EVERY ID-TAKING ENTRY MAKES, WITH THE TWO WAYS IT CAN FAIL TOLD APART, because they are
     different accusations and a caller reading one `@E` line is standing where the fix has to be made. An id
     at or beyond the counter was NEVER MINTED here. An id below it names a renderer this table has already
     buried, which is one death reported twice and would free an agent cluster that has a live instance. */
  function slotRequire(routingId) {
    var found = null;
    CHECK(typeof routingId === "number" && routingId > 0 && routingId < _nextRoutingId,
          "the renderer registry was told about a renderer whose routing id it never minted (" + routingId +
          ") — an id comes out of this table and out of nothing else, so one arriving from outside its issued " +
          "range is a renderer some other component decided existed");
    _renderers.forEach(function (s, key) { if (s.routingId === routingId) found = { slot: s, key: key }; });
    CHECK(found !== null,
          "the renderer registry was told about renderer " + routingId + ", which it has already buried — the " +
          "id was minted here, so this is one renderer reported dead twice, and the second report frees an " +
          "agent cluster that either has a live instance or is about to be given one");
    return found;
  }

  /* ═══ THE FOUR TRANSITIONS ═══
     They are the whole of this component's surface, and every refusal in them is a CHECK rather than a
     returned value: a returned refusal is a value a caller can ignore, and what it would be ignoring is two
     heaps behind one principal. */

  /* THE DECISION AND THE SLOT, IN ONE SYNCHRONOUS STEP. `renderer-host.js` calls this and then materializes
     the frame; there is no await between the refusal below and the write under it, so the window in which a
     second arrival for one cluster could find an empty table does not exist. */
  function registerRenderer(clusterKey) {
    var routingId, p = clusterPair(clusterKey);
    CHECK(!_renderers.has(clusterKey),
          "the renderer registry was asked for a SECOND renderer for agent cluster `" + pairText(p) +
          "` — two heaps for one similar-origin window agent is " +
          "the split SECURITY.md's one-instance-per-cluster rule exists to forbid, and this table is the " +
          "authority that already held the answer");
    CHECK(_nextRoutingId < 0x7fffffff,
          "the renderer registry has exhausted the routing id space — the next id would wrap into one a live " +
          "renderer already answers to");
    routingId = _nextRoutingId;
    _nextRoutingId++;
    _renderers.set(clusterKey, { routingId: routingId, launched: false });
    invariants();
    return routingId;
  }

  function rendererLaunched(routingId) {
    var f = slotRequire(routingId);
    CHECK(!f.slot.launched,
          "the renderer registry was told renderer " + routingId + " launched, which it has already recorded " +
          "as launched — one fork produces one renderer, so a second report inflates the launch count the " +
          "document's own fork total is checked against and hides a fork that never arrived");
    f.slot.launched = true;
    _launched++;
    invariants();
  }

  /* A LAUNCH THAT FAILED FREES ITS CLUSTER. The frame is already gone — renderer-host.js reclaims its own —
     so leaving the registration would refuse this agent cluster a renderer forever, with nothing anywhere to
     say why. */
  function rendererLaunchFailed(routingId) {
    var f = slotRequire(routingId);
    CHECK(!f.slot.launched,
          "the renderer registry was told a LAUNCHED renderer (" + routingId + ") failed to launch — a " +
          "renderer that booted dies by termination, and recording it as a failed launch would leave the " +
          "launch count claiming a renderer that is gone");
    _renderers.delete(f.key);
    _failed++;
    invariants();
  }

  /* A RENDERER'S DEATH IS OBSERVED, NOT ORDERED — which is what a real one is: a child process can exit on
     its own and the browser learns of it. renderer-host.js owns the frame and so is what notices; this is
     where the agent cluster is freed, which is what lets the next request for that cluster be admitted
     instead of refused as a duplicate of a renderer that no longer exists. */
  function rendererTerminated(routingId) {
    var f = slotRequire(routingId);
    CHECK(f.slot.launched,
          "the renderer registry was told renderer " + routingId + " terminated while its launch has not been " +
          "recorded — a renderer that never booted is buried by its own launch failure, and doing it twice " +
          "would count one dead instance against both totals");
    _renderers.delete(f.key);
    _terminated++;
    invariants();
  }

  /* THE TABLE, READ AS ONE RECORD. The seven fields are rendered together from one walk, so no two of them
     can be read from different moments — which is the property `rendererPoolProbe` compares this document's
     frames against.
     `clusters` IS A LIST OF PAIRS AND NO LONGER A JOINED STRING, which is the whole reason the character-set
     assert above could go. It was `key.split(NUL).join("|")` pushed into a comma-joined string, so a `|` or a
     comma in either half produced a snapshot in which two clusters could not be told apart — and the group
     half of a rehydrated cold recipe contains a `|` by construction. Reported as `{group, origin}` there is
     nothing to disambiguate: the halves the key was built from are the halves that come back out, beside the
     routing id that names the renderer holding them. It is still a DIAGNOSTIC view and never the authority —
     the authority is the key string itself, which is what this table is keyed on. */
  function getRegistry() {
    var clusters = [], ids = [];
    invariants();
    _renderers.forEach(function (s, key) {
      var p = clusterPair(key);
      clusters.push({ group: p.group, origin: p.origin, routingId: s.routingId, launched: s.launched });
      ids.push(s.routingId);
    });
    clusters.sort(function (a, b) { return a.routingId - b.routingId; });
    return { clusters: clusters,
             routingIds: ids.sort(function (a, b) { return a - b; }).join(","),
             live: _renderers.size, launched: _launched, terminated: _terminated, failed: _failed,
             nextRoutingId: _nextRoutingId };
  }

  g.renderProcessHost = {
    registerRenderer: registerRenderer,
    rendererLaunched: rendererLaunched,
    rendererLaunchFailed: rendererLaunchFailed,
    rendererTerminated: rendererTerminated,
    getRegistry: getRegistry,
  };

  console.debug("[render-process-host] ready (self.renderProcessHost installed: the renderer registry)");
})(self);
