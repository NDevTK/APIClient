/* mojom.js — THE INTERFACE DEFINITIONS. This is the `.mojom` file: the one description of what may cross a
 * process boundary here, loaded by EVERY realm that speaks the transport (the offscreen document, the browser
 * process's Worker), because an interface only exists if both ends agree on it. Chromium splits exactly here —
 * `mojo/public/js/bindings.js` is the machinery (extension/mojo.js) and a generated `*.mojom.js` is the
 * description — and the split is what stops a capability from being a pair of hand-written switch arms that
 * drift.
 *
 * WHY EVERY PARAMETER CARRIES A `why`. It is not a comment: mojo.js's validator PRINTS it when the value is
 * wrong. Both superseded transports held a `checkHeaderFacts` whose real content was these sentences — that an
 * absent `Content-Type` is §5.1's undefined supplied type and says so with null, never with "" — written once
 * per side and free to drift. Here the sentence lives with the DECLARATION, so both ends print the same one and
 * neither can hold a different rule.
 *
 * THE PROCESS TOPOLOGY THESE FOUR DESCRIBE, and it is the one the platform forces rather than one chosen:
 *
 *   • The BROWSER PROCESS is `extension/browser-process.js`, a dedicated Worker of the offscreen document. It
 *     owns what a browser process owns — the renderer REGISTRY, the routing ids, and (in-process, which is a
 *     real Chromium configuration and not a shortcut) the network service's own §7/CORB algorithms.
 *   • It CANNOT create a renderer itself, and that is a fact about the platform: a dedicated Worker's global is
 *     `DedicatedWorkerGlobalScope`, which has no `document` and no DOM, so there is no `createElement` in it.
 *     Chromium's browser process cannot fork a renderer by itself either — on Linux it asks the ZYGOTE, a
 *     helper process holding the state a renderer starts from, to do the fork on its behalf.
 *   • So the OFFSCREEN DOCUMENT is the zygote: `content.mojom.Zygote` is implemented in renderer-host.js and
 *     called BY the browser process. It holds no admission rule, no agent-cluster key and no ranking — it
 *     materializes the frame it is ORDERED to materialize and hands back the pipe. Where the analogy stops is
 *     worth stating: Chromium's zygote is a forked helper and ours is the trusted document that owns the DOM,
 *     which is exactly why it can create a frame when the browser process cannot.
 */
