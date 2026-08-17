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

  /* `clusterKeyOf` joins the browsing-context group and the origin with a NUL, because neither half can
     contain one. A NUL is invisible in every console that prints it, so the DIAGNOSTIC view in `getRegistry`
     substitutes `|` for it and the authority itself never does. */
  var CLUSTER_KEY_SEP = String.fromCharCode(0);

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
      CHECK(typeof key === "string" && key !== "",
            "the renderer registry holds a renderer with no agent cluster key — a renderer IS a cluster's " +
            "instance, so a nameless slot is an instance nothing can find and a cluster nothing can free");
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
    var i, c, routingId;
    CHECK(typeof clusterKey === "string" && clusterKey !== "",
          "the renderer registry was asked for a renderer for an EMPTY agent cluster key — a renderer IS a " +
          "cluster's instance, so an empty one would put every document that failed to state its cluster " +
          "behind one heap and one principal");
    /* THE KEY IS DIAGNOSTIC-SAFE, ASSERTED WHERE IT ARRIVES rather than where the snapshot is rendered. A
       cluster key is a browsing-context group and a URL-serialized origin, neither of which can hold a comma,
       a `|` or a control character — the NUL that joins them is the one exception, and `getRegistry` renders
       it as `|`. Asserting it here names the caller that built the key out of something else; asserting it at
       the renderer would name the renderer. It is a DCHECK because what it protects is a reader's ability to
       tell two keys apart in one comma-joined string, not the admission decision itself. */
    for (i = 0; i < clusterKey.length; i++) {
      c = clusterKey.charCodeAt(i);
      DCHECK(c === 0 || (c >= 0x20 && c !== 0x2c && c !== 0x7c),
             "an agent cluster key carried a character neither an origin nor a browsing-context group can " +
             "hold — the key is a browser-stated group and a URL-serialized origin joined by a NUL, so a " +
             "comma, a `|` or another control character is a caller that built the key out of something else " +
             "and a registry snapshot in which two clusters cannot be told apart");
    }
    CHECK(!_renderers.has(clusterKey),
          "the renderer registry was asked for a SECOND renderer for agent cluster `" +
          clusterKey.split(CLUSTER_KEY_SEP).join("|") + "` — two heaps for one similar-origin window agent is " +
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
     frames against. `clusters` is a DIAGNOSTIC view (the NUL rendered as `|`) and never the authority. */
  function getRegistry() {
    var clusters = [], ids = [];
    invariants();
    _renderers.forEach(function (s, key) {
      clusters.push(key.split(CLUSTER_KEY_SEP).join("|"));
      ids.push(s.routingId);
    });
    return { clusters: clusters.sort().join(","),
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
