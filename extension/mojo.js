/* mojo.js — THE IPC PRIMITIVE, and it is Mojo rather than a vocabulary coined here. CLAUDE.md §Architecture:
 * "Use browser / JS-engine developers' concepts and names — never coin a system when an established one
 * exists." Chromium's inter-process layer is Mojo, its concepts are message pipes / interfaces / Remote /
 * Receiver / an interface broker, and every one of them maps onto something this platform already has.
 *
 * WHAT THIS REPLACES. Two ad-hoc envelope vocabularies grew, one per boundary, each carrying its own `id`
 * routing table, its own `op` string, its own `ok`/`err` convention and its own reply drain — renderer-host's
 * `{v:1,id,op:"call",fn,ret,args,bodies}` and browser-process-host's `{v:1,id,op:"corb",…}`. Two transports for
 * one problem is the dual system CLAUDE.md forbids: every capability a boundary gains is a new `op` hand-written
 * on both sides, and every omission is silent. Mojo's answer is that a boundary carries no capability list at
 * all — it carries ONE brokered request, `GetInterface(name)`, and everything else rides a pipe of its own.
 *
 * THE MAPPING, CONCEPT BY CONCEPT, and every row is a thing this platform actually has:
 *
 *   Mojo message pipe          → `MessagePort`. Point-to-point, ordered, and TRANSFERABLE, which is the whole
 *                                property: a pipe endpoint can be sent inside a message, which is how a new
 *                                interface is handed over. `MessageChannel` is `mojo::CreateMessagePipe`.
 *   Mojo node / invitation     → the `Worker` handle. Creating the child gives the parent one primordial pipe
 *                                and nothing else, exactly as `mojo::OutgoingInvitation` does; the child's
 *                                first message on it is its acceptance.
 *   `mojo::Remote<T>`          → `Remote` below: the caller end, with one JS method per mojom method.
 *   `mojo::Receiver<T>`        → `Receiver` below: the implementation end.
 *   `mojo::PendingRemote<T>` /
 *   `mojo::PendingReceiver<T>` → an UNBOUND `MessagePort` sitting in a message's transfer list. A method whose
 *                                parameter type is `handle<message_pipe>` is passing one.
 *   `BrowserInterfaceBroker`   → `Connection.bindInterface(name)`: a NAME goes out on the primordial pipe with
 *                                one end of a fresh pipe, and the peer binds its implementation to that end.
 *                                Chromium's binder map is `mojo.exposeInterface` — process-wide, registered at
 *                                startup, consulted for every peer, which is what `content::BinderMap` is.
 *   mojom IDL                  → extension/mojom.js. Interfaces are DECLARED in one file both processes load,
 *                                because an interface only exists if both ends agree on it.
 *   message header             → `{w, i, o, r, f}`: wire version, interface, ORDINAL, request id, flags. The
 *                                flags are Mojo's own two — `kMessageExpectsResponse` and `kMessageIsResponse`
 *                                — with Mojo's values. Ordinals are what the wire carries (mojom's `@0`), so
 *                                reordering a method list is not a wire change and renaming one is not either.
 *   `ReportBadMessage`         → a validation failure is not an error return. It is the peer being broken, so
 *                                it kills the CONNECTION: in the child that is posted as an abort and every
 *                                outstanding call rejects, which is what a real bad-message kill looks like
 *                                from the browser's side.
 *
 * THE TWO PLACES THIS DELIBERATELY DIVERGES FROM MOJO, stated rather than left to be noticed:
 *
 *   (1) THE INTERFACE NAME IS ON THE WIRE. Mojo does not put it there — the pipe implies the interface. It is
 *       carried here because the failure it catches is otherwise silent-then-wrong: a broker that bound the
 *       wrong implementation to a pipe would answer the FIRST few ordinals plausibly (they exist on both
 *       interfaces) and only diverge later, and the crash would then name an ordinal instead of the two
 *       interfaces that got crossed. It is asserted and never dispatched on, exactly like the `v:1` both
 *       superseded transports carried.
 *   (2) EVERY RECORD A CHILD POSTS CARRIES THE CHILD'S STDIO. Mojo has no such field, because a real child
 *       process has a real stderr the browser reads off an fd. Here the only path out of a Worker is
 *       postMessage, and the `@WHY` a CHECK prints immediately before abort() is the one line that says what
 *       broke — so it rides the TRANSPORT (attached by `_envelope`, absorbed by `_absorb`), never an
 *       interface's parameters. A process's output is a fact about the PROCESS and not about any interface it
 *       happens to serve, and which pipe answers next is not knowable, so it cannot live on one of them.
 *
 * ORDERING, AND WHAT IS NOT BUILT. Mojo guarantees ordering WITHIN a pipe and nothing across pipes; the tool
 * for cross-interface ordering is an ASSOCIATED interface, which multiplexes several interfaces onto one pipe
 * so their messages keep one order. None is built here, and the reason is specific rather than "not needed
 * yet": the one pair of messages whose relative order is load-bearing — `RendererTerminated` for an agent
 * cluster and the next `CreateRendererForCluster` for that same cluster, which would otherwise be refused as a
 * duplicate — are two METHODS OF ONE INTERFACE, so one pipe already orders them. Nothing else in mojom.js has a
 * cross-interface ordering requirement. The day one does, it is an associated interface and not a sleep.
 * A HANDLER THAT SUSPENDS DOES NOT BLOCK THE PIPE, which is also Mojo's behaviour: `CreateRendererForCluster`
 * awaits a fork order to the other process, and a `GetRegistry` behind it is answered while that await is
 * outstanding. That is why the registry entry is taken BEFORE the first await, the same discipline 94c5998e
 * fixed the engine pool with.
 */