(function (g) {
  "use strict";

  DCHECK(!!g.mojo && typeof g.mojo.defineInterface === "function",
         "extension/mojo.js is not loaded in this realm — the IDL is validated as it is declared, so a mojom " +
         "file that loads first would install interfaces nothing ever checked");

  /* Two facts every response question takes, and the reason they are stated per method rather than shared: a
     mojom method's parameter list IS its contract, and a shared record would let one method's list change
     under the other. The sentences repeat because the rules do. */
  var CONTENT_TYPE = { name: "contentType", type: "string?",
    why: "an ABSENT Content-Type is §5.1's \"the supplied MIME type is undefined\" and says so with null, " +
         "never with \"\" — an empty header is a value a server can really send and means something else" };
  var XCTO = { name: "xContentTypeOptions", type: "string?",
    why: "this boundary carries the HEADER VALUE and not the derived flag, so Fetch's determine-nosniff runs " +
         "once beside the algorithms that read it (network/nosniff.c) instead of as an `indexOf(\"nosniff\")` " +
         "in a zone the architecture leaves a bridge; an absent header is null exactly as an absent " +
         "Content-Type is, and `foo, nosniff` does NOT set the flag because the standard matches the FIRST value" };
  var HEADER = { name: "header", type: "array<uint8>",
    why: "§5.2's resource header is a BYTE SEQUENCE and the whole reason this decision is taken in another " +
         "process is that it reads the body — a string here is a zone that ran a decode it does not own, and " +
         "every non-ASCII signature in §6's tables is what the decode destroys" };

  /* ── THE NETWORK SERVICE'S CONTENT DECISIONS. In Chromium these run in the network service and never in a
     renderer: a renderer that classifies for itself can mine a cross-origin body it would otherwise have been
     handed empty. Here the network service is in-process with the browser process — one Worker — which is a
     configuration Chromium itself ships rather than a collapse invented here. */
  g.mojo.defineInterface({
    name: "network.mojom.ContentSniffer",
    version: 0,
    methods: [
      { ordinal: 0, name: "CheckCorb",
        params: [CONTENT_TYPE, XCTO,
          { name: "sameOrigin", type: "bool",
            why: "the principal comparison is a fact the TRUSTED zone MADE from the browser's " +
                 "MessageSender.origin (SECURITY.md forbids re-deriving it from a URL), so it crosses as a " +
                 "browser-stated boolean and this process has no URL to invent one from" },
          HEADER],
        reply: [
          { name: "allow", type: "bool",
            why: "the verdict, §7's computed essence and the rule that decided are written together by corb.c" },
          { name: "computed", type: "string", why: "§7's computed MIME type, written with the verdict by corb.c" },
          { name: "reason", type: "string", why: "the rule that decided, written with the verdict by corb.c" }] },

      { ordinal: 1, name: "ClassifyResource",
        params: [CONTENT_TYPE, XCTO,
          { name: "opaque", type: "bool",
            why: "Fetch §2.2.6: the response is an opaque filtered response, so its body is null and its " +
                 "header list is empty by construction — a fact only the zone HOLDING the Response can state, " +
                 "and one no amount of looking at bytes can tell apart from a body that was read and was empty" },
          HEADER],
        reply: [
          { name: "asset", type: "bool",
            why: "the verdict and the rule that decided are written together by resource_kind.c" },
          { name: "reason", type: "string", why: "the rule that decided, written with the verdict by resource_kind.c" }] },
    ],
  });

  /* ── THE BROWSER PROCESS'S RENDERER REGISTRY. The interface name is Chromium's own for the browser-side
     object that owns renderer processes. The registry lives behind it and nowhere else: which agent clusters
     have a renderer, what routing id each was given, and the refusal of a second one for a cluster that
     already has one — SECURITY.md's one-instance-per-`(browsing-context group, origin)` rule, held by the
     process whose job it is to hold it rather than by the zone that wants the renderer. */
  g.mojo.defineInterface({
    name: "content.mojom.RendererHost",
    version: 0,
    methods: [
      { ordinal: 0, name: "CreateRendererForCluster",
        params: [
          { name: "clusterKey", type: "string",
            why: "SECURITY.md's agent cluster — `(browsing-context group, origin)`, both halves BROWSER-STATED " +
                 "— which is the unit a renderer IS, so a renderer is asked for by cluster and by nothing else" }],
        reply: [
          { name: "routingId", type: "int32",
            why: "minted by this registry and by nothing else: it is the only name a renderer has, and one the " +
                 "asking zone could mint for itself would be a renderer this process never decided on" },
          { name: "pipe", type: "handle<message_pipe>?",
            why: "the renderer's own pipe, forked by the zygote and RELAYED through this process — null " +
                 "exactly when `error` is not, because a launch that failed has no pipe rather than a dead one" },
          { name: "error", type: "string?",
            why: "a launch that failed is an OUTCOME and not a broken message (a page whose engine aborted its " +
                 "boot is recorded as a crashed instance), so it is a declared nullable field and never a " +
                 "rejected pipe; exactly one of `pipe` and `error` is non-null and both ends assert it" }] },

      { ordinal: 1, name: "RendererTerminated",
        params: [
          { name: "routingId", type: "int32",
            why: "the renderer whose frame is gone. It is FIRE-AND-FORGET because a real renderer's death is " +
                 "OBSERVED rather than acknowledged, and it is a method of THIS interface — not a second one — " +
                 "so that one pipe orders it against the next CreateRendererForCluster for the same cluster, " +
                 "which would otherwise be refused as a duplicate of a renderer that no longer exists" }],
        reply: null },

      { ordinal: 2, name: "GetRegistry",
        params: [],
        reply: [
          { name: "clusters", type: "string",
            why: "the registered agent cluster keys, comma-joined and sorted, with the key's NUL separator " +
                 "rendered as `|` so a reader can see it — a diagnostic view of the authority, not a second copy" },
          { name: "routingIds", type: "string",
            why: "the live routing ids, comma-joined and sorted, so the asking zone can check the renderers it " +
                 "holds against the ones this process decided on rather than against its own count" },
          { name: "live", type: "int32", why: "registered renderers right now" },
          { name: "launched", type: "int32", why: "forks this registry ordered that produced a renderer" },
          { name: "terminated", type: "int32", why: "renderers this registry has been told are gone" },
          { name: "failed", type: "int32", why: "forks this registry ordered whose renderer did not boot" },
          { name: "nextRoutingId", type: "int32",
            why: "the next id this registry will mint — the counter is here, so a renderer's id is evidence of " +
                 "which process created it" }] },
    ],
  });

  /* ── THE ZYGOTE, implemented in the OFFSCREEN and called by the browser process. This is the inversion: the
     only code path in this extension that materializes a renderer frame is the implementation of this method,
     so a renderer exists if and only if the browser process ordered one. */
  g.mojo.defineInterface({
    name: "content.mojom.Zygote",
    version: 0,
    methods: [
      { ordinal: 0, name: "ForkRenderer",
        params: [
          { name: "routingId", type: "int32",
            why: "the id the browser process minted for this renderer — the zygote does not allocate it, " +
                 "because allocating it is deciding, and deciding is what this call is obeying" },
          { name: "clusterKey", type: "string",
            why: "the agent cluster this renderer IS, carried so the frame can be identified in the document " +
                 "(its title) and so the pool's own name for the instance can be checked against the browser's" }],
        reply: [
          { name: "pipe", type: "handle<message_pipe>?",
            why: "the renderer's pipe, TRANSFERRED — which is why this is a real pipe pass and not a clone: " +
                 "the zygote is detached from it here and gets it back only from the browser process, so the " +
                 "endpoint the pool ends up calling on provably travelled through the deciding process" },
          { name: "error", type: "string?",
            why: "the engine's own abort or a frame that never booted; null exactly when `pipe` is not" }] },
    ],
  });

  /* ── THE BROWSER'S CONTROL INTERFACE TO A CHILD PROCESS. Chromium's `content.mojom.ChildProcess` is the
     interface the browser holds to every child; this is the one method of it we need, and it is here rather
     than folded into another interface because "how much IPC does that process have open" is a fact about the
     PROCESS and not about anything it serves. */
  g.mojo.defineInterface({
    name: "content.mojom.ChildProcess",
    version: 0,
    methods: [
      { ordinal: 0, name: "GetMojoStats",
        params: [],
        reply: [
          { name: "remotes", type: "string", why: "interfaces this process calls OUT on, comma-joined and sorted" },
          { name: "receivers", type: "string", why: "interfaces this process IMPLEMENTS, comma-joined and sorted" },
          { name: "endpoints", type: "int32",
            why: "open pipe endpoints in this process — a Remote and a Receiver each hold one END, so this is " +
                 "the process's half of every pipe it is on" }] },
    ],
  });
})(self);