(function (g) {
  "use strict";

  var WIRE = 1;
  /* Mojo's own flag values (mojo/public/cpp/bindings/lib/message.h). */
  var F_EXPECTS_RESPONSE = 1;
  var F_IS_RESPONSE = 2;

  /* ── THE TYPE SYSTEM. mojom's spellings, and only the ones this transport actually carries. Structured
     clone reproduces each of them natively, so nothing is ever encoded in transit — a number stays a number
     (so a peer's `0` is still distinguishable from the string "0") and a byte sequence stays bytes. */
  function typeOk(t, v) {
    if (t === "string")  return typeof v === "string";
    if (t === "string?") return v === null || typeof v === "string";
    if (t === "bool")    return typeof v === "boolean";
    if (t === "int32")   return typeof v === "number" && (v | 0) === v;
    if (t === "array<uint8>") return v instanceof Uint8Array;
    if (t === "handle<message_pipe>")  return v instanceof MessagePort;
    if (t === "handle<message_pipe>?") return v === null || v instanceof MessagePort;
    DFAIL("a mojom declaration named a type this bindings layer does not carry: `" + t + "` — the type list is " +
          "the whole contract of what may cross a process boundary, and a new one is a serialization decision " +
          "(what structured clone reproduces, and whether it is a handle that must be transferred) rather than " +
          "a name to add");
    return false;   /* release path under the assert: an uncarryable type is not silently posted */
  }
  function isHandle(t) { return t === "handle<message_pipe>" || t === "handle<message_pipe>?"; }

  /* A DECLARATION LIST — a method's parameters, or its reply's. Every entry carries a `why`, and that is not
     documentation: it is the sentence the assert prints when the value is wrong. The two superseded transports
     each held a `checkHeaderFacts` whose real content was those sentences (an absent header is §5.1's undefined
     supplied type and says so with null, never with ""), written twice and drifting; here the sentence lives
     with the DECLARATION, so both ends print it and neither can hold a different one. */
  function checkDecls(where, decls) {
    DCHECK(Array.isArray(decls),
           where + " has no parameter list — a mojom method declares its parameters even when there are none, " +
           "with an empty list, because an absent list and a list of nothing are different claims");
    for (var i = 0; i < decls.length; i++) {
      var d = decls[i];
      DCHECK(!!d && typeof d.name === "string" && d.name !== "" && typeof d.type === "string",
             where + " parameter " + i + " is not a `{name, type, why}` declaration");
      DCHECK(typeof d.why === "string" && d.why !== "",
             where + " parameter `" + d.name + "` carries no `why` — that string is what the validator prints " +
             "when the value is wrong, so a parameter without one is a crash that names a type and not a rule");
    }
  }

  function checkValues(where, decls, vals) {
    DCHECK(Array.isArray(vals) && vals.length === decls.length,
           where + " carried " + (Array.isArray(vals) ? vals.length : "no") + " value(s) where its mojom " +
           "declares " + decls.length + " — a short list reads every later parameter one position early, which " +
           "is a wrong call rather than a missing one");
    for (var i = 0; i < decls.length; i++)
      DCHECK(typeOk(decls[i].type, vals[i]),
             where + " parameter `" + decls[i].name + "` is not the `" + decls[i].type + "` its mojom " +
             "declares — " + decls[i].why);
  }

  /* HANDLES ARE COLLECTED INTO THE TRANSFER LIST, which is what makes `handle<message_pipe>` a real pipe pass
     rather than a clone: a transferred port is DETACHED in the sender and re-materialized in the receiver, so
     the endpoint genuinely moves and the sender provably no longer holds it. */
  function collectHandles(decls, vals, into) {
    for (var i = 0; i < decls.length; i++)
      if (isHandle(decls[i].type) && vals[i] !== null) into.push(vals[i]);
  }

  /* ── THE IDL. `defineInterface` is what a generated `*.mojom.js` module does in Chromium: it is the single
     description both processes validate against, and it exists per realm because both realms load mojom.js. */
  var _defs = new Map();
  function lowerFirst(s) { return s.charAt(0).toLowerCase() + s.slice(1); }

  function defineInterface(def) {
    DCHECK(!!def && typeof def.name === "string" && /^[a-z]+\.mojom\.[A-Z][A-Za-z0-9]*$/.test(def.name),
           "a mojom interface must be named `<module>.mojom.<Interface>` — the module half is what says which " +
           "layer owns it (a network-service interface and a content-layer interface are not interchangeable) " +
           "and it is the name a bind request carries");
    DCHECK(typeof def.version === "number" && (def.version | 0) === def.version && def.version >= 0,
           def.name + " declares no version — a bind states the version it expects and the peer asserts it, so " +
           "an interface without one cannot say whether the two processes were built together");
    DCHECK(!_defs.has(def.name),
           def.name + " is defined twice in this realm — an interface is ONE description that both ends " +
           "validate against, and a second definition means two of them with nothing to say which is on the wire");
    DCHECK(Array.isArray(def.methods) && def.methods.length > 0,
           def.name + " declares no methods — an interface IS its method list");
    var byOrd = new Map(), byJs = new Map();
    for (var i = 0; i < def.methods.length; i++) {
      var m = def.methods[i];
      DCHECK(!!m && typeof m.ordinal === "number" && (m.ordinal | 0) === m.ordinal && m.ordinal >= 0,
             def.name + " has a method with no ordinal — the ORDINAL is what the wire carries (mojom's `@0`), " +
             "so a method identified by its position would change identity the day the list is reordered");
      DCHECK(!byOrd.has(m.ordinal),
             def.name + " gives ordinal " + m.ordinal + " to two methods — an ordinal is the method's identity " +
             "on the wire and a repeat routes one call to whichever was registered last");
      DCHECK(typeof m.name === "string" && /^[A-Z][A-Za-z0-9]*$/.test(m.name),
             def.name + " has a method whose name is not mojom's CamelCase");
      checkDecls(def.name + "." + m.name, m.params);
      DCHECK(m.reply === null || Array.isArray(m.reply),
             def.name + "." + m.name + " must declare a reply list or `null` — null is the positive statement " +
             "that this method is fire-and-forget, which is a different contract from a reply carrying nothing");
      if (m.reply !== null) checkDecls(def.name + "." + m.name + "'s reply", m.reply);
      m.iface = def.name;
      m.js = lowerFirst(m.name);   /* Chromium's JS bindings lower the first letter; `CheckCorb` → `checkCorb` */
      byOrd.set(m.ordinal, m);
      byJs.set(m.js, m);
    }
    def.byOrd = byOrd;
    def.byJs = byJs;
    _defs.set(def.name, def);
    return def;
  }

  function interfaceOf(name) {
    var d = _defs.get(name);
    DCHECK(d !== undefined,
           "no mojom interface named `" + name + "` is defined in this realm — extension/mojom.js is the one " +
           "place they are declared and every realm that speaks this transport loads it, so a missing one is a " +
           "realm that did not load the IDL rather than a name to be looked up somewhere else");
    return d;
  }

  /* ── THE PROCESS-WIDE BINDER MAP, which is `content::BinderMap`: what THIS process implements, registered by
     the component that implements it, consulted for every peer that asks. It is not per-connection because an
     interface is a capability of the PROCESS — a second peer asking for the same name must reach the same
     implementation, and a per-connection registry would let two of them diverge. */
  var _binders = new Map();
  function exposeInterface(name, impl) {
    var def = interfaceOf(name);
    DCHECK(!_binders.has(name),
           name + " is exposed twice in this realm — the binder map answers a bind request by NAME, so a second " +
           "registration silently decides which implementation every future peer reaches");
    DCHECK(!!impl && typeof impl === "object", name + " was exposed with no implementation object");
    def.byJs.forEach(function (m) {
      DCHECK(typeof impl[m.js] === "function",
             "the implementation exposed for " + name + " has no `" + m.js + "` — an interface is the WHOLE " +
             "list of its methods, and a peer calling a missing one would be answered by a TypeError inside a " +
             "message handler, which reaches nobody: the caller stays parked on a reply that is never coming");
    });
    _binders.set(name, impl);
  }

  /* ── LIVE ENDPOINTS OF THIS REALM. Each `Remote` and each `Receiver` holds ONE END of one pipe, so this is
     the realm's open-endpoint count and it is what a probe reads to say how many pipes this process has. It is
     kept here rather than per connection because a pipe outlives the bind that created it and belongs to the
     process, exactly as a handle count does. */
  var _endpoints = new Set();
  function stats() {
    var remotes = [], receivers = [];
    _endpoints.forEach(function (h) {
      (h.kind === "remote" ? remotes : receivers).push(h.def.name + "@" + h.def.version);
    });
    return { remotes: remotes.sort(), receivers: receivers.sort(), endpoints: _endpoints.size };
  }

  /* ── THE CALLER END. One JS method per mojom method, named by Chromium's JS-binding rule, taking the
     parameters positionally and answering an OBJECT keyed by the reply's declared names — which is what
     Chromium's generated bindings do, and it is why a reply field can be added without every call site
     re-counting positions. A fire-and-forget method (`reply: null`) returns nothing at all: there is no
     promise to await, because there is no answer, and handing one back would invite a caller to wait on it. */
  function Remote(conn, def, port) {
    var self_ = this;
    this.kind = "remote";
    this.conn = conn;
    this.def = def;
    this.port = port;
    this._next = 1;
    this._await = new Map();
    port.onmessage = function (e) { self_._onmessage(e.data); };
    _endpoints.add(this);
    conn._endpoints.add(this);
    def.byJs.forEach(function (m) {
      self_[m.js] = function () { return self_._send(m, Array.prototype.slice.call(arguments)); };
    });
  }

  Remote.prototype._send = function (m, args) {
    DCHECK(!this.conn.dead,
           "a call was made on " + m.iface + "." + m.name + " after " + this.conn.name + " died (" +
           this.conn.deadReason + ") — what failed is that process, so this call would be a second crash " +
           "reported as a first, and its caller would park on a reply nothing is left to produce");
    checkValues("a call to " + m.iface + "." + m.name, m.params, args);
    var xfer = [];
    collectHandles(m.params, args, xfer);
    var id = 0, p;
    if (m.reply !== null) {
      var self_ = this;
      id = this._next++;
      p = new Promise(function (res, rej) { self_._await.set(id, { resolve: res, reject: rej }); });
    }
    this.port.postMessage(this.conn._envelope({ w: WIRE, i: m.iface, o: m.ordinal, r: id,
                                                f: m.reply !== null ? F_EXPECTS_RESPONSE : 0, a: args }), xfer);
    return p;
  };

  Remote.prototype._onmessage = function (env) {
    try {
      DCHECK(!!env && env.w === WIRE && typeof env.i === "string" && typeof env.o === "number" &&
             typeof env.r === "number" && typeof env.f === "number" && Array.isArray(env.a),
             "a record on a " + this.def.name + " pipe is not this transport's — a message is a wire version, " +
             "the interface it belongs to, a method ordinal, a request id, flags and the parameter list");
      this.conn._absorb(env);
      DCHECK(env.i === this.def.name,
             "a " + this.def.name + " pipe carried a message for " + env.i + " — the interface name is on the " +
             "wire precisely so a broker that bound the wrong implementation to this pipe names both " +
             "interfaces here rather than diverging silently at whichever ordinal they stop sharing");
      DCHECK((env.f & F_IS_RESPONSE) !== 0,
             "a Remote received a REQUEST on " + this.def.name + " — a pipe has one Remote end and one " +
             "Receiver end, so a request arriving here is two Remotes bound to one pipe and the real receiver " +
             "is bound to nothing");
      var m = this.def.byOrd.get(env.o);
      DCHECK(m !== undefined,
             this.def.name + " has no method at ordinal " + env.o + " — both ends of this pipe are built from " +
             "one extension, so an unknown ordinal is a peer running a different generation of mojom.js");
      DCHECK(m.reply !== null,
             m.iface + "." + m.name + " is declared fire-and-forget and answered anyway");
      checkValues("the reply to " + m.iface + "." + m.name, m.reply, env.a);
      var w = this._await.get(env.r);
      DCHECK(w !== undefined,
             "a reply arrived for " + m.iface + "." + m.name + " request id " + env.r + ", which this endpoint " +
             "never made — the request id is the whole routing table for an answer, so an unknown one means " +
             "the call that IS outstanding will never be resolved");
      this._await.delete(env.r);
      var rec = {};
      for (var i = 0; i < m.reply.length; i++) rec[m.reply[i].name] = env.a[i];
      w.resolve(rec);
    } catch (e) { this.conn._crash(e); }
  };

  /* ── THE IMPLEMENTATION END. An impl method returns the reply as an object keyed by the declared names, or a
     promise of one; a fire-and-forget impl returns nothing. Both directions are validated, at BOTH ends: Mojo
     validates on receive, and this validates on send as well, because CLAUDE.md puts the assert at the value's
     ORIGIN — a producer that stopped writing a reply field is caught in its own process, on the line that
     returned it, rather than one hop later in a process that can only say it was handed something wrong. */
  function Receiver(conn, def, impl, port) {
    var self_ = this;
    this.kind = "receiver";
    this.conn = conn;
    this.def = def;
    this.impl = impl;
    this.port = port;
    port.onmessage = function (e) { self_._onmessage(e.data); };
    _endpoints.add(this);
    conn._endpoints.add(this);
  }

  Receiver.prototype._onmessage = function (env) {
    var self_ = this;
    try {
      DCHECK(!!env && env.w === WIRE && typeof env.i === "string" && typeof env.o === "number" &&
             typeof env.r === "number" && typeof env.f === "number" && Array.isArray(env.a),
             "a record on a " + this.def.name + " pipe is not this transport's — a message is a wire version, " +
             "the interface it belongs to, a method ordinal, a request id, flags and the parameter list");
      this.conn._absorb(env);
      DCHECK(env.i === this.def.name,
             "a " + this.def.name + " pipe carried a message for " + env.i + " — see the Remote's copy of this " +
             "assert: the name is on the wire so a crossed bind names both interfaces");
      DCHECK((env.f & F_IS_RESPONSE) === 0,
             "a Receiver received a RESPONSE on " + this.def.name + " — nothing on this end ever called out, " +
             "so this pipe has two Receivers and the Remote is bound to nothing");
      var m = this.def.byOrd.get(env.o);
      DCHECK(m !== undefined,
             this.def.name + " has no method at ordinal " + env.o + " — both ends are built from one " +
             "extension, so an unknown ordinal is a peer running a different generation of mojom.js");
      DCHECK(((env.f & F_EXPECTS_RESPONSE) !== 0) === (m.reply !== null),
             "the caller of " + m.iface + "." + m.name + " disagrees with the mojom about whether it answers — " +
             "one side would park on a reply the other will never send, or send one nobody is waiting for");
      checkValues("a call to " + m.iface + "." + m.name, m.params, env.a);
      var ret = this.impl[m.js].apply(this.impl, env.a);
      if (m.reply === null) {
        DCHECK(ret === undefined,
               m.iface + "." + m.name + " is declared fire-and-forget and its implementation returned a value " +
               "— nothing carries it anywhere, so a result computed there is a result discarded");
        return;
      }
      DCHECK(env.r !== 0,
             "a call to " + m.iface + "." + m.name + " expects a reply and carried request id 0, which is the " +
             "id a fire-and-forget message uses — there is no id to answer on");
      Promise.resolve(ret).then(function (rec) {
        DCHECK(!!rec && typeof rec === "object",
               "the implementation of " + m.iface + "." + m.name + " answered with no reply record — a method " +
               "that declares a reply returns an object keyed by the reply's declared names");
        var vals = [];
        for (var i = 0; i < m.reply.length; i++) vals.push(rec[m.reply[i].name]);
        checkValues("the reply " + m.iface + "." + m.name + "'s implementation returned", m.reply, vals);
        var xfer = [];
        collectHandles(m.reply, vals, xfer);
        self_.port.postMessage(self_.conn._envelope({ w: WIRE, i: m.iface, o: m.ordinal, r: env.r,
                                                      f: F_IS_RESPONSE, a: vals }), xfer);
      }, function (e) { self_.conn._crash(e); });
    } catch (e) { this.conn._crash(e); }
  };

  /* ── THE CONNECTION: one peer process, reached over the primordial pipe the platform's process creation
     already gave us. It carries exactly three kinds of record and no capability at all — an invitation
     acceptance, a bind request, and the child's abort — because every capability rides a pipe of its own.
     THE TWO ROLES ARE NOT SYMMETRIC AND ARE ASSERTED APART. A PARENT holds a child's output (nothing else can
     read it) and decorates the errors it raises with the tail of it; a CHILD drains its own output into every
     record it posts and reports its own death, because there is nothing above a parent to report to. Each role
     asserts the halves it must have rather than defaulting a missing hook to a no-op. */
  function Connection(transport, opts) {
    var self_ = this;
    DCHECK(!!transport && typeof transport.post === "function" && typeof transport.listen === "function",
           "a mojo Connection needs a primordial-pipe transport — `post(record, transfer)` and `listen(cb)` " +
           "over whatever the platform's process creation handed back (a Worker handle, a MessagePort)");
    DCHECK(!!opts && (opts.role === "parent" || opts.role === "child"),
           "a mojo Connection must state which side of the process boundary it is — the roles are not " +
           "symmetric (only a parent holds the child's output, only a child can report its own death)");
    DCHECK(!!opts && typeof opts.name === "string" && opts.name !== "",
           "a mojo Connection must name its PEER — every assert and every rejection this connection raises " +
           "says which process it is about, and `undefined died` names nothing");
    this.role = opts.role;
    this.name = opts.name;
    this.dead = false;
    this.deadReason = null;
    this._t = transport;
    this._endpoints = new Set();
    if (this.role === "parent") {
      DCHECK(typeof opts.onStdio === "function",
             "a parent connection to " + opts.name + " must say where the child's output goes — the `@WHY` a " +
             "CHECK prints immediately before abort() is the only thing that says what broke, and a child that " +
             "printed it into a dropped field is a crash that took its own cause with it");
      DCHECK(typeof opts.decorate === "function",
             "a parent connection to " + opts.name + " must say how an error it raises is decorated — the " +
             "rejection a caller sees is the only place the child's last lines can be attached");
      this._onStdio = opts.onStdio;
      this._decorate = opts.decorate;
      this._drainStdio = null;
      this.ready = new Promise(function (res, rej) { self_._readyRes = res; self_._readyRej = rej; });
    } else {
      DCHECK(typeof opts.drainStdio === "function",
             "a child connection must say how to drain its own output — it rides every record this process " +
             "posts, because which pipe answers next is not knowable and lines left here are lines nobody reads");
      this._drainStdio = opts.drainStdio;
      this._onStdio = null;
      this._decorate = null;
      this.ready = null;
    }
    transport.listen(function (m) { self_._onprimordial(m); });
  }

  Connection.prototype._envelope = function (e) {
    if (this.role === "child") e.out = this._drainStdio();
    return e;
  };

  Connection.prototype._absorb = function (env) {
    if (this.role === "child") {
      DCHECK(env.out === undefined,
             "the parent sent stdio to a child — output flows one way across a process boundary, and a field " +
             "arriving in this direction is a parent that has started narrating into its own child");
      return;
    }
    DCHECK(Array.isArray(env.out),
           "a record from " + this.name + " carried no output field — every record a child posts drains the " +
           "process's output with it, so a missing one is a child that stopped reporting and a crash whose " +
           "cause stays inside the process that died");
    this._onStdio(env.out);
  };

  Connection.prototype._post = function (m, xfer) {
    /* THE TRANSFER LIST IS STATED AT EVERY CALL, including where it is empty. A `xfer || []` default here
       would be one line, and it would be the line on which a caller that forgot the handle it MEANT to send
       posts a record whose port is silently cloned-and-refused rather than moved. */
    DCHECK(Array.isArray(xfer),
           "a record on the primordial pipe to " + this.name + " was posted with no transfer list — a message " +
           "either carries handles or states that it carries none, and the two are different messages");
    this._t.post(this._envelope(m), xfer);
  };

  /* THE CHILD'S ACCEPTANCE, which is `mojo::IncomingInvitation::Accept` and is also the thing a bare `onload`
     could never say: a worker script that LOADED is not a module that INSTANTIATED, and the parent must not
     send a call into a program that does not exist. A failure travels IN the acceptance rather than being
     swallowed, because there is no timeout on the far side — a wall clock there would report a loaded machine
     as a broken transport (CLAUDE.md §Testing). */
  Connection.prototype.acceptInvitation = function (ok, err) {
    DCHECK(this.role === "child", "only a child accepts an invitation");
    DCHECK(typeof ok === "boolean" && (err === null || typeof err === "string"),
           "an invitation acceptance states whether this process started and, when it did not, why");
    this._post({ w: WIRE, k: "accept-invitation", ok: ok, err: err }, []);
  };

  /* THE BROKER'S ONE REQUEST. A name goes out with one end of a fresh pipe; the peer's binder map answers it.
     This is the whole of what the primordial pipe carries besides life and death, and it is why a new
     capability is a new INTERFACE rather than a new `op` hand-written into two switch statements. */
  Connection.prototype.bindInterface = function (name) {
    DCHECK(!this.dead,
           "an interface was requested from " + this.name + " after it died (" + this.deadReason + ")");
    var def = interfaceOf(name);
    var ch = new MessageChannel();
    this._post({ w: WIRE, k: "bind", iface: name, version: def.version, port: ch.port2 }, [ch.port2]);
    return new Remote(this, def, ch.port1);
  };

  Connection.prototype._onprimordial = function (m) {
    try {
      DCHECK(!!m && m.w === WIRE && typeof m.k === "string",
             "a record on the primordial pipe to " + this.name + " is not this transport's — it carries a wire " +
             "version and a kind");
      this._absorb(m);
      if (m.k === "accept-invitation") {
        DCHECK(this.role === "parent", "a child received an invitation acceptance");
        DCHECK(typeof m.ok === "boolean", "an invitation acceptance from " + this.name + " states no outcome");
        if (m.ok) { this._readyRes(this); return; }
        this._die(this.name + " did not start: " + m.err);
        return;
      }
      if (m.k === "bind") { this._onbind(m); return; }
      if (m.k === "abort") {
        DCHECK(this.role === "parent",
               "a parent reported its own death to a child — there is nothing above a parent to report to, and " +
               "a child that believed it would go on making calls into a process that is gone");
        DCHECK(typeof m.reason === "string", "an abort from " + this.name + " carries the reason it died");
        this._die(this.name + " died: " + m.reason);
        return;
      }
      DFAIL("the primordial pipe to " + this.name + " carried a record kind it does not serve: `" + m.k + "` — " +
            "a capability is an INTERFACE brokered onto its own pipe, never a new kind here");
    } catch (e) { this._crash(e); }
  };

  Connection.prototype._onbind = function (m) {
    DCHECK(typeof m.iface === "string" && typeof m.version === "number" && m.port instanceof MessagePort,
           "a bind request from " + this.name + " is not one — it names an interface, states the version it " +
           "expects, and carries the pipe end this process is to bind its implementation to");
    var def = interfaceOf(m.iface);
    DCHECK(m.version === def.version,
           this.name + " asked to bind " + m.iface + " at version " + m.version + " while this process defines " +
           "version " + def.version + " — both ends ship out of ONE extension, so a skew is a build that " +
           "packaged two generations of mojom.js rather than a compatibility case to negotiate");
    var impl = _binders.get(m.iface);
    DCHECK(impl !== undefined,
           this.name + " asked this process for `" + m.iface + "`, which nothing here implements — an interface " +
           "reaches a peer only if a component registered it with mojo.exposeInterface, and a bind nobody " +
           "answers leaves the caller holding a pipe with no receiver on the far end and no error either");
    new Receiver(this, def, impl, m.port);
  };

  /* DEATH. One place, whichever way it arrives: the child said so, the child never started, or an invariant in
     this process broke. Every outstanding call rejects — Mojo's peer-closed — and the connection stays dead,
     which is the fail-closed direction a security gate must have: a browser process that is not there must
     refuse every later fetch with the same reason rather than intermittently re-provisioning into a state
     where nothing judges the body. */
  Connection.prototype._die = function (reason) {
    if (this.dead) return;
    this.dead = true;
    this.deadReason = reason;
    var self_ = this;
    this._endpoints.forEach(function (h) {
      if (h.kind !== "remote") return;
      h._await.forEach(function (w) {
        var e = new Error(reason);
        if (self_.role === "parent") self_._decorate(e);
        w.reject(e);
      });
      h._await.clear();
    });
    if (this.role === "parent") {
      var e2 = new Error(reason);
      this._decorate(e2);
      this._readyRej(e2);   /* a no-op once ready has resolved */
    }
  };

  /* A BROKEN MESSAGE IS THE PEER BEING BROKEN, so it kills the connection rather than returning an error — Mojo
     calls this ReportBadMessage and it kills the child. A CHILD posts the abort first, so the parent learns of
     the death it is about to have; a PARENT has nobody to tell. Both then RETHROW: the assert is the mechanism
     and a transport that swallowed it would be the one place the ONE assertion mechanism is locally disabled. */
  Connection.prototype._crash = function (e) {
    var reason = String((e && e.stack) || e);
    if (this.role === "child" && !this.dead) this._post({ w: WIRE, k: "abort", reason: reason }, []);
    this._die(this.name + " connection aborted: " + reason);
    throw e;
  };

  g.mojo = {
    defineInterface: defineInterface,
    exposeInterface: exposeInterface,
    Connection: Connection,
    stats: stats,
  };
})(self);
